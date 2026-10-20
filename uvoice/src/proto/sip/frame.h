// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * SIP message framing on a byte stream (src/proto/sip/frame.c).
 *
 * What it is for. UDP delivers one message per datagram, so transport.c hands
 * msg_parse() a whole message and never has to look for its end. TCP and TLS
 * deliver a byte stream: one read can carry half a message, two messages, or a
 * message plus the first bytes of the next one. This layer turns that stream
 * back into the message slices msg_parse() takes, and it reports the two
 * RFC 5626 keepalive payloads that also travel on the connection.
 *
 * Dependencies. libc only. No uloop, no socket, no allocation, exactly like
 * shared/msg.h. The same code runs in uvoiced, in test_sip_frame and in a fuzz
 * harness.
 *
 * Caller owned buffer. frame_init() takes the buffer and its capacity; this
 * layer never allocates and never owns memory. The per connection 64 KB slab
 * lives in struct sip_conn inside the stream pool (sip/stream.h), so the
 * framer costs one pointer and four small fields per connection.
 *
 * Content-Length is mandatory and trusted. On a stream the header is the only
 * thing that says where a message ends (RFC 3261 18.3), and RFC 3261 20.14
 * makes it mandatory for a stream transport. The compact form "l" is accepted,
 * because the same section defines it. A message with no Content-Length is
 * FRAME_ERR_NO_CLEN and one whose value is not a number is FRAME_ERR_BAD_CLEN.
 * The UDP rule of shared/msg.c, trust the shorter of the declared length and
 * the rest of the datagram, does not apply here and stays in shared/msg.c: a
 * stream has no "rest of the datagram" to compare against, and a short read is
 * not a short message.
 *
 * Every error is terminal. A byte stream carries no message boundary of its
 * own, so a framer that lost the boundary cannot find it again: the bytes
 * after a bad length are of unknown meaning. Any FRAME_ERR_* therefore closes
 * the connection. The framer latches the error and reports the same value on
 * every later call, so a caller that misses the first one still cannot make
 * progress on a stream it no longer understands.
 *
 * The deferred CRLF rule. RFC 5626 5.4 puts two payloads on the connection: a
 * double CRLF is a ping, which the receiver answers, and a single CRLF is a
 * pong, which the receiver counts and never answers. A read can end between
 * the two halves of a ping, and the two halves look exactly like two pongs.
 * The framer therefore holds a trailing lone CRLF as FRAME_MORE and emits
 * FRAME_PONG only when it knows the CRLF is not the first half of a ping:
 *
 *   - the CRLF is followed by a byte that is not CR, so no second CRLF can
 *     follow it, or
 *   - the connection closes, which frame_close() reports.
 *
 * A pong that is counted late is harmless, because a pong needs no answer. A
 * split ping read as two pongs would never be answered, and the peer would
 * drop the connection. test_sip_frame checks the rule at every split point
 * of CRLFCRLF.
 *
 * Slice lifetime. The slice FRAME_MSG reports points into the caller's buffer
 * and stays valid until the next frame_feed(), frame_fed(), frame_tail() or
 * frame_next() call on the same framer, because each of those first compacts
 * the buffer, dropping the message it emitted to make room for the bytes that
 * follow. The caller parses the slice, and finishes with it before it asks for
 * the next event or the next write buffer. This is the shared/msg.h buffer
 * contract one layer down: a message that has to survive is copied by the layer
 * that keeps it (sip/txn.h "Buffer contract").
 *
 * Size. A message that does not fit the buffer is FRAME_ERR_OVERSIZE, which
 * closes the connection. With the 64 KB slab that bound sits one byte above
 * SIP_MSG_LEN_MAX, so every message UDP accepts also frames here.
 *
 * A refused parse is not a framing error. The framer honoured the declared
 * length, so the boundary of the next message is still known: a slice that
 * msg_parse() then refuses is counted and dropped, and the connection stays
 * up. Only a FRAME_ERR_* value means the boundary itself is lost.
 */
#ifndef UVOICE_SIP_FRAME_H
#define UVOICE_SIP_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto/shared/msg.h"

/*
 * The slab the stream pool gives one connection, and with it the largest
 * message this layer accepts. It matches the UDP receive buffer of
 * sip/transport.h, so neither transport accepts a message the other refuses.
 */
#define SIP_FRAME_BUF_MAX	65536

/* the RFC 5626 5.4 payloads, the same two bytes sip/keepalive.h names */
#define SIP_FRAME_PING		"\r\n\r\n"
#define SIP_FRAME_PING_LEN	4
#define SIP_FRAME_PONG		"\r\n"
#define SIP_FRAME_PONG_LEN	2

/*
 * FRAME_MORE is 0, so a zeroed framer reports "nothing yet" and never an
 * event a caller would act on.
 *
 * FRAME_MSG    out holds one complete message, start line through body.
 * FRAME_PING   a double CRLF. The connection answers it with one pong; there
 *              is no ping callback on a stream (sip/stream.h).
 * FRAME_PONG   a single CRLF, counted and never answered.
 * FRAME_MORE   the buffer holds no complete event yet.
 * FRAME_ERR_*  terminal: close the connection.
 */
enum frame_ev {
	FRAME_MORE = 0,
	FRAME_MSG,
	FRAME_PING,
	FRAME_PONG,
	FRAME_ERR_OVERSIZE,	/* the message does not fit the buffer */
	FRAME_ERR_NO_CLEN,	/* no Content-Length on a stream */
	FRAME_ERR_BAD_CLEN,	/* Content-Length is not a number */
};

const char *frame_ev_name(enum frame_ev ev);

/*
 * True for the three FRAME_ERR_* values, so a caller states the terminal rule
 * once instead of listing the errors at every call site.
 */
static inline bool frame_ev_fatal(enum frame_ev ev)
{
	return ev >= FRAME_ERR_OVERSIZE;
}

/*
 * buf, cap  the caller's buffer. frame.c writes nothing outside it; inside it
 *           it unfolds a folded header in place, exactly as shared/msg.c does.
 * len       bytes held.
 * scan      the length of the event emitted last that still sits at the front
 *           of buf, waiting to be dropped; 0 when no event is pending a drop.
 *           The drop happens at the top of the next entry point, which is why
 *           the FRAME_MSG slice stays valid until then ("Slice lifetime"
 *           above).
 * hdr_scan  how far buf was already searched for the header terminator, so a
 *           message that arrives in many reads is scanned once and not once per
 *           read. Reset to 0 when the front event is dropped.
 * err       the latched enum frame_ev, FRAME_MORE while the framer is healthy.
 * crlf_held a trailing lone CRLF whose classification is deferred.
 *
 * Every field belongs to frame.c. A caller reads them through the helpers.
 */
struct sip_framer {
	char *buf;
	size_t cap;
	size_t len;
	size_t scan;
	size_t hdr_scan;
	uint8_t err;
	bool crlf_held;
};

/*
 * Bind a framer to a buffer. cap must be at least SIP_FRAME_PING_LEN; a
 * smaller buffer cannot hold the shortest event. The framer keeps the pointer,
 * so the buffer must outlive it.
 */
void frame_init(struct sip_framer *f, char *buf, size_t cap);

/* Drop every held byte and clear the latched error, for a reused connection. */
void frame_reset(struct sip_framer *f);

/* bytes held, and the room that is left for the next read */
size_t frame_pending(const struct sip_framer *f);
size_t frame_space(const struct sip_framer *f);

/*
 * Append bytes. False means they do not fit, which the framer latches as
 * FRAME_ERR_OVERSIZE: a message larger than the buffer can never complete, and
 * a stream offers no way to skip it. Nothing is appended in that case.
 */
bool frame_feed(struct sip_framer *f, const void *data, size_t len);

/*
 * The zero copy form of frame_feed(), for a caller that reads straight into
 * the buffer: frame_tail() reports where the next byte goes and how much room
 * there is, and frame_fed() commits what the read produced. Like frame_feed(),
 * frame_tail() first drops a message it already emitted, so a caller finishes
 * with a FRAME_MSG slice before it asks for the next write buffer ("Slice
 * lifetime" above). NULL, with *space set to 0, means the buffer is full, which
 * is the same oversize case frame_feed() latches. It exists so the receive path
 * does not copy the whole 64 KB slab a second time through a scratch buffer.
 */
char *frame_tail(struct sip_framer *f, size_t *space);
void frame_fed(struct sip_framer *f, size_t len);

/*
 * The next event. out receives the message slice for FRAME_MSG and is zeroed
 * for every other value, out may be NULL for a caller that only drains. A
 * caller loops until it gets FRAME_MORE or a fatal value, and revalidates its
 * connection handle before every call, because dispatching one message can
 * close the connection (sip/stream.h "Rx reentrancy").
 */
enum frame_ev frame_next(struct sip_framer *f, struct sip_str *out);

/*
 * The connection is closing: report a deferred pong. Returns FRAME_PONG when
 * a lone CRLF was held, FRAME_MORE otherwise. Bytes of an incomplete message
 * are dropped without an error, because a peer that closes mid message has
 * already said what it wanted.
 */
enum frame_ev frame_close(struct sip_framer *f);

#endif
