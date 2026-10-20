// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * SIP message framing on a byte stream. See sip/frame.h for the contracts.
 *
 * Buffer model. The framer holds bytes in buf[0..len). The front of the buffer
 * is always the start of the current event. An event is emitted without moving
 * any bytes, so the slice FRAME_MSG reports stays valid; the emitted event is
 * dropped from the front at the top of the next entry point, which is why the
 * slice lifetime ends there (sip/frame.h "Slice lifetime").
 *
 * scan and hdr_scan. scan is the length of the event emitted last and still
 * sitting at the front, waiting to be dropped; it is 0 when no event is
 * pending. hdr_scan is the header-terminator search cursor: the CRLFCRLF search
 * resumes from it and advances as bytes accumulate, so a message that arrives
 * in many reads is scanned once, not once per read. drop_front() resets
 * hdr_scan because the front, and with it the search, starts over.
 *
 * Content-Length. The header block end is the first "\r\n\r\n". The length is
 * read the way shared/msg.c reads it: a header line that begins with a space or
 * a tab is a continuation, so it is unfolded in place first (shared/msg.c
 * hdr_line_take); the header id parser accepts the compact "l" and any case;
 * the name and value are trimmed so the LWS around the colon does not matter;
 * and the value is a plain unsigned integer (shared/msg.c hdr_add, body_set).
 * Frame.c and shared/msg.c then agree on the Content-Length and the message
 * boundary for every input.
 */
#include <string.h>

#include "proto/sip/frame.h"

enum clen_result {
	CLEN_NONE,
	CLEN_BAD,
	CLEN_OK,
};

static enum frame_ev latch(struct sip_framer *f, enum frame_ev ev)
{
	f->err = (uint8_t)ev;

	return ev;
}

static void drop_front(struct sip_framer *f)
{
	if (!f->scan)
		return;

	memmove(f->buf, f->buf + f->scan, f->len - f->scan);
	f->len -= f->scan;
	f->scan = 0;
	f->hdr_scan = 0;
}

static const char *hdr_term_find(const char *buf, size_t len)
{
	const char *p = buf, *end = buf + len, *cr;

	while ((size_t)(end - p) >= 4) {
		cr = memchr(p, '\r', (size_t)(end - p) - 3);
		if (!cr)
			return NULL;
		if (cr[1] == '\n' && cr[2] == '\r' && cr[3] == '\n')
			return cr;
		p = cr + 1;
	}

	return NULL;
}

/*
 * Content-Length over the header block buf[0..hdr_end), read as shared/msg.c
 * reads it. A line that begins with a space or a tab continues the header
 * before it, so the fold CR and LF become spaces in place (shared/msg.c
 * hdr_line_take) and the value stays one contiguous run. That keeps a folded
 * value ("Content-Length:\r\n 5") readable and stops a folded line that
 * resembles a header (" Content-Length: 3") from being mistaken for one.
 */
static enum clen_result clen_read(char *buf, size_t hdr_end, uint32_t *out)
{
	char *p = buf, *end = buf + hdr_end, *ls, *lf, *ce, *next, *colon, *f;
	struct sip_str name, val;

	/* the start line carries no header name; skip it */
	lf = memchr(p, '\n', (size_t)(end - p));
	if (!lf)
		return CLEN_NONE;
	p = lf + 1;

	while (p < end) {
		ls = p;
		lf = memchr(ls, '\n', (size_t)(end - ls));
		ce = lf ? lf : end;
		if (ce > ls && ce[-1] == '\r')
			ce--;
		if (ce == ls)
			break;
		next = lf ? lf + 1 : end;

		while (next < end && (*next == ' ' || *next == '\t')) {
			for (f = ce; f < next; f++)
				*f = ' ';
			lf = memchr(next, '\n', (size_t)(end - next));
			ce = lf ? lf : end;
			if (ce > next && ce[-1] == '\r')
				ce--;
			next = lf ? lf + 1 : end;
		}

		colon = memchr(ls, ':', (size_t)(ce - ls));
		if (colon) {
			name = sip_str_trim(sip_str_init(ls, (size_t)(colon - ls)));
			if (sip_hdr_id_parse(name) == SIP_HDR_CONTENT_LENGTH) {
				val = sip_str_trim(sip_str_init(colon + 1,
						(size_t)(ce - colon - 1)));
				return sip_str_uint(val, out) ? CLEN_OK : CLEN_BAD;
			}
		}

		p = next;
	}

	return CLEN_NONE;
}

/*
 * The front begins with a CRLF. A double CRLF is a ping. A single CRLF is a
 * pong, but only once a following byte proves it is not the first half of a
 * ping: a prefix of "\r\n\r\n" that is not yet the whole thing is held as
 * FRAME_MORE (sip/frame.h "The deferred CRLF rule").
 */
static enum frame_ev frame_crlf(struct sip_framer *f)
{
	if (f->len >= 4 && f->buf[2] == '\r' && f->buf[3] == '\n') {
		f->scan = SIP_FRAME_PING_LEN;
		return FRAME_PING;
	}

	if (f->len == 2 || (f->len == 3 && f->buf[2] == '\r')) {
		f->crlf_held = true;
		return FRAME_MORE;
	}

	f->scan = SIP_FRAME_PONG_LEN;
	return FRAME_PONG;
}

static enum frame_ev frame_msg(struct sip_framer *f, struct sip_str *out)
{
	const char *term = hdr_term_find(f->buf + f->hdr_scan, f->len - f->hdr_scan);
	size_t hdr_end, msg_len;
	uint32_t clen;

	if (!term) {
		if (f->len >= f->cap)
			return latch(f, FRAME_ERR_OVERSIZE);
		f->hdr_scan = f->len >= 3 ? f->len - 3 : 0;
		return FRAME_MORE;
	}

	hdr_end = (size_t)(term - f->buf) + SIP_FRAME_PING_LEN;

	switch (clen_read(f->buf, hdr_end, &clen)) {
	case CLEN_NONE:
		return latch(f, FRAME_ERR_NO_CLEN);
	case CLEN_BAD:
		return latch(f, FRAME_ERR_BAD_CLEN);
	case CLEN_OK:
		break;
	}

	if (clen > f->cap - hdr_end)
		return latch(f, FRAME_ERR_OVERSIZE);

	msg_len = hdr_end + clen;
	if (f->len < msg_len) {
		f->hdr_scan = (size_t)(term - f->buf);
		return FRAME_MORE;
	}

	if (out)
		*out = sip_str_init(f->buf, msg_len);
	f->scan = msg_len;

	return FRAME_MSG;
}

const char *frame_ev_name(enum frame_ev ev)
{
	switch (ev) {
	case FRAME_MORE:
		return "more";
	case FRAME_MSG:
		return "msg";
	case FRAME_PING:
		return "ping";
	case FRAME_PONG:
		return "pong";
	case FRAME_ERR_OVERSIZE:
		return "oversize";
	case FRAME_ERR_NO_CLEN:
		return "no-clen";
	case FRAME_ERR_BAD_CLEN:
		return "bad-clen";
	}

	return "unknown";
}

void frame_init(struct sip_framer *f, char *buf, size_t cap)
{
	f->buf = buf;
	f->cap = cap;
	f->len = 0;
	f->scan = 0;
	f->hdr_scan = 0;
	f->err = FRAME_MORE;
	f->crlf_held = false;
}

void frame_reset(struct sip_framer *f)
{
	f->len = 0;
	f->scan = 0;
	f->hdr_scan = 0;
	f->err = FRAME_MORE;
	f->crlf_held = false;
}

size_t frame_pending(const struct sip_framer *f)
{
	return f->len - f->scan;
}

size_t frame_space(const struct sip_framer *f)
{
	return f->cap - (f->len - f->scan);
}

bool frame_feed(struct sip_framer *f, const void *data, size_t len)
{
	if (f->err)
		return false;

	drop_front(f);

	if (len > f->cap - f->len) {
		latch(f, FRAME_ERR_OVERSIZE);
		return false;
	}

	if (len)
		memcpy(f->buf + f->len, data, len);
	f->len += len;

	return true;
}

char *frame_tail(struct sip_framer *f, size_t *space)
{
	if (f->err) {
		if (space)
			*space = 0;
		return NULL;
	}

	drop_front(f);

	if (f->len >= f->cap) {
		latch(f, FRAME_ERR_OVERSIZE);
		if (space)
			*space = 0;
		return NULL;
	}

	if (space)
		*space = f->cap - f->len;

	return f->buf + f->len;
}

void frame_fed(struct sip_framer *f, size_t len)
{
	if (f->err)
		return;

	if (len > f->cap - f->len)
		len = f->cap - f->len;
	f->len += len;
}

enum frame_ev frame_next(struct sip_framer *f, struct sip_str *out)
{
	if (out)
		memset(out, 0, sizeof(*out));

	if (f->err)
		return (enum frame_ev)f->err;

	drop_front(f);
	f->crlf_held = false;

	if (f->len == 0)
		return FRAME_MORE;

	if (f->buf[0] == '\r') {
		if (f->len < 2)
			return FRAME_MORE;
		if (f->buf[1] == '\n')
			return frame_crlf(f);
	}

	return frame_msg(f, out);
}

/*
 * The stream ended. Classify the current front from the buffer itself, never
 * from a stale crlf_held: a genuine lone trailing CRLF is the deferred pong; a
 * completed ping, an incomplete message and an empty buffer are not. The
 * reported front is consumed so a second close is inert.
 */
enum frame_ev frame_close(struct sip_framer *f)
{
	if (f->err)
		return (enum frame_ev)f->err;

	drop_front(f);
	f->crlf_held = false;

	if (f->len >= 2 && f->buf[0] == '\r' && f->buf[1] == '\n') {
		bool ping = f->len >= 4 && f->buf[2] == '\r' && f->buf[3] == '\n';
		size_t take = ping ? SIP_FRAME_PING_LEN : SIP_FRAME_PONG_LEN;

		memmove(f->buf, f->buf + take, f->len - take);
		f->len -= take;

		return ping ? FRAME_MORE : FRAME_PONG;
	}

	f->len = 0;

	return FRAME_MORE;
}
