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
 * Dependencies. The message layer, for struct sip_str and struct sip_buf.
 * Nothing else: no uloop, no socket, no HAL. The codec vocabulary is this
 * layer's own enum; media_fill.c maps it to enum hal_codec, so this layer
 * stays free of hal.h exactly as it stays free of ubus and uci.
 *
 * What this layer owns, and why it has to.
 *
 *   The two-phase offer/answer state. A re-INVITE that fails (488, 491, a
 *   dead transaction) must leave the media exactly as the last committed
 *   exchange left it, so the state carries a committed result and a proposed
 *   result and only sdp_commit() moves proposed over committed. call.c calls
 *   sdp_commit() when the exchange completes (the ACK for the 2xx is sent or
 *   received) and sdp_rollback() on every failure path.
 *
 *   Codec selection in the answer follows the offer order (RFC 3264 6.1
 *   SHOULD): the first offered codec this side enables wins. Our own
 *   priority order applies to the offer we build, never to the answer.
 *   The answer lists the selected codec alone, plus telephone-event when
 *   both sides carry it (the offerer's payload type is mirrored back), plus
 *   CN when both sides carry it.
 *
 *   Telephone-event runs on the clock of the audio it rides (RFC 4733 2.1),
 *   so an offer that mixes clock families writes two event lines: the
 *   configured payload type carries the leading codec's clock and a spare,
 *   the highest free dynamic type, carries the other one. A single-family
 *   offer writes exactly one line, which is what every narrowband and G.722
 *   offer has always written. The answer writes the one line that matches
 *   the codec it selected, or none at all.
 *
 *   Nothing lands in a result that our offer did not put on the wire. The
 *   codec selection already ran that rule (a codec the collision policy
 *   dropped is off the line entirely); the two stream elements beside it run
 *   it too. sdp_answer_take() seats a telephone-event payload type only when
 *   this offer advertised an event line for the selected codec's clock and
 *   the answer's own event type sits on that clock, and it seats CN only
 *   when this offer actually carried the CN line, which the offer withholds
 *   for Opus (Opus never pairs with CN) even while the leg enable stays on.
 *   Anything else is the counted refusal, and call.c falls the digits back.
 *
 *   Direction and hold. sdp_dir_set() states the local direction, the media
 *   flow this side wants, and an offer writes it as an attribute when it is
 *   not sendrecv (sdp_dir_attr_enable() turns every direction attribute off
 *   for MGCP). Near-end hold is the local direction sendonly
 *   (sdp_hold_set(), the RFC 3264 8.4 form); a sendonly offer toward a peer
 *   that holds this side becomes inactive. Far-end hold is detected from a
 *   zero connection address, 0.0.0.0 or the IPv6 unspecified address (session
 *   or media level, RFC 4566 precedence), or from a peer direction of
 *   sendonly or inactive. The answer direction and the taken direction are
 *   the RFC 3264 6.1 intersection: this side sends only when it wants to send
 *   and the peer receives, and receives only when it wants to receive and the
 *   peer sends. A zero address reads as a peer sendonly.
 *
 *   One family per exchange. RFC 6157 section 5 runs one stream in one address
 *   family, and the socket the media backend binds has one family too. An
 *   answer therefore refuses a connection address outside the family of
 *   sdp_local.addr, and so does the answer take and the T.38 image gate. An
 *   RFC 3605 a=rtcp address outside the section's own family is the same
 *   refusal. sdp_offer_local_ok() asks both questions before the leg rings.
 *
 *   A leg whose own media address is no literal waives the sdp_local.addr half
 *   at every one of those sites and keeps the verdicts it had before the rule
 *   existed: such an address names no family, and it is the wildcard bind the
 *   call layer treats as legitimate. The a=rtcp half stands there, because it
 *   compares the section against itself.
 *
 *   T.38, gated. An offer with an m=image line is answered with that m= line
 *   at port 0 and the audio line negotiated normally, so the audio session
 *   survives, unless every gate of the image branch passes. The port-0
 *   refusal is the disabled path and answers every gate miss.
 *
 *   The gates, all of them:
 *
 *     - This leg has a committed exchange. T.38 is a mid-call upgrade, so an
 *       initial INVITE keeps the refusal path unchanged.
 *     - sdp_local.t38 enables T.38 on this leg.
 *     - The offered image section is usable: udptl, the t38 format token, a
 *       live port, and a connection address that is a literal, is not a zero
 *       address and is in the family of sdp_local.addr.
 *     - The offered audio section is absent, or it is at port 0 and the port
 *       parsed. Audio wins whenever a usable audio stream is left, so the
 *       audio session survives, and a port this layer could not read is a
 *       garbled stream, not a withdrawn one.
 *     - The offer named the audio section at most once and the image section
 *       at most once. A duplicate section goes to the parse sink, so no gate
 *       here can read its port.
 *
 *   An answer that passes every gate mirrors the offer's m= line order. It
 *   refuses an offered audio section at port 0, with the first format token
 *   that section named, and it negotiates the image section against the
 *   offered T38* attributes.
 *
 *   No image section carries a direction attribute on any exchange, and a
 *   T.38 result pins sendrecv: the DSP's T.38 direction handling is
 *   unverified. The local direction, hold included, therefore never reaches
 *   a T.38 exchange.
 *
 *   The T.38 enable is caller owned, exactly as the hold state is. call.c
 *   fills sdp_local.t38 and drives sdp_t38_set() around the upgrade, and it
 *   owns every set and every restore site the way it owns hold_prev. This
 *   layer reads st->local_t38 when it builds an offer and loc->t38 when it
 *   answers one.
 *
 *   The G.711 fax passthrough path is unchanged: the answering tone starts
 *   it and no option gates it.
 *
 *   SRTP, SDES (RFC 4568). An initial offer with sdp_local.srtp names RTP/SAVP
 *   (a re-offer follows the committed exchange, the last item of the matrix
 *   below) and carries one a=crypto line per suite, tag 1 for
 *   AES_CM_128_HMAC_SHA1_80 and tag 2 for AES_CM_128_HMAC_SHA1_32, each with
 *   the key the caller placed in sdp_local.srtp_key. The answer to an SAVP
 *   offer walks the offered crypto lines in offer order, takes the first one
 *   it can use, and mirrors that line's tag with a key of ours (RFC 4568
 *   5.1.2: an answer carries exactly one crypto attribute).
 *
 *   The matrix:
 *
 *     - an RTP/AVP initial offer is answered plain, whatever this leg carries,
 *       so a peer without SRTP still gets a call. A plain re-offer on a leg
 *       whose committed exchange names a suite is refused instead: answering
 *       it plain would commit the downgrade and no layer below reports it.
 *     - an RTP/SAVP offer on a leg without SRTP is refused, and so is every
 *       other transport. That is the behavior of the layer before SRTP.
 *     - an answer whose transport is not the one we offered is refused, in
 *       both directions (RFC 3264 6.2). An SAVP answer carries exactly one
 *       crypto line (RFC 4568 5.1.2) and it mirrors a tag we offered: no
 *       line, a line under another tag, and a second usable line are all
 *       refusals, the last because the peer's transmit key would then be
 *       ambiguous between its own two lines.
 *     - our own re-offer follows the transport the leg committed, not the leg
 *       enable: a leg that answered a plain first offer under accept-plain
 *       re-offers RTP/AVP while its exchange stays committed plain, and the
 *       answer to such an offer must be plain in the same way. Offering
 *       RTP/SAVP there would ask a peer with no SRTP for a transport it
 *       already declined, and its 488 would fail our own hold and clear the
 *       call at the first session refresh. This side never initiates an
 *       upgrade; the peer still may, by offering RTP/SAVP, and once that
 *       commits the next re-offer follows it.
 *
 *   A key is the 30 raw bytes of RFC 4568 6.1, and every octet value is valid.
 *
 *   The key material is caller owned, the way sess_id is: this layer has no
 *   entropy source. call.c fills both keys of sdp_local once per leg, and a
 *   re-offer on a keyed committed exchange re-emits them. The call layer
 *   stops and starts a stream whose key changes (media_program() in call.c),
 *   so the same keys keep a running stream running. The base64 text
 *   of an a=crypto line is wire output and is the one place a key appears as
 *   text.
 *
 *   T.38 is gated off on an SRTP leg. An image stream carries plain UDPTL, so
 *   an upspeed would drop the encryption the call negotiated without saying
 *   so: the answer gate closes and no offer is built while the committed
 *   exchange names a suite.
 *
 * Limits. G.711, G.729, G.722 and G.726-32 accept ptime values of 10, 20 or
 * 30. Opus accepts 10 or 20 and clamps 30 to 20; the Opus offer states this
 * ceiling as a=maxptime:20. The loader clamps 40 to 30. A configured
 * payload type of 129 means the codec is disabled and never offered. Two
 * codecs of one offer that carry the same payload type collide, and the
 * later one is dropped (sdp_offer_pt_owner below); a
 * dropped codec is then off the line entirely, so the answer build and the
 * answer take refuse it too and never seat a format on a contested type. An
 * offered direction on an image section is not mirrored, which is a
 * deliberate RFC 3264 6.1 deviation: a T.38 result pins sendrecv and the
 * image section this layer writes carries no direction attribute at all.
 */
#ifndef UVOICE_SIP_SDP_H
#define UVOICE_SIP_SDP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto/shared/msg.h"
#include "proto/shared/hdr.h"

/*
 * The parse side bound of fmt[] and the width of the local codec list. It grew
 * from 8 to 16 with G.722: a wideband peer offers more formats in one m= line
 * than a narrowband one, and a list this layer truncates hides a codec the
 * answer could have selected.
 */
#define SDP_CODEC_MAX		16
#define SDP_ADDR_MAX		46	/* INET6_ADDRSTRLEN */
#define SDP_FMT_NAME_MAX	20
#define SDP_PT_DISABLED		129	/* ptype 129 = codec disabled */
#define SDP_DTMF_PT_DEFAULT	101
#define SDP_SRTP_KEY_LEN	30	/* RFC 4568 6.1: 16 byte key, 14 byte salt */
#define SDP_CRYPTO_MAX		4

/*
 * This layer's codec vocabulary; media_fill.c maps it to enum hal_codec.
 *
 * G.722 runs its RTP clock at 8000 (RFC 3551 section 4.5.2), while its audio
 * runs at 16 kHz. The rtpmap therefore reads G722/8000 on both sides and the
 * HAL and the DSP own the audio rate; no SDP line of this layer ever names
 * 16000 for it.
 *
 * Opus always clocks at 48000 (RFC 7587 section 7: "only the highest, i.e.,
 * 48000, is used"), whatever rate the encoder runs at inside the payload, and
 * its rtpmap always names two channels. Every line of this layer therefore
 * reads opus/48000/2 on both sides, and the RTP clock of a stream is the one
 * fact that separates it from the codecs above.
 */
enum sdp_codec_id {
	SDP_CODEC_NONE = 0,
	SDP_CODEC_PCMU,			/* static PT 0 */
	SDP_CODEC_PCMA,			/* static PT 8 */
	SDP_CODEC_G729,			/* static PT 18 */
	SDP_CODEC_G722,			/* static PT 9 */
	SDP_CODEC_OPUS,			/* dynamic PT, clock 48000 */
	SDP_CODEC_G726_32,		/* dynamic PT, clock 8000 */
};

const char *sdp_codec_name(enum sdp_codec_id id);

enum sdp_dir {
	SDP_SENDRECV = 0,
	SDP_SENDONLY,
	SDP_RECVONLY,
	SDP_INACTIVE,
};

const char *sdp_dir_name(enum sdp_dir dir);

/* T.38 Annex D a=T38FaxRateManagement; NONE is this parser's absent state */
enum sdp_t38_rate {
	SDP_T38_RATE_NONE = 0,
	SDP_T38_RATE_LOCAL_TCF,
	SDP_T38_RATE_TRANSFERRED_TCF,
};

/*
 * T.38 Annex D a=T38FaxUdpEC. NONE is the absent state, which is also where an
 * unreadable value lands. FEC never survives a negotiation: an offered FEC is
 * answered with redundancy, so a result carries SDP_T38_EC_NONE or
 * SDP_T38_EC_REDUNDANCY alone.
 */
enum sdp_t38_ec {
	SDP_T38_EC_NONE = 0,
	SDP_T38_EC_REDUNDANCY,
	SDP_T38_EC_FEC,
};

/*
 * The T.38 Annex D attributes of one image section. Each number carries its own
 * present flag because 0 is a value the wire may name, T38FaxVersion:0 above
 * all, and the answer reconciliation reads an offered number only when the
 * offer named it. A value this layer cannot read leaves its field absent,
 * which is the tolerant-parse contract.
 *
 * T38FaxFillBitRemoval, T38FaxTranscodingMMR and T38FaxTranscodingJBIG stay
 * unparsed: this layer ignores them on receive and emits 0 for all three.
 */
struct sdp_t38_attrs {
	uint32_t version;
	uint32_t max_bit_rate;
	uint32_t max_buffer;
	uint32_t max_datagram;
	bool version_present;
	bool max_bit_rate_present;
	bool max_buffer_present;
	bool max_datagram_present;
	uint8_t rate_mgmt;		/* enum sdp_t38_rate, NONE = absent */
	uint8_t ec;			/* enum sdp_t38_ec, NONE = absent */
};

/*
 * The two SDES suites this stack carries (RFC 4568 6.2). The values equal
 * enum hal_srtp_suite and media_fill.c maps them by value, so this header
 * stays free of hal.h exactly as enum sdp_codec_id does.
 */
enum sdp_srtp_suite {
	SDP_SRTP_NONE = 0,
	SDP_SRTP_AES_CM_128_SHA1_80 = 1,
	SDP_SRTP_AES_CM_128_SHA1_32 = 2,
};

/* the RFC 4568 6.2 token of one suite, "none" for SDP_SRTP_NONE */
const char *sdp_srtp_suite_name(enum sdp_srtp_suite s);

/*
 * One a=crypto line this layer stored (RFC 4568 9.1). key is the decoded
 * concatenation, raw, exactly as it left the base64.
 */
struct sdp_crypto {
	uint32_t tag;
	uint8_t suite;			/* enum sdp_srtp_suite */
	uint8_t key[SDP_SRTP_KEY_LEN];
};

/*
 * The RFC 7587 6.1 parameters of a remote a=fmtp, which are the peer's decoder
 * speaking and therefore constrain our encoder. They are a separate record
 * from the fmtp this side emits, which states our own decoder preferences and
 * is built from constants, never from this.
 *
 * A key the peer left out leaves its field at the absent value, so the encoder
 * keeps the setting the codec section configured. A value outside the range
 * RFC 7587 gives for its key is dropped the same way ("SHOULD be ignored").
 *
 * usedtx is not here: the jitter buffer has no DTX gap handling, so this side
 * neither offers DTX nor honours an inbound one, and the key is read and
 * discarded like any other.
 */
struct sdp_opus_fmtp {
	uint32_t maxplaybackrate;	/* Hz, 0 = the peer named none */
	uint32_t maxaveragebitrate;	/* bit/s, 0 = the peer named none */
	bool fec;			/* useinbandfec, valid on fec_present */
	bool fec_present;
};

/*
 * One negotiable audio codec, cfg-derived, in our priority order.
 *
 * The opus block never reaches the wire. It is the encoder ask of the codec
 * section (bitrate, complexity, fec), carried here for the same reason vad is:
 * this struct is what a negotiation copies into sdp_result, so media_fill.c
 * reads the configured settings and the peer's constraints from one record
 * when it fills the stream description. Every other codec leaves the block
 * zero.
 */
struct sdp_codec {
	uint8_t id;			/* enum sdp_codec_id */
	uint8_t pt;			/* payload type to offer */
	bool vad;			/* codec section silence_suppression */
	/*
	 * The NEGOTIATED result, not a configured ask like vad: our TX DTX is
	 * armed for G.729 Annex B. It rides every copy vad does (answered,
	 * st->offered/st->proposed, sdp_result.codec) because the negotiated
	 * view call.c programs a stream from has to travel with the codec
	 * entry that carries it, and no separate sdp_result field exists for
	 * it.
	 *
	 * It is true only when the media plane carries Annex B (sdp_local.g729b)
	 * AND this side's codec section set vad AND the peer wrote an explicit
	 * annexb=yes for the payload type this exchange negotiated on, because
	 * A-only endpoints omit the fmtp. Every other codec leaves it zero.
	 * It does not describe what we may RECEIVE: this stack decodes a SID
	 * frame on every G.729 stream and advertises annexb=yes for it, which
	 * is the separate claim codec_rtpmap_write() puts on the wire.
	 */
	bool annexb;
	struct {
		uint32_t bitrate;	/* bit/s, 0 = the library default */
		uint8_t complexity;	/* 1..10, 0 = the library default */
		bool fec;
	} opus;
};

/*
 * Is this entry one an offer may write and an answer may select? It is the one
 * gate both sides of the local list run: media_fill.c runs it too, so a list
 * can never carry an entry the offer builder would skip.
 */
bool sdp_codec_offerable(const struct sdp_codec *c);

/*
 * One format of an m= line: the payload type the peer numbered it, the name
 * and clock an a=rtpmap gave it, and the one a=fmtp parameter this layer keeps
 * per format.
 *
 * annexb is that parameter (RFC 3555 4.1.9) and it is valid only on
 * annexb_set: an absent key, a value that is neither "yes" nor "no", and a
 * format desc_finish() judged not to be G.729 all leave the record clear.
 *
 * It lives on the format and not on the section because one m= line may name
 * G.729 twice, at two payload types, with a different annexb on each. A single
 * per-section record keeps the last one, and an answer that selects the other
 * payload type then reads a value the peer wrote about a format it did not
 * select: it would restate yes over an explicit no, which is the very
 * misadvertisement this parameter exists to prevent.
 */
struct sdp_fmt {
	uint8_t pt;
	char name[SDP_FMT_NAME_MAX];	/* upper-cased rtpmap name */
	uint32_t rate;
	bool annexb;
	bool annexb_set;
};

/*
 * One parsed m= line. fmt[] preserves the offer order, which is what the
 * answer selection walks. A static payload type (0, 8, 9, 18) is recognized
 * without an rtpmap; an rtpmap overrides the static meaning (RFC 3264 5.1).
 */
struct sdp_media_desc {
	bool present;
	bool proto_ok;			/* the proto the section's kind needs:
					   RTP/AVP for audio, udptl for image
					   (T.38 Annex D). An SAVP or other
					   transport makes the section unusable
					   and the answer refuses it (RFC 3264:
					   the answer transport must match) */
	bool t38_fmt;			/* the image fmt list named t38 */
	uint16_t port;			/* 0 = rejected/refused stream */
	bool port_ok;			/* the m= port was a number this layer
					   could read, so a 0 here is the peer
					   withdrawing the stream (RFC 3264 8.4)
					   and not a port it garbled. "5004/2"
					   is a valid parse, the count is
					   dropped; "nope" and 70000 are not */
	char addr[SDP_ADDR_MAX];	/* media c= wins over session c= */
	uint8_t ipver;			/* 4 or 6, from the c= line in force */
	struct sdp_fmt fmt[SDP_CODEC_MAX];
	unsigned int fmts;
	/*
	 * The m= line named more formats than fmt[] holds, so this section is
	 * an incomplete record of what the peer offered. A gate that reads the
	 * whole list as evidence has to refuse such a section: fmts alone
	 * cannot say so, because telephone-event and CN leave fmt[] again and
	 * a full list can shrink back under the cap after a format was already
	 * dropped.
	 */
	bool fmt_dropped;
	int fmt_first;			/* the first numeric fmt token the m= line
					   listed, -1 when it listed none. It is
					   recorded before telephone-event and CN
					   leave fmt[], so a stream that offered
					   one of those alone can still be
					   refused with a format it named
					   (RFC 4566: an m= line needs one) */
	/*
	 * Two telephone-event payload types, one per clock this stack can key
	 * a stream to. RFC 4733 section 2.1 runs the event stream on the clock
	 * of the audio it rides, so an Opus stream needs the 48000 one and
	 * every other codec the 8000 one, and a peer that offers both a
	 * 8000 clock codec and Opus names both at two payload types. A rate
	 * that is neither leaves both absent, which is the refusal the
	 * narrowband stack always made.
	 */
	int dtmf_pt;			/* telephone-event PT at 8000, -1 absent */
	int dtmf_pt_wb;			/* telephone-event PT at 48000, -1 absent */
	int cn_pt;			/* CN PT, -1 absent */
	/*
	 * The Opus a=fmtp of this section, and the payload type it belongs to
	 * (-1 when the section carried none). One record per section: the parse
	 * keeps the last a=fmtp whose parameters name an RFC 7587 key and then
	 * drops it unless that payload type is the section's Opus one, so a
	 * body that hangs Opus keys off another format loses the record instead
	 * of applying it to a codec it does not describe.
	 */
	int opus_fmtp_pt;
	struct sdp_opus_fmtp opus_fmtp;
	/*
	 * The G.729 a=fmtp annexb key (RFC 3555 4.1.9) has no record here. It
	 * rides the format it describes instead (struct sdp_fmt above), so a
	 * section that names G.729 at two payload types keeps both answers
	 * apart. desc_finish() then clears the record of every format that does
	 * not map to SDP_CODEC_G729, which is static PT 18 with no rtpmap or a
	 * dynamic PT whose rtpmap names G729, G729A or G729B: a body that hangs
	 * the key off another format loses it instead of applying it to a codec
	 * it does not describe.
	 */
	uint8_t dir;			/* enum sdp_dir: media else session attr */
	bool dir_present;		/* an explicit direction attribute seen */
	uint8_t ptime_ms;		/* 0 = absent */
	struct sdp_t38_attrs t38;	/* image section only */
	bool savp;			/* the proto was RTP/SAVP. It is a
					   separate flag and not a proto_ok
					   value, because proto_ok keeps its
					   exact meaning: the plain transport
					   the section's kind needs */
	struct sdp_crypto crypto[SDP_CRYPTO_MAX];
	unsigned int cryptos;		/* lines stored, so a fifth one and every
					   line this layer cannot honor is not
					   counted (RFC 4568 9.1). Tags are
					   unique here: the first line under a
					   tag wins and a later one is dropped,
					   because the answer names its chosen
					   line by tag alone */
	/*
	 * The RFC 3605 a=rtcp record of this section. rtcp_port is the port the
	 * peer receives RTCP on, and 0 means the section named none, which
	 * leaves the RTP port plus one (RFC 3550 section 11). rtcp_addr is the
	 * optional address of the attribute and stays empty when the attribute
	 * named a port alone. rtcp_ipver is 4 or 6 for that address and 0 when
	 * the section carries none.
	 *
	 * RFC 3605 defines the attribute at media level, so a session level one
	 * is tolerance and reaches the audio section alone: a UDPTL image
	 * stream carries no RTCP at all.
	 */
	uint16_t rtcp_port;
	char rtcp_addr[SDP_ADDR_MAX];
	uint8_t rtcp_ipver;
};

/* one parsed session: the audio line and, when present, the image line */
struct sdp_msg {
	uint64_t sess_id;
	uint64_t sess_ver;
	struct sdp_media_desc audio;
	struct sdp_media_desc image;	/* T.38; .present drives the refusal */
	bool addr_zero;			/* a zero c= in force, 0.0.0.0 or the
					   IPv6 unspecified address (RFC 2543
					   hold) */
	bool image_first;		/* the image m= line came before the
					   audio one, which is the order an
					   RFC 3264 6 answer has to mirror. A
					   third and later section keeps the
					   sink behavior: it is parsed and
					   dropped, and no answer names it */
	bool audio_dup;			/* the body named more than one m=audio
					   or more than one m=image section. The
					   second one goes to the parse sink, so
					   its port and its codec list are not
					   in audio/image above and no gate here
					   can read them. T.38 acceptance closes
					   on either flag rather than negotiate
					   against an offer it cannot see */
	bool image_dup;
};

/*
 * Tolerant parse: unknown lines and unknown attributes are skipped, LF-only
 * line ends are accepted, and only a body without a usable v=/m= structure
 * is refused. The body is read, never written; no allocation.
 */
bool sdp_parse(struct sip_str body, struct sdp_msg *out);

/*
 * Can the address layer program the audio stream this parsed offer names?
 * True when the resolved audio connection address (the media c= when it has
 * one, else the session c=, which is the field the answer build and the answer
 * take read) is an IPv4 or an IPv6 literal.
 *
 * It answers that question alone. An offer this predicate accepts may still be
 * refused for its codec, its port or its transport, and sdp_answer_build()
 * stays the one place those verdicts are made. call.c asks it before an
 * inbound leg rings, so an offer no answer could ever honor is refused with a
 * 488 instead of ringing the subscriber first.
 */
bool sdp_offer_usable(const struct sdp_msg *offer);

/*
 * Does the audio section of this offer run in one address family, and is it the
 * family of local_addr? It reads both addresses the section names, the
 * connection address and the RFC 3605 RTCP address. One stream cannot straddle
 * two families: the socket the media backend binds has one family, and RFC 6157
 * section 5 asks the media to follow the signalling this side already chose. A
 * mixed family offer is therefore refused in front of the ring, as an FQDN is,
 * instead of ringing the subscriber and answering 488 once the handset comes
 * up.
 *
 * A NULL or empty local_addr waives the local half alone: a caller that holds
 * no literal of its own names no family to compare against, and every verdict
 * of such a leg stays what it was. The RTCP address still has to match the
 * connection address, because that test asks nothing of this side.
 *
 * It answers this question alone. sdp_answer_build() still makes the codec, the
 * port and the transport verdicts.
 */
bool sdp_offer_local_ok(const struct sdp_msg *offer, const char *local_addr);

/*
 * Does this offer describe a plain G.711 audio session and nothing else? It is
 * the SDP half of the far-end fax upspeed recognizer (call.c
 * fax_follow_check): a gateway that switches a call to a fax narrows its audio
 * list to G.711, and no other token on the wire says so.
 *
 * True asks for all of: one audio section, every format in it PCMU or PCMA at
 * the G.711 clock, at least one such format, the effective peer direction
 * sendrecv, and no image section. telephone-event and comfort noise are out of
 * fmt[] by the time this runs, so they neither pass nor fail the list, and a
 * format the parser could not name fails it.
 *
 * It refuses everything the parser could not record whole. A body with two
 * m=audio sections keeps the second one out of offer->audio, and an m= line
 * longer than fmt[] loses its tail; a narrowing verdict taken on either would
 * be taken on evidence this layer never saw.
 */
bool sdp_audio_g711_only(const struct sdp_msg *offer);

/*
 * Our side of a negotiation, built by media_fill.c from cfg + the account.
 * Strings are copied into nothing: the struct itself is copied into the
 * call leg at leg start, so a reload never moves a live negotiation.
 */
struct sdp_local {
	struct sdp_codec codec[SDP_CODEC_MAX];	/* our priority order */
	unsigned int codecs;
	char addr[SDP_ADDR_MAX];	/* media address = account local host */
	uint16_t port;			/* allocated even RTP port */
	uint8_t ptime_ms;		/* cfg value, already clamped 40->30 */
	/*
	 * ptime_ms wins over the a=ptime of the peer in an answer and in the
	 * result of an answer taken, within the per-codec mask. The MGCP
	 * endpoint sets it for a p: of the call agent (RFC 3435 3.2.2.10);
	 * SIP leaves it false.
	 */
	bool ptime_pin;
	/*
	 * Offers and answers carry no a=ptime line; the packet time still holds
	 * for the stream. The MGCP endpoint sets it for send_ptime 0; SIP
	 * leaves it false.
	 */
	bool ptime_omit;
	uint8_t dtmf_pt;		/* 0 = do not offer telephone-event */
	bool cn;			/* cn_enable: offer CN (PT 13) */
	bool t38;			/* this leg may negotiate T.38 */
	bool srtp;			/* this leg negotiates SDES SRTP */
	/*
	 * The media plane under this leg carries G.729 Annex B (hal.h,
	 * HAL_FEAT_G729B). It is a backend capability and not a config switch,
	 * read once at leg start exactly as t38 and srtp above are, because a
	 * backend capability cannot move under a running call.
	 *
	 * It answers two questions of this layer. Our own G.729 rtpmap states
	 * annexb=yes (RFC 3555 4.1.9) only while it is set, as a claim about
	 * what this side decodes. A codec section with silence suppression may
	 * arm transmit DTX only while it is set. A leg on a
	 * backend without the bit therefore writes annexb=no and never arms,
	 * which is the whole of the Annex A behaviour.
	 */
	bool g729b;
	/*
	 * The key this side offers and answers under each suite: index
	 * suite - 1, so srtp_key[0] belongs to _80 and srtp_key[1] to _32.
	 * call.c fills both once per leg and this layer only reads them, the
	 * way it reads sess_id: there is no entropy source here.
	 */
	uint8_t srtp_key[2][SDP_SRTP_KEY_LEN];
};

/* everything call.c needs to program the HAL stream for one exchange */
struct sdp_result {
	struct sdp_codec codec;		/* the one negotiated codec */
	int dtmf_pt;			/* negotiated, -1 = fall back inband */
	int cn_pt;			/* negotiated, -1 = none */
	uint8_t dir;			/* our direction, enum sdp_dir */
	uint8_t ptime_ms;
	char remote_addr[SDP_ADDR_MAX];
	uint8_t remote_ipver;
	uint16_t remote_port;
	bool remote_hold;		/* a zero c= or a sendonly/inactive
					   peer */
	bool image_refused;		/* m=image answered with port 0 */
	bool t38;			/* the exchange carries T.38, not audio:
					   the remote tuple names the image
					   section and codec stays NONE */
	uint32_t t38_max_bit_rate;	/* negotiated, bit/s */
	uint16_t t38_max_datagram;	/* 0 = the peer named none */
	uint8_t t38_ec;			/* enum sdp_t38_ec, never FEC */
	/*
	 * enum sdp_srtp_suite, NONE = the exchange carries plain RTP. key_tx
	 * protects what this side sends and is our own key of the negotiated
	 * suite; key_rx opens what the peer sends and is the peer's inline key.
	 * Both ride sdp_commit() and sdp_rollback() as struct copies.
	 */
	uint8_t srtp_suite;
	uint8_t srtp_key_tx[SDP_SRTP_KEY_LEN];
	uint8_t srtp_key_rx[SDP_SRTP_KEY_LEN];
	/*
	 * The peer's a=fmtp for the codec above, empty unless that codec is
	 * Opus. It constrains our encoder and media_fill.c applies it; the
	 * configured ask it narrows rides codec.opus.
	 */
	struct sdp_opus_fmtp opus_remote;
	/*
	 * The peer's RTCP endpoint (RFC 3605). rtcp_port is the port the peer
	 * receives RTCP on, and 0 means the peer named none, which leaves the
	 * media port plus one (RFC 3550 section 11). rtcp_addr is the address
	 * the attribute named and is empty when it named a port alone;
	 * rtcp_ipver is 4 or 6 for that address and 0 when there is none. A
	 * T.38 exchange carries the three at zero: a UDPTL stream has no RTCP.
	 */
	uint16_t rtcp_port;
	char rtcp_addr[SDP_ADDR_MAX];
	uint8_t rtcp_ipver;
	bool valid;
};

enum sdp_verdict {
	SDP_OK = 0,
	SDP_NO_CODEC,			/* no usable audio: 488 / ACK then BYE */
	SDP_BAD,			/* no usable session structure */
};

/*
 * Two-phase negotiation state, embedded in the call leg. sess_id seeds the
 * o= line and comes from the caller (call.c uses the transaction rand), so
 * this layer needs no entropy source. sess_ver increments on every offer
 * and every changed answer this side emits (RFC 3264 8).
 */
struct sdp_state {
	uint64_t sess_id;
	uint64_t sess_ver;
	/*
	 * What the pending offer put on the wire beside its codecs, recorded by
	 * the offer builder so sdp_answer_take() judges an answer against the
	 * description this side sent.
	 *
	 * It is not recomputed from struct sdp_local at take time, because that
	 * view can move under a live leg: call.c rewrites it on a fax upspeed
	 * (loc_g711_clamp), and a reload replaces the codec sections. A view
	 * that moved would let the take accept a stream element the offer never
	 * carried, or refuse one it did, with nothing on the wire to say so.
	 *
	 * clock is the leading codec's RTP clock and dtmf_pt the event type
	 * written on it; alt_clock and alt_pt are the second event line of a
	 * mixed offer, alt_clock 0 when the offer wrote one line. Each payload
	 * type is -1 when the offer wrote none. A T.38 offer clears all five.
	 */
	struct {
		uint32_t clock;
		uint32_t alt_clock;
		int dtmf_pt;
		int alt_pt;
		int cn_pt;
		/*
		 * The payload type this offer's own G.729 entry carried, -1
		 * when the offer carried no G.729 entry, and whether that
		 * entry may arm TX DTX once the answer confirms it with an
		 * explicit annexb=yes. Keyed by payload type because a G.729
		 * entry need not be the leading codec of a mixed offer, and a
		 * single record per section would mishandle an offer with two
		 * G.729 payload types. sdp_answer_take() reads both
		 * against the answer's own selected payload type instead of
		 * recomputing the local half fresh, for the same reason
		 * offered.cn_pt is read and not recomputed: struct sdp_local
		 * can move under a live leg.
		 */
		int g729_pt;
		bool g729_annexb;
	} offered;
	uint8_t local_dir;		/* enum sdp_dir, set by sdp_dir_set() */
	bool dir_attr;			/* see sdp_dir_attr_enable() */
	bool local_t38;			/* the next offer we build is T.38 */
	bool offer_pending;		/* our offer awaits an answer */
	bool offer_in_200;		/* offerless INVITE: we offered in the
					   2xx, the answer arrives in the ACK */
	struct sdp_result committed;
	struct sdp_result proposed;
};

void sdp_state_init(struct sdp_state *st, uint64_t sess_id);

/*
 * The local direction of the next offer or answer this side builds,
 * SDP_SENDRECV after sdp_state_init(). A value outside enum sdp_dir changes
 * nothing. A T.38 exchange ignores it (see above).
 */
void sdp_dir_set(struct sdp_state *st, enum sdp_dir dir);

/* sdp_dir_set() with SDP_SENDONLY for hold, SDP_SENDRECV for resume */
void sdp_hold_set(struct sdp_state *st, bool hold);

/* the hold state of the call layer: the local direction is sendonly */
static inline bool sdp_local_hold(const struct sdp_state *st)
{
	return st->local_dir == SDP_SENDONLY;
}

/*
 * The direction attribute, true after sdp_state_init(). MGCP turns it off:
 * an MGCP connection descriptor carries no direction attribute (RFC 3435
 * 3.4 defines none) and its receiver ignores one (NCS 1.5 8.4.2.10),
 * because the ConnectionMode sets the direction. When it is off, the offer and
 * the answer write no direction attribute, and the answer build and the
 * answer take negotiate local_dir without reading the peer's attribute. A
 * zero connection address still reads as a peer sendonly, because it is an
 * address and not an attribute.
 */
void sdp_dir_attr_enable(struct sdp_state *st, bool on);
void sdp_t38_set(struct sdp_state *st, bool t38);
void sdp_t38_max_datagram_set(unsigned int n);

/*
 * Build an offer into b (initial INVITE, re-INVITE, or the 2xx to an
 * offerless INVITE; the caller sets st->offer_in_200 for the last case).
 * Fills st->proposed with the local view and sets offer_pending.
 */
bool sdp_offer_build(struct sdp_state *st, const struct sdp_local *loc,
		     struct sip_buf *b);

/*
 * The rtpmap line of the same offer that already claims the payload type of
 * loc->codec[idx], or NULL when sdp_offer_build() will write that codec. The
 * answer is the token this layer would print: a codec name, "telephone-event"
 * or "CN".
 *
 * It exists because this layer has no logger. sdp.c is a libc-only source
 * that both fuzz targets compile without libubox, so media_fill.c reads the
 * verdict back and names both sides in one line. The offer build itself
 * calls the same predicate, so the log and the wire can never disagree.
 */
const char *sdp_offer_pt_owner(const struct sdp_local *loc, unsigned int idx);

/*
 * Answer a received offer into b and fill st->proposed. SDP_NO_CODEC leaves
 * b untouched; the caller answers 488 (inbound) or clears the call.
 */
enum sdp_verdict sdp_answer_build(struct sdp_state *st,
				  const struct sdp_local *loc,
				  const struct sdp_msg *offer,
				  struct sip_buf *b);

/*
 * Take the answer to our pending offer and fill st->proposed. SDP_NO_CODEC
 * means the peer selected nothing usable; the caller ACKs and BYEs.
 *
 * After a T.38 offer, SDP_NO_CODEC means the peer refused the upgrade: an
 * answer without a usable image section is a refusal and never an audio
 * session, so the caller rolls back to the committed audio exchange and keeps
 * the call.
 */
enum sdp_verdict sdp_answer_take(struct sdp_state *st,
				 const struct sdp_local *loc,
				 const struct sdp_msg *answer);

/* the exchange completed (ACK sent or received): proposed becomes committed */
void sdp_commit(struct sdp_state *st);

/* the exchange failed (488, 491, timeout): proposed reverts to committed */
void sdp_rollback(struct sdp_state *st);

#endif
