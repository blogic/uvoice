// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the SDP offer/answer layer (src/proto/shared/sdp.c).
 *
 * Coverage follows shared/sdp.h. The suite covers the tolerant parse, the offer
 * that this side builds, the gated T.38 negotiation and the two phase commit.
 * It also covers the RFC 3264 6.1 answer: offer order selection, payload type
 * mirroring, the hold table and the T.38 port zero refusal.
 */
#include <stdlib.h>

#include "proto/shared/sdp.h"
#include "test_util.h"

#define HEAD	"v=0\r\no=- 7 9 IN IP4 198.51.100.7\r\ns=-\r\n" \
		"c=IN IP4 198.51.100.7\r\nt=0 0\r\n"

/* the same head in the other address family, for the RFC 6157 section 5 rows */
#define HEAD6	"v=0\r\no=- 7 9 IN IP6 2001:db8::1\r\ns=-\r\n" \
		"c=IN IP6 2001:db8::1\r\nt=0 0\r\n"

/* the media address of a line that signals in the other family */
#define LOCAL6	"2001:db8::10"

/* the five attributes both references hardcode, in their emit order */
#define T38_FIXED	"a=T38FaxVersion:0\r\n" \
			"a=T38FaxRateManagement:transferredTCF\r\n" \
			"a=T38FaxFillBitRemoval:0\r\n" \
			"a=T38FaxTranscodingMMR:0\r\n" \
			"a=T38FaxTranscodingJBIG:0\r\n"

static struct sdp_msg m;
static struct sip_buf b;
static char out[4096];

static bool parse(const char *text)
{
	return sdp_parse(sip_str_cstr(text), &m);
}

static void buf_reset(void)
{
	sip_buf_init(&b, out, sizeof(out));
}

static bool has(const char *needle)
{
	return strstr(out, needle) != NULL;
}

/*
 * The default local view of this suite. g729b is set because it is the shape
 * of every backend that carries the coder (hal.h, HAL_FEAT_G729B: the udsp and
 * stub backends set it), and a G.729 golden of this file is that board's wire
 * text. The capability-clear board, which is the Annex A one, gets its own
 * rows in g729_annexb_matrix_tests() and sets the field back to false there.
 */
static void local_init(struct sdp_local *loc)
{
	memset(loc, 0, sizeof(*loc));
	loc->g729b = true;
	loc->codec[0].id = SDP_CODEC_PCMU;
	loc->codec[0].pt = 0;
	loc->codec[1].id = SDP_CODEC_PCMA;
	loc->codec[1].pt = 8;
	loc->codecs = 2;
	snprintf(loc->addr, sizeof(loc->addr), "%s", "192.0.2.10");
	loc->port = 4000;
	loc->ptime_ms = 20;
	loc->dtmf_pt = 101;
}

static const char *fmt_name(unsigned int i)
{
	return m.audio.fmt[i].name;
}

/* the rtpmap line of the same offer that already claims this entry's type */
static bool owner_is(const struct sdp_local *loc, unsigned int idx, const char *name)
{
	const char *owner = sdp_offer_pt_owner(loc, idx);

	return owner && !strcmp(owner, name);
}

/*
 * A state that carries a real pending offer built from this local view.
 *
 * It builds the offer instead of setting the flag, because sdp_answer_take()
 * judges an answer against what the offer recorded it put on the wire
 * (shared/sdp.h, sdp_state.offered), and a state that never built one
 * advertised nothing. A view that cannot build an offer at all still gets the
 * flag, so the cases that drive the take with an empty or disabled list keep
 * their meaning.
 */
static void take_state_init(struct sdp_state *st, const struct sdp_local *loc)
{
	struct sip_buf ob;
	char obuf[2048];

	sdp_state_init(st, 77);
	sip_buf_init(&ob, obuf, sizeof(obuf));
	if (!sdp_offer_build(st, loc, &ob))
		st->offer_pending = true;
}

static void parse_tests(void)
{
	CASE("minimal CRLF body parses");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(m.audio.present);
	CHECK_UINT(m.audio.port, 5004);
	CHECK_UINT(m.audio.fmts, 1);
	CHECK_UINT(m.audio.fmt[0].pt, 0);

	CASE("bare LF body parses");
	CHECK(parse("v=0\no=- 1 1 IN IP4 198.51.100.7\ns=-\nc=IN IP4 198.51.100.7\n"
		    "t=0 0\nm=audio 6000 RTP/AVP 8\na=ptime:30\n"));
	CHECK(m.audio.present);
	CHECK_UINT(m.audio.port, 6000);
	CHECK(!strcmp(m.audio.addr, "198.51.100.7"));
	CHECK_UINT(m.audio.ptime_ms, 30);

	CASE("a body without v= is refused");
	CHECK(!parse("o=- 1 1 IN IP4 198.51.100.7\r\nm=audio 5004 RTP/AVP 0\r\n"));

	CASE("a body without m= is refused");
	CHECK(!parse("v=0\r\no=- 1 1 IN IP4 198.51.100.7\r\nc=IN IP4 198.51.100.7\r\n"));

	CASE("an empty body is refused");
	CHECK(!parse(""));
	CHECK(!sdp_parse(sip_str_init(NULL, 0), &m));

	CASE("o= carries the session id and version");
	CHECK(parse("v=0\r\no=alice 3141592653 17 IN IP4 198.51.100.7\r\n"
		    "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK_UINT(m.sess_id, 3141592653u);
	CHECK_UINT(m.sess_ver, 17);

	CASE("unparsable o= numbers become zero");
	CHECK(parse("v=0\r\no=- xx yy IN IP4 198.51.100.7\r\nm=audio 5004 RTP/AVP 0\r\n"));
	CHECK_UINT(m.sess_id, 0);
	CHECK_UINT(m.sess_ver, 0);

	CASE("a truncated o= line does not fail the body");
	CHECK(parse("v=0\r\no=-\r\nm=audio 5004 RTP/AVP 0\r\n"));
	CHECK_UINT(m.sess_id, 0);

	CASE("unknown lines are skipped");
	CHECK(parse(HEAD "b=AS:64\r\nz=21 -1h\r\nm=audio 5004 RTP/AVP 0\r\n"
		    "k=clear:secret\r\n"));
	CHECK(m.audio.present);
	CHECK_UINT(m.audio.fmts, 1);

	CASE("a line without the '=' is skipped");
	CHECK(parse(HEAD "garbage\r\nx\r\n\r\nm=audio 5004 RTP/AVP 0\r\n"));
	CHECK(m.audio.present);

	CASE("unknown attributes are skipped");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=maxptime:40\r\n"
		    "a=silenceSupp:off - - - -\r\na=X-unknown\r\n"));
	CHECK(m.audio.present);
	CHECK_UINT(m.audio.fmts, 1);
	CHECK_UINT(m.audio.ptime_ms, 0);

	CASE("static payload types are named without an rtpmap");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 8 18\r\n"));
	CHECK_UINT(m.audio.fmts, 3);
	CHECK(!strcmp(fmt_name(0), "PCMU"));
	CHECK(!strcmp(fmt_name(1), "PCMA"));
	CHECK(!strcmp(fmt_name(2), "G729"));
	CHECK_UINT(m.audio.fmt[0].rate, 8000);
	CHECK_UINT(m.audio.fmt[2].rate, 8000);

	/* RFC 3551 table 4: payload type 9 is G722, and its rtpmap rate is 8000 */
	CASE("static payload type 9 is named G722 at the 8000 clock");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 9\r\n"));
	CHECK_UINT(m.audio.fmts, 1);
	CHECK_UINT(m.audio.fmt[0].pt, 9);
	CHECK(!strcmp(fmt_name(0), "G722"));
	CHECK_UINT(m.audio.fmt[0].rate, 8000);

	CASE("an rtpmap that names G722 at 16000 keeps the rate the peer wrote");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 9\r\na=rtpmap:9 G722/16000\r\n"));
	CHECK(!strcmp(fmt_name(0), "G722"));
	CHECK_UINT(m.audio.fmt[0].rate, 16000);

	CASE("a repeated payload type keeps its first entry");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 9 0 9\r\n"));
	CHECK_UINT(m.audio.fmts, 2);
	CHECK_UINT(m.audio.fmt[0].pt, 9);
	CHECK_UINT(m.audio.fmt[1].pt, 0);

	CASE("a repeated payload type leaves one entry for its rtpmap to name");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 96\r\n"
		    "a=rtpmap:96 G722/8000\r\n"));
	CHECK_UINT(m.audio.fmts, 1);
	CHECK(!strcmp(fmt_name(0), "G722"));
	CHECK_UINT(m.audio.fmt[0].rate, 8000);

	CASE("a repeated static payload type is still counted once");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 0 0 8\r\n"));
	CHECK_UINT(m.audio.fmts, 2);
	CHECK_UINT(m.audio.fmt[0].pt, 0);
	CHECK_UINT(m.audio.fmt[1].pt, 8);
	CHECK_UINT(m.audio.fmt_first, 0);

	CASE("an rtpmap overrides a static payload type");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=rtpmap:0 PCMA/8000\r\n"));
	CHECK_UINT(m.audio.fmts, 1);
	CHECK_UINT(m.audio.fmt[0].pt, 0);
	CHECK(!strcmp(fmt_name(0), "PCMA"));

	CASE("rtpmap names are stored upper cased");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 pcmu/8000\r\n"));
	CHECK(!strcmp(fmt_name(0), "PCMU"));
	CHECK_UINT(m.audio.fmt[0].rate, 8000);

	CASE("fmt order is preserved");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18 8 0\r\n"));
	CHECK_UINT(m.audio.fmts, 3);
	CHECK_UINT(m.audio.fmt[0].pt, 18);
	CHECK_UINT(m.audio.fmt[1].pt, 8);
	CHECK_UINT(m.audio.fmt[2].pt, 0);

	/*
	 * SDP_CODEC_MAX grew from 8 to 16 with G.722, so a peer that lists ten
	 * formats keeps all ten and the cap bites at the seventeenth.
	 */
	CASE("SDP_CODEC_MAX carries sixteen formats");
	CHECK_UINT(SDP_CODEC_MAX, 16);

	CASE("ten offered payload types all fit");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 97 98 99 100 102 103 104 105 106\r\n"));
	CHECK_UINT(m.audio.fmts, 10);
	CHECK_UINT(m.audio.fmt[9].pt, 106);

	CASE("payload types beyond SDP_CODEC_MAX are dropped");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 97 98 99 100 102 103 104 105 106 "
		    "107 108 109 110 111 112 113 114\r\n"));
	CHECK_UINT(m.audio.fmts, SDP_CODEC_MAX);
	CHECK_UINT(m.audio.fmt[0].pt, 96);
	CHECK_UINT(m.audio.fmt[SDP_CODEC_MAX - 1].pt, 112);

	CASE("a payload type above 127 is ignored");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 200 0\r\n"));
	CHECK_UINT(m.audio.fmts, 1);
	CHECK_UINT(m.audio.fmt[0].pt, 0);

	CASE("a dynamic payload type without an rtpmap has no name");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\n"));
	CHECK_UINT(m.audio.fmts, 1);
	CHECK_UINT(m.audio.fmt[0].name[0], 0);
	CHECK_UINT(m.audio.fmt[0].rate, 0);

	CASE("an rtpmap encoding parameter is not part of the rate");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 PCMU/8000/1\r\n"));
	CHECK(!strcmp(fmt_name(0), "PCMU"));
	CHECK_UINT(m.audio.fmt[0].rate, 8000);

	CASE("an rtpmap for an unlisted payload type is ignored");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=rtpmap:97 G722/8000\r\n"));
	CHECK_UINT(m.audio.fmts, 1);
	CHECK(!strcmp(fmt_name(0), "PCMU"));

	CASE("an unlisted telephone-event rtpmap does not fill dtmf_pt");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"
		    "a=rtpmap:96 telephone-event/16000\r\n"));
	CHECK(m.audio.dtmf_pt == -1);
	CHECK_UINT(m.audio.fmts, 1);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=rtpmap:96 telephone-event/8000\r\n"));
	CHECK(m.audio.dtmf_pt == -1);

	CASE("an unlisted CN rtpmap does not fill cn_pt");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=rtpmap:97 CN/8000\r\n"));
	CHECK(m.audio.cn_pt == -1);
	CHECK_UINT(m.audio.fmts, 1);

	CASE("a telephone-event at another rate is dropped without a payload type");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 96\r\n"
		    "a=rtpmap:96 telephone-event/16000\r\n"));
	CHECK(m.audio.dtmf_pt == -1);
	CHECK_UINT(m.audio.fmts, 1);
	CHECK_UINT(m.audio.fmt[0].pt, 0);

	CASE("a CN at another rate is dropped without a payload type");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 97\r\na=rtpmap:97 CN/16000\r\n"));
	CHECK(m.audio.cn_pt == -1);
	CHECK_UINT(m.audio.fmts, 1);
	CHECK_UINT(m.audio.fmt[0].pt, 0);

	CASE("the session c= applies to the audio stream");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(!strcmp(m.audio.addr, "198.51.100.7"));
	CHECK_UINT(m.audio.ipver, 4);

	CASE("a media c= overrides the session c=");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\nc=IN IP4 203.0.113.9\r\n"));
	CHECK(!strcmp(m.audio.addr, "203.0.113.9"));
	CHECK(!m.addr_zero);

	CASE("c=0.0.0.0 at session level sets addr_zero");
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 198.51.100.7\r\nc=IN IP4 0.0.0.0\r\n"
		    "t=0 0\r\nm=audio 5004 RTP/AVP 0\r\n"));
	CHECK(m.addr_zero);
	CHECK(!strcmp(m.audio.addr, "0.0.0.0"));

	CASE("c=0.0.0.0 at media level sets addr_zero");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\nc=IN IP4 0.0.0.0\r\n"));
	CHECK(m.addr_zero);

	CASE("c=0.0.0.0 at both levels sets addr_zero");
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\n"
		    "m=audio 5004 RTP/AVP 0\r\nc=IN IP4 0.0.0.0\r\n"));
	CHECK(m.addr_zero);

	CASE("a real media c= beats a zero session c= (RFC 4566 precedence)");
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\n"
		    "m=audio 5004 RTP/AVP 0\r\nc=IN IP4 203.0.113.9\r\n"));
	CHECK(!m.addr_zero);
	CHECK(!strcmp(m.audio.addr, "203.0.113.9"));

	CASE("an IP6 c= line sets ipver 6");
	CHECK(parse("v=0\r\no=- 1 1 IN IP6 2001:db8::1\r\nc=IN IP6 2001:db8::1\r\n"
		    "t=0 0\r\nm=audio 5004 RTP/AVP 0\r\n"));
	CHECK_UINT(m.audio.ipver, 6);
	CHECK(!strcmp(m.audio.addr, "2001:db8::1"));
	CHECK(!m.addr_zero);

	CASE("no c= line leaves the address empty and ipver 4");
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 198.51.100.7\r\nt=0 0\r\n"
		    "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK_UINT(m.audio.addr[0], 0);
	CHECK_UINT(m.audio.ipver, 4);
	CHECK(!m.addr_zero);

	CASE("an oversized connection address falls back to empty");
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 198.51.100.7\r\n"
		    "c=IN IP4 0123456789012345678901234567890123456789012345678901234\r\n"
		    "t=0 0\r\nm=audio 5004 RTP/AVP 0\r\n"));
	CHECK_UINT(m.audio.addr[0], 0);
	CHECK(!m.addr_zero);

	CASE("a session direction is the media default");
	CHECK(parse(HEAD "a=sendonly\r\nm=audio 5004 RTP/AVP 0\r\n"));
	CHECK_UINT(m.audio.dir, SDP_SENDONLY);
	CHECK(m.audio.dir_present);

	CASE("a media direction overrides the session direction");
	CHECK(parse(HEAD "a=sendonly\r\nm=audio 5004 RTP/AVP 0\r\na=inactive\r\n"));
	CHECK_UINT(m.audio.dir, SDP_INACTIVE);
	CHECK(m.audio.dir_present);

	CASE("an absent direction is sendrecv and dir_present false");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK_UINT(m.audio.dir, SDP_SENDRECV);
	CHECK(!m.audio.dir_present);

	CASE("every direction attribute is recognized");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=recvonly\r\n"));
	CHECK_UINT(m.audio.dir, SDP_RECVONLY);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=SendRecv\r\n"));
	CHECK_UINT(m.audio.dir, SDP_SENDRECV);
	CHECK(m.audio.dir_present);

	CASE("a=ptime fills ptime_ms");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=ptime:30\r\n"));
	CHECK_UINT(m.audio.ptime_ms, 30);

	CASE("an unparsable ptime stays zero");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=ptime:abc\r\n"));
	CHECK_UINT(m.audio.ptime_ms, 0);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=ptime:9000\r\n"));
	CHECK_UINT(m.audio.ptime_ms, 0);

	CASE("telephone-event is taken out of fmt[]");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 101\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\na=fmtp:101 0-15\r\n"));
	CHECK_UINT(m.audio.dtmf_pt, 101);
	CHECK_UINT(m.audio.fmts, 1);
	CHECK_UINT(m.audio.fmt[0].pt, 0);

	CASE("telephone-event is matched case insensitively");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 96\r\n"
		    "a=rtpmap:96 Telephone-Event/8000\r\n"));
	CHECK_UINT(m.audio.dtmf_pt, 96);
	CHECK_UINT(m.audio.fmts, 1);

	CASE("no telephone-event leaves dtmf_pt at -1");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(m.audio.dtmf_pt == -1);
	CHECK(m.audio.cn_pt == -1);

	CASE("CN with an rtpmap is taken out of fmt[]");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 97\r\na=rtpmap:97 CN/8000\r\n"));
	CHECK_UINT(m.audio.cn_pt, 97);
	CHECK_UINT(m.audio.fmts, 1);
	CHECK_UINT(m.audio.fmt[0].pt, 0);

	CASE("payload type 13 is CN without an rtpmap");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 13\r\n"));
	CHECK_UINT(m.audio.cn_pt, 13);
	CHECK_UINT(m.audio.fmts, 1);
	CHECK_UINT(m.audio.fmt[0].pt, 0);

	CASE("an rtpmap overrides the static CN meaning of payload type 13");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 13\r\na=rtpmap:13 PCMA/8000\r\n"));
	CHECK(m.audio.cn_pt == -1);
	CHECK_UINT(m.audio.fmts, 1);
	CHECK_UINT(m.audio.fmt[0].pt, 13);
	CHECK(!strcmp(fmt_name(0), "PCMA"));

	CASE("moving CN out of fmt[] keeps the codec order");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 8 13 0\r\n"));
	CHECK_UINT(m.audio.cn_pt, 13);
	CHECK_UINT(m.audio.fmts, 2);
	CHECK_UINT(m.audio.fmt[0].pt, 8);
	CHECK_UINT(m.audio.fmt[1].pt, 0);

	CASE("m=image is parsed");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\nm=image 5006 udptl t38\r\n"
		    "a=T38FaxVersion:0\r\n"));
	CHECK(m.image.present);
	CHECK_UINT(m.image.port, 5006);
	CHECK(m.audio.present);
	CHECK_UINT(m.audio.port, 5004);

	CASE("an image only body parses with no audio");
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\n"));
	CHECK(m.image.present);
	CHECK(!m.audio.present);

	CASE("the first m=audio wins");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\nm=audio 7000 RTP/AVP 8\r\n"));
	CHECK_UINT(m.audio.port, 5004);
	CHECK_UINT(m.audio.fmts, 1);
	CHECK_UINT(m.audio.fmt[0].pt, 0);

	CASE("attributes after a second m=audio do not reach the first");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\nm=audio 7000 RTP/AVP 8 101\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\na=ptime:30\r\n"
		    "c=IN IP4 203.0.113.9\r\na=sendonly\r\n"));
	CHECK_UINT(m.audio.port, 5004);
	CHECK(m.audio.dtmf_pt == -1);
	CHECK_UINT(m.audio.ptime_ms, 0);
	CHECK_UINT(m.audio.dir, SDP_SENDRECV);
	CHECK(!strcmp(m.audio.addr, "198.51.100.7"));

	CASE("an unrelated media section does not reach the audio stream");
	CHECK(parse(HEAD "m=video 9000 RTP/AVP 34\r\na=sendonly\r\n"
		    "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(m.audio.present);
	CHECK_UINT(m.audio.port, 5004);
	CHECK_UINT(m.audio.dir, SDP_SENDRECV);

	CASE("RTP/AVP sets proto_ok");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(m.audio.proto_ok);

	CASE("the transport is matched case insensitively");
	CHECK(parse(HEAD "m=audio 5004 rtp/avp 0\r\n"));
	CHECK(m.audio.proto_ok);

	CASE("RTP/SAVP clears proto_ok");
	CHECK(parse(HEAD "m=audio 5004 RTP/SAVP 0\r\n"));
	CHECK(m.audio.present);
	CHECK(!m.audio.proto_ok);
	CHECK(parse(HEAD "m=audio 5004 RTP/SAVPF 0\r\n"));
	CHECK(!m.audio.proto_ok);
	CHECK(parse(HEAD "m=audio 5004 UDP/TLS/RTP/SAVP 0\r\n"));
	CHECK(!m.audio.proto_ok);

	CASE("an m= port count suffix is ignored");
	CHECK(parse(HEAD "m=audio 5004/2 RTP/AVP 0\r\n"));
	CHECK_UINT(m.audio.port, 5004);

	CASE("an m= port of zero parses as a refused stream");
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 0\r\n"));
	CHECK(m.audio.present);
	CHECK_UINT(m.audio.port, 0);

	CASE("an unparsable m= port is zero");
	CHECK(parse(HEAD "m=audio abc RTP/AVP 0\r\n"));
	CHECK_UINT(m.audio.port, 0);
	CHECK(parse(HEAD "m=audio 70000 RTP/AVP 0\r\n"));
	CHECK_UINT(m.audio.port, 0);

	CASE("a truncated m= line still counts as a media section");
	CHECK(parse(HEAD "m=audio\r\n"));
	CHECK(m.audio.present);
	CHECK_UINT(m.audio.fmts, 0);
}

/*
 * The pre-ring predicate. call.c asks it before an inbound leg rings, so its
 * acceptance set must be the address acceptance set of the answer refusal
 * itself. Every form that the answer refuses for its address is refused here,
 * and nothing else is. The cases below read the address cases of
 * answer_tests() through the predicate, the two prefix-strict forms included.
 * A predicate that answers a different question therefore fails by name.
 */
static void usable_tests(void)
{
	CASE("usable: an IPv4 literal c= is usable");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_offer_usable(&m));

	CASE("usable: an IPv6 literal c= is usable");
	CHECK(parse("v=0\r\no=- 1 1 IN IP6 2001:db8::1\r\nc=IN IP6 2001:db8::1\r\n"
		    "t=0 0\r\nm=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_offer_usable(&m));

	CASE("usable: a c= that names a host is not");
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 sip.example.com\r\nc=IN IP4 sip.example.com\r\n"
		    "t=0 0\r\nm=audio 5004 RTP/AVP 0\r\n"));
	CHECK(!sdp_offer_usable(&m));

	CASE("usable: the c=0.0.0.0 hold form is a literal and stays usable");
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\n"
		    "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_offer_usable(&m));

	CASE("usable: the RFC 4566 multicast TTL notation is not");
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 224.2.1.1\r\nc=IN IP4 224.2.1.1/127\r\n"
		    "t=0 0\r\nm=audio 5004 RTP/AVP 0\r\n"));
	CHECK(!sdp_offer_usable(&m));

	CASE("usable: an IP6 c= with a zone id is not");
	CHECK(parse("v=0\r\no=- 1 1 IN IP6 fe80::1\r\nc=IN IP6 fe80::1%eth0\r\n"
		    "t=0 0\r\nm=audio 5004 RTP/AVP 0\r\n"));
	CHECK(!sdp_offer_usable(&m));

	CASE("usable: the media c= is what it reads, and it wins over the session c=");
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 sip.example.com\r\nc=IN IP4 sip.example.com\r\n"
		    "t=0 0\r\nm=audio 5004 RTP/AVP 0\r\nc=IN IP4 203.0.113.9\r\n"));
	CHECK(sdp_offer_usable(&m));

	CASE("usable: and a host media c= is refused over a literal session c=");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\nc=IN IP4 sip.example.com\r\n"));
	CHECK(!sdp_offer_usable(&m));

	CASE("usable: a host c= on a media section we ignore changes nothing");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"
		    "m=video 5006 RTP/AVP 96\r\nc=IN IP4 video.example.com\r\n"));
	CHECK(sdp_offer_usable(&m));

	CASE("usable: an offer with no connection address at all is not usable");
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 198.51.100.7\r\nt=0 0\r\n"
		    "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(!sdp_offer_usable(&m));

	/*
	 * The predicate answers the address question alone. An offer the
	 * answer refuses for its port, its transport or its codec list passes
	 * here and meets sdp_answer_build() as any other offer does.
	 */
	CASE("usable: a port zero offer is usable, the port gate is elsewhere");
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 0\r\n"));
	CHECK(sdp_offer_usable(&m));

	CASE("usable: an SRTP offer is usable, the transport gate is elsewhere");
	CHECK(parse(HEAD "m=audio 5004 RTP/SAVP 0\r\n"));
	CHECK(sdp_offer_usable(&m));

	CASE("usable: an offer with no codec we share is usable too");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 99\r\na=rtpmap:99 SPEEX/8000\r\n"));
	CHECK(sdp_offer_usable(&m));

	CASE("usable: a NULL offer is not usable");
	CHECK(!sdp_offer_usable(NULL));
}

/*
 * The G.711 purity predicate, the SDP half of the far-end fax upspeed
 * recognizer (call.c fax_follow_check). A verdict of true converts a live call
 * to a fax carrier. Every case below is therefore a way in which a body could
 * look G.711-only without being one. The two hardest are the ones that the
 * parser itself hides. One is a second m=audio section, whose codec list never
 * reaches offer->audio at all. The other is an m= line longer than fmt[], whose
 * tail is dropped silently.
 */
static void g711_only_tests(void)
{
	unsigned int i;
	char body[2048];
	size_t n;

	CASE("g711 only: the static payload types alone, with no rtpmap at all");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 8\r\n"));
	CHECK(sdp_audio_g711_only(&m));

	CASE("g711 only: one of them is enough");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_audio_g711_only(&m));

	CASE("g711 only: and the rtpmap spelling of the same pair");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 97\r\n"
		    "a=rtpmap:96 PCMU/8000\r\na=rtpmap:97 pcma/8000\r\n"));
	CHECK(sdp_audio_g711_only(&m));

	/* both leave fmt[] before the predicate walks it, so the narrowing offer
	 * a real gateway sends still reads pure */
	CASE("g711 only: telephone-event and comfort noise beside them do not spoil it");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 8 101 13\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\na=rtpmap:13 CN/8000\r\n"));
	CHECK(sdp_audio_g711_only(&m));

	CASE("g711 only: a G.729 in the list is not");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 18 101\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\n"));
	CHECK(!sdp_audio_g711_only(&m));

	CASE("g711 only: nor a name this layer cannot map");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 99\r\na=rtpmap:99 SPEEX/8000\r\n"));
	CHECK(!sdp_audio_g711_only(&m));

	/* a dynamic type with no rtpmap names no format at all, and a verdict
	 * taken over it would be taken over a codec nobody described */
	CASE("g711 only: nor a dynamic type the offer never mapped");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 96\r\n"));
	CHECK(!sdp_audio_g711_only(&m));

	CASE("g711 only: nor PCMU at a clock this side cannot key");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 PCMU/16000\r\n"));
	CHECK(!sdp_audio_g711_only(&m));

	CASE("g711 only: an empty format list is not");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 101\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\n"));
	CHECK_UINT(m.audio.fmts, 0);
	CHECK(!sdp_audio_g711_only(&m));

	CASE("g711 only: and an offer with no audio section at all is not");
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\n"));
	CHECK(!sdp_audio_g711_only(&m));

	CASE("g711 only: a hold offer is not, in either spelling");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=sendonly\r\n"));
	CHECK(!sdp_audio_g711_only(&m));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=inactive\r\n"));
	CHECK(!sdp_audio_g711_only(&m));
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\n"
		    "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(!sdp_audio_g711_only(&m));

	CASE("g711 only: nor a recvonly one");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=recvonly\r\n"));
	CHECK(!sdp_audio_g711_only(&m));

	/* an absent direction is sendrecv (RFC 3264 5.1), and the cases above
	 * carry none; the explicit spelling reads the same */
	CASE("g711 only: an explicit sendrecv attribute reads the same as none");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 8\r\na=sendrecv\r\n"));
	CHECK(sdp_audio_g711_only(&m));

	/* the T.38 half of the same question: an image section is a different
	 * conversation and the G.711 profile has no place in it */
	CASE("g711 only: an image section beside the audio one is not");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\nm=image 5006 udptl t38\r\n"));
	CHECK(!sdp_audio_g711_only(&m));

	/*
	 * The parser keeps the FIRST m=audio and sends every later one to its
	 * sink, so offer->audio is not the whole offer here. A gateway that put
	 * G.729 in the second section would be followed on a list this layer
	 * never saw.
	 */
	CASE("g711 only: a second m=audio section refuses the whole offer");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 8\r\n"
		    "m=audio 5008 RTP/AVP 18\r\na=rtpmap:18 G729/8000\r\n"));
	CHECK(m.audio_dup);
	CHECK(!sdp_audio_g711_only(&m));

	/*
	 * The other hidden case, and the reason the parser carries a flag for
	 * it rather than the predicate testing fmts against the cap: the list
	 * below overflows fmt[] with G.711 entries, drops a G.729, and then
	 * SHRINKS BACK under the cap when telephone-event leaves fmt[]. A
	 * predicate that refused a full list alone would follow this offer.
	 */
	CASE("g711 only: an m= line longer than fmt[] refuses the offer");
	n = (size_t)snprintf(body, sizeof(body), HEAD "m=audio 5004 RTP/AVP 0 8 101");
	for (i = 0; i < SDP_CODEC_MAX - 3; i++)
		n += (size_t)snprintf(body + n, sizeof(body) - n, " %u", 102 + i);
	n += (size_t)snprintf(body + n, sizeof(body) - n, " 18\r\n"
			      "a=rtpmap:101 telephone-event/8000\r\n"
			      "a=rtpmap:18 G729/8000\r\n");
	for (i = 0; i < SDP_CODEC_MAX - 3; i++)
		n += (size_t)snprintf(body + n, sizeof(body) - n,
				      "a=rtpmap:%u PCMU/8000\r\n", 102 + i);
	CHECK(parse(body));
	CHECK(m.audio.fmt_dropped);
	CHECK(m.audio.fmts < SDP_CODEC_MAX);
	CHECK(!sdp_audio_g711_only(&m));

	CASE("g711 only: a NULL offer is not");
	CHECK(!sdp_audio_g711_only(NULL));
}

/*
 * The plain offer, byte for byte. It sits at file scope because the SRTP group
 * asserts the same text for a leg with srtp off: an offer without SRTP keeps
 * the shape it had before SRTP existed.
 */
static const char offer_plain_want[] =
	"v=0\r\n"
	"o=- 42 0 IN IP4 192.0.2.10\r\n"
	"s=-\r\n"
	"c=IN IP4 192.0.2.10\r\n"
	"t=0 0\r\n"
	"m=audio 4000 RTP/AVP 0 18 101 13\r\n"
	"a=rtpmap:0 PCMU/8000\r\n"
	"a=rtpmap:18 G729/8000\r\n"
	"a=fmtp:18 annexb=yes\r\n"
	"a=rtpmap:101 telephone-event/8000\r\n"
	"a=fmtp:101 0-15\r\n"
	"a=rtpmap:13 CN/8000\r\n"
	"a=ptime:20\r\n";

/* the local view the text above belongs to */
static void offer_plain_local_init(struct sdp_local *loc)
{
	local_init(loc);
	loc->codec[1].id = SDP_CODEC_G729;
	loc->codec[1].pt = 18;
	loc->cn = true;
}

static void offer_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;

	CASE("the offer is byte exact");
	offer_plain_local_init(&loc);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!strcmp(out, offer_plain_want));

	CASE("an offer without a usable codec fails");
	local_init(&loc);
	loc.codecs = 0;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(!sdp_offer_build(&st, &loc, &b));
	CHECK_UINT(sip_buf_len(&b), 0);

	CASE("a codec with payload type 129 is disabled and never offered");
	local_init(&loc);
	loc.codec[0].pt = SDP_PT_DISABLED;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 8 101\r\n"));
	CHECK(!has("PCMU"));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMA);

	CASE("every disabled codec fails the offer");
	local_init(&loc);
	loc.codec[0].pt = SDP_PT_DISABLED;
	loc.codec[1].pt = SDP_PT_DISABLED;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(!sdp_offer_build(&st, &loc, &b));

	/*
	 * The advertisement is a receive-side claim, decoupled from silence
	 * suppression, so both rows below
	 * write the same line and only the recorded TX-arm intent moves with
	 * vad.
	 */
	CASE("annexb=yes follows the G729 rtpmap when vad is off");
	local_init(&loc);
	loc.codec[0].id = SDP_CODEC_G729;
	loc.codec[0].pt = 18;
	loc.codec[0].vad = false;
	loc.codecs = 1;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=rtpmap:18 G729/8000\r\na=fmtp:18 annexb=yes\r\n"));
	CHECK(!st.offered.g729_annexb);

	CASE("annexb=yes follows the G729 rtpmap when vad is on too");
	loc.codec[0].vad = true;
	loc.cn = true;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=rtpmap:18 G729/8000\r\na=fmtp:18 annexb=yes\r\n"));
	CHECK(st.offered.g729_annexb);

	CASE("cn_enable off keeps the G729 annexb line but arms nothing");
	local_init(&loc);
	loc.codec[0].id = SDP_CODEC_G729;
	loc.codec[0].pt = 18;
	loc.codec[0].vad = true;
	loc.cn = false;
	loc.codecs = 1;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=rtpmap:18 G729/8000\r\na=fmtp:18 annexb=yes\r\n"));
	CHECK(!st.offered.g729_annexb);

	CASE("annexb is never written for G711");
	local_init(&loc);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!has("annexb"));

	CASE("the telephone-event block is absent when dtmf_pt is zero");
	local_init(&loc);
	loc.dtmf_pt = 0;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 0 8\r\n"));
	CHECK(!has("telephone-event"));
	CHECK(!has("a=fmtp:"));
	CHECK(st.proposed.dtmf_pt == -1);

	CASE("CN is offered only when cn is set");
	local_init(&loc);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!has("CN/8000"));
	CHECK(st.proposed.cn_pt == -1);
	loc.cn = true;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=rtpmap:13 CN/8000\r\n"));
	CHECK(has("m=audio 4000 RTP/AVP 0 8 101 13\r\n"));
	CHECK_UINT(st.proposed.cn_pt, 13);

	CASE("the ptime line carries the configured value");
	local_init(&loc);
	loc.ptime_ms = 30;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=ptime:30\r\n"));
	CHECK_UINT(st.proposed.ptime_ms, 30);

	CASE("an offer without hold carries no direction attribute");
	local_init(&loc);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!has("a=sendonly"));
	CHECK(!has("a=sendrecv"));
	CHECK(!has("a=inactive"));
	CHECK_UINT(st.proposed.dir, SDP_SENDRECV);

	CASE("a hold offer is sendonly");
	local_init(&loc);
	sdp_state_init(&st, 42);
	sdp_hold_set(&st, true);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=sendonly\r\n"));
	CHECK_UINT(st.proposed.dir, SDP_SENDONLY);

	CASE("a hold offer against a held peer is inactive");
	local_init(&loc);
	sdp_state_init(&st, 42);
	st.committed.remote_hold = true;
	sdp_hold_set(&st, true);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=inactive\r\n"));
	CHECK_UINT(st.proposed.dir, SDP_INACTIVE);

	CASE("hold clears again");
	sdp_hold_set(&st, false);
	CHECK(!sdp_local_hold(&st));

	CASE("the session version increments across offers");
	local_init(&loc);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("o=- 42 0 IN IP4 192.0.2.10\r\n"));
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("o=- 42 1 IN IP4 192.0.2.10\r\n"));
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("o=- 42 2 IN IP4 192.0.2.10\r\n"));
	CHECK_UINT(st.sess_ver, 3);

	CASE("an IP6 local address writes IP6");
	local_init(&loc);
	snprintf(loc.addr, sizeof(loc.addr), "%s", "2001:db8::10");
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("o=- 42 0 IN IP6 2001:db8::10\r\n"));
	CHECK(has("c=IN IP6 2001:db8::10\r\n"));

	CASE("the offer fills proposed and sets offer_pending");
	local_init(&loc);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(st.offer_pending);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);
	CHECK_UINT(st.proposed.codec.pt, 0);
	CHECK_UINT(st.proposed.dtmf_pt, 101);
	CHECK_UINT(st.proposed.ptime_ms, 20);

	CASE("the offer proposes no remote address until the answer");
	CHECK(!st.proposed.valid);
	CHECK_UINT(st.proposed.remote_port, 0);
	CHECK_UINT(st.proposed.remote_addr[0], 0);
	CHECK(!st.proposed.remote_hold);

	CASE("a NULL argument refuses the offer");
	CHECK(!sdp_offer_build(NULL, &loc, &b));
	CHECK(!sdp_offer_build(&st, NULL, &b));
	CHECK(!sdp_offer_build(&st, &loc, NULL));

	CASE("an offer that does not fit reports the overflow");
	local_init(&loc);
	sdp_state_init(&st, 42);
	sip_buf_init(&b, out, 32);
	CHECK(!sdp_offer_build(&st, &loc, &b));
	CHECK(!sip_buf_ok(&b));

	CASE("an offer that does not fit leaves the state untouched");
	CHECK_UINT(st.sess_ver, 0);
	CHECK(!st.offer_pending);
	CHECK(!st.proposed.valid);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_NONE);
	CHECK_UINT(st.proposed.ptime_ms, 0);
	CHECK(st.proposed.dtmf_pt == -1);

	CASE("a second offer after an overflow still emits version 0");
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("o=- 42 0 IN IP4 192.0.2.10\r\n"));
	CHECK_UINT(st.sess_ver, 1);
}

static void answer_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;

	CASE("the answer follows the offer order, not ours");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 8 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 8\r\n"));
	CHECK(has("a=rtpmap:8 PCMA/8000\r\n"));
	CHECK(!has("PCMU"));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMA);

	CASE("the answer is byte exact");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 101\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\na=ptime:20\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!strcmp(out,
		      "v=0\r\n"
		      "o=- 55 0 IN IP4 192.0.2.10\r\n"
		      "s=-\r\n"
		      "c=IN IP4 192.0.2.10\r\n"
		      "t=0 0\r\n"
		      "m=audio 4000 RTP/AVP 0 101\r\n"
		      "a=rtpmap:0 PCMU/8000\r\n"
		      "a=rtpmap:101 telephone-event/8000\r\n"
		      "a=fmtp:101 0-15\r\n"
		      "a=ptime:20\r\n"
		      "a=sendrecv\r\n"));

	CASE("the answer mirrors the offered payload type");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 PCMA/8000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 96\r\n"));
	CHECK(has("a=rtpmap:96 PCMA/8000\r\n"));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMA);
	CHECK_UINT(st.proposed.codec.pt, 96);

	CASE("the answer mirrors the offered telephone-event payload type");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 96\r\n"
		    "a=rtpmap:96 telephone-event/8000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 0 96\r\n"));
	CHECK(has("a=rtpmap:96 telephone-event/8000\r\n"));
	CHECK(has("a=fmtp:96 0-15\r\n"));
	CHECK(!has("101"));
	CHECK_UINT(st.proposed.dtmf_pt, 96);

	CASE("no telephone-event in the offer leaves it out of the answer");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!has("telephone-event"));
	CHECK(st.proposed.dtmf_pt == -1);

	CASE("our disabled telephone-event leaves it out of the answer");
	local_init(&loc);
	loc.dtmf_pt = 0;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 101\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!has("telephone-event"));
	CHECK(st.proposed.dtmf_pt == -1);

	CASE("CN is answered only when both sides carry it");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 13\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!has("CN/8000"));
	CHECK(st.proposed.cn_pt == -1);
	loc.cn = true;
	sdp_state_init(&st, 55);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 0 13\r\n"));
	CHECK(has("a=rtpmap:13 CN/8000\r\n"));
	CHECK_UINT(st.proposed.cn_pt, 13);

	CASE("CN is not answered when the offer omits it");
	local_init(&loc);
	loc.cn = true;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!has("CN/8000"));
	CHECK(st.proposed.cn_pt == -1);

	CASE("no common codec returns SDP_NO_CODEC and leaves the buffer untouched");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!strcmp(out, ""));
	CHECK(!st.proposed.valid);
	CHECK_UINT(st.sess_ver, 0);

	CASE("an unnamed dynamic payload type is not usable");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);

	CASE("a codec at the wrong clock rate is not usable");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 PCMU/16000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);

	CASE("an offer without audio is SDP_BAD");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_BAD);
	CHECK_UINT(sip_buf_len(&b), 0);

	CASE("an offered audio port of zero is SDP_NO_CODEC");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);

	CASE("a T.38 offer is refused at port 0 and the audio still negotiates");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\nm=image 5006 udptl t38\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 0\r\n"));
	CHECK(has("a=rtpmap:0 PCMU/8000\r\n"));
	CHECK(has("m=image 0 udptl t38\r\n"));
	CHECK(st.proposed.image_refused);
	CHECK(st.proposed.valid);

	CASE("the image line is the last line of the answer");
	CHECK(strlen(out) > 21);
	CHECK(!strcmp(out + strlen(out) - 21, "m=image 0 udptl t38\r\n"));

	CASE("no image line without a T.38 offer");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!has("m=image"));
	CHECK(!st.proposed.image_refused);

	CASE("a peer sendonly offer answers recvonly");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=sendonly\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=recvonly\r\n"));
	CHECK_UINT(st.proposed.dir, SDP_RECVONLY);
	CHECK(st.proposed.remote_hold);

	CASE("a peer sendonly offer with local hold answers inactive");
	local_init(&loc);
	sdp_state_init(&st, 55);
	sdp_hold_set(&st, true);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=inactive\r\n"));
	CHECK_UINT(st.proposed.dir, SDP_INACTIVE);
	CHECK(st.proposed.remote_hold);

	CASE("a peer inactive offer answers inactive");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=inactive\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=inactive\r\n"));
	CHECK_UINT(st.proposed.dir, SDP_INACTIVE);
	CHECK(st.proposed.remote_hold);

	CASE("a peer inactive offer with local hold stays inactive");
	sdp_state_init(&st, 55);
	sdp_hold_set(&st, true);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK_UINT(st.proposed.dir, SDP_INACTIVE);

	CASE("a peer sendrecv offer answers sendrecv explicitly");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=sendrecv\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=ptime:20\r\na=sendrecv\r\n"));
	CHECK(!has("a=sendonly"));
	CHECK_UINT(st.proposed.dir, SDP_SENDRECV);
	CHECK(!st.proposed.remote_hold);

	CASE("a peer sendrecv offer with local hold answers sendonly");
	sdp_state_init(&st, 55);
	sdp_hold_set(&st, true);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=sendonly\r\n"));
	CHECK_UINT(st.proposed.dir, SDP_SENDONLY);
	CHECK(!st.proposed.remote_hold);

	CASE("an offer without a direction answers sendrecv");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(!m.audio.dir_present);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=sendrecv\r\n"));
	CHECK_UINT(st.proposed.dir, SDP_SENDRECV);

	/* the uas_reinvite_hold scenario: the peer holds, then resumes the call */
	CASE("a resume answer emits a=sendrecv after a hold answer");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=sendonly\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=recvonly\r\n"));
	CHECK(st.proposed.remote_hold);
	sdp_commit(&st);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=sendrecv\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=sendrecv\r\n"));
	CHECK(!has("a=recvonly"));
	CHECK(!st.proposed.remote_hold);
	CHECK_UINT(st.proposed.dir, SDP_SENDRECV);

	CASE("the direction attribute is the last audio attribute of an answer");
	CHECK(has("a=ptime:20\r\na=sendrecv\r\n"));

	CASE("a peer recvonly offer answers sendonly");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=recvonly\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=sendonly\r\n"));
	CHECK_UINT(st.proposed.dir, SDP_SENDONLY);
	CHECK(!st.proposed.remote_hold);

	CASE("a peer recvonly offer with local hold stays sendonly");
	sdp_state_init(&st, 55);
	sdp_hold_set(&st, true);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=sendonly\r\n"));
	CHECK(!has("a=inactive"));
	CHECK_UINT(st.proposed.dir, SDP_SENDONLY);

	CASE("the RFC 2543 c=0.0.0.0 hold answers recvonly");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 198.51.100.7\r\nc=IN IP4 0.0.0.0\r\n"
		    "t=0 0\r\nm=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=recvonly\r\n"));
	CHECK_UINT(st.proposed.dir, SDP_RECVONLY);
	CHECK(st.proposed.remote_hold);

	CASE("the RFC 2543 hold with local hold answers inactive");
	sdp_state_init(&st, 55);
	sdp_hold_set(&st, true);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=inactive\r\n"));
	CHECK_UINT(st.proposed.dir, SDP_INACTIVE);

	CASE("an offered ptime of 10 is taken");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=ptime:10\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=ptime:10\r\n"));
	CHECK_UINT(st.proposed.ptime_ms, 10);

	CASE("an offered ptime of 30 is taken");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=ptime:30\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=ptime:30\r\n"));
	CHECK_UINT(st.proposed.ptime_ms, 30);

	CASE("an offered ptime of 40 falls back to the configured value");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=ptime:40\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=ptime:20\r\n"));
	CHECK_UINT(st.proposed.ptime_ms, 20);

	CASE("an absent ptime falls back to the configured value");
	local_init(&loc);
	loc.ptime_ms = 30;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=ptime:30\r\n"));

	CASE("a pinned ptime wins over the offered one");
	local_init(&loc);
	loc.ptime_ms = 30;
	loc.ptime_pin = true;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=ptime:20\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=ptime:30\r\n"));
	CHECK_UINT(st.proposed.ptime_ms, 30);

	CASE("ptime_omit leaves a=ptime out of an offer and an answer");
	local_init(&loc);
	loc.ptime_ms = 30;
	loc.ptime_omit = true;
	sdp_state_init(&st, 56);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!has("a=ptime"));
	CHECK_UINT(st.proposed.ptime_ms, 30);
	sdp_state_init(&st, 57);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=ptime:20\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!has("a=ptime"));
	CHECK_UINT(st.proposed.ptime_ms, 20);

	CASE("G729 is answered with annexb=yes when our vad is off");
	local_init(&loc);
	loc.codec[0].id = SDP_CODEC_G729;
	loc.codec[0].pt = 18;
	loc.codec[0].vad = false;
	loc.codecs = 1;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=rtpmap:18 G729/8000\r\na=fmtp:18 annexb=yes\r\n"));
	CHECK(!st.proposed.codec.annexb);

	CASE("G729 with our vad on writes the same line and still does not arm");
	loc.codec[0].vad = true;
	sdp_state_init(&st, 55);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=fmtp:18 annexb=yes\r\n"));
	CHECK(st.proposed.codec.vad);
	CHECK(!st.proposed.codec.annexb);

	CASE("the mirrored G729 payload type carries the annexb line");
	local_init(&loc);
	loc.codec[0].id = SDP_CODEC_G729;
	loc.codec[0].pt = 18;
	loc.codecs = 1;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 99\r\na=rtpmap:99 G729/8000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 99\r\n"));
	CHECK(has("a=fmtp:99 annexb=yes\r\n"));

	CASE("the answer fills the remote address, port and ip version");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\nc=IN IP4 203.0.113.9\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!strcmp(st.proposed.remote_addr, "203.0.113.9"));
	CHECK_UINT(st.proposed.remote_port, 5004);
	CHECK_UINT(st.proposed.remote_ipver, 4);
	CHECK(st.proposed.valid);

	CASE("the answer records an IP6 remote address");
	local_init(&loc);
	snprintf(loc.addr, sizeof(loc.addr), "%s", LOCAL6);
	sdp_state_init(&st, 55);
	CHECK(parse("v=0\r\no=- 1 1 IN IP6 2001:db8::1\r\nc=IN IP6 2001:db8::1\r\n"
		    "t=0 0\r\nm=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!strcmp(st.proposed.remote_addr, "2001:db8::1"));
	CHECK_UINT(st.proposed.remote_ipver, 6);

	CASE("the answer clears offer_pending");
	local_init(&loc);
	sdp_state_init(&st, 55);
	st.offer_pending = true;
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!st.offer_pending);

	CASE("the answer session version increments after the first answer");
	local_init(&loc);
	sdp_state_init(&st, 55);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("o=- 55 0 IN IP4 192.0.2.10\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("o=- 55 1 IN IP4 192.0.2.10\r\n"));

	CASE("an SRTP offer is refused with SDP_NO_CODEC");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/SAVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!st.proposed.valid);
	CHECK_UINT(st.sess_ver, 0);

	CASE("a mixed case RTP/AVP offer is answered");
	CHECK(parse(HEAD "m=audio 5004 rtp/avp 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 0\r\n"));

	CASE("an offer without a connection address is SDP_BAD");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 198.51.100.7\r\nt=0 0\r\n"
		    "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK_UINT(m.audio.addr[0], 0);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_BAD);
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!st.proposed.valid);
	CHECK_UINT(st.sess_ver, 0);

	CASE("an offer whose c= names a host is SDP_NO_CODEC");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 sip.example.com\r\nc=IN IP4 sip.example.com\r\n"
		    "t=0 0\r\nm=audio 5004 RTP/AVP 0\r\n"));
	CHECK(!strcmp(m.audio.addr, "sip.example.com"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!st.proposed.valid);
	CHECK_UINT(st.sess_ver, 0);

	CASE("the same offer with a literal c= is answered");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!strcmp(st.proposed.remote_addr, "198.51.100.7"));
	CHECK(st.proposed.valid);

	CASE("a literal media c= survives a host session c=");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 sip.example.com\r\nc=IN IP4 sip.example.com\r\n"
		    "t=0 0\r\nm=audio 5004 RTP/AVP 0\r\nc=IN IP4 203.0.113.9\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!strcmp(st.proposed.remote_addr, "203.0.113.9"));

	CASE("a host media c= is refused over a literal session c=");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\nc=IN IP4 sip.example.com\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);

	CASE("a host c= on a media section we ignore leaves the offer answerable");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"
		    "m=video 5006 RTP/AVP 96\r\nc=IN IP4 video.example.com\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!strcmp(st.proposed.remote_addr, "198.51.100.7"));

	CASE("the c=0.0.0.0 hold form is a literal and stays a direction change");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\n"
		    "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=recvonly\r\n"));
	CHECK(st.proposed.remote_hold);

	CASE("an IP6 literal c= is answered as it was before the refusal");
	local_init(&loc);
	snprintf(loc.addr, sizeof(loc.addr), "%s", LOCAL6);
	sdp_state_init(&st, 55);
	CHECK(parse("v=0\r\no=- 1 1 IN IP6 2001:db8::1\r\nc=IN IP6 2001:db8::1\r\n"
		    "t=0 0\r\nm=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!strcmp(st.proposed.remote_addr, "2001:db8::1"));
	CHECK_UINT(st.proposed.remote_ipver, 6);

	/*
	 * Both forms carry a valid literal prefix, so a refusal that reads the
	 * leading run alone accepts them and the ring-answer-drop shape returns.
	 */
	CASE("the RFC 4566 multicast TTL notation is SDP_NO_CODEC");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 224.2.1.1\r\nc=IN IP4 224.2.1.1/127\r\n"
		    "t=0 0\r\nm=audio 5004 RTP/AVP 0\r\n"));
	CHECK(!strcmp(m.audio.addr, "224.2.1.1/127"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);

	CASE("an IP6 c= with a zone id is SDP_NO_CODEC");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse("v=0\r\no=- 1 1 IN IP6 fe80::1\r\nc=IN IP6 fe80::1%eth0\r\n"
		    "t=0 0\r\nm=audio 5004 RTP/AVP 0\r\n"));
	CHECK(!strcmp(m.audio.addr, "fe80::1%eth0"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);

	/*
	 * The property call.c leans on when it answers 488 and rolls back: a
	 * refused re-offer moves no state, so the running session survives it.
	 */
	CASE("a refused re-offer leaves the committed exchange as it was");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	sdp_commit(&st);
	CHECK(st.committed.valid);
	CHECK_UINT(st.sess_ver, 1);
	CHECK(parse(HEAD "m=audio 6000 RTP/AVP 0\r\nc=IN IP4 sip.example.com\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK_UINT(st.sess_ver, 1);
	CHECK(st.committed.valid);
	CHECK(!strcmp(st.committed.remote_addr, "198.51.100.7"));
	CHECK_UINT(st.committed.remote_port, 5004);
	CHECK_UINT(st.committed.codec.id, SDP_CODEC_PCMU);

	CASE("the answer mirrors a dynamic CN payload type");
	local_init(&loc);
	loc.cn = true;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 97\r\na=rtpmap:97 CN/8000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 0 97\r\n"));
	CHECK(has("a=rtpmap:97 CN/8000\r\n"));
	CHECK(!has("13"));
	CHECK_UINT(st.proposed.cn_pt, 97);

	CASE("G729A and G729B rtpmap names select G729");
	local_init(&loc);
	loc.codec[0].id = SDP_CODEC_G729;
	loc.codec[0].pt = 18;
	loc.codecs = 1;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 G729A/8000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 96\r\n"));
	CHECK(has("a=rtpmap:96 G729/8000\r\n"));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_G729);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 g729b/8000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_G729);

	CASE("an answer that does not fit is SDP_BAD");
	local_init(&loc);
	sdp_state_init(&st, 55);
	st.offer_pending = true;
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	sip_buf_init(&b, out, 40);
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_BAD);
	CHECK(!sip_buf_ok(&b));

	CASE("an answer that does not fit leaves the state untouched");
	CHECK_UINT(st.sess_ver, 0);
	CHECK(!st.proposed.valid);
	CHECK(st.offer_pending);
	CHECK_UINT(st.proposed.remote_port, 0);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_NONE);

	CASE("the answer after an overflow still emits version 0");
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("o=- 55 0 IN IP4 192.0.2.10\r\n"));
	CHECK_UINT(st.sess_ver, 1);

	CASE("a NULL argument answers SDP_BAD");
	CHECK(sdp_answer_build(NULL, &loc, &m, &b) == SDP_BAD);
	CHECK(sdp_answer_build(&st, NULL, &m, &b) == SDP_BAD);
	CHECK(sdp_answer_build(&st, &loc, NULL, &b) == SDP_BAD);
	CHECK(sdp_answer_build(&st, &loc, &m, NULL) == SDP_BAD);
}

/*
 * The parsed annexb record of one payload type: 1 yes, 0 no, -1 the section
 * wrote none for that format, which is also the answer for a format the
 * section does not hold at all (shared/sdp.h, struct sdp_fmt).
 */
static int annexb_rec(uint8_t pt)
{
	unsigned int i;

	for (i = 0; i < m.audio.fmts; i++) {
		if (m.audio.fmt[i].pt != pt)
			continue;
		if (!m.audio.fmt[i].annexb_set)
			return -1;

		return m.audio.fmt[i].annexb ? 1 : 0;
	}

	return -1;
}

/* does any format of the parsed section carry a record at all */
static bool annexb_none(void)
{
	unsigned int i;

	for (i = 0; i < m.audio.fmts; i++) {
		if (m.audio.fmt[i].annexb_set)
			return false;
	}

	return true;
}

/*
 * The G.729 annexb fmtp: the parse record and its settle against the format
 * that carries it. offer_tests() and answer_tests() cover
 * codec_rtpmap_write() and the offer/answer wire text, beside every other
 * G.729 wire case. The live negotiation matrix is g729_annexb_matrix_tests()
 * below. This group covers the parsed record alone.
 */
static void g729_fmtp_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;

	CASE("g729 fmtp parse: an absent fmtp stores no record");
	local_init(&loc);
	loc.codec[0].id = SDP_CODEC_G729;
	loc.codec[0].pt = 18;
	loc.codec[0].vad = true;
	loc.codecs = 1;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\n"));
	CHECK(annexb_none());

	CASE("g729 fmtp parse: an explicit annexb=no is stored");
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\na=fmtp:18 annexb=no\r\n"));
	CHECK(annexb_rec(18) == 0);

	CASE("g729 fmtp parse: an explicit annexb=yes is stored");
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\na=fmtp:18 annexb=yes\r\n"));
	CHECK(annexb_rec(18) == 1);

	CASE("g729 fmtp parse: a dynamic G729A payload type still binds the record");
	local_init(&loc);
	loc.codec[0].id = SDP_CODEC_G729;
	loc.codec[0].pt = 96;
	loc.codecs = 1;
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 G729A/8000\r\n"
		    "a=fmtp:96 annexb=yes\r\n"));
	CHECK(annexb_rec(96) == 1);

	CASE("g729 fmtp parse: annexb=yes on a non-G729 payload type is dropped");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=fmtp:0 annexb=yes\r\n"));
	CHECK(annexb_none());

	CASE("g729 fmtp parse: a malformed annexb value is ignored");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\na=fmtp:18 annexb=maybe\r\n"));
	CHECK(annexb_none());

	CASE("g729 fmtp parse: a later valid line replaces an earlier one");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\na=fmtp:18 annexb=yes\r\n"
		    "a=fmtp:18 annexb=no\r\n"));
	CHECK(annexb_rec(18) == 0);

	CASE("g729 fmtp parse: a later malformed line does not erase an earlier valid one");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\na=fmtp:18 annexb=yes\r\n"
		    "a=fmtp:18 annexb=maybe\r\n"));
	CHECK(annexb_rec(18) == 1);

	CASE("g729 fmtp parse: an unknown parameter beside annexb changes nothing");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\na=fmtp:18 bitrate=8000;annexb=yes\r\n"));
	CHECK(annexb_rec(18) == 1);

	CASE("g729 fmtp parse: the same pair in the other order reads the same");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\na=fmtp:18 annexb=no;bitrate=8000\r\n"));
	CHECK(annexb_rec(18) == 0);

	CASE("g729 fmtp parse: the key and value are case insensitive");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\na=fmtp:18 ANNEXB=YES\r\n"));
	CHECK(annexb_rec(18) == 1);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\na=fmtp:18 AnnexB=No\r\n"));
	CHECK(annexb_rec(18) == 0);

	CASE("g729 fmtp parse: whitespace around the pair is accepted");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\na=fmtp:18 annexb = yes\r\n"));
	CHECK(annexb_rec(18) == 1);

	CASE("g729 fmtp parse: a bare annexb key with no value is ignored");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\na=fmtp:18 annexb\r\n"));
	CHECK(annexb_none());

	CASE("g729 fmtp parse: annexb= with an empty value is ignored");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\na=fmtp:18 annexb=\r\n"));
	CHECK(annexb_none());

	CASE("g729 fmtp parse: the fmtp line ahead of its rtpmap still binds");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=fmtp:96 annexb=yes\r\n"
		    "a=rtpmap:96 G729/8000\r\n"));
	CHECK(annexb_rec(96) == 1);

	CASE("g729 fmtp parse: an fmtp for an unlisted payload type claims nothing");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\na=fmtp:99 annexb=yes\r\n"));
	CHECK(annexb_none());

	CASE("g729 fmtp parse: annexb on the telephone-event payload type is dropped");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18 101\r\na=rtpmap:101 telephone-event/8000\r\n"
		    "a=fmtp:101 annexb=yes\r\n"));
	CHECK(annexb_none());

	CASE("g729 fmtp parse: annexb on the CN payload type is dropped");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18 13\r\na=rtpmap:13 CN/8000\r\n"
		    "a=fmtp:13 annexb=yes\r\n"));
	CHECK(annexb_none());

	CASE("g729 fmtp parse: pt 18 remapped to another codec drops the record");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\na=rtpmap:18 PCMU/8000\r\n"
		    "a=fmtp:18 annexb=yes\r\n"));
	CHECK(annexb_none());

	CASE("g729 fmtp parse: a G729B rtpmap name still binds the record");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 97\r\na=rtpmap:97 G729B/8000\r\n"
		    "a=fmtp:97 annexb=no\r\n"));
	CHECK(annexb_rec(97) == 0);

	/* local_init() leaves vad clear, which is silence_suppression off: the
	 * four cases below are the ss=0 row of the matrix, where the offer
	 * records its payload type but arms nothing whatever the peer says. */
	CASE("g729 fmtp take: our offer records its own pt and arms nothing with ss off");
	local_init(&loc);
	loc.codec[0].id = SDP_CODEC_G729;
	loc.codec[0].pt = 18;
	loc.codecs = 1;
	take_state_init(&st, &loc);
	CHECK_UINT(st.offered.g729_pt, 18);
	CHECK(!st.offered.g729_annexb);

	CASE("g729 fmtp take: a G729 entry that is not the leading codec still records its pt");
	local_init(&loc);
	loc.codec[1].id = SDP_CODEC_G729;
	loc.codec[1].pt = 18;
	loc.codecs = 2;
	take_state_init(&st, &loc);
	CHECK_UINT(st.offered.g729_pt, 18);
	CHECK(!st.offered.g729_annexb);

	CASE("g729 fmtp take: an offer with no G729 entry records no pt");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(st.offered.g729_pt == -1);

	CASE("g729 fmtp take: an explicit peer yes leaves TX DTX unarmed with ss off");
	local_init(&loc);
	loc.codec[0].id = SDP_CODEC_G729;
	loc.codec[0].pt = 18;
	loc.codecs = 1;
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\na=fmtp:18 annexb=yes\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_G729);
	CHECK(!st.proposed.codec.annexb);

	CASE("g729 fmtp take: an explicit peer no also leaves it unarmed");
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\na=fmtp:18 annexb=no\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(!st.proposed.codec.annexb);

	CASE("g729 fmtp take: an absent peer fmtp leaves it unarmed too");
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(!st.proposed.codec.annexb);
}

/*
 * The live Annex B matrix, both negotiation directions,
 * driven end to end through sdp_offer_build(), sdp_answer_build() and
 * sdp_answer_take(). Nothing here reaches past shared/sdp.h, and no field of
 * struct sdp_state is written by hand: both capability constants are true in
 * this build, so the local half of every rule comes from the codec section's
 * own vad flag exactly as a running leg supplies it.
 *
 * The three rules under test are separate on purpose. The media plane's own
 * capability (sdp_local.g729b, hal.h HAL_FEAT_G729B) gates both of the others:
 * a board without it writes annexb=no and never arms, whatever the config and
 * the peer say. Above it, the wire annexb states what THIS side can decode and
 * is not gated by vad at all, withdrawn only to echo a peer's explicit no; and
 * the TX arm needs vad AND an explicit peer yes (a deliberate deviation from
 * the RFC 3555 absent-parameter default).
 *
 * Every row also reads the comfort noise lines back, because CN negotiation is
 * untouched by all of this: PT 13 leaves the offer and the answer under an
 * armed exchange exactly as it does under an unarmed one. What the leg then
 * sends is settled above and below this layer: media_fill clears the
 * generic detector on an unarmed G.729 leg, and the engine keeps PT 13 off the
 * wire behind an armed one.
 */

/* A G.729-led leg with comfort noise on, so every row reads the CN lines back
 * beside the annexb one. ss is the codec section's silence_suppression. */
static void g729_local_init(struct sdp_local *loc, bool ss)
{
	local_init(loc);
	loc->codec[0].id = SDP_CODEC_G729;
	loc->codec[0].pt = 18;
	loc->codec[0].vad = ss;
	loc->codecs = 1;
	loc->cn = true;
}

/*
 * The two whole bodies this leg writes, as goldens. Only the annexb value
 * moves between the rows of the matrix, so a full-body comparison is what
 * proves the rest of the description did not: a duplicated, reordered or
 * dropped comfort noise attribute passes a substring test and fails these.
 *
 * The version is 0 in both because each row starts a fresh state, and
 * sdp_offer_build() and sdp_answer_build() raise it after they write the body.
 */
#define G729_BODY(sess, ver, annexb, tail) \
	"v=0\r\n" \
	"o=- " sess " " ver " IN IP4 192.0.2.10\r\n" \
	"s=-\r\n" \
	"c=IN IP4 192.0.2.10\r\n" \
	"t=0 0\r\n" \
	"m=audio 4000 RTP/AVP 18 101 13\r\n" \
	"a=rtpmap:18 G729/8000\r\n" \
	"a=fmtp:18 annexb=" annexb "\r\n" \
	"a=rtpmap:101 telephone-event/8000\r\n" \
	"a=fmtp:101 0-15\r\n" \
	"a=rtpmap:13 CN/8000\r\n" \
	"a=ptime:20\r\n" tail

/* an offer of this leg carries no direction attribute; an answer states one */
#define G729_OFFER_WANT(annexb)		G729_BODY("42", "0", annexb, "")
#define G729_ANSWER_WANT(annexb)	G729_BODY("55", "0", annexb, "a=sendrecv\r\n")

/* the description this row just built, byte for byte */
static bool body_is(const char *want)
{
	return !strcmp(out, want);
}

/*
 * The offer both directions of a row start from. annexb is the value this
 * board's rtpmap has to carry, so one helper serves the capable board and the
 * Annex A one, and the whole body is compared either way.
 */
static void g729_offer_run(struct sdp_state *st, const struct sdp_local *loc,
			   const char *annexb)
{
	sdp_state_init(st, 42);
	buf_reset();
	CHECK(sdp_offer_build(st, loc, &b));
	CHECK(body_is(annexb[0] == 'y' ? G729_OFFER_WANT("yes") : G729_OFFER_WANT("no")));
}

/* what a peer answers or offers on payload type 18, with CN alongside */
#define PEER_G729(fmtp)	HEAD "m=audio 5004 RTP/AVP 18 101 13\r\n" \
			"a=rtpmap:101 telephone-event/8000\r\n" \
			"a=rtpmap:13 CN/8000\r\n" fmtp

static void g729_annexb_matrix_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;

	CASE("matrix ss=0: our offer states annexb=yes and the answer arms nothing");
	g729_local_init(&loc, false);
	g729_offer_run(&st, &loc, "yes");
	CHECK(!st.offered.g729_annexb);
	CHECK(parse(PEER_G729("a=fmtp:18 annexb=yes\r\n")));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_G729);
	CHECK(!st.proposed.codec.annexb);
	CHECK_UINT(st.proposed.cn_pt, 13);

	CASE("matrix ss=0: our answer states annexb=yes and arms nothing either");
	sdp_state_init(&st, 55);
	CHECK(parse(PEER_G729("a=fmtp:18 annexb=yes\r\n")));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(body_is(G729_ANSWER_WANT("yes")));
	CHECK(!st.proposed.codec.annexb);
	CHECK_UINT(st.proposed.cn_pt, 13);

	CASE("matrix ss=1 + explicit peer yes: our offer, their answer, armed");
	g729_local_init(&loc, true);
	g729_offer_run(&st, &loc, "yes");
	CHECK(st.offered.g729_annexb);
	CHECK(parse(PEER_G729("a=fmtp:18 annexb=yes\r\n")));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(st.proposed.codec.annexb);
	CHECK(st.proposed.codec.vad);
	CHECK_UINT(st.proposed.cn_pt, 13);

	CASE("matrix ss=1 + explicit peer yes: their offer, our answer, armed");
	sdp_state_init(&st, 55);
	CHECK(parse(PEER_G729("a=fmtp:18 annexb=yes\r\n")));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(body_is(G729_ANSWER_WANT("yes")));
	CHECK(st.proposed.codec.annexb);
	CHECK_UINT(st.proposed.cn_pt, 13);

	/* RFC 3555 section 4.1.9 reads an absent annexb as yes; the TX arm
	 * reads it as no */
	CASE("matrix ss=1 + absent peer fmtp: our offer, their answer, NOT armed");
	g729_offer_run(&st, &loc, "yes");
	CHECK(parse(PEER_G729("")));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(!st.proposed.codec.annexb);
	CHECK_UINT(st.proposed.cn_pt, 13);

	CASE("matrix ss=1 + absent peer fmtp: their offer, our answer, NOT armed");
	sdp_state_init(&st, 55);
	CHECK(parse(PEER_G729("")));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(body_is(G729_ANSWER_WANT("yes")));
	CHECK(!st.proposed.codec.annexb);

	CASE("matrix peer no: our answer echoes annexb=no and arms nothing");
	sdp_state_init(&st, 55);
	CHECK(parse(PEER_G729("a=fmtp:18 annexb=no\r\n")));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(body_is(G729_ANSWER_WANT("no")));
	CHECK(!st.proposed.codec.annexb);
	CHECK_UINT(st.proposed.cn_pt, 13);

	CASE("matrix peer no: an answer of no to our offer arms nothing either");
	g729_offer_run(&st, &loc, "yes");
	CHECK(parse(PEER_G729("a=fmtp:18 annexb=no\r\n")));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(!st.proposed.codec.annexb);
	CHECK_UINT(st.proposed.cn_pt, 13);

	/* The echo is keyed by payload type: a no the peer hung off another
	 * format is not this format's refusal. */
	CASE("matrix peer no: a no on another payload type does not withdraw ours");
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18 96\r\n"
		    "a=rtpmap:96 G729/8000\r\na=fmtp:96 annexb=no\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=fmtp:18 annexb=yes\r\n"));

	CASE("matrix: a payload type our offer never armed does not arm");
	g729_offer_run(&st, &loc, "yes");
	st.offered.g729_pt = 96;		/* a stale or mismatched record */
	CHECK(parse(PEER_G729("a=fmtp:18 annexb=yes\r\n")));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(!st.proposed.codec.annexb);

	CASE("matrix: G729 as the second codec of the offer still arms");
	local_init(&loc);
	loc.cn = true;
	loc.codec[0].id = SDP_CODEC_PCMA;
	loc.codec[0].pt = 8;
	loc.codec[1].id = SDP_CODEC_G729;
	loc.codec[1].pt = 18;
	loc.codec[1].vad = true;
	loc.codecs = 2;
	take_state_init(&st, &loc);
	CHECK_UINT(st.offered.g729_pt, 18);
	CHECK(st.offered.g729_annexb);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\na=fmtp:18 annexb=yes\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_G729);
	CHECK(st.proposed.codec.annexb);

	CASE("matrix: a G729-to-PCMA renegotiation clears the flag");
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 8\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMA);
	CHECK(!st.proposed.codec.annexb);

	/* the same move on the answering side: a G.711 re-offer that drops
	 * G.729 answers with no annexb line at all and no armed flag */
	CASE("matrix: a G729-to-PCMU renegotiation clears it on the answer side too");
	g729_local_init(&loc, true);
	loc.codec[1].id = SDP_CODEC_PCMU;
	loc.codec[1].pt = 0;
	loc.codecs = 2;
	sdp_state_init(&st, 55);
	CHECK(parse(PEER_G729("a=fmtp:18 annexb=yes\r\n")));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(st.proposed.codec.annexb);
	sdp_commit(&st);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 13\r\na=rtpmap:13 CN/8000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);
	CHECK(!st.proposed.codec.annexb);
	CHECK(!has("annexb"));
	CHECK(has("a=rtpmap:13 CN/8000\r\n"));
	CHECK_UINT(st.proposed.cn_pt, 13);

	/*
	 * The Annex A board (hal.h: HAL_FEAT_G729 without HAL_FEAT_G729B, which
	 * is what a backend whose G.729 is Annex A alone reports). It is
	 * the row the two rules above must not reach: the claim is untrue on
	 * such a board and an armed transmitter would put SID frames at a
	 * decoder that answers them with an error. The config asks for silence
	 * suppression in every case below, and the peer confirms Annex B in
	 * every case below, so the capability is the only thing refusing.
	 */
	CASE("matrix no capability: our offer states annexb=no and records no arm");
	g729_local_init(&loc, true);
	loc.g729b = false;
	g729_offer_run(&st, &loc, "no");
	CHECK(st.offered.g729_pt == 18);
	CHECK(!st.offered.g729_annexb);

	CASE("matrix no capability: an explicit peer yes still arms nothing");
	CHECK(parse(PEER_G729("a=fmtp:18 annexb=yes\r\n")));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_G729);
	CHECK(!st.proposed.codec.annexb);
	CHECK(st.proposed.codec.vad);
	CHECK_UINT(st.proposed.cn_pt, 13);

	CASE("matrix no capability: our answer states annexb=no and arms nothing");
	sdp_state_init(&st, 55);
	CHECK(parse(PEER_G729("a=fmtp:18 annexb=yes\r\n")));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(body_is(G729_ANSWER_WANT("no")));
	CHECK(!st.proposed.codec.annexb);
	CHECK_UINT(st.proposed.cn_pt, 13);

	/* the peer's own no changes nothing here: the line already reads no */
	CASE("matrix no capability: a peer that offers no is answered the same way");
	sdp_state_init(&st, 55);
	CHECK(parse(PEER_G729("a=fmtp:18 annexb=no\r\n")));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(body_is(G729_ANSWER_WANT("no")));
	CHECK(!st.proposed.codec.annexb);
}

/*
 * One m= line that names G.729 twice, at two payload types, with a different
 * annexb on each. It is legal SDP and it is the shape a per-section record
 * cannot hold: the record keeps whichever line came last, and the exchange
 * then reads a value the peer wrote about a format it did not select.
 *
 * Both halves of the layer are checked because they fail differently. The arm
 * is fail-safe under a per-section record, since it needs the record's own
 * payload type AND an explicit yes and every miss disarms. The ECHO is not: a
 * stale (96, yes) over a selection of 18 restates yes across the peer's
 * explicit no, which is the misadvertisement class this parameter exists to
 * prevent.
 *
 * Both selection orders run, because the answer follows the offer order (RFC
 * 3264 6.1) and the peer decides which of its two types this side selects.
 */
#define DUAL_G729(first, second, f18, f96) \
	HEAD "m=audio 5004 RTP/AVP " #first " " #second " 101 13\r\n" \
	"a=rtpmap:96 G729/8000\r\n" \
	"a=rtpmap:101 telephone-event/8000\r\n" \
	"a=rtpmap:13 CN/8000\r\n" \
	"a=fmtp:18 annexb=" f18 "\r\n" \
	"a=fmtp:96 annexb=" f96 "\r\n"

static void g729_dual_pt_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;

	CASE("dual pt: the parse keeps one record per format, not one per section");
	CHECK(parse(DUAL_G729(18, 96, "no", "yes")));
	CHECK(annexb_rec(18) == 0);
	CHECK(annexb_rec(96) == 1);

	CASE("dual pt: and the same two records whichever order the m= line used");
	CHECK(parse(DUAL_G729(96, 18, "no", "yes")));
	CHECK(annexb_rec(18) == 0);
	CHECK(annexb_rec(96) == 1);

	/* a yes here would restate over the peer's own no on 18 */
	CASE("dual pt: our answer selecting 18 echoes 18's no, not 96's yes");
	g729_local_init(&loc, true);
	sdp_state_init(&st, 55);
	CHECK(parse(DUAL_G729(18, 96, "no", "yes")));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 18 101 13\r\n"));
	CHECK(has("a=rtpmap:18 G729/8000\r\na=fmtp:18 annexb=no\r\n"));
	CHECK(!has("annexb=yes"));
	CHECK(!st.proposed.codec.annexb);
	CHECK(has("a=rtpmap:13 CN/8000\r\n"));
	CHECK_UINT(st.proposed.cn_pt, 13);

	CASE("dual pt: our answer selecting 96 takes 96's yes and arms");
	sdp_state_init(&st, 55);
	CHECK(parse(DUAL_G729(96, 18, "no", "yes")));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 96 101 13\r\n"));
	CHECK(has("a=rtpmap:96 G729/8000\r\na=fmtp:96 annexb=yes\r\n"));
	CHECK(!has("annexb=no"));
	CHECK(st.proposed.codec.annexb);
	CHECK(has("a=rtpmap:13 CN/8000\r\n"));
	CHECK_UINT(st.proposed.cn_pt, 13);

	CASE("dual pt: the reverse pair arms on 18 and echoes its yes");
	sdp_state_init(&st, 55);
	CHECK(parse(DUAL_G729(18, 96, "yes", "no")));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=rtpmap:18 G729/8000\r\na=fmtp:18 annexb=yes\r\n"));
	CHECK(!has("annexb=no"));
	CHECK(st.proposed.codec.annexb);

	CASE("dual pt: and it does not arm on 96, whose own record says no");
	sdp_state_init(&st, 55);
	CHECK(parse(DUAL_G729(96, 18, "yes", "no")));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=rtpmap:96 G729/8000\r\na=fmtp:96 annexb=no\r\n"));
	CHECK(!st.proposed.codec.annexb);

	/*
	 * The take direction. Our offer numbered G.729 18, so an answer that
	 * selects 18 reads 18's own record; one that selects 96 disarms on the
	 * offered-pt key alone, which is the S1 design and not this fix.
	 */
	CASE("dual pt: an answer selecting 18 reads 18's no and does not arm");
	g729_offer_run(&st, &loc, "yes");
	CHECK(parse(DUAL_G729(18, 96, "no", "yes")));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.pt, 18);
	CHECK(!st.proposed.codec.annexb);
	CHECK_UINT(st.proposed.cn_pt, 13);

	CASE("dual pt: an answer selecting 18 reads 18's yes and arms");
	g729_offer_run(&st, &loc, "yes");
	CHECK(parse(DUAL_G729(18, 96, "yes", "no")));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.pt, 18);
	CHECK(st.proposed.codec.annexb);
	CHECK_UINT(st.proposed.cn_pt, 13);

	CASE("dual pt: an answer selecting 96 disarms on the offered payload type");
	g729_offer_run(&st, &loc, "yes");
	CHECK(parse(DUAL_G729(96, 18, "no", "yes")));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.pt, 96);
	CHECK(!st.proposed.codec.annexb);
}

static void take_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;

	CASE("the answer to our offer fills proposed");
	local_init(&loc);
	take_state_init(&st, &loc);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 101\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\na=ptime:20\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(st.proposed.valid);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);
	CHECK_UINT(st.proposed.codec.pt, 0);
	CHECK_UINT(st.proposed.dtmf_pt, 101);
	CHECK_UINT(st.proposed.ptime_ms, 20);
	CHECK_UINT(st.proposed.dir, SDP_SENDRECV);
	CHECK(!strcmp(st.proposed.remote_addr, "198.51.100.7"));
	CHECK_UINT(st.proposed.remote_port, 5004);
	CHECK(!st.proposed.remote_hold);

	CASE("the answer take clears offer_pending");
	CHECK(!st.offer_pending);

	CASE("the peer picked our second codec");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 8\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMA);
	CHECK_UINT(st.proposed.codec.pt, 8);

	CASE("the answer take stores the payload type the peer listed");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 PCMU/8000\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);
	CHECK_UINT(st.proposed.codec.pt, 96);

	CASE("no telephone-event in the answer leaves dtmf_pt at -1");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(st.proposed.dtmf_pt == -1);
	CHECK(st.proposed.cn_pt == -1);

	CASE("our disabled telephone-event ignores the answered one");
	local_init(&loc);
	loc.dtmf_pt = 0;
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 101\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(st.proposed.dtmf_pt == -1);

	CASE("the answered CN payload type is taken when both sides carry it");
	local_init(&loc);
	loc.cn = true;
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 13\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.cn_pt, 13);

	CASE("a peer sendonly answer means our recvonly and a remote hold");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=sendonly\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.dir, SDP_RECVONLY);
	CHECK(st.proposed.remote_hold);

	CASE("a peer recvonly answer means our sendonly");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=recvonly\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.dir, SDP_SENDONLY);
	CHECK(!st.proposed.remote_hold);

	CASE("a peer inactive answer means inactive");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=inactive\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.dir, SDP_INACTIVE);
	CHECK(st.proposed.remote_hold);

	CASE("a sendrecv answer with local hold stays sendonly");
	local_init(&loc);
	take_state_init(&st, &loc);
	sdp_hold_set(&st, true);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.dir, SDP_SENDONLY);

	CASE("an RFC 2543 c=0.0.0.0 answer is a remote hold");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\n"
		    "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(st.proposed.remote_hold);
	CHECK_UINT(st.proposed.dir, SDP_RECVONLY);

	CASE("an answered ptime of 30 is taken and 40 is refused");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=ptime:30\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.ptime_ms, 30);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=ptime:40\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.ptime_ms, 20);

	CASE("an answer with audio port 0 is SDP_NO_CODEC");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 0\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);
	CHECK(st.offer_pending);

	CASE("an answer with an empty fmt list is SDP_NO_CODEC");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);

	CASE("an answer with no audio stream is SDP_NO_CODEC");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);

	CASE("an answer of formats our offer never wrote is SDP_NO_CODEC");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);

	CASE("the answer order decides, not our own: 8 before 0 selects PCMA");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 8 0\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMA);
	CHECK_UINT(st.proposed.codec.pt, 8);

	CASE("an answered format our offer never wrote is skipped, not seated");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18 0 8\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);
	CHECK_UINT(st.proposed.codec.pt, 0);

	CASE("an answer with no offer of ours pending is SDP_BAD");
	local_init(&loc);
	sdp_state_init(&st, 77);
	CHECK(!st.offer_pending);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_BAD);
	CHECK(!st.proposed.valid);
	CHECK_UINT(st.proposed.remote_port, 0);

	CASE("a repeated answer is refused once the offer was taken");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(!st.offer_pending);
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_BAD);

	CASE("an SRTP answer is SDP_NO_CODEC");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/SAVP 0\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);
	CHECK(st.offer_pending);

	CASE("an answer without a connection address is SDP_BAD");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 198.51.100.7\r\nt=0 0\r\n"
		    "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_BAD);
	CHECK(!st.proposed.valid);

	CASE("an answer whose c= names a host is SDP_NO_CODEC");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 sip.example.com\r\nc=IN IP4 sip.example.com\r\n"
		    "t=0 0\r\nm=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);
	CHECK(st.offer_pending);

	CASE("the same answer with a literal c= is taken");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(!strcmp(st.proposed.remote_addr, "198.51.100.7"));

	CASE("a host media c= is refused over a literal session c= in an answer");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\nc=IN IP4 sip.example.com\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);

	CASE("an IP6 literal answer is taken as it was before the refusal");
	local_init(&loc);
	snprintf(loc.addr, sizeof(loc.addr), "%s", LOCAL6);
	take_state_init(&st, &loc);
	CHECK(parse("v=0\r\no=- 1 1 IN IP6 2001:db8::1\r\nc=IN IP6 2001:db8::1\r\n"
		    "t=0 0\r\nm=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(!strcmp(st.proposed.remote_addr, "2001:db8::1"));
	CHECK_UINT(st.proposed.remote_ipver, 6);

	CASE("a peer recvonly answer with local hold stays sendonly");
	local_init(&loc);
	take_state_init(&st, &loc);
	sdp_hold_set(&st, true);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=recvonly\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.dir, SDP_SENDONLY);

	CASE("a G729A answer selects G729");
	local_init(&loc);
	loc.codec[0].id = SDP_CODEC_G729;
	loc.codec[0].pt = 18;
	loc.codecs = 1;
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 18\r\na=rtpmap:18 G729A/8000\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_G729);
	CHECK_UINT(st.proposed.codec.pt, 18);

	CASE("a NULL argument takes SDP_BAD");
	CHECK(sdp_answer_take(NULL, &loc, &m) == SDP_BAD);
	CHECK(sdp_answer_take(&st, NULL, &m) == SDP_BAD);
	CHECK(sdp_answer_take(&st, &loc, NULL) == SDP_BAD);
}

static void state_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;

	CASE("state init clears both results");
	sdp_state_init(&st, 99);
	CHECK_UINT(st.sess_id, 99);
	CHECK_UINT(st.sess_ver, 0);
	CHECK(!sdp_local_hold(&st));
	CHECK(!st.offer_pending);
	CHECK(!st.offer_in_200);
	CHECK(!st.committed.valid);
	CHECK(!st.proposed.valid);
	CHECK(st.committed.dtmf_pt == -1);
	CHECK(st.proposed.cn_pt == -1);

	CASE("commit moves proposed over committed");
	local_init(&loc);
	sdp_state_init(&st, 99);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	sdp_commit(&st);
	CHECK(st.committed.valid);
	CHECK_UINT(st.committed.codec.id, SDP_CODEC_PCMU);
	CHECK_UINT(st.committed.remote_port, 5004);

	CASE("commit clears the offer flags");
	st.offer_pending = true;
	st.offer_in_200 = true;
	sdp_commit(&st);
	CHECK(!st.offer_pending);
	CHECK(!st.offer_in_200);

	CASE("a rejected re-offer rolls back to the committed media");
	local_init(&loc);
	CHECK(parse(HEAD "m=audio 7000 RTP/AVP 8\r\na=sendonly\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMA);
	CHECK_UINT(st.proposed.remote_port, 7000);
	CHECK_UINT(st.committed.codec.id, SDP_CODEC_PCMU);
	sdp_rollback(&st);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);
	CHECK_UINT(st.proposed.remote_port, 5004);
	CHECK_UINT(st.proposed.dir, SDP_SENDRECV);
	CHECK(st.proposed.valid);

	CASE("rollback clears the offer flags");
	st.offer_pending = true;
	st.offer_in_200 = true;
	sdp_rollback(&st);
	CHECK(!st.offer_pending);
	CHECK(!st.offer_in_200);

	CASE("rollback of a failed first offer leaves nothing valid");
	local_init(&loc);
	sdp_state_init(&st, 99);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(st.offer_pending);
	sdp_rollback(&st);
	CHECK(!st.proposed.valid);
	CHECK(!st.offer_pending);

	CASE("a NULL state passed to every state call does not crash");
	sdp_state_init(NULL, 1);
	sdp_hold_set(NULL, true);
	sdp_commit(NULL);
	sdp_rollback(NULL);

	CASE("the codec and direction names");
	CHECK(!strcmp(sdp_codec_name(SDP_CODEC_PCMU), "PCMU"));
	CHECK(!strcmp(sdp_codec_name(SDP_CODEC_PCMA), "PCMA"));
	CHECK(!strcmp(sdp_codec_name(SDP_CODEC_G729), "G729"));
	CHECK(!strcmp(sdp_codec_name(SDP_CODEC_NONE), "none"));
	CHECK(!strcmp(sdp_dir_name(SDP_SENDRECV), "sendrecv"));
	CHECK(!strcmp(sdp_dir_name(SDP_SENDONLY), "sendonly"));
	CHECK(!strcmp(sdp_dir_name(SDP_RECVONLY), "recvonly"));
	CHECK(!strcmp(sdp_dir_name(SDP_INACTIVE), "inactive"));
}

/*
 * The gated T.38 negotiation. Three fixtures carry the state the gates read,
 * because every one of them is a refusal on its own: loc.t38 is the per-leg
 * enable, committed.valid says the leg is mid-call (an initial INVITE has no
 * committed exchange and therefore keeps the refusal path), and a pending T.38
 * offer of ours is what makes an answer a T.38 answer.
 */
static void t38_local_init(struct sdp_local *loc)
{
	local_init(loc);
	loc->t38 = true;
}

static void t38_answer_state_init(struct sdp_state *st)
{
	sdp_state_init(st, 55);
	st->committed.valid = true;
}

static void t38_take_state_init(struct sdp_state *st, struct sdp_local *loc)
{
	t38_local_init(loc);
	sdp_state_init(st, 77);
	sdp_t38_set(st, true);
	buf_reset();
	sdp_offer_build(st, loc, &b);
}

/* the inbound switch shape: the audio stream withdrawn, the image stream offered */
static bool t38_offer_parse(const char *image_attrs)
{
	char body[1024];

	snprintf(body, sizeof(body), "%s%s", HEAD "m=audio 0 RTP/AVP 0\r\n"
		 "m=image 5006 udptl t38\r\n", image_attrs);

	return parse(body);
}

static void t38_parse_tests(void)
{
	CASE("t38 parse: udptl sets proto_ok on the image section");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\nm=image 5006 udptl t38\r\n"));
	CHECK(m.image.present);
	CHECK(m.image.proto_ok);
	CHECK(m.image.t38_fmt);
	CHECK_UINT(m.image.port, 5006);

	CASE("t38 parse: the image proto is matched case insensitively");
	CHECK(parse(HEAD "m=image 5006 UDPTL T38\r\n"));
	CHECK(m.image.proto_ok);
	CHECK(m.image.t38_fmt);

	CASE("t38 parse: an image section over RTP/AVP is not udptl");
	CHECK(parse(HEAD "m=image 5006 RTP/AVP t38\r\n"));
	CHECK(m.image.present);
	CHECK(!m.image.proto_ok);
	CHECK(m.image.t38_fmt);

	CASE("t38 parse: an image section over tcp is not udptl");
	CHECK(parse(HEAD "m=image 5006 tcp t38\r\n"));
	CHECK(!m.image.proto_ok);

	CASE("t38 parse: udptl on the audio section stays unusable");
	CHECK(parse(HEAD "m=audio 5004 udptl t38\r\n"));
	CHECK(m.audio.present);
	CHECK(!m.audio.proto_ok);
	CHECK(!m.audio.t38_fmt);

	CASE("t38 parse: an image section without the t38 token has no fmt");
	CHECK(parse(HEAD "m=image 5006 udptl\r\n"));
	CHECK(m.image.proto_ok);
	CHECK(!m.image.t38_fmt);
	CHECK_UINT(m.image.fmts, 0);

	CASE("t38 parse: the t38 token never enters the fmt list");
	CHECK(parse(HEAD "m=image 5006 udptl t38 0\r\n"));
	CHECK(m.image.t38_fmt);
	CHECK_UINT(m.image.fmts, 1);
	CHECK_UINT(m.image.fmt[0].pt, 0);

	CASE("t38 parse: the audio m= line order is recorded");
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 0\r\nm=image 5006 udptl t38\r\n"));
	CHECK(!m.image_first);
	CHECK(m.audio.present);
	CHECK(m.image.present);

	CASE("t38 parse: the image m= line order is recorded");
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\nm=audio 0 RTP/AVP 0\r\n"));
	CHECK(m.image_first);
	CHECK(m.audio.present);
	CHECK(m.image.present);

	CASE("t38 parse: an image only body records the image first");
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\n"));
	CHECK(m.image_first);
	CHECK(!m.audio.present);

	CASE("t38 parse: an audio only body records no image order");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(!m.image_first);

	CASE("t38 parse: a section we ignore does not move the order");
	CHECK(parse(HEAD "m=video 9000 RTP/AVP 34\r\nm=audio 0 RTP/AVP 0\r\n"
		    "m=image 5006 udptl t38\r\n"));
	CHECK(!m.image_first);

	CASE("t38 parse: the session c= reaches the image section");
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\n"));
	CHECK(!strcmp(m.image.addr, "198.51.100.7"));
	CHECK_UINT(m.image.ipver, 4);

	CASE("t38 parse: a media c= on the image section wins");
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\nc=IN IP4 203.0.113.9\r\n"));
	CHECK(!strcmp(m.image.addr, "203.0.113.9"));

	CASE("t38 parse: every T38 attribute is read");
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\na=T38FaxVersion:3\r\n"
		    "a=T38MaxBitRate:9600\r\na=T38FaxRateManagement:localTCF\r\n"
		    "a=T38FaxMaxBuffer:400\r\na=T38FaxMaxDatagram:160\r\n"
		    "a=T38FaxUdpEC:t38UDPRedundancy\r\n"));
	CHECK(m.image.t38.version_present);
	CHECK_UINT(m.image.t38.version, 3);
	CHECK(m.image.t38.max_bit_rate_present);
	CHECK_UINT(m.image.t38.max_bit_rate, 9600);
	CHECK(m.image.t38.max_buffer_present);
	CHECK_UINT(m.image.t38.max_buffer, 400);
	CHECK(m.image.t38.max_datagram_present);
	CHECK_UINT(m.image.t38.max_datagram, 160);
	CHECK_UINT(m.image.t38.rate_mgmt, SDP_T38_RATE_LOCAL_TCF);
	CHECK_UINT(m.image.t38.ec, SDP_T38_EC_REDUNDANCY);

	CASE("t38 parse: the attribute names are matched case insensitively");
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\na=t38maxbitrate:2400\r\n"
		    "a=T38FAXRATEMANAGEMENT:TransferredTCF\r\n"
		    "a=t38faxudpec:T38UDPFEC\r\n"));
	CHECK_UINT(m.image.t38.max_bit_rate, 2400);
	CHECK_UINT(m.image.t38.rate_mgmt, SDP_T38_RATE_TRANSFERRED_TCF);
	CHECK_UINT(m.image.t38.ec, SDP_T38_EC_FEC);

	CASE("t38 parse: a space after the attribute colon is tolerated");
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\na=T38MaxBitRate: 9600\r\n"
		    "a=T38FaxUdpEC:  t38UDPFEC\r\n"));
	CHECK(m.image.t38.max_bit_rate_present);
	CHECK_UINT(m.image.t38.max_bit_rate, 9600);
	CHECK_UINT(m.image.t38.ec, SDP_T38_EC_FEC);

	CASE("t38 parse: a zero valued attribute is present, not absent");
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\na=T38FaxVersion:0\r\n"
		    "a=T38MaxBitRate:0\r\na=T38FaxMaxBuffer:0\r\n"
		    "a=T38FaxMaxDatagram:0\r\n"));
	CHECK(m.image.t38.version_present);
	CHECK(m.image.t38.max_bit_rate_present);
	CHECK(m.image.t38.max_buffer_present);
	CHECK(m.image.t38.max_datagram_present);
	CHECK_UINT(m.image.t38.max_bit_rate, 0);

	CASE("t38 parse: no T38 attribute leaves every field absent");
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\n"));
	CHECK(!m.image.t38.version_present);
	CHECK(!m.image.t38.max_bit_rate_present);
	CHECK(!m.image.t38.max_buffer_present);
	CHECK(!m.image.t38.max_datagram_present);
	CHECK_UINT(m.image.t38.rate_mgmt, SDP_T38_RATE_NONE);
	CHECK_UINT(m.image.t38.ec, SDP_T38_EC_NONE);

	CASE("t38 parse: a junk number leaves the field absent");
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\na=T38FaxVersion:abc\r\n"
		    "a=T38MaxBitRate:-1\r\na=T38FaxMaxBuffer:\r\n"
		    "a=T38FaxMaxDatagram:9999999999999\r\n"));
	CHECK(!m.image.t38.version_present);
	CHECK(!m.image.t38.max_bit_rate_present);
	CHECK(!m.image.t38.max_buffer_present);
	CHECK(!m.image.t38.max_datagram_present);

	CASE("t38 parse: a junk enum value leaves the field absent");
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\n"
		    "a=T38FaxRateManagement:someTCF\r\n"
		    "a=T38FaxUdpEC:t38UDPWhatever\r\n"));
	CHECK_UINT(m.image.t38.rate_mgmt, SDP_T38_RATE_NONE);
	CHECK_UINT(m.image.t38.ec, SDP_T38_EC_NONE);

	CASE("t38 parse: a junk T38 attribute never fails the body");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\nm=image 5006 udptl t38\r\n"
		    "a=T38MaxBitRate:x\r\na=T38FaxUdpEC\r\n"));
	CHECK(m.audio.present);
	CHECK(m.image.present);
	CHECK_UINT(m.audio.port, 5004);

	CASE("t38 parse: a T38 attribute before any m= line is stored nowhere");
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 198.51.100.7\r\nc=IN IP4 198.51.100.7\r\n"
		    "a=T38MaxBitRate:9600\r\na=T38FaxUdpEC:t38UDPRedundancy\r\n"
		    "a=T38FaxMaxDatagram:160\r\nt=0 0\r\n"
		    "m=image 5006 udptl t38\r\n"));
	CHECK(m.image.present);
	CHECK(!m.image.t38.max_bit_rate_present);
	CHECK(!m.image.t38.max_datagram_present);
	CHECK_UINT(m.image.t38.ec, SDP_T38_EC_NONE);

	CASE("t38 parse: the m= port records whether it could be read");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(m.audio.port_ok);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 0\r\n"));
	CHECK(m.audio.port_ok);
	CHECK_UINT(m.audio.port, 0);
	CHECK(parse(HEAD "m=audio 0/2 RTP/AVP 0\r\n"));
	CHECK(m.audio.port_ok);
	CHECK_UINT(m.audio.port, 0);

	CASE("t38 parse: an unreadable m= port is not a readable zero");
	CHECK(parse(HEAD "m=audio nope RTP/AVP 0\r\n"));
	CHECK(!m.audio.port_ok);
	CHECK_UINT(m.audio.port, 0);
	CHECK(parse(HEAD "m=audio 70000 RTP/AVP 0\r\n"));
	CHECK(!m.audio.port_ok);
	CHECK(parse(HEAD "m=audio\r\n"));
	CHECK(m.audio.present);
	CHECK(!m.audio.port_ok);

	CASE("t38 parse: a duplicate audio or image section is recorded");
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 0\r\nm=audio 5004 RTP/AVP 0\r\n"));
	CHECK(m.audio_dup);
	CHECK(!m.image_dup);
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\nm=image 5008 udptl t38\r\n"));
	CHECK(m.image_dup);
	CHECK(!m.audio_dup);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 0\r\nm=image 5006 udptl t38\r\n"));
	CHECK(!m.audio_dup);
	CHECK(!m.image_dup);

	CASE("t38 parse: a section we ignore is not a duplicate");
	CHECK(parse(HEAD "m=video 9000 RTP/AVP 34\r\nm=audio 0 RTP/AVP 0\r\n"
		    "m=image 5006 udptl t38\r\n"));
	CHECK(!m.audio_dup);
	CHECK(!m.image_dup);

	/*
	 * The raw token survives the CN and telephone-event moves, which is what
	 * lets a withdrawn stream be refused with a format the peer really named.
	 */
	CASE("t38 parse: the first raw fmt token outlives an emptied fmt list");
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 101\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\n"));
	CHECK_UINT(m.audio.fmts, 0);
	CHECK_UINT(m.audio.fmt_first, 101);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 13\r\n"));
	CHECK_UINT(m.audio.fmts, 0);
	CHECK_UINT(m.audio.fmt_first, 13);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 8 0\r\n"));
	CHECK_UINT(m.audio.fmt_first, 8);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP\r\n"));
	CHECK(m.audio.fmt_first == -1);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 200 8\r\n"));
	CHECK_UINT(m.audio.fmt_first, 8);

	CASE("t38 parse: a T38 attribute after the image section stays there");
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\na=T38MaxBitRate:9600\r\n"
		    "m=audio 5004 RTP/AVP 0\r\na=T38MaxBitRate:2400\r\n"));
	CHECK_UINT(m.image.t38.max_bit_rate, 9600);
	CHECK_UINT(m.audio.t38.max_bit_rate, 2400);

	CASE("t38 parse: the fixed attributes we never read stay unparsed");
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\na=T38FaxFillBitRemoval:1\r\n"
		    "a=T38FaxTranscodingMMR:1\r\na=T38FaxTranscodingJBIG:1\r\n"));
	CHECK(m.image.present);
	CHECK(!m.image.t38.version_present);
}

static void t38_datagram_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;
	static const char want_offer[] =
		"v=0\r\n"
		"o=- 42 0 IN IP4 192.0.2.10\r\n"
		"s=-\r\n"
		"c=IN IP4 192.0.2.10\r\n"
		"t=0 0\r\n"
		"m=image 4000 udptl t38\r\n"
		T38_FIXED
		"a=T38MaxBitRate:14400\r\n"
		"a=T38FaxUdpEC:t38UDPRedundancy\r\n"
		"a=T38FaxMaxBuffer:200\r\n"
		"a=T38FaxMaxDatagram:72\r\n";
	static const char want_answer[] =
		"v=0\r\n"
		"o=- 55 0 IN IP4 192.0.2.10\r\n"
		"s=-\r\n"
		"c=IN IP4 192.0.2.10\r\n"
		"t=0 0\r\n"
		"m=audio 0 RTP/AVP 0\r\n"
		"m=image 4000 udptl t38\r\n"
		T38_FIXED
		"a=T38MaxBitRate:14400\r\n"
		"a=T38FaxMaxBuffer:200\r\n"
		"a=T38FaxMaxDatagram:72\r\n";

	CASE("t38 datagram: the default offer and answer are byte exact at 72");
	t38_local_init(&loc);
	sdp_state_init(&st, 42);
	sdp_t38_set(&st, true);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!strcmp(out, want_offer));
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse(""));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!strcmp(out, want_answer));

	CASE("t38 datagram: set zero restores 72 after 512");
	sdp_t38_max_datagram_set(512);
	sdp_state_init(&st, 42);
	sdp_t38_set(&st, true);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=T38FaxMaxDatagram:512\r\n"));
	sdp_t38_max_datagram_set(0);
	sdp_state_init(&st, 42);
	sdp_t38_set(&st, true);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=T38FaxMaxDatagram:72\r\n"));
}

static void t38_offer_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;
	static const char want[] =
		"v=0\r\n"
		"o=- 42 0 IN IP4 192.0.2.10\r\n"
		"s=-\r\n"
		"c=IN IP4 192.0.2.10\r\n"
		"t=0 0\r\n"
		"m=image 4000 udptl t38\r\n"
		"a=T38FaxVersion:0\r\n"
		"a=T38FaxRateManagement:transferredTCF\r\n"
		"a=T38FaxFillBitRemoval:0\r\n"
		"a=T38FaxTranscodingMMR:0\r\n"
		"a=T38FaxTranscodingJBIG:0\r\n"
		"a=T38MaxBitRate:14400\r\n"
		"a=T38FaxUdpEC:t38UDPRedundancy\r\n"
		"a=T38FaxMaxBuffer:200\r\n"
		"a=T38FaxMaxDatagram:512\r\n";

	sdp_t38_max_datagram_set(512);

	CASE("t38 offer: the image offer is byte exact");
	t38_local_init(&loc);
	sdp_state_init(&st, 42);
	sdp_t38_set(&st, true);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!strcmp(out, want));

	CASE("t38 offer: no rtpmap, no ptime and no direction ride the image line");
	CHECK(!has("a=rtpmap"));
	CHECK(!has("a=ptime"));
	CHECK(!has("a=sendrecv"));
	CHECK(!has("a=sendonly"));
	CHECK(!has("a=recvonly"));
	CHECK(!has("a=inactive"));
	CHECK(!has("m=audio"));

	CASE("t38 offer: the hold state does not reach the image section");
	t38_local_init(&loc);
	sdp_state_init(&st, 42);
	sdp_t38_set(&st, true);
	sdp_hold_set(&st, true);
	st.committed.remote_hold = true;
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!strcmp(out, want));
	CHECK_UINT(st.proposed.dir, SDP_SENDRECV);

	CASE("t38 offer: the local codec list does not reach the image section");
	t38_local_init(&loc);
	loc.cn = true;
	loc.dtmf_pt = 101;
	sdp_state_init(&st, 42);
	sdp_t38_set(&st, true);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!strcmp(out, want));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_NONE);
	CHECK(st.proposed.dtmf_pt == -1);
	CHECK(st.proposed.cn_pt == -1);

	CASE("t38 offer: a leg with no usable codec still offers T.38");
	t38_local_init(&loc);
	loc.codecs = 0;
	sdp_state_init(&st, 42);
	sdp_t38_set(&st, true);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!strcmp(out, want));

	CASE("t38 offer: the offer fills proposed and sets offer_pending");
	CHECK(st.offer_pending);
	CHECK(st.proposed.t38);
	CHECK_UINT(st.proposed.dir, SDP_SENDRECV);
	CHECK_UINT(st.proposed.ptime_ms, 0);
	CHECK(!st.proposed.valid);
	CHECK_UINT(st.proposed.remote_port, 0);
	CHECK_UINT(st.proposed.t38_max_bit_rate, 0);

	CASE("t38 offer: the session version increments");
	t38_local_init(&loc);
	sdp_state_init(&st, 42);
	sdp_t38_set(&st, true);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("o=- 42 0 IN IP4 192.0.2.10\r\n"));
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("o=- 42 1 IN IP4 192.0.2.10\r\n"));
	CHECK_UINT(st.sess_ver, 2);

	CASE("t38 offer: an image offer that does not fit leaves the state alone");
	t38_local_init(&loc);
	sdp_state_init(&st, 42);
	sdp_t38_set(&st, true);
	sip_buf_init(&b, out, 64);
	CHECK(!sdp_offer_build(&st, &loc, &b));
	CHECK(!sip_buf_ok(&b));
	CHECK_UINT(st.sess_ver, 0);
	CHECK(!st.offer_pending);
	CHECK(!st.proposed.t38);

	CASE("t38 offer: without the enable the audio offer is unchanged");
	t38_local_init(&loc);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 0 8 101\r\n"));
	CHECK(has("a=ptime:20\r\n"));
	CHECK(!has("m=image"));
	CHECK(!has("T38"));
	CHECK(!st.proposed.t38);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);

	CASE("t38 offer: sdp_t38_set is visible in the next offer only");
	t38_local_init(&loc);
	sdp_state_init(&st, 42);
	sdp_t38_set(&st, true);
	CHECK(st.local_t38);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=image 4000 udptl t38\r\n"));
	sdp_t38_set(&st, false);
	CHECK(!st.local_t38);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 0 8 101\r\n"));
	CHECK(!has("m=image"));

	CASE("t38 offer: a NULL state passed to sdp_t38_set does not crash");
	sdp_t38_set(NULL, true);

	sdp_t38_max_datagram_set(0);
}

static void t38_answer_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;
	static const char want[] =
		"v=0\r\n"
		"o=- 55 0 IN IP4 192.0.2.10\r\n"
		"s=-\r\n"
		"c=IN IP4 192.0.2.10\r\n"
		"t=0 0\r\n"
		"m=audio 0 RTP/AVP 0\r\n"
		"m=image 4000 udptl t38\r\n"
		"a=T38FaxVersion:0\r\n"
		"a=T38FaxRateManagement:transferredTCF\r\n"
		"a=T38FaxFillBitRemoval:0\r\n"
		"a=T38FaxTranscodingMMR:0\r\n"
		"a=T38FaxTranscodingJBIG:0\r\n"
		"a=T38MaxBitRate:14400\r\n"
		"a=T38FaxMaxBuffer:200\r\n"
		"a=T38FaxMaxDatagram:512\r\n";

	sdp_t38_max_datagram_set(512);

	CASE("t38 answer: the switch upgrade answer is byte exact");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse(""));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!strcmp(out, want));

	CASE("t38 answer: the image section carries no ptime and no direction");
	CHECK(!has("a=ptime"));
	CHECK(!has("a=sendrecv"));
	CHECK(!has("a=recvonly"));
	CHECK(!has("a=rtpmap"));

	CASE("t38 answer: proposed names the image stream");
	CHECK(st.proposed.t38);
	CHECK(st.proposed.valid);
	CHECK(!st.proposed.image_refused);
	CHECK(!strcmp(st.proposed.remote_addr, "198.51.100.7"));
	CHECK_UINT(st.proposed.remote_port, 5006);
	CHECK_UINT(st.proposed.remote_ipver, 4);
	CHECK_UINT(st.proposed.dir, SDP_SENDRECV);
	CHECK_UINT(st.proposed.ptime_ms, 0);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_NONE);
	CHECK_UINT(st.proposed.t38_max_bit_rate, 14400);
	CHECK_UINT(st.proposed.t38_max_datagram, 0);
	CHECK_UINT(st.proposed.t38_ec, SDP_T38_EC_NONE);
	CHECK(!st.offer_pending);
	CHECK_UINT(st.sess_ver, 1);

	/* the hold table belongs to audio: an image exchange has no direction */
	CASE("t38 answer: a held audio section is no remote hold on the image one");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 0\r\na=sendonly\r\n"
		    "m=image 5006 udptl t38\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!has("a=sendonly"));
	CHECK(!has("a=recvonly"));
	CHECK(!st.proposed.remote_hold);
	CHECK_UINT(st.proposed.dir, SDP_SENDRECV);

	CASE("t38 answer: an image first offer is answered image first");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\nm=audio 0 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!strcmp(out,
		      "v=0\r\n"
		      "o=- 55 0 IN IP4 192.0.2.10\r\n"
		      "s=-\r\n"
		      "c=IN IP4 192.0.2.10\r\n"
		      "t=0 0\r\n"
		      "m=image 4000 udptl t38\r\n"
		      T38_FIXED
		      "a=T38MaxBitRate:14400\r\n"
		      "a=T38FaxMaxBuffer:200\r\n"
		      "a=T38FaxMaxDatagram:512\r\n"
		      "m=audio 0 RTP/AVP 0\r\n"));

	CASE("t38 answer: an image only offer is answered with one m= line");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!has("m=audio"));
	CHECK(has("m=image 4000 udptl t38\r\n"));
	CHECK(st.proposed.t38);

	CASE("t38 answer: the refused audio line keeps the first offered fmt");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 8 0\r\nm=image 5006 udptl t38\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 0 RTP/AVP 8\r\n"));

	CASE("t38 answer: an empty offered fmt list refuses audio with type 0");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP\r\nm=image 5006 udptl t38\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 0 RTP/AVP 0\r\n"));

	/*
	 * Both of these empty fmt[] in the parse, so an echo that read fmt[0]
	 * would name payload type 0, which the withdrawn stream never offered.
	 */
	CASE("t38 answer: a withdrawn telephone-event only stream echoes its own type");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 101\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\n"
		    "m=image 5006 udptl t38\r\n"));
	CHECK_UINT(m.audio.fmts, 0);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 0 RTP/AVP 101\r\n"));
	CHECK(!has("m=audio 0 RTP/AVP 0\r\n"));

	CASE("t38 answer: a withdrawn CN only stream echoes its own type");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 13\r\nm=image 5006 udptl t38\r\n"));
	CHECK_UINT(m.audio.fmts, 0);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 0 RTP/AVP 13\r\n"));
	CHECK(!has("m=audio 0 RTP/AVP 0\r\n"));

	/*
	 * The audio exchange is loaded on purpose: proposed still holds its
	 * codec, both payload types and the peer hold when the upgrade arrives,
	 * so a T.38 answer that forgot to reset the result would carry audio
	 * state into a stream that has none.
	 */
	CASE("t38 answer: a real committed exchange opens the gate");
	t38_local_init(&loc);
	loc.cn = true;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 13 101\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\na=sendonly\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);
	CHECK_UINT(st.proposed.dtmf_pt, 101);
	CHECK_UINT(st.proposed.cn_pt, 13);
	CHECK(st.proposed.remote_hold);
	sdp_commit(&st);
	CHECK(st.committed.valid);
	CHECK(t38_offer_parse(""));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("o=- 55 1 IN IP4 192.0.2.10\r\n"));
	CHECK(has("m=image 4000 udptl t38\r\n"));
	CHECK(st.proposed.t38);

	CASE("t38 answer: the upgrade leaks no audio state into the result");
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_NONE);
	CHECK_UINT(st.proposed.codec.pt, 0);
	CHECK(st.proposed.dtmf_pt == -1);
	CHECK(st.proposed.cn_pt == -1);
	CHECK(!st.proposed.remote_hold);
	CHECK(!st.proposed.image_refused);
	CHECK_UINT(st.proposed.remote_port, 5006);
	CHECK_UINT(st.proposed.ptime_ms, 0);

	CASE("t38 answer: the offered rate is taken when it is lower");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse("a=T38MaxBitRate:9600\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=T38MaxBitRate:9600\r\n"));
	CHECK_UINT(st.proposed.t38_max_bit_rate, 9600);

	CASE("t38 answer: a higher offered rate is clamped to ours");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse("a=T38MaxBitRate:33600\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=T38MaxBitRate:14400\r\n"));
	CHECK_UINT(st.proposed.t38_max_bit_rate, 14400);

	/*
	 * The lowest rate the DSP can run is 2400. At or above
	 * it the plain minimum holds, because the stream then stays under the
	 * ceiling the answer advertised. Below it a kept value would advertise a
	 * ceiling the running stream has to break, so it reads as absent.
	 */
	CASE("t38 answer: the lowest real rate is still taken");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse("a=T38MaxBitRate:2400\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=T38MaxBitRate:2400\r\n"));
	CHECK_UINT(st.proposed.t38_max_bit_rate, 2400);

	CASE("t38 answer: a rate below the lowest real one reads as absent");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse("a=T38MaxBitRate:1200\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=T38MaxBitRate:14400\r\n"));
	CHECK(!has("1200"));
	CHECK_UINT(st.proposed.t38_max_bit_rate, 14400);

	CASE("t38 answer: an offered rate of zero reads as absent");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse("a=T38MaxBitRate:0\r\n"));
	CHECK(m.image.t38.max_bit_rate_present);
	CHECK_UINT(m.image.t38.max_bit_rate, 0);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=T38MaxBitRate:14400\r\n"));
	CHECK_UINT(st.proposed.t38_max_bit_rate, 14400);

	CASE("t38 answer: an absent rate falls back to ours");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse("a=T38FaxVersion:0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=T38MaxBitRate:14400\r\n"));
	CHECK_UINT(st.proposed.t38_max_bit_rate, 14400);

	CASE("t38 answer: an unreadable rate falls back to ours");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse("a=T38MaxBitRate:fast\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=T38MaxBitRate:14400\r\n"));
	CHECK_UINT(st.proposed.t38_max_bit_rate, 14400);

	CASE("t38 answer: an offered buffer wins and our datagram stays ours");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse("a=T38FaxMaxBuffer:400\r\na=T38FaxMaxDatagram:160\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=T38FaxMaxBuffer:400\r\n"));
	CHECK(has("a=T38FaxMaxDatagram:512\r\n"));
	CHECK_UINT(st.proposed.t38_max_datagram, 160);

	CASE("t38 answer: a zero offered buffer and datagram fall back to ours");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse("a=T38FaxMaxBuffer:0\r\na=T38FaxMaxDatagram:0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=T38FaxMaxBuffer:200\r\n"));
	CHECK(has("a=T38FaxMaxDatagram:512\r\n"));
	CHECK_UINT(st.proposed.t38_max_datagram, 0);

	CASE("t38 answer: a datagram past the result width is clamped, not wrapped");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse("a=T38FaxMaxDatagram:100000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=T38FaxMaxDatagram:512\r\n"));
	CHECK_UINT(st.proposed.t38_max_datagram, 65535);

	CASE("t38 answer: a named peer limit bounds sends but not our answer");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse("a=T38FaxMaxDatagram:320\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=T38FaxMaxDatagram:512\r\n"));
	CHECK_UINT(st.proposed.t38_max_datagram, 320);

	CASE("t38 answer: no peer limit leaves the send limit unset");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse(""));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=T38FaxMaxDatagram:512\r\n"));
	CHECK_UINT(st.proposed.t38_max_datagram, 0);

	CASE("t38 answer: offered redundancy is answered with redundancy");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse("a=T38FaxUdpEC:t38UDPRedundancy\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=T38MaxBitRate:14400\r\na=T38FaxUdpEC:t38UDPRedundancy\r\n"
		  "a=T38FaxMaxBuffer:200\r\n"));
	CHECK_UINT(st.proposed.t38_ec, SDP_T38_EC_REDUNDANCY);

	CASE("t38 answer: offered FEC is downgraded to redundancy");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse("a=T38FaxUdpEC:t38UDPFEC\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=T38FaxUdpEC:t38UDPRedundancy\r\n"));
	CHECK(!has("t38UDPFEC"));
	CHECK_UINT(st.proposed.t38_ec, SDP_T38_EC_REDUNDANCY);

	CASE("t38 answer: an absent error correction is left out of the answer");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse(""));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!has("T38FaxUdpEC"));
	CHECK_UINT(st.proposed.t38_ec, SDP_T38_EC_NONE);

	CASE("t38 answer: an unreadable error correction is left out too");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse("a=T38FaxUdpEC:t38UDPWhatever\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!has("T38FaxUdpEC"));
	CHECK_UINT(st.proposed.t38_ec, SDP_T38_EC_NONE);

	CASE("t38 answer: the offered rate management never reaches the answer");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse("a=T38FaxRateManagement:localTCF\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=T38FaxRateManagement:transferredTCF\r\n"));
	CHECK(!has("localTCF"));

	CASE("t38 answer: an answer that does not fit leaves the state alone");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse(""));
	sip_buf_init(&b, out, 80);
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_BAD);
	CHECK(!sip_buf_ok(&b));
	CHECK_UINT(st.sess_ver, 0);
	CHECK(!st.proposed.t38);
	CHECK(!st.proposed.valid);

	sdp_t38_max_datagram_set(0);
}

/*
 * Every gate miss takes the pre-T.38 path. The refusal ladder and the port-0
 * image tail are the wire form of that path. Each case below therefore
 * asserts the pre-T.38 verdict, not a T.38 one.
 */
static void t38_gate_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;

	CASE("t38 gate: the per-leg enable off takes the refusal path");
	local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(!loc.t38);
	CHECK(t38_offer_parse(""));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!st.proposed.t38);
	CHECK_UINT(st.sess_ver, 0);

	CASE("t38 gate: an initial INVITE has no committed exchange and is refused");
	t38_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(!st.committed.valid);
	CHECK(t38_offer_parse(""));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!st.proposed.t38);

	CASE("t38 gate: an initial image only INVITE is still SDP_BAD");
	t38_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_BAD);
	CHECK_UINT(sip_buf_len(&b), 0);

	CASE("t38 gate: an image port of zero takes the refusal path");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 0\r\nm=image 0 udptl t38\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK(!st.proposed.t38);

	CASE("t38 gate: an image section over tcp takes the refusal path");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 0\r\nm=image 5006 tcp t38\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK(!st.proposed.t38);

	CASE("t38 gate: an image section over RTP/AVP takes the refusal path");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 0\r\nm=image 5006 RTP/AVP t38\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK(!st.proposed.t38);

	CASE("t38 gate: no t38 format token takes the refusal path");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 0\r\nm=image 5006 udptl\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK(!st.proposed.t38);

	CASE("t38 gate: no connection address at all stays SDP_BAD");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 198.51.100.7\r\nt=0 0\r\n"
		    "m=audio 0 RTP/AVP 0\r\nm=image 5006 udptl t38\r\n"));
	CHECK_UINT(m.image.addr[0], 0);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_BAD);
	CHECK(!st.proposed.t38);

	CASE("t38 gate: an image c= that names a host takes the refusal path");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 0\r\nm=image 5006 udptl t38\r\n"
		    "c=IN IP4 fax.example.com\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK(!st.proposed.t38);

	/*
	 * Audio wins: a usable audio section keeps the whole exchange on the
	 * audio path and refuses the image at port 0.
	 */
	CASE("t38 gate: a usable audio section wins over the image section");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\nm=image 5006 udptl t38\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 0\r\n"));
	CHECK(has("a=rtpmap:0 PCMU/8000\r\n"));
	CHECK(has("a=ptime:20\r\na=sendrecv\r\n"));
	CHECK(!st.proposed.t38);
	CHECK(st.proposed.image_refused);
	CHECK_UINT(st.proposed.remote_port, 5004);

	CASE("t38 gate: the port zero image refusal is still the last line");
	CHECK(strlen(out) > 21);
	CHECK(!strcmp(out + strlen(out) - 21, "m=image 0 udptl t38\r\n"));

	CASE("t38 gate: an image first usable audio offer keeps the old shape");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\nm=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 0\r\n"));
	CHECK(!strcmp(out + strlen(out) - 21, "m=image 0 udptl t38\r\n"));
	CHECK(st.proposed.image_refused);
	CHECK(!st.proposed.t38);

	/*
	 * A port the parse could not read lands at 0 exactly like a withdrawn
	 * stream. Accepting it would negotiate T.38 against an audio section
	 * this layer garbled, so only a readable zero is a withdrawal.
	 */
	CASE("t38 gate: an unreadable audio port takes the refusal path");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio nope RTP/AVP 0\r\nm=image 5006 udptl t38\r\n"));
	CHECK(!m.audio.port_ok);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!st.proposed.t38);
	CHECK_UINT(st.sess_ver, 0);

	CASE("t38 gate: an out of range audio port takes the refusal path");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio 70000 RTP/AVP 0\r\nm=image 5006 udptl t38\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK(!st.proposed.t38);

	CASE("t38 gate: a truncated audio m= line takes the refusal path");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio\r\nm=image 5006 udptl t38\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK(!st.proposed.t38);

	CASE("t38 gate: a port count suffix is still a readable zero");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio 0/2 RTP/AVP 0\r\nm=image 5006 udptl t38\r\n"));
	CHECK(m.audio.port_ok);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=image 4000 udptl t38\r\n"));
	CHECK(st.proposed.t38);

	/*
	 * The second section goes to the parse sink, so its live port is
	 * invisible to the audio preference test. The gate closes rather than
	 * negotiate T.38 over an offer that still carries usable audio.
	 */
	CASE("t38 gate: a second audio section takes the refusal path");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 0\r\nm=image 5006 udptl t38\r\n"
		    "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(m.audio_dup);
	CHECK_UINT(m.audio.port, 0);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!st.proposed.t38);

	CASE("t38 gate: a second image section takes the refusal path");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 0\r\nm=image 5006 udptl t38\r\n"
		    "m=image 5008 udptl t38\r\n"));
	CHECK(m.image_dup);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK(!st.proposed.t38);

	/*
	 * 0.0.0.0 passes the literal test and still names no stream. On the
	 * image section there is no direction attribute to read it as hold, so
	 * the only honest verdict is the refusal.
	 */
	CASE("t38 gate: a zero session address takes the refusal path");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\n"
		    "m=audio 0 RTP/AVP 0\r\nm=image 5006 udptl t38\r\n"));
	CHECK(!strcmp(m.image.addr, "0.0.0.0"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!st.proposed.t38);
	CHECK(!st.proposed.valid);

	CASE("t38 gate: a zero image address takes the refusal path");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 0\r\nm=image 5006 udptl t38\r\n"
		    "c=IN IP4 0.0.0.0\r\n"));
	CHECK(!strcmp(m.image.addr, "0.0.0.0"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK(!st.proposed.t38);

	CASE("t38 gate: an audio only offer never reaches the image branch");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!has("m=image"));
	CHECK(!st.proposed.t38);
	CHECK(!st.proposed.image_refused);
}

static void t38_take_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;

	sdp_t38_max_datagram_set(512);

	CASE("t38 take: an image answer fills proposed from the answer");
	t38_take_state_init(&st, &loc);
	CHECK(st.proposed.t38);
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\na=T38MaxBitRate:9600\r\n"
		    "a=T38FaxMaxDatagram:160\r\na=T38FaxUdpEC:t38UDPFEC\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(st.proposed.valid);
	CHECK(st.proposed.t38);
	CHECK_UINT(st.proposed.t38_max_bit_rate, 9600);
	CHECK_UINT(st.proposed.t38_max_datagram, 160);
	CHECK_UINT(st.proposed.t38_ec, SDP_T38_EC_REDUNDANCY);
	CHECK(!strcmp(st.proposed.remote_addr, "198.51.100.7"));
	CHECK_UINT(st.proposed.remote_port, 5006);
	CHECK_UINT(st.proposed.dir, SDP_SENDRECV);
	CHECK_UINT(st.proposed.ptime_ms, 0);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_NONE);
	CHECK(!st.offer_pending);

	CASE("t38 take: an answer without T38 attributes takes our numbers");
	t38_take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.t38_max_bit_rate, 14400);
	CHECK_UINT(st.proposed.t38_max_datagram, 0);
	CHECK_UINT(st.proposed.t38_ec, SDP_T38_EC_NONE);

	CASE("t38 take: a named peer limit bounds sends");
	t38_take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\n"
		    "a=T38FaxMaxDatagram:320\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.t38_max_datagram, 320);

	CASE("t38 take: no peer limit leaves the send limit unset");
	t38_take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.t38_max_datagram, 0);

	CASE("t38 take: a peer limit past the result width is clamped");
	t38_take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\n"
		    "a=T38FaxMaxDatagram:100000\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.t38_max_datagram, 65535);

	CASE("t38 take: a higher answered rate is clamped to ours");
	t38_take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\na=T38MaxBitRate:33600\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.t38_max_bit_rate, 14400);

	CASE("t38 take: the lowest real answered rate is still taken");
	t38_take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\na=T38MaxBitRate:2400\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.t38_max_bit_rate, 2400);

	CASE("t38 take: an answered rate below the lowest real one reads as absent");
	t38_take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\na=T38MaxBitRate:300\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.t38_max_bit_rate, 14400);

	CASE("t38 take: an answered rate of zero reads as absent");
	t38_take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\na=T38MaxBitRate:0\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.t38_max_bit_rate, 14400);

	CASE("t38 take: an audio answer to a T.38 offer is SDP_NO_CODEC");
	t38_take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);
	CHECK(st.offer_pending);

	CASE("t38 take: an image answer at port zero is SDP_NO_CODEC");
	t38_take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=image 0 udptl t38\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);
	CHECK(st.offer_pending);

	CASE("t38 take: an answer without the t38 token is SDP_NO_CODEC");
	t38_take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=image 5006 udptl\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(st.offer_pending);

	CASE("t38 take: an answer over RTP/AVP is SDP_NO_CODEC");
	t38_take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=image 5006 RTP/AVP t38\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(st.offer_pending);

	CASE("t38 take: an image c= that names a host is SDP_NO_CODEC");
	t38_take_state_init(&st, &loc);
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 fax.example.com\r\n"
		    "c=IN IP4 fax.example.com\r\nt=0 0\r\n"
		    "m=image 5006 udptl t38\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(st.offer_pending);

	CASE("t38 take: a zero image address is SDP_NO_CODEC");
	t38_take_state_init(&st, &loc);
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\n"
		    "m=image 5006 udptl t38\r\n"));
	CHECK(!strcmp(m.image.addr, "0.0.0.0"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);
	CHECK(st.offer_pending);

	CASE("t38 take: an answer with no connection address is SDP_NO_CODEC");
	t38_take_state_init(&st, &loc);
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 198.51.100.7\r\nt=0 0\r\n"
		    "m=image 5006 udptl t38\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(st.offer_pending);

	CASE("t38 take: an image section beside an audio one still wins");
	t38_take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\nm=image 5006 udptl t38\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(st.proposed.t38);
	CHECK_UINT(st.proposed.remote_port, 5006);

	CASE("t38 take: a repeated answer is refused once the offer was taken");
	CHECK(!st.offer_pending);
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_BAD);

	CASE("t38 take: an audio pending offer ignores the image section");
	t38_local_init(&loc);
	sdp_state_init(&st, 77);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!st.proposed.t38);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\nm=image 5006 udptl t38\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(!st.proposed.t38);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);
	CHECK_UINT(st.proposed.remote_port, 5004);
	CHECK_UINT(st.proposed.ptime_ms, 20);

	CASE("t38 take: an audio pending offer still refuses an image only answer");
	t38_local_init(&loc);
	sdp_state_init(&st, 77);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(st.offer_pending);

	sdp_t38_max_datagram_set(0);
}

static void t38_state_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;

	CASE("t38 state: commit carries the T.38 result and its numbers");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse("a=T38MaxBitRate:4800\r\na=T38FaxMaxDatagram:96\r\n"
			      "a=T38FaxUdpEC:t38UDPRedundancy\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	sdp_commit(&st);
	CHECK(st.committed.t38);
	CHECK(st.committed.valid);
	CHECK_UINT(st.committed.t38_max_bit_rate, 4800);
	CHECK_UINT(st.committed.t38_max_datagram, 96);
	CHECK_UINT(st.committed.t38_ec, SDP_T38_EC_REDUNDANCY);
	CHECK_UINT(st.committed.remote_port, 5006);

	CASE("t38 state: a refused upgrade rolls back to the audio exchange");
	t38_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	sdp_commit(&st);
	sdp_t38_set(&st, true);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(st.proposed.t38);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	sdp_rollback(&st);
	CHECK(!st.proposed.t38);
	CHECK(st.proposed.valid);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);
	CHECK_UINT(st.proposed.remote_port, 5004);
	CHECK_UINT(st.proposed.ptime_ms, 20);
	CHECK_UINT(st.proposed.t38_max_bit_rate, 0);

	CASE("t38 state: rollback restores a committed T.38 exchange");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(t38_offer_parse("a=T38MaxBitRate:7200\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	sdp_commit(&st);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!st.proposed.t38);
	sdp_rollback(&st);
	CHECK(st.proposed.t38);
	CHECK_UINT(st.proposed.t38_max_bit_rate, 7200);
	CHECK_UINT(st.proposed.remote_port, 5006);

	CASE("t38 state: state init clears the T.38 fields");
	sdp_state_init(&st, 99);
	CHECK(!st.local_t38);
	CHECK(!st.committed.t38);
	CHECK(!st.proposed.t38);
	CHECK_UINT(st.committed.t38_max_bit_rate, 0);
	CHECK_UINT(st.proposed.t38_max_datagram, 0);
	CHECK_UINT(st.proposed.t38_ec, SDP_T38_EC_NONE);
}

/*
 * SDES SRTP (RFC 4568). The keys are fixtures, printable where a case asserts
 * what came off the wire: a base64 literal below is the exact text of the
 * bytes named beside it, so a codec that drifts fails by name.
 *
 * KEY0 and KEY1 are the two keys a caller places in sdp_local, one per suite.
 * The R_* literals are what a peer sends.
 */
#define S80		"AES_CM_128_HMAC_SHA1_80"
#define S32		"AES_CM_128_HMAC_SHA1_32"

#define KEY0_B64	"AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0e"	/* 0x01..0x1e */
#define KEY1_B64	"ZWZnaGlqa2xtbm9wcXJzdHV2d3h5ent8fX5/gIGC"	/* 0x65..0x82 */

#define R_TEXT		"abcdefghijklmnopqrstuvwxyz0123"
#define R_BASE		"YWJjZGVmZ2hpamtsbW5vcHFyc3R1dnd4eXowMTIz"
#define R_ZERO29	"YWJjZGVmZ2hpamtsbW5vcHFyc3R1dnd4eXowMTIA"

/* R_TEXT with 0x00 at bytes 0, 5 and 28 */
#define R_ZEROS		"AGJjZGUAZ2hpamtsbW5vcHFyc3R1dnd4eXowMQAz"
static const uint8_t r_zeros_bytes[SDP_SRTP_KEY_LEN] = {
	0x00, 0x62, 0x63, 0x64, 0x65, 0x00, 0x67, 0x68, 0x69, 0x6a,
	0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74,
	0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x30, 0x31, 0x00, 0x33,
};

#define R_SECOND_TEXT	"ZYXWVUTSRQPONMLKJIHGFEDCBA9876"
#define R_SECOND	"WllYV1ZVVFNSUVBPTk1MS0pJSEdGRURDQkE5ODc2"

/* RFC 4648 10: BASE64("foobar") = "Zm9vYmFy", five times over is 30 bytes */
#define R_FOOBAR_TEXT	"foobarfoobarfoobarfoobarfoobar"
#define R_FOOBAR	"Zm9vYmFyZm9vYmFyZm9vYmFyZm9vYmFyZm9vYmFy"

/* the alphabet's last two characters, which no printable key would reach */
#define R_PLUS		"U/MnZqcNE9OKGV6WD+mCNwoXcGwSPhiObRDUkSDz"
static const uint8_t r_plus_bytes[SDP_SRTP_KEY_LEN] = {
	0x53, 0xf3, 0x27, 0x66, 0xa7, 0x0d, 0x13, 0xd3, 0x8a, 0x19,
	0x5e, 0x96, 0x0f, 0xe9, 0x82, 0x37, 0x0a, 0x17, 0x70, 0x6c,
	0x12, 0x3e, 0x18, 0x8e, 0x6d, 0x10, 0xd4, 0x91, 0x20, 0xf3,
};

#define SAVP_M		"m=audio 5004 RTP/SAVP 0\r\n"
#define CRYPTO(tag, suite, key)	"a=crypto:" tag " " suite " inline:" key "\r\n"

/* a 30 byte key against the text it decodes from, terminator excluded */
static bool key_is(const uint8_t *key, const char *text)
{
	return !memcmp(key, text, SDP_SRTP_KEY_LEN);
}

static bool key_eq(const uint8_t *a, const uint8_t *b2)
{
	return !memcmp(a, b2, SDP_SRTP_KEY_LEN);
}

/* what a plain exchange leaves behind: a case compares all 30 bytes, not one */
static const uint8_t key_none[SDP_SRTP_KEY_LEN] = { 0 };

/*
 * The caller owned key material of one leg: srtp_key[0] keys the _80 suite and
 * srtp_key[1] the _32 suite, which is the index = suite - 1 rule shared/sdp.h
 * states.
 */
static void srtp_local_init(struct sdp_local *loc)
{
	unsigned int i;

	local_init(loc);
	loc->srtp = true;
	for (i = 0; i < SDP_SRTP_KEY_LEN; i++) {
		loc->srtp_key[0][i] = (uint8_t)(1 + i);
		loc->srtp_key[1][i] = (uint8_t)(101 + i);
	}
}

/* the mid-call fixture: a committed SAVP exchange under the named suite */
static void srtp_committed_init(struct sdp_state *st, uint8_t suite)
{
	sdp_state_init(st, 55);
	st->committed.valid = true;
	st->committed.srtp_suite = suite;
}

static void srtp_parse_tests(void)
{
	CASE("srtp parse: RTP/SAVP sets savp and leaves proto_ok clear");
	CHECK(parse(HEAD SAVP_M));
	CHECK(m.audio.present);
	CHECK(m.audio.savp);
	CHECK(!m.audio.proto_ok);

	CASE("srtp parse: the SAVP transport is matched case insensitively");
	CHECK(parse(HEAD "m=audio 5004 rtp/savp 0\r\n"));
	CHECK(m.audio.savp);

	CASE("srtp parse: RTP/AVP leaves savp clear");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(!m.audio.savp);
	CHECK(m.audio.proto_ok);
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: RTP/SAVPF and DTLS are neither transport");
	CHECK(parse(HEAD "m=audio 5004 RTP/SAVPF 0\r\n"));
	CHECK(!m.audio.savp);
	CHECK(!m.audio.proto_ok);
	CHECK(parse(HEAD "m=audio 5004 UDP/TLS/RTP/SAVP 0\r\n"));
	CHECK(!m.audio.savp);
	CHECK(!m.audio.proto_ok);

	CASE("srtp parse: a crypto line stores the tag, the suite and 30 key bytes");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_FOOBAR)));
	CHECK_UINT(m.audio.cryptos, 1);
	CHECK_UINT(m.audio.crypto[0].tag, 1);
	CHECK_UINT(m.audio.crypto[0].suite, SDP_SRTP_AES_CM_128_SHA1_80);
	CHECK(key_is(m.audio.crypto[0].key, R_FOOBAR_TEXT));

	CASE("srtp parse: the _32 suite token is stored under its own value");
	CHECK(parse(HEAD SAVP_M CRYPTO("2", S32, R_BASE)));
	CHECK_UINT(m.audio.cryptos, 1);
	CHECK_UINT(m.audio.crypto[0].tag, 2);
	CHECK_UINT(m.audio.crypto[0].suite, SDP_SRTP_AES_CM_128_SHA1_32);
	CHECK(key_is(m.audio.crypto[0].key, R_TEXT));

	CASE("srtp parse: an unknown suite token drops the line");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", "AES_CM_128_HMAC_SHA1_48", R_BASE)));
	CHECK_UINT(m.audio.cryptos, 0);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", "F8_128_HMAC_SHA1_80", R_BASE)));
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: the suite token is matched case sensitively");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", "aes_cm_128_hmac_sha1_80", R_BASE)));
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: a missing tag drops the line");
	CHECK(parse(HEAD SAVP_M "a=crypto:\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);
	CHECK(parse(HEAD SAVP_M "a=crypto: " S80 " inline:" R_BASE "\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: a tag that is not a number drops the line");
	CHECK(parse(HEAD SAVP_M CRYPTO("1a", S80, R_BASE)));
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: nine digits of tag are taken and ten drop the line");
	CHECK(parse(HEAD SAVP_M CRYPTO("999999999", S80, R_BASE)));
	CHECK_UINT(m.audio.cryptos, 1);
	CHECK_UINT(m.audio.crypto[0].tag, 999999999u);
	CHECK(parse(HEAD SAVP_M CRYPTO("1234567890", S80, R_BASE)));
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: a lifetime is parsed and ignored, and the line stays");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE "|2^31")));
	CHECK_UINT(m.audio.cryptos, 1);
	CHECK(key_is(m.audio.crypto[0].key, R_TEXT));
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE "|1048576")));
	CHECK_UINT(m.audio.cryptos, 1);

	CASE("srtp parse: the 2^ lifetime form is kept");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE "|2^48")));
	CHECK_UINT(m.audio.cryptos, 1);
	CHECK(key_is(m.audio.crypto[0].key, R_TEXT));

	CASE("srtp parse: a plain decimal lifetime is kept");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE "|4294967296")));
	CHECK_UINT(m.audio.cryptos, 1);

	CASE("srtp parse: a bare bar with no lifetime drops the line");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE "|")));
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: a lifetime that is not a number drops the line");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE "|abc")));
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: a lifetime of 2^ alone drops the line");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE "|2^")));
	CHECK_UINT(m.audio.cryptos, 0);

	/*
	 * RFC 4568 9.1: the ';' starts a second key-param, which this layer
	 * refuses. Behind a lifetime it would otherwise ride past the
	 * session-param refusal, which only reads whitespace separated words.
	 */
	CASE("srtp parse: a session parameter behind a lifetime drops the line");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE "|2^31;UNENCRYPTED_SRTP")));
	CHECK_UINT(m.audio.cryptos, 0);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE "|2^31;inline:" R_SECOND)));
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: an MKI drops the line");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE "|1:4")));
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: a lifetime and an MKI together drop the line");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE "|2^31|1:4")));
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: every session parameter drops the line");
	/* the same line without one, so the parameter is what drops it */
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE)));
	CHECK_UINT(m.audio.cryptos, 1);
	CHECK(parse(HEAD SAVP_M "a=crypto:1 " S80 " inline:" R_BASE " KDR=1\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);
	CHECK(parse(HEAD SAVP_M "a=crypto:1 " S80 " inline:" R_BASE
		    " UNENCRYPTED_SRTP\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);
	CHECK(parse(HEAD SAVP_M "a=crypto:1 " S80 " inline:" R_BASE
		    " UNENCRYPTED_SRTCP\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);
	CHECK(parse(HEAD SAVP_M "a=crypto:1 " S80 " inline:" R_BASE
		    " UNAUTHENTICATED_SRTP\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);
	CHECK(parse(HEAD SAVP_M "a=crypto:1 " S80 " inline:" R_BASE
		    " FEC_ORDER=FEC_SRTP\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);
	CHECK(parse(HEAD SAVP_M "a=crypto:1 " S80 " inline:" R_BASE
		    " FEC_KEY=inline:" R_SECOND "\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);
	CHECK(parse(HEAD SAVP_M "a=crypto:1 " S80 " inline:" R_BASE " WSH=64\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: two key parameters drop the line");
	CHECK(parse(HEAD SAVP_M "a=crypto:1 " S80 " inline:" R_BASE ";inline:"
		    R_SECOND "\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: a key method other than inline drops the line");
	CHECK(parse(HEAD SAVP_M "a=crypto:1 " S80 " outline:" R_BASE "\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);
	CHECK(parse(HEAD SAVP_M "a=crypto:1 " S80 " " R_BASE "\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: a 39 character base64 drops the line");
	CHECK(parse(HEAD SAVP_M "a=crypto:1 " S80
		    " inline:YWJjZGVmZ2hpamtsbW5vcHFyc3R1dnd4eXowMTI\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: a 41 character base64 drops the line");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE "A")));
	CHECK_UINT(m.audio.cryptos, 0);

	/*
	 * A base64 span longer than the key is a write the decode must refuse,
	 * not only a count it must reject: without the write bound in
	 * key_b64_decode() these two overrun the 30 byte key on the parser's
	 * stack, which ASan reports and a plain build does not see.
	 */
	CASE("srtp parse: a 44 character base64 drops the line");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE "ABCD")));
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: a 200 character base64 drops the line");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80,
				       R_BASE R_BASE R_BASE R_BASE R_BASE)));
	CHECK_UINT(m.audio.cryptos, 0);

	/*
	 * The method is matched by name. "outline:" is refused by its own colon
	 * landing in the base64 span, so only a method of the same length as
	 * "inline:" tells a name test apart from a seven character skip.
	 */
	CASE("srtp parse: the inline key method is matched by name, not by length");
	CHECK(parse(HEAD SAVP_M "a=crypto:1 " S80 " foobar:" R_BASE "\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: an empty inline key drops the line");
	CHECK(parse(HEAD SAVP_M "a=crypto:1 " S80 " inline:\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: a base64 character outside the alphabet drops the line");
	CHECK(parse(HEAD SAVP_M "a=crypto:1 " S80
		    " inline:YWJjZGVmZ2hpamtsbW5vcHFyc3R1dnd4eXowMTI-\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);
	CHECK(parse(HEAD SAVP_M "a=crypto:1 " S80
		    " inline:YWJjZGVmZ2hpamtsbW5vcHFyc3R1dnd4eXowMTI@\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: base64 padding is tolerated while the count stays at 30");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE "====")));
	CHECK_UINT(m.audio.cryptos, 1);
	CHECK(key_is(m.audio.crypto[0].key, R_TEXT));
	CHECK(parse(HEAD SAVP_M "a=crypto:1 " S80
		    " inline:YWJjZGVmZ2hpamtsbW5vcHFyc3R1dnd4eXowMTI=\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: four crypto lines are stored in offer order");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE) CRYPTO("2", S32, R_SECOND)
		    CRYPTO("3", S80, R_FOOBAR) CRYPTO("4", S32, R_PLUS)));
	CHECK_UINT(m.audio.cryptos, 4);
	CHECK_UINT(m.audio.crypto[0].tag, 1);
	CHECK_UINT(m.audio.crypto[1].tag, 2);
	CHECK_UINT(m.audio.crypto[2].tag, 3);
	CHECK_UINT(m.audio.crypto[3].tag, 4);
	CHECK(key_is(m.audio.crypto[2].key, R_FOOBAR_TEXT));

	CASE("srtp parse: a repeated identical tag stores the line once");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE) CRYPTO("1", S80, R_BASE)));
	CHECK_UINT(m.audio.cryptos, 1);
	CHECK_UINT(m.audio.crypto[0].tag, 1);
	CHECK(key_is(m.audio.crypto[0].key, R_TEXT));

	CASE("srtp parse: a duplicate tag with a different key stores the first only");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE) CRYPTO("1", S32, R_SECOND)));
	CHECK_UINT(m.audio.cryptos, 1);
	CHECK_UINT(m.audio.crypto[0].suite, SDP_SRTP_AES_CM_128_SHA1_80);
	CHECK(key_is(m.audio.crypto[0].key, R_TEXT));

	CASE("srtp parse: a duplicate tag leaves room for the other tags");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE) CRYPTO("1", S80, R_SECOND)
		    CRYPTO("2", S32, R_FOOBAR) CRYPTO("2", S32, R_PLUS)));
	CHECK_UINT(m.audio.cryptos, 2);
	CHECK_UINT(m.audio.crypto[0].tag, 1);
	CHECK_UINT(m.audio.crypto[1].tag, 2);
	CHECK(key_is(m.audio.crypto[1].key, R_FOOBAR_TEXT));

	CASE("srtp parse: a fifth crypto line is dropped and the count stays at four");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE) CRYPTO("2", S32, R_SECOND)
		    CRYPTO("3", S80, R_FOOBAR) CRYPTO("4", S32, R_PLUS)
		    CRYPTO("5", S80, R_ZERO29)));
	CHECK_UINT(m.audio.cryptos, SDP_CRYPTO_MAX);
	CHECK_UINT(m.audio.crypto[3].tag, 4);

	CASE("srtp parse: a session level crypto line is ignored");
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 198.51.100.7\r\nc=IN IP4 198.51.100.7\r\n"
		    "t=0 0\r\n" CRYPTO("1", S80, R_BASE) SAVP_M));
	CHECK(m.audio.present);
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: a crypto line on a section we ignore stays there");
	CHECK(parse(HEAD SAVP_M "m=video 5006 RTP/SAVP 96\r\n"
		    CRYPTO("1", S80, R_BASE)));
	CHECK_UINT(m.audio.cryptos, 0);

	CASE("srtp parse: a truncated crypto line drops");
	CHECK(parse(HEAD SAVP_M "a=crypto\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);
	CHECK(parse(HEAD SAVP_M "a=crypto:1\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);
	CHECK(parse(HEAD SAVP_M "a=crypto:1 " S80 "\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);

	/*
	 * These two pin that the crypto branch swallows nothing: every other
	 * attribute of the section still reaches its own parser beside a crypto
	 * line. They cannot pin the branch order, because t38_attr_parse()
	 * ignores a name it does not know, so a crypto line reaching it first
	 * would change no observable here. No case covers the placement before
	 * the catch-all.
	 */
	CASE("srtp parse: the T.38 attributes still parse after the crypto branch");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE)
		    "m=image 5006 udptl t38\r\na=T38FaxVersion:0\r\n"
		    "a=T38MaxBitRate:7200\r\na=T38FaxUdpEC:t38UDPRedundancy\r\n"));
	CHECK_UINT(m.audio.cryptos, 1);
	CHECK(m.image.t38.version_present);
	CHECK_UINT(m.image.t38.max_bit_rate, 7200);
	CHECK_UINT(m.image.t38.ec, SDP_T38_EC_REDUNDANCY);

	CASE("srtp parse: the audio attributes still parse after a crypto line");
	CHECK(parse(HEAD "m=audio 5004 RTP/SAVP 0 101\r\n" CRYPTO("1", S80, R_BASE)
		    "a=rtpmap:101 telephone-event/8000\r\na=ptime:30\r\na=sendonly\r\n"));
	CHECK_UINT(m.audio.dtmf_pt, 101);
	CHECK_UINT(m.audio.ptime_ms, 30);
	CHECK_UINT(m.audio.dir, SDP_SENDONLY);
	CHECK_UINT(m.audio.cryptos, 1);
}

/*
 * The base64 codec. It is private to sdp.c, so every case reads it through the
 * wire: the offer is the encoder and the parse is the decoder. The RFC 4648 10
 * vectors enter where their lengths allow, which is at a multiple of three.
 * "foo" and "foobar" are then whole base64 groups, and appear unchanged as the
 * prefix of a 30 byte key.
 */
static void srtp_base64_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;

	CASE("srtp base64: the RFC 4648 foobar vector encodes as the key prefix");
	srtp_local_init(&loc);
	memcpy(loc.srtp_key[0], R_FOOBAR_TEXT, SDP_SRTP_KEY_LEN);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("inline:" R_FOOBAR "\r\n"));
	CHECK(has("inline:Zm9v"));
	CHECK(has("inline:Zm9vYmFy"));

	CASE("srtp base64: the same vector decodes back to the 30 key bytes");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_FOOBAR)));
	CHECK_UINT(m.audio.cryptos, 1);
	CHECK(key_is(m.audio.crypto[0].key, R_FOOBAR_TEXT));

	CASE("srtp base64: a 30 byte key round trips through the offer and the parse");
	srtp_local_init(&loc);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(parse(out));
	CHECK_UINT(m.audio.cryptos, 2);
	CHECK(key_eq(m.audio.crypto[0].key, loc.srtp_key[0]));
	CHECK(key_eq(m.audio.crypto[1].key, loc.srtp_key[1]));
	CHECK_UINT(m.audio.crypto[0].suite, SDP_SRTP_AES_CM_128_SHA1_80);
	CHECK_UINT(m.audio.crypto[1].suite, SDP_SRTP_AES_CM_128_SHA1_32);

	CASE("srtp base64: the +/ characters of the alphabet decode and encode back");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_PLUS)));
	CHECK_UINT(m.audio.cryptos, 1);
	CHECK(key_eq(m.audio.crypto[0].key, r_plus_bytes));
	srtp_local_init(&loc);
	memcpy(loc.srtp_key[0], r_plus_bytes, SDP_SRTP_KEY_LEN);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("inline:" R_PLUS "\r\n"));

	CASE("srtp base64: a character outside the alphabet is refused");
	CHECK(parse(HEAD SAVP_M "a=crypto:1 " S80
		    " inline:YWJjZGVmZ2hpamtsbW5vcHFyc3R1dnd4eXowMT!z\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);
	CHECK(parse(HEAD SAVP_M "a=crypto:1 " S80
		    " inline:YWJjZGVmZ2hpamtsbW5vcHFyc3R1dnd4eXowMT.z\r\n"));
	CHECK_UINT(m.audio.cryptos, 0);
}

static void srtp_offer_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;
	static const char want[] =
		"v=0\r\n"
		"o=- 42 0 IN IP4 192.0.2.10\r\n"
		"s=-\r\n"
		"c=IN IP4 192.0.2.10\r\n"
		"t=0 0\r\n"
		"m=audio 4000 RTP/SAVP 0 8 101\r\n"
		"a=crypto:1 " S80 " inline:" KEY0_B64 "\r\n"
		"a=crypto:2 " S32 " inline:" KEY1_B64 "\r\n"
		"a=rtpmap:0 PCMU/8000\r\n"
		"a=rtpmap:8 PCMA/8000\r\n"
		"a=rtpmap:101 telephone-event/8000\r\n"
		"a=fmtp:101 0-15\r\n"
		"a=ptime:20\r\n";

	CASE("srtp offer: a leg without srtp writes the offer byte for byte as before");
	offer_plain_local_init(&loc);
	CHECK(!loc.srtp);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!strcmp(out, offer_plain_want));
	CHECK(!has("a=crypto"));
	CHECK(!has("SAVP"));

	CASE("srtp offer: the SAVP offer is byte exact");
	srtp_local_init(&loc);
	sdp_state_init(&st, 42);
	/* the initial offer, the one that still reads the leg enable */
	CHECK(!st.committed.valid);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!strcmp(out, want));

	CASE("srtp offer: both crypto lines precede the first rtpmap");
	CHECK(strstr(out, "a=crypto:1") < strstr(out, "a=rtpmap:"));
	CHECK(strstr(out, "a=crypto:2") < strstr(out, "a=rtpmap:"));
	CHECK(strstr(out, "m=audio") < strstr(out, "a=crypto:1"));

	CASE("srtp offer: no lifetime and no MKI ride the offered lines");
	CHECK(!has("|"));

	CASE("srtp offer: the offer proposes no suite and no key until the answer");
	CHECK(st.offer_pending);
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_NONE);
	CHECK(!st.proposed.srtp_key_tx[0]);
	CHECK(!st.proposed.srtp_key_rx[0]);
	CHECK(!st.proposed.valid);

	CASE("srtp offer: a re-offer emits the same two keys");
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("o=- 42 1 IN IP4 192.0.2.10\r\n"));
	CHECK(has("a=crypto:1 " S80 " inline:" KEY0_B64 "\r\n"));
	CHECK(has("a=crypto:2 " S32 " inline:" KEY1_B64 "\r\n"));

	CASE("srtp offer: a hold re-offer emits the same two keys");
	sdp_hold_set(&st, true);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=sendonly\r\n"));
	CHECK(has("a=crypto:1 " S80 " inline:" KEY0_B64 "\r\n"));
	CHECK(has("a=crypto:2 " S32 " inline:" KEY1_B64 "\r\n"));

	CASE("srtp offer: an SAVP offer that does not fit leaves the state alone");
	srtp_local_init(&loc);
	sdp_state_init(&st, 42);
	sip_buf_init(&b, out, 120);
	CHECK(!sdp_offer_build(&st, &loc, &b));
	CHECK(!sip_buf_ok(&b));
	CHECK_UINT(st.sess_ver, 0);
	CHECK(!st.offer_pending);

	/*
	 * Our re-offer follows the committed exchange. This leg carries the
	 * enable and accepted a plain first offer under accept-plain, so every
	 * re-offer of ours stays plain: a peer that has no SRTP would answer 488
	 * to an RTP/SAVP re-offer, and our own hold or session refresh would
	 * fail against a call that runs well.
	 */
	CASE("srtp offer: a committed plain exchange re-offers RTP/AVP with no crypto");
	srtp_local_init(&loc);
	srtp_committed_init(&st, SDP_SRTP_NONE);
	CHECK(loc.srtp);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 0 8 101\r\n"));
	CHECK(!has("a=crypto"));
	CHECK(!has("SAVP"));
	CHECK(st.offer_pending);
	CHECK_UINT(st.sess_ver, 1);

	CASE("srtp offer: and a hold re-offer of that leg stays plain too");
	sdp_hold_set(&st, true);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 0 8 101\r\n"));
	CHECK(has("a=sendonly\r\n"));
	CHECK(!has("a=crypto"));

	/*
	 * The fax fallback is the same row. A leg that ran T.38 committed an
	 * exchange that names no suite, so the audio re-offer that falls back to
	 * G.711 is plain like the hold above. Without the committed exchange
	 * rule an accept-plain leg that had faxed would re-offer RTP/SAVP here.
	 */
	CASE("srtp offer: the audio re-offer after a committed T.38 exchange is plain");
	srtp_local_init(&loc);
	srtp_committed_init(&st, SDP_SRTP_NONE);
	st.committed.t38 = true;
	CHECK(!st.local_t38);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 0 8 101\r\n"));
	CHECK(!has("a=crypto"));
	CHECK(!has("SAVP"));

	CASE("srtp offer: a committed keyed exchange re-offers RTP/SAVP with both keys");
	srtp_local_init(&loc);
	srtp_committed_init(&st, SDP_SRTP_AES_CM_128_SHA1_80);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/SAVP 0 8 101\r\n"));
	CHECK(has("a=crypto:1 " S80 " inline:" KEY0_B64 "\r\n"));
	CHECK(has("a=crypto:2 " S32 " inline:" KEY1_B64 "\r\n"));

	CASE("srtp offer: the committed _32 suite re-offers both suites the same way");
	srtp_local_init(&loc);
	srtp_committed_init(&st, SDP_SRTP_AES_CM_128_SHA1_32);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/SAVP 0 8 101\r\n"));
	CHECK(has("a=crypto:1 " S80 " inline:" KEY0_B64 "\r\n"));
	CHECK(has("a=crypto:2 " S32 " inline:" KEY1_B64 "\r\n"));
}

static void srtp_answer_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;
	static const char want[] =
		"v=0\r\n"
		"o=- 55 0 IN IP4 192.0.2.10\r\n"
		"s=-\r\n"
		"c=IN IP4 192.0.2.10\r\n"
		"t=0 0\r\n"
		"m=audio 4000 RTP/SAVP 0\r\n"
		"a=crypto:1 " S80 " inline:" KEY0_B64 "\r\n"
		"a=rtpmap:0 PCMU/8000\r\n"
		"a=ptime:20\r\n"
		"a=sendrecv\r\n";

	CASE("srtp answer: an RTP/AVP offer on an SRTP leg is answered plain");
	srtp_local_init(&loc);
	sdp_state_init(&st, 55);
	/* the initial offer, which is the one accept-plain still covers */
	CHECK(!st.committed.valid);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 0\r\n"));
	CHECK(!has("a=crypto"));
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_NONE);
	CHECK(!st.proposed.srtp_key_tx[0]);
	CHECK(!st.proposed.srtp_key_rx[0]);
	CHECK(st.proposed.valid);

	CASE("srtp answer: an AVP offer that carries crypto lines is still plain");
	srtp_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n" CRYPTO("1", S80, R_BASE)));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 0\r\n"));
	CHECK(!has("a=crypto"));
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_NONE);

	/*
	 * The upgrade direction is unchanged: accept-plain narrows the plain
	 * direction alone. A committed exchange without a suite answers an SAVP
	 * re-offer exactly as a first offer does.
	 */
	CASE("srtp answer: an SAVP re-offer on a committed plain leg is still answered SAVP");
	srtp_local_init(&loc);
	srtp_committed_init(&st, SDP_SRTP_NONE);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE)));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/SAVP 0\r\n"));
	CHECK(has("a=crypto:1 " S80 " inline:" KEY0_B64 "\r\n"));
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);
	CHECK(key_is(st.proposed.srtp_key_rx, R_TEXT));

	CASE("srtp answer: an SAVP offer on a leg without srtp is SDP_NO_CODEC");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE)));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!strcmp(out, ""));
	CHECK(!st.proposed.valid);
	CHECK_UINT(st.sess_ver, 0);

	CASE("srtp answer: the first usable _80 line is mirrored byte for byte");
	srtp_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE)));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!strcmp(out, want));

	CASE("srtp answer: the mirrored line fills the suite and both keys");
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);
	CHECK(key_eq(st.proposed.srtp_key_tx, loc.srtp_key[0]));
	CHECK(key_is(st.proposed.srtp_key_rx, R_TEXT));
	CHECK(st.proposed.valid);
	CHECK_UINT(st.proposed.remote_port, 5004);

	CASE("srtp answer: a _32 line at tag 7 is mirrored with tag 7 and our second key");
	srtp_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD SAVP_M CRYPTO("7", S32, R_SECOND)));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/SAVP 0\r\n"));
	CHECK(has("a=crypto:7 " S32 " inline:" KEY1_B64 "\r\n"));
	CHECK(!has("a=crypto:1"));
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_32);
	CHECK(key_eq(st.proposed.srtp_key_tx, loc.srtp_key[1]));
	CHECK(key_is(st.proposed.srtp_key_rx, R_SECOND_TEXT));

	CASE("srtp answer: exactly one crypto line rides an answer to a two line offer");
	srtp_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE) CRYPTO("2", S32, R_SECOND)));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=crypto:1 " S80 " inline:" KEY0_B64 "\r\n"));
	CHECK(!has("a=crypto:2"));
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);

	CASE("srtp answer: a single line with 0x00 at bytes 0, 5 and 28 is mirrored");
	srtp_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_ZEROS)));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/SAVP 0\r\n"));
	CHECK(has("a=crypto:1 " S80 " inline:" KEY0_B64 "\r\n"));
	CHECK(st.proposed.valid);
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);
	CHECK(key_eq(st.proposed.srtp_key_tx, loc.srtp_key[0]));
	CHECK(key_eq(st.proposed.srtp_key_rx, r_zeros_bytes));

	CASE("srtp answer: a key with 0x00 at byte 29 is accepted");
	srtp_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_ZERO29)));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=crypto:1 " S80 " inline:" KEY0_B64 "\r\n"));
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);
	CHECK(!memcmp(st.proposed.srtp_key_rx, R_TEXT, SDP_SRTP_KEY_LEN - 1));
	CHECK_UINT(st.proposed.srtp_key_rx[SDP_SRTP_KEY_LEN - 1], 0);

	CASE("srtp answer: an SAVP offer with no crypto line is SDP_NO_CODEC");
	srtp_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD SAVP_M));
	CHECK_UINT(m.audio.cryptos, 0);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);

	CASE("srtp answer: an SAVP offer whose only line was dropped is SDP_NO_CODEC");
	srtp_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE "|1:4")));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);

	CASE("srtp answer: an SAVPF offer stays SDP_NO_CODEC on an SRTP leg");
	srtp_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/SAVPF 0\r\n" CRYPTO("1", S80, R_BASE)));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);

	CASE("srtp answer: an SAVP offer at port 0 stays SDP_NO_CODEC");
	srtp_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 0 RTP/SAVP 0\r\n" CRYPTO("1", S80, R_BASE)));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);

	CASE("srtp answer: an SAVP offer with no codec of ours is SDP_NO_CODEC");
	srtp_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/SAVP 18\r\n" CRYPTO("1", S80, R_BASE)));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);

	CASE("srtp answer: the SAVP answer still refuses an image section at port 0");
	srtp_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE)
		    "m=image 5006 udptl t38\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/SAVP 0\r\n"));
	CHECK(has("a=crypto:1 " S80 " inline:" KEY0_B64 "\r\n"));
	CHECK(has("m=image 0 udptl t38\r\n"));
	CHECK(st.proposed.image_refused);
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);

	CASE("srtp answer: the image refusal is still the last line");
	CHECK(strlen(out) > 21);
	CHECK(!strcmp(out + strlen(out) - 21, "m=image 0 udptl t38\r\n"));

	CASE("srtp answer: the hold table still runs on an SAVP offer");
	srtp_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE) "a=sendonly\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=recvonly\r\n"));
	CHECK(st.proposed.remote_hold);
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);

	/*
	 * A hold is a re-offer, so the one that keeps the transport is answered
	 * on a keyed leg exactly as it is on a first offer. The case above runs
	 * at the initial state and this one at a committed keyed state, which is
	 * where the accept-plain gate could reach it.
	 */
	CASE("srtp answer: an SAVP hold re-offer on a committed keyed leg still holds");
	srtp_local_init(&loc);
	srtp_committed_init(&st, SDP_SRTP_AES_CM_128_SHA1_80);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE) "a=sendonly\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/SAVP 0\r\n"));
	CHECK(has("a=recvonly\r\n"));
	CHECK(st.proposed.remote_hold);
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);

	/*
	 * The structural refusals stay in front of the accept-plain gate. A body
	 * with no connection address names no stream at all, so it keeps SDP_BAD
	 * on a keyed leg too: the caller ends such an exchange quietly, where a
	 * refusal answers 488.
	 */
	CASE("srtp answer: a plain re-offer with no connection address stays SDP_BAD");
	srtp_local_init(&loc);
	srtp_committed_init(&st, SDP_SRTP_AES_CM_128_SHA1_80);
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 198.51.100.7\r\nt=0 0\r\n"
		    "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK_UINT(m.audio.addr[0], 0);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_BAD);
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!st.proposed.valid);
	CHECK_UINT(st.sess_ver, 0);
}

static void srtp_take_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;

	CASE("srtp take: a plain answer to our SAVP offer is SDP_NO_CODEC");
	srtp_local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);
	CHECK(st.offer_pending);

	CASE("srtp take: an SAVP answer to our plain offer is SDP_NO_CODEC");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE)));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);
	CHECK(st.offer_pending);

	/*
	 * The take reads the transport we offered, and the offer is built here
	 * rather than assumed, so the pair cannot drift: an accept-plain leg
	 * re-offers plain, and the plain answer to it is the answer we asked
	 * for.
	 */
	CASE("srtp take: a plain answer to our plain re-offer on a committed plain leg is taken");
	srtp_local_init(&loc);
	srtp_committed_init(&st, SDP_SRTP_NONE);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!has("a=crypto"));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(st.proposed.valid);
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_NONE);
	CHECK(key_eq(st.proposed.srtp_key_tx, key_none));
	CHECK(key_eq(st.proposed.srtp_key_rx, key_none));
	CHECK(!st.offer_pending);

	CASE("srtp take: an SAVP answer to that plain re-offer is SDP_NO_CODEC");
	srtp_local_init(&loc);
	srtp_committed_init(&st, SDP_SRTP_NONE);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE)));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);
	CHECK(st.offer_pending);

	/*
	 * The invariant guard of the SAVP branch. call.c draws key material
	 * behind loc.srtp alone, so a committed suite implies the enable and
	 * this state cannot occur on a leg. It is an input this layer's API
	 * accepts, and taking it would key the stream with 30 zero bytes.
	 */
	CASE("srtp take: an SAVP answer on a leg that drew no keys is refused");
	srtp_local_init(&loc);
	loc.srtp = false;
	memset(loc.srtp_key, 0, sizeof(loc.srtp_key));
	srtp_committed_init(&st, SDP_SRTP_AES_CM_128_SHA1_80);
	st.offer_pending = true;
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE)));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_NONE);

	CASE("srtp take: a plain answer to our SAVP re-offer on a keyed leg is still refused");
	srtp_local_init(&loc);
	srtp_committed_init(&st, SDP_SRTP_AES_CM_128_SHA1_80);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=crypto:1 " S80 " inline:" KEY0_B64 "\r\n"));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);
	CHECK(st.offer_pending);
	CHECK_UINT(st.committed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);

	CASE("srtp take: tag 1 with the _80 suite fills the suite and both keys");
	srtp_local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE)));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);
	CHECK(key_eq(st.proposed.srtp_key_tx, loc.srtp_key[0]));
	CHECK(key_is(st.proposed.srtp_key_rx, R_TEXT));
	CHECK(st.proposed.valid);
	CHECK(!st.offer_pending);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);

	CASE("srtp take: tag 2 with the _32 suite takes our second key");
	srtp_local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD SAVP_M CRYPTO("2", S32, R_SECOND)));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_32);
	CHECK(key_eq(st.proposed.srtp_key_tx, loc.srtp_key[1]));
	CHECK(key_is(st.proposed.srtp_key_rx, R_SECOND_TEXT));

	CASE("srtp take: tag 2 carrying the _80 suite is SDP_NO_CODEC");
	srtp_local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD SAVP_M CRYPTO("2", S80, R_BASE)));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);
	CHECK(st.offer_pending);

	CASE("srtp take: tag 1 carrying the _32 suite is SDP_NO_CODEC");
	srtp_local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S32, R_BASE)));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);

	CASE("srtp take: a tag we never offered is SDP_NO_CODEC");
	srtp_local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD SAVP_M CRYPTO("3", S80, R_BASE)));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);

	CASE("srtp take: an answer key with 0x00 at bytes 0, 5 and 28 is taken");
	srtp_local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_ZEROS)));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);
	CHECK(key_eq(st.proposed.srtp_key_rx, r_zeros_bytes));

	CASE("srtp take: an answer key with 0x00 at byte 29 is taken");
	srtp_local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_ZERO29)));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);
	CHECK_UINT(st.proposed.srtp_key_rx[SDP_SRTP_KEY_LEN - 1], 0);

	CASE("srtp take: an SAVP answer with no crypto line is SDP_NO_CODEC");
	srtp_local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD SAVP_M));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);

	CASE("srtp take: an SAVPF answer to our SAVP offer is SDP_NO_CODEC");
	srtp_local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/SAVPF 0\r\n" CRYPTO("1", S80, R_BASE)));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);

	/*
	 * RFC 4568 5.1.2 lets an answer carry one crypto attribute, so a second
	 * one this layer could also use makes the peer's transmit key ambiguous
	 * and the whole answer goes. A line the parse dropped never counted.
	 */
	CASE("srtp take: two valid crypto lines in an answer are SDP_NO_CODEC");
	srtp_local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE) CRYPTO("2", S32, R_SECOND)));
	CHECK_UINT(m.audio.cryptos, 2);
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);
	CHECK(st.offer_pending);
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_NONE);

	CASE("srtp take: a mirrored line beside an unusable second line is still refused");
	srtp_local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE) CRYPTO("3", S80, R_SECOND)));
	CHECK_UINT(m.audio.cryptos, 2);
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);

	CASE("srtp take: one valid line beside a line the parser dropped is taken");
	srtp_local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE) CRYPTO("2", S32, R_SECOND "|1:4")));
	CHECK_UINT(m.audio.cryptos, 1);
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);
	CHECK(key_eq(st.proposed.srtp_key_tx, loc.srtp_key[0]));
	CHECK(key_is(st.proposed.srtp_key_rx, R_TEXT));

	CASE("srtp take: a duplicate tag does not substitute a usable line");
	srtp_local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD SAVP_M CRYPTO("2", S80, R_BASE) CRYPTO("2", S32, R_SECOND)));
	CHECK_UINT(m.audio.cryptos, 1);
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);

	CASE("srtp take: the plain answer path is unchanged on a plain leg");
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_NONE);
	CHECK(!st.proposed.srtp_key_tx[0]);
	CHECK(!st.proposed.srtp_key_rx[0]);
}

static void srtp_state_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;
	uint64_t ver;

	CASE("srtp state: state init clears the suite and both keys");
	sdp_state_init(&st, 99);
	CHECK_UINT(st.committed.srtp_suite, SDP_SRTP_NONE);
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_NONE);
	CHECK(!st.committed.srtp_key_tx[0]);
	CHECK(!st.proposed.srtp_key_rx[SDP_SRTP_KEY_LEN - 1]);

	CASE("srtp state: commit carries the suite and both keys");
	srtp_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE)));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	sdp_commit(&st);
	CHECK_UINT(st.committed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);
	CHECK(key_eq(st.committed.srtp_key_tx, loc.srtp_key[0]));
	CHECK(key_is(st.committed.srtp_key_rx, R_TEXT));

	CASE("srtp state: a re-offer that is answered moves the proposed keys");
	CHECK(parse(HEAD SAVP_M CRYPTO("2", S32, R_SECOND)));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_32);
	CHECK(key_is(st.proposed.srtp_key_rx, R_SECOND_TEXT));
	CHECK_UINT(st.committed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);

	CASE("srtp state: rollback restores the committed suite and keys");
	sdp_rollback(&st);
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);
	CHECK(key_eq(st.proposed.srtp_key_tx, loc.srtp_key[0]));
	CHECK(key_is(st.proposed.srtp_key_rx, R_TEXT));

	CASE("srtp state: a refused re-offer leaves the committed suite as it was");
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE "|1:4")));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(st.committed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);
	CHECK(key_is(st.committed.srtp_key_rx, R_TEXT));

	/*
	 * Accept-plain narrows to the initial offer. This leg carries a
	 * committed SRTP exchange, so the plain re-offer is refused instead of
	 * answered, and the exchange it would have downgraded stands.
	 */
	CASE("srtp state: a plain re-offer on a committed SRTP exchange is refused");
	CHECK(st.committed.valid);
	ver = st.sess_ver;
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!strcmp(out, ""));
	CHECK(st.sess_ver == ver);
	CHECK_UINT(st.committed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);
	CHECK(key_eq(st.committed.srtp_key_tx, loc.srtp_key[0]));
	CHECK(key_is(st.committed.srtp_key_rx, R_TEXT));

	CASE("srtp state: and the rollback behind that refusal restores the suite");
	sdp_rollback(&st);
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);
	CHECK(key_eq(st.proposed.srtp_key_tx, loc.srtp_key[0]));
	CHECK(key_is(st.proposed.srtp_key_rx, R_TEXT));

	CASE("srtp state: the suite name of every value");
	CHECK(!strcmp(sdp_srtp_suite_name(SDP_SRTP_AES_CM_128_SHA1_80), S80));
	CHECK(!strcmp(sdp_srtp_suite_name(SDP_SRTP_AES_CM_128_SHA1_32), S32));
	CHECK(!strcmp(sdp_srtp_suite_name(SDP_SRTP_NONE), "none"));
}

/*
 * The T.38 gate on an SRTP leg. The image stream is plain UDPTL, so an upspeed
 * would drop the encryption without a word. The committed suite therefore
 * closes the answer gate and refuses to build an offer.
 */
static void srtp_t38_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;

	CASE("srtp t38: a committed suite refuses to build a T.38 offer");
	srtp_local_init(&loc);
	loc.t38 = true;
	srtp_committed_init(&st, SDP_SRTP_AES_CM_128_SHA1_80);
	sdp_t38_set(&st, true);
	buf_reset();
	CHECK(!sdp_offer_build(&st, &loc, &b));
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!strcmp(out, ""));

	CASE("srtp t38: the refused T.38 offer moves no state");
	CHECK_UINT(st.sess_ver, 0);
	CHECK(!st.offer_pending);
	CHECK(!st.proposed.t38);
	CHECK(st.local_t38);
	CHECK_UINT(st.committed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);

	CASE("srtp t38: the same leg with a plain committed exchange offers T.38");
	st.committed.srtp_suite = SDP_SRTP_NONE;
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=image 4000 udptl t38\r\n"));
	CHECK(st.proposed.t38);
	CHECK_UINT(st.sess_ver, 1);

	/* the image offer names no audio transport at all, so the committed
	 * exchange rule that picks one never reaches it */
	CASE("srtp t38: and that image offer carries no audio transport and no crypto");
	CHECK(!has("RTP/"));
	CHECK(!has("a=crypto"));

	CASE("srtp t38: a committed suite refuses a withdrawn audio T.38 offer");
	srtp_local_init(&loc);
	loc.t38 = true;
	srtp_committed_init(&st, SDP_SRTP_AES_CM_128_SHA1_32);
	CHECK(t38_offer_parse(""));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!st.proposed.t38);
	CHECK(!st.proposed.valid);
	CHECK_UINT(st.sess_ver, 0);

	CASE("srtp t38: the same offer upgrades when the committed exchange is plain");
	srtp_local_init(&loc);
	loc.t38 = true;
	srtp_committed_init(&st, SDP_SRTP_NONE);
	CHECK(t38_offer_parse(""));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=image 4000 udptl t38\r\n"));
	CHECK(st.proposed.t38);

	CASE("srtp t38: a committed suite answers a live SAVP offer with the image at port 0");
	srtp_local_init(&loc);
	loc.t38 = true;
	srtp_committed_init(&st, SDP_SRTP_AES_CM_128_SHA1_80);
	CHECK(parse(HEAD SAVP_M CRYPTO("1", S80, R_BASE)
		    "m=image 5006 udptl t38\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/SAVP 0\r\n"));
	CHECK(has("m=image 0 udptl t38\r\n"));
	CHECK(!st.proposed.t38);
	CHECK(st.proposed.image_refused);
	CHECK_UINT(st.proposed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);

	/*
	 * The same offer with plain audio is refused whole. The T.38 gate closes
	 * on the committed suite, so without the accept-plain narrowing this
	 * offer would take the audio ladder and be answered plain with the image
	 * at port 0, which is the downgrade the narrowing closes. The image
	 * section changes nothing: the audio it carries is live and it carries
	 * no SRTP.
	 */
	CASE("srtp t38: a committed suite refuses an image offer beside LIVE plain audio");
	srtp_local_init(&loc);
	loc.t38 = true;
	srtp_committed_init(&st, SDP_SRTP_AES_CM_128_SHA1_80);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"
		    "m=image 5006 udptl t38\r\n"));
	CHECK(m.audio.present);
	CHECK_UINT(m.audio.port, 5004);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!strcmp(out, ""));
	CHECK(!st.proposed.t38);
	CHECK_UINT(st.sess_ver, 0);
	CHECK_UINT(st.committed.srtp_suite, SDP_SRTP_AES_CM_128_SHA1_80);
}

/*
 * G.722, a wideband codec. Its RTP clock is 8000 (RFC 3551 section
 * 4.5.2) while its audio runs at 16 kHz, so every line this layer writes for
 * it names 8000 and the HAL owns the audio rate. It reaches an offer only
 * through a local view that carries it, which is what leaves the default offer
 * byte for byte as it was.
 */
static void g722_local_init(struct sdp_local *loc)
{
	local_init(loc);
	loc->codec[0].id = SDP_CODEC_G722;
	loc->codec[0].pt = 9;
	loc->codec[1].id = SDP_CODEC_PCMU;
	loc->codec[1].pt = 0;
	loc->codecs = 2;
}

static void g726_local_init(struct sdp_local *loc)
{
	local_init(loc);
	loc->codec[0].id = SDP_CODEC_G726_32;
	loc->codec[0].pt = 111;
	loc->codec[1].id = SDP_CODEC_PCMU;
	loc->codec[1].pt = 0;
	loc->codecs = 2;
}

static void g726_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;
	const char offer_want[] =
		"v=0\r\n"
		"o=- 42 0 IN IP4 192.0.2.10\r\n"
		"s=-\r\n"
		"c=IN IP4 192.0.2.10\r\n"
		"t=0 0\r\n"
		"m=audio 4000 RTP/AVP 111 0 101 13\r\n"
		"a=rtpmap:111 G726-32/8000\r\n"
		"a=rtpmap:0 PCMU/8000\r\n"
		"a=rtpmap:101 telephone-event/8000\r\n"
		"a=fmtp:101 0-15\r\n"
		"a=rtpmap:13 CN/8000\r\n"
		"a=ptime:20\r\n";
	const char answer_want[] =
		"v=0\r\n"
		"o=- 55 0 IN IP4 192.0.2.10\r\n"
		"s=-\r\n"
		"c=IN IP4 192.0.2.10\r\n"
		"t=0 0\r\n"
		"m=audio 4000 RTP/AVP 111 101 13\r\n"
		"a=rtpmap:111 G726-32/8000\r\n"
		"a=rtpmap:101 telephone-event/8000\r\n"
		"a=fmtp:101 0-15\r\n"
		"a=rtpmap:13 CN/8000\r\n"
		"a=ptime:20\r\n"
		"a=sendrecv\r\n";

	CASE("g726: the configured dynamic payload type writes an exact offer");
	g726_local_init(&loc);
	loc.cn = true;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!strcmp(out, offer_want));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_G726_32);
	CHECK_UINT(st.proposed.codec.pt, 111);

	CASE("g726: comfort noise pairs at 8000 Hz");
	CHECK(has("a=rtpmap:13 CN/8000\r\n"));
	CHECK_UINT(st.proposed.cn_pt, 13);

	CASE("g726: an offer at payload type 111 gets an exact answer");
	g726_local_init(&loc);
	loc.cn = true;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 111 101 13\r\n"
		    "a=rtpmap:111 G726-32/8000\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\n"
		    "a=rtpmap:13 CN/8000\r\n"
		    "a=ptime:20\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!strcmp(out, answer_want));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_G726_32);

	CASE("g726: an answer follows the peer payload type");
	g726_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 112\r\n"
		    "a=rtpmap:112 G726-32/8000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 112\r\n"));
	CHECK(has("a=rtpmap:112 G726-32/8000\r\n"));
	CHECK_UINT(st.proposed.codec.pt, 112);

	CASE("g726: the lower-case encoding name is accepted");
	g726_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 111\r\n"
		    "a=rtpmap:111 g726-32/8000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_G726_32);

	CASE("g726: the AAL2 encoding name has no audio codec match");
	g726_local_init(&loc);
	loc.codecs = 1;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 111\r\n"
		    "a=rtpmap:111 AAL2-G726-32/8000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!strcmp(out, ""));
	CHECK(!st.proposed.valid);
	CHECK_UINT(st.sess_ver, 0);

	CASE("g726: an offered ptime of 10 is accepted");
	g726_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 111\r\n"
		    "a=rtpmap:111 G726-32/8000\r\n"
		    "a=ptime:10\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=ptime:10\r\n"));

	CASE("g726: an offered ptime of 20 is accepted");
	g726_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 111\r\n"
		    "a=rtpmap:111 G726-32/8000\r\n"
		    "a=ptime:20\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=ptime:20\r\n"));

	CASE("g726: an offered ptime of 30 is accepted like G.711");
	g726_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 111\r\n"
		    "a=rtpmap:111 G726-32/8000\r\n"
		    "a=ptime:30\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=ptime:30\r\n"));
	CHECK_UINT(st.proposed.ptime_ms, 30);

	CASE("g726: an offer keeps a configured ptime of 30");
	g726_local_init(&loc);
	loc.ptime_ms = 30;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=ptime:30\r\n"));
	CHECK(!has("a=maxptime:"));
}

static void g722_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;
	const char offer_g722_want[] =
		"v=0\r\n"
		"o=- 42 0 IN IP4 192.0.2.10\r\n"
		"s=-\r\n"
		"c=IN IP4 192.0.2.10\r\n"
		"t=0 0\r\n"
		"m=audio 4000 RTP/AVP 9 0 101\r\n"
		"a=rtpmap:9 G722/8000\r\n"
		"a=rtpmap:0 PCMU/8000\r\n"
		"a=rtpmap:101 telephone-event/8000\r\n"
		"a=fmtp:101 0-15\r\n"
		"a=ptime:20\r\n";
	const char answer_g722_want[] =
		"v=0\r\n"
		"o=- 55 0 IN IP4 192.0.2.10\r\n"
		"s=-\r\n"
		"c=IN IP4 192.0.2.10\r\n"
		"t=0 0\r\n"
		"m=audio 4000 RTP/AVP 9 101\r\n"
		"a=rtpmap:9 G722/8000\r\n"
		"a=rtpmap:101 telephone-event/8000\r\n"
		"a=fmtp:101 0-15\r\n"
		"a=ptime:20\r\n"
		"a=sendrecv\r\n";

	/*
	 * The default offer oracle. offer_plain_want is the default offer of
	 * this layer and it did not move: a local view that names no G.722
	 * writes the same bytes it wrote before wideband existed.
	 */
	CASE("g722: a local view without G722 writes the default offer byte for byte");
	offer_plain_local_init(&loc);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!strcmp(out, offer_plain_want));
	CHECK(!has("G722"));
	CHECK(!has("a=rtpmap:9 "));

	CASE("g722: the configured offer is byte exact");
	g722_local_init(&loc);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!strcmp(out, offer_g722_want));
	CHECK(!has("/16000"));
	CHECK(!has("a=fmtp:9"));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_G722);
	CHECK_UINT(st.proposed.codec.pt, 9);

	CASE("g722: a payload type of 129 disables it like every other codec");
	g722_local_init(&loc);
	loc.codec[0].pt = SDP_PT_DISABLED;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 0 101\r\n"));
	CHECK(!has("G722"));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);

	/*
	 * Comfort noise pairs with G.722 the way it pairs with G.711, at
	 * payload type 13 on the same 8000 clock, because one SSRC carries one
	 * clock (RFC 7160) and the decoder synthesises the 16 kHz noise itself.
	 */
	CASE("g722: comfort noise rides payload type 13 at the 8000 clock");
	g722_local_init(&loc);
	loc.cn = true;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 9 0 101 13\r\n"));
	CHECK(has("a=rtpmap:13 CN/8000\r\n"));
	CHECK(!has("CN/16000"));
	CHECK_UINT(st.proposed.cn_pt, 13);

	CASE("g722: telephone-event stays at the 8000 clock beside it");
	CHECK(has("a=rtpmap:101 telephone-event/8000\r\n"));
	CHECK(!has("telephone-event/16000"));

	CASE("g722: the answer selecting it is byte exact");
	g722_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 9 101\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\na=ptime:20\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!strcmp(out, answer_g722_want));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_G722);
	CHECK_UINT(st.proposed.codec.pt, 9);

	CASE("g722: an answer follows the offer order and takes G722 first");
	g722_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 9 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 9\r\n"));
	CHECK(!has("PCMU"));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_G722);

	CASE("g722: an offer that lists PCMU first is answered PCMU");
	g722_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 9\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 0\r\n"));
	CHECK(has("a=rtpmap:0 PCMU/8000\r\n"));
	CHECK(!has("G722"));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);

	CASE("g722: an answer mirrors a dynamic payload type the peer numbered");
	g722_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 G722/8000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 96\r\n"));
	CHECK(has("a=rtpmap:96 G722/8000\r\n"));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_G722);
	CHECK_UINT(st.proposed.codec.pt, 96);

	/* the rate gate: 16000 names a stream this side cannot key (RFC 3551) */
	CASE("g722: an offer that names it at 16000 is not selected");
	g722_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 9\r\na=rtpmap:9 G722/16000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);

	CASE("g722: a 16000 rtpmap falls through to the PCMU the offer also listed");
	g722_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 9 0\r\na=rtpmap:9 G722/16000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 0\r\n"));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);

	CASE("g722: the answer to our offer takes it back");
	g722_local_init(&loc);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 9 101\r\n"
		    "a=rtpmap:9 G722/8000\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_G722);
	CHECK_UINT(st.proposed.codec.pt, 9);
	CHECK_UINT(st.proposed.dtmf_pt, 101);
	CHECK_UINT(st.proposed.remote_port, 5004);

	/*
	 * The refusal path. A peer that answers our G.722 offer with PCMU has
	 * refused the wideband codec, and the exchange still programs a call:
	 * the answer decides what this side sends (RFC 3264 section 6.2).
	 */
	CASE("g722: an answer that refuses it falls back to PCMU");
	g722_local_init(&loc);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 9 0 101\r\n"));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 101\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);
	CHECK_UINT(st.proposed.codec.pt, 0);
	CHECK(st.proposed.valid);

	CASE("g722: an answer that names it at 16000 refuses the exchange");
	g722_local_init(&loc);
	loc.codecs = 1;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 9\r\na=rtpmap:9 G722/16000\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);

	/* G.722 takes the same three ptime values as the narrowband codecs */
	CASE("g722: the answered ptime follows the offered 10, 20 or 30");
	g722_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 9\r\na=ptime:30\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=ptime:30\r\n"));
	CHECK_UINT(st.proposed.ptime_ms, 30);

	CASE("g722: an offered ptime of 40 falls back to the configured value");
	g722_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 9\r\na=ptime:40\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=ptime:20\r\n"));
	CHECK_UINT(st.proposed.ptime_ms, 20);

	/*
	 * Opus ended the G.722 bound. An Opus entry is written at its own
	 * clock and a list of Opus alone builds an offer, while the G.722
	 * assertions above them do not move.
	 */
	CASE("g722: an Opus entry beside G722 is written at its own clock");
	g722_local_init(&loc);
	loc.codec[0].id = SDP_CODEC_OPUS;
	loc.codec[0].pt = 96;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 96 0 101 127\r\n"));
	CHECK(has("a=rtpmap:96 opus/48000/2\r\n"));
	CHECK(has("a=rtpmap:0 PCMU/8000\r\n"));
	CHECK(has("a=rtpmap:101 telephone-event/48000\r\n"));
	CHECK(has("a=rtpmap:127 telephone-event/8000\r\n"));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_OPUS);

	CASE("g722: a list of Opus alone still builds an offer");
	g722_local_init(&loc);
	loc.codec[0].id = SDP_CODEC_OPUS;
	loc.codec[0].pt = 96;
	loc.codecs = 1;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 96 101\r\n"));
	CHECK(!has("G722"));

	/* the local view of this suite names no Opus, so nothing matches it */
	CASE("g722: an offered opus rtpmap is not selected");
	g722_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
}

/*
 * Opus. The suite walks the four things that separate it from every codec
 * before it: the 48000 clock of its rtpmap and its telephone-event, the two
 * fmtp records (ours states this decoder's preferences, theirs constrains our
 * encoder), the comfort noise it never pairs with, and the 10/20 packet mask.
 */
static void opus_local_init(struct sdp_local *loc)
{
	local_init(loc);
	loc->codec[0].id = SDP_CODEC_OPUS;
	loc->codec[0].pt = 96;
	loc->codec[0].opus.bitrate = 32000;
	loc->codec[0].opus.complexity = 5;
	loc->codec[0].opus.fec = true;
	loc->codec[1].id = SDP_CODEC_PCMU;
	loc->codec[1].pt = 0;
	loc->codecs = 2;
}

/* the same list with the narrowband codec first, which moves the whole clock */
static void opus_second_local_init(struct sdp_local *loc)
{
	opus_local_init(loc);
	loc->codec[0].id = SDP_CODEC_PCMU;
	loc->codec[0].pt = 0;
	loc->codec[1].id = SDP_CODEC_OPUS;
	loc->codec[1].pt = 96;
}

#define OPUS_FMTP	"a=fmtp:96 maxplaybackrate=16000;maxaveragebitrate=64000;" \
			"useinbandfec=1\r\n"

static void opus_parse_tests(void)
{
	CASE("opus parse: the rtpmap names the codec at 48000 with two channels");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"));
	CHECK_UINT(m.audio.fmts, 1);
	CHECK(!strcmp(fmt_name(0), "OPUS"));
	CHECK_UINT(m.audio.fmt[0].rate, 48000);
	CHECK(m.audio.opus_fmtp_pt == -1);

	CASE("opus parse: the three keys of a remote fmtp are stored");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:96 maxplaybackrate=8000;maxaveragebitrate=24000;useinbandfec=0\r\n"));
	CHECK(m.audio.opus_fmtp_pt == 96);
	CHECK_UINT(m.audio.opus_fmtp.maxplaybackrate, 8000);
	CHECK_UINT(m.audio.opus_fmtp.maxaveragebitrate, 24000);
	CHECK(m.audio.opus_fmtp.fec_present);
	CHECK(!m.audio.opus_fmtp.fec);

	CASE("opus parse: the RFC 7587 spacing between pairs is accepted");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:96 maxplaybackrate=16000; maxaveragebitrate=20000; useinbandfec=1\r\n"));
	CHECK_UINT(m.audio.opus_fmtp.maxplaybackrate, 16000);
	CHECK_UINT(m.audio.opus_fmtp.maxaveragebitrate, 20000);
	CHECK(m.audio.opus_fmtp.fec);

	/* this stack exposes no DTX, so the key is read and dropped */
	CASE("opus parse: usedtx is ignored beside a key that is kept");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:96 usedtx=1;useinbandfec=1\r\n"));
	CHECK(m.audio.opus_fmtp_pt == 96);
	CHECK(m.audio.opus_fmtp.fec);

	CASE("opus parse: an fmtp of usedtx alone stores no record at all");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:96 usedtx=1\r\n"));
	CHECK(m.audio.opus_fmtp_pt == -1);

	CASE("opus parse: unknown keys change nothing");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:96 stereo=1;cbr=1;sprop-stereo=0;maxplaybackrate=12000\r\n"));
	CHECK_UINT(m.audio.opus_fmtp.maxplaybackrate, 12000);
	CHECK_UINT(m.audio.opus_fmtp.maxaveragebitrate, 0);
	CHECK(!m.audio.opus_fmtp.fec_present);

	/* RFC 7587 6.1: a value outside its key's range should be ignored */
	CASE("opus parse: a bitrate under 6000 and a playback rate under 8000 are dropped");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:96 maxplaybackrate=100;maxaveragebitrate=1000\r\n"));
	CHECK(m.audio.opus_fmtp_pt == 96);
	CHECK_UINT(m.audio.opus_fmtp.maxplaybackrate, 0);
	CHECK_UINT(m.audio.opus_fmtp.maxaveragebitrate, 0);

	CASE("opus parse: a bitrate over 510000 and a playback rate over 48000 are dropped");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:96 maxplaybackrate=96000;maxaveragebitrate=600000\r\n"));
	CHECK_UINT(m.audio.opus_fmtp.maxplaybackrate, 0);
	CHECK_UINT(m.audio.opus_fmtp.maxaveragebitrate, 0);

	CASE("opus parse: a non-numeric value is skipped");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:96 maxaveragebitrate=fast;useinbandfec=1\r\n"));
	CHECK_UINT(m.audio.opus_fmtp.maxaveragebitrate, 0);
	CHECK(m.audio.opus_fmtp.fec);

	CASE("opus parse: the telephone-event fmtp does not displace the record");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 101\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:96 maxaveragebitrate=24000\r\n"
		    "a=rtpmap:101 telephone-event/48000\r\na=fmtp:101 0-15\r\n"));
	CHECK(m.audio.opus_fmtp_pt == 96);
	CHECK_UINT(m.audio.opus_fmtp.maxaveragebitrate, 24000);

	CASE("opus parse: an fmtp hung off another format is dropped whole");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:0 maxaveragebitrate=24000\r\n"));
	CHECK(m.audio.opus_fmtp_pt == -1);
	CHECK_UINT(m.audio.opus_fmtp.maxaveragebitrate, 0);

	CASE("opus parse: an fmtp for a type the m= line never listed cannot displace it");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:96 maxaveragebitrate=24000\r\n"
		    "a=fmtp:97 maxaveragebitrate=32000\r\n"));
	CHECK(m.audio.opus_fmtp_pt == 96);
	CHECK_UINT(m.audio.opus_fmtp.maxaveragebitrate, 24000);

	CASE("opus parse: an fmtp without an rtpmap behind it is dropped");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\n"
		    "a=fmtp:96 maxaveragebitrate=24000\r\n"));
	CHECK(m.audio.opus_fmtp_pt == -1);

	/* RFC 4733 2.1: the event stream carries the clock of the audio it rides */
	CASE("opus parse: telephone-event at 48000 lands in its own field");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 101\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=rtpmap:101 telephone-event/48000\r\n"));
	CHECK(m.audio.dtmf_pt_wb == 101);
	CHECK(m.audio.dtmf_pt == -1);
	CHECK_UINT(m.audio.fmts, 1);

	CASE("opus parse: a peer may name both clocks at two payload types");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 0 101 102\r\n"
		    "a=rtpmap:96 opus/48000/2\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\n"
		    "a=rtpmap:102 telephone-event/48000\r\n"));
	CHECK(m.audio.dtmf_pt == 101);
	CHECK(m.audio.dtmf_pt_wb == 102);

	CASE("opus parse: telephone-event at 16000 is still refused on both fields");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 101\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=rtpmap:101 telephone-event/16000\r\n"));
	CHECK(m.audio.dtmf_pt == -1);
	CHECK(m.audio.dtmf_pt_wb == -1);

	/*
	 * The first claim wins per payload type, the duplicate rule the codec
	 * rtpmap follows below. A body that maps one type at both event clocks
	 * would otherwise seat it as the narrowband event and as the wideband
	 * one, and an answer could then select an event stream the m= line
	 * never described.
	 */
	CASE("opus parse: one payload type cannot claim both event clocks");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 101\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\n"
		    "a=rtpmap:101 telephone-event/48000\r\n"));
	CHECK(m.audio.dtmf_pt == 101);
	CHECK(m.audio.dtmf_pt_wb == -1);

	CASE("opus parse: the same body in the other order keeps the other clock");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 101\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=rtpmap:101 telephone-event/48000\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\n"));
	CHECK(m.audio.dtmf_pt_wb == 101);
	CHECK(m.audio.dtmf_pt == -1);

	CASE("opus parse: a second rtpmap cannot re-map a codec type either");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=rtpmap:96 G722/8000\r\n"));
	CHECK(!strcmp(fmt_name(0), "OPUS"));
	CHECK_UINT(m.audio.fmt[0].rate, 48000);

	/* the claim is taken at the encoding name, so a garbled line blocks nothing */
	CASE("opus parse: an rtpmap this layer could not read claims nothing");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96\r\n"
		    "a=rtpmap:96 opus/48000/2\r\n"));
	CHECK(!strcmp(fmt_name(0), "OPUS"));

	/*
	 * An empty encoding name is the same kind of line. "a=rtpmap:96 /8000"
	 * splits into no name and a rate, so it names no format at all; claiming
	 * the payload type for it would block the real mapping behind it and
	 * fail a negotiation the tolerant contract promises to survive.
	 */
	CASE("opus parse: an empty encoding name claims nothing either");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 /8000\r\n"
		    "a=rtpmap:96 opus/48000/2\r\n"));
	CHECK(!strcmp(fmt_name(0), "OPUS"));
	CHECK_UINT(m.audio.fmt[0].rate, 48000);

	CASE("opus parse: an empty encoding name alone maps nothing");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 /8000\r\n"));
	CHECK_UINT(m.audio.fmts, 1);
	CHECK_UINT(m.audio.fmt[0].pt, 96);
	CHECK(!fmt_name(0)[0]);
	CHECK_UINT(m.audio.fmt[0].rate, 0);

	CASE("opus parse: an empty name cannot displace a mapping already taken");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=rtpmap:96 /8000\r\n"));
	CHECK(!strcmp(fmt_name(0), "OPUS"));
	CHECK_UINT(m.audio.fmt[0].rate, 48000);

	/*
	 * The last fmtp for a payload type wins outright. A later line whose
	 * every value fails to parse still replaces the record, because the
	 * peer restated that format and the record has to say what the peer
	 * last said; keeping the older values would constrain the encoder
	 * against a description the body no longer carries.
	 */
	CASE("opus parse: a later fmtp of garbage resets the record it replaces");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:96 maxaveragebitrate=24000;useinbandfec=1\r\n"
		    "a=fmtp:96 maxaveragebitrate=abc\r\n"));
	CHECK(m.audio.opus_fmtp_pt == 96);
	CHECK_UINT(m.audio.opus_fmtp.maxaveragebitrate, 0);
	CHECK(!m.audio.opus_fmtp.fec_present);

	CASE("opus parse: a later fmtp of real values replaces them the same way");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:96 maxaveragebitrate=24000;useinbandfec=1\r\n"
		    "a=fmtp:96 maxplaybackrate=8000\r\n"));
	CHECK_UINT(m.audio.opus_fmtp.maxplaybackrate, 8000);
	CHECK_UINT(m.audio.opus_fmtp.maxaveragebitrate, 0);
	CHECK(!m.audio.opus_fmtp.fec_present);

	CASE("opus parse: a garbage fmtp for another type still displaces nothing");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 0\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:96 maxaveragebitrate=24000\r\n"
		    "a=fmtp:0 nonsense\r\n"));
	CHECK(m.audio.opus_fmtp_pt == 96);
	CHECK_UINT(m.audio.opus_fmtp.maxaveragebitrate, 24000);

	/* comfort noise has one clock and Opus is not on it */
	CASE("opus parse: CN at 48000 is refused");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 13\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=rtpmap:13 CN/48000\r\n"));
	CHECK(m.audio.cn_pt == -1);
}

static void opus_offer_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;
	const char offer_opus_want[] =
		"v=0\r\n"
		"o=- 42 0 IN IP4 192.0.2.10\r\n"
		"s=-\r\n"
		"c=IN IP4 192.0.2.10\r\n"
		"t=0 0\r\n"
		"m=audio 4000 RTP/AVP 96 0 101 127\r\n"
		"a=rtpmap:96 opus/48000/2\r\n"
		OPUS_FMTP
		"a=rtpmap:0 PCMU/8000\r\n"
		"a=rtpmap:101 telephone-event/48000\r\n"
		"a=fmtp:101 0-15\r\n"
		"a=rtpmap:127 telephone-event/8000\r\n"
		"a=fmtp:127 0-15\r\n"
		"a=ptime:20\r\n"
		"a=maxptime:20\r\n";

	/* the default offer of this layer did not move one byte */
	CASE("opus offer: a local view without Opus writes the default offer byte for byte");
	offer_plain_local_init(&loc);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!strcmp(out, offer_plain_want));
	CHECK(!has("opus"));
	CHECK(!has("48000"));
	CHECK(!has("a=maxptime"));

	CASE("opus offer: the configured offer is byte exact");
	opus_local_init(&loc);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!strcmp(out, offer_opus_want));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_OPUS);
	CHECK_UINT(st.proposed.codec.pt, 96);

	/* RFC 7587 section 7: the rtpmap clock is 48000 and the channels are 2 */
	CASE("opus offer: the rtpmap never names the internal rate or one channel");
	CHECK(!has("opus/16000"));
	CHECK(!has("opus/8000"));
	CHECK(!has("opus/48000/1"));

	/* no dtx key leaves this side, in either fmtp */
	CASE("opus offer: no dtx key is ever written");
	CHECK(!has("usedtx"));
	CHECK(!has("dtx"));

	/*
	 * RFC 7587 section 7 maps ptime and maxptime to SDP attributes and
	 * defines no minptime at all, so the packet mask leaves this side as
	 * a=ptime and a=maxptime and never as an fmtp parameter.
	 */
	CASE("opus offer: the packet mask rides the SDP attributes, not the fmtp");
	CHECK(has("a=maxptime:20\r\n"));
	CHECK(!has("minptime"));
	CHECK(!has("maxptime=20"));

	CASE("opus offer: a narrowband codec first keeps the 8000 event clock");
	opus_second_local_init(&loc);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 0 96 101 127\r\n"));
	CHECK(has("a=rtpmap:0 PCMU/8000\r\n"));
	CHECK(has("a=rtpmap:96 opus/48000/2\r\n"));
	CHECK(has(OPUS_FMTP));
	CHECK(has("a=rtpmap:101 telephone-event/8000\r\n"));
	CHECK(!has("a=maxptime"));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);
	CHECK_UINT(st.proposed.dtmf_pt, 101);

	/*
	 * Both event clocks ride a mixed offer. The configured type stays with
	 * the leading codec's clock and the spare takes the highest free
	 * dynamic type, so a peer that answers either codec has a legal event
	 * stream to select and nothing it selects is a line this offer
	 * withheld.
	 */
	CASE("opus offer: the second event clock rides the same mixed offer");
	CHECK(has("a=rtpmap:127 telephone-event/48000\r\n"));
	CHECK(has("a=fmtp:127 0-15\r\n"));

	CASE("opus offer: the Opus-led list carries the narrowband event line too");
	opus_local_init(&loc);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=rtpmap:101 telephone-event/48000\r\n"));
	CHECK(has("a=rtpmap:127 telephone-event/8000\r\n"));
	CHECK_UINT(st.proposed.dtmf_pt, 101);

	CASE("opus offer: a single-family list still carries exactly one event line");
	opus_local_init(&loc);
	loc.codecs = 1;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 96 101\r\n"));
	CHECK(has("a=rtpmap:101 telephone-event/48000\r\n"));
	CHECK(!has("telephone-event/8000"));
	CHECK(!has("a=rtpmap:127"));

	CASE("opus offer: a leg that offers no telephone-event gets no spare either");
	opus_local_init(&loc);
	loc.dtmf_pt = 0;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 96 0\r\n"));
	CHECK(!has("telephone-event"));

	/* 127 is the top of the RFC 3551 dynamic range, where the spare walk starts */
	CASE("opus offer: the spare skips a codec sitting on the highest type");
	opus_local_init(&loc);
	loc.codec[1].pt = 127;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 96 127 101 126\r\n"));
	CHECK(has("a=rtpmap:127 PCMU/8000\r\n"));
	CHECK(has("a=rtpmap:126 telephone-event/8000\r\n"));

	/* one SSRC carries one clock, so Opus pairs with no CN */
	CASE("opus offer: comfort noise is withheld when Opus leads the list");
	opus_local_init(&loc);
	loc.cn = true;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 96 0 101 127\r\n"));
	CHECK(!has("CN"));
	CHECK(st.proposed.cn_pt == -1);

	CASE("opus offer: the same list led by PCMU keeps comfort noise");
	opus_second_local_init(&loc);
	loc.cn = true;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 0 96 101 127 13\r\n"));
	CHECK(has("a=rtpmap:13 CN/8000\r\n"));
	CHECK(st.proposed.cn_pt == 13);

	/* the Opus packet mask is 0x03, so 30 is not one of its values */
	CASE("opus offer: a configured ptime of 30 is clamped to 20");
	opus_local_init(&loc);
	loc.ptime_ms = 30;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=ptime:20\r\n"));
	CHECK(has("a=maxptime:20\r\n"));
	CHECK_UINT(st.proposed.ptime_ms, 20);

	CASE("opus offer: a configured ptime of 10 stands");
	loc.ptime_ms = 10;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=ptime:10\r\n"));
	CHECK_UINT(st.proposed.ptime_ms, 10);

	CASE("opus offer: the same 30 stands on a list Opus does not lead");
	opus_second_local_init(&loc);
	loc.ptime_ms = 30;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=ptime:30\r\n"));
	CHECK_UINT(st.proposed.ptime_ms, 30);

	CASE("opus offer: a payload type of 129 disables it like every other codec");
	opus_local_init(&loc);
	loc.codec[0].pt = SDP_PT_DISABLED;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 0 101\r\n"));
	CHECK(!has("opus"));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);

	CASE("opus offer: the telephone-event type beats it in a collision");
	opus_local_init(&loc);
	loc.codec[0].pt = SDP_DTMF_PT_DEFAULT;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 0 101\r\n"));
	CHECK(has("a=rtpmap:101 telephone-event/8000\r\n"));
	CHECK(!has("opus"));
	CHECK(owner_is(&loc, 0, "telephone-event"));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);

	/*
	 * The two rules meet. The collision rule says the codec loses a
	 * contested payload type, always, and the pairing rule says Opus pairs
	 * with no comfort noise. An Opus entry on payload type 13 therefore
	 * still loses the type, the offer falls to the narrowband codec behind
	 * it, and comfort noise rides beside that codec, which is a pairing the
	 * second rule allows. No schema value reaches this shape (opus_ptype is
	 * 96 to 127 or 129); the case pins the order the two rules resolve in.
	 */
	CASE("opus offer: an Opus entry on payload type 13 loses it to comfort noise");
	opus_local_init(&loc);
	loc.codec[0].pt = 13;
	loc.cn = true;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 0 101 13\r\n"));
	CHECK(has("a=rtpmap:13 CN/8000\r\n"));
	CHECK(!has("opus"));
	CHECK(owner_is(&loc, 0, "CN"));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);
	CHECK(st.proposed.cn_pt == 13);

	/*
	 * The mirror of that config: the narrowband codec is the one on payload
	 * type 13 and Opus stands behind it. The first resolution drops the
	 * narrowband codec to comfort noise and leaves Opus leading, so comfort
	 * noise is withdrawn, and the second resolution then has type 13 free
	 * for the narrowband codec again. The offer ends with both codecs and no
	 * comfort noise at all.
	 */
	CASE("opus offer: a narrowband codec on payload type 13 takes CN down with it");
	opus_second_local_init(&loc);
	loc.codec[0].pt = 13;
	loc.cn = true;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 13 96 101 127\r\n"));
	CHECK(has("a=rtpmap:13 PCMU/8000\r\n"));
	CHECK(has("a=rtpmap:96 opus/48000/2\r\n"));
	CHECK(!has("CN"));
	CHECK(sdp_offer_pt_owner(&loc, 0) == NULL);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);
	CHECK(st.proposed.cn_pt == -1);
}

static void opus_answer_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;
	const char answer_opus_want[] =
		"v=0\r\n"
		"o=- 55 0 IN IP4 192.0.2.10\r\n"
		"s=-\r\n"
		"c=IN IP4 192.0.2.10\r\n"
		"t=0 0\r\n"
		"m=audio 4000 RTP/AVP 96 101\r\n"
		"a=rtpmap:96 opus/48000/2\r\n"
		OPUS_FMTP
		"a=rtpmap:101 telephone-event/48000\r\n"
		"a=fmtp:101 0-15\r\n"
		"a=ptime:20\r\n"
		"a=maxptime:20\r\n"
		"a=sendrecv\r\n";

	CASE("opus answer: the answer selecting it is byte exact");
	opus_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 101\r\n"
		    "a=rtpmap:96 opus/48000/2\r\n"
		    "a=rtpmap:101 telephone-event/48000\r\na=ptime:20\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!strcmp(out, answer_opus_want));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_OPUS);
	CHECK_UINT(st.proposed.codec.pt, 96);
	CHECK(st.proposed.dtmf_pt == 101);

	CASE("opus answer: the negotiated codec carries the configured encoder ask");
	CHECK_UINT(st.proposed.codec.opus.bitrate, 32000);
	CHECK_UINT(st.proposed.codec.opus.complexity, 5);
	CHECK(st.proposed.codec.opus.fec);

	CASE("opus answer: an offer that lists PCMU first is answered PCMU at 8000");
	opus_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 96 101\r\n"
		    "a=rtpmap:96 opus/48000/2\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 0 101\r\n"));
	CHECK(has("a=rtpmap:101 telephone-event/8000\r\n"));
	CHECK(!has("opus"));
	CHECK(!has("a=maxptime"));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);

	/* the clock table: a rate that is not the codec's names no format of ours */
	CASE("opus answer: an offer that names it at 8000 is not selected");
	opus_local_init(&loc);
	loc.codecs = 1;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/8000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);

	CASE("opus answer: an offer that names it at 16000 is not selected");
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/16000/2\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);

	CASE("opus answer: a narrowband codec at 48000 is not selected either");
	opus_second_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=rtpmap:0 PCMU/48000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);

	CASE("opus answer: an encoding parameter of 1 channel still selects it");
	opus_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/1\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=rtpmap:96 opus/48000/2\r\n"));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_OPUS);

	/*
	 * An Opus stream needs telephone-event/48000, so a peer that offered
	 * the narrowband one alone leaves the answer with no event payload type
	 * and call.c falls the digits back.
	 */
	CASE("opus answer: a narrowband telephone-event is refused beside it");
	opus_local_init(&loc);
	loc.codecs = 1;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 101\r\n"
		    "a=rtpmap:96 opus/48000/2\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 96\r\n"));
	CHECK(!has("telephone-event"));
	CHECK(st.proposed.dtmf_pt == -1);

	CASE("opus answer: the wideband event type of a peer that named both is taken");
	opus_local_init(&loc);
	loc.codecs = 1;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 101 102\r\n"
		    "a=rtpmap:96 opus/48000/2\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\n"
		    "a=rtpmap:102 telephone-event/48000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 96 102\r\n"));
	CHECK(has("a=rtpmap:102 telephone-event/48000\r\n"));
	CHECK(st.proposed.dtmf_pt == 102);

	CASE("opus answer: comfort noise the peer offered is not seated beside it");
	opus_local_init(&loc);
	loc.codecs = 1;
	loc.cn = true;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 13\r\n"
		    "a=rtpmap:96 opus/48000/2\r\na=rtpmap:13 CN/8000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 96\r\n"));
	CHECK(!has("CN"));
	CHECK(st.proposed.cn_pt == -1);

	CASE("opus answer: the same offer answered PCMU keeps its comfort noise");
	opus_second_local_init(&loc);
	loc.cn = true;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 96 13\r\n"
		    "a=rtpmap:96 opus/48000/2\r\na=rtpmap:13 CN/8000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 0 13\r\n"));
	CHECK(has("a=rtpmap:13 CN/8000\r\n"));
	CHECK(st.proposed.cn_pt == 13);

	CASE("opus answer: the remote fmtp reaches the result as its own record");
	opus_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:96 maxplaybackrate=8000;maxaveragebitrate=20000;useinbandfec=0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK_UINT(st.proposed.opus_remote.maxplaybackrate, 8000);
	CHECK_UINT(st.proposed.opus_remote.maxaveragebitrate, 20000);
	CHECK(st.proposed.opus_remote.fec_present);
	CHECK(!st.proposed.opus_remote.fec);

	CASE("opus answer: our own fmtp is unchanged by theirs");
	CHECK(has(OPUS_FMTP));
	CHECK_UINT(st.proposed.codec.opus.bitrate, 32000);

	CASE("opus answer: an exchange on another codec carries no remote record");
	opus_second_local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:96 maxaveragebitrate=20000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);
	CHECK_UINT(st.proposed.opus_remote.maxaveragebitrate, 0);

	/*
	 * The narrowband line. A line whose local list names no Opus answers an
	 * inbound Opus offer on the narrowband codec beside it, which is what
	 * an unconfigured line and a backend without the codec both produce.
	 * The remote fmtp still parses and is still pinned; it simply reaches
	 * no exchange.
	 */
	CASE("opus answer: a local view without Opus answers the offer on PCMU");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 0 101\r\n"
		    "a=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:96 maxplaybackrate=16000;maxaveragebitrate=20000;useinbandfec=1\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\n"));
	CHECK(m.audio.opus_fmtp_pt == 96);
	CHECK_UINT(m.audio.opus_fmtp.maxaveragebitrate, 20000);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 0 101\r\n"));
	CHECK(has("a=rtpmap:101 telephone-event/8000\r\n"));
	CHECK(!has("opus"));
	CHECK(!has("a=maxptime"));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);
	CHECK_UINT(st.proposed.opus_remote.maxaveragebitrate, 0);

	CASE("opus answer: an offer of Opus alone is refused by such a view");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:96 useinbandfec=1\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);

	/*
	 * The tolerant contract, end to end: a body whose first rtpmap for the
	 * type names no encoding still negotiates on the valid one behind it.
	 */
	CASE("opus answer: an empty-name rtpmap does not fail the negotiation");
	opus_local_init(&loc);
	loc.codecs = 1;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 /8000\r\n"
		    "a=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:96 maxaveragebitrate=24000\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 96\r\n"));
	CHECK(has("a=rtpmap:96 opus/48000/2\r\n"));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_OPUS);
	CHECK_UINT(st.proposed.opus_remote.maxaveragebitrate, 24000);

	CASE("opus answer: an offered ptime of 30 is answered 20");
	opus_local_init(&loc);
	loc.codecs = 1;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=ptime:30\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=ptime:20\r\n"));
	CHECK_UINT(st.proposed.ptime_ms, 20);

	CASE("opus answer: an offered ptime of 10 is answered 10");
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=ptime:10\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=ptime:10\r\n"));
	CHECK_UINT(st.proposed.ptime_ms, 10);

	CASE("opus answer: a dynamic payload type the peer numbered is mirrored");
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 111\r\na=rtpmap:111 opus/48000/2\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 111\r\n"));
	CHECK(has("a=rtpmap:111 opus/48000/2\r\n"));
	CHECK(has("a=fmtp:111 maxplaybackrate=16000;"));
	CHECK_UINT(st.proposed.codec.pt, 111);
}

static void opus_take_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;

	CASE("opus take: the answer to our offer takes it back with its fmtp");
	opus_local_init(&loc);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 101\r\n"
		    "a=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:96 maxplaybackrate=12000;maxaveragebitrate=16000;useinbandfec=0\r\n"
		    "a=rtpmap:101 telephone-event/48000\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_OPUS);
	CHECK_UINT(st.proposed.codec.pt, 96);
	CHECK(st.proposed.dtmf_pt == 101);
	CHECK_UINT(st.proposed.opus_remote.maxplaybackrate, 12000);
	CHECK_UINT(st.proposed.opus_remote.maxaveragebitrate, 16000);
	CHECK(st.proposed.opus_remote.fec_present);
	CHECK(!st.proposed.opus_remote.fec);

	CASE("opus take: an answer that refuses it falls back to PCMU");
	opus_local_init(&loc);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 101\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);
	CHECK(st.proposed.dtmf_pt == 101);
	CHECK_UINT(st.proposed.opus_remote.maxaveragebitrate, 0);
	CHECK(st.proposed.valid);

	CASE("opus take: an answer that names it at 8000 refuses the exchange");
	opus_local_init(&loc);
	loc.codecs = 1;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/8000/2\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);

	CASE("opus take: an answer with only the narrowband event type loses the relay");
	opus_local_init(&loc);
	loc.codecs = 1;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 101\r\n"
		    "a=rtpmap:96 opus/48000/2\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_OPUS);
	CHECK(st.proposed.dtmf_pt == -1);

	CASE("opus take: comfort noise an answer named is not seated beside it");
	opus_local_init(&loc);
	loc.codecs = 1;
	loc.cn = true;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 13\r\n"
		    "a=rtpmap:96 opus/48000/2\r\na=rtpmap:13 CN/8000\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(st.proposed.cn_pt == -1);

	/*
	 * Nothing the offer did not carry is seated. An Opus-led offer
	 * withholds comfort noise and carries both event clocks, so a peer that
	 * answers the narrowband codec and names both gets its event stream
	 * accepted and its comfort noise refused.
	 */
	CASE("opus take: a PCMU answer takes the narrowband event line the offer carried");
	opus_local_init(&loc);
	loc.cn = true;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 96 0 101 127\r\n"));
	CHECK(has("a=rtpmap:127 telephone-event/8000\r\n"));
	CHECK(!has("CN"));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 127 13\r\n"
		    "a=rtpmap:127 telephone-event/8000\r\n"
		    "a=rtpmap:13 CN/8000\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);
	CHECK(st.proposed.dtmf_pt == 127);

	CASE("opus take: and the comfort noise the withdrawal pass withheld is refused");
	CHECK(st.proposed.cn_pt == -1);

	/*
	 * The mirror: the same answer against an offer that really carried the
	 * CN line, which is the narrowband-led list. Nothing about comfort noise
	 * moved for a leg whose offer never withdrew it.
	 */
	CASE("opus take: a narrowband-led offer seats the comfort noise it did carry");
	opus_second_local_init(&loc);
	loc.cn = true;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=rtpmap:13 CN/8000\r\n"));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 101 13\r\n"
		    "a=rtpmap:101 telephone-event/8000\r\n"
		    "a=rtpmap:13 CN/8000\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(st.proposed.cn_pt == 13);
	CHECK(st.proposed.dtmf_pt == 101);

	CASE("opus take: an Opus answer to that same offer still refuses the CN");
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 127 13\r\n"
		    "a=rtpmap:96 opus/48000/2\r\n"
		    "a=rtpmap:127 telephone-event/48000\r\n"
		    "a=rtpmap:13 CN/8000\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_OPUS);
	CHECK(st.proposed.cn_pt == -1);
	CHECK(st.proposed.dtmf_pt == 127);

	/*
	 * An event stream at the clock the selected codec does not run on is
	 * refused whichever way round it comes (RFC 4733 section 2.1).
	 */
	CASE("opus take: a PCMU answer naming the wideband event alone loses the relay");
	opus_second_local_init(&loc);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 127\r\n"
		    "a=rtpmap:127 telephone-event/48000\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);
	CHECK(st.proposed.dtmf_pt == -1);

	/*
	 * The record is the offer's, not the local view's. call.c rewrites
	 * sdp_local under a live leg (loc_g711_clamp on a fax upspeed) and a
	 * reload replaces the codec sections, so a take that recomputed what
	 * had been advertised could seat a stream element the wire never
	 * carried. The offer below advertises one event clock, the view then
	 * grows a second codec family, and the answer that selects it still
	 * gets no event type.
	 */
	CASE("opus take: a local view that moved cannot seat an unoffered event type");
	opus_local_init(&loc);
	loc.codecs = 1;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 96 101\r\n"));
	CHECK(!has("telephone-event/8000"));
	loc.codecs = 2;
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 127\r\n"
		    "a=rtpmap:127 telephone-event/8000\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMU);
	CHECK(st.proposed.dtmf_pt == -1);

	CASE("opus take: the same answer against an offer that advertised it is taken");
	opus_local_init(&loc);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=rtpmap:127 telephone-event/8000\r\n"));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 127\r\n"
		    "a=rtpmap:127 telephone-event/8000\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(st.proposed.dtmf_pt == 127);

	CASE("opus take: a local view that moved cannot seat an unoffered CN either");
	opus_second_local_init(&loc);
	loc.cn = false;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!has("CN"));
	loc.cn = true;
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0 13\r\na=rtpmap:13 CN/8000\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(st.proposed.cn_pt == -1);

	/*
	 * The record never outlives the offer that made it. A T.38 description
	 * carries no payload type of any kind, so the image offer clears every
	 * audio element the audio offer before it recorded.
	 */
	CASE("opus take: an audio offer records the elements it wrote");
	opus_second_local_init(&loc);
	loc.t38 = true;
	loc.cn = true;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=rtpmap:101 telephone-event/8000\r\n"));
	CHECK(has("a=rtpmap:127 telephone-event/48000\r\n"));
	CHECK(has("a=rtpmap:13 CN/8000\r\n"));
	CHECK(st.offered.dtmf_pt == 101);
	CHECK_UINT(st.offered.clock, 8000);
	CHECK(st.offered.alt_pt == 127);
	CHECK_UINT(st.offered.alt_clock, 48000);
	CHECK(st.offered.cn_pt == 13);

	CASE("opus take: a T.38 offer behind it clears every one of them");
	sdp_t38_set(&st, true);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=image 4000 udptl t38\r\n"));
	CHECK(st.offered.dtmf_pt == -1);
	CHECK(st.offered.alt_pt == -1);
	CHECK(st.offered.cn_pt == -1);
	CHECK_UINT(st.offered.clock, 0);
	CHECK_UINT(st.offered.alt_clock, 0);
	sdp_t38_set(&st, false);

	CASE("opus take: a single-family offer records one clock and no spare");
	opus_local_init(&loc);
	loc.codecs = 1;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(st.offered.dtmf_pt == 101);
	CHECK_UINT(st.offered.clock, 48000);
	CHECK(st.offered.alt_pt == -1);
	CHECK_UINT(st.offered.alt_clock, 0);

	CASE("opus take: a leg that offered no event line seats none whatever the answer says");
	opus_local_init(&loc);
	loc.dtmf_pt = 0;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!has("telephone-event"));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96 101\r\n"
		    "a=rtpmap:96 opus/48000/2\r\n"
		    "a=rtpmap:101 telephone-event/48000\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(st.proposed.dtmf_pt == -1);

	/*
	 * A live re-INVITE that moves the remote fmtp. The udsp side recreates
	 * the codec pair on a parameter change, so the second exchange has to
	 * carry the new constraint set into the result; a stale record would
	 * leave the encoder on the first exchange's terms with nothing on the
	 * wire to say so.
	 */
	CASE("opus take: a re-INVITE carries the peer's NEW constraints");
	opus_local_init(&loc);
	loc.codecs = 1;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:96 maxplaybackrate=16000;maxaveragebitrate=48000;useinbandfec=1\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	sdp_commit(&st);
	CHECK_UINT(st.committed.opus_remote.maxaveragebitrate, 48000);
	CHECK_UINT(st.committed.opus_remote.maxplaybackrate, 16000);
	CHECK(st.committed.opus_remote.fec);

	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=fmtp:96 maxplaybackrate=8000;maxaveragebitrate=16000;useinbandfec=0\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.opus_remote.maxaveragebitrate, 16000);
	CHECK_UINT(st.proposed.opus_remote.maxplaybackrate, 8000);
	CHECK(st.proposed.opus_remote.fec_present);
	CHECK(!st.proposed.opus_remote.fec);

	CASE("opus take: the committed record moves only when the exchange commits");
	CHECK_UINT(st.committed.opus_remote.maxaveragebitrate, 48000);
	sdp_commit(&st);
	CHECK_UINT(st.committed.opus_remote.maxaveragebitrate, 16000);

	CASE("opus take: a re-INVITE the peer answers with no fmtp clears the record");
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.opus_remote.maxaveragebitrate, 0);
	CHECK(!st.proposed.opus_remote.fec_present);

	CASE("opus take: a rolled back re-INVITE restores the committed constraints");
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	sdp_rollback(&st);
	CHECK_UINT(st.proposed.opus_remote.maxaveragebitrate, 16000);

	CASE("opus take: an answered ptime of 30 is held at 20");
	opus_local_init(&loc);
	loc.codecs = 1;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n"
		    "a=ptime:30\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.ptime_ms, 20);
}

/*
 * The offerability gate. call.c runs it on every entry that it adds to the
 * codec list. A local list therefore never carries a codec that the offer
 * builder skips.
 */
static void opus_offerable_tests(void)
{
	struct sdp_codec c;

	memset(&c, 0, sizeof(c));

	CASE("offerable: a NULL entry and the none codec are refused");
	CHECK(!sdp_codec_offerable(NULL));
	CHECK(!sdp_codec_offerable(&c));

	CASE("offerable: every codec of the vocabulary passes at a live type");
	for (c.id = SDP_CODEC_PCMU; c.id <= SDP_CODEC_G726_32; c.id++) {
		c.pt = 96;
		CHECK(sdp_codec_offerable(&c));
	}

	CASE("offerable: an id past the vocabulary is refused");
	c.id = SDP_CODEC_G726_32 + 1;
	c.pt = 96;
	CHECK(!sdp_codec_offerable(&c));

	CASE("offerable: the disabled payload type and every type past 127 are refused");
	c.id = SDP_CODEC_OPUS;
	c.pt = SDP_PT_DISABLED;
	CHECK(!sdp_codec_offerable(&c));
	c.pt = 128;
	CHECK(!sdp_codec_offerable(&c));
	c.pt = 127;
	CHECK(sdp_codec_offerable(&c));
	c.pt = 0;
	CHECK(sdp_codec_offerable(&c));
}

static void opus_tests(void)
{
	opus_parse_tests();
	opus_offer_tests();
	opus_answer_tests();
	opus_take_tests();
	opus_offerable_tests();
}

/*
 * The payload type collision rule of an offer. Two rtpmap lines under one
 * payload type name two formats for one type, so the later codec is dropped.
 * Telephone-event and comfort noise are not codecs, and keep their types. The
 * rule is offer side only: an answer names the type that the peer numbered,
 * so nothing there can collide.
 */
static void pt_clash_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;

	CASE("clash: the default codec list claims no type twice");
	offer_plain_local_init(&loc);
	CHECK(sdp_offer_pt_owner(&loc, 0) == NULL);
	CHECK(sdp_offer_pt_owner(&loc, 1) == NULL);

	CASE("clash: the later of two codecs on one payload type is dropped");
	local_init(&loc);
	loc.codec[0].id = SDP_CODEC_G722;
	loc.codec[0].pt = 9;
	loc.codec[1].id = SDP_CODEC_PCMA;
	loc.codec[1].pt = 9;
	loc.codecs = 2;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 9 101\r\n"));
	CHECK(has("a=rtpmap:9 G722/8000\r\n"));
	CHECK(!has("PCMA"));
	CHECK(sdp_offer_pt_owner(&loc, 0) == NULL);
	CHECK(owner_is(&loc, 1, "G722"));

	CASE("clash: telephone-event keeps its type and the codec loses it");
	local_init(&loc);
	CHECK_UINT(loc.dtmf_pt, 101);
	loc.codec[1].pt = 101;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 0 101\r\n"));
	CHECK(has("a=rtpmap:101 telephone-event/8000\r\n"));
	CHECK(!has("PCMA"));
	CHECK(owner_is(&loc, 1, "telephone-event"));

	CASE("clash: comfort noise keeps payload type 13 and the codec loses it");
	local_init(&loc);
	loc.cn = true;
	loc.codec[1].pt = 13;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 0 101 13\r\n"));
	CHECK(has("a=rtpmap:13 CN/8000\r\n"));
	CHECK(!has("PCMA"));
	CHECK(owner_is(&loc, 1, "CN"));

	CASE("clash: payload type 13 is free again when CN is off");
	loc.cn = false;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 0 13 101\r\n"));
	CHECK(has("a=rtpmap:13 PCMA/8000\r\n"));
	CHECK(sdp_offer_pt_owner(&loc, 1) == NULL);

	CASE("clash: a dropped first codec does not become the proposal");
	local_init(&loc);
	loc.codec[0].pt = 101;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 8 101\r\n"));
	CHECK(!has("PCMU"));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_PCMA);
	CHECK_UINT(st.proposed.codec.pt, 8);

	CASE("clash: an offer whose every codec is dropped fails");
	local_init(&loc);
	loc.codec[0].pt = 101;
	loc.codec[1].pt = 101;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(!sdp_offer_build(&st, &loc, &b));
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!st.offer_pending);

	CASE("clash: a disabled codec claims no payload type");
	local_init(&loc);
	loc.codec[0].pt = SDP_PT_DISABLED;
	loc.codec[1].id = SDP_CODEC_G722;
	loc.codec[1].pt = 9;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 9 101\r\n"));
	CHECK(sdp_offer_pt_owner(&loc, 0) == NULL);
	CHECK(sdp_offer_pt_owner(&loc, 1) == NULL);

	CASE("clash: the third codec loses to the first that claimed the type");
	local_init(&loc);
	loc.codec[0].id = SDP_CODEC_PCMU;
	loc.codec[0].pt = 9;
	loc.codec[1].id = SDP_CODEC_PCMA;
	loc.codec[1].pt = 9;
	loc.codec[2].id = SDP_CODEC_G722;
	loc.codec[2].pt = 9;
	loc.codecs = 3;
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 9 101\r\n"));
	CHECK(has("a=rtpmap:9 PCMU/8000\r\n"));
	CHECK(owner_is(&loc, 1, "PCMU"));
	CHECK(owner_is(&loc, 2, "PCMU"));

	/*
	 * A dropped codec is off the line. The three cases below walk the two
	 * selection paths and the ambiguity below: the answer build refuses it,
	 * the answer take refuses it, and the codec that kept the contested
	 * type still negotiates.
	 */
	CASE("clash: the answer refuses a codec the offer would have dropped");
	local_init(&loc);
	loc.codec[0].id = SDP_CODEC_G722;
	loc.codec[0].pt = 9;
	loc.codec[1].id = SDP_CODEC_PCMA;
	loc.codec[1].pt = 9;
	loc.codecs = 2;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 8\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!st.proposed.valid);

	CASE("clash: the codec that kept the contested type still answers");
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 9\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("m=audio 4000 RTP/AVP 9\r\n"));
	CHECK(has("a=rtpmap:9 G722/8000\r\n"));
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_G722);

	/*
	 * The ambiguity. Our offer gave payload type 9 to G.722 and withheld
	 * PCMA. A peer that answers PCMA on 9 describes a stream whose type
	 * each end would decode as a different codec, so the exchange is
	 * refused instead of programmed.
	 */
	CASE("clash: an answer that names a dropped codec on the contested type is refused");
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=audio 4000 RTP/AVP 9 101\r\n"));
	CHECK(has("a=rtpmap:9 G722/8000\r\n"));
	CHECK(!has("PCMA"));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 9\r\na=rtpmap:9 PCMA/8000\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);

	CASE("clash: the same answer under the codec we did offer is taken");
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 9\r\na=rtpmap:9 G722/8000\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK_UINT(st.proposed.codec.id, SDP_CODEC_G722);
	CHECK_UINT(st.proposed.codec.pt, 9);

	CASE("clash: a codec dropped by the telephone-event type is refused in the answer");
	local_init(&loc);
	loc.codec[1].pt = 101;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 8\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);

	CASE("clash: the owner predicate refuses a NULL view and an index past the list");
	local_init(&loc);
	CHECK(sdp_offer_pt_owner(NULL, 0) == NULL);
	CHECK(sdp_offer_pt_owner(&loc, loc.codecs) == NULL);
	CHECK(sdp_offer_pt_owner(&loc, SDP_CODEC_MAX) == NULL);
}

/*
 * The RFC 3605 a=rtcp record: what the parse keeps, where a session level
 * attribute reaches, and what a result carries. call.c programs the RTCP
 * socket from the port. A section that names none therefore leaves the media
 * port plus one (RFC 3550 section 11), and the record stays zero.
 */
static void rtcp_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;

	CASE("rtcp: a media level a=rtcp port is recorded");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=rtcp:5005\r\n"));
	CHECK_UINT(m.audio.rtcp_port, 5005);
	CHECK_UINT(m.audio.rtcp_addr[0], 0);
	CHECK_UINT(m.audio.rtcp_ipver, 0);

	CASE("rtcp: a session level a=rtcp reaches the audio section");
	CHECK(parse("v=0\r\no=- 7 9 IN IP4 198.51.100.7\r\ns=-\r\n"
		    "c=IN IP4 198.51.100.7\r\nt=0 0\r\na=rtcp:5011\r\n"
		    "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK_UINT(m.audio.rtcp_port, 5011);

	CASE("rtcp: a session level a=rtcp does not reach the image section");
	CHECK(parse("v=0\r\no=- 7 9 IN IP4 198.51.100.7\r\ns=-\r\n"
		    "c=IN IP4 198.51.100.7\r\nt=0 0\r\na=rtcp:5011\r\n"
		    "m=audio 5004 RTP/AVP 0\r\nm=image 5006 udptl t38\r\n"));
	CHECK_UINT(m.audio.rtcp_port, 5011);
	CHECK_UINT(m.image.rtcp_port, 0);

	CASE("rtcp: a media level a=rtcp wins over the session level one");
	CHECK(parse("v=0\r\no=- 7 9 IN IP4 198.51.100.7\r\ns=-\r\n"
		    "c=IN IP4 198.51.100.7\r\nt=0 0\r\na=rtcp:5011\r\n"
		    "m=audio 5004 RTP/AVP 0\r\na=rtcp:5005\r\n"));
	CHECK_UINT(m.audio.rtcp_port, 5005);

	CASE("rtcp: a=rtcp with IN IP4 and an address records the address");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"
		    "a=rtcp:5005 IN IP4 203.0.113.9\r\n"));
	CHECK_UINT(m.audio.rtcp_port, 5005);
	CHECK(!strcmp(m.audio.rtcp_addr, "203.0.113.9"));
	CHECK_UINT(m.audio.rtcp_ipver, 4);

	CASE("rtcp: a=rtcp with IN IP6 and an address records ipver 6");
	CHECK(parse(HEAD6 "m=audio 5004 RTP/AVP 0\r\n"
		    "a=rtcp:5005 IN IP6 2001:db8::9\r\n"));
	CHECK_UINT(m.audio.rtcp_port, 5005);
	CHECK(!strcmp(m.audio.rtcp_addr, "2001:db8::9"));
	CHECK_UINT(m.audio.rtcp_ipver, 6);

	CASE("rtcp: an unreadable a=rtcp port leaves the record absent");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=rtcp:nope\r\n"));
	CHECK_UINT(m.audio.rtcp_port, 0);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=rtcp:0\r\n"));
	CHECK_UINT(m.audio.rtcp_port, 0);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=rtcp:70000\r\n"));
	CHECK_UINT(m.audio.rtcp_port, 0);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=rtcp:\r\n"));
	CHECK_UINT(m.audio.rtcp_port, 0);

	CASE("rtcp: an a=rtcp address that is no literal keeps the port and "
	     "drops the address");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"
		    "a=rtcp:5005 IN IP4 rtcp.example.com\r\n"));
	CHECK_UINT(m.audio.rtcp_port, 5005);
	CHECK_UINT(m.audio.rtcp_addr[0], 0);
	CHECK_UINT(m.audio.rtcp_ipver, 0);

	/* the address type the attribute names is what the literal must be */
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"
		    "a=rtcp:5005 IN IP4 2001:db8::9\r\n"));
	CHECK_UINT(m.audio.rtcp_port, 5005);
	CHECK_UINT(m.audio.rtcp_addr[0], 0);
	CHECK_UINT(m.audio.rtcp_ipver, 0);

	/*
	 * An address type this layer does not know must not read as IP4. The
	 * record feeds a family comparison, so the guess would raise a conflict
	 * against an IP6 section that named a good address under a type neither
	 * end understands.
	 */
	CASE("rtcp: an unknown addrtype keeps the port and drops the address");
	CHECK(parse(HEAD6 "m=audio 5004 RTP/AVP 0\r\n"
		    "a=rtcp:5005 IN IP7 203.0.113.9\r\n"));
	CHECK_UINT(m.audio.rtcp_port, 5005);
	CHECK_UINT(m.audio.rtcp_addr[0], 0);
	CHECK_UINT(m.audio.rtcp_ipver, 0);

	/* so the section is answered, and no family conflict is raised */
	local_init(&loc);
	snprintf(loc.addr, sizeof(loc.addr), "%s", LOCAL6);
	sdp_state_init(&st, 55);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK_UINT(st.proposed.rtcp_port, 5005);
	CHECK_UINT(st.proposed.rtcp_ipver, 0);
	CHECK(sdp_offer_local_ok(&m, LOCAL6));

	CASE("rtcp: the answer result carries the peer's rtcp port");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"
		    "a=rtcp:5005 IN IP4 203.0.113.9\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK_UINT(st.proposed.rtcp_port, 5005);
	CHECK(!strcmp(st.proposed.rtcp_addr, "203.0.113.9"));
	CHECK_UINT(st.proposed.rtcp_ipver, 4);

	/* this side writes none of its own: our RTCP is the port plus one */
	CHECK(!has("a=rtcp:"));

	CASE("rtcp: a T.38 result carries no rtcp port");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 0\r\n"
		    "m=image 5006 udptl t38\r\na=rtcp:5007\r\n"));
	CHECK_UINT(m.image.rtcp_port, 5007);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(st.proposed.t38);
	CHECK_UINT(st.proposed.rtcp_port, 0);
	CHECK_UINT(st.proposed.rtcp_addr[0], 0);
	CHECK_UINT(st.proposed.rtcp_ipver, 0);

	CASE("rtcp: an a=rtcp address in another family than the "
	     "c= line is SDP_NO_CODEC");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"
		    "a=rtcp:5005 IN IP6 2001:db8::9\r\n"));
	CHECK_UINT(m.audio.rtcp_ipver, 6);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!st.proposed.valid);

	/* and the same line in an answer to an offer of ours */
	local_init(&loc);
	take_state_init(&st, &loc);
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);
	CHECK(st.offer_pending);
}

/*
 * RFC 6157 section 5: one stream runs in one address family, and the media
 * follows the family that the signaling already chose. Every refusal below is
 * the sdp-488 class, so a mixed family description is answered 488 and never
 * programmed. The IPv6 unspecified address is the hold address of an IP6
 * session, which RFC 4566 leaves unnamed.
 */
static void family_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;

	CASE("family: an IP4 offer against an IP6 local address is "
	     "SDP_NO_CODEC");
	local_init(&loc);
	snprintf(loc.addr, sizeof(loc.addr), "%s", LOCAL6);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!st.proposed.valid);
	CHECK_UINT(st.sess_ver, 0);

	CASE("family: an IP6 offer against an IP4 local address is "
	     "SDP_NO_CODEC");
	local_init(&loc);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD6 "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!st.proposed.valid);

	CASE("family: an IP6 offer against an IP6 local address is answered");
	local_init(&loc);
	snprintf(loc.addr, sizeof(loc.addr), "%s", LOCAL6);
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD6 "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("c=IN IP6 2001:db8::10\r\n"));
	CHECK(has("m=audio 4000 RTP/AVP 0\r\n"));
	CHECK(!strcmp(st.proposed.remote_addr, "2001:db8::1"));
	CHECK_UINT(st.proposed.remote_ipver, 6);
	CHECK(st.proposed.valid);

	CASE("family: local_ok refuses a mixed family offer before the ring");
	CHECK(parse(HEAD6 "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_offer_usable(&m));
	CHECK(!sdp_offer_local_ok(&m, "192.0.2.10"));
	CHECK(sdp_offer_local_ok(&m, LOCAL6));
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(!sdp_offer_local_ok(&m, LOCAL6));
	CHECK(sdp_offer_local_ok(&m, "192.0.2.10"));
	CHECK(!sdp_offer_local_ok(NULL, "192.0.2.10"));

	CASE("family: local_ok with no local address keeps the literal "
	     "test alone");
	CHECK(parse(HEAD6 "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_offer_local_ok(&m, NULL));
	CHECK(sdp_offer_local_ok(&m, ""));
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 sip.example.com\r\n"
		    "c=IN IP4 sip.example.com\r\nt=0 0\r\n"
		    "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_offer_local_ok(&m, ""));
	CHECK(!sdp_offer_usable(&m));

	CASE("family: local_ok refuses an a=rtcp address in the other "
	     "family before the ring");
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"
		    "a=rtcp:5005 IN IP6 2001:db8::9\r\n"));
	CHECK(sdp_offer_usable(&m));
	CHECK(!sdp_offer_local_ok(&m, "192.0.2.10"));

	/* the test reads the section against itself, so it stands without an
	 * address of ours */
	CHECK(!sdp_offer_local_ok(&m, ""));
	CHECK(!sdp_offer_local_ok(&m, NULL));

	/* the same offer with the attribute in its own family passes */
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"
		    "a=rtcp:5005 IN IP4 203.0.113.9\r\n"));
	CHECK(sdp_offer_local_ok(&m, "192.0.2.10"));
	CHECK(sdp_offer_local_ok(&m, ""));

	CASE("family: an IP4 answer to an IP6 offer is SDP_NO_CODEC");
	local_init(&loc);
	snprintf(loc.addr, sizeof(loc.addr), "%s", LOCAL6);
	take_state_init(&st, &loc);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);
	CHECK(st.offer_pending);

	/*
	 * A leg that holds no literal of its own names no family, so the local
	 * half of the rule waives and the answer, the take and the pre-ring
	 * check all read what they read before the rule existed.
	 */
	CASE("family: a local address that is no literal keeps the "
	     "verdicts of before");
	local_init(&loc);
	loc.addr[0] = 0;
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!strcmp(st.proposed.remote_addr, "198.51.100.7"));
	CHECK(sdp_offer_local_ok(&m, loc.addr));
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD6 "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!strcmp(st.proposed.remote_addr, "2001:db8::1"));
	CHECK_UINT(st.proposed.remote_ipver, 6);
	CHECK(sdp_offer_local_ok(&m, loc.addr));
	take_state_init(&st, &loc);
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(st.proposed.valid);

	/* an FQDN names no family either, so it reads the same way */
	local_init(&loc);
	snprintf(loc.addr, sizeof(loc.addr), "%s", "sip.example.com");
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!strcmp(st.proposed.remote_addr, "198.51.100.7"));
	CHECK(sdp_offer_local_ok(&m, loc.addr));
	sdp_state_init(&st, 55);
	CHECK(parse(HEAD6 "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!strcmp(st.proposed.remote_addr, "2001:db8::1"));
	CHECK(sdp_offer_local_ok(&m, loc.addr));

	/* and the waiver reaches the image gate of both T.38 sites */
	t38_local_init(&loc);
	loc.addr[0] = 0;
	t38_answer_state_init(&st);
	CHECK(parse(HEAD6 "m=audio 0 RTP/AVP 0\r\nm=image 5006 udptl t38\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(st.proposed.t38);
	t38_take_state_init(&st, &loc);
	loc.addr[0] = 0;
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(st.proposed.t38);

	t38_local_init(&loc);
	snprintf(loc.addr, sizeof(loc.addr), "%s", "sip.example.com");
	t38_answer_state_init(&st);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(st.proposed.t38);
	t38_take_state_init(&st, &loc);
	snprintf(loc.addr, sizeof(loc.addr), "%s", "sip.example.com");
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
	CHECK(st.proposed.t38);

	/*
	 * An image stream in the other family is a session this side cannot
	 * run, so it answers the 488 class even when the offer names no audio
	 * section beside it. A body that names no usable stream at all keeps
	 * SDP_BAD.
	 */
	CASE("family: an IP6 image-only offer against an IP4 local "
	     "address is SDP_NO_CODEC");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD6 "m=image 5006 udptl t38\r\n"));
	CHECK(!m.audio.present);
	CHECK_UINT(m.image.ipver, 6);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK_UINT(sip_buf_len(&b), 0);
	CHECK(!st.proposed.valid);

	/* the same offer in our own family is the T.38 upgrade it names */
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(st.proposed.t38);

	/* and an offer with no usable stream at all is still SDP_BAD */
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=video 5006 RTP/AVP 96\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_BAD);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=image 5006 udptl 96\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_BAD);

	CASE("family: an IP4 image-only offer against an IP6 local "
	     "address is SDP_NO_CODEC");
	t38_local_init(&loc);
	snprintf(loc.addr, sizeof(loc.addr), "%s", LOCAL6);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=image 5006 udptl t38\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);

	/* the take side of the same exchange was already the 488 class */
	t38_take_state_init(&st, &loc);
	snprintf(loc.addr, sizeof(loc.addr), "%s", LOCAL6);
	CHECK(sdp_answer_take(&st, &loc, &m) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);

	CASE("family: a T.38 image section in the other family is refused");
	t38_local_init(&loc);
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 0\r\n"
		    "m=image 5006 udptl t38\r\nc=IN IP6 2001:db8::9\r\n"));
	CHECK_UINT(m.image.ipver, 6);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_NO_CODEC);
	CHECK(!st.proposed.valid);

	/* the same body with the image in our own family takes the upgrade */
	t38_answer_state_init(&st);
	CHECK(parse(HEAD "m=audio 0 RTP/AVP 0\r\n"
		    "m=image 5006 udptl t38\r\nc=IN IP4 203.0.113.9\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(st.proposed.t38);

	CASE("hold: c=IN IP6 :: is the zero hold address");
	CHECK(parse("v=0\r\no=- 1 1 IN IP6 ::\r\nc=IN IP6 ::\r\nt=0 0\r\n"
		    "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(m.addr_zero);
	CHECK(!m.audio.dir_present);
	local_init(&loc);
	snprintf(loc.addr, sizeof(loc.addr), "%s", LOCAL6);
	sdp_state_init(&st, 55);
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(has("a=recvonly\r\n"));
	CHECK(st.proposed.remote_hold);

	CASE("hold: c=IN IP6 ::0 is the hold address too");
	CHECK(parse("v=0\r\no=- 1 1 IN IP6 ::0\r\nc=IN IP6 ::0\r\nt=0 0\r\n"
		    "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(m.addr_zero);

	/* every other IPv6 address stays an address, hold or not */
	CHECK(parse(HEAD6 "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(!m.addr_zero);
	CHECK(parse("v=0\r\no=- 1 1 IN IP6 ::1\r\nc=IN IP6 ::1\r\nt=0 0\r\n"
		    "m=audio 5004 RTP/AVP 0\r\n"));
	CHECK(!m.addr_zero);

	CASE("write: an IP6 local address still writes IP6 through the "
	     "parsed family");
	local_init(&loc);
	snprintf(loc.addr, sizeof(loc.addr), "%s", LOCAL6);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("o=- 42 0 IN IP6 2001:db8::10\r\n"));
	CHECK(has("c=IN IP6 2001:db8::10\r\n"));

	/* and the IP4 spelling is what it always was */
	local_init(&loc);
	sdp_state_init(&st, 42);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("o=- 42 0 IN IP4 192.0.2.10\r\n"));
	CHECK(has("c=IN IP4 192.0.2.10\r\n"));
}

/*
 * RFC 3264 6.1: the answer direction for each offered peer direction (row)
 * and local direction (column), both in enum sdp_dir order.
 */
static const uint8_t dir_table[4][4] = {
	[SDP_SENDRECV] = { SDP_SENDRECV, SDP_SENDONLY, SDP_RECVONLY,
			   SDP_INACTIVE },
	[SDP_SENDONLY] = { SDP_RECVONLY, SDP_INACTIVE, SDP_RECVONLY,
			   SDP_INACTIVE },
	[SDP_RECVONLY] = { SDP_SENDONLY, SDP_SENDONLY, SDP_INACTIVE,
			   SDP_INACTIVE },
	[SDP_INACTIVE] = { SDP_INACTIVE, SDP_INACTIVE, SDP_INACTIVE,
			   SDP_INACTIVE },
};

/* the audio section of an offer or an answer that names the peer direction */
static bool dir_peer_parse(enum sdp_dir peer)
{
	char body[512];

	snprintf(body, sizeof(body), "%sm=audio 5004 RTP/AVP 0\r\na=%s\r\n",
		 HEAD, sdp_dir_name(peer));

	return parse(body);
}

/* one cell of dir_table on the answer build: the result and the wire */
static bool dir_answer_cell(enum sdp_dir peer, enum sdp_dir local)
{
	enum sdp_dir want = (enum sdp_dir)dir_table[peer][local];
	struct sdp_local loc;
	struct sdp_state st;
	char attr[32];

	local_init(&loc);
	sdp_state_init(&st, 55);
	sdp_dir_set(&st, local);
	if (!dir_peer_parse(peer))
		return false;

	buf_reset();
	if (sdp_answer_build(&st, &loc, &m, &b) != SDP_OK)
		return false;

	snprintf(attr, sizeof(attr), "a=ptime:20\r\na=%s\r\n",
		 sdp_dir_name(want));

	return st.proposed.dir == want && has(attr);
}

/* one cell of dir_table on the answer take, the peer direction answering */
static bool dir_take_cell(enum sdp_dir peer, enum sdp_dir local)
{
	struct sdp_local loc;
	struct sdp_state st;

	local_init(&loc);
	take_state_init(&st, &loc);
	sdp_dir_set(&st, local);
	if (!dir_peer_parse(peer))
		return false;
	if (sdp_answer_take(&st, &loc, &m) != SDP_OK)
		return false;

	return st.proposed.dir == dir_table[peer][local];
}

/* the local direction of sdp_dir_set() on the offer, the answer and the take */
static void dir_tests(void)
{
	char first[sizeof(out) + 16];
	struct sdp_state st, twin;
	struct sdp_local loc;
	uint64_t ver;

	CASE("a recvonly local direction offers a=recvonly");
	local_init(&loc);
	sdp_state_init(&st, 42);
	sdp_dir_set(&st, SDP_RECVONLY);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=ptime:20\r\na=recvonly\r\n"));
	CHECK(!has("a=sendonly"));
	CHECK(!has("a=inactive"));
	CHECK_UINT(st.proposed.dir, SDP_RECVONLY);
	CHECK(!sdp_local_hold(&st));

	CASE("an inactive local direction offers a=inactive");
	local_init(&loc);
	sdp_state_init(&st, 42);
	sdp_dir_set(&st, SDP_INACTIVE);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=ptime:20\r\na=inactive\r\n"));
	CHECK(!has("a=recvonly"));
	CHECK_UINT(st.proposed.dir, SDP_INACTIVE);
	CHECK(!sdp_local_hold(&st));

	CASE("a sendrecv local direction offers no direction attribute");
	sdp_dir_set(&st, SDP_SENDRECV);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!has("a=sendrecv"));
	CHECK(!has("a=inactive"));
	CHECK_UINT(st.proposed.dir, SDP_SENDRECV);

	CASE("a recvonly offer toward a held peer stays recvonly");
	local_init(&loc);
	sdp_state_init(&st, 42);
	st.committed.remote_hold = true;
	sdp_dir_set(&st, SDP_RECVONLY);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("a=recvonly\r\n"));
	CHECK_UINT(st.proposed.dir, SDP_RECVONLY);

	CASE("answer intersection: a peer sendrecv offer, each local "
	     "direction");
	CHECK(dir_answer_cell(SDP_SENDRECV, SDP_SENDRECV));
	CHECK(dir_answer_cell(SDP_SENDRECV, SDP_SENDONLY));
	CHECK(dir_answer_cell(SDP_SENDRECV, SDP_RECVONLY));
	CHECK(dir_answer_cell(SDP_SENDRECV, SDP_INACTIVE));

	CASE("answer intersection: a peer sendonly offer, each local "
	     "direction");
	CHECK(dir_answer_cell(SDP_SENDONLY, SDP_SENDRECV));
	CHECK(dir_answer_cell(SDP_SENDONLY, SDP_SENDONLY));
	CHECK(dir_answer_cell(SDP_SENDONLY, SDP_RECVONLY));
	CHECK(dir_answer_cell(SDP_SENDONLY, SDP_INACTIVE));

	CASE("answer intersection: a peer recvonly offer, each local "
	     "direction");
	CHECK(dir_answer_cell(SDP_RECVONLY, SDP_SENDRECV));
	CHECK(dir_answer_cell(SDP_RECVONLY, SDP_SENDONLY));
	CHECK(dir_answer_cell(SDP_RECVONLY, SDP_RECVONLY));
	CHECK(dir_answer_cell(SDP_RECVONLY, SDP_INACTIVE));

	CASE("answer intersection: a peer inactive offer, each local "
	     "direction");
	CHECK(dir_answer_cell(SDP_INACTIVE, SDP_SENDRECV));
	CHECK(dir_answer_cell(SDP_INACTIVE, SDP_SENDONLY));
	CHECK(dir_answer_cell(SDP_INACTIVE, SDP_RECVONLY));
	CHECK(dir_answer_cell(SDP_INACTIVE, SDP_INACTIVE));

	CASE("answer intersection: a zero address is a peer sendonly, each "
	     "local direction");
	local_init(&loc);
	for (unsigned int l = SDP_SENDRECV; l <= SDP_INACTIVE; l++) {
		sdp_state_init(&st, 55);
		sdp_dir_set(&st, (enum sdp_dir)l);
		CHECK(parse("v=0\r\no=- 1 1 IN IP4 198.51.100.7\r\n"
			    "c=IN IP4 0.0.0.0\r\nt=0 0\r\n"
			    "m=audio 5004 RTP/AVP 0\r\n"));
		buf_reset();
		CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
		CHECK_UINT(st.proposed.dir, dir_table[SDP_SENDONLY][l]);
	}

	CASE("take intersection: every pair of answered and local direction");
	for (unsigned int p = SDP_SENDRECV; p <= SDP_INACTIVE; p++) {
		for (unsigned int l = SDP_SENDRECV; l <= SDP_INACTIVE; l++)
			CHECK(dir_take_cell((enum sdp_dir)p, (enum sdp_dir)l));
	}

	CASE("sdp_hold_set(true) builds what sdp_dir_set(SDP_SENDONLY) "
	     "builds");
	local_init(&loc);
	sdp_state_init(&st, 42);
	sdp_state_init(&twin, 42);
	sdp_hold_set(&st, true);
	sdp_dir_set(&twin, SDP_SENDONLY);
	CHECK(sdp_local_hold(&st) && sdp_local_hold(&twin));
	CHECK_UINT(st.local_dir, SDP_SENDONLY);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	snprintf(first, sizeof(first), "%s", out);
	buf_reset();
	CHECK(sdp_offer_build(&twin, &loc, &b));
	CHECK(!strcmp(first, out));
	CHECK(has("a=sendonly\r\n"));
	CHECK(dir_peer_parse(SDP_SENDRECV));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	snprintf(first, sizeof(first), "%s", out);
	buf_reset();
	CHECK(sdp_answer_build(&twin, &loc, &m, &b) == SDP_OK);
	CHECK(!strcmp(first, out));
	CHECK_UINT(st.proposed.dir, twin.proposed.dir);

	CASE("sdp_hold_set(false) builds what sdp_dir_set(SDP_SENDRECV) "
	     "builds");
	sdp_hold_set(&st, false);
	sdp_dir_set(&twin, SDP_SENDRECV);
	CHECK(!sdp_local_hold(&st) && !sdp_local_hold(&twin));
	CHECK_UINT(st.local_dir, SDP_SENDRECV);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	snprintf(first, sizeof(first), "%s", out);
	buf_reset();
	CHECK(sdp_offer_build(&twin, &loc, &b));
	CHECK(!strcmp(first, out));
	CHECK(!has("a=sendonly"));

	CASE("sdp_local_hold() is true for the local direction sendonly "
	     "alone");
	sdp_state_init(&st, 42);
	sdp_dir_set(&st, SDP_RECVONLY);
	CHECK(!sdp_local_hold(&st));
	sdp_dir_set(&st, SDP_INACTIVE);
	CHECK(!sdp_local_hold(&st));
	sdp_dir_set(&st, SDP_SENDONLY);
	CHECK(sdp_local_hold(&st));
	sdp_dir_set(&st, SDP_SENDRECV);
	CHECK(!sdp_local_hold(&st));

	/* a direction change re-offers the same session under the SIP rule */
	CASE("a direction change alone re-offers the committed session with "
	     "the new attribute");
	local_init(&loc);
	sdp_state_init(&st, 42);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	sdp_commit(&st);
	twin = st;
	ver = st.sess_ver;
	buf_reset();
	CHECK(sdp_offer_build(&twin, &loc, &b));
	snprintf(first, sizeof(first), "%sa=inactive\r\n", out);
	sdp_dir_set(&st, SDP_INACTIVE);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!strcmp(first, out));
	CHECK_UINT(st.sess_ver, ver + 1);
	CHECK_UINT(st.proposed.dir, SDP_INACTIVE);
	CHECK(st.offer_pending);

	CASE("a direction change back to sendrecv re-offers without the "
	     "attribute");
	sdp_rollback(&st);
	sdp_dir_set(&st, SDP_SENDRECV);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!has("a=inactive"));
	CHECK(!has("a=sendrecv"));
	CHECK_UINT(st.sess_ver, ver + 2);
	CHECK_UINT(st.proposed.dir, SDP_SENDRECV);

	CASE("a value outside enum sdp_dir changes nothing");
	sdp_state_init(&st, 42);
	sdp_dir_set(&st, SDP_RECVONLY);
	sdp_dir_set(&st, (enum sdp_dir)(SDP_INACTIVE + 1));
	CHECK_UINT(st.local_dir, SDP_RECVONLY);
	CHECK(!sdp_local_hold(&st));
	sdp_dir_set(NULL, SDP_RECVONLY);

	CASE("a T.38 offer ignores the local direction");
	t38_local_init(&loc);
	sdp_state_init(&st, 42);
	sdp_t38_set(&st, true);
	sdp_dir_set(&st, SDP_RECVONLY);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(has("m=image 4000 udptl t38\r\n"));
	CHECK(!has("a=recvonly"));
	CHECK_UINT(st.proposed.dir, SDP_SENDRECV);
}

/* no line of the body names a direction */
static bool dir_attr_absent(void)
{
	return !has("a=sendrecv") && !has("a=sendonly") && !has("a=recvonly") &&
	       !has("a=inactive");
}

/* sdp_dir_attr_enable(false): the MGCP form, with no direction attribute */
static void dir_attr_tests(void)
{
	struct sdp_local loc;
	struct sdp_state st;

	CASE("the direction attribute is enabled after sdp_state_init()");
	sdp_state_init(&st, 42);
	CHECK(st.dir_attr);
	sdp_dir_attr_enable(&st, false);
	CHECK(!st.dir_attr);
	sdp_dir_attr_enable(&st, true);
	CHECK(st.dir_attr);
	sdp_dir_attr_enable(NULL, false);

	CASE("attribute off: an offer names no direction, each local "
	     "direction");
	local_init(&loc);
	for (unsigned int l = SDP_SENDRECV; l <= SDP_INACTIVE; l++) {
		sdp_state_init(&st, 42);
		sdp_dir_attr_enable(&st, false);
		sdp_dir_set(&st, (enum sdp_dir)l);
		buf_reset();
		CHECK(sdp_offer_build(&st, &loc, &b));
		CHECK(has("a=ptime:20\r\n"));
		CHECK(dir_attr_absent());
		CHECK_UINT(st.proposed.dir, l);
	}

	CASE("attribute off: an answer names no direction, each local "
	     "direction");
	for (unsigned int l = SDP_SENDRECV; l <= SDP_INACTIVE; l++) {
		sdp_state_init(&st, 55);
		sdp_dir_attr_enable(&st, false);
		sdp_dir_set(&st, (enum sdp_dir)l);
		CHECK(dir_peer_parse(SDP_SENDRECV));
		buf_reset();
		CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
		CHECK(has("a=ptime:20\r\n"));
		CHECK(dir_attr_absent());
		CHECK_UINT(st.proposed.dir, l);
	}

	CASE("attribute off: a peer sendonly offer and local sendrecv "
	     "negotiate sendrecv");
	sdp_state_init(&st, 55);
	sdp_dir_attr_enable(&st, false);
	CHECK(dir_peer_parse(SDP_SENDONLY));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(dir_attr_absent());
	CHECK_UINT(st.proposed.dir, SDP_SENDRECV);
	CHECK(!st.proposed.remote_hold);

	CASE("attribute off: a take of an a=inactive answer keeps the local "
	     "direction");
	for (unsigned int l = SDP_SENDRECV; l <= SDP_INACTIVE; l++) {
		take_state_init(&st, &loc);
		sdp_dir_attr_enable(&st, false);
		sdp_dir_set(&st, (enum sdp_dir)l);
		CHECK(dir_peer_parse(SDP_INACTIVE));
		CHECK(sdp_answer_take(&st, &loc, &m) == SDP_OK);
		CHECK_UINT(st.proposed.dir, l);
		CHECK(!st.proposed.remote_hold);
	}

	CASE("attribute off: a zero address still reads as a peer sendonly");
	sdp_state_init(&st, 55);
	sdp_dir_attr_enable(&st, false);
	CHECK(parse("v=0\r\no=- 1 1 IN IP4 198.51.100.7\r\nc=IN IP4 0.0.0.0\r\n"
		    "t=0 0\r\nm=audio 5004 RTP/AVP 0\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(dir_attr_absent());
	CHECK_UINT(st.proposed.dir, SDP_RECVONLY);
	CHECK(st.proposed.remote_hold);
}

/*
 * The SIP hold sequence as whole fixed bodies, so a change of the wire bytes
 * fails here even when the hold and the direction paths move together.
 */
static void hold_bytes_tests(void)
{
	static const char hold_offer[] =
		"v=0\r\n"
		"o=- 42 0 IN IP4 192.0.2.10\r\n"
		"s=-\r\n"
		"c=IN IP4 192.0.2.10\r\n"
		"t=0 0\r\n"
		"m=audio 4000 RTP/AVP 0 8 101\r\n"
		"a=rtpmap:0 PCMU/8000\r\n"
		"a=rtpmap:8 PCMA/8000\r\n"
		"a=rtpmap:101 telephone-event/8000\r\n"
		"a=fmtp:101 0-15\r\n"
		"a=ptime:20\r\n"
		"a=sendonly\r\n";
	static const char resume_offer[] =
		"v=0\r\n"
		"o=- 42 1 IN IP4 192.0.2.10\r\n"
		"s=-\r\n"
		"c=IN IP4 192.0.2.10\r\n"
		"t=0 0\r\n"
		"m=audio 4000 RTP/AVP 0 8 101\r\n"
		"a=rtpmap:0 PCMU/8000\r\n"
		"a=rtpmap:8 PCMA/8000\r\n"
		"a=rtpmap:101 telephone-event/8000\r\n"
		"a=fmtp:101 0-15\r\n"
		"a=ptime:20\r\n";
	static const char hold_answer[] =
		"v=0\r\n"
		"o=- 55 0 IN IP4 192.0.2.10\r\n"
		"s=-\r\n"
		"c=IN IP4 192.0.2.10\r\n"
		"t=0 0\r\n"
		"m=audio 4000 RTP/AVP 0\r\n"
		"a=rtpmap:0 PCMU/8000\r\n"
		"a=ptime:20\r\n"
		"a=sendonly\r\n";
	struct sdp_local loc;
	struct sdp_state st;

	CASE("hold bytes: a hold offer is byte exact");
	local_init(&loc);
	sdp_state_init(&st, 42);
	sdp_hold_set(&st, true);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!strcmp(out, hold_offer));

	CASE("hold bytes: the resume offer after it is byte exact");
	sdp_hold_set(&st, false);
	buf_reset();
	CHECK(sdp_offer_build(&st, &loc, &b));
	CHECK(!strcmp(out, resume_offer));

	CASE("hold bytes: a sendonly answer to a sendrecv offer is byte exact");
	sdp_state_init(&st, 55);
	sdp_hold_set(&st, true);
	CHECK(parse(HEAD "m=audio 5004 RTP/AVP 0\r\na=sendrecv\r\n"));
	buf_reset();
	CHECK(sdp_answer_build(&st, &loc, &m, &b) == SDP_OK);
	CHECK(!strcmp(out, hold_answer));
}

int main(void)
{
	parse_tests();
	usable_tests();
	g711_only_tests();
	offer_tests();
	g726_tests();
	g722_tests();
	opus_tests();
	pt_clash_tests();
	answer_tests();
	g729_fmtp_tests();
	g729_annexb_matrix_tests();
	g729_dual_pt_tests();
	take_tests();
	state_tests();
	t38_parse_tests();
	t38_datagram_tests();
	t38_offer_tests();
	t38_answer_tests();
	t38_gate_tests();
	t38_take_tests();
	t38_state_tests();
	srtp_parse_tests();
	srtp_base64_tests();
	srtp_offer_tests();
	srtp_answer_tests();
	srtp_take_tests();
	srtp_state_tests();
	srtp_t38_tests();
	rtcp_tests();
	family_tests();
	dir_tests();
	dir_attr_tests();
	hold_bytes_tests();

	return tu_summary("test_sdp");
}
