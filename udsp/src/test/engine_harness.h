// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The engine harness that test_core.c and test_media.c share: the common part
 * of their paced fake TDM back ends, and the event, wait, job and hook seize
 * helpers of a handle that runs on one of them.
 *
 * The back end paces read_period on CLOCK_MONOTONIC at a settable period and
 * records the playback of channel 0. It also records channel 1 when the suite
 * allocates that record. From block_after on, a read waits on block_fd, and
 * the gate helpers at the end of this file drive that wait. Each suite adds
 * its own capture source and its own echo model.
 *
 * A suite defines EH_WAIT_SLACK_US before it includes this file. It is the
 * wall-time margin that eh_wait_ms() adds to the engine span.
 */
#ifndef UDSP_TEST_ENGINE_HARNESS_H
#define UDSP_TEST_ENGINE_HARNESS_H

#include <errno.h>
#include <math.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "udsp/tdm.h"
#include "udsp/udsp.h"

#include "../core/udsp_priv.h"
#include "../dsp/dsp.h"
#include "../slic/fake.h"
#include "level.h"
#include "util.h"

#ifndef EH_WAIT_SLACK_US
#error "define EH_WAIT_SLACK_US before engine_harness.h"
#endif

/* 1 in an address or thread sanitizer build. GCC defines the __SANITIZE_*
 * macros, and clang answers __has_feature. */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define EH_SANITIZED		1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define EH_SANITIZED		1
#endif
#endif
#ifndef EH_SANITIZED
#define EH_SANITIZED		0
#endif

#define EH_PACE_US		1000u
#define EH_REC_SAMPLES		400000u
#define EH_EV_MAX		4096u
/* The watchdog latches fatal at the first unchanged tick sample. A bound
 * below one interval usually lets the gate CHECK fail first, but the sample
 * phase can still let the watchdog latch first. */
#define EH_GATE_WAIT_US		800000u
#define EH_GATE_STEP_FRAMES	20u

static struct {
	/* A case can change the period while the tick runs. */
	_Atomic uint_least32_t pace_us;
	uint64_t deadline_us;
	bool paced;
	unsigned channels;
	_Atomic uint_least64_t reads;
	_Atomic int block_fd;
	_Atomic uint_least64_t block_after;
	_Atomic bool block_entered;
	int16_t *rec;
	/* The playback of channel 1, recorded beside channel 0 so a case can
	 * watch both ports of a cross connection. */
	int16_t *rec1;
	/* The engine thread appends and the test thread reads, so the length
	 * is the release point. The base lets a case take a fresh window
	 * without a rewind of a buffer that the engine still writes. */
	_Atomic uint_least32_t rec_len;
	unsigned rec_base;
	struct udsp_tdm_geom geom;
	int token;
} eh_be;

static inline uint64_t eh_mono_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static inline uint32_t eh_now_ms(void)
{
	return (uint32_t)(atomic_load_explicit(&eh_be.reads,
					       memory_order_acquire) * 10u);
}

static inline void eh_sleep_ms(unsigned ms)
{
	struct timespec ts = { .tv_sec = ms / 1000,
			       .tv_nsec = (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

/* Runs before udsp_open, so no engine thread reads the fields yet. */
static inline void eh_be_reset(bool rec1)
{
	atomic_store(&eh_be.pace_us, EH_PACE_US);
	eh_be.deadline_us = 0;
	eh_be.paced = true;
	eh_be.channels = 0;
	atomic_store(&eh_be.reads, 0u);
	atomic_store(&eh_be.block_fd, -1);
	atomic_store(&eh_be.block_after, UINT64_MAX);
	atomic_store(&eh_be.block_entered, false);
	atomic_store(&eh_be.rec_len, 0u);
	eh_be.rec_base = 0;
	if (!eh_be.rec)
		eh_be.rec = calloc(EH_REC_SAMPLES, sizeof(*eh_be.rec));
	if (rec1 && !eh_be.rec1)
		eh_be.rec1 = calloc(EH_REC_SAMPLES, sizeof(*eh_be.rec1));
}

static inline unsigned eh_rec_have(void)
{
	return (unsigned)atomic_load_explicit(&eh_be.rec_len,
					      memory_order_acquire) -
	       eh_be.rec_base;
}

static inline void eh_rec_mark(void)
{
	eh_be.rec_base = (unsigned)atomic_load_explicit(&eh_be.rec_len,
							memory_order_acquire);
}

/* One period of wall time, so the engine runs at the cadence a real ring
 * would pace it at. */
static inline void eh_pace(uint64_t now)
{
	if (!eh_be.deadline_us)
		eh_be.deadline_us = now;
	eh_be.deadline_us += (uint64_t)atomic_load_explicit(&eh_be.pace_us,
							   memory_order_relaxed);
	if (eh_be.deadline_us <= now)
		return;

	struct timespec rel = {
		.tv_sec = 0,
		.tv_nsec = (long)(eh_be.deadline_us - now) * 1000L,
	};

	nanosleep(&rel, NULL);
}

/* True when the read must return -EAGAIN: the gate holds the tick. */
static inline bool eh_wedge(uint64_t reads)
{
	int wedge = atomic_load(&eh_be.block_fd);
	char sink;

	if (wedge < 0 || reads < atomic_load(&eh_be.block_after))
		return false;
	/* This boundary follows playback, unlike the read count. */
	atomic_store_explicit(&eh_be.block_entered, true, memory_order_release);
	/* A stale byte costs one EAGAIN because each call checks the block
	 * condition again. Reuse and a timeout disarm therefore cannot
	 * advance a frame through a stale wake. */
	while (read(wedge, &sink, 1) < 0 && errno == EINTR)
		continue;

	return true;
}

static inline void eh_rec_add(const int16_t *buf)
{
	unsigned frames = eh_be.geom.frames_per_period;
	unsigned len = (unsigned)atomic_load_explicit(&eh_be.rec_len,
						      memory_order_relaxed);

	if (!eh_be.rec || len + frames > EH_REC_SAMPLES)
		return;
	memcpy(eh_be.rec + len, buf, frames * sizeof(*buf));
	if (eh_be.rec1 && eh_be.geom.channels > 1u)
		memcpy(eh_be.rec1 + len, buf + frames, frames * sizeof(*buf));
	atomic_store_explicit(&eh_be.rec_len, len + frames,
			      memory_order_release);
}

static inline struct udsp_tdm *eh_open_geom(const struct udsp_tdm_geom *want,
					    struct udsp_tdm_geom *got)
{
	*got = *want;
	got->paced = eh_be.paced;
	if (eh_be.channels)
		got->channels = eh_be.channels;
	eh_be.geom = *got;

	return (struct udsp_tdm *)&eh_be.token;
}

static inline void eh_close_be(struct udsp_tdm *t) { }
static inline int eh_recover(struct udsp_tdm *t) { return 0; }
static inline int eh_stop_be(struct udsp_tdm *t) { return 0; }

static inline long eh_delay(struct udsp_tdm *t)
{
	return (long)(eh_be.geom.prefill_periods - 1) *
	       eh_be.geom.frames_per_period;
}

/*
 * An independent Goertzel at a rate the caller states. dsp/goertzel.c compiles
 * the narrowband rate in, so a 16 kHz record and a G.722 frame need their own
 * reference. It also tells a tone from its rate-error image, which is the
 * shape a missing resampler leaves behind. For a sine of amplitude A at the
 * bin frequency over n samples, the power reads (A * n / 2)^2.
 */
static inline double eh_pcm_tone_power(const int16_t *pcm, unsigned n,
				       double hz, unsigned rate)
{
	double coeff = 2.0 * cos(2.0 * M_PI * hz / (double)rate);
	double q1 = 0.0;
	double q2 = 0.0;

	for (unsigned i = 0; i < n; i++) {
		double q0 = (double)pcm[i] + coeff * q1 - q2;

		q2 = q1;
		q1 = q0;
	}

	return q1 * q1 + q2 * q2 - coeff * q1 * q2;
}

/* eh_pcm_tone_power over the record, or 0 when the record is short of it. */
static inline double eh_tone_power(unsigned from, unsigned len, double hz,
				   unsigned rate)
{
	if (from + len > eh_rec_have())
		return 0.0;

	return eh_pcm_tone_power(eh_be.rec + eh_be.rec_base + from, len, hz,
				 rate);
}

/* The level of one tone of a window of the record, in dBm0, from its
 * amplitude A = 2 * sqrt(power) / len. */
static inline double eh_tone_dbm0(unsigned from, unsigned len, double hz,
				  unsigned rate)
{
	double a = 2.0 * sqrt(eh_tone_power(from, len, hz, rate)) / len;

	return rms_dbm0_get(a / sqrt(2.0));
}

static inline double eh_power(unsigned from, unsigned len, double hz)
{
	struct udsp_goertzel_bank bank;

	if (from + len > eh_rec_have())
		return 0.0;
	udsp_goertzel_init(&bank, &hz, 1);
	udsp_goertzel_process(&bank, eh_be.rec + eh_be.rec_base + from, len);

	return udsp_goertzel_power(&bank, 0);
}

/* The mean square of the window, or 0 when the record is short of it. */
static inline double eh_energy(unsigned from, unsigned len)
{
	double sum = 0.0;

	if (from + len > eh_rec_have())
		return 0.0;
	for (unsigned i = 0; i < len; i++) {
		int16_t v = eh_be.rec[eh_be.rec_base + from + i];

		sum += (double)v * v;
	}

	return sum / len;
}

/* --- the handle and its events ------------------------------------------- */

struct eh {
	udsp_t *s;
	int rx_fd;			/* the read end of the test */
	int tx_fd;			/* the end that udsp_open takes */
	/* The case stopped the handle itself, so the teardown skips udsp_stop. */
	bool stopped;
};

static struct udsp_event eh_ev[EH_EV_MAX];
static unsigned eh_ev_len;

static inline void eh_ev_reset(void)
{
	eh_ev_len = 0;
}

static inline unsigned eh_ev_poll(struct eh *t)
{
	struct udsp_event ev;
	unsigned got = 0;

	while (recv(t->rx_fd, &ev, sizeof(ev), MSG_DONTWAIT) ==
	       (ssize_t)sizeof(ev)) {
		if (eh_ev_len < EH_EV_MAX)
			eh_ev[eh_ev_len++] = ev;
		got++;
	}

	return got;
}

/* Waits in the engine time base, not the wall clock, so a case is written in
 * the units the line machines use. The wall ceiling only keeps a broken run
 * bounded. each, when given, runs on every pass and once at the end. */
static inline void eh_wait_each(struct eh *t, uint32_t ms,
				void (*each)(void *arg), void *arg)
{
	uint32_t end = eh_now_ms() + ms;
	uint64_t stop = eh_mono_us() + (uint64_t)ms * 1000u + EH_WAIT_SLACK_US;

	while (eh_now_ms() < end && eh_mono_us() < stop) {
		eh_ev_poll(t);
		if (each)
			each(arg);
		eh_sleep_ms(1);
	}
	eh_ev_poll(t);
	if (each)
		each(arg);
}

static inline void eh_wait_ms(struct eh *t, uint32_t ms)
{
	eh_wait_each(t, ms, NULL, NULL);
}

static inline const struct udsp_event *eh_ev_find(unsigned type, unsigned from)
{
	for (unsigned i = from; i < eh_ev_len; i++) {
		if (eh_ev[i].type == type)
			return &eh_ev[i];
	}

	return NULL;
}

static inline unsigned eh_ev_count(unsigned type)
{
	unsigned n = 0;

	for (unsigned i = 0; i < eh_ev_len; i++) {
		if (eh_ev[i].type == type)
			n++;
	}

	return n;
}

static inline unsigned eh_seize_count(void)
{
	unsigned n = 0;

	for (unsigned i = 0; i < eh_ev_len; i++)
		n += eh_ev[i].type == UDSP_EV_HOOK && eh_ev[i].line == 0 &&
		     eh_ev[i].u.hook.state == UDSP_HOOK_SEIZE;

	return n;
}

/*
 * Takes line 0 off hook and waits, 5 s at most, for one more SEIZE report
 * than the record held before. The report comes from the tick, so the tick
 * consumed the level that the canceller gate reads; a hook query proves only
 * the publish of the slic thread. The wait counts from the reports already in
 * the record, so an earlier SEIZE of the case cannot end it. A lift before the
 * first sample makes the boot report the SEIZE, which proves the level too.
 */
static inline bool eh_hook_seize(struct eh *t)
{
	uint64_t end = eh_mono_us() + 5000000u;
	unsigned want;

	eh_ev_poll(t);
	want = eh_seize_count() + 1u;
	udsp_slic_fake_hook_set(t->s->slic, 0, true);
	for (;;) {
		eh_ev_poll(t);
		if (eh_seize_count() >= want)
			return true;
		if (eh_mono_us() >= end)
			return false;
		eh_sleep_ms(1);
	}
}

static inline bool eh_job_wait(struct eh *t, uint32_t token, unsigned ms,
			       int32_t *status)
{
	uint64_t end = eh_mono_us() + (uint64_t)ms * 1000u;

	while (eh_mono_us() < end) {
		eh_ev_poll(t);
		for (unsigned i = 0; i < eh_ev_len; i++) {
			if (eh_ev[i].type != UDSP_EV_JOB_DONE ||
			    eh_ev[i].u.job.token != token)
				continue;
			if (status)
				*status = eh_ev[i].u.job.status;
			return true;
		}
		eh_sleep_ms(1);
	}

	return false;
}

/*
 * Polls the canceller record of line 0 until it reads the given ec_active and
 * loop_open, 5 s at most. A SYNC-TICK op takes effect in the tick that acks
 * it, and that tick publishes its record after the ack (core/chan.c
 * udsp_engine_hook). The tick also skips a publish while a reader holds the
 * slot (core/snap.h). A read right after an op can therefore return a record
 * from before the op. A case that switches the filter off waits for ec_active
 * 0. If the filter ran on every tick before the op, that record carries the
 * counters of the last frame the filter ran.
 */
static inline bool eh_lec_state_wait(struct eh *t, struct udsp_lec_stats *ls,
				     bool ec_active, bool loop_open)
{
	uint64_t end = eh_mono_us() + 5000000u;

	do {
		eh_wait_ms(t, 5);
		if (udsp_line_lec_stats(t->s, 0, ls) != UDSP_OK)
			continue;
		if (ls->ec_active == ec_active && ls->loop_open == loop_open)
			return true;
	} while (eh_mono_us() < end);

	return false;
}

/* --- the gate that holds the tick ---------------------------------------- */

struct eh_gate {
	int fd[2];
};

static inline bool eh_gate_wait(void)
{
	uint64_t end = eh_mono_us() + EH_GATE_WAIT_US;
	bool entered;

	while (!atomic_load_explicit(&eh_be.block_entered,
				     memory_order_acquire) &&
	       eh_mono_us() < end)
		sched_yield();
	entered = atomic_load_explicit(&eh_be.block_entered,
				       memory_order_acquire);
	CHECK(entered);

	return entered;
}

static inline bool eh_gate_wake(struct eh_gate *gate)
{
	char one = 'x';
	ssize_t n;

	do {
		n = write(gate->fd[1], &one, 1);
	} while (n < 0 && errno == EINTR);
	CHECK(n == 1);

	return n == 1;
}

/* The caller sets a new gate to { -1, -1 }. The first park opens the pipe,
 * and a later park of the same gate reuses it. */
static inline bool eh_gate_park(struct eh_gate *gate)
{
	if (gate->fd[0] < 0) {
		int rc = pipe(gate->fd);

		CHECK_UINT(rc, 0);
		if (rc) {
			gate->fd[0] = gate->fd[1] = -1;
			return false;
		}
	}
	atomic_store(&eh_be.block_entered, false);
	atomic_store(&eh_be.block_after, atomic_load(&eh_be.reads));
	atomic_store(&eh_be.block_fd, gate->fd[0]);

	return eh_gate_wait();
}

static inline bool eh_gate_step_once(struct eh_gate *gate, unsigned frames)
{
	bool entered = atomic_load(&eh_be.block_entered);
	uint64_t reads = atomic_load(&eh_be.reads);
	uint64_t want = reads + frames;

	CHECK(entered);
	if (!entered)
		return false;
	atomic_store(&eh_be.block_after, want);
	atomic_store(&eh_be.block_entered, false);
	if (!eh_gate_wake(gate) || !eh_gate_wait())
		return false;
	reads = atomic_load(&eh_be.reads);
	CHECK_UINT(reads, want);

	return reads == want;
}

static inline bool eh_gate_step(struct eh_gate *gate, unsigned frames)
{
	/* A long grant must not share one wall-time acknowledgment budget. */
	while (frames > EH_GATE_STEP_FRAMES) {
		if (!eh_gate_step_once(gate, EH_GATE_STEP_FRAMES))
			return false;
		frames -= EH_GATE_STEP_FRAMES;
	}

	return eh_gate_step_once(gate, frames);
}

static inline void eh_gate_disarm(struct eh_gate *gate)
{
	atomic_store(&eh_be.block_after, UINT64_MAX);
	if (atomic_exchange(&eh_be.block_fd, -1) >= 0)
		eh_gate_wake(gate);
}

static inline void eh_gate_close(struct eh_gate *gate)
{
	if (gate->fd[0] >= 0)
		close(gate->fd[0]);
	if (gate->fd[1] >= 0)
		close(gate->fd[1]);
}

#endif
