// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The media helpers of a protocol that negotiates SDP (src/proto/shared/
 * media_fill.c): the local offer list a channel derives from its config and
 * the HAL codec mask, the stream description a negotiated result fills, and
 * the maps from the SDP and config vocabulary onto the HAL one. They are pure,
 * so test_call drives them without a daemon.
 */
#ifndef UVOICE_PROTO_SHARED_MEDIA_FILL_H
#define UVOICE_PROTO_SHARED_MEDIA_FILL_H

#include <stdbool.h>
#include <stdint.h>

#include "cfg/cfg.h"
#include "hal/hal.h"
#include "proto/shared/sdp.h"

/*
 * struct hal_media carries the RTCP interval in milliseconds in a uint16_t, so
 * a larger value would wrap into a much shorter interval. The schema accepts
 * seconds up to INT_MAX, so the fill caps it and the leg traces the cap at
 * every programming of the stream (media_program()).
 */
#define MEDIA_RTCP_INTERVAL_MAX	65
#define MEDIA_RTCP_INTERVAL_DEFAULT	5

/* The HAL codec of an SDP codec id, HAL_CODEC_NONE for none. */
enum hal_codec media_codec_of(enum sdp_codec_id id);

/* The payload type the channel gives an SDP codec, SDP_PT_DISABLED for
 * none. */
uint8_t media_codec_pt_of(enum sdp_codec_id id, const struct cfg_channel *ch);

/* The HAL DTMF mode of the configured one. */
enum hal_dtmf_mode media_dtmf_of(enum cfg_dtmf_mode mode);

/*
 * The cfg to sdp_local derivation. codec_mask filters against the HAL
 * capability the leg start reads from calls_hal_codecs_set; tests pass ~0u.
 */
void media_sdp_local_build(const struct cfg *base,
			   const struct cfg_channel *ch, uint32_t codec_mask,
			   const char *addr, uint16_t port,
			   struct sdp_local *out);

/*
 * The hal_media fill. The rtcp fields travel by value, because a leg must
 * never read the cfg after start.
 *
 * fax_kind is the leg's own fax state (enum hal_passthrough), and every kind
 * but HAL_PASSTHROUGH_OFF overrides the negotiation for the four fields a fax
 * carrier cannot survive: an adaptive jitter buffer re-times the carrier while
 * the page runs, a VAD gate cuts the quiet parts of that page, comfort
 * noise replaces them with something the receiver decodes as line noise, and
 * the codec's own DTX (hal_media.annexb) cuts them at the encoder instead. The
 * kind itself travels as a fifth field, because the backend reads it for the
 * canceller policy of the line. This fill is the one place every stream
 * programming passes through, so a re-INVITE, a session refresh or a far-end
 * hold made during a fax re-asserts the passthrough profile instead of
 * reverting the fields to whatever the SDP last agreed.
 *
 * rtcp_addr says the backend takes a separate RTCP address
 * (HAL_FEAT_RTCP_ADDR). The peer's RFC 3605 port always travels, in
 * hal_media.remote.rtcp_port, because every backend reads it; the address of
 * that attribute reaches hal_media.rtcp_remote only with the bit, so a backend
 * without it keeps sending RTCP to the media address.
 *
 * One stream runs in one address family (RFC 6157 section 5). A local and a
 * remote address of two families empty the whole description, the way a remote
 * address this layer cannot honor does, so media_program() refuses it before
 * the backend sees it.
 *
 * punch_interval_ms is the cadence of the media punch the leg took from its
 * view (account.h, acct_call_view), written to the description as it stands;
 * 0 leaves the backend its default (hal.h, hal_media.punch_interval_ms).
 */
void media_fill(const struct cfg_channel *ch, bool rtcp_enable,
		unsigned int rtcp_interval_s, bool rtcp_addr,
		const struct sdp_result *res, const char *local_addr,
		uint16_t local_port, const char *bind_dev,
		uint8_t fax_kind, uint16_t punch_interval_ms,
		struct hal_media *out);

/*
 * The G.711 passthrough profile the DSP is asked for on a fax or modem tone,
 * on the codec use. The SIP legs and the MGCP endpoints both switch a stream
 * with it.
 *
 * The four disable flags and the fixed jitter buffer are what a fax carrier
 * needs: suppression, comfort noise and silence compression cut the quiet
 * parts of the page, the canceller works on the carrier, and an adaptive
 * buffer re-times it. The V.152 fields stay untouched: this hardware clears
 * both VBD capability bits, so the manual toggle is the whole switch and
 * modem_fax_flag has no reader on this route.
 */
void media_fax_profile_build(const struct sdp_codec *use,
			     struct hal_fax_profile *fax);

#endif
