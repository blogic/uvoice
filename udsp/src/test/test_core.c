// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Unit tests for the engine control plane (the core directory).
 *
 * Three backends drive the cases. The paced backend below is the primary one.
 * It clocks read_period from CLOCK_MONOTONIC at a settable period, reports
 * paced = true, sources capture from a small script, and records every
 * playback period. A case is therefore deterministic in the time base of the
 * engine, while the slic and emitter threads still get real wakes.
 *
 * The file backend (tdm/file.c) is unpaced and ticks at CPU speed. It runs
 * only the cases whose subject is that property: the UDSP_MAX_TICKS bound and
 * the paced guard of the watchdog. A virtual clock that outruns wall time by
 * three orders of magnitude gives the hook sampler one sample per thousand
 * ticks. The raw ALSA backend runs the library end to end against the model of
 * the PCM core in tdm_raw_fake.h.
 *
 * Quiescence. While a job runs, the journal and the per-channel record of the
 * fake belong to the slic thread. A case reads them only when no job is in
 * flight (tc_quiesce) or after the udsp_stop completion. The other fake entry
 * points that a case calls while the engine runs are thread-safe, as their
 * declarations in slic/fake.h state.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "udsp/udsp.h"

#include "../core/safe_state.h"
#include "../core/stream.h"
#include "../core/udsp_priv.h"
#include "../dsp/cid.h"
#include "../dsp/dsp.h"
#include "../dsp/resamp.h"
#include "../slic/fake.h"
#include "../tdm/tdm_common.h"

#define EH_WAIT_SLACK_US	5000000u

#include "bench/lec_replay.h"
#include "engine_harness.h"
#include "fax_fake.h"
#include "level.h"
#include "region_edit.h"
#include "tdm_raw_fake.h"
#include "util.h"

/*
 * The bounded stop leaks the memory of a thread it could not join. Every such
 * deliberate leak happens inside a forked child that leaves through
 * _exit(), so LeakSanitizer's atexit check never runs on a leaked graph and
 * this suite needs no suppression at all.
 */
#define TC_DTMF_MAX		4u
/* The shortened watchdog cadence of the stall cases, and a window of ten of
 * its runs for the cases that prove the absence of a stall. */
#define TC_WATCHDOG_MS		"20"
#define TC_WATCHDOG_WINDOW_US	200000u
#define TC_PACE_FAST_US		100u
/* The longest wait of a fast tick for the slic thread: one tick period. The
 * wait then leaves one period for the tick itself below the two periods that
 * the engine counts as a capture stall (engine_interval_note). */
#define TC_FOLLOW_US		10000u
#define TC_FOLLOW_STEP_NS	20000L
/* The line lag that the flash window adds to a measured open, hook.c
 * LINE_HOOK_FLASH_TOL_MS. */
#define TC_HOOK_FLASH_TOL_MS	50u

/* --- paced test tdm backend ---------------------------------------------- */

struct tc_dtmf_item {
	_Atomic int digit;
	_Atomic uint_least32_t from_ms;
	_Atomic uint_least32_t to_ms;
};

/* The fields of the paced back end that test_media.c does not share. */
static struct {
	/* A case sets this after open, when the tick already runs, so the
	 * field is atomic: a plain write races the reader. */
	_Atomic bool recurse;
	/* One impulse per channel, armed by a case and taken by the next
	 * capture period, with the playback frame of the tick that took it.
	 * The two make the delay of the loop measurable in whole frames. */
	_Atomic int cap_mark[2];
	_Atomic uint_least32_t cap_mark_at[2];
	/* Scripted refusals and faults. udsp_open tears the engine down and
	 * reopens the TDM at 8000 when the plug-in reports no wideband
	 * capability or when the TDM refuses the rate, so both halves of
	 * that open must be drivable here: refuse_rate makes open() decline
	 * exactly that rate, start_err fails the start, which is a cause
	 * that has nothing to do with the rate. opens counts the open()
	 * calls, so a case can see whether the engine retried. */
	unsigned refuse_rate;
	int start_err;
	unsigned opens;
	/* The near end: a tone at the line rate, and the hybrid echo of the
	 * playback written echo_periods ticks ago. echo_shift is the return
	 * loss of that hybrid: the echo is divided by 1 << echo_shift, so 0 is
	 * a unity hybrid and 2 is a 12 dB one. A canceller reads the loss, so
	 * a case about the canceller needs a hybrid inside the range the
	 * detector expects. */
	_Atomic uint_least32_t tone_hz;
	double tone_phase;
	/* A case sets these while the tick reads them, so they are atomic like
	 * recurse above: a plain write races the reader. */
	_Atomic unsigned echo_periods;
	_Atomic unsigned echo_shift;
	/* The line offset and the room of the near end, in thousandths of an
	 * LSB: a constant, and a noise of that RMS. The SLIC adds the offset
	 * after its gain stage, so a case moves it to model a gain change. The
	 * quantizer carries its rounding error, so the mean of the samples is
	 * the offset exactly. The generator state belongs to the engine
	 * thread. */
	_Atomic int dc_milli;
	_Atomic unsigned room_milli;
	uint64_t room_rng;
	double room_carry;
	/* The part mutes an on-hook line in both directions, the offset
	 * included, so a muted capture reads exact zeros. One store sets the
	 * mute, so no frame carries the room without the offset. */
	_Atomic bool mute;
	/* The next clip_frames capture periods of channel 0 carry the word of
	 * an overdriven line: half a frame at each rail. The first sample puts
	 * the mean of the frame on the offset, so the estimate stays where it
	 * is and the frame after the run reads the carry alone. */
	_Atomic unsigned clip_frames;
	/* The next read reports this many capture periods as overwritten, the
	 * soft overrun of tdm.h. It is what a hold leaves behind, without a
	 * ring model: the engine counts them, steps its clock over them and
	 * calls the skip hook with them. */
	_Atomic unsigned skip_periods;
	/* tc_hold_ops only: the next read returns after this many periods of
	 * hardware time with no tick to spend them, which is the hold a kernel
	 * holder on the engine CPU leaves behind. */
	_Atomic unsigned hold_periods;
	/* tc_hold_ops only: frames added to the reported playback queue. A case
	 * moves the latched loop reference with it and touches neither the ring
	 * model, nor the trim, nor the episode rules. A card reads its two
	 * pointers a moment apart and the sum moves with the phase; this is
	 * that offset. Each change must stay within one period of the value the
	 * standing delay_ref was latched with, and must be made while the
	 * calibration stands. engine_delay_drift allows one period, and a
	 * two-period step would count a delay stall or open an episode of its
	 * own. */
	_Atomic long delay_bias;
	/* tc_hold_ops only: frames the reported playback queue reads deeper
	 * from a hold until the next one, on top of delay_bias. It is the move
	 * of a part whose pointers restart at a new phase after a dry ring, the
	 * one the canceller follows. The engine refills the ring to its rest
	 * depth after a hold (engine.c engine_rest_reached), so without this
	 * shift a hold leaves the loop where it was. A hold sets the shift, it
	 * does not add to it, so every hold leaves the same figure. */
	_Atomic long hold_shift;
	struct tc_dtmf_item dtmf[TC_DTMF_MAX];
	_Atomic unsigned dtmf_count;
	struct udsp_dtmf_generator gen;
	int gen_digit;
	/* The handle whose slic thread a fast tick waits for. The test thread
	 * sets it after the open and clears it before the close. */
	_Atomic(udsp_t *) follow;
} tc_be;

/* One period per slot, wide enough for two channels at the wideband frame. */
#define TC_ECHO_DEPTH	8u
static int16_t tc_echo[TC_ECHO_DEPTH][2 * UDSP_FRAME_MAX];
static unsigned tc_echo_w;

/* The ring model of tc_hold_ops below: the capture backlog and the playback
 * queue in frames, engine thread only. */
static long tc_backlog;
static long tc_queued;
/* The hold_shift the last hold applied, engine thread only. */
static long tc_hold_bias;

/* The shift of every tc_hold_ops case: two periods at 8 kHz, so the model
 * reads a loop of 240 at rest and 400 after any hold. */
#define TC_HOLD_SHIFT	160L

static void tc_backend_reset(void)
{
	eh_be_reset(true);
	memset(&tc_be.dtmf, 0, sizeof(tc_be.dtmf));
	atomic_store(&tc_be.dtmf_count, 0u);
	tc_be.gen_digit = 0;
	tc_be.refuse_rate = 0;
	tc_be.start_err = 0;
	tc_be.opens = 0;
	atomic_store(&tc_be.tone_hz, 0u);
	atomic_store(&tc_be.skip_periods, 0u);
	atomic_store(&tc_be.hold_periods, 0u);
	atomic_store(&tc_be.delay_bias, 0L);
	atomic_store(&tc_be.hold_shift, TC_HOLD_SHIFT);
	tc_backlog = 0;
	tc_queued = 0;
	tc_hold_bias = 0;
	tc_be.tone_phase = 0.0;
	tc_be.echo_periods = 0;
	tc_be.echo_shift = 0;
	atomic_store(&tc_be.dc_milli, 0);
	atomic_store(&tc_be.room_milli, 0u);
	tc_be.room_rng = 0x5eed0ffc0ffeeu;
	tc_be.room_carry = 0.0;
	atomic_store(&tc_be.mute, false);
	atomic_store(&tc_be.clip_frames, 0u);
	memset(tc_echo, 0, sizeof(tc_echo));
	tc_echo_w = 0;
	tc_be.recurse = false;
	for (unsigned i = 0; i < 2u; i++) {
		atomic_store(&tc_be.cap_mark[i], 0);
		atomic_store(&tc_be.cap_mark_at[i], UINT32_MAX);
	}
	atomic_store(&tc_be.follow, NULL);
}

static struct udsp_tdm *tc_open(const char *play, const char *cap,
				const struct udsp_tdm_geom *want,
				struct udsp_tdm_geom *got,
				struct udsp_tdm_fail *fail)
{
	tc_be.opens++;
	/* A device that will not take this rate at all, which is the first
	 * half of the settle: the back end's own geometry answer (tdm.h
	 * Refusal), so the settle may move the rate on it. */
	if (tc_be.refuse_rate && want->rate == tc_be.refuse_rate) {
		tdm_fail_set(fail, UDSP_TDM_DIR_PLAYBACK, true, -EINVAL, "rate");
		return NULL;
	}

	return eh_open_geom(want, got);
}

static int tc_start_be(struct udsp_tdm *t) { return tc_be.start_err; }

/* A near-end tone at the line rate: the handset does not know the detectors
 * run at 8 kHz behind the line decimators. */
static void tc_tone_fill(int16_t *buf, unsigned frames)
{
	double hz = (double)atomic_load_explicit(&tc_be.tone_hz,
						 memory_order_acquire);
	double step = 2.0 * M_PI * hz / (double)eh_be.geom.rate;

	for (unsigned i = 0; i < frames; i++) {
		buf[i] = (int16_t)(8000.0 * sin(tc_be.tone_phase));
		tc_be.tone_phase += step;
		if (tc_be.tone_phase > 2.0 * M_PI)
			tc_be.tone_phase -= 2.0 * M_PI;
	}
}

static void tc_capture_fill(int16_t *buf, unsigned frames)
{
	uint32_t now = eh_now_ms();
	unsigned n = atomic_load_explicit(&tc_be.dtmf_count, memory_order_acquire);
	int digit = 0;

	if (atomic_load_explicit(&tc_be.tone_hz, memory_order_acquire)) {
		tc_tone_fill(buf, frames);
		return;
	}
	for (unsigned i = 0; i < n && i < TC_DTMF_MAX; i++) {
		uint32_t from = (uint32_t)atomic_load(&tc_be.dtmf[i].from_ms);
		uint32_t to = (uint32_t)atomic_load(&tc_be.dtmf[i].to_ms);

		if (now >= from && now < to)
			digit = atomic_load(&tc_be.dtmf[i].digit);
	}
	if (!digit) {
		tc_be.gen_digit = 0;
		return;
	}
	if (digit != tc_be.gen_digit) {
		/* The handset dials at the line rate, so a wideband line is
		 * fed 16 kHz digits and the detector only sees them through
		 * the line decimator. */
		tc_be.gen_digit = digit;
		udsp_dtmf_generator_init(&tc_be.gen, (char)digit, -10,
					 eh_be.geom.rate);
	}
	udsp_dtmf_generator_render(&tc_be.gen, buf, frames);
}

/* The recursion runs on the engine tick thread, and no other: only the
 * alternate stack that thread installs for itself can serve the fault. */
static void (*tc_read_recurse_next)(int depth);

static void tc_read_recurse(int depth)
{
	volatile char pad[2048];

	pad[0] = (char)depth;
	tc_read_recurse_next(depth + 1);
	pad[1] = pad[0];
}

/* One armed impulse per channel, in the first sample of this period, with the
 * playback frame of this tick beside it: the record holds one frame per write,
 * so the two frame numbers differ by exactly the delay the loop played. */
static void tc_cap_mark_apply(int16_t *buf, unsigned frames)
{
	unsigned len = (unsigned)atomic_load_explicit(&eh_be.rec_len,
						      memory_order_relaxed);

	for (unsigned ch = 0; ch < eh_be.geom.channels && ch < 2u; ch++) {
		int v = atomic_exchange_explicit(&tc_be.cap_mark[ch], 0,
						 memory_order_acq_rel);

		if (!v)
			continue;
		buf[ch * frames] = (int16_t)v;
		atomic_store_explicit(&tc_be.cap_mark_at[ch], len / frames,
				      memory_order_release);
	}
}

/* Twelve uniforms less six: a unit normal close enough for a room, with a
 * fixed sequence from the reset of the back end. */
static double tc_room_gauss(void)
{
	double sum = 0.0;

	for (unsigned i = 0; i < 12u; i++) {
		tc_be.room_rng = tc_be.room_rng * 6364136223846793005u +
				 1442695040888963407u;
		sum += (double)(tc_be.room_rng >> 11) / 9007199254740992.0;
	}

	return sum - 6.0;
}

static void tc_room_add(int16_t *buf, unsigned n)
{
	double dc = atomic_load_explicit(&tc_be.dc_milli,
					 memory_order_relaxed) / 1000.0;
	double room = atomic_load_explicit(&tc_be.room_milli,
					   memory_order_relaxed) / 1000.0;

	if (dc == 0.0 && room == 0.0)
		return;
	for (unsigned i = 0; i < n; i++) {
		double v = dc + tc_be.room_carry;
		int32_t s;
		double q;

		if (room != 0.0)
			v += room * tc_room_gauss();
		q = rint(v);
		tc_be.room_carry = v - q;
		s = buf[i] + (int32_t)q;
		buf[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
	}
}

/* The rail runs sum to 32768 - half, so the first sample brings the sum of
 * the frame to frames times the offset. */
static void tc_clip_apply(int16_t *buf, unsigned frames)
{
	unsigned half = frames / 2u;
	long sum;

	if (!atomic_load_explicit(&tc_be.clip_frames, memory_order_relaxed))
		return;
	atomic_fetch_sub_explicit(&tc_be.clip_frames, 1u, memory_order_relaxed);
	sum = lrint(frames * atomic_load_explicit(&tc_be.dc_milli,
						  memory_order_relaxed) /
		    1000.0);
	buf[0] = (int16_t)(sum - (32768L - (long)half));
	for (unsigned i = 1; i <= half; i++)
		buf[i] = 32767;
	for (unsigned i = half + 1u; i < frames; i++)
		buf[i] = -32768;
}

static void tc_capture_frame(int16_t *buf)
{
	unsigned frames = eh_be.geom.frames_per_period;
	uint64_t reads = atomic_load_explicit(&eh_be.reads,
					      memory_order_relaxed);

	memset(buf, 0, (size_t)frames * eh_be.geom.channels * sizeof(*buf));
	tc_capture_fill(buf, frames);
	tc_cap_mark_apply(buf, frames);
	/* The hybrid: what the line played echo_periods ticks ago comes back
	 * up. The depth is the engine's own loop delay, so the canceller's
	 * reference lines up with it and the filter can converge. */
	if (tc_be.echo_periods) {
		unsigned r = (tc_echo_w + TC_ECHO_DEPTH - tc_be.echo_periods) %
			     TC_ECHO_DEPTH;
		unsigned n_ech = frames * eh_be.geom.channels;
		int loss = 1 << tc_be.echo_shift;

		for (unsigned i = 0; i < n_ech; i++)
			buf[i] = (int16_t)(buf[i] + tc_echo[r][i] / loss);
	}
	tc_room_add(buf, frames * eh_be.geom.channels);
	tc_clip_apply(buf, frames);
	if (atomic_load_explicit(&tc_be.mute, memory_order_relaxed))
		memset(buf, 0,
		       (size_t)frames * eh_be.geom.channels * sizeof(*buf));
	atomic_store_explicit(&eh_be.reads, reads + 1u, memory_order_release);
}

/*
 * The fault poll and the monitor cadence count passes of the slic thread, not
 * ticks. On the target the slic thread runs SCHED_FIFO, and in the per-tick
 * modes it makes one pass per tick. On a loaded host the fast pace can run
 * many ticks per pass, and a fixed engine span then holds no poll. A fast tick
 * therefore waits until the slic thread is back in its wait. The wait must be
 * bounded, because a stop, a start or a SLOW job keeps the slic thread out of
 * its wait while it waits for the engine. The loads are relaxed. The wait
 * changes the timing only, so the ThreadSanitizer leg sees no ordering that
 * the target lacks.
 */
static void tc_slic_follow(void)
{
	udsp_t *s = atomic_load_explicit(&tc_be.follow, memory_order_relaxed);
	struct timespec step = { .tv_sec = 0, .tv_nsec = TC_FOLLOW_STEP_NS };
	uint64_t end;

	if (!s || atomic_load_explicit(&eh_be.pace_us, memory_order_relaxed) >=
		  EH_PACE_US)
		return;
	if (atomic_load_explicit(&s->slic_armed, memory_order_relaxed))
		return;
	end = eh_mono_us() + TC_FOLLOW_US;
	while (!atomic_load_explicit(&s->slic_armed, memory_order_relaxed) &&
	       eh_mono_us() < end)
		nanosleep(&step, NULL);
	/* The time of the wait is not engine time to recover. Without a new
	 * base, the next ticks run with no pause and outrun the test thread. */
	eh_be.deadline_us = 0;
}

static int tc_read(struct udsp_tdm *t, int16_t *buf)
{
	uint64_t reads = atomic_load_explicit(&eh_be.reads,
					      memory_order_relaxed);

	if (tc_be.recurse) {
		tc_read_recurse_next = tc_read_recurse;
		tc_read_recurse(0);
	}
	if (eh_wedge(reads))
		return -EAGAIN;

	tc_slic_follow();
	eh_pace(eh_mono_us());
	tc_capture_frame(buf);

	return (int)atomic_exchange_explicit(&tc_be.skip_periods, 0u,
					     memory_order_acq_rel);
}

static int tc_write(struct udsp_tdm *t, const int16_t *buf)
{
	unsigned frames = eh_be.geom.frames_per_period;

	if (tc_be.echo_periods) {
		memcpy(tc_echo[tc_echo_w], buf,
		       (size_t)frames * eh_be.geom.channels * sizeof(*buf));
		tc_echo_w = (tc_echo_w + 1u) % TC_ECHO_DEPTH;
	}
	eh_rec_add(buf);

	return 0;
}

static const struct udsp_tdm_ops tc_ops = {
	.name = "tc",
	.open = tc_open,
	.close = eh_close_be,
	.recover = eh_recover,
	.start = tc_start_be,
	.stop = eh_stop_be,
	.read_period = tc_read,
	.write_period = tc_write,
	.delay = eh_delay,
};

/*
 * The same back end with the three fresh reads of tdm.h, over a model of the
 * two rings. One period of hardware time arrives in capture and leaves
 * playback, and the model spends the wall time of that period where a real
 * pair spends it: in the read that waits for its period and in the write that
 * waits for room. A hold is that time passing with no tick to spend it, so the
 * catch-up ticks that follow it run with no wait at all, exactly as they do on
 * a card.
 */
static long tc_period(void)
{
	return (long)eh_be.geom.frames_per_period;
}

static long tc_play_ring(void)
{
	return (long)eh_be.geom.prefill_periods * tc_period();
}

static void tc_ring_step(unsigned periods)
{
	long f = (long)periods * tc_period();

	tc_backlog += f;
	tc_queued -= tc_queued < f ? tc_queued : f;
}

static void tc_ring_wait(void)
{
	eh_pace(eh_mono_us());
	tc_ring_step(1);
}

static int tc_hold_read(struct udsp_tdm *t, int16_t *buf)
{
	unsigned hold = atomic_exchange_explicit(&tc_be.hold_periods, 0u,
						 memory_order_acq_rel);

	if (hold) {
		tc_ring_step(hold);
		tc_hold_bias = atomic_load_explicit(&tc_be.hold_shift,
						    memory_order_relaxed);
	}
	if (tc_backlog < tc_period())
		tc_ring_wait();
	tc_backlog -= tc_period();
	tc_capture_frame(buf);

	return 0;
}

static int tc_hold_write(struct udsp_tdm *t, const int16_t *buf)
{
	if (tc_play_ring() - tc_queued < tc_period())
		tc_ring_wait();
	tc_queued += tc_period();

	return tc_write(t, buf);
}

/* The bias and the hold shift ride delay() alone. avail, the fresh room and
 * the skip stay raw.
 * The ring model, the trim and the episode open are therefore untouched, and
 * only the latched reference moves. The reading stops at 0, as a card's does.
 * The first write after a hold leaves one period queued. Without the floor, a
 * bias below minus one period reads a negative delay there. The engine takes
 * that as a fault and rebuilds the rings at rest. */
static long tc_hold_delay(struct udsp_tdm *t)
{
	long queued = tc_queued + tc_hold_bias +
		      atomic_load_explicit(&tc_be.delay_bias,
					   memory_order_relaxed);

	return queued < 0 ? 0 : queued;
}

static long tc_hold_avail(struct udsp_tdm *t)
{
	return tc_backlog;
}

static long tc_hold_room_fresh(struct udsp_tdm *t)
{
	return tc_play_ring() - tc_queued;
}

static int tc_hold_skip(struct udsp_tdm *t, unsigned periods)
{
	long f = (long)periods * tc_period();

	tc_backlog -= tc_backlog < f ? tc_backlog : f;

	return 0;
}

static int tc_hold_recover(struct udsp_tdm *t)
{
	tc_backlog = 0;
	tc_queued = 0;
	tc_hold_bias = 0;

	return eh_recover(t);
}

/* A stopped card holds nothing, so the next start begins at rest and its
 * prefill fills an empty ring. */
static int tc_hold_stop(struct udsp_tdm *t)
{
	tc_backlog = 0;
	tc_queued = 0;
	tc_hold_bias = 0;

	return eh_stop_be(t);
}

static const struct udsp_tdm_ops tc_hold_ops = {
	.name = "tc-hold",
	.open = tc_open,
	.close = eh_close_be,
	.recover = tc_hold_recover,
	.start = tc_start_be,
	.stop = tc_hold_stop,
	.read_period = tc_hold_read,
	.write_period = tc_hold_write,
	.delay = tc_hold_delay,
	.avail = tc_hold_avail,
	.capture_avail_fresh = tc_hold_avail,
	.playback_room_fresh = tc_hold_room_fresh,
	.capture_skip = tc_hold_skip,
};

/*
 * The two deliberate-leak cases each get a backend state of their own. Their
 * engine thread never joins, so it keeps whatever memory it touched for good,
 * and a later case that reused the same block would be reading and writing
 * behind a thread nobody can wait for.
 */
struct tw_state {
	_Atomic uint_least64_t reads;
	_Atomic uint_least64_t block_after;
	bool paced;
	int block_fd;
	struct udsp_tdm_geom geom;
	int token;
};

static struct tw_state tw[3];

static struct udsp_tdm *tw_open(struct tw_state *w, const struct udsp_tdm_geom *want,
				struct udsp_tdm_geom *got)
{
	*got = *want;
	got->paced = w->paced;
	w->geom = *got;

	return (struct udsp_tdm *)&w->token;
}

static int tw_read(struct tw_state *w, int16_t *buf)
{
	unsigned frames = w->geom.frames_per_period;
	uint64_t reads = atomic_load_explicit(&w->reads, memory_order_relaxed);
	struct timespec rel = { .tv_sec = 0, .tv_nsec = 1000000L };

	if (reads >= atomic_load_explicit(&w->block_after, memory_order_acquire)) {
		char sink;

		while (read(w->block_fd, &sink, 1) < 0 && errno == EINTR)
			continue;
		return -EAGAIN;
	}
	nanosleep(&rel, NULL);
	memset(buf, 0, (size_t)frames * w->geom.channels * sizeof(*buf));
	atomic_store_explicit(&w->reads, reads + 1u, memory_order_release);

	return 0;
}

static long tw_delay(struct tw_state *w)
{
	return (long)(w->geom.prefill_periods - 1) * w->geom.frames_per_period;
}

#define TW_BACKEND(n)							\
static struct udsp_tdm *tw##n##_open(const char *play, const char *cap,	\
				     const struct udsp_tdm_geom *want,	\
				     struct udsp_tdm_geom *got,		\
				     struct udsp_tdm_fail *fail)	\
{									\
	return tw_open(&tw[n], want, got);				\
}									\
static void tw##n##_close(struct udsp_tdm *t) { }			\
static int tw##n##_recover(struct udsp_tdm *t) { return 0; }		\
static int tw##n##_start(struct udsp_tdm *t) { return 0; }		\
static int tw##n##_stop(struct udsp_tdm *t) { return 0; }		\
static int tw##n##_read(struct udsp_tdm *t, int16_t *buf)		\
{									\
	return tw_read(&tw[n], buf);					\
}									\
static int tw##n##_write(struct udsp_tdm *t, const int16_t *buf)	\
{									\
	return 0;							\
}									\
static long tw##n##_delay(struct udsp_tdm *t)				\
{									\
	return tw_delay(&tw[n]);					\
}									\
static const struct udsp_tdm_ops tw##n##_ops = {			\
	.name = "tw" #n,						\
	.open = tw##n##_open,						\
	.close = tw##n##_close,						\
	.recover = tw##n##_recover,					\
	.start = tw##n##_start,						\
	.stop = tw##n##_stop,						\
	.read_period = tw##n##_read,					\
	.write_period = tw##n##_write,					\
	.delay = tw##n##_delay,						\
}

TW_BACKEND(0);
TW_BACKEND(1);
TW_BACKEND(2);

/* --- harness ------------------------------------------------------------- */

/*
 * One stream's socket pairs and what the peer ends received. Index 0 of each
 * pair goes to udsp_stream_config, index 1 is the far end. The pairs are
 * AF_UNIX SOCK_DGRAM: a 0 byte send arrives as a 0 byte datagram, recv
 * returns 0 for it and -1 with EAGAIN for none, so the empty datagrams the
 * punch sends and the datagrams that carry bytes count apart.
 */
struct tc_dgrams {
	unsigned empty;
	unsigned data;
};

struct tc_stream {
	int rtp[2];
	int rtcp[2];
	struct tc_dgrams rtp_got;
	struct tc_dgrams rtcp_got;
};

static void tc_dgrams_drain(int fd, struct tc_dgrams *d)
{
	uint8_t buf[UDSP_NET_DGRAM_MAX];

	if (fd < 0)
		return;
	for (;;) {
		ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);

		if (n < 0)
			return;
		if (n == 0)
			d->empty++;
		else
			d->data++;
	}
}

static void tc_stream_drain(void *arg)
{
	struct tc_stream *w = arg;

	if (!w)
		return;
	tc_dgrams_drain(w->rtp[1], &w->rtp_got);
	tc_dgrams_drain(w->rtcp[1], &w->rtcp_got);
}

#define TC_TS_MAX	512u
static uint32_t tc_ts[TC_TS_MAX];
static unsigned tc_ts_n;

static void tc_ts_reset(void)
{
	tc_ts_n = 0;
}

static void tc_ts_drain(void *arg)
{
	struct tc_stream *w = arg;
	uint8_t buf[UDSP_NET_DGRAM_MAX];

	for (;;) {
		ssize_t n = recv(w->rtp[1], buf, sizeof(buf), MSG_DONTWAIT);

		if (n < (ssize_t)RTP_HEADER_SIZE)
			return;
		if (tc_ts_n >= TC_TS_MAX)
			return;
		tc_ts[tc_ts_n++] = ((uint32_t)buf[4] << 24) |
				   ((uint32_t)buf[5] << 16) |
				   ((uint32_t)buf[6] << 8) | (uint32_t)buf[7];
	}
}

static void tc_ts_wait(struct eh *t, struct tc_stream *w, uint32_t ms)
{
	eh_wait_each(t, ms, tc_ts_drain, w);
}

/* The far end of w, when given, is drained on every pass, so a sending stream
 * never fills its socket buffer and every datagram is counted. */
static void tc_stream_wait(struct eh *t, struct tc_stream *w, uint32_t ms)
{
	eh_wait_each(t, ms, tc_stream_drain, w);
}

/*
 * The recording sink of udsp_config.log. The library calls it from the control
 * thread and from the slic thread, so the record takes a lock; a case reads it
 * after the call it drove returned. tc_cfg() clears the record, so it holds
 * the lines of the handle a case opens next.
 */
#define TC_LOG_MAX	32
#define TC_LOG_LINE	256

static struct {
	pthread_mutex_t lock;
	char line[TC_LOG_MAX][TC_LOG_LINE];
	uint8_t level[TC_LOG_MAX];
	unsigned n;
} tc_log = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void tc_log_sink(void *ctx, enum udsp_log_level level, const char *msg)
{
	(void)ctx;
	pthread_mutex_lock(&tc_log.lock);
	if (tc_log.n < TC_LOG_MAX) {
		snprintf(tc_log.line[tc_log.n], TC_LOG_LINE, "%s", msg);
		tc_log.level[tc_log.n] = (uint8_t)level;
		tc_log.n++;
	}
	pthread_mutex_unlock(&tc_log.lock);
}

static void tc_log_reset(void)
{
	pthread_mutex_lock(&tc_log.lock);
	tc_log.n = 0;
	pthread_mutex_unlock(&tc_log.lock);
}

static bool tc_log_has(enum udsp_log_level level, const char *needle)
{
	bool found = false;

	pthread_mutex_lock(&tc_log.lock);
	for (unsigned i = 0; i < tc_log.n && !found; i++)
		found = tc_log.level[i] == level && strstr(tc_log.line[i], needle);
	pthread_mutex_unlock(&tc_log.lock);

	return found;
}

static unsigned tc_log_count_needle(enum udsp_log_level level,
				    const char *needle)
{
	unsigned n = 0;

	pthread_mutex_lock(&tc_log.lock);
	for (unsigned i = 0; i < tc_log.n; i++) {
		if (tc_log.level[i] == level && strstr(tc_log.line[i], needle))
			n++;
	}
	pthread_mutex_unlock(&tc_log.lock);

	return n;
}

/* The watchdog cadence is the emitter's own, so a case that waits for one of
 * its records shrinks the cadence with UDSP_WATCHDOG_MS and polls to the
 * bound instead of sleeping a fixed span. */
static bool tc_log_wait_count(enum udsp_log_level level, const char *needle,
			      unsigned count, unsigned ms)
{
	uint64_t end = eh_mono_us() + (uint64_t)ms * 1000u;

	while (eh_mono_us() < end) {
		if (tc_log_count_needle(level, needle) >= count)
			return true;
		eh_sleep_ms(1);
	}

	return tc_log_count_needle(level, needle) >= count;
}

static bool tc_log_wait(enum udsp_log_level level, const char *needle,
			unsigned ms)
{
	return tc_log_wait_count(level, needle, 1u, ms);
}

/* The emitter thread and the slic thread publish on their own time, so a case
 * waits for the state it needs and not for an engine span. Polls the event
 * socket until done answers true or ms of wall time pass. */
static bool tc_poll_until(struct eh *t,
			  bool (*done)(struct eh *t, const void *arg),
			  const void *arg, unsigned ms)
{
	uint64_t end = eh_mono_us() + (uint64_t)ms * 1000u;

	for (;;) {
		eh_ev_poll(t);
		if (done(t, arg))
			return true;
		if (eh_mono_us() >= end)
			return false;
		eh_sleep_ms(1);
	}
}

static unsigned tc_log_count(enum udsp_log_level level)
{
	unsigned n = 0;

	pthread_mutex_lock(&tc_log.lock);
	for (unsigned i = 0; i < tc_log.n; i++)
		n += tc_log.level[i] == level;
	pthread_mutex_unlock(&tc_log.lock);

	return n;
}

static unsigned tc_log_hits(enum udsp_log_level level, const char *needle)
{
	unsigned n = 0;

	pthread_mutex_lock(&tc_log.lock);
	for (unsigned i = 0; i < tc_log.n; i++)
		n += tc_log.level[i] == level && strstr(tc_log.line[i], needle);
	pthread_mutex_unlock(&tc_log.lock);

	return n;
}

static void tc_cfg(struct udsp_config *cfg, const char *backend, const char *region)
{
	memset(cfg, 0, sizeof(*cfg));
	/* udsp_open validates the backend name against the names the public API
	 * allows, so a case that injects the paced backend still writes
	 * "file"; the override decides which ops the engine gets. */
	snprintf(cfg->tdm_backend, sizeof(cfg->tdm_backend), "%s",
		 strcmp(backend, "tc") ? backend : "file");
	cfg->channels = 2;
	cfg->prefill_periods = 3;
	cfg->populated_ports = 2;
	cfg->engine_cpu = -1;
	cfg->engine_prio = 0;
	cfg->log = tc_log_sink;
	tc_log_reset();
	if (region)
		snprintf(cfg->region, sizeof(cfg->region), "%s", region);
}

static bool tc_open_handle(struct eh *t, const struct udsp_config *cfg)
{
	int fd[2];

	memset(t, 0, sizeof(*t));
	t->rx_fd = t->tx_fd = -1;
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fd))
		return false;
	t->rx_fd = fd[0];
	t->tx_fd = fd[1];
	eh_ev_reset();
	atomic_store(&tc_be.follow, NULL);
	t->s = udsp_open(cfg, t->tx_fd);
	atomic_store(&tc_be.follow, t->s);

	return t->s != NULL;
}

static void tc_free(struct eh *t)
{
	atomic_store(&tc_be.follow, NULL);
	if (t->s)
		udsp_close(t->s);
	t->s = NULL;
	if (t->rx_fd >= 0)
		close(t->rx_fd);
	if (t->tx_fd >= 0)
		close(t->tx_fd);
	t->rx_fd = t->tx_fd = -1;
}

static void tc_engine_stats(struct eh *t, struct engine_stats *st)
{
	memset(st, 0, sizeof(*st));
	pthread_mutex_lock(&t->s->engine_lock);
	if (t->s->engine)
		engine_stats_get(t->s->engine, st);
	pthread_mutex_unlock(&t->s->engine_lock);
}

static bool tc_start(struct eh *t)
{
	struct udsp_job job = { 0 };
	int32_t status = -1;

	if (udsp_start(t->s, &job) != UDSP_OK)
		return false;

	return eh_job_wait(t, job.token, 5000, &status) && status == 0;
}

static bool tc_stop(struct eh *t)
{
	struct udsp_job job = { 0 };
	int32_t status = -1;

	if (udsp_stop(t->s, &job) != UDSP_OK)
		return false;

	return eh_job_wait(t, job.token, 5000, &status) && status == 0;
}

/* One paced handle with the region, started, hooks sampling. rate 0 takes the
 * narrowband default (udsp.h sample_rate). */
/* One handle on the given back end with the tick paced at the given interval,
 * so a case can watch a window that lasts a fixed number of ticks. */
static bool tc_up_paced_ports(struct eh *t, const struct udsp_tdm_ops *ops,
			      const char *region, unsigned pace_us,
			      unsigned ports)
{
	struct udsp_config cfg;

	tc_backend_reset();
	eh_be.pace_us = pace_us;
	udsp_slic_fake_reset();
	udsp_tdm_override(ops);
	tc_cfg(&cfg, "tc", region);
	cfg.populated_ports = (uint8_t)ports;
	if (!tc_open_handle(t, &cfg))
		return false;

	return tc_start(t);
}

static bool tc_up_paced(struct eh *t, const struct udsp_tdm_ops *ops,
			const char *region, unsigned pace_us)
{
	return tc_up_paced_ports(t, ops, region, pace_us, 1);
}

/*
 * The fault trial and the recovery holdoff run on the engine clock. The fault
 * poll and the monitor cadence count passes of the slic thread, and at the
 * fast pace tc_slic_follow keeps one pass per tick. A case that only waits
 * them out therefore runs the tick at TC_PACE_FAST_US once the start
 * completed. A case with a wall-clock timer of the slic thread, a lost ack or
 * a delay of the fake keeps EH_PACE_US, because its wall-clock step would
 * cover more engine time at the fast pace.
 */
static void tc_pace_fast(void)
{
	atomic_store(&eh_be.pace_us, TC_PACE_FAST_US);
}

/* The offset, the room and the mute of the line stand before the open, so the
 * first frame of the line carries them. */
static bool tc_up_line(struct eh *t, const char *region, unsigned ports,
		       unsigned rate, int dc_milli, unsigned room_milli,
		       bool mute)
{
	struct udsp_config cfg;

	tc_backend_reset();
	atomic_store(&tc_be.dc_milli, dc_milli);
	atomic_store(&tc_be.room_milli, room_milli);
	atomic_store(&tc_be.mute, mute);
	udsp_slic_fake_reset();
	udsp_tdm_override(&tc_ops);
	tc_cfg(&cfg, "tc", region);
	cfg.populated_ports = (uint8_t)ports;
	cfg.sample_rate = (uint16_t)rate;
	if (!tc_open_handle(t, &cfg))
		return false;

	return tc_start(t);
}

static bool tc_up_rate(struct eh *t, const char *region, unsigned ports,
		       unsigned rate)
{
	return tc_up_line(t, region, ports, rate, 0, 0u, false);
}

static bool tc_up_ports(struct eh *t, const char *region, unsigned ports)
{
	return tc_up_rate(t, region, ports, 0);
}

static bool tc_up(struct eh *t, const char *region)
{
	return tc_up_ports(t, region, 2);
}

/*
 * A region whose caller-identity burst is short enough that the whole type 1
 * sequence still fits in the fake's 64-row journal, which the 10 ms hook poll
 * otherwise fills in well under a second. Only the [cid] timings move; every
 * tone and the hook plan stay the shipped USA ones.
 *
 * wait_ms is a parameter because the [cid] pre-message pause is the one timing
 * a case may need at its shipped value: a caller delay equal to that pause is
 * the production shape, and shortening either half hides it.
 */
static char tc_fast_dir[64];

/* Writes text as the file name in a new directory made from tmpl, and points
 * UDSP_REGION_DIR at that directory. */
static bool tc_region_dir_write(const char *tmpl, const char *name,
				const char *text)
{
	char path[256];
	FILE *f;

	snprintf(tc_fast_dir, sizeof(tc_fast_dir), "%s", tmpl);
	if (!mkdtemp(tc_fast_dir))
		return false;
	snprintf(path, sizeof(path), "%s/%s", tc_fast_dir, name);
	f = fopen(path, "wb");
	if (!f)
		return false;
	fwrite(text, 1, strlen(text), f);
	fclose(f);
	setenv("UDSP_REGION_DIR", tc_fast_dir, 1);

	return true;
}

static void tc_region_dir_drop(const char *name)
{
	char path[256];

	unsetenv("UDSP_REGION_DIR");
	snprintf(path, sizeof(path), "%s/%s", tc_fast_dir, name);
	unlink(path);
	rmdir(tc_fast_dir);
}

static bool tc_region_make(unsigned wait_ms)
{
	char wait_to[24];
	char text[REGION_EDIT_TEXT_MAX];

	snprintf(wait_to, sizeof(wait_to), "wait_ms = %-3u", wait_ms);

	const struct region_edit edit[] = {
		{ NULL, "seizure_bits = 300", "seizure_bits = 0  " },
		{ NULL, "mark_bits = 180", "mark_bits = 20 " },
		{ NULL, "wait_ms = 500", wait_to },
		{ NULL, "iso3 = USA", "iso3 = FST" },
	};

	if (!region_text_edit(UDSP_REGION_DIR "/USA.conf", text, sizeof(text),
			      edit, 4))
		return false;

	return tc_region_dir_write("/tmp/udsp_fst_XXXXXX", "FST.conf", text);
}

static bool tc_region_fast(void)
{
	return tc_region_make(50);
}

static void tc_region_fast_drop(void)
{
	tc_region_dir_drop("FST.conf");
}

/* The shipped USA file with body in place of the [tone sit] keys, written as
 * <iso3>.conf, so a case plays a tone of its own through the sit slot. The
 * caller drops the file with tc_region_dir_drop. */
static bool tc_region_sit_write(const char *iso3, const char *body)
{
	char iso_to[16], name[16];
	char text[REGION_EDIT_TEXT_MAX];

	snprintf(iso_to, sizeof(iso_to), "iso3 = %s", iso3);
	snprintf(name, sizeof(name), "%s.conf", iso3);

	const struct region_edit edit[] = {
		{ "[tone sit]", "segment = 914 -24 276\n"
		  "segment = 1371 -24 276\nsegment = 1777 -24 380\n"
		  "silence = 1000\nrepeat = forever\n", body },
		{ NULL, "iso3 = USA", iso_to },
	};

	if (!region_text_edit(UDSP_REGION_DIR "/USA.conf", text, sizeof(text),
			      edit, 2))
		return false;

	return tc_region_dir_write("/tmp/udsp_sit_XXXXXX", name, text);
}

static void tc_down(struct eh *t)
{
	tc_stop(t);
	tc_free(t);
	udsp_tdm_override(NULL);
}

static bool tc_hook_fail_wait(struct eh *t, unsigned failures, unsigned lines,
			      uint64_t end);

/* --- fake journal helpers ------------------------------------------------ */

static struct udsp_slic_fake_entry tc_jr[UDSP_SLIC_FAKE_JOURNAL_MAX];
static unsigned tc_jr_len;

/*
 * The fake's journal and per-channel record belong to the slic thread while a
 * job runs (fake.h Quiescence). The job counter is the release point the test
 * acquires, so the reads below see everything the slic thread wrote and the
 * ThreadSanitizer leg has the edge it needs.
 */
static void tc_quiesce(struct eh *t)
{
	uint64_t end = eh_mono_us() + 5000000u;

	while (atomic_load_explicit(&t->s->job_inflight, memory_order_acquire) &&
	       eh_mono_us() < end)
		eh_sleep_ms(1);
}

static bool tc_state(struct eh *t, unsigned chan,
		     struct udsp_slic_fake_chan_state *out)
{
	tc_quiesce(t);

	return udsp_slic_fake_chan_state_get(t->s->slic, chan, out);
}

static void tc_journal(struct eh *t)
{
	unsigned lost = 0;

	tc_quiesce(t);

	tc_jr_len = udsp_slic_fake_journal_get(t->s->slic, tc_jr,
					       UDSP_SLIC_FAKE_JOURNAL_MAX, &lost);
	if (tc_jr_len > UDSP_SLIC_FAKE_JOURNAL_MAX)
		tc_jr_len = UDSP_SLIC_FAKE_JOURNAL_MAX;
}

static int tc_jr_find(unsigned call, unsigned chan, int32_t arg0, unsigned from)
{
	for (unsigned i = from; i < tc_jr_len; i++) {
		if (tc_jr[i].call == call && tc_jr[i].chan == chan &&
		    (arg0 < 0 || tc_jr[i].arg0 == arg0))
			return (int)i;
	}

	return -1;
}

static unsigned tc_jr_count(unsigned call, unsigned chan)
{
	unsigned n = 0;

	for (unsigned i = 0; i < tc_jr_len; i++) {
		if (tc_jr[i].call == call && tc_jr[i].chan == chan)
			n++;
	}

	return n;
}

/* --- line test helpers --------------------------------------------------- */

static const struct udsp_event *tc_job_find(uint32_t token)
{
	for (unsigned i = 0; i < eh_ev_len; i++) {
		if (eh_ev[i].type == UDSP_EV_JOB_DONE &&
		    eh_ev[i].u.job.token == token)
			return &eh_ev[i];
	}

	return NULL;
}

/* The delay the fake's metallic line test op takes before it answers, which is
 * what makes the blocking observable; 0 clears it. */
static void tc_mlt_delay(struct eh *t, unsigned ms)
{
	udsp_slic_fake_delay_set(t->s->slic, UDSP_SLIC_FAKE_MLT_RUN, ms);
}

/*
 * Waits until the plug-in holds the line: the slic thread invalidates the
 * published hook pair as it enters the op, so a query that answers TRANSIENT
 * is the first moment a case may act on a line under test. A fixed sleep
 * proves nothing under load.
 */
static bool tc_test_entered(struct eh *t, unsigned line)
{
	uint64_t end = eh_mono_us() + 5000000u;
	bool level = false;

	while (eh_mono_us() < end) {
		if (udsp_line_hook_query(t->s, line, &level) == UDSP_TRANSIENT)
			return true;
		eh_sleep_ms(1);
	}

	return false;
}

/* Submits one test and waits for its completion; *status takes the status the
 * completion carried. */
static bool tc_test_run(struct eh *t, unsigned line, enum udsp_line_test test,
			int32_t *status)
{
	struct udsp_job job = { 0 };

	if (udsp_line_test(t->s, line, test, &job) != UDSP_OK)
		return false;

	return eh_job_wait(t, job.token, 5000, status);
}

/* --- playback oracles ---------------------------------------------------- */

static unsigned tc_first_loud(unsigned from, int16_t level)
{
	unsigned have = eh_rec_have();

	for (unsigned i = from; i < have; i++) {
		int16_t v = eh_be.rec[eh_be.rec_base + i];

		if (v > level || v < -level)
			return i;
	}

	return have;
}

static unsigned tc_last_loud(int16_t level)
{
	unsigned have = eh_rec_have();

	while (have > 0) {
		int16_t v = eh_be.rec[eh_be.rec_base + have - 1];

		if (v > level || v < -level)
			return have - 1;
		have--;
	}

	return 0;
}

/* The first sample of a tone that starts loud. A tone starts on a frame edge
 * at phase 0, so its first loud sample lies in its first frame. */
static unsigned tc_tone_base(void)
{
	return tc_first_loud(0, 500) / UDSP_FRAME_SAMPLES * UDSP_FRAME_SAMPLES;
}

/*
 * Each 10 ms frame of the record from base up to end_ms against the loud
 * spans, in ms from base. A frame inside a span must be loud and every other
 * frame silent, so every span edge falls on a frame edge. Returns the count of
 * the frames that disagree.
 */
static unsigned tc_spans_bad(unsigned base, const uint32_t (*span)[2],
			     unsigned spans, uint32_t end_ms)
{
	unsigned bad = 0;

	for (uint32_t ms = 0; ms < end_ms; ms += 10u) {
		double e = eh_energy(base + ms * (UDSP_SAMPLE_RATE / 1000u),
				     UDSP_FRAME_SAMPLES);
		bool loud = false;

		for (unsigned i = 0; i < spans; i++)
			loud |= ms >= span[i][0] && ms < span[i][1];
		bad += loud ? e <= 100000.0 : e > 100.0;
	}

	return bad;
}

/*
 * The largest residual of the record in [from, from + len) against a sum of
 * two sinusoids at hz1 and hz2. The step y[n] = x[n - 1] + x[n + 1] - c x[n],
 * with c = 2 cos(2 pi hz / 8000), cancels a sinusoid of that frequency at any
 * phase and level. After both steps, the rounding of the samples leaves less
 * than 8, and a phase step of either frequency leaves a residual that grows
 * with the step.
 */
static double tc_two_tone_residual(unsigned from, unsigned len, double hz1,
				   double hz2)
{
	double c1 = 2.0 * cos(2.0 * M_PI * hz1 / UDSP_SAMPLE_RATE);
	double c2 = 2.0 * cos(2.0 * M_PI * hz2 / UDSP_SAMPLE_RATE);
	const int16_t *x = eh_be.rec + eh_be.rec_base + from;
	double worst = 0.0;

	if (len < 5u || from + len > eh_rec_have())
		return HUGE_VAL;
	for (unsigned n = 2; n + 2u < len; n++) {
		double y0 = x[n - 2] + x[n] - c1 * x[n - 1];
		double y1 = x[n - 1] + x[n + 1] - c1 * x[n];
		double y2 = x[n] + x[n + 2] - c1 * x[n + 1];
		double z = fabs(y0 + y2 - c2 * y1);

		if (z > worst)
			worst = z;
	}

	return worst;
}

/* Independent FSK demodulator, adapted from test/test_cid.c: correlate each
 * bit cell against the mark and space carriers and take the stronger. */
static size_t tc_bit_edge(size_t bit)
{
	return (bit * UDSP_SAMPLE_RATE + 1199u) / 1200u;
}

static bool tc_bit(unsigned base, size_t bit, double mark_hz, double space_hz)
{
	size_t start = base + tc_bit_edge(bit);
	size_t end = base + tc_bit_edge(bit + 1u);
	double mi = 0.0, mq = 0.0, si = 0.0, sq = 0.0;

	for (size_t n = start; n < end && n < eh_rec_have(); n++) {
		double ma = 2.0 * M_PI * mark_hz * n / UDSP_SAMPLE_RATE;
		double sa = 2.0 * M_PI * space_hz * n / UDSP_SAMPLE_RATE;
		int16_t v = eh_be.rec[eh_be.rec_base + n];

		mi += v * cos(ma);
		mq += v * sin(ma);
		si += v * cos(sa);
		sq += v * sin(sa);
	}

	return mi * mi + mq * mq > si * si + sq * sq;
}

/*
 * Folds a 16 kHz recording onto the 8 kHz grid the oracle above works on, the
 * same 2:1 decimation the line hands its own detectors (dsp/resamp.h: one
 * instance, reset before first use, even counts, at most UDSP_FRAME_MAX per
 * call, in-place allowed). The engine must be stopped: this rewrites the
 * recorder in place.
 */
static void tc_rec_decimate(void)
{
	struct udsp_resamp rs;
	unsigned have = eh_rec_have();
	unsigned out = 0;

	udsp_resamp_reset(&rs);
	for (unsigned i = 0; i + UDSP_FRAME_MAX <= have; i += UDSP_FRAME_MAX) {
		udsp_resamp_down2(&rs, eh_be.rec + eh_be.rec_base + i,
				  UDSP_FRAME_MAX, eh_be.rec + eh_be.rec_base + out);
		out += UDSP_FRAME_MAX / 2u;
	}
	atomic_store_explicit(&eh_be.rec_len, eh_be.rec_base + out,
			      memory_order_release);
}

/* Decodes the 8N1 data bytes that follow the seizure and mark phases. */
static unsigned tc_fsk_decode(unsigned base, unsigned seizure, unsigned mark,
			      uint8_t *out, unsigned max)
{
	unsigned n = 0;

	while (n < max) {
		size_t off = seizure + mark + (size_t)n * 10u;
		uint8_t byte = 0;

		if (base + tc_bit_edge(off + 10u) >= eh_rec_have())
			break;
		if (tc_bit(base, off, 1200.0, 2200.0))
			break;
		for (unsigned b = 0; b < 8; b++)
			byte |= (uint8_t)(tc_bit(base, off + b + 1u, 1200.0,
						 2200.0) << b);
		out[n++] = byte;
	}

	return n;
}

/* --- the crash-handler child --------------------------------------------- */

/*
 * A leak case has to run in a process of its own. The bounded stop
 * deliberately abandons a thread and keeps the one-live-handle guard taken, so
 * a second open in the same process is refused by design. The child also gives
 * the guard rule itself an oracle.
 */
struct tc_child_page {
	bool opened;
	bool started;
	bool stop_accepted;
	bool job_seen;
	bool dead;
	bool fatal_op;
	bool reopen_refused;
	bool no_fault;
	bool cap_armed;
	bool cap_bit;
	int32_t job_status;
	int cap_rc;
	uint64_t stop_us;
	unsigned faults;
	unsigned stop_timeouts;
	unsigned sync_tick_ms;
	unsigned net_ack_ms;
};

static struct tc_child_page *tc_child;

struct tc_blit_page {
	unsigned messages;
	unsigned segs[16];
	unsigned len;
	uint8_t bytes[UDSP_SLIC_SAFE_MAX];
};

static struct tc_blit_page *tc_page;

static int tc_blit_record(void *ctx, struct spi_ioc_transfer *tr, size_t n)
{
	struct tc_blit_page *p = ctx;

	if (p->messages >= 16)
		return 0;
	p->segs[p->messages] = (unsigned)n;
	for (size_t i = 0; i < n; i++) {
		const uint8_t *src = (const uint8_t *)(uintptr_t)tr[i].tx_buf;

		for (uint32_t k = 0; k < tr[i].len && p->len < UDSP_SLIC_SAFE_MAX; k++)
			p->bytes[p->len++] = src[k];
	}
	p->messages++;

	return 0;
}

/* The volatile frame and the use after the call keep the recursion real at
 * -Os, so the fault hits a guard page and only SA_ONSTACK can serve it.
 * The recursion is unbounded on purpose: the overflow is the case. */
static void (*tc_stack_next)(int depth);

static void tc_stack_eat(int depth)
{
	volatile char pad[2048];

	pad[0] = (char)depth;
	tc_stack_next(depth + 1);
	pad[1] = pad[0];
}

static bool tc_fork_run(void (*body)(void))
{
	pid_t pid;
	int st = 0;

	if (!tc_child)
		return false;
	memset(tc_child, 0, sizeof(*tc_child));
	fflush(stdout);
	pid = fork();
	if (pid < 0)
		return false;
	if (!pid) {
		body();
		_exit(0);
	}
	if (waitpid(pid, &st, 0) != pid)
		return false;

	return WIFEXITED(st) && !WEXITSTATUS(st);
}

static void tc_ack_defaults(void)
{
	if (unsetenv("UDSP_SYNC_TICK_MS") || unsetenv("UDSP_NET_ACK_MS"))
		_exit(2);
	tc_child->sync_tick_ms = cmd_sync_tick_ms_get();
	tc_child->net_ack_ms = net_ack_ms_get();
}

static void case_ack_defaults(void)
{
	CASE("the ack deadlines default to 100 ms without an override");
	/* The child keeps the defaults out of the parent's once-only caches. */
	CHECK(tc_fork_run(tc_ack_defaults));
	CHECK_UINT(tc_child->sync_tick_ms, 100u);
	CHECK_UINT(tc_child->net_ack_ms, 100u);
}

static void tc_crash_child(bool overflow_stack)
{
	struct udsp_config cfg;
	struct eh t;

	tc_backend_reset();
	udsp_slic_fake_reset();
	udsp_tdm_override(&tc_ops);
	udsp_safe_state_msg_set(tc_blit_record, tc_page);
	tc_cfg(&cfg, "tc", NULL);
	if (!tc_open_handle(&t, &cfg))
		_exit(2);

	if (overflow_stack) {
		tc_stack_next = tc_stack_eat;
		tc_stack_eat(0);
	}
	{
		/* A store into a PROT_NONE page, not a null dereference: the
		 * fault has to reach the handler as a real SIGSEGV, and a
		 * sanitizer build diagnoses a null store before delivery. */
		volatile int *p = mmap(NULL, 4096, PROT_NONE,
				       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (p == MAP_FAILED)
			_exit(5);
		*p = 1;
	}
	_exit(4);
}

static bool tc_crash_child_run(void (*body)(void), int *sig)
{
	pid_t pid;
	int st = 0;

	fflush(stdout);
	pid = fork();
	if (pid < 0)
		return false;
	if (!pid) {
		body();
		_exit(9);
	}
	if (waitpid(pid, &st, 0) != pid)
		return false;
	if (!WIFSIGNALED(st))
		return false;
	*sig = WTERMSIG(st);

	return true;
}

static void tc_crash_body_null(void)
{
	tc_crash_child(false);
}

static void tc_crash_body_overflow(void)
{
	tc_crash_child(true);
}

static bool tc_crash_run(bool overflow_stack, int *sig)
{
	memset(tc_page, 0, sizeof(*tc_page));

	return tc_crash_child_run(overflow_stack ? tc_crash_body_overflow :
						   tc_crash_body_null, sig);
}

static unsigned tc_safe_expect(uint8_t *out, unsigned max, unsigned *messages)
{
	const struct udsp_slic_ops *ops = udsp_slic_fake_descriptor.ops;
	struct udsp_slic_safe safe;
	struct udsp_slic *s;
	unsigned channels = 0;
	unsigned n;

	udsp_slic_fake_reset();
	s = ops->attach(NULL, &channels);
	if (!s)
		return 0;
	memset(&safe, 0, sizeof(safe));
	ops->safe_xfer(s, &safe);
	ops->detach(s);

	n = safe.len < max ? (unsigned)safe.len : max;
	memcpy(out, safe.bytes, n);
	*messages = (unsigned)((safe.len + SLIC_HOST_SEGS_MAX - 1) /
			       SLIC_HOST_SEGS_MAX);

	return n;
}

/* --- cases --------------------------------------------------------------- */

static void case_crash(void)
{
	uint8_t want[UDSP_SLIC_SAFE_MAX];
	unsigned want_msgs = 0;
	unsigned want_len;
	int sig = 0;

	CASE("crash handler blits the safe transfer and re-raises");
	{
		want_len = tc_safe_expect(want, sizeof(want), &want_msgs);
		CHECK(want_len > 0);
		CHECK(tc_crash_run(false, &sig));
		CHECK(sig == SIGSEGV);
		CHECK_UINT(tc_page->messages, want_msgs);
		CHECK_UINT(tc_page->len, want_len);
		CHECK(memcmp(tc_page->bytes, want, want_len) == 0);
		/* Per-byte chip select is the measured production framing, so
		 * every segment carries exactly one byte. */
		CHECK_UINT(tc_page->segs[0], want_len);
	}

	CASE("crash handler runs on its own stack after a stack overflow");
	{
		CHECK(tc_crash_run(true, &sig));
		CHECK(sig == SIGSEGV);
		CHECK_UINT(tc_page->messages, want_msgs);
		CHECK(memcmp(tc_page->bytes, want, want_len) == 0);
	}
}

static void case_lifecycle(void)
{
	struct udsp_config cfg;
	struct eh t;

	CASE("open start stop close drives the fake through its lifecycle");
	{
		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_tdm_override(&tc_ops);
		tc_cfg(&cfg, "tc", "USA");
		CHECK(tc_open_handle(&t, &cfg));
		if (t.s) {
			CHECK_UINT(t.s->cap.lines, 2);
			CHECK(tc_start(&t));
			eh_wait_ms(&t, 60);
			CHECK(tc_stop(&t));
			tc_journal(&t);
			CHECK(tc_jr_find(UDSP_SLIC_FAKE_ATTACH, 0, -1, 0) == 0);
			CHECK(tc_jr_find(UDSP_SLIC_FAKE_INIT, 0, -1, 0) > 0);
			CHECK(tc_jr_find(UDSP_SLIC_FAKE_INIT, 1, -1, 0) > 0);
			CHECK(tc_jr_find(UDSP_SLIC_FAKE_LINEFEED_SET, 0,
					 UDSP_LF_FWD_ACTIVE, 0) > 0);
			CHECK(tc_jr_find(UDSP_SLIC_FAKE_LINEFEED_SET, 0,
					 UDSP_LF_OPEN, 0) > 0);
			tc_free(&t);
		}
		udsp_tdm_override(NULL);
	}

	CASE("job completions carry seq 0 and hardware events count from 1");
	{
		unsigned prev = 0;
		bool rising = true;
		bool jobs = false;

		CHECK(eh_ev_len > 0);
		for (unsigned i = 0; i < eh_ev_len; i++) {
			if (eh_ev[i].type == UDSP_EV_JOB_DONE) {
				jobs = true;
				if (eh_ev[i].seq != 0)
					rising = false;
				continue;
			}
			if (eh_ev[i].seq <= prev)
				rising = false;
			prev = eh_ev[i].seq;
		}
		CHECK(jobs);
		CHECK(rising);
		CHECK(eh_ev_find(UDSP_EV_HOOK, 0) != NULL);
	}

	CASE("the file backend runs the lifecycle under UDSP_MAX_TICKS");
	{
		struct udsp_counters ctr;

		udsp_slic_fake_reset();
		setenv("UDSP_MAX_TICKS", "4000000", 1);
		tc_cfg(&cfg, "file", "USA");
		CHECK(tc_open_handle(&t, &cfg));
		if (t.s) {
			CHECK(tc_start(&t));
			CHECK(tc_stop(&t));
			udsp_counters(t.s, &ctr);
			CHECK(ctr.events_sent > 0);
			tc_free(&t);
		}
		unsetenv("UDSP_MAX_TICKS");
	}

	CASE("an unpaced backend without a tick bound refuses to open");
	{
		unsetenv("UDSP_MAX_TICKS");
		udsp_slic_fake_reset();
		tc_cfg(&cfg, "file", "USA");
		CHECK(!tc_open_handle(&t, &cfg));
		tc_free(&t);
	}

	/* The library end to end on the raw ALSA back end, against the model of
	 * the PCM core in tdm_raw_fake.h. The name in the configuration picks
	 * the back end: no ops override runs here, so the case fails if either
	 * the validator or the name table loses "alsa-raw". Only the six system
	 * calls are replaced. The model paces the tick from its own wait, so
	 * the engine runs at CPU speed and fifty ticks come at once. The wait
	 * is on the tick counter, not on eh_wait_ms, because that helper reads
	 * a counter the instrumented back end owns and this back end never
	 * touches. The journal and the counters are read after tc_free, the
	 * quiescence rule of this file's header. */
	CASE("the library runs a tick loop on the raw ALSA back end");
	{
		struct engine_stats st;

		unsetenv("UDSP_MAX_TICKS");
		fk_reset(&fk_an7581);
		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_tdm_override(NULL);
		tc_cfg(&cfg, "alsa-raw", "USA");
		cfg.sample_rate = 8000;
		snprintf(cfg.tdm_playback, sizeof(cfg.tdm_playback), "hw:0,0");
		snprintf(cfg.tdm_capture, sizeof(cfg.tdm_capture), "hw:0,1");
		memset(&st, 0, sizeof(st));
		CHECK(tc_open_handle(&t, &cfg));
		if (t.s) {
			uint64_t deadline;

			/* alsa-raw defers the DMA start to the start job so the
			 * SLIC init runs with no capture stream to reset, so the
			 * tick loop runs after start, not after open. */
			CHECK(tc_start(&t));
			deadline = eh_mono_us() + 2000000u;

			do {
				tc_engine_stats(&t, &st);
				if (st.ticks >= 50)
					break;
				eh_sleep_ms(1);
			} while (eh_mono_us() < deadline);
			CHECK(st.ticks >= 50);
			CHECK(st.paced);
			CHECK(!st.stopped_fatal);
			CHECK_UINT(st.xruns, 0);
			CHECK_UINT(st.read_errors, 0);
			CHECK_UINT(st.write_errors, 0);
			CHECK_UINT(st.delay_errors, 0);
			CHECK_UINT(st.avail_errors, 0);
			CHECK_UINT(st.capture_stalls, 0);
			CHECK_UINT(st.rate, UDSP_SAMPLE_RATE);
			CHECK_UINT(st.frames_per_period, 80);
			CHECK_UINT(st.granted_periods, 4);
			CHECK_UINT(st.prefill_periods, 3);
		}
		tc_free(&t);
		CHECK_UINT(fk_avail_min_unset, 0);
		CHECK_UINT(fk_start_unprepared, 0);
		/* The journal holds the opening sequence; a free-running tick
		 * loop fills the rest of it, so the close is checked through
		 * the model's link flag. */
		CHECK(!strncmp(fk_journal, "LPpppSaaaeE", 11));
		CHECK(!fk_linked);
		fk_release();
	}

	/* The AN7581 back end masters PCLK/FSYNC from the prepared block with
	 * the DMA off, so the reorder keeps the SLIC init clear of a capture
	 * stream that could reset the PCM block mid-init. */
	CASE("the raw back end defers its DMA start until the SLIC init");
	{
		struct engine_stats st;

		unsetenv("UDSP_MAX_TICKS");
		fk_reset(&fk_an7581);
		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_tdm_override(NULL);
		tc_cfg(&cfg, "alsa-raw", "USA");
		cfg.sample_rate = 8000;
		snprintf(cfg.tdm_playback, sizeof(cfg.tdm_playback), "hw:0,0");
		snprintf(cfg.tdm_capture, sizeof(cfg.tdm_capture), "hw:0,1");
		memset(&st, 0, sizeof(st));
		CHECK(tc_open_handle(&t, &cfg));
		if (t.s) {
			uint64_t deadline;

			/* Open prepares the block, so the clock runs, but starts
			 * neither the DMA nor the tick. */
			tc_engine_stats(&t, &st);
			CHECK_UINT(st.ticks, 0);
			CHECK(!st.running);
			CHECK(strchr(fk_journal, 'P') != NULL);
			CHECK(strchr(fk_journal, 'S') == NULL);

			/* The start job runs the SLIC init, then the DMA; the
			 * running tick and the started backend confirm it, so the
			 * journal is read after tc_free with the threads joined. */
			CHECK(tc_start(&t));

			deadline = eh_mono_us() + 2000000u;
			do {
				tc_engine_stats(&t, &st);
				if (st.ticks >= 50)
					break;
				eh_sleep_ms(1);
			} while (eh_mono_us() < deadline);
			CHECK(st.ticks >= 50);
			CHECK(st.running);
		}
		tc_free(&t);
		CHECK(strchr(fk_journal, 'S') != NULL);
		fk_release();
	}

	/* A start that never runs (the SLIC init fails all lines) closes a
	 * deferred engine: the parked tick must exit and the prepared, unstarted
	 * stream must stop cleanly. */
	CASE("closing a deferred raw engine that never started stops cleanly");
	{
		struct engine_stats st;

		unsetenv("UDSP_MAX_TICKS");
		fk_reset(&fk_an7581);
		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_tdm_override(NULL);
		tc_cfg(&cfg, "alsa-raw", "USA");
		cfg.sample_rate = 8000;
		snprintf(cfg.tdm_playback, sizeof(cfg.tdm_playback), "hw:0,0");
		snprintf(cfg.tdm_capture, sizeof(cfg.tdm_capture), "hw:0,1");
		memset(&st, 0, sizeof(st));
		CHECK(tc_open_handle(&t, &cfg));
		if (t.s) {
			tc_engine_stats(&t, &st);
			CHECK_UINT(st.ticks, 0);
			CHECK(!st.running);
		}
		tc_free(&t);
		fk_release();
	}

	/* A stop that supersedes the not-yet-run start meets a deferred engine
	 * whose tick is parked; it must complete cleanly, not wait out the
	 * safe-pass ack deadline. */
	CASE("stopping a deferred raw engine that never started is clean");
	{
		unsetenv("UDSP_MAX_TICKS");
		fk_reset(&fk_an7581);
		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_tdm_override(NULL);
		tc_cfg(&cfg, "alsa-raw", "USA");
		cfg.sample_rate = 8000;
		snprintf(cfg.tdm_playback, sizeof(cfg.tdm_playback), "hw:0,0");
		snprintf(cfg.tdm_capture, sizeof(cfg.tdm_capture), "hw:0,1");
		CHECK(tc_open_handle(&t, &cfg));
		if (t.s)
			CHECK(tc_stop(&t));
		tc_free(&t);
		fk_release();
	}

	/* A tick op posted before a deferred engine starts cannot be acked, so
	 * the guard refuses it fast. Without the refusal the sync waits out the
	 * 100 ms tick deadline and the config is lost; the elapsed bound catches
	 * that wait. */
	CASE("a channel config before the deferred start is transient");
	{
		struct udsp_chan_cfg ccfg;
		uint64_t t0;

		unsetenv("UDSP_MAX_TICKS");
		fk_reset(&fk_an7581);
		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_tdm_override(NULL);
		tc_cfg(&cfg, "alsa-raw", "USA");
		cfg.sample_rate = 8000;
		snprintf(cfg.tdm_playback, sizeof(cfg.tdm_playback), "hw:0,0");
		snprintf(cfg.tdm_capture, sizeof(cfg.tdm_capture), "hw:0,1");
		CHECK(tc_open_handle(&t, &cfg));
		if (t.s) {
			memset(&ccfg, 0, sizeof(ccfg));
			ccfg.slic_gain_tx = -6;
			ccfg.slic_gain_rx = -18;
			t0 = eh_mono_us();
			CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_TRANSIENT);
			CHECK(eh_mono_us() - t0 < 50000u);
		}
		tc_free(&t);
		fk_release();
	}

	/* The raw back end refuses more than alsa-lib does, and none of those
	 * refusals is the device's answer about the rate: the open must fail
	 * at 16 kHz with the call and the errno on the sink, not degrade to
	 * 8 kHz and report a rate refusal that never happened (tdm.h Refusal,
	 * engine.h enum engine_open_fail). The playback HW_PARAMS count is
	 * the oracle for the retry: a degrade would open the device twice. */
	CASE("a raw refusal that is not the rate fails at 16 kHz with its reason");
	{
		unsetenv("UDSP_MAX_TICKS");
		fk_reset(&fk_an7581);
		fk_info_no_mmap = true;
		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_tdm_override(NULL);
		tc_cfg(&cfg, "alsa-raw", "USA");
		cfg.sample_rate = 16000;
		snprintf(cfg.tdm_playback, sizeof(cfg.tdm_playback), "hw:0,0");
		snprintf(cfg.tdm_capture, sizeof(cfg.tdm_capture), "hw:0,1");
		CHECK(!tc_open_handle(&t, &cfg));
		tc_free(&t);
		CHECK_UINT(fk_hw_params_n[FK_PLAY], 1);
		CHECK_UINT(fk_fd_live, 0);
		CHECK(tc_log_has(UDSP_LOG_ERR,
				 "tdm alsa-raw hw:0,0 hw:0,1 at 16000 Hz: "
				 "the engine did not open "
				 "(playback no mmap access: Invalid argument)"));
		CHECK_UINT(tc_log_count(UDSP_LOG_WARN), 0);

		/* A node another process holds, or a missing one, is a common
		 * case on a target and reads as itself. */
		fk_reset(&fk_an7581);
		fk_open_errno = ENOENT;
		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_tdm_override(NULL);
		tc_cfg(&cfg, "alsa-raw", "USA");
		cfg.sample_rate = 16000;
		snprintf(cfg.tdm_playback, sizeof(cfg.tdm_playback), "hw:0,0");
		snprintf(cfg.tdm_capture, sizeof(cfg.tdm_capture), "hw:0,1");
		CHECK(!tc_open_handle(&t, &cfg));
		tc_free(&t);
		CHECK_UINT(fk_hw_params_n[FK_PLAY], 0);
		CHECK(tc_log_has(UDSP_LOG_ERR,
				 "(playback open: No such file or directory)"));
		CHECK_UINT(tc_log_count(UDSP_LOG_WARN), 0);
		fk_release();
	}
}

static void case_cap(void)
{
	struct udsp_config cfg;
	struct eh t;

	CASE("cap lines is min of slic channels and populated ports");
	{
		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_tdm_override(&tc_ops);
		udsp_slic_fake_channels_set(2);
		tc_cfg(&cfg, "tc", NULL);
		cfg.populated_ports = 1;
		CHECK(tc_open_handle(&t, &cfg));
		if (t.s) {
			CHECK_UINT(t.s->cap.lines, 1);
			CHECK_UINT(t.s->cap.streams_per_chan, 2);
			CHECK(!strcmp(t.s->cap.backend, "fake"));
			CHECK(!strcmp(t.s->cap.slic, "fake"));
			CHECK(!strcmp(t.s->cap.version, "libudsp " UDSP_VERSION " fake"));
			CHECK((t.s->cap.features & UDSP_FEAT_POLARITY) != 0);
			CHECK((t.s->cap.codecs & (1u << UDSP_SEL_PCMU)) != 0);
		}
		tc_free(&t);

		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_slic_fake_channels_set(1);
		tc_cfg(&cfg, "tc", NULL);
		cfg.populated_ports = 2;
		CHECK(tc_open_handle(&t, &cfg));
		if (t.s)
			CHECK_UINT(t.s->cap.lines, 1);
		tc_free(&t);
	}

	/* A reset channel carries the line rate and its frame count; a
	 * channel the open never touched stays the zero of the allocation,
	 * which is the memory the two-line board never pays for. */
	CASE("open resets the channels the board has and no other");
	{
		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_slic_fake_channels_set(2);
		tc_cfg(&cfg, "tc", NULL);
		cfg.populated_ports = 1;
		CHECK(tc_open_handle(&t, &cfg));
		if (t.s) {
			CHECK_UINT(t.s->chan[0].frame, UDSP_FRAME_SAMPLES);
			CHECK_UINT(t.s->chan[0].rate, UDSP_SAMPLE_RATE);
			CHECK_UINT(t.s->chan[1].frame, 0);
			CHECK_UINT(t.s->chan[1].rate, 0);
			CHECK_UINT(t.s->chan[UDSP_LINE_MAX - 1].frame, 0);
		}
		tc_free(&t);

		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_slic_fake_channels_set(1);
		tc_cfg(&cfg, "tc", NULL);
		cfg.populated_ports = 2;
		CHECK(tc_open_handle(&t, &cfg));
		if (t.s) {
			CHECK_UINT(t.s->chan[0].frame, UDSP_FRAME_SAMPLES);
			CHECK_UINT(t.s->chan[1].frame, 0);
		}
		tc_free(&t);

		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_slic_fake_channels_set(2);
		tc_cfg(&cfg, "tc", NULL);
		cfg.populated_ports = 2;
		CHECK(tc_open_handle(&t, &cfg));
		if (t.s) {
			CHECK_UINT(t.s->chan[1].frame, UDSP_FRAME_SAMPLES);
			CHECK_UINT(t.s->chan[1].line, 1);
			CHECK_UINT(t.s->chan[2].frame, 0);
		}
		tc_free(&t);
	}

	CASE("cap refuses more lines than the tdm geometry grants");
	{
		tc_backend_reset();
		udsp_slic_fake_reset();
		eh_be.channels = 1;
		udsp_slic_fake_channels_set(2);
		tc_cfg(&cfg, "tc", NULL);
		cfg.populated_ports = 2;
		CHECK(!tc_open_handle(&t, &cfg));
		tc_free(&t);
	}

	CASE("cap mwi lamp feature follows the descriptor");
	{
		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_slic_fake_optional_set(UDSP_SLIC_FAKE_OPT_ALL &
					    ~(uint32_t)UDSP_SLIC_FAKE_OPT_MWI_LAMP);
		tc_cfg(&cfg, "tc", NULL);
		CHECK(tc_open_handle(&t, &cfg));
		if (t.s)
			CHECK_UINT(t.s->cap.features & UDSP_FEAT_MWI_LAMP, 0);
		tc_free(&t);

		tc_backend_reset();
		udsp_slic_fake_reset();
		tc_cfg(&cfg, "tc", NULL);
		CHECK(tc_open_handle(&t, &cfg));
		if (t.s)
			CHECK(t.s->cap.features & UDSP_FEAT_MWI_LAMP);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}
}

/*
 * The jitter buffer diagnostics are FASTPATH ops. A bad argument therefore
 * gets its answer on the thread of the caller, and reaches no other thread.
 * This case covers the two shapes that a caller can get wrong on its own. They
 * are no handle at all, and a handle whose streams are all idle.
 */
static void case_jb_debug(void)
{
	struct udsp_jb_trace rec[4];
	struct udsp_jb_stats js;
	struct eh t;

	CASE("the jitter buffer diagnostics refuse a handle with no stream");
	{
		memset(&js, 0xa5, sizeof(js));
		CHECK(udsp_stream_jb_stats(NULL, 0, 0, &js) == UDSP_UNSUPPORTED);
		/* The record is cleared before the refusal, so a caller that
		 * ignores the status still reads zeros. */
		CHECK_UINT(js.frames_out, 0);
		CHECK_UINT(js.target_ms, 0);
		udsp_debug_set(NULL, UDSP_DEBUG_JB_TRACE);
		CHECK_UINT(udsp_jb_trace_read(NULL, rec, 4), 0);

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, NULL) == UDSP_UNSUPPORTED);
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &js) == UDSP_UNSUPPORTED);
		CHECK(udsp_stream_jb_stats(t.s, UDSP_LINE_MAX, 0, &js) ==
		      UDSP_UNSUPPORTED);
		CHECK(udsp_stream_jb_stats(t.s, 0, UDSP_STREAM_MAX, &js) ==
		      UDSP_UNSUPPORTED);
		CHECK_UINT(udsp_jb_trace_read(t.s, NULL, 4), 0);
		CHECK_UINT(udsp_jb_trace_read(t.s, rec, 0), 0);
		CHECK_UINT(udsp_jb_trace_read(t.s, rec, 4), 0);

		udsp_debug_set(t.s, UDSP_DEBUG_JB_TRACE);
		eh_wait_ms(&t, 100);
		/* No stream is configured, so the tick has nothing to trace. */
		CHECK_UINT(udsp_jb_trace_read(t.s, rec, 4), 0);
		udsp_debug_set(t.s, 0);
		tc_down(&t);
	}
}

static void case_hook_query(void)
{
	struct udsp_config cfg;
	struct eh t;
	bool offhook = true;

	CASE("hook query is transient before the first sample then cached");
	{
		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_tdm_override(&tc_ops);
		tc_cfg(&cfg, "tc", "USA");
		CHECK(tc_open_handle(&t, &cfg));
		if (!t.s)
			return;
		/* Sampling starts with the init of the start job, so nothing
		 * has been read yet and the fastpath has no answer. */
		CHECK(udsp_line_hook_query(t.s, 0, &offhook) == UDSP_TRANSIENT);
		CHECK(tc_start(&t));
		eh_wait_ms(&t, 60);
		CHECK(udsp_line_hook_query(t.s, 0, &offhook) == UDSP_OK);
		CHECK(!offhook);

		{
			struct udsp_slic_fake_chan_state st;

			for (unsigned i = 0; i < 50; i++)
				CHECK(udsp_line_hook_query(t.s, 0, &offhook) ==
				      UDSP_OK);
			CHECK(tc_stop(&t));
			CHECK(tc_state(&t, 0, &st));
			/* The sampler reads the hook at most once per tick, so a
			 * count inside the tick budget proves the fifty queries
			 * reached no register (udsp.h: cached, never SPI). */
			CHECK(st.hook_read_count <= eh_now_ms() / 10u + 4u);
			CHECK(st.hook_read_count > 0);
		}
		tc_free(&t);
		udsp_tdm_override(NULL);
	}
}

/* Drives the machines directly with an ideal schedule and returns the hook
 * events they produce, the differential oracle for the engine's own journal. */
struct tc_hook_step { uint32_t ms; bool level; };

static unsigned tc_hook_oracle(const struct udsp_region_hook *timing, bool pulse,
			       uint32_t flash_min, uint32_t flash_max,
			       uint32_t release_min,
			       const struct tc_hook_step *step, unsigned steps,
			       uint32_t end_ms, uint8_t *state, uint8_t *pulses,
			       unsigned max)
{
	struct line_hook h;
	struct line_out out;
	unsigned n = 0;
	unsigned si = 0;
	bool level = false;

	line_hook_init(&h, timing);
	line_hook_pulse_enable(&h, pulse);
	line_hook_timing_set(&h, flash_min, flash_max, release_min);
	for (uint32_t ms = 0; ms <= end_ms; ms += 10) {
		struct line_hook_report rep;

		while (si < steps && step[si].ms <= ms)
			level = step[si++].level;
		line_out_reset(&out);
		rep = line_hook_tick(&h, ms, level, NULL);
		line_hook_emit(&rep, 0, ms, &out);
		for (unsigned i = 0; i < out.count && n < max; i++) {
			state[n] = out.rec[i].u.event.u.hook.state;
			pulses[n] = out.rec[i].u.event.u.hook.pulses;
			n++;
		}
	}

	return n;
}

static unsigned tc_hook_events(uint8_t *state, uint8_t *pulses, unsigned max)
{
	unsigned n = 0;

	for (unsigned i = 0; i < eh_ev_len && n < max; i++) {
		if (eh_ev[i].type != UDSP_EV_HOOK || eh_ev[i].line != 0)
			continue;
		state[n] = eh_ev[i].u.hook.state;
		pulses[n] = eh_ev[i].u.hook.pulses;
		n++;
	}

	return n;
}

static void tc_hook_set(struct eh *t, bool offhook)
{
	udsp_slic_fake_hook_set(t->s->slic, 0, offhook);
}

static unsigned tc_hook_reads(struct eh *t, unsigned line)
{
	struct udsp_slic_fake_chan_state st;

	if (!udsp_slic_fake_chan_state_get(t->s->slic, line, &st))
		return 0;
	return st.hook_read_count;
}

static unsigned tc_fault_reads(struct eh *t, unsigned line)
{
	struct udsp_slic_fake_chan_state st;

	if (!udsp_slic_fake_chan_state_get(t->s->slic, line, &st))
		return 0;
	return st.fault_read_count;
}

/* A wait on the wall clock, for the slic thread's IDLE deadlines, which run
 * on CLOCK_MONOTONIC while the paced engine clock runs ten times faster. */
static void tc_wait_real_ms(struct eh *t, unsigned ms)
{
	uint64_t end = eh_mono_us() + (uint64_t)ms * 1000u;

	while (eh_mono_us() < end) {
		eh_ev_poll(t);
		eh_sleep_ms(1);
	}
	eh_ev_poll(t);
}

static uint8_t tc_hook_last(void)
{
	uint8_t state[16], pulses[16];
	unsigned n = tc_hook_events(state, pulses, 16);

	return n ? state[n - 1] : UDSP_HOOK_ERROR;
}

/* The stamp of the last sample that the slic thread published for line 0. */
static unsigned tc_hook_pub_stamp(struct eh *t)
{
	return (uint32_t)atomic_load_explicit(&t->s->chan[0].hook_pub,
					      memory_order_acquire);
}

/* The stamp and the gap of the last hook record of line 0 at this level, read
 * back from the DEBUG record line of slic_hook_push. */
static bool tc_hook_record_last(bool offhook, unsigned *stamp, unsigned *gap)
{
	bool found = false;

	pthread_mutex_lock(&tc_log.lock);
	for (unsigned i = 0; i < tc_log.n; i++) {
		unsigned at, level, width;

		if (tc_log.level[i] != UDSP_LOG_DEBUG ||
		    sscanf(tc_log.line[i],
			   "hook record line 0 stamp %u level %u gap %u", &at,
			   &level, &width) != 3 ||
		    (level != 0) != offhook)
			continue;
		*stamp = at;
		*gap = width;
		found = true;
	}
	pthread_mutex_unlock(&tc_log.lock);

	return found;
}

static bool tc_hook_sample_wait(struct eh *t, bool offhook, unsigned head)
{
	struct udsp_chan *c = &t->s->chan[0];
	uint64_t end = eh_mono_us() + EH_GATE_WAIT_US;
	uint32_t now_ms = udsp_clock_now_ms(&t->s->clock);
	bool observed = false;

	/* hook_pub precedes the edge push. Its stamp proves this tick was
	 * sampled; the queue head's release store publishes the edge itself.
	 * The paced stamp includes a sub-tick offset, so compare tick bins. */
	do {
		uint64_t pub = atomic_load_explicit(&c->hook_pub,
						     memory_order_acquire);
		unsigned published = atomic_load_explicit(&c->edges.head,
							  memory_order_acquire);

		observed = (pub & UDSP_HOOK_PUB_VALID) &&
			   !!(pub & UDSP_HOOK_PUB_LEVEL) == offhook &&
			   (uint32_t)pub / UDSP_TICK_MS ==
				   now_ms / UDSP_TICK_MS && published == head;
		if (!observed)
			sched_yield();
	} while (!observed && eh_mono_us() < end);
	CHECK(observed);

	return observed;
}

static bool tc_hook_gate_park(struct eh_gate *gate, struct eh *t)
{
	/* Before injection, the boot on-hook sample is the only queued edge. */
	return eh_gate_park(gate) && tc_hook_sample_wait(t, false, 1u);
}

static bool tc_hook_hold(struct eh_gate *gate, struct eh *t, bool offhook,
			 unsigned frames)
{
	struct udsp_chan *c = &t->s->chan[0];
	unsigned head = atomic_load_explicit(&c->edges.head,
					     memory_order_acquire);
	uint64_t pub = atomic_load_explicit(&c->hook_pub, memory_order_acquire);

	if (!!(pub & UDSP_HOOK_PUB_LEVEL) != offhook)
		head = (head + 1u) % UDSP_HOOK_EDGE_DEPTH;
	tc_hook_set(t, offhook);
	for (unsigned i = 0; i < frames; i++) {
		if (!eh_gate_step(gate, 1) ||
		    !tc_hook_sample_wait(t, offhook, head))
			return false;
		eh_ev_poll(t);
	}

	return true;
}

static bool tc_hook_pulse_train(struct eh_gate *gate, struct eh *t,
				unsigned pulses)
{
	for (unsigned i = 0; i < pulses; i++) {
		if (!tc_hook_hold(gate, t, false, 6) ||
		    !tc_hook_hold(gate, t, true, 6))
			return false;
	}

	return true;
}

/*
 * Grants ticks until the machine reports state, at most 500 of them. When the
 * grants come slower than the pace period, the pace deadline falls behind
 * (eh_pace) and the next ticks run with no wait. The 500 ticks can then end
 * before the emitter thread writes the report. After the last tick the wait
 * therefore polls to the wall bound.
 */
static bool tc_hook_report_wait(struct eh_gate *gate, struct eh *t,
				uint8_t state)
{
	uint64_t end = eh_mono_us() + 5000000u;
	uint64_t pub = atomic_load_explicit(&t->s->chan[0].hook_pub,
					     memory_order_acquire);
	bool offhook = (pub & UDSP_HOOK_PUB_LEVEL) != 0;
	uint8_t states[16], pulses[16];
	bool reported = false;

	for (unsigned i = 0; eh_mono_us() < end; i++) {
		unsigned n;

		eh_ev_poll(t);
		n = tc_hook_events(states, pulses, 16);
		reported = n >= 3 && states[n - 1] == state;
		if (reported)
			break;
		if (i >= 500u) {
			eh_sleep_ms(1);
			continue;
		}
		if (!tc_hook_hold(gate, t, offhook, 1))
			break;
	}
	CHECK(reported);

	return reported;
}

/*
 * The sampling modes of the slic thread behind an edge source (slic_thread.h
 * enum slic_mode), with an eventfd in place of the sysfs value node. The fake
 * counts hook_read per line, so the cadence is a number. It is one per tick in
 * BURST, and one per SLIC_IDLE_SAMPLE_MS of wall clock in IDLE. The burst
 * window is in engine time, and the hook windows are those of the USA region
 * (release at 1301 ms). Those waits are therefore engine waits. The IDLE
 * cadence is wall clock, so its waits are real.
 */
static void case_slic_modes(void)
{
	struct eh t;
	uint64_t one = 1;
	uint64_t start;
	unsigned before, after, faults, n;
	int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

	CASE("the slic thread samples at tick rate after start and idles after");
	CHECK(efd >= 0);
	udsp_slic_irq_override(efd, POLLIN);
	if (!tc_up(&t, "USA")) {
		CHECK(false);
		udsp_slic_irq_override(-1, 0);
		close(efd);
		return;
	}
	CHECK_UINT(t.s->slic_mode, SLIC_MODE_BURST);
	before = tc_hook_reads(&t, 0);
	eh_wait_ms(&t, 500);
	after = tc_hook_reads(&t, 0);
	CHECK(after - before >= 10);
	eh_wait_ms(&t, 2100);
	CHECK_UINT(t.s->slic_mode, SLIC_MODE_IDLE);
	before = tc_hook_reads(&t, 0);
	faults = tc_fault_reads(&t, 0);
	tc_wait_real_ms(&t, 1000);
	after = tc_hook_reads(&t, 0);
	CHECK(after - before >= 3);
	CHECK(after - before <= 6);
	/* The fault cadence carries across the transition: one second of
	 * idle time after the burst's last read holds at least one read. */
	tc_wait_real_ms(&t, 150);
	CHECK(tc_fault_reads(&t, 0) - faults >= 1);
	CHECK(tc_fault_reads(&t, 0) - faults <= 2);
	CHECK(udsp_line_hook_query(t.s, 0, &(bool){ false }) == UDSP_OK);

	CASE("an edge in idle is sampled at once and starts a burst");
	eh_ev_poll(&t);
	n = eh_ev_count(UDSP_EV_HOOK);
	udsp_debug_set(t.s, UDSP_DEBUG_SLIC_LOG);
	tc_hook_set(&t, true);
	CHECK(write(efd, &one, sizeof(one)) == (ssize_t)sizeof(one));
	tc_wait_real_ms(&t, 50);
	CHECK(eh_ev_count(UDSP_EV_HOOK) == n + 1);
	CHECK_UINT(tc_hook_last(), UDSP_HOOK_SEIZE);
	/* The record the edge pass pushed carries the IRQ mark, in the mode
	 * the pass ran in (chan.h UDSP_HOOK_EDGE_IRQ). */
	CHECK_UINT(tc_log_hits(UDSP_LOG_DEBUG, "flags 0x08 mode idle"), 1);
	CHECK_UINT(t.s->slic_mode, SLIC_MODE_BURST);
	before = tc_hook_reads(&t, 0);
	eh_wait_ms(&t, 500);
	after = tc_hook_reads(&t, 0);
	CHECK(after - before >= 10);

	CASE("a level change with no edge is caught by the safety sample");
	eh_wait_ms(&t, 2100);
	CHECK_UINT(t.s->slic_mode, SLIC_MODE_IDLE);
	eh_ev_poll(&t);
	n = eh_ev_count(UDSP_EV_HOOK);
	/* The open is found by a safety sample within SLIC_IDLE_SAMPLE_MS and
	 * the burst it starts carries the release window at tick rate. */
	tc_hook_set(&t, false);
	tc_wait_real_ms(&t, 600);
	CHECK(eh_ev_count(UDSP_EV_HOOK) == n + 1);
	CHECK_UINT(tc_hook_last(), UDSP_HOOK_RELEASE);
	eh_wait_ms(&t, 2100);
	CHECK_UINT(t.s->slic_mode, SLIC_MODE_IDLE);
	tc_hook_set(&t, true);
	tc_wait_real_ms(&t, 400);
	CHECK(eh_ev_count(UDSP_EV_HOOK) == n + 2);
	CHECK_UINT(tc_hook_last(), UDSP_HOOK_SEIZE);

	CASE("records posted in idle are executed within the sync deadline");
	eh_wait_ms(&t, 2100);
	CHECK_UINT(t.s->slic_mode, SLIC_MODE_IDLE);
	before = tc_hook_reads(&t, 0);
	/* Forty records, each acked by the pushed-record wake alone: a wake
	 * the engine and the thread lost between them would wait for the
	 * 250 ms safety pass and fail the bound. */
	for (n = 0; n < 20; n++) {
		start = eh_mono_us();
		CHECK(udsp_line_polarity(t.s, 0, UDSP_POL_REVERSE) == UDSP_OK);
		CHECK(udsp_line_polarity(t.s, 0, UDSP_POL_FORWARD) == UDSP_OK);
		CHECK(eh_mono_us() - start < 100000u);
	}
	/* Executed by the pushed-record wake, which starts no burst. */
	tc_wait_real_ms(&t, 300);
	CHECK_UINT(t.s->slic_mode, SLIC_MODE_IDLE);
	CHECK(tc_hook_reads(&t, 0) - before <= 4);

	CASE("a lost edge source falls back to the per-tick pass");
	/* The closed descriptor reports POLLNVAL to the wait; the thread
	 * counts it once and runs the pass of a board with no edge source. */
	close(efd);
	tc_wait_real_ms(&t, 400);
	CHECK_UINT(t.s->slic_mode, SLIC_MODE_POLL);
	before = tc_hook_reads(&t, 0);
	eh_wait_ms(&t, 500);
	CHECK(tc_hook_reads(&t, 0) - before >= 10);

	tc_down(&t);
	udsp_slic_irq_override(-1, 0);
}

/*
 * The hook sampling record of udsp_counters (slic_thread.c
 * slic_hook_gap_note). If the fake holds a hook read for longer than a tick,
 * the slic thread stalls inside its pass. The next sample of the line then
 * comes late on the wall clock. The gap counts, its width is kept, and a gap
 * wide enough to move a pulse logs once. The fake has no edge source, so the
 * thread runs POLL, the per-tick mode that the record measures. In IDLE, the
 * cadence is that of the safety sample, and nothing counts.
 */
static void case_hook_gaps(void)
{
	struct eh t;
	struct udsp_counters c;
	uint64_t gaps;
	int efd;

	CASE("a late hook sample counts a gap and keeps its width");
	if (!tc_up(&t, "USA")) {
		CHECK(false);
		return;
	}
	eh_wait_ms(&t, 100);
	udsp_counters(t.s, &c);
	CHECK_UINT(c.hook_gaps, 0);
	CHECK_UINT(c.hook_gap_max_ms, 0);
	CHECK_UINT(c.hook_moved_gaps, 0);
	udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_HOOK_READ, 60);
	tc_wait_real_ms(&t, 400);
	udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_HOOK_READ, 0);
	udsp_counters(t.s, &c);
	CHECK(c.hook_gaps >= 2);
	CHECK(c.hook_gap_max_ms >= 60);
	CHECK_UINT(c.hook_moved_gaps, 0);
	CHECK(tc_log_hits(UDSP_LOG_WARN, "came") >= 1);
	tc_down(&t);

	CASE("the IDLE cadence counts no gap");
	efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	CHECK(efd >= 0);
	udsp_slic_irq_override(efd, POLLIN);
	if (!tc_up(&t, "USA")) {
		CHECK(false);
		udsp_slic_irq_override(-1, 0);
		close(efd);
		return;
	}
	eh_wait_ms(&t, 2100);
	CHECK_UINT(t.s->slic_mode, SLIC_MODE_IDLE);
	udsp_counters(t.s, &c);
	gaps = c.hook_gaps;
	tc_wait_real_ms(&t, 1000);
	udsp_counters(t.s, &c);
	CHECK_UINT(c.hook_gaps, gaps);
	tc_down(&t);
	udsp_slic_irq_override(-1, 0);
	close(efd);
}

static void case_hook(void)
{
	static const struct udsp_region_hook usa = {
		.pulse_enable = true, .debounce_ms = 10,
		.flash_min_ms = 40, .flash_max_ms = 200, .release_min_ms = 1301,
		.pulse_break_min_ms = 33, .pulse_break_max_ms = 100,
		.pulse_make_max_ms = 85, .pulse_interdigit_ms = 300,
	};
	uint8_t got_state[16], got_pulse[16];
	uint8_t want_state[16], want_pulse[16];
	struct udsp_chan_cfg ccfg;
	struct eh t;

	CASE("hook boot report and seize match the machine oracle");
	{
		static const struct tc_hook_step step[] = {
			{ 200, true }, { 1600, false },
		};
		struct eh_gate gate = { .fd = { -1, -1 } };
		unsigned got, want;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_PULSE;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(udsp_line_hook_timing(t.s, 0, 40, 200, 1301) == UDSP_OK);
		if (!tc_hook_gate_park(&gate, &t) ||
		    !tc_hook_hold(&gate, &t, false, 20) ||
		    !tc_hook_hold(&gate, &t, true, 140) ||
		    !tc_hook_hold(&gate, &t, false, 160) ||
		    !tc_hook_report_wait(&gate, &t, UDSP_HOOK_RELEASE))
			goto hook_boot_out;

		{
			uint32_t end_ms = eh_now_ms();
			uint32_t prev = 0;
			bool bounded = true;

			for (unsigned i = 0; i < eh_ev_len; i++) {
				if (eh_ev[i].type != UDSP_EV_HOOK)
					continue;
				/* Every stamp is in the engine base and the
				 * sequence never goes backwards. */
				if (!eh_ev[i].stamp_ms ||
				    eh_ev[i].stamp_ms > end_ms ||
				    eh_ev[i].stamp_ms < prev)
					bounded = false;
				prev = eh_ev[i].stamp_ms;
			}
			CHECK(bounded);
			CHECK(prev > 0);
		}
		got = tc_hook_events(got_state, got_pulse, 16);
		want = tc_hook_oracle(&usa, true, 40, 200, 1301, step, 2, 3400,
				      want_state, want_pulse, 16);
		CHECK_UINT(got, want);
		CHECK(got >= 3);
		if (got == want && got)
			CHECK(memcmp(got_state, want_state, got) == 0);
		CHECK_UINT(got_state[0], UDSP_HOOK_RELEASE);
		CHECK_UINT(got_state[1], UDSP_HOOK_SEIZE);
		CHECK_UINT(got_state[2], UDSP_HOOK_RELEASE);
	hook_boot_out:
		eh_gate_disarm(&gate);
		tc_down(&t);
		eh_gate_close(&gate);
	}

	CASE("a flash is reported when pulse classification is off");
	{
		static const struct tc_hook_step step[] = {
			{ 200, true }, { 700, false }, { 800, true },
		};
		struct eh_gate gate = { .fd = { -1, -1 } };
		unsigned got, want;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		memset(&ccfg, 0, sizeof(ccfg));
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(udsp_line_hook_timing(t.s, 0, 40, 200, 1301) == UDSP_OK);
		if (!tc_hook_gate_park(&gate, &t) ||
		    !tc_hook_hold(&gate, &t, false, 20) ||
		    !tc_hook_hold(&gate, &t, true, 50) ||
		    !tc_hook_hold(&gate, &t, false, 10) ||
		    !tc_hook_hold(&gate, &t, true, 50) ||
		    !tc_hook_report_wait(&gate, &t, UDSP_HOOK_FLASH))
			goto hook_flash_out;

		got = tc_hook_events(got_state, got_pulse, 16);
		want = tc_hook_oracle(&usa, false, 40, 200, 1301, step, 3, 1300,
				      want_state, want_pulse, 16);
		CHECK_UINT(got, want);
		if (got == want && got)
			CHECK(memcmp(got_state, want_state, got) == 0);
		CHECK(got >= 3);
		CHECK_UINT(got_state[2], UDSP_HOOK_FLASH);
	hook_flash_out:
		eh_gate_disarm(&gate);
		tc_down(&t);
		eh_gate_close(&gate);
	}

	/*
	 * The window uvoice requires, straight from the region file, against
	 * the real sampler. 30 sample periods of open measure about 290 ms.
	 * The closing gap credit and the lag tolerance then reach flash_min_ms
	 * (hook.c, "Windows").
	 * The oracle runs an ideal sampler with no gap, so it has nothing to
	 * say about this case and is not called.
	 */
	CASE("a flash of 300 ms is reported at the region window");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		unsigned open_at, open_gap, close_at, close_gap, lo;
		unsigned open_prev = 0, close_prev = 0;
		char trace[64];
		unsigned got;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		memset(&ccfg, 0, sizeof(ccfg));
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK_UINT(t.s->chan[0].hook.timing.flash_min_ms, 300);
		udsp_debug_set(t.s, UDSP_DEBUG_SLIC_LOG);
		if (!tc_hook_gate_park(&gate, &t) ||
		    !tc_hook_hold(&gate, &t, false, 20) ||
		    !tc_hook_hold(&gate, &t, true, 50))
			goto hook_window_out;
		open_prev = tc_hook_pub_stamp(&t);
		if (!tc_hook_hold(&gate, &t, false, 30))
			goto hook_window_out;
		close_prev = tc_hook_pub_stamp(&t);
		if (!tc_hook_hold(&gate, &t, true, 50) ||
		    !tc_hook_report_wait(&gate, &t, UDSP_HOOK_FLASH))
			goto hook_window_out;

		got = tc_hook_events(got_state, got_pulse, 16);
		CHECK_UINT(got, 3);
		if (got < 3)
			goto hook_window_out;
		CHECK_UINT(got_state[0], UDSP_HOOK_RELEASE);
		CHECK_UINT(got_state[1], UDSP_HOOK_SEIZE);
		CHECK_UINT(got_state[2], UDSP_HOOK_FLASH);
		/*
		 * The slic thread logs the open line of the hook trace behind
		 * the tick that closed the open. The line holds the measured
		 * bounds, the gap of each edge and the class. A stamp carries
		 * the offset of its sample inside the tick (clock.h
		 * udsp_clock_stamp_ms), and a loaded host makes the offset
		 * larger. The case therefore builds the expected line from the
		 * two records. It checks each record gap against the sample
		 * stamp that the test read before the edge. Samples on the tick
		 * boundary give "lo 290 hi 350 gap 10/10 flash". Two gated
		 * samples fall in adjacent tick bins, so a record gap is at
		 * most 19 ms. The 30 ms cap of the credit (hook.c gap_credit)
		 * therefore does not apply.
		 */
		if (!tc_hook_record_last(false, &open_at, &open_gap) ||
		    !tc_hook_record_last(true, &close_at, &close_gap)) {
			CHECK(false);
			goto hook_window_out;
		}
		CHECK_UINT(open_gap, open_at - open_prev);
		CHECK_UINT(close_gap, close_at - close_prev);
		CHECK_UINT(close_at / UDSP_TICK_MS - open_at / UDSP_TICK_MS,
			   30);
		lo = close_at - close_gap - open_at;
		snprintf(trace, sizeof(trace), "lo %u hi %u gap %u/%u flash",
			 lo, lo + close_gap + TC_HOOK_FLASH_TOL_MS, open_gap,
			 close_gap);
		CHECK_UINT(tc_log_hits(UDSP_LOG_DEBUG, trace), 1);
	hook_window_out:
		eh_gate_disarm(&gate);
		tc_down(&t);
		eh_gate_close(&gate);
	}

	CASE("a pulse train reports its digit when pulse classification is on");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		unsigned got;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_PULSE;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(udsp_line_hook_timing(t.s, 0, 40, 200, 1301) == UDSP_OK);
		if (!tc_hook_gate_park(&gate, &t) ||
		    !tc_hook_hold(&gate, &t, false, 20) ||
		    !tc_hook_hold(&gate, &t, true, 30) ||
		    !tc_hook_pulse_train(&gate, &t, 3) ||
		    !tc_hook_report_wait(&gate, &t, UDSP_HOOK_PULSE))
			goto hook_pulse_out;

		got = tc_hook_events(got_state, got_pulse, 16);
		CHECK(got >= 3);
		CHECK_UINT(got_state[0], UDSP_HOOK_RELEASE);
		CHECK_UINT(got_state[1], UDSP_HOOK_SEIZE);
		CHECK_UINT(got_state[2], UDSP_HOOK_PULSE);
		CHECK_UINT(got_pulse[2], 3);
	hook_pulse_out:
		eh_gate_disarm(&gate);
		tc_down(&t);
		eh_gate_close(&gate);
	}
}

/* The count of hook events of one state on line 0. */
static unsigned tc_hook_state_count(uint8_t state)
{
	uint8_t st[32], pu[32];
	unsigned n = tc_hook_events(st, pu, 32);
	unsigned count = 0;

	for (unsigned i = 0; i < n; i++)
		count += st[i] == state;

	return count;
}

/*
 * The decadic coding of the last pulse event on line 0, or UINT8_MAX when the
 * line reported no pulse. others counts the other hook events of line 0 that
 * carry a coding.
 */
static uint8_t tc_hook_coding(unsigned *others)
{
	uint8_t coding = UINT8_MAX;

	*others = 0;
	for (unsigned i = 0; i < eh_ev_len; i++) {
		const struct udsp_event *ev = &eh_ev[i];

		if (ev->type != UDSP_EV_HOOK || ev->line != 0)
			continue;
		if (ev->u.hook.state == UDSP_HOOK_PULSE)
			coding = ev->u.hook.pulse_invert;
		else
			*others += ev->u.hook.pulse_invert != 0;
	}

	return coding;
}

struct tc_hook_want {
	uint8_t state;
	unsigned count;
};

static bool tc_hook_state_reached(struct eh *t, const void *arg)
{
	const struct tc_hook_want *w = arg;

	(void)t;
	return tc_hook_state_count(w->state) >= w->count;
}

/*
 * The engine posts a hook report at the tick that makes it, and the emitter
 * thread writes the report to the socket later. On a loaded host the test can
 * grant the last ticks of a case before the emitter thread runs. A case
 * therefore waits for the report it expects before it counts. The emitter
 * keeps the order of the engine reports, so every report made before the
 * awaited one has arrived too. A report made after it can still be in
 * transit, so on a loaded host a count that must stay zero proves less.
 */
static bool tc_hook_state_wait(struct eh *t, uint8_t state, unsigned count)
{
	struct tc_hook_want w = { .state = state, .count = count };

	return tc_poll_until(t, tc_hook_state_reached, &w, 5000);
}

static bool tc_hook_boot_wait(struct eh *t);

/*
 * Plants toggles the level does not show and grants the two ticks the slic
 * thread needs to hold and then resolve the candidate (slic_thread.c
 * slic_gap_resolve). The head moves on the second grant alone, when the
 * confirmed GAP record is pushed; the level stays where it is.
 */
static bool tc_hook_gap_inject(struct eh_gate *gate, struct eh *t,
			       unsigned toggles, bool confirmed)
{
	struct udsp_chan *c = &t->s->chan[0];
	unsigned head = atomic_load_explicit(&c->edges.head,
					     memory_order_acquire);
	uint64_t pub = atomic_load_explicit(&c->hook_pub, memory_order_acquire);
	bool level = (pub & UDSP_HOOK_PUB_LEVEL) != 0;

	udsp_slic_fake_hook_inject(t->s->slic, 0, level, toggles);
	if (!eh_gate_step(gate, 1) || !tc_hook_sample_wait(t, level, head))
		return false;
	if (confirmed)
		head = (head + 1u) % UDSP_HOOK_EDGE_DEPTH;
	if (!eh_gate_step(gate, 1) || !tc_hook_sample_wait(t, level, head))
		return false;
	eh_ev_poll(t);

	return true;
}

/*
 * The gap verdict of the sampler through the engine. A lost pair that the
 * part recorded drops the train with one UDSP_HOOK_ERROR. A stall of the
 * sampler inside a train does the same, and never flushes a partial digit. A
 * stall inside a break reports no release.
 * The race between the two reads of the part produces one clean edge. A
 * plug-in without the change record still classifies a clean train.
 */
static void case_hook_gap(void)
{
	struct udsp_chan_cfg ccfg;
	struct udsp_counters c;
	struct udsp_config cfg;
	struct eh t;

	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.detect_mask = UDSP_DET_PULSE;

	CASE("a lost pair inside a train drops the train with one error");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(udsp_line_hook_timing(t.s, 0, 40, 200, 1301) == UDSP_OK);
		/* Two clean pulses, then a pair the level never shows, then two
		 * more: the digit would have read four with the pair, and reads
		 * as nothing. A clean train after the interdigit gap counts. */
		if (!tc_hook_gate_park(&gate, &t) ||
		    !tc_hook_hold(&gate, &t, false, 20) ||
		    !tc_hook_hold(&gate, &t, true, 30) ||
		    !tc_hook_pulse_train(&gate, &t, 2) ||
		    !tc_hook_gap_inject(&gate, &t, 2, true) ||
		    !tc_hook_pulse_train(&gate, &t, 2) ||
		    !tc_hook_hold(&gate, &t, true, 40) ||
		    !tc_hook_pulse_train(&gate, &t, 3) ||
		    !tc_hook_report_wait(&gate, &t, UDSP_HOOK_PULSE))
			goto gap_pair_out;
		CHECK_UINT(tc_hook_state_count(UDSP_HOOK_ERROR), 1);
		CHECK_UINT(tc_hook_state_count(UDSP_HOOK_PULSE), 1);
		CHECK_UINT(tc_hook_last(), UDSP_HOOK_PULSE);
		udsp_counters(t.s, &c);
		CHECK_UINT(c.hook_moved_gaps, 1);
	gap_pair_out:
		eh_gate_disarm(&gate);
		tc_down(&t);
		eh_gate_close(&gate);
	}

	CASE("a lost pair on a seized idle line drops the next train only");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(udsp_line_hook_timing(t.s, 0, 40, 200, 1301) == UDSP_OK);
		/* The pair falls before any pulse: the error is reported at
		 * once, the interdigit gap that follows ends the poisoned
		 * train, and the digit dialed after it counts. */
		if (!tc_hook_gate_park(&gate, &t) ||
		    !tc_hook_hold(&gate, &t, false, 20) ||
		    !tc_hook_hold(&gate, &t, true, 30) ||
		    !tc_hook_gap_inject(&gate, &t, 2, true) ||
		    !tc_hook_hold(&gate, &t, true, 40) ||
		    !tc_hook_pulse_train(&gate, &t, 2) ||
		    !tc_hook_report_wait(&gate, &t, UDSP_HOOK_PULSE))
			goto gap_idle_out;
		CHECK_UINT(tc_hook_state_count(UDSP_HOOK_ERROR), 1);
		CHECK_UINT(tc_hook_state_count(UDSP_HOOK_PULSE), 1);
	gap_idle_out:
		eh_gate_disarm(&gate);
		tc_down(&t);
		eh_gate_close(&gate);
	}

	CASE("a lost pair on an on-hook line is a glitch and reports nothing");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		unsigned before;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		if (!tc_hook_gate_park(&gate, &t) ||
		    !tc_hook_hold(&gate, &t, false, 20))
			goto gap_onhook_out;
		CHECK(tc_hook_boot_wait(&t));
		before = eh_ev_count(UDSP_EV_HOOK);
		if (!tc_hook_gap_inject(&gate, &t, 2, true) ||
		    !tc_hook_hold(&gate, &t, false, 5))
			goto gap_onhook_out;
		CHECK_UINT(eh_ev_count(UDSP_EV_HOOK), before);
		udsp_counters(t.s, &c);
		CHECK_UINT(c.hook_moved_gaps, 1);
	gap_onhook_out:
		eh_gate_disarm(&gate);
		tc_down(&t);
		eh_gate_close(&gate);
	}

	CASE("the race between the part's two reads is one clean edge");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct udsp_chan *ch;
		unsigned head;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		ch = &t.s->chan[0];
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		/* moved with the old level, then the new level with no move:
		 * the candidate is dropped and the seize goes out one pass
		 * late, with no error. */
		if (!tc_hook_gate_park(&gate, &t) ||
		    !tc_hook_hold(&gate, &t, false, 20))
			goto gap_race_out;
		head = atomic_load_explicit(&ch->edges.head, memory_order_acquire);
		udsp_slic_fake_hook_inject(t.s->slic, 0, false, 1);
		if (!eh_gate_step(&gate, 1) ||
		    !tc_hook_sample_wait(&t, false, head))
			goto gap_race_out;
		udsp_slic_fake_hook_inject(t.s->slic, 0, true, 0);
		head = (head + 1u) % UDSP_HOOK_EDGE_DEPTH;
		if (!eh_gate_step(&gate, 1) ||
		    !tc_hook_sample_wait(&t, true, head) ||
		    !tc_hook_hold(&gate, &t, true, 5))
			goto gap_race_out;
		CHECK(tc_hook_state_wait(&t, UDSP_HOOK_SEIZE, 1));
		CHECK_UINT(tc_hook_last(), UDSP_HOOK_SEIZE);
		CHECK_UINT(tc_hook_state_count(UDSP_HOOK_ERROR), 0);
		udsp_counters(t.s, &c);
		CHECK_UINT(c.hook_moved_gaps, 0);
	gap_race_out:
		eh_gate_disarm(&gate);
		tc_down(&t);
		eh_gate_close(&gate);
	}

	CASE("a sampler stalled inside a train drops it instead of a wrong digit");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct udsp_job job = { 0 };
		int32_t status = 0;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(udsp_line_hook_timing(t.s, 0, 40, 200, 1301) == UDSP_OK);
		/* Two pulses, then a line test on the sibling holds the slic
		 * thread (a delayed hook read would not do: the sampler stamps
		 * before it reads) while the third break begins. The thread
		 * finds the break six ticks late, so the edge carries the gap
		 * and the train reads as nothing instead of as a wrong digit. */
		if (!tc_hook_gate_park(&gate, &t) ||
		    !tc_hook_hold(&gate, &t, false, 20) ||
		    !tc_hook_hold(&gate, &t, true, 30) ||
		    !tc_hook_pulse_train(&gate, &t, 2))
			goto gap_stall_out;
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_MLT_RUN, 600);
		CHECK(udsp_line_test(t.s, 1, UDSP_TEST_LOOP, &job) == UDSP_OK);
		eh_sleep_ms(100);
		tc_hook_set(&t, false);
		if (!eh_gate_step(&gate, 6))
			goto gap_stall_out;
		CHECK(eh_job_wait(&t, job.token, 5000, &status));
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_MLT_RUN, 0);
		if (!tc_hook_hold(&gate, &t, false, 1) ||
		    !tc_hook_hold(&gate, &t, true, 40))
			goto gap_stall_out;
		CHECK(tc_hook_state_wait(&t, UDSP_HOOK_ERROR, 1));
		CHECK_UINT(tc_hook_state_count(UDSP_HOOK_ERROR), 1);
		CHECK_UINT(tc_hook_state_count(UDSP_HOOK_PULSE), 0);
	gap_stall_out:
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_MLT_RUN, 0);
		eh_gate_disarm(&gate);
		tc_down(&t);
		eh_gate_close(&gate);
	}

	CASE("a sampler stalled inside a make drops the train at its return");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct udsp_job job = { 0 };
		int32_t status = 0;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(udsp_line_hook_timing(t.s, 0, 40, 200, 1301) == UDSP_OK);
		/* Two pulses, then the sibling's line test holds the slic thread
		 * while the level sits in a make, so the sample that ends the
		 * stall pushes no record: the stall reaches the machine on the
		 * tick's own sample, before the make it interrupted, run past
		 * the interdigit gap by the stalled ticks, flushes the two
		 * pulses as a digit; the remainder after the return is dropped
		 * with them. */
		if (!tc_hook_gate_park(&gate, &t) ||
		    !tc_hook_hold(&gate, &t, false, 20) ||
		    !tc_hook_hold(&gate, &t, true, 30) ||
		    !tc_hook_pulse_train(&gate, &t, 2))
			goto gap_make_out;
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_MLT_RUN, 600);
		CHECK(udsp_line_test(t.s, 1, UDSP_TEST_LOOP, &job) == UDSP_OK);
		eh_sleep_ms(100);
		if (!eh_gate_step(&gate, 40))
			goto gap_make_out;
		CHECK(eh_job_wait(&t, job.token, 5000, &status));
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_MLT_RUN, 0);
		if (!tc_hook_pulse_train(&gate, &t, 3) ||
		    !tc_hook_hold(&gate, &t, true, 40))
			goto gap_make_out;
		CHECK(tc_hook_state_wait(&t, UDSP_HOOK_ERROR, 1));
		CHECK_UINT(tc_hook_state_count(UDSP_HOOK_ERROR), 1);
		CHECK_UINT(tc_hook_state_count(UDSP_HOOK_PULSE), 0);
	gap_make_out:
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_MLT_RUN, 0);
		eh_gate_disarm(&gate);
		tc_down(&t);
		eh_gate_close(&gate);
	}

	CASE("a sampler stalled inside a break reports no release");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct udsp_job job = { 0 };
		int32_t status = 0;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(udsp_line_hook_timing(t.s, 0, 40, 200, 1301) == UDSP_OK);
		/* The break of a pulse is sampled, then a line test on the
		 * sibling holds the slic thread for longer than the engine
		 * takes to run past release_min_ms. The machine measures the
		 * open to the last sample and reports nothing; the handset
		 * closes before the thread returns, and the closing edge
		 * carries the gap, so the train reads as an error, not as a
		 * release and not as a digit. */
		if (!tc_hook_gate_park(&gate, &t) ||
		    !tc_hook_hold(&gate, &t, false, 20) ||
		    !tc_hook_hold(&gate, &t, true, 30) ||
		    !tc_hook_pulse_train(&gate, &t, 1) ||
		    !tc_hook_hold(&gate, &t, false, 2))
			goto gap_release_out;
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_MLT_RUN, 1500);
		CHECK(udsp_line_test(t.s, 1, UDSP_TEST_LOOP, &job) == UDSP_OK);
		eh_sleep_ms(100);
		if (!eh_gate_step(&gate, 140))
			goto gap_release_out;
		eh_ev_poll(&t);
		CHECK_UINT(tc_hook_state_count(UDSP_HOOK_RELEASE), 1);
		CHECK_UINT(tc_hook_state_count(UDSP_HOOK_PULSE), 0);
		tc_hook_set(&t, true);
		CHECK(eh_job_wait(&t, job.token, 5000, &status));
		if (!tc_hook_hold(&gate, &t, true, 45))
			goto gap_release_out;
		CHECK(tc_hook_state_wait(&t, UDSP_HOOK_ERROR, 1));
		CHECK_UINT(tc_hook_state_count(UDSP_HOOK_RELEASE), 1);
		CHECK_UINT(tc_hook_state_count(UDSP_HOOK_PULSE), 0);
		CHECK_UINT(tc_hook_state_count(UDSP_HOOK_ERROR), 1);
	gap_release_out:
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_MLT_RUN, 0);
		eh_gate_disarm(&gate);
		tc_down(&t);
		eh_gate_close(&gate);
	}

	CASE("a plug-in without the change record classifies a clean train");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };

		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_slic_fake_optional_set(UDSP_SLIC_FAKE_OPT_ALL &
					    ~(uint32_t)UDSP_SLIC_FAKE_OPT_HOOK_MOVED);
		udsp_tdm_override(&tc_ops);
		tc_cfg(&cfg, "tc", "USA");
		if (!tc_open_handle(&t, &cfg) || !tc_start(&t)) {
			CHECK(false);
			return;
		}
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(udsp_line_hook_timing(t.s, 0, 40, 200, 1301) == UDSP_OK);
		if (!tc_hook_gate_park(&gate, &t) ||
		    !tc_hook_hold(&gate, &t, false, 20) ||
		    !tc_hook_hold(&gate, &t, true, 30) ||
		    !tc_hook_pulse_train(&gate, &t, 3) ||
		    !tc_hook_report_wait(&gate, &t, UDSP_HOOK_PULSE))
			goto gap_plain_out;
		CHECK_UINT(tc_hook_last(), UDSP_HOOK_PULSE);
		udsp_counters(t.s, &c);
		CHECK_UINT(c.hook_moved_gaps, 0);
	gap_plain_out:
		eh_gate_disarm(&gate);
		tc_down(&t);
		eh_gate_close(&gate);
	}
}

static bool tc_region_load(struct eh *t, const char *iso3)
{
	struct udsp_job job = { 0 };
	int32_t status = -1;

	return udsp_region_load(t->s, iso3, &job) == UDSP_OK &&
	       eh_job_wait(t, job.token, 3000, &status) && status == 0;
}

/*
 * Seizes line 0, opens it for open_ticks sample periods and waits for the
 * class of that open. An open of N periods measures about (N - 1) * 10 ms
 * (case_hook, "a flash of 300 ms"). The probe stops the handle before it
 * closes the gate, because the tick can read the gate until it stops.
 */
static void tc_hook_open_probe(struct eh *t, unsigned open_ticks, uint8_t want)
{
	struct eh_gate gate = { .fd = { -1, -1 } };

	if (tc_hook_gate_park(&gate, t) &&
	    tc_hook_hold(&gate, t, false, 20) &&
	    tc_hook_hold(&gate, t, true, 30) &&
	    tc_hook_hold(&gate, t, false, open_ticks) &&
	    tc_hook_hold(&gate, t, true, 1))
		tc_hook_report_wait(&gate, t, want);
	eh_gate_disarm(&gate);
	tc_down(t);
	eh_gate_close(&gate);
}

/*
 * The region decides pulse classification, and UDSP_DET_PULSE can only turn
 * it off (core/chan.c chan_hook_pulse_apply). GBR ships pulses off, because
 * its break window lies inside its flash window. Under the core windows of 40
 * to 200 ms, an open of 50 to 70 ms is a pulse break of the USA and the default
 * windows and also a flash, so its class shows the state of the line.
 */
static void case_hook_pulse(void)
{
	struct udsp_chan_cfg off, pulse;
	struct eh t;

	memset(&off, 0, sizeof(off));
	pulse = off;
	pulse.detect_mask = UDSP_DET_PULSE;

	CASE("a GBR line with the pulse bit takes a 70 ms open as a flash");
	if (!tc_up(&t, "GBR")) {
		CHECK(false);
		return;
	}
	CHECK(udsp_chan_config(t.s, 0, &pulse) == UDSP_OK);
	tc_hook_open_probe(&t, 8, UDSP_HOOK_FLASH);
	CHECK_UINT(tc_hook_state_count(UDSP_HOOK_PULSE), 0);

	CASE("a USA line without the pulse bit counts no pulse");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(udsp_chan_config(t.s, 0, &off) == UDSP_OK);
		CHECK(udsp_line_hook_timing(t.s, 0, 40, 200, 1301) == UDSP_OK);
		/* The make after the train outlasts the interdigit timeout, so
		 * a counted train would report its digit. */
		if (!tc_hook_gate_park(&gate, &t) ||
		    !tc_hook_hold(&gate, &t, false, 20) ||
		    !tc_hook_hold(&gate, &t, true, 30) ||
		    !tc_hook_pulse_train(&gate, &t, 3) ||
		    !tc_hook_hold(&gate, &t, true, 40))
			goto pulse_off_out;
		CHECK(tc_hook_state_wait(&t, UDSP_HOOK_FLASH, 3));
		CHECK_UINT(tc_hook_state_count(UDSP_HOOK_PULSE), 0);
	pulse_off_out:
		eh_gate_disarm(&gate);
		tc_down(&t);
		eh_gate_close(&gate);
	}

	CASE("a swap from USA to GBR turns pulses off on a configured line");
	if (!tc_up(&t, "USA")) {
		CHECK(false);
		return;
	}
	CHECK(udsp_chan_config(t.s, 0, &pulse) == UDSP_OK);
	CHECK(tc_region_load(&t, "GBR"));
	tc_hook_open_probe(&t, 8, UDSP_HOOK_FLASH);
	CHECK_UINT(tc_hook_state_count(UDSP_HOOK_PULSE), 0);

	CASE("a line configured before any region keeps its veto at the load");
	if (!tc_up(&t, NULL)) {
		CHECK(false);
		return;
	}
	CHECK(udsp_chan_config(t.s, 0, &off) == UDSP_OK);
	CHECK(udsp_line_hook_timing(t.s, 0, 40, 200, 1301) == UDSP_OK);
	CHECK(tc_region_load(&t, "USA"));
	tc_hook_open_probe(&t, 7, UDSP_HOOK_FLASH);
	CHECK_UINT(tc_hook_state_count(UDSP_HOOK_PULSE), 0);

	CASE("a swap on an unconfigured line takes the value of the region");
	if (!tc_up(&t, "GBR")) {
		CHECK(false);
		return;
	}
	CHECK(udsp_line_hook_timing(t.s, 0, 40, 200, 1301) == UDSP_OK);
	CHECK(tc_region_load(&t, "USA"));
	tc_hook_open_probe(&t, 7, UDSP_HOOK_PULSE);

	CASE("a line with no region counts no pulse");
	if (!tc_up(&t, NULL)) {
		CHECK(false);
		return;
	}
	CHECK(udsp_chan_config(t.s, 0, &pulse) == UDSP_OK);
	CHECK(udsp_line_hook_timing(t.s, 0, 40, 200, 1301) == UDSP_OK);
	tc_hook_open_probe(&t, 7, UDSP_HOOK_FLASH);
	CHECK_UINT(tc_hook_state_count(UDSP_HOOK_PULSE), 0);
}

/* Opens the engine under region, configures line 0 for pulses, and swaps to
 * next when a name is given. One pulse train must then carry want, and every
 * other hook event of the line must carry 0. */
static void tc_hook_coding_probe(const char *region, const char *next,
				 uint8_t want)
{
	struct udsp_chan_cfg pulse;
	unsigned others;
	struct eh t;

	memset(&pulse, 0, sizeof(pulse));
	pulse.detect_mask = UDSP_DET_PULSE;
	if (!tc_up(&t, region)) {
		CHECK(false);
		return;
	}
	CHECK(udsp_chan_config(t.s, 0, &pulse) == UDSP_OK);
	if (next)
		CHECK(tc_region_load(&t, next));
	tc_hook_open_probe(&t, 7, UDSP_HOOK_PULSE);
	CHECK_UINT(tc_hook_coding(&others), want);
	CHECK_UINT(others, 0);
}

/*
 * A pulse event carries the decadic coding of the region that counted the
 * train (event.h u.hook.pulse_invert). The channel reset gives the hook
 * machine the coding of the region at open. A region swap on a running line
 * gives it the coding of the next region. NZL inverts and USA does not, and a
 * 60 ms open is a pulse break in both regions.
 */
static void case_hook_pulse_invert(void)
{
	CASE("a USA pulse train carries the direct coding");
	tc_hook_coding_probe("USA", NULL, 0);

	CASE("an NZL pulse train carries the inverted coding");
	tc_hook_coding_probe("NZL", NULL, 1);

	CASE("a swap from USA to NZL inverts the next train of a running line");
	tc_hook_coding_probe("USA", "NZL", 1);

	CASE("a swap from NZL to USA restores the direct coding");
	tc_hook_coding_probe("NZL", "USA", 0);
}

static void tc_ring_fill(struct udsp_ring *r, uint16_t on, uint16_t off,
			 uint32_t duration_ms)
{
	memset(r, 0, sizeof(*r));
	r->cadence[0].on_ms = on;
	r->cadence[0].off_ms = off;
	r->cadence_count = 1;
	r->duration_ms = duration_ms;
}

static void case_ring(void)
{
	struct udsp_ring ring;
	struct eh t;

	CASE("ring cadence drives the relay and the ring events");
	{
		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		tc_ring_fill(&ring, 200, 300, 1300);
		CHECK(udsp_ring_start(t.s, 0, &ring) == UDSP_OK);
		eh_wait_ms(&t, 1600);
		CHECK(eh_ev_find(UDSP_EV_RING_START, 0) != NULL);
		{
			const struct udsp_event *end =
				eh_ev_find(UDSP_EV_RING_END, 0);

			CHECK(end != NULL);
			if (end)
				CHECK_UINT(end->u.line.state, LINE_RING_EXPIRED);
		}
		CHECK(tc_stop(&t));
		{
			struct udsp_slic_fake_chan_state st;

			/* The counters, not the journal: the hook sampler fills
			 * the 64-row journal in well under a second. */
			CHECK(tc_state(&t, 0, &st));
			/* 1300 ms of a 200/300 cadence is three bursts. */
			CHECK_UINT(st.ring_start_count, 3);
			CHECK_UINT(st.ring_stop_count, 3);
		}
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a ring trip reports RING_END before the hook seize");
	{
		int end_at = -1, seize_at = -1;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 100);
		tc_ring_fill(&ring, 400, 400, 4000);
		CHECK(udsp_ring_start(t.s, 0, &ring) == UDSP_OK);
		eh_wait_ms(&t, 200);
		tc_hook_set(&t, true);
		eh_wait_ms(&t, 300);

		for (unsigned i = 0; i < eh_ev_len; i++) {
			if (eh_ev[i].type == UDSP_EV_RING_END && end_at < 0)
				end_at = (int)i;
			if (eh_ev[i].type == UDSP_EV_HOOK &&
			    eh_ev[i].u.hook.state == UDSP_HOOK_SEIZE &&
			    seize_at < 0)
				seize_at = (int)i;
		}
		CHECK(end_at >= 0);
		CHECK(seize_at >= 0);
		CHECK(end_at < seize_at);
		if (end_at >= 0)
			CHECK_UINT(eh_ev[end_at].u.line.state, LINE_RING_ANSWERED);
		tc_hook_set(&t, false);
		tc_down(&t);
	}
}

static void tc_cid_fill(struct udsp_cid *cid, const char *number, const char *name)
{
	memset(cid, 0, sizeof(*cid));
	cid->format = UDSP_CIDF_BELLCORE_FSK;
	snprintf(cid->number, sizeof(cid->number), "%s", number);
	snprintf(cid->name, sizeof(cid->name), "%s", name);
	snprintf(cid->datetime, sizeof(cid->datetime), "07041200");
	snprintf(cid->blockinfo, sizeof(cid->blockinfo), "O");
}

static void case_cid_type1(void)
{
	struct udsp_ring ring;
	struct udsp_cid cid;
	uint8_t want[UDSP_CID_MSG_MAX];
	uint8_t got[UDSP_CID_MSG_MAX];
	size_t want_len;
	struct eh t;

	CASE("type 1 caller identity plays the FSK burst inside the break");
	{
		unsigned base, loud, decoded, mark_ms;

		CHECK(tc_region_fast());
		if (!tc_up_ports(&t, "FST", 1)) {
			CHECK(false);
			return;
		}
		tc_cid_fill(&cid, "5551234", "TEST CALLER");
		want_len = udsp_cid_msg_build(&cid, UDSP_CIDF_BELLCORE_FSK, false,
					      want, sizeof(want));
		CHECK(want_len > 0);

		/* cid_at_ring 0 puts the message before the first burst, so the
		 * whole sequence falls inside the fake's journal window, and a
		 * zero delay takes the region pause, which this region shortens
		 * to 50 ms for the same reason. */
		tc_ring_fill(&ring, 200, 400, 6000);
		ring.cid_present = true;
		ring.cid_at_ring = 0;
		ring.cid_wait_ms = 0;
		ring.cid = cid;
		eh_rec_mark();
		mark_ms = udsp_now_ms(t.s);
		CHECK(udsp_ring_start(t.s, 0, &ring) == UDSP_OK);
		eh_wait_ms(&t, 1200);

		{
			const struct udsp_event *sent =
				eh_ev_find(UDSP_EV_CID_SENT, 0);

			CHECK(sent != NULL);
			if (sent)
				CHECK_UINT((uint8_t)sent->u.cid_sent.status, 0);
			/*
			 * The completion waits for the line, not for the
			 * modulator. The recording is the frame stream the
			 * engine wrote from the mark on, one period per tick,
			 * so its last loud sample is the tick the modulator
			 * finished on. The playback pipeline holds prefill
			 * periods (3 here) after that tick, and the completion
			 * that restores the muted feed must come after them
			 * (chan.c CHAN_SEQ_DRAIN_MARGIN), which is where the
			 * SLIC would otherwise cut the tail of the message.
			 */
			if (sent) {
				unsigned end_ms = mark_ms + tc_last_loud(1000) /
					(UDSP_SAMPLE_RATE / 1000u);

				CHECK(sent->stamp_ms >= end_ms + 50u);
				CHECK(sent->stamp_ms <= end_ms + 120u);
			}
		}

		loud = tc_first_loud(0, 1000);
		base = (loud / UDSP_FRAME_SAMPLES) * UDSP_FRAME_SAMPLES;
		CHECK(loud < eh_rec_have());
		/* A zero delay takes the region pause, 50 ms here, so the
		 * carrier is up long before the override case below. */
		CHECK(loud / (UDSP_SAMPLE_RATE / 1000u) < 400u);
		decoded = tc_fsk_decode(base, 0, 20, got, sizeof(got));
		CHECK_UINT(decoded, (uint64_t)want_len);
		if (decoded == want_len)
			CHECK(memcmp(got, want, want_len) == 0);

		CHECK(udsp_ring_stop(t.s, 0) == UDSP_OK);
		CHECK(tc_stop(&t));
		tc_journal(&t);
		{
			int up = tc_jr_find(UDSP_SLIC_FAKE_LINEFEED_SET, 0,
					    UDSP_LF_FWD_ACTIVE, 0);
			int oht = up < 0 ? -1 :
				  tc_jr_find(UDSP_SLIC_FAKE_LINEFEED_SET, 0,
					     UDSP_LF_FWD_OHT, (unsigned)up + 1u);
			int back = oht < 0 ? -1 :
				   tc_jr_find(UDSP_SLIC_FAKE_LINEFEED_SET, 0,
					      UDSP_LF_FWD_ACTIVE,
					      (unsigned)oht + 1u);
			int relay = oht < 0 ? -1 :
				    tc_jr_find(UDSP_SLIC_FAKE_RING_START, 0, -1,
					       (unsigned)oht + 1u);

			CHECK(up >= 0);
			CHECK(oht > up);
			CHECK(back > oht);
			/* The message goes out in the break, so the first burst
			 * only follows the restore (line.h section 3). */
			CHECK(relay > oht);
		}
		tc_free(&t);
		udsp_tdm_override(NULL);
		tc_region_fast_drop();
	}

	/*
	 * The production shape. uvoice ring_build() sends cid_wait_ms 500 as
	 * the GR-30 delay from the break to the burst, and USA.conf carries the
	 * same 500 as the [cid] pre-message pause. The two are separate timings
	 * that happen to share a value, so the ring machine must not read the
	 * caller's delay as its own window bound: doing so closes the break on
	 * the very tick the sequencer leaves LINE_CID_WAIT, and
	 * line_cidseq_tick() aborts -ETIMEDOUT before the burst is armed.
	 *
	 * Every other caller-identity case here and in test_line.c leaves the
	 * delay far above the pause, so none of them covers the equal value.
	 */
	CASE("a caller delay equal to the region pause still sends the burst");
	{
		CHECK(tc_region_make(500));
		if (!tc_up_ports(&t, "FST", 1)) {
			CHECK(false);
			return;
		}
		tc_cid_fill(&cid, "5551234", "TEST CALLER");
		want_len = udsp_cid_msg_build(&cid, UDSP_CIDF_BELLCORE_FSK, false,
					      want, sizeof(want));
		CHECK(want_len > 0);

		/* cid_at_ring 1 is the shipped placement: the break after the
		 * first burst, which is the break uvoice asks for. */
		tc_ring_fill(&ring, 200, 400, 6000);
		ring.cid_present = true;
		ring.cid_at_ring = 1;
		ring.cid_wait_ms = 500;
		ring.cid = cid;
		CHECK(udsp_ring_start(t.s, 0, &ring) == UDSP_OK);
		eh_wait_ms(&t, 1600);
		{
			const struct udsp_event *sent =
				eh_ev_find(UDSP_EV_CID_SENT, 0);

			CHECK(sent != NULL);
			if (sent)
				CHECK_UINT((uint8_t)sent->u.cid_sent.status, 0);
		}
		CHECK(udsp_ring_stop(t.s, 0) == UDSP_OK);
		tc_down(&t);
		tc_region_fast_drop();
	}

	/* The caller's delay replaces the region pause for this ring; it does
	 * not add to it. That is the whole point of carrying it in udsp_ring.
	 * The region here pauses 50 ms, so an 800 ms delay is unambiguous in
	 * the recording. */
	CASE("the caller delay replaces the region pause");
	{
		unsigned loud_ms;

		CHECK(tc_region_fast());
		if (!tc_up_ports(&t, "FST", 1)) {
			CHECK(false);
			return;
		}
		tc_cid_fill(&cid, "5551234", "TEST CALLER");
		tc_ring_fill(&ring, 200, 400, 6000);
		ring.cid_present = true;
		ring.cid_at_ring = 0;
		ring.cid_wait_ms = 800;
		ring.cid = cid;
		eh_rec_mark();
		CHECK(udsp_ring_start(t.s, 0, &ring) == UDSP_OK);
		eh_wait_ms(&t, 1600);
		loud_ms = tc_first_loud(0, 1000) / (UDSP_SAMPLE_RATE / 1000u);
		CHECK(loud_ms >= 700u);
		CHECK(loud_ms <= 1100u);
		CHECK(udsp_ring_stop(t.s, 0) == UDSP_OK);
		tc_down(&t);
		tc_region_fast_drop();
	}

	/* A delay that cannot fit the break is refused before the bell starts,
	 * so no subscriber is rung for a record that can never arrive. */
	CASE("a delay the break cannot hold refuses the ring outright");
	{
		CHECK(tc_region_fast());
		if (!tc_up_ports(&t, "FST", 1)) {
			CHECK(false);
			return;
		}
		tc_cid_fill(&cid, "5551234", "TEST CALLER");
		tc_ring_fill(&ring, 200, 400, 6000);
		ring.cid_present = true;
		ring.cid_at_ring = 1;
		ring.cid_wait_ms = LINE_CID_WINDOW_MAX_MS;
		ring.cid = cid;
		CHECK(udsp_ring_start(t.s, 0, &ring) == UDSP_UNSUPPORTED);
		eh_wait_ms(&t, 300);
		CHECK(eh_ev_find(UDSP_EV_RING_START, 0) == NULL);
		tc_down(&t);
		tc_region_fast_drop();
	}

	/*
	 * udsp.h promises the engine supplies the date and time when the caller
	 * leaves the field empty, and GR-30 puts that parameter first in a call
	 * setup message. Both public entry points carry a record, so the case
	 * covers both.
	 */
	CASE("an empty date and time is filled on both entry points");
	{
		unsigned base, loud, decoded;

		CHECK(tc_region_fast());
		if (!tc_up_ports(&t, "FST", 1)) {
			CHECK(false);
			return;
		}
		tc_cid_fill(&cid, "5551234", "TEST CALLER");
		cid.datetime[0] = '\0';
		tc_ring_fill(&ring, 200, 400, 6000);
		ring.cid_present = true;
		ring.cid_at_ring = 0;
		ring.cid_wait_ms = 0;
		ring.cid = cid;
		eh_rec_mark();
		CHECK(udsp_ring_start(t.s, 0, &ring) == UDSP_OK);
		eh_wait_ms(&t, 1200);
		loud = tc_first_loud(0, 1000);
		base = (loud / UDSP_FRAME_SAMPLES) * UDSP_FRAME_SAMPLES;
		decoded = tc_fsk_decode(base, 0, 20, got, sizeof(got));
		CHECK(decoded > 12);
		if (decoded > 12) {
			CHECK_UINT(got[2], 0x01);
			CHECK_UINT(got[3], 8);
			for (unsigned i = 0; i < 8; i++)
				CHECK(got[4 + i] >= '0' && got[4 + i] <= '9');
		}
		CHECK(udsp_ring_stop(t.s, 0) == UDSP_OK);

		/* The same record through cid_send, with no ring to ride. */
		eh_wait_ms(&t, 100);
		eh_rec_mark();
		CHECK(udsp_cid_send(t.s, 0, UDSP_CID_TYPE1, &cid) == UDSP_OK);
		eh_wait_ms(&t, 1200);
		loud = tc_first_loud(0, 1000);
		base = (loud / UDSP_FRAME_SAMPLES) * UDSP_FRAME_SAMPLES;
		decoded = tc_fsk_decode(base, 0, 20, got, sizeof(got));
		CHECK(decoded > 12);
		if (decoded > 12) {
			CHECK_UINT(got[2], 0x01);
			CHECK_UINT(got[3], 8);
		}

		/* A supplied value is left alone. */
		tc_cid_fill(&cid, "5551234", "TEST CALLER");
		want_len = udsp_cid_msg_build(&cid, UDSP_CIDF_BELLCORE_FSK, false,
					      want, sizeof(want));
		CHECK(want_len > 0);
		eh_wait_ms(&t, 100);
		eh_rec_mark();
		CHECK(udsp_cid_send(t.s, 0, UDSP_CID_TYPE1, &cid) == UDSP_OK);
		eh_wait_ms(&t, 1200);
		loud = tc_first_loud(0, 1000);
		base = (loud / UDSP_FRAME_SAMPLES) * UDSP_FRAME_SAMPLES;
		decoded = tc_fsk_decode(base, 0, 20, got, sizeof(got));
		CHECK_UINT(decoded, (uint64_t)want_len);
		if (decoded == want_len)
			CHECK(memcmp(got, want, want_len) == 0);

		tc_down(&t);
		tc_region_fast_drop();
	}
}

static void case_cid_type2(void)
{
	struct udsp_cid cid;
	struct udsp_chan_cfg ccfg;
	struct eh t;

	CASE("type 2 caller identity plays CAS and waits for the acknowledgement");
	{
		uint32_t t0;
		unsigned base;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_DTMF;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) == UDSP_OK);
		eh_wait_ms(&t, 50);

		tc_cid_fill(&cid, "5559876", "WAITING");
		eh_rec_mark();
		t0 = eh_now_ms();
		CHECK(udsp_cid_send(t.s, 0, UDSP_CID_TYPE2, &cid) == UDSP_OK);

		atomic_store(&tc_be.dtmf[0].digit, 'D');
		atomic_store(&tc_be.dtmf[0].from_ms, t0 + 130u);
		atomic_store(&tc_be.dtmf[0].to_ms, t0 + 230u);
		atomic_store_explicit(&tc_be.dtmf_count, 1u, memory_order_release);

		base = eh_rec_have();
		{
			uint64_t end = eh_mono_us() + 2000000u;

			/* The mute brackets the whole sequence, so the far end
			 * hears none of it. */
			while (eh_mono_us() < end &&
			       !atomic_load(&t.s->chan[0].mute)) {
				eh_ev_poll(&t);
				eh_sleep_ms(1);
			}
			CHECK(atomic_load(&t.s->chan[0].mute));
		}
		eh_wait_ms(&t, 1500);
		CHECK(!atomic_load(&t.s->chan[0].mute));
		atomic_store_explicit(&tc_be.dtmf_count, 0u, memory_order_release);

		/* CAS is 2130 plus 2750 Hz for 80 ms (line.h LINE_CAS_*). */
		CHECK(eh_power(base + 80, 480, 2130.0) >
		      eh_power(base + 80, 480, 1000.0) * 8.0);
		CHECK(eh_power(base + 80, 480, 2750.0) >
		      eh_power(base + 80, 480, 1000.0) * 8.0);
		CHECK(eh_ev_find(UDSP_EV_DIGIT, 0) != NULL);
		{
			const struct udsp_event *sent =
				eh_ev_find(UDSP_EV_CID_SENT, 0);

			CHECK(sent != NULL);
			if (sent) {
				CHECK_UINT((uint8_t)sent->u.cid_sent.status, 0);
				CHECK_UINT(sent->u.cid_sent.kind, UDSP_CID_TYPE2);
			}
		}
		CHECK_UINT(eh_ev_count(UDSP_EV_CID2_TIMEOUT), 0);
		tc_down(&t);
	}

	CASE("type 2 without an acknowledgement reports CID2_TIMEOUT");
	{
		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_DTMF;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) == UDSP_OK);
		tc_cid_fill(&cid, "5559876", "WAITING");
		CHECK(udsp_cid_send(t.s, 0, UDSP_CID_TYPE2, &cid) == UDSP_OK);
		eh_wait_ms(&t, 900);
		CHECK_UINT(eh_ev_count(UDSP_EV_CID2_TIMEOUT), 1);
		CHECK_UINT(eh_ev_count(UDSP_EV_CID_SENT), 0);
		tc_down(&t);
	}
}

static void case_vmwi(void)
{
	struct udsp_cid cid;
	uint8_t want[UDSP_CID_MSG_MAX];
	uint8_t got[UDSP_CID_MSG_MAX];
	size_t want_len;
	struct eh t;

	CASE("the visual indicator burst carries the vmwi message");
	{
		unsigned base, loud, decoded;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		memset(&cid, 0, sizeof(cid));
		cid.format = UDSP_CIDF_BELLCORE_FSK;
		cid.mwi = true;
		want_len = udsp_cid_vmwi_build(true, "", UDSP_CIDF_BELLCORE_FSK,
					       want, sizeof(want));
		CHECK(want_len > 0);
		eh_rec_mark();
		CHECK(udsp_cid_send(t.s, 0, UDSP_CID_TYPE1, &cid) == UDSP_OK);
		eh_wait_ms(&t, 2000);

		loud = tc_first_loud(0, 1000);
		base = (loud / UDSP_FRAME_SAMPLES) * UDSP_FRAME_SAMPLES;
		decoded = tc_fsk_decode(base, 300, 180, got, sizeof(got));
		CHECK_UINT(decoded, (uint64_t)want_len);
		if (decoded == want_len)
			CHECK(memcmp(got, want, want_len) == 0);
		tc_down(&t);
	}

	CASE("an all empty identity is the visual indicator off burst");
	{
		unsigned base, loud, decoded;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		memset(&cid, 0, sizeof(cid));
		cid.format = UDSP_CIDF_BELLCORE_FSK;
		want_len = udsp_cid_vmwi_build(false, "", UDSP_CIDF_BELLCORE_FSK,
					       want, sizeof(want));
		CHECK(want_len > 0);
		eh_rec_mark();
		CHECK(udsp_cid_send(t.s, 0, UDSP_CID_TYPE1, &cid) == UDSP_OK);
		eh_wait_ms(&t, 2000);

		loud = tc_first_loud(0, 1000);
		base = (loud / UDSP_FRAME_SAMPLES) * UDSP_FRAME_SAMPLES;
		decoded = tc_fsk_decode(base, 300, 180, got, sizeof(got));
		CHECK_UINT(decoded, (uint64_t)want_len);
		if (decoded == want_len)
			CHECK(memcmp(got, want, want_len) == 0);
		tc_down(&t);
	}

	CASE("udsp_mwi drives the lamp op alone");
	{
		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		eh_rec_mark();
		CHECK(udsp_mwi(t.s, 0, true) == UDSP_OK);
		eh_wait_ms(&t, 200);
		CHECK(tc_stop(&t));
		tc_journal(&t);
		CHECK(tc_jr_find(UDSP_SLIC_FAKE_MWI_LAMP, 0, 1, 0) > 0);
		CHECK_UINT(eh_ev_count(UDSP_EV_CID_SENT), 0);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("udsp_mwi is unsupported without the lamp op");
	{
		struct udsp_config cfg;

		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_tdm_override(&tc_ops);
		udsp_slic_fake_optional_set(UDSP_SLIC_FAKE_OPT_ALL &
					    ~(uint32_t)UDSP_SLIC_FAKE_OPT_MWI_LAMP);
		tc_cfg(&cfg, "tc", "USA");
		CHECK(tc_open_handle(&t, &cfg));
		if (t.s) {
			CHECK(tc_start(&t));
			CHECK(udsp_mwi(t.s, 0, true) == UDSP_UNSUPPORTED);
			tc_down(&t);
		}
	}
}

/*
 * A tone of the sit slot with silent segments, against its bursts. Every
 * length is a whole number of frames, so each frame of the record is wholly
 * loud or wholly silent.
 */
struct tc_seg_shape {
	const char *body;	/* the [tone sit] keys */
	uint32_t lead_ms;	/* the silent first segment, 0 for none */
	uint32_t total_ms;	/* the length of a finite tone, 0 for endless */
	uint32_t span[3][2];	/* the bursts, in ms from the first one */
	unsigned spans;
	uint32_t end_ms;	/* the frames the case checks, from the first */
};

static void tc_seg_shape_check(const struct tc_seg_shape *sh)
{
	const unsigned per_ms = UDSP_SAMPLE_RATE / 1000u;
	const struct udsp_event *ev;
	unsigned base;
	uint32_t t0;
	struct eh t;

	if (!tc_region_sit_write("SEG", sh->body)) {
		CHECK(false);
		return;
	}
	if (!tc_up(&t, "SEG")) {
		CHECK(false);
		tc_region_dir_drop("SEG.conf");
		return;
	}
	eh_rec_mark();
	t0 = udsp_now_ms(t.s);
	CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_SIT, 0) == UDSP_OK);
	eh_wait_ms(&t, sh->lead_ms + sh->end_ms + 300u);
	/* The tone starts after the mark, so the silent first segment puts
	 * the first burst at least its length past the mark. */
	base = tc_tone_base();
	CHECK(base >= sh->lead_ms * per_ms);
	CHECK(eh_rec_have() >= base + sh->end_ms * per_ms);
	CHECK_UINT(tc_spans_bad(base, sh->span, sh->spans, sh->end_ms), 0);
	ev = eh_ev_find(UDSP_EV_AUDIO_END, 0);
	CHECK_UINT(eh_ev_count(UDSP_EV_AUDIO_END), sh->total_ms ? 1u : 0u);
	/* The tone starts at or after t0, so a silent last segment that the
	 * engine dropped would end the tone early. */
	if (sh->total_ms && ev)
		CHECK(ev->stamp_ms - t0 >= sh->total_ms);
	tc_down(&t);
	tc_region_dir_drop("SEG.conf");
}

static void case_tone(void)
{
	struct eh t;

	CASE("dial tone plays its region frequencies");
	{
		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		eh_rec_mark();
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
		eh_wait_ms(&t, 300);
		CHECK(eh_rec_have() > 1600);
		CHECK(eh_power(400, 800, 350.0) > eh_power(400, 800, 1000.0) * 8.0);
		CHECK(eh_power(400, 800, 440.0) > eh_power(400, 800, 1000.0) * 8.0);
		CHECK(udsp_tone_stop(t.s, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);
		{
			unsigned tail = eh_rec_have();

			CHECK(eh_energy(tail - 400, 400) <= 1000.0);
		}
		tc_down(&t);
	}

	CASE("call waiting variants render, and an absent one falls back");
	{
		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		/* A variant above the grammar bound is refused. */
		CHECK(udsp_cw_tone_play(t.s, 0, UDSP_REGION_CW_VIEW_MAX + 1, 0) ==
		      UDSP_UNSUPPORTED);

		/* c3 is 100 ms on, 100 ms off, repeated: 440 Hz in the first burst
		 * and silence in the first break. A fallback to the plain 300 ms
		 * tone would fill the break, so the silence check proves the
		 * distinctive cadence rendered, not the base tone. */
		eh_rec_mark();
		CHECK(udsp_cw_tone_play(t.s, 0, 3, 0) == UDSP_OK);
		eh_wait_ms(&t, 250);
		CHECK(eh_power(400, 300, 440.0) > eh_power(400, 300, 1000.0) * 8.0);
		CHECK(eh_energy(1250, 300) <= 100000.0);
		CHECK(udsp_tone_stop(t.s, 0) == UDSP_OK);
		eh_wait_ms(&t, 60);

		/* variant 0 is the plain call-waiting tone, one 300 ms burst. */
		eh_rec_mark();
		CHECK(udsp_cw_tone_play(t.s, 0, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);
		CHECK(eh_power(400, 800, 440.0) > eh_power(400, 800, 1000.0) * 8.0);
		CHECK(udsp_tone_stop(t.s, 0) == UDSP_OK);
		tc_down(&t);
	}

	CASE("a variant a region omits falls back to the plain tone");
	{
		/* GBR defines no [cw] rows, so any distinctive variant plays the
		 * plain 400 Hz call-waiting tone rather than being refused. */
		if (!tc_up(&t, "GBR")) {
			CHECK(false);
			return;
		}
		eh_rec_mark();
		CHECK(udsp_cw_tone_play(t.s, 0, 2, 0) == UDSP_OK);
		eh_wait_ms(&t, 110);
		CHECK(eh_power(400, 300, 400.0) > eh_power(400, 300, 1000.0) * 8.0);
		CHECK(udsp_tone_stop(t.s, 0) == UDSP_OK);
		tc_down(&t);
	}

	CASE("a finite tone runs its repeat count then ends");
	{
		unsigned bursts = 0;
		bool on = false;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		eh_rec_mark();
		/* USA confirm: 350 + 440 Hz, 100/100 ms, repeat 3. */
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_CONFIRM, 0) == UDSP_OK);
		eh_wait_ms(&t, 1000);
		for (unsigned i = 0; i + 80 <= eh_rec_have(); i += 80) {
			bool loud = eh_energy(i, 80) > 100000.0;

			if (loud && !on)
				bursts++;
			on = loud;
		}
		CHECK_UINT(bursts, 3);
		CHECK_UINT(eh_ev_count(UDSP_EV_AUDIO_END), 1);
		tc_down(&t);
	}

	CASE("a stutter tone continues steady after its repeat count");
	{
		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		eh_rec_mark();
		/* USA stutter: 100/100 ms, repeat 10, then steady. */
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_STUTTER, 0) == UDSP_OK);
		eh_wait_ms(&t, 2600);
		CHECK(eh_rec_have() > 20000);
		/* The 2 s mark is past the ten cycles, so nothing interrupts. */
		CHECK(eh_energy(17000, 800) > 100000.0);
		CHECK(eh_energy(18000, 800) > 100000.0);
		CHECK_UINT(eh_ev_count(UDSP_EV_AUDIO_END), 0);
		tc_down(&t);
	}

	CASE("a segmented tone plays its frequencies in order");
	{
		unsigned base;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		eh_rec_mark();
		/* USA sit: 914, 1371 then 1777 Hz (GR-506 low-tone triple). */
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_SIT, 0) == UDSP_OK);
		eh_wait_ms(&t, 2300);
		CHECK(eh_power(400, 800, 914.0) > eh_power(400, 800, 1371.0) * 8.0);
		CHECK(eh_power(2700, 800, 1371.0) > eh_power(2700, 800, 914.0) * 8.0);
		CHECK(eh_power(4900, 800, 1777.0) > eh_power(4900, 800, 1371.0) * 8.0);
		/* The segments end at 932 ms. The 1 s of trailing silence
		 * follows, and the next cycle starts on 914 Hz at 1932 ms. */
		base = tc_tone_base();
		CHECK(eh_energy(base + 7600, 7700) <= 100.0);
		CHECK(eh_power(base + 15800, 1200, 914.0) >
		      eh_power(base + 15800, 1200, 1371.0) * 8.0);
		tc_down(&t);
	}

	CASE("the GBR stutter interrupts 350 Hz while 440 Hz runs on");
	{
		unsigned base;

		if (!tc_up(&t, "GBR")) {
			CHECK(false);
			return;
		}
		eh_rec_mark();
		/* SIN 350: 350 + 440 Hz for 750 ms, then 440 Hz alone. */
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_STUTTER, 0) == UDSP_OK);
		eh_wait_ms(&t, 2400);
		base = tc_tone_base();
		CHECK(eh_rec_have() >= base + 17600);
		CHECK(eh_power(base + 400, 5200, 350.0) >
		      eh_power(base + 400, 5200, 1000.0) * 8.0);
		CHECK(eh_power(base + 400, 5200, 440.0) >
		      eh_power(base + 400, 5200, 1000.0) * 8.0);
		CHECK(eh_power(base + 6400, 5200, 350.0) * 100.0 <
		      eh_power(base + 400, 5200, 350.0));
		CHECK(eh_power(base + 6400, 5200, 440.0) >
		      eh_power(base + 6400, 5200, 1000.0) * 8.0);
		/* The cycle wraps to the pair. */
		CHECK(eh_power(base + 12400, 5200, 350.0) >
		      eh_power(base + 12400, 5200, 1000.0) * 8.0);
		CHECK_UINT(eh_ev_count(UDSP_EV_AUDIO_END), 0);
		tc_down(&t);
	}

	CASE("the AUS congestion tone plays its second burst 10 dB down");
	{
		double first, second, next;
		unsigned base;

		if (!tc_up(&t, "AUS")) {
			CHECK(false);
			return;
		}
		eh_rec_mark();
		/* S002: 425 Hz at -13 dBm0 for 375 ms, 375 ms off, 425 Hz at
		 * -23 dBm0 for 375 ms, then 375 ms of silence. */
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_REORDER, 0) == UDSP_OK);
		eh_wait_ms(&t, 2100);
		base = tc_tone_base();
		CHECK(eh_rec_have() >= base + 14600);
		first = eh_power(base + 400, 2200, 425.0);
		second = eh_power(base + 6400, 2200, 425.0);
		next = eh_power(base + 12400, 2200, 425.0);
		CHECK(first > eh_power(base + 400, 2200, 1000.0) * 8.0);
		CHECK(second > 0.0 && next > 0.0);
		if (second > 0.0 && next > 0.0) {
			CHECK(fabs(10.0 * log10(first / second) - 10.0) <= 1.0);
			/* The next cycle starts at the full level again. */
			CHECK(fabs(10.0 * log10(next / first)) <= 1.0);
		}
		CHECK(eh_energy(base + 3080, 2840) <= 100.0);
		CHECK(eh_energy(base + 9080, 2840) <= 100.0);
		tc_down(&t);
	}

	CASE("a silent first segment delays every burst of an endless tone");
	{
		static const struct tc_seg_shape sh = {
			.body = "segment = off 200\nsegment = 1000 -13 100\n"
				"silence = 0\nrepeat = forever\n",
			.lead_ms = 200,
			.span = { { 0, 100 }, { 300, 400 }, { 600, 700 } },
			.spans = 3,
			.end_ms = 900,
		};

		tc_seg_shape_check(&sh);
	}

	CASE("a silent first segment counts in a finite tone");
	{
		static const struct tc_seg_shape sh = {
			.body = "segment = off 200\nsegment = 1000 -13 100\n"
				"silence = 0\nrepeat = 2\n",
			.lead_ms = 200,
			.total_ms = 600,
			.span = { { 0, 100 }, { 300, 400 } },
			.spans = 2,
			.end_ms = 700,
		};

		tc_seg_shape_check(&sh);
	}

	CASE("a silent middle segment splits the bursts of an endless tone");
	{
		static const struct tc_seg_shape sh = {
			.body = "segment = 1000 -13 100\nsegment = off 200\n"
				"segment = 1000 -13 100\nsilence = 0\n"
				"repeat = forever\n",
			.span = { { 0, 100 }, { 300, 500 }, { 700, 900 } },
			.spans = 3,
			.end_ms = 1100,
		};

		tc_seg_shape_check(&sh);
	}

	CASE("a silent middle segment counts in a finite tone");
	{
		static const struct tc_seg_shape sh = {
			.body = "segment = 1000 -13 100\nsegment = off 200\n"
				"segment = 1000 -13 100\nsilence = 0\n"
				"repeat = 2\n",
			.total_ms = 800,
			.span = { { 0, 100 }, { 300, 500 }, { 700, 800 } },
			.spans = 3,
			.end_ms = 1000,
		};

		tc_seg_shape_check(&sh);
	}

	CASE("a silent last segment closes each cycle of an endless tone");
	{
		static const struct tc_seg_shape sh = {
			.body = "segment = 1000 -13 100\nsegment = off 200\n"
				"silence = 0\nrepeat = forever\n",
			.span = { { 0, 100 }, { 300, 400 }, { 600, 700 } },
			.spans = 3,
			.end_ms = 900,
		};

		tc_seg_shape_check(&sh);
	}

	CASE("a silent last segment delays the end of a finite tone");
	{
		static const struct tc_seg_shape sh = {
			.body = "segment = 1000 -13 100\nsegment = off 200\n"
				"silence = 0\nrepeat = 2\n",
			.total_ms = 600,
			.span = { { 0, 100 }, { 300, 400 } },
			.spans = 2,
			.end_ms = 800,
		};

		tc_seg_shape_check(&sh);
	}

	CASE("a frequency keeps its phase at a new index and across the wrap");
	{
		unsigned base;

		/* 400 Hz runs 133.2 cycles and 700 Hz 233.1 cycles in each
		 * 333 ms segment, and the two segments list the pair in the
		 * opposite order. A phase reset or a restore by index steps
		 * a phase at each boundary, and leaves a residual above a
		 * thousand. */
		if (!tc_region_sit_write("PHS", "segment = 400+700 -13 333\n"
					 "segment = 700+400 -13 333\n"
					 "silence = 0\nrepeat = forever\n")) {
			CHECK(false);
			return;
		}
		if (!tc_up(&t, "PHS")) {
			CHECK(false);
			tc_region_dir_drop("PHS.conf");
			return;
		}
		eh_rec_mark();
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_SIT, 0) == UDSP_OK);
		eh_wait_ms(&t, 1600);
		base = tc_tone_base();
		/* From past the onset across the index swap at 2664, the wrap
		 * at 5328 and the swap again at 7992. */
		CHECK(tc_two_tone_residual(base + 8, 4u * 2664u, 400.0,
					   700.0) <= 16.0);
		tc_down(&t);
		tc_region_dir_drop("PHS.conf");
	}

	CASE("the sequencer slot overrides the api slot and it resumes");
	{
		struct udsp_chan_cfg ccfg;
		unsigned base;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		memset(&ccfg, 0, sizeof(ccfg));
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) == UDSP_OK);
		eh_rec_mark();
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);
		base = eh_rec_have();
		{
			struct udsp_cid cid;

			tc_cid_fill(&cid, "5551111", "CW");
			CHECK(udsp_cid_send(t.s, 0, UDSP_CID_TYPE2, &cid) == UDSP_OK);
		}
		eh_wait_ms(&t, 100);
		/* Slot B is the CAS: the dial pair is gone while it runs. */
		CHECK(eh_power(base + 160, 400, 2130.0) >
		      eh_power(base + 160, 400, 350.0) * 4.0);
		eh_wait_ms(&t, 1200);
		{
			unsigned tail = eh_rec_have() - 800;

			/* Slot A resumed after the sequence gave up. */
			CHECK(eh_power(tail, 800, 350.0) >
			      eh_power(tail, 800, 2130.0) * 8.0);
		}
		tc_down(&t);
	}
}

static void case_digit(void)
{
	struct udsp_dtmf_detector det;
	struct udsp_event ev;
	struct eh t;
	char seen = 0;

	CASE("digit play renders the digit and ends");
	{
		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		eh_rec_mark();
		CHECK(udsp_digit_play(t.s, 0, '5', 120) == UDSP_OK);
		eh_wait_ms(&t, 400);
		udsp_dtmf_detector_init(&det, false);
		for (unsigned i = 0; i + UDSP_FRAME_SAMPLES <= eh_rec_have();
		     i += UDSP_FRAME_SAMPLES) {
			if (udsp_dtmf_detector_process(&det,
						       eh_be.rec + eh_be.rec_base + i,
						       &ev))
				seen = ev.u.digit.digit;
		}
		CHECK(seen == '5');
		CHECK_UINT(eh_ev_count(UDSP_EV_AUDIO_END), 1);
		tc_down(&t);
	}
}

/* --- the level at the line ----------------------------------------------- */

/*
 * The receive pairs (gain_rx, slic_gain_rx) of the level cases, in half-dB
 * steps: no gain, the uvoice default, a half-dB loss option beside it, two
 * deep losses up to the library bound, and a SLIC gain with a DSP gain.
 */
static const int8_t tc_level_pair[][2] = {
	{ 0, 0 }, { 0, -18 }, { -1, -18 }, { 0, -40 }, { 0, -60 }, { 4, 12 },
};
#define TC_LEVEL_PAIRS	(sizeof(tc_level_pair) / sizeof(tc_level_pair[0]))
/* A reading spans 100 ms from 50 ms after the first loud sample, inside the
 * on part of every tone below. */
#define TC_LEVEL_LEAD_MS	50u
#define TC_LEVEL_SPAN_MS	100u

enum tc_level_kind {
	TC_LEVEL_DIAL,
	TC_LEVEL_BUSY,
	TC_LEVEL_SIT,
	TC_LEVEL_DIGIT,
};

static const char *const tc_level_name[] = { "dial", "busy", "sit", "digit" };

static uint32_t tc_gain_confirmed(struct eh *t, unsigned line);
static bool tc_gain_wait(struct eh *t, unsigned line, uint32_t want,
			 unsigned ms);

/*
 * The receive gain the SLIC took for line 0, in dB. The wait expects the gain
 * half of the receive value alone, and the reading takes the pair the part
 * confirmed. A loss that reached the SLIC therefore fails the wait and lowers
 * the line level.
 */
static double tc_slic_rx_db(struct eh *t, int slic_gain_rx)
{
	CHECK(tc_gain_wait(t, 0, udsp_gain_confirmed_pack(0, slic_gain_rx > 0 ?
							  slic_gain_rx : 0),
			   2000));

	return (int8_t)(tc_gain_confirmed(t, 0) & 0xffu) / 2.0;
}

/* The total level of a window of the record, in dBm0 at the slot. */
static double tc_slot_dbm0(unsigned from, unsigned len)
{
	return rms_dbm0_get(sqrt(eh_energy(from, len)));
}


/* The total of n tones of one level each, in dBm0. */
static double tc_dbm0_total(int level_dbm0, unsigned n)
{
	return level_dbm0 + 10.0 * log10((double)n);
}

/* The level the active region gives the tone: the total over its
 * frequencies, of the first segment for a segmented tone. */
static double tc_region_dbm0(struct eh *t, enum udsp_tone tone)
{
	const struct udsp_region *r = udsp_region_active(t->s);
	const struct udsp_region_tone *rt;
	double p = 0.0;

	if (!r)
		return 0.0;
	rt = &r->tone[tone];
	if (rt->segmented)
		return tc_dbm0_total(rt->segment[0].level_dbm0,
				     rt->segment[0].freq_count);
	for (unsigned i = 0; i < rt->desc.frequency_count; i++)
		p += pow(10.0, rt->desc.level_dbm0[i] / 10.0);

	return 10.0 * log10(p);
}

static bool tc_level_play(struct eh *t, enum tc_level_kind kind)
{
	static const enum udsp_tone tone[] = {
		UDSP_TONE_DIAL, UDSP_TONE_BUSY, UDSP_TONE_SIT,
	};

	if (kind == TC_LEVEL_DIGIT)
		return udsp_digit_play(t->s, 0, '5', 300) == UDSP_OK;

	return udsp_tone_play(t->s, 0, tone[kind], 0) == UDSP_OK;
}

static double tc_level_want(struct eh *t, enum tc_level_kind kind)
{
	switch (kind) {
	case TC_LEVEL_DIAL:
		return tc_region_dbm0(t, UDSP_TONE_DIAL);
	case TC_LEVEL_BUSY:
		return tc_region_dbm0(t, UDSP_TONE_BUSY);
	case TC_LEVEL_SIT:
		return tc_region_dbm0(t, UDSP_TONE_SIT);
	default:
		return LINE_CID_DTMF_LEVEL_DBM0;
	}
}

/*
 * One playout under every pair at one line rate. The digit reads its row
 * tone alone (770 Hz for '5'), so the level of the column cannot move the
 * reading. Every other kind reads the total of the window. A tone is a local
 * signal and keeps its region level under every pair. A digit is the far end's
 * (udsp_digit_play) and takes the whole receive gain, gain_rx plus
 * slic_gain_rx, as network audio does.
 */
static void tc_level_run(unsigned rate, enum tc_level_kind kind)
{
	const unsigned per_ms = rate / 1000u;
	const unsigned span = TC_LEVEL_SPAN_MS * per_ms;
	struct udsp_chan_cfg ccfg;
	struct eh t;
	double want;

	if (!tc_up_rate(&t, "USA", 2, rate)) {
		CHECK(false);
		return;
	}
	want = tc_level_want(&t, kind);
	for (unsigned i = 0; i < TC_LEVEL_PAIRS; i++) {
		int slic_rx = tc_level_pair[i][1];
		unsigned from;
		double got;

		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.gain_rx = tc_level_pair[i][0];
		ccfg.slic_gain_rx = (int8_t)slic_rx;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		eh_rec_mark();
		CHECK(tc_level_play(&t, kind));
		eh_wait_ms(&t, 200);
		from = tc_tone_base() + TC_LEVEL_LEAD_MS * per_ms;
		CHECK(eh_rec_have() >= from + span);
		if (kind == TC_LEVEL_DIGIT)
			got = eh_tone_dbm0(from, span, 770.0, rate);
		else
			got = tc_slot_dbm0(from, span);
		got += tc_slic_rx_db(&t, slic_rx);
		double want_i = want;

		if (kind == TC_LEVEL_DIGIT)
			want_i += (tc_level_pair[i][0] + slic_rx) / 2.0;
		printf("# level %s %u Hz pair (%d, %d): %.2f dBm, want %.2f\n",
		       tc_level_name[kind], rate, tc_level_pair[i][0], slic_rx,
		       got, want_i);
		CHECK(fabs(got - want_i) <= 0.2);
		CHECK(udsp_tone_stop(t.s, 0) == UDSP_OK);
		eh_wait_ms(&t, 20);
	}
	tc_down(&t);
}

/*
 * The SLIC takes only the gain half of slic_gain_rx, and the engine applies a
 * loss ahead of the playout sum (core/chan.c chan_rx_split_apply). A local
 * signal at N dBm0 therefore plays at N dBm for every pair. The reading adds
 * the gain the SLIC applies after the slot.
 */
static const unsigned tc_level_rate[2] = {
	UDSP_SAMPLE_RATE, UDSP_SAMPLE_RATE_WB,
};

static void case_level(void)
{
	const unsigned *rate = tc_level_rate;

	CASE("local signals play at the region level for every gain pair");
	for (unsigned r = 0; r < 2u; r++)
		tc_level_run(rate[r], TC_LEVEL_DIAL);

	CASE("a cadenced tone plays at its region level for every gain pair");
	for (unsigned r = 0; r < 2u; r++)
		tc_level_run(rate[r], TC_LEVEL_BUSY);

	CASE("a segmented tone plays at its region level for every gain pair");
	for (unsigned r = 0; r < 2u; r++)
		tc_level_run(rate[r], TC_LEVEL_SIT);

	CASE("a played digit takes the receive gain of every gain pair");
	for (unsigned r = 0; r < 2u; r++)
		tc_level_run(rate[r], TC_LEVEL_DIGIT);
}

/* One caller identity burst at one rate and one receive value. */
static void tc_cid_level_run(unsigned rate, int slic_gain_rx,
			     enum udsp_cid_kind kind)
{
	const unsigned per_ms = rate / 1000u;
	const struct udsp_region *r;
	struct udsp_chan_cfg ccfg;
	struct udsp_cid cid;
	unsigned from, span;
	double want, got;
	struct eh t;

	if (!tc_up_rate(&t, "USA", 2, rate)) {
		CHECK(false);
		return;
	}
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.detect_mask = UDSP_DET_DTMF;
	ccfg.slic_gain_rx = (int8_t)slic_gain_rx;
	CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
	CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) == UDSP_OK);
	eh_wait_ms(&t, 50);
	r = udsp_region_active(t.s);
	if (!r) {
		CHECK(false);
		tc_down(&t);
		return;
	}
	tc_cid_fill(&cid, "5551234", "LEVEL");
	eh_rec_mark();
	CHECK(udsp_cid_send(t.s, 0, kind, &cid) == UDSP_OK);
	eh_wait_ms(&t, kind == UDSP_CID_TYPE1 ? 1000 : 300);
	if (kind == UDSP_CID_TYPE1) {
		/* Bell 202 runs 1200 bits a second. */
		from = tc_tone_base() + (r->cid.seizure_bits * 1000u / 1200u +
					 25u) * per_ms;
		span = 100u * per_ms;
		want = LINE_CID_FSK_LEVEL_DBM0;
	} else {
		from = tc_tone_base() + 10u * per_ms;
		span = 60u * per_ms;
		want = tc_dbm0_total(LINE_CAS_LEVEL_DBM0, 2);
	}
	CHECK(eh_rec_have() >= from + span);
	got = tc_slot_dbm0(from, span) + tc_slic_rx_db(&t, slic_gain_rx);
	printf("# level %s %u Hz slic_gain_rx %d: %.2f dBm, want %.2f\n",
	       kind == UDSP_CID_TYPE1 ? "fsk" : "cas", rate, slic_gain_rx, got,
	       want);
	CHECK(fabs(got - want) <= 0.3);
	tc_down(&t);
}

/*
 * The caller identity plays from slot B, the sequencer slot. It keeps its
 * fixed level for a receive value of -18 and of 0 half-dB, at both rates.
 * The FSK reads 100 ms of the steady mark after the seizure. The CAS reads
 * 60 ms of its 80 ms.
 */
static void case_cid_level(void)
{
	static const int slic_rx[2] = { -18, 0 };
	const unsigned *rate = tc_level_rate;

	CASE("caller identity plays at its fixed level");
	for (unsigned r = 0; r < 2u; r++) {
		for (unsigned i = 0; i < 2u; i++) {
			tc_cid_level_run(rate[r], slic_rx[i], UDSP_CID_TYPE1);
			tc_cid_level_run(rate[r], slic_rx[i], UDSP_CID_TYPE2);
		}
	}
}

static void case_detect(void)
{
	struct udsp_chan_cfg ccfg;
	struct eh t;

	CASE("the dtmf detector reports captured digits");
	{
		uint32_t t0;
		unsigned digits = 0;
		bool got1 = false, gothash = false;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_DTMF;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		t0 = eh_now_ms();
		atomic_store(&tc_be.dtmf[0].digit, '1');
		atomic_store(&tc_be.dtmf[0].from_ms, t0 + 100u);
		atomic_store(&tc_be.dtmf[0].to_ms, t0 + 200u);
		atomic_store(&tc_be.dtmf[1].digit, '#');
		atomic_store(&tc_be.dtmf[1].from_ms, t0 + 400u);
		atomic_store(&tc_be.dtmf[1].to_ms, t0 + 500u);
		atomic_store_explicit(&tc_be.dtmf_count, 2u, memory_order_release);
		eh_wait_ms(&t, 800);
		atomic_store_explicit(&tc_be.dtmf_count, 0u, memory_order_release);

		for (unsigned i = 0; i < eh_ev_len; i++) {
			if (eh_ev[i].type != UDSP_EV_DIGIT)
				continue;
			digits++;
			CHECK_UINT(eh_ev[i].u.digit.remote, 0);
			CHECK(eh_ev[i].u.digit.duration_ms >= 120);
			CHECK(eh_ev[i].u.digit.duration_ms <= 130);
			if (eh_ev[i].u.digit.digit == '1')
				got1 = true;
			if (eh_ev[i].u.digit.digit == '#')
				gothash = true;
		}
		CHECK_UINT(digits, 2);
		CHECK(got1);
		CHECK(gothash);
		tc_down(&t);
	}

	CASE("the dtmf detector is silent when the mask is clear");
	{
		uint32_t t0;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		memset(&ccfg, 0, sizeof(ccfg));
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		t0 = eh_now_ms();
		atomic_store(&tc_be.dtmf[0].digit, '1');
		atomic_store(&tc_be.dtmf[0].from_ms, t0 + 100u);
		atomic_store(&tc_be.dtmf[0].to_ms, t0 + 300u);
		atomic_store_explicit(&tc_be.dtmf_count, 1u, memory_order_release);
		eh_wait_ms(&t, 600);
		atomic_store_explicit(&tc_be.dtmf_count, 0u, memory_order_release);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 0);
		tc_down(&t);
	}
}

/*
 * The 16 kHz line. One rate per handle, settled at open: the config asks, the
 * TDM grants and the plug-in reports the capability, and any refusal degrades
 * the whole line to 8 kHz before udsp_start. The detectors keep running at
 * 8 kHz behind the per-line decimators, so the cases dial and demodulate at
 * the line rate and let the engine do the folding.
 */
/*
 * The description a case programs. tc_stream_desc_default is the silent PCMU
 * stream: it is the only thing that lets the per-line media chain run at all
 * (core/stream.c stream_line_active), and the line canceller and both fax
 * detectors sit behind that gate. No RTP is ever received on it, so the
 * downlink stays silent and the uplink is the backend's own capture. The
 * cases that use it never exercise the 16 kHz rate conversion in the codec.
 * t38 selects the T.38 mode with the triple uvoice programs and leaves codec
 * unread; 0 in an interval takes the library default.
 */
struct tc_stream_desc {
	uint8_t codec;
	uint8_t direction;
	uint8_t ptime_ms;
	bool rtcp_enable;
	uint16_t rtcp_interval_ms;
	uint16_t punch_interval_ms;
	bool t38;
	uint16_t remote_port;
};

static void tc_stream_desc_default(struct tc_stream_desc *d)
{
	memset(d, 0, sizeof(*d));
	d->codec = UDSP_SEL_PCMU;
	d->direction = UDSP_MEDIA_SENDRECV;
	d->ptime_ms = 10;
	d->remote_port = 5000;
}

static bool tc_stream_pairs(struct tc_stream *w)
{
	memset(w, 0, sizeof(*w));
	w->rtp[0] = w->rtp[1] = w->rtcp[0] = w->rtcp[1] = -1;
	if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, w->rtp))
		return false;

	return !socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, w->rtcp);
}

static void tc_media_fill(struct udsp_media *m, const struct tc_stream_desc *d)
{
	memset(m, 0, sizeof(*m));
	m->local.ipver = 4;
	m->local.port = 4000;
	m->remote.ipver = 4;
	m->remote.port = d->remote_port;
	m->codec = d->t38 ? UDSP_SEL_T38 : d->codec;
	m->direction = d->direction;
	m->ptime_ms = d->ptime_ms;
	m->jb_mode = UDSP_JB_ADAPTIVE;
	m->jb_init_ms = 20;
	m->jb_max_ms = 200;
	m->rtcp_enable = d->rtcp_enable;
	m->rtcp_interval_ms = d->rtcp_interval_ms;
	m->punch_interval_ms = d->punch_interval_ms;
	if (d->t38) {
		m->t38.max_bit_rate = 14400;
		m->t38.max_datagram = 200;
		m->t38.ec = UDSP_T38_EC_REDUNDANCY;
	}
}

/* Programs the description on the pairs the stream already holds, which is
 * the re-INVITE shape: the same descriptors, re-dup'd by the library. */
static bool tc_stream_program(struct eh *t, struct tc_stream *w, unsigned line,
			      const struct tc_stream_desc *d)
{
	struct udsp_media m;

	tc_media_fill(&m, d);
	/* libudsp dups both descriptors (core/stream.c fd_dup_nb), so this
	 * side keeps and closes its own. */
	return udsp_stream_config(t->s, line, 0, &m, w->rtp[0], w->rtcp[0]) ==
	       UDSP_OK;
}

static bool tc_stream_config_desc(struct eh *t, struct tc_stream *w,
				  unsigned line, const struct tc_stream_desc *d)
{
	return tc_stream_pairs(w) && tc_stream_program(t, w, line, d);
}

static bool tc_stream_config(struct eh *t, struct tc_stream *w, unsigned line)
{
	struct tc_stream_desc d;

	tc_stream_desc_default(&d);

	return tc_stream_config_desc(t, w, line, &d);
}

static bool tc_stream_up_desc(struct eh *t, struct tc_stream *w, unsigned line,
			      const struct tc_stream_desc *d)
{
	return tc_stream_config_desc(t, w, line, d) &&
	       udsp_stream_start(t->s, line, 0) == UDSP_OK;
}

static bool tc_stream_up(struct eh *t, struct tc_stream *w, unsigned line)
{
	struct tc_stream_desc d;

	tc_stream_desc_default(&d);

	return tc_stream_up_desc(t, w, line, &d);
}

static void tc_stream_fds_close(struct tc_stream *w)
{
	int *fd[4] = { &w->rtp[0], &w->rtp[1], &w->rtcp[0], &w->rtcp[1] };

	for (unsigned i = 0; i < 4; i++) {
		if (*fd[i] >= 0)
			close(*fd[i]);
		*fd[i] = -1;
	}
}

/* The stop is the release a direct read of the stream record acquires, so a
 * case that takes one needs its answer. */
static bool tc_stream_down(struct eh *t, struct tc_stream *w, unsigned line)
{
	bool ok = udsp_stream_stop(t->s, line, 0) == UDSP_OK;

	tc_stream_fds_close(w);

	return ok;
}

static void case_wideband(void)
{
	struct udsp_chan_cfg ccfg;
	struct udsp_config cfg;
	struct engine_stats st;
	struct eh t;

	CASE("a 16 kHz ask reaches the lines with the wideband capability");
	{
		if (!tc_up_rate(&t, "USA", 2, 16000)) {
			CHECK(false);
			return;
		}
		CHECK_UINT(t.s->cap.features & UDSP_FEAT_WIDEBAND,
			   UDSP_FEAT_WIDEBAND);
		CHECK_UINT(t.s->cap.codecs & (1u << UDSP_SEL_G722),
			   1u << UDSP_SEL_G722);
		CHECK_UINT(t.s->rate, UDSP_SAMPLE_RATE_WB);
		CHECK_UINT(t.s->frame, 160);
		CHECK_UINT(t.s->rate_degrades, 0);
		CHECK_UINT(t.s->chan[0].rate, UDSP_SAMPLE_RATE_WB);
		CHECK_UINT(t.s->chan[0].frame, 160);
		/* The whole line runs at the settled rate: the tick moves a
		 * 160-frame period and still comes every 10 ms. */
		tc_engine_stats(&t, &st);
		CHECK_UINT(st.rate, UDSP_SAMPLE_RATE_WB);
		CHECK_UINT(st.frames_per_period, 160);
		CHECK_UINT(st.period_us, 10000);
		/* LINES_UP ran: both lines are inited and sampling. */
		tc_journal(&t);
		CHECK(tc_jr_find(UDSP_SLIC_FAKE_INIT, 0, 0, 0) >= 0);
		CHECK(tc_jr_find(UDSP_SLIC_FAKE_INIT, 1, 0, 0) >= 0);
		CHECK(t.s->chan[0].live);
		tc_down(&t);
	}

	CASE("a digit dialled at 16 kHz reaches the detector");
	{
		uint32_t t0;
		unsigned digits = 0;
		bool got5 = false;

		if (!tc_up_rate(&t, "USA", 2, 16000)) {
			CHECK(false);
			return;
		}
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_DTMF;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		t0 = eh_now_ms();
		atomic_store(&tc_be.dtmf[0].digit, '5');
		atomic_store(&tc_be.dtmf[0].from_ms, t0 + 100u);
		atomic_store(&tc_be.dtmf[0].to_ms, t0 + 200u);
		atomic_store_explicit(&tc_be.dtmf_count, 1u, memory_order_release);
		eh_wait_ms(&t, 600);
		atomic_store_explicit(&tc_be.dtmf_count, 0u, memory_order_release);

		for (unsigned i = 0; i < eh_ev_len; i++) {
			if (eh_ev[i].type != UDSP_EV_DIGIT)
				continue;
			digits++;
			CHECK_UINT(eh_ev[i].u.digit.remote, 0);
			if (eh_ev[i].u.digit.digit == '5')
				got5 = true;
		}
		CHECK_UINT(digits, 1);
		CHECK(got5);
		tc_down(&t);
	}

	/*
	 * The canceller runs on the same frame. The media chain, and with it
	 * the line canceller, only runs behind an active stream (core/stream.c
	 * stream_line_active), so this case brings one up, arms the canceller
	 * and gives the backend a matched hybrid echo of the dial tone the
	 * line is playing. The digit is dialed into that echo and must still
	 * be reported: the detector reads the uplink the SLIC handed up, and
	 * a canceller that is adapting on a live reference does not change
	 * that (rs_ul_pre).
	 */
	CASE("a digit survives a running canceller on a 16 kHz line");
	{
		struct tc_stream w;
		uint32_t t0;
		unsigned digits = 0;
		bool got8 = false;

		if (!tc_up_rate(&t, "USA", 2, 16000)) {
			CHECK(false);
			return;
		}
		/* Off hook, because the tick runs no canceller on an open
		 * loop. */
		CHECK(eh_hook_seize(&t));
		/* The canceller instance is made at stream config, and only
		 * when the channel already asks for it (core/stream.c
		 * stream_lec_ensure), so the channel is configured first. */
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_DTMF;
		ccfg.ec_enable = true;
		ccfg.ec_nlp_enable = true;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		if (!tc_stream_up(&t, &w, 0)) {
			CHECK(false);
			tc_down(&t);
			return;
		}
		/* The engine's own loop delay is three periods here, so the
		 * hybrid returns the playback of three ticks ago and the
		 * canceller's reference lines up with it. */
		tc_be.echo_periods = 3;
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 3000) == UDSP_OK);
		eh_wait_ms(&t, 400);
		CHECK(atomic_load(&t.s->lec_ready[0]));
		CHECK_UINT(atomic_load(&t.s->lec_delay[0]), 2u * 160u);
		CHECK(atomic_load(&t.s->chan[0].ec) != NULL);

		t0 = eh_now_ms();
		atomic_store(&tc_be.dtmf[0].digit, '8');
		atomic_store(&tc_be.dtmf[0].from_ms, t0 + 100u);
		atomic_store(&tc_be.dtmf[0].to_ms, t0 + 300u);
		atomic_store_explicit(&tc_be.dtmf_count, 1u, memory_order_release);
		eh_wait_ms(&t, 700);
		atomic_store_explicit(&tc_be.dtmf_count, 0u, memory_order_release);
		for (unsigned i = 0; i < eh_ev_len; i++) {
			if (eh_ev[i].type != UDSP_EV_DIGIT)
				continue;
			digits++;
			if (eh_ev[i].u.digit.digit == '8')
				got8 = true;
		}
		CHECK_UINT(digits, 1);
		CHECK(got8);
		tc_be.echo_periods = 0;
		tc_stream_down(&t, &w, 0);
		tc_down(&t);
	}

	/* Arming twice is the shape uvoice drives (a detector armed per call),
	 * so the wideband feed has to survive a disarm with audio still on the
	 * line and an arm after it. */
	CASE("the 16 kHz detector feed survives a re-arm mid-stream");
	{
		uint32_t t0;
		bool got1 = false, got9 = false;

		if (!tc_up_rate(&t, "USA", 2, 16000)) {
			CHECK(false);
			return;
		}
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_DTMF;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		t0 = eh_now_ms();
		atomic_store(&tc_be.dtmf[0].digit, '1');
		atomic_store(&tc_be.dtmf[0].from_ms, t0 + 50u);
		atomic_store(&tc_be.dtmf[0].to_ms, t0 + 200u);
		atomic_store_explicit(&tc_be.dtmf_count, 1u, memory_order_release);
		eh_wait_ms(&t, 400);
		atomic_store_explicit(&tc_be.dtmf_count, 0u, memory_order_release);

		/* Disarmed, with a loud tone still running on the line: the
		 * decimator stops being called and keeps whatever it last
		 * filtered. */
		memset(&ccfg, 0, sizeof(ccfg));
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		atomic_store(&tc_be.tone_hz, 1000u);
		eh_wait_ms(&t, 200);
		atomic_store(&tc_be.tone_hz, 0u);

		ccfg.detect_mask = UDSP_DET_DTMF;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		t0 = eh_now_ms();
		atomic_store(&tc_be.dtmf[0].digit, '9');
		atomic_store(&tc_be.dtmf[0].from_ms, t0 + 50u);
		atomic_store(&tc_be.dtmf[0].to_ms, t0 + 250u);
		atomic_store_explicit(&tc_be.dtmf_count, 1u, memory_order_release);
		eh_wait_ms(&t, 500);
		atomic_store_explicit(&tc_be.dtmf_count, 0u, memory_order_release);

		for (unsigned i = 0; i < eh_ev_len; i++) {
			if (eh_ev[i].type != UDSP_EV_DIGIT)
				continue;
			if (eh_ev[i].u.digit.digit == '1')
				got1 = true;
			if (eh_ev[i].u.digit.digit == '9')
				got9 = true;
		}
		CHECK(got1);
		CHECK(got9);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 2);
		tc_down(&t);
	}

	/*
	 * The second decimator (rs_ul_post). The local fax detector reads the
	 * uplink after the canceller, inside the media chain, so this case
	 * needs an active stream too. A 2100 Hz answer tone held for half a
	 * second is the detector's CED rule (dsp/fax_det.c ANSWER_MIN_FRAMES);
	 * the tone is generated at the line rate, so only the decimator can
	 * put it on the 8 kHz grid the detector works on.
	 */
	CASE("a 16 kHz answer tone reaches the local fax detector");
	{
		struct tc_stream w;
		unsigned ced = 0;

		if (!tc_up_rate(&t, "USA", 2, 16000)) {
			CHECK(false);
			return;
		}
		if (!tc_stream_up(&t, &w, 0)) {
			CHECK(false);
			tc_down(&t);
			return;
		}
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_MODEM;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		atomic_store(&tc_be.tone_hz, 2100u);
		eh_wait_ms(&t, 900);
		atomic_store(&tc_be.tone_hz, 0u);
		eh_wait_ms(&t, 100);
		for (unsigned i = 0; i < eh_ev_len; i++) {
			if (eh_ev[i].type != UDSP_EV_TONE)
				continue;
			if (eh_ev[i].u.tone.code != UDSP_TONE_DET_CED)
				continue;
			CHECK_UINT(eh_ev[i].u.tone.direction,
				   UDSP_TONE_DIR_LOCAL);
			ced++;
		}
		CHECK(ced > 0);
		tc_stream_down(&t, &w, 0);
		tc_down(&t);
	}

	CASE("a caller identity on a 16 kHz line demodulates");
	{
		struct udsp_ring ring;
		struct udsp_cid cid;
		uint8_t want[UDSP_CID_MSG_MAX];
		uint8_t got[UDSP_CID_MSG_MAX];
		size_t want_len;
		unsigned base, loud, decoded;

		CHECK(tc_region_fast());
		if (!tc_up_rate(&t, "FST", 1, 16000)) {
			CHECK(false);
			return;
		}
		tc_cid_fill(&cid, "5551234", "TEST CALLER");
		want_len = udsp_cid_msg_build(&cid, UDSP_CIDF_BELLCORE_FSK, false,
					      want, sizeof(want));
		CHECK(want_len > 0);

		tc_ring_fill(&ring, 200, 400, 6000);
		ring.cid_present = true;
		ring.cid_at_ring = 0;
		ring.cid_wait_ms = 0;
		ring.cid = cid;
		eh_rec_mark();
		CHECK(udsp_ring_start(t.s, 0, &ring) == UDSP_OK);
		eh_wait_ms(&t, 1200);
		{
			const struct udsp_event *sent =
				eh_ev_find(UDSP_EV_CID_SENT, 0);

			CHECK(sent != NULL);
			if (sent)
				CHECK_UINT((uint8_t)sent->u.cid_sent.status, 0);
		}
		CHECK(udsp_ring_stop(t.s, 0) == UDSP_OK);
		/* The recorder holds the 16 kHz line, so it is stopped first
		 * and then folded to the 8 kHz grid the oracle reads. */
		CHECK(tc_stop(&t));
		tc_rec_decimate();
		loud = tc_first_loud(0, 1000);
		CHECK(loud < eh_rec_have());
		/* The burst still starts on a frame boundary, but the fold
		 * costs the decimator's group delay: UDSP_RESAMP_DELAY at
		 * 16 kHz is half that on the 8 kHz grid, and one 1200 baud
		 * bit cell is only 6.67 samples wide, so the bit clock starts
		 * where the carrier now sits. Measured: the decode window is
		 * offsets 9 to 13, and the delay falls at 12. */
		base = (loud / UDSP_FRAME_SAMPLES) * UDSP_FRAME_SAMPLES +
		       (UDSP_RESAMP_DELAY + 1u) / 2u;
		decoded = tc_fsk_decode(base, 0, 20, got, sizeof(got));
		CHECK_UINT(decoded, (uint64_t)want_len);
		if (decoded == want_len)
			CHECK(memcmp(got, want, want_len) == 0);
		tc_free(&t);
		udsp_tdm_override(NULL);
		tc_region_fast_drop();
	}

	CASE("a part without wideband degrades the line to 8 kHz");
	{
		tc_backend_reset();
		udsp_slic_fake_reset();
		/* caps_get answers without the bit, as the proslic plug-in
		 * does for a part or a profile with no wideband pair. */
		udsp_slic_fake_wideband_set(false);
		udsp_tdm_override(&tc_ops);
		tc_cfg(&cfg, "tc", "USA");
		cfg.sample_rate = 16000;
		CHECK(tc_open_handle(&t, &cfg));
		if (!t.s) {
			CHECK(false);
			return;
		}
		/* The open succeeds at the narrowband rate, and says why. */
		CHECK_UINT(t.s->rate, UDSP_SAMPLE_RATE);
		CHECK_UINT(t.s->frame, UDSP_FRAME_SAMPLES);
		CHECK_UINT(t.s->rate_degrades, 1);
		CHECK_UINT(t.s->rate_reason, UDSP_RATE_DEGRADE_NO_WIDEBAND);
		CHECK(tc_log_has(UDSP_LOG_WARN, "16000 Hz refused (the slic part "
				 "has no wideband mode), the line runs at 8000 Hz"));
		CHECK_UINT(t.s->cap.features & UDSP_FEAT_WIDEBAND, 0);
		CHECK_UINT(t.s->cap.codecs & (1u << UDSP_SEL_G722), 0);
		CHECK_UINT(t.s->chan[0].rate, UDSP_SAMPLE_RATE);
		CHECK_UINT(t.s->chan[0].frame, UDSP_FRAME_SAMPLES);
		/* The reopened engine really runs the narrowband geometry. */
		tc_engine_stats(&t, &st);
		CHECK_UINT(st.rate, UDSP_SAMPLE_RATE);
		CHECK_UINT(st.frames_per_period, UDSP_FRAME_SAMPLES);
		/* And the degraded line still comes up. */
		CHECK(tc_start(&t));
		eh_wait_ms(&t, 100);
		CHECK(t.s->chan[0].live);
		tc_down(&t);
		udsp_slic_fake_reset();
	}

	/*
	 * The other half of the settle. The TDM carries the rate, so a device
	 * that will not open at 16 kHz degrades the line exactly as a plug-in
	 * without the capability does. It does so before the SLIC is even
	 * attached.
	 */
	CASE("a tdm that refuses 16 kHz degrades the line");
	{
		tc_backend_reset();
		udsp_slic_fake_reset();
		tc_be.refuse_rate = 16000;
		udsp_tdm_override(&tc_ops);
		tc_cfg(&cfg, "tc", "USA");
		cfg.sample_rate = 16000;
		CHECK(tc_open_handle(&t, &cfg));
		if (!t.s) {
			CHECK(false);
			return;
		}
		CHECK_UINT(t.s->rate, UDSP_SAMPLE_RATE);
		CHECK_UINT(t.s->frame, UDSP_FRAME_SAMPLES);
		CHECK_UINT(t.s->rate_degrades, 1);
		CHECK_UINT(t.s->rate_reason, UDSP_RATE_DEGRADE_TDM);
		CHECK(tc_log_has(UDSP_LOG_WARN, "16000 Hz refused (the tdm device "
				 "refused the rate)"));
		/* The back end's record rides on the same line. */
		CHECK(tc_log_has(UDSP_LOG_WARN, "the line runs at 8000 Hz "
				 "(playback rate: Invalid argument)"));
		CHECK_UINT(t.s->cap.features & UDSP_FEAT_WIDEBAND, 0);
		CHECK_UINT(t.s->cap.codecs & (1u << UDSP_SEL_G722), 0);
		CHECK_UINT(t.s->chan[0].rate, UDSP_SAMPLE_RATE);
		/* Exactly two opens: the refused 16 kHz one and the 8 kHz
		 * retry. */
		CHECK_UINT(tc_be.opens, 2);
		tc_engine_stats(&t, &st);
		CHECK_UINT(st.rate, UDSP_SAMPLE_RATE);
		CHECK_UINT(st.frames_per_period, UDSP_FRAME_SAMPLES);
		CHECK(tc_start(&t));
		eh_wait_ms(&t, 100);
		CHECK(t.s->chan[0].live);
		tc_down(&t);
	}

	/*
	 * Only the geometry moves the rate. A start that fails has nothing to
	 * do with the rate: it fails the same way at 8000, so the open must
	 * fail at 16 kHz instead of retrying and reporting a rate refusal that
	 * never happened (engine.h enum engine_open_fail). The count of opens
	 * is the oracle: a retry would open the device twice.
	 */
	CASE("an open failure that is not the rate fails at 16 kHz");
	{
		tc_backend_reset();
		udsp_slic_fake_reset();
		tc_be.start_err = -EIO;
		udsp_tdm_override(&tc_ops);
		tc_cfg(&cfg, "tc", NULL);
		cfg.sample_rate = 16000;
		CHECK(!tc_open_handle(&t, &cfg));
		CHECK_UINT(tc_be.opens, 1);
		CHECK(tc_log_has(UDSP_LOG_ERR,
				 "at 16000 Hz: the engine did not open"));
		tc_free(&t);

		/* The same fault, the same verdict, at the narrowband rate. */
		tc_backend_reset();
		udsp_slic_fake_reset();
		tc_be.start_err = -EIO;
		tc_cfg(&cfg, "tc", NULL);
		cfg.sample_rate = 8000;
		CHECK(!tc_open_handle(&t, &cfg));
		CHECK_UINT(tc_be.opens, 1);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	/* A plug-in with no caps_get at all (slic.h, the op is nullable). */
	CASE("a plug-in without caps_get degrades the line");
	{
		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_slic_fake_optional_set(UDSP_SLIC_FAKE_OPT_ALL &
					    ~(uint32_t)UDSP_SLIC_FAKE_OPT_CAPS);
		udsp_tdm_override(&tc_ops);
		tc_cfg(&cfg, "tc", "USA");
		cfg.sample_rate = 16000;
		CHECK(tc_open_handle(&t, &cfg));
		if (!t.s) {
			CHECK(false);
			return;
		}
		CHECK_UINT(t.s->rate, UDSP_SAMPLE_RATE);
		CHECK_UINT(t.s->rate_degrades, 1);
		CHECK_UINT(t.s->rate_reason, UDSP_RATE_DEGRADE_NO_CAPS);
		CHECK(tc_log_has(UDSP_LOG_WARN, "16000 Hz refused (the slic plug-in "
				 "reports no capabilities)"));
		CHECK_UINT(t.s->cap.features & UDSP_FEAT_WIDEBAND, 0);
		CHECK_UINT(t.s->cap.codecs & (1u << UDSP_SEL_G722), 0);
		CHECK(tc_start(&t));
		eh_wait_ms(&t, 100);
		CHECK(t.s->chan[0].live);
		tc_down(&t);
		udsp_slic_fake_reset();
	}

	CASE("an unsupported sample rate fails the open");
	{
		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_tdm_override(&tc_ops);
		tc_cfg(&cfg, "tc", NULL);
		cfg.sample_rate = 12345;
		CHECK(!tc_open_handle(&t, &cfg));
		tc_free(&t);

		/* The two admitted values still open. */
		tc_cfg(&cfg, "tc", NULL);
		cfg.sample_rate = 8000;
		CHECK(tc_open_handle(&t, &cfg));
		if (t.s) {
			CHECK_UINT(t.s->rate, UDSP_SAMPLE_RATE);
			CHECK_UINT(t.s->cap.codecs & (1u << UDSP_SEL_G722), 0);
		}
		tc_free(&t);

		tc_backend_reset();
		udsp_slic_fake_reset();
		tc_cfg(&cfg, "tc", NULL);
		cfg.sample_rate = 16000;
		CHECK(tc_open_handle(&t, &cfg));
		if (t.s) {
			CHECK_UINT(t.s->rate, UDSP_SAMPLE_RATE_WB);
			CHECK_UINT(t.s->cap.codecs & (1u << UDSP_SEL_G722),
				   1u << UDSP_SEL_G722);
		}
		tc_free(&t);
		udsp_tdm_override(NULL);
	}
}

/*
 * The punch. Inbound media is admitted by connection tracking alone, so the
 * engine sends an empty datagram on each descriptor at every configuration
 * and again whenever nothing else left on it for punch_interval_ms
 * (core/stream.c stream_punch). The interval runs at a few hundred ms of
 * engine time wherever the property allows; the report floor of five seconds
 * (UDSP_RTCP_MIN_MS) sets the one case that needs a report inside the window.
 * The counters the engine writes are read through the private record after
 * udsp_stream_stop: the deactivate that the stop acknowledges is the release
 * the read acquires, and no tick writes an inactive stream. The description
 * is read after udsp_stream_config returns, on the thread that wrote it.
 * The fake gateway hands out one datagram per frame it takes, so a T.38
 * stream with no output is one whose direction does not send.
 */
#define TC_PUNCH_MS	300u

static void case_punch(void)
{
	const struct udsp_stream *st;
	struct tc_stream_desc d;
	struct tc_stream w;
	struct eh t;

	fax_fake_reset();
	udsp_fax_override(&fax_fake_ops);
	if (!tc_up(&t, "USA")) {
		CHECK(false);
		udsp_fax_override(NULL);
		return;
	}
	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL);
	CHECK(udsp_cap(t.s)->features & UDSP_FEAT_T38);
	if (!st) {
		tc_down(&t);
		udsp_fax_override(NULL);
		return;
	}

	CASE("an idle stream punches both descriptors once at start");
	{
		tc_stream_desc_default(&d);
		d.direction = UDSP_MEDIA_RECVONLY;
		d.punch_interval_ms = TC_PUNCH_MS;
		CHECK(tc_stream_up_desc(&t, &w, 0, &d));
		tc_stream_wait(&t, &w, 100);
		CHECK_UINT(w.rtp_got.empty, 1);
		CHECK_UINT(w.rtcp_got.empty, 1);
		CHECK_UINT(w.rtp_got.data, 0);
		CHECK_UINT(w.rtcp_got.data, 0);
	}

	CASE("an idle stream punches again after the punch interval");
	{
		tc_stream_wait(&t, &w, TC_PUNCH_MS + 100);
		CHECK_UINT(w.rtp_got.empty, 2);
		CHECK_UINT(w.rtcp_got.empty, 2);
		CHECK_UINT(w.rtp_got.data, 0);
		CHECK_UINT(w.rtcp_got.data, 0);
	}

	CASE("a punch counts in punch_sent and is not a first transmit");
	{
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 20);
		CHECK_UINT(atomic_load_explicit(&st->punch_sent,
						memory_order_relaxed), 4);
		CHECK(!st->tx_seen);
		CHECK_UINT(eh_ev_count(UDSP_EV_RTP_FIRST_TX), 0);
		tc_stream_down(&t, &w, 0);
	}

	CASE("a report refreshes the RTCP stamp ahead of the punch");
	{
		tc_stream_desc_default(&d);
		d.direction = UDSP_MEDIA_RECVONLY;
		d.rtcp_enable = true;
		d.rtcp_interval_ms = UDSP_RTCP_MIN_MS;
		d.punch_interval_ms = UDSP_RTCP_MIN_MS + 100;
		CHECK(tc_stream_up_desc(&t, &w, 0, &d));
		tc_stream_wait(&t, &w, 100);
		CHECK_UINT(w.rtcp_got.empty, 1);
		CHECK_UINT(w.rtcp_got.data, 0);
		/* The first report goes out at the floor, ahead of the punch
		 * the interval would send 100 ms later, and the second report
		 * refreshes the stamp again before that one comes round. */
		tc_stream_wait(&t, &w, UDSP_RTCP_MIN_MS + 200);
		CHECK_UINT(w.rtcp_got.data, 1);
		CHECK_UINT(w.rtcp_got.empty, 1);
		/* The RTP socket carried nothing and punched on its cadence. */
		CHECK_UINT(w.rtp_got.empty, 2);
		CHECK_UINT(w.rtp_got.data, 0);
		tc_stream_down(&t, &w, 0);
	}

	CASE("a report interval past the punch interval leaves the RTCP "
	     "socket to the punch cadence");
	{
		tc_stream_desc_default(&d);
		d.direction = UDSP_MEDIA_RECVONLY;
		d.rtcp_enable = true;
		d.rtcp_interval_ms = UDSP_RTCP_MIN_MS;
		d.punch_interval_ms = TC_PUNCH_MS;
		CHECK(tc_stream_up_desc(&t, &w, 0, &d));
		tc_stream_wait(&t, &w, 3 * TC_PUNCH_MS + 200);
		CHECK_UINT(w.rtcp_got.empty, 4);
		CHECK_UINT(w.rtcp_got.data, 0);
		CHECK_UINT(w.rtp_got.empty, 4);
		tc_stream_down(&t, &w, 0);
	}

	CASE("with reports off the punch alone holds the RTCP tuple");
	{
		tc_stream_desc_default(&d);
		d.direction = UDSP_MEDIA_RECVONLY;
		d.punch_interval_ms = TC_PUNCH_MS;
		CHECK(tc_stream_up_desc(&t, &w, 0, &d));
		tc_stream_wait(&t, &w, 3 * TC_PUNCH_MS + 200);
		CHECK_UINT(w.rtcp_got.empty, 4);
		CHECK_UINT(w.rtcp_got.data, 0);
		tc_stream_down(&t, &w, 0);
	}

	CASE("a sending stream never punches the RTP socket");
	{
		tc_stream_desc_default(&d);
		d.punch_interval_ms = TC_PUNCH_MS;
		CHECK(tc_stream_up_desc(&t, &w, 0, &d));
		tc_stream_wait(&t, &w, 3 * TC_PUNCH_MS + 200);
		CHECK_UINT(w.rtp_got.empty, 0);
		CHECK(w.rtp_got.data >= 100);
		CHECK_UINT(w.rtcp_got.empty, 4);
		CHECK_UINT(eh_ev_count(UDSP_EV_RTP_FIRST_TX), 1);
		tc_stream_down(&t, &w, 0);
	}

	CASE("a T.38 stream with nothing to send punches both descriptors");
	{
		tc_stream_desc_default(&d);
		d.t38 = true;
		d.direction = UDSP_MEDIA_RECVONLY;
		d.punch_interval_ms = TC_PUNCH_MS;
		CHECK(tc_stream_up_desc(&t, &w, 0, &d));
		tc_stream_wait(&t, &w, 3 * TC_PUNCH_MS + 200);
		CHECK_UINT(w.rtp_got.empty, 4);
		CHECK_UINT(w.rtcp_got.empty, 4);
		CHECK_UINT(w.rtp_got.data, 0);
		CHECK_UINT(w.rtcp_got.data, 0);
		tc_stream_down(&t, &w, 0);
	}

	CASE("UDPTL leaving on the RTP socket refreshes its stamp");
	{
		tc_stream_desc_default(&d);
		d.t38 = true;
		d.punch_interval_ms = TC_PUNCH_MS;
		CHECK(tc_stream_up_desc(&t, &w, 0, &d));
		tc_stream_wait(&t, &w, 3 * TC_PUNCH_MS + 200);
		CHECK_UINT(w.rtp_got.empty, 0);
		CHECK(w.rtp_got.data >= 100);
		CHECK_UINT(w.rtcp_got.empty, 4);
		tc_stream_down(&t, &w, 0);
	}

	/* The re-arm cases run a long interval, so every punch they count is
	 * the re-arm's and none is the cadence's. The adopt resets punch_sent
	 * and the reconfiguration keeps it, which is what tells the two apart
	 * in the record. */
	CASE("an audio to T.38 move re-arms both descriptors");
	{
		tc_stream_desc_default(&d);
		d.direction = UDSP_MEDIA_RECVONLY;
		d.punch_interval_ms = 4 * TC_PUNCH_MS;
		CHECK(tc_stream_up_desc(&t, &w, 0, &d));
		tc_stream_wait(&t, &w, 100);
		CHECK_UINT(w.rtp_got.empty, 1);
		CHECK_UINT(w.rtcp_got.empty, 1);
		d.t38 = true;
		CHECK(tc_stream_program(&t, &w, 0, &d));
		tc_stream_wait(&t, &w, 100);
		CHECK_UINT(w.rtp_got.empty, 2);
		CHECK_UINT(w.rtcp_got.empty, 2);
	}

	CASE("a T.38 far end move re-arms both descriptors");
	{
		d.remote_port = 5002;
		CHECK(tc_stream_program(&t, &w, 0, &d));
		tc_stream_wait(&t, &w, 100);
		CHECK_UINT(w.rtp_got.empty, 3);
		CHECK_UINT(w.rtcp_got.empty, 3);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		/* Two since the adopt of the mode move, two more across the
		 * reconfiguration that kept the count. */
		CHECK_UINT(atomic_load_explicit(&st->punch_sent,
						memory_order_relaxed), 4);
		tc_stream_down(&t, &w, 0);
	}

	CASE("an audio reconfiguration re-arms both descriptors");
	{
		tc_stream_desc_default(&d);
		d.direction = UDSP_MEDIA_RECVONLY;
		d.punch_interval_ms = 4 * TC_PUNCH_MS;
		CHECK(tc_stream_up_desc(&t, &w, 0, &d));
		tc_stream_wait(&t, &w, 100);
		CHECK_UINT(w.rtp_got.empty, 1);
		d.ptime_ms = 20;
		CHECK(tc_stream_program(&t, &w, 0, &d));
		tc_stream_wait(&t, &w, 100);
		CHECK_UINT(w.rtp_got.empty, 2);
		CHECK_UINT(w.rtcp_got.empty, 2);
		CHECK_UINT(w.rtp_got.data, 0);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		CHECK_UINT(atomic_load_explicit(&st->punch_sent,
						memory_order_relaxed), 4);
		tc_stream_down(&t, &w, 0);
	}

	CASE("a failed punch keeps the stamp and the next tick retries");
	{
		uint32_t closed_ms;

		tc_stream_desc_default(&d);
		d.direction = UDSP_MEDIA_RECVONLY;
		d.punch_interval_ms = TC_PUNCH_MS;
		CHECK(tc_stream_up_desc(&t, &w, 0, &d));
		tc_stream_wait(&t, &w, 100);
		CHECK_UINT(w.rtp_got.empty, 1);
		/* The far end of the RTP pair goes away, so every send on
		 * rtp_fd fails from here on; the RTCP far end stays. */
		close(w.rtp[1]);
		w.rtp[1] = -1;
		closed_ms = eh_now_ms();
		tc_stream_wait(&t, &w, 700);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		/* One failure per tick from the interval on: the 700 ms less
		 * the 300 ms the stamp still covered, give or take the ticks
		 * of the start and the stop. */
		CHECK(atomic_load_explicit(&st->tx_eagain,
					   memory_order_relaxed) >= 45);
		CHECK(atomic_load_explicit(&st->tx_eagain,
					   memory_order_relaxed) <= 58);
		CHECK(st->rtp_tx_ms <= closed_ms);
		CHECK(st->rtcp_tx_ms > closed_ms);
		CHECK_UINT(atomic_load_explicit(&st->punch_sent,
						memory_order_relaxed), 4);
		tc_stream_down(&t, &w, 0);
	}

	CASE("punch_interval_ms 0 takes the default and a value is kept");
	{
		tc_stream_desc_default(&d);
		CHECK(tc_stream_config_desc(&t, &w, 0, &d));
		CHECK_UINT(st->media.punch_interval_ms, 15000);
		CHECK_UINT(UDSP_TX_PUNCH_DEFAULT_MS, 15000);
		d.t38 = true;
		CHECK(tc_stream_program(&t, &w, 0, &d));
		CHECK_UINT(st->media.punch_interval_ms, 15000);
		d.t38 = false;
		d.punch_interval_ms = 5000;
		CHECK(tc_stream_program(&t, &w, 0, &d));
		CHECK_UINT(st->media.punch_interval_ms, 5000);
		tc_stream_down(&t, &w, 0);
	}

	tc_down(&t);
	udsp_fax_override(NULL);
}

/*
 * The catch-up rule of a late engine, end to end. The hold is injected through
 * the model of the PCM core (tdm_raw_fake.h fk_hold_periods): the next capture
 * wait steps both DMA clocks by the periods the engine was away for, exactly
 * as the interrupts would have. The knob is set before the handle is opened,
 * so no case writes a field the tick thread reads; the raw back end defers its
 * DMA start, so the first wait of the tick is the one that takes the hold.
 *
 * The figures are the model's arithmetic. At 8 kHz a period is 80 frames, the
 * playback ring is the prefill plus one (four periods) and the capture ring is
 * the fifteen the AN7581 grants. A tick of the model spends one period of
 * hardware time in the wait it takes, so the periods a hold leaves are spent
 * one per tick until the rule drops them.
 */
struct tc_hold_arm {
	const struct fk_profile *prof;
	unsigned hold;
	bool jit;
	bool silence;
	/* The phased clock (tdm_raw_fake.h): a dry ring restarts at the kick,
	 * and the hold ends extra frames past its whole periods. */
	bool phased;
	unsigned extra;
	/* The hold waits for fk_hold_go, and a playback fault, err 0 for
	 * none, counts its pushes from the hold. */
	bool gated;
	struct fk_sync_fault fault;
	/* The dry ring of the phased clock restarts on the next capture
	 * completion, not at the kick (tdm_raw_fake.h, the grid variant). */
	bool grid;
};

static bool tc_hold_open(struct eh *t, struct udsp_config *cfg,
			 const struct tc_hold_arm *arm)
{
	unsetenv("UDSP_MAX_TICKS");
	fk_reset(arm->prof);
	fk_jit_playback = arm->jit;
	fk_silence_fill = arm->silence;
	fk_phased = arm->phased;
	fk_tx_restart_at_kick = arm->phased && !arm->grid;
	fk_hold_periods[FK_CAPTURE] = arm->hold;
	fk_hold_extra[FK_CAPTURE] = arm->extra;
	fk_hold_gated = arm->gated;
	fk_sync_fault[FK_PLAY] = arm->fault;
	fk_fault_at_hold = arm->fault.err != 0;
	tc_backend_reset();
	udsp_slic_fake_reset();
	udsp_tdm_override(NULL);
	tc_cfg(cfg, "alsa-raw", "USA");
	cfg->sample_rate = 8000;
	snprintf(cfg->tdm_playback, sizeof(cfg->tdm_playback), "hw:0,0");
	snprintf(cfg->tdm_capture, sizeof(cfg->tdm_capture), "hw:0,1");
	if (!tc_open_handle(t, cfg))
		return false;
	/* the hold record is a DEBUG line, behind the debug mask */
	udsp_debug_set(t->s, UDSP_DEBUG_SLIC_LOG);

	return tc_start(t);
}

static bool tc_ticks_wait(struct eh *t, struct engine_stats *st, uint64_t want)
{
	uint64_t deadline = eh_mono_us() + 10000000u;

	memset(st, 0, sizeof(*st));
	if (!t->s)
		return false;
	do {
		tc_engine_stats(t, st);
		if (st->ticks >= want)
			return true;
		eh_sleep_ms(1);
	} while (eh_mono_us() < deadline);

	return false;
}

static unsigned tc_hold_lines(void)
{
	return tc_log_count_needle(UDSP_LOG_DEBUG, "engine hold");
}

static void tc_counters(struct eh *t, struct udsp_counters *ctr)
{
	memset(ctr, 0, sizeof(*ctr));
	if (t->s)
		udsp_counters(t->s, ctr);
}

static void case_hold(void)
{
	struct tc_hold_arm arm = { .prof = &fk_an7581, .jit = true };
	struct udsp_config cfg;
	struct engine_stats st;
	struct udsp_counters ctr;
	struct eh t;

	/* The watchdog publishes the figures and carries the warning, so every
	 * case here shrinks its cadence instead of waiting out the production
	 * second. */
	setenv("UDSP_WATCHDOG_MS", "20", 1);

	CASE("a six period hold is one episode and never xruns");
	{
		arm.hold = 6;
		CHECK(tc_hold_open(&t, &cfg, &arm));
		CHECK(tc_ticks_wait(&t, &st, 100));
		CHECK_UINT(st.xruns, 0);
		CHECK_UINT(st.holds, 1);
		CHECK_UINT(st.capture_stalls, 1);
		CHECK_UINT(st.delay_stalls, 0);
		CHECK_UINT(st.capture_drops, 0);
		CHECK_UINT(st.playback_expired, 0);
		/* The hold is longer than the queue, so the AN7581 ring ran
		 * dry and stopped. The rephase drops the read of the hold tick
		 * and the five periods behind it and renders two periods on
		 * that drop. It restarts the ring with them on the next
		 * capture completion (engine.c engine_rephase_read). The skip
		 * hook gets four periods: the six dropped less the two
		 * rendered. */
		CHECK_UINT(st.trim_drops, 6);
		CHECK_UINT(st.rephases, 1);
		CHECK_UINT(st.rephase_rendered, 2);
		CHECK_UINT(st.rephase_lead, 0);
		CHECK_UINT(st.cal_restarts, 1);
		CHECK(st.delay_calibrated);
		/* Both directions are back at rest: the rephase dropped every
		 * period the hold left, and the ring restarted at the prefill
		 * depth. loop_ref, the sum of the two queues after the write,
		 * reads the three periods it reads at rest. */
		CHECK_UINT((uint64_t)st.delay_dev_last, 0);
		CHECK_UINT((uint64_t)st.loop_ref, 240);
		CHECK(tc_log_wait(UDSP_LOG_DEBUG,
				  "engine hold: 1 episodes, 0 capture periods "
				  "overwritten, 6 trimmed, 0 playback expired",
				  500));
		CHECK(tc_stop(&t));
		tc_counters(&t, &ctr);
		CHECK_UINT(ctr.holds, 1);
		CHECK_UINT(ctr.capture_drops, 0);
		CHECK_UINT(ctr.trim_drops, 6);
		CHECK_UINT(ctr.playback_expired, 0);
		CHECK_UINT(ctr.tdm_stalls, 1);
		tc_free(&t);
		CHECK(!fk_linked);
		fk_release();
	}

	CASE("a sixteen period hold overwrites, trims and never xruns");
	{
		arm.hold = 16;
		CHECK(tc_hold_open(&t, &cfg, &arm));
		CHECK(tc_ticks_wait(&t, &st, 100));
		CHECK_UINT(st.xruns, 0);
		CHECK_UINT(st.holds, 1);
		CHECK_UINT(st.capture_stalls, 1);
		CHECK_UINT(st.delay_stalls, 0);
		/* The ring keeps fifteen periods and the read of the safe
		 * distance leaves two more behind, so the engine is told about
		 * the periods it lost and drops the rest of the excess itself.
		 * The playback ring ran dry, so the rephase drops the read of
		 * the hold tick too: all sixteen periods of hardware time are
		 * overwritten or dropped. */
		CHECK(st.capture_drops > 0);
		CHECK(st.trim_drops > 0);
		CHECK_UINT(st.capture_drops + st.trim_drops, 16);
		CHECK_UINT(st.rephases, 1);
		CHECK_UINT(st.rephase_rendered, 2);
		CHECK_UINT(st.rephase_lead, 0);
		CHECK_UINT(st.cal_restarts, 1);
		CHECK_UINT((uint64_t)st.loop_ref, 240);
		CHECK(tc_stop(&t));
		tc_counters(&t, &ctr);
		CHECK_UINT(ctr.holds, 1);
		CHECK(ctr.capture_drops > 0);
		CHECK(ctr.trim_drops > 0);
		tc_free(&t);
		fk_release();
	}

	/* The MediaTek front end reads the ring itself, so the hardware
	 * pointer runs past the application pointer during a hold and the
	 * write reconciles it instead of stopping the pair. Six periods, not a
	 * whole number of wrap points: the model folds the pointer at two ring
	 * lengths, so a hold of exactly that much would alias to no movement
	 * at all. */
	CASE("a hold on a cyclic playback dma expires periods and never xruns");
	{
		struct tc_hold_arm mtk = { .prof = &fk_mt7986, .hold = 6,
					   .silence = true };

		CHECK(tc_hold_open(&t, &cfg, &mtk));
		CHECK(tc_ticks_wait(&t, &st, 100));
		CHECK_UINT(st.xruns, 0);
		CHECK_UINT(st.write_errors, 0);
		CHECK_UINT(st.holds, 1);
		CHECK_UINT(st.cal_restarts, 1);
		CHECK(st.playback_expired > 0);
		CHECK(tc_stop(&t));
		tc_counters(&t, &ctr);
		CHECK(ctr.playback_expired > 0);
		tc_free(&t);
		fk_release();
	}

	CASE("a quiet run on either profile never opens an episode");
	{
		const struct fk_profile *prof[2] = { &fk_an7581, &fk_mt7986 };
		const unsigned sync_cap[2] = { 2, 3 };

		for (unsigned i = 0; i < 2; i++) {
			struct tc_hold_arm quiet = {
				.prof = prof[i],
				.jit = prof[i] == &fk_an7581,
				.silence = prof[i] == &fk_mt7986,
			};

			CHECK(tc_hold_open(&t, &cfg, &quiet));
			CHECK(tc_ticks_wait(&t, &st, 2000));
			CHECK_UINT(st.holds, 0);
			CHECK_UINT(st.capture_stalls, 0);
			CHECK_UINT(st.delay_stalls, 0);
			CHECK_UINT(st.capture_drops, 0);
			CHECK_UINT(st.trim_drops, 0);
			CHECK_UINT(st.playback_expired, 0);
			CHECK_UINT(st.xruns, 0);
			CHECK_UINT(st.cal_restarts, 0);
			/* The loop at rest: the playback queue is the prefill
			 * and the read left no backlog. */
			CHECK_UINT((uint64_t)st.loop_ref, 240);
			CHECK_UINT((uint64_t)st.delay_dev_last, 0);
			CHECK_UINT(tc_hold_lines(), 0);
			tc_free(&t);
			/* The call budget of the tick, pinned so the catch-up
			 * rule cannot grow it: the hold detection reads the
			 * pointer pair the read's own push already refreshed,
			 * and the three fresh reads run inside an episode
			 * only. Per tick the AN7581 makes one capture push,
			 * one capture refresh beside delay() and one playback
			 * push; the sample-accurate profile adds the pointer
			 * read of its wake rule. One wait per tick on capture
			 * and none on playback, because the write finds room
			 * at rest. The constants are the open and the
			 * prefill. */
			CHECK_UINT(fk_sync_n[FK_CAPTURE],
				   sync_cap[i] * fk_poll_n[FK_CAPTURE] + 1u);
			CHECK_UINT(fk_sync_n[FK_PLAY],
				   fk_poll_n[FK_CAPTURE] + 5u);
			CHECK_UINT(fk_poll_n[FK_PLAY], 0);
			fk_release();
		}
	}

	/*
	 * One line per run in which a figure moved. Two runs with nothing
	 * new must add none, and a replacement engine counts from zero again,
	 * so the mark it carries must reset instead of reporting a drop.
	 */
	CASE("the hold record covers one watchdog run and one engine");
	{
		arm.hold = 6;
		CHECK(tc_hold_open(&t, &cfg, &arm));
		CHECK(tc_ticks_wait(&t, &st, 100));
		CHECK(tc_log_wait(UDSP_LOG_DEBUG, "engine hold", 500));
		CHECK_UINT(tc_hold_lines(), 1);
		eh_sleep_ms(60);
		CHECK_UINT(tc_hold_lines(), 1);
		CHECK(tc_stop(&t));
		CHECK(tc_start(&t));
		CHECK(tc_ticks_wait(&t, &st, 50));
		CHECK_UINT(st.holds, 0);
		eh_sleep_ms(60);
		CHECK_UINT(tc_hold_lines(), 1);
		tc_free(&t);
		fk_release();
	}

	unsetenv("UDSP_WATCHDOG_MS");
}

/* The production cadence applies when the variable is unset, so the override
 * cannot hide a wrong constant. */
static void case_watchdog_default(void)
{
	/* A sign is the trap strtoul wraps instead of refusing, so the parser
	 * refuses anything that does not start with a decimal digit. */
	static const char *const bad[] = {
		"-18446744073709551615", "-1", "+20", "20x", " 20", "",
		"0", "nonsense", "18446744073709551616",
	};

	CASE("the watchdog cadence defaults to one second");
	unsetenv("UDSP_WATCHDOG_MS");
	CHECK_UINT(udsp_watchdog_ms_get(), 1000u);
	setenv("UDSP_WATCHDOG_MS", "20", 1);
	CHECK_UINT(udsp_watchdog_ms_get(), 20u);
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		setenv("UDSP_WATCHDOG_MS", bad[i], 1);
		CHECK_UINT(udsp_watchdog_ms_get(), 1000u);
	}
	unsetenv("UDSP_WATCHDOG_MS");
}

/*
 * The media clock of a sending stream against a skip. The instrumented back
 * end reports the periods as overwritten, which is the soft overrun of tdm.h,
 * so the engine counts them, steps its tick clock over them and calls the skip
 * hook. The far end reads the gap in the timestamps: one packet per tick at
 * ptime 10, so every step is one frame except the one the skip covers.
 *
 * The second case takes the run clock. The staged frames of a 30 ms packet
 * leave the box from inside the skip hook, so that packet is the first
 * transmit and its record carries the clock the skipped periods already
 * reached, not the clock of the tick before them. The tick is paced at 20 ms
 * so the skip falls inside the three ticks the first packet takes to fill.
 */
static void skip_clock_steps(unsigned *jumps, unsigned *steps)
{
	*jumps = 0;
	*steps = 0;
	for (unsigned i = 1; i < tc_ts_n; i++) {
		uint32_t step = tc_ts[i] - tc_ts[i - 1];

		if (step == 80)
			(*steps)++;
		else if (step == 7 * 80)
			(*jumps)++;
	}
}

static void case_skip_clock(void)
{
	const struct udsp_event *ev;
	struct udsp_stream *st;
	struct tc_stream_desc d;
	struct tc_stream w;
	unsigned jumps, steps;
	uint32_t t0, stamp;
	struct eh t;

	CASE("a skip steps the media clock by the periods it covers");
	if (!tc_up(&t, "USA")) {
		CHECK(false);
		return;
	}
	CHECK(tc_stream_up(&t, &w, 0));
	tc_ts_reset();
	tc_ts_wait(&t, &w, 100);
	CHECK(tc_ts_n > 2);
	atomic_store_explicit(&tc_be.skip_periods, 6u, memory_order_release);
	tc_ts_wait(&t, &w, 200);
	CHECK(tc_ts_n > 10);
	skip_clock_steps(&jumps, &steps);
	/* One packet carries the gap, and it carries the whole gap: the
	 * skipped periods plus the tick that reported them. */
	CHECK_UINT(jumps, 1);
	CHECK_UINT(steps, tc_ts_n - 2);
	tc_stream_down(&t, &w, 0);
	tc_down(&t);

	CASE("a packet the skip flushes carries the clock of its own tick");
	if (!tc_up_paced(&t, &tc_ops, "USA", 20000)) {
		CHECK(false);
		return;
	}
	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL);
	tc_stream_desc_default(&d);
	d.ptime_ms = 30;
	CHECK(tc_stream_config_desc(&t, &w, 0, &d));
	t0 = udsp_now_ms(t.s);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	atomic_store_explicit(&tc_be.skip_periods, 6u, memory_order_release);
	tc_stream_wait(&t, &w, 300);
	ev = eh_ev_find(UDSP_EV_RTP_FIRST_TX, 0);
	CHECK(ev != NULL);
	/* One tick plus the six periods it stepped over. The slack is the
	 * ticks the arming can slip by. */
	stamp = ev ? ev->stamp_ms : 0;
	CHECK(stamp >= t0 + 70);
	CHECK(stamp <= t0 + 110);
	if (!tc_stream_down(&t, &w, 0) || !st) {
		CHECK(false);
		tc_down(&t);
		return;
	}
	CHECK(st->rtp_tx_ms >= stamp);
	tc_down(&t);
}

/*
 * A finite cadence that runs out inside a skip. The skipped frames are
 * rendered and thrown away, so the slot reaches its end with no tick to
 * finalize it; the answer must stand until the next tick, which stops the slot
 * and posts the end exactly once. A segmented tone is the one form that starts
 * its cadence again on a render past the end, so the region here gives the
 * special information tone three short segments and one cycle.
 */
static bool tc_region_sit_once(void)
{
	return tc_region_sit_write("SIT", "segment = 914 -10 30\n"
				   "segment = 1371 -10 30\n"
				   "segment = 1777 -10 30\n"
				   "silence = 30\nrepeat = 1\n");
}

static void tc_region_sit_drop(void)
{
	tc_region_dir_drop("SIT.conf");
}

static void case_playout_skip(void)
{
	struct eh t;

	CASE("a cadence that ends inside a skip ends once and stays ended");
	{
		if (!tc_region_sit_once()) {
			CHECK(false);
			return;
		}
		CHECK(tc_up_paced(&t, &tc_ops, "SIT", 2000));
		eh_rec_mark();
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_SIT, 0) == UDSP_OK);
		eh_wait_ms(&t, 20);
		/* Twenty periods of skipped time cover the whole 120 ms
		 * cadence, so its last cycle ends with no tick of its own. */
		atomic_store_explicit(&tc_be.skip_periods, 20u,
				      memory_order_release);
		eh_wait_ms(&t, 400);
		CHECK_UINT(eh_ev_count(UDSP_EV_AUDIO_END), 1);
		/* A restart would put a segment here. */
		CHECK(eh_energy(0, 160) > 10000.0);
		CHECK(eh_energy(1600, 800) <= 100.0);
		tc_down(&t);
		tc_region_sit_drop();
	}
}

/*
 * The reference rule without an engine (core/chan.c chan_lec_measure and
 * chan_lec_line_decide), on line 0 of a tracker. The 640 and 800 readings the
 * AN7581 alternates between are one period apart at 16 kHz. The vector
 * therefore runs at that rate, with a 160 sample period.
 */
#define TC_REF_RATE	16000u
#define TC_REF_FRAME	160u

/* The decision of one line against the offer that stands. */
static void tc_ref_line_check(struct chan_lec_track *tr, unsigned line,
			      enum chan_lec_verdict want, uint32_t gen_delta,
			      uint32_t want_delay)
{
	uint32_t gen = tr->line[line].gen;
	uint32_t delay = 0;

	CHECK_UINT(chan_lec_line_decide(tr, line, TC_REF_RATE, TC_REF_FRAME,
					&delay), want);
	CHECK_UINT(tr->line[line].gen - gen, gen_delta);
	CHECK_UINT(delay, want_delay);
}

/* One tick on line 0: the measurement, then the decision. gen_delta counts
 * the moves of both, because the consumers compare generation levels. */
static void tc_ref_check(struct chan_lec_track *tr, struct engine_stats *st,
			 enum chan_lec_verdict want, uint32_t gen_delta,
			 uint32_t want_delay)
{
	uint32_t gen = tr->line[0].gen;
	uint32_t delay = 0;

	chan_lec_measure(tr, st);
	CHECK_UINT(chan_lec_line_decide(tr, 0, TC_REF_RATE, TC_REF_FRAME,
					&delay), want);
	CHECK_UINT(tr->line[0].gen - gen, gen_delta);
	CHECK_UINT(delay, want_delay);
}

/* A tracker with one measured value applied on line 0, which is the
 * alignment a converged filter stands on. */
static void tc_ref_seed(struct chan_lec_track *tr, struct engine_stats *st,
			uint64_t serial, int32_t loop)
{
	uint32_t delay = 0;

	memset(tr, 0, sizeof(*tr));
	st->serial = serial;
	st->cal_restarts = 0;
	st->delay_calibrated = true;
	st->loop_ref = loop;
	chan_lec_measure(tr, st);
	chan_lec_line_decide(tr, 0, TC_REF_RATE, TC_REF_FRAME, &delay);
}

static void case_lec_ref_rule(void)
{
	struct chan_lec_track tr;
	struct engine_stats st;

	CASE("the 640 and 800 phases are one reference");
	{
		memset(&st, 0, sizeof(st));
		st.prefill_periods = 3;
		st.frames_per_period = TC_REF_FRAME;

		/* The first engine: the serial move carries the generation,
		 * and the value that follows it applies verbatim. */
		tc_ref_seed(&tr, &st, 1, 480);
		CHECK_UINT(tr.line[0].gen, 1);
		CHECK_UINT(tr.line[0].delay, 320);

		/* One period above, with the window restarted under it: the
		 * same reference, and the restart alone decides nothing. */
		st.loop_ref = 640;
		st.cal_restarts = 1;
		tc_ref_check(&tr, &st, CHAN_LEC_KEEP, 0, 320);

		/* Two periods above: a move, and the taps go with it. */
		st.loop_ref = 800;
		tc_ref_check(&tr, &st, CHAN_LEC_RETIME, 1, 640);

		/* The flip back, and the one above: both inside the period. */
		st.loop_ref = 640;
		tc_ref_check(&tr, &st, CHAN_LEC_KEEP, 0, 640);
		st.loop_ref = 960;
		tc_ref_check(&tr, &st, CHAN_LEC_KEEP, 0, 640);
		st.loop_ref = 1120;
		tc_ref_check(&tr, &st, CHAN_LEC_RETIME, 1, 960);

		/* A window that restarted and completed at the value already
		 * applied is the same alignment. */
		st.cal_restarts = 2;
		tc_ref_check(&tr, &st, CHAN_LEC_KEEP, 0, 960);

		/* A replacement engine zeroes the filters, so the tolerance
		 * protects nothing and its first measurement applies whatever
		 * stood before. 560 is half a period from 480, which the
		 * tolerance would otherwise swallow. The measurement moves the
		 * generation, and the first value of the line is a KEEP. */
		tc_ref_seed(&tr, &st, 1, 480);
		st.serial = 2;
		st.loop_ref = 560;
		tc_ref_check(&tr, &st, CHAN_LEC_KEEP, 1, 400);

		/* An open window inside the allowance publishes nothing. */
		st.delay_calibrated = false;
		st.cal_restarts = 2;
		tc_ref_check(&tr, &st, CHAN_LEC_HOLD, 0, 400);

		/* Past the allowance a measured value stands instead of the
		 * nominal guess. */
		tc_ref_seed(&tr, &st, 3, 800);
		st.delay_calibrated = false;
		st.cal_restarts = 3;
		tc_ref_check(&tr, &st, CHAN_LEC_KEEP, 0, 640);

		/* With nothing applied the nominal prefill becomes the
		 * standing value, because the filters adapt against it from
		 * there. A window that completes two periods away then moves
		 * it like any other measurement. */
		memset(&tr, 0, sizeof(tr));
		st.serial = 4;
		st.cal_restarts = 0;
		st.delay_calibrated = false;
		tc_ref_check(&tr, &st, CHAN_LEC_HOLD, 1, 0);
		st.cal_restarts = 3;
		tc_ref_check(&tr, &st, CHAN_LEC_KEEP, 0, 320);
		st.delay_calibrated = true;
		st.loop_ref = 800;
		tc_ref_check(&tr, &st, CHAN_LEC_RETIME, 1, 640);

		/* The same nominal, and a window that completes half a period
		 * from it: the guess stands. */
		memset(&tr, 0, sizeof(tr));
		st.serial = 5;
		st.cal_restarts = 0;
		st.delay_calibrated = false;
		tc_ref_check(&tr, &st, CHAN_LEC_HOLD, 1, 0);
		st.cal_restarts = 3;
		tc_ref_check(&tr, &st, CHAN_LEC_KEEP, 0, 320);
		st.delay_calibrated = true;
		st.loop_ref = 560;
		tc_ref_check(&tr, &st, CHAN_LEC_KEEP, 0, 320);

		/* The compare is on the measurement and not on the clamp. 3840
		 * publishes 3680 and 4160 publishes the reach bound, so the
		 * two published values differ and a clamped compare would
		 * still see the move. */
		tc_ref_seed(&tr, &st, 6, 3840);
		CHECK_UINT(tr.line[0].delay, 3680);
		st.loop_ref = 4160;
		tc_ref_check(&tr, &st, CHAN_LEC_RETIME, 1, 3936);

		/* Both raw values above the reach: the bound publishes 3936
		 * for each of them, so only the measurement can tell the two
		 * references apart. */
		tc_ref_seed(&tr, &st, 7, 4160);
		CHECK_UINT(tr.line[0].delay, 3936);
		st.loop_ref = 4480;
		tc_ref_check(&tr, &st, CHAN_LEC_RETIME, 1, 3936);
	}
}

/*
 * The canceller against a hold. Every episode restarts the delay calibration,
 * so the canceller freezes until the new window completes; a window that
 * completes must not spend the allowance the fallback counts, or a run of
 * holds would leave a later open window publishing a guessed reference. The
 * tick is paced at 5 ms so a sixteen tick window is 80 ms of wall time.
 *
 * What the new window latches decides the rest. The model runs at 8 kHz with
 * a prefill of 3 and a granted ring of 4. It reads a loop of 240 frames at
 * rest, and of 400 after any six period hold: the engine refills the ring to
 * its rest depth and tc_be.hold_shift (TC_HOLD_SHIFT) adds two periods to
 * the reading. tc_be.delay_bias moves that figure by one period at a time.
 * A case can therefore put the re-latched value inside the tolerance or
 * outside it.
 *
 * The model hybrid is indexed by writes, and reads and writes stay one to one
 * per tick through a hold and a trim. The echo of a tick is therefore the
 * downlink of three ticks before it, for the life of the run. The reference
 * stands one period under the loop, so a kept 160 puts that echo at tap 80
 * and a re-timed 320 puts it before tap 0. A case that re-times
 * reads the frame count, the re-time count and the divergence count, never an
 * echo return loss. The mechanism is what the host model can prove.
 */
static bool tc_lec_ready(struct eh *t)
{
	return atomic_load_explicit(&t->s->lec_ready[0], memory_order_relaxed);
}

/* One hold episode, watched from the freeze to the release. frozen, when
 * given, takes the last record published while adaptation was held, which is
 * where a filter that kept its taps still carries their energy. */
/* The ready flag and the record are two observations, and the tick can
 * complete the window between them. The record's own ref_ready is what says
 * the reading belongs to the frozen window. */
static void tc_lec_frozen_take(struct eh *t, struct udsp_lec_stats *frozen)
{
	struct udsp_lec_stats ls;

	if (!frozen || udsp_line_lec_stats(t->s, 0, &ls) != UDSP_OK)
		return;
	if (ls.ref_ready)
		return;
	*frozen = ls;
}

static bool tc_lec_window_watch(struct eh *t, struct udsp_lec_stats *frozen)
{
	uint64_t end = eh_mono_us() + 3000000u;
	bool froze = false;

	if (frozen)
		memset(frozen, 0, sizeof(*frozen));
	atomic_store_explicit(&tc_be.hold_periods, 6u, memory_order_release);
	while (eh_mono_us() < end) {
		if (tc_lec_ready(t) && froze)
			return true;
		if (!tc_lec_ready(t)) {
			froze = true;
			tc_lec_frozen_take(t, frozen);
		}
		eh_sleep_ms(1);
	}

	return false;
}

/* One hold episode, watched on the engine and not on a line: a line without
 * a canceller never becomes ready. The wait ends when the restart count moved
 * and the new window completed. */
static bool tc_engine_window_watch(struct eh *t)
{
	uint64_t end = eh_mono_us() + 3000000u;
	struct engine_stats st;
	uint64_t restarts;

	tc_engine_stats(t, &st);
	restarts = st.cal_restarts;
	atomic_store_explicit(&tc_be.hold_periods, 6u, memory_order_release);
	while (eh_mono_us() < end) {
		tc_engine_stats(t, &st);
		if (st.cal_restarts != restarts && st.delay_calibrated)
			return true;
		eh_sleep_ms(1);
	}

	return false;
}

/* The handle every case below shares. The lines run on the ring model, off
 * hook, with a canceller on every line below ports. A dial tone plays, and the
 * model hybrid returns it three periods later. Line 0 carries a stream. The
 * line is seized because the tick runs no canceller on an open loop. */
static bool tc_lec_hold_up_ports(struct eh *t, struct tc_stream *w,
				 unsigned ports)
{
	struct udsp_chan_cfg ccfg;

	if (!tc_up_paced_ports(t, &tc_hold_ops, "USA", 5000, ports) ||
	    !eh_hook_seize(t))
		return false;
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.ec_enable = true;
	ccfg.ec_nlp_enable = true;
	ccfg.ec_tail_ms = 32;
	for (unsigned i = 0; i < ports; i++) {
		if (udsp_chan_config(t->s, i, &ccfg) != UDSP_OK)
			return false;
	}
	atomic_store(&tc_be.echo_periods, 3u);
	atomic_store(&tc_be.echo_shift, 2u);
	if (!tc_stream_up(t, w, 0))
		return false;

	return udsp_tone_play(t->s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK;
}

static bool tc_lec_hold_up(struct eh *t, struct tc_stream *w)
{
	return tc_lec_hold_up_ports(t, w, 1);
}

/* Runs the filter on the hybrid until it has taken energy from it. A hold that
 * follows then has an alignment to keep or to lose. */
static bool tc_lec_converge(struct eh *t, struct udsp_lec_stats *ls)
{
	uint64_t end = eh_mono_us() + 20000000u;

	do {
		eh_wait_ms(t, 100);
		if (udsp_line_lec_stats(t->s, 0, ls) != UDSP_OK)
			continue;
		if (ls->erle_mdb >= 6000 && ls->coef_energy_micro > 0)
			return true;
	} while (eh_mono_us() < end);

	return false;
}

static bool tc_lec_settled(struct eh *t, struct udsp_lec_stats *ls)
{
	eh_wait_ms(t, 200);

	return udsp_line_lec_stats(t->s, 0, ls) == UDSP_OK;
}

/* Polls the record until the processor acts, in the shape of
 * tc_lec_converge. */
static bool tc_lec_nlp_wait(struct eh *t, struct udsp_lec_stats *ls)
{
	uint64_t end = eh_mono_us() + 5000000u;

	do {
		eh_wait_ms(t, 20);
		if (udsp_line_lec_stats(t->s, 0, ls) == UDSP_OK &&
		    ls->nlp_active)
			return true;
	} while (eh_mono_us() < end);

	return false;
}

/* Polls the record until the filter ran count frames past the record in ls.
 * The count and not the wall time bounds the wait, because the pace of the
 * tick is a property of the case. */
static bool tc_lec_frames_wait(struct eh *t, struct udsp_lec_stats *ls,
			       uint32_t count)
{
	uint32_t from = ls->frames;
	uint64_t end = eh_mono_us() + 5000000u;

	do {
		eh_wait_ms(t, 5);
		if (udsp_line_lec_stats(t->s, 0, ls) == UDSP_OK &&
		    ls->frames >= from + count)
			return true;
	} while (eh_mono_us() < end);

	return false;
}

/*
 * The peak tap of a filter on the model hybrid. The hybrid returns the
 * playback of three ticks before, a loop of 240 samples at 8 kHz. The
 * reference stands one period under the loop at 160, so the echo sits at tap
 * 80. A dial tone does not pin the solution to that tap. A delay of 91
 * samples is within half a sample of four periods of 350 Hz and of five
 * periods of 440 Hz, so tap 171 carries almost the same echo, and the filter
 * can hold its largest coefficient on either lobe. A window of 8 taps around
 * each lobe takes the neighbor taps of an onset. It still refuses a peak
 * counted from the loop (160) and the 0 of an empty filter. The check holds on
 * the plain backend only: on the ring model of tc_lec_hold_up, tap 0 grows
 * larger than tap 80 under the same tone (case_lec_verdicts).
 */
static bool tc_lec_lobe_match(uint32_t tap)
{
	return (tap >= 72u && tap <= 88u) || (tap >= 163u && tap <= 179u);
}

static bool tc_lec_fallback_watch(struct eh *t, unsigned ms)
{
	struct engine_stats st;

	for (unsigned i = 0; i < ms; i++) {
		eh_sleep_ms(1);
		tc_engine_stats(t, &st);
		if (!st.delay_calibrated && tc_lec_ready(t))
			return true;
	}

	return false;
}

/* Holds closer together than a window is long, so no window ever completes. */
static bool tc_lec_fallback_hunt(struct eh *t)
{
	for (unsigned i = 0; i < 8; i++) {
		atomic_store_explicit(&tc_be.hold_periods, 6u,
				      memory_order_release);
		if (tc_lec_fallback_watch(t, 40))
			return true;
	}

	return false;
}

static void case_lec_hold(void)
{
	/* The two tolerance cases run on one handle and one filter. The record
	 * the first took before its hold is what the second compares the
	 * restarted count against. */
	struct udsp_lec_stats kept_before;
	uint32_t kept_marked;
	struct engine_stats st;
	struct tc_stream kept_w;
	struct eh t;

	CASE("three hold episodes re-time the canceller once");
	{
		struct udsp_lec_stats before, first, ls;
		uint32_t marked, elapsed;
		struct tc_stream w;

		if (!tc_lec_hold_up(&t, &w)) {
			CHECK(false);
			return;
		}
		CHECK(tc_lec_converge(&t, &before));
		marked = eh_now_ms();
		CHECK_UINT(before.ref_delay_samples, 160u);
		CHECK_UINT(before.retimes, 0);
		CHECK(before.frames > 0);

		/* The first episode moves the loop two periods, which is
		 * beyond the tolerance, so the taps go and the count moves. */
		CHECK(tc_lec_window_watch(&t, NULL));
		CHECK(tc_lec_settled(&t, &first));
		CHECK_UINT(first.retimes, 1);
		CHECK_UINT(first.ref_delay_samples, 320u);
		/* The frame count started over, which is the reset itself.
		 * Without a reset, the count reads the earlier count plus the
		 * frames of engine time since the mark. Late ticks move both
		 * sides alike, so the check is against that sum and not against
		 * the earlier count alone. The 8 frames cover the tick that the
		 * two reads straddle. */
		elapsed = (eh_now_ms() - marked) / 10u;
		CHECK(first.frames + 8u < before.frames + elapsed);

		/* The next two fall on the value that now stands, so they cost
		 * nothing at all. */
		for (unsigned i = 0; i < 2; i++)
			CHECK(tc_lec_window_watch(&t, NULL));
		CHECK(tc_lec_settled(&t, &ls));
		CHECK_UINT(ls.retimes, 1);
		CHECK(ls.frames > first.frames);
		CHECK_UINT(ls.ref_delay_samples, 320u);
		CHECK_UINT(ls.diverge_resets, 0);
		tc_engine_stats(&t, &st);
		CHECK_UINT(st.holds, 3);
		CHECK_UINT(st.cal_restarts, 3);
		CHECK(st.delay_calibrated);
		CHECK(tc_stream_down(&t, &w, 0));
		tc_down(&t);
	}

	CASE("a hold whose reference stands one period away keeps the taps");
	{
		struct udsp_lec_stats frozen, ls;

		if (!tc_lec_hold_up(&t, &kept_w)) {
			CHECK(false);
			return;
		}
		CHECK(tc_lec_converge(&t, &kept_before));
		kept_marked = eh_now_ms();
		CHECK_UINT(kept_before.ref_delay_samples, 160u);

		/* One period under the model's own figure, applied while the
		 * calibration stands, so the drift rule absorbs the step and
		 * the episode rules see nothing. The hold then latches 320
		 * against the 240 the taps stand one period under. */
		atomic_store_explicit(&tc_be.delay_bias, -80L,
				      memory_order_release);
		CHECK(tc_lec_window_watch(&t, &frozen));
		CHECK(tc_lec_settled(&t, &ls));
		CHECK_UINT(ls.ref_delay_samples, 160u);
		CHECK_UINT(ls.retimes, 0);
		CHECK(ls.frames > kept_before.frames);
		/* The taps stood through the frozen window and carry the same
		 * energy after it. */
		CHECK(frozen.coef_energy_micro > 0);
		CHECK_UINT(frozen.ref_ready, 0);
		CHECK(ls.coef_energy_micro > 0);
		CHECK(ls.erle_mdb >= 6000);
		tc_engine_stats(&t, &st);
		CHECK_UINT(st.holds, 1);
		CHECK_UINT(st.cal_restarts, 1);
		CHECK_UINT((uint64_t)st.loop_ref, 320u);
	}

	/* The same handle and the same filter: the bias goes back one period,
	 * which puts the next latched value two periods from the one that
	 * stands. */
	CASE("a hold whose reference moves two periods re-times and counts");
	{
		struct udsp_lec_stats ls;
		uint32_t elapsed;

		atomic_store_explicit(&tc_be.delay_bias, 0L,
				      memory_order_release);
		CHECK(tc_lec_window_watch(&t, NULL));
		CHECK(tc_lec_settled(&t, &ls));
		CHECK_UINT(ls.ref_delay_samples, 320u);
		CHECK_UINT(ls.retimes, 1);
		/* The first case explains the check against engine time. */
		elapsed = (eh_now_ms() - kept_marked) / 10u;
		CHECK(ls.frames + 8u < kept_before.frames + elapsed);
		CHECK_UINT(ls.diverge_resets, 0);
		CHECK(tc_stream_down(&t, &kept_w, 0));
		tc_down(&t);
	}

	CASE("a canceller created after a re-time starts clean and counts "
	     "nothing");
	{
		struct udsp_lec_stats ls;
		uint32_t frames;
		struct tc_stream w;

		if (!tc_lec_hold_up(&t, &w)) {
			CHECK(false);
			return;
		}
		CHECK(tc_lec_converge(&t, &ls));
		CHECK_UINT(ls.ref_delay_samples, 160u);
		CHECK(tc_stream_down(&t, &w, 0));
		eh_wait_ms(&t, 100);
		CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_UNSUPPORTED);

		/* The loop moves with no canceller on the line. A line without
		 * an instance decides nothing, so no generation moves, and the
		 * instance made afterwards takes the offer of 400 verbatim. The
		 * wait watches the engine, because the line never becomes
		 * ready without an instance. */
		CHECK(tc_engine_window_watch(&t));
		CHECK(tc_stream_up(&t, &w, 0));
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
		CHECK(tc_lec_settled(&t, &ls));
		CHECK_UINT(ls.ref_delay_samples, 320u);
		CHECK_UINT(ls.retimes, 0);
		frames = ls.frames;
		CHECK(frames > 0);
		CHECK(tc_lec_settled(&t, &ls));
		CHECK_UINT(ls.retimes, 0);
		CHECK(ls.frames > frames);
		CHECK(tc_stream_down(&t, &w, 0));
		tc_down(&t);
	}

	CASE("a canceller replaced while its media does not run publishes "
	     "a fresh count");
	{
		struct udsp_lec_stats ls;
		struct tc_stream w;

		if (!tc_lec_hold_up(&t, &w)) {
			CHECK(false);
			return;
		}
		CHECK(tc_lec_converge(&t, &ls));
		CHECK(tc_lec_window_watch(&t, NULL));
		CHECK(tc_lec_settled(&t, &ls));
		CHECK_UINT(ls.retimes, 1);

		/* The count belongs to the line and goes with the instance,
		 * whatever address the allocator hands the next one. The new
		 * canceller never runs, so only the publish describes it. */
		CHECK(tc_stream_down(&t, &w, 0));
		eh_wait_ms(&t, 100);
		CHECK(tc_stream_config(&t, &w, 0));
		CHECK(tc_lec_settled(&t, &ls));
		CHECK_UINT(ls.retimes, 0);
		CHECK_UINT(ls.frames, 0);
		CHECK_UINT(ls.ec_active, 0);
		CHECK(tc_stream_down(&t, &w, 0));
		tc_down(&t);
	}

	CASE("an open window still releases the canceller");
	{
		struct udsp_lec_stats ls;
		struct tc_stream w;

		if (!tc_lec_hold_up(&t, &w)) {
			CHECK(false);
			return;
		}
		CHECK(tc_lec_converge(&t, &ls));
		CHECK_UINT(ls.ref_delay_samples, 160u);
		/* No window completes, so the fallback decides. The measured
		 * value stands through it and adaptation is released again on
		 * the reference the taps already carry. */
		CHECK(tc_lec_fallback_hunt(&t));
		CHECK(tc_lec_ready(&t));
		tc_engine_stats(&t, &st);
		CHECK(!st.delay_calibrated);
		CHECK_UINT(atomic_load_explicit(&t.s->lec_delay[0],
						memory_order_relaxed), 160u);
		CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
		CHECK_UINT(ls.retimes, 0);
		CHECK(tc_stream_down(&t, &w, 0));
		tc_down(&t);
	}
}

/* The refusal shapes of the fourth open record need no real-time privilege,
 * because the formatter takes a synthetic cap. */
static void case_rt_line(void)
{
	struct udsp_cap cap;
	enum udsp_log_level level;
	char line[UDSP_LOG_MAX];
	uint8_t slic = UDSP_RT_POLICY_FIFO;

	CASE("a granted request is an INFO record naming the pin");
	memset(&cap, 0, sizeof(cap));
	cap.rt_policy = UDSP_RT_POLICY_FIFO;
	cap.rt_net_policy = UDSP_RT_POLICY_FIFO;
	cap.rt_mlock = true;
	cap.rt_cpu = 3;
	cap.rt_prio = 50;
	udsp_rt_line_format(&cap, 3, slic, &level, line, sizeof(line));
	CHECK_UINT(level, UDSP_LOG_INFO);
	CHECK(!strcmp(line, "open: rt fifo 50 granted, engine fifo, net fifo, "
			    "slic fifo, cpu 3, mlock yes"));

	CASE("a refused scheduler request is a WARN record with the errno");
	cap.rt_policy = UDSP_RT_POLICY_OTHER;
	cap.rt_net_policy = UDSP_RT_POLICY_OTHER;
	cap.rt_sched_errno = EPERM;
	udsp_rt_line_format(&cap, 3, slic, &level, line, sizeof(line));
	CHECK_UINT(level, UDSP_LOG_WARN);
	CHECK(!strcmp(line, "open: rt fifo 50 refused: Operation not permitted, "
			    "engine other, net other, slic fifo, cpu 3, mlock yes"));

	CASE("a refused pin names the cpu the config asked for");
	cap.rt_policy = UDSP_RT_POLICY_FIFO;
	cap.rt_net_policy = UDSP_RT_POLICY_FIFO;
	cap.rt_sched_errno = 0;
	cap.rt_cpu = -1;
	cap.rt_affinity_errno = EINVAL;
	udsp_rt_line_format(&cap, 3, slic, &level, line, sizeof(line));
	CHECK_UINT(level, UDSP_LOG_WARN);
	CHECK(!strcmp(line, "open: rt fifo 50 granted, engine fifo, net fifo, "
			    "slic fifo, cpu 3 refused: Invalid argument, mlock yes"));

	CASE("a refused lock is a WARN record with the errno");
	cap.rt_cpu = 3;
	cap.rt_affinity_errno = 0;
	cap.rt_mlock = false;
	cap.rt_mlock_errno = ENOMEM;
	udsp_rt_line_format(&cap, 3, slic, &level, line, sizeof(line));
	CHECK_UINT(level, UDSP_LOG_WARN);
	CHECK(!strcmp(line, "open: rt fifo 50 granted, engine fifo, net fifo, "
			    "slic fifo, cpu 3, mlock refused: Cannot allocate memory"));

	CASE("a policy that could not be read prints as unknown");
	cap.rt_mlock_errno = 0;
	cap.rt_mlock = true;
	cap.rt_net_policy = UDSP_RT_POLICY_UNKNOWN;
	udsp_rt_line_format(&cap, 3, slic, &level, line, sizeof(line));
	CHECK_UINT(level, UDSP_LOG_INFO);
	CHECK(!strcmp(line, "open: rt fifo 50 granted, engine fifo, "
			    "net unknown, slic fifo, cpu 3, mlock yes"));
}

/*
 * The text sink of udsp_config.log carries what no return value does. It
 * carries the reason an open failed, the rate settle, and what the open
 * settled on. The fake plug-in takes no host hook, so these cases check the
 * lines that the library itself writes. test_slic_host checks the hook itself.
 */
static void case_log_sink(void)
{
	struct udsp_config cfg;
	struct eh t;

	CASE("the open records name the tdm, the rate and the plug-ins");
	tc_backend_reset();
	udsp_slic_fake_reset();
	udsp_tdm_override(&tc_ops);
	tc_cfg(&cfg, "tc", NULL);
	CHECK(tc_open_handle(&t, &cfg));
	if (!t.s) {
		CHECK(false);
		return;
	}
	CHECK(tc_log_has(UDSP_LOG_INFO, "open: tdm file - -, 2 ch, 8000 Hz, "
			 "80 frames/period, prefill 3"));
	CHECK(tc_log_has(UDSP_LOG_INFO, "open: slic fake"));
	CHECK(tc_log_has(UDSP_LOG_INFO, "2 line(s) of 2 port(s), level poll"));
	CHECK(tc_log_has(UDSP_LOG_INFO,
			 "open: slic plug-in (fake), fax plug-in none"));
	CHECK(tc_log_has(UDSP_LOG_INFO, "open: rt not requested, engine other, "
			 "net other, slic other, unpinned, mlock no"));
	CHECK_UINT(tc_log_count(UDSP_LOG_ERR), 0);
	CHECK_UINT(tc_log_count(UDSP_LOG_WARN), 0);

	CASE("the capability carries the outcome the record names");
	{
		const struct udsp_cap *cap = udsp_cap(t.s);

		CHECK_UINT(cap->rt_policy, UDSP_RT_POLICY_OTHER);
		CHECK_UINT(cap->rt_net_policy, UDSP_RT_POLICY_OTHER);
		CHECK_UINT((unsigned)cap->rt_prio, 0);
		CHECK(cap->rt_cpu == -1);
		CHECK_UINT((unsigned)cap->rt_sched_errno, 0);
		CHECK(!cap->rt_mlock);
	}

	CASE("a debug line is dropped until the slic log bit is set");
	udsp_log(t.s, UDSP_LOG_DEBUG, "probe %d", 1);
	CHECK(!tc_log_has(UDSP_LOG_DEBUG, "probe 1"));
	udsp_debug_set(t.s, UDSP_DEBUG_SLIC_LOG);
	udsp_log(t.s, UDSP_LOG_DEBUG, "probe %d", 2);
	CHECK(tc_log_has(UDSP_LOG_DEBUG, "probe 2"));
	udsp_debug_set(t.s, 0);
	udsp_log(t.s, UDSP_LOG_DEBUG, "probe %d", 3);
	CHECK(!tc_log_has(UDSP_LOG_DEBUG, "probe 3"));
	CHECK_UINT(tc_log_count(UDSP_LOG_DEBUG), 1);
	tc_free(&t);

	CASE("a spidev that will not open fails the open and says so");
	tc_backend_reset();
	udsp_slic_fake_reset();
	tc_cfg(&cfg, "tc", NULL);
	snprintf(cfg.slic_plugin, sizeof(cfg.slic_plugin),
		 "/nonexistent/plugin.so");
	snprintf(cfg.spidev, sizeof(cfg.spidev), "/nonexistent/spidev9.9");
	CHECK(!tc_open_handle(&t, &cfg));
	CHECK(tc_log_has(UDSP_LOG_ERR, "spidev /nonexistent/spidev9.9: "
			 "No such file or directory"));
	CHECK_UINT(tc_log_count(UDSP_LOG_INFO), 0);
	tc_free(&t);

	CASE("a configuration without a sink opens and stays silent");
	tc_backend_reset();
	udsp_slic_fake_reset();
	tc_cfg(&cfg, "tc", NULL);
	cfg.log = NULL;
	CHECK(tc_open_handle(&t, &cfg));
	if (!t.s) {
		CHECK(false);
		return;
	}
	udsp_log(t.s, UDSP_LOG_ERR, "unheard");
	CHECK_UINT(tc_log_count(UDSP_LOG_INFO), 0);
	CHECK_UINT(tc_log_count(UDSP_LOG_ERR), 0);
	tc_free(&t);

	/* The two refusals before a handle exists take the sink straight from
	 * the configuration. */
	CASE("a refused configuration names the field, before any handle");
	tc_backend_reset();
	udsp_slic_fake_reset();
	tc_cfg(&cfg, "tc", NULL);
	cfg.channels = 9;
	CHECK(!tc_open_handle(&t, &cfg));
	CHECK(tc_log_has(UDSP_LOG_ERR, "the configuration is refused: channels"));
	tc_free(&t);

	CASE("a second handle in the process is refused and says so");
	tc_backend_reset();
	udsp_slic_fake_reset();
	tc_cfg(&cfg, "tc", NULL);
	CHECK(tc_open_handle(&t, &cfg));
	if (!t.s) {
		CHECK(false);
		return;
	}
	CHECK(udsp_open(&cfg, t.tx_fd) == NULL);
	CHECK(tc_log_has(UDSP_LOG_ERR, "a handle is open already"));
	tc_free(&t);

	/*
	 * The rate limit of the bus read lines runs on the monotonic clock,
	 * because it bounds the syslog volume, while the test backend's engine
	 * clock runs several times faster than real time, so the waits here
	 * are real. One failed hook read logs at once; the recovery logs once
	 * after a second of clean reads; a second failure inside the ten
	 * second window logs nothing, and so opens no run that a second
	 * recovery line would close. The fake arms a failure per op and not
	 * per channel, and every line keeps its own window, so the case runs
	 * one populated port: two lines could take the two failures apart and
	 * each log once.
	 */
	CASE("a failing bus read logs once per window and once on recovery");
	tc_backend_reset();
	udsp_slic_fake_reset();
	tc_cfg(&cfg, "tc", "USA");
	cfg.populated_ports = 1;
	CHECK(tc_open_handle(&t, &cfg));
	if (!t.s) {
		CHECK(false);
		return;
	}
	CHECK(tc_start(&t));
	eh_wait_ms(&t, 100);
	tc_log_reset();
	udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_HOOK_READ, -EIO);
	tc_wait_real_ms(&t, 100);
	CHECK_UINT(tc_log_hits(UDSP_LOG_ERR, "slic hook read failed on line"), 1);
	CHECK_UINT(tc_log_hits(UDSP_LOG_INFO, "recovered"), 0);
	tc_wait_real_ms(&t, 1300);
	CHECK_UINT(tc_log_hits(UDSP_LOG_INFO, "slic reads on line"), 1);
	udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_HOOK_READ, -EIO);
	tc_wait_real_ms(&t, 100);
	CHECK_UINT(tc_log_hits(UDSP_LOG_ERR, "slic hook read failed on line"), 1);
	tc_wait_real_ms(&t, 1300);
	CHECK_UINT(tc_log_hits(UDSP_LOG_INFO, "slic reads on line"), 1);
	tc_down(&t);
}

/*
 * The interface of the line canceller. The getter is a FASTPATH read of what
 * the tick published, so this case drives the canceller for real against the
 * paced backend. It reads the record from the control thread while the engine
 * runs, the only way that a target uses the interface.
 *
 * The hybrid is attenuated (tc_be.echo_shift). A unity echo is not a line: the
 * Geigel detector expects a hybrid at or under the gain bound of dsp/lec.c. An
 * echo as loud as the playback therefore reads as near-end speech, and the
 * filter holds instead of adapting.
 */
static void case_lec_stats(void)
{
	struct udsp_lec_stats ls;
	struct udsp_chan_cfg ccfg;
	struct tc_stream w;
	struct eh t;

	CASE("the canceller getter refuses what it has no line for");
	{
		memset(&ls, 0xa5, sizeof(ls));
		CHECK(udsp_line_lec_stats(NULL, 0, &ls) == UDSP_UNSUPPORTED);
		/* The record is cleared before the refusal, so a caller that
		 * ignores the status still reads zeros. */
		CHECK_UINT(ls.tail_ms, 0);
		CHECK_UINT(ls.echo_gain_micro, 0);
	}

	if (!tc_up(&t, "USA")) {
		CHECK(false);
		return;
	}
	/* Off hook for every case on this handle, because the tick runs no
	 * canceller on an open loop. */
	CHECK(eh_hook_seize(&t));

	CASE("a line that runs no canceller is skipped, not answered");
	{
		CHECK(udsp_line_lec_stats(t.s, 0, NULL) == UDSP_UNSUPPORTED);
		CHECK(udsp_line_lec_stats(t.s, UDSP_LINE_MAX, &ls) ==
		      UDSP_UNSUPPORTED);
		/* No stream, so core/stream.c stream_lec_ensure has made no
		 * canceller for this line yet. */
		CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_UNSUPPORTED);
	}

	/* The instance is made at stream config and takes the tail the channel
	 * carries (core/stream.c stream_lec_ensure), so the channel goes first
	 * and the tail it asks for is the tail the record must report. */
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.ec_enable = true;
	ccfg.ec_nlp_enable = true;
	ccfg.ec_tail_ms = 32;
	CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
	if (!tc_stream_config(&t, &w, 0)) {
		CHECK(false);
		tc_down(&t);
		return;
	}
	eh_wait_ms(&t, 50);

	CASE("a configured line whose media does not run reads an idle filter");
	{
		/* The instance is made at the configuration, and the tick gates
		 * the run on the line's media. The record therefore measures a
		 * filter that ran nothing, not one the configuration allows. */
		CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
		CHECK_UINT(ls.ec_active, 0);
		CHECK_UINT(ls.nlp_enabled, 0);
		CHECK_UINT(ls.frames, 0);
		CHECK_UINT(ls.tail_ms, 32);
		CHECK_UINT(ls.passthrough, UDSP_PASSTHROUGH_OFF);
	}

	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	/*
	 * Long enough for the engine's delay calibration to settle, and no
	 * longer harmful to the case: the line is silent, so the reference is
	 * inactive, no frame adapts and the two learned figures stay where the
	 * instance was created with them.
	 */
	eh_wait_ms(&t, 300);

	CASE("a fresh canceller publishes the state it was created with");
	{
		CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
		CHECK_UINT(ls.tail_ms, 32);
		CHECK_UINT(ls.taps, 32u * UDSP_SAMPLE_RATE / 1000u);
		/* Nothing has adapted yet, and the two scaled figures start
		 * apart: the filter carries no energy and the learned gain
		 * bound stands at the LEC_GEIGEL_GAIN_MIN of dsp/lec.c, which
		 * is 0.25 and reads as 250000 micro-units. */
		CHECK_UINT(ls.coef_energy_micro, 0);
		CHECK_UINT(ls.echo_gain_micro, 250000);
		CHECK(ls.erle_mdb == 0);
		CHECK_UINT(ls.diverge_resets, 0);
		CHECK_UINT(ls.probes, 0);
		CHECK_UINT(ls.double_talk, 0);
		CHECK_UINT(ls.nlp_active, 0);
		/* The reference alignment belongs to the line, and the line
		 * takes the measurement on the first frame of its canceller. It
		 * therefore stands before this canceller learned anything: the
		 * engine measured three periods of loop here. A reading like
		 * this one, aligned and with no energy in the filter, is a
		 * canceller that was let adapt and learned nothing. */
		CHECK_UINT(ls.ref_ready, 1);
		CHECK_UINT(ls.ref_delay_samples, 2u * UDSP_SAMPLE_RATE / 100u);
		/* The stream runs, so the tick put the filter on every frame of
		 * it under the voice policy. */
		CHECK_UINT(ls.ec_active, 1);
		CHECK_UINT(ls.passthrough, UDSP_PASSTHROUGH_OFF);
	}

	CASE("a canceller that ran against a hybrid reports what it learned");
	{
		/* Three periods is the engine's own loop delay here, so the
		 * hybrid returns what the line played exactly as far back as
		 * the canceller's reference reaches. */
		tc_be.echo_periods = 3;
		tc_be.echo_shift = 2;
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 3000) == UDSP_OK);
		eh_wait_ms(&t, 800);
		CHECK(atomic_load(&t.s->lec_ready[0]));
		CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
		CHECK(ls.frames > 0);
		/* Frames whose update stood are the difference between the
		 * two, so an adapting canceller holds fewer than it ran. */
		CHECK(ls.hold_frames < ls.frames);
		/* The filter took energy from the hybrid it canceled, which
		 * is the reading the field exists for. */
		CHECK(ls.coef_energy_micro > 0);
		CHECK(ls.echo_gain_micro > 0);
		/* The tracked return loss enhancement moved off the zero the
		 * fresh instance published, which is the figure the field
		 * carries in milli-decibels (measured 57 dB here). */
		CHECK(ls.erle_mdb > 0);
		/* A converged filter against a hybrid this clean leaves the
		 * non-linear processor holding the residual, which is the
		 * reading an operator sees on a healthy line. */
		CHECK_UINT(ls.nlp_active, 1);
		CHECK_UINT(ls.tail_ms, 32);
		CHECK_UINT(ls.diverge_resets, 0);
		printf("# coef_peak_tap %u\n", ls.coef_peak_tap);
		CHECK(tc_lec_lobe_match(ls.coef_peak_tap));
		/* The record carries what the tick published for the line,
		 * not a constant of its own. */
		CHECK_UINT(ls.ref_ready, 1);
		CHECK_UINT(ls.ref_delay_samples,
			   (unsigned)atomic_load(&t.s->lec_delay[0]));
		CHECK_UINT(ls.ref_delay_samples, 2u * UDSP_SAMPLE_RATE / 100u);
		CHECK_UINT(ls.ec_active, 1);
		CHECK_UINT(ls.passthrough, UDSP_PASSTHROUGH_OFF);
	}

	CASE("the record follows the line it was asked for");
	{
		/* Line 1 is populated and carries no stream, so it runs no
		 * canceller while line 0 answers: a getter that indexed the
		 * wrong line would answer for both. */
		CHECK(udsp_line_lec_stats(t.s, 1, &ls) == UDSP_UNSUPPORTED);
		CHECK_UINT(ls.frames, 0);
		CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
		CHECK(ls.frames > 0);
	}

	CASE("a fax profile reads a bypassed filter and frozen counters");
	{
		uint32_t frames;

		/* The profile bypasses the canceller whole, so the instance
		 * stays and the record describes a filter that ran nothing. */
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_OK);
		eh_wait_ms(&t, 50);
		CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
		CHECK_UINT(ls.ec_active, 0);
		CHECK_UINT(ls.nlp_enabled, 0);
		CHECK_UINT(ls.passthrough, UDSP_PASSTHROUGH_MODEM);
		/* The tail belongs to the instance, so it stands through the
		 * bypass and tells a live row from a cleared one. */
		CHECK_UINT(ls.tail_ms, 32);
		frames = ls.frames;
		eh_wait_ms(&t, 50);
		CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
		CHECK_UINT(ls.frames, frames);

		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_OFF, 0, 0) ==
		      UDSP_OK);
		eh_wait_ms(&t, 50);
		CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
		CHECK_UINT(ls.ec_active, 1);
		CHECK_UINT(ls.passthrough, UDSP_PASSTHROUGH_OFF);
	}

	CASE("a channel that switches the canceller off still answers");
	{
		ccfg.ec_enable = false;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		eh_wait_ms(&t, 50);
		CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
		CHECK_UINT(ls.ec_active, 0);
		CHECK_UINT(ls.nlp_enabled, 0);
		/* The instance outlives the switch: the line releases it with
		 * its last stream. Switching the canceller back on therefore
		 * puts the same filter back on the frame. */
		ccfg.ec_enable = true;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		eh_wait_ms(&t, 50);
		CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
		CHECK_UINT(ls.ec_active, 1);
		CHECK_UINT(ls.tail_ms, 32);
	}

	CASE("the reading does not outlive the canceller");
	{
		tc_be.echo_periods = 0;
		tc_be.echo_shift = 0;
		tc_stream_down(&t, &w, 0);
		eh_wait_ms(&t, 50);
		CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_UNSUPPORTED);
		CHECK_UINT(ls.frames, 0);
	}

	tc_down(&t);

	/*
	 * A dead line is not a line without a canceller, and the two answers
	 * are different. The library releases nothing a dead line still holds,
	 * so the surface says so instead of reading as a line that simply has
	 * nothing to report. The line is killed through ten consecutive hook
	 * read failures, because that is the path a target takes.
	 */
	CASE("a dead line answers fatal and not a missing canceller");
	{
		bool line_down;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.ec_enable = true;
		ccfg.ec_nlp_enable = true;
		ccfg.ec_tail_ms = 32;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		if (!tc_stream_up(&t, &w, 0)) {
			CHECK(false);
			tc_down(&t);
			return;
		}
		eh_wait_ms(&t, 100);
		CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);

		line_down = tc_hook_fail_wait(&t, UDSP_HOOK_FAIL_LIMIT, 1,
					      eh_mono_us() + 5000000u);
		CHECK(line_down);
		if (!line_down)
			goto dead_line_out;
		memset(&ls, 0xa5, sizeof(ls));
		CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_FATAL);
		/* Cleared before the refusal, like every other answer here. */
		CHECK_UINT(ls.tail_ms, 0);
		CHECK_UINT(ls.frames, 0);
		CHECK_UINT(ls.echo_gain_micro, 0);
	dead_line_out:
		udsp_slic_fake_error_set(t.s->slic,
					 UDSP_SLIC_FAKE_HOOK_READ, 0);
		tc_stream_down(&t, &w, 0);
		tc_down(&t);
	}
}

#define TC_REGION_DIR_STR	UDSP_REGION_DIR

static _Atomic bool tc_region_hammer_stop;

/* The failure count travels the return value: the tu counters are not
 * thread-safe, so the thread asserts nothing itself. */
static void *tc_region_hammer(void *arg)
{
	udsp_t *s = arg;
	uintptr_t bad = 0;

	while (!atomic_load(&tc_region_hammer_stop)) {
		struct udsp_region_view view;

		if (udsp_region_get(s, &view) != UDSP_OK) {
			bad++;
			continue;
		}
		if (strcmp(view.iso3, "USA") && strcmp(view.iso3, "GBR"))
			bad++;
	}
	return (void *)bad;
}

static void case_region(void)
{
	struct udsp_job job = { 0 };
	char path[512];
	int32_t status = -1;
	struct eh t;

	/* The probe needs no handle, and the override directory proves that it
	 * reads the path the load reads. */
	CASE("the region probe finds a shipped file and misses an absent one");
	{
		CHECK(udsp_region_probe("USA") == 0);
		CHECK(udsp_region_probe("DEU") == -ENOENT);
		CHECK(udsp_region_probe("") == -EINVAL);
		CHECK(tc_region_fast());
		CHECK(udsp_region_probe("FST") == 0);
		CHECK(udsp_region_probe("USA") == -ENOENT);
		tc_region_fast_drop();
	}

	CASE("region load swaps the tone plan");
	{
		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		snprintf(path, sizeof(path), "%s/GBR.conf", TC_REGION_DIR_STR);
		CHECK(udsp_region_load(t.s, path, &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 3000, &status));
		CHECK_UINT((uint32_t)status, 0);

		snprintf(path, sizeof(path), "%s/ETS.conf", TC_REGION_DIR_STR);
		CHECK(udsp_region_load(t.s, path, &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 3000, &status));
		CHECK_UINT((uint32_t)status, 0);

		eh_rec_mark();
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
		eh_wait_ms(&t, 300);
		/* ETS dial tone is a single 425 Hz, not the 350 + 440 pair. */
		CHECK(eh_power(400, 800, 425.0) > eh_power(400, 800, 350.0) * 8.0);
		tc_down(&t);
	}

	CASE("a region swap keeps the core hook windows");
	{
		struct udsp_chan_cfg ccfg;
		struct eh_gate gate = { .fd = { -1, -1 } };
		uint8_t st_state[16], st_pulse[16];
		unsigned got;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		memset(&ccfg, 0, sizeof(ccfg));
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(udsp_line_hook_timing(t.s, 0, 300, 600, 1301) == UDSP_OK);
		snprintf(path, sizeof(path), "%s/GBR.conf", TC_REGION_DIR_STR);
		CHECK(udsp_region_load(t.s, path, &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 3000, &status));
		CHECK_UINT((uint32_t)status, 0);

		if (!tc_hook_gate_park(&gate, &t) ||
		    !tc_hook_hold(&gate, &t, false, 10) ||
		    !tc_hook_hold(&gate, &t, true, 40) ||
		    !tc_hook_hold(&gate, &t, false, 40) ||
		    !tc_hook_hold(&gate, &t, true, 40) ||
		    !tc_hook_report_wait(&gate, &t, UDSP_HOOK_FLASH))
			goto region_hook_out;

		got = tc_hook_events(st_state, st_pulse, 16);
		CHECK(got >= 3);
		/* GBR calls 35..145 ms a flash; the core window the caller set
		 * is 300..600 and survives the swap. */
		CHECK_UINT(st_state[2], UDSP_HOOK_FLASH);
	region_hook_out:
		eh_gate_disarm(&gate);
		tc_down(&t);
		eh_gate_close(&gate);
	}

	CASE("a bad region file leaves the old plan in place");
	{
		static const char junk[] = "this is not a region file\n";
		char bad[] = "/tmp/udsp_core_bad_XXXXXX";
		int fd;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(udsp_region_load(t.s, "/nonexistent/XXX.conf", &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 3000, &status));
		CHECK(status == UDSP_UNSUPPORTED);
		CHECK(tc_log_has(UDSP_LOG_ERR, "region file /nonexistent/XXX.conf: "
				 "No such file or directory"));

		/* A file the parser opens and rejects, written here so the case
		 * depends on no other file's size or syntax: a file above
		 * UDSP_REGION_FILE_MAX fails with -EFBIG, which is a different
		 * status. */
		fd = mkstemp(bad);
		CHECK(fd >= 0);
		if (fd >= 0) {
			CHECK(write(fd, junk, sizeof(junk) - 1) ==
			      (ssize_t)(sizeof(junk) - 1));
			close(fd);
		}
		CHECK(udsp_region_load(t.s, bad, &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 3000, &status));
		CHECK(status == UDSP_UNSUPPORTED);
		CHECK(tc_log_has(UDSP_LOG_ERR, "region file /tmp/udsp_core_bad_"));
		CHECK(tc_log_has(UDSP_LOG_ERR, ": line 1: "));
		unlink(bad);

		eh_rec_mark();
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
		eh_wait_ms(&t, 300);
		CHECK(eh_power(400, 800, 350.0) > eh_power(400, 800, 425.0) * 8.0);
		tc_down(&t);
	}

	CASE("the region getter answers the loaded plan");
	{
		struct udsp_region_view view;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		memset(&view, 0, sizeof(view));
		CHECK(udsp_region_get(t.s, &view) == UDSP_OK);
		CHECK(!strcmp(view.iso3, "USA"));
		CHECK_UINT(view.cid_format, UDSP_CIDF_BELLCORE_FSK);
		CHECK_UINT(view.mwi_format, UDSP_CIDF_BELLCORE_FSK);
		CHECK_UINT(view.hook_flash_min_ms, 300);
		CHECK_UINT(view.hook_flash_max_ms, 1300);
		CHECK_UINT(view.hook_release_min_ms, 1301);
		CHECK_UINT(view.ring_count, 8);
		CHECK_UINT(view.ring[0].cadence_count, 1);
		CHECK_UINT(view.ring[0].cadence[0].on_ms, 2000);
		CHECK_UINT(view.ring[0].cadence[0].off_ms, 4000);
		CHECK_UINT(view.cw_sas_total_ms, 20600);
		CHECK_UINT(view.cw_count, 4);
		CHECK_UINT(view.cw[0].present, 1);
		CHECK_UINT(view.cw[0].lead_ms, 300);
		CHECK_UINT(view.cw[0].total_ms, 20600);
		CHECK_UINT(view.cw[1].present, 1);
		CHECK_UINT(view.cw[1].lead_ms, 300);	/* c1, the plain pattern */
		CHECK_UINT(view.cw[2].lead_ms, 300);	/* c2 */
		CHECK_UINT(view.cw[3].lead_ms, 500);	/* c3 */
		CHECK_UINT(view.cw[4].lead_ms, 700);	/* c4 */
		CHECK_UINT(view.cw[4].present, 1);
		CHECK_UINT(view.cw[4].total_ms, 21400);
		CHECK_UINT(view.flash_service, UDSP_FLASH_SERVICE_DROP);
		CHECK(!strcmp(view.describe, "United States"));
		CHECK_UINT(view.confirm_total_ms, 600);

		CHECK(udsp_region_load(t.s, "GBR", &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 3000, &status));
		CHECK_UINT((uint32_t)status, 0);
		CHECK(udsp_region_get(t.s, &view) == UDSP_OK);
		CHECK(!strcmp(view.iso3, "GBR"));
		CHECK_UINT(view.cid_format, UDSP_CIDF_ETSI_LR_DTAS);
		CHECK_UINT(view.mwi_format, UDSP_CIDF_ETSI_LR_DTAS);
		CHECK_UINT(view.hook_flash_min_ms, 35);
		CHECK_UINT(view.hook_flash_max_ms, 145);
		CHECK_UINT(view.hook_release_min_ms, 590);
		CHECK_UINT(view.cw_sas_total_ms, 8200);
		CHECK_UINT(view.cw_count, 0);
		CHECK_UINT(view.cw[0].present, 1);
		CHECK_UINT(view.cw[0].lead_ms, 100);
		CHECK_UINT(view.flash_service, UDSP_FLASH_SERVICE_DROP);
		CHECK(!strcmp(view.describe, "United Kingdom"));
		CHECK_UINT(view.confirm_total_ms, 20000);
		tc_down(&t);
	}

	CASE("the getter answers a finite and an endless confirm tone");
	{
		struct udsp_region_view view;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(udsp_region_load(t.s, "NZL", &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 3000, &status));
		CHECK_UINT((uint32_t)status, 0);
		CHECK(udsp_region_get(t.s, &view) == UDSP_OK);
		CHECK(!strcmp(view.iso3, "NZL"));
		CHECK_UINT(view.confirm_total_ms, 3000);

		CHECK(udsp_region_load(t.s, "AUS", &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 3000, &status));
		CHECK_UINT((uint32_t)status, 0);
		CHECK(udsp_region_get(t.s, &view) == UDSP_OK);
		CHECK(!strcmp(view.iso3, "AUS"));
		CHECK_UINT(view.confirm_total_ms, 0);
		tc_down(&t);
	}

	CASE("a segmented call waiting tone bounds nothing and has no lead");
	{
		static const struct region_edit edit[] = {
			{ "[tone call_waiting]", "freq = 440\nlevel = -13\n"
			  "cadence = 300 10000\nrepeat = 2\n",
			  "segment = 440 -13 300\nsilence = 10000\nrepeat = 2\n" },
			{ NULL, "iso3 = USA", "iso3 = CWS" },
		};
		char text[REGION_EDIT_TEXT_MAX];
		struct udsp_region_view view;

		if (!region_text_edit(UDSP_REGION_DIR "/USA.conf", text,
				      sizeof(text), edit, 2) ||
		    !tc_region_dir_write("/tmp/udsp_cws_XXXXXX", "CWS.conf", text)) {
			CHECK(false);
			return;
		}
		if (!tc_up(&t, "CWS")) {
			CHECK(false);
			tc_region_dir_drop("CWS.conf");
			return;
		}
		CHECK(udsp_region_get(t.s, &view) == UDSP_OK);
		CHECK_UINT(view.cw_sas_total_ms, 0);
		CHECK_UINT(view.cw[0].total_ms, 0);
		CHECK_UINT(view.cw[0].lead_ms, 0);
		/* The variants stay simple tones and keep their bounds. */
		CHECK_UINT(view.cw[1].total_ms, 20600);
		tc_down(&t);
		tc_region_dir_drop("CWS.conf");
	}

	CASE("the getter refuses before a region parses");
	{
		struct udsp_config cfg;
		struct udsp_region_view view;

		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_tdm_override(&tc_ops);
		tc_cfg(&cfg, "tc", NULL);
		cfg.populated_ports = 1;
		CHECK(tc_open_handle(&t, &cfg));
		if (!t.s) {
			udsp_tdm_override(NULL);
			return;
		}
		CHECK(udsp_region_get(t.s, &view) == UDSP_UNSUPPORTED);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a forever repeat bounds nothing");
	{
		static const char from[] = "cadence = 300 10000\nrepeat = 2";
		static const char to[] = "cadence = 300 10000\nrepeat = forever";
		static char text[UDSP_REGION_FILE_MAX + 16];
		struct udsp_region_view view;
		char dir[64], fpath[128], upath[256];
		char *at;
		size_t n, tail;
		FILE *f;

		snprintf(dir, sizeof(dir), "/tmp/udsp_core_cwf_XXXXXX");
		CHECK(mkdtemp(dir) != NULL);
		snprintf(upath, sizeof(upath), "%s/USA.conf", TC_REGION_DIR_STR);
		f = fopen(upath, "rb");
		CHECK(f != NULL);
		if (!f)
			return;
		n = fread(text, 1, sizeof(text) - 8, f);
		fclose(f);
		text[n] = '\0';
		at = strstr(text, from);
		CHECK(at != NULL);
		if (!at)
			return;
		tail = strlen(at + strlen(from));
		memmove(at + strlen(to), at + strlen(from), tail + 1);
		memcpy(at, to, strlen(to));
		snprintf(fpath, sizeof(fpath), "%s/CWF.conf", dir);
		f = fopen(fpath, "wb");
		CHECK(f != NULL);
		if (f) {
			fwrite(text, 1, strlen(text), f);
			fclose(f);
		}

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(udsp_region_load(t.s, fpath, &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 3000, &status));
		CHECK_UINT((uint32_t)status, 0);
		CHECK(udsp_region_get(t.s, &view) == UDSP_OK);
		CHECK(!strcmp(view.iso3, "USA"));
		CHECK_UINT(view.cw_sas_total_ms, 0);
		tc_down(&t);
		unlink(fpath);
		rmdir(dir);
	}

	CASE("the getter is clean against concurrent loads");
	{
		pthread_t thread;
		void *bad = NULL;
		unsigned i;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		atomic_store(&tc_region_hammer_stop, false);
		CHECK(!pthread_create(&thread, NULL, tc_region_hammer, t.s));
		for (i = 0; i < 4; i++) {
			CHECK(udsp_region_load(t.s, i & 1 ? "USA" : "GBR",
					       &job) == UDSP_OK);
			CHECK(eh_job_wait(&t, job.token, 3000, &status));
			CHECK_UINT((uint32_t)status, 0);
		}
		atomic_store(&tc_region_hammer_stop, true);
		CHECK(!pthread_join(thread, &bad));
		CHECK_UINT((unsigned)(uintptr_t)bad, 0);
		tc_down(&t);
	}
}

/*
 * The metallic line test. The fake carries mlt_run, so every handle these
 * cases open reports the capability, and the record the op writes is the
 * fake's own: an eight byte head with the test id at byte 1 and a clear status
 * byte, then a four byte little-endian count of the calls that channel took.
 * libudsp copies those bytes and decodes none of them (udsp.h).
 *
 * A job completion carries a status and not an errno (cmd.h
 * cmd_job_status_map). A run that did not finish completes -EBUSY and reads as
 * UDSP_TRANSIENT, with the reason in the record it wrote; a length the record
 * cannot hold completes -EPROTO and reads as UDSP_FATAL.
 */
#define TC_MLT_LEN	12u
/* The head alone, which is what a run that ended early reports. */
#define TC_MLT_HEAD_LEN	8u
/* One byte past the record the contract carries (slic.h data[512]). */
#define TC_MLT_LEN_OVER	513

static uint32_t tc_mlt_calls(const uint8_t *rec)
{
	return (uint32_t)rec[8] | ((uint32_t)rec[9] << 8) |
	       ((uint32_t)rec[10] << 16) | ((uint32_t)rec[11] << 24);
}

static const struct udsp_event *tc_hook_ev_find(unsigned line, unsigned from)
{
	for (unsigned i = from; i < eh_ev_len; i++) {
		if (eh_ev[i].type == UDSP_EV_HOOK && eh_ev[i].line == line)
			return &eh_ev[i];
	}

	return NULL;
}

static bool tc_hook_booted(struct eh *t, const void *arg)
{
	(void)arg;
	for (unsigned line = 0; line < t->s->cap.lines; line++) {
		if (!tc_hook_ev_find(line, 0))
			return false;
	}

	return true;
}

/* The emitter thread writes the boot report of a line some time after the
 * engine posts it. A case that counts the hook events therefore takes its
 * first count after the boot report of every line. */
static bool tc_hook_boot_wait(struct eh *t)
{
	return tc_poll_until(t, tc_hook_booted, NULL, 5000);
}

static void case_line_test_cap(void)
{
	struct udsp_config cfg;
	struct eh t;

	CASE("the capability carries the line test bit with the fake");
	{
		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_tdm_override(&tc_ops);
		tc_cfg(&cfg, "tc", NULL);
		CHECK(tc_open_handle(&t, &cfg));
		if (t.s)
			CHECK(t.s->cap.features & UDSP_FEAT_LINE_TEST);
		tc_free(&t);

		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_slic_fake_optional_set(UDSP_SLIC_FAKE_OPT_ALL &
					    ~(uint32_t)UDSP_SLIC_FAKE_OPT_MLT);
		tc_cfg(&cfg, "tc", NULL);
		CHECK(tc_open_handle(&t, &cfg));
		if (t.s) {
			struct udsp_job job = { 0 };

			CHECK_UINT(t.s->cap.features & UDSP_FEAT_LINE_TEST, 0);
			/* No op behind it, so there is nothing to run. */
			CHECK(udsp_line_test(t.s, 0, UDSP_TEST_GR909, &job) ==
			      UDSP_UNSUPPORTED);
			CHECK_UINT(job.token, 0);
		}
		tc_free(&t);
		udsp_tdm_override(NULL);
	}
}

static void case_line_test_result(void)
{
	uint8_t buf[64];
	size_t len = 1;
	struct eh t;

	CASE("a line test completes and its result reads back byte for byte");
	{
		struct udsp_job job = { 0 };
		int32_t status = -1;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(udsp_line_test_result(t.s, 0, buf, sizeof(buf), &len) ==
		      UDSP_UNSUPPORTED);
		CHECK_UINT(len, 0);
		CHECK(udsp_line_test(t.s, 0, UDSP_TEST_GR909, &job) == UDSP_OK);
		CHECK(job.token != 0);
		CHECK(eh_job_wait(&t, job.token, 5000, &status));
		CHECK_UINT((uint32_t)status, UDSP_OK);
		{
			const struct udsp_event *ev = tc_job_find(job.token);

			CHECK(ev != NULL);
			if (ev)
				CHECK_UINT(ev->seq, 0);
		}
		memset(buf, 0xa5, sizeof(buf));
		CHECK(udsp_line_test_result(t.s, 0, buf, sizeof(buf), &len) ==
		      UDSP_OK);
		CHECK_UINT(len, TC_MLT_LEN);
		CHECK_UINT(buf[0], 0);
		CHECK_UINT(buf[1], UDSP_TEST_GR909);
		CHECK_UINT(buf[2], 0);
		CHECK_UINT(tc_mlt_calls(buf), 1);
		/* Nothing past the record was written. */
		CHECK_UINT(buf[TC_MLT_LEN], 0xa5);
		tc_down(&t);
	}

	/*
	 * A handset lifted just before the test starts. The slic thread may
	 * or may not have sampled the new level by the time the job runs, and
	 * the machine above has not answered it either way, so only the job's
	 * own fresh sample can tell. The plug-in must not be called, and the
	 * seize still reaches the core.
	 */
	CASE("a line test on a line just taken off hook is refused, and the "
	     "seize still reaches the core");
	{
		struct udsp_slic_fake_chan_state st;
		struct udsp_job job = { 0 };
		int32_t status = -1;
		bool seized = false;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(tc_hook_boot_wait(&t));
		tc_hook_set(&t, true);
		CHECK(udsp_line_test(t.s, 0, UDSP_TEST_GR909, &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 5000, &status));
		CHECK_UINT((uint32_t)status, UDSP_TRANSIENT);
		CHECK(udsp_line_test_result(t.s, 0, buf, sizeof(buf), &len) ==
		      UDSP_UNSUPPORTED);
		eh_wait_ms(&t, 300);
		for (unsigned i = 0; i < eh_ev_len; i++) {
			if (eh_ev[i].type == UDSP_EV_HOOK && eh_ev[i].line == 0 &&
			    eh_ev[i].u.hook.state == UDSP_HOOK_SEIZE)
				seized = true;
		}
		CHECK(seized);
		tc_hook_set(&t, false);
		CHECK(tc_stop(&t));
		CHECK(tc_state(&t, 0, &st));
		CHECK_UINT(st.mlt_count, 0);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a second line test invalidates the first result and replaces it");
	{
		struct udsp_job job = { 0 };
		bool gap = false;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(tc_test_run(&t, 0, UDSP_TEST_GR909, NULL));
		CHECK(udsp_line_test_result(t.s, 0, buf, sizeof(buf), &len) ==
		      UDSP_OK);
		CHECK_UINT(tc_mlt_calls(buf), 1);

		tc_mlt_delay(&t, 200);
		CHECK(udsp_line_test(t.s, 0, UDSP_TEST_ROH, &job) == UDSP_OK);
		for (unsigned i = 0; i < 2000 && !gap; i++) {
			if (udsp_line_test_result(t.s, 0, buf, sizeof(buf),
						  &len) == UDSP_UNSUPPORTED)
				gap = true;
			else
				eh_sleep_ms(1);
		}
		/* The record is invalidated when the job starts, so a reader
		 * between the start and the completion has no result at all. */
		CHECK(gap);
		CHECK(eh_job_wait(&t, job.token, 5000, NULL));
		tc_mlt_delay(&t, 0);
		CHECK(udsp_line_test_result(t.s, 0, buf, sizeof(buf), &len) ==
		      UDSP_OK);
		CHECK_UINT(len, TC_MLT_LEN);
		CHECK_UINT(buf[1], UDSP_TEST_ROH);
		CHECK_UINT(tc_mlt_calls(buf), 2);
		tc_down(&t);
	}

	CASE("each line test kind reaches the plug-in with its own id");
	{
		static const enum udsp_line_test kind[4] = {
			UDSP_TEST_GR909, UDSP_TEST_ROH, UDSP_TEST_CAPACITANCE,
			UDSP_TEST_LOOP,
		};
		struct udsp_slic_fake_chan_state st;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		for (unsigned i = 0; i < 4; i++) {
			int32_t status = -1;

			CHECK(tc_test_run(&t, 0, kind[i], &status));
			CHECK_UINT((uint32_t)status, UDSP_OK);
			CHECK(udsp_line_test_result(t.s, 0, buf, sizeof(buf),
						    &len) == UDSP_OK);
			CHECK_UINT(len, TC_MLT_LEN);
			CHECK_UINT(buf[1], kind[i]);
			CHECK_UINT(tc_mlt_calls(buf), i + 1u);
		}
		CHECK(tc_stop(&t));
		CHECK(tc_state(&t, 0, &st));
		CHECK_UINT(st.mlt_count, 4);
		CHECK_UINT(st.mlt_test_id, UDSP_TEST_LOOP);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a short cap copies what fits and reports the whole length");
	{
		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(tc_test_run(&t, 0, UDSP_TEST_GR909, NULL));
		memset(buf, 0xa5, sizeof(buf));
		CHECK(udsp_line_test_result(t.s, 0, buf, 4, &len) == UDSP_OK);
		CHECK_UINT(len, TC_MLT_LEN);
		CHECK_UINT(buf[1], UDSP_TEST_GR909);
		CHECK_UINT(buf[4], 0xa5);
		/* A call that only measures takes no buffer. */
		len = 0;
		CHECK(udsp_line_test_result(t.s, 0, NULL, 0, &len) == UDSP_OK);
		CHECK_UINT(len, TC_MLT_LEN);
		/* A cap with nowhere to put the bytes is refused. */
		len = 1;
		CHECK(udsp_line_test_result(t.s, 0, NULL, 8, &len) ==
		      UDSP_UNSUPPORTED);
		CHECK_UINT(len, 0);
		tc_down(&t);
	}

	CASE("a missing result and a line out of range are unsupported");
	{
		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		len = 1;
		CHECK(udsp_line_test_result(t.s, 1, buf, sizeof(buf), &len) ==
		      UDSP_UNSUPPORTED);
		CHECK_UINT(len, 0);
		len = 1;
		CHECK(udsp_line_test_result(t.s, UDSP_LINE_MAX, buf,
					    sizeof(buf), &len) ==
		      UDSP_UNSUPPORTED);
		CHECK_UINT(len, 0);
		/* The record belongs to the line that ran the test. */
		CHECK(tc_test_run(&t, 0, UDSP_TEST_GR909, NULL));
		len = 1;
		CHECK(udsp_line_test_result(t.s, 1, buf, sizeof(buf), &len) ==
		      UDSP_UNSUPPORTED);
		CHECK_UINT(len, 0);
		tc_down(&t);
	}
}

static void case_line_test_gate(void)
{
	struct tc_stream w;
	struct eh t;

	CASE("a test on a line with a running stream is transient");
	{
		struct udsp_job job = { 0 };

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(tc_stream_up(&t, &w, 0));
		CHECK(udsp_line_test(t.s, 0, UDSP_TEST_GR909, &job) ==
		      UDSP_TRANSIENT);
		CHECK_UINT(job.token, 0);
		/* The line beside it carries no call and answers. */
		CHECK(tc_test_run(&t, 1, UDSP_TEST_GR909, NULL));
		tc_stream_down(&t, &w, 0);
		CHECK(tc_test_run(&t, 0, UDSP_TEST_GR909, NULL));
		tc_down(&t);
	}

	CASE("a stream that starts before the job runs completes it busy");
	{
		struct udsp_slic_fake_chan_state st;
		struct udsp_job job = { 0 };
		int32_t status = -1;
		uint8_t buf[64];
		size_t len = 1;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 50);
		/* A record of an earlier test, so the refusal below is seen to
		 * clear it. */
		CHECK(tc_test_run(&t, 0, UDSP_TEST_GR909, NULL));
		CHECK(udsp_line_test_result(t.s, 0, buf, sizeof(buf), &len) ==
		      UDSP_OK);
		/* The thread runs one job step per pass and samples the hook
		 * after it, so a hook read that takes 200 ms parks it before
		 * the step and the stream has time to start. */
		udsp_slic_fake_delay_set(t.s->slic,
					 UDSP_SLIC_FAKE_HOOK_READ, 200);
		eh_sleep_ms(20);
		CHECK(udsp_line_test(t.s, 0, UDSP_TEST_GR909, &job) == UDSP_OK);
		CHECK(tc_stream_up(&t, &w, 0));
		udsp_slic_fake_delay_set(t.s->slic,
					 UDSP_SLIC_FAKE_HOOK_READ, 0);
		CHECK(eh_job_wait(&t, job.token, 5000, &status));
		CHECK_UINT((uint32_t)status, UDSP_TRANSIENT);
		/* A test that never ran leaves no record, which is what tells
		 * it apart from one that ran and did not finish. */
		CHECK(udsp_line_test_result(t.s, 0, buf, sizeof(buf), &len) ==
		      UDSP_UNSUPPORTED);
		CHECK_UINT(len, 0);
		tc_stream_down(&t, &w, 0);
		CHECK(tc_stop(&t));
		CHECK(tc_state(&t, 0, &st));
		/* The gate ran again on the slic thread, so the refused job
		 * added no call to the one test that did run. */
		CHECK_UINT(st.mlt_count, 1);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a test on a ringing line is transient");
	{
		struct udsp_job job = { 0 };
		struct udsp_ring ring;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		memset(&ring, 0, sizeof(ring));
		ring.cadence[0].on_ms = 2000;
		ring.cadence[0].off_ms = 4000;
		ring.cadence_count = 1;
		CHECK(udsp_ring_start(t.s, 0, &ring) == UDSP_OK);
		CHECK(udsp_line_test(t.s, 0, UDSP_TEST_GR909, &job) ==
		      UDSP_TRANSIENT);
		CHECK_UINT(job.token, 0);
		CHECK(udsp_ring_stop(t.s, 0) == UDSP_OK);
		CHECK(tc_test_run(&t, 0, UDSP_TEST_GR909, NULL));
		tc_down(&t);
	}

	CASE("a test during a caller identity burst is transient");
	{
		struct udsp_job job = { 0 };
		struct udsp_cid cid;

		if (!tc_region_fast() || !tc_up(&t, "FST")) {
			CHECK(false);
			tc_region_fast_drop();
			return;
		}
		memset(&cid, 0, sizeof(cid));
		snprintf(cid.number, sizeof(cid.number), "5551234");
		snprintf(cid.name, sizeof(cid.name), "TEST");
		/* A standalone burst drives the feed and plays the carrier on
		 * a line nothing else holds, so the gate must see it. */
		CHECK(udsp_cid_send(t.s, 0, UDSP_CID_TYPE1, &cid) == UDSP_OK);
		CHECK(udsp_line_test(t.s, 0, UDSP_TEST_GR909, &job) ==
		      UDSP_TRANSIENT);
		CHECK_UINT(job.token, 0);
		eh_wait_ms(&t, 3000);
		CHECK(tc_test_run(&t, 0, UDSP_TEST_GR909, NULL));
		tc_down(&t);
		tc_region_fast_drop();
	}

	CASE("a stream start during the test is transient and starts after");
	{
		struct udsp_job job = { 0 };
		struct tc_stream sw;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		/* Configured but not started, so the line is free when the
		 * test is submitted. */
		CHECK(tc_stream_config(&t, &sw, 0));
		tc_mlt_delay(&t, 200);
		CHECK(udsp_line_test(t.s, 0, UDSP_TEST_GR909, &job) == UDSP_OK);
		CHECK(tc_test_entered(&t, 0));
		/* The plug-in holds the line, so audio must not join it. */
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_TRANSIENT);
		CHECK(udsp_stream_config(t.s, 0, 0, NULL, -1, -1) ==
		      UDSP_TRANSIENT);
		CHECK(eh_job_wait(&t, job.token, 5000, NULL));
		tc_mlt_delay(&t, 0);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		tc_stream_down(&t, &sw, 0);
		tc_down(&t);
	}

	CASE("a caller identity burst during the test is transient");
	{
		struct udsp_job job = { 0 };
		struct udsp_cid cid;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		memset(&cid, 0, sizeof(cid));
		snprintf(cid.number, sizeof(cid.number), "5551234");
		tc_mlt_delay(&t, 200);
		CHECK(udsp_line_test(t.s, 0, UDSP_TEST_GR909, &job) == UDSP_OK);
		CHECK(tc_test_entered(&t, 0));
		/* The burst would drive the feed to on-hook transmission and
		 * play its carrier into the measurement. */
		CHECK(udsp_cid_send(t.s, 0, UDSP_CID_TYPE1, &cid) ==
		      UDSP_TRANSIENT);
		CHECK(eh_job_wait(&t, job.token, 5000, NULL));
		tc_mlt_delay(&t, 0);
		CHECK(udsp_cid_send(t.s, 0, UDSP_CID_TYPE1, &cid) == UDSP_OK);
		tc_down(&t);
	}

	CASE("a ring start and a channel config during the test are transient");
	{
		struct udsp_chan_cfg ccfg;
		struct udsp_job job = { 0 };
		struct udsp_ring ring;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		memset(&ring, 0, sizeof(ring));
		ring.cadence[0].on_ms = 2000;
		ring.cadence[0].off_ms = 4000;
		ring.cadence_count = 1;
		memset(&ccfg, 0, sizeof(ccfg));
		tc_mlt_delay(&t, 200);
		CHECK(udsp_line_test(t.s, 0, UDSP_TEST_GR909, &job) == UDSP_OK);
		CHECK(tc_test_entered(&t, 0));
		/* Ringing the line and writing its gain both reach the part
		 * the plug-in is driving. */
		CHECK(udsp_ring_start(t.s, 0, &ring) == UDSP_TRANSIENT);
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_TRANSIENT);
		CHECK(eh_job_wait(&t, job.token, 5000, NULL));
		tc_mlt_delay(&t, 0);
		CHECK(udsp_ring_start(t.s, 0, &ring) == UDSP_OK);
		CHECK(udsp_ring_stop(t.s, 0) == UDSP_OK);
		tc_down(&t);
	}

	CASE("a stop supersedes a line test that is still queued");
	{
		struct udsp_slic_fake_chan_state st;
		struct udsp_job stop = { 0 };
		struct udsp_job job = { 0 };
		int32_t status = -1;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 50);
		/* Parked in a hook read, so the test is still in the queue
		 * when the stop arrives. */
		udsp_slic_fake_delay_set(t.s->slic,
					 UDSP_SLIC_FAKE_HOOK_READ, 400);
		eh_sleep_ms(20);
		CHECK(udsp_line_test(t.s, 0, UDSP_TEST_GR909, &job) == UDSP_OK);
		CHECK(udsp_stop(t.s, &stop) == UDSP_OK);
		udsp_slic_fake_delay_set(t.s->slic,
					 UDSP_SLIC_FAKE_HOOK_READ, 0);
		CHECK(eh_job_wait(&t, job.token, 5000, &status));
		/* Nothing failed and the caller may ask again once the lines
		 * are back. */
		CHECK_UINT((uint32_t)status, UDSP_TRANSIENT);
		status = -1;
		CHECK(eh_job_wait(&t, stop.token, 5000, &status));
		CHECK_UINT((uint32_t)status, UDSP_OK);
		CHECK(tc_state(&t, 0, &st));
		/* The line went safe instead of being driven for seconds. */
		CHECK_UINT(st.mlt_count, 0);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a second line test while one is in flight is transient");
	{
		struct udsp_job first = { 0 };
		struct udsp_job second = { 0 };

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		tc_mlt_delay(&t, 200);
		CHECK(udsp_line_test(t.s, 0, UDSP_TEST_GR909, &first) ==
		      UDSP_OK);
		CHECK(udsp_line_test(t.s, 1, UDSP_TEST_GR909, &second) ==
		      UDSP_TRANSIENT);
		CHECK_UINT(second.token, 0);
		CHECK(eh_job_wait(&t, first.token, 5000, NULL));
		tc_mlt_delay(&t, 0);
		CHECK(tc_test_run(&t, 1, UDSP_TEST_GR909, NULL));
		tc_down(&t);
	}

	CASE("a sync slic op is transient during a line test");
	{
		struct udsp_job job = { 0 };

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		tc_mlt_delay(&t, 200);
		CHECK(udsp_line_test(t.s, 0, UDSP_TEST_GR909, &job) == UDSP_OK);
		CHECK(udsp_line_state(t.s, 1, UDSP_LINE_OHT) == UDSP_TRANSIENT);
		CHECK(eh_job_wait(&t, job.token, 5000, NULL));
		tc_mlt_delay(&t, 0);
		CHECK(udsp_line_state(t.s, 1, UDSP_LINE_OHT) == UDSP_OK);
		tc_down(&t);
	}

	CASE("a line test on a dead line is fatal");
	{
		struct udsp_job job = { 0 };
		bool line_down;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		line_down = tc_hook_fail_wait(&t, UDSP_HOOK_FAIL_LIMIT, 1,
					      eh_mono_us() + 5000000u);
		CHECK(line_down);
		if (!line_down)
			goto dead_line_out;
		CHECK(udsp_line_test(t.s, 0, UDSP_TEST_GR909, &job) ==
		      UDSP_FATAL);
		CHECK_UINT(job.token, 0);
	dead_line_out:
		udsp_slic_fake_error_set(t.s->slic,
					 UDSP_SLIC_FAKE_HOOK_READ, 0);
		tc_stop(&t);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}
}

static void case_line_test_fail(void)
{
	uint8_t buf[64];
	size_t len = 1;
	struct eh t;

	CASE("a run that did not finish is transient and reports its reason");
	{
		int32_t status = -1;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(tc_test_run(&t, 0, UDSP_TEST_GR909, NULL));
		CHECK(udsp_line_test_result(t.s, 0, buf, sizeof(buf), &len) ==
		      UDSP_OK);
		CHECK_UINT(len, TC_MLT_LEN);
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_MLT_RUN,
					 -EIO);
		CHECK(tc_test_run(&t, 0, UDSP_TEST_GR909, &status));
		/* The line is healthy and the caller may ask again, so the
		 * completion must not read fatal. */
		CHECK_UINT((uint32_t)status, UDSP_TRANSIENT);
		memset(buf, 0xa5, sizeof(buf));
		CHECK(udsp_line_test_result(t.s, 0, buf, sizeof(buf), &len) ==
		      UDSP_OK);
		/* The head alone, with the code that ended the run. */
		CHECK_UINT(len, TC_MLT_HEAD_LEN);
		CHECK_UINT(buf[1], UDSP_TEST_GR909);
		CHECK_UINT(buf[2], EIO);
		CHECK_UINT(buf[TC_MLT_HEAD_LEN], 0xa5);
		tc_down(&t);
	}

	CASE("a part the plug-in does not cover completes unsupported");
	{
		int32_t status = -1;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_MLT_RUN,
					 -ENOSYS);
		CHECK(tc_test_run(&t, 0, UDSP_TEST_GR909, &status));
		/* Not a broken line: the request asked a part for a test its
		 * driver does not carry. No record is kept. */
		CHECK_UINT((uint32_t)status, UDSP_UNSUPPORTED);
		CHECK(udsp_line_test_result(t.s, 0, buf, sizeof(buf), &len) ==
		      UDSP_UNSUPPORTED);
		tc_down(&t);
	}

	CASE("a record length outside the record fails the job");
	{
		struct udsp_slic_fake_chan_state st;
		int32_t status = -1;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		udsp_slic_fake_mlt_len_set(0);
		CHECK(tc_test_run(&t, 0, UDSP_TEST_GR909, &status));
		CHECK_UINT((uint32_t)status, UDSP_FATAL);
		len = 1;
		CHECK(udsp_line_test_result(t.s, 0, buf, sizeof(buf), &len) ==
		      UDSP_UNSUPPORTED);
		CHECK_UINT(len, 0);

		udsp_slic_fake_mlt_len_set(TC_MLT_LEN_OVER);
		status = -1;
		CHECK(tc_test_run(&t, 0, UDSP_TEST_GR909, &status));
		CHECK_UINT((uint32_t)status, UDSP_FATAL);
		len = 1;
		CHECK(udsp_line_test_result(t.s, 0, buf, sizeof(buf), &len) ==
		      UDSP_UNSUPPORTED);
		CHECK_UINT(len, 0);
		/* The same bound on the path a failed run takes: a length
		 * outside the record is a broken contract whatever errno came
		 * with it, and never a transient answer. */
		udsp_slic_fake_mlt_len_set(0);
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_MLT_RUN,
					 -EIO);
		status = -1;
		CHECK(tc_test_run(&t, 0, UDSP_TEST_GR909, &status));
		CHECK_UINT((uint32_t)status, UDSP_FATAL);
		len = 1;
		CHECK(udsp_line_test_result(t.s, 0, buf, sizeof(buf), &len) ==
		      UDSP_UNSUPPORTED);
		CHECK_UINT(len, 0);

		udsp_slic_fake_mlt_len_set(TC_MLT_LEN_OVER);
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_MLT_RUN,
					 -EIO);
		status = -1;
		CHECK(tc_test_run(&t, 0, UDSP_TEST_GR909, &status));
		CHECK_UINT((uint32_t)status, UDSP_FATAL);
		udsp_slic_fake_mlt_len_set(-1);

		CHECK(tc_stop(&t));
		CHECK(tc_state(&t, 0, &st));
		/* The plug-in ran twice; the two failed runs never reached the
		 * counter, because an armed error ends the call first. */
		CHECK_UINT(st.mlt_count, 2);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("closing the handle waits for a test longer than its budget");
	{
		const struct udsp_event *ev;
		struct udsp_job job = { 0 };
		uint64_t at, took;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		/* Past UDSP_STOP_SLIC_MS, so the ordinary bounded stop alone
		 * would abandon the thread and leak the handle. */
		tc_mlt_delay(&t, 6000);
		CHECK(udsp_line_test(t.s, 0, UDSP_TEST_GR909, &job) == UDSP_OK);
		at = eh_mono_us();
		udsp_close(t.s);
		took = eh_mono_us() - at;
		t.s = NULL;
		/* The op carries no cancel, so the close waits for it. */
		CHECK(took > 5500000u);
		CHECK(took < 20000000u);
		eh_ev_poll(&t);
		ev = tc_job_find(job.token);
		/* The emitter drains once more after it is told to quit, so
		 * the completion still reaches the caller. */
		CHECK(ev != NULL);
		if (ev)
			CHECK_UINT((uint32_t)ev->u.job.status, UDSP_OK);
		if (getenv("TC_DEBUG"))
			fprintf(stderr, "close over a 6 s test: %llu us\n",
				(unsigned long long)took);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}
}

static void case_line_test_hook(void)
{
	struct eh t;

	CASE("hook sampling stops on the tested line while the test runs");
	{
		struct udsp_job job = { 0 };
		unsigned before;
		bool level = true;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		CHECK(tc_hook_boot_wait(&t));
		CHECK(udsp_line_hook_query(t.s, 0, &level) == UDSP_OK);
		CHECK(!level);
		before = eh_ev_count(UDSP_EV_HOOK);

		tc_mlt_delay(&t, 200);
		CHECK(udsp_line_test(t.s, 0, UDSP_TEST_GR909, &job) == UDSP_OK);
		CHECK(tc_test_entered(&t, 0));
		udsp_slic_fake_hook_set(t.s->slic, 0, true);
		eh_sleep_ms(100);
		eh_ev_poll(&t);
		/* The sampler is suspended, so the level the plug-in drives
		 * reaches no line machine and the published pair is invalid. */
		CHECK_UINT(eh_ev_count(UDSP_EV_HOOK), before);
		CHECK(udsp_line_hook_query(t.s, 0, &level) == UDSP_TRANSIENT);

		CHECK(eh_job_wait(&t, job.token, 5000, NULL));
		tc_mlt_delay(&t, 0);
		level = false;
		CHECK(udsp_line_hook_query(t.s, 0, &level) == UDSP_OK);
		/* One fresh sample follows the test, so the published level is
		 * the level the line holds now. */
		CHECK(level);
		eh_wait_ms(&t, 400);
		CHECK(eh_ev_count(UDSP_EV_HOOK) > before);
		tc_down(&t);
	}

	CASE("another line's hook change waits for the test to finish");
	{
		struct udsp_job job = { 0 };
		uint64_t at, seen = 0;
		unsigned from;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		CHECK(tc_hook_boot_wait(&t));
		from = eh_ev_len;

		tc_mlt_delay(&t, 200);
		CHECK(udsp_line_test(t.s, 0, UDSP_TEST_GR909, &job) == UDSP_OK);
		/* The op is running from here, so the whole latency below
		 * belongs to the stall and not to the submission. */
		CHECK(tc_test_entered(&t, 0));
		udsp_slic_fake_hook_set(t.s->slic, 1, true);
		at = eh_mono_us();
		for (unsigned i = 0; i < 4000 && !seen; i++) {
			eh_ev_poll(&t);
			if (tc_hook_ev_find(1, from))
				seen = eh_mono_us();
			else
				eh_sleep_ms(1);
		}
		CHECK(seen != 0);
		/* The thread runs one job step before it samples, so the rest
		 * of the test time is added to every other line's report. */
		CHECK(seen - at > 100000u);
		if (getenv("TC_DEBUG"))
			fprintf(stderr, "other line stall: %llu us\n",
				(unsigned long long)(seen - at));
		CHECK(eh_job_wait(&t, job.token, 5000, NULL));
		tc_mlt_delay(&t, 0);
		tc_down(&t);
	}
}

static void case_sync(void)
{
	struct udsp_counters before, after;
	struct eh t;

	CASE("a slic error surfaces as the real status");
	{
		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		udsp_counters(t.s, &before);
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_LINEFEED_SET,
					 -EIO);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_OHT) == UDSP_TRANSIENT);
		udsp_counters(t.s, &after);
		CHECK_UINT(after.op_errors - before.op_errors, 1);

		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_LINEFEED_SET,
					 -EINVAL);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_DOWN) == UDSP_UNSUPPORTED);
		tc_down(&t);
	}

	CASE("a state equal to the current one takes no slic call");
	{
		unsigned before_n;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 50);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) == UDSP_OK);
		eh_wait_ms(&t, 30);
		CHECK(tc_stop(&t));
		tc_journal(&t);
		before_n = tc_jr_count(UDSP_SLIC_FAKE_LINEFEED_SET, 0);
		/* Start wrote FWD_ACTIVE and stop wrote OPEN; the redundant
		 * ACTIVE in between reached no register. */
		CHECK_UINT(before_n, 2);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a sync slic op is transient while a job runs");
	{
		struct udsp_job job = { 0 };
		char path[512];

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		snprintf(path, sizeof(path), "%s/AUS.conf", TC_REGION_DIR_STR);
		CHECK(udsp_region_load(t.s, path, &job) == UDSP_OK);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_OHT) == UDSP_TRANSIENT);
		CHECK(eh_job_wait(&t, job.token, 3000, NULL));
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_OHT) == UDSP_OK);
		tc_down(&t);
	}
}

static void case_ack(void)
{
	struct cmd_ack ack;
	int32_t status = 7;

	CASE("the ack slot discards a completion for a dead waiter");
	{
		CHECK(cmd_ack_init(&ack) == 0);
		cmd_ack_arm(&ack, 11u);
		/* Only the token the waiter published can complete it. */
		CHECK(!cmd_ack_post(&ack, 12u, -5));
		CHECK(cmd_ack_wait(&ack, 11u, 5, &status) == -ETIMEDOUT);
		CHECK(cmd_ack_stale(&ack, 11u));
		CHECK(!cmd_ack_post(&ack, 11u, -5));

		cmd_ack_arm(&ack, 12u);
		/* The completion of the command that timed out arrives late and
		 * must not reach the waiter that took the slot. */
		CHECK(!cmd_ack_post(&ack, 11u, -6));
		CHECK(cmd_ack_post(&ack, 12u, -EIO));
		status = 0;
		CHECK(cmd_ack_wait(&ack, 12u, 100, &status) == 0);
		CHECK(status == -EIO);
		CHECK(cmd_status_map(status) == UDSP_TRANSIENT);
		CHECK(cmd_status_map(-EINVAL) == UDSP_UNSUPPORTED);
		/* The tick's refusal of a session on a dead line reads as the
		 * guard's answer for that line, never as a retry. */
		CHECK(cmd_status_map(-ENODEV) == UDSP_FATAL);
		CHECK(cmd_status_map(0) == UDSP_OK);
		cmd_ack_release(&ack);
	}

	CASE("the ack finish tail takes a completion instead of timing out");
	{
		int32_t st = 0;

		CHECK(cmd_ack_init(&ack) == 0);
		/* A completer claimed and published the slot as the deadline
		 * passed, so the finish tail returns that completion. */
		cmd_ack_arm(&ack, 21u);
		CHECK(cmd_ack_post(&ack, 21u, -EIO));
		CHECK(cmd_ack_finish(&ack, 21u, &st) == 0);
		CHECK(st == -EIO);

		/* With no completer the finish tail cancels the slot. */
		cmd_ack_arm(&ack, 22u);
		CHECK(cmd_ack_finish(&ack, 22u, &st) == -ETIMEDOUT);
		CHECK(cmd_ack_stale(&ack, 22u));
		cmd_ack_release(&ack);
	}
}

/* --- regression coverage ------------------------------------------------- */

static uint8_t tc_lf_confirmed(struct eh *t, unsigned line)
{
	return (uint8_t)atomic_load_explicit(&t->s->chan[line].lf_confirmed,
					     memory_order_acquire);
}

static uint32_t tc_gain_confirmed(struct eh *t, unsigned line)
{
	return (uint32_t)atomic_load_explicit(&t->s->chan[line].gain_confirmed,
					      memory_order_acquire);
}

/* A machine gain record acks nothing, so a case bounds its wait on the
 * confirmed atomic itself; the deadline only keeps a broken run finite. */
static bool tc_gain_wait(struct eh *t, unsigned line, uint32_t want, unsigned ms)
{
	uint64_t end = eh_mono_us() + (uint64_t)ms * 1000u;

	while (eh_mono_us() < end) {
		if (tc_gain_confirmed(t, line) == want)
			return true;
		eh_ev_poll(t);
		eh_sleep_ms(1);
	}

	return tc_gain_confirmed(t, line) == want;
}

/* A machine write that failed advances no state, so op_errors is the
 * observable that orders the case behind the execution. */
static bool tc_op_errors_wait(struct eh *t, uint64_t at_least, unsigned ms)
{
	uint64_t end = eh_mono_us() + (uint64_t)ms * 1000u;
	struct udsp_counters ctr;

	for (;;) {
		udsp_counters(t->s, &ctr);
		if (ctr.op_errors >= at_least)
			return true;
		if (eh_mono_us() >= end)
			return false;
		eh_ev_poll(t);
		eh_sleep_ms(1);
	}
}

/* tc_jr_find treats a negative arg0 as a wildcard, so the signed gain rows
 * need their own matcher. */
static unsigned tc_jr_gain_count(unsigned chan, int32_t tx, int32_t rx)
{
	unsigned n = 0;

	for (unsigned i = 0; i < tc_jr_len; i++) {
		if (tc_jr[i].call == UDSP_SLIC_FAKE_GAIN_SET &&
		    tc_jr[i].chan == chan && tc_jr[i].arg0 == tx &&
		    tc_jr[i].arg1 == rx)
			n++;
	}

	return n;
}

static void case_confirmed_state(void)
{
	struct udsp_slic_fake_chan_state st;
	struct eh t;

	CASE("a failed linefeed write is retried to the same target");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);

		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_LINEFEED_SET,
					 -EIO);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_OHT) == UDSP_TRANSIENT);
		/* The hardware never took OHT, so the confirmed state must not
		 * have moved. */
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		/* The same target again must reach the register. */
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_OHT) == UDSP_OK);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_OHT);
		tc_down(&t);
	}

	CASE("a failed polarity write is retried to the same target");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_LINEFEED_SET,
					 -EIO);
		CHECK(udsp_line_polarity(t.s, 0, UDSP_POL_REVERSE) ==
		      UDSP_TRANSIENT);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK(udsp_line_polarity(t.s, 0, UDSP_POL_REVERSE) == UDSP_OK);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_REV_ACTIVE);
		CHECK(udsp_line_polarity(t.s, 0, UDSP_POL_FORWARD) == UDSP_OK);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		tc_down(&t);
	}

	CASE("sleep drives powersave and a failed write is retried");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_POWERSAVE,
					 -EIO);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_SLEEP) == UDSP_TRANSIENT);
		CHECK(!atomic_load(&t.s->chan[0].ps_confirmed));
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_SLEEP) == UDSP_OK);
		CHECK(atomic_load(&t.s->chan[0].ps_confirmed));
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) == UDSP_OK);
		CHECK(!atomic_load(&t.s->chan[0].ps_confirmed));
		CHECK(tc_stop(&t));
		CHECK(tc_state(&t, 0, &st));
		CHECK_UINT(st.powersave_count, 2);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a machine record failure never lands on the next command");
	{
		struct udsp_counters before, after;
		struct udsp_ring ring;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		udsp_counters(t.s, &before);
		tc_ring_fill(&ring, 200, 300, 2000);
		CHECK(udsp_ring_start(t.s, 0, &ring) == UDSP_OK);
		/* The relay off at the end of burst one is a machine record: it
		 * carries no token. */
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_RING_STOP,
					 -EIO);
		eh_wait_ms(&t, 400);
		udsp_counters(t.s, &after);
		CHECK(after.op_errors > before.op_errors);
		/* The next acknowledged command must not inherit that errno. */
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_OHT) == UDSP_OK);
		CHECK(udsp_ring_stop(t.s, 0) == UDSP_OK);
		tc_down(&t);
	}

	CASE("a failed on hook transmission aborts the armed sequence");
	{
		struct udsp_ring ring;
		struct udsp_cid cid;

		CHECK(tc_region_fast());
		if (!tc_up_ports(&t, "FST", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		tc_cid_fill(&cid, "5551234", "TEST CALLER");
		tc_ring_fill(&ring, 200, 400, 6000);
		ring.cid_present = true;
		ring.cid_at_ring = 0;
		ring.cid_wait_ms = 3000;
		ring.cid = cid;
		/* The sequencer's own OHT record carries no ack, so only the
		 * asynchronous completion can carry its failure. */
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_LINEFEED_SET,
					 -EIO);
		CHECK(udsp_ring_start(t.s, 0, &ring) == UDSP_OK);
		eh_wait_ms(&t, 400);
		{
			const struct udsp_event *sent =
				eh_ev_find(UDSP_EV_CID_SENT, 0);

			CHECK(sent != NULL);
			if (sent)
				CHECK(sent->u.cid_sent.status < 0);
		}
		CHECK(udsp_ring_stop(t.s, 0) == UDSP_OK);
		tc_down(&t);
		tc_region_fast_drop();
	}

	CASE("a reinit failure opens a line that was live");
	{
		struct udsp_job job = { 0 };
		int32_t status = 0;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);

		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_INIT, -EIO);
		CHECK(udsp_start(t.s, &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 5000, &status));
		CHECK(status == UDSP_FATAL);
		/* An energized line must not be abandoned energized when its
		 * reinit fails. */
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) == UDSP_FATAL);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a persistent fault still gets its five second trial");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 40);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_THERMAL);
		/* The fault is never cleared here: a report that keeps arriving
		 * must not postpone the trial for ever. */
		eh_wait_ms(&t, 13000);
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		CHECK(tc_stop(&t));
		CHECK(tc_state(&t, 0, &st));
		/* start, fault open, trial, fault open, trial, ... stop. */
		if (getenv("TC_DEBUG"))
			fprintf(stderr, "persistent: lf=%u faults=%u reads=%u\n",
				st.linefeed_count, st.fault_read_count,
				st.hook_read_count);
		CHECK(st.linefeed_count >= 5);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a job completion is visible before the guard reopens");
	{
		char path[512];

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		snprintf(path, sizeof(path), "%s/USA.conf", TC_REGION_DIR_STR);
		for (unsigned i = 0; i < 8; i++) {
			struct udsp_job job = { 0 };

			CHECK(udsp_region_load(t.s, path, &job) == UDSP_OK);
			CHECK(eh_job_wait(&t, job.token, 3000, NULL));
			/* The completion arrived, so no job is in flight and the
			 * op must not be refused. */
			CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) ==
			      UDSP_OK);
		}
		tc_down(&t);
	}

	CASE("hook timing keeps its sync slic lifecycle");
	{
		struct udsp_config cfg;
		struct udsp_job job = { 0 };
		char path[512];

		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_tdm_override(&tc_ops);
		tc_cfg(&cfg, "tc", "USA");
		cfg.populated_ports = 1;
		CHECK(tc_open_handle(&t, &cfg));
		if (!t.s) {
			udsp_tdm_override(NULL);
			return;
		}
		/* Before start it is transient, exactly as every other
		 * SYNC-SLIC op is. */
		CHECK(udsp_line_hook_timing(t.s, 0, 40, 200, 1301) ==
		      UDSP_TRANSIENT);
		CHECK(tc_start(&t));
		CHECK(udsp_line_hook_timing(t.s, 0, 40, 200, 1301) == UDSP_OK);
		snprintf(path, sizeof(path), "%s/AUS.conf", TC_REGION_DIR_STR);
		CHECK(udsp_region_load(t.s, path, &job) == UDSP_OK);
		CHECK(udsp_line_hook_timing(t.s, 0, 40, 200, 1301) ==
		      UDSP_TRANSIENT);
		CHECK(eh_job_wait(&t, job.token, 3000, NULL));
		tc_down(&t);
	}

	CASE("a cancelled command is not revived by the next one");
	{
		struct udsp_cmd a, b, out = { 0 };
		struct cmd_ring ring;
		struct cmd_ack ack;

		cmd_ring_init(&ring);
		CHECK(cmd_ack_init(&ack) == 0);
		memset(&a, 0, sizeof(a));
		memset(&b, 0, sizeof(b));
		a.type = UDSP_CMD_LINE_STATE;
		a.token = 1u;
		a.ack = &ack;
		b = a;
		b.token = 2u;

		cmd_ack_arm(&ack, 1u);
		CHECK(cmd_ring_push(&ring, &a));
		CHECK(cmd_ack_wait(&ack, 1u, 5, NULL) == -ETIMEDOUT);
		/* The slot is taken again while A is still queued. */
		cmd_ack_arm(&ack, 2u);
		CHECK(cmd_ring_pop(&ring, &out));
		CHECK_UINT(out.token, 1);
		/* The executor compares the record's own generation, so A stays
		 * skipped and B stays runnable (cmd.h Cancellation per command
		 * generation). */
		CHECK(cmd_ack_stale(&ack, out.token));
		CHECK(!cmd_ack_stale(&ack, 2u));
		CHECK(cmd_ring_push(&ring, &b));
		CHECK(cmd_ring_pop(&ring, &out));
		CHECK(!cmd_ack_stale(&ack, out.token));
		cmd_ack_release(&ack);
	}

	CASE("a failed open never closes descriptor zero");
	{
		struct udsp_config cfg;
		int pair[2];
		int keep;

		CHECK(pipe(pair) == 0);
		keep = dup(0);
		CHECK(dup2(pair[1], 0) == 0);

		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_tdm_override(&tc_ops);
		tc_cfg(&cfg, "tc", "ZZZ");
		/* The region parse fails before the emitter exists, and the
		 * release path must not mistake a zeroed member for a live
		 * descriptor. */
		CHECK(!tc_open_handle(&t, &cfg));
		{
			char sink = 0;
			ssize_t n;

			/* The read end is non-blocking: if the library closed
			 * descriptor zero and an eventfd took the slot, the
			 * write above goes somewhere else and this read must
			 * fail rather than wait for a byte that never comes. */
			CHECK(fcntl(pair[0], F_SETFL, O_NONBLOCK) == 0);
			CHECK(write(0, "x", 1) == 1);
			n = read(pair[0], &sink, 1);
			CHECK(n == 1);
			CHECK(sink == 'x');
		}
		CHECK(dup2(keep, 0) == 0);
		close(keep);
		close(pair[0]);
		close(pair[1]);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("the host exposes its spidev descriptor to the crash handler");
	{
		struct slic_host_cfg hc;
		struct slic_host *h = NULL;
		int fd[2];

		CHECK_UINT((uint64_t)(slic_host_fd(NULL) == -1), 1);
		CHECK(pipe(fd) == 0);
		memset(&hc, 0, sizeof(hc));
		hc.spidev = "/dev/null";
		CHECK(slic_host_open_fd(&hc, fd[1], &h) == 0);
		CHECK(h != NULL);
		if (h) {
			/* The handler cannot call the host, so the descriptor
			 * has to be readable before a signal arrives. */
			CHECK(slic_host_fd(h) == fd[1]);
			slic_host_close(h);
		}
		close(fd[0]);
		close(fd[1]);
	}
}

static void case_slic_gain(void)
{
	struct udsp_slic_fake_chan_state st;
	struct udsp_chan_cfg ccfg;
	struct eh t;

	CASE("bring-up programs the SLIC gain explicitly");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		CHECK_UINT(tc_gain_confirmed(&t, 0),
			   udsp_gain_confirmed_pack(0, 0));
		CHECK(tc_stop(&t));
		tc_journal(&t);
		CHECK_UINT(tc_jr_count(UDSP_SLIC_FAKE_GAIN_SET, 0), 1);
		CHECK_UINT(tc_jr_gain_count(0, 0, 0), 1);
		CHECK(tc_state(&t, 0, &st));
		CHECK_UINT(st.gain_count, 1);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a changed SLIC gain reaches the plug-in once");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.slic_gain_tx = -6;
		ccfg.slic_gain_rx = -18;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		/* The receive loss stays in the engine, so the SLIC takes its
		 * 0 dB receive preset. */
		CHECK(tc_gain_wait(&t, 0, udsp_gain_confirmed_pack(-6, 0),
				   2000));
		/* The same pair again is suppressed against the confirmed
		 * record and reaches no register. */
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(tc_stop(&t));
		tc_journal(&t);
		CHECK_UINT(tc_jr_gain_count(0, -6, 0), 1);
		CHECK(tc_state(&t, 0, &st));
		CHECK(st.gain_tx_half_db == -6);
		CHECK(st.gain_rx_half_db == 0);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("+6 reaches the plug-in as +6");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.slic_gain_tx = -6;
		ccfg.slic_gain_rx = 6;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(tc_gain_wait(&t, 0, udsp_gain_confirmed_pack(-6, 6),
				   2000));
		CHECK(tc_stop(&t));
		tc_journal(&t);
		CHECK_UINT(tc_jr_gain_count(0, -6, 6), 1);
		CHECK(tc_state(&t, 0, &st));
		CHECK(st.gain_rx_half_db == 6);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a change of a negative receive value writes no record");
	{
		struct udsp_chan *c;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.slic_gain_tx = -6;
		ccfg.slic_gain_rx = -18;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(tc_gain_wait(&t, 0, udsp_gain_confirmed_pack(-6, 0),
				   2000));
		ccfg.slic_gain_rx = -30;
		/* The ack of the SYNC-TICK op orders the reads of the fields
		 * that the tick wrote when it applied the configuration. */
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		c = &t.s->chan[0];
		CHECK(c->cfg.slic_gain_rx == -30);
		CHECK(c->dl_voice_gain == -30);
		CHECK(c->dl_local_gain == 0);
		CHECK(c->dl_ref_loss == 30);
		eh_wait_ms(&t, 40);
		CHECK_UINT(tc_gain_confirmed(&t, 0),
			   udsp_gain_confirmed_pack(-6, 0));
		CHECK(tc_stop(&t));
		tc_journal(&t);
		/* One row from the bring-up and one from the first config. */
		CHECK_UINT(tc_jr_count(UDSP_SLIC_FAKE_GAIN_SET, 0), 2);
		CHECK_UINT(tc_jr_gain_count(0, -6, 0), 1);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("the trim follows a sign change and a failed write");
	{
		struct udsp_counters before, after;
		struct udsp_chan *c;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.slic_gain_tx = -6;
		ccfg.slic_gain_rx = -18;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(tc_gain_wait(&t, 0, udsp_gain_confirmed_pack(-6, 0),
				   2000));
		c = &t.s->chan[0];
		ccfg.slic_gain_rx = 6;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(c->dl_voice_gain == 0);
		CHECK(c->dl_local_gain == -6);
		CHECK(c->dl_ref_loss == 0);
		CHECK(tc_gain_wait(&t, 0, udsp_gain_confirmed_pack(-6, 6),
				   2000));

		/* The fields follow the configuration at once, and the part
		 * keeps the pair it confirmed until a write heals it. */
		udsp_counters(t.s, &before);
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_GAIN_SET,
					 -EIO);
		ccfg.slic_gain_rx = 8;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(c->dl_voice_gain == 0);
		CHECK(c->dl_local_gain == -8);
		CHECK(c->dl_ref_loss == 0);
		CHECK(tc_op_errors_wait(&t, before.op_errors + 1u, 2000));
		CHECK_UINT(tc_gain_confirmed(&t, 0),
			   udsp_gain_confirmed_pack(-6, 6));
		udsp_counters(t.s, &after);
		CHECK_UINT(after.op_errors - before.op_errors, 1);
		CHECK(tc_stop(&t));
		tc_journal(&t);
		CHECK_UINT(tc_jr_gain_count(0, -6, 6), 1);
		CHECK_UINT(tc_jr_gain_count(0, -6, 8), 0);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a failed SLIC gain write heals at the next config");
	{
		struct udsp_counters before, after;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		udsp_counters(t.s, &before);
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_GAIN_SET,
					 -EIO);
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.slic_gain_tx = -6;
		ccfg.slic_gain_rx = -18;
		/* The gain write is a machine record, so the failure never
		 * reaches the caller; it is counted instead. */
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(tc_op_errors_wait(&t, before.op_errors + 1u, 2000));
		/* The hardware never took the pair (an armed fake error
		 * changes nothing), so the confirmed record must still hold
		 * the bring-up pair. The fake's own pair is read only after
		 * the stop, under the quiescence rule. */
		CHECK_UINT(tc_gain_confirmed(&t, 0),
			   udsp_gain_confirmed_pack(0, 0));
		udsp_counters(t.s, &after);
		CHECK_UINT(after.op_errors - before.op_errors, 1);
		/* The same config again re-emits, because the suppression
		 * compares the confirmed pair, and heals the state. */
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(tc_gain_wait(&t, 0, udsp_gain_confirmed_pack(-6, 0),
				   2000));
		CHECK(tc_stop(&t));
		tc_journal(&t);
		CHECK_UINT(tc_jr_gain_count(0, -6, 0), 1);
		CHECK(tc_state(&t, 0, &st));
		CHECK(st.gain_tx_half_db == -6);
		CHECK(st.gain_rx_half_db == 0);
		CHECK_UINT(st.gain_count, 2);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a restart programs the SLIC gain again");
	{
		struct udsp_job job = { 0 };
		int32_t status = -1;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.slic_gain_tx = -6;
		ccfg.slic_gain_rx = -18;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(tc_stop(&t));

		CHECK(udsp_start(t.s, &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 5000, &status));
		CHECK_UINT((uint32_t)status, 0);
		eh_wait_ms(&t, 60);
		CHECK_UINT(tc_gain_confirmed(&t, 0),
			   udsp_gain_confirmed_pack(-6, 0));
		CHECK(tc_stop(&t));
		tc_journal(&t);
		/* One row from the config, one more row from the restart's
		 * LINES_UP: init reset the gain RAM, so suppression must not
		 * swallow the second write. */
		CHECK_UINT(tc_jr_gain_count(0, -6, 0), 2);
		{
			int init2 = tc_jr_find(UDSP_SLIC_FAKE_INIT, 0, -1, 0);
			int gain2 = -1;

			if (init2 >= 0)
				init2 = tc_jr_find(UDSP_SLIC_FAKE_INIT, 0, -1,
						   (unsigned)init2 + 1u);
			for (unsigned i = 0; i < tc_jr_len; i++) {
				if (tc_jr[i].call == UDSP_SLIC_FAKE_GAIN_SET &&
				    tc_jr[i].chan == 0)
					gain2 = (int)i;
			}
			CHECK(init2 >= 0);
			CHECK(gain2 > init2);
		}
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a SLIC gain the plug-in cannot reach is refused");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		/* PROSLIC_GAIN_MAX +6 dB and PROSLIC_GAIN_MIN -30 dB bound
		 * the pair to [-60, +12] half dB; one step past either end is
		 * refused before posting so -ERANGE never reaches the tick. */
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.slic_gain_tx = 13;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_UNSUPPORTED);
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.slic_gain_rx = -61;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_UNSUPPORTED);
		/* The bounds themselves are reachable. */
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.slic_gain_tx = 12;
		ccfg.slic_gain_rx = -60;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(tc_stop(&t));
		tc_journal(&t);
		CHECK_UINT(tc_jr_gain_count(0, 13, 0), 0);
		CHECK_UINT(tc_jr_gain_count(0, 0, -61), 0);
		/* The receive bound is a digital loss, so the SLIC takes its
		 * 0 dB receive preset. */
		CHECK_UINT(tc_jr_gain_count(0, 12, 0), 1);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a failed bring-up gain write does not fail the start");
	{
		struct udsp_counters before, after;
		struct udsp_config cfg;
		bool offhook = true;

		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_tdm_override(&tc_ops);
		tc_cfg(&cfg, "tc", "USA");
		cfg.populated_ports = 1;
		CHECK(tc_open_handle(&t, &cfg));
		if (!t.s) {
			udsp_tdm_override(NULL);
			return;
		}
		udsp_counters(t.s, &before);
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_GAIN_SET,
					 -EIO);
		/* The gain record is a machine record: an audio-level write
		 * is not service viability, so the start completes and the
		 * confirmed model heals the pair later. */
		CHECK(tc_start(&t));
		eh_wait_ms(&t, 40);
		CHECK(udsp_line_hook_query(t.s, 0, &offhook) == UDSP_OK);
		CHECK_UINT(tc_gain_confirmed(&t, 0), 0);
		udsp_counters(t.s, &after);
		CHECK_UINT(after.op_errors - before.op_errors, 1);

		memset(&ccfg, 0, sizeof(ccfg));
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(tc_gain_wait(&t, 0, udsp_gain_confirmed_pack(0, 0),
				   2000));
		CHECK(tc_stop(&t));
		tc_journal(&t);
		CHECK_UINT(tc_jr_gain_count(0, 0, 0), 1);
		CHECK(tc_state(&t, 0, &st));
		CHECK_UINT(st.gain_count, 1);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a channel config before start arms without touching the part");
	{
		struct udsp_config cfg;

		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_tdm_override(&tc_ops);
		tc_cfg(&cfg, "tc", "USA");
		cfg.populated_ports = 1;
		CHECK(tc_open_handle(&t, &cfg));
		if (!t.s) {
			udsp_tdm_override(NULL);
			return;
		}
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.slic_gain_tx = -6;
		ccfg.slic_gain_rx = -18;
		/* Before LINES_UP the pair is staged only; the ack is the
		 * tick arm. */
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(tc_start(&t));
		eh_wait_ms(&t, 40);
		CHECK_UINT(tc_gain_confirmed(&t, 0),
			   udsp_gain_confirmed_pack(-6, 0));
		CHECK(tc_stop(&t));
		tc_journal(&t);
		{
			int init0 = tc_jr_find(UDSP_SLIC_FAKE_INIT, 0, -1, 0);
			int gain0 = -1;

			for (unsigned i = 0; i < tc_jr_len; i++) {
				if (tc_jr[i].call == UDSP_SLIC_FAKE_GAIN_SET) {
					gain0 = (int)i;
					break;
				}
			}
			CHECK(init0 >= 0);
			/* No GAIN_SET row precedes the INIT rows: the part
			 * was never touched before its init. */
			CHECK(gain0 > init0);
		}
		/* The LINES_UP row carries the staged pair. */
		CHECK_UINT(tc_jr_gain_count(0, -6, 0), 1);
		CHECK_UINT(tc_jr_gain_count(0, 0, 0), 0);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}
}

static void case_slic_region(void)
{
	struct udsp_slic_fake_chan_state st;
	struct udsp_job job = { 0 };
	int32_t status = -1;
	struct eh t;

	CASE("a region load programs the SLIC region");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		CHECK(udsp_region_load(t.s, "GBR", &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 3000, &status));
		CHECK_UINT((uint32_t)status, 0);
		CHECK(tc_stop(&t));
		tc_journal(&t);
		/* tc_up_ports loads USA at open (tc_cfg writes cfg.region), so
		 * a region is active at start and the start path already
		 * programmed USA once after init: two rows total, the second
		 * from the load. */
		CHECK_UINT(tc_jr_count(UDSP_SLIC_FAKE_REGION_SET, 0), 2);
		{
			int init0 = tc_jr_find(UDSP_SLIC_FAKE_INIT, 0, -1, 0);
			int reg0 = tc_jr_find(UDSP_SLIC_FAKE_REGION_SET, 0, -1,
					      0);
			int reg1 = -1;

			if (reg0 >= 0)
				reg1 = tc_jr_find(UDSP_SLIC_FAKE_REGION_SET, 0,
						  -1, (unsigned)reg0 + 1u);
			CHECK(init0 >= 0);
			CHECK(reg0 > init0);
			CHECK(reg1 > reg0);
		}
		CHECK(tc_state(&t, 0, &st));
		CHECK_UINT(st.region_count, 2);
		CHECK(strcmp(st.region_iso3, "GBR") == 0);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a restart re-programs the SLIC region");
	{
		struct udsp_job job2 = { 0 };

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		CHECK(udsp_region_load(t.s, "GBR", &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 3000, &status));
		CHECK_UINT((uint32_t)status, 0);
		CHECK(tc_stop(&t));

		CHECK(udsp_start(t.s, &job2) == UDSP_OK);
		CHECK(eh_job_wait(&t, job2.token, 5000, &status));
		CHECK_UINT((uint32_t)status, 0);
		eh_wait_ms(&t, 40);
		CHECK(tc_stop(&t));
		tc_journal(&t);
		/* USA at the first start, GBR from the load, GBR again from
		 * the restart: the region slots survive a stop and the new
		 * init needs the country re-programmed. */
		CHECK_UINT(tc_jr_count(UDSP_SLIC_FAKE_REGION_SET, 0), 3);
		{
			int init2 = tc_jr_find(UDSP_SLIC_FAKE_INIT, 0, -1, 0);
			int reg_last = -1;

			if (init2 >= 0)
				init2 = tc_jr_find(UDSP_SLIC_FAKE_INIT, 0, -1,
						   (unsigned)init2 + 1u);
			for (unsigned i = 0; i < tc_jr_len; i++) {
				if (tc_jr[i].call == UDSP_SLIC_FAKE_REGION_SET &&
				    tc_jr[i].chan == 0)
					reg_last = (int)i;
			}
			CHECK(init2 >= 0);
			CHECK(reg_last > init2);
		}
		CHECK(tc_state(&t, 0, &st));
		CHECK_UINT(st.region_count, 3);
		CHECK(strcmp(st.region_iso3, "GBR") == 0);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a missing region op is tolerated");
	{
		struct udsp_config cfg;

		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_slic_fake_optional_set(UDSP_SLIC_FAKE_OPT_ALL &
					    ~(uint32_t)UDSP_SLIC_FAKE_OPT_REGION);
		udsp_tdm_override(&tc_ops);
		tc_cfg(&cfg, "tc", "USA");
		cfg.populated_ports = 1;
		CHECK(tc_open_handle(&t, &cfg));
		if (!t.s) {
			udsp_tdm_override(NULL);
			return;
		}
		CHECK(tc_start(&t));
		eh_wait_ms(&t, 40);
		CHECK(udsp_region_load(t.s, "GBR", &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 3000, &status));
		/* region_set is nullable (slic.h): both the start path and
		 * the region job succeed without it. */
		CHECK_UINT((uint32_t)status, 0);
		CHECK(tc_stop(&t));
		tc_journal(&t);
		CHECK_UINT(tc_jr_count(UDSP_SLIC_FAKE_REGION_SET, 0), 0);
		CHECK(tc_state(&t, 0, &st));
		CHECK_UINT(st.region_count, 0);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a failing region op is counted and the job succeeds");
	{
		struct udsp_counters before, after;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		udsp_counters(t.s, &before);
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_REGION_SET,
					 -EIO);
		CHECK(udsp_region_load(t.s, "GBR", &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 3000, &status));
		/* The tone-table swap succeeded and is not rolled back, so the
		 * SLIC write failure must not ride the job status (uvoice
		 * reads a non-OK status as "no tone profile"); it is counted
		 * and reported on the machine-record path. */
		CHECK_UINT((uint32_t)status, 0);
		udsp_counters(t.s, &after);
		CHECK_UINT(after.op_errors - before.op_errors, 1);
		/* The line stays up. */
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_OHT) == UDSP_OK);
		CHECK(tc_stop(&t));
		tc_journal(&t);
		CHECK(tc_state(&t, 0, &st));
		/* The armed error is spent before the fake records anything,
		 * so only the start-path USA row was recorded. */
		CHECK_UINT(st.region_count, 1);
		CHECK(strcmp(st.region_iso3, "USA") == 0);
		CHECK_UINT(tc_jr_count(UDSP_SLIC_FAKE_REGION_SET, 0), 1);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a failing region op at start does not fail the start");
	{
		struct udsp_counters before, after;
		struct udsp_config cfg;
		bool offhook = true;

		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_tdm_override(&tc_ops);
		tc_cfg(&cfg, "tc", "USA");
		cfg.populated_ports = 1;
		CHECK(tc_open_handle(&t, &cfg));
		if (!t.s) {
			udsp_tdm_override(NULL);
			return;
		}
		udsp_counters(t.s, &before);
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_REGION_SET,
					 -EIO);
		/* The init succeeded and the part serves calls on the profile
		 * defaults, so the start completes and the error is counted,
		 * exactly as it is for a failed bring-up gain write. */
		CHECK(tc_start(&t));
		eh_wait_ms(&t, 40);
		CHECK(udsp_line_hook_query(t.s, 0, &offhook) == UDSP_OK);
		udsp_counters(t.s, &after);
		CHECK_UINT(after.op_errors - before.op_errors, 1);
		/* The following load programs the line. */
		CHECK(udsp_region_load(t.s, "GBR", &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 3000, &status));
		CHECK_UINT((uint32_t)status, 0);
		CHECK(tc_stop(&t));
		tc_journal(&t);
		/* The start-path call failed before the fake recorded it, so
		 * the load's GBR row is the only one. */
		CHECK_UINT(tc_jr_count(UDSP_SLIC_FAKE_REGION_SET, 0), 1);
		CHECK(tc_state(&t, 0, &st));
		CHECK_UINT(st.region_count, 1);
		CHECK(strcmp(st.region_iso3, "GBR") == 0);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a region failure on one line still programs the others");
	{
		struct udsp_counters before, after;
		struct udsp_slic_fake_chan_state st1;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		udsp_counters(t.s, &before);
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_REGION_SET,
					 -EIO);
		CHECK(udsp_region_load(t.s, "GBR", &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 3000, &status));
		CHECK_UINT((uint32_t)status, 0);
		udsp_counters(t.s, &after);
		CHECK_UINT(after.op_errors - before.op_errors, 1);
		CHECK(tc_stop(&t));
		tc_journal(&t);
		/* The one-shot error hits line 0, the first the job visits,
		 * so line 0 keeps its start-path USA row alone and line 1
		 * gains the GBR row: the later lines still run. */
		CHECK_UINT(tc_jr_count(UDSP_SLIC_FAKE_REGION_SET, 0), 1);
		CHECK_UINT(tc_jr_count(UDSP_SLIC_FAKE_REGION_SET, 1), 2);
		CHECK(tc_state(&t, 0, &st));
		CHECK_UINT(st.region_count, 1);
		CHECK(strcmp(st.region_iso3, "USA") == 0);
		CHECK(tc_state(&t, 1, &st1));
		CHECK_UINT(st1.region_count, 2);
		CHECK(strcmp(st1.region_iso3, "GBR") == 0);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a region load invalidates the confirmed SLIC gain");
	{
		struct udsp_chan_cfg ccfg;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.slic_gain_tx = -6;
		ccfg.slic_gain_rx = -18;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		/* The gain write is a machine record and acks nothing, so the
		 * wait is bound on the confirmed atomic. */
		CHECK(tc_gain_wait(&t, 0, udsp_gain_confirmed_pack(-6, 0),
				   2000));
		CHECK(udsp_region_load(t.s, "GBR", &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 3000, &status));
		CHECK_UINT((uint32_t)status, 0);
		/* region_set re-ran the presets, so the pair the SLIC holds
		 * is unknown and the record must be invalid. */
		CHECK_UINT(tc_gain_confirmed(&t, 0), 0);
		/* The pair the record already confirmed must reach the
		 * register again instead of being suppressed. */
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(tc_gain_wait(&t, 0, udsp_gain_confirmed_pack(-6, 0),
				   2000));
		CHECK(tc_stop(&t));
		tc_journal(&t);
		CHECK_UINT(tc_jr_gain_count(0, -6, 0), 2);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}
}

static void case_emitter(void)
{
	struct udsp_counters ctr;
	struct udsp_ring ring;
	struct udsp_job job = { 0 };
	struct eh t;

	CASE("a full event socket drops hardware events and keeps completions");
	{
		int small = 2048;
		unsigned seq_gaps = 0;
		unsigned prev = 0;
		unsigned jobs = 0;
		int32_t status = -1;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		setsockopt(t.tx_fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
		setsockopt(t.rx_fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
		eh_wait_ms(&t, 50);
		eh_ev_reset();

		/* A restart over a live session is two events, so the burst
		 * overruns the socket long before the ring behind the emitter
		 * could fill. */
		tc_ring_fill(&ring, 100, 100, 60000);
		for (unsigned i = 0; i < 120; i++) {
			if (udsp_ring_start(t.s, 0, &ring) != UDSP_OK)
				break;
		}
		CHECK(udsp_ring_stop(t.s, 0) == UDSP_OK);
		CHECK(udsp_region_load(t.s, "/nonexistent/YYY.conf", &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 5000, &status));

		/* With the socket draining again, the events that follow carry
		 * the seq the dropped ones consumed, so the gap is visible. */
		for (unsigned i = 0; i < 6; i++) {
			CHECK(udsp_ring_start(t.s, 0, &ring) == UDSP_OK);
			eh_ev_poll(&t);
			eh_sleep_ms(20);
			eh_ev_poll(&t);
		}
		CHECK(udsp_ring_stop(t.s, 0) == UDSP_OK);
		eh_sleep_ms(50);
		eh_ev_poll(&t);

		for (unsigned i = 0; i < eh_ev_len; i++) {
			if (eh_ev[i].type == UDSP_EV_JOB_DONE) {
				if (eh_ev[i].u.job.token == job.token)
					jobs++;
				continue;
			}
			if (prev && eh_ev[i].seq > prev + 1)
				seq_gaps += eh_ev[i].seq - prev - 1u;
			prev = eh_ev[i].seq;
		}
		udsp_counters(t.s, &ctr);
		CHECK(ctr.events_dropped > 0);
		CHECK_UINT(jobs, 1);
		CHECK(status == UDSP_UNSUPPORTED);
		CHECK_UINT(seq_gaps, ctr.events_dropped);
		tc_down(&t);
	}

	CASE("every completion survives a blocked event socket");
	{
		uint32_t tok[80];
		char path[512];
		int small = 2048;
		unsigned posted = 0;
		unsigned collected = 0;
		uint64_t end;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		setsockopt(t.tx_fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
		setsockopt(t.rx_fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
		eh_wait_ms(&t, 50);
		eh_ev_reset();

		/* Nothing drains the socket, so the emitter parks holding one
		 * completion and the ring behind it fills. A push that gave up
		 * would strand its caller for good. */
		snprintf(path, sizeof(path), "%s/USA.conf", TC_REGION_DIR_STR);
		end = eh_mono_us() + 3000000u;
		while (posted < 80 && eh_mono_us() < end) {
			struct udsp_job job = { 0 };

			if (udsp_region_load(t.s, path, &job) == UDSP_OK)
				tok[posted++] = job.token;
			else
				eh_sleep_ms(2);
		}
		/* Far more jobs than the socket, the 16 slot completion ring
		 * and the 8 slot job queue can hold together, so back pressure
		 * is the one thing that can bound the count. */
		CHECK(posted > 16);
		CHECK(posted < 80);

		end = eh_mono_us() + 20000000u;
		while (collected < posted && eh_mono_us() < end) {
			eh_ev_poll(&t);
			collected = 0;
			for (unsigned i = 0; i < posted; i++) {
				for (unsigned k = 0; k < eh_ev_len; k++) {
					if (eh_ev[k].type == UDSP_EV_JOB_DONE &&
					    eh_ev[k].u.job.token == tok[i]) {
						collected++;
						break;
					}
				}
			}
			eh_sleep_ms(5);
		}
		if (getenv("TC_DEBUG"))
			fprintf(stderr, "completions: posted=%u collected=%u\n",
				posted, collected);
		CHECK_UINT(collected, posted);
		tc_down(&t);
	}

	/*
	 * The ring's pending loss rides the next record that the producer
	 * queues, and a tail loss with no following record is burned after the
	 * queue empties. Reason: a burn at the start of a drain stamps the
	 * records queued before the loss after the gap. The position that the
	 * consumer sees then depends on when the emitter thread runs.
	 *
	 * The emitter thread stays stopped and the drain runs here, so the
	 * interleaving is fixed and the sequence numbers are exact.
	 */
	CASE("a producer loss is burned after the records it followed");
	{
		struct udsp_emitter em;
		struct udsp_event ev;
		struct udsp_event out;
		unsigned queued = 0;
		unsigned lost = 0;
		unsigned seen = 0;
		unsigned first = 0;
		unsigned last = 0;
		int big = 1 << 20;
		int fd[2];

		CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fd) == 0);
		setsockopt(fd[1], SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
		setsockopt(fd[0], SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
		CHECK(udsp_emitter_init(&em, fd[1], NULL, 100000, NULL, NULL) == 0);

		memset(&ev, 0, sizeof(ev));
		ev.type = UDSP_EV_HOOK;
		/* Fill the engine ring, then keep pushing: those three are the
		 * losses, and they happen after everything already queued. */
		while (udsp_emit(&em, UDSP_EV_PROD_ENGINE, &ev))
			queued++;
		lost = 1;
		for (unsigned i = 0; i < 2u; i++) {
			CHECK(!udsp_emit(&em, UDSP_EV_PROD_ENGINE, &ev));
			lost++;
		}
		CHECK(queued > 8u);
		CHECK_UINT(lost, 3);

		CHECK(udsp_emitter_pump(&em));
		while (recv(fd[0], &out, sizeof(out), MSG_DONTWAIT) ==
		       (ssize_t)sizeof(out)) {
			if (!seen)
				first = out.seq;
			last = out.seq;
			seen++;
		}
		CHECK_UINT(seen, queued);
		/* The queue is older than the loss, so it keeps the numbers
		 * from 1 and the gap opens behind it. */
		CHECK_UINT(first, 1);
		CHECK_UINT(last, queued);

		/* The next record proves where the gap went: three numbers were
		 * burned between the queue and it, and nowhere else. */
		CHECK(udsp_emit(&em, UDSP_EV_PROD_ENGINE, &ev));
		CHECK(udsp_emitter_pump(&em));
		CHECK(recv(fd[0], &out, sizeof(out), MSG_DONTWAIT) ==
		      (ssize_t)sizeof(out));
		CHECK_UINT(out.seq, queued + lost + 1u);
		CHECK_UINT(udsp_emitter_dropped(&em), lost);

		udsp_emitter_release(&em);
		close(fd[0]);
		close(fd[1]);
	}

	CASE("the emitter sequence wraps past 65535 to 1");
	{
		struct udsp_emitter em;
		struct udsp_event ev;
		int fd[2];
		unsigned prev = 0;
		unsigned wrapped = 0;
		unsigned seen = 0;
		bool zero = false;

		CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fd) == 0);
		CHECK(udsp_emitter_init(&em, fd[1], NULL, 100000, NULL, NULL) == 0);
		CHECK(udsp_emitter_start(&em) == 0);

		memset(&ev, 0, sizeof(ev));
		ev.type = UDSP_EV_HOOK;
		for (unsigned round = 0; round < 700 && !zero; round++) {
			for (unsigned i = 0; i < 100; i++) {
				while (!udsp_emit(&em, UDSP_EV_PROD_CTL, &ev))
					eh_sleep_ms(1);
			}
			for (unsigned i = 0; i < 100; i++) {
				struct udsp_event out;
				uint64_t end = eh_mono_us() + 2000000u;

				while (recv(fd[0], &out, sizeof(out), MSG_DONTWAIT) !=
				       (ssize_t)sizeof(out)) {
					if (eh_mono_us() > end)
						break;
					eh_sleep_ms(1);
				}
				seen++;
				if (!out.seq)
					zero = true;
				/* A loss-tolerant wrap check: a retried push
				 * costs two numbers, so an exact 65535 -> 1
				 * step is not guaranteed. */
			if (out.seq < prev)
					wrapped++;
				prev = out.seq;
			}
		}
		CHECK(seen >= 65536u);
		CHECK(!zero);
		CHECK_UINT(wrapped, 1);
		CHECK(udsp_emitter_stop(&em, 1000) == 0);
		udsp_emitter_release(&em);
		close(fd[0]);
		close(fd[1]);
	}
}

/* --- the deliberate-leak children ---------------------------------------- */

static int tc_wedge_pipe[2] = { -1, -1 };

static void tc_wedge_open(struct eh *t, const struct udsp_tdm_ops *ops,
			  struct tw_state *w, bool paced)
{
	struct udsp_config cfg;

	udsp_slic_fake_reset();
	w->paced = paced;
	w->block_fd = tc_wedge_pipe[0];
	atomic_store(&w->reads, 0u);
	atomic_store(&w->block_after, UINT64_MAX);
	udsp_tdm_override(ops);
	tc_cfg(&cfg, "tc", "USA");
	tc_child->opened = tc_open_handle(t, &cfg);
	if (!tc_child->opened)
		return;
	tc_child->started = tc_start(t);
	atomic_store(&w->block_after, atomic_load(&w->reads));
}

static void tc_child_bounded_stop(void)
{
	struct udsp_job job = { 0 };
	struct udsp_config cfg;
	uint64_t begin;
	struct eh t;

	tc_wedge_open(&t, &tw0_ops, &tw[0], true);
	if (!tc_child->started)
		return;

	eh_sleep_ms(50);
	begin = eh_mono_us();
	tc_child->stop_accepted = udsp_stop(t.s, &job) == UDSP_OK;
	tc_child->job_seen = eh_job_wait(&t, job.token, 4000,
					 &tc_child->job_status);
	tc_child->stop_us = eh_mono_us() - begin;
	tc_child->dead = atomic_load(&t.s->dead);

	udsp_close(t.s);
	/* The graph leaked, so the guard must still refuse a second handle. */
	tc_cfg(&cfg, "tc", "USA");
	{
		struct eh again;

		tc_child->reopen_refused = !tc_open_handle(&again, &cfg);
		tc_free(&again);
	}
}

static void tc_fault_poll(struct eh *t, uint64_t us, bool until_fault)
{
	uint64_t end = eh_mono_us() + us;

	while (eh_mono_us() < end) {
		eh_ev_poll(t);
		if (until_fault && eh_ev_find(UDSP_EV_SLIC_FAULT, 0) != NULL)
			return;
		eh_sleep_ms(5);
	}
	eh_ev_poll(t);
}

static void tc_child_watchdog(void)
{
	struct eh t;

	setenv("UDSP_WATCHDOG_MS", TC_WATCHDOG_MS, 1);
	tc_wedge_open(&t, &tw1_ops, &tw[1], true);
	if (!tc_child->started)
		return;

	tc_fault_poll(&t, 3000000u, true);
	/* The latch posts one fault. The window after it shows that no second
	 * fault follows. */
	tc_fault_poll(&t, TC_WATCHDOG_WINDOW_US, false);
	for (unsigned i = 0; i < eh_ev_len; i++) {
		if (eh_ev[i].type == UDSP_EV_SLIC_FAULT &&
		    eh_ev[i].u.fault.code == UDSP_FAULT_ENGINE_STALL)
			tc_child->faults++;
	}
	tc_child->fatal_op = udsp_line_state(t.s, 0, UDSP_LINE_DOWN) == UDSP_FATAL;
}

static void tc_child_unpaced(void)
{
	unsigned faults = 0;
	struct eh t;

	setenv("UDSP_MAX_TICKS", "1000000", 1);
	setenv("UDSP_WATCHDOG_MS", TC_WATCHDOG_MS, 1);
	tc_wedge_open(&t, &tw2_ops, &tw[2], false);
	unsetenv("UDSP_MAX_TICKS");
	if (!tc_child->started)
		return;

	tc_fault_poll(&t, TC_WATCHDOG_WINDOW_US, false);
	for (unsigned i = 0; i < eh_ev_len; i++) {
		if (eh_ev[i].type == UDSP_EV_SLIC_FAULT)
			faults++;
	}
	tc_child->no_fault = faults == 0;
}

/* The engine tick thread overflows its own stack. Only the alternate stack it
 * installs before rt_apply can serve that fault. */
static void tc_child_engine_overflow(void)
{
	struct udsp_config cfg;
	struct eh t;

	tc_backend_reset();
	udsp_slic_fake_reset();
	udsp_safe_state_msg_set(tc_blit_record, tc_page);
	udsp_tdm_override(&tc_ops);
	tc_cfg(&cfg, "tc", NULL);
	if (!tc_open_handle(&t, &cfg))
		_exit(2);
	/* Arm the overflow only once open returned: the paced tick runs during
	 * the attach, before the crash handler is armed, so an earlier fault
	 * would arrive unhandled. */
	tc_be.recurse = true;
	eh_sleep_ms(4000);
	_exit(3);
}

static void case_watchdog(void)
{
	struct udsp_config cfg;
	struct eh t;

	CHECK(pipe(tc_wedge_pipe) == 0);

	CHECK(tc_fork_run(tc_child_bounded_stop));

	CASE("a wedged backend bounds the stop and leaks deliberately");
	{
		CHECK(tc_child->opened);
		CHECK(tc_child->started);
		CHECK(tc_child->stop_accepted);
		CHECK(tc_child->job_seen);
		/* The safe pass and the engine stop are each bounded by the
		 * engine deadline. */
		CHECK(tc_child->stop_us <
		      (2u * UDSP_STOP_ENGINE_MS + 600u) * 1000u);
		CHECK(tc_child->job_status != 0);
		CHECK(tc_child->dead);
	}

	CASE("a leaked handle keeps the one live handle guard taken");
	{
		/* A thread nobody could stop still holds the plug-in
		 * instance, so no second handle may open. */
		CHECK(tc_child->reopen_refused);
	}

	CASE("the watchdog faults a stalled paced engine");
	{
		CHECK(tc_fork_run(tc_child_watchdog));
		CHECK(tc_child->started);
		CHECK_UINT(tc_child->faults, 1);
		CHECK(tc_child->fatal_op);
	}

	CASE("an unpaced engine that stops ticking raises no stall");
	{
		CHECK(tc_fork_run(tc_child_unpaced));
		CHECK(tc_child->started);
		/* A virtual clock has no pace to miss, so a tick count that
		 * stops moving is not a stall: the paced guard is the false
		 * positive rule. */
		CHECK(tc_child->no_fault);
	}

	close(tc_wedge_pipe[0]);
	close(tc_wedge_pipe[1]);

	CASE("the engine tick thread faults on its own alternate stack");
	{
		int sig = 0;

		memset(tc_page, 0, sizeof(*tc_page));
		CHECK(tc_crash_child_run(tc_child_engine_overflow, &sig));
		CHECK(sig == SIGSEGV);
		CHECK(tc_page->messages > 0);
	}

	CASE("a bounded file backend run raises no stall");
	{
		unsigned faults = 0;

		udsp_slic_fake_reset();
		setenv("UDSP_MAX_TICKS", "3000", 1);
		setenv("UDSP_WATCHDOG_MS", TC_WATCHDOG_MS, 1);
		tc_cfg(&cfg, "file", "USA");
		CHECK(tc_open_handle(&t, &cfg));
		if (t.s) {
			tc_fault_poll(&t, TC_WATCHDOG_WINDOW_US, false);
			for (unsigned i = 0; i < eh_ev_len; i++) {
				if (eh_ev[i].type == UDSP_EV_SLIC_FAULT)
					faults++;
			}
			CHECK_UINT(faults, 0);
			tc_free(&t);
		}
		unsetenv("UDSP_WATCHDOG_MS");
		unsetenv("UDSP_MAX_TICKS");
	}

	CASE("a paced backend ignores the tick bound of the environment");
	{
		struct engine_stats st;

		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_tdm_override(&tc_ops);
		setenv("UDSP_MAX_TICKS", "5", 1);
		tc_cfg(&cfg, "tc", "USA");
		CHECK(tc_open_handle(&t, &cfg));
		unsetenv("UDSP_MAX_TICKS");
		if (t.s) {
			CHECK(tc_start(&t));
			eh_wait_ms(&t, 300);
			pthread_mutex_lock(&t.s->engine_lock);
			memset(&st, 0, sizeof(st));
			if (t.s->engine)
				engine_stats_get(t.s->engine, &st);
			pthread_mutex_unlock(&t.s->engine_lock);
			/* The tick bound binds unpaced backends only. */
			CHECK(st.ticks > 20);
			CHECK(st.running);
			tc_down(&t);
		}
	}
}

static void case_restart(void)
{
	struct eh t;
	bool offhook = true;

	CASE("stop and start again brings the lines back");
	{
		struct udsp_job job = { 0 };
		int32_t status = -1;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 50);
		CHECK(udsp_line_hook_query(t.s, 0, &offhook) == UDSP_OK);
		CHECK(tc_stop(&t));
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) == UDSP_TRANSIENT);

		CHECK(udsp_start(t.s, &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 5000, &status));
		CHECK_UINT((uint32_t)status, 0);
		eh_wait_ms(&t, 60);
		CHECK(udsp_line_hook_query(t.s, 0, &offhook) == UDSP_OK);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_OHT) == UDSP_OK);
		tc_down(&t);
	}

	CASE("stream and diagnostic ops answer unsupported");
	{
		struct udsp_media_stats stats;
		struct udsp_media media;
		struct udsp_job job = { 0 };

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		memset(&media, 0, sizeof(media));
		CHECK(udsp_stream_config(t.s, 0, 0, &media, -1, -1) == UDSP_UNSUPPORTED);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_UNSUPPORTED);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_UNSUPPORTED);
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '1', 100) == UDSP_UNSUPPORTED);
		CHECK(udsp_stream_stats(t.s, 0, 0, &stats) == UDSP_UNSUPPORTED);
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_OFF, 0, 0) == UDSP_UNSUPPORTED);
		CHECK(udsp_conf(t.s, 0, UDSP_CONF_OFF) == UDSP_UNSUPPORTED);
		/* A test the caller never ran has no result to copy. */
		CHECK(udsp_line_test(t.s, 0, UDSP_TEST_LOOP + 1, &job) ==
		      UDSP_UNSUPPORTED);
		CHECK_UINT(job.token, 0);
		{
			size_t len = 1;

			CHECK(udsp_line_test_result(t.s, 0, NULL, 0, &len) ==
			      UDSP_UNSUPPORTED);
			CHECK_UINT(len, 0);
		}
		tc_down(&t);
	}
}

/* The engine's published fault word of the line (chan.h UDSP_FAULT_PUB_*),
 * read the way the monitor getter and the slic thread read it. */
static uint32_t tc_fault_pub(struct eh *t, unsigned line)
{
	return (uint32_t)atomic_load_explicit(&t->s->chan[line].fault_pub,
					      memory_order_acquire);
}

static unsigned tc_fault_events(unsigned line, bool clear)
{
	unsigned n = 0;

	for (unsigned i = 0; i < eh_ev_len; i++) {
		if (eh_ev[i].type != UDSP_EV_SLIC_FAULT ||
		    eh_ev[i].line != line)
			continue;
		if ((eh_ev[i].u.fault.code == 0) == clear)
			n++;
	}

	return n;
}

/* The status of the nth UDSP_EV_CID_SENT, or INT_MIN past the last. */
static int tc_cid_sent_nth(unsigned n)
{
	const struct udsp_event *ev = eh_ev_find(UDSP_EV_CID_SENT, 0);

	while (ev && n--)
		ev = eh_ev_find(UDSP_EV_CID_SENT, (unsigned)(ev - eh_ev) + 1u);

	return ev ? ev->u.cid_sent.status : INT_MIN;
}

static int tc_cid_sent_status(void)
{
	return tc_cid_sent_nth(0);
}

static unsigned tc_line_events(unsigned state)
{
	unsigned n = 0;

	for (unsigned i = 0; i < eh_ev_len; i++) {
		if (eh_ev[i].type == UDSP_EV_LINE &&
		    eh_ev[i].u.line.state == state)
			n++;
	}

	return n;
}

static unsigned tc_line_down_count(void)
{
	return tc_line_events(UDSP_LINE_DOWN);
}

static bool tc_hook_fail_wait(struct eh *t, unsigned failures, unsigned lines,
			      uint64_t end)
{
	unsigned mask = 0;
	unsigned want = (1u << lines) - 1u;
	bool in_time = eh_mono_us() < end;

	CHECK(in_time);
	if (!in_time)
		return false;
	/* One-shot arms race the sample passes and permit successful reads
	 * between failures, so the whole consecutive batch must stay armed. */
	udsp_slic_fake_error_repeat_set(t->s->slic, UDSP_SLIC_FAKE_HOOK_READ,
					-EIO, failures);
	while (tc_line_down_count() < lines && eh_mono_us() < end) {
		eh_ev_poll(t);
		if (tc_line_down_count() < lines)
			eh_sleep_ms(1);
	}
	eh_ev_poll(t);
	in_time = eh_mono_us() < end;
	for (unsigned i = 0; i < eh_ev_len; i++) {
		if (eh_ev[i].type == UDSP_EV_LINE &&
		    eh_ev[i].u.line.state == UDSP_LINE_DOWN &&
		    eh_ev[i].line < UDSP_LINE_MAX)
			mask |= 1u << eh_ev[i].line;
	}
	CHECK(in_time);
	CHECK_UINT(tc_line_down_count(), lines);
	CHECK_UINT(mask, want);

	return in_time && tc_line_down_count() == lines && mask == want;
}

static unsigned tc_converter_read_count(struct eh *t, unsigned line)
{
	struct udsp_slic_fake_chan_state st;

	if (!udsp_slic_fake_chan_state_get(t->s->slic, line, &st))
		return 0;
	return st.converter_read_count;
}

/* The code of the nth fault event of line 0, or a sentinel past the last.
 * Keeps the event-sequence checks out of a fourth block level. */
static uint32_t tc_fault_code(unsigned n)
{
	unsigned seen = 0;

	for (unsigned i = 0; i < eh_ev_len; i++) {
		if (eh_ev[i].type != UDSP_EV_SLIC_FAULT || eh_ev[i].line != 0)
			continue;
		if (seen++ == n)
			return eh_ev[i].u.fault.code;
	}

	return 0xffffffffu;
}

static unsigned tc_init_count(struct eh *t, unsigned line)
{
	struct udsp_slic_fake_chan_state st;

	if (!udsp_slic_fake_chan_state_get(t->s->slic, line, &st))
		return 0;
	return st.init_count;
}

static unsigned tc_region_count(struct eh *t, unsigned line)
{
	struct udsp_slic_fake_chan_state st;

	if (!udsp_slic_fake_chan_state_get(t->s->slic, line, &st))
		return 0;
	return st.region_count;
}

static unsigned tc_reinit_count(struct eh *t, unsigned line)
{
	struct udsp_slic_fake_chan_state st;

	if (!udsp_slic_fake_chan_state_get(t->s->slic, line, &st))
		return 0;
	return st.reinit_count;
}

static unsigned tc_converter_count(struct eh *t, unsigned line)
{
	struct udsp_slic_fake_chan_state st;

	if (!udsp_slic_fake_chan_state_get(t->s->slic, line, &st))
		return 0;
	return st.converter_restart_count;
}

static unsigned tc_recover_jobs(struct eh *t)
{
	return (unsigned)atomic_load_explicit(&t->s->recover_jobs,
					      memory_order_acquire);
}

/* Waits in the wall clock for a recovery to be in flight, then a moment more
 * so the vendor op is running. */
static bool tc_wait_recover(struct eh *t, unsigned ms)
{
	uint64_t end = eh_mono_us() + (uint64_t)ms * 1000u;

	while (eh_mono_us() < end) {
		if (tc_recover_jobs(t)) {
			tc_wait_real_ms(t, 20);
			return true;
		}
		eh_ev_poll(t);
		eh_sleep_ms(1);
	}

	return false;
}

/* The DCDC-down converter words a clean restart reads: the converter is
 * powered down, no over-voltage lock-out, the loop open. */
static void tc_monitor_dcdc_down(struct udsp_slic_monitor *m)
{
	memset(m, 0, sizeof(*m));
	m->pd_dcdc = 1u << 20;
	m->vbat_mv = -48000;
	m->vbat_tgt_mv = -48000;
	m->vbat_irq_th_mv = 5000;
}

/* True when the fault events of line 0 alternate, a fault first: a clear is
 * posted once per fault and never twice in a row. */
static bool tc_fault_alternates(void)
{
	bool want_fault = true;

	for (unsigned i = 0; i < eh_ev_len; i++) {
		if (eh_ev[i].type != UDSP_EV_SLIC_FAULT || eh_ev[i].line != 0)
			continue;
		if ((eh_ev[i].u.fault.code != 0) != want_fault)
			return false;
		want_fault = !want_fault;
	}

	return true;
}

static unsigned tc_linefeed_count(struct eh *t, unsigned line)
{
	struct udsp_slic_fake_chan_state st;

	if (!udsp_slic_fake_chan_state_get(t->s->slic, line, &st))
		return 0;
	return st.linefeed_count;
}

/* True when no two consecutive fault events of line 0 carry the same code.
 * A fault posts once per set, so a set that repeats posts nothing. */
static bool tc_fault_no_repeat(void)
{
	uint32_t prev = 0xffffffffu;
	bool seen = false;

	for (unsigned i = 0; i < eh_ev_len; i++) {
		if (eh_ev[i].type != UDSP_EV_SLIC_FAULT || eh_ev[i].line != 0)
			continue;
		if (eh_ev[i].u.fault.code == 0)
			continue;
		if (seen && eh_ev[i].u.fault.code == prev)
			return false;
		prev = eh_ev[i].u.fault.code;
		seen = true;
	}

	return true;
}

/* One line up on a fake whose nullable ops are opt_mask. The mask is set
 * before the open, because the engine reads the ops table on its own thread
 * from then on. */
static bool tc_up_opt(struct eh *t, uint32_t opt_mask)
{
	struct udsp_config cfg;

	tc_backend_reset();
	udsp_slic_fake_reset();
	udsp_slic_fake_optional_set(opt_mask);
	udsp_tdm_override(&tc_ops);
	tc_cfg(&cfg, "tc", "USA");
	cfg.populated_ports = 1;
	if (!tc_open_handle(t, &cfg))
		return false;

	return tc_start(t);
}

/*
 * A fault whose hold lasts until the next start. The feed opens and never
 * returns on the trial. A feed command changes nothing on the line. A read
 * without the fault bit lifts nothing. A stop and start bring the line back
 * with the clear that the fault still has to post.
 */
static void tc_fault_held_until_start(struct eh *t, uint32_t bit)
{
	unsigned writes;

	tc_pace_fast();
	eh_wait_ms(t, 60);
	udsp_slic_fake_fault_set(t->s->slic, 0, bit);
	eh_wait_ms(t, 1400);
	CHECK_UINT(tc_fault_events(0, false), 1);
	CHECK_UINT(tc_fault_pub(t, 0),
		   bit | UDSP_FAULT_PUB_OPEN | UDSP_FAULT_PUB_HELD);
	/* Past the 5 s trial the feed is still open. */
	eh_wait_ms(t, 7000);
	CHECK_UINT(tc_lf_confirmed(t, 0), UDSP_LF_OPEN);
	CHECK_UINT(tc_fault_pub(t, 0),
		   bit | UDSP_FAULT_PUB_OPEN | UDSP_FAULT_PUB_HELD);
	writes = tc_linefeed_count(t, 0);
	CHECK_UINT(writes, 2);
	CHECK(udsp_line_state(t->s, 0, UDSP_LINE_ACTIVE) == UDSP_OK);
	eh_wait_ms(t, 300);
	CHECK_UINT(tc_linefeed_count(t, 0), writes);
	CHECK_UINT(tc_lf_confirmed(t, 0), UDSP_LF_OPEN);
	/* The restart's init is what clears the plug-in's verdict; a clean
	 * read before it lifts nothing. */
	udsp_slic_fake_fault_set(t->s->slic, 0, 0);
	eh_wait_ms(t, 2500);
	CHECK_UINT(tc_fault_pub(t, 0),
		   UDSP_FAULT_PUB_OPEN | UDSP_FAULT_PUB_HELD);
	CHECK_UINT(tc_linefeed_count(t, 0), writes);
	CHECK_UINT(tc_fault_events(0, true), 0);
	CHECK(tc_stop(t));
	CHECK(tc_start(t));
	eh_wait_ms(t, 1500);
	CHECK_UINT(tc_lf_confirmed(t, 0), UDSP_LF_FWD_ACTIVE);
	CHECK_UINT(tc_fault_events(0, true), 1);
	CHECK_UINT(tc_fault_events(0, false), 1);
	CHECK_UINT(tc_fault_pub(t, 0), 0);
	tc_down(t);
}

static void case_fault(void)
{
	struct eh t;

	CASE("a start with no line ready completes fatal");
	{
		struct udsp_config cfg;
		struct udsp_job job = { 0 };
		int32_t status = 0;

		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_slic_fake_channels_set(1);
		udsp_tdm_override(&tc_ops);
		tc_cfg(&cfg, "tc", "USA");
		cfg.populated_ports = 1;
		CHECK(tc_open_handle(&t, &cfg));
		if (t.s) {
			udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_INIT,
						 -EIO);
			CHECK(udsp_start(t.s, &job) == UDSP_OK);
			CHECK(eh_job_wait(&t, job.token, 5000, &status));
			CHECK(status == UDSP_FATAL);
			/* The line is dead, so every later op is fatal. */
			CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) ==
			      UDSP_FATAL);
			tc_free(&t);
		}
		udsp_tdm_override(NULL);
	}

	CASE("ten hook read failures mark the line down");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		if (!tc_hook_fail_wait(&t, UDSP_HOOK_FAIL_LIMIT, 1,
				       eh_mono_us() + 5000000u))
			goto hook_fail_out;
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) == UDSP_FATAL);
		{
			bool level = false;

			/* The cached level of a dead line is whatever the bus
			 * last managed to read. */
			CHECK(udsp_line_hook_query(t.s, 0, &level) == UDSP_FATAL);
		}
	hook_fail_out:
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_HOOK_READ, 0);
		tc_stop(&t);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a slic fault opens the feed and raises the event");
	{
		struct udsp_slic_fake_chan_state st;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_THERMAL);
		eh_wait_ms(&t, 1400);
		CHECK_UINT(tc_fault_events(0, false), 1);
		CHECK_UINT(tc_fault_code(0), UDSP_FAULT_SLIC_THERMAL);
		CHECK_UINT(tc_fault_pub(&t, 0),
			   UDSP_FAULT_SLIC_THERMAL | UDSP_FAULT_PUB_OPEN);
		/* The retry re-asserts base_lf 5 s after the feed was opened.
		 * Only a fault_read that returns 0 lets it stand. The clear
		 * follows the first clean read after the feed confirmed. */
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		eh_wait_ms(&t, 7000);
		CHECK_UINT(tc_fault_code(1), 0);
		CHECK_UINT(tc_fault_events(0, true), 1);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_fault_pub(&t, 0), 0);
		CHECK(tc_stop(&t));
		CHECK(tc_state(&t, 0, &st));
		/* FWD_ACTIVE at start, OPEN on the fault, FWD_ACTIVE on the
		 * retry, OPEN at stop: a run without a fault writes two. */
		CHECK_UINT(st.linefeed_count, 4);
		CHECK_UINT(st.linefeed, UDSP_LF_OPEN);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a persistent fault posts one event and no clear");
	{
		struct udsp_slic_fake_chan_state st;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_THERMAL);
		/* The read comes within a second of the set, at r. The trial
		 * runs at r + 5 s and the next read reopens the feed at
		 * r + 6 s. So 13.5 s holds two trials and two reopens with a
		 * margin on both sides. */
		eh_wait_ms(&t, 13500);
		CHECK_UINT(tc_fault_events(0, false), 1);
		CHECK_UINT(tc_fault_events(0, true), 0);
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		CHECK(tc_stop(&t));
		CHECK(tc_state(&t, 0, &st));
		/* start, fault open, trial, fault open, trial, fault open;
		 * the stop finds the feed open and writes nothing. */
		CHECK_UINT(st.linefeed_count, 6);
		CHECK_UINT(tc_fault_events(0, true), 0);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a flapping fault posts one event per fault and one clear each");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		/* Each round: the fault posts, and a clean read inside the 5 s
		 * trial clears nothing. The fault back again posts nothing,
		 * and the clean read after the trial clears once. */
		for (unsigned i = 0; i < 3; i++) {
			udsp_slic_fake_fault_set(t.s->slic, 0,
						 UDSP_FAULT_SLIC_THERMAL);
			eh_wait_ms(&t, 1400);
			CHECK_UINT(tc_fault_events(0, false), i + 1);
			CHECK_UINT(tc_fault_events(0, true), i);
			udsp_slic_fake_fault_set(t.s->slic, 0, 0);
			eh_wait_ms(&t, 2000);
			CHECK_UINT(tc_fault_events(0, true), i);
			udsp_slic_fake_fault_set(t.s->slic, 0,
						 UDSP_FAULT_SLIC_THERMAL);
			eh_wait_ms(&t, 1400);
			CHECK_UINT(tc_fault_events(0, false), i + 1);
			udsp_slic_fake_fault_set(t.s->slic, 0, 0);
			eh_wait_ms(&t, 7200);
			CHECK_UINT(tc_fault_events(0, true), i + 1);
		}
		CHECK(tc_fault_alternates());
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_fault_pub(&t, 0), 0);
		tc_down(&t);
	}

	CASE("a stop mid-hold and a start deliver the clear after the start");
	{
		unsigned hooks;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_THERMAL);
		eh_wait_ms(&t, 1400);
		CHECK_UINT(tc_fault_events(0, false), 1);
		/* Inside the 5 s trial, with the fault still in the cell. No
		 * clean read can precede the stop, and sampling stops before
		 * the lines go safe. */
		CHECK(tc_stop(&t));
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		CHECK_UINT(tc_fault_events(0, true), 0);
		CHECK_UINT(tc_fault_pub(&t, 0),
			   UDSP_FAULT_SLIC_THERMAL | UDSP_FAULT_PUB_OPEN);
		/* The first fault read after the start sleeps, so the word read
		 * here is the one the start itself published. */
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_FAULT_READ,
					 1500);
		CHECK(tc_start(&t));
		CHECK_UINT(tc_fault_pub(&t, 0), 0);
		/* The first fault read after the start still sleeps, so the
		 * hook machine runs at once. Its seize arrives well before the
		 * five second trial from before the stop can expire. */
		hooks = eh_ev_count(UDSP_EV_HOOK);
		tc_hook_set(&t, true);
		eh_wait_ms(&t, 500);
		CHECK(eh_ev_count(UDSP_EV_HOOK) > hooks);
		CHECK_UINT(tc_hook_last(), UDSP_HOOK_SEIZE);
		tc_hook_set(&t, false);
		/* Let the delayed clean read finish, then the clear follows
		 * once the feed confirms. */
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_FAULT_READ,
					 0);
		tc_wait_real_ms(&t, 1700);
		eh_wait_ms(&t, 1500);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_fault_events(0, true), 1);
		CHECK_UINT(tc_fault_events(0, false), 1);
		CHECK_UINT(tc_fault_pub(&t, 0), 0);
		tc_down(&t);
	}

	CASE("a fault on a DOWN line clears only once the line takes a feed");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_DOWN) == UDSP_OK);
		eh_wait_ms(&t, 100);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_THERMAL);
		eh_wait_ms(&t, 1400);
		CHECK_UINT(tc_fault_events(0, false), 1);
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		/* The open base feed of a DOWN line is confirmed trivially and
		 * clears nothing: the line never took a feed back. */
		eh_wait_ms(&t, 8000);
		CHECK_UINT(tc_fault_events(0, true), 0);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		CHECK_UINT(tc_fault_pub(&t, 0), 0);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) == UDSP_OK);
		eh_wait_ms(&t, 1500);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_fault_events(0, true), 1);
		tc_down(&t);
	}

	CASE("a reinit verdict on a plug-in without the op holds the line");
	{
		if (!tc_up_opt(&t, UDSP_SLIC_FAKE_OPT_ALL &
				   ~(uint32_t)UDSP_SLIC_FAKE_OPT_REINIT)) {
			CHECK(false);
			return;
		}
		tc_fault_held_until_start(&t, UDSP_FAULT_SLIC_REINIT);
	}

	CASE("a converter fault on a plug-in without the op holds the line");
	{
		if (!tc_up_opt(&t, UDSP_SLIC_FAKE_OPT_ALL &
				   ~(uint32_t)UDSP_SLIC_FAKE_OPT_CONVERTER)) {
			CHECK(false);
			return;
		}
		tc_fault_held_until_start(&t, UDSP_FAULT_SLIC_DCDC);
	}

	CASE("a clock fault holds the line until the clocks read back");
	{
		struct udsp_slic_fake_chan_state st;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_CLOCK);
		eh_wait_ms(&t, 1400);
		CHECK_UINT(tc_fault_events(0, false), 1);
		CHECK_UINT(tc_fault_pub(&t, 0), UDSP_FAULT_SLIC_CLOCK |
			   UDSP_FAULT_PUB_OPEN | UDSP_FAULT_PUB_HELD);
		eh_wait_ms(&t, 7000);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		CHECK_UINT(tc_linefeed_count(&t, 0), 2);
		CHECK_UINT(tc_fault_events(0, true), 0);
		/* A read without the bit lifts the hold and the base feed
		 * returns; the clear follows once the feed confirmed. */
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		eh_wait_ms(&t, 2500);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_fault_events(0, true), 1);
		CHECK_UINT(tc_fault_pub(&t, 0), 0);
		CHECK(tc_stop(&t));
		CHECK(tc_state(&t, 0, &st));
		CHECK_UINT(st.linefeed_count, 4);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a device reset verdict holds the line until the next start");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_fault_held_until_start(&t, UDSP_FAULT_DEVICE_RESET);
	}

	CASE("a flapping clock fault cannot postpone the trial of another");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		/* The clock bit comes and goes every 1.3 s against the 1 s
		 * fault poll while THERMAL stands. The open deadline is armed
		 * once, so the THERMAL trial runs at the first clock-clean read
		 * past it, and THERMAL reopens the feed after it. */
		for (unsigned i = 0; i < 12; i++) {
			udsp_slic_fake_fault_set(t.s->slic, 0, i % 2 ?
						 UDSP_FAULT_SLIC_THERMAL :
						 UDSP_FAULT_SLIC_THERMAL |
						 UDSP_FAULT_SLIC_CLOCK);
			eh_wait_ms(&t, 1300);
		}
		CHECK(tc_linefeed_count(&t, 0) >= 3);
		CHECK(tc_fault_events(0, false) >= 2);
		/* One event per set change, no duplicate, and a bound: 12
		 * toggles cannot post more than 13 fault events. */
		CHECK(tc_fault_events(0, false) <= 13);
		CHECK(tc_fault_no_repeat());
		/* The first read carries the thermal bit. */
		CHECK(tc_fault_code(0) & UDSP_FAULT_SLIC_THERMAL);
		CHECK_UINT(tc_fault_events(0, true), 0);
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		eh_wait_ms(&t, 8000);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_fault_events(0, true), 1);
		CHECK_UINT(tc_fault_pub(&t, 0), 0);
		tc_down(&t);
	}

	CASE("a ring session ended during a hold leaves the part open");
	{
		struct udsp_ring ring;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		tc_ring_fill(&ring, 2000, 4000, 30000);
		CHECK(udsp_ring_start(t.s, 0, &ring) == UDSP_OK);
		eh_wait_ms(&t, 200);
		CHECK(eh_ev_find(UDSP_EV_RING_START, 0) != NULL);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_CLOCK);
		eh_wait_ms(&t, 1400);
		CHECK_UINT(tc_fault_events(0, false), 1);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		CHECK_UINT(tc_linefeed_count(&t, 0), 2);
		/* The vendor ring stop writes the active feed, so a held line
		 * is put back open and lf_confirmed follows it. */
		CHECK(udsp_ring_stop(t.s, 0) == UDSP_OK);
		eh_wait_ms(&t, 300);
		CHECK(eh_ev_find(UDSP_EV_RING_END, 0) != NULL);
		CHECK_UINT(tc_linefeed_count(&t, 0), 3);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		eh_wait_ms(&t, 2500);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_linefeed_count(&t, 0), 4);
		CHECK_UINT(tc_fault_events(0, true), 1);
		tc_down(&t);
	}

	CASE("a caller identity sequence aborted during a hold writes no feed");
	{
		struct udsp_cid cid;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		/* A long message: the burst outlasts the second the fault read
		 * can take to arrive. The fault then freezes the sequence
		 * between its on-hook transmission feed and its restore. */
		tc_cid_fill(&cid, "12345678901234567890123456789012",
			    "abcdefghijklmnopqrstuvwxyzabcdef");
		CHECK(udsp_cid_send(t.s, 0, UDSP_CID_TYPE1, &cid) == UDSP_OK);
		eh_wait_ms(&t, 100);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_OHT);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_CLOCK);
		eh_wait_ms(&t, 1400);
		CHECK_UINT(tc_fault_events(0, false), 1);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		CHECK_UINT(tc_linefeed_count(&t, 0), 3);
		CHECK_UINT(eh_ev_count(UDSP_EV_CID_SENT), 0);
		CHECK(udsp_ring_stop(t.s, 0) == UDSP_OK);
		eh_wait_ms(&t, 300);
		CHECK(tc_cid_sent_status() == -ECANCELED);
		CHECK_UINT(tc_linefeed_count(&t, 0), 3);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		eh_wait_ms(&t, 2500);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_linefeed_count(&t, 0), 4);
		CHECK_UINT(tc_fault_events(0, true), 1);
		tc_down(&t);
	}

	CASE("a state and a polarity request during a hold apply on the lift");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_CLOCK);
		eh_wait_ms(&t, 1400);
		CHECK_UINT(tc_fault_events(0, false), 1);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_OHT) == UDSP_OK);
		CHECK(udsp_line_polarity(t.s, 0, UDSP_POL_REVERSE) == UDSP_OK);
		eh_wait_ms(&t, 300);
		CHECK_UINT(tc_linefeed_count(&t, 0), 2);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		/* The polarity resolves against the deferred OHT, not the
		 * active feed the hold interrupted. */
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		eh_wait_ms(&t, 2500);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_REV_OHT);
		CHECK_UINT(tc_linefeed_count(&t, 0), 3);
		CHECK_UINT(tc_fault_events(0, true), 1);
		CHECK_UINT(tc_fault_pub(&t, 0), 0);
		tc_down(&t);
	}

	CASE("a DOWN request during a hold leaves the lifted line open");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_CLOCK);
		eh_wait_ms(&t, 1400);
		CHECK_UINT(tc_fault_events(0, false), 1);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_DOWN) == UDSP_OK);
		eh_wait_ms(&t, 300);
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		eh_wait_ms(&t, 3000);
		/* The lift applies the deferred open feed: nothing to write,
		 * and no clear for a line that took no feed back. */
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		CHECK_UINT(tc_linefeed_count(&t, 0), 2);
		CHECK_UINT(tc_fault_events(0, true), 0);
		CHECK_UINT(tc_fault_pub(&t, 0), 0);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) == UDSP_OK);
		eh_wait_ms(&t, 1500);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_fault_events(0, true), 1);
		tc_down(&t);
	}
}

static void case_recover(void)
{
	struct eh t;

	CASE("a reinit verdict with the op present recovers and clears");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_REINIT);
		eh_wait_ms(&t, 4000);
		CHECK_UINT(tc_reinit_count(&t, 0), 1);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_fault_events(0, false), 1);
		CHECK_UINT(tc_fault_events(0, true), 1);
		/* The recovery never set a hold; the op was present. */
		CHECK_UINT(tc_fault_pub(&t, 0) & UDSP_FAULT_PUB_HELD, 0);
		CHECK_UINT(tc_fault_pub(&t, 0), 0);
		tc_down(&t);
	}

	CASE("a failed restore write defers the clear until the feed confirms");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_THERMAL);
		eh_wait_ms(&t, 1400);
		CHECK_UINT(tc_fault_events(0, false), 1);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		/* Every restore write fails, so the base feed cannot confirm
		 * and no clear is due. The open write already ran, so this
		 * fails only the powered restore. */
		udsp_slic_fake_error_repeat_set(t.s->slic,
						UDSP_SLIC_FAKE_LINEFEED_SET,
						-EIO, 100);
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		eh_wait_ms(&t, 9000);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		CHECK_UINT(tc_fault_events(0, true), 0);
		/* The error disarmed, the next clean read re-asserts the feed,
		 * it confirms and the clear follows. */
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_LINEFEED_SET,
					 0);
		eh_wait_ms(&t, 4000);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_fault_events(0, true), 1);
		tc_down(&t);
	}

	CASE("a reinit that fails twice then succeeds recovers");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		udsp_slic_fake_error_repeat_set(t.s->slic,
						UDSP_SLIC_FAKE_REINIT, -EIO, 2);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_REINIT);
		/* Two holdoffs of five seconds of engine time each, then the
		 * third attempt is the one the fake counts. */
		eh_wait_ms(&t, 14000);
		CHECK_UINT(tc_reinit_count(&t, 0), 1);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_fault_events(0, true), 1);
		CHECK_UINT(tc_fault_pub(&t, 0) & UDSP_FAULT_PUB_HELD, 0);
		tc_down(&t);
	}

	CASE("three reinit failures hold the line with one log line");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		tc_log_reset();
		udsp_slic_fake_error_repeat_set(t.s->slic,
						UDSP_SLIC_FAKE_REINIT, -EIO, 5);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_REINIT);
		eh_wait_ms(&t, 20000);
		CHECK_UINT(tc_reinit_count(&t, 0), 0);
		CHECK(tc_fault_pub(&t, 0) & UDSP_FAULT_PUB_HELD);
		CHECK_UINT(tc_fault_events(0, true), 0);
		CHECK_UINT(tc_log_hits(UDSP_LOG_ERR, "held after"), 1);
		CHECK_UINT(tc_line_down_count(), 0);
		tc_down(&t);
	}

	CASE("a converter fault with clean words runs the converter op");
	{
		struct udsp_slic_monitor m;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		tc_monitor_dcdc_down(&m);
		udsp_slic_fake_monitor_set(t.s->slic, 0, &m);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_DCDC);
		eh_wait_ms(&t, 4000);
		CHECK_UINT(tc_converter_count(&t, 0), 1);
		CHECK_UINT(tc_reinit_count(&t, 0), 0);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_fault_events(0, true), 1);
		tc_down(&t);
	}

	CASE("a converter down with a short waits until the loop clears");
	{
		struct udsp_slic_monitor m;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		tc_monitor_dcdc_down(&m);
		m.lcrrtp = 1u << 1;		/* LCR: the loop is closed */
		udsp_slic_fake_monitor_set(t.s->slic, 0, &m);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_DCDC);
		eh_wait_ms(&t, 6000);
		/* A suspected short: no attempt while the loop reads closed. */
		CHECK_UINT(tc_converter_count(&t, 0), 0);
		CHECK_UINT(tc_fault_events(0, true), 0);
		m.lcrrtp = 0;
		udsp_slic_fake_monitor_set(t.s->slic, 0, &m);
		/* The next holdoff re-reads the words and runs the attempt. */
		eh_wait_ms(&t, 7000);
		CHECK_UINT(tc_converter_count(&t, 0), 1);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_fault_events(0, true), 1);
		tc_down(&t);
	}

	CASE("an over-voltage lock-out read twice holds the line");
	{
		struct udsp_slic_monitor m;
		uint64_t end;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		tc_monitor_dcdc_down(&m);
		m.dcdc_status = 1u << 22;	/* OVLO */
		udsp_slic_fake_monitor_set(t.s->slic, 0, &m);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_DCDC);
		/* Missed SLIC passes delay both reads beyond the holdoff.
		 * Each tick wait has its own wall limit, so a stopped clock
		 * also needs one limit for the whole loop. */
		end = eh_mono_us() + 5000000u;
		for (unsigned i = 0; i < 3000u && eh_mono_us() < end &&
		     !(tc_fault_pub(&t, 0) & UDSP_FAULT_PUB_HELD); i++)
			eh_wait_ms(&t, 10);
		/* Never restarted: the vendor power-up would erase the lock-out
		 * evidence. */
		CHECK_UINT(tc_converter_count(&t, 0), 0);
		CHECK_UINT(tc_converter_read_count(&t, 0), 2);
		CHECK(tc_fault_pub(&t, 0) & UDSP_FAULT_PUB_HELD);
		CHECK_UINT(tc_fault_events(0, true), 0);
		tc_down(&t);
	}

	CASE("a reinit and converter fault runs the converter op first");
	{
		struct udsp_slic_monitor m;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		tc_monitor_dcdc_down(&m);
		udsp_slic_fake_monitor_set(t.s->slic, 0, &m);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_REINIT |
					 UDSP_FAULT_SLIC_DCDC);
		eh_wait_ms(&t, 6000);
		/* The converter op runs first; the reinit op runs only after a
		 * read without the DCDC bit, which the converter op cleared. */
		CHECK_UINT(tc_converter_count(&t, 0), 1);
		CHECK_UINT(tc_reinit_count(&t, 0), 1);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_fault_events(0, true), 1);
		tc_down(&t);
	}

	CASE("a sibling sync slic op is transient during a recovery");
	{
		if (!tc_up_ports(&t, "USA", 2)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_REINIT, 400);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_REINIT);
		CHECK(tc_wait_recover(&t, 4000));
		CHECK(udsp_line_state(t.s, 1, UDSP_LINE_OHT) == UDSP_TRANSIENT);
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_REINIT, 0);
		{
			uint64_t end = eh_mono_us() + 2000000u;

			while (tc_recover_jobs(&t) && eh_mono_us() < end) {
				eh_ev_poll(&t);
				eh_sleep_ms(1);
			}
		}
		eh_wait_ms(&t, 500);
		CHECK(udsp_line_state(t.s, 1, UDSP_LINE_OHT) == UDSP_OK);
		tc_down(&t);
	}

	CASE("a stop queued during a pass wins over an eligible recovery");
	{
		struct udsp_job region = { 0 };
		struct udsp_job stop = { 0 };
		int32_t status = -1;
		char path[512];

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		/* The first attempt fails at once, so the retry waits out the
		 * holdoff. Every pass then parks in a hook read and the region
		 * job sleeps through the next pass, so the holdoff elapses
		 * while that job runs. The stop queued during its sleep sits
		 * behind it when the recovery step runs. The step must yield to
		 * it, and the stop then clears the want. */
		snprintf(path, sizeof(path), "%s/USA.conf", TC_REGION_DIR_STR);
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_REINIT,
					 -EIO);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_REINIT);
		eh_wait_ms(&t, 1400);
		CHECK_UINT(tc_fault_events(0, false), 1);
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_HOOK_READ,
					 500);
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_REGION_SET,
					 1500);
		eh_sleep_ms(20);
		CHECK(udsp_region_load(t.s, path, &region) == UDSP_OK);
		tc_wait_real_ms(&t, 800);
		CHECK(udsp_stop(t.s, &stop) == UDSP_OK);
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_HOOK_READ,
					 0);
		CHECK(eh_job_wait(&t, region.token, 8000, NULL));
		CHECK(eh_job_wait(&t, stop.token, 8000, &status));
		CHECK_UINT((uint32_t)status, 0);
		CHECK_UINT(tc_reinit_count(&t, 0), 0);
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_REGION_SET,
					 0);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a dropped recovery completion self heals");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		atomic_store_explicit(&t.s->recover_ack_drop, true,
				      memory_order_relaxed);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_REINIT);
		/* The first completion is dropped; the slic thread's ack times
		 * out at two seconds of wall clock and it re-posts. */
		tc_wait_real_ms(&t, 5000);
		CHECK_UINT(tc_reinit_count(&t, 0), 1);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_fault_events(0, true), 1);
		tc_down(&t);
	}

	CASE("a lost recovery result holds the line until the next start");
	{
		uint64_t released = 0;
		uint64_t start;
		uint64_t end;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		tc_log_reset();
		/* Every completion is dropped. The slic thread re-posts three
		 * times and gives up, and the tick reads the result as lost
		 * after two of its deadlines. */
		atomic_store_explicit(&t.s->recover_ack_drop, true,
				      memory_order_relaxed);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_REINIT);
		CHECK(tc_wait_recover(&t, 4000));
		start = eh_mono_us();
		end = start + 9500000u;
		while (eh_mono_us() < end) {
			atomic_store_explicit(&t.s->recover_ack_drop, true,
					      memory_order_relaxed);
			if (!released && !tc_recover_jobs(&t))
				released = (eh_mono_us() - start) / 1000u;
			eh_ev_poll(&t);
			/* The release drops the pending completion of the
			 * slic thread, so no re-post follows it. */
			if (released &&
			    (tc_fault_pub(&t, 0) & UDSP_FAULT_PUB_HELD))
				break;
			eh_sleep_ms(1);
		}
		atomic_store_explicit(&t.s->recover_ack_drop, false,
				      memory_order_relaxed);
		/* On the paced fake the tick's lost window (40 s of engine
		 * time) is 4 s of wall clock, ahead of the 8 s re-post budget:
		 * the lost arm releases the line after the second or third
		 * post, never at the budget's end. */
		CHECK(released > 3000u);
		CHECK(released < 7000u);
		CHECK_UINT(tc_reinit_count(&t, 0), 1);
		CHECK(tc_fault_pub(&t, 0) & UDSP_FAULT_PUB_HELD);
		CHECK_UINT(tc_log_hits(UDSP_LOG_WARN, "recovery result lost"),
			   1);
		/* The verdict returns; no attempt may lift the hold. */
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_REINIT);
		tc_wait_real_ms(&t, 2500);
		CHECK_UINT(tc_reinit_count(&t, 0), 1);
		CHECK(tc_fault_pub(&t, 0) & UDSP_FAULT_PUB_HELD);
		CHECK_UINT(tc_fault_events(0, true), 0);
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		tc_down(&t);
	}

	CASE("a failed attempt keeps the ordinary trial deadline");
	{
		struct udsp_slic_monitor m;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		tc_monitor_dcdc_down(&m);
		udsp_slic_fake_monitor_set(t.s->slic, 0, &m);
		udsp_slic_fake_error_set(t.s->slic,
					 UDSP_SLIC_FAKE_CONVERTER_RESTART,
					 -EIO);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_DCDC);
		eh_wait_ms(&t, 1400);
		CHECK_UINT(tc_fault_events(0, false), 1);
		tc_wait_real_ms(&t, 100);
		/* A short from here on: no retry runs, so only the tick's trial
		 * can move the feed. It re-energizes five seconds after the
		 * failed attempt, and the standing fault reopens it. */
		m.lcrrtp = 1u << 1;
		udsp_slic_fake_monitor_set(t.s->slic, 0, &m);
		tc_pace_fast();
		eh_wait_ms(&t, 12000);
		CHECK_UINT(tc_converter_count(&t, 0), 0);
		CHECK(tc_linefeed_count(&t, 0) >= 4);
		tc_down(&t);
	}

	CASE("a close during a recovery joins within its budget");
	{
		uint64_t start;
		uint64_t took_ms;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		/* A stuck vendored call of seven seconds: past the ordinary
		 * thread stop budget, inside the recovery wait. The close waits
		 * for the op. The op returns, sees the quit flag and releases
		 * the line. */
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_REINIT,
					 7000);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_REINIT);
		CHECK(tc_wait_recover(&t, 4000));
		start = eh_mono_us();
		tc_free(&t);
		took_ms = (eh_mono_us() - start) / 1000u;
		CHECK(took_ms > UDSP_STOP_SLIC_MS);
		CHECK(took_ms < UDSP_STOP_RECOVER_MS);
		/* The thread is gone: a second handle opens, which a leaked
		 * one refuses. */
		udsp_tdm_override(NULL);
		CHECK(tc_up_ports(&t, "USA", 1));
		tc_down(&t);
	}

	CASE("a reinit on line 1 leaves line 0 in its call untouched");
	{
		struct tc_stream w;

		if (!tc_up_ports(&t, "USA", 2)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		CHECK(tc_stream_up(&t, &w, 0));
		udsp_slic_fake_fault_set(t.s->slic, 1, UDSP_FAULT_SLIC_REINIT);
		eh_wait_ms(&t, 4000);
		CHECK_UINT(tc_reinit_count(&t, 1), 1);
		CHECK_UINT(tc_reinit_count(&t, 0), 0);
		CHECK_UINT(tc_fault_events(1, false), 1);
		CHECK_UINT(tc_fault_events(1, true), 1);
		CHECK_UINT(tc_lf_confirmed(&t, 1), UDSP_LF_FWD_ACTIVE);
		/* The healthy line kept its feed, its call and a clean word. */
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_linefeed_count(&t, 0), 1);
		CHECK_UINT(tc_fault_events(0, false), 0);
		CHECK_UINT(tc_fault_pub(&t, 0), 0);
		tc_stream_down(&t, &w, 0);
		tc_down(&t);
	}

	CASE("a feed write dropped by a cancelled command wedges nothing");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_THERMAL);
		eh_wait_ms(&t, 1400);
		CHECK_UINT(tc_fault_events(0, false), 1);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		/* The next hook read parks the slic thread past the sync bound
		 * and short of the trial. A feed command placed inside that
		 * sleep times out, and its record is dropped as stale when the
		 * thread reaches it. The engine must not wait on that write. */
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_HOOK_READ,
					 400);
		eh_sleep_ms(20);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) ==
		      UDSP_TRANSIENT);
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_HOOK_READ,
					 0);
		tc_wait_real_ms(&t, 450);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		tc_pace_fast();
		eh_wait_ms(&t, 8000);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_fault_events(0, true), 1);
		tc_down(&t);
	}

	CASE("a lock-out latched between the scan and the attempt stops it");
	{
		struct udsp_slic_monitor m;
		uint64_t end;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		tc_monitor_dcdc_down(&m);
		udsp_slic_fake_monitor_set(t.s->slic, 0, &m);
		/* Every converter read sleeps, so the scan's read and the
		 * attempt's fresh read are 300 ms apart; the lock-out falls
		 * between them. */
		udsp_slic_fake_delay_set(t.s->slic,
					 UDSP_SLIC_FAKE_CONVERTER_READ, 300);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_DCDC);
		end = eh_mono_us() + 4000000u;
		while (tc_converter_read_count(&t, 0) < 1 && eh_mono_us() < end)
			eh_sleep_ms(1);
		CHECK_UINT(tc_converter_read_count(&t, 0), 1);
		m.dcdc_status = 1u << 22;
		udsp_slic_fake_monitor_set(t.s->slic, 0, &m);
		tc_wait_real_ms(&t, 1500);
		CHECK_UINT(tc_converter_count(&t, 0), 0);
		udsp_slic_fake_delay_set(t.s->slic,
					 UDSP_SLIC_FAKE_CONVERTER_READ, 0);
		tc_down(&t);
	}

	CASE("a converter fault without the fresh read op holds the line");
	{
		uint32_t opt = UDSP_SLIC_FAKE_OPT_ALL &
			       ~(uint32_t)UDSP_SLIC_FAKE_OPT_CONVERTER_READ;

		if (!tc_up_opt(&t, opt)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_DCDC);
		eh_wait_ms(&t, 7000);
		CHECK(tc_fault_pub(&t, 0) & UDSP_FAULT_PUB_HELD);
		CHECK_UINT(tc_converter_count(&t, 0), 0);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		CHECK_UINT(tc_linefeed_count(&t, 0), 2);
		tc_down(&t);
	}

	CASE("a device reset read starts no recovery");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_DEVICE_RESET);
		eh_wait_ms(&t, 6000);
		CHECK_UINT(tc_reinit_count(&t, 0), 0);
		CHECK_UINT(tc_converter_count(&t, 0), 0);
		CHECK(tc_fault_pub(&t, 0) & UDSP_FAULT_PUB_HELD);
		tc_down(&t);
	}

	CASE("a held line stays held through a later clean converter read");
	{
		struct udsp_slic_monitor m;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		tc_monitor_dcdc_down(&m);
		m.dcdc_status = 1u << 22;
		udsp_slic_fake_monitor_set(t.s->slic, 0, &m);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_DCDC);
		eh_wait_ms(&t, 7000);
		CHECK(tc_fault_pub(&t, 0) & UDSP_FAULT_PUB_HELD);
		/* The lock-out reads clear again while the fault stands: the
		 * hold lasts until the next start, so no attempt runs. */
		m.dcdc_status = 0;
		udsp_slic_fake_monitor_set(t.s->slic, 0, &m);
		eh_wait_ms(&t, 12000);
		CHECK_UINT(tc_converter_count(&t, 0), 0);
		CHECK(tc_fault_pub(&t, 0) & UDSP_FAULT_PUB_HELD);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		tc_down(&t);
	}

	CASE("a clock hold refuses the converter attempt the ladder selects");
	{
		struct udsp_slic_monitor m;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		/* Clean converter words, so the ladder selects the converter op;
		 * the clock hold the same read set is the only refusal, and a
		 * success would lift it through the start path. */
		tc_monitor_dcdc_down(&m);
		udsp_slic_fake_monitor_set(t.s->slic, 0, &m);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_DCDC |
					 UDSP_FAULT_SLIC_CLOCK);
		eh_wait_ms(&t, 8000);
		CHECK_UINT(tc_converter_count(&t, 0), 0);
		CHECK(tc_fault_pub(&t, 0) & UDSP_FAULT_PUB_HELD);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		tc_down(&t);
	}

	CASE("a clean read inside a caller identity burst keeps its feed");
	{
		struct udsp_cid cid;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		/* The trial's restore keeps failing, so the restore stays due
		 * when the burst takes the on-hook transmission feed. The clean
		 * reads of the fault poll during the burst must not write the
		 * base into it; the burst restores the base itself at its end. */
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_THERMAL);
		eh_wait_ms(&t, 1400);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		udsp_slic_fake_error_repeat_set(t.s->slic,
						UDSP_SLIC_FAKE_LINEFEED_SET,
						-EIO, 1000000);
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		eh_wait_ms(&t, 7000);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		CHECK_UINT(tc_fault_events(0, true), 0);
		CHECK_UINT(tc_linefeed_count(&t, 0), 2);
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_LINEFEED_SET,
					 0);
		tc_cid_fill(&cid, "12345678901234567890123456789012",
			    "abcdefghijklmnopqrstuvwxyzabcdef");
		CHECK(udsp_cid_send(t.s, 0, UDSP_CID_TYPE1, &cid) == UDSP_OK);
		eh_wait_ms(&t, 700);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_OHT);
		CHECK_UINT(eh_ev_count(UDSP_EV_CID_SENT), 0);
		eh_wait_ms(&t, 6000);
		CHECK_UINT(eh_ev_count(UDSP_EV_CID_SENT), 1);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		/* The burst's own two writes and none into it. */
		CHECK_UINT(tc_linefeed_count(&t, 0), 4);
		CHECK_UINT(tc_fault_events(0, true), 1);
		tc_down(&t);
	}

	CASE("a held ring stop whose open write fails ends open");
	{
		struct udsp_ring ring;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		tc_ring_fill(&ring, 2000, 4000, 30000);
		CHECK(udsp_ring_start(t.s, 0, &ring) == UDSP_OK);
		eh_wait_ms(&t, 200);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_CLOCK);
		eh_wait_ms(&t, 1400);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		CHECK_UINT(tc_linefeed_count(&t, 0), 2);
		/* The vendor ring stop leaves the active feed. The OPEN after
		 * it fails once, so the confirmed feed reads active and the
		 * hold re-emits until the part is open. */
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_LINEFEED_SET,
					 -EIO);
		/* The stop's ack carries the failed write's errno, which maps
		 * to the transient answer; the relay still opened. */
		CHECK(udsp_ring_stop(t.s, 0) == UDSP_TRANSIENT);
		eh_wait_ms(&t, 1000);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		CHECK_UINT(tc_linefeed_count(&t, 0), 3);
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		tc_down(&t);
	}

	CASE("a reinit read after a shorted converter read runs the reinit");
	{
		struct udsp_slic_monitor m;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		tc_monitor_dcdc_down(&m);
		m.lcrrtp = 1u << 1;
		udsp_slic_fake_monitor_set(t.s->slic, 0, &m);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_DCDC);
		eh_wait_ms(&t, 2000);
		CHECK_UINT(tc_converter_count(&t, 0), 0);
		/* The converter came up on its own; the reinit verdict alone
		 * remains, and the short seen before it does not refuse it. */
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_REINIT);
		eh_wait_ms(&t, 5000);
		CHECK_UINT(tc_reinit_count(&t, 0), 1);
		CHECK_UINT(tc_converter_count(&t, 0), 0);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		tc_down(&t);
	}

	CASE("a failed open on the recovery begin skips the vendor op");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		/* Every open write fails, so the line never reaches the open
		 * feed and the begin's ack carries the errno. No op may run
		 * against a powered line; the attempts count down to the
		 * hold. */
		udsp_slic_fake_error_repeat_set(t.s->slic,
						UDSP_SLIC_FAKE_LINEFEED_SET,
						-EIO, 1000000);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_REINIT);
		eh_wait_ms(&t, 20000);
		CHECK_UINT(tc_reinit_count(&t, 0), 0);
		CHECK(tc_fault_pub(&t, 0) & UDSP_FAULT_PUB_HELD);
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_LINEFEED_SET,
					 0);
		tc_down(&t);
	}

	CASE("a standing converter fault reads its words once per holdoff");
	{
		struct udsp_slic_monitor m;
		unsigned reads;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		tc_monitor_dcdc_down(&m);
		m.lcrrtp = 1u << 1;
		udsp_slic_fake_monitor_set(t.s->slic, 0, &m);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_DCDC);
		eh_wait_ms(&t, 1400);
		reads = tc_converter_read_count(&t, 0);
		CHECK_UINT(reads, 1);
		/* Twelve engine seconds of a standing short: the edge read
		 * above, then one read per five second holdoff. */
		eh_wait_ms(&t, 12000);
		CHECK(tc_converter_read_count(&t, 0) - reads <= 3);
		CHECK(tc_converter_read_count(&t, 0) - reads >= 2);
		tc_down(&t);
	}

	CASE("a stop queued behind a pending completion runs first");
	{
		struct udsp_job stop = { 0 };
		int32_t status = -1;
		uint64_t start;
		uint64_t end;
		bool done = false;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		atomic_store_explicit(&t.s->recover_ack_drop, true,
				      memory_order_relaxed);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_REINIT);
		CHECK(tc_wait_recover(&t, 4000));
		/* The first completion is in its dropped ack wait. The stop
		 * queued now must run on the pass after it, not after a second
		 * dropped wait. */
		tc_wait_real_ms(&t, 400);
		start = eh_mono_us();
		CHECK(udsp_stop(t.s, &stop) == UDSP_OK);
		end = start + 6000000u;
		while (!done && eh_mono_us() < end) {
			atomic_store_explicit(&t.s->recover_ack_drop, true,
					      memory_order_relaxed);
			done = eh_job_wait(&t, stop.token, 1, &status);
		}
		CHECK(done);
		CHECK_UINT((uint32_t)status, 0);
		CHECK((eh_mono_us() - start) / 1000u < 2800u);
		atomic_store_explicit(&t.s->recover_ack_drop, false,
				      memory_order_relaxed);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("a failed open write on a held line still confirms open");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		/* The hold's open write fails once, so the feed reads active
		 * under the hold; the tick must re-emit open until it
		 * confirms. */
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_LINEFEED_SET,
					 -EIO);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_CLOCK);
		eh_wait_ms(&t, 4000);
		CHECK(tc_fault_pub(&t, 0) & UDSP_FAULT_PUB_HELD);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		tc_down(&t);
	}
}

/* Two ports with one armed init error: the start leaves line 0 dead and
 * line 1 up, and the arm is spent, so the next init succeeds. */
static bool tc_up_dead_line(struct eh *t)
{
	struct udsp_config cfg;

	tc_backend_reset();
	udsp_slic_fake_reset();
	udsp_tdm_override(&tc_ops);
	tc_cfg(&cfg, "tc", "USA");
	if (!tc_open_handle(t, &cfg))
		return false;
	udsp_slic_fake_error_repeat_set(t->s->slic, UDSP_SLIC_FAKE_INIT,
					-EIO, 1);

	return tc_start(t);
}

/* Two ports on a handle opened with lines_start_down: the start leaves every
 * line open, and the consumer raises each one. */
static bool tc_up_down(struct eh *t, unsigned ports, bool dead_line)
{
	struct udsp_config cfg;

	tc_backend_reset();
	udsp_slic_fake_reset();
	udsp_tdm_override(&tc_ops);
	tc_cfg(&cfg, "tc", "USA");
	cfg.populated_ports = (uint8_t)ports;
	cfg.lines_start_down = true;
	if (!tc_open_handle(t, &cfg))
		return false;
	if (dead_line)
		udsp_slic_fake_error_repeat_set(t->s->slic, UDSP_SLIC_FAKE_INIT,
						-EIO, 1);

	return tc_start(t);
}

static void case_start_down(void)
{
	struct eh t;

	CASE("a handle opened with lines_start_down brings every line up open");
	{
		struct udsp_slic_fake_chan_state st;

		if (!tc_up_down(&t, 2, false)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 100);
		/* One open write per line at LINES_UP: the raise is the
		 * consumer's, and the engine still confirms the state init
		 * left. */
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		CHECK_UINT(tc_lf_confirmed(&t, 1), UDSP_LF_OPEN);
		CHECK_UINT(tc_linefeed_count(&t, 0), 1);
		CHECK_UINT(tc_linefeed_count(&t, 1), 1);
		CHECK(tc_state(&t, 0, &st));
		CHECK_UINT(st.linefeed, UDSP_LF_OPEN);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) == UDSP_OK);
		eh_wait_ms(&t, 100);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_lf_confirmed(&t, 1), UDSP_LF_OPEN);
		/* A stop and a start keep the last request per line. */
		CHECK(tc_stop(&t));
		CHECK(tc_start(&t));
		eh_wait_ms(&t, 100);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_lf_confirmed(&t, 1), UDSP_LF_OPEN);
		tc_down(&t);
	}

	CASE("a recovery DONE keeps a line the consumer holds down open");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_DOWN) == UDSP_OK);
		eh_wait_ms(&t, 100);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_REINIT);
		eh_wait_ms(&t, 4000);
		CHECK_UINT(tc_reinit_count(&t, 0), 1);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		CHECK_UINT(tc_fault_events(0, false), 1);
		/* No feed came back, so no clear: the consumer's raise brings
		 * both. */
		CHECK_UINT(tc_fault_events(0, true), 0);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) == UDSP_OK);
		eh_wait_ms(&t, 1500);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_fault_events(0, true), 1);
		tc_down(&t);
	}

	CASE("a dead line's return keeps a line the handle started down open");
	{
		uint64_t t0;

		setenv("UDSP_SLIC_DEAD_RETRY_MS", "200", 1);
		if (!tc_up_down(&t, 2, true)) {
			CHECK(false);
			unsetenv("UDSP_SLIC_DEAD_RETRY_MS");
			return;
		}
		t0 = eh_mono_us();
		while (!tc_line_events(UDSP_LINE_ACTIVE) &&
		       eh_mono_us() < t0 + 4000000u) {
			eh_ev_poll(&t);
			eh_sleep_ms(10);
		}
		/* The return event says the line is back, whatever feed the
		 * consumer holds it at. */
		CHECK_UINT(tc_line_events(UDSP_LINE_ACTIVE), 1);
		CHECK(tc_log_hits(UDSP_LOG_INFO, "back after init retry") == 1);
		eh_wait_ms(&t, 300);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		CHECK_UINT(tc_lf_confirmed(&t, 1), UDSP_LF_OPEN);
		CHECK_UINT(tc_linefeed_count(&t, 0), 1);
		CHECK_UINT(tc_linefeed_count(&t, 1), 1);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) == UDSP_OK);
		eh_wait_ms(&t, 100);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		tc_down(&t);
		unsetenv("UDSP_SLIC_DEAD_RETRY_MS");
	}

	CASE("a caller identity sequence ends on the base feed set during it");
	{
		struct udsp_cid cid;

		CHECK(tc_region_fast());
		if (!tc_up_ports(&t, "FST", 1)) {
			CHECK(false);
			return;
		}
		tc_cid_fill(&cid, "5551234", "TEST CALLER");
		eh_wait_ms(&t, 100);
		CHECK(udsp_cid_send(t.s, 0, UDSP_CID_TYPE1, &cid) == UDSP_OK);
		eh_wait_ms(&t, 150);
		/* The sequence owns the line, so the request is deferred to
		 * its end, which restores the base feed it moved and not the
		 * one the sequence armed with. */
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_DOWN) == UDSP_OK);
		eh_wait_ms(&t, 1500);
		CHECK_UINT(eh_ev_count(UDSP_EV_CID_SENT), 1);
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_OPEN);
		tc_down(&t);
		tc_region_fast_drop();
	}
}

static void case_dead_retry(void)
{
	struct eh t;

	CASE("an init that fails at start leaves the line dead, sibling up");
	{
		const struct udsp_event *ev;

		if (!tc_up_dead_line(&t)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 100);
		CHECK_UINT(tc_line_down_count(), 1);
		ev = eh_ev_find(UDSP_EV_LINE, 0);
		CHECK(ev != NULL && ev->line == 0);
		CHECK(tc_log_hits(UDSP_LOG_ERR,
				  "dead after a failed init") == 1);
		CHECK_UINT(tc_lf_confirmed(&t, 1), UDSP_LF_FWD_ACTIVE);
		/* The published state of the dead line is fatal; the sibling
		 * serves. */
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) == UDSP_FATAL);
		CHECK(udsp_line_state(t.s, 1, UDSP_LINE_OHT) == UDSP_OK);
		/* The production cadence is checked two ways: the constant is
		 * the shipped value, and with no override the handle holds it
		 * as the retry cadence of the dead line. */
		CHECK_UINT(SLIC_DEAD_RETRY_MS, 10000);
		CHECK_UINT(t.s->dead_retry_ms, SLIC_DEAD_RETRY_MS);
		eh_ev_poll(&t);
		CHECK_UINT(tc_init_count(&t, 0), 0);
		CHECK_UINT(tc_line_events(UDSP_LINE_ACTIVE), 0);
		tc_down(&t);
	}

	/* The remaining cases shrink the retry cadence through the env override,
	 * read once at open, so a deadline elapses in milliseconds. */
	setenv("UDSP_SLIC_DEAD_RETRY_MS", "200", 1);

	CASE("the retry brings the dead line back inside a bounded window");
	{
		uint64_t t0;
		uint64_t elapsed_ms = 0;
		unsigned hooks;

		if (!tc_up_dead_line(&t)) {
			CHECK(false);
			return;
		}
		t0 = eh_mono_us();
		/* The shortened cadence brings the line back well under a
		 * second. The first case of this function checks the production
		 * cadence. */
		while (!tc_line_events(UDSP_LINE_ACTIVE) &&
		       eh_mono_us() < t0 + 4000000u) {
			eh_ev_poll(&t);
			eh_sleep_ms(10);
		}
		elapsed_ms = (eh_mono_us() - t0) / 1000u;
		CHECK_UINT(tc_line_events(UDSP_LINE_ACTIVE), 1);
		CHECK(elapsed_ms < 4000u);
		CHECK_UINT(tc_line_down_count(), 1);
		CHECK_UINT(tc_init_count(&t, 0), 1);
		CHECK(tc_log_hits(UDSP_LOG_INFO, "back after init retry") == 1);
		eh_wait_ms(&t, 300);
		/* The LINES_UP mask carried the returned line alone: the
		 * sibling kept its one start-time feed write. */
		CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_ACTIVE);
		CHECK_UINT(tc_linefeed_count(&t, 0), 1);
		CHECK_UINT(tc_linefeed_count(&t, 1), 1);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) == UDSP_OK);
		/* The helper programmed the region on the return, and the line
		 * is live again: a region load reprograms it too. */
		CHECK_UINT(tc_region_count(&t, 0), 1);
		{
			struct udsp_job region = { 0 };
			char path[512];

			snprintf(path, sizeof(path), "%s/USA.conf",
				 TC_REGION_DIR_STR);
			CHECK(udsp_region_load(t.s, path, &region) == UDSP_OK);
			CHECK(eh_job_wait(&t, region.token, 8000, NULL));
			CHECK_UINT(tc_region_count(&t, 0), 2);
		}
		/* Hook sampling resumed on the returned line. */
		hooks = eh_ev_count(UDSP_EV_HOOK);
		tc_hook_set(&t, true);
		eh_wait_ms(&t, 500);
		CHECK(eh_ev_count(UDSP_EV_HOOK) > hooks);
		CHECK_UINT(tc_hook_last(), UDSP_HOOK_SEIZE);
		tc_hook_set(&t, false);
		eh_wait_ms(&t, 1500);
		/* The fault scan resumed on the returned line. */
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_THERMAL);
		eh_wait_ms(&t, 1400);
		CHECK_UINT(tc_fault_events(0, false), 1);
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		tc_down(&t);
	}

	CASE("the returned line samples at tick rate: the return enters burst");
	{
		int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		uint64_t t0;
		unsigned before, after;

		CHECK(efd >= 0);
		/* A longer cadence so the retry fires after the start burst
		 * expires: the line idles when it returns, and only the
		 * return's own burst can raise the sample rate again. */
		setenv("UDSP_SLIC_DEAD_RETRY_MS", "2500", 1);
		udsp_slic_irq_override(efd, POLLIN);
		if (!tc_up_dead_line(&t)) {
			CHECK(false);
			udsp_slic_irq_override(-1, 0);
			close(efd);
			return;
		}
		eh_wait_ms(&t, 2100);
		CHECK_UINT(t.s->slic_mode, SLIC_MODE_IDLE);
		t0 = eh_mono_us();
		while (!tc_line_events(UDSP_LINE_ACTIVE) &&
		       eh_mono_us() < t0 + 4000000u) {
			eh_ev_poll(&t);
			eh_sleep_ms(10);
		}
		CHECK_UINT(tc_line_events(UDSP_LINE_ACTIVE), 1);
		/* The return raised a fresh burst, so the mode is burst and the
		 * returned line samples at tick rate. Without the burst the
		 * thread stays idle and samples once per SLIC_IDLE_SAMPLE_MS. */
		CHECK_UINT(t.s->slic_mode, SLIC_MODE_BURST);
		before = tc_hook_reads(&t, 0);
		eh_wait_ms(&t, 500);
		after = tc_hook_reads(&t, 0);
		CHECK(after - before >= 10);
		tc_down(&t);
		udsp_slic_irq_override(-1, 0);
		close(efd);
		setenv("UDSP_SLIC_DEAD_RETRY_MS", "200", 1);
	}

	CASE("a failed attempt is silent and the next deadline succeeds");
	{
		uint64_t t0;
		uint64_t elapsed_ms = 0;

		if (!tc_up_dead_line(&t)) {
			CHECK(false);
			return;
		}
		/* One more armed error: the first retry fails, changes nothing
		 * and logs nothing; the second succeeds a deadline later. */
		udsp_slic_fake_error_repeat_set(t.s->slic, UDSP_SLIC_FAKE_INIT,
						-EIO, 1);
		t0 = eh_mono_us();
		while (!tc_line_events(UDSP_LINE_ACTIVE) &&
		       eh_mono_us() < t0 + 5000000u) {
			eh_ev_poll(&t);
			eh_sleep_ms(10);
		}
		elapsed_ms = (eh_mono_us() - t0) / 1000u;
		CHECK_UINT(tc_line_events(UDSP_LINE_ACTIVE), 1);
		CHECK(elapsed_ms < 5000u);
		CHECK_UINT(tc_init_count(&t, 0), 1);
		CHECK_UINT(tc_line_down_count(), 1);
		CHECK(tc_log_hits(UDSP_LOG_ERR,
				  "dead after a failed init") == 1);
		CHECK(tc_log_hits(UDSP_LOG_INFO, "back after init retry") == 1);
		tc_down(&t);
	}

	CASE("a bring-up write that fails keeps the line dead");
	{
		uint64_t t0;

		if (!tc_up_dead_line(&t)) {
			CHECK(false);
			return;
		}
		/* The retry's init succeeds, but the LINES_UP feed write fails
		 * on the bus, so the ack carries a negative status. That is a
		 * failed attempt, not a return: the line dies again with a
		 * DOWN, and the next deadline retries. */
		udsp_slic_fake_error_repeat_set(t.s->slic,
						UDSP_SLIC_FAKE_LINEFEED_SET,
						-EIO, 1);
		t0 = eh_mono_us();
		while (tc_line_down_count() < 2 && eh_mono_us() < t0 + 5000000u) {
			eh_ev_poll(&t);
			eh_sleep_ms(10);
		}
		CHECK_UINT(tc_init_count(&t, 0), 1);
		/* No return was declared: a second DOWN and no ACTIVE. */
		CHECK_UINT(tc_line_events(UDSP_LINE_ACTIVE), 0);
		CHECK_UINT(tc_line_down_count(), 2);
		CHECK(tc_log_hits(UDSP_LOG_ERR,
				  "dead after a failed bring-up write") == 1);
		CHECK(tc_log_hits(UDSP_LOG_INFO, "back after init retry") == 0);
		/* The error is spent; a later deadline returns the line. */
		t0 = eh_mono_us();
		while (!tc_line_events(UDSP_LINE_ACTIVE) &&
		       eh_mono_us() < t0 + 5000000u) {
			eh_ev_poll(&t);
			eh_sleep_ms(10);
		}
		CHECK_UINT(tc_line_events(UDSP_LINE_ACTIVE), 1);
		CHECK_UINT(tc_init_count(&t, 0), 2);
		tc_down(&t);
	}

	CASE("a stop queued during the dead period wins over the retry");
	{
		struct udsp_job region = { 0 };
		struct udsp_job stop = { 0 };
		int32_t status = -1;
		char path[512];

		/* This case needs the deadline to fall while a job parks the
		 * thread, so it runs a longer cadence than the fast cases. */
		setenv("UDSP_SLIC_DEAD_RETRY_MS", "1500", 1);
		if (!tc_up_dead_line(&t)) {
			CHECK(false);
			return;
		}
		/* A region job parks the thread across the 1500 ms retry
		 * deadline and the stop sits queued behind it. The pass after
		 * the job must yield to the queued stop. The delayed region op
		 * spans from the death to about 2500 ms after it, so the
		 * deadline falls inside it with a wide margin. */
		snprintf(path, sizeof(path), "%s/USA.conf", TC_REGION_DIR_STR);
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_REGION_SET,
					 2500);
		CHECK(udsp_region_load(t.s, path, &region) == UDSP_OK);
		tc_wait_real_ms(&t, 100);
		CHECK(udsp_stop(t.s, &stop) == UDSP_OK);
		CHECK(eh_job_wait(&t, region.token, 8000, NULL));
		CHECK(eh_job_wait(&t, stop.token, 8000, &status));
		CHECK_UINT((uint32_t)status, 0);
		CHECK_UINT(tc_init_count(&t, 0), 0);
		CHECK_UINT(tc_line_events(UDSP_LINE_ACTIVE), 0);
		/* After the stop no retry runs at all: the deadline is long
		 * past and the queue is empty, yet the line stays dead. */
		tc_wait_real_ms(&t, 1500);
		CHECK_UINT(tc_init_count(&t, 0), 0);
		CHECK_UINT(tc_line_events(UDSP_LINE_ACTIVE), 0);
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_REGION_SET,
					 0);
		tc_free(&t);
		udsp_tdm_override(NULL);
		setenv("UDSP_SLIC_DEAD_RETRY_MS", "200", 1);
	}

	CASE("a close during a retry joins within the recovery budget");
	{
		uint64_t start;
		uint64_t took_ms;

		if (!tc_up_dead_line(&t)) {
			CHECK(false);
			return;
		}
		/* The retry's init parks the thread for seven seconds and the
		 * close comes inside it: past the ordinary thread stop budget,
		 * inside the recovery wait. The op returns, sees the quit flag
		 * and declares no return, so exactly one death stands. */
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_INIT, 7000);
		CHECK(tc_wait_recover(&t, 15000));
		start = eh_mono_us();
		udsp_close(t.s);
		took_ms = (eh_mono_us() - start) / 1000u;
		t.s = NULL;
		CHECK(took_ms > UDSP_STOP_SLIC_MS);
		CHECK(took_ms < UDSP_STOP_RECOVER_MS);
		eh_ev_poll(&t);
		CHECK_UINT(tc_line_events(UDSP_LINE_ACTIVE), 0);
		CHECK_UINT(tc_line_down_count(), 1);
		tc_free(&t);
		/* The thread is gone: a second handle opens, which a leaked
		 * one refuses. */
		udsp_tdm_override(NULL);
		CHECK(tc_up_ports(&t, "USA", 1));
		tc_down(&t);
	}

	CASE("two dead lines both return, one per pass, neither starved");
	{
		uint64_t end;

		/* The setup bound starts before open, so line 0 cannot return
		 * and consume the sibling's hook failures inside that bound. */
		setenv("UDSP_SLIC_DEAD_RETRY_MS", "2500", 1);
		end = eh_mono_us() + 1500000u;
		if (!tc_up_dead_line(&t)) {
			CHECK(false);
			setenv("UDSP_SLIC_DEAD_RETRY_MS", "200", 1);
			return;
		}
		eh_wait_ms(&t, 40);
		if (!tc_hook_fail_wait(&t, UDSP_HOOK_FAIL_LIMIT, 2, end))
			goto dead_pair_out;
		/* Both dead lines return: the scan reaches the second line, so
		 * no line is left dead while the other recovers. Only a slow
		 * failing retry exercises the arm after the completion, and the
		 * fake cannot produce one. */
		end = eh_mono_us() + 6000000u;
		while (tc_line_events(UDSP_LINE_ACTIVE) < 2 &&
		       eh_mono_us() < end) {
			eh_ev_poll(&t);
			eh_sleep_ms(10);
		}
		CHECK_UINT(tc_line_events(UDSP_LINE_ACTIVE), 2);
		/* The armed start init counted nothing on line 0; the sibling
		 * carries its start init plus the retry. */
		CHECK_UINT(tc_init_count(&t, 0), 1);
		CHECK_UINT(tc_init_count(&t, 1), 2);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) == UDSP_OK);
		CHECK(udsp_line_state(t.s, 1, UDSP_LINE_ACTIVE) == UDSP_OK);
	dead_pair_out:
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_HOOK_READ,
					 0);
		tc_down(&t);
		setenv("UDSP_SLIC_DEAD_RETRY_MS", "200", 1);
	}

	CASE("a ring session ends when its line dies; both lines return");
	{
		struct udsp_ring ring;
		struct udsp_slic_fake_chan_state st;
		const struct udsp_event *end_ev;
		uint64_t end;

		if (!tc_up_ports(&t, "USA", 2)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		/* A ring long enough to own line 0 across the deaths below. */
		tc_ring_fill(&ring, 2000, 4000, 600000);
		CHECK(udsp_ring_start(t.s, 0, &ring) == UDSP_OK);
		eh_wait_ms(&t, 200);
		if (!tc_hook_fail_wait(&t, 2u * UDSP_HOOK_FAIL_LIMIT, 2,
				       eh_mono_us() + 5000000u))
			goto dead_ring_out;
		/* The tick ends the session on the death, so nothing owns the
		 * line, and every op on it keeps the dead line's answer. */
		eh_wait_ms(&t, 50);
		end_ev = eh_ev_find(UDSP_EV_RING_END, 0);
		CHECK(end_ev != NULL);
		if (end_ev)
			CHECK_UINT(end_ev->u.line.state, LINE_RING_DEAD);
		CHECK(!atomic_load_explicit(&t.s->chan[0].owned,
					    memory_order_acquire));
		CHECK(udsp_ring_stop(t.s, 0) == UDSP_FATAL);
		/* Both lines return: the retry then reads line 0 as free. */
		end = eh_mono_us() + 6000000u;
		while (tc_line_events(UDSP_LINE_ACTIVE) < 2 &&
		       eh_mono_us() < end) {
			eh_ev_poll(&t);
			eh_sleep_ms(10);
		}
		CHECK_UINT(tc_line_events(UDSP_LINE_ACTIVE), 2);
		CHECK_UINT(tc_init_count(&t, 0), 2);
		CHECK_UINT(tc_init_count(&t, 1), 2);
		/* The session did not resume with the line. */
		eh_wait_ms(&t, 300);
		CHECK(udsp_slic_fake_chan_state_get(t.s->slic, 0, &st));
		CHECK_UINT(st.ring_start_count, 1);
		CHECK_UINT(eh_ev_count(UDSP_EV_RING_END), 1);
	dead_ring_out:
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_HOOK_READ,
					 0);
		tc_down(&t);
	}

	CASE("no retry on a dead line while its stream stays active; the stop releases it");
	{
		struct tc_stream w;
		uint64_t end;

		if (!tc_up_ports(&t, "USA", 2)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		if (!tc_stream_up(&t, &w, 0)) {
			CHECK(false);
			tc_down(&t);
			return;
		}
		eh_wait_ms(&t, 100);
		if (!tc_hook_fail_wait(&t, 2u * UDSP_HOOK_FAIL_LIMIT, 2,
				       eh_mono_us() + 5000000u))
			goto dead_stream_out;
		/* The free sibling returns; the line in a call gets no vendor
		 * init, because the PCM audio can outlive the control bus. */
		end = eh_mono_us() + 6000000u;
		while (!tc_line_events(UDSP_LINE_ACTIVE) &&
		       eh_mono_us() < end) {
			eh_ev_poll(&t);
			eh_sleep_ms(10);
		}
		CHECK_UINT(tc_line_events(UDSP_LINE_ACTIVE), 1);
		CHECK_UINT(tc_init_count(&t, 1), 2);
		CHECK_UINT(tc_init_count(&t, 0), 1);
		tc_wait_real_ms(&t, 1500);
		CHECK_UINT(tc_line_events(UDSP_LINE_ACTIVE), 1);
		CHECK_UINT(tc_init_count(&t, 0), 1);
		/* The stop is admitted on the dead line, and the released line
		 * returns at the next cadence. */
		CHECK(tc_stream_down(&t, &w, 0));
		end = eh_mono_us() + 6000000u;
		while (tc_line_events(UDSP_LINE_ACTIVE) < 2 &&
		       eh_mono_us() < end) {
			eh_ev_poll(&t);
			eh_sleep_ms(10);
		}
		CHECK_UINT(tc_line_events(UDSP_LINE_ACTIVE), 2);
		CHECK_UINT(tc_init_count(&t, 0), 2);
	dead_stream_out:
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_HOOK_READ,
					 0);
		tc_down(&t);
	}

	CASE("a caller identity burst ends when its line dies");
	{
		struct udsp_cid cid;
		const struct udsp_event *sent;
		uint64_t end;

		if (!tc_up_ports(&t, "USA", 2)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		tc_cid_fill(&cid, "5551234", "TEST CALLER");
		CHECK(udsp_cid_send(t.s, 0, UDSP_CID_TYPE1, &cid) == UDSP_OK);
		/* Inside the burst: the type 1 sequence runs for a second. */
		eh_wait_ms(&t, 200);
		if (!tc_hook_fail_wait(&t, 2u * UDSP_HOOK_FAIL_LIMIT, 2,
				       eh_mono_us() + 5000000u))
			goto dead_cid_out;
		eh_wait_ms(&t, 50);
		sent = eh_ev_find(UDSP_EV_CID_SENT, 0);
		CHECK(sent != NULL);
		if (sent)
			CHECK(sent->u.cid_sent.status < 0);
		CHECK(!atomic_load_explicit(&t.s->chan[0].owned,
					    memory_order_acquire));
		end = eh_mono_us() + 6000000u;
		while (tc_line_events(UDSP_LINE_ACTIVE) < 2 &&
		       eh_mono_us() < end) {
			eh_ev_poll(&t);
			eh_sleep_ms(10);
		}
		CHECK_UINT(tc_line_events(UDSP_LINE_ACTIVE), 2);
		CHECK_UINT(tc_init_count(&t, 0), 2);
	dead_cid_out:
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_HOOK_READ,
					 0);
		tc_down(&t);
	}

	CASE("a ring and a stream together: the ring ends at death, the stream holds the retry");
	{
		struct udsp_ring ring;
		struct tc_stream w;
		uint64_t end;

		if (!tc_up_ports(&t, "USA", 2)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 40);
		if (!tc_stream_up(&t, &w, 0)) {
			CHECK(false);
			tc_down(&t);
			return;
		}
		tc_ring_fill(&ring, 2000, 4000, 600000);
		CHECK(udsp_ring_start(t.s, 0, &ring) == UDSP_OK);
		eh_wait_ms(&t, 200);
		if (!tc_hook_fail_wait(&t, 2u * UDSP_HOOK_FAIL_LIMIT, 2,
				       eh_mono_us() + 5000000u))
			goto dead_both_out;
		eh_wait_ms(&t, 50);
		CHECK_UINT(eh_ev_count(UDSP_EV_RING_END), 1);
		CHECK(!atomic_load_explicit(&t.s->chan[0].owned,
					    memory_order_acquire));
		/* The ring's end does not bypass the stream half of the gate. */
		end = eh_mono_us() + 6000000u;
		while (!tc_line_events(UDSP_LINE_ACTIVE) &&
		       eh_mono_us() < end) {
			eh_ev_poll(&t);
			eh_sleep_ms(10);
		}
		CHECK_UINT(tc_line_events(UDSP_LINE_ACTIVE), 1);
		CHECK_UINT(tc_init_count(&t, 1), 2);
		tc_wait_real_ms(&t, 1500);
		CHECK_UINT(tc_init_count(&t, 0), 1);
		CHECK(tc_stream_down(&t, &w, 0));
		end = eh_mono_us() + 6000000u;
		while (tc_line_events(UDSP_LINE_ACTIVE) < 2 &&
		       eh_mono_us() < end) {
			eh_ev_poll(&t);
			eh_sleep_ms(10);
		}
		CHECK_UINT(tc_line_events(UDSP_LINE_ACTIVE), 2);
		CHECK_UINT(tc_init_count(&t, 0), 2);
	dead_both_out:
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_HOOK_READ,
					 0);
		tc_down(&t);
	}

	CASE("an all-dead start on a started handle disarms the retry");
	{
		struct udsp_job job = { 0 };
		int32_t status = 0;

		if (!tc_up_ports(&t, "USA", 2)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		udsp_slic_fake_error_repeat_set(t.s->slic, UDSP_SLIC_FAKE_INIT,
						-EIO, 2);
		CHECK(udsp_start(t.s, &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 5000, &status));
		CHECK(status == UDSP_FATAL);
		CHECK_UINT(tc_line_down_count(), 2);
		/* The arms are spent, so a retry would succeed and betray
		 * itself; started is false again, so none may run. The wait
		 * spans several shortened cadences. */
		tc_wait_real_ms(&t, 2000);
		CHECK_UINT(tc_line_events(UDSP_LINE_ACTIVE), 0);
		CHECK_UINT(tc_init_count(&t, 0), 1);
		CHECK_UINT(tc_init_count(&t, 1), 1);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("an all-dead start still fails and no retry runs after it");
	{
		struct udsp_config cfg;
		struct udsp_job job = { 0 };
		int32_t status = 0;

		tc_backend_reset();
		udsp_slic_fake_reset();
		udsp_slic_fake_channels_set(1);
		udsp_tdm_override(&tc_ops);
		tc_cfg(&cfg, "tc", "USA");
		cfg.populated_ports = 1;
		CHECK(tc_open_handle(&t, &cfg));
		if (!t.s) {
			CHECK(false);
			return;
		}
		/* One armed error: a retry, were one to run, would succeed
		 * and betray itself with the count and the ACTIVE event. */
		udsp_slic_fake_error_repeat_set(t.s->slic, UDSP_SLIC_FAKE_INIT,
						-EIO, 1);
		CHECK(udsp_start(t.s, &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 5000, &status));
		CHECK(status == UDSP_FATAL);
		tc_wait_real_ms(&t, 2000);
		CHECK_UINT(tc_init_count(&t, 0), 0);
		CHECK_UINT(tc_line_events(UDSP_LINE_ACTIVE), 0);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) == UDSP_FATAL);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	unsetenv("UDSP_SLIC_DEAD_RETRY_MS");
}

/* Reads the line monitor in a tight loop for a while. The ThreadSanitizer
 * leg then sees the getter race the slic thread's publish and the engine's
 * fault word. */
struct tc_mon_reader {
	struct eh *t;
	_Atomic bool stop;
};

static void *tc_mon_read_loop(void *arg)
{
	struct tc_mon_reader *r = arg;
	struct udsp_monitor mon;

	while (!atomic_load_explicit(&r->stop, memory_order_acquire))
		udsp_line_monitor(r->t->s, 0, &mon);

	return NULL;
}

static void case_monitor(void)
{
	struct udsp_monitor mon;
	struct eh t;

	CASE("the monitor answers unsupported without the op");
	{
		if (!tc_up_opt(&t, UDSP_SLIC_FAKE_OPT_ALL &
				   ~(uint32_t)UDSP_SLIC_FAKE_OPT_MONITOR)) {
			CHECK(false);
			return;
		}
		CHECK(udsp_line_monitor(t.s, 0, &mon) == UDSP_UNSUPPORTED);
		tc_down(&t);
	}

	CASE("the monitor answers valid zero before the first sample");
	{
		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		/* The edge sample answers no data with no earlier reading to
		 * republish, so the fault word stands with no sample behind. */
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_MONITOR_READ,
					 -ENODATA);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_THERMAL);
		eh_wait_ms(&t, 1400);
		memset(&mon, 0xa5, sizeof(mon));
		CHECK(udsp_line_monitor(t.s, 0, &mon) == UDSP_OK);
		CHECK_UINT(mon.valid, 0);
		CHECK_UINT(mon.stale, 0);
		CHECK_UINT(mon.stamp_ms, 0);
		CHECK_UINT(mon.fault_bits, UDSP_FAULT_SLIC_THERMAL);
		CHECK_UINT(mon.fault_open, 1);
		CHECK_UINT(mon.fault_held, 0);
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		tc_down(&t);
	}

	CASE("a periodic sample lands with a bracketed stamp");
	{
		struct udsp_slic_monitor m;
		uint32_t before;
		uint32_t after;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		memset(&m, 0, sizeof(m));
		m.vbat_mv = -48000;
		m.p_hvic_mw = 120;
		udsp_slic_fake_monitor_set(t.s->slic, 0, &m);
		before = (uint32_t)(udsp_mono_us() / 1000u);
		/* The per-tick monitor cadence is a thousand passes. */
		eh_wait_ms(&t, 11000);
		after = (uint32_t)(udsp_mono_us() / 1000u);
		CHECK(udsp_line_monitor(t.s, 0, &mon) == UDSP_OK);
		CHECK_UINT(mon.valid, 1);
		CHECK_UINT(mon.stale, 0);
		CHECK(mon.stamp_ms - before <= after - before);
		CHECK(mon.vbat_mv == -48000);
		CHECK_UINT(mon.p_hvic_mw, 120);
		tc_down(&t);
	}

	CASE("the monitor decodes each status bit on its own");
	{
		struct udsp_slic_monitor m;
		uint32_t fault = UDSP_FAULT_SLIC_THERMAL;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		for (unsigned i = 0; i < 7; i++) {
			memset(&m, 0, sizeof(m));
			if (i == 0)
				m.pd_dcdc = 1u << 20;
			if (i == 1)
				m.dcdc_status = 1u << 22;
			if (i == 2)
				m.linefeed = 0x22u;
			if (i == 3)
				m.lcrrtp = 1u << 3;
			if (i == 4)
				m.lcrrtp = 1u << 1;
			if (i == 5)
				m.lcrrtp = 1u << 0;
			if (i == 6)
				m.linefeed = 0x12u;
			udsp_slic_fake_monitor_set(t.s->slic, 0, &m);
			/* A new fault set is an edge, and each edge samples. */
			fault ^= UDSP_FAULT_SLIC_DC;
			udsp_slic_fake_fault_set(t.s->slic, 0, fault);
			eh_wait_ms(&t, 1400);
			CHECK(udsp_line_monitor(t.s, 0, &mon) == UDSP_OK);
			CHECK_UINT(mon.valid, 1);
			CHECK_UINT(mon.dcdc_down, i == 0);
			CHECK_UINT(mon.ovlo, i == 1);
			CHECK_UINT(mon.settled, i == 2);
			CHECK_UINT(mon.voc_err, i == 3);
			CHECK_UINT(mon.lcr, i == 4);
			CHECK_UINT(mon.rtp, i == 5);
		}
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		tc_down(&t);
	}

	CASE("a fault edge produces one snapshot and one log line");
	{
		struct udsp_slic_monitor m;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		tc_log_reset();
		memset(&m, 0, sizeof(m));
		m.vbat_mv = -48123;
		m.vbat_tgt_mv = -48000;
		m.vbat_irq_th_mv = 5000;
		m.vtr_mv = 1234;
		m.itr_ua = -567;
		m.p_hvic_mw = 12;
		m.pd_dcdc = 0x400000u;
		m.dcdc_status = 0x000abcu;
		m.lcrrtp = 0x0bu;
		m.linefeed = 0x11u;
		udsp_slic_fake_monitor_set(t.s->slic, 0, &m);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_THERMAL);
		eh_wait_ms(&t, 1400);
		CHECK(udsp_line_monitor(t.s, 0, &mon) == UDSP_OK);
		CHECK_UINT(mon.valid, 1);
		CHECK(mon.vbat_mv == -48123);
		CHECK_UINT(tc_log_hits(UDSP_LOG_WARN, "fault 0x2:"), 1);
		CHECK(tc_log_has(UDSP_LOG_WARN,
				 "line 0 fault 0x2: vbat -48123 mV "
				 "tgt -48000 mV vtr 1234 mV itr -567 uA "
				 "p_hvic 12 mW pd_dcdc 0x400000 "
				 "dcdc_status 0x000abc lcrrtp 0x0b "
				 "linefeed 0x11"));
		/* Inside the trip band: no sag line. A repeat read of the same
		 * set is no edge, so no second snapshot. */
		CHECK_UINT(tc_log_hits(UDSP_LOG_WARN, "battery sag"), 0);
		eh_wait_ms(&t, 3000);
		CHECK_UINT(tc_log_hits(UDSP_LOG_WARN, "fault 0x2:"), 1);
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		tc_down(&t);
	}

	CASE("a battery sag at the fault edge logs once and is not acted on");
	{
		struct udsp_slic_monitor m;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		tc_log_reset();
		memset(&m, 0, sizeof(m));
		m.vbat_mv = -40000;
		m.vbat_tgt_mv = -48000;
		m.vbat_irq_th_mv = 5000;
		udsp_slic_fake_monitor_set(t.s->slic, 0, &m);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_THERMAL);
		eh_wait_ms(&t, 1400);
		CHECK_UINT(tc_log_hits(UDSP_LOG_WARN, "battery sag"), 1);
		CHECK(tc_log_has(UDSP_LOG_WARN,
				 "line 0 battery sag: vbat -40000 mV "
				 "tgt -48000 mV th 5000 mV"));
		/* The sag persists through the trial and the reopen: no second
		 * line, no different feed than the fault alone commands. */
		eh_wait_ms(&t, 7000);
		CHECK_UINT(tc_log_hits(UDSP_LOG_WARN, "battery sag"), 1);
		CHECK_UINT(tc_converter_count(&t, 0), 0);
		CHECK_UINT(tc_reinit_count(&t, 0), 0);
		/* A converter in shutdown reads no battery: a zero reading
		 * against a target is not a sag. */
		m.vbat_mv = 0;
		m.pd_dcdc = 1u << 20;
		udsp_slic_fake_monitor_set(t.s->slic, 0, &m);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_DC);
		eh_wait_ms(&t, 1400);
		CHECK_UINT(tc_log_hits(UDSP_LOG_WARN, "fault 0x4:"), 1);
		CHECK_UINT(tc_log_hits(UDSP_LOG_WARN, "battery sag"), 1);
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		tc_down(&t);
	}

	CASE("a read with no data marks the row stale with the last values");
	{
		struct udsp_slic_monitor m;
		uint32_t stamp;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		memset(&m, 0, sizeof(m));
		m.vbat_mv = -47000;
		udsp_slic_fake_monitor_set(t.s->slic, 0, &m);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_THERMAL);
		eh_wait_ms(&t, 1400);
		CHECK(udsp_line_monitor(t.s, 0, &mon) == UDSP_OK);
		CHECK_UINT(mon.valid, 1);
		CHECK_UINT(mon.stale, 0);
		stamp = mon.stamp_ms;
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		eh_wait_ms(&t, 6000);
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_MONITOR_READ,
					 -ENODATA);
		udsp_slic_fake_fault_set(t.s->slic, 0, UDSP_FAULT_SLIC_THERMAL);
		eh_wait_ms(&t, 1400);
		CHECK(udsp_line_monitor(t.s, 0, &mon) == UDSP_OK);
		CHECK_UINT(mon.valid, 1);
		CHECK_UINT(mon.stale, 1);
		CHECK_UINT(mon.stamp_ms, stamp);
		CHECK(mon.vbat_mv == -47000);
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		tc_down(&t);
	}

	CASE("the monitor answers fatal on a dead line");
	{
		bool line_down;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		line_down = tc_hook_fail_wait(&t, UDSP_HOOK_FAIL_LIMIT, 1,
					      eh_mono_us() + 5000000u);
		CHECK(line_down);
		if (!line_down)
			goto dead_line_out;
		CHECK(udsp_line_monitor(t.s, 0, &mon) == UDSP_FATAL);
	dead_line_out:
		udsp_slic_fake_error_set(t.s->slic,
					 UDSP_SLIC_FAKE_HOOK_READ, 0);
		tc_stop(&t);
		tc_free(&t);
		udsp_tdm_override(NULL);
	}

	CASE("the monitor getter races the slic thread cleanly");
	{
		struct tc_mon_reader r;
		struct udsp_slic_monitor m;
		pthread_t th;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		memset(&m, 0, sizeof(m));
		m.vbat_mv = -48000;
		udsp_slic_fake_monitor_set(t.s->slic, 0, &m);
		r.t = &t;
		atomic_init(&r.stop, false);
		CHECK(pthread_create(&th, NULL, tc_mon_read_loop, &r) == 0);
		for (unsigned i = 0; i < 6; i++) {
			udsp_slic_fake_fault_set(t.s->slic, 0, i % 2 ?
						 0 : UDSP_FAULT_SLIC_THERMAL);
			eh_wait_ms(&t, 600);
		}
		atomic_store_explicit(&r.stop, true, memory_order_release);
		pthread_join(th, NULL);
		CHECK(udsp_line_monitor(t.s, 0, &mon) == UDSP_OK);
		udsp_slic_fake_fault_set(t.s->slic, 0, 0);
		tc_down(&t);
	}
}

static bool tc_diag_wait(struct eh *t, struct udsp_engine_diag *diag,
			 uint64_t after_ticks)
{
	uint64_t end = eh_mono_us() + 2000000u;

	do {
		if (udsp_engine_diag(t->s, diag) == UDSP_OK &&
		    diag->ticks > after_ticks)
			return true;
		eh_ev_poll(t);
		eh_sleep_ms(1);
	} while (eh_mono_us() < end);

	return false;
}

static void tc_diag_equal(const struct udsp_engine_diag *diag,
			  const struct engine_stats *st)
{
	CHECK_UINT(diag->serial, st->serial);
	CHECK_UINT(diag->ticks, st->ticks);
	for (unsigned i = 0; i < ENGINE_HIST_BUCKETS; i++)
		CHECK_UINT(diag->interval[i], st->interval[i]);
	CHECK_UINT(diag->interval_min_us, st->interval_min_us);
	CHECK_UINT(diag->interval_max_us, st->interval_max_us);
	CHECK_UINT(diag->capture_stalls, st->capture_stalls);
	CHECK_UINT(diag->delay_stalls, st->delay_stalls);
	CHECK_UINT(diag->xruns, st->xruns);
	CHECK_UINT(diag->holds, st->holds);
	CHECK_UINT(diag->capture_drops, st->capture_drops);
	CHECK_UINT(diag->trim_drops, st->trim_drops);
	CHECK_UINT(diag->playback_expired, st->playback_expired);
	CHECK_UINT(diag->recover_failures, st->recover_failures);
	CHECK_UINT(diag->read_errors, st->read_errors);
	CHECK_UINT(diag->write_errors, st->write_errors);
	CHECK_UINT(diag->cal_restarts, st->cal_restarts);
	CHECK(diag->delay_ref == st->delay_ref);
	CHECK(diag->loop_ref == st->loop_ref);
	CHECK(diag->delay_drift_min == st->delay_drift_min);
	CHECK(diag->delay_drift_max == st->delay_drift_max);
	CHECK_UINT(diag->granted_periods, st->granted_periods);
	CHECK_UINT(diag->period_us, st->period_us);
	CHECK(diag->running == st->running);
	CHECK(diag->stopped_fatal == st->stopped_fatal);
	CHECK(diag->delay_calibrated == st->delay_calibrated);
}

static void tc_diag_publish_check(struct eh *t)
{
	struct udsp_engine_diag diag;
	struct udsp_counters counters;
	struct engine_stats published;

	/* Hold the publisher still so both getters describe the same run. */
	pthread_mutex_lock(&t->s->engine_lock);
	engine_diag_snap_get(&t->s->engine_diag, &published);
	CHECK(udsp_engine_diag(t->s, &diag) == UDSP_OK);
	udsp_counters(t->s, &counters);
	pthread_mutex_unlock(&t->s->engine_lock);
	tc_diag_equal(&diag, &published);
	CHECK_UINT(diag.holds, counters.holds);
	CHECK_UINT(diag.capture_drops, counters.capture_drops);
	CHECK_UINT(diag.trim_drops, counters.trim_drops);
	CHECK_UINT(diag.playback_expired, counters.playback_expired);
	CHECK_UINT(diag.capture_stalls + diag.delay_stalls + diag.xruns,
		   counters.tdm_stalls);
}

static void case_engine_diag_deferred(void)
{
	struct udsp_engine_diag before, after;
	struct engine_stats stats;
	struct eh t;

	CASE("a deferred engine snapshot keeps both public "
	     "counter views unchanged");
	setenv("UDSP_WATCHDOG_MS", "20", 1);
	if (!tc_up_paced(&t, &tc_hold_ops, "USA", EH_PACE_US)) {
		CHECK(false);
		unsetenv("UDSP_WATCHDOG_MS");
		return;
	}
	/* the hold record is a DEBUG line, behind the debug mask */
	udsp_debug_set(t.s, UDSP_DEBUG_SLIC_LOG);
	CHECK(tc_diag_wait(&t, &before, ENGINE_DELAY_CAL));
	/* Hold a reader across a watchdog run without a scheduler-dependent
	 * race between the reader's copy and the publisher's check. */
	pthread_mutex_lock(&t.s->engine_lock);
	atomic_fetch_add(&t.s->engine_diag.readers, 1u);
	CHECK(udsp_engine_diag(t.s, &before) == UDSP_OK);
	pthread_mutex_unlock(&t.s->engine_lock);
	CHECK_UINT(before.holds, 0);
	atomic_store(&tc_be.hold_periods, 16u);
	CHECK(tc_ticks_wait(&t, &stats, before.ticks + 100u));
	CHECK(stats.holds > before.holds);
	CHECK(tc_log_wait(UDSP_LOG_DEBUG, "engine hold", 500));
	CHECK(udsp_engine_diag(t.s, &after) == UDSP_OK);
	CHECK_UINT(after.ticks, before.ticks);
	CHECK_UINT(after.holds, before.holds);
	tc_diag_publish_check(&t);

	atomic_fetch_sub(&t.s->engine_diag.readers, 1u);
	CHECK(tc_diag_wait(&t, &after, before.ticks));
	CHECK(after.holds > before.holds);
	tc_diag_publish_check(&t);
	tc_down(&t);
	unsetenv("UDSP_WATCHDOG_MS");
}

static atomic_bool tc_diag_read_fail;

static int tc_diag_read(struct udsp_tdm *tdm, int16_t *buffer)
{
	if (atomic_load_explicit(&tc_diag_read_fail, memory_order_relaxed))
		return -EPIPE;

	return tc_read(tdm, buffer);
}

static void case_engine_diag(void)
{
	struct udsp_engine_diag before, after, zero = { 0 };
	struct engine_stats stats;
	struct udsp_tdm_ops ops = tc_ops;
	struct eh t;
	uint64_t end;

	CASE("engine diagnostics refuse absent or unpublished engines");
	setenv("UDSP_WATCHDOG_MS", "60000", 1);
	if (!tc_up(&t, "USA")) {
		CHECK(false);
		goto done;
	}
	memset(&before, 0xa5, sizeof(before));
	CHECK(udsp_engine_diag(t.s, &before) == UDSP_UNSUPPORTED);
	CHECK(!memcmp(&before, &zero, sizeof(before)));
	CHECK(udsp_engine_diag(NULL, &before) == UDSP_UNSUPPORTED);
	CHECK(udsp_engine_diag(t.s, NULL) == UDSP_UNSUPPORTED);
	CHECK(tc_stop(&t));
	CHECK(udsp_engine_diag(t.s, &before) == UDSP_UNSUPPORTED);
	tc_free(&t);

	CASE("engine diagnostics follow watchdog publication while running");
	setenv("UDSP_WATCHDOG_MS", "20", 1);
	if (!tc_up(&t, "USA")) {
		CHECK(false);
		goto done;
	}
	CHECK(tc_diag_wait(&t, &before, ENGINE_DELAY_CAL));
	CHECK(before.running);
	CHECK(!before.stopped_fatal);
	CHECK(before.delay_calibrated);
	CHECK_UINT(before.period_us, 10000);
	CHECK_UINT(before.granted_periods, 4);
	tc_engine_stats(&t, &stats);
	CHECK_UINT(before.serial, stats.serial);
	CHECK(before.ticks <= stats.ticks);
	tc_diag_publish_check(&t);
	CHECK(tc_diag_wait(&t, &after, before.ticks));
	CHECK_UINT(after.serial, before.serial);
	CHECK(after.ticks > before.ticks);
	tc_diag_publish_check(&t);

	CASE("engine diagnostics clear on teardown and refresh on restart");
	CHECK(tc_stop(&t));
	CHECK(udsp_engine_diag(t.s, &after) == UDSP_UNSUPPORTED);
	tc_wait_real_ms(&t, 2u * udsp_watchdog_ms_get());
	CHECK(udsp_engine_diag(t.s, &after) == UDSP_UNSUPPORTED);
	CHECK(tc_start(&t));
	CHECK(tc_diag_wait(&t, &after, ENGINE_DELAY_CAL));
	CHECK(after.serial > before.serial);
	CHECK(after.running);
	CHECK(!after.stopped_fatal);
	tc_down(&t);

	CASE("engine diagnostics report a fatal engine without a lock");
	ops.read_period = tc_diag_read;
	atomic_store_explicit(&tc_diag_read_fail, false, memory_order_relaxed);
	if (!tc_up_paced(&t, &ops, "USA", EH_PACE_US)) {
		CHECK(false);
		goto done;
	}
	CHECK(tc_diag_wait(&t, &before, ENGINE_DELAY_CAL));
	atomic_store_explicit(&tc_diag_read_fail, true, memory_order_relaxed);
	end = eh_mono_us() + 2000000u;
	do {
		CHECK(udsp_engine_diag(t.s, &after) == UDSP_OK);
		if (after.stopped_fatal)
			break;
		eh_ev_poll(&t);
		eh_sleep_ms(1);
	} while (eh_mono_us() < end);
	CHECK(after.stopped_fatal);
	CHECK(!after.running);
	CHECK(!after.delay_calibrated);
	CHECK_UINT(after.xruns, 10);
	CHECK_UINT(after.read_errors, 10);
	tc_engine_stats(&t, &stats);
	tc_diag_equal(&after, &stats);
	tc_diag_publish_check(&t);
	tc_free(&t);
	atomic_store_explicit(&tc_diag_read_fail, false, memory_order_relaxed);

done:
	udsp_tdm_override(NULL);
	unsetenv("UDSP_WATCHDOG_MS");
}

static void case_engine_diag_hold(void)
{
	const struct tc_hold_arm arms[] = {
		{ .prof = &fk_an7581, .hold = 16, .jit = true },
		{ .prof = &fk_mt7986, .hold = 6, .silence = true },
	};

	setenv("UDSP_WATCHDOG_MS", "20", 1);
	for (unsigned i = 0; i < 2; i++) {
		struct udsp_engine_diag diag;
		struct udsp_config cfg;
		struct eh t;

		CASE(i ? "engine diagnostics publish expired playback periods" :
		     "engine diagnostics publish capture and trim drops");
		if (!tc_hold_open(&t, &cfg, &arms[i])) {
			CHECK(false);
			break;
		}
		CHECK(tc_diag_wait(&t, &diag, 100));
		CHECK_UINT(diag.holds, 1);
		CHECK_UINT(diag.capture_stalls, 1);
		CHECK_UINT(diag.cal_restarts, 1);
		CHECK_UINT(diag.xruns, 0);
		if (i) {
			CHECK(diag.playback_expired > 0);
		} else {
			CHECK(diag.capture_drops > 0);
			CHECK(diag.trim_drops > 0);
		}
		tc_diag_publish_check(&t);
		tc_free(&t);
		fk_release();
	}
	unsetenv("UDSP_WATCHDOG_MS");
}

/* --- the delayed loop ---------------------------------------------------- */

/*
 * The impulse the injector puts in one capture sample and the level the oracle
 * reads it back at. No stream, tone or playout runs on a line these cases
 * drive, so a playback frame that carries anything carries the impulse.
 */
#define TC_LOOP_MARK	4000
#define TC_LOOP_LEVEL	2000
/* The answer of an observation that found no impulse, and of a delay the
 * window never played. It is a value no frame number takes. */
#define TC_LOOP_NONE	UINT_MAX
/* Every case below runs the narrowband line. */
#define TC_LOOP_FRAME	UDSP_FRAME_SAMPLES

static unsigned tc_loop_frames_total(void)
{
	return (unsigned)atomic_load_explicit(&eh_be.rec_len,
					      memory_order_acquire) /
	       TC_LOOP_FRAME;
}

static int32_t tc_loop_peak(const int16_t *rec, unsigned frame)
{
	int32_t peak = 0;

	for (unsigned i = 0; i < TC_LOOP_FRAME; i++) {
		int32_t v = rec[frame * TC_LOOP_FRAME + i];

		if (v < 0)
			v = -v;
		if (v > peak)
			peak = v;
	}

	return peak;
}

/* The first frame at or after from that carries the impulse, or TC_LOOP_NONE
 * when no frame of the window does. The bound is read once and the answer for
 * "nowhere" is a fixed value, so a tick that appends between two reads cannot
 * move it. */
static unsigned tc_loop_hit(const int16_t *rec, unsigned from)
{
	unsigned total = tc_loop_frames_total();

	for (unsigned f = from; f < total; f++) {
		if (tc_loop_peak(rec, f) > TC_LOOP_LEVEL)
			return f;
	}

	return TC_LOOP_NONE;
}

static void tc_loop_arm(unsigned ch)
{
	atomic_store_explicit(&tc_be.cap_mark_at[ch], UINT32_MAX,
			      memory_order_release);
	atomic_store_explicit(&tc_be.cap_mark[ch], TC_LOOP_MARK,
			      memory_order_release);
}

/* The playback frame of the tick that captured the impulse, or TC_LOOP_NONE
 * while no capture period took it. */
static unsigned tc_loop_at(unsigned ch)
{
	uint32_t at = (uint32_t)atomic_load_explicit(&tc_be.cap_mark_at[ch],
						     memory_order_acquire);

	return at == UINT32_MAX ? TC_LOOP_NONE : (unsigned)at;
}

/*
 * Injects one impulse on the capture of ch and answers the frames between the
 * tick that captured it and the frame the loop played it in. TC_LOOP_NONE says
 * the window played it nowhere.
 */
static unsigned tc_loop_delay_get(struct eh *t, unsigned ch, const int16_t *rec,
				  uint32_t wait_ms)
{
	unsigned at, hit;

	tc_loop_arm(ch);
	eh_wait_ms(t, wait_ms);
	at = tc_loop_at(ch);
	if (at == TC_LOOP_NONE)
		return TC_LOOP_NONE;
	hit = tc_loop_hit(rec, at);

	return hit == TC_LOOP_NONE ? TC_LOOP_NONE : hit - at;
}

/* The level the loop played the impulse at, or 0 when the window played it
 * nowhere. at is the frame the impulse was captured in. */
static int32_t tc_loop_level(const int16_t *rec, unsigned at)
{
	unsigned hit = at == TC_LOOP_NONE ? TC_LOOP_NONE : tc_loop_hit(rec, at);

	return hit == TC_LOOP_NONE ? 0 : tc_loop_peak(rec, hit);
}

/* The length of the first run of silent frames at or after from, with *at
 * taking the frame it starts on. A frame of a loop that plays the capture tone
 * is never silent, so the run is the audio the loop had none of. */
static unsigned tc_loop_quiet_run(const int16_t *rec, unsigned from,
				  unsigned *at)
{
	unsigned total = tc_loop_frames_total();
	unsigned start = from;
	unsigned n = 0;

	while (start < total && tc_loop_peak(rec, start))
		start++;
	while (start + n < total && !tc_loop_peak(rec, start + n))
		n++;
	*at = start;

	return n;
}

static void case_loop(void)
{
	struct udsp_chan_cfg ccfg;
	struct tc_stream w;
	struct eh t;

	CASE("a self loop plays the capture back the delay it was armed with");
	{
		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0, 200) ==
		      UDSP_OK);
		CHECK_UINT(tc_loop_delay_get(&t, 0, eh_be.rec, 600), 20);
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_OFF, 0, 0) == UDSP_OK);
		tc_down(&t);
	}

	CASE("a zero and a five millisecond delay both take one frame");
	{
		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0, 0) == UDSP_OK);
		CHECK_UINT(tc_loop_delay_get(&t, 0, eh_be.rec, 200), 1);
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0, 5) == UDSP_OK);
		CHECK_UINT(tc_loop_delay_get(&t, 0, eh_be.rec, 200), 1);
		tc_down(&t);
	}

	CASE("a cross loop plays each port the capture of the other");
	{
		unsigned at;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_PEER, 1, 0) == UDSP_OK);
		CHECK(udsp_line_loop(t.s, 1, UDSP_LOOP_PEER, 0, 0) == UDSP_OK);

		/* Line 0 speaks: line 1 plays it one tick later and line 0
		 * plays nothing of it, whatever order the tick takes the two
		 * lines in. */
		CHECK_UINT(tc_loop_delay_get(&t, 0, eh_be.rec1, 200), 1);
		at = tc_loop_at(0);
		CHECK(at != TC_LOOP_NONE);
		if (at != TC_LOOP_NONE)
			CHECK_UINT(tc_loop_hit(eh_be.rec, at), TC_LOOP_NONE);

		/* And the other way, which is the direction the index order
		 * would break. */
		CHECK_UINT(tc_loop_delay_get(&t, 1, eh_be.rec, 200), 1);
		at = tc_loop_at(1);
		CHECK(at != TC_LOOP_NONE);
		if (at != TC_LOOP_NONE)
			CHECK_UINT(tc_loop_hit(eh_be.rec1, at), TC_LOOP_NONE);
		tc_down(&t);
	}

	CASE("a one second loop lands a hundred frames later across the wrap");
	{
		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0,
				     UDSP_LOOP_DELAY_MAX_MS) == UDSP_OK);
		/* The ring holds 101 slots, so every pass carries the index
		 * over a wrap. */
		for (unsigned i = 0; i < 3u; i++)
			CHECK_UINT(tc_loop_delay_get(&t, 0, eh_be.rec, 1400),
				   100);

		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_PEER, 1,
				     UDSP_LOOP_DELAY_MAX_MS) == UDSP_OK);
		for (unsigned i = 0; i < 2u; i++)
			CHECK_UINT(tc_loop_delay_get(&t, 1, eh_be.rec, 1400),
				   100);

		CHECK(udsp_line_loop(t.s, 1, UDSP_LOOP_PEER, 0,
				     UDSP_LOOP_DELAY_MAX_MS) == UDSP_OK);
		for (unsigned i = 0; i < 2u; i++)
			CHECK_UINT(tc_loop_delay_get(&t, 0, eh_be.rec1, 1400),
				   100);
		tc_down(&t);
	}

	CASE("a disarm clears the ring and a re-arm replays nothing behind it");
	{
		unsigned at;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0,
				     UDSP_LOOP_DELAY_MAX_MS) == UDSP_OK);
		tc_loop_arm(0);
		/* Long enough for a capture period to take the impulse and far
		 * short of the second it would have played it in. */
		eh_wait_ms(&t, 200);
		at = tc_loop_at(0);
		CHECK(at != TC_LOOP_NONE);
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_OFF, 0, 0) == UDSP_OK);
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0,
				     UDSP_LOOP_DELAY_MAX_MS) == UDSP_OK);
		eh_wait_ms(&t, 1400);
		if (at != TC_LOOP_NONE)
			CHECK_UINT(tc_loop_hit(eh_be.rec, at), TC_LOOP_NONE);
		tc_down(&t);
	}

	CASE("a bad handle, line, mode, delay or peer is unsupported");
	{
		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(udsp_line_loop(NULL, 0, UDSP_LOOP_SELF, 0, 100) ==
		      UDSP_UNSUPPORTED);
		CHECK(udsp_line_loop(t.s, 2, UDSP_LOOP_SELF, 0, 100) ==
		      UDSP_UNSUPPORTED);
		CHECK(udsp_line_loop(t.s, UDSP_LINE_MAX, UDSP_LOOP_SELF, 0,
				     100) == UDSP_UNSUPPORTED);
		CHECK(udsp_line_loop(t.s, 0, (enum udsp_loop_mode)3, 0, 100) ==
		      UDSP_UNSUPPORTED);
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0,
				     UDSP_LOOP_DELAY_MAX_MS + 1u) ==
		      UDSP_UNSUPPORTED);
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_PEER, 0, 100) ==
		      UDSP_UNSUPPORTED);
		/* Past the two lines of this handle: a refusal taken after
		 * udsp_line_busy would index the channel array out of bound. */
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_PEER, 7, 100) ==
		      UDSP_UNSUPPORTED);
		/* The arguments are read before the exclusions, so a busy line
		 * still answers unsupported for a delay it cannot take. */
		CHECK(tc_stream_up(&t, &w, 0));
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0,
				     UDSP_LOOP_DELAY_MAX_MS + 1u) ==
		      UDSP_UNSUPPORTED);
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0, 100) ==
		      UDSP_TRANSIENT);
		tc_stream_down(&t, &w, 0);
		tc_down(&t);
	}

	CASE("the loop excludes a metallic test, a ring and a stream");
	{
		struct udsp_job job = { 0 };
		struct udsp_ring ring;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);

		/* The plug-in drives the wire for the whole test. */
		tc_mlt_delay(&t, 200);
		CHECK(udsp_line_test(t.s, 0, UDSP_TEST_GR909, &job) == UDSP_OK);
		CHECK(tc_test_entered(&t, 0));
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0, 100) ==
		      UDSP_TRANSIENT);
		/* A disarm is refused for nothing: it plays no audio. */
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_OFF, 0, 0) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 5000, NULL));
		tc_mlt_delay(&t, 0);

		/* A ring session owns the feed the loop would play into. */
		memset(&ring, 0, sizeof(ring));
		ring.cadence[0].on_ms = 2000;
		ring.cadence[0].off_ms = 4000;
		ring.cadence_count = 1;
		CHECK(udsp_ring_start(t.s, 0, &ring) == UDSP_OK);
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0, 100) ==
		      UDSP_TRANSIENT);
		CHECK(udsp_ring_stop(t.s, 0) == UDSP_OK);
		/* The journal republishes the ownership on the tick after the
		 * stop, so the arm waits one tick for it. */
		eh_wait_ms(&t, 20);
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0, 100) ==
		      UDSP_OK);

		/* And the other way: the armed loop holds the line against
		 * both of them until it is disarmed. */
		CHECK(udsp_line_test(t.s, 0, UDSP_TEST_GR909, &job) ==
		      UDSP_TRANSIENT);
		CHECK(udsp_stream_config(t.s, 0, 0, NULL, -1, -1) ==
		      UDSP_TRANSIENT);
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_OFF, 0, 0) == UDSP_OK);
		CHECK(tc_test_run(&t, 0, UDSP_TEST_GR909, NULL));

		/* A stream configured before the arm is not active, so the arm
		 * stands; its start would put media and the loop into the same
		 * downlink, so it waits for the disarm. */
		CHECK(tc_stream_config(&t, &w, 0));
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0, 100) == UDSP_OK);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_TRANSIENT);
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_OFF, 0, 0) == UDSP_OK);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		tc_stream_down(&t, &w, 0);
		tc_down(&t);
	}

	CASE("the capability reports the loop feature");
	{
		const struct udsp_cap *cap;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		cap = udsp_cap(t.s);
		CHECK(cap != NULL);
		CHECK(cap && (cap->features & UDSP_FEAT_LOOP));
		tc_down(&t);
	}

	CASE("a stop and a start disarm the loop");
	{
		unsigned base;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0, 200) ==
		      UDSP_OK);
		CHECK_UINT(tc_loop_delay_get(&t, 0, eh_be.rec, 600), 20);
		CHECK(tc_stop(&t));
		CHECK(tc_start(&t));

		/* Nothing of the line reaches the line any more. The mode is
		 * read from the playback and not from the channel, because the
		 * tick owns that field and no edge orders a read of it here. */
		base = tc_loop_frames_total();
		tc_loop_arm(0);
		eh_wait_ms(&t, 600);
		CHECK(tc_loop_at(0) != TC_LOOP_NONE);
		CHECK_UINT(tc_loop_hit(eh_be.rec, base), TC_LOOP_NONE);
		/* The control side forgot the arm with it, so the ops the loop
		 * holds out are admitted again. */
		CHECK(tc_test_run(&t, 0, UDSP_TEST_GR909, NULL));
		tc_down(&t);
	}

	CASE("a skip plays silence for its periods and nothing older");
	{
		static const unsigned skip[3] = { 1u, 100u,
						  UDSP_LOOP_SLOTS + 20u };
		static const unsigned want[3] = { 1u, 100u, 100u };

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		/* A tone in every slot, so a slot the skip leaves alone replays
		 * the audio of the previous wrap instead of silence. */
		atomic_store_explicit(&tc_be.tone_hz, 1000u,
				      memory_order_release);
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0,
				     UDSP_LOOP_DELAY_MAX_MS) == UDSP_OK);
		eh_wait_ms(&t, 2000);
		for (unsigned i = 0; i < 3u; i++) {
			unsigned base = tc_loop_frames_total();
			unsigned at = 0;

			CHECK(tc_loop_peak(eh_be.rec, base - 1u) >
			      TC_LOOP_LEVEL);
			atomic_store_explicit(&tc_be.skip_periods, skip[i],
					      memory_order_release);
			eh_wait_ms(&t, 2000);
			CHECK_UINT(tc_loop_quiet_run(eh_be.rec, base, &at),
				   want[i]);
		}
		tc_down(&t);
	}

	CASE("the loop carries no transmit gain and one receive gain");
	{
		int32_t peak;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.gain_tx = 12;		/* 6 dB on the uplink alone */
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0, 100) ==
		      UDSP_OK);
		CHECK_UINT(tc_loop_delay_get(&t, 0, eh_be.rec, 400), 10);
		/* The loop takes the capture before the transmit gain. */
		CHECK_UINT(tc_loop_level(eh_be.rec, tc_loop_at(0)),
			   TC_LOOP_MARK);

		ccfg.gain_tx = 0;
		ccfg.gain_rx = 12;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK_UINT(tc_loop_delay_get(&t, 0, eh_be.rec, 400), 10);
		peak = tc_loop_level(eh_be.rec, tc_loop_at(0));
		/* Six decibels up, once: 4000 becomes about 7981. */
		CHECK(peak > 7500);
		CHECK(peak < 8400);
		tc_down(&t);
	}

	CASE("the loop takes the whole receive gain");
	{
		int32_t peak;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		/* +6 dB of DSP gain and a 9 dB receive loss of the port: the
		 * loop plays 3 dB down, and the SLIC stays at 0 dB. */
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.gain_rx = 12;
		ccfg.slic_gain_rx = -18;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0, 100) ==
		      UDSP_OK);
		CHECK_UINT(tc_loop_delay_get(&t, 0, eh_be.rec, 400), 10);
		peak = tc_loop_level(eh_be.rec, tc_loop_at(0));
		/* 4000 becomes about 2832. */
		CHECK(peak > 2650);
		CHECK(peak < 3000);
		tc_down(&t);
	}

	CASE("a dead source plays silence");
	{
		unsigned at;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_PEER, 1,
				     UDSP_LOOP_DELAY_MAX_MS) == UDSP_OK);
		tc_loop_arm(1);
		eh_wait_ms(&t, 200);
		at = tc_loop_at(1);
		CHECK(at != TC_LOOP_NONE);
		/* The source dies well before the second of delay elapses. The
		 * armed init failure keeps the line down for the rest of the
		 * case, so the retry cannot bring it back. */
		udsp_slic_fake_error_repeat_set(t.s->slic, UDSP_SLIC_FAKE_INIT,
						-EIO, 8);
		atomic_store_explicit(&t.s->chan[1].dead, true,
				      memory_order_release);
		eh_wait_ms(&t, 1400);
		if (at != TC_LOOP_NONE)
			CHECK_UINT(tc_loop_hit(eh_be.rec, at), TC_LOOP_NONE);
		udsp_slic_fake_error_repeat_set(t.s->slic, UDSP_SLIC_FAKE_INIT,
						0, 0);
		tc_down(&t);
	}

	CASE("the loop delay bound and the ring size are pinned");
	{
		CHECK_UINT(UDSP_LOOP_DELAY_MAX_MS, 1000);
		CHECK_UINT(UDSP_LOOP_SLOTS, 101);
		/* One slot more than the longest delay, so a read never falls
		 * on the slot its own tick just wrote. */
		CHECK_UINT(UDSP_LOOP_SLOTS,
			   UDSP_LOOP_DELAY_MAX_MS / UDSP_TICK_MS + 1u);
	}

	CASE("a self loop ignores the peer whatever value it carries");
	{
		/* The command packs the peer beside the delay, so a peer the
		 * mode never reads must not reach the delay field. */
		static const unsigned peer[3] = { 1u, 256u, 70000u };

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		for (unsigned i = 0; i < 3u; i++) {
			CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, peer[i],
					     200) == UDSP_OK);
			CHECK_UINT(tc_loop_delay_get(&t, 0, eh_be.rec, 600), 20);
		}
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_OFF, 0, 0) == UDSP_OK);
		tc_down(&t);
	}

	CASE("a stop disarms a line that died while it was armed");
	{
		unsigned base;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(udsp_line_loop(t.s, 1, UDSP_LOOP_SELF, 0, 200) == UDSP_OK);
		/* The line dies with the loop standing. The armed init failure
		 * keeps the retry from reviving it before the stop. */
		udsp_slic_fake_error_repeat_set(t.s->slic, UDSP_SLIC_FAKE_INIT,
						-EIO, 8);
		atomic_store_explicit(&t.s->chan[1].dead, true,
				      memory_order_release);
		eh_wait_ms(&t, 50);
		CHECK(tc_stop(&t));
		udsp_slic_fake_error_repeat_set(t.s->slic, UDSP_SLIC_FAKE_INIT,
						0, 0);
		CHECK(tc_start(&t));
		/* The safe pass cleared the loop of the dead line too, so the
		 * line that came back plays its own capture nowhere. */
		base = tc_loop_frames_total();
		tc_loop_arm(1);
		eh_wait_ms(&t, 600);
		CHECK(tc_loop_at(1) != TC_LOOP_NONE);
		CHECK_UINT(tc_loop_hit(eh_be.rec1, base), TC_LOOP_NONE);
		/* The control-side record went with it, so the line admits the
		 * ops a loop holds out. */
		CHECK(tc_test_run(&t, 1, UDSP_TEST_GR909, NULL));
		tc_down(&t);
	}

	CASE("a metallic test and an arm at the queue boundary let one win");
	{
		struct udsp_job job = { 0 };
		enum udsp_status loop;
		int32_t status = -1;
		uint8_t buf[64];
		size_t len = 1;

		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		/* The thread runs one job step per pass and samples the hook
		 * after it, so a parked hook read leaves the test in the queue
		 * while the arm arrives. */
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_HOOK_READ,
					 200);
		eh_sleep_ms(20);
		CHECK(udsp_line_test(t.s, 0, UDSP_TEST_GR909, &job) == UDSP_OK);
		loop = udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0, 100);
		udsp_slic_fake_delay_set(t.s->slic, UDSP_SLIC_FAKE_HOOK_READ, 0);
		CHECK(eh_job_wait(&t, job.token, 5000, &status));
		if (loop == UDSP_OK) {
			/* The arm won, so the worker refused the test and the
			 * plug-in was never called: a test that did not run
			 * leaves no record. */
			CHECK_UINT((uint32_t)status, UDSP_TRANSIENT);
			CHECK(udsp_line_test_result(t.s, 0, buf, sizeof(buf),
						    &len) == UDSP_UNSUPPORTED);
			CHECK_UINT(len, 0);
			CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_OFF, 0, 0) ==
			      UDSP_OK);
		} else {
			/* The test won, so the tick refused the arm. */
			CHECK_UINT(loop, UDSP_TRANSIENT);
			CHECK_UINT((uint32_t)status, UDSP_OK);
		}
		tc_down(&t);
	}
}

/* --- record failures during a caller identity sequence ------------------- */

/* A caller identity request that blocks on its arm ack while the gate holds
 * the tick, so it runs on its own thread. */
struct tc_cid_async {
	udsp_t *s;
	struct udsp_cid cid;
	enum udsp_status status;
};

static void *tc_cid_send_run(void *arg)
{
	struct tc_cid_async *a = arg;

	a->status = udsp_cid_send(a->s, 0, UDSP_CID_TYPE1, &a->cid);

	return NULL;
}

/* True once a command waits in the ring that the next tick drains. */
static bool tc_ctl_queued_wait(struct eh *t)
{
	struct cmd_ring *r = &t->s->ctl_ring;
	uint64_t end = eh_mono_us() + EH_GATE_WAIT_US;
	bool queued;

	do {
		queued = atomic_load_explicit(&r->head, memory_order_acquire) !=
			 atomic_load_explicit(&r->tail, memory_order_acquire);
		if (!queued)
			sched_yield();
	} while (!queued && eh_mono_us() < end);
	CHECK(queued);

	return queued;
}

struct tc_ev_want {
	unsigned type;
	unsigned n;
};

static bool tc_ev_reached(struct eh *t, const void *arg)
{
	const struct tc_ev_want *w = arg;

	(void)t;
	return eh_ev_count(w->type) >= w->n;
}

static bool tc_ev_wait(struct eh *t, unsigned type, unsigned n, unsigned ms)
{
	struct tc_ev_want w = { .type = type, .n = n };

	return tc_poll_until(t, tc_ev_reached, &w, ms);
}

struct tc_lf_want {
	unsigned line;
	uint8_t lf;
};

static bool tc_lf_reached(struct eh *t, const void *arg)
{
	const struct tc_lf_want *w = arg;

	return tc_lf_confirmed(t, w->line) == w->lf;
}

static bool tc_lf_wait(struct eh *t, unsigned line, uint8_t want, unsigned ms)
{
	struct tc_lf_want w = { .line = line, .lf = want };

	return tc_poll_until(t, tc_lf_reached, &w, ms);
}

/* Steps the held tick until the burst of line 0 ends. The tick of the end
 * queues the restore write and then parks. */
static bool tc_burst_end_step(struct eh_gate *gate, struct eh *t)
{
	struct udsp_chan *c = &t->s->chan[0];

	for (unsigned i = 0; i < 1000u; i++) {
		if (!atomic_load_explicit(&c->owned, memory_order_acquire))
			return true;
		if (!eh_gate_step(gate, 1))
			return false;
	}
	CHECK(false);

	return false;
}

/* Parks the tick inside the burst of line 0 and fails the restore write at the
 * end of the burst. True once the slic thread ran the failed write. The gate
 * still holds the tick, so no write of the feed follows it yet. */
static bool tc_burst_restore_fail(struct eh_gate *gate, struct eh *t)
{
	if (!eh_gate_park(gate))
		return false;
	/* The next linefeed write is the restore of the burst. */
	udsp_slic_fake_error_set(t->s->slic, UDSP_SLIC_FAKE_LINEFEED_SET, -EIO);
	if (!tc_burst_end_step(gate, t))
		return false;
	/* The slic thread queues the failure record, then logs it. */
	if (tc_log_wait(UDSP_LOG_WARN, "slic linefeed failed on line 0",
			EH_GATE_WAIT_US / 1000u))
		return true;
	CHECK(false);

	return false;
}

/* Steps the held tick until the slic thread logs a failed linefeed write of
 * line 0. The slic thread runs the write after the tick that queued it, so
 * each step waits a moment for the log line. */
static bool tc_lf_fail_step(struct eh_gate *gate)
{
	for (unsigned i = 0; i < 1000u; i++) {
		if (!eh_gate_step(gate, 1))
			return false;
		if (tc_log_wait(UDSP_LOG_WARN, "slic linefeed failed on line 0",
				20))
			return true;
	}
	CHECK(false);

	return false;
}

static void case_cid_rec_fail(void)
{
	struct eh t;

	/*
	 * The restore write of burst one fails while the gate holds the tick.
	 * The next tick drains the arm of burst two, then the failure record.
	 * The failure belongs to burst one, which ended, so burst two runs to
	 * its end.
	 */
	CASE("a write failure before the arm leaves the new burst running");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct tc_cid_async a;
		pthread_t th;
		bool sent = false;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		memset(&a, 0, sizeof(a));
		a.s = t.s;
		tc_cid_fill(&a.cid, "5551234", "TEST CALLER");
		CHECK(udsp_cid_send(t.s, 0, UDSP_CID_TYPE1, &a.cid) == UDSP_OK);
		if (!tc_lf_wait(&t, 0, UDSP_LF_FWD_OHT, 5000) ||
		    !tc_burst_restore_fail(&gate, &t))
			goto stale_out;
		sent = !pthread_create(&th, NULL, tc_cid_send_run, &a);
		CHECK(sent);
		if (sent && tc_ctl_queued_wait(&t))
			CHECK(eh_gate_step(&gate, 1));
	stale_out:
		eh_gate_disarm(&gate);
		if (sent) {
			pthread_join(th, NULL);
			CHECK_UINT(a.status, UDSP_OK);
		}
		CHECK(tc_ev_wait(&t, UDSP_EV_CID_SENT, 2, 5000));
		CHECK(tc_cid_sent_nth(0) == 0);
		CHECK(tc_cid_sent_nth(1) == 0);
		/* Burst two wrote its own feed and its own restore. */
		CHECK(tc_lf_wait(&t, 0, UDSP_LF_FWD_ACTIVE, 5000));
		tc_down(&t);
		eh_gate_close(&gate);
	}

	CASE("a burst whose own feed write fails reports the failure");
	{
		struct udsp_cid cid;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		tc_cid_fill(&cid, "5551234", "TEST CALLER");
		/* The next linefeed write is the on-hook transmission feed that
		 * the burst writes after its arm. */
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_LINEFEED_SET,
					 -EIO);
		CHECK(udsp_cid_send(t.s, 0, UDSP_CID_TYPE1, &cid) == UDSP_OK);
		CHECK(tc_ev_wait(&t, UDSP_EV_CID_SENT, 1, 5000));
		CHECK(tc_cid_sent_nth(0) == -EIO);
		tc_down(&t);
	}

	/* The relay off at the end of the first ring is a record of the ring
	 * session. The session queues it after the arm, and the burst waits in
	 * the break that it opens. */
	CASE("a relay write queued after the arm aborts the burst");
	{
		struct udsp_ring ring;

		CHECK(tc_region_fast());
		if (!tc_up_ports(&t, "FST", 1)) {
			CHECK(false);
			return;
		}
		tc_pace_fast();
		eh_wait_ms(&t, 60);
		tc_ring_fill(&ring, 200, 4000, 30000);
		ring.cid_present = true;
		ring.cid_at_ring = 1;
		tc_cid_fill(&ring.cid, "5551234", "TEST CALLER");
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_RING_STOP,
					 -EIO);
		CHECK(udsp_ring_start(t.s, 0, &ring) == UDSP_OK);
		CHECK(tc_ev_wait(&t, UDSP_EV_CID_SENT, 1, 5000));
		CHECK(tc_cid_sent_nth(0) == -EIO);
		CHECK(udsp_ring_stop(t.s, 0) == UDSP_OK);
		tc_down(&t);
		tc_region_fast_drop();
	}

	/*
	 * Burst one arms, and its own feed write fails. A second send drains
	 * in the tick that also drains that failure. The second arm is refused
	 * and must not move cid_arm_seq, or the failure of burst one would read
	 * as a write from before its arm.
	 */
	CASE("a refused arm leaves the running burst its own failure");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct tc_cid_async one, two;
		pthread_t th_one, th_two;
		bool sent_one = false, sent_two = false;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		memset(&one, 0, sizeof(one));
		one.s = t.s;
		tc_cid_fill(&one.cid, "5551234", "TEST CALLER");
		two = one;
		if (!eh_gate_park(&gate))
			goto refused_out;
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_LINEFEED_SET,
					 -EIO);
		sent_one = !pthread_create(&th_one, NULL, tc_cid_send_run, &one);
		CHECK(sent_one);
		if (!sent_one || !tc_ctl_queued_wait(&t))
			goto refused_out;
		CHECK(eh_gate_step(&gate, 1));
		if (!tc_log_wait(UDSP_LOG_WARN, "slic linefeed failed on line 0",
				 EH_GATE_WAIT_US / 1000u)) {
			CHECK(false);
			goto refused_out;
		}
		sent_two = !pthread_create(&th_two, NULL, tc_cid_send_run, &two);
		CHECK(sent_two);
		if (sent_two && tc_ctl_queued_wait(&t))
			CHECK(eh_gate_step(&gate, 1));
	refused_out:
		eh_gate_disarm(&gate);
		if (sent_one) {
			pthread_join(th_one, NULL);
			CHECK_UINT(one.status, UDSP_OK);
		}
		if (sent_two) {
			pthread_join(th_two, NULL);
			CHECK_UINT(two.status, UDSP_TRANSIENT);
		}
		CHECK(tc_ev_wait(&t, UDSP_EV_CID_SENT, 1, 5000));
		CHECK(tc_cid_sent_nth(0) == -EIO);
		tc_down(&t);
		eh_gate_close(&gate);
	}

	/*
	 * The restore write at the end of a standalone burst fails, and no
	 * session or command follows it. The restore stays due, so a clean
	 * read of the fault poll writes the base feed again. The burst itself
	 * completed, so its status stays 0.
	 */
	CASE("a failed restore at the end of a burst is written again");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct udsp_cid cid;
		bool failed = false;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		tc_cid_fill(&cid, "5551234", "TEST CALLER");
		CHECK(udsp_cid_send(t.s, 0, UDSP_CID_TYPE1, &cid) == UDSP_OK);
		if (!tc_lf_wait(&t, 0, UDSP_LF_FWD_OHT, 5000)) {
			CHECK(false);
			goto retry_out;
		}
		failed = tc_burst_restore_fail(&gate, &t);
		if (failed)
			CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_OHT);
	retry_out:
		eh_gate_disarm(&gate);
		if (failed) {
			CHECK(tc_lf_wait(&t, 0, UDSP_LF_FWD_ACTIVE, 5000));
			/* The start, the burst and one write of the retry. */
			CHECK_UINT(tc_linefeed_count(&t, 0), 3);
		}
		CHECK(tc_ev_wait(&t, UDSP_EV_CID_SENT, 1, 5000));
		CHECK(tc_cid_sent_nth(0) == 0);
		tc_down(&t);
		eh_gate_close(&gate);
	}

	/* A DOWN request during the burst goes into its restore as the open
	 * feed. A failed write of that feed leaves the line powered, so it
	 * gets the same retry as a powered base. */
	CASE("a failed open restore after a DOWN request is written again");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct udsp_cid cid;
		bool failed = false;

		if (!tc_up_ports(&t, "USA", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		tc_cid_fill(&cid, "5551234", "TEST CALLER");
		CHECK(udsp_cid_send(t.s, 0, UDSP_CID_TYPE1, &cid) == UDSP_OK);
		if (!tc_lf_wait(&t, 0, UDSP_LF_FWD_OHT, 5000)) {
			CHECK(false);
			goto down_out;
		}
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_DOWN) == UDSP_OK);
		failed = tc_burst_restore_fail(&gate, &t);
		if (failed)
			CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_OHT);
	down_out:
		eh_gate_disarm(&gate);
		if (failed) {
			CHECK(tc_lf_wait(&t, 0, UDSP_LF_OPEN, 5000));
			/* The start, the burst and one write of the retry. */
			CHECK_UINT(tc_linefeed_count(&t, 0), 3);
		}
		tc_down(&t);
		eh_gate_close(&gate);
	}

	/*
	 * The burst of a ring session ends with a failed restore. The session
	 * holds the line, so the retry writes nothing into it. The base feed
	 * returns only after the session ends.
	 */
	CASE("a failed restore inside a ring session waits for the session end");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct udsp_ring ring;
		unsigned writes = 0;
		bool failed = false;

		CHECK(tc_region_fast());
		if (!tc_up_ports(&t, "FST", 1)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 60);
		tc_ring_fill(&ring, 200, 4000, 30000);
		ring.cid_present = true;
		ring.cid_at_ring = 1;
		tc_cid_fill(&ring.cid, "5551234", "TEST CALLER");
		CHECK(udsp_ring_start(t.s, 0, &ring) == UDSP_OK);
		if (!tc_lf_wait(&t, 0, UDSP_LF_FWD_OHT, 5000) ||
		    !eh_gate_park(&gate)) {
			CHECK(false);
			goto ring_restore_out;
		}
		/* The next linefeed write is the restore of the burst. */
		udsp_slic_fake_error_set(t.s->slic, UDSP_SLIC_FAKE_LINEFEED_SET,
					 -EIO);
		failed = tc_lf_fail_step(&gate);
		/* The baseline comes while the gate holds the tick, so no fault
		 * read can write the base before it. */
		if (failed) {
			writes = tc_linefeed_count(&t, 0);
			CHECK_UINT(tc_lf_confirmed(&t, 0), UDSP_LF_FWD_OHT);
		}
	ring_restore_out:
		eh_gate_disarm(&gate);
		if (failed) {
			tc_pace_fast();
			/* Three fault reads while the session runs. */
			eh_wait_ms(&t, 3000);
			CHECK_UINT(tc_linefeed_count(&t, 0), writes);
			CHECK(udsp_ring_stop(t.s, 0) == UDSP_OK);
			CHECK(tc_lf_wait(&t, 0, UDSP_LF_FWD_ACTIVE, 5000));
		}
		CHECK(tc_ev_wait(&t, UDSP_EV_CID_SENT, 1, 5000));
		CHECK(tc_cid_sent_nth(0) == 0);
		tc_down(&t);
		eh_gate_close(&gate);
		tc_region_fast_drop();
	}
}

/* The count of equal pairs among n values. */
static unsigned tc_lec_same_count(const uint32_t *v, unsigned n)
{
	unsigned same = 0;

	for (unsigned i = 0; i < n; i++) {
		for (unsigned j = i + 1u; j < n; j++)
			same += v[i] == v[j];
	}

	return same;
}

/*
 * The processor verdicts and the peak tap travel from the stopped instance to
 * the record. Once the processor acts, 40 frames of far end, 20 of near-end
 * tone over the echo and 60 of silence follow. A field swap fails only on
 * unequal values, so the case prints whether the seven values differ, with no
 * assert. Two values can match: the wait count reads 7 after one engage run,
 * and the not-echo-like count follows the far-end start tick. On the
 * tc_lec_hold_up ring model, tap 0 outgrows tap 80 (tc_lec_lobe_match), so the
 * peak tap reads 0 and an omitted copy passes. The case "a canceller that ran
 * against a hybrid reports what it learned" proves the peak copy at tap 80.
 */
static void case_lec_verdicts(void)
{
	struct udsp_ec_nlms_info info;
	struct udsp_lec_stats ls;
	struct udsp_chan_cfg ccfg;
	struct udsp_ec *ec;
	struct tc_stream w;
	bool stopped;
	struct eh t;

	CASE("the record carries the processor verdicts and the peak tap of "
	     "the filter");
	if (!tc_lec_hold_up(&t, &w)) {
		CHECK(false);
		return;
	}
	CHECK(tc_lec_converge(&t, &ls));
	CHECK(tc_lec_nlp_wait(&t, &ls));
	/* The three phases spread the counts: the far end alone puts the
	 * active count well above the others, the near-end tone adds double
	 * talk, and a far-end pause puts the quiet count above the rest. */
	CHECK(tc_lec_frames_wait(&t, &ls, 40u));
	atomic_store(&tc_be.tone_hz, 1000u);
	CHECK(tc_lec_frames_wait(&t, &ls, 20u));
	atomic_store(&tc_be.tone_hz, 0u);
	CHECK(udsp_tone_stop(t.s, 0) == UDSP_OK);
	CHECK(tc_lec_frames_wait(&t, &ls, 60u));
	/* A SYNC-TICK op: the filter runs no frame from its tick on, so the
	 * counters stand still from the last frame the filter ran. */
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.ec_nlp_enable = true;
	ccfg.ec_tail_ms = 32;
	CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
	eh_wait_ms(&t, 50);
	CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
	printf("# frames %u: active %u wait %u erle %u unlike %u dt %u "
	       "quiet %u, coef_peak_tap %u\n", ls.frames, ls.nlp_active_frames,
	       ls.nlp_wait_frames, ls.nlp_erle_frames, ls.nlp_unlike_frames,
	       ls.nlp_dt_frames, ls.nlp_quiet_frames, ls.coef_peak_tap);
	/*
	 * dsp/lec.h allows the read of the instance from the thread that owns
	 * it alone. The tick still feeds the reference into a switched-off
	 * canceller (core/chan.c chan_lec_run), so the read waits for a
	 * stopped engine. A stop job that answers 0 joined the engine thread
	 * first (core/udsp.c udsp_engine_destroy). The slic thread releases
	 * the job count after the join, and tc_quiesce acquires it. The
	 * restart gives the stream back its engine for the teardown.
	 */
	memset(&info, 0, sizeof(info));
	stopped = tc_stop(&t);
	CHECK(stopped);
	tc_quiesce(&t);
	ec = atomic_load(&t.s->chan[0].ec);
	CHECK(ec != NULL);
	if (ec && stopped)
		udsp_ec_nlms_info_get(ec, &info);
	CHECK(tc_start(&t));
	CHECK_UINT(ls.frames, info.frames);
	CHECK_UINT(ls.nlp_active_frames, info.nlp_active_frames);
	CHECK_UINT(ls.nlp_wait_frames, info.nlp_wait_frames);
	CHECK_UINT(ls.nlp_erle_frames, info.nlp_erle_frames);
	CHECK_UINT(ls.nlp_unlike_frames, info.nlp_unlike_frames);
	CHECK_UINT(ls.nlp_dt_frames, info.nlp_dt_frames);
	CHECK_UINT(ls.nlp_quiet_frames, info.nlp_quiet_frames);
	CHECK_UINT(ls.coef_peak_tap, info.coef_peak_tap);
	/* The channel asks for the processor on every frame, so each frame
	 * carries one verdict. */
	CHECK_UINT(ls.nlp_active_frames + ls.nlp_wait_frames +
		   ls.nlp_erle_frames + ls.nlp_unlike_frames +
		   ls.nlp_dt_frames + ls.nlp_quiet_frames, ls.frames);
	CHECK(ls.nlp_active_frames > 0u);
	CHECK(ls.nlp_wait_frames >= 7u);
	CHECK(ls.nlp_erle_frames > 0u);
	CHECK(ls.nlp_unlike_frames > 0u);
	CHECK(ls.nlp_dt_frames > 0u);
	CHECK(ls.nlp_quiet_frames > 0u);
	{
		const uint32_t seven[7] = {
			ls.nlp_active_frames, ls.nlp_wait_frames,
			ls.nlp_erle_frames, ls.nlp_unlike_frames,
			ls.nlp_dt_frames, ls.nlp_quiet_frames, ls.coef_peak_tap,
		};

		printf("# verdicts distinct: %s\n",
		       tc_lec_same_count(seven, 7u) ? "no" : "yes");
	}
	CHECK_UINT(ls.loop_open, 0);
	CHECK(tc_stream_down(&t, &w, 0));
	tc_down(&t);
}

/* Polls the record until it shows the loop the case set: loop_open 1 after an
 * open, ec_active 1 after a close. No hook report marks an open this early,
 * because the release verdict waits for release_min_ms. */
static bool tc_lec_loop_wait(struct eh *t, struct udsp_lec_stats *ls,
			     bool opened)
{
	return eh_lec_state_wait(t, ls, !opened, opened);
}

static bool tc_within_pct(uint32_t v, uint32_t ref, unsigned pct)
{
	uint64_t lo = (uint64_t)ref * (100u - pct);
	uint64_t hi = (uint64_t)ref * (100u + pct);
	uint64_t x = (uint64_t)v * 100u;

	return x >= lo && x <= hi;
}

/*
 * The 32 ms tail of tc_lec_hold_up spans 3.2 frames of 10 ms, so the fourth
 * frame after a skip ends the first tail.
 */
#define TC_LEC_TAIL_FRAMES	4u
/*
 * The dial tone of the far end repeats every 10 frames, because 350 Hz and
 * 440 Hz share a period of 100 ms. After a skip of a whole number of periods,
 * the stale window equals the true one, and a case cannot tell the feed from
 * its absence. A skip of 35 frames lies half a period from each whole number.
 */
#define TC_LEC_SKIP_MS		350u
/*
 * The open of the flash case. The slic thread samples the hook, so the open
 * that the gate sees can differ from the wait by one frame. An open of 34 or
 * 36 frames hides a missing feed too: the double-talk detector holds the
 * frames after the close while the window is stale, so the energy and the
 * ERLE stand. An open of 37, 38 or 39 frames shows the missing feed.
 */
#define TC_LEC_OPEN_MS		380u

static void tc_lec_skip_wait(struct eh *t, uint32_t from_ms, uint32_t skip_ms)
{
	uint32_t now = eh_now_ms();

	if (now < from_ms + skip_ms)
		eh_wait_ms(t, from_ms + skip_ms - now);
}

/*
 * The canceller on a loop that opens and closes. The tick skips the filter
 * while the loop is open, so the record keeps the call that it describes, and
 * the uplink still sends. A flash resumes on the taps that it left. A re-time
 * decided while the loop is open waits for the first closed frame. A canceller
 * created on an open loop reads fresh. The cases share one handle, converged
 * on the model hybrid of tc_lec_hold_up.
 */
static void case_lec_hook(void)
{
	struct udsp_lec_stats frozen, at_open, ls;
	uint32_t sent, from_ms, open;
	struct tc_stream w;
	struct eh t;

	CASE("an open loop stops the canceller and leaves its record standing");
	if (!tc_lec_hold_up(&t, &w)) {
		CHECK(false);
		return;
	}
	CHECK(tc_lec_converge(&t, &ls));
	eh_wait_ms(&t, 1000);
	from_ms = eh_now_ms();
	tc_hook_set(&t, false);
	CHECK(tc_lec_loop_wait(&t, &frozen, true));
	tc_stream_drain(&w);
	sent = w.rtp_got.data;
	tc_stream_wait(&t, &w, 300);
	CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
	printf("# open: frames %u hold %u erle_mdb %d energy %u, rtp +%u\n",
	       ls.frames, ls.hold_frames, ls.erle_mdb, ls.coef_energy_micro,
	       w.rtp_got.data - sent);
	CHECK_UINT(ls.frames, frozen.frames);
	CHECK_UINT(ls.hold_frames, frozen.hold_frames);
	CHECK(ls.erle_mdb == frozen.erle_mdb);
	CHECK_UINT(ls.coef_energy_micro, frozen.coef_energy_micro);
	CHECK_UINT(ls.ec_active, 0);
	CHECK_UINT(ls.nlp_enabled, 0);
	CHECK_UINT(ls.loop_open, 1);
	CHECK_UINT(ls.passthrough, UDSP_PASSTHROUGH_OFF);
	/* The gate holds the filter and not the frame. */
	CHECK(w.rtp_got.data >= sent + 5u);

	CASE("a flash keeps the taps");
	tc_lec_skip_wait(&t, from_ms, TC_LEC_OPEN_MS);
	open = (eh_now_ms() - from_ms) / 10u;
	tc_hook_set(&t, true);
	CHECK(tc_lec_loop_wait(&t, &ls, false));
	printf("# flash: open of %u frames, frames %u energy %u against %u, "
	       "erle_mdb %d against %d, retimes %u\n", open, ls.frames,
	       ls.coef_energy_micro, frozen.coef_energy_micro, ls.erle_mdb,
	       frozen.erle_mdb, ls.retimes);
	CHECK_UINT(ls.loop_open, 0);
	CHECK_UINT(ls.retimes, 0);
	/* The filter adapted on the frames since the close, so the energy is
	 * compared within a margin and not for equality. */
	CHECK(tc_within_pct(ls.coef_energy_micro, frozen.coef_energy_micro, 1u));
	CHECK(ls.frames > frozen.frames);

	CASE("a re-time decided on an open loop lands on the first closed frame");
	tc_hook_set(&t, false);
	CHECK(tc_lec_loop_wait(&t, &at_open, true));
	/* A six period hold moves the loop from 240 to 400. */
	CHECK(tc_lec_window_watch(&t, NULL));
	CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
	printf("# retime open: frames %u retimes %u ref %u\n", ls.frames,
	       ls.retimes, ls.ref_delay_samples);
	/* The reference fields are live and can read 320 already. A gate
	 * behind the generation compare resets the filter here. */
	CHECK_UINT(ls.retimes, 0);
	CHECK_UINT(ls.frames, at_open.frames);
	tc_hook_set(&t, true);
	CHECK(tc_lec_loop_wait(&t, &ls, false));
	printf("# retime closed: frames %u against %u, retimes %u ref %u\n",
	       ls.frames, at_open.frames, ls.retimes, ls.ref_delay_samples);
	CHECK_UINT(ls.retimes, 1);
	CHECK(ls.frames < at_open.frames);
	CHECK_UINT(ls.ref_delay_samples, 320u);

	CASE("a fresh canceller on an open loop reads fresh");
	tc_hook_set(&t, false);
	CHECK(tc_lec_loop_wait(&t, &ls, true));
	/* A hook release does not release the canceller. The stop of the last
	 * stream does (core/stream.c stream_lec_release). */
	CHECK(tc_stream_down(&t, &w, 0));
	eh_wait_ms(&t, 100);
	CHECK(tc_stream_up(&t, &w, 0));
	CHECK(tc_lec_settled(&t, &ls));
	printf("# fresh: frames %u ec_active %u loop_open %u\n", ls.frames,
	       ls.ec_active, ls.loop_open);
	CHECK_UINT(ls.frames, 0);
	CHECK_UINT(ls.ec_active, 0);
	CHECK_UINT(ls.loop_open, 1);
	CHECK(tc_stream_down(&t, &w, 0));
	tc_down(&t);
}

/*
 * The filter adapts on the frames after the skip, so the energy and the ERLE
 * are compared within a margin and not for equality. A stale window drops the
 * ERLE by tens of dB on the first tail, so a margin of 1 dB separates the two.
 */
static void tc_lec_resume_check(struct eh *t, const char *what,
				const struct udsp_lec_stats *before,
				uint32_t from_ms)
{
	uint32_t skip = (eh_now_ms() - from_ms) / 10u;
	struct udsp_lec_stats ls = *before;

	CHECK(tc_lec_frames_wait(t, &ls, TC_LEC_TAIL_FRAMES));
	printf("# %s: skip of %u frames, frames %u against %u, energy %u "
	       "against %u, erle_mdb %d against %d\n", what, skip, ls.frames,
	       before->frames, ls.coef_energy_micro, before->coef_energy_micro,
	       ls.erle_mdb, before->erle_mdb);
	CHECK_UINT(ls.ec_active, 1);
	CHECK_UINT(ls.retimes, before->retimes);
	CHECK(tc_within_pct(ls.coef_energy_micro, before->coef_energy_micro,
			    1u));
	CHECK(ls.erle_mdb >= before->erle_mdb - 1000);
}

/*
 * The canceller across the modem kind, across a configuration that switches
 * it off and across a fax profile, on a closed loop. The tick feeds each
 * skipped frame into the history of the filter, so the first tail after the
 * skip adapts against the true window. The cases share one handle, converged
 * on the model hybrid of tc_lec_hold_up.
 *
 * The modem case stores the latch of the tone disabler, so that it skips on
 * the modem kind alone. No detector is armed, so the latch stands until the
 * caller clear.
 */
static void case_lec_skip(void)
{
	struct udsp_lec_stats before, ls;
	struct udsp_chan_cfg ccfg;
	struct tc_stream w;
	uint32_t from_ms;
	struct eh t;

	CASE("a modem kind keeps the history of the canceller in step");
	if (!tc_lec_hold_up(&t, &w)) {
		CHECK(false);
		return;
	}
	CHECK(tc_lec_converge(&t, &ls));
	eh_wait_ms(&t, 1000);
	from_ms = eh_now_ms();
	atomic_store(&t.s->chan[0].fax_bypass_kind, UDSP_PASSTHROUGH_MODEM);
	CHECK(eh_lec_state_wait(&t, &before, 0, 0));
	CHECK_UINT(before.passthrough, UDSP_PASSTHROUGH_MODEM);
	tc_lec_skip_wait(&t, from_ms, TC_LEC_SKIP_MS);
	CHECK(udsp_line_lec_latch_clear(t.s, 0) == UDSP_OK);
	tc_lec_resume_check(&t, "modem", &before, from_ms);

	CASE("a canceller switched off and on keeps its history in step");
	/* A failure above leaves a filter that must converge again first. */
	eh_wait_ms(&t, 1000);
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.ec_nlp_enable = true;
	ccfg.ec_tail_ms = 32;
	CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
	from_ms = eh_now_ms();
	CHECK(eh_lec_state_wait(&t, &before, 0, 0));
	CHECK_UINT(before.passthrough, UDSP_PASSTHROUGH_OFF);
	tc_lec_skip_wait(&t, from_ms, TC_LEC_SKIP_MS);
	ccfg.ec_enable = true;
	CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
	tc_lec_resume_check(&t, "switch", &before, from_ms);

	CASE("a fax profile keeps the history of the canceller in step");
	/*
	 * The profile sets the modem kind. Each call deactivates the stream
	 * for a tick or two (stream.c stream_quiesce), two on the host model.
	 * The quiesce of the profile off comes right before the first tail,
	 * and a stale window of two frames moves the energy and the ERLE.
	 */
	eh_wait_ms(&t, 1000);
	from_ms = eh_now_ms();
	CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH, UDSP_SEL_PCMU,
			       0) == UDSP_OK);
	CHECK(eh_lec_state_wait(&t, &before, 0, 0));
	CHECK_UINT(before.passthrough, UDSP_PASSTHROUGH_MODEM);
	tc_lec_skip_wait(&t, from_ms, TC_LEC_SKIP_MS);
	CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_OFF, 0, 0) == UDSP_OK);
	tc_lec_resume_check(&t, "profile", &before, from_ms);
	CHECK(tc_stream_down(&t, &w, 0));
	tc_down(&t);
}

#define TC_LEC_SECOND_NEEDLE	"lec second line 0"

/* The words of a SECOND line of the verdict trace that the cases read
 * (slic_thread.c slic_lec_second_log). */
struct tc_lec_second {
	unsigned gen;
	unsigned frame;
	unsigned fired;
	unsigned dt;
	unsigned lost;
};

/* The SECOND lines of line 0 in the log sink, in the order the slic thread
 * wrote them. lines counts every line that carries the needle in the same
 * pass, so a line that does not parse shows as a difference. */
static unsigned tc_lec_seconds_read(struct tc_lec_second *out, unsigned max,
				    unsigned *lines)
{
	unsigned n = 0;

	*lines = 0;
	pthread_mutex_lock(&tc_log.lock);
	for (unsigned i = 0; i < tc_log.n && n < max; i++) {
		struct tc_lec_second *r = &out[n];
		unsigned at, probes;

		if (tc_log.level[i] != UDSP_LOG_DEBUG ||
		    !strstr(tc_log.line[i], TC_LEC_SECOND_NEEDLE))
			continue;
		(*lines)++;
		if (sscanf(tc_log.line[i],
			   "lec second line 0 gen %u at %u frame %u fired %u "
			   "dt %u probes %u lost %u", &r->gen, &at, &r->frame,
			   &r->fired, &r->dt, &probes, &r->lost) == 7)
			n++;
	}
	pthread_mutex_unlock(&tc_log.lock);

	return n;
}

/* The VERDICT lines of line 0 in the log sink. read counts the lines whose
 * trust word and stale word parse. untrusted counts the lines with a stale
 * run above 0 under a trust word of 0. The return counts every VERDICT line
 * of the same pass. */
static unsigned tc_lec_stale_read(unsigned *read, unsigned *untrusted)
{
	unsigned n = 0;

	*read = 0;
	*untrusted = 0;
	pthread_mutex_lock(&tc_log.lock);
	for (unsigned i = 0; i < tc_log.n; i++) {
		const char *trust = strstr(tc_log.line[i], " trusted ");
		const char *run = strstr(tc_log.line[i], " run ");
		unsigned trusted, dt_run, stale;

		if (tc_log.level[i] != UDSP_LOG_DEBUG ||
		    !strstr(tc_log.line[i], "lec verdict line 0"))
			continue;
		n++;
		if (!trust || !run ||
		    sscanf(trust, " trusted %u", &trusted) != 1 ||
		    sscanf(run, " run %u stale %u", &dt_run, &stale) != 2)
			continue;
		(*read)++;
		if (!trusted && stale)
			(*untrusted)++;
	}
	pthread_mutex_unlock(&tc_log.lock);

	return n;
}

/*
 * The verdict trace of the canceller under the slic log bit, on the converged
 * handle of tc_lec_hold_up. The far end alone prints a SECOND line every
 * hundred frames. A near-end tone prints a VERDICT line, and a later SECOND
 * counts the verdict frames and the hangover behind them. Each VERDICT line
 * carries the stale word, and the word reads 0 where the trust word reads 0.
 * The trace stops when the bit clears. The log sink keeps 32 lines, so each
 * phase clears the sink and reads the line it needs first.
 */
static void case_lec_trace(void)
{
	struct tc_lec_second sec[TC_LOG_MAX];
	struct tc_lec_second base = { 0 };
	struct udsp_lec_stats ls;
	struct tc_stream w;
	struct eh t;
	unsigned lines;
	unsigned head;
	unsigned n;

	CASE("the verdict trace prints a second line every hundred frames "
	     "under the slic log bit");
	if (!tc_lec_hold_up(&t, &w)) {
		CHECK(false);
		return;
	}
	CHECK(tc_lec_converge(&t, &ls));
	udsp_debug_set(t.s, UDSP_DEBUG_SLIC_LOG);
	tc_log_reset();
	CHECK(tc_log_wait_count(UDSP_LOG_DEBUG, TC_LEC_SECOND_NEEDLE, 2u,
				3000));
	n = tc_lec_seconds_read(sec, TC_LOG_MAX, &lines);
	printf("# far end: %u second lines of %u, %u verdict lines, %u debug "
	       "lines\n", n, lines,
	       tc_log_count_needle(UDSP_LOG_DEBUG, "lec verdict line 0"),
	       tc_log_count(UDSP_LOG_DEBUG));
	CHECK(n >= 2u);
	CHECK_UINT(n, lines);
	for (unsigned i = 0; i < n; i++) {
		CHECK_UINT(sec[i].frame % 100u, 0);
		CHECK(sec[i].fired <= sec[i].dt);
		CHECK_UINT(sec[i].lost, 0);
		CHECK_UINT(sec[i].gen, sec[0].gen);
		if (i && sec[i].gen == sec[i - 1u].gen)
			CHECK_UINT(sec[i].frame - sec[i - 1u].frame, 100);
	}
	if (n)
		base = sec[n - 1u];

	CASE("the verdict trace prints a verdict line on near-end sound and "
	     "counts it in the next second");
	tc_log_reset();
	CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
	atomic_store(&tc_be.tone_hz, 1000u);
	CHECK(tc_lec_frames_wait(&t, &ls, 20u));
	CHECK(tc_log_wait(UDSP_LOG_DEBUG, "lec verdict line 0", 1000));
	atomic_store(&tc_be.tone_hz, 0u);
	printf("# near end: %u verdict lines, %u with hold 0\n",
	       tc_log_count_needle(UDSP_LOG_DEBUG, "lec verdict line 0"),
	       tc_log_count_needle(UDSP_LOG_DEBUG, "hold 0 arm"));
	CHECK(tc_log_count_needle(UDSP_LOG_DEBUG, "hold 0 arm") > 0u);
	{
		unsigned read, untrusted;
		unsigned verdicts = tc_lec_stale_read(&read, &untrusted);

		printf("# near end: the stale word reads on %u of %u verdict "
		       "lines, %u with a stale run under no trust\n", read,
		       verdicts, untrusted);
		CHECK(verdicts > 0u);
		CHECK_UINT(read, verdicts);
		CHECK_UINT(untrusted, 0);
	}
	/* A SECOND of a tone frame can still wait in the ring for a tick, so
	 * the sink clears once the frames after the tone ran. */
	CHECK(tc_lec_frames_wait(&t, &ls, 5u));
	tc_log_reset();
	CHECK(tc_log_wait(UDSP_LOG_DEBUG, TC_LEC_SECOND_NEEDLE, 3000));
	n = tc_lec_seconds_read(sec, 1u, &lines);
	CHECK_UINT(n, 1);
	if (n) {
		unsigned fired = sec[0].fired - base.fired;
		unsigned dt = sec[0].dt - base.dt;

		printf("# next second: frame %u, since frame %u: fired %u "
		       "dt %u\n", sec[0].frame, base.frame, fired, dt);
		/* The counters start over with a new gen, so a delta holds
		 * inside one gen only. */
		CHECK_UINT(sec[0].gen, base.gen);
		CHECK(fired >= 1u);
		CHECK(dt > fired);
	}

	CASE("the verdict trace stops when the bit clears");
	udsp_debug_set(t.s, 0);
	tc_log_reset();
	/* A tick that loaded the bit before the clear can still push, so the
	 * head is read two ticks later. */
	eh_wait_ms(&t, 20);
	head = atomic_load(&t.s->chan[0].lec_traces.head);
	atomic_store(&tc_be.tone_hz, 1000u);
	eh_wait_ms(&t, 300);
	atomic_store(&tc_be.tone_hz, 0u);
	CHECK_UINT(tc_log_count_needle(UDSP_LOG_DEBUG, "lec verdict"), 0);
	CHECK_UINT(atomic_load(&t.s->chan[0].lec_traces.head), head);
	CHECK(tc_stream_down(&t, &w, 0));
	tc_down(&t);
}

/* --- the canceller capture ----------------------------------------------- */

/* A capture record reaches the replay of bench/lec_replay.h by a copy of its
 * fields. The public values must therefore be the values of the .lecr
 * format. */
_Static_assert((unsigned)UDSP_LEC_CAPTURE_RUN == (unsigned)LECR_KIND_RUN &&
	       (unsigned)UDSP_LEC_CAPTURE_FEED == (unsigned)LECR_KIND_FEED &&
	       (unsigned)UDSP_LEC_CAPTURE_END == (unsigned)LECR_KIND_END,
	       "the capture kinds are the .lecr kinds");
_Static_assert((unsigned)UDSP_LEC_CAPTURE_NEW == (unsigned)LECR_FLAG_NEW &&
	       (unsigned)UDSP_LEC_CAPTURE_RESET == (unsigned)LECR_FLAG_RESET &&
	       (unsigned)UDSP_LEC_CAPTURE_FREEZE ==
	       (unsigned)LECR_FLAG_FREEZE &&
	       (unsigned)UDSP_LEC_CAPTURE_NLP == (unsigned)LECR_FLAG_NLP &&
	       (unsigned)UDSP_LEC_CAPTURE_FIRED == (unsigned)LECR_FLAG_FIRED &&
	       (unsigned)UDSP_LEC_CAPTURE_DT == (unsigned)LECR_FLAG_DT &&
	       (unsigned)UDSP_LEC_CAPTURE_ACTIVE ==
	       (unsigned)LECR_FLAG_ACTIVE &&
	       (unsigned)UDSP_LEC_CAPTURE_PROBING ==
	       (unsigned)LECR_FLAG_PROBING &&
	       (unsigned)UDSP_LEC_CAPTURE_SKIP_OPEN ==
	       (unsigned)LECR_FLAG_SKIP_OPEN &&
	       (unsigned)UDSP_LEC_CAPTURE_SKIP_MODEM ==
	       (unsigned)LECR_FLAG_SKIP_MODEM &&
	       (unsigned)UDSP_LEC_CAPTURE_SKIP_OFF ==
	       (unsigned)LECR_FLAG_SKIP_OFF &&
	       (unsigned)UDSP_LEC_CAPTURE_SKIP_INACTIVE ==
	       (unsigned)LECR_FLAG_SKIP_INACTIVE &&
	       (unsigned)UDSP_LEC_CAPTURE_SKIPPED ==
	       (unsigned)LECR_FLAG_SKIPPED &&
	       (unsigned)UDSP_LEC_CAPTURE_SKIP_SYNTHETIC ==
	       (unsigned)LECR_FLAG_SKIP_SYNTHETIC,
	       "the capture flags are the .lecr flags");
_Static_assert((unsigned)UDSP_LEC_CAPTURE_VERDICT_OFF ==
	       (unsigned)LECR_VERDICT_OFF &&
	       (unsigned)UDSP_LEC_CAPTURE_VERDICT_QUIET ==
	       (unsigned)LECR_VERDICT_QUIET &&
	       (unsigned)UDSP_LEC_CAPTURE_VERDICT_DT ==
	       (unsigned)LECR_VERDICT_DT &&
	       (unsigned)UDSP_LEC_CAPTURE_VERDICT_UNLIKE ==
	       (unsigned)LECR_VERDICT_UNLIKE &&
	       (unsigned)UDSP_LEC_CAPTURE_VERDICT_ERLE ==
	       (unsigned)LECR_VERDICT_ERLE &&
	       (unsigned)UDSP_LEC_CAPTURE_VERDICT_PASS ==
	       (unsigned)LECR_VERDICT_PASS &&
	       UDSP_LEC_CAPTURE_NOISE_UNMEASURED == LECR_NOISE_UNMEASURED,
	       "the capture verdicts are the .lecr verdicts");

#define TC_CAP_MAX	8192u
#define TC_CAP_SKIP	(UDSP_LEC_CAPTURE_SKIP_OPEN | \
			 UDSP_LEC_CAPTURE_SKIP_MODEM | \
			 UDSP_LEC_CAPTURE_SKIP_OFF | \
			 UDSP_LEC_CAPTURE_SKIP_INACTIVE | \
			 UDSP_LEC_CAPTURE_SKIP_SYNTHETIC)

/* The records a case drained, in the order of the ring. over counts the
 * records that found the buffer full. */
struct tc_cap {
	udsp_t *s;
	struct udsp_lec_capture *rec;
	unsigned n;
	unsigned over;
};

static bool tc_cap_open(struct tc_cap *k)
{
	memset(k, 0, sizeof(*k));
	k->rec = calloc(TC_CAP_MAX, sizeof(*k->rec));

	return k->rec != NULL;
}

static void tc_cap_drain(void *arg)
{
	struct tc_cap *k = arg;
	struct udsp_lec_capture spill;
	unsigned got;

	do {
		if (k->n < TC_CAP_MAX) {
			got = udsp_lec_capture_read(k->s, &k->rec[k->n],
						    TC_CAP_MAX - k->n);
			k->n += got;
		} else {
			got = udsp_lec_capture_read(k->s, &spill, 1);
			k->over += got;
		}
	} while (got);
}

/* Waits ms of engine time and drains on every pass, so a long wait cannot
 * fill the ring. */
static void tc_cap_wait(struct eh *t, struct tc_cap *k, uint32_t ms)
{
	eh_wait_each(t, ms, tc_cap_drain, k);
}

static unsigned tc_cap_bit(struct eh *t)
{
	return (atomic_load(&t->s->debug) & UDSP_DEBUG_LEC_CAPTURE) ? 1u : 0u;
}

/* The records of the line from index from on, of the kind, whose flags under
 * mask read want. */
static unsigned tc_cap_count(const struct tc_cap *k, unsigned from,
			     unsigned line, unsigned kind, unsigned mask,
			     unsigned want)
{
	unsigned n = 0;

	for (unsigned i = from; i < k->n; i++) {
		const struct udsp_lec_capture *r = &k->rec[i];

		n += r->line == line && r->kind == kind &&
		     (r->flags & mask) == want;
	}

	return n;
}

/* The index of the first record of the line from index from on that has
 * the kind and every flag of mask, or k->n. */
static unsigned tc_cap_find(const struct tc_cap *k, unsigned from,
			    unsigned line, unsigned kind, unsigned mask)
{
	for (unsigned i = from; i < k->n; i++) {
		const struct udsp_lec_capture *r = &k->rec[i];

		if (r->line == line && r->kind == kind &&
		    (r->flags & mask) == mask)
			return i;
	}

	return k->n;
}

/*
 * The records that break the shape of udsp.h. samples is 80 or 160. A FEED
 * carries exactly one skip flag, and a RUN or an END carries none. SKIPPED
 * stands exactly when skipped is not 0, and never on NEW or on an END.
 */
static unsigned tc_cap_shape_bad(const struct tc_cap *k)
{
	unsigned bad = 0;

	for (unsigned i = 0; i < k->n; i++) {
		const struct udsp_lec_capture *r = &k->rec[i];
		unsigned skip = r->flags & TC_CAP_SKIP;
		bool one = skip && !(skip & (skip - 1u));
		bool feed = r->kind == UDSP_LEC_CAPTURE_FEED;
		bool skipped = (r->flags & UDSP_LEC_CAPTURE_SKIPPED) != 0;

		bad += (r->samples != 80u && r->samples != 160u) ||
		       (feed ? !one : skip != 0) ||
		       skipped != (r->skipped != 0) ||
		       (r->skipped && (r->kind == UDSP_LEC_CAPTURE_END ||
				       (r->flags & UDSP_LEC_CAPTURE_NEW)));
	}

	return bad;
}

/*
 * The jump rule of udsp.h on one line, for a case with one record per tick.
 * The tick of each record less the tick of the record of the line before it
 * is skipped + lost_before + 1. NEW starts over, so its jump is not checked.
 * An END carries no skipped count, so a case checks one only where no skip
 * comes before it. lost takes the sum of the loss counts.
 */
static unsigned tc_cap_loss_bad(const struct tc_cap *k, unsigned line,
				uint64_t *lost)
{
	const struct udsp_lec_capture *prev = NULL;
	unsigned bad = 0;

	*lost = 0;
	for (unsigned i = 0; i < k->n; i++) {
		const struct udsp_lec_capture *r = &k->rec[i];
		bool fresh = r->kind != UDSP_LEC_CAPTURE_END &&
			     (r->flags & UDSP_LEC_CAPTURE_NEW);

		if (r->line != line)
			continue;
		*lost += r->lost_before;
		if (prev && !fresh &&
		    r->tick - prev->tick != r->skipped + r->lost_before + 1u)
			bad++;
		prev = r;
	}

	return bad;
}

static void tc_cap_lecr(const struct udsp_lec_capture *c,
			struct lecr_record *l)
{
	size_t bytes = c->samples * sizeof(int16_t);

	memset(l, 0, sizeof(*l));
	l->stamp_ns = c->stamp_ns;
	l->tick = c->tick;
	l->lost_before = c->lost_before;
	l->frames = c->frames;
	l->hold_frames = c->hold_frames;
	l->erle_mdb = c->erle_mdb;
	l->echo_gain_micro = c->echo_gain_micro;
	l->coef_energy_micro = c->coef_energy_micro;
	l->ref_delay_samples = c->ref_delay_samples;
	l->gen = c->gen;
	l->flags = c->flags;
	l->samples = c->samples;
	l->tail_ms = c->tail_ms;
	l->noise_ddbov = c->noise_ddbov;
	l->probes = c->probes;
	l->diverge_resets = c->diverge_resets;
	l->line = c->line;
	l->kind = c->kind;
	l->nlp_verdict = c->nlp_verdict;
	l->hold_left = c->hold_left;
	l->skipped = c->skipped;
	if (c->kind == UDSP_LEC_CAPTURE_END)
		return;
	memcpy(l->ref, c->ref, bytes);
	memcpy(l->mic, c->mic, bytes);
	if (c->kind == UDSP_LEC_CAPTURE_RUN)
		memcpy(l->out, c->out, bytes);
}

/* state_diffs counts the RUN records whose state fields differ from the
 * reading of the replayed instance (tc_cap_state_differs). */
struct tc_cap_seen {
	unsigned runs;
	unsigned feeds;
	unsigned diffs;
	unsigned state_diffs;
	unsigned out_delta;
};

/* The replay on the host runs the code of the tick on the same host. Every
 * state field of a RUN therefore matches the replayed instance bit for
 * bit. */
static bool tc_cap_state_differs(const struct lecr_record *live,
				 const struct lecr_record *got)
{
	return live->hold_frames != got->hold_frames ||
	       live->hold_left != got->hold_left ||
	       live->erle_mdb != got->erle_mdb ||
	       live->echo_gain_micro != got->echo_gain_micro ||
	       live->coef_energy_micro != got->coef_energy_micro ||
	       live->noise_ddbov != got->noise_ddbov ||
	       live->probes != got->probes ||
	       live->diverge_resets != got->diverge_resets ||
	       live->tail_ms != got->tail_ms;
}

static void tc_cap_seen_add(void *ctx, const struct lecr_record *rec,
			    const struct lecr_replay *r,
			    const struct lecr_step *st,
			    const struct lecr_agree *a, bool closed)
{
	struct tc_cap_seen *seen = ctx;

	if (!rec || !st->replayed)
		return;
	if (rec->kind == LECR_KIND_FEED) {
		seen->feeds++;
		return;
	}
	seen->runs++;
	seen->diffs += st->diff ? 1u : 0u;
	seen->state_diffs += tc_cap_state_differs(rec, &r->got) ? 1u : 0u;
	if (st->out_delta > seen->out_delta)
		seen->out_delta = st->out_delta;
}

/* The records of the line from index from on, through the .lecr writer into
 * memory, back through the reader and into the replay. */
static bool tc_cap_replay(const struct tc_cap *k, unsigned line,
			  unsigned from, struct lecr_agree *a,
			  struct tc_cap_seen *seen)
{
	struct lecr_replay r;
	struct lecr_record rec;
	struct lecr_file file;
	char *buf = NULL;
	size_t size = 0;
	FILE *f = open_memstream(&buf, &size);
	bool ok;

	memset(seen, 0, sizeof(*seen));
	lecr_agree_init(a, 0);
	if (!f || from >= k->n)
		return false;
	ok = !lecr_write_open(f, &file, (uint16_t)line,
			      k->rec[from].samples * LECR_TICKS_PER_SECOND);
	for (unsigned i = from; ok && i < k->n; i++) {
		if (k->rec[i].line != line)
			continue;
		tc_cap_lecr(&k->rec[i], &rec);
		ok = !lecr_write_record(f, &file, &rec);
	}
	ok = ok && !lecr_write_close(f, &file);
	if (fclose(f))
		ok = false;
	f = ok ? fmemopen(buf, size, "rb") : NULL;
	if (f) {
		lecr_replay_init(&r, false);
		ok = !lecr_stream_replay(f, &file, &r, a, tc_cap_seen_add,
					 seen);
		fclose(f);
	}
	free(buf);

	return ok && f;
}

/* The index of the first NEW record of the line from index from on, a RUN or
 * a FEED, or k->n. */
static unsigned tc_cap_new_find(const struct tc_cap *k, unsigned from,
				unsigned line)
{
	unsigned run = tc_cap_find(k, from, line, UDSP_LEC_CAPTURE_RUN,
				   UDSP_LEC_CAPTURE_NEW);
	unsigned feed = tc_cap_find(k, from, line, UDSP_LEC_CAPTURE_FEED,
				    UDSP_LEC_CAPTURE_NEW);

	return run < feed ? run : feed;
}

/* The replay of line 0 from its first NEW at index start or after it. Every
 * RUN replays bit for bit, its state fields included, and every FEED is
 * taken, with no loss and no gap. The answer is the count of the jumps with
 * SKIPPED that the replay accepted. */
static unsigned tc_cap_replay_check(const struct tc_cap *k, unsigned start,
				    const char *what)
{
	unsigned from = tc_cap_new_find(k, start, 0);
	struct tc_cap_seen seen;
	struct lecr_agree a;

	CHECK(tc_cap_replay(k, 0, from, &a, &seen));
	printf("# replay %s: %u runs, %u feeds, %u resets, %u differ, %u "
	       "states differ, out delta %u, gaps %u, losses %u, skips %u of "
	       "%llu periods\n", what, seen.runs, seen.feeds, a.resets,
	       seen.diffs, seen.state_diffs, seen.out_delta, a.gaps, a.losses,
	       a.skip_marks, (unsigned long long)a.skip_periods);
	CHECK_UINT(seen.runs,
		   tc_cap_count(k, from, 0, UDSP_LEC_CAPTURE_RUN, 0, 0));
	CHECK_UINT(seen.feeds,
		   tc_cap_count(k, from, 0, UDSP_LEC_CAPTURE_FEED, 0, 0));
	CHECK_UINT(seen.diffs, 0);
	CHECK_UINT(seen.state_diffs, 0);
	CHECK_UINT(a.frames_bad, 0);
	CHECK_UINT(a.gaps + a.losses, 0);
	CHECK_UINT(lecr_agree_result(&a), LECR_PASS);

	return a.skip_marks;
}

/* The periods the engine skipped since its start. */
static uint64_t tc_cap_drops(struct eh *t)
{
	struct engine_stats st;

	tc_engine_stats(t, &st);

	return st.trim_drops + st.capture_drops;
}

/* Waits, with a drain, until the engine skipped more periods than from. */
static bool tc_cap_skip_wait(struct eh *t, struct tc_cap *k, uint64_t from)
{
	for (unsigned i = 0; i < 150u; i++) {
		tc_cap_wait(t, k, 20);
		if (tc_cap_drops(t) > from)
			return true;
	}

	return false;
}

/*
 * One skip of periods on the back end of tc_up: the next read reports them
 * overwritten, and the engine calls the skip hook with them. With k NULL the
 * wait drains nothing, so a full ring stays full.
 */
static bool tc_cap_skip_force(struct eh *t, struct tc_cap *k, unsigned periods)
{
	uint64_t from = tc_cap_drops(t);
	uint64_t end = eh_mono_us() + 2000000u;

	atomic_store_explicit(&tc_be.skip_periods, periods,
			      memory_order_release);
	while (tc_cap_drops(t) < from + periods) {
		if (eh_mono_us() >= end)
			return false;
		if (k)
			tc_cap_drain(k);
		eh_sleep_ms(1);
	}

	return tc_cap_drops(t) == from + periods;
}

/* The calibrated echo loop of the engine (engine.h loop_ref), or -1 while
 * the window of the calibration is open. */
static int32_t tc_cap_loop(struct eh *t)
{
	struct engine_stats st;

	tc_engine_stats(t, &st);

	return st.delay_calibrated ? st.loop_ref : -1;
}

/* Waits, with a drain, until the engine calibrated an echo loop other than
 * before. The reference rule then re-times on its next tick. */
static bool tc_cap_loop_wait(struct eh *t, struct tc_cap *k, int32_t before)
{
	for (unsigned i = 0; i < 100u; i++) {
		tc_cap_wait(t, k, 50);
		if (tc_cap_loop(t) >= 0 && tc_cap_loop(t) != before)
			return true;
	}

	return false;
}

/* The re-times of line 0 in the record, 0 while no record stands. */
static uint32_t tc_cap_retimes(struct eh *t)
{
	struct udsp_lec_stats ls;

	return udsp_line_lec_stats(t->s, 0, &ls) == UDSP_OK ? ls.retimes : 0u;
}

static bool tc_cap_retime_wait(struct eh *t, struct tc_cap *k,
			       uint32_t before)
{
	for (unsigned i = 0; i < 100u; i++) {
		tc_cap_wait(t, k, 50);
		if (tc_cap_retimes(t) > before)
			return true;
	}

	return false;
}

/* Two lines with one stream each: line 0 off hook and running, line 1
 * configured and never started. Every tick then gives one record of each
 * line. */
static bool tc_cap_two_up(struct eh *t, struct tc_stream *w0,
			  struct tc_stream *w1)
{
	struct udsp_chan_cfg ccfg;

	if (!tc_up(t, "USA") || !eh_hook_seize(t))
		return false;
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.ec_enable = true;
	ccfg.ec_nlp_enable = true;
	ccfg.ec_tail_ms = 32;
	if (udsp_chan_config(t->s, 0, &ccfg) != UDSP_OK ||
	    udsp_chan_config(t->s, 1, &ccfg) != UDSP_OK)
		return false;

	return tc_stream_up(t, w0, 0) && tc_stream_config(t, w1, 1);
}

/* The type of the command the parked tick has not taken yet, or -1 once the
 * wait for one expires. The tick is parked, so the tail stands still. */
static int tc_ctl_pending_wait(struct eh *t)
{
	uint64_t end = eh_mono_us() + 2000000u;
	unsigned tail = atomic_load(&t->s->ctl_ring.tail);

	while (tail == atomic_load(&t->s->ctl_ring.head)) {
		if (eh_mono_us() >= end)
			return -1;
		eh_sleep_ms(1);
	}

	return t->s->ctl_ring.slot[tail].type;
}

struct tc_cap_stop {
	udsp_t *s;
	enum udsp_status st;
};

static void *tc_cap_stop_run(void *arg)
{
	struct tc_cap_stop *p = arg;

	p->st = udsp_stream_stop(p->s, 0, 0);

	return NULL;
}

/*
 * The drop of an instance while losses of its line stand. The ring is full,
 * and the tick is parked between two ticks. Another thread stops the stream
 * of line 0. The gate lets one tick through per command, so the ticks after
 * the deactivation lose their records too. Once the release waits, room
 * takes one record out, and the next tick runs the release first. The END
 * then finds the free slot and takes the count. Without room, the END finds
 * the ring full and is lost. A non-NULL occupant goes into the retire
 * mailbox of line 0 while the release waits, so the release finds the
 * mailbox occupied. The engine thread can still read the pipe of the gate
 * after the disarm, so the caller closes the gate after the handle is down.
 */
static bool tc_cap_end_drop(struct eh *t, struct tc_cap *k,
			    struct eh_gate *gate, bool room,
			    struct udsp_ec *occupant)
{
	struct tc_cap_stop stop = { .s = t->s, .st = UDSP_FATAL };
	bool ok = false;
	pthread_t th;
	int type;

	if (!eh_gate_park(gate))
		return false;
	if (pthread_create(&th, NULL, tc_cap_stop_run, &stop)) {
		eh_gate_disarm(gate);
		return false;
	}
	while ((type = tc_ctl_pending_wait(t)) >= 0 &&
	       type != UDSP_CMD_LEC_RELEASE) {
		if (!eh_gate_step(gate, 1))
			break;
	}
	ok = type == UDSP_CMD_LEC_RELEASE;
	if (ok && room)
		ok = k->n < TC_CAP_MAX &&
		     udsp_lec_capture_read(t->s, &k->rec[k->n], 1) == 1u;
	if (ok && room)
		k->n++;
	if (ok && occupant)
		atomic_store(&t->s->chan[0].ec_retired, occupant);
	if (ok)
		ok = eh_gate_step(gate, 1);
	eh_gate_disarm(gate);
	pthread_join(th, NULL);

	return ok && stop.st == UDSP_OK;
}

struct tc_cap_config {
	udsp_t *s;
	struct udsp_media m;
	int rtp;
	int rtcp;
	enum udsp_status st;
};

static void *tc_cap_config_run(void *arg)
{
	struct tc_cap_config *p = arg;

	p->st = udsp_stream_config(p->s, 0, 0, &p->m, p->rtp, p->rtcp);

	return NULL;
}

/*
 * A configuration of stream 0 of line 0 whose prepare fails after it created
 * the canceller. The tick is parked, so the instance takes no frame before
 * the prepare releases it. The release is then the one command that waits,
 * and one tick runs it before the lines. st takes the answer of the
 * configuration. The caller closes the gate, as for tc_cap_end_drop.
 */
static bool tc_cap_prep_fail(struct eh *t, struct eh_gate *gate,
			     struct tc_stream *w, enum udsp_status *st)
{
	struct tc_cap_config cfg = { .s = t->s, .st = UDSP_OK };
	struct tc_stream_desc d;
	bool ok;
	pthread_t th;

	if (!tc_stream_pairs(w) || !eh_gate_park(gate))
		return false;
	tc_stream_desc_default(&d);
	tc_media_fill(&cfg.m, &d);
	cfg.rtp = w->rtp[0];
	cfg.rtcp = w->rtcp[0];
	atomic_store(&t->s->prep_fail, (uint_least32_t)ENOMEM);
	if (pthread_create(&th, NULL, tc_cap_config_run, &cfg)) {
		atomic_store(&t->s->prep_fail, 0u);
		eh_gate_disarm(gate);
		return false;
	}
	ok = tc_ctl_pending_wait(t) == UDSP_CMD_LEC_RELEASE &&
	     eh_gate_step(gate, 1);
	eh_gate_disarm(gate);
	pthread_join(th, NULL);
	*st = cfg.st;

	return ok;
}

struct tc_cap_disarm {
	udsp_t *s;
	_Atomic int rc;
	_Atomic bool done;
};

static void *tc_cap_disarm_run(void *arg)
{
	struct tc_cap_disarm *p = arg;

	atomic_store(&p->rc, udsp_lec_capture_enable(p->s, false));
	atomic_store(&p->done, true);

	return NULL;
}

/*
 * A disarm on another thread while the tick is parked. blocked says that the
 * call had not returned 50 ms later. type is the command that it left for the
 * tick, or -1 when it did not wait. With expire, the tick stays parked until
 * the call returns, so the wait of the call runs out. One tick through the
 * gate then releases it, and rc takes its answer. The caller closes the gate,
 * as for tc_cap_end_drop.
 */
static bool tc_cap_disarm_gated(struct eh *t, struct eh_gate *gate,
				bool expire, bool *blocked, int *type, int *rc)
{
	struct tc_cap_disarm d = { .s = t->s, .rc = 1, .done = false };
	uint64_t end;
	bool ok;
	pthread_t th;

	*blocked = false;
	*type = -1;
	if (!eh_gate_park(gate))
		return false;
	if (pthread_create(&th, NULL, tc_cap_disarm_run, &d)) {
		eh_gate_disarm(gate);
		return false;
	}
	for (unsigned i = 0; i < 50u && !atomic_load(&d.done); i++)
		eh_sleep_ms(1);
	*blocked = !atomic_load(&d.done);
	if (*blocked)
		*type = tc_ctl_pending_wait(t);
	end = eh_mono_us() + 5000000u;
	while (expire && !atomic_load(&d.done) && eh_mono_us() < end)
		eh_sleep_ms(1);
	ok = eh_gate_step(gate, 1);
	eh_gate_disarm(gate);
	pthread_join(th, NULL);
	*rc = atomic_load(&d.rc);

	return ok;
}

/* The index of the first record of the line from index from on, or k->n. */
static unsigned tc_cap_first_find(const struct tc_cap *k, unsigned from,
				  unsigned line)
{
	for (unsigned i = from; i < k->n; i++) {
		if (k->rec[i].line == line)
			return i;
	}

	return k->n;
}

/* The index of the record of the line of record i before it, or k->n. */
static unsigned tc_cap_prev_find(const struct tc_cap *k, unsigned i)
{
	for (unsigned j = i; j-- > 0;) {
		if (k->rec[j].line == k->rec[i].line)
			return j;
	}

	return k->n;
}

/* The tick jump to record i from the record of its line before it, or 0
 * when no record of the line comes before it. */
static uint64_t tc_cap_jump(const struct tc_cap *k, unsigned i)
{
	unsigned j = tc_cap_prev_find(k, i);

	return j < k->n ? k->rec[i].tick - k->rec[j].tick : 0;
}

/* The indexes of the records of the line from index from on that carry
 * SKIPPED, up to max. The answer is the count of such records. */
static unsigned tc_cap_skipped_find(const struct tc_cap *k, unsigned from,
				    unsigned line, unsigned *idx, unsigned max)
{
	unsigned n = 0;

	for (unsigned i = from; i < k->n; i++) {
		const struct udsp_lec_capture *r = &k->rec[i];

		if (r->line != line || !(r->flags & UDSP_LEC_CAPTURE_SKIPPED))
			continue;
		if (n < max)
			idx[n] = i;
		n++;
	}

	return n;
}

/* The records of the line from index from on whose stamp_ns lies outside
 * [lo_ns, hi_ns], or does not rise with tick. */
static unsigned tc_cap_stamp_bad(const struct tc_cap *k, unsigned from,
				 unsigned line, uint64_t lo_ns, uint64_t hi_ns)
{
	const struct udsp_lec_capture *prev = NULL;
	unsigned bad = 0;

	for (unsigned i = from; i < k->n; i++) {
		const struct udsp_lec_capture *r = &k->rec[i];

		if (r->line != line)
			continue;
		bad += r->stamp_ns < lo_ns || r->stamp_ns > hi_ns ||
		       (prev && (r->tick <= prev->tick ||
				 r->stamp_ns <= prev->stamp_ns));
		prev = r;
	}

	return bad;
}

/* The RUN and FEED records of the line from index from on whose
 * ref_delay_samples is not delay. */
static unsigned tc_cap_delay_bad(const struct tc_cap *k, unsigned from,
				 unsigned line, uint32_t delay)
{
	unsigned bad = 0;

	for (unsigned i = from; i < k->n; i++) {
		const struct udsp_lec_capture *r = &k->rec[i];

		bad += r->line == line && r->kind != UDSP_LEC_CAPTURE_END &&
		       r->ref_delay_samples != delay;
	}

	return bad;
}

/*
 * The FEED records of the line from index from on that do not carry the
 * near-end tone in mic, on a line at rate. The tone of tc_tone_fill is 1000
 * Hz. A frame of it peaks between 7300 and 8000. cos(pi / 8) of 8000 is 7391
 * at 8 kHz, and cos(pi / 16) of it is 7846 at 16 kHz.
 * Each inner sample x[n] also meets x[n - 1] + x[n + 1] = c x[n], within the
 * truncation of the three samples. Here c is 2 cos(2 pi 1000 / rate). A plane
 * copied short of its samples therefore fails at the cut.
 */
static unsigned tc_cap_tone_bad(const struct tc_cap *k, unsigned from,
				unsigned line, unsigned rate)
{
	double c = 2.0 * cos(2.0 * M_PI * 1000.0 / (double)rate);
	unsigned bad = 0;

	for (unsigned i = from; i < k->n; i++) {
		const struct udsp_lec_capture *r = &k->rec[i];
		const int16_t *x = r->mic;
		bool off = r->samples != rate / 100u;
		int peak = 0;

		if (r->line != line || r->kind != UDSP_LEC_CAPTURE_FEED)
			continue;
		for (unsigned j = 0; j < r->samples && !off; j++) {
			if (abs(x[j]) > peak)
				peak = abs(x[j]);
			if (j && j + 1u < r->samples)
				off = fabs(x[j - 1] + x[j + 1] - c * x[j]) >
				      4.0;
		}
		bad += off || peak < 7300 || peak > 8000;
	}

	return bad;
}

/*
 * A re-time while a new instance takes FEED records only. The instance has
 * not run, so its first RUN seeds the generation of the reference rule and
 * carries no RESET. The bias moves two more periods down, to -320. The drift
 * opens an episode as the -160 of the re-time phase does, and the window
 * after it latches a shorter loop.
 */
static void tc_cap_feed_retime_check(struct eh *t, struct tc_stream *w,
				     struct tc_cap *k)
{
	unsigned from, first;
	int32_t loop;

	CHECK(tc_stream_down(t, w, 0));
	tc_cap_wait(t, k, 100);
	from = k->n;
	CHECK(tc_stream_config(t, w, 0));
	tc_cap_wait(t, k, 50);
	loop = tc_cap_loop(t);
	atomic_store_explicit(&tc_be.delay_bias, -320L, memory_order_release);
	CHECK(tc_cap_loop_wait(t, k, loop));
	tc_cap_wait(t, k, 50);
	CHECK(udsp_stream_start(t->s, 0, 0) == UDSP_OK);
	tc_cap_wait(t, k, 300);
	first = tc_cap_find(k, from, 0, UDSP_LEC_CAPTURE_RUN, 0);
	printf("# re-time before the first RUN: loop %d to %d, %u FEED "
	       "records\n", (int)loop, (int)tc_cap_loop(t), first - from);
	CHECK(from < first && first < k->n);
	if (!(from < first && first < k->n))
		return;
	CHECK_UINT(k->rec[from].kind, UDSP_LEC_CAPTURE_FEED);
	CHECK_UINT(k->rec[from].flags & UDSP_LEC_CAPTURE_NEW,
		   UDSP_LEC_CAPTURE_NEW);
	CHECK_UINT(k->rec[first].flags & (UDSP_LEC_CAPTURE_NEW |
					  UDSP_LEC_CAPTURE_RESET), 0);
	CHECK_UINT(k->rec[first].gen, k->rec[from].gen);
	CHECK_UINT(k->rec[first].frames, 1);
	CHECK_UINT(tc_cap_count(k, from, 0, UDSP_LEC_CAPTURE_RUN,
				UDSP_LEC_CAPTURE_RESET,
				UDSP_LEC_CAPTURE_RESET), 0);
	CHECK_UINT(tc_cap_retimes(t), 0);
}

/* The prepare of tc_cap_prep_fail on line 0, which holds no canceller. Line
 * 0 gives no record at all, while the tick feeds line 1 on. */
static void tc_cap_unseen_check(struct eh *t, struct tc_cap *k,
				struct eh_gate *gate)
{
	enum udsp_status st = UDSP_OK;
	struct tc_stream w;
	unsigned from;

	tc_cap_wait(t, k, 50);
	from = k->n;
	CHECK(atomic_load(&t->s->chan[0].ec) == NULL);
	CHECK(tc_cap_prep_fail(t, gate, &w, &st));
	tc_stream_fds_close(&w);
	tc_cap_wait(t, k, 50);
	CHECK_UINT(st, UDSP_TRANSIENT);
	CHECK(atomic_load(&t->s->chan[0].ec) == NULL);
	CHECK_UINT(tc_cap_first_find(k, from, 0), k->n);
	CHECK(tc_cap_first_find(k, from, 1) < k->n);
}

/*
 * An instance of line 0 whose END finds the ring full. The next instance
 * starts once the ring is drained, and its NEW record carries the count. The
 * replay closes the lineage before the NEW as lossy and replays the new
 * lineage whole. The answer is the index of the NEW record.
 */
static unsigned tc_cap_end_lost_check(struct eh *t, struct tc_stream *w,
				      struct tc_cap *k, struct eh_gate *gate)
{
	struct tc_cap_seen seen;
	struct lecr_agree a;
	unsigned from, again;

	tc_cap_wait(t, k, 50);
	from = k->n;
	CHECK(tc_stream_up(t, w, 0));
	tc_cap_wait(t, k, 200);
	/* No drain, so the ring is full when the release runs. */
	eh_wait_ms(t, 300);
	CHECK(tc_cap_end_drop(t, k, gate, false, NULL));
	tc_stream_fds_close(w);
	tc_cap_wait(t, k, 50);
	CHECK(tc_stream_up(t, w, 0));
	tc_cap_wait(t, k, 200);
	again = tc_cap_new_find(k, tc_cap_new_find(k, from, 0) + 1u, 0);
	CHECK(again < k->n);
	if (again >= k->n)
		return k->n;
	printf("# lost END: the NEW at tick %llu carries lost_before %u\n",
	       (unsigned long long)k->rec[again].tick,
	       k->rec[again].lost_before);
	CHECK(k->rec[again].lost_before > 0u);
	CHECK_UINT(tc_cap_count(k, from, 0, UDSP_LEC_CAPTURE_END, 0, 0), 0);
	CHECK(tc_cap_replay(k, 0, tc_cap_new_find(k, from, 0), &a, &seen));
	CHECK_UINT(a.instances, 2);
	CHECK_UINT(a.losses, 1);
	CHECK_UINT(a.gaps, 0);
	CHECK_UINT(a.evaluated, 1);
	CHECK_UINT(seen.runs, tc_cap_count(k, from, 0, UDSP_LEC_CAPTURE_RUN,
					   0, 0));
	CHECK_UINT(seen.diffs, 0);
	CHECK_UINT(seen.state_diffs, 0);
	CHECK_UINT(lecr_agree_result(&a), LECR_PASS);

	return again;
}

/*
 * A skip of 3 periods while the ring is full. The first record of each line
 * that fits after the drain carries both counts, and its jump is their sum
 * plus one. The replay from the NEW at index start reads that record as a
 * loss, not as a skip and not as a gap. The NEW carries the count of the
 * lost END, which is the other loss of the replay.
 */
static void tc_cap_skip_loss_check(struct eh *t, struct tc_cap *k,
				   unsigned start)
{
	struct tc_cap_seen seen;
	struct lecr_agree a;
	unsigned from, i;
	uint64_t lost;

	tc_cap_wait(t, k, 50);
	from = k->n;
	eh_wait_ms(t, 300);
	CHECK(tc_cap_skip_force(t, NULL, 3u));
	tc_cap_wait(t, k, 100);
	for (unsigned line = 0; line < 2u; line++) {
		i = tc_cap_first_find(k, from, line);
		while (i < k->n && !k->rec[i].lost_before)
			i = tc_cap_first_find(k, i + 1u, line);
		CHECK(i < k->n);
		if (i >= k->n)
			continue;
		CHECK_UINT(k->rec[i].skipped, 3);
		CHECK_UINT(k->rec[i].flags & UDSP_LEC_CAPTURE_SKIPPED,
			   UDSP_LEC_CAPTURE_SKIPPED);
		CHECK_UINT(tc_cap_jump(k, i), 3u + k->rec[i].lost_before + 1u);
		CHECK_UINT(tc_cap_loss_bad(k, line, &lost), 0);
	}
	CHECK(tc_cap_replay(k, 0, start, &a, &seen));
	CHECK_UINT(a.losses, 2);
	CHECK_UINT(a.gaps, 0);
	CHECK_UINT(a.skip_marks, 0);
}

static bool tc_cap_stream1_config(struct eh *t, struct tc_stream *w)
{
	struct tc_stream_desc d;
	struct udsp_media m;

	if (!tc_stream_pairs(w))
		return false;
	tc_stream_desc_default(&d);
	tc_media_fill(&m, &d);

	return udsp_stream_config(t->s, 0, 1, &m, w->rtp[0], w->rtcp[0]) ==
	       UDSP_OK;
}

/*
 * A canceller created while another stream of line 0 runs. Stream 0 starts
 * while the channel configuration switches the canceller off, so it creates
 * none. The configuration of stream 1 then creates it on an active line, and
 * the first frame of the instance is a RUN. The answer is the index where
 * the records of the instance start.
 */
static unsigned tc_cap_run_first_check(struct eh *t, struct tc_stream *w,
				       struct tc_cap *k)
{
	struct udsp_chan_cfg ccfg;
	struct tc_stream w2;
	unsigned from, i;

	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.ec_nlp_enable = true;
	ccfg.ec_tail_ms = 32;
	CHECK(tc_stream_down(t, w, 0));
	CHECK(udsp_chan_config(t->s, 0, &ccfg) == UDSP_OK);
	CHECK(tc_stream_up(t, w, 0));
	tc_cap_wait(t, k, 50);
	from = k->n;
	ccfg.ec_enable = true;
	CHECK(udsp_chan_config(t->s, 0, &ccfg) == UDSP_OK);
	CHECK(tc_cap_stream1_config(t, &w2));
	tc_cap_wait(t, k, 200);
	CHECK(udsp_stream_stop(t->s, 0, 1) == UDSP_OK);
	tc_stream_fds_close(&w2);
	i = tc_cap_first_find(k, from, 0);
	CHECK(i < k->n);
	if (i >= k->n)
		return from;
	CHECK_UINT(k->rec[i].kind, UDSP_LEC_CAPTURE_RUN);
	CHECK_UINT(k->rec[i].flags & UDSP_LEC_CAPTURE_NEW,
		   UDSP_LEC_CAPTURE_NEW);
	CHECK_UINT(k->rec[i].frames, 1);

	return from;
}

/*
 * Two skips of the engine, 3 and then 5 periods. Exactly two records of each
 * line carry SKIPPED, each with the count of its own skip. The jump before
 * each is its count plus one.
 */
static void tc_cap_skips_check(struct eh *t, struct tc_cap *k)
{
	static const unsigned periods[2] = { 3u, 5u };
	unsigned idx[2];
	unsigned from, n, p;
	uint64_t lost;

	tc_cap_wait(t, k, 50);
	from = k->n;
	for (p = 0; p < 2u; p++) {
		CHECK(tc_cap_skip_force(t, k, periods[p]));
		tc_cap_wait(t, k, 50);
	}
	for (unsigned line = 0; line < 2u; line++) {
		n = tc_cap_skipped_find(k, from, line, idx, 2u);
		CHECK_UINT(n, 2);
		for (p = 0; p < 2u && p < n; p++) {
			CHECK_UINT(k->rec[idx[p]].skipped, periods[p]);
			CHECK_UINT(tc_cap_jump(k, idx[p]), periods[p] + 1u);
		}
		CHECK_UINT(tc_cap_loss_bad(k, line, &lost), 0);
		CHECK_UINT(lost, 0);
	}
}

/*
 * A skip of 4 periods while the bit is clear, and more ticks with the bit
 * clear after it. The first record of each line after the arm carries the
 * count of the skip alone. Its jump is longer than the count explains, so
 * the replay from the NEW at index start reads a gap there.
 */
static void tc_cap_skip_gap_check(struct eh *t, struct tc_cap *k,
				  unsigned start)
{
	struct tc_cap_seen seen;
	struct lecr_agree a;
	unsigned from, i;

	CHECK(udsp_lec_capture_enable(t->s, false) == 0);
	tc_cap_drain(k);
	from = k->n;
	CHECK(tc_cap_skip_force(t, NULL, 4u));
	eh_wait_ms(t, 50);
	CHECK(udsp_lec_capture_enable(t->s, true) == 0);
	tc_cap_wait(t, k, 50);
	for (unsigned line = 0; line < 2u; line++) {
		i = tc_cap_first_find(k, from, line);
		CHECK(i < k->n);
		if (i >= k->n)
			continue;
		CHECK_UINT(k->rec[i].skipped, 4);
		CHECK_UINT(k->rec[i].lost_before, 0);
		CHECK(tc_cap_jump(k, i) > 4u + 1u);
	}
	CHECK(tc_cap_replay(k, 0, start, &a, &seen));
	CHECK_UINT(a.gaps, 1);
	CHECK_UINT(a.skip_marks, 2);
	CHECK_UINT(lecr_agree_result(&a), LECR_FAIL);
}

/*
 * A skip between the END of an instance and the NEW of the next. The NEW
 * record of line 0 carries no count, and the record of line 1 after the skip
 * carries it. The near-end tone stands while the new stream waits, for the
 * check of the FEED records. The answer is the index of the NEW record.
 */
static unsigned tc_cap_skip_new_check(struct eh *t, struct tc_stream *w,
				      struct tc_cap *k)
{
	unsigned idx[2];
	unsigned from, i, n;

	CHECK(tc_stream_down(t, w, 0));
	tc_cap_wait(t, k, 50);
	from = k->n;
	CHECK(tc_cap_skip_force(t, k, 2u));
	atomic_store_explicit(&tc_be.tone_hz, 1000u, memory_order_release);
	CHECK(tc_stream_config(t, w, 0));
	tc_cap_wait(t, k, 100);
	n = tc_cap_skipped_find(k, from, 1, idx, 2u);
	CHECK_UINT(n, 1);
	if (n)
		CHECK_UINT(k->rec[idx[0]].skipped, 2);
	i = tc_cap_new_find(k, from, 0);
	CHECK(i < k->n);
	if (i >= k->n)
		return i;
	CHECK_UINT(k->rec[i].kind, UDSP_LEC_CAPTURE_FEED);
	CHECK_UINT(k->rec[i].skipped, 0);
	CHECK_UINT(k->rec[i].flags & UDSP_LEC_CAPTURE_SKIPPED, 0);

	return i;
}

#define TC_CAP_REUSE_TRIES	8u

/*
 * A new canceller of line 0 at the address of the instance before it. The
 * tracker of the capture must still read it as new, so its first record
 * carries NEW and the next generation. The allocator decides the address, so
 * the case tries a bounded number of times and skips when no try gave the
 * address back. Stream 0 of line 0 is configured and not started.
 */
static void tc_cap_reuse_check(struct eh *t, struct tc_stream *w,
			       struct tc_cap *k)
{
	const struct udsp_ec *before;
	bool reused = false;
	unsigned from = k->n;
	unsigned i, tries;

	for (tries = 0; tries < TC_CAP_REUSE_TRIES && !reused; tries++) {
		tc_cap_wait(t, k, 30);
		from = k->n;
		before = atomic_load(&t->s->chan[0].ec);
		CHECK(tc_stream_down(t, w, 0));
		CHECK(tc_stream_config(t, w, 0));
		tc_cap_wait(t, k, 50);
		reused = atomic_load(&t->s->chan[0].ec) == before;
	}
	printf("# recycled address: %s after %u tries\n",
	       reused ? "seen" : "not seen", tries);
	if (!reused) {
		tu_skip("the allocator gave a new address on every try");
		return;
	}
	i = tc_cap_first_find(k, from, 0);
	CHECK(i < k->n);
	if (i >= k->n)
		return;
	CHECK_UINT(k->rec[i].kind, UDSP_LEC_CAPTURE_END);
	from = i;
	i = tc_cap_first_find(k, i + 1u, 0);
	CHECK(i < k->n);
	if (i >= k->n)
		return;
	CHECK_UINT(k->rec[i].flags & UDSP_LEC_CAPTURE_NEW,
		   UDSP_LEC_CAPTURE_NEW);
	CHECK_UINT(k->rec[i].gen, (uint16_t)(k->rec[from].gen + 1u));
}

/*
 * A release of line 0 whose acknowledgment is lost (udsp_priv.h
 * lec_ack_drop). The stop waits out the SYNC-TICK bound, harvests the
 * mailbox and frees the instance. The END was written before the tick made
 * the instance visible there, so it reads the frames of the record before
 * it. The deactivation of the stop comes first, so the record before the END
 * can be a FEED. Stream 0 of line 0 is configured and not started, and runs
 * again after.
 */
static void tc_cap_lost_ack_check(struct eh *t, struct tc_stream *w,
				  struct tc_cap *k)
{
	unsigned from, i, j;

	CHECK(udsp_stream_start(t->s, 0, 0) == UDSP_OK);
	tc_cap_wait(t, k, 100);
	from = k->n;
	atomic_store(&t->s->lec_ack_drop, true);
	CHECK(tc_stream_down(t, w, 0));
	tc_cap_wait(t, k, 50);
	CHECK(!atomic_load(&t->s->lec_ack_drop));
	CHECK(atomic_load(&t->s->chan[0].ec) == NULL);
	CHECK(atomic_load(&t->s->chan[0].ec_retired) == NULL);
	i = tc_cap_find(k, from, 0, UDSP_LEC_CAPTURE_END, 0);
	CHECK(i < k->n);
	j = i < k->n ? tc_cap_prev_find(k, i) : k->n;
	CHECK(j < k->n);
	if (j < k->n) {
		printf("# lost ack: END at tick %llu reads frames %u, the %s "
		       "before it %u\n", (unsigned long long)k->rec[i].tick,
		       k->rec[i].frames,
		       k->rec[j].kind == UDSP_LEC_CAPTURE_RUN ? "RUN" : "FEED",
		       k->rec[j].frames);
		CHECK(k->rec[i].frames > 0u);
		CHECK_UINT(k->rec[i].frames, k->rec[j].frames);
		CHECK_UINT(k->rec[i].lost_before, 0);
	}
	CHECK(tc_stream_up(t, w, 0));
}

/*
 * A release of line 0 that finds the retire mailbox occupied. The test puts
 * a spare instance there while the parked tick holds the release. The tick
 * then leaves ec live, pushes no END and keeps the tracker. No record of the
 * instance after it carries NEW or a new generation. The stop harvests
 * the spare, and the library frees it. Stream 0 of line 0 runs before and
 * after.
 */
static void tc_cap_occupied_check(struct eh *t, struct tc_stream *w,
				  struct tc_cap *k, struct eh_gate *gate)
{
	const struct udsp_ec *ec;
	struct udsp_ec *spare;
	unsigned from, last, bad = 0;

	tc_cap_wait(t, k, 50);
	from = k->n;
	for (last = from; last-- > 0 && k->rec[last].line != 0;)
		continue;
	CHECK(last < from);
	spare = udsp_ec_nlms.create_rate(32, UDSP_SAMPLE_RATE);
	CHECK(spare != NULL);
	if (last >= from || !spare) {
		if (spare)
			udsp_ec_nlms.destroy(spare);
		return;
	}
	ec = atomic_load(&t->s->chan[0].ec);
	CHECK(ec != NULL);
	CHECK(tc_cap_end_drop(t, k, gate, false, spare));
	tc_stream_fds_close(w);
	CHECK(atomic_load(&t->s->chan[0].ec) == ec);
	CHECK(atomic_load(&t->s->chan[0].ec_retired) == NULL);
	CHECK(tc_stream_up(t, w, 0));
	tc_cap_wait(t, k, 100);
	CHECK(atomic_load(&t->s->chan[0].ec) == ec);
	CHECK_UINT(tc_cap_count(k, from, 0, UDSP_LEC_CAPTURE_END, 0, 0), 0);
	CHECK_UINT(tc_cap_new_find(k, from, 0), k->n);
	CHECK(tc_cap_first_find(k, from, 0) < k->n);
	for (unsigned i = from; i < k->n; i++)
		bad += k->rec[i].line == 0 && k->rec[i].gen != k->rec[last].gen;
	CHECK_UINT(bad, 0);
}

/*
 * One lineage on a 16 kHz line: FEED records while the stream waits, with
 * the near-end tone in mic, then RUN records on the echo of the dial tone,
 * then the END. Each plane holds 160 samples. A copy cut to the 80 samples
 * of a narrowband frame fails the tone check or the replay.
 */
static void tc_cap_wide_check(struct tc_cap *k)
{
	struct udsp_chan_cfg ccfg;
	unsigned from, tone_bad, bad = 0;
	struct tc_stream w;
	struct eh t;

	memset(k->rec, 0, TC_CAP_MAX * sizeof(*k->rec));
	k->n = 0;
	if (!tc_up_rate(&t, "USA", 2, 16000)) {
		CHECK(false);
		return;
	}
	k->s = t.s;
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.ec_enable = true;
	ccfg.ec_nlp_enable = true;
	ccfg.ec_tail_ms = 32;
	CHECK(eh_hook_seize(&t));
	CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
	CHECK(udsp_lec_capture_enable(t.s, true) == 0);
	atomic_store_explicit(&tc_be.tone_hz, 1000u, memory_order_release);
	CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
	CHECK(tc_stream_config(&t, &w, 0));
	tc_cap_wait(&t, k, 100);
	/* The records drained so far came before the tone stops. */
	tone_bad = tc_cap_tone_bad(k, tc_cap_new_find(k, 0, 0), 0, 16000u);
	atomic_store_explicit(&tc_be.tone_hz, 0u, memory_order_release);
	atomic_store(&tc_be.echo_periods, 3u);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	tc_cap_wait(&t, k, 300);
	CHECK(tc_stream_down(&t, &w, 0));
	tc_cap_wait(&t, k, 50);
	atomic_store(&tc_be.echo_periods, 0u);
	CHECK(udsp_lec_capture_enable(t.s, false) == 0);
	tc_cap_drain(k);
	from = tc_cap_new_find(k, 0, 0);
	for (unsigned i = 0; i < k->n; i++)
		bad += k->rec[i].samples != 160u;
	printf("# 16 kHz: %u RUN, %u FEED and %u END records of line 0, %u "
	       "records not at 160 samples\n",
	       tc_cap_count(k, from, 0, UDSP_LEC_CAPTURE_RUN, 0, 0),
	       tc_cap_count(k, from, 0, UDSP_LEC_CAPTURE_FEED, 0, 0),
	       tc_cap_count(k, from, 0, UDSP_LEC_CAPTURE_END, 0, 0), bad);
	CHECK_UINT(bad, 0);
	CHECK(tc_cap_count(k, from, 0, UDSP_LEC_CAPTURE_FEED, 0, 0) >= 5u);
	CHECK(tc_cap_count(k, from, 0, UDSP_LEC_CAPTURE_RUN, 0, 0) >= 20u);
	CHECK_UINT(tc_cap_count(k, from, 0, UDSP_LEC_CAPTURE_END, 0, 0), 1);
	CHECK_UINT(tone_bad, 0);
	CHECK_UINT(tc_cap_shape_bad(k), 0);
	tc_cap_replay_check(k, 0, "16 kHz");
	tc_down(&t);
}

/* The records of the reference check of case_rephase_reference, from the
 * first rendered tick on. */
#define TC_REPHASE_RECS		6u

struct tc_rephase {
	unsigned first;		/* the record of the first rendered tick */
	unsigned checked;	/* records checked from it */
	unsigned synthetic;	/* FEED records of a rendered tick */
	unsigned bad;		/* records off what the refill says */
	uint32_t delay;
};

/* A reference of tone: a tone sample can read zero at a crossing, so a frame
 * of tone can hold a few, and a frame of silence holds nothing else. */
static bool tc_ref_tone(const struct udsp_lec_capture *r)
{
	unsigned zeros = 0;

	for (unsigned i = 0; i < r->samples; i++)
		zeros += r->ref[i] == 0;

	return zeros <= r->samples / 16u;
}

static bool tc_mic_zero(const struct udsp_lec_capture *r)
{
	for (unsigned i = 0; i < r->samples; i++) {
		if (r->mic[i])
			return false;
	}

	return true;
}

/* One record of line 0, the j-th from the first rendered tick: the two
 * rendered ticks feed the canceller with a zero microphone, and the filter
 * runs from the tick of the restart on. Every reference reads tone. */
static void tc_rephase_rec(const struct udsp_lec_capture *r,
			   const struct udsp_lec_capture *a, unsigned j,
			   struct tc_rephase *o)
{
	o->bad += r->tick != a->tick + j || r->ref_delay_samples != o->delay;
	o->bad += !tc_ref_tone(r);
	if (j >= 2u) {
		o->bad += r->kind != UDSP_LEC_CAPTURE_RUN || r->skipped;
		return;
	}
	o->synthetic += r->kind == UDSP_LEC_CAPTURE_FEED &&
			(r->flags & UDSP_LEC_CAPTURE_SKIP_SYNTHETIC);
	o->bad += !tc_mic_zero(r) || (j && r->skipped);
}

/* The records of line 0 from the last one before the hold on: one per tick,
 * at one delay. The first record after the hold carries the skip. */
static void tc_rephase_scan(const struct tc_cap *k, struct tc_rephase *o)
{
	const struct udsp_lec_capture *a;
	unsigned prev = k->n;

	memset(o, 0, sizeof(*o));
	o->first = k->n;
	for (unsigned i = 0; i < k->n; i++) {
		if (k->rec[i].line)
			continue;
		if (k->rec[i].flags & UDSP_LEC_CAPTURE_SKIPPED) {
			o->first = i;
			break;
		}
		prev = i;
	}
	if (o->first == k->n || prev == k->n)
		return;
	a = &k->rec[o->first];
	o->delay = a->ref_delay_samples;
	o->bad += !tc_ref_tone(&k->rec[prev]);
	for (unsigned i = o->first; i < k->n; i++) {
		if (k->rec[i].line)
			continue;
		tc_rephase_rec(&k->rec[i], a, o->checked, o);
		if (++o->checked == TC_REPHASE_RECS)
			break;
	}
}

/* The handle runs on the raw back end, whose clock eh_now_ms does not
 * follow, so the waits poll the line and the engine on the wall clock. */
static bool tc_rephase_tone_wait(struct eh *t, struct udsp_lec_stats *ls)
{
	uint64_t end = eh_mono_us() + 5000000u;

	do {
		eh_sleep_ms(1);
		if (udsp_line_lec_stats(t->s, 0, ls) == UDSP_OK &&
		    ls->frames >= 100u)
			return true;
	} while (eh_mono_us() < end);

	return false;
}

/* Drains the capture until the engine reports the rephase and two passes
 * more. */
static bool tc_rephase_wait(struct eh *t, struct tc_cap *k,
			    struct engine_stats *st)
{
	uint64_t end = eh_mono_us() + 5000000u;
	unsigned after = 0;

	do {
		eh_sleep_ms(1);
		tc_cap_drain(k);
		tc_engine_stats(t, st);
		if (st->rephases && ++after > 2u)
			return true;
	} while (eh_mono_us() < end);

	return false;
}

static void tc_rephase_check(const struct fk_sync_fault *fault)
{
	struct tc_hold_arm dry = { .prof = &fk_an7581, .hold = 6,
				   .jit = true, .phased = true, .extra = 40,
				   .gated = true };
	struct udsp_chan_cfg ccfg;
	struct udsp_lec_stats ls;
	struct engine_stats st;
	struct udsp_config cfg;
	struct tc_rephase o;
	struct tc_stream w;
	struct tc_cap k;
	struct eh t;
	uint64_t hist[2] = { 0, 0 };
	uint64_t pushed;
	bool up;

	if (fault)
		dry.fault = *fault;
	if (!tc_cap_open(&k)) {
		CHECK(false);
		return;
	}
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.ec_enable = true;
	ccfg.ec_tail_ms = 32;
	memset(&ls, 0, sizeof(ls));
	up = tc_hold_open(&t, &cfg, &dry) && eh_hook_seize(&t) &&
	     udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK &&
	     tc_stream_up(&t, &w, 0) &&
	     udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK &&
	     tc_rephase_tone_wait(&t, &ls);
	CHECK(up);
	if (up) {
		/* A few records of tone, then the hold. The records after it
		 * reach the buffer first, so a full one loses none of them. */
		k.s = t.s;
		CHECK(udsp_lec_capture_enable(t.s, true) == 0);
		while (k.n < 4u && tc_ticks_wait(&t, &st, 0))
			tc_cap_drain(&k);
		atomic_store_explicit(&fk_hold_go, true, memory_order_release);
		CHECK(tc_rephase_wait(&t, &k, &st));
		CHECK(udsp_lec_capture_enable(t.s, false) == 0);
		tc_cap_drain(&k);
	}
	tc_engine_stats(&t, &st);
	CHECK(tc_stop(&t));
	/* The stopped tick writes no more frames. The free joins the tick
	 * thread before the model's count is read. */
	for (unsigned i = 0; i < 2u && t.s; i++)
		hist[i] = atomic_load(&t.s->chan[i].hist_frames);
	tc_free(&t);
	tc_stream_fds_close(&w);
	pushed = fk_push_n;
	fk_release();
	tc_rephase_scan(&k, &o);
	printf("# rephase reference: %llu periods pushed, %llu and %llu "
	       "history frames, %u records, first rendered %u, delay %u, %u "
	       "rendered records, %u off\n", (unsigned long long)pushed,
	       (unsigned long long)hist[0], (unsigned long long)hist[1], k.n,
	       o.first, o.delay, o.synthetic, o.bad);
	CHECK_UINT(st.holds, 1);
	CHECK_UINT(st.rephases, 1);
	CHECK_UINT(st.rephase_rendered, 2);
	CHECK_UINT(st.rephase_lead, 0);
	CHECK_UINT(st.trim_drops, 6);
	CHECK_UINT(st.xruns, 0);
	CHECK_UINT(st.again, fault ? 1 : 0);
	CHECK_UINT(pushed - hist[0], 3);
	CHECK_UINT(pushed - hist[1], 3);
	CHECK(o.first < k.n);
	/* The six dropped periods less the two rendered. */
	CHECK_UINT(o.first < k.n ? k.rec[o.first].skipped : 0, 4);
	CHECK_UINT(o.first < k.n ? k.rec[o.first].lost_before : 1, 0);
	CHECK_UINT(o.checked, TC_REPHASE_RECS);
	CHECK(o.delay >= 160u);
	CHECK_UINT(o.synthetic, 2);
	CHECK_UINT(o.bad, 0);
	free(k.rec);
}

/*
 * The canceller history across a rephase, with a dial tone in the canceller of
 * line 0. The hold runs the AN7581 ring dry on the running line
 * (tdm_raw_fake.h fk_hold_gated). The drop renders two periods of the refill,
 * each a tick whose capture record is a FEED with the reason SKIP_SYNTHETIC
 * and a zero microphone. Every reference from the last tick before the hold
 * to four ticks after the restart reads tone, at one delay. Every pushed
 * period after the prefill enters each line's history once.
 */
static void case_rephase_reference(void)
{
	/* The second refill write meets EINTR once and retries in place. */
	const struct fk_sync_fault eintr = { EINTR, true, 1, 0, 0, 0 };

	CASE("a rephase keeps the dial tone in the canceller history through "
	     "its refill");
	tc_rephase_check(NULL);

	CASE("a refill write that a signal interrupts enters the history once");
	tc_rephase_check(&eintr);
}

/*
 * The order check of case_rephase_order. The downlink of line 0 is its own
 * capture a loop delay late, and each capture sample of channel 0 reads
 * 1 + 256 (k % 100) + i at place i of period k (tdm_raw_fake.h fk_sample).
 * So each 8 kHz frame names the period it came from.
 */
#define TC_ORDER_FRAME		80u
#define TC_ORDER_RECS		10u
#define TC_ORDER_FRAMES_MAX	256u

/* The period a frame of the pattern names, or -1 when a sample breaks it. */
static long tc_order_frame(const int16_t *x)
{
	long k;

	if (x[0] < 1 || (x[0] - 1) % 256)
		return -1;
	k = (x[0] - 1) / 256;
	for (unsigned i = 0; i < TC_ORDER_FRAME; i++) {
		if (x[i] != (int16_t)(1 + 256 * k + (long)i))
			return -1;
	}

	return k;
}

/* The whole frames the line played from the hold on, in play order, and the
 * frames it left incomplete after the first. Silence ends a frame. */
static unsigned tc_order_played(long *wk, unsigned max, unsigned *broken)
{
	unsigned n = 0, run = 0;
	long k = -1;

	*broken = 0;
	for (unsigned i = 0; i < fk_play_rec_n; i++) {
		int v = fk_play_rec_v[i];
		long vk = (v - 1) / 256;
		unsigned vi = (unsigned)((v - 1) % 256);

		if (v < 1 || vi == 0 || vk != k || vi != run) {
			*broken += run != 0;
			run = 0;
		}
		if (v < 1 || (!run && vi))
			continue;
		k = vk;
		if (++run < TC_ORDER_FRAME)
			continue;
		if (n < max)
			wk[n++] = k;
		run = 0;
	}

	return n;
}

struct tc_order {
	unsigned first;		/* the record of the first rendered tick */
	unsigned checked;	/* records from it on */
	long rk[TC_ORDER_RECS];	/* the period each reference names */
	unsigned bad;		/* references that break the pattern */
};

static void tc_order_scan(const struct tc_cap *k, struct tc_order *o)
{
	memset(o, 0, sizeof(*o));
	o->first = k->n;
	for (unsigned i = 0; i < k->n; i++) {
		if (!k->rec[i].line &&
		    (k->rec[i].flags & UDSP_LEC_CAPTURE_SKIP_SYNTHETIC)) {
			o->first = i;
			break;
		}
	}
	for (unsigned i = o->first; i < k->n && o->checked < TC_ORDER_RECS;
	     i++) {
		const struct udsp_lec_capture *r = &k->rec[i];

		if (r->line)
			continue;
		o->rk[o->checked] = tc_order_frame(r->ref);
		o->bad += o->rk[o->checked] < 0 ||
			  r->samples != TC_ORDER_FRAME;
		o->checked++;
	}
}

/* The step from period a to period b. The pattern names a period modulo
 * 100, and the case spans far fewer. */
static unsigned tc_order_step(long a, long b)
{
	return (unsigned)(((b - a) % 100 + 100) % 100);
}

/* The steps other than one in a run of periods, and the first of them. */
static unsigned tc_order_jumps(const long *x, unsigned n, unsigned *jump)
{
	unsigned jumps = 0;

	*jump = 0;
	for (unsigned i = 1; i < n; i++) {
		if (tc_order_step(x[i - 1], x[i]) == 1u)
			continue;
		if (!jumps++)
			*jump = tc_order_step(x[i - 1], x[i]);
	}

	return jumps;
}

/* The index of the frame of period k among the first frames the line played
 * from the hold on, where no period repeats, or n when it is not there. */
static unsigned tc_order_find(const long *wk, unsigned n, long k)
{
	for (unsigned i = 0; i < n && i < 50u; i++) {
		if (wk[i] == k)
			return i;
	}

	return n;
}

/*
 * The recovery of case_rephase_order, step by step. The references run: two
 * frames before the hold, the hold's step, the two rendered frames and the
 * frame of the tick whose flush recovered, then that frame again, because
 * the tick after the recovery runs its index again, then one frame a tick.
 * The line played neither rendered frame and the repeated frame once, so its
 * play order is the references without entries 2, 3 and 5. Returns the
 * entries off either rule.
 */
static unsigned tc_order_recovered(const struct tc_order *o, const long *wk,
				   unsigned n, unsigned at, unsigned hold)
{
	static const unsigned steps[TC_ORDER_RECS - 1u] = {
		1, 0, 1, 1, 0, 1, 1, 1, 1,
	};
	unsigned off = 0, w = at;

	for (unsigned i = 1; i < o->checked; i++)
		off += tc_order_step(o->rk[i - 1], o->rk[i]) !=
		       (i == 2u ? hold : steps[i - 1u]);
	for (unsigned i = 0; i < o->checked; i++) {
		if (i == 2u || i == 3u || i == 5u)
			continue;
		off += w >= n || wk[w] != o->rk[i];
		w++;
	}

	return off;
}

/* Drains the capture until the attempt restarted the ring or aborted, and two
 * passes more. */
static bool tc_order_wait(struct eh *t, struct tc_cap *k,
			  struct engine_stats *st)
{
	uint64_t end = eh_mono_us() + 5000000u;
	unsigned after = 0;

	do {
		eh_sleep_ms(1);
		tc_cap_drain(k);
		tc_engine_stats(t, st);
		if ((st->rephases || st->rephase_aborts) && ++after > 2u)
			return true;
	} while (eh_mono_us() < end);

	return false;
}

/* What a variant of case_rephase_order does at the refill. */
enum tc_order_kind {
	TC_ORDER_CLEAN,
	TC_ORDER_RETRY,		/* the second refill write retries */
	TC_ORDER_FLUSH,		/* abort, and a flush write retries */
	TC_ORDER_RECOVER,	/* abort, and a flush write recovers */
};

static void tc_order_check(enum tc_order_kind kind)
{
	static const struct fk_sync_fault faults[4] = {
		[TC_ORDER_RETRY] = { EINTR, true, 1, 1, 0, 0 },
		[TC_ORDER_FLUSH] = { EINTR, true, 0, 4, 0, 0 },
		[TC_ORDER_RECOVER] = { EINTR, true, 0, 3, EIO, 1 },
	};
	struct tc_hold_arm dry = { .prof = &fk_an7581, .hold = 6,
				   .jit = true, .phased = true, .extra = 40,
				   .gated = true, .fault = faults[kind] };
	long wk[TC_ORDER_FRAMES_MAX];
	struct udsp_chan_cfg ccfg;
	struct engine_stats st;
	struct udsp_config cfg;
	struct tc_order o;
	struct tc_stream w;
	unsigned n, broken, at, off = 0;
	unsigned jump, jumps, ref_jump, ref_jumps;
	struct tc_cap k;
	struct eh t;
	bool up;

	if (!tc_cap_open(&k)) {
		CHECK(false);
		return;
	}
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.ec_enable = true;
	ccfg.ec_tail_ms = 32;
	/* The stream is configured, so the line holds a canceller, and not
	 * started, so the loop may run. */
	up = tc_hold_open(&t, &cfg, &dry) && eh_hook_seize(&t) &&
	     udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK &&
	     tc_stream_config(&t, &w, 0) &&
	     udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0, 300) == UDSP_OK &&
	     tc_ticks_wait(&t, &st, 0) &&
	     tc_ticks_wait(&t, &st, st.ticks + 100u);
	CHECK(up);
	if (up) {
		k.s = t.s;
		CHECK(udsp_lec_capture_enable(t.s, true) == 0);
		while (k.n < 4u && tc_ticks_wait(&t, &st, 0))
			tc_cap_drain(&k);
		atomic_store(&fk_play_rec_arm, true);
		atomic_store_explicit(&fk_hold_go, true, memory_order_release);
		CHECK(tc_order_wait(&t, &k, &st));
		atomic_store(&fk_play_rec_arm, false);
		CHECK(udsp_lec_capture_enable(t.s, false) == 0);
		tc_cap_drain(&k);
	}
	tc_engine_stats(&t, &st);
	CHECK(tc_stop(&t));
	tc_free(&t);
	tc_stream_fds_close(&w);
	tc_order_scan(&k, &o);
	n = tc_order_played(wk, TC_ORDER_FRAMES_MAX, &broken);
	at = o.checked ? tc_order_find(wk, n, o.rk[0]) : n;
	for (unsigned i = 0; at < n && i < o.checked; i++)
		off += at + i >= n || wk[at + i] != o.rk[i];
	/* The frames around the hold: the loop plays the capture the hold
	 * lost 300 ms later, outside this window. */
	jumps = tc_order_jumps(wk, at < n ? at + o.checked : n, &jump);
	ref_jumps = tc_order_jumps(o.rk, o.checked, &ref_jump);
	printf("# rephase order %u: %u records from %u, references %ld..%ld "
	       "with %u steps of %u, %u frames played with %u steps of %u, "
	       "%u broken, %u off the play order, %llu rendered, %llu trims, "
	       "%llu aborts, %llu write errors\n", (unsigned)kind, o.checked,
	       o.first, o.checked ? o.rk[0] : -1L,
	       o.checked ? o.rk[o.checked - 1u] : -1L, ref_jumps, ref_jump, n,
	       jumps, jump, broken, off,
	       (unsigned long long)st.rephase_rendered,
	       (unsigned long long)st.trim_drops,
	       (unsigned long long)st.rephase_aborts,
	       (unsigned long long)st.write_errors);
	CHECK_UINT(st.rephase_rendered, 2);
	CHECK_UINT(st.rephase_lead, 0);
	CHECK_UINT(o.checked, TC_ORDER_RECS);
	CHECK_UINT(o.bad, 0);
	/* The references name successive periods but for a step at the hold:
	 * the periods the skip hook passed, and one. The rendered frames
	 * follow it in the history, in the order of their ticks. */
	CHECK_UINT(ref_jump, st.trim_drops - st.rephase_rendered + 1u);
	CHECK_UINT(broken, 0);
	CHECK_UINT(jumps, 1);
	CHECK(at < n);
	if (kind == TC_ORDER_RECOVER) {
		CHECK_UINT(st.rephase_aborts, 1);
		CHECK_UINT(st.write_errors, 1);
		CHECK_UINT(tc_order_recovered(&o, wk, n, at, ref_jump), 0);
		free(k.rec);
		fk_release();
		return;
	}
	CHECK_UINT(ref_jumps, 1);
	CHECK_UINT(st.rephase_aborts, kind == TC_ORDER_FLUSH ? 1u : 0u);
	CHECK_UINT(st.rephases, kind == TC_ORDER_FLUSH ? 0u : 1u);
	/* The line plays the frames in the order of the references, the
	 * refill included, with the same step. */
	CHECK_UINT(jump, ref_jump);
	CHECK_UINT(off, 0);
	free(k.rec);
	fk_release();
}

/*
 * Line 0 plays its own capture 300 ms late (udsp_line_loop), so each frame
 * names the period it came from. Stream 0 is configured and not started, so
 * the canceller takes a reference each tick. Each reference from the first
 * rendered tick on must be a whole frame. The line must play the frames in
 * that order, with one step at the hold. The variants interrupt a refill
 * write or the flush of an abort, or make that flush recover. The recovery
 * discards a refill that the history keeps.
 */
static void case_rephase_order(void)
{
	CASE("a rephase plays and references its refill in the order of its "
	     "ticks");
	tc_order_check(TC_ORDER_CLEAN);

	CASE("a refill write that a signal interrupts keeps the order");
	tc_order_check(TC_ORDER_RETRY);

	CASE("an abort whose flush write a signal interrupts keeps the order");
	tc_order_check(TC_ORDER_FLUSH);

	CASE("an abort whose flush write recovers keeps the reference order "
	     "and plays no refill");
	tc_order_check(TC_ORDER_RECOVER);
}

/* What the line played from the hold on: the silent intervals, where the
 * transmit DMA stood still, and the longest run of zero samples it played. */
struct tc_line {
	unsigned gaps;
	unsigned long silent;		/* frames of the gaps */
	unsigned long dry_at;		/* the first gap */
	unsigned long restart_at;
	unsigned zero_run;
};

static void tc_line_scan(struct tc_line *l)
{
	unsigned run = 0;

	memset(l, 0, sizeof(*l));
	for (unsigned i = 0; i < fk_play_rec_n; i++) {
		run = fk_play_rec_v[i] ? 0 : run + 1u;
		if (run > l->zero_run)
			l->zero_run = run;
		if (!i || fk_play_rec_at[i] == fk_play_rec_at[i - 1] + 1u)
			continue;
		if (!l->gaps++) {
			l->dry_at = fk_play_rec_at[i - 1] + 1u;
			l->restart_at = fk_play_rec_at[i];
		}
		l->silent += fk_play_rec_at[i] - fk_play_rec_at[i - 1] - 1u;
	}
}

/*
 * The line across a rephase, on the phased AN7581 clock at 8 kHz. Line 0
 * plays a dial tone, and a hold of six periods and 40 frames starts on the
 * running line. The model records what the transmit DMA plays from the hold
 * on. The line goes silent once: from the instant the queue runs out inside
 * the hold to the restart. The restart comes on the first capture completion
 * after the hold, so the silence is the dry time plus the drain wait. The
 * refill and everything after it play the tone, with no run of zero samples.
 */
static void case_rephase_line(void)
{
	struct tc_hold_arm dry = { .prof = &fk_an7581, .hold = 6,
				   .jit = true, .phased = true, .extra = 40,
				   .gated = true };
	struct engine_stats st;
	struct udsp_config cfg;
	struct tc_line l;
	struct eh t;
	bool up;

	CASE("a rephase leaves the dial tone silent for the dry time and the "
	     "drain wait alone");
	up = tc_hold_open(&t, &cfg, &dry) && eh_hook_seize(&t) &&
	     udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK &&
	     tc_ticks_wait(&t, &st, 0) &&
	     tc_ticks_wait(&t, &st, st.ticks + 100u);
	CHECK(up);
	if (up) {
		atomic_store(&fk_play_rec_arm, true);
		atomic_store_explicit(&fk_hold_go, true, memory_order_release);
		do {
			CHECK(tc_ticks_wait(&t, &st, st.ticks + 50u));
		} while (!st.rephases && st.ticks < 1000000u);
		CHECK(tc_ticks_wait(&t, &st, st.ticks + 100u));
	}
	/* The record ends before the stop, which ends the tone. */
	atomic_store(&fk_play_rec_arm, false);
	tc_engine_stats(&t, &st);
	CHECK(tc_stop(&t));
	tc_free(&t);
	tc_line_scan(&l);
	printf("# rephase line: %u frames played, dry at %lu, hold end %lu, "
	       "restart %lu, %u silent intervals of %lu frames, longest zero "
	       "run %u\n", fk_play_rec_n, l.dry_at, fk_hold_end_at,
	       l.restart_at, l.gaps, l.silent, l.zero_run);
	CHECK_UINT(st.rephases, 1);
	CHECK_UINT(st.rephase_rendered, 2);
	CHECK(fk_play_rec_n > 1000u);
	CHECK_UINT(l.gaps, 1);
	/* The queue runs out inside the hold, and the restart waits for the
	 * next capture completion after it, one period at most. */
	CHECK(l.dry_at < fk_hold_end_at);
	CHECK(l.restart_at > fk_hold_end_at);
	CHECK(l.restart_at - fk_hold_end_at <= 80u);
	CHECK_UINT(l.silent, l.restart_at - l.dry_at);
	CHECK(l.zero_run < 8u);
	fk_release();
}

/* A burst of PCMU at ptime 10 from the far end of w, sent at once. */
static void tc_rtp_burst(struct tc_stream *w, unsigned packets, uint16_t *seq,
			 uint32_t *ts)
{
	uint8_t pkt[12 + 80];

	memset(pkt, 0, sizeof(pkt));
	pkt[0] = 0x80;
	pkt[8] = 0x51;
	pkt[9] = 0x50;
	pkt[10] = 0x12;
	pkt[11] = 0x34;
	memset(pkt + 12, 0x7f, 80);
	for (unsigned i = 0; i < packets; i++) {
		pkt[2] = (uint8_t)(*seq >> 8);
		pkt[3] = (uint8_t)*seq;
		pkt[4] = (uint8_t)(*ts >> 24);
		pkt[5] = (uint8_t)(*ts >> 16);
		pkt[6] = (uint8_t)(*ts >> 8);
		pkt[7] = (uint8_t)*ts;
		(*seq)++;
		*ts += 80u;
		if (send(w->rtp[1], pkt, sizeof(pkt),
			 MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
			return;
	}
}

/* Waits, 5 s at most, for the jitter buffer of stream 0 to play a frame. */
static bool tc_jb_played_wait(struct eh *t)
{
	uint64_t end = eh_mono_us() + 5000000u;
	struct udsp_jb_stats js;

	do {
		if (udsp_stream_jb_stats(t->s, 0, 0, &js) == UDSP_OK &&
		    js.frames_out)
			return true;
		eh_sleep_ms(1);
	} while (eh_mono_us() < end);

	return false;
}

/*
 * The skipped periods of a hold reach the receive side of a running stream on
 * either part, whatever the rephase does. The rows: the MediaTek trim, the
 * AN7581 drain, a withheld read of the grid variant, and an aborted attempt.
 * The far end sends a burst into the stream as the hold opens. The stream's
 * jitter buffer pays each skipped period, by a move of its clock or a wait,
 * and accounts for every slot. The two periods a rephase renders play as
 * ticks instead. The tick runs free on this model, so the case reads no unit
 * of the burst.
 */
static void case_hold_receive(void)
{
	static const char *const names[4] = {
		"mt7986 trim", "an7581 rephase", "an7581 withhold",
		"an7581 abort",
	};
	const struct tc_hold_arm arms[4] = {
		{ .prof = &fk_mt7986, .hold = 6, .silence = true,
		  .gated = true },
		{ .prof = &fk_an7581, .hold = 6, .jit = true, .phased = true,
		  .extra = 40, .gated = true },
		{ .prof = &fk_an7581, .hold = 6, .jit = true, .phased = true,
		  .extra = 80, .gated = true, .grid = true },
		{ .prof = &fk_an7581, .hold = 6, .jit = true, .phased = true,
		  .extra = 40, .gated = true,
		  .fault = { EINTR, true, 0, 3, 0, 0 } },
	};
	struct engine_stats st;
	struct udsp_config cfg;
	struct tc_stream w;
	struct eh t;

	CASE("the skipped periods of a hold reach a stream's jitter buffer on "
	     "either part");
	for (unsigned i = 0; i < 4u; i++) {
		struct udsp_stream *rx = NULL;
		uint64_t paid = 0, skipped;
		unsigned slots = 0;
		uint32_t ts = 160000u;
		uint16_t seq = 1000;
		bool up;

		up = tc_hold_open(&t, &cfg, &arms[i]) &&
		     tc_stream_up(&t, &w, 0) && tc_ticks_wait(&t, &st, 0);
		CHECK(up);
		if (up) {
			rx = stream_at(t.s, 0, 0);
			CHECK(rx != NULL);
			/* Two packets start the timeline before the hold, so
			 * no packet of the burst starts one inside it. */
			tc_rtp_burst(&w, 2, &seq, &ts);
			CHECK(tc_jb_played_wait(&t));
			CHECK(tc_ticks_wait(&t, &st, st.ticks + 50u));
			tc_rtp_burst(&w, 20, &seq, &ts);
			atomic_store_explicit(&fk_hold_go, true,
					      memory_order_release);
			do {
				CHECK(tc_ticks_wait(&t, &st, st.ticks + 50u));
			} while (st.holds && !st.cal_restarts &&
				 st.ticks < 1000000u);
			CHECK(tc_ticks_wait(&t, &st, st.ticks + 50u));
		}
		/* No period is skipped after the episode, so the figures read
		 * before the stop stand; the stop joins the tick thread, so the
		 * jitter buffer stands still after it. */
		tc_engine_stats(&t, &st);
		CHECK(tc_stop(&t));
		if (rx) {
			paid = (uint64_t)rx->jb.skip_units + rx->jb.skip_waits;
			slots = rx->jb.pool_count + rx->jb.count;
			if (rx->jb.loan < UDSP_JB_SLOTS &&
			    (!rx->jb.count || rx->jb.order[0] != rx->jb.loan))
				slots++;
		}
		skipped = st.trim_drops + st.capture_drops;
		printf("# hold receive %s: %llu periods skipped, %llu paid, "
		       "%u of them audio, %llu rendered, %llu rephases, %llu "
		       "withheld, %llu aborts\n", names[i],
		       (unsigned long long)skipped, (unsigned long long)paid,
		       rx ? rx->jb.skip_frames : 0u,
		       (unsigned long long)st.rephase_rendered,
		       (unsigned long long)st.rephases,
		       (unsigned long long)st.rephase_withheld,
		       (unsigned long long)st.rephase_aborts);
		CHECK_UINT(st.holds >= 1u, 1);
		CHECK(skipped > 0u);
		/* The rendered periods are ticks, so the buffer pays the rest. */
		CHECK_UINT(st.rephase_lead, 0);
		CHECK_UINT(paid + st.rephase_rendered, skipped);
		CHECK_UINT(st.rephase_rendered, i ? 2u : 0u);
		CHECK_UINT(slots, UDSP_JB_SLOTS);
		if (i == 0u)
			CHECK_UINT(st.rephase_starts, 0);
		if (i == 1u)
			CHECK_UINT(st.rephases, 1);
		if (i == 2u)
			CHECK(st.rephase_withheld >= 1u);
		if (i == 3u)
			CHECK_UINT(st.rephase_aborts, 1);
		tc_free(&t);
		tc_stream_fds_close(&w);
		fk_release();
	}
}

/*
 * The canceller capture (udsp.h struct udsp_lec_capture) on four handles.
 * On the converged handle of tc_lec_hold_up, each phase of a call replays
 * from its NEW through bench/lec_replay.h. The phases are a frozen start, a
 * processor switched off, an engine skip, a re-time and an open loop. The
 * modem kind, a canceller switched off, a fax profile, the verdict trace and
 * a re-time before the first RUN follow. A two-line handle with a small ring
 * then loses records, drops an instance, loses an END and skips beside a
 * loss. Two more handles check the first RUN, the skip counts, the FEED
 * planes at 8 and 16 kHz, the releases and the disarm.
 */
static void case_lec_capture(void)
{
	struct tc_lec_second sec[TC_LOG_MAX];
	struct eh_gate gate = { { -1, -1 } };
	struct lec_capture_ring *ring;
	struct udsp_lec_capture spill;
	struct udsp_chan_cfg ccfg;
	struct tc_stream w, w1;
	unsigned from, first, i;
	unsigned n, lines, bad;
	unsigned start, head;
	uint32_t retimes;
	uint32_t delay;
	uint64_t t0_ns;
	uint64_t lost;
	uint16_t gen = 0;
	struct tc_cap k;
	struct eh t;
	bool up;

	CASE("the canceller capture pushes nothing while its bit is clear");
	if (!tc_cap_open(&k)) {
		CHECK(false);
		return;
	}
	if (!tc_lec_hold_up(&t, &w)) {
		CHECK(false);
		free(k.rec);
		return;
	}
	k.s = t.s;
	/* The bit alone starts nothing: no ring exists to push into. */
	udsp_debug_set(t.s, UDSP_DEBUG_LEC_CAPTURE);
	eh_wait_ms(&t, 100);
	CHECK(atomic_load(&t.s->lec_cap) == NULL);
	CHECK_UINT(udsp_lec_capture_read(t.s, &spill, 1), 0);

	CASE("a refused capture allocation leaves the bit clear");
	CHECK(udsp_lec_capture_enable(NULL, true) == -EINVAL);
	setenv("UDSP_TEST_LEC_CAPTURE_ALLOC_FAIL", "1", 1);
	CHECK(udsp_lec_capture_enable(t.s, true) == -ENOMEM);
	unsetenv("UDSP_TEST_LEC_CAPTURE_ALLOC_FAIL");
	CHECK(atomic_load(&t.s->lec_cap) == NULL);
	CHECK_UINT(tc_cap_bit(&t), 0);

	CASE("the capture enable allocates the ring once and keeps it on a "
	     "disarm");
	CHECK(udsp_lec_capture_enable(t.s, true) == 0);
	ring = atomic_load(&t.s->lec_cap);
	CHECK(ring != NULL);
	CHECK_UINT(tc_cap_bit(&t), 1);
	/* The production depth, with the variable unset. */
	CHECK_UINT(ring ? ring->mask + 1u : 0u, 1024);
	CHECK(udsp_lec_capture_enable(t.s, false) == 0);
	CHECK_UINT(tc_cap_bit(&t), 0);
	CHECK(atomic_load(&t.s->lec_cap) == ring);
	/* An arm that allocated again would meet the refusal. */
	setenv("UDSP_TEST_LEC_CAPTURE_ALLOC_FAIL", "1", 1);
	CHECK(udsp_lec_capture_enable(t.s, true) == 0);
	unsetenv("UDSP_TEST_LEC_CAPTURE_ALLOC_FAIL");
	CHECK(atomic_load(&t.s->lec_cap) == ring);
	CHECK_UINT(tc_cap_bit(&t), 1);
	CHECK(udsp_lec_capture_enable(t.s, false) == 0);

	CASE("the capture pushes nothing after a disarm");
	tc_cap_drain(&k);
	k.n = 0;
	eh_wait_ms(&t, 200);
	CHECK_UINT(udsp_lec_capture_read(t.s, &spill, 1), 0);

	CASE("a canceller whose first frame is a FEED carries NEW and a new "
	     "generation, and its first RUN carries the same generation");
	CHECK(udsp_lec_capture_enable(t.s, true) == 0);
	t0_ns = eh_mono_us() * 1000u;
	delay = atomic_load(&t.s->lec_delay[0]);
	CHECK(tc_stream_down(&t, &w, 0));
	tc_cap_wait(&t, &k, 100);
	/* The instance that ran when the capture started leaves with an END. */
	CHECK_UINT(tc_cap_count(&k, 0, 0, UDSP_LEC_CAPTURE_END, 0, 0), 1);
	from = k.n;
	/* The latch of the tone disabler holds the new filter from its first
	 * frame, so a replay that ignored FREEZE would adapt from zero taps. No
	 * detector is armed, so the latch stands until the caller clear. */
	atomic_store(&t.s->chan[0].fax_bypass_kind, UDSP_PASSTHROUGH_FAX);
	CHECK(tc_stream_config(&t, &w, 0));
	tc_cap_wait(&t, &k, 50);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
	tc_cap_wait(&t, &k, 300);
	CHECK(udsp_line_lec_latch_clear(t.s, 0) == UDSP_OK);
	tc_cap_wait(&t, &k, 700);
	first = tc_cap_find(&k, from, 0, UDSP_LEC_CAPTURE_RUN, 0);
	CHECK(from < first && first < k.n);
	if (from < first && first < k.n) {
		gen = k.rec[from].gen;
		CHECK_UINT(k.rec[from].kind, UDSP_LEC_CAPTURE_FEED);
		CHECK_UINT(k.rec[from].flags, UDSP_LEC_CAPTURE_NEW |
			   UDSP_LEC_CAPTURE_SKIP_INACTIVE);
		i = tc_cap_find(&k, 0, 0, UDSP_LEC_CAPTURE_END, 0);
		CHECK(i < from);
		if (i < from)
			CHECK_UINT(gen, (uint16_t)(k.rec[i].gen + 1u));
		for (bad = 0, i = from; i < first; i++)
			bad += k.rec[i].gen != gen;
		CHECK_UINT(bad, 0);
		CHECK_UINT(k.rec[first].gen, gen);
		CHECK_UINT(k.rec[first].flags & UDSP_LEC_CAPTURE_NEW, 0);
		CHECK_UINT(k.rec[first].frames, 1);
		/* The fax kind of the latch holds the filter and switches the
		 * processor off. */
		CHECK_UINT(k.rec[first].flags & (UDSP_LEC_CAPTURE_FREEZE |
						 UDSP_LEC_CAPTURE_NLP),
			   UDSP_LEC_CAPTURE_FREEZE);
	}

	CASE("the capture gives one RUN record per canceller frame");
	for (n = 1, bad = 0, i = first + 1u; i < k.n; i++, n++) {
		const struct udsp_lec_capture *r = &k.rec[i];

		if (r->kind != UDSP_LEC_CAPTURE_RUN)
			break;
		bad += r->tick != k.rec[i - 1u].tick + 1u ||
		       r->frames != k.rec[i - 1u].frames + 1u ||
		       r->samples != t.s->chan[0].frame || r->lost_before;
	}
	printf("# %u RUN records in a row, %u out of step\n", n, bad);
	CHECK(n >= 90u);
	CHECK_UINT(bad, 0);

	CASE("each record carries the stamp of its tick and the reference "
	     "delay that the tick applied");
	CHECK_UINT(tc_cap_stamp_bad(&k, from, 0, t0_ns, eh_mono_us() * 1000u),
		   0);
	printf("# reference delay %u\n", delay);
	CHECK(delay > 0u);
	CHECK_UINT(atomic_load(&t.s->lec_delay[0]), delay);
	CHECK_UINT(tc_cap_delay_bad(&k, from, 0, delay), 0);

	CASE("a replay reproduces every frame of the capture bit for bit");
	tc_cap_replay_check(&k, 0, "start");

	CASE("a processor switched off reads in the capture and the replay");
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.ec_enable = true;
	ccfg.ec_tail_ms = 32;
	from = k.n;
	CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
	tc_cap_wait(&t, &k, 300);
	ccfg.ec_nlp_enable = true;
	CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
	tc_cap_wait(&t, &k, 300);
	n = tc_cap_count(&k, from, 0, UDSP_LEC_CAPTURE_RUN,
			 UDSP_LEC_CAPTURE_NLP, 0);
	printf("# processor off: %u RUN records\n", n);
	CHECK(n >= 20u);
	tc_cap_replay_check(&k, 0, "processor off");

	CASE("a skip of the engine gives the next record its count, and the "
	     "replay holds");
	{
		struct engine_stats st;
		uint64_t drops;

		from = k.n;
		tc_engine_stats(&t, &st);
		drops = st.trim_drops + st.capture_drops;
		retimes = tc_cap_retimes(&t);
		/* A six period hold on the ring model. The catch-up trims the
		 * period that the hold left (case_hold), and the skip hook
		 * steps the tick over it. The loop reads 400 after the hold,
		 * so the window after it re-times the canceller too. */
		atomic_store_explicit(&tc_be.hold_periods, 6u,
				      memory_order_release);
		CHECK(tc_cap_skip_wait(&t, &k, drops));
		CHECK(tc_cap_retime_wait(&t, &k, retimes));
		tc_cap_wait(&t, &k, 100);
		tc_engine_stats(&t, &st);
		for (i = from + 1u; i < k.n; i++)
			if (k.rec[i].flags & UDSP_LEC_CAPTURE_SKIPPED)
				break;
		CHECK(i < k.n);
		if (i < k.n) {
			printf("# skip: %llu periods dropped, tick %llu after "
			       "%llu\n", (unsigned long long)(st.trim_drops +
							     st.capture_drops -
							     drops),
			       (unsigned long long)k.rec[i].tick,
			       (unsigned long long)k.rec[i - 1u].tick);
			CHECK(k.rec[i].tick > k.rec[i - 1u].tick + 1u);
			CHECK_UINT(k.rec[i].tick - k.rec[i - 1u].tick - 1u,
				   st.trim_drops + st.capture_drops - drops);
			CHECK_UINT(k.rec[i].skipped,
				   st.trim_drops + st.capture_drops - drops);
			CHECK_UINT(k.rec[i].lost_before, 0);
		}
		CHECK_UINT(tc_cap_replay_check(&k, 0, "engine skip"), 1);
	}

	CASE("a re-time marks RESET and starts the frame count, and the "
	     "replay holds");
	from = k.n;
	retimes = tc_cap_retimes(&t);
	/* Two periods under the latched value, so the drift opens an episode
	 * and the window after it re-times the canceller (tc_hold_delay). */
	atomic_store_explicit(&tc_be.delay_bias, -160L, memory_order_release);
	CHECK(tc_cap_retime_wait(&t, &k, retimes));
	tc_cap_wait(&t, &k, 200);
	i = tc_cap_find(&k, from, 0, UDSP_LEC_CAPTURE_RUN,
			UDSP_LEC_CAPTURE_RESET);
	CHECK(from < i && i < k.n);
	if (from < i && i < k.n) {
		CHECK_UINT(k.rec[i].frames, 1);
		CHECK_UINT(k.rec[i].gen, (uint16_t)(k.rec[i - 1u].gen + 1u));
	}
	CHECK_UINT(tc_cap_count(&k, from, 0, UDSP_LEC_CAPTURE_RUN,
				UDSP_LEC_CAPTURE_RESET,
				UDSP_LEC_CAPTURE_RESET), 1);
	n = tc_cap_count(&k, from, 0, UDSP_LEC_CAPTURE_RUN,
			 UDSP_LEC_CAPTURE_FREEZE, UDSP_LEC_CAPTURE_FREEZE);
	printf("# re-time: %u frozen RUN records\n", n);
	CHECK(n > 0u);
	tc_cap_replay_check(&k, 0, "re-time");

	CASE("an open loop gives FEED records, and the replay holds after the "
	     "close");
	from = k.n;
	tc_hook_set(&t, false);
	tc_cap_wait(&t, &k, TC_LEC_OPEN_MS);
	tc_hook_set(&t, true);
	tc_cap_wait(&t, &k, 300);
	n = tc_cap_count(&k, from, 0, UDSP_LEC_CAPTURE_FEED,
			 UDSP_LEC_CAPTURE_SKIP_OPEN,
			 UDSP_LEC_CAPTURE_SKIP_OPEN);
	printf("# open loop: %u FEED records\n", n);
	CHECK(n >= 20u);
	tc_cap_replay_check(&k, 0, "open loop");

	CASE("the modem kind gives FEED records, and the replay holds");
	from = k.n;
	atomic_store(&t.s->chan[0].fax_bypass_kind, UDSP_PASSTHROUGH_MODEM);
	tc_cap_wait(&t, &k, TC_LEC_SKIP_MS);
	CHECK(udsp_line_lec_latch_clear(t.s, 0) == UDSP_OK);
	tc_cap_wait(&t, &k, 300);
	n = tc_cap_count(&k, from, 0, UDSP_LEC_CAPTURE_FEED,
			 UDSP_LEC_CAPTURE_SKIP_MODEM,
			 UDSP_LEC_CAPTURE_SKIP_MODEM);
	printf("# modem: %u FEED records\n", n);
	CHECK(n >= 20u);
	tc_cap_replay_check(&k, 0, "modem");

	CASE("a canceller switched off gives FEED records, and the replay "
	     "holds");
	from = k.n;
	ccfg.ec_enable = false;
	CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
	tc_cap_wait(&t, &k, TC_LEC_SKIP_MS);
	ccfg.ec_enable = true;
	CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
	tc_cap_wait(&t, &k, 300);
	n = tc_cap_count(&k, from, 0, UDSP_LEC_CAPTURE_FEED,
			 UDSP_LEC_CAPTURE_SKIP_OFF, UDSP_LEC_CAPTURE_SKIP_OFF);
	printf("# switched off: %u FEED records\n", n);
	CHECK(n >= 20u);
	tc_cap_replay_check(&k, 0, "switched off");

	CASE("the inactive stream of a fax profile gives FEED records, and the "
	     "replay holds");
	from = k.n;
	CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH, UDSP_SEL_PCMU,
			       0) == UDSP_OK);
	tc_cap_wait(&t, &k, TC_LEC_SKIP_MS);
	CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_OFF, 0, 0) == UDSP_OK);
	tc_cap_wait(&t, &k, 300);
	n = tc_cap_count(&k, from, 0, UDSP_LEC_CAPTURE_FEED,
			 UDSP_LEC_CAPTURE_SKIP_INACTIVE,
			 UDSP_LEC_CAPTURE_SKIP_INACTIVE);
	printf("# fax profile: %u inactive and %u modem FEED records\n", n,
	       tc_cap_count(&k, from, 0, UDSP_LEC_CAPTURE_FEED,
			    UDSP_LEC_CAPTURE_SKIP_MODEM,
			    UDSP_LEC_CAPTURE_SKIP_MODEM));
	CHECK(n > 0u);
	tc_cap_replay_check(&k, 0, "fax profile");

	CASE("the verdict trace and the capture run together without loss");
	from = k.n;
	udsp_debug_set(t.s, atomic_load(&t.s->debug) | UDSP_DEBUG_SLIC_LOG);
	tc_log_reset();
	CHECK(tc_log_wait_count(UDSP_LOG_DEBUG, TC_LEC_SECOND_NEEDLE, 2u,
				3000));
	tc_cap_drain(&k);
	udsp_debug_set(t.s, atomic_load(&t.s->debug) &
		       ~(uint_least32_t)UDSP_DEBUG_SLIC_LOG);
	n = tc_lec_seconds_read(sec, TC_LOG_MAX, &lines);
	CHECK(n >= 2u);
	CHECK_UINT(n, lines);
	for (bad = 0, i = 0; i < n; i++)
		bad += sec[i].lost != 0;
	CHECK_UINT(bad, 0);
	for (lost = 0, i = from; i < k.n; i++)
		lost += k.rec[i].lost_before;
	CHECK(k.n > from + 100u);
	CHECK_UINT(lost, 0);
	tc_cap_replay_check(&k, 0, "verdict trace");

	CASE("a re-time between the first FEED and the first RUN of an "
	     "instance marks no RESET and keeps one generation");
	tc_cap_feed_retime_check(&t, &w, &k);
	tc_cap_replay_check(&k, 0, "re-time before the first RUN");

	CASE("the capture stops when its bit clears");
	CHECK(udsp_lec_capture_enable(t.s, false) == 0);
	tc_cap_drain(&k);
	eh_wait_ms(&t, 300);
	CHECK_UINT(udsp_lec_capture_read(t.s, &spill, 1), 0);
	CHECK_UINT(tc_cap_shape_bad(&k), 0);
	CHECK_UINT(k.over, 0);

	CASE("a capture disarm on a stopped engine returns at once");
	CHECK(tc_stream_down(&t, &w, 0));
	CHECK(tc_stop(&t));
	CHECK(udsp_lec_capture_enable(t.s, true) == 0);
	CHECK_UINT(tc_cap_bit(&t), 1);
	head = atomic_load(&t.s->ctl_ring.head);
	CHECK(udsp_lec_capture_enable(t.s, false) == 0);
	CHECK_UINT(tc_cap_bit(&t), 0);
	/* The disarm posted no command for a tick that does not run. */
	CHECK_UINT(atomic_load(&t.s->ctl_ring.head), head);
	tc_free(&t);
	udsp_tdm_override(NULL);

	CASE("a full capture ring counts each loss in the next record of its "
	     "line");
	memset(k.rec, 0, TC_CAP_MAX * sizeof(*k.rec));
	k.n = 0;
	setenv("UDSP_TEST_LEC_CAPTURE_SLOTS", "16", 1);
	if (!tc_cap_two_up(&t, &w, &w1)) {
		unsetenv("UDSP_TEST_LEC_CAPTURE_SLOTS");
		CHECK(false);
		free(k.rec);
		return;
	}
	k.s = t.s;
	/* A period five times the one of tc_up. A drain on every pass then
	 * keeps up with a ring of 15 records on a loaded host. */
	atomic_store(&eh_be.pace_us, 5000u);
	CHECK(udsp_lec_capture_enable(t.s, true) == 0);
	unsetenv("UDSP_TEST_LEC_CAPTURE_SLOTS");
	ring = atomic_load(&t.s->lec_cap);
	CHECK_UINT(ring ? ring->mask + 1u : 0u, 16);
	/* No drain for 30 ticks, then a drain on every tick. */
	eh_wait_ms(&t, 300);
	tc_cap_wait(&t, &k, 100);
	for (unsigned line = 0; line < 2u; line++) {
		bad = tc_cap_loss_bad(&k, line, &lost);
		printf("# line %u: %u records, %llu lost, %u out of rule\n",
		       line, tc_cap_count(&k, 0, line, UDSP_LEC_CAPTURE_RUN, 0,
					  0) +
		       tc_cap_count(&k, 0, line, UDSP_LEC_CAPTURE_FEED, 0, 0),
		       (unsigned long long)lost, bad);
		CHECK(lost > 0u);
		CHECK_UINT(bad, 0);
	}

	CASE("a canceller drop pushes END with the loss count still open");
	eh_wait_ms(&t, 300);
	CHECK(tc_cap_end_drop(&t, &k, &gate, true, NULL));
	tc_stream_fds_close(&w);
	tc_cap_wait(&t, &k, 100);
	i = tc_cap_find(&k, 0, 0, UDSP_LEC_CAPTURE_END, 0);
	CHECK(i < k.n);
	if (i < k.n) {
		printf("# END: lost_before %u\n", k.rec[i].lost_before);
		CHECK(k.rec[i].lost_before > 0u);
		CHECK_UINT(k.rec[i].samples, t.s->chan[0].frame);
	}
	CHECK_UINT(tc_cap_count(&k, 0, 0, UDSP_LEC_CAPTURE_END, 0, 0), 1);
	CHECK_UINT(tc_cap_loss_bad(&k, 0, &lost), 0);
	CHECK_UINT(tc_cap_loss_bad(&k, 1, &lost), 0);
	CHECK_UINT(tc_cap_shape_bad(&k), 0);

	CASE("an instance that leaves before its first frame gives no record, "
	     "END included");
	tc_cap_unseen_check(&t, &k, &gate);

	CASE("an END that finds the ring full leaves its count to the NEW of "
	     "the next instance");
	start = tc_cap_end_lost_check(&t, &w, &k, &gate);

	CASE("a skip and a loss in one jump read as a loss");
	tc_cap_skip_loss_check(&t, &k, start);
	CHECK_UINT(tc_cap_shape_bad(&k), 0);

	CASE("a capture disarm returns only after a tick took its command");
	{
		bool blocked;
		int type;
		int rc = 1;

		CHECK(tc_cap_disarm_gated(&t, &gate, false, &blocked, &type,
					  &rc));
		CHECK(blocked);
		/* The barrier is the named command, not the empty type that
		 * the tick refuses. */
		CHECK_UINT(type, UDSP_CMD_TICK_SYNC);
		CHECK_UINT(rc, 0);
		CHECK_UINT(tc_cap_bit(&t), 0);
	}
	CHECK(tc_stream_down(&t, &w, 0));
	CHECK(tc_stream_down(&t, &w1, 1));
	tc_down(&t);
	eh_gate_close(&gate);

	CASE("a canceller created while its line runs gives a RUN with NEW as "
	     "its first record");
	memset(k.rec, 0, TC_CAP_MAX * sizeof(*k.rec));
	k.n = 0;
	gate.fd[0] = gate.fd[1] = -1;
	/* The disarm below holds the tick for the whole SYNC-TICK bound, and
	 * the watchdog must not declare the handle dead in that time. */
	setenv("UDSP_WATCHDOG_MS", "60000", 1);
	up = tc_cap_two_up(&t, &w, &w1);
	unsetenv("UDSP_WATCHDOG_MS");
	if (!up) {
		CHECK(false);
		free(k.rec);
		return;
	}
	k.s = t.s;
	CHECK(udsp_lec_capture_enable(t.s, true) == 0);
	start = tc_cap_new_find(&k, tc_cap_run_first_check(&t, &w, &k), 0);
	tc_cap_replay_check(&k, start, "run first");

	CASE("each skip of the engine gives its own count to the next record "
	     "of each line");
	tc_cap_skips_check(&t, &k);
	CHECK_UINT(tc_cap_replay_check(&k, start, "two skips"), 2);

	CASE("a skip while the bit is clear reads as a gap after the arm");
	tc_cap_skip_gap_check(&t, &k, start);

	CASE("a NEW record after a skip carries no count");
	start = tc_cap_skip_new_check(&t, &w, &k);
	CHECK_UINT(tc_cap_shape_bad(&k), 0);

	CASE("each FEED record carries the near-end frame of its tick in mic");
	n = tc_cap_count(&k, start, 0, UDSP_LEC_CAPTURE_FEED, 0, 0);
	printf("# near-end tone: %u FEED records\n", n);
	CHECK(n >= 5u);
	CHECK_UINT(tc_cap_tone_bad(&k, start, 0, UDSP_SAMPLE_RATE), 0);
	atomic_store_explicit(&tc_be.tone_hz, 0u, memory_order_release);

	CASE("a canceller at the address of the one before it still gives NEW");
	tc_cap_reuse_check(&t, &w, &k);

	CASE("a release whose acknowledgment is lost pushes its END before the "
	     "control thread frees the instance");
	tc_cap_lost_ack_check(&t, &w, &k);

	CASE("a release that finds the retire mailbox occupied leaves the "
	     "canceller live and pushes no END");
	tc_cap_occupied_check(&t, &w, &k, &gate);

	CASE("a capture disarm whose tick does not answer times out with the "
	     "bit clear");
	{
		bool blocked;
		int type;
		int rc = 1;

		CHECK(tc_cap_disarm_gated(&t, &gate, true, &blocked, &type,
					  &rc));
		CHECK(blocked);
		CHECK_UINT(type, UDSP_CMD_TICK_SYNC);
		CHECK(rc == -ETIMEDOUT);
		CHECK_UINT(tc_cap_bit(&t), 0);
	}
	CHECK(tc_stream_down(&t, &w, 0));
	CHECK(tc_stream_down(&t, &w1, 1));
	tc_down(&t);
	eh_gate_close(&gate);

	CASE("a 16 kHz line captures 160 samples in each plane, and the replay "
	     "holds");
	tc_cap_wide_check(&k);
	free(k.rec);
}

/* The child of case_lec_capture_stop_timeout. It arms the capture, wedges the
 * backend and stops the handle, whose engine stop then times out. */
static void tc_child_cap_stop_timeout(void)
{
	struct udsp_job job = { 0 };
	struct eh t;

	tc_wedge_open(&t, &tw0_ops, &tw[0], true);
	if (!tc_child->started)
		return;
	tc_child->cap_armed = udsp_lec_capture_enable(t.s, true) == 0;
	eh_sleep_ms(50);
	tc_child->stop_accepted = udsp_stop(t.s, &job) == UDSP_OK;
	tc_child->job_seen = eh_job_wait(&t, job.token, 4000,
					 &tc_child->job_status);
	tc_child->dead = atomic_load(&t.s->dead);
	tc_child->stop_timeouts = (unsigned)atomic_load(&t.s->stop_timeouts);
	tc_child->cap_rc = udsp_lec_capture_enable(t.s, false);
	tc_child->cap_bit = tc_cap_bit(&t) != 0;
	udsp_close(t.s);
}

/*
 * A capture disarm after an engine stop that timed out. The tick thread did
 * not join and can still publish a record it claimed, so the disarm cannot
 * show that the producer finished. The case runs in a child process, for the
 * leak reason of struct tc_child_page.
 */
static void case_lec_capture_stop_timeout(void)
{
	CASE("a capture disarm after an engine stop that timed out answers "
	     "-ETIMEDOUT with the bit clear");
	if (pipe(tc_wedge_pipe)) {
		CHECK(false);
		return;
	}
	CHECK(tc_fork_run(tc_child_cap_stop_timeout));
	CHECK(tc_child->started);
	CHECK(tc_child->cap_armed);
	CHECK(tc_child->stop_accepted);
	CHECK(tc_child->job_seen);
	CHECK(tc_child->dead);
	CHECK_UINT(tc_child->stop_timeouts, 1);
	CHECK(tc_child->cap_rc == -ETIMEDOUT);
	CHECK(!tc_child->cap_bit);
	close(tc_wedge_pipe[0]);
	close(tc_wedge_pipe[1]);
	tc_wedge_pipe[0] = tc_wedge_pipe[1] = -1;
}

/* --- the offset of the uplink -------------------------------------------- */

/* The rate suffix of test_lec.c case_rate. The page generator reads the
 * helpers that open a case in each file on its own, so each file has one. */
static void tc_case_rate(unsigned rate, const char *what)
{
	char name[160];

	snprintf(name, sizeof(name), "%s%s", what,
		 rate == UDSP_SAMPLE_RATE_WB ? " at 16 kHz" : "");
	CASE(name);
}

#define TC_UL_DC_MILLI		5300
#define TC_UL_DC_STEP_MILLI	7300
/* The room of the near end: 2.3 LSB RMS, the level of the quiet frames of a
 * real line at 8 kHz. */
#define TC_UL_ROOM_MILLI	2300u
/* A removal without its carry leaves 0.3 LSB of the 5.3 LSB offset, so the
 * bound on a removed offset stands under that. */
#define TC_UL_DC_TOL		0.1
/*
 * The windows of the removed offset, in frames. The carry adds each rounding
 * error to the next sample, so the mean over a window of W frames is 63 / W
 * times the move of the estimate across it. Through the room, the seed carries
 * the noise of one frame mean, 0.26 LSB RMS at 8 kHz. A window that starts at
 * the first frame needs 800 frames to hold the bound at five deviations. A
 * later window takes 150 frames: the estimate carries 0.023 LSB RMS there, and
 * the tail of a step takes up to 0.01 LSB of the bound.
 */
#define TC_UL_SEED_FRAMES	800u
#define TC_UL_WIN_FRAMES	150u
/* The kind argument of tc_cap_mic_avg that takes a RUN and a FEED. */
#define TC_CAP_PLANE		0xffu

static double tc_cap_mic_mean(const struct udsp_lec_capture *r)
{
	double sum = 0.0;

	for (unsigned i = 0; i < r->samples; i++)
		sum += r->mic[i];

	return sum / r->samples;
}

/*
 * The mean of the microphone planes of line 0 over the records from index from
 * on whose tick is in [t0, t1), whose kind is kind and whose flags under mask
 * read want. n takes the count of those records.
 */
static double tc_cap_mic_avg(const struct tc_cap *k, unsigned from,
			     uint64_t t0, uint64_t t1, unsigned kind,
			     unsigned mask, unsigned want, unsigned *n)
{
	double sum = 0.0;

	*n = 0;
	for (unsigned i = from; i < k->n; i++) {
		const struct udsp_lec_capture *r = &k->rec[i];
		bool plane = r->kind != UDSP_LEC_CAPTURE_END;

		if (r->line != 0 || r->tick < t0 || r->tick >= t1 ||
		    (kind == TC_CAP_PLANE ? !plane : r->kind != kind) ||
		    (r->flags & mask) != want)
			continue;
		sum += tc_cap_mic_mean(r);
		(*n)++;
	}

	return *n ? sum / *n : 0.0;
}

/* The tick of the last record of line 0 with a plane, or 0. */
static uint64_t tc_cap_tick_last(const struct tc_cap *k)
{
	for (unsigned i = k->n; i-- > 0;) {
		if (k->rec[i].line == 0 &&
		    k->rec[i].kind != UDSP_LEC_CAPTURE_END)
			return k->rec[i].tick;
	}

	return 0;
}

/* The index of the first record of line 0 from index from on whose plane mean
 * reaches at least mean, or k->n. */
static unsigned tc_cap_mic_rise_find(const struct tc_cap *k, unsigned from,
				     double mean)
{
	for (unsigned i = from; i < k->n; i++) {
		const struct udsp_lec_capture *r = &k->rec[i];

		if (r->line == 0 && r->kind != UDSP_LEC_CAPTURE_END &&
		    tc_cap_mic_mean(r) >= mean)
			return i;
	}

	return k->n;
}

static bool tc_cap_mic_zero(const struct udsp_lec_capture *r)
{
	for (unsigned i = 0; i < r->samples; i++) {
		if (r->mic[i])
			return false;
	}

	return true;
}

/* The index of the first record of line 0 with a plane in [from, to) whose
 * microphone plane is all zeros when zero is true, and not all zeros when it is
 * false, or to. to is at most k->n. */
static unsigned tc_cap_zero_find(const struct tc_cap *k, unsigned from,
				 unsigned to, bool zero)
{
	for (unsigned i = from; i < to; i++) {
		const struct udsp_lec_capture *r = &k->rec[i];

		if (r->line == 0 && r->kind != UDSP_LEC_CAPTURE_END &&
		    tc_cap_mic_zero(r) == zero)
			return i;
	}

	return to;
}

/*
 * The muted run of the records in [from, to): n takes the count of the planes
 * of line 0 from the first plane of exact zeros on. The answer is false when a
 * plane after that one is not all zeros. The records before it are the frames
 * that the line sent before the mute.
 */
static bool tc_cap_muted_run(const struct tc_cap *k, unsigned from,
			     unsigned to, unsigned *n)
{
	*n = 0;
	for (unsigned i = tc_cap_zero_find(k, from, to, true); i < to; i++) {
		const struct udsp_lec_capture *r = &k->rec[i];

		if (r->line != 0 || r->kind == UDSP_LEC_CAPTURE_END)
			continue;
		if (!tc_cap_mic_zero(r))
			return false;
		(*n)++;
	}

	return true;
}

/*
 * One line of the offset cases at the rate given: the offset of 5.3 LSB, the
 * room, the canceller and its capture on, a stream up and the hook off. The
 * answer is false when the line did not open.
 */
static bool tc_ul_line_up(struct eh *t, struct tc_stream *w, struct tc_cap *k,
			  unsigned rate, bool mute)
{
	struct udsp_chan_cfg ccfg;

	k->n = 0;
	k->over = 0;
	if (!k->rec || !tc_up_line(t, "USA", 1, rate, TC_UL_DC_MILLI,
				   TC_UL_ROOM_MILLI, mute))
		return false;
	k->s = t->s;
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.ec_enable = true;
	ccfg.ec_nlp_enable = true;
	ccfg.ec_tail_ms = 32;
	CHECK(udsp_chan_config(t->s, 0, &ccfg) == UDSP_OK);
	CHECK(udsp_lec_capture_enable(t->s, true) == 0);
	CHECK(tc_stream_up(t, w, 0));
	CHECK(eh_hook_seize(t));

	return true;
}

static void tc_ul_line_down(struct eh *t, struct tc_stream *w)
{
	CHECK(udsp_lec_capture_enable(t->s, false) == 0);
	CHECK(tc_stream_down(t, w, 0));
	tc_down(t);
}

/*
 * The offset of the uplink on one line at the rate given. The fake line adds
 * 5.3 LSB and the room of a real line. The microphone plane of the capture
 * is the frame that the canceller takes after chan_ul, so it reads the
 * removal. The first record comes a few ticks after the open, and the first
 * window starts there. A tracker with no seed reads most of the offset over
 * that window.
 */
static void tc_ul_offset_run(struct tc_cap *k, unsigned rate)
{
	struct tc_stream w = { .rtp = { -1, -1 }, .rtcp = { -1, -1 } };
	unsigned from, first, step, n;
	double mean;
	uint64_t at;
	struct eh t;

	tc_case_rate(rate, "the uplink reads without the line offset from its "
		     "first frame");
	if (!tc_ul_line_up(&t, &w, k, rate, false)) {
		CHECK(false);
		return;
	}
	tc_cap_wait(&t, k, 8300);
	first = tc_cap_new_find(k, 0, 0);
	CHECK(first < k->n);
	if (first >= k->n)
		goto down;
	at = k->rec[first].tick;
	mean = tc_cap_mic_avg(k, first, at, at + TC_UL_SEED_FRAMES,
			      TC_CAP_PLANE, 0, 0, &n);
	printf("# %u Hz: from the first record at tick %llu, %u records read "
	       "%.4f LSB; the first record reads %.3f\n", rate,
	       (unsigned long long)at, n, mean,
	       tc_cap_mic_mean(&k->rec[first]));
	CHECK(n >= TC_UL_SEED_FRAMES - 10u);
	CHECK(fabs(mean) <= TC_UL_DC_TOL);

	tc_case_rate(rate, "the uplink frame reads within 0.1 LSB of zero "
		     "after two seconds");
	mean = tc_cap_mic_avg(k, first, at + 200u, at + 200u + TC_UL_WIN_FRAMES,
			      TC_CAP_PLANE, 0, 0, &n);
	printf("# %u Hz: two seconds on, %u records read %.4f LSB\n", rate, n,
	       mean);
	CHECK(n >= TC_UL_WIN_FRAMES - 10u);
	CHECK(fabs(mean) <= TC_UL_DC_TOL);

	tc_case_rate(rate, "the uplink estimate follows a step of the line "
		     "offset within about one second");
	from = k->n;
	atomic_store(&tc_be.dc_milli, TC_UL_DC_STEP_MILLI);
	tc_cap_wait(&t, k, 5000);
	step = tc_cap_mic_rise_find(k, from, 1.0);
	CHECK(step < k->n);
	if (step >= k->n)
		goto down;
	at = k->rec[step].tick;
	mean = tc_cap_mic_avg(k, step, at + 100u, at + 100u + TC_UL_WIN_FRAMES,
			      TC_CAP_PLANE, 0, 0, &n);
	printf("# %u Hz: the step reads %.3f LSB, and %u records from one "
	       "second on read %.3f\n", rate, tc_cap_mic_mean(&k->rec[step]), n,
	       mean);
	CHECK(n >= TC_UL_WIN_FRAMES - 10u);
	CHECK(fabs(mean) <= 0.5);
	mean = tc_cap_mic_avg(k, step, at + 300u, at + 300u + TC_UL_WIN_FRAMES,
			      TC_CAP_PLANE, 0, 0, &n);
	printf("# %u Hz: three seconds after the step, %u records read %.4f "
	       "LSB\n", rate, n, mean);
	CHECK(n >= TC_UL_WIN_FRAMES - 10u);
	CHECK(fabs(mean) <= TC_UL_DC_TOL);

	tc_case_rate(rate, "the modem kind keeps the raw uplink word, and the "
		     "estimate follows the line under it");
	from = k->n;
	atomic_store(&t.s->chan[0].fax_bypass_kind, UDSP_PASSTHROUGH_MODEM);
	tc_cap_wait(&t, k, 2000);
	mean = tc_cap_mic_avg(k, from, 0, UINT64_MAX, UDSP_LEC_CAPTURE_FEED,
			      UDSP_LEC_CAPTURE_SKIP_MODEM,
			      UDSP_LEC_CAPTURE_SKIP_MODEM, &n);
	printf("# %u Hz: the modem kind reads %.4f LSB over %u records\n",
	       rate, mean, n);
	CHECK(n >= 150u);
	CHECK(fabs(mean - TC_UL_DC_STEP_MILLI / 1000.0) <= TC_UL_DC_TOL);
	/* A step under the modem kind: an estimate that stood still would
	 * read the 2 LSB of the step at the return to voice. */
	from = k->n;
	atomic_store(&tc_be.dc_milli, TC_UL_DC_MILLI);
	tc_cap_wait(&t, k, 3000);
	CHECK(udsp_line_lec_latch_clear(t.s, 0) == UDSP_OK);
	tc_cap_wait(&t, k, 1700);
	step = tc_cap_find(k, from, 0, UDSP_LEC_CAPTURE_RUN, 0);
	CHECK(step < k->n);
	if (step >= k->n)
		goto down;
	at = k->rec[step].tick;
	mean = tc_cap_mic_avg(k, step, at, at + TC_UL_WIN_FRAMES,
			      UDSP_LEC_CAPTURE_RUN, 0, 0, &n);
	printf("# %u Hz: the return to voice reads %.4f LSB over %u records\n",
	       rate, mean, n);
	CHECK(n >= TC_UL_WIN_FRAMES - 10u);
	CHECK(fabs(mean) <= TC_UL_DC_TOL);

	tc_case_rate(rate, "the fax kind takes the uplink frame without the "
		     "line offset");
	from = k->n;
	atomic_store(&t.s->chan[0].fax_bypass_kind, UDSP_PASSTHROUGH_FAX);
	tc_cap_wait(&t, k, 1500);
	CHECK(udsp_line_lec_latch_clear(t.s, 0) == UDSP_OK);
	mean = tc_cap_mic_avg(k, from, 0, UINT64_MAX, UDSP_LEC_CAPTURE_RUN, 0,
			      0, &n);
	printf("# %u Hz: the fax kind reads %.4f LSB over %u records\n", rate,
	       mean, n);
	CHECK(n >= 100u);
	CHECK(fabs(mean) <= TC_UL_DC_TOL);
down:
	tc_ul_line_down(&t, &w);
}

/*
 * The offset that the SLIC adds to the uplink word leaves the uplink at the
 * TDM read (chan.c chan_ul_offset_remove). The canceller, the capture, the
 * detectors and the encoders then read the frame without it. The modem kind
 * keeps the raw word.
 */
static void case_ul_offset(void)
{
	struct tc_cap k;

	/* The first case of each run fails on a buffer that did not allocate,
	 * so the answer stays unread here. */
	(void)tc_cap_open(&k);
	tc_ul_offset_run(&k, UDSP_SAMPLE_RATE);
	tc_ul_offset_run(&k, UDSP_SAMPLE_RATE_WB);
	free(k.rec);
}

/* The line offsets of the floor case, as real lines carry them: 7.1 LSB at
 * 8 kHz and 5.1 LSB at 16 kHz. */
#define TC_UL_FLOOR_DC_MILLI	7100
#define TC_UL_FLOOR_DC_WB_MILLI	5100
/* With the removal, the coefficient energy of a run stands within 0.3 percent
 * of the twin. Without it, the energy reads 19 to 29 percent under the twin.
 * A factor of 1.1 separates the two at both rates. */
#define TC_UL_COEF_FACTOR	1.1

/* What the canceller of one run reads over its last four seconds. coef_micro
 * is the mean coefficient energy of the RUN records, in millionths. */
struct tc_ul_floor {
	double floor_dbov;
	double erle_db;
	double cn_db;
	double coef_micro;
	unsigned floor_n;
	unsigned erle_n;
	unsigned cn_n;
};

/*
 * The comfort noise of the records from index from on: the RMS of the output
 * plane in dB against one LSB, over each RUN of line 0 that the processor
 * acted on through the whole frame. The frame after the engage edge carries
 * the fade, so a frame counts only when the record before it was active too.
 */
static double tc_cap_cn_db(const struct tc_cap *k, unsigned from,
			   unsigned *n)
{
	bool was = false;
	double sum = 0.0;
	unsigned samples = 0;

	*n = 0;
	for (unsigned i = from; i < k->n; i++) {
		const struct udsp_lec_capture *r = &k->rec[i];
		bool active = (r->flags & UDSP_LEC_CAPTURE_ACTIVE) != 0;

		if (r->line != 0 || r->kind != UDSP_LEC_CAPTURE_RUN)
			continue;
		if (active && was) {
			for (unsigned j = 0; j < r->samples; j++)
				sum += (double)r->out[j] * r->out[j];
			samples += r->samples;
			(*n)++;
		}
		was = active;
	}

	return samples ? 10.0 * log10(sum / samples) : -99.0;
}

/*
 * One run of the floor case at the rate given: the busy tone as the far end
 * through a 12 dB hybrid, the room, and the line offset dc_milli. The busy
 * tone pauses every half second, and the canceller measures its floor in the
 * pauses.
 */
static bool tc_ul_floor_run(struct tc_cap *k, unsigned rate, int dc_milli,
			    struct tc_ul_floor *out)
{
	struct udsp_chan_cfg ccfg;
	double floor = 0.0, erle = 0.0, coef = 0.0;
	struct tc_stream w = { .rtp = { -1, -1 }, .rtcp = { -1, -1 } };
	unsigned from = 0;
	uint64_t last;
	struct eh t;
	bool ok;

	memset(out, 0, sizeof(*out));
	k->n = 0;
	k->over = 0;
	if (!tc_up_line(&t, "USA", 1, rate, dc_milli, TC_UL_ROOM_MILLI, false))
		return false;
	k->s = t.s;
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.ec_enable = true;
	ccfg.ec_nlp_enable = true;
	ccfg.ec_tail_ms = 32;
	ok = eh_hook_seize(&t) && udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK;
	atomic_store(&tc_be.echo_periods, 3u);
	atomic_store(&tc_be.echo_shift, 2u);
	ok = ok && udsp_lec_capture_enable(t.s, true) == 0;
	ok = ok && tc_stream_up(&t, &w, 0);
	ok = ok && udsp_tone_play(t.s, 0, UDSP_TONE_BUSY, 0) == UDSP_OK;
	if (ok)
		tc_cap_wait(&t, k, 8000);
	ok = udsp_lec_capture_enable(t.s, false) == 0 && ok;
	tc_cap_drain(k);
	ok = tc_stream_down(&t, &w, 0) && ok;
	atomic_store(&tc_be.echo_periods, 0u);
	tc_down(&t);
	last = tc_cap_tick_last(k);
	for (unsigned i = 0; i < k->n; i++) {
		const struct udsp_lec_capture *r = &k->rec[i];

		if (r->line != 0 || r->kind != UDSP_LEC_CAPTURE_RUN ||
		    r->tick + 400u <= last)
			continue;
		from = out->erle_n ? from : i;
		erle += r->erle_mdb / 1000.0;
		coef += r->coef_energy_micro;
		out->erle_n++;
		if (r->noise_ddbov == UDSP_LEC_CAPTURE_NOISE_UNMEASURED)
			continue;
		floor += r->noise_ddbov / 10.0;
		out->floor_n++;
	}
	out->erle_db = out->erle_n ? erle / out->erle_n : 0.0;
	out->coef_micro = out->erle_n ? coef / out->erle_n : 0.0;
	out->floor_dbov = out->floor_n ? floor / out->floor_n : 0.0;
	out->cn_db = tc_cap_cn_db(k, from, &out->cn_n);

	return ok && k->over == 0;
}

/*
 * The canceller floor, the ERLE, the comfort noise and the coefficient energy
 * read the room through a line offset, at the rate given. The offset is that
 * of a real line, and a twin run without it gives the reference. Without the
 * removal, the floor and the comfort noise read about 10 dB high at 8 kHz, and
 * 7 to 8 dB at 16 kHz. The tracked ERLE falls, and the coefficient energy
 * reads under the twin. On a real line whose far end carries an offset too,
 * the energy reads 13 to 17 times high at 16 kHz. The busy tone of this case
 * carries none.
 */
static void tc_ul_floor_leg(struct tc_cap *k, unsigned rate)
{
	int dc_milli = rate == UDSP_SAMPLE_RATE_WB ? TC_UL_FLOOR_DC_WB_MILLI :
						     TC_UL_FLOOR_DC_MILLI;
	double room_db = 20.0 * log10(TC_UL_ROOM_MILLI / 1000.0);
	unsigned khz = rate / 1000u;
	struct tc_ul_floor dc, twin;
	double coef_ratio;
	bool ok;

	tc_case_rate(rate, "the canceller floor, the ERLE, the comfort noise "
		     "and the coefficient energy read the room through a line "
		     "offset");
	if (!k->rec) {
		CHECK(false);
		return;
	}
	ok = tc_ul_floor_run(k, rate, dc_milli, &dc);
	CHECK(ok);
	ok = tc_ul_floor_run(k, rate, 0, &twin);
	CHECK(ok);
	coef_ratio = twin.coef_micro > 0.0 ? dc.coef_micro / twin.coef_micro :
					     0.0;
	printf("# %u kHz, offset %.1f LSB: floor %.1f dBov, ERLE %.1f dB, "
	       "comfort noise %.1f dB over %u frames, coefficient energy "
	       "%.0f\n", khz, dc_milli / 1000.0, dc.floor_dbov, dc.erle_db,
	       dc.cn_db, dc.cn_n, dc.coef_micro);
	printf("# %u kHz, no offset: floor %.1f dBov, ERLE %.1f dB, comfort "
	       "noise %.1f dB over %u frames, coefficient energy %.0f; room "
	       "%.1f dB; coefficient energy ratio %.3f\n", khz,
	       twin.floor_dbov, twin.erle_db, twin.cn_db, twin.cn_n,
	       twin.coef_micro, room_db, coef_ratio);
	CHECK(dc.floor_n >= 100u && twin.floor_n >= 100u);
	CHECK(dc.erle_n >= 300u && twin.erle_n >= 300u);
	CHECK(dc.cn_n >= 50u);
	CHECK(fabs(dc.floor_dbov - twin.floor_dbov) <= 1.0);
	CHECK(fabs(dc.erle_db - twin.erle_db) <= 1.0);
	CHECK(fabs(dc.cn_db - room_db) <= 2.0);
	CHECK(coef_ratio >= 1.0 / TC_UL_COEF_FACTOR &&
	      coef_ratio <= TC_UL_COEF_FACTOR);
}

/* The canceller reads the room through a line offset at both rates, because
 * the uplink removes the offset before the canceller takes the frame. */
static void case_ul_offset_floor(void)
{
	struct tc_cap k;

	/* Each leg fails on a buffer that did not allocate, so the answer
	 * stays unread here. */
	(void)tc_cap_open(&k);
	tc_ul_floor_leg(&k, UDSP_SAMPLE_RATE);
	tc_ul_floor_leg(&k, UDSP_SAMPLE_RATE_WB);
	free(k.rec);
}

/*
 * The muted line at the rate given. The fake line sends exact zeros while
 * tc_be.mute stands, as the part does on hook. The line opens muted, so the
 * first frame that is not all zeros seeds the estimate. The second mute
 * follows a run with the offset. A tracker that read the zero frames would
 * decay toward zero there, and the next call would start with most of the
 * offset.
 * A tracker that subtracted on them would write the negative offset onto the
 * muted zeros.
 */
static void tc_ul_mute_run(struct tc_cap *k, unsigned rate)
{
	struct tc_stream w = { .rtp = { -1, -1 }, .rtcp = { -1, -1 } };
	unsigned from, first, n;
	double mean;
	uint64_t at;
	bool exact;
	struct eh t;

	tc_case_rate(rate, "the uplink of a muted line reads exact zeros from "
		     "the open");
	if (!tc_ul_line_up(&t, &w, k, rate, true)) {
		CHECK(false);
		return;
	}
	tc_cap_wait(&t, k, 1000);
	exact = tc_cap_muted_run(k, 0, k->n, &n);
	printf("# %u Hz: muted from the open, %u of %u records read exact "
	       "zeros\n", rate, n, k->n);
	CHECK(exact);
	CHECK(tc_cap_zero_find(k, 0, k->n, false) == k->n);
	CHECK(n >= 90u);

	tc_case_rate(rate, "the first uplink frame that is not all zeros seeds "
		     "the estimate");
	from = k->n;
	atomic_store(&tc_be.mute, false);
	tc_cap_wait(&t, k, 8300);
	first = tc_cap_zero_find(k, from, k->n, false);
	CHECK(first < k->n);
	if (first >= k->n)
		goto down;
	at = k->rec[first].tick;
	mean = tc_cap_mic_avg(k, first, at, at + TC_UL_SEED_FRAMES,
			      TC_CAP_PLANE, 0, 0, &n);
	printf("# %u Hz: from the first frame that is not all zeros, %u "
	       "records read %.4f LSB; that frame reads %.3f\n", rate, n, mean,
	       tc_cap_mic_mean(&k->rec[first]));
	CHECK(n >= TC_UL_SEED_FRAMES - 10u);
	CHECK(fabs(mean) <= TC_UL_DC_TOL);

	tc_case_rate(rate, "a muted frame after a run with the offset reads "
		     "exact zeros on the uplink");
	from = k->n;
	atomic_store(&tc_be.mute, true);
	tc_cap_wait(&t, k, 2000);
	exact = tc_cap_muted_run(k, from, k->n, &n);
	printf("# %u Hz: the second mute reads exact zeros on %u records, %s\n",
	       rate, n, exact ? "and no sample after them is not zero" :
				"then a sample that is not zero");
	CHECK(exact);
	CHECK(n >= 180u);

	tc_case_rate(rate, "the uplink estimate carries over the muted frames "
		     "to the next call");
	from = k->n;
	atomic_store(&tc_be.mute, false);
	tc_cap_wait(&t, k, 2500);
	first = tc_cap_zero_find(k, from, k->n, false);
	CHECK(first < k->n);
	if (first >= k->n)
		goto down;
	at = k->rec[first].tick;
	mean = tc_cap_mic_avg(k, first, at, at + TC_UL_WIN_FRAMES,
			      TC_CAP_PLANE, 0, 0, &n);
	printf("# %u Hz: after the second mute, %u records read %.4f LSB\n",
	       rate, n, mean);
	CHECK(n >= TC_UL_WIN_FRAMES - 10u);
	CHECK(fabs(mean) <= TC_UL_DC_TOL);
down:
	tc_ul_line_down(&t, &w);
}

/*
 * A frame of exact zeros is not a reading of the line offset (chan.c
 * chan_ul_offset_remove). It reaches the consumers as exact zeros, and the
 * estimate carries over it from one call to the next.
 */
static void case_ul_offset_mute(void)
{
	struct tc_cap k;

	/* Each run fails on a buffer that did not allocate, so the answer
	 * stays unread here. */
	(void)tc_cap_open(&k);
	tc_ul_mute_run(&k, UDSP_SAMPLE_RATE);
	tc_ul_mute_run(&k, UDSP_SAMPLE_RATE_WB);
	free(k.rec);
}

#define TC_UL_CLIP_FRAMES	3u
/* The room of the fake line stands within six deviations, 13.8 LSB, because
 * tc_room_gauss sums twelve uniforms. With the two roundings and the error of
 * the estimate, a sample of a quiet frame stands within 16 LSB of zero. */
#define TC_UL_ROOM_PEAK		16

/*
 * The clipped records of line 0 from index from on. n takes their count, and
 * last the index of the last of them. The answer counts the records whose
 * half - 1 samples at the lower rail read -32768, while the half at the upper
 * rail read 32767 less the estimate within 1 LSB.
 */
static unsigned tc_cap_clip_read(const struct tc_cap *k, unsigned from,
				 double est, unsigned *n, unsigned *last)
{
	unsigned good = 0;

	*n = 0;
	*last = k->n;
	for (unsigned i = from; i < k->n; i++) {
		const struct udsp_lec_capture *r = &k->rec[i];
		unsigned half = r->samples / 2u;
		unsigned low = 0, high = 0;

		if (r->line != 0 || r->kind == UDSP_LEC_CAPTURE_END)
			continue;
		for (unsigned j = 0; j < r->samples; j++) {
			low += r->mic[j] == INT16_MIN;
			high += fabs(r->mic[j] - (32767.0 - est)) <= 1.0;
		}
		if (!low)
			continue;
		(*n)++;
		*last = i;
		good += low == half - 1u && high == half;
	}

	return good;
}

static int tc_cap_mic_peak(const struct udsp_lec_capture *r)
{
	int peak = 0;

	for (unsigned i = 0; i < r->samples; i++) {
		int v = abs(r->mic[i]);

		peak = v > peak ? v : peak;
	}

	return peak;
}

/*
 * A clipped run on the uplink at the rate given: three frames of the word of an
 * overdriven line (tc_be.clip_frames). The removal pushes the samples at the
 * lower rail past it, and they saturate. The carry holds the rounding error of
 * the unsaturated value, so it stays within 0.5 LSB. A carry taken from the
 * saturated sample would grow by the offset on each such sample, until the
 * next unsaturated sample takes it all. A window mean cannot show that carry,
 * because that sample gives the sum back. The case therefore reads the carry
 * under the gate and the peak of the frame after the run.
 */
static void tc_ul_clip_run(struct tc_cap *k, unsigned rate)
{
	struct tc_stream w = { .rtp = { -1, -1 }, .rtcp = { -1, -1 } };
	struct eh_gate gate = { .fd = { -1, -1 } };
	unsigned from, last, good, n;
	struct chan_ul_dc d = { 0 };
	double mean;
	uint64_t at;
	struct eh t;
	int peak;

	tc_case_rate(rate, "a clipped run saturates the uplink at the rail");
	if (!tc_ul_line_up(&t, &w, k, rate, false)) {
		CHECK(false);
		return;
	}
	tc_cap_wait(&t, k, 2000);
	from = k->n;
	if (!eh_gate_park(&gate))
		goto down;
	atomic_store(&tc_be.clip_frames, TC_UL_CLIP_FRAMES);
	if (!eh_gate_step(&gate, TC_UL_CLIP_FRAMES))
		goto down;
	d = t.s->chan[0].ul_dc;
	eh_gate_disarm(&gate);
	tc_cap_wait(&t, k, 2000);
	good = tc_cap_clip_read(k, from, d.est, &n, &last);
	printf("# %u Hz: %u clipped records, %u of them saturated at the rail; "
	       "the estimate reads %.3f LSB\n", rate, n, good, d.est);
	CHECK_UINT(n, TC_UL_CLIP_FRAMES);
	CHECK_UINT(good, TC_UL_CLIP_FRAMES);

	tc_case_rate(rate, "the uplink carry stays within 0.5 LSB through a "
		     "clipped run");
	printf("# %u Hz: the carry after the clipped run reads %.3f LSB\n",
	       rate, d.carry);
	CHECK(fabs(d.carry) <= 0.5);
	CHECK(last + 1u < k->n);
	if (last + 1u >= k->n)
		goto down;
	peak = tc_cap_mic_peak(&k->rec[last + 1u]);
	at = k->rec[last + 1u].tick;
	mean = tc_cap_mic_avg(k, last + 1u, at, at + TC_UL_WIN_FRAMES,
			      TC_CAP_PLANE, 0, 0, &n);
	printf("# %u Hz: the frame after the run peaks at %d LSB, and %u "
	       "records from it read %.4f LSB\n", rate, peak, n, mean);
	CHECK(peak <= TC_UL_ROOM_PEAK);
	CHECK(n >= TC_UL_WIN_FRAMES - 10u);
	CHECK(fabs(mean) <= TC_UL_DC_TOL);
down:
	eh_gate_disarm(&gate);
	tc_ul_line_down(&t, &w);
	eh_gate_close(&gate);
}

/*
 * The offset removal saturates a sample that it pushes past a rail, and its
 * carry holds the rounding error alone.
 */
static void case_ul_offset_clip(void)
{
	struct tc_cap k;

	/* Each run fails on a buffer that did not allocate, so the answer
	 * stays unread here. */
	(void)tc_cap_open(&k);
	tc_ul_clip_run(&k, UDSP_SAMPLE_RATE);
	tc_ul_clip_run(&k, UDSP_SAMPLE_RATE_WB);
	free(k.rec);
}

/*
 * A partial slice at the rate given. The engine refuses at the open a period
 * other than rate / 100 (engine.c engine_tdm_open), so only a direct call of
 * the tick hook delivers one. chan_ul returns before the offset removal on a
 * partial slice, because the rest of its frame holds the samples of the tick
 * before. The gate holds the engine thread in its read during the call, and
 * the case reads the tracker before and after it.
 */
static void tc_ul_partial_run(unsigned rate)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	struct chan_ul_dc before, after;
	int16_t rx[UDSP_FRAME_MAX];
	int16_t tx[UDSP_FRAME_MAX];
	unsigned frame;
	uint64_t tick;
	struct eh t;

	tc_case_rate(rate, "a partial slice leaves the uplink estimate and its "
		     "carry unchanged");
	if (!tc_up_line(&t, "USA", 1, rate, TC_UL_DC_MILLI, TC_UL_ROOM_MILLI,
			false)) {
		CHECK(false);
		return;
	}
	eh_wait_ms(&t, 1000);
	if (!eh_gate_park(&gate))
		goto out;
	frame = t.s->chan[0].frame;
	for (unsigned i = 0; i < UDSP_FRAME_MAX; i++)
		rx[i] = 1000;
	before = t.s->chan[0].ul_dc;
	/* The slice belongs to the tick that the engine published last. */
	tick = udsp_clock_tick(&t.s->clock) - t.s->clock.base;
	udsp_engine_hook(t.s, rx, tx, 1u, frame / 2u, tick);
	after = t.s->chan[0].ul_dc;
	printf("# %u Hz: a slice of %u samples: the estimate reads %.6f then "
	       "%.6f LSB, the carry %.6f then %.6f\n", rate, frame / 2u,
	       before.est, after.est, before.carry, after.carry);
	CHECK(before.seeded && after.seeded);
	CHECK(after.est == before.est);
	CHECK(after.carry == before.carry);
out:
	eh_gate_disarm(&gate);
	tc_down(&t);
	eh_gate_close(&gate);
}

/*
 * The offset removal runs on whole frames only (chan.c chan_ul): a partial
 * slice leaves the tracker of the line as it is.
 */
static void case_ul_offset_partial(void)
{
	tc_ul_partial_run(UDSP_SAMPLE_RATE);
	tc_ul_partial_run(UDSP_SAMPLE_RATE_WB);
}

/*
 * The hold of a played digit (core/chan.c chan_digit_hold_arm) reads the loop
 * that the engine measured last. The one-period guard and the reach clamp of
 * the reference apply to it, and four ticks follow. Before the first window of
 * an engine completes, the loop is the nominal prefill. A line without a
 * canceller gets the same hold, because the echo of the digit crosses the loop
 * either way.
 */
static void case_lec_digit_hold(void)
{
	/* An 8 kHz line, 80 samples a tick. The reach is 8000 * 256 / 1000 - 80
	 * = 1968 (chan.c chan_lec_delay_clamp):
	 *
	 * - 1000 guards to 920, which is 11 ticks plus 4;
	 * - 2000 guards to 1920, under the reach: 24 ticks plus 4;
	 * - 30000 clamps to the reach, 1968, which is 24 ticks plus 4;
	 * - 240 guards to 160, and 2 ticks plus 4 stand under the floor. */
	static const struct {
		uint32_t loop;
		unsigned ticks;
	} arm[] = {
		{ 1000u, 15u },
		{ 2000u, 28u },
		{ 30000u, 28u },
		{ 240u, UDSP_DL_DIGIT_HOLD_TICKS },
	};
	struct chan_lec_track tr;
	struct engine_stats st;
	struct udsp_chan *c;
	struct udsp *s;

	CASE("the tracker keeps the last measured loop for the digit hold");
	{
		memset(&tr, 0, sizeof(tr));
		memset(&st, 0, sizeof(st));
		st.prefill_periods = 3;
		st.frames_per_period = TC_REF_FRAME;

		st.serial = 1;
		tc_ref_check(&tr, &st, CHAN_LEC_HOLD, 1, 0);
		CHECK_UINT(tr.loop_last, 480);

		/* The published delay is the reach bound, and the loop is the
		 * measurement. */
		st.delay_calibrated = true;
		st.loop_ref = 4480;
		tc_ref_check(&tr, &st, CHAN_LEC_KEEP, 0, 3936);
		CHECK_UINT(tr.loop_last, 4480);

		st.delay_calibrated = false;
		st.cal_restarts = 1;
		tc_ref_check(&tr, &st, CHAN_LEC_HOLD, 0, 3936);
		CHECK_UINT(tr.loop_last, 4480);

		st.delay_calibrated = true;
		st.loop_ref = 640;
		tc_ref_check(&tr, &st, CHAN_LEC_RETIME, 1, 480);
		CHECK_UINT(tr.loop_last, 640);

		/* The nominal follows the prefill of the new engine. The new
		 * engine clears the line, so the line holds no value. */
		st.serial = 2;
		st.cal_restarts = 0;
		st.delay_calibrated = false;
		st.prefill_periods = ENGINE_PREFILL_MAX;
		tc_ref_check(&tr, &st, CHAN_LEC_HOLD, 1, 0);
		CHECK_UINT(tr.loop_last, 2560);

		st.serial = 3;
		st.prefill_periods = 3;
		tc_ref_check(&tr, &st, CHAN_LEC_HOLD, 1, 0);
		CHECK_UINT(tr.loop_last, 480);
	}

	CASE("a played digit holds the detector for the measured loop on a "
	     "line without a canceller");
	{
		s = calloc(1, sizeof(*s));
		if (!s) {
			CHECK(false);
			return;
		}
		c = &s->chan[1];
		c->rate = UDSP_SAMPLE_RATE;
		c->frame = UDSP_SAMPLE_RATE / 100u;
		atomic_init(&c->ec, NULL);
		for (unsigned i = 0; i < UDSP_LINE_MAX; i++) {
			atomic_init(&s->lec_ready[i], false);
			atomic_init(&s->lec_delay[i], 0u);
		}
		atomic_init(&s->lec_loop, 0u);
		for (unsigned i = 0; i < sizeof(arm) / sizeof(arm[0]); i++) {
			atomic_store(&s->lec_loop, arm[i].loop);
			chan_digit_hold_arm(s, c);
			CHECK_UINT(c->dl_digit_hold, arm[i].ticks);
		}
		free(s);
	}
}

/*
 * The reference rule with two lines, without an engine (core/chan.c
 * chan_lec_measure and chan_lec_line_decide), at 16 kHz with a prefill of 3.
 * The engine measures one loop per handle, and each line keeps the value its
 * own taps stand on. A line takes the offer only when the tick decides it. The
 * tick decides at the first frame of an instance, and at the top of each tick
 * while the instance stands. A line without an instance is not decided, so the
 * vector leaves it out of the ticks that model one.
 */
static void case_lec_ref_lines(void)
{
	struct chan_lec_track tr;
	struct engine_stats st;
	uint32_t gen0, gen1;

	memset(&st, 0, sizeof(st));
	st.prefill_periods = 3;
	st.frames_per_period = TC_REF_FRAME;

	CASE("a new instance takes the measurement while another line keeps "
	     "its value");
	{
		tc_ref_seed(&tr, &st, 1, 480);
		gen0 = tr.line[0].gen;
		st.loop_ref = 640;
		st.cal_restarts = 1;
		CHECK_UINT(chan_lec_measure(&tr, &st), CHAN_LEC_OFFER_WINDOW);
		tc_ref_line_check(&tr, 0, CHAN_LEC_KEEP, 0, 320);
		tc_ref_line_check(&tr, 1, CHAN_LEC_KEEP, 0, 480);
		CHECK(tr.line[1].applied);
		CHECK_UINT(tr.line[1].loop, 640);
		/* The adoption of line 1 leaves the taps of line 0 where they
		 * stand. */
		CHECK(tr.line[0].applied);
		CHECK_UINT(tr.line[0].loop, 480);
		CHECK_UINT(tr.line[0].gen, gen0);
		tc_ref_line_check(&tr, 0, CHAN_LEC_KEEP, 0, 320);
	}

	CASE("a line without an instance takes nothing until its next "
	     "instance");
	{
		/* The release of line 1. 560 is half a period from the 640 it
		 * stood on, so only a line with no value takes it. */
		gen1 = tr.line[1].gen;
		chan_lec_line_drop(&tr, 1);
		CHECK(!tr.line[1].applied);
		CHECK_UINT(tr.line[1].loop, 0);
		CHECK_UINT(tr.line[1].delay, 0);
		CHECK_UINT(tr.line[1].gen, gen1);
		st.loop_ref = 560;
		CHECK_UINT(chan_lec_measure(&tr, &st), CHAN_LEC_OFFER_WINDOW);
		tc_ref_line_check(&tr, 0, CHAN_LEC_KEEP, 0, 320);
		CHECK(!tr.line[1].applied);
		CHECK_UINT(tr.line[1].gen, gen1);
		/* The next instance of line 1 takes 560 verbatim. */
		tc_ref_line_check(&tr, 1, CHAN_LEC_KEEP, 0, 400);
		CHECK_UINT(tr.line[1].loop, 560);
	}

	CASE("an open window leaves a new instance unapplied");
	{
		chan_lec_line_drop(&tr, 1);
		st.delay_calibrated = false;
		st.cal_restarts = 2;
		CHECK_UINT(chan_lec_measure(&tr, &st), CHAN_LEC_OFFER_NONE);
		tc_ref_line_check(&tr, 1, CHAN_LEC_HOLD, 0, 0);
		CHECK(!tr.line[1].applied);
		/* The first later window applies to it verbatim. */
		st.delay_calibrated = true;
		st.loop_ref = 640;
		CHECK_UINT(chan_lec_measure(&tr, &st), CHAN_LEC_OFFER_WINDOW);
		tc_ref_line_check(&tr, 1, CHAN_LEC_KEEP, 0, 480);
		tc_ref_line_check(&tr, 0, CHAN_LEC_KEEP, 0, 320);
	}

	CASE("the fallback offers each line its own value or the last window");
	{
		/* Line 0 stands on 800 and keeps it against a window of 960.
		 * The window is the last one this engine completes. */
		tc_ref_seed(&tr, &st, 2, 800);
		st.loop_ref = 960;
		st.cal_restarts = 1;
		CHECK_UINT(chan_lec_measure(&tr, &st), CHAN_LEC_OFFER_WINDOW);
		tc_ref_line_check(&tr, 0, CHAN_LEC_KEEP, 0, 640);
		st.delay_calibrated = false;
		st.cal_restarts = 4;
		CHECK_UINT(chan_lec_measure(&tr, &st), CHAN_LEC_OFFER_FALLBACK);
		tc_ref_line_check(&tr, 0, CHAN_LEC_KEEP, 0, 640);
		CHECK_UINT(tr.line[0].loop, 800);
		tc_ref_line_check(&tr, 1, CHAN_LEC_KEEP, 0, 800);
		CHECK_UINT(tr.line[1].loop, 960);

		/* No window under this engine: the nominal prefill. */
		memset(&tr, 0, sizeof(tr));
		st.serial = 3;
		st.cal_restarts = 0;
		CHECK_UINT(chan_lec_measure(&tr, &st), CHAN_LEC_OFFER_NONE);
		st.cal_restarts = 3;
		CHECK_UINT(chan_lec_measure(&tr, &st), CHAN_LEC_OFFER_FALLBACK);
		tc_ref_line_check(&tr, 1, CHAN_LEC_KEEP, 0, 320);
		CHECK_UINT(tr.line[1].loop, 480);
	}

	CASE("an engine swap clears every line and moves every generation");
	{
		tc_ref_seed(&tr, &st, 4, 480);
		tc_ref_line_check(&tr, 1, CHAN_LEC_KEEP, 0, 320);
		CHECK(tr.line[0].applied && tr.line[1].applied);
		gen0 = tr.line[0].gen;
		gen1 = tr.line[1].gen;
		st.serial = 5;
		st.cal_restarts = 0;
		st.delay_calibrated = false;
		CHECK_UINT(chan_lec_measure(&tr, &st), CHAN_LEC_OFFER_NONE);
		CHECK_UINT(tr.line[0].gen, gen0 + 1u);
		CHECK_UINT(tr.line[1].gen, gen1 + 1u);
		CHECK(!tr.line[0].applied);
		CHECK(!tr.line[1].applied);
		tc_ref_line_check(&tr, 0, CHAN_LEC_HOLD, 0, 0);
		tc_ref_line_check(&tr, 1, CHAN_LEC_HOLD, 0, 0);
	}
}

/*
 * The adoption on the engine, on the hold model at 8 kHz (case_lec_hold). The
 * model reads a loop of 240 at rest, and tc_be.delay_bias moves the latched
 * reading by one period at a time. Line 0 holds a call through every step, and
 * its taps stand on 240. Line 1 has a canceller configured and takes an
 * instance only when its stream is configured. The reference of line 0 stays
 * 160 throughout, with no reset, while line 1 takes the loop that each hold
 * leaves.
 */
static void case_lec_ref_adopt(void)
{
	struct udsp_lec_stats before, ls;
	struct tc_stream w0, w1;
	struct engine_stats st;
	uint32_t frames;
	unsigned mark, i;
	uint64_t lost;
	struct tc_cap k;
	struct eh t;

	CASE("a canceller created after a hold adopts the loop the hold left");
	if (!tc_cap_open(&k)) {
		CHECK(false);
		return;
	}
	if (!tc_lec_hold_up_ports(&t, &w0, 2)) {
		CHECK(false);
		free(k.rec);
		return;
	}
	k.s = t.s;
	CHECK(tc_lec_converge(&t, &before));
	CHECK_UINT(before.ref_delay_samples, 160u);
	CHECK_UINT(before.retimes, 0);
	tc_engine_stats(&t, &st);
	CHECK_UINT((uint64_t)st.loop_ref, 240u);
	CHECK(udsp_line_lec_stats(t.s, 1, &ls) == UDSP_UNSUPPORTED);
	CHECK(udsp_lec_capture_enable(t.s, true) == 0);

	/* One period under the model's figure after a hold, set while the
	 * calibration stands. The hold then latches 320, inside the tolerance
	 * of line 0. */
	atomic_store_explicit(&tc_be.delay_bias, -80L, memory_order_release);
	CHECK(tc_lec_window_watch(&t, NULL));
	tc_cap_wait(&t, &k, 200);
	tc_engine_stats(&t, &st);
	CHECK_UINT((uint64_t)st.loop_ref, 320u);
	CHECK_UINT(atomic_load(&t.s->lec_loop), 320u);
	CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
	CHECK_UINT(ls.ref_delay_samples, 160u);
	CHECK_UINT(ls.retimes, 0);
	CHECK(ls.frames > before.frames);

	/* The new instance takes 320 verbatim on its first frame, a FEED of
	 * the configured stream, and stands one period under it. */
	mark = k.n;
	CHECK(tc_stream_config(&t, &w1, 1));
	tc_cap_wait(&t, &k, 100);
	i = tc_cap_new_find(&k, mark, 1);
	CHECK(i < k.n);
	if (i < k.n) {
		CHECK_UINT(k.rec[i].kind, UDSP_LEC_CAPTURE_FEED);
		CHECK_UINT(k.rec[i].flags, UDSP_LEC_CAPTURE_NEW |
			   UDSP_LEC_CAPTURE_SKIP_INACTIVE);
		CHECK_UINT(k.rec[i].ref_delay_samples, 240u);
	}
	CHECK_UINT(tc_cap_delay_bad(&k, mark, 1, 240u), 0);
	CHECK_UINT(tc_cap_delay_bad(&k, 0, 0, 160u), 0);
	CHECK(udsp_line_lec_stats(t.s, 1, &ls) == UDSP_OK);
	CHECK_UINT(ls.ref_delay_samples, 240u);
	CHECK_UINT(ls.ref_ready, 1);

	CASE("a released line publishes no reference");
	CHECK(tc_stream_down(&t, &w1, 1));
	CHECK(!atomic_load(&t.s->lec_ready[1]));
	CHECK_UINT(atomic_load(&t.s->lec_delay[1]), 0u);
	tc_cap_wait(&t, &k, 50);
	CHECK(udsp_line_lec_stats(t.s, 1, &ls) == UDSP_UNSUPPORTED);

	CASE("a hold at idle does not seed the next canceller with a stale "
	     "value");
	CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
	frames = ls.frames;
	/* The bias goes back one more period, and the hold latches 240. Line 1
	 * has no instance through it, so it decides nothing. */
	atomic_store_explicit(&tc_be.delay_bias, -160L, memory_order_release);
	CHECK(tc_lec_window_watch(&t, NULL));
	tc_cap_wait(&t, &k, 200);
	tc_engine_stats(&t, &st);
	CHECK_UINT((uint64_t)st.loop_ref, 240u);
	CHECK_UINT(atomic_load(&t.s->lec_loop), 240u);
	CHECK(!atomic_load(&t.s->lec_ready[1]));
	CHECK_UINT(atomic_load(&t.s->lec_delay[1]), 0u);
	CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
	CHECK_UINT(ls.ref_delay_samples, 160u);
	CHECK_UINT(ls.retimes, 0);
	CHECK(ls.frames > frames);

	mark = k.n;
	CHECK(tc_stream_config(&t, &w1, 1));
	tc_cap_wait(&t, &k, 100);
	i = tc_cap_new_find(&k, mark, 1);
	CHECK(i < k.n);
	if (i < k.n)
		CHECK_UINT(k.rec[i].ref_delay_samples, 160u);
	CHECK_UINT(tc_cap_delay_bad(&k, mark, 1, 160u), 0);

	/* Line 0 through every step: no reset, one reference, no loss. */
	CHECK_UINT(tc_cap_count(&k, 0, 0, UDSP_LEC_CAPTURE_RUN,
				UDSP_LEC_CAPTURE_RESET,
				UDSP_LEC_CAPTURE_RESET), 0);
	CHECK_UINT(tc_cap_delay_bad(&k, 0, 0, 160u), 0);
	CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
	CHECK_UINT(ls.retimes, 0);
	CHECK_UINT(k.over, 0);
	CHECK_UINT(tc_cap_loss_bad(&k, 0, &lost), 0);
	CHECK_UINT(lost, 0);
	CHECK(udsp_lec_capture_enable(t.s, false) == 0);
	CHECK(tc_stream_down(&t, &w1, 1));
	CHECK(tc_stream_down(&t, &w0, 0));
	tc_down(&t);
	free(k.rec);
}

/*
 * The canceller of line 0 with the tick parked, so the test thread reads the
 * instance the tick owns. The park follows the store of the tick, so every
 * write of the instance before it is visible. false when no instance stands.
 */
static bool tc_lec_parked_read(struct eh_gate *gate, struct eh *t,
			       unsigned *loss, struct udsp_ec_nlms_info *info)
{
	struct udsp_ec *ec;
	bool parked = eh_gate_park(gate);

	ec = atomic_load(&t->s->chan[0].ec);
	if (parked && ec) {
		*loss = udsp_ec_nlms_ref_loss_get(ec);
		udsp_ec_nlms_info_get(ec, info);
	}
	eh_gate_disarm(gate);

	return parked && ec;
}

/* Waits until the canceller of line 0 ran at least frames frames. */
static bool tc_lec_ran_wait(struct eh *t, uint32_t frames)
{
	uint64_t end = eh_mono_us() + 5000000u;
	struct udsp_lec_stats ls;

	do {
		eh_wait_ms(t, 10);
		if (udsp_line_lec_stats(t->s, 0, &ls) == UDSP_OK &&
		    ls.frames >= frames)
			return true;
	} while (eh_mono_us() < end);

	return false;
}

/*
 * The receive loss of the port stands ahead of the canceller reference
 * (core/chan.c chan_rx_split_apply), so the tick gives it to every instance
 * of the line. The control thread creates an instance at stream config with a
 * loss of 0, and releases it after the last stream leaves. A changed loss
 * starts a running instance over.
 */
static void case_lec_ref_loss(void)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	struct udsp_ec_nlms_info info = { 0 }, before = { 0 };
	struct udsp_chan_cfg ccfg;
	struct tc_stream w;
	unsigned loss = 0;
	struct eh t;

	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.ec_enable = true;
	ccfg.slic_gain_rx = -18;

	CASE("the loss reaches a new canceller instance");
	{
		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		/* The tick runs no canceller on an open loop. */
		CHECK(eh_hook_seize(&t));
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(tc_stream_up(&t, &w, 0));
		CHECK(tc_lec_ran_wait(&t, 2));
		CHECK(tc_lec_parked_read(&gate, &t, &loss, &info));
		CHECK_UINT(loss, 18);

		/* The last stream leaves, so the line releases its instance. */
		CHECK(tc_stream_down(&t, &w, 0));
		CHECK(atomic_load(&t.s->chan[0].ec) == NULL);
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(tc_stream_up(&t, &w, 0));
		CHECK(tc_lec_ran_wait(&t, 2));
		loss = 0;
		CHECK(tc_lec_parked_read(&gate, &t, &loss, &info));
		CHECK_UINT(loss, 18);
		CHECK(tc_stream_down(&t, &w, 0));
		tc_down(&t);
		eh_gate_close(&gate);
	}

	/*
	 * The tick applies the new loss before it runs the frame, and the
	 * setter starts the instance over, so the first frame after the change
	 * counts as frame 1. The count before the change stands far above the
	 * few ticks the park takes.
	 */
	CASE("a changed loss starts the running instance over");
	{
		gate = (struct eh_gate){ .fd = { -1, -1 } };
		if (!tc_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(eh_hook_seize(&t));
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(tc_stream_up(&t, &w, 0));
		CHECK(tc_lec_ran_wait(&t, 100));
		CHECK(tc_lec_parked_read(&gate, &t, &loss, &before));
		CHECK_UINT(loss, 18);
		CHECK(before.frames >= 100u);

		ccfg.slic_gain_rx = -30;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(tc_lec_parked_read(&gate, &t, &loss, &info));
		CHECK_UINT(loss, 30);
		CHECK(info.frames < before.frames);
		printf("# lec ref loss: frames %u before the change, "
		       "%u after\n", before.frames, info.frames);
		CHECK(tc_stream_down(&t, &w, 0));
		tc_down(&t);
		eh_gate_close(&gate);
	}
}

int main(void)
{
	/* A starved tick or net thread can miss 100 wall ms under sanitizers.
	 * Keep an explicit caller override for deadline tests. */
	if (setenv("UDSP_SYNC_TICK_MS", "1000", 0) ||
	    setenv("UDSP_NET_ACK_MS", "1000", 0))
		return 1;

	tc_page = mmap(NULL, sizeof(*tc_page), PROT_READ | PROT_WRITE,
		       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	tc_child = mmap(NULL, sizeof(*tc_child), PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (tc_page == MAP_FAILED || tc_child == MAP_FAILED)
		return 1;

	case_ack_defaults();
	/* The crash cases fork, so they run while the process is still single
	 * threaded and no other thread can hold a libc lock. */
	case_crash();
	case_lifecycle();
	case_cap();
	case_jb_debug();
	case_hook_query();
	case_hook();
	case_hook_gap();
	case_hook_pulse();
	case_hook_pulse_invert();
	case_slic_modes();
	case_hook_gaps();
	case_ring();
	case_cid_type1();
	case_cid_type2();
	case_vmwi();
	case_tone();
	case_digit();
	case_level();
	case_cid_level();
	case_detect();
	case_wideband();
	case_log_sink();
	case_rt_line();
	case_lec_stats();
	case_region();
	case_line_test_cap();
	case_line_test_result();
	case_line_test_gate();
	case_line_test_fail();
	case_line_test_hook();
	case_sync();
	case_ack();
	case_confirmed_state();
	case_slic_gain();
	case_slic_region();
	case_fault();
	case_recover();
	case_dead_retry();
	case_start_down();
	case_monitor();
	case_emitter();
	case_restart();
	case_watchdog();
	case_punch();
	case_hold();
	case_rephase_reference();
	case_rephase_line();
	case_rephase_order();
	case_hold_receive();
	case_watchdog_default();
	case_skip_clock();
	case_playout_skip();
	case_lec_ref_rule();
	case_lec_hold();
	case_engine_diag();
	case_engine_diag_hold();
	case_engine_diag_deferred();
	case_loop();
	case_cid_rec_fail();
	case_lec_verdicts();
	case_lec_hook();
	case_lec_trace();
	case_lec_skip();
	case_lec_capture();
	case_lec_capture_stop_timeout();
	case_ul_offset();
	case_ul_offset_floor();
	case_ul_offset_mute();
	case_ul_offset_clip();
	case_ul_offset_partial();
	case_lec_digit_hold();
	case_lec_ref_lines();
	case_lec_ref_adopt();
	case_lec_ref_loss();

	return tu_summary("core");
}
