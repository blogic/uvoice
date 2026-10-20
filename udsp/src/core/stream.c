// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The media plane. The control half of this file implements the quiesce
 * protocol of udsp.h section 3 and the engine half runs the per-tick receive
 * and transmit chains.
 *
 * Reconfiguring a running stream is the common re-INVITE, so it never stops
 * the stream: the net thread drops the descriptors from its epoll set, the
 * engine deactivates the stream at a tick boundary, the control thread swaps
 * what changed, and both come back. SSRC, sequence and timestamp survive and
 * the first packet after the swap carries the marker. A live SRTP rekey
 * arrives the same way and rekeys the existing object: a fresh create would
 * hand libsrtp a new replay window and a captured packet would be accepted
 * twice.
 *
 * Three time bases reach the RTP layer and the engine supplies all of them
 * (rtp.h): the arrival stamp the net thread took from CLOCK_MONOTONIC, the
 * monotonic 16.16 clock behind LSR and DLSR, and wall NTP for the SR fields
 * and the round trip. clock_gettime is lock-free and async-signal-safe, so the
 * tick samples both clocks itself. An NTP step therefore skews the round-trip
 * estimate alone: every jitter and playout decision stays on the monotonic
 * base.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "net.h"
#include "udsp_priv.h"
#include "stream.h"

/* Defaults: jitter_target 50, jitter_buffer_max 100. A zero field is a caller
 * that stated no preference, so it takes the default. */
#define STREAM_JB_INIT_MS	50u
#define STREAM_JB_MAX_MS	100u
#define STREAM_PTIME_DEFAULT	20u
/* RFC 3389 comfort noise is carried on the static payload type. */
#define STREAM_CN_PT		13u

/*
 * enum jb_state of rtp/jb.c, which is private to that file. The trace carries
 * the number as a diagnostic and udsp.h says so, so a local mirror is enough:
 * nothing here decides anything from the value, it only labels a transition.
 */
#define STREAM_JB_IDLE		0u
#define STREAM_JB_PREFILL	1u
#define STREAM_JB_PLAY		2u

/* --- the pool ------------------------------------------------------------ */

struct udsp_stream *stream_at(struct udsp *s, unsigned line, unsigned stream)
{
	if (!s || line >= udsp_lines(s) || stream >= UDSP_STREAM_MAX)
		return NULL;

	return s->stream[line * UDSP_STREAM_MAX + stream];
}

/*
 * Every stream of a line the caller checked. The walk stops at an empty slot:
 * udsp_open allocates every slot of the pool before it publishes the line
 * count, so a checked line has no empty slot.
 */
#define stream_line_for_each(s, line, st)				\
	for (unsigned stream_i_ = 0;					\
	     stream_i_ < UDSP_STREAM_MAX &&				\
	     ((st) = (s)->stream[(line) * UDSP_STREAM_MAX + stream_i_])	\
	     != NULL;							\
	     stream_i_++)

/*
 * One allocation per stream, never one block for the whole pool: a stream is
 * 58.5 KiB, so the pool of a two-line board would be a quarter of a megabyte
 * in a single chunk, and the C library serves a request that large from its
 * own mapping instead of the heap it already holds.
 */
int stream_pool_alloc(struct udsp *s)
{
	unsigned n = (unsigned)s->cap.lines * UDSP_STREAM_MAX;

	for (unsigned i = 0; i < n; i++) {
		struct udsp_stream *st = calloc(1, sizeof(*st));

		if (!st)
			return -ENOMEM;
		s->stream[i] = st;
		st->line = (uint8_t)(i / UDSP_STREAM_MAX);
		st->index = (uint8_t)(i % UDSP_STREAM_MAX);
		st->rtp_fd = -1;
		st->rtcp_fd = -1;
		st->net_rtp_fd = -1;
		st->net_rtcp_fd = -1;
		atomic_init(&st->state, STREAM_IDLE);
		atomic_init(&st->gen, 0u);
		atomic_init(&st->net_mode, STREAM_MODE_AUDIO);
		atomic_init(&st->jb_snap_valid, false);
		atomic_init(&st->dead, false);
		atomic_init(&st->active, false);
		atomic_init(&st->srtp_armed, false);
		atomic_init(&st->ring.head, 0u);
		atomic_init(&st->ring.tail, 0u);
		atomic_init(&st->net_drops, 0u);
		atomic_init(&st->net_stale, 0u);
		atomic_init(&st->net_trunc, 0u);
		atomic_init(&st->tx_eagain, 0u);
		atomic_init(&st->punch_sent, 0u);
	}

	return 0;
}

void stream_pool_free(struct udsp *s)
{
	for (unsigned i = 0; i < UDSP_LINE_MAX * UDSP_STREAM_MAX; i++) {
		free(s->stream[i]);
		s->stream[i] = NULL;
	}
}

bool stream_line_active(const struct udsp *s, unsigned line)
{
	const struct udsp_stream *st;

	if (line >= udsp_lines(s))
		return false;

	stream_line_for_each(s, line, st) {
		if (atomic_load_explicit(&st->active, memory_order_acquire))
			return true;
	}

	return false;
}

/*
 * Which streams speak for the line's canceller.
 *
 * A dead-marked stream stays active and keeps its profile, because the
 * bounded-stop rule releases nothing a lost ack left in doubt (stream.h). The
 * canceller is the line's, so counting that stream would hold it bypassed for
 * every healthy stream on the line until the handle closes, which is the
 * per-stream failure isolation the mark exists to keep.
 *
 * A leg the caller put on local hold does not govern the line either. It puts
 * nothing of its own on the wire (udsp.h hold-tx), so its carrier is not on the
 * line any more, and the other call of the line needs its canceller and its
 * detectors back for as long as the hold lasts.
 */
static bool stream_governs(const struct udsp_stream *st)
{
	if (!st || !atomic_load_explicit(&st->active, memory_order_acquire))
		return false;
	if (atomic_load_explicit(&st->dead, memory_order_acquire))
		return false;

	return !st->media.tx_silence;
}

/*
 * The canceller belongs to the line and the line has one, so two streams that
 * carry different kinds still get one policy. The modem kind wins: it takes
 * the canceller off the frame, which is the safe answer for a carrier of
 * either kind, and the fax kind's own policy only relaxes it.
 */
enum udsp_passthrough stream_line_passthrough(const struct udsp *s,
					      unsigned line)
{
	enum udsp_passthrough kind = UDSP_PASSTHROUGH_OFF;
	const struct udsp_stream *st;

	if (line >= udsp_lines(s))
		return UDSP_PASSTHROUGH_OFF;

	stream_line_for_each(s, line, st) {
		if (!stream_governs(st))
			continue;
		if (st->passthrough_kind == UDSP_PASSTHROUGH_MODEM)
			return UDSP_PASSTHROUGH_MODEM;
		if (st->passthrough_kind == UDSP_PASSTHROUGH_FAX)
			kind = UDSP_PASSTHROUGH_FAX;
	}

	return kind;
}

unsigned stream_line_govern_count(const struct udsp *s, unsigned line)
{
	const struct udsp_stream *st;
	unsigned count = 0;

	if (line >= udsp_lines(s))
		return 0;

	stream_line_for_each(s, line, st) {
		if (stream_governs(st))
			count++;
	}

	return count;
}

static bool stream_line_used(struct udsp *s, unsigned line,
			     const struct udsp_stream *skip)
{
	const struct udsp_stream *st;

	stream_line_for_each(s, line, st) {
		if (st == skip)
			continue;
		if (atomic_load_explicit(&st->state, memory_order_relaxed) !=
		    STREAM_IDLE)
			return true;
	}

	return false;
}

/* --- validation ---------------------------------------------------------- */

static const struct udsp_codec_ops *codec_find(uint8_t sel)
{
	const struct udsp_codec_ops *const *p = udsp_codec_builtin.ops;

	for (; p && *p; p++) {
		if ((unsigned)(*p)->id == sel)
			return *p;
	}

	return NULL;
}

/*
 * codec.h states ptime_mask bit k-1 as the permission for a k*10 ms packet,
 * and every fixed-frame codec allows all three. The payload bound is the
 * codec's own claim:
 * a codec whose widest packet cannot fit the transmit staging is refused here
 * rather than met with a per-frame -ENOBUFS on every tick of a live call.
 */
static int media_codec_check(const struct udsp_media *m,
			     const struct udsp_codec_ops *ops)
{
	unsigned slot = m->ptime_ms / UDSP_TICK_MS;

	if (!ops)
		return -EINVAL;
	if (!(ops->ptime_mask & (1u << (slot - 1u))))
		return -EINVAL;
	if (ops->max_payload > UDSP_CODEC_PAYLOAD_MAX)
		return -EINVAL;

	return 0;
}

/* The mode key is the codec selector, which is what the backend moves when it
 * programs the T.38 transition (udsp.h UDSP_SEL_T38). */
static bool media_t38(const struct udsp_media *m)
{
	return m->codec == UDSP_SEL_T38;
}

/*
 * What this description carries, speech or a data carrier of one kind. The
 * T.38 mode reads as the modem kind: the gateway demodulates the carrier
 * itself and needs the canceller off the frame whole, which is the modem
 * policy exactly.
 */
static enum udsp_passthrough media_passthrough_kind(const struct udsp_media *m)
{
	if (media_t38(m))
		return UDSP_PASSTHROUGH_MODEM;

	return (enum udsp_passthrough)m->passthrough;
}

/* Which chain a description asks the tick to run (enum stream_mode). */
static uint8_t stream_mode_get(const struct udsp_media *m)
{
	return media_t38(m) ? STREAM_MODE_T38 : STREAM_MODE_AUDIO;
}

/*
 * The fax bit rates of the V.27ter, V.29 and V.17 modulations. hal.h asks the
 * backend for the highest fax rate that does not exceed the request, so a
 * request of zero and a request below the lowest rate both take the top rate:
 * a peer that named no ceiling asks for every rate the gateway has.
 */
#define T38_BIT_RATE_COUNT	6u

static const uint32_t t38_bit_rates[T38_BIT_RATE_COUNT] = {
	2400u, 4800u, 7200u, 9600u, 12000u, 14400u,
};

static uint32_t t38_bit_rate_settle(uint32_t asked)
{
	uint32_t out = t38_bit_rates[T38_BIT_RATE_COUNT - 1u];

	if (!asked || asked < t38_bit_rates[0])
		return out;
	for (unsigned i = 0; i < T38_BIT_RATE_COUNT; i++) {
		if (t38_bit_rates[i] <= asked)
			out = t38_bit_rates[i];
	}

	return out;
}

/*
 * The T.38 mode's own description. The gateway is not a codec, so nothing
 * here looks one up, and the fields hal.h scopes out of the mode (the jitter
 * buffer, the payload types, the packet duration, the voice detector and the
 * comfort noise) reach no reader in it and stay as the caller passed them.
 */
static int media_t38_prepare(const struct udsp *s, struct udsp_media *out)
{
	if (!s->fax_ops)
		return -EINVAL;
	if (out->direction > UDSP_MEDIA_SENDRECV)
		return -EINVAL;
	/* The net thread unprotects a datagram in place before the engine sees
	 * it, and a UDPTL datagram is not an SRTP packet. */
	if (out->srtp.suite != UDSP_SRTP_NONE)
		return -EINVAL;
	if (out->t38.ec > UDSP_T38_EC_REDUNDANCY)
		return -EINVAL;
	/* T.30 paces the image packets and a UDPTL session carries no reports,
	 * so no sender report, receiver report or goodbye has a place to go.
	 * The interval keeps the RFC 3550 6.2 floor the audio path gives it,
	 * because the tick compares against it every time and a zero would
	 * call the report builder on every one of them to be turned away. */
	out->rtcp_enable = false;
	if (out->rtcp_interval_ms < UDSP_RTCP_MIN_MS)
		out->rtcp_interval_ms = UDSP_RTCP_MIN_MS;
	out->t38.max_bit_rate = t38_bit_rate_settle(out->t38.max_bit_rate);
	/* The peer's receive limit is our send bound: udptl_out fills the
	 * transmit staging through a cap of UDSP_NET_DGRAM_MAX, and 576 is the
	 * IPv4 minimum reassembly buffer. Zero keeps the plug-in default. */
	if (out->t38.max_datagram > UDSP_NET_DGRAM_MAX)
		out->t38.max_datagram = (uint16_t)UDSP_NET_DGRAM_MAX;

	return 0;
}

static int media_prepare(const struct udsp *s, const struct udsp_media *in,
			 struct udsp_media *out)
{
	const struct udsp_codec_ops *ops;

	*out = *in;
	/* Ahead of the mode split, because the kind is a closed set in both
	 * modes: the T.38 mode reads as the modem kind whatever the field
	 * holds, and a value outside the set is a caller that means something
	 * this library does not carry. */
	if (out->passthrough > UDSP_PASSTHROUGH_FAX)
		return -EINVAL;
	/* Ahead of the split as well: the punch holds the tuple of either
	 * mode, and a zero means the RFC 6263 default in both. */
	if (!out->punch_interval_ms)
		out->punch_interval_ms = UDSP_TX_PUNCH_DEFAULT_MS;
	if (media_t38(out))
		return media_t38_prepare(s, out);
	if (!out->ptime_ms)
		out->ptime_ms = STREAM_PTIME_DEFAULT;
	/* udsp_media.ptime_ms: a 40 ms offer is clamped, anything else
	 * outside the three supported values is refused. */
	if (out->ptime_ms == 40u)
		out->ptime_ms = 30u;
	if (out->ptime_ms != 10u && out->ptime_ms != 20u && out->ptime_ms != 30u)
		return -EINVAL;
	if (out->codec_pt > 127u || out->dtmf_pt > 127u)
		return -EINVAL;
	if (out->direction > UDSP_MEDIA_SENDRECV || out->jb_mode > UDSP_JB_OFF)
		return -EINVAL;
	if (out->dtmf_mode > UDSP_DTMF_INFO)
		return -EINVAL;
	if (out->srtp.suite > UDSP_SRTP_AES_CM_128_SHA1_32)
		return -EINVAL;
	ops = codec_find(out->codec);
	if (media_codec_check(out, ops))
		return -EINVAL;
	/*
	 * RFC 3389 comfort noise pairs with every codec that does not carry
	 * discontinuous transmission of its own, G.722 included,
	 * because the stream's RTP clock is 8000 either way and the decoder
	 * synthesises the noise at its own sample rate. A codec that says
	 * UDSP_CODEC_F_NO_CN takes the voice detector down with it: a talkspurt
	 * this layer closed would leave the far end nothing to play.
	 */
	if (ops->flags & UDSP_CODEC_F_NO_CN) {
		out->vad = false;
		out->cng = false;
	}
	/* The codec-native pair replaces the generic one, the F_NO_CN rule
	 * applied from the other side: with the codec's own detector on the
	 * encoder, a generic detector would close talkspurts the codec still
	 * describes, and PT 13 would describe a silence the SID already does.
	 * A codec with no SID frame has no native pair to offer, so the flag
	 * means nothing on it and is cleared rather than stored. */
	if (out->dtx && ops->sid_bytes) {
		out->vad = false;
		out->cng = false;
	}
	if (!ops->sid_bytes)
		out->dtx = false;
	/* A closed talkspurt with nothing that describes the silence leaves
	 * the far end digital silence and, on a backward-adaptive codec, a
	 * decoder out of step with the encoder at every onset: the generic
	 * detector runs only beside the comfort noise it sends. This follows
	 * the dtx rules above so that a dtx which meant nothing on the codec
	 * cannot stand in for the noise. */
	if (!out->cng)
		out->vad = false;
	/* A data carrier has no talkspurt: the detector clips its training
	 * sequences and comfort noise replaces them with a hiss the far end
	 * demodulates as data. An adaptive buffer moves the very samples the
	 * far end demodulates, and a buffer that is off hands every reordered
	 * packet straight through. The kind therefore carries the whole
	 * non-codec half of the fax profile, so a caller that writes it into
	 * its own description gets the same treatment the profile gives and
	 * cannot pair a carrier with a playout that does not suit it. */
	if (out->passthrough != UDSP_PASSTHROUGH_OFF) {
		out->vad = false;
		out->cng = false;
		out->dtx = false;
		out->jb_mode = UDSP_JB_FIXED;
	}
	if (!out->jb_init_ms)
		out->jb_init_ms = STREAM_JB_INIT_MS;
	if (!out->jb_max_ms)
		out->jb_max_ms = STREAM_JB_MAX_MS;
	if (out->jb_max_ms < out->jb_init_ms)
		out->jb_max_ms = out->jb_init_ms;
	/* RFC 3550 floors the interval; a zero means the board default. */
	if (out->rtcp_interval_ms < UDSP_RTCP_MIN_MS)
		out->rtcp_interval_ms = UDSP_RTCP_MIN_MS;

	return 0;
}

static bool media_sends(const struct udsp_media *m)
{
	return m->direction == UDSP_MEDIA_SENDONLY ||
	       m->direction == UDSP_MEDIA_SENDRECV;
}

static bool media_receives(const struct udsp_media *m)
{
	return m->direction == UDSP_MEDIA_RECVONLY ||
	       m->direction == UDSP_MEDIA_SENDRECV;
}

/*
 * Whether this description may put an RFC 4733 event on the wire. The event
 * payload type is the sender's own rule (rtp.h media_event_pt_get): dtmf_pt 0
 * is hal.h's not-negotiated, and a type the codec already uses names no event
 * either. A direction that does not send has no wire to put one on. A stream
 * told to send silence puts nothing of its own on the wire, and a digit is as
 * much its own as the room is (udsp.h hold-tx). A stream that carries a data
 * carrier refuses one because the event packets replace carrier frames the
 * far end demodulates, the T.38 mode reading as the modem kind.
 *
 * Three readers share it: udsp_stream_dtmf on the control thread, the line's
 * own digit on the engine tick, and the cut a new description makes. It tests
 * the description alone, so all three answer the same question.
 */
static bool stream_relay_allows(const struct udsp_stream *st)
{
	return media_event_pt_get(&st->media) != RTP_PT_DISABLED &&
	       media_sends(&st->media) && !st->media.tx_silence &&
	       media_passthrough_kind(&st->media) == UDSP_PASSTHROUGH_OFF;
}

static void cname_build(struct udsp_stream *st, const struct udsp_media *m)
{
	char host[INET6_ADDRSTRLEN];

	snprintf(host, sizeof(host), "%s", "0.0.0.0");
	if (m->local.ipver == 4)
		inet_ntop(AF_INET, m->local.addr, host, sizeof(host));
	else if (m->local.ipver == 6)
		inet_ntop(AF_INET6, m->local.addr, host, sizeof(host));
	snprintf(st->cname, sizeof(st->cname), "udsp%u%u@%s", st->line, st->index,
		 host);
}

/* The sender needs three independent draws, because a sequence derived from
 * the SSRC is predictable from one observed packet. */
static int entropy_fill(struct rtp_entropy *e)
{
	uint8_t buf[12];
	size_t got = 0;

	while (got < sizeof(buf)) {
		ssize_t n = getrandom(buf + got, sizeof(buf) - got, 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		got += (size_t)n;
	}
	memcpy(&e->ssrc, buf, 4);
	memcpy(&e->sequence, buf + 4, 4);
	memcpy(&e->timestamp, buf + 8, 4);
	e->sequence &= 0xffffu;

	return 0;
}

/* --- control-side resources ---------------------------------------------- */

static int fd_dup_nb(int fd, int *out)
{
	int flags;
	int n;

	*out = -1;
	if (fd < 0)
		return 0;
	n = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (n < 0)
		return -errno;
	/* The dup shares its file status flags with the caller's descriptor, so
	 * this also marks the backend's socket; every send and receive still
	 * passes MSG_DONTWAIT and never depends on it. */
	flags = fcntl(n, F_GETFL, 0);
	if (flags >= 0)
		fcntl(n, F_SETFL, flags | O_NONBLOCK);
	*out = n;

	return 0;
}

static void fd_drop(int *fd)
{
	if (*fd >= 0)
		close(*fd);
	*fd = -1;
}

struct codec_pair {
	const struct udsp_codec_ops *ops;
	struct udsp_codec *enc;
	struct udsp_codec *dec;
	struct udsp_codec_params params;
};

/*
 * codec.h scopes params.ptime_ms and the Opus record to the whole-packet
 * codecs, which are the only ones that read either of them. A fixed-frame
 * codec encodes one 10 ms frame per call and ignores both, so a description
 * refresh that moves a packet duration or an Opus field must not recreate a
 * live G.711 or G.726 pair and throw its concealment history away. line_rate
 * is unconditional: it is what a codec that follows the line is created
 * against, and it never changes under a handle.
 *
 * plc_off is unconditional too, and it is what a data carrier moves: the
 * instance of a passthrough keeps no concealment history, so the description
 * that turns a kind on recreates the pair and the description that turns it
 * off recreates it again. That is the whole removal of the stale App I state,
 * and it costs one create on a boundary that already re-inits the buffer.
 */
static void codec_params_build(const struct udsp *s, const struct udsp_media *m,
			       const struct udsp_codec_ops *ops,
			       struct udsp_codec_params *out)
{
	memset(out, 0, sizeof(*out));
	out->line_rate = s->rate;
	out->plc_off = media_passthrough_kind(m) != UDSP_PASSTHROUGH_OFF;
	/* Unconditional, as plc_off is: the encoder consumed it at create, so
	 * a description that flips it reaches the instance only through a new
	 * pair. media_prepare already cleared it on a codec with no SID
	 * frame. */
	out->dtx = m->dtx;
	if (!(ops->flags & UDSP_CODEC_F_PACKET))
		return;

	out->ptime_ms = m->ptime_ms;
	out->bitrate = m->opus.bitrate;
	out->complexity = m->opus.complexity;
	out->max_bandwidth = m->opus.max_bandwidth;
	out->fec = m->opus.fec;
}

/* Member by member, never memcmp: the record has padding and a create that
 * never wrote it would decide the comparison. */
static bool codec_params_same(const struct udsp_codec_params *a,
			      const struct udsp_codec_params *b)
{
	return a->line_rate == b->line_rate && a->ptime_ms == b->ptime_ms &&
	       a->bitrate == b->bitrate && a->complexity == b->complexity &&
	       a->max_bandwidth == b->max_bandwidth && a->fec == b->fec &&
	       a->plc_off == b->plc_off && a->dtx == b->dtx;
}

static void codec_pair_free(struct codec_pair *p)
{
	if (p->ops && p->enc)
		p->ops->destroy(p->enc);
	if (p->ops && p->dec)
		p->ops->destroy(p->dec);
	memset(p, 0, sizeof(*p));
}

/*
 * Builds the instances aside and installs nothing: until every fallible step
 * of the reconfiguration has succeeded, the stream keeps the codec it runs.
 * An empty pair means the stream already holds this codec.
 *
 * The same vtable is not the same codec: a create consumed the parameters, so
 * a description that moves one reaches the instance only through a new pair.
 */
static int codec_pair_create(const struct udsp_stream *st,
			     const struct udsp_codec_ops *ops,
			     const struct udsp_codec_params *params,
			     struct codec_pair *out)
{
	memset(out, 0, sizeof(*out));
	if (st->ops == ops && st->enc && st->dec &&
	    codec_params_same(&st->params, params))
		return 0;

	out->ops = ops;
	out->params = *params;
	out->enc = ops->create(params);
	out->dec = ops->create(params);
	if (out->enc && out->dec)
		return 0;

	codec_pair_free(out);

	return -ENOMEM;
}

static void codec_pair_install(struct udsp_stream *st, struct codec_pair *in)
{
	struct codec_pair old = { st->ops, st->enc, st->dec, st->params };

	if (!in->ops)
		return;
	st->ops = in->ops;
	st->enc = in->enc;
	st->dec = in->dec;
	st->params = in->params;
	memset(in, 0, sizeof(*in));
	codec_pair_free(&old);
}

/*
 * The stream leaves the audio mode. codec_pair_install returns early on an
 * empty pair and the teardown does not run on the reconfiguration path, so
 * this is the only thing that frees a pair the T.38 mode displaced.
 */
static void stream_codec_drop(struct udsp_stream *st)
{
	struct codec_pair old = { st->ops, st->enc, st->dec, st->params };

	st->ops = NULL;
	st->enc = NULL;
	st->dec = NULL;
	/* The record of what the instances were created with dies with them,
	 * so a later configuration compares against nothing. */
	memset(&st->params, 0, sizeof(st->params));
	codec_pair_free(&old);
}

static void t38_params_build(const struct udsp_media *m,
			     struct udsp_t38_params *out)
{
	memset(out, 0, sizeof(*out));
	out->max_bit_rate = m->t38.max_bit_rate;
	out->max_datagram = m->t38.max_datagram;
	out->ec = m->t38.ec;
}

/* Member by member, for the reason codec_params_same is: the record has
 * padding a create never wrote. */
static bool t38_params_same(const struct udsp_t38_params *a,
			    const struct udsp_t38_params *b)
{
	return a->max_bit_rate == b->max_bit_rate &&
	       a->max_datagram == b->max_datagram && a->ec == b->ec;
}

static void fax_gate_free(struct fax_gate *g)
{
	if (g->ops && g->inst)
		g->ops->destroy(g->inst);
	memset(g, 0, sizeof(*g));
}

static int fax_gate_create(const struct udsp_stream *st,
			   const struct udsp_fax_ops *ops,
			   const struct udsp_t38_params *params,
			   struct fax_gate *out)
{
	memset(out, 0, sizeof(*out));
	if (st->fax_gate.ops == ops && st->fax_gate.inst &&
	    t38_params_same(&st->fax_gate.params, params))
		return 0;

	out->ops = ops;
	out->params = *params;
	/* udsp/fax.h takes the resolved values; media_prepare settled all
	 * three before this call. */
	out->inst = ops->create((int)params->max_bit_rate,
				(int)params->max_datagram, (int)params->ec);
	if (out->inst)
		return 0;

	fax_gate_free(out);

	return -ENOMEM;
}

static void fax_gate_install(struct udsp_stream *st, struct fax_gate *in)
{
	struct fax_gate old = st->fax_gate;

	if (!in->ops)
		return;
	st->fax_gate = *in;
	memset(in, 0, sizeof(*in));
	fax_gate_free(&old);
}

/* The stream leaves the T.38 mode, or ends: the gateway goes with it. See
 * rule 3 of core/fax_plugin.h. */
static void stream_fax_drop(struct udsp_stream *st)
{
	fax_gate_free(&st->fax_gate);
}

/* A bare reconfiguration rekeys the existing object; only a stream that had
 * no session at all gets a fresh create. */
static int stream_srtp_set(struct udsp_stream *st, const struct udsp_media *m)
{
	int rc;

	if (m->srtp.suite == UDSP_SRTP_NONE) {
		if (st->srtp_live)
			udsp_srtp_destroy(&st->srtp);
		st->srtp_live = false;
		atomic_store_explicit(&st->srtp_armed, false,
				      memory_order_release);
		return 0;
	}
	rc = st->srtp_live ? udsp_srtp_rekey(&st->srtp, m) :
			     udsp_srtp_create(&st->srtp, m);
	if (rc)
		return rc;
	st->srtp_live = true;
	atomic_store_explicit(&st->srtp_armed, true, memory_order_release);

	return 0;
}

/* made reports whether this call published the canceller, so a configuration
 * that fails later knows which one is its own to take back. */
static int stream_lec_ensure(struct udsp *s, struct udsp_chan *c, bool *made)
{
	struct udsp_ec *ec;

	*made = false;
	if (!c->cfg.ec_enable ||
	    atomic_load_explicit(&c->ec, memory_order_acquire))
		return 0;

	/* The canceller runs on the channel frame, so it is created at the line
	 * rate and not at any codec's (dsp/lec.h create_rate).
	 * create_rate(tail, 8000) is create(tail), so the narrowband line keeps
	 * the instance it had. */
	ec = udsp_ec_nlms.create_rate(c->cfg.ec_tail_ms, c->rate);
	if (!ec)
		return -ENOMEM;
	udsp_ec_nlms.nlp(ec, c->cfg.ec_nlp_enable);
	atomic_store_explicit(&c->ec, ec, memory_order_release);
	*made = true;

	return 0;
}

/*
 * Reclaims the line canceller for the control thread and returns the instance
 * to free, or NULL when there is none.
 *
 * chan_lec_publish reads c->ec on every tick, active stream or not, so a
 * control-side free must not race a tick that already loaded the pointer. The
 * tick answers UDSP_CMD_LEC_RELEASE by moving c->ec into the c->ec_retired
 * mailbox and nulling c->ec inside the command drain, ahead of that tick's
 * publish loop, so once the mailbox holds an instance no tick reads it and the
 * free is safe (dsp/lec.h rule 1).
 *
 * The harvest is an acquire exchange and does not depend on the ack. On a raced
 * timeout the tick may have run the retire yet lost the ack; the instance still
 * sits in the mailbox, and the retire fills the mailbox only when empty, so no
 * later release overwrote it. A harvest that returns non-NULL read the tick's
 * release store of the pointer. That store follows every tick access to the
 * instance, the read for the END record included, so each one comes before
 * this free. The first branch below runs where engine_live reads false: before
 * the first start, after a clean stop and in stream_pool_shutdown after the
 * join. No tick reads c->ec there, so the exchanges alone are enough. A stop
 * that timed out also clears engine_live, without a join; the exchanges of the
 * branch already carry that hazard. The control thread runs the drop of the
 * tick's release handler in the same branch, on the same argument. Without it,
 * the line keeps the instance as the one the tick last handed a frame. The
 * next engine would then decide a reference for a line that has no canceller.
 */
static struct udsp_ec *stream_lec_reclaim(struct udsp *s, struct udsp_chan *c)
{
	struct udsp_cmd cmd;
	struct udsp_ec *pending;

	if (!atomic_load_explicit(&s->engine_live, memory_order_acquire)) {
		pending = atomic_exchange_explicit(&c->ec_retired, NULL,
						   memory_order_acquire);
		if (pending)
			udsp_ec_nlms.destroy(pending);
		if (atomic_load_explicit(&c->ec, memory_order_acquire))
			chan_lec_tracker_drop(s, c);
		return atomic_exchange_explicit(&c->ec, NULL,
						memory_order_acquire);
	}

	/* No live canceller: a retire may have just published one into the
	 * mailbox and nulled c->ec. Its ec-null store is release, sequenced
	 * after the store into the mailbox, so a NULL read here happens-after
	 * that store and the harvest below sees the instance. Take it rather
	 * than leave the free to shutdown. */
	if (!atomic_load_explicit(&c->ec, memory_order_acquire))
		return atomic_exchange_explicit(&c->ec_retired, NULL,
						memory_order_acquire);

	/* Drain any earlier retiree so this release fills an empty mailbox
	 * rather than defer. No reachable path leaves one pending. */
	pending = atomic_exchange_explicit(&c->ec_retired, NULL,
					   memory_order_acquire);
	if (pending)
		udsp_ec_nlms.destroy(pending);

	memset(&cmd, 0, sizeof(cmd));
	cmd.type = UDSP_CMD_LEC_RELEASE;
	cmd.line = c->line;
	/* The ack outcome does not gate the harvest. On a raced timeout the tick
	 * may have run the retire yet lost the ack, so the instance is in the
	 * mailbox regardless. The retire fills only an empty mailbox, so no
	 * release overwrote it. udsp_sync_locked still marks the transient
	 * error. */
	udsp_sync_locked(s, &cmd, cmd_sync_tick_ms_get());

	return atomic_exchange_explicit(&c->ec_retired, NULL,
					memory_order_acquire);
}

/* Reached once every stream of the line is idle. The canceller free is handed
 * off through stream_lec_reclaim so the tick never reads a freed instance. */
static void stream_lec_release(struct udsp *s, struct udsp_chan *c)
{
	struct udsp_ec *ec = stream_lec_reclaim(s, c);

	if (ec)
		udsp_ec_nlms.destroy(ec);
	c->fax_active = false;
	/* The teardown release of the tone disabler: the line has no stream
	 * left to govern the latch and no canceller left for it to act on.
	 * Under udsp_chan_cfg.ec_tone_latch it is one of three releases, with
	 * the topology guard and a caller clear. */
	chan_fax_latch_clear(c);
}

/* --- control-side commands ----------------------------------------------- */

static enum udsp_status stream_cmd_post(struct udsp *s, struct udsp_stream *st,
					enum stream_op op, unsigned flags,
					uint32_t arg1, int32_t arg2)
{
	struct udsp_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.type = UDSP_CMD_STREAM;
	cmd.line = st->line;
	cmd.stream = st->index;
	cmd.flags = (uint8_t)flags;
	cmd.arg0 = (uint32_t)op;
	cmd.arg1 = arg1;
	cmd.arg2 = arg2;

	return udsp_sync_locked(s, &cmd, cmd_sync_tick_ms_get());
}

/* The conference and the loop are properties of the line, so their command
 * names stream 0, whose lookup on the engine is what proves the line exists.
 * Both callers checked the line, so the slot is there. */
static enum udsp_status stream_line_cmd_post(struct udsp *s, unsigned line,
					     enum stream_op op, uint32_t arg1)
{
	return stream_cmd_post(s, stream_at(s, line, 0), op, 0, arg1, 0);
}

/*
 * The stream is gone from the caller's point of view and no retry can bring it
 * back, so the answer is UDSP_FATAL, not UDSP_TRANSIENT. See the dead note in
 * stream.h for why nothing is released here.
 */
static enum udsp_status stream_dead_mark(struct udsp *s, struct udsp_stream *st)
{
	if (!atomic_exchange_explicit(&st->dead, true, memory_order_acq_rel))
		udsp_op_error(s);

	return UDSP_FATAL;
}

static enum udsp_status stream_activate(struct udsp *s, struct udsp_stream *st)
{
	enum udsp_status rc;
	bool off;

	/* Make this stream's RT buffers resident before the engine adopts it and
	 * before the net thread receives into the ring, so under MCL_ONFAULT
	 * neither real-time thread takes a first-touch fault. Only an activating
	 * stream pays this, so the idle pool stays out of RSS. Every staging
	 * array the tick writes is named here rather than left to the structure
	 * layout: a field that happens to share a page with a named one is
	 * covered by accident, and a layout change withdraws that cover with no
	 * sign at all. */
	rt_range_prefault(&st->ring, sizeof(st->ring));
	rt_range_prefault(&st->jb, sizeof(st->jb));
	rt_range_prefault(st->rx_unit, sizeof(st->rx_unit));
	/* The tick writes this one on the audio decode path and again on the
	 * T.38 receive path, so under MCL_ONFAULT it faults there without
	 * this. */
	rt_range_prefault(st->rx_pcm, sizeof(st->rx_pcm));
	rt_range_prefault(st->tx_pcm, sizeof(st->tx_pcm));
	rt_range_prefault(st->pkt, sizeof(st->pkt));

	/*
	 * The mode the receive trace labels a datagram with (stream.h
	 * net_mode), published here and nowhere else: this is the one point
	 * where the descriptors are outside the epoll set and the committed
	 * description is the one they are about to carry. A publish while they
	 * are inside it would label a datagram of the session that is ending
	 * with the mode of the one that follows, which is what the stop does
	 * to the engine's own copy.
	 */
	atomic_store_explicit(&st->net_mode, stream_mode_get(&st->media),
			      memory_order_relaxed);

	rc = stream_cmd_post(s, st, STREAM_OP_ACTIVATE, 0, 0, 0);
	if (rc != UDSP_OK)
		return rc;
	if (!net_attach(s, st->line, st->index, st->rtp_fd, st->rtcp_fd))
		return UDSP_OK;

	/* net_cmd_attach installs the epoll entries before its ack, so a lost
	 * ack leaves the net side attached while only the engine rolls back.
	 * Every failed attach is therefore paired with a detach, and a detach
	 * that fails in turn takes the dead mark. */
	off = stream_cmd_post(s, st, STREAM_OP_DEACTIVATE, 0, 0, 0) == UDSP_OK;
	if (net_detach(s, st->line, st->index) || !off)
		return stream_dead_mark(s, st);

	return UDSP_TRANSIENT;
}

/*
 * The two quiesce orders of udsp.h section 3 are not the same. A
 * reconfiguration takes the descriptors out of the epoll set first, so no
 * datagram of the old description can still be queued when the engine adopts
 * the new one; a stop deactivates the engine first, because the BYE has to
 * leave while the engine still holds the descriptors.
 */
static enum udsp_status stream_quiesce(struct udsp *s, struct udsp_stream *st,
				       bool bye)
{
	enum udsp_status rc = UDSP_OK;

	if (!bye && net_quiesce(s, st->line, st->index))
		rc = UDSP_TRANSIENT;
	if (stream_cmd_post(s, st, STREAM_OP_DEACTIVATE, bye ? 1u : 0u, 0, 0) !=
	    UDSP_OK)
		rc = UDSP_TRANSIENT;
	if (bye && net_quiesce(s, st->line, st->index))
		rc = UDSP_TRANSIENT;

	return rc;
}

static void stream_teardown(struct udsp *s, struct udsp_stream *st)
{
	struct udsp_chan *c = &s->chan[st->line];

	fd_drop(&st->rtp_fd);
	fd_drop(&st->rtcp_fd);
	atomic_store_explicit(&st->srtp_armed, false, memory_order_release);
	if (st->srtp_live)
		udsp_srtp_destroy(&st->srtp);
	st->srtp_live = false;
	stream_codec_drop(st);
	stream_fax_drop(st);
	/* The key bytes of the stored description die with the stream. */
	explicit_bzero(&st->media.srtp, sizeof(st->media.srtp));
	memset(&st->media, 0, sizeof(st->media));
	memset(&st->entropy, 0, sizeof(st->entropy));
	/* The description this profile displaced is gone, so the record of it
	 * must go too. The engine half goes with the DEACTIVATE that got us
	 * here: an inactive stream governs no line. The engine's own view goes
	 * with it, because the next call's first adoption asks whether the
	 * stream was already in a passthrough, and the answer must be no: the
	 * depth this call pinned belongs to a line that is no longer up. Safe
	 * from this thread for the reason the description above is: the stream
	 * is deactivated. */
	memset(&st->fax, 0, sizeof(st->fax));
	st->passthrough_kind = UDSP_PASSTHROUGH_OFF;
	st->jb_pin_ms = 0;
	st->jb_ops = NULL;
	st->jb_ptime_ms = 0;
	atomic_store_explicit(&st->state, STREAM_IDLE, memory_order_release);
	if (stream_line_used(s, st->line, NULL))
		return;
	stream_lec_release(s, c);
	/* No stream carries the line any more, so there is nothing to mix and
	 * the arm must not outlive the call. Safe here for the same reason the
	 * canceller release is: the engine runs no media chain on an idle line. */
	c->conf_mode = UDSP_CONF_OFF;
}

/* Called from udsp_close once every thread that could touch a stream has
 * stopped: a caller that closed the handle without stopping its streams still
 * gets its descriptors closed and its key material zeroized. */
void stream_pool_shutdown(struct udsp *s)
{
	for (unsigned i = 0; i < (unsigned)s->cap.lines * UDSP_STREAM_MAX; i++) {
		struct udsp_stream *st = s->stream[i];
		bool held;

		if (!st)
			continue;
		/* A dead-marked stream can sit in any state, so its descriptors
		 * and its key material are released here and nowhere else. */
		held = atomic_load_explicit(&st->dead, memory_order_relaxed) ||
		       atomic_load_explicit(&st->state, memory_order_relaxed) !=
		       STREAM_IDLE;
		if (held)
			stream_teardown(s, st);
	}
	for (unsigned i = 0; i < s->cap.lines; i++)
		stream_lec_release(s, &s->chan[i]);
	stream_pool_free(s);
}

/* --- the public ops ------------------------------------------------------ */

/* Every stream op reads the mark again under ctl_lock: another call may have
 * marked the stream while this one waited for the lock, and the mark must
 * outlive that wait. */
static bool stream_dead(const struct udsp_stream *st)
{
	return atomic_load_explicit(&st->dead, memory_order_acquire);
}

/*
 * The mark is read before the lifecycle answer: a stopped
 * engine reports UDSP_TRANSIENT, and that invites the retry a dead stream can
 * never satisfy. This runs without ctl_lock, so it is the cheap fast path only;
 * every op rechecks under the lock, where the mark cannot move under it.
 */
/* release admits the op on a dead line (udsp_release_guard); only the stop
 * asks for it, because it is the one op that gives a line's stream back. */
static enum udsp_status stream_guard(struct udsp *s, unsigned line,
				     unsigned stream, bool release,
				     struct udsp_stream **out)
{
	struct udsp_stream *st = stream_at(s, line, stream);
	enum udsp_status g;

	*out = NULL;
	if (st && stream_dead(st))
		return UDSP_FATAL;

	g = release ? udsp_release_guard(s, line) : udsp_guard(s, line, false);
	if (g != UDSP_OK)
		return g;
	if (stream >= s->cap.streams_per_chan)
		return UDSP_UNSUPPORTED;
	*out = st;

	return st ? UDSP_OK : UDSP_UNSUPPORTED;
}

struct stream_prep {
	int rtp_fd;
	int rtcp_fd;
	struct codec_pair codec;
	struct fax_gate fax;
	bool lec_made;		/* this prepare published the line's canceller */
};

static void stream_prep_drop(struct udsp *s, struct udsp_stream *st,
			     struct stream_prep *p)
{
	fd_drop(&p->rtp_fd);
	fd_drop(&p->rtcp_fd);
	codec_pair_free(&p->codec);
	fax_gate_free(&p->fax);
	/* A canceller this prepare published belongs to a configuration that
	 * never happened, so it runs on a line nobody asked for it on until the
	 * handle closes. It goes back only when no other stream holds the line:
	 * one an earlier configuration made is not this one's to take. */
	if (p->lec_made && !stream_line_used(s, st->line, NULL))
		stream_lec_release(s, &s->chan[st->line]);
	p->lec_made = false;
}

/*
 * Test-only fault injection. The prepare error path only matters for a failure
 * after the canceller is published, and every step that can produce one lives
 * in src/rtp or src/dsp. A test arms the errno it wants from the step between
 * the codec and the session; zero, the value of every shipped run, disarms it.
 * One relaxed load per configuration.
 */
static int stream_prep_inject(struct udsp *s)
{
	uint_least32_t err = atomic_load_explicit(&s->prep_fail,
						  memory_order_relaxed);

	if (!err)
		return 0;
	atomic_store_explicit(&s->prep_fail, 0u, memory_order_relaxed);

	return -(int)err;
}

static int stream_prep_codec(struct udsp *s, struct udsp_stream *st,
			     const struct udsp_media *m,
			     const struct udsp_codec_ops *ops,
			     struct stream_prep *p)
{
	struct udsp_codec_params params;

	codec_params_build(s, m, ops, &params);

	return codec_pair_create(st, ops, &params, &p->codec);
}

static int stream_prep_gateway(struct udsp *s, struct udsp_stream *st,
			       const struct udsp_media *m,
			       struct stream_prep *p)
{
	struct udsp_t38_params params;

	t38_params_build(m, &params);

	return fax_gate_create(st, s->fax_ops, &params, &p->fax);
}

/*
 * Nothing the engine or the net thread can see changes until the last step.
 * The descriptors and the codec instances are built aside; the SRTP session
 * is the one unavoidable in-place mutation, because a fresh create on a bare
 * reconfiguration would reset the replay window and libsrtp has no way to
 * stage a rekey, so it goes last and nothing follows it that can fail.
 */
static int stream_prepare(struct udsp *s, struct udsp_stream *st,
			  const struct udsp_media *m,
			  const struct udsp_codec_ops *ops, int rtp_fd,
			  int rtcp_fd, struct stream_prep *p)
{
	int err;

	memset(p, 0, sizeof(*p));
	p->rtp_fd = -1;
	p->rtcp_fd = -1;

	err = fd_dup_nb(rtp_fd, &p->rtp_fd);
	if (!err)
		err = fd_dup_nb(rtcp_fd, &p->rtcp_fd);
	if (!err)
		err = stream_lec_ensure(s, &s->chan[st->line], &p->lec_made);
	if (!err)
		err = media_t38(m) ? stream_prep_gateway(s, st, m, p) :
				     stream_prep_codec(s, st, m, ops, p);
	if (!err)
		err = stream_prep_inject(s);
	if (!err)
		err = stream_srtp_set(st, m);
	if (err)
		stream_prep_drop(s, st, p);

	return err;
}

static void stream_commit(struct udsp_stream *st, struct stream_prep *p,
			  const struct udsp_media *m,
			  const struct rtp_entropy *entropy)
{
	codec_pair_install(st, &p->codec);
	fax_gate_install(st, &p->fax);
	/* The description that leaves a mode frees what that mode held: both
	 * installs above return early on an empty one, so nothing else
	 * would. */
	if (media_t38(m))
		stream_codec_drop(st);
	else
		stream_fax_drop(st);
	fd_drop(&st->rtp_fd);
	fd_drop(&st->rtcp_fd);
	st->rtp_fd = p->rtp_fd;
	st->rtcp_fd = p->rtcp_fd;
	p->rtp_fd = -1;
	p->rtcp_fd = -1;
	st->media = *m;
	st->entropy = *entropy;
	/* The caller's description replaces whatever the fax profile put in its
	 * place, so nothing is left to restore. */
	memset(&st->fax, 0, sizeof(st->fax));
	cname_build(st, m);
	/* The generation the packet trace records carry (stream.h gen). It
	 * moves with the description, so a reader that kept the addresses of
	 * each one can still place a record drained after a reconfiguration. */
	atomic_fetch_add_explicit(&st->gen, 1u, memory_order_relaxed);
}

/* Two published ticks. One is not enough: the tick publishes its clock before
 * it drains the command ring, so the publish that is ordered after the drain of
 * tick T is the one tick T+1 makes. */
#define STREAM_SETTLE_TICKS	2u
#define STREAM_SETTLE_MS	100u

/*
 * A stream command whose ack expired may still have run: cancellation withdraws
 * the ack, not the work (cmd.h Cancellation per command generation), and
 * stream_engine_adopt reads the stream description while it runs. The published
 * tick counter is a release store the load below acquires, so two of them put a
 * happens-before edge between that read and the write the control thread is
 * about to make.
 *
 * True only on proof. An engine preempted inside the cancelled command
 * publishes nothing, and the deadline expiring says only that the wait gave
 * up: it does not say the engine left. The caller must treat a false here as
 * a stream it may not free.
 */
static bool stream_tick_settle(struct udsp *s)
{
	uint64_t want = udsp_clock_tick(&s->clock) + STREAM_SETTLE_TICKS;
	uint64_t end = udsp_mono_us() + STREAM_SETTLE_MS * 1000u;

	while (udsp_clock_tick(&s->clock) < want) {
		struct timespec nap = { .tv_sec = 0, .tv_nsec = 200000L };

		if (udsp_mono_us() >= end)
			return false;
		nanosleep(&nap, NULL);
	}

	return true;
}

/* The old description is fully intact, so a running stream just comes back. */
static enum udsp_status stream_prep_fail(struct udsp *s, struct udsp_stream *st,
					 bool running, int err)
{
	udsp_op_error(s);
	if (running && stream_activate(s, st) != UDSP_OK)
		return stream_dead_mark(s, st);

	return cmd_status_map(err);
}

/*
 * An idle stream never activated anything and the engine never touches an
 * inactive stream, so the whole configuration is undone and the stream is clean
 * and reusable; the cancelled command's token guard discards a late execution's
 * ack. That undo needs proof that the engine left the cancelled command, and an
 * engine that published no tick inside the bound gave none: freeing on an
 * expiry would hand the resumed tick a destroyed session and freed codec
 * instances, which is the very hazard the settle exists for. Without the proof
 * the stream takes the dead mark instead, exactly as a running stream does;
 * the handle stays usable either way.
 */
static enum udsp_status stream_config_fail(struct udsp *s,
					   struct udsp_stream *st, bool running,
					   enum udsp_status rc)
{
	if (running || !stream_tick_settle(s))
		return stream_dead_mark(s, st);

	stream_teardown(s, st);
	udsp_op_error(s);

	return rc;
}

/*
 * A description that moves the RTP clock cannot be a reconfiguration: RFC
 * 7160 gives one clock per SSRC, and every timeline the
 * stream carries (the sender timestamp, the receiver's transit, the jitter
 * buffer's whole state, the telephone-event durations) is counted in the old
 * one. The source therefore ends and a new one begins, which is what
 * STREAM_OP_ADOPT does with the fresh entropy this call already drew.
 */
static bool stream_clock_moved(const struct udsp_stream *st,
			       const struct udsp_media *m,
			       const struct udsp_codec_ops *ops)
{
	bool was_t38 = media_t38(&st->media);
	bool now_t38 = media_t38(m);

	/* A move between audio and T.38 ends every timeline the stream
	 * carried, and one T.38 description after another keeps the session
	 * the gateway holds. The mode decides it, because the clock below
	 * cannot: one side of such a move has none. */
	if (was_t38 || now_t38)
		return was_t38 != now_t38;

	return !st->ops || st->ops->clock_rate != ops->clock_rate;
}

static enum udsp_status stream_config_locked(struct udsp *s,
					     struct udsp_stream *st,
					     const struct udsp_media *m,
					     const struct rtp_entropy *entropy,
					     int rtp_fd, int rtcp_fd)
{
	/* The gateway is not a codec and UDSP_SEL_T38 is a hole in the
	 * registry, so the T.38 mode carries no ops at all. */
	const struct udsp_codec_ops *ops = media_t38(m) ? NULL :
					   codec_find(m->codec);
	struct stream_prep prep;
	enum udsp_status rc;
	bool running;
	bool adopt;
	int err;

	running = atomic_load_explicit(&st->state, memory_order_relaxed) ==
		  STREAM_RUNNING;
	if (running && stream_quiesce(s, st, false) != UDSP_OK)
		return stream_dead_mark(s, st);

	err = stream_prepare(s, st, m, ops, rtp_fd, rtcp_fd, &prep);
	if (err)
		return stream_prep_fail(s, st, running, err);

	/* Before the commit, which installs the new pair: the question is what
	 * the stream ran at. */
	adopt = !running || stream_clock_moved(st, m, ops);
	stream_commit(st, &prep, m, entropy);
	rc = stream_cmd_post(s, st,
			     adopt ? STREAM_OP_ADOPT : STREAM_OP_RECONFIG,
			     0, 0, 0);
	if (rc == UDSP_OK && running)
		rc = stream_activate(s, st);
	if (rc != UDSP_OK)
		return stream_config_fail(s, st, running, rc);
	atomic_store_explicit(&st->state, running ? STREAM_RUNNING : STREAM_READY,
			      memory_order_release);

	return UDSP_OK;
}

enum udsp_status stream_config(struct udsp *s, unsigned line, unsigned stream,
			       const struct udsp_media *media, int rtp_fd,
			       int rtcp_fd)
{
	struct rtp_entropy entropy;
	struct udsp_stream *st;
	struct udsp_media m;
	enum udsp_status rc;

	rc = stream_guard(s, line, stream, false, &st);
	if (rc != UDSP_OK)
		return rc;
	if (!media || rtp_fd < 0 || media_prepare(s, media, &m))
		return UDSP_UNSUPPORTED;
	if (entropy_fill(&entropy))
		return UDSP_TRANSIENT;

	pthread_mutex_lock(&s->ctl_lock);
	rc = stream_dead(st) ? UDSP_FATAL :
	     stream_config_locked(s, st, &m, &entropy, rtp_fd, rtcp_fd);
	pthread_mutex_unlock(&s->ctl_lock);

	return rc;
}

static enum udsp_status stream_start_locked(struct udsp *s,
					    struct udsp_stream *st)
{
	enum udsp_status rc;

	switch (atomic_load_explicit(&st->state, memory_order_relaxed)) {
	case STREAM_IDLE:
		return UDSP_UNSUPPORTED;
	case STREAM_RUNNING:
		return UDSP_OK;
	default:
		break;
	}
	rc = stream_activate(s, st);
	if (rc == UDSP_OK)
		atomic_store_explicit(&st->state, STREAM_RUNNING,
				      memory_order_release);

	return rc;
}

enum udsp_status stream_start(struct udsp *s, unsigned line, unsigned stream)
{
	struct udsp_stream *st;
	enum udsp_status rc;

	rc = stream_guard(s, line, stream, false, &st);
	if (rc != UDSP_OK)
		return rc;

	pthread_mutex_lock(&s->ctl_lock);
	rc = stream_dead(st) ? UDSP_FATAL : stream_start_locked(s, st);
	pthread_mutex_unlock(&s->ctl_lock);

	return rc;
}

static enum udsp_status stream_stop_locked(struct udsp *s,
					   struct udsp_stream *st)
{
	unsigned state = atomic_load_explicit(&st->state, memory_order_relaxed);

	if (state == STREAM_IDLE)
		return UDSP_UNSUPPORTED;
	/* A quiesce that did not complete leaves the engine or the net thread
	 * possibly still inside the SRTP session, the codec instances and the
	 * descriptors, so the teardown below must not run. */
	if (state == STREAM_RUNNING && stream_quiesce(s, st, true) != UDSP_OK)
		return stream_dead_mark(s, st);
	stream_teardown(s, st);

	return UDSP_OK;
}

enum udsp_status stream_stop(struct udsp *s, unsigned line, unsigned stream)
{
	struct udsp_stream *st;
	enum udsp_status rc;

	rc = stream_guard(s, line, stream, true, &st);
	if (rc != UDSP_OK)
		return rc;

	pthread_mutex_lock(&s->ctl_lock);
	/* A mark set while this call waited for the lock is exactly the
	 * case the teardown must not run in. */
	rc = stream_dead(st) ? UDSP_FATAL : stream_stop_locked(s, st);
	pthread_mutex_unlock(&s->ctl_lock);

	return rc;
}

static enum udsp_status stream_dtmf_locked(struct udsp *s,
					   struct udsp_stream *st, char digit,
					   uint32_t duration_ms)
{
	/* The description decides it (stream_relay_allows). This way in stays
	 * open under UDSP_FEAT_DTMF_RELAY: it carries the digits the line
	 * never heard, a pulse digit among them. A digit the detector
	 * validates pre-empts an event it started. */
	if (atomic_load_explicit(&st->state, memory_order_relaxed) !=
	    STREAM_RUNNING || !stream_relay_allows(st))
		return UDSP_UNSUPPORTED;
	if (!duration_ms)
		duration_ms = UDSP_TELEV_DEFAULT_MS;

	return stream_cmd_post(s, st, STREAM_OP_DTMF, 0, duration_ms,
			       (int32_t)(unsigned char)digit);
}

enum udsp_status stream_dtmf(struct udsp *s, unsigned line, unsigned stream,
			     char digit, uint32_t duration_ms)
{
	struct udsp_stream *st;
	enum udsp_status rc;

	rc = stream_guard(s, line, stream, false, &st);
	if (rc != UDSP_OK)
		return rc;

	pthread_mutex_lock(&s->ctl_lock);
	rc = stream_dead(st) ? UDSP_FATAL :
	     stream_dtmf_locked(s, st, digit, duration_ms);
	pthread_mutex_unlock(&s->ctl_lock);

	return rc;
}

static enum udsp_status stream_stats_lookup(struct udsp *s, unsigned line,
					    unsigned stream, const void *out,
					    struct udsp_stream **result)
{
	struct udsp_stream *st;

	if (!s || !out || line >= s->cap.lines)
		return UDSP_UNSUPPORTED;
	st = stream_at(s, line, stream);
	if (!st)
		return UDSP_UNSUPPORTED;
	/* No lock: the dead mark must outrank the lifecycle state. */
	if (stream_dead(st))
		return UDSP_FATAL;
	if (atomic_load_explicit(&st->state, memory_order_acquire) ==
	    STREAM_IDLE)
		return UDSP_UNSUPPORTED;
	*result = st;

	return UDSP_OK;
}

/* FASTPATH: the engine republishes the media snapshot at
 * UDSP_STATS_PERIOD_MS. This copies one whole consistent slot. */
enum udsp_status stream_stats(struct udsp *s, unsigned line, unsigned stream,
			      struct udsp_media_stats *out)
{
	struct udsp_stream *st;
	enum udsp_status rc;

	if (out)
		memset(out, 0, sizeof(*out));
	rc = stream_stats_lookup(s, line, stream, out, &st);
	if (rc != UDSP_OK)
		return rc;
	media_snap_get(&st->snap, out);

	return UDSP_OK;
}

/* FASTPATH, the same shape as stream_stats: the engine publishes the jitter
 * buffer counters into their own double buffer every tick. */
enum udsp_status stream_jb_stats(struct udsp *s, unsigned line, unsigned stream,
				 struct udsp_jb_stats *out)
{
	struct udsp_stream *st;
	enum udsp_status rc;

	if (out)
		memset(out, 0, sizeof(*out));
	rc = stream_stats_lookup(s, line, stream, out, &st);
	if (rc != UDSP_OK)
		return rc;
	/* No tick of this configuration has published yet, so the slot still
	 * holds the previous call. The answer is the same UDSP_UNSUPPORTED an
	 * idle stream gives, which a caller already skips. */
	if (!atomic_load_explicit(&st->jb_snap_valid, memory_order_acquire))
		return UDSP_UNSUPPORTED;
	jb_snap_get(&st->jb_snap, out);

	return UDSP_OK;
}

/* FASTPATH: relaxed counter loads need no packet-thread acknowledgement. */
enum udsp_status stream_diag(struct udsp *s, unsigned line, unsigned stream,
			     struct udsp_stream_diag *out)
{
	struct udsp_stream *st;
	enum udsp_status rc;

	if (out)
		memset(out, 0, sizeof(*out));
	rc = stream_stats_lookup(s, line, stream, out, &st);
	if (rc != UDSP_OK)
		return rc;

	out->net_drops = atomic_load_explicit(&st->net_drops,
					     memory_order_relaxed);
	out->net_stale = atomic_load_explicit(&st->net_stale,
					     memory_order_relaxed);
	out->net_trunc = atomic_load_explicit(&st->net_trunc,
					     memory_order_relaxed);
	out->tx_fail = atomic_load_explicit(&st->tx_eagain,
					   memory_order_relaxed);
	out->punch_sent = atomic_load_explicit(&st->punch_sent,
					      memory_order_relaxed);
	out->srtp_protect_ok = atomic_load_explicit(
		&st->srtp.stats.protect_ok, memory_order_relaxed);
	out->srtp_protect_fail = atomic_load_explicit(
		&st->srtp.stats.protect_errors, memory_order_relaxed);
	out->srtp_unprotect_ok = atomic_load_explicit(
		&st->srtp.stats.unprotect_ok, memory_order_relaxed);
	out->srtp_unprotect_auth_fail = atomic_load_explicit(
		&st->srtp.stats.unprotect_auth_fail, memory_order_relaxed);
	out->srtp_unprotect_replay = atomic_load_explicit(
		&st->srtp.stats.unprotect_replay, memory_order_relaxed);

	return UDSP_OK;
}

/* FASTPATH, no lock: one relaxed load of the value the producers read at the
 * tap (udsp.h udsp_stream_gen). A stream the pool does not hold, and one that
 * never took a description, both read 0. */
uint32_t stream_gen(struct udsp *s, unsigned line, unsigned stream)
{
	struct udsp_stream *st = stream_at(s, line, stream);

	if (!st)
		return 0;

	return (uint32_t)atomic_load_explicit(&st->gen, memory_order_relaxed);
}

/* --- the fax profile and the conference ---------------------------------- */

static void stream_fax_save(struct stream_fax_saved *out,
			    const struct udsp_media *m)
{
	out->armed = true;
	out->vad = m->vad;
	out->cng = m->cng;
	out->dtx = m->dtx;
	out->passthrough = m->passthrough;
	out->codec = m->codec;
	out->codec_pt = m->codec_pt;
	out->jb_mode = m->jb_mode;
}

/*
 * The description the profile asks for. It starts from one media_prepare
 * already accepted, so only the two values the caller supplies are checked
 * again. The six fields of the passthrough profile are the whole
 * displacement; everything else, the descriptors and the key material
 * included, stays untouched.
 */
static int stream_fax_media(const struct udsp_stream *st,
			    enum udsp_fax_mode mode, uint8_t codec,
			    uint8_t codec_pt, struct udsp_media *out)
{
	*out = st->media;
	if (mode == UDSP_FAX_OFF) {
		out->codec = st->fax.codec;
		out->codec_pt = st->fax.codec_pt;
		out->jb_mode = st->fax.jb_mode;
		out->vad = st->fax.vad;
		out->cng = st->fax.cng;
		out->dtx = st->fax.dtx;
		out->passthrough = st->fax.passthrough;
	} else {
		out->codec = codec;
		out->codec_pt = codec_pt;
		/* A modem carrier has no talkspurt, so a voice detector
		 * clips its training sequences and comfort noise replaces
		 * them; and an adaptive buffer moves the very samples the far
		 * end demodulates, so the playout is fixed for the duration. */
		out->jb_mode = UDSP_JB_FIXED;
		out->vad = false;
		out->cng = false;
		/* The profile bypasses media_prepare, so the clear that a
		 * passthrough description gets there happens here instead: a
		 * carrier survives an untransmitted frame no better than it
		 * survives comfort noise. */
		out->dtx = false;
		/* The description carries the passthrough from here on, so a
		 * later udsp_stream_config that keeps the kind keeps the
		 * canceller policy, the held detectors and the silent
		 * concealment with it (udsp.h udsp_media.passthrough). The
		 * kind is the modem one: a CED answer says a data device
		 * answered and nothing yet says which, and the modem policy is
		 * the safe reading of either. The fax kind reaches the library
		 * on a description, after the caller heard the V.21 preamble.
		 */
		out->passthrough = UDSP_PASSTHROUGH_MODEM;
	}
	/* One check covers both: the caller's two values are new, and the
	 * restored pair is only as trustworthy as the record it came from. */
	if (out->codec_pt > 127u || !codec_find(out->codec))
		return -EINVAL;

	return 0;
}

/*
 * The transactional shape of stream_config with no descriptor and no key
 * material in it: the codec pair is the one fallible resource and it is built
 * aside, and one command boundary publishes the new description and everything
 * the engine derives from it, the passthrough kind included, so no tick ever
 * sees one without the other.
 *
 * A profile that moves the RTP clock ends the source, for the reason
 * stream_clock_moved states: the profile takes the codec the caller names,
 * G.711 from uvoice, and a stream that ran Opus counted every timeline in
 * 48000. The question is asked before the
 * install, because codec_pair_install overwrites the ops the stream ran at.
 */
static enum udsp_status stream_fax_apply(struct udsp *s, struct udsp_stream *st,
					 const struct udsp_media *m,
					 const struct rtp_entropy *entropy)
{
	const struct udsp_codec_ops *ops = codec_find(m->codec);
	struct udsp_codec_params params;
	struct codec_pair pair;
	enum udsp_status rc;
	bool adopt;

	adopt = stream_clock_moved(st, m, ops);
	if (stream_quiesce(s, st, false) != UDSP_OK)
		return stream_dead_mark(s, st);
	codec_params_build(s, m, ops, &params);
	if (codec_pair_create(st, ops, &params, &pair))
		return stream_prep_fail(s, st, true, -ENOMEM);

	codec_pair_install(st, &pair);
	st->media = *m;
	st->entropy = *entropy;
	rc = stream_cmd_post(s, st,
			     adopt ? STREAM_OP_ADOPT : STREAM_OP_RECONFIG,
			     0, 0, 0);
	if (rc == UDSP_OK)
		rc = stream_activate(s, st);
	/* The description is installed already, so a stream that cannot be put
	 * back takes the dead mark, exactly as a failed reconfiguration does. */
	if (rc != UDSP_OK)
		return stream_dead_mark(s, st);

	return UDSP_OK;
}

static enum udsp_status stream_fax_profile_locked(struct udsp *s,
						  struct udsp_stream *st,
						  enum udsp_fax_mode mode,
						  uint8_t codec,
						  uint8_t codec_pt,
						  const struct rtp_entropy *e)
{
	struct stream_fax_saved want;
	struct udsp_media m;
	enum udsp_status rc;

	if (atomic_load_explicit(&st->state, memory_order_relaxed) !=
	    STREAM_RUNNING)
		return UDSP_UNSUPPORTED;
	/* A profile on a T.38 stream would install a G.711 pair over the
	 * gateway and move the mode key with it, leaving the gateway with no
	 * owner. The profile belongs to the audio path alone. */
	if (media_t38(&st->media))
		return UDSP_UNSUPPORTED;
	/* Nothing was displaced, so there is nothing to put back. */
	if (mode == UDSP_FAX_OFF && !st->fax.armed)
		return UDSP_OK;
	if (stream_fax_media(st, mode, codec, codec_pt, &m))
		return UDSP_UNSUPPORTED;

	want = st->fax;
	if (mode == UDSP_FAX_OFF)
		memset(&want, 0, sizeof(want));
	else if (!want.armed)
		stream_fax_save(&want, &st->media);
	/* The record follows the description: a profile that did not apply left
	 * the old description running, so it must leave the old record too. */
	rc = stream_fax_apply(s, st, &m, e);
	if (rc == UDSP_OK)
		st->fax = want;

	return rc;
}

enum udsp_status stream_fax_profile(struct udsp *s, unsigned line,
				    unsigned stream, enum udsp_fax_mode mode,
				    uint8_t codec, uint8_t codec_pt)
{
	struct rtp_entropy entropy;
	struct udsp_stream *st;
	enum udsp_status rc;

	rc = stream_guard(s, line, stream, false, &st);
	if (rc != UDSP_OK)
		return rc;
	if (mode > UDSP_FAX_T38)
		return UDSP_UNSUPPORTED;
	/*
	 * T.38 is the stream-only path: the codec reaches libudsp as
	 * UDSP_SEL_T38 on udsp_stream_config and this op touches no channel
	 * profile for it. The mode is refused whatever the capability says,
	 * because the profile body (stream_fax_profile_locked) validates the
	 * codec the caller passed with it and would otherwise arm a G.711
	 * passthrough for a T.38 request.
	 * The refusal comes before anything is displaced.
	 */
	if (mode == UDSP_FAX_T38)
		return UDSP_UNSUPPORTED;
	/* Outside the lock, as stream_config draws it: getrandom may block,
	 * and a profile that moves the RTP clock starts its new source from
	 * these three draws. */
	if (entropy_fill(&entropy))
		return UDSP_TRANSIENT;

	pthread_mutex_lock(&s->ctl_lock);
	rc = stream_dead(st) ? UDSP_FATAL :
	     stream_fax_profile_locked(s, st, mode, codec, codec_pt, &entropy);
	pthread_mutex_unlock(&s->ctl_lock);

	return rc;
}

enum udsp_status stream_conf(struct udsp *s, unsigned line,
			     enum udsp_conf_mode mode)
{
	enum udsp_status rc = udsp_guard(s, line, false);

	if (rc != UDSP_OK)
		return rc;
	if (mode > UDSP_CONF_LOCAL_MIX)
		return UDSP_UNSUPPORTED;

	pthread_mutex_lock(&s->ctl_lock);
	/* The mix is a property of the line's media plane, and that plane
	 * exists only while a stream is configured on it: with none there is
	 * nothing to mix and nothing to turn off, which is the same
	 * UDSP_UNSUPPORTED an idle stream gets from stream_start. No resource
	 * changes hands here, so an expired ack needs no dead mark: the stale
	 * token discards the command and the retry UDSP_TRANSIENT invites arms
	 * the same mode again. */
	rc = stream_line_used(s, line, NULL) ?
	     stream_line_cmd_post(s, line, STREAM_OP_CONF, (uint32_t)mode) :
	     UDSP_UNSUPPORTED;
	pthread_mutex_unlock(&s->ctl_lock);

	return rc;
}

/* Whole ticks, rounded up, and never zero: the read of a tick runs after its
 * own write, so a delay of no frames would play the frame being captured
 * (chan.h UDSP_LOOP_SLOTS). */
static uint32_t stream_loop_frames(uint32_t delay_ms)
{
	uint32_t frames = (delay_ms + UDSP_TICK_MS - 1u) / UDSP_TICK_MS;

	return frames ? frames : 1u;
}

/*
 * No stream_line_used gate, unlike stream_conf: the loop is the line's own
 * audio and needs no media plane. The engine writes the arm record itself, so
 * a stop that disarms the line on the tick cannot leave a record behind
 * (udsp_priv.h loop_armed). The post waits for the ack, so the record stands
 * by the time this returns.
 */
enum udsp_status stream_line_loop(struct udsp *s, unsigned line,
				  enum udsp_loop_mode mode, unsigned peer,
				  uint32_t delay_ms)
{
	uint32_t frames = stream_loop_frames(delay_ms);
	enum udsp_status rc;
	uint32_t arg1;

	/* Only PEER reads the peer, so every other mode packs a zero: an
	 * argument the mode ignores must not reach the field beside it. Each
	 * field is masked to its own width for the same reason. */
	if (mode != UDSP_LOOP_PEER)
		peer = 0;
	arg1 = ((uint32_t)mode & 0xffu) | (((uint32_t)peer & 0xffu) << 8) |
	       ((frames & 0xffffu) << 16);

	pthread_mutex_lock(&s->ctl_lock);
	rc = stream_line_cmd_post(s, line, STREAM_OP_LOOP, arg1);
	pthread_mutex_unlock(&s->ctl_lock);

	return rc;
}

/* --- engine side: clocks ------------------------------------------------- */

static uint32_t mono_16_16(uint64_t ns)
{
	uint64_t sec = ns / 1000000000ull;
	uint64_t rem = ns % 1000000000ull;

	return (uint32_t)((sec << 16) | ((rem << 16) / 1000000000ull));
}

static uint32_t ntp_now(uint32_t *msw, uint32_t *lsw)
{
	struct timespec ts;
	uint32_t hi, lo;

	clock_gettime(CLOCK_REALTIME, &ts);
	hi = (uint32_t)ts.tv_sec + 2208988800u;
	lo = (uint32_t)(((uint64_t)ts.tv_nsec << 32) / 1000000000ull);
	if (msw)
		*msw = hi;
	if (lsw)
		*lsw = lo;

	return (hi << 16) | (lo >> 16);
}

/* --- engine side: helpers ------------------------------------------------ */

static void stream_event(struct udsp *s, const struct udsp_stream *st,
			 struct udsp_event *ev, uint32_t now_ms)
{
	ev->line = st->line;
	ev->stream = st->index;
	ev->stamp_ms = now_ms;
	udsp_emit(&s->emitter, UDSP_EV_PROD_ENGINE, ev);
}

/*
 * The end of a T.38 session. hal.h and the backend's own table read state 0
 * as the disconnect, and that is the only state this library produces: UDPTL
 * carries no payload type, so the payload-type change has no producer and
 * nothing may borrow its number. The two edges that end a session are the
 * stop and the move back to audio.
 */
static void stream_t38_event(struct udsp *s, struct udsp_stream *st,
			     uint32_t now_ms)
{
	struct udsp_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = UDSP_EV_T38;
	stream_event(s, st, &ev, now_ms);
}

static void stream_mark(struct udsp *s, struct udsp_stream *st, bool tx,
			uint32_t now_ms)
{
	struct udsp_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = tx ? UDSP_EV_RTP_FIRST_TX : UDSP_EV_RTP_FIRST_RX;
	stream_event(s, st, &ev, now_ms);
}

/*
 * One transmit record of the plaintext packet trace (udsp.h). stream_pkt_send
 * calls it before stream_protect, so an SRTP stream is taped in the clear, and
 * the UDPTL send calls it before its send. The record is therefore a send
 * attempt, and the protect or the send after it can still fail. The empty
 * datagram of stream_punch is not media and is not taped.
 *
 * The tick carries no debug mask of its own on the transmit half, so the gate
 * is read here: the ring pointer first, which is NULL for every run that never
 * armed the trace, and the mask only behind it.
 */
static void stream_tap_tx(struct udsp *s, const struct udsp_stream *st,
			  const void *buf, size_t len, unsigned kind)
{
	struct pkt_trace_ring *r;
	struct pkt_trace_meta m;

	/* The test hook of stream_tx_media fabricates a stream with no handle
	 * behind it (stream.h). */
	if (!s)
		return;
	r = atomic_load_explicit(&s->pkt_tx, memory_order_acquire);
	if (!r || !(atomic_load_explicit(&s->debug, memory_order_relaxed) &
		    UDSP_DEBUG_PKT_TRACE))
		return;

	/* The stamp the tick already read at the top of the period (core/chan.c
	 * udsp_engine_hook), so the tap reads no clock of its own and both
	 * directions carry one CLOCK_MONOTONIC base. */
	m.stamp_ns = s->tick_ns;
	m.gen = (uint32_t)atomic_load_explicit(&st->gen, memory_order_relaxed);
	m.line = st->line;
	m.stream = st->index;
	m.dir = UDSP_PKT_TX;
	m.kind = (uint8_t)kind;
	pkt_trace_push(r, &m, buf, len);
}

static bool stream_protect(struct udsp_stream *st, size_t *len, bool rtcp)
{
	size_t out = 0;
	int rc;

	if (!atomic_load_explicit(&st->srtp_armed, memory_order_acquire))
		return true;

	rc = rtcp ? udsp_srtp_protect_rtcp(&st->srtp, st->pkt, *len,
					   sizeof(st->pkt), &out) :
		    udsp_srtp_protect(&st->srtp, st->pkt, *len, sizeof(st->pkt),
				      &out);
	if (rc)
		return false;
	*len = out;

	return true;
}

/* Never blocks: the tick owns this thread and a stalled socket must cost one
 * counter, not a period. */
static void stream_send(struct udsp *s, struct udsp_stream *st, int fd,
			size_t len, bool rtp, uint32_t now_ms)
{
	if (fd < 0)
		return;
	if (send(fd, st->pkt, len, MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)len) {
		atomic_fetch_add_explicit(&st->tx_eagain, 1u,
					  memory_order_relaxed);
		return;
	}
	/* The stamp follows the descriptor and not the rtp flag: UDPTL leaves
	 * on rtp_fd with the flag clear and holds the tuple as RTP does. */
	if (fd == st->rtp_fd)
		st->rtp_tx_ms = now_ms;
	else if (fd == st->rtcp_fd)
		st->rtcp_tx_ms = now_ms;
	if (!rtp || st->tx_seen)
		return;
	st->tx_seen = true;
	stream_mark(s, st, true, now_ms);
}

static void stream_pkt_send(struct udsp *s, struct udsp_stream *st, size_t len,
			    bool rtcp, uint32_t now_ms)
{
	stream_tap_tx(s, st, st->pkt, len, rtcp ? UDSP_PKT_RTCP : UDSP_PKT_RTP);
	if (!stream_protect(st, &len, rtcp))
		return;
	stream_send(s, st, rtcp ? st->rtcp_fd : st->rtp_fd, len, !rtcp, now_ms);
}

/*
 * The empty datagram that holds an idle tuple open (RFC 6263 section 4.1).
 * Inbound media is admitted by connection tracking alone, and the entry exists
 * only after this box sent on the tuple, so a descriptor nothing else writes
 * to (a recvonly stream, a T.38 session before its first packet, the RTCP
 * socket before its first report) gets one of these every punch_interval_ms.
 * A receiver discards it on the RFC 3550 minimum length before any SRTP or
 * payload type logic runs. The zero length RTP packet of section 4.6 is not
 * used: it would put unprotected RTP shaped bytes into an SRTP session
 * and cost a sequence number. Not through stream_send: a punch is not media,
 * so it never sets tx_seen, never emits UDSP_EV_RTP_FIRST_TX and never passes
 * stream_protect. A failed send keeps the stamp, so the next tick tries again.
 */
static void stream_punch(struct udsp_stream *st, int fd, uint32_t *stamp,
			 uint32_t now_ms)
{
	if (fd < 0)
		return;
	if (send(fd, st->pkt, 0, MSG_DONTWAIT | MSG_NOSIGNAL) != 0) {
		atomic_fetch_add_explicit(&st->tx_eagain, 1u,
					  memory_order_relaxed);
		return;
	}
	*stamp = now_ms;
	atomic_fetch_add_explicit(&st->punch_sent, 1u, memory_order_relaxed);
}

/* --- engine side: receive ------------------------------------------------ */

/*
 * The type 2 acknowledgement is the local CPE's own DTMF and reaches the
 * sequencer from the UL detector in chan.c only: the far end cannot acknowledge
 * alerting it never heard, so a wire digit is reported and nothing more.
 */
static void stream_digit_report(struct udsp *s, struct udsp_stream *st,
				const struct telev_event *tev, uint32_t now_ms)
{
	struct udsp_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = UDSP_EV_DIGIT;
	ev.u.digit.digit = tev->digit;
	ev.u.digit.remote = 1;
	ev.u.digit.duration_ms = tev->duration_ms;
	stream_event(s, st, &ev, now_ms);
}

static void stream_rtcp_receive(struct udsp_stream *st,
				const struct net_slot *slot)
{
	struct rtcp_parse_result res;

	if (rtcp_compound_parse(slot->data, slot->len, st->sender.ssrc, &res))
		return;
	/* rtcp_state_receive feeds rtp_stats_rtcp_report itself when the report
	 * names our SSRC (rtcp.c), so calling it again would double count the
	 * round trip. */
	rtcp_state_receive(&st->rtcp, &res, mono_16_16(slot->arrival_ns),
			   ntp_now(NULL, NULL));
}

/*
 * The trace producer. Everything below runs only while
 * UDSP_DEBUG_JB_TRACE is set in the mask the tick loaded once: with the bit
 * clear the receive path reads no jitter buffer field, takes no statistics
 * copy and touches no ring, which is what keeps the diagnostic free.
 *
 * jb.h gives the engine the buffer object, so the transition flags are read
 * from it directly; the depth is not a field but a computation private to
 * rtp/jb.c, so the one statistics copy a record needs is the only route to it.
 */
struct stream_jb_mark {
	uint32_t playout_ts;
	uint64_t resyncs;
	uint64_t starve_resyncs;
	uint64_t cn_reanchors;
	uint64_t underruns;
	uint8_t state;
};

static void stream_jb_mark_take(const struct udsp_stream *st,
				struct stream_jb_mark *m)
{
	m->playout_ts = st->jb.playout_ts;
	m->resyncs = st->jb.total.resyncs;
	m->starve_resyncs = st->jb.total.starve_resyncs;
	m->cn_reanchors = st->jb.total.cn_reanchors;
	m->underruns = st->jb.total.underruns;
	m->state = st->jb.state;
}

static bool stream_jb_recovered(const struct udsp_stream *st,
				const struct stream_jb_mark *m)
{
	return st->jb.total.resyncs != m->resyncs ||
	       st->jb.total.starve_resyncs != m->starve_resyncs ||
	       st->jb.total.cn_reanchors != m->cn_reanchors;
}

/* A packet the buffer simply took changes nothing a reader could act on, so
 * only a rejection or a move of the timeline earns a record. */
static bool stream_jb_put_notable(const struct udsp_stream *st,
				  const struct stream_jb_mark *m, int rc)
{
	if (rc || st->jb.state != m->state)
		return true;

	return st->jb.playout_ts != m->playout_ts ||
	       stream_jb_recovered(st, m);
}

static uint16_t stream_jb_put_flags(const struct udsp_stream *st,
				    const struct stream_jb_mark *m)
{
	uint16_t flags = 0;

	if (st->jb.total.resyncs != m->resyncs)
		flags |= UDSP_JB_TRACE_RESYNC | UDSP_JB_TRACE_ANCHOR;
	if (st->jb.total.starve_resyncs != m->starve_resyncs)
		flags |= UDSP_JB_TRACE_STARVE;
	if (st->jb.total.cn_reanchors != m->cn_reanchors)
		flags |= UDSP_JB_TRACE_CN_REANCHOR;
	if (m->state == STREAM_JB_IDLE && st->jb.state == STREAM_JB_PREFILL)
		flags |= UDSP_JB_TRACE_ANCHOR;
	/* A stream that goes back to prefill with no recovery counter behind it
	 * is the refill of jb.c: the queue ran dry and this packet said where
	 * the timeline continues. */
	if (m->state == STREAM_JB_PLAY && st->jb.state == STREAM_JB_PREFILL &&
	    !stream_jb_recovered(st, m))
		flags |= UDSP_JB_TRACE_REFILL;

	return flags;
}

static void stream_jb_trace_fill(const struct udsp_stream *st,
				 const struct stream_jb_mark *m,
				 const struct udsp_jb_stats *jb, uint8_t kind,
				 uint32_t now_ms, struct udsp_jb_trace *rec)
{
	memset(rec, 0, sizeof(*rec));
	rec->stamp_ms = now_ms;
	rec->playout_ts = m->playout_ts;
	rec->depth_ms = jb->depth_ms;
	rec->target_ms = jb->target_ms;
	rec->conceal_run_ms = st->jb.conceal_run_ms;
	rec->line = st->line;
	rec->stream = st->index;
	rec->kind = kind;
	rec->state_before = m->state;
	rec->state_after = st->jb.state;
	rec->count = st->jb.count;
	rec->skew_pending = st->jb.skew_pending;
}

static void stream_jb_trace_put(struct udsp *s, const struct udsp_stream *st,
				const struct stream_jb_mark *m,
				const struct udsp_jb_packet *pkt, int rc,
				uint32_t now_ms)
{
	struct udsp_jb_trace rec;
	struct udsp_jb_stats jb;

	if (!stream_jb_put_notable(st, m, rc))
		return;

	udsp_jb_stats(&st->jb, &jb);
	stream_jb_trace_fill(st, m, &jb, UDSP_JB_TRACE_PUT, now_ms, &rec);
	rec.pkt_ts = pkt->rtp_timestamp;
	rec.lateness_ts = (int32_t)(m->playout_ts - pkt->rtp_timestamp);
	rec.seq = pkt->seq;
	rec.flags = stream_jb_put_flags(st, m);
	rec.rc = (int8_t)rc;
	jb_trace_push(&s->trace, &rec);
}

/* The row level of a received event: its RFC 4733 volume as -dBm0. The pair
 * with its column UDSP_DTMF_TWIST_DB higher fits int16 only while the row is
 * at -4 dBm0 or lower (dsp.h), so a louder volume plays at -4. */
static int stream_telev_level_get(uint8_t volume)
{
	return volume < 4u ? -4 : -(int)volume;
}

/* One packet that the receiver accepted, into the event state or the
 * jitter buffer. */
static void stream_rtp_accept(struct udsp *s, struct udsp_stream *st,
			      const struct rtp_packet *pkt, uint32_t now_ms,
			      uint32_t debug, bool tail)
{
	struct stream_jb_mark mark;
	struct udsp_jb_packet jbp;
	struct telev_event tev;
	int rc;

	if (!st->rx_seen) {
		st->rx_seen = true;
		stream_mark(s, st, false, now_ms);
	}
	/* rtp.h: every accepted packet advances the event timeouts, which is
	 * how an all-ends-lost event still reports. */
	if (telev_packet_parse(&st->tev_rx, pkt, &tev) == 1)
		stream_digit_report(s, st, &tev, now_ms);
	if (pkt->kind == RTP_PAYLOAD_TELEPHONE_EVENT) {
		/* The digit plays at the volume the far end announced, and the
		 * receive gain of the port follows, as for in-band audio
		 * (core/chan.c chan_dl). */
		if (telev_play_packet(&st->tev_play, pkt) == 1)
			udsp_dtmf_generator_init(&st->tev_gen,
				st->tev_play.digit,
				stream_telev_level_get(st->tev_play.volume),
				st->line_frame * 100u);
		return;
	}

	memset(&jbp, 0, sizeof(jbp));
	jbp.payload = pkt->payload;
	jbp.len = pkt->payload_len;
	jbp.arrival_ns = pkt->arrival_ns;
	jbp.ssrc = pkt->ssrc;
	jbp.rtp_timestamp = pkt->timestamp;
	jbp.seq = pkt->sequence;
	jbp.kind = pkt->kind;
	/* jb.h Talkspurt: the sender's own word that the gap before this packet
	 * is a silence it suppressed, not audio the network lost. */
	jbp.marker = pkt->marker;
	/* jb.h tail: every packet after the first shares the datagram's
	 * arrival stamp, which is one arrival observation, not several. */
	jbp.tail = tail;
	if (!(debug & UDSP_DEBUG_JB_TRACE)) {
		udsp_jb_put(&st->jb, &jbp);
		return;
	}
	stream_jb_mark_take(st, &mark);
	rc = udsp_jb_put(&st->jb, &jbp);
	stream_jb_trace_put(s, st, &mark, &jbp, rc, now_ms);
}

/* One RTP packet out of the datagram. A non-zero return says the parse
 * refused it, which ends the walk over whatever follows it. */
static int stream_rtp_packet(struct udsp *s, struct udsp_stream *st,
			     const uint8_t *data, size_t len,
			     uint64_t arrival_ns, uint32_t now_ms,
			     uint32_t debug, bool tail, struct rtp_packet *pkt)
{
	bool source_valid = st->receiver.source_valid;
	uint32_t ssrc = st->receiver.ssrc;
	struct rtp_packet held;
	int rc;

	rc = rtp_packet_parse(&st->receiver, data, len, arrival_ns, pkt);
	/*
	 * A new source is a new timeline. rtp.c re-anchors
	 * the receiver on it and the jitter buffer resyncs on it, and the
	 * telephone-event state is scoped the same way: its reported-event
	 * ring, its open chain and the identities of the chains it already
	 * reported are all keyed on an event code and a timestamp, which the
	 * new source is free to reuse for an event of its own.
	 *
	 * The receiver's own record is what says the source moved, not the
	 * parsed packet: rtp.c adopts the new source before the RFC 3550 A.1
	 * probation, so the packet that carries the change is held on
	 * probation, and the first packet out of the receiver already reads
	 * as the new source.
	 */
	if (!source_valid || ssrc != st->receiver.ssrc) {
		telev_receiver_init_cfg(&st->tev_rx, st->ops->clock_rate);
		telev_play_init(&st->tev_play);
	}
	/* The packet that proved the source releases its held predecessor,
	 * which plays first, even when this packet's own type is refused. */
	if (rtp_receiver_held_take(&st->receiver, &held))
		stream_rtp_accept(s, st, &held, now_ms, debug, false);
	if (rc)
		return rc;
	stream_rtp_accept(s, st, pkt, now_ms, debug, tail);
	return 0;
}

/*
 * One datagram, one or more packets (RFC 3550 11). The field far
 * end delivers two 20 ms packets in one UDP datagram, so rtp.c hands the rest
 * of the PDU back on pkt.tail and the walk continues over it. The bound is
 * RTP_PDU_PACKETS_MAX: a crafted datagram must not turn one tick into an
 * unbounded run of puts. A refused packet ends the walk, because the parse
 * reports a tail only for a packet it accepted.
 */
static void stream_rtp_receive(struct udsp *s, struct udsp_stream *st,
			       const struct net_slot *slot, uint32_t now_ms,
			       uint32_t debug)
{
	const uint8_t *data = slot->data;
	size_t len = slot->len;
	struct rtp_packet pkt;
	unsigned n;

	for (n = 0; n < RTP_PDU_PACKETS_MAX && len; n++) {
		if (stream_rtp_packet(s, st, data, len, slot->arrival_ns,
				      now_ms, debug, n > 0, &pkt))
			return;
		data = pkt.tail;
		len = pkt.tail_len;
	}
	/* The bound stopped the walk with packets still in the
	 * datagram. They reached this box and this box dropped them, so they
	 * are counted as discards here; silent, they would come back as loss
	 * against a far end that sent them perfectly well. */
	if (len)
		rtp_receiver_pdu_residual_discard(&st->receiver, data, len);
}

/*
 * One UDPTL datagram into the gateway. A T.38 stream runs no report session,
 * so a datagram that arrived on the control descriptor belongs to nothing
 * here; so does one the description refuses to receive. Both are counted
 * rather than parsed.
 */
static void stream_udptl_receive(struct udsp_stream *st,
				 const struct net_slot *slot)
{
	if (slot->kind == NET_KIND_RTCP || !media_receives(&st->media)) {
		st->t38_dropped++;
		return;
	}
	if (st->fax_gate.ops->udptl_in(st->fax_gate.inst, slot->data,
				       slot->len)) {
		st->t38_errors++;
		return;
	}
	st->t38_recv++;
	st->t38_bytes_recv += slot->len;
}

/*
 * At most UDSP_NET_DRAIN_MAX slots reach the parser per tick and a burst below
 * that keeps its place in the ring: the jitter buffer is what absorbs a burst,
 * not this bound. A ring the net thread found full is different: datagrams
 * were already refused, so the backlog is shed from the oldest end, which is
 * the audio whose playout instant has passed anyway.
 */
static void stream_ring_drain(struct udsp *s, struct udsp_stream *st,
			      uint32_t now_ms, uint32_t debug)
{
	unsigned head = atomic_load_explicit(&st->ring.head, memory_order_acquire);
	unsigned tail = atomic_load_explicit(&st->ring.tail, memory_order_relaxed);
	unsigned queued = head - tail;
	unsigned end;

	if (queued >= UDSP_NET_SLOTS) {
		atomic_fetch_add_explicit(&st->net_stale,
					  queued - UDSP_NET_DRAIN_MAX,
					  memory_order_relaxed);
		tail = head - UDSP_NET_DRAIN_MAX;
		queued = UDSP_NET_DRAIN_MAX;
	}
	end = tail + (queued < UDSP_NET_DRAIN_MAX ? queued : UDSP_NET_DRAIN_MAX);
	while (tail != end) {
		const struct net_slot *slot =
			&st->ring.slot[tail % UDSP_NET_SLOTS];

		tail++;
		if (!slot->len)
			continue;
		if (st->mode == STREAM_MODE_T38)
			stream_udptl_receive(st, slot);
		else if (slot->kind == NET_KIND_RTCP)
			stream_rtcp_receive(st, slot);
		else
			stream_rtp_receive(s, st, slot, now_ms, debug);
	}
	atomic_store_explicit(&st->ring.tail, tail, memory_order_release);
}

/*
 * The drain of a skip (stream_rx_skip): every slot of the ring snapshot whose
 * arrival stamp is at or before cutoff_ns, the skip hook's own stamp. It
 * reads at most UDSP_NET_SLOTS slots, the whole ring, because the skip pays
 * its periods against the audio that arrived in them; the per-tick bound
 * would leave the rest of a burst to a tick after the debt is paid. The net
 * thread stamps each slot before it publishes the batch, and in ring order,
 * so a later stamp ends the walk: that slot and the ones after it wait for
 * the ordinary drain. A full ring sheds nothing here. The net thread counted
 * every datagram it refused in net_drops, and the skip passes the oldest
 * audio anyway.
 */
static void stream_ring_skip_drain(struct udsp *s, struct udsp_stream *st,
				   uint64_t cutoff_ns, uint32_t now_ms,
				   uint32_t debug)
{
	unsigned head = atomic_load_explicit(&st->ring.head,
					     memory_order_acquire);
	unsigned tail = atomic_load_explicit(&st->ring.tail,
					     memory_order_relaxed);

	while (tail != head) {
		const struct net_slot *slot =
			&st->ring.slot[tail % UDSP_NET_SLOTS];

		if (slot->arrival_ns > cutoff_ns)
			break;
		tail++;
		if (!slot->len)
			continue;
		if (slot->kind == NET_KIND_RTCP)
			stream_rtcp_receive(st, slot);
		else
			stream_rtp_receive(s, st, slot, now_ms, debug);
	}
	atomic_store_explicit(&st->ring.tail, tail, memory_order_release);
}

/* The one 2:1 step of the rate conversion. dsp/resamp.h fixes the ratio, so
 * the two counts decide the direction and nothing else does; equal counts are
 * a matched rate, which copies. */
static void stream_resample_run(struct udsp_resamp *rs, const int16_t *in,
			    unsigned n_in, int16_t *out, unsigned n_out)
{
	if (n_out == n_in)
		memcpy(out, in, n_out * sizeof(*out));
	else if (n_out < n_in)
		udsp_resamp_down2(rs, in, n_in, out);
	else
		udsp_resamp_up2(rs, in, n_in, out);
}

/* What the codec plays when there is no frame to decode. External for the
 * test hook alone (stream.h): no caller outside this file and test_media
 * exists. */
void stream_conceal_unit(struct udsp_stream *st,
			 const struct udsp_jb_out *out, int16_t *pcm,
			 unsigned n)
{
	const struct udsp_codec_ops *ops = st->ops;

	/*
	 * A carrier has neither a pitch to repeat nor a noise level to
	 * describe. G.711 App I repeats the pitch of the last speech it heard
	 * for up to 60 ms, and comfort noise synthesises a hiss; the far end
	 * demodulates both as data and reads a corrupted page instead of a
	 * dropout it can retrain out of. Every unit a passthrough description
	 * cannot decode is therefore silence, whatever the buffer decided: a
	 * failed decode, a comfort-noise packet, a concealed gap and a skew
	 * insert alike.
	 */
	if (st->passthrough_kind != UDSP_PASSTHROUGH_OFF) {
		memset(pcm, 0, n * sizeof(*pcm));
		return;
	}
	/* Comfort noise from a codec SID frame goes to the codec's own
	 * decode: a stored SID update carries the payload once, and len 0 is
	 * the continuation (codec.h). A short return is the pre-SID -EAGAIN,
	 * which falls to the plc ladder below; the codec routes CNG-time
	 * concealment internally, so the ladder stays safe. */
	if (out->decision == UDSP_JB_CN && out->cn_sid &&
	    ops->decode(st->dec, out->len ? out->payload : NULL, out->len,
			pcm, n) == (int)n)
		return;
	if (out->decision == UDSP_JB_CN && !out->cn_sid && ops->cn && out->len) {
		/* RFC 3389 carries the level as -dBov in the first octet. */
		ops->cn(st->dec, out->payload[0] & 0x7f, pcm, n);
		return;
	}
	/* The next packet stays NULL here: only a buffer lookahead can hand one
	 * over, and the buffer has none yet. codec.h makes the argument
	 * optional for exactly this reason, so a codec with UDSP_CODEC_F_FEC
	 * conceals natively until a lookahead supplies it. */
	if (out->decision != UDSP_JB_SILENCE && ops->plc &&
	    ops->plc(st->dec, pcm, n, NULL, 0) == (int)n)
		return;
	memset(pcm, 0, n * sizeof(*pcm));
}

/* One output unit at the codec's rate, whatever the jitter buffer decided. */
static void stream_decode_unit(struct udsp_stream *st,
			       const struct udsp_jb_out *out, int16_t *pcm,
			       unsigned n)
{
	if (out->decision == UDSP_JB_FRAME &&
	    st->ops->decode(st->dec, out->payload, out->len, pcm, n) == (int)n)
		return;
	stream_conceal_unit(st, out, pcm, n);
}

/*
 * The whole-packet unit (jb.h Units). The buffer hands the payload out once
 * per unit_ts and marks the slices after it, so the decode runs once and the
 * ticks of the unit play 10 ms slices of the staging. NULL says "nothing to
 * play", which the caller conceals.
 *
 * The codec measures the unit, not the buffer. codec.h makes one packet one
 * indivisible unit whose length only decode knows, and the buffer's slice
 * count is its playout grid: the two disagree wherever the span learner has no
 * pair yet, which UDSP_JB_OFF reaches on the very first packet, and wherever
 * the far end moves its packet duration without signalling it. Decoding
 * against the staging's own capacity therefore plays what the packet holds and
 * conceals only the slices past it, instead of throwing a whole good packet
 * away; the capacity argument is what keeps the codec inside the staging.
 */
static const int16_t *stream_unit_pcm(struct udsp_stream *st,
				      const struct udsp_jb_out *out)
{
	if (out->payload && out->len) {
		int got;

		st->rx_unit_valid = false;
		got = st->ops->decode(st->dec, out->payload, out->len,
				      st->rx_unit, UDSP_PCM_STAGE_MAX);
		if (got < (int)st->codec_frame)
			return NULL;
		st->rx_unit_ts = out->unit_ts;
		st->rx_unit_slices = (uint8_t)((unsigned)got / st->codec_frame);
		st->rx_unit_valid = true;
	}
	if (!st->rx_unit_valid || st->rx_unit_ts != out->unit_ts ||
	    out->slice >= st->rx_unit_slices)
		return NULL;

	return st->rx_unit + (unsigned)out->slice * st->codec_frame;
}

/* One decode per unit into the staging, one 10 ms slice of it per tick. Every
 * other decision conceals on rx_pcm, so the staging outlives it and the slices
 * after a concealed tick still find their audio. */
static void stream_decode_slices(struct udsp_stream *st,
				 const struct udsp_jb_out *out)
{
	const int16_t *pcm = NULL;

	if (out->decision == UDSP_JB_FRAME)
		pcm = stream_unit_pcm(st, out);
	if (!pcm) {
		stream_conceal_unit(st, out, st->rx_pcm, st->codec_frame);
		pcm = st->rx_pcm;
	}
	stream_resample_run(&st->rs_rx, pcm, st->codec_frame, st->dl,
			st->line_frame);
}

/*
 * Every output unit goes through the resampler, audio, comfort noise,
 * concealment and silence alike: the resampler carries its history from one
 * call to the next, so a unit that skipped it would leave the next one
 * filtering against samples from an older instant.
 */
static void stream_decode(struct udsp_stream *st, const struct udsp_jb_out *out)
{
	if (st->unit_packet) {
		stream_decode_slices(st, out);
		return;
	}
	if (!st->resample) {
		stream_decode_unit(st, out, st->dl, st->codec_frame);
		return;
	}
	stream_decode_unit(st, out, st->rx_pcm, st->codec_frame);
	stream_resample_run(&st->rs_rx, st->rx_pcm, st->codec_frame, st->dl,
			st->line_frame);
}

/* The caller takes one statistics copy per tick and hands it to the snapshot,
 * to this event and to the trace, so no path takes a second one. */
static void stream_jb_event(struct udsp *s, struct udsp_stream *st,
			    const struct udsp_jb_stats *now, uint32_t now_ms)
{
	struct udsp_event ev;

	if (!memcmp(now, &st->jb_last, sizeof(*now)))
		return;
	if ((int32_t)(now_ms - st->jb_event_ms) < (int32_t)UDSP_JB_EVENT_MIN_MS)
		return;
	st->jb_last = *now;
	st->jb_event_ms = now_ms;

	memset(&ev, 0, sizeof(ev));
	ev.type = UDSP_EV_JB_UPDATE;
	/* jb.h Event mapping: the five output counters partition the units. */
	ev.u.jb.total = now->frames_out + now->plc + now->cn + now->silence +
			now->skew_inserts;
	ev.u.jb.drop = now->late_drop + now->dup_drop + now->overflow_drop +
		       now->hold_drop;
	ev.u.jb.size_ms = now->target_ms;
	ev.u.jb.jitter_avg = now->jitter_ms;
	stream_event(s, st, &ev, now_ms);
}

static uint16_t stream_jb_out_flags(const struct udsp_jb_out *out)
{
	uint16_t flags = 0;

	if (out->skew_drop)
		flags |= UDSP_JB_TRACE_SKEW_DROP;
	if (out->skew_insert)
		flags |= UDSP_JB_TRACE_SKEW_INSERT;
	if (out->clock_parked)
		flags |= UDSP_JB_TRACE_PARKED;

	return flags;
}

/*
 * A GET record costs one entry per decision change, never one per tick: clean
 * playout repeats the same key for the whole call. An underrun and the
 * heartbeat of an unchanged concealment run are the two exceptions, so a long
 * outage still leaves a trail and a reader can tell it from a dead trace. The
 * decision fills bits 0 and 1, so the markers start at bit 2.
 */
static uint16_t stream_jb_get_key(const struct udsp_jb_out *out, uint8_t state)
{
	return (uint16_t)(out->decision | stream_jb_out_flags(out) << 2 |
			  (uint16_t)state << 8);
}

/*
 * The heartbeat sits inside a concealment run and nowhere else. jb.h
 * counts comfort noise the far end asked for as neither an underrun nor part
 * of the run, and the silence of a buffer that has not started yet is not
 * concealment either. Both repeat one tuple for as long as they last, so a
 * beat there would be noise. jb.c leaves conceal_run_ms at zero for both and
 * raises it for every concealed unit, which is the test.
 */
static bool stream_jb_concealing(const struct udsp_jb_out *out, uint16_t run_ms)
{
	if (out->decision != UDSP_JB_PLC && out->decision != UDSP_JB_SILENCE)
		return false;

	return run_ms != 0;
}

static uint16_t stream_jb_get_flags(const struct udsp_jb_out *out,
				    bool underrun, bool beat)
{
	uint16_t flags = stream_jb_out_flags(out);

	if (underrun)
		flags |= UDSP_JB_TRACE_UNDERRUN;
	if (beat)
		flags |= UDSP_JB_TRACE_HEARTBEAT;

	return flags;
}

static void stream_jb_trace_get(struct udsp *s, struct udsp_stream *st,
				const struct udsp_jb_out *out,
				const struct udsp_jb_stats *jb,
				const struct stream_jb_mark *m, uint32_t now_ms)
{
	uint16_t key = stream_jb_get_key(out, st->jb.state);
	bool underrun = st->jb.total.underruns != m->underruns;
	struct udsp_jb_trace rec;
	bool beat;

	beat = stream_jb_concealing(out, st->jb.conceal_run_ms) &&
	       (int32_t)(now_ms - st->jb_trace_ms) >=
	       (int32_t)UDSP_JB_TRACE_BEAT_MS;
	if (st->jb_trace_have && key == st->jb_trace_key && !underrun && !beat)
		return;
	st->jb_trace_have = true;
	st->jb_trace_key = key;
	st->jb_trace_ms = now_ms;

	stream_jb_trace_fill(st, m, jb, UDSP_JB_TRACE_GET, now_ms, &rec);
	rec.pkt_ts = out->rtp_timestamp;
	rec.flags = stream_jb_get_flags(out, underrun, beat);
	rec.rc = (int8_t)out->decision;
	jb_trace_push(&s->trace, &rec);
}

/*
 * The counters a UDPTL session has. There is no loss, no jitter and no round
 * trip to report, because no receiver ran, and the quality figures stay zero:
 * the E-model would read a clean call out of an empty record, and the jitter
 * conversion would divide by a clock this mode has none of.
 */
static void stream_t38_stats_fill(const struct udsp_stream *st,
				  struct udsp_media_stats *out)
{
	memset(out, 0, sizeof(*out));
	out->packets_sent = st->t38_sent;
	out->packets_recv = st->t38_recv;
	out->bytes_sent = st->t38_bytes_sent;
	out->bytes_recv = st->t38_bytes_recv;
	/* A datagram this box took in and threw away is a discard, which is
	 * what the field means on the audio path too: the direction refused
	 * it, or it arrived on the descriptor that carries no image data. */
	out->packets_discarded = st->t38_dropped;
}

static void stream_stats_publish(const struct udsp *s, struct udsp_stream *st)
{
	struct udsp_media_stats out;

	if (st->mode == STREAM_MODE_T38) {
		stream_t38_stats_fill(st, &out);
		media_snap_publish(&st->snap, &out);
		return;
	}

	/* The model delay of the E-model is the playout delay this stream
	 * actually runs at: the buffer target plus one packet plus the
	 * standing playback ring the engine keeps (udsp_priv.h prefill_ms).
	 * The ring is invisible to the buffer's own depth, so leaving it out
	 * would under-report the delay by the whole prefill. */
	rtp_stats_delay_set(&st->stats,
			    (uint32_t)st->jb.target_ms + st->media.ptime_ms +
			    s->prefill_ms);
	rtp_stats_snapshot(&st->stats, &st->receiver, &out);
	media_snap_publish(&st->snap, &out);
}

/* The E-model of rtp/stats.c costs a pow and a log per call, so the snapshot
 * is republished at a diagnostic cadence, 100 ms, instead of every tick. */
static void stream_stats_tick(const struct udsp *s, struct udsp_stream *st,
			      uint32_t now_ms)
{
	if ((int32_t)(now_ms - st->stats_next_ms) < 0)
		return;
	st->stats_next_ms = now_ms + UDSP_STATS_PERIOD_MS;
	stream_stats_publish(s, st);
}

/*
 * The receive half of the gateway: one 8 kHz frame toward the phone per tick,
 * converted to the line rate where the two differ. dsp/resamp.h needs
 * separate buffers for the interpolation, so the frame is staged first.
 */
static void stream_rx_gateway(struct udsp_stream *st)
{
	if (!st->resample) {
		st->fax_gate.ops->tx_pcm(st->fax_gate.inst, st->dl);
		return;
	}
	st->fax_gate.ops->tx_pcm(st->fax_gate.inst, st->rx_pcm);
	stream_resample_run(&st->rs_rx, st->rx_pcm, UDSP_FRAME_SAMPLES, st->dl,
			    st->line_frame);
}

/*
 * The event the far end sent, rendered toward the line. The generator runs at
 * the line rate and the tracker counts the stream's clock units, so the frame
 * length crosses between the two. The tracker advances whether the frame is
 * written or not, because the event runs on the far end's clock and a mute
 * that lifts mid-digit must find the render where the wire left it.
 *
 * The return says whether the line heard the digit, which is what holds the
 * uplink detector (core/chan.c chan_digit_hold_arm).
 */
static bool stream_rx_telev_play(struct udsp *s, struct udsp_stream *st,
				 bool mute, int16_t *frame, unsigned n)
{
	int16_t buf[UDSP_FRAME_MAX];
	unsigned count = n;
	uint32_t left;

	if (!telev_play_tick(&st->tev_play, s->tick_ns, &left))
		return false;
	if (left < st->ts_per_frame)
		count = left * st->line_frame / st->ts_per_frame;
	if (!count)
		return false;
	telev_play_advance(&st->tev_play,
			   count * st->ts_per_frame / st->line_frame);
	if (mute || !media_receives(&st->media))
		return false;
	udsp_dtmf_generator_render(&st->tev_gen, buf, count);
	frame_add_sat(frame, buf, count);

	return true;
}

/*
 * One statistics copy per tick per stream feeds the snapshot, the rate-limited
 * event and the trace. The event and the trace need the copy anyway, so
 * publishing every tick costs the double-buffer write and nothing more.
 */
static bool stream_rx_run(struct udsp *s, struct udsp_stream *st, bool mute,
			  int16_t *frame, unsigned n, uint32_t now_ms,
			  uint32_t debug)
{
	bool trace = (debug & UDSP_DEBUG_JB_TRACE) != 0;
	struct stream_jb_mark mark = { 0 };
	struct udsp_jb_stats jb;
	struct udsp_jb_out out;

	stream_ring_drain(s, st, now_ms, debug);
	/* No jitter buffer, no decode and no trace belongs to a UDPTL session:
	 * the gateway plays what T.30 gives it, tick by tick. */
	if (st->mode == STREAM_MODE_T38) {
		stream_rx_gateway(st);
		stream_stats_tick(s, st, now_ms);
		if (!mute && media_receives(&st->media))
			frame_add_sat(frame, st->dl, n);
		return false;
	}
	if (trace)
		stream_jb_mark_take(st, &mark);
	/* The render stands outside the decode: a tick the buffer gave no
	 * output on is still a tick of the digit the far end is sending. */
	if (udsp_jb_get(&st->jb, s->tick_ns, &out))
		return stream_rx_telev_play(s, st, mute, frame, n);
	stream_decode(st, &out);
	udsp_jb_stats(&st->jb, &jb);
	/* The flag follows the publish, never the attempt: a publish snap.h
	 * skipped for an active reader leaves the previous call's counters in
	 * the slot, and a reader must not be told they are this call's. */
	if (jb_snap_publish(&st->jb_snap, &jb))
		atomic_store_explicit(&st->jb_snap_valid, true,
				      memory_order_release);
	stream_jb_event(s, st, &jb, now_ms);
	if (trace)
		stream_jb_trace_get(s, st, &out, &jb, &mark, now_ms);
	stream_stats_tick(s, st, now_ms);
	/* Mute silences the far end in both directions; the local playout of
	 * chan.c is not muted. */
	if (!mute && media_receives(&st->media))
		frame_add_sat(frame, st->dl, n);

	return stream_rx_telev_play(s, st, mute, frame, n);
}

void stream_tick_rx(struct udsp *s, struct udsp_chan *c, int16_t *frame,
		    uint32_t now_ms)
{
	bool mute = atomic_load_explicit(&c->mute, memory_order_acquire);
	uint32_t debug = (uint32_t)atomic_load_explicit(&s->debug,
							memory_order_relaxed);
	struct udsp_stream *st;
	bool digit = false;

	/* The caller cleared the frame; this only sums into it. */
	stream_line_for_each(s, c->line, st) {
		if (!atomic_load_explicit(&st->active, memory_order_acquire))
			continue;
		digit |= stream_rx_run(s, st, mute, frame, c->frame, now_ms,
				       debug);
	}
	/* The line played a digit of the far end's, so the uplink detector is
	 * held as it is for a digit the caller played (core/chan.c). */
	if (digit)
		chan_digit_hold_arm(s, c);
}

/* --- engine side: transmit ----------------------------------------------- */

static void stream_cn_send(struct udsp *s, struct udsp_stream *st,
			   const int16_t *frame, unsigned n, uint32_t now_ms)
{
	size_t len = RTP_HEADER_SIZE + 1u;

	if (rtp_header_build(&st->sender, STREAM_CN_PT, st->sender.marker_pending,
			     st->sender.timestamp, st->pkt, sizeof(st->pkt)))
		return;
	st->pkt[RTP_HEADER_SIZE] = (uint8_t)udsp_cn_level_get(frame, n);
	st->sender.marker_pending = false;
	st->stats.packets_sent++;
	st->stats.bytes_sent += len;
	st->stats.octets_sent += 1u;
	stream_pkt_send(s, st, len, false, now_ms);
}

/*
 * The frames already aggregated were captured, so the media clock steps over
 * them before anything else stamps a packet. Zeroing the aggregate alone leaves
 * the sender timestamp behind the audio: telev_sender_start then latches it and
 * stamps the digit up to one packet early, and a talkspurt that reopens on a
 * stale frame count builds a short packet under a full timestamp step.
 */
static void stream_agg_settle(struct udsp_stream *st)
{
	st->sender.timestamp += (uint32_t)st->agg_frames * st->ts_per_frame;
	st->agg_frames = 0;
	st->payload_len = 0;
}

/*
 * Opens an event where the media clock stands. The settle runs before
 * telev_sender_start latches the sender timestamp, never after: the digit
 * belongs where the clock stands. A refused start puts the staged frames
 * back, so the caller loses nothing.
 *
 * target_ms is what ends the event. The line's own digit gives the ceiling
 * and the detector's end edge closes it earlier; udsp_stream_dtmf gives the
 * duration the caller asked for. The ceiling is the event's and not the
 * field's: stream_tx_telev segments what the field cannot carry.
 */
static int stream_telev_open(struct udsp_stream *st, char digit,
			     uint8_t volume, uint32_t target_ms, bool native)
{
	uint8_t frames = st->agg_frames;
	uint16_t len = st->payload_len;

	stream_agg_settle(st);
	if (telev_sender_start(&st->tev_tx, digit, volume)) {
		st->sender.timestamp -= (uint32_t)frames * st->ts_per_frame;
		st->agg_frames = frames;
		st->payload_len = len;

		return -EINVAL;
	}
	st->tev_elapsed_ms = 0;
	st->tev_seg_ts = 0;
	st->tev_target_ms = target_ms > UDSP_TELEV_MAX_MS ? UDSP_TELEV_MAX_MS :
							    target_ms;
	st->tev_native = native;
	st->tev_digit = native ? digit : 0;

	return 0;
}

/*
 * The one send of the staged media packet, for a full packet and for the
 * flush-first rule (codec.h Frames) alike. An untransmitted frame and an encode
 * error both end at stream_agg_settle, and the settle never sends, so the
 * speech frames already staged leave the box first or not at all. The send
 * advances the media clock over the flushed frames, so the settle that follows
 * covers only the frames staged after it.
 */
static void stream_agg_flush(struct udsp *s, struct udsp_stream *st,
			     uint32_t now_ms)
{
	uint32_t samples = (uint32_t)st->agg_frames * st->ts_per_frame;
	size_t len = 0;

	if (!st->payload_len)
		return;
	if (!rtp_packet_build(&st->sender, st->payload, st->payload_len,
			      samples, st->pkt, sizeof(st->pkt), &len))
		stream_pkt_send(s, st, len, false, now_ms);
	st->agg_frames = 0;
	st->payload_len = 0;
}

/*
 * The feed the voice detector and the RFC 3389 level measurement read, at the
 * codec's rate (rs_vad). It is its own resampler instance because it keeps
 * running while the talkspurt is closed, which is exactly when the encoder
 * feed stands still.
 *
 * Limit: dsp/vad.c reads a fixed 80 samples, so on a stream whose codec runs
 * at 16000 the detector sees the first 5 ms of each frame. The measurement
 * follows the codec rate because the level it reports is the level of what
 * the far end reconstructs; a per-count voice detector would let it read the
 * whole frame.
 */
static const int16_t *stream_tx_measure(struct udsp_stream *st,
					const int16_t *frame, unsigned *n)
{
	if (!st->resample)
		return frame;

	stream_resample_run(&st->rs_vad, frame, *n, st->vad_pcm, st->codec_frame);
	*n = st->codec_frame;

	return st->vad_pcm;
}

/*
 * The whole-packet transmit (codec.h Frames). A codec with frame_ms 0 is
 * handed the whole packet, so the tick stages its frame at the codec's rate
 * and only the last tick of the packet encodes. The staging is sized for the
 * widest packet ptime_mask can admit, and frames_per_packet is derived from a
 * ptime media_prepare already checked against that mask, so the write always
 * falls inside tx_pcm.
 *
 * Zero bytes is not an error: codec.h reserves it for a codec with nothing to
 * send, and the media clock still steps over the packet, because the far end
 * counts the gap in timestamps. An encode error suppresses the packet the same
 * way, the rule the fixed-frame path applies per frame.
 */
static void stream_tx_unit(struct udsp *s, struct udsp_stream *st,
			   const int16_t *frame, unsigned n, uint32_t now_ms)
{
	int16_t *stage = st->tx_pcm + (unsigned)st->agg_frames * st->codec_frame;
	int bytes;

	stream_resample_run(&st->rs_tx, frame, n, stage, st->codec_frame);
	st->agg_frames++;
	if (st->agg_frames < st->frames_per_packet)
		return;
	bytes = st->ops->encode(st->enc, st->tx_pcm,
				(unsigned)st->agg_frames * st->codec_frame,
				st->payload, sizeof(st->payload));
	if (bytes <= 0) {
		stream_agg_settle(st);
		return;
	}
	st->payload_len = (uint16_t)bytes;
	stream_agg_flush(s, st, now_ms);
}

static void stream_tx_edge(struct udsp *s, struct udsp_stream *st, bool voice,
			   uint32_t now_ms)
{
	if (!voice && !st->silent) {
		/* The flush-first rule applies to the close of a talkspurt as
		 * it does to an untransmitted frame: the staged frames went
		 * through the encoder, and a backward-adaptive decoder that
		 * never sees them leaves step with it for the whole next
		 * talkspurt. */
		stream_agg_flush(s, st, now_ms);
		st->silent = true;
		st->cn_next_ms = now_ms;
	}
	if (voice && st->silent) {
		stream_agg_settle(st);
		st->silent = false;
		/* RFC 3551: the first packet after a silence period carries the
		 * marker. */
		rtp_sender_restart(&st->sender);
	}
}

/* meas is the frame the voice detector read, at the codec's rate. */
static void stream_tx_silent(struct udsp *s, struct udsp_stream *st,
			     const int16_t *meas, unsigned meas_n,
			     uint32_t now_ms)
{
	uint32_t samples;

	st->agg_frames++;
	if (st->agg_frames < st->frames_per_packet)
		return;
	samples = (uint32_t)st->agg_frames * st->ts_per_frame;
	st->agg_frames = 0;
	st->payload_len = 0;
	/* The description carries the instant the silence starts, so it goes
	 * out before the media clock steps over the gap. The level is measured
	 * on the codec's frame, which is what the far end reconstructs the
	 * noise from. */
	if (st->media.cng && (int32_t)(now_ms - st->cn_next_ms) >= 0) {
		st->cn_next_ms = now_ms + UDSP_CN_REPEAT_MS;
		stream_cn_send(s, st, meas, meas_n, now_ms);
	}
	st->sender.timestamp += samples;
}

static void stream_tx_encoded(struct udsp *s, struct udsp_stream *st,
			      int bytes, uint32_t now_ms)
{
	if (bytes <= 0) {
		/* An untransmitted frame (codec-native DTX) and an encode error
		 * both suppress this frame without slipping the media clock
		 * and without dropping the frames already staged: the staged
		 * speech leaves first, then the clock settles over this frame,
		 * because the far end counts the gap in timestamps. Only a
		 * codec with a SID frame may return 0, so only one arms the
		 * silence state: a stray 0 from any other codec must not put a
		 * marker on the packet after it. */
		stream_agg_flush(s, st, now_ms);
		st->agg_frames++;
		stream_agg_settle(st);
		if (!bytes && st->ops->sid_bytes)
			st->silent = true;
		return;
	}
	if (st->ops->sid_bytes && bytes == (int)st->ops->sid_bytes) {
		/* codec.h: a SID ends the packet, below frames_per_packet
		 * included, so the far end learns the silence before the
		 * clock steps over it. A SID that refreshes a standing
		 * silence travels alone the same way. */
		bool marker = st->sender.marker_pending;

		st->payload_len = (uint16_t)(st->payload_len + (unsigned)bytes);
		st->agg_frames++;
		/* A SID sits inside the silence, and RFC 3551 marks the
		 * first packet after it: a marker a mid-silence
		 * reconfiguration armed (rtp_sender_reconfigure raises it on
		 * any description change) must go with the next speech packet,
		 * so the flush may not wear it. */
		st->sender.marker_pending = false;
		stream_agg_flush(s, st, now_ms);
		st->sender.marker_pending = marker;
		st->silent = true;
		return;
	}
	if (st->silent) {
		st->silent = false;
		/* RFC 3551: the first packet after the suppressed silence
		 * carries the marker; under codec-native DTX the encode
		 * return is what reopens the talkspurt. */
		rtp_sender_restart(&st->sender);
	}
	st->payload_len = (uint16_t)(st->payload_len + (unsigned)bytes);
	st->agg_frames++;
	if (st->agg_frames < st->frames_per_packet)
		return;
	stream_agg_flush(s, st, now_ms);
}

/* External for the test hook alone (stream.h), like stream_conceal_unit. */
void stream_tx_media(struct udsp *s, struct udsp_stream *st,
		     const int16_t *frame, unsigned n, uint32_t now_ms)
{
	const int16_t *pcm = frame;
	int bytes;

	/*
	 * The silence edges and the silent early return below belong to the
	 * generic detector alone. Under codec-native DTX (media.dtx) the
	 * encoder must run every frame, silence included, because its own
	 * detector needs the analysis; st->silent is then the codec-DTX
	 * state, written by the encode returns.
	 */
	if (st->media.vad) {
		unsigned meas_n = n;
		const int16_t *meas = stream_tx_measure(st, frame, &meas_n);

		stream_tx_edge(s, st, udsp_vad_process(&st->vad, meas), now_ms);
		if (st->silent) {
			stream_tx_silent(s, st, meas, meas_n, now_ms);
			return;
		}
	}
	if (st->unit_packet) {
		stream_tx_unit(s, st, frame, n, now_ms);
		return;
	}
	if (st->resample) {
		stream_resample_run(&st->rs_tx, frame, n, st->tx_pcm,
				st->codec_frame);
		pcm = st->tx_pcm;
	}
	bytes = st->ops->encode(st->enc, pcm, st->codec_frame,
				st->payload + st->payload_len,
				sizeof(st->payload) - st->payload_len);
	stream_tx_encoded(s, st, bytes, now_ms);
}

/* RFC 4733 2.5.1.4: the last event packet is sent three times, so a single
 * loss cannot leave the far end with a digit that never ends. */
static void stream_telev_packet(struct udsp *s, struct udsp_stream *st,
				uint32_t now_ms)
{
	size_t len = 0;

	if (telev_sender_packet_build(&st->tev_tx, st->pkt, sizeof(st->pkt),
				      &len))
		return;
	stream_pkt_send(s, st, len, false, now_ms);
}

/* The duration this event packet carries, in the stream clock's units and
 * counted from the start of the segment it belongs to (RFC 4733 2.5.1.3). The
 * base is held in those units and not in milliseconds: the segment bound is
 * 65535 ticks, which is not a whole millisecond at every clock. */
static uint32_t stream_telev_duration(const struct udsp_stream *st)
{
	uint32_t ts_per_ms = st->ts_per_frame / UDSP_TICK_MS;

	return st->tev_elapsed_ms * ts_per_ms - st->tev_seg_ts;
}

/*
 * The event outran the 16-bit duration field, so this segment closes with the
 * field at its maximum and the E bit clear, and the next segment starts at
 * that end on the same event code (RFC 4733 2.5.1.3). The
 * event has not ended, so nothing here may set E: a receiver reads that as the
 * end of the digit and reports the chain twice.
 *
 * The close goes out UDSP_TELEV_REPEAT times, as the last packet of the event
 * does: the copies carry the same timestamp and the same duration and only
 * their sequence numbers advance, and a receiver reads them as the
 * retransmissions they are. One lost close would otherwise take the whole
 * segment out of the duration the far end reports.
 */
static void stream_telev_segment(struct udsp *s, struct udsp_stream *st,
				 uint32_t now_ms)
{
	unsigned i;

	telev_sender_update(&st->tev_tx, TELEV_DURATION_MAX);
	for (i = 0; i < UDSP_TELEV_REPEAT; i++)
		stream_telev_packet(s, st, now_ms);
	telev_sender_segment(&st->tev_tx);
	st->tev_seg_ts += TELEV_DURATION_MAX;
}

/*
 * The end of an event, whichever side started it. The loop is bounded: a
 * packet the sender cannot build leaves the event active for ever otherwise,
 * and a description that dropped the event payload type is exactly that case,
 * so an event the builder refuses is dropped instead.
 */
static void stream_telev_close(struct udsp *s, struct udsp_stream *st,
			       uint32_t now_ms)
{
	unsigned i;

	/* The close comes on any tick, not only on a packet boundary
	 * (stream_tx_digit), so the segment step runs here as well: a duration
	 * the 16 bit field cannot hold reaches telev_sender_end below, which
	 * refuses it, and the event would end with no E bit at all. */
	if (stream_telev_duration(st) > TELEV_DURATION_MAX)
		stream_telev_segment(s, st, now_ms);
	telev_sender_end(&st->tev_tx, (uint16_t)stream_telev_duration(st));
	for (i = 0; i < UDSP_TELEV_REPEAT && st->tev_tx.active; i++)
		stream_telev_packet(s, st, now_ms);
	if (st->tev_tx.active)
		telev_sender_init(&st->tev_tx, &st->sender);
	/* The media clock resumes where the event left it (telev.c moved the
	 * sender's timestamp), and the next media packet opens a talkspurt. */
	rtp_sender_restart(&st->sender);
	st->agg_frames = 0;
	st->payload_len = 0;
	st->tev_native = false;
	st->tev_digit = 0;
}

static void stream_tx_telev(struct udsp *s, struct udsp_stream *st,
			    uint32_t now_ms)
{
	st->tev_elapsed_ms += UDSP_TICK_MS;
	st->agg_frames++;
	if (st->agg_frames < st->frames_per_packet)
		return;
	st->agg_frames = 0;

	/* The close runs the segment step of its own, so it stays here for
	 * the update alone. */
	if (st->tev_elapsed_ms >= st->tev_target_ms) {
		stream_telev_close(s, st, now_ms);
		return;
	}
	if (stream_telev_duration(st) > TELEV_DURATION_MAX)
		stream_telev_segment(s, st, now_ms);
	telev_sender_update(&st->tev_tx, (uint16_t)stream_telev_duration(st));
	stream_telev_packet(s, st, now_ms);
}

/*
 * The transmit half of the gateway. The demodulator needs every frame the
 * line captured, so rx_pcm runs whatever the direction says and only the
 * datagrams are held back; dsp/resamp.h decimates into the staging first
 * where the line and the gateway run at different rates.
 *
 * The send loop is bounded by the ring the plug-in keeps, so a backlog cannot
 * turn one tick into an unbounded run of sends. udptl_out answers 0 when it
 * has nothing more and a negative errno when the cap it was given is too
 * small, which is a fault of this caller and is counted as one.
 */
#define STREAM_T38_BURST	8u

static void stream_tx_gateway(struct udsp *s, struct udsp_stream *st,
			      const int16_t *ul, unsigned n, uint32_t now_ms)
{
	if (st->resample) {
		stream_resample_run(&st->rs_tx, ul, n, st->tx_pcm,
				    UDSP_FRAME_SAMPLES);
		st->fax_gate.ops->rx_pcm(st->fax_gate.inst, st->tx_pcm);
	} else {
		st->fax_gate.ops->rx_pcm(st->fax_gate.inst, ul);
	}
	if (!media_sends(&st->media))
		return;
	for (unsigned i = 0; i < STREAM_T38_BURST; i++) {
		int len = st->fax_gate.ops->udptl_out(st->fax_gate.inst,
						      st->pkt,
						      UDSP_NET_DGRAM_MAX);

		if (len < 0)
			st->t38_errors++;
		if (len <= 0)
			return;
		st->t38_sent++;
		st->t38_bytes_sent += (uint64_t)len;
		stream_tap_tx(s, st, st->pkt, (size_t)len, UDSP_PKT_UDPTL);
		stream_send(s, st, st->rtp_fd, (size_t)len, false, now_ms);
	}
}

/*
 * The line's own digit on this stream (chan.h). The close reads the digit the
 * line holds and not the end edge of one tick: a reconfiguration takes the
 * stream off the tick for about three ticks, and an end raised inside that
 * window would reach no transmit tick of this stream, which would leave the
 * event running to the ceiling over live audio. The level answer covers the
 * end, an end this stream did not see, and a change straight to another
 * digit.
 *
 * The start stays an edge. A stream that becomes active in the middle of a
 * digit sees the digit and no start of its own, and relays nothing for it.
 *
 * A start while an event still runs ends that event first. The analog tone is
 * the line's own digit and pre-empts an event udsp_stream_dtmf began, and the
 * far end closes the first digit on the ends it saw. The edges are consumed
 * on the tick they arrive, ahead of the packet interval stream_tx_telev
 * returns on, so the event opens where the mute opens and the end packets
 * leave where the tone ended.
 */
static void stream_tx_digit(struct udsp *s, struct udsp_stream *st,
			    const struct udsp_chan *c, uint32_t now_ms)
{
	if (st->tev_native && c->digit_active != st->tev_digit)
		stream_telev_close(s, st, now_ms);
	if (!c->digit_started || !stream_relay_allows(st))
		return;
	if (st->tev_tx.active)
		stream_telev_close(s, st, now_ms);

	stream_telev_open(st, c->digit_active, (uint8_t)-c->digit_level_dbm0,
			  UDSP_TELEV_MAX_MS, true);
}

/*
 * An RFC 4733 packet is media, so a direction that does not send must not put
 * one on the wire either.
 *
 * The hold silence enters here (udsp.h udsp_media.tx_silence, hold-tx). The
 * uplink is replaced at the one point where it reaches the transmit chain,
 * ahead of the voice detector, the resampler and the packet staging, so the
 * substitution is codec-blind and rate-blind: every encoder below sees the
 * zeros and produces its own silence, and the wideband path is silent for the
 * same reason the narrowband one is. quiet lives on the tick stack, which
 * rt_stack_prefault made resident, exactly as the mute frame of stream_tick_tx
 * does.
 *
 * The digit branch needs no test of its own. The events are silenced where
 * they are decided and not here: stream_dtmf refuses to start one on a
 * silenced stream and stream_engine_reconfig cuts one in flight, so tev_tx is
 * never active while the field is set.
 */
static void stream_tx_run(struct udsp *s, struct udsp_stream *st,
			  const struct udsp_chan *c, const int16_t *ul,
			  unsigned n, uint32_t now_ms)
{
	int16_t quiet[UDSP_FRAME_MAX];
	const int16_t *tx = ul;

	if (st->media.tx_silence) {
		memset(quiet, 0, sizeof(quiet));
		tx = quiet;
	}
	/* The substitution above stands ahead of the mode branch, so a held
	 * T.38 stream feeds the gateway the same silence an audio stream
	 * encodes. The gateway runs before the direction gate, because the
	 * demodulator needs the frame even on a stream that never sends. */
	if (st->mode == STREAM_MODE_T38) {
		stream_tx_gateway(s, st, tx, n, now_ms);
		return;
	}
	if (!media_sends(&st->media))
		return;
	stream_tx_digit(s, st, c, now_ms);
	if (st->tev_tx.active) {
		stream_tx_telev(s, st, now_ms);
		return;
	}
	stream_tx_media(s, st, tx, n, now_ms);
}

static void stream_rtcp_send(struct udsp *s, struct udsp_stream *st, bool bye,
			     uint32_t now_ms)
{
	struct rtcp_sender_info info;
	struct rtcp_report report;
	uint32_t lsr = 0, dlsr = 0;
	uint32_t msw = 0, lsw = 0;
	size_t len = 0;
	size_t n = 0;
	bool have;

	if (st->rtcp_fd < 0 || !st->media.rtcp_enable)
		return;
	ntp_now(&msw, &lsw);
	rtcp_state_report_timing(&st->rtcp, mono_16_16(s->tick_ns), &lsr, &dlsr);
	rtp_receiver_report(&st->receiver, lsr, dlsr, &report);
	have = st->receiver.source_valid && !st->receiver.probation;

	if (st->stats.packets_sent) {
		rtcp_sender_info_get(&st->sender, msw, lsw, &info);
		if (rtcp_sr_build(&info, have ? &report : NULL, st->pkt,
				  sizeof(st->pkt), &len))
			return;
	} else if (rtcp_rr_build(st->sender.ssrc, have ? &report : NULL, st->pkt,
				 sizeof(st->pkt), &len)) {
		return;
	}
	if (rtcp_sdes_build(st->sender.ssrc, st->cname, st->pkt + len,
			    sizeof(st->pkt) - len, &n))
		return;
	len += n;
	/* RFC 3550 6.6: a BYE travels inside the compound packet, never alone. */
	if (bye && !rtcp_bye_build(st->sender.ssrc, NULL, st->pkt + len,
				   sizeof(st->pkt) - len, &n))
		len += n;
	stream_pkt_send(s, st, len, true, now_ms);
}

static void stream_rtcp_tick(struct udsp *s, struct udsp_stream *st,
			     uint32_t now_ms)
{
	if ((int32_t)(now_ms - st->rtcp_next_ms) < 0)
		return;
	st->rtcp_next_ms = now_ms + st->media.rtcp_interval_ms;
	stream_rtcp_send(s, st, false, now_ms);
}

/* Runs in every direction and in both modes: a stream that sends nothing of
 * its own is the one whose tuple needs holding. With rtcp_enable false the
 * report cadence sends nothing and this alone holds the RTCP tuple. */
static void stream_punch_run(struct udsp_stream *st, uint32_t now_ms)
{
	int32_t interval = (int32_t)st->media.punch_interval_ms;

	if ((int32_t)(now_ms - st->rtp_tx_ms) >= interval)
		stream_punch(st, st->rtp_fd, &st->rtp_tx_ms, now_ms);
	if ((int32_t)(now_ms - st->rtcp_tx_ms) >= interval)
		stream_punch(st, st->rtcp_fd, &st->rtcp_tx_ms, now_ms);
}

/*
 * Which streams read the line's relay tap (chan.h) instead of the live
 * uplink. A stream that carries the digit out of band takes the delayed
 * frame, which is silent over the tone the line heard; every other stream
 * takes the live frame, so the inband path carries the tone unchanged. An
 * event mode whose description names no event payload type keeps the tone
 * too, because nothing would relay the digit (rtp.h media_event_pt_get, the
 * rule the relay gate reads).
 *
 * The description is read on a tick of a stream the engine activated, which
 * is the rule stream_governs reads tx_silence under.
 */
static bool stream_relay_tap_reads(const struct udsp_stream *st)
{
	if (st->mode != STREAM_MODE_AUDIO ||
	    st->passthrough_kind != UDSP_PASSTHROUGH_OFF)
		return false;
	if (st->media.dtmf_mode == UDSP_DTMF_INFO)
		return true;

	return media_event_pt_get(&st->media) != RTP_PT_DISABLED;
}

/*
 * A leg puts on the bus exactly what the local playout hears from it and takes
 * back exactly what it put on: a direction that does not receive contributes
 * nothing, so the mix-minus subtracts nothing from it either.
 */
static bool stream_conf_leg(const struct udsp_stream *st)
{
	return atomic_load_explicit(&st->active, memory_order_acquire) &&
	       media_receives(&st->media);
}

/*
 * The bus carries the frames stream_tick_rx decoded on the previous tick,
 * because UL runs before DL in the tick order (chan.h): the conference costs
 * one period of delay between the legs, and the alternative is a second decode
 * pass per tick.
 */
static void stream_conf_bus_fill(struct udsp *s, struct udsp_chan *c)
{
	struct udsp_stream *st;

	udsp_mix_bus_reset(&c->conf_bus);
	stream_line_for_each(s, c->line, st) {
		if (!stream_conf_leg(st))
			continue;
		udsp_mix_bus_add(&c->conf_bus, st->dl, c->frame);
	}
}

/*
 * The send half of periods that no capture frame went with: the periods the
 * engine skipped (stream_tick_skip) and the ticks of a refill
 * (stream_tick_tx_synthetic). Those frames never reached the encoder, so the
 * media clock steps over them and the far end reads the gap in the timestamps
 * instead of hearing the audio move one gap closer for the rest of the call.
 *
 * The staged frames go first. They were captured before the gap, so they carry
 * their own timestamp and leave the box under the flush-first rule of codec.h;
 * the settle behind the flush carries the clock over frames a whole-packet
 * codec staged with no bytes to send, which are dropped.
 *
 * An event in flight owns the sender timestamp: rtp/telev.c recomputes it from
 * the event start and the duration on every update, so a step taken here would
 * be overwritten. The event's elapsed time takes the step instead, which is
 * what the duration the far end reads is built from.
 */
static void stream_tx_skip(struct udsp *s, struct udsp_stream *st,
			   unsigned periods, uint32_t now_ms)
{
	if (st->tev_tx.active) {
		st->tev_elapsed_ms += periods * UDSP_TICK_MS;
		return;
	}
	stream_agg_flush(s, st, now_ms);
	stream_agg_settle(st);
	st->sender.timestamp += periods * st->ts_per_frame;
}

/* The send half of periods that no capture frame went with, on every stream
 * of the line that sends. */
static void stream_tx_skip_line(struct udsp *s, struct udsp_chan *c,
				unsigned periods, uint32_t now_ms)
{
	struct udsp_stream *st;

	stream_line_for_each(s, c->line, st) {
		/* The acquire stands ahead of every other field: an inactive
		 * stream's description belongs to the control thread. */
		if (!atomic_load_explicit(&st->active, memory_order_acquire))
			continue;
		if (st->mode == STREAM_MODE_T38 || !media_sends(&st->media))
			continue;
		stream_tx_skip(s, st, periods, now_ms);
	}
}

/*
 * The uplink of a synthetic tick (chan.c chan_ul_synthetic): the period the
 * engine rendered ahead has no capture frame, so the send side steps over it
 * as over a skipped period. The far end reads one more period of gap in the
 * timestamps, and the uplink loses what a skip of the same length loses.
 */
void stream_tick_tx_synthetic(struct udsp *s, struct udsp_chan *c,
			      uint32_t now_ms)
{
	stream_tx_skip_line(s, c, 1u, now_ms);
}

void stream_tick_tx(struct udsp *s, struct udsp_chan *c, const int16_t *frame,
		    uint32_t now_ms)
{
	bool mix = c->conf_mode == (uint8_t)UDSP_CONF_LOCAL_MIX;
	bool mute = atomic_load_explicit(&c->mute, memory_order_acquire);
	int16_t quiet[UDSP_FRAME_MAX];
	const int16_t *ul = frame;
	struct udsp_stream *st;

	if (mute) {
		memset(quiet, 0, sizeof(quiet));
		ul = quiet;
		/* Mute silences the far end, and under a conference every leg
		 * is a far end: the mix stays outside the mute bracket, so a
		 * muted line sounds exactly as it does with no conference at
		 * all. */
		mix = false;
	}
	if (mix)
		stream_conf_bus_fill(s, c);

	stream_line_for_each(s, c->line, st) {
		const int16_t *tx;

		if (!atomic_load_explicit(&st->active, memory_order_acquire))
			continue;
		/* The mute covers the whole line, so it stands over the tap;
		 * both put silence where the far end must hear none. */
		tx = !mute && stream_relay_tap_reads(st) ? c->ul_relay : ul;
		if (mix) {
			udsp_mix_minus(&c->conf_bus,
				       stream_conf_leg(st) ? st->dl : NULL, tx,
				       c->conf_tx, c->frame);
			tx = c->conf_tx;
		}
		stream_tx_run(s, st, c, tx, c->frame, now_ms);
		stream_rtcp_tick(s, st, now_ms);
		/* After the media and the report: a descriptor one of them
		 * wrote this tick carries a fresh stamp and is not punched. */
		stream_punch_run(st, now_ms);
	}
}

/* --- engine side: skipped periods ---------------------------------------- */

/*
 * The skipped opportunities a stream decodes and discards in one skip: 100 ms,
 * which carries a stateful decoder, the receive resampler and the slice
 * staging across the gap as if the units played. A longer skip passes its
 * earlier units without a decode, restarts the three, and decodes this many
 * at its end.
 */
#define STREAM_SKIP_DECODE_MAX	10u

/* The identity of the event the line plays (rtp.h struct telev_play): its
 * code, the timestamp its chain began at and the arrival that began it. A
 * segment that continues the chain at the duration ceiling keeps all three. */
struct stream_tev_chain {
	uint64_t start_ns;
	uint32_t chain_ts;
	uint8_t event;
	bool playing;
};

static void stream_tev_chain_take(const struct udsp_stream *st,
				  struct stream_tev_chain *ch)
{
	const struct telev_play *p = &st->tev_play;

	ch->playing = p->active && !p->done;
	ch->event = p->event;
	ch->chain_ts = p->event_ts - p->seg_units;
	ch->start_ns = p->start_ns;
}

/*
 * The event that played before the skip plays on through it, as on the ticks
 * that the engine never ran: the tracker advances, and the generator does not
 * render, as on a muted tick. Only that event advances. The packets the skip
 * put first can replace it, end it or time it out, and an event they started
 * takes no part of the skipped time.
 */
static void stream_tev_skip(struct udsp_stream *st,
			    const struct stream_tev_chain *was,
			    unsigned periods, uint64_t now_ns)
{
	struct stream_tev_chain now;
	uint64_t units;
	uint32_t left;

	stream_tev_chain_take(st, &now);
	if (!was->playing || !now.playing || was->event != now.event ||
	    was->chain_ts != now.chain_ts || was->start_ns != now.start_ns)
		return;
	if (!telev_play_tick(&st->tev_play, now_ns, &left))
		return;
	units = (uint64_t)periods * st->ts_per_frame;
	telev_play_advance(&st->tev_play,
			   units < left ? (uint32_t)units : left);
}

/*
 * The decoder, the receive resampler and the whole-packet staging start over,
 * because the run passed encoded audio without a decode. A codec SID that the
 * run passed goes to the fresh decoder first (udsp_jb_skip_sid). Reason: the
 * comfort noise that the buffer plays next reaches the decoder as empty
 * continuations, which a decoder with no SID refuses.
 */
static void stream_rx_restart(struct udsp_stream *st)
{
	int16_t pcm[UDSP_PCM_STAGE_MAX];
	const uint8_t *sid;
	size_t len;

	if (st->dec && st->ops->reset)
		st->ops->reset(st->dec);
	udsp_resamp_reset(&st->rs_rx);
	st->rx_unit_valid = false;
	len = udsp_jb_skip_sid(&st->jb, &sid);
	if (len && st->dec && st->ops->sid_bytes)
		(void)st->ops->decode(st->dec, sid, len, pcm,
				      UDSP_PCM_STAGE_MAX);
}

/*
 * The jitter buffer pays the skipped opportunities (jb.h Skipped
 * opportunities), in its own units: one 10 ms unit is ts_per_frame of the RTP
 * clock, 80 for G.722 although its decoder writes 160 samples. Each of the
 * last STREAM_SKIP_DECODE_MAX goes through the decode a tick runs, and the
 * frame is discarded: no audio of it reaches the line, the canceller's
 * history or a conference leg, whose bus reads dl on the next tick. The run
 * before them restarts the decode only if it passed encoded audio. A run of
 * comfort noise or concealment alone leaves the decoder's comfort noise
 * state as it stands.
 */
static void stream_rx_skip_units(struct udsp_stream *st, unsigned units,
				 uint64_t now_ns)
{
	unsigned decode = units < STREAM_SKIP_DECODE_MAX ?
			  units : STREAM_SKIP_DECODE_MAX;
	int16_t keep[UDSP_FRAME_MAX];
	struct udsp_jb_out out;

	if (units > decode && udsp_jb_skip(&st->jb, units - decode, now_ns))
		stream_rx_restart(st);
	memcpy(keep, st->dl, sizeof(keep));
	for (unsigned i = 0; i < decode; i++) {
		udsp_jb_skip_get(&st->jb, now_ns, &out);
		stream_decode(st, &out);
	}
	memcpy(st->dl, keep, sizeof(keep));
	udsp_jb_skip_end(&st->jb);
}

/*
 * The receive half of a skip. The far end kept sending through the periods
 * the engine never ran, so each of them leaves a get of the jitter buffer
 * open; left open, they would stand as downlink delay for the rest of the
 * call.
 *
 * The order. The packets in the ring that arrived by now_ns go in first, with
 * their own arrival stamps (stream_ring_skip_drain), so the buffer passes
 * over the audio that arrived for the skipped periods. The opportunities are
 * then paid to the timeline they belonged to. A put that started a timeline
 * (the first packet, a new source, a reset) cancelled them: the new timeline
 * starts unadvanced, the probation release of rtp.c included. The event that
 * played before goes last.
 *
 * Accounting. A skip is a count of opportunities, not an interval. The engine
 * delivers the periods it never ran in one call or over several: a catch-up
 * that trims after fast ticks, or a drain that drops one read at a time. Each
 * call pays its count, one 10 ms unit per period. now_ns, the stamp of the
 * skip hook, is the one time it reads. It is the arrival cutoff of the drain,
 * and it moves the buffer's time once. No opportunity gets an instant of its
 * own, so a skip infers no arrival time and rebuilds no interval.
 */
static void stream_rx_skip(struct udsp *s, struct udsp_stream *st,
			   unsigned periods, uint64_t now_ns, uint32_t now_ms,
			   uint32_t debug)
{
	uint32_t timeline = udsp_jb_timeline(&st->jb);
	struct stream_tev_chain chain;

	stream_tev_chain_take(st, &chain);
	stream_ring_skip_drain(s, st, now_ns, now_ms, debug);
	if (udsp_jb_timeline(&st->jb) == timeline)
		stream_rx_skip_units(st, periods, now_ns);
	stream_tev_skip(st, &chain, periods, now_ns);
}

/*
 * The engine skipped capture periods (engine.h engine_hooks.skip): hardware
 * time in which the tick never ran. The receive half runs first, for every
 * audio stream, whatever its direction (stream_rx_skip). The send half then
 * steps the media clock of a stream that sends (stream_tx_skip).
 *
 * The periods of a refill are not among them. The engine renders those as
 * ticks and gives the skip hook that many periods fewer (engine.c
 * engine_refill_charge), so the jitter buffer plays them as gets.
 */
void stream_tick_skip(struct udsp *s, struct udsp_chan *c, unsigned periods,
		      uint64_t now_ns, uint32_t now_ms)
{
	uint32_t debug = (uint32_t)atomic_load_explicit(&s->debug,
							memory_order_relaxed);
	struct udsp_stream *st;

	stream_line_for_each(s, c->line, st) {
		/* The acquire stands ahead of every other field: an inactive
		 * stream's description belongs to the control thread. */
		if (!atomic_load_explicit(&st->active, memory_order_acquire))
			continue;
		if (st->mode == STREAM_MODE_T38)
			continue;
		stream_rx_skip(s, st, periods, now_ns, now_ms, debug);
		if (media_sends(&st->media))
			stream_tx_skip(s, st, periods, now_ms);
	}
}

/* --- engine side: commands ----------------------------------------------- */

/*
 * udsp_jb_init clears the buffer's counters, so what the snapshot still holds
 * belongs to a call that is gone. Publishing the fresh record here does not
 * close the window: snap.h skips a publish while a reader holds the slot, and
 * an adopted stream that is not started yet runs no tick to try again, so a
 * reader would keep the previous call's totals for the whole of call setup.
 * The flag says "no counters for this configuration yet" instead, and the
 * first publish that succeeds clears it. The trace forgets its last decision
 * for the same reason, so the first output unit of the new call is recorded
 * instead of being compared against a stream that is gone.
 */
static void stream_jb_restart(struct udsp_stream *st, uint32_t now_ms)
{
	atomic_store_explicit(&st->jb_snap_valid, false, memory_order_release);
	st->jb_trace_have = false;
	st->jb_trace_key = 0;
	st->jb_trace_ms = now_ms;
}

/*
 * What the codec and the line agree on for this description. A codec with no
 * sample rate of its own follows the line, so the rate comparison decides
 * everything: at a matched rate no resampler is instantiated, the codec reads
 * and writes the line frame, and the timestamp step is the frame count.
 * ts_per_frame comes from the RTP clock, not from the audio rate:
 * G.722 samples at 16000 and stamps at 8000 (RFC 3551).
 */
static void stream_resample_settle(struct udsp_stream *st, unsigned line_rate)
{
	/* udsp/fax.h hands the gateway one 80-sample frame at 8000 Hz whatever
	 * the line runs at, and a UDPTL session has no packet duration and no
	 * RTP clock, so the mode settles as a narrowband codec with neither. */
	bool t38 = st->mode == STREAM_MODE_T38;
	unsigned codec_rate = UDSP_SAMPLE_RATE;

	if (!t38)
		codec_rate = st->ops->sample_rate ? st->ops->sample_rate :
						    line_rate;
	st->codec_rate = codec_rate;
	st->codec_frame = codec_rate / (1000u / UDSP_TICK_MS);
	st->line_frame = line_rate / (1000u / UDSP_TICK_MS);
	st->ts_per_frame = t38 ? 0u :
			   st->ops->clock_rate / (1000u / UDSP_TICK_MS);
	st->resample = codec_rate != line_rate;
	/* codec.h Frames: frame_ms 0 is a whole-packet codec, whose packet no
	 * layer below this one may cut. */
	st->unit_packet = !t38 && st->ops->frame_ms == 0;
	st->rx_unit_valid = false;
	st->rx_unit_ts = 0;
	st->rx_unit_slices = 0;
	/* dsp/resamp.h: the stream layer resets its instances on adopt and on
	 * reconfig, so a new description never filters against the old one. */
	udsp_resamp_reset(&st->rs_tx);
	udsp_resamp_reset(&st->rs_rx);
	udsp_resamp_reset(&st->rs_vad);
}

/*
 * The depth this configuration asks the buffer for. A passthrough keeps the
 * depth the line adapted to before it began, for as long as it lasts: the
 * configured target is what the buffer started from, and the carrier needs
 * what the line proved it needs.
 */
static uint16_t stream_jb_depth(const struct udsp_stream *st)
{
	if (st->passthrough_kind == UDSP_PASSTHROUGH_OFF)
		return st->media.jb_init_ms;

	return st->jb_pin_ms;
}

/* The buffer counts in the stream's RTP units and slices by its rule, so it is
 * configured from the settled codec (jb.h RTP clock and Units). The two
 * members recorded here are what the next description compares against to
 * decide whether that rule still stands. */
static void stream_jb_init(struct udsp_stream *st)
{
	uint16_t ts_per_ms = (uint16_t)(st->ops->clock_rate / 1000u);
	uint16_t sid_len = (uint16_t)st->ops->sid_bytes;
	/* Every fixed-frame codec states its frame size (0 on a packet codec),
	 * so the buffer stamps a packet by its payload: a talkspurt that
	 * closes on an odd frame count ends on a packet below the learned
	 * ptime. The SID length goes in whenever the codec carries one,
	 * negotiated or not: a stray SID still slices right. Both inputs
	 * depend on the ops alone, so stream_jb_settle's re-init guard keeps
	 * covering every slicing input. */
	uint16_t frame_bytes = (uint16_t)st->ops->frame_bytes;

	udsp_jb_init_cfg(&st->jb, st->media.jb_mode, stream_jb_depth(st),
			 st->media.jb_max_ms, ts_per_ms,
			 (uint16_t)(ts_per_ms * UDSP_JB_FRAME_MS),
			 frame_bytes, sid_len, st->unit_packet);
	st->jb_ops = st->ops;
	st->jb_ptime_ms = st->media.ptime_ms;
}

/*
 * The pin, taken on the tick that first sees a kind on a stream that carried
 * none, and given back when the kind leaves. The buffer's own target is the
 * reading, because that is the depth the line adapted to; a buffer that never
 * played has none, and the configured target is the only figure there is.
 */
static void stream_jb_pin(struct udsp_stream *st, enum udsp_passthrough kind)
{
	if (kind == UDSP_PASSTHROUGH_OFF) {
		st->jb_pin_ms = 0;
		return;
	}
	if (st->passthrough_kind != UDSP_PASSTHROUGH_OFF)
		return;
	/* The buffer's target counts only while the configuration that
	 * produced it stands. A stop leaves the record of a call that is over,
	 * and the next call's own target is the only figure it has. */
	st->jb_pin_ms = st->jb_ops && st->jb.target_ms ? st->jb.target_ms :
			st->media.jb_init_ms;
}

/*
 * The reconfiguration's buffer. A description that keeps the codec and the
 * packet duration keeps the slicing rule and the learned span, so a
 * passthrough moves the mode alone and the queue plays on: the audio in it is
 * carrier the far end is already demodulating, and a re-init would drop it.
 * Anything else re-inits, at the pinned depth, because the payloads the queue
 * holds belong to a codec the buffer no longer slices by. A buffer that never
 * played re-inits too: it holds no audio to keep and no target to pin, and the
 * init is what puts the pinned depth in.
 */
static void stream_jb_settle(struct udsp_stream *st)
{
	if (st->passthrough_kind != UDSP_PASSTHROUGH_OFF && st->jb.target_ms &&
	    st->jb_ops == st->ops && st->jb_ptime_ms == st->media.ptime_ms) {
		udsp_jb_mode_set(&st->jb, st->media.jb_mode);
		return;
	}
	stream_jb_init(st);
}

/* What every description the engine takes starts with, in both modes, for an
 * adoption and a reconfiguration alike. */
static void stream_engine_take(struct udsp_stream *st, uint32_t now_ms)
{
	enum udsp_passthrough kind = media_passthrough_kind(&st->media);

	/* The backend reconnects the descriptors to a moved far end before it
	 * configures, and the core cannot see the connect, so every
	 * configuration earns one punch per descriptor on the next tick. */
	st->rtp_tx_ms = now_ms - st->media.punch_interval_ms;
	st->rtcp_tx_ms = now_ms - st->media.punch_interval_ms;
	st->mode = stream_mode_get(&st->media);
	/* A digit still rendering belongs to the description that carried it:
	 * the new one may run another clock, another line rate or no codec at
	 * all, and the far end has its own end packets for the event. No RFC
	 * 4733 event reaches a UDPTL session, so none renders there either. */
	telev_play_init(&st->tev_play);
	/*
	 * The committed description alone says what the line carries: a
	 * description with a kind, or the T.38 mode, governs the canceller
	 * policy, and one with neither puts the speech policy back. That is
	 * what leaves no profile state dangling behind a re-INVITE.
	 *
	 * The T.38 mode reads as the modem kind, because a converged filter
	 * subtracts its estimate from the very carrier the gateway demodulates.
	 * A source that ends keeps the depth the line adapted to: a profile
	 * that clamps an Opus call to G.711 adopts a new source and still
	 * carries the playout the call settled at. The pin runs before the
	 * buffer init and the buffer settle read it.
	 */
	stream_jb_pin(st, kind);
	st->passthrough_kind = (uint8_t)kind;
	memset(st->dl, 0, sizeof(st->dl));
	stream_jb_restart(st, now_ms);
}

/*
 * The machines of the audio mode. No RTP source, no jitter buffer, no comfort
 * noise and no telephone event belongs to a UDPTL session, and every one of
 * those machines is configured from a codec the T.38 mode has none of. So the
 * T.38 adoption initialises none of them and nothing reads them afterwards:
 * both chains take the gateway branch, the report cadence returns on the
 * forced rtcp_enable false, and udsp_stream_dtmf refuses the digit that would
 * reach the event sender.
 */
static void stream_engine_adopt_audio(struct udsp_stream *st, uint32_t now_ms)
{
	rtp_stats_init_cfg(&st->stats, st->ops->clock_rate);
	rtp_sender_init(&st->sender, &st->media, &st->entropy, &st->stats);
	rtp_receiver_init_cfg(&st->receiver, &st->media, st->ops->clock_rate,
			      &st->stats);
	rtcp_state_init(&st->rtcp, st->sender.ssrc, &st->stats);
	telev_sender_init(&st->tev_tx, &st->sender);
	telev_receiver_init_cfg(&st->tev_rx, st->ops->clock_rate);
	udsp_vad_init(&st->vad);
	/* A new source plays none of the audio the old one queued, so the
	 * buffer starts over whatever the description kept. */
	stream_jb_init(st);
	st->frames_per_packet = (uint8_t)(st->media.ptime_ms / UDSP_TICK_MS);
	st->agg_frames = 0;
	st->payload_len = 0;
	st->silent = false;
	st->tev_elapsed_ms = 0;
	st->tev_target_ms = 0;
	st->tev_seg_ts = 0;
	st->tev_native = false;
	st->rtcp_next_ms = now_ms + st->media.rtcp_interval_ms;
	st->jb_event_ms = now_ms;
	memset(&st->jb_last, 0, sizeof(st->jb_last));
}

static void stream_t38_clear(struct udsp_stream *st)
{
	st->t38_sent = 0;
	st->t38_recv = 0;
	st->t38_bytes_sent = 0;
	st->t38_bytes_recv = 0;
	st->t38_dropped = 0;
	st->t38_errors = 0;
}

static void stream_engine_adopt(const struct udsp *s, struct udsp_stream *st,
				unsigned line_rate, uint32_t now_ms)
{
	stream_engine_take(st, now_ms);
	stream_resample_settle(st, line_rate);
	atomic_store_explicit(&st->ring.head, 0u, memory_order_relaxed);
	atomic_store_explicit(&st->ring.tail, 0u, memory_order_relaxed);
	atomic_store_explicit(&st->net_drops, 0u, memory_order_relaxed);
	atomic_store_explicit(&st->net_stale, 0u, memory_order_relaxed);
	atomic_store_explicit(&st->net_trunc, 0u, memory_order_relaxed);
	st->rx_seen = false;
	st->tx_seen = false;
	atomic_store_explicit(&st->tx_eagain, 0u, memory_order_relaxed);
	atomic_store_explicit(&st->punch_sent, 0u, memory_order_relaxed);
	if (st->mode == STREAM_MODE_T38)
		stream_t38_clear(st);
	else
		stream_engine_adopt_audio(st, now_ms);
	st->stats_next_ms = now_ms;
	stream_stats_publish(s, st);
}

/* The description changes, the source does not. rtp_sender_reconfigure
 * keeps the SSRC, sequence and timestamp and raises the marker; jb.h's wiring
 * contract re-inits the buffer so the span learner adopts the new ptime. */
static void stream_engine_reconfig(struct udsp_stream *st, unsigned line_rate,
				   uint32_t now_ms)
{
	stream_engine_take(st, now_ms);
	if (st->mode == STREAM_MODE_T38) {
		/* The session continues across a port move, so the gateway and
		 * its counters stand and only the rate conversion settles
		 * again. */
		stream_resample_settle(st, line_rate);
		return;
	}
	rtp_sender_reconfigure(&st->sender, &st->media);
	rtp_receiver_reconfigure(&st->receiver, &st->media);
	/*
	 * A description that no longer allows the relay cuts an event in
	 * flight (stream_relay_allows): no further packet may leave, and
	 * RFC 4733 2.5.1.4 leaves the far end its own net, because the ends it
	 * already saw are what close the digit there. A direction that stopped
	 * sending, the hold silence and a passthrough kind each cut one for
	 * their own reason. A description that dropped the event payload type
	 * cuts one because the sender can build no packet without it, so the
	 * event would never end.
	 */
	if (!stream_relay_allows(st)) {
		telev_sender_init(&st->tev_tx, &st->sender);
		st->tev_native = false;
	}
	st->frames_per_packet = (uint8_t)(st->media.ptime_ms / UDSP_TICK_MS);
	/* The settle steps the media clock over the frames already aggregated,
	 * so it runs on the old timestamp step, before the new one applies. */
	stream_agg_settle(st);
	stream_resample_settle(st, line_rate);
	/* A same-clock codec change still moves the slicing rule, so the buffer
	 * is configured after the settle, not before it (jb.h wiring). */
	stream_jb_settle(st);
	/* The suppression the cleared state described really happened, and
	 * the sender cannot see a dtx flip (none of its description fields
	 * moves), so the first packet of the new description opens a
	 * talkspurt (RFC 3551). */
	if (st->silent) {
		st->silent = false;
		rtp_sender_restart(&st->sender);
	}
}

static int stream_engine_dtmf(struct udsp_stream *st, char digit,
			      uint32_t duration_ms)
{
	/* A digit still on the wire is back pressure, not a bad request: the
	 * caller may send the next one once this event ends. The line's own
	 * digit holds the sender the same way, and pre-empts this one when it
	 * arrives (stream_tx_digit). */
	if (st->tev_tx.active)
		return -EBUSY;

	return stream_telev_open(st, digit, UDSP_TELEV_VOLUME, duration_ms,
				 false);
}

int stream_cmd_run(struct udsp *s, const struct udsp_cmd *cmd, uint32_t now_ms)
{
	struct udsp_stream *st = stream_at(s, cmd->line, cmd->stream);

	if (!st)
		return -EINVAL;

	switch ((enum stream_op)cmd->arg0) {
	case STREAM_OP_ADOPT: {
		bool was_t38 = st->mode == STREAM_MODE_T38;

		stream_engine_adopt(s, st, s->rate, now_ms);
		/* The mode moved away from T.38 without a stop, so the session
		 * ended here; a move into it starts one and reports nothing. */
		if (was_t38 && st->mode != STREAM_MODE_T38)
			stream_t38_event(s, st, now_ms);
		return 0;
	}
	case STREAM_OP_RECONFIG:
		stream_engine_reconfig(st, s->rate, now_ms);
		return 0;
	case STREAM_OP_ACTIVATE:
		/* An idle line must not carry queued audio into its next
		 * activation. An active sibling still owns the shared tap. */
		if (!stream_line_active(s, st->line))
			chan_relay_tap_reset(&s->chan[st->line]);
		atomic_store_explicit(&st->active, true, memory_order_release);
		return 0;
	case STREAM_OP_DEACTIVATE:
		/* The BYE goes out while the engine still holds the
		 * descriptors: the control thread closes them next. The stop
		 * bit is what tells a stop from the quiesce of a running
		 * reconfiguration, whose session continues. */
		if (cmd->flags & 1u) {
			stream_rtcp_send(s, st, true, now_ms);
			/* The call ends here, so a digit the far end left
			 * running renders no further frame toward the line. */
			telev_play_init(&st->tev_play);
			if (st->mode == STREAM_MODE_T38) {
				stream_t38_event(s, st, now_ms);
				/* The session ended with the stop, so the next
				 * description starts a new one. The copy the
				 * net thread reads does not follow: the
				 * descriptors are still registered here, and a
				 * datagram that arrives before the detach
				 * belongs to the session that is ending
				 * (stream_activate publishes the next one). */
				st->mode = STREAM_MODE_AUDIO;
			}
		}
		atomic_store_explicit(&st->active, false, memory_order_release);
		return 0;
	case STREAM_OP_DTMF:
		return stream_engine_dtmf(st, (char)cmd->arg2, cmd->arg1);
	case STREAM_OP_CONF:
		/* Line scoped: the stream index above is 0 and its lookup is
		 * only what proves the line exists. */
		s->chan[cmd->line].conf_mode = (uint8_t)cmd->arg1;
		return 0;
	case STREAM_OP_LOOP: {
		struct udsp_chan *c = &s->chan[cmd->line];
		uint8_t mode = (uint8_t)(cmd->arg1 & 0xffu);

		/* Line scoped, as CONF is. The arm and the disarm both clear
		 * the line's own ring, so a SELF loop never plays audio from
		 * before its arm. A PEER loop reads the peer's ring, which the
		 * arm does not clear. */
		if (mode == UDSP_LOOP_OFF) {
			chan_loop_disarm(s, c);
			return 0;
		}
		/* A metallic line test took the line between the control-side
		 * check and this tick. The plug-in drives the wire for the
		 * whole test, so the arm loses; -EBUSY reaches the caller as
		 * UDSP_TRANSIENT (core/cmd.c cmd_status_map). */
		if (atomic_load_explicit(&c->under_test, memory_order_acquire))
			return -EBUSY;
		c->loop_mode = mode;
		c->loop_peer = (uint8_t)((cmd->arg1 >> 8) & 0xffu);
		c->loop_delay = (uint16_t)(cmd->arg1 >> 16);
		chan_loop_ring_clear(s, c->line);
		/* The record the control-side guards read, written after the
		 * fields it describes. */
		atomic_store_explicit(&s->loop_armed[cmd->line], 1u,
				      memory_order_release);
		return 0;
	}
	}

	return -ENOTSUP;
}
