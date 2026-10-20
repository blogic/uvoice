// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * NLMS line echo canceller: the vtable export and the introspection the
 * engine, the cli and the tests read. include/udsp/ec.h holds the stable
 * behaviour contract and dsp/dsp.h belongs to another part of the library, so
 * the extern for the backend lives here. udsp_codec_builtin and
 * udsp_slic_<name>_descriptor are the same kind of named export for a
 * linked-in backend.
 *
 * create(tail_ms) takes 0 as the contract sentinel for the default tail, the
 * meaning the stable udsp.h gives it at udsp_chan_cfg.ec_tail_ms: 48 ms at
 * 8 kHz and 64 ms at 16 kHz. Any other value outside 16..128 ms clamps, and
 * udsp_ec_nlms_tail_get reports what the instance applied.
 *
 * create_rate(tail_ms, rate_hz) is the same call at an explicit line rate
 * (ec.h). It accepts 8000 and 16000 and returns NULL for every other rate.
 * create(tail_ms) is create_rate(tail_ms, 8000), so the 8 kHz line keeps its
 * behaviour. The frame is rate_hz/100 samples and the tap count is tail_ms
 * times rate_hz/1000. The tail clamps to 16..128 ms at 8 kHz and to 16..64 ms
 * at 16 kHz, because the work per millisecond of wideband tail grounds the
 * ceiling at LEC_TAIL_MS_WB_MAX in dsp/lec.c. A 128 ms request on a wideband
 * line therefore reads back as 64 ms with 1024 taps.
 *
 * freeze(on) holds adaptation and nothing else. It never changes the NLP
 * state, and it is not the fax bypass: the engine bypasses this canceller for
 * a fax profile.
 *
 * Rules.
 * 1. create and destroy run on the control path and may allocate.
 * 2. run, freeze and nlp run on the engine tick. They never allocate, never
 *    take a lock and never log.
 * 3. The introspection calls read plain state, so the thread that owns the
 *    instance can call them safely.
 */
#ifndef UDSP_DSP_LEC_H
#define UDSP_DSP_LEC_H

#include <stdbool.h>
#include <stdint.h>

#include "udsp/ec.h"

/*
 * The mean square of a frame, a float sum of float squares over the count.
 * It is the power that every rule of the canceller reads and that the frame
 * reading reports, so the tests and the replay take it from here and read
 * what the canceller reads, to the bit. n is never 0 on a caller's path.
 */
static inline float udsp_ec_nlms_frame_power_get(const int16_t *samples,
						 unsigned n)
{
	float energy = 0.0f;
	unsigned i;

	for (i = 0; i < n; i++)
		energy += (float)samples[i] * (float)samples[i];
	return energy / n;
}

/*
 * What the instance actually applied and what it currently sees. tail_ms is
 * the clamped tail: create() accepts a request outside the supported range
 * and the caller reads back what it got, because a silent clamp hides a
 * request the instance did not honor. core/engine.h reports granted_periods
 * the same way.
 *
 * hold_frames counts the frames whose update did not stand, for any of the
 * three reasons: a silent reference, freeze, or double talk. coef_energy is
 * the energy of the filter, which the divergence watchdog bounds and which a
 * converged instance holds at the echo return loss of the hybrid. probes
 * counts the adaptation trials the detector ran to escape a hold it could not
 * end by verdict. There are three ways in:
 * 1. a latched double-talk run;
 * 2. a frame run in which the canceller was measured to add energy to the
 *    line rather than remove it, which is a hybrid that changed rather than a
 *    talker (dsp/lec.c, LEC_PATH_CHANGE_RATIO);
 * 3. a stale run: fired frames on a trusted estimate that the classic bound
 *    could explain, which is an estimate that went stale (dsp/lec.c,
 *    LEC_GEIGEL_STALE_FRAMES).
 *
 * echo_gain is the learned lower bound of the hybrid's power gain, because a
 * minimum statistic under-reads; echo_gain times the LEC_GEIGEL_GAIN_HEADROOM
 * of dsp/lec.c is the upper bound the Geigel expectation and the
 * combined-loss NLP gate consume. It reads LEC_GEIGEL_GAIN_MIN, over the
 * reference loss of udsp_ec_nlms_ref_loss_set, until the tracker has seen
 * enough active frames to say less.
 *
 * skip_frames counts the frames the tap loops did not run on
 * because the whole reference window was zero (dsp/lec.c frame_zero_run). Each
 * of them is also a hold frame, so skip_frames against hold_frames says how
 * much of the hold was digital silence rather than a quiet or a refused
 * reference.
 *
 * The six nlp counters give each frame the verdict of the processor
 * gate (dsp/lec.c nlp_gate_get). A refused frame counts under the first
 * condition that refused it, in this order:
 * 1. a quiet reference window (nlp_quiet_frames);
 * 2. double talk (nlp_dt_frames);
 * 3. a microphone that the echo estimate does not explain (nlp_unlike_frames);
 * 4. either arm of the ERLE gate (nlp_erle_frames).
 * The processor engages when its engage run holds LEC_NLP_ON_FRAMES eligible
 * frames (dsp/lec.c). A quiet frame whose microphone is echo-like leaves the
 * run as it is. A dt, unlike or erle frame cuts a complete run to
 * LEC_NLP_DECAY_FRAMES of dsp/lec.c, and every other refused frame zeroes it.
 * The eligible frames before the run completes count in nlp_wait_frames.
 * The frame it engages on and each eligible frame after it count in
 * nlp_active_frames, the frames that read nlp_active. A quiet frame counts in
 * nlp_quiet_frames whether it keeps the run or not. A frame with the processor
 * switched off counts nowhere, so the six plus those frames add up to frames.
 * The reset zeroes the six with frames, because they describe one alignment
 * of the reference.
 *
 * coef_peak_tap is the index of the coefficient with the largest
 * magnitude. It counts from tap 0 of the applied reference, so it says where
 * the echo sits against that reference. It reads 0 while coef_energy reads 0.
 * Read the two together, because a filter aligned on its first tap also reads
 * 0.
 *
 * noise_dbov is the near-end floor the instance tracks. It uses the
 * dBov scale of dsp/cn.c udsp_cn_level_get, so a larger number is quieter. It
 * reads 127 while no floor is measured. The instance measures the floor in
 * far-end pauses. Under a far end that never pauses, the seed of dsp/lec.c
 * (noise_seed_update) measures it on active frames. It is introspection for
 * the suites and is not part of the stable record, struct udsp_lec_stats of
 * udsp.h.
 *
 * nlp_dt_fired_frames counts the nlp_dt_frames on which the
 * detector itself fired. The rest of nlp_dt_frames is the hangover behind a
 * verdict. The reset zeroes it with the six. It is not part of the stable
 * record either.
 *
 * ref_floor is the far-end floor after the last frame joined it:
 * the lowest frame power of the reference over the last 300 to 349 frames,
 * a mean square in int16 units (dsp/lec.c, ref_floor_get). It reads 0 until
 * the history is full. run() and udsp_ec_nlms_ref_feed add a frame, and the
 * reset keeps the floor.
 */
struct udsp_ec_nlms_info {
	unsigned tail_ms;
	unsigned taps;
	uint32_t frames;
	uint32_t hold_frames;
	uint32_t diverge_resets;
	uint32_t probes;
	float coef_energy;
	float erle_db;
	float echo_gain;
	bool double_talk;
	bool nlp_active;
	uint32_t skip_frames;
	uint32_t nlp_active_frames;
	uint32_t nlp_wait_frames;
	uint32_t nlp_erle_frames;
	uint32_t nlp_unlike_frames;
	uint32_t nlp_dt_frames;
	uint32_t nlp_quiet_frames;
	unsigned coef_peak_tap;
	float noise_dbov;
	uint32_t nlp_dt_fired_frames;
	float ref_floor;
};

enum udsp_ec_nlms_trial_arm {
	UDSP_EC_NLMS_TRIAL_ARM_NONE,
	UDSP_EC_NLMS_TRIAL_ARM_LATCH,
	UDSP_EC_NLMS_TRIAL_ARM_PATH,
	UDSP_EC_NLMS_TRIAL_ARM_STALE,
};

enum udsp_ec_nlms_trial_end {
	UDSP_EC_NLMS_TRIAL_END_NONE,
	UDSP_EC_NLMS_TRIAL_END_KEPT,
	UDSP_EC_NLMS_TRIAL_END_REJECTED,
	UDSP_EC_NLMS_TRIAL_END_DIVERGED,
	UDSP_EC_NLMS_TRIAL_END_TALKER,
};

/*
 * The reading of the detector and the processor on the last frame that run()
 * took, for the verdict trace of core/chan.c. Like struct udsp_ec_nlms_info
 * it is not part of the stable record. The reset clears it, so a read after
 * a reset returns zeros.
 *
 * The powers are mean squares in int16 units. The detector fires when the
 * window is active and mic_power reaches bar, the echo expectation with every
 * term times the Geigel margin. The reader divides mic_power by bar, so the
 * tick divides nothing. ref_power is the frame's own reference and
 * window_power the loudest frame of the window.
 *
 * - frames: the frame count of the instance after this frame.
 * - noise_power: the tracked near-end floor that the bar carries.
 * - erle_db: the tracked ERLE that the detector read. The frame can move the
 *   tracker after the verdict, so a later read of the tracker can differ.
 * - window_run: the frames the window has been active for, 1 on the first
 *   and 0 while it is inactive. It saturates at UINT16_MAX.
 * - dt_run: the latch run at the end of the frame, saturated the same way.
 * - stale_run: the stale run at the end of the frame, saturated the same way
 *   (dsp/lec.c, LEC_GEIGEL_STALE_FRAMES).
 * - trust_run: the trust count at the end of the frame, the clear active
 *   frames since its last restart, saturated the same way (dsp/lec.c,
 *   LEC_GEIGEL_TRUST_FRAMES). The detector of the frame read the count before
 *   the frame. The trust therefore first holds on the frame after the one
 *   that reads the full count.
 * - hold_left: the hangover before this frame changed it, so 0 on a fresh
 *   verdict and above 0 on a re-arm inside a hangover. On the frame on which
 *   a talker ends a trial, it reads the hangover that the snapshot of the
 *   trial restored.
 * - nlp_verdict: the verdict of the processor gate, which
 *   udsp_ec_nlms_verdict_name names. A trial that ends in the frame can
 *   clear double talk before the gate reads it.
 * - dt_fired: the detector fired on this frame.
 * - classic_arm: the classic bound set the bar, because the ERLE was not
 *   trusted and the bound was above the estimate.
 * - trusted: the ERLE was at or over the trust line, and the coefficient
 *   energy explained the learned gain (dsp/lec.c,
 *   LEC_GEIGEL_TRUST_COEF_RATIO). The trust count was complete too.
 * - probing: a trial was running when the frame started. A trial that ends
 *   in the frame reads 1, and a trial that starts in it reads 0. The detector
 *   ran under that trial state, except on the frame on which a talker ends a
 *   trial: that frame reads 1, and the detector then ran with no trial.
 * - trial_arm: the way into the trial that starts on this frame (struct
 *   udsp_ec_nlms_info names the three), as enum udsp_ec_nlms_trial_arm. It
 *   reads NONE on every frame that starts no trial.
 * - trial_end: the end of the trial that ends on this frame, as enum
 *   udsp_ec_nlms_trial_end. The verdict of the trial gives KEPT or REJECTED,
 *   the watchdog stop gives DIVERGED, and a talker frame gives TALKER. It
 *   reads NONE on every frame that ends no trial. A trial that the reset
 *   clears has no end frame.
 * - ref_quiet: the window is active and the frame's own reference is not,
 *   which is the tail of a talkspurt.
 * - talker: the window is active and mic_power exceeds 16 times (12 dB) the
 *   guard window, the larger window maximum of this frame and of the frame
 *   before (dsp/lec.c, LEC_TALKER_RATIO). The reference loss raises the
 *   ratio (udsp_ec_nlms_ref_loss_set). A frame that starts inside a trial
 *   reads the window against the fixed bar. A talker that ends the trial can
 *   therefore leave the window inactive under ref_bar.
 * - ref_bar: the activity bar of the frame (dsp/lec.c, ref_bar_get). The
 *   frame is active when ref_power reaches it, and the window is active when
 *   window_power reaches it. It reads the fixed bar while a trial runs, and
 *   it follows the end of a trial that a talker ends. The fixed bar is 2500
 *   less the reference loss (udsp_ec_nlms_ref_loss_set).
 * - ref_bar_max: the cap of ref_bar on this frame. It comes from the learned
 *   gain before the frame ran, the echo_gain that info read after the frame
 *   before.
 * - background: the window stands under ref_bar and at or over the fixed
 *   bar, so the far-end floor alone holds the window inactive.
 */
struct udsp_ec_nlms_frame {
	uint32_t frames;
	float mic_power;
	float window_power;
	float estimate_power;
	float bar;
	float noise_power;
	float ref_power;
	float erle_db;
	float ref_bar;
	float ref_bar_max;
	uint16_t window_run;
	uint16_t dt_run;
	uint16_t stale_run;
	uint16_t trust_run;
	uint8_t hold_left;
	uint8_t nlp_verdict;
	uint8_t trial_arm;
	uint8_t trial_end;
	bool dt_fired;
	bool classic_arm;
	bool trusted;
	bool probing;
	bool ref_quiet;
	bool talker;
	bool background;
};

/*
 * Which form of the per-tap loops (the FIR accumulation and the NLMS update)
 * the canceller runs. SIMD is the aarch64 Advanced SIMD form and is the
 * default there; it exists nowhere else, and path_set returns false for it on
 * every other target. The scalar form stays complete and canonical at both
 * line rates and on every target.
 *
 * The two forms are equivalent, not identical: the vector dot product
 * accumulates in thirty-two partial sums instead of one, so the estimate
 * differs from the scalar one by the reassociation of a 128 to 1024 term sum.
 * The vector form must stay within 4 counts per sample and 0.1 dB of ERLE of
 * the scalar one, and test/test_lec.c measures that.
 */
enum udsp_ec_nlms_path {
	UDSP_EC_NLMS_PATH_SCALAR,
	UDSP_EC_NLMS_PATH_SIMD,
};

extern const struct udsp_ec_ops udsp_ec_nlms;

unsigned udsp_ec_nlms_tail_get(const struct udsp_ec *ec);
void udsp_ec_nlms_info_get(const struct udsp_ec *ec,
			   struct udsp_ec_nlms_info *info);
void udsp_ec_nlms_frame_get(const struct udsp_ec *ec,
			    struct udsp_ec_nlms_frame *frame);
/* The word of a processor verdict (off, quiet, dt, unlike, erle, pass), and
 * "?" for a value that is not a verdict. */
const char *udsp_ec_nlms_verdict_name(unsigned verdict);
/*
 * Full adaptive-state reset: coefficients, trackers, detector, probe and the
 * gain estimator go back to the created state; the tail, the NLP switch, the
 * freeze, the comfort noise generator, the measured noise floor and the
 * far-end floor stay. For the engine tick when the measured loop delay
 * recalibrates: the old coefficients then subtract an estimate misaligned
 * against the re-timed reference, which is worse than starting over. RT-safe:
 * no allocation, no logging, no locks.
 */
void udsp_ec_nlms_reset(struct udsp_ec *ec);

/*
 * Takes one frame of reference into the history of the instance, as run()
 * does, and nothing else: no filter, no update, no counter and no processor.
 * Of the trackers it feeds the far-end floor alone, because a skipped frame
 * carries the far end too. For the engine tick when it skips run() on a
 * frame. A filter that does not see the downlink resumes on a window that
 * stopped at the skip, and its first tail of frames adapts against that stale
 * window: on the host model one resumed frame moved the coefficient energy by
 * 16 percent and dropped the ERLE from 60 dB to 15 dB. RT-safe on the terms
 * of run().
 */
void udsp_ec_nlms_ref_feed(struct udsp_ec *ec, const int16_t *ref);

/*
 * The loss in half-dB steps that the engine applies to the downlink ahead of
 * the frame that the instance reads as its reference. The line still carries
 * that loss. The reference therefore stands the loss under the line, and the
 * echo path from the reference to the microphone stands the loss over the
 * hybrid. The instance scales each absolute rule of dsp/lec.c by the loss, so
 * its verdicts stay those of a reference that carries the loss. A loss of 0
 * leaves the arithmetic of every rule as it is, bit for bit.
 *
 * A new instance reads 0, and udsp_ec_nlms_reset keeps the loss. A changed
 * loss starts the instance over through udsp_ec_nlms_reset and starts its
 * far-end floor over, whether the instance ran or not, because its reference
 * history, its learned gain and that floor stand in the old scale of the
 * reference. The same loss again changes nothing. A loss over
 * UDSP_EC_NLMS_REF_LOSS_MAX clamps to it: the engine's receive loss reaches
 * 30 dB (core/udsp.c UDSP_SLIC_GAIN_MIN), and a larger value drives the
 * scaled rules out of range. RT-safe on the terms of the reset.
 */
#define UDSP_EC_NLMS_REF_LOSS_MAX	60u
void udsp_ec_nlms_ref_loss_set(struct udsp_ec *ec, unsigned half_db);
unsigned udsp_ec_nlms_ref_loss_get(const struct udsp_ec *ec);

/*
 * The path switch is a control for the tests and the bench, not a runtime API.
 * It is process wide and applies to every instance from the next per-tap loop
 * on, so a caller that runs two instances side by side sets it between the two
 * run() calls (the equivalence suite does exactly that). path_set returns
 * false when the build carries no such path and leaves the selection alone;
 * path_name returns NULL for a value that is not a path.
 *
 * Quiescence required: path_set must not run while any thread is inside run()
 * on any instance. The selection is a plain file-scope flag with no
 * synchronisation of any kind. path_set allocates nothing, logs nothing and
 * takes no lock, but that makes it RT-safe, not thread-safe. Production never
 * calls it: the default is the fastest form the target carries.
 */
bool udsp_ec_nlms_path_set(enum udsp_ec_nlms_path path);
enum udsp_ec_nlms_path udsp_ec_nlms_path_get(void);
const char *udsp_ec_nlms_path_name(enum udsp_ec_nlms_path path);

/*
 * The zero-reference skip switch of one instance, a control for the tests and
 * the bench that production never writes. A new instance has it on, and the
 * reset keeps it. Off, a frame whose whole reference window is zero walks the
 * tap loops as every other frame does; on, it skips them and counts itself in
 * skip_frames. The two forms produce the same samples and the same state bit
 * for bit, and test_lec runs twin instances with the switch on and off. The
 * setters allocate nothing, take no lock and log nothing. A NULL instance
 * reads off and takes no write.
 */
void udsp_ec_nlms_zero_skip_set(struct udsp_ec *ec, bool on);
bool udsp_ec_nlms_zero_skip_get(const struct udsp_ec *ec);

/*
 * The far-end floor switch of one instance, on the terms of the zero-skip
 * switch above. On, the activity bar of a frame follows the far-end floor
 * (dsp/lec.c, ref_bar_get); off, it is the fixed bar. The floor itself runs
 * either way, so a change of the switch moves the bar on the next frame.
 * test_lec runs twin instances with the switch on and off.
 */
void udsp_ec_nlms_ref_floor_set(struct udsp_ec *ec, bool on);
bool udsp_ec_nlms_ref_floor_get(const struct udsp_ec *ec);

#endif
