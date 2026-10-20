// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the MGCP text layer (src/proto/mgcp/msg.c).
 *
 * The inputs follow RFC 3435: the command and response lines of 3.2.1 and
 * 3.3, the parameter grammar of Appendix A, the piggyback rule of 3.5.5 and
 * the example encodings of Appendix F. Each parse runs on an allocation of
 * the exact input size, so a read past the message hits an ASan redzone.
 */
#include <stdlib.h>

#include "proto/mgcp/msg.h"
#include "test_util.h"

static struct mgcp_msg m;
static char *pbuf;

static enum mgcp_parse_status parse_len(const char *text, size_t len)
{
	free(pbuf);
	pbuf = malloc(len ? len : 1);
	if (!pbuf)
		return MGCP_PARSE_ERR_EMPTY;
	memcpy(pbuf, text, len);

	return mgcp_msg_parse(&m, pbuf, len);
}

static enum mgcp_parse_status parse(const char *text)
{
	return parse_len(text, strlen(text));
}

static struct sip_str val(enum mgcp_param code)
{
	struct sip_str v = { "", 0 };

	if (!mgcp_msg_value(&m, code, &v))
		return sip_str_init("<absent>", 8);

	return v;
}

static const struct sip_str pkg_l = { "L", 1 };
static const struct sip_str pkg_d = { "D", 1 };

/* RFC 3435 F.3, the third example: a provisional then a final with K: */
static const char crcx_long[] =
	"CRCX 1206 aaln/1@rgw-2569.whatever.net MGCP 1.0\r\n"
	"K: 1205\r\n"
	"C: A3C47F21456789F0\r\n"
	"L: p:10, a:PCMU\r\n"
	"M: inactive\r\n"
	"X: 0123456789AD\r\n"
	"R: L/hd(E(R(D/[0-9#T](D),L/hu(N)),S(L/dl),D([0-9].[#T])))\r\n"
	"S: L/ci(10/14/17/26, \"555 1212\", \"Doe, John\"), L/rg\r\n"
	"\r\n"
	"v=0\r\n"
	"o=- 25678 753849 IN IP4 128.96.41.1\r\n"
	"s=-\r\n"
	"c=IN IP4 128.96.41.1\r\n"
	"t=0 0\r\n"
	"m=audio 3456 RTP/AVP 0\r\n";

/* ------------------------------------------------------------ first line */

static void command_line_tests(void)
{
	static const struct {
		const char *text;
		enum mgcp_verb verb;
	} verbs[] = {
		{ "EPCF 2001 aaln/1@rgw.example.net MGCP 1.0\r\nB: e:mu\r\n",
		  MGCP_VERB_EPCF },
		{ "CRCX 1204 aaln/1@rgw-2567.whatever.net MGCP 1.0\r\n"
		  "C: A3C47F21456789F0\r\nL: p:10, a:PCMU\r\nM: recvonly\r\n",
		  MGCP_VERB_CRCX },
		{ "MDCX 1209 aaln/1@rgw-2567.whatever.net MGCP 1.0\r\n"
		  "C: A3C47F21456789F0\r\nI: FDE234C8\r\nM: sendrecv\r\n",
		  MGCP_VERB_MDCX },
		{ "DLCX 1210 aaln/1@rgw-2567.whatever.net MGCP 1.0\r\n",
		  MGCP_VERB_DLCX },
		{ "RQNT 1201 aaln/1@rgw-2567.whatever.net MGCP 1.0\r\n"
		  "X: 0123456789AC\r\nR: l/hd(N)\r\nS: l/rg\r\n",
		  MGCP_VERB_RQNT },
		{ "NTFY 2002 aaln/1@rgw-2567.whatever.net MGCP 1.0\r\n"
		  "X: 0123456789AC\r\nO: L/hd\r\n", MGCP_VERB_NTFY },
		{ "AUEP 1200 *@rgw-2567.whatever.net MGCP 1.0\r\n",
		  MGCP_VERB_AUEP },
		{ "AUCX 2003 aaln/1@rgw-2567.whatever.net MGCP 1.0\r\n"
		  "I: 32F345E2\r\nF: C,N,L,M,LC,P\r\n", MGCP_VERB_AUCX },
		{ "RSIP 1200 aaln/1@rgw-2567.whatever.net MGCP 1.0\r\n"
		  "RM: graceful\r\nRD: 300\r\n", MGCP_VERB_RSIP },
	};
	unsigned int i;

	CASE("every verb of RFC 3435 2.3 parses to its code");
	for (i = 0; i < sizeof(verbs) / sizeof(verbs[0]); i++) {
		CHECK(parse(verbs[i].text) == MGCP_PARSE_OK);
		CHECK(m.is_command);
		CHECK(m.verb == verbs[i].verb);
		CHECK(!strcmp(mgcp_verb_name(m.verb), mgcp_verb_name(
			mgcp_verb_parse(m.verb_name))));
		CHECK(!(m.flags & MGCP_MSG_F_BAD_VERSION));
		CHECK(!(m.flags & MGCP_MSG_F_VERB_BAD));
	}

	CASE("the command line fields");
	CHECK(parse(verbs[1].text) == MGCP_PARSE_OK);
	CHECK_UINT(m.txid, 1204);
	CHECK_STR(m.verb_name, "CRCX");
	CHECK_STR(m.endpoint, "aaln/1@rgw-2567.whatever.net");
	CHECK_STR(m.version, "MGCP 1.0");
	CHECK_UINT(m.profile.len, 0);
	CHECK_UINT(m.param_count, 3);
	CHECK_UINT(m.sdp.len, 0);

	CASE("verbs compare case insensitively (3.2.1.1)");
	CHECK(parse("crcx 1 aaln/1@gw MGCP 1.0\r\n") == MGCP_PARSE_OK);
	CHECK(m.verb == MGCP_VERB_CRCX);
	CHECK_STR(m.verb_name, "crcx");
	CHECK(parse("RsIp 2 *@gw mgcp 1.0\r\nRM: restart\r\n") ==
	      MGCP_PARSE_OK);
	CHECK(m.verb == MGCP_VERB_RSIP);
	CHECK(!(m.flags & MGCP_MSG_F_BAD_VERSION));

	CASE("an unknown verb keeps its name");
	CHECK(parse("XPER 7 aaln/1@gw MGCP 1.0\r\n") == MGCP_PARSE_OK);
	CHECK(m.verb == MGCP_VERB_UNKNOWN);
	CHECK_STR(m.verb_name, "XPER");
	CHECK(!(m.flags & MGCP_MSG_F_VERB_BAD));
	CHECK(parse("NOTIFY 7 aaln/1@gw MGCP 1.0\r\n") == MGCP_PARSE_OK);
	CHECK(m.verb == MGCP_VERB_UNKNOWN);
	CHECK(m.flags & MGCP_MSG_F_VERB_BAD);

	CASE("extra white space between the words (3.2.1)");
	CHECK(parse("NTFY \t 3  aaln/1@gw\t MGCP   1.0  \r\n") ==
	      MGCP_PARSE_OK);
	CHECK_UINT(m.txid, 3);
	CHECK_STR(m.endpoint, "aaln/1@gw");
	CHECK_STR(m.version, "MGCP   1.0");
	CHECK(mgcp_msg_version_is(&m, MGCP_VERSION_RFC3435));
	CHECK(!mgcp_msg_version_is(&m, MGCP_VERSION_NCS));

	CASE("the NCS version token and its profile (NCS 1.5 8.2.1.4)");
	CHECK(parse("RQNT 1201 aaln/1@rgw-2567.whatever.net MGCP 1.0 NCS 1.0"
		    "\r\nX: 1\r\n") == MGCP_PARSE_OK);
	CHECK_STR(m.version, "MGCP 1.0 NCS 1.0");
	CHECK_STR(m.profile, "NCS 1.0");
	CHECK(mgcp_msg_version_is(&m, MGCP_VERSION_NCS));
	CHECK(mgcp_msg_version_is(&m, "mgcp 1.0  ncs 1.0"));
	CHECK(!mgcp_msg_version_is(&m, MGCP_VERSION_RFC3435));

	CASE("a foreign version parses and is flagged for 528 (3.2.1.4)");
	CHECK(parse("CRCX 5 aaln/1@gw MGCP 2\r\n") == MGCP_PARSE_OK);
	CHECK(m.flags & MGCP_MSG_F_BAD_VERSION);
	CHECK(parse("CRCX 5 aaln/1@gw SIP/2.0\r\n") == MGCP_PARSE_OK);
	CHECK(m.flags & MGCP_MSG_F_BAD_VERSION);
	CHECK(parse("CRCX 5 aaln/1@gw MGCP 1.x\r\n") == MGCP_PARSE_OK);
	CHECK(m.flags & MGCP_MSG_F_BAD_VERSION);

	CASE("an unreadable first line is refused");
	CHECK(parse("CRCX 5 aaln/1@gw\r\n") == MGCP_PARSE_ERR_FIRST_LINE);
	CHECK(parse("CRCX 5\r\n") == MGCP_PARSE_ERR_FIRST_LINE);
	CHECK(parse("200\r\n") == MGCP_PARSE_ERR_FIRST_LINE);
	CHECK(parse("   \r\n") == MGCP_PARSE_ERR_FIRST_LINE);

	CASE("empty and oversized buffers");
	CHECK(parse("") == MGCP_PARSE_ERR_EMPTY);
	CHECK(parse("\r\n\r\n") == MGCP_PARSE_ERR_EMPTY);
	CHECK(mgcp_msg_parse(&m, NULL, 4) == MGCP_PARSE_ERR_EMPTY);
	{
		char *big = malloc(MGCP_MSG_LEN_MAX + 1);

		CHECK(big != NULL);
		if (big) {
			memset(big, 'A', MGCP_MSG_LEN_MAX + 1);
			CHECK(mgcp_msg_parse(&m, big, MGCP_MSG_LEN_MAX + 1) ==
			      MGCP_PARSE_ERR_TOO_LARGE);
			free(big);
		}
	}

	CASE("empty lines before the first line are skipped and flagged");
	CHECK(parse("\r\n\nAUEP 9 aaln/1@gw MGCP 1.0\r\n") == MGCP_PARSE_OK);
	CHECK(m.flags & MGCP_MSG_F_LEAD_EOL);
	CHECK(m.verb == MGCP_VERB_AUEP);
}

static void txid_tests(void)
{
	uint32_t v;

	CASE("transaction id 0 is refused (3.2.1.2)");
	CHECK(parse("CRCX 0 aaln/1@gw MGCP 1.0\r\n") == MGCP_PARSE_ERR_TXID);
	CHECK(parse("200 0 OK\r\n") == MGCP_PARSE_ERR_TXID);

	CASE("transaction id 999999999 is the top of the range");
	CHECK(parse("CRCX 999999999 aaln/1@gw MGCP 1.0\r\n") == MGCP_PARSE_OK);
	CHECK_UINT(m.txid, MGCP_TXID_MAX);
	CHECK(parse("200 999999999\r\n") == MGCP_PARSE_OK);
	CHECK_UINT(m.txid, 999999999);

	CASE("transaction id 1000000000 is refused");
	CHECK(parse("CRCX 1000000000 aaln/1@gw MGCP 1.0\r\n") ==
	      MGCP_PARSE_ERR_TXID);
	CHECK(parse("250 1000000000 OK\r\n") == MGCP_PARSE_ERR_TXID);

	CASE("leading zeros count by value, ten digits are refused");
	CHECK(parse("NTFY 0001 aaln/1@gw MGCP 1.0\r\n") == MGCP_PARSE_OK);
	CHECK_UINT(m.txid, 1);
	CHECK(parse("NTFY 0000000001 aaln/1@gw MGCP 1.0\r\n") ==
	      MGCP_PARSE_ERR_TXID);
	CHECK(parse("NTFY 12a aaln/1@gw MGCP 1.0\r\n") == MGCP_PARSE_ERR_TXID);
	CHECK(parse("NTFY -1 aaln/1@gw MGCP 1.0\r\n") == MGCP_PARSE_ERR_TXID);

	CASE("the bare transaction id parser");
	CHECK(mgcp_txid_parse(sip_str_cstr("1"), &v) && v == 1);
	CHECK(mgcp_txid_parse(sip_str_cstr("999999999"), &v) && v == 999999999);
	CHECK(!mgcp_txid_parse(sip_str_cstr("0"), &v));
	CHECK(!mgcp_txid_parse(sip_str_cstr(""), &v));
	CHECK(!mgcp_txid_parse(sip_str_cstr("1000000000"), &v));
	CHECK(!mgcp_txid_parse(sip_str_cstr(" 5"), &v));
}

static void response_line_tests(void)
{
	CASE("a response with a comment (3.3)");
	CHECK(parse("200 1203 OK\r\n") == MGCP_PARSE_OK);
	CHECK(!m.is_command);
	CHECK_UINT(m.code, 200);
	CHECK_UINT(m.txid, 1203);
	CHECK_STR(m.comment, "OK");
	CHECK(mgcp_rc_class_of(m.code) == MGCP_RC_SUCCESS);

	CASE("a response with a multi word comment");
	CHECK(parse("401 1205 Phone off-hook\r\n") == MGCP_PARSE_OK);
	CHECK_UINT(m.code, 401);
	CHECK_STR(m.comment, "Phone off-hook");

	CASE("a response without a comment");
	CHECK(parse("200 1201\r\n") == MGCP_PARSE_OK);
	CHECK_UINT(m.code, 200);
	CHECK_UINT(m.comment.len, 0);

	CASE("the response acknowledgment 000 (3.3.1)");
	CHECK(parse("000 1204\r\n") == MGCP_PARSE_OK);
	CHECK_UINT(m.code, 0);
	CHECK_UINT(m.txid, 1204);
	CHECK(mgcp_rc_class_of(m.code) == MGCP_RC_ACK);
	CHECK(parse("000 1207 OK\r\n") == MGCP_PARSE_OK);
	CHECK_STR(m.comment, "OK");

	CASE("a package response code carries its package (3.3)");
	CHECK(parse("801 77 /XYZ package trouble\r\n") == MGCP_PARSE_OK);
	CHECK_UINT(m.code, 801);
	CHECK_STR(m.package, "XYZ");
	CHECK_STR(m.comment, "package trouble");
	CHECK(mgcp_rc_class_of(801) == MGCP_RC_PACKAGE);

	CASE("return code classes (RFC 3435 2.4, RFC 3661 2.1)");
	CHECK(mgcp_rc_class_of(99) == MGCP_RC_ACK);
	CHECK(mgcp_rc_class_of(100) == MGCP_RC_PROVISIONAL);
	CHECK(mgcp_rc_class_of(101) == MGCP_RC_PROVISIONAL);
	CHECK(mgcp_rc_class_of(250) == MGCP_RC_SUCCESS);
	CHECK(mgcp_rc_class_of(405) == MGCP_RC_TRANSIENT);
	CHECK(mgcp_rc_class_of(521) == MGCP_RC_PERMANENT);
	CHECK(mgcp_rc_class_of(300) == MGCP_RC_UNKNOWN);
	CHECK(mgcp_rc_class_of(999) == MGCP_RC_UNKNOWN);

	CASE("a response code that is not three digits is a command line");
	CHECK(parse("2000 1 OK\r\n") == MGCP_PARSE_ERR_FIRST_LINE);
	CHECK(parse("20 1 OK\r\n") == MGCP_PARSE_ERR_FIRST_LINE);
}

/* ------------------------------------------------------- lines and params */

static void line_tests(void)
{
	const struct mgcp_param_line *pl;
	unsigned int n;

	CASE("bare LF line ends are accepted and flagged");
	CHECK(parse("NTFY 5 aaln/1@gw MGCP 1.0\nX: 1\nO: L/hd\n") ==
	      MGCP_PARSE_OK);
	CHECK(m.flags & MGCP_MSG_F_BARE_LF);
	CHECK_UINT(m.param_count, 2);
	CHECK_STR(val(MGCP_PARAM_O), "L/hd");

	CASE("CRLF line ends set no flag");
	CHECK(parse("NTFY 5 aaln/1@gw MGCP 1.0\r\nX: 1\r\n") == MGCP_PARSE_OK);
	CHECK(!(m.flags & MGCP_MSG_F_BARE_LF));
	CHECK_STR(val(MGCP_PARAM_X), "1");

	CASE("mixed CRLF and bare LF with an SDP body");
	CHECK(parse("200 9 OK\nI: FDE234C8\r\n\nv=0\r\nm=audio 1 RTP/AVP 0\n")
	      == MGCP_PARSE_OK);
	CHECK_STR(val(MGCP_PARAM_I), "FDE234C8");
	CHECK_STR(m.sdp, "v=0\r\nm=audio 1 RTP/AVP 0\n");

	CASE("parameter codes compare case insensitively (3.2)");
	CHECK(parse("RQNT 1 aaln/1@gw MGCP 1.0\r\nx: 12\r\nrm: Restart\r\n"
		    "Es: L/hu\r\nz2: aaln/2@gw\r\n") == MGCP_PARSE_OK);
	CHECK_STR(val(MGCP_PARAM_X), "12");
	CHECK_STR(val(MGCP_PARAM_RM), "Restart");
	CHECK_STR(val(MGCP_PARAM_ES), "L/hu");
	CHECK_STR(val(MGCP_PARAM_Z2), "aaln/2@gw");
	CHECK(!strcmp(mgcp_param_name(MGCP_PARAM_ES), "ES"));

	CASE("white space around the colon and the value is trimmed");
	CHECK(parse("RQNT 1 aaln/1@gw MGCP 1.0\r\nX :   0123   \r\n"
		    "S:\r\nD:\t(xx)\r\n") == MGCP_PARSE_OK);
	CHECK_STR(val(MGCP_PARAM_X), "0123");
	CHECK_STR(val(MGCP_PARAM_S), "");
	CHECK_STR(val(MGCP_PARAM_D), "(xx)");

	CASE("unknown parameter codes keep their raw names (3.2.2)");
	CHECK(parse("RQNT 1 aaln/1@gw MGCP 1.0\r\nX-Flower: Daisy\r\n"
		    "X+Critical: 1\r\nL/foo: bar\r\nQQ: 9\r\n") ==
	      MGCP_PARSE_OK);
	CHECK_UINT(m.param_count, 4);
	CHECK(m.param[0].code == MGCP_PARAM_UNKNOWN);
	CHECK_STR(m.param[0].name, "X-Flower");
	CHECK_STR(m.param[0].value, "Daisy");
	pl = mgcp_msg_param_named(&m, "x+critical");
	CHECK(pl != NULL);
	if (pl)
		CHECK_STR(pl->value, "1");
	CHECK(mgcp_msg_param_named(&m, "L/FOO") != NULL);
	CHECK(mgcp_msg_param(&m, MGCP_PARAM_UNKNOWN) == NULL);

	CASE("a parameter line without a colon is flagged, not fatal");
	CHECK(parse("RQNT 1 aaln/1@gw MGCP 1.0\r\nbroken line\r\nX: 7\r\n") ==
	      MGCP_PARSE_OK);
	CHECK(m.flags & MGCP_MSG_F_PARAM_BAD);
	CHECK(m.param[0].code == MGCP_PARAM_UNKNOWN);
	CHECK_STR(m.param[0].name, "broken line");
	CHECK_STR(val(MGCP_PARAM_X), "7");

	CASE("repeated parameter lines iterate in order");
	CHECK(parse("200 1200 OK\r\nZ: aaln/1@rgw\r\nI: 1\r\nZ: aaln/2@rgw\r\n")
	      == MGCP_PARSE_OK);
	n = 0;
	for (pl = mgcp_msg_param(&m, MGCP_PARAM_Z); pl;
	     pl = mgcp_msg_param_next(&m, pl)) {
		CHECK_STR(pl->value, n ? "aaln/2@rgw" : "aaln/1@rgw");
		n++;
	}
	CHECK_UINT(n, 2);

	CASE("64 parameter lines fit, a 65th refuses the message");
	{
		char text[4096];
		size_t used;
		unsigned int i;

		used = (size_t)snprintf(text, sizeof(text),
					"AUEP 1 aaln/1@gw MGCP 1.0\r\n");
		for (i = 0; i < MGCP_PARAM_LINE_MAX; i++)
			used += (size_t)snprintf(text + used,
						 sizeof(text) - used,
						 "X-P%u: v\r\n", i);
		CHECK(parse_len(text, used) == MGCP_PARSE_OK);
		CHECK_UINT(m.param_count, MGCP_PARAM_LINE_MAX);
		used += (size_t)snprintf(text + used, sizeof(text) - used,
					 "X-P64: v\r\n");
		CHECK(parse_len(text, used) == MGCP_PARSE_ERR_PARAM_OVERFLOW);
	}

	CASE("a last SDP line without its line end is accepted and flagged");
	CHECK(parse("200 1001 OK\r\nI: 1\r\n\r\nv=0\r\nm=audio 1 RTP/AVP 0") ==
	      MGCP_PARSE_OK);
	CHECK(m.flags & MGCP_MSG_F_NO_FINAL_EOL);
	CHECK_STR(m.sdp, "v=0\r\nm=audio 1 RTP/AVP 0");
	CHECK_STR(val(MGCP_PARAM_I), "1");

	CASE("a last parameter line without its line end is accepted and "
	     "flagged");
	CHECK(parse("NTFY 5 aaln/1@gw MGCP 1.0\r\nX: 1\r\nO: L/hd") ==
	      MGCP_PARSE_OK);
	CHECK(m.flags & MGCP_MSG_F_NO_FINAL_EOL);
	CHECK_STR(val(MGCP_PARAM_O), "L/hd");
	CHECK(parse("NTFY 5 aaln/1@gw MGCP 1.0\r\nX: 1\r") == MGCP_PARSE_OK);
	CHECK(m.flags & MGCP_MSG_F_NO_FINAL_EOL);
	CHECK_STR(val(MGCP_PARAM_X), "1");
	CHECK(parse("000 1206") == MGCP_PARSE_OK);
	CHECK(m.flags & MGCP_MSG_F_NO_FINAL_EOL);
	CHECK_UINT(m.txid, 1206);
	CHECK(parse("NTFY 5 aaln/1@gw MGCP 1.0\r\nX: 1\r\n") ==
	      MGCP_PARSE_OK);
	CHECK(!(m.flags & MGCP_MSG_F_NO_FINAL_EOL));

	CASE("a line of white space alone ends the parameters (Appendix A)");
	CHECK(parse("200 1001 OK\r\nI: 1\r\n \r\nv=0\r\n") == MGCP_PARSE_OK);
	CHECK_UINT(m.param_count, 1);
	CHECK_STR(m.sdp, "v=0\r\n");
	CHECK(parse("200 1001 OK\r\nI: 1\r\n\t \r\nv=0\r\n") == MGCP_PARSE_OK);
	CHECK_STR(m.sdp, "v=0\r\n");

	CASE("a quoted parameter value can hold a line end (Appendix A)");
	CHECK(parse("RQNT 1 aaln/1@gw MGCP 1.0\r\n"
		    "S: L/ci(10/14, \"555\r\n1212\", \"Doe\"), L/rg\r\n"
		    "X: 1\r\n") == MGCP_PARSE_OK);
	CHECK_UINT(m.param_count, 2);
	CHECK_STR(val(MGCP_PARAM_S),
		  "L/ci(10/14, \"555\r\n1212\", \"Doe\"), L/rg");
	CHECK_STR(val(MGCP_PARAM_X), "1");

	CASE("a quote that never closes ends the line at its line end");
	CHECK(parse("RQNT 1 aaln/1@gw MGCP 1.0\r\nS: L/adsi(\"abc\r\nX: 7\r\n")
	      == MGCP_PARSE_OK);
	CHECK_UINT(m.param_count, 2);
	CHECK_STR(val(MGCP_PARAM_S), "L/adsi(\"abc");
	CHECK_STR(val(MGCP_PARAM_X), "7");

	CASE("a literal quote inside a value does not join two lines");
	CHECK(parse("RQNT 1 aaln/1@gw MGCP 1.0\r\n"
		    "X-A: a\"b\r\nX-B: c\"d\r\n") == MGCP_PARSE_OK);
	CHECK_UINT(m.param_count, 2);
	CHECK_STR(m.param[0].value, "a\"b");
	CHECK_STR(m.param[1].value, "c\"d");
	CHECK(parse("RQNT 1 aaln/1@gw MGCP 1.0\r\nS: L/x(ab\"c)\r\n"
		    "X: 1\"\r\n") == MGCP_PARSE_OK);
	CHECK_UINT(m.param_count, 2);
	CHECK_STR(val(MGCP_PARAM_X), "1\"");

	CASE("a quote opens where a value starts (Appendix A pages 172, 173)");
	CHECK(parse("RQNT 1 aaln/1@gw MGCP 1.0\r\nX-V:  \"a\r\nb\"\r\nX: 1\r\n")
	      == MGCP_PARSE_OK);
	CHECK_UINT(m.param_count, 2);
	CHECK_STR(m.param[0].value, "\"a\r\nb\"");
	CHECK(parse("RQNT 1 aaln/1@gw MGCP 1.0\r\nS: L/x(a= \"p\r\nq\")\r\n"
		    "X: 1\r\n") == MGCP_PARSE_OK);
	CHECK_UINT(m.param_count, 2);
	CHECK_STR(val(MGCP_PARAM_S), "L/x(a= \"p\r\nq\")");
	CHECK(parse("RQNT 1 aaln/1@gw MGCP 1.0\r\n"
		    "S: L/x(\"a\"\"\r\nb\")\r\nX: 1\r\n") == MGCP_PARSE_OK);
	CHECK_UINT(m.param_count, 2);
	CHECK_STR(val(MGCP_PARAM_X), "1");

	CASE("an empty line with no description after it is an empty body");
	CHECK(parse("200 5 OK\r\nI: 1\r\n\r\n") == MGCP_PARSE_OK);
	CHECK_UINT(m.sdp.len, 0);
	CHECK(parse("200 5 OK\r\n\r\n\r\n") == MGCP_PARSE_OK);
	CHECK_UINT(m.sdp.len, 0);
}

static void prefix_loop_tests(void)
{
	struct mgcp_msg full;
	char *copy;
	size_t len = strlen(crcx_long), first, n, i;
	unsigned int bad = 0, ok = 0;

	/*
	 * Once the first line is whole, every prefix parses: the datagram end
	 * ends the message. Each parameter but the last is whole, the last is
	 * a prefix of its full value, and the flag marks a cut line.
	 */
	CASE("every prefix of a long command parses to a sane answer");
	copy = malloc(len);
	CHECK(copy != NULL);
	if (!copy)
		return;
	memcpy(copy, crcx_long, len);
	CHECK(mgcp_msg_parse(&full, copy, len) == MGCP_PARSE_OK);
	CHECK_UINT(full.param_count, 7);
	first = (size_t)(strchr(crcx_long, '\n') - crcx_long) + 1;

	for (n = 1; n <= len; n++) {
		enum mgcp_parse_status st = parse_len(crcx_long, n);
		bool at_end = crcx_long[n - 1] == '\n';

		if (st != MGCP_PARSE_OK) {
			if (n >= first)
				bad++;
			continue;
		}

		ok++;
		if (!(m.flags & MGCP_MSG_F_NO_FINAL_EOL) != at_end)
			bad++;
		if (m.param_count > full.param_count)
			bad++;
		for (i = 0; i < m.param_count; i++) {
			struct sip_str got = m.param[i].value;
			struct sip_str want = full.param[i].value;
			bool last = i + 1 == m.param_count && !at_end;

			if (got.len > want.len ||
			    (!last && got.len != want.len) ||
			    memcmp(got.p, want.p, got.len))
				bad++;
		}
		if (m.sdp.len > full.sdp.len ||
		    memcmp(m.sdp.p, full.sdp.p, m.sdp.len))
			bad++;
	}
	CHECK_UINT(bad, 0);
	CHECK(ok >= len - first + 1);
	free(copy);
}

static void name_tests(void)
{
	char text[2048], part[300];

	memset(part, 'a', 256);
	part[256] = 0;

	CASE("an endpoint half over 255 characters is flagged (3.2.1.3)");
	snprintf(text, sizeof(text), "AUEP 1 %s@gw MGCP 1.0\r\n", part);
	CHECK(parse(text) == MGCP_PARSE_OK);
	CHECK(m.flags & MGCP_MSG_F_NAME_LONG);
	CHECK_UINT(m.endpoint.len, 259);
	snprintf(text, sizeof(text), "AUEP 1 aaln/1@%s MGCP 1.0\r\n", part);
	CHECK(parse(text) == MGCP_PARSE_OK);
	CHECK(m.flags & MGCP_MSG_F_NAME_LONG);

	CASE("two halves of 255 characters each are not flagged");
	part[255] = 0;
	snprintf(text, sizeof(text), "AUEP 1 %s@%s MGCP 1.0\r\n", part, part);
	CHECK(parse(text) == MGCP_PARSE_OK);
	CHECK(!(m.flags & MGCP_MSG_F_NAME_LONG));
	CHECK_UINT(m.endpoint.len, 511);

	CASE("an overlong verb and an overlong value");
	memset(part, 'Q', 299);
	part[299] = 0;
	snprintf(text, sizeof(text), "%s 1 aaln/1@gw MGCP 1.0\r\nX: %s\r\n",
		 part, part);
	CHECK(parse(text) == MGCP_PARSE_OK);
	CHECK(m.verb == MGCP_VERB_UNKNOWN);
	CHECK(m.flags & MGCP_MSG_F_VERB_BAD);
	CHECK_UINT(m.verb_name.len, 299);
	CHECK_UINT(val(MGCP_PARAM_X).len, 299);
}

/* ------------------------------------------------------------ piggyback */

static void split_tests(void)
{
	static const char two[] =
		"200 2005 OK\r\n"
		".\r\n"
		"DLCX 1244 card23/21@tgw-7.example.net MGCP 1.0\r\n"
		"C: A3C47F21456789F0\r\n"
		"I: FDE234C8\r\n";
	static const char three[] =
		"RSIP 1 aaln/*@gw MGCP 1.0\nRM: restart\n"
		".\n"
		"NTFY 2 aaln/1@gw MGCP 1.0\nX: 0\nO: L/hd\n"
		".\n"
		"NTFY 3 aaln/1@gw MGCP 1.0\nX: 0\nO: L/hu\n";
	struct mgcp_split it;
	struct sip_str one;
	uint32_t ids[4];
	unsigned int n;

	CASE("a datagram of two messages (3.5.5 example)");
	mgcp_split_init(&it, two, strlen(two));
	CHECK(mgcp_split_next(&it, &one));
	CHECK_STR(one, "200 2005 OK\r\n");
	CHECK(mgcp_split_next(&it, &one));
	CHECK(parse_len(one.p, one.len) == MGCP_PARSE_OK);
	CHECK(m.verb == MGCP_VERB_DLCX);
	CHECK_UINT(m.txid, 1244);
	CHECK_STR(val(MGCP_PARAM_I), "FDE234C8");
	CHECK(!mgcp_split_next(&it, &one));

	CASE("a datagram of three messages, in order, with bare LF");
	mgcp_split_init(&it, three, strlen(three));
	n = 0;
	while (mgcp_split_next(&it, &one) && n < 4) {
		CHECK(parse_len(one.p, one.len) == MGCP_PARSE_OK);
		ids[n++] = m.txid;
	}
	CHECK_UINT(n, 3);
	CHECK_UINT(ids[0], 1);
	CHECK_UINT(ids[1], 2);
	CHECK_UINT(ids[2], 3);

	CASE("a single message passes whole");
	mgcp_split_init(&it, crcx_long, strlen(crcx_long));
	CHECK(mgcp_split_next(&it, &one));
	CHECK_UINT(one.len, strlen(crcx_long));
	CHECK(!mgcp_split_next(&it, &one));

	CASE("a \".\" line inside an SDP body separates (3.4, 3.5.5)");
	{
		static const char body_dot[] =
			"CRCX 9 aaln/1@gw MGCP 1.0\r\nC: 1\r\nM: recvonly\r\n"
			"\r\n"
			"v=0\r\n"
			".\r\n"
			"m=audio 3456 RTP/AVP 0\r\n";

		mgcp_split_init(&it, body_dot, strlen(body_dot));
		CHECK(mgcp_split_next(&it, &one));
		CHECK(parse_len(one.p, one.len) == MGCP_PARSE_OK);
		CHECK_STR(m.sdp, "v=0\r\n");
		CHECK(mgcp_split_next(&it, &one));
		CHECK_STR(one, "m=audio 3456 RTP/AVP 0\r\n");
		CHECK(parse_len(one.p, one.len) == MGCP_PARSE_OK);
		CHECK(m.verb == MGCP_VERB_UNKNOWN);
		CHECK(m.flags & MGCP_MSG_F_VERB_BAD);
		CHECK(m.flags & MGCP_MSG_F_BAD_VERSION);
	}

	CASE("only a line that holds one dot separates");
	{
		static const char dots[] =
			"200 1 OK\r\n"
			"\r\n"
			"v=0\r\n"
			"..\r\n"
			". x\r\n"
			"a=.\r\n"
			" .\r\n"
			".  \t\r\n"
			"200 2 OK\r\n";

		mgcp_split_init(&it, dots, strlen(dots));
		CHECK(mgcp_split_next(&it, &one));
		CHECK(parse_len(one.p, one.len) == MGCP_PARSE_OK);
		CHECK_STR(m.sdp, "v=0\r\n..\r\n. x\r\na=.\r\n .\r\n");
		CHECK(mgcp_split_next(&it, &one));
		CHECK_STR(one, "200 2 OK\r\n");
		CHECK(!mgcp_split_next(&it, &one));
	}

	CASE("empty parts and separators at the edges are skipped");
	{
		static const char edges[] =
			".\r\n\r\n.\r\n200 1 OK\r\n.\r\n.\n.";

		mgcp_split_init(&it, edges, strlen(edges));
		CHECK(mgcp_split_next(&it, &one));
		CHECK_STR(one, "200 1 OK\r\n");
		CHECK(!mgcp_split_next(&it, &one));
		mgcp_split_init(&it, "", 0);
		CHECK(!mgcp_split_next(&it, &one));
		mgcp_split_init(&it, NULL, 0);
		CHECK(!mgcp_split_next(&it, &one));
	}

	CASE("a broken second message does not affect the first");
	{
		static const char cut[] =
			"200 1 OK\r\n.\r\nNTFY 0 aaln/1@gw MG";

		mgcp_split_init(&it, cut, strlen(cut));
		CHECK(mgcp_split_next(&it, &one));
		CHECK(parse_len(one.p, one.len) == MGCP_PARSE_OK);
		CHECK(mgcp_split_next(&it, &one));
		CHECK(parse_len(one.p, one.len) == MGCP_PARSE_ERR_TXID);
	}

	CASE("a \".\" line inside a quoted value does not split (Appendix A)");
	{
		static const char quoted[] =
			"RQNT 7 aaln/1@gw MGCP 1.0\r\n"
			"S: L/adsi(\"a\r\n.\r\nb\")\r\n"
			"X: 1\r\n"
			".\r\n"
			"200 8 OK\r\n";

		mgcp_split_init(&it, quoted, strlen(quoted));
		CHECK(mgcp_split_next(&it, &one));
		CHECK(parse_len(one.p, one.len) == MGCP_PARSE_OK);
		CHECK_UINT(m.txid, 7);
		CHECK_UINT(m.param_count, 2);
		CHECK_STR(val(MGCP_PARAM_S), "L/adsi(\"a\r\n.\r\nb\")");
		CHECK(mgcp_split_next(&it, &one));
		CHECK_STR(one, "200 8 OK\r\n");
		CHECK(!mgcp_split_next(&it, &one));
	}

	CASE("literal quotes in two messages do not join them (Appendix A)");
	{
		static const char lit[] =
			"RQNT 7 aaln/1@gw MGCP 1.0\r\n"
			"X-A: a\"b\r\n"
			".\r\n"
			"RQNT 8 aaln/1@gw MGCP 1.0\r\n"
			"X-B: c\"d\r\n";
		unsigned int n = 0;

		mgcp_split_init(&it, lit, strlen(lit));
		while (mgcp_split_next(&it, &one)) {
			CHECK(parse_len(one.p, one.len) == MGCP_PARSE_OK);
			CHECK_UINT(m.txid, 7 + n);
			CHECK_UINT(m.param_count, 1);
			n++;
		}
		CHECK_UINT(n, 2);
	}
}

/* -------------------------------------------------------- R:, S:, O:, T: */

static void requested_event_tests(void)
{
	struct mgcp_req_iter it;
	struct mgcp_req_event re;
	struct mgcp_embedded emb;
	struct mgcp_req_iter eit;
	struct mgcp_req_event ere;
	unsigned int n;

	CASE("R: actions per event (3.2.2.16 examples)");
	CHECK(mgcp_req_first(sip_str_cstr("L/hu(N), L/hf(S,N)"), pkg_l, &it,
			     &re));
	CHECK_STR(re.ev.pkg, "L");
	CHECK_STR(re.ev.name, "hu");
	CHECK_UINT(re.act, MGCP_ACT_N);
	CHECK(mgcp_req_next(&it, &re));
	CHECK_STR(re.ev.name, "hf");
	CHECK_UINT(re.act, MGCP_ACT_S | MGCP_ACT_N);
	CHECK(!mgcp_req_next(&it, &re));
	CHECK(!it.bad);

	CASE("every action of 3.2.2.16");
	CHECK(mgcp_req_first(sip_str_cstr("L/hd(N),L/hu(A), D/[0-9#T](D),"
					  " L/hf(S), L/oc(I), G/ft(K),"
					  " L/rg(E(S(L/dl)))"), pkg_l, &it,
			     &re));
	CHECK_UINT(re.act, MGCP_ACT_N);
	CHECK(mgcp_req_next(&it, &re));
	CHECK_UINT(re.act, MGCP_ACT_A);
	CHECK(mgcp_req_next(&it, &re));
	CHECK_STR(re.ev.pkg, "D");
	CHECK_STR(re.ev.name, "[0-9#T]");
	CHECK_UINT(re.act, MGCP_ACT_D);
	CHECK(mgcp_req_next(&it, &re));
	CHECK_UINT(re.act, MGCP_ACT_S);
	CHECK(mgcp_req_next(&it, &re));
	CHECK_UINT(re.act, MGCP_ACT_I);
	CHECK(mgcp_req_next(&it, &re));
	CHECK_STR(re.ev.pkg, "G");
	CHECK_UINT(re.act, MGCP_ACT_K);
	CHECK(mgcp_req_next(&it, &re));
	CHECK_UINT(re.act, MGCP_ACT_E);
	CHECK_STR(re.embedded, "S(L/dl)");
	CHECK(!mgcp_req_next(&it, &re));
	CHECK(!it.bad);

	CASE("one embedded request with its three groups (3.2.2.16)");
	CHECK(mgcp_req_first(sip_str_cstr("L/hd(E(R(D/[0-9#T](D),L/hu(N)),"
					  "S(L/dl),D([0-9].[#T])))"), pkg_l,
			     &it, &re));
	CHECK_UINT(re.act, MGCP_ACT_E);
	CHECK(mgcp_embedded_parse(re.embedded, &emb));
	CHECK(emb.has_r && emb.has_s && emb.has_d);
	CHECK_STR(emb.r, "D/[0-9#T](D),L/hu(N)");
	CHECK_STR(emb.s, "L/dl");
	CHECK_STR(emb.d, "[0-9].[#T]");
	n = 0;
	if (mgcp_req_first(emb.r, pkg_l, &eit, &ere)) {
		do {
			n++;
		} while (mgcp_req_next(&eit, &ere));
	}
	CHECK_UINT(n, 2);
	CHECK(!eit.bad);

	CASE("an embedded request in another group order (Appendix F.1)");
	CHECK(mgcp_req_first(sip_str_cstr("L/hd(A, E(S(L/dl),R(L/oc, L/hu, "
					  "D/[0-9#*T](D))))"), pkg_l, &it,
			     &re));
	CHECK_UINT(re.act, MGCP_ACT_A | MGCP_ACT_E);
	CHECK(mgcp_embedded_parse(re.embedded, &emb));
	CHECK(emb.has_r && emb.has_s && !emb.has_d);
	CHECK_STR(emb.s, "L/dl");
	CHECK_STR(emb.r, "L/oc, L/hu, D/[0-9#*T](D)");

	CASE("embedded request refusals");
	CHECK(!mgcp_embedded_parse(sip_str_cstr("X(L/dl)"), &emb));
	CHECK(!mgcp_embedded_parse(sip_str_cstr("S(L/dl),S(L/rg)"), &emb));
	CHECK(!mgcp_embedded_parse(sip_str_cstr(""), &emb));
	CHECK(!mgcp_embedded_parse(sip_str_cstr("S(L/dl"), &emb));
	CHECK(!mgcp_embedded_parse(sip_str_cstr("SS(L/dl)"), &emb));

	CASE("an event without actions takes Notify (3.2.2.16)");
	CHECK(mgcp_req_first(sip_str_cstr("L/hu"), pkg_l, &it, &re));
	CHECK_UINT(re.act, MGCP_ACT_N);
	CHECK_UINT(re.actions.len, 0);

	CASE("actions compare case insensitively, names keep their case");
	CHECK(mgcp_req_first(sip_str_cstr("l/hd(n,k)"), pkg_l, &it, &re));
	CHECK_STR(re.ev.pkg, "l");
	CHECK_UINT(re.act, MGCP_ACT_N | MGCP_ACT_K);

	CASE("event parameters after the actions (3.2.2.16)");
	CHECK(mgcp_req_first(sip_str_cstr("X/foobar(N)(epar=2)"), pkg_l, &it,
			     &re));
	CHECK_STR(re.ev.pkg, "X");
	CHECK_STR(re.ev.name, "foobar");
	CHECK(re.ev.has_params);
	CHECK_STR(re.ev.params, "epar=2");
	CHECK_STR(re.ev.raw, "X/foobar(N)(epar=2)");

	CASE("the default package fills a name without one (3.2.2.4)");
	CHECK(mgcp_req_first(sip_str_cstr("hd(N), [0-9#T](D)"), pkg_l, &it,
			     &re));
	CHECK(re.ev.pkg_default);
	CHECK_STR(re.ev.pkg, "L");
	CHECK_STR(re.ev.name, "hd");
	it.default_pkg = pkg_d;
	CHECK(mgcp_req_next(&it, &re));
	CHECK_STR(re.ev.pkg, "D");
	CHECK_STR(re.ev.name, "[0-9#T]");

	CASE("a connection and the wildcard forms (3.2.2.4)");
	CHECK(mgcp_req_first(sip_str_cstr("G/rt@0A3F58(N), R/qa@*, G/rt@$, "
					  "T/all, */all"), pkg_l, &it, &re));
	CHECK_STR(re.ev.name, "rt");
	CHECK_STR(re.ev.conn, "0A3F58");
	CHECK(mgcp_req_next(&it, &re));
	CHECK_STR(re.ev.conn, "*");
	CHECK(mgcp_req_next(&it, &re));
	CHECK_STR(re.ev.conn, "$");
	CHECK(mgcp_req_next(&it, &re));
	CHECK_STR(re.ev.name, "all");
	CHECK(mgcp_req_next(&it, &re));
	CHECK_STR(re.ev.pkg, "*");

	CASE("an extension action is kept (Appendix A ExtensionAction)");
	CHECK(mgcp_req_first(sip_str_cstr("L/hd(N, X/foo(1,2))"), pkg_l, &it,
			     &re));
	CHECK_UINT(re.act, MGCP_ACT_N | MGCP_ACT_EXT);
	CHECK_STR(re.ext, "X/foo(1,2)");

	CASE("malformed requested events stop the walk with bad set");
	CHECK(!mgcp_req_first(sip_str_cstr("L/hd("), pkg_l, &it, &re));
	CHECK(it.bad);
	CHECK(!mgcp_req_first(sip_str_cstr("L/hd()"), pkg_l, &it, &re));
	CHECK(it.bad);
	CHECK(!mgcp_req_first(sip_str_cstr("L/hd(N)x"), pkg_l, &it, &re));
	CHECK(it.bad);
	CHECK(!mgcp_req_first(sip_str_cstr("L/hd(E(S(L/dl)),E(S(L/rg)))"),
			      pkg_l, &it, &re));
	CHECK(it.bad);
	CHECK(!mgcp_req_first(sip_str_cstr("/hd(N)"), pkg_l, &it, &re));
	CHECK(it.bad);
	CHECK(mgcp_req_first(sip_str_cstr("L/hu(N), L/hd("), pkg_l, &it,
			     &re));
	CHECK(!mgcp_req_next(&it, &re));
	CHECK(it.bad);
	CHECK(!mgcp_req_first(sip_str_cstr(""), pkg_l, &it, &re));
	CHECK(!it.bad);
}

static void signal_tests(void)
{
	struct mgcp_sig_iter it;
	struct mgcp_event ev;
	struct sip_str rest, name, value;
	char out[64];
	unsigned int n;

	CASE("S: signals with quoted parameters (3.2.2.21)");
	CHECK(mgcp_sig_first(sip_str_cstr("L/adsi(\"123456 Your friend\"), "
					  "L/rg"), pkg_l, &it, &ev));
	CHECK_STR(ev.name, "adsi");
	CHECK_STR(ev.params, "\"123456 Your friend\"");
	CHECK(mgcp_unquote(ev.params, out, sizeof(out)) == 18);
	CHECK(!strcmp(out, "123456 Your friend"));
	CHECK(mgcp_sig_next(&it, &ev));
	CHECK_STR(ev.name, "rg");
	CHECK(!ev.has_params);
	CHECK(!mgcp_sig_next(&it, &ev));

	CASE("S: parameters that hold commas and spaces");
	CHECK(mgcp_sig_first(sip_str_cstr("A/ann(http://ann.example.net/"
					  "no-such-number.au, 1234567)"),
			     pkg_l, &it, &ev));
	rest = ev.params;
	CHECK(mgcp_event_param_next(&rest, &name, &value));
	CHECK_UINT(name.len, 0);
	CHECK_STR(value, "http://ann.example.net/no-such-number.au");
	CHECK(mgcp_event_param_next(&rest, &name, &value));
	CHECK_STR(value, "1234567");
	CHECK(!mgcp_event_param_next(&rest, &name, &value));

	CASE("the caller id signal with a quoted comma (NCS L/ci)");
	CHECK(mgcp_sig_first(sip_str_cstr("L/ci(10/14/17/26, \"555 1212\", "
					  "\"Doe, John\"), L/rg"), pkg_l, &it,
			     &ev));
	rest = ev.params;
	n = 0;
	while (mgcp_event_param_next(&rest, &name, &value)) {
		if (n == 0)
			CHECK_STR(value, "10/14/17/26");
		if (n == 1)
			CHECK_STR(value, "\"555 1212\"");
		if (n == 2) {
			CHECK_STR(value, "\"Doe, John\"");
			CHECK(mgcp_unquote(value, out, sizeof(out)) == 9);
			CHECK(!strcmp(out, "Doe, John"));
		}
		n++;
	}
	CHECK_UINT(n, 3);
	CHECK(mgcp_sig_next(&it, &ev));
	CHECK_STR(ev.name, "rg");

	CASE("named and nested signal parameters (3.2.2.4)");
	CHECK(mgcp_sig_first(sip_str_cstr("L/rg(to=6000), L/rg(to(6000)), "
					  "L/vmwi(+)"), pkg_l, &it, &ev));
	rest = ev.params;
	CHECK(mgcp_event_param_next(&rest, &name, &value));
	CHECK_STR(name, "to");
	CHECK_STR(value, "6000");
	CHECK(mgcp_sig_next(&it, &ev));
	rest = ev.params;
	CHECK(mgcp_event_param_next(&rest, &name, &value));
	CHECK_STR(name, "to");
	CHECK_STR(value, "6000");
	CHECK(mgcp_sig_next(&it, &ev));
	CHECK_STR(ev.params, "+");

	CASE("a doubled quote is an escaped quote (Appendix A quotedString)");
	CHECK(mgcp_sig_first(sip_str_cstr("L/adsi(\"say \"\"hi, you\"\"\"), "
					  "L/rg"), pkg_l, &it, &ev));
	CHECK(mgcp_unquote(ev.params, out, sizeof(out)) == 13);
	CHECK(!strcmp(out, "say \"hi, you\""));
	CHECK(mgcp_sig_next(&it, &ev));
	CHECK_STR(ev.name, "rg");
	CHECK(mgcp_unquote(sip_str_cstr("\"toolong\""), out, 4) == -1);

	CASE("an empty S: is an empty list, not an error");
	CHECK(!mgcp_sig_first(sip_str_cstr(""), pkg_l, &it, &ev));
	CHECK(!it.bad);

	CASE("O: and T: use the signal grammar (Appendix A)");
	CHECK(mgcp_sig_first(sip_str_cstr("L/hd,D/9,D/1,L/oc(L/rg),D/T"),
			     pkg_l, &it, &ev));
	n = 1;
	while (mgcp_sig_next(&it, &ev)) {
		if (n == 3) {
			CHECK_STR(ev.name, "oc");
			CHECK_STR(ev.params, "L/rg");
		}
		n++;
	}
	CHECK_UINT(n, 5);
	CHECK(!it.bad);
	CHECK(mgcp_sig_first(sip_str_cstr("L/hu,L/hd,L/hf,D/[0-9#*]"), pkg_l,
			     &it, &ev));
	CHECK(mgcp_sig_next(&it, &ev));
	CHECK(mgcp_sig_next(&it, &ev));
	CHECK(mgcp_sig_next(&it, &ev));
	CHECK_STR(ev.name, "[0-9#*]");

	CASE("malformed signals stop the walk with bad set");
	CHECK(!mgcp_sig_first(sip_str_cstr("L/rg(to=6000"), pkg_l, &it, &ev));
	CHECK(it.bad);
	CHECK(!mgcp_sig_first(sip_str_cstr("L/rg(1)(2)"), pkg_l, &it, &ev));
	CHECK(it.bad);
	CHECK(!mgcp_sig_first(sip_str_cstr("L/@c"), pkg_l, &it, &ev));
	CHECK(it.bad);
}

/* ---------------------------------------------------- typed parameters */

static void lco_tests(void)
{
	struct mgcp_lco lco;
	struct sip_str rest, codec;
	unsigned int n;

	CASE("L: attributes (3.2.2.10)");
	CHECK(mgcp_lco_parse(sip_str_cstr("p:10-20, a:PCMU;G726-32, b:64, "
					  "e:off, gc:-6, s:on, t:b8, r:cl, "
					  "k:clear:secret, nt:IN"), &lco));
	CHECK_STR(lco.p, "10-20");
	CHECK_STR(lco.a, "PCMU;G726-32");
	CHECK_STR(lco.b, "64");
	CHECK_STR(lco.e, "off");
	CHECK_STR(lco.gc, "-6");
	CHECK_STR(lco.s, "on");
	CHECK_STR(lco.t, "b8");
	CHECK_STR(lco.r, "cl");
	CHECK_STR(lco.k, "clear:secret");
	CHECK_STR(lco.nt, "IN");
	CHECK_UINT(lco.unknown, 0);

	CASE("the a: codec list walks on ';'");
	rest = lco.a;
	n = 0;
	while (mgcp_codec_next(&rest, &codec)) {
		CHECK_STR(codec, n ? "G726-32" : "PCMU");
		n++;
	}
	CHECK_UINT(n, 2);
	rest = sip_str_cstr("image/t38 ; ;PCMA");
	CHECK(mgcp_codec_next(&rest, &codec));
	CHECK_STR(codec, "image/t38");
	CHECK(mgcp_codec_next(&rest, &codec));
	CHECK_STR(codec, "PCMA");
	CHECK(!mgcp_codec_next(&rest, &codec));

	CASE("L: vendor attributes: x+ is mandatory, x- is not (3.2.2.10)");
	CHECK(mgcp_lco_parse(sip_str_cstr("x-foo:1, a:PCMU, x+bar, X+baz:2"),
			     &lco));
	CHECK_UINT(lco.unknown, 3);
	CHECK_STR(lco.ext_mandatory, "x+bar");
	CHECK_STR(lco.a, "PCMU");
	CHECK(!mgcp_lco_parse(sip_str_cstr("a:PCMU, :x"), &lco));

	CASE("a stray \"[\" in an LCO value does not join two attributes");
	CHECK(mgcp_lco_parse(sip_str_cstr("a:foo[bar, p:20"), &lco));
	CHECK_STR(lco.a, "foo[bar");
	CHECK_STR(lco.p, "20");
	rest = sip_str_cstr("x[, y");
	CHECK(mgcp_list_next(&rest, &codec));
	CHECK_STR(codec, "x[");
	CHECK(mgcp_list_next(&rest, &codec));
	CHECK_STR(codec, "y");

	CASE("L: from a message, case insensitive attribute names");
	CHECK(parse("CRCX 1 aaln/1@gw MGCP 1.0\r\nL: P:20, A:PCMA\r\n") ==
	      MGCP_PARSE_OK);
	CHECK(mgcp_msg_lco(&m, &lco));
	CHECK_STR(lco.p, "20");
	CHECK_STR(lco.a, "PCMA");

	CASE("A: capabilities use the same attributes plus v and m (3.2.2.3)");
	CHECK(mgcp_lco_parse(sip_str_cstr("a:PCMU;G728, p:10-100, e:on, s:off,"
					  " t:1, v:L, m:sendonly;recvonly;"
					  "sendrecv;inactive"), &lco));
	CHECK_STR(lco.v, "L");
	CHECK_STR(lco.m, "sendonly;recvonly;sendrecv;inactive");
	CHECK_STR(lco.p, "10-100");
}

static void typed_tests(void)
{
	struct mgcp_entity e;
	struct mgcp_quarantine q;
	struct mgcp_ack_iter ai;
	struct mgcp_reason r;
	struct mgcp_conn_params cp;
	struct mgcp_bearer b;
	struct sip_str v, rest, name;
	enum mgcp_param code;
	enum mgcp_mode mode;
	enum mgcp_rm rm;
	uint32_t lo, hi, u;

	CASE("C:, X: and I: are 1 to 32 hex digits (3.2.2.2, .5, .18)");
	CHECK(parse("MDCX 1 aaln/1@gw MGCP 1.0\r\nC: A3C47F21456789F0\r\n"
		    "I: FDE234C8, DFE233D1\r\nX: 0123456789AC\r\n") ==
	      MGCP_PARSE_OK);
	CHECK(mgcp_msg_call_id(&m, &v));
	CHECK_STR(v, "A3C47F21456789F0");
	CHECK(mgcp_msg_conn_id(&m, &v));
	CHECK_STR(v, "FDE234C8");
	CHECK(mgcp_msg_request_id(&m, &v));
	CHECK(mgcp_hex_id_valid(sip_str_cstr("0")));
	CHECK(mgcp_hex_id_valid(sip_str_cstr(
		"0123456789abcdef0123456789ABCDEF")));
	CHECK(!mgcp_hex_id_valid(sip_str_cstr(
		"0123456789abcdef0123456789ABCDEF0")));
	CHECK(!mgcp_hex_id_valid(sip_str_cstr("12G4")));
	CHECK(!mgcp_hex_id_valid(sip_str_cstr("")));
	CHECK(parse("MDCX 1 aaln/1@gw MGCP 1.0\r\nC: xyz\r\n") ==
	      MGCP_PARSE_OK);
	CHECK(!mgcp_msg_call_id(&m, &v));
	CHECK(!mgcp_msg_conn_id(&m, &v));

	CASE("N: notified entity forms (3.2.1.3, Appendix A)");
	CHECK(mgcp_entity_parse(sip_str_cstr("ca@ca1.whatever.net:5678"), &e));
	CHECK_STR(e.local, "ca");
	CHECK_STR(e.domain, "ca1.whatever.net");
	CHECK_UINT(e.port, 5678);
	CHECK(mgcp_entity_parse(sip_str_cstr("[128.96.41.12]"), &e));
	CHECK_UINT(e.local.len, 0);
	CHECK_STR(e.domain, "[128.96.41.12]");
	CHECK_UINT(e.port, 0);
	CHECK(mgcp_entity_parse(sip_str_cstr("CA-1@whatever.net"), &e));
	CHECK_STR(e.local, "CA-1");
	CHECK(mgcp_entity_parse(sip_str_cstr("ca@[2001:db8::1]:2727"), &e));
	CHECK_STR(e.domain, "[2001:db8::1]");
	CHECK_UINT(e.port, 2727);
	CHECK(!mgcp_entity_parse(sip_str_cstr("x@y:0"), &e));
	CHECK(!mgcp_entity_parse(sip_str_cstr("x@y:70000"), &e));
	CHECK(!mgcp_entity_parse(sip_str_cstr("x@y:"), &e));
	CHECK(!mgcp_entity_parse(sip_str_cstr("@y"), &e));
	CHECK(!mgcp_entity_parse(sip_str_cstr("x@[1.2.3.4"), &e));
	CHECK(!mgcp_entity_parse(sip_str_cstr("x@[1.2.3.4]5"), &e));
	CHECK(!mgcp_entity_parse(sip_str_cstr(""), &e));

	CASE("M: every mode of 3.2.2.6 and an extension mode");
	CHECK(mgcp_mode_parse(sip_str_cstr("sendonly")) == MGCP_MODE_SENDONLY);
	CHECK(mgcp_mode_parse(sip_str_cstr("RECVONLY")) == MGCP_MODE_RECVONLY);
	CHECK(mgcp_mode_parse(sip_str_cstr("sendrecv")) == MGCP_MODE_SENDRECV);
	CHECK(mgcp_mode_parse(sip_str_cstr("confrnce")) == MGCP_MODE_CONFRNCE);
	CHECK(mgcp_mode_parse(sip_str_cstr("inactive")) == MGCP_MODE_INACTIVE);
	CHECK(mgcp_mode_parse(sip_str_cstr("loopback")) == MGCP_MODE_LOOPBACK);
	CHECK(mgcp_mode_parse(sip_str_cstr("conttest")) == MGCP_MODE_CONTTEST);
	CHECK(mgcp_mode_parse(sip_str_cstr("netwloop")) == MGCP_MODE_NETWLOOP);
	CHECK(mgcp_mode_parse(sip_str_cstr("netwtest")) == MGCP_MODE_NETWTEST);
	CHECK(mgcp_mode_parse(sip_str_cstr("X/data")) == MGCP_MODE_EXTENSION);
	CHECK(mgcp_mode_parse(sip_str_cstr("data")) == MGCP_MODE_UNKNOWN);
	CHECK(!strcmp(mgcp_mode_name(MGCP_MODE_CONFRNCE), "confrnce"));
	CHECK(parse("MDCX 1 aaln/1@gw MGCP 1.0\r\nM: sendrecv\r\n") ==
	      MGCP_PARSE_OK);
	CHECK(mgcp_msg_mode(&m, &mode) && mode == MGCP_MODE_SENDRECV);

	CASE("D: the digit map as received");
	CHECK(parse("RQNT 1202 aaln/1@gw MGCP 1.0\r\n"
		    "D: (0T|00T|#xxxxxxx|*xx|91xxxxxxxxxx|9011x.T)\r\n") ==
	      MGCP_PARSE_OK);
	CHECK(mgcp_msg_digit_map(&m, &v));
	CHECK_STR(v, "(0T|00T|#xxxxxxx|*xx|91xxxxxxxxxx|9011x.T)");

	CASE("Q: quarantine keywords (3.2.2.14)");
	CHECK(mgcp_quarantine_parse(sip_str_cstr("loop"), &q));
	CHECK(q.loop && !q.discard);
	CHECK(mgcp_quarantine_parse(sip_str_cstr("process"), &q));
	CHECK(!q.loop && !q.discard);
	CHECK(mgcp_quarantine_parse(sip_str_cstr("loop,discard"), &q));
	CHECK(q.loop && q.discard);
	CHECK(mgcp_quarantine_parse(sip_str_cstr("Step, PROCESS"), &q));
	CHECK(!q.loop && !q.discard);
	CHECK(!mgcp_quarantine_parse(sip_str_cstr("loop,step"), &q));
	CHECK(!mgcp_quarantine_parse(sip_str_cstr("hold"), &q));
	CHECK(!mgcp_quarantine_parse(sip_str_cstr(""), &q));

	CASE("K: confirmed ranges (3.2.2.19)");
	mgcp_ack_init(&ai, sip_str_cstr("6234-6255, 6257, 19030-19044"));
	CHECK(mgcp_ack_next(&ai, &lo, &hi) && lo == 6234 && hi == 6255);
	CHECK(mgcp_ack_next(&ai, &lo, &hi) && lo == 6257 && hi == 6257);
	CHECK(mgcp_ack_next(&ai, &lo, &hi) && lo == 19030 && hi == 19044);
	CHECK(!mgcp_ack_next(&ai, &lo, &hi));
	CHECK(!ai.bad);
	CHECK(mgcp_ack_covers(sip_str_cstr("6234-6255, 6257"), 6240));
	CHECK(mgcp_ack_covers(sip_str_cstr("6234-6255, 6257"), 6257));
	CHECK(!mgcp_ack_covers(sip_str_cstr("6234-6255, 6257"), 6256));

	CASE("K: malformed ranges and the empty K:");
	mgcp_ack_init(&ai, sip_str_cstr("5-3"));
	CHECK(!mgcp_ack_next(&ai, &lo, &hi) && ai.bad);
	mgcp_ack_init(&ai, sip_str_cstr("0"));
	CHECK(!mgcp_ack_next(&ai, &lo, &hi) && ai.bad);
	mgcp_ack_init(&ai, sip_str_cstr("12, abc, 14"));
	CHECK(mgcp_ack_next(&ai, &lo, &hi));
	CHECK(!mgcp_ack_next(&ai, &lo, &hi) && ai.bad);
	mgcp_ack_init(&ai, sip_str_cstr(""));
	CHECK(!mgcp_ack_next(&ai, &lo, &hi) && !ai.bad);
	CHECK(parse("200 1206 OK\r\nK:\r\nI: DFE233D1\r\n") == MGCP_PARSE_OK);
	CHECK(mgcp_msg_param(&m, MGCP_PARAM_K) != NULL);
	CHECK_UINT(val(MGCP_PARAM_K).len, 0);

	CASE("F: requested info codes (3.2.2.17)");
	rest = sip_str_cstr("N,X,R,S,D,Q,T, RC, LC, A, ES, ZZ");
	CHECK(mgcp_info_next(&rest, &code, &name) && code == MGCP_PARAM_N);
	CHECK(mgcp_info_next(&rest, &code, &name) && code == MGCP_PARAM_X);
	CHECK(mgcp_info_next(&rest, &code, &name) && code == MGCP_PARAM_R);
	CHECK(mgcp_info_next(&rest, &code, &name) && code == MGCP_PARAM_S);
	CHECK(mgcp_info_next(&rest, &code, &name) && code == MGCP_PARAM_D);
	CHECK(mgcp_info_next(&rest, &code, &name) && code == MGCP_PARAM_Q);
	CHECK(mgcp_info_next(&rest, &code, &name) && code == MGCP_PARAM_T);
	CHECK(mgcp_info_next(&rest, &code, &name) && code == MGCP_PARAM_RC);
	CHECK(mgcp_info_next(&rest, &code, &name) && code == MGCP_PARAM_LC);
	CHECK(mgcp_info_next(&rest, &code, &name) && code == MGCP_PARAM_A);
	CHECK(mgcp_info_next(&rest, &code, &name) && code == MGCP_PARAM_ES);
	CHECK(mgcp_info_next(&rest, &code, &name) &&
	      code == MGCP_PARAM_UNKNOWN);
	CHECK_STR(name, "ZZ");
	CHECK(!mgcp_info_next(&rest, &code, &name));

	CASE("B: bearer encoding (3.2.2.1)");
	CHECK(parse("EPCF 1 aaln/1@gw MGCP 1.0\r\nB: e:mu\r\n") ==
	      MGCP_PARSE_OK);
	CHECK(mgcp_msg_bearer(&m, &b));
	CHECK_STR(b.e, "mu");
	CHECK(parse("EPCF 1 aaln/1@gw MGCP 1.0\r\nB: x-foo:1, E:A\r\n") ==
	      MGCP_PARSE_OK);
	CHECK(mgcp_msg_bearer(&m, &b));
	CHECK_STR(b.e, "A");

	CASE("RM: restart methods (3.2.2.20)");
	CHECK(mgcp_rm_parse(sip_str_cstr("graceful")) == MGCP_RM_GRACEFUL);
	CHECK(mgcp_rm_parse(sip_str_cstr("forced")) == MGCP_RM_FORCED);
	CHECK(mgcp_rm_parse(sip_str_cstr("RESTART")) == MGCP_RM_RESTART);
	CHECK(mgcp_rm_parse(sip_str_cstr("disconnected")) ==
	      MGCP_RM_DISCONNECTED);
	CHECK(mgcp_rm_parse(sip_str_cstr("cancel-graceful")) ==
	      MGCP_RM_CANCEL_GRACEFUL);
	CHECK(mgcp_rm_parse(sip_str_cstr("pkg/keepalive")) ==
	      MGCP_RM_EXTENSION);
	CHECK(mgcp_rm_parse(sip_str_cstr("reboot")) == MGCP_RM_UNKNOWN);
	CHECK(!strcmp(mgcp_rm_name(MGCP_RM_CANCEL_GRACEFUL),
		      "cancel-graceful"));

	CASE("RD: and MD: digit bounds");
	CHECK(parse("RSIP 1 aaln/1@gw MGCP 1.0\r\nRM: graceful\r\nRD: 300\r\n"
		    "MD: 8100\r\n") == MGCP_PARSE_OK);
	CHECK(mgcp_msg_rm(&m, &rm) && rm == MGCP_RM_GRACEFUL);
	CHECK(mgcp_msg_rd(&m, &u) && u == 300);
	CHECK(mgcp_msg_md(&m, &u) && u == 8100);
	CHECK(parse("RSIP 1 aaln/1@gw MGCP 1.0\r\nRD: 1234567\r\n") ==
	      MGCP_PARSE_OK);
	CHECK(!mgcp_msg_rd(&m, &u));
	CHECK(!mgcp_msg_rm(&m, &rm));

	CASE("E: reason codes (3.2.2.15)");
	CHECK(mgcp_reason_parse(sip_str_cstr("900 Endpoint malfunctioning"),
				&r));
	CHECK_UINT(r.code, 900);
	CHECK_STR(r.comment, "Endpoint malfunctioning");
	CHECK(mgcp_reason_parse(sip_str_cstr("900 - Hardware error"), &r));
	CHECK_STR(r.comment, "- Hardware error");
	CHECK(mgcp_reason_parse(sip_str_cstr("801 /XYZ odd"), &r));
	CHECK_STR(r.package, "XYZ");
	CHECK_STR(r.comment, "odd");
	CHECK(mgcp_reason_parse(sip_str_cstr("0"), &r) == false);
	CHECK(!mgcp_reason_parse(sip_str_cstr("9000"), &r));

	CASE("P: connection parameters (3.2.2.7)");
	CHECK(mgcp_conn_params_parse(sip_str_cstr("PS=1245, OS=62345, PR=780, "
						  "OR=45123, PL=10, JI=27,"
						  "LA=48"), &cp));
	CHECK_UINT(cp.ps, 1245);
	CHECK_UINT(cp.os, 62345);
	CHECK_UINT(cp.pr, 780);
	CHECK_UINT(cp.or_, 45123);
	CHECK_UINT(cp.pl, 10);
	CHECK_UINT(cp.ji, 27);
	CHECK_UINT(cp.la, 48);
	CHECK_UINT(cp.present, 0x7f);
	CHECK(mgcp_conn_params_parse(sip_str_cstr("X-FOO=5, PS=1"), &cp));
	CHECK_UINT(cp.present, MGCP_CP_PS);
	CHECK(!mgcp_conn_params_parse(sip_str_cstr("PS=abc"), &cp));
	CHECK(!mgcp_conn_params_parse(sip_str_cstr("PS=1234567890"), &cp));
	CHECK(!mgcp_conn_params_parse(sip_str_cstr("PS"), &cp));
}

static void sdp_tests(void)
{
	struct sip_str rest, one;

	CASE("the session description after the empty line (3.4)");
	CHECK(parse(crcx_long) == MGCP_PARSE_OK);
	CHECK(sip_str_prefix(m.sdp, "v=0\r\no=- 25678"));
	CHECK(m.sdp.p[m.sdp.len - 1] == '\n');
	CHECK_UINT(m.param_count, 7);

	CASE("a response with a local and a remote description (3.3)");
	CHECK(parse("200 1203 OK\r\n"
		    "C: A3C47F21456789F0\r\n"
		    "\r\n"
		    "v=0\r\nc=IN IP4 128.96.41.1\r\nm=audio 1296 RTP/AVP 0\r\n"
		    "\r\n"
		    "v=0\r\nc=IN IP4 128.96.63.25\r\n"
		    "m=audio 1296 RTP/AVP 0 96\r\n") == MGCP_PARSE_OK);
	rest = m.sdp;
	CHECK(mgcp_sdp_next(&rest, &one));
	CHECK_STR(one, "v=0\r\nc=IN IP4 128.96.41.1\r\n"
		  "m=audio 1296 RTP/AVP 0\r\n");
	CHECK(mgcp_sdp_next(&rest, &one));
	CHECK(sip_str_prefix(one, "v=0\r\nc=IN IP4 128.96.63.25"));
	CHECK(!mgcp_sdp_next(&rest, &one));

	CASE("a description that holds only its version line (3.3.7)");
	CHECK(parse("200 1203 OK\r\n\r\nv=0\r\n") == MGCP_PARSE_OK);
	rest = m.sdp;
	CHECK(mgcp_sdp_next(&rest, &one));
	CHECK_STR(one, "v=0\r\n");
	CHECK(!mgcp_sdp_next(&rest, &one));
}

/* --------------------------------------------------------- Appendix F */

static void appendix_f_tests(void)
{
	struct mgcp_req_iter it;
	struct mgcp_req_event re;
	struct mgcp_sig_iter sit;
	struct mgcp_event ev;
	struct mgcp_conn_params cp;
	struct mgcp_reason r;
	struct mgcp_entity e;
	struct sip_str v;
	unsigned int n;

	CASE("F.1 NotificationRequest examples");
	CHECK(parse("RQNT 1201 aaln/1@rgw-2567.whatever.net MGCP 1.0\r\n"
		    "N: ca@ca1.whatever.net:5678\r\n"
		    "X: 0123456789AC\r\n"
		    "R: l/hd(N)\r\n"
		    "S: l/rg\r\n") == MGCP_PARSE_OK);
	CHECK(mgcp_entity_parse(val(MGCP_PARAM_N), &e) && e.port == 5678);
	CHECK(mgcp_req_first(val(MGCP_PARAM_R), pkg_l, &it, &re));
	CHECK_STR(re.ev.name, "hd");
	CHECK(mgcp_sig_first(val(MGCP_PARAM_S), pkg_l, &sit, &ev));
	CHECK_STR(ev.name, "rg");
	CHECK(parse("RQNT 1202 aaln/1@rgw-2567.whatever.net MGCP 1.0\r\n"
		    "N: ca@ca1.whatever.net:5678\r\n"
		    "X: 0123456789AC\r\n"
		    "R: L/hd(A, E(S(L/dl),R(L/oc, L/hu, D/[0-9#*T](D))))\r\n"
		    "D: (0T|00T|#xxxxxxx|*xx|91xxxxxxxxxx|9011x.T)\r\n"
		    "S:\r\n"
		    "Q: process\r\n"
		    "T: G/ft\r\n") == MGCP_PARSE_OK);
	CHECK_UINT(m.param_count, 7);
	CHECK(mgcp_req_first(val(MGCP_PARAM_R), pkg_l, &it, &re));
	CHECK_UINT(re.act, MGCP_ACT_A | MGCP_ACT_E);
	CHECK_STR(val(MGCP_PARAM_S), "");

	CASE("F.2 Notify");
	CHECK(parse("NTFY 2002 aaln/1@rgw-2567.whatever.net MGCP 1.0\r\n"
		    "N: ca@ca1.whatever.net:5678\r\n"
		    "X: 0123456789AC\r\n"
		    "O: L/hd,D/9,D/1,D/2,D/0,D/1,D/8,D/2,D/9,D/4,D/2,D/6,"
		    "D/6\r\n") == MGCP_PARSE_OK);
	n = 0;
	if (mgcp_sig_first(val(MGCP_PARAM_O), pkg_l, &sit, &ev)) {
		do {
			n++;
		} while (mgcp_sig_next(&sit, &ev));
	}
	CHECK_UINT(n, 13);

	CASE("F.3 CreateConnection and its responses");
	CHECK(parse("200 1204 OK\r\n"
		    "I: FDE234C8\r\n"
		    "\r\n"
		    "v=0\r\n"
		    "o=- 25678 753849 IN IP4 128.96.41.1\r\n"
		    "s=-\r\n"
		    "c=IN IP4 128.96.41.1\r\n"
		    "t=0 0\r\n"
		    "m=audio 3456 RTP/AVP 0\r\n") == MGCP_PARSE_OK);
	CHECK(mgcp_msg_conn_id(&m, &v));
	CHECK(sip_str_prefix(m.sdp, "v=0"));
	CHECK(parse("401 1205 Phone off-hook\r\n") == MGCP_PARSE_OK);
	CHECK(parse("100 1206 Pending\r\nI: DFE233D1\r\n\r\nv=0\r\n") ==
	      MGCP_PARSE_OK);
	CHECK(mgcp_rc_class_of(m.code) == MGCP_RC_PROVISIONAL);
	CHECK(parse("200 1206 OK\r\nK:\r\nI: DFE233D1\r\n\r\nv=0\r\n") ==
	      MGCP_PARSE_OK);
	CHECK(mgcp_msg_param(&m, MGCP_PARAM_K) != NULL);
	CHECK(parse("000 1206\r\n") == MGCP_PARSE_OK);
	CHECK(parse(crcx_long) == MGCP_PARSE_OK);
	CHECK(mgcp_ack_covers(val(MGCP_PARAM_K), 1205));

	CASE("F.4 ModifyConnection");
	CHECK(parse("MDCX 1210 aaln/1@rgw-2567.whatever.net MGCP 1.0\r\n"
		    "C: A3C47F21456789F0\r\n"
		    "I: FDE234C8\r\n"
		    "M: recvonly\r\n"
		    "X: 0123456789AE\r\n"
		    "R: L/hu\r\n"
		    "S: G/rt\r\n"
		    "\r\n"
		    "v=0\r\n"
		    "o=- 4723891 7428910 IN IP4 128.96.63.25\r\n"
		    "s=-\r\n"
		    "c=IN IP4 128.96.63.25\r\n"
		    "t=0 0\r\n"
		    "m=audio 3456 RTP/AVP 0\r\n") == MGCP_PARSE_OK);
	CHECK(m.verb == MGCP_VERB_MDCX);
	CHECK(mgcp_sig_first(val(MGCP_PARAM_S), pkg_l, &sit, &ev));
	CHECK_STR(ev.pkg, "G");

	CASE("F.5 to F.7 DeleteConnection in both directions");
	CHECK(parse("250 1210 OK\r\n"
		    "P: PS=1245, OS=62345, PR=780, OR=45123, PL=10, JI=27, "
		    "LA=48\r\n") == MGCP_PARSE_OK);
	CHECK(mgcp_msg_conn_params(&m, &cp));
	CHECK_UINT(cp.la, 48);
	CHECK(parse("DLCX 1210 aaln/1@rgw-2567.whatever.net MGCP 1.0\r\n"
		    "C: A3C47F21456789F0\r\n"
		    "I: FDE234C8\r\n"
		    "E: 900 - Hardware error\r\n"
		    "P: PS=1245, OS=62345, PR=780, OR=45123, PL=10, JI=27, "
		    "LA=48\r\n") == MGCP_PARSE_OK);
	CHECK(mgcp_msg_reason(&m, &r) && r.code == 900);
	CHECK(parse("DLCX 1210 aaln/*@rgw-2567.whatever.net MGCP 1.0\r\n") ==
	      MGCP_PARSE_OK);
	CHECK_STR(m.endpoint, "aaln/*@rgw-2567.whatever.net");

	CASE("F.8 AuditEndpoint and its responses");
	CHECK(parse("AUEP 2002 aaln/1@rgw-2567.whatever.net MGCP 1.0\r\n"
		    "F: R,D,S,X,N,I,T,O,ES\r\n") == MGCP_PARSE_OK);
	CHECK(parse("200 1201 OK\r\n"
		    "A: a:PCMU, p:10-100, e:on, s:off, v:L;S, m:sendonly;"
		    "recvonly;sendrecv;inactive;netwloop;netwtest\r\n"
		    "A: a:G729, p:30-90, e:on, s:on, v:L;S, m:sendonly;"
		    "recvonly;sendrecv;inactive;confrnce;netwloop\r\n") ==
	      MGCP_PARSE_OK);
	CHECK(mgcp_msg_param_next(&m, mgcp_msg_param(&m, MGCP_PARAM_A)) !=
	      NULL);
	CHECK(parse("200 2002 OK\r\n"
		    "R: L/hu,L/oc(N),D/[0-9](N)\r\n"
		    "D:\r\n"
		    "S: L/vmwi(+)\r\n"
		    "X: 0123456789B1\r\n"
		    "N: [128.96.41.12]\r\n"
		    "I: 32F345E2\r\n"
		    "T: G/ft\r\n"
		    "O: L/hd,D/9,D/1,D/2\r\n"
		    "ES: L/hd\r\n") == MGCP_PARSE_OK);
	CHECK_UINT(m.param_count, 9);
	CHECK(mgcp_req_first(val(MGCP_PARAM_R), pkg_l, &it, &re));
	CHECK(mgcp_req_next(&it, &re));
	CHECK(mgcp_req_next(&it, &re));
	CHECK_STR(re.ev.name, "[0-9]");
	CHECK(mgcp_sig_first(val(MGCP_PARAM_ES), pkg_l, &sit, &ev));
	CHECK_STR(ev.name, "hd");

	CASE("F.9 AuditConnection");
	CHECK(parse("AUCX 1203 aaln/2@rgw-2567.whatever.net MGCP 1.0\r\n"
		    "I: FDE234C8\r\n"
		    "F: RC,LC\r\n") == MGCP_PARSE_OK);
	CHECK(parse("200 1203 OK\r\n\r\nv=0\r\no=- 4723891 7428910 IN IP4 "
		    "128.96.63.25\r\ns=-\r\n\r\nv=0\r\n") == MGCP_PARSE_OK);

	CASE("F.10 RestartInProgress and the 521 redirect");
	CHECK(parse("RSIP 1204 *@rgw-2567.whatever.net MGCP 1.0\r\n"
		    "RM: restart\r\n"
		    "RD: 0\r\n") == MGCP_PARSE_OK);
	CHECK_STR(m.endpoint, "*@rgw-2567.whatever.net");
	CHECK(parse("521 1204 OK\r\nN: CA-1@whatever.net\r\n") ==
	      MGCP_PARSE_OK);
	CHECK(mgcp_entity_parse(val(MGCP_PARAM_N), &e));
	CHECK_STR(e.domain, "whatever.net");
}

/* -------------------------------------------------------------- builders */

static char out[2048];

static bool ntfy_fill(struct sip_buf *b, size_t size)
{
	static struct mgcp_ack_list acks = { { 1, 2, 3, 7, 9, 10 }, 6 };
	struct mgcp_ntfy n = {
		.txid = 2002,
		.endpoint = "aaln/1@rgw-2567.whatever.net",
		.version = MGCP_VERSION_RFC3435,
		.acks = &acks,
		.notified_entity = "ca@ca1.whatever.net:5678",
		.request_id = "0123456789AC",
		.observed = "L/hd,D/9,D/1",
	};

	sip_buf_init(b, out, size);

	return mgcp_ntfy_build(b, &n);
}

static bool rsip_fill(struct sip_buf *b, size_t size)
{
	struct mgcp_rsip r = {
		.txid = 1200,
		.endpoint = "aaln/*@rgw-2567.whatever.net",
		.version = MGCP_VERSION_NCS,
		.method = "disconnected",
		.delay = 45,
		.has_delay = true,
		.reason = 900,
		.reason_text = "Endpoint malfunctioning",
	};

	sip_buf_init(b, out, size);

	return mgcp_rsip_build(b, &r);
}

static bool dlcx_fill(struct sip_buf *b, size_t size)
{
	static const struct mgcp_conn_params cp = {
		.ps = 1245, .os = 62345, .pr = 780, .or_ = 45123, .pl = 10,
		.ji = 27, .la = 48, .present = 0x7f,
	};
	struct mgcp_dlcx d = {
		.txid = 1210,
		.endpoint = "aaln/1@rgw-2567.whatever.net",
		.version = MGCP_VERSION_RFC3435,
		.call_id = "A3C47F21456789F0",
		.conn_id = "FDE234C8",
		.reason = 900,
		.reason_text = "- Hardware error",
		.stats = &cp,
	};

	sip_buf_init(b, out, size);

	return mgcp_dlcx_build(b, &d);
}

static bool resp_fill(struct sip_buf *b, size_t size)
{
	static const struct mgcp_param_kv kv[] = {
		{ MGCP_PARAM_I, NULL, "DFE233D1" },
		{ MGCP_PARAM_UNKNOWN, "X-Vendor", "abc" },
	};
	static const char sdp[] = "v=0\r\nc=IN IP4 128.96.63.25\r\n"
				  "m=audio 3456 RTP/AVP 0";
	struct mgcp_resp r = {
		.code = 200,
		.txid = 1206,
		.comment = "OK",
		.ack_request = true,
		.params = kv,
		.param_count = 2,
		.sdp = sdp,
		.sdp_len = sizeof(sdp) - 1,
	};

	sip_buf_init(b, out, size);

	return mgcp_resp_build(b, &r);
}

static bool ack_fill(struct sip_buf *b, size_t size)
{
	struct mgcp_resp r = { .code = 0, .txid = 1206 };

	sip_buf_init(b, out, size);

	return mgcp_resp_build(b, &r);
}

static void builder_tests(void)
{
	static const struct {
		const char *name;
		bool (*fill)(struct sip_buf *b, size_t size);
	} builders[] = {
		{ "ntfy", ntfy_fill },
		{ "rsip", rsip_fill },
		{ "dlcx", dlcx_fill },
		{ "resp", resp_fill },
		{ "ack", ack_fill },
	};
	struct mgcp_conn_params cp;
	struct mgcp_reason r;
	struct mgcp_ack_iter ai;
	struct mgcp_rsip rs;
	struct mgcp_ntfy nt;
	struct mgcp_dlcx dl;
	struct mgcp_resp rp;
	struct sip_buf b;
	struct sip_str rest, one;
	struct mgcp_split it;
	enum mgcp_rm rm;
	uint32_t lo, hi, u;
	size_t full, size;
	unsigned int i, bad;

	CASE("NTFY round trip with K:, N:, X: and O:");
	CHECK(ntfy_fill(&b, sizeof(out)));
	CHECK(parse_len(out, sip_buf_len(&b)) == MGCP_PARSE_OK);
	CHECK(m.verb == MGCP_VERB_NTFY);
	CHECK_UINT(m.txid, 2002);
	CHECK_STR(m.endpoint, "aaln/1@rgw-2567.whatever.net");
	CHECK(mgcp_msg_version_is(&m, MGCP_VERSION_RFC3435));
	CHECK_STR(val(MGCP_PARAM_K), "1-3, 7, 9-10");
	CHECK_STR(val(MGCP_PARAM_N), "ca@ca1.whatever.net:5678");
	CHECK_STR(val(MGCP_PARAM_X), "0123456789AC");
	CHECK_STR(val(MGCP_PARAM_O), "L/hd,D/9,D/1");
	mgcp_ack_init(&ai, val(MGCP_PARAM_K));
	CHECK(mgcp_ack_next(&ai, &lo, &hi) && lo == 1 && hi == 3);
	CHECK(!(m.flags & MGCP_MSG_F_BARE_LF));

	CASE("RSIP round trip with the NCS version token, RD: and E:");
	CHECK(rsip_fill(&b, sizeof(out)));
	CHECK(parse_len(out, sip_buf_len(&b)) == MGCP_PARSE_OK);
	CHECK(m.verb == MGCP_VERB_RSIP);
	CHECK(mgcp_msg_version_is(&m, MGCP_VERSION_NCS));
	CHECK_STR(m.profile, "NCS 1.0");
	CHECK(mgcp_msg_rm(&m, &rm) && rm == MGCP_RM_DISCONNECTED);
	CHECK(mgcp_msg_rd(&m, &u) && u == 45);
	CHECK(mgcp_msg_reason(&m, &r) && r.code == 900);
	CHECK_STR(r.comment, "Endpoint malfunctioning");
	CHECK(mgcp_msg_param(&m, MGCP_PARAM_K) == NULL);

	CASE("RSIP refuses a delay with forced and cancel-graceful (2.3.12)");
	memset(&rs, 0, sizeof(rs));
	rs.txid = 1;
	rs.endpoint = "aaln/1@gw";
	rs.version = MGCP_VERSION_RFC3435;
	rs.method = "forced";
	rs.has_delay = true;
	sip_buf_init(&b, out, sizeof(out));
	CHECK(!mgcp_rsip_build(&b, &rs));
	CHECK_UINT(sip_buf_len(&b), 0);
	rs.method = "cancel-graceful";
	CHECK(!mgcp_rsip_build(&b, &rs));
	rs.method = "forced";
	rs.has_delay = false;
	CHECK(mgcp_rsip_build(&b, &rs));
	CHECK(parse_len(out, sip_buf_len(&b)) == MGCP_PARSE_OK);
	CHECK(mgcp_msg_param(&m, MGCP_PARAM_RD) == NULL);
	rs.method = "restart";
	rs.has_delay = true;
	rs.delay = 1000000;
	sip_buf_init(&b, out, sizeof(out));
	CHECK(!mgcp_rsip_build(&b, &rs));

	CASE("gateway DLCX round trip with E: and P: (F.6)");
	CHECK(dlcx_fill(&b, sizeof(out)));
	CHECK(parse_len(out, sip_buf_len(&b)) == MGCP_PARSE_OK);
	CHECK(m.verb == MGCP_VERB_DLCX);
	CHECK_STR(val(MGCP_PARAM_C), "A3C47F21456789F0");
	CHECK_STR(val(MGCP_PARAM_I), "FDE234C8");
	CHECK_STR(val(MGCP_PARAM_E), "900 - Hardware error");
	CHECK_STR(val(MGCP_PARAM_P), "PS=1245, OS=62345, PR=780, OR=45123, "
		  "PL=10, JI=27, LA=48");
	CHECK(mgcp_msg_conn_params(&m, &cp));
	CHECK_UINT(cp.present, 0x7f);
	CHECK_UINT(cp.os, 62345);

	CASE("P: counters stop at nine digits (3.2.2.7)");
	memset(&cp, 0, sizeof(cp));
	cp.ps = 4000000000u;
	cp.ji = 5;
	cp.present = MGCP_CP_PS | MGCP_CP_JI;
	sip_buf_init(&b, out, sizeof(out));
	CHECK(mgcp_conn_params_write(&b, &cp));
	CHECK(!strcmp(out, "P: PS=999999999, JI=5\r\n"));
	cp.present = 0;
	sip_buf_init(&b, out, sizeof(out));
	CHECK(mgcp_conn_params_write(&b, &cp));
	CHECK_UINT(sip_buf_len(&b), 0);

	CASE("a final response round trip with K:, parameters and SDP");
	CHECK(resp_fill(&b, sizeof(out)));
	CHECK(parse_len(out, sip_buf_len(&b)) == MGCP_PARSE_OK);
	CHECK(!m.is_command);
	CHECK_UINT(m.code, 200);
	CHECK_UINT(m.txid, 1206);
	CHECK_STR(m.comment, "OK");
	CHECK(mgcp_msg_param(&m, MGCP_PARAM_K) != NULL);
	CHECK_STR(val(MGCP_PARAM_K), "");
	CHECK_STR(val(MGCP_PARAM_I), "DFE233D1");
	CHECK(mgcp_msg_param_named(&m, "X-Vendor") != NULL);
	CHECK_STR(m.sdp, "v=0\r\nc=IN IP4 128.96.63.25\r\n"
		  "m=audio 3456 RTP/AVP 0\r\n");

	CASE("the response acknowledgment round trip (3.3.1)");
	CHECK(ack_fill(&b, sizeof(out)));
	CHECK(!strcmp(out, "000 1206\r\n"));
	CHECK(parse_len(out, sip_buf_len(&b)) == MGCP_PARSE_OK);
	CHECK_UINT(m.code, 0);
	CHECK_UINT(m.comment.len, 0);

	CASE("a short buffer is refused by every builder, never truncated");
	bad = 0;
	for (i = 0; i < sizeof(builders) / sizeof(builders[0]); i++) {
		CHECK(builders[i].fill(&b, sizeof(out)));
		full = sip_buf_len(&b);
		for (size = 0; size <= full; size++) {
			if (builders[i].fill(&b, size)) {
				printf("    %s built into %zu of %zu bytes\n",
				       builders[i].name, size, full);
				bad++;
			}
			if (sip_buf_len(&b) != 0)
				bad++;
		}
		if (!builders[i].fill(&b, full + 1) ||
		    sip_buf_len(&b) != full)
			bad++;
	}
	CHECK_UINT(bad, 0);

	CASE("a builder keeps what the buffer held before it");
	sip_buf_init(&b, out, 40);
	CHECK(sip_buf_add(&b, "PREFIX"));
	{
		struct mgcp_resp big = { .code = 200, .txid = 1,
					 .comment = "a comment far too long "
						    "for this small buffer" };

		CHECK(!mgcp_resp_build(&b, &big));
		CHECK_UINT(sip_buf_len(&b), 6);
		CHECK(!strcmp(out, "PREFIX"));
		CHECK(sip_buf_ok(&b));
	}

	CASE("a refused build clears the overflow, so a shorter write follows");
	CHECK(!ntfy_fill(&b, 40));
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(sip_buf_ok(&b));
	CHECK(mgcp_sep_write(&b));
	CHECK(!strcmp(out, ".\r\n"));
	sip_buf_init(&b, out, 3);
	CHECK(!mgcp_sep_write(&b));
	CHECK(sip_buf_ok(&b));
	sip_buf_init(&b, out, 4);
	b.overflow = true;
	CHECK(!mgcp_sep_write(&b));
	CHECK(!sip_buf_ok(&b));

	CASE("a response names each parameter once (3.3)");
	{
		static const struct mgcp_param_kv twice[] = {
			{ MGCP_PARAM_I, NULL, "1" },
			{ MGCP_PARAM_I, NULL, "2" },
		};
		static const struct mgcp_param_kv named[] = {
			{ MGCP_PARAM_UNKNOWN, "X-A", "1" },
			{ MGCP_PARAM_UNKNOWN, "x-a", "2" },
		};
		static const struct mgcp_param_kv ack[] = {
			{ MGCP_PARAM_K, NULL, "" },
		};
		struct mgcp_resp r = { .code = 200, .txid = 5, .params = twice,
				       .param_count = 2 };

		sip_buf_init(&b, out, sizeof(out));
		CHECK(!mgcp_resp_build(&b, &r));
		CHECK_UINT(sip_buf_len(&b), 0);
		r.params = named;
		CHECK(!mgcp_resp_build(&b, &r));
		r.params = ack;
		r.param_count = 1;
		r.ack_request = true;
		CHECK(!mgcp_resp_build(&b, &r));
		r.ack_request = false;
		CHECK(mgcp_resp_build(&b, &r));
		CHECK(!strcmp(out, "200 5\r\nK:\r\n"));
	}

	CASE("a response comment admits %x20-7E alone (Appendix A)");
	memset(&rp, 0, sizeof(rp));
	rp.code = 200;
	rp.txid = 5;
	rp.comment = "tab\there";
	sip_buf_init(&b, out, sizeof(out));
	CHECK(!mgcp_resp_build(&b, &rp));
	rp.comment = "byte \x80";
	CHECK(!mgcp_resp_build(&b, &rp));
	rp.comment = "OK ~ fine";
	CHECK(mgcp_resp_build(&b, &rp));

	CASE("builders refuse values that would split a line");
	memset(&nt, 0, sizeof(nt));
	nt.txid = 1;
	nt.endpoint = "aaln/1@gw";
	nt.version = MGCP_VERSION_RFC3435;
	nt.request_id = "12";
	nt.observed = "L/hd\r\nX: 9";
	sip_buf_init(&b, out, sizeof(out));
	CHECK(!mgcp_ntfy_build(&b, &nt));
	nt.observed = "L/hd";
	nt.endpoint = "aaln/1 @gw";
	CHECK(!mgcp_ntfy_build(&b, &nt));
	nt.endpoint = "aaln/1@gw";
	nt.request_id = "not-hex";
	CHECK(!mgcp_ntfy_build(&b, &nt));
	nt.request_id = NULL;
	CHECK(!mgcp_ntfy_build(&b, &nt));
	nt.request_id = "0";
	nt.txid = 0;
	CHECK(!mgcp_ntfy_build(&b, &nt));
	nt.txid = MGCP_TXID_MAX + 1;
	CHECK(!mgcp_ntfy_build(&b, &nt));
	nt.txid = MGCP_TXID_MAX;
	CHECK(mgcp_ntfy_build(&b, &nt));
	memset(&rp, 0, sizeof(rp));
	rp.code = 200;
	rp.txid = 5;
	rp.comment = "bad\ncomment";
	sip_buf_init(&b, out, sizeof(out));
	CHECK(!mgcp_resp_build(&b, &rp));
	rp.comment = NULL;
	rp.code = 1000;
	CHECK(!mgcp_resp_build(&b, &rp));
	rp.code = 100;
	rp.ack_request = true;
	CHECK(!mgcp_resp_build(&b, &rp));
	memset(&dl, 0, sizeof(dl));
	dl.txid = 3;
	dl.endpoint = "aaln/1@gw";
	dl.version = MGCP_VERSION_RFC3435;
	dl.call_id = "A3C4";
	CHECK(!mgcp_dlcx_build(&b, &dl));
	dl.conn_id = "FD";
	CHECK(mgcp_dlcx_build(&b, &dl));
	CHECK_UINT(sip_buf_len(&b) > 0, 1);

	CASE("a piggybacked datagram built from the builders splits back");
	{
		char dgram[1024];
		struct sip_buf d;
		struct mgcp_resp r = { .code = 200, .txid = 77,
				       .comment = "OK" };
		unsigned int n = 0;

		sip_buf_init(&d, dgram, sizeof(dgram));
		CHECK(mgcp_resp_build(&d, &r));
		CHECK(mgcp_sep_write(&d));
		memset(&rs, 0, sizeof(rs));
		rs.txid = 78;
		rs.endpoint = "aaln/1@gw";
		rs.version = MGCP_VERSION_RFC3435;
		rs.method = "disconnected";
		CHECK(mgcp_rsip_build(&d, &rs));
		mgcp_split_init(&it, dgram, sip_buf_len(&d));
		while (mgcp_split_next(&it, &one)) {
			CHECK(parse_len(one.p, one.len) == MGCP_PARSE_OK);
			CHECK_UINT(m.txid, 77 + n);
			n++;
		}
		CHECK_UINT(n, 2);
		rest = sip_str_init(dgram, sip_buf_len(&d));
		CHECK(sip_str_find_ci(rest, "\r\n.\r\nRSIP 78", NULL));
	}

	CASE("an empty K: list writes no line");
	{
		struct mgcp_ack_list none = { { 0 }, 0 };

		sip_buf_init(&b, out, sizeof(out));
		CHECK(mgcp_ack_list_write(&b, &none));
		CHECK(mgcp_ack_list_write(&b, NULL));
		CHECK_UINT(sip_buf_len(&b), 0);
		none.count = 1;
		CHECK(!mgcp_ack_list_write(&b, &none));
	}
}

int main(void)
{
	command_line_tests();
	txid_tests();
	response_line_tests();
	line_tests();
	prefix_loop_tests();
	name_tests();
	split_tests();
	requested_event_tests();
	signal_tests();
	lco_tests();
	typed_tests();
	sdp_tests();
	appendix_f_tests();
	builder_tests();
	free(pbuf);

	return tu_summary("test_mgcp_msg");
}
