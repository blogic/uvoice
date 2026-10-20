// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The media records that a signaling protocol and the core share: the stream
 * ops the core hands to every protocol (struct proto_host media), the record
 * of one released leg that the call record takes, and the clearing classes
 * that record carries.
 *
 * It includes hal/hal.h and no other header of the tree, so the core, every
 * protocol unit and every suite read one definition. The append rule of
 * proto/proto.h applies to every record here.
 */
#ifndef UVOICE_PROTO_MEDIA_H
#define UVOICE_PROTO_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal/hal.h"

/*
 * Why a call failed, in the four clearing classes the subscriber hears: busy,
 * congestion (a fast busy), unobtainable and quiet (a far end that vanished).
 * A protocol maps its own outcomes onto them.
 */
enum proto_fail_class {
	PROTO_FAIL_BUSY = 0,
	PROTO_FAIL_CONGESTION,
	PROTO_FAIL_UNOBTAINABLE,
	PROTO_FAIL_QUIET,
};

/*
 * The class by name. The words are the FailWord vocabulary of the API schema,
 * which cdr.c writes as the "fail" key of a call record.
 */
static inline const char *proto_fail_name(enum proto_fail_class cls)
{
	switch (cls) {
	case PROTO_FAIL_BUSY:
		return "busy";
	case PROTO_FAIL_CONGESTION:
		return "congestion";
	case PROTO_FAIL_UNOBTAINABLE:
		return "unobtainable";
	case PROTO_FAIL_QUIET:
		return "quiet";
	}

	return "?";
}

/*
 * The codec by the RTP encoding name the SDP layer writes in an rtpmap
 * (sdp.c sdp_codec_name), so a call record names the codec the way the wire
 * did. A V.152 voice band data codec takes the name of the G.711 law it
 * carries. HAL_CODEC_T38 answers "t38", the word cdr.c and the status dump
 * write for an image session. HAL_CODEC_NONE and an unknown value answer
 * "none".
 */
static inline const char *hal_codec_name(enum hal_codec codec)
{
	switch (codec) {
	case HAL_CODEC_PCMU:
	case HAL_CODEC_VBD_PCMU:
		return "PCMU";
	case HAL_CODEC_PCMA:
	case HAL_CODEC_VBD_PCMA:
		return "PCMA";
	case HAL_CODEC_G729:
		return "G729";
	case HAL_CODEC_G726_32:
		return "G726-32";
	case HAL_CODEC_G722:
		return "G722";
	case HAL_CODEC_OPUS:
		return "opus";
	case HAL_CODEC_T38:
		return "t38";
	case HAL_CODEC_NONE:
		break;
	}

	return "none";
}

/*
 * Injected stream ops. The core wraps hal_backend_ops; a suite records.
 * line and stream are the HAL indexes; one leg drives one stream.
 */
struct call_media_ops {
	enum hal_status (*config_set)(void *priv, unsigned int line,
				      unsigned int stream,
				      const struct hal_media *media);
	enum hal_status (*start)(void *priv, unsigned int line,
				 unsigned int stream);
	enum hal_status (*stop)(void *priv, unsigned int line,
				unsigned int stream);
	enum hal_status (*dtmf_send)(void *priv, unsigned int line,
				     unsigned int stream, char digit,
				     uint32_t duration_ms);
	/*
	 * Take the media numbers of this stream, before it is stopped. Fire
	 * and forget: the leg neither sees the result nor waits for it,
	 * because the answer arrives from a slow HAL job long after the leg is
	 * gone.
	 */
	void (*stats_snapshot)(void *priv, unsigned int line,
			       unsigned int stream);
	/*
	 * Switch this stream to G.711 fax passthrough. The leg owns the
	 * profile, because the committed codec is what decides
	 * which G.711 the DSP keeps sending, and the core owns the backend
	 * call. It is the one media op with no SDP behind it: the exchange that
	 * tells the far end follows it, and the DSP has to be converted first.
	 */
	enum hal_status (*fax_apply)(void *priv, unsigned int line,
				     unsigned int stream,
				     const struct hal_fax_profile *fax);
	/*
	 * Drop the tone-disabler latch of this line (hal.h
	 * line_lec_latch_clear). The leg asks for it when it reads an answer
	 * tone as a false one: the latch is the line's and outlives the
	 * description, so a leg that returns to voice has to say so twice.
	 *
	 * It takes the line and no stream, because the disabler is the line's.
	 * A NULL op, and every refusal, is traced and blocks nothing: the
	 * latch then falls on the disabler's own quiet rule instead.
	 */
	enum hal_status (*lec_latch_clear)(void *priv, unsigned int line);
	/*
	 * The entropy the leg keys SDES SRTP with. The core wires it to its
	 * own entropy source, the one struct proto_host random_bytes reads.
	 *
	 * It fills len bytes of buf. False means the buffer holds no usable
	 * entropy and the caller must not use it: an implementation is free
	 * to have written part of it before it failed. The leg then fails at
	 * its start, the way a leg with no RTP port fails, because a key this
	 * side cannot trust would key the stream with whatever the buffer
	 * held.
	 *
	 * A NULL op is no SRTP at all. The leg offers plain RTP even where the
	 * config switch and the backend feature bit both ask for SRTP, and it
	 * traces that once at its start, so a caller that drives the legs
	 * directly gets a plain call instead of a refusal.
	 */
	bool (*rand_bytes)(void *priv, void *buf, size_t len);
	void *priv;
};

/*
 * What one leg was, reported once when the protocol releases it. It is the
 * source of the call record (cdr.c), keyed by the line and the stream the leg
 * ran on. It carries the facts a record needs and no verdict. The three
 * strings point into the leg and are valid for the call only.
 *
 * The clear is reported as an age, not as a time. A leg can keep its stream
 * after the call ended, while its protocol exchange drains, so
 * cleared_ago_ms is the distance from that end to this report, measured on
 * the protocol's own clock. The reader subtracts it from its own elapsed
 * time, so the two clock domains never meet. cleared is false for every other
 * ending, and then cleared_ago_ms is 0.
 */
struct proto_call_ended {
	bool inbound;
	bool answered;			/* the leg reached active */
	bool failed;			/* ended through a failure class */
	enum proto_fail_class fail_class;	/* valid when failed */
	bool media_ran;			/* a stream was started at least once */
	bool cleared;			/* a release or a transfer ended the
					   call before this report */
	uint32_t cleared_ago_ms;	/* age of that end, when cleared */
	const char *number;		/* dialed, outbound; may be "" */
	const char *remote_number;	/* caller id, inbound; may be "" */
	const char *remote_name;	/* may be "" */
	uint8_t codec;			/* enum hal_codec committed, or
					   HAL_CODEC_NONE */
	uint8_t ptime_ms;		/* committed, 0 when none */
	uint16_t rtp_port;		/* 0 when never allocated */
	/*
	 * The leg ended while the session carried T.38. codec stays
	 * HAL_CODEC_NONE for such a leg, because an image exchange negotiates
	 * no audio codec, so this flag tells a record that carried a fax
	 * apart from one that carried nothing.
	 */
	bool t38;
	/*
	 * The leg ended because a transfer took the call away. The flag does
	 * not claim that the transfer completed: it says that this leg ended
	 * for a referral and not for an ordinary reason. The record of the leg
	 * that took the call over carries the other half of the pair (struct
	 * proto_host cdr_call_correlate).
	 */
	bool transferred;
	/*
	 * How long ago this leg was answered, on the protocol's own clock, and
	 * 0 when it never was. It is an age for the reason cleared_ago_ms is
	 * one. It dates the answer of a leg whose answer no other report
	 * announced.
	 */
	uint32_t answered_ago_ms;
};

#endif
