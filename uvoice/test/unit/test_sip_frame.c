// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the stream framer (src/proto/sip/frame.c).
 *
 * The cases feed scripted byte sequences and use no clock and no socket,
 * because the framer is pure libc over a caller buffer.
 *
 * Two checks matter most. Every split point of a CRLFCRLF ping yields one
 * FRAME_PING and no FRAME_PONG. Every FRAME_ERR_* is terminal.
 */
#include "proto/sip/frame.h"
#include "test_util.h"

/* a message with an empty body, and one with a five byte body */
#define M_EMPTY	"OPTIONS sip:a SIP/2.0\r\nCall-ID: 7\r\nContent-Length: 0\r\n\r\n"
#define M_BODY	"MESSAGE sip:a SIP/2.0\r\nContent-Length: 5\r\n\r\nhello"

#define BUFSZ	8192
static char backing[BUFSZ];
static struct sip_framer F;
static char scratch[BUFSZ];

static size_t joinv(const char *const *parts, size_t n)
{
	size_t at = 0, i;

	for (i = 0; i < n; i++) {
		size_t l = strlen(parts[i]);

		memcpy(scratch + at, parts[i], l);
		at += l;
	}

	return at;
}

/* header text plus a body of bodylen bytes, built into scratch */
static size_t build_body(const char *hdr, size_t bodylen, char fill)
{
	size_t hl = strlen(hdr);

	memcpy(scratch, hdr, hl);
	memset(scratch + hl, fill, bodylen);

	return hl + bodylen;
}

/* feed scratch[0..n) one byte at a time, return the last frame_next result */
static enum frame_ev drip(size_t n, struct sip_str *out)
{
	enum frame_ev ev = FRAME_MORE;
	size_t i;

	for (i = 0; i < n; i++) {
		CHECK(frame_feed(&F, scratch + i, 1));
		ev = frame_next(&F, out);
	}

	return ev;
}

static void feed(const char *s)
{
	CHECK(frame_feed(&F, s, strlen(s)));
}

static void contract_tests(void)
{
	CASE("frame: the frozen event and payload constants");
	CHECK_UINT(FRAME_MORE, 0);
	CHECK_UINT(SIP_FRAME_PING_LEN, strlen(SIP_FRAME_PING));
	CHECK_UINT(SIP_FRAME_PONG_LEN, strlen(SIP_FRAME_PONG));
	CHECK_UINT(SIP_FRAME_BUF_MAX, 65536);
	CHECK(frame_ev_fatal(FRAME_ERR_OVERSIZE));
	CHECK(frame_ev_fatal(FRAME_ERR_NO_CLEN));
	CHECK(frame_ev_fatal(FRAME_ERR_BAD_CLEN));
	CHECK(!frame_ev_fatal(FRAME_MSG));
	CHECK(!frame_ev_fatal(FRAME_PING));
	CHECK(!frame_ev_fatal(FRAME_PONG));
	CHECK(!frame_ev_fatal(FRAME_MORE));

	CASE("frame: every event has a distinct name");
	CHECK_STR(sip_str_cstr(frame_ev_name(FRAME_MSG)), "msg");
	CHECK_STR(sip_str_cstr(frame_ev_name(FRAME_PING)), "ping");
	CHECK_STR(sip_str_cstr(frame_ev_name(FRAME_PONG)), "pong");
	CHECK_STR(sip_str_cstr(frame_ev_name(FRAME_ERR_NO_CLEN)), "no-clen");
}

static void whole_message_tests(void)
{
	struct sip_str out;

	CASE("frame: a whole message with an empty body");
	frame_init(&F, backing, BUFSZ);
	feed(M_EMPTY);
	CHECK_UINT(frame_next(&F, &out), FRAME_MSG);
	CHECK_UINT(out.len, strlen(M_EMPTY));
	CHECK(out.p == backing);
	CHECK(memcmp(out.p, M_EMPTY, strlen(M_EMPTY)) == 0);
	CHECK_UINT(frame_next(&F, &out), FRAME_MORE);
	CHECK(out.p == NULL);

	CASE("frame: a whole message with a body");
	frame_init(&F, backing, BUFSZ);
	feed(M_BODY);
	CHECK_UINT(frame_next(&F, &out), FRAME_MSG);
	CHECK_UINT(out.len, strlen(M_BODY));
	CHECK(memcmp(out.p, M_BODY, strlen(M_BODY)) == 0);
	CHECK_UINT(frame_next(&F, &out), FRAME_MORE);

	CASE("frame: byte drip is MORE until the last body byte");
	frame_init(&F, backing, BUFSZ);
	{
		size_t n = strlen(M_BODY), i;

		for (i = 0; i < n; i++) {
			enum frame_ev ev;

			CHECK(frame_feed(&F, M_BODY + i, 1));
			ev = frame_next(&F, &out);
			if (i + 1 < n) {
				CHECK_UINT(ev, FRAME_MORE);
			} else {
				CHECK_UINT(ev, FRAME_MSG);
				CHECK_UINT(out.len, n);
			}
		}
	}

	CASE("frame: two pipelined messages then MORE");
	frame_init(&F, backing, BUFSZ);
	{
		const char *parts[] = { M_EMPTY, M_BODY };
		size_t total = joinv(parts, 2);

		CHECK(frame_feed(&F, scratch, total));
		CHECK_UINT(frame_next(&F, &out), FRAME_MSG);
		CHECK_UINT(out.len, strlen(M_EMPTY));
		CHECK(memcmp(out.p, M_EMPTY, strlen(M_EMPTY)) == 0);
		CHECK_UINT(frame_next(&F, &out), FRAME_MSG);
		CHECK_UINT(out.len, strlen(M_BODY));
		CHECK(memcmp(out.p, M_BODY, strlen(M_BODY)) == 0);
		CHECK_UINT(frame_next(&F, &out), FRAME_MORE);
	}
}

static void keepalive_tests(void)
{
	struct sip_str out;

	CASE("frame: a lone ping");
	frame_init(&F, backing, BUFSZ);
	feed(SIP_FRAME_PING);
	CHECK_UINT(frame_next(&F, NULL), FRAME_PING);
	CHECK_UINT(frame_next(&F, NULL), FRAME_MORE);

	CASE("frame: a ping between two messages");
	frame_init(&F, backing, BUFSZ);
	{
		const char *parts[] = { M_EMPTY, SIP_FRAME_PING, M_BODY };
		size_t total = joinv(parts, 3);

		CHECK(frame_feed(&F, scratch, total));
		CHECK_UINT(frame_next(&F, &out), FRAME_MSG);
		CHECK_UINT(out.len, strlen(M_EMPTY));
		CHECK_UINT(frame_next(&F, NULL), FRAME_PING);
		CHECK_UINT(frame_next(&F, &out), FRAME_MSG);
		CHECK_UINT(out.len, strlen(M_BODY));
		CHECK_UINT(frame_next(&F, NULL), FRAME_MORE);
	}

	CASE("frame: a lone CRLF between two messages is a pong");
	frame_init(&F, backing, BUFSZ);
	{
		const char *parts[] = { M_EMPTY, SIP_FRAME_PONG, M_BODY };
		size_t total = joinv(parts, 3);

		CHECK(frame_feed(&F, scratch, total));
		CHECK_UINT(frame_next(&F, &out), FRAME_MSG);
		CHECK_UINT(out.len, strlen(M_EMPTY));
		CHECK_UINT(frame_next(&F, NULL), FRAME_PONG);
		CHECK_UINT(frame_next(&F, &out), FRAME_MSG);
		CHECK_UINT(out.len, strlen(M_BODY));
		CHECK_UINT(frame_next(&F, NULL), FRAME_MORE);
	}

	CASE("frame: a leading pong then a message");
	frame_init(&F, backing, BUFSZ);
	{
		const char *parts[] = { SIP_FRAME_PONG, M_BODY };
		size_t total = joinv(parts, 2);

		CHECK(frame_feed(&F, scratch, total));
		CHECK_UINT(frame_next(&F, NULL), FRAME_PONG);
		CHECK_UINT(frame_next(&F, &out), FRAME_MSG);
		CHECK_UINT(out.len, strlen(M_BODY));
	}

	CASE("frame: a held CRLF becomes a pong when a non-CR byte follows");
	frame_init(&F, backing, BUFSZ);
	feed(SIP_FRAME_PONG);
	CHECK_UINT(frame_next(&F, NULL), FRAME_MORE);
	feed("X");
	CHECK_UINT(frame_next(&F, NULL), FRAME_PONG);
	CHECK_UINT(frame_next(&F, NULL), FRAME_MORE);

	CASE("frame: a trailing lone CRLF is a pong at close");
	frame_init(&F, backing, BUFSZ);
	feed(SIP_FRAME_PONG);
	CHECK_UINT(frame_next(&F, NULL), FRAME_MORE);
	CHECK_UINT(frame_close(&F), FRAME_PONG);

	CASE("frame: close with no held CRLF is MORE");
	frame_init(&F, backing, BUFSZ);
	CHECK_UINT(frame_close(&F), FRAME_MORE);

	CASE("frame: close drops an incomplete message");
	frame_init(&F, backing, BUFSZ);
	feed("MESSAGE sip:a SIP/2.0\r\nContent-Length: 5\r\n\r\nhe");
	CHECK_UINT(frame_next(&F, NULL), FRAME_MORE);
	CHECK_UINT(frame_close(&F), FRAME_MORE);
}

/*
 * The deferred-CRLF check: feed the first k bytes of a CRLFCRLF ping, drain,
 * feed the rest, drain, then close. For every split k the total is exactly one
 * ping and never a pong.
 */
static void ping_split_tests(void)
{
	static const char ping[SIP_FRAME_PING_LEN] = { '\r', '\n', '\r', '\n' };
	size_t k;

	CASE("frame: every CRLFCRLF split yields one ping and no pong");
	for (k = 1; k < SIP_FRAME_PING_LEN; k++) {
		int pings = 0, pongs = 0;
		enum frame_ev ev;

		frame_init(&F, backing, BUFSZ);
		CHECK(frame_feed(&F, ping, k));
		while ((ev = frame_next(&F, NULL)) != FRAME_MORE) {
			if (ev == FRAME_PING)
				pings++;
			else if (ev == FRAME_PONG)
				pongs++;
			else
				break;
		}
		CHECK(frame_feed(&F, ping + k, SIP_FRAME_PING_LEN - k));
		while ((ev = frame_next(&F, NULL)) != FRAME_MORE) {
			if (ev == FRAME_PING)
				pings++;
			else if (ev == FRAME_PONG)
				pongs++;
			else
				break;
		}
		if (frame_close(&F) == FRAME_PONG)
			pongs++;

		CHECK_UINT(pings, 1);
		CHECK_UINT(pongs, 0);
	}
}

static void error_tests(void)
{
	struct sip_str out;

	CASE("frame: a missing Content-Length is a terminal error");
	frame_init(&F, backing, BUFSZ);
	feed("FOO sip:a SIP/2.0\r\nX: y\r\n\r\n");
	CHECK_UINT(frame_next(&F, NULL), FRAME_ERR_NO_CLEN);
	CHECK(frame_ev_fatal(frame_next(&F, NULL)));
	CHECK_UINT(frame_next(&F, NULL), FRAME_ERR_NO_CLEN);

	CASE("frame: an unparseable Content-Length is a terminal error");
	frame_init(&F, backing, BUFSZ);
	feed("FOO sip:a SIP/2.0\r\nContent-Length: abc\r\n\r\n");
	CHECK_UINT(frame_next(&F, NULL), FRAME_ERR_BAD_CLEN);
	CHECK_UINT(frame_next(&F, NULL), FRAME_ERR_BAD_CLEN);

	CASE("frame: an empty Content-Length is a bad-clen error");
	frame_init(&F, backing, BUFSZ);
	feed("FOO sip:a SIP/2.0\r\nContent-Length: \r\n\r\n");
	CHECK_UINT(frame_next(&F, NULL), FRAME_ERR_BAD_CLEN);

	CASE("frame: an error latches over a later feed");
	frame_init(&F, backing, BUFSZ);
	feed("FOO sip:a SIP/2.0\r\nX: y\r\n\r\n");
	CHECK_UINT(frame_next(&F, NULL), FRAME_ERR_NO_CLEN);
	CHECK(!frame_feed(&F, M_EMPTY, strlen(M_EMPTY)));
	CHECK_UINT(frame_next(&F, NULL), FRAME_ERR_NO_CLEN);

	CASE("frame: a framed length exactly at the cap fits");
	frame_init(&F, backing, strlen(M_BODY));
	feed(M_BODY);
	CHECK_UINT(frame_next(&F, &out), FRAME_MSG);
	CHECK_UINT(out.len, strlen(M_BODY));

	CASE("frame: a framed length one over the cap is oversize");
	frame_init(&F, backing, strlen(M_BODY) - 1);
	{
		size_t hdr = strlen(M_BODY) - 5;	/* "hello" is 5 bytes */

		CHECK(frame_feed(&F, M_BODY, hdr));
		CHECK_UINT(frame_next(&F, NULL), FRAME_ERR_OVERSIZE);
	}

	CASE("frame: a feed that cannot fit is oversize");
	frame_init(&F, backing, 8);
	CHECK(!frame_feed(&F, "123456789", 9));
	CHECK_UINT(frame_next(&F, NULL), FRAME_ERR_OVERSIZE);

	CASE("frame: a header that fills the buffer with no terminator is oversize");
	{
		const char *noterm = "INVITE sip:x SIP/2.0\r\nX: yy";

		frame_init(&F, backing, strlen(noterm));
		CHECK(frame_feed(&F, noterm, strlen(noterm)));
		CHECK_UINT(frame_next(&F, NULL), FRAME_ERR_OVERSIZE);
	}
}

static void tolerant_clen_tests(void)
{
	struct sip_str out;

	CASE("frame: the compact l form and LWS around the colon");
	frame_init(&F, backing, BUFSZ);
	{
		const char *m = "FOO sip:a SIP/2.0\r\nl : 5\r\n\r\nhello";

		feed(m);
		CHECK_UINT(frame_next(&F, &out), FRAME_MSG);
		CHECK_UINT(out.len, strlen(m));
	}

	CASE("frame: the Content-Length name is case insensitive");
	frame_init(&F, backing, BUFSZ);
	{
		const char *m = "FOO sip:a SIP/2.0\r\ncoNTeNT-lenGTH: 5\r\n\r\nhello";

		feed(m);
		CHECK_UINT(frame_next(&F, &out), FRAME_MSG);
		CHECK_UINT(out.len, strlen(m));
	}

	CASE("frame: the first Content-Length wins");
	frame_init(&F, backing, BUFSZ);
	{
		const char *m = "FOO sip:a SIP/2.0\r\nContent-Length: 3\r\n"
				"Content-Length: 99\r\n\r\nabc";

		feed(m);
		CHECK_UINT(frame_next(&F, &out), FRAME_MSG);
		CHECK_UINT(out.len, strlen(m));
	}
}

static void buffer_tests(void)
{
	struct sip_str out;

	CASE("frame: pending and space track the held bytes");
	frame_init(&F, backing, 100);
	CHECK_UINT(frame_pending(&F), 0);
	CHECK_UINT(frame_space(&F), 100);
	feed("abc");
	CHECK_UINT(frame_pending(&F), 3);
	CHECK_UINT(frame_space(&F), 97);

	CASE("frame: pending excludes an emitted-but-not-dropped message");
	frame_init(&F, backing, BUFSZ);
	{
		const char *parts[] = { M_EMPTY, M_BODY };
		size_t total = joinv(parts, 2);

		CHECK(frame_feed(&F, scratch, total));
		CHECK_UINT(frame_next(&F, &out), FRAME_MSG);
		CHECK_UINT(frame_pending(&F), strlen(M_BODY));
	}

	CASE("frame: the zero-copy tail path frames a message");
	frame_init(&F, backing, BUFSZ);
	{
		size_t space = 0;
		char *tail = frame_tail(&F, &space);

		CHECK(tail != NULL);
		CHECK(space >= strlen(M_BODY));
		memcpy(tail, M_BODY, strlen(M_BODY));
		frame_fed(&F, strlen(M_BODY));
		CHECK_UINT(frame_next(&F, &out), FRAME_MSG);
		CHECK_UINT(out.len, strlen(M_BODY));
	}

	CASE("frame: reset returns the framer to a usable state");
	frame_init(&F, backing, BUFSZ);
	feed("FOO sip:a SIP/2.0\r\nX: y\r\n\r\n");
	CHECK_UINT(frame_next(&F, NULL), FRAME_ERR_NO_CLEN);
	frame_reset(&F);
	CHECK_UINT(frame_next(&F, NULL), FRAME_MORE);
	feed(M_EMPTY);
	CHECK_UINT(frame_next(&F, &out), FRAME_MSG);
	CHECK_UINT(out.len, strlen(M_EMPTY));
}

/*
 * Content-Length unfolding: frame.c must read the length as shared/msg.c does.
 * A folded value is therefore read, and a folded line that resembles a header
 * is not mistaken for one. If frame.c reads either wrong, the boundary
 * desyncs from msg_parse.
 */
static void folded_clen_tests(void)
{
	struct sip_str out;

	CASE("frame: a folded Content-Length value is read");
	frame_init(&F, backing, BUFSZ);
	{
		const char *m = "FOO sip:a SIP/2.0\r\nContent-Length:\r\n 5\r\n\r\nhello";

		feed(m);
		CHECK_UINT(frame_next(&F, &out), FRAME_MSG);
		CHECK_UINT(out.len, strlen(m));
	}

	CASE("frame: a folded line resembling a CL is not the CL");
	frame_init(&F, backing, BUFSZ);
	{
		const char *hdr = "FOO sip:a SIP/2.0\r\nX: a\r\n Content-Length: 3\r\n"
				  "Content-Length: 99\r\n\r\n";
		size_t hl = strlen(hdr);
		size_t total = build_body(hdr, 99, 'z');

		CHECK(frame_feed(&F, scratch, total));
		CHECK_UINT(frame_next(&F, &out), FRAME_MSG);
		CHECK_UINT(out.len, hl + 99);
	}
}

/* the hdr_scan cursor: a large header delivered one byte at a time frames */
static void large_header_tests(void)
{
	struct sip_str out;
	static char big[2200];
	const char *head = "FOO sip:a SIP/2.0\r\nX: ";
	const char *tail = "\r\nContent-Length: 3\r\n\r\nabc";
	size_t at = 0, i;

	CASE("frame: a large header byte-dripped frames correctly");
	memcpy(big, head, strlen(head));
	at = strlen(head);
	for (i = 0; i < 2000; i++)
		big[at++] = 'a';
	memcpy(big + at, tail, strlen(tail));
	at += strlen(tail);

	frame_init(&F, backing, BUFSZ);
	memcpy(scratch, big, at);
	CHECK_UINT(drip(at, &out), FRAME_MSG);
	CHECK_UINT(out.len, at);
}

/*
 * After feed("\r\n"); next[holds]; feed("\r\n"), the front is a complete
 * ping, not a lone CRLF. frame_close must not report a stale pong, and a second
 * close must be inert.
 */
static void close_robustness_tests(void)
{
	CASE("frame: close does not report a completed ping as a pong");
	frame_init(&F, backing, BUFSZ);
	feed(SIP_FRAME_PONG);
	CHECK_UINT(frame_next(&F, NULL), FRAME_MORE);
	feed(SIP_FRAME_PONG);
	CHECK_UINT(frame_close(&F), FRAME_MORE);
	CHECK_UINT(frame_close(&F), FRAME_MORE);

	CASE("frame: a second close after a pong is MORE");
	frame_init(&F, backing, BUFSZ);
	feed(SIP_FRAME_PONG);
	CHECK_UINT(frame_next(&F, NULL), FRAME_MORE);
	CHECK_UINT(frame_close(&F), FRAME_PONG);
	CHECK_UINT(frame_close(&F), FRAME_MORE);
}

static void boundary_tests(void)
{
	struct sip_str out;

	CASE("frame: the body is bounded by Content-Length, not a second terminator");
	frame_init(&F, backing, BUFSZ);
	{
		const char *m = "FOO sip:a SIP/2.0\r\nContent-Length: 7\r\n\r\n\r\n\r\nend";
		const char *parts[] = { m, M_EMPTY };
		size_t total = joinv(parts, 2);

		CHECK(frame_feed(&F, scratch, total));
		CHECK_UINT(frame_next(&F, &out), FRAME_MSG);
		CHECK_UINT(out.len, strlen(m));
		CHECK(memcmp(out.p, m, strlen(m)) == 0);
		CHECK_UINT(frame_next(&F, &out), FRAME_MSG);
		CHECK_UINT(out.len, strlen(M_EMPTY));
	}

	/*
	 * Known v1 limitation: framing is strict CRLFCRLF (RFC 5626), so a
	 * bare-LF header block never forms a boundary and fills to oversize.
	 * shared/msg.c's bare-LF tolerance applies only inside a framed slice.
	 */
	CASE("frame: a bare-LF header block over a stream is oversize");
	{
		const char *bare = "FOO sip:a SIP/2.0\nContent-Length: 0\n\n";

		frame_init(&F, backing, strlen(bare));
		CHECK(frame_feed(&F, bare, strlen(bare)));
		CHECK_UINT(frame_next(&F, NULL), FRAME_ERR_OVERSIZE);
	}
}

int main(void)
{
	contract_tests();
	whole_message_tests();
	keepalive_tests();
	ping_split_tests();
	error_tests();
	tolerant_clen_tests();
	folded_clen_tests();
	large_header_tests();
	close_robustness_tests();
	boundary_tests();
	buffer_tests();

	return tu_summary("test_sip_frame");
}
