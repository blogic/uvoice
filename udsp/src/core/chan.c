// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The per-line half of the engine tick (line.h section 2, Tick order). It runs
 * inside the engine's injection hook, between the capture read and the
 * playback write, and it is the only writer of the line machines, the
 * generators and the SLIC command ring.
 *
 * Order per line. Control commands first (they apply at the tick boundary),
 * then UL (gain, the offset removal, DTMF detection, the media hook), then the
 * line machines in the mandatory four-call order, then the journal, then DL
 * (the media hook, the loop, the voice gain, then the active playout with its
 * trim) into the playback slot.
 *
 * The four-call order is what puts UDSP_EV_RING_END before the UDSP_EV_HOOK
 * seize of a ring trip (line.h section 2, the answer race); it runs once per
 * drained hook edge with that edge's own stamp and once more with the tick's
 * stamp. The machines are stamp-driven, so a replayed call cannot
 * double-advance a timer.
 *
 * Nothing on the tick path logs, allocates or takes a mutex. The one exception
 * in this file is the loop ring pool, which udsp_open takes and udsp_close
 * releases, both on the control thread.
 */
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "udsp_priv.h"
#include "stream.h"

/* LINES_UP emits two records per line (linefeed and gain) and UDSP_LINE_MAX
 * is 8, which is 16 exactly; 24 keeps headroom for one more per-line record
 * class. */
#define CHAN_BATCH_MAX	24u

struct chan_batch {
	struct slic_rec rec[CHAN_BATCH_MAX];
	unsigned count;
	bool overflow;
};

/*
 * The tone disabler's release windows, counted in the 10 ms frames the
 * detectors read. The modem window is the 250 ms of G.168 7.8, inside its
 * 250 plus or minus 150 ms and above the 100 ms dropout the same clause asks
 * the disabler to sit through.
 *
 * The fax window is a deliberate extension of it. T.30 leaves multi-second
 * silences between commands (T2 is about 6 s), so a 250 ms release would drop
 * the policy in the middle of a handshake and let the non-linear processor eat
 * the next command. Seven seconds outlasts T2.
 */
#define UDSP_FAX_BYPASS_RELEASE_FRAMES		25u
#define UDSP_FAX_BYPASS_FAX_RELEASE_FRAMES	700u
/*
 * How long the governing stream count must stand moved before the topology
 * guard drops the latch. A reconfiguration quiesces a running stream for the
 * tick or two the control thread needs (stream.c stream_quiesce), and a guard
 * that cleared on the first moved frame would drop the bypass under every
 * mid-call codec or jitter update; a leg that held, stopped or joined moves
 * the count for good and passes this window without noticing it.
 */
#define UDSP_FAX_TOPO_RELEASE_FRAMES		5u
/*
 * The level the disabler holds on, one threshold between the release point of
 * G.168 7.4 (at or below -36 dBm0) and its hold point (at or above -31 dBm0
 * for 700 to 3000 Hz). The basis is the detector's own TONE_MIN_ENERGY
 * (dsp/fax_det.c), 1.0e6 for the -43 dBm0 of a frame; ten times that energy is
 * ten decibels up, which is -33 dBm0. The measurement it is compared against
 * is a high-passed frame energy and not the per-band sensitivities of the
 * recommendation (dsp/fax_det.c hold_energy_update).
 */
#define UDSP_FAX_HOLD_ENERGY			1.0e7

/* Fault handling: these sources take the feed open and are retried on a
 * timer, unless the read also sets a hold (chan_fault_held_reason). LONGSTAT
 * joins for symmetry; the silicon does not raise it. */
#define CHAN_FAULT_OPEN_SET	(UDSP_FAULT_SLIC_THERMAL | UDSP_FAULT_SLIC_PQ | \
				 UDSP_FAULT_SLIC_VBAT | \
				 UDSP_FAULT_SLIC_LONGSTAT | \
				 UDSP_FAULT_SLIC_REINIT | \
				 UDSP_FAULT_SLIC_DCDC | \
				 UDSP_FAULT_SLIC_CLOCK | \
				 UDSP_FAULT_DEVICE_RESET)

static void chan_event_post(struct udsp *s, const struct udsp_event *ev)
{
	udsp_emit(&s->emitter, UDSP_EV_PROD_ENGINE, ev);
}

void frame_add_sat(int16_t *dst, const int16_t *src, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		int32_t v = (int32_t)dst[i] + src[i];

		if (v > 32767)
			v = 32767;
		else if (v < -32768)
			v = -32768;
		dst[i] = (int16_t)v;
	}
}

/* --- media chain --------------------------------------------------------- */

static void chan_dl_history_add(struct udsp_chan *c, const int16_t *frame)
{
	unsigned w = c->hist_w;

	for (unsigned i = 0; i < c->frame; i++) {
		c->hist[w] = frame[i];
		w = (w + 1u) % UDSP_LEC_HIST_SAMPLES;
	}
	c->hist_w = w;
	atomic_store_explicit(&c->hist_frames,
			      atomic_load_explicit(&c->hist_frames,
						   memory_order_relaxed) + 1u,
			      memory_order_relaxed);
}

/* UL runs before this tick's DL frame enters the history, so a delay under
 * one frame would read part of the reference from the frame's own future in
 * the ring, which is a whole ring stale there; the high side keeps the read
 * inside the UDSP_LEC_HIST_MS the rate affords, which is the same reach in
 * time at every rate. The ring is sized for the widest rate, so its length is
 * not the ceiling: taking it would move the 8 kHz bound. The tracker compares
 * the measured value before this clamp, so a move the bound would swallow is
 * still a move of the reference. */
static unsigned chan_lec_delay_clamp(unsigned delay, unsigned rate,
				     unsigned frame)
{
	unsigned reach = rate * UDSP_LEC_HIST_MS / 1000u - frame;

	if (delay < frame)
		return frame;
	if (delay > reach)
		return reach;
	return delay;
}

/* The line rate is one per handle, so the per-udsp delay tracker clamps
 * against the settled rate the handle carries and gets the same answer as
 * every per-channel consumer below. Reading chan[0] would be wrong: line 0 can
 * be dead or absent, and the rate is a property of the handle, not of a
 * line. */
static unsigned chan_lec_rate_get(const struct udsp *s)
{
	return s->rate;
}

static unsigned chan_lec_frame_get(const struct udsp *s)
{
	return s->frame;
}

/* The reference is the DL frame the line played delay samples ago, which is
 * what the hybrid returns as echo (dsp/lec.h). The answer is the delay the
 * take applied, for the capture record. */
static unsigned chan_lec_ref_take(struct udsp *s, struct udsp_chan *c)
{
	unsigned delay = (unsigned)atomic_load_explicit(&s->lec_delay[c->line],
							memory_order_relaxed);
	unsigned pos;

	delay = chan_lec_delay_clamp(delay, c->rate, c->frame);
	pos = (c->hist_w + UDSP_LEC_HIST_SAMPLES - delay) % UDSP_LEC_HIST_SAMPLES;
	for (unsigned i = 0; i < c->frame; i++) {
		c->ref[i] = c->hist[pos];
		pos = (pos + 1u) % UDSP_LEC_HIST_SAMPLES;
	}

	return delay;
}

/*
 * The two fixed-point conversions of struct udsp_lec_stats. They saturate
 * instead of wrapping, and a value that is not a number reads as the zero a
 * fresh instance carries. The record is a diagnostic, and a wrapped counter
 * would read as a measurement.
 */
static int32_t chan_lec_mdb(float db)
{
	double x = (double)db * 1000.0;

	if (isnan(x))
		return 0;
	if (x <= (double)INT32_MIN)
		return INT32_MIN;
	if (x >= (double)INT32_MAX)
		return INT32_MAX;

	return (int32_t)x;
}

static uint32_t chan_lec_micro(float v)
{
	double x = (double)v * 1000000.0;

	/* A value that is not a number fails this comparison, and so do the
	 * negative values these two quantities never take. */
	if (!(x > 0.0))
		return 0;
	if (x >= (double)UINT32_MAX)
		return UINT32_MAX;

	return (uint32_t)x;
}

/* The floor of the capture record in tenths of dB, saturated as the two
 * above. A value that is not a number reads as the unmeasured floor. */
static uint16_t chan_lec_ddbov(float dbov)
{
	double x = (double)dbov * 10.0;

	if (isnan(x))
		return UDSP_LEC_CAPTURE_NOISE_UNMEASURED;
	if (x <= 0.0)
		return 0;
	if (x >= (double)UINT16_MAX)
		return UINT16_MAX;

	return (uint16_t)lrint(x);
}

static uint16_t chan_lec_u16(uint64_t v)
{
	return v > UINT16_MAX ? UINT16_MAX : (uint16_t)v;
}

/*
 * The canceller capture of udsp.h. At rest a frame pays one relaxed load of
 * the mask here. A claim that finds the ring full counts the record in
 * lec_cap_lost and writes nothing. The next record of the line that fits then
 * carries the loss.
 */
static struct udsp_lec_capture *lec_capture_claim(struct udsp *s,
						  struct udsp_chan *c)
{
	struct lec_capture_ring *r;
	struct udsp_lec_capture *rec;

	if (!(atomic_load_explicit(&s->debug, memory_order_relaxed) &
	      UDSP_DEBUG_LEC_CAPTURE))
		return NULL;
	r = atomic_load_explicit(&s->lec_cap, memory_order_acquire);
	if (!r)
		return NULL;
	rec = lec_capture_ring_claim(r);
	if (!rec)
		c->lec_cap_lost++;

	return rec;
}

/* The header of a record: the tick, the line and the state of the instance
 * after the frame. Only a RUN reads the verdicts, because the frame reading
 * of dsp/lec.h still describes the last RUN on a FEED or an END. */
static void lec_capture_head_fill(struct udsp *s, struct udsp_chan *c,
				  const struct udsp_ec *ec,
				  struct udsp_lec_capture *rec, uint8_t kind,
				  uint16_t flags, unsigned delay)
{
	struct udsp_ec_nlms_frame f;
	struct udsp_ec_nlms_info info;

	udsp_ec_nlms_info_get(ec, &info);
	rec->stamp_ns = s->tick_ns;
	rec->tick = s->loop_tick;
	rec->frames = info.frames;
	rec->hold_frames = info.hold_frames;
	rec->erle_mdb = chan_lec_mdb(info.erle_db);
	rec->echo_gain_micro = chan_lec_micro(info.echo_gain);
	rec->coef_energy_micro = chan_lec_micro(info.coef_energy);
	rec->ref_delay_samples = delay;
	rec->gen = c->lec_trace_gen;
	rec->samples = (uint16_t)c->frame;
	rec->tail_ms = (uint16_t)info.tail_ms;
	rec->noise_ddbov = chan_lec_ddbov(info.noise_dbov);
	rec->probes = chan_lec_u16(info.probes);
	rec->diverge_resets = chan_lec_u16(info.diverge_resets);
	rec->line = (uint8_t)c->line;
	rec->kind = kind;
	rec->nlp_verdict = UDSP_LEC_CAPTURE_VERDICT_OFF;
	rec->hold_left = 0;
	if (kind == UDSP_LEC_CAPTURE_RUN) {
		udsp_ec_nlms_frame_get(ec, &f);
		rec->nlp_verdict = f.nlp_verdict;
		rec->hold_left = f.hold_left;
		flags |= (f.dt_fired ? UDSP_LEC_CAPTURE_FIRED : 0u) |
			 (info.double_talk ? UDSP_LEC_CAPTURE_DT : 0u) |
			 (info.nlp_active ? UDSP_LEC_CAPTURE_ACTIVE : 0u) |
			 (f.probing ? UDSP_LEC_CAPTURE_PROBING : 0u);
	}
	rec->flags = flags;
}

/*
 * The counts are taken after the record is written and before the head
 * moves. The record therefore carries exactly the losses of its line before
 * it. The ring pointer stands from the arm to udsp_release, and the claim
 * loaded it with acquire in this tick.
 *
 * skipped takes every period that the engine skipped since the previous
 * record of the line, whatever the jump. The reader compares the jump with
 * it, so a jump that also covers a time with the bit clear reads as a gap. A
 * NEW record starts a lineage and an END closes one, so neither carries a
 * count.
 */
static void lec_capture_publish(struct udsp *s, struct udsp_chan *c,
				struct udsp_lec_capture *rec)
{
	uint64_t skipped = s->lec_skip_total - c->lec_cap_skips;

	rec->lost_before = c->lec_cap_lost;
	rec->skipped = 0;
	if (rec->kind != UDSP_LEC_CAPTURE_END &&
	    !(rec->flags & UDSP_LEC_CAPTURE_NEW))
		rec->skipped = chan_lec_u16(skipped);
	if (rec->skipped)
		rec->flags |= UDSP_LEC_CAPTURE_SKIPPED;
	c->lec_cap_lost = 0;
	c->lec_cap_skips = s->lec_skip_total;
	lec_capture_ring_publish(atomic_load_explicit(&s->lec_cap,
						      memory_order_relaxed));
}

static void lec_capture_feed(struct udsp *s, struct udsp_chan *c,
			     const struct udsp_ec *ec, const int16_t *frame,
			     unsigned delay, uint16_t flags)
{
	struct udsp_lec_capture *rec = lec_capture_claim(s, c);
	size_t bytes = c->frame * sizeof(*frame);

	if (!rec)
		return;
	memcpy(rec->ref, c->ref, bytes);
	memcpy(rec->mic, frame, bytes);
	lec_capture_head_fill(s, c, ec, rec, UDSP_LEC_CAPTURE_FEED, flags,
			      delay);
	lec_capture_publish(s, c, rec);
}

/* The run writes its output over the frame, so mic is copied before it. */
static struct udsp_lec_capture *lec_capture_begin(struct udsp *s,
						  struct udsp_chan *c,
						  const int16_t *frame)
{
	struct udsp_lec_capture *rec = lec_capture_claim(s, c);

	if (rec)
		memcpy(rec->mic, frame, c->frame * sizeof(*frame));

	return rec;
}

static void lec_capture_end(struct udsp *s, struct udsp_chan *c,
			    const struct udsp_ec *ec,
			    struct udsp_lec_capture *rec, const int16_t *frame,
			    unsigned delay, uint16_t flags)
{
	size_t bytes = c->frame * sizeof(*frame);

	memcpy(rec->ref, c->ref, bytes);
	memcpy(rec->out, frame, bytes);
	lec_capture_head_fill(s, c, ec, rec, UDSP_LEC_CAPTURE_RUN, flags,
			      delay);
	lec_capture_publish(s, c, rec);
}

/* The tracker is per udsp, and there is one live udsp per process
 * (udsp_priv.h). File scope therefore holds it (chan.h struct
 * chan_lec_track). */
static struct chan_lec_track lec_track;

/* The published value is the applied one, one period under the loop the line
 * stands on. It is never the raw measurement, so
 * udsp_lec_stats.ref_delay_samples reads what the taps are aligned to.
 * chan_lec_ref_take clamps again, which is then a no-op. */
static void chan_lec_line_apply(struct udsp *s, unsigned line)
{
	uint32_t delay = 0;

	if (chan_lec_line_decide(&lec_track, line, chan_lec_rate_get(s),
				 chan_lec_frame_get(s), &delay) ==
	    CHAN_LEC_HOLD) {
		atomic_store_explicit(&s->lec_ready[line], false,
				      memory_order_relaxed);
		return;
	}
	atomic_store_explicit(&s->lec_delay[line], delay, memory_order_relaxed);
	atomic_store_explicit(&s->lec_ready[line], true, memory_order_relaxed);
}

/*
 * The first frame the tick hands an instance, on any path: a RUN, a skip
 * inside chan_lec_run or the feed of an inactive stream. The frame count of
 * the instance starts over, so the generation of the traces moves here and
 * not at the first RUN. The FEED records of a new instance then carry the
 * generation of its first RUN. The tracker runs whatever the mask says, so
 * NEW is exact for a capture that starts between two calls.
 *
 * Every instance leaves through chan_lec_tracker_drop, which clears the line.
 * A new instance therefore takes the offer of this tick verbatim, and the
 * record of its first frame carries it. Zero taps have no alignment to
 * protect.
 */
static bool chan_lec_instance_take(struct udsp *s, struct udsp_chan *c,
				   const struct udsp_ec *ec)
{
	bool fresh = c->lec_cap_for != ec;

	c->lec_cap_for = ec;
	if (!fresh)
		return false;
	c->lec_trace_gen++;
	chan_lec_line_apply(s, c->line);

	return true;
}

/* A frame that skips the filter still enters the history of the instance.
 * Without the feed, the history stops at the skip, and the first tail of
 * frames after the skip adapts against a stale window (dsp/lec.h
 * udsp_ec_nlms_ref_feed). The replay of a capture must take the same frame,
 * so every feed goes through here and pushes its FEED record. */
static void chan_lec_ref_feed(struct udsp *s, struct udsp_chan *c,
			      struct udsp_ec *ec, const int16_t *frame,
			      uint16_t reason)
{
	uint16_t flags = reason;
	unsigned delay;

	if (chan_lec_instance_take(s, c, ec))
		flags |= UDSP_LEC_CAPTURE_NEW;
	delay = chan_lec_ref_take(s, c);
	udsp_ec_nlms_ref_feed(ec, c->ref);
	lec_capture_feed(s, c, ec, frame, delay, flags);
}

static enum udsp_passthrough chan_fax_latch_get(const struct udsp_chan *c)
{
	return (enum udsp_passthrough)
		atomic_load_explicit(&c->fax_bypass_kind, memory_order_relaxed);
}

void chan_fax_latch_clear(struct udsp_chan *c)
{
	atomic_store_explicit(&c->fax_bypass_kind, UDSP_PASSTHROUGH_OFF,
			      memory_order_relaxed);
	c->fax_quiet_frames = 0;
	c->fax_latch_streams = 0;
	c->fax_topo_frames = 0;
}

/*
 * The passthrough kind the line runs under: what its streams describe, and the
 * tone disabler's latch while they describe nothing. A committed description
 * wins, so a caller that says fax keeps the fax policy over a latch an answer
 * tone set, and the latch governs only the case call control never described.
 *
 * A udsp_fax_profile(UDSP_FAX_OFF) or a description that drops the kind
 * therefore lets a standing latch resurface. Under the default configuration
 * the silence release bounds that; under ec_tone_latch the topology guard of
 * chan_fax_bypass_tick, a caller clear and the end of the line's streams do.
 */
static enum udsp_passthrough chan_passthrough_get(const struct udsp *s,
						  const struct udsp_chan *c)
{
	enum udsp_passthrough kind = stream_line_passthrough(s, c->line);

	if (kind != UDSP_PASSTHROUGH_OFF)
		return kind;

	return chan_fax_latch_get(c);
}

/* The processor the policy asks for. The run and the publish both read this,
 * so the record never carries a switch a configuration changed while no media
 * ran to program it into the instance. */
static bool chan_nlp_want(const struct udsp_chan *c, enum udsp_passthrough kind)
{
	return c->cfg.ec_nlp_enable && kind != UDSP_PASSTHROUGH_FAX;
}

/* The hook trace and the verdict trace of chan.h, produced only under
 * UDSP_DEBUG_SLIC_LOG so the tick pays one relaxed load at rest. */
static bool chan_trace_on(struct udsp *s)
{
	return (atomic_load_explicit(&s->debug, memory_order_relaxed) &
		UDSP_DEBUG_SLIC_LOG) != 0u;
}

/*
 * The verdict trace of chan.h. A fresh verdict always pushes a record. The
 * re-arms inside a hangover push one record per CHAN_LEC_TRACE_REARM verdict
 * frames. A talker who makes the detector fire on every frame therefore costs
 * about 13 records a second, not 100. The SECOND record counts every verdict
 * frame, so the trace samples the re-arms and hides no count.
 */
#define CHAN_LEC_TRACE_REARM	8u
#define CHAN_LEC_TRACE_SECOND	100u

static bool lec_trace_push(struct udsp_chan *c, const struct udsp_lec_trace *t)
{
	if (udsp_lec_traces_push(&c->lec_traces, t))
		return true;
	c->lec_trace_lost++;
	return false;
}

static uint8_t lec_trace_flags(const struct udsp_ec_nlms_frame *f)
{
	return (uint8_t)((f->dt_fired ? UDSP_LEC_TRACE_FIRED : 0u) |
			 (f->classic_arm ? UDSP_LEC_TRACE_CLASSIC : 0u) |
			 (f->trusted ? UDSP_LEC_TRACE_TRUSTED : 0u) |
			 (f->probing ? UDSP_LEC_TRACE_PROBING : 0u) |
			 (f->ref_quiet ? UDSP_LEC_TRACE_TAIL : 0u));
}

static void lec_trace_verdict_push(struct udsp_chan *c,
				   const struct udsp_ec_nlms_frame *f,
				   uint32_t now_ms)
{
	struct udsp_lec_trace t;

	if (!f->dt_fired)
		return;
	if (f->hold_left && ++c->lec_trace_since < CHAN_LEC_TRACE_REARM)
		return;
	c->lec_trace_since = 0;
	t = (struct udsp_lec_trace){
		.kind = UDSP_LEC_TRACE_VERDICT,
		.flags = lec_trace_flags(f),
		.hold_left = f->hold_left,
		.nlp_verdict = f->nlp_verdict,
		.gen = c->lec_trace_gen,
		.window_run = f->window_run,
		.dt_run = f->dt_run,
		.stale_run = f->stale_run,
		.stamp_ms = now_ms,
		.frames = f->frames,
		.bar = f->bar,
		.mic_power = f->mic_power,
		.ref_power = f->ref_power,
		.window_power = f->window_power,
		.estimate_power = f->estimate_power,
		.noise_power = f->noise_power,
		.erle_db = f->erle_db,
	};
	lec_trace_push(c, &t);
}

/* The counters come from their own read on this frame: chan_lec_publish reads
 * the instance only after the rest of the tick. */
static void lec_trace_second_push(struct udsp_chan *c, const struct udsp_ec *ec,
				  uint32_t frames, uint32_t now_ms)
{
	struct udsp_ec_nlms_info info;
	struct udsp_lec_trace t;

	if (frames % CHAN_LEC_TRACE_SECOND)
		return;
	udsp_ec_nlms_info_get(ec, &info);
	t = (struct udsp_lec_trace){
		.kind = UDSP_LEC_TRACE_SECOND,
		.gen = c->lec_trace_gen,
		.stamp_ms = now_ms,
		.frames = frames,
		.fired = info.nlp_dt_fired_frames,
		.dt = info.nlp_dt_frames,
		.probes = info.probes,
		.lost = c->lec_trace_lost,
		.echo_gain = info.echo_gain,
	};
	/* Only a SECOND that goes in zeroes the count. A refused one leaves
	 * the count in lec_trace_lost, and lec_trace_push adds the refusal to
	 * it, so the next SECOND reports both. */
	if (lec_trace_push(c, &t))
		c->lec_trace_lost = 0;
}

static void lec_trace_put(struct udsp *s, struct udsp_chan *c,
			  const struct udsp_ec *ec, uint32_t now_ms)
{
	struct udsp_ec_nlms_frame f;

	if (!chan_trace_on(s))
		return;
	udsp_ec_nlms_frame_get(ec, &f);
	lec_trace_verdict_push(c, &f, now_ms);
	lec_trace_second_push(c, ec, f.frames, now_ms);
}

static void chan_lec_ref_loss_push(struct udsp_chan *c, struct udsp_ec *ec)
{
	c->ec_ref_loss = (uint16_t)c->dl_ref_loss;
	udsp_ec_nlms_ref_loss_set(ec, c->ec_ref_loss);
}

/*
 * Adaptation is held until the engine has measured the loop delay, because a
 * canceller that adapts against the wrong reference diverges instead of
 * converging, and while a local tone sounds: chan_fax_run raises fax_active on
 * the start row of a local CED, ANSam, ANSbar, V.21 or MODEM and lowers it on
 * the end row. A CNG never holds it, because the detector reports both rows of
 * a CNG in one batch after the burst ended. The fax passthrough profile
 * bypasses the canceller whole.
 */
static void chan_lec_run(struct udsp *s, struct udsp_chan *c, int16_t *frame,
			 uint32_t now_ms)
{
	struct udsp_ec *ec = atomic_load_explicit(&c->ec, memory_order_acquire);
	struct chan_lec_line *l = &lec_track.line[c->line];
	struct udsp_lec_capture *cap;
	enum udsp_passthrough kind;
	bool reset = false;
	unsigned delay;
	bool nlp_want;
	bool fresh;
	bool hold;

	if (!ec)
		return;
	if (!c->cfg.ec_enable) {
		chan_lec_ref_feed(s, c, ec, frame, UDSP_LEC_CAPTURE_SKIP_OFF);
		return;
	}
	/*
	 * An open loop skips the filter whole, ahead of every policy. In the
	 * active feed the part mutes an on-hook line in both directions. The
	 * hook machine reports a release only after release_min_ms, so that a
	 * flash can be told from a hang-up (line/hook.c open_run). A filter
	 * that runs through that interval adapts against digital silence. Its
	 * ERLE goes negative and its taps decay, so the record of the call
	 * describes the hang-up.
	 *
	 * The level is the one that the line control of the previous tick
	 * consumed, because the uplink half runs first. The gate therefore
	 * follows the loop one tick late in both directions. The return comes
	 * before the generation compare, so the moves decided while the loop
	 * is open reset the filter once, on the first frame it runs again.
	 * The gate stays out of chan_tick_ul_media, because the fax detector,
	 * the relay tap and the uplink transmit still need the frame.
	 *
	 * The slic thread pushes its first sample as an edge, so hook_seen is
	 * true on every line that samples. The term keeps the canceller on a
	 * line whose slic never published a sample, because that line has no
	 * open loop to skip.
	 */
	if (c->hook_seen && !c->hook_level) {
		chan_lec_ref_feed(s, c, ec, frame, UDSP_LEC_CAPTURE_SKIP_OPEN);
		c->lec_loop_open = true;
		return;
	}
	/*
	 * The passthrough policy, one per line, because the canceller is the
	 * line's. A modem exchange is full duplex: both carriers stand at
	 * once, so the canceller is skipped whole, with no filter output, no
	 * update and no non-linear processor. include/udsp/ec.h gives freeze()
	 * one meaning, hold adaptation, and dsp/lec.h says in as many words
	 * that it is not the fax bypass: a frozen filter still subtracts its
	 * converged estimate from the carrier, which is the distortion this
	 * skip exists to avoid.
	 *
	 * A fax exchange is half duplex, so the canceller runs and only the
	 * non-linear processor goes off. The processor replaces the residual
	 * with its own signal and mutes the direction it decides is idle,
	 * which a turn of a half-duplex exchange cannot afford; the filter
	 * itself sees clean single talk and adapts on it, under the rules
	 * every other frame follows.
	 *
	 * The two policies read the same whether a description or the tone
	 * disabler's latch names the kind, which is what makes the disabler
	 * the same switch call control already has.
	 */
	kind = chan_passthrough_get(s, c);
	if (kind == UDSP_PASSTHROUGH_MODEM) {
		chan_lec_ref_feed(s, c, ec, frame, UDSP_LEC_CAPTURE_SKIP_MODEM);
		return;
	}
	fresh = chan_lec_instance_take(s, c, ec);
	/*
	 * The tracker follows the instance. stream_lec_ensure creates the
	 * canceller on the control thread and programs the channel's own
	 * setting into it; this tick owns the tracker and never saw that
	 * write. Seeding it when the pointer moves is what makes the compare
	 * below decide anything for the first frame of a new instance: a line
	 * whose first description already carries the fax kind would otherwise
	 * read a tracker that agrees by accident and leave the processor on.
	 * The reference loss takes the same path, because the control thread
	 * creates every instance at a loss of 0.
	 *
	 * The line's generation is seeded here too, on the first frame the
	 * filter runs and not on a feed before it. A canceller that has not
	 * run carries no alignment to lose, so it must be neither reset nor
	 * counted.
	 */
	if (c->ec_nlp_for != ec) {
		c->ec_nlp_for = ec;
		c->ec_nlp = c->cfg.ec_nlp_enable;
		l->ran_gen = l->gen;
		chan_lec_ref_loss_push(c, ec);
	}
	/*
	 * The receive loss of the line stands ahead of the reference
	 * (chan_rx_split_apply). On an instance that ran, the setter starts the
	 * instance over (dsp/lec.h udsp_ec_nlms_ref_loss_set), so the frame
	 * count starts over and the generation of the traces moves.
	 */
	if (c->ec_ref_loss != (uint16_t)c->dl_ref_loss) {
		chan_lec_ref_loss_push(c, ec);
		c->lec_trace_gen++;
		reset = true;
	}
	nlp_want = chan_nlp_want(c, kind);
	if (c->ec_nlp != nlp_want) {
		c->ec_nlp = nlp_want;
		udsp_ec_nlms.nlp(ec, c->ec_nlp);
	}
	/*
	 * A move of the line's reference re-times it. The stale coefficients
	 * would then subtract a misaligned estimate for up to a second, so the
	 * instance starts over instead (dsp/lec.h udsp_ec_nlms_reset). The
	 * frame count starts over, so the generation of the traces moves.
	 */
	if (l->ran_gen != l->gen) {
		l->ran_gen = l->gen;
		c->lec_retimes++;
		udsp_ec_nlms_reset(ec);
		c->lec_trace_gen++;
		reset = true;
	}
	/*
	 * Adaptation waits until the line's reference stands. The tone
	 * disabler's latch holds adaptation too. A latch is set on a tone
	 * alone, so the filter would otherwise adapt against a pure tone and
	 * diverge. The fax kind of a description is call control saying a fax
	 * call stands. There fax_active freezes the carrier bursts themselves,
	 * and the speech between them still adapts.
	 */
	hold = !atomic_load_explicit(&s->lec_ready[c->line],
				     memory_order_relaxed) ||
	       c->fax_active || chan_fax_latch_get(c) != UDSP_PASSTHROUGH_OFF;
	udsp_ec_nlms.freeze(ec, hold);
	delay = chan_lec_ref_take(s, c);
	cap = lec_capture_begin(s, c, frame);
	udsp_ec_nlms.run(ec, frame, c->ref);
	c->ec_ran = true;
	lec_trace_put(s, c, ec, now_ms);
	if (cap)
		lec_capture_end(s, c, ec, cap, frame, delay,
				(fresh ? UDSP_LEC_CAPTURE_NEW : 0u) |
				(reset ? UDSP_LEC_CAPTURE_RESET : 0u) |
				(hold ? UDSP_LEC_CAPTURE_FREEZE : 0u) |
				(c->ec_nlp ? UDSP_LEC_CAPTURE_NLP : 0u));
}

static void chan_lec_stats_fill(struct udsp_lec_stats *out,
				const struct udsp_ec_nlms_info *info)
{
	memset(out, 0, sizeof(*out));
	out->tail_ms = info->tail_ms;
	out->taps = info->taps;
	out->frames = info->frames;
	out->hold_frames = info->hold_frames;
	out->skip_frames = info->skip_frames;
	out->diverge_resets = info->diverge_resets;
	out->probes = info->probes;
	out->erle_mdb = chan_lec_mdb(info->erle_db);
	out->coef_energy_micro = chan_lec_micro(info->coef_energy);
	out->echo_gain_micro = chan_lec_micro(info->echo_gain);
	out->double_talk = info->double_talk ? 1u : 0u;
	out->nlp_active = info->nlp_active ? 1u : 0u;
	out->nlp_active_frames = info->nlp_active_frames;
	out->nlp_wait_frames = info->nlp_wait_frames;
	out->nlp_erle_frames = info->nlp_erle_frames;
	out->nlp_unlike_frames = info->nlp_unlike_frames;
	out->nlp_dt_frames = info->nlp_dt_frames;
	out->nlp_quiet_frames = info->nlp_quiet_frames;
	out->coef_peak_tap = info->coef_peak_tap;
}

/*
 * The canceller's state, published the way the jitter buffer counters are
 * (stream.c stream_rx_run): one double-buffer write per line per tick, and a
 * flag that only a publish which succeeded sets, so a reader is refused rather
 * than handed a reading of a canceller the line no longer runs.
 *
 * The one state-dependent refusal the getter gives is decided here, on the
 * thread that owns the instance. dsp/lec.h allows udsp_ec_nlms_info_get from
 * that thread alone, and a line with no canceller has nothing to read. Every
 * line that holds an instance answers, whatever policy or configuration the
 * frame ran under. A modem passthrough, a channel that switched the canceller
 * off and an open loop leave the filter unrun. The record then reads ec_active
 * 0 and nlp_enabled 0, and its counters stand still. loop_open tells the open
 * loop apart from the other two. A fax passthrough runs the filter with its
 * non-linear processor off. That reads ec_active 1 and nlp_enabled 0 for as
 * long as the kind lasts.
 *
 * The line's stream does not gate the publish. A canceller is created at
 * stream config and applies its tail there. A line between the config and the
 * start therefore reads frames 0, which is true of it.
 *
 * The tone disabler's latch reads exactly as the description kind it mirrors,
 * because the effective kind decides here too. Every transition of it comes
 * one frame late: the remote detector decides in the downlink half and this
 * publish follows it, so a transition row describes a frame the filter ran
 * under the policy before it.
 *
 * The conversions run here and not in the getter for the reason the fill runs
 * one field at a time: the tick is the only writer, so the record a reader
 * copies is the record the engine measured and nothing reshapes it later.
 */
static void chan_lec_publish(struct udsp *s, struct udsp_chan *c)
{
	struct udsp_ec *ec = atomic_load_explicit(&c->ec, memory_order_acquire);
	enum udsp_passthrough kind = chan_passthrough_get(s, c);
	struct udsp_ec_nlms_info info;
	struct udsp_lec_stats out;

	if (!ec) {
		s->lec_pub[c->line] = NULL;
		atomic_store_explicit(&s->lec_valid[c->line], false,
				      memory_order_release);
		return;
	}
	/*
	 * A line that changed canceller has no reading for the new one yet,
	 * and snap.h skips a publish while a reader holds the slot, so the
	 * flag is cleared first and the publish below sets it again (the
	 * stream_jb_restart rule of core/stream.c).
	 */
	if (s->lec_pub[c->line] != ec) {
		s->lec_pub[c->line] = ec;
		atomic_store_explicit(&s->lec_valid[c->line], false,
				      memory_order_release);
	}

	udsp_ec_nlms_info_get(ec, &info);
	chan_lec_stats_fill(&out, &info);
	out.ec_active = c->ec_ran ? 1u : 0u;
	out.passthrough = (uint8_t)kind;
	/* The policy's switch and not the instance's. A configuration that
	 * changed while no media ran has programmed nothing yet, and the record
	 * must not carry the value it replaced. The gate on the measured run
	 * keeps a skipped filter from reporting a processor. */
	out.nlp_enabled = (c->ec_ran && chan_nlp_want(c, kind)) ? 1u : 0u;
	/*
	 * The reference alignment is state of the line and not of the
	 * instance, so it is read here and not in the fill. These are the two
	 * atomics chan_lec_run consumes, loaded the same relaxed way. The
	 * record therefore carries the reference this tick applied, and the
	 * readiness that decided whether the tick let the filter adapt at all.
	 *
	 * While the instance stands, the line keeps its value within one period
	 * of the measurement. It can therefore differ from the engine's
	 * loop_ref by that much, and two lines can read different values.
	 */
	out.ref_delay_samples =
		(uint32_t)atomic_load_explicit(&s->lec_delay[c->line],
					       memory_order_relaxed);
	out.ref_ready = atomic_load_explicit(&s->lec_ready[c->line],
					     memory_order_relaxed) ? 1u : 0u;
	/* State of the channel, not of the instance: the count survives the
	 * reset it names and goes only when the instance leaves the line. */
	out.retimes = c->lec_retimes;
	/* The predicate the run read and not the level, because the line
	 * control of this tick can consume an edge after the run. */
	out.loop_open = c->lec_loop_open ? 1u : 0u;
	if (lec_snap_publish(&s->lec_snap[c->line], &out))
		atomic_store_explicit(&s->lec_valid[c->line], true,
				      memory_order_release);
}

/*
 * The one place the re-time count is zeroed. The count follows the channel and
 * not the address the allocator hands out. The publish target, the processor
 * tracker and the capture tracker go with it. The next canceller can get the
 * address this one had, and it must be recognized as new.
 *
 * Under the capture bit the drop pushes the END record of an instance that the
 * tick handed a frame. An instance that never had one gave no record, so it
 * gives no END either. The loss count of the line survives the drop. An END
 * that finds the ring full therefore leaves its count to the next record of
 * the line.
 *
 * The drop clears the reference of the line and publishes none. A line
 * without an instance decides nothing. A hold at idle therefore cannot leave
 * it a value inside the tolerance, and the next instance takes the offer
 * verbatim.
 *
 * The tick's release handler calls it while c->ec still holds the instance and
 * before the mailbox holds it. The control thread can free the instance as
 * soon as it harvests the mailbox. These fields belong to the tick, so a
 * control-side write while the engine ticks would race it. A release whose
 * acknowledgment is lost runs the handler anyway, and a stopped engine can
 * still hold a tick thread that did not join.
 *
 * The control thread calls it in the branch of core/stream.c
 * stream_lec_reclaim that reads engine_live false, before the exchange that
 * nulls c->ec. A clean stop clears engine_live after the join, so no tick can
 * read the instance then. A stop that timed out clears it without a join. The
 * exchanges of that branch already carry that hazard, and the drop adds none.
 * Without the drop, a later engine would decide a reference for the line. A
 * new instance at the same address would also not read as new.
 */
void chan_lec_tracker_drop(struct udsp *s, struct udsp_chan *c)
{
	const struct udsp_ec *ec = atomic_load_explicit(&c->ec,
							memory_order_acquire);
	struct udsp_lec_capture *rec = NULL;

	if (c->lec_cap_for == ec)
		rec = lec_capture_claim(s, c);
	if (rec) {
		lec_capture_head_fill(s, c, ec, rec, UDSP_LEC_CAPTURE_END, 0,
				      0);
		lec_capture_publish(s, c, rec);
	}
	s->lec_pub[c->line] = NULL;
	c->ec_nlp_for = NULL;
	c->lec_cap_for = NULL;
	c->lec_retimes = 0;
	chan_lec_line_drop(&lec_track, c->line);
	atomic_store_explicit(&s->lec_ready[c->line], false,
			      memory_order_relaxed);
	atomic_store_explicit(&s->lec_delay[c->line], 0u,
			      memory_order_relaxed);
}

/*
 * FASTPATH, the stream_jb_stats shape step for step: no lock, the dead mark
 * read before the state, and the same UDSP_UNSUPPORTED for every line that
 * cannot answer. It runs on the caller's thread and reads only what the tick
 * published, so the file's rule that nothing here logs, allocates or takes a
 * mutex holds for it too.
 */
enum udsp_status chan_lec_stats(struct udsp *s, unsigned line,
				struct udsp_lec_stats *out)
{
	if (out)
		memset(out, 0, sizeof(*out));
	if (!s || !out || line >= s->cap.lines)
		return UDSP_UNSUPPORTED;
	if (atomic_load_explicit(&s->chan[line].dead, memory_order_acquire))
		return UDSP_FATAL;
	if (!atomic_load_explicit(&s->lec_valid[line], memory_order_acquire))
		return UDSP_UNSUPPORTED;
	lec_snap_get(&s->lec_snap[line], out);

	return UDSP_OK;
}

/*
 * The 8 kHz frame a detector reads. A narrowband line hands its own frame
 * over untouched, so no filter runs and the detector sees the line samples
 * themselves; a wideband line decimates into the per-line scratch
 * (dsp/resamp.h: even counts, at most UDSP_FRAME_MAX in, one instance per
 * feed, reset before first use).
 */
static const int16_t *chan_det_frame(struct udsp_chan *c,
				     struct udsp_resamp *rs, const int16_t *frame)
{
	if (c->rate == UDSP_SAMPLE_RATE)
		return frame;

	udsp_resamp_down2(rs, frame, c->frame, c->det);

	return c->det;
}

static void chan_fax_report(struct udsp *s, struct udsp_chan *c,
			    const struct udsp_fax_result *row, uint32_t now_ms)
{
	struct udsp_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = UDSP_EV_TONE;
	ev.line = (uint16_t)c->line;
	ev.stamp_ms = now_ms;
	ev.u.tone.code = row->tone;
	ev.u.tone.direction = row->direction;
	ev.u.tone.end = !row->start;
	ev.u.tone.duration_ms = row->duration_ms > UINT16_MAX ? UINT16_MAX :
				(uint16_t)row->duration_ms;
	chan_event_post(s, &ev);
}

/*
 * The disabler's own report, and the reason it is an event and not a log line.
 * The policy moves inside the engine tick, where nothing logs, allocates or
 * takes a mutex (dsp/lec.h), so the move travels the channel the tone rows
 * already travel and the consumer that reads them names it.
 */
static void chan_lec_policy_report(struct udsp *s, struct udsp_chan *c,
				   enum udsp_passthrough kind,
				   enum udsp_lec_reason reason, uint16_t tone,
				   uint8_t direction, uint32_t now_ms)
{
	struct udsp_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = UDSP_EV_LEC_POLICY;
	ev.line = (uint16_t)c->line;
	ev.stamp_ms = now_ms;
	ev.u.lec.kind = (uint8_t)kind;
	ev.u.lec.reason = (uint8_t)reason;
	ev.u.lec.tone = tone;
	ev.u.lec.direction = direction;
	chan_event_post(s, &ev);
}

/* The one weaker kind a standing latch admits is the V.21 demotion of the modem
 * kind; every other move takes a kind the line held none of, or the stronger
 * kind a later answer tone asked for. */
static enum udsp_lec_reason chan_fax_latch_reason(enum udsp_passthrough was,
						  enum udsp_passthrough want)
{
	if (was == UDSP_PASSTHROUGH_MODEM && want == UDSP_PASSTHROUGH_FAX)
		return UDSP_LEC_REASON_DEMOTE;

	return UDSP_LEC_REASON_LATCH;
}

/*
 * The tone disabler, G.168 clause 7 applied to a start row from either
 * direction: the called fax answers on the wire and the calling one on the
 * phone side, and the recommendation asks a disabler to honour both paths.
 *
 * The kind ladder is the mapping of 7.1. Phase reversals are the one signal
 * the recommendation lets a disabler answer with a full disable, so they take
 * the modem policy; a plain 2100 Hz takes the fax policy, which is the
 * sanctioned optional behaviour for it, unless the caller asked for the full
 * disable on it too. A plain tone never demotes a latch the reversals
 * established, and a V.21 preamble does: it is the fax confirming itself,
 * which is the super G3 tone followed by a G3 fallback.
 *
 * A generic modem carrier row neither promotes nor demotes. The page carriers
 * of a fax fall in the modem bank, and the policy of a page in flight is the
 * one its handshake chose.
 */
static void chan_fax_latch(struct udsp *s, struct udsp_chan *c, uint16_t tone,
			   uint8_t direction, uint32_t now_ms)
{
	enum udsp_passthrough kind = chan_fax_latch_get(c);
	enum udsp_passthrough want = kind;

	switch (tone) {
	case UDSP_TONE_DET_ANSBAR:
		want = UDSP_PASSTHROUGH_MODEM;
		break;
	case UDSP_TONE_DET_CED:
	case UDSP_TONE_DET_ANSAM:
		if (kind == UDSP_PASSTHROUGH_OFF)
			want = c->cfg.ec_tone_force_disable ?
			       UDSP_PASSTHROUGH_MODEM : UDSP_PASSTHROUGH_FAX;
		break;
	case UDSP_TONE_DET_V21_PREAMBLE:
		if (kind != UDSP_PASSTHROUGH_OFF)
			want = UDSP_PASSTHROUGH_FAX;
		break;
	default:
		return;
	}
	if (want == UDSP_PASSTHROUGH_OFF)
		return;
	atomic_store_explicit(&c->fax_bypass_kind, (uint_least8_t)want,
			      memory_order_relaxed);
	/* A row that asks for the kind the latch already holds restarts the
	 * release window below and says nothing about the policy, so only a
	 * kind that moved is reported, and it is reported after the store so
	 * the record never precedes the state it describes. */
	if (want != kind)
		chan_lec_policy_report(s, c, want,
				       chan_fax_latch_reason(kind, want), tone,
				       direction, now_ms);
	/* Every row that latches restarts the release window and records the
	 * topology again, so a later answer tone re-latches a line whose
	 * streams moved under the previous one. */
	c->fax_quiet_frames = 0;
	c->fax_topo_frames = 0;
	c->fax_latch_streams = (uint8_t)stream_line_govern_count(s, c->line);
}

static void chan_fax_run(struct udsp *s, struct udsp_chan *c, bool local,
			 const int16_t *frame, uint32_t now_ms)
{
	struct udsp_fax_result row[UDSP_FAX_RESULT_MAX];
	struct udsp_fax_detector *det = local ? &c->fax_local : &c->fax_remote;
	const int16_t *in;
	size_t n;

	if (!c->fax_armed)
		return;

	in = chan_det_frame(c, local ? &c->rs_ul_post : &c->rs_dl, frame);
	n = udsp_fax_detector_process(det, in, row, UDSP_FAX_RESULT_MAX);
	for (size_t i = 0; i < n; i++) {
		if (local)
			c->fax_active = row[i].start;
		/* The row leaves before the policy it moves, so a reader gets
		 * the tone and then what the disabler did with it. The latch
		 * takes its direction from the row and not from local, so the
		 * two records name the same feed by construction. */
		chan_fax_report(s, c, &row[i], now_ms);
		if (row[i].start)
			chan_fax_latch(s, c, row[i].tone, row[i].direction,
				       now_ms);
	}
}

/* Either feed of the last frame carried the holding band, so the disabler
 * holds. Both are read, because G.168 7.8 releases on the drop in both
 * directions and not on the drop in one. */
static bool chan_fax_band_busy(const struct udsp_chan *c)
{
	return udsp_fax_detector_hold_energy_get(&c->fax_local) >=
	       UDSP_FAX_HOLD_ENERGY ||
	       udsp_fax_detector_hold_energy_get(&c->fax_remote) >=
	       UDSP_FAX_HOLD_ENERGY;
}

/*
 * The release rule of the tone disabler, run once per tick after both
 * detectors have measured their frame.
 *
 * It runs on a line whose media chain ran and nowhere else, because both rules
 * below read what that chain produced. An idle line has no detector reading to
 * release on and no topology to compare against, and its release is the one
 * stream_lec_release gives it. The rule matters for more than an idle line: a
 * description that reconfigures a running stream deactivates it for the ticks
 * the control thread needs to prepare the new one, and a guard that read the
 * line then would take that for a leg that left.
 *
 * The topology guard comes first. The latch is the line's, and a leg that went
 * on hold or died is a leg whose carrier is not on the line any more, so a
 * latch it set must not keep the canceller off a leg that is still talking. A
 * stream that joined the line moves the count the same way. Either way the
 * line needs a new answer tone to latch again. The move must outlast
 * UDSP_FAX_TOPO_RELEASE_FRAMES, because a reconfiguration of one leg of a
 * two-leg line quiesces it for a tick or two and restores the same topology,
 * and a latch that fell to that would drop the bypass under every mid-call
 * codec or jitter update.
 *
 * Then the level rule of G.168 7.4 and 7.8: the hold stands while either feed
 * carries the holding band, and the window is counted from the frame the band
 * fell quiet. A modem session keeps its own energy above the floor, so the
 * hold survives the end rows of the tone that set it.
 */
static void chan_fax_bypass_tick(struct udsp *s, struct udsp_chan *c,
				 uint32_t now_ms)
{
	enum udsp_passthrough kind = chan_fax_latch_get(c);
	unsigned window;

	if (kind == UDSP_PASSTHROUGH_OFF || !c->fax_armed)
		return;
	if (!stream_line_active(s, c->line))
		return;
	if (stream_line_govern_count(s, c->line) != c->fax_latch_streams) {
		if (++c->fax_topo_frames < UDSP_FAX_TOPO_RELEASE_FRAMES)
			return;
		chan_fax_latch_clear(c);
		chan_lec_policy_report(s, c, UDSP_PASSTHROUGH_OFF,
				       UDSP_LEC_REASON_RELEASE_TOPOLOGY, 0, 0,
				       now_ms);
		return;
	}
	c->fax_topo_frames = 0;
	if (c->cfg.ec_tone_latch)
		return;
	if (chan_fax_band_busy(c)) {
		c->fax_quiet_frames = 0;
		return;
	}
	window = kind == UDSP_PASSTHROUGH_FAX ?
		 UDSP_FAX_BYPASS_FAX_RELEASE_FRAMES :
		 UDSP_FAX_BYPASS_RELEASE_FRAMES;
	if (++c->fax_quiet_frames < window)
		return;
	chan_fax_latch_clear(c);
	chan_lec_policy_report(s, c, UDSP_PASSTHROUGH_OFF,
			       UDSP_LEC_REASON_RELEASE_QUIET, 0, 0, now_ms);
}

/* The first tick of an idle line clears the ring and the ticks after it write
 * nothing, so a reader outside the tick sees a settled record. The digit
 * fields stay as they are: they belong to the detector, not to the ring. */
void chan_relay_tap_reset(struct udsp_chan *c)
{
	if (!c->ul_delay_fill && !c->ul_delay_head)
		return;

	memset(c->ul_delay_mute, 0, sizeof(c->ul_delay_mute));
	c->ul_delay_head = 0;
	c->ul_delay_fill = 0;
}

/*
 * The lookahead ring a stream in an out of band digit mode reads (chan.h).
 * The mark covers every frame the ring holds and not merely the new one, so
 * the tick that validates a digit also removes the onset of the tone the ring
 * still carries. Setting it again on each tick of the digit costs one pass of
 * three flags and needs no second edge.
 *
 * A digit playing toward the line marks the ring too, for the tail of the
 * hold: the detector holds no digit there, and the echo of the tone would
 * otherwise leave as the uplink of a stream that carries the digit out of
 * band.
 */
static void chan_relay_tap_run(struct udsp_chan *c, const int16_t *frame)
{
	bool digit = c->digit_active != 0 || c->dl_digit_hold != 0;
	size_t bytes = c->frame * sizeof(*frame);
	unsigned head = c->ul_delay_head;
	unsigned i;

	if (digit) {
		for (i = 0; i < UDSP_DTMF_MUTE_LOOKAHEAD_FRAMES; i++)
			c->ul_delay_mute[i] = true;
	}
	if (c->ul_delay_fill < UDSP_DTMF_MUTE_LOOKAHEAD_FRAMES) {
		memset(c->ul_relay, 0, bytes);
		c->ul_delay_fill++;
	} else if (c->ul_delay_mute[head]) {
		memset(c->ul_relay, 0, bytes);
	} else {
		memcpy(c->ul_relay, c->ul_delay[head], bytes);
	}
	memcpy(c->ul_delay[head], frame, bytes);
	c->ul_delay_mute[head] = digit;
	c->ul_delay_head = (head + 1u) % UDSP_DTMF_MUTE_LOOKAHEAD_FRAMES;
}

void chan_tick_ul_media(struct udsp *s, struct udsp_chan *c, int16_t *frame,
			uint32_t now_ms)
{
	struct udsp_ec *ec;

	if (!stream_line_active(s, c->line)) {
		chan_relay_tap_reset(c);
		/*
		 * A reconfiguration deactivates a running stream for a tick or
		 * two (stream.c stream_quiesce), and the canceller stays on the
		 * line through it. Without the feed, the first tail after the
		 * stream comes back adapts against a stale window.
		 */
		ec = atomic_load_explicit(&c->ec, memory_order_acquire);
		if (ec)
			chan_lec_ref_feed(s, c, ec, frame,
					  UDSP_LEC_CAPTURE_SKIP_INACTIVE);
		return;
	}

	chan_lec_run(s, c, frame, now_ms);
	chan_fax_run(s, c, true, frame, now_ms);
	chan_relay_tap_run(c, frame);
	stream_tick_tx(s, c, frame, now_ms);
}

void chan_tick_dl_media(struct udsp *s, struct udsp_chan *c, int16_t *frame,
			uint32_t now_ms)
{
	memset(frame, 0, c->frame * sizeof(*frame));
	if (!stream_line_active(s, c->line))
		return;

	stream_tick_rx(s, c, frame, now_ms);
	chan_fax_run(s, c, false, frame, now_ms);
}

/* --- the delayed loop ---------------------------------------------------- */

/*
 * One ring per line, UDSP_LOOP_SLOTS frames of the settled line rate, taken at
 * open in the stream_pool_alloc shape and made resident, because the tick
 * writes a slot every period and never allocates. A narrowband line costs
 * about 16 KB and a wideband one about 32 KB.
 */
int chan_loop_pool_alloc(struct udsp *s)
{
	size_t n = (size_t)UDSP_LOOP_SLOTS * s->frame;

	for (unsigned i = 0; i < s->cap.lines; i++) {
		int16_t *ring = calloc(n, sizeof(*ring));

		if (!ring)
			return -ENOMEM;
		s->loop_ring[i] = ring;
		rt_range_prefault(ring, n * sizeof(*ring));
	}

	return 0;
}

/* Every line, as stream_pool_free walks every slot, so an open that failed
 * part way through the pool still releases what it took. */
void chan_loop_pool_free(struct udsp *s)
{
	for (unsigned i = 0; i < UDSP_LINE_MAX; i++) {
		free(s->loop_ring[i]);
		s->loop_ring[i] = NULL;
	}
}

/* The slot of one tick, or NULL for a line that carries no ring: a line past
 * the capability, or an open that failed before the pool. */
static int16_t *chan_loop_slot(struct udsp *s, unsigned line, uint64_t tick)
{
	int16_t *ring = s->loop_ring[line];

	if (!ring)
		return NULL;

	return ring + (size_t)(tick % UDSP_LOOP_SLOTS) * s->frame;
}

void chan_loop_ring_clear(struct udsp *s, unsigned line)
{
	int16_t *ring = s->loop_ring[line];

	if (!ring)
		return;

	memset(ring, 0, (size_t)UDSP_LOOP_SLOTS * s->frame * sizeof(*ring));
}

/*
 * The record goes with the mode, on the one thread that writes both. The tick
 * reaches this from the OFF arm and from chan_lines_safe, where nothing on the
 * control side knows the loop is gone.
 */
void chan_loop_disarm(struct udsp *s, struct udsp_chan *c)
{
	c->loop_mode = UDSP_LOOP_OFF;
	c->loop_peer = 0;
	c->loop_delay = 0;
	chan_loop_ring_clear(s, c->line);
	atomic_store_explicit(&s->loop_armed[c->line], 0u,
			      memory_order_release);
}

static void chan_loop_slot_zero(struct udsp *s, unsigned line, uint64_t tick)
{
	int16_t *slot = chan_loop_slot(s, line, tick);

	if (!slot)
		return;

	memset(slot, 0, s->frame * sizeof(*slot));
}

/*
 * The raw capture of this tick, taken before gain_tx so the loop carries what
 * the hybrid delivered. It runs for every live line and not only for an armed
 * one, so a loop that arms now finds no stale audio behind it. A partial slice
 * cannot happen with the geometry the engine grants; the slot is zeroed if one
 * ever does, because a short copy would leave the tail of the previous wrap in
 * it.
 */
static void chan_loop_write(struct udsp *s, struct udsp_chan *c,
			    const int16_t *rx, unsigned frames)
{
	int16_t *slot;

	if (frames != c->frame) {
		chan_loop_slot_zero(s, c->line, s->loop_tick);
		return;
	}
	slot = chan_loop_slot(s, c->line, s->loop_tick);
	if (!slot)
		return;

	memcpy(slot, rx, frames * sizeof(*slot));
}

/*
 * The delayed frame the loop plays, mixed into the downlink before the voice
 * gain and before the local playout, so the loop takes the whole receive gain
 * of the port (dl_voice_gain). The source is the peer under PEER and the line
 * itself otherwise.
 *
 * A dead source plays silence: its ring stopped at the death, and the frames
 * behind it are older than the line the peer holds now. Every line reads the
 * one tick index, so a cross connection carries exactly loop_delay ticks in
 * both directions whatever order the lines run in.
 */
static void chan_loop_dl(struct udsp *s, struct udsp_chan *c)
{
	unsigned src = c->loop_mode == UDSP_LOOP_PEER ? c->loop_peer : c->line;
	const int16_t *slot;

	if (c->loop_mode == UDSP_LOOP_OFF || s->loop_tick < c->loop_delay)
		return;
	if (atomic_load_explicit(&s->chan[src].dead, memory_order_acquire))
		return;
	slot = chan_loop_slot(s, src, s->loop_tick - c->loop_delay);
	if (!slot)
		return;

	frame_add_sat(c->dl, slot, c->frame);
}

static void chan_loop_lines_zero(struct udsp *s, unsigned lines, uint64_t tick)
{
	for (unsigned i = 0; i < lines; i++)
		chan_loop_slot_zero(s, i, tick);
}

/*
 * The engine dropped capture periods, so no frame of them reached a ring. The
 * slots of the skipped ticks are zeroed, or the loop would play the audio of
 * the previous wrap where the line carried none. The tick index is already
 * past the skipped periods (engine.c engine_capture), so the interval is the
 * periods before it, and one ring holds at most UDSP_LOOP_SLOTS of them.
 */
static void chan_loop_skip(struct udsp *s, unsigned periods)
{
	unsigned lines = udsp_lines(s);

	if (periods > UDSP_LOOP_SLOTS)
		periods = UDSP_LOOP_SLOTS;
	if ((uint64_t)periods > s->loop_tick)
		periods = (unsigned)s->loop_tick;

	for (unsigned i = 1; i <= periods; i++)
		chan_loop_lines_zero(s, lines, s->loop_tick - i);
}

bool udsp_hook_edge_push(struct udsp_hook_edges *e, uint32_t stamp_ms, bool level,
			 uint8_t flags, uint16_t gap_ms)
{
	unsigned head = atomic_load_explicit(&e->head, memory_order_relaxed);
	unsigned next = (head + 1u) % UDSP_HOOK_EDGE_DEPTH;

	if (next == atomic_load_explicit(&e->tail, memory_order_acquire))
		return false;

	e->slot[head].stamp_ms = stamp_ms;
	e->slot[head].level = level ? 1u : 0u;
	e->slot[head].flags = flags;
	e->slot[head].gap_ms = gap_ms;
	atomic_store_explicit(&e->head, next, memory_order_release);

	return true;
}

bool udsp_hook_edge_pop(struct udsp_hook_edges *e, struct udsp_hook_edge *out)
{
	unsigned tail = atomic_load_explicit(&e->tail, memory_order_relaxed);

	if (tail == atomic_load_explicit(&e->head, memory_order_acquire))
		return false;

	*out = e->slot[tail];
	atomic_store_explicit(&e->tail, (tail + 1u) % UDSP_HOOK_EDGE_DEPTH,
			      memory_order_release);

	return true;
}

/* --- slic record batching ------------------------------------------------ */

static struct slic_rec *batch_add(struct chan_batch *b, enum slic_rec_type type,
				  unsigned line, unsigned arg)
{
	if (b->count >= CHAN_BATCH_MAX) {
		b->overflow = true;
		return NULL;
	}

	struct slic_rec *r = &b->rec[b->count++];

	memset(r, 0, sizeof(*r));
	r->type = (uint8_t)type;
	r->line = (uint8_t)line;
	r->arg = (uint8_t)arg;

	return r;
}

/* The ack goes with the last record of the command and carries the first
 * failing errno the slic thread met on the way. */
static bool batch_flush(struct udsp *s, struct chan_batch *b, uint32_t token,
			struct cmd_ack *ack)
{
	if (b->overflow)
		udsp_fatal_latch(s, UDSP_EV_PROD_ENGINE);
	if (!b->count)
		return false;

	for (unsigned i = 0; i < b->count; i++) {
		/* Every record of one command carries its token, so the slic
		 * thread charges their failures to the ack instead of counting
		 * them twice; only the last one carries the ack itself. */
		b->rec[i].token = token;
		if (ack && i + 1 == b->count) {
			b->rec[i].ack = ack;
			b->rec[i].last = true;
		}
		if (slic_ring_push(&s->ring, &b->rec[i]))
			continue;
		/* Cadence-paced traffic cannot reach 128 records, so an
		 * overflow is an engine defect, not back pressure. */
		udsp_op_error(s);
		udsp_fatal_latch(s, UDSP_EV_PROD_ENGINE);
	}
	s->slic_ring_pushed = true;
	b->count = 0;

	return true;
}

static uint8_t lf_polar(uint8_t lf, uint8_t pol)
{
	bool rev = pol == (uint8_t)UDSP_POL_REVERSE;

	switch (lf) {
	case UDSP_LF_FWD_ACTIVE:
	case UDSP_LF_REV_ACTIVE:
		return rev ? UDSP_LF_REV_ACTIVE : UDSP_LF_FWD_ACTIVE;
	case UDSP_LF_FWD_OHT:
	case UDSP_LF_REV_OHT:
		return rev ? UDSP_LF_REV_OHT : UDSP_LF_FWD_OHT;
	default:
		return lf;
	}
}

/* A fault hold and a recovery in flight keep the feed open. Every powered
 * write is dropped until they end: a base request survives in base_lf_wanted,
 * a machine's write is lost with the session it belonged to. */
static bool lf_allowed(const struct udsp_chan *c, uint8_t lf)
{
	if (lf == UDSP_LF_OPEN)
		return true;

	return !c->fault_held && !c->fault_reinit;
}

/* Each linefeed, polarity or ring relay record carries a write generation.
 * The engine tells a queued feed write from one the slic thread ran by it
 * (chan.h). */
static void lf_rec_emit(struct udsp_chan *c, struct chan_batch *b,
			enum slic_rec_type type, uint8_t arg)
{
	struct slic_rec *r = batch_add(b, type, c->line, arg);

	if (r)
		r->lf_seq = ++c->lf_seq_emit;
}

/* The confirmed value is not touched here: only a successful linefeed_set on
 * the slic thread advances it. */
static void lf_emit(struct udsp_chan *c, struct chan_batch *b, uint8_t lf)
{
	if (!lf_allowed(c, lf))
		return;
	lf_rec_emit(c, b, SLIC_REC_LINEFEED, lf);
}

static uint8_t lf_confirmed(const struct udsp_chan *c)
{
	return (uint8_t)atomic_load_explicit(&c->lf_confirmed,
					     memory_order_acquire);
}

/* True while a queued feed write (slic_rec_seq_carried) has not run on the
 * slic thread. The clear and the hold lift wait for it, so neither fires
 * against a confirmation the pending write is about to change. */
static bool chan_lf_in_flight(const struct udsp_chan *c)
{
	return c->lf_seq_emit !=
	       (uint32_t)atomic_load_explicit(&c->lf_seq_done,
					      memory_order_acquire);
}

/* The one writer of the published fault word (chan.h UDSP_FAULT_PUB_*), called
 * wherever the tick moves one of its three inputs. */
static void chan_fault_publish(struct udsp_chan *c)
{
	unsigned word = c->fault_read_bits & UDSP_FAULT_PUB_BITS;

	if (c->fault_open)
		word |= UDSP_FAULT_PUB_OPEN;
	if (c->fault_held)
		word |= UDSP_FAULT_PUB_HELD;
	atomic_store_explicit(&c->fault_pub, word, memory_order_release);
}

/* Write suppression compares the confirmed pair, never the desired one, so a
 * retry after a failed write reaches the register instead of being answered
 * UDSP_OK against a state the hardware never entered. */
static void gain_emit(struct udsp_chan *c, struct chan_batch *b)
{
	int8_t rx = udsp_slic_rx_programmed(c->cfg.slic_gain_rx);
	uint32_t want = udsp_gain_confirmed_pack(c->cfg.slic_gain_tx, rx);
	struct slic_rec *r;

	if (atomic_load_explicit(&c->gain_confirmed,
				 memory_order_acquire) == want)
		return;
	r = batch_add(b, SLIC_REC_GAIN, c->line, 0);
	if (!r)
		return;
	r->gain_tx = c->cfg.slic_gain_tx;
	r->gain_rx = rx;
}

/* Every SLIC gain record is a machine record on both paths: an audio-level
 * write is not service viability, and an errno answered to uvoice's
 * detector_arm would take the line out of service over a level write. A
 * failure counts udsp_op_error (slic_records_run) and heals at the next emit,
 * because the suppression compares the confirmed pair. */
static void gain_machine_emit(struct udsp *s, struct udsp_chan *c)
{
	struct chan_batch mb = { .count = 0, .overflow = false };

	gain_emit(c, &mb);
	batch_flush(s, &mb, 0, NULL);
}

/* --- playout ------------------------------------------------------------- */

static void playout_stop(struct udsp_playout *p)
{
	memset(p, 0, sizeof(*p));
}

static void tone_desc_steady(struct udsp_tone_desc *d)
{
	d->cadence_count = 0;
	d->repeat = true;
}

static uint32_t tone_cycle_ms(const struct udsp_tone_desc *d)
{
	uint32_t sum = 0;

	for (unsigned i = 0; i < d->cadence_count; i++)
		sum += d->cadence[i].make_ms + d->cadence[i].break_ms;

	return sum;
}

static void playout_tone_start(struct udsp_playout *p,
			       const struct udsp_tone_desc *desc,
			       uint32_t duration_ms, uint16_t repeat_count,
			       bool then_steady, unsigned rate)
{
	playout_stop(p);
	p->rate = rate;
	p->desc = *desc;
	p->cycle_ms = tone_cycle_ms(&p->desc);
	/* The generator has no finite-count field (line.h), so the engine owns
	 * the count: the descriptor always repeats and the limit stops it. */
	p->desc.repeat = true;
	p->limit_ms = (repeat_count && p->cycle_ms) ?
		      (uint32_t)repeat_count * p->cycle_ms : 0;
	p->then_steady = then_steady && p->limit_ms;
	p->duration_ms = duration_ms;
	if (!udsp_tone_generator_init(&p->tone, &p->desc, p->rate))
		return;
	p->kind = UDSP_PLAYOUT_TONE;
}

/*
 * A frequency that the previous segment also played keeps its oscillator
 * phase, whatever its index, so the boundary adds no phase step. A level step
 * still clicks. After a silence, each burst starts at phase 0, as the first
 * burst does.
 */
static void playout_phase_carry(struct udsp_playout *p,
				const struct udsp_tone_desc *was,
				const struct udsp_oscillator *osc)
{
	for (unsigned i = 0; i < p->desc.frequency_count; i++) {
		for (unsigned k = 0; k < was->frequency_count; k++) {
			if (was->frequency_hz[k] != p->desc.frequency_hz[i])
				continue;
			p->tone.oscillator[i] = osc[k];
			break;
		}
	}
}

/* An off segment plays as a step with no frequency: in_silence stays the mark
 * of the trailing silence alone, which playout_segments_advance keys on. */
static void playout_segment_enter(struct udsp_playout *p)
{
	const struct udsp_region_segment *seg = &p->segment[p->segment_index];
	struct udsp_oscillator osc[UDSP_TONE_FREQ_MAX];
	struct udsp_tone_desc was = p->desc;

	if (p->in_silence)
		was.frequency_count = 0;
	memcpy(osc, p->tone.oscillator, sizeof(osc));
	memset(&p->desc, 0, sizeof(p->desc));
	p->desc.tone = UDSP_TONE_SIT;
	p->desc.frequency_count = seg->freq_count;
	for (unsigned i = 0; i < seg->freq_count; i++) {
		p->desc.frequency_hz[i] = seg->freq_hz[i];
		p->desc.level_dbm0[i] = seg->level_dbm0;
	}
	p->desc.repeat = true;
	udsp_tone_generator_init(&p->tone, &p->desc, p->rate);
	playout_phase_carry(p, &was, osc);
	p->in_silence = false;
	p->phase_left = (uint32_t)seg->duration_ms * (p->rate / 1000u);
}

static void playout_segments_start(struct udsp_playout *p,
				   const struct udsp_region_tone *t,
				   uint32_t duration_ms, unsigned rate)
{
	playout_stop(p);
	p->rate = rate;
	p->segment_count = t->segment_count;
	memcpy(p->segment, t->segment, sizeof(p->segment));
	p->silence_ms = t->silence_ms;
	p->then_steady = t->then_steady && t->repeat_count;
	p->cycles_left = t->repeat_count;
	p->duration_ms = duration_ms;
	if (!p->segment_count)
		return;
	p->segment_index = 0;
	playout_segment_enter(p);
	p->kind = UDSP_PLAYOUT_SEGMENTS;
}

static void playout_digits_start(struct udsp_playout *p, const char *digits,
				 unsigned count, int level_dbm0, unsigned on_ms,
				 unsigned off_ms, unsigned rate)
{
	playout_stop(p);
	p->rate = rate;
	if (!count || count > LINE_CID_DTMF_MAX)
		return;
	memcpy(p->digits, digits, count);
	p->digit_count = (uint8_t)count;
	p->level_dbm0 = (int8_t)level_dbm0;
	p->on_ms = (uint16_t)on_ms;
	p->off_ms = (uint16_t)off_ms;
	p->digit_index = 0;
	p->in_on = true;
	p->phase_left = (uint32_t)on_ms * (p->rate / 1000u);
	udsp_dtmf_generator_init(&p->dtmf, p->digits[0], level_dbm0, p->rate);
	p->kind = UDSP_PLAYOUT_DIGITS;
}

static void playout_fsk_start(struct udsp_playout *p, const struct line_cmd *cmd,
			      unsigned rate)
{
	playout_stop(p);
	p->rate = rate;
	udsp_fsk_mod_init(&p->fsk, (enum udsp_fsk_std)cmd->u.fsk.std,
			  cmd->u.fsk.level_dbm0, cmd->u.fsk.seizure_bits,
			  cmd->u.fsk.mark_bits, p->rate);
	if (!udsp_fsk_mod_data(&p->fsk, cmd->u.fsk.bytes, cmd->u.fsk.len))
		return;
	p->kind = UDSP_PLAYOUT_FSK;
}

static bool playout_active(const struct udsp_playout *p)
{
	return p->kind != UDSP_PLAYOUT_NONE;
}

static bool playout_tone_render(struct udsp_playout *p, int16_t *out, unsigned n)
{
	bool active;

	if (p->limit_ms && p->elapsed_ms >= p->limit_ms) {
		if (!p->then_steady || p->steady)
			return false;
		/* [tone] then = on: the finite pattern runs out and the tone
		 * continues steadily on the last frequency set (line.h). */
		p->steady = true;
		p->limit_ms = 0;
		tone_desc_steady(&p->desc);
		udsp_tone_generator_init(&p->tone, &p->desc, p->rate);
	}
	active = udsp_tone_generator_render(&p->tone, out, n);
	p->elapsed_ms += UDSP_TICK_MS;

	return active;
}

static bool playout_segments_advance(struct udsp_playout *p)
{
	if (!p->in_silence && p->segment_index + 1u < p->segment_count) {
		p->segment_index++;
		playout_segment_enter(p);
		return true;
	}
	if (!p->in_silence && p->silence_ms) {
		p->in_silence = true;
		p->phase_left = (uint32_t)p->silence_ms * (p->rate / 1000u);
		return true;
	}
	if (p->cycles_left && !--p->cycles_left) {
		if (!p->then_steady)
			return false;
		p->steady = true;
		p->segment_index = (uint8_t)(p->segment_count - 1u);
		playout_segment_enter(p);
		p->phase_left = UINT32_MAX;
		return true;
	}
	p->segment_index = 0;
	playout_segment_enter(p);

	return true;
}

static bool playout_segments_render(struct udsp_playout *p, int16_t *out,
				    unsigned n)
{
	unsigned pos = 0;

	while (pos < n) {
		unsigned k;

		if (!p->phase_left && !playout_segments_advance(p)) {
			memset(out + pos, 0, (n - pos) * sizeof(*out));
			p->elapsed_ms += UDSP_TICK_MS;
			return false;
		}
		k = n - pos;
		if (p->phase_left < k)
			k = p->phase_left;
		if (p->in_silence)
			memset(out + pos, 0, k * sizeof(*out));
		else
			udsp_tone_generator_render(&p->tone, out + pos, k);
		if (p->phase_left != UINT32_MAX)
			p->phase_left -= k;
		pos += k;
	}
	p->elapsed_ms += UDSP_TICK_MS;

	return true;
}

static bool playout_digits_advance(struct udsp_playout *p)
{
	if (p->in_on && p->off_ms) {
		p->in_on = false;
		p->phase_left = (uint32_t)p->off_ms * (p->rate / 1000u);
		return true;
	}
	p->digit_index++;
	if (p->digit_index >= p->digit_count)
		return false;
	p->in_on = true;
	p->phase_left = (uint32_t)p->on_ms * (p->rate / 1000u);
	udsp_dtmf_generator_init(&p->dtmf, p->digits[p->digit_index],
				 p->level_dbm0, p->rate);

	return true;
}

static bool playout_digits_render(struct udsp_playout *p, int16_t *out,
				  unsigned n)
{
	unsigned pos = 0;

	while (pos < n) {
		unsigned k;

		if (!p->phase_left && !playout_digits_advance(p)) {
			memset(out + pos, 0, (n - pos) * sizeof(*out));
			p->elapsed_ms += UDSP_TICK_MS;
			return false;
		}
		k = n - pos;
		if (p->phase_left < k)
			k = p->phase_left;
		if (p->in_on)
			udsp_dtmf_generator_render(&p->dtmf, out + pos, k);
		else
			memset(out + pos, 0, k * sizeof(*out));
		p->phase_left -= k;
		pos += k;
	}
	p->elapsed_ms += UDSP_TICK_MS;

	return true;
}

/* Slot B overrides slot A, and slot A pauses with its remaining duration
 * frozen until B ends. The pause is set here because the rule that picks the
 * live slot is the rule that pauses the other one. */
static struct udsp_playout *playout_live_get(struct udsp_chan *c)
{
	struct udsp_playout *api = &c->slot[UDSP_SLOT_API];
	struct udsp_playout *seq = &c->slot[UDSP_SLOT_SEQ];
	bool is_seq = playout_active(seq);

	api->paused = is_seq && playout_active(api);
	if (is_seq)
		return seq;

	return playout_active(api) ? api : NULL;
}

static bool playout_kind_render(struct udsp_playout *p, int16_t *out,
				unsigned n)
{
	switch (p->kind) {
	case UDSP_PLAYOUT_TONE:
		return playout_tone_render(p, out, n);
	case UDSP_PLAYOUT_SEGMENTS:
		return playout_segments_render(p, out, n);
	case UDSP_PLAYOUT_DIGITS:
		return playout_digits_render(p, out, n);
	case UDSP_PLAYOUT_FSK:
		memset(out, 0, n * sizeof(*out));
		p->elapsed_ms += UDSP_TICK_MS;
		/* udsp_fsk_mod_run adds into the frame by contract. */
		return udsp_fsk_mod_run(&p->fsk, out, n);
	default:
		break;
	}
	memset(out, 0, n * sizeof(*out));

	return false;
}

/* out is filled whole; the return says whether the slot survives this frame.
 * The answer is latched: a segmented cadence that runs out starts over on the
 * render after it, and the frames a skip renders and throws away must not
 * reach that (chan_playout_skip). */
static bool playout_render(struct udsp_playout *p, int16_t *out, unsigned n)
{
	if (p->done || (p->duration_ms && p->elapsed_ms >= p->duration_ms)) {
		memset(out, 0, n * sizeof(*out));
		p->done = true;
		return false;
	}
	if (playout_kind_render(p, out, n))
		return true;

	p->done = true;

	return false;
}

/* --- region tone selection ----------------------------------------------- */

static void chan_tone_desc_play(struct udsp_chan *c,
				const struct udsp_region_tone *t,
				uint32_t duration_ms)
{
	if (!t->present)
		return;
	if (t->segmented)
		playout_segments_start(&c->slot[UDSP_SLOT_API], t, duration_ms,
				       c->rate);
	else
		playout_tone_start(&c->slot[UDSP_SLOT_API], &t->desc, duration_ms,
				   t->repeat_count, t->then_steady, c->rate);
}

static void chan_tone_start(struct udsp *s, struct udsp_chan *c,
			    enum udsp_tone tone, uint32_t duration_ms)
{
	const struct udsp_region *r = udsp_region_active(s);

	if (!r || (unsigned)tone >= UDSP_REGION_TONE_MAX)
		return;
	chan_tone_desc_play(c, &r->tone[tone], duration_ms);
}

/* variant 0, a variant above the defined count, or a variant whose row the
 * region omits all fall back to the plain call-waiting tone. */
static void chan_cw_tone_start(struct udsp *s, struct udsp_chan *c,
			       unsigned variant, uint32_t duration_ms)
{
	const struct udsp_region *r = udsp_region_active(s);
	const struct udsp_region_tone *t;

	if (!r)
		return;
	if (variant == 0 || variant > r->cw_count || !r->cw[variant - 1].present)
		t = &r->tone[UDSP_TONE_CALL_WAITING];
	else
		t = &r->cw[variant - 1];
	chan_tone_desc_play(c, t, duration_ms);
}

/*
 * A finished sequencer playout is still in the playback pipeline: the tick
 * writes prefill_periods ahead of the highway, so the last frame the modulator
 * rendered reaches the line that many ticks later. The sequencer restores the
 * linefeed on burst_done, and the SLIC mutes an on-hook line in the active
 * feed. A burst_done at render time would cut the tail of every type 1
 * message, the checksum with it, and every terminal would reject the record.
 * So burst_done waits for the pipeline to drain. The margin covers the
 * driver's period quantisation and the AFE FIFO.
 */
#define CHAN_SEQ_DRAIN_MARGIN	3u

static void chan_seq_drain_arm(struct udsp *s, struct udsp_chan *c)
{
	c->seq_drain = (uint8_t)(s->cfg.prefill_periods + CHAN_SEQ_DRAIN_MARGIN);
}

static void chan_seq_drain_tick(struct udsp_chan *c)
{
	if (!c->seq_drain)
		return;
	if (--c->seq_drain == 0)
		line_cidseq_burst_done(&c->cid);
}

/* --- journal execution --------------------------------------------------- */


static void chan_cmd_apply(struct udsp *s, struct udsp_chan *c,
			   struct chan_batch *b, const struct line_cmd *cmd)
{
	struct udsp_playout *seq = &c->slot[UDSP_SLOT_SEQ];

	switch (cmd->type) {
	case LINE_CMD_LINEFEED:
		/* A sequence end restores the base feed the engine holds now,
		 * not the one the sequence armed with: chan_base_lf_set defers
		 * a line state request while a sequence owns the line, and this
		 * write is where that request takes effect. */
		lf_emit(c, b, cmd->u.linefeed.restore ? c->base_lf :
						       cmd->u.linefeed.state);
		/* A sequence that ends on the tick has no ack for this write.
		 * The restore stays due until it confirms
		 * (chan_feed_restore). */
		if (cmd->u.linefeed.restore)
			c->fault_restore = true;
		return;
	case LINE_CMD_POLARITY: {
		uint8_t lf = lf_polar(c->base_lf, cmd->u.polarity.state);

		/* The engine owns base_lf, so it resolves the variant and the
		 * slic thread stays a dumb executor. The record is a linefeed
		 * write, so the hold gate applies. */
		if (lf_allowed(c, lf))
			lf_rec_emit(c, b, SLIC_REC_POLARITY, lf);
		return;
	}
	case LINE_CMD_RING_RELAY:
		/* A ring into a held or recovering line is dropped like a
		 * powered feed write. The relay-off of a ring stop still runs,
		 * because the relay must open. The vendor ring stop writes the
		 * active feed, which lf_confirmed does not track, so a held
		 * line is put back open and lf_confirmed follows it. */
		if (cmd->u.relay.on && (c->fault_held || c->fault_reinit))
			return;
		lf_rec_emit(c, b, SLIC_REC_RING_RELAY, cmd->u.relay.on);
		if (!cmd->u.relay.on && (c->fault_held || c->fault_reinit))
			lf_emit(c, b, UDSP_LF_OPEN);
		return;
	case LINE_CMD_TONE:
		playout_tone_start(seq, &cmd->u.tone.desc, cmd->u.tone.duration_ms,
				   0, false, c->rate);
		return;
	case LINE_CMD_TONE_STOP:
		/* One command covers tone, FSK and DTMF alike: cidseq has one
		 * audio_on flag. A drain in flight belongs to the playout this
		 * stops, never to the next one. */
		c->seq_drain = 0;
		playout_stop(seq);
		return;
	case LINE_CMD_FSK:
		c->seq_drain = 0;
		playout_fsk_start(seq, cmd, c->rate);
		return;
	case LINE_CMD_DTMF:
		c->seq_drain = 0;
		playout_digits_start(seq, cmd->u.dtmf.digits, cmd->u.dtmf.count,
				     cmd->u.dtmf.level_dbm0, cmd->u.dtmf.on_ms,
				     cmd->u.dtmf.off_ms, c->rate);
		return;
	case LINE_CMD_MUTE:
		atomic_store_explicit(&c->mute, cmd->u.mute.on,
				      memory_order_release);
		return;
	default:
		return;
	}
}

static void chan_journal_run(struct udsp *s, struct udsp_chan *c,
			     struct chan_batch *b)
{
	if (c->out.dropped)
		udsp_op_error(s);

	for (unsigned i = 0; i < c->out.count; i++) {
		const struct line_rec *rec = &c->out.rec[i];

		if (rec->kind == LINE_REC_CMD) {
			chan_cmd_apply(s, c, b, &rec->u.cmd);
			continue;
		}
		chan_event_post(s, &rec->u.event);
		/* The ring machine owns the relay; base_lf is what the end of
		 * a session restores. */
		if (rec->u.event.type == UDSP_EV_RING_END &&
		    lf_confirmed(c) != c->base_lf)
			lf_emit(c, b, c->base_lf);
	}
	line_out_reset(&c->out);
	/* Every path that starts or ends a ring session or a burst ends here,
	 * so this is the one place that state reaches the other threads. */
	atomic_store_explicit(&c->owned,
			      line_ring_active(&c->ring) ||
			      line_cidseq_busy(&c->cid),
			      memory_order_release);
}

/* --- control commands ---------------------------------------------------- */

/*
 * A command drives the machines with the current tick stamp, while a hook edge
 * carries the stamp of the tick it was sampled in, which is older. The machines
 * must never see time run backwards (line.h section 1), so a command raises the
 * line's stamp high-water mark and the edge replay clamps up to it.
 */
static void chan_stamp_mark(struct udsp_chan *c, uint32_t now_ms)
{
	c->last_stamp_ms = now_ms;
}

static uint8_t chan_state_lf(const struct udsp_chan *c, enum udsp_line_state st)
{
	switch (st) {
	case UDSP_LINE_DOWN:
		return UDSP_LF_OPEN;
	case UDSP_LINE_OHT:
		return lf_polar(UDSP_LF_FWD_OHT, c->polarity);
	default:
		/* BUSY and HOWLER are the active feed: the core plays those
		 * tones itself (uvoice line.c uses DOWN, ACTIVE and OHT). */
		return lf_polar(UDSP_LF_FWD_ACTIVE, c->polarity);
	}
}

static void chan_base_lf_set(struct udsp *s, struct udsp_chan *c,
			     struct chan_batch *b, uint8_t lf)
{
	c->base_lf_wanted = lf;
	/* A held or recovering line keeps its open feed whatever the caller
	 * asks. The lift of the hold or the DONE of the recovery applies the
	 * last request. */
	if (c->fault_held || c->fault_reinit)
		return;
	c->base_lf = lf;
	/* A ring session owns the line while it runs, so the new feed is what
	 * its end restores instead of cutting the burst here. */
	if (line_ring_active(&c->ring) || line_cidseq_busy(&c->cid))
		return;
	if (lf_confirmed(c) == lf)
		return;
	lf_emit(c, b, lf);
}

/* Only a successful arm moves cid_arm_seq. A refused arm keeps the value of
 * the running sequence, so a failed write of that sequence still aborts it. */
static int chan_cid_arm(struct udsp_chan *c, enum udsp_cid_kind kind,
			const struct udsp_cid *cid, const struct udsp_region *r,
			bool wait_ring, enum line_cid_mwi mwi, uint32_t wait_ms)
{
	int rc = line_cidseq_arm(&c->cid, kind, cid, r,
				 (enum udsp_linefeed)c->base_lf, wait_ring, mwi,
				 wait_ms);

	if (!rc)
		c->cid_arm_seq = c->lf_seq_emit;

	return rc;
}

static int chan_ring_start(struct udsp *s, struct udsp_chan *c,
			   const struct udsp_ring *ring, uint32_t now_ms)
{
	const struct udsp_region *r = udsp_region_active(s);
	int rc;

	if (ring->cid_present && !r)
		return -ENOTSUP;

	rc = line_ring_start(&c->ring, ring, now_ms, &c->gate, &c->out);
	if (rc)
		return rc;
	if (!ring->cid_present)
		return 0;

	return chan_cid_arm(c, UDSP_CID_TYPE1, &ring->cid, r, true,
			    LINE_CID_MWI_NONE, ring->cid_wait_ms);
}

/* VMWI: mwi true is the visual indicator on burst; a record with mwi false and
 * number, name and blockinfo all empty is the off burst, which is exactly what
 * uvoice mwi_fsk_send builds; anything else is a caller identity. */
static enum line_cid_mwi chan_cid_mwi(const struct udsp_cid *cid)
{
	if (cid->mwi)
		return LINE_CID_MWI_ON;
	if (!cid->number[0] && !cid->name[0] && !cid->blockinfo[0])
		return LINE_CID_MWI_OFF;

	return LINE_CID_MWI_NONE;
}

static int chan_cid_send(struct udsp *s, struct udsp_chan *c,
			 enum udsp_cid_kind kind, const struct udsp_cid *cid)
{
	const struct udsp_region *r = udsp_region_active(s);
	enum line_cid_mwi mwi = chan_cid_mwi(cid);
	bool wait_ring;

	if (!r)
		return -ENOTSUP;
	wait_ring = mwi != LINE_CID_MWI_NONE ? line_ring_active(&c->ring) :
		    (kind == UDSP_CID_TYPE1 && line_ring_active(&c->ring));

	/* A standalone burst has no ring break to delay against, so the region
	 * pause stands; only udsp_ring carries a caller delay. */
	return chan_cid_arm(c, kind, cid, r, wait_ring, mwi, 0);
}

/*
 * What the line does with a digit of its own, wherever the digit comes from.
 * The acknowledgement is stamped at the digit start: a digit inside the window
 * stays valid when the deciding tick comes later (line.h).
 */
static void chan_digit_post(struct udsp *s, struct udsp_chan *c,
			    struct udsp_event *ev, uint32_t now_ms)
{
	ev->line = (uint16_t)c->line;
	ev->stamp_ms = now_ms;
	ev->u.digit.remote = 0;
	chan_event_post(s, ev);
	line_cidseq_dtmf_ack(&c->cid, now_ms - ev->u.digit.duration_ms,
			     ev->u.digit.digit);
}

/* The digit the detector decided but has not ended yet leaves here. Nothing
 * else emits it, so a digit left inside the detector is one the caller never
 * hears, and one the caller identity sequence never counts: the flush ends a
 * real digit, so it acknowledges the sequence as the deciding tick does. */
static void chan_dtmf_flush(struct udsp *s, struct udsp_chan *c,
			    uint32_t now_ms)
{
	struct udsp_event ev;

	if (!udsp_dtmf_detector_flush(&c->detect, &ev))
		return;

	chan_digit_post(s, c, &ev, now_ms);
}

/* Each feed resets its own instance on arm (dsp/resamp.h), so an arm never
 * carries the last signal's tail into the first frame of this one. */
static void chan_dtmf_arm(struct udsp_chan *c)
{
	udsp_dtmf_detector_init(&c->detect, false);
	udsp_resamp_reset(&c->rs_ul_pre);
}

static uint32_t chan_lec_ref_guard(uint32_t loop, unsigned frame);

/*
 * The tail of the hold a digit playing toward the line takes, re-armed on
 * every tick the digit renders. The tone comes back as echo, so the tail must
 * outlast the loop, and four ticks cover the span the detector decides over.
 * The arm reads the loop the engine last measured, not the applied reference
 * of a canceller. The echo crosses that loop whether or not the line runs a
 * canceller. The tolerance can also keep the applied reference one period
 * away from the loop. The guard and the clamp of the reference still apply,
 * so a reference that stands on the latest window gives the same tail. A
 * converged canceller is not enough on its own, because it subtracts its
 * estimate after the feed the detector reads (chan_ul).
 */
void chan_digit_hold_arm(struct udsp *s, struct udsp_chan *c)
{
	uint32_t loop = (uint32_t)atomic_load_explicit(&s->lec_loop,
						       memory_order_relaxed);
	unsigned delay, ticks;

	delay = chan_lec_delay_clamp(chan_lec_ref_guard(loop, c->frame),
				     c->rate, c->frame);
	ticks = delay / c->frame + 4u;
	if (ticks < UDSP_DL_DIGIT_HOLD_TICKS)
		ticks = UDSP_DL_DIGIT_HOLD_TICKS;
	if (ticks > UINT8_MAX)
		ticks = UINT8_MAX;
	c->dl_digit_hold = (uint8_t)ticks;
}

/*
 * The line holds its DTMF detector while a stream of it carries a data
 * carrier. A fax or a modem carrier is not a digit, so every decision the
 * detector makes over one is talk-off. The digit it reports reaches the
 * caller. The caller relays it as an RFC 4733 event, and those packets replace
 * the carrier frames the far end demodulates.
 *
 * A digit playing toward the line holds it for the same reason: the tone the
 * line plays is not a digit the subscriber dialled, and a detector that read
 * the echo of it would report it and the relay would put it back on the wire.
 * The type 2 acknowledgement window is the one carve-out, because the handset
 * answers the alerting tone with a digit of its own and the line must hear it;
 * the risk of a relayed echo inside that 300 ms window is accepted.
 *
 * The entry flushes, as the disarm does: a digit the detector already decided
 * belongs to the speech before the carrier. The exit arms the detector again,
 * because the window holds carrier samples and no digit may come out of them.
 *
 * The hold follows the line, because the detector is the line's. A second call
 * on a line whose other leg carries a fax therefore detects no digit until
 * that leg ends or goes on hold.
 *
 * A carrier the tone disabler latched onto holds the detector for the same
 * reason a described one does: the talk-off is a property of the signal on the
 * line, not of who named it.
 */
static bool chan_dtmf_hold(struct udsp *s, struct udsp_chan *c, uint32_t now_ms)
{
	bool hold = chan_passthrough_get(s, c) != UDSP_PASSTHROUGH_OFF ||
		    (c->dl_digit_hold && !line_cidseq_ack_open(&c->cid));

	if (hold == c->dtmf_held)
		return hold;

	c->dtmf_held = hold;
	if (hold)
		chan_dtmf_flush(s, c, now_ms);
	else
		chan_dtmf_arm(c);

	return hold;
}

/*
 * The digit this tick carries, the edge that opened it and its row level. The
 * detector reports a digit only once it has ended, so the digit it holds from
 * the validating decision on is what the uplink mute and the relay key on. Both
 * belong to the line and not to a stream: a stream that becomes active in the
 * middle of a digit finds the digit and no start of its own, and relays
 * nothing for it.
 */
static void chan_digit_track(struct udsp_chan *c)
{
	char digit = udsp_dtmf_detector_active(&c->detect);

	c->digit_started = digit && digit != c->digit_active;
	c->digit_active = digit;
	if (c->digit_started)
		c->digit_level_dbm0 =
			(int8_t)udsp_dtmf_detector_active_level(&c->detect);
}

/*
 * The region owns pulse classification. Its break window overlaps the flash
 * window in most European profiles, and only the region knows which of the two
 * the country uses. A caller can turn pulses off on a line, but it cannot turn
 * a recall into a digit. Before the first configuration, the region value
 * stands alone, as line_hook_init set it at the reset. Without a region, no
 * country chose a break window, so pulses stay off.
 */
void chan_hook_pulse_apply(struct udsp_chan *c, const struct udsp_region *r)
{
	bool on = r && r->hook.pulse_enable;

	if (c->cfg_applied && !(c->cfg.detect_mask & UDSP_DET_PULSE))
		on = false;
	line_hook_pulse_enable(&c->hook, on);
}

/*
 * The split of the receive gain of the port (chan.h dl_voice_gain). A loss of
 * slic_gain_rx moves ahead of the playout sum, so a local signal reaches the
 * line at its own level, and only a gain reaches the SLIC. The fields follow
 * the configuration at once, while the SLIC write runs later on the slic
 * thread. For a positive slic_gain_rx the local trim therefore leads the part
 * for that window, as the voice gain does.
 */
static void chan_rx_split_apply(struct udsp_chan *c)
{
	int rx = c->cfg.slic_gain_rx;
	int loss = rx < 0 ? -rx : 0;

	c->dl_voice_gain = (int16_t)(c->cfg.gain_rx - loss);
	c->dl_local_gain = (int16_t)-udsp_slic_rx_programmed((int8_t)rx);
	c->dl_ref_loss = (int16_t)loss;
}

static void chan_config_apply(struct udsp *s, struct udsp_chan *c,
			      const struct udsp_chan_cfg *cfg)
{
	bool want = (cfg->detect_mask & UDSP_DET_DTMF) != 0;

	bool fax = (cfg->detect_mask & UDSP_DET_MODEM) != 0;

	c->cfg = *cfg;
	c->cfg_applied = true;
	chan_rx_split_apply(c);
	/* Before the first LINES_UP the part is uninitialised: the pair is
	 * staged only and LINES_UP programs it. The write is a machine record,
	 * so the command always acks at the tick and no gain errno reaches the
	 * caller. */
	if (c->ready)
		gain_machine_emit(s, c);
	chan_hook_pulse_apply(c, udsp_region_active(s));
	if (fax && !c->fax_armed) {
		udsp_fax_detector_init(&c->fax_local, UDSP_TONE_DIR_LOCAL);
		udsp_fax_detector_init(&c->fax_remote, UDSP_TONE_DIR_REMOTE);
		/* Each feed resets its own instance on arm (dsp/resamp.h), so
		 * a re-arm never carries the last call's tail into the first
		 * frame of this one. */
		udsp_resamp_reset(&c->rs_ul_post);
		udsp_resamp_reset(&c->rs_dl);
	}
	/* Disarming MODEM leaves no detector to report the end of the carrier
	 * or to measure the release, so a freeze the local detector raised
	 * would hold the canceller for ever and a latch would keep it off the
	 * line for ever; both die with the arm. */
	if (c->fax_armed != fax) {
		c->fax_active = false;
		chan_fax_latch_clear(c);
	}
	c->fax_armed = fax;
	if (want == c->dtmf_armed)
		return;
	if (want) {
		chan_dtmf_arm(c);
		c->dtmf_armed = true;
		return;
	}
	chan_dtmf_flush(s, c, udsp_now_ms(s));
	c->dtmf_armed = false;
	/* The hold is a record of what the tick last did to a detector that
	 * runs, so the disarm takes it with it: the arm that follows gets a
	 * fresh detector and the tick decides the hold again from the line. */
	c->dtmf_held = false;
}

static void chan_lines_up(struct udsp *s, struct chan_batch *b, uint32_t mask,
			  uint32_t now_ms)
{
	for (unsigned i = 0; i < udsp_lines(s); i++) {
		struct udsp_chan *c = &s->chan[i];

		if (!(mask & (1u << i)))
			continue;
		chan_stamp_mark(c, now_ms);
		c->ready = true;
		c->dead_done = false;
		/* The start ends every hold and every open state: init reset
		 * the part. fault_reported stands, because the clear is still
		 * due for a fault that was posted before the stop. */
		c->fault_open = false;
		c->fault_until_ms = 0;
		c->fault_read_bits = 0;
		c->fault_reinit = false;
		c->fault_restore = false;
		c->fault_held = false;
		c->fault_held_reason = UDSP_FAULT_HELD_NONE;
		chan_fault_publish(c);
		/* A line the consumer holds down comes up open; SLEEP and OHT
		 * requests come back as the active feed and the consumer's
		 * re-derive settles them. The write is unconditional either
		 * way: init left the part open whatever lf_confirmed still
		 * says, which is stale after a hook-read death. */
		if (c->down_wanted) {
			c->state = UDSP_LINE_DOWN;
			c->base_lf = UDSP_LF_OPEN;
		} else {
			c->state = UDSP_LINE_ACTIVE;
			c->base_lf = lf_polar(UDSP_LF_FWD_ACTIVE, c->polarity);
		}
		c->base_lf_wanted = c->base_lf;
		lf_emit(c, b, c->base_lf);
		/* Init reset the gain RAM to the preset's 0 dB plan and
		 * slic_job_start invalidated the confirmed record, so this
		 * always programs the configured pair. The machine emit keeps
		 * the start job's acked group contiguous and the job never
		 * fails over the gain. */
		gain_machine_emit(s, c);
	}
}

static void chan_lines_safe(struct udsp *s, struct chan_batch *b, uint32_t now_ms)
{
	for (unsigned i = 0; i < udsp_lines(s); i++) {
		struct udsp_chan *c = &s->chan[i];

		/* The loop is engine state and no write of the part, so it goes
		 * before the dead-line skip: a line that died with a loop armed
		 * would otherwise carry it, and its control-side record, into
		 * the next start. */
		chan_loop_disarm(s, c);
		/* A dead line answers -EIO to every write, and a stop must not
		 * fail because one channel of a dual part is unpopulated. */
		if (atomic_load_explicit(&c->dead, memory_order_acquire))
			continue;
		chan_stamp_mark(c, now_ms);
		line_ring_stop(&c->ring, now_ms, LINE_RING_STOPPED, &c->gate,
			       &c->out);
		line_cidseq_abort(&c->cid, -ECANCELED, now_ms, &c->gate, &c->out);
		chan_journal_run(s, c, b);
		playout_stop(&c->slot[UDSP_SLOT_API]);
		playout_stop(&c->slot[UDSP_SLOT_SEQ]);
		c->base_lf = UDSP_LF_OPEN;
		c->base_lf_wanted = UDSP_LF_OPEN;
		c->fault_restore = false;
		c->state = UDSP_LINE_DOWN;
		if (lf_confirmed(c) != UDSP_LF_OPEN)
			lf_emit(c, b, UDSP_LF_OPEN);
	}
}

/* The commands that start a session or a playout. The guard admits them on a
 * live line before they are queued, so the tick refuses one whose line died in
 * between: the death cleanup already ran (chan_line_dead), and a session
 * started now would own a line nothing can drive. Every other command
 * releases, or records a request the line's return applies. */
static bool chan_cmd_starts(uint8_t type)
{
	switch (type) {
	case UDSP_CMD_RING_START:
	case UDSP_CMD_CID_SEND:
	case UDSP_CMD_TONE_PLAY:
	case UDSP_CMD_CW_TONE_PLAY:
	case UDSP_CMD_DIGIT_PLAY:
		return true;
	default:
		return false;
	}
}

static int chan_cmd_run(struct udsp *s, const struct udsp_cmd *cmd,
			struct chan_batch *b, uint32_t now_ms)
{
	struct udsp_chan *c = &s->chan[cmd->line < UDSP_LINE_MAX ? cmd->line : 0];

	chan_stamp_mark(c, now_ms);
	if (chan_cmd_starts(cmd->type) &&
	    atomic_load_explicit(&c->dead, memory_order_acquire))
		return -ENODEV;

	switch (cmd->type) {
	case UDSP_CMD_LINE_STATE: {
		enum udsp_line_state st = (enum udsp_line_state)cmd->arg0;
		bool sleep = st == UDSP_LINE_SLEEP;

		c->down_wanted = st == UDSP_LINE_DOWN;
		chan_base_lf_set(s, c, b, chan_state_lf(c, st));
		if (s->ops->powersave &&
		    sleep != atomic_load_explicit(&c->ps_confirmed,
						  memory_order_acquire))
			batch_add(b, SLIC_REC_POWERSAVE, c->line, sleep);
		c->sleeping = sleep;
		c->state = (uint8_t)st;
		return 0;
	}
	case UDSP_CMD_POLARITY:
		c->polarity = (uint8_t)cmd->arg0;
		chan_base_lf_set(s, c, b,
				 lf_polar(c->base_lf_wanted, c->polarity));
		return 0;
	case UDSP_CMD_HOOK_TIMING:
		line_hook_timing_set(&c->hook, cmd->arg0, cmd->arg1,
				     (uint32_t)cmd->arg2);
		c->core_windows = true;
		return 0;
	case UDSP_CMD_RING_START:
		return chan_ring_start(s, c, &cmd->u.ring, now_ms);
	case UDSP_CMD_RING_STOP:
		line_ring_stop(&c->ring, now_ms, LINE_RING_STOPPED, &c->gate,
			       &c->out);
		line_cidseq_abort(&c->cid, -ECANCELED, now_ms, &c->gate, &c->out);
		return 0;
	case UDSP_CMD_CID_SEND:
		return chan_cid_send(s, c, (enum udsp_cid_kind)cmd->u.cid.kind,
				     &cmd->u.cid.cid);
	case UDSP_CMD_MWI_LAMP:
		/* SetMWIState drives the feed on the real part, so a lamp
		 * request is dropped on a held or recovering line like a
		 * powered feed write. */
		if (c->fault_held || c->fault_reinit)
			return 0;
		batch_add(b, SLIC_REC_MWI_LAMP, c->line, cmd->arg0);
		return 0;
	case UDSP_CMD_TONE_PLAY:
		chan_tone_start(s, c, (enum udsp_tone)cmd->arg0, cmd->arg1);
		return 0;
	case UDSP_CMD_CW_TONE_PLAY:
		chan_cw_tone_start(s, c, cmd->arg0, cmd->arg1);
		return 0;
	case UDSP_CMD_TONE_STOP:
		playout_stop(&c->slot[UDSP_SLOT_API]);
		return 0;
	case UDSP_CMD_DIGIT_PLAY: {
		char digit = (char)cmd->arg0;

		playout_digits_start(&c->slot[UDSP_SLOT_API], &digit, 1,
				     LINE_CID_DTMF_LEVEL_DBM0, cmd->arg1, 0,
				     c->rate);
		return c->slot[UDSP_SLOT_API].kind ? 0 : -EINVAL;
	}
	case UDSP_CMD_CHAN_CONFIG:
		chan_config_apply(s, c, &cmd->u.chan);
		return 0;
	case UDSP_CMD_STREAM:
		return stream_cmd_run(s, cmd, now_ms);
	case UDSP_CMD_LEC_LATCH_CLEAR:
		if (chan_fax_latch_get(c) == UDSP_PASSTHROUGH_OFF)
			return 0;
		/* fax_active follows the detector's own edges: a clear while
		 * a local tone still sounds must not let the filter adapt
		 * against that tone. */
		chan_fax_latch_clear(c);
		chan_lec_policy_report(s, c, UDSP_PASSTHROUGH_OFF,
				       UDSP_LEC_REASON_RELEASE_CALLER, 0, 0,
				       now_ms);
		return 0;
	case UDSP_CMD_LEC_RELEASE: {
		struct udsp_ec *cur = atomic_load_explicit(&c->ec,
							   memory_order_acquire);

		/* A release that finds the mailbox occupied leaves ec live for
		 * the next reclaim (stream.c stream_lec_release). A release
		 * that timed out thus cannot overwrite an unharvested instance.
		 * Only the tick stores a non-NULL pointer there, so a mailbox
		 * that reads empty stays empty until the store below. */
		if (!cur || atomic_load_explicit(&c->ec_retired,
						 memory_order_relaxed))
			return 0;
		/*
		 * The drop reads the instance for its END record. It therefore
		 * runs before the store that makes the instance visible.
		 * stream_lec_reclaim harvests the mailbox after an expired wait
		 * without the ack, and the control thread frees at once. The
		 * drop also runs before the null, because stream_lec_ensure
		 * can store a new instance at the same address once ec reads
		 * NULL. The drain runs before the publish of this tick, so no
		 * tick reads the instance after the store.
		 */
		chan_lec_tracker_drop(s, c);
		atomic_store_explicit(&c->ec_retired, cur,
				      memory_order_release);
		atomic_store_explicit(&c->ec, NULL, memory_order_release);
		return 0;
	}
	case UDSP_CMD_LINES_UP:
		chan_lines_up(s, b, cmd->arg0, now_ms);
		return 0;
	case UDSP_CMD_LINES_SAFE:
		chan_lines_safe(s, b, now_ms);
		return 0;
	default:
		return -ENOTSUP;
	}
}

/* See the ack_drop_op note in udsp_priv.h; one relaxed load per command. */
static bool chan_ack_drop(struct udsp *s, const struct udsp_cmd *cmd)
{
	uint_least32_t armed;

	if (cmd->type == UDSP_CMD_LEC_RELEASE)
		return atomic_exchange_explicit(&s->lec_ack_drop, false,
						memory_order_relaxed);

	armed = atomic_load_explicit(&s->ack_drop_op, memory_order_relaxed);
	if (!armed || cmd->type != UDSP_CMD_STREAM || armed - 1u != cmd->arg0)
		return false;
	atomic_store_explicit(&s->ack_drop_op, 0u, memory_order_relaxed);

	return true;
}

static void chan_cmd_drain(struct udsp *s, uint32_t now_ms)
{
	struct udsp_cmd cmd;

	while (cmd_ring_pop(&s->ctl_ring, &cmd)) {
		struct chan_batch b = { .count = 0, .overflow = false };
		bool rode;
		int rc;

		/* The slot must still hold this command's token. A waiter that
		 * timed out left CANCELLED behind, and a later command may have
		 * taken the slot since; either way the work is skipped, so slot
		 * reuse can never revive a cancelled command. */
		if (cmd_ack_stale(cmd.ack, cmd.token))
			continue;
		/* The tick barrier that udsp_lec_capture_enable posts: the ack
		 * alone is the answer, so the command touches no line, not
		 * even the stamp mark of chan_cmd_run. */
		if (cmd.type == UDSP_CMD_TICK_SYNC) {
			cmd_ack_post(cmd.ack, cmd.token, 0);
			continue;
		}

		rc = chan_cmd_run(s, &cmd, &b, now_ms);
		if (cmd.line < UDSP_LINE_MAX)
			chan_journal_run(s, &s->chan[cmd.line], &b);
		/* A command that produced no slic record is acked here, at the
		 * tick. That is the arm ack of udsp.h section 1, the same
		 * semantics udsp_cid_send has: the caller learns the request
		 * was accepted, and a failure of the SLIC work an armed
		 * sequence issues later reaches it as the async negative
		 * UDSP_EV_CID_SENT. */
		rode = batch_flush(s, &b, rc ? 0u : cmd.token,
				   rc ? NULL : cmd.ack);
		if ((rc || !rode) && !chan_ack_drop(s, &cmd))
			cmd_ack_post(cmd.ack, cmd.token, rc);
	}
}

/* --- slic thread reports ------------------------------------------------- */

/* The five second deadline is armed when the line enters the open state and is
 * never pushed out by a repeat report: a fault that keeps reporting would
 * otherwise postpone the trial for ever and the line would never come back. A
 * trial that finds the fault still live re-enters here. */
static void chan_fault_open(struct udsp *s, struct udsp_chan *c,
			    struct chan_batch *b, uint32_t now_ms)
{
	if (c->fault_open)
		return;

	c->fault_open = true;
	c->fault_until_ms = now_ms + UDSP_FAULT_RETRY_MS;
	if (lf_confirmed(c) != UDSP_LF_OPEN)
		lf_emit(c, b, UDSP_LF_OPEN);
}

/*
 * Writes the base feed again while a restore is due (fault_restore). Only a
 * successful write advances lf_confirmed to the base, and the fault clear
 * waits for that. The in-flight test comes first: the slic thread stores
 * lf_confirmed before it marks the write done, so once the mark reads done,
 * lf_confirmed is current.
 */
static void chan_feed_restore(struct udsp_chan *c, struct chan_batch *b)
{
	if (chan_lf_in_flight(c))
		return;
	if (lf_confirmed(c) == c->base_lf) {
		c->fault_restore = false;
		return;
	}
	/* A session that holds its own feed restores the base itself when
	 * it ends, as chan_base_lf_set defers to it. */
	if (line_ring_active(&c->ring) || line_cidseq_busy(&c->cid))
		return;
	lf_emit(c, b, c->base_lf);
}

static void chan_fault_trial(struct udsp_chan *c, struct chan_batch *b)
{
	c->fault_open = false;
	c->fault_restore = true;
	chan_feed_restore(c, b);
}

/* The hold a read asks for, or NONE. The device verdicts come first; a clock
 * fault and a device reset never read together (event.h). A plug-in with no
 * recovery op for its own verdict leaves the line nothing to try. */
static uint8_t chan_fault_held_reason(const struct udsp *s, uint32_t bits)
{
	if (bits & UDSP_FAULT_DEVICE_RESET)
		return UDSP_FAULT_HELD_DEVICE_RESET;
	if (bits & UDSP_FAULT_SLIC_CLOCK)
		return UDSP_FAULT_HELD_CLOCK;
	if ((bits & UDSP_FAULT_SLIC_REINIT) && !s->ops->reinit)
		return UDSP_FAULT_HELD_NO_OP;
	/* The converter ladder needs both the restart and the fresh read;
	 * without either the line has nothing to try. */
	if ((bits & UDSP_FAULT_SLIC_DCDC) &&
	    (!s->ops->converter_restart || !s->ops->converter_read))
		return UDSP_FAULT_HELD_NO_OP;

	return UDSP_FAULT_HELD_NONE;
}

static void chan_fault_held_set(struct udsp_chan *c, uint8_t reason)
{
	c->fault_held = true;
	c->fault_held_reason = reason;
}

/* The request a command recorded during the hold becomes the base state; the
 * caller re-asserts the feed. */
static void chan_fault_held_lift(struct udsp_chan *c)
{
	c->fault_held = false;
	c->fault_held_reason = UDSP_FAULT_HELD_NONE;
	c->base_lf = c->base_lf_wanted;
}

/*
 * A standing hold outlives every later read, except the clock hold. The
 * clocks' return ends that one: a read without the CLOCK bit lifts it. When
 * that read carries another hold reason, the hold stays and takes that
 * reason. True when the hold lifted.
 */
static bool chan_fault_held_update(struct udsp *s, struct udsp_chan *c,
				   uint32_t bits)
{
	uint8_t reason = chan_fault_held_reason(s, bits);

	if (c->fault_held_reason == UDSP_FAULT_HELD_CLOCK &&
	    !(bits & UDSP_FAULT_SLIC_CLOCK)) {
		if (reason == UDSP_FAULT_HELD_NONE) {
			chan_fault_held_lift(c);
			return true;
		}
		c->fault_held_reason = reason;
		return false;
	}
	if (reason != UDSP_FAULT_HELD_NONE && !c->fault_held)
		chan_fault_held_set(c, reason);

	return false;
}

static void chan_fault_report(struct udsp *s, struct udsp_chan *c,
			      uint32_t bits, uint32_t now_ms)
{
	struct udsp_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = UDSP_EV_SLIC_FAULT;
	ev.line = (uint16_t)c->line;
	ev.stamp_ms = now_ms;
	ev.u.fault.code = bits;
	chan_event_post(s, &ev);
	c->fault_reported = bits;
}

/*
 * The clear rule of event.h. The read is clean. No open state, hold or
 * recovery stands. The base feed is a powered one the line confirmed. A DOWN
 * or safed line holds base_lf OPEN, and lf_confirmed matches it without the
 * line ever taking a feed back; that base never clears.
 */
static bool chan_fault_clear_due(const struct udsp_chan *c)
{
	if (c->fault_open || c->fault_held || c->fault_reinit)
		return false;
	if (chan_lf_in_flight(c))
		return false;
	if (c->base_lf == UDSP_LF_OPEN || lf_confirmed(c) != c->base_lf)
		return false;

	return c->fault_reported != 0;
}

/*
 * A fault posts once per set until the clear. A repeat read of the same set
 * posts nothing, so a persistent alarm costs one event whatever the trial
 * count. A lifted hold with a clean read ends the open state at once. A
 * lifted hold with a fault still in the read keeps the open state and its
 * deadline. chan_fault_open re-arms nothing on a line already open, so a
 * clock fault that flaps cannot postpone the trial of the other fault.
 */
static void chan_up_fault(struct udsp *s, struct udsp_chan *c,
			  struct chan_batch *b, uint32_t bits, uint32_t now_ms)
{
	bool lifted;

	c->fault_read_bits = bits;
	lifted = chan_fault_held_update(s, c, bits);
	if (bits & CHAN_FAULT_OPEN_SET)
		chan_fault_open(s, c, b, now_ms);
	else if (lifted)
		chan_fault_trial(c, b);
	else if (!bits && c->fault_restore && !c->fault_open &&
		 !c->fault_held && !c->fault_reinit)
		chan_feed_restore(c, b);
	if (bits && bits != c->fault_reported)
		chan_fault_report(s, c, bits, now_ms);
	else if (!bits && chan_fault_clear_due(c))
		chan_fault_report(s, c, 0, now_ms);
	chan_fault_publish(c);
}

/*
 * A feed write failed on the bus. The ring-start or cid_send ack was the arm,
 * so the caller can only learn about the failure from the asynchronous
 * completion: the sequence is aborted and line_cidseq_abort puts the negative
 * status into UDSP_EV_CID_SENT. The slic thread already counted the failure.
 *
 * Only a write that the tick queued after the arm aborts the sequence: a
 * write of the sequence, of its ring session, or the open feed of a fault.
 * The slic thread runs the records in order, so the failure of an earlier
 * write can arrive after the arm. The slic thread reports no other record
 * (slic_thread.c slic_rec_fail_report).
 */
static void chan_up_rec_fail(struct udsp *s, struct udsp_chan *c,
			     struct chan_batch *b,
			     const struct slic_up_rec *rec, uint32_t now_ms)
{
	if (!line_cidseq_busy(&c->cid))
		return;
	if ((int32_t)(rec->bits - c->cid_arm_seq) <= 0)
		return;

	chan_stamp_mark(c, now_ms);
	line_cidseq_abort(&c->cid, rec->rc < 0 ? rec->rc : -EIO, now_ms,
			  &c->gate, &c->out);
	chan_journal_run(s, c, b);
}

/*
 * The recovery begins. The line is safed as a stop safes it, minus the base_lf
 * and state change, so a success returns it to the base it held. The flags are
 * set first, so the abort's own restore writes are dropped as powered feeds.
 */
static void chan_recover_begin(struct udsp *s, struct udsp_chan *c,
			       struct chan_batch *b, uint32_t now_ms)
{
	c->fault_reinit = true;
	c->fault_reinit_late = 0;
	c->fault_open = true;
	c->fault_until_ms = now_ms + SLIC_RECOVER_LOST_MS / 2u;
	chan_stamp_mark(c, now_ms);
	line_ring_stop(&c->ring, now_ms, LINE_RING_STOPPED, &c->gate, &c->out);
	line_cidseq_abort(&c->cid, -ECANCELED, now_ms, &c->gate, &c->out);
	chan_journal_run(s, c, b);
	playout_stop(&c->slot[UDSP_SLOT_API]);
	playout_stop(&c->slot[UDSP_SLOT_SEQ]);
	if (lf_confirmed(c) != UDSP_LF_OPEN)
		lf_emit(c, b, UDSP_LF_OPEN);
	chan_fault_publish(c);
}

/*
 * The recovery ended. On success the line comes back up on its base feed,
 * which chan_lines_up queues. No clear is posted here: the clear rule posts
 * it on the next clean read, once lf_confirmed follows. A SLEEP or OHT base
 * comes back ACTIVE; uvoice's re-derive on the clear settles it. An exhausted
 * recovery (rc == -ENOTRECOVERABLE) holds the line. Any other failure leaves
 * the feed open and the reported bits standing, with the ordinary trial
 * deadline for the retry.
 */
static void chan_recover_done(struct udsp *s, struct udsp_chan *c,
			      struct chan_batch *b, uint32_t mask, int32_t rc,
			      uint32_t now_ms)
{
	c->fault_reinit = false;
	c->fault_reinit_late = 0;
	/* A DONE that arrived after the tick gave the result up: the line
	 * came back, so the loss is not logged. */
	atomic_store_explicit(&c->recover_lost, false, memory_order_release);
	if (rc == 0) {
		chan_lines_up(s, b, mask, now_ms);
		c->fault_open = false;
		c->fault_read_bits = 0;
		c->fault_restore = true;
		/* The reinit put powersave back to the plug-in's init state, so
		 * the confirmation is stale: the next sleep request reaches the
		 * SLIC again. */
		c->sleeping = false;
		atomic_store_explicit(&c->ps_confirmed, false,
				      memory_order_release);
	} else if (rc == -ENOTRECOVERABLE) {
		chan_fault_held_set(c, UDSP_FAULT_HELD_EXHAUSTED);
	} else {
		c->fault_until_ms = now_ms + UDSP_FAULT_RETRY_MS;
	}
	chan_fault_publish(c);
}

/* A recovery whose DONE never came: hold the line and let the slic thread log
 * the loss, because the tick never logs. */
static void chan_recover_lost_mark(struct udsp_chan *c)
{
	c->fault_reinit = false;
	chan_fault_held_set(c, UDSP_FAULT_HELD_EXHAUSTED);
	atomic_store_explicit(&c->recover_lost, true, memory_order_release);
}

/* Test hook: the tick drops the next recovery completion whole and unacked,
 * the loss a stale token leaves; the slic thread re-posts it. */
static bool chan_recover_ack_dropped(struct udsp *s)
{
	return atomic_exchange_explicit(&s->recover_ack_drop, false,
					memory_order_relaxed);
}

/* A hold the slic thread's ladder decided without an attempt: it opens the
 * feed and names the reason (an over-voltage lock-out read twice). */
static void chan_recover_hold(struct udsp_chan *c, struct chan_batch *b,
			      uint8_t reason)
{
	chan_fault_held_set(c, reason);
	c->fault_open = true;
	if (lf_confirmed(c) != UDSP_LF_OPEN)
		lf_emit(c, b, UDSP_LF_OPEN);
	chan_fault_publish(c);
}

static void chan_up_drain(struct udsp *s, uint32_t now_ms)
{
	struct slic_up_rec rec;

	while (slic_up_ring_pop(&s->up, &rec)) {
		struct chan_batch b = { .count = 0, .overflow = false };
		struct udsp_chan *c = &s->chan[rec.line < UDSP_LINE_MAX ?
					       rec.line : 0];
		struct cmd_ack *ack = NULL;
		uint32_t token = 0;

		/* A request whose waiter left is skipped for the same reason a
		 * control command is. */
		if (cmd_ack_stale(rec.ack, rec.token))
			continue;

		switch (rec.type) {
		case SLIC_UP_FAULT:
			chan_up_fault(s, c, &b, rec.bits, now_ms);
			break;
		case SLIC_UP_REC_FAIL:
			chan_up_rec_fail(s, c, &b, &rec, now_ms);
			break;
		case SLIC_UP_LINE_DEAD:
			c->ready = false;
			break;
		case SLIC_UP_REGION_SWAP:
			udsp_region_swap_apply(s);
			cmd_ack_post(rec.ack, rec.token, 0);
			break;
		case SLIC_UP_LINES_UP:
			chan_lines_up(s, &b, rec.bits, now_ms);
			ack = rec.ack;
			token = rec.token;
			break;
		case SLIC_UP_LINES_SAFE:
			chan_lines_safe(s, &b, now_ms);
			ack = rec.ack;
			token = rec.token;
			break;
		case SLIC_UP_LINE_RECOVER_BEGIN:
			chan_recover_begin(s, c, &b, now_ms);
			ack = rec.ack;
			token = rec.token;
			break;
		case SLIC_UP_LINE_RECOVER_DONE:
			if (chan_recover_ack_dropped(s))
				break;
			chan_recover_done(s, c, &b, rec.bits, rec.rc, now_ms);
			ack = rec.ack;
			token = rec.token;
			break;
		case SLIC_UP_LINE_HOLD:
			chan_recover_hold(c, &b, rec.reason);
			break;
		default:
			break;
		}
		if (!batch_flush(s, &b, token, ack) && ack)
			cmd_ack_post(ack, token, 0);
	}
}

/* --- per line ------------------------------------------------------------ */

static void hook_trace_open_push(struct udsp *s, struct udsp_chan *c,
				 uint32_t stamp_ms,
				 const struct line_hook_report *rep)
{
	struct udsp_hook_trace t = {
		.kind = UDSP_HOOK_TRACE_OPEN,
		.open_class = rep->open_class,
		.open_gap_ms = rep->open_gap_ms,
		.close_gap_ms = rep->close_gap_ms,
		.stamp_ms = stamp_ms,
		.open_lo_ms = rep->open_lo_ms,
		.open_hi_ms = rep->open_hi_ms,
	};

	if (!chan_trace_on(s))
		return;
	udsp_hook_traces_push(&c->hook_traces, &t);
}

static void hook_trace_stall_push(struct udsp *s, struct udsp_chan *c,
				  uint32_t now_ms, uint32_t jump)
{
	struct udsp_hook_trace t = {
		.kind = UDSP_HOOK_TRACE_STALL,
		.stamp_ms = now_ms,
		.open_lo_ms = jump,
	};

	if (!chan_trace_on(s))
		return;
	udsp_hook_traces_push(&c->hook_traces, &t);
}

static void chan_machines_run(struct udsp *s, struct udsp_chan *c,
			      uint32_t stamp_ms, bool level,
			      const struct line_hook_sample *sample)
{
	struct line_hook_report rep;

	/* Before the first debounced sample the hook FSM must not run at all,
	 * or it would boot-report a release for a line the SLIC has not even
	 * initialised: the boot state follows the first debounce. */
	if (c->hook_seen)
		rep = line_hook_tick(&c->hook, stamp_ms, level, sample);
	else
		memset(&rep, 0, sizeof(rep));
	if (rep.measured)
		hook_trace_open_push(s, c, stamp_ms, &rep);

	line_ring_tick(&c->ring, stamp_ms, &rep, &c->gate, &c->out);
	line_cidseq_tick(&c->cid, stamp_ms, &rep, &c->gate, &c->out);
	line_hook_emit(&rep, c->line, stamp_ms, &c->out);
}

/*
 * The instant the level was last known, for the hook machine's durations. The
 * slic thread publishes the stamp of every sample before it pushes the edge,
 * so a stamp the tick reads here is at most the stamp of an edge it has not
 * popped yet, and the level the machine holds is confirmed to it. A publish
 * the slic thread invalidated (a line held for a test) freezes the instant:
 * the machine then measures nothing until the line is sampled again.
 */
static uint32_t chan_hook_known(struct udsp_chan *c, uint32_t now_ms,
				bool *stall)
{
	uint64_t pub = atomic_load_explicit(&c->hook_pub, memory_order_acquire);
	uint32_t stamp;

	/* The mark is consumed with the stamp it came with: an exchange that
	 * fails saw a newer publish, which is re-read with its own mark. */
	*stall = false;
	while (pub & UDSP_HOOK_PUB_STALL) {
		if (atomic_compare_exchange_weak_explicit(&c->hook_pub, &pub,
							  pub & ~UDSP_HOOK_PUB_STALL,
							  memory_order_acq_rel,
							  memory_order_acquire)) {
			*stall = true;
			break;
		}
	}
	if (!(pub & UDSP_HOOK_PUB_VALID))
		return c->hook_known_ms;
	stamp = (uint32_t)pub;
	if ((int32_t)(stamp - now_ms) > 0)
		stamp = now_ms;
	if ((int32_t)(stamp - c->hook_known_ms) > 0)
		c->hook_known_ms = stamp;

	return c->hook_known_ms;
}

static void chan_line_control(struct udsp *s, struct udsp_chan *c,
			      uint32_t now_ms)
{
	struct chan_batch b = { .count = 0, .overflow = false };
	struct udsp_hook_edge edge;
	struct line_hook_sample sample;
	uint32_t known;
	uint32_t jump;
	bool stall;
	unsigned n = 0;

	while (n++ < UDSP_HOOK_EDGE_PER_TICK && udsp_hook_edge_pop(&c->edges, &edge)) {
		uint32_t stamp = edge.stamp_ms;

		if (stamp > now_ms)
			stamp = now_ms;
		if (stamp < c->last_stamp_ms)
			stamp = c->last_stamp_ms;
		c->last_stamp_ms = stamp;
		c->hook_level = edge.level != 0;
		c->hook_seen = true;
		if ((int32_t)(stamp - c->hook_known_ms) > 0)
			c->hook_known_ms = stamp;
		sample.known_ms = stamp;
		sample.gap_ms = edge.gap_ms;
		sample.flags = edge.flags;
		chan_machines_run(s, c, stamp, c->hook_level, &sample);
	}
	c->last_stamp_ms = now_ms;
	/* A late sample the slic thread published carries its stall to the
	 * machine on this path too: a sample with an unchanged level pushes no
	 * record, and the record of a lost pair comes one sample later, so
	 * without this the machine would credit the stall to the make it
	 * interrupted and flush the partial digit before the record arrives.
	 * The gap is the jump of the known instant; a
	 * record popped above already moved the instant to its stamp, so after
	 * one the jump is the few milliseconds to the newest publish. */
	known = c->hook_known_ms;
	sample.known_ms = chan_hook_known(c, now_ms, &stall);
	jump = sample.known_ms - known;
	sample.gap_ms = 0;
	sample.flags = 0;
	if (stall) {
		sample.gap_ms = jump > UINT16_MAX ? UINT16_MAX : (uint16_t)jump;
		sample.flags = UDSP_HOOK_EDGE_STALL;
		hook_trace_stall_push(s, c, now_ms, jump);
	}
	chan_machines_run(s, c, now_ms, c->hook_level, &sample);
	chan_journal_run(s, c, &b);
	batch_flush(s, &b, 0, NULL);
}

/*
 * The SLIC adds a constant offset to the uplink word after its own gain stage.
 * The offset depends on the gain and the rate, 4.1 to 7.1 LSB on the Si32282,
 * so no fixed value cancels it. Without the removal, the canceller floor and
 * the comfort noise read about 10 dB high at 8 kHz.
 *
 * The first frame that is not all zeros seeds the estimate, so the line reads
 * without the offset from that frame. The time constant of 64 frames puts the
 * corner near 0.25 Hz, far under any voice or modem band. The carry adds the
 * rounding error of each sample to the next, so no fraction of the estimate
 * stays as a bias. It holds the rounding error alone, so a clipped run cannot
 * grow it.
 *
 * A frame of exact zeros is not a reading of the offset. It leaves the
 * estimate, the seed and the carry as they are, and it takes no subtraction.
 * The part mutes an on-hook line in both directions, and an on-hook capture
 * reads exact digital silence in PCMU. The offset would
 * show there as the code 0xFE, so the muted word stands within 3 LSB of zero.
 * The estimate of one call therefore carries over the on-hook time to the
 * next call, and a muted frame reaches the consumers as exact zeros.
 *
 * The modem kind skips the subtraction, so a modem passthrough carries the
 * word as the SLIC and gain_tx left it. The estimate still tracks, so the
 * return to voice starts from the current offset. The fax kind takes the frame
 * without the offset, which carries no fax information. The fax detector
 * removes it anyway with its own high-pass (dsp/fax_det.c hold_energy_update).
 */
static void chan_ul_offset_remove(struct udsp *s, struct udsp_chan *c)
{
	chan_ul_dc_frame(&c->ul_dc, c->ul, c->frame,
			 chan_passthrough_get(s, c) != UDSP_PASSTHROUGH_MODEM);
}

static void chan_ul(struct udsp *s, struct udsp_chan *c, const int16_t *slot,
		    unsigned frames, uint32_t now_ms)
{
	struct udsp_event ev;

	/* Ahead of the two exits below, so a line the caller disarmed still
	 * runs the hold out instead of firing it at the next arm. */
	if (c->dl_digit_hold)
		c->dl_digit_hold--;
	memcpy(c->ul, slot, frames * sizeof(*slot));
	if (c->cfg.gain_tx)
		udsp_gain_apply(c->ul, frames, c->cfg.gain_tx);
	if (frames != c->frame)
		return;
	chan_ul_offset_remove(s, c);
	if (!c->dtmf_armed)
		goto media;
	if (chan_dtmf_hold(s, c, now_ms))
		goto media;
	if (!udsp_dtmf_detector_process(&c->detect,
					chan_det_frame(c, &c->rs_ul_pre, c->ul),
					&ev))
		goto media;

	chan_digit_post(s, c, &ev, now_ms);
media:
	/* The media chain runs last on the UL frame, so the detectors see the
	 * captured signal and the canceller's output goes to the network. */
	chan_digit_track(c);
	chan_tick_ul_media(s, c, c->ul, now_ms);
}

/*
 * The uplink half of a synthetic tick (udsp_engine_render). No capture period
 * goes with it, so no detector, no digit hold, no relay tap and no encoder
 * sees a frame: each would read the zeros as audio, and a digit or a tone
 * would end on them. The send side of every stream steps one period, as for
 * a skipped period (stream_tick_tx_synthetic). The canceller takes the
 * reference of the tick into its history without a filter run, because the
 * line plays the frame this tick computes: a FEED with the reason
 * SKIP_SYNTHETIC and a zero microphone plane.
 */
static void chan_ul_synthetic(struct udsp *s, struct udsp_chan *c,
			      uint32_t now_ms)
{
	struct udsp_ec *ec = atomic_load_explicit(&c->ec, memory_order_acquire);

	memset(c->ul, 0, c->frame * sizeof(*c->ul));
	if (ec)
		chan_lec_ref_feed(s, c, ec, c->ul,
				  UDSP_LEC_CAPTURE_SKIP_SYNTHETIC);
	if (stream_line_active(s, c->line))
		stream_tick_tx_synthetic(s, c, now_ms);
}

static void chan_audio_end(struct udsp *s, struct udsp_chan *c, uint32_t now_ms)
{
	struct udsp_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = UDSP_EV_AUDIO_END;
	ev.line = (uint16_t)c->line;
	ev.stamp_ms = now_ms;
	chan_event_post(s, &ev);
}

/*
 * The gain a playout frame takes before the sum. A digit played through
 * udsp_digit_play is the far end's key press that the caller relays (udsp.h
 * UDSP_FEAT_DTMF_PLAYOUT), so it is network audio and takes the port's receive
 * gain, as decoded audio and a rendered telephone event do. Every other
 * playout is a local signal and keeps its region level at the line
 * (chan_rx_split_apply). Reason: with the receive gain on the voice path alone,
 * an INFO digit played 9 dB louder than the same digit carried by RFC 4733 at
 * a -9 dB receive gain.
 */
static int16_t chan_playout_gain(const struct udsp_chan *c,
				 const struct udsp_playout *live)
{
	if (live == &c->slot[UDSP_SLOT_API] &&
	    live->kind == UDSP_PLAYOUT_DIGITS)
		return c->dl_voice_gain;

	return c->dl_local_gain;
}

static void chan_dl(struct udsp *s, struct udsp_chan *c, int16_t *slot,
		    unsigned frames, uint32_t now_ms, bool synthetic)
{
	struct udsp_playout *api = &c->slot[UDSP_SLOT_API];
	struct udsp_playout *live;
	bool is_seq;

	chan_tick_dl_media(s, c, c->dl, now_ms);
	chan_loop_dl(s, c);
	/* The receive gain of the port runs ahead of the playout sum, and a
	 * local playout takes only the trim of a SLIC gain
	 * (chan_rx_split_apply, chan_playout_gain). No gain follows the sum,
	 * so a local signal keeps its region level. */
	if (c->dl_voice_gain)
		udsp_gain_apply(c->dl, frames, c->dl_voice_gain);
	/* After the downlink detector and only on a tick that ran the uplink
	 * one too: chan_ul skips its media chain on a partial slice and
	 * chan_ul_synthetic runs none, and the release rule reads both feeds
	 * of the same frame. */
	if (frames == c->frame && !synthetic)
		chan_fax_bypass_tick(s, c, now_ms);
	chan_seq_drain_tick(c);

	live = playout_live_get(c);
	is_seq = live == &c->slot[UDSP_SLOT_SEQ];

	if (live) {
		bool alive = playout_render(live, c->mix, frames);
		int16_t gain = chan_playout_gain(c, live);

		if (gain)
			udsp_gain_apply(c->mix, frames, gain);
		frame_add_sat(c->dl, c->mix, frames);
		/* Slot B is the caller identity sequencer, whose DTMF string is
		 * a type 1 on-hook transport: no handset answers it and no
		 * detector may hear it, so it takes no hold. */
		if (live == api && api->kind == UDSP_PLAYOUT_DIGITS)
			chan_digit_hold_arm(s, c);
		if (!alive) {
			playout_stop(live);
			if (is_seq)
				chan_seq_drain_arm(s, c);
			else
				chan_audio_end(s, c, now_ms);
		}
	}
	memcpy(slot, c->dl, frames * sizeof(*slot));
	if (frames == c->frame)
		chan_dl_history_add(c, c->dl);
}

/* A DONE of the recovery is due. Two deadlines with none is a lost result: the
 * line is held and the slic thread logs it, because the tick never logs. */
static void chan_recover_wait_tick(struct udsp_chan *c, uint32_t now_ms)
{
	if (now_ms < c->fault_until_ms)
		return;
	c->fault_until_ms = now_ms + SLIC_RECOVER_LOST_MS / 2u;
	if (++c->fault_reinit_late >= 2)
		chan_recover_lost_mark(c);
}

/* A held-open line keeps its edge ring drained so the slic thread never
 * blocks on it, but runs no machine until the timed retry. A recovery in
 * flight and a hold refuse the retry. The slic thread ends the recovery; a
 * clean read of the clocks or the next start ends the hold. */
static void chan_fault_hold(struct udsp *s, struct udsp_chan *c, uint32_t now_ms)
{
	struct chan_batch b = { .count = 0, .overflow = false };
	struct udsp_hook_edge edge;

	while (udsp_hook_edge_pop(&c->edges, &edge))
		c->hook_level = edge.level != 0;

	/* The open feed is the target of a held, a recovering and a pre-trial
	 * line. A transient error on the open write leaves lf_confirmed short
	 * of OPEN, so re-emit it until it confirms. The in-flight guard keeps
	 * the re-emit to one record at a time. */
	if (lf_confirmed(c) != UDSP_LF_OPEN && !chan_lf_in_flight(c))
		lf_emit(c, &b, UDSP_LF_OPEN);

	if (c->fault_reinit)
		chan_recover_wait_tick(c, now_ms);
	else if (!c->fault_held && now_ms >= c->fault_until_ms)
		chan_fault_trial(c, &b);
	chan_fault_publish(c);
	batch_flush(s, &b, 0, NULL);
}

/*
 * The tick's half of a death. The ring session and the caller identity burst
 * the line carried end here, on the thread that owns both machines, and the
 * journal run republishes owned as false, so the init retry stops reading the
 * line as busy (slic_thread.c slic_dead_retry_step). It runs from the tick's
 * own dead branch and not from the SLIC_UP_LINE_DEAD record, whose push the
 * slic thread cannot wait for: a full up ring would drop the record and leave
 * the line owned for the life of the handle.
 *
 * The relay-off and feed records still flow to the slic thread, which answers
 * -EIO for a dead line without a bus access and marks the write generation
 * done (slic_rec_seq_mark). Dropping them here would leave chan_lf_in_flight
 * standing for a write that never ran.
 */
static void chan_line_dead(struct udsp *s, struct udsp_chan *c, uint32_t now_ms)
{
	struct chan_batch b = { .count = 0, .overflow = false };

	c->dead_done = true;
	chan_stamp_mark(c, now_ms);
	line_ring_stop(&c->ring, now_ms, LINE_RING_DEAD, &c->gate, &c->out);
	line_cidseq_abort(&c->cid, -EIO, now_ms, &c->gate, &c->out);
	chan_journal_run(s, c, &b);
	playout_stop(&c->slot[UDSP_SLOT_API]);
	playout_stop(&c->slot[UDSP_SLOT_SEQ]);
	batch_flush(s, &b, 0, NULL);
}

static void chan_tick(struct udsp *s, struct udsp_chan *c, const int16_t *rx,
		      int16_t *tx, unsigned frames, uint32_t now_ms,
		      bool synthetic)
{
	if (atomic_load_explicit(&c->dead, memory_order_acquire)) {
		if (!c->dead_done)
			chan_line_dead(s, c, now_ms);
		/* A peer that loops this line must hear silence from here on,
		 * not the second of audio the ring still carries. */
		chan_loop_slot_zero(s, c->line, s->loop_tick);
		memset(tx, 0, frames * sizeof(*tx));
		return;
	}
	c->ec_ran = false;
	c->lec_loop_open = false;
	chan_loop_write(s, c, rx, frames);
	if (synthetic)
		chan_ul_synthetic(s, c, now_ms);
	else
		chan_ul(s, c, rx, frames, now_ms);
	if (c->fault_open)
		chan_fault_hold(s, c, now_ms);
	else
		chan_line_control(s, c, now_ms);
	chan_dl(s, c, tx, frames, now_ms, synthetic);
	/* After the UL half, so the record carries the frame the canceller
	 * just ran, and after the command drain, so a channel configuration
	 * that switched the canceller off in this tick is already applied. */
	chan_lec_publish(s, c);
}

/*
 * The canceller's reference delay is the engine's measured loop delay, so the
 * tick reads the published statistics every tick: engine_stats_get is a cheap
 * double-buffered copy, and a once-a-second poll would feed the old reference
 * to the filters for up to a second after a recalibration. After three
 * restarts that never completed, a line without a value takes the last
 * completed window, or the nominal prefill depth before the first one.
 *
 * The engine's restart count is monotonic (engine.h cal_restarts). A count of
 * the calibrated-to-uncalibrated edge would never reach the fallback on a board
 * whose every window is cut short before the first completion, and the
 * canceller would stay frozen for the life of the run. The three are counted
 * from the last window that completed, so a run of hold episodes, each with
 * a window that completes, never spends the allowance.
 */
#define CHAN_LEC_RESTART_MAX	3u

void chan_lec_line_drop(struct chan_lec_track *t, unsigned line)
{
	struct chan_lec_line *l = &t->line[line];

	l->loop = 0;
	l->delay = 0;
	l->applied = false;
}

enum chan_lec_offer chan_lec_measure(struct chan_lec_track *t,
				     const struct engine_stats *st)
{
	/*
	 * A replacement engine (stop and start) counts cal_restarts from zero
	 * again. The stats serial is monotonic across engine instances and is
	 * never 0 for a live engine (engine.h serial). A recycled allocation
	 * at the old address therefore cannot alias, as it could under a
	 * compare of the engine pointer. The zero udsp_chan_all_reset leaves
	 * here matches no engine, so the first tick under any engine moves the
	 * generation of every line.
	 *
	 * The move sits above the open-window return on purpose. A stale
	 * converged filter must not subtract a misaligned estimate against the
	 * new ring while the new engine's window is still open. The move
	 * zeroes the filters, so no alignment stands, and the next value of
	 * each line applies verbatim.
	 *
	 * The new engine holds no measured loop yet, so the hold of a played
	 * digit reads the nominal prefill until its first window completes.
	 */
	if (st->serial != t->serial) {
		t->serial = st->serial;
		t->restarts_done = st->cal_restarts;
		t->loop_last = st->prefill_periods * st->frames_per_period;
		for (unsigned i = 0; i < UDSP_LINE_MAX; i++) {
			chan_lec_line_drop(t, i);
			t->line[i].gen++;
		}
	}
	/*
	 * A completed window offers loop_ref and not delay_ref, because the
	 * echo crosses the playback queue and the capture backlog (engine.h
	 * loop_ref). The playback half alone puts the reference 160 samples
	 * early on MT7987, and taps misaligned that far never converge.
	 *
	 * A completed window is also the mark the fallback counts from. A hold
	 * episode restarts the window on purpose, and its window completes, so
	 * it must not spend the allowance of a window that never completes.
	 * The hold of a played digit reads the window whether or not the
	 * tolerance keeps an older reference on a line.
	 */
	if (st->delay_calibrated) {
		t->restarts_done = st->cal_restarts;
		t->offer = CHAN_LEC_OFFER_WINDOW;
		t->offer_loop = (uint32_t)(st->loop_ref < 0 ? 0 : st->loop_ref);
		t->loop_last = t->offer_loop;
		return CHAN_LEC_OFFER_WINDOW;
	}
	/* An open window inside the allowance offers nothing, so adaptation
	 * waits. */
	if (st->cal_restarts - t->restarts_done < CHAN_LEC_RESTART_MAX) {
		t->offer = CHAN_LEC_OFFER_NONE;
		return CHAN_LEC_OFFER_NONE;
	}
	t->offer = CHAN_LEC_OFFER_FALLBACK;
	t->offer_loop = t->loop_last;

	return CHAN_LEC_OFFER_FALLBACK;
}

/*
 * The loop that one line takes from the offer of this tick. Past the
 * allowance a line with a value keeps it. A measurement beats the nominal
 * guess. Replacing the reference of an aligned filter with the guess would
 * cost two resets for one open window. A line without a value takes the last
 * completed window of this engine, or the nominal prefill before the first
 * one (loop_last).
 *
 * The nominal is the playback prefill alone, and a line takes it only where
 * this engine completed no window. A guessed capture allowance overshoots a
 * back end with no backlog. A reference past the true loop is non-causal,
 * which no tap can cancel, while a short one only spends tail. Underestimate
 * on purpose; chan_lec_ref_guard applies the same rule to every value that
 * stands on a line.
 */
static uint32_t chan_lec_loop_pick(const struct chan_lec_track *t,
				   const struct chan_lec_line *l)
{
	if (t->offer == CHAN_LEC_OFFER_FALLBACK && l->applied)
		return l->loop;

	return t->offer_loop;
}

/*
 * One period of tolerance, inclusive, in either direction. The reason is the
 * quantized reading of the AN7581. Its pointer is period-granular, so after a
 * hold the reading at 16 kHz flips between 640 and 800. By its peak, the echo
 * stands at 687 to 791 samples in the 640 state, and at 875 to 927 in the
 * 800 state. A strict bound re-times on every flip. Inside the tolerance
 * the standing value wins, because the taps are aligned to it. A move of the
 * reference without a move of the taps misaligns them by up to one period.
 * That costs a reconvergence and keeps the stale taps through it.
 *
 * The limit: a hold inside a call moves the physical loop, by one period on
 * the MT7988. The tolerance then keeps taps that stand one period off.
 */
static bool chan_lec_ref_moved(uint32_t loop, uint32_t applied, unsigned frame)
{
	uint32_t step = loop > applied ? loop - applied : applied - loop;

	return step > frame;
}

/*
 * One period of causal margin under the loop a line stands on. A fit of
 * captured echo paths puts the echo onset at -20 dB past loop_ref at the base
 * delay:
 *
 * - on the AN7581, 3 samples: 0.2 ms at 16 kHz and 0.4 ms at 8 kHz;
 * - on the MT7988, 58 samples (3.6 ms) at 16 kHz and 59 (7.4 ms) at 8 kHz.
 *
 * The guard stays, because a reference past the onset is non-causal. A filter
 * cannot cancel what sits before its first tap. An ideal filter on those
 * paths loses 4 to 6 dB per millisecond the onset stands ahead of it. The
 * period of margin
 * spends tail the measured responses do not use. The tolerance compare stays
 * on the measurement, so the guard moves nothing else.
 */
static uint32_t chan_lec_ref_guard(uint32_t loop, unsigned frame)
{
	return loop > frame ? loop - frame : 0;
}

enum chan_lec_verdict chan_lec_line_decide(struct chan_lec_track *t,
					   unsigned line, unsigned rate,
					   unsigned frame, uint32_t *delay)
{
	struct chan_lec_line *l = &t->line[line];
	uint32_t gen = l->gen;
	uint32_t loop;

	if (t->offer == CHAN_LEC_OFFER_NONE) {
		*delay = l->delay;
		return CHAN_LEC_HOLD;
	}
	loop = chan_lec_loop_pick(t, l);
	if (!l->applied) {
		l->loop = loop;
		l->applied = true;
	} else if (chan_lec_ref_moved(loop, l->loop, frame)) {
		l->loop = loop;
		l->gen++;
	}
	l->delay = chan_lec_delay_clamp(chan_lec_ref_guard(l->loop, frame),
					rate, frame);
	*delay = l->delay;

	return l->gen != gen ? CHAN_LEC_RETIME : CHAN_LEC_KEEP;
}

/*
 * The handle step of the tick, then every line that holds an instance. The
 * pass reads lec_cap_for, which only the tick sets (chan_lec_instance_take)
 * and clears (chan_lec_tracker_drop). It never reads c->ec: the control
 * thread stores that pointer at any instant, one tick or more before the
 * first frame. A pass keyed on it would adopt before the first frame. A hold
 * between that adoption and the frame would then fall inside the tolerance,
 * and the NEW record would carry the stale value.
 *
 * A line without an instance decides nothing. A line whose first frame falls
 * in an open window stays unapplied and takes the first later offer here.
 */
static void chan_lec_delay_track(struct udsp *s)
{
	unsigned lines = udsp_lines(s);
	struct engine_stats st;
	struct engine *e;

	e = atomic_load_explicit(&s->engine_pub, memory_order_acquire);
	if (!e)
		return;

	engine_stats_get(e, &st);
	chan_lec_measure(&lec_track, &st);
	atomic_store_explicit(&s->lec_loop, lec_track.loop_last,
			      memory_order_relaxed);
	for (unsigned i = 0; i < lines; i++) {
		if (s->chan[i].lec_cap_for)
			chan_lec_line_apply(s, i);
	}
}

/*
 * The playout half of a skip. A slot renders one frame per tick, so its phase
 * is in samples the engine played and not in wall time: the frames the engine
 * never wrote are rendered here and thrown away, which is what the line heard.
 * A cadence, a digit string and a tone limit then all stand where wall time
 * puts them. The render stops at the frame that ends the slot, and the latched
 * answer stands until the next tick: chan_dl reads it there, stops the slot
 * and posts the end exactly once, so a finite cadence never starts again.
 *
 * A caller identity burst does not survive this: the modulator loses the bits
 * of the skipped frames and the far end reads a corrupt message. A hold long
 * enough to reach that has already cut the burst on the line.
 */
static void chan_playout_skip(struct udsp_chan *c, unsigned periods)
{
	struct udsp_playout *live = playout_live_get(c);

	if (!live)
		return;

	for (unsigned i = 0; i < periods; i++) {
		if (!playout_render(live, c->mix, c->frame))
			return;
	}
}

/*
 * The engine dropped capture periods (engine.h engine_hooks.skip). The tick
 * clock has already stepped over them, so every machine that reads it keeps
 * wall time by itself: the hook detector and the line machines need nothing
 * here. What does not read that clock advances per period it runs, and there
 * are three: the jitter buffer of a received stream, which plays one unit per
 * get, the media clock of a sending stream and the sample phase of a playout.
 * stream_tick_skip pays the first two, chan_playout_skip the third.
 *
 * The run clock names this tick before any of that runs. The staged packet a
 * stream flushes here leaves the box on this tick and must be stamped with it,
 * and the tick hook publishing the same value a moment later costs nothing.
 */
void udsp_engine_skip(void *ctx, unsigned periods, uint64_t tick)
{
	struct udsp *s = ctx;
	unsigned lines = udsp_lines(s);
	uint64_t mono_us;
	uint32_t now_ms;

	if (!lines || !periods)
		return;

	mono_us = udsp_mono_us();
	udsp_clock_publish(&s->clock, tick, mono_us);
	s->loop_tick = udsp_clock_tick(&s->clock);
	now_ms = udsp_now_ms(s);
	/* The canceller of no line sees these periods. The next capture record
	 * of each line counts them, unless it is NEW or an END
	 * (lec_capture_publish). */
	s->lec_skip_total += periods;
	chan_loop_skip(s, periods);
	for (unsigned i = 0; i < lines; i++) {
		struct udsp_chan *c = &s->chan[i];

		if (atomic_load_explicit(&c->dead, memory_order_acquire))
			continue;
		stream_tick_skip(s, c, periods, mono_us * 1000u, now_ms);
		chan_playout_skip(c, periods);
	}
}

/*
 * The clock of a tick, a synthetic one included. The loop ticks from engine
 * start, so it is running while udsp_open is still probing: it keeps the ring
 * drained and the PCM clock alive, which is what the SLIC attach needs, but
 * the capability record, the stream pool and the SLIC instance do not exist
 * yet. Until udsp_open publishes the line count a tick only moves periods,
 * and the master clock the other threads stamp against starts with the first
 * line. False then.
 */
static bool chan_tick_clock(struct udsp *s, uint64_t tick, uint32_t *now_ms)
{
	uint64_t mono_us;

	if (!udsp_lines(s))
		return false;

	mono_us = udsp_mono_us();
	udsp_clock_publish(&s->clock, tick, mono_us);
	/* The one index the loop rings are written and read at this tick. */
	s->loop_tick = udsp_clock_tick(&s->clock);
	/* The jitter buffer and the arrival stamps of the net thread share one
	 * CLOCK_MONOTONIC base (jb.h Time). */
	s->tick_ns = mono_us * 1000u;
	*now_ms = udsp_now_ms(s);

	return true;
}

static void chan_tick_lines(struct udsp *s, const int16_t *rx, int16_t *tx,
			    unsigned channels, unsigned frames,
			    uint32_t now_ms, bool synthetic)
{
	unsigned lines = udsp_lines(s);

	if (lines > channels)
		lines = channels;
	for (unsigned i = 0; i < lines; i++)
		chan_tick(s, &s->chan[i], rx + i * frames, tx + i * frames,
			  frames, now_ms, synthetic);

	udsp_slic_tick_wake(s);
}

void udsp_engine_hook(void *ctx, int16_t *rx, int16_t *tx, unsigned channels,
		      unsigned frames, uint64_t tick)
{
	struct udsp *s = ctx;
	uint32_t now_ms;

	if (!chan_tick_clock(s, tick, &now_ms))
		return;
	chan_lec_delay_track(s);

	chan_cmd_drain(s, now_ms);
	chan_up_drain(s, now_ms);

	chan_tick_lines(s, rx, tx, channels, frames, now_ms, false);
}

/*
 * A period of the refill of a restart (engine.h engine_hooks.render): the
 * engine computes the periods of an empty ring ahead, on a read it dropped,
 * and writes them when the ring restarts. Each is a tick without a capture
 * period. The downlink half runs as on any tick: the jitter buffer, the
 * playouts, the loop and the playback history, so the line plays the audio
 * of these indices and the canceller's reference indexes it. The uplink half
 * runs chan_ul_synthetic. The commands do not drain here: the control hook
 * of the same read ran them first.
 */
void udsp_engine_render(void *ctx, int16_t *rx, int16_t *tx, unsigned channels,
			unsigned frames, uint64_t tick)
{
	struct udsp *s = ctx;
	uint32_t now_ms;

	if (!chan_tick_clock(s, tick, &now_ms))
		return;
	chan_lec_delay_track(s);
	chan_tick_lines(s, rx, tx, channels, frames, now_ms, true);
}

/*
 * A read the engine dropped (engine.h engine_hooks.control). The commands and
 * the slic reports drain as on a tick, so a SYNC-TICK caller gets its ack
 * from the first read after a hold, not from the first write after the drain
 * of a restart. No line runs: there is no period.
 */
void udsp_engine_control(void *ctx, uint64_t tick)
{
	struct udsp *s = ctx;
	uint32_t now_ms;

	if (!chan_tick_clock(s, tick, &now_ms))
		return;
	chan_cmd_drain(s, now_ms);
	chan_up_drain(s, now_ms);
	udsp_slic_tick_wake(s);
}

void udsp_chan_reset(struct udsp *s, unsigned line, unsigned rate)
{
	struct udsp_chan *c = &s->chan[line];
	const struct udsp_region *r = udsp_region_active(s);

	memset(c, 0, sizeof(*c));
	c->line = line;
	c->rate = rate;
	c->frame = rate / 100u;
	/* The published canceller record belongs to the channel this call
	 * replaces, so the reader is refused until a tick publishes for the
	 * new one. */
	s->lec_pub[line] = NULL;
	atomic_store_explicit(&s->lec_valid[line], false, memory_order_release);
	atomic_store_explicit(&s->monitor_valid[line], false,
			      memory_order_release);
	/* dsp/resamp.h: every instance is reset before its first use. */
	udsp_resamp_reset(&c->rs_ul_pre);
	udsp_resamp_reset(&c->rs_ul_post);
	udsp_resamp_reset(&c->rs_dl);
	atomic_init(&c->dead, false);
	atomic_init(&c->hist_frames, 0u);
	atomic_init(&c->mute, false);
	atomic_init(&c->fax_bypass_kind, UDSP_PASSTHROUGH_OFF);
	atomic_init(&c->ec, NULL);
	atomic_init(&c->ec_retired, NULL);
	atomic_init(&c->sampling, false);
	atomic_init(&c->owned, false);
	atomic_init(&c->under_test, false);
	atomic_init(&c->hook_pub, 0u);
	atomic_init(&c->fault_pub, 0u);
	atomic_init(&c->lf_seq_done, 0u);
	atomic_init(&c->recover_lost, false);
	atomic_init(&c->edges.head, 0u);
	atomic_init(&c->edges.tail, 0u);
	udsp_hook_traces_init(&c->hook_traces);
	udsp_lec_traces_init(&c->lec_traces);
	line_hook_init(&c->hook, r ? &r->hook : NULL);
	line_ring_init(&c->ring, line);
	line_cidseq_init(&c->cid, line);
	c->base_lf = UDSP_LF_OPEN;
	atomic_init(&c->lf_confirmed, UDSP_LF_OPEN);
	atomic_init(&c->ps_confirmed, false);
	atomic_init(&c->gain_confirmed, 0u);
	c->state = UDSP_LINE_DOWN;
	c->down_wanted = s->cfg.lines_start_down;
}

void udsp_chan_all_reset(struct udsp *s, unsigned lines, unsigned rate)
{
	memset(&lec_track, 0, sizeof(lec_track));
	for (unsigned i = 0; i < lines; i++)
		udsp_chan_reset(s, i, rate);
}
