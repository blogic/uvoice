// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Unit tests for the minimal engine (core/engine.c, core/rt.c).
 *
 * Two backends drive the cases. The file backend (tdm/file.c) provides the
 * byte check: one successful tick appends exactly one period. The test
 * therefore checks the period content and the file length against the bytes
 * on disk. The file backend cannot check the call order, because its output
 * looks the same whether the prefill ran before or after start(). A
 * re-prefill that carried stale capture would still make a well-formed file.
 *
 * The instrumented backend below journals every call. The test therefore
 * asserts the two required sequences directly, silence and all: (open,
 * prefill, start) and (recover, prefill, start). The backend also injects
 * faults on demand.
 *
 * The real-time setup is not meaningfully unit-testable on a host. SCHED_FIFO
 * needs a privilege that the CI user does not have, and only the target
 * answers what RT_GROUP_SCHED does. The cases here check that rt.c reports its
 * result coherently either way, and that it finishes before the streams start.
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../core/clock.h"
#include "../core/engine.h"
#include "../tdm/tdm_common.h"
#include "tdm_raw_fake.h"
#include "util.h"

#define T_FRAMES	80u
#define T_CHANS		2u
#define T_NSAMP		(T_FRAMES * T_CHANS)
#define T_NBYTE		(T_NSAMP * sizeof(int16_t))
#define T_PREFILL	3u
#define T_MAXBYTE	(64u * 1024u)
#define T_JOURNAL_MAX	1024u

static unsigned char t_out[T_MAXBYTE];
static unsigned char t_cap[T_MAXBYTE];

static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void msleep(long ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000 };

	nanosleep(&ts, NULL);
}

/* --- instrumented tdm back end ---------------------------------------- */

struct t_script {
	unsigned read_fail_from;	/* 1-based read index */
	unsigned read_fail_count;
	int read_fail_err;
	unsigned write_fail_at;		/* 1-based write index, fails once */
	int write_fail_err;
	unsigned delay_fail_from;	/* 1-based delay index */
	unsigned delay_fail_count;
	int delay_fail_err;
	int start_err;			/* start() fails with this errno */
	bool paced;			/* what open() reports in got.paced */
	long avail_value;		/* frames the backend says are queued */
	long avail_err;			/* avail() fails with this errno once */
	unsigned grant_periods;		/* granted ring depth, 0 = echo want */
	unsigned grant_frames;		/* granted period, 0 = echo want */
	unsigned grant_rate;		/* granted rate, 0 = echo want */
	bool grant_rate_zero;		/* grant the literal zero of tdm.h */
	bool open_null;			/* the device will not open at all */
	/* open_null refuses as the device's geometry answer (a declined
	 * rate) instead of as a node another process holds (tdm.h Refusal) */
	bool open_fail_geometry;
	long delay_bias;		/* constant offset the driver holds */
	unsigned delay_step_from;	/* 1-based delay index of an excursion */
	unsigned delay_step_count;
	long delay_step;
	/* The ring model of t_ops_hold below. hold_at is the 1-based read
	 * index the engine returns at, hold_periods the hardware periods it
	 * was away for. cap_periods is the capture ring depth (the playback
	 * one is the granted prefill depth). expire_at is the 1-based write
	 * index that reports expired playback periods. */
	unsigned hold_at[2];
	unsigned hold_periods[2];
	unsigned cap_periods;
	unsigned expire_at;
	unsigned expire_periods;
	/* The holder that arrives while the engine is inside a write: the
	 * hardware drains this many periods during that one write, instead of
	 * the one period a wait for room costs. */
	unsigned write_hold_at;		/* 1-based write index */
	unsigned write_hold_periods;
	unsigned skip_err_n;		/* capture_skip calls that fail */
	int skip_err;			/* the errno they fail with */
};

static struct t_script t_script;
static char t_journal[T_JOURNAL_MAX];
static unsigned t_journal_len;
static unsigned t_reads, t_writes, t_delays;
/* The ring state of t_ops_hold: capture frames waiting and playback frames
 * the ring still holds. */
static long t_backlog;
static long t_queued;
static unsigned t_skips;
static long t_skip_frames;
static uint64_t t_start_us;
static int16_t t_write_first[T_JOURNAL_MAX];
static struct udsp_tdm_geom t_geom;
static int t_token;

static void t_reset(void)
{
	memset(&t_script, 0, sizeof(t_script));
	memset(t_journal, 0, sizeof(t_journal));
	memset(t_write_first, 0, sizeof(t_write_first));
	t_journal_len = 0;
	t_reads = t_writes = t_delays = 0;
	t_start_us = 0;
	t_backlog = 0;
	t_queued = 0;
	t_skips = 0;
	t_skip_frames = 0;
}

static void t_note(char c)
{
	if (t_journal_len + 1 < T_JOURNAL_MAX)
		t_journal[t_journal_len++] = c;
}

static unsigned t_samples(void)
{
	return t_geom.channels * t_geom.frames_per_period;
}

static bool t_silent(const int16_t *buf)
{
	for (unsigned i = 0; i < t_samples(); i++) {
		if (buf[i])
			return false;
	}

	return true;
}

static struct udsp_tdm *t_open(const char *play, const char *cap,
			       const struct udsp_tdm_geom *want,
			       struct udsp_tdm_geom *got,
			       struct udsp_tdm_fail *fail)
{
	t_note('o');
	if (t_script.open_null) {
		if (t_script.open_fail_geometry)
			tdm_fail_set(fail, UDSP_TDM_DIR_PLAYBACK, true, -EINVAL,
				     "rate");
		else
			tdm_fail_set(fail, UDSP_TDM_DIR_PLAYBACK, false, -EBUSY,
				     "open");
		return NULL;
	}
	*got = *want;
	got->paced = t_script.paced;
	if (t_script.grant_periods)
		got->prefill_periods = t_script.grant_periods;
	if (t_script.grant_frames)
		got->frames_per_period = t_script.grant_frames;
	if (t_script.grant_rate)
		got->rate = t_script.grant_rate;
	if (t_script.grant_rate_zero)
		got->rate = 0;
	t_geom = *got;

	return (struct udsp_tdm *)&t_token;
}

static void t_close(struct udsp_tdm *t)
{
	t_note('c');
}

static int t_recover(struct udsp_tdm *t)
{
	t_note('r');
	/* The prepare resets both rings, and the engine re-prefills playback
	 * from zero behind it. */
	t_backlog = 0;
	t_queued = 0;

	return 0;
}

static int t_start(struct udsp_tdm *t)
{
	t_note('s');
	if (!t_start_us)
		t_start_us = now_us();

	return t_script.start_err;
}

static int t_stop(struct udsp_tdm *t)
{
	t_note('S');

	return 0;
}

static bool t_in_window(unsigned n, unsigned from, unsigned count)
{
	return count && n >= from && n < from + count;
}

static int t_read(struct udsp_tdm *t, int16_t *buf)
{
	t_reads++;
	if (t_in_window(t_reads, t_script.read_fail_from, t_script.read_fail_count)) {
		t_note('e');
		return t_script.read_fail_err;
	}

	for (unsigned i = 0; i < t_samples(); i++)
		buf[i] = (int16_t)(1 + t_reads * 1000u + i);
	t_note('R');

	return 0;
}

static int t_write(struct udsp_tdm *t, const int16_t *buf)
{
	t_writes++;
	if (t_writes < T_JOURNAL_MAX)
		t_write_first[t_writes - 1] = buf[0];
	if (t_script.write_fail_at == t_writes) {
		t_note('x');
		return t_script.write_fail_err;
	}

	t_note(t_silent(buf) ? 'w' : 'W');

	return 0;
}

static long t_delay(struct udsp_tdm *t)
{
	t_delays++;
	if (t_in_window(t_delays, t_script.delay_fail_from, t_script.delay_fail_count)) {
		t_note('D');
		return t_script.delay_fail_err;
	}

	long queued = (long)(t_geom.prefill_periods - 1) * t_geom.frames_per_period +
		      t_script.delay_bias;

	if (t_in_window(t_delays, t_script.delay_step_from, t_script.delay_step_count))
		queued += t_script.delay_step;

	t_note('d');

	return queued;
}

/* With avail() the engine measures the capture backlog instead of inferring it
 * from its own tick interval. */
static long t_avail(struct udsp_tdm *t)
{
	if (t_script.avail_err) {
		long err = t_script.avail_err;

		t_script.avail_err = 0;
		t_note('A');
		return err;
	}
	t_note('a');

	return t_script.avail_value;
}

static const struct udsp_tdm_ops t_ops = {
	.name = "test",
	.open = t_open,
	.close = t_close,
	.recover = t_recover,
	.start = t_start,
	.stop = t_stop,
	.read_period = t_read,
	.write_period = t_write,
	.delay = t_delay,
};

/* The same backend, plus the direct backlog measurement through avail(). */
static const struct udsp_tdm_ops t_ops_avail = {
	.name = "test-avail",
	.open = t_open,
	.close = t_close,
	.recover = t_recover,
	.start = t_start,
	.stop = t_stop,
	.read_period = t_read,
	.write_period = t_write,
	.delay = t_delay,
	.avail = t_avail,
};

/*
 * The same backend with the three fresh reads of tdm.h, over a model of the
 * two rings. One period of hardware time puts one period into the capture ring
 * and takes one out of the playback ring, and the model spends that time
 * exactly where a real pair does: in the read that waits for its period and in
 * the write that waits for room. A hold is that time passing with no tick to
 * spend it, which is what a kernel holder on the engine CPU leaves behind.
 *
 * The capture ring keeps cap_periods and overwrites what does not fit, so the
 * read reports the periods it lost the way the raw back end does.
 */
#define T_CAP_PERIODS	15u

static long t_period_frames(void)
{
	return (long)t_geom.frames_per_period;
}

static long t_play_ring(void)
{
	return (long)t_geom.prefill_periods * t_period_frames();
}

static long t_cap_ring(void)
{
	unsigned periods = t_script.cap_periods ? t_script.cap_periods :
						  T_CAP_PERIODS;

	return (long)periods * t_period_frames();
}

static void t_time_step(unsigned periods)
{
	long frames = (long)periods * t_period_frames();

	t_backlog += frames;
	t_queued -= t_queued < frames ? t_queued : frames;
}

/* What the ring overwrote before the engine came back for it. */
static unsigned t_overrun_take(void)
{
	long over = t_backlog - t_cap_ring();
	unsigned dropped;

	if (over <= 0)
		return 0;

	dropped = (unsigned)((over + t_period_frames() - 1) /
			     t_period_frames());
	t_backlog -= (long)dropped * t_period_frames();

	return dropped;
}

static int t_hold_read(struct udsp_tdm *t, int16_t *buf)
{
	unsigned dropped;

	t_reads++;
	for (unsigned i = 0; i < 2; i++) {
		if (t_script.hold_at[i] == t_reads)
			t_time_step(t_script.hold_periods[i]);
	}
	if (t_backlog < t_period_frames())
		t_time_step(1);
	dropped = t_overrun_take();
	t_backlog -= t_period_frames();
	for (unsigned i = 0; i < t_samples(); i++)
		buf[i] = (int16_t)(1 + t_reads * 1000u + i);
	t_note('R');

	return (int)dropped;
}

static int t_hold_write(struct udsp_tdm *t, const int16_t *buf)
{
	t_writes++;
	if (t_writes < T_JOURNAL_MAX)
		t_write_first[t_writes - 1] = buf[0];
	if (t_script.write_hold_at && t_script.write_hold_at == t_writes)
		t_time_step(t_script.write_hold_periods);
	else if (t_play_ring() - t_queued < t_period_frames())
		t_time_step(1);
	t_queued += t_period_frames();
	t_note(t_silent(buf) ? 'w' : 'W');
	if (t_script.expire_at == t_writes)
		return (int)t_script.expire_periods;

	return 0;
}

static long t_hold_delay(struct udsp_tdm *t)
{
	t_delays++;
	t_note('d');
	if (t_in_window(t_delays, t_script.delay_step_from,
			t_script.delay_step_count))
		return t_queued + t_script.delay_step;

	return t_queued;
}

static long t_hold_avail(struct udsp_tdm *t)
{
	t_note('a');

	return t_backlog;
}

static long t_cap_avail_fresh(struct udsp_tdm *t)
{
	t_note('f');

	return t_backlog;
}

static long t_play_room_fresh(struct udsp_tdm *t)
{
	t_note('F');

	return t_play_ring() - t_queued;
}

static int t_cap_skip(struct udsp_tdm *t, unsigned periods)
{
	long frames = (long)periods * t_period_frames();

	t_note('k');
	if (t_script.skip_err_n) {
		t_script.skip_err_n--;
		return t_script.skip_err;
	}
	t_skips++;
	t_skip_frames += frames;
	t_backlog -= t_backlog < frames ? t_backlog : frames;

	return 0;
}

static const struct udsp_tdm_ops t_ops_hold = {
	.name = "test-hold",
	.open = t_open,
	.close = t_close,
	.recover = t_recover,
	.start = t_start,
	.stop = t_stop,
	.read_period = t_hold_read,
	.write_period = t_hold_write,
	.delay = t_hold_delay,
	.avail = t_hold_avail,
	.capture_avail_fresh = t_cap_avail_fresh,
	.playback_room_fresh = t_play_room_fresh,
	.capture_skip = t_cap_skip,
};

/* The same back end with one of the three fresh reads missing. The episode
 * state machine needs all three, so each of these keeps the per-tick stall
 * rule. */
#define T_OPS_PARTIAL(sym, label, ...)					\
static const struct udsp_tdm_ops sym = {				\
	.name = label,							\
	.open = t_open,							\
	.close = t_close,						\
	.recover = t_recover,						\
	.start = t_start,						\
	.stop = t_stop,							\
	.read_period = t_hold_read,					\
	.write_period = t_hold_write,					\
	.delay = t_hold_delay,						\
	__VA_ARGS__							\
}

T_OPS_PARTIAL(t_ops_no_skip, "test-no-skip",
	      .avail = t_hold_avail,
	      .capture_avail_fresh = t_cap_avail_fresh,
	      .playback_room_fresh = t_play_room_fresh,);
T_OPS_PARTIAL(t_ops_no_cap_fresh, "test-no-avail-fresh",
	      .avail = t_hold_avail,
	      .playback_room_fresh = t_play_room_fresh,
	      .capture_skip = t_cap_skip,);
T_OPS_PARTIAL(t_ops_no_room, "test-no-room-fresh",
	      .avail = t_hold_avail,
	      .capture_avail_fresh = t_cap_avail_fresh,
	      .capture_skip = t_cap_skip,);
/* The three fresh reads with no avail: the read side has no backlog to open an
 * episode on, and the stall count falls back to the tick-interval
 * inference. */
T_OPS_PARTIAL(t_ops_no_avail, "test-no-avail",
	      .capture_avail_fresh = t_cap_avail_fresh,
	      .playback_room_fresh = t_play_room_fresh,
	      .capture_skip = t_cap_skip,);

/* --- helpers ---------------------------------------------------------- */

static unsigned t_journal_count(const char *needle)
{
	const char *at = t_journal;
	unsigned n = 0;

	while ((at = strstr(at, needle))) {
		n++;
		at++;
	}

	return n;
}

static void tmp_path(char *out)
{
	strcpy(out, "/tmp/udsp_engine_XXXXXX");
	int fd = mkstemp(out);

	if (fd >= 0)
		close(fd);
}

static void cfg_init(struct udsp_config *cfg, const char *play, const char *cap)
{
	memset(cfg, 0, sizeof(*cfg));
	snprintf(cfg->tdm_backend, sizeof(cfg->tdm_backend), "file");
	if (play)
		snprintf(cfg->tdm_playback, sizeof(cfg->tdm_playback), "%s", play);
	if (cap)
		snprintf(cfg->tdm_capture, sizeof(cfg->tdm_capture), "%s", cap);
	cfg->channels = (uint8_t)T_CHANS;
	cfg->prefill_periods = (uint8_t)T_PREFILL;
	cfg->engine_cpu = -1;
	cfg->engine_prio = 0;
}

static void capture_fixture(const char *path, unsigned periods)
{
	FILE *f = fopen(path, "wb");

	if (!f)
		return;

	for (unsigned p = 0; p < periods; p++) {
		for (unsigned i = 0; i < T_NSAMP; i++) {
			int16_t v = (int16_t)(1 + p * 1000u + i);

			fwrite(&v, sizeof(v), 1, f);
		}
	}
	fclose(f);
}

static size_t file_slurp(const char *path, unsigned char *out, size_t max)
{
	FILE *f = fopen(path, "rb");

	if (!f)
		return 0;

	size_t n = fread(out, 1, max, f);

	fclose(f);

	return n;
}

static bool bytes_zero(const unsigned char *p, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (p[i])
			return false;
	}

	return true;
}

static bool engine_wait(struct engine *e, unsigned limit_ms)
{
	for (unsigned i = 0; i < limit_ms; i++) {
		if (engine_done(e))
			return true;
		msleep(1);
	}

	return false;
}

static bool engine_run_ops(const struct udsp_tdm_ops *ops,
			   const struct udsp_config *cfg,
			   const struct engine_opts *opts, struct engine_stats *st)
{
	struct engine *e = ops ? engine_open_ops(ops, cfg, opts, NULL) :
			   engine_open(cfg, opts, NULL);

	if (!e)
		return false;

	bool finished = engine_wait(e, 5000);

	engine_stop(e, st);
	engine_close(e);

	return finished;
}

static bool engine_run(const struct udsp_config *cfg, const struct engine_opts *opts,
		       struct engine_stats *st)
{
	return engine_run_ops(NULL, cfg, opts, st);
}

static bool stats_coherent(const struct engine_stats *s)
{
	uint64_t sum = 0;

	for (unsigned b = 0; b < ENGINE_HIST_BUCKETS; b++)
		sum += s->interval[b];

	if (sum != s->interval_samples)
		return false;
	/* A resync drops the interval across it, so the sample count trails
	 * the tick count on a run that recovered. */
	if (s->interval_samples > (s->ticks ? s->ticks - 1 : 0))
		return false;
	if (s->delay_samples + s->delay_errors != s->ticks)
		return false;

	return s->frames_per_period == T_FRAMES && s->channels == T_CHANS;
}

/* A fault-free run measures every gap between consecutive ticks, so a torn
 * copy shows up as a broken equality, not only as a broken bound. */
static bool stats_exact(const struct engine_stats *s)
{
	return stats_coherent(s) &&
	       s->interval_samples == (s->ticks ? s->ticks - 1 : 0);
}

/* The tick thread skips a publish while a reader holds the count, so the
 * reader pauses between rounds; a back-to-back reader can starve the publish
 * and see a tick count that never moves and means nothing. */
static bool snapshot_watch(struct engine *e, unsigned rounds, uint64_t *last)
{
	bool grew = false;
	uint64_t prev = 0;

	for (unsigned i = 0; i < rounds; i++) {
		struct engine_stats s;

		engine_stats_get(e, &s);
		if (!stats_exact(&s) || s.ticks < prev)
			return false;
		if (s.ticks > prev)
			grew = true;
		prev = s.ticks;
		msleep(1);
	}

	*last = prev;

	return grew;
}

/* A step that succeeded carries no errno, a pinned cpu appears only when the
 * affinity call succeeded, and a step that was not asked for reports neither. */
static bool rt_report_coherent(const struct rt_report *rt)
{
	if (rt->sched_applied && rt->sched_errno)
		return false;
	if (rt->mlock_applied && rt->mlock_errno)
		return false;
	if (rt->affinity_applied != (rt->cpu >= 0))
		return false;

	return true;
}

/* The per-tick injection point: a per-object hook, with no global state in the
 * engine itself. */
static struct {
	unsigned calls;
	unsigned channels;
	unsigned frames;
	uint64_t first_tick;
	uint64_t last_tick;
	int16_t rx_seen;
	bool tx_was_silent;
} hook_ctx;

/* The skip hook's own record: it runs before the tick hook of the same tick,
 * so tick_calls holds how many tick hooks had run when it was called. */
static struct {
	unsigned calls;
	unsigned periods_first;
	unsigned periods_total;
	uint64_t tick_first;
	uint64_t tick_last;
	unsigned tick_calls;
} skip_ctx;

static void hook_reset(void)
{
	memset(&hook_ctx, 0, sizeof(hook_ctx));
	memset(&skip_ctx, 0, sizeof(skip_ctx));
	hook_ctx.tx_was_silent = true;
}

static void hook_skip(void *ctx, unsigned periods, uint64_t tick)
{
	(void)ctx;

	if (!skip_ctx.calls) {
		skip_ctx.periods_first = periods;
		skip_ctx.tick_first = tick;
		skip_ctx.tick_calls = hook_ctx.calls;
	}
	skip_ctx.calls++;
	skip_ctx.periods_total += periods;
	skip_ctx.tick_last = tick;
}

static void hook_tick(void *ctx, int16_t *rx, int16_t *tx, unsigned channels,
		      unsigned frames, uint64_t tick)
{
	(void)ctx;

	if (!hook_ctx.calls)
		hook_ctx.first_tick = tick;
	hook_ctx.last_tick = tick;
	hook_ctx.calls++;
	hook_ctx.channels = channels;
	hook_ctx.frames = frames;
	hook_ctx.rx_seen = rx[0];
	for (unsigned i = 0; i < channels * frames; i++) {
		if (tx[i])
			hook_ctx.tx_was_silent = false;
	}
	for (unsigned i = 0; i < channels * frames; i++)
		tx[i] = (int16_t)0x5aa5;
}

struct reader_arg {
	struct engine *e;
	atomic_bool quit;
};

static void *reader_loop(void *arg)
{
	struct reader_arg *r = arg;
	struct engine_stats s;

	while (!atomic_load(&r->quit))
		engine_stats_get(r->e, &s);

	return NULL;
}

/* One stop with a reader inside the snapshot almost every moment. The final
 * publication must still go out, or every later reader sees a running engine
 * that stopped. */
static bool final_snapshot_round(void)
{
	struct udsp_config cfg;
	struct engine_opts opts = { .loopback = true, .max_ticks = 200000 };
	struct engine_stats fin, pub;
	struct reader_arg r;
	pthread_t reader;

	cfg_init(&cfg, NULL, NULL);

	struct engine *e = engine_open(&cfg, &opts, NULL);

	if (!e)
		return false;

	r.e = e;
	atomic_init(&r.quit, false);
	if (pthread_create(&reader, NULL, reader_loop, &r)) {
		engine_close(e);
		return false;
	}

	msleep(1);
	engine_stop(e, &fin);
	atomic_store(&r.quit, true);
	pthread_join(reader, NULL);
	engine_stats_get(e, &pub);
	engine_close(e);

	return !pub.running && pub.ticks == fin.ticks && !fin.running;
}

/* --- the phased raw model ------------------------------------------------ */

/*
 * The raw back end on the phased clock of tdm_raw_fake.h, with a tick hook of
 * the case's own. The hook spends model time for its work (fk_work), so the
 * write of each tick falls that far after the capture completion that woke
 * it. Two windows of PH_SETTLE_TICKS ticks are read from the engine's
 * snapshot and the model: the clean one ends on the tick before the hold, and
 * the settled one ends the run. Each tick of both must show a completed
 * calibration, and of the settled one a calibration after the episode. The
 * settled window must read the references of the clean one, the same
 * transmit queue after each push (fk_tx_pending), which is the completion
 * order, and the same span of physical ages, the downlink within the cost of
 * the push that restarts a dry ring. Work that varies by tick moves the ages
 * from tick to tick; a window of 40 ticks covers every residue it takes.
 */
#define PH_HOLD_TICK		70u
#define PH_SETTLE_TICKS		40u
#define PH_TICKS		(PH_HOLD_TICK + 240u)

/*
 * The calls of the rephase, seen through a wrapper of the raw back end. The
 * calls between a capture read and the hook of its tick are the rephase's:
 * the room and the backlog it measures, the backlog it skips and the refill
 * it writes. The catch-up and the write of the tick call after the hook. A
 * ph_inject row picks one kind of those calls, lets a count of them pass and
 * then fails some in a row with an errno, or arms a hold at the first one it
 * does not let pass. PH_AT_SETTLE counts the hooks from the first one after
 * a refill, and PH_AT_WRITE the writes after a hook: a refill an abort left,
 * and the write of the tick. Every count starts at the case's hold. A hold
 * at PH_AT_WRITE takes a snapshot of the rephase counters at the next hook.
 */
enum ph_at {
	PH_AT_NONE = 0,
	PH_AT_ROOM,		/* playback_room_fresh before the hook */
	PH_AT_AVAIL,		/* capture_avail_fresh before the hook */
	PH_AT_SKIP,		/* capture_skip before the hook */
	PH_AT_REFILL,		/* write_period before the hook */
	PH_AT_SETTLE,		/* a hook after a refill */
	PH_AT_WRITE,		/* write_period after the hook */
	PH_AT_KINDS,
};

struct ph_inject {
	unsigned at;		/* enum ph_at, PH_AT_NONE for none */
	unsigned skip;		/* calls of the kind that pass first */
	unsigned times;		/* calls in a row the errno fails */
	int err;		/* 0 for no fault */
	unsigned hold;		/* periods of a hold, 0 for none */
	bool hold_in_write;	/* that hold parks the next push */
};

struct ph_cfg {
	const struct fk_profile *prof;
	unsigned rate;
	unsigned prefill;
	unsigned work;		/* frames of hook work each tick */
	unsigned work_var;	/* more work, below this, varied by tick */
	unsigned ack;		/* frames each playback push costs */
	unsigned hold;		/* periods of the hold, 0 for none */
	unsigned extra;		/* frames past them: the release phase */
	unsigned hold2_tick;	/* a second hold at this tick, 0 for none */
	unsigned ticks;		/* the run, PH_TICKS for 0 */
	bool in_write;		/* the hold parks the next playback push */
	bool kick;		/* a dry ring restarts at the kick */
	bool silence;		/* kernel silence fill (cyclic DMA) */
	bool hidden;		/* the platform driver is not registered */
	unsigned fault_dir;	/* FK_PLAY or FK_CAPTURE */
	struct fk_sync_fault fault;	/* armed at the hold, err 0 for none */
	struct ph_inject inject[2];	/* at the rephase's calls */
};

struct ph_obs {
	int32_t delay_ref;
	int32_t loop_ref;
	long down;
	long up;
	unsigned pending;
};

struct ph_span {
	struct ph_obs lo, hi;
	unsigned n;
	bool calibrated;
	uint64_t cal_first;	/* cal_restarts on the first tick */
};

struct ph_out {
	struct engine_stats st;
	struct ph_span clean;
	struct ph_span settled;
	unsigned hold_taken;
	unsigned hold_left;
	unsigned push_taken;
	unsigned dry;
	unsigned empty_reads;
	unsigned empty_pushes;
	unsigned calls[PH_AT_KINDS];	/* the rephase's calls, by kind */
	unsigned failed;		/* calls an injected errno failed */
	unsigned inj_holds;		/* holds armed at a call */
	/* The rephase counters at the first hook after a hold at
	 * PH_AT_WRITE, one snapshot per such hold. */
	unsigned snaps;
	uint64_t snap_starts[2];
	uint64_t snap_aborts[2];
	/* The hooks: the indices each hook got against the engine's count,
	 * the render and control calls, and the dropped reads. */
	unsigned order_bad;
	unsigned renders;
	unsigned controls;
	unsigned drops;
	uint64_t skipped;		/* periods the skip hook got */
	uint64_t wall;			/* periods the capture side passed */
	bool finished;
};

static const struct ph_cfg *ph_c;
static struct ph_out *ph_o;
static struct engine *_Atomic ph_e;

/* Tick thread only, read after the stop. */
static struct {
	bool armed;		/* the case's hold is armed */
	bool pre;		/* between a read and its hook */
	bool refilled;		/* a write came before a hook */
	bool snap_due;		/* a hold at PH_AT_WRITE wants a snapshot */
	unsigned snaps;
	uint64_t snap_starts[2];
	uint64_t snap_aborts[2];
	uint64_t next;		/* the index the next hook gets */
	unsigned order_bad;
	unsigned renders;
	unsigned controls;
	unsigned drops;
	uint64_t skipped;
	uint64_t wall;
	unsigned seen[PH_AT_KINDS];
	unsigned left[2];	/* failures still to come, per row */
	unsigned failed;
	unsigned holds;
} ph_w;
static struct udsp_tdm_ops ph_ops;

static void ph_inject_hold(const struct ph_inject *in)
{
	if (in->hold_in_write)
		fk_hold_push = in->hold;
	else
		fk_hold_periods[FK_CAPTURE] = in->hold;
	ph_w.holds++;
}

/* One call of kind at: 0 passes it on, a negative errno fails it. */
static int ph_inject_call(unsigned at)
{
	unsigned n;
	int rc = 0;

	if (!ph_w.armed ||
	    (at != PH_AT_SETTLE && at != PH_AT_WRITE && !ph_w.pre))
		return 0;
	n = ph_w.seen[at]++;
	for (unsigned i = 0; i < 2u; i++) {
		const struct ph_inject *in = &ph_c->inject[i];

		if (in->at != at || n < in->skip)
			continue;
		if (in->hold && n == in->skip) {
			ph_inject_hold(in);
			ph_w.snap_due = at == PH_AT_WRITE;
		}
		if (in->err && ph_w.left[i]) {
			ph_w.left[i]--;
			ph_w.failed++;
			rc = -in->err;
		}
	}

	return rc;
}

/* A read that finds the last read still without its hook follows a read the
 * engine dropped. */
static int ph_read(struct udsp_tdm *t, int16_t *buf)
{
	int rc = udsp_tdm_alsa_raw.read_period(t, buf);

	if (rc < 0)
		return rc;
	ph_w.drops += ph_w.pre;
	ph_w.pre = true;
	ph_w.wall += 1u + (unsigned)rc;

	return rc;
}

static int ph_write(struct udsp_tdm *t, const int16_t *buf)
{
	int rc = ph_inject_call(ph_w.pre ? PH_AT_REFILL : PH_AT_WRITE);

	if (rc)
		return rc;
	rc = udsp_tdm_alsa_raw.write_period(t, buf);
	if (rc >= 0 && ph_w.pre && ph_w.armed)
		ph_w.refilled = true;

	return rc;
}

static long ph_room(struct udsp_tdm *t)
{
	int rc = ph_inject_call(PH_AT_ROOM);

	return rc ? rc : udsp_tdm_alsa_raw.playback_room_fresh(t);
}

static long ph_avail(struct udsp_tdm *t)
{
	int rc = ph_inject_call(PH_AT_AVAIL);

	return rc ? rc : udsp_tdm_alsa_raw.capture_avail_fresh(t);
}

static int ph_skip(struct udsp_tdm *t, unsigned periods)
{
	int rc = ph_inject_call(PH_AT_SKIP);

	if (!rc)
		rc = udsp_tdm_alsa_raw.capture_skip(t, periods);
	if (!rc)
		ph_w.wall += periods;

	return rc;
}

/* The snapshot a hold at PH_AT_WRITE asked for, at the first hook after it:
 * the counters the write of that hold published. */
static void ph_snap(void)
{
	struct engine_stats st;
	struct engine *e = atomic_load(&ph_e);

	if (!ph_w.snap_due || !e)
		return;
	ph_w.snap_due = false;
	engine_stats_get(e, &st);
	if (ph_w.snaps < 2u) {
		ph_w.snap_starts[ph_w.snaps] = st.rephase_starts;
		ph_w.snap_aborts[ph_w.snaps] = st.rephase_aborts;
	}
	ph_w.snaps++;
}

/* The index of a hook against the engine's count: a skip hook gets the index
 * after the periods it reports, a render or a tick hook the next one. */
static void ph_index(uint64_t tick, uint64_t periods)
{
	ph_w.order_bad += tick != ph_w.next + periods;
	ph_w.next = tick;
}

/* The skip and the control hooks cost a quarter of the work of a tick, and
 * a render the whole of it: model time passes in each, with its
 * completions. */
static void ph_skip_hook(void *ctx, unsigned periods, uint64_t tick)
{
	(void)ctx;
	ph_snap();
	ph_index(tick, periods);
	ph_w.skipped += periods;
	fk_work(ph_c->work / 4u);
}

/* A render runs on a dropped read, between the read and the next one. */
static void ph_render(void *ctx, int16_t *rx, int16_t *tx, unsigned channels,
		      unsigned frames, uint64_t tick)
{
	(void)ctx;
	ph_w.order_bad += !ph_w.pre;
	for (unsigned i = 0; i < channels * frames; i++)
		ph_w.order_bad += rx[i] != 0 || tx[i] != 0;
	ph_index(tick, 0);
	ph_w.next++;
	ph_w.renders++;
	fk_work(ph_c->work);
}

static void ph_control(void *ctx, uint64_t tick)
{
	(void)ctx;
	ph_snap();
	ph_w.order_bad += !ph_w.pre || tick != ph_w.next;
	ph_w.controls++;
	fk_work(ph_c->work / 4u);
}

static void ph_wrap(const struct ph_cfg *c)
{
	memset(&ph_w, 0, sizeof(ph_w));
	for (unsigned i = 0; i < 2u; i++)
		ph_w.left[i] = c->inject[i].err ? c->inject[i].times : 0;
	ph_ops = udsp_tdm_alsa_raw;
	ph_ops.read_period = ph_read;
	ph_ops.write_period = ph_write;
	ph_ops.playback_room_fresh = ph_room;
	ph_ops.capture_avail_fresh = ph_avail;
	ph_ops.capture_skip = ph_skip;
}

static unsigned ph_ticks(const struct ph_cfg *c)
{
	return c->ticks ? c->ticks : PH_TICKS;
}

static void ph_arm(const struct ph_cfg *c)
{
	if (c->in_write) {
		fk_hold_push = c->hold;
		fk_hold_push_extra = c->extra;
	} else {
		fk_hold_periods[FK_CAPTURE] = c->hold;
		fk_hold_extra[FK_CAPTURE] = c->extra;
	}
}

static void ph_observe(struct ph_obs *o, const struct engine_stats *st)
{
	o->delay_ref = st->delay_ref;
	o->loop_ref = st->loop_ref;
	o->down = fk_age_down;
	o->up = fk_age_up;
	o->pending = fk_tx_pending;
}

static void ph_span_note(struct ph_span *w, const struct engine_stats *st,
			 bool after_episode)
{
	struct ph_obs o;

	ph_observe(&o, st);
	if (!st->delay_calibrated || (after_episode && !st->cal_restarts))
		w->calibrated = false;
	if (!w->n++) {
		w->lo = o;
		w->hi = o;
		w->cal_first = st->cal_restarts;
		return;
	}
#define PH_SPAN(f) do {							\
		if (o.f < w->lo.f)					\
			w->lo.f = o.f;					\
		if (o.f > w->hi.f)					\
			w->hi.f = o.f;					\
	} while (0)
	PH_SPAN(delay_ref);
	PH_SPAN(loop_ref);
	PH_SPAN(down);
	PH_SPAN(up);
	PH_SPAN(pending);
#undef PH_SPAN
}

static void ph_tick(void *ctx, int16_t *rx, int16_t *tx, unsigned channels,
		    unsigned frames, uint64_t tick)
{
	const struct ph_cfg *c = ph_c;
	unsigned end = ph_ticks(c);
	struct engine_stats st;
	struct engine *e;
	unsigned work = c->work;

	(void)ctx;
	(void)rx;
	(void)tx;
	(void)channels;
	(void)frames;
	ph_w.pre = false;
	ph_snap();
	ph_index(tick, 0);
	ph_w.next++;
	if (ph_w.refilled)
		(void)ph_inject_call(PH_AT_SETTLE);
	if (c->work_var)
		work += (unsigned)(tick * 7919u % c->work_var);
	fk_work(work);
	if (tick + PH_SETTLE_TICKS >= PH_HOLD_TICK && tick < PH_HOLD_TICK) {
		/* The open stores the engine as it returns; the tick can be
		 * there first on a clock this fast. */
		while (!(e = atomic_load(&ph_e)))
			sched_yield();
		engine_stats_get(e, &st);
		ph_span_note(&ph_o->clean, &st, false);
	}
	if (c->hold && (tick == PH_HOLD_TICK ||
			(c->hold2_tick && tick == c->hold2_tick))) {
		ph_arm(c);
		if (tick == PH_HOLD_TICK && c->fault.err)
			fk_sync_fault[c->fault_dir] = c->fault;
		ph_w.armed = true;
	}
	if (tick + 2u + PH_SETTLE_TICKS >= end && tick + 2u < end &&
	    (e = atomic_load(&ph_e))) {
		engine_stats_get(e, &st);
		ph_span_note(&ph_o->settled, &st, true);
	}
}

static bool ph_run(const struct ph_cfg *c, struct ph_out *o)
{
	struct engine_opts opts = { .loopback = false };
	struct udsp_config cfg;
	struct engine *e;

	memset(o, 0, sizeof(*o));
	o->clean.calibrated = true;
	o->settled.calibrated = true;
	fk_reset(c->prof);
	fk_phased = true;
	fk_tx_restart_at_kick = c->kick;
	fk_ack_frames = c->ack;
	fk_silence_fill = c->silence;
	fk_driver_hidden = c->hidden;
	cfg_init(&cfg, "hw:0,0", "hw:0,1");
	snprintf(cfg.tdm_backend, sizeof(cfg.tdm_backend), "alsa-raw");
	cfg.sample_rate = (uint16_t)c->rate;
	cfg.prefill_periods = (uint8_t)c->prefill;
	ph_c = c;
	ph_o = o;
	ph_wrap(c);
	atomic_store(&ph_e, NULL);
	opts.max_ticks = ph_ticks(c);
	opts.hooks.tick = ph_tick;
	opts.hooks.skip = ph_skip_hook;
	opts.hooks.render = ph_render;
	opts.hooks.control = ph_control;
	e = engine_open_ops(&ph_ops, &cfg, &opts, NULL);
	if (!e) {
		fk_release();
		return false;
	}
	atomic_store(&ph_e, e);
	o->finished = engine_wait(e, 5000);
	engine_stop(e, &o->st);
	atomic_store(&ph_e, NULL);
	engine_close(e);
	o->hold_taken = fk_hold_taken[FK_CAPTURE];
	o->hold_left = fk_hold_periods[FK_CAPTURE] + fk_hold_push;
	o->push_taken = fk_hold_push_taken;
	o->dry = fk_tx_dry_n;
	o->empty_reads = fk_play_empty_reads;
	o->empty_pushes = fk_play_empty_pushes;
	memcpy(o->calls, ph_w.seen, sizeof(o->calls));
	o->failed = ph_w.failed;
	o->inj_holds = ph_w.holds;
	o->order_bad = ph_w.order_bad;
	o->renders = ph_w.renders;
	o->controls = ph_w.controls;
	o->drops = ph_w.drops;
	o->skipped = ph_w.skipped;
	o->wall = ph_w.wall;
	o->snaps = ph_w.snaps;
	memcpy(o->snap_starts, ph_w.snap_starts, sizeof(o->snap_starts));
	memcpy(o->snap_aborts, ph_w.snap_aborts, sizeof(o->snap_aborts));
	fk_release();

	return o->finished;
}

static void ph_line(const char *tag, const struct ph_cfg *c,
		    const struct ph_out *o)
{
	printf("# %s %s %u Hz prefill %u work %u+%u ack %u hold %u+%u%s%s: "
	       "dry %u holds %llu trims %llu rephase %llu/%llu aborts %llu "
	       "held %llu rendered %llu/%llu delay_ref %d..%d/%d "
	       "loop_ref %d..%d/%d pending %u..%u/%u age_down %ld..%ld/%ld..%ld "
	       "age_up %ld..%ld/%ld empty %u/%u calls %u/%u/%u/%u/%u "
	       "failed %u cal %llu errors %llu/%llu/%llu again %llu "
	       "ticks %llu/%llu skipped %llu drops %u/%u order %u\n",
	       tag, c->prof->name, c->rate, c->prefill, c->work, c->work_var,
	       c->ack, c->hold, c->extra, c->in_write ? " in write" : "",
	       c->kick || !c->prof->tx_ack ? "" : " grid", o->dry,
	       (unsigned long long)o->st.holds,
	       (unsigned long long)o->st.trim_drops,
	       (unsigned long long)o->st.rephases,
	       (unsigned long long)o->st.rephase_starts,
	       (unsigned long long)o->st.rephase_aborts,
	       (unsigned long long)o->st.rephase_withheld,
	       (unsigned long long)o->st.rephase_rendered,
	       (unsigned long long)o->st.rephase_lead,
	       o->settled.lo.delay_ref, o->settled.hi.delay_ref,
	       o->clean.hi.delay_ref, o->settled.lo.loop_ref,
	       o->settled.hi.loop_ref, o->clean.hi.loop_ref,
	       o->settled.lo.pending, o->settled.hi.pending,
	       o->clean.hi.pending, o->settled.lo.down, o->settled.hi.down,
	       o->clean.lo.down, o->clean.hi.down, o->settled.lo.up,
	       o->settled.hi.up, o->clean.hi.up, o->empty_reads,
	       o->empty_pushes, o->calls[PH_AT_ROOM], o->calls[PH_AT_AVAIL],
	       o->calls[PH_AT_SKIP], o->calls[PH_AT_REFILL],
	       o->calls[PH_AT_SETTLE], o->failed,
	       (unsigned long long)o->st.cal_restarts,
	       (unsigned long long)o->st.avail_errors,
	       (unsigned long long)o->st.read_errors,
	       (unsigned long long)o->st.write_errors,
	       (unsigned long long)o->st.again,
	       (unsigned long long)o->st.ticks, (unsigned long long)o->wall,
	       (unsigned long long)o->skipped, o->drops, o->controls,
	       o->order_bad);
}

enum ph_want {
	PH_CONVERGE,		/* the loop returns, rephase or not */
	PH_REPHASE,		/* a rephase runs and the loop returns */
	PH_NO_REPHASE,		/* the capability is off: no rephase starts */
};

/* The checks every row shares: each hold was injected and consumed, the
 * first window was calibrated, and the run ended. */
static bool ph_ran(const struct ph_cfg *c, const struct ph_out *o,
		   unsigned holds)
{
	unsigned cap = c->in_write ? 0 : holds;
	unsigned push = c->in_write ? holds : 0;
	unsigned armed = 0;

	for (unsigned i = 0; i < 2u; i++) {
		if (!c->inject[i].hold)
			continue;
		armed++;
		if (c->inject[i].hold_in_write)
			push++;
		else
			cap++;
	}
	CHECK(o->finished);
	CHECK_UINT(o->inj_holds, armed);
	CHECK_UINT(o->hold_left, 0);
	CHECK_UINT(o->push_taken, push);
	CHECK_UINT(o->hold_taken, cap);
	CHECK_UINT(o->clean.n, PH_SETTLE_TICKS);
	CHECK(o->clean.calibrated);
	CHECK_UINT(o->st.xruns, 0);

	return o->finished && o->clean.n;
}

/* The settled window: calibrated after the last episode, and the first
 * window's loop, completion order and ages on every tick of it. Each episode
 * restarts the calibration once, at its end or at the recovery that ends it,
 * and no restart comes after the first tick of the window. */
static void ph_converged(const struct ph_cfg *c, const struct ph_out *o)
{
	const struct ph_span *cl = &o->clean, *se = &o->settled;

	CHECK_UINT(se->n, PH_SETTLE_TICKS);
	CHECK(se->calibrated);
	CHECK(o->st.cal_restarts >= 1);
	CHECK_UINT(o->st.cal_restarts, o->st.holds);
	CHECK_UINT(se->cal_first, o->st.cal_restarts);
	CHECK(o->st.delay_calibrated);
	CHECK_UINT((uint64_t)cl->lo.delay_ref, (uint64_t)cl->hi.delay_ref);
	CHECK_UINT((uint64_t)cl->lo.loop_ref, (uint64_t)cl->hi.loop_ref);
	CHECK_UINT((uint64_t)se->lo.delay_ref, (uint64_t)cl->hi.delay_ref);
	CHECK_UINT((uint64_t)se->hi.delay_ref, (uint64_t)cl->hi.delay_ref);
	CHECK_UINT((uint64_t)se->lo.loop_ref, (uint64_t)cl->hi.loop_ref);
	CHECK_UINT((uint64_t)se->hi.loop_ref, (uint64_t)cl->hi.loop_ref);
	CHECK_UINT(se->lo.pending, cl->lo.pending);
	CHECK_UINT(se->hi.pending, cl->hi.pending);
	CHECK(se->lo.down >= cl->lo.down &&
	      se->lo.down - cl->lo.down <= (long)c->ack);
	CHECK(se->hi.down >= cl->hi.down &&
	      se->hi.down - cl->hi.down <= (long)c->ack);
	CHECK_UINT((uint64_t)se->lo.up, (uint64_t)cl->lo.up);
	CHECK_UINT((uint64_t)se->hi.up, (uint64_t)cl->hi.up);
}

/*
 * The clock of a run without a fault. Every index a hook got is the engine's
 * count at that call, and the count ends on the periods the capture side
 * passed: each read, overwritten period and skipped period is one tick, a
 * skip hook period or a render, exactly once. The refill took nothing ahead
 * of the capture, and each dropped read ran the control hook.
 */
static void ph_accounted(const struct ph_out *o)
{
	CHECK_UINT(o->order_bad, 0);
	CHECK_UINT(o->st.rephase_lead, 0);
	CHECK_UINT(o->renders, o->st.rephase_rendered);
	CHECK_UINT(o->st.ticks, o->wall);
	CHECK(o->controls >= o->drops && o->controls <= o->drops + 1u);
}

static void ph_check(const char *tag, const struct ph_cfg *c,
		     enum ph_want want, bool dry_wanted)
{
	unsigned holds = c->hold ? (c->hold2_tick ? 2u : 1u) : 0u;
	struct ph_out o;

	CHECK(ph_run(c, &o));
	ph_line(tag, c, &o);
	if (!ph_ran(c, &o, holds))
		return;
	CHECK(o.st.holds >= 1);
	CHECK(dry_wanted ? o.dry >= 1 : o.dry == 0);
	ph_accounted(&o);
	if (want == PH_NO_REPHASE) {
		CHECK_UINT(o.st.rephase_starts, 0);
		CHECK_UINT(o.st.rephase_rendered, 0);
		CHECK_UINT(o.renders, 0);
		return;
	}
	if (want == PH_REPHASE) {
		CHECK(o.st.rephases >= holds);
		CHECK_UINT(o.st.rephase_aborts, 0);
		/* One attempt per dry hold: no write of the rephase starts
		 * another. Each renders the rest depth less one period. */
		CHECK_UINT(o.st.rephase_starts, o.st.rephases);
		CHECK_UINT(o.st.rephase_rendered,
			   o.st.rephases * (o.clean.hi.delay_ref /
					    (int32_t)(c->rate / 100u) - 1));
	} else {
		CHECK_UINT(o.st.rephase_starts, 0);
	}
	ph_converged(c, &o);
}

/*
 * The rephase of a dry transmit ring, on the phased clock. A hold longer than
 * the queue runs the AN7581 ring dry, inside a read or inside a write. The
 * rows sweep the release phase, the hook work and the push cost. The run must
 * settle at the references, the queue after each push and the ages of the
 * clean window. Other rows cover both restart variants, two holds and a fault
 * inside the back end's calls; case_phased_transitions covers the rest. The
 * capability negatives and two controls need no rephase.
 */
static void case_phased_hold(void)
{
	static const unsigned rates[2] = { 8000, 16000 };

	CASE("a dry transmit hold leaves the loop where the first window "
	     "put it");
	for (unsigned r = 0; r < 2u; r++) {
		unsigned p = rates[r] / 100u;
		const unsigned works[2] = { p / 16u, p / 4u };
		const unsigned acks[2] = { 0, p / 32u };
		const unsigned phases[5] = { 0, p / 8u, p / 4u, p / 2u,
					     3u * p / 4u };

		for (unsigned w = 0; w < 2u; w++) {
			for (unsigned a = 0; a < 2u; a++) {
				for (unsigned f = 0; f < 5u; f++) {
					struct ph_cfg c = {
						.prof = &fk_an7581,
						.rate = rates[r], .prefill = 3,
						.work = works[w],
						.ack = acks[a],
						.hold = 6, .extra = phases[f],
						.kick = true,
					};

					ph_check("dry", &c, PH_REPHASE, true);
				}
			}
		}
		/* Work that varies from tick to tick, and a prefill of two. */
		for (unsigned f = 0; f < 2u; f++) {
			struct ph_cfg c = {
				.prof = &fk_an7581, .rate = rates[r],
				.prefill = 3, .work = p / 16u,
				.work_var = p / 4u, .hold = 6,
				.extra = f * p / 2u, .kick = true,
			};

			ph_check("dry varied", &c, PH_REPHASE, true);
			c.prefill = 2;
			c.work_var = 0;
			ph_check("dry small", &c, PH_REPHASE, true);
		}
	}

	CASE("a dry transmit hold inside a write leaves the loop where the "
	     "first window put it");
	for (unsigned r = 0; r < 2u; r++) {
		unsigned p = rates[r] / 100u;

		for (unsigned f = 0; f < 3u; f++) {
			struct ph_cfg c = {
				.prof = &fk_an7581, .rate = rates[r],
				.prefill = 3, .work = p / 16u, .hold = 6,
				.extra = f * p / 4u, .in_write = true,
				.kick = true,
			};

			ph_check("write", &c, PH_REPHASE, true);
		}
		/* A deep drain: twelve periods of capture wait behind a ring
		 * the write just restarted. */
		{
			struct ph_cfg c = {
				.prof = &fk_an7581, .rate = rates[r],
				.prefill = 3, .work = p / 16u, .hold = 12,
				.extra = p / 3u, .in_write = true,
				.kick = true,
			};

			ph_check("write deep", &c, PH_REPHASE, true);
		}
	}

	/* A ring of four queued periods that a hold of three leaves with its
	 * last descriptor in flight. Released one frame before that descriptor
	 * ends, the ring runs dry inside the first catch-up tick, and the write
	 * of that tick restarts it. */
	CASE("a transmit ring that runs dry inside the catch-up leaves the "
	     "loop where the first window put it");
	for (unsigned r = 0; r < 2u; r++) {
		unsigned p = rates[r] / 100u;
		struct ph_cfg c = {
			.prof = &fk_an7581, .rate = rates[r], .prefill = 4,
			.work = p / 16u, .hold = 3, .extra = p - p / 16u - 1u,
			.kick = true,
		};

		ph_check("near-empty", &c, PH_REPHASE, true);
	}

	/* The work of the drop: the skip and control hooks and two renders of a
	 * quarter period each, so a capture completion falls inside the work at
	 * some release phases. The release sweeps the period in sixteenths, and
	 * the step just before a completion. The restart must wait for the
	 * completion after the work, at every phase. */
	CASE("a capture completion during the work of a drop does not start the "
	     "ring late");
	for (unsigned r = 0; r < 2u; r++) {
		unsigned p = rates[r] / 100u;

		for (unsigned f = 0; f <= 16u; f++) {
			struct ph_cfg c = {
				.prof = &fk_an7581, .rate = rates[r],
				.prefill = 3, .work = p / 4u, .hold = 6,
				.extra = f < 16u ? f * p / 16u : p - 1u,
				.kick = true,
			};

			ph_check("work", &c, PH_REPHASE, true);
		}
	}

	/* The grid variant: the restarted ring starts at the next capture
	 * completion, so its first period completes a period after that, and
	 * the first write after the restart comes before it. The settle check
	 * holds a write back until the queue reads the rest depth. */
	CASE("a dry ring that restarts on the next capture completion settles "
	     "at the rest depth");
	for (unsigned r = 0; r < 2u; r++) {
		unsigned p = rates[r] / 100u;
		struct ph_cfg c = {
			.prof = &fk_an7581, .rate = rates[r], .prefill = 3,
			.work = p / 16u, .hold = 6, .extra = p / 2u,
		};
		struct ph_out o;

		ph_check("grid", &c, PH_REPHASE, true);
		CHECK(ph_run(&c, &o));
		CHECK(o.st.rephase_withheld >= 1);
	}

	CASE("two dry holds each leave the loop where the first window put it");
	for (unsigned r = 0; r < 2u; r++) {
		unsigned p = rates[r] / 100u;
		struct ph_cfg c = {
			.prof = &fk_an7581, .rate = rates[r], .prefill = 3,
			.work = p / 16u, .hold = 6, .extra = p / 4u,
			.hold2_tick = PH_HOLD_TICK + 100u,
			.ticks = PH_TICKS + 100u, .kick = true,
		};

		ph_check("twice", &c, PH_REPHASE, true);
	}

	/* A fault inside the back end, at the room read of the entry, at the
	 * backlog skip of the entry and at the first refill write. A
	 * recovery rebuilds both rings with a linked start and clears the
	 * attempt; a transient fault retries the one write it hit. */
	CASE("a fault inside a rephase recovers and leaves the loop where the "
	     "first window put it");
	{
		const struct {
			const char *tag;
			unsigned dir;
			struct fk_sync_fault fault;
			enum ph_want want;
		} rows[] = {
			{ "fault room", FK_PLAY, { EIO, false, 0, 0, 0, 0 },
			  PH_CONVERGE },
			{ "fault skip", FK_CAPTURE, { EIO, true, 1, 0, 0, 0 },
			  PH_CONVERGE },
			{ "fault refill eintr", FK_PLAY,
			  { EINTR, true, 1, 0, 0, 0 }, PH_REPHASE },
			{ "fault refill", FK_PLAY, { EIO, true, 1, 0, 0, 0 },
			  PH_CONVERGE },
		};

		for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
			struct ph_cfg c = {
				.prof = &fk_an7581, .rate = 16000,
				.prefill = 3, .work = 10, .hold = 6,
				.extra = 40, .kick = true,
				.fault_dir = rows[i].dir,
				.fault = rows[i].fault,
			};
			struct ph_out o;

			CHECK(ph_run(&c, &o));
			ph_line(rows[i].tag, &c, &o);
			if (!ph_ran(&c, &o, 1))
				continue;
			if (rows[i].want == PH_REPHASE) {
				CHECK_UINT(o.st.rephases, 1);
				CHECK_UINT(o.st.rephase_rendered, 2);
				CHECK(o.st.again >= 1);
			} else {
				/* The fault recovered: one restart of the
				 * pair, and no refill rendered twice. */
				CHECK(o.st.read_errors + o.st.write_errors +
				      o.st.avail_errors >= 1);
				CHECK(o.st.rephase_rendered <= 2);
				CHECK_UINT(o.st.rephases, 0);
			}
			ph_converged(&c, &o);
		}
	}

	/* The capability gate. A cyclic DMA runs on through an empty queue, so
	 * the room equal to the ring and the write that leaves one period both
	 * occur on it; it never rephases. With no hook work, a read hold of
	 * three periods ends exactly when the queue of three runs out, on the
	 * capture completion that opens the episode, and a hold of two inside
	 * the next write ends when the two periods before it run out. */
	CASE("a cyclic playback dma at an exactly empty queue never rephases");
	for (unsigned r = 0; r < 2u; r++) {
		struct ph_cfg at_read = {
			.prof = &fk_mt7986, .rate = rates[r], .prefill = 3,
			.hold = 3, .silence = true,
		};
		struct ph_cfg at_write = {
			.prof = &fk_mt7986, .rate = rates[r], .prefill = 3,
			.hold = 2, .in_write = true, .silence = true,
		};
		struct ph_out o;

		CHECK(ph_run(&at_read, &o));
		ph_line("cyclic exact read", &at_read, &o);
		CHECK(o.st.holds >= 1);
		CHECK(o.empty_reads >= 1);
		CHECK_UINT(o.st.rephase_starts, 0);
		CHECK(ph_run(&at_write, &o));
		ph_line("cyclic exact write", &at_write, &o);
		CHECK(o.st.holds >= 1);
		CHECK(o.empty_pushes >= 1);
		CHECK_UINT(o.st.rephase_starts, 0);
	}

	CASE("a ring without the restart capability never rephases");
	{
		/* BATCH without SYNC_APPLPTR or a known driver. */
		struct ph_cfg batch = {
			.prof = &fk_batch_mmap, .rate = 8000, .prefill = 3,
			.work = 5, .hold = 6,
		};
		/* Both flags, but no known driver registered. */
		struct ph_cfg hidden = {
			.prof = &fk_an7581, .rate = 16000, .prefill = 3,
			.work = 10, .hold = 6, .kick = true, .hidden = true,
		};
		/* A granted ring of two: the rest depth is one period, and no
		 * early write can come before the hook. */
		struct ph_cfg small = {
			.prof = &fk_an7581, .rate = 16000, .prefill = 1,
			.work = 10, .hold = 6, .kick = true,
		};

		ph_check("batch", &batch, PH_NO_REPHASE, false);
		ph_check("hidden", &hidden, PH_NO_REPHASE, true);
		ph_check("granted two", &small, PH_NO_REPHASE, true);
	}

	/* The controls: a ring that never runs dry, and a cyclic DMA that runs
	 * past the application pointer. With a prefill of three, every hold
	 * that opens an episode also runs the ring dry, so the armed control
	 * queues four. */
	CASE("a hold that leaves the transmit ring armed keeps the loop");
	for (unsigned r = 0; r < 2u; r++) {
		unsigned p = rates[r] / 100u;
		struct ph_cfg c = {
			.prof = &fk_an7581, .rate = rates[r], .prefill = 4,
			.work = p / 16u, .hold = 3, .kick = true,
		};

		ph_check("armed", &c, PH_CONVERGE, false);
	}

	CASE("a hold on a cyclic playback dma keeps the loop");
	for (unsigned r = 0; r < 2u; r++) {
		unsigned p = rates[r] / 100u;
		struct ph_cfg c = {
			.prof = &fk_mt7986, .rate = rates[r], .prefill = 3,
			.work = p / 16u, .hold = 6, .silence = true,
		};

		ph_check("cyclic", &c, PH_CONVERGE, false);
	}
}

/* A row of the transition cases: the dry hold of the fault rows, a fault or
 * a hold at calls of the rephase, and the exact record it leaves. */
struct ph_row {
	const char *tag;
	bool in_write;		/* the case's hold parks a push */
	struct ph_inject in;
	unsigned episodes;	/* holds */
	unsigned starts;	/* rephase_starts */
	unsigned rephases;
	unsigned aborts;
	unsigned rendered;	/* rephase_rendered */
	unsigned trims;		/* trim_drops */
	unsigned failed;	/* calls the errno failed */
	unsigned errors;	/* faults that recovered */
	unsigned dry;		/* fk_tx_dry_n */
	unsigned hold2;		/* a second hold this many ticks later */
	unsigned lead;		/* rephase_lead */
	/* The loop stands at the rest depth, at the phase of a write after
	 * the hook: an abort wrote the refill as ordinary writes. */
	bool late;
	struct ph_inject in2;	/* a second injection, none for 0 */
	unsigned prefill;	/* 3 for 0 */
	unsigned late_frames;	/* the restart may come this late */
	/* The snapshots of the holds at PH_AT_WRITE: rephase_starts and
	 * rephase_aborts at the first hook after each. */
	unsigned snaps;
	unsigned snap_starts[2];
	unsigned snap_aborts[2];
};

/* A loop that stands still: one calibration after the last episode, and the
 * same references and completion order on every tick of the window. */
static void ph_steady(const struct ph_out *o)
{
	const struct ph_span *se = &o->settled;

	CHECK_UINT(se->n, PH_SETTLE_TICKS);
	CHECK(se->calibrated);
	CHECK_UINT(o->st.cal_restarts, o->st.holds);
	CHECK_UINT(se->cal_first, o->st.cal_restarts);
	CHECK_UINT((uint64_t)se->lo.delay_ref, (uint64_t)se->hi.delay_ref);
	CHECK_UINT((uint64_t)se->lo.loop_ref, (uint64_t)se->hi.loop_ref);
	CHECK_UINT(se->lo.pending, se->hi.pending);
}

static void ph_row_run(const struct ph_row *r)
{
	struct ph_cfg c = {
		.prof = &fk_an7581, .rate = 16000,
		.prefill = r->prefill ? r->prefill : 3u, .work = 10,
		.hold = 6, .extra = 40, .in_write = r->in_write, .kick = true,
		.inject = { r->in, r->in2 },
	};
	struct ph_cfg wide;
	struct ph_out o;

	if (r->hold2) {
		c.hold2_tick = PH_HOLD_TICK + r->hold2;
		c.ticks = PH_TICKS + r->hold2;
	}
	CHECK(ph_run(&c, &o));
	ph_line(r->tag, &c, &o);
	if (!ph_ran(&c, &o, r->hold2 ? 2u : 1u))
		return;
	CHECK_UINT(o.st.holds, r->episodes);
	CHECK_UINT(o.st.rephase_starts, r->starts);
	CHECK_UINT(o.st.rephases, r->rephases);
	CHECK_UINT(o.st.rephase_aborts, r->aborts);
	CHECK_UINT(o.st.rephase_rendered, r->rendered);
	CHECK_UINT(o.st.trim_drops, r->trims);
	CHECK_UINT(o.st.capture_drops, 0);
	CHECK_UINT(o.failed, r->failed);
	CHECK_UINT(o.st.read_errors + o.st.write_errors + o.st.avail_errors,
		   r->errors);
	CHECK_UINT(o.dry, r->dry);
	CHECK_UINT(o.renders, r->rendered);
	CHECK_UINT(o.st.rephase_lead, r->lead);
	/* Lead or not, the clock ends on the capture. A recovery after a
	 * hook loses the index of that hook. */
	if (!r->errors)
		CHECK_UINT(o.st.ticks, o.wall);
	CHECK_UINT(o.snaps, r->snaps);
	for (unsigned i = 0; i < r->snaps && i < 2u; i++) {
		CHECK_UINT(o.snap_starts[i], r->snap_starts[i]);
		CHECK_UINT(o.snap_aborts[i], r->snap_aborts[i]);
	}
	if (!r->late) {
		wide = c;
		wide.ack += r->late_frames;
		ph_converged(&wide, &o);
		return;
	}
	ph_steady(&o);
	CHECK_UINT((uint64_t)o.settled.hi.delay_ref,
		   (uint64_t)o.clean.hi.delay_ref);
	CHECK_UINT((uint64_t)o.settled.hi.loop_ref,
		   (uint64_t)o.clean.hi.loop_ref);
	CHECK_UINT(o.settled.hi.pending, o.clean.hi.pending);
	CHECK_UINT((uint64_t)o.settled.lo.down,
		   (uint64_t)(o.clean.lo.down + (long)c.work));
	CHECK_UINT((uint64_t)o.settled.hi.down,
		   (uint64_t)(o.clean.hi.down + (long)c.work));
}

#define PH_FAULT(at, skip, times, err)	{ at, skip, times, err, 0, false }
#define PH_HOLD(at, skip, n, in_write)	{ at, skip, 0, 0, n, in_write }

/*
 * The transitions of the rephase, one call at a time. Each row runs the dry
 * hold of the fault rows: AN7581 at 16 kHz, prefill three, a ring of four, six
 * periods. Undisturbed, it opens one episode with one start and two rendered
 * periods. It trims six periods at the read, or seven inside a write. The
 * drain takes at most six reads after its first.
 */
static void case_phased_transitions(void)
{
	static const struct ph_row faults[] = {
		/* A fault at the drain's room or backlog measurement, and at
		 * the backlog measurement after the work of the first drop. */
		{ .tag = "drain room", .in = PH_FAULT(PH_AT_ROOM, 1, 1, EIO),
		  .episodes = 1, .starts = 1, .rendered = 2, .trims = 6,
		  .failed = 1, .errors = 1, .dry = 1 },
		{ .tag = "drain avail",
		  .in = PH_FAULT(PH_AT_AVAIL, 2, 1, EIO),
		  .episodes = 1, .starts = 1, .rendered = 2, .trims = 6,
		  .failed = 1, .errors = 1, .dry = 1 },
		{ .tag = "fresh avail",
		  .in = PH_FAULT(PH_AT_AVAIL, 1, 1, EIO),
		  .episodes = 1, .starts = 1, .rendered = 2, .trims = 6,
		  .failed = 1, .errors = 1, .dry = 1 },
		/* The read is dropped and the next one is not fresh, so the
		 * drain takes two more reads. */
		{ .tag = "drain avail eintr",
		  .in = PH_FAULT(PH_AT_AVAIL, 2, 1, EINTR),
		  .episodes = 1, .starts = 1, .rephases = 1, .rendered = 2,
		  .trims = 8, .failed = 1, .dry = 1 },
		/* The check after the work of the first drop fails, so the
		 * next read is not fresh and is dropped too. */
		{ .tag = "fresh avail eintr",
		  .in = PH_FAULT(PH_AT_AVAIL, 1, 1, EINTR),
		  .episodes = 1, .starts = 1, .rephases = 1, .rendered = 2,
		  .trims = 7, .failed = 1, .dry = 1 },
		/* The backlog clear fails: the next read comes from the
		 * backlog, and the drain clears it then. The first drop
		 * skipped one period and rendered two, so the clock leads by
		 * one until the next drop pays it. */
		{ .tag = "entry skip eintr",
		  .in = PH_FAULT(PH_AT_SKIP, 0, 1, EINTR),
		  .episodes = 1, .starts = 1, .rephases = 1, .rendered = 2,
		  .trims = 6, .failed = 1, .dry = 1, .lead = 1 },
		/* One refill period written, then a fault on the second. */
		{ .tag = "refill partial",
		  .in = PH_FAULT(PH_AT_REFILL, 1, 1, EIO),
		  .episodes = 1, .starts = 1, .rendered = 2, .trims = 6,
		  .failed = 1, .errors = 1, .dry = 1 },
		{ .tag = "refill partial eintr",
		  .in = PH_FAULT(PH_AT_REFILL, 1, 1, EINTR),
		  .episodes = 1, .starts = 1, .rephases = 1, .rendered = 2,
		  .trims = 6, .failed = 1, .dry = 1 },
	};
	static const struct ph_row aborts[] = {
		/* Six drain reads fail and are dropped, the seventh aborts.
		 * Its tick writes the refill and then its own period, so the
		 * ring stands at the rest depth from there, at the phase of
		 * that write: the hook work late. */
		{ .tag = "drain room eintr",
		  .in = PH_FAULT(PH_AT_ROOM, 1, 6, EINTR),
		  .episodes = 1, .starts = 1, .aborts = 1, .rendered = 2,
		  .trims = 12, .failed = 6, .dry = 1, .late = true },
		{ .tag = "drain avail eintr",
		  .in = PH_FAULT(PH_AT_AVAIL, 1, 6, EINTR),
		  .episodes = 1, .starts = 1, .aborts = 1, .rendered = 2,
		  .trims = 12, .failed = 6, .dry = 1, .late = true },
		/* Faults that stay: a second dry hold. Its first read meets a
		 * fault and runs the tick, whose write starts the attempt, and
		 * that attempt aborts within the same bound. Its first drop
		 * skips one period, so the clock leads by one until the next
		 * drop pays it. */
		{ .tag = "drain room eintr twice",
		  .in = PH_FAULT(PH_AT_ROOM, 1, 12, EINTR),
		  .episodes = 2, .starts = 2, .aborts = 2, .rendered = 4,
		  .trims = 18, .failed = 12, .dry = 3, .hold2 = 100,
		  .lead = 1, .late = true },
		/* The first refill write fails three times. */
		{ .tag = "refill eintr",
		  .in = PH_FAULT(PH_AT_REFILL, 0, 3, EINTR),
		  .episodes = 1, .starts = 1, .aborts = 1, .rendered = 2,
		  .trims = 6, .failed = 3, .dry = 1, .late = true },
		/* The bar after an abort. The first refill write fails three
		 * times and the attempt aborts; its refill goes out after the
		 * hook. The write of the tick then waits two periods, so it
		 * finds the ring empty and leaves one period queued. The bar
		 * keeps that write from starting a new attempt at once. */
		{ .tag = "refill eintr, late write",
		  .in = PH_FAULT(PH_AT_REFILL, 0, 3, EINTR),
		  .in2 = PH_HOLD(PH_AT_WRITE, 3, 2, true),
		  .episodes = 1, .starts = 1, .aborts = 1, .rendered = 2,
		  .trims = 6, .failed = 3, .dry = 2, .late = true, .snaps = 1,
		  .snap_starts = { 1 }, .snap_aborts = { 1 } },
		/* Then a window completes, which lifts the bar. A later write
		 * waits one period on a ring of rest depth two: it finds the
		 * ring empty, and starts a new attempt. */
		{ .tag = "refill eintr, window, late write", .prefill = 2,
		  .in = PH_FAULT(PH_AT_REFILL, 0, 3, EINTR),
		  .in2 = PH_HOLD(PH_AT_WRITE, 40, 1, true),
		  .episodes = 2, .starts = 2, .rephases = 1, .aborts = 1,
		  .rendered = 2, .trims = 8, .failed = 3, .dry = 3, .snaps = 1,
		  .snap_starts = { 2 }, .snap_aborts = { 1 } },
		/* The second one: the first restarted the ring on the
		 * completion, and the second goes out after the hook, ahead
		 * of the period of the tick. */
		{ .tag = "refill eintr after one",
		  .in = PH_FAULT(PH_AT_REFILL, 1, 3, EINTR),
		  .episodes = 1, .starts = 1, .aborts = 1, .rendered = 2,
		  .trims = 6, .failed = 3, .dry = 1 },
	};
	static const struct ph_row holds[] = {
		/* A hold of one and of four periods at the second drain
		 * read of a write hold. */
		/* The hold falls in the wait of the read after the work of the
		 * drop, so that read returns late on its completion, by the
		 * work of the skip and the control hooks, and the restart
		 * with it. */
		{ .tag = "hold in drain", .in_write = true,
		  .in = PH_HOLD(PH_AT_ROOM, 1, 1, false),
		  .episodes = 1, .starts = 1, .rephases = 1, .rendered = 2,
		  .trims = 7, .dry = 2, .late_frames = 4 },
		{ .tag = "hold in drain deep", .in_write = true,
		  .in = PH_HOLD(PH_AT_ROOM, 1, 4, false),
		  .episodes = 1, .starts = 1, .rephases = 1, .rendered = 2,
		  .trims = 11, .dry = 2 },
		/* A hold of six periods inside the first or the second
		 * refill write. */
		{ .tag = "hold in refill",
		  .in = PH_HOLD(PH_AT_REFILL, 0, 6, true),
		  .episodes = 1, .starts = 1, .rephases = 1, .rendered = 2,
		  .trims = 12, .dry = 1 },
		{ .tag = "hold in second refill",
		  .in = PH_HOLD(PH_AT_REFILL, 1, 6, true),
		  .episodes = 1, .starts = 1, .rephases = 1, .rendered = 2,
		  .trims = 11, .dry = 2 },
		/* A hold of six periods at the first and at the fourth hook
		 * of the settle check: the ring runs dry again, and a second
		 * attempt runs inside the same episode. */
		{ .tag = "hold in settle",
		  .in = PH_HOLD(PH_AT_SETTLE, 0, 6, false),
		  .episodes = 1, .starts = 2, .rephases = 2, .rendered = 4,
		  .trims = 12, .dry = 3 },
		{ .tag = "hold late in settle",
		  .in = PH_HOLD(PH_AT_SETTLE, 3, 6, false),
		  .episodes = 1, .starts = 2, .rephases = 2, .rendered = 4,
		  .trims = 12, .dry = 3 },
	};

	CASE("a fault at a call of the drain or the refill recovers or "
	     "retries, and the loop returns");
	for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); i++)
		ph_row_run(&faults[i]);

	CASE("a rephase that keeps failing aborts within its bound and writes "
	     "its refill");
	for (unsigned i = 0; i < sizeof(aborts) / sizeof(aborts[0]); i++)
		ph_row_run(&aborts[i]);

	CASE("a hold while a rephase runs ends in one settled episode");
	for (unsigned i = 0; i < sizeof(holds) / sizeof(holds[0]); i++)
		ph_row_run(&holds[i]);
}

int main(void)
{
	/* mlockall(MCL_FUTURE) succeeding here would lock every later mapping,
	 * including a sanitizer's shadow, so the RT case runs against a zero
	 * lock budget and exercises the refusal path the target also hits. */
	struct rlimit lock = { .rlim_cur = 0, .rlim_max = 0 };

	setrlimit(RLIMIT_MEMLOCK, &lock);

	CASE("engine runs the requested ticks and stops cleanly");
	{
		char play[64];
		tmp_path(play);
		struct udsp_config cfg;
		cfg_init(&cfg, play, NULL);
		struct engine_opts opts = { .loopback = false, .max_ticks = 25 };
		struct engine_stats st;

		CHECK(engine_run(&cfg, &opts, &st));
		CHECK_UINT(st.ticks, 25);
		CHECK(!st.running);
		CHECK(!st.stopped_fatal);
		CHECK_UINT(st.read_errors + st.write_errors + st.xruns, 0);
		CHECK_UINT(st.prefill_periods, T_PREFILL);
		CHECK_UINT(st.granted_periods, T_PREFILL + 1);
		CHECK_UINT(st.frames_per_period, T_FRAMES);
		CHECK_UINT(st.channels, T_CHANS);
		CHECK(!st.paced);
		struct stat sb;
		CHECK(stat(play, &sb) == 0);
		CHECK_UINT((uint64_t)sb.st_size, (uint64_t)((T_PREFILL + 25) * T_NBYTE));
		unlink(play);
	}

	CASE("engine writes silence when loopback is off");
	{
		char play[64], cap[64];
		tmp_path(play);
		tmp_path(cap);
		capture_fixture(cap, 4);
		struct udsp_config cfg;
		cfg_init(&cfg, play, cap);
		struct engine_opts opts = { .loopback = false, .max_ticks = 4 };
		struct engine_stats st;

		CHECK(engine_run(&cfg, &opts, &st));
		CHECK_UINT(st.ticks, 4);
		size_t n = file_slurp(play, t_out, sizeof(t_out));
		CHECK_UINT((uint64_t)n, (uint64_t)((T_PREFILL + 4) * T_NBYTE));
		CHECK(bytes_zero(t_out, n));
		unlink(play);
		unlink(cap);
	}

	CASE("loopback copies capture periods to playback");
	{
		char play[64], cap[64];
		tmp_path(play);
		tmp_path(cap);
		capture_fixture(cap, 6);
		struct udsp_config cfg;
		cfg_init(&cfg, play, cap);
		struct engine_opts opts = { .loopback = true, .max_ticks = 6 };
		struct engine_stats st;

		CHECK(engine_run(&cfg, &opts, &st));
		CHECK_UINT(st.ticks, 6);
		size_t n = file_slurp(play, t_out, sizeof(t_out));
		size_t c = file_slurp(cap, t_cap, sizeof(t_cap));
		CHECK_UINT((uint64_t)c, (uint64_t)(6 * T_NBYTE));
		CHECK_UINT((uint64_t)n, (uint64_t)((T_PREFILL + 6) * T_NBYTE));
		CHECK(memcmp(t_out + T_PREFILL * T_NBYTE, t_cap, c) == 0);
		unlink(play);
		unlink(cap);
	}

	CASE("prefill silence precedes the first loopback period");
	{
		char play[64], cap[64];
		tmp_path(play);
		tmp_path(cap);
		capture_fixture(cap, 4);
		struct udsp_config cfg;
		cfg_init(&cfg, play, cap);
		struct engine_opts opts = { .loopback = true, .max_ticks = 4 };
		struct engine_stats st;

		CHECK(engine_run(&cfg, &opts, &st));
		size_t n = file_slurp(play, t_out, sizeof(t_out));
		size_t c = file_slurp(cap, t_cap, sizeof(t_cap));
		CHECK_UINT((uint64_t)c, (uint64_t)(4 * T_NBYTE));
		CHECK_UINT((uint64_t)n, (uint64_t)((T_PREFILL + 4) * T_NBYTE));
		CHECK(bytes_zero(t_out, T_PREFILL * T_NBYTE));
		CHECK(!bytes_zero(t_cap, T_NBYTE));
		CHECK(memcmp(t_out + T_PREFILL * T_NBYTE, t_cap, T_NBYTE) == 0);
		unlink(play);
		unlink(cap);
	}

	CASE("a short capture counts an xrun and re-prefills");
	{
		char play[64], cap[64];
		tmp_path(play);
		tmp_path(cap);
		capture_fixture(cap, 3);
		struct udsp_config cfg;
		cfg_init(&cfg, play, cap);
		struct engine_opts opts = { .loopback = true, .max_ticks = 8 };
		struct engine_stats st;

		CHECK(engine_run(&cfg, &opts, &st));
		CHECK_UINT(st.ticks, 8);
		CHECK_UINT(st.xruns, 2);
		CHECK_UINT(st.read_errors, 2);
		CHECK_UINT(st.recover_failures, 0);
		CHECK_UINT(st.delay_stalls, 0);
		CHECK(engine_stats_stalls(&st) >= 2);
		CHECK(!st.stopped_fatal);
		CHECK_UINT((uint64_t)st.last_errno, (uint64_t)EPIPE);
		struct stat sb;
		CHECK(stat(play, &sb) == 0);
		CHECK_UINT((uint64_t)sb.st_size,
			   (uint64_t)((T_PREFILL * (1 + st.xruns) + st.ticks) * T_NBYTE));
		unlink(play);
		unlink(cap);
	}

	/*
	 * chan.c falls back to the nominal prefill reference after three
	 * calibration restarts, so the engine has to count them and the count
	 * has to be monotonic from engine start: a window that never completed
	 * once still has to reach the fallback, and an edge count cannot see
	 * one.
	 */
	CASE("every recovery counts a calibration restart");
	{
		char play[64], cap[64];
		tmp_path(play);
		tmp_path(cap);
		capture_fixture(cap, 3);
		struct udsp_config cfg;
		cfg_init(&cfg, play, cap);
		struct engine_opts opts = { .loopback = true, .max_ticks = 8 };
		struct engine_stats st;

		CHECK(engine_run(&cfg, &opts, &st));
		CHECK_UINT(st.xruns, 2);
		/* One per recovery, and the calibration never completed, so the
		 * count stands even with no calibrated period to leave. */
		CHECK_UINT(st.cal_restarts, 2);
		CHECK(!st.delay_calibrated);
		unlink(play);
		unlink(cap);
	}

	CASE("a run without a fault restarts no calibration");
	{
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 20 };
		struct engine_stats st;

		CHECK(engine_run(&cfg, &opts, &st));
		CHECK_UINT(st.cal_restarts, 0);
		CHECK(st.delay_calibrated);
	}

	CASE("steady state keeps the playback delay at the prefill");
	{
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 20 };
		struct engine_stats st;

		CHECK(engine_run(&cfg, &opts, &st));
		CHECK_UINT(st.delay_samples, 20);
		CHECK(st.delay_dev_min == 0);
		CHECK(st.delay_dev_max == 0);
		CHECK(st.delay_dev_last == 0);
		CHECK_UINT(st.delay_stalls, 0);
		CHECK_UINT(st.delay_errors, 0);
		CHECK_UINT(st.interval_samples, 19);
	}

	CASE("stats snapshot stays coherent while the loop runs");
	{
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		/* Bounded, and with no playback path: the reader watches a
		 * loop that writes nothing to disk. */
		struct engine_opts opts = { .loopback = true, .max_ticks = 50000 };
		struct engine *e = engine_open(&cfg, &opts, NULL);

		CHECK(e != NULL);
		if (e) {
			uint64_t last = 0;
			bool ok = snapshot_watch(e, 200, &last);
			struct engine_stats fin;

			engine_stop(e, &fin);
			CHECK(ok);
			CHECK(last > 0);
			CHECK(fin.ticks >= last);
			CHECK(stats_exact(&fin));
			CHECK(!fin.running);
			engine_close(e);
		}
	}

	CASE("rt reports the SCHED_FIFO result coherently");
	{
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		cfg.engine_prio = 50;
		struct engine_opts opts = { .loopback = false, .max_ticks = 20 };
		struct engine_stats st;

		CHECK(engine_run(&cfg, &opts, &st));
		CHECK(rt_report_coherent(&st.rt));
		CHECK(st.rt.sched_applied || st.rt.sched_errno != 0);
		/* pthread_setschedparam returns the error as its result and
		 * never touches errno, and only these three can come back
		 * here: EPERM with no privilege, ENOSYS from a libc stub,
		 * EINVAL for a bad policy or priority. A stale errno would
		 * show up as anything else, ENOMEM from the mlockall above
		 * most likely. */
		CHECK(st.rt.sched_applied || st.rt.sched_errno == EPERM ||
		      st.rt.sched_errno == ENOSYS || st.rt.sched_errno == EINVAL);
		CHECK_UINT((uint64_t)st.rt.prio, 50);
		CHECK(st.rt.sched_applied ? st.rt.policy == SCHED_FIFO :
		      st.rt.policy == SCHED_OTHER);
		CHECK(st.rt.cpu == -1);
		CHECK(!st.rt.affinity_applied);
		CHECK_UINT(st.ticks, 20);
	}

	CASE("rt asks for nothing at priority zero");
	{
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = false, .max_ticks = 5 };
		struct engine_stats st;

		CHECK(engine_run(&cfg, &opts, &st));
		CHECK(rt_report_coherent(&st.rt));
		CHECK(!st.rt.sched_applied);
		CHECK_UINT((uint64_t)st.rt.sched_errno, 0);
		CHECK(!st.rt.mlock_applied);
		CHECK(st.rt.policy == SCHED_OTHER);
	}

	CASE("engine refuses an unbounded file back end run");
	{
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 0 };

		CHECK(engine_open(&cfg, &opts, NULL) == NULL);
	}

	CASE("engine refuses an unknown tdm backend");
	{
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		snprintf(cfg.tdm_backend, sizeof(cfg.tdm_backend), "nope");
		struct engine_opts opts = { .loopback = false, .max_ticks = 1 };

		CHECK(engine_open(&cfg, &opts, NULL) == NULL);
		CHECK(engine_open_ops(NULL, &cfg, &opts, NULL) == NULL);
	}

	CASE("real time setup finishes before the streams start");
	{
		t_reset();
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 5 };
		struct engine_stats st;

		CHECK(engine_run_ops(&t_ops, &cfg, &opts, &st));
		CHECK(st.rt_done_us > 0);
		CHECK(t_start_us > 0);
		CHECK(st.rt_done_us <= t_start_us);
		CHECK(strncmp(t_journal, "owwws", 5) == 0);
		CHECK(t_journal[5] == 'R');
		CHECK_UINT(st.ticks, 5);
	}

	CASE("prefill writes silence before start and after recover");
	{
		t_reset();
		t_script.read_fail_from = 4;
		t_script.read_fail_count = 1;
		t_script.read_fail_err = -EPIPE;
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 6 };
		struct engine_stats st;

		CHECK(engine_run_ops(&t_ops, &cfg, &opts, &st));
		CHECK(strncmp(t_journal, "owwws", 5) == 0);
		CHECK(strstr(t_journal, "erwwws") != NULL);
		CHECK_UINT(st.xruns, 1);
		CHECK_UINT(st.ticks, 6);
		CHECK(!st.stopped_fatal);
	}

	CASE("a transient write fault retries the same period");
	{
		t_reset();
		t_script.write_fail_at = T_PREFILL + 1;
		t_script.write_fail_err = -EAGAIN;
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 3 };
		struct engine_stats st;

		CHECK(engine_run_ops(&t_ops, &cfg, &opts, &st));
		CHECK(strstr(t_journal, "RxW") != NULL);
		CHECK_UINT(st.again, 1);
		CHECK_UINT(st.write_errors, 0);
		CHECK_UINT(st.read_errors, 0);
		CHECK_UINT(st.ticks, 3);
		CHECK_UINT(t_reads, 3);
		CHECK(t_write_first[T_PREFILL] == t_write_first[T_PREFILL + 1]);
		CHECK(t_write_first[T_PREFILL] != 0);
	}

	CASE("ten consecutive faults stop the loop");
	{
		t_reset();
		t_script.read_fail_from = 1;
		t_script.read_fail_count = 10;
		t_script.read_fail_err = -EIO;
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 100 };
		struct engine_stats st;

		CHECK(engine_run_ops(&t_ops, &cfg, &opts, &st));
		CHECK(st.stopped_fatal);
		CHECK_UINT(st.ticks, 0);
		CHECK_UINT(st.read_errors, 10);
		CHECK_UINT(t_reads, 10);
		CHECK_UINT((uint64_t)st.last_errno, (uint64_t)EIO);
	}

	CASE("nine consecutive faults are survivable");
	{
		t_reset();
		t_script.read_fail_from = 1;
		t_script.read_fail_count = 9;
		t_script.read_fail_err = -EIO;
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 2 };
		struct engine_stats st;

		CHECK(engine_run_ops(&t_ops, &cfg, &opts, &st));
		CHECK(!st.stopped_fatal);
		CHECK_UINT(st.ticks, 2);
		CHECK_UINT(st.read_errors, 9);
	}

	CASE("a delay fault feeds the strike run");
	{
		t_reset();
		t_script.delay_fail_from = 1;
		t_script.delay_fail_count = 10;
		t_script.delay_fail_err = -EIO;
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 100 };
		struct engine_stats st;

		CHECK(engine_run_ops(&t_ops, &cfg, &opts, &st));
		CHECK(st.stopped_fatal);
		CHECK_UINT(st.delay_errors, 10);
		CHECK_UINT(st.ticks, 10);
		CHECK_UINT(st.delay_samples, 0);
		CHECK(stats_coherent(&st));
	}

	CASE("a failed start releases the parked worker");
	{
		t_reset();
		t_script.start_err = -EIO;
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 5 };

		/* The worker is already created and parked on the gate when the
		 * start fails, so the open path has to release and join it. */
		CHECK(engine_open_ops(&t_ops, &cfg, &opts, NULL) == NULL);
		CHECK(strncmp(t_journal, "owwws", 5) == 0);
		CHECK(strchr(t_journal, 'R') == NULL);
		CHECK(t_journal[t_journal_len - 1] == 'c');
	}

	CASE("a clamped prefill is reported, not hidden");
	{
		t_reset();
		t_script.grant_periods = 4;
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		cfg.prefill_periods = 5;
		struct engine_opts opts = { .loopback = true, .max_ticks = 4 };
		struct engine_stats st;

		CHECK(engine_run_ops(&t_ops, &cfg, &opts, &st));
		CHECK_UINT(st.prefill_requested, 5);
		CHECK_UINT(st.prefill_periods, 3);
		CHECK_UINT(st.granted_periods, 4);
		CHECK(strncmp(t_journal, "owwws", 5) == 0);
		CHECK(t_journal[5] == 'R');
	}

	/*
	 * The tick stays 10 ms at every rate. The period follows the line
	 * rate, so a 16 kHz line moves 160 frames per tick and the period is
	 * still 10000 us. The byte oracle on disk proves the engine moved the
	 * wider period, not a short one.
	 */
	CASE("a 16 kHz line runs 160-frame periods at the same 10 ms tick");
	{
		char play[64];
		tmp_path(play);
		struct udsp_config cfg;
		cfg_init(&cfg, play, NULL);
		cfg.sample_rate = 16000;
		struct engine_opts opts = { .loopback = false, .max_ticks = 25 };
		struct engine_stats st;

		CHECK(engine_run(&cfg, &opts, &st));
		CHECK_UINT(st.ticks, 25);
		CHECK_UINT(st.frames_per_period, 160);
		CHECK_UINT(st.rate, 16000);
		CHECK_UINT(st.period_us, 10000);
		CHECK_UINT(st.channels, T_CHANS);
		struct stat sb;
		CHECK(stat(play, &sb) == 0);
		CHECK_UINT((uint64_t)sb.st_size,
			   (uint64_t)((T_PREFILL + 25) * 160u * T_CHANS *
				      sizeof(int16_t)));
		unlink(play);
	}

	CASE("an 8 kHz line reports its rate and tick too");
	{
		char play[64];
		tmp_path(play);
		struct udsp_config cfg;
		cfg_init(&cfg, play, NULL);
		struct engine_opts opts = { .loopback = false, .max_ticks = 4 };
		struct engine_stats st;

		/* cfg.sample_rate is left at zero, which is 8000 (tdm.h
		 * Geometry). */
		CHECK(engine_run(&cfg, &opts, &st));
		CHECK_UINT(st.rate, 8000);
		CHECK_UINT(st.period_us, 10000);
		CHECK_UINT(st.frames_per_period, T_FRAMES);
		unlink(play);
	}

	/*
	 * Every consumer downstream sizes its frame from the geometry the
	 * engine reports, so a back end that granted something else must not
	 * run at all. Both directions of the mismatch are pinned: the period
	 * and the rate.
	 */
	CASE("the engine refuses a granted period other than the request");
	{
		t_reset();
		t_script.paced = true;
		t_script.grant_frames = 64;
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 4 };

		CHECK(engine_open_ops(&t_ops, &cfg, &opts, NULL) == NULL);
	}

	CASE("the engine refuses a granted rate other than the request");
	{
		t_reset();
		t_script.paced = true;
		t_script.grant_rate = 16000;
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 4 };

		CHECK(engine_open_ops(&t_ops, &cfg, &opts, NULL) == NULL);

		/* The reverse ask, refused the same way. */
		t_reset();
		t_script.paced = true;
		t_script.grant_rate = 8000;
		cfg.sample_rate = 16000;
		CHECK(engine_open_ops(&t_ops, &cfg, &opts, NULL) == NULL);

	}

	/*
	 * A rate of zero means 8000 on both sides of the contract (tdm.h
	 * Geometry), so a back end that leaves the field untouched still
	 * conforms. The script grants the literal zero here, which is what an
	 * unconverted back end returns. The engine must normalize it against
	 * an 8 kHz ask and refuse it against a 16 kHz one.
	 */
	CASE("a granted rate of zero is the narrowband rate, not a wildcard");
	{
		struct udsp_config cfg;
		struct engine_opts opts = { .loopback = true, .max_ticks = 4 };
		struct engine_stats st;
		struct engine *e;

		t_reset();
		t_script.paced = true;
		t_script.grant_rate_zero = true;
		cfg_init(&cfg, NULL, NULL);
		cfg.sample_rate = 8000;
		e = engine_open_ops(&t_ops, &cfg, &opts, NULL);
		CHECK(e != NULL);
		if (e) {
			engine_stop(e, &st);
			CHECK_UINT(st.rate, 8000);
			CHECK_UINT(st.period_us, 10000);
			engine_close(e);
		}

		t_reset();
		t_script.paced = true;
		t_script.grant_rate_zero = true;
		cfg.sample_rate = 16000;
		CHECK(engine_open_ops(&t_ops, &cfg, &opts, NULL) == NULL);
	}

	/*
	 * Which half refused. Only a geometry answer may move the line rate,
	 * so the open separates the back end's verdict on the rate and the
	 * period from every other cause (engine.h Why an open failed). A
	 * failing start is the other arm: it fails the same way at either
	 * rate.
	 */
	CASE("the open reports a geometry refusal apart from any other cause");
	{
		struct udsp_config cfg;
		struct engine_opts opts = { .loopback = true, .max_ticks = 4 };
		struct engine_open_verdict why = { 0 };
		struct engine *e;

		t_reset();
		t_script.paced = true;
		t_script.grant_rate = 8000;
		cfg_init(&cfg, NULL, NULL);
		cfg.sample_rate = 16000;
		CHECK(engine_open_ops(&t_ops, &cfg, &opts, &why) == NULL);
		CHECK_UINT(why.cause, ENGINE_OPEN_GEOM);
		/* The grant, not the back end, refused: no record. */
		CHECK_UINT(why.tdm.err, 0);

		t_reset();
		t_script.paced = true;
		t_script.grant_frames = 64;
		why.cause = ENGINE_OPEN_OK;
		CHECK(engine_open_ops(&t_ops, &cfg, &opts, &why) == NULL);
		CHECK_UINT(why.cause, ENGINE_OPEN_GEOM);

		/* A device that will not open at this rate is the back end's
		 * answer about the geometry only when its record says so
		 * (tdm.h Refusal), and the record travels out with the
		 * verdict. */
		t_reset();
		t_script.paced = true;
		t_script.open_null = true;
		t_script.open_fail_geometry = true;
		why.cause = ENGINE_OPEN_OK;
		CHECK(engine_open_ops(&t_ops, &cfg, &opts, &why) == NULL);
		CHECK_UINT(why.cause, ENGINE_OPEN_GEOM);
		CHECK(why.tdm.geometry);
		CHECK(why.tdm.err == -EINVAL);
		CHECK(why.tdm.stream == UDSP_TDM_DIR_PLAYBACK);
		CHECK(why.tdm.what && !strcmp(why.tdm.what, "rate"));

		/* A refusal without that mark, here a node another process
		 * holds, fails the same way at 8000, so it is other. */
		t_reset();
		t_script.paced = true;
		t_script.open_null = true;
		why.cause = ENGINE_OPEN_OK;
		CHECK(engine_open_ops(&t_ops, &cfg, &opts, &why) == NULL);
		CHECK_UINT(why.cause, ENGINE_OPEN_OTHER);
		CHECK(!why.tdm.geometry);
		CHECK(why.tdm.err == -EBUSY);
		CHECK(why.tdm.what && !strcmp(why.tdm.what, "open"));

		/* The record of one open never outlives it. */
		t_reset();
		t_script.paced = true;
		t_script.start_err = -EIO;
		CHECK(engine_open_ops(&t_ops, &cfg, &opts, &why) == NULL);
		CHECK_UINT(why.cause, ENGINE_OPEN_OTHER);
		CHECK_UINT(why.tdm.err, 0);

		/* The unpaced bound is a policy refusal, not the back end's
		 * answer about the geometry. */
		t_reset();
		t_script.paced = false;
		why.cause = ENGINE_OPEN_OK;
		struct engine_opts unbounded = { .loopback = true };

		CHECK(engine_open_ops(&t_ops, &cfg, &unbounded, &why) == NULL);
		CHECK_UINT(why.cause, ENGINE_OPEN_OTHER);

		/* A successful open reports OK. */
		t_reset();
		t_script.paced = true;
		why.cause = ENGINE_OPEN_OTHER;
		e = engine_open_ops(&t_ops, &cfg, &opts, &why);
		CHECK(e != NULL);
		CHECK_UINT(why.cause, ENGINE_OPEN_OK);
		if (e)
			engine_close(e);
	}

	CASE("the tdm refusal formats as one clause and an empty string");
	{
		struct udsp_tdm_fail f = { 0 };
		char buf[64];

		engine_tdm_fail_text(&f, buf, sizeof(buf));
		CHECK(!strcmp(buf, ""));

		f.err = -ENOENT;
		f.stream = UDSP_TDM_DIR_CAPTURE;
		f.what = "open";
		engine_tdm_fail_text(&f, buf, sizeof(buf));
		CHECK(!strcmp(buf, "capture open: No such file or directory"));

		f.err = -EINVAL;
		f.stream = UDSP_TDM_DIR_NONE;
		f.what = "link";
		engine_tdm_fail_text(&f, buf, sizeof(buf));
		CHECK(!strcmp(buf, "link: Invalid argument"));

		/* A short buffer truncates and stays terminated. */
		f.stream = UDSP_TDM_DIR_PLAYBACK;
		engine_tdm_fail_text(&f, buf, 8);
		CHECK(!strcmp(buf, "playbac"));
	}

	CASE("the engine refuses a rate its period cannot carry");
	{
		char play[64];
		tmp_path(play);
		struct udsp_config cfg;
		cfg_init(&cfg, play, NULL);
		struct engine_opts opts = { .loopback = false, .max_ticks = 4 };

		/* 48 kHz would need 480 frames, past ENGINE_FRAMES_MAX. */
		cfg.sample_rate = 48000;
		CHECK(engine_open(&cfg, &opts, NULL) == NULL);
		unlink(play);
	}

	CASE("a constant delay bias does not stall");
	{
		t_reset();
		t_script.delay_bias = -208;
		/* One outlier inside the calibration window: a median absorbs
		 * it, a mean would move the reference by 2000/16 frames and a
		 * first-sample latch would depend on which tick it fell on. */
		t_script.delay_step_from = 3;
		t_script.delay_step_count = 1;
		t_script.delay_step = 2000;
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 40 };
		struct engine_stats st;

		CHECK(engine_run_ops(&t_ops, &cfg, &opts, &st));
		CHECK(st.delay_calibrated);
		CHECK(st.delay_ref == (int32_t)(T_PREFILL * T_FRAMES) - 208);
		/* No capture backlog: the echo loop is the playback queue. */
		CHECK(st.loop_ref == st.delay_ref);
		CHECK(st.delay_dev_last == -208);
		CHECK_UINT(st.delay_stalls, 0);
		CHECK_UINT(st.delay_drift_samples, 40 - ENGINE_DELAY_CAL);
		CHECK(st.delay_drift_min == 0);
		CHECK(st.delay_drift_max == 0);
	}

	CASE("the capture backlog joins the calibrated echo loop");
	{
		t_reset();
		t_script.delay_bias = -208;
		/* The field shape: the playback queue and the capture backlog
		 * are two halves of one ring, and the canceller aligns to
		 * their sum. The playback half alone puts the reference 160
		 * samples early on MT7987 (engine.h loop_ref). */
		t_script.avail_value = 78;
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 40 };
		struct engine_stats st;

		CHECK(engine_run_ops(&t_ops_avail, &cfg, &opts, &st));
		CHECK(st.delay_calibrated);
		CHECK(st.delay_ref == (int32_t)(T_PREFILL * T_FRAMES) - 208);
		CHECK(st.loop_ref == st.delay_ref + 78);
		/* The stall detector keeps the playback-only reference: a
		 * backlog swing is the tick phase, not a driver drift. */
		CHECK_UINT(st.delay_stalls, 0);
		CHECK_UINT(st.capture_stalls, 0);
	}

	CASE("a drift beyond one period stalls");
	{
		t_reset();
		t_script.delay_bias = -208;
		t_script.delay_step_from = 20;
		t_script.delay_step_count = 5;
		t_script.delay_step = 160;
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 40 };
		struct engine_stats st;

		CHECK(engine_run_ops(&t_ops, &cfg, &opts, &st));
		CHECK(st.delay_calibrated);
		CHECK(st.delay_ref == (int32_t)(T_PREFILL * T_FRAMES) - 208);
		CHECK_UINT(st.delay_stalls, 5);
		CHECK(st.delay_drift_max == 160);
		CHECK(st.delay_drift_min == 0);
		CHECK(st.delay_drift_last == 0);
	}

	/* snd_pcm_delay on MT7987 quantizes to period boundaries, so the
	 * drift detector reads exactly +80 about once a second at idle while
	 * xruns stay 0 and capture_stalls stay stable. A one-period wobble is
	 * the reading's granularity, not loss. The stall counter allows one
	 * period; the raw drift record keeps the wobble on purpose. */
	CASE("a one-period wobble is not a stall");
	{
		t_reset();
		t_script.delay_bias = -208;
		t_script.delay_step_from = 20;
		t_script.delay_step_count = 5;
		t_script.delay_step = T_FRAMES;
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 40 };
		struct engine_stats st;

		CHECK(engine_run_ops(&t_ops, &cfg, &opts, &st));
		CHECK(st.delay_calibrated);
		CHECK_UINT(st.delay_stalls, 0);
		CHECK(st.delay_drift_max == (int32_t)T_FRAMES);
		CHECK(st.delay_drift_min == 0);
		CHECK_UINT(st.delay_drift_samples, 40 - ENGINE_DELAY_CAL);

		/* No bias on the negative arm: under the -208 bias the
		 * reference is 32 frames, and a -80 step would return a
		 * negative queued count, which engine_tick reads as a fault
		 * errno, not as a delay sample. */
		t_reset();
		t_script.delay_step_from = 20;
		t_script.delay_step_count = 5;
		t_script.delay_step = -(long)T_FRAMES;
		CHECK(engine_run_ops(&t_ops, &cfg, &opts, &st));
		CHECK(st.delay_calibrated);
		CHECK_UINT(st.delay_stalls, 0);
		CHECK(st.delay_drift_min == -(int32_t)T_FRAMES);
		CHECK(st.delay_drift_max == 0);
		CHECK_UINT(st.delay_drift_samples, 40 - ENGINE_DELAY_CAL);
	}

	/* One frame past the allowance stalls, so the allowance is pinned at
	 * one period and no more: a later widening to two periods or to a
	 * percentage fails here first. */
	CASE("one frame beyond the allowance stalls");
	{
		t_reset();
		t_script.delay_bias = -208;
		t_script.delay_step_from = 20;
		t_script.delay_step_count = 5;
		t_script.delay_step = T_FRAMES + 1;
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 40 };
		struct engine_stats st;

		CHECK(engine_run_ops(&t_ops, &cfg, &opts, &st));
		CHECK(st.delay_calibrated);
		CHECK_UINT(st.delay_stalls, 5);
		CHECK(st.delay_drift_max == (int32_t)(T_FRAMES + 1));
		CHECK(st.delay_drift_min == 0);
	}

	CASE("the final snapshot survives a reader overlapping the stop");
	{
		bool ok = true;

		for (unsigned i = 0; i < 20; i++)
			ok &= final_snapshot_round();

		CHECK(ok);
	}

	CASE("a capture backlog above one period is a stall");
	{
		t_reset();
		t_script.avail_value = T_FRAMES * 2;
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 10 };
		struct engine_stats st;

		CHECK(engine_run_ops(&t_ops_avail, &cfg, &opts, &st));
		CHECK_UINT(st.ticks, 10);
		CHECK_UINT(st.capture_stalls, 10);
		CHECK_UINT(st.avail_errors, 0);
		CHECK(strchr(t_journal, 'a') != NULL);
	}

	CASE("a backlog of one period or less is not a stall");
	{
		t_reset();
		t_script.avail_value = T_FRAMES;
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 10 };
		struct engine_stats st;

		CHECK(engine_run_ops(&t_ops_avail, &cfg, &opts, &st));
		CHECK_UINT(st.capture_stalls, 0);
		CHECK_UINT(st.interval_samples, 9);
	}

	CASE("an avail fault feeds the strike run");
	{
		t_reset();
		t_script.avail_value = T_FRAMES;
		t_script.avail_err = -EIO;
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 4 };
		struct engine_stats st;

		CHECK(engine_run_ops(&t_ops_avail, &cfg, &opts, &st));
		CHECK_UINT(st.avail_errors, 1);
		CHECK_UINT((uint64_t)st.last_errno, (uint64_t)EIO);
		CHECK(strstr(t_journal, "Ar") != NULL);
		CHECK(!st.stopped_fatal);
	}

	CASE("the paced flag alone decides an unbounded run");
	{
		t_reset();
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 0 };

		/* got.paced false: an unpaced backend ticks at CPU speed and
		 * an unbounded run fills the disk. */
		CHECK(engine_open_ops(&t_ops, &cfg, &opts, NULL) == NULL);

		t_reset();
		t_script.paced = true;
		struct engine *e = engine_open_ops(&t_ops, &cfg, &opts, NULL);

		CHECK(e != NULL);
		if (e) {
			struct engine_stats st;

			msleep(2);
			engine_stop(e, &st);
			CHECK(st.paced);
			CHECK(st.ticks > 0);
			engine_close(e);
		}
	}

	CASE("a held gate keeps the loop parked until it opens");
	{
		t_reset();
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 5,
					    .hold_gate = true };
		struct engine *e = engine_open_ops(&t_ops, &cfg, &opts, NULL);

		CHECK(e != NULL);
		if (e) {
			struct engine_stats st;

			msleep(20);
			engine_stats_get(e, &st);
			CHECK_UINT(st.ticks, 0);
			CHECK(!st.running);
			CHECK(strchr(t_journal, 'R') == NULL);
			engine_gate_open(e);
			CHECK(engine_wait(e, 5000));
			engine_stop(e, &st);
			CHECK_UINT(st.ticks, 5);
			engine_close(e);
		}
	}

	CASE("the tick hook sees the captured period and rewrites playback");
	{
		t_reset();
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = false, .max_ticks = 4 };
		struct engine_stats st;

		hook_reset();
		opts.hooks.ctx = &hook_ctx;
		opts.hooks.tick = hook_tick;
		CHECK(engine_run_ops(&t_ops, &cfg, &opts, &st));
		CHECK_UINT(hook_ctx.calls, 4);
		CHECK_UINT(hook_ctx.channels, T_CHANS);
		CHECK_UINT(hook_ctx.frames, T_FRAMES);
		CHECK_UINT(hook_ctx.first_tick, 0);
		CHECK_UINT(hook_ctx.last_tick, 3);
		CHECK(hook_ctx.rx_seen != 0);
		CHECK(hook_ctx.tx_was_silent);
		/* The hook owns tx: every non-prefill period carries its mark. */
		for (unsigned i = 0; i < 4; i++)
			CHECK(t_write_first[T_PREFILL + i] == (int16_t)0x5aa5);
	}

	CASE("the engine clock publishes the tick it is on");
	{
		t_reset();
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 6 };
		struct engine *e = engine_open_ops(&t_ops, &cfg, &opts, NULL);

		CHECK(e != NULL);
		if (e) {
			uint64_t tick = 0, mono = 0;
			struct engine_stats st;

			CHECK(engine_wait(e, 5000));
			engine_clock_get(e, &tick, &mono);
			CHECK_UINT(tick, 5);
			CHECK(mono > 0);
			CHECK_UINT(engine_now_ms(e), 50);
			engine_stop(e, &st);
			engine_close(e);
		}
	}

	CASE("the file backend reports its own capture backlog");
	{
		struct udsp_tdm_geom want = {
			.frames_per_period = T_FRAMES,
			.channels = T_CHANS,
			.prefill_periods = T_PREFILL + 1,
			.layout = UDSP_TDM_INTERLEAVED,
		};
		struct udsp_tdm_geom got = { 0 };
		char cap[64];
		int16_t buf[T_NSAMP];

		tmp_path(cap);
		capture_fixture(cap, 2);

		struct udsp_tdm *t = udsp_tdm_file.open(NULL, cap, &want, &got,
							NULL);

		CHECK(t != NULL);
		if (t) {
			CHECK(udsp_tdm_file.avail != NULL);
			CHECK(udsp_tdm_file.start(t) == 0);
			/* Two periods on disk: a full period is ready, then one
			 * more, then the source is empty. */
			CHECK(udsp_tdm_file.avail(t) == (long)T_FRAMES);
			CHECK(udsp_tdm_file.read_period(t, buf) == 0);
			CHECK(udsp_tdm_file.avail(t) == (long)T_FRAMES);
			CHECK(udsp_tdm_file.read_period(t, buf) == 0);
			CHECK(udsp_tdm_file.avail(t) == 0);
			CHECK(udsp_tdm_file.read_period(t, buf) == -EPIPE);
			/* recover rewinds, so the source is full again. */
			CHECK(udsp_tdm_file.recover(t) == 0);
			CHECK(udsp_tdm_file.avail(t) == (long)T_FRAMES);
			udsp_tdm_file.close(t);
		}
		unlink(cap);
	}

	CASE("the synthetic capture source never runs dry");
	{
		struct udsp_tdm_geom want = {
			.frames_per_period = T_FRAMES,
			.channels = T_CHANS,
			.prefill_periods = T_PREFILL + 1,
			.layout = UDSP_TDM_INTERLEAVED,
		};
		struct udsp_tdm_geom got = { 0 };
		int16_t buf[T_NSAMP];
		struct udsp_tdm *t = udsp_tdm_file.open(NULL, NULL, &want, &got,
							NULL);

		CHECK(t != NULL);
		if (t) {
			CHECK(!got.paced);
			CHECK(udsp_tdm_file.start(t) == 0);
			for (unsigned i = 0; i < 5; i++) {
				CHECK(udsp_tdm_file.avail(t) == (long)T_FRAMES);
				CHECK(udsp_tdm_file.read_period(t, buf) == 0);
			}
			udsp_tdm_file.close(t);
		}
	}

	CASE("a paced backend drops the environment tick bound");
	{
		t_reset();
		t_script.paced = true;
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 3,
					    .max_ticks_unpaced_only = true };
		struct engine *e = engine_open_ops(&t_ops, &cfg, &opts, NULL);

		CHECK(e != NULL);
		if (e) {
			struct engine_stats st;

			msleep(5);
			engine_stats_get(e, &st);
			/* max_ticks_unpaced_only scopes the tick bound to
			 * unpaced backends, so a caller with its own bound
			 * leaves the flag clear. */
			CHECK(st.ticks > 3);
			engine_stop(e, &st);
			CHECK(st.running == false);
			engine_close(e);
		}

		t_reset();
		struct engine_opts keep = { .loopback = true, .max_ticks = 3 };

		t_script.paced = true;
		CHECK(engine_run_ops(&t_ops, &cfg, &keep, &(struct engine_stats){ 0 }));
		CHECK_UINT(t_reads, 3);
	}

	/*
	 * chan.c detects a replacement engine by the stats serial:
	 * engine_close frees and engine_open_ops callocs the same size, so a
	 * recycled allocation at the old address dodges a pointer compare. The
	 * identity has to be a value, monotonic across instances and never 0
	 * for a live engine (engine.h serial). The first snapshot already
	 * carries it, so a consumer that reads before the first tick sees the
	 * same identity as the final counters.
	 */
	CASE("the engine serial grows across instances");
	{
		t_reset();
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 5 };
		struct engine_stats snap = { 0 }, first = { 0 }, second = { 0 };
		struct engine *e = engine_open_ops(&t_ops, &cfg, &opts, NULL);

		CHECK(e != NULL);
		if (e) {
			engine_stats_get(e, &snap);
			CHECK(engine_wait(e, 5000));
			engine_stop(e, &first);
			engine_close(e);
		}
		CHECK(first.serial != 0);
		CHECK_UINT(snap.serial, first.serial);

		t_reset();
		CHECK(engine_run_ops(&t_ops, &cfg, &opts, &second));
		CHECK(second.serial != 0);
		CHECK(second.serial > first.serial);
	}

	/*
	 * The identity must reach the first published snapshot, not only a
	 * later one: the held gate on the unpaced backend keeps the loop from
	 * ever ticking, so a reader that opens and asks at once sees only the
	 * pre-tick publishes, and a stamp that rides the tick side would
	 * still read 0 here.
	 */
	CASE("a held-gate engine already reports its serial");
	{
		t_reset();
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 5,
					    .hold_gate = true };
		struct engine *e = engine_open_ops(&t_ops, &cfg, &opts, NULL);

		CHECK(e != NULL);
		if (e) {
			struct engine_stats st;

			engine_stats_get(e, &st);
			CHECK(st.serial != 0);
			CHECK_UINT(st.ticks, 0);
			engine_close(e);
		}
	}

	/*
	 * The catch-up rule. The numbers below are the model's arithmetic, not
	 * a tolerance: at rest the read waits for its period and the write
	 * finds room, so a tick spends exactly one period of hardware time.
	 * A hold of sixteen periods on a fifteen period capture ring loses one
	 * period to the ring. The read of the hold tick and the read of the
	 * next tick spend two more, while their writes refill the playback
	 * queue towards its prefill depth. The tick whose write brings the
	 * queue back to that depth trims the twelve periods still waiting, and
	 * the tick after it leaves
	 * the episode.
	 */
	CASE("a hold trims the capture backlog once and leaves the episode");
	{
		t_reset();
		hook_reset();
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 60 };
		struct engine_stats st;

		t_script.hold_at[0] = 5;
		t_script.hold_periods[0] = 16;
		opts.hooks.ctx = &hook_ctx;
		opts.hooks.tick = hook_tick;
		opts.hooks.skip = hook_skip;
		CHECK(engine_run_ops(&t_ops_hold, &cfg, &opts, &st));
		CHECK_UINT(st.xruns, 0);
		CHECK_UINT(st.holds, 1);
		CHECK_UINT(st.capture_stalls, 1);
		CHECK_UINT(st.delay_stalls, 0);
		CHECK_UINT(st.capture_drops, 1);
		CHECK_UINT(st.trim_drops, 12);
		CHECK_UINT(st.playback_expired, 0);
		CHECK_UINT(st.cal_restarts, 1);
		CHECK_UINT(t_skips, 1);
		CHECK_UINT((uint64_t)t_skip_frames, 12u * T_FRAMES);
		/* Both directions are back at the depth of a clean start: the
		 * read waits for its period again, and the playback queue after
		 * the write stands at the prefill, not at the granted ring one
		 * period deeper. */
		CHECK_UINT((uint64_t)t_backlog, 0);
		CHECK_UINT((uint64_t)t_queued, (uint64_t)T_PREFILL * T_FRAMES);
		CHECK(t_queued < t_play_ring());
	}

	CASE("the tick clock counts the periods the engine was away for");
	{
		t_reset();
		hook_reset();
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 60 };
		struct engine_stats st;

		t_script.hold_at[0] = 5;
		t_script.hold_periods[0] = 16;
		opts.hooks.ctx = &hook_ctx;
		opts.hooks.tick = hook_tick;
		opts.hooks.skip = hook_skip;
		CHECK(engine_run_ops(&t_ops_hold, &cfg, &opts, &st));
		/* Every period of hardware time is either a tick that ran or a
		 * period the engine skipped, so the clock never falls behind
		 * the ring. */
		CHECK_UINT(st.ticks, (uint64_t)(t_writes - T_PREFILL) +
				     st.capture_drops + st.trim_drops);
		/* One call for the period the ring overwrote, one for the
		 * trim, both before the tick hook of their own tick. */
		CHECK_UINT(skip_ctx.calls, 2);
		CHECK_UINT(skip_ctx.periods_first, 1);
		CHECK_UINT(skip_ctx.periods_total, 13);
		CHECK_UINT(skip_ctx.tick_calls, 4);
		CHECK_UINT(skip_ctx.tick_first, 5);
		CHECK(skip_ctx.tick_last > skip_ctx.tick_first);
		CHECK_UINT(hook_ctx.last_tick, st.ticks - 1);
	}

	CASE("two holds add their counters and leave no backlog behind");
	{
		t_reset();
		hook_reset();
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true,
					    .max_ticks = 120 };
		struct engine_stats st;

		t_script.hold_at[0] = 5;
		t_script.hold_periods[0] = 16;
		t_script.hold_at[1] = 40;
		t_script.hold_periods[1] = 16;
		opts.hooks.ctx = &hook_ctx;
		opts.hooks.tick = hook_tick;
		opts.hooks.skip = hook_skip;
		CHECK(engine_run_ops(&t_ops_hold, &cfg, &opts, &st));
		CHECK_UINT(st.holds, 2);
		CHECK_UINT(st.capture_stalls, 2);
		CHECK_UINT(st.cal_restarts, 2);
		CHECK_UINT(st.trim_drops, 24);
		/* The first hold leaves no backlog, so the second one starts
		 * from rest and loses the same one period to the overwrite. */
		CHECK_UINT(st.capture_drops, 2);
		CHECK_UINT(st.xruns, 0);
		CHECK_UINT((uint64_t)t_backlog, 0);
		CHECK_UINT((uint64_t)t_queued, (uint64_t)T_PREFILL * T_FRAMES);
	}

	CASE("a short hold trims the one period it leaves");
	{
		t_reset();
		hook_reset();
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 40 };
		struct engine_stats st;

		t_script.hold_at[0] = 5;
		t_script.hold_periods[0] = 6;
		opts.hooks.ctx = &hook_ctx;
		opts.hooks.tick = hook_tick;
		opts.hooks.skip = hook_skip;
		CHECK(engine_run_ops(&t_ops_hold, &cfg, &opts, &st));
		CHECK_UINT(st.holds, 1);
		CHECK_UINT(st.capture_stalls, 1);
		CHECK_UINT(st.capture_drops, 0);
		/* The reads of the hold tick and the two refill ticks spend
		 * three of the six periods, so three stand when the playback
		 * queue is back at its prefill depth, and the trim takes
		 * them. */
		CHECK_UINT(st.trim_drops, 3);
		CHECK_UINT(st.cal_restarts, 1);
		CHECK_UINT(st.xruns, 0);
		CHECK_UINT(skip_ctx.calls, 1);
		CHECK_UINT(skip_ctx.periods_total, 3);
		CHECK_UINT((uint64_t)t_backlog, 0);
		CHECK_UINT((uint64_t)t_queued, (uint64_t)T_PREFILL * T_FRAMES);
	}

	CASE("an expired playback period is counted and is not elapsed time");
	{
		t_reset();
		hook_reset();
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 20 };
		struct engine_stats st;

		t_script.expire_at = T_PREFILL + 7;
		t_script.expire_periods = 2;
		opts.hooks.ctx = &hook_ctx;
		opts.hooks.tick = hook_tick;
		opts.hooks.skip = hook_skip;
		CHECK(engine_run_ops(&t_ops_hold, &cfg, &opts, &st));
		CHECK_UINT(st.playback_expired, 2);
		CHECK_UINT(st.xruns, 0);
		CHECK_UINT(st.write_errors, 0);
		CHECK_UINT(st.holds, 0);
		CHECK_UINT(skip_ctx.calls, 0);
		CHECK_UINT(st.ticks, 20);
	}

	/*
	 * A fault inside the catch-up ends the tick. The recovery rebuilt both
	 * rings and re-prefilled playback, so the period the hook filled
	 * belongs to a ring that is gone: the journal must show the recovery,
	 * the three prefill writes and the start, and then a read, never a
	 * write of the period that was held.
	 */
	CASE("a capture skip fault ends the tick and rebuilds the ring");
	{
		t_reset();
		hook_reset();
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 80 };
		struct engine_stats st;

		t_script.hold_at[0] = 5;
		t_script.hold_periods[0] = 16;
		t_script.hold_at[1] = 40;
		t_script.hold_periods[1] = 16;
		t_script.skip_err_n = 2;
		t_script.skip_err = -EIO;
		opts.hooks.ctx = &hook_ctx;
		opts.hooks.tick = hook_tick;
		opts.hooks.skip = hook_skip;
		CHECK(engine_run_ops(&t_ops_hold, &cfg, &opts, &st));
		CHECK_UINT(st.read_errors, 2);
		CHECK_UINT((uint64_t)st.last_errno, (uint64_t)EIO);
		CHECK_UINT(t_journal_count("krwwwsR"), 2);
		CHECK_UINT(t_journal_count("krwwwsw"), 0);
		CHECK_UINT(t_journal_count("krwwwsW"), 0);
		CHECK_UINT(st.recover_failures, 0);
		CHECK_UINT(st.trim_drops, 0);
		CHECK(!st.stopped_fatal);
		/* Each fault is one strike and the run resets it, so ten
		 * consecutive ones are still what ends a loop. */
		CHECK_UINT(st.ticks, 80);
	}

	/*
	 * The episode state machine takes all three fresh reads or none. A back
	 * end that cannot skip the backlog would hold an episode open for the
	 * life of the stream, so it keeps the per-tick rule instead: one
	 * capture stall for every tick that finds a backlog above one period.
	 */
	CASE("a back end short of one fresh read opens no episode");
	{
		const struct udsp_tdm_ops *part[3] = {
			&t_ops_no_skip, &t_ops_no_cap_fresh, &t_ops_no_room,
		};
		struct udsp_config cfg;
		struct engine_stats st;

		cfg_init(&cfg, NULL, NULL);
		for (unsigned i = 0; i < 3; i++) {
			t_reset();
			hook_reset();
			struct engine_opts opts = { .loopback = true,
						    .max_ticks = 60 };

			t_script.hold_at[0] = 5;
			t_script.hold_periods[0] = 16;
			opts.hooks.ctx = &hook_ctx;
			opts.hooks.tick = hook_tick;
			opts.hooks.skip = hook_skip;
			CHECK(engine_run_ops(part[i], &cfg, &opts, &st));
			CHECK_UINT(st.holds, 0);
			CHECK_UINT(st.trim_drops, 0);
			CHECK_UINT(st.cal_restarts, 0);
			CHECK_UINT(t_skips, 0);
			CHECK(st.capture_stalls > 10);
			CHECK(strchr(t_journal, 'k') == NULL);
			CHECK(strchr(t_journal, 'F') == NULL);
			CHECK(strchr(t_journal, 'f') == NULL);
		}
	}

	/*
	 * A transient fault on the trim keeps the captured period and runs the
	 * catch-up again on the next tick. The two fresh reads and the skip
	 * repeat, and no write comes between them.
	 */
	CASE("a transient capture skip fault retries before the write");
	{
		t_reset();
		hook_reset();
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 60 };
		struct engine_stats st;

		t_script.hold_at[0] = 5;
		t_script.hold_periods[0] = 16;
		t_script.skip_err_n = 2;
		t_script.skip_err = -EAGAIN;
		opts.hooks.ctx = &hook_ctx;
		opts.hooks.tick = hook_tick;
		opts.hooks.skip = hook_skip;
		CHECK(engine_run_ops(&t_ops_hold, &cfg, &opts, &st));
		CHECK_UINT(st.again, 2);
		CHECK_UINT(st.read_errors, 0);
		CHECK_UINT(st.recover_failures, 0);
		/* The loopback tick writes the captured period, so the write
		 * of the retried tick is the W after the third attempt. */
		CHECK_UINT(t_journal_count("FfkFfkFfkW"), 1);
		CHECK_UINT(t_journal_count("FfkW"), 1);
		CHECK_UINT(t_skips, 1);
		CHECK_UINT(st.trim_drops, 12);
		CHECK_UINT(st.holds, 1);
		CHECK(!st.stopped_fatal);
	}

	/*
	 * The episode threshold is two whole periods after the read. One period
	 * is what every tick finds at its own wake, so it is not a hold.
	 */
	CASE("one period of backlog is not an episode and two are");
	{
		struct udsp_config cfg;
		struct engine_stats st;

		cfg_init(&cfg, NULL, NULL);
		for (unsigned h = 2; h <= 3; h++) {
			t_reset();
			hook_reset();
			struct engine_opts opts = { .loopback = true,
						    .max_ticks = 40 };

			t_script.hold_at[0] = 10;
			t_script.hold_periods[0] = h;
			opts.hooks.ctx = &hook_ctx;
			opts.hooks.tick = hook_tick;
			opts.hooks.skip = hook_skip;
			CHECK(engine_run_ops(&t_ops_hold, &cfg, &opts, &st));
			CHECK_UINT(st.holds, h - 2u);
			CHECK_UINT(st.capture_stalls, h - 2u);
			CHECK_UINT(st.cal_restarts, h - 2u);
			CHECK_UINT(st.trim_drops, 0);
			CHECK_UINT(st.xruns, 0);
		}
	}

	/*
	 * The delay half of the episode. A hold moves the playback queue by
	 * design, so the drift it causes is measured and reported but not
	 * counted; a drift outside an episode still counts. The excursion runs
	 * well after the second calibration window closes.
	 */
	CASE("a hold holds the delay stalls down and a drift outside counts");
	{
		t_reset();
		hook_reset();
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 90 };
		struct engine_stats st;

		t_script.hold_at[0] = 25;
		t_script.hold_periods[0] = 16;
		t_script.delay_step_from = 70;
		t_script.delay_step_count = 3;
		t_script.delay_step = 2 * T_FRAMES;
		opts.hooks.ctx = &hook_ctx;
		opts.hooks.tick = hook_tick;
		opts.hooks.skip = hook_skip;
		CHECK(engine_run_ops(&t_ops_hold, &cfg, &opts, &st));
		CHECK_UINT(st.holds, 1);
		CHECK(st.delay_calibrated);
		/* The three excursion samples and nothing else: the episode
		 * swung the queue from an empty ring back to its prefill
		 * depth. */
		CHECK_UINT(st.delay_stalls, 3);
	}

	/*
	 * The holder that arrives in the write. The hardware drains the ring
	 * while the engine is in that write, the delay sample after it reads
	 * two periods below the reference, and that is the episode. The read
	 * of the next tick continues it and counts nothing of its own, so one
	 * holder is one hold and one capture stall.
	 */
	CASE("a hold inside a write is one episode and no delay stall");
	{
		t_reset();
		hook_reset();
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 60 };
		struct engine_stats st;

		t_script.hold_at[0] = 5;
		t_script.hold_periods[0] = 6;
		/* Well past the sixteen samples the window after the first
		 * episode takes, so the reference stands. */
		t_script.write_hold_at = T_PREFILL + 30;
		t_script.write_hold_periods = 6;
		opts.hooks.ctx = &hook_ctx;
		opts.hooks.tick = hook_tick;
		opts.hooks.skip = hook_skip;
		CHECK(engine_run_ops(&t_ops_hold, &cfg, &opts, &st));
		CHECK_UINT(st.holds, 2);
		CHECK_UINT(st.capture_stalls, 2);
		CHECK_UINT(st.delay_stalls, 0);
		CHECK_UINT(st.cal_restarts, 2);
		CHECK_UINT(st.capture_drops, 0);
		CHECK_UINT(st.xruns, 0);
		CHECK(st.delay_calibrated);
		CHECK(st.delay_drift_min <= -2 * (int32_t)T_FRAMES);
		CHECK_UINT((uint64_t)t_backlog, 0);
		CHECK_UINT((uint64_t)t_queued, (uint64_t)T_PREFILL * T_FRAMES);
	}

	/*
	 * The refill stops at the depth of a clean start, not at the granted
	 * ring. On a ring of eight periods under a prefill of three, a refill
	 * to the ring would leave the downlink five periods deeper than at
	 * rest for the life of the stream.
	 */
	CASE("a hold on a deep ring refills playback to the prefill depth");
	{
		t_reset();
		hook_reset();
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 60 };
		struct engine_stats st;

		t_script.grant_periods = 8;
		t_script.hold_at[0] = 25;
		t_script.hold_periods[0] = 12;
		opts.hooks.ctx = &hook_ctx;
		opts.hooks.tick = hook_tick;
		opts.hooks.skip = hook_skip;
		CHECK(engine_run_ops(&t_ops_hold, &cfg, &opts, &st));
		CHECK_UINT(st.prefill_periods, T_PREFILL);
		CHECK_UINT(st.granted_periods, 8);
		CHECK_UINT(st.holds, 1);
		CHECK_UINT(st.cal_restarts, 1);
		CHECK(st.delay_calibrated);
		CHECK_UINT((uint64_t)t_backlog, 0);
		CHECK_UINT((uint64_t)t_queued, (uint64_t)T_PREFILL * T_FRAMES);
		/* The window after the hold reads the reference of the window
		 * before it. */
		CHECK_UINT((uint64_t)st.delay_ref, (uint64_t)T_PREFILL * T_FRAMES);
	}

	/*
	 * The fresh reads without avail. The read side opens an episode on the
	 * backlog avail reports, so this back end opens none at all. The drift
	 * the hold leaves counts as a delay stall, because a back end that
	 * cannot trim opens no playback episode either. The capture stalls it
	 * reports come from the tick-interval inference alone, which this
	 * model never reaches because it spends no wall time on a hold.
	 */
	CASE("the fresh reads without avail open no episode");
	{
		t_reset();
		hook_reset();
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true, .max_ticks = 60 };
		struct engine_stats st;

		t_script.hold_at[0] = 25;
		t_script.hold_periods[0] = 16;
		opts.hooks.ctx = &hook_ctx;
		opts.hooks.tick = hook_tick;
		opts.hooks.skip = hook_skip;
		CHECK(engine_run_ops(&t_ops_no_avail, &cfg, &opts, &st));
		CHECK_UINT(st.holds, 0);
		CHECK_UINT(st.capture_stalls, 0);
		CHECK_UINT(st.delay_stalls, 1);
		CHECK_UINT(st.trim_drops, 0);
		CHECK_UINT(st.cal_restarts, 0);
		CHECK_UINT(t_skips, 0);
		CHECK(strchr(t_journal, 'f') == NULL);
		CHECK(strchr(t_journal, 'F') == NULL);
		CHECK(strchr(t_journal, 'k') == NULL);
	}

	CASE("a run without a hold never fires the catch-up rule");
	{
		t_reset();
		hook_reset();
		struct udsp_config cfg;
		cfg_init(&cfg, NULL, NULL);
		struct engine_opts opts = { .loopback = true,
					    .max_ticks = 2000 };
		struct engine_stats st;

		opts.hooks.ctx = &hook_ctx;
		opts.hooks.tick = hook_tick;
		opts.hooks.skip = hook_skip;
		CHECK(engine_run_ops(&t_ops_hold, &cfg, &opts, &st));
		CHECK_UINT(st.ticks, 2000);
		CHECK_UINT(st.holds, 0);
		CHECK_UINT(st.capture_stalls, 0);
		CHECK_UINT(st.delay_stalls, 0);
		CHECK_UINT(st.capture_drops, 0);
		CHECK_UINT(st.trim_drops, 0);
		CHECK_UINT(st.playback_expired, 0);
		CHECK_UINT(st.xruns, 0);
		CHECK_UINT(st.cal_restarts, 0);
		CHECK_UINT(skip_ctx.calls, 0);
		CHECK_UINT(t_skips, 0);
		/* The fresh reads are the decision path and they run only
		 * inside an episode, so a quiet run makes none of them. */
		CHECK(strchr(t_journal, 'F') == NULL);
		CHECK(strchr(t_journal, 'k') == NULL);
	}

	case_phased_hold();
	case_phased_transitions();

	return tu_summary("engine");
}
