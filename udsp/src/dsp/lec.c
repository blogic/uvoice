// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * NLMS line echo canceller behind the stable ec.h vtable: one instance per
 * line, running at the line rate with rate/100 samples per frame (ec.h).
 * create_rate() takes 8000 or 16000 and refuses every other rate; create() is
 * the 8 kHz form of it. The caller feeds the DL frame already delayed by the
 * measured loop delay (ec.h), so tap 0 is the first sample of the hybrid
 * response and the tail is counted from there.
 *
 * Every per-frame constant below counts 10 ms frames rather than samples, so
 * it holds its meaning at any line rate: the frame is rate/100 samples and the
 * tick stays 10 ms (core/clock.h).
 *
 * Single precision state, int16 saturating output: the aarch64 targets have a
 * hard FPU and the frame arithmetic stays far above the float epsilon (a
 * 1024-tap dot product carries about -114 dB of rounding noise, two decades
 * below the deepest level this canceller has to reach).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "dsp.h"
#include "lec.h"

#define LEC_TAIL_MS_MIN		16u
#define LEC_TAIL_MS_MAX		128u
/* The analog line profile: EC on with NLP, 48 ms default tail. */
#define LEC_TAIL_MS_DEFAULT	48u
/*
 * The wideband default is the wideband cap. On two lines at 16 kHz, single
 * calls read a median ERLE 1.6 and 4.2 dB higher with 64 ms of taps than with
 * 48, and the far end hears less tail echo. A least-squares fit of the
 * captured paths puts the gain at 0.3 to 0.6 dB, so the fit does not explain
 * the live reading. The default rests on the live reading. The tick budget
 * is measured at this tail.
 */
#define LEC_TAIL_MS_WB_DEFAULT	64u

/*
 * Wideband tail ceiling. A millisecond of tail at 16 kHz costs twice the taps,
 * and it falls in a frame of twice the samples, so the work of run() per
 * millisecond of tail is four times the narrowband one: a 128 ms wideband tail
 * would cost 655360 multiply-accumulates per 10 ms tick per line, four times
 * the worst case the narrowband line ever asks for, against the same 400 us
 * per frame per line the target allows. 64 ms holds the wideband worst case at
 * twice the narrowband one. The bench admits a longer tail only on measured
 * evidence, and the two-band canceller is the recorded fallback if 64 ms
 * cannot reach the gate.
 */
#define LEC_TAIL_MS_WB_MAX	64u

/* The ceiling is the wideband tap count, so one instance layout serves both
 * line rates; the window bound divides by the narrowband frame, which is the
 * smallest frame any rate asks for and therefore the upper bound. */
#define LEC_TAPS_MAX		(LEC_TAIL_MS_MAX * (UDSP_SAMPLE_RATE_WB / 1000u))
#define LEC_WINDOW_FRAMES_MAX	((LEC_TAPS_MAX + UDSP_FRAME_SAMPLES - 1u) / \
				 UDSP_FRAME_SAMPLES)

/*
 * Step size of the normalised update. NLMS converges for 0 < mu < 2; 0.25 is
 * the telephony compromise, fast enough to clear about 39 dB of misalignment
 * per second on a 48 ms tail and slow enough that a single double-talk frame
 * the detector misses cannot move the filter far.
 */
#define LEC_STEP		0.25f

/*
 * Regularisation of the NLMS denominator. One LSB squared per tap is the
 * smallest reference energy a 16-bit input can carry, so it is the natural
 * floor: it bounds the step on a silent reference without biasing it at any
 * usable level.
 *
 * It is not enough on its own. The tracked near-end floor times the tap
 * count joins it every frame (ec_run): on the first samples of a talkspurt
 * after a quiet reference the window carries almost no energy, and the
 * update of a tap is then the error over a few LSB, which is the near-end
 * noise at nearly full scale. Outside a trial the step scales of
 * talk_scale_get damp that frame; inside a trial they are exempt. Without
 * the floor, a live line whose far end pauses between words grows its
 * coefficient energy by a step at every trial. With the floor in the
 * denominator the
 * step on such a sample is bounded by the error over the noise energy of the
 * window, which is the update a reference below the noise can justify.
 *
 * The whole floor is an energy of the reference, so step_floor_scale scales
 * it by the reference loss (udsp_ec_nlms_ref_loss_set). The loss stands ahead
 * of the reference, and the line still carries it.
 */
#define LEC_POWER_PER_TAP	1.0f

/*
 * Mean square per sample below which the reference carries no usable
 * excitation (about -50 dBm0): the fixed activity bar. Adapting on it only
 * injects gradient noise, so the update, the ERLE tracker and the double-talk
 * decision all stand still and the near-end noise floor is measured instead.
 * The bar of a frame stands at or over it (the far-end floor below).
 *
 * active_bar scales it by the reference loss (udsp_ec_nlms_ref_loss_set). The
 * loss stands ahead of the reference, and the line still carries it, so the
 * bar keeps its level at the line.
 */
#define LEC_REF_ACTIVE_POWER	2500.0f

/*
 * The far-end floor. A stationary far-end background over the fixed bar
 * keeps the reference window active. The detector then reads near speech as
 * double talk for as long as the near party talks. After LEC_GEIGEL_LATCH_MAX
 * frames the latch starts a trial that adapts on that speech. The activity
 * bar of a frame therefore stands over the stationary level of the reference,
 * up to the level whose echo returns at -65 dBm0 (ref_bar_get):
 *
 *   bound   = min(LEC_GEIGEL_GAIN_MIN, echo_gain * LEC_GEIGEL_GAIN_HEADROOM)
 *   bar_max = max(LEC_REF_ACTIVE_POWER, LEC_REF_RETURN_MAX / bound)
 *   bar     = clamp(ref_floor * LEC_REF_FLOOR_MARGIN,
 *                   LEC_REF_ACTIVE_POWER, bar_max)
 *
 * A frame is active when its own power reaches the bar, and its window when
 * the window maximum does. Every consumer reads the two verdicts as before.
 *
 * ref_floor is the lowest raw frame power of the reference over the
 * LEC_REF_FLOOR_SUBS completed sub-windows of LEC_REF_FLOOR_SUB_FRAMES frames
 * and the running one (struct ref_floor). The bar of a frame reads the floor
 * before the frame joins it, so the history holds the 300 to 349 frames
 * before the frame. A ringback burst of 2 s never fills that history. A
 * background that starts reaches the bar within 3.5 s, under the 5 s of the
 * latch. The minima start at 0, so a new instance runs on the fixed bar for
 * its first 300 frames. A smoothed power cannot serve as the floor. It climbs
 * into continuous far speech, and it breaks the identity below on a source
 * with short pauses.
 *
 * LEC_REF_FLOOR_MARGIN is 8 dB. On a captured two-way call the background
 * reads 2.2 dB over its median at p90, and the minimum reads under the
 * median. A model of the latch run on that call latches again at a margin of
 * 4 dB. The margin and the sub-window figures are start values.
 *
 * The identity: one frame of the history under LEC_REF_ACTIVE_POWER over
 * LEC_REF_FLOOR_MARGIN (396.8, about -58 dBm0) gives the fixed bar. Every
 * consumer then runs the arithmetic of the fixed bar bit for bit. A far end
 * that pauses to silence is such a far end, and so is the silence of G.168
 * test 9 (under -60 dBm0).
 *
 * LEC_REF_RETURN_MAX is the mean square of -65 dBm0. dBm0 is dBFS plus 6.15
 * against a full-scale square of 32767 (dbm0_rms_get in test/level.h).
 * -65 dBm0 is therefore -71.15 dBFS, and 32767^2
 * times 10^-7.115 is 82.4. A frame turns inactive only when the loudest echo
 * that the classic bound lets it return (echo_bound_factor_get) stands under
 * that level before any cancellation. G.168 (06/2002) Figure 9 puts the
 * returned echo of a test signal of -30 to -10 dBm0 at or under -65 dBm0,
 * with the processor on. G.168 states no activity bar. The use of that level
 * for the echo before cancellation is therefore a choice of this design.
 *
 * A running trial reads the fixed bar. Under a raised bar, a trial that
 * started on far speech loses its active frames when the phrase ends. Its
 * half-adapted filter then keeps its estimate on the background, and the
 * talker guard does not see those frames. Under the fixed bar, a background
 * over that bar spends the budget of the trial, and the trial ends within
 * LEC_PROBE_FRAMES frames as it does without the floor.
 *
 * A frame whose window stands under the raised bar and at or over the fixed
 * bar is a background frame (background_get). It leaves the latch run as it
 * is, so an echo path that gets louder under a background still reaches the
 * latch trial on the frames of far speech.
 */
#define LEC_REF_FLOOR_SUB_FRAMES	50u
#define LEC_REF_FLOOR_SUBS	6u
#define LEC_REF_FLOOR_MARGIN	6.3f
#define LEC_REF_RETURN_MAX	82.4f

/*
 * Geigel: near-end speech is present when the microphone power rises above
 * the echo the far end can explain, and the margin is what a near-end talker
 * must add on top before the detector fires.
 *
 * Which echo the far end can explain has two answers. The classic one is the
 * loudest frame of the tail window scaled by the 6 dB hybrid ERL: a reference
 * statistic the filter cannot influence, and a window maximum so that a
 * far-end onset does not read as double talk. It is 6 dB pessimistic on a
 * 12 dB hybrid and misses a near-end talker sitting just above the echo, so
 * once the tracker says the filter explains the echo (18 dB of ERLE bounds
 * the misalignment near 1.6 percent) the expectation becomes the echo
 * estimate of this same frame. That estimate is the same reference through
 * the same filter, so it tracks the echo sample for sample instead of only in
 * the mean, and it is what makes a 3 dB near-end excess separable at all.
 *
 * The release runs on clean frames only. The minimum arm is safe under a
 * talker, because near-end speech only ever raises the ratio and a higher
 * ratio cannot lower a minimum. The release is not safe under one: a talker
 * who speaks without a pause raises the learned gain by 2 dB a second while
 * the speech lasts. On the bench talk-over rows at 100 percent overlap, a
 * release under a talker lets the tracker climb from the hybrid's own 0.001
 * towards the near-to-far power ratio, which loosens the bound the step
 * control of talk_scale_get reads and costs that control most of its effect
 * (resets 23 against 0 with the release held). What the release exists for
 * is a hybrid that grew louder, and a frame the detector is holding cannot
 * tell that from a talker.
 *
 * The 6 dB assumption is itself an upper bound the line can beat: the live
 * hybrids measure 22 to 24 dB of ERL, which puts the classic expectation about
 * 17 dB above the echo, and a continuous near-end talker under that ceiling
 * adapts into the filter for the whole bootstrap. The expectation therefore
 * also learns the hybrid's power gain by minimum statistics of the smoothed
 * mic-to-window ratio on active frames, with 2x of headroom on top; the
 * LEC_GEIGEL_GAIN_MIN ceiling keeps a hybrid at or above the 6 dB assumption
 * exactly where it was. The ratio is smoothed and the tracker warms up for 25
 * active frames because a raw onset frame, whose mic does not yet carry the
 * delayed echo, poisons an instantaneous minimum for tens of seconds (measured
 * on a live line).
 *
 * Until then the two are combined by taking the louder: a filter that already
 * predicts more echo than the 6 dB assumption is reporting a hybrid louder
 * than the assumption, and believing the assumption instead holds adaptation
 * for ever on a hybrid that resonates in band (a 1000 Hz reference on a
 * hybrid with 4.5 dB of in-band ERL, G.168 test 6 bench scenario: the hold
 * keeps the filter from ever reaching the ERLE that would trust it).
 *
 * A detector fed by the filter it holds can latch the other way: a hybrid
 * that gets louder leaves the estimate short for ever. A double-talk run the
 * clear frames cannot pay down (they repay four frames each; five unbroken
 * seconds is the shortest run that latches) is taken as that latch rather
 * than as a conversation, and it ends in the adaptation probe below rather
 * than in a change of threshold. Distrusting the estimate for a while instead
 * fails: on a hybrid the classic expectation cannot see
 * either, it hands the filter five seconds of unprotected adaptation on the
 * near-end talker and costs 45 dB of ERLE. The run decays on a clear frame
 * instead of resetting because live far-end speech carries DTX gaps every few
 * hundred milliseconds: a reset there keeps the escape from ever firing on a
 * line that needs it (probes=0 on every live call). A frame of a far-end
 * background under the raised bar of the far-end floor neither adds to the
 * run nor decays it (doubletalk_update).
 *
 * The trusted estimate can also go stale on a hybrid that did not change. A
 * filter that adapts on a tone learns the tone and nothing else, and the
 * tracker gives it trust on that tone. On broadband speech its estimate then
 * stands 20 dB or more under the echo. Each far-end frame fires on the
 * estimate arm under the classic bound, so no clear frame moves the tracker.
 * The far-end pauses pay the latch run down, and the hold lasts the call. One
 * captured call holds that way after a ringback, and another fires that way
 * on its first far-end speech.
 *
 * Three rules answer that. The trust also needs a coefficient energy no lower
 * than the learned gain over LEC_GEIGEL_TRUST_COEF_RATIO. The rule sets no
 * upper bound. For a white reference both are the power gain of the path, so
 * a converged filter carries the gain in its energy. A filter that learned a
 * tone carries the energy of the tone alone. The trust also needs
 * LEC_GEIGEL_TRUST_FRAMES clear active frames since the last restart of the
 * trust count. The count restarts on these events, in the order of a frame:
 * - the window turns active after LEC_GEIGEL_TRUST_PAUSE_FRAMES quiet-window
 *   frames, while the trust did not hold since the last restart
 *   (quiet_run_update, before the detector);
 * - the energy rule turns from refused to held (trust_update, in the
 *   detector, before the trust of the frame);
 * - the instance is created, or udsp_ec_nlms_reset runs.
 *
 * The stale run is the defense behind those rules, for an estimate that goes
 * stale after the count. It counts the fired frames that the classic bar
 * could explain, and LEC_GEIGEL_STALE_FRAMES of them start a trial
 * (stale_due_get). The rules of the run:
 * - a frame of an active window at or over the classic bar clears it,
 *   because the far end cannot explain that frame, so a talker is on the
 *   line;
 * - a clear frame of an active window repays LEC_GEIGEL_RUN_DECAY;
 * - a hangover frame and a quiet window leave it, so far-end pauses do not
 *   pay it down;
 * - an untrusted frame clears it.
 *
 * A frame applies its two clears before the trial arms read the run
 * (probe_arm), because the run keeps its count through a far-end pause. If
 * the arms read the run first, a talker who speaks on the frame that the far
 * end returns on starts a trial. The trial then adapts on that talker for
 * LEC_PROBE_FRAMES active frames.
 *
 * The run has its price. A near signal from about 4 dB under the echo to 3 to
 * 9 dB over it fires on a trusted estimate and stays under the classic bar: a
 * quiet talker, room sound, a television. A run of 200 such fired frames
 * costs one trial that adapts on the near signal, and a rejected trial stands
 * the run down (stale_due_get). The trial emits the samples it adapted on,
 * because a rejection restores the state and not the audio.
 *
 * gain_min scales LEC_GEIGEL_GAIN_MIN by the reference loss
 * (udsp_ec_nlms_ref_loss_set); it seeds the learned gain and caps the bound
 * of echo_bound_factor_get. The loss stands ahead of the reference, and
 * the line still carries it, so the gain from the reference to the
 * microphone stands the loss higher.
 */
#define LEC_GEIGEL_GAIN_MIN	0.25f
#define LEC_GEIGEL_MARGIN	1.41f
#define LEC_GEIGEL_TRUST_DB	18.0f
/*
 * The coefficient energy and the learned gain compare as linear power gains.
 * The rule bounds the energy from below only. On the bench hybrids at 6 to
 * 30 dB of return loss, a filter converged on white noise or on the word
 * source reads 2.0 dB under the gain to 7.8 dB over it. A hybrid quieter than
 * 30 dB holds the gain at its floor, and at 35 dB the filter reads 5 dB under
 * that floor. A filter that learned the ringback read 14 to 19 dB under the
 * gain on a captured call. Six captures read 8.2 to 15.1 dB under it at the
 * end of the ringback. A ringback filter less than the ratio under the gain
 * passes this rule. The trust count is the first defense there, and the
 * stale run the second.
 */
#define LEC_GEIGEL_TRUST_COEF_RATIO	10.0f
/*
 * The trust count (trust_update). A filter that trained on a ringback can
 * pass the energy rule, or turn it held on the first far-end frames. The
 * tracker then still holds the ERLE of the tone, so the trust returns on that
 * figure. The first far-end speech then fires on an estimate that misses its
 * echo. The count makes the trust wait for clear active frames since the
 * last restart. Until then the classic bound sets the bar, so the first
 * far-end speech stays under the bar and adapts. A frame counts after the
 * detector passed it. A fired frame is a frame that the filter does not
 * explain, and a pause gives nothing to learn from.
 *
 * LEC_GEIGEL_TRUST_FRAMES is 200. The line captures hold 13 lines that
 * locked out after the ringback. On 12 of them, the count at the
 * first trusted verdict reads 101 clear frames or fewer. After 200 updates
 * the tracker keeps 0.9^200, under 1e-9, of its state before the restart, so
 * the count needs no second tracker. A larger count delays the first trust of
 * every call.
 *
 * LEC_GEIGEL_TRUST_PAUSE_FRAMES is 200 quiet-window frames, 2 s. On the
 * lockout lines that need the restart, the shortest quiet run before the
 * first far-end speech is 241 frames. The off time of a ringback cadence,
 * 4 s, passes it. A pause after the trust held restarts nothing. Otherwise,
 * on a two-way call, each far return opens the classic bound to a near talker
 * for the whole count. The filter then adapts on that talker.
 */
#define LEC_GEIGEL_TRUST_FRAMES	200u
#define LEC_GEIGEL_TRUST_PAUSE_FRAMES	200u
/* About 6 s on a captured line that a stale estimate held, whose detector
 * fired on 32 frames a second. */
#define LEC_GEIGEL_STALE_FRAMES	200u
#define LEC_GEIGEL_LATCH_MAX	500u
#define LEC_GEIGEL_HANGOVER	24u
/*
 * The hangover of a floor excursion (hangover_get). The long hangover
 * bridges the gaps of a near-end talker. A verdict that the floor's own
 * excursion raises needs no such bridge. On two lines with a silent near
 * end, the long hangover behind such verdicts was most of the double-talk
 * share.
 *
 * A verdict takes the short hangover only when all of these conditions are
 * true:
 * - it stands within 3 dB of the bar (LEC_GEIGEL_MARGINAL_RATIO);
 * - no hangover is running;
 * - no verdict fell within the last LEC_GEIGEL_REPEAT_FRAMES frames;
 * - the microphone stands within 12 dB of the tracked floor
 *   (LEC_GEIGEL_SOFT_RATIO).
 *
 * Each condition keeps a talker out. A talker near the echo level fires
 * again within a few frames, so the next verdict meets the hangover or the
 * repeat limit. The captured verdicts under the short hangover stood 29
 * frames or more apart. A talker whom the far end can hear stands well over
 * the floor. Of the captured verdicts with a measured floor, 40 of 42 stood
 * within
 * 12 dB of it. A line without a measured floor reads a floor of 0. No
 * microphone stands within 12 dB of that, so every verdict there takes the
 * long hangover.
 *
 * With the short hangover on every marginal verdict, the filter adapts on a
 * talker 3 dB over the echo. One limit stays open: verdicts more than the
 * repeat limit apart.
 */
#define LEC_GEIGEL_HANGOVER_SHORT	4u
#define LEC_GEIGEL_MARGINAL_RATIO	2.0f
#define LEC_GEIGEL_SOFT_RATIO	16.0f
#define LEC_GEIGEL_REPEAT_FRAMES	24u
#define LEC_GEIGEL_RUN_DECAY	4u
/*
 * The talker guard. A window-active frame whose microphone power exceeds
 * LEC_TALKER_RATIO times the guard window is near-end speech (talker_get).
 * The guard window is the larger window maximum of this frame and of the frame
 * before. The window of window_power_get covers window_frames frames, the
 * current one included. An echo delayed by nearly the whole tail also reads
 * samples of the frame before them. At 8 kHz with 48 ms, an echo delayed by
 * 47 ms reads 56 of them. The two windows together cover every reference
 * sample that reaches the microphone frame, for every tail of ec_create_rate.
 *
 * A talker frame clears the latch run before probe_arm reads it. Without the
 * clear, a far end that never pauses keeps every frame of a near talker in
 * the run. The latch then starts a trial that adapts on the talker for
 * LEC_PROBE_FRAMES active frames. A talker frame inside a trial ends the
 * trial before the frame adapts (ec_run).
 *
 * The ratio is 12 dB. No path of the suite returns its echo that far over the
 * guard window. The loudest is the hybrid that amplifies by 8 dB. On a white
 * far end at both line rates, its microphone stands 7.5 to 7.6 dB over the
 * guard window on average and 8.6 dB at most. The watchdog bound of 6 dB
 * (LEC_COEF_ENERGY_MAX) does not serve as the ratio. A coefficient energy of
 * 4 bounds the echo only for a white reference, and that hybrid exceeds the
 * bound. A guard at 6 dB reads its echo as a talker, and the latch never
 * starts the trial that the watchdog stops. A talker 12 dB or less over the
 * guard window stays in the latch run.
 *
 * talker_ratio scales the ratio by the reference loss
 * (udsp_ec_nlms_ref_loss_set). The loss stands ahead of the reference, and
 * the line still carries it, so the echo stands the loss higher over the
 * guard window.
 */
#define LEC_TALKER_RATIO	16.0f
/*
 * Learned-bound release and shape. The tighter expectation buys the bootstrap
 * protection at a price the bench measures: after a hybrid path change the
 * lower ceiling holds more transition frames, and reconvergence completes
 * about 2.3 s later than under the fixed 6 dB assumption (the bench row that
 * changes to a late hybrid); the reconvergence test in test_lec bounds that
 * cost.
 * LEC_GEIGEL_GAIN_RISE is +2 dB per second of release (1.0046^100 is about
 * 1.58).
 *
 * gain_floor scales LEC_GEIGEL_GAIN_FLOOR by the reference loss
 * (udsp_ec_nlms_ref_loss_set). The loss stands ahead of the reference, and
 * the line still carries it, so the gain from the reference to the
 * microphone stands the loss higher.
 */
#define LEC_GEIGEL_GAIN_RISE	1.0046f
#define LEC_GEIGEL_GAIN_HEADROOM 2.0f
#define LEC_GEIGEL_GAIN_FLOOR	0.001f
/*
 * The floor terms of the detector's bar and of the processor's echo-likeness
 * test carry this headroom over the tracked floor on an active window. The
 * tracker of noise_floor_update follows a quieter reading fast and a louder
 * one slowly, so it reads under the floor's mean level. The seed takes a low
 * statistic for the same reason. A frame that carries the floor alone
 * therefore stands above the term by the floor's own excursion. On lines
 * with a silent near end, most verdicts stood within 3 dB of the bar.
 * Their reference was at a low level or had just stopped. The processor's
 * test carries the headroom too. Without it there, a frame that the detector
 * passes is refused as not echo-like on the same excursion.
 *
 * On a quiet frame the processor's test takes LEC_GEIGEL_PAUSE_HEADROOM
 * instead. There the test decides only whether the frame keeps the engage
 * run (nlp_apply). On eight one-way captures, 45 of 305 pauses that a
 * complete run entered held a quiet frame over the bar of the active
 * headroom, and 26 of them held one such frame. Those 130 frames stood 0.01
 * to 5.5 dB over the bar at -80 to -72 dBFS. 96 of them stood within 1.46 dB
 * of it, the gap between the two bars, and most of them were excursions of
 * the floor. 9 stood 3 to 6 dB over it. One pause held 40 of the 130. Each
 * such pause zeroed the run, so the next far word waited 7 frames. With a
 * silent reference, the pause headroom puts the bar about 6 dB over the
 * tracked floor (2.8 times the margin), where the active headroom puts it
 * about 4.5 dB over. Every such frame then stands under the bar in 32 of the
 * 45 pauses.
 *
 * The cost: a near sound in a pause between the two bars keeps the run, and
 * the first pass frame of the far return replaces it. On seven two-way
 * captures, 122 pauses that a complete run entered held a frame over the bar
 * of the active headroom. 90 of them also held a frame over the bar of the
 * pause headroom and zeroed the run under either headroom. The loudest frame
 * of 67 of those 90 stood 6 dB or more over the bar of the active headroom.
 */
#define LEC_GEIGEL_FLOOR_HEADROOM 2.0f
#define LEC_GEIGEL_PAUSE_HEADROOM 2.8f
#define LEC_GAIN_AVG_ALPHA	0.04f
#define LEC_GAIN_WARMUP_FRAMES	25u

/*
 * Smoothing of the ERLE tracker that gates the NLP, and the convergence
 * thresholds the NLP waits for. 18 dB of ERLE is well past the point where
 * the residual is dominated by path non-linearity rather than misalignment,
 * and it stays the fast path. The far listener hears the hybrid loss and the
 * canceller's loss together, so the gate also engages on the combined loss: on
 * the measured 22 to 24 dB hybrids the erle-only gate is unreachable although
 * the listener long has more loss than the 18 dB line delivers at 0 dB of ERL.
 * The hybrid loss term is the conservative bound, echo_gain times the
 * headroom, because the minimum statistic under-reads the gain and the raw
 * value would claim 0.5 to 1.5 dB of loss the line does not have. The 6 dB
 * ERLE floor proves the filter tracks the echo before the NLP replaces
 * anything; with the bound at its floor the loss term caps at 27 dB, so the
 * 33 dB gate already implies 6 dB of ERLE and the floor stands as defence in
 * depth. LEC_NLP_ON_FRAMES is the engage hysteresis. The engage run counts the
 * pass frames of nlp_gate_get, and the processor acts only on a pass frame
 * whose run is complete. A quiet frame whose microphone is echo-like leaves
 * the run as it is. A dt, unlike or erle frame cuts a complete run to
 * LEC_NLP_DECAY_FRAMES, and every other refused frame zeroes it (nlp_apply).
 * Reason for the run: without it, a marginal line flaps the gate once a
 * second, and the 10 ms on/off alternation garbles the near path. That flap
 * is a refused frame inside far speech. Reason for the kept run: a pause
 * between far words is no flap, so the processor stays ready for the next
 * word.
 *
 * The combined loss adds the reference loss of udsp_ec_nlms_ref_loss_set to
 * the hybrid loss term. The loss stands ahead of the reference, and the line
 * still carries it, so the far listener hears it on top of the echo_gain that
 * the reference reads.
 */
#define LEC_ERLE_ALPHA		0.1f
#define LEC_ERLE_FLOOR		1.0f
#define LEC_NLP_ERLE_DB		18.0f
#define LEC_NLP_ERLE_MIN_DB	6.0f
#define LEC_NLP_LOSS_DB		33.0f
#define LEC_NLP_ON_FRAMES	8u
/*
 * The count that a dt, unlike or erle frame leaves in a complete engage run
 * (nlp_apply). The processor then acts again on the 4th pass frame after the
 * refusal, not on the 8th. A refused frame inside a run that is not complete
 * zeroes it, and so do a quiet frame over the echo-likeness bar and the off
 * verdict. Refusals with 3 pass frames or fewer between them therefore hold
 * the gate off, because each of them meets a run under LEC_NLP_ON_FRAMES.
 * With 4 pass frames between two refusals, the processor acts on 1 frame in
 * 5. That is the boundary of the guard against a flap.
 *
 * Reason: on the captures, the loudest wait frames followed a single
 * marginal refusal inside a far word. On two short-word captures, the cut
 * leaves 48 and 36 wait frames, against 72 and 56 with the run zeroed. On the
 * second, the loudest wait frame stands at -62.1 dBFS against -53.5 dBFS. On
 * seven two-way captures, the cut adds 0 to 10 engage events per capture and
 * moves no near frame that the processor acts on.
 */
#define LEC_NLP_DECAY_FRAMES	4u

/*
 * Near-end noise floor tracker, in the dBov units of dsp/cn.c (a larger
 * number is quieter). It reads the floor in quiet windows, the frames whose
 * whole reference window is silent. A far end that never pauses has no such
 * frame, and the seed below reads the floor on active frames instead.
 *
 * In a quiet window the tracker follows a quieter reading fast, so it reads
 * under the mean level of the floor and LEC_GEIGEL_FLOOR_HEADROOM keeps its
 * meaning. It follows a louder reading within LEC_NOISE_RISE_DB slowly.
 *
 * A reading more than LEC_NOISE_RISE_DB louder than the floor is near-end
 * speech or a floor that rose. It joins a rise run, and the floor takes the
 * mean of the run only when the run is long and stationary. The reasons for
 * the three rise constants:
 * - LEC_NOISE_RISE_DB mirrors LEC_NOISE_STALE_DB upward. A white floor reads
 *   with a standard deviation of 0.43 to 0.69 dB at 80 or 160 samples.
 * - LEC_NOISE_RISE_FRAMES, 0.5 s, is longer than a syllable and than the
 *   longest word of the test word source (32 frames).
 * - LEC_NOISE_SPREAD_DB bounds the spread of the run. White noise spreads 3
 *   to 5 dB over 50 frames, and colored noise about 7 dB. Syllabic speech
 *   spreads 14 dB or more, and a word next to a pause over 30 dB.
 *
 * The run survives the frames between two quiet windows, so the readings of
 * short far-end pauses add up. Only the run corrects a floor that is more
 * than LEC_NOISE_RISE_DB too low. A run older than LEC_NOISE_RISE_AGE frames
 * starts again, so the readings of two different rooms do not mix.
 *
 * The three rise constants are start values.
 */
#define LEC_NOISE_ALPHA_DOWN	0.25f
#define LEC_NOISE_ALPHA_UP	0.02f
#define LEC_NOISE_RISE_DB	3.0f
#define LEC_NOISE_RISE_FRAMES	50u
#define LEC_NOISE_SPREAD_DB	8.0f
#define LEC_NOISE_RISE_AGE	500u
#define LEC_NOISE_STALE_DB	3.0f
/*
 * And how many active frames in a row have to carry that evidence before the
 * floor is dropped. One frame is not evidence: a 10 ms residual is one
 * realisation of a near end that is not stationary, and a single quiet frame
 * would throw a valid floor away and leave the line with none, because a
 * reference that never falls silent again never re-measures one.
 *
 * The number is the tracker's own downward time constant, 1 over
 * LEC_NOISE_ALPHA_DOWN, which is four frames. Evidence shorter than that is
 * a fluctuation the tracker would have absorbed by itself, so it cannot tell
 * a stale floor from a quiet moment; evidence longer than that describes a
 * floor the tracker would already have followed down if it had been allowed
 * to see the frame at all.
 */
#define LEC_NOISE_STALE_FRAMES	4u

/*
 * The seed of the floor under a far end that never pauses, for example a
 * fan at the far end. Without a floor the bar of the detector carries no
 * floor term, so a frame of the fan echo plus the near floor stands over the
 * bar. The detector then holds the line in double talk. On a host comparison
 * the latch then runs a trial every 8.5 s.
 *
 * The seed runs on active frames, and only while no floor is measured. It
 * never moves a measured floor. A frame qualifies when all of these hold:
 * - (a) no trial runs, because a trial adapts against the detector;
 * - (b) the tracked ERLE is at or over LEC_NOISE_SEED_ERLE_DB;
 * - (c) the residual is at least LEC_NOISE_ECHO_MARGIN times the echo that
 *   this ERLE leaves in the estimate;
 * - (d) the microphone stands within 12 dB of the bar
 *   (LEC_GEIGEL_SOFT_RATIO);
 * - (e) the residual is measurable, a level under 127;
 * - (f) the residual stands within 12 dB of the smoothed residual of the
 *   clear active frames, the residual of the ERLE tracker. The gate refuses
 *   held frames only. ec_run updates that tracker before the seed, so on a
 *   clear active frame the smoothed residual holds at least a tenth of the
 *   residual of the frame (LEC_ERLE_ALPHA).
 *
 * The tracker of the ERLE updates on clear active frames only. It therefore
 * reads the ERLE that the filter reached on the far-end words, not on the
 * fan. Gate (c) holds the leftover echo under a tenth of the residual, so
 * the echo biases a reading by less than 0.4 dB. That bound assumes a linear
 * path inside the band that the filter learned.
 *
 * Gate (f) keeps a steady near talker and a misaligned echo out. A real floor
 * is present on the clear frames too, so their residual carries it. A talker
 * or the echo of a hybrid that changed shows on held frames only. Gates (b)
 * to (e) pass both: a white talker 3 dB over the echo stands 3 dB over the
 * bar, and a line without a floor reads an ERLE of 50 dB or more.
 *
 * LEC_NOISE_SEED_ERLE_DB is 12 dB, under the 18 dB trust line of the
 * detector. Before a floor exists, the detector holds most frames, and the
 * tracked ERLE climbs slowly. On the host comparison at 16 kHz the seed came
 * after 15.6 to 30.0 s with this gate at 18 dB, and after 5.0 to 13.0 s at
 * 12 dB. Gates (c) to (f) guard the bias of the reading.
 *
 * A qualifying frame joins the seed run, and any other frame clears it. A
 * full run of LEC_NOISE_SEED_FRAMES readings with a spread of
 * LEC_NOISE_SPREAD_DB or less seeds the floor. The seed takes the lowest
 * power of the run, plus LEC_NOISE_SEED_BIAS_DB. The lowest power is the
 * largest dBov level of the run, not its smallest number, so the seed is
 * that level less 1. A low statistic keeps a louder frame of the run out of
 * the floor, where the mean would take it in. The bias moves the seed back
 * toward the mean level. On the whole-dB readings of a white floor the seed
 * reads 0.15 to 1.15 dB louder than the floor.
 */
#define LEC_NOISE_SEED_ERLE_DB	12.0f
#define LEC_NOISE_ECHO_MARGIN	10.0f
#define LEC_NOISE_SEED_FRAMES	20u
#define LEC_NOISE_SEED_BIAS_DB	1.0f

/*
 * Adaptation probe. A hybrid that gets louder than the classic 6 dB
 * expectation re-latches the detector as soon as the latch escape lifts: the
 * echo really is louder than any expectation the detector holds, so no
 * verdict it can reach will ever release it. The escape therefore ends in a
 * trial rather than in a verdict. The filter adapts for one second whatever
 * the detector says, with the divergence watchdog underneath, and the trial
 * stands only if the live residual over the live microphone falls by 3 dB
 * against the same ratio measured before it. A trial that fails is undone
 * from a snapshot, so a genuine talker 12 dB or less over the guard window
 * (LEC_TALKER_RATIO) who speaks for five seconds without a pause costs one
 * second of adaptation that is then thrown away.
 *
 * The verdict is a ratio and not the residual power on its own, and the
 * latch arm starts a trial on an active frame only. On a line whose near end
 * listens, the residual follows the far-end speech envelope by 10 to 20 dB
 * between syllables, so an absolute compare accepts trials that end on a
 * quiet syllable and rejects trials that end on a loud one; and a latch that
 * comes due inside a far-end pause would measure its "before" on the near-end
 * floor, which no filter halves, so every such trial would fail by
 * construction.
 */
#define LEC_PROBE_FRAMES	100u
#define LEC_PROBE_IMPROVE	0.5f

/*
 * Path-change escape, the second way into the trial above.
 *
 * A hybrid that changes while the far end keeps talking leaves the filter
 * predicting an echo that is no longer there and missing the one that is. The
 * two are uncorrelated, so the residual carries the sum of both energies and
 * the canceller adds about 3 dB to the line. The detector reads that residual
 * as a near-end talker every few frames, the 24-frame hangover bridges the
 * gaps, and the run never decays: measured at both line rates, the hold then
 * stands for the whole LEC_GEIGEL_LATCH_MAX period with the block ERLE at
 * -3 dB (bench, per-path table, late 28 ms to flat 8 ms).
 *
 * The verdict that ends it is the one the microphone alone cannot give: the
 * canceller's own output against its own input. Near-end speech adds the same
 * energy to both sides and can only move that ratio towards one, so no talker
 * can forge a residual louder than the microphone; only a wrong estimate can.
 * Measured on this bench at 8 kHz, live residual over live microphone: 1.6 to
 * 2.1 after a hybrid change, 0.63 on a near end 3 dB over the echo, 0.92 on a
 * near end at the far-end level, 0.90 on the 0 dB hot hybrid with the filter
 * still at zero, and 0.25 on a hybrid that only got 6 dB louder. The bar sits
 * at 1 dB of added energy, 1.3 dB above the loudest benign reading.
 *
 * The escape ends in the same trial as the latch rather than in a release,
 * because a trial is undone from a snapshot and a release is not: a wrong
 * verdict then costs one adapted second that is thrown away, and the
 * divergence watchdog stays underneath either way. LEC_PATH_CHANGE_FRAMES is
 * the hysteresis, spent on active frames only like the trial's own budget, so
 * a far-end pause neither arms nor disarms it; the live trackers need about
 * eleven frames to cross the bar after a change, and twenty more make a burst
 * of gradient noise unable to reach it.
 */
#define LEC_PATH_CHANGE_RATIO	1.26f
#define LEC_PATH_CHANGE_FRAMES	20u
/*
 * Stand-down after a trial the line refused. Without it a signal that holds
 * the ratio above the bar buys a fresh one-second trial every twenty frames,
 * which is five times the adapted seconds the latch arm alone ever hands to a
 * talker. The stand-down is one whole latch cycle, the latch period plus the
 * trial it ends in: the escape's worst case then costs one adapted second per
 * cycle, exactly the cadence of the latch arm alone. The common case, a trial
 * the line keeps, pays nothing, because only a refusal arms the stand-down.
 * It is spent in active frames, the currency the trial and the run are
 * counted in, and it spends no frame while a trial runs.
 *
 * One counter serves the path-change escape and the stale run, with a mask
 * of the arms it holds (LEC_ARM_PATH, LEC_ARM_STALE). The two never stand
 * down for different lengths: a refusal sets the one length, and a trial
 * carries a running stand-down on (below). A held arm starts no trial while
 * the counter runs; the latch arm is never held.
 *
 * A refused trial stands down the arm that started it, and every arm that
 * was standing down when it started (the carry, probe_start). A latch-arm
 * trial that fails on its own arms nothing: standing the escape down there
 * as well costs 4.5 s of lockout on a hybrid change that follows a failed
 * latch trial (bench, all twelve seeds), which is the very fault the
 * escape exists to remove. The carry exists because the latch arm comes due
 * inside the stand-down: a refusal zeroes dt_run on the frame it starts the
 * stand-down, so a latch trial starts while the stand-down runs. Without the
 * carry, the refusal of that trial leaves the stand-down to run out and hand
 * the escape back twenty frames later. That alternation runs trials at twice
 * the cadence of the latch arm alone (measured 4 in fifteen seconds against
 * the 2 of a canceller with no escape). A steady talker between the two bars
 * drives the latch run and the stale run alike; without the carry the two
 * take turns, 6 trials in 30 s of such a talker on the bench against the 5
 * of the latch arm alone.
 *
 * The length is the whole cycle and not the latch period alone. Reason: the
 * stand-down and the latch run both start on the refusal frame and both
 * spend active frames, and a stand-down of the latch period alone runs out
 * one frame before the latch comes due, so the latch trial finds nothing to
 * carry. The counter is spent after the arms of a frame read it (probe_arm),
 * so a stand-down at 1 on the frame a trial starts is still carried.
 */
#define LEC_TRIAL_STANDDOWN_FRAMES	(LEC_GEIGEL_LATCH_MAX + LEC_PROBE_FRAMES)
#define LEC_ARM_PATH		(1u << 0)
#define LEC_ARM_STALE		(1u << 1)

/*
 * Coefficient energy bound of the divergence watchdog. A passive hybrid can
 * only lose energy, so a solution with more than 6 dB of gain is not a path
 * any more; the filter is reset rather than allowed to feed the line.
 *
 * coef_energy_max scales the bound by the reference loss
 * (udsp_ec_nlms_ref_loss_set). The loss stands ahead of the reference, and
 * the line still carries it, so a filter of the same hybrid holds the loss
 * more energy.
 */
#define LEC_COEF_ENERGY_MAX	4.0f

/*
 * What a rejected trial has to give back. The trial adapts against the
 * detector's verdict, so it drives the trackers, the learned bound and the
 * hangover along with the coefficients; restoring the filter alone would
 * leave the detector thresholding against a line it only imagined, and the
 * escape makes that path far easier to reach than the latch arm does.
 *
 * live_mic_power and live_res_power stay out of the snapshot on purpose: they
 * are measurements of the line rather than beliefs about it, the trial's own
 * verdict is computed from them, and the next escape has to see what is true
 * now. dt_run, path_run and stale_run are not here either, because a failure
 * zeroes them on purpose (the re-arm guards). The comfort noise floor, the
 * processor state and the lifetime counters belong to the audio already
 * emitted. The rise run of the floor tracker stays out too: its readings of
 * quiet windows measure the near end, not the filter. The seed run stays out
 * as well. It holds consecutive frames, and the frames of a trial break it.
 *
 * The trust state is here too: the count, the energy rule of the frame before
 * and the flag of a held trust. A refusal keeps the smaller of the count of
 * the snapshot and the count of the frame (probe_reject). The refusal restores
 * the tracker of the start frame, so the clear frames of the trial gave that
 * tracker nothing. A restart inside the trial stands, because the event that
 * caused it did happen. The rule and the flag take the values of the start
 * frame. The detector goes back to that frame, except for a restart inside
 * the trial, which the count keeps. The quiet-window run stays out: it
 * describes the far end, and a refusal does not undo a pause. The rest of
 * the instance stays out as well: the verdict gap, the stand-down, the
 * processor run, the window run and the window maximum of the frame before,
 * the stale run of the floor, and the cached coefficient energy, which the
 * restored filter recomputes.
 *
 * The instance holds these fields as one struct, so that the snapshot and
 * the restore are one assignment each and a new field cannot miss either.
 *
 * - trust_run is the trust count of the LEC_GEIGEL_TRUST_FRAMES comment,
 *   saturating at UINT16_MAX. trust_rule holds the energy rule of the frame
 *   before. trust_held is set when the trust held on a frame since the last
 *   restart of the count.
 */
struct lec_belief {
	float mic_power_avg;
	float res_power_avg;
	float erle_db;
	float echo_gain;
	float gain_mic_avg;
	float gain_win_avg;
	unsigned gain_warmup;
	unsigned dt_hangover;
	unsigned trust_run;
	bool trust_rule;
	bool trust_held;
};

/*
 * Floor readings that the tracker collects before it moves the floor, in the
 * whole-dB levels of udsp_cn_level_get. quiet and loud are the extremes of
 * the run, so quiet - loud is its spread. age counts the frames since the
 * first reading of the rise run. The seed run needs no age, because a frame
 * that does not qualify clears it.
 */
struct noise_run {
	unsigned frames;
	unsigned age;
	float sum;
	int quiet;
	int loud;
};

/*
 * The history of the far-end floor (the LEC_REF_FLOOR_SUB_FRAMES comment):
 * the lowest frame power of each completed sub-window, with next on the
 * oldest, and the lowest power of the run_frames frames of the running
 * sub-window.
 */
struct ref_floor {
	float sub_min[LEC_REF_FLOOR_SUBS];
	float run_min;
	unsigned run_frames;
	unsigned next;
};

struct udsp_ec {
	unsigned frame;
	unsigned taps;
	unsigned tail_ms;
	unsigned window_frames;
	unsigned window_index;
	unsigned hist_pos;
	unsigned dt_run;
	/* The frames since the last verdict, counted up to
	 * LEC_GEIGEL_REPEAT_FRAMES. It starts at the limit, so the first
	 * verdict of an instance is not a repeat. */
	unsigned dt_gap;
	unsigned path_run;
	unsigned stale_run;
	/* The stand-down of the LEC_TRIAL_STANDDOWN_FRAMES comment: the active
	 * frames left, the arms it holds, and the arms that a refusal of the
	 * running trial stands down. */
	unsigned standdown;
	unsigned standdown_arms;
	unsigned trial_owed;
	unsigned probe_frames;
	unsigned nlp_run;
	uint32_t frames;
	uint32_t hold_frames;
	uint32_t skip_frames;
	uint32_t diverge_resets;
	uint32_t probes;
	uint32_t nlp_active_frames;
	uint32_t nlp_wait_frames;
	uint32_t nlp_erle_frames;
	uint32_t nlp_unlike_frames;
	uint32_t nlp_dt_frames;
	uint32_t nlp_quiet_frames;
	uint32_t nlp_dt_fired_frames;
	/* The frames the window has been active for, saturating at UINT16_MAX.
	 * A skipped frame pushes the window through udsp_ec_nlms_ref_feed,
	 * which recomputes neither the window state nor this count. Across a
	 * skip the count can therefore be stale by the frames of the skip. */
	unsigned window_run;
	/* The quiet-window frames in a row, up to
	 * LEC_GEIGEL_TRUST_PAUSE_FRAMES. A skipped frame leaves it, as it
	 * leaves window_run. */
	unsigned quiet_run;
	/* The window maximum of the frame before, the second half of the
	 * guard window of talker_get. */
	float window_prev;
	/* The exact energy of the history window (hist_push). */
	double hist_energy;
	float power_floor;
	/* The reference loss and the rules it scales, which
	 * ref_loss_scale_set computes once from the constants. g2 is the
	 * power factor of the loss. A loss of 0 makes g2 exactly 1.0f, so
	 * each field then equals its constant. */
	unsigned ref_loss_half_db;
	float ref_loss_db;
	float active_bar;
	float gain_min;
	float gain_floor;
	float talker_ratio;
	float coef_energy_max;
	float step_floor_scale;
	float live_mic_power;
	float live_res_power;
	float probe_ratio_before;
	/* What the instance believes about the line and the filter, and the
	 * snapshot of it that a trial takes (struct lec_belief). */
	struct lec_belief belief;
	struct lec_belief probe_state;
	float noise_dbov;
	float noise_power;
	unsigned noise_stale_run;
	struct noise_run noise_rise;
	struct noise_run noise_seed;
	/* Out of struct lec_belief and kept by the reset: it measures the
	 * far end, not the filter. */
	struct ref_floor ref_floor;
	float coef_energy;
	unsigned coef_peak_tap;
	/* Set by every write to coef; coef_energy_track recomputes the energy
	 * and the peak tap on the next read and clears it, so a frame that
	 * left the filter alone reads the values of the frame that last moved
	 * it. */
	bool coef_dirty;
	bool freeze_on;
	bool nlp_on;
	/* The two test switches of dsp/lec.h, which exist so test_lec can run
	 * two forms side by side. The first says whether a frame whose whole
	 * reference window is zero skips the tap loops (frame_zero_run); the
	 * skip produces the output of the full form bit for bit. The second
	 * says whether the activity bar follows the far-end floor
	 * (ref_bar_get); the floor moves the bar and with it the output. */
	bool zero_skip_on;
	bool ref_floor_on;
	bool noise_valid;
	bool nlp_active;
	struct udsp_ec_nlms_frame reading;
	struct udsp_cn cn;
	float window_power[LEC_WINDOW_FRAMES_MAX];
	float *coef;
	float *backup;
	float *probe;
	float *hist;
	/*
	 * 16 bytes so the vector tap loops load the coefficients aligned: the
	 * four sub-arrays start at multiples of the tap count, and a tap count
	 * is a multiple of 8 floats (ec_create_rate: tail_ms times rate/1000,
	 * and rate/1000 is 8 or 16), so one aligned base aligns all four.
	 * calloc already returns 16-byte alignment on both libcs; this states
	 * the requirement rather than inheriting it.
	 */
	_Alignas(16) float mem[];
};

/*
 * The first condition that refused the processor on a frame, in the order
 * nlp_gate_get checks them, so that each frame has one verdict and nlp_count
 * one field to count it in.
 */
enum nlp_verdict {
	NLP_VERDICT_OFF,
	NLP_VERDICT_QUIET,
	NLP_VERDICT_DT,
	NLP_VERDICT_UNLIKE,
	NLP_VERDICT_ERLE,
	NLP_VERDICT_PASS,
};

/*
 * The record of one frame: the powers run() measures, the bars, the window
 * state and the verdicts of the detector and the processor. It lives on the
 * stack of ec_run, every stage of the frame reads and writes it, and
 * reading_fill copies it into the frame reading at the end of the frame. The
 * reading is telemetry only; no stage reads it.
 *
 * Three readings of "a trial runs" stay distinct: trial_open is the state at
 * entry, probing the state after a talker ended a trial, and the seed gate
 * reads the live count (noise_seed_qualified).
 */
struct lec_tick {
	float mic_power;
	float ref_power;
	float window_power;
	float estimate_power;
	float res_power;
	float bar_max;
	float ref_bar;
	float bar;
	float noise_power;
	float erle_db;
	int level;
	unsigned hold_left;
	enum nlp_verdict nlp_verdict;
	bool active;
	bool window_active;
	bool talker;
	bool held;
	bool trial_open;
	bool probing;
	bool background;
	bool dt_fired;
	bool echo_like;
	bool classic_arm;
	bool trusted;
};

/*
 * The per-tap loops in two forms. The scalar one is canonical: it defines the
 * arithmetic every behavioural constant in this file was measured against, and
 * it is the only form on a target without Advanced SIMD.
 *
 * Advanced SIMD is architecturally mandatory on aarch64 (the Arm architecture
 * reference manual, A1.5), so the vector form needs no runtime probe and no
 * per-tap branch: the switch below picks the whole loop once per sample, and
 * production never writes it. The vector form is a drop-in replacement: the
 * tap sequence, the operands and the update order are the scalar ones, and
 * only the order in which the dot product accumulates changes. The equivalence
 * suite of test/test_lec.c bounds what that costs.
 */
#if defined(__aarch64__)
static bool lec_simd_on = true;
#endif

static float taps_dot_scalar(const float *coef, const float *win, unsigned taps)
{
	float estimate = 0.0f;
	unsigned k;

	for (k = 0; k < taps; k++)
		estimate += coef[k] * win[k];
	return estimate;
}

static void taps_update_scalar(float *coef, const float *win, unsigned taps,
			       float step)
{
	unsigned k;

	for (k = 0; k < taps; k++)
		coef[k] += step * win[k];
}

/*
 * The two loops of a single pass over the coefficients: the update of a sample
 * against its own window, and the estimate of the next sample against the
 * window that follows it. The tap order and the operand of each operation are
 * the ones the two separate loops use, and every coefficient is updated before
 * it is read, so the result is what update-then-dot produces, tap for tap.
 *
 * The two windows overlap by every tap but one: win[k] == next[k+1] for k in
 * 0..taps-1. That is a relation between the contents rather than between the
 * pointers, and it holds everywhere. It holds in the frame where the write
 * index wraps and next sits taps floats above win instead of one float below
 * it: the doubled taps+1 ring is what makes hist[j] and hist[j + taps + 1] the
 * same sample. The vector form below derives one window from the other with
 * it; this one reads both.
 */
static float taps_fused_scalar(float *coef, const float *win, const float *next,
			       unsigned taps, float step)
{
	float estimate = 0.0f;
	unsigned k;

	for (k = 0; k < taps; k++) {
		coef[k] += step * win[k];
		estimate += coef[k] * next[k];
	}
	return estimate;
}

#if defined(__aarch64__)
/*
 * The three vector kernels stay out of line. run() is one large function and
 * the allocator spends its vector registers on the rest of it: inlined, the
 * fused kernel funnels the sixteen window loads and the eight shifted windows
 * through a handful of registers and every accumulate waits on its own
 * operand. Measured in the whole run() at the 64 ms wideband tail, 2.77 ns per
 * tap inlined against 2.44 out of line; the call costs one branch per sample
 * against a thousand taps of work behind it.
 */
/*
 * Eight accumulators of four lanes, so thirty-two taps of the dot product are
 * in flight at once. The measurement behind that number (bench micro-runs on
 * AN7581 at 1.2 GHz): the vector unit takes a Q-form FMLA every 1.3
 * cycles and a Q-form load every 2.4, it never overlaps the two, and the FMLA
 * latency is what four accumulators fail to cover. Four accumulators measure
 * 2.24 ns per tap here, eight measure 1.25, and the two-register and
 * four-register LD1 forms are both slower than plain vld1q_f32 on this core
 * (1.65 and 1.53).
 *
 * The window walks backwards by one sample per call, so its loads are
 * unaligned by construction; with eight accumulators that costs 0.06 ns per
 * tap (1.25 against 1.19 with an aligned window), which is why the layout
 * above aligns the coefficients and leaves the window where it is.
 *
 * The operands are loaded into named registers before the accumulation and the
 * arrays are walked by pointer: with the loads written inside the
 * multiply-accumulate the compiler funnels all sixteen of them through two
 * registers, and every accumulation then waits for its own load. Measured in
 * the whole run() at the 64 ms wideband tail: 3.87 ns per tap that way, 3.43
 * with the named operands, 3.03 with the pointer walk on top (the compiler
 * then pairs the loads and stores).
 *
 * A tap count is a multiple of 8 (ec_create_rate), so the four-wide loop
 * closes the array exactly and no single-tap remainder exists to write. It
 * accumulates into the first accumulator, so the reduction order is fixed
 * whatever the tail is.
 */
__attribute__((noinline))
static float taps_dot_simd(const float *coef, const float *win, unsigned taps)
{
	float32x4_t acc0 = vdupq_n_f32(0.0f);
	float32x4_t acc1 = vdupq_n_f32(0.0f);
	float32x4_t acc2 = vdupq_n_f32(0.0f);
	float32x4_t acc3 = vdupq_n_f32(0.0f);
	float32x4_t acc4 = vdupq_n_f32(0.0f);
	float32x4_t acc5 = vdupq_n_f32(0.0f);
	float32x4_t acc6 = vdupq_n_f32(0.0f);
	float32x4_t acc7 = vdupq_n_f32(0.0f);
	const float *c = coef;
	const float *w = win;
	unsigned k = 0;

	for (; k + 32u <= taps; k += 32u, c += 32u, w += 32u) {
		float32x4_t c0 = vld1q_f32(c);
		float32x4_t c1 = vld1q_f32(c + 4u);
		float32x4_t c2 = vld1q_f32(c + 8u);
		float32x4_t c3 = vld1q_f32(c + 12u);
		float32x4_t c4 = vld1q_f32(c + 16u);
		float32x4_t c5 = vld1q_f32(c + 20u);
		float32x4_t c6 = vld1q_f32(c + 24u);
		float32x4_t c7 = vld1q_f32(c + 28u);
		float32x4_t w0 = vld1q_f32(w);
		float32x4_t w1 = vld1q_f32(w + 4u);
		float32x4_t w2 = vld1q_f32(w + 8u);
		float32x4_t w3 = vld1q_f32(w + 12u);
		float32x4_t w4 = vld1q_f32(w + 16u);
		float32x4_t w5 = vld1q_f32(w + 20u);
		float32x4_t w6 = vld1q_f32(w + 24u);
		float32x4_t w7 = vld1q_f32(w + 28u);

		acc0 = vfmaq_f32(acc0, c0, w0);
		acc1 = vfmaq_f32(acc1, c1, w1);
		acc2 = vfmaq_f32(acc2, c2, w2);
		acc3 = vfmaq_f32(acc3, c3, w3);
		acc4 = vfmaq_f32(acc4, c4, w4);
		acc5 = vfmaq_f32(acc5, c5, w5);
		acc6 = vfmaq_f32(acc6, c6, w6);
		acc7 = vfmaq_f32(acc7, c7, w7);
	}
	for (; k + 4u <= taps; k += 4u, c += 4u, w += 4u)
		acc0 = vfmaq_f32(acc0, vld1q_f32(c), vld1q_f32(w));
	acc0 = vaddq_f32(acc0, acc4);
	acc1 = vaddq_f32(acc1, acc5);
	acc2 = vaddq_f32(acc2, acc6);
	acc3 = vaddq_f32(acc3, acc7);
	return vaddvq_f32(vaddq_f32(vaddq_f32(acc0, acc1),
				    vaddq_f32(acc2, acc3)));
}

/*
 * The update has no accumulation chain: its cost is the three memory accesses
 * per four taps plus one multiply-accumulate. What it does have is a store
 * that depends on the multiply-accumulate one instruction earlier, and this
 * core pays the whole latency for it. Loading all eight blocks first, then
 * accumulating them, then storing them, puts eight independent operations in
 * between: measured on the same core and run, 3.19 ns per tap for the
 * load-accumulate-store block written in place, 1.74 for four blocks split
 * this way, 1.56 for eight.
 */
__attribute__((noinline))
static void taps_update_simd(float *coef, const float *win, unsigned taps,
			     float step)
{
	float32x4_t gain = vdupq_n_f32(step);
	float *c = coef;
	const float *w = win;
	unsigned k = 0;

	for (; k + 32u <= taps; k += 32u, c += 32u, w += 32u) {
		float32x4_t c0 = vld1q_f32(c);
		float32x4_t c1 = vld1q_f32(c + 4u);
		float32x4_t c2 = vld1q_f32(c + 8u);
		float32x4_t c3 = vld1q_f32(c + 12u);
		float32x4_t c4 = vld1q_f32(c + 16u);
		float32x4_t c5 = vld1q_f32(c + 20u);
		float32x4_t c6 = vld1q_f32(c + 24u);
		float32x4_t c7 = vld1q_f32(c + 28u);
		float32x4_t w0 = vld1q_f32(w);
		float32x4_t w1 = vld1q_f32(w + 4u);
		float32x4_t w2 = vld1q_f32(w + 8u);
		float32x4_t w3 = vld1q_f32(w + 12u);
		float32x4_t w4 = vld1q_f32(w + 16u);
		float32x4_t w5 = vld1q_f32(w + 20u);
		float32x4_t w6 = vld1q_f32(w + 24u);
		float32x4_t w7 = vld1q_f32(w + 28u);

		vst1q_f32(c, vfmaq_f32(c0, gain, w0));
		vst1q_f32(c + 4u, vfmaq_f32(c1, gain, w1));
		vst1q_f32(c + 8u, vfmaq_f32(c2, gain, w2));
		vst1q_f32(c + 12u, vfmaq_f32(c3, gain, w3));
		vst1q_f32(c + 16u, vfmaq_f32(c4, gain, w4));
		vst1q_f32(c + 20u, vfmaq_f32(c5, gain, w5));
		vst1q_f32(c + 24u, vfmaq_f32(c6, gain, w6));
		vst1q_f32(c + 28u, vfmaq_f32(c7, gain, w7));
	}
	for (; k + 4u <= taps; k += 4u, c += 4u, w += 4u)
		vst1q_f32(c, vfmaq_f32(vld1q_f32(c), gain, vld1q_f32(w)));
}

/*
 * The fused pass, and the reason it exists in two forms. Two passes over the
 * coefficients cost seven vector operations per four taps (two loads and an
 * accumulate for the estimate; two loads, an accumulate and a store for the
 * update), and this core issues them one at a time: it overlaps neither loads
 * with arithmetic nor one load with the next. One pass costs six, because the
 * coefficients are loaded once instead of twice and the second window is
 * derived instead of loaded.
 *
 * Deriving it: win[k] == next[k+1] (taps_fused_scalar states why, and why it
 * is a relation between contents rather than between pointers), so
 * win[4j..4j+3] is next[4j+1..4j+4], which is vextq_f32 of the two
 * next-window vectors that straddle it. That is why the loop loads the next
 * window and shifts, instead of loading both. It reads one vector past the
 * window end for the last block and uses only its first lane, which is the
 * sample the taps+1 ring keeps and mem_floats_get pads for.
 *
 * Measured on AN7581 at 1.2 GHz, per tap, at the 64 ms wideband tail:
 * 3.03 ns for two separate passes, 2.17 for the fused one.
 */
__attribute__((noinline))
static float taps_fused_simd(float *coef, const float *win, const float *next,
			     unsigned taps, float step)
{
	float32x4_t gain = vdupq_n_f32(step);
	float32x4_t acc0 = vdupq_n_f32(0.0f);
	float32x4_t acc1 = vdupq_n_f32(0.0f);
	float32x4_t acc2 = vdupq_n_f32(0.0f);
	float32x4_t acc3 = vdupq_n_f32(0.0f);
	float *c = coef;
	const float *n = next;
	unsigned k = 0;

	for (; k + 32u <= taps; k += 32u, c += 32u, n += 32u) {
		float32x4_t n0 = vld1q_f32(n);
		float32x4_t n1 = vld1q_f32(n + 4u);
		float32x4_t n2 = vld1q_f32(n + 8u);
		float32x4_t n3 = vld1q_f32(n + 12u);
		float32x4_t n4 = vld1q_f32(n + 16u);
		float32x4_t n5 = vld1q_f32(n + 20u);
		float32x4_t n6 = vld1q_f32(n + 24u);
		float32x4_t n7 = vld1q_f32(n + 28u);
		float32x4_t n8 = vld1q_f32(n + 32u);
		float32x4_t c0 = vfmaq_f32(vld1q_f32(c), gain,
					   vextq_f32(n0, n1, 1));
		float32x4_t c1 = vfmaq_f32(vld1q_f32(c + 4u), gain,
					   vextq_f32(n1, n2, 1));
		float32x4_t c2 = vfmaq_f32(vld1q_f32(c + 8u), gain,
					   vextq_f32(n2, n3, 1));
		float32x4_t c3 = vfmaq_f32(vld1q_f32(c + 12u), gain,
					   vextq_f32(n3, n4, 1));
		float32x4_t c4 = vfmaq_f32(vld1q_f32(c + 16u), gain,
					   vextq_f32(n4, n5, 1));
		float32x4_t c5 = vfmaq_f32(vld1q_f32(c + 20u), gain,
					   vextq_f32(n5, n6, 1));
		float32x4_t c6 = vfmaq_f32(vld1q_f32(c + 24u), gain,
					   vextq_f32(n6, n7, 1));
		float32x4_t c7 = vfmaq_f32(vld1q_f32(c + 28u), gain,
					   vextq_f32(n7, n8, 1));

		vst1q_f32(c, c0);
		vst1q_f32(c + 4u, c1);
		vst1q_f32(c + 8u, c2);
		vst1q_f32(c + 12u, c3);
		vst1q_f32(c + 16u, c4);
		vst1q_f32(c + 20u, c5);
		vst1q_f32(c + 24u, c6);
		vst1q_f32(c + 28u, c7);
		acc0 = vfmaq_f32(acc0, c0, n0);
		acc1 = vfmaq_f32(acc1, c1, n1);
		acc2 = vfmaq_f32(acc2, c2, n2);
		acc3 = vfmaq_f32(acc3, c3, n3);
		acc0 = vfmaq_f32(acc0, c4, n4);
		acc1 = vfmaq_f32(acc1, c5, n5);
		acc2 = vfmaq_f32(acc2, c6, n6);
		acc3 = vfmaq_f32(acc3, c7, n7);
	}
	/* The tail loads the update window instead of shifting into it: there
	 * is no next block to straddle, and a tap count is a multiple of 8 so
	 * this runs at most twice. */
	for (; k + 4u <= taps; k += 4u, c += 4u, n += 4u) {
		float32x4_t cv = vfmaq_f32(vld1q_f32(c), gain,
					   vld1q_f32(win + k));

		vst1q_f32(c, cv);
		acc0 = vfmaq_f32(acc0, cv, vld1q_f32(n));
	}
	return vaddvq_f32(vaddq_f32(vaddq_f32(acc0, acc1),
				    vaddq_f32(acc2, acc3)));
}
#endif

static float taps_dot(const float *coef, const float *win, unsigned taps)
{
#if defined(__aarch64__)
	if (lec_simd_on)
		return taps_dot_simd(coef, win, taps);
#endif
	return taps_dot_scalar(coef, win, taps);
}

static void taps_update(float *coef, const float *win, unsigned taps,
			float step)
{
#if defined(__aarch64__)
	if (lec_simd_on) {
		taps_update_simd(coef, win, taps, step);
		return;
	}
#endif
	taps_update_scalar(coef, win, taps, step);
}

static float taps_fused(float *coef, const float *win, const float *next,
			unsigned taps, float step)
{
#if defined(__aarch64__)
	if (lec_simd_on)
		return taps_fused_simd(coef, win, next, taps, step);
#endif
	return taps_fused_scalar(coef, win, next, taps, step);
}

/*
 * coef, backup and probe are one tap array each; the history ring is the
 * doubled taps+1 window of hist_push plus a four-float pad, because the vector
 * fused kernel reads one vector past the window end and discards the lanes it
 * does not use.
 */
static unsigned mem_floats_get(unsigned taps)
{
	return 3u * taps + 2u * (taps + 1u) + 4u;
}

/*
 * The loudest echo the far end can explain in this frame: the reference
 * window maximum times the learned hybrid gain with its headroom, capped by
 * the 6 dB assumption the same way the detector's classic arm is (the
 * LEC_GEIGEL_GAIN_MIN comment carries the derivation). It is a property of the
 * reference and the hybrid, so near-end speech cannot move it, which is what
 * makes it usable where the filter's own estimate is not.
 *
 * The factor alone also sets the cap of the activity bar (ref_bar_max_get),
 * so the bar and the bound read one figure of the hybrid.
 */
static float echo_bound_factor_get(const struct udsp_ec *ec)
{
	float bound = ec->belief.echo_gain * LEC_GEIGEL_GAIN_HEADROOM;

	if (bound > ec->gain_min)
		bound = ec->gain_min;
	return bound;
}

static float echo_bound_get(const struct udsp_ec *ec, float window_power)
{
	return window_power * echo_bound_factor_get(ec);
}

/*
 * The share of the microphone in this frame that the far end can explain at
 * all.
 *
 * The case it answers: on the frame a near-end word starts, the detector can
 * miss the verdict by a decibel, and the update then runs at full step
 * against an error that is near-end speech. On the field lines the near end
 * is about 15 dB over the echo, so one such frame moves the coefficient
 * energy by two orders of magnitude, and a field call resets the watchdog
 * eight times that way. The bench attribution is exact: of the frames that
 * adapted with the near end talking, every one was let through by the
 * estimate arm, at a mean step scale of 0.999, a mean 2.3 dB under the bar,
 * with the microphone 15.1 dB above what the hybrid could return.
 *
 * Why the detector cannot answer it alone: its estimate arm is computed from
 * the same frame it is judging, and the per-sample update has already pulled
 * the filter towards the near end by the end of that frame. The arm
 * therefore rises with the contamination and hides it. The bound above does
 * not, so the step is scaled by the fraction of the microphone that the bound
 * alone can account for.
 *
 * It is inert wherever nothing is unexplainable: a microphone at or under the
 * explainable power returns exactly 1.0f. That exactness is structural in
 * that branch alone, and single talk does leave the branch: the learned gain
 * is a minimum statistic that under-reads, so a clean frame can sit a
 * fraction of a decibel over the bound and take a slightly smaller step. It
 * costs the 12 dB gate table a measurable amount at both line rates, worst
 * over the forty-seed sweep: 0.60 dB of ERLE at one second at 8 kHz and
 * 0.61 dB at 16 kHz, with reconvergence up to 9 ms longer, and the
 * clean-frame adaptation counts unchanged at every talk-over density. A
 * microphone this code cannot read falls on the inert end, for the reason
 * step_scale_get gives.
 *
 * The two scales multiply because they answer different questions: how much
 * of the residual is still echo to remove, and how much of the microphone the
 * far end can explain at all.
 */
static float talk_scale_get(const struct udsp_ec *ec, float mic_power,
			    float window_power)
{
	float explain;

	/*
	 * A trial is exempt. The throttle is derived from the learned gain,
	 * and a trial exists to test the one hypothesis the learned gain
	 * cannot hold: that the hybrid grew louder than anything the tracker
	 * has seen. Throttling the trial by that tracker lets the
	 * hypothesis veto its own test, and it does: measured on a 23 dB
	 * hybrid stepped to 3 dB, ten trials fired in sixty seconds and every
	 * one failed its improvement test and restored the old filter, where
	 * a canceller without this control reconverges in 6.0 s.
	 *
	 * This is the same exemption ec_run already grants the trial against
	 * the double-talk hold ("probing || !double_talk_get"), for the same
	 * reason and with the same net underneath it. A talker more than 12 dB
	 * over the guard window ends the trial before the frame adapts
	 * (LEC_TALKER_RATIO). A trial whose coefficients leave the watchdog
	 * bound ends on that frame in probe_diverged. probe_reject puts the
	 * snapshot back in both.
	 */
	if (ec->probe_frames)
		return 1.0f;
	/*
	 * The bound is the detector's own classic arm, the same call and the
	 * same 6 dB ceiling, so the two never disagree about what the far end
	 * can account for. What lies between them is a band: a microphone
	 * over the bound but under the detector's margin adapts, at a step
	 * reduced in proportion to the excess, which is the graded answer
	 * this control exists to give between "clearly echo" and "clearly
	 * something else". Above the margin the detector holds the frame
	 * outright, and the only adaptation left there is a trial, exempt
	 * above.
	 *
	 * Nothing else joins the bound. Two more terms add nothing: the
	 * previous frame's estimate, for the learned gain's climb after a
	 * hybrid gets louder, and the uncapped gain, for a hybrid louder than
	 * the assumption. With the trial exempt, the trial covers both, and
	 * neither term changes a single measured number (23 dB stepped to
	 * 13, 16 and 3 dB, the amplifying rows at -8, -4, 0 and +4 dB ERL,
	 * and the noise-dominated convergence, all identical with and
	 * without them).
	 */
	explain = echo_bound_get(ec, window_power);
	/*
	 * Total in the same sense step_scale_get is: the false arm of each
	 * comparison is the one a value that is not a number takes, and the
	 * quotient is reached only when the ordering that bounds it is
	 * proven. An unreadable microphone or an unreadable bound therefore
	 * falls on the unscaled step, the step of a canceller without this
	 * control, rather than on a scale that could be negative
	 * or larger than one.
	 */
	if (!(explain >= 0.0f) || !(mic_power > explain))
		return 1.0f;
	return explain / mic_power;
}

/*
 * The step of a frame, scaled by the share of the residual that is still echo.
 *
 * The gradient of an NLMS update is the error, and on a line whose near end
 * carries its own floor part of that error is noise the reference cannot
 * explain. Adapting on it at the full step buys nothing and costs
 * misadjustment: the filter walks around its optimum and the walk is added to
 * the line. Measured on the weak-hybrid bench rows with the echo 12 dB over
 * the floor, 44 of 149 single-talk windows came out louder than they went in,
 * by up to 1.8 dB, which the far end hears as a repeated syllable.
 *
 * The scale is the classic one: the residual with the floor taken out, over
 * the residual. It is 1 while the residual is echo, and it closes as the
 * filter reaches the floor, which is the point where there is nothing left
 * to learn. The residual is the smoothed live one, so the scale is the
 * previous frames' measurement and never this frame's own output.
 *
 * A line with no measurable floor returns exactly 1.0f, so the arithmetic
 * downstream is bit for bit that of the unscaled step (noise_power_set states
 * when the floor is not measurable). A residual at or under the floor returns
 * 0: the filter holds its solution instead of stirring it.
 */
static float step_scale_get(const struct udsp_ec *ec)
{
	float floor = ec->noise_power;
	float res = ec->live_res_power;

	/*
	 * Every comparison below is written so that its false arm is the one
	 * a value that is not a number takes: NaN compares false against
	 * everything, so `!(floor > 0.0f)` catches it and no separate
	 * isfinite() is needed anywhere in this function.
	 *
	 * The two ends are chosen, not incidental. A floor that is not a
	 * usable number means the line has told us nothing about its near
	 * end, so the step is the unscaled one, as in a canceller without
	 * this control. A residual that is not a usable
	 * number is the opposite: it is the quantity the update is driven by,
	 * and an update driven by a measurement this code cannot read is the
	 * one thing that must not happen, so it falls on the zero step.
	 * divergence_check re-seeds both live powers from the frame's own
	 * microphone, which is a float of a bounded int16 sum and therefore
	 * always finite, so neither end can latch.
	 */
	if (!(floor > 0.0f))
		return 1.0f;
	if (!(res > floor) || !(res < INFINITY))
		return 0.0f;
	/* res > floor > 0 and res is finite, so the quotient is in (0, 1). */
	return (res - floor) / res;
}

/*
 * The window is kept twice so the tap loop reads it as one forward run. The
 * write index walks backwards, which puts the newest sample first: win[k] is
 * the reference delayed by k samples.
 *
 * The ring holds one sample more than the window, and that extra slot is what
 * makes the fused pass of frame_filter_run possible. That pass holds the
 * windows of two consecutive samples at once, and their union is taps+1
 * distinct samples; a ring of exactly taps slots has room for taps of them, so
 * the push of sample i+1 falls on the last element of the window of sample i.
 * It falls there twice over: on the duplicate write at pos+taps in the common
 * case, and on the primary write when pos wraps to 0, where hist[taps-1] is at
 * once the newest sample of window i+1 and the oldest of window i. Deferring
 * the duplicate write alone therefore leaves the collision in place, and the
 * ring is sized taps+1 instead.
 *
 * The window contents are those of a ring of taps slots, sample for sample,
 * so the extra slot changes no arithmetic downstream.
 */
static const float *hist_push(struct udsp_ec *ec, int16_t sample)
{
	unsigned period = ec->taps + 1u;
	unsigned pos = ec->hist_pos;
	float value = (float)sample;
	float leaving = ec->hist[pos + ec->taps - 1u];

	/*
	 * int16 values are exact in float and their squares are exact in
	 * double, so this sliding sum is the window energy with no drift; a
	 * periodic rescan would buy nothing.
	 */
	ec->hist_energy -= (double)leaving * leaving;
	pos = pos ? pos - 1u : period - 1u;
	ec->hist[pos] = value;
	ec->hist[pos + period] = value;
	ec->hist_energy += (double)value * value;
	ec->hist_pos = pos;
	return &ec->hist[pos];
}

/*
 * One frame of NLMS, sample by sample: the estimate comes out of the
 * coefficients the previous sample left behind, and the update follows
 * immediately. Deferring the updates of a whole frame is cheaper to write but
 * it applies the same correction once per repetition of a periodic reference,
 * and a 1000 Hz tone then drives an 80-sample block update past the
 * stability bound and into the divergence watchdog (G.168 test 6 bench
 * scenario).
 *
 * The per-sample work does not change, and the frame is walked in one pass
 * over the coefficients per sample instead of two: the update of sample i and
 * the estimate of sample i+1 read the same coefficient array, so they are done
 * together (taps_fused). What remains outside the fused pass is the estimate
 * of the first sample of the frame, which has no update in front of it, and
 * the update of the last, which has no estimate behind it. A frame that does
 * not adapt has no update to fuse and walks the estimate alone.
 *
 * The order of every arithmetic operation is the one of two separate loops,
 * tap for tap: the step of sample i is taken before the push of sample i+1
 * moves the reference energy, and each coefficient is updated before the
 * estimate reads it. The fused pass and two separate loops therefore give
 * the same numbers.
 *
 * Returns the residual energy of the frame; the residual itself is written
 * back over the microphone frame, saturated.
 */
static float frame_filter_run(struct udsp_ec *ec, int16_t *mic,
			      const int16_t *ref, bool adapt, float mu,
			      float step_floor, float *estimate_power)
{
	const float *win = hist_push(ec, ref[0]);
	float estimate = taps_dot(ec->coef, win, ec->taps);
	float res_power = 0.0f;
	unsigned i;

	for (i = 0; i < ec->frame; i++) {
		const float *next;
		float error;
		float step = 0.0f;

		*estimate_power += estimate * estimate;
		error = (float)mic[i] - estimate;
		res_power += error * error;
		mic[i] = udsp_sat16_round(error);
		if (adapt)
			step = mu * error /
			       ((float)ec->hist_energy + step_floor);
		if (i + 1u == ec->frame) {
			if (adapt)
				taps_update(ec->coef, win, ec->taps, step);
			break;
		}
		next = hist_push(ec, ref[i + 1u]);
		estimate = adapt ?
			taps_fused(ec->coef, win, next, ec->taps, step) :
			taps_dot(ec->coef, next, ec->taps);
		win = next;
	}
	return res_power;
}

/*
 * The frame over a reference whose whole window is zero: the history before
 * the frame carries no energy and the frame's own samples are zero. The
 * estimate of every sample is then exactly 0.0f, because every product of
 * taps_dot is a signed zero and a sum of signed zeros is a signed zero, and a
 * residual of mic minus a signed zero is mic itself, sample for sample. The
 * frame is inactive, so it adapts nothing. What the tap loops fed is kept: the
 * history push per sample, so the ring stands where the full form leaves it,
 * and the residual energy in the same accumulation order.
 */
static float frame_zero_run(struct udsp_ec *ec, const int16_t *mic,
			    const int16_t *ref)
{
	float res_power = 0.0f;
	unsigned i;

	for (i = 0; i < ec->frame; i++) {
		float error = (float)mic[i];

		hist_push(ec, ref[i]);
		res_power += error * error;
	}
	return res_power;
}

/* The ring entry of one frame of reference, read by window_power_get. */
static void window_push(struct udsp_ec *ec, float ref_power)
{
	ec->window_power[ec->window_index] = ref_power;
	ec->window_index = (ec->window_index + 1u) % ec->window_frames;
}

static float window_power_get(const struct udsp_ec *ec)
{
	float loudest = 0.0f;
	unsigned i;

	for (i = 0; i < ec->window_frames; i++) {
		if (ec->window_power[i] > loudest)
			loudest = ec->window_power[i];
	}
	return loudest;
}

/* One frame of reference into the history of the far-end floor. A full
 * running sub-window replaces the oldest completed one. */
static void ref_floor_add(struct ref_floor *rf, float power)
{
	if (!rf->run_frames || power < rf->run_min)
		rf->run_min = power;
	if (++rf->run_frames < LEC_REF_FLOOR_SUB_FRAMES)
		return;
	rf->sub_min[rf->next] = rf->run_min;
	rf->next = (rf->next + 1u) % LEC_REF_FLOOR_SUBS;
	rf->run_frames = 0;
}

/* The lowest frame power of the history. An empty running sub-window holds
 * no frame, so it does not count. */
static float ref_floor_get(const struct ref_floor *rf)
{
	float lowest = rf->run_frames ? rf->run_min : rf->sub_min[0];
	unsigned i;

	for (i = 0; i < LEC_REF_FLOOR_SUBS; i++) {
		if (rf->sub_min[i] < lowest)
			lowest = rf->sub_min[i];
	}
	return lowest;
}

/*
 * The cap of the activity bar, from the learned gain as it stands. ec_run
 * reads it before the frame runs, because a talker that ends a trial restores
 * another gain. A quotient that is not a number takes the fixed bar, the rule
 * of step_scale_get.
 */
static float ref_bar_max_get(const struct udsp_ec *ec)
{
	float bar_max = LEC_REF_RETURN_MAX / echo_bound_factor_get(ec);

	return bar_max > ec->active_bar ? bar_max : ec->active_bar;
}

/*
 * The activity bar of a frame, by the rule of the LEC_REF_FLOOR_SUB_FRAMES
 * comment. The frame has not joined the floor yet. probing says whether a
 * trial runs on the frame, after the end that a talker frame gives a trial.
 */
static float ref_bar_get(const struct udsp_ec *ec, float bar_max,
			 bool probing)
{
	float bar;

	if (probing || !ec->ref_floor_on)
		return ec->active_bar;
	bar = ref_floor_get(&ec->ref_floor) * LEC_REF_FLOOR_MARGIN;
	if (!(bar > ec->active_bar))
		return ec->active_bar;
	return bar < bar_max ? bar : bar_max;
}

/*
 * The verdict of the LEC_TALKER_RATIO comment on this frame. window_prev still
 * holds the window maximum of the frame before. Each comparison is written so
 * that a value that is not a number takes the false arm, the rule of
 * step_scale_get.
 */
static bool talker_get(const struct udsp_ec *ec, const struct lec_tick *t)
{
	float guard = ec->window_prev > t->window_power ? ec->window_prev :
							  t->window_power;

	if (!t->window_active)
		return false;
	return t->mic_power > guard * ec->talker_ratio;
}

/*
 * The peak tap comes from the same pass, because this is the only pass over
 * the filter and it runs only after a write. The compare reads the
 * coefficient and not its square: a second use of the product stops the
 * compiler from fusing the sum, and the energy would then change on a target
 * that fuses. A filter whose energy reads 0 reports tap 0, so the two values
 * never disagree about an empty filter.
 */
static float coef_energy_get(const struct udsp_ec *ec, unsigned *peak)
{
	float energy = 0.0f;
	float loudest = 0.0f;
	unsigned best = 0;
	unsigned k;

	for (k = 0; k < ec->taps; k++) {
		energy += ec->coef[k] * ec->coef[k];
		if (fabsf(ec->coef[k]) > loudest) {
			loudest = fabsf(ec->coef[k]);
			best = k;
		}
	}
	if (!(energy > 0.0f))
		best = 0;
	*peak = best;
	return energy;
}

/*
 * The energy of the filter as it stands, recomputed only after a write to
 * the coefficients. The same sum over the same coefficients is the same
 * float, so a cached reading is the reading the sum would give; what the
 * cache removes is one pass over the taps on every frame that held.
 */
static float coef_energy_track(struct udsp_ec *ec)
{
	if (ec->coef_dirty) {
		ec->coef_energy = coef_energy_get(ec, &ec->coef_peak_tap);
		ec->coef_dirty = false;
	}
	return ec->coef_energy;
}

/* For a writer that zeroes the filter: the telemetry of this frame must read
 * it empty, and the dirty flag would leave the old values until the next
 * pass. */
static void coef_energy_clear(struct udsp_ec *ec)
{
	ec->coef_energy = 0.0f;
	ec->coef_peak_tap = 0;
	ec->coef_dirty = false;
}

/*
 * The live residual over the live microphone, the quantity a trial is judged
 * by. Total in the sense of step_scale_get: a microphone or a residual this
 * code cannot read falls on 1.0f, the ratio of a filter that removes nothing,
 * so an unreadable "before" cannot accept a trial and an unreadable "after"
 * cannot keep one.
 */
static float probe_ratio_get(const struct udsp_ec *ec)
{
	float mic = ec->live_mic_power;
	float res = ec->live_res_power;

	if (!(mic > 0.0f) || !(res >= 0.0f) || !(res < INFINITY))
		return 1.0f;
	return res / mic;
}

/* The three runs that a trial start and a trial end zero: the latch run,
 * the path-change run and the stale run. */
static void runs_clear(struct udsp_ec *ec)
{
	ec->dt_run = 0;
	ec->path_run = 0;
	ec->stale_run = 0;
}

/* The ERLE tracker as a pair of powers at or over LEC_ERLE_FLOOR and the
 * ratio in dB that the detector and the processor read. */
static void erle_set(struct udsp_ec *ec, float mic_avg, float res_avg)
{
	if (mic_avg < LEC_ERLE_FLOOR)
		mic_avg = LEC_ERLE_FLOOR;
	if (res_avg < LEC_ERLE_FLOOR)
		res_avg = LEC_ERLE_FLOOR;
	ec->belief.mic_power_avg = mic_avg;
	ec->belief.res_power_avg = res_avg;
	ec->belief.erle_db = 10.0f * log10f(mic_avg / res_avg);
}

/* ec_run copies the taps after the rollback of the start frame, so the
 * snapshot of every arm is the filter that the start frame ends with. own_arm
 * is the LEC_ARM_* bit of the arm that starts the trial, 0 for the latch arm;
 * a refusal stands it down with every arm that is standing down now. */
static void probe_start(struct udsp_ec *ec, unsigned own_arm)
{
	ec->trial_owed = (ec->standdown ? ec->standdown_arms : 0u) | own_arm;
	ec->probe_state = ec->belief;
	ec->probe_ratio_before = probe_ratio_get(ec);
	ec->probe_frames = LEC_PROBE_FRAMES;
	runs_clear(ec);
	ec->probes++;
}

/*
 * Undo the trial whole and end it: the filter, the trackers and the detector
 * go back to the frame the trial started on. Three callers reach it, the
 * trial that ends without the improvement it promised, the divergence
 * watchdog that stops one early, and the talker frame that ends one before
 * the frame adapts (the LEC_TALKER_RATIO comment); all need the same thing
 * undone. end is the way out that the trace reads.
 */
static void probe_reject(struct udsp_ec *ec, enum udsp_ec_nlms_trial_end end)
{
	unsigned trust_run;

	ec->reading.trial_end = (uint8_t)end;
	ec->probe_frames = 0;
	memcpy(ec->coef, ec->probe, sizeof(*ec->coef) * ec->taps);
	ec->coef_dirty = true;
	/* The trust count keeps the smaller of the two (struct lec_belief). */
	trust_run = ec->belief.trust_run;
	ec->belief = ec->probe_state;
	if (trust_run < ec->belief.trust_run)
		ec->belief.trust_run = trust_run;
	/*
	 * Zeroed, not left to the clear-frame decay: dt_run kept climbing
	 * through the trial, and the talker who just failed it is still on
	 * the line. The remainder would re-arm the next trial in a fraction
	 * of the latch period and burn one adapted second on the same speech
	 * over and over.
	 */
	runs_clear(ec);
	if (ec->trial_owed) {
		ec->standdown = LEC_TRIAL_STANDDOWN_FRAMES;
		ec->standdown_arms = ec->trial_owed;
	}
	/* The frame-end telemetry has to describe the filter the line ends
	 * the frame with, and this call is the last thing that changes it. */
	coef_energy_track(ec);
}

static void probe_finish(struct udsp_ec *ec)
{
	if (probe_ratio_get(ec) > ec->probe_ratio_before * LEC_PROBE_IMPROVE) {
		probe_reject(ec, UDSP_EC_NLMS_TRIAL_END_REJECTED);
		return;
	}
	ec->reading.trial_end = UDSP_EC_NLMS_TRIAL_END_KEPT;
	/*
	 * The trial stands, so the hold that produced it was wrong about this
	 * line. The tracker is reseeded from what was just measured rather
	 * than from the values it froze before the hold, otherwise the
	 * detector would keep thresholding against a filter it no longer has.
	 */
	erle_set(ec, ec->live_mic_power, ec->live_res_power);
	ec->belief.dt_hangover = 0;
	runs_clear(ec);
}

/* The verdict of the detector stands for as long as its hangover runs. */
static bool double_talk_get(const struct udsp_ec *ec)
{
	return ec->belief.dt_hangover != 0u;
}

/*
 * True when the canceller has been adding energy to the line for a run of
 * active frames while the detector holds it: the LEC_PATH_CHANGE_RATIO comment
 * above carries the measurements. The live powers are the ones the probe
 * judges its own trial by, measured every frame, hold or no hold, so the
 * verdict is available exactly when the filter stands still and nothing else
 * moves. A frame whose own reference is silent neither arms nor disarms it,
 * the rule the trial's budget follows: on silence the estimate goes to zero,
 * the residual becomes the microphone, and the ratio means nothing.
 */
static bool path_change_get(struct udsp_ec *ec, bool active)
{
	if (!active)
		return false;
	if (ec->standdown && (ec->standdown_arms & LEC_ARM_PATH)) {
		ec->path_run = 0;
		return false;
	}
	if (!double_talk_get(ec)) {
		ec->path_run = 0;
		return false;
	}
	if (ec->live_res_power < ec->live_mic_power * LEC_PATH_CHANGE_RATIO) {
		ec->path_run = 0;
		return false;
	}
	ec->path_run++;
	return ec->path_run >= LEC_PATH_CHANGE_FRAMES;
}

/*
 * True when the stale run of the LEC_GEIGEL_STALE_FRAMES comment is due. Like
 * the latch arm, it waits for an active frame. A stand-down that holds the
 * stale arm (LEC_TRIAL_STANDDOWN_FRAMES: a refused stale trial, or a refused
 * trial that carried one) holds it, and the run keeps its count through the
 * stand-down. A signal that holds the run up therefore costs one trial per
 * stand-down plus trial, a fixed cadence in active frames.
 */
static bool stale_due_get(const struct udsp_ec *ec, bool active)
{
	if (!active)
		return false;
	if (ec->standdown && (ec->standdown_arms & LEC_ARM_STALE))
		return false;
	return ec->stale_run >= LEC_GEIGEL_STALE_FRAMES;
}

/*
 * The trial arms of the detector: the latch run, the path-change verdict and
 * the stale run. None is re-armed mid-trial: the countdown spends active
 * frames only, so a sparse far end stretches the trial past LATCH_MAX plain
 * frames, and a second probe_start would replace the snapshot with the
 * half-adapted trial coefficients that probe_finish must be able to undo.
 */
static void probe_arm(struct udsp_ec *ec, bool active)
{
	bool path_change;
	bool stale;

	if (ec->probe_frames)
		return;
	path_change = path_change_get(ec, active);
	stale = stale_due_get(ec, active);
	/* The latch arm waits for an active frame (the LEC_PROBE_FRAMES
	 * comment): a run that comes due in a far-end pause starts on the first
	 * frame that carries a reference again. */
	if (path_change || (active && ec->dt_run >= LEC_GEIGEL_LATCH_MAX)) {
		probe_start(ec, path_change ? LEC_ARM_PATH : 0u);
		ec->reading.trial_arm = path_change ?
					UDSP_EC_NLMS_TRIAL_ARM_PATH :
					UDSP_EC_NLMS_TRIAL_ARM_LATCH;
	} else if (stale) {
		probe_start(ec, LEC_ARM_STALE);
		ec->reading.trial_arm = UDSP_EC_NLMS_TRIAL_ARM_STALE;
	}
	/* Spent after the arms read it: a stand-down at 1 on the frame a
	 * trial starts is still carried (the LEC_TRIAL_STANDDOWN_FRAMES
	 * comment). */
	if (active && ec->standdown)
		ec->standdown--;
}

/* The energy rule of the LEC_GEIGEL_TRUST_COEF_RATIO comment. coef_energy is
 * the energy of the filter whose estimate this frame judges, before the update
 * of the frame. The read costs no pass over the taps. */
static bool trust_rule_get(const struct udsp_ec *ec)
{
	return ec->coef_energy * LEC_GEIGEL_TRUST_COEF_RATIO >=
	       ec->belief.echo_gain;
}

static void trust_restart(struct udsp_ec *ec)
{
	ec->belief.trust_run = 0;
	ec->belief.trust_held = false;
}

/*
 * The trust of the detector on this frame, by the three rules of the
 * LEC_GEIGEL_GAIN_MIN comment. A turn of the energy rule restarts the count
 * before the trust reads it. Reason: the frame on which the rule turns can be
 * the first far-end frame that the filter of a tone does not explain. The
 * count of the frame moves after the detector (erle_update), so the trust
 * reads the count of the frames before it.
 */
static bool trust_update(struct udsp_ec *ec)
{
	bool rule = trust_rule_get(ec);

	if (rule && !ec->belief.trust_rule)
		trust_restart(ec);
	ec->belief.trust_rule = rule;
	if (!(ec->belief.erle_db >= LEC_GEIGEL_TRUST_DB) || !rule ||
	    ec->belief.trust_run < LEC_GEIGEL_TRUST_FRAMES)
		return false;
	ec->belief.trust_held = true;
	return true;
}

static unsigned run_decay(unsigned run)
{
	return run > LEC_GEIGEL_RUN_DECAY ? run - LEC_GEIGEL_RUN_DECAY : 0u;
}

/* The window stands under the bar of the frame and at or over the fixed bar,
 * so the far-end floor alone holds it inactive. */
static bool background_get(const struct udsp_ec *ec, const struct lec_tick *t)
{
	return !t->window_active && t->window_power >= ec->active_bar;
}

/*
 * Called on frames whose own reference is active, never on the DTX-silent
 * frames inside a still-active tail window: there the mic carries no echo
 * while the window maximum is still the old spurt, so the ratio under-reads
 * and the minimum walks down (2.67 dB in the bench, pinned at the floor
 * through DTX-heavy live speech).
 */
static void echo_gain_update(struct udsp_ec *ec, const struct lec_tick *t)
{
	float ratio;

	ec->belief.gain_mic_avg += (t->mic_power - ec->belief.gain_mic_avg) *
			    LEC_GAIN_AVG_ALPHA;
	ec->belief.gain_win_avg += (t->window_power - ec->belief.gain_win_avg) *
			    LEC_GAIN_AVG_ALPHA;
	if (ec->belief.gain_warmup < LEC_GAIN_WARMUP_FRAMES) {
		ec->belief.gain_warmup++;
		return;
	}
	ratio = ec->belief.gain_mic_avg / ec->belief.gain_win_avg;
	if (ratio < ec->belief.echo_gain)
		ec->belief.echo_gain = ratio;
	else if (!double_talk_get(ec))
		ec->belief.echo_gain *= LEC_GEIGEL_GAIN_RISE;
	if (ec->belief.echo_gain < ec->gain_floor)
		ec->belief.echo_gain = ec->gain_floor;
}

/* The hangover that a verdict sets, by the conditions of the
 * LEC_GEIGEL_HANGOVER_SHORT comment. A verdict inside a running hangover takes
 * the long one, so no verdict shortens a hangover. */
static unsigned hangover_get(const struct udsp_ec *ec, float mic_power,
			     float bar)
{
	if (ec->belief.dt_hangover)
		return LEC_GEIGEL_HANGOVER;
	if (ec->dt_gap < LEC_GEIGEL_REPEAT_FRAMES)
		return LEC_GEIGEL_HANGOVER;
	if (mic_power >= bar * LEC_GEIGEL_MARGINAL_RATIO)
		return LEC_GEIGEL_HANGOVER;
	if (mic_power >= ec->noise_power * LEC_GEIGEL_SOFT_RATIO)
		return LEC_GEIGEL_HANGOVER;
	return LEC_GEIGEL_HANGOVER_SHORT;
}

static void doubletalk_update(struct udsp_ec *ec, struct lec_tick *t)
{
	float mic_power = t->mic_power;
	float window_power = t->window_power;
	float classic;
	float classic_bar;
	float misalign;
	float expected = t->estimate_power;
	float headroom;
	float bar;
	bool trusted;
	bool stale_clear;

	/* The trace reads the trackers as the detector read them. */
	t->noise_power = ec->noise_power;
	t->erle_db = ec->belief.erle_db;
	t->hold_left = ec->belief.dt_hangover;
	t->dt_fired = false;
	/*
	 * The NLP's echo-likeness verdict compares against the per-frame echo
	 * estimate, and it does so before the classic merge below. The classic
	 * arm is a window-maximum bound, loose by the window-to-frame ratio,
	 * so a near talker under it never reads as double talk and would be
	 * handed to the comfort noise whole (measured live: garbled near
	 * speech). A frame carrying more than the margin over the predicted
	 * echo is not an echo frame and the NLP must not replace it.
	 *
	 * The predicted echo carries the tracked near-end floor, the same term
	 * the expectation below takes: a pure-echo frame is echo plus floor.
	 * Without the term, no frame of a live line whose floor sits within a
	 * few decibels of its echo is echo-like, so the processor stays out
	 * of a residual that is mostly the floor.
	 *
	 * The term takes the headroom of the detector on an active window. On
	 * a quiet frame the flag decides only whether the frame keeps the
	 * engage run, and the term takes the larger pause headroom, so an
	 * excursion of the floor in a pause keeps the run
	 * (LEC_GEIGEL_PAUSE_HEADROOM).
	 */
	headroom = t->window_active ? LEC_GEIGEL_FLOOR_HEADROOM :
				      LEC_GEIGEL_PAUSE_HEADROOM;
	t->echo_like = mic_power <=
		       (expected + ec->noise_power * headroom) *
		       LEC_GEIGEL_MARGIN;
	classic = echo_bound_get(ec, window_power);
	/*
	 * What a pure-echo frame carries is the estimate plus the residual the
	 * filter is known to leave: the two are orthogonal after an update, so
	 * their powers add. The estimate alone is therefore the expectation of
	 * a filter that has already converged, and a filter that has not
	 * under-reads its own microphone by exactly the misalignment the ERLE
	 * tracker is reporting. Reading it without that term costs a false
	 * detection and its hangover during the bootstrap: the
	 * frame that fires is a 1.5 dB excursion over an estimate carrying
	 * 8.3 dB of ERLE (bench, composite hybrid at the 64 ms tail, one seed
	 * in forty at 8 kHz and eight in forty on the late hybrid at 16 kHz).
	 * The term is the tracker's own ratio, so it vanishes as the filter
	 * converges (0.07 dB at the 18 dB trust line) and no constant sets it.
	 * It is capped at the microphone: a residual louder than the input is
	 * not a misalignment the model above describes, and the path-change
	 * escape is what answers that state.
	 */
	misalign = ec->belief.res_power_avg < ec->belief.mic_power_avg ?
		   ec->belief.res_power_avg / ec->belief.mic_power_avg : 1.0f;
	expected *= 1.0f + misalign;
	classic_bar = (classic + ec->noise_power * LEC_GEIGEL_FLOOR_HEADROOM) *
		      LEC_GEIGEL_MARGIN;
	trusted = trust_update(ec);
	/* The clears of the stale run and of the latch run come before
	 * probe_arm reads the runs, for the reasons of the
	 * LEC_GEIGEL_STALE_FRAMES and LEC_TALKER_RATIO comments. */
	stale_clear = !trusted ||
		      (t->window_active && mic_power >= classic_bar);
	if (stale_clear)
		ec->stale_run = 0;
	if (t->talker)
		ec->dt_run = 0;
	/*
	 * No arm on a frame that started inside a trial. While a trial runs
	 * the arms return at once, and on the frame a talker ends one they
	 * would spend a frame of the stand-down that the end just armed;
	 * those start to spend on the next frame.
	 */
	if (!t->trial_open)
		probe_arm(ec, t->active);
	t->trusted = trusted;
	t->classic_arm = !trusted && classic > expected;
	if (t->classic_arm)
		expected = classic;
	/*
	 * What a pure-echo frame carries is the echo plus the line's own near
	 * end, and both arms above model the echo alone. On the 12 dB bench
	 * hybrids that omission is invisible, because the echo stands 30 dB
	 * over any floor; on the field lines the two meet. Measured on the
	 * weak-hybrid bench rows at an echo 11 dB over the floor: of 162
	 * verdicts raised with the room silent, 137 had the microphone within
	 * 0.8 dB of the bar, and the microphone on those frames was the floor
	 * (29.2 dB) with the echo 5 dB under it. The detector was right that
	 * the microphone carried more than the echo, and wrong that a talker
	 * was the only thing that could.
	 *
	 * The floor is the tracked one, measured in quiet windows by
	 * noise_floor_update. Near-end speech in a far-end pause does not
	 * raise it, because a louder reading moves the floor only as part of
	 * a long stationary run. A talker who speaks on into the next far-end
	 * word therefore meets a bar at the true floor. A far end that never
	 * pauses has no quiet window, and the seed of noise_seed_update
	 * measures the floor there. A frame of the far-end noise echo plus
	 * the floor then stays under the bar. The term vanishes where it is
	 * not needed. A line whose echo stands 20 dB over its floor moves the
	 * bar by 0.09 dB with the headroom in. The headroom is the floor's
	 * excursion over its tracked level (LEC_GEIGEL_FLOOR_HEADROOM).
	 */
	expected += ec->noise_power * LEC_GEIGEL_FLOOR_HEADROOM;
	bar = expected * LEC_GEIGEL_MARGIN;
	t->bar = bar;
	if (t->window_active && mic_power >= bar) {
		t->dt_fired = true;
		ec->belief.dt_hangover = hangover_get(ec, mic_power, bar);
		ec->dt_gap = 0;
		ec->dt_run++;
		if (!stale_clear)
			ec->stale_run++;
		return;
	}
	if (ec->dt_gap < LEC_GEIGEL_REPEAT_FRAMES)
		ec->dt_gap++;
	if (ec->belief.dt_hangover)
		ec->belief.dt_hangover--;
	/*
	 * A frame of a far-end background under the raised bar leaves the latch
	 * run as it is. The decay exists so that the run survives the DTX gaps
	 * of far speech, and a background frame is no such gap: the far end is
	 * still there. The quiet window forms no verdict, so the run does not
	 * grow either. With the decay there, the pauses between far words pay
	 * down the run of an echo path that got louder, and no latch trial
	 * teaches the filter that path.
	 */
	if (t->background)
		return;
	if (!double_talk_get(ec)) {
		ec->dt_run = run_decay(ec->dt_run);
		if (t->window_active)
			ec->stale_run = run_decay(ec->stale_run);
		return;
	}
	ec->dt_run++;
}

/* One clear active frame into the ERLE tracker. The trust count is the
 * updates of the tracker since its last restart (the LEC_GEIGEL_TRUST_FRAMES
 * comment), so it moves here. */
static void erle_update(struct udsp_ec *ec, float mic_power, float res_power)
{
	float mic_avg = ec->belief.mic_power_avg;
	float res_avg = ec->belief.res_power_avg;

	mic_avg += (mic_power - mic_avg) * LEC_ERLE_ALPHA;
	res_avg += (res_power - res_avg) * LEC_ERLE_ALPHA;
	erle_set(ec, mic_avg, res_avg);
	if (ec->belief.trust_run < UINT16_MAX)
		ec->belief.trust_run++;
}

/*
 * The floor as a power, in the units of udsp_ec_nlms_frame_power_get, so the
 * detector can add it to an echo expectation. dsp/cn.c states the level in
 * dBov against a full-scale sine of 32767, and a larger number is quieter.
 * It is derived here, on the reference-silent frames the level is measured
 * on, instead of in the detector: the conversion then costs nothing on the
 * frames that carry speech.
 */
static void noise_power_set(struct udsp_ec *ec)
{
	/*
	 * 127 is the "quieter than one LSB" sentinel udsp_cn_level_get
	 * returns, which says the line has no measurable floor rather than a
	 * very quiet one. A term derived from it would be an invention, and
	 * every scenario whose near end is digital silence would carry it.
	 */
	if (ec->noise_dbov >= 127.0f) {
		ec->noise_power = 0.0f;
		return;
	}
	ec->noise_power = 32767.0f * 32767.0f *
			  powf(10.0f, ec->noise_dbov * -0.1f);
}

/* The one writer of the floor: its level, whether the level is a measured
 * floor, and the power the detector adds. 127 with valid false is no floor. */
static void noise_level_set(struct udsp_ec *ec, float dbov, bool valid)
{
	ec->noise_dbov = dbov;
	ec->noise_valid = valid;
	noise_power_set(ec);
}

/*
 * An active frame can only falsify a measured floor. Its residual carries
 * the echo the filter has not removed on top of the near end, so it bounds
 * that floor from above and never moves it: a run of residuals under the
 * tracked floor proves the floor stale, and the tracker drops it rather than
 * slewing towards it. Slewing is degenerate: the floor then
 * follows a converging residual down frame by frame, the two stay equal, and
 * the step control below reads no echo left to learn (measured, the reset case
 * reconverged to 5 dB instead of 59).
 *
 * The floor needs that way out because the first measurable reading of a
 * quiet window is taken whole (noise_quiet_update). A near-end talkspurt in a
 * far-end pause can supply that reading and set the floor to the level of the
 * speech. The rise test does not protect against that, because it guards a
 * floor the tracker already holds. The seed of noise_seed_update can also
 * read a quiet talker under the bar. A poisoned floor is dropped here. The
 * next pause measures it again, or the seed under a far end that never
 * pauses.
 *
 * Two things together make it proof rather than jitter. LEC_NOISE_STALE_DB
 * is the size: the level is quantised to whole dB by udsp_cn_level_get and
 * the tracker is smoothed, so a gap of one or two dB is measurement noise
 * and a gap of three is not. LEC_NOISE_STALE_FRAMES is the length, and it is
 * the one that answers a near end that simply went quiet for a frame.
 */
static void noise_stale_update(struct udsp_ec *ec, float level)
{
	if (!ec->noise_valid || level <= ec->noise_dbov + LEC_NOISE_STALE_DB) {
		ec->noise_stale_run = 0;
		return;
	}
	if (++ec->noise_stale_run < LEC_NOISE_STALE_FRAMES)
		return;
	ec->noise_stale_run = 0;
	noise_level_set(ec, 127.0f, false);
}

static void noise_run_clear(struct noise_run *run)
{
	memset(run, 0, sizeof(*run));
}

/*
 * Adds one reading to the run. A run that reaches full readings is copied to
 * *done and cleared. The call then returns true when the spread of the run is
 * within LEC_NOISE_SPREAD_DB.
 */
static bool noise_run_add(struct noise_run *run, int level, unsigned full,
			  struct noise_run *done)
{
	if (!run->frames || level > run->quiet)
		run->quiet = level;
	if (!run->frames || level < run->loud)
		run->loud = level;
	run->sum += (float)level;
	if (++run->frames < full)
		return false;
	*done = *run;
	noise_run_clear(run);
	return (float)(done->quiet - done->loud) <= LEC_NOISE_SPREAD_DB;
}

static void noise_quiet_update(struct udsp_ec *ec, int level)
{
	float gap = ec->noise_dbov - (float)level;
	struct noise_run done;
	float dbov;

	/*
	 * Digital silence is not a reading of a floor. If it took the first
	 * reading, the first real floor would reach the tracker only through
	 * a full rise run.
	 */
	if (level >= 127 && !ec->noise_valid)
		return;
	/*
	 * The first measurable reading is taken whole, the dsp/vad.c idiom.
	 * The rise test keeps near-end speech out of a floor the tracker
	 * holds. It does not exist to climb from the silence the instance
	 * starts at.
	 */
	if (!ec->noise_valid) {
		dbov = (float)level;
	} else if (gap < 0.0f) {
		dbov = ec->noise_dbov - gap * LEC_NOISE_ALPHA_DOWN;
	} else if (gap <= LEC_NOISE_RISE_DB) {
		dbov = ec->noise_dbov - gap * LEC_NOISE_ALPHA_UP;
	} else if (noise_run_add(&ec->noise_rise, level, LEC_NOISE_RISE_FRAMES,
				 &done)) {
		dbov = done.sum / (float)done.frames;
	} else {
		return;
	}
	noise_run_clear(&ec->noise_rise);
	noise_level_set(ec, dbov, true);
}

/*
 * The gates of the LEC_NOISE_SEED_ERLE_DB comment. Each comparison is written
 * so that a value that is not a number takes the false arm, the rule of
 * step_scale_get. The frame record holds the microphone and the bar that the
 * detector read on this frame.
 */
static bool noise_seed_qualified(const struct udsp_ec *ec,
				 const struct lec_tick *t)
{
	float leftover;

	if (ec->noise_valid || ec->probe_frames || t->level >= 127)
		return false;
	if (!(ec->belief.erle_db >= LEC_NOISE_SEED_ERLE_DB))
		return false;
	leftover = t->estimate_power * powf(10.0f, ec->belief.erle_db * -0.1f);
	if (!(t->res_power >= leftover * LEC_NOISE_ECHO_MARGIN))
		return false;
	if (!(t->mic_power < t->bar * LEC_GEIGEL_SOFT_RATIO))
		return false;
	return t->res_power <= ec->belief.res_power_avg * LEC_GEIGEL_SOFT_RATIO;
}

/*
 * A seed is a first reading of the floor. It clears a rise run that the
 * stale rule left behind, as the first reading of a quiet window does.
 */
static void noise_seed_update(struct udsp_ec *ec, const struct lec_tick *t)
{
	struct noise_run done;

	if (!noise_seed_qualified(ec, t)) {
		noise_run_clear(&ec->noise_seed);
		return;
	}
	if (!noise_run_add(&ec->noise_seed, t->level, LEC_NOISE_SEED_FRAMES,
			   &done))
		return;
	noise_run_clear(&ec->noise_rise);
	noise_level_set(ec, (float)done.quiet - LEC_NOISE_SEED_BIAS_DB, true);
}

/*
 * One reading of the floor per frame, in the residual of the frame. An
 * active frame can drop a measured floor and can seed an unmeasured one. A
 * quiet-window frame measures the floor. The frames between the two, with a
 * silent reference inside an active window, only break the seed run.
 * The record holds the residual in the dBov units of udsp_cn_level_get and
 * the mean squares of the frame.
 */
static void noise_floor_update(struct udsp_ec *ec, const struct lec_tick *t)
{
	if (ec->noise_rise.frames &&
	    ++ec->noise_rise.age > LEC_NOISE_RISE_AGE)
		noise_run_clear(&ec->noise_rise);
	if (t->active) {
		noise_stale_update(ec, (float)t->level);
		noise_seed_update(ec, t);
		return;
	}
	noise_run_clear(&ec->noise_seed);
	ec->noise_stale_run = 0;
	/*
	 * The frames after a talkspurt still carry the echo of the tail window
	 * through a filter that has not converged, and the seed is taken
	 * whole: measured on the residual of the first such frame, the floor
	 * reads the echo plus the room, and the stale rule then drops it four
	 * frames into the next talkspurt on a live line. The floor is a
	 * property of the near end alone, so it is read once the whole window
	 * is quiet.
	 */
	if (t->window_active)
		return;
	noise_quiet_update(ec, t->level);
}

static bool coef_diverged(const struct udsp_ec *ec, float energy)
{
	return !(isfinite(energy) && energy <= ec->coef_energy_max);
}

/*
 * A filter that blew up inside a trial is undone rather than zeroed (a field
 * trace: diverge_resets 6 to 14 in one call, every one of them on a real
 * double-talk episode, each one audible on the far end).
 *
 * The trial adapts against the detector's verdict and takes no same-frame
 * rollback. The echo of a hybrid that amplifies, which no passive path
 * returns, therefore drives the coefficients through the bound. A near talker
 * more than 12 dB over the guard window ends the trial before it adapts
 * (LEC_TALKER_RATIO). A quieter talker holds the coefficient energy under the
 * bound. Measured with a white talker 11 dB over a white far end at -25 dBm0
 * on the weak hybrid (test/bench/lec_paths.h, path_weak_init): 5 trials and
 * no reset at each rate, and a largest coefficient energy of 2.07 at 8 kHz
 * and 2.00 at 16 kHz.
 *
 * Zeroing the coefficients there is the worst answer available: the line
 * loses the converged filter it had, the trial keeps running on a dead one
 * for the rest of its budget, and the hybrid's raw echo goes to the far end
 * for up to LEC_PROBE_FRAMES of active line. The snapshot the trial took
 * is a filter the watchdog passed on every frame before the trial, so the
 * reject path restores a filter known to be good and ends the trial in one
 * step.
 *
 * The verdict is taken here, before the trial's own. The countdown expires on
 * an adapted frame like any other, and the trial's improvement test reads the
 * smoothed live residual, which lags a divergent frame by a tenth. Judged
 * there, a diverged filter can be declared an improvement and kept, and the
 * plain watchdog behind it then finds a trial that is already over and zeroes
 * the line's filter after all. Gating the countdown on this call removes
 * both. The pass over the coefficients that it costs is the one the watchdog
 * runs on the frame anyway: coef_energy_track caches the energy until the
 * next write, so the two calls share it.
 *
 * The counter counts this as a reset, because it counts what the field reads
 * it for: the filter blew up.
 */
static bool probe_diverged(struct udsp_ec *ec)
{
	if (!coef_diverged(ec, coef_energy_track(ec)))
		return false;
	ec->diverge_resets++;
	probe_reject(ec, UDSP_EC_NLMS_TRIAL_END_DIVERGED);
	return true;
}

/*
 * Outside a trial there is no snapshot to go back to and the zero-reset
 * stands: a diverged filter with nothing to restore is worse than no filter.
 *
 * The live powers are re-seeded from the microphone, which the trial's own
 * snapshot deliberately avoids (struct lec_belief states why). The two cases
 * are opposites. There the filter is put back, so a power measured through
 * it stays true of the line; here the filter is destroyed, so a residual
 * measured through it is a measurement of a filter that no longer exists. A
 * zeroed filter passes the microphone through, so the microphone is what the
 * residual will be. mic_power is a float sum of int16 squares and is
 * therefore always finite, which is what makes this the point where any
 * non-finite value in either tracker cannot survive.
 */
static void divergence_check(struct udsp_ec *ec, float mic_power)
{
	if (!coef_diverged(ec, coef_energy_track(ec)))
		return;
	ec->diverge_resets++;
	memset(ec->coef, 0, sizeof(*ec->coef) * ec->taps);
	coef_energy_clear(ec);
	ec->belief.mic_power_avg = LEC_ERLE_FLOOR;
	ec->belief.res_power_avg = LEC_ERLE_FLOOR;
	ec->belief.erle_db = 0.0f;
	ec->live_mic_power = mic_power;
	ec->live_res_power = mic_power;
}

/*
 * The NLP replaces the residual only when there is an echo to replace and the
 * detector has just confirmed that nothing else is on the line. Without the
 * reference term a converged instance suppresses the caller the moment the
 * far end stops talking: the detector cannot flag near-end speech that no
 * reference explains, the tracker holds its last converged value, and the
 * comfort noise takes the place of speech. The gate therefore needs a
 * confirmed near-end absence. The reference is taken over the tail window, not
 * over this frame, so the echo of a talkspurt that just ended is still
 * covered.
 */
static enum nlp_verdict nlp_gate_get(const struct udsp_ec *ec,
				     const struct lec_tick *t)
{
	float loss_db;

	if (!ec->nlp_on)
		return NLP_VERDICT_OFF;
	if (!t->window_active)
		return NLP_VERDICT_QUIET;
	if (double_talk_get(ec))
		return NLP_VERDICT_DT;
	if (!t->echo_like)
		return NLP_VERDICT_UNLIKE;
	if (ec->belief.erle_db >= LEC_NLP_ERLE_DB)
		return NLP_VERDICT_PASS;
	if (ec->belief.erle_db < LEC_NLP_ERLE_MIN_DB)
		return NLP_VERDICT_ERLE;
	/*
	 * The combined-loss arm (the LEC_NLP_LOSS_DB comment above): what the
	 * far listener hears is the hybrid loss plus the ERLE. The loss term
	 * takes the headroom, the same upper bound the detector consumes,
	 * because the raw minimum statistic overstates the loss.
	 */
	loss_db = ec->belief.erle_db -
		  10.0f * log10f(ec->belief.echo_gain *
				 LEC_GEIGEL_GAIN_HEADROOM);
	loss_db += ec->ref_loss_db;
	return loss_db >= LEC_NLP_LOSS_DB ? NLP_VERDICT_PASS : NLP_VERDICT_ERLE;
}

/*
 * One count per frame: nlp_active says only whether the processor acted on
 * the last frame, and the counts say why it stood out of the others. An
 * eligible frame counts as a wait until the engage run completes. A frame
 * with the processor switched off counts nowhere, because no condition
 * refused it.
 */
static void nlp_count(struct udsp_ec *ec, const struct lec_tick *t)
{
	switch (t->nlp_verdict) {
	case NLP_VERDICT_OFF:
		return;
	case NLP_VERDICT_QUIET:
		ec->nlp_quiet_frames++;
		return;
	case NLP_VERDICT_DT:
		ec->nlp_dt_frames++;
		if (t->dt_fired)
			ec->nlp_dt_fired_frames++;
		return;
	case NLP_VERDICT_UNLIKE:
		ec->nlp_unlike_frames++;
		return;
	case NLP_VERDICT_ERLE:
		ec->nlp_erle_frames++;
		return;
	case NLP_VERDICT_PASS:
		break;
	}
	if (ec->nlp_active)
		ec->nlp_active_frames++;
	else
		ec->nlp_wait_frames++;
}

static void nlp_apply(struct udsp_ec *ec, struct lec_tick *t, int16_t *out)
{
	enum nlp_verdict verdict = nlp_gate_get(ec, t);
	int16_t noise[UDSP_FRAME_MAX];
	float target;
	float from = ec->nlp_active ? 1.0f : 0.0f;
	int level = t->level;
	bool run_kept;
	unsigned i;

	t->nlp_verdict = verdict;
	/*
	 * Engage hysteresis (LEC_NLP_ON_FRAMES): only a run of eligible
	 * frames engages, one ineligible frame releases at once. Without it,
	 * the gate flaps once a second on a marginal line, and the 10 ms on/off
	 * alternation garbles the near path. That flap is a refused frame
	 * inside far speech. A dt, unlike or erle verdict therefore cuts a
	 * complete run to LEC_NLP_DECAY_FRAMES and zeroes a run that is not
	 * complete. After a single refusal the processor acts again on the 4th
	 * pass frame, and refusals with 3 pass frames or fewer between them
	 * hold the gate off.
	 *
	 * A pause is no flap. When the microphone is echo-like, a quiet
	 * verdict leaves the run as it is, whatever the far end carries under
	 * the activity bar. After a pause whose every quiet frame kept a
	 * complete run, the processor acts on the first pass frame of the
	 * next far word. On a quiet frame, a microphone over the
	 * echo-likeness bar of doubletalk_update zeroes the run. That bar is
	 * the estimate plus the floor term, times the margin. A near signal
	 * under that bar keeps the run, and the next pass frame can replace
	 * that signal. On a line with no measured floor, the bar of a quiet
	 * frame is the estimate times the margin. Room noise in a pause of a
	 * silent far end stands over it, and digital silence never does. The
	 * processor acts only on a pass frame whose run is complete. It
	 * therefore releases on each quiet frame and acts again on the next
	 * pass frame, at most once per window length plus one frame.
	 */
	run_kept = verdict == NLP_VERDICT_QUIET && t->echo_like;
	if (verdict == NLP_VERDICT_PASS) {
		if (ec->nlp_run < LEC_NLP_ON_FRAMES)
			ec->nlp_run++;
	} else if (!run_kept) {
		ec->nlp_run = verdict != NLP_VERDICT_QUIET &&
			      verdict != NLP_VERDICT_OFF &&
			      ec->nlp_run >= LEC_NLP_ON_FRAMES ?
			      LEC_NLP_DECAY_FRAMES : 0;
	}
	target = verdict == NLP_VERDICT_PASS &&
		 ec->nlp_run >= LEC_NLP_ON_FRAMES ? 1.0f : 0.0f;
	ec->nlp_active = target > 0.0f;
	nlp_count(ec, t);
	if (target == 0.0f && from == 0.0f)
		return;
	/*
	 * Comfort noise never louder than the residual it replaces. The floor
	 * is measured in far-end pauses, or seeded on active frames under a
	 * far end that never pauses. It is stale when the near end went quiet
	 * since. Near-end speech can also supply the first measurable reading
	 * or a seed.
	 */
	if ((float)level < ec->noise_dbov)
		level = (int)lrintf(ec->noise_dbov);
	/*
	 * In the steady engaged state the noise replaces the frame whole: at
	 * a mix of exactly 1 the fade below returns each noise sample as it
	 * is, so the frame takes the noise directly and skips the loop.
	 */
	if (from == target) {
		udsp_cn_generate(&ec->cn, level, out, ec->frame);
		return;
	}
	udsp_cn_generate(&ec->cn, level, noise, ec->frame);
	/*
	 * One frame of cross-fade at a gate edge: switching between the
	 * residual and the comfort noise in one sample is an audible click.
	 */
	for (i = 0; i < ec->frame; i++) {
		float mix = from + (target - from) *
			((float)(i + 1u) / ec->frame);

		out[i] = udsp_sat16_round((1.0f - mix) * (float)out[i] +
					  mix * (float)noise[i]);
	}
}

/*
 * The rules of the udsp_ec_nlms_ref_loss_set comment in dsp/lec.h. The
 * arithmetic stays in float, and the loss of 0 takes powf(10, -0), which is
 * exactly 1.0f, so every field then equals its constant bit for bit.
 */
static void ref_loss_scale_set(struct udsp_ec *ec, unsigned half_db)
{
	float db = (float)half_db / 2.0f;
	float g2 = powf(10.0f, db * -0.1f);

	ec->ref_loss_half_db = half_db;
	ec->ref_loss_db = db;
	ec->active_bar = LEC_REF_ACTIVE_POWER * g2;
	ec->gain_min = LEC_GEIGEL_GAIN_MIN / g2;
	ec->gain_floor = LEC_GEIGEL_GAIN_FLOOR / g2;
	ec->talker_ratio = LEC_TALKER_RATIO / g2;
	ec->coef_energy_max = LEC_COEF_ENERGY_MAX / g2;
	ec->step_floor_scale = g2;
}

static struct udsp_ec *ec_create_rate(unsigned tail_ms, unsigned rate_hz)
{
	struct udsp_ec *ec;
	unsigned tail_max;
	unsigned taps;

	if (rate_hz != UDSP_SAMPLE_RATE && rate_hz != UDSP_SAMPLE_RATE_WB)
		return NULL;
	tail_max = rate_hz == UDSP_SAMPLE_RATE_WB ? LEC_TAIL_MS_WB_MAX :
						    LEC_TAIL_MS_MAX;
	/*
	 * 0 is the contract sentinel, not an out-of-range request: the stable
	 * udsp.h says so at udsp_chan_cfg.ec_tail_ms ("0 = default"). It maps
	 * to the default tail, and every other value out of range clamps.
	 */
	if (!tail_ms)
		tail_ms = rate_hz == UDSP_SAMPLE_RATE_WB ? LEC_TAIL_MS_WB_DEFAULT :
							   LEC_TAIL_MS_DEFAULT;
	if (tail_ms < LEC_TAIL_MS_MIN)
		tail_ms = LEC_TAIL_MS_MIN;
	if (tail_ms > tail_max)
		tail_ms = tail_max;
	taps = tail_ms * (rate_hz / 1000u);
	ec = calloc(1, sizeof(*ec) + sizeof(float) * mem_floats_get(taps));
	if (!ec)
		return NULL;
	ec->frame = rate_hz / 100u;
	ec->taps = taps;
	ec->tail_ms = tail_ms;
	ec->coef = ec->mem;
	ec->backup = ec->mem + taps;
	ec->probe = ec->mem + 2u * taps;
	ec->hist = ec->mem + 3u * taps;
	ec->power_floor = LEC_POWER_PER_TAP * (float)taps;
	ec->window_frames = (taps + ec->frame - 1u) / ec->frame;
	ref_loss_scale_set(ec, 0);
	/* The reset is the one definition of the adaptive start state. */
	udsp_ec_nlms_reset(ec);
	/* Silence until a far-end pause or a seed measures the real floor. */
	noise_level_set(ec, 127.0f, false);
	/* The analog line profile: the default is EC on with NLP. */
	ec->nlp_on = true;
	ec->zero_skip_on = true;
	ec->ref_floor_on = true;
	udsp_cn_init(&ec->cn, 0x1ec5eedu);
	return ec;
}

static struct udsp_ec *ec_create(unsigned tail_ms)
{
	return ec_create_rate(tail_ms, UDSP_SAMPLE_RATE);
}

static void ec_destroy(struct udsp_ec *ec)
{
	free(ec);
}

static uint16_t u16_sat(unsigned v)
{
	return v > UINT16_MAX ? UINT16_MAX : (uint16_t)v;
}

static void window_run_update(struct udsp_ec *ec, const struct lec_tick *t)
{
	if (!t->window_active)
		ec->window_run = 0;
	else if (ec->window_run < UINT16_MAX)
		ec->window_run++;
}

/*
 * The restart of the trust count on a far return after a pause (the
 * LEC_GEIGEL_TRUST_PAUSE_FRAMES comment). ec_run calls it once the window
 * state of the frame is final, after the end that a talker gives a trial,
 * and before the detector. The detector therefore judges the return frame
 * without the trust, because that frame can fire on the estimate of the tone.
 */
static void quiet_run_update(struct udsp_ec *ec, const struct lec_tick *t)
{
	if (!t->window_active) {
		if (ec->quiet_run < LEC_GEIGEL_TRUST_PAUSE_FRAMES)
			ec->quiet_run++;
		return;
	}
	if (ec->quiet_run >= LEC_GEIGEL_TRUST_PAUSE_FRAMES &&
	    !ec->belief.trust_held)
		trust_restart(ec);
	ec->quiet_run = 0;
}

/*
 * The trial fields of the frame reading, cleared before a talker can end a
 * trial. probe_arm, probe_reject and probe_finish write them directly,
 * because a trial starts or ends in the middle of the frame. A frame starts
 * a trial or ends one, never both: every end runs under the trial state of
 * the frame's entry, or of the talker end, and the arm runs only on a frame
 * with no trial at entry (doubletalk_update). One clear per frame therefore
 * serves both fields.
 */
static void reading_trial_clear(struct udsp_ec *ec)
{
	ec->reading.trial_arm = UDSP_EC_NLMS_TRIAL_ARM_NONE;
	ec->reading.trial_end = UDSP_EC_NLMS_TRIAL_END_NONE;
}

/* The frame reading from the record, at the end of the frame. The trial
 * fields stay as the frame wrote them. */
static void reading_fill(struct udsp_ec *ec, const struct lec_tick *t)
{
	ec->reading.frames = ec->frames;
	ec->reading.mic_power = t->mic_power;
	ec->reading.window_power = t->window_power;
	ec->reading.estimate_power = t->estimate_power;
	ec->reading.bar = t->bar;
	ec->reading.noise_power = t->noise_power;
	ec->reading.ref_power = t->ref_power;
	ec->reading.erle_db = t->erle_db;
	ec->reading.ref_bar = t->ref_bar;
	ec->reading.ref_bar_max = t->bar_max;
	ec->reading.window_run = u16_sat(ec->window_run);
	ec->reading.dt_run = u16_sat(ec->dt_run);
	ec->reading.stale_run = u16_sat(ec->stale_run);
	ec->reading.trust_run = u16_sat(ec->belief.trust_run);
	ec->reading.hold_left = (uint8_t)t->hold_left;
	ec->reading.nlp_verdict = (uint8_t)t->nlp_verdict;
	ec->reading.dt_fired = t->dt_fired;
	ec->reading.classic_arm = t->classic_arm;
	ec->reading.trusted = t->trusted;
	ec->reading.probing = t->trial_open;
	ec->reading.ref_quiet = t->window_active && !t->active;
	ec->reading.talker = t->talker;
	ec->reading.background = t->background;
}

static void ec_run(struct udsp_ec *ec, int16_t *mic, const int16_t *ref)
{
	struct lec_tick t;
	float step_floor;
	bool zero;
	bool adapt;

	memset(&t, 0, sizeof(t));
	t.ref_power = udsp_ec_nlms_frame_power_get(ref, ec->frame);
	t.mic_power = udsp_ec_nlms_frame_power_get(mic, ec->frame);
	t.bar_max = ref_bar_max_get(ec);
	t.trial_open = ec->probe_frames != 0u;
	/* Read before the frame pushes anything: the exact energy of the
	 * history window (hist_push) and of the frame, both zero only when
	 * every sample the tap loops would read is zero. */
	zero = ec->zero_skip_on && ec->hist_energy == 0.0 &&
	       t.ref_power == 0.0f;
	window_push(ec, t.ref_power);
	t.window_power = window_power_get(ec);
	t.ref_bar = ref_bar_get(ec, t.bar_max, t.trial_open);
	t.window_active = t.window_power >= t.ref_bar;
	t.talker = talker_get(ec, &t);
	ec->window_prev = t.window_power;
	reading_trial_clear(ec);
	/*
	 * A talker ends a running trial before the frame adapts, and probing
	 * follows that end, so the countdown does not run on the frame. The
	 * frame is held whatever the restored state says. probe_reject
	 * restores the hangover from the snapshot, and a trial that started
	 * after the hangover ran out restores none. Without the hold, the
	 * frame would then adapt on the talker at the normal step. The bar
	 * follows the end too: the frame runs with no trial. The end counts
	 * no divergence reset, because the filter did not leave its bound.
	 */
	t.held = t.talker && t.trial_open;
	if (t.held) {
		probe_reject(ec, UDSP_EC_NLMS_TRIAL_END_TALKER);
		t.ref_bar = ref_bar_get(ec, t.bar_max, false);
		t.window_active = t.window_power >= t.ref_bar;
	}
	window_run_update(ec, &t);
	quiet_run_update(ec, &t);
	t.background = background_get(ec, &t);
	ref_floor_add(&ec->ref_floor, t.ref_power);
	t.active = t.ref_power >= t.ref_bar;
	t.probing = ec->probe_frames != 0u;
	adapt = t.active && !ec->freeze_on && !t.held &&
		(t.probing || !double_talk_get(ec));
	/* The denominator floor of this frame: power_floor plus the tracked
	 * near-end floor over the taps (the LEC_POWER_PER_TAP comment). */
	step_floor = (ec->power_floor + (float)ec->taps * ec->noise_power) *
		     ec->step_floor_scale;
	if (adapt)
		memcpy(ec->backup, ec->coef, sizeof(*ec->coef) * ec->taps);
	if (zero) {
		t.res_power = frame_zero_run(ec, mic, ref);
		ec->skip_frames++;
	} else {
		t.res_power = frame_filter_run(ec, mic, ref, adapt,
					       LEC_STEP * step_scale_get(ec) *
					       talk_scale_get(ec, t.mic_power,
							      t.window_power),
					       step_floor, &t.estimate_power);
	}
	/* Whatever the verdict below does with the frame's update, the
	 * coefficients moved, and a rollback moves them again. */
	if (adapt)
		ec->coef_dirty = true;
	t.estimate_power /= ec->frame;
	t.res_power /= ec->frame;
	/*
	 * Measured every frame, hold or no hold: these are the only powers a
	 * held instance still sees, and the probe judges its trial by them.
	 */
	ec->live_mic_power += (t.mic_power - ec->live_mic_power) *
			      LEC_ERLE_ALPHA;
	ec->live_res_power += (t.res_power - ec->live_res_power) *
			      LEC_ERLE_ALPHA;
	if (t.active)
		echo_gain_update(ec, &t);
	doubletalk_update(ec, &t);
	/*
	 * The detector compares the microphone against the echo estimate of
	 * this same frame, which only exists once the frame is filtered, so
	 * the frame that turns out to carry near-end speech has already been
	 * adapted on. Its update is rolled back from the copy taken above:
	 * the alternative, judging a frame late, lets one onset frame move
	 * the filter and costs seconds of reconvergence. A probe frame is
	 * exempt, because the whole point of the trial is to move the filter
	 * against the verdict.
	 */
	if (adapt && double_talk_get(ec) && !t.probing) {
		memcpy(ec->coef, ec->backup, sizeof(*ec->coef) * ec->taps);
		adapt = false;
	}
	/* The snapshot of a trial that doubletalk_update started on this frame
	 * is the filter the frame ends with, so it follows the rollback. */
	if (!t.probing && ec->probe_frames)
		memcpy(ec->probe, ec->coef, sizeof(*ec->coef) * ec->taps);
	if (!adapt)
		ec->hold_frames++;
	/*
	 * The trial's budget is spent in the currency it adapts in: on a
	 * silent reference the filter stands still, so counting those frames
	 * would let a far-end pause consume the whole trial against silence.
	 * freeze stands the filter still for the same reason from the other
	 * side, so the countdown follows the frame's own adapt verdict and a
	 * freeze that straddles a trial cannot hand it an incomplete one.
	 */
	if (t.probing && adapt && !probe_diverged(ec) &&
	    --ec->probe_frames == 0u)
		probe_finish(ec);
	if (t.active && !double_talk_get(ec))
		erle_update(ec, t.mic_power, t.res_power);
	/* The level of the residual, read once for the floor tracker and the
	 * processor: nothing writes the frame between the two. */
	t.level = udsp_cn_level_get(mic, ec->frame);
	noise_floor_update(ec, &t);
	divergence_check(ec, t.mic_power);
	nlp_apply(ec, &t, mic);
	ec->frames++;
	reading_fill(ec, &t);
}

static void ec_freeze(struct udsp_ec *ec, bool on)
{
	ec->freeze_on = on;
}

static void ec_nlp(struct udsp_ec *ec, bool on)
{
	ec->nlp_on = on;
}

unsigned udsp_ec_nlms_tail_get(const struct udsp_ec *ec)
{
	if (!ec)
		return 0;
	return ec->tail_ms;
}

void udsp_ec_nlms_info_get(const struct udsp_ec *ec,
			   struct udsp_ec_nlms_info *info)
{
	memset(info, 0, sizeof(*info));
	if (!ec)
		return;
	info->tail_ms = ec->tail_ms;
	info->taps = ec->taps;
	info->frames = ec->frames;
	info->hold_frames = ec->hold_frames;
	info->skip_frames = ec->skip_frames;
	info->diverge_resets = ec->diverge_resets;
	info->probes = ec->probes;
	info->coef_energy = ec->coef_energy;
	info->erle_db = ec->belief.erle_db;
	info->echo_gain = ec->belief.echo_gain;
	info->double_talk = double_talk_get(ec);
	info->nlp_active = ec->nlp_active;
	info->nlp_active_frames = ec->nlp_active_frames;
	info->nlp_wait_frames = ec->nlp_wait_frames;
	info->nlp_erle_frames = ec->nlp_erle_frames;
	info->nlp_unlike_frames = ec->nlp_unlike_frames;
	info->nlp_dt_frames = ec->nlp_dt_frames;
	info->nlp_quiet_frames = ec->nlp_quiet_frames;
	info->coef_peak_tap = ec->coef_peak_tap;
	info->noise_dbov = ec->noise_dbov;
	info->nlp_dt_fired_frames = ec->nlp_dt_fired_frames;
	info->ref_floor = ref_floor_get(&ec->ref_floor);
}

void udsp_ec_nlms_frame_get(const struct udsp_ec *ec,
			    struct udsp_ec_nlms_frame *frame)
{
	memset(frame, 0, sizeof(*frame));
	if (!ec)
		return;
	*frame = ec->reading;
}

const char *udsp_ec_nlms_verdict_name(unsigned verdict)
{
	switch ((enum nlp_verdict)verdict) {
	case NLP_VERDICT_OFF:
		return "off";
	case NLP_VERDICT_QUIET:
		return "quiet";
	case NLP_VERDICT_DT:
		return "dt";
	case NLP_VERDICT_UNLIKE:
		return "unlike";
	case NLP_VERDICT_ERLE:
		return "erle";
	case NLP_VERDICT_PASS:
		return "pass";
	}
	return "?";
}

bool udsp_ec_nlms_path_set(enum udsp_ec_nlms_path path)
{
	if (!udsp_ec_nlms_path_name(path))
		return false;
#if defined(__aarch64__)
	lec_simd_on = path == UDSP_EC_NLMS_PATH_SIMD;
	return true;
#else
	return path == UDSP_EC_NLMS_PATH_SCALAR;
#endif
}

enum udsp_ec_nlms_path udsp_ec_nlms_path_get(void)
{
#if defined(__aarch64__)
	return lec_simd_on ? UDSP_EC_NLMS_PATH_SIMD : UDSP_EC_NLMS_PATH_SCALAR;
#else
	return UDSP_EC_NLMS_PATH_SCALAR;
#endif
}

void udsp_ec_nlms_zero_skip_set(struct udsp_ec *ec, bool on)
{
	if (ec)
		ec->zero_skip_on = on;
}

bool udsp_ec_nlms_zero_skip_get(const struct udsp_ec *ec)
{
	return ec && ec->zero_skip_on;
}

void udsp_ec_nlms_ref_floor_set(struct udsp_ec *ec, bool on)
{
	if (ec)
		ec->ref_floor_on = on;
}

bool udsp_ec_nlms_ref_floor_get(const struct udsp_ec *ec)
{
	return ec && ec->ref_floor_on;
}

const char *udsp_ec_nlms_path_name(enum udsp_ec_nlms_path path)
{
	switch (path) {
	case UDSP_EC_NLMS_PATH_SCALAR:
		return "scalar";
	case UDSP_EC_NLMS_PATH_SIMD:
		return "simd";
	}
	return NULL;
}

void udsp_ec_nlms_reset(struct udsp_ec *ec)
{
	if (!ec)
		return;
	memset(ec->mem, 0, sizeof(float) * mem_floats_get(ec->taps));
	memset(ec->window_power, 0, sizeof(ec->window_power));
	ec->window_index = 0;
	ec->window_prev = 0.0f;
	ec->hist_pos = 0;
	ec->hist_energy = 0.0;
	ec->belief.dt_hangover = 0;
	ec->dt_run = 0;
	ec->dt_gap = LEC_GEIGEL_REPEAT_FRAMES;
	ec->path_run = 0;
	ec->stale_run = 0;
	ec->standdown = 0;
	ec->standdown_arms = 0;
	ec->trial_owed = 0;
	ec->probe_frames = 0;
	ec->probe_ratio_before = 0.0f;
	memset(&ec->probe_state, 0, sizeof(ec->probe_state));
	/* The processor restarts disengaged like everything else: an engaged
	 * state kept across a reset cross-fades the first frame after it into
	 * comfort noise. */
	ec->nlp_active = false;
	ec->nlp_run = 0;
	ec->belief.gain_warmup = 0;
	/* The floor itself survives a reset (below); a half-collected run of
	 * evidence about it does not, because it was collected through the
	 * filter this call just threw away. A half-collected rise run goes
	 * too: the quiet-window gate that took its readings read the old
	 * alignment of the reference. The seed run goes for the same reason
	 * as the stale run: its gates read the ERLE and the estimate of the
	 * old filter. */
	ec->noise_stale_run = 0;
	noise_run_clear(&ec->noise_rise);
	noise_run_clear(&ec->noise_seed);
	ec->belief.gain_mic_avg = 0.0f;
	ec->belief.gain_win_avg = 0.0f;
	ec->belief.echo_gain = ec->gain_min;
	ec->frames = 0;
	ec->hold_frames = 0;
	ec->skip_frames = 0;
	ec->nlp_active_frames = 0;
	ec->nlp_wait_frames = 0;
	ec->nlp_erle_frames = 0;
	ec->nlp_unlike_frames = 0;
	ec->nlp_dt_frames = 0;
	ec->nlp_quiet_frames = 0;
	ec->nlp_dt_fired_frames = 0;
	ec->window_run = 0;
	ec->belief.trust_run = 0;
	ec->quiet_run = 0;
	ec->belief.trust_rule = false;
	ec->belief.trust_held = false;
	memset(&ec->reading, 0, sizeof(ec->reading));
	ec->live_mic_power = 0.0f;
	ec->live_res_power = 0.0f;
	ec->belief.mic_power_avg = LEC_ERLE_FLOOR;
	ec->belief.res_power_avg = LEC_ERLE_FLOOR;
	ec->belief.erle_db = 0.0f;
	coef_energy_clear(ec);
	/*
	 * diverge_resets and probes survive on purpose: they are lifetime
	 * telemetry the instruments accumulate across recalibrations. The
	 * noise floor survives too, because the near end did not change with
	 * the reference timing and re-measuring it would dip the comfort
	 * noise to silence for no reason. The far-end floor survives as well:
	 * the far end did not change with the reference timing, and a new
	 * history would hold the fixed bar for 300 frames. The reference loss
	 * survives with its scaled rules, because the engine sets it, and so
	 * do the freeze, the processor switch and the two test switches.
	 */
}

void udsp_ec_nlms_ref_loss_set(struct udsp_ec *ec, unsigned half_db)
{
	if (!ec)
		return;
	if (half_db > UDSP_EC_NLMS_REF_LOSS_MAX)
		half_db = UDSP_EC_NLMS_REF_LOSS_MAX;
	if (half_db == ec->ref_loss_half_db)
		return;
	ref_loss_scale_set(ec, half_db);
	/* A reference feed fills the history and the far-end floor before the
	 * first run, and a reset keeps the floor, so the frame count cannot
	 * tell an empty instance from one with a history in the old scale.
	 * Every history therefore starts over with the loss. */
	udsp_ec_nlms_reset(ec);
	memset(&ec->ref_floor, 0, sizeof(ec->ref_floor));
}

unsigned udsp_ec_nlms_ref_loss_get(const struct udsp_ec *ec)
{
	return ec ? ec->ref_loss_half_db : 0u;
}

/*
 * The history intake of run() alone: the window ring entry, the window
 * maximum that the guard window of the next frame reads, the far-end floor,
 * and one hist_push per sample, which also keeps the exact hist_energy sum that
 * the zero skip reads. run() interleaves the same pushes with the estimate,
 * so it keeps its own loops and its arithmetic order.
 */
void udsp_ec_nlms_ref_feed(struct udsp_ec *ec, const int16_t *ref)
{
	float power;
	unsigned i;

	if (!ec)
		return;
	power = udsp_ec_nlms_frame_power_get(ref, ec->frame);
	window_push(ec, power);
	ec->window_prev = window_power_get(ec);
	ref_floor_add(&ec->ref_floor, power);
	for (i = 0; i < ec->frame; i++)
		hist_push(ec, ref[i]);
}

const struct udsp_ec_ops udsp_ec_nlms = {
	.name = "nlms",
	.create = ec_create,
	.destroy = ec_destroy,
	.run = ec_run,
	.freeze = ec_freeze,
	.nlp = ec_nlp,
	.create_rate = ec_create_rate,
};
