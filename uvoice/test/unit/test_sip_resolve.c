// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the DNS wire layer and the resolver state machine of
 * src/proto/shared/resolve.c.
 *
 * The suite builds every packet byte by byte. A case therefore states exactly
 * what is on the wire, and a hostile packet is as easy to write as a well
 * formed one.
 *
 * The wire layer cases cover the two parts that face untrusted input, the name
 * decoder and the record walk. They also cover the RFC 2782 ordering and the
 * resolv.conf reader.
 *
 * The endpoint harness drives the resolver state machine. It replaces the
 * resolver transport with a scripted transport and a virtual clock, so no case
 * opens a socket or runs uloop. The live socket path needs a real socket and
 * uloop, and the daemon exercises it.
 *
 * The alarm in main() is part of the contract: a compression loop that hangs
 * must fail the suite, not stall it.
 */
#include <arpa/inet.h>
#include <stdlib.h>
#include <unistd.h>

#include "proto/shared/resolve.h"
#include "proto/sip/txn.h"
#include "proto_host_fake.h"
#include "test_util.h"

#define WIRE_MAX	1024

struct wire {
	uint8_t b[WIRE_MAX];
	size_t len;
};

static void w_reset(struct wire *w)
{
	memset(w, 0, sizeof(*w));
}

static void w_u8(struct wire *w, uint8_t v)
{
	if (w->len < WIRE_MAX)
		w->b[w->len++] = v;
}

static void w_u16(struct wire *w, uint16_t v)
{
	w_u8(w, (uint8_t)(v >> 8));
	w_u8(w, (uint8_t)v);
}

static void w_u32(struct wire *w, uint32_t v)
{
	w_u16(w, (uint16_t)(v >> 16));
	w_u16(w, (uint16_t)v);
}

static void w_bytes(struct wire *w, const void *p, size_t n)
{
	const uint8_t *b = p;
	size_t i;

	for (i = 0; i < n; i++)
		w_u8(w, b[i]);
}

/* an uncompressed name, "" is the root */
static void w_name(struct wire *w, const char *name)
{
	const char *p = name;

	while (p && *p) {
		const char *dot = strchr(p, '.');
		size_t l = dot ? (size_t)(dot - p) : strlen(p);

		w_u8(w, (uint8_t)l);
		w_bytes(w, p, l);
		if (!dot)
			break;

		p = dot + 1;
	}

	w_u8(w, 0);
}

static void w_ptr(struct wire *w, uint16_t off)
{
	w_u16(w, (uint16_t)(0xc000u | off));
}

static void w_hdr(struct wire *w, uint16_t id, uint16_t flags, uint16_t qd, uint16_t an,
		  uint16_t ns, uint16_t ar)
{
	w_u16(w, id);
	w_u16(w, flags);
	w_u16(w, qd);
	w_u16(w, an);
	w_u16(w, ns);
	w_u16(w, ar);
}

static void w_question(struct wire *w, const char *name, uint16_t type)
{
	w_name(w, name);
	w_u16(w, type);
	w_u16(w, DNS_CLASS_IN);
}

/* name, type, class, ttl and a zero rdlength; returns the rdlength offset */
static size_t w_rr_head(struct wire *w, const char *name, uint16_t type, uint32_t ttl)
{
	size_t off;

	w_name(w, name);
	w_u16(w, type);
	w_u16(w, DNS_CLASS_IN);
	w_u32(w, ttl);
	off = w->len;
	w_u16(w, 0);

	return off;
}

static void w_rr_end(struct wire *w, size_t rdlen_off)
{
	size_t rdlen = w->len - (rdlen_off + 2);

	w->b[rdlen_off] = (uint8_t)(rdlen >> 8);
	w->b[rdlen_off + 1] = (uint8_t)rdlen;
}

static void w_rr_a(struct wire *w, const char *name, const char *addr, uint32_t ttl)
{
	size_t off = w_rr_head(w, name, DNS_T_A, ttl);
	struct in_addr a;

	inet_pton(AF_INET, addr, &a);
	w_bytes(w, &a, 4);
	w_rr_end(w, off);
}

static void w_rr_aaaa(struct wire *w, const char *name, const char *addr, uint32_t ttl)
{
	size_t off = w_rr_head(w, name, DNS_T_AAAA, ttl);
	struct in6_addr a;

	inet_pton(AF_INET6, addr, &a);
	w_bytes(w, &a, 16);
	w_rr_end(w, off);
}

static void w_rr_cname(struct wire *w, const char *name, const char *target, uint32_t ttl)
{
	size_t off = w_rr_head(w, name, DNS_T_CNAME, ttl);

	w_name(w, target);
	w_rr_end(w, off);
}

static void w_rr_srv(struct wire *w, const char *name, uint16_t prio, uint16_t weight,
		     uint16_t port, const char *target, uint32_t ttl)
{
	size_t off = w_rr_head(w, name, DNS_T_SRV, ttl);

	w_u16(w, prio);
	w_u16(w, weight);
	w_u16(w, port);
	w_name(w, target);
	w_rr_end(w, off);
}

/* ------------------------------------------------------------- name write */

static void test_name_write(void)
{
	static const uint8_t want[] = { 1, 'a', 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
					3, 'c', 'o', 'm', 0 };
	char big[600];
	uint8_t buf[512];
	int n;

	CASE("name write: a.example.com");
	n = dns_name_write(buf, sizeof(buf), 0, "a.example.com");
	CHECK_UINT(n, sizeof(want));
	CHECK(!memcmp(buf, want, sizeof(want)));

	CASE("name write: the root is one zero byte");
	n = dns_name_write(buf, sizeof(buf), 0, "");
	CHECK_UINT(n, 1);
	CHECK_UINT(buf[0], 0);

	CASE("name write: a trailing dot is the same name");
	CHECK_UINT(dns_name_write(buf, sizeof(buf), 0, "example.com."), 13);
	CHECK_UINT(buf[0], 7);

	CASE("name write: an empty label is refused");
	CHECK(dns_name_write(buf, sizeof(buf), 0, "a..b") < 0);
	CHECK(dns_name_write(buf, sizeof(buf), 0, ".a") < 0);

	CASE("name write: a label over 63 bytes is refused");
	memset(big, 'x', 64);
	big[64] = 0;
	CHECK(dns_name_write(buf, sizeof(buf), 0, big) < 0);

	CASE("name write: a short buffer is refused, never truncated");
	CHECK(dns_name_write(buf, 5, 0, "a.example.com") < 0);
	CHECK(dns_name_write(buf, 1, 0, "a") < 0);

	CASE("name write: a name over 255 wire bytes is refused");
	memset(big, 0, sizeof(big));
	for (n = 0; n < 5; n++) {
		memset(big + n * 64, 'a', 63);
		big[n * 64 + 63] = '.';
	}
	big[5 * 64 - 1] = 0;
	CHECK(dns_name_write(buf, sizeof(buf), 0, big) < 0);

	CASE("name write: an offset inside the buffer is honoured");
	memset(buf, 0xee, sizeof(buf));
	n = dns_name_write(buf, sizeof(buf), 12, "a");
	CHECK_UINT(n, 3);
	CHECK_UINT(buf[11], 0xee);
	CHECK_UINT(buf[12], 1);
	CHECK_UINT(buf[13], 'a');
	CHECK_UINT(buf[14], 0);
}

/* -------------------------------------------------------------- name read */

static void test_name_read(void)
{
	char out[SIP_DNS_NAME_MAX];
	struct wire w;
	size_t next;

	CASE("name read: a plain name and the offset behind it");
	w_reset(&w);
	w_name(&w, "sip.example.com");
	w_u8(&w, 0xaa);
	next = 0;
	CHECK(dns_name_read(w.b, w.len, 0, out, sizeof(out), &next) == 0);
	CHECK(!strcmp(out, "sip.example.com"));
	CHECK_UINT(next, w.len - 1);

	CASE("name read: the root is the empty string");
	w_reset(&w);
	w_name(&w, "");
	CHECK(dns_name_read(w.b, w.len, 0, out, sizeof(out), &next) == 0);
	CHECK_UINT(strlen(out), 0);
	CHECK_UINT(next, 1);

	CASE("name read: a backward pointer ends the encoded name after 2 bytes");
	w_reset(&w);
	w_name(&w, "example.com");		/* at 0 */
	w_u8(&w, 3);				/* at 13: "sip" */
	w_bytes(&w, "sip", 3);
	w_ptr(&w, 0);
	CHECK(dns_name_read(w.b, w.len, 13, out, sizeof(out), &next) == 0);
	CHECK(!strcmp(out, "sip.example.com"));
	CHECK_UINT(next, 13 + 4 + 2);

	CASE("name read: a pointer to itself is refused");
	w_reset(&w);
	w_ptr(&w, 0);
	CHECK(dns_name_read(w.b, w.len, 0, out, sizeof(out), &next) < 0);

	CASE("name read: a forward pointer is refused");
	w_reset(&w);
	w_ptr(&w, 4);
	w_u16(&w, 0);
	w_name(&w, "a");
	CHECK(dns_name_read(w.b, w.len, 0, out, sizeof(out), &next) < 0);

	CASE("name read: a two pointer loop is refused");
	w_reset(&w);
	w_ptr(&w, 2);				/* 0 -> 2 */
	w_ptr(&w, 0);				/* 2 -> 0 */
	CHECK(dns_name_read(w.b, w.len, 0, out, sizeof(out), &next) < 0);
	CHECK(dns_name_read(w.b, w.len, 2, out, sizeof(out), &next) < 0);

	CASE("name read: a label plus a backward pointer loop is refused");
	w_reset(&w);
	w_u8(&w, 1);				/* 0: "a" */
	w_u8(&w, 'a');
	w_ptr(&w, 0);				/* 2 -> 0, and 0 runs into 2 again */
	CHECK(dns_name_read(w.b, w.len, 0, out, sizeof(out), &next) < 0);

	CASE("name read: a label length over 63 is refused");
	w_reset(&w);
	w_u8(&w, 64);
	w_bytes(&w, "0123456789012345678901234567890123456789012345678901234567890123",
		64);
	w_u8(&w, 0);
	CHECK(dns_name_read(w.b, w.len, 0, out, sizeof(out), &next) < 0);

	CASE("name read: a name with no terminator is refused");
	w_reset(&w);
	w_u8(&w, 3);
	w_bytes(&w, "sip", 3);
	CHECK(dns_name_read(w.b, w.len, 0, out, sizeof(out), &next) < 0);

	CASE("name read: a truncated label is refused");
	w_reset(&w);
	w_u8(&w, 9);
	w_bytes(&w, "sip", 3);
	CHECK(dns_name_read(w.b, w.len, 0, out, sizeof(out), &next) < 0);

	CASE("name read: the reserved label types 0x40 and 0x80 are refused");
	w_reset(&w);
	w_u8(&w, 0x40);
	w_u8(&w, 0);
	CHECK(dns_name_read(w.b, w.len, 0, out, sizeof(out), &next) < 0);
	w_reset(&w);
	w_u8(&w, 0x80);
	w_u8(&w, 0);
	CHECK(dns_name_read(w.b, w.len, 0, out, sizeof(out), &next) < 0);

	CASE("name read: a pointer with only one byte left is refused");
	w_reset(&w);
	w_u8(&w, 0xc0);
	CHECK(dns_name_read(w.b, w.len, 0, out, sizeof(out), &next) < 0);

	CASE("name read: an output buffer that is too small is refused");
	w_reset(&w);
	w_name(&w, "sip.example.com");
	CHECK(dns_name_read(w.b, w.len, 0, out, 8, &next) < 0);
	CHECK(dns_name_read(w.b, w.len, 0, out, 1, &next) < 0);

	CASE("name read: an offset outside the buffer is refused");
	w_reset(&w);
	w_name(&w, "a");
	CHECK(dns_name_read(w.b, w.len, w.len + 4, out, sizeof(out), &next) < 0);

	CASE("name read: a wire name over 255 bytes is refused");
	w_reset(&w);
	{
		unsigned int i;

		for (i = 0; i < 5; i++) {
			w_u8(&w, 63);
			w_bytes(&w, "012345678901234567890123456789012345678901234567890"
				    "123456789012", 63);
		}
		w_u8(&w, 0);
	}
	CHECK(dns_name_read(w.b, w.len, 0, out, sizeof(out), &next) < 0);

	CASE("name read: a chain of backward pointers still terminates");
	w_reset(&w);
	w_name(&w, "com");			/* 0 */
	w_u8(&w, 7);				/* 5 */
	w_bytes(&w, "example", 7);
	w_ptr(&w, 0);
	w_u8(&w, 3);				/* 15 */
	w_bytes(&w, "sip", 3);
	w_ptr(&w, 5);
	CHECK(dns_name_read(w.b, w.len, 15, out, sizeof(out), &next) == 0);
	CHECK(!strcmp(out, "sip.example.com"));
	CHECK_UINT(next, 21);
}

static void test_name_eq(void)
{
	CASE("name eq: case does not matter");
	CHECK(dns_name_eq("SIP.Example.COM", "sip.example.com"));

	CASE("name eq: one trailing dot does not matter");
	CHECK(dns_name_eq("example.com.", "example.com"));
	CHECK(dns_name_eq("example.com", "example.com."));

	CASE("name eq: a different name is not equal");
	CHECK(!dns_name_eq("a.example.com", "b.example.com"));
	CHECK(!dns_name_eq("example.com", "example.co"));
	CHECK(!dns_name_eq("", "a"));
	CHECK(dns_name_eq("", ""));
}

/* ------------------------------------------------------------ query build */

static void test_query_build(void)
{
	uint8_t buf[512];
	char name[SIP_DNS_NAME_MAX];
	struct dns_pkt p;
	size_t next;
	int n;

	CASE("query build: header, question and no OPT without edns");
	n = dns_query_build(buf, sizeof(buf), 0x1234, "sip.example.com", DNS_T_A, false);
	CHECK(n > 0);
	CHECK_UINT((buf[0] << 8) | buf[1], 0x1234);
	CHECK_UINT((buf[2] << 8) | buf[3], DNS_F_RD);
	CHECK_UINT((buf[4] << 8) | buf[5], 1);
	CHECK_UINT((buf[6] << 8) | buf[7], 0);
	CHECK_UINT((buf[10] << 8) | buf[11], 0);
	CHECK(dns_name_read(buf, (size_t)n, 12, name, sizeof(name), &next) == 0);
	CHECK(!strcmp(name, "sip.example.com"));
	CHECK_UINT((buf[next] << 8) | buf[next + 1], DNS_T_A);
	CHECK_UINT((buf[next + 2] << 8) | buf[next + 3], DNS_CLASS_IN);
	CHECK_UINT(n, next + 4);

	CASE("query build: edns adds one OPT record with the payload size");
	n = dns_query_build(buf, sizeof(buf), 1, "a", DNS_T_SRV, true);
	CHECK(n > 0);
	CHECK_UINT((buf[10] << 8) | buf[11], 1);
	CHECK_UINT(buf[n - 11], 0);				/* root name */
	CHECK_UINT((buf[n - 10] << 8) | buf[n - 9], DNS_T_OPT);
	CHECK_UINT((buf[n - 8] << 8) | buf[n - 7], SIP_DNS_EDNS_SIZE);
	CHECK_UINT((buf[n - 2] << 8) | buf[n - 1], 0);		/* rdlength */

	CASE("query build: a buffer that cannot hold the question is refused");
	CHECK(dns_query_build(buf, 8, 1, "a", DNS_T_A, false) < 0);
	CHECK(dns_query_build(buf, 16, 1, "sip.example.com", DNS_T_A, false) < 0);

	CASE("query build: a buffer that fits the question but not the OPT is refused");
	n = dns_query_build(buf, sizeof(buf), 1, "a", DNS_T_A, false);
	CHECK(dns_query_build(buf, (size_t)n + 5, 1, "a", DNS_T_A, true) < 0);

	CASE("query build: an unusable name is refused");
	CHECK(dns_query_build(buf, sizeof(buf), 1, "a..b", DNS_T_A, false) < 0);

	CASE("query build: the result parses as its own question");
	n = dns_query_build(buf, sizeof(buf), 0x4321, "_sip._udp.example.com", DNS_T_SRV,
			    true);
	CHECK(dns_pkt_parse(&p, buf, (size_t)n) == 0);
	CHECK_UINT(p.id, 0x4321);
	CHECK(p.has_question);
	CHECK(!strcmp(p.qname, "_sip._udp.example.com"));
	CHECK_UINT(p.qtype, DNS_T_SRV);
	CHECK_UINT(p.count[DNS_SECTION_ADDITIONAL], 1);
}

/* ------------------------------------------------------------ packet parse */

static void test_pkt_parse(void)
{
	struct dns_pkt p;
	struct wire w;

	CASE("pkt parse: a buffer shorter than the header is refused");
	w_reset(&w);
	w_bytes(&w, "12345678901", 11);
	CHECK(dns_pkt_parse(&p, w.b, w.len) < 0);

	CASE("pkt parse: two questions are refused");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 2, 0, 0, 0);
	w_question(&w, "a", DNS_T_A);
	w_question(&w, "b", DNS_T_A);
	CHECK(dns_pkt_parse(&p, w.b, w.len) < 0);

	CASE("pkt parse: an unreadable question is refused");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 0, 0, 0);
	w_u8(&w, 9);
	w_bytes(&w, "ab", 2);
	CHECK(dns_pkt_parse(&p, w.b, w.len) < 0);

	CASE("pkt parse: a question with no type and class is refused");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 0, 0, 0);
	w_name(&w, "a");
	w_u16(&w, DNS_T_A);
	CHECK(dns_pkt_parse(&p, w.b, w.len) < 0);

	CASE("pkt parse: a header with no question at all is accepted");
	w_reset(&w);
	w_hdr(&w, 7, DNS_F_QR | DNS_RCODE_SERVFAIL, 0, 0, 0, 0);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(!p.has_question);
	CHECK_UINT(p.rr_off, DNS_HDR_LEN);
	CHECK_UINT(DNS_RCODE(p.flags), DNS_RCODE_SERVFAIL);

	CASE("pkt parse: counts, flags and the record offset");
	w_reset(&w);
	w_hdr(&w, 0xbeef, DNS_F_QR | DNS_F_RD | DNS_F_RA | DNS_F_TC, 1, 2, 3, 4);
	w_question(&w, "sip.example.com", DNS_T_A);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK_UINT(p.id, 0xbeef);
	CHECK(p.flags & DNS_F_TC);
	CHECK_UINT(p.count[DNS_SECTION_ANSWER], 2);
	CHECK_UINT(p.count[DNS_SECTION_AUTHORITY], 3);
	CHECK_UINT(p.count[DNS_SECTION_ADDITIONAL], 4);
	CHECK_UINT(p.qclass, DNS_CLASS_IN);
	CHECK_UINT(p.rr_off, w.len);
}

/* --------------------------------------------------------------- rr walk */

static void test_rr_walk(void)
{
	struct dns_pkt p;
	struct dns_rr_iter it;
	struct dns_rr rr;
	struct sip_txn_dst dst;
	struct wire w;
	char host[64];

	CASE("rr walk: one A answer");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 1, 0, 0);
	w_question(&w, "sip.example.com", DNS_T_A);
	w_rr_a(&w, "sip.example.com", "203.0.113.7", 300);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(dns_rr_first(&p, &it, &rr));
	CHECK(!strcmp(rr.name, "sip.example.com"));
	CHECK_UINT(rr.type, DNS_T_A);
	CHECK_UINT(rr.rclass, DNS_CLASS_IN);
	CHECK_UINT(rr.ttl, 300);
	CHECK_UINT(rr.rdlen, 4);
	CHECK_UINT(rr.section, DNS_SECTION_ANSWER);
	CHECK(!dns_rr_next(&p, &it, &rr));

	CASE("rr walk: the A record becomes a destination with the caller's port");
	CHECK(dns_rr_first(&p, &it, &rr));
	CHECK(dns_rr_addr(&p, &rr, 5060, &dst));
	CHECK(sip_dst_host(&dst, host, sizeof(host)));
	CHECK(!strcmp(host, "203.0.113.7"));
	CHECK_UINT(sip_dst_port(&dst), 5060);

	CASE("rr walk: two answers in receive order");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 2, 0, 0);
	w_question(&w, "sip.example.com", DNS_T_A);
	w_rr_a(&w, "sip.example.com", "198.51.100.1", 60);
	w_rr_a(&w, "sip.example.com", "198.51.100.2", 60);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(dns_rr_first(&p, &it, &rr));
	CHECK(dns_rr_addr(&p, &rr, 0, &dst));
	CHECK(sip_dst_host(&dst, host, sizeof(host)));
	CHECK(!strcmp(host, "198.51.100.1"));
	CHECK(dns_rr_next(&p, &it, &rr));
	CHECK(dns_rr_addr(&p, &rr, 0, &dst));
	CHECK(sip_dst_host(&dst, host, sizeof(host)));
	CHECK(!strcmp(host, "198.51.100.2"));
	CHECK(!dns_rr_next(&p, &it, &rr));

	CASE("rr walk: an answer count that lies ends the walk at the buffer");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 9, 0, 0);
	w_question(&w, "a", DNS_T_A);
	w_rr_a(&w, "a", "192.0.2.1", 60);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(dns_rr_first(&p, &it, &rr));
	CHECK(!dns_rr_next(&p, &it, &rr));

	CASE("rr walk: an rdlength past the end of the buffer ends the walk");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 1, 0, 0);
	w_question(&w, "a", DNS_T_A);
	w_name(&w, "a");
	w_u16(&w, DNS_T_A);
	w_u16(&w, DNS_CLASS_IN);
	w_u32(&w, 60);
	w_u16(&w, 0xffff);
	w_bytes(&w, "\x01\x02\x03\x04", 4);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(!dns_rr_first(&p, &it, &rr));

	CASE("rr walk: a record cut off before its type ends the walk");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 1, 0, 0);
	w_question(&w, "a", DNS_T_A);
	w_name(&w, "a");
	w_u16(&w, DNS_T_A);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(!dns_rr_first(&p, &it, &rr));

	CASE("rr walk: the header alone with a non zero count yields nothing");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 0, 1, 1, 1);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(!dns_rr_first(&p, &it, &rr));

	CASE("rr walk: answer, authority and additional report their section");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 1, 1, 1);
	w_question(&w, "a", DNS_T_A);
	w_rr_a(&w, "a", "192.0.2.1", 60);
	w_rr_a(&w, "ns", "192.0.2.2", 60);
	w_rr_a(&w, "extra", "192.0.2.3", 60);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(dns_rr_first(&p, &it, &rr));
	CHECK_UINT(rr.section, DNS_SECTION_ANSWER);
	CHECK(dns_rr_next(&p, &it, &rr));
	CHECK_UINT(rr.section, DNS_SECTION_AUTHORITY);
	CHECK(dns_rr_next(&p, &it, &rr));
	CHECK_UINT(rr.section, DNS_SECTION_ADDITIONAL);
	CHECK(!strcmp(rr.name, "extra"));
	CHECK(!dns_rr_next(&p, &it, &rr));

	CASE("rr walk: an empty answer section skips straight to additional");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 0, 0, 1);
	w_question(&w, "a", DNS_T_A);
	w_rr_a(&w, "extra", "192.0.2.3", 60);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(dns_rr_first(&p, &it, &rr));
	CHECK_UINT(rr.section, DNS_SECTION_ADDITIONAL);

	CASE("rr walk: an OPT record walks like any other and is skipped by type");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 1, 0, 1);
	w_question(&w, "a", DNS_T_A);
	w_rr_a(&w, "a", "192.0.2.1", 60);
	w_u8(&w, 0);
	w_u16(&w, DNS_T_OPT);
	w_u16(&w, SIP_DNS_EDNS_SIZE);
	w_u32(&w, 0);
	w_u16(&w, 0);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(dns_rr_first(&p, &it, &rr));
	CHECK(dns_rr_next(&p, &it, &rr));
	CHECK_UINT(rr.type, DNS_T_OPT);
	CHECK(!dns_rr_addr(&p, &rr, 0, &dst));
	CHECK(!dns_rr_next(&p, &it, &rr));
}

/* ------------------------------------------------------------- extractors */

static void test_extractors(void)
{
	struct dns_pkt p;
	struct dns_rr_iter it;
	struct dns_rr rr;
	struct sip_txn_dst dst;
	struct wire w;
	char host[64];
	char name[SIP_DNS_NAME_MAX];
	uint16_t prio, weight, port;
	size_t off;

	CASE("addr: an A record whose rdlength is not 4 is refused");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 1, 0, 0);
	w_question(&w, "a", DNS_T_A);
	off = w_rr_head(&w, "a", DNS_T_A, 60);
	w_bytes(&w, "\x01\x02\x03", 3);
	w_rr_end(&w, off);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(dns_rr_first(&p, &it, &rr));
	CHECK_UINT(rr.rdlen, 3);
	CHECK(!dns_rr_addr(&p, &rr, 5060, &dst));

	CASE("addr: an AAAA record becomes a v6 destination");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 1, 0, 0);
	w_question(&w, "a", DNS_T_AAAA);
	w_rr_aaaa(&w, "a", "2001:db8::35", 60);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(dns_rr_first(&p, &it, &rr));
	CHECK(dns_rr_addr(&p, &rr, 5060, &dst));
	CHECK_UINT(dst.sa.ss_family, AF_INET6);
	CHECK(sip_dst_host(&dst, host, sizeof(host)));
	CHECK(!strcmp(host, "2001:db8::35"));

	CASE("addr: an AAAA record whose rdlength is not 16 is refused");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 1, 0, 0);
	w_question(&w, "a", DNS_T_AAAA);
	off = w_rr_head(&w, "a", DNS_T_AAAA, 60);
	w_bytes(&w, "0123456789abcde", 15);
	w_rr_end(&w, off);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(dns_rr_first(&p, &it, &rr));
	CHECK(!dns_rr_addr(&p, &rr, 5060, &dst));

	CASE("srv: priority, weight, port and target");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 1, 0, 0);
	w_question(&w, "_sip._udp.example.com", DNS_T_SRV);
	w_rr_srv(&w, "_sip._udp.example.com", 10, 60, 5062, "a.example.com", 120);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(dns_rr_first(&p, &it, &rr));
	CHECK(dns_rr_srv(&p, &rr, &prio, &weight, &port, name, sizeof(name)));
	CHECK_UINT(prio, 10);
	CHECK_UINT(weight, 60);
	CHECK_UINT(port, 5062);
	CHECK(!strcmp(name, "a.example.com"));

	CASE("srv: an rdata shorter than one target is refused");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 1, 0, 0);
	w_question(&w, "s", DNS_T_SRV);
	off = w_rr_head(&w, "s", DNS_T_SRV, 60);
	w_u16(&w, 1);
	w_u16(&w, 1);
	w_u16(&w, 5060);
	w_rr_end(&w, off);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(dns_rr_first(&p, &it, &rr));
	CHECK_UINT(rr.rdlen, 6);
	CHECK(!dns_rr_srv(&p, &rr, &prio, &weight, &port, name, sizeof(name)));

	CASE("srv: a target of \".\" reads as the empty name");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 1, 0, 0);
	w_question(&w, "s", DNS_T_SRV);
	w_rr_srv(&w, "s", 0, 0, 0, "", 60);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(dns_rr_first(&p, &it, &rr));
	CHECK(dns_rr_srv(&p, &rr, &prio, &weight, &port, name, sizeof(name)));
	CHECK_UINT(strlen(name), 0);

	CASE("srv: a compressed target is decoded against the whole packet");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 1, 0, 0);
	w_question(&w, "example.com", DNS_T_SRV);	/* the name sits at 12 */
	off = w_rr_head(&w, "example.com", DNS_T_SRV, 60);
	w_u16(&w, 5);
	w_u16(&w, 0);
	w_u16(&w, 5060);
	w_u8(&w, 1);
	w_u8(&w, 'a');
	w_ptr(&w, 12);
	w_rr_end(&w, off);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(dns_rr_first(&p, &it, &rr));
	CHECK(dns_rr_srv(&p, &rr, &prio, &weight, &port, name, sizeof(name)));
	CHECK(!strcmp(name, "a.example.com"));

	CASE("srv: a target whose compression pointer loops is refused");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 1, 0, 0);
	w_question(&w, "s", DNS_T_SRV);
	off = w_rr_head(&w, "s", DNS_T_SRV, 60);
	w_u16(&w, 5);
	w_u16(&w, 0);
	w_u16(&w, 5060);
	w_ptr(&w, (uint16_t)w.len);			/* points at itself */
	w_rr_end(&w, off);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(dns_rr_first(&p, &it, &rr));
	CHECK(!dns_rr_srv(&p, &rr, &prio, &weight, &port, name, sizeof(name)));

	CASE("name: a CNAME target is read from the rdata");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 1, 0, 0);
	w_question(&w, "sip.example.com", DNS_T_A);
	w_rr_cname(&w, "sip.example.com", "edge.example.net", 60);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(dns_rr_first(&p, &it, &rr));
	CHECK(dns_rr_name(&p, &rr, name, sizeof(name)));
	CHECK(!strcmp(name, "edge.example.net"));

	CASE("name: an empty rdata carries no name");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 1, 0, 0);
	w_question(&w, "a", DNS_T_A);
	off = w_rr_head(&w, "a", DNS_T_CNAME, 60);
	w_rr_end(&w, off);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(dns_rr_first(&p, &it, &rr));
	CHECK(!dns_rr_name(&p, &rr, name, sizeof(name)));
}

/* --------------------------------------------------------- realistic sets */

static void test_cname_chain(void)
{
	struct dns_pkt p;
	struct dns_rr_iter it;
	struct dns_rr rr;
	struct wire w;
	char name[SIP_DNS_NAME_MAX];
	unsigned int cnames = 0;
	unsigned int addrs = 0;
	bool more;

	CASE("cname chain: two hops and the address behind them");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 3, 0, 0);
	w_question(&w, "sip.example.com", DNS_T_A);
	w_rr_cname(&w, "sip.example.com", "edge.example.net", 60);
	w_rr_cname(&w, "edge.example.net", "pop1.example.net", 60);
	w_rr_a(&w, "pop1.example.net", "203.0.113.9", 45);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);

	for (more = dns_rr_first(&p, &it, &rr); more; more = dns_rr_next(&p, &it, &rr)) {
		if (rr.type == DNS_T_CNAME) {
			CHECK(dns_rr_name(&p, &rr, name, sizeof(name)));
			cnames++;
		}
		if (rr.type == DNS_T_A)
			addrs++;
	}
	CHECK_UINT(cnames, 2);
	CHECK_UINT(addrs, 1);
	CHECK(!strcmp(name, "pop1.example.net"));

	CASE("cname chain: a self referencing chain still walks and terminates");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 2, 0, 0);
	w_question(&w, "a.example.com", DNS_T_A);
	w_rr_cname(&w, "a.example.com", "b.example.com", 60);
	w_rr_cname(&w, "b.example.com", "a.example.com", 60);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	cnames = 0;
	for (more = dns_rr_first(&p, &it, &rr); more; more = dns_rr_next(&p, &it, &rr))
		cnames++;
	CHECK_UINT(cnames, 2);

	CASE("cname chain: compressed owner names decode to the chain");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 2, 0, 0);
	w_question(&w, "sip.example.com", DNS_T_A);	/* name at offset 12 */
	w_rr_cname(&w, "sip.example.com", "edge.example.com", 60);
	{
		size_t off;

		w_ptr(&w, 12 + 4);			/* "example.com" */
		w_u16(&w, DNS_T_A);
		w_u16(&w, DNS_CLASS_IN);
		w_u32(&w, 30);
		off = w.len;
		w_u16(&w, 0);
		w_bytes(&w, "\xcb\x00\x71\x0a", 4);
		w_rr_end(&w, off);
	}
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(dns_rr_first(&p, &it, &rr));
	CHECK(dns_rr_next(&p, &it, &rr));
	CHECK(!strcmp(rr.name, "example.com"));
	CHECK_UINT(rr.type, DNS_T_A);
}

static void test_srv_answer(void)
{
	struct dns_pkt p;
	struct dns_rr_iter it;
	struct dns_rr rr;
	struct sip_txn_dst dst;
	struct wire w;
	char name[SIP_DNS_NAME_MAX];
	char host[64];
	uint16_t prio, weight, port;
	unsigned int srvs = 0;
	unsigned int glue = 0;
	bool more;

	CASE("srv answer: two records with their glue in the additional section");
	w_reset(&w);
	w_hdr(&w, 0x2a2a, DNS_F_QR | DNS_F_RD | DNS_F_RA, 1, 2, 0, 2);
	w_question(&w, "_sip._udp.example.com", DNS_T_SRV);
	w_rr_srv(&w, "_sip._udp.example.com", 10, 60, 5060, "a.example.com", 300);
	w_rr_srv(&w, "_sip._udp.example.com", 20, 0, 5061, "b.example.com", 300);
	w_rr_a(&w, "a.example.com", "203.0.113.10", 300);
	w_rr_a(&w, "b.example.com", "203.0.113.20", 300);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK_UINT(p.count[DNS_SECTION_ANSWER], 2);
	CHECK_UINT(p.count[DNS_SECTION_ADDITIONAL], 2);

	for (more = dns_rr_first(&p, &it, &rr); more; more = dns_rr_next(&p, &it, &rr)) {
		if (rr.section == DNS_SECTION_ANSWER && rr.type == DNS_T_SRV) {
			CHECK(dns_rr_srv(&p, &rr, &prio, &weight, &port, name,
					 sizeof(name)));
			srvs++;
			continue;
		}

		if (rr.section == DNS_SECTION_ADDITIONAL && rr.type == DNS_T_A) {
			CHECK(dns_rr_addr(&p, &rr, 5060, &dst));
			glue++;
		}
	}
	CHECK_UINT(srvs, 2);
	CHECK_UINT(glue, 2);
	CHECK(!strcmp(name, "b.example.com"));
	CHECK_UINT(prio, 20);
	CHECK(sip_dst_host(&dst, host, sizeof(host)));
	CHECK(!strcmp(host, "203.0.113.20"));

	CASE("srv answer: compressed SRV owner and target names");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 1, 0, 1);
	w_question(&w, "_sip._udp.example.com", DNS_T_SRV);
	{
		size_t off;

		w_ptr(&w, 12);
		w_u16(&w, DNS_T_SRV);
		w_u16(&w, DNS_CLASS_IN);
		w_u32(&w, 300);
		off = w.len;
		w_u16(&w, 0);
		w_u16(&w, 5);
		w_u16(&w, 100);
		w_u16(&w, 5060);
		w_u8(&w, 4);
		w_bytes(&w, "edge", 4);
		w_ptr(&w, 12 + 10);			/* "example.com" */
		w_rr_end(&w, off);
	}
	w_rr_a(&w, "edge.example.com", "192.0.2.44", 300);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(dns_rr_first(&p, &it, &rr));
	CHECK(!strcmp(rr.name, "_sip._udp.example.com"));
	CHECK(dns_rr_srv(&p, &rr, &prio, &weight, &port, name, sizeof(name)));
	CHECK(!strcmp(name, "edge.example.com"));
	CHECK_UINT(weight, 100);
	CHECK(dns_rr_next(&p, &it, &rr));
	CHECK(!strcmp(rr.name, "edge.example.com"));
}

/* ---------------------------------------------------------------- sorting */

static uint32_t rand_seq[8];
static unsigned int rand_pos;

static uint32_t rand_fixed(void *priv)
{
	if (rand_pos >= 8)
		return 0;

	return rand_seq[rand_pos++];
}

static uint16_t group_seq;

/* each target is its own SRV record unless a case overrides t->group */
static void target_set(struct sip_dns_target *t, const char *addr, uint16_t port,
		       uint16_t prio, uint16_t weight)
{
	memset(t, 0, sizeof(*t));
	sip_dst_set(&t->dst, addr, port);
	snprintf(t->host, sizeof(t->host), "%s", addr);
	t->port = port;
	t->prio = prio;
	t->weight = weight;
	t->group = group_seq++;
}

static void test_sort(void)
{
	struct sip_dns_target t[4];

	CASE("canon sort: ascending priority");
	target_set(&t[0], "192.0.2.3", 5060, 30, 0);
	target_set(&t[1], "192.0.2.1", 5060, 10, 0);
	target_set(&t[2], "192.0.2.2", 5060, 20, 0);
	sip_dns_canon_sort(t, 3);
	CHECK_UINT(t[0].prio, 10);
	CHECK_UINT(t[1].prio, 20);
	CHECK_UINT(t[2].prio, 30);

	CASE("canon sort: weight 0 comes first inside one priority");
	target_set(&t[0], "192.0.2.1", 5060, 10, 50);
	target_set(&t[1], "192.0.2.2", 5060, 10, 0);
	target_set(&t[2], "192.0.2.3", 5060, 10, 10);
	sip_dns_canon_sort(t, 3);
	CHECK_UINT(t[0].weight, 0);
	CHECK_UINT(t[1].weight, 10);
	CHECK_UINT(t[2].weight, 50);

	CASE("canon sort: the same input in any order gives the same result");
	target_set(&t[0], "192.0.2.9", 5060, 10, 5);
	target_set(&t[1], "192.0.2.1", 5060, 10, 5);
	sip_dns_canon_sort(t, 2);
	CHECK(!strcmp(t[0].host, "192.0.2.1"));
	target_set(&t[0], "192.0.2.1", 5060, 10, 5);
	target_set(&t[1], "192.0.2.9", 5060, 10, 5);
	sip_dns_canon_sort(t, 2);
	CHECK(!strcmp(t[0].host, "192.0.2.1"));

	CASE("srv sort: a NULL rand leaves the canonical order");
	target_set(&t[0], "192.0.2.2", 5060, 20, 10);
	target_set(&t[1], "192.0.2.1", 5060, 10, 10);
	sip_dns_srv_sort(t, 2, NULL, NULL);
	CHECK_UINT(t[0].prio, 10);
	CHECK_UINT(t[1].prio, 20);

	CASE("srv sort: the draw never moves a record across a priority");
	target_set(&t[0], "192.0.2.1", 5060, 10, 1);
	target_set(&t[1], "192.0.2.2", 5060, 10, 100);
	target_set(&t[2], "192.0.2.3", 5060, 20, 1);
	target_set(&t[3], "192.0.2.4", 5060, 20, 100);
	rand_pos = 0;
	rand_seq[0] = 99;
	rand_seq[1] = 0;
	rand_seq[2] = 99;
	rand_seq[3] = 0;
	sip_dns_srv_sort(t, 4, rand_fixed, NULL);
	CHECK_UINT(t[0].prio, 10);
	CHECK_UINT(t[1].prio, 10);
	CHECK_UINT(t[2].prio, 20);
	CHECK_UINT(t[3].prio, 20);

	CASE("srv sort: a draw at the top of the range picks the heavy record");
	target_set(&t[0], "192.0.2.1", 5060, 10, 1);
	target_set(&t[1], "192.0.2.2", 5060, 10, 100);
	rand_pos = 0;
	rand_seq[0] = 101;		/* 101 % 102 = 101, past the first weight */
	sip_dns_srv_sort(t, 2, rand_fixed, NULL);
	CHECK_UINT(t[0].weight, 100);
	CHECK_UINT(t[1].weight, 1);

	CASE("srv sort: a draw of zero picks the first record of the group");
	target_set(&t[0], "192.0.2.1", 5060, 10, 1);
	target_set(&t[1], "192.0.2.2", 5060, 10, 100);
	rand_pos = 0;
	rand_seq[0] = 0;
	sip_dns_srv_sort(t, 2, rand_fixed, NULL);
	CHECK_UINT(t[0].weight, 1);

	CASE("srv sort: a group whose weights are all zero is left alone");
	target_set(&t[0], "192.0.2.1", 5060, 10, 0);
	target_set(&t[1], "192.0.2.2", 5060, 10, 0);
	rand_pos = 0;
	rand_seq[0] = 12345;
	sip_dns_srv_sort(t, 2, rand_fixed, NULL);
	CHECK(!strcmp(t[0].host, "192.0.2.1"));
	CHECK_UINT(rand_pos, 0);

	CASE("srv sort: one record and no record are both safe");
	target_set(&t[0], "192.0.2.1", 5060, 10, 7);
	sip_dns_srv_sort(t, 1, rand_fixed, NULL);
	CHECK(!strcmp(t[0].host, "192.0.2.1"));
	sip_dns_srv_sort(t, 0, rand_fixed, NULL);
	sip_dns_canon_sort(t, 0);

	/*
	 * Two A records of one SRV record share a group, so the weight counts
	 * once. Group 7 has weight 100 across two addresses; group 8 has weight
	 * 1. A draw of 101 selects group 8 first only if the group-7 weight is
	 * 100, not 200: with double counting (total 201) the same draw would
	 * land inside group 7 and pick one of its addresses instead.
	 */
	CASE("srv sort: two addresses of one SRV record count as one weight");
	target_set(&t[0], "192.0.2.1", 5060, 10, 100);
	target_set(&t[1], "192.0.2.2", 5060, 10, 100);
	t[0].group = 7;
	t[1].group = 7;
	target_set(&t[2], "192.0.2.9", 5060, 10, 1);
	t[2].group = 8;
	rand_pos = 0;
	rand_seq[0] = 101;		/* 101 > 100 selects the second run (group 8) */
	rand_seq[1] = 0;
	sip_dns_srv_sort(t, 3, rand_fixed, NULL);
	CHECK(!strcmp(t[0].host, "192.0.2.9"));

	CASE("srv sort: the two addresses of one record stay adjacent");
	CHECK_UINT(t[1].group, 7);
	CHECK_UINT(t[2].group, 7);

	/*
	 * The removal of a drawn zero-weight record must not reorder the rest.
	 * Three zero-weight records draw out in their input order.
	 */
	CASE("srv sort: zero-weight records keep their relative order");
	target_set(&t[0], "192.0.2.1", 5060, 10, 0);
	target_set(&t[1], "192.0.2.2", 5060, 10, 0);
	target_set(&t[2], "192.0.2.3", 5060, 10, 0);
	rand_pos = 0;
	sip_dns_srv_sort(t, 3, rand_fixed, NULL);
	CHECK(!strcmp(t[0].host, "192.0.2.1"));
	CHECK(!strcmp(t[1].host, "192.0.2.2"));
	CHECK(!strcmp(t[2].host, "192.0.2.3"));
}

/* ------------------------------------------------------------ resolv.conf */

static void test_servers(void)
{
	struct sip_txn_dst srv[SIP_DNS_SERVER_MAX];
	char host[64];

	CASE("resolv.conf: two nameservers in file order");
	CHECK_UINT(sip_dns_servers_parse("nameserver 10.0.0.1\n"
					 "nameserver 10.0.0.2\n", srv,
					 SIP_DNS_SERVER_MAX), 2);
	CHECK(sip_dst_host(&srv[0], host, sizeof(host)));
	CHECK(!strcmp(host, "10.0.0.1"));
	CHECK_UINT(sip_dst_port(&srv[0]), SIP_DNS_PORT);
	CHECK(sip_dst_host(&srv[1], host, sizeof(host)));
	CHECK(!strcmp(host, "10.0.0.2"));

	CASE("resolv.conf: comments, search and options lines are ignored");
	CHECK_UINT(sip_dns_servers_parse("# comment\n"
					 "search lan\n"
					 "options timeout:1\n"
					 "domain lan\n"
					 "nameserver 127.0.0.1\n", srv,
					 SIP_DNS_SERVER_MAX), 1);

	CASE("resolv.conf: an IPv6 nameserver is taken");
	CHECK_UINT(sip_dns_servers_parse("nameserver 2001:db8::53\n", srv,
					 SIP_DNS_SERVER_MAX), 1);
	CHECK_UINT(srv[0].sa.ss_family, AF_INET6);

	CASE("resolv.conf: a scoped link local server is skipped");
	CHECK_UINT(sip_dns_servers_parse("nameserver fe80::1%br-lan\n"
					 "nameserver 10.0.0.1\n", srv,
					 SIP_DNS_SERVER_MAX), 1);
	CHECK(sip_dst_host(&srv[0], host, sizeof(host)));
	CHECK(!strcmp(host, "10.0.0.1"));

	CASE("resolv.conf: a malformed address is skipped, the next one is taken");
	CHECK_UINT(sip_dns_servers_parse("nameserver not-an-address\n"
					 "nameserver\n"
					 "nameserver 10.0.0.9\n", srv,
					 SIP_DNS_SERVER_MAX), 1);

	CASE("resolv.conf: the list stops at the maximum");
	CHECK_UINT(sip_dns_servers_parse("nameserver 10.0.0.1\n"
					 "nameserver 10.0.0.2\n"
					 "nameserver 10.0.0.3\n"
					 "nameserver 10.0.0.4\n"
					 "nameserver 10.0.0.5\n"
					 "nameserver 10.0.0.6\n", srv,
					 SIP_DNS_SERVER_MAX), SIP_DNS_SERVER_MAX);

	CASE("resolv.conf: an empty file names nobody");
	CHECK_UINT(sip_dns_servers_parse("", srv, SIP_DNS_SERVER_MAX), 0);
	CHECK_UINT(sip_dns_servers_parse("\n\n\n", srv, SIP_DNS_SERVER_MAX), 0);

	CASE("resolv.conf: a line without a newline at the end still counts");
	CHECK_UINT(sip_dns_servers_parse("nameserver 10.0.0.1", srv,
					 SIP_DNS_SERVER_MAX), 1);

	CASE("resolv.conf: tabs and carriage returns are separators");
	CHECK_UINT(sip_dns_servers_parse("nameserver\t10.0.0.1\r\n", srv,
					 SIP_DNS_SERVER_MAX), 1);

	CASE("resolv.conf: a line longer than the reader's buffer does not overrun");
	{
		char big[600];

		memset(big, 'x', sizeof(big) - 1);
		big[sizeof(big) - 1] = 0;
		memcpy(big, "nameserver ", 11);
		CHECK_UINT(sip_dns_servers_parse(big, srv, SIP_DNS_SERVER_MAX), 0);
	}
}

/* -------------------------------------------------------------- hostile */

static void test_hostile(void)
{
	struct dns_pkt p;
	struct dns_rr_iter it;
	struct dns_rr rr;
	struct wire w;
	char out[SIP_DNS_NAME_MAX];
	unsigned int seen;
	bool more;

	CASE("hostile: an RR owner name that points into the header reads the header");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 1, 0, 0);
	w_question(&w, "a", DNS_T_A);
	w_ptr(&w, 4);			/* the question count, 0x0001: a root label */
	w_u16(&w, DNS_T_A);
	w_u16(&w, DNS_CLASS_IN);
	w_u32(&w, 60);
	w_u16(&w, 4);
	w_bytes(&w, "\x01\x02\x03\x04", 4);
	CHECK(dns_pkt_parse(&p, w.b, w.len) == 0);
	CHECK(dns_rr_first(&p, &it, &rr));
	CHECK_UINT(strlen(rr.name), 0);
	CHECK_UINT(rr.type, DNS_T_A);
	CHECK(!dns_rr_next(&p, &it, &rr));

	CASE("hostile: every prefix of a well formed packet parses or is refused");
	w_reset(&w);
	w_hdr(&w, 1, DNS_F_QR, 1, 2, 0, 1);
	w_question(&w, "_sip._udp.example.com", DNS_T_SRV);
	w_rr_srv(&w, "_sip._udp.example.com", 10, 60, 5060, "a.example.com", 300);
	w_rr_srv(&w, "_sip._udp.example.com", 20, 0, 5061, "b.example.com", 300);
	w_rr_a(&w, "a.example.com", "203.0.113.10", 300);
	{
		size_t cut;

		for (cut = 0; cut <= w.len; cut++) {
			if (dns_pkt_parse(&p, w.b, cut) < 0)
				continue;

			seen = 0;
			for (more = dns_rr_first(&p, &it, &rr); more;
			     more = dns_rr_next(&p, &it, &rr))
				seen++;

			CHECK(seen <= 3);
		}
	}

	CASE("hostile: every single byte flip still terminates the walk");
	{
		struct wire orig = w;
		size_t i;

		for (i = 0; i < orig.len; i++) {
			unsigned int bit;

			for (bit = 0; bit < 8; bit++) {
				w = orig;
				w.b[i] ^= (uint8_t)(1u << bit);

				if (dns_pkt_parse(&p, w.b, w.len) < 0)
					continue;

				seen = 0;
				for (more = dns_rr_first(&p, &it, &rr); more;
				     more = dns_rr_next(&p, &it, &rr)) {
					char name[SIP_DNS_NAME_MAX];
					struct sip_txn_dst dst;
					uint16_t a, b2, c;

					dns_rr_addr(&p, &rr, 5060, &dst);
					dns_rr_name(&p, &rr, name, sizeof(name));
					dns_rr_srv(&p, &rr, &a, &b2, &c, name,
						   sizeof(name));
					if (++seen > 64)
						break;
				}

				CHECK(seen <= 64);
			}
		}
	}

	CASE("hostile: a pointer chain is refused past DNS_PTR_MAX jumps");
	{
		unsigned int i;
		size_t short_end;

		/*
		 * b[0] is the root. The pointer at 2k+1 names the one at 2k-1,
		 * so every hop is backwards and legal, and the chain is as long
		 * as the packet. Half the cap decodes, twice the cap does not.
		 */
		w_reset(&w);
		w_u8(&w, 0);
		w_ptr(&w, 0);
		for (i = 2; i < 2 * DNS_PTR_MAX; i++)
			w_ptr(&w, (uint16_t)(2 * i - 3));

		short_end = 1 + 2 * (DNS_PTR_MAX / 2);
		CHECK(dns_name_read(w.b, w.len, short_end - 2, out, sizeof(out),
				    NULL) == 0);
		CHECK_UINT(strlen(out), 0);
		CHECK(dns_name_read(w.b, w.len, w.len - 2, out, sizeof(out), NULL) < 0);
	}

	CASE("hostile: 255 labels of one byte each stay inside the name limit");
	w_reset(&w);
	{
		unsigned int i;

		for (i = 0; i < 200; i++) {
			w_u8(&w, 1);
			w_u8(&w, 'a');
		}
		w_u8(&w, 0);
	}
	CHECK(dns_name_read(w.b, w.len, 0, out, sizeof(out), NULL) < 0);
}

/* ---------------------------------------------------- endpoint harness */

/*
 * A scripted transport plus a virtual clock, the same shape as the transaction
 * layer's struct sip_txn_env: no socket, no uloop, no network. It drives the
 * resolver state machine deterministically so the paths a live query only hits
 * by luck (a stage that times out after another answered, a lost glue lookup, a
 * send failure, a server failover, a stale or duplicate reply, an owner that
 * vanishes, a reused slot) are each one case.
 */
static struct sip_dns dns;
static struct sip_txn_dst ep_srv[2];

static struct {
	uint64_t now;
	struct {
		struct sip_dns_query *q;
		uint64_t due;
		bool used;
	} tmr[8];

	unsigned int sends;
	unsigned int fail_sends;		/* fail this many upcoming sends */
	char qname[SIP_DNS_NAME_MAX];
	uint16_t qtype;
	uint16_t id;
	struct sip_txn_dst server;		/* where the last send went */

	unsigned int dones;
	struct sip_dns_result last;
	uint32_t last_query;
	uint32_t last_owner;
} ep;

static uint32_t ep_rand(void *priv)
{
	return 1;
}

static int ep_send(struct sip_dns *r, struct sip_dns_query *q, const void *buf,
		   size_t len, const struct sip_txn_dst *server)
{
	ep.sends++;
	snprintf(ep.qname, sizeof(ep.qname), "%s", q->qname);
	ep.qtype = q->qtype;
	ep.id = q->id;
	ep.server = *server;

	if (ep.fail_sends) {
		ep.fail_sends--;
		return -1;
	}

	return (int)len;
}

static void ep_release(struct sip_dns *r, struct sip_dns_query *q)
{
}

static uint64_t ep_now(struct sip_dns *r)
{
	return ep.now;
}

static void ep_timer_drop(struct sip_dns_query *q)
{
	unsigned int i;

	for (i = 0; i < 8; i++) {
		if (ep.tmr[i].used && ep.tmr[i].q == q)
			ep.tmr[i].used = false;
	}
}

static void ep_timer_set(struct sip_dns *r, struct sip_dns_query *q, unsigned int ms)
{
	unsigned int i;

	ep_timer_drop(q);
	for (i = 0; i < 8; i++) {
		if (ep.tmr[i].used)
			continue;

		ep.tmr[i].used = true;
		ep.tmr[i].q = q;
		ep.tmr[i].due = ep.now + ms;
		return;
	}
}

static void ep_timer_cancel(struct sip_dns *r, struct sip_dns_query *q)
{
	ep_timer_drop(q);
}

static const struct sip_dns_io ep_io = {
	.send = ep_send,
	.release = ep_release,
	.now_ms = ep_now,
	.timer_set = ep_timer_set,
	.timer_cancel = ep_timer_cancel,
};

static void ep_advance(uint64_t ms)
{
	uint64_t target = ep.now + ms;

	for (;;) {
		int sel = -1;
		uint64_t best = 0;
		unsigned int i;

		for (i = 0; i < 8; i++) {
			if (!ep.tmr[i].used || ep.tmr[i].due > target)
				continue;
			if (sel < 0 || ep.tmr[i].due < best) {
				sel = (int)i;
				best = ep.tmr[i].due;
			}
		}

		if (sel < 0) {
			ep.now = target;
			return;
		}

		ep.now = ep.tmr[sel].due;
		ep.tmr[sel].used = false;
		sip_dns_query_fire(ep.tmr[sel].q);
	}
}

static void ep_done(struct sip_dns *r, uint32_t query, uint32_t owner,
		    const struct sip_dns_result *res, void *priv)
{
	ep.dones++;
	ep.last = *res;
	ep.last_query = query;
	ep.last_owner = owner;
}

static void ep_begin(uint32_t retries, uint32_t timeout_ms, bool ipv6, unsigned int servers)
{
	struct sip_dns_cfg cfg;
	struct sip_dns_cb cb;

	sip_dns_cfg_default(&cfg);
	cfg.retries = retries;
	cfg.timeout_ms = timeout_ms;
	cfg.ipv6 = ipv6;
	cfg.edns = false;
	cfg.trace_host = proto_host_fake();
	cfg.trace_dom = TRACE_SIP;

	memset(&cb, 0, sizeof(cb));
	cb.done = ep_done;
	cb.rand32 = ep_rand;

	memset(&ep, 0, sizeof(ep));
	sip_dns_init(&dns, &cfg, &cb, NULL);
	sip_dns_io_set(&dns, &ep_io);

	sip_dst_set(&ep_srv[0], "10.0.0.1", SIP_DNS_PORT);
	sip_dst_set(&ep_srv[1], "10.0.0.2", SIP_DNS_PORT);
	sip_dns_servers_set(&dns, ep_srv, servers);
}

static void ep_inject(uint32_t h, struct wire *w, const struct sip_txn_dst *src)
{
	sip_dns_input(&dns, h, w->b, w->len, src);
}

static void ep_reply_open(struct wire *w, uint16_t id, uint16_t an, uint16_t ns,
			  uint16_t ar, uint16_t rcode)
{
	w_reset(w);
	w_hdr(w, id, DNS_F_QR | DNS_F_RD | DNS_F_RA | rcode, 1, an, ns, ar);
	w_question(w, ep.qname, ep.qtype);
}

/*
 * Replay one captured server answer, the udp payload of a frame. The id of the
 * question in flight goes over the first two bytes, because query_accept()
 * compares the id, the question and the server, and the capture carries the id
 * of the unit that recorded it.
 */
static void ep_replay(uint32_t h, struct wire *w, const uint8_t *pkt,
		      size_t len)
{
	w_reset(w);
	w_bytes(w, pkt, len);
	w->b[0] = (uint8_t)(ep.id >> 8);
	w->b[1] = (uint8_t)ep.id;
	ep_inject(h, w, &ep.server);
}

static void test_endpoint(void)
{
	struct wire w;
	uint32_t h;

	CASE("endpoint: a plain A answer resolves through the fake transport");
	ep_begin(2, 1000, false, 2);
	h = sip_dns_resolve(&dns, "sip.example.com", SIP_DNS_A, 5060, 1);
	CHECK(h != SIP_HANDLE_NONE);
	CHECK_UINT(ep.sends, 1);
	CHECK(!strcmp(ep.qname, "sip.example.com"));
	CHECK_UINT(ep.qtype, DNS_T_A);
	CHECK_UINT(ep.dones, 0);		/* done never runs inside resolve */
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "203.0.113.7", 120);
	ep_inject(h, &w, &ep.server);
	CHECK_UINT(ep.dones, 0);		/* nor inside input, only on the timer */
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 1);
	CHECK(!strcmp(ep.last.target[0].host, "203.0.113.7"));
	CHECK_UINT(ep.last.target[0].port, 5060);
	sip_dns_done(&dns);

	CASE("endpoint: AAAA that fails after A answered keeps the A targets");
	ep_begin(1, 1000, true, 1);
	h = sip_dns_resolve(&dns, "www.example.com", SIP_DNS_ADDR, 5060, 2);
	CHECK_UINT(ep.qtype, DNS_T_A);
	ep_reply_open(&w, ep.id, 2, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "198.51.100.1", 300);
	w_rr_a(&w, ep.qname, "198.51.100.2", 300);
	ep_inject(h, &w, &ep.server);
	CHECK_UINT(ep.qtype, DNS_T_AAAA);	/* it moved on to AAAA */
	CHECK_UINT(ep.dones, 0);
	ep_advance(10000);			/* AAAA never answers, times out */
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 2);		/* the A targets survived */
	sip_dns_done(&dns);

	CASE("endpoint: a lost SRV glue lookup keeps the resolved target");
	ep_begin(1, 1000, false, 1);
	h = sip_dns_resolve(&dns, "example.com", SIP_DNS_SRV, 5060, 3);
	CHECK_UINT(ep.qtype, DNS_T_SRV);
	CHECK(!strcmp(ep.qname, "_sip._udp.example.com"));
	ep_reply_open(&w, ep.id, 2, 0, 1, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 60, 5060, "a.example.com", 300);
	w_rr_srv(&w, ep.qname, 20, 0, 5061, "b.example.com", 300);
	w_rr_a(&w, "a.example.com", "203.0.113.10", 300);	/* glue for a only */
	ep_inject(h, &w, &ep.server);
	CHECK(!strcmp(ep.qname, "b.example.com"));		/* chasing b's glue */
	CHECK_UINT(ep.qtype, DNS_T_A);
	ep_advance(10000);			/* b never answers */
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 1);
	CHECK(!strcmp(ep.last.target[0].host, "203.0.113.10"));
	CHECK_UINT(ep.last.target[0].port, 5060);
	sip_dns_done(&dns);

	CASE("endpoint: a send failure fails over to the next server");
	ep_begin(1, 1000, false, 2);
	ep.fail_sends = 1;			/* the first send is refused */
	h = sip_dns_resolve(&dns, "sip.example.com", SIP_DNS_A, 5060, 4);
	CHECK_UINT(ep.sends, 2);		/* it retried on server 2 at once */
	CHECK(sip_dst_eq(&ep.server, &ep_srv[1]));
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "192.0.2.50", 60);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK(!strcmp(ep.last.target[0].host, "192.0.2.50"));
	sip_dns_done(&dns);

	CASE("endpoint: a server that times out fails over to the next");
	ep_begin(1, 100, false, 2);
	h = sip_dns_resolve(&dns, "sip.example.com", SIP_DNS_A, 5060, 5);
	CHECK(sip_dst_eq(&ep.server, &ep_srv[0]));
	ep_advance(100);			/* retry on server 1 */
	ep_advance(100);			/* server 1 exhausted, move to 2 */
	CHECK(sip_dst_eq(&ep.server, &ep_srv[1]));
	CHECK_UINT(ep.dones, 0);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "192.0.2.60", 60);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	sip_dns_done(&dns);

	CASE("endpoint: a stale id, a wrong name and a wrong source are dropped");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "sip.example.com", SIP_DNS_A, 5060, 6);
	ep_reply_open(&w, ep.id ^ 0x1u, 1, 0, 0, DNS_RCODE_OK);	/* wrong id */
	w_rr_a(&w, ep.qname, "192.0.2.1", 60);
	ep_inject(h, &w, &ep.server);
	CHECK_UINT(ep.dones, 0);
	{
		char save[SIP_DNS_NAME_MAX];
		struct sip_txn_dst other;

		snprintf(save, sizeof(save), "%s", ep.qname);
		snprintf(ep.qname, sizeof(ep.qname), "other.example.com");
		ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);	/* wrong name */
		w_rr_a(&w, ep.qname, "192.0.2.2", 60);
		snprintf(ep.qname, sizeof(ep.qname), "%s", save);
		ep_inject(h, &w, &ep.server);
		CHECK_UINT(ep.dones, 0);

		sip_dst_set(&other, "10.9.9.9", SIP_DNS_PORT);	/* wrong source */
		ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
		w_rr_a(&w, ep.qname, "192.0.2.3", 60);
		ep_inject(h, &w, &other);
		CHECK_UINT(ep.dones, 0);
	}
	CHECK(sip_dns_stat(&dns)->dropped >= 3);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);	/* now the real answer */
	w_rr_a(&w, ep.qname, "192.0.2.9", 60);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK(!strcmp(ep.last.target[0].host, "192.0.2.9"));

	CASE("endpoint: a duplicate answer after completion is ignored");
	ep_inject(h, &w, &ep.server);		/* the same datagram again */
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);		/* no second callback, no crash */
	sip_dns_done(&dns);

	CASE("endpoint: an owner released mid query gets no callback");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "sip.example.com", SIP_DNS_A, 5060, 77);
	CHECK_UINT(sip_dns_query_count(&dns), 1);
	CHECK_UINT(sip_dns_owner_release(&dns, 77), 1);
	CHECK_UINT(sip_dns_query_count(&dns), 0);
	ep_advance(10000);
	CHECK_UINT(ep.dones, 0);
	sip_dns_done(&dns);

	CASE("endpoint: the pool refuses a fifth query and reuses a freed slot");
	ep_begin(2, 1000, false, 1);
	{
		uint32_t hh[SIP_DNS_QUERY_MAX];
		uint32_t reuse;
		unsigned int i;

		for (i = 0; i < SIP_DNS_QUERY_MAX; i++) {
			char name[32];

			snprintf(name, sizeof(name), "h%u.example.com", i);
			hh[i] = sip_dns_resolve(&dns, name, SIP_DNS_A, 5060, 100 + i);
			CHECK(hh[i] != SIP_HANDLE_NONE);
		}
		CHECK_UINT(sip_dns_query_count(&dns), SIP_DNS_QUERY_MAX);
		CHECK(sip_dns_resolve(&dns, "over.example.com", SIP_DNS_A, 5060, 9) ==
		      SIP_HANDLE_NONE);
		CHECK(sip_dns_stat(&dns)->denied >= 1);

		sip_dns_cancel(&dns, hh[0]);
		CHECK_UINT(sip_dns_query_count(&dns), SIP_DNS_QUERY_MAX - 1);

		reuse = sip_dns_resolve(&dns, "new.example.com", SIP_DNS_A, 5060, 200);
		CHECK(reuse != SIP_HANDLE_NONE);
		CHECK_UINT(sip_handle_index(reuse), sip_handle_index(hh[0]));
		CHECK(reuse != hh[0]);		/* same slot, new generation */
	}
	sip_dns_done(&dns);

	CASE("endpoint: a server override does not move an in flight query");
	ep_begin(2, 1000, false, 2);
	h = sip_dns_resolve(&dns, "sip.example.com", SIP_DNS_A, 5060, 8);
	CHECK(sip_dst_eq(&ep.server, &ep_srv[0]));	/* snapshot is [s0, s1] */
	{
		struct sip_txn_dst other = ep_srv[0];

		sip_dst_set(&other, "10.5.5.5", SIP_DNS_PORT);
		sip_dns_servers_set(&dns, &other, 1);	/* reload to a new server */
	}
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "192.0.2.70", 60);
	ep_inject(h, &w, &ep_srv[0]);		/* still answered from the snapshot */
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	sip_dns_done(&dns);

	CASE("endpoint: a cache hit reports the remaining ttl, not the original");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "sip.example.com", SIP_DNS_A, 5060, 10);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "192.0.2.80", 100);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.last.ttl, 100);
	CHECK(!ep.last.cached);
	CHECK(!ep.last.changed);		/* the first answer changes nothing */
	ep_advance(30000);			/* 30 s pass */
	h = sip_dns_resolve(&dns, "sip.example.com", SIP_DNS_A, 5060, 11);
	ep_advance(0);
	CHECK(ep.last.cached);
	CHECK_UINT(ep.last.ttl, 70);		/* 100 - 30 */
	sip_dns_done(&dns);

	CASE("endpoint: a changed address reports changed on the second lookup");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "roam.example.com", SIP_DNS_A, 5060, 12);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "192.0.2.1", 15);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK(!ep.last.changed);
	ep_advance(16000);			/* let the 15 s ttl expire */
	h = sip_dns_resolve(&dns, "roam.example.com", SIP_DNS_A, 5060, 13);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "192.0.2.2", 15);	/* a different address */
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK(ep.last.changed);
	CHECK(!ep.last.cached);
	sip_dns_done(&dns);

	CASE("endpoint: an expired entry sends the next lookup to the wire");
	ep_begin(2, 1000, false, 1);
	{
		uint64_t hits;

		h = sip_dns_resolve(&dns, "spin.example.com", SIP_DNS_A, 5060, 19);
		ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
		w_rr_a(&w, ep.qname, "192.0.2.20", 300);
		ep_inject(h, &w, &ep.server);
		ep_advance(0);
		CHECK(!ep.last.cached);

		hits = sip_dns_stat(&dns)->cache_hits;
		sip_dns_cache_expire(&dns, "spin.example.com", SIP_DNS_A, 5060, NULL);
		h = sip_dns_resolve(&dns, "spin.example.com", SIP_DNS_A, 5060, 20);
		CHECK(!strcmp(ep.qname, "spin.example.com"));
		CHECK_UINT(ep.qtype, DNS_T_A);
		CHECK_UINT(sip_dns_stat(&dns)->cache_hits, hits);
		ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
		w_rr_a(&w, ep.qname, "192.0.2.20", 300);	/* the same address */
		ep_inject(h, &w, &ep.server);
		ep_advance(0);
		CHECK(!ep.last.cached);

		CASE("endpoint: the expired entry still feeds the changed comparison");
		CHECK(!ep.last.changed);	/* the same answer after the expire */
		sip_dns_cache_expire(&dns, "spin.example.com", SIP_DNS_A, 5060, NULL);
		h = sip_dns_resolve(&dns, "spin.example.com", SIP_DNS_A, 5060, 21);
		ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
		w_rr_a(&w, ep.qname, "192.0.2.21", 300);	/* a different address */
		ep_inject(h, &w, &ep.server);
		ep_advance(0);
		CHECK(ep.last.changed);
		CHECK(!ep.last.cached);

		CASE("endpoint: expiring a key the cache does not hold changes nothing");
		hits = sip_dns_stat(&dns)->cache_hits;
		sip_dns_cache_expire(&dns, "absent.example.com", SIP_DNS_A, 5060, NULL);
		sip_dns_cache_expire(&dns, "spin.example.com", SIP_DNS_A, 5061, NULL);
		h = sip_dns_resolve(&dns, "spin.example.com", SIP_DNS_A, 5060, 22);
		ep_advance(0);
		CHECK(ep.last.cached);		/* the entry still serves its key */
		CHECK_UINT(sip_dns_stat(&dns)->cache_hits, hits + 1);
	}
	sip_dns_done(&dns);

	CASE("endpoint: another name takes a free slot and not the expired entry");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "held.example.com", SIP_DNS_A, 5060, 23);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "192.0.2.30", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	sip_dns_cache_expire(&dns, "held.example.com", SIP_DNS_A, 5060, NULL);
	h = sip_dns_resolve(&dns, "other.example.com", SIP_DNS_A, 5060, 24);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "192.0.2.31", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(sip_dns_cache_count(&dns), 2);
	h = sip_dns_resolve(&dns, "held.example.com", SIP_DNS_A, 5060, 25);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "192.0.2.32", 300);	/* a different address */
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK(ep.last.changed);		/* the stored answer was still there */
	sip_dns_done(&dns);

	CASE("endpoint: a short lived CNAME bounds the answer ttl");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "sip.example.com", SIP_DNS_A, 5060, 14);
	ep_reply_open(&w, ep.id, 2, 0, 0, DNS_RCODE_OK);
	w_rr_cname(&w, ep.qname, "edge.example.net", 30);	/* CNAME ttl 30 */
	w_rr_a(&w, "edge.example.net", "203.0.113.55", 3600);	/* A ttl 3600 */
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.ttl, 30);		/* the CNAME ttl, not 3600 */
	sip_dns_done(&dns);

	CASE("endpoint: an OPT record in additional does not corrupt the ttl");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "sip.example.com", SIP_DNS_A, 5060, 30);
	ep_reply_open(&w, ep.id, 1, 0, 1, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "192.0.2.1", 300);
	w_u8(&w, 0);				/* OPT: root name */
	w_u16(&w, DNS_T_OPT);
	w_u16(&w, 1232);			/* UDP payload size */
	w_u32(&w, 0);				/* extended rcode and flags, ttl field 0 */
	w_u16(&w, 0);				/* rdlength */
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.ttl, 300);		/* the A ttl, not the OPT ttl of 0 */
	sip_dns_done(&dns);

	CASE("endpoint: a record ttl of 0 is kept, clamped up to the ttl floor");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "sip.example.com", SIP_DNS_A, 5060, 15);
	ep_reply_open(&w, ep.id, 2, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "192.0.2.1", 0);	/* ttl 0: do not cache */
	w_rr_a(&w, ep.qname, "192.0.2.2", 3600);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.last.ttl, SIP_DNS_TTL_MIN);	/* 0 kept, clamped to the min */
	sip_dns_done(&dns);

	CASE("endpoint: an SRV zone that refuses the service reports no-service");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "example.com", SIP_DNS_SRV, 5060, 16);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 0, 0, 0, "", 60);	/* target "." */
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_NO_SERVICE);
	sip_dns_done(&dns);

	CASE("endpoint: no SRV record falls back to the plain address");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "example.com", SIP_DNS_SRV, 5062, 17);
	CHECK_UINT(ep.qtype, DNS_T_SRV);
	ep_reply_open(&w, ep.id, 0, 0, 0, DNS_RCODE_OK);	/* empty SRV answer */
	ep_inject(h, &w, &ep.server);
	CHECK_UINT(ep.qtype, DNS_T_A);		/* fell back to A of example.com */
	CHECK(!strcmp(ep.qname, "example.com"));
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "203.0.113.99", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.target[0].port, 5062);	/* the fallback port */
	CHECK(!ep.last.srv);			/* the address built these targets */
	sip_dns_done(&dns);

	/*
	 * Which answer built the targets, for the status surface of the account
	 * layer. It travels into the cache with them, so a line served from the
	 * cache reports the same word, and it stays out of the target
	 * comparison, so it never makes an unchanged answer read as changed.
	 */
	CASE("endpoint: an SRV answer reports that SRV records built it");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "srvflag.example.com", SIP_DNS_SRV, 5060, 40);
	CHECK_UINT(ep.qtype, DNS_T_SRV);
	ep_reply_open(&w, ep.id, 1, 0, 1, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 0, 5070, "a.example.com", 300);
	w_rr_a(&w, "a.example.com", "203.0.113.30", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.target[0].port, 5070);
	CHECK(ep.last.srv);
	CHECK(!ep.last.changed);

	CASE("endpoint: and the cache hit of that answer reports it too");
	h = sip_dns_resolve(&dns, "srvflag.example.com", SIP_DNS_SRV, 5060, 41);
	ep_advance(0);
	CHECK(ep.last.cached);
	CHECK(ep.last.srv);
	CHECK(!ep.last.changed);
	sip_dns_done(&dns);

	CASE("endpoint: an address lookup carries the caller's port and no flag");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "addr.example.com", SIP_DNS_ADDR, 5080, 42);
	CHECK_UINT(ep.qtype, DNS_T_A);
	CHECK(!strcmp(ep.qname, "addr.example.com"));
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "203.0.113.31", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 1);
	CHECK_UINT(ep.last.target[0].port, 5080);
	CHECK(!ep.last.srv);
	sip_dns_done(&dns);

	CASE("endpoint: NXDOMAIN on a plain lookup is reported");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "nope.invalid", SIP_DNS_A, 5060, 18);
	ep_reply_open(&w, ep.id, 0, 0, 0, DNS_RCODE_NXDOMAIN);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_NXDOMAIN);
	CHECK_UINT(ep.last.count, 0);
	sip_dns_done(&dns);

	CASE("endpoint: a truncated answer is delivered and flagged");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "sip.example.com", SIP_DNS_A, 5060, 19);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w.b[2] |= (uint8_t)(DNS_F_TC >> 8);	/* set TC on the answer */
	w_rr_a(&w, ep.qname, "192.0.2.1", 60);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK(ep.last.truncated);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	sip_dns_done(&dns);
}

/* ---------------------------------------------------------- family order */

/*
 * The delivery order under a family preference. RFC 2782 priority and weight
 * are the routing policy of the zone operator. The partition therefore stays
 * inside one (priority, group) run, and the cache keeps the canonical order.
 */
static void test_family_order(void)
{
	struct wire w;
	uint32_t h;

	CASE("family: the preference puts AAAA targets first inside one SRV "
	     "record");
	ep_begin(1, 1000, true, 1);
	sip_dns_prefer_family_set(&dns, AF_INET6);
	CHECK_UINT(sip_dns_prefer_family(&dns), AF_INET6);
	h = sip_dns_resolve(&dns, "example.com", SIP_DNS_SRV, 5060, 1);
	ep_reply_open(&w, ep.id, 1, 0, 2, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 0, 5060, "a.example.com", 300);
	w_rr_a(&w, "a.example.com", "203.0.113.10", 300);
	w_rr_aaaa(&w, "a.example.com", "2001:db8::10", 300);
	ep_inject(h, &w, &ep.server);
	CHECK_UINT(ep.sends, 1);		/* the glue was complete */
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 2);
	CHECK(!strcmp(ep.last.target[0].host, "2001:db8::10"));
	CHECK(!strcmp(ep.last.target[1].host, "203.0.113.10"));
	sip_dns_done(&dns);

	CASE("family: the preference never crosses an SRV priority");
	ep_begin(1, 1000, true, 1);
	sip_dns_prefer_family_set(&dns, AF_INET6);
	h = sip_dns_resolve(&dns, "example.com", SIP_DNS_SRV, 5060, 2);
	ep_reply_open(&w, ep.id, 2, 0, 2, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 0, 5060, "a.example.com", 300);
	w_rr_srv(&w, ep.qname, 20, 0, 5061, "b.example.com", 300);
	w_rr_a(&w, "a.example.com", "203.0.113.10", 300);
	w_rr_aaaa(&w, "b.example.com", "2001:db8::20", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(20000);		/* the missing families time out */
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 2);
	/* priority 10 stays in front of the preferred family */
	CHECK(!strcmp(ep.last.target[0].host, "203.0.113.10"));
	CHECK(!strcmp(ep.last.target[1].host, "2001:db8::20"));
	sip_dns_done(&dns);

	CASE("family: the preference never crosses an SRV record");
	ep_begin(1, 1000, true, 1);
	sip_dns_prefer_family_set(&dns, AF_INET6);
	h = sip_dns_resolve(&dns, "example.com", SIP_DNS_SRV, 5060, 3);
	ep_reply_open(&w, ep.id, 2, 0, 2, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 0, 5060, "a.example.com", 300);
	w_rr_srv(&w, ep.qname, 10, 0, 5061, "b.example.com", 300);
	w_rr_a(&w, "a.example.com", "203.0.113.10", 300);
	w_rr_aaaa(&w, "b.example.com", "2001:db8::20", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(20000);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.count, 2);
	/* the first record stays in front of the preferred family */
	CHECK(!strcmp(ep.last.target[0].host, "203.0.113.10"));
	CHECK(!strcmp(ep.last.target[1].host, "2001:db8::20"));
	sip_dns_done(&dns);

	CASE("family: a preference of zero leaves the order alone");
	ep_begin(1, 1000, true, 1);
	CHECK_UINT(sip_dns_prefer_family(&dns), 0);
	h = sip_dns_resolve(&dns, "example.com", SIP_DNS_SRV, 5060, 4);
	ep_reply_open(&w, ep.id, 1, 0, 2, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 0, 5060, "a.example.com", 300);
	w_rr_a(&w, "a.example.com", "203.0.113.10", 300);
	w_rr_aaaa(&w, "a.example.com", "2001:db8::10", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.last.count, 2);
	/* the canonical order: the family is numeric, so v4 comes first */
	CHECK(!strcmp(ep.last.target[0].host, "203.0.113.10"));
	CHECK(!strcmp(ep.last.target[1].host, "2001:db8::10"));
	sip_dns_done(&dns);

	CASE("family: a plain address answer partitions whole");
	ep_begin(1, 1000, true, 1);
	sip_dns_prefer_family_set(&dns, AF_INET6);
	h = sip_dns_resolve(&dns, "www.example.com", SIP_DNS_ADDR, 5060, 5);
	CHECK_UINT(ep.qtype, DNS_T_A);
	ep_reply_open(&w, ep.id, 2, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "198.51.100.1", 300);
	w_rr_a(&w, ep.qname, "198.51.100.2", 300);
	ep_inject(h, &w, &ep.server);
	CHECK_UINT(ep.qtype, DNS_T_AAAA);
	ep_reply_open(&w, ep.id, 2, 0, 0, DNS_RCODE_OK);
	w_rr_aaaa(&w, ep.qname, "2001:db8::1", 300);
	w_rr_aaaa(&w, ep.qname, "2001:db8::2", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.last.count, 4);
	CHECK(!strcmp(ep.last.target[0].host, "2001:db8::1"));
	CHECK(!strcmp(ep.last.target[1].host, "2001:db8::2"));
	CHECK(!strcmp(ep.last.target[2].host, "198.51.100.1"));
	CHECK(!strcmp(ep.last.target[3].host, "198.51.100.2"));
	sip_dns_done(&dns);

	CASE("family: a cached answer is delivered under the preference of "
	     "the moment");
	ep_begin(1, 1000, true, 1);
	sip_dns_prefer_family_set(&dns, AF_INET);
	h = sip_dns_resolve(&dns, "dual.example.com", SIP_DNS_ADDR, 5060, 6);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "198.51.100.7", 300);
	ep_inject(h, &w, &ep.server);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_aaaa(&w, ep.qname, "2001:db8::7", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK(!ep.last.cached);
	CHECK(!strcmp(ep.last.target[0].host, "198.51.100.7"));
	sip_dns_prefer_family_set(&dns, AF_INET6);
	h = sip_dns_resolve(&dns, "dual.example.com", SIP_DNS_ADDR, 5060, 7);
	ep_advance(0);
	CHECK(ep.last.cached);
	CHECK_UINT(ep.last.count, 2);
	/* the preference of the moment, not the earlier one */
	CHECK(!strcmp(ep.last.target[0].host, "2001:db8::7"));
	CHECK(!strcmp(ep.last.target[1].host, "198.51.100.7"));
	sip_dns_done(&dns);

	CASE("family: the canonical cache order is unchanged, so "
	     "changed stays false");
	ep_begin(1, 1000, true, 1);
	sip_dns_prefer_family_set(&dns, AF_INET6);
	h = sip_dns_resolve(&dns, "steady.example.com", SIP_DNS_ADDR, 5060, 8);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "198.51.100.8", 15);
	ep_inject(h, &w, &ep.server);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_aaaa(&w, ep.qname, "2001:db8::8", 15);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK(!ep.last.changed);
	CHECK(!strcmp(ep.last.target[0].host, "2001:db8::8"));
	ep_advance(16000);			/* let the 15 s ttl expire */
	sip_dns_prefer_family_set(&dns, AF_INET);
	h = sip_dns_resolve(&dns, "steady.example.com", SIP_DNS_ADDR, 5060, 9);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "198.51.100.8", 15);
	ep_inject(h, &w, &ep.server);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_aaaa(&w, ep.qname, "2001:db8::8", 15);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK(!ep.last.cached);
	/* another delivery order, and the same answer */
	CHECK(!strcmp(ep.last.target[0].host, "198.51.100.8"));
	CHECK(!ep.last.changed);
	CHECK_UINT(sip_dns_stat(&dns)->changed, 0);
	sip_dns_done(&dns);

	CASE("family: a preference set after the arm and before the delivery "
	     "is honoured");
	ep_begin(1, 1000, true, 1);
	h = sip_dns_resolve(&dns, "late.example.com", SIP_DNS_ADDR, 5060, 10);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "198.51.100.11", 300);
	ep_inject(h, &w, &ep.server);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_aaaa(&w, ep.qname, "2001:db8::11", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK(!strcmp(ep.last.target[0].host, "198.51.100.11"));
	h = sip_dns_resolve(&dns, "late.example.com", SIP_DNS_ADDR, 5060, 11);
	CHECK_UINT(ep.dones, 1);	/* armed, with no preference yet */
	sip_dns_prefer_family_set(&dns, AF_INET6);
	ep_advance(0);
	CHECK_UINT(ep.dones, 2);
	CHECK(ep.last.cached);
	CHECK_UINT(ep.last.count, 2);
	CHECK(!strcmp(ep.last.target[0].host, "2001:db8::11"));
	CHECK(!strcmp(ep.last.target[1].host, "198.51.100.11"));
	sip_dns_done(&dns);
}

/* ------------------------------------------------------------ glue chase */

/*
 * The SRV glue chase with cfg.ipv6 set asks both families per target. The
 * sticky target walk of the registration layer can therefore move from a dead
 * v6 address to the v4 address of the same host.
 */
static void test_glue_family(void)
{
	struct wire w;
	uint32_t h;

	CASE("glue: an SRV target with no glue is asked for A and AAAA");
	ep_begin(1, 1000, true, 1);
	h = sip_dns_resolve(&dns, "example.com", SIP_DNS_SRV, 5060, 10);
	CHECK_UINT(ep.qtype, DNS_T_SRV);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 0, 5062, "a.example.com", 300);
	ep_inject(h, &w, &ep.server);
	CHECK(!strcmp(ep.qname, "a.example.com"));
	CHECK_UINT(ep.qtype, DNS_T_A);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "203.0.113.10", 300);
	ep_inject(h, &w, &ep.server);
	CHECK(!strcmp(ep.qname, "a.example.com"));
	CHECK_UINT(ep.qtype, DNS_T_AAAA);	/* the second family */
	CHECK_UINT(ep.sends, 3);
	CHECK_UINT(ep.dones, 0);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_aaaa(&w, ep.qname, "2001:db8::10", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.count, 2);
	sip_dns_done(&dns);

	CASE("glue: a target whose additional section carried A is asked "
	     "for AAAA alone");
	ep_begin(1, 1000, true, 1);
	h = sip_dns_resolve(&dns, "example.com", SIP_DNS_SRV, 5060, 11);
	ep_reply_open(&w, ep.id, 1, 0, 1, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 0, 5062, "a.example.com", 300);
	w_rr_a(&w, "a.example.com", "203.0.113.10", 300);
	ep_inject(h, &w, &ep.server);
	CHECK_UINT(ep.sends, 2);
	CHECK(!strcmp(ep.qname, "a.example.com"));
	CHECK_UINT(ep.qtype, DNS_T_AAAA);	/* the A came with the answer */

	CASE("glue: both families of one target survive into the answer");
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_aaaa(&w, ep.qname, "2001:db8::10", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.count, 2);
	CHECK_UINT(ep.last.target[0].dst.sa.ss_family, AF_INET);
	CHECK(!strcmp(ep.last.target[0].host, "203.0.113.10"));
	CHECK_UINT(ep.last.target[1].dst.sa.ss_family, AF_INET6);
	CHECK(!strcmp(ep.last.target[1].host, "2001:db8::10"));
	CHECK_UINT(ep.last.target[1].port, 5062);	/* the SRV port */
	CHECK_UINT(ep.last.target[1].prio, 10);
	sip_dns_done(&dns);

	CASE("glue: a target with no address in either family is dropped");
	ep_begin(1, 1000, true, 1);
	h = sip_dns_resolve(&dns, "example.com", SIP_DNS_SRV, 5060, 12);
	ep_reply_open(&w, ep.id, 2, 0, 2, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 0, 5060, "a.example.com", 300);
	w_rr_srv(&w, ep.qname, 20, 0, 5061, "gone.example.com", 300);
	w_rr_a(&w, "a.example.com", "203.0.113.10", 300);
	w_rr_aaaa(&w, "a.example.com", "2001:db8::10", 300);
	ep_inject(h, &w, &ep.server);
	CHECK(!strcmp(ep.qname, "gone.example.com"));
	CHECK_UINT(ep.qtype, DNS_T_A);
	ep_advance(20000);		/* neither family of gone answers */
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 2);	/* the two addresses of a alone */
	CHECK(!strcmp(ep.last.target[0].host, "203.0.113.10"));
	CHECK(!strcmp(ep.last.target[1].host, "2001:db8::10"));
	sip_dns_done(&dns);

	CASE("glue: an AAAA glue that times out after the A answered "
	     "keeps the A");
	ep_begin(1, 1000, true, 1);
	h = sip_dns_resolve(&dns, "example.com", SIP_DNS_SRV, 5060, 13);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 0, 5060, "a.example.com", 300);
	ep_inject(h, &w, &ep.server);
	CHECK_UINT(ep.qtype, DNS_T_A);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "203.0.113.10", 300);
	ep_inject(h, &w, &ep.server);
	CHECK_UINT(ep.qtype, DNS_T_AAAA);
	CHECK_UINT(ep.dones, 0);
	ep_advance(20000);			/* the AAAA never answers */
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 1);
	CHECK(!strcmp(ep.last.target[0].host, "203.0.113.10"));
	sip_dns_done(&dns);

	CASE("glue: with ipv6 off the chase asks A alone");
	ep_begin(1, 1000, false, 1);
	h = sip_dns_resolve(&dns, "example.com", SIP_DNS_SRV, 5060, 14);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 0, 5060, "a.example.com", 300);
	ep_inject(h, &w, &ep.server);
	CHECK_UINT(ep.qtype, DNS_T_A);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "203.0.113.10", 300);
	ep_inject(h, &w, &ep.server);
	CHECK_UINT(ep.sends, 2);		/* no AAAA question followed */
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.count, 1);
	sip_dns_done(&dns);

	/*
	 * A walk that ends with no address reports the reason its questions
	 * gave. The account layer logs that word, so "the zone says the target
	 * does not exist" must not read as "the answers held no record".
	 */
	CASE("glue: a walk whose every question ended NXDOMAIN reports "
	     "NXDOMAIN");
	ep_begin(1, 1000, false, 1);
	h = sip_dns_resolve(&dns, "example.com", SIP_DNS_SRV, 5060, 15);
	ep_reply_open(&w, ep.id, 2, 0, 0, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 0, 5060, "a.example.com", 300);
	w_rr_srv(&w, ep.qname, 20, 0, 5061, "b.example.com", 300);
	ep_inject(h, &w, &ep.server);
	CHECK(!strcmp(ep.qname, "a.example.com"));
	ep_reply_open(&w, ep.id, 0, 0, 0, DNS_RCODE_NXDOMAIN);
	ep_inject(h, &w, &ep.server);
	CHECK(!strcmp(ep.qname, "b.example.com"));
	ep_reply_open(&w, ep.id, 0, 0, 0, DNS_RCODE_NXDOMAIN);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_NXDOMAIN);
	CHECK_UINT(ep.last.count, 0);
	sip_dns_done(&dns);

	CASE("glue: a walk whose every question timed out reports the timeout");
	ep_begin(1, 1000, false, 1);
	h = sip_dns_resolve(&dns, "example.com", SIP_DNS_SRV, 5060, 16);
	ep_reply_open(&w, ep.id, 2, 0, 0, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 0, 5060, "a.example.com", 300);
	w_rr_srv(&w, ep.qname, 20, 0, 5061, "b.example.com", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(20000);		/* neither target ever answers */
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_TIMEOUT);
	CHECK_UINT(ep.last.count, 0);
	sip_dns_done(&dns);

	CASE("glue: a walk whose answers held no usable record reports the "
	     "format error");
	ep_begin(1, 1000, false, 1);
	h = sip_dns_resolve(&dns, "example.com", SIP_DNS_SRV, 5060, 17);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 0, 5060, "a.example.com", 300);
	ep_inject(h, &w, &ep.server);
	CHECK(!strcmp(ep.qname, "a.example.com"));
	/* the one record of the answer names another host */
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, "other.example.com", "203.0.113.10", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_FORMAT);
	CHECK_UINT(ep.last.count, 0);
	sip_dns_done(&dns);

	CASE("glue: one surviving address still reports OK with the reason "
	     "discarded");
	ep_begin(1, 1000, false, 1);
	h = sip_dns_resolve(&dns, "example.com", SIP_DNS_SRV, 5060, 18);
	ep_reply_open(&w, ep.id, 2, 0, 0, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 0, 5060, "a.example.com", 300);
	w_rr_srv(&w, ep.qname, 20, 0, 5061, "b.example.com", 300);
	ep_inject(h, &w, &ep.server);
	ep_reply_open(&w, ep.id, 0, 0, 0, DNS_RCODE_NXDOMAIN);
	ep_inject(h, &w, &ep.server);
	CHECK(!strcmp(ep.qname, "b.example.com"));
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "203.0.113.20", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 1);
	CHECK(!strcmp(ep.last.target[0].host, "203.0.113.20"));
	CHECK_UINT(ep.last.target[0].port, 5061);
	sip_dns_done(&dns);

	/* the reason of the walk, not the reason of the SRV answer in front */
	CASE("glue: the good SRV answer in front does not turn a full timeout "
	     "into a format error");
	ep_begin(1, 1000, false, 1);
	h = sip_dns_resolve(&dns, "example.com", SIP_DNS_SRV, 5060, 19);
	ep_reply_open(&w, ep.id, 1, 0, 1, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 0, 5060, "a.example.com", 300);
	/* the name server of the zone, which no SRV record names */
	w_rr_a(&w, "ns.example.com", "203.0.113.53", 300);
	ep_inject(h, &w, &ep.server);
	CHECK(!strcmp(ep.qname, "a.example.com"));
	ep_advance(20000);			/* the target never answers */
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_TIMEOUT);
	CHECK_UINT(ep.last.count, 0);
	sip_dns_done(&dns);
}

/* ------------------------------------------------------- captured answers */

/*
 * Answers of real servers in two field captures, replayed byte for byte.
 * Each array is the udp payload of one frame. Both captures carry example
 * host names and addresses in place of the field ones, with each name the
 * same length on the wire, so every length field and compression pointer
 * still holds.
 */

/*
 * The first capture, the SRV answer of voip.example.com: three records in
 * wire order priority 20, 99 and 10, each port 5080 and TTL 2, and no address
 * record.
 */
static const uint8_t cap_port_srv[] = {
	0x04, 0x7f, 0x81, 0x80, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
	0x04, 0x5f, 0x73, 0x69, 0x70, 0x04, 0x5f, 0x75, 0x64, 0x70, 0x04, 0x76,
	0x6f, 0x69, 0x70, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03,
	0x63, 0x6f, 0x6d, 0x00, 0x00, 0x21, 0x00, 0x01, 0xc0, 0x0c, 0x00, 0x21,
	0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x2a, 0x00, 0x14, 0x00, 0x64,
	0x13, 0xd8, 0x11, 0x76, 0x6f, 0x69, 0x70, 0x2d, 0x63, 0x6f, 0x72, 0x65,
	0x2d, 0x30, 0x34, 0x31, 0x2d, 0x67, 0x72, 0x72, 0x04, 0x7a, 0x6f, 0x6e,
	0x65, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03, 0x63, 0x6f,
	0x6d, 0x00, 0xc0, 0x0c, 0x00, 0x21, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x2a, 0x00, 0x63, 0x00, 0x64, 0x13, 0xd8, 0x11, 0x76, 0x6f, 0x69,
	0x70, 0x2d, 0x63, 0x6f, 0x72, 0x65, 0x2d, 0x30, 0x32, 0x31, 0x2d, 0x6c,
	0x61, 0x73, 0x04, 0x7a, 0x6f, 0x6e, 0x65, 0x07, 0x65, 0x78, 0x61, 0x6d,
	0x70, 0x6c, 0x65, 0x03, 0x63, 0x6f, 0x6d, 0x00, 0xc0, 0x0c, 0x00, 0x21,
	0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x2a, 0x00, 0x0a, 0x00, 0x64,
	0x13, 0xd8, 0x11, 0x76, 0x6f, 0x69, 0x70, 0x2d, 0x63, 0x6f, 0x72, 0x65,
	0x2d, 0x30, 0x33, 0x31, 0x2d, 0x64, 0x66, 0x77, 0x04, 0x7a, 0x6f, 0x6e,
	0x65, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03, 0x63, 0x6f,
	0x6d, 0x00,
};

/*
 * The first capture, the A answer of the priority 20 target, 203.0.113.25.
 */
static const uint8_t cap_port_a_grr[] = {
	0x04, 0x7f, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x11, 0x76, 0x6f, 0x69, 0x70, 0x2d, 0x63, 0x6f, 0x72, 0x65, 0x2d, 0x30,
	0x34, 0x31, 0x2d, 0x67, 0x72, 0x72, 0x04, 0x7a, 0x6f, 0x6e, 0x65, 0x07,
	0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03, 0x63, 0x6f, 0x6d, 0x00,
	0x00, 0x01, 0x00, 0x01, 0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
	0x29, 0x67, 0x00, 0x04, 0xcb, 0x00, 0x71, 0x19,
};

/*
 * The first capture, the A answer of the priority 10 target, 203.0.113.26.
 * The capture holds no answer for the priority 99 target.
 */
static const uint8_t cap_port_a_dfw[] = {
	0x04, 0x7f, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x11, 0x76, 0x6f, 0x69, 0x70, 0x2d, 0x63, 0x6f, 0x72, 0x65, 0x2d, 0x30,
	0x33, 0x31, 0x2d, 0x64, 0x66, 0x77, 0x04, 0x7a, 0x6f, 0x6e, 0x65, 0x07,
	0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03, 0x63, 0x6f, 0x6d, 0x00,
	0x00, 0x01, 0x00, 0x01, 0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
	0x25, 0x10, 0x00, 0x04, 0xcb, 0x00, 0x71, 0x1a,
};

/*
 * The second capture, the SRV answer of sip.example.com: two records in wire
 * order priority 60 and 50, an NS record of the zone in the authority
 * section, and the address of that name server in the additional section.
 * Neither of the two is a target.
 */
static const uint8_t cap_nx_srv[] = {
	0x18, 0x3b, 0x85, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x00, 0x01,
	0x04, 0x5f, 0x73, 0x69, 0x70, 0x04, 0x5f, 0x75, 0x64, 0x70, 0x03, 0x73,
	0x69, 0x70, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03, 0x63,
	0x6f, 0x6d, 0x00, 0x00, 0x21, 0x00, 0x01, 0xc0, 0x0c, 0x00, 0x21, 0x00,
	0x01, 0x00, 0x01, 0x51, 0x80, 0x00, 0x1d, 0x00, 0x3c, 0x00, 0x37, 0x13,
	0xc4, 0x05, 0x73, 0x73, 0x2d, 0x6e, 0x79, 0x03, 0x73, 0x69, 0x70, 0x07,
	0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03, 0x63, 0x6f, 0x6d, 0x00,
	0xc0, 0x0c, 0x00, 0x21, 0x00, 0x01, 0x00, 0x01, 0x51, 0x80, 0x00, 0x1e,
	0x00, 0x32, 0x00, 0x32, 0x13, 0xc4, 0x06, 0x73, 0x73, 0x2d, 0x63, 0x68,
	0x69, 0x03, 0x73, 0x69, 0x70, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c,
	0x65, 0x03, 0x63, 0x6f, 0x6d, 0x00, 0xc0, 0x6d, 0x00, 0x02, 0x00, 0x01,
	0x00, 0x01, 0x51, 0x80, 0x00, 0x06, 0x03, 0x6e, 0x73, 0x31, 0xc0, 0x6d,
	0xc0, 0x8a, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x51, 0x80, 0x00, 0x04,
	0xc0, 0x00, 0x02, 0x9b,
};

/*
 * The second capture, NXDOMAIN with an SOA for the A question of
 * ss-ny.sip.example.com, the priority 60 target.
 */
static const uint8_t cap_nx_a_ny[] = {
	0x18, 0x3b, 0x85, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
	0x05, 0x73, 0x73, 0x2d, 0x6e, 0x79, 0x03, 0x73, 0x69, 0x70, 0x07, 0x65,
	0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03, 0x63, 0x6f, 0x6d, 0x00, 0x00,
	0x01, 0x00, 0x01, 0xc0, 0x12, 0x00, 0x06, 0x00, 0x01, 0x00, 0x01, 0x51,
	0x80, 0x00, 0x21, 0x03, 0x6e, 0x73, 0x31, 0xc0, 0x12, 0x04, 0x72, 0x6f,
	0x6f, 0x74, 0xc0, 0x12, 0x00, 0x00, 0x01, 0x82, 0x00, 0x09, 0x3a, 0x80,
	0x00, 0x01, 0x51, 0x80, 0x00, 0x24, 0xea, 0x00, 0x00, 0x01, 0x51, 0x80,
};

/*
 * The second capture, NXDOMAIN with an SOA for the A question of
 * ss-chi.sip.example.com, the priority 50 target.
 */
static const uint8_t cap_nx_a_chi[] = {
	0x18, 0x3b, 0x85, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
	0x06, 0x73, 0x73, 0x2d, 0x63, 0x68, 0x69, 0x03, 0x73, 0x69, 0x70, 0x07,
	0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03, 0x63, 0x6f, 0x6d, 0x00,
	0x00, 0x01, 0x00, 0x01, 0xc0, 0x13, 0x00, 0x06, 0x00, 0x01, 0x00, 0x01,
	0x51, 0x80, 0x00, 0x21, 0x03, 0x6e, 0x73, 0x31, 0xc0, 0x13, 0x04, 0x72,
	0x6f, 0x6f, 0x74, 0xc0, 0x13, 0x00, 0x00, 0x01, 0x82, 0x00, 0x09, 0x3a,
	0x80, 0x00, 0x01, 0x51, 0x80, 0x00, 0x24, 0xea, 0x00, 0x00, 0x01, 0x51,
	0x80,
};

/*
 * The two field captures, replayed from end to end. The first keeps the SRV
 * port over the configured one, and drops the target that its zone never
 * answers for. The second hands the caller the NXDOMAIN of its zone. It asks
 * nothing for the bare domain behind it (RFC 3263 section 4.2 has no such
 * fallback).
 */
static void test_replay(void)
{
	char asked[3][SIP_DNS_NAME_MAX];
	struct wire w;
	uint32_t h;

	CASE("replay: the first capture's SRV answer yields three targets on "
	     "port 5080");
	ep_begin(1, 1000, false, 1);
	h = sip_dns_resolve(&dns, "voip.example.com", SIP_DNS_SRV, 5060, 1);
	CHECK(!strcmp(ep.qname, "_sip._udp.voip.example.com"));
	CHECK_UINT(ep.qtype, DNS_T_SRV);
	ep_replay(h, &w, cap_port_srv, sizeof(cap_port_srv));
	snprintf(asked[0], sizeof(asked[0]), "%s", ep.qname);
	ep_replay(h, &w, cap_port_a_grr, sizeof(cap_port_a_grr));
	snprintf(asked[1], sizeof(asked[1]), "%s", ep.qname);
	/* the zone answered the third target outside the capture window */
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "192.0.2.99", 300);
	ep_inject(h, &w, &ep.server);
	snprintf(asked[2], sizeof(asked[2]), "%s", ep.qname);
	ep_replay(h, &w, cap_port_a_dfw, sizeof(cap_port_a_dfw));
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 3);
	CHECK(ep.last.srv);

	CASE("replay: its three glue questions follow in answer order, one per "
	     "target");
	CHECK_UINT(ep.sends, 4);	/* one SRV question, one per target */
	CHECK(!strcmp(asked[0], "voip-core-041-grr.zone.example.com"));
	CHECK(!strcmp(asked[1], "voip-core-021-las.zone.example.com"));
	CHECK(!strcmp(asked[2], "voip-core-031-dfw.zone.example.com"));

	CASE("replay: the caller's 5060 is overridden by the SRV port on every "
	     "target");
	CHECK_UINT(ep.last.target[0].port, 5080);
	CHECK_UINT(ep.last.target[1].port, 5080);
	CHECK_UINT(ep.last.target[2].port, 5080);

	CASE("replay: the answer is delivered in priority order 10, 20, 99");
	CHECK_UINT(ep.last.target[0].prio, 10);
	CHECK(!strcmp(ep.last.target[0].host, "203.0.113.26"));
	CHECK_UINT(ep.last.target[1].prio, 20);
	CHECK(!strcmp(ep.last.target[1].host, "203.0.113.25"));
	CHECK_UINT(ep.last.target[2].prio, 99);
	CHECK(!strcmp(ep.last.target[2].host, "192.0.2.99"));

	CASE("replay: its two second TTL is clamped up to the configured "
	     "floor");
	CHECK_UINT(ep.last.ttl, SIP_DNS_TTL_MIN);
	sip_dns_done(&dns);

	CASE("replay: a target the zone never answers for is dropped and the "
	     "rest stand");
	ep_begin(1, 1000, false, 1);
	h = sip_dns_resolve(&dns, "voip.example.com", SIP_DNS_SRV, 5060, 2);
	ep_replay(h, &w, cap_port_srv, sizeof(cap_port_srv));
	ep_replay(h, &w, cap_port_a_grr, sizeof(cap_port_a_grr));
	CHECK(!strcmp(ep.qname, "voip-core-021-las.zone.example.com"));
	ep_advance(2500);		/* the zone never answers for it */
	CHECK(!strcmp(ep.qname, "voip-core-031-dfw.zone.example.com"));
	ep_replay(h, &w, cap_port_a_dfw, sizeof(cap_port_a_dfw));
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 2);
	CHECK(!strcmp(ep.last.target[0].host, "203.0.113.26"));
	CHECK(!strcmp(ep.last.target[1].host, "203.0.113.25"));
	sip_dns_done(&dns);

	CASE("replay: the second capture's SRV answer takes neither the NS nor "
	     "its glue address");
	ep_begin(1, 1000, false, 1);
	h = sip_dns_resolve(&dns, "sip.example.com", SIP_DNS_SRV, 5060, 3);
	CHECK(!strcmp(ep.qname, "_sip._udp.sip.example.com"));
	ep_replay(h, &w, cap_nx_srv, sizeof(cap_nx_srv));
	snprintf(asked[0], sizeof(asked[0]), "%s", ep.qname);
	CHECK(strcmp(asked[0], "ns1.sip.example.com") != 0);
	CHECK_UINT(ep.sends, 2);	/* one question, for the first target */
	CHECK_UINT(ep.dones, 0);	/* no address of it finished the walk */

	CASE("replay: both SRV targets are asked for A in answer order");
	CHECK(!strcmp(asked[0], "ss-ny.sip.example.com"));
	CHECK_UINT(ep.qtype, DNS_T_A);
	ep_replay(h, &w, cap_nx_a_ny, sizeof(cap_nx_a_ny));
	snprintf(asked[1], sizeof(asked[1]), "%s", ep.qname);
	CHECK(!strcmp(asked[1], "ss-chi.sip.example.com"));
	CHECK_UINT(ep.qtype, DNS_T_A);

	CASE("replay: the captured NXDOMAIN answers drop both targets");
	ep_replay(h, &w, cap_nx_a_chi, sizeof(cap_nx_a_chi));
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.count, 0);

	CASE("replay: and no question for the bare domain follows them");
	CHECK_UINT(ep.sends, 3);	/* one SRV question, one per target */
	CHECK(!strcmp(ep.qname, "ss-chi.sip.example.com"));

	CASE("replay: the delivered status is the NXDOMAIN the zone gave, not "
	     "a format error");
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_NXDOMAIN);
	sip_dns_done(&dns);

	CASE("replay: with ipv6 on, an AAAA that times out does not downgrade "
	     "the NXDOMAIN");
	ep_begin(1, 1000, true, 1);
	h = sip_dns_resolve(&dns, "sip.example.com", SIP_DNS_SRV, 5060, 4);
	ep_replay(h, &w, cap_nx_srv, sizeof(cap_nx_srv));
	CHECK(!strcmp(ep.qname, "ss-ny.sip.example.com"));
	ep_replay(h, &w, cap_nx_a_ny, sizeof(cap_nx_a_ny));
	CHECK_UINT(ep.qtype, DNS_T_AAAA);
	ep_advance(2500);			/* the AAAA never answers */
	CHECK(!strcmp(ep.qname, "ss-chi.sip.example.com"));
	CHECK_UINT(ep.qtype, DNS_T_A);
	ep_replay(h, &w, cap_nx_a_chi, sizeof(cap_nx_a_chi));
	CHECK_UINT(ep.qtype, DNS_T_AAAA);
	ep_advance(2500);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_NXDOMAIN);
	CHECK_UINT(ep.last.count, 0);
	sip_dns_done(&dns);
}

/* -------------------------------------------------------- the ipv6 switch */

/*
 * The operator gate moves at run time. sip_dns_ipv6_set() flushes the answers
 * built under the other question set. A query already in flight cannot put a
 * v6 target back into the flushed cache.
 */
static void test_ipv6_switch(void)
{
	struct wire w;
	unsigned int i;
	uint32_t h;

	CASE("ipv6_set: turning the questions off flushes the cache");
	ep_begin(1, 1000, true, 1);
	h = sip_dns_resolve(&dns, "sip.example.com", SIP_DNS_A, 5060, 20);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "203.0.113.60", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(sip_dns_cache_count(&dns), 1);
	sip_dns_ipv6_set(&dns, false);
	CHECK_UINT(sip_dns_cache_count(&dns), 0);
	sip_dns_done(&dns);

	CASE("ipv6_set: an unchanged value keeps the cache");
	ep_begin(1, 1000, true, 1);
	h = sip_dns_resolve(&dns, "sip.example.com", SIP_DNS_A, 5060, 21);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "203.0.113.61", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(sip_dns_cache_count(&dns), 1);
	sip_dns_ipv6_set(&dns, true);		/* the value it already holds */
	CHECK_UINT(sip_dns_cache_count(&dns), 1);
	sip_dns_done(&dns);

	CASE("ipv6_set: turning the questions on mid-chase revisits "
	     "the records the chase passed");
	ep_begin(1, 1000, false, 1);
	h = sip_dns_resolve(&dns, "example.com", SIP_DNS_SRV, 5060, 25);
	ep_reply_open(&w, ep.id, 2, 0, 0, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 0, 5060, "a.example.com", 300);
	w_rr_srv(&w, ep.qname, 20, 0, 5061, "b.example.com", 300);
	ep_inject(h, &w, &ep.server);
	CHECK(!strcmp(ep.qname, "a.example.com"));
	CHECK_UINT(ep.qtype, DNS_T_A);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "203.0.113.10", 300);
	ep_inject(h, &w, &ep.server);
	CHECK(!strcmp(ep.qname, "b.example.com"));	/* the cursor moved */
	CHECK_UINT(ep.qtype, DNS_T_A);
	sip_dns_ipv6_set(&dns, true);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "203.0.113.20", 300);
	ep_inject(h, &w, &ep.server);
	CHECK(!strcmp(ep.qname, "a.example.com"));	/* and it came back */
	CHECK_UINT(ep.qtype, DNS_T_AAAA);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_aaaa(&w, ep.qname, "2001:db8::10", 300);
	ep_inject(h, &w, &ep.server);
	CHECK(!strcmp(ep.qname, "b.example.com"));
	CHECK_UINT(ep.qtype, DNS_T_AAAA);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_aaaa(&w, ep.qname, "2001:db8::20", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 4);
	CHECK(!strcmp(ep.last.target[0].host, "203.0.113.10"));
	CHECK(!strcmp(ep.last.target[1].host, "2001:db8::10"));
	CHECK(!strcmp(ep.last.target[2].host, "203.0.113.20"));
	CHECK(!strcmp(ep.last.target[3].host, "2001:db8::20"));
	sip_dns_done(&dns);

	CASE("ipv6_set: the questions turned off before an armed cache hit is "
	     "delivered drop its AAAA targets");
	ep_begin(1, 1000, true, 1);
	h = sip_dns_resolve(&dns, "dual.example.com", SIP_DNS_ADDR, 5060, 26);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "198.51.100.7", 300);
	ep_inject(h, &w, &ep.server);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_aaaa(&w, ep.qname, "2001:db8::7", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.last.count, 2);
	h = sip_dns_resolve(&dns, "dual.example.com", SIP_DNS_ADDR, 5060, 27);
	CHECK_UINT(ep.dones, 1);		/* the cache hit is armed */
	sip_dns_ipv6_set(&dns, false);
	ep_advance(0);
	CHECK_UINT(ep.dones, 2);
	CHECK(ep.last.cached);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 1);
	CHECK(!strcmp(ep.last.target[0].host, "198.51.100.7"));
	sip_dns_done(&dns);

	CASE("ipv6_set: the questions turned off before a fresh answer is "
	     "delivered drop its AAAA targets");
	ep_begin(1, 1000, true, 1);
	h = sip_dns_resolve(&dns, "fresh.example.com", SIP_DNS_ADDR, 5060, 28);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "198.51.100.9", 300);
	ep_inject(h, &w, &ep.server);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_aaaa(&w, ep.qname, "2001:db8::9", 300);
	ep_inject(h, &w, &ep.server);
	CHECK_UINT(ep.dones, 0);		/* the answer is armed */
	sip_dns_ipv6_set(&dns, false);
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 1);
	CHECK(!strcmp(ep.last.target[0].host, "198.51.100.9"));
	CHECK_UINT(sip_dns_cache_count(&dns), 0);	/* the flush ran */
	sip_dns_done(&dns);

	CASE("ipv6_set: a v6 literal armed before the questions turn off is "
	     "still delivered");
	ep_begin(1, 1000, true, 1);
	h = sip_dns_resolve(&dns, "2001:db8::5", SIP_DNS_ADDR, 5060, 29);
	CHECK(h != SIP_HANDLE_NONE);
	CHECK_UINT(ep.sends, 0);
	CHECK_UINT(ep.dones, 0);
	sip_dns_ipv6_set(&dns, false);
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 1);
	CHECK(!strcmp(ep.last.target[0].host, "2001:db8::5"));
	CHECK_UINT(ep.last.target[0].dst.sa.ss_family, AF_INET6);
	sip_dns_done(&dns);

	CASE("finish: with ipv6 off an AAAA collected earlier is dropped "
	     "before the cache");
	ep_begin(1, 1000, true, 1);
	h = sip_dns_resolve(&dns, "example.com", SIP_DNS_SRV, 5060, 22);
	ep_reply_open(&w, ep.id, 2, 0, 2, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 0, 5060, "a.example.com", 300);
	w_rr_srv(&w, ep.qname, 20, 0, 5061, "b.example.com", 300);
	w_rr_a(&w, "a.example.com", "203.0.113.10", 300);
	w_rr_aaaa(&w, "a.example.com", "2001:db8::10", 300);
	ep_inject(h, &w, &ep.server);
	CHECK(!strcmp(ep.qname, "b.example.com"));	/* the chase runs */
	sip_dns_ipv6_set(&dns, false);
	CHECK_UINT(sip_dns_cache_count(&dns), 0);
	ep_advance(20000);			/* b never answers */
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 1);
	CHECK_UINT(ep.last.target[0].dst.sa.ss_family, AF_INET);
	CHECK(!strcmp(ep.last.target[0].host, "203.0.113.10"));
	CHECK_UINT(sip_dns_cache_count(&dns), 1);
	h = sip_dns_resolve(&dns, "example.com", SIP_DNS_SRV, 5060, 23);
	ep_advance(0);
	CHECK(ep.last.cached);
	CHECK_UINT(ep.last.count, 1);	/* the entry holds no v6 target */
	CHECK(!strcmp(ep.last.target[0].host, "203.0.113.10"));
	sip_dns_done(&dns);

	CASE("targets: a four record zone with both families fits sixteen");
	ep_begin(1, 1000, true, 1);
	sip_dns_prefer_family_set(&dns, AF_INET6);
	h = sip_dns_resolve(&dns, "ex.net", SIP_DNS_SRV, 5060, 24);
	ep_reply_open(&w, ep.id, 4, 0, 16, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 0, 5060, "a.ex.net", 300);
	w_rr_srv(&w, ep.qname, 20, 0, 5060, "b.ex.net", 300);
	w_rr_srv(&w, ep.qname, 30, 0, 5060, "c.ex.net", 300);
	w_rr_srv(&w, ep.qname, 40, 0, 5060, "d.ex.net", 300);
	w_rr_a(&w, "a.ex.net", "203.0.113.1", 300);
	w_rr_a(&w, "a.ex.net", "203.0.113.2", 300);
	w_rr_aaaa(&w, "a.ex.net", "2001:db8::a1", 300);
	w_rr_aaaa(&w, "a.ex.net", "2001:db8::a2", 300);
	w_rr_a(&w, "b.ex.net", "203.0.113.11", 300);
	w_rr_a(&w, "b.ex.net", "203.0.113.12", 300);
	w_rr_aaaa(&w, "b.ex.net", "2001:db8::b1", 300);
	w_rr_aaaa(&w, "b.ex.net", "2001:db8::b2", 300);
	w_rr_a(&w, "c.ex.net", "203.0.113.21", 300);
	w_rr_a(&w, "c.ex.net", "203.0.113.22", 300);
	w_rr_aaaa(&w, "c.ex.net", "2001:db8::c1", 300);
	w_rr_aaaa(&w, "c.ex.net", "2001:db8::c2", 300);
	w_rr_a(&w, "d.ex.net", "203.0.113.31", 300);
	w_rr_a(&w, "d.ex.net", "203.0.113.32", 300);
	w_rr_aaaa(&w, "d.ex.net", "2001:db8::d1", 300);
	w_rr_aaaa(&w, "d.ex.net", "2001:db8::d3", 300);
	ep_inject(h, &w, &ep.server);
	CHECK_UINT(ep.sends, 1);	/* every record carried its glue */
	ep_advance(0);
	CHECK_UINT(SIP_DNS_TARGET_MAX, 16);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 16);
	CHECK(!strcmp(ep.last.target[0].host, "2001:db8::a1"));
	CHECK(!strcmp(ep.last.target[1].host, "2001:db8::a2"));
	CHECK(!strcmp(ep.last.target[2].host, "203.0.113.1"));
	CHECK(!strcmp(ep.last.target[3].host, "203.0.113.2"));
	CHECK(!strcmp(ep.last.target[12].host, "2001:db8::d1"));
	CHECK(!strcmp(ep.last.target[13].host, "2001:db8::d3"));
	CHECK(!strcmp(ep.last.target[14].host, "203.0.113.31"));
	CHECK(!strcmp(ep.last.target[15].host, "203.0.113.32"));
	sip_dns_done(&dns);

	/*
	 * Nine records without glue, A then AAAA each. The A of every record
	 * lands in the record's own slot, and each AAAA appends, so the array
	 * is full after the AAAA of record 6 and the last two records keep
	 * their A alone.
	 */
	CASE("targets: a nine record zone with both families keeps sixteen "
	     "addresses and drops the rest");
	ep_begin(1, 1000, true, 1);
	h = sip_dns_resolve(&dns, "ex.net", SIP_DNS_SRV, 5060, 30);
	ep_reply_open(&w, ep.id, 9, 0, 0, DNS_RCODE_OK);
	for (i = 0; i < 9; i++) {
		uint16_t prio = (uint16_t)(10 + i);
		char target[32];

		snprintf(target, sizeof(target), "n%u.ex.net", i);
		w_rr_srv(&w, ep.qname, prio, 0, 5060, target, 300);
	}
	ep_inject(h, &w, &ep.server);
	for (i = 0; i < 18; i++) {
		unsigned int rec = (unsigned int)(ep.qname[1] - '0');
		char addr[40];

		CHECK(rec < 9);
		ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
		if (ep.qtype == DNS_T_AAAA) {
			snprintf(addr, sizeof(addr), "2001:db8::%u", rec + 1);
			w_rr_aaaa(&w, ep.qname, addr, 300);
		} else {
			snprintf(addr, sizeof(addr), "203.0.113.%u", rec + 1);
			w_rr_a(&w, ep.qname, addr, 300);
		}
		ep_inject(h, &w, &ep.server);
	}
	CHECK_UINT(ep.sends, 19);	/* the SRV question and 18 chases */
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 16);
	for (i = 0; i < 7; i++) {
		char want[40];

		snprintf(want, sizeof(want), "203.0.113.%u", i + 1);
		CHECK(!strcmp(ep.last.target[2 * i].host, want));
		snprintf(want, sizeof(want), "2001:db8::%u", i + 1);
		CHECK(!strcmp(ep.last.target[2 * i + 1].host, want));
	}
	CHECK(!strcmp(ep.last.target[14].host, "203.0.113.8"));
	CHECK(!strcmp(ep.last.target[15].host, "203.0.113.9"));
	sip_dns_done(&dns);
}

/* ------------------------------------------------------- numeric targets */

/*
 * RFC 3263 section 4: a target that is already an address is used directly.
 * Every case here asserts the wire stayed silent, because a resolver that
 * sends "127.0.0.1" to a nameserver gets nxdomain for a usable address.
 */
static void test_literal(void)
{
	struct wire w;
	uint32_t h;

	CASE("literal: an IPv4 target answers with no question on the wire");
	ep_begin(2, 1000, false, 2);
	h = sip_dns_resolve(&dns, "127.0.0.1", SIP_DNS_A, 5060, 1);
	CHECK(h != SIP_HANDLE_NONE);
	CHECK_UINT(ep.sends, 0);
	CHECK_UINT(ep.dones, 0);		/* done never runs inside resolve */
	CHECK_UINT(sip_dns_query_count(&dns), 1);
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 1);
	CHECK(!strcmp(ep.last.target[0].host, "127.0.0.1"));
	CHECK_UINT(ep.last.target[0].port, 5060);
	CHECK_UINT(ep.last.target[0].prio, 0);
	CHECK_UINT(ep.last.target[0].weight, 0);
	CHECK_UINT(ep.last.ttl, 3600);		/* the literal ttl */
	CHECK(!ep.last.changed);
	CHECK_UINT(ep.sends, 0);		/* still nothing sent */
	CHECK_UINT(sip_dns_stat(&dns)->tx, 0);
	CHECK_UINT(sip_dns_query_count(&dns), 0);
	sip_dns_done(&dns);

	CASE("literal: an SRV lookup of an address delivers it, with no SRV question");
	ep_begin(2, 1000, false, 2);
	h = sip_dns_resolve(&dns, "192.0.2.10", SIP_DNS_SRV, 5062, 2);
	CHECK(h != SIP_HANDLE_NONE);
	CHECK_UINT(ep.sends, 0);
	CHECK_UINT(strlen(ep.qname), 0);	/* no question was ever built */
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 1);
	CHECK(!strcmp(ep.last.target[0].host, "192.0.2.10"));
	CHECK_UINT(ep.last.target[0].port, 5062);	/* the port the caller asked */
	CHECK_UINT(ep.sends, 0);
	/* a literal has no zone, so no SRV record can have built it */
	CHECK(!ep.last.srv);
	sip_dns_done(&dns);

	CASE("literal: an IPv6 target answers directly and keeps its family");
	ep_begin(2, 1000, true, 1);
	h = sip_dns_resolve(&dns, "2001:db8::1", SIP_DNS_ADDR, 5060, 3);
	CHECK(h != SIP_HANDLE_NONE);
	CHECK_UINT(ep.sends, 0);
	ep_advance(0);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 1);
	CHECK(!strcmp(ep.last.target[0].host, "2001:db8::1"));
	CHECK_UINT(ep.last.target[0].dst.sa.ss_family, AF_INET6);
	CHECK_UINT(ep.last.target[0].port, 5060);
	sip_dns_done(&dns);

	/*
	 * cfg.ipv6 picks the question for a name, an AAAA next to the A. A
	 * literal asks nothing, so it does not consult that flag.
	 */
	CASE("literal: an IPv6 target does not depend on cfg.ipv6");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "2001:db8::2", SIP_DNS_SRV, 5060, 4);
	CHECK(h != SIP_HANDLE_NONE);
	CHECK_UINT(ep.sends, 0);
	ep_advance(0);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 1);
	CHECK(!strcmp(ep.last.target[0].host, "2001:db8::2"));
	CHECK_UINT(ep.last.target[0].dst.sa.ss_family, AF_INET6);
	sip_dns_done(&dns);

	CASE("literal: the delivery waits for the timer, two of them at once");
	ep_begin(2, 1000, false, 1);
	CHECK(sip_dns_resolve(&dns, "198.51.100.1", SIP_DNS_A, 5060, 5) !=
	      SIP_HANDLE_NONE);
	CHECK(sip_dns_resolve(&dns, "198.51.100.2", SIP_DNS_A, 5060, 6) !=
	      SIP_HANDLE_NONE);
	CHECK_UINT(sip_dns_query_count(&dns), 2);
	CHECK_UINT(ep.dones, 0);		/* neither ran inside resolve */
	ep_advance(0);
	CHECK_UINT(ep.dones, 2);
	CHECK_UINT(sip_dns_query_count(&dns), 0);
	sip_dns_done(&dns);

	CASE("literal: an owner released before the timer gets no callback");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "203.0.113.5", SIP_DNS_A, 5060, 77);
	CHECK(h != SIP_HANDLE_NONE);
	CHECK_UINT(sip_dns_query_count(&dns), 1);
	CHECK_UINT(sip_dns_owner_release(&dns, 77), 1);
	CHECK_UINT(sip_dns_query_count(&dns), 0);
	ep_advance(10000);
	CHECK_UINT(ep.dones, 0);
	sip_dns_done(&dns);

	CASE("literal: a cancelled query never delivers, and its slot comes back");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "203.0.113.6", SIP_DNS_A, 5060, 7);
	sip_dns_cancel(&dns, h);
	CHECK_UINT(sip_dns_query_count(&dns), 0);
	ep_advance(10000);
	CHECK_UINT(ep.dones, 0);
	{
		uint32_t reuse = sip_dns_resolve(&dns, "203.0.113.6", SIP_DNS_A, 5060, 8);

		CHECK(reuse != SIP_HANDLE_NONE);
		CHECK_UINT(sip_handle_index(reuse), sip_handle_index(h));
		CHECK(reuse != h);		/* same slot, new generation */
		ep_advance(0);
		CHECK_UINT(ep.dones, 1);
		CHECK_UINT(ep.last_query, reuse);
		CHECK_UINT(ep.last_owner, 8);
	}
	sip_dns_done(&dns);

	CASE("literal: changed is false on the first answer and on a repeat");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "192.0.2.20", SIP_DNS_A, 5060, 9);
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK(!ep.last.changed);
	ep_advance(60000);
	h = sip_dns_resolve(&dns, "192.0.2.20", SIP_DNS_A, 5060, 10);
	CHECK(h != SIP_HANDLE_NONE);
	ep_advance(0);
	CHECK_UINT(ep.dones, 2);
	CHECK(!ep.last.changed);		/* the address is inside the name */
	CHECK_UINT(ep.last.ttl, 3600);		/* and the ttl does not run down */
	sip_dns_done(&dns);

	CASE("literal: the cache stays empty in both directions");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "192.0.2.30", SIP_DNS_SRV, 5060, 11);
	ep_advance(0);
	h = sip_dns_resolve(&dns, "192.0.2.30", SIP_DNS_SRV, 5060, 12);
	ep_advance(0);
	CHECK_UINT(ep.dones, 2);
	CHECK_UINT(sip_dns_cache_count(&dns), 0);
	CHECK_UINT(sip_dns_stat(&dns)->cache_hits, 0);
	sip_dns_done(&dns);

	CASE("literal: a literal does not evict the entry of a real name");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "sip.example.com", SIP_DNS_A, 5060, 13);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "203.0.113.40", 100);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(sip_dns_cache_count(&dns), 1);
	h = sip_dns_resolve(&dns, "192.0.2.40", SIP_DNS_A, 5060, 14);
	ep_advance(0);
	CHECK_UINT(sip_dns_cache_count(&dns), 1);	/* the literal added nothing */
	h = sip_dns_resolve(&dns, "sip.example.com", SIP_DNS_A, 5060, 15);
	ep_advance(0);
	CHECK(ep.last.cached);			/* the name still comes from the cache */
	CHECK(!strcmp(ep.last.target[0].host, "203.0.113.40"));
	CHECK_UINT(ep.sends, 1);		/* one question in the whole case */
	sip_dns_done(&dns);

	CASE("literal: an address resolves with no nameserver at all");
	ep_begin(2, 1000, false, 0);
	CHECK_UINT(sip_dns_server_count(&dns), 0);
	h = sip_dns_resolve(&dns, "sip.example.com", SIP_DNS_A, 5060, 16);
	ep_advance(0);
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_NO_SERVER);	/* a name cannot */
	h = sip_dns_resolve(&dns, "127.0.0.1", SIP_DNS_SRV, 5060, 17);
	ep_advance(0);
	CHECK_UINT(ep.dones, 2);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);		/* the address still can */
	CHECK(!strcmp(ep.last.target[0].host, "127.0.0.1"));
	CHECK_UINT(ep.sends, 0);
	sip_dns_done(&dns);

	CASE("literal: a five part dotted name is a name, and goes to DNS");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "1.2.3.4.5", SIP_DNS_A, 5060, 18);
	CHECK(h != SIP_HANDLE_NONE);
	CHECK_UINT(ep.sends, 1);		/* it asked */
	CHECK(!strcmp(ep.qname, "1.2.3.4.5"));
	CHECK_UINT(ep.qtype, DNS_T_A);
	CHECK_UINT(ep.dones, 0);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "203.0.113.50", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK(!strcmp(ep.last.target[0].host, "203.0.113.50"));
	sip_dns_done(&dns);

	CASE("literal: a trailing character makes it a name, and goes to DNS");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "127.0.0.1x", SIP_DNS_A, 5060, 19);
	CHECK(h != SIP_HANDLE_NONE);
	CHECK_UINT(ep.sends, 1);
	CHECK(!strcmp(ep.qname, "127.0.0.1x"));
	CHECK_UINT(ep.dones, 0);
	ep_advance(10000);			/* nobody answers a made up name */
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_TIMEOUT);
	sip_dns_done(&dns);

	CASE("literal: a three part dotted name is a name, and goes to DNS");
	ep_begin(2, 1000, false, 1);
	h = sip_dns_resolve(&dns, "1.2.3", SIP_DNS_SRV, 5060, 20);
	CHECK(h != SIP_HANDLE_NONE);
	CHECK_UINT(ep.sends, 1);
	CHECK(!strcmp(ep.qname, "_sip._udp.1.2.3"));	/* the SRV question */
	CHECK_UINT(ep.qtype, DNS_T_SRV);
	sip_dns_done(&dns);
}

/*
 * The service label is part of the cache identity (shared/resolve.h "Cache").
 * These two cases check the anti-alias property that the label exists to
 * create, and the rule of sip_dns_resolve_svc() that NULL and "" take the
 * default.
 */
static void test_service_label(void)
{
	struct wire w;
	uint32_t h;

	CASE("service: _sip._tcp and _sips._tcp on one name do not share a cache entry");
	ep_begin(2, 1000, false, 1);

	h = sip_dns_resolve_svc(&dns, "alias.example.net", SIP_DNS_SRV, 5060, 30,
				SIP_DNS_SERVICE_TCP);
	CHECK(h != SIP_HANDLE_NONE);
	CHECK(!strcmp(ep.qname, "_sip._tcp.alias.example.net"));
	CHECK_UINT(ep.qtype, DNS_T_SRV);
	ep_reply_open(&w, ep.id, 1, 0, 1, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 60, 5061, "tcp.example.net", 300);
	w_rr_a(&w, "tcp.example.net", "203.0.113.11", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 1);
	CHECK(!strcmp(ep.last.target[0].host, "203.0.113.11"));

	{
		unsigned int sends_after_tcp = ep.sends;

		h = sip_dns_resolve_svc(&dns, "alias.example.net", SIP_DNS_SRV, 5060, 31,
					SIP_DNS_SERVICE_TLS);
		CHECK(h != SIP_HANDLE_NONE);
		CHECK(ep.sends > sends_after_tcp);	/* not served from the TCP cache */
		CHECK(!strcmp(ep.qname, "_sips._tcp.alias.example.net"));
	}
	ep_reply_open(&w, ep.id, 1, 0, 1, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 60, 5061, "tls.example.net", 300);
	w_rr_a(&w, "tls.example.net", "203.0.113.22", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 1);
	CHECK(!strcmp(ep.last.target[0].host, "203.0.113.22"));	/* its own answer */

	{
		unsigned int sends_before = ep.sends;

		h = sip_dns_resolve_svc(&dns, "alias.example.net", SIP_DNS_SRV, 5060, 32,
					SIP_DNS_SERVICE_TCP);
		ep_advance(0);
		CHECK(ep.last.cached);			/* the TCP entry still stands */
		CHECK_UINT(ep.sends, sends_before);	/* served without the wire */
		CHECK_UINT(ep.last.count, 1);
		CHECK(!strcmp(ep.last.target[0].host, "203.0.113.11"));
	}
	sip_dns_done(&dns);

	CASE("service: NULL, the default label, and \"\" share one cache identity");
	ep_begin(2, 1000, false, 1);

	h = sip_dns_resolve(&dns, "svc.example.net", SIP_DNS_SRV, 5060, 40);
	CHECK(!strcmp(ep.qname, "_sip._udp.svc.example.net"));	/* cfg.service default */
	ep_reply_open(&w, ep.id, 1, 0, 1, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 60, 5061, "node.example.net", 300);
	w_rr_a(&w, "node.example.net", "203.0.113.30", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK(!strcmp(ep.last.target[0].host, "203.0.113.30"));
	CHECK(!ep.last.cached);

	{
		unsigned int sends_before = ep.sends;

		h = sip_dns_resolve_svc(&dns, "svc.example.net", SIP_DNS_SRV, 5060, 41,
					SIP_DNS_SERVICE_DEFAULT);
		ep_advance(0);
		CHECK(ep.last.cached);			/* explicit default hit the NULL entry */
		CHECK_UINT(ep.sends, sends_before);
		CHECK(!strcmp(ep.last.target[0].host, "203.0.113.30"));
	}

	{
		unsigned int sends_before = ep.sends;

		h = sip_dns_resolve_svc(&dns, "svc.example.net", SIP_DNS_SRV, 5060, 42, "");
		ep_advance(0);
		CHECK(ep.last.cached);			/* "" is the default, not a "" identity */
		CHECK_UINT(ep.sends, sends_before);
		CHECK(!strcmp(ep.last.target[0].host, "203.0.113.30"));
	}
	sip_dns_done(&dns);
}

/* ------------------------------------------------------- address, then SRV */

static uint32_t as_resolve(const char *name, uint32_t owner)
{
	return sip_dns_resolve(&dns, name, SIP_DNS_ADDR_SRV, 5060, owner);
}

/* answer the question in flight with nothing, NOERROR or the given rcode */
static void ep_reply_empty(uint32_t h, uint16_t rcode)
{
	struct wire w;

	ep_reply_open(&w, ep.id, 0, 0, 0, rcode);
	ep_inject(h, &w, &ep.server);
}

/* answer the SRV question in flight with one record and its glue */
static void ep_reply_srv_glue(uint32_t h, const char *target, const char *addr)
{
	struct wire w;

	ep_reply_open(&w, ep.id, 1, 0, 1, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 0, 5070, target, 300);
	w_rr_a(&w, target, addr, 300);
	ep_inject(h, &w, &ep.server);
}

/*
 * SIP_DNS_ADDR_SRV asks the address of the name first, and SRV only when every
 * address question got a definitive negative: NXDOMAIN, or a whole NOERROR
 * answer that holds no record and no CNAME. A truncated answer of any rcode
 * and every other outcome can hide an address, and a name with an address
 * must get no SRV question.
 */
static void test_addr_srv(void)
{
	char long_name[SIP_DNS_NAME_MAX];
	unsigned int sends;
	struct wire w;
	uint32_t h;

	CASE("addr_srv: a name with an address asks no SRV");
	ep_begin(1, 1000, false, 1);
	h = as_resolve("proxy.example.net", 1);
	CHECK_UINT(ep.qtype, DNS_T_A);
	CHECK(!strcmp(ep.qname, "proxy.example.net"));
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "203.0.113.40", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.sends, 1);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 1);
	CHECK_UINT(ep.last.target[0].port, 5060);
	CHECK(!ep.last.srv);
	sip_dns_done(&dns);

	CASE("addr_srv: with ipv6 on, an A answer and an empty AAAA "
	     "ask no SRV");
	ep_begin(1, 1000, true, 1);
	h = as_resolve("proxy.example.net", 31);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "203.0.113.47", 300);
	ep_inject(h, &w, &ep.server);
	CHECK_UINT(ep.qtype, DNS_T_AAAA);
	ep_reply_empty(h, DNS_RCODE_OK);
	ep_advance(0);
	CHECK_UINT(ep.sends, 2);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 1);
	CHECK(!ep.last.srv);
	sip_dns_done(&dns);

	/* the shape of a zone that names its service in SRV records alone */
	CASE("addr_srv: an empty answer asks SRV of the service label");
	ep_begin(1, 1000, false, 1);
	h = as_resolve("zone.example.net", 2);
	CHECK_UINT(ep.qtype, DNS_T_A);
	ep_reply_empty(h, DNS_RCODE_OK);
	CHECK_UINT(ep.qtype, DNS_T_SRV);
	CHECK(!strcmp(ep.qname, "_sip._udp.zone.example.net"));
	ep_reply_open(&w, ep.id, 2, 0, 2, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 50, 55, 5070, "is.zone.example.net", 3600);
	w_rr_srv(&w, ep.qname, 70, 60, 5070, "hsv.zone.example.net", 3600);
	w_rr_a(&w, "is.zone.example.net", "203.0.113.41", 1000);
	w_rr_a(&w, "hsv.zone.example.net", "198.51.100.42", 1000);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.sends, 2);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 2);
	/* priority 50 first, at the SRV port and not the caller's 5060 */
	CHECK(!strcmp(ep.last.target[0].host, "203.0.113.41"));
	CHECK_UINT(ep.last.target[0].port, 5070);
	CHECK_UINT(ep.last.target[0].prio, 50);
	CHECK(!strcmp(ep.last.target[1].host, "198.51.100.42"));
	CHECK(ep.last.srv);

	CASE("addr_srv: the answer is cached under its own kind");
	sends = ep.sends;
	h = as_resolve("zone.example.net", 3);
	ep_advance(0);
	CHECK_UINT(ep.sends, sends);
	CHECK(ep.last.cached);
	CHECK(ep.last.srv);
	CHECK_UINT(ep.last.target[0].port, 5070);

	CASE("addr_srv: a plain address lookup of the name does not share it");
	h = sip_dns_resolve(&dns, "zone.example.net", SIP_DNS_ADDR, 5060, 4);
	CHECK_UINT(ep.sends, sends + 1);
	CHECK_UINT(ep.qtype, DNS_T_A);
	sip_dns_cancel(&dns, h);

	CASE("addr_srv: another service label does not share it");
	h = sip_dns_resolve_svc(&dns, "zone.example.net", SIP_DNS_ADDR_SRV,
				5060, 5, SIP_DNS_SERVICE_TCP);
	CHECK_UINT(ep.sends, sends + 2);
	CHECK_UINT(ep.qtype, DNS_T_A);
	ep_reply_empty(h, DNS_RCODE_OK);
	CHECK(!strcmp(ep.qname, "_sip._tcp.zone.example.net"));
	sip_dns_cancel(&dns, h);

	CASE("addr_srv: an expired entry asks again and reads a move");
	sip_dns_cache_expire(&dns, "zone.example.net", SIP_DNS_ADDR_SRV, 5060,
			     NULL);
	sends = ep.sends;
	h = as_resolve("zone.example.net", 6);
	CHECK_UINT(ep.sends, sends + 1);
	CHECK_UINT(ep.qtype, DNS_T_A);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "192.0.2.43", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK(!ep.last.cached);
	CHECK(ep.last.changed);
	CHECK(!ep.last.srv);
	CHECK_UINT(ep.last.target[0].port, 5060);
	sip_dns_done(&dns);

	CASE("addr_srv: NXDOMAIN of the name asks SRV");
	ep_begin(1, 1000, false, 1);
	h = as_resolve("gone.example.net", 7);
	ep_reply_empty(h, DNS_RCODE_NXDOMAIN);
	CHECK_UINT(ep.qtype, DNS_T_SRV);
	CHECK(!strcmp(ep.qname, "_sip._udp.gone.example.net"));
	ep_reply_srv_glue(h, "t.example.net", "203.0.113.44");
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK(ep.last.srv);
	sip_dns_done(&dns);

	CASE("addr_srv: a timeout asks no SRV");
	ep_begin(1, 1000, false, 1);
	h = as_resolve("slow.example.net", 8);
	ep_advance(10000);
	CHECK_UINT(ep.qtype, DNS_T_A);
	CHECK_UINT(ep.sends, 2);		/* the A and its one retry */
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_TIMEOUT);
	sip_dns_done(&dns);

	CASE("addr_srv: a server failure asks no SRV");
	ep_begin(1, 1000, false, 1);
	h = as_resolve("fail.example.net", 9);
	ep_reply_empty(h, DNS_RCODE_SERVFAIL);
	ep_advance(0);
	CHECK_UINT(ep.sends, 1);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_SERVFAIL);
	sip_dns_done(&dns);

	CASE("addr_srv: a truncated empty answer asks no SRV");
	ep_begin(1, 1000, false, 1);
	h = as_resolve("tc.example.net", 10);
	ep_reply_open(&w, ep.id, 0, 0, 0, DNS_RCODE_OK);
	w.b[2] |= (uint8_t)(DNS_F_TC >> 8);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.sends, 1);
	CHECK_UINT(ep.dones, 1);
	CHECK(ep.last.truncated);
	CHECK(ep.last.status != SIP_DNS_OK);
	sip_dns_done(&dns);

	CASE("addr_srv: a truncated NXDOMAIN asks no SRV");
	ep_begin(1, 1000, false, 1);
	h = as_resolve("tcnx.example.net", 29);
	ep_reply_open(&w, ep.id, 0, 0, 0, DNS_RCODE_NXDOMAIN);
	w.b[2] |= (uint8_t)(DNS_F_TC >> 8);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.sends, 1);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_NXDOMAIN);
	sip_dns_done(&dns);

	CASE("addr_srv: a truncated CNAME answer blocks SRV after an empty "
	     "target");
	ep_begin(1, 1000, false, 1);
	h = as_resolve("tcalias.example.net", 30);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w.b[2] |= (uint8_t)(DNS_F_TC >> 8);
	w_rr_cname(&w, ep.qname, "edge.example.org", 60);
	ep_inject(h, &w, &ep.server);
	CHECK(!strcmp(ep.qname, "edge.example.org"));
	ep_reply_empty(h, DNS_RCODE_OK);
	ep_advance(0);
	CHECK_UINT(ep.qtype, DNS_T_A);
	CHECK_UINT(ep.sends, 2);
	CHECK_UINT(ep.dones, 1);
	CHECK(ep.last.truncated);
	sip_dns_done(&dns);

	CASE("addr_srv: an answer section that does not parse asks no SRV");
	ep_begin(1, 1000, false, 1);
	h = as_resolve("cut.example.net", 11);
	/* one answer record in the header, and no record bytes */
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.sends, 1);
	CHECK_UINT(ep.dones, 1);
	CHECK(ep.last.status != SIP_DNS_OK);
	sip_dns_done(&dns);

	CASE("addr_srv: a CNAME chain at its hop limit asks no SRV");
	ep_begin(1, 1000, false, 1);
	h = as_resolve("c0.example.net", 12);
	ep_reply_open(&w, ep.id, 5, 0, 0, DNS_RCODE_OK);
	w_rr_cname(&w, "c0.example.net", "c1.example.net", 60);
	w_rr_cname(&w, "c1.example.net", "c2.example.net", 60);
	w_rr_cname(&w, "c2.example.net", "c3.example.net", 60);
	w_rr_cname(&w, "c3.example.net", "c4.example.net", 60);
	w_rr_cname(&w, "c4.example.net", "c5.example.net", 60);
	ep_inject(h, &w, &ep.server);
	CHECK(!strcmp(ep.qname, "c4.example.net"));	/* the last hop taken */
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_cname(&w, "c4.example.net", "c5.example.net", 60);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.qtype, DNS_T_A);
	CHECK_UINT(ep.sends, 2);
	CHECK_UINT(ep.dones, 1);
	CHECK(ep.last.status != SIP_DNS_OK);
	sip_dns_done(&dns);

	CASE("addr_srv: a CNAME to no address asks SRV of the name asked");
	ep_begin(1, 1000, false, 1);
	h = as_resolve("alias.example.net", 13);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_cname(&w, ep.qname, "edge.example.org", 60);
	ep_inject(h, &w, &ep.server);
	CHECK(!strcmp(ep.qname, "edge.example.org"));
	CHECK_UINT(ep.qtype, DNS_T_A);
	ep_reply_empty(h, DNS_RCODE_OK);
	CHECK_UINT(ep.qtype, DNS_T_SRV);
	CHECK(!strcmp(ep.qname, "_sip._udp.alias.example.net"));
	sip_dns_done(&dns);

	CASE("addr_srv: a CNAME whose target times out asks no SRV");
	ep_begin(1, 1000, false, 1);
	h = as_resolve("alias.example.net", 14);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_cname(&w, ep.qname, "edge.example.org", 60);
	ep_inject(h, &w, &ep.server);
	ep_advance(10000);
	CHECK_UINT(ep.qtype, DNS_T_A);
	CHECK(!strcmp(ep.qname, "edge.example.org"));
	CHECK_UINT(ep.dones, 1);
	CHECK(ep.last.status != SIP_DNS_OK);
	sip_dns_done(&dns);

	CASE("addr_srv: with ipv6 on, an empty A and an empty AAAA ask SRV");
	ep_begin(1, 1000, true, 1);
	h = as_resolve("zone6.example.net", 15);
	ep_reply_empty(h, DNS_RCODE_OK);
	CHECK_UINT(ep.qtype, DNS_T_AAAA);
	ep_reply_empty(h, DNS_RCODE_OK);
	CHECK_UINT(ep.qtype, DNS_T_SRV);
	sip_dns_done(&dns);

	CASE("addr_srv: with ipv6 on, an AAAA answer after an empty A "
	     "asks no SRV");
	ep_begin(1, 1000, true, 1);
	h = as_resolve("zone6.example.net", 16);
	ep_reply_empty(h, DNS_RCODE_OK);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_aaaa(&w, ep.qname, "2001:db8::45", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.sends, 2);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK(!ep.last.srv);
	sip_dns_done(&dns);

	CASE("addr_srv: with ipv6 on, an empty A and an AAAA timeout "
	     "ask no SRV");
	ep_begin(1, 1000, true, 1);
	h = as_resolve("zone6.example.net", 17);
	ep_reply_empty(h, DNS_RCODE_OK);
	ep_advance(10000);
	CHECK_UINT(ep.qtype, DNS_T_AAAA);
	CHECK_UINT(ep.dones, 1);
	CHECK(ep.last.status != SIP_DNS_OK);
	sip_dns_done(&dns);

	CASE("addr_srv: with ipv6 on, an A timeout and an empty AAAA "
	     "ask no SRV");
	ep_begin(1, 1000, true, 1);
	h = as_resolve("zone6.example.net", 18);
	ep_advance(2000);		/* the A and its retry time out */
	CHECK_UINT(ep.qtype, DNS_T_AAAA);
	ep_reply_empty(h, DNS_RCODE_OK);
	ep_advance(0);
	CHECK_UINT(ep.qtype, DNS_T_AAAA);
	CHECK_UINT(ep.dones, 1);
	CHECK(ep.last.status != SIP_DNS_OK);
	sip_dns_done(&dns);

	CASE("addr_srv: with ipv6 on, an empty A and an AAAA server failure "
	     "ask no SRV");
	ep_begin(1, 1000, true, 1);
	h = as_resolve("zone6.example.net", 19);
	ep_reply_empty(h, DNS_RCODE_OK);
	ep_reply_empty(h, DNS_RCODE_SERVFAIL);
	ep_advance(0);
	CHECK_UINT(ep.qtype, DNS_T_AAAA);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_SERVFAIL);
	sip_dns_done(&dns);

	CASE("addr_srv: an empty SRV answer ends with no third question");
	ep_begin(1, 1000, false, 1);
	h = as_resolve("none.example.net", 20);
	ep_reply_empty(h, DNS_RCODE_OK);
	ep_reply_empty(h, DNS_RCODE_OK);
	ep_advance(0);
	CHECK_UINT(ep.sends, 2);
	CHECK_UINT(ep.qtype, DNS_T_SRV);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_FORMAT);
	sip_dns_done(&dns);

	/* the reason starts again for the SRV stage */
	CASE("addr_srv: an SRV timeout reports the timeout");
	ep_begin(1, 1000, false, 1);
	h = as_resolve("none.example.net", 21);
	ep_reply_empty(h, DNS_RCODE_OK);
	ep_advance(10000);
	CHECK_UINT(ep.qtype, DNS_T_SRV);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_TIMEOUT);
	sip_dns_done(&dns);

	CASE("addr_srv: SRV NXDOMAIN reports NXDOMAIN");
	ep_begin(1, 1000, false, 1);
	h = as_resolve("none.example.net", 22);
	ep_reply_empty(h, DNS_RCODE_OK);
	ep_reply_empty(h, DNS_RCODE_NXDOMAIN);
	ep_advance(0);
	CHECK_UINT(ep.sends, 2);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_NXDOMAIN);
	sip_dns_done(&dns);

	CASE("addr_srv: an SRV server failure reports the failure");
	ep_begin(1, 1000, false, 1);
	h = as_resolve("none.example.net", 23);
	ep_reply_empty(h, DNS_RCODE_OK);
	ep_reply_empty(h, DNS_RCODE_SERVFAIL);
	ep_advance(0);
	CHECK_UINT(ep.sends, 2);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_SERVFAIL);
	sip_dns_done(&dns);

	CASE("addr_srv: an SRV target of \".\" reports no-service");
	ep_begin(1, 1000, false, 1);
	h = as_resolve("none.example.net", 24);
	ep_reply_empty(h, DNS_RCODE_OK);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 0, 0, 0, "", 60);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_NO_SERVICE);
	sip_dns_done(&dns);

	/*
	 * The NOERROR answer that carried the CNAME already set the format
	 * reason, and a later timeout does not replace it (fail_note()). A
	 * plain SIP_DNS_SRV query reports the same. What this case pins is the
	 * end: one delivery, and no question for the address of the name.
	 */
	CASE("addr_srv: an SRV CNAME whose target times out asks no address");
	ep_begin(1, 1000, false, 1);
	h = as_resolve("none.example.net", 25);
	ep_reply_empty(h, DNS_RCODE_OK);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_cname(&w, ep.qname, "_sip._udp.other.example.org", 60);
	ep_inject(h, &w, &ep.server);
	CHECK(!strcmp(ep.qname, "_sip._udp.other.example.org"));
	ep_advance(10000);
	CHECK_UINT(ep.qtype, DNS_T_SRV);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_FORMAT);
	sip_dns_done(&dns);

	CASE("addr_srv: address, SRV and glue questions build the targets");
	ep_begin(1, 1000, true, 1);
	h = as_resolve("walk.example.net", 26);
	ep_reply_empty(h, DNS_RCODE_OK);			/* A */
	ep_reply_empty(h, DNS_RCODE_OK);			/* AAAA */
	CHECK_UINT(ep.qtype, DNS_T_SRV);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);	/* no glue */
	w_rr_srv(&w, ep.qname, 10, 0, 5070, "node.example.net", 300);
	ep_inject(h, &w, &ep.server);
	CHECK(!strcmp(ep.qname, "node.example.net"));
	CHECK_UINT(ep.qtype, DNS_T_A);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_a(&w, ep.qname, "203.0.113.46", 300);
	ep_inject(h, &w, &ep.server);
	CHECK_UINT(ep.qtype, DNS_T_AAAA);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_aaaa(&w, ep.qname, "2001:db8::46", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(0);
	CHECK_UINT(ep.sends, 5);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_OK);
	CHECK_UINT(ep.last.count, 2);
	CHECK_UINT(ep.last.target[0].port, 5070);
	CHECK_UINT(ep.last.target[1].port, 5070);
	CHECK(ep.last.srv);
	sip_dns_done(&dns);

	CASE("addr_srv: a glue walk where no target answers ends once");
	ep_begin(1, 1000, false, 1);
	h = as_resolve("walk.example.net", 27);
	ep_reply_empty(h, DNS_RCODE_OK);
	ep_reply_open(&w, ep.id, 1, 0, 0, DNS_RCODE_OK);
	w_rr_srv(&w, ep.qname, 10, 0, 5070, "node.example.net", 300);
	ep_inject(h, &w, &ep.server);
	ep_advance(20000);
	CHECK(!strcmp(ep.qname, "node.example.net"));	/* the last question */
	CHECK_UINT(ep.sends, 4);	/* A, SRV, and the glue A twice */
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_TIMEOUT);
	sip_dns_done(&dns);

	/* 4 labels of 60 bytes: the name fits, with "_sip._udp." it does not */
	memset(long_name, 'a', sizeof(long_name));
	long_name[60] = long_name[121] = long_name[182] = '.';
	snprintf(long_name + 243, sizeof(long_name) - 243, ".net");
	CASE("addr_srv: an SRV name over the limit fails after the address");
	ep_begin(1, 1000, false, 1);
	h = sip_dns_resolve(&dns, long_name, SIP_DNS_ADDR_SRV, 5060, 28);
	CHECK(h != SIP_HANDLE_NONE);
	CHECK_UINT(ep.qtype, DNS_T_A);
	ep_reply_empty(h, DNS_RCODE_OK);
	ep_advance(0);
	CHECK_UINT(ep.sends, 1);
	CHECK_UINT(ep.dones, 1);
	CHECK_UINT(ep.last.status, SIP_DNS_ERR_INTERNAL);
	sip_dns_done(&dns);

	CASE("addr_srv: the kind names itself");
	CHECK(!strcmp(sip_dns_kind_name(SIP_DNS_ADDR_SRV), "addr_srv"));
}

int main(void)
{
	/*
	 * A compression loop that hangs is a defect, so the suite bounds its own
	 * run time: SIGALRM kills the process and ctest reports the failure.
	 */
	alarm(60);

	test_name_write();
	test_name_read();
	test_name_eq();
	test_query_build();
	test_pkt_parse();
	test_rr_walk();
	test_extractors();
	test_cname_chain();
	test_srv_answer();
	test_sort();
	test_servers();
	test_hostile();
	test_endpoint();
	test_family_order();
	test_glue_family();
	test_replay();
	test_ipv6_switch();
	test_literal();
	test_service_label();
	test_addr_srv();

	return tu_summary("test_sip_resolve");
}
