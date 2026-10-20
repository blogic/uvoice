// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Private core header of the engine tick loop. This header is private;
 * include/udsp/udsp.h is the public contract. Channels, streams, command
 * queues, the event emitter and the net thread sit on top of this object; the
 * engine itself carries only the tick loop.
 *
 * The tick loop owns one pthread. After engine_open returns it never logs,
 * never allocates, never takes a mutex and uses no floating point: it updates
 * a private struct engine_stats and publishes a copy into a double buffer with
 * an atomic index (a seqlock over plain fields is a C11 data race).
 * engine_stats_get returns one whole consistent copy, never a mix of two
 * ticks.
 *
 * Observables. The real-time gate needs the tick interval distribution, the
 * playback delay deviation at prefill 3 and 5, the stall and xrun counts and
 * the real-time setup result. Every one of them is a counter or a fixed field
 * here; the tool prints them, the engine never does.
 */
#ifndef UDSP_CORE_ENGINE_H
#define UDSP_CORE_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "udsp/udsp.h"
#include "udsp/tdm.h"

/* The widest period the tick can carry: 10 ms at 16 kHz (tdm.h Geometry).
 * The period itself follows the line rate, rate/100. */
#define ENGINE_FRAMES_MAX		160u
#define ENGINE_PREFILL_MAX		16u
#define ENGINE_HIST_BUCKETS		8u
/* Delay samples taken before the playback reference is latched. */
#define ENGINE_DELAY_CAL		16u

/* Upper edge in microseconds of tick-interval bucket i; the last bucket holds
 * everything above the last edge. 10 ms +- 1 ms is bucket 1, so a healthy run
 * puts every sample there. */
extern const uint32_t engine_hist_edge_us[ENGINE_HIST_BUCKETS - 1];

/*
 * What the real-time setup actually achieved. Nothing here is fatal: with
 * RT_GROUP_SCHED and no cpu.rt_runtime_us grant the SCHED_FIFO request is
 * refused with EPERM, and the counters must observe exactly that instead of a
 * failed start, so the engine keeps running SCHED_OTHER and reports the
 * errno.
 */
struct rt_report {
	bool sched_applied;
	bool mlock_applied;
	bool affinity_applied;
	int sched_errno;
	int mlock_errno;
	int affinity_errno;
	int policy;		/* sched_getscheduler() after the attempt */
	int prio;		/* requested priority, 0 = no request */
	int cpu;		/* pinned cpu, -1 when not pinned */
};

struct engine_stats {
	/* Monotonic across engine instances in the process, never 0 for a
	 * live engine. cal_restarts counts from zero per instance, so a
	 * replacement engine needs an identity the consumers can compare by
	 * value: a recycled allocation can reuse the old engine's address, so
	 * a pointer compare cannot tell the two apart. */
	uint64_t serial;
	uint64_t ticks;
	uint64_t interval[ENGINE_HIST_BUCKETS];
	uint64_t interval_samples;
	uint64_t delay_samples;
	/* One per hold episode on a back end with the fresh reads, a tick
	 * interval above two periods on one without them. */
	uint64_t capture_stalls;
	/* Playback delay above the calibrated reference by more than one
	 * period: a swing of exactly one period is the MT7987 snd_pcm_delay
	 * quantization step, not loss. Counted outside a hold episode only,
	 * because the catch-up moves the queue on purpose. On a back end that
	 * carries avail and the three fresh reads, the same distance below the
	 * reference is not counted here at all: only the hardware consuming
	 * more than the engine wrote produces it, which is a late engine, so
	 * it opens a hold episode and counts one capture stall like any other.
	 * On a back end short of any of them, that distance counts here too. */
	uint64_t delay_stalls;
	uint64_t xruns;
	/* The catch-up record of a late engine. A hold episode is an interval
	 * in which the engine found two or more capture periods waiting; the
	 * three drop counters are in periods. */
	uint64_t holds;
	uint64_t capture_drops;		/* the ring overwrote them unread */
	uint64_t trim_drops;		/* the catch-up rule skipped them */
	uint64_t playback_expired;	/* the hardware pointer ran past them */
	/* The restart of a playback ring that ran dry, on a back end whose
	 * DMA stops when empty (tdm.h tx_restarts; engine.c
	 * engine_rephase_read). Its dropped capture periods count in
	 * trim_drops. rephases counts an attempt once its refill is written,
	 * before the settle check ends. A recovery clears an attempt without
	 * an abort, so starts need not equal rephases plus aborts.
	 * rephase_rendered counts the periods the render hook computed for a
	 * refill, and rephase_lead the ones of them that no skipped period
	 * paid for when they ran (engine.c engine_refill_render). */
	uint64_t rephase_starts;	/* a dry ring was found */
	uint64_t rephases;		/* restarted on a capture completion */
	uint64_t rephase_aborts;	/* gave up; the catch-up ran alone */
	uint64_t rephase_withheld;	/* reads dropped, the queue too deep */
	uint64_t rephase_rendered;	/* refill periods rendered ahead */
	uint64_t rephase_lead;		/* of them, ahead of the capture */
	uint64_t recover_failures;
	uint64_t again;			/* EAGAIN or EINTR, retried */
	uint64_t read_errors;
	uint64_t write_errors;
	uint64_t delay_errors;
	uint64_t avail_errors;
	uint32_t interval_min_us;
	uint32_t interval_max_us;
	int32_t delay_dev_min;		/* playback delay minus prefill, frames */
	int32_t delay_dev_max;
	int32_t delay_dev_last;
	/* The driver's own steady state, latched as the median of the first
	 * ENGINE_DELAY_CAL samples: the mt7986 AFE settles about two periods
	 * below the nominal prefill, so a stall is a drift beyond one period
	 * from this reference, not from prefill * frames; the period-sized
	 * allowance absorbs the MT7987 snd_pcm_delay quantization step. */
	int32_t delay_ref;		/* absolute queued frames */
	/* The playback queue plus the capture backlog, latched by the same
	 * calibration window. The two queues share one codec clock, so their
	 * sum is the ring the echo crosses and it holds still while each side
	 * swings with the tick phase (measured: play 268/cap 80 at rest,
	 * play 344/cap 4 across one late tick, the sum constant). delay_ref
	 * alone under-reads the echo loop by the capture side, which puts the
	 * canceller reference 160 samples early on MT7987. Backends
	 * without avail() latch the playback queue alone. */
	int32_t loop_ref;
	int32_t delay_drift_min;	/* playback delay minus delay_ref */
	int32_t delay_drift_max;
	int32_t delay_drift_last;
	uint64_t delay_drift_samples;
	/* How many times the delay calibration window restarted, counted from
	 * engine start and never reset. chan.c falls back to the nominal
	 * prefill reference after three of them, and a count of edges cannot
	 * see a window that never completed once. */
	uint64_t cal_restarts;
	/* CLOCK_MONOTONIC microseconds at which the worker finished rt_apply.
	 * The streams start after it, never before, so a test and the bring-up
	 * log can both check the order. */
	uint64_t rt_done_us;
	unsigned prefill_requested;	/* what the config asked for */
	unsigned prefill_periods;	/* what the engine prefills */
	unsigned granted_periods;	/* got.prefill_periods, the ring depth */
	unsigned frames_per_period;
	/* The granted line rate in Hz and the tick length that follows from it
	 * and the period. The tick stays 10 ms at every rate, so period_us
	 * reads 10000 on a 16 kHz line too. */
	unsigned rate;
	unsigned period_us;
	unsigned channels;
	bool paced;			/* the backend clocks the tick itself */
	int last_errno;
	bool running;
	bool stopped_fatal;
	bool delay_calibrated;
	struct rt_report rt;
};

/*
 * Per-object injection point for the per-tick work (no global state). tick()
 * runs on the tick thread between the capture read and the playback write: rx
 * is the captured planar buffer, tx the outgoing planar buffer the engine has
 * already filled per its loopback or silence rule, and the hook overwrites tx.
 * A zeroed engine_opts leaves the loop as the bare tick loop.
 */
/*
 * Why an open failed. Only a geometry refusal, the back end declining the rate
 * or the period it was asked for, may move the line rate: udsp_open degrades a
 * 16 kHz ask on that answer alone. Every other cause means the same thing at
 * 8000 as at 16000, so retrying there would hide it. The geometry verdict
 * covers the back end's own answer (tdm.h udsp_tdm_fail.geometry) and the
 * grant it returned; a back end that refuses without that answer, memory,
 * threads, the prefill and the start are other.
 */
enum engine_open_fail {
	ENGINE_OPEN_OK = 0,
	ENGINE_OPEN_GEOM,
	ENGINE_OPEN_OTHER,
};

/* The cause and, when the tdm open refused, its record; tdm.err stays 0 for
 * every other cause. */
struct engine_open_verdict {
	enum engine_open_fail cause;
	struct udsp_tdm_fail tdm;
};

/* The tdm record as one clause for a log line, "<direction> <what>:
 * <strerror>", or an empty string when the open left no record. Control
 * thread only. */
void engine_tdm_fail_text(const struct udsp_tdm_fail *f, char *buf,
			  size_t len);

struct engine_hooks {
	void *ctx;
	void (*tick)(void *ctx, int16_t *rx, int16_t *tx, unsigned channels,
		     unsigned frames, uint64_t tick);
	/* Called on the tick thread before the tick hook when the engine
	 * skipped capture periods since the last tick: the ring overwrote
	 * them, or the catch-up rule dropped them. periods is that count and
	 * tick the index the tick clock stands at after it advanced over
	 * them. Nullable. A consumer treats the skipped periods as elapsed
	 * time (the RTP timestamp, the tone and event generators). */
	void (*skip)(void *ctx, unsigned periods, uint64_t tick);
	/* Called on the tick thread to compute one playback period that no
	 * capture period goes with: the refill a restart writes ahead of the
	 * next tick (engine.c engine_refill_render). rx reads zero and is no
	 * capture; tx is a staging slot the engine filled with silence, and
	 * the hook overwrites it. tick is the index of the period, one more
	 * per call. The engine writes the period later, in call order and
	 * before the period of the next tick. Nullable: the refill is then
	 * silence. */
	void (*render)(void *ctx, int16_t *rx, int16_t *tx, unsigned channels,
		       unsigned frames, uint64_t tick);
	/* Called on the tick thread on each read the engine drops, after the
	 * skip hook and before any render call, so the consumer runs the
	 * work that needs no period: the commands and their acks. tick is
	 * the index the skip hook got. Nullable. */
	void (*control)(void *ctx, uint64_t tick);
};

struct engine_opts {
	bool loopback;		/* copy capture into playback, else silence */
	/* Park the worker on its gate until engine_gate_open(). udsp_open
	 * releases it as its last step, so an unpaced engine cannot run out of
	 * ticks in the middle of an open. */
	bool hold_gate;
	/* Open and prepare the back end but do not start the DMA or release the
	 * tick; hold both until engine_start_streaming(). Only a back end whose
	 * clock free-runs without a running stream may set this: the AN7581
	 * alsa-raw path masters PCLK/FSYNC at CFG_VALID with the DMA off, so the
	 * SLIC init runs with the clock up and no capture stream that could
	 * xrun, recover and reset the PCM block mid-init. */
	bool defer_start;
	/* Stop after this many ticks. 0 runs until engine_stop, which only a
	 * back end with a real clock may do: the file back end ticks at CPU
	 * speed, so engine_open refuses it without a cap. */
	uint64_t max_ticks;
	/* max_ticks came from the UDSP_MAX_TICKS environment variable, which
	 * binds unpaced backends only: the engine drops the bound when the
	 * backend reports paced. A caller with its own bound (the udsp-cli
	 * --seconds run) leaves this false. */
	bool max_ticks_unpaced_only;
	struct engine_hooks hooks;
};

struct engine;

/* Opens the tdm back end named by cfg->tdm_backend with the geometry
 * cfg->sample_rate asks for (rate/100 frames, 0 = 8000), starts the tick
 * thread, prefills playback with silence and starts both directions, in the
 * order the Startup order note of engine.c fixes. NULL on any failure, which
 * includes a back end that granted another rate or another period than the
 * one requested. */
/* why is nullable and takes the verdict on failure, ENGINE_OPEN_OK on
 * success. */
struct engine *engine_open(const struct udsp_config *cfg,
			   const struct engine_opts *opts,
			   struct engine_open_verdict *why);
/* The same, with the back end passed in instead of looked up by name: the
 * injection point for a test back end and for the plug-in back ends. An
 * unpaced back end (geom.paced false) is refused without opts->max_ticks. */
struct engine *engine_open_ops(const struct udsp_tdm_ops *ops,
			       const struct udsp_config *cfg,
			       const struct engine_opts *opts,
			       struct engine_open_verdict *why);
/* Releases a gate held by engine_opts.hold_gate. Idempotent; control side. */
void engine_gate_open(struct engine *e);
/* Starts the DMA and releases the tick for an engine opened with
 * engine_opts.defer_start; a no-op on one that already streams. The slic thread
 * calls it once the SLIC init is done. 0 on success, a negative errno on a
 * start failure. Control side. */
int engine_start_streaming(struct engine *e);
/* True while a deferred engine has not started its stream yet, so its tick is
 * still parked on the gate. False for a running or a non-deferred engine. The
 * slic thread reads it after engine_start_streaming to learn whether the start
 * happened. */
bool engine_deferred(const struct engine *e);
/* Asks the loop to leave without waiting for it. The bounded stopper polls
 * engine_done() to its deadline and joins only a thread that finished, so a
 * wedged backend read never blocks the caller. */
void engine_stop_request(struct engine *e);
/* Stops the loop, joins the thread, stops the tdm and reports the final
 * counters. Idempotent; control thread only. out may be NULL. */
void engine_stop(struct engine *e, struct engine_stats *out);
/* Stops if needed, then releases everything. Control thread only. */
void engine_close(struct engine *e);
/* One consistent copy of the published counters; safe while the loop runs. */
void engine_stats_get(struct engine *e, struct engine_stats *out);
/* True once the loop left on its own (max_ticks reached or a fatal fault). */
bool engine_done(struct engine *e);

/*
 * What a stall is. A back end with all three fresh reads (capture_avail_fresh,
 * playback_room_fresh, capture_skip) runs the hold episodes and counts one
 * capture_stalls per episode, not one per late tick: a burst would otherwise
 * inflate the figure by its own length. The backlog the episode opens on is
 * tdm_ops.avail right after the read, which costs no call of its own. A back
 * end with fewer of them keeps the per-tick rule: with tdm_ops.avail a backlog
 * above one period after each read is the stall, with neither the engine
 * infers it from its own tick interval, so every backend reports the same
 * counter. The playback half is direct in all three cases: delay() drift
 * beyond one period from the calibrated reference, counted outside a hold
 * episode only.
 */
/* The value that feeds udsp_counters.tdm_stalls. */
static inline uint64_t engine_stats_stalls(const struct engine_stats *st)
{
	return st->capture_stalls + st->delay_stalls + st->xruns;
}

/* core/rt.c: called once by the tick thread before its first tick. Every step
 * is best effort and reported; prio <= 0 requests no real-time setup at all. */
void rt_apply(int cpu, int prio, struct rt_report *rep);

/* core/rt.c prefault helpers, so a real-time thread makes its working set
 * resident before MCL_ONFAULT would otherwise fault it on first touch.
 * rt_stack_prefault touches the calling thread's stack. */
void rt_stack_prefault(void);
void rt_range_prefault(volatile void *base, size_t len);

#endif
