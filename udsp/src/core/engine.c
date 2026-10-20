// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The engine tick loop. One thread, one tdm plug-in, no channels, no streams,
 * no events of its own: read one capture period, put either that period
 * (loopback) or silence into playback, run the injected per-tick hook, write
 * it, measure. Everything else sits on top.
 *
 * Startup order. udsp.h section 2 fixes the order of the SLIC against the PCM
 * clocks; it says nothing about the worker. The clocks must not run while
 * nobody drains capture or refills playback, so the worker is created first and
 * finishes its real-time setup, then parks on a gate; only then does the
 * control thread prefill playback and start the streams, and the gate opens on
 * a loop that is ready to tick. The two semaphores also keep the counter block
 * single-owner: the worker owns it until it posts ready, the control thread
 * owns it until it posts the gate.
 *
 * Prefill depth. tdm_ops.open takes the wanted depth and reports the granted
 * buffer depth in periods. A prefill that fills the whole ring leaves the
 * first tick's write_period with nowhere to put its period, and on ALSA that
 * write blocks with the stream not started (tdm/alsa.c open note), so the
 * engine asks for one period more than it prefills and clamps its prefill to
 * the granted depth minus one.
 *
 * What a stall is here. tdm_ops.avail is nullable. A backend that implements
 * it (both built-in ones do) is measured directly: a capture backlog above one
 * period after the read is the stall. A backend without it keeps the inference
 * from the engine's own tick interval, where a gap above two periods means at
 * least one period queued up behind the blocking read. The playback half is
 * direct either way: delay() drift beyond one period from the calibrated
 * reference, where engine_delay_drift states the one-period quantization
 * allowance. The drop/insert resync belongs with the streams.
 *
 * The catch-up rule. A kernel holder on the engine CPU parks the tick for tens
 * of milliseconds. The back end does not stop the ring for it (tdm.h), so
 * the engine returns to a deep capture backlog and a playback ring it refills
 * in a few immediate ticks. From there the blocking write paces the loop at
 * one period per tick and the backlog stands for the life of the stream, which
 * is uplink latency nothing heals. A back end that offers the three fresh
 * reads (capture_avail_fresh, playback_room_fresh, capture_skip) therefore
 * enters a hold episode, drops the whole waiting backlog once the playback
 * queue is back at its rest depth, and leaves the episode when the backlog is
 * back inside one period. engine_catchup_run holds the rule and its reasons.
 *
 * Either side of the loop opens the episode: a capture backlog of two periods
 * after the read, or a playback queue more than one period below the
 * calibrated reference after the write. The second is not redundant. A holder
 * that parks the engine inside a write that waits leaves the queue, not the
 * backlog, to report it first.
 *
 * What the episode leaves behind. The uplink returns to its own depth, because
 * the trim takes every period the hold left. The playback queue returns to the
 * depth the first window after the start latched, because the refill stops
 * there (engine_rest_reached). No write is ever skipped. The calibration
 * restart puts the canceller's reference on the ring as it is.
 *
 * A playback ring that ran dry. On a back end whose playback DMA stops when no
 * period is queued and restarts at the next write (tdm.h tx_restarts), a hold
 * longer than the queue restarts the ring at the instant of the first write
 * after it. The refill above then counts periods that nothing has consumed
 * yet, and the ring settles one period deeper than after the linked start, or
 * at the phase of that write. The rephase restarts the ring the way the start
 * did instead: it drops capture until the ring is empty, then writes the
 * refill right after a capture completion, and it holds back a write while
 * the queue after the write reads a period too deep. The refill is downlink
 * audio, rendered on the first dropped read (engine_refill_render).
 * engine_rephase_read holds the rule and its reasons.
 *
 * The loop never logs, never allocates, never takes a mutex and uses no
 * floating point. The gate semaphore is waited on before the first tick.
 */
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "udsp/tdm.h"

#include "clock.h"
#include "engine.h"
#include "snap.h"
#include "thread.h"

#define ENGINE_STACK_BYTES	(256 * 1024)
#define ENGINE_RATE_HZ		8000u	/* the rate a zero asks for, tdm.h */
#define ENGINE_FAIL_LIMIT	10u	/* consecutive faults before giving up */
#define ENGINE_FINAL_TRIES	10000u	/* final publish retries, 100 us apart */
/* The writes the settle check reads after a rephase, the samples a period too
 * deep that hold one write back, and the writes it may hold back. */
#define ENGINE_REPHASE_SETTLE_WRITES	6u
#define ENGINE_REPHASE_DEEP_SAMPLES	2u
#define ENGINE_REPHASE_WITHHOLD_MAX	2u
/* Transient faults a refill write of the rephase retries in place. */
#define ENGINE_REFILL_TRIES		3u

/* The rephase of a playback ring that ran dry (engine_rephase_read). */
enum engine_rephase {
	ENGINE_REPHASE_NONE = 0,
	ENGINE_REPHASE_DRAIN,	/* capture is dropped until the ring is empty */
	ENGINE_REPHASE_SETTLE,	/* the ring restarted: the order is checked */
};

/* What the rephase makes of one capture read. */
enum engine_rephase_read {
	ENGINE_REPHASE_RUN = 0,	/* the hook and the write run */
	ENGINE_REPHASE_DROP,	/* the period is dropped: no hook, no write */
	ENGINE_REPHASE_RESET,	/* a recovery rebuilt both rings */
	ENGINE_REPHASE_STOP,	/* a fault ended the loop */
};

const uint32_t engine_hist_edge_us[ENGINE_HIST_BUCKETS - 1] = {
	9000, 11000, 12000, 15000, 20000, 30000, 50000,
};

/* Starts at 1 so no live engine ever reports serial 0, which is what a zeroed
 * consumer tracker holds (engine.h serial). */
static _Atomic uint_least64_t engine_serial_next = 1;

UDSP_SNAP_DECLARE(engine_snap, struct engine_stats);

struct engine {
	const struct udsp_tdm_ops *ops;
	struct udsp_tdm *tdm;
	struct udsp_tdm_geom geom;
	struct engine_opts opts;
	int16_t *rx;
	int16_t *tx;
	void *sig_stack;		/* tick thread only */
	stack_t sig_prev;
	size_t period_bytes;
	unsigned prefill;
	unsigned prefill_want;
	uint32_t period_us;
	int cpu;
	int prio;

	pthread_t thread;
	bool thread_live;		/* control thread only */
	bool gate_released;		/* control thread only */
	bool deferred_start;		/* set at open, cleared by the start */
	bool sem_live;			/* control thread only */
	sem_t ready;			/* worker: real-time setup finished */
	sem_t gate;			/* control: streams are running, tick */
	atomic_bool stop;
	atomic_bool done;
	/* The master clock every other libudsp thread stamps against: the tick
	 * index and the microsecond mark of that tick. */
	_Atomic uint_least64_t clock_tick;
	_Atomic uint_least64_t clock_mono_us;

	struct engine_stats live;	/* single owner, see Startup order */
	uint64_t last_us;		/* tick thread only */
	unsigned fail_run;		/* tick thread only */
	bool tx_pending;		/* tick thread only: tx holds a period */
	bool catching_up;		/* tick thread only: an episode runs */
	/* Tick thread only: capture periods the ring overwrote or the trim
	 * dropped since the last skip hook call. */
	unsigned skipped;
	long cal[ENGINE_DELAY_CAL];	/* tick thread only: calibration window */
	long cal_loop[ENGINE_DELAY_CAL];	/* tick thread only */
	unsigned cal_count;		/* tick thread only */
	/* Tick thread only: the playback queue, in frames after the write, of
	 * the first window that completed after a start or a recovery, and
	 * whether it stands. A hold end restarts the window but keeps this
	 * value, because the catch-up refills the ring to it
	 * (engine_rest_reached). */
	long rest_ref;
	bool rest_latched;
	/* Tick thread only: the rephase of a dry ring (engine_rephase_read).
	 * budget counts the reads left to the drain; fresh says the backlog
	 * stood clear after the work of the last dropped read, so the next
	 * read waits for a new completion; writes and deep belong to the
	 * settle check; withhold
	 * drops the next read. barred says an attempt aborted: no attempt
	 * starts before a calibration window completes, a recovery, or an
	 * episode that a late read or write opens. */
	uint8_t rephase;
	bool rephase_fresh;
	bool rephase_withhold;
	bool rephase_barred;
	unsigned rephase_budget;
	unsigned rephase_writes;
	unsigned rephase_deep;
	unsigned rephase_withheld;
	/* Tick thread only: the refill (engine_refill_render). stage holds
	 * stage_cap periods, the most a refill takes; staged counts the
	 * periods it holds and sent the ones written. lead counts rendered
	 * periods that no skipped period has paid for yet. */
	int16_t *stage;
	unsigned stage_cap;
	unsigned staged;
	unsigned staged_sent;
	unsigned lead;

	/* The tick thread publishes e->live here; a reader copies one whole
	 * consistent slot (snap.h). */
	struct engine_snap snap;
};

static const struct udsp_tdm_ops *engine_backend_find(const char *name)
{
	if (!strcmp(name, "file"))
		return &udsp_tdm_file;
	if (!strcmp(name, "alsa-raw"))
		return &udsp_tdm_alsa_raw;
#ifdef HAVE_ALSA
	if (!strcmp(name, "alsa"))
		return &udsp_tdm_alsa;
#endif
	return NULL;
}

/* Every backend fills geom.paced (tdm/file.c and tdm/alsa.c both do), so the
 * field is the answer and no ops-pointer identity test is needed. */
static bool engine_paced(const struct engine *e, const struct udsp_tdm_geom *got)
{
	(void)e;

	return got->paced;
}

static bool engine_publish(struct engine *e)
{
	return engine_snap_publish(&e->snap, &e->live);
}

/* Control thread, after the join. The last figures must reach the snapshot even
 * when a reader was mid-copy as the loop ended, or every later reader sees a
 * running engine that stopped. A reader holds the count for one
 * memcpy, so the retry ends in the first idle window. */
static void engine_publish_final(struct engine *e)
{
	struct timespec nap = { .tv_sec = 0, .tv_nsec = 100000 };

	for (unsigned i = 0; i < ENGINE_FINAL_TRIES; i++) {
		if (engine_publish(e))
			return;
		nanosleep(&nap, NULL);
	}
}

void engine_stats_get(struct engine *e, struct engine_stats *out)
{
	if (!e || !out)
		return;

	engine_snap_get(&e->snap, out);
}

bool engine_done(struct engine *e)
{
	return e && atomic_load(&e->done);
}

static unsigned engine_hist_bucket(uint32_t us)
{
	for (unsigned i = 0; i < ENGINE_HIST_BUCKETS - 1; i++) {
		if (us <= engine_hist_edge_us[i])
			return i;
	}

	return ENGINE_HIST_BUCKETS - 1;
}

static void engine_interval_note(struct engine *e, uint64_t now_us, bool infer)
{
	struct engine_stats *st = &e->live;
	uint64_t delta = now_us - e->last_us;

	if (!e->last_us) {
		e->last_us = now_us;
		return;
	}

	e->last_us = now_us;

	uint32_t us = delta > UINT32_MAX ? UINT32_MAX : (uint32_t)delta;

	st->interval[engine_hist_bucket(us)]++;
	if (!st->interval_samples || us < st->interval_min_us)
		st->interval_min_us = us;
	if (us > st->interval_max_us)
		st->interval_max_us = us;
	st->interval_samples++;

	if (infer && us > 2 * e->period_us)
		st->capture_stalls++;
}

static long engine_median(const long *v, unsigned n)
{
	long tmp[ENGINE_DELAY_CAL];

	memcpy(tmp, v, n * sizeof(*tmp));
	for (unsigned i = 1; i < n; i++) {
		long key = tmp[i];
		unsigned j = i;

		while (j && tmp[j - 1] > key) {
			tmp[j] = tmp[j - 1];
			j--;
		}
		tmp[j] = key;
	}

	return (tmp[n / 2 - 1] + tmp[n / 2]) / 2;
}

/* The driver's steady state is not the nominal prefill: the mt7986 AFE settles
 * about two periods below it, so a detector anchored on prefill * frames would
 * report a stall on most ticks. The reference is the median of the first
 * ENGINE_DELAY_CAL samples, which is immune to the ramp of the opening ticks
 * and to a single outlier. */
static void engine_delay_calibrate(struct engine *e, long queued, long loop)
{
	e->cal[e->cal_count] = queued;
	e->cal_loop[e->cal_count] = loop;
	e->cal_count++;
	if (e->cal_count < ENGINE_DELAY_CAL)
		return;

	e->live.delay_ref = (int32_t)engine_median(e->cal, ENGINE_DELAY_CAL);
	e->live.loop_ref = (int32_t)engine_median(e->cal_loop,
						  ENGINE_DELAY_CAL);
	e->live.delay_calibrated = true;
	e->rephase_barred = false;
	if (!e->rest_latched) {
		e->rest_ref = e->live.delay_ref;
		e->rest_latched = true;
	}
}

/* What a hold episode needs: the three fresh reads it decides on, and avail
 * for the backlog it opens on. All four or none. An episode the engine cannot
 * end by trimming would hold the delay stalls down and the calibration open
 * for the life of the stream; and without avail the read side could never open
 * one, while the tick-interval inference engine_interval_note falls back to
 * would count a second capture stall for the hold the playback side opened. A
 * back end short of any of them keeps the per-tick stall rule instead. */
static bool engine_trim_capable(const struct engine *e)
{
	return e->ops->avail && e->ops->capture_avail_fresh &&
	       e->ops->playback_room_fresh && e->ops->capture_skip;
}

/*
 * The start of a hold episode. Either side of the loop can find the engine
 * late, and the episode gets one capture stall whichever side opens it:
 * counting every late tick would inflate the figure by the length of the
 * burst, and counting both sides would report two stalls for one holder.
 */
static void engine_hold_open(struct engine *e)
{
	if (e->catching_up)
		return;

	e->catching_up = true;
	e->rephase_barred = false;
	e->live.holds++;
	e->live.capture_stalls++;
}

static void engine_delay_drift(struct engine *e, long queued)
{
	struct engine_stats *st = &e->live;
	int32_t drift = (int32_t)(queued - st->delay_ref);

	if (!st->delay_drift_samples || drift < st->delay_drift_min)
		st->delay_drift_min = drift;
	if (!st->delay_drift_samples || drift > st->delay_drift_max)
		st->delay_drift_max = drift;
	st->delay_drift_last = drift;
	st->delay_drift_samples++;

	int32_t mag = drift < 0 ? -drift : drift;

	/* Strictly beyond one period: snd_pcm_delay on MT7987 quantizes
	 * to period boundaries, so the drift reads exactly +80 about once a
	 * second at idle with xruns 0 and capture_stalls stable. A wobble of
	 * exactly one quantization step is the reading's granularity, not a
	 * queue drift. A hold episode moves the queue by design, and the
	 * episode already counts one capture stall, so the drift of those
	 * ticks is measured and reported but not counted as a fault. */
	if (mag <= (int32_t)e->geom.frames_per_period)
		return;
	/*
	 * Below the reference by more than a period: the hardware consumed
	 * more than this tick wrote, which only happens when the engine was
	 * away, and a holder that falls between the read and the write shows
	 * here first. So the playback side opens the episode here instead of
	 * counting a stall, and the read that finds the backlog next tick
	 * continues the same episode. Without this rule, an injected hold
	 * reports about two stalls.
	 */
	if (drift < 0 && engine_trim_capable(e)) {
		engine_hold_open(e);
		return;
	}
	if (!e->catching_up)
		st->delay_stalls++;
}

static void engine_delay_note(struct engine *e, long queued, long loop)
{
	struct engine_stats *st = &e->live;
	long want = (long)e->prefill * (long)e->geom.frames_per_period;
	int32_t dev = (int32_t)(queued - want);

	/* The raw deviation from the nominal prefill stays reported: it is the
	 * driver's constant bias, and it is compared across boards. */
	if (!st->delay_samples || dev < st->delay_dev_min)
		st->delay_dev_min = dev;
	if (!st->delay_samples || dev > st->delay_dev_max)
		st->delay_dev_max = dev;
	st->delay_dev_last = dev;
	st->delay_samples++;

	if (!st->delay_calibrated) {
		engine_delay_calibrate(e, queued, loop);
		return;
	}

	engine_delay_drift(e, queued);
}

static bool engine_prefill_write(struct engine *e)
{
	memset(e->tx, 0, e->period_bytes);

	for (unsigned i = 0; i < e->prefill; i++) {
		int rc = e->ops->write_period(e->tdm, e->tx);

		if (rc >= 0)
			continue;
		e->live.write_errors++;
		e->live.last_errno = -rc;
		return false;
	}

	return true;
}

/* The refill is gone. Its periods stay in the consumer's history, so the
 * references of the next ticks can read audio that the line never played. */
static void engine_stage_clear(struct engine *e)
{
	e->staged = 0;
	e->staged_sent = 0;
}

static void engine_rephase_clear(struct engine *e)
{
	e->rephase = ENGINE_REPHASE_NONE;
	e->rephase_fresh = false;
	e->rephase_withhold = false;
	e->rephase_budget = 0;
	e->rephase_writes = 0;
	e->rephase_deep = 0;
	e->rephase_withheld = 0;
}

static bool engine_resync_fail(struct engine *e, int rc)
{
	e->live.recover_failures++;
	if (rc < 0)
		e->live.last_errno = -rc;
	e->live.stopped_fatal = true;

	return false;
}

static bool engine_resync(struct engine *e)
{
	int rc = e->ops->recover(e->tdm);

	/* The ring was reset, so the captured period held for a retry is stale
	 * and the re-prefill must be silence (tdm.h Linked start). The playback
	 * queue it rebuilds is a new steady state, so the reference is measured
	 * again. */
	e->tx_pending = false;
	e->cal_count = 0;
	e->live.delay_calibrated = false;
	e->live.cal_restarts++;
	e->rest_latched = false;
	/* The prepare reset both rings, so the backlog the episode was
	 * catching up with is gone with them, and a rephase with them: the
	 * re-prefill and the linked start restart the ring as the open did. */
	e->catching_up = false;
	engine_rephase_clear(e);
	e->rephase_barred = false;
	engine_stage_clear(e);
	if (rc < 0)
		return engine_resync_fail(e, rc);
	if (!engine_prefill_write(e))
		return engine_resync_fail(e, 0);

	rc = e->ops->start(e->tdm);
	if (rc < 0)
		return engine_resync_fail(e, rc);

	/* The gap across a recovery is not a tick interval; the next tick
	 * starts a new measurement. */
	e->last_us = 0;

	return true;
}

/* EAGAIN and EINTR are transient: the caller retries the same operation and the
 * strike run is untouched. Every other fault counts, feeds the
 * strike run and recovers; ENGINE_FAIL_LIMIT consecutive ones end the loop. */
static bool engine_fault(struct engine *e, int rc, uint64_t *counter)
{
	struct engine_stats *st = &e->live;

	st->last_errno = -rc;
	if (rc == -EAGAIN || rc == -EINTR) {
		st->again++;
		return true;
	}

	(*counter)++;
	if (rc == -EPIPE || rc == -ESTRPIPE)
		st->xruns++;

	e->fail_run++;
	if (e->fail_run >= ENGINE_FAIL_LIMIT) {
		st->stopped_fatal = true;
		return false;
	}

	return engine_resync(e);
}

static void engine_payload_make(struct engine *e)
{
	if (e->opts.loopback) {
		memcpy(e->tx, e->rx, e->period_bytes);
		return;
	}

	memset(e->tx, 0, e->period_bytes);
}

/* The capture side: a backlog that deep is time the engine did not keep the
 * rate of the ring. */
static void engine_hold_note(struct engine *e, long queued)
{
	if (queued < 2 * (long)e->geom.frames_per_period)
		return;

	engine_hold_open(e);
}

/* The direct backlog measurement. avail() < 0 is a fault like delay()'s: it
 * counts and feeds the strike run. The figure is exact and costs no call here:
 * read_period pushed the application pointer with HWSYNC, and no write has run
 * since. The fresh reads belong to the trim, which decides between the hook
 * and the write. */
static bool engine_backlog_note(struct engine *e)
{
	long queued;

	if (!e->ops->avail)
		return true;

	queued = e->ops->avail(e->tdm);
	if (queued < 0)
		return engine_fault(e, (int)queued, &e->live.avail_errors);
	if (engine_trim_capable(e)) {
		engine_hold_note(e, queued);
		return true;
	}
	if (queued > (long)e->geom.frames_per_period)
		e->live.capture_stalls++;

	return true;
}

/* The periods the engine never handed to the hook are elapsed time: the tick
 * clock steps over them (engine_capture) and the skip hook passes the count to
 * the consumers whose own clocks run per frame. */
static void engine_skip_note(struct engine *e, unsigned periods,
			     uint64_t *counter)
{
	*counter += periods;
	e->skipped += periods;
}

static void engine_skip_hook(struct engine *e)
{
	if (!e->skipped)
		return;
	if (e->opts.hooks.skip)
		e->opts.hooks.skip(e->opts.hooks.ctx, e->skipped,
				   e->live.ticks);
	e->skipped = 0;
}

/*
 * The capture-side trim. The loop is paced by the playback write, so the
 * periods a hold leaves in the capture ring would stand as uplink latency for
 * the life of the stream: the fast ticks that follow a hold only refill the
 * playback ring, and from a full ring on, one period arrives for every period
 * written. The write is about to block, so every whole period waiting is that
 * excess and all of it goes. It is the capture side and not the playback side
 * because the write is never skipped: every period the downlink history and
 * the jitter buffer already recorded reaches the line. The application pointer
 * can never pass the hardware one, so the newest period the skip leaves is
 * never one the DMA is still filling.
 */
/* What the catch-up leaves the tick to do. A transient fault (EAGAIN, EINTR)
 * on any of the three calls keeps the captured period in tx and runs the whole
 * catch-up again on the next tick, before any write: the same retry contract
 * read_period and write_period have, so a period is never written past a
 * backlog the engine has not decided on yet. */
enum engine_catchup {
	ENGINE_CATCHUP_WRITE = 0,
	ENGINE_CATCHUP_RETRY,
	ENGINE_CATCHUP_STOP,
};

static enum engine_catchup engine_catchup_fault(struct engine *e, int rc,
						uint64_t *counter)
{
	if (!engine_fault(e, rc, counter))
		return ENGINE_CATCHUP_STOP;
	if (rc == -EAGAIN || rc == -EINTR)
		return ENGINE_CATCHUP_RETRY;

	/* Anything else recovered, which rebuilt both rings and cleared the
	 * pending period; engine_tick reads that and ends the tick. */
	return ENGINE_CATCHUP_WRITE;
}

static enum engine_catchup engine_trim_run(struct engine *e, long avail)
{
	unsigned period = e->geom.frames_per_period;
	unsigned n = (unsigned)(avail / (long)period);
	int rc = e->ops->capture_skip(e->tdm, n);

	if (rc < 0)
		return engine_catchup_fault(e, rc, &e->live.read_errors);
	engine_skip_note(e, n, &e->live.trim_drops);

	return ENGINE_CATCHUP_WRITE;
}

/*
 * The end of a hold episode. The backlog is back to one period, which is what
 * a tick finds at rest, so the catch-up is done and the delay calibration
 * starts again: the queues the canceller's reference is measured from are not
 * the ones the window latched before the hold. The restart moves the window
 * and nothing else. What the canceller does with the new measurement is the
 * consumer's decision (chan.c chan_lec_line_decide). A value within one period
 * of the applied reference keeps the taps. A value beyond it re-times them.
 * chan_lec_delay_track holds the canceller for the ENGINE_DELAY_CAL
 * ticks the new window takes: the filter does not adapt while no measurement
 * stands. That cost is accepted, because a reference that names the wrong
 * ring is worse than a short freeze. The nominal prefill it falls back to is
 * for a window that fails again and again, not for this restart.
 */
static void engine_hold_end(struct engine *e)
{
	e->catching_up = false;
	e->cal_count = 0;
	e->live.delay_calibrated = false;
	e->live.cal_restarts++;
}

/*
 * Whether the write of this tick puts the playback queue back at its rest
 * depth, so the catch-up may drop the capture backlog. room is the fresh
 * playback room in frames. The rest depth is the queue after the write that
 * the first window after a start or a recovery latched (rest_ref), or the
 * nominal prefill before that window completes.
 *
 * The test is half a period wide. The queue after a write only takes values
 * one period apart, because every write and every read moves one whole period
 * and a playback period that expired is written at the next period boundary
 * (tdm/alsa_raw.c tdm_raw_write). So exactly one write falls within half a
 * period of the rest depth, and the queue after it is the rest depth itself.
 *
 * A full ring also ends the refill, because the write would wait there.
 *
 * Reason for the rest depth and not the granted depth: a ring refilled to
 * the granted depth makes the write wait on every tick from then on, so the
 * tick computes each downlink period two periods before it plays, for the
 * life of the stream. On MT7988 at 16 kHz that is loop_ref 750 against
 * 430, and every downlink timing 20 ms late.
 */
static bool engine_rest_reached(const struct engine *e, long room)
{
	long period = (long)e->geom.frames_per_period;
	long ring = (long)e->geom.prefill_periods * period;
	long rest = e->rest_latched ? e->rest_ref : (long)e->prefill * period;
	long after = ring - room + period;

	if (room < period)
		return true;

	return 2 * after >= 2 * rest - period;
}

/*
 * The catch-up half of the tick, between the hook and the write. The fast ticks
 * after a hold spend the capture backlog on the downlink until the playback
 * queue is back at its rest depth (engine_rest_reached). From there every whole
 * period still waiting is time the hold took, and the trim drops it. Dropping
 * capture earlier would take audio the loop has not delayed yet. Dropping it
 * later would leave the downlink deeper than at rest.
 *
 * The episode does not end on the state of the write. The backlog alone says
 * whether the uplink is back: from the trim on, the read waits for its period
 * again, as it does at rest.
 */
static enum engine_catchup engine_catchup_run(struct engine *e)
{
	unsigned period = e->geom.frames_per_period;
	long room, avail;

	if (!e->catching_up)
		return ENGINE_CATCHUP_WRITE;

	room = e->ops->playback_room_fresh(e->tdm);
	if (room < 0)
		return engine_catchup_fault(e, (int)room,
					    &e->live.avail_errors);
	avail = e->ops->capture_avail_fresh(e->tdm);
	if (avail < 0)
		return engine_catchup_fault(e, (int)avail,
					    &e->live.avail_errors);

	if (avail >= (long)period && engine_rest_reached(e, room))
		return engine_trim_run(e, avail);
	/* A rephase keeps the episode open: its drops and its settle
	 * writes move the queues, and the calibration window must start on
	 * the ring it leaves. */
	if (avail <= (long)period && e->rephase == ENGINE_REPHASE_NONE)
		engine_hold_end(e);

	return ENGINE_CATCHUP_WRITE;
}

/* --- the rephase of a dry ring ------------------------------------------ */

static long engine_ring_frames(const struct engine *e)
{
	return (long)e->geom.prefill_periods * (long)e->geom.frames_per_period;
}

/* The rest depth in whole periods: the queue after a write that the first
 * window latched, or the prefill before that window completes. Both come from
 * the granted geometry, because the prefill is clamped to it. */
static unsigned engine_rest_periods(const struct engine *e)
{
	long period = (long)e->geom.frames_per_period;
	long rest = e->rest_latched ? e->rest_ref : (long)e->prefill * period;

	return (unsigned)((rest + period / 2) / period);
}

/*
 * Whether a dry ring can be restarted on a capture completion. The back end
 * must say that its playback DMA stops when empty and restarts at the next
 * write (tdm.h tx_restarts), and it must carry the catch-up calls. The rest
 * depth must be two periods or more and leave the ring a free period: the
 * refill is the rest depth less the write of the tick, and with one period
 * no write comes before the hook, so the restart would keep the phase of the
 * hook's write. Both directions share one period (tdm.h Geometry).
 */
static bool engine_rephase_capable(const struct engine *e)
{
	unsigned rest = engine_rest_periods(e);

	return e->geom.tx_restarts && engine_trim_capable(e) && rest >= 2u &&
	       rest < e->geom.prefill_periods;
}

static void engine_rephase_begin(struct engine *e)
{
	engine_rephase_clear(e);
	e->rephase = ENGINE_REPHASE_DRAIN;
	e->rephase_budget = e->geom.prefill_periods + 2u;
	e->live.rephase_starts++;
	/* The restart moves both queues, so it runs inside an episode: no
	 * delay stall counts and the calibration window starts after it. */
	engine_hold_open(e);
}

/* The attempt gives up, and the catch-up runs alone. The refill goes out
 * ahead of the write of this tick (engine_stage_flush). The bar keeps a write
 * that leaves one period on a ring that the drain emptied from starting the
 * next attempt at once: the episode ends on this tick, and a whole
 * calibration window of ordinary ticks comes first. */
static void engine_rephase_abort(struct engine *e)
{
	e->live.rephase_aborts++;
	engine_rephase_clear(e);
	e->rephase_barred = true;
}

/* A fault of one of the rephase's calls on a read it drops. A transient one
 * drops the period read and leaves the rest to the next read, which can come
 * from the backlog that stays, so that read is not fresh. A recovery rebuilt
 * both rings and cleared the rephase. */
static enum engine_rephase_read engine_rephase_fault(struct engine *e, int rc,
						     uint64_t *counter)
{
	e->rephase_fresh = false;
	if (!engine_fault(e, rc, counter))
		return ENGINE_REPHASE_STOP;
	if (rc != -EAGAIN && rc != -EINTR)
		return ENGINE_REPHASE_RESET;
	engine_skip_note(e, 1, &e->live.trim_drops);

	return ENGINE_REPHASE_DROP;
}

/* Drops the period just read and every whole period waiting behind it. The
 * drops are trims: the tick clock and the skip hook step over them. A clear
 * that worked marks the read for the freshness check after the work of the
 * drop (engine_drain_fresh); a fault leaves it unmarked. */
static enum engine_rephase_read engine_rephase_drop(struct engine *e)
{
	unsigned period = e->geom.frames_per_period;
	long avail = e->ops->capture_avail_fresh(e->tdm);
	unsigned n;
	int rc;

	if (avail < 0)
		return engine_rephase_fault(e, (int)avail,
					    &e->live.avail_errors);
	n = (unsigned)(avail / (long)period);
	if (n) {
		rc = e->ops->capture_skip(e->tdm, n);
		if (rc < 0)
			return engine_rephase_fault(e, rc,
						    &e->live.read_errors);
		engine_skip_note(e, n, &e->live.trim_drops);
	}
	engine_skip_note(e, 1, &e->live.trim_drops);
	e->rephase_fresh = true;

	return ENGINE_REPHASE_DROP;
}

/* --- the refill of a restart --------------------------------------------- */

static void engine_clock_publish(struct engine *e, uint64_t now_us)
{
	atomic_store_explicit(&e->clock_mono_us, now_us, memory_order_relaxed);
	atomic_store_explicit(&e->clock_tick, e->live.ticks, memory_order_release);
}

/* The periods a refill takes: the rest depth less the write of the tick that
 * follows it, as many as the stage holds. */
static unsigned engine_refill_periods(const struct engine *e)
{
	unsigned rest = engine_rest_periods(e);
	unsigned n = rest > 1u ? rest - 1u : 0u;

	return n < e->stage_cap ? n : e->stage_cap;
}

/* The refill is due on the first read the drain drops. */
static unsigned engine_refill_due(const struct engine *e)
{
	if (e->rephase != ENGINE_REPHASE_DRAIN || e->staged)
		return 0;

	return engine_refill_periods(e);
}

/* The skipped periods pay for the periods the render will compute, so the
 * skip hook gets that many fewer. What they cannot pay stands as lead. */
static void engine_refill_charge(struct engine *e, unsigned periods)
{
	unsigned paid = periods < e->skipped ? periods : e->skipped;

	e->skipped -= paid;
	e->lead += periods - paid;
	e->live.rephase_lead += periods - paid;
}

/* Lead from an earlier refill takes skipped periods before the skip hook
 * does, until the tick clock is back on the capture. */
static void engine_lead_pay(struct engine *e)
{
	unsigned paid = e->lead < e->skipped ? e->lead : e->skipped;

	e->skipped -= paid;
	e->lead -= paid;
}

/*
 * The refill of a restart, rendered on the first read the drain drops, right
 * after the drop. The read that a fresh completion wakes then writes it back
 * to back (engine_refill_write). The render runs before the wait for that
 * completion, and a completion that comes during it does not count
 * (engine_drain_fresh): the restart waits for the first one after it.
 *
 * The refill is downlink audio. Each render call is a tick without a capture
 * period: the tick clock steps one index per call, and the skip hook got that
 * many periods fewer (engine_refill_charge). So the clock and every consumer
 * clock that follows the skip hook stay on hardware time, and the line plays
 * what the consumer computed for those indices instead of silence. A drop
 * with fewer skipped periods than the refill leaves the rest as lead. The
 * next skipped periods pay it (engine_lead_pay); until then the tick clock
 * stands that many periods ahead of the capture.
 *
 * The stage holds the refill of one attempt, the rest depth less one period
 * of every channel. Without a render hook the refill is silence.
 */
static void engine_refill_render(struct engine *e, unsigned periods,
				 uint64_t now_us)
{
	size_t step = e->period_bytes / sizeof(*e->stage);

	if (!periods)
		return;
	for (unsigned i = 0; i < periods; i++) {
		int16_t *tx = e->stage + i * step;

		memset(e->rx, 0, e->period_bytes);
		memset(tx, 0, e->period_bytes);
		engine_clock_publish(e, now_us);
		if (e->opts.hooks.render)
			e->opts.hooks.render(e->opts.hooks.ctx, e->rx, tx,
					     e->geom.channels,
					     e->geom.frames_per_period,
					     e->live.ticks);
		e->live.ticks++;
	}
	e->staged = periods;
	e->staged_sent = 0;
	e->live.rephase_rendered += periods;
}

/*
 * The freshness of the next read of a drain, taken after the work of the drop.
 * The skip, control and render hooks run between the clear of the drop and
 * the next read, and a capture completion can come in that time. The next
 * read would then take that period without a wait, and the restart would
 * kick at the end of the work, late on the completion. So the drain measures
 * the backlog again here and drops what completed, as trims, and the next
 * read waits for a completion that comes after the work. A read that a fault
 * left unmarked stays so: the fault rule of engine_rephase_fault holds. A
 * fault here takes the same rule. Only a fault that stays ends the loop.
 */
static bool engine_drain_fresh(struct engine *e)
{
	unsigned period = e->geom.frames_per_period;
	long avail;
	unsigned n;
	int rc;

	if (e->rephase != ENGINE_REPHASE_DRAIN || !e->rephase_fresh)
		return true;
	e->rephase_fresh = false;
	avail = e->ops->capture_avail_fresh(e->tdm);
	if (avail < 0)
		return engine_fault(e, (int)avail, &e->live.avail_errors);
	n = (unsigned)(avail / (long)period);
	if (n) {
		rc = e->ops->capture_skip(e->tdm, n);
		if (rc < 0)
			return engine_fault(e, rc, &e->live.read_errors);
		engine_skip_note(e, n, &e->live.trim_drops);
	}
	e->rephase_fresh = true;

	return true;
}

/*
 * A read the rephase dropped: no tick hook and no write. The dropped period is
 * elapsed time, as a trimmed one is. The control hook then runs the work that
 * needs no period, so a command waits for no write across a hold and its
 * drain. The first drop of a drain renders the refill last. The freshness of
 * the next read comes after all of that work (engine_drain_fresh).
 */
static bool engine_drop_run(struct engine *e, uint64_t now_us)
{
	unsigned refill = engine_refill_due(e);

	engine_refill_charge(e, refill);
	engine_lead_pay(e);
	e->live.ticks += e->skipped;
	engine_clock_publish(e, now_us);
	engine_skip_hook(e);
	if (e->opts.hooks.control)
		e->opts.hooks.control(e->opts.hooks.ctx, e->live.ticks);
	engine_refill_render(e, refill, now_us);

	return engine_drain_fresh(e);
}

/* One period of the stage, the next one not written. */
static int engine_stage_write(struct engine *e)
{
	size_t step = e->period_bytes / sizeof(*e->stage);
	int rc = e->ops->write_period(e->tdm,
				      e->stage + e->staged_sent * step);

	if (rc < 0)
		return rc;
	if (rc > 0)
		e->live.playback_expired += (unsigned)rc;
	e->staged_sent++;

	return 0;
}

/*
 * The refill, written right after the read that a fresh capture completion
 * woke, before the hook. The first period restarts the ring at this
 * completion, as the linked start did, and the period of the tick follows
 * the rest. A transient fault retries the write that failed,
 * ENGINE_REFILL_TRIES attempts in all; a written period is never written
 * again. Past that, the attempt aborts, and the periods left go out ahead of
 * the write of the tick (engine_stage_flush). Any other fault recovers,
 * which discards them.
 */
static enum engine_rephase_read engine_refill_write(struct engine *e)
{
	unsigned tries = 0;

	while (e->staged_sent < e->staged) {
		int rc = engine_stage_write(e);

		if (!rc) {
			tries = 0;
			continue;
		}
		e->live.last_errno = -rc;
		if (rc != -EAGAIN && rc != -EINTR)
			return engine_rephase_fault(e, rc,
						    &e->live.write_errors);
		e->live.again++;
		if (++tries < ENGINE_REFILL_TRIES)
			continue;
		engine_rephase_abort(e);
		return ENGINE_REPHASE_RUN;
	}
	engine_stage_clear(e);

	return ENGINE_REPHASE_RUN;
}

/*
 * The refill that an aborted attempt left, written ahead of the period of the
 * tick as ordinary writes: the consumer's history holds those periods in
 * front of the tick's own. A fault takes the path of the tick's own write. A
 * transient one keeps the rest for the next pass, and a recovery discards it.
 */
static bool engine_stage_flush(struct engine *e)
{
	while (e->staged_sent < e->staged) {
		int rc = engine_stage_write(e);

		if (rc)
			return engine_fault(e, rc, &e->live.write_errors);
	}
	engine_stage_clear(e);

	return true;
}

/*
 * The rephase decision of one capture read, before the hook.
 *
 * Entry at the read. Inside an episode, on a capable back end, a fresh room
 * equal to the whole ring says the queue is empty and the DMA stopped. Entry
 * at the write is engine_rephase_write.
 *
 * The drain. The first read drops itself and the backlog; each later read is
 * dropped too while the ring still plays. The first drop renders the refill
 * (engine_refill_render). After the work of each drop, the drain clears what
 * completed during it (engine_drain_fresh), so the next read waits for a
 * capture completion that comes after the work. A read that such a fresh
 * completion woke, with the ring empty and no backlog, writes the refill and
 * runs the tick: the ring restarts on that completion, and the episode's
 * calibration window then measures the depth of a clean start.
 *
 * The bound. Each read of the drain after the first spends one unit of the
 * budget, whether its calls work, fault or drop it. The read that finds the
 * budget spent runs the hook and the write, and the attempt aborts. So does
 * a refill write that fails ENGINE_REFILL_TRIES times in a row. Either way
 * the refill goes out ahead of the write of the tick. After an abort the
 * catch-up runs alone. No attempt starts again before the next calibration
 * window completes, a recovery, or an episode that a late read or write
 * opens. So faults that stay cost at most one bounded drain per window.
 *
 * The settle check (engine_rephase_write) can ask for a held-back write,
 * which drops the next read the same way. Reason for both: at the linked
 * start the ring has completed one period before the first write of a tick.
 * A ring restarted by a write has not, so every write after it comes before
 * the completion it waits on, and the queue stands a period deeper for good:
 * delay_ref and loop_ref 640 against 480 at 16 kHz on AN7581 after holds. A
 * dropped read is a trim, so no computed frame is ever discarded.
 */
static enum engine_rephase_read engine_rephase_read(struct engine *e)
{
	long period = (long)e->geom.frames_per_period;
	long ring = engine_ring_frames(e);
	enum engine_rephase_read r;
	long room, avail;

	if (e->rephase_withhold) {
		e->rephase_withhold = false;
		e->rephase_withheld++;
		e->live.rephase_withheld++;
		engine_skip_note(e, 1, &e->live.trim_drops);
		return ENGINE_REPHASE_DROP;
	}
	if (e->rephase == ENGINE_REPHASE_SETTLE)
		return ENGINE_REPHASE_RUN;
	if (e->rephase == ENGINE_REPHASE_NONE) {
		if (!e->catching_up || e->rephase_barred ||
		    !engine_rephase_capable(e))
			return ENGINE_REPHASE_RUN;
		room = e->ops->playback_room_fresh(e->tdm);
		if (room < 0) {
			if (!engine_fault(e, (int)room, &e->live.avail_errors))
				return ENGINE_REPHASE_STOP;
			if (room == -EAGAIN || room == -EINTR)
				return ENGINE_REPHASE_RUN;
			return ENGINE_REPHASE_RESET;
		}
		if (room < ring)
			return ENGINE_REPHASE_RUN;
		engine_rephase_begin(e);
		return engine_rephase_drop(e);
	}

	if (!e->rephase_budget) {
		engine_rephase_abort(e);
		return ENGINE_REPHASE_RUN;
	}
	e->rephase_budget--;
	room = e->ops->playback_room_fresh(e->tdm);
	if (room < 0)
		return engine_rephase_fault(e, (int)room,
					    &e->live.avail_errors);
	avail = e->ops->capture_avail_fresh(e->tdm);
	if (avail < 0)
		return engine_rephase_fault(e, (int)avail,
					    &e->live.avail_errors);
	if (e->rephase_fresh && room >= ring && avail < period) {
		r = engine_refill_write(e);
		if (r != ENGINE_REPHASE_RUN ||
		    e->rephase != ENGINE_REPHASE_DRAIN)
			return r;
		e->rephase = ENGINE_REPHASE_SETTLE;
		e->rephase_writes = ENGINE_REPHASE_SETTLE_WRITES;
		e->live.rephases++;
		return ENGINE_REPHASE_RUN;
	}

	return engine_rephase_drop(e);
}

/*
 * The write side of the rephase, after each write with the queue it left.
 *
 * Entry at the write: on a capable back end, a write that leaves exactly one
 * period queued and expired nothing found the ring empty and restarted it at
 * the phase of this write. That covers a ring that runs dry inside the
 * catch-up, after the read had found it still playing, and a hold inside the
 * write itself. A write of the rephase never enters it again, and an abort
 * bars this entry as it bars the one at the read.
 *
 * The settle check reads ENGINE_REPHASE_SETTLE_WRITES writes after the
 * restart, and starts the count again when it holds a read back. A queue a
 * period or more above the rest depth on ENGINE_REPHASE_DEEP_SAMPLES writes in
 * a row says the writes still come before the completions they wait on. The
 * next read is then held back, at most ENGINE_REPHASE_WITHHOLD_MAX times. A
 * restart whose first completion comes late, for example on the next capture
 * completion, needs it; one that completes inside the tick never reads that
 * deep. A write that leaves one period during the check found the ring dry
 * again, and starts a new attempt.
 */
static void engine_rephase_write(struct engine *e, long queued, int expired)
{
	long period = (long)e->geom.frames_per_period;
	long rest;

	if (e->rephase == ENGINE_REPHASE_SETTLE && !expired &&
	    queued == period) {
		engine_rephase_begin(e);
		return;
	}
	if (e->rephase == ENGINE_REPHASE_SETTLE) {
		rest = (long)engine_rest_periods(e) * period;
		if (2 * queued >= 2 * rest + period)
			e->rephase_deep++;
		else
			e->rephase_deep = 0;
		if (e->rephase_deep >= ENGINE_REPHASE_DEEP_SAMPLES &&
		    e->rephase_withheld < ENGINE_REPHASE_WITHHOLD_MAX) {
			e->rephase_deep = 0;
			e->rephase_withhold = true;
			e->rephase_writes = ENGINE_REPHASE_SETTLE_WRITES;
			return;
		}
		if (e->rephase_writes && --e->rephase_writes)
			return;
		engine_rephase_clear(e);
		return;
	}
	if (e->rephase != ENGINE_REPHASE_NONE || e->rephase_barred ||
	    expired || !engine_rephase_capable(e) || queued != period)
		return;
	engine_rephase_begin(e);
}

static bool engine_capture(struct engine *e)
{
	uint64_t now_us;
	int rc = e->ops->read_period(e->tdm, e->rx);

	if (rc < 0)
		return engine_fault(e, rc, &e->live.read_errors);
	if (rc > 0)
		engine_skip_note(e, (unsigned)rc, &e->live.capture_drops);

	now_us = udsp_mono_us();
	engine_interval_note(e, now_us, e->ops->avail == NULL);
	if (!engine_backlog_note(e))
		return false;
	switch (engine_rephase_read(e)) {
	case ENGINE_REPHASE_STOP:
		return false;
	case ENGINE_REPHASE_RESET:
		return true;
	case ENGINE_REPHASE_DROP:
		return engine_drop_run(e, now_us);
	default:
		break;
	}
	/* The skipped periods are hardware time the engine was not there for,
	 * so the clock steps over them before it publishes; the index it
	 * publishes is still the tick the hook runs on. */
	engine_lead_pay(e);
	e->live.ticks += e->skipped;
	engine_clock_publish(e, now_us);
	engine_payload_make(e);
	engine_skip_hook(e);
	if (e->opts.hooks.tick)
		e->opts.hooks.tick(e->opts.hooks.ctx, e->rx, e->tx,
				   e->geom.channels, e->geom.frames_per_period,
				   e->live.ticks);
	e->tx_pending = true;

	return true;
}

uint32_t engine_now_ms(const struct engine *e)
{
	if (!e)
		return 0;

	return (uint32_t)(atomic_load_explicit(&e->clock_tick,
					       memory_order_acquire) * 10u);
}

void engine_clock_get(const struct engine *e, uint64_t *tick, uint64_t *mono_us)
{
	if (!e)
		return;
	if (tick)
		*tick = atomic_load_explicit(&e->clock_tick, memory_order_acquire);
	if (mono_us)
		*mono_us = atomic_load_explicit(&e->clock_mono_us,
						memory_order_relaxed);
}

static bool engine_tick(struct engine *e)
{
	/* A transient write fault keeps the captured period in tx: the retry
	 * writes the same period before the next read, so a period is never
	 * dropped on the floor. */
	if (!e->tx_pending && !engine_capture(e))
		return false;
	if (!e->tx_pending)
		return true;
	/* The refill of an aborted rephase goes first, and the catch-up then
	 * measures the queue it left. A transient fault keeps the rest for
	 * the next pass; a recovery discarded it with the period of the
	 * tick. */
	if (e->staged_sent < e->staged && !engine_stage_flush(e))
		return false;
	if (e->staged || !e->tx_pending)
		return true;
	switch (engine_catchup_run(e)) {
	case ENGINE_CATCHUP_STOP:
		return false;
	case ENGINE_CATCHUP_RETRY:
		return true;
	default:
		break;
	}
	/* A fault inside the catch-up that recovered rebuilt both rings and
	 * re-prefilled playback, so the period held in tx belongs to a ring
	 * that is gone and the tick ends here. */
	if (!e->tx_pending)
		return true;

	int rc = e->ops->write_period(e->tdm, e->tx);

	if (rc < 0)
		return engine_fault(e, rc, &e->live.write_errors);
	if (rc > 0)
		e->live.playback_expired += (unsigned)rc;

	e->tx_pending = false;
	e->live.ticks++;

	long queued = e->ops->delay(e->tdm);

	if (queued >= 0) {
		/* Sampled beside delay() so the pair shares one instant: the
		 * echo loop is their sum, which holds still while each side
		 * swings with the tick phase (engine.h loop_ref). An avail
		 * fault here only degrades this sample to the playback half;
		 * the strike accounting stays with engine_backlog_note. */
		long backlog = e->ops->avail ? e->ops->avail(e->tdm) : 0;

		engine_delay_note(e, queued,
				  queued + (backlog > 0 ? backlog : 0));
		engine_rephase_write(e, queued, rc);
		e->fail_run = 0;
		engine_publish(e);
		return true;
	}

	/* A delay fault is a fault like any other: it feeds the strike run, and
	 * the run resets only on a tick whose every operation worked. */
	engine_publish(e);

	return engine_fault(e, (int)queued, &e->live.delay_errors);
}

static bool engine_ticks_left(const struct engine *e)
{
	return !e->opts.max_ticks || e->live.ticks < e->opts.max_ticks;
}

static void engine_sem_wait(sem_t *s)
{
	while (sem_wait(s) != 0) {
		if (errno != EINTR)
			return;
	}
}

static void *engine_loop(void *arg)
{
	struct engine *e = arg;

	/* Before rt_apply, so the crash handler can serve a fault on this
	 * thread's stack from the very first instruction that can fault: the
	 * tick thread is the one that overflows on a deep signal chain. Best
	 * effort, exactly like the rest of rt setup. */
	if (udsp_thread_sigstack_install(&e->sig_stack, &e->sig_prev) < 0)
		e->sig_stack = NULL;

	rt_apply(e->cpu, e->prio, &e->live.rt);
	e->live.rt_done_us = udsp_mono_us();
	engine_publish(e);
	sem_post(&e->ready);

	engine_sem_wait(&e->gate);

	e->live.running = true;
	while (!atomic_load(&e->stop) && engine_ticks_left(e)) {
		if (!engine_tick(e))
			break;
	}

	e->live.running = false;
	atomic_store(&e->done, true);
	engine_publish(e);
	udsp_thread_sigstack_release(&e->sig_stack, &e->sig_prev);

	return NULL;
}

/* The tool and uvoiced handle their own signals; a signal delivered to the
 * tick thread would only break the blocking read. */
static void engine_sigmask_block(sigset_t *prev)
{
	sigset_t block;

	sigemptyset(&block);
	sigaddset(&block, SIGINT);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGHUP);
	sigaddset(&block, SIGQUIT);
	sigaddset(&block, SIGUSR1);
	sigaddset(&block, SIGUSR2);
	sigaddset(&block, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &block, prev);
}

static bool engine_thread_start(struct engine *e)
{
	pthread_attr_t attr;
	sigset_t prev;

	if (pthread_attr_init(&attr))
		return false;

	/* A fixed stack the prefault covers, on both glibc and musl. */
	pthread_attr_setstacksize(&attr, ENGINE_STACK_BYTES);
	engine_sigmask_block(&prev);

	int rc = pthread_create(&e->thread, &attr, engine_loop, e);

	pthread_sigmask(SIG_SETMASK, &prev, NULL);
	pthread_attr_destroy(&attr);
	if (rc)
		return false;

	e->thread_live = true;
	engine_sem_wait(&e->ready);

	return true;
}

static void engine_gate_release(struct engine *e)
{
	if (!e->thread_live || e->gate_released)
		return;

	e->gate_released = true;
	sem_post(&e->gate);
}

static unsigned engine_prefill_want(const struct udsp_config *cfg)
{
	unsigned want = cfg->prefill_periods ? cfg->prefill_periods : 3;

	return want > ENGINE_PREFILL_MAX ? ENGINE_PREFILL_MAX : want;
}

/* A zero means 8000 on both sides of the geometry (tdm.h), so both directions
 * are normalised before they are compared. */
static unsigned engine_rate_of(unsigned rate)
{
	return rate ? rate : ENGINE_RATE_HZ;
}

static unsigned engine_frames_for_rate(unsigned rate)
{
	return rate / 100u;
}

void engine_tdm_fail_text(const struct udsp_tdm_fail *f, char *buf, size_t len)
{
	static const char *const dir[] = { "playback ", "capture " };
	const char *side = "";

	if (!len)
		return;
	buf[0] = '\0';
	if (!f->err)
		return;
	if (f->stream == UDSP_TDM_DIR_PLAYBACK ||
	    f->stream == UDSP_TDM_DIR_CAPTURE)
		side = dir[f->stream];
	snprintf(buf, len, "%s%s: %s", side, f->what ? f->what : "open",
		 strerror(-f->err));
}

/* The geometry verdict of engine.h: every answer the back end gives about the
 * rate, the period and the ring is ENGINE_OPEN_GEOM, and the one policy check
 * that is not the back end's answer is ENGINE_OPEN_OTHER. A NULL open is the
 * back end's answer only when its record says so. */
static enum engine_open_fail engine_tdm_open(struct engine *e,
					     const struct udsp_config *cfg,
					     struct udsp_tdm_fail *fail)
{
	unsigned want_prefill = engine_prefill_want(cfg);
	unsigned rate = engine_rate_of(cfg->sample_rate);
	unsigned frames = engine_frames_for_rate(rate);
	struct udsp_tdm_geom want = {
		.frames_per_period = frames,
		.channels = cfg->channels ? cfg->channels : 1,
		.prefill_periods = want_prefill + 1,
		.layout = UDSP_TDM_INTERLEAVED,
		.rate = rate,
	};
	struct udsp_tdm_geom got = { 0 };

	if (!frames || frames > ENGINE_FRAMES_MAX)
		return ENGINE_OPEN_GEOM;
	e->tdm = e->ops->open(cfg->tdm_playback, cfg->tdm_capture, &want, &got,
			      fail);
	if (!e->tdm)
		return fail->geometry ? ENGINE_OPEN_GEOM : ENGINE_OPEN_OTHER;
	if (!got.frames_per_period || !got.channels || got.prefill_periods < 2)
		return ENGINE_OPEN_GEOM;
	/* The line rate and the tick length are fixed at open: every consumer
	 * downstream sizes its frame from them, so a back end that granted
	 * another rate or another period is refused here rather than run
	 * against a geometry nothing else knows about. */
	if (engine_rate_of(got.rate) != rate || got.frames_per_period != frames)
		return ENGINE_OPEN_GEOM;
	/* An unpaced backend ticks at CPU speed, so an unbounded run appends
	 * playback periods until the disk fills. */
	if (!engine_paced(e, &got) && !e->opts.max_ticks)
		return ENGINE_OPEN_OTHER;
	/* The UDSP_MAX_TICKS bound binds unpaced backends only. The caller says
	 * whether its bound came from there, so a stale variable in the
	 * environment cannot cut a live ALSA run short, and udsp-cli's own
	 * --seconds bound still holds. */
	if (e->opts.max_ticks_unpaced_only && engine_paced(e, &got))
		e->opts.max_ticks = 0;

	e->geom = got;
	/* The granted rate with the zero normalised, which the refusal above
	 * has already proven equal to the request; the tick length and the
	 * published statistics both read it from here. */
	e->geom.rate = engine_rate_of(got.rate);
	e->prefill_want = want_prefill;
	e->prefill = got.prefill_periods - 1 < want_prefill ?
		     got.prefill_periods - 1 : want_prefill;
	e->period_bytes = (size_t)got.channels * got.frames_per_period *
			  sizeof(int16_t);
	e->period_us = (uint32_t)((uint64_t)got.frames_per_period * 1000000u /
				  e->geom.rate);

	return ENGINE_OPEN_OK;
}

/* The stage of the refill exists where a rephase can run: the rest depth
 * is two periods below the ring at most (engine_rephase_capable). */
static bool engine_buffers_alloc(struct engine *e)
{
	e->rx = calloc(1, e->period_bytes);
	e->tx = calloc(1, e->period_bytes);
	if (e->geom.tx_restarts && e->geom.prefill_periods > 2u)
		e->stage_cap = e->geom.prefill_periods - 2u;
	if (e->stage_cap) {
		e->stage = calloc(e->stage_cap, e->period_bytes);
		if (e->stage)
			rt_range_prefault(e->stage,
					  e->stage_cap * e->period_bytes);
	}

	return e->rx && e->tx && (!e->stage_cap || e->stage);
}

static void engine_stats_init(struct engine *e)
{
	e->live.prefill_requested = e->prefill_want;
	e->live.prefill_periods = e->prefill;
	e->live.granted_periods = e->geom.prefill_periods;
	e->live.frames_per_period = e->geom.frames_per_period;
	e->live.rate = e->geom.rate;
	e->live.period_us = e->period_us;
	e->live.channels = e->geom.channels;
	e->live.paced = engine_paced(e, &e->geom);
	e->live.rt.cpu = -1;
	e->live.rt.prio = e->prio;
	engine_publish(e);
}

static bool engine_sem_init(struct engine *e)
{
	if (sem_init(&e->ready, 0, 0))
		return false;
	if (sem_init(&e->gate, 0, 0)) {
		sem_destroy(&e->ready);
		return false;
	}

	e->sem_live = true;

	return true;
}

static void engine_verdict_init(struct engine_open_verdict *why)
{
	if (!why)
		return;
	memset(why, 0, sizeof(*why));
	why->cause = ENGINE_OPEN_OTHER;
}

struct engine *engine_open_ops(const struct udsp_tdm_ops *ops,
			       const struct udsp_config *cfg,
			       const struct engine_opts *opts,
			       struct engine_open_verdict *why)
{
	enum engine_open_fail cause = ENGINE_OPEN_OTHER;
	struct udsp_tdm_fail tdm_fail = { 0 };

	engine_verdict_init(why);
	if (!ops || !cfg || !opts)
		return NULL;

	struct engine *e = calloc(1, sizeof(*e));

	if (!e)
		return NULL;

	e->ops = ops;
	e->opts = *opts;
	e->cpu = cfg->engine_cpu;
	e->prio = cfg->engine_prio;
	atomic_init(&e->stop, false);
	atomic_init(&e->done, false);
	atomic_init(&e->clock_tick, 0u);
	atomic_init(&e->clock_mono_us, 0u);

	if (!engine_sem_init(e))
		goto fail;
	cause = engine_tdm_open(e, cfg, &tdm_fail);
	if (cause != ENGINE_OPEN_OK)
		goto fail;
	cause = ENGINE_OPEN_OTHER;
	if (!engine_buffers_alloc(e))
		goto fail;

	/* Stamped before engine_stats_init publishes the first snapshot, so
	 * the very first engine_stats_get already carries the identity. */
	e->live.serial = atomic_fetch_add_explicit(&engine_serial_next, 1u,
						   memory_order_relaxed);
	engine_stats_init(e);

	/* The worker owns the counter block until it posts ready; the control
	 * thread owns it from there until the gate opens. */
	if (!engine_thread_start(e))
		goto fail;

	/*
	 * A back end whose clock free-runs without a running stream defers the
	 * DMA start and the tick to engine_start_streaming(), so the SLIC init
	 * runs with the clock up and no capture stream that could xrun, recover
	 * and reset the PCM block mid-init (an7581-pcm-tdm.c resets the block on
	 * every capture restart, which stops the SLIC's only clock).
	 */
	e->deferred_start = opts->defer_start;
	if (!e->deferred_start) {
		if (!engine_prefill_write(e) || e->ops->start(e->tdm) < 0)
			goto fail;

		/*
		 * The gate hold is a virtual-clock guard: an unpaced backend ticks at
		 * CPU speed and would run out of ticks inside udsp_open. A paced
		 * backend drives a real ring, and parking the loop across the
		 * seconds-long SLIC attach overruns capture in about 60 ms.
		 * The raw back end does not stop the pair on that overrun
		 * (tdm.h), so no XRUN gates the MediaTek AFE clock for the
		 * whole attach on it; the alsa-lib back end stops at one ring
		 * and gates the clock that way (measured on MT7987). The
		 * hold stays ignored on every paced backend, because a parked
		 * loop leaves the attach with a ring nobody drains either way.
		 */
		if (!opts->hold_gate || e->live.paced)
			engine_gate_release(e);
	}

	if (why)
		why->cause = ENGINE_OPEN_OK;

	return e;
fail:
	if (why) {
		why->cause = cause;
		why->tdm = tdm_fail;
	}
	engine_close(e);

	return NULL;
}

struct engine *engine_open(const struct udsp_config *cfg,
			   const struct engine_opts *opts,
			   struct engine_open_verdict *why)
{
	engine_verdict_init(why);
	if (!cfg)
		return NULL;

	return engine_open_ops(engine_backend_find(cfg->tdm_backend), cfg, opts,
			       why);
}

int engine_start_streaming(struct engine *e)
{
	if (!e || !e->deferred_start)
		return 0;

	/* A close that won the engine_lock already asked the tick to leave, so
	 * do not start a stream the teardown is about to drop. */
	if (atomic_load_explicit(&e->stop, memory_order_acquire))
		return 0;

	/* DMA first, then the tick: the tick reads capture as soon as the gate
	 * opens, so the ring must already run. */
	if (!engine_prefill_write(e) || e->ops->start(e->tdm) < 0)
		return -EIO;

	e->deferred_start = false;
	engine_gate_release(e);

	return 0;
}

bool engine_deferred(const struct engine *e)
{
	return e && e->deferred_start;
}

void engine_gate_open(struct engine *e)
{
	/* A deferred engine has no running stream yet, so opening its gate would
	 * park the tick on an empty ring; engine_start_streaming() opens it. */
	if (e && !e->deferred_start)
		engine_gate_release(e);
}

void engine_stop_request(struct engine *e)
{
	if (!e)
		return;

	atomic_store(&e->stop, true);
	engine_gate_release(e);
}

void engine_stop(struct engine *e, struct engine_stats *out)
{
	if (!e)
		return;

	atomic_store(&e->stop, true);
	if (e->thread_live) {
		engine_gate_release(e);
		pthread_join(e->thread, NULL);
		e->thread_live = false;
		if (e->tdm)
			e->ops->stop(e->tdm);
		e->live.running = false;
		engine_publish_final(e);
	}

	/* The thread is joined, so its private block is the final word. */
	if (out)
		*out = e->live;
}

void engine_close(struct engine *e)
{
	if (!e)
		return;

	engine_stop(e, NULL);
	if (e->tdm)
		e->ops->close(e->tdm);
	if (e->sem_live) {
		sem_destroy(&e->ready);
		sem_destroy(&e->gate);
	}
	free(e->rx);
	free(e->tx);
	free(e->stage);
	free(e);
}
