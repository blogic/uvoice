// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <errno.h>
#include <string.h>

#include "jb.h"

/*
 * Tunables. Every constant below is a policy choice, not a protocol constant.
 *
 * JB_TARGET_K: the adaptive target is k times the 2 s high-water of the RFC
 * 3550 A.8 smoothed delay variation. k = 3 is the 3-sigma reading of a jitter
 * estimate that is itself a mean deviation, so the target covers the arrival
 * tail instead of the average.
 * JB_PLC_MAX_MS: G.711 Appendix I concealment stays useful for a few hundred
 * milliseconds; beyond that the synthetic signal is worse than comfort noise.
 * JB_SHRINK_MS: the target sheds at most one frame per second of clean
 * playout, so a single quiet second can never collapse the buffer.
 * JB_SKEW_INTERVAL_MS / JB_SKEW_FREE_MS: a drop or insert is armed only by a
 * drift the 4 s depth history proves, and then runs at one frame per second
 * until the depth is back at the setpoint; the first second after (re)init is
 * unlimited so startup converges at once.
 * JB_SPAN_AGREE: a packet lost in a 10 ms stream makes one timestamp delta look
 * like a 20 ms ptime, so the learned span changes only after this many agreeing
 * observations from consecutive packets.
 * JB_STARVE_MS: a buffer that has concealed for a full second with nothing
 * queued has lost the far end's timeline; the next packet re-anchors it instead
 * of being dropped as late forever.
 * JB_CN_RUN_MAX_MS: an armed SID silence keeps the media clock running with
 * nothing queued (an Annex B sender may put one SID on the wire and then
 * nothing for the whole silence), and while comfort noise is active
 * jb_hold_needed is false, so none of the parked-clock recovery can run: with
 * a far end whose timestamp stands still through the silence, a lost resume
 * packet leaves every later packet behind the free-running clock, late-dropped
 * forever. The clock therefore parks after this much SID comfort
 * noise with no arrival at all; RFC 3389 comfort noise is exempt, because its
 * senders repeat the description and the repetition is the arrival the SID
 * path lacks. Ten seconds is JB_RESYNC_SEC, the bound past which
 * every other mechanism in this file stops trusting a timeline it has not
 * heard from; past it the free-run is an extrapolation, and the parked-clock
 * machinery (starve, refill, hold) owns the resume.
 * JB_HOLD_MAX, the deferred refill anchor: a parked clock refuses a far-ahead
 * refill at most this many times in a row; the packet after them is adopted.
 * Two, because the confirm window tolerates sequence gaps up to 3: a run of
 * refusals that window can never confirm has lost the far end's timeline,
 * which is jb_late_starved's principle extended to the ahead direction.
 * Without the bound a genuine far resume under heavy loss (arriving gaps of
 * 4) is permanent silence.
 * JB_STEP_GATE_K (UDSP_JB_ADAPTIVE only): the confirm distance must also
 * exceed this multiple of the internal jitter estimate. On a line whose
 * arrival jitter puts packets beyond the static window + span bound,
 * correlated reorder-tail pairs satisfy the two-packet proof and each false
 * resync discards the queue (without the gate: 11 resyncs per 120 s at 0 to
 * 300 ms jitter). Four is the lowest multiple that gave no false resync in a
 * sweep of the multiple. The cost
 * is a higher standing depth while the line's jitter stays high, and slower
 * adoption of genuine steps below the gate, which absorb through the trend
 * corrector instead. UDSP_JB_FIXED is excluded: a gate-blocked genuine step
 * there sheds carrier frames at the trend rate for tens of seconds, the loss
 * class the fax path must never take, and a fixed target cannot follow the
 * estimate, so the churn the gate removes is cheaper than the absorption it
 * would impose.
 * JB_TRIM_EXCESS_MS / JB_TRIM_HOLD_MS, the offset trim: a standing depth
 * above the target that the trend gate never saw rise (the prefill's surplus
 * once the target contracted, or skew below the gate's rate) is shed at the
 * clock-skew correction rate. Arming needs the excess above
 * JB_TRIM_EXCESS_MS (two frames: a one-frame wobble never arms, two frames is
 * the surplus tolerated before it counts as latency) without a break for
 * JB_TRIM_HOLD_MS in which nothing concealed, late-dropped or refilled: every
 * one of those resets trim_calm_ns, so a line that recently needed its
 * reserve keeps it by construction. Ten seconds distinguishes a parked
 * surplus from the quiet stretch of a bursty line. The trim runs only in
 * UDSP_JB_ADAPTIVE with target_ms below max_ms: UDSP_JB_FIXED playout is
 * pinned for the fax path (stream.c), and a clamped target means the depth
 * above it is reserve the clamp refused, not surplus. A trim-armed run ends
 * at half a packet span above the target, not at it: the averaged excess
 * lags, and a run to the bare target was measured to drag the queue floor a
 * full packet below it.
 */
#define JB_TARGET_K		3U
#define JB_PLC_MAX_MS		400U
#define JB_SHRINK_MS		1000U
#define JB_SKEW_INTERVAL_MS	1000U
#define JB_SPAN_AGREE		2U
#define JB_STEP_GATE_K		4U
#define JB_HOLD_MAX		2U
#define JB_SKEW_FREE_MS		1000U
#define JB_STARVE_MS		1000U
#define JB_CN_RUN_MAX_MS	(JB_RESYNC_SEC * 1000U)
#define JB_TRIM_EXCESS_MS	20U
#define JB_TRIM_HOLD_MS		10000U
#define JB_CONCEAL_RUN_MAX_MS	60000U
#define JB_DEPTH_SHIFT		6U		/* ~640 ms averaging at 10 ms ticks */
#define JB_RESYNC_SEC		10U
#define JB_BUCKET_MS		(UDSP_JB_WINDOW_MS / UDSP_JB_WINDOW_BUCKETS)
/* One trend window beyond the window the trend is measured over.
 * Derived, not chosen: jb_depth_trend_ms compares the average against the
 * oldest of UDSP_JB_TREND_BUCKETS buckets, so a drift that is still there one
 * whole window later moved the depth twice; an event that merely stepped it
 * did not, and the conceal or refill it carries resets the hold anyway. */
#define JB_TREND_HOLD_MS	(UDSP_JB_TREND_BUCKETS * JB_BUCKET_MS)

_Static_assert(RTP_MARKED_STEP_MS ==
	       UDSP_JB_DEPTH_MAX_MS + UDSP_JB_PTIME_MAX_MS,
	       "RTP_MARKED_STEP_MS must be the widest buffer window");
_Static_assert(RTP_HELD_PAYLOAD_MAX == UDSP_JB_PAYLOAD_MAX,
	       "a packet held on probation must fit one slot");

enum jb_state {
	JB_STATE_IDLE = 0,
	JB_STATE_PREFILL,
	JB_STATE_PLAY,
};

enum jb_skew {
	JB_SKEW_NONE = 0,
	JB_SKEW_DROP,
	JB_SKEW_INSERT,
};

/*
 * RFC 3550 5.1: RTP timestamps and sequence numbers wrap. The unsigned
 * difference cast back to a signed type is the ordering test rtp.c already
 * relies on for the A.8 transit delta; it is implementation-defined and correct
 * on every compiler this tree builds with.
 */
static int32_t ts_delta(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b);
}

static int16_t seq_delta(uint16_t a, uint16_t b)
{
	return (int16_t)(a - b);
}

static uint64_t ms_to_ns(uint32_t ms)
{
	return (uint64_t)ms * 1000000ULL;
}

static uint32_t ms_to_ts(const struct udsp_jb *jb, uint32_t ms)
{
	return ms * jb->ts_per_ms;
}

static uint32_t ts_to_ms(const struct udsp_jb *jb, int32_t ts)
{
	if (ts <= 0)
		return 0;
	return (uint32_t)ts / jb->ts_per_ms;
}

/* The static reorder window in the stream's timestamp units. */
static int32_t jb_window_ts(const struct udsp_jb *jb)
{
	return (int32_t)ms_to_ts(jb, jb->max_ms);
}

/* The timestamp jump bound: ten seconds is a timeline step at any clock, so
 * the bound is held in time and converted here. rtp.h keeps the estimator's
 * half of the same rule. */
static uint32_t jb_resync_jump(const struct udsp_jb *jb)
{
	return JB_RESYNC_SEC * 1000U * jb->ts_per_ms;
}

static uint16_t ms_clamp(uint32_t ms, uint32_t low, uint32_t high)
{
	if (ms < low)
		ms = low;
	if (ms > high)
		ms = high;
	return (uint16_t)ms;
}

/*
 * A SID-shaped payload: N speech frames of frame_bytes and one trailing SID of
 * sid_len (codec.h Frames: the SID is the last frame of the packet and ends
 * it), or the SID alone. Returns the unit count, 0 for every other shape. The
 * count is derived from the payload instead of the learned span, because a SID
 * flush ends the packet below the learned ptime and a SID-only update spans
 * one frame: the learned span would stamp both wrong, slice them wrong, and
 * drift the playout clock by the difference; a first-packet SID would never
 * emit its SID slice at all. The bound is the widest packet the span learner
 * itself accepts plus the SID's own frame; a payload beyond it, or one whose
 * derived span the slot could not hold, belongs to no stream this buffer
 * covers and keeps the even slice, at store time and at emit time alike. A
 * whole_packet stream never slices, so it never has a SID shape either.
 */
static uint32_t jb_sid_units(const struct udsp_jb *jb, uint32_t len)
{
	uint32_t units = 1;

	if (jb->whole_packet || !jb->sid_len || len < jb->sid_len)
		return 0;
	if (len > jb->sid_len) {
		if (!jb->frame_bytes || (len - jb->sid_len) % jb->frame_bytes)
			return 0;
		units += (len - jb->sid_len) / jb->frame_bytes;
	}
	if (units > UDSP_JB_PTIME_MAX_MS / UDSP_JB_FRAME_MS + 1U)
		return 0;
	if (units * (uint32_t)jb->frame_ts > 0xffffU)
		return 0;
	return units;
}

/*
 * A payload of N whole speech frames on a stream that stated its frame size
 * (codec.h Frames: every speech frame of a fixed-frame codec encodes to
 * frame_bytes). Returns N, 0 for any other shape, for a whole_packet stream,
 * or when no frame size was stated. The learned span cannot say how long such
 * a packet is: a talkspurt that closes on an odd frame count ends on a packet
 * below the learned ptime, and the learned span would stamp it wrong, slice
 * it into pieces no decoder takes, and hold the playout clock over the comfort
 * noise that follows. The bound is the SID shape's.
 */
static uint32_t jb_frame_units(const struct udsp_jb *jb, uint32_t len)
{
	uint32_t units;

	if (jb->whole_packet || !jb->frame_bytes || !len ||
	    len % jb->frame_bytes)
		return 0;
	units = len / jb->frame_bytes;
	if (units > UDSP_JB_PTIME_MAX_MS / UDSP_JB_FRAME_MS)
		return 0;
	if (units * (uint32_t)jb->frame_ts > 0xffffU)
		return 0;
	return units;
}

/* The units a payload states by its own shape, the SID shape first; 0 leaves
 * the learned span to speak. */
static uint32_t jb_derived_units(const struct udsp_jb *jb, uint32_t len)
{
	uint32_t units = jb_sid_units(jb, len);

	return units ? units : jb_frame_units(jb, len);
}

/* The span one packet covers on the wire: derived from the payload for a SID
 * shape and for whole speech frames, learned for every other. The slot store,
 * the step candidate's continuation and the stream frontier all read this one
 * rule, so a SID-tailed or short close cannot leave a step successor reading
 * one frame early or a frontier standing past the audio it saw. */
static uint16_t jb_pkt_span(const struct udsp_jb *jb,
			    const struct udsp_jb_packet *pkt)
{
	uint32_t units = jb_derived_units(jb, (uint32_t)pkt->len);

	if (units)
		return (uint16_t)(units * jb->frame_ts);
	return jb->pkt_span_ts;
}

/* The packet ends with a SID: comfort noise in audio clothing, so the span
 * learner and the seq_high pair reference must not read it as a packet
 * duration. */
static bool jb_sid_tailed(const struct udsp_jb *jb,
			  const struct udsp_jb_packet *pkt)
{
	return pkt->kind == RTP_PAYLOAD_AUDIO &&
	       jb_sid_units(jb, (uint32_t)pkt->len) != 0;
}

static struct udsp_jb_slot *jb_slot(struct udsp_jb *jb, unsigned pos)
{
	return &jb->slot[jb->order[pos]];
}

static const struct udsp_jb_slot *jb_slot_const(const struct udsp_jb *jb,
						unsigned pos)
{
	return &jb->slot[jb->order[pos]];
}

static uint32_t jb_depth_ms(const struct udsp_jb *jb)
{
	const struct udsp_jb_slot *last;
	int32_t depth;

	if (!jb->count)
		return 0;
	last = jb_slot_const(jb, jb->count - 1U);
	depth = ts_delta(last->ts + last->span_ts, jb->playout_ts);
	return ts_to_ms(jb, depth);
}

/* The tail-based depth counts a single early packet parked far ahead of the
 * stream as if the queue held that much audio, and a clock-skew correction
 * acting on it discards the real reserve. The reach walks from the head and
 * stops at the first gap the buffer could never cover; a slot beyond it is
 * not queued audio, and neither is a head that far ahead of the clock. The
 * !jb_frame_ready gate in jb_skew_gate still keeps a drop from firing while
 * the head is not playable.
 *
 * The same walk sums the reachable audio for callers that pass audio_ts: only
 * the part of each span beyond the running end counts, so a head the clock
 * already entered contributes its unplayed remainder and slots overlapped by
 * a first-learn restamp are not double counted. The reach end would also span
 * the concealment gaps between slots. */
static uint32_t jb_reach_end_ts(const struct udsp_jb *jb, uint32_t *audio_ts)
{
	const struct udsp_jb_slot *slot;
	uint32_t audio = 0;
	int32_t gap_max;
	int32_t fresh;
	uint32_t end;
	unsigned i;

	end = jb->playout_ts;
	gap_max = jb_window_ts(jb);
	for (i = 0; i < jb->count; i++) {
		slot = jb_slot_const(jb, i);
		/* A slot the buffer could never cover is not reachable audio,
		 * and neither is a head that far ahead of the clock. Read as
		 * reach, such a slot inflates the averaged depth and arms the
		 * trend corrector, and it hides the sibling of a step candidate
		 * from the on-timeline test in jb_step_track. */
		if (ts_delta(slot->ts, end) > gap_max)
			break;
		fresh = ts_delta(slot->ts + slot->span_ts, end);
		if (fresh <= 0)
			continue;
		audio += fresh < slot->span_ts ? (uint32_t)fresh : slot->span_ts;
		end = slot->ts + slot->span_ts;
	}
	if (audio_ts)
		*audio_ts = audio;
	return end;
}

static uint32_t jb_depth_reach_ms(const struct udsp_jb *jb)
{
	if (!jb->count)
		return 0;
	return ts_to_ms(jb, ts_delta(jb_reach_end_ts(jb, NULL),
				     jb->playout_ts));
}

static bool jb_frame_ready(const struct udsp_jb *jb)
{
	return jb->count && ts_delta(jb->playout_ts, jb_slot_const(jb, 0)->ts) >= 0;
}

/* The far end announced silence and stopped sending: this position carries
 * comfort noise, not concealment, so it is neither an underrun nor part of the
 * concealment run (RFC 3389). Only the SID silence is bounded: the run bound
 * (JB_CN_RUN_MAX_MS) ends its free-run once nothing at all has arrived for
 * that long, and the clock then parks so the parked-clock recovery owns the
 * resume. An RFC 3389 description stays active for as long as it stands: its
 * senders repeat the description, and that repetition is the arrival the SID
 * path lacks. */
static bool jb_cn_active(const struct udsp_jb *jb)
{
	if (!jb->cn_armed || ts_delta(jb->playout_ts, jb->cn_ts) < 0)
		return false;
	return !jb->cn_sid || jb->cn_run_ms < JB_CN_RUN_MAX_MS;
}

/* With nothing queued the media clock holds in every mode: a clock that keeps
 * running past an empty queue drifts ahead of the sender for good, and every
 * later packet then arrives late. Holding is not a reorder wait, because the
 * queue head still plays at the first get after it arrives. */
static bool jb_hold_needed(const struct udsp_jb *jb)
{
	return !jb->count && !jb_cn_active(jb);
}

/* The slot on loan to the caller stays out of the pool, so no reset and no put
 * can overwrite a payload the caller still holds. */
static void jb_pool_reset(struct udsp_jb *jb)
{
	unsigned i;

	jb->pool_count = 0;
	for (i = 0; i < UDSP_JB_SLOTS; i++) {
		if (i != jb->loan)
			jb->pool[jb->pool_count++] = (uint8_t)i;
	}
	jb->count = 0;
}

static void jb_order_pop(struct udsp_jb *jb)
{
	uint8_t index = jb->order[0];

	if (index != jb->loan)
		jb->pool[jb->pool_count++] = index;
	jb->count--;
	memmove(&jb->order[0], &jb->order[1], jb->count);
}

/* The loan ends at the next get: the caller had the whole tick to run its
 * codec over the payload. */
static void jb_loan_release(struct udsp_jb *jb)
{
	if (jb->loan >= UDSP_JB_SLOTS)
		return;
	if (jb->count && jb->order[0] == jb->loan)
		return;
	jb->pool[jb->pool_count++] = jb->loan;
	jb->loan = UDSP_JB_SLOTS;
}

/* Insertion by RTP timestamp from the tail backwards: in-order arrival costs no
 * move at all and a reorder costs one memmove of the index array, never of a
 * payload. */
static void jb_order_insert(struct udsp_jb *jb, uint8_t index, uint32_t ts)
{
	unsigned pos = jb->count;

	while (pos && ts_delta(jb->slot[jb->order[pos - 1U]].ts, ts) > 0)
		pos--;
	memmove(&jb->order[pos + 1U], &jb->order[pos], jb->count - pos);
	jb->order[pos] = index;
	jb->count++;
}

static void jb_stale_purge(struct udsp_jb *jb)
{
	while (jb->count &&
	       ts_delta(jb->playout_ts,
			jb_slot(jb, 0)->ts + jb_slot(jb, 0)->span_ts) >= 0)
		jb_order_pop(jb);
}

/* The averaged depth is the control input and its 4 s history is the drift
 * evidence, so both are seeded together whenever playout (re)starts. */
static void jb_depth_seed(struct udsp_jb *jb, uint32_t depth_ms)
{
	unsigned i;

	jb->depth_avg_q8 = depth_ms << 8;
	for (i = 0; i < UDSP_JB_TREND_BUCKETS; i++)
		jb->depth_hist_q8[i] = jb->depth_avg_q8;
}

static void jb_window_advance(struct udsp_jb *jb, uint64_t now_ns)
{
	uint64_t steps;

	if (now_ns <= jb->window_ns)
		return;
	steps = (now_ns - jb->window_ns) / ms_to_ns(JB_BUCKET_MS);
	if (!steps)
		return;
	if (steps >= UDSP_JB_TREND_BUCKETS) {
		memset(jb->window_peak, 0, sizeof(jb->window_peak));
		jb_depth_seed(jb, jb->depth_avg_q8 >> 8);
		jb->window_head = 0;
		jb->window_ns = now_ns;
		return;
	}
	while (steps--) {
		jb->window_head = (uint8_t)((jb->window_head + 1U) %
					    UDSP_JB_TREND_BUCKETS);
		jb->window_peak[jb->window_head % UDSP_JB_WINDOW_BUCKETS] = 0;
		jb->depth_hist_q8[jb->window_head] = jb->depth_avg_q8;
		jb->window_ns += ms_to_ns(JB_BUCKET_MS);
	}
}

static uint32_t jb_window_high_water(const struct udsp_jb *jb)
{
	uint32_t peak = 0;
	unsigned i;

	for (i = 0; i < UDSP_JB_WINDOW_BUCKETS; i++) {
		if (jb->window_peak[i] > peak)
			peak = jb->window_peak[i];
	}
	return peak;
}

/* Timing state only. Cumulative totals survive, as they do across the
 * per-SSRC timing reset in rtp.c. */
static void jb_timing_reset(struct udsp_jb *jb)
{
	jb_pool_reset(jb);
	memset(jb->window_peak, 0, sizeof(jb->window_peak));
	jb->state = JB_STATE_IDLE;
	jb->skew_pending = JB_SKEW_NONE;
	jb->window_head = 0;
	jb->late_want_ms = 0;
	jb->span_candidate_ts = 0;
	jb->span_agree = 0;
	jb->hold_run = 0;
	jb->conceal_run_ms = 0;
	jb->cn_valid = false;
	jb->cn_armed = false;
	jb->cn_sid = false;
	jb->cn_len = 0;
	jb->cn_run_ms = 0;
	jb->seq_high_sid = false;
	/* The emitted-unit mark belongs to the playout position (jb.h Units),
	 * and every reset moves it. jb_anchor needs no clear of its own: it is
	 * reached from JB_STATE_IDLE, where the mark is clear since the init,
	 * and from jb_stream_reset, which arrives through this reset. */
	jb->emit_unit_valid = false;
	jb->emit_unit_ts = 0;
	jb->have_seq = false;
	jb->have_arrival = false;
	jb->playout_ts = 0;
	jb->last_rx_ts = 0;
	jb->cn_ts = 0;
	jb->step_ts = 0;
	jb->step_seq = 0;
	jb->step_agree = 0;
	jb->stream_end_ts = 0;
	jb->jitter_q4 = 0;
	jb_depth_seed(jb, 0);
	jb->seq_mask = 0;
	jb->seq_high = 0;
	jb->seq_high_ts = 0;
	jb->anchor_ns = 0;
	jb->fill_ns = 0;
	jb->window_ns = 0;
	jb->last_arrival_ns = 0;
	jb->last_skew_ns = 0;
	jb->shrink_ns = 0;
	jb->trim_calm_ns = 0;
	jb->trim_pending = false;
	jb->target_ms = jb->init_ms;
}

void udsp_jb_init_cfg(struct udsp_jb *jb, enum udsp_jb_mode mode,
		      uint16_t init_ms, uint16_t max_ms, uint16_t ts_per_ms,
		      uint16_t frame_ts, uint16_t frame_bytes, uint16_t sid_len,
		      bool whole_packet)
{
	if (!jb)
		return;
	memset(jb, 0, sizeof(*jb));
	jb->ts_per_ms = ts_per_ms ? ts_per_ms : (uint16_t)UDSP_JB_TS_PER_MS;
	jb->frame_ts = frame_ts ? frame_ts :
		       (uint16_t)(jb->ts_per_ms * UDSP_JB_FRAME_MS);
	jb->frame_bytes = frame_bytes;
	/* A SID longer than the comfort-noise slot could never be stored or
	 * refreshed whole, so the configuration is refused and the SID
	 * machinery stays dead, rather than truncating decoder input. */
	jb->sid_len = sid_len <= UDSP_JB_CN_MAX ? sid_len : 0;
	jb->whole_packet = whole_packet;
	jb->mode = (uint8_t)mode;
	if (mode != UDSP_JB_ADAPTIVE && mode != UDSP_JB_FIXED &&
	    mode != UDSP_JB_OFF)
		jb->mode = UDSP_JB_ADAPTIVE;
	jb->init_ms = ms_clamp(init_ms, UDSP_JB_FRAME_MS, UDSP_JB_DEPTH_MAX_MS);
	jb->max_ms = ms_clamp(max_ms, jb->init_ms, UDSP_JB_DEPTH_MAX_MS);
	if (jb->mode == UDSP_JB_OFF)
		jb->init_ms = 0;
	/* The far end's ptime is a property of the sender, not of the timeline,
	 * so it survives every resync and is relearned only from new consecutive
	 * packets. */
	jb->pkt_span_ts = jb->frame_ts;
	jb->span_learned = false;
	jb->loan = UDSP_JB_SLOTS;
	jb_timing_reset(jb);
}

void udsp_jb_init(struct udsp_jb *jb, enum udsp_jb_mode mode, uint16_t init_ms,
		  uint16_t max_ms)
{
	udsp_jb_init_cfg(jb, mode, init_ms, max_ms, UDSP_JB_TS_PER_MS,
			 UDSP_JB_FRAME_TS, 0, 0, false);
}

/*
 * A data carrier is demodulated from the very samples the playout moves, so
 * the switch to a fixed buffer must keep the queue and the depth the line
 * proved it needs. jb_target_update reads init_ms in UDSP_JB_FIXED, so the
 * pin is a write of init_ms and nothing else: the queue, the span learner and
 * the playout clock are the state that carries the audio across the switch.
 */
void udsp_jb_mode_set(struct udsp_jb *jb, enum udsp_jb_mode mode)
{
	if (!jb)
		return;
	if (mode != UDSP_JB_ADAPTIVE && mode != UDSP_JB_FIXED &&
	    mode != UDSP_JB_OFF)
		return;
	/*
	 * A trim-armed drop run is the adaptive surplus being shed, and
	 * jb_skew_decide resumes a pending run before it reaches the trim gate
	 * that armed it, so the run would carry on into a mode that never
	 * arms one. A trend-armed run is not cancelled: it corrects a proven
	 * clock drift, which the fixed mode corrects too.
	 */
	if (jb->mode == UDSP_JB_ADAPTIVE && mode != UDSP_JB_ADAPTIVE &&
	    jb->trim_pending) {
		jb->skew_pending = JB_SKEW_NONE;
		jb->trim_pending = false;
	}
	jb->mode = (uint8_t)mode;
	if (mode != UDSP_JB_FIXED)
		return;
	jb->init_ms = ms_clamp(jb->target_ms, UDSP_JB_FRAME_MS, jb->max_ms);
}

static void jb_anchor(struct udsp_jb *jb, const struct udsp_jb_packet *pkt)
{
	jb->timeline++;
	jb->playout_ts = pkt->rtp_timestamp;
	jb->stream_end_ts = pkt->rtp_timestamp;
	jb->anchor_ns = pkt->arrival_ns;
	jb->fill_ns = pkt->arrival_ns;
	jb->shrink_ns = pkt->arrival_ns;
	jb->trim_calm_ns = pkt->arrival_ns;
	jb->last_skew_ns = 0;
	jb->skew_pending = JB_SKEW_NONE;
	jb->trim_pending = false;
	jb->conceal_run_ms = 0;
	jb_depth_seed(jb, jb->target_ms);
	jb->state = jb->mode == UDSP_JB_OFF ? JB_STATE_PLAY : JB_STATE_PREFILL;
}

/*
 * The queue ran dry and the media clock parked. This packet decides where the
 * stream continues: at its own timestamp when it is ahead (the frames between
 * are lost or were never sent), or where the clock parked when it is not. In
 * UDSP_JB_ADAPTIVE a forward jump beyond one max window plus one learned span
 * is not adopted at once: it waits for a second packet that confirms the new
 * timeline, or for the refusal after JB_HOLD_MAX consecutive holds. The
 * buffer then fills to the target again before it plays, so one outage does
 * not leave the stream running with no reserve; concealment continues
 * meanwhile, which is why this is not the cold-start prefill.
 */
static void jb_refill(struct udsp_jb *jb, const struct udsp_jb_packet *pkt)
{
	if (ts_delta(pkt->rtp_timestamp, jb->playout_ts) > 0)
		jb->playout_ts = pkt->rtp_timestamp;
	jb->fill_ns = pkt->arrival_ns;
	jb->hold_run = 0;
	/* An adopted forward jump is a timeline event, not jitter: the
	 * reference the next observation would measure against is stale by
	 * the whole jump, so it re-seeds without contributing. */
	jb->have_arrival = false;
	/* The re-anchor moves the frame of reference a pending step
	 * observation was measured against, and equally the depth a pending
	 * clock-skew correction was armed on. A bound adoption arrives with the
	 * candidate still armed on the previous held packet, whose continuation
	 * is the adopted timeline itself; without this clear a packet on that
	 * timeline confirms the candidate and resyncs the stream the refill
	 * just adopted. */
	jb->step_agree = 0;
	jb->skew_pending = JB_SKEW_NONE;
	jb->trim_pending = false;
	jb->trim_calm_ns = pkt->arrival_ns;
	/* The clock left the unit the last frame output came from, so whatever
	 * unit the refill starts on is fresh (jb.h Units). */
	jb->emit_unit_valid = false;
	jb->state = JB_STATE_PREFILL;
}

/* Every timeline reset (SSRC change, timestamp jump, starve resync, step
 * confirm) funnels here and throws the queue away, so the reachable audio sum
 * at entry is the exact playable audio the reset discarded. The reach end
 * would overstate it by the concealment gaps between slots (measured 510 vs
 * 380 ms on the reorder-churn shape). Every other queue-dropping path needs
 * no count of its own: it runs with count == 0, it moves the clock backward
 * and discards nothing (the prefill pull-back), or its discard is already the
 * counted correction (the skew drop purges the frame its own skew_drops
 * counter records). */
static void jb_stream_reset(struct udsp_jb *jb, const struct udsp_jb_packet *pkt)
{
	uint32_t audio_ts;

	jb_reach_end_ts(jb, &audio_ts);
	jb->total.resync_discard_ms += ts_to_ms(jb, (int32_t)audio_ts);
	jb_timing_reset(jb);
	jb->ssrc = pkt->ssrc;
	jb_anchor(jb, pkt);
}

/* Modular magnitude: negating the signed difference would be undefined at
 * INT32_MIN, which is exactly a half-range timestamp step. */
static bool jb_ts_jump(const struct udsp_jb *jb, const struct udsp_jb_packet *pkt)
{
	uint32_t delta = pkt->rtp_timestamp - jb->last_rx_ts;

	if (delta > 0x80000000U)
		delta = 0U - delta;
	return delta > jb_resync_jump(jb);
}

/* Replay window over the last 64 sequence numbers: a duplicate is caught
 * whether its original is still queued or already played (RFC 3550 A.1 keeps
 * only the highest sequence, which cannot answer that question). seq_high_ts
 * and seq_high_sid move along: the span learner pairs against the seq_high
 * packet, so the timestamp and the SID mark must move exactly where seq_high
 * moves. */
static bool jb_seq_duplicate(struct udsp_jb *jb, const struct udsp_jb_packet *pkt)
{
	unsigned back;
	int32_t delta;

	if (!jb->have_seq) {
		jb->have_seq = true;
		jb->seq_high = pkt->seq;
		jb->seq_high_ts = pkt->rtp_timestamp;
		jb->seq_high_sid = jb_sid_tailed(jb, pkt);
		jb->seq_mask = 1;
		return false;
	}
	delta = seq_delta(pkt->seq, jb->seq_high);
	if (delta > 0) {
		jb->seq_mask = delta >= 64 ? 0 : jb->seq_mask << delta;
		jb->seq_mask |= 1;
		jb->seq_high = pkt->seq;
		jb->seq_high_ts = pkt->rtp_timestamp;
		jb->seq_high_sid = jb_sid_tailed(jb, pkt);
		return false;
	}
	back = (unsigned)(-delta);
	if (back >= 64)
		return false;
	if (jb->seq_mask & (1ULL << back))
		return true;
	jb->seq_mask |= 1ULL << back;
	return false;
}

/*
 * The timestamp jump bound and an armed SID silence disagree about a long
 * DTX gap: an Annex B sender's timestamp keeps counting through a silence it
 * never transmits, so the talkspurt resume arrives with the whole silence as
 * its delta from the SID. Without an exemption, a resume past JB_RESYNC_SEC
 * reads as a timeline step and pays a resync and a prefill. The
 * resume is exempt when the sender vouches for it (the RFC 3551 marker, or
 * the successor sequence number: DTX consumes no sequence numbers, so the
 * resume follows the SID directly) AND its distance from the playout clock
 * stays inside the resync bound: the free-running comfort-noise clock tracked
 * the sender through the silence (and stands at most JB_CN_RUN_MAX_MS behind
 * it after the park), so a genuine resume falls within that bound while a
 * foreign timeline falls outside it with near certainty, and still resyncs.
 * Audio only: a comfort-noise packet at such a distance stays owned by the
 * jump resync, as everywhere else in this file.
 */
static bool jb_dtx_resume_exempt(const struct udsp_jb *jb,
				 const struct udsp_jb_packet *pkt)
{
	uint32_t dist;

	if (!jb->cn_armed || !jb->cn_sid || pkt->kind != RTP_PAYLOAD_AUDIO)
		return false;
	if (!pkt->marker &&
	    !(jb->have_seq && seq_delta(pkt->seq, jb->seq_high) == 1))
		return false;
	dist = pkt->rtp_timestamp - jb->playout_ts;
	if (dist > 0x80000000U)
		dist = 0U - dist;
	return dist <= jb_resync_jump(jb);
}

/* The far bound of the deferred refill anchor, measured against the playout
 * clock only. The frontier is deliberately not consulted: a held packet
 * raises stream_end_ts, so a frontier-gated refusal could never re-admit a
 * genuine resume into the window the junk packet moved (measured as a
 * permanent hold livelock). The parked clock is the one reference a
 * mis-stamp cannot move. */
static bool jb_refill_far(const struct udsp_jb *jb,
			  const struct udsp_jb_packet *pkt)
{
	return ts_delta(pkt->rtp_timestamp, jb->playout_ts) >
	       jb_window_ts(jb) + jb->pkt_span_ts;
}

/* The step candidate's arming bound, shared between jb_step_track and the
 * comfort-noise distrust below: a packet beyond the stream frontier by the
 * max window and beyond the playout clock by the window plus one span is off
 * every timeline the buffer could cover. */
static bool jb_ts_far(const struct udsp_jb *jb,
		      const struct udsp_jb_packet *pkt)
{
	return ts_delta(pkt->rtp_timestamp, jb->stream_end_ts) >
	       jb_window_ts(jb) && jb_refill_far(jb, pkt);
}

/* In UDSP_JB_ADAPTIVE the confirming packet's distance from the playout clock
 * must also clear JB_STEP_GATE_K times the internal jitter estimate. The
 * static bound alone is no proof on a line whose measured jitter routinely
 * puts packets beyond it: there the reorder tail supplies correlated pairs
 * that satisfy the two-packet agreement, and each false resync discards the
 * queue and pays a prefill. The gate follows the estimator, decay window
 * included: for the 16..32 packets after a lone delayed packet the estimate
 * still carries the event, and a genuine step arriving inside that echo is
 * blocked and absorbs (an accepted residual: in band the step is
 * indistinguishable from a second echo of the stall). The shift runs before
 * the multiply: the product then stays inside int32_t for every uint32_t
 * estimate up to 2^28 ticks, with no reliance on the estimator's dynamic
 * bound. */
static bool jb_confirm_far(const struct udsp_jb *jb,
			   const struct udsp_jb_packet *pkt, int32_t bound)
{
	int32_t gate = (int32_t)(JB_STEP_GATE_K * (jb->jitter_q4 >> 4));

	if (gate < bound)
		gate = bound;
	return ts_delta(pkt->rtp_timestamp, jb->playout_ts) > gate;
}

/* A packet the recovery machinery refuses to trust contributes no observation
 * and does not serve as the next observation's reference. Two kinds are
 * refused: audio the parked hold refuses, and comfort noise beyond jb_ts_far
 * or, while the clock is parked, beyond the playout-only bound. The refusal
 * invalidates the reference instead of keeping a private one: the next sane
 * packet re-seeds and contributes nothing, so every observed pair is two
 * consecutively accepted packets and the timestamp jump bound holds for
 * ts_step by construction. */
static void jb_jitter_update(struct udsp_jb *jb,
			     const struct udsp_jb_packet *pkt, bool refused)
{
	uint64_t arrival_delta_ns;
	int32_t variation;
	int32_t ts_step;
	uint32_t jitter;

	if (refused) {
		jb->have_arrival = false;
		return;
	}
	/* jb.h tail: the shared stamp is one arrival, not two. */
	if (pkt->tail)
		return;
	if (!jb->have_arrival) {
		jb->have_arrival = true;
		return;
	}
	if (pkt->arrival_ns <= jb->last_arrival_ns)
		arrival_delta_ns = 0;
	else
		arrival_delta_ns = pkt->arrival_ns - jb->last_arrival_ns;
	if (arrival_delta_ns > ms_to_ns(JB_RESYNC_SEC * 1000U))
		return;
	ts_step = ts_delta(pkt->rtp_timestamp, jb->last_rx_ts);
	/* RFC 3550 A.8 D(i-1,i) in its difference-of-differences form, so no
	 * absolute arrival-to-RTP conversion is needed and the 32-bit wrap of
	 * the timestamp term is the only wrap in play. The arrival term counts
	 * in the stream's own units, which is what ts_step measures in. */
	variation = (int32_t)(arrival_delta_ns * jb->ts_per_ms / 1000000ULL) -
		    ts_step;
	/* A marked audio packet whose timestamp ran ahead of its arrival by
	 * more than the buffer could ever cover is the sender's timeline
	 * moving (rtp.h RTP_MARKED_STEP_MS), not transit jitter. Read as
	 * jitter, a timeline step of a field call takes the target to max_ms.
	 * The packet stays the reference, because it sits on the new
	 * timeline. A delayed packet runs the other way and still counts. */
	if (pkt->kind == RTP_PAYLOAD_AUDIO && pkt->marker &&
	    variation < -(jb_window_ts(jb) + (int32_t)jb->pkt_span_ts))
		return;
	if (variation < 0)
		variation = -variation;
	jb->jitter_q4 += (uint32_t)variation - ((jb->jitter_q4 + 8U) >> 4);
	jitter = jb->jitter_q4 >> 4;
	if (jitter > jb->window_peak[jb->window_head % UDSP_JB_WINDOW_BUCKETS])
		jb->window_peak[jb->window_head % UDSP_JB_WINDOW_BUCKETS] = jitter;
}

/* On the first learn the queued slots may take the span the pair just
 * established; otherwise the first packets of every stream would be sliced as
 * if they carried a single frame. In JB_STATE_PREFILL nothing has played and
 * every slot follows. A first learn can also occur in JB_STATE_PLAY, when deep
 * startup reorder starves the learner past the whole prefill; there the
 * behind-clock guard skips a slot the clock is inside. On the 10 ms
 * timestamp grid that slot cannot exist at put time (every pre-learn slot
 * carries the one-frame span and leaves the queue in the get that emits it),
 * so the guard defends one thing: an off-grid sender whose stored slot
 * straddles the clock, where a restamp would re-index slices the caller
 * already played. The JB_SPAN_AGREE change path never restamps: an
 * unsignalled mid-stream change keeps queued slots on the span they were
 * stored under. */
static void jb_span_restamp(struct udsp_jb *jb)
{
	struct udsp_jb_slot *slot;
	unsigned i;

	for (i = 0; i < jb->count; i++) {
		slot = &jb->slot[jb->order[i]];
		if (jb->state != JB_STATE_PREFILL &&
		    ts_delta(slot->ts, jb->playout_ts) < 0)
			continue;
		/* A slot whose span is derived from its own payload is not the
		 * learner's to rewrite. */
		if (jb_derived_units(jb, slot->len))
			continue;
		slot->span_ts = jb->pkt_span_ts;
	}
}

/*
 * The learning pair is the arriving packet and the seq_high packet whenever
 * the two are within three sequence numbers, measured in sender order
 * through the timestamp seq_high carries. The packet that arrived before is
 * not the sender's neighbour once packets reorder: a delta against it
 * either never forms (a swap pattern keeps every arrival away from
 * seq_high, so the learner starves and 20 ms packets are emitted as single
 * units no codec accepts) or measures two spans across a hole the reorder
 * left (the false first learn). A gap-n pair on a linear timeline measures
 * exactly n spans, across genuine losses as well, so the delta is normalised
 * by the gap; a delta the gap does not divide is not a linear pair and
 * teaches nothing. Three is the step confirm window's own tolerance; a deeper
 * gap spans too much loss to trust one delta. Comfort noise takes part like
 * any packet: it consumes a sequence number and RFC 3389 stamps it where the
 * next audio frame would have started.
 */
static int32_t jb_span_pair_delta(const struct udsp_jb *jb,
				  const struct udsp_jb_packet *pkt)
{
	int32_t delta;
	int32_t gap;

	if (!jb->have_seq)
		return 0;
	gap = seq_delta(pkt->seq, jb->seq_high);
	if (!gap || gap > 3 || gap < -3)
		return 0;
	if (gap > 0) {
		delta = ts_delta(pkt->rtp_timestamp, jb->seq_high_ts);
	} else {
		delta = ts_delta(jb->seq_high_ts, pkt->rtp_timestamp);
		gap = -gap;
	}
	if (delta <= 0 || delta % gap)
		return 0;
	return delta / gap;
}

/*
 * The stream span is learned from sequence-near packets only (the pair rule
 * above; a non-pair passes delta 0). The first plausible observation is
 * adopted at once while nothing has played since the init, and only then:
 * the caller re-inits on every media reconfiguration, and before the first
 * emitted frame the adoption rewrites no audio anyone heard. A first learn
 * on a stream that already played takes the same corroboration an
 * established span demands: a single pair there can be a comfort-noise
 * boundary or a mis-stamp, and adopting it at once rewrites the whole queue
 * (measured: 45 wrong-length units on a reordered VAD stream). Changing an
 * established span needs JB_SPAN_AGREE agreeing observations for the same
 * reason: one packet lost in a 10 ms stream makes a single raw delta look
 * like a 20 ms ptime, and slicing an 80-byte frame as two 40-byte halves
 * corrupts audio on every loss. Slicing itself never reads a neighbour's
 * timestamp.
 */
static void jb_span_learn(struct udsp_jb *jb, int32_t delta)
{
	if (delta <= 0 || delta > (int32_t)ms_to_ts(jb, UDSP_JB_PTIME_MAX_MS))
		return;
	if ((uint32_t)delta % jb->frame_ts)
		return;
	if (!jb->span_learned && !jb->total.frames_out && !jb->skip_frames) {
		/* A pending step candidate survives this learn. The old span
		 * is still the init value, one frame, the least the learner
		 * accepts, so a continuation measured with it lies at or
		 * before the true one, and the confirm window takes it. A
		 * derived span never reads the learned one. A clear here lets a
		 * step and its successor that arrive in one tick at call start
		 * teach the span and void the candidate, with the frontier
		 * already moved. */
		jb->pkt_span_ts = (uint16_t)delta;
		jb->span_learned = true;
		jb_span_restamp(jb);
		return;
	}
	if (!jb->span_learned) {
		if ((uint16_t)delta != jb->span_candidate_ts) {
			jb->span_candidate_ts = (uint16_t)delta;
			jb->span_agree = 1;
			return;
		}
		if (++jb->span_agree < JB_SPAN_AGREE)
			return;
		jb->pkt_span_ts = jb->span_candidate_ts;
		jb->span_learned = true;
		jb->span_agree = 0;
		jb->step_agree = 0;
		jb_span_restamp(jb);
		return;
	}
	if ((uint16_t)delta == jb->pkt_span_ts) {
		jb->span_agree = 0;
		return;
	}
	if ((uint16_t)delta != jb->span_candidate_ts) {
		jb->span_candidate_ts = (uint16_t)delta;
		jb->span_agree = 1;
		return;
	}
	if (++jb->span_agree < JB_SPAN_AGREE)
		return;
	jb->pkt_span_ts = jb->span_candidate_ts;
	jb->span_agree = 0;
	jb->step_agree = 0;
}

static void jb_late_grow(struct udsp_jb *jb, int32_t lateness)
{
	uint32_t want;

	want = (uint32_t)jb->target_ms + ts_to_ms(jb, lateness) +
	       UDSP_JB_FRAME_MS;
	if (want > jb->max_ms)
		want = jb->max_ms;
	if (want > jb->late_want_ms)
		jb->late_want_ms = (uint16_t)want;
	jb->shrink_ns = jb->now_ns;
	jb->trim_calm_ns = jb->now_ns;
}

/* Audio that resumes behind an active comfort-noise period is not late: the far
 * end restarted where it stopped while our clock ran on through the silence.
 * A resume packet seq-consecutive with the comfort noise is the far end's own
 * continuation wherever its clock stopped, so it re-anchors at any distance.
 * The max_ms bound alone turns a silence longer than max_ms with a far
 * timestamp that stands still into a permanent late-drop livelock,
 * because jb_hold_needed is false while comfort noise is active and none of
 * the parked-clock recovery can run. The bound still rejects stray old
 * packets, which are never seq-consecutive. */
static bool jb_cn_resume(const struct udsp_jb *jb,
			 const struct udsp_jb_packet *pkt, bool successor)
{
	int32_t behind;

	if (jb->state != JB_STATE_PLAY || jb->count || !jb->cn_armed)
		return false;
	/* The successor test runs before the signed behind test: a resume more
	 * than 2^31 timestamp ticks (74.6 hours at 8 kHz) behind wraps
	 * ts_delta positive and the signed test would refuse it. */
	if (successor)
		return true;
	behind = ts_delta(jb->playout_ts, pkt->rtp_timestamp);
	return behind > 0 && behind <= jb_window_ts(jb);
}

static int jb_cn_store(struct udsp_jb *jb, const struct udsp_jb_packet *pkt)
{
	size_t len = pkt->len;

	if (len > UDSP_JB_CN_MAX)
		len = UDSP_JB_CN_MAX;
	if (len)
		memcpy(jb->cn_payload, pkt->payload, len);
	jb->cn_len = (uint16_t)len;
	jb->cn_ts = pkt->rtp_timestamp;
	jb->cn_valid = true;
	jb->cn_armed = true;
	/* Last write wins on the shared slot: an RFC 3389 description over a
	 * codec SID means silence either way, and without the clear the
	 * consumer would hand the RFC 3389 octet to the codec's own decode. */
	jb->cn_sid = false;
	jb->cn_run_ms = 0;
	return 0;
}

/*
 * A SID-only packet while SID comfort noise is armed is the far end updating
 * its noise description mid-silence, not a talkspurt: it refreshes the shared
 * comfort-noise slot in place, and the stored payload goes out on the next
 * gap tick exactly once. Neither the playout clock nor cn_ts moves, so a
 * late or reordered update cannot rewind playout the way jb_cn_reanchor
 * would; only a speech-bearing packet resumes the talkspurt.
 */
static int jb_cn_sid_refresh(struct udsp_jb *jb,
			     const struct udsp_jb_packet *pkt)
{
	size_t len = pkt->len;

	if (len > UDSP_JB_CN_MAX)
		len = UDSP_JB_CN_MAX;
	if (len)
		memcpy(jb->cn_payload, pkt->payload, len);
	jb->cn_len = (uint16_t)len;
	jb->cn_valid = true;
	jb->cn_armed = true;
	jb->cn_sid = true;
	jb->cn_run_ms = 0;
	return 0;
}

static void jb_cn_reanchor(struct udsp_jb *jb, const struct udsp_jb_packet *pkt)
{
	jb->total.cn_reanchors++;
	jb->playout_ts = pkt->rtp_timestamp;
	jb->emit_unit_valid = false;
	jb->cn_armed = false;
	/* The talkspurt resumed, so the silence description is spent: a stale
	 * SID left valid would reach the codec decode on the next loss
	 * ladder, and a stale RFC 3389 level belongs to a silence that is
	 * over. */
	jb->cn_sid = false;
	jb->cn_valid = false;
	jb->cn_len = 0;
}

/*
 * The step resync. Without it, a forward timestamp step below the timestamp
 * jump bound leaves the clock concealing the whole gap while the pool
 * overflows at the latency the step created, for up to minutes.
 * Two audio packets that arrive beyond what the buffer could ever cover and
 * agree on the new timeline prove a timeline move, and one resync costs one
 * prefill. In UDSP_JB_ADAPTIVE the confirming packet must also clear the
 * jitter-scaled distance gate (jb_confirm_far).
 *
 * The arming reference is the stream frontier stream_end_ts, the end of the
 * newest accepted audio packet whether or not its slot fit the pool: unlike
 * the last received timestamp it never regresses on a reordered or
 * mis-stamped packet, and unlike the queue reach it keeps rising through an
 * overflow regime, so neither a backward mis-stamp behind a deep queue nor
 * the packets after an overflow gap can read as a step. The candidate holds
 * its own sequence number and a sequence agreement of 1..3 confirms, so one
 * lost or swapped packet at the step boundary cannot defeat the proof; the
 * confirmation is checked first so a stale candidate stays confirmable,
 * which is why every re-anchor (refill, resync, relearned span) must clear
 * the candidate. The first learn before any frame played is not a re-anchor
 * and keeps it (jb_span_learn). A packet near the queue reach is on-timeline
 * traffic and voids the candidate; a packet between reach and frontier
 * decides nothing.
 * UDSP_JB_OFF keeps no recovery machinery, and comfort noise neither arms nor
 * confirms, so a mis-stamped CN packet cannot start a candidate that one
 * stray audio packet completes.
 *
 * The tracker runs in PREFILL as well as in PLAY. RTP source probation (rtp.c)
 * refuses the first packet of a stream, so a sender that steps on its third
 * packet puts the step on the buffer's second one, before the prefill ends.
 * A tracker gated on PLAY lets that packet move the frontier unobserved. Every
 * later packet then sits on the frontier, and the tracker never sees the step:
 * in a field call, that overflowed the pool for 80 s.
 */
static bool jb_step_track(struct udsp_jb *jb, const struct udsp_jb_packet *pkt)
{
	int32_t window = jb_window_ts(jb);
	int32_t bound = window + jb->pkt_span_ts;
	bool confirmed = false;
	int32_t seq_gap;
	int32_t cont;

	if (pkt->kind != RTP_PAYLOAD_AUDIO)
		return false;
	if (jb->mode == UDSP_JB_OFF)
		goto frontier;
	if (jb->step_agree) {
		seq_gap = seq_delta(pkt->seq, jb->step_seq);
		cont = ts_delta(pkt->rtp_timestamp, jb->step_ts);
		if (seq_gap >= 1 && seq_gap <= 3 && cont >= 0 && cont <= window) {
			/* The distance gate runs in UDSP_JB_ADAPTIVE only: a
			 * gate-blocked genuine step in UDSP_JB_FIXED sheds
			 * carrier frames at the trend rate for tens of
			 * seconds, the loss class the fax path must never
			 * take. A gate-blocked confirm clears the candidate
			 * as well, deliberately: the reorder tail supplies
			 * agreeing pairs continuously, and a candidate that
			 * stayed armed would confirm on the first pair whose
			 * distance moves past the gate. */
			jb->step_agree = 0;
			confirmed = jb->mode != UDSP_JB_ADAPTIVE ||
				    jb_confirm_far(jb, pkt, bound);
			goto frontier;
		}
	}
	if (jb_ts_far(jb, pkt)) {
		jb->step_agree = 1;
		jb->step_seq = pkt->seq;
		jb->step_ts = pkt->rtp_timestamp + jb_pkt_span(jb, pkt);
	} else if (ts_delta(pkt->rtp_timestamp, jb_reach_end_ts(jb, NULL)) <= bound) {
		jb->step_agree = 0;
	}
frontier:
	if (ts_delta(pkt->rtp_timestamp + jb_pkt_span(jb, pkt),
		     jb->stream_end_ts) > 0)
		jb->stream_end_ts = pkt->rtp_timestamp + jb_pkt_span(jb, pkt);
	return confirmed;
}

static int jb_drop_late(struct udsp_jb *jb, int32_t lateness)
{
	jb->total.late_drop++;
	jb_late_grow(jb, lateness);
	return -ETIME;
}

/* The media clock is parked: the queue ran dry and the last get concealed.
 * UDSP_JB_OFF is excluded, so neither the refill nor the starvation re-anchor
 * can act on a buffer the caller asked to have no recovery machinery. */
static bool jb_parked(const struct udsp_jb *jb)
{
	return jb->state == JB_STATE_PLAY && jb->mode != UDSP_JB_OFF &&
	       jb->conceal_run_ms && jb_hold_needed(jb);
}

/* A clock parked for a full second has lost the far end's timeline (a skew
 * beyond the correction rate, or a sender that resumed behind our position).
 * Re-anchoring costs one glitch; dropping every further packet costs the call. */
static bool jb_late_starved(const struct udsp_jb *jb,
			    const struct udsp_jb_packet *pkt)
{
	if (!jb_parked(jb) || jb->conceal_run_ms < JB_STARVE_MS)
		return false;
	return ts_delta(jb->playout_ts, pkt->rtp_timestamp) > 0;
}

/* A far-ahead packet on a parked clock is refused until a second packet
 * confirms the new timeline or the run reaches JB_HOLD_MAX. The candidate is
 * armed here directly, not through jb_step_track's frontier test: this held
 * packet already raised the frontier, so a frontier-gated arm could never
 * fire for the packets after it and the stream would hold forever. The hold
 * calls no jb_late_grow: the lateness is negative, and growing the target one
 * frame per junk packet is half of the churn this mechanism removes. */
static int jb_drop_held(struct udsp_jb *jb, const struct udsp_jb_packet *pkt)
{
	jb->hold_run++;
	jb->step_agree = 1;
	jb->step_seq = pkt->seq;
	jb->step_ts = pkt->rtp_timestamp + jb_pkt_span(jb, pkt);
	jb->total.hold_drop++;
	return -ETIME;
}

static int jb_slot_store(struct udsp_jb *jb, const struct udsp_jb_packet *pkt)
{
	struct udsp_jb_slot *slot;
	uint8_t index;

	if (!jb->pool_count) {
		jb->total.overflow_drop++;
		return -ENOBUFS;
	}
	index = jb->pool[--jb->pool_count];
	slot = &jb->slot[index];
	slot->ts = pkt->rtp_timestamp;
	slot->seq = pkt->seq;
	slot->len = (uint16_t)pkt->len;
	/* The SID ends the packet (jb.h SID), so the span is the payload's own
	 * unit count, whatever the learner believes: exact playout in both jb
	 * modes, and a first-packet SID emits before the learner settles. */
	slot->span_ts = jb_pkt_span(jb, pkt);
	slot->marker = pkt->marker;
	if (pkt->len)
		memcpy(slot->payload, pkt->payload, pkt->len);
	jb_order_insert(jb, index, pkt->rtp_timestamp);
	/* A stored packet is on a timeline the buffer covers, so a later
	 * refusal starts a new run: refusals prove a lost timeline only while
	 * they follow one another. It is an arrival, so the comfort-noise
	 * free-run bound starts over as well. */
	jb->hold_run = 0;
	jb->cn_run_ms = 0;
	return 0;
}

int udsp_jb_put(struct udsp_jb *jb, const struct udsp_jb_packet *pkt)
{
	bool successor;
	bool sid_tail;
	bool sid_refresh;
	bool cn_refused;
	bool hold_want;
	bool parked;
	bool step;
	int32_t span_delta;
	int32_t lateness;

	if (!jb || !pkt || (!pkt->payload && pkt->len))
		return -EINVAL;
	if (pkt->kind != RTP_PAYLOAD_AUDIO &&
	    pkt->kind != RTP_PAYLOAD_COMFORT_NOISE)
		return -EINVAL;
	if (pkt->len > UDSP_JB_PAYLOAD_MAX) {
		jb->total.overflow_drop++;
		return -EMSGSIZE;
	}
	jb_window_advance(jb, pkt->arrival_ns);
	/* An eligible SID-only update (jb_cn_sid_refresh below) is classified
	 * before the timestamp jump and the starve resync: a refresh
	 * reordered across a long armed silence must neither reset the
	 * stream nor rewind a starved clock on its way to the in-place
	 * store. A source change still outranks it, and the replay window
	 * below still rejects its duplicates. */
	sid_refresh = jb->ssrc == pkt->ssrc && jb->cn_armed && jb->cn_sid &&
		      pkt->kind == RTP_PAYLOAD_AUDIO &&
		      jb_sid_units(jb, (uint32_t)pkt->len) == 1U;
	if (jb->state == JB_STATE_IDLE) {
		jb->ssrc = pkt->ssrc;
		jb_anchor(jb, pkt);
	} else if (jb->ssrc != pkt->ssrc ||
		   (!sid_refresh && jb_ts_jump(jb, pkt) &&
		    !jb_dtx_resume_exempt(jb, pkt))) {
		jb->total.resyncs++;
		jb_stream_reset(jb, pkt);
	} else if (!sid_refresh && jb_late_starved(jb, pkt)) {
		jb->total.starve_resyncs++;
		jb_stream_reset(jb, pkt);
	}
	successor = jb->have_seq && seq_delta(pkt->seq, jb->seq_high) == 1;
	sid_tail = jb_sid_tailed(jb, pkt);
	/* The timestamp delta into a marked packet measures the silence the
	 * sender suppressed (jb.h Talkspurt), not its packet duration, so the
	 * pair teaches the span learner nothing. Without this a 10 ms stream whose
	 * far end suppresses one frame of silence offers a clean 20 ms
	 * observation, and JB_SPAN_AGREE of them would halve every slice. A
	 * SID-ended packet is the same trap without the marker: DTX
	 * consumes no sequence numbers, so the resume pairs seq-consecutively
	 * with the SID-ended packet and the delta measures the silence, a
	 * clean on-grid observation. Every pair the SID-ended packet is a
	 * member of teaches nothing, whichever of its two sides arrives
	 * first. A gap pair that steps OVER a SID-ended packet (its neighbour
	 * lost) is outside this guard and can still measure a normalised
	 * silence, a narrow class that the loss-divisibility rule limits. */
	span_delta = pkt->marker || sid_tail || jb->seq_high_sid ?
		     0 : jb_span_pair_delta(jb, pkt);
	if (jb_seq_duplicate(jb, pkt)) {
		jb->total.dup_drop++;
		return -EEXIST;
	}
	/* One classification per packet: the estimator and the parked branch
	 * below act on the same verdict, taken before the span relearn can
	 * change the bound's span term inside this put. jb_step_track keeps
	 * its own frontier-gated far test, which can differ by one span; only
	 * the refusal verdict is shared. UDSP_JB_FIXED keeps the immediate
	 * refill, because the fax path never discards a carrier frame;
	 * UDSP_JB_OFF is already outside jb_parked. A parked clock distrusts
	 * far comfort noise by the playout-only bound as well: a held junk
	 * packet may already have moved the frontier half of jb_ts_far, and a
	 * junk CN it admits would re-seed the reference the genuine resume is
	 * then measured against. */
	parked = jb_parked(jb);
	hold_want = parked && jb->mode == UDSP_JB_ADAPTIVE &&
		    pkt->kind == RTP_PAYLOAD_AUDIO && jb_refill_far(jb, pkt);
	cn_refused = pkt->kind == RTP_PAYLOAD_COMFORT_NOISE &&
		     (jb_ts_far(jb, pkt) ||
		      (parked && jb_refill_far(jb, pkt)));
	jb_jitter_update(jb, pkt, hold_want || cn_refused);
	jb_span_learn(jb, span_delta);
	/* A SID-only update is comfort noise in audio clothing: like a PT 13
	 * packet it neither arms, confirms nor voids a step candidate and
	 * never moves the frontier, so a spurious candidate cannot use the
	 * refresh and confirm on the next talkspurt's first packet. */
	step = sid_refresh ? false : jb_step_track(jb, pkt);
	/* A tail packet leaves the arrival reference on the datagram head
	 * (jb.h tail); moving it here would hand the next head a reference
	 * whose timestamp advanced without its arrival, one false span of
	 * variation per datagram. jb_ts_jump keeps working off the head. An
	 * invalid reference still seeds, or a stream whose first accepted
	 * packet is a tail (the head consumed as a telephone event) would
	 * leave last_rx_ts at zero and the next packet would read a resync
	 * jump against it. */
	if (!pkt->tail || !jb->have_arrival) {
		jb->last_arrival_ns = pkt->arrival_ns;
		jb->last_rx_ts = pkt->rtp_timestamp;
	}
	if (pkt->kind == RTP_PAYLOAD_COMFORT_NOISE) {
		/* A trusted comfort-noise packet proves the far end still
		 * sends a coverable timeline, so it ends the refusal run like
		 * a stored packet; a refused junk CN does not, or alternating
		 * junk audio and junk CN could suppress the hold bound. The
		 * candidate itself survives comfort noise either way. */
		if (!cn_refused)
			jb->hold_run = 0;
		return jb_cn_store(jb, pkt);
	}
	/* The SID-only update (jb_cn_sid_refresh), consumed before
	 * jb_cn_resume so a late or reordered update can neither rewind
	 * playout through the re-anchor nor be dropped as late. It
	 * follows the trusted-CN hold_run rule above. */
	if (sid_refresh) {
		if (!hold_want && !jb_ts_far(jb, pkt))
			jb->hold_run = 0;
		return jb_cn_sid_refresh(jb, pkt);
	}
	if (parked) {
		if (step || !hold_want || jb->hold_run >= JB_HOLD_MAX)
			jb_refill(jb, pkt);
		else
			return jb_drop_held(jb, pkt);
	} else if (step) {
		jb->total.resyncs++;
		jb_stream_reset(jb, pkt);
		/* The timestamp jump resync runs before the receive bookkeeping
		 * and the assignments above repair what jb_timing_reset
		 * cleared; this reset runs after them, so the packet re-seeds
		 * them here or the next packet reads a 10 s jump against
		 * last_rx_ts 0, and a replay of this packet would pass the
		 * empty replay window. */
		jb->last_rx_ts = pkt->rtp_timestamp;
		jb->last_arrival_ns = pkt->arrival_ns;
		jb->have_seq = true;
		jb->seq_high = pkt->seq;
		jb->seq_high_ts = pkt->rtp_timestamp;
		jb->seq_high_sid = sid_tail;
		jb->seq_mask = 1;
	} else if (jb_cn_resume(jb, pkt, successor)) {
		jb_cn_reanchor(jb, pkt);
	}
	lateness = ts_delta(jb->playout_ts, pkt->rtp_timestamp);
	if (jb->state == JB_STATE_PLAY && lateness > 0)
		return jb_drop_late(jb, lateness);
	if (jb->state == JB_STATE_PREFILL && lateness > 0) {
		/* An unbounded pull re-anchors the whole stream on one stray
		 * old packet and then conceals the gap back to the live
		 * timeline. A genuine backward step is seq-consecutive and
		 * keeps the unbounded pull. */
		if (!successor && lateness > jb_window_ts(jb))
			return jb_drop_late(jb, lateness);
		/* No resync_discard_ms: the pull-back moves the clock
		 * backward and discards nothing. */
		jb->playout_ts = pkt->rtp_timestamp;
	}
	return jb_slot_store(jb, pkt);
}

static void jb_now_set(struct udsp_jb *jb, uint64_t now_ns)
{
	if (now_ns > jb->now_ns)
		jb->now_ns = now_ns;
	jb_window_advance(jb, jb->now_ns);
}

static uint64_t jb_since(const struct udsp_jb *jb, uint64_t mark_ns)
{
	if (jb->now_ns <= mark_ns)
		return 0;
	return jb->now_ns - mark_ns;
}

/*
 * The configured jitter_target is the floor until the measurement window holds
 * a full 2 s of history; after it, the floor is one learned packet span plus
 * one frame.
 *
 * Why the extra frame. jb_prefill_done compares the setpoint against
 * jb_depth_ms, which is read at the top of the sawtooth, right after an
 * arrival. A stream of ptime P therefore leaves playout with a standing floor
 * of target - P, so a floor of exactly one span leaves no standing reserve at
 * all: the queue bottoms at empty every cycle and any arrival later than the
 * tick that needs it underruns. One frame above the span moves the prefill to
 * the next whole packet, so the floor of the sawtooth is one full span of
 * reserve. The accepted cost is the latency that reserve holds.
 */
static uint16_t jb_target_floor(const struct udsp_jb *jb)
{
	if (jb_since(jb, jb->anchor_ns) < ms_to_ns(UDSP_JB_WINDOW_MS))
		return jb->init_ms;
	return ms_clamp(jb->pkt_span_ts / jb->ts_per_ms + UDSP_JB_FRAME_MS,
			UDSP_JB_FRAME_MS, jb->max_ms);
}

static uint16_t jb_target_want(struct udsp_jb *jb)
{
	uint32_t high_water = jb_window_high_water(jb);
	uint32_t want;

	if (high_water > ms_to_ts(jb, UDSP_JB_DEPTH_MAX_MS))
		high_water = ms_to_ts(jb, UDSP_JB_DEPTH_MAX_MS);
	want = high_water * JB_TARGET_K / jb->ts_per_ms;
	if (want < jb->late_want_ms)
		want = jb->late_want_ms;
	jb->late_want_ms = 0;
	return ms_clamp(want, jb_target_floor(jb), jb->max_ms);
}

static void jb_target_update(struct udsp_jb *jb)
{
	uint16_t want;

	if (jb->mode == UDSP_JB_OFF) {
		jb->target_ms = 0;
		return;
	}
	if (jb->mode == UDSP_JB_FIXED) {
		jb->target_ms = jb->init_ms;
		return;
	}
	want = jb_target_want(jb);
	if (want > jb->target_ms) {
		jb->target_ms = want;
		jb->shrink_ns = jb->now_ns;
		return;
	}
	if (want == jb->target_ms ||
	    jb_since(jb, jb->shrink_ns) < ms_to_ns(JB_SHRINK_MS))
		return;
	jb->shrink_ns = jb->now_ns;
	jb->target_ms -= UDSP_JB_FRAME_MS;
	if (jb->target_ms < want)
		jb->target_ms = want;
}

/* The control input is the averaged depth: with a ptime above one frame the
 * instantaneous depth saws by a whole packet every arrival, which no sustained
 * condition could ever observe. */
static void jb_depth_track(struct udsp_jb *jb)
{
	uint32_t depth = jb_depth_reach_ms(jb) << 8;

	if (depth > jb->depth_avg_q8)
		jb->depth_avg_q8 += (depth - jb->depth_avg_q8) >> JB_DEPTH_SHIFT;
	else
		jb->depth_avg_q8 -= (jb->depth_avg_q8 - depth) >> JB_DEPTH_SHIFT;
	jb->depth_hist_q8[jb->window_head] = jb->depth_avg_q8;
}

static int32_t jb_depth_window_ms(const struct udsp_jb *jb)
{
	unsigned oldest = (jb->window_head + 1U) % UDSP_JB_TREND_BUCKETS;

	return (int32_t)(jb->depth_hist_q8[oldest] >> 8);
}

/* Growth or decay of the averaged depth across the 4 s history. Over that
 * window, half a percent of skew (one frame every two seconds) moves the depth
 * by two frames, twice the drift_min of a line without jitter. A target that
 * merely contracted leaves the depth flat and measures nothing. */
static int32_t jb_depth_trend_ms(const struct udsp_jb *jb)
{
	return (int32_t)(jb->depth_avg_q8 >> 8) - jb_depth_window_ms(jb);
}

/* The drop side is fenced by !jb_frame_ready; the insert side needs the same
 * fence for a stray far-ahead head: under the cover rule its reach reads
 * empty, the average sinks, and an insert would slip the clock for a queue
 * that is not starved. */
static bool jb_head_beyond(const struct udsp_jb *jb)
{
	if (!jb->count)
		return false;
	return ts_delta(jb_slot_const(jb, 0)->ts, jb->playout_ts) >
	       jb_window_ts(jb);
}

static enum jb_skew jb_skew_gate(struct udsp_jb *jb, enum jb_skew want)
{
	if (want == JB_SKEW_DROP && !jb_frame_ready(jb))
		return JB_SKEW_NONE;
	if (want == JB_SKEW_INSERT && jb_head_beyond(jb))
		return JB_SKEW_NONE;
	/* The averaged depth lags a drain by its own window, so a free-window
	 * correction run with no fence drains past the setpoint until the queue
	 * underruns again. A drop fires only while the queue reachably holds
	 * more than the target. */
	if (want == JB_SKEW_DROP && jb_depth_reach_ms(jb) <= jb->target_ms)
		return JB_SKEW_NONE;
	if (jb_since(jb, jb->fill_ns) >= ms_to_ns(JB_SKEW_FREE_MS) &&
	    jb_since(jb, jb->last_skew_ns) < ms_to_ns(JB_SKEW_INTERVAL_MS))
		return JB_SKEW_NONE;
	jb->last_skew_ns = jb->now_ns;
	return want;
}

/* Once a drift is proven the correction runs at one frame per second until the
 * depth is back at the setpoint, so the offset built while the drift was being
 * proven is cleared instead of becoming permanent latency. */
static enum jb_skew jb_skew_run(struct udsp_jb *jb, int32_t excess)
{
	enum jb_skew want = (enum jb_skew)jb->skew_pending;
	int32_t done = 0;

	/* A trim-armed run stops half a span above the target: the averaged
	 * excess lags the queue by up to a frame, and running it to zero was
	 * measured to drag the sawtooth floor a full packet below the target.
	 * A trend run still stops at the target itself. */
	if (want == JB_SKEW_DROP && jb->trim_pending)
		done = (int32_t)(jb->pkt_span_ts / jb->ts_per_ms / 2U);
	if ((want == JB_SKEW_DROP && excess <= done) ||
	    (want == JB_SKEW_INSERT && excess >= 0)) {
		jb->skew_pending = JB_SKEW_NONE;
		jb->trim_pending = false;
		return JB_SKEW_NONE;
	}
	return jb_skew_gate(jb, want);
}

static enum jb_skew jb_skew_arm(struct udsp_jb *jb, enum jb_skew want,
				int32_t excess)
{
	jb->skew_pending = (uint8_t)want;
	return jb_skew_run(jb, excess);
}

/* The offset trim arms on a surplus that stood clean for the whole hold.
 * trim_calm_ns is the start of the clean-and-in-excess window;
 * jb_conceal_emit, jb_late_grow, jb_refill and jb_anchor reset it, and so
 * does the excess falling to the threshold here, so the age is continuous by
 * construction. A refused far-ahead hold (jb_drop_held) does not reset it and
 * does not need to: the park it falls in conceals every tick, so
 * jb_conceal_emit keeps the window dirty for as long as the clock is
 * parked. */
static bool jb_trim_due(struct udsp_jb *jb, int32_t excess)
{
	if (jb->mode != UDSP_JB_ADAPTIVE || jb->target_ms >= jb->max_ms)
		return false;
	if (excess <= (int32_t)JB_TRIM_EXCESS_MS) {
		jb->trim_calm_ns = jb->now_ns;
		return false;
	}
	return jb_since(jb, jb->trim_calm_ns) >= ms_to_ns(JB_TRIM_HOLD_MS);
}

static enum jb_skew jb_skew_decide(struct udsp_jb *jb)
{
	int32_t excess;
	int32_t trend;
	int32_t start;
	int32_t span_ms;
	int32_t drift_min;

	/* UDSP_JB_OFF feeds no correction, so it skips the reach walk. */
	if (jb->mode == UDSP_JB_OFF)
		return JB_SKEW_NONE;
	jb_depth_track(jb);
	if (!jb->count)
		return JB_SKEW_NONE;
	excess = (int32_t)(jb->depth_avg_q8 >> 8) - (int32_t)jb->target_ms;
	if (jb->skew_pending != JB_SKEW_NONE)
		return jb_skew_run(jb, excess);
	trend = jb_depth_trend_ms(jb);
	/* A jittery line moves the averaged depth on its own, so the drift a
	 * correction acts on must stand out above that movement; with no jitter
	 * the threshold is one frame. */
	drift_min = (int32_t)UDSP_JB_FRAME_MS +
		    (int32_t)((jb->jitter_q4 >> 4) / jb->ts_per_ms);
	/* Arm only on a drift away from the setpoint that the whole history
	 * shows: the depth was already on the far side of the target when the
	 * history starts and has moved a further frame since. A target that
	 * merely contracted leaves the depth flat and arms nothing. */
	start = jb_depth_window_ms(jb) - (int32_t)jb->target_ms;
	span_ms = (int32_t)(jb->pkt_span_ts / jb->ts_per_ms);
	/*
	 * The two halves of the clock-skew correction's drop arm. The control
	 * input saws by a whole packet span every arrival and a one-time
	 * transit step moves it by another, so an excess of one or two spans
	 * says nothing about drift: it is the measurement's own amplitude.
	 * The prefill seed and one transit step together produce excess 10,
	 * trend 10, drift_min 10 and start 0. A single arm fires at that
	 * boundary and sheds the reserve that the next gap needs: 37 times in
	 * 233 s of a field trace.
	 *
	 * An excess above two spans is larger than either can make, so it is
	 * drift and it arms at once: that is the paced-stall backlog, where
	 * waiting would sit on hundreds of milliseconds of latency.
	 *
	 * Two spans is the threshold that arms, not a bound on the excess.
	 * What bounds the excess afterwards is the correction rate, one frame
	 * per JB_SKEW_INTERVAL_MS through jb_skew_gate, so an excess far above
	 * the threshold is expected while a fast drift is worked off. The arm
	 * keeps the queue from running away: without it, a 1% lossy probe
	 * grew the queue to 3080 ms. Nothing here promises to hold
	 * the excess at two spans and nothing needs to.
	 *
	 * An excess below that is corrected only once it stood unchanged for
	 * a whole further trend window with nothing concealed, late-dropped
	 * or refilled in it, which is jb_trim_due's evidence rule applied to
	 * the trend arm: a drift keeps its excess through a quiet window, a
	 * transit step does not, because the park that follows it resets
	 * trim_calm_ns. One window beyond the window the trend itself is
	 * measured over is the least that can distinguish them.
	 */
	if (excess > 2 * span_ms && trend >= drift_min && start >= 0)
		return jb_skew_arm(jb, JB_SKEW_DROP, excess);
	if (excess >= (int32_t)UDSP_JB_FRAME_MS && trend >= drift_min &&
	    start >= 0 &&
	    jb_since(jb, jb->trim_calm_ns) >= ms_to_ns(JB_TREND_HOLD_MS))
		return jb_skew_arm(jb, JB_SKEW_DROP, excess);
	if (jb_trim_due(jb, excess)) {
		jb->trim_pending = true;
		return jb_skew_arm(jb, JB_SKEW_DROP, excess);
	}
	if (excess <= -(int32_t)UDSP_JB_FRAME_MS && trend <= -drift_min &&
	    start <= 0 && !jb_head_beyond(jb))
		return jb_skew_arm(jb, JB_SKEW_INSERT, excess);
	return JB_SKEW_NONE;
}

/* An output counter. A skipped opportunity played nothing, so it counts in
 * none of them and in no underrun (jb.h Skipped opportunities). */
static void jb_out_count(const struct udsp_jb *jb, uint64_t *counter)
{
	if (!jb->skipping)
		(*counter)++;
}

/* A SID that a skipped opportunity hands the decoder, kept for a caller that
 * restarts its decoder after a run (udsp_jb_skip_sid). */
static void jb_skip_sid_note(struct udsp_jb *jb, const uint8_t *sid,
			     uint32_t len)
{
	if (!jb->skipping || !len || len > UDSP_JB_CN_MAX)
		return;
	memcpy(jb->skip_sid, sid, len);
	jb->skip_sid_len = (uint8_t)len;
}

/* Emit from a staging copy that only get writes: a comfort-noise packet
 * arriving before the caller has run its codec must not move the bytes under
 * the pointer it holds. */
static void jb_cn_emit(struct udsp_jb *jb, struct udsp_jb_out *out)
{
	if (jb->cn_len)
		memcpy(jb->cn_out, jb->cn_payload, jb->cn_len);
	if (jb->cn_sid)
		jb_skip_sid_note(jb, jb->cn_payload, jb->cn_len);
	out->decision = UDSP_JB_CN;
	out->payload = jb->cn_out;
	out->len = jb->cn_len;
	out->cn_sid = jb->cn_sid;
	/* A SID reprograms the decoder, so a stored update goes out exactly
	 * once and the ticks after it carry the len-0 continuation; an RFC
	 * 3389 payload describes a level and repeats. */
	if (jb->cn_sid)
		jb->cn_len = 0;
	jb_out_count(jb, &jb->total.cn);
}

static void jb_idle_emit(struct udsp_jb *jb, struct udsp_jb_out *out)
{
	out->rtp_timestamp = jb->playout_ts;
	out->decision = UDSP_JB_SILENCE;
	jb_out_count(jb, &jb->total.silence);
}

static void jb_conceal_emit(struct udsp_jb *jb, struct udsp_jb_out *out)
{
	jb->shrink_ns = jb->now_ns;
	jb->trim_calm_ns = jb->now_ns;
	if (jb_cn_active(jb)) {
		jb_cn_emit(jb, out);
		/* Only the SID free-run is measured (jb_cn_active). */
		if (jb->cn_sid)
			jb->cn_run_ms += UDSP_JB_FRAME_MS;
		return;
	}
	if (!jb->conceal_run_ms)
		jb_out_count(jb, &jb->total.underruns);
	if (jb->conceal_run_ms < JB_PLC_MAX_MS) {
		out->decision = UDSP_JB_PLC;
		jb_out_count(jb, &jb->total.plc);
	} else if (jb->cn_valid) {
		jb_cn_emit(jb, out);
	} else {
		out->decision = UDSP_JB_SILENCE;
		jb_out_count(jb, &jb->total.silence);
	}
	if (jb->conceal_run_ms < JB_CONCEAL_RUN_MAX_MS)
		jb->conceal_run_ms += UDSP_JB_FRAME_MS;
}

/* A whole-packet unit leaves the buffer once and is marked after that (jb.h
 * Units). The payload goes out on the first slice the clock reaches, which is
 * slice 0 unless a skew drop stepped over it, so one decode per unit_ts holds
 * on every path the clock can take through the unit. */
static void jb_unit_emit(struct udsp_jb *jb, struct udsp_jb_out *out,
			 const struct udsp_jb_slot *slot)
{
	bool fresh = !jb->emit_unit_valid || jb->emit_unit_ts != slot->ts;

	out->payload = fresh ? slot->payload : NULL;
	out->len = fresh ? slot->len : 0;
	jb->emit_unit_ts = slot->ts;
	jb->emit_unit_valid = true;
}

static void jb_slice_emit(struct udsp_jb_out *out,
			  const struct udsp_jb_slot *slot, uint32_t index,
			  uint32_t frames)
{
	uint32_t bytes = slot->len / frames;

	out->payload = slot->payload + index * bytes;
	out->len = index + 1U == frames ? slot->len - index * bytes : bytes;
}

/* The SID-shaped slice rule (jb.h SID): the speech frames first, each of
 * frame_bytes, then the SID slice of sid_len. With the span derived at store
 * time the slice count equals the payload's units, so every speech slice is
 * whole and the SID tail is the one uneven slice. */
static void jb_sid_slice_emit(struct udsp_jb *jb, struct udsp_jb_out *out,
			      const struct udsp_jb_slot *slot, uint32_t index,
			      uint32_t frames)
{
	if (index + 1U == frames) {
		out->payload = slot->payload + slot->len - jb->sid_len;
		out->len = jb->sid_len;
		jb_skip_sid_note(jb, out->payload, (uint32_t)out->len);
		return;
	}
	out->payload = slot->payload + index * jb->frame_bytes;
	out->len = jb->frame_bytes;
}

static void jb_frame_emit(struct udsp_jb *jb, struct udsp_jb_out *out)
{
	const struct udsp_jb_slot *slot = jb_slot_const(jb, 0);
	uint32_t sid_units = jb_sid_units(jb, slot->len);
	uint32_t frames = slot->span_ts / jb->frame_ts;
	uint32_t index;

	index = (uint32_t)ts_delta(jb->playout_ts, slot->ts) / jb->frame_ts;
	if (index >= frames)
		index = frames - 1U;
	out->decision = UDSP_JB_FRAME;
	out->unit_ts = slot->ts;
	out->slice = (uint8_t)index;
	out->slices = (uint8_t)frames;
	if (jb->whole_packet)
		jb_unit_emit(jb, out, slot);
	else if (sid_units)
		jb_sid_slice_emit(jb, out, slot, index, frames);
	else
		jb_slice_emit(out, slot, index, frames);
	/* A skew drop earlier in this same get can pop the slot still on loan,
	 * and jb_order_pop keeps a loaned slot out of the pool. Overwriting the
	 * loan here without this release loses that slot for the life of the
	 * stream, one slot per drop, until the pool is empty and every put
	 * overflows. */
	jb_loan_release(jb);
	jb->loan = jb->order[0];
	jb_out_count(jb, &jb->total.frames_out);
	jb->conceal_run_ms = 0;
	if (sid_units && index + 1U == frames) {
		/* The SID slice closes the talkspurt, so the gap after this
		 * packet is announced silence: the ticks in it play SID-source
		 * comfort noise instead of the loss ladder. The slice itself
		 * already reached the decoder as this frame, so the slot arms
		 * with nothing stored and the gap starts on the len-0
		 * continuation; only jb_cn_sid_refresh stores a payload. */
		jb->cn_len = 0;
		jb->cn_valid = true;
		jb->cn_armed = true;
		jb->cn_sid = true;
		jb->cn_ts = slot->ts + slot->span_ts;
		jb->cn_run_ms = 0;
		return;
	}
	if (ts_delta(jb->playout_ts, jb->cn_ts) >= 0) {
		jb->cn_armed = false;
		if (jb->cn_sid) {
			/* The talkspurt resumed and the SID was consumed, so
			 * the slot holds nothing to replay; an RFC 3389
			 * description stays valid for the loss ladder. */
			jb->cn_sid = false;
			jb->cn_valid = false;
			jb->cn_len = 0;
		}
	}
}

static void jb_advance(struct udsp_jb *jb)
{
	jb->playout_ts += jb->frame_ts;
	jb_stale_purge(jb);
}

/*
 * The talkspurt skip (jb.h Talkspurt). The queue head carries the RFC 3551
 * 4.1 marker and sits ahead of the clock, so the frames between the clock and
 * that head are a silence the sender chose not to transmit. The clock moves
 * straight to the head and plays it, which costs no concealment frame and no
 * underrun.
 *
 * The bound is jb_window_ts, the static reorder window every other forward
 * decision already reads: a marker on a mis-stamped packet must not move the
 * clock, and past that window the timestamp jump resync, the step confirm and
 * the parked hold own the packet and decide it on their own two-packet
 * proofs. Only the head is consulted, so the clock can never step over audio
 * the buffer still holds, and only a forward step qualifies, so a marker on a
 * packet that continues the timeline does nothing.
 *
 * UDSP_JB_ADAPTIVE only. UDSP_JB_FIXED is the fax and modem playout that
 * stream.c pins, where the timeline must stay continuous and a hole is
 * concealed at its full length instead of removed: that path never shortens
 * the carrier. UDSP_JB_OFF keeps no recovery machinery at all, and
 * jb_off_anchor already advances it to the head.
 */
static bool jb_talkspurt_due(const struct udsp_jb *jb)
{
	const struct udsp_jb_slot *head;
	int32_t ahead;

	if (jb->mode != UDSP_JB_ADAPTIVE || !jb->count)
		return false;
	head = jb_slot_const(jb, 0);
	if (!head->marker)
		return false;
	ahead = ts_delta(head->ts, jb->playout_ts);
	return ahead > 0 && ahead <= jb_window_ts(jb);
}

static bool jb_talkspurt_skip(struct udsp_jb *jb)
{
	if (!jb_talkspurt_due(jb))
		return false;
	jb->playout_ts = jb_slot_const(jb, 0)->ts;
	return true;
}

/*
 * A gap with packets still queued behind it is a lost frame: the timeline must
 * move on or the queued audio never plays. A gap with nothing queued cannot be
 * told from a far end that is behind us, so the media clock parks instead of
 * running away from the sender; jb_put re-anchors it on the next arrival. The
 * clock_parked marker tells the caller the timeline stood still; it is ordinary
 * concealment and is counted as such, unlike a skew insert.
 *
 * One gap is neither of those: the queued packet on its far side carries the
 * RFC 3551 marker, which says the sender suppressed a silence rather than
 * lost audio. jb_talkspurt_skip steps the clock over it, so that gap costs no
 * concealment and no underrun. A gap the queue does not yet cover stays a
 * park and stays counted, because the buffer really did run dry: the sender's
 * intent is only knowable from a packet that has arrived.
 */
static void jb_play(struct udsp_jb *jb, struct udsp_jb_out *out)
{
	bool park;

	/* Before anything else reads the clock: a suppressed silence is not a
	 * gap to conceal, so the position this get plays from may be the
	 * marked head rather than where the last frame left off. */
	if (jb_talkspurt_skip(jb))
		out->talkspurt = true;
	park = jb_hold_needed(jb);
	out->rtp_timestamp = jb->playout_ts;
	if (jb_frame_ready(jb)) {
		jb_frame_emit(jb, out);
		jb_advance(jb);
		return;
	}
	jb_conceal_emit(jb, out);
	if (!park) {
		jb_advance(jb);
		return;
	}
	out->clock_parked = true;
}

static void jb_insert_emit(struct udsp_jb *jb, struct udsp_jb_out *out)
{
	out->rtp_timestamp = jb->playout_ts;
	out->decision = UDSP_JB_PLC;
	out->skew_insert = true;
	jb->total.skew_inserts++;
}

static void jb_frame_drop(struct udsp_jb *jb, struct udsp_jb_out *out)
{
	out->skew_drop = true;
	jb->total.skew_drops++;
	jb_advance(jb);
}

/* Filling before the first frame of a stream is silence; filling after an
 * outage continues the concealment the parked clock started. */
static void jb_fill_emit(struct udsp_jb *jb, struct udsp_jb_out *out)
{
	out->rtp_timestamp = jb->playout_ts;
	if (!jb->conceal_run_ms) {
		jb_idle_emit(jb, out);
		return;
	}
	jb_conceal_emit(jb, out);
	out->clock_parked = true;
}

/* Playout starts once the queue holds the target, or once the anchor is
 * jb_max_ms old, so a stream that never fills the target still plays. */
static bool jb_prefill_done(struct udsp_jb *jb)
{
	if (jb_depth_ms(jb) < jb->target_ms &&
	    jb_since(jb, jb->fill_ns) < ms_to_ns(jb->max_ms))
		return false;
	jb->state = JB_STATE_PLAY;
	/* Seed the averaged depth with the setpoint, not with the instantaneous
	 * queue: prefill ends on a packet arrival, so the queue is at the top of
	 * its sawtooth and would look like a full packet of clock skew. */
	jb_depth_seed(jb, jb->target_ms);
	return true;
}

/* UDSP_JB_OFF plays the first packet at the first get after it arrives: there
 * is no reorder wait, so the timeline follows the queue head. */
static void jb_off_anchor(struct udsp_jb *jb)
{
	if (jb->mode != UDSP_JB_OFF || !jb->count)
		return;
	if (ts_delta(jb_slot_const(jb, 0)->ts, jb->playout_ts) > 0)
		jb->playout_ts = jb_slot_const(jb, 0)->ts;
}

int udsp_jb_get(struct udsp_jb *jb, uint64_t now_ns, struct udsp_jb_out *out)
{
	enum jb_skew skew;

	if (!jb || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	jb_loan_release(jb);
	jb_now_set(jb, now_ns);
	if (jb->state == JB_STATE_IDLE) {
		jb_idle_emit(jb, out);
		return 0;
	}
	jb_target_update(jb);
	if (jb->state == JB_STATE_PREFILL && !jb_prefill_done(jb)) {
		jb_fill_emit(jb, out);
		return 0;
	}
	jb_stale_purge(jb);
	jb_off_anchor(jb);
	skew = jb_skew_decide(jb);
	if (skew == JB_SKEW_INSERT) {
		jb_insert_emit(jb, out);
		return 0;
	}
	if (skew == JB_SKEW_DROP)
		jb_frame_drop(jb, out);
	jb_play(jb, out);
	return 0;
}

void udsp_jb_stats(const struct udsp_jb *jb, struct udsp_jb_stats *out)
{
	if (!jb || !out)
		return;
	*out = jb->total;
	out->target_ms = jb->target_ms;
	out->depth_ms = (uint16_t)jb_depth_ms(jb);
	out->jitter_ms = (uint16_t)((jb->jitter_q4 >> 4) / jb->ts_per_ms);
	/* The loaned slot is not free; a gauge that decays across a call is a
	 * slot leak. */
	out->pool_free = jb->pool_count;
	out->mode = jb->mode;
}

/* --- skipped playout opportunities (jb.h) -------------------------------- */

uint32_t udsp_jb_timeline(const struct udsp_jb *jb)
{
	return jb ? jb->timeline : 0;
}

/* A buffer in prefill plays once its queue holds the target, so it passes
 * only the units above it; below it a get waits. */
static bool jb_skip_waits(const struct udsp_jb *jb)
{
	if (jb->state == JB_STATE_IDLE)
		return true;
	return jb->state == JB_STATE_PREFILL &&
	       jb_depth_ms(jb) < (uint32_t)jb->target_ms + UDSP_JB_FRAME_MS;
}

/* One skipped opportunity, with jb->skipping set by the caller. */
static void jb_skip_one(struct udsp_jb *jb, uint64_t now_ns,
			struct udsp_jb_out *out)
{
	uint32_t from;

	memset(out, 0, sizeof(*out));
	jb_loan_release(jb);
	jb_now_set(jb, now_ns);
	from = jb->playout_ts;
	if (jb->state == JB_STATE_IDLE) {
		jb_idle_emit(jb, out);
	} else if (jb_skip_waits(jb)) {
		jb_fill_emit(jb, out);
	} else {
		jb_stale_purge(jb);
		jb_off_anchor(jb);
		jb_play(jb, out);
	}
	if (jb->playout_ts == from) {
		jb->skip_waits++;
		return;
	}
	jb->skip_units++;
	if (out->decision == UDSP_JB_FRAME)
		jb->skip_frames++;
}

int udsp_jb_skip_get(struct udsp_jb *jb, uint64_t now_ns,
		     struct udsp_jb_out *out)
{
	if (!jb || !out)
		return -EINVAL;
	jb->skipping = true;
	jb_skip_one(jb, now_ns, out);
	jb->skipping = false;

	return 0;
}

/* The clock passes units frames under concealment, or holds. */
static void jb_skip_move(struct udsp_jb *jb, uint32_t units, bool moves)
{
	if (!moves) {
		jb->skip_waits += units;
		return;
	}
	jb->playout_ts += units * jb->frame_ts;
	jb->skip_units += units;
}

/*
 * units opportunities of the loss ladder: what that many calls of
 * jb_conceal_emit leave behind once no comfort noise stands, without the
 * counters. The ladder reaches comfort noise once JB_PLC_MAX_MS passes, which
 * sends a stored SID update out once (jb_cn_emit).
 */
static void jb_skip_ladder(struct udsp_jb *jb, uint32_t units, bool moves)
{
	uint64_t run = (uint64_t)jb->conceal_run_ms +
		       (uint64_t)units * UDSP_JB_FRAME_MS;

	if (jb->cn_valid && jb->cn_sid &&
	    run - UDSP_JB_FRAME_MS >= JB_PLC_MAX_MS) {
		jb_skip_sid_note(jb, jb->cn_payload, jb->cn_len);
		jb->cn_len = 0;
	}
	jb->conceal_run_ms = (uint16_t)(run < JB_CONCEAL_RUN_MAX_MS ?
					run : JB_CONCEAL_RUN_MAX_MS);
	jb_skip_move(jb, units, moves);
}

/*
 * units concealed opportunities paid at once: comfort noise while it stands,
 * then the loss ladder. SID comfort noise stands until JB_CN_RUN_MAX_MS
 * (jb_cn_active). cn_moves says the clock runs under the comfort noise, as in
 * playout; ladder_moves says it runs under the ladder, as it does before a
 * queued head. A prefill holds it under both, and a dry queue under the
 * ladder.
 */
static void jb_skip_conceal(struct udsp_jb *jb, uint32_t units, bool cn_moves,
			    bool ladder_moves)
{
	uint32_t cn;

	jb->shrink_ns = jb->now_ns;
	jb->trim_calm_ns = jb->now_ns;
	if (jb_cn_active(jb)) {
		cn = units;
		if (jb->cn_sid) {
			uint32_t left = (JB_CN_RUN_MAX_MS - jb->cn_run_ms) /
					UDSP_JB_FRAME_MS;

			if (cn > left)
				cn = left;
			jb_skip_sid_note(jb, jb->cn_payload, jb->cn_len);
			jb->cn_len = 0;
			jb->cn_run_ms = (uint16_t)(jb->cn_run_ms +
						   cn * UDSP_JB_FRAME_MS);
		}
		jb_skip_move(jb, cn, cn_moves);
		units -= cn;
	}
	if (units)
		jb_skip_ladder(jb, units, ladder_moves);
}

/*
 * The opportunities a gap before an ADAPTIVE marked head pays before the head
 * enters the talkspurt window. The opportunity after them steps over the rest
 * of the gap (jb_talkspurt_due), as a get would. UINT32_MAX for a head that
 * never steps: one without the marker, or another mode.
 */
static uint32_t jb_skip_talkspurt_cap(const struct udsp_jb *jb, int32_t ahead)
{
	int32_t over;

	if (jb->mode != UDSP_JB_ADAPTIVE || !jb_slot_const(jb, 0)->marker)
		return UINT32_MAX;
	over = ahead - jb_window_ts(jb);
	if (over <= 0)
		return 0;

	return ((uint32_t)over + jb->frame_ts - 1u) / jb->frame_ts;
}

/*
 * The opportunities a prefill with surplus pays before it waits: a get passes
 * while the queue holds the target and a frame more (jb_skip_waits), and each
 * one takes a frame of the depth. UINT32_MAX outside prefill.
 */
static uint32_t jb_skip_reserve_cap(const struct udsp_jb *jb)
{
	uint32_t depth;

	if (jb->state != JB_STATE_PREFILL)
		return UINT32_MAX;
	depth = jb_depth_ms(jb);
	if (depth < jb->target_ms)
		return 0;

	return (depth - jb->target_ms) / UDSP_JB_FRAME_MS;
}

/*
 * A gap before a queued head, which jb_play conceals one frame at a time with
 * the clock moving, because audio waits behind it. The talkspurt rule steps
 * over a marked gap in one opportunity instead, and UDSP_JB_OFF anchors on
 * the head, so both stay with udsp_jb_skip_get. A playing buffer and a
 * prefill with surplus both take the gap. Returns the units paid: at most the
 * whole frames up to the head, and no more than the opportunities before a
 * marked head becomes due or a prefill starts to wait.
 */
static uint32_t jb_skip_gap(struct udsp_jb *jb, uint32_t units)
{
	uint32_t gap, cap;
	int32_t ahead;

	jb_stale_purge(jb);
	if ((jb->state != JB_STATE_PLAY && jb->state != JB_STATE_PREFILL) ||
	    jb->mode == UDSP_JB_OFF || !jb->count || jb_frame_ready(jb) ||
	    jb_talkspurt_due(jb))
		return 0;
	ahead = ts_delta(jb_slot_const(jb, 0)->ts, jb->playout_ts);
	gap = (uint32_t)ahead / jb->frame_ts;
	cap = jb_skip_talkspurt_cap(jb, ahead);
	if (cap > jb_skip_reserve_cap(jb))
		cap = jb_skip_reserve_cap(jb);
	if (gap > cap)
		gap = cap;
	if (gap > units)
		gap = units;
	if (!gap)
		return 0;
	jb_skip_conceal(jb, gap, true, true);
	return gap;
}

/*
 * A run that the queue cannot shorten, paid at once: the buffer is idle, a
 * prefill waits below its target, or nothing is queued. True when it paid all
 * of units. Each of these leaves the queue as it is, so every unit of the run
 * does the same thing.
 */
static bool jb_skip_still(struct udsp_jb *jb, uint32_t units)
{
	if (jb->state == JB_STATE_IDLE) {
		jb->skip_waits += units;
		return true;
	}
	if (jb_skip_waits(jb)) {
		if (jb->conceal_run_ms)
			jb_skip_conceal(jb, units, false, false);
		else
			jb->skip_waits += units;
		return true;
	}
	if (jb->count)
		return false;
	jb_skip_conceal(jb, units, true, false);
	return true;
}

/*
 * The run. Still runs and gaps go at once; what remains goes one unit at a
 * time, each one slice of queued audio, the first frame past an off-grid gap
 * or a talkspurt step. So the steps are bounded by the queued slices and the
 * gaps between them, never by units.
 */
uint32_t udsp_jb_skip(struct udsp_jb *jb, uint32_t units, uint64_t now_ns)
{
	struct udsp_jb_out out;
	uint32_t frames;
	uint32_t gap;

	if (!jb)
		return 0;
	jb_now_set(jb, now_ns);
	jb->skip_sid_len = 0;
	jb->skipping = true;
	frames = jb->skip_frames;
	while (units && !jb_skip_still(jb, units)) {
		gap = jb_skip_gap(jb, units);
		if (gap) {
			units -= gap;
			continue;
		}
		jb_skip_one(jb, now_ns, &out);
		jb->skip_steps++;
		units--;
	}
	jb->skipping = false;
	jb_loan_release(jb);
	jb->emit_unit_valid = false;

	return jb->skip_frames - frames;
}

size_t udsp_jb_skip_sid(const struct udsp_jb *jb, const uint8_t **sid)
{
	if (!jb || !sid || !jb->skip_sid_len)
		return 0;
	*sid = jb->skip_sid;

	return jb->skip_sid_len;
}

void udsp_jb_skip_end(struct udsp_jb *jb)
{
	if (!jb || jb->state == JB_STATE_IDLE)
		return;
	jb->skew_pending = JB_SKEW_NONE;
	jb->trim_pending = false;
	jb->trim_calm_ns = jb->now_ns;
	if (jb->state == JB_STATE_PLAY)
		jb_depth_seed(jb, jb_depth_reach_ms(jb));
}
