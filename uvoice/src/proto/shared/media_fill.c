// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The media helpers of a protocol that negotiates SDP. See media_fill.h for
 * the contracts.
 */
#include <arpa/inet.h>
#include <string.h>

#include <libubox/ulog.h>

#include "proto/shared/media_fill.h"
#include "proto/shared/sdp.h"
#include "str.h"

/* --------------------------------------------------------- codec mapping */

enum hal_codec media_codec_of(enum sdp_codec_id id)
{
	switch (id) {
	case SDP_CODEC_PCMU:
		return HAL_CODEC_PCMU;
	case SDP_CODEC_PCMA:
		return HAL_CODEC_PCMA;
	case SDP_CODEC_G729:
		return HAL_CODEC_G729;
	case SDP_CODEC_G722:
		return HAL_CODEC_G722;
	case SDP_CODEC_OPUS:
		return HAL_CODEC_OPUS;
	case SDP_CODEC_G726_32:
		return HAL_CODEC_G726_32;
	case SDP_CODEC_NONE:
		break;
	}

	return HAL_CODEC_NONE;
}

/*
 * The two error correction values a T.38 negotiation can end on. FEC never
 * survives sdp.c's reconciliation, so it cannot reach a result; the switch
 * names it anyway, so a new SDP member fails the build here instead of being
 * programmed as no correction at all.
 */
static enum hal_t38_ec media_t38_ec_of(enum sdp_t38_ec ec)
{
	switch (ec) {
	case SDP_T38_EC_REDUNDANCY:
		return HAL_T38_EC_REDUNDANCY;
	case SDP_T38_EC_FEC:
	case SDP_T38_EC_NONE:
		break;
	}

	return HAL_T38_EC_NONE;
}

/*
 * TR-104 codec identity of the uci codec sections to this layer's enum.
 *
 * Ids 4, 5 and 6 reach the list only through a named section. The fallback
 * names the three schema codecs, so a line without codec sections offers no
 * G.726-32, G.722 or Opus.
 */
static enum sdp_codec_id sdp_codec_of_tr104(unsigned int tr104_id)
{
	switch (tr104_id) {
	case 1:
		return SDP_CODEC_PCMU;
	case 2:
		return SDP_CODEC_PCMA;
	case 3:
		return SDP_CODEC_G729;
	case 4:
		return SDP_CODEC_G726_32;
	case 5:
		return SDP_CODEC_G722;
	case 6:
		return SDP_CODEC_OPUS;
	default:
		return SDP_CODEC_NONE;
	}
}

uint8_t media_codec_pt_of(enum sdp_codec_id id, const struct cfg_channel *ch)
{
	switch (id) {
	case SDP_CODEC_PCMU:
		return 0;
	case SDP_CODEC_PCMA:
		return (uint8_t)(ch->g711a_ptype ? ch->g711a_ptype : 8);
	case SDP_CODEC_G729:
		return (uint8_t)(ch->g729a_ptype ? ch->g729a_ptype : 18);
	case SDP_CODEC_G722:
		return (uint8_t)(ch->g722_ptype ? ch->g722_ptype : 9);
	case SDP_CODEC_OPUS:
		return (uint8_t)(ch->opus_ptype ? ch->opus_ptype : 96);
	case SDP_CODEC_G726_32:
		return (uint8_t)(ch->g726_ptype ? ch->g726_ptype : 111);
	case SDP_CODEC_NONE:
		break;
	}

	return SDP_PT_DISABLED;
}

/*
 * The Opus encoder ask of one codec section, which never reaches the wire: our
 * own a=fmtp states this decoder's preferences instead (shared/sdp.h, struct
 * sdp_codec). A zero bitrate or complexity means the library default (hal.h,
 * the opus block), so an unset option is carried through as it stands.
 */
static void opus_params_take(struct sdp_codec *c, const struct cfg_codec *cc)
{
	c->opus.bitrate = cc->bitrate;
	c->opus.complexity = (uint8_t)cc->complexity;
	c->opus.fec = cc->fec;
}

/*
 * One offerability gate for the list and the offer. The entry is built first
 * and sdp_codec_offerable() then judges the whole entry, so a codec this build
 * would seat in the local list is one sdp.c will write. A test of the HAL mask
 * alone would let a mask that carried a codec sdp.c refuses create an entry
 * the offer builder skips. The two views of the line would then disagree,
 * with nothing on the wire to say so.
 */
static bool codec_add(struct sdp_local *out, enum sdp_codec_id id, uint8_t pt, bool vad,
		      const struct cfg_codec *cc, uint32_t mask)
{
	struct sdp_codec c;
	unsigned int i;

	memset(&c, 0, sizeof(c));
	c.id = (uint8_t)id;
	c.pt = pt;
	c.vad = vad;
	if (id == SDP_CODEC_OPUS && cc)
		opus_params_take(&c, cc);

	if (!sdp_codec_offerable(&c))
		return false;
	if (out->codecs >= SDP_CODEC_MAX)
		return false;
	if (!(mask & (1u << media_codec_of(id))))
		return false;

	for (i = 0; i < out->codecs; i++) {
		if (out->codec[i].id == c.id)
			return false;
	}

	out->codec[out->codecs++] = c;

	return true;
}

/*
 * One log line per codec the offer will drop. sdp.c decides
 * which codec loses a shared payload type and it cannot say so itself: it is
 * a libc-only source that logs nothing. Reading the verdict back here keeps
 * one policy and still names both sides for an operator.
 */
static void codec_clash_report(const struct sdp_local *loc, unsigned int line)
{
	unsigned int i;

	for (i = 0; i < loc->codecs; i++) {
		const char *owner = sdp_offer_pt_owner(loc, i);

		if (!owner)
			continue;

		ULOG_WARN("line %u drops codec %s from the offer: payload type %u already carries %s\n",
			  line, sdp_codec_name((enum sdp_codec_id)loc->codec[i].id),
			  (unsigned int)loc->codec[i].pt, owner);
	}
}

/*
 * Our offer, in our priority order. The codec sections arrive already sorted
 * by priority (cfg/uci.c), so the walk keeps their order. A section for
 * another channel is skipped, a disabled one is skipped, payload type 129
 * means the codec is off (the uci schema sentinel), and codec_mask is the HAL
 * capability set so a build without G.729 never offers it. G.726-32, G.722
 * and Opus reach the list through a named section alone, so a mask that
 * carries them changes nothing on a line that names neither.
 *
 * A line with no codec section of its own falls back to the three codecs the
 * schema names, in the schema order.
 */
void media_sdp_local_build(const struct cfg *base,
			   const struct cfg_channel *ch, uint32_t codec_mask,
			   const char *addr, uint16_t port,
			   struct sdp_local *out)
{
	unsigned int i, sections = 0;

	memset(out, 0, sizeof(*out));
	if (!ch)
		return;

	str_copy(out->addr, sizeof(out->addr), addr);
	out->port = port;
	out->ptime_ms = (uint8_t)(ch->ptime_ms ? ch->ptime_ms : 20);
	out->dtmf_pt = ch->dtmf_mode == CFG_DTMF_RFC2833 ? SDP_DTMF_PT_DEFAULT : 0;
	out->cn = ch->cn_enable;

	for (i = 0; base && i < base->codecs && i < CFG_CODEC_MAX; i++) {
		const struct cfg_codec *cc = &base->codec[i];
		enum sdp_codec_id id;

		if (cc->channel && cc->channel - 1 != ch->id)
			continue;

		sections++;
		if (!cc->enabled)
			continue;

		id = sdp_codec_of_tr104(cc->tr104_id);
		codec_add(out, id, media_codec_pt_of(id, ch), cc->silence_suppression, cc, codec_mask);
	}

	/* the fallback names no section, so it carries no Opus parameters */
	if (!sections) {
		codec_add(out, SDP_CODEC_PCMU, media_codec_pt_of(SDP_CODEC_PCMU, ch), false, NULL,
			  codec_mask);
		codec_add(out, SDP_CODEC_PCMA, media_codec_pt_of(SDP_CODEC_PCMA, ch), false, NULL,
			  codec_mask);
		codec_add(out, SDP_CODEC_G729, media_codec_pt_of(SDP_CODEC_G729, ch), false, NULL,
			  codec_mask);
	}

	codec_clash_report(out, ch->id);
}

/* --------------------------------------------------------- hal_media fill */

/*
 * Two forms the address layer cannot honor, and what each costs on the wire.
 *
 * A host that is neither an IPv4 nor an IPv6 literal, an FQDN for instance,
 * which SDP allows and sdp.c copies verbatim, leaves the address all zero. A
 * media socket connected to 0.0.0.0 connects to its own source address and
 * filters every real peer out, so the stream is dead while the program reports
 * success.
 *
 * A port of 65535 wraps the RTCP derivation below to 0. An RTCP socket
 * connected to port 0 keeps the address filter and loses the port filter, and
 * getpeername() reports nothing. The rule sits here because the +1 sits here.
 * 65534 is the last port a description may name.
 */
static bool hal_addr_set(struct hal_addr *a, const char *host, uint16_t port, uint8_t ipver)
{
	struct in_addr v4;
	struct in6_addr v6;

	memset(a, 0, sizeof(*a));
	a->ipver = ipver ? ipver : 4;
	a->port = port;
	if (port == UINT16_MAX)
		return false;

	a->rtcp_port = port ? (uint16_t)(port + 1) : 0;

	if (host && inet_pton(AF_INET, host, &v4) == 1) {
		a->ipver = 4;
		memcpy(a->addr, &v4, sizeof(v4));
		return true;
	}

	if (host && inet_pton(AF_INET6, host, &v6) == 1) {
		a->ipver = 6;
		memcpy(a->addr, &v6, sizeof(v6));
		return true;
	}

	return false;
}

/*
 * The far end's own RTCP address (RFC 3605 section 2.1), parsed in the family
 * its section named. A text this layer cannot parse leaves the record at zero,
 * and zero means the peer named no address: the backend then sends RTCP to the
 * media address (hal.h, hal_media.rtcp_remote). The port does not land here,
 * because remote.rtcp_port carries it either way.
 */
static void hal_rtcp_addr_set(struct hal_rtcp_addr *a, const char *host,
			      uint8_t ipver)
{
	struct in_addr v4;
	struct in6_addr v6;

	memset(a, 0, sizeof(*a));

	if (ipver == 4 && inet_pton(AF_INET, host, &v4) == 1) {
		a->ipver = 4;
		memcpy(a->addr, &v4, sizeof(v4));
		return;
	}

	if (ipver == 6 && inet_pton(AF_INET6, host, &v6) == 1) {
		a->ipver = 6;
		memcpy(a->addr, &v6, sizeof(v6));
	}
}

static enum hal_media_dir media_dir_of(enum sdp_dir dir)
{
	switch (dir) {
	case SDP_SENDRECV:
		return HAL_MEDIA_SENDRECV;
	case SDP_SENDONLY:
		return HAL_MEDIA_SENDONLY;
	case SDP_RECVONLY:
		return HAL_MEDIA_RECVONLY;
	case SDP_INACTIVE:
		return HAL_MEDIA_INACTIVE;
	}

	return HAL_MEDIA_SENDRECV;
}

/*
 * RFC 7587 section 6.1: maxplaybackrate is the highest sampling rate the peer
 * can render, and an encoder SHOULD NOT encode frequencies above it. The rate
 * therefore picks the widest Opus bandwidth that fits under it; a peer that
 * named none leaves the encoder on the library default.
 *
 * The vocabulary is hal.h's (enum hal_opus_bw), so this layer keeps no
 * copy of the numbers the backend reads.
 */
static uint8_t opus_bw_of(uint32_t maxplaybackrate)
{
	if (!maxplaybackrate)
		return HAL_OPUS_BW_AUTO;
	if (maxplaybackrate < 12000)
		return HAL_OPUS_BW_NB;
	if (maxplaybackrate < 16000)
		return HAL_OPUS_BW_MB;

	return HAL_OPUS_BW_WB;
}

/*
 * The Opus encoder of this exchange. The configured ask rides the negotiated
 * codec entry and the peer's a=fmtp is its decoder speaking, so every key it
 * named narrows the ask and a key it left out changes nothing.
 *
 * maxaveragebitrate caps our bitrate and replaces a configured 0, because 0
 * means the library default (hal.h) and a default is free to run above the cap
 * the peer stated. useinbandfec=0 says the peer's decoder does not use in-band
 * FEC, so sending it would only spend bitrate. usedtx is neither read nor
 * written: no uci, HAL or media field exposes DTX until the jitter buffer
 * handles DTX gaps.
 *
 * Silence suppression is forced off here and comfort noise is not. The vad
 * flag is cfg-derived and has no token on the wire, so this is its only gate.
 * The cng flag follows the negotiated cn_pt, which sdp.c already withholds
 * from every Opus exchange, and a second clear here would hide a defect in
 * that layer rather than fix one.
 */
static void opus_media_fill(const struct sdp_result *res, struct hal_media *out)
{
	const struct sdp_opus_fmtp *rem = &res->opus_remote;

	out->vad = false;
	out->opus.bitrate = res->codec.opus.bitrate;
	out->opus.complexity = res->codec.opus.complexity;
	out->opus.fec = res->codec.opus.fec;
	out->opus.max_bandwidth = opus_bw_of(rem->maxplaybackrate);

	if (rem->maxaveragebitrate &&
	    (!out->opus.bitrate || rem->maxaveragebitrate < out->opus.bitrate))
		out->opus.bitrate = rem->maxaveragebitrate;
	if (rem->fec_present && !rem->fec)
		out->opus.fec = false;
}

enum hal_dtmf_mode media_dtmf_of(enum cfg_dtmf_mode mode)
{
	switch (mode) {
	case CFG_DTMF_RFC2833:
		return HAL_DTMF_RFC2833;
	case CFG_DTMF_INFO:
		return HAL_DTMF_INFO;
	case CFG_DTMF_INBAND:
		break;
	}

	return HAL_DTMF_INBAND;
}

/*
 * The suite crosses the two layers by value (shared/sdp.h: "The values equal
 * enum hal_srtp_suite and media_fill.c maps them by value"). Neither header
 * includes the other, so this is the only place the contract can be checked,
 * and a value that moved on either side would otherwise key a stream with the
 * wrong suite and no error anywhere.
 */
_Static_assert((int)HAL_SRTP_NONE == (int)SDP_SRTP_NONE,
	       "the SDP and HAL SRTP suite enums must agree by value");
_Static_assert((int)HAL_SRTP_AES_CM_128_SHA1_80 == (int)SDP_SRTP_AES_CM_128_SHA1_80,
	       "the SDP and HAL SRTP suite enums must agree by value");
_Static_assert((int)HAL_SRTP_AES_CM_128_SHA1_32 == (int)SDP_SRTP_AES_CM_128_SHA1_32,
	       "the SDP and HAL SRTP suite enums must agree by value");

/* both records carry the RFC 4568 6.1 concatenation, so the copies below are
 * whole-key copies from both ends */
_Static_assert(sizeof(((struct hal_media *)0)->srtp.key_tx) == SDP_SRTP_KEY_LEN,
	       "the HAL and SDP master key widths must agree");
_Static_assert(sizeof(((struct hal_media *)0)->srtp.key_rx) == SDP_SRTP_KEY_LEN,
	       "the HAL and SDP master key widths must agree");

/*
 * The uci options carry a TOS byte (rtp_tos 184) and the driver takes a DSCP,
 * so both fields shift (hal.h, struct hal_media). A telephone-event payload
 * type the peer did not negotiate downgrades the mode to inband, because a
 * relay with no payload type sends nothing.
 */
void media_fill(const struct cfg_channel *ch, bool rtcp_enable,
		unsigned int rtcp_interval_s, bool rtcp_addr,
		const struct sdp_result *res, const char *local_addr,
		uint16_t local_port, const char *bind_dev,
		uint8_t fax_kind, uint16_t punch_interval_ms,
		struct hal_media *out)
{
	memset(out, 0, sizeof(*out));
	if (!ch || !res)
		return;

	out->codec = (uint8_t)media_codec_of((enum sdp_codec_id)res->codec.id);
	out->codec_pt = res->codec.pt;
	out->dtmf_mode = (uint8_t)media_dtmf_of(ch->dtmf_mode);
	if (res->dtmf_pt >= 0) {
		out->dtmf_pt = (uint8_t)res->dtmf_pt;
	} else {
		out->dtmf_pt = 0;
		if (out->dtmf_mode == HAL_DTMF_RFC2833)
			out->dtmf_mode = HAL_DTMF_INBAND;
	}

	out->direction = (uint8_t)media_dir_of((enum sdp_dir)res->dir);
	out->ptime_ms = res->ptime_ms ? res->ptime_ms : 20;
	out->vad = res->codec.vad;
	out->cng = res->cn_pt >= 0;
	/* The negotiated Annex B result, not a configured ask like vad above
	 * (hal.h, hal_media.annexb): sdp.c arms it only when the codec section
	 * asked for silence suppression and the peer confirmed Annex B. */
	out->annexb = res->codec.annexb;
	/* G.729 suppresses silence through Annex B or not at all. Its decoder
	 * is backward adaptive, so the generic detector, with or without RFC
	 * 3389 comfort noise, leaves the far end out of step with the encoder
	 * at every talkspurt. The cng answer stands, because it also says what
	 * this side accepts. */
	if (out->codec == HAL_CODEC_G729 && !out->annexb)
		out->vad = false;

	out->jb_mode = HAL_JB_ADAPTIVE;
	out->jb_init_ms = (uint16_t)ch->jitter_target;
	out->jb_max_ms = (uint16_t)ch->jitter_max;

	/*
	 * The fax override (media_fill.h). It sits after the four fields it
	 * replaces rather than around them, so the negotiated values stay
	 * readable above and the override is one block that says what a fax
	 * needs. The buffer sizes are not touched: those are the ones the call
	 * negotiated and the backend edits the mode alone for the same reason.
	 *
	 * The kind rides with them (hal.h, hal_media.passthrough). It is the
	 * one field of the four a backend reads for the canceller policy, and
	 * it is written on every description so a backend keeps the policy
	 * across the re-INVITE that the upspeed itself sends.
	 */
	out->passthrough = fax_kind;
	if (fax_kind != HAL_PASSTHROUGH_OFF) {
		out->jb_mode = HAL_JB_FIXED;
		out->vad = false;
		out->cng = false;
		out->annexb = false;
	}

	/*
	 * The T.38 arm. The codec is the whole mode key (hal.h): a
	 * stream programmed with HAL_CODEC_T38 carries UDPTL and reads the block
	 * below, and the backend ignores the jitter, payload type, telephone
	 * event and suppression fields in that mode. The fax override above may
	 * therefore stand exactly as it is, and a leg that upgraded from a
	 * passthrough call keeps one description for both phases.
	 */
	if (res->t38) {
		out->codec = HAL_CODEC_T38;
		out->t38.max_bit_rate = res->t38_max_bit_rate;
		out->t38.max_datagram = res->t38_max_datagram;
		out->t38.ec = (uint8_t)media_t38_ec_of((enum sdp_t38_ec)res->t38_ec);
	}

	/*
	 * It reads the codec field and not res->codec.id, so a T.38 arm above
	 * takes this block out of the picture the way it takes the SRTP one
	 * out: an image stream carries neither an encoder nor a payload type.
	 */
	if (out->codec == HAL_CODEC_OPUS)
		opus_media_fill(res, out);

	/*
	 * The SDES key material of this exchange (hal.h, the srtp block).
	 * key_tx protects what this stream sends and key_rx opens what it
	 * receives, exactly as the negotiation named them.
	 *
	 * A T.38 stream carries none of it. The image path is plain UDPTL on
	 * this board, so an upgraded leg would drop the encryption the call
	 * negotiated and say nothing about it: the block keeps the memset zero,
	 * which is HAL_SRTP_NONE and two zero keys. It is read behind the arm
	 * above rather than off res->t38, so the one field that decides the
	 * stream mode also decides this. The T.38 upspeed is gated off on an
	 * SRTP leg; nothing in the HAL enforces it.
	 */
	if (out->codec != HAL_CODEC_T38) {
		out->srtp.suite = res->srtp_suite;
		memcpy(out->srtp.key_tx, res->srtp_key_tx, sizeof(out->srtp.key_tx));
		memcpy(out->srtp.key_rx, res->srtp_key_rx, sizeof(out->srtp.key_rx));
	}

	out->rtcp_enable = rtcp_enable;
	/* RFC 3550 floors it at 5 s; hal.h gives a zero interval no meaning. */
	if (rtcp_enable && !rtcp_interval_s)
		rtcp_interval_s = MEDIA_RTCP_INTERVAL_DEFAULT;
	if (rtcp_interval_s > MEDIA_RTCP_INTERVAL_MAX)
		rtcp_interval_s = MEDIA_RTCP_INTERVAL_MAX;
	if (rtcp_interval_s)
		out->rtcp_interval_ms = (uint16_t)(rtcp_interval_s * 1000);

	out->rtp_dscp = (uint8_t)(ch->rtp_tos >> 2);
	out->rtcp_dscp = (uint8_t)(ch->rtcp_tos >> 2);
	str_copy(out->bind_dev, sizeof(out->bind_dev), bind_dev);
	out->punch_interval_ms = punch_interval_ms;

	/*
	 * Our own address has no refusal to report: the transport hands up a
	 * literal, net_port_alloc() hands up an even port, and an all zero
	 * local address is a wildcard bind, which is a legitimate one. The hint
	 * is 4 for that bind alone, because a literal names its own family
	 * here. The outcome is read for the remote address alone, and a refused
	 * remote takes the whole description with it: this function has no
	 * return of its own (media_fill.h), so the empty description is what
	 * carries the refusal to media_program().
	 */
	hal_addr_set(&out->local, local_addr, local_port, 4);
	if (!hal_addr_set(&out->remote, res->remote_addr, res->remote_port,
			  res->remote_ipver)) {
		memset(out, 0, sizeof(*out));
		return;
	}

	/*
	 * The peer's own RTCP port (RFC 3605 section 2.1). Every backend reads
	 * it, so it travels whatever the feature bits say; a section that named
	 * none leaves the RTP port plus one that hal_addr_set() derived.
	 */
	if (res->rtcp_port)
		out->remote.rtcp_port = res->rtcp_port;

	/*
	 * The address of that attribute reaches a backend which asked for it.
	 * Without HAL_FEAT_RTCP_ADDR the field stays zero whatever the answer
	 * named, so such a backend sends RTCP to the media address.
	 */
	if (rtcp_addr && res->rtcp_ipver)
		hal_rtcp_addr_set(&out->rtcp_remote, res->rtcp_addr,
				  res->rtcp_ipver);

	/*
	 * One stream, one address family (RFC 6157 section 5). A description
	 * whose two ends disagree names a stream no socket carries, so it is
	 * emptied the way a refused remote address is and media_program() then
	 * refuses it. stream_config_set rule 4 refuses the same shape one layer
	 * down, and this side answers before the backend ever sees it.
	 */
	if (out->local.ipver != out->remote.ipver)
		memset(out, 0, sizeof(*out));
}

void media_fax_profile_build(const struct sdp_codec *use,
			     struct hal_fax_profile *fax)
{
	memset(fax, 0, sizeof(*fax));
	fax->mode = HAL_FAX_PASSTHROUGH;
	fax->codec = (uint8_t)media_codec_of((enum sdp_codec_id)use->id);
	fax->codec_pt = use->pt;
	fax->modem_fax_flag = HAL_MODEM_FAX_NONE;
	fax->ec_disable = true;
	fax->vad_disable = true;
	fax->cng_disable = true;
	fax->silence_comp_disable = true;
	fax->jb_fixed = true;
}

