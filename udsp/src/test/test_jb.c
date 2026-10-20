// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Jitter buffer simulation suite. Everything is deterministic. The suite uses a
 * virtual CLOCK_MONOTONIC in nanoseconds, a fixed-seed PRNG and an ordered
 * delivery list that models the socket. No sleep and no real clock appear
 * anywhere.
 *
 * Payload check. Byte n of a packet holds the frame index (RTP timestamp /
 * frame_ts, modulo 256) of the 10 ms slice that it belongs to. A wrong slice
 * offset, a wrong slice length or a wrong slot is therefore visible in the
 * first output byte.
 *
 * Clock. sim_reset runs the narrowband stream that udsp_jb_init states: 8 kHz,
 * an 80-tick frame and byte slicing. Every narrowband case therefore keeps its
 * numbers. sim_reset_cfg states a clock and the whole-packet rule for the
 * 48 kHz cases.
 *
 * Talkspurts. sim_silence advances the timestamp of the sender and consumes no
 * sequence number, as a far end that suppresses silence does on the wire.
 * sim_send_marked raises the RTP marker on the packet that ends the silence.
 * A field capture carries exactly that shape: 35 gaps, each
 * closed by a marked packet, and no marker anywhere else.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../rtp/jb.h"
#include "util.h"

#define SIM_QUEUE		256U
#define TICK_NS			(UDSP_JB_FRAME_MS * 1000000ULL)
#define MS_NS			1000000ULL
/* jb.c JB_PLC_MAX_MS expressed in output units. */
#define JB_PLC_LIMIT_FRAMES	40U

struct sim_pkt {
	uint64_t at_ns;
	uint32_t serial;
	uint32_t ts;
	uint32_t ssrc;
	uint16_t seq;
	uint16_t len;
	uint8_t kind;
	bool used;
	bool marker;
	bool tail;
	uint8_t data[UDSP_JB_PAYLOAD_MAX];
};

struct sim {
	struct udsp_jb jb;
	struct sim_pkt queue[SIM_QUEUE];
	uint32_t serial;
	uint64_t now_ns;
	uint32_t ssrc;
	uint32_t ts;
	uint16_t seq;
	unsigned span_ms;
	unsigned bpf;
	unsigned ts_per_ms;
	unsigned frame_ts;
	unsigned sid_len;
	bool whole;
	uint32_t unit_ts;
	bool unit_valid;
	unsigned unit_payloads;
	unsigned unit_repeats;
	unsigned unit_errors;
	unsigned accepted, late, dup, over, other;
	/* rtp.c receiver_jitter_update replica: the CDR jitter_max is the peak of
	 * this smoothed estimate, so a hypothesis is fitted against it here. */
	uint32_t rtp_jitter_q4;
	uint32_t rtp_jitter_max_ms;
	int32_t rtp_transit;
	bool rtp_transit_valid;
	uint32_t expect_ts;
	bool expect_valid;
	bool check_order;
	bool stalled;
	bool prefill_seen;
	unsigned order_errors;
	unsigned payload_errors;
	unsigned account_errors;
	unsigned talkspurts;
	bool mark_next;
	bool tail_next;
	uint64_t last_reanchors;
};

static struct sim sim;
static uint32_t rng_state;

static uint32_t rng_next(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static void sim_common(enum udsp_jb_mode mode, unsigned span_ms, unsigned bpf,
		       unsigned ts_per_ms, bool whole)
{
	sim.ssrc = 0x11223344U;
	sim.ts = 160000U;
	sim.seq = 1000U;
	sim.span_ms = span_ms;
	sim.bpf = bpf;
	sim.ts_per_ms = ts_per_ms;
	sim.frame_ts = ts_per_ms * UDSP_JB_FRAME_MS;
	sim.whole = whole;
	sim.now_ns = 5000000000ULL;
	sim.check_order = mode != UDSP_JB_OFF;
	rng_state = 0x2545f491U;
}

static void sim_reset(enum udsp_jb_mode mode, uint16_t init_ms, uint16_t max_ms,
		      unsigned span_ms, unsigned bpf)
{
	memset(&sim, 0, sizeof(sim));
	udsp_jb_init(&sim.jb, mode, init_ms, max_ms);
	sim_common(mode, span_ms, bpf, RTP_CLOCK_RATE / 1000U, false);
}

/* The wideband stream form: an RTP clock of its own and, for a whole-packet
 * codec, one indivisible unit per packet. */
static void sim_reset_cfg(enum udsp_jb_mode mode, uint16_t init_ms,
			  uint16_t max_ms, unsigned span_ms, unsigned bpf,
			  unsigned ts_per_ms, bool whole)
{
	memset(&sim, 0, sizeof(sim));
	udsp_jb_init_cfg(&sim.jb, mode, init_ms, max_ms, (uint16_t)ts_per_ms,
			 (uint16_t)(ts_per_ms * UDSP_JB_FRAME_MS), 0, 0, whole);
	sim_common(mode, span_ms, bpf, ts_per_ms, whole);
}

/* The Annex B stream form: bpf is the codec's speech frame (frame_bytes) and
 * sid_len its SID frame, on the narrowband clock. */
static void sim_reset_sid(enum udsp_jb_mode mode, uint16_t init_ms,
			  uint16_t max_ms, unsigned span_ms, unsigned bpf,
			  unsigned sid_len)
{
	memset(&sim, 0, sizeof(sim));
	udsp_jb_init_cfg(&sim.jb, mode, init_ms, max_ms,
			 RTP_CLOCK_RATE / 1000U, UDSP_JB_FRAME_TS,
			 (uint16_t)bpf, (uint16_t)sid_len, false);
	sim_common(mode, span_ms, bpf, RTP_CLOCK_RATE / 1000U, false);
	sim.sid_len = sid_len;
}

static struct sim_pkt *sim_slot(void)
{
	unsigned i;

	for (i = 0; i < SIM_QUEUE; i++) {
		if (!sim.queue[i].used)
			return &sim.queue[i];
	}
	return NULL;
}

static void sim_queue(uint64_t at_ns, uint32_t ts, uint16_t seq, uint8_t kind,
		      unsigned len)
{
	struct sim_pkt *pkt = sim_slot();
	unsigned i;

	if (!pkt)
		return;
	memset(pkt, 0, sizeof(*pkt));
	pkt->at_ns = at_ns;
	pkt->serial = sim.serial++;
	pkt->ts = ts;
	pkt->ssrc = sim.ssrc;
	pkt->seq = seq;
	pkt->kind = kind;
	pkt->len = (uint16_t)len;
	pkt->used = true;
	pkt->marker = sim.mark_next;
	sim.mark_next = false;
	pkt->tail = sim.tail_next;
	sim.tail_next = false;
	for (i = 0; i < len; i++)
		pkt->data[i] = (uint8_t)((ts / sim.frame_ts + i / sim.bpf) &
					 0xffU);
}

/* One audio packet of the configured ptime, taking the next timestamp and
 * sequence number of the modeled sender. */
static void sim_send(uint64_t at_ns)
{
	sim_queue(at_ns, sim.ts, sim.seq, RTP_PAYLOAD_AUDIO,
		  sim.span_ms / UDSP_JB_FRAME_MS * sim.bpf);
	sim.ts += sim.span_ms * sim.ts_per_ms;
	sim.seq++;
}

/* The first packet of a talkspurt: RFC 3551 4.1 marker set. */
static void sim_send_marked(uint64_t at_ns)
{
	sim.mark_next = true;
	sim_send(at_ns);
}

/* Two packets of one datagram (RFC 3550 11): the head carries the wire
 * arrival, the tail shares the stamp and says so (jb.h tail). */
static void sim_send_bundled(uint64_t at_ns)
{
	sim_send(at_ns);
	sim.tail_next = true;
	sim_send(at_ns);
}

static void sim_skip(unsigned packets)
{
	while (packets--) {
		sim.ts += sim.span_ms * sim.ts_per_ms;
		sim.seq++;
	}
}

/* Media the sender suppressed rather than lost: the timestamp covers it, the
 * sequence numbers do not, which is how the field far end sends silence. */
static void sim_silence(unsigned packets)
{
	sim.ts += packets * sim.span_ms * sim.ts_per_ms;
}

/* An Annex B talkspurt close: speech_frames speech frames and the SID tail in
 * one packet. The sender's clock covers every frame of it, the SID's
 * included, and the next packet continues from there. */
static void sim_send_sid(uint64_t at_ns, unsigned speech_frames)
{
	sim_queue(at_ns, sim.ts, sim.seq, RTP_PAYLOAD_AUDIO,
		  speech_frames * sim.bpf + sim.sid_len);
	sim.ts += (speech_frames + 1U) * sim.frame_ts;
	sim.seq++;
}

/* The untransmitted frames of a DTX silence: the timestamp counts them, no
 * packet and no sequence number covers them (RFC 3551 4.5.6). */
static void sim_dtx_gap(unsigned frames)
{
	sim.ts += frames * sim.frame_ts;
}

static void sim_rtp_jitter(const struct sim_pkt *pkt)
{
	uint32_t clock = sim.ts_per_ms * 1000U;
	uint32_t arrival_ts = (uint32_t)(pkt->at_ns / 1000000000ULL * clock +
					 pkt->at_ns % 1000000000ULL * clock /
					 1000000000ULL);
	int32_t transit = (int32_t)(arrival_ts - pkt->ts);
	uint32_t magnitude;
	int32_t delta;

	if (pkt->kind != RTP_PAYLOAD_AUDIO)
		return;
	if (!sim.rtp_transit_valid) {
		sim.rtp_transit = transit;
		sim.rtp_transit_valid = true;
		return;
	}
	delta = (int32_t)((uint32_t)transit - (uint32_t)sim.rtp_transit);
	sim.rtp_transit = transit;
	magnitude = delta < 0 ? 0U - (uint32_t)delta : (uint32_t)delta;
	/* rtp.c skips a marked forward step beyond RTP_MARKED_STEP_MS */
	if (pkt->marker && delta < 0 &&
	    magnitude > sim.ts_per_ms * RTP_MARKED_STEP_MS)
		return;
	sim.rtp_jitter_q4 += magnitude - ((sim.rtp_jitter_q4 + 8U) >> 4);
	if ((sim.rtp_jitter_q4 >> 4) / sim.ts_per_ms > sim.rtp_jitter_max_ms)
		sim.rtp_jitter_max_ms = (sim.rtp_jitter_q4 >> 4) / sim.ts_per_ms;
}

static void sim_put(const struct sim_pkt *pkt)
{
	struct udsp_jb_packet in;
	int rc;

	sim_rtp_jitter(pkt);
	memset(&in, 0, sizeof(in));
	in.payload = pkt->data;
	in.len = pkt->len;
	in.arrival_ns = pkt->at_ns;
	in.ssrc = pkt->ssrc;
	in.rtp_timestamp = pkt->ts;
	in.seq = pkt->seq;
	in.kind = pkt->kind;
	in.marker = pkt->marker;
	in.tail = pkt->tail;
	rc = udsp_jb_put(&sim.jb, &in);
	if (!rc)
		sim.accepted++;
	else if (rc == -ETIME)
		sim.late++;
	else if (rc == -EEXIST)
		sim.dup++;
	else if (rc == -ENOBUFS || rc == -EMSGSIZE)
		sim.over++;
	else
		sim.other++;
}

/* Delivery in arrival order, which is what the net thread hands the engine.
 * Equal arrival times keep send order, so the model stays deterministic. */
static bool sim_earlier(const struct sim_pkt *a, const struct sim_pkt *b)
{
	if (a->at_ns != b->at_ns)
		return a->at_ns < b->at_ns;
	return a->serial < b->serial;
}

static void sim_deliver(uint64_t upto_ns)
{
	struct sim_pkt *best;
	unsigned i;

	for (;;) {
		best = NULL;
		for (i = 0; i < SIM_QUEUE; i++) {
			if (!sim.queue[i].used || sim.queue[i].at_ns > upto_ns)
				continue;
			if (!best || sim_earlier(&sim.queue[i], best))
				best = &sim.queue[i];
		}
		if (!best)
			return;
		sim_put(best);
		best->used = false;
	}
}

/* Every slot is in exactly one place: the free pool, the ordered queue, or on
 * loan to the caller; the loaned slot may still head the queue while its later
 * slices are due. A slot in none of them is leaked until the next timing
 * reset, which is a permanent capacity loss on a live stream. */
static void sim_account_verify(void)
{
	const struct udsp_jb *jb = &sim.jb;
	unsigned slots = jb->pool_count + jb->count;

	if (jb->loan < UDSP_JB_SLOTS &&
	    (!jb->count || jb->order[0] != jb->loan))
		slots++;
	if (slots != UDSP_JB_SLOTS)
		sim.account_errors++;
}

/* The output timestamp advances one frame per get, two on a skew drop and none
 * on a skew insert. A parked clock (skew_insert on a starved queue) resumes at
 * or after the parked position, never behind it. The payload oracle checks the
 * slice the buffer handed out. */
/* A whole-packet unit leaves the buffer one time only (jb.h Units). A slice
 * that names a unit the caller has not seen must carry the payload, a slice of
 * the unit it already holds must not repeat the decode, and the emitted
 * timestamp must be the unit's start plus the slice. */
static void sim_unit_verify(const struct udsp_jb_out *out)
{
	bool fresh = !sim.unit_valid || sim.unit_ts != out->unit_ts;

	if (out->decision != UDSP_JB_FRAME)
		return;
	if (fresh && !out->payload)
		sim.unit_errors++;
	if (!fresh && out->payload)
		sim.unit_repeats++;
	if (out->payload)
		sim.unit_payloads++;
	if (out->slice >= out->slices)
		sim.unit_errors++;
	if (out->rtp_timestamp != out->unit_ts + out->slice * sim.frame_ts)
		sim.unit_errors++;
	sim.unit_ts = out->unit_ts;
	sim.unit_valid = true;
}

static void sim_verify(const struct udsp_jb_out *out)
{
	uint32_t want;

	sim_account_verify();
	if (sim.whole)
		sim_unit_verify(out);

	if (out->decision == UDSP_JB_FRAME && out->len) {
		uint32_t base = sim.whole ? out->unit_ts : out->rtp_timestamp;
		size_t bytes = sim.whole ?
			       sim.span_ms / UDSP_JB_FRAME_MS * sim.bpf : sim.bpf;
		/* A SID stream's last slice is the SID tail; the cases that
		 * send one assert its exact shape themselves, so the oracle
		 * only lets that one length through. */
		bool sid_tail = sim.sid_len && out->len == sim.sid_len &&
				out->slice + 1U == out->slices;

		if (out->payload[0] != (uint8_t)((base / sim.frame_ts) & 0xffU))
			sim.payload_errors++;
		if (out->len != bytes && !sid_tail)
			sim.payload_errors++;
	}
	/* The clock steps over a silence the sender suppressed (jb.h
	 * Talkspurt), so the emitted timestamp is the marked packet's. The
	 * oracle re-seeds on it instead of reading the step as a lost frame;
	 * the size of the step is asserted by the cases that provoke it. */
	if (out->talkspurt) {
		sim.talkspurts++;
		sim.expect_ts = out->rtp_timestamp;
		sim.expect_valid = true;
	}
	if (!sim.check_order)
		return;
	if (!sim.expect_valid) {
		if (out->decision != UDSP_JB_FRAME)
			return;
		sim.expect_valid = true;
		sim.expect_ts = out->rtp_timestamp + sim.frame_ts;
		return;
	}
	if (sim.stalled) {
		/* A parked clock may resume behind the parked position when the
		 * refill pull-back re-anchored on the far end. That path always
		 * passes through PREFILL, so a backward resume without a
		 * prefill episode still counts as an ordering error. */
		if ((int32_t)(out->rtp_timestamp - sim.expect_ts) < 0 &&
		    !sim.prefill_seen)
			sim.order_errors++;
		sim.expect_ts = out->rtp_timestamp;
		if (out->clock_parked)
			return;
		sim.stalled = false;
		/* a skew insert on the resume tick holds the clock, so the
		 * expectation must not advance past it */
		if (!out->skew_insert)
			sim.expect_ts += sim.frame_ts;
		return;
	}
	want = sim.expect_ts;
	if (out->skew_drop)
		want += sim.frame_ts;
	if (out->rtp_timestamp != want)
		sim.order_errors++;
	if (out->clock_parked) {
		sim.stalled = true;
		sim.prefill_seen = false;
	}
	sim.expect_ts = out->skew_insert || out->clock_parked ?
			want : want + sim.frame_ts;
}

/* A resync, starve resync or CN re-anchor moves the playout timeline by
 * design, so the order oracle re-seeds on the emitted frame that follows.
 * This relaxes the oracle only in the cases that assert those counters (the
 * resync, starve, cn-resume and step cases): everywhere else the counters
 * stay zero, the re-seed never runs, and the strict oracle stands. Within a
 * resync case the oracle still binds every frame between two re-anchors. */
static void sim_reanchor_note(void)
{
	struct udsp_jb_stats st;
	uint64_t reanchors;

	udsp_jb_stats(&sim.jb, &st);
	reanchors = st.resyncs + st.starve_resyncs + st.cn_reanchors;
	if (reanchors == sim.last_reanchors)
		return;
	sim.last_reanchors = reanchors;
	sim.expect_valid = false;
	sim.stalled = false;
	/* A re-anchor may put the clock back on a unit the buffer already
	 * emitted, and the buffer re-arms it: the oracle must not read the
	 * fresh payload that follows as a repeat. */
	sim.unit_valid = false;
}

static void sim_tick(struct udsp_jb_out *out)
{
	sim_deliver(sim.now_ns);
	sim_reanchor_note();
	udsp_jb_get(&sim.jb, sim.now_ns, out);
	if (sim.jb.state == 1U)
		sim.prefill_seen = true;
	sim_verify(out);
	sim.now_ns += TICK_NS;
}

static void sim_stats(struct udsp_jb_stats *st)
{
	udsp_jb_stats(&sim.jb, st);
}

static void test_steady_20ms(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned frames = 0;
	unsigned conceal = 0;
	unsigned i;

	CASE("jb_steady_20ms");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 3000U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
		if (out.decision == UDSP_JB_FRAME)
			frames++;
		else if (sim.expect_valid)
			conceal++;
	}
	sim_stats(&st);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(conceal, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.dup_drop, 0);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(st.underruns, 0);
	CHECK_UINT(st.skew_inserts, 0);
	CHECK_UINT(st.frames_out, frames);
	/* The contraction itself costs no frame, but the prefill's surplus above
	 * the contracted target (the time-capped prefill parked 40 ms against a
	 * target that decayed to 10) is shed at one frame per second once it
	 * stands across the depth history. The queue then rests at target plus
	 * one span. The span floor and the standing reserve each take one frame
	 * off that surplus, so the startup shed is two drops. */
	CHECK_UINT(st.skew_drops, 2);
	CHECK_UINT(st.skew_inserts, 0);
	/* The post-startup floor is one learned span, so the target rests at 20.
	 * The settled floor is that packet span plus one frame. */
	CHECK_UINT(st.target_ms, 30);
	CHECK(st.depth_ms >= 10 && st.depth_ms <= 40);
}

static void jitter_stream(enum udsp_jb_mode mode, uint16_t init_ms,
			  uint16_t max_ms, unsigned ticks, unsigned *underrun_late)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;
	int jitter;

	sim_reset(mode, init_ms, max_ms, 20, 80);
	for (i = 0; i < ticks; i++) {
		if (!(i % 2)) {
			jitter = (int)(rng_next() % 31U) - 15;
			sim_send(sim.now_ns + (uint64_t)(jitter + 15) * MS_NS);
		}
		sim_tick(&out);
		if (i != 500U)
			continue;
		sim_stats(&st);
		*underrun_late = (unsigned)st.underruns;
	}
}

static void test_adaptive_jitter(void)
{
	struct udsp_jb_stats st;
	unsigned early = 0;

	CASE("jb_adaptive_uniform_jitter");
	jitter_stream(UDSP_JB_ADAPTIVE, 50, 100, 3000U, &early);
	sim_stats(&st);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(st.underruns, early);
	CHECK_UINT(st.underruns, 0);
	CHECK(st.target_ms >= 20 && st.target_ms <= 60);
	CHECK(st.jitter_ms >= 5 && st.jitter_ms <= 20);
	CHECK_UINT(st.overflow_drop, 0);
	/* A jittery line without clock skew must not arm a correction: the
	 * offset trim's threshold sits two frames above the setpoint, which the
	 * jitter-driven sawtooth does not clear across the whole history window
	 * inside 30 s (the 600 s jb_offset_trim_jittery case sheds three). */
	CHECK_UINT(st.skew_drops, 0);
	CHECK_UINT(st.skew_inserts, 0);
	CHECK_UINT(st.plc, 0);
}

/* A far end that bundles two 20 ms packets per 40 ms datagram on an otherwise
 * clean line. The tails carry the jb.h tail flag, so the estimator reads one
 * arrival observation per datagram. The only correct verdict is zero jitter:
 * the target must settle on the floor of a stream with one packet per
 * datagram (test_steady_20ms). */
static void test_bundled_datagram_no_jitter(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_bundled_datagram_no_jitter");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 3000U; i++) {
		if (!(i % 4))
			sim_send_bundled(sim.now_ns);
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(st.underruns, 0);
	CHECK_UINT(st.plc, 0);
	CHECK_UINT(st.jitter_ms, 0);
	CHECK_UINT(st.target_ms, 30);
}

/* The same bundles with the tail flag clear, the arithmetic that the flag
 * prevents. Every tail reads a zero arrival delta against one whole span of
 * timestamp. The estimate converges on the span, and the target triples it.
 * The case checks that failure mode. Together, the two cases fail an
 * implementation that ignores the flag in either direction. */
static void test_bundled_datagram_untagged_inflates(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_bundled_datagram_untagged_inflates");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 3000U; i++) {
		if (!(i % 4)) {
			sim_send(sim.now_ns);
			sim_send(sim.now_ns);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK(st.jitter_ms >= 15 && st.jitter_ms <= 20);
	CHECK(st.target_ms >= 55 && st.target_ms <= 60);
}

static void test_fixed_holds_target(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned mismatches = 0;
	unsigned i;
	int jitter;

	CASE("jb_fixed_holds_target");
	sim_reset(UDSP_JB_FIXED, 50, 100, 20, 80);
	for (i = 0; i < 3000U; i++) {
		if (!(i % 2)) {
			jitter = (int)(rng_next() % 31U) - 15;
			sim_send(sim.now_ns + (uint64_t)(jitter + 15) * MS_NS);
		}
		sim_tick(&out);
		sim_stats(&st);
		if (st.target_ms != 50)
			mismatches++;
	}
	CHECK_UINT(mismatches, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(st.target_ms, 50);
	CHECK_UINT(st.overflow_drop, 0);
}

/*
 * The switch that a data carrier makes. The mode moves to fixed, and the
 * target stays where the adaptive run put it, instead of going back to the
 * configured one. The run below adapts under jitter first. The two figures
 * therefore differ, and the case cannot pass on the configuration alone.
 */
static void test_mode_set_fixed_pins(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned mismatches = 0;
	uint64_t frames_before;
	uint16_t adapted;
	unsigned i;
	int jitter;

	CASE("jb_mode_set_fixed_pins_adapted_target");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 3000U; i++) {
		if (!(i % 2)) {
			jitter = (int)(rng_next() % 31U) - 15;
			sim_send(sim.now_ns + (uint64_t)(jitter + 15) * MS_NS);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	adapted = st.target_ms;
	frames_before = st.frames_out;
	CHECK(adapted > 0);
	CHECK(adapted != 50);
	/* The record names the mode the buffer runs, so a reader tells the
	 * pinned target from a configured one without the description. */
	CHECK_UINT(st.mode, UDSP_JB_ADAPTIVE);

	udsp_jb_mode_set(&sim.jb, UDSP_JB_FIXED);
	for (i = 0; i < 3000U; i++) {
		if (!(i % 2)) {
			jitter = (int)(rng_next() % 31U) - 15;
			sim_send(sim.now_ns + (uint64_t)(jitter + 15) * MS_NS);
		}
		sim_tick(&out);
		sim_stats(&st);
		if (st.target_ms != adapted)
			mismatches++;
	}
	CHECK_UINT(mismatches, 0);
	CHECK_UINT(st.target_ms, adapted);
	CHECK_UINT(st.mode, UDSP_JB_FIXED);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(st.resyncs, 0);
	/* The audio never stopped: the run after the switch played its own
	 * 1500 packets on top of what the run before it played. */
	CHECK(st.frames_out > frames_before);
	CHECK_UINT(st.underruns, 0);
}

/* The queue is the audio that the far end already sent, and a carrier is
 * demodulated from it. The switch must therefore not throw it away. */
static void test_mode_set_keeps_queue(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint32_t playout_before;
	uint8_t count_before;
	unsigned i;

	CASE("jb_mode_set_keeps_the_queue");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 400U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	count_before = sim.jb.count;
	playout_before = sim.jb.playout_ts;
	sim_stats(&st);
	CHECK(count_before > 0);

	udsp_jb_mode_set(&sim.jb, UDSP_JB_FIXED);
	CHECK_UINT(sim.jb.count, count_before);
	CHECK_UINT(sim.jb.playout_ts, playout_before);
	CHECK_UINT(sim.jb.mode, UDSP_JB_FIXED);
	CHECK(sim.jb.span_learned);
	/* The learned span and the pool are the state the next get reads, so
	 * the audio plays on with no concealed unit between the two modes. */
	for (i = 0; i < 200U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
		if (out.decision != UDSP_JB_FRAME)
			CHECK(false);
	}
	sim_stats(&st);
	CHECK_UINT(st.mode, UDSP_JB_FIXED);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.underruns, 0);
}

/*
 * The trim sheds the adaptive surplus. jb_skew_decide resumes a pending run
 * before it reaches the gate that armed the run. A run left standing would
 * therefore carry on in a mode that arms none. A trend run is not canceled,
 * and the skew corrector keeps running in the fixed mode by design.
 */
static void test_mode_set_cancels_trim(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint16_t target_at_switch;
	uint64_t drops_at_switch;
	uint64_t next;
	unsigned i;

	CASE("jb_mode_set_cancels_a_pending_trim");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	next = sim.now_ns;
	/* The sender of the offset-trim cases: a hair fast, so the queue
	 * settles above the target and the trim arms on the standing offset. */
	for (i = 0; i < 20000U && !sim.jb.trim_pending; i++) {
		while (next <= sim.now_ns) {
			sim_send(next);
			next += 19999800ULL;
		}
		sim_tick(&out);
	}
	CHECK(sim.jb.trim_pending);
	sim_stats(&st);
	drops_at_switch = st.skew_drops;
	target_at_switch = st.target_ms;

	udsp_jb_mode_set(&sim.jb, UDSP_JB_FIXED);
	CHECK(!sim.jb.trim_pending);
	/* jb.h skew_pending: 0 none, 1 drop, 2 insert. */
	CHECK_UINT(sim.jb.skew_pending, 0);
	for (i = 0; i < 2000U; i++) {
		while (next <= sim.now_ns) {
			sim_send(next);
			next += 19999800ULL;
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.skew_drops, drops_at_switch);
	CHECK_UINT(st.target_ms, target_at_switch);
	CHECK_UINT(st.underruns, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
}

static void test_off_no_added_delay(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;
	int jitter;

	CASE("jb_off_no_added_delay");
	sim_reset(UDSP_JB_OFF, 50, 100, 20, 80);
	for (i = 0; i < 3000U; i++) {
		if (!(i % 2)) {
			jitter = (int)(rng_next() % 31U) - 15;
			sim_send(sim.now_ns + (uint64_t)(jitter + 15) * MS_NS);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.mode, UDSP_JB_OFF);
	CHECK_UINT(st.target_ms, 0);
	CHECK_UINT(st.frames_out, 2995);
	CHECK_UINT(st.silence + st.plc, 5);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.dup_drop, 0);
	/* The first two packets of this stream are emitted whole: UDSP_JB_OFF
	 * has no prefill in which to learn the ptime before the first packet
	 * plays, and after that first frame the span rule makes the first pair
	 * a nomination, adopted by the second pair one packet later. An at-once
	 * adoption here is indistinguishable in-band from a comfort-noise
	 * poisoned span. A pair that comes before the first frame still adopts
	 * at once: jb_span_off_pair_before_first_frame. */
	CHECK_UINT(sim.payload_errors, 2);
}

/* A packet that arrives after a newer one played is late in every mode, and
 * UDSP_JB_OFF never waits for it. */
static void test_off_drops_late(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint32_t held_ts[5];
	uint16_t held_seq[5];
	unsigned held_at[5];
	unsigned held = 0;
	unsigned i, k;

	CASE("jb_off_drops_late_tail");
	sim_reset(UDSP_JB_OFF, 50, 100, 20, 80);
	for (i = 0; i < 200U; i++) {
		if (!(i % 2) && held < 5U && i >= 20U && !((i - 20U) % 40U)) {
			held_ts[held] = sim.ts;
			held_seq[held] = sim.seq;
			held_at[held] = i + 6U;
			held++;
			sim_skip(1);
		} else if (!(i % 2)) {
			sim_send(sim.now_ns);
		}
		for (k = 0; k < held; k++) {
			if (held_at[k] != i)
				continue;
			sim_queue(sim.now_ns, held_ts[k], held_seq[k],
				  RTP_PAYLOAD_AUDIO, 2U * sim.bpf);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.late_drop, 5);
	CHECK_UINT(sim.late, 5);
	CHECK_UINT(st.dup_drop, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK(st.frames_out > 170);
}

/* UDSP_JB_OFF has no recovery machinery. A stalled sender that resumes behind
 * the playout position therefore stays dropped. */
static void test_off_starved_late(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint32_t paused_ts = 0;
	unsigned i;

	CASE("jb_off_starved_late_still_dropped");
	sim_reset(UDSP_JB_OFF, 50, 100, 20, 80);
	for (i = 0; i < 400U; i++) {
		if (i == 100U)
			paused_ts = sim.ts;
		if (i == 300U)
			sim.ts = paused_ts - 100U * UDSP_JB_FRAME_TS;
		if (!(i % 2) && (i < 100U || i >= 300U))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.late_drop, 50);
	CHECK_UINT(sim.late, 50);
}

static void reorder_run(uint16_t target_ms, unsigned distance)
{
	struct udsp_jb_out out;
	unsigned pending = 0;
	uint64_t hold[8];
	uint32_t hold_ts[8];
	uint16_t hold_seq[8];
	unsigned i;

	sim_reset(UDSP_JB_FIXED, target_ms, 500, 20, 80);
	for (i = 0; i < 600U; i++) {
		if (i % 2) {
			sim_tick(&out);
			continue;
		}
		if (i && !((i / 2) % (distance + 1U)) && pending < 8U) {
			hold[pending] = sim.now_ns +
					(uint64_t)distance * sim.span_ms * MS_NS;
			hold_ts[pending] = sim.ts;
			hold_seq[pending] = sim.seq;
			pending++;
			sim_skip(1);
			sim_tick(&out);
			continue;
		}
		sim_send(sim.now_ns);
		while (pending && hold[pending - 1U] <= sim.now_ns) {
			pending--;
			sim_queue(sim.now_ns, hold_ts[pending], hold_seq[pending],
				  RTP_PAYLOAD_AUDIO, 2U * sim.bpf);
		}
		sim_tick(&out);
	}
}

static void test_reorder(void)
{
	struct udsp_jb_stats st;

	CASE("jb_reorder_adjacent");
	reorder_run(60, 1);
	sim_stats(&st);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.plc, 0);
	CHECK(st.frames_out > 500);

	CASE("jb_reorder_distance2");
	reorder_run(80, 2);
	sim_stats(&st);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.plc, 0);
	CHECK(st.frames_out > 500);

	CASE("jb_reorder_distance5");
	reorder_run(150, 5);
	sim_stats(&st);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.plc, 0);
	CHECK(st.frames_out > 450);
}

static void test_duplicate_and_late(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint32_t dup_ts = 0;
	uint16_t dup_seq = 0;
	unsigned i;

	CASE("jb_duplicate_and_late_counts");
	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	for (i = 0; i < 200U; i++) {
		if (!(i % 2)) {
			if (i == 20U) {
				dup_ts = sim.ts;
				dup_seq = sim.seq;
			}
			sim_send(sim.now_ns);
		}
		if (i == 22U)
			sim_queue(sim.now_ns, dup_ts, dup_seq, RTP_PAYLOAD_AUDIO,
				  2U * sim.bpf);
		if (i == 60U)
			sim_queue(sim.now_ns, dup_ts, dup_seq, RTP_PAYLOAD_AUDIO,
				  2U * sim.bpf);
		if (i == 100U)
			sim_queue(sim.now_ns, dup_ts - 800U,
				  (uint16_t)(dup_seq - 200U), RTP_PAYLOAD_AUDIO,
				  2U * sim.bpf);
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.dup_drop, 2);
	CHECK_UINT(st.late_drop, 1);
	CHECK_UINT(sim.dup, 2);
	CHECK_UINT(sim.late, 1);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(st.plc, 0);
}

static void test_burst_loss(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned prefill_silence = 0;
	unsigned plc_run = 0;
	unsigned longest = 0;
	uint32_t prev_ts = 0;
	uint32_t last_ts = 0;
	uint32_t resume_ts = 0;
	unsigned i;

	CASE("jb_burst_loss_120ms");
	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	for (i = 0; i < 400U; i++) {
		if (!(i % 2)) {
			if (i >= 100U && i < 112U)
				sim_skip(1);
			else
				sim_send(sim.now_ns);
		}
		sim_tick(&out);
		if (!sim.expect_valid)
			prefill_silence++;
		if (out.decision == UDSP_JB_PLC) {
			if (!plc_run)
				last_ts = prev_ts;
			plc_run++;
			continue;
		}
		if (plc_run > longest) {
			longest = plc_run;
			resume_ts = out.rtp_timestamp;
		}
		if (out.decision == UDSP_JB_FRAME)
			prev_ts = out.rtp_timestamp;
		plc_run = 0;
	}
	sim_stats(&st);
	CHECK_UINT(longest, 12);
	CHECK_UINT(st.plc, 12);
	CHECK_UINT(st.underruns, 1);
	CHECK_UINT(st.silence, prefill_silence);
	CHECK_UINT(st.cn, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK(resume_ts != 0 && last_ts != 0);
	CHECK_UINT(resume_ts, last_ts + 13U * UDSP_JB_FRAME_TS);
}

/* One CN packet stands in for the audio packet of its own slot, which is what a
 * far end with VAD sends when it enters silence. */
static void cn_send(void)
{
	unsigned bpf = sim.bpf;

	sim.bpf = 1U;
	sim_queue(sim.now_ns, sim.ts, sim.seq, RTP_PAYLOAD_COMFORT_NOISE, 2);
	sim.bpf = bpf;
	sim_skip(1);
}

static void long_loss_run(bool with_cn)
{
	struct udsp_jb_out out;
	unsigned i;

	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	for (i = 0; i < 400U; i++) {
		if (i % 2) {
			sim_tick(&out);
			continue;
		}
		if (i == 40U && with_cn)
			cn_send();
		else if (i >= 100U && i < 200U)
			sim_skip(1);
		else
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
}

static void test_plc_limit(void)
{
	struct udsp_jb_stats st;

	CASE("jb_loss_beyond_plc_limit_silence");
	long_loss_run(false);
	sim_stats(&st);
	CHECK_UINT(st.plc, JB_PLC_LIMIT_FRAMES);
	CHECK_UINT(st.cn, 0);
	CHECK_UINT(st.silence, 4 + 60);
	CHECK_UINT(st.underruns, 1);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(sim.order_errors, 0);

	CASE("jb_loss_beyond_plc_limit_cn");
	long_loss_run(true);
	sim_stats(&st);
	CHECK_UINT(st.plc, JB_PLC_LIMIT_FRAMES);
	CHECK_UINT(st.cn, 62);
	CHECK_UINT(st.silence, 4);
	CHECK_UINT(st.underruns, 1);
	CHECK_UINT(sim.order_errors, 0);
}

static void test_cn_gap(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned cn_first = 0;
	unsigned cn_payload_ok = 0;
	unsigned prefill_silence = 0;
	unsigned i;

	CASE("jb_cn_gap_plays_received_payload");
	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	for (i = 0; i < 300U; i++) {
		if (!(i % 2) && i == 100U)
			cn_send();
		else if (!(i % 2) && (i < 100U || i >= 200U))
			sim_send(sim.now_ns);
		else if (!(i % 2))
			sim_skip(1);
		sim_tick(&out);
		if (!sim.expect_valid)
			prefill_silence++;
		if (out.decision != UDSP_JB_CN)
			continue;
		if (!cn_first)
			cn_first = i;
		if (out.len == 2U && out.payload &&
		    out.payload[0] == (uint8_t)((sim.jb.cn_ts /
						 UDSP_JB_FRAME_TS) & 0xffU))
			cn_payload_ok++;
	}
	sim_stats(&st);
	CHECK(cn_first >= 100 && cn_first <= 112);
	CHECK(st.cn > 80);
	CHECK_UINT(cn_payload_ok, st.cn);
	CHECK_UINT(st.plc, 0);
	CHECK_UINT(st.underruns, 0);
	CHECK_UINT(st.silence, prefill_silence);
	CHECK_UINT(sim.order_errors, 0);
}

/* Half a period of skew: the far end sends one 10 ms frame every 9.95 ms or
 * every 10.05 ms while the engine consumes one every 10 ms. */
static uint16_t skew_peak;
static uint16_t skew_trough;
static unsigned skew_gap_min;

static void skew_run(uint64_t interval_ns, unsigned ticks, uint16_t *half_depth)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint64_t next;
	unsigned last_adjust = 0;
	unsigned i;

	sim_reset(UDSP_JB_FIXED, 50, 100, 10, 80);
	skew_peak = 0;
	skew_trough = 0xffffU;
	skew_gap_min = ticks;
	next = sim.now_ns;
	for (i = 0; i < ticks; i++) {
		while (next <= sim.now_ns) {
			sim_send(next);
			next += interval_ns;
		}
		sim_tick(&out);
		sim_stats(&st);
		if (i > 500U && st.depth_ms > skew_peak)
			skew_peak = st.depth_ms;
		if (i > 500U && st.depth_ms < skew_trough)
			skew_trough = st.depth_ms;
		if (i == ticks / 2U)
			*half_depth = st.depth_ms;
		if (!out.skew_drop && !out.skew_insert)
			continue;
		if (last_adjust && i - last_adjust < skew_gap_min)
			skew_gap_min = i - last_adjust;
		last_adjust = i;
	}
}

static void test_skew(void)
{
	struct udsp_jb_stats st;
	uint16_t half = 0;

	CASE("jb_skew_far_end_fast");
	skew_run(9950000ULL, 6000U, &half);
	sim_stats(&st);
	CHECK(st.skew_drops >= 26 && st.skew_drops <= 34);
	CHECK_UINT(st.skew_inserts, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(st.underruns, 0);
	CHECK_UINT(st.plc, 0);
	/* The correction ends within one frame of the setpoint, and the
	 * half-way sample proves the depth is not drifting. */
	CHECK(st.depth_ms + UDSP_JB_FRAME_MS >= st.target_ms &&
	      st.depth_ms <= st.target_ms + UDSP_JB_FRAME_MS);
	CHECK(half + UDSP_JB_FRAME_MS >= st.target_ms &&
	      half <= st.target_ms + UDSP_JB_FRAME_MS);
	CHECK(skew_peak <= st.target_ms + 2U * UDSP_JB_FRAME_MS);
	CHECK(skew_trough + 3U * UDSP_JB_FRAME_MS >= st.target_ms);
	/* The correction runs at one frame per second, so consecutive
	 * adjustments are a second apart and never further. */
	CHECK(skew_gap_min >= 100U && skew_gap_min <= 150U);
	CHECK_UINT(sim.order_errors, 0);

	CASE("jb_skew_far_end_slow");
	skew_run(10050000ULL, 6000U, &half);
	sim_stats(&st);
	CHECK(st.skew_inserts >= 26 && st.skew_inserts <= 34);
	CHECK_UINT(st.skew_drops, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.underruns, 0);
	CHECK_UINT(st.plc, 0);
	CHECK(st.depth_ms + UDSP_JB_FRAME_MS >= st.target_ms &&
	      st.depth_ms <= st.target_ms + UDSP_JB_FRAME_MS);
	CHECK(half + UDSP_JB_FRAME_MS >= st.target_ms &&
	      half <= st.target_ms + UDSP_JB_FRAME_MS);
	CHECK(skew_trough + 4U * UDSP_JB_FRAME_MS >= st.target_ms);
	CHECK(skew_peak <= st.target_ms + 2U * UDSP_JB_FRAME_MS);
	CHECK(skew_gap_min >= 100U && skew_gap_min <= 150U);
	CHECK_UINT(sim.order_errors, 0);
}

static void test_resync_ssrc(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned recover = 0;
	unsigned i;

	CASE("jb_resync_ssrc_change");
	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	for (i = 0; i < 400U; i++) {
		if (i == 200U) {
			sim.ssrc = 0x99887766U;
			sim.ts = 40000U;
			sim.seq = 5U;
		}
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
		if (i > 200U && !recover && out.decision == UDSP_JB_FRAME)
			recover = i - 200U;
	}
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 1);
	CHECK(recover > 0 && recover * UDSP_JB_FRAME_MS <= 100U);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK(st.frames_out > 380);
	/* The reset threw away the two queued reachable slots, 40 ms of playable
	 * audio. The exact value fails a counter that does not count the
	 * discard. */
	CHECK_UINT(st.resync_discard_ms, 40);
}

static void test_resync_ts_jump(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned recover = 0;
	unsigned i;

	CASE("jb_resync_timestamp_jump");
	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	for (i = 0; i < 400U; i++) {
		if (i == 200U)
			sim.ts += 11U * RTP_CLOCK_RATE;
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
		if (i > 200U && !recover && out.decision == UDSP_JB_FRAME)
			recover = i - 200U;
	}
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 1);
	CHECK(recover > 0 && recover * UDSP_JB_FRAME_MS <= 100U);
	CHECK(st.frames_out > 380);
	CHECK_UINT(st.late_drop, 0);
	/* The timestamp jump resync discards the same 40 ms of queued audio. */
	CHECK_UINT(st.resync_discard_ms, 40);
}

static void test_wrap(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_wrap_timestamp");
	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	sim.ts = 0xffffff00U;
	for (i = 0; i < 400U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.dup_drop, 0);
	CHECK_UINT(st.plc, 0);
	CHECK_UINT(st.silence, 4);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK(st.frames_out > 380);

	CASE("jb_wrap_sequence");
	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	sim.seq = 65530U;
	for (i = 0; i < 400U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.dup_drop, 0);
	CHECK_UINT(st.plc, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK(st.frames_out > 380);

	CASE("jb_wrap_sequence_duplicate");
	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	sim.seq = 65534U;
	for (i = 0; i < 40U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		if (i == 10U)
			sim_queue(sim.now_ns, sim.ts - 320U,
				  (uint16_t)(sim.seq - 2U), RTP_PAYLOAD_AUDIO,
				  2U * sim.bpf);
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.dup_drop, 1);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
}

/* A half-range timestamp step is INT32_MIN as a signed difference, so the jump
 * test must take its magnitude in unsigned arithmetic. */
static void test_timestamp_half_range(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_timestamp_half_range_step");
	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	for (i = 0; i < 200U; i++) {
		if (i == 100U)
			sim.ts = sim.jb.last_rx_ts + 0x80000000U;
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 1);
	CHECK(st.frames_out > 180);
	CHECK_UINT(st.overflow_drop, 0);
	/* The half-range timestamp jump reset discards the same 40 ms of
	 * queued audio. */
	CHECK_UINT(st.resync_discard_ms, 40);
}

static void test_shrink_rate(void)
{
	struct udsp_jb_out out;
	struct udsp_jb_stats st;
	unsigned violations = 0;
	unsigned samples = 0;
	uint16_t previous;
	unsigned i;

	CASE("jb_shrink_rate_limited");
	sim_reset(UDSP_JB_ADAPTIVE, 100, 200, 20, 80);
	sim_stats(&st);
	previous = st.target_ms;
	for (i = 0; i < 2000U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
		if ((i + 1U) % 100U)
			continue;
		sim_stats(&st);
		samples++;
		if (previous > st.target_ms &&
		    (unsigned)(previous - st.target_ms) > UDSP_JB_FRAME_MS)
			violations++;
		previous = st.target_ms;
	}
	CHECK_UINT(violations, 0);
	CHECK_UINT(samples, 20);
	/* The shrink decays to the span floor and no further; that settled floor
	 * is one packet span plus one frame. */
	CHECK_UINT(st.target_ms, 30);
	CHECK(st.frames_out > 1800);
}

static void test_slice_low_rate(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_slice_30ms_low_rate_codec");
	sim_reset(UDSP_JB_FIXED, 60, 100, 30, 10);
	for (i = 0; i < 300U; i++) {
		if (!(i % 3))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(st.plc, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK(st.frames_out > 280);
}

static void test_overflow_and_validation(void)
{
	struct udsp_jb_packet in;
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint8_t payload[UDSP_JB_PAYLOAD_MAX + 1U];
	unsigned i;

	CASE("jb_overflow_and_argument_validation");
	memset(payload, 7, sizeof(payload));
	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	memset(&in, 0, sizeof(in));
	in.payload = payload;
	in.len = UDSP_JB_PAYLOAD_MAX + 1U;
	in.arrival_ns = sim.now_ns;
	in.ssrc = sim.ssrc;
	in.rtp_timestamp = sim.ts;
	in.seq = sim.seq;
	in.kind = RTP_PAYLOAD_AUDIO;
	CHECK(udsp_jb_put(&sim.jb, &in) == -EMSGSIZE);
	in.len = 160;
	in.kind = RTP_PAYLOAD_TELEPHONE_EVENT;
	CHECK(udsp_jb_put(&sim.jb, &in) == -EINVAL);
	in.kind = RTP_PAYLOAD_AUDIO;
	in.payload = NULL;
	CHECK(udsp_jb_put(&sim.jb, &in) == -EINVAL);
	CHECK(udsp_jb_put(NULL, &in) == -EINVAL);
	CHECK(udsp_jb_put(&sim.jb, NULL) == -EINVAL);
	CHECK(udsp_jb_get(&sim.jb, sim.now_ns, NULL) == -EINVAL);
	CHECK(udsp_jb_get(NULL, sim.now_ns, &out) == -EINVAL);
	udsp_jb_stats(&sim.jb, &st);
	CHECK_UINT(st.overflow_drop, 1);

	for (i = 0; i < UDSP_JB_SLOTS + 8U; i++)
		sim_send(sim.now_ns);
	sim_deliver(sim.now_ns);
	sim_stats(&st);
	CHECK_UINT(st.overflow_drop, 9);
	CHECK_UINT(sim.over, 8);
	CHECK_UINT(sim.accepted, UDSP_JB_SLOTS);

	udsp_jb_init(&sim.jb, (enum udsp_jb_mode)77, 0, 0);
	udsp_jb_stats(&sim.jb, &st);
	CHECK_UINT(st.target_ms, UDSP_JB_FRAME_MS);
	CHECK_UINT(st.frames_out, 0);
	udsp_jb_init(&sim.jb, UDSP_JB_ADAPTIVE, 700, 20);
	udsp_jb_stats(&sim.jb, &st);
	CHECK_UINT(st.target_ms, UDSP_JB_DEPTH_MAX_MS);
	udsp_jb_init(NULL, UDSP_JB_ADAPTIVE, 50, 100);
	udsp_jb_stats(NULL, &st);
	udsp_jb_stats(&sim.jb, NULL);
}

static void test_starvation_recovery(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned frames_after = 0;
	uint32_t paused_ts = 0;
	unsigned i;

	CASE("jb_stalled_sender_resumes_behind");
	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	for (i = 0; i < 600U; i++) {
		if (i == 100U)
			paused_ts = sim.ts;
		if (i == 300U)
			sim.ts = paused_ts - 100U * UDSP_JB_FRAME_TS;
		if (!(i % 2) && (i < 100U || i >= 300U))
			sim_send(sim.now_ns);
		sim_tick(&out);
		if (i > 320U && out.decision == UDSP_JB_FRAME)
			frames_after++;
	}
	sim_stats(&st);
	CHECK_UINT(st.starve_resyncs, 1);
	CHECK_UINT(st.resyncs, 0);
	CHECK(frames_after > 260);
	CHECK(st.late_drop <= 1);
}


/* One lost packet in a 10 ms stream makes a single 160-tick delta. The slicing
 * span must not follow it, or every 80-byte frame after a loss is emitted as
 * two 40-byte halves. */
static void test_span_loss(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned short_frames = 0;
	unsigned i;

	CASE("jb_span_loss_does_not_split_frames");
	sim_reset(UDSP_JB_FIXED, 60, 100, 10, 80);
	for (i = 0; i < 300U; i++) {
		if (i == 150U)
			sim_skip(1);
		else
			sim_send(sim.now_ns);
		sim_tick(&out);
		if (out.decision == UDSP_JB_FRAME && out.len != 80U)
			short_frames++;
	}
	sim_stats(&st);
	CHECK_UINT(short_frames, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(st.plc, 1);
	CHECK_UINT(st.underruns, 1);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK(sim.jb.pkt_span_ts == UDSP_JB_FRAME_TS);
}

/* An unsignaled ptime change from 20 ms to 30 ms in mid-stream still adapts.
 * The accepted cost is the packets that arrive before the new span is proven.
 * A re-INVITE that moves ptime re-inits the buffer instead. */
static void test_span_ptime_change(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned settled_errors;
	unsigned i;

	CASE("jb_span_ptime_change_adapts");
	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	for (i = 0; i < 200U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	CHECK_UINT(sim.jb.pkt_span_ts, 160);
	sim.span_ms = 30U;
	for (i = 0; i < 60U; i++) {
		if (!(i % 3))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	CHECK_UINT(sim.jb.pkt_span_ts, 240);
	settled_errors = sim.payload_errors;
	for (i = 0; i < 300U; i++) {
		if (!(i % 3))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(sim.payload_errors - settled_errors, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK(st.frames_out > 500);
}

/* A buffer re-inited for a 30 ms re-INVITE slices its very first packet
 * correctly. The first plausible observation establishes the span while the
 * prefill still holds every slot. */
static void test_span_first_packet(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned frames = 0;
	unsigned i;

	CASE("jb_span_first_packet_after_init");
	sim_reset(UDSP_JB_FIXED, 60, 100, 30, 80);
	for (i = 0; i < 300U; i++) {
		if (!(i % 3))
			sim_send(sim.now_ns);
		sim_tick(&out);
		if (out.decision == UDSP_JB_FRAME)
			frames++;
	}
	sim_stats(&st);
	CHECK_UINT(sim.jb.pkt_span_ts, 240);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(st.plc, 0);
	CHECK_UINT(st.late_drop, 0);
	/* Every frame of the first packet plays: the run starts at its first
	 * slice and never skips one. */
	CHECK_UINT(st.frames_out, frames);
	CHECK(frames >= 291);
}

/* One packet of a different length is a sender hiccup, not a ptime change, so
 * the learned span must ignore it. */
static void test_span_single_odd_packet(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_span_single_odd_packet_ignored");
	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	for (i = 0; i < 300U; i++) {
		if (!(i % 2) && i == 100U) {
			sim.span_ms = 30U;
			sim_send(sim.now_ns);
			sim.span_ms = 20U;
		} else if (!(i % 2)) {
			sim_send(sim.now_ns);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(sim.jb.pkt_span_ts, 160);
	/* Only the odd packet's own two slices are mis-sized; the learned span
	 * never moves, so every later packet stays whole. */
	CHECK_UINT(sim.payload_errors, 2);
	CHECK_UINT(st.plc, 1);
	CHECK_UINT(st.late_drop, 0);
	CHECK(st.frames_out > 280);
}

/* With the frame size stated, the span of a packet is its own payload. The
 * close of a talkspurt on an odd frame count is stamped and sliced whole, and
 * the learned span never moves. */
static void test_span_frame_units(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_span_short_packet_slices_by_frame");
	sim_reset_sid(UDSP_JB_FIXED, 60, 100, 20, 80, 0);
	for (i = 0; i < 300U; i++) {
		if (i == 100U) {
			sim.span_ms = 10U;
			sim_send(sim.now_ns);
			sim.span_ms = 20U;
		} else if (!(i % 2)) {
			sim_send(sim.now_ns);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(sim.jb.pkt_span_ts, 160);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(st.plc, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK(st.frames_out > 280);

	CASE("jb_span_odd_packet_slices_by_frame");
	sim_reset_sid(UDSP_JB_FIXED, 60, 100, 20, 80, 0);
	for (i = 0; i < 300U; i++) {
		if (i == 100U) {
			sim.span_ms = 30U;
			sim_send(sim.now_ns);
			sim.span_ms = 20U;
		} else if (!(i % 2)) {
			sim_send(sim.now_ns);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(sim.jb.pkt_span_ts, 160);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(st.plc, 0);
	CHECK_UINT(st.late_drop, 0);

	/* One speech frame and no SID on an Annex B codec is not the SID
	 * shape; it is one whole frame. */
	CASE("jb_span_one_speech_frame_on_a_sid_codec");
	sim_reset_sid(UDSP_JB_FIXED, 60, 100, 20, 10, 2);
	for (i = 0; i < 300U; i++) {
		if (i == 100U) {
			sim.span_ms = 10U;
			sim_send(sim.now_ns);
			sim.span_ms = 20U;
		} else if (!(i % 2)) {
			sim_send(sim.now_ns);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(sim.jb.pkt_span_ts, 160);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(st.plc, 0);
	CHECK_UINT(st.late_drop, 0);

	/* The close of a talkspurt on an odd frame count, then the comfort
	 * noise on the timestamp it left: the short packet slices whole and
	 * the noise is on time, the shape a sender with a voice detector puts
	 * on the wire at every odd close. */
	CASE("jb_short_close_then_cn_plays_whole");
	sim_reset_sid(UDSP_JB_FIXED, 60, 100, 20, 80, 0);
	for (i = 0; i < 300U; i++) {
		if (i == 100U) {
			sim.span_ms = 10U;
			sim_send(sim.now_ns);
			sim.span_ms = 20U;
		} else if (i == 102U) {
			cn_send();
		} else if (!(i % 2) && (i < 100U || i >= 200U)) {
			sim_send(sim.now_ns);
		} else if (!(i % 2) && i > 102U) {
			sim_skip(1);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK(st.cn > 80);
	CHECK_UINT(st.plc, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(sim.order_errors, 0);
}

/* Sustained adjacent-swap delivery never delivers two seq-consecutive packets
 * back to back. A learner that measures the successor delta against the
 * previous arrival time therefore starves. With such a learner, pkt_span_ts
 * stays 80 while every packet carries 160 bytes, and jb_frame_emit hands out
 * 160-byte "10 ms" units. A real codec refuses every one (g711/g726 decode
 * take exactly one frame), so the tick plays PLC while frames_out climbs. The
 * pair must form through the timestamp of the seq_high packet: the delayed
 * predecessor teaches the span at the second arrival. */
static void test_span_reorder_swap(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned bad_len = 0;
	uint32_t hold_ts;
	uint16_t hold_seq;
	unsigned i;

	CASE("jb_span_reorder_adjacent_swap");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 400U; i++) {
		if (!(i % 4)) {
			hold_ts = sim.ts;
			hold_seq = sim.seq;
			sim_skip(1);
			sim_send(sim.now_ns);
			sim_queue(sim.now_ns + 5U * MS_NS, hold_ts, hold_seq,
				  RTP_PAYLOAD_AUDIO, 2U * sim.bpf);
		}
		/* the delayed predecessor of the first pair arrived inside
		 * tick 1, so the span is learned from the second arrival on */
		if (i == 2U)
			CHECK(sim.jb.span_learned);
		sim_tick(&out);
		if (out.decision == UDSP_JB_FRAME && out.len != sim.bpf)
			bad_len++;
	}
	sim_stats(&st);
	CHECK_UINT(sim.jb.pkt_span_ts, 160);
	CHECK_UINT(bad_len, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(st.plc, 0);
	CHECK_UINT(st.underruns, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.dup_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 380);
}

/* Deep startup reorder starves the learner past the prefill. The pair rule
 * reaches three sequence numbers from seq_high, so the starving delivery keeps
 * every arrival at least 4 away from the running high: the high lane jumps in
 * fours (packets 4, 8, 12, ... on a small delay) and the three packets between
 * fall 5 to 7 below the current high (delivery order 4, 0, 8, 1, 2, 3, 12, 5,
 * 6, 7, 16, ...). No pair forms and PLAY runs on the default span with every
 * emitted unit wrong. When in-order delivery resumes at t=2 s, the first learn
 * is in PLAY through the corroboration rule: the boundary pair nominates and
 * its agreeing successor adopts. Mis-spanned slots are still queued at that
 * point. The slots wholly ahead of the playout clock must take the learned
 * span, or each of them still emits one 160-byte unit after the learn. */
static unsigned span_starve_delay_ms(unsigned k, unsigned frame_ms)
{
	unsigned delay;

	if (k == 0U)
		delay = 8U;
	else if (k == 4U)
		delay = 3U;
	else
		delay = k % 4U ? 9U : 1U;
	return delay * frame_ms;
}

static void test_span_reorder_startup(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned errors_before = 0;
	unsigned bad_len_after = 0;
	unsigned i, k;

	CASE("jb_span_reorder_startup_restamp");
	sim_reset(UDSP_JB_FIXED, 300, 500, 20, 80);
	for (i = 0; i < 600U; i++) {
		if (!(i % 2)) {
			k = i / 2U;
			if (k < 100U)
				sim_send(sim.now_ns + (uint64_t)
					 span_starve_delay_ms(k, 20U) * MS_NS);
			else
				sim_send(sim.now_ns);
		}
		/* packet 102, the second boundary pair, adopts inside tick 204:
		 * the queue behind it is restamped there and no later unit
		 * may carry a wrong length */
		if (i == 200U)
			CHECK(!sim.jb.span_learned);
		if (i == 204U)
			errors_before = sim.payload_errors;
		sim_tick(&out);
		if (i >= 204U && out.decision == UDSP_JB_FRAME &&
		    out.len != sim.bpf)
			bad_len_after++;
	}
	sim_stats(&st);
	CHECK(sim.jb.span_learned);
	CHECK_UINT(sim.jb.pkt_span_ts, 160);
	CHECK_UINT(sim.payload_errors - errors_before, 0);
	CHECK_UINT(bad_len_after, 0);
	/* the mis-sliced units before the corroborated learn are the
	 * deterministic cost of the unlearned window itself (one more than
	 * an at-once adoption would pay: the boundary pair only nominates) */
	CHECK_UINT(errors_before, 85);
	CHECK_UINT(st.plc, 85);
	CHECK_UINT(st.underruns, 85);
	/* the learned span doubles the reachable depth reading in one put
	 * (new packets and the restamped queue both carry it); the trend
	 * corrector sheds the surplus at one frame per second. The count is
	 * the same with the restamp reverted: the depth move comes from the
	 * learn, not from the restamp. */
	CHECK_UINT(st.skew_drops, 4);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.dup_drop, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 480);
}

/* The comfort-noise case. CN packets consume sequence numbers and take part in
 * pair formation (RFC 3389 stamps a CN where the next audio frame would
 * start). Under reorder, a learner that pairs the successor of a CN with the
 * last arrival fails. Arrival order CN(n+1), A(n), A(n+2) then measures
 * ts(n+2) - ts(n), two spans across the hole. It learns a false 160 on an
 * 80-tick stream and halves every slice, until two agreeing pairs repair it.
 * The seq_high pair measures one span, and the delayed predecessor of the CN
 * already taught the right span. */
static void test_span_cn_reorder(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint32_t hold_ts;
	uint16_t hold_seq;
	unsigned i;

	CASE("jb_span_cn_reorder_first_learn");
	sim_reset(UDSP_JB_FIXED, 60, 100, 10, 80);
	hold_ts = sim.ts;
	hold_seq = sim.seq;
	sim_skip(1);
	cn_send();
	sim_queue(sim.now_ns + 5U * MS_NS, hold_ts, hold_seq,
		  RTP_PAYLOAD_AUDIO, sim.bpf);
	for (i = 0; i < 200U; i++) {
		if (i)
			sim_send(sim.now_ns);
		/* a learner that pairs the CN successor with the last
		 * arrival would read 160 here */
		if (i == 3U)
			CHECK_UINT(sim.jb.pkt_span_ts, 80);
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK(sim.jb.span_learned);
	CHECK_UINT(sim.jb.pkt_span_ts, 80);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.dup_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 180);
}

/* Sustained gap-2 delivery, in the order 0,2,4,1,6,3,8,5... Every arrival sits
 * 2 or 3 away from seq_high, so adjacent-only pairing starves and emits every
 * unit with a wrong length. A gap-n pair on a linear timeline measures exactly
 * n spans. The normalized pair therefore learns from the second arrival, and
 * every unit is right. */
static void test_span_gap2_20ms(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned bad_len = 0;
	uint32_t base_ts;
	uint16_t base_seq;
	unsigned i, k;

	CASE("jb_span_gap2_reorder_20ms");
	/* FIXED 100: the odd sibling of each group is two groups late (65 ms
	 * behind its even), so the pinned playout point needs that margin */
	sim_reset(UDSP_JB_FIXED, 100, 200, 20, 80);
	base_ts = sim.ts;
	base_seq = sim.seq;
	for (i = 0; i < 800U; i++) {
		if (!(i % 4)) {
			k = i / 4U;
			sim_queue(sim.now_ns, base_ts + 320U * k,
				  (uint16_t)(base_seq + 2U * k),
				  RTP_PAYLOAD_AUDIO, 2U * sim.bpf);
			if (k >= 2U)
				sim_queue(sim.now_ns + 5U * MS_NS,
					  base_ts + 320U * (k - 2U) + 160U,
					  (uint16_t)(base_seq + 2U * (k - 2U) + 1U),
					  RTP_PAYLOAD_AUDIO, 2U * sim.bpf);
		}
		if (i == 8U)
			CHECK(sim.jb.span_learned);
		sim_tick(&out);
		if (out.decision == UDSP_JB_FRAME && out.len != sim.bpf)
			bad_len++;
	}
	sim_stats(&st);
	CHECK_UINT(sim.jb.pkt_span_ts, 160);
	CHECK_UINT(bad_len, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 700);
}

/* The 10 ms variant. The starved default span happens to be right here, so
 * adjacent-only pairing slices nothing wrong but never learns. The check of
 * span_learned is the one that fails first. The case also fails an
 * implementation that adopts the raw gap-2 delta without normalization: it
 * reads 160 on this 80-tick stream and halves every slice. */
static void test_span_gap2_10ms(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned bad_len = 0;
	uint32_t base_ts;
	uint16_t base_seq;
	unsigned i, k;

	CASE("jb_span_gap2_reorder_10ms");
	sim_reset(UDSP_JB_FIXED, 60, 100, 10, 80);
	base_ts = sim.ts;
	base_seq = sim.seq;
	for (i = 0; i < 400U; i++) {
		if (!(i % 2)) {
			k = i / 2U;
			sim_queue(sim.now_ns, base_ts + 160U * k,
				  (uint16_t)(base_seq + 2U * k),
				  RTP_PAYLOAD_AUDIO, 80);
			if (k >= 2U)
				sim_queue(sim.now_ns + 5U * MS_NS,
					  base_ts + 160U * (k - 2U) + 80U,
					  (uint16_t)(base_seq + 2U * (k - 2U) + 1U),
					  RTP_PAYLOAD_AUDIO, 80);
		}
		if (i == 8U)
			CHECK(sim.jb.span_learned);
		sim_tick(&out);
		if (out.decision == UDSP_JB_FRAME && out.len != sim.bpf)
			bad_len++;
	}
	sim_stats(&st);
	CHECK(sim.jb.span_learned);
	CHECK_UINT(sim.jb.pkt_span_ts, 80);
	CHECK_UINT(bad_len, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* A VAD far end on a gap-2 reordered line. The gap-2 pairs teach the true span
 * inside the prefill. The comfort-noise boundary pair then meets an
 * established span, and the JB_SPAN_AGREE logic absorbs it. On this 80-tick
 * stream, one 20 ms silence reads as a 160-tick delta. Adjacent-only pairing
 * starves through the reorder, then adopts that single boundary pair at once
 * and restamps the whole queue. */
static void test_span_cn_silence_shield(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned bad_len = 0;
	uint32_t base_ts;
	uint16_t base_seq;
	unsigned i;

	CASE("jb_span_cn_silence_change_shield");
	sim_reset(UDSP_JB_ADAPTIVE, 40, 100, 10, 80);
	base_ts = sim.ts;
	base_seq = sim.seq;
	for (i = 0; i < 40U; i++) {
		sim_queue(sim.now_ns, base_ts + 80U * (2U * i),
			  (uint16_t)(base_seq + 2U * i), RTP_PAYLOAD_AUDIO, 80);
		if (i >= 2U)
			sim_queue(sim.now_ns + 3U * MS_NS,
				  base_ts + 80U * (2U * (i - 2U) + 1U),
				  (uint16_t)(base_seq + 2U * (i - 2U) + 1U),
				  RTP_PAYLOAD_AUDIO, 80);
		sim_tick(&out);
	}
	sim.seq = (uint16_t)(base_seq + 80U);
	sim.ts = base_ts + 80U * 80U;
	cn_send();
	sim.ts += 80U;
	for (i = 0; i < 160U; i++) {
		if (i < 80U)
			sim_send(sim.now_ns);
		sim_tick(&out);
		if (out.decision == UDSP_JB_FRAME && out.len != sim.bpf)
			bad_len++;
	}
	/* the gap-2 form: the first resume packet after a second silence
	 * is lost, so the boundary pair spans the CN and the survivor two
	 * sequence numbers apart; its normalized delta (120) is off the
	 * frame grid and teaches nothing */
	cn_send();
	sim.ts += 80U;
	sim_skip(1);
	for (i = 0; i < 80U; i++) {
		sim_send(sim.now_ns);
		sim_tick(&out);
		if (out.decision == UDSP_JB_FRAME && out.len != sim.bpf)
			bad_len++;
	}
	sim_stats(&st);
	CHECK(sim.jb.span_learned);
	CHECK_UINT(sim.jb.pkt_span_ts, 80);
	CHECK_UINT(bad_len, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* The corroboration half. Block-interleaved delivery (the startup-restamp
 * pattern at ptime 10) starves the learner for two seconds while the stream
 * plays. The far end then sends comfort noise and resumes one silence later: a
 * single +1 pair that reads 160 on an 80-tick stream. A first learn on a
 * stream that already played must corroborate. The boundary pair therefore
 * only nominates, and the in-order resume pairs adopt the true 80. An at-once
 * rule adopts the 160 and restamps the deep queue behind it. */
static void test_span_cn_late_first_learn(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned bad_len = 0;
	unsigned i;

	CASE("jb_span_cn_late_first_learn");
	sim_reset(UDSP_JB_FIXED, 200, 500, 10, 80);
	for (i = 0; i < 200U; i++) {
		sim_send(sim.now_ns +
			 (uint64_t)span_starve_delay_ms(i, 10U) * MS_NS);
		if (i == 199U)
			CHECK(!sim.jb.span_learned);
		sim_tick(&out);
	}
	cn_send();
	sim.ts += 80U;
	for (i = 0; i < 400U; i++) {
		if (i < 200U)
			sim_send(sim.now_ns);
		if (i == 40U)
			CHECK_UINT(sim.jb.pkt_span_ts, 80);
		sim_tick(&out);
		if (out.decision == UDSP_JB_FRAME && out.len != sim.bpf)
			bad_len++;
	}
	sim_stats(&st);
	CHECK(sim.jb.span_learned);
	CHECK_UINT(sim.jb.pkt_span_ts, 80);
	CHECK_UINT(bad_len, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* The at-once half. UDSP_JB_OFF has no prefill. A pair that arrives before the
 * first frame is emitted still adopts at once, and the restamp slices the
 * first packet right. This checks the frames-emitted gate against the rejected
 * PREFILL-state gate. That gate would force corroboration on every OFF stream,
 * and emit the first two packets whole. */
static void test_span_off_early_pair(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint32_t hold_ts;
	uint16_t hold_seq;
	unsigned i;

	CASE("jb_span_off_pair_before_first_frame");
	sim_reset(UDSP_JB_OFF, 50, 100, 20, 80);
	hold_ts = sim.ts;
	hold_seq = sim.seq;
	sim_skip(1);
	sim_queue(sim.now_ns, hold_ts, hold_seq, RTP_PAYLOAD_AUDIO,
		  2U * sim.bpf);
	sim_send(sim.now_ns);
	for (i = 0; i < 200U; i++) {
		if (i && !(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK(sim.jb.span_learned);
	CHECK_UINT(sim.jb.pkt_span_ts, 160);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK(st.frames_out > 190);
}

/* The re-seed of the step resync. Gap-4 loss keeps the span unlearned through
 * two seconds of PLAY. A timeline step, armed and confirmed three sequence
 * numbers apart on a non-empty queue, then resyncs the stream. The pair across
 * the resync packet is the first observation of the relearn, and only the
 * seq_high_ts re-seed of the step path can measure it. Without the re-seed,
 * the pair reads the absolute timestamp against zero and is rejected. The
 * corroborated adoption then comes one packet later, and one more packet is
 * sliced wrong. */
static void test_span_step_reseed(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint32_t step_ts;
	uint16_t step_seq;
	unsigned i;

	CASE("jb_span_step_relearn_reseed");
	sim_reset(UDSP_JB_ADAPTIVE, 60, 100, 20, 80);
	for (i = 0; i < 193U; i++) {
		if (!(i % 2)) {
			if ((i / 2U) % 4U)
				sim_skip(1);
			else
				sim_send(sim.now_ns);
		}
		sim_tick(&out);
	}
	CHECK(!sim.jb.span_learned);
	step_ts = sim.ts + 2000U * 8U;
	step_seq = sim.seq;
	/* the last survivor played in the tick before, so the arming packet
	 * arrives on an unparked clock and the confirm arrives on a non-empty
	 * queue: only that path takes the full step resync whose re-seed
	 * this case pins */
	sim_queue(sim.now_ns, step_ts, step_seq, RTP_PAYLOAD_AUDIO,
		  2U * sim.bpf);
	sim_tick(&out);
	sim_queue(sim.now_ns, step_ts + 480U, (uint16_t)(step_seq + 3U),
		  RTP_PAYLOAD_AUDIO, 2U * sim.bpf);
	sim_tick(&out);
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 1);
	sim.seq = (uint16_t)(step_seq + 4U);
	sim.ts = step_ts + 640U;
	for (i = 0; i < 200U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK(sim.jb.span_learned);
	CHECK_UINT(sim.jb.pkt_span_ts, 160);
	CHECK_UINT(st.resyncs, 1);
	/* 22 whole-emitted survivors before the resync, then two resume
	 * packets sliced with the default span while the boundary pair only
	 * nominates; the corroborated adoption stops the count here. Without
	 * the step re-seed the pair straddling the resync packet measures an
	 * absolute timestamp against zero and is rejected, so the adoption
	 * comes one pair later: on this shape that costs two output frames,
	 * two extra silence units and two trend drops. */
	CHECK_UINT(sim.payload_errors, 24);
	CHECK_UINT(st.frames_out, 222);
	CHECK_UINT(st.silence, 11);
	CHECK_UINT(st.skew_drops, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	/* One pre-learn slot (the default 80-tick span, 10 ms) was reachable at
	 * the step reset. The end of the reach spans the loss gaps of this
	 * one-in-four stream as well, so a reach-end counter reads more than
	 * the slot span sum here. */
	CHECK_UINT(st.resync_discard_ms, 10);
}

/* The far end resumes audio one frame behind an active comfort-noise period.
 * Without the re-anchor, every packet stays late for the rest of the stream. */
static void test_cn_resume_behind(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned resume_tick = 0;
	unsigned i;

	CASE("jb_cn_resume_behind_replays_audio");
	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	for (i = 0; i < 400U; i++) {
		if (i < 100U && !(i % 2))
			sim_send(sim.now_ns);
		if (i == 100U)
			cn_send();
		if (i == 200U)
			sim.ts = sim.jb.playout_ts - UDSP_JB_FRAME_TS;
		if (i >= 200U && !(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
		if (i > 200U && !resume_tick && out.decision == UDSP_JB_FRAME)
			resume_tick = i;
	}
	sim_stats(&st);
	CHECK_UINT(st.cn_reanchors, 1);
	CHECK_UINT(st.late_drop, 0);
	CHECK(resume_tick > 200U && (resume_tick - 200U) * UDSP_JB_FRAME_MS <= 60U);
	CHECK(st.cn > 40);
	CHECK(st.frames_out > 240);
}

/* The emitted payload belongs to the caller until its next get. A pool
 * exhausted by later puts must not move the bytes under the pointer. */
static void test_payload_loan(void)
{
	struct udsp_jb_packet in;
	struct udsp_jb_out out;
	uint8_t canary[80];
	uint8_t filler[160];
	unsigned i;

	CASE("jb_payload_loan_survives_puts");
	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	for (i = 0; i < 20U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	CHECK(out.decision == UDSP_JB_FRAME && out.len == 80U);
	/* On a broken build the warmup can end on a conceal unit with no
	 * payload; the CHECK above already failed, so bail out instead of
	 * copying through a NULL pointer. */
	if (out.decision != UDSP_JB_FRAME || !out.payload || out.len < 80U)
		return;
	memcpy(canary, out.payload, sizeof(canary));
	memset(filler, 0x5a, sizeof(filler));
	memset(&in, 0, sizeof(in));
	in.payload = filler;
	in.len = sizeof(filler);
	in.arrival_ns = sim.now_ns;
	in.ssrc = sim.ssrc;
	in.kind = RTP_PAYLOAD_AUDIO;
	for (i = 0; i < 4U * UDSP_JB_SLOTS; i++) {
		in.rtp_timestamp = sim.ts;
		in.seq = sim.seq;
		sim.ts += 160U;
		sim.seq++;
		udsp_jb_put(&sim.jb, &in);
	}
	CHECK(memcmp(canary, out.payload, sizeof(canary)) == 0);
	udsp_jb_get(&sim.jb, sim.now_ns, &out);
	CHECK(sim.jb.loan < UDSP_JB_SLOTS);
}


/* One packet of the slot takes a wrong, far-ahead timestamp; the stream
 * continues on its own timeline. The buffer holds the outlier until its
 * timestamp comes due; the only loss is the 20 ms hole the mis-stamp left. */
static void outlier_send(uint32_t ahead_ts)
{
	uint32_t save = sim.ts;

	sim.ts += ahead_ts;
	sim_send(sim.now_ns);
	sim.ts = save + sim.span_ms * sim.ts_per_ms;
}

/* Without the loan release, a skew drop that occurs while the loaned slot
 * still heads the queue pops that slot without a pool return. The frame
 * emitted in the same get then overwrites the loan. One slot would leak per
 * occurrence, until the pool is gone and every put overflows. The case needs
 * a ptime above one frame; the 5000 ppm suite case runs at ptime 10 and never
 * sees the leak. */
static void test_skew_fast_leak(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint64_t next;
	unsigned i;

	CASE("jb_skew_drops_leak_no_pool_slot");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	next = sim.now_ns;
	for (i = 0; i < 30000U; i++) {
		while (next <= sim.now_ns) {
			sim_send(next);
			next += 19950000ULL;
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(sim.account_errors, 0);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK(st.skew_drops >= 60);
	CHECK(st.depth_ms <= st.target_ms + 2U * UDSP_JB_FRAME_MS);
	/* The span floor keeps a packet queued at the end, so free plus queued
	 * covers the pool up to the loaned slot. */
	CHECK(st.pool_free + sim.jb.count >= UDSP_JB_SLOTS - 1U);
}

/* The free-window drop after a refill runs on the lagging depth average. On
 * that average alone, a lossless stall-and-burst drains to empty and underruns
 * again. The drop must stop at the depth that the queue actually reaches. */
static void test_stall_burst(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint64_t release = 0;
	unsigned i;

	CASE("jb_stall_burst_trims_to_reachable_depth");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 4000U; i++) {
		if (!(i % 2) && i >= 1000U && i < 1064U) {
			if (i == 1000U)
				release = sim.now_ns + 640U * MS_NS;
			sim_send(release);
		} else if (!(i % 2)) {
			sim_send(sim.now_ns);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(sim.account_errors, 0);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.underruns, 1);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK(st.frames_out > 3800);
	/* Free plus queued covers the pool up to the loaned slot, so the gauge
	 * reads 63 or 64 once the queue is idle; any lower reading is leaked
	 * capacity. */
	CHECK(st.pool_free + sim.jb.count >= UDSP_JB_SLOTS - 1U);
}

/* A single forward outlier is an early packet: it waits in its slot and plays
 * at its own timestamp. No packet of the real stream is late, and a correction
 * must not fire on the depth that the outlier fakes. */
static void test_forward_outlier(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_forward_outlier_waits_without_resync");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 6000U; i++) {
		if (!(i % 2)) {
			if (i == 1000U)
				outlier_send(2000U * 8U);
			else
				sim_send(sim.now_ns);
		}
		if (i == 990U) {
			sim_stats(&st);
			/* the ten-second hold has not elapsed, so nothing is
			 * trimmed before the outlier arrives */
			CHECK_UINT(st.skew_drops, 0);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(st.plc, 2);
	CHECK_UINT(st.underruns, 1);
	/* The outlier itself arms nothing, and under the ten-second hold it
	 * arrives on a still-untrimmed queue, so no insert-and-drop churn
	 * occurs. The drops are the prefill surplus, shed
	 * once the line is clean again. The span floor and the standing reserve
	 * each take one frame off that surplus, so two drops remain. */
	CHECK_UINT(st.skew_drops, 2);
	CHECK_UINT(st.skew_inserts, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.order_errors, 0);
}

/* Periodic outliers keep the tail-based depth near 2 s. A drop armed on that
 * depth would erode the real reserve to zero. The stream would then late-drop
 * all but the outliers: the bench pattern of 20 ms of audio per 500 ms. */
static void test_periodic_outliers(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned sent = 0;
	unsigned i;

	CASE("jb_periodic_outliers_arm_no_trim");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 9000U; i++) {
		if (!(i % 2)) {
			sent++;
			if (sent > 500U && !(sent % 25U))
				outlier_send(2000U * 8U);
			else
				sim_send(sim.now_ns);
		}
		if (i == 990U) {
			sim_stats(&st);
			CHECK_UINT(st.skew_drops, 0);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.late_drop, 0);
	/* The outliers arm nothing, and their holes conceal every half second,
	 * so the trim hold never completes: a line that keeps concealing keeps
	 * its surplus by construction. */
	CHECK_UINT(st.skew_drops, 0);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 8500);
}

/* A sustained forward step below the 10 s resync bound is a timeline move.
 * Concealment of the whole gap runs the queue at 2 s of latency, overflows the
 * pool, and plays a third of the audio for minutes. Two seq-consecutive
 * packets on the new timeline prove the move, and the buffer resyncs at
 * once. */
static void test_forward_step(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned recover = 0;
	unsigned i;

	CASE("jb_forward_step_resyncs_once");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 6000U; i++) {
		if (!(i % 2)) {
			if (i == 2000U)
				sim.ts += 2000U * 8U;
			sim_send(sim.now_ns);
		}
		sim_tick(&out);
		if (i > 2000U && !recover && out.decision == UDSP_JB_FRAME)
			recover = i - 2000U;
	}
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 1);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK(recover > 0 && recover * UDSP_JB_FRAME_MS <= 300U);
	CHECK(st.frames_out >= 5985);
	/* One transit delta of 2 s: the smoothed A.8 peak the CDR would show. */
	CHECK(sim.rtp_jitter_max_ms >= 115 && sim.rtp_jitter_max_ms <= 135);
	/* At the step confirm the head slot sits 2 s beyond the clock, farther
	 * than the buffer could ever cover, so the reachable walk reads empty
	 * and the reset discards no playable audio. A tail-based counter would
	 * read 2010 here, so the check tells the two counters apart. */
	CHECK_UINT(st.resync_discard_ms, 0);
}

/*
 * The shape of a field call, as the buffer sees it. The far end's first
 * packet never reaches the buffer, so its second anchors the buffer and its
 * third steps the timestamp by 2180 ms, marker set and sequence continuous,
 * 20 ms later: the step arrives in PREFILL. A tracker gated on PLAY lets it
 * move the frontier unseen, and the pool overflows for 80 s.
 *
 * pstep_run replays that shape on the stream sim_reset* configured: pre
 * packets on the old timeline (the first one marked when mark_first, as the
 * wire carried it, with [lost_from, lost_from + lost) of them lost), the step
 * packet step_ms of timestamp after the last of them, and then the stream.
 * The successor of the step arrives one packet later, or before any get in
 * the same tick when same_tick is set. The run records the state on each side
 * of the step packet, the resync count right after the successor's put, and
 * the first frame played on the new timeline.
 */
struct pstep {
	struct udsp_jb_stats st;
	uint32_t step_ts;
	uint32_t jitter_before;
	uint32_t jitter_after;
	unsigned recover_ms;
	unsigned target_after_step;
	unsigned target_max;
	uint64_t resyncs_at_successor;
	bool prefill_before;
	bool armed_after;
	bool recovered;
};

static void pstep_successor(struct pstep *r)
{
	struct udsp_jb_stats st;

	sim_send(sim.now_ns);
	sim_deliver(sim.now_ns);
	udsp_jb_stats(&sim.jb, &st);
	r->resyncs_at_successor = st.resyncs;
}

static void pstep_run(unsigned pre, unsigned lost_from, unsigned lost,
		      bool mark_first, uint32_t step_ms, bool same_tick,
		      unsigned ticks, struct pstep *r)
{
	struct udsp_jb_out out;
	unsigned step_tick = 0;
	bool stepped = false;
	unsigned sent = 0;
	unsigned i;

	memset(r, 0, sizeof(*r));
	for (i = 0; i < ticks; i++) {
		if (!(i % 2)) {
			if (sent < pre && sent >= lost_from &&
			    sent < lost_from + lost) {
				sim_skip(1);
			} else if (sent < pre) {
				sim.mark_next = mark_first && !sent;
				sim_send(sim.now_ns);
			} else if (sent == pre) {
				r->prefill_before = sim.jb.state == 1U;
				r->jitter_before = sim.jb.jitter_q4;
				sim.ts += (step_ms - sim.span_ms) *
					  sim.ts_per_ms;
				r->step_ts = sim.ts;
				step_tick = i;
				stepped = true;
				sim_send_marked(sim.now_ns);
				sim_deliver(sim.now_ns);
				r->armed_after = sim.jb.step_agree != 0;
				r->jitter_after = sim.jb.jitter_q4;
				if (same_tick) {
					pstep_successor(r);
					sent++;
				}
			} else if (sent == pre + 1U && !same_tick) {
				pstep_successor(r);
			} else {
				sim_send(sim.now_ns);
			}
			sent++;
		}
		sim_tick(&out);
		if (stepped && i == step_tick)
			r->target_after_step = sim.jb.target_ms;
		if (sim.jb.target_ms > r->target_max)
			r->target_max = sim.jb.target_ms;
		if (stepped && !r->recovered && out.decision == UDSP_JB_FRAME &&
		    (int32_t)(out.rtp_timestamp - r->step_ts) >= 0) {
			r->recovered = true;
			r->recover_ms = (i - step_tick) * UDSP_JB_FRAME_MS;
		}
	}
	sim_stats(&r->st);
}

/* The assertions every prefill step case shares: the step arrived in PREFILL
 * and armed there, the successor's put confirmed it (a late recovery through
 * a re-arm reads 0 here), and the stream recovered with no drop. Neither
 * estimator read the marked step as jitter: the target never left the 30 ms
 * the stream configured, plus one frame, and the modeled RTP estimate
 * (sim_rtp_jitter, not rtp.c) stays at 0. */
static void pstep_check(const struct pstep *r)
{
	CHECK(r->prefill_before);
	CHECK(r->armed_after);
	CHECK_UINT(r->resyncs_at_successor, 1);
	CHECK_UINT(r->st.resyncs, 1);
	CHECK_UINT(r->st.starve_resyncs, 0);
	CHECK_UINT(r->st.overflow_drop, 0);
	CHECK_UINT(r->st.late_drop, 0);
	CHECK(r->recovered && r->recover_ms <= 300U);
	CHECK(r->target_max <= 30U + UDSP_JB_FRAME_MS);
	CHECK(sim.rtp_jitter_max_ms <= 1U);
	CHECK_UINT(sim.account_errors, 0);
	CHECK_UINT(sim.order_errors, 0);
}

/*
 * The trace shape of the field call runs on six stream forms. Each form must
 * resync once, at the successor of the step, with no drop. Each frame floor
 * sits 100 ms under the measured count.
 */
static void test_prefill_step(void)
{
	struct pstep r;

	/* With the step tracker gated on PLAY, the trace shape overflows 759
	 * packets, and no resync runs. */

	CASE("jb_prefill_step_resyncs_once");
	sim_reset_sid(UDSP_JB_ADAPTIVE, 30, 250, 20, 80, 0);
	pstep_run(1, 0, 0, false, 2180, false, 6000, &r);
	pstep_check(&r);
	CHECK_UINT(r.recover_ms, 40);
	CHECK(r.st.frames_out >= 5986);
	CHECK_UINT(sim.payload_errors, 0);

	/* A stream with no stated frame size learns its span. The anchor plays
	 * before any pair teaches it, and the marked step teaches nothing, so
	 * the anchor and the first packet after the reset each play as one
	 * 10 ms unit: two wrong-length slices. That cost is the learner's
	 * startup, not the step's; the stream codecs all state a frame size. */
	CASE("jb_prefill_step_learned_span");
	sim_reset(UDSP_JB_ADAPTIVE, 30, 250, 20, 80);
	pstep_run(1, 0, 0, false, 2180, false, 6000, &r);
	pstep_check(&r);
	CHECK_UINT(r.recover_ms, 40);
	CHECK(r.st.frames_out >= 5984);
	CHECK_UINT(sim.payload_errors, 2);

	CASE("jb_prefill_step_wideband_whole");
	sim_reset_cfg(UDSP_JB_ADAPTIVE, 30, 250, 20, 40, 48U, true);
	pstep_run(1, 0, 0, false, 2180, false, 6000, &r);
	pstep_check(&r);
	CHECK_UINT(r.recover_ms, 40);
	CHECK(r.st.frames_out >= 5986);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.unit_errors, 0);

	CASE("jb_prefill_step_fixed");
	sim_reset_sid(UDSP_JB_FIXED, 30, 250, 20, 80, 0);
	pstep_run(1, 0, 0, false, 2180, false, 6000, &r);
	pstep_check(&r);
	CHECK_UINT(r.recover_ms, 40);
	CHECK(r.st.frames_out >= 5986);
	CHECK_UINT(sim.payload_errors, 0);

	CASE("jb_prefill_step_below_pool");
	sim_reset_sid(UDSP_JB_ADAPTIVE, 30, 250, 20, 80, 0);
	pstep_run(1, 0, 0, false, 800, false, 6000, &r);
	pstep_check(&r);
	CHECK_UINT(r.recover_ms, 40);
	CHECK(r.st.frames_out >= 5976);
	CHECK_UINT(sim.payload_errors, 0);

	/* 100 ms of loss parks the clock after 8 s of play, the resume refills
	 * into PREFILL, and the step is the packet after the resume. */
	CASE("jb_refill_prefill_step");
	sim_reset_sid(UDSP_JB_ADAPTIVE, 30, 250, 20, 80, 0);
	pstep_run(400, 394, 5, false, 2180, false, 6000, &r);
	pstep_check(&r);
	CHECK_UINT(r.recover_ms, 40);
	CHECK(r.st.frames_out >= 5976);
	CHECK_UINT(sim.payload_errors, 0);
}

/* The step and its successor arrive in one tick at call start, before any
 * frame plays. The successor teaches the first span there, and that learn must
 * keep the candidate. The derived span and the learned span both confirm on
 * the successor. A learn that clears the candidate overflows 783 packets in
 * both forms, with no resync. */
static void test_prefill_step_same_tick(void)
{
	struct pstep r;

	CASE("jb_prefill_step_same_tick");
	sim_reset_sid(UDSP_JB_ADAPTIVE, 30, 250, 20, 80, 0);
	pstep_run(1, 0, 0, false, 2180, true, 6000, &r);
	pstep_check(&r);
	CHECK_UINT(r.recover_ms, 20);
	CHECK(r.st.frames_out >= 5986);
	CHECK_UINT(sim.payload_errors, 0);

	CASE("jb_prefill_step_same_tick_learned");
	sim_reset(UDSP_JB_ADAPTIVE, 30, 250, 20, 80);
	pstep_run(1, 0, 0, false, 2180, true, 6000, &r);
	pstep_check(&r);
	CHECK_UINT(r.recover_ms, 20);
	CHECK(r.st.frames_out >= 5986);
	CHECK_UINT(sim.payload_errors, 0);
}

/* The two-packet proof holds in PREFILL as it does in PLAY. At cold start a
 * lone far-ahead packet, with on-timeline traffic after it, and a swapped pair
 * must not resync the stream. */
static void test_prefill_stray_no_resync(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint32_t held_ts = 0;
	uint16_t held_seq = 0;
	unsigned i;

	CASE("jb_prefill_stray_no_resync");
	sim_reset_sid(UDSP_JB_ADAPTIVE, 30, 250, 20, 80, 0);
	for (i = 0; i < 6000U; i++) {
		if (i == 2U) {
			outlier_send(2180U * 8U);
		} else if (i == 6U) {
			held_ts = sim.ts;
			held_seq = sim.seq;
			sim_skip(1);
			sim_send(sim.now_ns);
			sim_queue(sim.now_ns + TICK_NS, held_ts, held_seq,
				  RTP_PAYLOAD_AUDIO, 2U * sim.bpf);
		} else if (!(i % 2)) {
			sim_send(sim.now_ns);
		}
		if (i == 2U)
			CHECK_UINT(sim.jb.state, 1);
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(st.late_drop, 0);
	/* 5947 measured, the same with the tracker gated on PLAY */
	CHECK(st.frames_out >= 5937);
	CHECK_UINT(sim.account_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
}

/* The control: the same step on the wire shape, with the first packet
 * delivered as well. rtp.c gives this shape, because it releases the packet
 * that RFC 3550 A.1 probation holds. The two pre-step packets fill the 30 ms
 * target, the prefill ends before the step, and the tracker in PLAY claims
 * it. This case passes with and without the PREFILL tracker. */
static void test_prefill_step_wire_shape(void)
{
	struct pstep r;

	CASE("jb_prefill_step_wire_shape");
	sim_reset_sid(UDSP_JB_ADAPTIVE, 30, 250, 20, 80, 0);
	pstep_run(2, 0, 0, true, 2180, false, 6000, &r);
	CHECK(!r.prefill_before);
	CHECK_UINT(r.resyncs_at_successor, 1);
	CHECK_UINT(r.st.resyncs, 1);
	CHECK_UINT(r.st.overflow_drop, 0);
	CHECK_UINT(r.st.late_drop, 0);
	CHECK(r.recovered && r.recover_ms <= 300U);
	CHECK_UINT(sim.account_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
}

/* The marked step of the trace shape adds no jitter observation. The estimate
 * is the same on each side of the step packet. The target stays at 30 ms
 * through the get that ends the prefill. Read as jitter, the step takes the
 * estimate to 1080 ticks (135 ms) and the target to the 250 ms maximum. */
static void test_jitter_skips_marked_step(void)
{
	struct pstep r;

	CASE("jb_jitter_skips_marked_step");
	sim_reset_sid(UDSP_JB_ADAPTIVE, 30, 250, 20, 80, 0);
	pstep_run(1, 0, 0, false, 2180, false, 600, &r);
	CHECK_UINT(r.jitter_after, r.jitter_before);
	CHECK_UINT(r.target_after_step, 30);
}

/* One packet put with no get after it. The estimator reads the put alone, so
 * a case can state the exact RFC 3550 A.8 value of the estimate. */
static void jput(uint64_t at_ns, uint32_t ts, bool marker, uint8_t kind)
{
	unsigned len = kind == RTP_PAYLOAD_AUDIO ?
		       sim.span_ms / UDSP_JB_FRAME_MS * sim.bpf : 1U;

	sim.mark_next = marker;
	sim_queue(at_ns, ts, sim.seq++, kind, len);
	sim_deliver(at_ns);
}

/* Two audio packets on time seed the estimate at 0 and teach the span. The
 * third is the probe: late_ns after its due arrival, ahead_ts past its due
 * timestamp. The result is the estimate after the probe. */
static uint32_t jitter_probe(uint64_t late_ns, uint32_t ahead_ts, bool marker)
{
	uint64_t gap_ns = sim.span_ms * MS_NS;
	uint32_t span = sim.span_ms * sim.ts_per_ms;
	uint64_t t0 = sim.now_ns;
	uint32_t ts0 = sim.ts;

	jput(t0, ts0, false, RTP_PAYLOAD_AUDIO);
	jput(t0 + gap_ns, ts0 + span, false, RTP_PAYLOAD_AUDIO);
	jput(t0 + 2U * gap_ns + late_ns, ts0 + 2U * span + ahead_ts, marker,
	     RTP_PAYLOAD_AUDIO);
	return sim.jb.jitter_q4;
}

/* The rule reads the marker. The same 2 s forward step with no marker is one
 * observation of 16000 ticks, and its marked twin is none. */
static void test_jitter_counts_unmarked_step(void)
{
	CASE("jb_jitter_counts_unmarked_step");
	sim_reset_sid(UDSP_JB_ADAPTIVE, 30, 250, 20, 80, 0);
	CHECK_UINT(jitter_probe(0, 2000U * 8U, false), 16000);
	sim_reset_sid(UDSP_JB_ADAPTIVE, 30, 250, 20, 80, 0);
	CHECK_UINT(jitter_probe(0, 2000U * 8U, true), 0);
}

/* The rule reads the sign. A marked packet 300 ms late is one observation of
 * 2400 ticks. That is above the 2160-tick bound. RFC 3551 4.1 lets a
 * talkspurt start move the playout for a changed network delay, so the
 * estimate must see it. */
static void test_jitter_counts_marked_delay(void)
{
	CASE("jb_jitter_counts_marked_delay");
	sim_reset_sid(UDSP_JB_ADAPTIVE, 30, 250, 20, 80, 0);
	CHECK_UINT(jitter_probe(300U * MS_NS, 0, true), 2400);
}

/* A skipped marked step keeps a nonzero estimate as it was, and the step
 * packet is the reference of the next observation. The second packet is
 * 5 ms late, 40 ticks, and the step leaves that estimate. The next packet
 * comes four sequence numbers later, so it cannot confirm the step, and no
 * stream reset clears the estimate. It is on time against the step packet:
 * one observation of 0, and the estimate decays to 37. */
static void test_jitter_marked_step_keeps_reference(void)
{
	uint64_t t0;
	uint32_t ts0;

	CASE("jb_jitter_marked_step_keeps_reference");
	sim_reset_sid(UDSP_JB_ADAPTIVE, 30, 250, 20, 80, 0);
	t0 = sim.now_ns;
	ts0 = sim.ts;
	jput(t0, ts0, false, RTP_PAYLOAD_AUDIO);
	jput(t0 + 25U * MS_NS, ts0 + 160U, false, RTP_PAYLOAD_AUDIO);
	CHECK_UINT(sim.jb.jitter_q4, 40);
	jput(t0 + 40U * MS_NS, ts0 + 320U + 16000U, true, RTP_PAYLOAD_AUDIO);
	CHECK_UINT(sim.jb.jitter_q4, 40);
	sim.seq += 3U;
	jput(t0 + 120U * MS_NS, ts0 + 960U + 16000U, false, RTP_PAYLOAD_AUDIO);
	CHECK_UINT(sim.jb.jitter_q4, 40U - ((40U + 8U) >> 4));
	CHECK_UINT(sim.jb.total.resyncs, 0);
}

/* The bound is max_ms plus the learned span, and it is strict. A marked step
 * of exactly the bound is still an observation, and one tick more is none.
 * The case runs at 8 kHz and at 48 kHz. */
static void test_jitter_marked_bound_edges(void)
{
	uint32_t bound;

	CASE("jb_jitter_marked_bound_edges");
	sim_reset_sid(UDSP_JB_ADAPTIVE, 30, 250, 20, 80, 0);
	bound = 250U * 8U + 160U;
	CHECK_UINT(jitter_probe(0, bound, true), bound);
	sim_reset_sid(UDSP_JB_ADAPTIVE, 30, 250, 20, 80, 0);
	CHECK_UINT(jitter_probe(0, bound + 1U, true), 0);

	sim_reset_cfg(UDSP_JB_ADAPTIVE, 30, 250, 20, 40, 48U, true);
	bound = 250U * 48U + 960U;
	CHECK_UINT(jitter_probe(0, bound, true), bound);
	sim_reset_cfg(UDSP_JB_ADAPTIVE, 30, 250, 20, 40, 48U, true);
	CHECK_UINT(jitter_probe(0, bound + 1U, true), 0);
}

static void jitter_every_run(bool mark, uint32_t *q4,
			     uint32_t peak[UDSP_JB_WINDOW_BUCKETS])
{
	unsigned i;

	sim_reset_sid(UDSP_JB_ADAPTIVE, 30, 250, 20, 80, 0);
	for (i = 0; i < 1000U; i++) {
		jput(sim.now_ns + (uint64_t)(rng_next() % 41U) * MS_NS, sim.ts,
		     mark && i >= 100U, RTP_PAYLOAD_AUDIO);
		sim.ts += 160U;
		sim.now_ns += 20U * MS_NS;
	}
	*q4 = sim.jb.jitter_q4;
	memcpy(peak, sim.jb.window_peak, sizeof(sim.jb.window_peak));
}

/* A sender that marks every packet, on a line with 0 to 40 ms of transit
 * jitter, below the bound. After the same unmarked prefix, the marked run and
 * the unmarked run must hold the same estimate. They must also hold the same
 * 2 s high-water, which the target reads. The runs make no get. With a marker
 * on every packet, the talkspurt skip moves the clock, and the playout would
 * differ for a reason outside the estimator. */
static void test_jitter_marker_every_packet(void)
{
	uint32_t peak_plain[UDSP_JB_WINDOW_BUCKETS];
	uint32_t peak_marked[UDSP_JB_WINDOW_BUCKETS];
	uint32_t q4_plain;
	uint32_t q4_marked;

	CASE("jb_jitter_marker_every_packet");
	jitter_every_run(false, &q4_plain, peak_plain);
	jitter_every_run(true, &q4_marked, peak_marked);
	CHECK(q4_plain > 0);
	CHECK_UINT(q4_marked, q4_plain);
	CHECK(!memcmp(peak_marked, peak_plain, sizeof(peak_plain)));
}

/* The rule is for audio only. Comfort noise far ahead of the frontier is
 * refused anyway. The case therefore builds the one shape where a trusted
 * comfort-noise packet has a large negative variation. A comfort-noise packet
 * stamped 1 s behind (+8000 ticks) is the reference. The next one, marked and
 * back on the timeline, reads -8000 and must still count. */
static void test_jitter_counts_marked_cn(void)
{
	uint64_t t0;
	uint32_t ts0;

	CASE("jb_jitter_counts_marked_cn");
	sim_reset_sid(UDSP_JB_ADAPTIVE, 30, 250, 20, 80, 0);
	t0 = sim.now_ns;
	ts0 = sim.ts;
	jput(t0, ts0, false, RTP_PAYLOAD_AUDIO);
	jput(t0 + 20U * MS_NS, ts0 + 160U, false, RTP_PAYLOAD_AUDIO);
	jput(t0 + 40U * MS_NS, ts0 + 320U - 8000U, false,
	     RTP_PAYLOAD_COMFORT_NOISE);
	CHECK_UINT(sim.jb.jitter_q4, 8000);
	jput(t0 + 60U * MS_NS, ts0 + 480U, true, RTP_PAYLOAD_COMFORT_NOISE);
	CHECK_UINT(sim.jb.jitter_q4, 8000U + 8000U - ((8000U + 8U) >> 4));
}

/* The far end enters comfort noise, freezes its RTP timestamp through the
 * silence, and resumes 2 s behind the free-running playout clock. The max_ms
 * bound alone refuses the resume. Every audio packet then late-drops for the
 * rest of the stream while cn climbs: a livelock. */
static void test_cn_frozen_resume(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned frames_after = 0;
	unsigned i;

	CASE("jb_cn_resume_reanchors_behind_clock");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 6000U; i++) {
		if (!(i % 2) && i < 2000U)
			sim_send(sim.now_ns);
		if (i == 2000U)
			cn_send();
		if (!(i % 2) && i >= 2200U)
			sim_send(sim.now_ns);
		sim_tick(&out);
		if (i > 2200U && out.decision == UDSP_JB_FRAME)
			frames_after++;
	}
	sim_stats(&st);
	CHECK_UINT(st.cn_reanchors, 1);
	CHECK(st.late_drop <= 1);
	CHECK(frames_after > 1800);
	CHECK_UINT(sim.account_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.order_errors, 0);
	/* The 2 s frozen-timestamp silence is one transit delta near 2 s. */
	CHECK(sim.rtp_jitter_max_ms >= 115 && sim.rtp_jitter_max_ms <= 135);
}

/* A backward step of any size costs the queued depth in late drops, one
 * parked get, and one refill. The stream then continues on the old timeline.
 * This case checks the verdict that the backward direction is cheap. */
static void test_backward_step(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_backward_step_continues_without_resync");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 6000U; i++) {
		if (!(i % 2)) {
			if (i == 2000U)
				sim.ts -= 2000U * 8U;
			sim_send(sim.now_ns);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK(st.late_drop <= 3);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK(st.frames_out > 5700);
}

/* A stray old packet delivered while the buffer refills would re-anchor the
 * whole stream 800 ms into the past. The gap back to the live timeline would
 * then be concealed. Only a seq-consecutive packet, or one within max_ms, can
 * pull the prefill clock back. */
static void test_prefill_straggler(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint32_t old_ts = 0;
	uint16_t old_seq = 0;
	unsigned i;

	CASE("jb_prefill_straggler_late_dropped");
	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	for (i = 0; i < 1000U; i++) {
		if (i == 160U) {
			old_ts = sim.ts;
			old_seq = sim.seq;
			sim_skip(1);
		} else if (!(i % 2) && (i < 200U || i >= 240U)) {
			sim_send(sim.now_ns);
		}
		if (i == 238U)
			sim_queue(sim.now_ns, old_ts, old_seq,
				  RTP_PAYLOAD_AUDIO, 2U * sim.bpf);
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.late_drop, 1);
	CHECK_UINT(st.dup_drop, 0);
	CHECK(st.plc + st.silence <= 50U + 4U);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 940);
}

/* An early packet does not reset the recovery machinery into a late-drop
 * livelock. The behind timeline re-anchors through the refill and plays. */
static void test_early_packet_no_livelock(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned sent = 0;
	unsigned i;

	CASE("jb_early_packets_no_late_drop_livelock");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 6000U; i++) {
		if (!(i % 2)) {
			sent++;
			if (i == 2000U)
				sim.ts -= 300U * 8U;
			if (sent > 1000U && !(sent % 40U))
				outlier_send(500U * 8U);
			else
				sim_send(sim.now_ns);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK(st.late_drop <= 10);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK(st.frames_out > 5500);
}


/* The first packet of the step is lost in the network. The detector must still
 * prove the move from the surviving packets: a seq agreement of 1..3
 * tolerates the lost boundary packet. */
static void test_step_first_packet_lost(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned recover = 0;
	unsigned i;

	CASE("jb_step_confirms_with_first_packet_lost");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 6000U; i++) {
		if (!(i % 2)) {
			/* the first post-step packet is lost, and so is the one
			 * after the survivor: the confirmation comes at a
			 * sequence agreement of two */
			if (i == 2000U) {
				sim.ts += 2000U * 8U;
				sim_skip(1);
			}
			if (i == 2002U)
				sim_skip(1);
			sim_send(sim.now_ns);
		}
		sim_tick(&out);
		if (i > 2000U && !recover && out.decision == UDSP_JB_FRAME)
			recover = i - 2000U;
	}
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 1);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK(recover > 0 && recover * UDSP_JB_FRAME_MS <= 400U);
	CHECK(st.frames_out > 5950);
}

/* The first two post-step packets arrive swapped. The candidate re-arms on
 * the older one and the third packet confirms. */
static void test_step_swapped_start(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned recover = 0;
	uint32_t held_ts = 0;
	uint16_t held_seq = 0;
	unsigned i;

	CASE("jb_step_confirms_with_swapped_start");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 6000U; i++) {
		if (!(i % 2)) {
			if (i == 2000U) {
				sim.ts += 2000U * 8U;
				held_ts = sim.ts;
				held_seq = sim.seq;
				sim_skip(1);
				sim_send(sim.now_ns);
				sim_queue(sim.now_ns, held_ts, held_seq,
					  RTP_PAYLOAD_AUDIO, 2U * sim.bpf);
			} else {
				sim_send(sim.now_ns);
			}
		}
		sim_tick(&out);
		if (i > 2000U && !recover && out.decision == UDSP_JB_FRAME)
			recover = i - 2000U;
	}
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 1);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK(st.late_drop <= 1);
	CHECK(recover > 0 && recover * UDSP_JB_FRAME_MS <= 400U);
	CHECK(st.frames_out > 5900);
}

/* A lossless 500 ms burst leaves a deep queue, and one seq-consecutive packet
 * then carries a backward mis-stamp. A detector that arms against the
 * regressed last-received timestamp resyncs away the whole queue. The arming
 * reference is the stream frontier stream_end_ts, which never regresses, so
 * nothing fires. */
static void test_burst_misstamp_no_resync(void)
{
	static const uint32_t back_ms[3] = { 150U, 300U, 1000U };
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint64_t release;
	unsigned k, i;

	for (k = 0; k < 3U; k++) {
		CASE(k == 0 ? "jb_burst_misstamp_150_no_resync" :
		     k == 1 ? "jb_burst_misstamp_300_no_resync" :
			      "jb_burst_misstamp_1000_no_resync");
		sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
		release = 0;
		for (i = 0; i < 4000U; i++) {
			if (!(i % 2) && i >= 1000U && i < 1050U) {
				if (i == 1000U)
					release = sim.now_ns + 500U * MS_NS;
				sim_send(release);
			} else if (!(i % 2)) {
				if (i == 1050U) {
					/* sim_send consumed the slot: the next
					 * packet stays seq-consecutive */
					sim.ts -= back_ms[k] * 8U;
					sim_send(sim.now_ns);
					sim.ts += back_ms[k] * 8U;
				} else {
					sim_send(sim.now_ns);
				}
			}
			sim_tick(&out);
		}
		sim_stats(&st);
		CHECK_UINT(st.resyncs, 0);
		CHECK_UINT(st.starve_resyncs, 0);
		CHECK_UINT(st.overflow_drop, 0);
		CHECK_UINT(sim.account_errors, 0);
		CHECK_UINT(sim.payload_errors, 0);
		CHECK(st.late_drop <= 1);
		CHECK(st.frames_out > 3800);
	}
}

/* Reorder inside an ordinary burst release stays a reorder. */
static void test_burst_reorder_no_resync(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint64_t release = 0;
	uint32_t held_ts = 0;
	uint16_t held_seq = 0;
	unsigned i;

	CASE("jb_burst_reorder_no_resync");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 4000U; i++) {
		if (!(i % 2) && i >= 1000U && i < 1030U) {
			if (i == 1000U)
				release = sim.now_ns + 300U * MS_NS;
			if (i == 1010U) {
				held_ts = sim.ts;
				held_seq = sim.seq;
				sim_skip(1);
				sim_send(release);
				sim_queue(release, held_ts, held_seq,
					  RTP_PAYLOAD_AUDIO, 2U * sim.bpf);
			} else {
				sim_send(release);
			}
		} else if (!(i % 2)) {
			sim_send(sim.now_ns);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK(st.frames_out > 3800);
}

/* UDSP_JB_OFF keeps no recovery machinery, so a step burst delivered inside
 * one tick must not resync. jb_off_anchor follows the queue head instead. */
static void test_off_step_burst_no_resync(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_off_step_burst_no_resync");
	sim_reset(UDSP_JB_OFF, 50, 100, 20, 80);
	for (i = 0; i < 400U; i++) {
		if (!(i % 2)) {
			if (i == 200U) {
				sim.ts += 2000U * 8U;
				sim_send(sim.now_ns);
			}
			sim_send(sim.now_ns);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK(st.frames_out > 360);
}

/* The confirming packet re-seeds the replay window after the step reset,
 * exactly as the timestamp jump path does, so its duplicate stays caught. */
static void test_step_dup_confirm_rejected(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint32_t confirm_ts = 0;
	uint16_t confirm_seq = 0;
	unsigned i;

	CASE("jb_step_confirm_duplicate_rejected");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 4000U; i++) {
		if (!(i % 2)) {
			if (i == 2000U)
				sim.ts += 2000U * 8U;
			if (i == 2002U) {
				confirm_ts = sim.ts;
				confirm_seq = sim.seq;
			}
			sim_send(sim.now_ns);
		}
		if (i == 2003U)
			sim_queue(sim.now_ns, confirm_ts, confirm_seq,
				  RTP_PAYLOAD_AUDIO, 2U * sim.bpf);
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 1);
	CHECK_UINT(st.dup_drop, 1);
	CHECK_UINT(sim.dup, 1);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
}

/* The safety half of the comfort-noise resume rule. A stray old packet during
 * an armed comfort-noise period is not the resume of the far end. It is never
 * seq-consecutive, and the max_ms bound must keep rejecting it. */
static void test_cn_straggler_stays_late(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint32_t old_ts = 0;
	uint16_t old_seq = 0;
	unsigned i;

	CASE("jb_cn_straggler_stays_late");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 3000U; i++) {
		if (i == 1900U) {
			old_ts = sim.ts;
			old_seq = sim.seq;
			sim_skip(1);
		} else if (!(i % 2) && i < 2000U) {
			sim_send(sim.now_ns);
		}
		if (i == 2000U)
			cn_send();
		if (i == 2200U)
			sim_queue(sim.now_ns, old_ts, old_seq,
				  RTP_PAYLOAD_AUDIO, 2U * sim.bpf);
		if (!(i % 2) && i >= 2400U)
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.late_drop, 1);
	CHECK_UINT(st.cn_reanchors, 1);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.dup_drop, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 2500);
}

/* The drop gate must read the reachable depth, not the tail, and must block
 * at equality. A released burst whose middle exceeds the max_ms gap tolerance
 * leaves the tail and the reach apart. The tail says trim, and the reach says
 * that the playable run is already at the target. */
static void test_gate_reach_boundary(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint64_t release = 0;
	uint32_t save_ts = 0;
	unsigned i;

	CASE("jb_drop_gate_stops_at_reachable_depth");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 2000U; i++) {
		if (!(i % 2) && i >= 1500U && i < 1564U) {
			/* 32 packets delayed 640 ms and released together; the
			 * 30th carries a +3 s mis-stamp, so after the release
			 * the tail reads 3 s while the reach is the 640 ms the
			 * queue playably holds. The free-window trim on the
			 * lagging average must stop at the reach: a tail-based
			 * gate would drain the run to empty and late-drop the
			 * continuing stream. */
			if (i == 1500U)
				release = sim.now_ns + 640U * MS_NS;
			if (i == 1558U) {
				save_ts = sim.ts;
				sim.ts += 3000U * 8U;
				sim_send(release);
				sim.ts = save_ts + 160U;
			} else {
				sim_send(release);
			}
		} else if (!(i % 2)) {
			sim_send(sim.now_ns);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	/* Two concealment runs start: the stall itself and the 20 ms hole the
	 * mis-stamped packet left in the released queue. */
	CHECK(st.underruns <= 2);
	/* Deterministic decomposition: 58 free-window drops trim the released
	 * burst, and the trim hold, restarted by the stall's concealment,
	 * releases three more trim drops once the line is clean again inside
	 * the run. The standing reserve raises the setpoint, so the free-window
	 * run stops one drop earlier. */
	CHECK_UINT(st.skew_drops, 60);
	CHECK(st.frames_out > 1800);
}

/* Comfort noise must not arm a step candidate: a junk CN far ahead and an
 * audio packet with the same mis-stamp would agree. The buffer would then
 * resync onto garbage; the deferred refill anchor instead holds the junk audio
 * in the micro-park that the CN hole left. The clock stays parked, and neither
 * junk packet feeds a jitter observation. The snapshot before the junk shows
 * that the drops are the pre-junk prefill shed, and that the junk event adds
 * zero. The case needs jb_parked, which needs an empty queue. Three lost
 * packets drain the reserve of the floor and the packet in flight before the
 * outlier arrives.
 */
static void test_junk_cn_no_arm(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned bpf;
	unsigned i;

	CASE("jb_junk_cn_arms_no_candidate");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 4000U; i++) {
		if (!(i % 2)) {
			if (i == 2000U) {
				bpf = sim.bpf;
				sim.bpf = 1U;
				sim_queue(sim.now_ns, sim.ts + 2000U * 8U,
					  sim.seq, RTP_PAYLOAD_COMFORT_NOISE, 2);
				sim.bpf = bpf;
				sim_skip(1);
			} else if (i > 2000U && i < 2006U) {
				sim_skip(1);
			} else if (i == 2006U) {
				outlier_send(2000U * 8U + 160U);
			} else {
				sim_send(sim.now_ns);
			}
		}
		if (i == 1990U) {
			sim_stats(&st);
	/* the span floor and the standing reserve each take one frame off the
	 * prefill surplus */
			CHECK_UINT(st.skew_drops, 2);
			CHECK_UINT(st.skew_inserts, 0);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.skew_drops, 2);
	CHECK_UINT(st.skew_inserts, 0);
	CHECK_UINT(st.hold_drop, 1);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	/* jb_cn_resume fires only on a packet that catches the queue empty, and
	 * the floor keeps a span of reserve, so the armed junk CN period ends
	 * by the clock passing cn_ts instead of by a re-anchor. The obligations
	 * above (resyncs 0, hold_drop 1, late_drop 0) hold with the reserve. */
	CHECK_UINT(st.cn_reanchors, 0);
	/* the drain of the reserve conceals five frames more than a floor
	 * with no reserve */
	CHECK_UINT(st.plc, 8);
	CHECK_UINT(st.cn, 0);
	CHECK_UINT(st.frames_out, 3988);
	/* the settled floor is one packet span plus one frame */
	CHECK_UINT(st.target_ms, 30);
	/* the queue holds its reserve where a floor with no reserve runs
	 * empty */
	CHECK_UINT(st.depth_ms, 20);
	/* one slot carries the reserve */
	CHECK_UINT(st.pool_free, 62);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* The FIXED variant: with a 60 ms queue, the clock is not parked when the
 * outlier arrives. The refill therefore cannot mask a candidate that the CN
 * must never arm. An implementation that lets comfort noise arm therefore
 * fails here. The hold runs in ADAPTIVE only, like the offset trim, so every
 * counter checked here matches the behavior without the hold. The estimator
 * state differs: the distrust clause refuses the far junk CN, which the
 * estimator would otherwise observe. In FIXED, no checked counter can see that
 * difference. */
static void test_junk_cn_no_arm_fixed(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned bpf;
	unsigned i;

	CASE("jb_junk_cn_arms_no_candidate_fixed");
	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	for (i = 0; i < 4000U; i++) {
		if (!(i % 2)) {
			if (i == 2000U) {
				bpf = sim.bpf;
				sim.bpf = 1U;
				sim_queue(sim.now_ns, sim.ts + 2000U * 8U,
					  sim.seq, RTP_PAYLOAD_COMFORT_NOISE, 2);
				sim.bpf = bpf;
				sim_skip(1);
			} else if (i == 2002U) {
				outlier_send(2000U * 8U + 160U);
			} else {
				sim_send(sim.now_ns);
			}
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.hold_drop, 0);
	CHECK_UINT(st.skew_drops, 0);
	CHECK_UINT(st.skew_inserts, 0);
	CHECK_UINT(st.frames_out, 3992);
	CHECK_UINT(sim.account_errors, 0);
}

/* The far resume after one second of loss is held once, and its confirming
 * successor adopts it. In this shape, an arm and a refill therefore never
 * share a put: while the clock is parked, the queue is empty. The void bound
 * and the far bound cover the window between them, so every parked audio
 * packet voids the candidate (near) or is held (far). The candidate clear of
 * the refill stays as a defense: a bound adoption, after two consecutive
 * refusals, refills with the candidate still armed. The candidate sits on the
 * previous held packet, whose continuation is the adopted timeline itself.
 * The case checks the deferred cost: one hold_drop and one extra span of
 * concealment. */
static void test_refill_clears_candidate(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_refill_clears_step_candidate");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 6000U; i++) {
		/* One second of loss parks the clock. The resume packet arrives
		 * a second ahead of the parked position: the deferred anchor
		 * refuses it once, its successor confirms, and the refill
		 * adopts with no resync and no correction churn. */
		if (!(i % 2)) {
			if (i >= 3000U && i < 3100U)
				sim_skip(1);
			else
				sim_send(sim.now_ns);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.hold_drop, 1);
	/* one more cold-start silence frame before the deeper prefill
	 * completes */
	CHECK_UINT(st.silence, 66);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(st.underruns, 1);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 5800);
}

/* On-timeline traffic voids a pending candidate. Two lone outliers three
 * sequence numbers apart with real packets between them must never read as an
 * agreeing pair. */
static void test_outlier_pair_no_resync(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_outlier_pair_no_resync");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 4000U; i++) {
		if (!(i % 2)) {
			if (i == 2000U || i == 2006U)
				outlier_send(2000U * 8U);
			else
				sim_send(sim.now_ns);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK(st.frames_out > 3900);
}

/* A long stall at the sender or on the path releases its backlog more slowly
 * than the engine drain of 16 slots per tick. The stall then never fills the
 * net ring, and loses nothing at any stall length. The backlog still exceeds
 * the 64-slot pool. A free-window trim that leaks slots exhausts the pool on
 * this shape, as on a bench call. The case reproduces that call with that one
 * mechanism. */
static void test_paced_stall_release(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint64_t next = 0;
	unsigned backlog = 0;
	unsigned i;

	CASE("jb_paced_stall_release_no_leak");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 6000U; i++) {
		if (!(i % 2)) {
			if (i == 2000U)
				next = sim.now_ns + 1920U * MS_NS;
			if (i >= 2000U && next > sim.now_ns) {
				/* The path queue drains in order at 400
				 * packets per second until it catches up. */
				backlog++;
				sim_send(next);
				next += 2500000ULL;
			} else {
				sim_send(sim.now_ns);
			}
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK(backlog >= 96 && backlog <= 112);
	/* The backlog exceeds the pool by 96 - 64 packets: the one-time
	 * overflow is bounded by that; a decayed pool_free or growing
	 * overflow is the leak. */
	CHECK(st.overflow_drop <= 40);
	CHECK(st.pool_free + sim.jb.count >= UDSP_JB_SLOTS - 1U);
	CHECK(st.frames_out > 5500);
	CHECK(sim.rtp_jitter_max_ms >= 100 && sim.rtp_jitter_max_ms <= 135);
}


/* A replica of the acceptance call. The time-capped prefill parks 40 ms, and
 * the adaptive target decays to its floor. A far end 10 ppm fast then grows the
 * surplus for the rest of the call. The offset trim sheds that surplus at
 * exactly one frame per second down to the setpoint, and then stops. */
static void test_offset_trim_clean(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint64_t next;
	uint64_t drops_at_60s = 0;
	unsigned last_drop = 0;
	unsigned gap_min = 1000, gap_max = 0;
	unsigned i;

	CASE("jb_offset_trim_clean");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	next = sim.now_ns;
	for (i = 0; i < 12000U; i++) {
		while (next <= sim.now_ns) {
			sim_send(next);
			next += 19999800ULL;
		}
		sim_tick(&out);
		if (out.skew_drop) {
			if (last_drop) {
				if (i - last_drop < gap_min)
					gap_min = i - last_drop;
				if (i - last_drop > gap_max)
					gap_max = i - last_drop;
			}
			last_drop = i;
		}
		if (i == 1000U) {
			sim_stats(&st);
			/* the ten-second hold is still running */
			CHECK_UINT(st.skew_drops, 0);
		}
		if (i == 6000U) {
			sim_stats(&st);
			drops_at_60s = st.skew_drops;
		}
	}
	sim_stats(&st);
	/* the span floor and the standing reserve each take one frame off the
	 * prefill surplus */
	CHECK_UINT(st.skew_drops, 2);
	CHECK_UINT(st.skew_drops, drops_at_60s);
	CHECK(gap_min >= 99U && gap_max <= 101U);
	CHECK_UINT(st.underruns, 0);
	CHECK_UINT(st.plc, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(st.skew_inserts, 0);
	CHECK(st.depth_ms <= st.target_ms + 20U);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* The reach gate is the floor of every drop run. The runs of the offset trim
 * stop at the setpoint before the reach can touch the gate, because the
 * averaged excess ends them first. The run that exercises the gate is
 * therefore the free-window trim after a refill. At ptime 10, the depth has no
 * sawtooth, and the equality boundary of the gate is exact. With the gate
 * removed, the run drains below the target and underruns again. With the
 * boundary at "below" instead of "at or below", one drop falls exactly on the
 * target. */
static void test_offset_trim_floor(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint64_t release = 0;
	unsigned violations = 0;
	unsigned i;

	CASE("jb_offset_trim_floor");
	sim_reset(UDSP_JB_FIXED, 50, 100, 10, 80);
	for (i = 0; i < 3000U; i++) {
		uint16_t depth_before, target_before;

		if (i >= 1000U && i < 1030U) {
			if (i == 1000U)
				release = sim.now_ns + 300U * MS_NS;
			sim_send(release);
		} else {
			sim_send(sim.now_ns);
		}
		sim_deliver(sim.now_ns);
		sim_stats(&st);
		depth_before = st.depth_ms;
		target_before = st.target_ms;
		sim_tick(&out);
		if (out.skew_drop && depth_before <= target_before)
			violations++;
	}
	sim_stats(&st);
	CHECK_UINT(violations, 0);
	CHECK(st.skew_drops >= 20);
	CHECK_UINT(st.underruns, 1);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* On a jittery link, the adaptive target is rightly high: here about 36 ms
 * against 0..30 ms of arrival jitter. The trim sheds only the surplus of the
 * prefill above that adapted target. The underrun and plc counts stay at the
 * untrimmed baseline (both 0 over 600 s). Without the trim, the depth rests at
 * 70 ms for the whole run. */
static void test_offset_trim_jittery(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_offset_trim_jittery");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 60000U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns + (uint64_t)(rng_next() % 31U) * MS_NS);
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.underruns, 0);
	CHECK_UINT(st.plc, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK(st.target_ms >= 30 && st.target_ms <= 45);
	CHECK(st.skew_drops >= 2 && st.skew_drops <= 5);
	CHECK_UINT(st.skew_inserts, 0);
	CHECK(st.depth_ms <= st.target_ms + 20U);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* The growth concern. Each cycle has a jittery quarter minute (the target
 * rises), a 400 ms outage (park and refill), and a clean stretch (the target
 * decays). Every refill re-parks the queue at the target of that moment. The
 * trim then sheds what the decay leaves above the setpoint. The depth at the
 * tail of each stretch therefore stays bounded. The underrun and plc counts do
 * not exceed the untrimmed baseline (26 and 448 over 660 s). */
static void test_offset_trim_refill(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned tail_excess_max = 0;
	unsigned i;

	CASE("jb_offset_trim_refill");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 66000U; i++) {
		unsigned phase = i % 6000U;

		if (!(i % 2)) {
			if (phase < 1500U)
				sim_send(sim.now_ns +
					 (uint64_t)(rng_next() % 31U) * MS_NS);
			else if (phase < 1540U)
				sim_skip(1);
			else
				sim_send(sim.now_ns);
		}
		sim_tick(&out);
		if (phase == 5990U) {
			sim_stats(&st);
			if (st.depth_ms > st.target_ms &&
			    (unsigned)(st.depth_ms - st.target_ms) > tail_excess_max)
				tail_excess_max = (unsigned)(st.depth_ms - st.target_ms);
		}
	}
	sim_stats(&st);
	/* The untrimmed baseline is 26 underruns and 448 plc. The trim sheds
	 * the surplus of every clean stretch, so each jitter onset meets the
	 * queue at its floor before the target adapts. Seven more underruns per
	 * 660 s is the measured price of the shed latency; the trim does not
	 * always improve on the untrimmed run. The deferred anchor holds the
	 * first far resume packet of every 400 ms outage cycle: eleven cycles,
	 * eleven hold_drops, one extra span of concealment per cycle, split
	 * between plc and silence by where each cycle's PLC budget stood.
	 * late_drop stays zero because a held packet counts as a hold_drop. */
	CHECK(st.underruns <= 36);
	CHECK_UINT(st.plc, 453);
	/* the reserve shortens every refill's park, so six silence frames
	 * become six frames of audio: a strict improvement */
	CHECK_UINT(st.silence, 19);
	CHECK_UINT(st.frames_out, 65528);
	CHECK(st.skew_drops >= 5);
	CHECK_UINT(st.hold_drop, 11);
	CHECK(tail_excess_max <= 20U);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(st.skew_inserts, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* A 40 ms forward step that reverts two seconds later is a transient
 * excursion, shorter than the hold time. The case runs in FIXED, where
 * jb_trim_due refuses the offset trim before it reads the excess. The jitter
 * spike of the step keeps the trend gate shut, so nothing is dropped. */
static void test_offset_trim_sustain(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_offset_trim_sustain");
	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	for (i = 0; i < 3000U; i++) {
		if (!(i % 2)) {
			if (i == 1000U)
				sim.ts += 40U * 8U;
			if (i == 1200U)
				sim.ts -= 40U * 8U;
			sim_send(sim.now_ns);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.skew_drops, 0);
	CHECK_UINT(st.skew_inserts, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(sim.account_errors, 0);
}


/* FIXED is excluded from the trim, like OFF. The fax path pins the playout
 * (stream.c: the demodulator owns the samples). Under jitter above the fixed
 * target, a trim costs a FIXED fax-class call one carrier frame per second.
 * A +40 ms forward step leaves a standing surplus that the trend gate
 * never sees. In FIXED, that surplus must stay. */
static void test_offset_trim_fixed_excluded(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_offset_trim_fixed_excluded");
	/* FIXED is excluded from the trim for the fax path. The four drops here
	 * are the trend corrector's response to the +40 ms step; the trim adds
	 * nothing, and no FIXED shape can make it observable: a surplus is
	 * either below one frame of drift (invisible to both mechanisms), a
	 * visible rise (the trend arms first), or dirty (the hold resets). The
	 * exclusion is therefore pinned as the exact trend-only count. */
	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	for (i = 0; i < 6000U; i++) {
		if (!(i % 2)) {
			if (i >= 500U && i < 520U)
				sim_skip(1);
			else if (i == 800U) {
				sim.ts += 40U * 8U;
				sim_send(sim.now_ns);
			} else {
				sim_send(sim.now_ns);
			}
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.skew_drops, 4);
	CHECK_UINT(st.skew_inserts, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK(st.depth_ms >= 40);
	CHECK_UINT(sim.account_errors, 0);
}

/* While the target is clamped at max_ms, the depth above it is the reserve
 * that the clamp refused, not surplus. The trim must not arm there. On this
 * shape, a trim that arms raises the underruns from 3 to 487 per 300 s. */
static void test_offset_trim_clamped(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_offset_trim_clamped");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 30000U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns + (uint64_t)(rng_next() % 161U) * MS_NS);
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.target_ms, 100);
	CHECK_UINT(st.skew_drops, 0);
	CHECK(st.underruns <= 12);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* Any concealment inside the hold window resets the age, so a
 * lossy line keeps its surplus by construction. One packet lost every three
 * seconds keeps the hold from ever completing. */
static void test_offset_trim_conceal_resets_hold(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned sent = 0;
	unsigned i;

	CASE("jb_offset_trim_conceal_resets_hold");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 6000U; i++) {
		if (!(i % 2)) {
			sent++;
			if (!(sent % 150U))
				sim_skip(1);
			else
				sim_send(sim.now_ns);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.skew_drops, 0);
	CHECK_UINT(st.skew_inserts, 0);
	CHECK(st.depth_ms >= 20);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* A measured limit: on a clean line, the depth reading moves in whole frames.
 * Sixty seconds of 100 ppm skew (six milliseconds of phase) therefore change
 * nothing after the initial trim. The excess parks at five, and no threshold
 * in (5, 25) is distinguishable here. The jittery cases carry the margin of
 * the threshold, because their averages sit off the frame grid.
 * test_adaptive_jitter fails a threshold of 15 ms. This case checks the
 * sub-frame invisibility. */
static void test_offset_trim_threshold_margin(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint64_t next;
	unsigned i;

	CASE("jb_offset_trim_threshold_margin");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	next = sim.now_ns;
	for (i = 0; i < 12000U; i++) {
		while (next <= sim.now_ns) {
			sim_send(next);
			next += i < 6000U ? 19998000ULL : 20000000ULL;
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	/* the span floor and the standing reserve each take one frame off the
	 * prefill surplus */
	CHECK_UINT(st.skew_drops, 2);
	CHECK_UINT(st.underruns, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* A refill is a re-anchor, so it clears a pending clock-skew correction as it
 * clears a step candidate. A far end that runs slow arms inserts, and an
 * outage then parks and refills. The resumed stream must carry no stale
 * correction, and the order check must hold across the resume. */
static void test_park_refill_insert(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint64_t next;
	uint64_t inserts_at_park = 0;
	unsigned i;

	CASE("jb_park_refill_insert");
	sim_reset(UDSP_JB_FIXED, 50, 100, 10, 80);
	next = sim.now_ns;
	for (i = 0; i < 5000U; i++) {
		while (next <= sim.now_ns) {
			if (i < 3000U || i >= 3050U)
				sim_send(next);
			next += 10050000ULL;
		}
		sim_tick(&out);
		if (i == 3000U) {
			sim_stats(&st);
			inserts_at_park = st.skew_inserts;
		}
	}
	sim_stats(&st);
	CHECK(inserts_at_park >= 5);
	CHECK(st.skew_inserts >= inserts_at_park);
	CHECK(st.skew_inserts <= inserts_at_park + 12U);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(sim.account_errors, 0);
}


/* The shape that needs the span floor. With a floor of one frame, the trimmed
 * sawtooth bottoms at empty, and a few milliseconds of arrival jitter turn the
 * phase drift into underruns: this run reads 9. At the floor of one span plus
 * one frame, it reads none. */
static void test_floor_margin(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint64_t next;
	unsigned i;

	CASE("jb_floor_margin");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	next = sim.now_ns;
	for (i = 0; i < 50000U; i++) {
		/* 50 ppm of far-end-fast phase drift under 0..4 ms of jitter:
		 * at the one-frame floor this run reads 9 underruns */
		while (next <= sim.now_ns) {
			sim_send(next + (uint64_t)(rng_next() % 5U) * MS_NS);
			next += 19999000ULL;
		}
		if (i == 150U) {
			sim_stats(&st);
			/* the floor stays at init_ms through the startup
			 * window, so the shrink cannot leave it before t=2 s */
			CHECK_UINT(st.target_ms, 50);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.underruns, 0);
	CHECK_UINT(st.plc, 0);
	/* the settled floor is one packet span plus one frame */
	CHECK_UINT(st.target_ms, 30);
	/* the span floor and the standing reserve each take one frame off the
	 * prefill surplus */
	CHECK_UINT(st.skew_drops, 4);
	CHECK(st.depth_ms >= 10 && st.depth_ms <= 50);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* At ptime 30 the floor is one 30 ms span. */
static void test_floor_ptime30(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_floor_ptime30");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 30, 80);
	for (i = 0; i < 9000U; i++) {
		if (!(i % 3))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	sim_stats(&st);
	/* the settled floor is one packet span plus one frame */
	CHECK_UINT(st.target_ms, 40);
	CHECK_UINT(st.underruns, 0);
	CHECK_UINT(st.plc, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK(st.depth_ms <= 60);
	CHECK_UINT(sim.account_errors, 0);
}

/* A mid-stream ptime relearn moves the floor with the learned span, in both
 * directions. The mis-sliced packets around each relearn are the accepted cost
 * of an unsignaled change (test_span_ptime_change). */
static void test_floor_relearn(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_floor_relearn");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 4000U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	sim_stats(&st);
	/* the settled floor is one packet span plus one frame */
	CHECK_UINT(st.target_ms, 30);
	sim.span_ms = 30U;
	for (i = 0; i < 4000U; i++) {
		if (!(i % 3))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(sim.jb.pkt_span_ts, 240);
	CHECK_UINT(st.target_ms, 40);
	sim.span_ms = 20U;
	for (i = 0; i < 4000U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(sim.jb.pkt_span_ts, 160);
	CHECK_UINT(st.target_ms, 30);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* The deferred refill anchor. A single far-ahead mis-stamped audio packet that
 * arrives during a micro-park is refused, and the clock does not move. The
 * junk costs one hold_drop, the candidate that it armed dies on the first
 * on-timeline packet, and the resume takes the ordinary near refill. No
 * counted re-anchor happens, so the strict order check stands for the whole
 * case. The case needs jb_parked, which needs an empty queue. The floor
 * carries a packet span of reserve, so three lost packets drain it and the
 * packet in flight before the outlier arrives. */
static void test_hold_far_single(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint32_t parked_ts = 0;
	unsigned i;

	CASE("jb_hold_far_single_parked");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 4000U; i++) {
		if (!(i % 2)) {
			if (i >= 2000U && i < 2006U)
				sim_skip(1);
			else if (i == 2006U)
				outlier_send(2000U * 8U);
			else
				sim_send(sim.now_ns);
		}
		if (i == 1990U) {
			sim_stats(&st);
	/* the span floor and the standing reserve each take one frame off the
	 * prefill surplus */
			CHECK_UINT(st.skew_drops, 2);
			CHECK_UINT(st.skew_inserts, 0);
		}
		if (i == 2006U)
			parked_ts = sim.jb.playout_ts;
		sim_tick(&out);
		if (i == 2006U)
			CHECK_UINT(sim.jb.playout_ts, parked_ts);
	}
	sim_stats(&st);
	CHECK_UINT(st.hold_drop, 1);
	CHECK_UINT(sim.late, 1);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.cn_reanchors, 0);
	CHECK_UINT(st.skew_drops, 2);
	CHECK_UINT(st.skew_inserts, 0);
	CHECK_UINT(st.underruns, 1);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 3900);
}

/* The distrust of the jitter estimator. The held junk packet contributes no
 * observation, and does not serve as the reference of the next observation.
 * The target therefore never spikes off the floor, and the jb jitter gauge
 * never reports timestamp garbage as arrival jitter. If the observation is
 * re-admitted, or the junk kept as the reference for the return leg, the
 * smoothed estimate jumps by about 124 ms. The high-water then clamps the
 * target to max_ms, and both snapshots below fail. The case needs jb_parked,
 * which needs an empty queue; three lost packets drain the reserve of the
 * floor and the packet in flight. */
static void test_hold_jitter_distrust(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_hold_jitter_distrust");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 4000U; i++) {
		if (!(i % 2)) {
			if (i >= 2000U && i < 2006U)
				sim_skip(1);
			else if (i == 2006U)
				outlier_send(2000U * 8U);
			else
				sim_send(sim.now_ns);
		}
		if (i == 2010U) {
			sim_stats(&st);
	/* the settled floor is one packet span plus one frame */
			CHECK_UINT(st.target_ms, 30);
			CHECK_UINT(st.jitter_ms, 0);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.target_ms, 30);
	CHECK_UINT(st.jitter_ms, 0);
	CHECK_UINT(st.hold_drop, 1);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* A real 3 s outage parks the clock: the far end kept its timeline, and every
 * packet was lost. The resume is far ahead but real. The first resume packet
 * is refused once, and arms the candidate. Its successor confirms, and the
 * refill adopts. The cost is one hold_drop, no resync, and exactly one extra
 * span of concealment against an immediate adoption. Here that span is two
 * silence units, because the PLC budget is spent. */
static void test_hold_far_resume(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned recover = 0;
	unsigned i;

	CASE("jb_hold_far_resume_3s");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 6000U; i++) {
		if (!(i % 2)) {
			if (i >= 2000U && i < 2300U)
				sim_skip(1);
			else
				sim_send(sim.now_ns);
		}
		sim_tick(&out);
		if (i > 2300U && !recover && out.decision == UDSP_JB_FRAME)
			recover = i - 2300U;
	}
	sim_stats(&st);
	CHECK_UINT(st.hold_drop, 1);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.underruns, 1);
	CHECK_UINT(st.plc, 40);
	/* An immediate adoption reads 263. The deferred adoption spends one
	 * more span of the outage concealing, past the PLC budget, and the
	 * deeper prefill adds one more silence frame. */
	CHECK_UINT(st.silence, 266);
	CHECK(recover > 0 && recover * UDSP_JB_FRAME_MS <= 100U);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 5600);
}

/* Frontier poisoning. A junk packet held during a first micro-park raises the
 * monotonic stream frontier by two seconds. A real outage resume inside that
 * poisoned window can then never re-arm through the frontier test of
 * jb_step_track. The hold arms the candidate against the parked clock instead,
 * and the resume confirms on its second packet. An arm gated on the frontier
 * holds every resume packet forever: 1851 refusals and no recovery. */
static void test_hold_poisoned_frontier(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned recover = 0;
	unsigned i;

	CASE("jb_hold_poisoned_frontier");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 6000U; i++) {
		if (!(i % 2)) {
			if (i == 2000U || i == 2002U || i == 2004U) {
				sim_skip(1);
			} else if (i == 2006U) {
				outlier_send(2000U * 8U);
			} else if (i >= 2200U && i < 2300U) {
				sim_skip(1);
			} else if (i == 2300U) {
				/* the far end moved its timeline into the
				 * window the held junk poisoned */
				sim.ts = sim.jb.stream_end_ts + 400U;
				sim_send(sim.now_ns);
			} else {
				sim_send(sim.now_ns);
			}
		}
		sim_tick(&out);
		if (i > 2300U && !recover && out.decision == UDSP_JB_FRAME)
			recover = i - 2300U;
	}
	sim_stats(&st);
	CHECK(recover > 0 && recover * UDSP_JB_FRAME_MS <= 60U);
	CHECK_UINT(st.hold_drop, 2);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	/* the span floor and the standing reserve each take one frame off the
	 * prefill surplus */
	CHECK_UINT(st.skew_drops, 2);
	CHECK_UINT(st.skew_inserts, 0);
	CHECK_UINT(st.underruns, 2);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 5600);
}

/* The hold bound. A real far resume under 75% loss arrives with sequence gaps
 * of 4, which the 1..3 confirm window can never agree with. The behind-only
 * starve resync cannot rescue a stream that runs ahead. The bound adopts the
 * third packet: two refusals, recovery inside a second, and no resync. An
 * unbounded hold is permanent silence: 475 refusals and no recovery. */
static void test_hold_heavy_loss_resume(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned recover = 0;
	unsigned sent = 0;
	unsigned i;

	CASE("jb_hold_heavy_loss_resume");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 6000U; i++) {
		if (!(i % 2)) {
			if (i >= 2000U && i < 2200U) {
				sim_skip(1);
			} else if (i >= 2200U) {
				sent++;
				if (sent % 4U)
					sim_skip(1);
				else
					sim_send(sim.now_ns);
			} else {
				sim_send(sim.now_ns);
			}
		}
		sim_tick(&out);
		if (i > 2200U && !recover && out.decision == UDSP_JB_FRAME)
			recover = i - 2200U;
	}
	sim_stats(&st);
	CHECK(recover > 0 && recover * UDSP_JB_FRAME_MS <= 1000U);
	CHECK_UINT(st.hold_drop, 2);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 2900);
}

/* A documented corner. A late comfort-noise packet stamped behind the parked
 * clock ends the park (cn_active). The held candidate survives, because
 * comfort noise neither arms, confirms nor voids. A second junk packet that
 * continues the candidate then confirms into the full PLAY-state resync, not
 * the parked refill. The case checks the bounded cost: one resync onto the
 * junk timeline, then the seq-consecutive real stream pulls the PREFILL clock
 * back and play resumes. Two agreeing junk audio packets get exactly what the
 * step proof promises them. */
static void test_hold_cn_mid_hold(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned recover = 0;
	unsigned i;

	CASE("jb_hold_cn_mid_hold");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 4000U; i++) {
		if (!(i % 2)) {
			if (i == 2000U || i == 2002U)
				sim_skip(1);
			else if (i == 2004U || i == 2006U)
				outlier_send(2000U * 8U);
			else
				sim_send(sim.now_ns);
		}
		if (i == 2005U) {
			sim_queue(sim.now_ns, sim.jb.playout_ts - 160U, sim.seq,
				  RTP_PAYLOAD_COMFORT_NOISE, 2);
			sim_skip(1);
		}
		sim_tick(&out);
		if (i > 2006U && !recover && out.decision == UDSP_JB_FRAME)
			recover = i - 2006U;
	}
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 1);
	CHECK_UINT(st.hold_drop, 1);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.cn_reanchors, 0);
	CHECK(recover > 0 && recover * UDSP_JB_FRAME_MS <= 100U);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 3900);
}

/* A documented corner: the far resume of a stream whose span was never
 * learned. The stream before the resume keeps one packet in four. The pair
 * rule learns across gaps up to 3, so span_learned stays false until the
 * resume. The first learn of a played stream only nominates, so nothing clears
 * the step candidate that the held resume packet armed. Its successor confirms
 * at once, at the cost of one hold_drop. The span then corroborates to the
 * true 80 from the resume pairs. */
static void test_hold_unlearned_span(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint64_t holds_before = 0;
	unsigned recover = 0;
	unsigned i;

	CASE("jb_hold_unlearned_span");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 10, 80);
	for (i = 0; i < 6000U; i++) {
		if (i < 2000U) {
			if (i % 4)
				sim_skip(1);
			else
				sim_send(sim.now_ns);
		} else if (i < 2300U) {
			sim_skip(1);
		} else {
			sim_send(sim.now_ns);
		}
		if (i == 2299U) {
			sim_stats(&st);
			holds_before = st.hold_drop;
			CHECK(!sim.jb.span_learned);
		}
		sim_tick(&out);
		if (i > 2300U && !recover && out.decision == UDSP_JB_FRAME)
			recover = i - 2300U;
	}
	sim_stats(&st);
	CHECK_UINT(holds_before, 0);
	CHECK_UINT(st.hold_drop, 1);
	CHECK(sim.jb.span_learned);
	CHECK_UINT(sim.jb.pkt_span_ts, 80);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK(recover > 0 && recover * UDSP_JB_FRAME_MS <= 100U);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* A documented corner: the first two packets of a real far resume arrive
 * swapped. The first (newer) is held and arms. The second (older) fails the
 * confirm window backwards, is held too, and re-arms the candidate on itself.
 * The third packet then confirms against the re-armed candidate. The cost is
 * two refusals, and a bounded adoption on the third arriving packet. */
static void test_hold_swapped_resume(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint32_t held_ts = 0;
	uint16_t held_seq = 0;
	unsigned recover = 0;
	unsigned i;

	CASE("jb_hold_swapped_resume");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 6000U; i++) {
		if (!(i % 2)) {
			if (i >= 2000U && i < 2300U) {
				sim_skip(1);
			} else if (i == 2300U) {
				held_ts = sim.ts;
				held_seq = sim.seq;
				sim_skip(1);
				sim_send(sim.now_ns);
				sim_queue(sim.now_ns, held_ts, held_seq,
					  RTP_PAYLOAD_AUDIO, 2U * sim.bpf);
			} else {
				sim_send(sim.now_ns);
			}
		}
		sim_tick(&out);
		if (i > 2300U && !recover && out.decision == UDSP_JB_FRAME)
			recover = i - 2300U;
	}
	sim_stats(&st);
	CHECK_UINT(st.hold_drop, 2);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.dup_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK(recover > 0 && recover * UDSP_JB_FRAME_MS <= 100U);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 5600);
}

/* FIXED is excluded from the hold, like the offset trim. The fax path never
 * discards a carrier frame, and a FIXED target can neither spike nor decay.
 * The churn that the hold removes therefore cannot arise. A far resume during
 * a FIXED park takes the immediate refill: adoption on the first resume
 * packet, and zero hold_drop. */
static void test_hold_fixed_far_resume(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned recover = 0;
	unsigned i;

	CASE("jb_hold_fixed_far_resume");
	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	for (i = 0; i < 6000U; i++) {
		if (!(i % 2)) {
			if (i >= 2000U && i < 2300U)
				sim_skip(1);
			else
				sim_send(sim.now_ns);
		}
		sim_tick(&out);
		if (i > 2300U && !recover && out.decision == UDSP_JB_FRAME)
			recover = i - 2300U;
	}
	sim_stats(&st);
	CHECK_UINT(st.hold_drop, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.plc, 40);
	CHECK_UINT(st.underruns, 1);
	CHECK(recover > 0 && recover * UDSP_JB_FRAME_MS <= 100U);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 5600);
}

/* The comfort-noise distrust must not depend on a frontier that held junk
 * poisoned. The held junk audio raises stream_end_ts by two seconds, so a junk
 * CN at that edge passes the frontier half of jb_ts_far. A parked clock
 * therefore also refuses a CN beyond the playout-only far bound, so the resume
 * re-seeds from a dead reference and observes nothing. Without that rule, the
 * CN re-seeds the estimator reference: the resume measures a two-second step,
 * jitter 125 ms, target 100 and six extra trim drops. jb_cn_store still
 * accepts the packet, so the comfort-noise resume path stands. The case needs
 * jb_parked, which needs an empty queue; three lost packets drain the reserve
 * of the floor and the packet in flight. */
static void test_hold_cn_poisoned_distrust(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_hold_cn_poisoned_distrust");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 4000U; i++) {
		if (!(i % 2)) {
			if (i >= 2000U && i < 2006U)
				sim_skip(1);
			else if (i == 2006U)
				outlier_send(2000U * 8U);
			else
				sim_send(sim.now_ns);
		}
		if (i == 2007U) {
			sim_queue(sim.now_ns, sim.jb.stream_end_ts, sim.seq,
				  RTP_PAYLOAD_COMFORT_NOISE, 2);
			sim_skip(1);
		}
		if (i == 1990U) {
			sim_stats(&st);
	/* the span floor and the standing reserve each take one frame off the
	 * prefill surplus */
			CHECK_UINT(st.skew_drops, 2);
			CHECK_UINT(st.skew_inserts, 0);
		}
		if (i == 2010U) {
			sim_stats(&st);
	/* the settled floor is one packet span plus one frame */
			CHECK_UINT(st.target_ms, 30);
			CHECK_UINT(st.jitter_ms, 0);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.skew_drops, 2);
	CHECK_UINT(st.skew_inserts, 0);
	CHECK_UINT(st.hold_drop, 1);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	/* as in jb_junk_cn_arms_no_candidate, jb_cn_resume needs an empty queue and
	 * the reserve denies it one; hold_drop 1 above is the proof */
	CHECK_UINT(st.cn_reanchors, 0);
	CHECK_UINT(st.target_ms, 30);
	CHECK_UINT(st.jitter_ms, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 3900);
}

/* hold_run must not leak across a park exit that stores nothing: a
 * behind-stamped CN ends the park, and an ahead-stamped CN re-forms it.
 * Without a reset on that path, the refusal runs of three bridged parks sum,
 * and the third park adopts its junk without a confirm. A trusted comfort-noise
 * packet, one that the estimator did not refuse, therefore resets the run like
 * a stored packet. A refused junk CN does not, so alternating junk cannot
 * suppress the bound. Every bridged park below refuses its junk afresh, and the
 * clock never moves onto junk. The real far resume still adopts through one
 * refusal and a confirming successor. */
static void test_hold_cn_bridged_parks(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint32_t parked_ts = 0;
	unsigned i;

	CASE("jb_hold_cn_bridged_parks");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 6000U; i++) {
		if (i < 2000U && !(i % 2))
			sim_send(sim.now_ns);
		/* The junk and bridge packets carry sequence numbers at least
		 * 64 behind the stream, outside the replay window: they must
		 * neither read as duplicates nor advance seq_high, and the
		 * resumed stream must never reuse their numbers. */
		if (i == 2010U || i == 2030U || i == 2050U) {
			parked_ts = sim.jb.playout_ts;
			sim_queue(sim.now_ns, sim.jb.playout_ts + 16000U,
				  (uint16_t)(sim.seq - 100U + (i - 2010U) / 2U),
				  RTP_PAYLOAD_AUDIO, 2U * sim.bpf);
		}
		if (i == 2016U || i == 2036U)
			sim_queue(sim.now_ns, sim.jb.playout_ts - 160U,
				  (uint16_t)(sim.seq - 96U + (i - 2016U) / 2U),
				  RTP_PAYLOAD_COMFORT_NOISE, 2);
		if (i == 2022U || i == 2042U)
			sim_queue(sim.now_ns, sim.jb.playout_ts + 800U,
				  (uint16_t)(sim.seq - 95U + (i - 2022U) / 2U),
				  RTP_PAYLOAD_COMFORT_NOISE, 2);
		if (i == 2060U)
			sim_skip(30);
		if (i >= 2060U && !(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
		if (i == 2010U || i == 2030U || i == 2050U)
			CHECK_UINT(sim.jb.playout_ts, parked_ts);
	}
	sim_stats(&st);
	CHECK_UINT(st.hold_drop, 4);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.cn_reanchors, 0);
	CHECK_UINT(st.dup_drop, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 5800);
}

/* The refused half of the trusted-CN rule. Junk comfort noise interleaved with
 * a heavy-loss far resume must not reset the refusal run. Otherwise the hold
 * bound never fires, and the resume is held forever. The parked playout-only
 * bound refuses the junk CNs here; the held resume already poisoned the
 * frontier half. The bound still adopts the third audio packet, exactly as in
 * jb_hold_heavy_loss_resume. An unconditional CN reset livelocks this
 * shape. */
static void test_hold_junk_cn_suppression(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned recover = 0;
	unsigned sent = 0;
	unsigned i;

	CASE("jb_hold_junk_cn_suppression");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 6000U; i++) {
		if (!(i % 2)) {
			if (i >= 2000U && i < 2200U) {
				sim_skip(1);
			} else if (i >= 2200U) {
				sent++;
				if (sent % 4U)
					sim_skip(1);
				else
					sim_send(sim.now_ns);
			} else {
				sim_send(sim.now_ns);
			}
		}
		/* one refused junk CN after each held resume packet; the
		 * sequence numbers sit outside the replay window */
		if (i == 2207U || i == 2215U)
			sim_queue(sim.now_ns, sim.jb.playout_ts + 16000U,
				  (uint16_t)(sim.seq - 100U + (i - 2207U) / 8U),
				  RTP_PAYLOAD_COMFORT_NOISE, 2);
		sim_tick(&out);
		if (i > 2200U && !recover && out.decision == UDSP_JB_FRAME)
			recover = i - 2200U;
	}
	sim_stats(&st);
	CHECK(recover > 0 && recover * UDSP_JB_FRAME_MS <= 1000U);
	CHECK_UINT(st.hold_drop, 2);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.dup_drop, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 2900);
}

/* In FIXED, the mode excludes the hold: a far junk during a park is adopted at
 * once, and its observation spikes the estimator once. That is the accepted
 * cost of the FIXED exclusion, and a FIXED target cannot move with the spike.
 * The reference invalidation of the refill stops a doubled spike here.
 * hold_want is false by mode, so only the clear of the refill keeps the real
 * return leg from measuring the junk as its reference. Measured at the
 * snapshot: jitter 125 ms with the invalidation, 241 without. The successor
 * pull-back recovers the timeline in the same put. */
static void test_hold_fixed_junk_spike(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_hold_fixed_junk_spike");
	sim_reset(UDSP_JB_FIXED, 60, 100, 20, 80);
	for (i = 0; i < 4000U; i++) {
		if (!(i % 2)) {
			if (i >= 2000U && i < 2060U)
				sim_skip(1);
			else if (i == 2060U)
				outlier_send(2000U * 8U);
			else
				sim_send(sim.now_ns);
		}
		if (i == 2064U) {
			sim_stats(&st);
			CHECK_UINT(st.jitter_ms, 125);
			CHECK_UINT(st.target_ms, 60);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.hold_drop, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.target_ms, 60);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 3800);
}

/* The measured edge. The far bound is max_ms plus one learned span, strictly
 * greater. At ptime 20 and max 100, the edge sits at 960 ticks. A parked
 * packet exactly 960 ahead takes the immediate refill, and one at 961 is held
 * once and adopted by its confirming successor. The near side separates the
 * bound from bare max_ms: a bound without the span would hold the 960
 * packet. */
static void test_hold_far_boundary(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_hold_far_boundary");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 6000U; i++) {
		if (!(i % 2)) {
			if (i == 2010U) {
				sim.ts = sim.jb.playout_ts + 960U;
				sim_send(sim.now_ns);
			} else if (i == 3010U) {
				sim.ts = sim.jb.playout_ts + 961U;
				sim_send(sim.now_ns);
			} else if ((i >= 2000U && i < 2010U) ||
				   (i >= 3000U && i < 3010U)) {
				/* five lost slots park the clock */
			} else {
				sim_send(sim.now_ns);
			}
		}
		sim_tick(&out);
		if (i == 2010U) {
			sim_stats(&st);
			CHECK_UINT(st.hold_drop, 0);
		}
	}
	sim_stats(&st);
	CHECK_UINT(st.hold_drop, 1);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 5700);
}

/* The step candidate must die with the timeline. An SSRC change re-anchors the
 * whole stream through jb_timing_reset. If a candidate armed against the old
 * timeline survived the reset, the first PLAY-state packet of the new stream
 * in its window would confirm it. The case crafts that: the new stream starts
 * one span past the timestamp of the old outlier. The buffer would then
 * resync a second time onto garbage. The case allows exactly one resync. */
static void test_resync_clears_candidate(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_resync_clears_candidate");
	sim_reset(UDSP_JB_ADAPTIVE, 20, 100, 20, 80);
	for (i = 0; i < 4000U; i++) {
		if (!(i % 2)) {
			if (i == 1000U) {
				outlier_send(2000U * 8U);
			} else if (i == 1002U) {
				sim.ssrc = 0x55667788U;
				sim.ts = sim.jb.step_ts;
				sim_send(sim.now_ns);
			} else {
				sim_send(sim.now_ns);
			}
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 1);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.hold_drop, 0);
	CHECK_UINT(st.dup_drop, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 3900);
}

/* The netem reproduction shape: uniform 0..300 ms arrival jitter on a 20 ms
 * ptime stream with max_ms 100. The reorder tail puts packets beyond window +
 * span constantly, and correlated pairs of them satisfy the two-packet step
 * proof: without the gate this shape reads 11 resyncs and 8932 frames per 120 s
 * (seed 1). The jitter gate at K = 4 must hold the resync rate at zero on every
 * seed, discard nothing, and deliver at least the ungated frame count plus 5%;
 * the post-resync cascade is the frame loss. The depth ceilings pin the
 * quantity the gate spends, which is standing depth while the line stays
 * filthy. The mean tail depth is the named cost metric and the peak is its
 * excursion bound, both with margin over the measured values; the tail depth
 * bounds the reach from above, so the pins cover both readings. The K sweep is
 * churn-blind above 4: zero resyncs means the gate never fires, so K = 4 and
 * K = 8 run identically here, and the boundary pair is what fails a higher K.
 * The order oracle stays unpinned here: on this beyond-spec line it counts one
 * benign artifact without the gate too. */
static void gate_churn_run(uint32_t seed, unsigned ticks, unsigned *dep_max,
			   unsigned *dep_mean)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint64_t dep_sum = 0;
	unsigned i;

	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	rng_state = seed;
	*dep_max = 0;
	for (i = 0; i < ticks; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns + (uint64_t)(rng_next() % 301U) * MS_NS);
		sim_tick(&out);
		sim_stats(&st);
		dep_sum += st.depth_ms;
		if (st.depth_ms > *dep_max)
			*dep_max = st.depth_ms;
	}
	*dep_mean = (unsigned)(dep_sum / ticks);
}

static void test_gate_churn(void)
{
	/* frame floors are the per-seed ungated baselines plus 5 percent
	 * (8932, 9036, 8867, 9029 per 120 s and 46360 per 600 s); the depth
	 * pins carry margin over the measured gated values (mean 176, 176,
	 * 155, 160, 187; peak 320, 270, 350, 300, 320) */
	static const struct {
		const char *name;
		uint32_t seed;
		unsigned ticks;
		unsigned frames_floor;
		unsigned dep_ceiling;
		unsigned mean_ceiling;
	} run[5] = {
		{ "jb_gate_churn_seed1", 0x2545f491U, 12000U, 9379U, 350U, 200U },
		{ "jb_gate_churn_seed2", 0x9e3779b9U, 12000U, 9488U, 300U, 200U },
		{ "jb_gate_churn_seed3", 0x6c078965U, 12000U, 9311U, 380U, 180U },
		{ "jb_gate_churn_seed4", 0x41c64e6dU, 12000U, 9481U, 330U, 185U },
		{ "jb_gate_churn_600s", 0x2545f491U, 60000U, 48678U, 350U, 210U },
	};
	struct udsp_jb_stats st;
	unsigned dep_max, dep_mean;
	unsigned k;

	for (k = 0; k < 5U; k++) {
		CASE(run[k].name);
		gate_churn_run(run[k].seed, run[k].ticks, &dep_max, &dep_mean);
		sim_stats(&st);
		CHECK_UINT(st.resyncs, 0);
		CHECK_UINT(st.starve_resyncs, 0);
		CHECK_UINT(st.resync_discard_ms, 0);
		CHECK_UINT(st.overflow_drop, 0);
		CHECK(st.frames_out >= run[k].frames_floor);
		CHECK(dep_max <= run[k].dep_ceiling);
		CHECK(dep_mean <= run[k].mean_ceiling);
		CHECK_UINT(sim.payload_errors, 0);
		CHECK_UINT(sim.account_errors, 0);
	}
}

/* The sweep over real steps. A 2 s forward step on the same heavily impaired
 * line must keep the recovery without the gate at every step tick. The
 * recovery ticks checked here are the values measured without the gate: 13,
 * 84, 87, 137, 73, 70. They depend on the tick through the arrival pattern
 * around each injection. The assertions around the injection are deltas per
 * event, not run totals. Exactly one resync happens, and only the tick 6000
 * injection catches reachable audio (80 ms) in the queue at the confirm. */
static void test_gate_step_parity(void)
{
	static const struct {
		unsigned step_tick;
		unsigned recover_want;
		unsigned discard_want;
	} run[6] = {
		{ 6000U, 13U, 80U },
		{ 2400U, 84U, 0U },
		{ 6244U, 87U, 0U },
		{ 7332U, 137U, 0U },
		{ 9002U, 73U, 0U },
		{ 10500U, 70U, 0U },
	};
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint64_t rs_before, disc_before;
	uint32_t step_dst;
	unsigned recover;
	unsigned i, k;

	CASE("jb_gate_step_parity");
	for (k = 0; k < 6U; k++) {
		sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
		rs_before = 0;
		disc_before = 0;
		step_dst = 0;
		recover = 0;
		for (i = 0; i < 12000U; i++) {
			if (!(i % 2)) {
				if (i == run[k].step_tick) {
					sim_stats(&st);
					rs_before = st.resyncs;
					disc_before = st.resync_discard_ms;
					sim.ts += 2000U * 8U;
					step_dst = sim.ts;
				}
				sim_send(sim.now_ns +
					 (uint64_t)(rng_next() % 301U) * MS_NS);
			}
			sim_tick(&out);
			if (i > run[k].step_tick && !recover &&
			    out.decision == UDSP_JB_FRAME &&
			    (int32_t)(out.rtp_timestamp - step_dst) >= 0)
				recover = i - run[k].step_tick;
		}
		sim_stats(&st);
		CHECK_UINT(recover, run[k].recover_want);
		CHECK_UINT(st.resyncs - rs_before, 1);
		CHECK_UINT(st.resync_discard_ms - disc_before,
			   run[k].discard_want);
		CHECK_UINT(st.overflow_drop, 0);
		CHECK_UINT(sim.payload_errors, 0);
		CHECK_UINT(sim.account_errors, 0);
	}
}

/* The near-gate boundary pair, in ADAPTIVE only. A deterministic alternating
 * arrival delay (every second packet 60 ms late) holds the internal estimate at
 * exactly jitter_q4 7672 (479 ticks) after 3000 warmup ticks. The sustained
 * jitter clamps the adaptive target at max_ms, which pins the standing depth
 * and so stabilizes the confirm distance. The confirm attempt after a step of
 * 1840 ticks falls at distance 2320 against gate 2320; the estimate reads 580
 * ticks there, inflated by the step packets' own observations. Equality is
 * refused, the candidate clears, and the step is absorbed at the pinned cost
 * below. That is the ADAPTIVE band: window + span < distance <= K x jitter. The
 * clock walks the offset off at the trend rate, recovery 29 ticks against 10
 * for the confirmed twin, 2.9x. One tick more, 1841, falls at 2321 and
 * confirms: one resync, ungated recovery, zero absorption cost. Without the
 * gate both sub-cases resync, because the static bound reads 960, so the below
 * sub-case is the gate's failing-first proof. The pair pins K from both sides:
 * K = 3 reads gate 1740 and confirms the below case; K = 8 reads 4640 and
 * refuses the above case. It also pins the shift-before-multiply form:
 * jitter_q4 at the above confirm is 9291 (mod 16 = 11), so a multiply-first
 * gate computes floor(9291 / 4) = 2322 and would refuse distance 2321. */
static void gate_boundary_run(unsigned step_ticks, unsigned *recover,
			      struct udsp_jb_stats *pre, unsigned *dep_max,
			      unsigned *dep_end)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint32_t step_dst = 0;
	unsigned pkt = 0;
	unsigned i;

	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	*recover = 0;
	*dep_max = 0;
	for (i = 0; i < 6000U; i++) {
		if (!(i % 2)) {
			if (i == 3000U) {
				sim_stats(pre);
				CHECK_UINT(sim.jb.jitter_q4, 7672);
				CHECK_UINT(pre->depth_ms, 60);
				CHECK_UINT(pre->target_ms, 100);
				sim.ts += step_ticks;
				step_dst = sim.ts;
			}
			pkt++;
			sim_send(sim.now_ns +
				 (uint64_t)((pkt % 2U) * 60U) * MS_NS);
		}
		sim_tick(&out);
		sim_stats(&st);
		if (i > 3000U && st.depth_ms > *dep_max)
			*dep_max = st.depth_ms;
		if (i > 3000U && !*recover && out.decision == UDSP_JB_FRAME &&
		    (int32_t)(out.rtp_timestamp - step_dst) >= 0)
			*recover = i - 3000U;
	}
	*dep_end = st.depth_ms;
}

static void test_gate_boundary(void)
{
	struct udsp_jb_stats st, pre;
	unsigned recover, dep_max, dep_end;

	CASE("jb_gate_boundary_below");
	gate_boundary_run(1840U, &recover, &pre, &dep_max, &dep_end);
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.resync_discard_ms, 0);
	CHECK_UINT(recover, 29);
	CHECK_UINT(st.plc - pre.plc, 23);
	CHECK_UINT(st.skew_drops - pre.skew_drops, 20);
	CHECK_UINT(st.late_drop - pre.late_drop, 0);
	CHECK_UINT(st.underruns - pre.underruns, 1);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(dep_max, 300);
	CHECK_UINT(dep_end, 90);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);

	CASE("jb_gate_boundary_above");
	gate_boundary_run(1841U, &recover, &pre, &dep_max, &dep_end);
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 1);
	CHECK_UINT(recover, 10);
	CHECK_UINT(st.resync_discard_ms, 0);
	CHECK_UINT(st.plc - pre.plc, 0);
	CHECK_UINT(st.skew_drops - pre.skew_drops, 0);
	CHECK_UINT(st.late_drop - pre.late_drop, 0);
	CHECK_UINT(st.underruns - pre.underruns, 0);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(dep_end, 60);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* An accepted limit: the gate follows the decay window of the estimator, whose
 * 1/16 EWMA carries an echo for 16..32 packets. One packet delayed 600 ms on an
 * otherwise clean line spikes jitter_q4 to 9300 inside one tick, because both
 * legs of the reorder arrive together. A real 300 ms step one send later is, in
 * band, indistinguishable from a second echo of the stall. Its confirm
 * (distance ~2480) sits under the still-armed gate. The step absorbs at a cost:
 * 32 ticks of recovery against 6 without the gate, plc +30, trend drops +31,
 * no late drops. The step size bounds the cost, and the trend rate sheds it;
 * the resync path would pay the queue discard. */
static void test_gate_stall_echo(void)
{
	struct udsp_jb_stats st, pre;
	struct udsp_jb_out out;
	uint32_t step_dst = 0;
	unsigned recover = 0;
	unsigned i;

	CASE("jb_gate_step_in_stall_echo");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 6000U; i++) {
		if (!(i % 2)) {
			if (i == 2062U) {
				sim_stats(&pre);
				CHECK_UINT(sim.jb.jitter_q4, 9300);
				CHECK_UINT(pre.target_ms, 100);
				sim.ts += 300U * 8U;
				step_dst = sim.ts;
			}
			if (i == 2000U)
				sim_send(sim.now_ns + 600U * MS_NS);
			else
				sim_send(sim.now_ns);
		}
		sim_tick(&out);
		if (i > 2062U && !recover && out.decision == UDSP_JB_FRAME &&
		    (int32_t)(out.rtp_timestamp - step_dst) >= 0)
			recover = i - 2062U;
	}
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.resync_discard_ms, 0);
	/* the deeper setpoint takes two more ticks to recover and one more drop
	 * to shed */
	CHECK_UINT(recover, 32);
	CHECK_UINT(st.plc - pre.plc, 30);
	CHECK_UINT(st.skew_drops - pre.skew_drops, 31);
	CHECK_UINT(st.late_drop - pre.late_drop, 0);
	CHECK_UINT(st.underruns - pre.underruns, 1);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* The alternating warmup arms the gate at jitter_q4 7672; held packets are
 * refused and add no observation, so the value stays through the outage. A
 * 1.4 s loss run parks the clock. The far end resumes 175 ms ahead: inside
 * jb_refill_far (past window + span), but under the gate (1400..1720 against
 * 1916). Each resume packet is held and re-arms, and the confirm of its
 * successor is blocked by the gate and clears. The third packet adopts
 * through the JB_HOLD_MAX bound: two hold_drops, no resync, and prefill and
 * play inside 120 ms. Without the gate, the second packet confirms past the
 * static bound and adopts after one hold. */
static void test_gate_parked_block(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint32_t resume_dst = 0;
	unsigned recover = 0;
	unsigned pkt = 0;
	unsigned i;

	CASE("jb_gate_parked_blocked_confirm");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 6000U; i++) {
		if (!(i % 2)) {
			pkt++;
			if (i < 3000U) {
				sim_send(sim.now_ns +
					 (uint64_t)((pkt % 2U) * 60U) * MS_NS);
			} else if (i < 3140U) {
				sim_skip(1);
			} else {
				if (i == 3140U) {
					CHECK_UINT(sim.jb.jitter_q4, 7672);
					sim.ts = sim.jb.playout_ts + 1400U;
					resume_dst = sim.ts;
				}
				sim_send(sim.now_ns);
			}
		}
		sim_tick(&out);
		if (i > 3140U && !recover && out.decision == UDSP_JB_FRAME &&
		    (int32_t)(out.rtp_timestamp - resume_dst) >= 0)
			recover = i - 3140U;
	}
	sim_stats(&st);
	CHECK_UINT(st.hold_drop, 2);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.resync_discard_ms, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(recover, 12);
	CHECK_UINT(st.plc, 40);
	CHECK_UINT(st.silence, 112);
	CHECK_UINT(st.underruns, 1);
	CHECK_UINT(st.frames_out, 5848);
	/* the bound adoption refills to the clamped target and the trim returns
	 * the queue to the span floor by run end: one span of standing reserve */
	CHECK_UINT(st.depth_ms, 20);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* FIXED is excluded from the gate. On the boundary_below shape, a real step
 * blocked by the gate in FIXED sheds 163 carrier packets in 30 s. That is
 * eleven times the loss rate that excludes FIXED from the offset trim; the fax
 * path outranks the churn benefit. This jittery FIXED shape must therefore
 * read exactly the counters it reads without the gate, resync churn included.
 * resync_discard_ms runs in every mode, so the case checks it too. The case
 * does not check order_errors: on this line beyond the
 * specification, the order check counts 3 benign artifacts without the gate
 * too. */
static void test_gate_fixed_churn(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned dep_max = 0;
	unsigned i;

	CASE("jb_gate_fixed_unchanged_churn");
	sim_reset(UDSP_JB_FIXED, 50, 100, 20, 80);
	for (i = 0; i < 12000U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns + (uint64_t)(rng_next() % 301U) * MS_NS);
		sim_tick(&out);
		sim_stats(&st);
		if (st.depth_ms > dep_max)
			dep_max = st.depth_ms;
	}
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 11);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.frames_out, 8026);
	CHECK_UINT(st.skew_drops, 95);
	CHECK_UINT(st.late_drop, 1899);
	CHECK_UINT(st.plc, 3913);
	CHECK_UINT(st.underruns, 1292);
	CHECK_UINT(st.hold_drop, 0);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(st.resync_discard_ms, 350);
	CHECK_UINT(dep_max, 320);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* --- the 48 kHz stream --------------------------------------------------- */

/*
 * Opus through the buffer: a 48 kHz clock, 20 ms packets of 80 bytes and one
 * indivisible unit per packet. The unit is handed out once and marked for its
 * second slice, so the caller decodes once per packet and plays two 10 ms
 * slices. Nothing slices the payload by bytes.
 */
static void test_wb_whole_packet_units(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned slices_two = 0;
	unsigned markers = 0;
	unsigned i;

	CASE("jb48_whole_packet_units");
	sim_reset_cfg(UDSP_JB_ADAPTIVE, 50, 100, 20, 40, 48U, true);
	for (i = 0; i < 2000U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
		if (out.decision != UDSP_JB_FRAME)
			continue;
		if (out.slices == 2U)
			slices_two++;
		if (!out.payload)
			markers++;
	}
	sim_stats(&st);
	CHECK_UINT(sim.jb.pkt_span_ts, 960);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.unit_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(st.underruns, 0);
	CHECK_UINT(st.plc, 0);
	/* the settled floor is one packet span plus one frame */
	CHECK_UINT(st.target_ms, 30);
	/* Every unit is two slices wide, and one payload plus one marker is
	 * what each of them costs: the two partition the frame outputs. Four
	 * units play a single slice, the three the offset trim stepped over
	 * (the same three the 8 kHz steady case sheds) and the one the run
	 * ends inside. */
	CHECK_UINT(slices_two, st.frames_out);
	CHECK_UINT(sim.unit_payloads + markers, st.frames_out);
	CHECK_UINT(st.frames_out, 1996);
	/* the deeper prefill leaves one unit fewer inside the run */
	CHECK_UINT(sim.unit_payloads, 999);
	CHECK_UINT(markers, 997);
	/* the span floor and the standing reserve each take one frame off the
	 * prefill surplus */
	CHECK_UINT(st.skew_drops, 2);
	CHECK_UINT(st.skew_inserts, 0);
	CHECK_UINT(st.silence, 4);
	CHECK_UINT(sim.unit_repeats, 0);
}

/*
 * The span learner at 48 kHz: 480 per 10 ms, 960 for the
 * 20 ms Opus packet, 1440 at the UDSP_JB_PTIME_MAX_MS bound, and nothing
 * else. The rules are the 8 kHz ones read at the stream's clock, so a delta
 * the 10 ms grid does not divide and a delta past the bound both teach
 * nothing.
 */
static void wb_pair_deliver(uint32_t ts, uint32_t delta)
{
	sim_queue(sim.now_ns, ts, sim.seq++, RTP_PAYLOAD_AUDIO, sim.bpf);
	sim_queue(sim.now_ns, ts + delta, sim.seq++, RTP_PAYLOAD_AUDIO, sim.bpf);
	sim_deliver(sim.now_ns);
}

static void test_wb_span_bounds(void)
{
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb48_span_learn_bounds");
	sim_reset_cfg(UDSP_JB_FIXED, 60, 100, 20, 40, 48U, true);
	CHECK_UINT(sim.jb.pkt_span_ts, 480);
	wb_pair_deliver(sim.ts, 960U);
	CHECK_UINT(sim.jb.pkt_span_ts, 960);
	CHECK(sim.jb.span_learned);

	/* 30 ms is the ceiling and is learned; 40 ms is past it. */
	sim_reset_cfg(UDSP_JB_FIXED, 60, 100, 30, 40, 48U, true);
	wb_pair_deliver(sim.ts, 1440U);
	CHECK_UINT(sim.jb.pkt_span_ts, 1440);
	sim_reset_cfg(UDSP_JB_FIXED, 60, 100, 20, 40, 48U, true);
	wb_pair_deliver(sim.ts, 1920U);
	CHECK_UINT(sim.jb.pkt_span_ts, 480);
	CHECK(!sim.jb.span_learned);

	/* Off the 10 ms grid: 800 ticks is 16.6 ms and no packet span. */
	sim_reset_cfg(UDSP_JB_FIXED, 60, 100, 20, 40, 48U, true);
	wb_pair_deliver(sim.ts, 800U);
	CHECK_UINT(sim.jb.pkt_span_ts, 480);
	CHECK(!sim.jb.span_learned);

	/* And the corroboration rule still binds a change at the new clock: one
	 * pair moves nothing once a span is established. */
	sim_reset_cfg(UDSP_JB_ADAPTIVE, 50, 100, 20, 40, 48U, true);
	for (i = 0; i < 200U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	CHECK_UINT(sim.jb.pkt_span_ts, 960);
	sim.span_ms = 30U;
	for (i = 0; i < 3U; i++) {
		sim_send(sim.now_ns);
		sim_tick(&out);
	}
	CHECK_UINT(sim.jb.pkt_span_ts, 1440);
}

/*
 * Rule 6 at the stream clock: the jump that discards the queue is ten
 * seconds, which is 480000 ticks at 48 kHz. The bound is exclusive, so
 * exactly ten seconds is still jitter and one tick more is a timeline step.
 */
static void wb_step_probe(uint32_t jump, struct udsp_jb_stats *st)
{
	struct udsp_jb_out out;
	unsigned i;

	sim_reset_cfg(UDSP_JB_FIXED, 60, 100, 20, 40, 48U, true);
	for (i = 0; i < 200U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	/* The rule reads the delta against the last received timestamp, so the
	 * probe is anchored there and not on the sender's next packet. */
	sim.ts = sim.jb.last_rx_ts + jump;
	sim_send(sim.now_ns);
	sim_deliver(sim.now_ns);
	sim_stats(st);
}

static void test_wb_resync_bound(void)
{
	struct udsp_jb_stats st;

	CASE("jb48_resync_bound_at_clock");
	wb_step_probe(480000U, &st);
	CHECK_UINT(st.resyncs, 0);
	wb_step_probe(480001U, &st);
	CHECK_UINT(st.resyncs, 1);
	/* Nine seconds is inside the bound at 48 kHz although it is far past
	 * the 8 kHz one, which is the whole point of the rescale. */
	wb_step_probe(432000U, &st);
	CHECK_UINT(st.resyncs, 0);
}

/*
 * The payload bound is one whole packet of the widest codec. A
 * 512-byte unit is stored and emitted whole; one byte more is refused and
 * counted, never truncated into the slot.
 */
static void test_wb_payload_bound(void)
{
	struct udsp_jb_packet in;
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint8_t payload[UDSP_JB_PAYLOAD_MAX + 1U];
	unsigned frames = 0;
	unsigned i;

	CASE("jb48_payload_512_boundary");
	CHECK_UINT(UDSP_JB_PAYLOAD_MAX, 512);
	sim_reset_cfg(UDSP_JB_FIXED, 60, 100, 20, 256U, 48U, true);
	memset(payload, 3, sizeof(payload));
	memset(&in, 0, sizeof(in));
	in.payload = payload;
	in.len = UDSP_JB_PAYLOAD_MAX + 1U;
	in.arrival_ns = sim.now_ns;
	in.ssrc = sim.ssrc;
	in.rtp_timestamp = sim.ts;
	in.seq = sim.seq;
	in.kind = RTP_PAYLOAD_AUDIO;
	CHECK(udsp_jb_put(&sim.jb, &in) == -EMSGSIZE);
	in.len = UDSP_JB_PAYLOAD_MAX;
	CHECK(udsp_jb_put(&sim.jb, &in) == 0);
	udsp_jb_stats(&sim.jb, &st);
	CHECK_UINT(st.overflow_drop, 1);

	sim_reset_cfg(UDSP_JB_ADAPTIVE, 50, 100, 20, 256U, 48U, true);
	for (i = 0; i < 400U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
		if (out.decision == UDSP_JB_FRAME && out.payload) {
			frames++;
			CHECK_UINT(out.len, UDSP_JB_PAYLOAD_MAX);
		}
	}
	sim_stats(&st);
	CHECK(frames > 190);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.unit_errors, 0);
	CHECK_UINT(st.overflow_drop, 0);
	CHECK_UINT(st.underruns, 0);
}

/*
 * The deferred anchor at 48 kHz. A parked clock refuses a far-ahead packet,
 * and the stream resumes on the timeline that follows. The far bound is read
 * at the new clock: 100 ms of window is 4800 ticks, not 800. The case needs
 * jb_parked, which needs an empty queue; three lost packets drain the reserve
 * of the floor and the packet in flight.
 */
static void test_wb_hold_refill(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint32_t parked_ts = 0;
	unsigned i;

	CASE("jb48_hold_far_then_refill");
	sim_reset_cfg(UDSP_JB_ADAPTIVE, 50, 100, 20, 40, 48U, true);
	for (i = 0; i < 4000U; i++) {
		if (!(i % 2)) {
			if (i >= 2000U && i < 2006U)
				sim_skip(1);
			else if (i == 2006U)
				outlier_send(2000U * 48U);
			else
				sim_send(sim.now_ns);
		}
		if (i == 2006U)
			parked_ts = sim.jb.playout_ts;
		sim_tick(&out);
		if (i == 2006U)
			CHECK_UINT(sim.jb.playout_ts, parked_ts);
	}
	sim_stats(&st);
	CHECK_UINT(st.hold_drop, 1);
	CHECK_UINT(sim.late, 1);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.underruns, 1);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.unit_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK(st.frames_out > 3900);
}

/* A skew insert holds the clock for a tick, so the slice that it interrupts is
 * emitted again. That repeat must not carry the payload a second time. The
 * caller decoded the unit already, and an Opus decoder that gets the same
 * packet twice answers with different audio. */
static void test_wb_unit_skew_insert(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint64_t next;
	unsigned i;

	CASE("jb48_unit_skew_insert");
	sim_reset_cfg(UDSP_JB_FIXED, 50, 100, 20, 40, 48U, true);
	next = sim.now_ns;
	for (i = 0; i < 5000U; i++) {
		while (next <= sim.now_ns) {
			if (i < 3000U || i >= 3050U)
				sim_send(next);
			next += 20100000ULL;
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK(st.skew_inserts >= 5);
	CHECK_UINT(sim.unit_repeats, 0);
	CHECK_UINT(sim.unit_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* Runs the stream and answers with the last unit the buffer emitted. */
static uint32_t wb_unit_run(unsigned ticks)
{
	struct udsp_jb_out out;
	uint32_t unit_ts = 0;
	unsigned i;

	for (i = 0; i < ticks; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
		if (out.decision == UDSP_JB_FRAME)
			unit_ts = out.unit_ts;
	}
	return unit_ts;
}

/* Plays out what is queued and answers with the last unit the buffer emitted,
 * which is the one an emitted-unit mark still names. */
static uint32_t wb_unit_drain(unsigned ticks, uint32_t unit_ts)
{
	struct udsp_jb_out out;
	unsigned i;

	for (i = 0; i < ticks; i++) {
		sim_tick(&out);
		if (out.decision == UDSP_JB_FRAME)
			unit_ts = out.unit_ts;
	}
	return unit_ts;
}

/* Counts the emissions of one unit that carry the payload. */
static unsigned wb_unit_replay(uint32_t unit_ts, unsigned ticks)
{
	struct udsp_jb_out out;
	unsigned payloads = 0;
	unsigned i;

	for (i = 0; i < ticks; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
		if (out.decision == UDSP_JB_FRAME && out.unit_ts == unit_ts &&
		    out.payload)
			payloads++;
	}
	return payloads;
}

/*
 * Every path that moves the playout clock back onto a unit that the buffer
 * already emitted must clear the emitted-unit mark. Otherwise the buffer hands
 * the caller a marker for audio that it never decoded, and the caller replays
 * what its staging still held. The case has one leg per clearing site: jb.c
 * jb_timing_reset, jb_refill and jb_cn_reanchor. jb_anchor needs none of its
 * own, for the reason that the timing reset states.
 */
static void test_wb_unit_reanchor(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint32_t replay_ts;

	CASE("jb48_unit_payload_after_resync");
	sim_reset_cfg(UDSP_JB_ADAPTIVE, 50, 100, 20, 40, 48U, true);
	replay_ts = wb_unit_run(200U);
	CHECK(replay_ts != 0);
	/* A new source restarting on a timestamp this stream already played:
	 * the reset and the anchor both run here. */
	sim.ssrc = 0x55667788U;
	sim.ts = replay_ts;
	CHECK_UINT(wb_unit_replay(replay_ts, 200U), 1);
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 1);
	CHECK_UINT(sim.unit_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);

	CASE("jb48_unit_payload_after_refill");
	sim_reset_cfg(UDSP_JB_ADAPTIVE, 50, 100, 20, 40, 48U, true);
	replay_ts = wb_unit_drain(40U, wb_unit_run(200U));
	CHECK(replay_ts != 0);
	CHECK(sim.jb.conceal_run_ms > 0);
	sim_tick(&out);
	CHECK(out.clock_parked);
	/* The far end resumes where it stopped, which the parked clock has
	 * already run past: the refill pulls back onto the played unit. */
	sim.ts = replay_ts;
	CHECK_UINT(wb_unit_replay(replay_ts, 200U), 1);
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(sim.unit_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);

	CASE("jb48_unit_payload_after_cn_reanchor");
	sim_reset_cfg(UDSP_JB_FIXED, 60, 100, 20, 40, 48U, true);
	replay_ts = wb_unit_run(100U);
	CHECK(replay_ts != 0);
	cn_send();
	replay_ts = wb_unit_drain(100U, replay_ts);
	sim.ts = replay_ts;
	CHECK_UINT(wb_unit_replay(replay_ts, 200U), 1);
	sim_stats(&st);
	CHECK_UINT(st.cn_reanchors, 1);
	CHECK_UINT(sim.unit_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
}


/*
 * ---------------------------------------------------------------------------
 * The RFC 3551 4.1 marker (jb.h Talkspurt).
 * ---------------------------------------------------------------------------
 */

/* The shared shape: a settled 20 ms stream that suppresses `silence` packets
 * of media at tick 200 and closes the silence with a packet that carries the
 * marker when `mark` says so. `late_ticks` delays that closing packet, which
 * is what decides whether the clock is still fed or has already parked. */
static void talkspurt_run(enum udsp_jb_mode mode, uint16_t init_ms,
			  uint16_t max_ms, unsigned silence, bool mark,
			  unsigned late_ticks, struct udsp_jb_stats *st)
{
	struct udsp_jb_out out;
	uint64_t at;
	unsigned i;

	sim_reset(mode, init_ms, max_ms, 20, 80);
	for (i = 0; i < 400U; i++) {
		if (!(i % 2)) {
			if (i == 200U) {
				sim_silence(silence);
				at = sim.now_ns + late_ticks * TICK_NS;
				if (mark)
					sim_send_marked(at);
				else
					sim_send(at);
			} else {
				sim_send(sim.now_ns);
			}
		}
		sim_tick(&out);
	}
	sim_stats(st);
}

/*
 * The marked packet is queued before the clock reaches the hole, so the buffer
 * steps over the suppressed silence. No concealment frame and no underrun
 * occur, and the emitted timestamp jumps by the silence plus the ordinary
 * one-frame step.
 */
static void test_talkspurt_skips_queued_gap(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint32_t before = 0;
	uint32_t after = 0;
	uint64_t settled_silence = 0;
	bool seen = false;
	unsigned i;

	CASE("jb_talkspurt_marker_skips_queued_gap");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 400U; i++) {
		if (!(i % 2)) {
			if (i == 200U) {
				sim_silence(1);
				sim_send_marked(sim.now_ns);
			} else {
				sim_send(sim.now_ns);
			}
		}
		sim_tick(&out);
		/* The cold start fills with silence until prefill ends, and
		 * how many frames that takes is the prefill's business, not
		 * this rule's. The baseline is taken once the stream plays. */
		if (i == 100U) {
			sim_stats(&st);
			settled_silence = st.silence;
		}
		if (out.talkspurt) {
			after = out.rtp_timestamp;
			seen = true;
		} else if (!seen && out.decision == UDSP_JB_FRAME) {
			before = out.rtp_timestamp;
		}
	}
	sim_stats(&st);
	CHECK_UINT(sim.talkspurts, 1);
	CHECK(seen);
	/* 20 ms of suppressed media plus the frame the clock would have
	 * advanced anyway. */
	CHECK_UINT(after - before, 240U);
	CHECK_UINT(st.underruns, 0);
	CHECK_UINT(st.plc, 0);
	CHECK_UINT(st.silence, settled_silence);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* The control. The same silence, with no marker on the packet that closes it,
 * cannot be told from loss, so it is concealed and counted. A lost marker bit
 * falls back to this half of the pair. */
static void test_talkspurt_gap_without_marker_conceals(void)
{
	struct udsp_jb_stats st;

	CASE("jb_talkspurt_gap_without_marker_conceals");
	talkspurt_run(UDSP_JB_ADAPTIVE, 50, 100, 1, false, 0, &st);
	CHECK_UINT(sim.talkspurts, 0);
	CHECK_UINT(st.underruns, 1);
	CHECK_UINT(st.plc, 2);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/* A marker on a packet that continues the timeline has no gap to close, so
 * nothing moves. Senders raise it on the first packet of a stream and after a
 * re-INVITE, and the clock must not step for either. */
static void test_talkspurt_marker_needs_a_gap(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_talkspurt_marker_needs_a_gap");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 400U; i++) {
		if (!(i % 2)) {
			if (i == 200U)
				sim_send_marked(sim.now_ns);
			else
				sim_send(sim.now_ns);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(sim.talkspurts, 0);
	CHECK_UINT(st.underruns, 0);
	CHECK_UINT(st.plc, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/*
 * A marked packet 200 ms ahead of the clock is beyond the static reorder
 * window. The clock conceals until the gap shrinks into the window, and then
 * steps over only the rest. No second packet follows, so the step track stays
 * out (resyncs 0), and the case measures the bound of jb_talkspurt_skip only.
 *
 * Without the bound, the clock jumps the whole 200 ms on one packet: ten
 * frames of concealment and one underrun disappear. A single mis-stamped
 * marked packet would then take the timeline with it.
 *
 * The outlier stands alone: on a stream that keeps sending, the two-packet
 * step proof, with the same max_ms bound, always claims the move first.
 */
static void test_talkspurt_marker_beyond_window_ignored(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_talkspurt_marker_beyond_window_ignored");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 1200U; i++) {
		if (!(i % 2) && i < 400U)
			sim_send(sim.now_ns);
		/* The marked outlier also runs 1600 ticks ahead of its
		 * arrival, beyond the 960-tick far bound, so the jitter
		 * estimate skips it (jb_jitter_update). The counts below hold
		 * with and without that skip. */
		if (i == 400U) {
			sim.mark_next = true;
			sim_queue(sim.now_ns, sim.ts + 200U * 8U, sim.seq,
				  RTP_PAYLOAD_AUDIO, 160);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	/* the step is taken, but only once it fits the window */
	CHECK_UINT(sim.talkspurts, 1);
	/* the concealment the bound forces: without it, 40 and 1 */
	CHECK_UINT(st.plc, 50);
	CHECK_UINT(st.underruns, 2);
	/* nothing confirmed the outlier, so the step track never owned it */
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/*
 * UDSP_JB_FIXED is the fax and modem playout. There the timeline must stay
 * continuous: a hole is concealed at its full length rather than removed, so
 * the demodulator keeps its sample count. Same wire, opposite decision.
 */
static void test_talkspurt_marker_fixed_conceals(void)
{
	struct udsp_jb_stats st;

	CASE("jb_talkspurt_marker_fixed_conceals");
	talkspurt_run(UDSP_JB_FIXED, 50, 100, 1, true, 0, &st);
	CHECK_UINT(sim.talkspurts, 0);
	CHECK_UINT(st.underruns, 1);
	CHECK_UINT(st.plc, 2);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/*
 * The deliberate limit of the rule, checked so that nobody "fixes" it by
 * accident. When the closing packet arrives after the queue ran dry, the
 * buffer had nothing to play at those ticks and concealed. That is a real
 * underrun, and it stays counted. The marker only says what the sender did,
 * too late to spare the frames that the clock already emitted. Only a marked
 * packet that the buffer already holds proves the silence in time.
 */
static void test_talkspurt_marker_parked_clock_still_counts(void)
{
	struct udsp_jb_stats st;

	CASE("jb_talkspurt_marker_parked_clock_still_counts");
	talkspurt_run(UDSP_JB_ADAPTIVE, 50, 100, 1, true, 8U, &st);
	CHECK_UINT(sim.talkspurts, 0);
	CHECK_UINT(st.underruns, 1);
	CHECK(st.plc >= 2);
	CHECK_UINT(sim.account_errors, 0);
}

/*
 * The span learner must not read a silence as a packet duration. A 10 ms
 * stream that suppresses one frame offers a clean 20 ms observation. Two of
 * them back to back are the JB_SPAN_AGREE that the learner asks for. The span
 * would flip, and every later 80-byte frame would be sliced into 40-byte
 * halves. The marker says that the delta measured a silence, so the delta
 * teaches nothing.
 */
static void test_talkspurt_marker_teaches_no_span(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_talkspurt_marker_teaches_no_span");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 10, 80);
	for (i = 0; i < 400U; i++) {
		/* Back to back, because one ordinary packet between them
		 * passes a delta equal to the learned span and resets
		 * span_agree. Two talkspurts in a row are what the learner
		 * would otherwise accept as JB_SPAN_AGREE. */
		if (i == 200U || i == 201U) {
			sim_silence(1);
			sim_send_marked(sim.now_ns);
		} else {
			sim_send(sim.now_ns);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	CHECK_UINT(sim.jb.pkt_span_ts, 80U);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}


/*
 * ---------------------------------------------------------------------------
 * The standing reserve and the two halves of the clock-skew correction.
 * ---------------------------------------------------------------------------
 */

/*
 * The floor is one packet span plus one frame. What that buys is a sawtooth
 * whose floor is one whole span: the queue never runs empty between arrivals,
 * so an arrival later than the tick that needs it still finds audio to play.
 * The property is stated as the measured floor of depth_ms, not as the target,
 * because the target is the knob and the floor is the behavior under test.
 *
 * With a bare-span floor the same streams floor at 10 ms (ptime 20) and at zero
 * (ptime 10): depth_ms then reads 0 to 10, with the mode at 0.
 */
static void talkspurt_reserve_probe(unsigned span_ms, unsigned bpf,
				    uint16_t *floor_ms, struct udsp_jb_stats *st)
{
	struct udsp_jb_out out;
	unsigned i;

	*floor_ms = 0xffffU;
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, span_ms, bpf);
	for (i = 0; i < 6000U; i++) {
		if (!(i % (span_ms / UDSP_JB_FRAME_MS)))
			sim_send(sim.now_ns);
		sim_tick(&out);
		/* past the startup shed, where the prefill surplus is gone */
		if (i > 3000U) {
			sim_stats(st);
			if (st->depth_ms < *floor_ms)
				*floor_ms = st->depth_ms;
		}
	}
	sim_stats(st);
}

static void test_jb2_floor_holds_one_span(void)
{
	struct udsp_jb_stats st;
	uint16_t floor_ms;

	CASE("jb_floor_holds_one_span_of_reserve");

	talkspurt_reserve_probe(20, 80, &floor_ms, &st);
	CHECK_UINT(st.target_ms, 30);
	CHECK_UINT(floor_ms, 20);		/* bare span floors at 10 */
	CHECK_UINT(st.underruns, 0);
	CHECK_UINT(st.plc, 0);

	talkspurt_reserve_probe(10, 80, &floor_ms, &st);
	CHECK_UINT(st.target_ms, 20);
	CHECK_UINT(floor_ms, 10);		/* bare span floors at 0 */
	CHECK_UINT(st.underruns, 0);
	CHECK_UINT(st.plc, 0);

	talkspurt_reserve_probe(30, 80, &floor_ms, &st);
	CHECK_UINT(st.target_ms, 40);
	CHECK_UINT(floor_ms, 30);
	CHECK_UINT(st.underruns, 0);
	CHECK_UINT(st.plc, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/*
 * The handoff between the two skew arms, and why neither alone is enough.
 *
 * The far end runs 1% fast, which is far beyond any real oscillator and is
 * chosen so the drift outruns the arms quickly enough to see. The lossy half
 * adds one lost packet a second, so a concealment falls inside every trend
 * window and the patient arm's clean hold can never mature.
 *
 * The immediate arm is what saves it: once the excess passes two packet spans
 * it is larger than the sawtooth and one transit step together can make, so it
 * is drift by construction and arms at once. Two spans is the arming threshold,
 * not a bound on the excess: the correction runs at one frame per second, so a
 * 1% drift stands well above the threshold while it is worked off (80 ms
 * instantaneous, 82 ms averaged, against a 40 ms threshold). What the arm buys
 * is that the queue does not run away, and it buys the same standing depth
 * whether the line loses packets or not.
 *
 * Remove the patient arm's hold and the correction runs slightly early, so the
 * line settles a frame shallower (90 against 110). Remove the immediate arm and
 * the lossy line never corrects: the hold is reset before it can mature and the
 * queue grows to three seconds (measured: drops 0, depth 3080 ms, 1830
 * underruns, 6888 concealed frames).
 */
static void jb2_skew_run(int lossy, struct udsp_jb_stats *st)
{
	struct udsp_jb_out out;
	uint64_t next;
	unsigned i;

	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	next = sim.now_ns;
	for (i = 0; i < 30000U; i++) {
		while (next <= sim.now_ns) {
			if (lossy && (i % 100U) == 50U)
				sim_skip(1);
			else
				sim_send(next);
			next += 19800000ULL;
		}
		sim_tick(&out);
	}
	sim_stats(st);
}

static void test_jb2_skew_arms_hand_off(void)
{
	struct udsp_jb_stats st;

	CASE("jb_skew_arms_hand_off_on_a_lossy_line");

	jb2_skew_run(0, &st);
	CHECK_UINT(st.skew_drops, 297);
	CHECK_UINT(st.underruns, 0);
	CHECK_UINT(st.plc, 0);
	CHECK_UINT(st.depth_ms, 110);

	jb2_skew_run(1, &st);
	/* the loss changes what is concealed, never how the drift is corrected */
	CHECK_UINT(st.skew_drops, 297);
	CHECK_UINT(st.depth_ms, 110);
	CHECK_UINT(st.underruns, 152);
	CHECK_UINT(st.plc, 304);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
}

/*
 * ---------------------------------------------------------------------------
 * Annex B: SID slicing, DTX gaps and the SID-source comfort noise (jb.h SID).
 * The scripted codec of the sender is one 10-byte speech frame and a 2-byte
 * SID, so a SID-tailed packet is N*10 + 2 bytes and a SID-only packet is 2.
 * ---------------------------------------------------------------------------
 */
#define SID_BPF		10U
#define SID_LEN		2U

/* A settled Annex B stream of the given ptime that closes its talkspurt with a
 * SID-tailed packet and then goes dark: the buffer must play comfort noise
 * across the DTX gap, not the loss ladder. */
static void sid_slice_run(unsigned span_ms)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned fpp = span_ms / UDSP_JB_FRAME_MS;
	unsigned short_speech = 0;
	unsigned sid_slices = 0;
	unsigned i;

	sim_reset_sid(UDSP_JB_FIXED, 60, 100, span_ms, SID_BPF, SID_LEN);
	for (i = 0; i < 300U; i++) {
		if (!(i % fpp))
			sim_send(sim.now_ns);
		sim_tick(&out);
		if (out.decision != UDSP_JB_FRAME)
			continue;
		if (out.slice + 1U < out.slices && out.len != SID_BPF)
			short_speech++;
	}
	/* Close the talkspurt on a full ptime of speech plus the SID. */
	sim_send_sid(sim.now_ns, fpp);
	for (i = 0; i < 300U; i++) {
		if (!(i % fpp) && i < 20U)
			sim_send_sid(sim.now_ns, fpp);
		sim_tick(&out);
		if (out.decision == UDSP_JB_FRAME && out.len == SID_LEN &&
		    out.slice + 1U == out.slices)
			sid_slices++;
	}
	sim_stats(&st);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(short_speech, 0);
	CHECK(sid_slices > 0);
	CHECK_UINT(sim.jb.pkt_span_ts, (uint16_t)(span_ms * sim.ts_per_ms));
	CHECK_UINT(st.late_drop, 0);
}

static void test_sid_slicing(void)
{
	CASE("jb_sid_slice_ptime10");
	sid_slice_run(10);
	CASE("jb_sid_slice_ptime20");
	sid_slice_run(20);
	CASE("jb_sid_slice_ptime30");
	sid_slice_run(30);
}

/* A SID-only slot spans exactly one frame and emits its 2-byte SID slice,
 * whatever the learned span. A SID flush of a partial packet ends the packet
 * below the learned ptime, and still slices right. */
static void test_sid_only_and_partial(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned sid_only = 0;
	unsigned sid_tails = 0;
	unsigned i;

	CASE("jb_sid_only_slot_spans_one_frame");
	sim_reset_sid(UDSP_JB_FIXED, 40, 100, 20, SID_BPF, SID_LEN);
	/* Warm up a 20 ms speech stream so the span learns 160. */
	for (i = 0; i < 40U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	CHECK_UINT(sim.jb.pkt_span_ts, 160);
	/* A SID-only packet: one frame span, one SID slice. Its timestamp is
	 * where the next speech frame would have started. */
	{
		uint32_t sid_ts = sim.ts;

		sim_send_sid(sim.now_ns, 0);
		sim_deliver(sim.now_ns);
		/* The stream frontier advances by the DERIVED one-frame span,
		 * not the learned packet span (jb_pkt_span). */
		CHECK_UINT(sim.jb.stream_end_ts, sid_ts + 80U);
	}
	for (i = 0; i < 4U; i++) {
		sim_tick(&out);
		if (out.decision == UDSP_JB_FRAME && out.len == SID_LEN) {
			sid_only++;
			CHECK_UINT(out.slices, 1);
		}
	}
	CHECK(sid_only > 0);

	CASE("jb_sid_flush_partial_packet");
	sim_reset_sid(UDSP_JB_FIXED, 40, 100, 30, SID_BPF, SID_LEN);
	for (i = 0; i < 60U; i++) {
		if (!(i % 3))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	CHECK_UINT(sim.jb.pkt_span_ts, 240);
	/* One speech frame then the SID: a two-frame packet against a 30 ms
	 * learned ptime, the early flush of a partial packet. */
	sim_send_sid(sim.now_ns, 1);
	for (i = 0; i < 20U; i++) {
		sim_tick(&out);
		if (out.decision != UDSP_JB_FRAME)
			continue;
		if (out.len == SID_LEN) {
			sid_tails++;
			/* The SID tail is slice 1 of a two-slice packet. */
			CHECK_UINT(out.slices, 2);
			CHECK_UINT(out.slice, 1);
		}
	}
	/* Exactly one SID tail slice: the partial packet flushed and sliced
	 * right against the 30 ms learned span. */
	CHECK_UINT(sid_tails, 1);
	sim_stats(&st);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(st.late_drop, 0);
}

/* The very first packet of a stream is SID-tailed, before the span learner has
 * a pair. The derived span emits every slice, including the SID tail. */
static void test_sid_first_packet(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned sid_slices = 0;
	unsigned i;

	CASE("jb_sid_first_packet_before_span_learn");
	sim_reset_sid(UDSP_JB_FIXED, 40, 100, 20, SID_BPF, SID_LEN);
	CHECK(!sim.jb.span_learned);
	/* Two speech frames and a SID as the first packet ever seen. */
	sim_send_sid(sim.now_ns, 2);
	for (i = 0; i < 20U; i++) {
		sim_tick(&out);
		if (out.decision == UDSP_JB_FRAME && out.len == SID_LEN &&
		    out.slice + 1U == out.slices)
			sid_slices++;
	}
	sim_stats(&st);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sid_slices, 1);
	/* Three units played: two speech slices and the SID slice. */
	CHECK_UINT(st.frames_out, 3);
	CHECK_UINT(st.plc, 0);
}

/* The gap after a SID plays SID-source comfort noise. The marker is set, and
 * the stored SID payload emits once, then len-0 continuations, never the PLC
 * ladder. */
static void test_sid_gap_plays_cn(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned cn_marked = 0;
	unsigned cn_zero = 0;
	unsigned cn_payload = 0;
	unsigned i;

	CASE("jb_sid_gap_plays_source_cn");
	sim_reset_sid(UDSP_JB_FIXED, 40, 100, 20, SID_BPF, SID_LEN);
	for (i = 0; i < 40U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	/* Close with a full-ptime speech packet plus SID, then a mid-gap SID
	 * refresh, then nothing. */
	sim_send_sid(sim.now_ns, 2);
	for (i = 0; i < 200U; i++) {
		if (i == 10U) {
			sim_dtx_gap(5);
			sim_send_sid(sim.now_ns, 0);
		}
		sim_tick(&out);
		if (out.decision != UDSP_JB_CN)
			continue;
		CHECK(out.cn_sid);
		cn_marked++;
		if (out.len == SID_LEN)
			cn_payload++;
		else if (out.len == 0)
			cn_zero++;
	}
	sim_stats(&st);
	CHECK(cn_marked > 0);
	CHECK(cn_zero > 0);
	/* The mid-gap SID refresh emits its stored payload exactly once. */
	CHECK_UINT(cn_payload, 1);
	CHECK_UINT(st.cn, cn_marked);
	CHECK_UINT(st.plc, 0);
	CHECK_UINT(sim.payload_errors, 0);
}

/* A gap with no preceding SID keeps the loss ladder: this is the loss/DTX
 * distinction. */
static void test_sid_gap_without_sid_plcs(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_gap_without_sid_keeps_plc");
	sim_reset_sid(UDSP_JB_FIXED, 60, 100, 20, SID_BPF, SID_LEN);
	for (i = 0; i < 300U; i++) {
		if (!(i % 2)) {
			if (i >= 100U && i < 112U)
				sim_skip(1);
			else
				sim_send(sim.now_ns);
		}
		sim_tick(&out);
	}
	sim_stats(&st);
	/* Plain speech packets, plain loss: the ladder runs and no comfort
	 * noise is synthesized. */
	CHECK(st.plc >= 12);
	CHECK_UINT(st.cn, 0);
	CHECK_UINT(sim.payload_errors, 0);
}

/* A talkspurt resume without the RFC 3551 marker still teaches the span
 * learner nothing across the SID-ended packet. DTX consumed no sequence
 * number, so the resume is seq-consecutive with the SID close, and the delta
 * measures the silence. */
static void test_sid_resume_no_marker(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_sid_resume_without_marker_teaches_no_span");
	sim_reset_sid(UDSP_JB_FIXED, 60, 100, 10, SID_BPF, SID_LEN);
	/* A 10 ms speech stream, so the learned span is 80. */
	for (i = 0; i < 60U; i++) {
		sim_send(sim.now_ns);
		sim_tick(&out);
	}
	CHECK_UINT(sim.jb.pkt_span_ts, 80);
	/* Two SID-ended silences back to back, each closed and reopened with a
	 * seq-consecutive unmarked packet: a naive learner would read the
	 * two-frame silence span twice and flip pkt_span_ts to 160. */
	for (unsigned rep = 0; rep < 2U; rep++) {
		sim_send_sid(sim.now_ns, 1);
		sim_dtx_gap(1);
		for (i = 0; i < 4U; i++)
			sim_tick(&out);
		for (i = 0; i < 10U; i++) {
			sim_send(sim.now_ns);
			sim_tick(&out);
		}
	}
	sim_stats(&st);
	CHECK_UINT(sim.jb.pkt_span_ts, 80);
	CHECK_UINT(sim.payload_errors, 0);
}

/* A resume after a silence longer than the resync bound is exempt from the
 * timestamp jump resync while SID CNG is armed. The timestamp of the sender
 * counted through the whole silence, so the marked resume continues the
 * talkspurt and does not resync. */
static void test_sid_long_silence_resume(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned frames_after = 0;
	unsigned i;

	CASE("jb_sid_resume_after_long_silence_no_resync");
	sim_reset_sid(UDSP_JB_ADAPTIVE, 50, 100, 20, SID_BPF, SID_LEN);
	for (i = 0; i < 200U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	/* Close the talkspurt, then let 15 s of silence pass with nothing on
	 * the wire; the sender's timestamp keeps counting through it. This is
	 * past the resync bound (10 s), so the resume's delta from the last
	 * received timestamp reads as a jump, and only the DTX exemption keeps
	 * it from resyncing. The free-running CNG clock parks at the 10 s
	 * bound, so the resume stays inside the exemption's playout-distance
	 * check. */
	sim_send_sid(sim.now_ns, 2);
	sim_dtx_gap(15U * 100U);
	for (i = 0; i < 1500U; i++)
		sim_tick(&out);
	/* The resume carries the RFC 3551 marker after the suppressed silence,
	 * 15 s ahead of where we last had audio but continuous for the sender. */
	sim.mark_next = true;
	sim_send(sim.now_ns);
	for (i = 0; i < 400U; i++) {
		if (i && !(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
		if (out.decision == UDSP_JB_FRAME)
			frames_after++;
	}
	sim_stats(&st);
	/* The exemption spared the resync; the talkspurt plays on. */
	CHECK_UINT(st.resyncs, 0);
	CHECK(frames_after > 180);
	CHECK_UINT(sim.payload_errors, 0);
}

/*
 * The CNG clock bound. A sender within the specification can send one SID and
 * then nothing. If it also freezes its RTP timestamp through the silence, the
 * comfort-noise clock of the buffer runs free ahead of it for the whole
 * silence. While the noise is active, jb_hold_needed is false, so none of the
 * parked-clock recovery can run. A resume stamped at the frozen timestamp
 * falls behind the free-running clock and late-drops forever. The bound parks
 * the clock after a fixed CNG run with no arrivals, and the starve resync then
 * re-anchors the resume.
 */
static void test_sid_cng_bound_frozen_resume(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned frames_after = 0;
	uint32_t resume_ts;
	unsigned i;

	CASE("jb_sid_cng_bound_frozen_resume_recovers");
	sim_reset_sid(UDSP_JB_ADAPTIVE, 50, 100, 20, SID_BPF, SID_LEN);
	for (i = 0; i < 200U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	/* Close the talkspurt and then freeze the sender: its timestamp stands
	 * still for 15 s while our CNG clock free-runs. */
	sim_send_sid(sim.now_ns, 2);
	resume_ts = sim.ts;
	for (i = 0; i < 1500U; i++)
		sim_tick(&out);
	/* The resume arrives at the frozen timestamp, far behind the
	 * free-running clock. Only a clock that parked can re-anchor it. */
	sim.ts = resume_ts;
	for (i = 0; i < 600U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
		if (i > 20U && out.decision == UDSP_JB_FRAME)
			frames_after++;
	}
	sim_stats(&st);
	/* The bound parked the clock and the starve resync recovered it. */
	CHECK_UINT(st.starve_resyncs, 1);
	CHECK(frames_after > 200);
	CHECK_UINT(sim.payload_errors, 0);
}

/*
 * The regression check of the run bound for sid_len 0: RFC 3389 comfort noise
 * is not bounded. One PT 13 description, then 15 s of silence, gives comfort
 * noise only, no concealment and no park. The frozen-sender resume re-anchors
 * through jb_cn_resume, unchanged by Annex B.
 */
static void test_pt13_cn_unbounded(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned frames_after = 0;
	unsigned i;

	CASE("jb_pt13_cn_unbounded_15s");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 100, 20, 80);
	for (i = 0; i < 2000U; i++) {
		if (!(i % 2) && i < 2000U)
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	cn_send();
	/* 15 s of silence under one standing RFC 3389 description; the far
	 * end's timestamp stands still through it. */
	for (i = 0; i < 1500U; i++)
		sim_tick(&out);
	for (i = 0; i < 400U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
		if (out.decision == UDSP_JB_FRAME)
			frames_after++;
	}
	sim_stats(&st);
	/* The whole silence played comfort noise; nothing parked and nothing
	 * concealed, and the resume took the comfort-noise re-anchor. */
	CHECK(st.cn > 1400);
	CHECK_UINT(st.plc, 0);
	CHECK_UINT(st.underruns, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.cn_reanchors, 1);
	CHECK(frames_after > 180);
	CHECK_UINT(sim.account_errors, 0);
}

/*
 * A SID-only refresh reordered across a long armed silence. It is classified
 * before the timestamp jump and the starve resync. It therefore does not reset
 * the stream (ahead, beyond the jump bound, not seq-consecutive), and does not
 * rewind a starved clock (behind a parked one). It leaves no step candidate
 * armed.
 */
static void test_sid_refresh_survives_jump_and_starve(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint32_t playout_before;
	unsigned cn_payload = 0;
	unsigned i;

	CASE("jb_sid_refresh_beyond_jump_no_reset");
	sim_reset_sid(UDSP_JB_ADAPTIVE, 50, 100, 20, SID_BPF, SID_LEN);
	for (i = 0; i < 200U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	/* Close, then 12 s of dark silence: the SID clock bound parks. */
	sim_send_sid(sim.now_ns, 2);
	sim_dtx_gap(12U * 100U);
	for (i = 0; i < 1200U; i++)
		sim_tick(&out);
	/* The refresh follows a lost packet (gap 2, unmarked), stamped at the
	 * sender's current silence position: 12 s past the last received
	 * timestamp. It must be consumed in place, not resync. */
	sim.seq++;
	sim.bpf = SID_BPF;
	sim_queue(sim.now_ns, sim.ts, sim.seq, RTP_PAYLOAD_AUDIO, SID_LEN);
	sim.seq++;
	sim.ts += sim.frame_ts;
	sim_deliver(sim.now_ns);
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	/* No step candidate rides the refresh (jb_step_track skipped). */
	CHECK_UINT(sim.jb.step_agree, 0);
	/* The refresh re-armed the noise: its payload goes out once. */
	for (i = 0; i < 6U; i++) {
		sim_tick(&out);
		if (out.decision == UDSP_JB_CN && out.cn_sid &&
		    out.len == SID_LEN)
			cn_payload++;
	}
	CHECK_UINT(cn_payload, 1);

	CASE("jb_sid_refresh_behind_starved_clock_no_rewind");
	sim_reset_sid(UDSP_JB_ADAPTIVE, 50, 100, 20, SID_BPF, SID_LEN);
	for (i = 0; i < 200U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	/* Close, then 12 s of dark silence with the sender's clock frozen:
	 * the run bound parks and the concealment run passes the starve
	 * threshold. */
	sim_send_sid(sim.now_ns, 2);
	for (i = 0; i < 1200U; i++)
		sim_tick(&out);
	playout_before = sim.jb.playout_ts;
	/* The refresh arrives at the frozen position, far behind the parked
	 * clock, after a lost packet: the starve resync must not seize it
	 * and playout must not move. */
	sim.seq++;
	sim_queue(sim.now_ns, sim.ts, sim.seq, RTP_PAYLOAD_AUDIO, SID_LEN);
	sim.seq++;
	sim_deliver(sim.now_ns);
	sim_stats(&st);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(sim.jb.playout_ts, playout_before);
}

/*
 * The derived span reaches the step candidate and the deferred hold. A far
 * resume that opens with a SID-tailed packet on a learned 30 ms stream spans
 * two frames, not three. Its seq-consecutive successor therefore falls exactly
 * on the continuation of the candidate, and confirms on the second packet.
 * With the learned span, the successor reads one frame early, and the confirm
 * window misses. The adoption then costs a third packet and a second
 * hold_drop.
 */
static void test_sid_span_step_held_ptime30(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned frames_after = 0;
	uint32_t t;
	unsigned i;

	CASE("jb_sid_span_step_candidate_ptime30");
	sim_reset_sid(UDSP_JB_ADAPTIVE, 50, 100, 30, SID_BPF, SID_LEN);
	for (i = 0; i < 300U; i++) {
		if (!(i % 3))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	CHECK_UINT(sim.jb.pkt_span_ts, 240);
	/* 1.2 s of loss parks the clock. */
	for (i = 0; i < 120U; i++) {
		if (!(i % 3))
			sim_skip(1);
		sim_tick(&out);
	}
	/* The far resume: a one-frame talkspurt closed by its SID, then the
	 * continuation. pkt2 starts where pkt1's DERIVED two-frame span
	 * ends. */
	t = sim.jb.playout_ts + 16000U;
	sim_queue(sim.now_ns, t, sim.seq, RTP_PAYLOAD_AUDIO,
		  SID_BPF + SID_LEN);
	sim_queue(sim.now_ns, t + 160U, (uint16_t)(sim.seq + 1U),
		  RTP_PAYLOAD_AUDIO, 3U * SID_BPF);
	sim_queue(sim.now_ns, t + 400U, (uint16_t)(sim.seq + 2U),
		  RTP_PAYLOAD_AUDIO, 3U * SID_BPF);
	sim.seq += 3U;
	sim.ts = t + 640U;
	for (i = 0; i < 300U; i++) {
		if (!(i % 3))
			sim_send(sim.now_ns);
		sim_tick(&out);
		if (out.decision == UDSP_JB_FRAME)
			frames_after++;
	}
	sim_stats(&st);
	/* One refusal, then the successor confirms against the derived
	 * continuation: the learned-span step_ts costs a second hold here. */
	CHECK_UINT(st.hold_drop, 1);
	CHECK_UINT(st.resyncs, 0);
	CHECK_UINT(st.starve_resyncs, 0);
	CHECK(frames_after > 200);
	CHECK_UINT(sim.account_errors, 0);
}

/*
 * The restamp skip. A SID-tailed packet queued through the prefill keeps its
 * derived two-frame span when a 30 ms first learn restamps the queue.
 * Otherwise it would emit three slices, and slip the clock a frame into its
 * neighbor.
 */
static void test_sid_restamp_skips_sid_slot(void)
{
	struct udsp_jb_out out;
	uint32_t sid_unit;
	unsigned bad_slices = 0;
	unsigned seen = 0;
	unsigned i;

	CASE("jb_sid_restamp_skips_sid_slot");
	sim_reset_sid(UDSP_JB_FIXED, 200, 300, 30, SID_BPF, SID_LEN);
	CHECK(!sim.jb.span_learned);
	/* The SID-tailed packet first, still queued when the first-learn pair
	 * behind it adopts the 30 ms span and restamps the queue. */
	sid_unit = sim.ts;
	sim_send_sid(sim.now_ns, 1);
	sim_send(sim.now_ns);
	sim_send(sim.now_ns);
	sim_deliver(sim.now_ns);
	CHECK(sim.jb.span_learned);
	CHECK_UINT(sim.jb.pkt_span_ts, 240);
	for (i = 0; i < 60U; i++) {
		sim_tick(&out);
		if (out.decision != UDSP_JB_FRAME || out.unit_ts != sid_unit)
			continue;
		seen++;
		if (out.slices != 2U)
			bad_slices++;
	}
	/* Two slices, speech then SID, whatever the learner adopted. */
	CHECK_UINT(seen, 2);
	CHECK_UINT(bad_slices, 0);
	CHECK_UINT(sim.payload_errors, 0);
}

/* jb_timing_reset clears the three Annex B fields with everything else, so a
 * new timeline starts with no armed SID state. The first accepted packet also
 * re-seeds cn_run_ms and seq_high_sid. The clear of cn_sid is the one that
 * nothing downstream shadows. */
static void test_sid_timing_reset_clears(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_sid_timing_reset_clears_annexb_state");
	sim_reset_sid(UDSP_JB_FIXED, 40, 100, 20, SID_BPF, SID_LEN);
	for (i = 0; i < 40U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	sim_send_sid(sim.now_ns, 2);
	for (i = 0; i < 15U; i++)
		sim_tick(&out);
	CHECK(sim.jb.cn_sid);
	CHECK(sim.jb.cn_run_ms > 0);
	CHECK(sim.jb.seq_high_sid);
	/* A new source: the timing reset must take the SID state with it. */
	sim.ssrc = 0x99887766U;
	sim_send(sim.now_ns);
	sim_deliver(sim.now_ns);
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 1);
	CHECK(!sim.jb.cn_sid);
	CHECK(!sim.jb.cn_valid);
	CHECK_UINT(sim.jb.cn_run_ms, 0);
	CHECK(!sim.jb.seq_high_sid);
}

/* The playout-distance bound of the exemption. Measured from the playout
 * clock, a marked, seq-consecutive packet beyond the resync bound is a foreign
 * timeline, even while SID CNG is armed. It still resyncs. */
static void test_sid_exempt_distance_bound(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_sid_exempt_distance_bound_resyncs");
	sim_reset_sid(UDSP_JB_ADAPTIVE, 50, 100, 20, SID_BPF, SID_LEN);
	for (i = 0; i < 200U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	/* Close, then 2 s of tracked silence: the CNG clock stands ~2 s past
	 * the close. */
	sim_send_sid(sim.now_ns, 2);
	sim_dtx_gap(200U);
	for (i = 0; i < 200U; i++)
		sim_tick(&out);
	/* A marked packet 11 s past the clock: marker and successor both
	 * vouch, the distance does not. */
	sim_dtx_gap(1100U);
	sim.mark_next = true;
	sim_send(sim.now_ns);
	sim_deliver(sim.now_ns);
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 1);
}

/* The cn_sid requirement of the exemption. Under an RFC 3389 silence, the
 * timestamp jump keeps the meaning that it has without Annex B, marker or
 * not. */
static void test_sid_exempt_needs_cn_sid(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_sid_exempt_requires_cn_sid");
	sim_reset_sid(UDSP_JB_ADAPTIVE, 50, 100, 20, SID_BPF, SID_LEN);
	for (i = 0; i < 200U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	/* An RFC 3389 silence: cn_armed without cn_sid. */
	cn_send();
	for (i = 0; i < 1100U; i++) {
		sim_dtx_gap(1);
		sim_tick(&out);
	}
	/* The marked, seq-consecutive resume arrives right at the free-running
	 * clock, 11 s past the last received timestamp: without an armed SID
	 * silence the jump owns it. */
	sim.mark_next = true;
	sim_send(sim.now_ns);
	sim_deliver(sim.now_ns);
	sim_stats(&st);
	CHECK_UINT(st.resyncs, 1);
}

/* The jb_sid_units cap through the public API. A payload of more units than
 * the widest packet plus the SID keeps the even slice. */
static void test_sid_units_cap(void)
{
	struct udsp_jb_out out;
	unsigned len26 = 0;
	unsigned len_sid = 0;
	uint32_t unit;
	unsigned i;

	CASE("jb_sid_units_cap_keeps_even_slice");
	sim_reset_sid(UDSP_JB_FIXED, 60, 100, 20, SID_BPF, SID_LEN);
	for (i = 0; i < 40U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	CHECK_UINT(sim.jb.pkt_span_ts, 160);
	/* 52 bytes reads as five speech frames and a SID, one unit past the
	 * cap: no stream this buffer covers, so the learned span and the even
	 * slice stand. */
	unit = sim.ts;
	sim_queue(sim.now_ns, sim.ts, sim.seq, RTP_PAYLOAD_AUDIO, 52);
	sim.ts += 160U;
	sim.seq++;
	sim_deliver(sim.now_ns);
	for (i = 0; i < 12U; i++) {
		sim_tick(&out);
		if (out.decision != UDSP_JB_FRAME || out.unit_ts != unit)
			continue;
		if (out.len == 26U)
			len26++;
		if (out.len == SID_LEN || out.len == SID_BPF)
			len_sid++;
	}
	CHECK_UINT(len26, 2);
	CHECK_UINT(len_sid, 0);
}

/* A late or reordered SID refresh during an armed SID silence must refresh the
 * noise in place, and never rewind the playout clock. */
static void test_sid_late_refresh_no_rewind(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	uint32_t playout_before;
	unsigned i;

	CASE("jb_sid_late_refresh_no_playout_rewind");
	sim_reset_sid(UDSP_JB_FIXED, 40, 100, 20, SID_BPF, SID_LEN);
	for (i = 0; i < 40U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	sim_send_sid(sim.now_ns, 2);
	for (i = 0; i < 10U; i++)
		sim_tick(&out);
	playout_before = sim.jb.playout_ts;
	/* A SID refresh stamped behind the current playout clock (a reordered
	 * update): it must not move the clock and must not late-drop. */
	sim.bpf = SID_BPF;
	sim_queue(sim.now_ns, sim.jb.playout_ts - 320U, sim.seq,
		  RTP_PAYLOAD_AUDIO, SID_LEN);
	sim.seq++;
	/* sim_deliver ticks no clock, so the position must be bit-equal. */
	sim_deliver(sim.now_ns);
	CHECK_UINT(sim.jb.playout_ts, playout_before);
	for (i = 0; i < 4U; i++)
		sim_tick(&out);
	sim_stats(&st);
	/* No re-anchor and no late drop: the update applies in place. */
	CHECK_UINT(st.cn_reanchors, 0);
	CHECK_UINT(st.late_drop, 0);
	CHECK_UINT(st.resyncs, 0);
}

/* An empty PT 13 comfort-noise packet stays on the RFC 3389 path, with cn_sid
 * false. The consumer dispatches on the SID-source marker, never on the
 * payload length. */
static void test_sid_pt13_stays_rfc3389(void)
{
	struct udsp_jb_out out;
	unsigned cn = 0;
	unsigned i;

	CASE("jb_empty_pt13_stays_rfc3389_cn_sid_false");
	sim_reset_sid(UDSP_JB_FIXED, 60, 100, 20, SID_BPF, SID_LEN);
	for (i = 0; i < 40U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	/* An empty (len 0) PT 13 packet: jb_cn_store keeps it, and the emitted
	 * comfort noise must not claim to be codec-sourced. */
	sim.bpf = 1U;
	sim_queue(sim.now_ns, sim.ts, sim.seq, RTP_PAYLOAD_COMFORT_NOISE, 0);
	sim.bpf = SID_BPF;
	sim_skip(1);
	for (i = 0; i < 40U; i++) {
		sim_tick(&out);
		if (out.decision != UDSP_JB_CN)
			continue;
		CHECK(!out.cn_sid);
		cn++;
	}
	CHECK(cn > 0);
}

/* A PT 13 store after a SID armed the codec silence clears cn_sid. The last
 * write wins on the shared slot. Otherwise, the consumer would hand an RFC 3389
 * octet to the codec decode. */
static void test_sid_pt13_after_sid_clears(void)
{
	struct udsp_jb_out out;
	unsigned cn_sid = 0;
	unsigned cn_plain = 0;
	unsigned i;

	CASE("jb_pt13_after_sid_clears_cn_sid");
	sim_reset_sid(UDSP_JB_FIXED, 60, 100, 20, SID_BPF, SID_LEN);
	for (i = 0; i < 40U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	/* SID close arms cn_sid; drain the queue into the gap and confirm the
	 * comfort noise is codec-sourced. */
	sim_send_sid(sim.now_ns, 2);
	for (i = 0; i < 30U; i++) {
		sim_tick(&out);
		if (out.decision == UDSP_JB_CN && out.cn_sid)
			cn_sid++;
	}
	/* A one-byte PT 13 arrives in the same silence: last write wins, so the
	 * shared slot now describes RFC 3389 noise. */
	sim.bpf = 1U;
	sim_queue(sim.now_ns, sim.ts, sim.seq, RTP_PAYLOAD_COMFORT_NOISE, 1);
	sim.bpf = SID_BPF;
	sim_skip(1);
	for (i = 0; i < 20U; i++) {
		sim_tick(&out);
		if (out.decision == UDSP_JB_CN && !out.cn_sid)
			cn_plain++;
	}
	/* The gap started on codec noise, the PT 13 flipped it to RFC 3389. */
	CHECK(cn_sid > 0);
	CHECK(cn_plain > 0);
}

/*
 * The span learner ignores a SID-ended pair in both directions. A fresh stream
 * adopts its first plausible observation before any frame plays, so an
 * unguarded SID-poisoned pair in the prefill would flip the span. Each
 * direction runs on its own fresh buffer, so the at-once adoption is the exact
 * test. The seq_high_sid guard covers a SID-ended seq_high with a plain
 * arrival, and the sid_tail guard a SID-ended arrival with a plain seq_high. A
 * SID-ended packet spans two frames plus one suppressed frame. On a 10 ms
 * grid, that is a 240-tick delta, which a naive learner reads as a 30 ms
 * ptime.
 */
static void test_sid_learner_immune_both_directions(void)
{
	CASE("jb_sid_learner_immune_seq_high_sid");
	sim_reset_sid(UDSP_JB_FIXED, 200, 300, 10, SID_BPF, SID_LEN);
	/* A SID-ended packet becomes seq_high, then a seq-consecutive plain
	 * resume 240 ticks later, both before the first frame plays. */
	sim_send_sid(sim.now_ns, 1);
	sim_dtx_gap(1);
	sim.bpf = SID_BPF;
	sim_queue(sim.now_ns, sim.ts, sim.seq, RTP_PAYLOAD_AUDIO, SID_BPF);
	sim.ts += sim.frame_ts;
	sim.seq++;
	sim_deliver(sim.now_ns);
	/* The seq_high_sid guard held: no at-once flip to 240. */
	CHECK(!sim.jb.span_learned);
	CHECK_UINT(sim.jb.pkt_span_ts, 80);

	CASE("jb_sid_learner_immune_sid_tail");
	sim_reset_sid(UDSP_JB_FIXED, 200, 300, 10, SID_BPF, SID_LEN);
	/* A plain packet becomes seq_high, then a seq-consecutive SID-ended
	 * packet 240 ticks later: the arriving side is the SID. */
	sim_queue(sim.now_ns, sim.ts, sim.seq, RTP_PAYLOAD_AUDIO, SID_BPF);
	sim.ts += 3U * sim.frame_ts;
	sim.seq++;
	sim_send_sid(sim.now_ns, 1);
	sim_deliver(sim.now_ns);
	/* The sid_tail guard held. */
	CHECK(!sim.jb.span_learned);
	CHECK_UINT(sim.jb.pkt_span_ts, 80);
}

/* A zero-length audio payload passes through as a len-0 FRAME slice, whether
 * or not the stream is SID-capable. The codec turns it into a continuation or
 * a concealment, and the buffer applies no special case. */
static void test_sid_zero_len_audio_passthrough(void)
{
	struct udsp_jb_out out;
	unsigned zero_frames = 0;
	unsigned i;

	CASE("jb_zero_length_audio_passthrough");
	sim_reset_sid(UDSP_JB_FIXED, 40, 100, 10, SID_BPF, SID_LEN);
	for (i = 0; i < 40U; i++) {
		sim_send(sim.now_ns);
		sim_tick(&out);
	}
	/* A len-0 audio packet on the speech timeline: stored and emitted as a
	 * FRAME slice with len 0, not routed to comfort noise. */
	sim_queue(sim.now_ns, sim.ts, sim.seq, RTP_PAYLOAD_AUDIO, 0);
	sim.ts += sim.frame_ts;
	sim.seq++;
	sim_deliver(sim.now_ns);
	for (i = 0; i < 4U; i++) {
		sim_tick(&out);
		if (out.decision == UDSP_JB_FRAME && out.len == 0)
			zero_frames++;
	}
	CHECK(zero_frames > 0);
}

/* The regression check for sid_len 0. An init with no SID configuration
 * behaves exactly as the narrowband stream, so a 12-byte packet is
 * byte-sliced evenly. The rest of the suite relies on that contract, and this
 * case checks it once, explicitly. */
static void test_sid_len0_regression(void)
{
	struct udsp_jb_stats st;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_sid_len0_even_slice_regression");
	sim_reset_sid(UDSP_JB_FIXED, 60, 100, 20, SID_BPF, 0);
	CHECK_UINT(sim.jb.sid_len, 0);
	for (i = 0; i < 200U; i++) {
		if (!(i % 2))
			sim_send(sim.now_ns);
		sim_tick(&out);
		if (out.decision == UDSP_JB_FRAME)
			CHECK_UINT(out.len, SID_BPF);
	}
	sim_stats(&st);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(st.cn, 0);
	CHECK_UINT(st.plc, 0);
}

/* --- skipped playout opportunities (jb.h) --------------------------------- */

/* The output partition of jb.h Event mapping. */
static uint64_t skip_out_total(const struct udsp_jb_stats *st)
{
	return st->frames_out + st->plc + st->cn + st->silence +
	       st->skew_inserts;
}

/*
 * A hold of n ticks: the far end sends one packet a tick when sending is set,
 * and no get runs. The arrivals then go in with their own stamps, the skip
 * pays the n opportunities, one at a time or as a run, and the order oracle
 * takes the clock the skip left. The unit oracle reads the outputs a caller
 * decodes; after a run, the next unit goes out whole (jb.h udsp_jb_skip).
 */
static void skip_hold(unsigned n, bool sending, bool run,
		      struct udsp_jb_out *outs)
{
	struct udsp_jb_out out;
	uint32_t from;
	unsigned i;

	for (i = 0; i < n; i++) {
		if (sending)
			sim_send(sim.now_ns);
		sim.now_ns += TICK_NS;
	}
	sim_deliver(sim.now_ns);
	sim_reanchor_note();
	from = sim.jb.playout_ts;
	if (run) {
		udsp_jb_skip(&sim.jb, n, sim.now_ns);
		sim.unit_valid = false;
	} else {
		for (i = 0; i < n; i++) {
			udsp_jb_skip_get(&sim.jb, sim.now_ns, &out);
			if (sim.whole)
				sim_unit_verify(&out);
			if (outs)
				outs[i] = out;
		}
	}
	udsp_jb_skip_end(&sim.jb);
	if (sim.expect_valid && !sim.stalled)
		sim.expect_ts += sim.jb.playout_ts - from;
}

/* A steady 10 ms stream, one packet a tick, with no loss and no jitter. */
static void skip_steady(enum udsp_jb_mode mode, unsigned ticks)
{
	struct udsp_jb_out out;
	unsigned i;

	sim_reset(mode, 20, 100, 10, 80);
	for (i = 0; i < ticks; i++) {
		sim_send(sim.now_ns);
		sim_tick(&out);
	}
}

/* One pass of test_skip_play: single opportunities, or one run. */
static void skip_play_run(bool run)
{
	struct udsp_jb_stats st0, st1, st2;
	struct udsp_jb_out out;
	uint32_t playout0;
	unsigned i;

	skip_steady(UDSP_JB_ADAPTIVE, 300);
	sim_stats(&st0);
	playout0 = sim.jb.playout_ts;
	skip_hold(6, true, run, NULL);
	sim_stats(&st1);
	CHECK_UINT(sim.jb.playout_ts - playout0, 6U * UDSP_JB_FRAME_TS);
	CHECK_UINT(sim.jb.skip_units, 6);
	CHECK_UINT(sim.jb.skip_frames, 6);
	CHECK_UINT(sim.jb.skip_waits, 0);
	CHECK_UINT(st1.depth_ms, st0.depth_ms);
	CHECK_UINT(skip_out_total(&st1), skip_out_total(&st0));
	CHECK_UINT(st1.underruns, st0.underruns);
	CHECK_UINT(st1.skew_drops, st0.skew_drops);
	CHECK_UINT(st1.skew_inserts, st0.skew_inserts);
	CHECK_UINT(sim.jb.skew_pending, 0);
	for (i = 0; i < 300U; i++) {
		sim_send(sim.now_ns);
		sim_tick(&out);
	}
	sim_stats(&st2);
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.account_errors, 0);
	CHECK_UINT(st2.late_drop, 0);
	CHECK_UINT(st2.underruns, st0.underruns);
	CHECK_UINT(st2.skew_drops, st0.skew_drops);
	CHECK_UINT(st2.depth_ms, st0.depth_ms);
	CHECK_UINT(skip_out_total(&st2), skip_out_total(&st0) + 300U);
}

/*
 * The far end keeps sending through a hold of six periods. Paid one get each,
 * the six opportunities take the oldest six units of the queue, and the queue
 * stands where it stood. No output counter, underrun or clock-skew correction
 * counts them, and the stream plays on in order. Unpaid, the six units stand
 * as 60 ms of delay.
 */
static void test_skip_play(void)
{
	struct udsp_jb_stats st0, st1;
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_skip_get_pays_each_unit");
	skip_play_run(false);
	CASE("jb_skip_run_pays_each_unit");
	skip_play_run(true);

	CASE("jb_skip_unpaid_hold_stands_as_delay");
	skip_steady(UDSP_JB_ADAPTIVE, 300);
	sim_stats(&st0);
	for (i = 0; i < 6U; i++) {
		sim_send(sim.now_ns);
		sim.now_ns += TICK_NS;
	}
	sim_send(sim.now_ns);
	sim_tick(&out);
	sim_stats(&st1);
	CHECK_UINT(st1.depth_ms, st0.depth_ms + 60U);
}

/*
 * A prefill playout begins at its target. So a hold inside the prefill passes
 * only the units that the queue holds above the target. Here the hold fills
 * the queue to 90 ms, and the target is 50. Four units pass, four
 * opportunities wait, and the next get starts playout at the target.
 */
static void test_skip_prefill(void)
{
	struct udsp_jb_out out;

	CASE("jb_skip_prefill_passes_the_surplus");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 200, 10, 80);
	sim_send(sim.now_ns);
	sim_tick(&out);
	CHECK_UINT(sim.jb.state, 1);
	skip_hold(8, true, false, NULL);
	CHECK_UINT(sim.jb.state, 1);
	CHECK_UINT(sim.jb.skip_units, 4);
	CHECK_UINT(sim.jb.skip_frames, 4);
	CHECK_UINT(sim.jb.skip_waits, 4);
	sim_send(sim.now_ns);
	sim_tick(&out);
	CHECK_UINT(sim.jb.state, 2);
	CHECK_UINT(out.decision, UDSP_JB_FRAME);
	/* Units 0 to 3 passed, so unit 4 plays, with the target behind it. */
	CHECK_UINT(out.rtp_timestamp, 160000U + 4U * UDSP_JB_FRAME_TS);

	CASE("jb_skip_idle_waits");
	sim_reset(UDSP_JB_ADAPTIVE, 50, 200, 10, 80);
	skip_hold(5, false, false, NULL);
	CHECK_UINT(sim.jb.state, 0);
	CHECK_UINT(sim.jb.skip_waits, 5);
	CHECK_UINT(sim.jb.skip_units, 0);
	CHECK_UINT(udsp_jb_timeline(&sim.jb), 0);
	sim_send(sim.now_ns);
	sim_tick(&out);
	CHECK_UINT(udsp_jb_timeline(&sim.jb), 1);
}

/*
 * A clock that parked on a dry queue stays parked through a hold with no
 * arrival. The opportunities conceal and leave the clock where it stood, so
 * the resume of the far end is not late. Comfort noise is the other case: an
 * RFC 3389 description runs the clock, one frame per opportunity.
 */
static void test_skip_dry(void)
{
	struct udsp_jb_stats st0, st1;
	struct udsp_jb_out out;
	uint32_t playout0;
	uint16_t run0;
	unsigned i;

	CASE("jb_skip_parked_clock_holds");
	skip_steady(UDSP_JB_ADAPTIVE, 300);
	for (i = 0; i < 10U; i++) {
		sim_skip(1);
		sim_tick(&out);
	}
	CHECK(out.clock_parked);
	sim_stats(&st0);
	playout0 = sim.jb.playout_ts;
	run0 = sim.jb.conceal_run_ms;
	for (i = 0; i < 10U; i++)
		sim_skip(1);
	skip_hold(10, false, true, NULL);
	sim_stats(&st1);
	CHECK_UINT(sim.jb.playout_ts, playout0);
	CHECK_UINT(sim.jb.conceal_run_ms, run0 + 100U);
	CHECK_UINT(sim.jb.skip_waits, 10);
	CHECK_UINT(skip_out_total(&st1), skip_out_total(&st0));
	CHECK_UINT(st1.underruns, st0.underruns);
	for (i = 0; i < 200U; i++) {
		sim_send(sim.now_ns);
		sim_tick(&out);
	}
	sim_stats(&st1);
	CHECK_UINT(st1.late_drop, 0);
	CHECK_UINT(st1.starve_resyncs, 0);
	CHECK_UINT(sim.order_errors, 0);

	CASE("jb_skip_rfc3389_runs_the_clock");
	skip_steady(UDSP_JB_ADAPTIVE, 300);
	cn_send();
	for (i = 0; i < 20U; i++) {
		sim_skip(1);
		sim_tick(&out);
	}
	CHECK_UINT(out.decision, UDSP_JB_CN);
	sim_stats(&st0);
	playout0 = sim.jb.playout_ts;
	for (i = 0; i < 10U; i++)
		sim_skip(1);
	skip_hold(10, false, true, NULL);
	sim_stats(&st1);
	CHECK_UINT(sim.jb.playout_ts - playout0, 10U * UDSP_JB_FRAME_TS);
	CHECK_UINT(sim.jb.skip_units, 10);
	CHECK_UINT(skip_out_total(&st1), skip_out_total(&st0));
	for (i = 0; i < 200U; i++) {
		sim_send(sim.now_ns);
		sim_tick(&out);
	}
	sim_stats(&st1);
	CHECK_UINT(st1.late_drop, 0);
	CHECK_UINT(st1.cn_reanchors, 0);
	CHECK_UINT(sim.order_errors, 0);
}

/* One mode of test_skip_talkspurt. */
static void skip_talkspurt_run(bool fixed)
{
	struct udsp_jb_out outs[8];
	unsigned conceal, talkspurt;
	uint32_t playout0;
	unsigned i;

	skip_steady(fixed ? UDSP_JB_FIXED : UDSP_JB_ADAPTIVE, 300);
	playout0 = sim.jb.playout_ts;
	for (i = 0; i < 8U; i++) {
		if (i == 2U || i == 3U || i == 4U)
			sim_silence(1);
		else if (i == 5U)
			sim_send_marked(sim.now_ns);
		else
			sim_send(sim.now_ns);
		sim.now_ns += TICK_NS;
	}
	sim.now_ns -= 8U * TICK_NS;
	skip_hold(8, false, false, outs);
	/* A park after the queue ran dry is not the gap. */
	conceal = talkspurt = 0;
	for (i = 0; i < 8U; i++) {
		conceal += outs[i].decision == UDSP_JB_PLC &&
			   !outs[i].clock_parked;
		talkspurt += outs[i].talkspurt;
	}
	if (fixed) {
		CHECK_UINT(talkspurt, 0);
		CHECK_UINT(conceal, 3);
		CHECK_UINT(sim.jb.playout_ts - playout0,
			   8U * UDSP_JB_FRAME_TS);
		CHECK_UINT(sim.jb.skip_units, 8);
	} else {
		CHECK_UINT(talkspurt, 1);
		CHECK_UINT(conceal, 0);
		CHECK(sim.jb.playout_ts - playout0 >
		      8U * UDSP_JB_FRAME_TS);
	}
}

/*
 * A hold that holds a suppressed silence. The adaptive buffer steps over it
 * with the talkspurt rule, in one opportunity. The fixed buffer of the fax
 * path never shortens the timeline. It conceals the three frames of the gap,
 * one opportunity each. So the skip passes exactly one frame per opportunity,
 * and the queue behind the gap stands as it stood.
 */
static void test_skip_talkspurt(void)
{
	CASE("jb_skip_adaptive_steps_over_a_suppressed_gap");
	skip_talkspurt_run(false);
	CASE("jb_skip_fixed_conceals_a_suppressed_gap");
	skip_talkspurt_run(true);
}

/*
 * The end of a skip clears a pending clock-skew or trim run and restarts the
 * trim's clean window. It seeds the depth history with the depth after the
 * skip, so the trend reads no step.
 */
static void test_skip_reconcile(void)
{
	struct udsp_jb_stats st;
	unsigned i;

	CASE("jb_skip_end_clears_corrections_and_reseeds_depth");
	skip_steady(UDSP_JB_ADAPTIVE, 300);
	sim.jb.skew_pending = 1U;
	sim.jb.trim_pending = true;
	sim.jb.depth_avg_q8 = 400U << 8;
	skip_hold(3, true, false, NULL);
	sim_stats(&st);
	CHECK_UINT(sim.jb.skew_pending, 0);
	CHECK_UINT(sim.jb.trim_pending, 0);
	CHECK_UINT(sim.jb.trim_calm_ns, sim.now_ns);
	CHECK_UINT(sim.jb.depth_avg_q8, (uint32_t)st.depth_ms << 8);
	for (i = 0; i < UDSP_JB_TREND_BUCKETS; i++)
		CHECK_UINT(sim.jb.depth_hist_q8[i], sim.jb.depth_avg_q8);
}

/*
 * The timeline generation moves on the first packet and on every reset, and a
 * refill after an outage keeps it.
 */
static void test_skip_timeline(void)
{
	struct udsp_jb_out out;
	unsigned i;

	CASE("jb_timeline_moves_on_anchor_and_reset_only");
	sim_reset(UDSP_JB_ADAPTIVE, 20, 100, 10, 80);
	CHECK_UINT(udsp_jb_timeline(&sim.jb), 0);
	for (i = 0; i < 100U; i++) {
		sim_send(sim.now_ns);
		sim_tick(&out);
	}
	CHECK_UINT(udsp_jb_timeline(&sim.jb), 1);
	for (i = 0; i < 20U; i++) {
		sim_skip(1);
		sim_tick(&out);
	}
	for (i = 0; i < 50U; i++) {
		sim_send(sim.now_ns);
		sim_tick(&out);
	}
	CHECK_UINT(udsp_jb_timeline(&sim.jb), 1);
	sim.ssrc++;
	for (i = 0; i < 50U; i++) {
		sim_send(sim.now_ns);
		sim_tick(&out);
	}
	CHECK_UINT(udsp_jb_timeline(&sim.jb), 2);
}

/* One pass of test_skip_whole_packet. */
static void skip_whole_run(bool run)
{
	struct udsp_jb_out out;
	unsigned i;

	sim_reset_cfg(UDSP_JB_ADAPTIVE, 40, 200, 20, 120, 48, true);
	for (i = 0; i < 400U; i++) {
		if (!(i % 2U))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	for (i = 0; i < 3U; i++) {
		if (!(i % 2U))
			sim_send(sim.now_ns);
		sim.now_ns += TICK_NS;
	}
	sim.now_ns -= 3U * TICK_NS;
	skip_hold(3, false, run, NULL);
	for (i = 0; i < 400U; i++) {
		if (i % 2U)
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	CHECK_UINT(sim.unit_errors, 0);
	CHECK_UINT(sim.unit_repeats, 0);
	CHECK_UINT(sim.payload_errors, 0);
	CHECK_UINT(sim.order_errors, 0);
}

/*
 * A whole-packet stream. A run that ends inside a unit hands the next slice
 * out with the payload, so the caller decodes the unit fresh. Single
 * opportunities hand each unit out once.
 */
static void test_skip_whole_packet(void)
{
	CASE("jb_skip_get_hands_each_unit_out_once");
	skip_whole_run(false);
	CASE("jb_skip_run_inside_a_unit_hands_it_out_whole");
	skip_whole_run(true);
}

/* The states a run pays at once, against the same state paid one opportunity
 * at a time: the two must leave the same buffer behind. */
enum skip_state {
	SKIP_IDLE,
	SKIP_PREFILL,
	SKIP_PREFILL_OUTAGE,
	SKIP_PARKED,
	SKIP_RFC3389,
	SKIP_SID,
	SKIP_PLAY,
	SKIP_GAP,
	SKIP_GAP_CN,
	SKIP_SPARSE,
	SKIP_SID_GAP,
	SKIP_MARKED,
	SKIP_MARKED_OFFGRID,
	SKIP_PREFILL_GAP,
	SKIP_PREFILL_SPARSE,
	SKIP_PREFILL_RESERVE,
	SKIP_STATES,
};

/* A hold over a suppressed silence: the far end sends nothing for n ticks,
 * then one marked packet, and no get runs. No packet follows it, so no step
 * confirms it however far it stands. Past the queued audio, the marked head
 * stands n frames from the clock, past the 100 ms window for n of 11 or more.
 * offset moves it off the 10 ms grid. */
static void skip_marked_build(unsigned n, uint32_t offset)
{
	unsigned i;

	skip_steady(UDSP_JB_ADAPTIVE, 300);
	for (i = 0; i < n; i++) {
		sim_silence(1);
		sim.now_ns += TICK_NS;
	}
	sim.ts += offset;
	sim_send_marked(sim.now_ns);
	sim_deliver(sim.now_ns);
}

/* The same hold with twenty packets of audio after the marked one, one a
 * tick, on a line whose jitter estimate is high: the distance gate of the
 * step confirm (jb_confirm_far) then keeps the packets from a resync, and
 * they all stay queued. */
static void skip_marked_follow_build(unsigned n, uint32_t offset)
{
	unsigned i;

	skip_steady(UDSP_JB_ADAPTIVE, 300);
	sim.jb.jitter_q4 = 2000U << 4;
	for (i = 0; i < n; i++) {
		sim_silence(1);
		sim.now_ns += TICK_NS;
	}
	sim.ts += offset;
	sim_send_marked(sim.now_ns);
	sim.now_ns += TICK_NS;
	for (i = 0; i < 20U; i++) {
		sim_send(sim.now_ns);
		sim.now_ns += TICK_NS;
	}
	sim_deliver(sim.now_ns);
}

/* A prefill that holds more than its target: one packet with its get, then
 * the packets after it with no get, so the buffer stays in prefill. */
static void skip_prefill_build(void)
{
	struct udsp_jb_out out;

	sim_reset(UDSP_JB_ADAPTIVE, 50, 200, 10, 80);
	sim_send(sim.now_ns);
	sim_tick(&out);
}

static void skip_state_build(enum skip_state state)
{
	struct udsp_jb_out out;
	unsigned i;

	switch (state) {
	case SKIP_IDLE:
		sim_reset(UDSP_JB_ADAPTIVE, 50, 200, 10, 80);
		return;
	case SKIP_PREFILL:
		sim_reset(UDSP_JB_ADAPTIVE, 50, 200, 10, 80);
		sim_send(sim.now_ns);
		sim_tick(&out);
		return;
	case SKIP_PREFILL_OUTAGE:
		skip_steady(UDSP_JB_ADAPTIVE, 300);
		for (i = 0; i < 20U; i++) {
			sim_skip(1);
			sim_tick(&out);
		}
		sim_send(sim.now_ns);
		sim_deliver(sim.now_ns);
		return;
	case SKIP_PARKED:
		skip_steady(UDSP_JB_ADAPTIVE, 300);
		for (i = 0; i < 10U; i++) {
			sim_skip(1);
			sim_tick(&out);
		}
		return;
	case SKIP_RFC3389:
		skip_steady(UDSP_JB_ADAPTIVE, 300);
		cn_send();
		for (i = 0; i < 20U; i++) {
			sim_skip(1);
			sim_tick(&out);
		}
		return;
	case SKIP_SID:
		sim_reset_sid(UDSP_JB_ADAPTIVE, 60, 100, 20, SID_BPF, SID_LEN);
		for (i = 0; i < 300U; i++) {
			if (!(i % 2U))
				sim_send(sim.now_ns);
			sim_tick(&out);
		}
		sim_send_sid(sim.now_ns, 2);
		for (i = 0; i < 40U; i++)
			sim_tick(&out);
		return;
	case SKIP_GAP:
		/* Twenty packets lost, five queued behind the hole. */
		skip_steady(UDSP_JB_ADAPTIVE, 300);
		sim_skip(20);
		for (i = 0; i < 5U; i++)
			sim_send(sim.now_ns);
		sim_deliver(sim.now_ns);
		return;
	case SKIP_GAP_CN:
		/* Comfort noise, and speech queued 30 frames on. */
		skip_steady(UDSP_JB_ADAPTIVE, 300);
		cn_send();
		for (i = 0; i < 5U; i++) {
			sim_skip(1);
			sim_tick(&out);
		}
		sim_skip(30);
		sim_send(sim.now_ns);
		sim_deliver(sim.now_ns);
		return;
	case SKIP_SPARSE:
		/* Ten packets 2 s apart, no two of them consecutive. */
		skip_steady(UDSP_JB_ADAPTIVE, 300);
		for (i = 0; i < 10U; i++) {
			sim.ts += 16000U;
			sim.seq += 3U;
			sim_send(sim.now_ns);
		}
		sim_deliver(sim.now_ns);
		return;
	case SKIP_SID_GAP:
		/* SID comfort noise, and speech queued 50 frames on. */
		skip_state_build(SKIP_SID);
		sim_dtx_gap(50);
		sim_send(sim.now_ns);
		sim_deliver(sim.now_ns);
		return;
	case SKIP_MARKED:
		skip_marked_build(12, 0);
		return;
	case SKIP_MARKED_OFFGRID:
		skip_marked_build(12, 37);
		return;
	case SKIP_PREFILL_GAP:
		/* Five frames lost, then thirty queued: a surplus with a
		 * gap in it. */
		skip_prefill_build();
		sim_skip(5);
		for (i = 0; i < 30U; i++)
			sim_send(sim.now_ns);
		sim_deliver(sim.now_ns);
		return;
	case SKIP_PREFILL_RESERVE:
		/* Four frames lost, then two queued: 70 ms against a target
		 * of 50, so the prefill waits halfway through the gap. */
		skip_prefill_build();
		sim_skip(4);
		for (i = 0; i < 2U; i++)
			sim_send(sim.now_ns);
		sim_deliver(sim.now_ns);
		return;
	case SKIP_PREFILL_SPARSE:
		/* Ten packets 2 s apart, in prefill. */
		skip_prefill_build();
		for (i = 0; i < 10U; i++) {
			sim.ts += 16000U;
			sim.seq += 3U;
			sim_send(sim.now_ns);
		}
		sim_deliver(sim.now_ns);
		return;
	default:
		skip_steady(UDSP_JB_ADAPTIVE, 300);
		return;
	}
}

static void test_skip_run_equals_gets(void)
{
	static const char *const names[SKIP_STATES] = {
		"idle", "prefill", "prefill after an outage", "parked",
		"rfc3389", "sid", "play", "gap", "gap under comfort noise",
		"sparse", "gap under sid comfort noise", "marked head",
		"marked head off the grid", "prefill with a gap",
		"sparse prefill", "prefill that waits inside a gap",
	};
	static const uint32_t units[3] = { 7, 400, 2000 };
	struct udsp_jb_stats sa, sb;
	struct udsp_jb_out out;
	static struct udsp_jb a;

	CASE("jb_skip_run_leaves_the_buffer_one_get_at_a_time_leaves");
	for (unsigned k = 0; k < SKIP_STATES; k++) {
		for (unsigned u = 0; u < 3u; u++) {
			unsigned bad = 0;

			skip_state_build((enum skip_state)k);
			sim.now_ns += 5U * TICK_NS;
			a = sim.jb;
			udsp_jb_skip(&a, units[u], sim.now_ns);
			for (uint32_t i = 0; i < units[u]; i++)
				udsp_jb_skip_get(&sim.jb, sim.now_ns, &out);
			sim.jb.emit_unit_valid = false;
			udsp_jb_stats(&a, &sa);
			udsp_jb_stats(&sim.jb, &sb);
			bad += a.playout_ts != sim.jb.playout_ts;
			bad += a.state != sim.jb.state;
			bad += a.count != sim.jb.count;
			bad += a.conceal_run_ms != sim.jb.conceal_run_ms;
			bad += a.cn_run_ms != sim.jb.cn_run_ms;
			bad += a.cn_len != sim.jb.cn_len;
			bad += a.skip_units != sim.jb.skip_units;
			bad += a.skip_waits != sim.jb.skip_waits;
			bad += a.skip_frames != sim.jb.skip_frames;
			bad += a.shrink_ns != sim.jb.shrink_ns;
			bad += a.trim_calm_ns != sim.jb.trim_calm_ns;
			/* The loan is the caller's until its next get, and a
			 * run hands nothing out. */
			sa.pool_free = sb.pool_free;
			bad += memcmp(&sa, &sb, sizeof(sa)) != 0;
			if (bad)
				printf("# skip %s, %u units: %u fields differ "
				       "(playout %u/%u units %u/%u waits "
				       "%u/%u)\n", names[k], units[u], bad,
				       a.playout_ts, sim.jb.playout_ts,
				       a.skip_units, sim.jb.skip_units,
				       a.skip_waits, sim.jb.skip_waits);
			CHECK_UINT(bad, 0);
			CHECK_UINT(sim.jb.skip_units + sim.jb.skip_waits,
				   units[u]);
		}
	}

	CASE("jb_skip_run_of_a_million_parks_at_once");
	skip_state_build(SKIP_PARKED);
	udsp_jb_skip(&sim.jb, 1000000U, sim.now_ns);
	CHECK_UINT(sim.jb.skip_waits, 1000000U);
	CHECK_UINT(sim.jb.conceal_run_ms, 60000U);
}

/*
 * A sparse queue: sixty packets nine seconds apart. Each one stays under the
 * timestamp jump bound, and none is a successor, so no resync and no step
 * takes them. A run of 54001 opportunities walks the gaps between them at
 * once. Its single steps stay bounded by the queued slices and the gaps, and
 * it leaves the buffer that the single opportunities leave.
 */
static void test_skip_sparse_bound(void)
{
	struct udsp_jb_out out;
	static struct udsp_jb a;
	uint32_t steps;
	unsigned i;

	CASE("jb_skip_run_over_a_sparse_queue_walks_its_gaps_at_once");
	skip_steady(UDSP_JB_ADAPTIVE, 300);
	for (i = 0; i < 60U; i++) {
		sim.ts += 72000U;
		sim.seq += 4U;
		sim_send(sim.now_ns);
	}
	sim_deliver(sim.now_ns);
	printf("# skip sparse: %u slots queued, %llu resyncs\n", sim.jb.count,
	       (unsigned long long)sim.jb.total.resyncs);
	CHECK(sim.jb.count >= 60U);
	CHECK_UINT(sim.jb.total.resyncs, 0);
	a = sim.jb;
	steps = a.skip_steps;
	udsp_jb_skip(&a, 54001U, sim.now_ns);
	for (i = 0; i < 54001U; i++)
		udsp_jb_skip_get(&sim.jb, sim.now_ns, &out);
	printf("# skip sparse: %u single steps for 54001 units\n",
	       a.skip_steps - steps);
	CHECK(a.skip_steps - steps <= 2U * UDSP_JB_SLOTS);
	CHECK_UINT(a.playout_ts, sim.jb.playout_ts);
	CHECK_UINT(a.skip_units, sim.jb.skip_units);
	CHECK_UINT(a.skip_waits, sim.jb.skip_waits);
	CHECK_UINT(a.skip_frames, sim.jb.skip_frames);
	CHECK_UINT(a.conceal_run_ms, sim.jb.conceal_run_ms);
	CHECK_UINT(a.count, sim.jb.count);
}

/*
 * The same sparse queue in prefill: no get ran, so the buffer never left it,
 * and its surplus is the whole queue. The run walks its gaps at once too, and
 * stops where the prefill starts to wait.
 */
static void test_skip_prefill_sparse_bound(void)
{
	struct udsp_jb_out out;
	static struct udsp_jb a;
	uint32_t steps;
	unsigned i;

	CASE("jb_skip_run_over_a_sparse_prefill_walks_its_gaps_at_once");
	skip_prefill_build();
	for (i = 0; i < 60U; i++) {
		sim.ts += 72000U;
		sim.seq += 4U;
		sim_send(sim.now_ns);
	}
	sim_deliver(sim.now_ns);
	CHECK(sim.jb.count >= 60U);
	CHECK_UINT(sim.jb.total.resyncs, 0);
	CHECK_UINT(sim.jb.state, 1);
	a = sim.jb;
	steps = a.skip_steps;
	udsp_jb_skip(&a, 54001U, sim.now_ns);
	for (i = 0; i < 54001U; i++)
		udsp_jb_skip_get(&sim.jb, sim.now_ns, &out);
	printf("# skip sparse prefill: %u single steps for 54001 units, %u "
	       "waits\n", a.skip_steps - steps, a.skip_waits);
	CHECK(a.skip_steps - steps <= 2U * UDSP_JB_SLOTS);
	CHECK_UINT(a.state, sim.jb.state);
	CHECK_UINT(a.playout_ts, sim.jb.playout_ts);
	CHECK_UINT(a.skip_units, sim.jb.skip_units);
	CHECK_UINT(a.skip_waits, sim.jb.skip_waits);
	CHECK_UINT(a.skip_frames, sim.jb.skip_frames);
	CHECK_UINT(a.conceal_run_ms, sim.jb.conceal_run_ms);
	CHECK_UINT(a.count, sim.jb.count);
}

/*
 * A marked head past the talkspurt window, on and off the 10 ms grid, alone
 * or with twenty packets after it. One get at a time conceals until the head
 * enters the window, then steps over the rest of the gap and plays it. A run
 * must stop at that opportunity and take the step there. Then a run of any
 * length, and a run followed by single opportunities, leave the buffer that
 * single opportunities leave. That split is the one of stream_rx_skip_units:
 * a run, then up to ten decoded units.
 */
static void test_skip_marked_split(void)
{
	static const uint32_t offsets[2] = { 0, 37 };
	static struct udsp_jb a, b;
	struct udsp_jb_out out;
	unsigned bad = 0, runs = 0, resyncs = 0;

	CASE("jb_skip_run_stops_where_a_marked_head_becomes_due");
	for (unsigned v = 0; v < 4U; v++) {
		for (unsigned n = 6; n <= 18U; n++) {
			for (unsigned total = 1; total <= 20U; total++) {
				for (unsigned k = 0; k <= total; k++) {
					unsigned o = v & 1U;

					if (v < 2U)
						skip_marked_build(n,
								  offsets[o]);
					else
						skip_marked_follow_build(
							n, offsets[o]);
					resyncs += (unsigned)
						   sim.jb.total.resyncs;
					a = sim.jb;
					b = sim.jb;
					udsp_jb_skip(&a, k, sim.now_ns);
					for (uint32_t i = k; i < total; i++)
						udsp_jb_skip_get(&a, sim.now_ns,
								 &out);
					for (uint32_t i = 0; i < total; i++)
						udsp_jb_skip_get(&b, sim.now_ns,
								 &out);
					runs++;
					if (a.playout_ts == b.playout_ts &&
					    a.count == b.count &&
					    a.skip_units == b.skip_units &&
					    a.skip_frames == b.skip_frames &&
					    a.conceal_run_ms ==
					    b.conceal_run_ms)
						continue;
					if (!bad++)
						printf("# marked split: gap %u "
						       "offset %u, %u units, run "
						       "%u: playout %u/%u "
						       "queued %u/%u\n", n,
						       offsets[o], total, k,
						       a.playout_ts,
						       b.playout_ts, a.count,
						       b.count);
				}
			}
		}
	}
	printf("# marked split: %u splits, %u differ, %u resyncs\n", runs,
	       bad, resyncs);
	CHECK_UINT(bad, 0);
	CHECK_UINT(resyncs, 0);
}

/*
 * A pending skew insert, cleared by the end of the skip: the next real get
 * plays a frame and inserts nothing.
 */
static void test_skip_pending_insert(void)
{
	struct udsp_jb_out out;

	CASE("jb_skip_end_clears_a_pending_insert_for_the_next_get");
	skip_steady(UDSP_JB_ADAPTIVE, 300);
	sim.jb.skew_pending = 2U;
	skip_hold(3, true, false, NULL);
	CHECK_UINT(sim.jb.skew_pending, 0);
	sim_send(sim.now_ns);
	sim_tick(&out);
	CHECK(!out.skew_insert);
	CHECK_UINT(out.decision, UDSP_JB_FRAME);
	CHECK_UINT(sim.order_errors, 0);
}

/*
 * The SID of a run. A run that passes the SID slice of a talkspurt close keeps
 * that SID for a caller that restarts its decoder (udsp_jb_skip_sid). A run
 * inside comfort noise whose SID went out before it passes no audio and no
 * SID. So the caller keeps its decoder as it stands.
 */
static void test_skip_sid(void)
{
	const uint8_t *sid = NULL;
	struct udsp_jb_out out;
	uint32_t frames;
	size_t len;
	unsigned i;

	CASE("jb_skip_run_keeps_the_sid_it_passes");
	sim_reset_sid(UDSP_JB_ADAPTIVE, 60, 100, 20, SID_BPF, SID_LEN);
	for (i = 0; i < 300U; i++) {
		if (!(i % 2U))
			sim_send(sim.now_ns);
		sim_tick(&out);
	}
	sim_send_sid(sim.now_ns, 2);
	sim_deliver(sim.now_ns);
	frames = udsp_jb_skip(&sim.jb, 40U, sim.now_ns);
	len = udsp_jb_skip_sid(&sim.jb, &sid);
	CHECK(frames > 0u);
	CHECK_UINT(len, SID_LEN);
	CHECK(sim.jb.cn_armed && sim.jb.cn_sid);

	CASE("jb_skip_run_of_comfort_noise_passes_no_sid");
	for (i = 0; i < 20U; i++)
		sim_tick(&out);
	CHECK_UINT(out.decision, UDSP_JB_CN);
	frames = udsp_jb_skip(&sim.jb, 40U, sim.now_ns);
	CHECK_UINT(frames, 0);
	CHECK_UINT(udsp_jb_skip_sid(&sim.jb, &sid), 0);
}

/*
 * Two deliveries of one hold, as a drain that drops a read at a time hands
 * them over. Arrivals and a skip, more arrivals and a skip, then the tick:
 * the buffer ends where one delivery leaves it.
 */
static void test_skip_deliveries(void)
{
	struct udsp_jb_stats st0, st1;
	struct udsp_jb_out out;
	uint32_t playout0;
	unsigned i;

	CASE("jb_skip_two_deliveries_of_one_hold_pay_it_whole");
	skip_steady(UDSP_JB_ADAPTIVE, 300);
	sim_stats(&st0);
	playout0 = sim.jb.playout_ts;
	skip_hold(4, true, false, NULL);
	skip_hold(2, true, true, NULL);
	sim_send(sim.now_ns);
	sim_tick(&out);
	sim_stats(&st1);
	CHECK_UINT(sim.jb.playout_ts - playout0, 7U * UDSP_JB_FRAME_TS);
	CHECK_UINT(sim.jb.skip_units, 6);
	CHECK_UINT(st1.depth_ms, st0.depth_ms);
	CHECK_UINT(skip_out_total(&st1), skip_out_total(&st0) + 1U);
	for (i = 0; i < 100U; i++) {
		sim_send(sim.now_ns);
		sim_tick(&out);
	}
	CHECK_UINT(sim.order_errors, 0);
	CHECK_UINT(sim.payload_errors, 0);
}

static void test_skip_off(void)
{
	uint32_t playout0;

	CASE("jb_skip_off_mode_passes_units");
	skip_steady(UDSP_JB_OFF, 300);
	playout0 = sim.jb.playout_ts;
	skip_hold(4, true, false, NULL);
	CHECK_UINT(sim.jb.playout_ts - playout0, 4U * UDSP_JB_FRAME_TS);
	CHECK_UINT(sim.jb.skip_frames, 4);
}

int main(void)
{
	test_steady_20ms();
	test_adaptive_jitter();
	test_bundled_datagram_no_jitter();
	test_bundled_datagram_untagged_inflates();
	test_fixed_holds_target();
	test_mode_set_fixed_pins();
	test_mode_set_keeps_queue();
	test_mode_set_cancels_trim();
	test_off_no_added_delay();
	test_off_drops_late();
	test_off_starved_late();
	test_reorder();
	test_duplicate_and_late();
	test_burst_loss();
	test_plc_limit();
	test_cn_gap();
	test_skew();
	test_resync_ssrc();
	test_resync_ts_jump();
	test_wrap();
	test_timestamp_half_range();
	test_shrink_rate();
	test_slice_low_rate();
	test_span_loss();
	test_span_ptime_change();
	test_span_first_packet();
	test_span_single_odd_packet();
	test_span_frame_units();
	test_span_reorder_swap();
	test_span_reorder_startup();
	test_span_cn_reorder();
	test_span_gap2_20ms();
	test_span_gap2_10ms();
	test_span_cn_silence_shield();
	test_span_cn_late_first_learn();
	test_span_off_early_pair();
	test_span_step_reseed();
	test_cn_resume_behind();
	test_payload_loan();
	test_overflow_and_validation();
	test_starvation_recovery();
	test_skew_fast_leak();
	test_stall_burst();
	test_forward_outlier();
	test_periodic_outliers();
	test_forward_step();
	test_prefill_step();
	test_prefill_step_same_tick();
	test_prefill_stray_no_resync();
	test_prefill_step_wire_shape();
	test_jitter_skips_marked_step();
	test_jitter_counts_unmarked_step();
	test_jitter_counts_marked_delay();
	test_jitter_marked_step_keeps_reference();
	test_jitter_marked_bound_edges();
	test_jitter_marker_every_packet();
	test_jitter_counts_marked_cn();
	test_cn_frozen_resume();
	test_backward_step();
	test_prefill_straggler();
	test_early_packet_no_livelock();
	test_step_first_packet_lost();
	test_step_swapped_start();
	test_burst_misstamp_no_resync();
	test_burst_reorder_no_resync();
	test_off_step_burst_no_resync();
	test_step_dup_confirm_rejected();
	test_cn_straggler_stays_late();
	test_gate_reach_boundary();
	test_junk_cn_no_arm();
	test_refill_clears_candidate();
	test_outlier_pair_no_resync();
	test_paced_stall_release();
	test_offset_trim_clean();
	test_offset_trim_floor();
	test_offset_trim_jittery();
	test_offset_trim_refill();
	test_offset_trim_sustain();
	test_junk_cn_no_arm_fixed();
	test_offset_trim_fixed_excluded();
	test_offset_trim_clamped();
	test_offset_trim_conceal_resets_hold();
	test_offset_trim_threshold_margin();
	test_park_refill_insert();
	test_floor_margin();
	test_floor_ptime30();
	test_floor_relearn();
	test_hold_far_single();
	test_hold_jitter_distrust();
	test_hold_far_resume();
	test_hold_poisoned_frontier();
	test_hold_heavy_loss_resume();
	test_hold_cn_mid_hold();
	test_hold_unlearned_span();
	test_hold_swapped_resume();
	test_hold_fixed_far_resume();
	test_resync_clears_candidate();
	test_hold_cn_poisoned_distrust();
	test_hold_cn_bridged_parks();
	test_hold_junk_cn_suppression();
	test_hold_fixed_junk_spike();
	test_hold_far_boundary();
	test_gate_churn();
	test_gate_step_parity();
	test_gate_boundary();
	test_gate_stall_echo();
	test_gate_parked_block();
	test_gate_fixed_churn();
	test_wb_whole_packet_units();
	test_wb_span_bounds();
	test_wb_resync_bound();
	test_wb_payload_bound();
	test_wb_hold_refill();
	test_wb_unit_skew_insert();
	test_wb_unit_reanchor();
	test_talkspurt_skips_queued_gap();
	test_talkspurt_gap_without_marker_conceals();
	test_talkspurt_marker_needs_a_gap();
	test_talkspurt_marker_beyond_window_ignored();
	test_talkspurt_marker_fixed_conceals();
	test_talkspurt_marker_parked_clock_still_counts();
	test_talkspurt_marker_teaches_no_span();
	test_jb2_floor_holds_one_span();
	test_jb2_skew_arms_hand_off();
	test_sid_slicing();
	test_sid_only_and_partial();
	test_sid_first_packet();
	test_sid_gap_plays_cn();
	test_sid_gap_without_sid_plcs();
	test_sid_resume_no_marker();
	test_sid_long_silence_resume();
	test_sid_cng_bound_frozen_resume();
	test_pt13_cn_unbounded();
	test_sid_refresh_survives_jump_and_starve();
	test_sid_span_step_held_ptime30();
	test_sid_restamp_skips_sid_slot();
	test_sid_timing_reset_clears();
	test_sid_exempt_distance_bound();
	test_sid_exempt_needs_cn_sid();
	test_sid_units_cap();
	test_sid_late_refresh_no_rewind();
	test_sid_pt13_stays_rfc3389();
	test_sid_pt13_after_sid_clears();
	test_sid_learner_immune_both_directions();
	test_sid_zero_len_audio_passthrough();
	test_sid_len0_regression();
	test_skip_play();
	test_skip_prefill();
	test_skip_dry();
	test_skip_talkspurt();
	test_skip_reconcile();
	test_skip_timeline();
	test_skip_whole_packet();
	test_skip_run_equals_gets();
	test_skip_off();
	test_skip_sparse_bound();
	test_skip_prefill_sparse_bound();
	test_skip_marked_split();
	test_skip_pending_insert();
	test_skip_sid();
	test_skip_deliveries();
	return tu_summary("jb");
}
