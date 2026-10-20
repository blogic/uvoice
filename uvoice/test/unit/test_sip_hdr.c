// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the typed header layer (src/proto/shared/hdr.c).
 *
 * Coverage follows the header set that the daemon needs, the RFC 4475 3.1.1.1
 * whitespace and escape cases, and the interop defects that deployed peers
 * show.
 */
#include <stdlib.h>

#include "proto/shared/hdr.h"
#include "test_util.h"

static struct sip_msg m;
static char buf[SIP_MSG_LEN_MAX + 8];
static char out[2048];

static enum sip_parse_status parse(const char *text)
{
	size_t len = strlen(text);

	memcpy(buf, text, len);

	return msg_parse(&m, buf, len);
}

static bool uri_roundtrip(const char *text)
{
	struct sip_uri u;
	struct sip_buf b;

	if (!uri_parse(sip_str_cstr(text), &u))
		return false;

	sip_buf_init(&b, out, sizeof(out));
	if (!uri_serialize(&u, &b))
		return false;

	return !strcmp(out, text);
}

static void uri_tests(void)
{
	struct sip_uri u;

	CASE("plain sip URI");
	CHECK(uri_parse(sip_str_cstr("sip:5551000@voip.example.net"), &u));
	CHECK_STR(u.scheme, "sip");
	CHECK_STR(u.user, "5551000");
	CHECK_STR(u.host, "voip.example.net");
	CHECK(!(u.flags & SIP_URI_F_PORT));
	CHECK(!(u.flags & SIP_URI_F_SECURE));

	CASE("sips URI with port and transport");
	CHECK(uri_parse(sip_str_cstr("sips:bob@atlanta.com:5061;transport=tls"), &u));
	CHECK(u.flags & SIP_URI_F_SECURE);
	CHECK(u.flags & SIP_URI_F_PORT);
	CHECK_UINT(u.port, 5061);
	CHECK_STR(u.transport, "tls");

	CASE("host only URI");
	CHECK(uri_parse(sip_str_cstr("sip:proxy.example.net"), &u));
	CHECK_UINT(u.user.len, 0);
	CHECK_STR(u.host, "proxy.example.net");

	CASE("IPv6 reference with a port");
	CHECK(uri_parse(sip_str_cstr("sip:a@[2001:db8::1]:5060;transport=udp"), &u));
	CHECK(u.flags & SIP_URI_F_IPV6);
	CHECK_STR(u.host, "2001:db8::1");
	CHECK_UINT(u.port, 5060);

	CASE("IPv6 reference without a port");
	CHECK(uri_parse(sip_str_cstr("sip:[::1]"), &u));
	CHECK(u.flags & SIP_URI_F_IPV6);
	CHECK_STR(u.host, "::1");
	CHECK(!(u.flags & SIP_URI_F_PORT));

	CASE("escaped userinfo is preserved, never decoded");
	CHECK(uri_parse(sip_str_cstr("sip:%2317@carrier.net"), &u));
	CHECK(u.flags & SIP_URI_F_ESCAPED_USER);
	CHECK_STR(u.user, "%2317");
	CHECK(sip_uri_unescape(u.user, out, sizeof(out)) == 3);
	CHECK(!strcmp(out, "#17"));

	CASE("a semicolon inside the user part belongs to the user");
	CHECK(uri_parse(sip_str_cstr("sip:+15551234;verstat=No-TN-Validation@carrier.net"),
			&u));
	CHECK_STR(u.user, "+15551234;verstat=No-TN-Validation");
	CHECK_STR(u.host, "carrier.net");

	CASE("unknown URI parameters and the headers part are kept verbatim");
	CHECK(uri_parse(sip_str_cstr("sip:a@b.com;lr;unknownwith=value;unknown-no-value"),
			&u));
	CHECK(u.flags & SIP_URI_F_LR);
	CHECK_STR(u.params, ";lr;unknownwith=value;unknown-no-value");
	CHECK(uri_parse(sip_str_cstr("sip:a@b.com?Replaces=1%3Bto-tag%3D2"), &u));
	CHECK_STR(u.headers, "Replaces=1%3Bto-tag%3D2");
	CHECK_STR(u.host, "b.com");

	CASE("user and maddr parameters");
	CHECK(uri_parse(sip_str_cstr("sip:+15551000@gw.net;user=phone;maddr=10.0.0.9"), &u));
	CHECK_STR(u.user_param, "phone");
	CHECK_STR(u.maddr, "10.0.0.9");

	CASE("password and tel URI");
	CHECK(uri_parse(sip_str_cstr("sip:alice:secret@atlanta.com"), &u));
	CHECK_STR(u.user, "alice");
	CHECK_STR(u.password, "secret");
	CHECK(uri_parse(sip_str_cstr("tel:+15551234;phone-context=+1"), &u));
	CHECK(u.flags & SIP_URI_F_TEL);
	CHECK_STR(u.user, "+15551234");

	CASE("malformed URIs are refused");
	CHECK(!uri_parse(sip_str_cstr("nocolon"), &u));
	CHECK(!uri_parse(sip_str_cstr("http://example.com/x"), &u));
	CHECK(!uri_parse(sip_str_cstr("sip:"), &u));
	CHECK(!uri_parse(sip_str_cstr("sip:a@host:70000"), &u));
	CHECK(!uri_parse(sip_str_cstr("sip:a@host:port"), &u));
	CHECK(!uri_parse(sip_str_cstr("sip:a@[::1"), &u));

	CASE("serialization round trips what it parsed");
	CHECK(uri_roundtrip("sip:5551000@voip.example.net"));
	CHECK(uri_roundtrip("sips:bob@atlanta.com:5061;transport=tls"));
	CHECK(uri_roundtrip("sip:a@[2001:db8::1]:5060;transport=udp"));
	CHECK(uri_roundtrip("sip:+15551234;verstat=No-TN-Validation@carrier.net"));
	CHECK(uri_roundtrip("sip:%2317@carrier.net"));
	CHECK(uri_roundtrip("sip:services.example.com;lr;unknownwith=value"));
	CHECK(uri_roundtrip("sip:a@b.com?Replaces=1%3Bto-tag%3D2"));
	CHECK(uri_roundtrip("tel:+15551234;phone-context=+1"));
	CHECK(uri_roundtrip("sip:alice:secret@atlanta.com"));
}

static void addr_tests(void)
{
	struct sip_addr a;
	struct sip_val_iter it;
	unsigned int n;

	CASE("quoted display name with escaped quotes (RFC 4475 wsinv)");
	CHECK(hdr_addr_parse(sip_str_cstr(
		"\"J Rosenberg \\\\\\\"\"       <sip:jdrosen@example.com>  ;   tag = 98asjd8"),
		&a));
	CHECK(a.flags & SIP_ADDR_F_QUOTED);
	CHECK(a.flags & SIP_ADDR_F_NAME_ADDR);
	CHECK_STR(a.display, "J Rosenberg \\\\\\\"");
	CHECK(hdr_addr_display_get(&a, out, sizeof(out)) == 14);
	CHECK(!strcmp(out, "J Rosenberg \\\""));
	CHECK_STR(a.tag, "98asjd8");
	CHECK_STR(a.uri.user, "jdrosen");

	CASE("unquoted display name");
	CHECK(hdr_addr_parse(sip_str_cstr("Bob Smith <sip:bob@biloxi.com>;tag=a48s"), &a));
	CHECK_STR(a.display, "Bob Smith");
	CHECK(!(a.flags & SIP_ADDR_F_QUOTED));
	CHECK_STR(a.tag, "a48s");

	CASE("addr-spec parameters are header parameters (RFC 3261 20.10)");
	CHECK(hdr_addr_parse(sip_str_cstr(
		"sip:vivekg@chair-dnrc.example.com ;   tag    = 1918181833n"), &a));
	CHECK(!(a.flags & SIP_ADDR_F_NAME_ADDR));
	CHECK_STR(a.uri.raw, "sip:vivekg@chair-dnrc.example.com");
	CHECK_UINT(a.uri.params.len, 0);
	CHECK_STR(a.tag, "1918181833n");

	CASE("name-addr keeps URI parameters out of the header parameters");
	CHECK(hdr_addr_parse(sip_str_cstr("<sip:a@b;transport=tcp>;tag=x"), &a));
	CHECK_STR(a.uri.params, ";transport=tcp");
	CHECK_STR(a.uri.transport, "tcp");
	CHECK_STR(a.tag, "x");

	CASE("Contact star");
	CHECK(hdr_addr_parse(sip_str_cstr("*"), &a));
	CHECK(a.flags & SIP_ADDR_F_STAR);

	CASE("Contact q and expires with whitespace around the equals");
	CHECK(hdr_addr_parse(sip_str_cstr(
		"\"Quoted string \\\"\\\"\" <sip:jdrosen@example.com> ; newparam =   "
		"newvalue ;  secondparam ; q = 0.33"), &a));
	CHECK(a.flags & SIP_ADDR_F_Q);
	CHECK_UINT(a.q_milli, 330);
	CHECK(hdr_addr_display_get(&a, out, sizeof(out)) == 16);
	CHECK(!strcmp(out, "Quoted string \"\""));
	CHECK(hdr_addr_parse(sip_str_cstr("<sip:a@b>;expires=180;q=1"), &a));
	CHECK(a.flags & SIP_ADDR_F_EXPIRES);
	CHECK_UINT(a.expires, 180);
	CHECK_UINT(a.q_milli, 1000);

	CASE("a broken URI still yields the tag");
	CHECK(hdr_addr_parse(sip_str_cstr("\"No URI\" <>;tag=keepme"), &a));
	CHECK_STR(a.tag, "keepme");
	CHECK_UINT(a.uri.host.len, 0);
	CHECK(hdr_addr_parse(sip_str_cstr("bogus"), &a));
	CHECK_UINT(a.uri.host.len, 0);
	CHECK(!hdr_addr_parse(sip_str_cstr(""), &a));

	CASE("a semicolon inside a quoted display name is not a parameter");
	CHECK(hdr_addr_parse(sip_str_cstr(
		"\"2015550123;verstat=No-TN-Validation\" "
		"<sip:2015550123;verstat=No-TN-Validation@198.51.100.201:5065>;"
		"tag=IJvv2uh23-L77J20260101000000N3706433"), &a));
	CHECK(a.flags & SIP_ADDR_F_QUOTED);
	CHECK(a.flags & SIP_ADDR_F_NAME_ADDR);
	CHECK_STR(a.display, "2015550123;verstat=No-TN-Validation");
	CHECK_STR(a.uri.user, "2015550123;verstat=No-TN-Validation");
	CHECK_STR(a.uri.host, "198.51.100.201");
	CHECK_UINT(a.uri.port, 5065);
	CHECK_STR(a.tag, "IJvv2uh23-L77J20260101000000N3706433");

	CASE("a repeated address parameter keeps the first value");
	CHECK(hdr_addr_parse(sip_str_cstr("<sip:x@y>;tag=1;tag=2;expires=10;expires=20"),
			     &a));
	CHECK_STR(a.tag, "1");
	CHECK_UINT(a.expires, 10);

	CASE("several contacts on one line and over two headers");
	CHECK(parse("SIP/2.0 200 OK\r\n"
		    "Contact: <sip:a@1.1.1.1>;q=0.5, \"B,C\" <sip:b@2.2.2.2>;expires=60\r\n"
		    "Contact: <sip:c@3.3.3.3?to=x,y>\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK_UINT(hdr_val_count(&m, SIP_HDR_CONTACT), 3);
	n = 0;
	if (hdr_addr_first(&m, SIP_HDR_CONTACT, &it, &a)) {
		do {
			if (n == 0)
				CHECK_UINT(a.q_milli, 500);
			if (n == 1) {
				CHECK_STR(a.display, "B,C");
				CHECK_UINT(a.expires, 60);
			}
			if (n == 2)
				CHECK_STR(a.uri.host, "3.3.3.3");
			n++;
		} while (hdr_addr_next(&it, &a));
	}
	CHECK_UINT(n, 3);

	CASE("From and To of a real response");
	CHECK(hdr_addr_get(&m, SIP_HDR_CONTACT, &a));
	CHECK_STR(a.uri.host, "1.1.1.1");
}

static void via_tests(void)
{
	struct sip_via via;

	CASE("simple Via");
	CHECK(parse("SIP/2.0 200 OK\r\n"
		    "Via: SIP/2.0/UDP 10.0.0.5:5060;branch=z9hG4bK776;received=203.0.113.9\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_via_top(&m, &via));
	CHECK_STR(via.proto, "SIP");
	CHECK_STR(via.ver, "2.0");
	CHECK_STR(via.transport, "UDP");
	CHECK_STR(via.host, "10.0.0.5");
	CHECK_UINT(via.port, 5060);
	CHECK(via.flags & SIP_VIA_F_PORT);
	CHECK_STR(via.branch, "z9hG4bK776");
	CHECK(via.flags & SIP_VIA_F_MAGIC);
	CHECK_STR(via.received, "203.0.113.9");
	CHECK(!(via.flags & SIP_VIA_F_RPORT));

	CASE("rport without and with a value");
	CHECK(hdr_via_parse(sip_str_cstr("SIP/2.0/UDP 10.0.0.5;rport;branch=z9hG4bK1"),
			    &via));
	CHECK(via.flags & SIP_VIA_F_RPORT);
	CHECK(!(via.flags & SIP_VIA_F_RPORT_VAL));
	CHECK(hdr_via_parse(sip_str_cstr("SIP/2.0/UDP 10.0.0.5;rport=6001;branch=z9hG4bK1"),
			    &via));
	CHECK(via.flags & SIP_VIA_F_RPORT_VAL);
	CHECK_UINT(via.rport, 6001);

	CASE("Via with an IPv6 sent-by");
	CHECK(hdr_via_parse(sip_str_cstr("SIP/2.0/UDP [2001:db8::2]:5060;branch=z9hG4bK2"),
			    &via));
	CHECK(via.flags & SIP_VIA_F_IPV6);
	CHECK_STR(via.host, "2001:db8::2");
	CHECK_UINT(via.port, 5060);

	CASE("Via with a non magic branch and ttl and maddr");
	CHECK(hdr_via_parse(sip_str_cstr(
		"SIP/2.0/UDP 192.0.2.2;branch=390skdjuw;ttl=16;maddr=224.0.1.75"), &via));
	CHECK(via.flags & SIP_VIA_F_BRANCH);
	CHECK(!(via.flags & SIP_VIA_F_MAGIC));
	CHECK(via.flags & SIP_VIA_F_TTL);
	CHECK_UINT(via.ttl, 16);
	CHECK_STR(via.maddr, "224.0.1.75");

	CASE("Via with whitespace around the slashes (RFC 4475 wsinv)");
	CHECK(hdr_via_parse(sip_str_cstr(
		"SIP  / 2.0  / TCP     spindle.example.com   ;   branch  =   z9hG4bK9ikj8"),
		&via));
	CHECK_STR(via.proto, "SIP");
	CHECK_STR(via.ver, "2.0");
	CHECK_STR(via.transport, "TCP");
	CHECK_STR(via.host, "spindle.example.com");
	CHECK_STR(via.branch, "z9hG4bK9ikj8");

	CASE("Via chain counted over headers and commas");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Via: SIP/2.0/UDP one;branch=z9hG4bK1, SIP/2.0/UDP two;branch=z9hG4bK2\r\n"
		    "Via: SIP/2.0/TCP three;branch=z9hG4bK3\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK_UINT(hdr_via_count(&m), 3);
	CHECK(hdr_via_top(&m, &via));
	CHECK_STR(via.host, "one");

	CASE("malformed Via values");
	CHECK(!hdr_via_parse(sip_str_cstr("SIP/2.0 UDP host"), &via));
	CHECK(!hdr_via_parse(sip_str_cstr("SIP/2.0/UDP"), &via));
	CHECK(!hdr_via_parse(sip_str_cstr(""), &via));
}

static void simple_hdr_tests(void)
{
	uint32_t n, d;
	enum sip_method method;
	enum sip_refresher refresher;
	struct sip_str v, params;

	CASE("CSeq");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nCSeq: 314159 INVITE\r\n\r\n") ==
	      SIP_PARSE_OK);
	CHECK(hdr_cseq_get(&m, &n, &method));
	CHECK_UINT(n, 314159);
	CHECK(method == SIP_METHOD_INVITE);

	CASE("CSeq with folded whitespace and leading zeros");
	CHECK(hdr_cseq_parse(sip_str_cstr("0009    INVITE"), &n, &method, &v));
	CHECK_UINT(n, 9);
	CHECK(method == SIP_METHOD_INVITE);
	CHECK_STR(v, "INVITE");

	CASE("CSeq rejects");
	CHECK(!hdr_cseq_parse(sip_str_cstr("INVITE 1"), &n, &method, NULL));
	CHECK(!hdr_cseq_parse(sip_str_cstr("42"), &n, &method, NULL));
	CHECK(!hdr_cseq_parse(sip_str_cstr(""), &n, &method, NULL));

	CASE("Call-ID is never comma split");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nCall-ID: a,b@host\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_call_id_get(&m, &v));
	CHECK_STR(v, "a,b@host");

	CASE("Max-Forwards, Expires, Min-Expires");
	CHECK(parse("REGISTER sip:a@b SIP/2.0\r\n"
		    "Max-Forwards: 0068\r\n"
		    "Expires: 3600\r\n"
		    "Min-Expires: 240\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_uint_get(&m, SIP_HDR_MAX_FORWARDS, &n) && n == 68);
	CHECK(hdr_uint_get(&m, SIP_HDR_EXPIRES, &n) && n == 3600);
	CHECK(hdr_uint_get(&m, SIP_HDR_MIN_EXPIRES, &n) && n == 240);
	CHECK(!hdr_uint_get(&m, SIP_HDR_MIN_SE, &n));

	CASE("RSeq and RAck (RFC 3262 7.1, 7.2)");
	CHECK(parse("PRACK sip:a@b SIP/2.0\r\n"
		    "RSeq: 988789\r\n"
		    "RAck: 776656 1 INVITE\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_rseq_get(&m, &n) && n == 988789);
	CHECK(hdr_rack_get(&m, &n, &d, &method));
	CHECK_UINT(n, 776656);
	CHECK_UINT(d, 1);
	CHECK(method == SIP_METHOD_INVITE);

	CASE("RAck rejects a missing field");
	CHECK(parse("PRACK sip:a@b SIP/2.0\r\nRAck: 776656 INVITE\r\n\r\n") ==
	      SIP_PARSE_OK);
	CHECK(!hdr_rack_get(&m, &n, &d, &method));
	CHECK(parse("PRACK sip:a@b SIP/2.0\r\nRAck: 776656 1\r\n\r\n") == SIP_PARSE_OK);
	CHECK(!hdr_rack_get(&m, &n, &d, &method));

	CASE("CSeq is bounded by 2**31 (RFC 3261 8.1.1.5)");
	CHECK(hdr_cseq_parse(sip_str_cstr("2147483647 INVITE"), &n, &method, NULL));
	CHECK_UINT(n, SIP_CSEQ_MAX);
	CHECK(!hdr_cseq_parse(sip_str_cstr("2147483648 INVITE"), &n, &method, NULL));
	CHECK(!hdr_cseq_parse(sip_str_cstr("4294967295 INVITE"), &n, &method, NULL));
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nCSeq: 2147483648 INVITE\r\n\r\n") ==
	      SIP_PARSE_OK);
	CHECK(!hdr_cseq_get(&m, &n, &method));

	CASE("RSeq and the RAck response number reach 2**32-1 (RFC 3262 7.1)");
	CHECK(parse("PRACK sip:a@b SIP/2.0\r\n"
		    "RSeq: 1\r\n"
		    "RAck: 1 1 INVITE\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_rseq_get(&m, &n));
	CHECK_UINT(n, 1);
	CHECK(hdr_rack_get(&m, &n, &d, &method));
	CHECK_UINT(n, 1);
	CHECK(parse("PRACK sip:a@b SIP/2.0\r\n"
		    "RSeq: 2147483647\r\n"
		    "RAck: 2147483647 1 INVITE\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_rseq_get(&m, &n));
	CHECK_UINT(n, 2147483647u);
	CHECK(hdr_rack_get(&m, &n, &d, &method));
	CHECK_UINT(n, 2147483647u);
	CHECK(parse("PRACK sip:a@b SIP/2.0\r\n"
		    "RSeq: 2147483648\r\n"
		    "RAck: 2147483648 1 INVITE\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_rseq_get(&m, &n));
	CHECK_UINT(n, 2147483648u);
	CHECK(hdr_rack_get(&m, &n, &d, &method));
	CHECK_UINT(n, 2147483648u);
	CHECK(parse("PRACK sip:a@b SIP/2.0\r\n"
		    "RSeq: 4294967295\r\n"
		    "RAck: 4294967295 1 INVITE\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_rseq_get(&m, &n));
	CHECK_UINT(n, 4294967295u);
	CHECK(hdr_rack_get(&m, &n, &d, &method));
	CHECK_UINT(n, 4294967295u);

	CASE("RSeq 0 and RAck response number 0 name no response (RFC 3262 3)");
	CHECK(parse("PRACK sip:a@b SIP/2.0\r\n"
		    "RSeq: 0\r\n"
		    "RAck: 0 1 INVITE\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(!hdr_rseq_get(&m, &n));
	CHECK(!hdr_rack_get(&m, &n, &d, &method));

	CASE("the RAck CSeq number keeps the CSeq bound (RFC 3261 8.1.1.5)");
	CHECK(parse("PRACK sip:a@b SIP/2.0\r\n"
		    "RAck: 1 2147483647 INVITE\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_rack_get(&m, &n, &d, &method));
	CHECK_UINT(d, SIP_CSEQ_MAX);
	CHECK(parse("PRACK sip:a@b SIP/2.0\r\nRAck: 1 2147483648 INVITE\r\n\r\n") ==
	      SIP_PARSE_OK);
	CHECK(!hdr_rack_get(&m, &n, &d, &method));

	CASE("a number above 2**32-1 is refused by the integer parser");
	CHECK(parse("PRACK sip:a@b SIP/2.0\r\n"
		    "RSeq: 4294967296\r\n"
		    "RAck: 4294967296 1 INVITE\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(!hdr_rseq_get(&m, &n));
	CHECK(!hdr_rack_get(&m, &n, &d, &method));

	CASE("Retry-After with a comment and a duration");
	CHECK(parse("SIP/2.0 503 Service Unavailable\r\n"
		    "Retry-After: 120 (I am in a meeting);duration=3600\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_retry_after_get(&m, &n, &d));
	CHECK_UINT(n, 120);
	CHECK_UINT(d, 3600);

	CASE("Retry-After plain");
	CHECK(parse("SIP/2.0 503 Service Unavailable\r\nRetry-After: 30\r\n\r\n") ==
	      SIP_PARSE_OK);
	CHECK(hdr_retry_after_get(&m, &n, &d));
	CHECK_UINT(n, 30);
	CHECK_UINT(d, 0);

	CASE("session timers (RFC 4028)");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Session-Expires: 1800;refresher=uac\r\n"
		    "Min-SE: 90\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_session_expires_get(&m, &n, &refresher));
	CHECK_UINT(n, 1800);
	CHECK(refresher == SIP_REFRESHER_UAC);
	CHECK(hdr_min_se_get(&m, &n) && n == 90);

	CASE("session timer without a refresher, compact form x");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nx: 1800\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_session_expires_get(&m, &n, &refresher));
	CHECK(refresher == SIP_REFRESHER_NONE);

	CASE("Content-Type with parameters");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Content-Type: multipart/mixed;boundary=\"x\"\r\n\r\n") ==
	      SIP_PARSE_OK);
	CHECK(hdr_content_type_get(&m, &v, &params));
	CHECK_STR(v, "multipart/mixed");
	CHECK_STR(params, ";boundary=\"x\"");
}

static void token_tests(void)
{
	CASE("option tags over several headers");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Supported: timer, replaces\r\n"
		    "Supported: 100rel\r\n"
		    "Require: 100rel\r\n"
		    "Allow: INVITE, ACK, CANCEL, BYE, OPTIONS, INFO, NOTIFY\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_token_has(&m, SIP_HDR_SUPPORTED, "timer"));
	CHECK(hdr_token_has(&m, SIP_HDR_SUPPORTED, "100rel"));
	CHECK(hdr_token_has(&m, SIP_HDR_SUPPORTED, "REPLACES"));
	CHECK(!hdr_token_has(&m, SIP_HDR_SUPPORTED, "norefersub"));
	CHECK(hdr_token_has(&m, SIP_HDR_REQUIRE, "100rel"));
	CHECK(hdr_token_has(&m, SIP_HDR_ALLOW, "BYE"));
	CHECK(!hdr_token_has(&m, SIP_HDR_ALLOW, "UPDATE"));
	CHECK(!hdr_token_has(&m, SIP_HDR_UNSUPPORTED, "100rel"));

	CASE("empty and missing token lists");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nSupported: \r\n\r\n") == SIP_PARSE_OK);
	CHECK(!hdr_token_has(&m, SIP_HDR_SUPPORTED, "timer"));
	CHECK_UINT(hdr_val_count(&m, SIP_HDR_SUPPORTED), 0);

	CASE("Privacy is semicolon separated (RFC 3323)");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nPrivacy: header;session;id\r\n\r\n") ==
	      SIP_PARSE_OK);
	CHECK(hdr_privacy_has(&m, "id"));
	CHECK(hdr_privacy_has(&m, "session"));
	CHECK(!hdr_privacy_has(&m, "none"));
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nPrivacy: none\r\n\r\n") == SIP_PARSE_OK);
	CHECK(!hdr_privacy_has(&m, "id"));
}

static void auth_tests(void)
{
	struct sip_chal_iter it;
	struct sip_challenge c;
	struct sip_cred cred;
	struct sip_buf b;

	CASE("digest challenge with qop and stale");
	CHECK(parse("SIP/2.0 401 Unauthorized\r\n"
		    "WWW-Authenticate: Digest realm=\"atlanta.example.com\", "
		    "domain=\"sip:boxesbybob.example.com\", "
		    "qop=\"auth,auth-int\", "
		    "nonce=\"f84f1cec41e6cbe5aea9c8e88d359\", "
		    "opaque=\"\", stale=TRUE, algorithm=MD5\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_chal_first(&m, SIP_HDR_WWW_AUTHENTICATE, &it, &c));
	CHECK(c.flags & SIP_CHAL_F_DIGEST);
	CHECK_STR(c.realm, "atlanta.example.com");
	CHECK_STR(c.nonce, "f84f1cec41e6cbe5aea9c8e88d359");
	CHECK_STR(c.algorithm, "MD5");
	CHECK_UINT(c.opaque.len, 0);
	CHECK(c.flags & SIP_CHAL_F_STALE);
	CHECK(c.flags & SIP_CHAL_F_QOP);
	CHECK(c.flags & SIP_CHAL_F_QOP_AUTH);
	CHECK(c.flags & SIP_CHAL_F_QOP_AUTH_INT);
	CHECK(!hdr_chal_next(&it, &c));

	CASE("digest challenge without qop (RFC 2069 form)");
	CHECK(parse("SIP/2.0 407 Proxy Authentication Required\r\n"
		    "Proxy-Authenticate: Digest realm=\"carrier\", nonce=\"abc123\"\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_chal_first(&m, SIP_HDR_PROXY_AUTHENTICATE, &it, &c));
	CHECK(!(c.flags & SIP_CHAL_F_QOP));
	CHECK(!(c.flags & SIP_CHAL_F_STALE));
	CHECK_STR(c.realm, "carrier");

	CASE("two challenges in one header");
	CHECK(parse("SIP/2.0 401 Unauthorized\r\n"
		    "WWW-Authenticate: Digest realm=\"one\", nonce=\"n1\", "
		    "Digest realm=\"two\", nonce=\"n2\", algorithm=SHA-256\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_chal_first(&m, SIP_HDR_WWW_AUTHENTICATE, &it, &c));
	CHECK_STR(c.realm, "one");
	CHECK_STR(c.nonce, "n1");
	CHECK(hdr_chal_next(&it, &c));
	CHECK_STR(c.realm, "two");
	CHECK_STR(c.algorithm, "SHA-256");
	CHECK(!hdr_chal_next(&it, &c));

	CASE("challenges spread over two headers");
	CHECK(parse("SIP/2.0 401 Unauthorized\r\n"
		    "WWW-Authenticate: Digest realm=\"a\", nonce=\"n1\"\r\n"
		    "WWW-Authenticate: Digest realm=\"b\", nonce=\"n2\"\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_chal_first(&m, SIP_HDR_WWW_AUTHENTICATE, &it, &c));
	CHECK_STR(c.realm, "a");
	CHECK(hdr_chal_next(&it, &c));
	CHECK_STR(c.realm, "b");
	CHECK(!hdr_chal_next(&it, &c));

	CASE("quoted challenge values keep their escapes");
	CHECK(hdr_chal_parse(sip_str_cstr("Digest realm=\"a \\\"quoted\\\" realm\", "
					  "nonce=\"n,1\", stale = true"), &it, &c));
	CHECK_STR(c.realm, "a \\\"quoted\\\" realm");
	CHECK(sip_unquote(c.realm, out, sizeof(out)) == 16);
	CHECK(!strcmp(out, "a \"quoted\" realm"));
	CHECK_STR(c.nonce, "n,1");
	CHECK(c.flags & SIP_CHAL_F_STALE);

	CASE("an unquoted qop list keeps every token, in either order");
	CHECK(hdr_chal_parse(sip_str_cstr(
		"Digest realm=\"r\", qop=auth-int,auth, nonce=\"n1\""), &it, &c));
	CHECK(c.flags & SIP_CHAL_F_QOP);
	CHECK(c.flags & SIP_CHAL_F_QOP_AUTH);
	CHECK(c.flags & SIP_CHAL_F_QOP_AUTH_INT);
	CHECK_STR(c.nonce, "n1");
	CHECK_STR(c.realm, "r");
	CHECK(!hdr_chal_next(&it, &c));
	CHECK(hdr_chal_parse(sip_str_cstr(
		"Digest realm=\"r\", qop=auth,auth-int, nonce=\"n2\""), &it, &c));
	CHECK(c.flags & SIP_CHAL_F_QOP_AUTH);
	CHECK(c.flags & SIP_CHAL_F_QOP_AUTH_INT);
	CHECK_STR(c.nonce, "n2");
	CHECK(hdr_chal_parse(sip_str_cstr("Digest realm=\"r\", qop=auth, nonce=\"n3\""),
			     &it, &c));
	CHECK(c.flags & SIP_CHAL_F_QOP_AUTH);
	CHECK(!(c.flags & SIP_CHAL_F_QOP_AUTH_INT));
	CHECK_STR(c.nonce, "n3");

	CASE("an unquoted qop list does not swallow a following challenge");
	CHECK(hdr_chal_parse(sip_str_cstr(
		"Digest realm=\"one\", qop=auth,auth-int, Digest realm=\"two\""), &it, &c));
	CHECK_STR(c.realm, "one");
	CHECK(c.flags & SIP_CHAL_F_QOP_AUTH_INT);
	CHECK(hdr_chal_next(&it, &c));
	CHECK_STR(c.realm, "two");

	CASE("a non digest scheme is reported, not parsed away");
	CHECK(hdr_chal_parse(sip_str_cstr("Basic realm=\"x\""), &it, &c));
	CHECK(!(c.flags & SIP_CHAL_F_DIGEST));
	CHECK_STR(c.scheme, "Basic");

	CASE("credentials with qop");
	memset(&cred, 0, sizeof(cred));
	cred.username = "5551000";
	cred.realm = "carrier";
	cred.nonce = "abc";
	cred.uri = "sip:carrier";
	cred.response = "deadbeef";
	cred.algorithm = "MD5";
	cred.cnonce = "0a4f113b";
	cred.qop = "auth";
	cred.opaque = "op";
	cred.nc = 1;
	sip_buf_init(&b, out, sizeof(out));
	CHECK(hdr_cred_write(&b, SIP_HDR_AUTHORIZATION, &cred));
	CHECK(!strcmp(out,
		      "Authorization: Digest username=\"5551000\", realm=\"carrier\", "
		      "nonce=\"abc\", uri=\"sip:carrier\", response=\"deadbeef\", "
		      "algorithm=MD5, cnonce=\"0a4f113b\", qop=auth, nc=00000001, "
		      "opaque=\"op\"\r\n"));

	CASE("credentials without qop and with an escaped username");
	memset(&cred, 0, sizeof(cred));
	cred.username = "odd\"user";
	cred.realm = "r";
	cred.nonce = "n";
	cred.uri = "sip:r";
	cred.response = "rr";
	sip_buf_init(&b, out, sizeof(out));
	CHECK(hdr_cred_write(&b, SIP_HDR_PROXY_AUTHORIZATION, &cred));
	CHECK(!strcmp(out,
		      "Proxy-Authorization: Digest username=\"odd\\\"user\", realm=\"r\", "
		      "nonce=\"n\", uri=\"sip:r\", response=\"rr\"\r\n"));

	CASE("credentials refuse to write without the mandatory fields");
	memset(&cred, 0, sizeof(cred));
	cred.username = "u";
	sip_buf_init(&b, out, sizeof(out));
	CHECK(!hdr_cred_write(&b, SIP_HDR_AUTHORIZATION, &cred));

	CASE("qop credentials refuse to write without cnonce and nc");
	memset(&cred, 0, sizeof(cred));
	cred.username = "u";
	cred.realm = "r";
	cred.nonce = "n";
	cred.uri = "sip:r";
	cred.response = "rr";
	cred.qop = "auth";
	cred.nc = 1;
	sip_buf_init(&b, out, sizeof(out));
	CHECK(!hdr_cred_write(&b, SIP_HDR_AUTHORIZATION, &cred));
	cred.cnonce = "";
	sip_buf_init(&b, out, sizeof(out));
	CHECK(!hdr_cred_write(&b, SIP_HDR_AUTHORIZATION, &cred));
	cred.cnonce = "0a4f113b";
	cred.nc = 0;
	sip_buf_init(&b, out, sizeof(out));
	CHECK(!hdr_cred_write(&b, SIP_HDR_AUTHORIZATION, &cred));
	cred.nc = 1;
	sip_buf_init(&b, out, sizeof(out));
	CHECK(hdr_cred_write(&b, SIP_HDR_AUTHORIZATION, &cred));
	/* an empty qop reads as absent; it never emits a bare "qop=" */
	cred.qop = "";
	cred.cnonce = NULL;
	cred.nc = 0;
	sip_buf_init(&b, out, sizeof(out));
	CHECK(hdr_cred_write(&b, SIP_HDR_AUTHORIZATION, &cred));
	CHECK(!strstr(out, "qop"));
	CHECK(!strstr(out, "nc="));
}

static void event_tests(void)
{
	struct sip_event ev;
	struct sip_sub_state st;

	CASE("Event package and id");
	CHECK(parse("NOTIFY sip:a@b SIP/2.0\r\n"
		    "Event: message-summary\r\n"
		    "Content-Type: application/simple-message-summary\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_event_get(&m, &ev));
	CHECK_STR(ev.package, "message-summary");
	CHECK_UINT(ev.id.len, 0);
	CHECK(parse("NOTIFY sip:a@b SIP/2.0\r\no: dialog;id=42\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_event_get(&m, &ev));
	CHECK_STR(ev.package, "dialog");
	CHECK_STR(ev.id, "42");

	CASE("Subscription-State active with expires");
	CHECK(parse("NOTIFY sip:a@b SIP/2.0\r\n"
		    "Subscription-State: active;expires=3600\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_sub_state_get(&m, &st));
	CHECK_STR(st.state, "active");
	CHECK(st.flags & SIP_SUB_F_EXPIRES);
	CHECK_UINT(st.expires, 3600);

	CASE("Subscription-State terminated with a reason and retry-after");
	CHECK(parse("NOTIFY sip:a@b SIP/2.0\r\n"
		    "Subscription-State: terminated;reason=timeout;retry-after=60\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_sub_state_get(&m, &st));
	CHECK_STR(st.state, "terminated");
	CHECK_STR(st.reason, "timeout");
	CHECK(st.flags & SIP_SUB_F_RETRY_AFTER);
	CHECK_UINT(st.retry_after, 60);
}

/*
 * An exact length copy of text on the heap. The four grammars below take a span
 * and not a C string, so a parser that walks one byte past the length reads
 * heap this test never wrote, and malloc(len) without a terminator turns that
 * into an ASan report instead of a pass (test_sip_sub.c holds the same rule).
 * The previous fixture is released on the next take and the last one at the end
 * of refer_tests().
 */
static char *fixture;

static struct sip_str fixture_take(const char *text)
{
	size_t len = strlen(text);

	free(fixture);
	fixture = malloc(len ? len : 1);
	if (len)
		memcpy(fixture, text, len);

	return sip_str_init(fixture, len);
}

static bool replaces_parse(const char *text, struct sip_replaces *rp)
{
	return hdr_replaces_parse(fixture_take(text), rp);
}

static bool headers_get(const char *text, const char *name, char *dst, size_t dlen)
{
	return uri_headers_get(fixture_take(text), name, dst, dlen);
}

static bool sipfrag_code(const char *text, uint32_t *code)
{
	return sipfrag_status(fixture_take(text), code);
}

/* the same exact length rule for a whole message */
static bool msg_take(const char *text)
{
	struct sip_str s = fixture_take(text);

	return msg_parse(&m, fixture, s.len) == SIP_PARSE_OK;
}

static void refer_tests(void)
{
	struct sip_replaces rp;
	struct sip_buf b;
	struct sip_uri u;
	struct sip_str v;
	uint32_t code;
	char small[8];
	char val[128];

	CASE("Replaces is indexed by name and has no compact form (RFC 3891)");
	CHECK(msg_take("INVITE sip:a@b SIP/2.0\r\n"
		       "Replaces: cid@h;to-tag=t1;from-tag=f1\r\n"
		       "r: <sip:c@d>\r\n"
		       "\r\n"));
	CHECK(!strcmp(sip_hdr_name(SIP_HDR_REPLACES), "Replaces"));
	CHECK(msg_hdr_value(&m, SIP_HDR_REPLACES, &v));
	CHECK(hdr_replaces_parse(v, &rp));
	CHECK_STR(rp.call_id, "cid@h");
	CHECK_STR(rp.to_tag, "t1");
	CHECK(msg_hdr_value(&m, SIP_HDR_REFER_TO, &v));
	CHECK_STR(v, "<sip:c@d>");

	CASE("Replaces: the minimal value (RFC 3891 6.1)");
	CHECK(replaces_parse("a84b4c76e66710@atlanta.example.com;to-tag=8321234356;"
			     "from-tag=9525", &rp));
	CHECK_STR(rp.call_id, "a84b4c76e66710@atlanta.example.com");
	CHECK_STR(rp.to_tag, "8321234356");
	CHECK_STR(rp.from_tag, "9525");
	CHECK(!rp.early_only);

	CASE("Replaces: early-only is a flag");
	CHECK(replaces_parse("cid@h;to-tag=t1;from-tag=f1;early-only", &rp));
	CHECK(rp.early_only);
	CHECK_STR(rp.to_tag, "t1");
	CHECK(replaces_parse("cid@h;early-only;to-tag=t1;from-tag=f1", &rp));
	CHECK(rp.early_only);
	CHECK(replaces_parse("cid@h;to-tag=t1;from-tag=f1;early-only=true", &rp));
	CHECK(rp.early_only);

	CASE("Replaces: the two tags in either order");
	CHECK(replaces_parse("cid@h;from-tag=f1;to-tag=t1", &rp));
	CHECK_STR(rp.to_tag, "t1");
	CHECK_STR(rp.from_tag, "f1");
	CHECK(!rp.early_only);

	CASE("Replaces: an unknown parameter is ignored");
	CHECK(replaces_parse("cid@h;vendor=x;to-tag=t1;novalue;from-tag=f1;trailing=\"a;b\"",
			     &rp));
	CHECK_STR(rp.call_id, "cid@h");
	CHECK_STR(rp.to_tag, "t1");
	CHECK_STR(rp.from_tag, "f1");

	CASE("Replaces: a missing to-tag fails (RFC 3891 6.1)");
	CHECK(!replaces_parse("cid@h;from-tag=f1", &rp));
	CHECK(!replaces_parse("cid@h;to-tag=;from-tag=f1", &rp));
	CHECK(!replaces_parse("cid@h;to-tag;from-tag=f1", &rp));

	CASE("Replaces: a missing from-tag fails (RFC 3891 6.1)");
	CHECK(!replaces_parse("cid@h;to-tag=t1", &rp));
	CHECK(!replaces_parse("cid@h;to-tag=t1;from-tag=", &rp));
	CHECK(!replaces_parse("cid@h;to-tag=t1;from-tag", &rp));

	CASE("Replaces: a repeated tag fails, exactly one of each is allowed");
	CHECK(!replaces_parse("cid@h;to-tag=t1;to-tag=t2;from-tag=f1", &rp));
	CHECK(!replaces_parse("cid@h;to-tag=t1;from-tag=f1;from-tag=f2", &rp));
	CHECK(!replaces_parse("cid@h;to-tag=t1;from-tag=f1;TO-TAG=t1", &rp));

	CASE("Replaces: an empty value and a value without parameters fail");
	CHECK(!replaces_parse("", &rp));
	CHECK(!replaces_parse("   ", &rp));
	CHECK(!replaces_parse("cid@h", &rp));
	CHECK(!replaces_parse(";to-tag=t1;from-tag=f1", &rp));

	CASE("Replaces: the Call-ID keeps every word character (RFC 3261 25.1)");
	CHECK(replaces_parse("f81d4fae-7dec-11d0-a765-00a0c91e6bf6@[2001:db8::1]"
			     ";to-tag=t1;from-tag=f1", &rp));
	CHECK_STR(rp.call_id, "f81d4fae-7dec-11d0-a765-00a0c91e6bf6@[2001:db8::1]");
	CHECK(replaces_parse("a,b/c?d{e}@host;to-tag=t1;from-tag=f1", &rp));
	CHECK_STR(rp.call_id, "a,b/c?d{e}@host");

	/*
	 * The Call-ID is cut at the raw first semicolon, not with the quote and
	 * angle aware scanner. A '"' or a '<' in the callid derails that scanner,
	 * so these two pin the cut: a later switch to sip_value_split() breaks
	 * them while the rest of the suite still passes.
	 */
	CASE("Replaces: a Call-ID with a quote or an angle bracket parses");
	CHECK(replaces_parse("a\"b@host;to-tag=t1;from-tag=f1", &rp));
	CHECK_STR(rp.call_id, "a\"b@host");
	CHECK_STR(rp.to_tag, "t1");
	CHECK_STR(rp.from_tag, "f1");
	CHECK(replaces_parse("a<b@host;to-tag=t2;from-tag=f2", &rp));
	CHECK_STR(rp.call_id, "a<b@host");
	CHECK_STR(rp.to_tag, "t2");
	CHECK_STR(rp.from_tag, "f2");

	/*
	 * An unmatched quote fails the value closed. The separator scan treats
	 * an opening quote as a run that swallows every ';' until the closing
	 * one, so an unterminated one hides the whole tail of the value: the
	 * two tags in front of it still parse, and a parameter behind it,
	 * ";early-only" above all, is invisible rather than merely unread. This
	 * grammar decides which call a request replaces, so a value this side
	 * cannot read to its end is refused.
	 */
	CASE("Replaces: an unterminated quoted parameter fails the value");
	CHECK(!replaces_parse("cid;to-tag=t;from-tag=f;vendor=\"unterminated;early-only", &rp));

	CASE("Replaces: and it fails wherever the quote opens");
	CHECK(!replaces_parse("cid;vendor=\"open;to-tag=t;from-tag=f", &rp));
	CHECK(!replaces_parse("cid;to-tag=t;from-tag=f;x=\"", &rp));
	CHECK(!replaces_parse("cid;to-tag=\"t;from-tag=f", &rp));

	CASE("Replaces: a CLOSED quoted parameter is still ignored, not refused");
	CHECK(replaces_parse("cid;to-tag=t1;from-tag=f1;vendor=\"a;b\";early-only", &rp));
	CHECK_STR(rp.to_tag, "t1");
	CHECK_STR(rp.from_tag, "f1");
	CHECK(rp.early_only);

	CASE("Replaces: an escaped quote inside a closed one does not open a run");
	CHECK(replaces_parse("cid;to-tag=t1;from-tag=f1;vendor=\"a\\\"b\";early-only", &rp));
	CHECK(rp.early_only);

	CASE("Replaces: an escaped quote that leaves the run open still fails");
	CHECK(!replaces_parse("cid;to-tag=t1;from-tag=f1;vendor=\"a\\\"", &rp));

	/* the rule leaves the raw Call-ID cut untouched: RFC 3261 25.1 lets a
	 * callid word carry a '"', and that quote reaches no scanner */
	CASE("Replaces: a lone quote in the Call-ID is not an unmatched one");
	CHECK(replaces_parse("a\"b@host;to-tag=t1;from-tag=f1;early-only", &rp));
	CHECK_STR(rp.call_id, "a\"b@host");
	CHECK(rp.early_only);

	CASE("Replaces: whitespace around the separators (RFC 4475 3.1.1.1)");
	CHECK(replaces_parse("cid@h ;  to-tag = t1 ;from-tag\t=\tf1 ; early-only ", &rp));
	CHECK_STR(rp.call_id, "cid@h");
	CHECK_STR(rp.to_tag, "t1");
	CHECK_STR(rp.from_tag, "f1");
	CHECK(rp.early_only);

	CASE("Replaces write: the exact bytes, with and without early-only");
	sip_buf_init(&b, out, sizeof(out));
	CHECK(hdr_replaces_write(&b, "a84b4c76e66710@atlanta.example.com", "8321234356",
				 "9525", false));
	CHECK(!strcmp(out, "a84b4c76e66710@atlanta.example.com;to-tag=8321234356;"
			   "from-tag=9525"));
	sip_buf_init(&b, out, sizeof(out));
	CHECK(hdr_replaces_write(&b, "cid@h", "t1", "f1", true));
	CHECK(!strcmp(out, "cid@h;to-tag=t1;from-tag=f1;early-only"));

	CASE("Replaces write: what it wrote parses back");
	CHECK(replaces_parse(out, &rp));
	CHECK_STR(rp.call_id, "cid@h");
	CHECK_STR(rp.to_tag, "t1");
	CHECK_STR(rp.from_tag, "f1");
	CHECK(rp.early_only);

	CASE("Replaces write: a missing field refuses and an overflow sticks");
	sip_buf_init(&b, out, sizeof(out));
	CHECK(!hdr_replaces_write(&b, "cid@h", NULL, "f1", false));
	CHECK(!hdr_replaces_write(&b, "cid@h", "", "f1", false));
	CHECK(!hdr_replaces_write(&b, NULL, "t1", "f1", false));
	CHECK(!hdr_replaces_write(&b, "cid@h", "t1", "", false));
	CHECK(!hdr_replaces_write(&b, "cid@h", "t1\r\nEvil: x", "f1", false));
	CHECK(!hdr_replaces_write(&b, "cid\r\n@h", "t1", "f1", false));
	CHECK(!hdr_replaces_write(&b, "cid@h", "t1", "f1\x7f", false));
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(sip_buf_ok(&b));
	sip_buf_init(&b, small, sizeof(small));
	CHECK(!hdr_replaces_write(&b, "cid@h", "t1", "f1", false));
	CHECK(!sip_buf_ok(&b));
	CHECK(sip_buf_str(&b) == NULL);

	CASE("URI headers: the name matches without case");
	CHECK(headers_get("Replaces=xyz", "replaces", out, sizeof(out)));
	CHECK(!strcmp(out, "xyz"));
	CHECK(headers_get("replaces=xyz", "Replaces", out, sizeof(out)));
	CHECK(!strcmp(out, "xyz"));

	CASE("URI headers: a later pair is found and a malformed one is stepped over");
	CHECK(headers_get("Subject=call&Replaces=xyz", "Replaces", out, sizeof(out)));
	CHECK(!strcmp(out, "xyz"));
	CHECK(headers_get("novalue&=noname&Replaces=xyz", "Replaces", out, sizeof(out)));
	CHECK(!strcmp(out, "xyz"));
	CHECK(headers_get("Replaces=first&Replaces=second", "Replaces", out, sizeof(out)));
	CHECK(!strcmp(out, "first"));

	CASE("URI headers: an escaped Replaces unescapes to its separators");
	CHECK(headers_get("Replaces=cid%40h%3Bto-tag%3Dt1%3Bfrom-tag%3Df1", "Replaces",
			  out, sizeof(out)));
	CHECK(!strcmp(out, "cid@h;to-tag=t1;from-tag=f1"));
	CHECK(replaces_parse(out, &rp));
	CHECK_STR(rp.to_tag, "t1");
	CHECK_STR(rp.from_tag, "f1");

	CASE("URI headers: a broken escape fails (RFC 3261 25.1)");
	CHECK(!headers_get("Replaces=%G1", "Replaces", out, sizeof(out)));
	CHECK(!headers_get("Replaces=a%", "Replaces", out, sizeof(out)));
	CHECK(!headers_get("Replaces=a%4", "Replaces", out, sizeof(out)));
	CHECK(!headers_get("Replaces=%4G1", "Replaces", out, sizeof(out)));
	CHECK(headers_get("Replaces=%41%42", "Replaces", out, sizeof(out)));
	CHECK(!strcmp(out, "AB"));

	CASE("URI headers: a decoded control byte is refused, out cleared, space kept");
	CHECK(!headers_get("Replaces=cid%0D%0AEvil%3A%20x", "Replaces", out, sizeof(out)));
	CHECK(out[0] == 0);
	CHECK(!headers_get("Replaces=a%00b", "Replaces", out, sizeof(out)));
	CHECK(out[0] == 0);
	CHECK(!headers_get("Replaces=tab%09here", "Replaces", out, sizeof(out)));
	CHECK(out[0] == 0);
	CHECK(headers_get("Replaces=a%20b", "Replaces", out, sizeof(out)));
	CHECK(!strcmp(out, "a b"));

	CASE("URI headers: a value that fills the buffer passes, one byte more fails");
	CHECK(headers_get("Replaces=1234567", "Replaces", small, sizeof(small)));
	CHECK(!strcmp(small, "1234567"));
	CHECK(!headers_get("Replaces=12345678", "Replaces", small, sizeof(small)));
	CHECK(headers_get("Replaces=123456%37", "Replaces", small, sizeof(small)));
	CHECK(!strcmp(small, "1234567"));
	CHECK(!headers_get("Replaces=1234567%38", "Replaces", small, sizeof(small)));

	CASE("URI headers: an absent name and an empty span fail");
	CHECK(!headers_get("Subject=call", "Replaces", out, sizeof(out)));
	CHECK(!headers_get("", "Replaces", out, sizeof(out)));
	CHECK(!headers_get("Replaces=x", "Replaces", out, 0));
	CHECK(!headers_get("Replaces=x", "Replaces", NULL, sizeof(out)));

	CASE("URI header escape: unreserved and hnv-unreserved pass through");
	sip_buf_init(&b, out, sizeof(out));
	CHECK(uri_header_escape(&b, "azAZ09-_.!~*'()[]/?:+$"));
	CHECK(!strcmp(out, "azAZ09-_.!~*'()[]/?:+$"));

	CASE("URI header escape: a high byte and a control byte, upper case hex");
	sip_buf_init(&b, out, sizeof(out));
	CHECK(uri_header_escape(&b, "caf\xC3\xA9"));
	CHECK(!strcmp(out, "caf%C3%A9"));
	sip_buf_init(&b, out, sizeof(out));
	CHECK(uri_header_escape(&b, "\x7F\x01"));
	CHECK(!strcmp(out, "%7F%01"));

	CASE("URI header escape: the exact bytes of an escaped Replaces");
	sip_buf_init(&b, out, sizeof(out));
	CHECK(uri_header_escape(&b, "a@b;to-tag=1&x=2 %"));
	CHECK(!strcmp(out, "a%40b%3Bto-tag%3D1%26x%3D2%20%25"));
	sip_buf_init(&b, small, sizeof(small));
	CHECK(!uri_header_escape(&b, "a@b;to-tag=1&x=2 %"));
	CHECK(!sip_buf_ok(&b));

	CASE("URI header escape: a round trip through uri_headers_get");
	sip_buf_init(&b, out, sizeof(out));
	sip_buf_add(&b, "Replaces=");
	CHECK(uri_header_escape(&b, "cid@h;to-tag=t1;from-tag=f1;x=a b%c&d"));
	CHECK(headers_get(out, "Replaces", val, sizeof(val)));
	CHECK(!strcmp(val, "cid@h;to-tag=t1;from-tag=f1;x=a b%c&d"));

	CASE("a Refer-To URI carries the Replaces of an attended transfer");
	CHECK(uri_parse(fixture_take("sip:b@example.net?Replaces=cid%40h%3Bto-tag%3Dt1"
				     "%3Bfrom-tag%3Df1&Require=replaces"), &u));
	CHECK_STR(u.user, "b");
	CHECK_STR(u.host, "example.net");
	CHECK(uri_headers_get(u.headers, "Replaces", val, sizeof(val)));
	CHECK(!strcmp(val, "cid@h;to-tag=t1;from-tag=f1"));
	CHECK(uri_headers_get(u.headers, "Require", out, sizeof(out)));
	CHECK(!strcmp(out, "replaces"));
	CHECK(replaces_parse(val, &rp));
	CHECK_STR(rp.call_id, "cid@h");
	CHECK_STR(rp.to_tag, "t1");
	CHECK_STR(rp.from_tag, "f1");

	CASE("sipfrag write: an explicit reason and the default one (RFC 3420)");
	sip_buf_init(&b, out, sizeof(out));
	CHECK(sipfrag_write(&b, 100, NULL));
	CHECK(!strcmp(out, "SIP/2.0 100 Trying\r\n"));
	sip_buf_init(&b, out, sizeof(out));
	CHECK(sipfrag_write(&b, 200, NULL));
	CHECK(!strcmp(out, "SIP/2.0 200 OK\r\n"));
	sip_buf_init(&b, out, sizeof(out));
	CHECK(sipfrag_write(&b, 486, "Busy Here"));
	CHECK(!strcmp(out, "SIP/2.0 486 Busy Here\r\n"));
	sip_buf_init(&b, small, sizeof(small));
	CHECK(!sipfrag_write(&b, 486, "Busy Here"));
	CHECK(!sip_buf_ok(&b));

	CASE("sipfrag status: a bare status line");
	CHECK(sipfrag_code("SIP/2.0 200 OK\r\n", &code));
	CHECK_UINT(code, 200);
	CHECK(sipfrag_code("sip/2.0 180 Ringing\r\n", &code));
	CHECK_UINT(code, 180);

	CASE("sipfrag status: trailing headers are ignored (RFC 3515 2.4.5)");
	CHECK(sipfrag_code("SIP/2.0 180 Ringing\r\n"
			   "Contact: <sip:b@example.net>\r\n"
			   "\r\n", &code));
	CHECK_UINT(code, 180);
	CHECK(sipfrag_code("SIP/2.0 486 Busy Here\nCSeq: 2 INVITE\n", &code));
	CHECK_UINT(code, 486);

	CASE("sipfrag status: a missing final CRLF and a leading one");
	CHECK(sipfrag_code("SIP/2.0 486 Busy Here", &code));
	CHECK_UINT(code, 486);
	CHECK(sipfrag_code("SIP/2.0 200 OK", &code));
	CHECK_UINT(code, 200);
	CHECK(sipfrag_code("\r\nSIP/2.0 202 Accepted\r\n", &code));
	CHECK_UINT(code, 202);

	CASE("sipfrag status: a body that is not a status line is refused");
	CHECK(!sipfrag_code("", &code));
	CHECK(!sipfrag_code("\r\n\r\n", &code));
	CHECK(!sipfrag_code("NOTIFY sip:a@b SIP/2.0\r\n", &code));
	CHECK(!sipfrag_code("SIP/2.0\r\n", &code));
	CHECK(!sipfrag_code("SIP/1.0 200 OK\r\n", &code));
	CHECK(!sipfrag_code("SIP/2.0 OK 200\r\n", &code));
	CHECK(!sipfrag_code("v=0\r\no=- 1 1 IN IP4 10.0.0.5\r\n", &code));

	CASE("sipfrag status: the code range is 100 to 699");
	CHECK(sipfrag_code("SIP/2.0 100 Trying\r\n", &code));
	CHECK_UINT(code, 100);
	CHECK(sipfrag_code("SIP/2.0 699 X\r\n", &code));
	CHECK_UINT(code, 699);
	CHECK(!sipfrag_code("SIP/2.0 099 X\r\n", &code));
	CHECK(!sipfrag_code("SIP/2.0 700 X\r\n", &code));
	CHECK(!sipfrag_code("SIP/2.0 20 X\r\n", &code));
	CHECK(!sipfrag_code("SIP/2.0 2000 X\r\n", &code));

	/* m indexed into the last fixture. Clear it before the fixture is freed
	 * so a test added after refer_tests() cannot read freed heap. */
	memset(&m, 0, sizeof(m));
	free(fixture);
	fixture = NULL;
}

static void alert_tests(void)
{
	struct sip_alert_info ai;
	struct sip_val_iter it;

	CASE("BroadSoft Bellcore-dr dialect");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://127.0.0.1/Bellcore-dr2>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_BELLCORE);
	CHECK_UINT(ai.ring, 2);
	CHECK_STR(ai.pattern, "Bellcore-dr2");
	CHECK_STR(ai.url, "http://127.0.0.1/Bellcore-dr2");

	CASE("Bellcore matching ignores case and honours alert_info_prefix");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/bellcore-dr5>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, "Bellcore-dr", &ai));
	CHECK_UINT(ai.ring, 5);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/da3>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, "da", &ai));
	CHECK(ai.dialect == SIP_ALERT_BELLCORE);
	CHECK_UINT(ai.ring, 3);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect != SIP_ALERT_BELLCORE);

	CASE("Sylantro info parameter dialect");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/moo.wav>;info=alert-external;delay=0\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_SYLANTRO);
	CHECK_STR(ai.pattern, "alert-external");
	CHECK_STR(ai.url, "http://host/moo.wav");

	CASE("Sylantro pattern inside the URL");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/alert-internal>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_SYLANTRO);
	CHECK_STR(ai.pattern, "alert-internal");

	CASE("unknown Alert-Info stays usable, and a later value carries the dialect");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/ring.wav>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_OTHER);
	CHECK_STR(ai.url, "http://host/ring.wav");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/ring.wav>, <http://host/Bellcore-dr4>\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_first(&m, NULL, &it, &ai));
	CHECK(ai.dialect == SIP_ALERT_OTHER);
	CHECK(hdr_alert_info_next(&it, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_BELLCORE);
	CHECK_UINT(ai.ring, 4);
	CHECK(!hdr_alert_info_next(&it, NULL, &ai));

	CASE("the one shot returns the first value that parses");
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_OTHER);
	CHECK_STR(ai.url, "http://host/ring.wav");

	CASE("Nortel dst_ring_ carries the digit");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://127.0.0.1/nortel/dst_ring_3>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_NORTEL);
	CHECK_UINT(ai.ring, 3);
	CHECK_STR(ai.pattern, "dst_ring_3");
	CHECK_STR(ai.url, "http://127.0.0.1/nortel/dst_ring_3");

	CASE("the Nortel sentinel is matched without case and without an anchor");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://127.0.0.1/NORTEL/x-DST_RING_9>\r\n\r\n") ==
	      SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_NORTEL);
	CHECK_UINT(ai.ring, 9);

	CASE("Nortel splash_ring and standard_ring are token forms");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://127.0.0.1/nortel/splash_ring>\r\n\r\n") ==
	      SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_NORTEL);
	CHECK_UINT(ai.ring, SIP_ALERT_NORTEL_SPLASH);
	CHECK_STR(ai.pattern, "splash_ring");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://127.0.0.1/nortel/standard_ring>\r\n\r\n") ==
	      SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_NORTEL);
	CHECK_UINT(ai.ring, SIP_ALERT_NORTEL_STANDARD);
	CHECK_STR(ai.pattern, "standard_ring");

	CASE("a Nortel digit out of range falls through to the token forms");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://127.0.0.1/nortel/dst_ring_0/splash_ring>\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_NORTEL);
	CHECK_UINT(ai.ring, SIP_ALERT_NORTEL_SPLASH);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://127.0.0.1/nortel/dst_ring_0>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_OTHER);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://127.0.0.1/nortel/dst_ring_>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_OTHER);

	/*
	 * One find per sentinel. A URL that carries the sentinel
	 * twice is a switch defect, and the first occurrence answers for the
	 * whole value.
	 */
	CASE("the first sentinel occurrence decides and a second is not searched");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://127.0.0.1/nortel/dst_ring_0/dst_ring_3>\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_OTHER);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/Bellcore-dr0/Bellcore-dr3>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_OTHER);

	CASE("the bare alert- token names no Sylantro pattern");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/x.wav>;info=alert-\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_OTHER);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/alert->\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_OTHER);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/alert-x>;info=alert-\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_SYLANTRO);
	CHECK_STR(ai.pattern, "alert-x");

	CASE("the Nortel sentinels are not read from the parameters");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/x.wav>;info=dst_ring_3\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_OTHER);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/x.wav>;info=splash_ring\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_OTHER);

	CASE("the NZ sentinel carries a digit 1 to 4");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://127.0.0.1/da3>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_NZ);
	CHECK_UINT(ai.ring, 3);
	CHECK_STR(ai.pattern, "da3");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://127.0.0.1/da5>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_OTHER);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://127.0.0.1/da0>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_OTHER);

	CASE("the NZ sentinel is anchored, so a file name selects no ring");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/veranda3.wav>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_OTHER);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/monday3.wav>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_OTHER);

	CASE("all five NZ anchor characters and the start of the URL are accepted");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/x?ring=DA2>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_NZ);
	CHECK_UINT(ai.ring, 2);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/ring-da1>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_NZ);
	CHECK_UINT(ai.ring, 1);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nAlert-Info: <da4>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_NZ);
	CHECK_UINT(ai.ring, 4);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/play?da3>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_NZ);
	CHECK_UINT(ai.ring, 3);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/x?a=1&da2>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_NZ);
	CHECK_UINT(ai.ring, 2);

	CASE("an anchored NZ sentinel still needs the digit right behind it");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/x?data=1>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_OTHER);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/x?date=3>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_OTHER);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/x?a=1&day=x>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_OTHER);

	CASE("an unanchored NZ hit does not end the search");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/veranda3.wav/da2>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_NZ);
	CHECK_UINT(ai.ring, 2);

	CASE("the NZ sentinel is not read from the parameters");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/x.wav>;info=da2\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_OTHER);

	CASE("one value is tested Bellcore, Nortel, Sylantro, NZ");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/Bellcore-dr2/dst_ring_3/alert-external/da1>\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_BELLCORE);
	CHECK_UINT(ai.ring, 2);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/dst_ring_3/alert-external/da1>\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_NORTEL);
	CHECK_UINT(ai.ring, 3);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/alert-external/da1>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_SYLANTRO);
	CHECK_STR(ai.pattern, "alert-external");

	CASE("a configured prefix still wins over the fixed sentinels");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/da2>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_get(&m, "da", &ai));
	CHECK(ai.dialect == SIP_ALERT_BELLCORE);
	CHECK_UINT(ai.ring, 2);

	CASE("the walk skips a value that carries no URL");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <>, <http://host/Bellcore-dr4>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_first(&m, NULL, &it, &ai));
	CHECK(ai.dialect == SIP_ALERT_BELLCORE);
	CHECK_UINT(ai.ring, 4);
	CHECK(!hdr_alert_info_next(&it, NULL, &ai));
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nAlert-Info: <>\r\n\r\n") == SIP_PARSE_OK);
	CHECK(!hdr_alert_info_first(&m, NULL, &it, &ai));
	CHECK(ai.dialect == SIP_ALERT_NONE);

	CASE("the walk crosses several Alert-Info headers");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Alert-Info: <http://host/ring.wav>\r\n"
		    "Alert-Info: <http://host/dst_ring_2>, <http://host/da4>\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_alert_info_first(&m, NULL, &it, &ai));
	CHECK(ai.dialect == SIP_ALERT_OTHER);
	CHECK(hdr_alert_info_next(&it, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_NORTEL);
	CHECK_UINT(ai.ring, 2);
	CHECK(hdr_alert_info_next(&it, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_NZ);
	CHECK_UINT(ai.ring, 4);
	CHECK(!hdr_alert_info_next(&it, NULL, &ai));

	CASE("no Alert-Info at all");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nCall-ID: x\r\n\r\n") == SIP_PARSE_OK);
	CHECK(!hdr_alert_info_get(&m, NULL, &ai));
	CHECK(ai.dialect == SIP_ALERT_NONE);
	CHECK(!hdr_alert_info_first(&m, NULL, &it, &ai));
	CHECK(ai.dialect == SIP_ALERT_NONE);
}

static void cid_tests(void)
{
	struct sip_cid cid;
	uint32_t plain = 0;

	CASE("P-Asserted-Identity wins over From");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"From Name\" <sip:5550000@carrier>;tag=1\r\n"
		    "P-Asserted-Identity: \"PAI Name\" <sip:5551111@carrier>\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK(cid.source == SIP_CID_PAI);
	CHECK_STR(cid.name, "PAI Name");
	CHECK_STR(cid.number, "5551111");
	CHECK(!(cid.flags & SIP_CID_F_NUMBER_PRIVATE));

	CASE("two P-Asserted-Identity values, the one with a number wins");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: <sip:x@carrier>;tag=1\r\n"
		    "P-Asserted-Identity: \"Name\" <sip:carrier>\r\n"
		    "P-Asserted-Identity: <tel:+15551234>\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK(cid.source == SIP_CID_PAI);
	CHECK_STR(cid.number, "+15551234");

	CASE("Remote-Party-ID when there is no PAI");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: <sip:x@carrier>;tag=1\r\n"
		    "Remote-Party-ID: \"RP Name\" <sip:5552222@carrier>;party=calling;"
		    "screen=yes;privacy=off\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK(cid.source == SIP_CID_RPID);
	CHECK_STR(cid.number, "5552222");
	CHECK(!(cid.flags & SIP_CID_F_NAME_PRIVATE));

	CASE("Remote-Party-ID privacy parameter");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: <sip:x@carrier>;tag=1\r\n"
		    "Remote-Party-ID: \"RP\" <sip:5552222@carrier>;privacy=full\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK(cid.flags & SIP_CID_F_NAME_PRIVATE);
	CHECK(cid.flags & SIP_CID_F_NUMBER_PRIVATE);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: <sip:x@carrier>;tag=1\r\n"
		    "Remote-Party-ID: \"RP\" <sip:5552222@carrier>;privacy=name\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK(cid.flags & SIP_CID_F_NAME_PRIVATE);
	CHECK(!(cid.flags & SIP_CID_F_NUMBER_PRIVATE));

	CASE("Privacy: id hides the asserted identity");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: <sip:x@carrier>;tag=1\r\n"
		    "P-Asserted-Identity: \"Name\" <sip:5553333@carrier>\r\n"
		    "Privacy: id\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK(cid.flags & SIP_CID_F_NAME_PRIVATE);
	CHECK(cid.flags & SIP_CID_F_NUMBER_PRIVATE);
	CHECK_STR(cid.number, "5553333");

	CASE("From fallback drops a verstat parameter from the number");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"CALLER NAME\" "
		    "<sip:+15551234;verstat=No-TN-Validation@carrier.net>;tag=9\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK(cid.source == SIP_CID_FROM);
	CHECK_STR(cid.number, "+15551234");
	CHECK_STR(cid.name, "CALLER NAME");

	/*
	 * A STIR/SHAKEN gateway repeats the user part in the display name, so
	 * the caller id name arrives as the number plus a verstat parameter.
	 * The number is already on the record, and the parameter is not a
	 * name, so this caller gets no name at all.
	 */
	CASE("a verstat display name equal to the number delivers no name");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"2015550123;verstat=No-TN-Validation\" "
		    "<sip:2015550123;verstat=No-TN-Validation"
		    "@198.51.100.201:5065>;tag=9\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK_STR(cid.number, "2015550123");
	CHECK_UINT(cid.name.len, 0);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"2015550123;verstat=TN-Validation-Passed\" "
		    "<sip:2015550123;verstat=TN-Validation-Passed"
		    "@198.51.100.201>;tag=9\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK_STR(cid.number, "2015550123");
	CHECK_UINT(cid.name.len, 0);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"2015550123;verstat=TN-Validation-Failed\" "
		    "<sip:2015550123;verstat=TN-Validation-Failed"
		    "@198.51.100.201>;tag=9\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK_STR(cid.number, "2015550123");
	CHECK_UINT(cid.name.len, 0);

	CASE("a verstat display name that differs from the number keeps the "
	     "cut");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"5551000;verstat=TN-Validation-Passed\" "
		    "<sip:5559999@carrier.net>;tag=9\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK_STR(cid.number, "5559999");
	CHECK_STR(cid.name, "5551000");

	CASE("a display number loses any parameter behind its first "
	     "semicolon");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"5551000;ext=12\" <sip:5559999@carrier.net>"
		    ";tag=9\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK_STR(cid.number, "5559999");
	CHECK_STR(cid.name, "5551000");

	CASE("a text display name with a semicolon is left whole");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"Smith; John\" <sip:5551234@carrier.net>;tag=9\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK_STR(cid.name, "Smith; John");

	CASE("a text display name loses its verstat parameter and keeps the "
	     "name");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"Test;verstat=No-TN-Validation\" "
		    "<sip:7770001;verstat=No-TN-Validation@172.20.8.10>;"
		    "tag=as0290bedd\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK_STR(cid.number, "7770001");
	CHECK_STR(cid.name, "Test");

	CASE("the verstat cut of a text name ignores case");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"Test;VerStat=No-TN-Validation\" "
		    "<sip:7770001@172.20.8.10>;tag=9\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK_STR(cid.name, "Test");

	CASE("a formatted display number equal to the number delivers no name");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"+1(212)555-1234;verstat=No-TN-Validation\" "
		    "<sip:+12125551234@carrier.net>;tag=9\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK_STR(cid.number, "+12125551234");
	CHECK_UINT(cid.name.len, 0);

	CASE("a display name of digits and no semicolon is never dropped");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"2015550123\" <sip:2015550123@carrier.net>"
		    ";tag=9\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK_STR(cid.name, "2015550123");

	CASE("the plus form and the bare form of one number are the same "
	     "number");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"+15551234;verstat=No-TN-Validation\" "
		    "<sip:15551234@carrier.net>;tag=9\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK_UINT(cid.name.len, 0);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"15551234;verstat=No-TN-Validation\" "
		    "<sip:+15551234@carrier.net>;tag=9\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK_UINT(cid.name.len, 0);

	/* one number is not the other because its digits run out first, in
	 * either direction */
	CASE("a display number that is a prefix of the number is a different "
	     "number");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"201;verstat=No-TN-Validation\" "
		    "<sip:2015550123@carrier.net>;tag=9\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK_STR(cid.number, "2015550123");
	CHECK_STR(cid.name, "201");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"2015550123;verstat=No-TN-Validation\" "
		    "<sip:201@carrier.net>;tag=9\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK_STR(cid.number, "201");
	CHECK_STR(cid.name, "2015550123");

	/* a separator carries no digit, so a display of separators alone is
	 * a text name and keeps the verstat rule */
	CASE("a display of separators and no digit is not a number form");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"-;verstat=No-TN-Validation\" "
		    "<sip:-@carrier.net>;tag=9\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK_STR(cid.number, "-");
	CHECK_STR(cid.name, "-");

	CASE("a P-Asserted-Identity display name takes the same cut");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"Test;verstat=No-TN-Validation\" "
		    "<sip:x@carrier>;tag=1\r\n"
		    "P-Asserted-Identity: \"5551111;verstat=No-TN-Validation\" "
		    "<sip:5551111@carrier>\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK(cid.source == SIP_CID_PAI);
	CHECK_STR(cid.number, "5551111");
	CHECK_UINT(cid.name.len, 0);

	CASE("a Remote-Party-ID display name takes the same cut");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: <sip:x@carrier>;tag=1\r\n"
		    "Remote-Party-ID: \"Test;verstat=No-TN-Validation\" "
		    "<sip:5552222@carrier>"
		    ";party=calling;screen=yes;privacy=off\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK(cid.source == SIP_CID_RPID);
	CHECK_STR(cid.number, "5552222");
	CHECK_STR(cid.name, "Test");

	CASE("anonymous caller");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"Anonymous\" <sip:anonymous@anonymous.invalid>;tag=1\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK(cid.flags & SIP_CID_F_ANONYMOUS);
	CHECK(cid.flags & SIP_CID_F_NUMBER_PRIVATE);
	plain = cid.flags;

	/*
	 * A gateway appends verstat to the anonymous form as well. The
	 * identity is read after the cut, so the caller stays withheld and
	 * the phone never shows the placeholder as a presented identity.
	 */
	CASE("an anonymous caller with verstat in the display name and the "
	     "user part is private");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"Anonymous;verstat=No-TN-Validation\" "
		    "<sip:anonymous;verstat=No-TN-Validation"
		    "@198.51.100.201>;tag=1\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK_UINT(cid.flags, plain);

	CASE("an anonymous user part with verstat and a text display name is "
	     "private");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"Test;verstat=No-TN-Validation\" "
		    "<sip:anonymous;verstat=No-TN-Validation"
		    "@198.51.100.201>;tag=1\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK_UINT(cid.flags, plain);
	CHECK_STR(cid.name, "Test");

	/* the control first: a plain Anonymous display withholds the identity
	 * whatever the user part carries */
	CASE("an Anonymous display name with verstat and a numbered user part "
	     "is private");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"Anonymous\" <sip:5551234@carrier.net>;tag=1\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK_UINT(cid.flags, plain);
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "From: \"Anonymous;verstat=No-TN-Validation\" "
		    "<sip:5551234@carrier.net>;tag=1\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(hdr_cid_get(&m, &cid));
	CHECK_UINT(cid.flags, plain);
	CHECK_STR(cid.number, "5551234");

	CASE("no identity header at all");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nCall-ID: x\r\n\r\n") == SIP_PARSE_OK);
	CHECK(!hdr_cid_get(&m, &cid));
}

/*
 * Eight Service-Route hops whose rendered Route block passes
 * SIP_ROUTE_HDRS_MAX. They are built here because the message is wider than
 * the line width of this file.
 */
static enum sip_parse_status sr_over_bound_parse(void)
{
	char text[2048];
	size_t used;
	unsigned int i;

	used = (size_t)snprintf(text, sizeof(text), "SIP/2.0 200 OK\r\n");
	for (i = 0; i < 8; i++)
		used += (size_t)snprintf(text + used, sizeof(text) - used,
					 "Service-Route: <sip:proxy-%u"
					 ".a-very-long-service-route-hop"
					 ".example.com;lr>\r\n", i);
	snprintf(text + used, sizeof(text) - used, "\r\n");

	return parse(text);
}

static void route_tests(void)
{
	struct sip_route_hdrs r;
	struct sip_route_set set;
	struct sip_buf b;

	CASE("Record-Route collection over headers and commas");
	CHECK(parse("SIP/2.0 200 OK\r\n"
		    "Record-Route: <sip:p1.example.com;lr>, <sip:p2.example.com;lr>\r\n"
		    "Record-Route: <sip:p3.example.com>\r\n"
		    "\r\n") == SIP_PARSE_OK);
	route_set_collect(&m, SIP_HDR_RECORD_ROUTE, &set);
	CHECK_UINT(set.count, 3);
	CHECK(!set.overflow);
	CHECK_STR(set.hop[0], "<sip:p1.example.com;lr>");
	CHECK_STR(set.hop[2], "<sip:p3.example.com>");

	CASE("route set reversal (RFC 3261 12.1.1)");
	route_set_reverse(&set);
	CHECK_STR(set.hop[0], "<sip:p3.example.com>");
	CHECK_STR(set.hop[1], "<sip:p2.example.com;lr>");
	CHECK_STR(set.hop[2], "<sip:p1.example.com;lr>");
	route_set_reverse(&set);
	CHECK_STR(set.hop[0], "<sip:p1.example.com;lr>");

	CASE("loose routing detection");
	CHECK(route_hop_lr(sip_str_cstr("<sip:p1.example.com;lr>")));
	CHECK(route_hop_lr(sip_str_cstr(
		"<sip:services.example.com;lr;unknownwith=value;unknown-no-value>")));
	CHECK(!route_hop_lr(sip_str_cstr("<sip:strict.example.com>")));
	CHECK(!route_hop_lr(sip_str_cstr("sip:strict.example.com;lr")));

	CASE("route set overflow is reported");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\n"
		    "Route: <sip:1>,<sip:2>,<sip:3>,<sip:4>,<sip:5>,<sip:6>,"
		    "<sip:7>,<sip:8>,<sip:9>,<sip:10>,<sip:11>\r\n"
		    "\r\n") == SIP_PARSE_OK);
	route_set_collect(&m, SIP_HDR_ROUTE, &set);
	CHECK(set.overflow);
	CHECK_UINT(set.count, SIP_ROUTE_MAX);

	CASE("no route set");
	CHECK(parse("INVITE sip:a@b SIP/2.0\r\nCall-ID: x\r\n\r\n") == SIP_PARSE_OK);
	route_set_collect(&m, SIP_HDR_ROUTE, &set);
	CHECK_UINT(set.count, 0);

	CASE("Service-Route collection over headers and commas");
	CHECK(parse("SIP/2.0 200 OK\r\n"
		    "Service-Route: <sip:p1.example.com;lr>, "
		    "<sip:p2.example.com;lr>\r\n"
		    "Service-Route: <sip:p3.example.com;lr>\r\n"
		    "\r\n") == SIP_PARSE_OK);
	route_set_collect(&m, SIP_HDR_SERVICE_ROUTE, &set);
	CHECK_UINT(set.count, 3);
	CHECK(!set.overflow);
	CHECK_STR(set.hop[0], "<sip:p1.example.com;lr>");
	CHECK_STR(set.hop[2], "<sip:p3.example.com;lr>");
	route_set_collect(&m, SIP_HDR_ROUTE, &set);
	CHECK_UINT(set.count, 0);

	CASE("the service route renders one Route header per hop, in order");
	CHECK(route_hdrs_take(&r, &m, SIP_HDR_SERVICE_ROUTE));
	CHECK_UINT(r.hops, 3);
	CHECK(!strcmp(r.text, "Route: <sip:p1.example.com;lr>\r\n"
			      "Route: <sip:p2.example.com;lr>\r\n"
			      "Route: <sip:p3.example.com;lr>\r\n"));
	CHECK_UINT(r.len, strlen(r.text));
	sip_buf_init(&b, out, sizeof(out));
	CHECK(route_hdrs_write(&b, &r));
	CHECK(!strcmp(out, r.text));
	route_hdrs_clear(&r);
	CHECK_UINT(r.len, 0);
	CHECK_UINT(r.hops, 0);

	CASE("a service route with no loose first hop is refused whole");
	CHECK(parse("SIP/2.0 200 OK\r\n"
		    "Service-Route: <sip:p1.example.com>, "
		    "<sip:p2.example.com;lr>\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(!route_hdrs_take(&r, &m, SIP_HDR_SERVICE_ROUTE));
	CHECK_UINT(r.len, 0);
	CHECK_UINT(r.hops, 0);
	CHECK_UINT(r.text[0], 0);

	CASE("a bracket-less service route hop is a strict hop and is refused");
	CHECK(parse("SIP/2.0 200 OK\r\n"
		    "Service-Route: sip:p1.example.com;lr\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(!route_hdrs_take(&r, &m, SIP_HDR_SERVICE_ROUTE));
	CHECK_UINT(r.len, 0);
	CHECK_UINT(r.hops, 0);

	/* RFC 3261 12.2.1.1 rewrites the request only for a strict FIRST hop,
	 * so a strict hop behind a loose one is a proxy's own business. */
	CASE("a strict hop behind a loose first hop is kept");
	CHECK(parse("SIP/2.0 200 OK\r\n"
		    "Service-Route: <sip:p1.example.com;lr>, "
		    "<sip:p2.example.com>\r\n"
		    "\r\n") == SIP_PARSE_OK);
	CHECK(route_hdrs_take(&r, &m, SIP_HDR_SERVICE_ROUTE));
	CHECK_UINT(r.hops, 2);
	CHECK(!strcmp(r.text, "Route: <sip:p1.example.com;lr>\r\n"
			      "Route: <sip:p2.example.com>\r\n"));

	CASE("a bare CR in a service route hop refuses the route");
	CHECK(parse("SIP/2.0 200 OK\r\n"
		    "Service-Route: <sip:p1.example.com;lr>"
		    "\rRoute: <sip:evil.example.com;lr>\r\n"
		    "\r\n") == SIP_PARSE_OK);
	route_set_collect(&m, SIP_HDR_SERVICE_ROUTE, &set);
	CHECK_UINT(set.count, 1);
	CHECK(!route_hdrs_take(&r, &m, SIP_HDR_SERVICE_ROUTE));
	CHECK_UINT(r.len, 0);
	CHECK_UINT(r.hops, 0);
	CHECK_UINT(r.text[0], 0);

	CASE("a bare CR in a later service route hop refuses the route");
	CHECK(parse("SIP/2.0 200 OK\r\n"
		    "Service-Route: <sip:p1.example.com;lr>, "
		    "<sip:p2.example.com;lr>"
		    "\rRoute: <sip:evil.example.com;lr>\r\n"
		    "\r\n") == SIP_PARSE_OK);
	route_set_collect(&m, SIP_HDR_SERVICE_ROUTE, &set);
	CHECK_UINT(set.count, 2);
	CHECK(!route_hdrs_take(&r, &m, SIP_HDR_SERVICE_ROUTE));
	CHECK_UINT(r.len, 0);
	CHECK_UINT(r.hops, 0);

	CASE("a NUL in a service route hop refuses the route");
	{
		static const char nul_msg[] =
			"SIP/2.0 200 OK\r\n"
			"Service-Route: <sip:p1.example.com;lr>;a=\0b, "
			"<sip:p2.example.com;lr>\r\n"
			"\r\n";
		size_t len = sizeof(nul_msg) - 1;

		memcpy(buf, nul_msg, len);
		CHECK(msg_parse(&m, buf, len) == SIP_PARSE_OK);
		route_set_collect(&m, SIP_HDR_SERVICE_ROUTE, &set);
		CHECK_UINT(set.count, 2);
		CHECK(!route_hdrs_take(&r, &m, SIP_HDR_SERVICE_ROUTE));
		CHECK_UINT(r.len, 0);
		CHECK_UINT(r.hops, 0);
		CHECK_UINT(r.text[0], 0);
	}

	CASE("a service route past the header block bound is refused, "
	     "never truncated");
	CHECK(sr_over_bound_parse() == SIP_PARSE_OK);
	route_set_collect(&m, SIP_HDR_SERVICE_ROUTE, &set);
	CHECK_UINT(set.count, 8);
	CHECK(!route_hdrs_take(&r, &m, SIP_HDR_SERVICE_ROUTE));
	CHECK_UINT(r.len, 0);
	CHECK_UINT(r.hops, 0);
	CHECK_UINT(r.text[0], 0);

	CASE("a message with no Service-Route leaves the block empty");
	CHECK(parse("SIP/2.0 200 OK\r\nCall-ID: x\r\n\r\n") == SIP_PARSE_OK);
	CHECK(route_hdrs_take(&r, &m, SIP_HDR_SERVICE_ROUTE));
	CHECK_UINT(r.len, 0);
	CHECK_UINT(r.hops, 0);
	sip_buf_init(&b, out, sizeof(out));
	CHECK(route_hdrs_write(&b, &r));
	CHECK(route_hdrs_write(&b, NULL));
	CHECK_UINT(sip_buf_len(&b), 0);
}

static const char invite_req[] =
	"INVITE sip:5551000@10.0.0.5 SIP/2.0\r\n"
	"Via: SIP/2.0/UDP 203.0.113.9:5060;branch=z9hG4bKfar\r\n"
	"Via: SIP/2.0/UDP 198.51.100.7:5060;branch=z9hG4bKnear\r\n"
	"Record-Route: <sip:p1.example.com;lr>\r\n"
	"From: \"Caller\" <sip:5559999@carrier>;tag=remote-1\r\n"
	"To: <sip:5551000@10.0.0.5>\r\n"
	"Call-ID: build-me@carrier\r\n"
	"CSeq: 7 INVITE\r\n"
	"Content-Length: 0\r\n"
	"\r\n";

/* the header set of a field capture, with no body: what the
 * response builder reads from the request */
static const char field_invite_req[] =
	"INVITE sip:oRYOOqsGFzQpUt2dOjeCRZ0@192.168.210.10:5065;transport=UDP"
	" SIP/2.0\r\n"
	"Via: SIP/2.0/UDP 198.51.100.201:5065"
	";branch=z9hG4bKpiv8v526alpi793r0927.5\r\n"
	"Contact: <sip:198.51.100.201:5065;transport=udp>\r\n"
	"CSeq: 22979 INVITE\r\n"
	"Content-Length: 0\r\n"
	"Call-ID: IJvv2uh23-1e834fc41fdc766ac355cc82bef42afa-ga8x0f3\r\n"
	"From: \"2015550123;verstat=No-TN-Validation\""
	" <sip:2015550123;verstat=No-TN-Validation@198.51.100.201:5065>"
	";tag=IJvv2uh23-L77J20260101000000N3706433\r\n"
	"To: sip:oRYOOqsGFzQpUt2dOjeCRZ0@203.0.113.251:10311;transport=udp\r\n"
	"Session-ID: 8dcc4b8f4aa366485628a232f8e358114d18792f\r\n"
	"Max-Forwards: 45\r\n"
	"Session-Expires: 1800;refresher=uac\r\n"
	"Supported: 100rel\r\n"
	"Supported: timer\r\n"
	"\r\n";

static void builder_tests(void)
{
	struct sip_buf b;
	struct sip_msg resp;
	enum sip_method method;
	uint32_t rseq, cseq;
	char tight[16];
	static char rbuf[2048];

	CASE("bracket: an IPv6 literal needs brackets");
	CHECK(sip_host_bracket_needed("2001:db8::1"));

	CASE("bracket: an already bracketed literal does not");
	CHECK(!sip_host_bracket_needed("[2001:db8::1]"));

	CASE("bracket: a name and an IPv4 literal do not");
	CHECK(!sip_host_bracket_needed("sip.example.com"));
	CHECK(!sip_host_bracket_needed("192.0.2.1"));

	CASE("bracket: NULL is not a host");
	CHECK(!sip_host_bracket_needed(NULL));

	CASE("request line and status line");
	sip_buf_init(&b, out, sizeof(out));
	CHECK(msg_req_line_write(&b, sip_method_name(SIP_METHOD_REGISTER),
				 "sip:voip.example.net"));
	CHECK(!strcmp(out, "REGISTER sip:voip.example.net SIP/2.0\r\n"));
	sip_buf_init(&b, out, sizeof(out));
	CHECK(msg_status_line_write(&b, 486, NULL));
	CHECK(!strcmp(out, "SIP/2.0 486 Busy Here\r\n"));
	sip_buf_init(&b, out, sizeof(out));
	CHECK(msg_status_line_write(&b, 199, "Early Dialog Terminated"));
	CHECK(!strcmp(out, "SIP/2.0 199 Early Dialog Terminated\r\n"));
	CHECK(!strcmp(sip_status_reason(420), "Bad Extension"));
	CHECK(!strcmp(sip_status_reason(422), "Session Interval Too Small"));
	CHECK(!strcmp(sip_status_reason(699), "Global Failure"));

	CASE("Via, CSeq and address writers use the long forms");
	sip_buf_init(&b, out, sizeof(out));
	CHECK(hdr_via_write(&b, "UDP", "10.0.0.5", 5060, "z9hG4bKuvoice1", true));
	CHECK(hdr_cseq_write(&b, 4, sip_method_name(SIP_METHOD_REGISTER)));
	CHECK(hdr_addr_write(&b, SIP_HDR_FROM, "Line 1", "sip:5551000@voip.example.net",
			     ";tag=uv1"));
	CHECK(hdr_addr_write(&b, SIP_HDR_TO, NULL, "sip:5551000@voip.example.net", NULL));
	CHECK(!strcmp(out,
		      "Via: SIP/2.0/UDP 10.0.0.5:5060;branch=z9hG4bKuvoice1;rport\r\n"
		      "CSeq: 4 REGISTER\r\n"
		      "From: \"Line 1\" <sip:5551000@voip.example.net>;tag=uv1\r\n"
		      "To: <sip:5551000@voip.example.net>\r\n"));

	CASE("the RAck writer and the RAck parser agree (RFC 3262 7.2)");
	sip_buf_init(&b, out, sizeof(out));
	CHECK(msg_req_line_write(&b, sip_method_name(SIP_METHOD_PRACK),
				 "sip:a@b"));
	CHECK(hdr_rack_write(&b, 4294967295u, 2147483647u, "INVITE"));
	CHECK(msg_body_write(&b, NULL, NULL, 0));
	CHECK(!strcmp(out, "PRACK sip:a@b SIP/2.0\r\n"
			   "RAck: 4294967295 2147483647 INVITE\r\n"
			   "Content-Length: 0\r\n\r\n"));
	memcpy(rbuf, out, strlen(out));
	CHECK(msg_parse(&resp, rbuf, strlen(out)) == SIP_PARSE_OK);
	CHECK(hdr_rack_get(&resp, &rseq, &cseq, &method));
	CHECK_UINT(rseq, 4294967295u);
	CHECK_UINT(cseq, 2147483647u);
	CHECK(method == SIP_METHOD_INVITE);

	CASE("the RAck writer copies the method token it is given");
	sip_buf_init(&b, out, sizeof(out));
	CHECK(hdr_rack_write(&b, 1, 2, "SUBSCRIBE"));
	CHECK(!strcmp(out, "RAck: 1 2 SUBSCRIBE\r\n"));

	CASE("a RAck that does not fit leaves the buffer in overflow");
	sip_buf_init(&b, tight, sizeof(tight));
	CHECK(!hdr_rack_write(&b, 4294967295u, 2147483647u, "INVITE"));
	CHECK(!sip_buf_ok(&b));

	CASE("Via writer brackets a bare IPv6 host");
	sip_buf_init(&b, out, sizeof(out));
	CHECK(hdr_via_write(&b, "UDP", "2001:db8::5", 0, "z9hG4bK6", false));
	CHECK(!strcmp(out, "Via: SIP/2.0/UDP [2001:db8::5];branch=z9hG4bK6\r\n"));

	CASE("Via writer keeps an already bracketed IPv6 host");
	sip_buf_init(&b, out, sizeof(out));
	CHECK(hdr_via_write(&b, "UDP", "[2001:db8::7]", 5060, NULL, false));
	CHECK(!strcmp(out, "Via: SIP/2.0/UDP [2001:db8::7]:5060\r\n"));

	CASE("body writer emits Content-Type and Content-Length");
	sip_buf_init(&b, out, sizeof(out));
	CHECK(msg_body_write(&b, "application/sdp", "v=0\r\n", 5));
	CHECK(!strcmp(out, "Content-Type: application/sdp\r\n"
			   "Content-Length: 5\r\n\r\nv=0\r\n"));
	sip_buf_init(&b, out, sizeof(out));
	CHECK(msg_body_write(&b, NULL, NULL, 0));
	CHECK(!strcmp(out, "Content-Length: 0\r\n\r\n"));

	CASE("response from a request copies the Via chain and adds a To tag");
	CHECK(parse(invite_req) == SIP_PARSE_OK);
	sip_buf_init(&b, out, sizeof(out));
	CHECK(msg_response_build(&b, &m, 180, NULL, "uvoice-tag-1", 0));
	CHECK(msg_body_write(&b, NULL, NULL, 0));
	memcpy(rbuf, out, strlen(out));
	CHECK(msg_parse(&resp, rbuf, strlen(out)) == SIP_PARSE_OK);
	CHECK_UINT(resp.status, 180);
	CHECK_UINT(hdr_via_count(&resp), 2);
	CHECK_UINT(msg_hdr_count(&resp, SIP_HDR_RECORD_ROUTE), 0);
	{
		struct sip_addr to;
		struct sip_str v;

		CHECK(hdr_addr_get(&resp, SIP_HDR_TO, &to));
		CHECK_STR(to.tag, "uvoice-tag-1");
		CHECK(hdr_addr_get(&resp, SIP_HDR_FROM, &to));
		CHECK_STR(to.tag, "remote-1");
		CHECK(hdr_call_id_get(&resp, &v));
		CHECK_STR(v, "build-me@carrier");
		CHECK(msg_hdr_value(&resp, SIP_HDR_CSEQ, &v));
		CHECK_STR(v, "7 INVITE");
	}

	CASE("a 100 gets no To tag, a 2xx can carry Record-Route");
	sip_buf_init(&b, out, sizeof(out));
	CHECK(msg_response_build(&b, &m, 100, NULL, "uvoice-tag-1", 0));
	CHECK(!strstr(out, "tag=uvoice-tag-1"));
	sip_buf_init(&b, out, sizeof(out));
	CHECK(msg_response_build(&b, &m, 200, NULL, "uvoice-tag-1",
				 SIP_RESP_F_RECORD_ROUTE));
	CHECK(strstr(out, "Record-Route: <sip:p1.example.com;lr>\r\n") != NULL);
	CHECK(strstr(out, "tag=uvoice-tag-1") != NULL);

	CASE("an existing To tag is not replaced");
	CHECK(parse("BYE sip:a@b SIP/2.0\r\n"
		    "Via: SIP/2.0/UDP 10.0.0.1;branch=z9hG4bK1\r\n"
		    "From: <sip:a@b>;tag=f1\r\n"
		    "To: <sip:c@d>;tag=t1\r\n"
		    "Call-ID: x\r\n"
		    "CSeq: 2 BYE\r\n\r\n") == SIP_PARSE_OK);
	sip_buf_init(&b, out, sizeof(out));
	CHECK(msg_response_build(&b, &m, 200, NULL, "should-not-appear", 0));
	CHECK(strstr(out, "To: <sip:c@d>;tag=t1\r\n") != NULL);
	CHECK(!strstr(out, "should-not-appear"));

	CASE("a To without angle brackets keeps its header parameter and "
	     "gains the tag");
	CHECK(parse(field_invite_req) == SIP_PARSE_OK);
	sip_buf_init(&b, out, sizeof(out));
	CHECK(msg_response_build(&b, &m, 180, NULL, "uvoice-tag-1", 0));
	CHECK(strstr(out, "To: sip:oRYOOqsGFzQpUt2dOjeCRZ0@203.0.113.251:10311"
			  ";transport=udp;tag=uvoice-tag-1\r\n") != NULL);

	CASE("a From with verstat in the display name and in the URI is "
	     "echoed byte for byte");
	CHECK(parse(field_invite_req) == SIP_PARSE_OK);
	sip_buf_init(&b, out, sizeof(out));
	CHECK(msg_response_build(&b, &m, 180, NULL, "uvoice-tag-1", 0));
	CHECK(strstr(out, "From: \"2015550123;verstat=No-TN-Validation\""
			  " <sip:2015550123;verstat=No-TN-Validation"
			  "@198.51.100.201:5065>"
			  ";tag=IJvv2uh23-L77J20260101000000N3706433\r\n") != NULL);

	CASE("a response cannot be built from a response");
	CHECK(parse("SIP/2.0 200 OK\r\nCall-ID: x\r\n\r\n") == SIP_PARSE_OK);
	sip_buf_init(&b, out, sizeof(out));
	CHECK(!msg_response_build(&b, &m, 200, NULL, NULL, 0));

	CASE("a request without Via cannot be answered (RFC 3261 8.2.6.2)");
	CHECK(parse("OPTIONS sip:a@b SIP/2.0\r\n"
		    "From: <sip:a@b>;tag=f1\r\n"
		    "To: <sip:c@d>\r\n"
		    "Call-ID: no-via\r\n"
		    "CSeq: 1 OPTIONS\r\n\r\n") == SIP_PARSE_OK);
	sip_buf_init(&b, out, sizeof(out));
	CHECK(!msg_response_build(&b, &m, 200, NULL, "t1", 0));
	CHECK_UINT(sip_buf_len(&b), 0);

	CASE("a request missing From, Call-ID or CSeq cannot be answered");
	CHECK(parse("OPTIONS sip:a@b SIP/2.0\r\n"
		    "Via: SIP/2.0/UDP 10.0.0.1;branch=z9hG4bK1\r\n"
		    "To: <sip:c@d>\r\n"
		    "Call-ID: no-from\r\n"
		    "CSeq: 1 OPTIONS\r\n\r\n") == SIP_PARSE_OK);
	sip_buf_init(&b, out, sizeof(out));
	CHECK(!msg_response_build(&b, &m, 200, NULL, "t1", 0));
	CHECK_UINT(sip_buf_len(&b), 0);

	CASE("above 100 the To tag is required, not optional");
	CHECK(parse(invite_req) == SIP_PARSE_OK);
	sip_buf_init(&b, out, sizeof(out));
	CHECK(!msg_response_build(&b, &m, 180, NULL, NULL, 0));
	CHECK_UINT(sip_buf_len(&b), 0);
	sip_buf_init(&b, out, sizeof(out));
	CHECK(!msg_response_build(&b, &m, 180, NULL, "", 0));
	sip_buf_init(&b, out, sizeof(out));
	CHECK(msg_response_build(&b, &m, 100, NULL, NULL, 0));
	CHECK(strstr(out, "To: <sip:5551000@10.0.0.5>\r\n") != NULL);

	CASE("a length near SIZE_MAX cannot wrap the capacity check");
	{
		char small[32];

		sip_buf_init(&b, small, sizeof(small));
		CHECK(!sip_buf_add_len(&b, "x", SIZE_MAX));
		CHECK(!sip_buf_ok(&b));
		sip_buf_init(&b, small, sizeof(small));
		CHECK(!sip_buf_add_len(&b, "x", SIZE_MAX - sizeof(small)));
		CHECK(!sip_buf_ok(&b));
		sip_buf_init(&b, small, sizeof(small));
		CHECK(!sip_buf_add_len(&b, "x", sizeof(small)));
		CHECK(!sip_buf_ok(&b));
	}

	CASE("the buffer fills to exactly size minus the terminator");
	{
		char small[8];

		sip_buf_init(&b, small, sizeof(small));
		CHECK(sip_buf_add(&b, "1234567"));
		CHECK(sip_buf_ok(&b));
		CHECK_UINT(sip_buf_len(&b), 7);
		CHECK(!strcmp(small, "1234567"));
		CHECK(!sip_buf_add(&b, "8"));
		CHECK(!sip_buf_ok(&b));
	}

	CASE("buffer overflow is sticky and never truncates silently");
	{
		char small[32];
		size_t at;

		sip_buf_init(&b, small, sizeof(small));
		CHECK(!msg_req_line_write(&b, "INVITE",
					  "sip:a-very-long-uri@example.invalid"));
		CHECK(!sip_buf_ok(&b));
		CHECK(sip_buf_str(&b) == NULL);
		at = sip_buf_len(&b);
		CHECK(!sip_buf_add(&b, "x"));
		CHECK_UINT(sip_buf_len(&b), at);
		sip_buf_init(&b, small, 0);
		CHECK(!sip_buf_ok(&b));
	}
}

static void addr_tag_tests(void)
{
	static const char head[] = "To: <sip:a@b>;tag=";
	static char tag[301];
	struct sip_buf b;
	char part[24];

	CASE("address tag: a tag of 300 characters is written whole");
	memset(tag, 't', sizeof(tag) - 1);
	sip_buf_init(&b, out, sizeof(out));
	CHECK(hdr_addr_tag_write(&b, SIP_HDR_TO, NULL, "sip:a@b", tag));
	CHECK_UINT(sip_buf_len(&b), strlen(head) + strlen(tag) + 2);
	CHECK(!strncmp(out, head, strlen(head)));
	CHECK(!strncmp(out + strlen(head), tag, strlen(tag)));

	CASE("address tag: a NULL tag writes no parameter");
	sip_buf_init(&b, out, sizeof(out));
	CHECK(hdr_addr_tag_write(&b, SIP_HDR_TO, NULL, "sip:a@b", NULL));
	CHECK(!strcmp(out, "To: <sip:a@b>\r\n"));

	CASE("address tag: an empty tag writes ;tag= with no value");
	sip_buf_init(&b, out, sizeof(out));
	CHECK(hdr_addr_tag_write(&b, SIP_HDR_FROM, "A \"b\"", "sip:a@b", ""));
	CHECK(!strcmp(out, "From: \"A \\\"b\\\"\" <sip:a@b>;tag=\r\n"));

	CASE("address tag: a full buffer keeps ;tag= and refuses the tag");
	sip_buf_init(&b, part, strlen("From: <sip:a@b>;tag=f1"));
	CHECK(!hdr_addr_tag_write(&b, SIP_HDR_FROM, NULL, "sip:a@b", "f1"));
	CHECK(!sip_buf_ok(&b));
	CHECK_UINT(sip_buf_len(&b), strlen("From: <sip:a@b>;tag="));
	CHECK(!strcmp(part, "From: <sip:a@b>;tag="));
}

static void user_escape_tests(void)
{
	char out[32];
	char tight[8];

	CASE("user escape: no mask passes the dial string through");
	CHECK(uri_user_escape(out, sizeof(out), "*84#", 0));
	CHECK(!strcmp(out, "*84#"));

	CASE("user escape: star and pound both hide under the full mask");
	CHECK(uri_user_escape(out, sizeof(out), "*84#",
			      SIP_URI_ESC_STAR | SIP_URI_ESC_POUND));
	CHECK(!strcmp(out, "%2A84%23"));

	CASE("user escape: the pound mask leaves the star alone");
	CHECK(uri_user_escape(out, sizeof(out), "*84#", SIP_URI_ESC_POUND));
	CHECK(!strcmp(out, "*84%23"));

	CASE("user escape: every other byte passes through under the full mask");
	CHECK(uri_user_escape(out, sizeof(out), "1a-_.!~",
			      SIP_URI_ESC_STAR | SIP_URI_ESC_POUND));
	CHECK(!strcmp(out, "1a-_.!~"));

	CASE("user escape: a literal that does not fit is refused and cleared");
	CHECK(!uri_user_escape(tight, 4, "12345", 0));
	CHECK(tight[0] == 0);

	CASE("user escape: an escape that does not fit is refused and cleared");
	CHECK(!uri_user_escape(tight, sizeof(tight), "12345#",
			       SIP_URI_ESC_POUND));
	CHECK(tight[0] == 0);

	CASE("user escape: NULL and empty write an empty string");
	CHECK(uri_user_escape(out, sizeof(out), NULL, SIP_URI_ESC_POUND));
	CHECK(out[0] == 0);
	CHECK(uri_user_escape(out, sizeof(out), "", 0));
	CHECK(out[0] == 0);

	CASE("user escape: a zero length or NULL buffer is refused");
	CHECK(!uri_user_escape(out, 0, "5", 0));
	CHECK(!uri_user_escape(NULL, sizeof(out), "5", 0));
}

int main(void)
{
	uri_tests();
	addr_tests();
	via_tests();
	simple_hdr_tests();
	token_tests();
	auth_tests();
	event_tests();
	refer_tests();
	user_escape_tests();
	alert_tests();
	cid_tests();
	route_tests();
	builder_tests();
	addr_tag_tests();

	return tu_summary("test_sip_hdr");
}
