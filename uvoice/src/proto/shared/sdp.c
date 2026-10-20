// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * SDP offer/answer for one audio call leg (RFC 4566, RFC 3264).
 *
 * Every buffer here is caller owned or on the stack: the parser reads spans of
 * the body and the builders append through struct sip_buf, so this file runs
 * unchanged in uvoiced, in test_sdp and in fuzz_sdp.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "proto/shared/sdp.h"

#define SDP_CLOCK_RATE		8000
#define SDP_CLOCK_RATE_OPUS	48000	/* RFC 7587 section 7 */
#define SDP_CN_PT		13
#define SDP_PT_MAX		127
#define SDP_PT_DYNAMIC_MIN	96	/* RFC 3551 section 6 */
#define SDP_PORT_MAX		65535

/*
 * What our own Opus a=fmtp and a=maxptime state, which is what this decoder
 * wants. The remote fmtp is the other record and constrains our encoder
 * instead.
 *
 * 16000 is the highest rate a wideband FXS line renders, 64000 is the average
 * bitrate the jitter buffer payload bound was sized for, and 20 ms is the top
 * of the packet mask this media plane carries for Opus (ptime_mask 0x03).
 *
 * usedtx is never written: this stack exposes no DTX.
 */
#define SDP_OPUS_MAXPLAYBACKRATE	16000
#define SDP_OPUS_MAXAVERAGEBITRATE	64000
#define SDP_OPUS_PTIME_MAX		20

/* RFC 7587 section 6.1: a value outside its key's range is ignored */
#define SDP_OPUS_PLAYBACKRATE_MIN	8000
#define SDP_OPUS_PLAYBACKRATE_MAX	48000
#define SDP_OPUS_BITRATE_MIN		6000
#define SDP_OPUS_BITRATE_MAX		510000

/*
 * The T.38 numbers this side asks for. uvoice has no per-line T.38 tuning, so
 * these defaults are the values. 14400 is the top V.17 rate.
 */
#define SDP_T38_MAX_BIT_RATE	14400
#define SDP_T38_MAX_BUFFER	200
#define SDP_T38_MAX_DATAGRAM	72

static uint16_t sdp_t38_max_datagram = SDP_T38_MAX_DATAGRAM;

/*
 * The lowest rate a T.38 stream can actually run: 2400 bit/s is the lower
 * V.27 ter rate, the slowest image data rate of T.30. A ceiling below it
 * names a stream no DSP can be programmed for, so the reconciliation reads
 * such a value as absent instead of advertising a ceiling the stream would
 * then exceed.
 */
#define SDP_T38_MIN_BIT_RATE	2400

/* base64 of SDP_SRTP_KEY_LEN bytes, and the highest suite the enum names */
#define SDP_SRTP_KEY_B64_LEN	40
#define SDP_SRTP_SUITE_MAX	SDP_SRTP_AES_CM_128_SHA1_32

/* RFC 4568 9.1: the tag is 1 to 9 digits, so it never overflows a uint32_t */
#define SDP_CRYPTO_TAG_DIGITS	9

struct sdp_static_pt {
	uint8_t pt;
	const char *name;
};

/*
 * RFC 3551 table 4. Comfort noise sits with the audio codecs so that an rtpmap
 * can still override payload type 13 (RFC 3264 5.1); desc_finish() moves an
 * entry that is still named CN out of fmt[] and into cn_pt.
 */
static const struct sdp_static_pt static_pt[] = {
	{ 0, "PCMU" },
	{ 8, "PCMA" },
	{ 9, "G722" },
	{ 13, "CN" },
	{ 18, "G729" },
};

const char *sdp_codec_name(enum sdp_codec_id id)
{
	switch (id) {
	case SDP_CODEC_PCMU:
		return "PCMU";
	case SDP_CODEC_PCMA:
		return "PCMA";
	case SDP_CODEC_G729:
		return "G729";
	case SDP_CODEC_G722:
		return "G722";
	case SDP_CODEC_OPUS:
		return "opus";
	case SDP_CODEC_G726_32:
		return "G726-32";
	case SDP_CODEC_NONE:
		break;
	}

	return "none";
}

const char *sdp_dir_name(enum sdp_dir dir)
{
	switch (dir) {
	case SDP_SENDRECV:
		return "sendrecv";
	case SDP_SENDONLY:
		return "sendonly";
	case SDP_RECVONLY:
		return "recvonly";
	case SDP_INACTIVE:
		return "inactive";
	}

	return "unknown";
}

const char *sdp_srtp_suite_name(enum sdp_srtp_suite s)
{
	switch (s) {
	case SDP_SRTP_AES_CM_128_SHA1_80:
		return "AES_CM_128_HMAC_SHA1_80";
	case SDP_SRTP_AES_CM_128_SHA1_32:
		return "AES_CM_128_HMAC_SHA1_32";
	case SDP_SRTP_NONE:
		break;
	}

	return "none";
}

/*
 * RFC 4648 4, the base64 alphabet, with no line breaks anywhere. libubox has a
 * codec, and this tier may not call it: sdp.c is a libc-only source that both
 * fuzz targets compile without libubox, and their link line is the assertion.
 */
static const char base64_alpha[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_val_lookup(char c)
{
	const char *p = memchr(base64_alpha, c, sizeof(base64_alpha) - 1);

	return p ? (int)(p - base64_alpha) : -1;
}

/*
 * A key is 30 bytes, a multiple of three, so the encoding is 40 characters and
 * needs no padding at all (RFC 4648 4: padding fills a partial group).
 */
static void key_b64_encode(char *dst, const uint8_t *key)
{
	unsigned int i, o;
	uint32_t v;

	for (i = 0; i < SDP_SRTP_KEY_LEN; i += 3) {
		v = ((uint32_t)key[i] << 16) | ((uint32_t)key[i + 1] << 8) | key[i + 2];
		o = i / 3 * 4;
		dst[o] = base64_alpha[(v >> 18) & 0x3f];
		dst[o + 1] = base64_alpha[(v >> 12) & 0x3f];
		dst[o + 2] = base64_alpha[(v >> 6) & 0x3f];
		dst[o + 3] = base64_alpha[v & 0x3f];
	}
	dst[SDP_SRTP_KEY_B64_LEN] = 0;
}

/*
 * The decode this layer needs answers one question: are these exactly the 30
 * bytes of a master key and salt? A character outside the alphabet, a length
 * that leaves bits over, and any other byte count are all the same answer.
 * Trailing '=' is tolerated because a peer may pad a group it did not have to
 * pad, and it changes nothing: the byte count still has to come out at 30.
 */
static bool key_b64_decode(uint8_t *key, struct sip_str s)
{
	unsigned int bits = 0, n = 0;
	uint32_t acc = 0;
	size_t i;
	int d;

	for (i = 0; i < s.len && s.p[i] != '='; i++) {
		d = base64_val_lookup(s.p[i]);
		if (d < 0)
			return false;
		acc = (acc << 6) | (uint32_t)d;
		bits += 6;
		if (bits < 8)
			continue;
		bits -= 8;
		if (n >= SDP_SRTP_KEY_LEN)
			return false;
		key[n++] = (uint8_t)(acc >> bits);
	}

	for (; i < s.len; i++) {
		if (s.p[i] != '=')
			return false;
	}

	return !bits && n == SDP_SRTP_KEY_LEN;
}

/*
 * The RTP clock of one codec. Every narrowband codec clocks at 8000 and so
 * does G.722 (RFC 3551 section 4.5.2: its rtpmap rate stays 8000 for
 * historical reasons while the coder samples at 16 kHz). Opus clocks at 48000
 * for every mode and every internal sampling rate (RFC 7587 section 7).
 */
static uint32_t codec_clock_rate(enum sdp_codec_id id)
{
	switch (id) {
	case SDP_CODEC_PCMU:
	case SDP_CODEC_PCMA:
	case SDP_CODEC_G729:
	case SDP_CODEC_G722:
	case SDP_CODEC_G726_32:
	case SDP_CODEC_NONE:
		return SDP_CLOCK_RATE;
	case SDP_CODEC_OPUS:
		return SDP_CLOCK_RATE_OPUS;
	}

	return 0;
}

/* the upper-cased rtpmap encoding names this side answers to */
static enum sdp_codec_id codec_name_lookup(const char *name)
{
	if (!strcmp(name, "PCMU"))
		return SDP_CODEC_PCMU;
	if (!strcmp(name, "PCMA"))
		return SDP_CODEC_PCMA;
	if (!strcmp(name, "G722"))
		return SDP_CODEC_G722;
	if (!strcmp(name, "OPUS"))
		return SDP_CODEC_OPUS;
	if (!strcmp(name, "G726-32"))
		return SDP_CODEC_G726_32;

	/* deployed gateways name the annex variants of G.729 in the rtpmap */
	if (!strcmp(name, "G729") || !strcmp(name, "G729A") || !strcmp(name, "G729B"))
		return SDP_CODEC_G729;

	return SDP_CODEC_NONE;
}

/*
 * The rate gate. A format is this side's only when the rtpmap names the clock
 * that codec runs on, so G722/16000, PCMU/16000 and opus/8000 all name streams
 * this side cannot key and none of them is selected.
 *
 * The channel count is not a gate. RFC 7587 section 7 makes opus/48000/2 the
 * one legal spelling and every writer of ours uses it, but an Opus packet says
 * for itself how many channels it carries, so a peer that wrote /1 still
 * describes a stream this side decodes. The tolerant parse keeps it.
 */
static enum sdp_codec_id codec_id_lookup(const char *name, uint32_t rate)
{
	enum sdp_codec_id id = codec_name_lookup(name);

	if (id == SDP_CODEC_NONE || rate != codec_clock_rate(id))
		return SDP_CODEC_NONE;

	return id;
}

/*
 * The schema value 129 disables the codec, so it never reaches the wire, and a
 * payload type past 127 is outside the range RFC 3551 lets an m= line name.
 *
 * It is exported because codec_add() in media_fill.c runs the same gate. With
 * a test of the HAL mask alone, a HAL that advertised a codec this predicate
 * refuses would seat an entry in the local list that the offer builder then
 * skips, and the list and the wire would disagree about what this line
 * offers. One predicate, both sides, every codec.
 */
bool sdp_codec_offerable(const struct sdp_codec *c)
{
	if (!c)
		return false;

	return c->id != SDP_CODEC_NONE && c->id <= SDP_CODEC_G726_32 &&
	       c->pt <= SDP_PT_MAX;
}

static unsigned int codec_count(const struct sdp_local *loc)
{
	return loc->codecs < SDP_CODEC_MAX ? loc->codecs : SDP_CODEC_MAX;
}

/* the two payload types an offer writes beside its codecs, -1 when it writes none */
static int offer_dtmf_pt(const struct sdp_local *loc)
{
	return loc->dtmf_pt ? (int)loc->dtmf_pt : -1;
}

/*
 * Opus never pairs with comfort noise. RFC 3389 carries CN at payload type 13
 * on the 8000 clock, one SSRC carries one clock (RFC 7160), and
 * an Opus stream clocks at 48000, so there is no comfort noise this side could
 * seat beside it. G.711, G.729, G.722 and G.726-32 pair with comfort noise.
 */
static bool cn_pairs_with(enum sdp_codec_id id)
{
	switch (id) {
	case SDP_CODEC_PCMU:
	case SDP_CODEC_PCMA:
	case SDP_CODEC_G729:
	case SDP_CODEC_G722:
	case SDP_CODEC_G726_32:
		return true;
	case SDP_CODEC_OPUS:
	case SDP_CODEC_NONE:
		break;
	}

	return false;
}

/*
 * The payload type collision rule of an offer. One offer writes one rtpmap per
 * usable codec plus telephone-event and comfort noise. One payload type names
 * one format (RFC 3264 section 5.1), so two rtpmap lines under one type leave
 * a stream no peer can demultiplex: it picks one of the two and this side
 * sends the other.
 *
 * The codec loses, always. telephone-event and comfort noise are not codecs and
 * keep their configured types, and between two codecs the one our priority
 * order names first keeps the type. Dropping one format still leaves a stream
 * both sides can carry, while an ambiguous type leaves one neither can.
 *
 * A dropped codec is off the line, not just off the offer. codec_select()
 * runs the same predicate, so this side can
 * neither answer with a codec its own offer withheld nor take one back from a
 * peer. Selecting it would seat a format on a payload type this offer already
 * gave to telephone-event, to comfort noise or to another codec, and the two
 * ends would then disagree about what that type carries with nothing on the
 * wire to say so.
 */
static const char *pt_owner_of(const struct sdp_local *loc, unsigned int idx, int dtmf, int cn)
{
	unsigned int i;
	uint8_t pt;

	if (idx >= codec_count(loc) || !sdp_codec_offerable(&loc->codec[idx]))
		return NULL;

	pt = loc->codec[idx].pt;
	if (dtmf == (int)pt)
		return "telephone-event";
	if (cn == (int)pt)
		return "CN";

	for (i = 0; i < idx; i++) {
		if (!sdp_codec_offerable(&loc->codec[i]) || loc->codec[i].pt != pt)
			continue;

		return sdp_codec_name((enum sdp_codec_id)loc->codec[i].id);
	}

	return NULL;
}

/* the first codec an offer built with these two extra types would write */
static const struct sdp_codec *codec_first_of(const struct sdp_local *loc, int dtmf, int cn)
{
	unsigned int i;

	for (i = 0; i < codec_count(loc); i++) {
		if (!sdp_codec_offerable(&loc->codec[i]) || pt_owner_of(loc, i, dtmf, cn))
			continue;

		return &loc->codec[i];
	}

	return NULL;
}

/*
 * Comfort noise is withdrawn when the stream would be an Opus one. The verdict
 * cannot ride the collision rule directly, because that rule already reads the
 * comfort noise type, so the resolution runs once with CN offered and the
 * winner then decides whether CN is written at all.
 *
 * A config that pushes a narrowband codec onto payload type 13 with comfort
 * noise on and names Opus behind it therefore ends with no comfort noise and
 * the narrowband codec back in the offer: the first pass drops that codec to
 * CN, the winner is Opus, CN goes, and the second resolution has type 13 free
 * again. No comfort noise is the safe end of that config either way.
 *
 * A list whose every codec loses its type keeps comfort noise offered, so the
 * collision report still names the codec that lost payload type 13 and the
 * offer still fails for having no codec. Withdrawing CN there would take the
 * drop back and change a verdict that has nothing to do with Opus.
 */
static int offer_cn_pt(const struct sdp_local *loc)
{
	const struct sdp_codec *top;

	if (!loc->cn)
		return -1;

	top = codec_first_of(loc, offer_dtmf_pt(loc), SDP_CN_PT);
	if (top && !cn_pairs_with((enum sdp_codec_id)top->id))
		return -1;

	return SDP_CN_PT;
}

const char *sdp_offer_pt_owner(const struct sdp_local *loc, unsigned int idx)
{
	if (!loc)
		return NULL;

	return pt_owner_of(loc, idx, offer_dtmf_pt(loc), offer_cn_pt(loc));
}

/* the codecs an offer writes: enabled, in range, and holding their own type */
static bool offer_codec_is(const struct sdp_local *loc, unsigned int idx)
{
	return sdp_codec_offerable(&loc->codec[idx]) && !sdp_offer_pt_owner(loc, idx);
}

static const struct sdp_codec *codec_first(const struct sdp_local *loc)
{
	return codec_first_of(loc, offer_dtmf_pt(loc), offer_cn_pt(loc));
}

/*
 * The clock a second telephone-event line of this offer would serve, 0 when
 * every codec the offer writes runs on one clock. Two clocks exist in this
 * vocabulary, so the first codec that disagrees with the leading one names it.
 */
static uint32_t offer_alt_clock(const struct sdp_local *loc, uint32_t clock)
{
	unsigned int i;
	uint32_t hz;

	for (i = 0; i < codec_count(loc); i++) {
		if (!offer_codec_is(loc, i))
			continue;

		hz = codec_clock_rate((enum sdp_codec_id)loc->codec[i].id);
		if (hz != clock)
			return hz;
	}

	return 0;
}

static bool pt_claimed_is(const struct sdp_local *loc, uint8_t pt)
{
	unsigned int i;

	for (i = 0; i < codec_count(loc); i++) {
		if (sdp_codec_offerable(&loc->codec[i]) && loc->codec[i].pt == pt)
			return true;
	}

	return false;
}

/*
 * The payload type of the second telephone-event line. One m= line carries one
 * event stream per RTP clock (RFC 4733 section 2.1), so an offer that mixes
 * clock families has to advertise both: without the second line a peer that
 * answers the narrowband codec of an Opus-led offer has no event type it may
 * legally select, and anything it names anyway is a stream element this offer
 * never carried.
 *
 * The configured type stays with the first codec's clock, so a single-family
 * offer writes exactly one line. The spare is the highest free dynamic type:
 * the walk starts at 127 and takes the first type that no codec entry of this
 * list, no comfort noise line and not the configured event type already
 * claims. Every entry of the list is skipped, offered or dropped, so no
 * reading of the collision rule can make the spare ambiguous. -1 means the
 * dynamic range is full, which no config this schema admits can reach.
 */
static int offer_dtmf_pt_alt(const struct sdp_local *loc, int dtmf, int cn)
{
	int pt;

	for (pt = SDP_PT_MAX; pt >= SDP_PT_DYNAMIC_MIN; pt--) {
		if (pt == dtmf || pt == cn || pt_claimed_is(loc, (uint8_t)pt))
			continue;

		return pt;
	}

	return -1;
}

/*
 * The event payload type our pending offer advertised for one clock, -1 when it
 * advertised none, read from what the offer builder recorded and never
 * recomputed from struct sdp_local (shared/sdp.h, sdp_state.offered). It is the
 * whole of the take-side half of the invariant: nothing lands in a result that
 * the offer did not put on the wire.
 */
static int offered_dtmf_pt(const struct sdp_state *st, uint32_t clock)
{
	if (st->offered.dtmf_pt >= 0 && clock == st->offered.clock)
		return st->offered.dtmf_pt;
	if (st->offered.alt_pt >= 0 && clock == st->offered.alt_clock)
		return st->offered.alt_pt;

	return -1;
}

/* every audio stream element of the pending offer, cleared or recorded */
static void offered_reset(struct sdp_state *st)
{
	memset(&st->offered, 0, sizeof(st->offered));
	st->offered.dtmf_pt = -1;
	st->offered.alt_pt = -1;
	st->offered.cn_pt = -1;
	st->offered.g729_pt = -1;
}

/*
 * struct sdp_local is caller owned and its address array may be filled to the
 * last byte, so every read of it is bounded instead of trusting a terminator.
 */
static void addr_take(char dst[SDP_ADDR_MAX], const char *src)
{
	size_t n = strnlen(src, SDP_ADDR_MAX - 1);

	memcpy(dst, src, n);
	dst[n] = 0;
}

/*
 * The address family of one host text: 4 for an IPv4 literal, 6 for an IPv6 one
 * and 0 for text that is neither. inet_pton() names the family of both forms,
 * so nothing here reads the text for a colon and guesses.
 */
static uint8_t addr_ipver(const char *addr)
{
	struct in_addr v4;
	struct in6_addr v6;

	if (inet_pton(AF_INET, addr, &v4) == 1)
		return 4;
	if (inet_pton(AF_INET6, addr, &v6) == 1)
		return 6;

	return 0;
}

/*
 * The hold address of a description. RFC 4566 names no IPv6 hold address, and
 * the unspecified address is what a peer that means the RFC 2543 IPv4 hold
 * writes in an IP6 session. "::" and "::0" are the one address under two
 * spellings, so the test parses the text instead of comparing it.
 */
static bool addr_zero_is(const char *addr)
{
	struct in6_addr v6;

	if (!strcmp(addr, "0.0.0.0"))
		return true;
	if (inet_pton(AF_INET6, addr, &v6) != 1)
		return false;

	return !memcmp(&v6, &in6addr_any, sizeof(v6));
}

/*
 * A c= host that is neither an IPv4 nor an IPv6 literal, an FQDN for
 * instance, names a stream the address layer can never program. Taking such a
 * description gives the ring-answer-drop shape: the leg rings, it answers 200,
 * and the media fill then refuses the same address and BYEs the call.
 * Refusing it here answers 488 instead. The two layers compose: the media
 * fill refusal stays as the backstop for an address this layer never
 * produced.
 */
static bool addr_literal_is(const char *addr)
{
	return addr_ipver(addr) != 0;
}

static const char *addr_ipver_name(const char *addr)
{
	return addr_ipver(addr) == 6 ? "IP6" : "IP4";
}

/*
 * The family of our own media address, and 0 when this side holds no literal of
 * its own. struct sdp_local is caller owned and its address array may be filled
 * to the last byte, so this read is bounded the way addr_take() bounds every
 * other read of it.
 */
static uint8_t local_ipver(const char *addr)
{
	char text[SDP_ADDR_MAX];

	if (!addr)
		return 0;

	addr_take(text, addr);

	return addr_ipver(text);
}

/*
 * Does a section address run in the family of our own media address?
 *
 * A local address that is no literal names no family. This function then
 * answers true, and every verdict of such a leg stays what it was. An empty
 * local address is the wildcard bind the call layer calls legitimate. A caller
 * that holds no address of its own has nothing to compare against.
 */
static bool local_family_ok(uint8_t ver, const char *local_addr)
{
	uint8_t ours = local_ipver(local_addr);

	return !ours || ver == ours;
}

/* parse side */

static bool str_u64(struct sip_str s, uint64_t *out)
{
	uint64_t v = 0;
	size_t i;

	if (!s.len || s.len > 20)
		return false;

	for (i = 0; i < s.len; i++) {
		if (!isdigit((unsigned char)s.p[i]))
			return false;
		if (v > (UINT64_MAX - (uint64_t)(s.p[i] - '0')) / 10)
			return false;
		v = v * 10 + (uint64_t)(s.p[i] - '0');
	}
	*out = v;

	return true;
}

static bool line_next(struct sip_str *rest, struct sip_str *line)
{
	const char *nl;
	size_t n;

	if (!rest->len || !rest->p)
		return false;

	nl = memchr(rest->p, '\n', rest->len);
	n = nl ? (size_t)(nl - rest->p) : rest->len;
	*line = sip_str_init(rest->p, n);
	rest->p += nl ? n + 1 : n;
	rest->len -= nl ? n + 1 : n;

	/* a bare LF body is as common on the wire as CRLF (RFC 4566 5) */
	if (line->len && line->p[line->len - 1] == '\r')
		line->len--;

	return true;
}

/*
 * An m= port may carry a port count ("5004/2"), which uvoice does not use. The
 * return value says whether the port was readable, because a port of 0 that the
 * peer wrote (RFC 3264 8.4, the stream is withdrawn) and a port this layer could
 * not read both leave *out at 0, and the T.38 acceptance gate has to tell them
 * apart: only the first is a peer withdrawing its audio on purpose.
 */
static bool port_take(struct sip_str w, uint16_t *out)
{
	uint32_t v;
	size_t i;

	for (i = 0; i < w.len && w.p[i] != '/'; i++)
		;
	w.len = i;
	*out = 0;
	if (!sip_str_uint(w, &v) || v > SDP_PORT_MAX)
		return false;

	*out = (uint16_t)v;

	return true;
}

static void name_upper_take(char *dst, size_t dlen, struct sip_str s)
{
	size_t i, n = s.len < dlen - 1 ? s.len : dlen - 1;

	for (i = 0; i < n; i++)
		dst[i] = (char)toupper((unsigned char)s.p[i]);
	dst[n] = 0;
}

static void desc_init(struct sdp_media_desc *m)
{
	memset(m, 0, sizeof(*m));
	m->fmt_first = -1;
	m->dtmf_pt = -1;
	m->dtmf_pt_wb = -1;
	m->cn_pt = -1;
	m->opus_fmtp_pt = -1;
}

/* where this section numbered pt in fmt[], or -1 when it named none */
static int fmt_idx(const struct sdp_media_desc *m, uint8_t pt)
{
	unsigned int i;

	for (i = 0; i < m->fmts; i++) {
		if (m->fmt[i].pt == pt)
			return (int)i;
	}

	return -1;
}

/*
 * RFC 4733 section 2.1: a telephone-event stream runs on the RTP clock of the
 * audio it rides, so the selected codec decides which of the two payload types
 * the peer may have named is the usable one.
 */
static int desc_dtmf_pt(const struct sdp_media_desc *m, uint32_t clock)
{
	return clock == SDP_CLOCK_RATE_OPUS ? m->dtmf_pt_wb : m->dtmf_pt;
}

static void fmt_add(struct sdp_media_desc *m, uint8_t pt)
{
	size_t i;

	if (m->fmts >= SDP_CODEC_MAX) {
		m->fmt_dropped = true;
		return;
	}

	/*
	 * A payload type listed twice keeps its first entry. One payload type
	 * carries one format (RFC 3264 section 5.1),
	 * so a second entry would only hand a later a=rtpmap a row to overwrite
	 * and would make the answer selection walk a preference the peer stated
	 * once. It also spends a fmt[] slot a real format needs.
	 */
	for (i = 0; i < m->fmts; i++) {
		if (m->fmt[i].pt == pt)
			return;
	}

	m->fmt[m->fmts].pt = pt;
	m->fmt[m->fmts].rate = 0;
	m->fmt[m->fmts].name[0] = 0;
	m->fmt[m->fmts].annexb = false;
	m->fmt[m->fmts].annexb_set = false;

	for (i = 0; i < sizeof(static_pt) / sizeof(static_pt[0]); i++) {
		if (static_pt[i].pt != pt)
			continue;
		snprintf(m->fmt[m->fmts].name, SDP_FMT_NAME_MAX, "%s", static_pt[i].name);
		m->fmt[m->fmts].rate = SDP_CLOCK_RATE;
		break;
	}
	m->fmts++;
}

static void fmt_remove_at(struct sdp_media_desc *m, unsigned int i)
{
	memmove(&m->fmt[i], &m->fmt[i + 1], (m->fmts - i - 1) * sizeof(m->fmt[0]));
	m->fmts--;
}

/* telephone-event and CN are not audio codecs, so they never stay in fmt[] */
static void fmt_drop(struct sdp_media_desc *m, uint8_t pt)
{
	unsigned int i;

	for (i = 0; i < m->fmts; i++) {
		if (m->fmt[i].pt != pt)
			continue;
		fmt_remove_at(m, i);
		return;
	}
}

/* a fmt entry still named CN kept the static meaning of payload type 13 */
static void cn_fmt_move(struct sdp_media_desc *m)
{
	unsigned int i = 0;

	while (i < m->fmts) {
		if (strcmp(m->fmt[i].name, "CN")) {
			i++;
			continue;
		}
		if (m->cn_pt < 0)
			m->cn_pt = m->fmt[i].pt;
		fmt_remove_at(m, i);
	}
}

struct sdp_parse_ctx {
	struct sdp_msg *msg;
	struct sdp_media_desc *cur;	/* NULL until the first m= line */
	struct sdp_media_desc sink;	/* every m= section uvoice ignores */
	char addr[SDP_ADDR_MAX];
	/*
	 * Every payload type the current m= line listed, including the ones
	 * past the fmt[] cap: an a=rtpmap for a type the m= line never offered
	 * must change nothing (RFC 4566 6, the rtpmap maps a listed type).
	 */
	uint8_t pt_seen[(SDP_PT_MAX + 8) / 8];
	/*
	 * Every payload type an a=rtpmap of the current m= line already
	 * claimed. The first claim wins (the duplicate rule of fmt_add applied
	 * one line further on): one payload type names one format (RFC 3264
	 * section 5.1), so a second rtpmap for a type is dropped. Without it a
	 * body that maps one type at both event clocks seats that type as the
	 * narrowband event and as the wideband one, and
	 * an answer then selects an event stream the m= line never described.
	 */
	uint8_t pt_mapped[(SDP_PT_MAX + 8) / 8];
	uint8_t ipver;
	uint8_t dir;
	bool dir_present;
	uint8_t ptime_ms;
	/*
	 * The session level a=rtcp record. RFC 3605 defines the attribute at
	 * media level, so this one is tolerance and desc_finish() hands it to
	 * the audio section alone.
	 */
	uint16_t rtcp_port;
	char rtcp_addr[SDP_ADDR_MAX];
	uint8_t rtcp_ipver;
	bool have_v;
	bool have_m;
	bool audio_seen;
	bool image_seen;
};

static void pt_seen_set(struct sdp_parse_ctx *ctx, uint8_t pt)
{
	ctx->pt_seen[pt / 8] |= (uint8_t)(1u << (pt % 8));
}

static bool pt_seen_is(const struct sdp_parse_ctx *ctx, uint8_t pt)
{
	return (ctx->pt_seen[pt / 8] & (uint8_t)(1u << (pt % 8))) != 0;
}

/* the first a=rtpmap for a payload type claims it; a later one is dropped */
static bool pt_mapped_take(struct sdp_parse_ctx *ctx, uint8_t pt)
{
	if (ctx->pt_mapped[pt / 8] & (uint8_t)(1u << (pt % 8)))
		return false;

	ctx->pt_mapped[pt / 8] |= (uint8_t)(1u << (pt % 8));

	return true;
}

static void origin_parse(struct sdp_parse_ctx *ctx, struct sip_str val)
{
	struct sip_str w;

	if (!sip_word_take(&val, &w))
		return;
	if (!sip_word_take(&val, &w) || !str_u64(w, &ctx->msg->sess_id))
		ctx->msg->sess_id = 0;
	if (!sip_word_take(&val, &w) || !str_u64(w, &ctx->msg->sess_ver))
		ctx->msg->sess_ver = 0;
}

static void conn_parse(struct sdp_parse_ctx *ctx, struct sip_str val)
{
	struct sip_str nettype, addrtype, addr;
	char *dst = ctx->cur ? ctx->cur->addr : ctx->addr;
	uint8_t ver;

	if (!sip_word_take(&val, &nettype) || !sip_word_take(&val, &addrtype))
		return;
	if (!sip_word_take(&val, &addr))
		return;

	ver = sip_str_eq_ci(addrtype, "IP6") ? 6 : 4;
	if (!sip_str_copy(dst, SDP_ADDR_MAX, addr))
		dst[0] = 0;

	if (ctx->cur)
		ctx->cur->ipver = ver;
	else
		ctx->ipver = ver;
}

static void media_parse(struct sdp_parse_ctx *ctx, struct sip_str val)
{
	struct sip_str kind, w;
	struct sdp_media_desc *m = &ctx->sink;
	bool image = false;
	uint32_t v;

	ctx->have_m = true;
	desc_init(&ctx->sink);
	ctx->cur = &ctx->sink;
	memset(ctx->pt_seen, 0, sizeof(ctx->pt_seen));
	memset(ctx->pt_mapped, 0, sizeof(ctx->pt_mapped));

	if (!sip_word_take(&val, &kind))
		return;

	/* one FXS line carries one audio stream, so a second m=audio is noise */
	if (sip_str_eq_ci(kind, "audio") && !ctx->audio_seen) {
		m = &ctx->msg->audio;
		ctx->audio_seen = true;
	} else if (sip_str_eq_ci(kind, "image") && !ctx->image_seen) {
		m = &ctx->msg->image;
		ctx->image_seen = true;
		image = true;

		/* RFC 3264 6: the answer mirrors the offer's m= line order */
		ctx->msg->image_first = !ctx->audio_seen;
	} else if (sip_str_eq_ci(kind, "audio")) {
		ctx->msg->audio_dup = true;
	} else if (sip_str_eq_ci(kind, "image")) {
		ctx->msg->image_dup = true;
	}

	ctx->cur = m;
	m->present = true;

	if (!sip_word_take(&val, &w))
		return;
	m->port_ok = port_take(w, &m->port);

	if (!sip_word_take(&val, &w))
		return;

	/*
	 * ITU-T T.38 Annex D carries an image stream over udptl, and this side
	 * takes no other proto for it. The audio section stays RTP/AVP only, so
	 * an "m=audio ... udptl" line reads unusable and the answer refuses it,
	 * the way SRTP and every other transport does.
	 */
	m->proto_ok = sip_str_eq_ci(w, image ? "udptl" : "RTP/AVP");

	/*
	 * RFC 4568 5: SDES rides RTP/SAVP. It is a second flag and not a
	 * proto_ok value, so every gate that means PLAIN RTP keeps reading the
	 * field it always read. RTP/SAVPF and UDP/TLS/RTP/SAVP are neither:
	 * this side negotiates neither of them.
	 */
	m->savp = sip_str_eq_ci(w, "RTP/SAVP");

	while (sip_word_take(&val, &w)) {
		/* T.38 Annex D: the fmt list of an image section is t38 */
		if (image && sip_str_eq_ci(w, "t38")) {
			m->t38_fmt = true;
			continue;
		}
		if (!sip_str_uint(w, &v) || v > SDP_PT_MAX)
			continue;
		pt_seen_set(ctx, (uint8_t)v);
		fmt_add(m, (uint8_t)v);

		/* before the CN and telephone-event moves empty fmt[] again */
		if (m->fmt_first < 0)
			m->fmt_first = (int)v;
	}
}

static void rtpmap_parse(struct sdp_parse_ctx *ctx, struct sip_str val)
{
	struct sdp_media_desc *m = ctx->cur;
	char name[SDP_FMT_NAME_MAX];
	struct sip_str w, enc, rate;
	uint32_t pt, hz = 0;
	unsigned int i;
	size_t at;

	if (!sip_word_take(&val, &w) || !sip_str_uint(w, &pt) || pt > SDP_PT_MAX)
		return;
	if (!pt_seen_is(ctx, (uint8_t)pt))
		return;
	if (!sip_word_take(&val, &w))
		return;

	enc = w;
	rate = sip_str_init(NULL, 0);
	for (at = 0; at < w.len; at++) {
		if (w.p[at] != '/')
			continue;
		enc = sip_str_init(w.p, at);
		rate = sip_str_init(w.p + at + 1, w.len - at - 1);
		break;
	}

	/* an encoding parameter (".../8000/1") is not part of the clock rate */
	for (at = 0; at < rate.len; at++) {
		if (rate.p[at] == '/') {
			rate.len = at;
			break;
		}
	}
	if (!sip_str_uint(rate, &hz))
		hz = 0;

	name_upper_take(name, sizeof(name), enc);

	/*
	 * An empty encoding name is a line this layer could not read, and
	 * "a=rtpmap:96 /8000" is exactly that shape: the rate split leaves the
	 * name empty and there is no format to map the payload type to. It
	 * takes no claim and writes nothing, so a valid rtpmap for the same type
	 * behind it still maps it.
	 */
	if (!name[0])
		return;

	/*
	 * The first claim wins. The claim is taken here and not at the payload
	 * type, so a line this layer could not read the encoding name of blocks
	 * nothing and the tolerant contract stands: a garbled a=rtpmap beside a
	 * good one for the same type still maps it.
	 */
	if (!pt_mapped_take(ctx, (uint8_t)pt))
		return;

	/*
	 * Neither one is an audio codec, so both leave fmt[] either way. A rate
	 * no stream of ours runs on makes the payload type unusable: it is then
	 * dropped without becoming the negotiated DTMF or comfort noise type,
	 * which is what keeps telephone-event/16000 refused.
	 *
	 * telephone-event has two usable clocks and comfort noise one. RFC 3389
	 * carries CN at 8000 and Opus never pairs with it, so CN/48000 stays a
	 * refusal.
	 */
	if (!strcmp(name, "TELEPHONE-EVENT")) {
		fmt_drop(m, (uint8_t)pt);
		if (hz == SDP_CLOCK_RATE)
			m->dtmf_pt = (int)pt;
		else if (hz == SDP_CLOCK_RATE_OPUS)
			m->dtmf_pt_wb = (int)pt;
		return;
	}
	if (!strcmp(name, "CN")) {
		fmt_drop(m, (uint8_t)pt);
		if (hz == SDP_CLOCK_RATE)
			m->cn_pt = (int)pt;
		return;
	}

	/* RFC 3264 5.1: an rtpmap overrides the static meaning of a payload type */
	for (i = 0; i < m->fmts; i++) {
		if (m->fmt[i].pt != pt)
			continue;
		snprintf(m->fmt[i].name, SDP_FMT_NAME_MAX, "%s", name);
		m->fmt[i].rate = hz;
		return;
	}
}

/*
 * One "key=value" pair of an a=fmtp parameter list (RFC 7587 section 7: a
 * semicolon separated list of parameter=value pairs). Space around a pair is
 * tolerated because the RFC's own example writes "; " between them. A pair
 * without a '=' yields an empty value, which no key below accepts.
 */
static bool fmtp_pair_take(struct sip_str *rest, struct sip_str *key, struct sip_str *val)
{
	size_t i;

	*rest = sip_str_trim(*rest);
	if (!rest->len)
		return false;

	for (i = 0; i < rest->len && rest->p[i] != ';'; i++)
		;
	*key = sip_str_trim(sip_str_init(rest->p, i));
	i = i < rest->len ? i + 1 : i;
	*rest = sip_str_init(rest->p + i, rest->len - i);

	*val = sip_str_init(NULL, 0);
	for (i = 0; i < key->len; i++) {
		if (key->p[i] != '=')
			continue;
		*val = sip_str_trim(sip_str_init(key->p + i + 1, key->len - i - 1));
		*key = sip_str_trim(sip_str_init(key->p, i));
		break;
	}

	return true;
}

/*
 * One RFC 7587 6.1 key of a remote a=fmtp. The return says whether the key is
 * one this layer knows, not whether its value was kept: a value outside the
 * range the RFC gives is dropped, as the RFC asks, and the encoder then keeps
 * its configured setting, but the line is still an Opus fmtp.
 *
 * usedtx, stereo, cbr, sprop-maxcapturerate and sprop-stereo are read and
 * discarded. usedtx is the one that matters: this stack exposes no DTX at
 * all, so an inbound one changes nothing.
 */
static bool opus_fmtp_key_take(struct sdp_opus_fmtp *f, struct sip_str key, uint32_t n)
{
	if (sip_str_eq_ci(key, "maxplaybackrate")) {
		if (n >= SDP_OPUS_PLAYBACKRATE_MIN && n <= SDP_OPUS_PLAYBACKRATE_MAX)
			f->maxplaybackrate = n;
		return true;
	}

	if (sip_str_eq_ci(key, "maxaveragebitrate")) {
		if (n >= SDP_OPUS_BITRATE_MIN && n <= SDP_OPUS_BITRATE_MAX)
			f->maxaveragebitrate = n;
		return true;
	}

	if (sip_str_eq_ci(key, "useinbandfec") && n <= 1) {
		f->fec = n != 0;
		f->fec_present = true;
		return true;
	}

	return false;
}

static bool opus_fmtp_take(struct sdp_opus_fmtp *f, struct sip_str val)
{
	struct sip_str key, v;
	bool known = false;
	uint32_t n;

	while (fmtp_pair_take(&val, &key, &v)) {
		if (!sip_str_uint(v, &n))
			continue;

		known |= opus_fmtp_key_take(f, key, n);
	}

	return known;
}

/*
 * RFC 3555 4.1.9: the one key of a remote G.729 a=fmtp. The return says
 * whether the line named annexb with a value of exactly "yes" or "no": a
 * missing key or any other value is a line this side cannot read, and it
 * changes nothing, which is the same tolerant contract opus_fmtp_key_take()
 * runs.
 */
static bool g729_fmtp_take(struct sip_str val, bool *annexb)
{
	struct sip_str key, v;
	bool found = false;

	while (fmtp_pair_take(&val, &key, &v)) {
		if (!sip_str_eq_ci(key, "annexb"))
			continue;
		if (sip_str_eq_ci(v, "yes"))
			*annexb = true;
		else if (sip_str_eq_ci(v, "no"))
			*annexb = false;
		else
			continue;
		found = true;
	}

	return found;
}

/*
 * a=fmtp:<pt> <parameters> (RFC 4566 section 6). The Opus record is one per
 * section and the annexb record is one per FORMAT, and desc_finish() then
 * keeps each only where that payload type is a codec of that kind. A body that
 * hangs Opus keys or the annexb key off another format therefore loses the
 * record instead of applying it to a codec it does not describe.
 *
 * The last line for a payload type wins outright, but the two records read
 * that rule differently. The Opus record is replaced whole by a later fmtp
 * for the type that already holds it, even when not one of its values
 * parses, because the peer restated that format and the record has to say
 * what the peer last said. An annexb record takes a later line only when
 * that line's value actually parses ("yes" or "no"): a malformed value is not
 * the peer restating the format, since annexb is its one key, and it leaves
 * whatever record that format already had.
 *
 * A line for another payload type takes a record only when it names a key
 * that record understands, so the telephone-event "0-15" line displaces
 * neither. A line for a payload type the m= list named but fmt[] no longer
 * holds, the telephone-event and CN types the parse already moved out, finds
 * no format to write and is dropped where it stands.
 */
static void fmtp_parse(struct sdp_parse_ctx *ctx, struct sip_str val)
{
	struct sdp_media_desc *m = ctx->cur;
	struct sdp_opus_fmtp f;
	bool annexb;
	struct sip_str w;
	uint32_t pt;
	int idx;

	if (!sip_word_take(&val, &w) || !sip_str_uint(w, &pt) || pt > SDP_PT_MAX)
		return;
	if (!pt_seen_is(ctx, (uint8_t)pt))
		return;

	memset(&f, 0, sizeof(f));
	if (opus_fmtp_take(&f, val) || m->opus_fmtp_pt == (int)pt) {
		m->opus_fmtp_pt = (int)pt;
		m->opus_fmtp = f;
	}

	idx = fmt_idx(m, (uint8_t)pt);
	if (idx >= 0 && g729_fmtp_take(val, &annexb)) {
		m->fmt[idx].annexb = annexb;
		m->fmt[idx].annexb_set = true;
	}
}

/* the stored a=fmtp belongs to the section's Opus payload type or to nobody */
static void opus_fmtp_settle(struct sdp_media_desc *m)
{
	unsigned int i;

	for (i = 0; i < m->fmts && m->opus_fmtp_pt >= 0; i++) {
		if (m->fmt[i].pt != (uint8_t)m->opus_fmtp_pt)
			continue;
		if (codec_id_lookup(m->fmt[i].name, m->fmt[i].rate) == SDP_CODEC_OPUS)
			return;
		break;
	}

	m->opus_fmtp_pt = -1;
	memset(&m->opus_fmtp, 0, sizeof(m->opus_fmtp));
}

/*
 * An annexb record belongs to a G.729 format or to nobody, the same settle
 * opus_fmtp_settle() runs and per format rather than per section.
 * codec_id_lookup() covers both shapes the payload type may take: the static
 * PT 18 fmt_add() already named "G729" when no rtpmap overrode it, and a
 * dynamic PT whose rtpmap named G729, G729A or G729B (codec_name_lookup()
 * folds all three to SDP_CODEC_G729).
 */
static void g729_fmtp_settle(struct sdp_media_desc *m)
{
	unsigned int i;

	for (i = 0; i < m->fmts; i++) {
		if (!m->fmt[i].annexb_set)
			continue;
		if (codec_id_lookup(m->fmt[i].name, m->fmt[i].rate) == SDP_CODEC_G729)
			continue;

		m->fmt[i].annexb = false;
		m->fmt[i].annexb_set = false;
	}
}

static bool dir_lookup(struct sip_str val, uint8_t *out)
{
	if (sip_str_eq_ci(val, "sendrecv"))
		*out = SDP_SENDRECV;
	else if (sip_str_eq_ci(val, "sendonly"))
		*out = SDP_SENDONLY;
	else if (sip_str_eq_ci(val, "recvonly"))
		*out = SDP_RECVONLY;
	else if (sip_str_eq_ci(val, "inactive"))
		*out = SDP_INACTIVE;
	else
		return false;

	return true;
}

/* one "name:value" attribute: on a name match val becomes the trimmed value */
static bool attr_val_take(struct sip_str *val, const char *name)
{
	size_t n = strlen(name);

	if (!sip_str_prefix_ci(*val, name))
		return false;

	*val = sip_str_trim(sip_str_init(val->p + n, val->len - n));

	return true;
}

static void t38_num_take(struct sip_str val, uint32_t *out, bool *present)
{
	uint32_t v;

	if (!sip_str_uint(val, &v))
		return;

	*out = v;
	*present = true;
}

/*
 * ITU-T T.38 Annex D image attributes. Tolerant like the rest of the parse: a
 * value this layer cannot read leaves its field absent, so the attribute
 * costs the answer reconciliation its offered number and never fails the
 * body. A matched name returns at once, so an unreadable value never falls
 * into the next name's test.
 */
static void t38_attr_parse(struct sdp_t38_attrs *t, struct sip_str val)
{
	if (attr_val_take(&val, "T38FaxVersion:")) {
		t38_num_take(val, &t->version, &t->version_present);
		return;
	}
	if (attr_val_take(&val, "T38MaxBitRate:")) {
		t38_num_take(val, &t->max_bit_rate, &t->max_bit_rate_present);
		return;
	}
	if (attr_val_take(&val, "T38FaxMaxBuffer:")) {
		t38_num_take(val, &t->max_buffer, &t->max_buffer_present);
		return;
	}
	if (attr_val_take(&val, "T38FaxMaxDatagram:")) {
		t38_num_take(val, &t->max_datagram, &t->max_datagram_present);
		return;
	}
	if (attr_val_take(&val, "T38FaxRateManagement:")) {
		if (sip_str_eq_ci(val, "localTCF"))
			t->rate_mgmt = SDP_T38_RATE_LOCAL_TCF;
		else if (sip_str_eq_ci(val, "transferredTCF"))
			t->rate_mgmt = SDP_T38_RATE_TRANSFERRED_TCF;
		return;
	}
	if (attr_val_take(&val, "T38FaxUdpEC:")) {
		if (sip_str_eq_ci(val, "t38UDPRedundancy"))
			t->ec = SDP_T38_EC_REDUNDANCY;
		else if (sip_str_eq_ci(val, "t38UDPFEC"))
			t->ec = SDP_T38_EC_FEC;
	}
}

/* RFC 4568 6.2 names the two tokens exactly, so they are matched exactly */
static enum sdp_srtp_suite srtp_suite_lookup(struct sip_str s)
{
	if (sip_str_eq(s, "AES_CM_128_HMAC_SHA1_80"))
		return SDP_SRTP_AES_CM_128_SHA1_80;
	if (sip_str_eq(s, "AES_CM_128_HMAC_SHA1_32"))
		return SDP_SRTP_AES_CM_128_SHA1_32;

	return SDP_SRTP_NONE;
}

/*
 * RFC 4568 9.1 lifetime: ["2^"] 1*DIGIT. The value is ignored, because deployed
 * PBXs declare lifetimes no voice call reaches (2^31 packets is about 497 days
 * at 50 packets a second) and refusing those lines would refuse those peers.
 * The shape is checked, because anything else in that field is not a lifetime
 * at all: a ';' there starts a second key-param and a ':' an MKI, and this
 * layer refuses both, so an unchecked field would carry either one past the
 * refusal that names it.
 */
static bool lifetime_ok(struct sip_str s)
{
	size_t i;

	if (sip_str_prefix(s, "2^"))
		s = sip_str_init(s.p + 2, s.len - 2);
	if (!s.len)
		return false;

	for (i = 0; i < s.len; i++) {
		if (!isdigit((unsigned char)s.p[i]))
			return false;
	}

	return true;
}

/*
 * One key-param (RFC 4568 9.1 key-info): "inline:" base64 ["|" lifetime]
 * ["|" MKI ":" length]. The grammar allows one field behind the key here and
 * it has to be a lifetime. An MKI is a refusal, because struct hal_media
 * carries one key per direction and no MKI, so a stream keyed by a master key
 * index is a stream this side cannot run; a second key-param is a refusal for
 * the same reason. Both ride in behind a '|', so the lifetime shape test above
 * is what keeps them out.
 */
static bool key_param_take(struct sip_str w, uint8_t *key)
{
	size_t at = 0;

	if (!sip_str_prefix_ci(w, "inline:"))
		return false;

	w = sip_str_init(w.p + 7, w.len - 7);
	while (at < w.len && w.p[at] != '|')
		at++;

	if (at < w.len && !lifetime_ok(sip_str_init(w.p + at + 1, w.len - at - 1)))
		return false;

	return key_b64_decode(key, sip_str_init(w.p, at));
}

static bool tag_stored_is(const struct sdp_media_desc *m, uint32_t tag)
{
	unsigned int i;

	for (i = 0; i < m->cryptos && i < SDP_CRYPTO_MAX; i++) {
		if (m->crypto[i].tag == tag)
			return true;
	}

	return false;
}

/*
 * RFC 4568 9.1: "crypto:" tag SP suite SP key-params [SP session-params]. A
 * line this layer cannot honor is dropped and never fails the body, the way
 * every other attribute here is tolerant, so a peer that names one usable
 * suite beside three exotic ones still negotiates.
 */
static void crypto_attr_parse(struct sdp_media_desc *m, struct sip_str val)
{
	struct sdp_crypto c;
	struct sip_str w;

	memset(&c, 0, sizeof(c));

	if (!sip_word_take(&val, &w) || w.len > SDP_CRYPTO_TAG_DIGITS)
		return;
	if (!sip_str_uint(w, &c.tag))
		return;
	if (!sip_word_take(&val, &w))
		return;

	c.suite = (uint8_t)srtp_suite_lookup(w);
	if (c.suite == SDP_SRTP_NONE)
		return;
	if (!sip_word_take(&val, &w) || !key_param_take(w, c.key))
		return;

	/*
	 * RFC 4568 9.1 session-params. struct hal_media carries none of them,
	 * and every one of them changes how the stream is keyed, encrypted or
	 * authenticated: KDR rekeys, the three UNENCRYPTED_/UNAUTHENTICATED_
	 * switches turn a protection off, FEC_ORDER and FEC_KEY name a second
	 * stream, WSH resizes the replay window. A line that names one names a
	 * session this side cannot run, so the line goes.
	 */
	if (sip_word_take(&val, &w))
		return;

	/*
	 * RFC 4568 9.1 gives each line its own tag, and the answer names the
	 * line it chose by tag alone, so the first occurrence of a tag wins and
	 * a later line under that tag is dropped. If the scan could substitute a
	 * duplicate, the offerer would be free to key with either of its two
	 * lines and the media would die one way with nothing to report. A
	 * repeated identical line still negotiates.
	 */
	if (tag_stored_is(m, c.tag))
		return;

	/* the extra lines are dropped and the count stays at the cap */
	if (m->cryptos >= SDP_CRYPTO_MAX)
		return;

	m->crypto[m->cryptos++] = c;
}

/*
 * RFC 3605 section 2.1: "rtcp:" port [space nettype space addrtype space
 * address]. The port is the one the peer receives RTCP on. A port this layer
 * cannot read, a port of 0 and a port past 65535 leave the record absent, which
 * is the tolerant rule every attribute here follows. An address type outside
 * IP4 and IP6, and an address that is not a literal of the type the attribute
 * names, both keep the port and leave the address empty. The RTCP stream then
 * follows the connection address of the section, as it does without the
 * attribute.
 *
 * The record of the current media section, else the session level one, exactly
 * as conn_parse() places a connection address.
 */
static void rtcp_attr_parse(struct sdp_parse_ctx *ctx, struct sip_str val)
{
	uint16_t *port = ctx->cur ? &ctx->cur->rtcp_port : &ctx->rtcp_port;
	char *addr = ctx->cur ? ctx->cur->rtcp_addr : ctx->rtcp_addr;
	uint8_t *ipver = ctx->cur ? &ctx->cur->rtcp_ipver : &ctx->rtcp_ipver;
	struct sip_str w, addrtype;
	uint32_t n;
	uint8_t ver;

	if (!sip_word_take(&val, &w) || !sip_str_uint(w, &n))
		return;
	if (!n || n > SDP_PORT_MAX)
		return;

	*port = (uint16_t)n;
	addr[0] = 0;
	*ipver = 0;

	/* the nettype is read and dropped, the way conn_parse() drops it */
	if (!sip_word_take(&val, &w) || !sip_word_take(&val, &addrtype))
		return;
	if (!sip_word_take(&val, &w))
		return;

	/*
	 * RFC 4566 section 8.2.6 names IP4 and IP6. This record then feeds a
	 * family comparison, so any other address type leaves the address
	 * empty instead of reading as IP4. A guess raises a family conflict
	 * against the section's own address and refuses the offer.
	 * conn_parse() keeps its own tolerance: no second address is compared
	 * against its verdict.
	 */
	if (sip_str_eq_ci(addrtype, "IP4"))
		ver = 4;
	else if (sip_str_eq_ci(addrtype, "IP6"))
		ver = 6;
	else
		return;

	if (!sip_str_copy(addr, SDP_ADDR_MAX, w) || addr_ipver(addr) != ver) {
		addr[0] = 0;
		return;
	}

	*ipver = ver;
}

static void attr_parse(struct sdp_parse_ctx *ctx, struct sip_str val)
{
	uint8_t dir;
	uint32_t ms;

	if (dir_lookup(val, &dir)) {
		if (!ctx->cur) {
			ctx->dir = dir;
			ctx->dir_present = true;
			return;
		}
		ctx->cur->dir = dir;
		ctx->cur->dir_present = true;
		return;
	}

	if (sip_str_prefix_ci(val, "ptime:")) {
		val = sip_str_init(val.p + 6, val.len - 6);
		if (!sip_str_uint(sip_str_trim(val), &ms) || ms > UINT8_MAX)
			return;
		if (ctx->cur)
			ctx->cur->ptime_ms = (uint8_t)ms;
		else
			ctx->ptime_ms = (uint8_t)ms;
		return;
	}

	if (ctx->cur && sip_str_prefix_ci(val, "rtpmap:")) {
		rtpmap_parse(ctx, sip_str_init(val.p + 7, val.len - 7));
		return;
	}

	if (ctx->cur && sip_str_prefix_ci(val, "fmtp:")) {
		fmtp_parse(ctx, sip_str_init(val.p + 5, val.len - 5));
		return;
	}

	/*
	 * RFC 3605 defines a=rtcp at media level. A session level one is taken
	 * too, which is tolerance, and desc_finish() then hands it to the audio
	 * section alone. The branch runs without a current section for that
	 * reason, and it sits before the two below, which read the attributes
	 * of the current section.
	 */
	if (sip_str_prefix_ci(val, "rtcp:")) {
		rtcp_attr_parse(ctx, sip_str_init(val.p + 5, val.len - 5));
		return;
	}

	/*
	 * RFC 4568 5: a crypto attribute is a media level attribute, so a
	 * session level one is ignored and never reaches a stream. The branch
	 * sits before the T.38 catch-all below, which reads every remaining
	 * attribute of the current section.
	 */
	if (ctx->cur && sip_str_prefix_ci(val, "crypto:")) {
		crypto_attr_parse(ctx->cur, sip_str_init(val.p + 7, val.len - 7));
		return;
	}

	if (ctx->cur)
		t38_attr_parse(&ctx->cur->t38, val);
}

/*
 * RFC 4566 5: a session level value is the default for every media section. The
 * RFC 3605 record is the one exception, and audio says so: an image section
 * carries UDPTL and no RTCP, so a session level a=rtcp never describes it.
 */
static void desc_finish(struct sdp_parse_ctx *ctx, struct sdp_media_desc *m,
			bool audio)
{
	if (!m->present)
		return;

	cn_fmt_move(m);
	opus_fmtp_settle(m);
	g729_fmtp_settle(m);

	if (!m->addr[0]) {
		memcpy(m->addr, ctx->addr, sizeof(m->addr));
		m->ipver = ctx->ipver;
	}
	if (!m->ipver)
		m->ipver = 4;
	if (!m->dir_present) {
		m->dir = ctx->dir;
		m->dir_present = ctx->dir_present;
	}
	if (!m->ptime_ms)
		m->ptime_ms = ctx->ptime_ms;

	/* a media level a=rtcp names a port, so it holds the session one off */
	if (audio && !m->rtcp_port) {
		m->rtcp_port = ctx->rtcp_port;
		memcpy(m->rtcp_addr, ctx->rtcp_addr, sizeof(m->rtcp_addr));
		m->rtcp_ipver = ctx->rtcp_ipver;
	}
}

bool sdp_parse(struct sip_str body, struct sdp_msg *out)
{
	struct sdp_parse_ctx ctx;
	struct sip_str line, val;

	if (!out)
		return false;

	memset(&ctx, 0, sizeof(ctx));
	memset(out, 0, sizeof(*out));
	desc_init(&out->audio);
	desc_init(&out->image);
	desc_init(&ctx.sink);
	ctx.msg = out;

	while (line_next(&body, &line)) {
		if (line.len < 2 || line.p[1] != '=')
			continue;

		val = sip_str_trim(sip_str_init(line.p + 2, line.len - 2));

		switch (line.p[0]) {
		case 'v':
			ctx.have_v = true;
			break;
		case 'o':
			origin_parse(&ctx, val);
			break;
		case 'c':
			conn_parse(&ctx, val);
			break;
		case 'm':
			media_parse(&ctx, val);
			break;
		case 'a':
			attr_parse(&ctx, val);
			break;
		default:
			break;
		}
	}

	if (!ctx.have_v || !ctx.have_m)
		return false;

	desc_finish(&ctx, &out->audio, true);
	desc_finish(&ctx, &out->image, false);

	/* RFC 4566 precedence: the media c= wins over the session c= */
	out->addr_zero = addr_zero_is(out->audio.present ? out->audio.addr : ctx.addr);

	return true;
}

/*
 * The address rules an audio section meets before this side programs a stream
 * from it (RFC 6157 section 5): the connection address is in the family of our
 * own media address, and an RFC 3605 RTCP address, when the section named one,
 * is in that family too. A description that straddles two families names a
 * stream no socket of ours carries, so it is refused the way an FQDN is
 * (addr_literal_is). The RTCP address is never dropped in silence, which would
 * send the reports to the media address instead.
 *
 * The RTCP half asks nothing of our own address, so it stands whatever this
 * side holds. The connection address half is the one local_family_ok() waives
 * for a leg without a literal of its own.
 *
 * sdp_offer_local_ok() runs it before the ring; the answer build and the answer
 * take run it again, as the backstop for a re-offer and for its answer, which
 * no pre-ring check ever sees.
 */
static bool audio_family_ok(const struct sdp_media_desc *m,
			    const char *local_addr)
{
	uint8_t ver = addr_ipver(m->addr);

	if (!local_family_ok(ver, local_addr))
		return false;

	return !m->rtcp_ipver || m->rtcp_ipver == ver;
}

/*
 * The one question the pre-ring check asks: can the address layer ever
 * program the stream this offer names? It reads the same resolved field
 * the take and the build sites read, so both refusals share one acceptance set.
 * The codec, the port and the transport gates stay in sdp_answer_build(), which
 * remains the backstop for every offer this predicate never saw.
 */
bool sdp_offer_usable(const struct sdp_msg *offer)
{
	if (!offer)
		return false;

	return addr_literal_is(offer->audio.addr);
}

/* shared/sdp.h: the family half of the same pre-ring check */
bool sdp_offer_local_ok(const struct sdp_msg *offer, const char *local_addr)
{
	if (!offer)
		return false;

	return audio_family_ok(&offer->audio, local_addr);
}

/* build side */

static bool head_write(struct sip_buf *b, uint64_t sess_id, uint64_t ver, const char *addr)
{
	const char *ipver = addr_ipver_name(addr);

	sip_buf_add(b, "v=0\r\n");
	sip_buf_addf(b, "o=- %" PRIu64 " %" PRIu64 " IN %s %s\r\n", sess_id, ver, ipver, addr);
	sip_buf_add(b, "s=-\r\n");
	sip_buf_addf(b, "c=IN %s %s\r\n", ipver, addr);
	sip_buf_add(b, "t=0 0\r\n");

	return sip_buf_ok(b);
}

/*
 * RFC 4733 section 2.1: the event stream runs on the RTP clock of the audio it
 * rides, so the clock is the selected codec's and not a constant. One m= line
 * carries one telephone-event payload type, so a mixed offer states the clock
 * of the codec it names first.
 */
static void dtmf_write(struct sip_buf *b, int pt, uint32_t clock)
{
	if (pt < 0)
		return;

	sip_buf_addf(b, "a=rtpmap:%d telephone-event/%u\r\n", pt, (unsigned int)clock);
	sip_buf_addf(b, "a=fmtp:%d 0-15\r\n", pt);
}

/*
 * Our own a=fmtp states what this decoder wants and nothing about what this
 * encoder does. RFC 7587 section 7 puts maxplaybackrate,
 * maxaveragebitrate and useinbandfec on this line and maps ptime and maxptime
 * to SDP attributes instead, so the packet mask is written by maxptime_write()
 * and never appears here.
 */
static void opus_fmtp_write(struct sip_buf *b, uint8_t pt)
{
	sip_buf_addf(b, "a=fmtp:%u maxplaybackrate=%u;maxaveragebitrate=%u;useinbandfec=1\r\n",
		     (unsigned int)pt, SDP_OPUS_MAXPLAYBACKRATE, SDP_OPUS_MAXAVERAGEBITRATE);
}

/*
 * The peer's own annexb record for one payload type, or NULL when that section
 * wrote none for it. Both predicates below read it and nothing else, so the
 * value they judge always belongs to the format the exchange selected: a
 * section that names G.729 twice, at two payload types, carries two records
 * and neither one answers for the other.
 */
static const struct sdp_fmt *g729_fmt_at(const struct sdp_media_desc *m, uint8_t pt)
{
	int idx = m ? fmt_idx(m, pt) : -1;

	if (idx < 0 || !m->fmt[idx].annexb_set)
		return NULL;

	return &m->fmt[idx];
}

/*
 * The wire value for a G.729 section's own annexb.
 *
 * cap is sdp_local.g729b, the media plane's own Annex B capability. RFC 3555
 * 4.1.9 says only that annexb=yes means Annex B "is used or preferred". This
 * side writes it as a receive-side claim, what the writer can decode, so it
 * is not config-gated: a backend that decodes a SID frame says so whatever
 * silence_suppression asks of our transmitter. A backend without the
 * capability writes no, which is the Annex A truth.
 *
 * The claim is withdrawn when the peer already named this payload type with
 * an explicit annexb=no. Restating "yes" over a refusal the peer just wrote
 * would contradict the line this side is answering; peer is NULL when there
 * is no peer yet (building an offer), so an offer's own claim never has
 * anything to echo. A no the peer wrote about another payload type withdraws
 * nothing: it describes a format this answer did not select.
 */
static bool g729_annexb_advertise(bool cap, const struct sdp_media_desc *peer, uint8_t pt)
{
	const struct sdp_fmt *f = g729_fmt_at(peer, pt);

	if (!cap)
		return false;

	return !(f && !f->annexb);
}

/*
 * One a=rtpmap and the a=fmtp its codec needs. Opus always writes
 * opus/48000/2: RFC 7587 section 7 makes the clock 48000 and the channel count
 * 2 mandatory, whatever rate the encoder runs at inside the payload and however
 * many channels a packet carries.
 *
 * peer is the description this rtpmap answers (NULL for an offer, which has
 * none yet) and g729b is sdp_local.g729b; codec_rtpmap_write() forwards both
 * to g729_annexb_advertise() and nowhere else. c->annexb plays no part in the
 * wire text: it is the TX arm result of g729_annexb_negotiate(), a question
 * g729_annexb_advertise()'s receive-side claim never answers.
 */
static void codec_rtpmap_write(struct sip_buf *b, const struct sdp_codec *c,
				const struct sdp_media_desc *peer, bool g729b)
{
	enum sdp_codec_id id = (enum sdp_codec_id)c->id;

	if (id == SDP_CODEC_OPUS) {
		sip_buf_addf(b, "a=rtpmap:%u opus/%u/2\r\n", (unsigned int)c->pt,
			     (unsigned int)SDP_CLOCK_RATE_OPUS);
		opus_fmtp_write(b, c->pt);
		return;
	}

	sip_buf_addf(b, "a=rtpmap:%u %s/%u\r\n", (unsigned int)c->pt, sdp_codec_name(id),
		     (unsigned int)codec_clock_rate(id));
	if (id == SDP_CODEC_G729)
		sip_buf_addf(b, "a=fmtp:%u annexb=%s\r\n", (unsigned int)c->pt,
			     g729_annexb_advertise(g729b, peer, c->pt) ? "yes" : "no");
}

/*
 * The local half of the transmit arm rule: may this codec entry arm TX DTX
 * once the peer confirms it explicitly.
 *
 * cap is sdp_local.g729b, the same media plane capability the advertisement
 * above reads, and the two stay separate questions: this one is transmit and
 * asks the codec section too. vad is that section's silence_suppression, which
 * keeps its TR-104 meaning of transmit silence suppression alone; on G.729 it
 * selects Annex B DTX over the generic voice detector and PT 13.
 *
 * cn is sdp_local.cn, the channel cn_enable. Every other codec runs silence
 * suppression only beside comfort noise, because media_prepare clears vad when
 * cng is off. G.729 obeys the same switch, so the arm needs cn_enable too.
 */
static bool g729_annexb_capable(bool cap, bool cn, const struct sdp_codec *c)
{
	return cap && cn && c->vad;
}

/*
 * The peer's effective annexb for a payload type (RFC 3555 4.1.9: an absent
 * parameter means yes). It answers the general question, what this side may
 * accept on receive; arming transmit asks the stricter question
 * g729_annexb_negotiate() runs below, because an absent fmtp must never arm
 * transmit DTX toward a peer that only implements Annex A.
 *
 * Its absent-default branch stays unreachable from that one caller, whose own
 * g729_annexb_set guard runs first, and no receive-side caller joins it: a
 * media plane that carries Annex B accepts a SID frame on every G.729 stream
 * of its own and asks this layer nothing about it, and one that does not
 * already said so on the wire. The branch states the RFC rule for a future
 * reader of the general question and the negotiate rule below states the
 * narrower one.
 */
static bool g729_fmtp_peer_annexb(const struct sdp_media_desc *m, uint8_t pt)
{
	const struct sdp_fmt *f = g729_fmt_at(m, pt);

	return f ? f->annexb : true;
}

/*
 * Our TX DTX arms only when this side could arm it AND the peer wrote an
 * EXPLICIT annexb=yes for the payload type this codec negotiated on. The
 * RFC 3555 absent-parameter default that g729_fmtp_peer_annexb() answers
 * yes to never satisfies this rule: a G.729A-only peer commonly omits the
 * fmtp, and the RFC default would arm DTX toward the decoders least able to
 * handle it.
 *
 * local_armable is the caller's local half of the rule: g729_annexb_capable()
 * of the codec this exchange selected in sdp_answer_build(), or, in
 * sdp_answer_take(), g729_offered_armable() reading what this side's own
 * offer already recorded rather than a fresh read of struct sdp_local: that
 * view can move under a live leg, exactly as st->offered.cn_pt is read and
 * not recomputed.
 */
static bool g729_annexb_negotiate(bool local_armable, const struct sdp_media_desc *m, uint8_t pt)
{
	return local_armable && g729_fmt_at(m, pt) && g729_fmtp_peer_annexb(m, pt);
}

/*
 * Whether our own pending offer's G.729 rtpmap (if it carried one) asked to
 * arm TX DTX for this exact payload type. sdp_offer_build() records the pt
 * and the armable flag it computed together, keyed by payload type because
 * a G.729 entry need not be the offer's leading codec; the pt guard here is
 * what keeps a peer's answer under a different payload type from inheriting
 * an armable state that belonged to whatever codec entry the offer numbered
 * under it instead.
 */
static bool g729_offered_armable(const struct sdp_state *st, uint8_t pt)
{
	return st->offered.g729_pt == (int)pt && st->offered.g729_annexb;
}

/*
 * RFC 7587 section 7: maxptime is an SDP attribute and not an fmtp parameter,
 * and it states the longest packet this decoder wants. 20 ms is the top of the
 * Opus packet mask this media plane carries (ptime_mask 0x03). Every other
 * codec of this stack takes the three narrowband values and states no ceiling
 * at all.
 */
static void maxptime_write(struct sip_buf *b, enum sdp_codec_id id)
{
	if (id != SDP_CODEC_OPUS)
		return;

	sip_buf_addf(b, "a=maxptime:%u\r\n", SDP_OPUS_PTIME_MAX);
}

static void dir_write(struct sip_buf *b, uint8_t dir)
{
	sip_buf_addf(b, "a=%s\r\n", sdp_dir_name((enum sdp_dir)dir));
}

/* the transport token of an audio m= line, which the crypto lines follow */
static const char *audio_proto_name(bool srtp)
{
	return srtp ? "RTP/SAVP" : "RTP/AVP";
}

/*
 * The transport our next offer carries, and the one an answer to that offer
 * has to carry back (RFC 3264: an offer and its answer name one transport). A
 * re-offer follows the committed exchange, which is the mirror of the rule
 * that refuses a plain re-offer on a keyed leg.
 *
 * A leg that answered a plain first offer under accept-plain runs a plain
 * exchange, and offering RTP/SAVP on the next re-INVITE would ask a peer that
 * has no SRTP to answer a transport it already declined. Such a peer answers
 * 488, so our own hold reports a failure and our session refresh clears a call
 * that was running perfectly well. Only an initial offer reads the leg enable,
 * which is where accept-plain still decides what we propose.
 *
 * This side starts no upgrade while the committed exchange stays plain. The
 * peer may still upgrade by offering RTP/SAVP, which the answer build accepts
 * on the same leg under the same enable; that exchange commits a suite, and
 * every re-offer of ours from then on follows the keyed transport.
 *
 * The offer and its answer read the same value, and the reason is call.c, not
 * this layer: exchanges are serialized there. A peer re-INVITE that arrives
 * while ours is outstanding is answered 491 (rx_reinvite tests CALL_ST_ACTIVE),
 * a second local offer cannot start (reinvite_start and the session refresh
 * test the same state), and a glare rollback rebuilds its offer in one frame.
 * Each caller of sdp_commit() takes the answer first. Should a crossing answer
 * still arrive, sdp_answer_build() has cleared offer_pending, so the later take
 * reads SDP_BAD instead of programming media against a moved predicate.
 */
static bool offer_srtp_is(const struct sdp_state *st, const struct sdp_local *loc)
{
	if (st->committed.valid)
		return st->committed.srtp_suite != SDP_SRTP_NONE;

	return loc->srtp;
}

static void crypto_line_write(struct sip_buf *b, uint32_t tag, uint8_t suite,
			      const uint8_t *key)
{
	char text[SDP_SRTP_KEY_B64_LEN + 1];

	key_b64_encode(text, key);
	sip_buf_addf(b, "a=crypto:%u %s inline:%s\r\n", (unsigned int)tag,
		     sdp_srtp_suite_name((enum sdp_srtp_suite)suite), text);
}

/*
 * RFC 4568 5.1.1: an offer names every suite this side accepts, each with a key
 * of its own, and the answer picks one. The keys are the caller's, one per
 * suite. A re-offer that carries crypto attributes at all re-emits exactly
 * those two keys. The call layer stops and starts a stream whose key or suite
 * changes (media_program() in call.c), so the same keys keep a running stream
 * running across a re-INVITE. Which re-offers carry them is offer_srtp_is()'s
 * verdict, not this writer's: a leg whose committed exchange names no suite
 * re-offers plain and writes no line here.
 */
static void crypto_offer_write(struct sip_buf *b, const struct sdp_local *loc, bool srtp)
{
	unsigned int s;

	if (!srtp)
		return;

	for (s = SDP_SRTP_AES_CM_128_SHA1_80; s <= SDP_SRTP_SUITE_MAX; s++)
		crypto_line_write(b, s, (uint8_t)s, loc->srtp_key[s - 1]);
}

/*
 * RFC 4568 5.1.2: an answer carries exactly one crypto attribute, and it
 * mirrors the tag of the line it accepted so the offerer knows which of its
 * suites won. The key is ours: a SDES key is per direction and one side never
 * echoes the other's.
 */
static void crypto_answer_write(struct sip_buf *b, const struct sdp_local *loc,
				const struct sdp_crypto *c)
{
	if (!c)
		return;

	crypto_line_write(b, c->tag, c->suite, loc->srtp_key[c->suite - 1]);
}

/*
 * What call.c programs the stream with. key_tx is our own key of the chosen
 * suite and key_rx is the peer's, so the two ends read what the other writes.
 * A plain exchange leaves the reset values: suite NONE and both keys zero.
 */
static void srtp_result_fill(struct sdp_result *r, const struct sdp_local *loc,
			     const struct sdp_crypto *c)
{
	if (!c)
		return;

	r->srtp_suite = c->suite;
	memcpy(r->srtp_key_tx, loc->srtp_key[c->suite - 1], SDP_SRTP_KEY_LEN);
	memcpy(r->srtp_key_rx, c->key, SDP_SRTP_KEY_LEN);
}

/*
 * The parse stores only a line this layer can honor (crypto_attr_parse), so the
 * answer takes the first stored line, in offer order.
 */
static const struct sdp_crypto *crypto_usable_first(const struct sdp_media_desc *m)
{
	return m->cryptos ? &m->crypto[0] : NULL;
}

/*
 * The one crypto line an answer to our offer may carry. The offer numbered each
 * suite with its own enum value, tag 1 for _80 and tag 2 for _32. A line whose
 * tag and suite disagree, and a line under any other tag, both name an offer
 * this side never made, so the scan skips them and takes the first line that
 * matches. If no line matches, the caller refuses the answer (RFC 3264 6.2).
 */
static const struct sdp_crypto *crypto_mirror_first(const struct sdp_media_desc *m)
{
	unsigned int i;

	for (i = 0; i < m->cryptos && i < SDP_CRYPTO_MAX; i++) {
		if (m->crypto[i].tag == m->crypto[i].suite)
			return &m->crypto[i];
	}

	return NULL;
}

/* the four numbers one image section carries, offered or reconciled */
struct sdp_t38_shape {
	uint32_t max_bit_rate;
	uint32_t max_buffer;
	uint16_t max_datagram;		/* the width sdp_result carries */
	uint8_t ec;			/* enum sdp_t38_ec, NONE omits it */
};

static void t38_shape_local(struct sdp_t38_shape *s)
{
	s->max_bit_rate = SDP_T38_MAX_BIT_RATE;
	s->max_buffer = SDP_T38_MAX_BUFFER;
	s->max_datagram = sdp_t38_max_datagram;
	s->ec = SDP_T38_EC_REDUNDANCY;
}

/*
 * One image section: the five fixed attributes first, then the four this side
 * reconciles. FillBitRemoval, TranscodingMMR and TranscodingJBIG are always 0,
 * and this layer never reads them back.
 *
 * No rtpmap, because udptl carries no payload type. No ptime, because an image
 * section has no packet interval. No direction attribute on any exchange, offer
 * or answer, because the DSP's T.38 direction handling is unverified and a
 * description that names none cannot be misread.
 */
static void t38_media_write(struct sip_buf *b, uint16_t port,
			    const struct sdp_t38_shape *s)
{
	sip_buf_addf(b, "m=image %u udptl t38\r\n", (unsigned int)port);
	sip_buf_add(b, "a=T38FaxVersion:0\r\n");
	sip_buf_add(b, "a=T38FaxRateManagement:transferredTCF\r\n");
	sip_buf_add(b, "a=T38FaxFillBitRemoval:0\r\n");
	sip_buf_add(b, "a=T38FaxTranscodingMMR:0\r\n");
	sip_buf_add(b, "a=T38FaxTranscodingJBIG:0\r\n");
	sip_buf_addf(b, "a=T38MaxBitRate:%u\r\n", (unsigned int)s->max_bit_rate);
	if (s->ec != SDP_T38_EC_NONE)
		sip_buf_add(b, "a=T38FaxUdpEC:t38UDPRedundancy\r\n");
	sip_buf_addf(b, "a=T38FaxMaxBuffer:%u\r\n", (unsigned int)s->max_buffer);
	sip_buf_addf(b, "a=T38FaxMaxDatagram:%u\r\n", (unsigned int)s->max_datagram);
}

/* shared/sdp.h ptime_omit: the description carries no a=ptime line */
static void ptime_write(struct sip_buf *b, uint8_t ptime_ms, bool omit)
{
	if (!omit)
		sip_buf_addf(b, "a=ptime:%u\r\n", (unsigned int)ptime_ms);
}

static void offer_tail_write(struct sip_buf *b, uint8_t ptime_ms, bool omit,
			     enum sdp_codec_id id, uint8_t dir, bool attr)
{
	ptime_write(b, ptime_ms, omit);
	maxptime_write(b, id);

	/* sendrecv is the default (RFC 4566 6), so an offer writes a hold state only */
	if (attr && dir != SDP_SENDRECV)
		dir_write(b, dir);
}

/*
 * An answer with the attribute enabled states the direction on every
 * exchange, sendrecv included: a peer that watches the attribute to follow
 * the hold state then sees the resume as an attribute change instead of as an
 * attribute that disappeared.
 */
static void answer_tail_write(struct sip_buf *b, uint8_t ptime_ms, bool omit,
			      enum sdp_codec_id id, uint8_t dir, bool attr)
{
	ptime_write(b, ptime_ms, omit);
	maxptime_write(b, id);
	if (attr)
		dir_write(b, dir);
}

#define SDP_DIR_SEND		1u
#define SDP_DIR_RECV		2u

/* a value outside enum sdp_dir reads as sendrecv, the RFC 4566 6 default */
static unsigned int dir_bits(uint8_t dir)
{
	switch (dir) {
	case SDP_SENDONLY:
		return SDP_DIR_SEND;
	case SDP_RECVONLY:
		return SDP_DIR_RECV;
	case SDP_INACTIVE:
		return 0;
	default:
		return SDP_DIR_SEND | SDP_DIR_RECV;
	}
}

static uint8_t dir_of_bits(unsigned int bits)
{
	switch (bits & (SDP_DIR_SEND | SDP_DIR_RECV)) {
	case SDP_DIR_SEND:
		return SDP_SENDONLY;
	case SDP_DIR_RECV:
		return SDP_RECVONLY;
	case 0:
		return SDP_INACTIVE;
	default:
		return SDP_SENDRECV;
	}
}

/*
 * RFC 3264 6.1: this side sends only what the peer receives and receives only
 * what the peer sends, within the local direction. c=0.0.0.0 arrives here as
 * a peer sendonly, because an RFC 2543 hold offer carries no direction
 * attribute.
 */
static uint8_t dir_answer(uint8_t peer, uint8_t local)
{
	unsigned int p = dir_bits(peer);
	unsigned int mirror = 0;

	if (p & SDP_DIR_SEND)
		mirror |= SDP_DIR_RECV;
	if (p & SDP_DIR_RECV)
		mirror |= SDP_DIR_SEND;

	return dir_of_bits(dir_bits(local) & mirror);
}

static uint8_t peer_dir_effective(const struct sdp_msg *m)
{
	if (m->addr_zero)
		return SDP_SENDONLY;

	return m->audio.dir;
}

static bool peer_hold_is(const struct sdp_msg *m)
{
	return m->addr_zero || m->audio.dir == SDP_SENDONLY || m->audio.dir == SDP_INACTIVE;
}

/* the peer's direction as this state reads it (sdp_dir_attr_enable()) */
static uint8_t peer_dir_read(const struct sdp_state *st, const struct sdp_msg *m)
{
	if (!st->dir_attr)
		return m->addr_zero ? SDP_SENDONLY : SDP_SENDRECV;

	return peer_dir_effective(m);
}

static bool peer_hold_read(const struct sdp_state *st, const struct sdp_msg *m)
{
	if (!st->dir_attr)
		return m->addr_zero;

	return peer_hold_is(m);
}

/* shared/sdp.h: the G.711 purity of one offered audio section */
bool sdp_audio_g711_only(const struct sdp_msg *offer)
{
	const struct sdp_media_desc *m;
	unsigned int i;

	if (!offer || offer->audio_dup || offer->image.present)
		return false;

	m = &offer->audio;
	if (!m->present || m->fmt_dropped || !m->fmts)
		return false;

	if (peer_dir_effective(offer) != SDP_SENDRECV)
		return false;

	for (i = 0; i < m->fmts; i++) {
		enum sdp_codec_id id = codec_id_lookup(m->fmt[i].name, m->fmt[i].rate);

		if (id != SDP_CODEC_PCMU && id != SDP_CODEC_PCMA)
			return false;
	}

	return true;
}

/*
 * The packet mask is per codec from here on. G.711, G.729, G.722 and
 * G.726-32 take 10, 20 or 30 (their packet mask is 0x7). Opus takes 10 or
 * 20, so 30 becomes 20. Leaving 30 would contradict the a=maxptime:20 this
 * side writes on the same description. The loader clamps 40 to 30 before
 * this layer reads it. ptime_offer() is the mask where the configured value
 * is the only input, which a pinned value (shared/sdp.h ptime_pin) is too.
 */
static uint8_t ptime_offer(enum sdp_codec_id id, uint8_t local)
{
	if (id != SDP_CODEC_OPUS || local == 10 || local == 20)
		return local;

	return SDP_OPUS_PTIME_MAX;
}

static uint8_t ptime_answer(enum sdp_codec_id id, uint8_t offered,
			    const struct sdp_local *loc)
{
	uint8_t local = loc->ptime_ms;

	if (loc->ptime_pin)
		return ptime_offer(id, local);
	if (offered == 10 || offered == 20 || (offered == 30 && id != SDP_CODEC_OPUS))
		return offered;
	if (id == SDP_CODEC_OPUS && local != 10 && local != 20)
		return SDP_OPUS_PTIME_MAX;

	return local;
}

static void result_reset(struct sdp_result *r)
{
	memset(r, 0, sizeof(*r));
	r->dtmf_pt = -1;
	r->cn_pt = -1;
}

/*
 * The peer's a=fmtp for the codec this exchange settled on. opus_fmtp_settle()
 * already proved the stored record belongs to an Opus payload type, so a
 * payload type match is the whole test: an exchange on any other codec leaves
 * the record zero and media_fill.c then fills the configured encoder
 * unchanged.
 */
static void opus_remote_take(struct sdp_result *r, const struct sdp_media_desc *m, uint8_t pt)
{
	if (m->opus_fmtp_pt != (int)pt)
		return;

	r->opus_remote = m->opus_fmtp;
}

static void result_remote_fill(struct sdp_result *r, const struct sdp_media_desc *m)
{
	addr_take(r->remote_addr, m->addr);
	r->remote_ipver = m->ipver;
	r->remote_port = m->port;
	r->rtcp_port = m->rtcp_port;
	addr_take(r->rtcp_addr, m->rtcp_addr);
	r->rtcp_ipver = m->rtcp_ipver;
}

/*
 * The image section this side can program, which is the same set for a
 * received offer and for the answer to an offer of ours: the T.38 Annex D
 * shape (udptl plus the t38 format token) at a live port, with a connection
 * address the address layer can use. The address test is addr_literal_is(), so
 * an FQDN names a stream nothing can program and is refused here instead of
 * after the leg rings.
 *
 * A zero address is a literal, so it passes that test, and it still names no
 * stream: a socket connected to it connects to its own source address and
 * filters every real peer out, which programs clean and then carries nothing.
 * On the audio side the address means RFC 2543 hold and the hold table handles
 * it, but an image section carries no direction at all, so there is nothing
 * here to read it as hold and the only honest verdict is to refuse it.
 *
 * The family test is RFC 6157 section 5: one stream runs in one family, so an
 * image section outside the family of local_addr names a stream this side
 * cannot bind. A leg without a literal of its own takes the waiver
 * local_family_ok() states.
 */
static bool t38_media_ok(const struct sdp_media_desc *m, const char *local_addr)
{
	return m->present && m->port && m->proto_ok && m->t38_fmt &&
	       m->addr[0] && !addr_zero_is(m->addr) &&
	       addr_literal_is(m->addr) &&
	       local_family_ok(addr_ipver(m->addr), local_addr);
}

/*
 * The answer reconciliation. Our own number stands unless the peer asks for
 * a lower rate or names a nonzero buffer. An offered FEC comes back as
 * redundancy, the only error correction uvoice supports.
 * An absent or unreadable correction produces no T38FaxUdpEC attribute.
 *
 * The answer states this daemon's receive limit, 512 under udsp and 72
 * otherwise. The datagram is the one attribute never reconciled with the
 * offer, because each side states its own receive limit. The result's send
 * limit is the peer's number.
 *
 * A rate below SDP_T38_MIN_BIT_RATE reads as absent rather than as a ceiling.
 * The DSP maps any kept rate downward and never exceeds the advertised ceiling,
 * so the plain minimum stays coherent over the whole representable range; below
 * that range it would advertise a ceiling the running stream has to break.
 */
static void t38_shape_answer(struct sdp_t38_shape *s, const struct sdp_t38_attrs *o)
{
	t38_shape_local(s);

	if (o->max_bit_rate_present && o->max_bit_rate >= SDP_T38_MIN_BIT_RATE &&
	    o->max_bit_rate < s->max_bit_rate)
		s->max_bit_rate = o->max_bit_rate;
	if (o->max_buffer_present && o->max_buffer)
		s->max_buffer = o->max_buffer;

	s->ec = o->ec == SDP_T38_EC_NONE ? SDP_T38_EC_NONE : SDP_T38_EC_REDUNDANCY;
}

/*
 * The answer states this daemon's receive limit, 512 under udsp and 72
 * otherwise. ITU-T T.38 Annex D defines this value as the sender's receive
 * limit. The answer never reconciles this attribute with the offer,
 * because each side states its own limit. The result stores the peer's number
 * as the send limit. A missing or zero peer limit stays 0.
 */
static void t38_result_fill(struct sdp_result *r, const struct sdp_t38_shape *s,
			    const struct sdp_media_desc *m)
{
	r->t38 = true;
	r->t38_max_bit_rate = s->max_bit_rate;
	r->t38_ec = s->ec;
	if (m->t38.max_datagram_present && m->t38.max_datagram) {
		r->t38_max_datagram = UINT16_MAX;
		if (m->t38.max_datagram < UINT16_MAX)
			r->t38_max_datagram = (uint16_t)m->t38.max_datagram;
	}

	/* a T.38 result pins sendrecv and carries no ptime: an image section
	 * has no packet interval and no direction attribute */
	r->dir = SDP_SENDRECV;
	r->ptime_ms = 0;
	r->valid = true;
	result_remote_fill(r, m);

	/* a UDPTL stream carries no RTCP, whatever a=rtcp the section named */
	r->rtcp_port = 0;
	r->rtcp_addr[0] = 0;
	r->rtcp_ipver = 0;
}

void sdp_state_init(struct sdp_state *st, uint64_t sess_id)
{
	if (!st)
		return;

	memset(st, 0, sizeof(*st));
	st->sess_id = sess_id;
	st->dir_attr = true;
	offered_reset(st);
	result_reset(&st->committed);
	result_reset(&st->proposed);
}

void sdp_t38_max_datagram_set(unsigned int n)
{
	if (!n) {
		sdp_t38_max_datagram = SDP_T38_MAX_DATAGRAM;
		return;
	}

	sdp_t38_max_datagram = n > UINT16_MAX ? UINT16_MAX : (uint16_t)n;
}

void sdp_dir_set(struct sdp_state *st, enum sdp_dir dir)
{
	if (!st || dir > SDP_INACTIVE)
		return;

	st->local_dir = (uint8_t)dir;
}

void sdp_hold_set(struct sdp_state *st, bool hold)
{
	sdp_dir_set(st, hold ? SDP_SENDONLY : SDP_SENDRECV);
}

void sdp_dir_attr_enable(struct sdp_state *st, bool on)
{
	if (!st)
		return;

	st->dir_attr = on;
}

/*
 * The caller owns this flag exactly as it owns the hold state: it sets it before
 * the upgrade offer and clears it on every path that abandons the upgrade. This
 * layer only reads it, and only when it builds an offer.
 */
void sdp_t38_set(struct sdp_state *st, bool t38)
{
	if (!st)
		return;

	st->local_t38 = t38;
}

/*
 * A T.38 offer replaces the audio section instead of adding to it, and it
 * emits no codec attribute at all. The hold state does not reach this shape,
 * because no image section ever carries a direction attribute.
 */
static bool t38_offer_build(struct sdp_state *st, const struct sdp_local *loc,
			    struct sip_buf *b)
{
	struct sdp_t38_shape shape;
	char addr[SDP_ADDR_MAX];

	t38_shape_local(&shape);
	addr_take(addr, loc->addr);
	head_write(b, st->sess_id, st->sess_ver, addr);
	t38_media_write(b, loc->port, &shape);

	/* an offer that did not fit was never sent, so the session did not change */
	if (!sip_buf_ok(b))
		return false;

	/* RFC 3264 8: the o= version changes whenever the session description does */
	st->sess_ver++;

	/*
	 * An offer knows this side only, so the remote tuple stays empty and
	 * proposed stays invalid until the answer arrives. The two fixed fields
	 * are already zero after the reset; the writes name the T.38 rule
	 * (sendrecv, no ptime) at the site the hold state and the configured
	 * ptime would otherwise reach.
	 */
	offered_reset(st);
	result_reset(&st->proposed);
	st->proposed.t38 = true;
	st->proposed.dir = SDP_SENDRECV;
	st->proposed.ptime_ms = 0;
	st->offer_pending = true;

	return true;
}

bool sdp_offer_build(struct sdp_state *st, const struct sdp_local *loc, struct sip_buf *b)
{
	const struct sdp_codec *top;
	char addr[SDP_ADDR_MAX];
	uint8_t dir;
	enum sdp_codec_id top_id;
	uint8_t ptime;
	unsigned int i;
	uint32_t clock, alt_clock;
	int dtmf, alt, cn, g729_pt = -1;
	bool srtp, g729_armable = false;

	if (!st || !loc || !b)
		return false;

	/*
	 * T.38 is gated off on an SRTP leg. An image stream carries plain
	 * UDPTL, so an upspeed would drop the encryption this call negotiated
	 * and say nothing. The state stays exactly as it is, so the caller
	 * keeps the committed audio exchange and the call.
	 */
	if (st->local_t38 && st->committed.srtp_suite != SDP_SRTP_NONE)
		return false;

	/* a T.38 offer needs no codec of ours, so it runs before the codec gate */
	if (st->local_t38)
		return t38_offer_build(st, loc, b);

	top = codec_first(loc);
	if (!top)
		return false;

	srtp = offer_srtp_is(st, loc);

	/*
	 * The first codec decides the stream's clock, and the telephone-event
	 * and the packet mask follow it: one m= line carries one event payload
	 * type, so an offer that names Opus first states telephone-event/48000
	 * and one that names a narrowband codec first keeps /8000 untouched.
	 */
	top_id = (enum sdp_codec_id)top->id;
	clock = codec_clock_rate(top_id);
	ptime = ptime_offer(top_id, loc->ptime_ms);

	dtmf = offer_dtmf_pt(loc);
	cn = offer_cn_pt(loc);

	/*
	 * The second event line, for the clock the first codec does not run
	 * on. It is written only when this offer really mixes clock families,
	 * so a narrowband list and a G.722 list carry the one line they always
	 * carried.
	 */
	alt_clock = dtmf >= 0 ? offer_alt_clock(loc, clock) : 0;
	alt = alt_clock ? offer_dtmf_pt_alt(loc, dtmf, cn) : -1;
	if (alt < 0)
		alt_clock = 0;

	/* a peer that holds this side receives nothing, so a hold sends nothing */
	dir = st->local_dir;
	if (dir == SDP_SENDONLY && st->committed.remote_hold)
		dir = SDP_INACTIVE;

	addr_take(addr, loc->addr);
	head_write(b, st->sess_id, st->sess_ver, addr);

	sip_buf_addf(b, "m=audio %u %s", (unsigned int)loc->port,
		     audio_proto_name(srtp));
	for (i = 0; i < codec_count(loc); i++) {
		if (offer_codec_is(loc, i))
			sip_buf_addf(b, " %u", (unsigned int)loc->codec[i].pt);
	}
	if (dtmf >= 0)
		sip_buf_addf(b, " %d", dtmf);
	if (alt >= 0)
		sip_buf_addf(b, " %d", alt);
	if (cn >= 0)
		sip_buf_addf(b, " %d", cn);
	sip_buf_crlf(b);

	/* RFC 4568 5.1.1: the crypto attributes follow the m= line they key */
	crypto_offer_write(b, loc, srtp);

	for (i = 0; i < codec_count(loc); i++) {
		if (!offer_codec_is(loc, i))
			continue;

		/*
		 * codec_rtpmap_write() decides the wire text on its own
		 * (g729_annexb_advertise(), independent of g729_armable
		 * below): advertisement and TX-arm are separate questions.
		 * This walk's own job is finding which entry is G.729 and
		 * what it could arm, since a G.729 codec need not be first.
		 */
		codec_rtpmap_write(b, &loc->codec[i], NULL, loc->g729b);
		if ((enum sdp_codec_id)loc->codec[i].id == SDP_CODEC_G729) {
			g729_pt = loc->codec[i].pt;
			g729_armable = g729_annexb_capable(loc->g729b, loc->cn, &loc->codec[i]);
		}
	}

	dtmf_write(b, dtmf, clock);
	dtmf_write(b, alt, alt_clock);
	if (cn >= 0)
		sip_buf_addf(b, "a=rtpmap:%d CN/%u\r\n", cn, (unsigned int)SDP_CLOCK_RATE);

	offer_tail_write(b, ptime, loc->ptime_omit, top_id, dir, st->dir_attr);

	/* an offer that did not fit was never sent, so the session did not change */
	if (!sip_buf_ok(b))
		return false;

	/* RFC 3264 8: the o= version changes whenever the session description does */
	st->sess_ver++;

	/*
	 * An offer knows this side only: the peer address and the selected
	 * codec arrive with the answer, so proposed stays invalid until then.
	 */
	offered_reset(st);
	st->offered.clock = clock;
	st->offered.alt_clock = alt_clock;
	st->offered.dtmf_pt = dtmf;
	st->offered.alt_pt = alt;
	st->offered.cn_pt = cn;
	st->offered.g729_pt = g729_pt;
	st->offered.g729_annexb = g729_armable;

	result_reset(&st->proposed);
	st->proposed.codec = *top;
	st->proposed.dtmf_pt = dtmf;
	st->proposed.cn_pt = cn;
	st->proposed.dir = dir;
	st->proposed.ptime_ms = ptime;
	st->offer_pending = true;

	return true;
}

struct sdp_sel {
	const struct sdp_codec *local;
	uint8_t pt;			/* the payload type the peer numbered it */
};

/*
 * RFC 3264 6.1: the answer follows the offer order, never our own priority.
 *
 * The local walk uses offer_codec_is() and not codec_usable(), so a codec the
 * payload type collision rule drops is unusable on both sides of the
 * negotiation. The alternative seats a format on a contested
 * type: our offer gave payload type 9 to G.722, the peer answers PCMA on 9,
 * and each end then decodes the other's media as the wrong codec.
 */
static bool codec_select(const struct sdp_local *loc, const struct sdp_media_desc *m,
			 struct sdp_sel *sel)
{
	unsigned int i, j;

	for (i = 0; i < m->fmts && i < SDP_CODEC_MAX; i++) {
		enum sdp_codec_id id = codec_id_lookup(m->fmt[i].name, m->fmt[i].rate);

		if (id == SDP_CODEC_NONE)
			continue;

		for (j = 0; j < codec_count(loc); j++) {
			if (!offer_codec_is(loc, j) || loc->codec[j].id != id)
				continue;
			sel->local = &loc->codec[j];
			sel->pt = m->fmt[i].pt;
			return true;
		}
	}

	return false;
}

/*
 * Every gate the image branch has to pass before it answers a T.38 offer. A
 * miss takes the audio ladder below and the port-0 image refusal.
 *
 * st->committed.valid alone keeps the initial INVITE path unchanged: T.38 is a
 * mid-call upgrade, so an inbound switch that already carries an established
 * audio leg has a committed exchange and a first INVITE never does. The audio
 * preference drops every port-0 stream before it looks the audio section up,
 * so the image branch runs only when no usable audio is left.
 *
 * Two shapes make that preference test blind, and both close the gate instead
 * of accepting an offer this layer cannot see:
 *
 *   - a port the parse could not read. It lands at 0 like a withdrawn stream,
 *     so "m=audio nope RTP/AVP 0" would otherwise read as an audio section the
 *     peer gave up, when it is an audio section this layer garbled.
 *   - a duplicated audio or image section. The second one goes to the sink, so
 *     a live second m=audio is invisible here and the preference test would
 *     accept T.38 over an offer that still carries usable audio.
 */
static bool t38_answer_gate(const struct sdp_state *st, const struct sdp_local *loc,
			    const struct sdp_msg *offer)
{
	if (!st->committed.valid || !loc->t38)
		return false;

	/*
	 * T.38 is gated off on an SRTP leg. An image stream carries plain
	 * UDPTL, so accepting the upgrade would drop the encryption this call
	 * negotiated and say nothing about it. The offer takes the audio ladder
	 * and the port 0 image refusal instead.
	 */
	if (st->committed.srtp_suite != SDP_SRTP_NONE)
		return false;

	if (offer->audio_dup || offer->image_dup)
		return false;
	if (!t38_media_ok(&offer->image, loc->addr))
		return false;

	return !offer->audio.present || (!offer->audio.port && offer->audio.port_ok);
}

/*
 * Is the family the one thing that refuses this image section? The section
 * meets every other rule of t38_media_ok(), and only our own address disagrees
 * with it. NULL waives the family half there, so the call asks the rest of the
 * shape alone.
 *
 * It separates two verdicts an offer without an audio section would otherwise
 * share. A mixed family image stream is a session this side cannot run, which
 * is SDP_NO_CODEC, the 488 class. Every other offer that names no audio
 * is a description this side cannot read, which stays SDP_BAD.
 */
static bool t38_family_refused(const struct sdp_media_desc *m,
			       const char *local_addr)
{
	if (local_family_ok(addr_ipver(m->addr), local_addr))
		return false;

	return t38_media_ok(m, NULL);
}

/*
 * RFC 4566: an m= line needs at least one format token, so a refused audio
 * stream keeps the first one the offer listed. RFC 3264 6: the answer names the
 * offered sections in the offered order, which is why the caller places this.
 *
 * fmt_first, not fmt[0]: the parse moves telephone-event and CN out of fmt[], so
 * a withdrawn stream that listed one of those alone leaves fmt[] EMPTY, and
 * reading fmt[0] would then invent payload type 0, a format the peer never named.
 * A section that listed no numeric token at all still falls back to 0, because
 * the line needs one token and there is nothing else to name.
 */
static void audio_refuse_write(struct sip_buf *b, const struct sdp_media_desc *m)
{
	if (!m->present)
		return;

	sip_buf_addf(b, "m=audio 0 RTP/AVP %u\r\n",
		     m->fmt_first >= 0 ? (unsigned int)m->fmt_first : 0);
}

static enum sdp_verdict t38_answer_build(struct sdp_state *st, const struct sdp_local *loc,
					 const struct sdp_msg *offer, struct sip_buf *b)
{
	struct sdp_t38_shape shape;
	char addr[SDP_ADDR_MAX];

	t38_shape_answer(&shape, &offer->image.t38);

	addr_take(addr, loc->addr);
	head_write(b, st->sess_id, st->sess_ver, addr);

	if (!offer->image_first)
		audio_refuse_write(b, &offer->audio);
	t38_media_write(b, loc->port, &shape);
	if (offer->image_first)
		audio_refuse_write(b, &offer->audio);

	/* a truncated answer never reaches the peer, so no media is negotiated */
	if (!sip_buf_ok(b))
		return SDP_BAD;

	st->sess_ver++;
	result_reset(&st->proposed);
	t38_result_fill(&st->proposed, &shape, &offer->image);
	st->offer_pending = false;

	return SDP_OK;
}

enum sdp_verdict sdp_answer_build(struct sdp_state *st, const struct sdp_local *loc,
				  const struct sdp_msg *offer, struct sip_buf *b)
{
	const struct sdp_crypto *crypto = NULL;
	char addr[SDP_ADDR_MAX];
	enum sdp_codec_id sel_id;
	struct sdp_codec answered;
	struct sdp_sel sel;
	uint8_t dir, ptime;
	uint32_t clock;
	int dtmf, cn;
	bool annexb = false;

	if (!st || !loc || !offer || !b)
		return SDP_BAD;

	if (t38_answer_gate(st, loc, offer))
		return t38_answer_build(st, loc, offer, b);

	if (!offer->audio.present) {
		/* the 488 class, not a description this layer could not read */
		if (t38_family_refused(&offer->image, loc->addr))
			return SDP_NO_CODEC;

		return SDP_BAD;
	}

	/* without a connection address there is nowhere to send the stream */
	if (!offer->audio.addr[0])
		return SDP_BAD;
	if (!addr_literal_is(offer->audio.addr))
		return SDP_NO_CODEC;

	/* RFC 6157 section 5, and a re-offer meets no pre-ring check */
	if (!audio_family_ok(&offer->audio, loc->addr))
		return SDP_NO_CODEC;

	/* port 0 removes the stream (RFC 3264 8.4), so there is nothing to answer */
	if (!offer->audio.port)
		return SDP_NO_CODEC;

	/*
	 * Accept-plain narrows to the initial offer. A leg whose committed
	 * exchange names a suite refuses a re-offer that carries no SRTP,
	 * because answering that one plain would commit the downgrade and
	 * nothing downstream would report it: the stream is programmed again
	 * with HAL_SRTP_NONE, so the call would then simply run in clear.
	 * The caller answers 488 and rolls back, which leaves the running
	 * SRTP exchange exactly as it was.
	 *
	 * The initial INVITE has no committed exchange, so a plain first offer
	 * on an SRTP leg is still answered plain. The suite term alone already
	 * says that, because a suite reaches committed only through a valid
	 * result. The valid term is kept beside it for the shape the T.38 gate
	 * above states the same condition in.
	 */
	if (st->committed.valid && st->committed.srtp_suite != SDP_SRTP_NONE &&
	    !offer->audio.savp)
		return SDP_NO_CODEC;

	/*
	 * The transport gate (RFC 3264: an answer keeps the offered transport).
	 * An RTP/AVP offer that reaches here is answered plain, so proto_ok
	 * alone still describes the whole plain path. An RTP/SAVP offer needs
	 * the leg enable and one crypto line this side can use; every other
	 * transport is refused as before.
	 */
	if (!offer->audio.proto_ok) {
		if (!offer->audio.savp || !loc->srtp)
			return SDP_NO_CODEC;

		crypto = crypto_usable_first(&offer->audio);
		if (!crypto)
			return SDP_NO_CODEC;
	}

	if (!codec_select(loc, &offer->audio, &sel))
		return SDP_NO_CODEC;

	/*
	 * The selected codec decides the clock, so a peer that offered Opus
	 * with telephone-event/8000 alone gets an answer with no
	 * telephone-event: an event stream on the wrong clock carries digits
	 * neither end can time (RFC 4733 section 2.1). call.c then falls the
	 * leg back to SIP INFO or inband and counts it.
	 */
	sel_id = (enum sdp_codec_id)sel.local->id;
	clock = codec_clock_rate(sel_id);
	dtmf = loc->dtmf_pt ? desc_dtmf_pt(&offer->audio, clock) : -1;
	cn = loc->cn && cn_pairs_with(sel_id) ? offer->audio.cn_pt : -1;
	dir = dir_answer(peer_dir_read(st, offer), st->local_dir);
	ptime = ptime_answer(sel_id, offer->audio.ptime_ms, loc);
	if (sel_id == SDP_CODEC_G729)
		annexb = g729_annexb_negotiate(g729_annexb_capable(loc->g729b, loc->cn, sel.local),
					       &offer->audio, sel.pt);

	addr_take(addr, loc->addr);
	head_write(b, st->sess_id, st->sess_ver, addr);

	sip_buf_addf(b, "m=audio %u %s %u", (unsigned int)loc->port,
		     audio_proto_name(crypto != NULL), (unsigned int)sel.pt);
	if (dtmf >= 0)
		sip_buf_addf(b, " %d", dtmf);
	if (cn >= 0)
		sip_buf_addf(b, " %d", cn);
	sip_buf_crlf(b);

	/* RFC 4568 5.1.2: the one accepted line, right behind the m= line */
	crypto_answer_write(b, loc, crypto);

	answered = *sel.local;
	answered.pt = sel.pt;
	answered.annexb = annexb;
	codec_rtpmap_write(b, &answered, &offer->audio, loc->g729b);

	dtmf_write(b, dtmf, clock);
	if (cn >= 0)
		sip_buf_addf(b, "a=rtpmap:%d CN/%u\r\n", cn, (unsigned int)SDP_CLOCK_RATE);

	answer_tail_write(b, ptime, loc->ptime_omit, sel_id, dir, st->dir_attr);

	/* port 0 refuses T.38 and keeps the audio stream alive (RFC 3264 6) */
	if (offer->image.present)
		sip_buf_add(b, "m=image 0 udptl t38\r\n");

	/* a truncated answer never reaches the peer, so no media is negotiated */
	if (!sip_buf_ok(b))
		return SDP_BAD;

	st->sess_ver++;
	result_reset(&st->proposed);
	st->proposed.codec = answered;
	opus_remote_take(&st->proposed, &offer->audio, sel.pt);
	st->proposed.dtmf_pt = dtmf;
	st->proposed.cn_pt = cn;
	st->proposed.dir = dir;
	st->proposed.ptime_ms = ptime;
	st->proposed.remote_hold = peer_hold_read(st, offer);
	st->proposed.image_refused = offer->image.present;
	st->proposed.valid = true;
	srtp_result_fill(&st->proposed, loc, crypto);
	result_remote_fill(&st->proposed, &offer->audio);
	st->offer_pending = false;

	return SDP_OK;
}

/*
 * An answer to a T.38 offer either carries a usable image section or it
 * refuses the upgrade: an audio-only answer is not a session this side may
 * program, so SDP_NO_CODEC makes the caller roll back to the committed audio
 * exchange and keep the call. The numbers come from the answer, reconciled the
 * way an answer of ours would be.
 */
static enum sdp_verdict t38_answer_take(struct sdp_state *st,
					const struct sdp_local *loc,
					const struct sdp_msg *answer)
{
	struct sdp_t38_shape shape;

	if (!t38_media_ok(&answer->image, loc->addr))
		return SDP_NO_CODEC;

	t38_shape_answer(&shape, &answer->image.t38);

	result_reset(&st->proposed);
	t38_result_fill(&st->proposed, &shape, &answer->image);
	st->offer_pending = false;

	return SDP_OK;
}

enum sdp_verdict sdp_answer_take(struct sdp_state *st, const struct sdp_local *loc,
				 const struct sdp_msg *answer)
{
	const struct sdp_crypto *crypto = NULL;
	enum sdp_codec_id sel_id;
	struct sdp_sel sel;
	uint32_t clock;
	int dtmf;
	bool annexb = false;

	if (!st || !loc || !answer)
		return SDP_BAD;

	/* an answer without an offer of ours behind it must not program media */
	if (!st->offer_pending)
		return SDP_BAD;

	if (st->proposed.t38)
		return t38_answer_take(st, loc, answer);

	if (!answer->audio.present || !answer->audio.port)
		return SDP_NO_CODEC;
	if (!answer->audio.addr[0])
		return SDP_BAD;
	if (!addr_literal_is(answer->audio.addr))
		return SDP_NO_CODEC;

	/* the answer to our own offer meets the same family rule */
	if (!audio_family_ok(&answer->audio, loc->addr))
		return SDP_NO_CODEC;

	/*
	 * RFC 3264 6.2: an answer keeps the transport of the offer. The
	 * predicate is the one the offer was built from, read from the same
	 * state, so a mismatch either way is an answer to an offer we never
	 * made: a plain answer to the plain re-offer of a committed plain leg is
	 * taken, and an SAVP answer to it is refused. An accepted SAVP answer
	 * then needs the one mirrored crypto line RFC 4568 5.1.2 demands.
	 */
	if (answer->audio.savp != offer_srtp_is(st, loc))
		return SDP_NO_CODEC;
	if (!answer->audio.savp && !answer->audio.proto_ok)
		return SDP_NO_CODEC;

	if (answer->audio.savp) {
		/*
		 * The leg must hold keys before an SAVP answer is accepted. The
		 * fill below copies loc->srtp_key into the exchange, and call.c
		 * draws that material behind loc.srtp alone (leg_media_take is
		 * its one writer; srtp_keys_take runs under it). A committed
		 * suite therefore implies the enable and this test cannot fire,
		 * but if it ever did, the stream would be keyed with 30 zero
		 * bytes and no layer below would report it.
		 */
		if (!loc->srtp)
			return SDP_NO_CODEC;

		/*
		 * RFC 4568 5.1.2: an answer carries exactly one crypto
		 * attribute. Two lines this layer could use leave the peer's
		 * own transmit key ambiguous between them, and a stream keyed
		 * against the wrong one carries noise with no error anywhere,
		 * so the whole answer is refused. A line the parse dropped
		 * never counted, which keeps the tolerant contract: one usable
		 * line beside one malformed line still negotiates.
		 */
		if (answer->audio.cryptos != 1)
			return SDP_NO_CODEC;

		crypto = crypto_mirror_first(&answer->audio);
		if (!crypto)
			return SDP_NO_CODEC;
	}

	/*
	 * The peer may answer with a payload type other than the one we offered.
	 * One symmetric type carries the stream, and the answer decides what we
	 * send, so the remapped type is taken as it stands.
	 */
	if (!codec_select(loc, &answer->audio, &sel))
		return SDP_NO_CODEC;

	/*
	 * Nothing lands here that our offer did not put on the wire. The codec
	 * selection already ran that invariant; the two stream elements beside
	 * it run it now.
	 *
	 * The event type has to pass both tests. Our offer must have advertised
	 * an event line for the clock the selected codec runs on, and the
	 * answer's own event type must be at that clock (RFC 4733 section 2.1).
	 * An answer that names an event stream our offer never carried is the
	 * counted refusal, not a seated payload type: call.c then falls the leg
	 * back to SIP INFO or inband.
	 *
	 * Comfort noise reads the same record and not loc->cn, because the
	 * comfort noise withdrawal pass may have withheld the CN line from an
	 * Opus-led offer while the leg enable stayed on. A peer that answers the
	 * narrowband codec of such an offer and names CN is naming a line that
	 * offer did not carry.
	 */
	sel_id = (enum sdp_codec_id)sel.local->id;
	clock = codec_clock_rate(sel_id);
	dtmf = offered_dtmf_pt(st, clock) >= 0 ?
	       desc_dtmf_pt(&answer->audio, clock) : -1;
	if (sel_id == SDP_CODEC_G729)
		annexb = g729_annexb_negotiate(g729_offered_armable(st, sel.pt),
					       &answer->audio, sel.pt);

	result_reset(&st->proposed);
	st->proposed.codec = *sel.local;
	st->proposed.codec.pt = sel.pt;
	st->proposed.codec.annexb = annexb;
	opus_remote_take(&st->proposed, &answer->audio, sel.pt);
	if (dtmf >= 0)
		st->proposed.dtmf_pt = dtmf;
	if (st->offered.cn_pt >= 0 && cn_pairs_with(sel_id) && answer->audio.cn_pt >= 0)
		st->proposed.cn_pt = answer->audio.cn_pt;
	st->proposed.dir = dir_answer(peer_dir_read(st, answer), st->local_dir);
	st->proposed.ptime_ms = ptime_answer(sel_id, answer->audio.ptime_ms, loc);
	st->proposed.remote_hold = peer_hold_read(st, answer);
	st->proposed.valid = true;
	srtp_result_fill(&st->proposed, loc, crypto);
	result_remote_fill(&st->proposed, &answer->audio);
	st->offer_pending = false;

	return SDP_OK;
}

void sdp_commit(struct sdp_state *st)
{
	if (!st)
		return;

	st->committed = st->proposed;
	st->offer_pending = false;
	st->offer_in_200 = false;
}

void sdp_rollback(struct sdp_state *st)
{
	if (!st)
		return;

	st->proposed = st->committed;
	st->offer_pending = false;
	st->offer_in_200 = false;
}
