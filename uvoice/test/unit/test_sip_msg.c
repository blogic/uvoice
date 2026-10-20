// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the SIP message parser (src/proto/shared/msg.c).
 *
 * The torture cases follow RFC 4475 3.1.1 (valid but hard). They cover
 * whitespace before the colon, folded values, escaped characters in quoted
 * strings and unusual but legal parameter lists. The tolerance cases follow
 * malformed messages that deployed peers send.
 */
#include <stdlib.h>

#include "proto/shared/hdr.h"
#include "test_util.h"

static struct sip_msg m;
static char buf[SIP_MSG_LEN_MAX + 8];

static enum sip_parse_status parse(const char *text)
{
	size_t len = strlen(text);

	memcpy(buf, text, len);

	return msg_parse(&m, buf, len);
}

/*
 * RFC 4475 3.1.1.1, the "wsinv" message, byte for byte. The SDP body is
 * exactly the 150 octets the Content-Length announces.
 */
static const char wsinv[] =
	"INVITE sip:vivekg@chair-dnrc.example.com;unknownparam SIP/2.0\r\n"
	"TO :\r\n"
	" sip:vivekg@chair-dnrc.example.com ;   tag    = 1918181833n\r\n"
	"from   : \"J Rosenberg \\\\\\\"\"       <sip:jdrosen@example.com>\r\n"
	"  ;\r\n"
	"  tag = 98asjd8\r\n"
	"MaX-fOrWaRdS: 0068\r\n"
	"Call-ID: wsinv.ndaksdj@192.0.2.1\r\n"
	"Content-Length   : 150\r\n"
	"cseq: 0009\r\n"
	"  INVITE\r\n"
	"Via  : SIP  /   2.0\r\n"
	" /UDP\r\n"
	"    192.0.2.2;branch=390skdjuw\r\n"
	"s :\r\n"
	"NewFangledHeader:   newfangled value\r\n"
	" continued newfangled value\r\n"
	"UnknownHeaderWithUnusualValue: ;;,,;;,;\r\n"
	"Content-Type: application/sdp\r\n"
	"Route:\r\n"
	" <sip:services.example.com;lr;unknownwith=value;unknown-no-value>\r\n"
	"v:  SIP  / 2.0  / TCP     spindle.example.com   ;\r\n"
	"  branch  =   z9hG4bK9ikj8  ,\r\n"
	" SIP  /    2.0   / UDP  192.168.255.111   ; branch=\r\n"
	" z9hG4bK30239\r\n"
	"m:\"Quoted string \\\"\\\"\" <sip:jdrosen@example.com> ; newparam =\r\n"
	"      newvalue ;\r\n"
	"  secondparam ; q = 0.33\r\n"
	"\r\n"
	"v=0\r\n"
	"o=mhandley 29739 7272939 IN IP4 192.0.2.1\r\n"
	"s=-\r\n"
	"c=IN IP4 192.0.2.1\r\n"
	"t=0 0\r\n"
	"m=audio 49217 RTP/AVP 0 12\r\n"
	"m=video 3227 RTP/AVP 31\r\n"
	"a=rtpmap:31 LPC\r\n";

static const char reg200[] =
	"SIP/2.0 200 OK\r\n"
	"Via: SIP/2.0/UDP 10.0.0.5:5060;branch=z9hG4bK776;received=203.0.113.9;rport=6001\r\n"
	"From: <sip:5551000@voip.example.net>;tag=abc\r\n"
	"To: <sip:5551000@voip.example.net>;tag=srv-1\r\n"
	"Call-ID: 1-reg@10.0.0.5\r\n"
	"CSeq: 4 REGISTER\r\n"
	"Contact: <sip:5551000@10.0.0.5:5060>;expires=180\r\n"
	"Content-Length: 0\r\n"
	"\r\n";

static void start_line_tests(void)
{
	CASE("request line, known method");
	CHECK(parse("INVITE sip:a@b.com SIP/2.0\r\nCall-ID: x\r\n\r\n") == SIP_PARSE_OK);
	CHECK(m.is_request);
	CHECK(m.method == SIP_METHOD_INVITE);
	CHECK_STR(m.ruri, "sip:a@b.com");
	CHECK_STR(m.version, "SIP/2.0");
	CHECK(!(m.flags & SIP_MSG_F_BAD_VERSION));

	CASE("request line, unknown method keeps its name");
	CHECK(parse("PUBLISH sip:a@b SIP/2.0\r\nCall-ID: x\r\n\r\n") == SIP_PARSE_OK);
	CHECK(m.method == SIP_METHOD_UNKNOWN);
	CHECK_STR(m.method_name, "PUBLISH");

	CASE("method matching is case sensitive (RFC 3261 7.1)");
	CHECK(parse("Invite sip:a@b SIP/2.0\r\nCall-ID: x\r\n\r\n") == SIP_PARSE_OK);
	CHECK(m.method == SIP_METHOD_UNKNOWN);
	CHECK_STR(m.method_name, "Invite");

	CASE("every handled method maps");
	CHECK(sip_method_parse(sip_str_cstr("ACK")) == SIP_METHOD_ACK);
	CHECK(sip_method_parse(sip_str_cstr("CANCEL")) == SIP_METHOD_CANCEL);
	CHECK(sip_method_parse(sip_str_cstr("BYE")) == SIP_METHOD_BYE);
	CHECK(sip_method_parse(sip_str_cstr("REGISTER")) == SIP_METHOD_REGISTER);
	CHECK(sip_method_parse(sip_str_cstr("OPTIONS")) == SIP_METHOD_OPTIONS);
	CHECK(sip_method_parse(sip_str_cstr("INFO")) == SIP_METHOD_INFO);
	CHECK(sip_method_parse(sip_str_cstr("NOTIFY")) == SIP_METHOD_NOTIFY);
	CHECK(sip_method_parse(sip_str_cstr("SUBSCRIBE")) == SIP_METHOD_SUBSCRIBE);
	CHECK(sip_method_parse(sip_str_cstr("UPDATE")) == SIP_METHOD_UPDATE);
	CHECK(sip_method_parse(sip_str_cstr("REFER")) == SIP_METHOD_REFER);
	CHECK(sip_method_parse(sip_str_cstr("PRACK")) == SIP_METHOD_PRACK);
	CHECK(sip_method_parse(sip_str_cstr("MESSAGE")) == SIP_METHOD_MESSAGE);

	CASE("status line");
	CHECK(parse(reg200) == SIP_PARSE_OK);
	CHECK(!m.is_request);
	CHECK_UINT(m.status, 200);
	CHECK_STR(m.reason, "OK");

	CASE("status line with an empty reason phrase");
	CHECK(parse("SIP/2.0 480 \r\nCall-ID: x\r\n\r\n") == SIP_PARSE_OK);
	CHECK_UINT(m.status, 480);
	CHECK_UINT(m.reason.len, 0);

	CASE("status line, 6xx and a multi word reason");
	CHECK(parse("SIP/2.0 604 Does Not Exist Anywhere\r\nCall-ID: x\r\n\r\n") ==
	      SIP_PARSE_OK);
	CHECK_UINT(m.status, 604);
	CHECK_STR(m.reason, "Does Not Exist Anywhere");

	CASE("status code must be three digits");
	CHECK(parse("SIP/2.0 20 OK\r\nCall-ID: x\r\n\r\n") == SIP_PARSE_ERR_STATUS);
	CHECK(parse("SIP/2.0 0200 OK\r\nCall-ID: x\r\n\r\n") == SIP_PARSE_ERR_STATUS);
	CHECK(parse("SIP/2.0 700 OK\r\nCall-ID: x\r\n\r\n") == SIP_PARSE_ERR_STATUS);

	CASE("unknown SIP version parses and flags for a 505");
	CHECK(parse("INVITE sip:a@b SIP/7.0\r\nCall-ID: x\r\n\r\n") == SIP_PARSE_OK);
	CHECK(m.flags & SIP_MSG_F_BAD_VERSION);
	CHECK_STR(m.version, "SIP/7.0");

	CASE("start line rejects");
	CHECK(parse("GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n") ==
	      SIP_PARSE_ERR_START_LINE);
	CHECK(parse("INVITE sip:a@b\r\nCall-ID: x\r\n\r\n") == SIP_PARSE_ERR_START_LINE);
	CHECK(parse("INVITE sip:a b SIP/2.0\r\nCall-ID: x\r\n\r\n") ==
	      SIP_PARSE_ERR_START_LINE);
	CHECK(parse("INVITE sip:a@b SIP/2.0") == SIP_PARSE_ERR_START_LINE);

	CASE("empty datagram and CRLF keepalive");
	CHECK(parse("") == SIP_PARSE_ERR_EMPTY);
	CHECK(parse("\r\n\r\n") == SIP_PARSE_ERR_EMPTY);

	CASE("leading CRLF before a real message is skipped");
	CHECK(parse("\r\n\r\nOPTIONS sip:a@b SIP/2.0\r\nCall-ID: x\r\n\r\n") ==
	      SIP_PARSE_OK);
	CHECK(m.flags & SIP_MSG_F_LEAD_CRLF);
	CHECK(m.method == SIP_METHOD_OPTIONS);

	CASE("message longer than the 64 KB contract is rejected");
	memset(buf, 'A', SIP_MSG_LEN_MAX + 1);
	CHECK(msg_parse(&m, buf, SIP_MSG_LEN_MAX + 1) == SIP_PARSE_ERR_TOO_LARGE);
}

static void header_tests(void)
{
	struct sip_str v;
	const struct sip_hdr *h;
	unsigned int n;

	CASE("bare LF line endings are accepted and flagged");
	CHECK(parse("BYE sip:a@b SIP/2.0\nCall-ID: bare@lf\nCSeq: 2 BYE\n\n") ==
	      SIP_PARSE_OK);
	CHECK(m.flags & SIP_MSG_F_BARE_LF);
	CHECK(msg_hdr_value(&m, SIP_HDR_CALL_ID, &v));
	CHECK_STR(v, "bare@lf");

	CASE("mixed CRLF and bare LF");
	CHECK(parse("BYE sip:a@b SIP/2.0\r\nCall-ID: mix\nCSeq: 2 BYE\r\n\r\n") ==
	      SIP_PARSE_OK);
	CHECK_UINT(m.hdr_count, 2);

	CASE("header names are case insensitive");
	CHECK(parse("OPTIONS sip:a@b SIP/2.0\r\ncAlL-iD: ci\r\nCONTENT-length: 0\r\n\r\n") ==
	      SIP_PARSE_OK);
	CHECK(msg_hdr_value(&m, SIP_HDR_CALL_ID, &v));
	CHECK_STR(v, "ci");
	CHECK(m.clen_present);

	CASE("compact forms map to the long ids");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "v: SIP/2.0/UDP 10.0.0.1;branch=z9hG4bK1\r\n"
		    "f: <sip:a@b>;tag=1\r\n"
		    "t: <sip:c@d>\r\n"
		    "i: compact@call\r\n"
		    "m: <sip:a@10.0.0.1>\r\n"
		    "e: gzip\r\n"
		    "l: 0\r\n"
		    "c: application/sdp\r\n"
		    "s: hi\r\n"
		    "k: 100rel\r\n"
		    "o: message-summary\r\n"
		    "u: refer\r\n"
		    "r: <sip:x@y>\r\n"
		    "b: <sip:z@y>\r\n"
		    "x: 1800\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(msg_hdr_first(&m, SIP_HDR_VIA) != NULL);
	CHECK(msg_hdr_first(&m, SIP_HDR_FROM) != NULL);
	CHECK(msg_hdr_first(&m, SIP_HDR_TO) != NULL);
	CHECK(msg_hdr_first(&m, SIP_HDR_CALL_ID) != NULL);
	CHECK(msg_hdr_first(&m, SIP_HDR_CONTACT) != NULL);
	CHECK(msg_hdr_first(&m, SIP_HDR_CONTENT_ENCODING) != NULL);
	CHECK(msg_hdr_first(&m, SIP_HDR_CONTENT_LENGTH) != NULL);
	CHECK(msg_hdr_first(&m, SIP_HDR_CONTENT_TYPE) != NULL);
	CHECK(msg_hdr_first(&m, SIP_HDR_SUBJECT) != NULL);
	CHECK(msg_hdr_first(&m, SIP_HDR_SUPPORTED) != NULL);
	CHECK(msg_hdr_first(&m, SIP_HDR_EVENT) != NULL);
	CHECK(msg_hdr_first(&m, SIP_HDR_ALLOW_EVENTS) != NULL);
	CHECK(msg_hdr_first(&m, SIP_HDR_REFER_TO) != NULL);
	CHECK(msg_hdr_first(&m, SIP_HDR_REFERRED_BY) != NULL);
	CHECK(msg_hdr_first(&m, SIP_HDR_SESSION_EXPIRES) != NULL);

	CASE("compact forms are case insensitive too");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nI: upper@compact\r\n\r\n") == SIP_PARSE_OK);
	CHECK(msg_hdr_value(&m, SIP_HDR_CALL_ID, &v));
	CHECK_STR(v, "upper@compact");

	CASE("Service-Route maps to its own id and keeps the Subject compact "
	     "form");
	CHECK(parse("SIP/2.0 200 OK\r\n"
		    "Route: <sip:p0.example.com;lr>\r\n"
		    "Service-Route: <sip:p1.example.com;lr>\r\n"
		    "s: hi\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK_UINT(msg_hdr_count(&m, SIP_HDR_SERVICE_ROUTE), 1);
	CHECK(msg_hdr_value(&m, SIP_HDR_SERVICE_ROUTE, &v));
	CHECK_STR(v, "<sip:p1.example.com;lr>");
	CHECK_UINT(msg_hdr_count(&m, SIP_HDR_ROUTE), 1);
	CHECK(msg_hdr_value(&m, SIP_HDR_ROUTE, &v));
	CHECK_STR(v, "<sip:p0.example.com;lr>");
	CHECK(msg_hdr_value(&m, SIP_HDR_SUBJECT, &v));
	CHECK_STR(v, "hi");
	CHECK(!strcmp(sip_hdr_name(SIP_HDR_SERVICE_ROUTE), "Service-Route"));

	CASE("whitespace between the name and the colon");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nContent-Length   : 0\r\nTo :<sip:c@d>\r\n\r\n") ==
	      SIP_PARSE_OK);
	CHECK(m.clen_present);
	CHECK(msg_hdr_value(&m, SIP_HDR_TO, &v));
	CHECK_STR(v, "<sip:c@d>");

	CASE("folding is undone in place");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nSubject: one\r\n two\r\n\tthree\r\n\r\n") ==
	      SIP_PARSE_OK);
	CHECK(m.flags & SIP_MSG_F_FOLDED);
	CHECK(msg_hdr_value(&m, SIP_HDR_SUBJECT, &v));
	CHECK_STR(v, "one   two  \tthree");

	CASE("a header without a colon is flagged, not fatal");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nBrokenHeaderLine\r\nCall-ID: ok\r\n\r\n") ==
	      SIP_PARSE_OK);
	CHECK(m.flags & SIP_MSG_F_HDR_MALFORMED);
	CHECK(m.hdr[0].flags & SIP_HDR_F_MALFORMED);
	CHECK(msg_hdr_value(&m, SIP_HDR_CALL_ID, &v));
	CHECK_STR(v, "ok");

	CASE("empty header value");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\ns :\r\nCall-ID: x\r\n\r\n") == SIP_PARSE_OK);
	CHECK(msg_hdr_value(&m, SIP_HDR_SUBJECT, &v));
	CHECK_UINT(v.len, 0);

	CASE("the fold flag lands on the folded header, not on its neighbours");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Subject: one\r\n two\r\n"
		    "Call-ID: plain\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(m.flags & SIP_MSG_F_FOLDED);
	CHECK_UINT(m.hdr_count, 2);
	CHECK(m.hdr[0].id == SIP_HDR_SUBJECT);
	CHECK(m.hdr[0].flags & SIP_HDR_F_FOLDED);
	CHECK(m.hdr[1].id == SIP_HDR_CALL_ID);
	CHECK(!(m.hdr[1].flags & SIP_HDR_F_FOLDED));

	CASE("a message without folding sets neither fold flag");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nSubject: one\r\nCall-ID: p\r\n\r\n") ==
	      SIP_PARSE_OK);
	CHECK(!(m.flags & SIP_MSG_F_FOLDED));
	CHECK(!(m.hdr[0].flags & SIP_HDR_F_FOLDED));

	CASE("an unusual unknown value does not disturb the index");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "UnknownHeaderWithUnusualValue: ;;,,;;,;\r\n"
		    "Call-ID: x\r\n\r\n") == SIP_PARSE_OK);
	h = msg_hdr_name_first(&m, "unknownheaderwithunusualvalue");
	CHECK(h != NULL);
	if (h)
		CHECK_STR(h->value, ";;,,;;,;");
	CHECK(msg_hdr_value(&m, SIP_HDR_CALL_ID, &v));

	CASE("repeated headers iterate in receive order");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Via: SIP/2.0/UDP one;branch=z9hG4bK1\r\n"
		    "Via: SIP/2.0/UDP two;branch=z9hG4bK2\r\n"
		    "Via: SIP/2.0/UDP three;branch=z9hG4bK3\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK_UINT(msg_hdr_count(&m, SIP_HDR_VIA), 3);
	n = 0;
	for (h = msg_hdr_first(&m, SIP_HDR_VIA); h; h = msg_hdr_next(&m, h))
		n++;
	CHECK_UINT(n, 3);
	h = msg_hdr_first(&m, SIP_HDR_VIA);
	CHECK_STR(h->value, "SIP/2.0/UDP one;branch=z9hG4bK1");

	CASE("repeated unknown headers iterate by name");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nX-Odd: 1\r\nOther: z\r\nx-odd: 2\r\n\r\n") ==
	      SIP_PARSE_OK);
	h = msg_hdr_name_first(&m, "X-Odd");
	CHECK(h != NULL);
	h = msg_hdr_next(&m, h);
	CHECK(h != NULL);
	if (h)
		CHECK_STR(h->value, "2");

	CASE("64 headers fit, 65 is a parse error");
	{
		size_t used = 0;
		unsigned int i;

		used += (size_t)snprintf(buf + used, sizeof(buf) - used,
					 "OPTIONS sip:a@b SIP/2.0\r\n");
		for (i = 0; i < SIP_HDR_COUNT_MAX; i++)
			used += (size_t)snprintf(buf + used, sizeof(buf) - used,
						 "X-Pad-%u: v\r\n", i);
		used += (size_t)snprintf(buf + used, sizeof(buf) - used, "\r\n");
		CHECK(msg_parse(&m, buf, used) == SIP_PARSE_OK);
		CHECK_UINT(m.hdr_count, SIP_HDR_COUNT_MAX);

		used -= 2;
		used += (size_t)snprintf(buf + used, sizeof(buf) - used,
					 "X-Pad-65: v\r\n\r\n");
		CHECK(msg_parse(&m, buf, used) == SIP_PARSE_ERR_HDR_OVERFLOW);
	}

	CASE("an oversized header is flagged and passed through unharmed");
	{
		size_t used = 0, want;
		unsigned int i;

		used += (size_t)snprintf(buf + used, sizeof(buf) - used,
					 "INVITE sip:a@b SIP/2.0\r\nIdentity: ");
		for (i = 0; i < 5000; i++)
			buf[used++] = 'Z';
		want = 5000;
		used += (size_t)snprintf(buf + used, sizeof(buf) - used,
					 "\r\nCall-ID: big\r\n\r\n");
		CHECK(msg_parse(&m, buf, used) == SIP_PARSE_OK);
		CHECK(m.flags & SIP_MSG_F_HDR_OVERSIZE);
		CHECK(m.hdr[0].flags & SIP_HDR_F_OVERSIZE);
		CHECK_UINT(m.hdr[0].value.len, want);
		CHECK(msg_hdr_value(&m, SIP_HDR_CALL_ID, &v));
		CHECK_STR(v, "big");
	}

	CASE("headers running to the buffer end");
	CHECK(parse("OPTIONS sip:a@b SIP/2.0\r\nCall-ID: noend\r\n") == SIP_PARSE_OK);
	CHECK(m.flags & SIP_MSG_F_NO_BODY_SEP);
	CHECK_UINT(m.hdr_count, 1);

	CASE("a truncated last header is rejected");
	CHECK(parse("OPTIONS sip:a@b SIP/2.0\r\nCall-ID: cut\r\nCSeq: 1 OPTIO") ==
	      SIP_PARSE_ERR_HDR_TRUNCATED);
}

static void body_tests(void)
{
	struct sip_str body;

	CASE("Content-Length bounds the body");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nContent-Length: 3\r\n\r\nabcdef") ==
	      SIP_PARSE_OK);
	CHECK(msg_body_get(&m, &body));
	CHECK_STR(body, "abc");
	CHECK(m.flags & SIP_MSG_F_BODY_EXTRA);

	CASE("Content-Length beyond the datagram trusts the buffer");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nContent-Length: 500\r\n\r\nshort") ==
	      SIP_PARSE_OK);
	CHECK(msg_body_get(&m, &body));
	CHECK_STR(body, "short");
	CHECK(m.flags & SIP_MSG_F_BODY_SHORT);
	CHECK_UINT(m.clen, 500);

	CASE("missing Content-Length means rest of datagram");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nCall-ID: x\r\n\r\nv=0\r\n") == SIP_PARSE_OK);
	CHECK(m.flags & SIP_MSG_F_NO_CLEN);
	CHECK(!m.clen_present);
	CHECK(msg_body_get(&m, &body));
	CHECK_STR(body, "v=0\r\n");

	CASE("Content-Length 0 with trailing bytes");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nContent-Length: 0\r\n\r\njunk") ==
	      SIP_PARSE_OK);
	CHECK(!msg_body_get(&m, &body));
	CHECK_UINT(m.body.len, 0);

	CASE("Content-Length that is not a number");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nContent-Length: abc\r\n\r\nbody") ==
	      SIP_PARSE_OK);
	CHECK(m.flags & SIP_MSG_F_BAD_CLEN);
	CHECK(msg_body_get(&m, &body));
	CHECK_STR(body, "body");

	CASE("body pointer lands inside the caller buffer");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nContent-Length: 4\r\n\r\nbody") ==
	      SIP_PARSE_OK);
	CHECK(m.body.p == buf + strlen("INVITE sip:a@b SIP/2.0\r\nContent-Length: 4\r\n\r\n"));

	CASE("plain application/sdp body");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Content-Type: application/SDP\r\n"
		    "\r\nv=0\r\ns=-\r\n") == SIP_PARSE_OK);
	CHECK(msg_body_sdp_get(&m, &body));
	CHECK_STR(body, "v=0\r\ns=-\r\n");

	CASE("a body with no Content-Type is taken as SDP when it looks like SDP");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nCall-ID: x\r\n\r\nv=0\r\ns=-\r\n") ==
	      SIP_PARSE_OK);
	CHECK(msg_body_sdp_get(&m, &body));
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nCall-ID: x\r\n\r\nnot sdp\r\n") ==
	      SIP_PARSE_OK);
	CHECK(!msg_body_sdp_get(&m, &body));
}

static void multipart_tests(void)
{
	struct sip_part_iter it;
	struct sip_part part;
	struct sip_str body, bnd;
	unsigned int n;

	CASE("SDP is found in the second part");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Content-Type: multipart/mixed;boundary=\"frontier\"\r\n"
		    "\r\n"
		    "--frontier\r\n"
		    "Content-Type: application/isup;version=itu-t92+\r\n"
		    "\r\n"
		    "ISUPBYTES\r\n"
		    "--frontier\r\n"
		    "Content-Type: application/sdp\r\n"
		    "\r\n"
		    "v=0\r\n"
		    "s=-\r\n"
		    "--frontier--\r\n") == SIP_PARSE_OK);
	CHECK(msg_body_sdp_get(&m, &body));
	CHECK_STR(body, "v=0\r\ns=-");

	CASE("part iteration reports both parts and their types");
	n = 0;
	if (msg_part_first(&m, &it, &part)) {
		do {
			if (!n)
				CHECK(sip_str_prefix_ci(part.ctype, "application/isup"));
			else
				CHECK(sip_str_eq_ci(part.ctype, "application/sdp"));
			n++;
		} while (part_next(&it, &part));
	}
	CHECK_UINT(n, 2);

	CASE("boundary parameter, quoted and unquoted");
	CHECK(sip_ctype_boundary(sip_str_cstr("multipart/mixed;boundary=\"a b\""), &bnd));
	CHECK_STR(bnd, "a b");
	CHECK(sip_ctype_boundary(sip_str_cstr("multipart/mixed; boundary=simple"), &bnd));
	CHECK_STR(bnd, "simple");
	CHECK(!sip_ctype_boundary(sip_str_cstr("multipart/mixed"), &bnd));

	CASE("nested multipart is walked one level down");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Content-Type: multipart/mixed;boundary=outer\r\n"
		    "\r\n"
		    "--outer\r\n"
		    "Content-Type: text/plain\r\n"
		    "\r\n"
		    "hello\r\n"
		    "--outer\r\n"
		    "Content-Type: multipart/alternative;boundary=inner\r\n"
		    "\r\n"
		    "--inner\r\n"
		    "Content-Type: application/sdp\r\n"
		    "\r\n"
		    "v=0\r\n"
		    "--inner--\r\n"
		    "--outer--\r\n") == SIP_PARSE_OK);
	CHECK(msg_body_sdp_get(&m, &body));
	CHECK_STR(body, "v=0");

	CASE("multipart without an SDP part");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Content-Type: multipart/mixed;boundary=x\r\n"
		    "\r\n"
		    "--x\r\n"
		    "Content-Type: text/plain\r\n"
		    "\r\n"
		    "nothing here\r\n"
		    "--x--\r\n") == SIP_PARSE_OK);
	CHECK(!msg_body_sdp_get(&m, &body));

	CASE("preamble and epilogue are ignored");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Content-Type: multipart/mixed;boundary=x\r\n"
		    "\r\n"
		    "this is a preamble\r\n"
		    "--x\r\n"
		    "Content-Type: application/sdp\r\n"
		    "\r\n"
		    "v=0\r\n"
		    "--x--\r\n"
		    "trailing epilogue\r\n") == SIP_PARSE_OK);
	CHECK(msg_body_sdp_get(&m, &body));
	CHECK_STR(body, "v=0");

	CASE("a part without a blank line still yields its body");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Content-Type: multipart/mixed;boundary=x\r\n"
		    "\r\n"
		    "--x\r\n"
		    "v=0\r\n"
		    "--x--\r\n") == SIP_PARSE_OK);
	CHECK(msg_part_first(&m, &it, &part));
	CHECK_STR(part.body, "v=0");

	CASE("a missing close delimiter still yields the last part");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Content-Type: multipart/mixed;boundary=x\r\n"
		    "\r\n"
		    "--x\r\n"
		    "Content-Type: application/sdp\r\n"
		    "\r\n"
		    "v=0\r\n") == SIP_PARSE_OK);
	CHECK(msg_body_sdp_get(&m, &body));
	CHECK(sip_str_prefix(body, "v=0"));

	CASE("a body line that only starts like the boundary is not a delimiter");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Content-Type: multipart/mixed;boundary=x\r\n"
		    "\r\n"
		    "--x\r\n"
		    "Content-Type: text/plain\r\n"
		    "\r\n"
		    "--xNOTBOUNDARY\r\n"
		    "same part\r\n"
		    "--x  \r\n"
		    "Content-Type: application/sdp\r\n"
		    "\r\n"
		    "v=0\r\n"
		    "--x--\r\n") == SIP_PARSE_OK);
	n = 0;
	if (msg_part_first(&m, &it, &part)) {
		do {
			if (!n)
				CHECK_STR(part.body, "--xNOTBOUNDARY\r\nsame part");
			n++;
		} while (part_next(&it, &part));
	}
	CHECK_UINT(n, 2);
	CHECK(msg_body_sdp_get(&m, &body));
	CHECK_STR(body, "v=0");

	CASE("a non multipart type is not walked");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Content-Type: application/isup\r\n"
		    "\r\nISUP\r\n") == SIP_PARSE_OK);
	CHECK(!msg_part_first(&m, &it, &part));
	CHECK(!msg_body_sdp_get(&m, &body));
}

static void lexer_tests(void)
{
	struct sip_str rest, item, name, val;
	char out[64];
	uint32_t u;

	CASE("csv split keeps quoted commas and angle brackets together");
	rest = sip_str_cstr("\"a,b\" <sip:x@y>;q=1, <sip:z@y?to=a,b>, plain");
	CHECK(sip_csv_next(&rest, &item));
	CHECK_STR(item, "\"a,b\" <sip:x@y>;q=1");
	CHECK(sip_csv_next(&rest, &item));
	CHECK_STR(item, "<sip:z@y?to=a,b>");
	CHECK(sip_csv_next(&rest, &item));
	CHECK_STR(item, "plain");
	CHECK(!sip_csv_next(&rest, &item));

	CASE("parameter walk trims around the separators");
	rest = sip_str_cstr(" ; newparam =    newvalue ;  secondparam ; q = 0.33");
	CHECK(sip_param_next(&rest, &name, &val));
	CHECK_STR(name, "newparam");
	CHECK_STR(val, "newvalue");
	CHECK(sip_param_next(&rest, &name, &val));
	CHECK_STR(name, "secondparam");
	CHECK_UINT(val.len, 0);
	CHECK(sip_param_next(&rest, &name, &val));
	CHECK_STR(name, "q");
	CHECK_STR(val, "0.33");
	CHECK(!sip_param_next(&rest, &name, &val));

	CASE("parameter find, quoted value holding a separator");
	CHECK(sip_param_find(sip_str_cstr(";qop=\"auth,auth-int\";stale=TRUE"), "qop", &val));
	CHECK_STR(val, "auth,auth-int");
	CHECK(sip_param_present(sip_str_cstr(";lr;transport=udp"), "lr"));
	CHECK(!sip_param_present(sip_str_cstr(";transport=udp"), "lr"));

	CASE("value split at the first free semicolon");
	CHECK(sip_value_split(sip_str_cstr("application/sdp;charset=\"a;b\""), &item, &rest));
	CHECK_STR(item, "application/sdp");
	CHECK_STR(rest, ";charset=\"a;b\"");
	CHECK(!sip_value_split(sip_str_cstr("application/sdp"), &item, &rest));

	CASE("quoted string unescaping");
	CHECK(sip_unquote(sip_str_cstr("\"J Rosenberg \\\\\\\"\""), out, sizeof(out)) == 14);
	CHECK(!strcmp(out, "J Rosenberg \\\""));
	CHECK(sip_unquote(sip_str_cstr("plain"), out, sizeof(out)) == 5);
	CHECK(sip_unquote(sip_str_cstr("\"toolong\""), out, 4) == -1);

	CASE("percent unescaping of the user part");
	CHECK(sip_uri_unescape(sip_str_cstr("%2317"), out, sizeof(out)) == 3);
	CHECK(!strcmp(out, "#17"));
	CHECK(sip_uri_unescape(sip_str_cstr("a%zzb"), out, sizeof(out)) == 5);
	CHECK(!strcmp(out, "a%zzb"));

	CASE("unsigned parsing");
	CHECK(sip_str_uint(sip_str_cstr("0068"), &u) && u == 68);
	CHECK(sip_str_uint(sip_str_cstr("4294967295"), &u) && u == 4294967295u);
	CHECK(!sip_str_uint(sip_str_cstr("4294967296"), &u));
	CHECK(!sip_str_uint(sip_str_cstr("-1"), &u));
	CHECK(!sip_str_uint(sip_str_cstr(""), &u));
	CHECK(!sip_str_uint(sip_str_cstr("12 "), &u));

	CASE("word take and trim");
	rest = sip_str_cstr("  0009   INVITE  ");
	CHECK(sip_word_take(&rest, &item));
	CHECK_STR(item, "0009");
	CHECK(sip_word_take(&rest, &item));
	CHECK_STR(item, "INVITE");
	CHECK(!sip_word_take(&rest, &item));
	CHECK_STR(sip_str_trim(sip_str_cstr("  x \t")), "x");

	CASE("bounded copy");
	CHECK(sip_str_copy(out, sizeof(out), sip_str_cstr("abc")));
	CHECK(!strcmp(out, "abc"));
	CHECK(!sip_str_copy(out, 3, sip_str_cstr("abc")));
	CHECK(!strcmp(out, "ab"));
}

static void torture_tests(void)
{
	struct sip_str v;
	const struct sip_hdr *h;

	CASE("RFC 4475 3.1.1.1 wsinv parses");
	CHECK(parse(wsinv) == SIP_PARSE_OK);
	CHECK(m.is_request);
	CHECK(m.method == SIP_METHOD_INVITE);
	CHECK_STR(m.ruri, "sip:vivekg@chair-dnrc.example.com;unknownparam");
	CHECK_UINT(m.hdr_count, 14);

	CASE("wsinv folded To and From");
	CHECK(msg_hdr_value(&m, SIP_HDR_TO, &v));
	CHECK_STR(v, "sip:vivekg@chair-dnrc.example.com ;   tag    = 1918181833n");
	CHECK(msg_hdr_value(&m, SIP_HDR_FROM, &v));
	CHECK(sip_str_prefix(v, "\"J Rosenberg \\\\\\\"\""));

	CASE("wsinv folded CSeq and mixed case Max-Forwards");
	CHECK(msg_hdr_value(&m, SIP_HDR_CSEQ, &v));
	CHECK_STR(v, "0009    INVITE");
	CHECK(msg_hdr_value(&m, SIP_HDR_MAX_FORWARDS, &v));
	CHECK_STR(v, "0068");

	CASE("wsinv unknown header keeps its folded value");
	h = msg_hdr_name_first(&m, "NewFangledHeader");
	CHECK(h != NULL);
	if (h)
		CHECK_STR(h->value, "newfangled value   continued newfangled value");

	CASE("wsinv body is the announced 150 octets");
	CHECK(m.clen_present);
	CHECK_UINT(m.clen, 150);
	CHECK_UINT(m.body.len, 150);
	CHECK(!(m.flags & (SIP_MSG_F_BODY_SHORT | SIP_MSG_F_BODY_EXTRA)));
	CHECK(sip_str_prefix(m.body, "v=0\r\no=mhandley"));

	CASE("wsinv body is reachable as SDP");
	CHECK(msg_body_sdp_get(&m, &v));
	CHECK_UINT(v.len, 150);

	CASE("escaped '#' in the user part survives");
	CHECK(parse("INVITE sip:%2317@carrier.net SIP/2.0\r\n"
		    "From: <sip:%2317@carrier.net>;tag=1\r\n\r\n") == SIP_PARSE_OK);
	CHECK_STR(m.ruri, "sip:%2317@carrier.net");

	CASE("verstat in the user part does not break the message");
	CHECK(parse("INVITE sip:b@host SIP/2.0\r\n"
		    "From: \"CALLER\" <sip:+15551234;verstat=No-TN-Validation@carrier.net>"
		    ";tag=9\r\n\r\n") == SIP_PARSE_OK);
	CHECK(msg_hdr_value(&m, SIP_HDR_FROM, &v));
	CHECK(sip_str_find_ci(v, "verstat", NULL));
}

int main(void)
{
	start_line_tests();
	header_tests();
	body_tests();
	multipart_tests();
	lexer_tests();
	torture_tests();

	return tu_summary("test_sip_msg");
}
