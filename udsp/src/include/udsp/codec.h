// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * udsp codec contract, version 4. Stable contract. One instance per
 * stream and direction. G.711 A/u, G.726, G.722 and G.729 are built into
 * libudsp (dsp/); Opus joins under HAVE_OPUS. This vtable includes no codec
 * library header.
 *
 * Version. libudsp loads no codec plug-in: every codec is built in, and the
 * SONAME of the library carries the package version, so a consumer runs
 * against the libudsp it was built with. The version number therefore
 * documents the contract and does not enforce it. A change to the contract
 * raises the number, and the change costs a rebuild and nothing else.
 *
 * Rates. sample_rate is the PCM rate on the codec side of the rate
 * conversion: 8000 (G.711, G.726, G.729), 16000 (G.722), or 0 for a codec
 * that follows the line rate (Opus). clock_rate is the RTP timestamp rate:
 * 8000 for G.711/G.726/G.729/G.722 (the RFC 3551 G.722 quirk), 48000 for
 * Opus. The stream layer resamples 2:1 between the line and a codec whose
 * sample_rate differs (dsp/resamp).
 *
 * Frames. frame_ms 10 is a fixed-frame codec: encode consumes rate/100
 * samples per call, every speech frame encodes to frame_bytes, and packets
 * concatenate frames (the jitter buffer byte-slices them evenly, so a
 * variable-size speech frame is forbidden). A codec whose sid_bytes is
 * non-zero has two more encode returns. A return of sid_bytes is a SID frame:
 * the caller sends it as the packet's last frame and closes the packet, and
 * that tail is the one exception to the even byte-slicing. A return of 0 is an
 * untransmitted frame: the caller first flushes any staged speech frames, then
 * advances the media clock over the frame. frame_ms 0 with
 * UDSP_CODEC_F_PACKET is a whole-packet codec (Opus): encode consumes
 * ptime worth of samples at once and one packet is one indivisible unit, and
 * frame_bytes is 0. ptime_mask bit k-1 allows (k*10) ms packets.
 *
 * PLC and CN. plc conceals n samples; with UDSP_CODEC_F_FEC the caller may
 * pass the next packet for in-band recovery. cn synthesises RFC 3389 comfort
 * noise; UDSP_CODEC_F_NO_CN forbids pairing with CN and VAD (Opus, native
 * DTX). The generic voice detector itself runs only beside that comfort
 * noise, and the codec's own DTX replaces the pair: a description that asks
 * for the detector alone loses it (udsp.h udsp_media). A NULL plc means the
 * engine plays silence. The create parameter
 * plc_off builds an instance that keeps no concealment history at all, which
 * a stream carrying a data carrier asks for: the decoder then holds no
 * history to recover from and no delay line to flush. G.711 is the only
 * built-in codec that reads it. The other codecs ignore it, which costs
 * nothing: a passthrough stream never calls plc, and plays silence for every
 * unit it cannot decode. The caller picks the codec of a passthrough, and the
 * library accepts any built-in codec there.
 */
#ifndef UDSP_CODEC_H
#define UDSP_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One packet's payload bound for the codec and the stream staging, sized
 * with the 576-byte net ring. Every ops.max_payload is <= this, and the
 * jitter buffer's own bound (jb.h UDSP_JB_PAYLOAD_MAX) equals it. Our fmtp
 * advertises maxaveragebitrate, which bounds the average, not a VBR peak: an
 * oversize packet is counted and concealed, never truncated. */
#define UDSP_CODEC_PAYLOAD_MAX	512U

enum udsp_codec_id {
	UDSP_CODEC_NONE = 0,
	UDSP_CODEC_PCMU,
	UDSP_CODEC_PCMA,
	UDSP_CODEC_G729,
	UDSP_CODEC_G726_16,
	UDSP_CODEC_G726_24,
	UDSP_CODEC_G726_32,
	UDSP_CODEC_G726_40,
	/* 8 is the T38 selector hole (udsp.h UDSP_SEL_T38). */
	UDSP_CODEC_G722 = 9,
	UDSP_CODEC_OPUS = 10,
};

enum udsp_codec_flag {
	UDSP_CODEC_F_PACKET	= (1 << 0),	/* one indivisible unit per packet */
	UDSP_CODEC_F_PLC	= (1 << 1),	/* plc is native to the codec */
	UDSP_CODEC_F_FEC	= (1 << 2),	/* plc accepts the next packet */
	UDSP_CODEC_F_NO_CN	= (1 << 3),	/* never pair with RFC 3389 or VAD */
};

/* All zero selects the library defaults. line_rate is the line's rate for a
 * codec whose sample_rate is 0; zero means 8000 (the narrowband default), so
 * two implementations cannot diverge. The Opus members mirror
 * udsp_media.opus. */
struct udsp_codec_params {
	unsigned line_rate;		/* 0 or 8000 = narrowband; 16000 */
	uint8_t ptime_ms;		/* packet duration for F_PACKET codecs */
	uint32_t bitrate;		/* bits/s; 0 = default */
	uint8_t complexity;		/* 1..10; 0 = default */
	uint8_t max_bandwidth;		/* enum udsp_opus_bw (udsp.h) */
	bool fec;			/* arm in-band FEC on the encoder */
	/* Build the instance without concealment. A stream that carries a data
	 * carrier sets it: the decoder then keeps no concealment history at
	 * all, so it holds no delay line to flush and no loss count to recover
	 * from, and the engine plays silence for every unit the buffer cannot
	 * fill. G.711 is the only built-in codec that reads it. The others need
	 * not: a passthrough stream plays silence for every unit it cannot
	 * decode, whatever codec the caller picked. */
	bool plc_off;
	/* Run the codec's native voice detector and discontinuous transmission
	 * on the encoder: encode then returns a SID frame or an untransmitted
	 * frame in place of speech. Only a codec whose sid_bytes is non-zero
	 * reads it. */
	bool dtx;
};

struct udsp_codec;

struct udsp_codec_ops {
	enum udsp_codec_id id;
	const char *name;
	unsigned sample_rate;		/* PCM rate on the codec side; 0 = line rate */
	unsigned clock_rate;		/* RTP timestamp rate */
	unsigned frame_ms;		/* 10 = fixed frames; 0 = whole packet */
	uint8_t ptime_mask;		/* bit k-1 = (k*10) ms packets allowed */
	unsigned max_payload;		/* bytes of one packet at the widest ptime */
	unsigned frame_bytes;		/* bytes of one speech frame; 0 = F_PACKET */
	unsigned sid_bytes;		/* SID frame bytes; 0 = no native SID frame */
	uint32_t flags;			/* enum udsp_codec_flag */

	/* p is never NULL; a zeroed struct selects the defaults. */
	struct udsp_codec *(*create)(const struct udsp_codec_params *p);
	void (*destroy)(struct udsp_codec *c);
	/* Clear history and filter state; the create-time params survive (a
	 * parameter change recreates the pair). */
	void (*reset)(struct udsp_codec *c);
	/* The instance's actual PCM rate (equals sample_rate when fixed). */
	unsigned (*rate_get)(const struct udsp_codec *c);

	/* Encode n samples (rate_get()/100 for frame_ms 10; the whole packet
	 * for F_PACKET) into out. On a fixed-frame codec the byte count
	 * identifies the kind: frame_bytes is speech, sid_bytes is a SID frame,
	 * and 0 is an untransmitted frame. Only a codec whose sid_bytes is
	 * non-zero returns the last two, and for 0 the caller flushes any
	 * staged speech frames first, then advances the media clock over it.
	 * Returns a negative errno on failure and never writes past out_cap
	 * bytes. */
	int (*encode)(struct udsp_codec *c, const int16_t *pcm, unsigned n,
		      uint8_t *out, size_t out_cap);
	/* Decode one frame or packet. Returns the sample count written,
	 * -ENOBUFS when pcm_cap cannot hold it, or another negative errno on
	 * malformed input. On a codec whose sid_bytes is non-zero, len 0
	 * decodes one untransmitted frame, the comfort-noise continuation. It
	 * returns -EAGAIN before any SID arrived, and again after reset, which
	 * clears the history and leaves the decoder no SID to continue. */
	int (*decode)(struct udsp_codec *c, const uint8_t *in, size_t len,
		      int16_t *pcm, unsigned pcm_cap);
	/* Conceal n samples; next/next_len may carry the following packet
	 * when F_FEC. Returns n or a negative errno. NULL member = the
	 * engine plays silence. */
	int (*plc)(struct udsp_codec *c, int16_t *pcm, unsigned n,
		   const uint8_t *next, size_t next_len);
	/* RFC 3389 level in -dBov. NULL for F_NO_CN codecs. */
	void (*cn)(struct udsp_codec *c, int level_dbov, int16_t *pcm, unsigned n);
};

/* The built-in codecs export their NULL-terminated ops array through this
 * descriptor, udsp_codec_builtin (dsp/dsp.h). No loader reads it, because
 * every codec links into libudsp. */
#define UDSP_CODEC_ABI_VERSION	4
struct udsp_codec_descriptor {
	uint32_t abi_version;			/* == UDSP_CODEC_ABI_VERSION */
	uint32_t struct_size;			/* == sizeof(struct udsp_codec_ops) */
	const struct udsp_codec_ops *const *ops;	/* NULL-terminated */
};

#endif
