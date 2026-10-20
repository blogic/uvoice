// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Adaptive jitter buffer for one receive stream.
 *
 * Ownership. The engine RT thread owns the object. udsp_jb_put, udsp_jb_get and
 * udsp_jb_stats never allocate, never lock, never log; one payload memcpy per
 * put is the whole copy budget.
 *
 * Time. Every instant comes from the caller: arrival_ns on put, now_ns on get,
 * both CLOCK_MONOTONIC. The buffer reads no clock of its own and uses no wall
 * time, so an NTP step cannot move a decision.
 *
 * RTP clock. ts_per_ms and frame_ts come from the stream (udsp_jb_init_cfg),
 * because the RTP timestamp rate is a property of the codec: 8000 for G.711,
 * G.726, G.722 and G.729, 48000 for Opus (codec.h Rates). Every bound below
 * that reads as a time is stated in milliseconds and converted through
 * ts_per_ms, so the same policy holds at any clock.
 *
 * Granularity. One get returns one 10 ms output unit, which is what
 * udsp_codec_ops decode/plc/cn consume (codec.h Frames: a fixed-frame codec
 * decodes rate/100 samples per 10 ms unit). A packet carrying 20 or 30 ms is
 * handed out as 2 or 3 successive slices of its own slot, so ptime never
 * reaches the codec layer and the caller runs one codec call per engine tick.
 *
 * Units. A fixed-frame stream slices the payload by bytes, which is what
 * codec.h Frames lets it do: every frame of such a codec encodes to the same
 * byte count. A stream that states that count (frame_bytes through
 * udsp_jb_init_cfg) also stamps a payload of N whole frames by its own shape
 * instead of the learned packet span, so a packet below the learned ptime,
 * the close of a talkspurt on an odd frame count, is stamped and sliced
 * whole; the learned span serves a whole_packet stream and a payload of no
 * such shape. A whole_packet stream (Opus) cannot slice: one packet is one
 * indivisible unit. The buffer hands the entire payload out on the first slice
 * of the unit and a marker with no payload on the slices after it. Every frame
 * output carries unit_ts, slice and slices, so the caller decodes once per
 * unit_ts into its own staging and plays one 10 ms slice per tick.
 *
 * SID. A stream whose codec carries a native SID frame states sid_len beside
 * frame_bytes through udsp_jb_init_cfg. A payload of the SID length, or of N
 * speech frames plus the SID length, ends with a SID (codec.h Frames: the SID
 * is the last frame of the packet), so its span is derived from the payload
 * instead of the learned packet span and it slices as N speech slices and one
 * SID slice. Emitting the SID slice arms codec-sourced comfort noise: the
 * following DTX gap plays UDSP_JB_CN with cn_sid set (len 0, the
 * continuation) instead of loss concealment. A SID-only packet arriving while
 * that state is armed refreshes the noise description in place; only a
 * speech-bearing packet moves the playout clock. frame_bytes and sid_len 0
 * keep every SID branch dead.
 *
 * Playout clock. The media clock advances one frame per get, two on a get that
 * carries skew_drop, and none on a get that carries skew_insert or
 * clock_parked. The two markers are distinct: skew_insert is the clock-skew
 * correction, counted in stats.skew_inserts; clock_parked is the concealment
 * emitted while the queue is dry and the clock waits for the far end, counted
 * in stats.plc, cn or silence like any other concealment.
 *
 * Talkspurt. RFC 3551 4.1: the RTP marker bit on an audio packet says the
 * packet starts a talkspurt after a silence period the sender did not
 * transmit. A hole whose far side is already queued and carries that bit is
 * therefore not lost audio; the sender sent nothing there on purpose. The get
 * moves the clock straight to the marked packet and plays it, so the silence
 * costs no concealment frame and no underrun, and the get that does it carries
 * talkspurt. A hole the queue does not yet cover is a different thing: the
 * clock parks and conceals, and stats.underruns counts it,
 * because the buffer really did run out of audio to play. A marker on a packet
 * that continues the timeline changes nothing.
 *
 * Payload lifetime. out.payload stays valid until the next udsp_jb_get on the
 * same buffer, whatever udsp_jb_put does meanwhile: the emitted slot is loaned
 * to the caller and returns to the free pool only at that next get, and comfort
 * noise is emitted from a staging copy that put never touches.
 *
 * Skipped opportunities. An engine that ran no tick for some periods
 * (engine.h engine_hooks.skip) leaves one get per period open, because the
 * far end kept sending through them. udsp_jb_skip_get pays one of them: the
 * clock moves as that get moves it, and out is what that get plays, for a
 * caller that decodes and discards it to keep its decoder in step.
 * udsp_jb_skip pays a run at once and hands nothing out. Neither counts an
 * output or an underrun, updates the target or runs a clock-skew correction.
 * In prefill the queue passes only the units it holds above the target,
 * because a get waits below it. The caller's now_ns is its own stamp, later
 * than every arrival it put. It moves the buffer's time, and no opportunity
 * gets an instant of its own. udsp_jb_skip_end clears the pending
 * corrections and seeds the depth history with the depth the skip left.
 * udsp_jb_timeline tells a caller whether a put replaced the timeline that
 * the opportunities belonged to.
 *
 * Event mapping. UDSP_EV_JB_UPDATE (event.h) is filled from udsp_jb_stats:
 * u.jb.total = frames_out + plc + cn + silence + skew_inserts (the five
 * counters partition the output units), u.jb.drop = late_drop + dup_drop +
 * overflow_drop + hold_drop, u.jb.size_ms = target_ms, u.jb.jitter_avg =
 * jitter_ms. Every user-visible time is in milliseconds.
 */
#ifndef UDSP_JB_H
#define UDSP_JB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rtp.h"

#define UDSP_JB_FRAME_MS	10U
/* The narrowband defaults of udsp_jb_init; a stream states its own through
 * udsp_jb_init_cfg. */
#define UDSP_JB_TS_PER_MS	8U
#define UDSP_JB_FRAME_TS	80U
#define UDSP_JB_PTIME_MAX_MS	30U		/* udsp.h clamps ptime to 30 */
#define UDSP_JB_DEPTH_MAX_MS	500U
/* One packet of the widest codec, the codec.h bound. */
#define UDSP_JB_PAYLOAD_MAX	512U
#define UDSP_JB_CN_MAX		32U		/* RFC 3389 level plus coefficients */
#define UDSP_JB_WINDOW_MS	2000U		/* jitter high-water window */
#define UDSP_JB_WINDOW_BUCKETS	8U
#define UDSP_JB_TREND_BUCKETS	16U		/* 4 s of depth history */

/* 500 ms at one 10 ms frame per slot, plus 14 slots of reorder headroom. */
#define UDSP_JB_SLOTS		64U

enum udsp_jb_decision {
	UDSP_JB_FRAME = 0,
	UDSP_JB_PLC,
	UDSP_JB_CN,
	UDSP_JB_SILENCE,
};

/* Telephone-event payloads are consumed by telev.c before the buffer (rtp.h),
 * so only RTP_PAYLOAD_AUDIO and RTP_PAYLOAD_COMFORT_NOISE reach udsp_jb_put. */
struct udsp_jb_packet {
	const uint8_t *payload;
	size_t len;
	uint64_t arrival_ns;
	uint32_t ssrc;
	uint32_t rtp_timestamp;
	uint16_t seq;
	enum rtp_payload_kind kind;
	/* The RTP marker bit as rtp.c parsed it (Talkspurt above). A caller
	 * that leaves it false gets the unmarked rules for every packet: no
	 * talkspurt step, and a hole conceals as lost audio. */
	bool marker;
	/* This packet was parsed from the tail of a datagram that carried more
	 * than one RTP packet (RFC 3550 11), so arrival_ns is the datagram's
	 * stamp, not a network arrival of its own. The jitter estimator skips
	 * it and keeps the datagram head as the arrival reference: the next
	 * head then measures a whole datagram interval against a whole
	 * timestamp step, instead of reading the bundling as transit jitter
	 * of one packet span per tail. A caller that leaves it false gets the
	 * RFC 3550 A.8 arithmetic on every packet. */
	bool tail;
};

struct udsp_jb_out {
	const uint8_t *payload;
	size_t len;
	uint32_t rtp_timestamp;
	/* UDSP_JB_FRAME only: the packet this slice belongs to, its position in
	 * that packet and the packet's slice count. A whole_packet stream sees
	 * the payload on the first emitted slice of a unit and NULL after it,
	 * so one decode per unit_ts is the whole decode budget. */
	uint32_t unit_ts;
	uint8_t slice;
	uint8_t slices;
	enum udsp_jb_decision decision;
	bool skew_drop;			/* one media frame skipped */
	bool skew_insert;		/* media clock held for one frame */
	bool clock_parked;		/* queue dry: media clock waits for the far end */
	/* Talkspurt: this get stepped the clock over a silence the sender
	 * suppressed. The emitted timestamp is the marked packet's, not the
	 * previous frame's plus one, so a caller that tracks continuity reads
	 * the jump here instead of counting it as an error. */
	bool talkspurt;
	/* UDSP_JB_CN only: the comfort noise comes from a codec SID frame,
	 * not from RFC 3389, so the consumer routes it to the codec's own
	 * decode (len 0 is the continuation) and never dispatches on the
	 * payload length. The buffer sets it once the stream configures a
	 * SID length; until then every get leaves it false. */
	bool cn_sid;
};

struct udsp_jb_slot {
	uint32_t ts;
	uint16_t seq;
	uint16_t len;
	uint16_t span_ts;		/* learned stream span, frozen at put time */
	bool marker;			/* this packet starts a talkspurt */
	uint8_t payload[UDSP_JB_PAYLOAD_MAX];
};

struct udsp_jb {
	uint8_t mode;			/* enum udsp_jb_mode */
	uint8_t state;
	uint8_t skew_pending;
	uint8_t window_head;
	uint16_t init_ms;
	uint16_t max_ms;
	uint16_t ts_per_ms;		/* the stream's RTP clock, per ms */
	uint16_t frame_ts;		/* one 10 ms output unit, in ticks */
	uint16_t target_ms;
	uint16_t late_want_ms;
	uint16_t pkt_span_ts;
	uint16_t span_candidate_ts;
	uint16_t conceal_run_ms;
	uint16_t seq_high;
	uint16_t step_seq;		/* sequence of the step candidate */
	uint16_t cn_len;
	uint16_t frame_bytes;		/* one speech frame; 0 = no SID slicing */
	uint16_t sid_len;		/* the codec's SID frame; 0 = no SID slicing */
	uint16_t cn_run_ms;		/* free-running SID CNG since the last arrival */
	uint8_t span_agree;
	uint8_t step_agree;		/* one step observation is pending */
	uint8_t hold_run;		/* deferred anchor: consecutive refusals
					 * of a far-ahead refill on a parked clock */
	uint8_t loan;			/* slot on loan to the caller, or UDSP_JB_SLOTS */
	bool span_learned;
	bool whole_packet;		/* one packet is one indivisible unit */
	bool emit_unit_valid;
	bool cn_valid;
	bool cn_armed;
	bool cn_sid;			/* the CN slot holds a codec SID, not RFC 3389 */
	bool seq_high_sid;		/* the seq_high packet ended with a SID */
	bool trim_pending;		/* the pending drop run is the trim's */
	bool have_seq;
	bool have_arrival;
	bool skipping;			/* the get pays a skipped opportunity */

	uint32_t ssrc;
	uint32_t playout_ts;
	uint32_t last_rx_ts;
	uint32_t seq_high_ts;		/* the seq_high packet's timestamp, the
					 * span learner's pair reference under reorder */
	uint32_t cn_ts;
	uint32_t emit_unit_ts;		/* the last emitted unit's timestamp */
	uint32_t step_ts;		/* expected next ts of a step candidate */
	uint32_t stream_end_ts;		/* end of the newest accepted audio */
	uint32_t jitter_q4;
	uint32_t depth_avg_q8;
	/* Skipped opportunities (above): the timeline generation, which every
	 * anchor moves; the opportunities that moved the clock, the queued
	 * audio among them, and the ones that moved nothing. No stats field
	 * carries them. */
	uint32_t timeline;
	uint32_t skip_units;
	uint32_t skip_frames;
	uint32_t skip_waits;
	uint32_t skip_steps;		/* the single steps of every run */
	uint32_t window_peak[UDSP_JB_WINDOW_BUCKETS];
	uint32_t depth_hist_q8[UDSP_JB_TREND_BUCKETS];

	uint64_t seq_mask;
	uint64_t now_ns;
	uint64_t anchor_ns;
	uint64_t fill_ns;
	uint64_t window_ns;
	uint64_t last_arrival_ns;
	uint64_t last_skew_ns;
	uint64_t shrink_ns;
	uint64_t trim_calm_ns;		/* the clean-and-in-excess hold began here */

	struct udsp_jb_stats total;

	uint8_t count;
	uint8_t pool_count;
	uint8_t order[UDSP_JB_SLOTS];
	uint8_t pool[UDSP_JB_SLOTS];
	uint8_t cn_payload[UDSP_JB_CN_MAX];
	uint8_t cn_out[UDSP_JB_CN_MAX];
	/* The last SID a run passed (udsp_jb_skip_sid). */
	uint8_t skip_sid[UDSP_JB_CN_MAX];
	uint8_t skip_sid_len;
	struct udsp_jb_slot slot[UDSP_JB_SLOTS];
};

_Static_assert(sizeof(struct udsp_jb) <= 65536,
	       "udsp_jb outgrew its 64 KiB budget");

/* Full reset, and the reset path the stream owner takes on every media
 * reconfiguration: a re-INVITE that changes ptime or codec re-inits the buffer,
 * so the span learner adopts the new ptime from the first packet pair instead
 * of confirming it (an unsignalled mid-stream change still needs the
 * confirmation). */
void udsp_jb_init_cfg(struct udsp_jb *jb, enum udsp_jb_mode mode,
		      uint16_t init_ms, uint16_t max_ms, uint16_t ts_per_ms,
		      uint16_t frame_ts, uint16_t frame_bytes, uint16_t sid_len,
		      bool whole_packet);
/* The narrowband wrapper: 8 kHz, one 80-tick frame, byte slicing, no SID. */
void udsp_jb_init(struct udsp_jb *jb, enum udsp_jb_mode mode, uint16_t init_ms,
		  uint16_t max_ms);
/* The mode alone moves, and the buffer keeps everything else: the queue, the
 * learned span, the playout clock and the counters. UDSP_JB_FIXED pins the
 * target where the buffer already stands, because init_ms is what the target
 * follows in that mode. The stream owner calls this where a description
 * arrives that changes the mode and nothing the slicing rule depends on, so
 * the audio already queued still plays. An unknown mode changes nothing. */
void udsp_jb_mode_set(struct udsp_jb *jb, enum udsp_jb_mode mode);
/* 0 accepted, -ETIME late or held (a far-ahead packet refused while the
 * clock is parked, counted in hold_drop rather than late_drop), -EEXIST
 * duplicate, -ENOBUFS or -EMSGSIZE overflow, -EINVAL malformed. Every
 * rejected packet is counted in the matching total; the -EINVAL argument
 * errors have no counter, and stream.c cannot produce them. */
int udsp_jb_put(struct udsp_jb *jb, const struct udsp_jb_packet *pkt);
/* One 10 ms output unit per call. 0 on success, -EINVAL on a bad argument. */
int udsp_jb_get(struct udsp_jb *jb, uint64_t now_ns, struct udsp_jb_out *out);
void udsp_jb_stats(const struct udsp_jb *jb, struct udsp_jb_stats *out);
/* The timeline generation: the first packet and every reset (SSRC change,
 * timestamp jump, starve resync, step confirm) move it. A refill after an
 * outage continues the timeline and leaves it. */
uint32_t udsp_jb_timeline(const struct udsp_jb *jb);
/* One skipped opportunity (Skipped opportunities above), out as a get fills
 * it. 0 on success, -EINVAL on a bad argument. */
int udsp_jb_skip_get(struct udsp_jb *jb, uint64_t now_ns,
		     struct udsp_jb_out *out);
/* units skipped opportunities, with no output, which leaves the buffer that
 * units calls of udsp_jb_skip_get leave. Returns the queued audio units among
 * them. The work is bounded by the queued slices, the gaps between them and
 * the talkspurt steps, not by units: a run that the queue cannot shorten, and
 * a gap before a queued head, are paid at once. A gap stops where a marked
 * head becomes due or a prefill starts to wait. The next unit the clock
 * enters goes out whole, so a whole-packet caller decodes it fresh. */
uint32_t udsp_jb_skip(struct udsp_jb *jb, uint32_t units, uint64_t now_ns);
/* The last SID the latest udsp_jb_skip passed to no decoder: a stored update
 * or the SID slice of a packet. 0 when it passed none. A caller that restarts
 * its decoder after the run decodes it first, so the comfort noise that the
 * buffer plays next finds a SID. *sid stays valid until the next run. */
size_t udsp_jb_skip_sid(const struct udsp_jb *jb, const uint8_t **sid);
/* The end of a skip: the pending clock-skew or trim run is cleared, the
 * trim's clean window starts over, and a playing buffer seeds its depth
 * history with its depth, so neither correction reads the skip as drift. */
void udsp_jb_skip_end(struct udsp_jb *jb);

#endif
