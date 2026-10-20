// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * One analog line as the engine tick sees it (line.h section 2). The tick
 * owns every field here except the two atomics: the hook edge ring the slic
 * thread fills, and the published hook pair udsp_line_hook_query reads
 * without touching SPI.
 *
 * Two playout slots per line. Slot A is the API playout (udsp_tone_play,
 * udsp_digit_play); slot B is the sequencer's (CAS, FSK, the DTMF
 * caller-identity string). B overrides A while it runs and A pauses with its
 * remaining duration frozen, then resumes when B ends.
 */
#ifndef UDSP_CORE_CHAN_H
#define UDSP_CORE_CHAN_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "udsp/udsp.h"

#include "../dsp/cid.h"
#include "../dsp/dsp.h"
#include "../dsp/fax_det.h"
#include "../dsp/lec.h"
#include "../dsp/resamp.h"
#include "../line/line.h"

#include "clock.h"
#include "spsc.h"

#define UDSP_HOOK_EDGE_DEPTH	8u
#define UDSP_HOOK_EDGE_PER_TICK	4u
/* GR-506 accepts a digit from 50 ms; 100 ms is the default for a
 * udsp_digit_play that asks for no duration. */
#define UDSP_DIGIT_DEFAULT_MS	100u
/* Fault handling: linefeed open, event, timed retry. */
#define UDSP_FAULT_RETRY_MS	5000u
/* A dead SPI bus has no fault bit in event.h and none is invented: ten
 * consecutive hook_read failures mark the line down instead. */
#define UDSP_HOOK_FAIL_LIMIT	10u
/* The echo canceller's reference is the DL frame delayed by the runtime loop
 * delay the engine measures (loop_ref: playback queue plus capture backlog,
 * about 350 frames on MT7987 at prefill 5), so the history holds the
 * largest reference the tdm geometry can ask for plus the frame being
 * cancelled. The reach is stated in time and is the same at every line rate;
 * the ring is counted in samples and is sized for the widest rate, so the
 * ceiling of the delay comes from the rate, never from the array size
 * (chan.c chan_lec_delay_clamp). */
#define UDSP_LEC_HIST_MS	256u
#define UDSP_LEC_HIST_SAMPLES	4096u

/* What the measurement of one tick offers the lines (chan.c
 * chan_lec_measure). */
enum chan_lec_offer {
	CHAN_LEC_OFFER_NONE = 0,	/* open window inside the allowance */
	CHAN_LEC_OFFER_WINDOW,		/* loop_ref of a completed window */
	CHAN_LEC_OFFER_FALLBACK,	/* past the allowance */
};

/* The reference that the taps of one line stand on. Tick thread only. */
struct chan_lec_line {
	uint32_t loop;		/* the measured loop the taps are aligned to */
	uint32_t delay;		/* clamp(guard(loop)), what the line consumes */
	uint32_t gen;		/* moves on a re-time and on an engine swap */
	uint32_t ran_gen;	/* the gen the instance last ran against */
	bool applied;		/* a value stands */
};

/*
 * The canceller reference, tracked per udsp. The engine measures one loop per
 * handle. The applied reference is per line, because each line keeps the
 * value that its own taps stand on. One live udsp per process holds one of
 * these (udsp_priv.h). The engine tick owns every field, and two writes run on
 * the control thread. udsp_chan_all_reset resets the tracker before the tick
 * can see a line. chan_lec_tracker_drop drops a line in the branch of
 * core/stream.c stream_lec_reclaim that reads engine_live false.
 *
 * In a line, loop is the measured value the canceller runs on, unclamped, and
 * delay is the clamp of it that the line consumes. applied says a value
 * stands. It is false on a fresh tracker, after an engine swap and after the
 * release of the instance. The filter then carries no alignment to protect.
 * gen counts the moves. A line whose ran_gen differs runs a canceller that
 * adapted against a reference timed differently from the one it is about to
 * consume.
 */
struct chan_lec_track {
	/* The engine the tracker measured last (engine.h serial). */
	uint64_t serial;
	/* The engine's restart count as it stood at the last completed
	 * calibration window. The fallback counts the restarts since then and
	 * not the restarts of the whole run. Every hold episode restarts the
	 * window, and each of those windows completes. A cumulative count
	 * would spend the allowance on them, and a window that never completes
	 * would then publish a guessed reference. */
	uint64_t restarts_done;
	/* enum chan_lec_offer, the offer of this tick. */
	uint8_t offer;
	/* The loop this tick offers: loop_ref, or loop_last under the
	 * fallback. */
	uint32_t offer_loop;
	/* The last completed calibration window under this engine, unclamped,
	 * or the nominal prefill until one completes. The hold of a played
	 * digit reads it (chan.c chan_digit_hold_arm). */
	uint32_t loop_last;
	/* The applied reference of each line. */
	struct chan_lec_line line[UDSP_LINE_MAX];
};

/* What the reference rule decided for one line.
 *
 * HOLD is an open measurement window inside the fallback allowance. No value
 * is published and adaptation stays held. HOLD wins over the two below, even
 * on the tick that takes a replacement engine.
 *
 * RETIME says the generation of the line moved in this call, because a
 * measurement moved past the tolerance. The canceller of the line then
 * resets. A replacement engine moves every generation in chan_lec_measure,
 * whatever it goes on to measure. The consumers compare generation levels and
 * not the verdict.
 *
 * KEEP publishes the standing value with no generation move in this call. The
 * first value a line applies is a KEEP, because nothing stood before it. */
enum chan_lec_verdict {
	CHAN_LEC_HOLD = 0,
	CHAN_LEC_KEEP,
	CHAN_LEC_RETIME,
};

/*
 * The uplink lookahead a stream that relays the digit out of band reads, in
 * frames of 10 ms (core/chan.c chan_relay_tap_run). The detector validates a
 * digit 42 to 48 ms after the tone starts, so the ring decides how much of
 * the onset reaches the far end:
 *
 *   frames   added delay   leaked tone
 *   0        0 ms          32 to 38 ms
 *   2        20 ms         12 to 18 ms
 *   3        30 ms         2 to 8 ms
 *   4        40 ms         0 ms
 *
 * Three frames hold the leak under the 20 ms an ETSI ES 201 235-3 receiver
 * may not report, and cost one packet interval of delay. The bound holds for
 * a digit of -3 to -25 dBm0 on a window that already runs. Two cases sit
 * outside it: a quieter digit waits for the absolute energy floor of
 * dsp/dtmf_det.c, and a digit that starts inside the two frame warm-up of a
 * freshly armed window validates up to 20 ms later.
 */
#define UDSP_DTMF_MUTE_LOOKAHEAD_FRAMES	3u
/*
 * The floor of the hold the uplink DTMF detector takes while a digit plays
 * toward the line, in ticks of 10 ms (core/chan.c chan_digit_hold_arm). The
 * echo of that digit crosses the loop the engine measures, so the arm takes
 * the larger of this floor and that loop under the reference guard plus four
 * ticks, with or without a canceller on the line. 100 ms outlasts the 70 ms
 * gap between two digits of one string, so the hold does not lapse inside a
 * string, and it stands well above the 35 ms of tone the detector accepts a
 * digit from.
 */
#define UDSP_DL_DIGIT_HOLD_TICKS	10u

/*
 * The slots of one line's loop ring (udsp.h udsp_line_loop), one frame each.
 * One more than the longest delay, because the write of tick T runs before the
 * read of tick T (chan.c chan_tick): with 100 slots a 100 tick delay would
 * read the slot this tick just wrote.
 */
#define UDSP_LOOP_SLOTS	((UDSP_LOOP_DELAY_MAX_MS / UDSP_TICK_MS) + 1u)

/*
 * The offset tracker of the uplink (chan.c chan_ul_offset_remove holds the
 * reasons): the estimate in LSB, the rounding carry of the subtraction, and
 * whether a frame seeded the estimate. CHAN_UL_DC_FRAMES is the time constant
 * of the estimate in frames, 0.64 s at either rate.
 */
#define CHAN_UL_DC_FRAMES	64.0

struct chan_ul_dc {
	double est;
	double carry;
	bool seeded;
};

/*
 * One frame through the tracker. With subtract false the estimate moves and
 * the frame stays as it is. A test that replays the uplink calls this function,
 * so its replay runs the production tracker and not a copy of it.
 */
static inline void chan_ul_dc_frame(struct chan_ul_dc *d, int16_t *pcm,
				    unsigned n, bool subtract)
{
	double sum = 0.0;
	bool reading = false;

	for (unsigned i = 0; i < n; i++) {
		sum += pcm[i];
		reading |= pcm[i] != 0;
	}
	if (!reading)
		return;
	if (d->seeded)
		d->est += (sum / (double)n - d->est) / CHAN_UL_DC_FRAMES;
	else
		d->est = sum / (double)n;
	d->seeded = true;
	if (!subtract)
		return;
	for (unsigned i = 0; i < n; i++) {
		double v = (double)pcm[i] - d->est + d->carry;
		double r = rint(v);

		d->carry = v - r;
		pcm[i] = udsp_sat16_round(r);
	}
}

_Static_assert(UDSP_SAMPLE_RATE_WB / 1000u * UDSP_LEC_HIST_MS <=
	       UDSP_LEC_HIST_SAMPLES,
	       "the reference ring is smaller than the wideband reach");

enum udsp_playout_kind {
	UDSP_PLAYOUT_NONE = 0,
	UDSP_PLAYOUT_TONE,
	UDSP_PLAYOUT_SEGMENTS,
	UDSP_PLAYOUT_DIGITS,
	UDSP_PLAYOUT_FSK,
};

enum udsp_playout_slot {
	UDSP_SLOT_API = 0,
	UDSP_SLOT_SEQ,
	UDSP_SLOT_MAX,
};

struct udsp_playout {
	uint8_t kind;			/* enum udsp_playout_kind */
	/* The line rate the slot renders at, taken from the channel when the
	 * playout starts: playout_stop zeroes the whole slot, so every start
	 * path sets it again. */
	unsigned rate;
	bool paused;
	bool steady;			/* the [tone] then = on tail is running */
	struct udsp_tone_desc desc;	/* the generator holds a pointer to it */
	struct udsp_tone_generator tone;
	struct udsp_dtmf_generator dtmf;
	struct udsp_fsk_mod fsk;

	uint8_t segment_count;
	uint8_t segment_index;
	struct udsp_region_segment segment[UDSP_REGION_SEG_MAX];
	uint16_t silence_ms;
	bool then_steady;
	bool in_silence;
	uint32_t phase_left;		/* samples left in this segment or gap */

	char digits[LINE_CID_DTMF_MAX];
	uint8_t digit_count;
	uint8_t digit_index;
	int8_t level_dbm0;
	uint16_t on_ms;
	uint16_t off_ms;
	bool in_on;

	uint16_t cycles_left;		/* segment form, 0 with a live limit_ms */
	uint32_t cycle_ms;		/* one cadence cycle of the simple form */
	uint32_t limit_ms;		/* finite repeat bound, 0 = unbounded */
	uint32_t duration_ms;		/* caller bound, 0 = unbounded */
	uint32_t elapsed_ms;
	/* The slot rendered its last frame. The answer must survive the frames
	 * a skip renders and throws away, because a render past the end of a
	 * finite cadence starts the cadence again; the tick that reads it
	 * stops the slot and posts the end exactly once. */
	bool done;
};

/*
 * Why a line's fault hold stands. A hold keeps the feed open past the 5 s
 * trial and drops every powered feed write until it lifts.
 *
 * NO_OP: a REINIT or DCDC fault on a plug-in with no matching recovery op.
 * EXHAUSTED: the recovery ran out of attempts.
 * OVLO: a confirmed over-voltage lock-out.
 * CLOCK: the device's clocks read faulty.
 * DEVICE_RESET: the device needs a reset pulse and a full init.
 *
 * CLOCK lifts on a read without the bit. Every other reason lifts at the
 * next start.
 */
enum udsp_fault_held_reason {
	UDSP_FAULT_HELD_NONE = 0,
	UDSP_FAULT_HELD_NO_OP,
	UDSP_FAULT_HELD_EXHAUSTED,
	UDSP_FAULT_HELD_OVLO,
	UDSP_FAULT_HELD_CLOCK,
	UDSP_FAULT_HELD_DEVICE_RESET,
};

/* The recovery op the slic thread's fault scan selected for a line. */
enum udsp_recover_want {
	UDSP_RECOVER_NONE = 0,
	UDSP_RECOVER_REINIT,
	UDSP_RECOVER_CONVERTER,
};

/* The layout of udsp_chan.fault_pub. The low half carries the fault bits the
 * slic thread last read. OPEN says the feed is held open for a trial or a
 * recovery. HELD says a hold stands that no trial ends. */
#define UDSP_FAULT_PUB_BITS	0xffffu
#define UDSP_FAULT_PUB_OPEN	(1u << 16)
#define UDSP_FAULT_PUB_HELD	(1u << 17)

/* GAP: the level did not move but the part recorded a transition, an even
 * count inside one sample interval. LOST: at least one earlier record of the
 * line was dropped on a full ring, so the machine's raw level may be stale.
 * STALL: the slic thread took this sample later than its cadence allows
 * (slic_thread.c slic_hook_gap_note), so the level was unknown for longer
 * than the stamp gap says a cadence would leave it. */
#define UDSP_HOOK_EDGE_GAP	(1u << 0)
#define UDSP_HOOK_EDGE_LOST	(1u << 1)
#define UDSP_HOOK_EDGE_STALL	(1u << 2)
/* IRQ: the pass that took this sample was started by the part's interrupt
 * edge, not by the tick or the IDLE deadline, so the edge it found happened
 * within the interrupt latency before its stamp rather than anywhere inside
 * gap_ms. The pin is shared by every channel, so every line sampled in that
 * pass carries it (slic_thread.c slic_hooks_sample). */
#define UDSP_HOOK_EDGE_IRQ	(1u << 3)

/* One sample record: the stamp, the level, the flags above and the engine
 * time since the line's previous sample, which the hook machine reads as the
 * error a window measured across this record can carry. Eight bytes. */
struct udsp_hook_edge {
	uint32_t stamp_ms;
	uint8_t level;
	uint8_t flags;
	uint16_t gap_ms;
};

struct udsp_hook_edges {
	struct udsp_hook_edge slot[UDSP_HOOK_EDGE_DEPTH];
	atomic_uint head;		/* slic thread */
	atomic_uint tail;		/* engine tick */
};

/* The hook trace: the tick produces it, the slic thread logs it at
 * UDSP_LOG_DEBUG (slic_thread.c slic_hook_trace_drain), so the RT tick writes
 * no syslog line. OPEN is the measurement of one open the machine closed off
 * a seized line (line.h struct line_hook_report); STALL is the jump of the
 * known instant the tick consumed (chan.c chan_line_control). Produced only
 * while UDSP_DEBUG_SLIC_LOG is set. A line that finds the ring full is
 * dropped: the trace is a bench aid and nothing acts on it. */
enum udsp_hook_trace_kind {
	UDSP_HOOK_TRACE_OPEN = 1,
	UDSP_HOOK_TRACE_STALL,
};

struct udsp_hook_trace {
	uint8_t kind;		/* enum udsp_hook_trace_kind */
	uint8_t open_class;	/* OPEN: enum line_hook_open_class */
	uint16_t open_gap_ms;	/* OPEN: the sampler gap of the opening edge */
	uint16_t close_gap_ms;	/* OPEN: the same of the closing edge */
	uint32_t stamp_ms;	/* the instant the machine ran at */
	uint32_t open_lo_ms;	/* OPEN: the open at its earliest; STALL: the jump */
	uint32_t open_hi_ms;	/* OPEN: the open at its latest */
};

#define UDSP_HOOK_TRACE_DEPTH	8u
UDSP_SPSC_DECLARE(udsp_hook_traces, struct udsp_hook_trace,
		  UDSP_HOOK_TRACE_DEPTH);

/*
 * The verdict trace of the line canceller follows the rules of the hook trace
 * above. The tick produces it while UDSP_DEBUG_SLIC_LOG is set. The slic
 * thread logs it at UDSP_LOG_DEBUG (slic_thread.c slic_lec_trace_drain).
 *
 * VERDICT carries the frame reading of dsp/lec.h struct udsp_ec_nlms_frame on
 * a frame the detector fired on. SECOND carries the counters of the instance
 * on every hundredth frame. gen moves wherever the frame count starts over, so
 * a reader can tell two records of one frame number apart.
 */
enum udsp_lec_trace_kind {
	UDSP_LEC_TRACE_VERDICT = 1,
	UDSP_LEC_TRACE_SECOND,
};

/* The five flags of the frame reading, in the flags byte of a VERDICT. */
#define UDSP_LEC_TRACE_FIRED	(1u << 0)
#define UDSP_LEC_TRACE_CLASSIC	(1u << 1)
#define UDSP_LEC_TRACE_TRUSTED	(1u << 2)
#define UDSP_LEC_TRACE_PROBING	(1u << 3)
#define UDSP_LEC_TRACE_TAIL	(1u << 4)

struct udsp_lec_trace {
	uint8_t kind;		/* enum udsp_lec_trace_kind */
	uint8_t flags;		/* VERDICT: UDSP_LEC_TRACE_* */
	uint8_t hold_left;	/* VERDICT */
	uint8_t nlp_verdict;	/* VERDICT */
	uint16_t gen;
	uint16_t window_run;	/* VERDICT */
	uint16_t dt_run;	/* VERDICT */
	uint16_t stale_run;	/* VERDICT */
	uint32_t stamp_ms;	/* the instant the tick ran at */
	uint32_t frames;
	/* VERDICT: the powers and the ERLE of the frame reading. */
	float bar;
	float mic_power;
	float ref_power;
	float window_power;
	float estimate_power;
	float noise_power;
	float erle_db;
	/* SECOND: nlp_dt_fired_frames, nlp_dt_frames, probes and echo_gain of
	 * dsp/lec.h struct udsp_ec_nlms_info, and the pushes this ring refused
	 * since the previous SECOND. */
	uint32_t fired;
	uint32_t dt;
	uint32_t probes;
	uint32_t lost;
	float echo_gain;
};

/*
 * 63 usable records per line. At the IDLE cadence of the slic thread, about
 * 250 ms, that is 630 ms of verdicts at one per frame. The drain takes at most
 * UDSP_LEC_TRACE_DRAIN_MAX records per line per wake. Reason: the log write is
 * synchronous, and the next hook sample of the thread waits behind it.
 */
#define UDSP_LEC_TRACE_DEPTH		64u
#define UDSP_LEC_TRACE_DRAIN_MAX	32u
UDSP_SPSC_DECLARE(udsp_lec_traces, struct udsp_lec_trace,
		  UDSP_LEC_TRACE_DEPTH);

struct udsp_chan {
	unsigned line;
	/* The line rate and the per-tick frame count, rate/100. One rate per
	 * handle, fixed at open; udsp_chan_reset takes it from its caller. */
	unsigned rate;
	unsigned frame;
	bool ready;			/* init succeeded, engine view */
	_Atomic bool dead;
	/* The tick ran chan_line_dead for this death. Engine tick only;
	 * chan_lines_up clears it on the line's return. */
	bool dead_done;
	uint8_t base_lf;		/* enum udsp_linefeed, engine-owned */
	uint8_t polarity;		/* enum udsp_polarity */
	uint8_t state;			/* enum udsp_line_state */
	bool sleeping;
	/* The consumer's last line state request was UDSP_LINE_DOWN, or the
	 * handle opened with udsp_config.lines_start_down and no request came
	 * yet. Every feed raise the engine makes on its own (the start, a
	 * recovery DONE, a dead line's return) reads it: a line the consumer
	 * holds down stays open through them. Engine-owned. */
	bool down_wanted;
	bool core_windows;		/* udsp_line_hook_timing was used */
	/* Mute, read by the media hooks. Atomic so a test can observe the
	 * type 2 mute bracket without racing the tick. */
	_Atomic bool mute;
	bool dtmf_armed;
	/* Engine only: the tick reads no frame into the detector, because a
	 * stream of the line carries a passthrough kind (chan.c
	 * chan_dtmf_hold). */
	bool dtmf_held;
	/* Engine only: the ticks left of the hold a digit playing toward the
	 * line takes. The downlink half arms it and the uplink half counts it
	 * down (chan.c chan_digit_hold_arm, chan_ul). */
	uint8_t dl_digit_hold;
	bool hook_level;		/* last level the tick consumed */
	bool hook_seen;			/* a debounced sample exists */
	uint8_t seq_drain;		/* ticks until the finished playout left the line */
	bool live;			/* the line was up at least once */
	uint32_t last_stamp_ms;		/* edge stamps stay non-decreasing */
	/*
	 * Desired versus confirmed. base_lf and sleeping are what the engine
	 * wants; the two atomics below are what the SLIC actually took, and
	 * only a successful op advances them. Write suppression compares the
	 * confirmed value, so a retry to the same target after a failed write
	 * reaches the register instead of being answered UDSP_OK against a
	 * state the hardware never entered.
	 */
	_Atomic uint_least8_t lf_confirmed;	/* enum udsp_linefeed */
	/*
	 * The feed write in flight. The tick bumps lf_seq_emit for every
	 * linefeed, polarity or ring relay record it queues
	 * (slic_thread.h slic_rec_seq_carried). The slic thread stores the
	 * record's seq in lf_seq_done once it ran, failed or dropped it. A
	 * mismatch means a queued feed write has not run yet. The clear and
	 * the hold lift wait for it, and a held line re-emits OPEN only when
	 * none is pending.
	 */
	uint32_t lf_seq_emit;			/* engine tick only */
	_Atomic uint_least32_t lf_seq_done;
	/* Engine tick only: lf_seq_emit when the caller identity sequence
	 * armed. Only the failure of a later feed write aborts the sequence
	 * (chan.c chan_up_rec_fail). */
	uint32_t cid_arm_seq;
	/* The last sleep state the SLIC took for this line. It does not give
	 * the state of the powersave bit: a plug-in can set the bit at init
	 * from its board data, and a part can share the bit between two
	 * lines. */
	_Atomic bool ps_confirmed;
	/* The SLIC gain pair follows the same model: the pair the SLIC last
	 * took plus a validity bit, packed in one word so the tick never
	 * compares against a half-updated pair. Zero (invalid) until the
	 * first successful gain_set; slic_job_start re-zeroes it before every
	 * init. */
	_Atomic uint_least32_t gain_confirmed;
	struct udsp_chan_cfg cfg;
	/*
	 * The split of the receive gain of the port in half-dB steps, from
	 * cfg (chan.c chan_rx_split_apply). The SLIC takes only the gain half
	 * of slic_gain_rx (udsp_slic_rx_programmed). The engine applies the
	 * loss half ahead of the playout sum, so a local signal at N dBm0
	 * plays at N dBm whatever the pair.
	 * - dl_voice_gain: the network audio and the loop, before the sum;
	 * - dl_local_gain: the playout frame, before the sum, zero or a loss;
	 * - dl_ref_loss: the loss ahead of the canceller reference.
	 */
	int16_t dl_voice_gain;
	int16_t dl_local_gain;
	int16_t dl_ref_loss;
	/* The engine applied a channel configuration since the reset. The zero
	 * cfg of the reset reads as a caller that turned every detector off,
	 * so the marker tells the two apart (chan.c chan_hook_pulse_apply).
	 * Engine-owned. */
	bool cfg_applied;

	struct line_hook hook;
	struct line_ring ring;
	struct line_cidseq cid;
	struct line_cid_gate gate;
	struct line_out out;

	struct udsp_dtmf_detector detect;
	struct udsp_playout slot[UDSP_SLOT_MAX];

	/*
	 * The detectors stay at 8 kHz. A 16 kHz line hands each of them a
	 * decimated copy of its own frame, so the V.21 table and the DTMF
	 * window keep the rate they were designed at. The T.38 gateway reads
	 * no feed here: see rule 4 of core/fax_plugin.h. One instance per
	 * feed, never one per line: the three feeds carry different signals,
	 * and a shared filter history would mix them. rs_ul_pre is the
	 * pre-canceller uplink the DTMF detector reads, rs_ul_post the
	 * post-canceller uplink the local fax detector reads, and rs_dl the
	 * summed downlink the remote fax detector reads. At 8000 none of them
	 * runs and every consumer reads the line frame itself, which is what
	 * keeps the narrowband path byte-identical.
	 */
	struct udsp_resamp rs_ul_pre;
	struct udsp_resamp rs_ul_post;
	struct udsp_resamp rs_dl;
	/* The decimated frame, one feed at a time: no consumer keeps the
	 * pointer past its own call. */
	int16_t det[UDSP_FRAME_SAMPLES];

	/* The control thread creates and destroys the canceller and the engine
	 * tick runs it, so the pointer itself is published (dsp/lec.h). */
	_Atomic(struct udsp_ec *) ec;
	/* Whether the filter ran on the frame this tick carries. chan_tick
	 * clears it and chan_lec_run sets it, so the published record measures
	 * the run instead of recomputing it. chan_tick_ul_media gates the run
	 * on the line's media. chan_lec_publish cannot read that gate, so a
	 * recomputed flag would report a filter on a configured idle line. */
	bool ec_ran;
	/* Whether the open-loop gate of chan_lec_run held the filter on the
	 * frame this tick carries, in the ec_ran shape. chan_lec_publish reads
	 * it and not the level, because the line control of the same tick can
	 * consume an edge that the run did not see. */
	bool lec_loop_open;
	bool ec_nlp;			/* the state the tick last applied */
	uint16_t ec_ref_loss;		/* the loss the tick last applied */
	/* The instance ec_nlp belongs to. A canceller arrives from the control
	 * thread already programmed, so the tick seeds the tracker from the
	 * channel configuration when the pointer moves and compares against it
	 * afterwards. */
	const struct udsp_ec *ec_nlp_for;
	/* The re-times the line paid, engine tick only. It belongs to the
	 * channel and not to the instance, so it survives the reset it counts.
	 * chan_lec_tracker_drop zeroes it when the instance leaves the
	 * channel. */
	uint32_t lec_retimes;
	/* The retired canceller mailbox. The tick moves ec here under
	 * UDSP_CMD_LEC_RELEASE and nulls ec, so the control thread frees it
	 * (dsp/lec.h rule 1) once no tick can still read the instance;
	 * chan_lec_publish reads ec on every tick, so a direct control-side free
	 * would race that read. Atomic because the control thread harvests it
	 * with an acquire exchange even on an ack timeout, where the ack gives no
	 * ordering edge. Only the tick stores a non-NULL pointer here. The
	 * retire fills it only when it reads empty, so a raced release never
	 * overwrites an unharvested instance. The retire writes the END record
	 * before the store, so no read of the instance follows the harvest. */
	_Atomic(struct udsp_ec *) ec_retired;
	bool fax_armed;			/* UDSP_DET_MODEM is set */
	bool fax_active;		/* a local carrier holds the freeze */
	/*
	 * The tone disabler's latch (enum udsp_passthrough), set by an answer
	 * tone from either direction. The tick clears it through
	 * chan_fax_bypass_tick, chan_config_apply, or UDSP_CMD_LEC_LATCH_CLEAR.
	 * Stream teardown clears it through stream_lec_release.
	 * A stream description's kind takes precedence (chan_passthrough_get).
	 *
	 * Atomic because chan_lec_publish reads it on every tick, active
	 * stream or not, while the control thread clears it from
	 * stream_lec_release. The two counters below are plain: the tick
	 * touches them only under an active stream, and the control side
	 * clears them only once the line is idle, which is the argument
	 * fax_active already stands on.
	 */
	_Atomic uint_least8_t fax_bypass_kind;
	uint16_t fax_quiet_frames;	/* both feeds below the hold floor */
	/* The governing streams the line had when the latch was set (stream.c
	 * stream_line_govern_count). A line whose topology moved since is a
	 * line the latch no longer describes, so the release clears it, once
	 * the move outlasts the ticks a reconfiguration quiesces a stream for
	 * (the counter below). */
	uint8_t fax_latch_streams;
	uint8_t fax_topo_frames;	/* consecutive frames the count moved */
	struct udsp_fax_detector fax_local;
	struct udsp_fax_detector fax_remote;
	int16_t hist[UDSP_LEC_HIST_SAMPLES];
	unsigned hist_w;
	/* Frames entered since the start. The tick writes it alone; a test
	 * reads it after a stop. */
	_Atomic uint_least64_t hist_frames;
	int16_t ref[UDSP_FRAME_MAX];

	/* Conference. The mode is engine state: udsp_conf is SYNC-TICK and
	 * reaches it through STREAM_OP_CONF, so no tick ever reads a mode half
	 * applied. The bus and the destination frame are preallocated here
	 * because the mixer runs on the tick, which never allocates. */
	uint8_t conf_mode;		/* enum udsp_conf_mode */
	struct udsp_mix_bus conf_bus;
	int16_t conf_tx[UDSP_FRAME_MAX];

	/* The delayed loop, engine state the tick command STREAM_OP_LOOP
	 * reaches, as the conference mode is. loop_delay is in frames and
	 * never zero while a mode stands. */
	uint8_t loop_mode;		/* enum udsp_loop_mode */
	uint8_t loop_peer;
	uint16_t loop_delay;

	struct udsp_hook_edges edges;
	struct udsp_hook_traces hook_traces;
	/* The verdict trace and its producer state, engine tick only (chan.c
	 * lec_trace_put):
	 * - lec_trace_lost: the pushes the ring refused since the last SECOND;
	 * - lec_trace_gen: the generation of the frame count;
	 * - lec_trace_since: the re-arms since the last VERDICT. */
	struct udsp_lec_traces lec_traces;
	uint32_t lec_trace_lost;
	uint16_t lec_trace_gen;
	uint8_t lec_trace_since;
	/* The canceller capture of udsp.h, engine tick only (chan.c
	 * lec_capture_claim):
	 * - lec_cap_for: the instance the tick last handed a frame to. A change
	 *   marks NEW and moves lec_trace_gen. chan_lec_tracker_drop nulls it,
	 *   for the address reason it nulls ec_nlp_for;
	 * - lec_cap_lost: the records of this line the ring refused since the
	 *   last record of the line it took. It survives the drop;
	 * - lec_cap_skips: the skipped periods of the handle at that last
	 *   record (udsp_priv.h lec_skip_total). The next record carries the
	 *   difference. */
	const struct udsp_ec *lec_cap_for;
	uint32_t lec_cap_lost;
	uint64_t lec_cap_skips;
	_Atomic uint_least64_t hook_pub;

	/*
	 * The fault state, engine tick only except the published word.
	 *
	 * fault_read_bits is the last set the slic thread read. fault_reported
	 * is the last set posted as an event. The two stay apart because the
	 * clear (event.h, code 0) is still due for the last posted fault. A
	 * stop and start must not lose that clear, so chan_lines_up keeps
	 * fault_reported and clears every other field here.
	 *
	 * fault_open says the feed is held open for the 5 s trial or for a
	 * recovery in flight (fault_reinit). fault_held is the hold no trial
	 * ends, with its reason beside it. A clock hold ends on a read
	 * without the CLOCK bit; when that read carries another hold reason,
	 * the hold stays and takes that reason.
	 *
	 * base_lf_wanted is the base feed the last command asked for. Every
	 * path that sets base_lf sets base_lf_wanted with it. A command
	 * during a hold sets base_lf_wanted alone. The lift of the hold then
	 * applies the deferred request, and a polarity change resolves
	 * against it.
	 */
	uint32_t fault_read_bits;
	uint32_t fault_reported;
	uint32_t fault_until_ms;	/* the 5 s re-assert deadline */
	bool fault_open;
	bool fault_reinit;
	/* Deadlines the recovery let pass with no DONE. At two the tick reads
	 * the result as lost, holds the line and tells the slic thread. */
	uint8_t fault_reinit_late;
	/* A write of the base feed is due. A trial, a recovery or the end of a
	 * caller identity sequence queued it, and it did not confirm yet. The
	 * tick writes it again on clean reads, never into a ring session or a
	 * caller identity sequence. The flag clears once lf_confirmed reads
	 * base_lf, and at a start or a stop. */
	bool fault_restore;
	bool fault_held;
	uint8_t fault_held_reason;	/* enum udsp_fault_held_reason */
	uint8_t base_lf_wanted;		/* enum udsp_linefeed */
	/* UDSP_FAULT_PUB_* of fault_read_bits, fault_open and fault_held. The
	 * tick writes it at every change. The monitor getter and the slic
	 * thread read it and never touch the plain fields. */
	atomic_uint fault_pub;
	/* The tick sets it when a recovery's DONE never came; the slic thread
	 * clears it and logs the loss, because the tick never logs. */
	_Atomic bool recover_lost;
	/*
	 * The recovery ladder, slic thread only. recover_want is the op the
	 * last fault read selected. recover_tries counts the failed attempts
	 * since the want was set. recover_holdoff_ms is the engine-clock
	 * instant before which no attempt and no fresh converter read runs.
	 * fault_last_read is the last set the thread read, for the snapshot
	 * edge. ovlo_seen counts consecutive over-voltage readings and stops
	 * at two. fault_inhibit is a suspected short: the converter is down
	 * with the loop closed or the ringer tripped, re-read at the next
	 * holdoff. ladder_next_ms is the engine-clock instant of the next
	 * converter read while a DCDC fault stands; the edge and every
	 * attempt read at once. A DONE the engine did not accept waits in
	 * recover_done_rc while recover_done_pending stands. It is re-posted
	 * on the following passes, and recover_done_tries counts those posts.
	 */
	uint8_t recover_want;		/* enum udsp_recover_want */
	uint8_t recover_tries;
	uint8_t recover_done_tries;
	uint8_t ovlo_seen;
	bool fault_inhibit;
	bool recover_done_pending;
	bool recover_done_was;		/* the sampling flag to restore */
	int32_t recover_done_rc;
	uint32_t recover_holdoff_ms;
	uint32_t ladder_next_ms;
	uint32_t fault_last_read;
	/* The line monitor, slic thread only: the last reading that arrived
	 * and whether one exists. A read that answers no data republishes it
	 * as stale. */
	struct udsp_slic_monitor monitor_last;
	uint32_t monitor_stamp_ms;
	bool monitor_have;
	/* The CLOCK_MONOTONIC instant of a dead line's next init retry, slic
	 * thread only. Armed when the line dies and before every attempt. */
	uint64_t dead_retry_us;
	unsigned hook_fail_run;		/* slic thread only */
	/* The hook sampling record, slic thread only: the wall instant and
	 * the engine stamp of the previous sample, and whether a gap between
	 * this sample and the previous one is a stall rather than a cadence.
	 * The arm is false on the first sample after sampling starts, after
	 * a hold and after a mode entry, because that gap is the cadence. */
	uint64_t hook_sample_us;
	uint32_t hook_stamp_prev_ms;
	bool hook_gap_armed;
	uint64_t hook_gap_log_us;	/* the last gap WARN of the line */
	/* A GAP candidate held one sample (slic_thread.c slic_hook_sample):
	 * the part recorded a transition the level did not show, which is
	 * either a lost pair or a transition that fell between the plug-in's
	 * two reads; the next sample tells them apart. edge_lost says a record
	 * was dropped on a full ring since the last one that pushed. Slic
	 * thread only. */
	bool gap_pending;
	bool gap_stall;
	bool gap_irq;
	bool edge_lost;
	uint16_t gap_gap_ms;
	uint32_t gap_stamp_ms;
	/* The last engine instant the level was known, from the published
	 * sample: the hook machine measures no duration past it, so a sampler
	 * that stalls freezes the machine instead of feeding it a level it
	 * never confirmed. Engine tick only. */
	uint32_t hook_known_ms;
	/* The log rate limit of the bus reads, slic thread only: the stamps
	 * of the last failed read and of the last ERR line (0 before the
	 * first), and whether a logged failure awaits its recovery line. */
	uint64_t read_fail_last_us;
	uint64_t read_fail_log_us;
	bool read_fail_open;
	/* A failed machine record logged once, until a record of the line
	 * confirms again. Slic thread only. */
	bool rec_fail_logged;
	bool slic_level;		/* slic thread only: the last sample */
	bool slic_primed;		/* slic thread only */
	_Atomic bool sampling;		/* slic thread reads, control sets */
	/* True from the slic thread's line gate until the metallic line test
	 * returns. The plug-in holds the line for that whole time: it opens
	 * the linefeed and rings the line for the ringer measurement, so every
	 * op that drives the line is refused while it is set. */
	_Atomic bool under_test;
	/* True while the ring machine or the caller identity sequencer owns
	 * the line, the pair chan_base_lf_set already defers to. The tick owns
	 * both machines, so it publishes the answer here for the two threads
	 * that cannot read them: a metallic line test must not start on a line
	 * that rings or carries a burst, and the slic thread tests it again
	 * before it calls the plug-in. */
	_Atomic bool owned;

	int16_t ul[UDSP_FRAME_MAX];
	int16_t dl[UDSP_FRAME_MAX];
	int16_t mix[UDSP_FRAME_MAX];

	/*
	 * The offset tracker of the uplink, engine tick only. Only
	 * udsp_chan_reset clears it, because the offset belongs to the line
	 * and not to a call.
	 */
	struct chan_ul_dc ul_dc;

	/*
	 * The relay tap, engine tick only: the post-canceller uplink held back
	 * by UDSP_DTMF_MUTE_LOOKAHEAD_FRAMES frames, with a mute mark per
	 * frame, and the frame the ring gives the streams that read it
	 * (core/stream.c stream_relay_tap_reads). The ring stands after the
	 * canceller because a muted near end would drive its estimate toward
	 * zero; the detector keeps the pre-canceller tap.
	 *
	 * digit_active is the digit the detector holds, 0 for none, and
	 * digit_started is the edge of this tick. Both are written wherever
	 * the detector ran and read by the transmit half of the same tick. The
	 * end of a digit is the level of digit_active and no edge of its own,
	 * because a stream off the tick for a reconfiguration would miss it
	 * (core/stream.c stream_tx_digit). digit_level_dbm0 is the row level
	 * the detector measured for digit_active, taken at the start edge. The
	 * relay sends it as the RFC 4733 volume.
	 */
	int16_t ul_delay[UDSP_DTMF_MUTE_LOOKAHEAD_FRAMES][UDSP_FRAME_MAX];
	bool ul_delay_mute[UDSP_DTMF_MUTE_LOOKAHEAD_FRAMES];
	unsigned ul_delay_head;
	unsigned ul_delay_fill;
	int16_t ul_relay[UDSP_FRAME_MAX];
	char digit_active;
	int8_t digit_level_dbm0;
	bool digit_started;
};

#define UDSP_HOOK_PUB_VALID	(UINT64_C(1) << 63)
#define UDSP_HOOK_PUB_LEVEL	(UINT64_C(1) << 32)
/* A sample since the tick's last read was late (UDSP_HOOK_EDGE_STALL): the
 * tick's own per-tick sample carries the jump of the known instant as a gap
 * only then, so the idle cadence of the slic thread is never read as a
 * stall. Sticky: the slic thread carries it across its own later publishes
 * and the tick clears it when it consumes it, both by exchange. */
#define UDSP_HOOK_PUB_STALL	(UINT64_C(1) << 33)

#define UDSP_GAIN_CONF_VALID	(1u << 16)

/* The one encoding of gain_confirmed, shared by the slic-thread store and the
 * engine compare so equality means "the SLIC took exactly this pair". */
static inline uint32_t udsp_gain_confirmed_pack(int8_t tx, int8_t rx)
{
	return UDSP_GAIN_CONF_VALID | ((uint32_t)(uint8_t)tx << 8) |
	       (uint32_t)(uint8_t)rx;
}

/* The receive half that reaches the SLIC: the gain of slic_gain_rx alone. The
 * engine applies a loss in the digital domain (udsp_chan.dl_voice_gain). */
static inline int8_t udsp_slic_rx_programmed(int8_t rx)
{
	return rx > 0 ? rx : 0;
}

/* dst += src with int16 saturation; the mixer primitive every playout and the
 * media path share. */
void frame_add_sat(int16_t *dst, const int16_t *src, unsigned n);

/*
 * The media plane's two places in the tick. UL runs after the gain, the
 * offset removal and the detectors and carries the line echo canceller, the
 * local fax detector and every stream's transmit chain; DL builds the sum of
 * the started streams before the local playout of chan.c is mixed on top.
 */
struct udsp;

void chan_tick_ul_media(struct udsp *s, struct udsp_chan *c, int16_t *frame,
			uint32_t now_ms);
void chan_tick_dl_media(struct udsp *s, struct udsp_chan *c, int16_t *frame,
			uint32_t now_ms);

/* Tick only: queued audio must not cross an idle boundary. */
void chan_relay_tap_reset(struct udsp_chan *c);

/* The loop ring of every line, one allocation each, taken by udsp_open on the
 * control thread and released by udsp_close. */
int chan_loop_pool_alloc(struct udsp *s);
void chan_loop_pool_free(struct udsp *s);
/* Tick only: the arm and the disarm of one line's loop (core/stream.c
 * stream_cmd_run), and the stop that chan_lines_safe runs. */
void chan_loop_ring_clear(struct udsp *s, unsigned line);
void chan_loop_disarm(struct udsp *s, struct udsp_chan *c);

/* Drops the tone disabler's latch. The engine calls it on its own release
 * rules; core/stream.c calls it from the teardown of the line's canceller, the
 * one clear that runs on the control thread. */
void chan_fax_latch_clear(struct udsp_chan *c);

/* Sets the pulse classification of the line from r, the active region, and
 * the channel configuration. The tick calls it from the configuration command,
 * and udsp_region_swap_apply from the swap, which the slic thread runs only
 * while the tick is parked (core/region_ctl.c). */
void chan_hook_pulse_apply(struct udsp_chan *c, const struct udsp_region *r);

/*
 * The reference rule in two steps, without the handle. chan_lec_measure is
 * the handle step: it reads the engine record once per tick and returns the
 * offer of the tick. chan_lec_line_decide applies that offer to one line and
 * returns the clamped value to publish in delay. The tick calls them through
 * chan_lec_delay_track and chan_lec_instance_take. test/test_core.c calls
 * them directly, which is why the whole decision lives here and the wrappers
 * carry only the atomics.
 *
 * A line keeps its standing value while the offer stays within one period of
 * it, in either direction. The taps of the line are aligned to the value that
 * stands. The compare is on the unclamped loop, so a move the reach clamp
 * would fold into its bound still counts as a move.
 */
struct engine_stats;

enum chan_lec_offer chan_lec_measure(struct chan_lec_track *t,
				     const struct engine_stats *st);
enum chan_lec_verdict chan_lec_line_decide(struct chan_lec_track *t,
					   unsigned line, unsigned rate,
					   unsigned frame, uint32_t *delay);
/* Forgets the value that the line stands on, so the next offer applies
 * verbatim. gen and ran_gen stay: the consumers compare their levels. */
void chan_lec_line_drop(struct chan_lec_track *t, unsigned line);

/* Forgets the instance the channel carried: the publish target, the processor
 * tracker, the capture tracker, the re-time count and the reference of the
 * line. Under the capture bit it pushes the END record of an instance that the
 * tick handed a frame. The tick's release handler calls it. The control thread
 * calls it only in the branch of core/stream.c stream_lec_reclaim that reads
 * engine_live false. chan.c gives the reason and the limit. */
void chan_lec_tracker_drop(struct udsp *s, struct udsp_chan *c);

/* Holds the line's uplink DTMF detector and mutes the relay tap for the tail
 * a digit playing toward the line needs (UDSP_DL_DIGIT_HOLD_TICKS). The
 * downlink half of the tick calls it for a digit the caller asked for, and
 * core/stream.c for a telephone-event the library renders itself. */
void chan_digit_hold_arm(struct udsp *s, struct udsp_chan *c);

bool udsp_hook_edge_push(struct udsp_hook_edges *e, uint32_t stamp_ms, bool level,
			 uint8_t flags, uint16_t gap_ms);
bool udsp_hook_edge_pop(struct udsp_hook_edges *e, struct udsp_hook_edge *out);

#endif
