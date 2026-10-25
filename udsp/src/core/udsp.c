// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The public API of libudsp (include/udsp/udsp.h). This file is the control
 * thread's whole view: it validates, translates and posts, and it blocks only
 * where the public API says a call blocks.
 *
 * One control mutex serialises the caller's uloop thread and its one SLOW
 * worker for the whole post-and-wait of a SYNC op, so the single ack slot has
 * exactly one waiter at a time. A SYNC op never holds a lock the engine or the
 * slic thread can want.
 *
 * Test and lab setting. On an unpaced tdm backend (tdm/file.c, a virtual
 * clock that ticks at CPU speed) an unbounded run appends playback
 * periods until the disk fills, so the engine refuses one. udsp_open reads the
 * environment variable UDSP_MAX_TICKS once, on the control thread, and uses it
 * as the tick bound; unset or 0 on an unpaced backend fails the open. A paced
 * backend ignores it and runs unbounded.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include "udsp/codec.h"

#include "../slic/fake.h"

#include "net.h"
#include "safe_state.h"
#include "udsp_priv.h"
#include "stream.h"

#define UDSP_ATTACH_WAIT_MS	10000u
#define UDSP_STREAMS_PER_CHAN	2u	/* uvoice call waiting needs two */
#ifndef UDSP_VERSION
#error "UDSP_VERSION must be supplied by the build; see src/CMakeLists.txt"
#endif
#define UDSP_VERSION_PREFIX	"libudsp " UDSP_VERSION
/* PROSLIC_GAIN_MIN -30 dB and PROSLIC_GAIN_MAX +6 dB (udsp-slic-proslic
 * api/inc/proslic.h), in the contract's 0.5 dB steps. The transmit bound is
 * the reach of the plug-in, which refuses a value outside it with -ERANGE, so
 * the refusal happens here and no range errno ever reaches the tick. The
 * plug-in receives only the gain half of the receive value (chan.c
 * gain_emit), so the receive low bound limits the digital loss instead. */
#define UDSP_SLIC_GAIN_MIN	(-60)
#define UDSP_SLIC_GAIN_MAX	12

_Static_assert(sizeof(((struct udsp_engine_diag *)0)->interval) /
	       sizeof(uint64_t) == ENGINE_HIST_BUCKETS,
	       "engine diagnostics carry every interval bucket");

/* Signal dispositions, the safe-state block and the sigaltstack are process
 * wide, so exactly one handle may live at a time. */
static atomic_bool udsp_live;

/* The tdm backend a test injects; NULL is the ordinary name lookup. */
static const struct udsp_tdm_ops *udsp_tdm_test_ops;

void udsp_tdm_override(const struct udsp_tdm_ops *ops)
{
	udsp_tdm_test_ops = ops;
}

/* The T.38 gateway a test injects; NULL is the ordinary dlopen. */
static const struct udsp_fax_ops *udsp_fax_test_ops;

void udsp_fax_override(const struct udsp_fax_ops *ops)
{
	udsp_fax_test_ops = ops;
}

/* The interrupt edge source a test injects; -1 is the host's sysfs node. */
static int udsp_slic_irq_test_fd = -1;
static short udsp_slic_irq_test_events;

void udsp_slic_irq_override(int fd, short events)
{
	udsp_slic_irq_test_fd = fd;
	udsp_slic_irq_test_events = events;
}

/*
 * The edge source of the slic thread, settled before the thread starts. A
 * board whose pin takes an edge starts in BURST and settles into IDLE two
 * seconds later; every other board keeps the per-tick pass (slic_thread.h
 * enum slic_mode). A refused edge is not an error: the board falls back on
 * the per-tick pass.
 */
static void udsp_slic_irq_open(struct udsp *s)
{
	int fd = -1;

	if (udsp_slic_irq_test_fd >= 0) {
		fd = udsp_slic_irq_test_fd;
		s->slic_irq_events = udsp_slic_irq_test_events;
	} else if (s->host) {
		fd = slic_host_irq_arm(s->host);
		s->slic_irq_events = POLLPRI;
	}
	s->slic_irq_fd = fd >= 0 ? fd : -1;
	s->slic_irq_host = fd >= 0 && udsp_slic_irq_test_fd < 0;
	atomic_store_explicit(&s->slic_mode,
			      fd >= 0 ? SLIC_MODE_BURST : SLIC_MODE_POLL,
			      memory_order_relaxed);
	atomic_store_explicit(&s->slic_wants_tick, true, memory_order_release);
}

/* --- helpers ------------------------------------------------------------- */

/* NULL for a usable configuration, else the field that refuses it. */
static const char *cfg_refused(const struct udsp_config *cfg)
{
	if (!cfg)
		return "no configuration";
	if (strcmp(cfg->tdm_backend, "alsa") &&
	    strcmp(cfg->tdm_backend, "alsa-raw") &&
	    strcmp(cfg->tdm_backend, "file"))
		return "tdm_backend";
	if (cfg->channels < 1 || cfg->channels > 2)
		return "channels";
	if (cfg->prefill_periods > ENGINE_PREFILL_MAX)
		return "prefill_periods";
	/* Three values only (udsp.h sample_rate): 0 and 8000
	 * are the narrowband line, 16000 asks for the wideband one, and
	 * anything else fails before a device is opened. */
	if (cfg->sample_rate && cfg->sample_rate != UDSP_SAMPLE_RATE &&
	    cfg->sample_rate != UDSP_SAMPLE_RATE_WB)
		return "sample_rate";
	if (cfg->populated_ports < 1 || cfg->populated_ports > UDSP_LINE_MAX)
		return "populated_ports";

	return NULL;
}

static unsigned rate_normalise(uint16_t asked)
{
	return asked ? asked : UDSP_SAMPLE_RATE;
}

static uint64_t max_ticks_get(void)
{
	const char *v = getenv("UDSP_MAX_TICKS");
	char *end = NULL;
	unsigned long long n;

	if (!v || !*v)
		return 0;
	n = strtoull(v, &end, 10);
	if (end == v || (end && *end))
		return 0;

	return (uint64_t)n;
}

/*
 * One millisecond cadence out of the environment, for every test-time override
 * this file carries. The value must be a whole positive decimal and nothing
 * else: a sign, a suffix, an empty string, a zero and an out-of-range figure
 * all keep the production cadence, so an override can never disable a deadline
 * or shorten it to nothing. The leading-digit test is what refuses a sign,
 * because strtoul accepts one and wraps it (core/cmd.c cmd_sync_tick_ms_init).
 */
static unsigned env_ms_get(const char *name, unsigned def)
{
	const char *v = getenv(name);
	char *end = NULL;
	unsigned long n;

	if (!v || *v < '0' || *v > '9')
		return def;
	errno = 0;
	n = strtoul(v, &end, 10);
	if (errno || end == v || (end && *end) || n == 0 || n > UINT32_MAX)
		return def;

	return (unsigned)n;
}

/* The dead-line retry cadence, SLIC_DEAD_RETRY_MS unless a test overrides
 * it. */
static uint32_t dead_retry_ms_get(void)
{
	return env_ms_get("UDSP_SLIC_DEAD_RETRY_MS", SLIC_DEAD_RETRY_MS);
}

/* The watchdog cadence, UDSP_WATCHDOG_MS unless a test overrides it. The tick
 * freeze it declares fatal is two runs, so a test that shortens the cadence
 * shortens that deadline with it. */
unsigned udsp_watchdog_ms_get(void)
{
	return env_ms_get("UDSP_WATCHDOG_MS", UDSP_WATCHDOG_MS);
}

void udsp_log(struct udsp *s, enum udsp_log_level level, const char *fmt, ...)
{
	char buf[UDSP_LOG_MAX];
	va_list ap;

	if (!s->cfg.log)
		return;
	if (level == UDSP_LOG_DEBUG &&
	    !(atomic_load_explicit(&s->debug, memory_order_relaxed) &
	      UDSP_DEBUG_SLIC_LOG))
		return;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	s->cfg.log(s->cfg.log_ctx, level, buf);
}

/* Room for "<direction> <what>: <strerror>" with the longest token of the
 * built-in backends and the longest strerror text of glibc and musl. */
#define UDSP_TDM_FAIL_TEXT_MAX	128

void udsp_engine_fail_log(struct udsp *s)
{
	char detail[UDSP_TDM_FAIL_TEXT_MAX];

	engine_tdm_fail_text(&s->engine_fail.tdm, detail, sizeof(detail));
	udsp_log(s, UDSP_LOG_ERR, "tdm %s %s %s at %u Hz: %s%s%s%s",
		 s->cfg.tdm_backend, s->cfg.tdm_playback, s->cfg.tdm_capture,
		 s->rate, s->engine_fail.cause == ENGINE_OPEN_GEOM ?
		 "the device refused the geometry" : "the engine did not open",
		 detail[0] ? " (" : "", detail, detail[0] ? ")" : "");
}

static enum udsp_log_level slic_level_map(enum udsp_slic_log_level level)
{
	switch (level) {
	case UDSP_SLIC_LOG_ERR:
		return UDSP_LOG_ERR;
	case UDSP_SLIC_LOG_WARN:
		return UDSP_LOG_WARN;
	case UDSP_SLIC_LOG_INFO:
		return UDSP_LOG_INFO;
	case UDSP_SLIC_LOG_DEBUG:
		break;
	}

	return UDSP_LOG_DEBUG;
}

static void udsp_host_log(void *ctx, enum udsp_slic_log_level level,
			  const char *msg)
{
	udsp_log(ctx, slic_level_map(level), "%s", msg);
}

void udsp_fatal_latch(struct udsp *s, enum udsp_ev_producer p)
{
	struct udsp_event ev;

	if (atomic_exchange_explicit(&s->fatal, true, memory_order_acq_rel))
		return;
	if (p >= UDSP_EV_PROD_MAX)
		return;

	memset(&ev, 0, sizeof(ev));
	ev.type = UDSP_EV_SLIC_FAULT;
	ev.stamp_ms = udsp_now_ms(s);
	ev.u.fault.code = UDSP_FAULT_ENGINE_STALL;
	udsp_emit(&s->emitter, p, &ev);
}

/* --- engine lifetime ----------------------------------------------------- */

/* The an7581 PCM framer masters PCLK and FSYNC from CFG_VALID with the DMA off,
 * so the engine can defer its start; the MediaTek AFE needs a running stream
 * for the clock. The an7581 plug-in pairs with the an7581 PCM, and the in-tree
 * fake (an empty plug-in path) stands in for it. */
static bool udsp_pcm_clock_free_running(const struct udsp_config *cfg)
{
	if (!cfg->slic_plugin[0])
		return true;

	return strstr(cfg->slic_plugin, "an7581") != NULL;
}

/* The figures udsp_counters answers with. They follow the engine that is
 * running now, and they are republished at every watchdog run and at every
 * engine teardown, which is the resolution a diagnostic counter needs
 * (udsp.h). */
static void udsp_engine_figures_publish(struct udsp *s,
					const struct engine_stats *st)
{
	atomic_store_explicit(&s->tdm_stalls, engine_stats_stalls(st),
			      memory_order_relaxed);
	atomic_store_explicit(&s->holds, st->holds, memory_order_relaxed);
	atomic_store_explicit(&s->capture_drops, st->capture_drops,
			      memory_order_relaxed);
	atomic_store_explicit(&s->trim_drops, st->trim_drops,
			      memory_order_relaxed);
	atomic_store_explicit(&s->playback_expired, st->playback_expired,
			      memory_order_relaxed);
}

int udsp_engine_create(struct udsp *s)
{
	struct engine_opts opts;
	struct engine_stats st;
	struct engine *e;

	pthread_mutex_lock(&s->engine_lock);
	e = s->engine;
	pthread_mutex_unlock(&s->engine_lock);
	memset(&s->engine_fail, 0, sizeof(s->engine_fail));
	if (e)
		return 0;

	/* Written before the tick thread exists: that thread runs from engine
	 * start, so a later write would race its first pass. */
	for (unsigned i = 0; i < UDSP_LINE_MAX; i++) {
		atomic_store_explicit(&s->lec_ready[i], false,
				      memory_order_relaxed);
		atomic_store_explicit(&s->lec_delay[i], 0u,
				      memory_order_relaxed);
	}
	atomic_store_explicit(&s->lec_loop, 0u, memory_order_relaxed);

	memset(&opts, 0, sizeof(opts));
	opts.max_ticks = s->max_ticks;
	/* The tick bound binds an unpaced backend only. */
	opts.max_ticks_unpaced_only = true;
	opts.hold_gate = true;
	/* Defer the DMA start until the SLIC init is done so no capture recovery
	 * can reset the PCM block mid-init. The MediaTek AFE gates its clock
	 * without a running stream, so the deferral is limited to the raw back
	 * end on the an7581 PCM. */
	opts.defer_start = !strcmp(s->cfg.tdm_backend, "alsa-raw") &&
			   udsp_pcm_clock_free_running(&s->cfg);
	opts.hooks.ctx = s;
	opts.hooks.tick = udsp_engine_hook;
	opts.hooks.skip = udsp_engine_skip;
	opts.hooks.render = udsp_engine_render;
	opts.hooks.control = udsp_engine_control;

	/* The base must be in place before the first tick can publish. */
	s->clock.base = udsp_clock_tick(&s->clock) + 1u;
	e = udsp_tdm_test_ops ?
	    engine_open_ops(udsp_tdm_test_ops, &s->cfg, &opts, &s->engine_fail) :
	    engine_open(&s->cfg, &opts, &s->engine_fail);
	if (!e)
		return -EIO;

	engine_stats_get(e, &st);
	s->prefill_ms = st.prefill_periods * UDSP_TICK_MS;
	atomic_store_explicit(&s->clock.paced, st.paced, memory_order_release);
	pthread_mutex_lock(&s->engine_lock);
	s->engine = e;
	pthread_mutex_unlock(&s->engine_lock);
	/* Published before the gate opens, so the first tick already sees it
	 * and the tick thread never takes the engine mutex. tick_parked is set
	 * before engine_live, so a guard that sees the engine live also sees
	 * whether its tick is parked. */
	atomic_store_explicit(&s->engine_pub, e, memory_order_release);
	atomic_store_explicit(&s->tick_parked, opts.defer_start,
			      memory_order_release);
	atomic_store_explicit(&s->engine_live, true, memory_order_release);

	return 0;
}

void udsp_engine_gate(struct udsp *s)
{
	pthread_mutex_lock(&s->engine_lock);
	if (s->engine)
		engine_gate_open(s->engine);
	pthread_mutex_unlock(&s->engine_lock);
}

int udsp_engine_start_streaming(struct udsp *s)
{
	int rc = 0;

	pthread_mutex_lock(&s->engine_lock);
	if (s->engine) {
		rc = engine_start_streaming(s->engine);
		/* Cleared only when the start released the gate. A start that
		 * aborted on a close leaves the tick parked, so tick_parked
		 * stays set and a following stop still skips the safe pass. */
		if (!engine_deferred(s->engine))
			atomic_store_explicit(&s->tick_parked, false,
					      memory_order_release);
	}
	pthread_mutex_unlock(&s->engine_lock);

	return rc;
}

int udsp_engine_destroy(struct udsp *s, enum udsp_ev_producer p)
{
	struct timespec nap = { .tv_sec = 0, .tv_nsec = 1000000L };
	struct engine_stats st;
	struct engine *e;
	uint64_t end;

	/* engine_stop_request touches the gate and e->live, which
	 * udsp_engine_start_streaming also touches under this lock from the slic
	 * thread; hold it across the request so a close cannot race a deferred
	 * start. */
	pthread_mutex_lock(&s->engine_lock);
	e = s->engine;
	if (e)
		engine_stop_request(e);
	atomic_store_explicit(&s->tick_parked, false, memory_order_release);
	pthread_mutex_unlock(&s->engine_lock);
	if (!e)
		return 0;
	end = udsp_mono_us() + (uint64_t)UDSP_STOP_ENGINE_MS * 1000u;
	while (!engine_done(e) && udsp_mono_us() < end)
		nanosleep(&nap, NULL);

	atomic_store_explicit(&s->engine_pub, NULL, memory_order_release);
	if (!engine_done(e)) {
		/* The thread is still inside the backend, so its arena, its
		 * buffers and the tdm handle stay allocated for good: freeing
		 * memory a live thread may touch corrupts uvoiced. */
		atomic_fetch_add_explicit(&s->stop_timeouts, 1u,
					  memory_order_relaxed);
		atomic_store_explicit(&s->dead, true, memory_order_release);
		atomic_store_explicit(&s->engine_live, false, memory_order_release);
		pthread_mutex_lock(&s->engine_lock);
		s->engine = NULL;
		atomic_store_explicit(&s->engine_diag_valid, false,
				      memory_order_release);
		pthread_mutex_unlock(&s->engine_lock);
		/* The caller names its own ring: every event ring is single
		 * producer, so the stop job posts as SLIC and udsp_close posts
		 * as CTL. */
		udsp_fatal_latch(s, p);
		return -ETIMEDOUT;
	}

	atomic_store_explicit(&s->engine_live, false, memory_order_release);
	pthread_mutex_lock(&s->engine_lock);
	engine_stop(e, &st);
	s->engine = NULL;
	atomic_store_explicit(&s->engine_diag_valid, false,
			      memory_order_release);
	pthread_mutex_unlock(&s->engine_lock);
	engine_close(e);
	udsp_engine_figures_publish(s, &st);
	s->clock.base = udsp_clock_tick(&s->clock) + 1u;

	return 0;
}

/* --- the line rate settle ------------------------------------------------ */

static const char *rate_reason_name(enum udsp_rate_degrade why)
{
	switch (why) {
	case UDSP_RATE_DEGRADE_TDM:
		return "the tdm device refused the rate";
	case UDSP_RATE_DEGRADE_NO_CAPS:
		return "the slic plug-in reports no capabilities";
	case UDSP_RATE_DEGRADE_NO_WIDEBAND:
		return "the slic part has no wideband mode";
	default:
		return "unknown";
	}
}

/*
 * The caller asked for a 16 kHz line and gets an 8 kHz one, which no return
 * value of udsp_open reports, so the reason goes to the sink. It runs on the
 * control thread, at most once per open, and udsp.h promises it
 * (udsp_config.sample_rate "with a logged reason"). detail is the tdm
 * refusal clause for the TDM reason and NULL for the others.
 */
static void rate_degrade(struct udsp *s, enum udsp_rate_degrade why,
			 const char *detail)
{
	bool detailed = detail && detail[0];

	s->rate = UDSP_SAMPLE_RATE;
	s->frame = UDSP_FRAME_SAMPLES;
	s->cfg.sample_rate = (uint16_t)UDSP_SAMPLE_RATE;
	s->rate_reason = (uint8_t)why;
	s->rate_degrades++;
	/* The plug-in reads the host fact at init, which runs inside the start
	 * job, so clearing it here is what keeps a degraded line narrowband on
	 * the wire as well (slic.h pcm_wideband). */
	if (s->host)
		slic_host_pcm_wideband_set(s->host, false);
	udsp_log(s, UDSP_LOG_WARN,
		 "%u Hz refused (%s), the line runs at %u Hz%s%s%s",
		 UDSP_SAMPLE_RATE_WB, rate_reason_name(why), UDSP_SAMPLE_RATE,
		 detailed ? " (" : "", detailed ? detail : "",
		 detailed ? ")" : "");
}

/*
 * The first half of the settle: the TDM carries the rate, so a refusal at
 * 16 kHz reopens the whole engine at 8000. The tdm open itself never
 * renegotiates (tdm.h).
 *
 * Only a geometry refusal degrades. An allocation, a thread, the prefill or
 * the start failing has nothing to do with the rate: it fails the same way at
 * 8000, so a retry there would report a rate refusal that never happened and
 * could mask a transient fault behind a working narrowband line. Those causes
 * fail udsp_open at 16 kHz exactly as they would at 8 kHz (engine.h
 * enum engine_open_fail).
 */
static int rate_settle_open(struct udsp *s)
{
	char detail[UDSP_TDM_FAIL_TEXT_MAX];

	if (!udsp_engine_create(s))
		return 0;
	if (s->rate == UDSP_SAMPLE_RATE ||
	    s->engine_fail.cause != ENGINE_OPEN_GEOM) {
		udsp_engine_fail_log(s);
		return -EIO;
	}

	engine_tdm_fail_text(&s->engine_fail.tdm, detail, sizeof(detail));
	rate_degrade(s, UDSP_RATE_DEGRADE_TDM, detail);

	if (udsp_engine_create(s)) {
		udsp_engine_fail_log(s);
		return -EIO;
	}

	return 0;
}

/*
 * The second half of the settle: the capability the attach decoded. slic.h
 * names udsp_open as caps_get's caller, and this is the one point where that
 * read is safe from the control thread: the attach job has completed, no other
 * job can be posted before udsp_open returns, and cap.lines is still zero, so
 * the slic thread's periodic pass runs over no line and calls no op.
 */
static int rate_settle_caps(struct udsp *s)
{
	if (s->rate != UDSP_SAMPLE_RATE_WB)
		return 0;
	if (!s->ops->caps_get)
		rate_degrade(s, UDSP_RATE_DEGRADE_NO_CAPS, NULL);
	else if (!(s->ops->caps_get(s->slic) & UDSP_SLIC_CAP_WIDEBAND))
		rate_degrade(s, UDSP_RATE_DEGRADE_NO_WIDEBAND, NULL);
	else
		return 0;

	if (udsp_engine_destroy(s, UDSP_EV_PROD_CTL)) {
		udsp_log(s, UDSP_LOG_ERR,
			 "the engine did not stop for the rate settle");
		return -EIO;
	}
	if (udsp_engine_create(s)) {
		udsp_engine_fail_log(s);
		return -EIO;
	}

	return 0;
}

/*
 * One line per watchdog run in which a catch-up figure moved, with the
 * cumulative totals. The engine itself never logs, and a line per episode
 * would come from the tick thread; the run that publishes the figures is the
 * one place a text sink can be reached at a bounded rate.
 */
static void udsp_hold_warn(struct udsp *s, const struct engine_stats *st)
{
	struct udsp_hold_mark *w = &s->hold_warned;

	if (st->serial != w->serial) {
		memset(w, 0, sizeof(*w));
		w->serial = st->serial;
	}
	if (st->holds == w->holds && st->capture_drops == w->capture_drops &&
	    st->trim_drops == w->trim_drops &&
	    st->playback_expired == w->playback_expired &&
	    st->rephases == w->rephases &&
	    st->rephase_aborts == w->rephase_aborts &&
	    st->rephase_withheld == w->rephase_withheld)
		return;

	w->holds = st->holds;
	w->capture_drops = st->capture_drops;
	w->trim_drops = st->trim_drops;
	w->playback_expired = st->playback_expired;
	w->rephases = st->rephases;
	w->rephase_aborts = st->rephase_aborts;
	w->rephase_withheld = st->rephase_withheld;
	udsp_log(s, UDSP_LOG_DEBUG,
		 "engine hold: %llu episodes, %llu capture periods "
		 "overwritten, %llu trimmed, %llu playback expired, "
		 "%llu rephased, %llu rephase aborts, %llu writes held",
		 (unsigned long long)st->holds,
		 (unsigned long long)st->capture_drops,
		 (unsigned long long)st->trim_drops,
		 (unsigned long long)st->playback_expired,
		 (unsigned long long)st->rephases,
		 (unsigned long long)st->rephase_aborts,
		 (unsigned long long)st->rephase_withheld);
}

void udsp_watchdog_run(void *ctx)
{
	struct udsp *s = ctx;
	struct engine_stats st;
	bool have = false;

	pthread_mutex_lock(&s->engine_lock);
	if (s->engine) {
		engine_stats_get(s->engine, &st);
		/* A reader can defer the snapshot, so defer both public views
		 * together to keep their counters on the same watchdog run. */
		if (engine_diag_snap_publish(&s->engine_diag, &st)) {
			udsp_engine_figures_publish(s, &st);
			atomic_store_explicit(&s->engine_diag_valid, true,
					      memory_order_release);
		}
		have = true;
	} else {
		atomic_store_explicit(&s->engine_diag_valid, false,
				      memory_order_release);
	}
	pthread_mutex_unlock(&s->engine_lock);
	if (!have) {
		s->watchdog_primed = false;
		return;
	}
	udsp_hold_warn(s, &st);
	if (st.stopped_fatal) {
		udsp_fatal_latch(s, UDSP_EV_PROD_WATCHDOG);
		return;
	}
	/* The paced guard is the false-positive rule: a file-backend run that
	 * ended at max_ticks is not a stall. */
	if (!st.running || !st.paced || !st.rt_done_us) {
		s->watchdog_primed = false;
		return;
	}
	if (!s->watchdog_primed || st.ticks != s->watchdog_ticks) {
		s->watchdog_primed = true;
		s->watchdog_ticks = st.ticks;
		return;
	}
	udsp_fatal_latch(s, UDSP_EV_PROD_WATCHDOG);
}

/* --- SYNC op mechanics --------------------------------------------------- */

static void slic_wake(struct udsp *s)
{
	uint64_t one = 1;

	if (write(s->slic_cmd_fd, &one, sizeof(one)) != (ssize_t)sizeof(one))
		return;
}

/*
 * True while the SLIC plug-in holds the line for a metallic line test. The
 * plug-in opens the linefeed for the measurement and rings the line at 16 Vrms
 * for the ringer part, so every op that drives the line or feeds it is refused
 * until the test returns: a burst, a gain write or a stream that starts now
 * would fight the plug-in for the same wire. The audio playout ops are not
 * refused, because a line with an open feed carries none of their sound.
 */
static bool line_under_test(const struct udsp *s, unsigned line)
{
	if (!s || line >= s->cap.lines)
		return false;

	return atomic_load_explicit(&s->chan[line].under_test,
				    memory_order_acquire);
}

/*
 * True while a delayed loop stands on the line (udsp.h udsp_line_loop). The
 * loop owns the line's downlink, so a metallic line test, a stream
 * configuration and a stream start are refused for as long as it does. The
 * tick is the one writer of the record and it writes it before it acks the
 * arm, so an arm this call follows is always seen.
 *
 * It is deliberately not folded into udsp_line_busy. The arm itself reads that
 * predicate, so a loop inside it would refuse every second arm, and the
 * dead-line init retry reads it too, where a loop must not hold a line down
 * for the length of a manufacturing test.
 */
bool udsp_line_loop_armed(const struct udsp *s, unsigned line)
{
	if (!s || line >= s->cap.lines)
		return false;

	return atomic_load_explicit(&s->loop_armed[line],
				    memory_order_acquire) != 0;
}

static enum udsp_status handle_guard(const struct udsp *s, unsigned line)
{
	if (!s || line >= s->cap.lines)
		return UDSP_UNSUPPORTED;
	if (atomic_load_explicit(&s->fatal, memory_order_acquire))
		return UDSP_FATAL;

	return UDSP_OK;
}

static enum udsp_status engine_guard(const struct udsp *s)
{
	if (!atomic_load_explicit(&s->engine_live, memory_order_acquire))
		return UDSP_TRANSIENT;
	/* A deferred engine parks its tick until the start, so a tick op posted
	 * now cannot be acked; refuse it fast rather than let it time out. */
	if (atomic_load_explicit(&s->tick_parked, memory_order_acquire))
		return UDSP_TRANSIENT;

	return UDSP_OK;
}

/* No dead test: the DEACTIVATE a stream stop posts is applied by the tick's
 * command drain, which does not read dead, and the stream a call left on a
 * dead line must still be returned or the line stays busy for the life of
 * the handle (udsp_line_busy, the init retry). */
enum udsp_status udsp_release_guard(struct udsp *s, unsigned line)
{
	enum udsp_status st = handle_guard(s, line);

	return st != UDSP_OK ? st : engine_guard(s);
}

enum udsp_status udsp_guard(struct udsp *s, unsigned line, bool slic_class)
{
	enum udsp_status st = handle_guard(s, line);

	if (st != UDSP_OK)
		return st;
	if (atomic_load_explicit(&s->chan[line].dead, memory_order_acquire))
		return UDSP_FATAL;
	st = engine_guard(s);
	if (st != UDSP_OK)
		return st;
	if (!slic_class)
		return UDSP_OK;
	if (!atomic_load_explicit(&s->started, memory_order_acquire))
		return UDSP_TRANSIENT;
	/* A SLOW job holds the slic thread for seconds, so a SYNC-SLIC op
	 * fails fast instead of eating its whole bound (udsp.h section 1). */
	if (atomic_load_explicit(&s->job_inflight, memory_order_acquire))
		return UDSP_TRANSIENT;
	/* Every op of this class writes the part, and a line test owns it. */
	if (line_under_test(s, line))
		return UDSP_TRANSIENT;

	return UDSP_OK;
}

enum udsp_status udsp_sync_locked(struct udsp *s, struct udsp_cmd *cmd,
				  unsigned timeout_ms)
{
	uint32_t token = ++s->ctl_token;
	enum udsp_status st;
	int32_t status = 0;

	if (!token || token >= CMD_TOKEN_DONE)
		token = s->ctl_token = 1u;
	st = cmd_post_sync(&s->ctl_ring, &s->ctl_ack, token, cmd, timeout_ms,
			   &status);
	if (st != UDSP_OK)
		udsp_op_error(s);

	return st;
}

static enum udsp_status udsp_sync(struct udsp *s, struct udsp_cmd *cmd,
				  unsigned timeout_ms, bool slic_class)
{
	enum udsp_status st;

	pthread_mutex_lock(&s->ctl_lock);
	/* The first guard ran before the lock, so a job or a stop could have
	 * started in between; the decision that counts is the one taken with
	 * the slot in hand. */
	st = udsp_guard(s, cmd->line, slic_class);
	if (st == UDSP_OK)
		st = udsp_sync_locked(s, cmd, timeout_ms);
	pthread_mutex_unlock(&s->ctl_lock);

	return st;
}

static void cmd_line_init(struct udsp_cmd *cmd, enum udsp_cmd_type type,
			  unsigned line)
{
	memset(cmd, 0, sizeof(*cmd));
	cmd->type = (uint8_t)type;
	cmd->line = (uint8_t)line;
}

/* --- SLOW jobs ----------------------------------------------------------- */

/* One job at a time reaches the slic thread for the two kinds that hold it for
 * seconds and drive the lines. A region load and a stop are not gated here:
 * the stop supersedes instead, and the region job is the shape the caller
 * repeats. */
static bool job_kind_exclusive(uint8_t kind)
{
	return kind == SLIC_JOB_START || kind == SLIC_JOB_LINE_TEST;
}

static enum udsp_status udsp_job_post(struct udsp *s,
				      const struct slic_job *tmpl,
				      struct udsp_job *job)
{
	struct slic_job j = *tmpl;

	pthread_mutex_lock(&s->ctl_lock);
	if (atomic_load_explicit(&s->fatal, memory_order_acquire)) {
		pthread_mutex_unlock(&s->ctl_lock);
		return UDSP_FATAL;
	}
	if (job_kind_exclusive(j.kind) &&
	    atomic_load_explicit(&s->job_inflight, memory_order_acquire)) {
		pthread_mutex_unlock(&s->ctl_lock);
		return UDSP_TRANSIENT;
	}
	j.token = ++s->job_token;
	if (!j.token)
		j.token = s->job_token = 1u;
	/* A stop supersedes a start and a line test that have not begun; one
	 * already executing completes first. */
	if (j.kind == SLIC_JOB_STOP)
		slic_jobq_supersede(&s->jobs);
	if (!slic_jobq_push(&s->jobs, &j)) {
		pthread_mutex_unlock(&s->ctl_lock);
		udsp_op_error(s);
		return UDSP_TRANSIENT;
	}
	atomic_fetch_add_explicit(&s->job_inflight, 1u, memory_order_acq_rel);
	if (j.kind == SLIC_JOB_LINE_TEST)
		atomic_fetch_add_explicit(&s->line_test_jobs, 1u,
					  memory_order_acq_rel);
	pthread_mutex_unlock(&s->ctl_lock);

	slic_wake(s);
	job->token = j.token;

	return UDSP_OK;
}

enum udsp_status udsp_start(udsp_t *s, struct udsp_job *job)
{
	struct slic_job j = { .kind = SLIC_JOB_START };

	if (!s || !job)
		return UDSP_UNSUPPORTED;

	return udsp_job_post(s, &j, job);
}

enum udsp_status udsp_stop(udsp_t *s, struct udsp_job *job)
{
	struct slic_job j = { .kind = SLIC_JOB_STOP };

	if (!s || !job)
		return UDSP_UNSUPPORTED;

	return udsp_job_post(s, &j, job);
}

enum udsp_status udsp_region_load(udsp_t *s, const char *iso3,
				  struct udsp_job *job)
{
	struct slic_job j = { .kind = SLIC_JOB_REGION };

	if (!s || !job || !iso3)
		return UDSP_UNSUPPORTED;
	/* The environment override is read on the control thread, where a
	 * getenv is safe. */
	if (udsp_region_path_build(iso3, j.path, sizeof(j.path)))
		return UDSP_UNSUPPORTED;

	return udsp_job_post(s, &j, job);
}

/* --- line control -------------------------------------------------------- */

enum udsp_status udsp_line_state(udsp_t *s, unsigned line, enum udsp_line_state st)
{
	enum udsp_status g = udsp_guard(s, line, true);
	struct udsp_cmd cmd;

	if (g != UDSP_OK)
		return g;
	if (st > UDSP_LINE_HOWLER)
		return UDSP_UNSUPPORTED;

	cmd_line_init(&cmd, UDSP_CMD_LINE_STATE, line);
	cmd.arg0 = (uint32_t)st;

	return udsp_sync(s, &cmd, UDSP_SYNC_SLIC_MS, true);
}

enum udsp_status udsp_line_polarity(udsp_t *s, unsigned line,
				    enum udsp_polarity pol)
{
	enum udsp_status g = udsp_guard(s, line, true);
	struct udsp_cmd cmd;

	if (g != UDSP_OK)
		return g;
	if (pol > UDSP_POL_REVERSE)
		return UDSP_UNSUPPORTED;

	cmd_line_init(&cmd, UDSP_CMD_POLARITY, line);
	cmd.arg0 = (uint32_t)pol;

	return udsp_sync(s, &cmd, UDSP_SYNC_SLIC_MS, true);
}

enum udsp_status udsp_line_hook_query(udsp_t *s, unsigned line, bool *offhook)
{
	uint64_t pub;

	if (!s || !offhook || line >= s->cap.lines)
		return UDSP_UNSUPPORTED;
	if (atomic_load_explicit(&s->fatal, memory_order_acquire))
		return UDSP_FATAL;
	/* A dead line's cached level is whatever the bus last managed to read,
	 * so it must not be served as an answer. */
	if (atomic_load_explicit(&s->chan[line].dead, memory_order_acquire))
		return UDSP_FATAL;

	pub = atomic_load_explicit(&s->chan[line].hook_pub, memory_order_acquire);
	if (!(pub & UDSP_HOOK_PUB_VALID))
		return UDSP_TRANSIENT;

	*offhook = (pub & UDSP_HOOK_PUB_LEVEL) != 0;

	return UDSP_OK;
}

/* Its class is SYNC-SLIC and it keeps that lifecycle: transient before start
 * and while a job runs. Only its action is
 * engine-side, because the debounce it retimes is software, so its ack is the
 * tick ack rather than a register write. */
enum udsp_status udsp_line_hook_timing(udsp_t *s, unsigned line,
				       uint32_t flash_min_ms, uint32_t flash_max_ms,
				       uint32_t release_min_ms)
{
	enum udsp_status g = udsp_guard(s, line, true);
	struct udsp_cmd cmd;

	if (g != UDSP_OK)
		return g;
	if (flash_min_ms > flash_max_ms || flash_max_ms >= release_min_ms)
		return UDSP_UNSUPPORTED;

	cmd_line_init(&cmd, UDSP_CMD_HOOK_TIMING, line);
	cmd.arg0 = flash_min_ms;
	cmd.arg1 = flash_max_ms;
	cmd.arg2 = (int32_t)release_min_ms;

	return udsp_sync(s, &cmd, cmd_sync_tick_ms_get(), true);
}

/*
 * EN 300 659-1 puts the date and time first in a call-setup message,
 * and udsp.h promises the engine supplies it when the caller leaves the field
 * empty. It is filled here, on the caller's thread: the sequencer builds the
 * message on the engine tick, where a clock call does not belong.
 *
 * The classification in chan_cid_mwi reads number, name and blockinfo only, so
 * a filled date cannot turn a visual-indicator burst into a caller identity.
 */
static void cid_datetime_fill(struct udsp_cid *cid)
{
	struct tm tm;
	time_t now;

	if (cid->datetime[0])
		return;
	now = time(NULL);
	if (!localtime_r(&now, &tm))
		return;
	snprintf(cid->datetime, sizeof(cid->datetime), "%02d%02d%02d%02d",
		 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

enum udsp_status udsp_ring_start(udsp_t *s, unsigned line,
				 const struct udsp_ring *ring)
{
	enum udsp_status g = udsp_guard(s, line, true);
	struct udsp_cmd cmd;

	if (g != UDSP_OK)
		return g;
	if (!ring)
		return UDSP_UNSUPPORTED;
	/* A ring without caller identity needs no region; only the sequencer
	 * does. */
	if (ring->cid_present) {
		const struct udsp_region *r = udsp_region_active(s);

		if (!r)
			return UDSP_UNSUPPORTED;
		/*
		 * Refuse a record the break cannot deliver before the bell
		 * starts. chan_ring_start rings first and arms second, so a
		 * refusal taken there would leave the line ringing behind a
		 * sequence that can never complete.
		 */
		if (line_cidseq_budget_ms(UDSP_CID_TYPE1, &ring->cid, r,
					  ring->cid_wait_ms) >
		    LINE_CID_WINDOW_MAX_MS)
			return UDSP_UNSUPPORTED;
	}

	cmd_line_init(&cmd, UDSP_CMD_RING_START, line);
	cmd.u.ring = *ring;
	if (ring->cid_present)
		cid_datetime_fill(&cmd.u.ring.cid);

	return udsp_sync(s, &cmd, UDSP_SYNC_SLIC_MS, true);
}

enum udsp_status udsp_ring_stop(udsp_t *s, unsigned line)
{
	enum udsp_status g = udsp_guard(s, line, true);
	struct udsp_cmd cmd;

	if (g != UDSP_OK)
		return g;

	cmd_line_init(&cmd, UDSP_CMD_RING_STOP, line);

	return udsp_sync(s, &cmd, UDSP_SYNC_SLIC_MS, true);
}

enum udsp_status udsp_cid_send(udsp_t *s, unsigned line, enum udsp_cid_kind kind,
			       const struct udsp_cid *cid)
{
	enum udsp_status g = line_under_test(s, line) ? UDSP_TRANSIENT :
			     udsp_guard(s, line, false);
	struct udsp_cmd cmd;

	if (g != UDSP_OK)
		return g;
	if (!cid || kind > UDSP_CID_TYPE2)
		return UDSP_UNSUPPORTED;
	/* The sequence drives the linefeed, so it needs an initialised line
	 * even though its own ack is the tick ack. */
	if (!atomic_load_explicit(&s->started, memory_order_acquire))
		return UDSP_TRANSIENT;
	if (!udsp_region_active(s))
		return UDSP_UNSUPPORTED;

	cmd_line_init(&cmd, UDSP_CMD_CID_SEND, line);
	cmd.u.cid.cid = *cid;
	cid_datetime_fill(&cmd.u.cid.cid);
	cmd.u.cid.kind = (uint8_t)kind;

	return udsp_sync(s, &cmd, cmd_sync_tick_ms_get(), false);
}

/* The lamp op alone: the FSK visual indicator travels udsp_cid_send, exactly
 * as the uvoice core drives it. */
enum udsp_status udsp_mwi(udsp_t *s, unsigned line, bool on)
{
	enum udsp_status g = udsp_guard(s, line, true);
	struct udsp_cmd cmd;

	if (g != UDSP_OK)
		return g;
	if (!s->ops->mwi_lamp)
		return UDSP_UNSUPPORTED;

	cmd_line_init(&cmd, UDSP_CMD_MWI_LAMP, line);
	cmd.arg0 = on ? 1u : 0u;

	return udsp_sync(s, &cmd, UDSP_SYNC_SLIC_MS, true);
}

enum udsp_status udsp_tone_play(udsp_t *s, unsigned line, enum udsp_tone tone,
				uint32_t duration_ms)
{
	enum udsp_status g = udsp_guard(s, line, false);
	const struct udsp_region *r;
	struct udsp_cmd cmd;

	if (g != UDSP_OK)
		return g;
	if ((unsigned)tone >= UDSP_REGION_TONE_MAX)
		return UDSP_UNSUPPORTED;

	r = udsp_region_active(s);
	if (!r || !r->tone[tone].present)
		return UDSP_UNSUPPORTED;

	cmd_line_init(&cmd, UDSP_CMD_TONE_PLAY, line);
	cmd.arg0 = (uint32_t)tone;
	cmd.arg1 = duration_ms;

	return udsp_sync(s, &cmd, cmd_sync_tick_ms_get(), false);
}

enum udsp_status udsp_tone_stop(udsp_t *s, unsigned line)
{
	enum udsp_status g = udsp_guard(s, line, false);
	struct udsp_cmd cmd;

	if (g != UDSP_OK)
		return g;

	cmd_line_init(&cmd, UDSP_CMD_TONE_STOP, line);

	return udsp_sync(s, &cmd, cmd_sync_tick_ms_get(), false);
}

enum udsp_status udsp_cw_tone_play(udsp_t *s, unsigned line, unsigned variant,
				   uint32_t duration_ms)
{
	enum udsp_status g = udsp_guard(s, line, false);
	const struct udsp_region *r;
	struct udsp_cmd cmd;

	if (g != UDSP_OK)
		return g;
	if (variant > UDSP_CW_VARIANT_MAX)
		return UDSP_UNSUPPORTED;

	r = udsp_region_active(s);
	if (!r || !r->tone[UDSP_TONE_CALL_WAITING].present)
		return UDSP_UNSUPPORTED;

	cmd_line_init(&cmd, UDSP_CMD_CW_TONE_PLAY, line);
	cmd.arg0 = variant;
	cmd.arg1 = duration_ms;

	return udsp_sync(s, &cmd, cmd_sync_tick_ms_get(), false);
}

enum udsp_status udsp_digit_play(udsp_t *s, unsigned line, char digit,
				 uint32_t duration_ms)
{
	enum udsp_status g = udsp_guard(s, line, false);
	struct udsp_cmd cmd;

	if (g != UDSP_OK)
		return g;

	cmd_line_init(&cmd, UDSP_CMD_DIGIT_PLAY, line);
	cmd.arg0 = (uint32_t)(unsigned char)digit;
	cmd.arg1 = duration_ms ? duration_ms : UDSP_DIGIT_DEFAULT_MS;

	return udsp_sync(s, &cmd, cmd_sync_tick_ms_get(), false);
}

enum udsp_status udsp_chan_config(udsp_t *s, unsigned line,
				  const struct udsp_chan_cfg *cfg)
{
	enum udsp_status g = line_under_test(s, line) ? UDSP_TRANSIENT :
			     udsp_guard(s, line, false);
	struct udsp_cmd cmd;

	if (g != UDSP_OK)
		return g;
	if (!cfg)
		return UDSP_UNSUPPORTED;
	if (cfg->slic_gain_tx < UDSP_SLIC_GAIN_MIN ||
	    cfg->slic_gain_tx > UDSP_SLIC_GAIN_MAX ||
	    cfg->slic_gain_rx < UDSP_SLIC_GAIN_MIN ||
	    cfg->slic_gain_rx > UDSP_SLIC_GAIN_MAX)
		return UDSP_UNSUPPORTED;

	cmd_line_init(&cmd, UDSP_CMD_CHAN_CONFIG, line);
	cmd.u.chan = *cfg;

	return udsp_sync(s, &cmd, cmd_sync_tick_ms_get(), false);
}

/* --- media --------------------------------------------------------------- */

enum udsp_status udsp_stream_config(udsp_t *s, unsigned line, unsigned stream,
				    const struct udsp_media *media, int rtp_fd,
				    int rtcp_fd)
{
	if (line_under_test(s, line) || udsp_line_loop_armed(s, line))
		return UDSP_TRANSIENT;

	return stream_config(s, line, stream, media, rtp_fd, rtcp_fd);
}

enum udsp_status udsp_stream_start(udsp_t *s, unsigned line, unsigned stream)
{
	/* A stream configured before the arm would otherwise start over the
	 * loop, and the tick would mix both into the same downlink. */
	if (line_under_test(s, line) || udsp_line_loop_armed(s, line))
		return UDSP_TRANSIENT;

	return stream_start(s, line, stream);
}

enum udsp_status udsp_stream_stop(udsp_t *s, unsigned line, unsigned stream)
{
	return stream_stop(s, line, stream);
}

enum udsp_status udsp_stream_dtmf(udsp_t *s, unsigned line, unsigned stream,
				  char digit, uint32_t duration_ms)
{
	return stream_dtmf(s, line, stream, digit, duration_ms);
}

enum udsp_status udsp_stream_stats(udsp_t *s, unsigned line, unsigned stream,
				   struct udsp_media_stats *out)
{
	return stream_stats(s, line, stream, out);
}

enum udsp_status udsp_stream_jb_stats(udsp_t *s, unsigned line, unsigned stream,
				      struct udsp_jb_stats *out)
{
	return stream_jb_stats(s, line, stream, out);
}

enum udsp_status udsp_stream_diag(udsp_t *s, unsigned line, unsigned stream,
				  struct udsp_stream_diag *out)
{
	return stream_diag(s, line, stream, out);
}

uint32_t udsp_stream_gen(udsp_t *s, unsigned line, unsigned stream)
{
	return stream_gen(s, line, stream);
}

/* The canceller is a property of the line, so this one takes no stream index;
 * the read discipline is the one above (core/chan.c). */
enum udsp_status udsp_line_lec_stats(udsp_t *s, unsigned line,
				     struct udsp_lec_stats *out)
{
	return chan_lec_stats(s, line, out);
}

enum udsp_status udsp_line_lec_latch_clear(udsp_t *s, unsigned line)
{
	enum udsp_status st = udsp_guard(s, line, false);
	struct udsp_cmd cmd;

	if (st != UDSP_OK)
		return st;
	cmd_line_init(&cmd, UDSP_CMD_LEC_LATCH_CLEAR, line);

	return udsp_sync(s, &cmd, cmd_sync_tick_ms_get(), false);
}

/* LF[2:0] requested equals LFS[6:4] actual in an active or on-hook feed
 * (AN867 Table 4.29; the settled values 0x11, 0x22, 0x55, 0x66). */
static bool monitor_settled(uint8_t linefeed)
{
	uint8_t lo = linefeed & 0x7u;
	uint8_t hi = (linefeed >> 4) & 0x7u;

	if (lo != hi)
		return false;

	return lo == 1u || lo == 2u || lo == 5u || lo == 6u;
}

/*
 * FASTPATH: the reader half of the line-monitor publish (core/slic_thread.c),
 * the shape udsp_line_lec_stats uses. The fault fields come from the engine's
 * published word always, so they answer even before the first sample and on a
 * stale row (udsp.h). The measurement fields and their decode read the newest
 * reading; a stale row carries the previous one and its stamp.
 */
enum udsp_status udsp_line_monitor(udsp_t *s, unsigned line,
				   struct udsp_monitor *out)
{
	struct udsp_monitor_rec rec;
	const struct udsp_slic_monitor *m;
	uint32_t pub;

	if (!s || !out || line >= s->cap.lines)
		return UDSP_UNSUPPORTED;
	if (!s->ops->monitor_read)
		return UDSP_UNSUPPORTED;
	/* The record is untouched on a refusal, the dead line included. */
	if (atomic_load_explicit(&s->chan[line].dead, memory_order_acquire))
		return UDSP_FATAL;

	memset(out, 0, sizeof(*out));
	pub = atomic_load_explicit(&s->chan[line].fault_pub,
				  memory_order_acquire);
	out->fault_bits = pub & UDSP_FAULT_PUB_BITS;
	out->fault_open = (pub & UDSP_FAULT_PUB_OPEN) ? 1u : 0u;
	out->fault_held = (pub & UDSP_FAULT_PUB_HELD) ? 1u : 0u;
	if (!atomic_load_explicit(&s->monitor_valid[line],
				  memory_order_acquire))
		return UDSP_OK;

	monitor_snap_get(&s->monitor_snap[line], &rec);
	m = &rec.m;
	out->valid = 1u;
	out->stale = rec.stale;
	out->stamp_ms = rec.stamp_ms;
	out->vtr_mv = m->vtr_mv;
	out->vtip_mv = m->vtip_mv;
	out->vring_mv = m->vring_mv;
	out->vlong_mv = m->vlong_mv;
	out->vbat_mv = m->vbat_mv;
	out->vdc_mv = m->vdc_mv;
	out->itr_ua = m->itr_ua;
	out->itip_ua = m->itip_ua;
	out->iring_ua = m->iring_ua;
	out->ilong_ua = m->ilong_ua;
	out->p_hvic_mw = m->p_hvic_mw;
	out->vbat_tgt_mv = m->vbat_tgt_mv;
	out->vbat_irq_th_mv = m->vbat_irq_th_mv;
	out->pd_dcdc = m->pd_dcdc;
	out->dcdc_status = m->dcdc_status;
	out->lcrrtp = m->lcrrtp;
	out->linefeed = m->linefeed;
	out->dcdc_down = (m->pd_dcdc & (1u << 20)) ? 1u : 0u;
	out->ovlo = (m->dcdc_status & (1u << 22)) ? 1u : 0u;
	out->settled = monitor_settled(m->linefeed) ? 1u : 0u;
	out->voc_err = (m->lcrrtp & (1u << 3)) ? 1u : 0u;
	out->lcr = (m->lcrrtp & (1u << 1)) ? 1u : 0u;
	out->rtp = (m->lcrrtp & (1u << 0)) ? 1u : 0u;

	return UDSP_OK;
}

/* FASTPATH: one relaxed store. The engine picks the new mask up on its next
 * tick, so an arm or a disarm never waits for a tick boundary. */
void udsp_debug_set(udsp_t *s, uint32_t flags)
{
	if (!s)
		return;

	atomic_store_explicit(&s->debug, flags, memory_order_relaxed);
}

/* FASTPATH, one consumer thread (udsp.h): the ring is single producer and
 * single consumer, so a second reader would take records from the first. */
unsigned udsp_jb_trace_read(udsp_t *s, struct udsp_jb_trace *out, unsigned cap)
{
	unsigned n = 0;

	if (!s || !out)
		return 0;
	while (n < cap && jb_trace_pop(&s->trace, &out[n]))
		n++;

	return n;
}

/*
 * Test-only fault injection. The allocation failure of the arm is unreachable
 * otherwise, and the answer it produces is part of the contract. The value
 * names the ring allocation that fails inside one arm: 1 the first and 2 the
 * second, so a test reaches the path that frees the ring that succeeded. The
 * read happens on the allocation path alone, so an arm that finds the rings
 * still answers 0 with the variable set, which is what proves the second arm
 * allocated nothing. Unset, the value of every shipped run, disarms it.
 */
static unsigned pkt_trace_alloc_fail(void)
{
	const char *v = getenv("UDSP_TEST_PKT_TRACE_ALLOC_FAIL");

	if (!v || v[0] < '1' || v[0] > '2' || v[1])
		return 0;

	return (unsigned)(v[0] - '0');
}

static struct pkt_trace_ring *pkt_trace_ring_new(void)
{
	struct pkt_trace_ring *r = calloc(1, sizeof(*r));

	if (!r)
		return NULL;
	pkt_trace_init(r);
	/* The producers are the engine tick and the net thread, and neither may
	 * take a page fault, so the whole ring is resident before the pointer
	 * to it is published (core/rt.c rt_range_prefault). */
	rt_range_prefault(r, sizeof(*r));

	return r;
}

/* The caller holds ctl_lock. Both rings or neither: a producer that found one
 * of the pair missing would have to test the other one too. */
static int pkt_trace_alloc(struct udsp *s)
{
	unsigned fail = pkt_trace_alloc_fail();
	struct pkt_trace_ring *tx;
	struct pkt_trace_ring *rx;

	tx = fail == 1u ? NULL : pkt_trace_ring_new();
	rx = fail == 2u ? NULL : pkt_trace_ring_new();
	if (!tx || !rx) {
		free(tx);
		free(rx);

		return -ENOMEM;
	}
	atomic_store_explicit(&s->pkt_tx, tx, memory_order_release);
	atomic_store_explicit(&s->pkt_rx, rx, memory_order_release);

	return 0;
}

/*
 * The bit moves with a read-modify-write, so an arm and a disarm keep every
 * other flag of the mask. udsp_debug_set stores the mask whole and takes no
 * lock, so the two calls belong to the one control thread (udsp.h).
 */
static void debug_bit_set(struct udsp *s, uint_least32_t bit, bool on)
{
	if (!on) {
		atomic_fetch_and_explicit(&s->debug, ~bit,
					  memory_order_relaxed);
		return;
	}
	atomic_fetch_or_explicit(&s->debug, bit, memory_order_relaxed);
}

/* Control thread (udsp.h): the allocation and the prefault are why this is an
 * op of its own and not a flag of udsp_debug_set. */
int udsp_pkt_trace_enable(udsp_t *s, bool on)
{
	int rc = 0;

	if (!s)
		return -EINVAL;

	pthread_mutex_lock(&s->ctl_lock);
	if (on && !atomic_load_explicit(&s->pkt_tx, memory_order_relaxed))
		rc = pkt_trace_alloc(s);
	/* A failed arm leaves no ring behind, so the bit goes down with it,
	 * even where the caller raised it through udsp_debug_set first: the
	 * trace is off and the mask says so. */
	debug_bit_set(s, UDSP_DEBUG_PKT_TRACE, on && !rc);
	pthread_mutex_unlock(&s->ctl_lock);

	return rc;
}

/* FASTPATH, one consumer thread (udsp.h): each ring is single producer and
 * single consumer, so a second reader takes records from the first. It answers
 * what the rings hold and never reads the debug bit, because the drain that
 * follows a disarm is the one that empties them. */
unsigned udsp_pkt_trace_read(udsp_t *s, struct udsp_pkt_trace *out,
			     unsigned cap)
{
	struct pkt_trace_ring *r;
	unsigned n = 0;

	if (!s || !out)
		return 0;
	r = atomic_load_explicit(&s->pkt_tx, memory_order_acquire);
	while (r && n < cap && pkt_trace_pop(r, &out[n]))
		n++;
	r = atomic_load_explicit(&s->pkt_rx, memory_order_acquire);
	while (r && n < cap && pkt_trace_pop(r, &out[n]))
		n++;

	return n;
}

/*
 * Test-only fault injection on the pkt_trace_alloc_fail terms: 1 makes the
 * allocation of the arm fail. The read happens on the allocation path alone,
 * so an arm that finds the ring still answers 0 with the variable set.
 */
static bool lec_capture_alloc_fail(void)
{
	const char *v = getenv("UDSP_TEST_LEC_CAPTURE_ALLOC_FAIL");

	return v && v[0] == '1' && !v[1];
}

/*
 * The depth of the ring: UDSP_LEC_CAPTURE_SLOTS, or a power of two from 2 up
 * to it in UDSP_TEST_LEC_CAPTURE_SLOTS. A test can thus fill the ring in a few
 * ticks. The allocation path reads it, once per handle. Any other value leaves
 * the production depth.
 */
static unsigned lec_capture_slots_get(void)
{
	const char *v = getenv("UDSP_TEST_LEC_CAPTURE_SLOTS");
	char *end = NULL;
	unsigned long n;

	if (!v || *v < '0' || *v > '9')
		return UDSP_LEC_CAPTURE_SLOTS;
	errno = 0;
	n = strtoul(v, &end, 10);
	if (errno || *end || n < 2u || n > UDSP_LEC_CAPTURE_SLOTS ||
	    (n & (n - 1u)))
		return UDSP_LEC_CAPTURE_SLOTS;

	return (unsigned)n;
}

/* The caller holds ctl_lock. */
static int lec_capture_alloc(struct udsp *s)
{
	unsigned slots = lec_capture_slots_get();
	size_t bytes = lec_capture_ring_bytes(slots);
	struct lec_capture_ring *r;

	if (lec_capture_alloc_fail())
		return -ENOMEM;
	r = malloc(bytes);
	if (!r)
		return -ENOMEM;
	lec_capture_ring_init(r, slots);
	/* The tick is the producer and may not take a page fault, so the ring
	 * is resident before the pointer is published. */
	rt_range_prefault(r, bytes);
	atomic_store_explicit(&s->lec_cap, r, memory_order_release);

	return 0;
}

/*
 * The tick barrier of a disarm. The command drain runs at the top of the tick,
 * before any line runs its canceller, and it acknowledges this command at once
 * (core/chan.c chan_cmd_drain). Every tick before it has finished. The lines
 * of the same tick load the mask after the drain took the command, so they
 * read the clear bit. A handle with no ring has no producer to wait for, and
 * neither has an engine that never started, stopped cleanly or is parked.
 *
 * A stop that timed out clears engine_live too, but its tick thread did not
 * join. That thread can still publish a record it claimed, and no command
 * reaches it, so the call cannot show that the producer finished. The count is
 * read after engine_live, whose release store follows the increment.
 *
 * udsp_sync_locked answers TRANSIENT for a full command ring and for an
 * expired wait alike. Only commands whose wait expired fill the ring, so both
 * say that the tick did not take a command in time, and both answer
 * -ETIMEDOUT.
 */
static int lec_capture_tick_wait(struct udsp *s)
{
	struct udsp_cmd cmd;
	uint64_t timeouts;

	if (!atomic_load_explicit(&s->lec_cap, memory_order_relaxed))
		return 0;
	if (engine_guard(s) != UDSP_OK) {
		timeouts = atomic_load_explicit(&s->stop_timeouts,
						memory_order_relaxed);
		return timeouts ? -ETIMEDOUT : 0;
	}
	memset(&cmd, 0, sizeof(cmd));
	cmd.type = UDSP_CMD_TICK_SYNC;
	if (udsp_sync_locked(s, &cmd, cmd_sync_tick_ms_get()) != UDSP_OK)
		return -ETIMEDOUT;

	return 0;
}

/* Control thread (udsp.h), on the udsp_pkt_trace_enable terms. The disarm
 * holds ctl_lock through the barrier, as every SYNC-TICK op holds it through
 * its post and wait. */
int udsp_lec_capture_enable(udsp_t *s, bool on)
{
	int rc = 0;

	if (!s)
		return -EINVAL;

	pthread_mutex_lock(&s->ctl_lock);
	if (on && !atomic_load_explicit(&s->lec_cap, memory_order_relaxed))
		rc = lec_capture_alloc(s);
	debug_bit_set(s, UDSP_DEBUG_LEC_CAPTURE, on && !rc);
	if (!on)
		rc = lec_capture_tick_wait(s);
	pthread_mutex_unlock(&s->ctl_lock);

	return rc;
}

/* FASTPATH, one consumer thread (udsp.h), on the udsp_pkt_trace_read
 * terms. */
unsigned udsp_lec_capture_read(udsp_t *s, struct udsp_lec_capture *out,
			       unsigned cap)
{
	struct lec_capture_ring *r;
	unsigned n = 0;

	if (!s || !out)
		return 0;
	r = atomic_load_explicit(&s->lec_cap, memory_order_acquire);
	while (r && n < cap && lec_capture_ring_pop(r, &out[n]))
		n++;

	return n;
}

enum udsp_status udsp_fax_profile(udsp_t *s, unsigned line, unsigned stream,
				  enum udsp_fax_mode mode, uint8_t codec,
				  uint8_t codec_pt)
{
	return stream_fax_profile(s, line, stream, mode, codec, codec_pt);
}

enum udsp_status udsp_conf(udsp_t *s, unsigned line, enum udsp_conf_mode mode)
{
	return stream_conf(s, line, mode);
}

/* A line a metallic test drives, or a session or a call already owns, cannot
 * take a loop: the plug-in holds the wire for the test, and a ring or a burst
 * owns the feed the loop would play into. */
static bool loop_line_blocked(const struct udsp *s, unsigned line)
{
	return line_under_test(s, line) || udsp_line_busy(s, line);
}

/*
 * SYNC-TICK. The refusal order is the one udsp.h states: the arguments, then
 * the lifecycle guard on the line and on the peer, then the exclusions a mode
 * that plays audio takes. OFF disarms whatever the line carries, so it reads
 * neither peer nor delay and takes no exclusion.
 */
enum udsp_status udsp_line_loop(udsp_t *s, unsigned line,
				enum udsp_loop_mode mode, unsigned peer,
				uint32_t delay_ms)
{
	enum udsp_status st;

	if (!s || line >= s->cap.lines || mode > UDSP_LOOP_PEER)
		return UDSP_UNSUPPORTED;
	/* Ahead of every guard below, because udsp_line_busy indexes the
	 * channel array with no bound of its own. */
	if (mode != UDSP_LOOP_OFF && delay_ms > UDSP_LOOP_DELAY_MAX_MS)
		return UDSP_UNSUPPORTED;
	if (mode == UDSP_LOOP_PEER && (peer >= s->cap.lines || peer == line))
		return UDSP_UNSUPPORTED;
	st = udsp_guard(s, line, false);
	if (st != UDSP_OK)
		return st;
	if (mode == UDSP_LOOP_PEER) {
		st = udsp_guard(s, peer, false);
		if (st != UDSP_OK)
			return st;
	}
	if (mode != UDSP_LOOP_OFF && loop_line_blocked(s, line))
		return UDSP_TRANSIENT;
	if (mode == UDSP_LOOP_PEER && loop_line_blocked(s, peer))
		return UDSP_TRANSIENT;

	return stream_line_loop(s, line, mode, peer, delay_ms);
}

bool udsp_line_busy(const struct udsp *s, unsigned line)
{
	if (stream_line_active(s, line))
		return true;

	return atomic_load_explicit(&s->chan[line].owned,
				    memory_order_acquire);
}

/*
 * SLOW. The plug-in drives the line for the whole test, seconds on real
 * hardware, so the op is refused on a line that carries a call, a ring session,
 * a caller identity burst or a delayed loop, and while another SLOW job holds
 * the slic thread. The guard adds the SYNC-SLIC lifecycle: the part is
 * initialised by udsp_start, so a test before it is transient and a dead line
 * is fatal.
 *
 * The line can take a call between this test and the job step, so the slic
 * thread tests the same conditions again and completes -EBUSY instead.
 */
enum udsp_status udsp_line_test(udsp_t *s, unsigned line,
				enum udsp_line_test test, struct udsp_job *job)
{
	struct slic_job j = { .kind = SLIC_JOB_LINE_TEST,
			      .line = (uint8_t)line, .test = (uint8_t)test };
	enum udsp_status g;

	if (!s || !job || test > UDSP_TEST_LOOP)
		return UDSP_UNSUPPORTED;
	if (!s->ops->mlt_run)
		return UDSP_UNSUPPORTED;
	g = udsp_guard(s, line, true);
	if (g != UDSP_OK)
		return g;
	if (udsp_line_busy(s, line) || udsp_line_loop_armed(s, line))
		return UDSP_TRANSIENT;

	return udsp_job_post(s, &j, job);
}

/*
 * FASTPATH: no lock and no cross-thread wait. The bytes are the plug-in's own
 * and libudsp never decodes them (udsp.h: the reserved interface). A cap below
 * the record length copies what fits and still reports the whole length, so
 * the caller learns the buffer it needs.
 *
 * The generation is read on both sides of the copy. The slic thread clears it
 * when a new test starts, so a second read that still finds the first value
 * proves the record copied out is the one the caller asked about.
 */
enum udsp_status udsp_line_test_result(udsp_t *s, unsigned line, void *buf,
				       size_t cap, size_t *out_len)
{
	struct udsp_slic_mlt_result rec;
	struct udsp_line_test_slot *slot;
	uint_least32_t gen;
	size_t len;

	if (out_len)
		*out_len = 0;
	if (!s || line >= s->cap.lines || (!buf && cap))
		return UDSP_UNSUPPORTED;

	slot = &s->line_test[line];
	gen = atomic_load_explicit(&slot->gen, memory_order_acquire);
	if (!gen)
		return UDSP_UNSUPPORTED;

	line_test_snap_get(&slot->snap, &rec);
	if (atomic_load_explicit(&slot->gen, memory_order_acquire) != gen)
		return UDSP_UNSUPPORTED;

	len = rec.len;
	if (buf && cap)
		memcpy(buf, rec.data, len < cap ? len : cap);
	if (out_len)
		*out_len = len;

	return UDSP_OK;
}

/* --- capability and counters --------------------------------------------- */

const struct udsp_cap *udsp_cap(udsp_t *s)
{
	return s ? &s->cap : NULL;
}

/* FASTPATH: no lock. The stall figure is republished by the watchdog once a
 * second and at every engine teardown, which is the resolution a diagnostic
 * counter needs; everything else here is an emitter or control-side atomic. */
void udsp_counters(udsp_t *s, struct udsp_counters *out)
{
	if (!s || !out)
		return;

	memset(out, 0, sizeof(*out));
	out->events_sent = udsp_emitter_sent(&s->emitter);
	out->events_dropped = udsp_emitter_dropped(&s->emitter);
	out->op_errors = atomic_load_explicit(&s->op_errors, memory_order_relaxed);
	out->tdm_stalls = atomic_load_explicit(&s->tdm_stalls,
					       memory_order_relaxed);
	out->holds = atomic_load_explicit(&s->holds, memory_order_relaxed);
	out->capture_drops = atomic_load_explicit(&s->capture_drops,
						  memory_order_relaxed);
	out->trim_drops = atomic_load_explicit(&s->trim_drops,
					       memory_order_relaxed);
	out->playback_expired = atomic_load_explicit(&s->playback_expired,
						     memory_order_relaxed);
	out->hook_gaps = atomic_load_explicit(&s->hook_gaps,
					      memory_order_relaxed);
	out->hook_moved_gaps = atomic_load_explicit(&s->hook_moved_gaps,
						    memory_order_relaxed);
	out->hook_gap_max_ms = atomic_load_explicit(&s->hook_gap_max_ms,
						    memory_order_relaxed);
}

enum udsp_status udsp_engine_diag(udsp_t *s, struct udsp_engine_diag *out)
{
	struct engine_stats st;

	if (out)
		memset(out, 0, sizeof(*out));
	if (!s || !out)
		return UDSP_UNSUPPORTED;
	if (!atomic_load_explicit(&s->engine_diag_valid, memory_order_acquire))
		return UDSP_UNSUPPORTED;
	engine_diag_snap_get(&s->engine_diag, &st);
	if (!atomic_load_explicit(&s->engine_diag_valid, memory_order_acquire))
		return UDSP_UNSUPPORTED;

	out->serial = st.serial;
	out->ticks = st.ticks;
	memcpy(out->interval, st.interval, sizeof(out->interval));
	out->interval_min_us = st.interval_min_us;
	out->interval_max_us = st.interval_max_us;
	out->capture_stalls = st.capture_stalls;
	out->delay_stalls = st.delay_stalls;
	out->xruns = st.xruns;
	out->holds = st.holds;
	out->capture_drops = st.capture_drops;
	out->trim_drops = st.trim_drops;
	out->playback_expired = st.playback_expired;
	out->recover_failures = st.recover_failures;
	out->read_errors = st.read_errors;
	out->write_errors = st.write_errors;
	out->cal_restarts = st.cal_restarts;
	out->delay_ref = st.delay_ref;
	out->loop_ref = st.loop_ref;
	out->delay_drift_min = st.delay_drift_min;
	out->delay_drift_max = st.delay_drift_max;
	out->granted_periods = st.granted_periods;
	out->period_us = st.period_us;
	out->running = st.running;
	out->stopped_fatal = st.stopped_fatal;
	out->delay_calibrated = st.delay_calibrated;

	return UDSP_OK;
}

/* --- open and close ------------------------------------------------------ */

/* lines is fixed at open to min(SLIC channels, populated ports); a tdm
 * geometry too narrow for it fails the open instead of reducing it
 * silently. Valid once the attach job captured attach_channels. */
static unsigned udsp_lines_count(const struct udsp *s)
{
	unsigned lines = s->attach_channels;

	if (lines > s->cfg.populated_ports)
		lines = s->cfg.populated_ports;

	return lines;
}

static void udsp_cap_fill(struct udsp *s)
{
	/* Captured by the attach job on the slic thread. */
	const char *part = s->part_name[0] ? s->part_name : NULL;
	unsigned lines = udsp_lines_count(s);

	snprintf(s->cap.backend, sizeof(s->cap.backend), "%s", s->ops->name);
	snprintf(s->cap.slic, sizeof(s->cap.slic), "%s",
		 part ? part : s->ops->name);
	snprintf(s->cap.version, sizeof(s->cap.version), "%s %s",
		 UDSP_VERSION_PREFIX, s->ops->name);
	s->cap.lines = (uint8_t)lines;
	s->cap.streams_per_chan = (uint8_t)UDSP_STREAMS_PER_CHAN;
	/* CONF and LOOP are library halves; the hal bit of each stays the
	 * backend's own gate until its op exists. */
	s->cap.features = UDSP_FEAT_POLARITY | UDSP_FEAT_G726 | UDSP_FEAT_CONF |
			  UDSP_FEAT_LOOP | UDSP_FEAT_G729 |
			  UDSP_FEAT_DTMF_RELAY | UDSP_FEAT_DTMF_PLAYOUT;
	if (s->srtp_ready)
		s->cap.features |= UDSP_FEAT_SRTP;
	if (s->ops->mwi_lamp)
		s->cap.features |= UDSP_FEAT_MWI_LAMP;
	if (s->ops->mlt_run)
		s->cap.features |= UDSP_FEAT_LINE_TEST;
	s->cap.codecs = (1u << UDSP_SEL_PCMU) | (1u << UDSP_SEL_PCMA) |
			(1u << UDSP_SEL_G726_16) | (1u << UDSP_SEL_G726_24) |
			(1u << UDSP_SEL_G726_32) | (1u << UDSP_SEL_G726_40) |
			(1u << UDSP_SEL_G729);
#ifdef HAVE_OPUS
	s->cap.codecs |= 1u << UDSP_SEL_OPUS;
#endif
	/* The settle already ran, so the bits report the line that stands: the
	 * config asked for 16 kHz, the TDM granted it and the plug-in reported
	 * the capability (udsp.h UDSP_FEAT_WIDEBAND). An 8 kHz line cannot
	 * carry the G.722 band, so the backend must not offer G.722 on it.
	 * udsp_stream_config still takes G.722 at 8 kHz, through the resampler
	 * pair. */
	if (s->rate == UDSP_SAMPLE_RATE_WB) {
		s->cap.features |= UDSP_FEAT_WIDEBAND;
		s->cap.codecs |= 1u << UDSP_SEL_G722;
	}
	/* Rule 2 of core/fax_plugin.h: the feature bit and the codec bit are
	 * one capability, because the backend offers the upgrade on the first
	 * and accepts the answer on the second. */
	if (s->fax_ops) {
		s->cap.features |= UDSP_FEAT_T38;
		s->cap.codecs |= 1u << UDSP_SEL_T38;
	}
}

static int udsp_fds_init(struct udsp *s)
{
	s->slic_cmd_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	s->slic_tick_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (s->slic_cmd_fd < 0 || s->slic_tick_fd < 0)
		return -errno;
	if (cmd_ack_init(&s->ctl_ack))
		return -EMFILE;

	return cmd_ack_init(&s->slic_ack);
}

static int udsp_host_open(struct udsp *s)
{
	struct slic_host_cfg hc;
	char err[SLIC_PLUGIN_ERR_MAX];
	int rc;

	if (!s->cfg.slic_plugin[0]) {
		/* The in-tree fake takes a NULL host: CI and the cli selftest
		 * run where no spidev exists. */
		s->ops = udsp_slic_fake_descriptor.ops;
		return 0;
	}

	memset(&hc, 0, sizeof(hc));
	hc.spidev = s->cfg.spidev;
	hc.gpio_reset = s->cfg.gpio_reset;
	hc.gpio_interrupt = s->cfg.gpio_interrupt;
	/* The part of the MT7987 boards answers only at mode 3 with per-byte
	 * chip select (measured on silicon). */
	hc.spi_mode = SPI_MODE_3;
	hc.cs_per_byte = true;
	/* Zero is the plug-in's "profile default" (slic.h), so it passes
	 * through untouched. */
	hc.pcm_offset_bits = s->cfg.pcm_offset_bits;
	/* The ask as it stands at host open: a TDM that already refused the
	 * rate has degraded it by now, and a plug-in that refuses it clears
	 * the fact again in the settle. */
	hc.pcm_wideband = s->rate == UDSP_SAMPLE_RATE_WB ? 1u : 0u;
	hc.log = s->cfg.log ? udsp_host_log : NULL;
	hc.log_ctx = s;
	rc = slic_host_open(&hc, &s->host);
	if (rc) {
		udsp_log(s, UDSP_LOG_ERR, "spidev %s: %s",
			 s->cfg.spidev[0] ? s->cfg.spidev : "(none)",
			 strerror(-rc));
		return rc;
	}

	rc = slic_plugin_load(s->cfg.slic_plugin, &s->plugin, err, sizeof(err));
	if (rc) {
		udsp_log(s, UDSP_LOG_ERR, "slic plug-in: %s", err);
		return rc;
	}
	s->ops = slic_plugin_ops(s->plugin);

	return s->ops ? 0 : -ENOSYS;
}

/*
 * The T.38 gateway, beside the SLIC host and before the capability record:
 * see rule 2 of core/fax_plugin.h. A gateway that will not load costs the
 * T.38 capability alone, so its reason is a warning and the open goes on.
 */
static void udsp_fax_open(struct udsp *s)
{
	char err[FAX_PLUGIN_ERR_MAX];

	if (udsp_fax_test_ops) {
		s->fax_ops = udsp_fax_test_ops;
		return;
	}
	if (!s->cfg.fax_plugin[0])
		return;
	if (fax_plugin_load(s->cfg.fax_plugin, &s->fax_plugin, err,
			    sizeof(err))) {
		udsp_op_error(s);
		udsp_log(s, UDSP_LOG_WARN, "fax plug-in: %s", err);
		return;
	}
	s->fax_ops = fax_plugin_ops(s->fax_plugin);
}

static int udsp_reset_release(struct udsp *s)
{
	const struct udsp_slic_host *h;
	struct timespec nap = {
		.tv_sec = UDSP_RESET_SETTLE_MS / 1000,
		.tv_nsec = (long)(UDSP_RESET_SETTLE_MS % 1000) * 1000000L,
	};
	struct timespec hold = {
		.tv_sec = 0,
		.tv_nsec = (long)UDSP_RESET_HOLD_MS * 1000000L,
	};
	int rc;

	if (!s->host)
		return 0;

	h = slic_host_hooks(s->host);
	/* ctrl_reset in the plug-in is active low, so releasing it is a 1;
	 * the canonical name maps to the board's sysfs export. A part still
	 * in reset answers nothing, so a failed write fails the open instead
	 * of probing a dead bus.
	 *
	 * The pin is asserted before it is released. A cold boot holds it low
	 * already, but a warm restart of the daemon finds it high from the
	 * previous run, and a release alone would then re-initialise a part
	 * that still holds that run's registers and RAM.
	 *
	 * Skyworks guidance reads: "a reboot needs RSTb held low for
	 * at least 32 PCLK cycles with PCLK running, and releasing reset
	 * without that can leave the DC-DC MOSFET gate drive stuck high". The
	 * startup order leaves the PCM clock running here, so the hold below
	 * runs against a live clock. The
	 * engine never pulses reset against a dead clock. A clock fault holds
	 * the line open instead. Only a fresh udsp_open, whose clock is
	 * running, resets the part. */
	rc = h->gpio_set(h->ctx, "slic_reset", 0);
	if (rc) {
		udsp_log(s, UDSP_LOG_ERR, "slic_reset assert: %s", strerror(-rc));
		return rc;
	}
	while (nanosleep(&hold, &hold) < 0 && errno == EINTR)
		continue;
	rc = h->gpio_set(h->ctx, "slic_reset", 1);
	if (rc) {
		udsp_log(s, UDSP_LOG_ERR, "slic_reset release: %s",
			 strerror(-rc));
		return rc;
	}

	/* A short settle is worse than none: the probe's --settle default is
	 * what the part needs, so an interrupted sleep is resumed. */
	while (nanosleep(&nap, &nap) < 0 && errno == EINTR)
		continue;

	return 0;
}

static int udsp_attach_run(struct udsp *s)
{
	struct slic_job j;
	int32_t status = 0;
	uint32_t token;
	int rc;

	memset(&j, 0, sizeof(j));
	j.kind = SLIC_JOB_ATTACH;
	token = j.token = ++s->ctl_token;
	j.ack = &s->ctl_ack;
	cmd_ack_arm(&s->ctl_ack, token);
	if (!slic_jobq_push(&s->jobs, &j))
		return -ENOSPC;
	atomic_fetch_add_explicit(&s->job_inflight, 1u, memory_order_acq_rel);
	slic_wake(s);

	rc = cmd_ack_wait(&s->ctl_ack, token, UDSP_ATTACH_WAIT_MS, &status);

	return rc ? rc : status;
}

static struct udsp *udsp_alloc(const struct udsp_config *cfg, int event_fd)
{
	struct udsp *s = calloc(1, sizeof(*s));

	if (!s)
		return NULL;

	s->cfg = *cfg;
	/* The requested rate until the settle, the settled one after it; the
	 * config copy carries it because the engine reopens the TDM at every
	 * udsp_start. */
	s->rate = rate_normalise(cfg->sample_rate);
	s->frame = s->rate / 100u;
	s->cfg.sample_rate = (uint16_t)s->rate;
	s->event_fd = event_fd;
	s->event_dup = -1;
	/* Every descriptor member starts at -1. A calloc'd zero is descriptor 0,
	 * which the caller owns, and an open that fails before the emitter is
	 * initialised would close it on the way out. */
	s->slic_cmd_fd = -1;
	s->slic_tick_fd = -1;
	s->slic_irq_fd = -1;
	s->ctl_ack.efd = -1;
	s->slic_ack.efd = -1;
	s->slic_thread.done_fd = -1;
	s->emitter.fd = -1;
	s->emitter.wake_fd = -1;
	s->emitter.thread.done_fd = -1;
	s->net.epoll_fd = -1;
	s->net.cmd_fd = -1;
	s->net.ack.efd = -1;
	s->net.thread.done_fd = -1;
	atomic_init(&s->engine_pub, NULL);
	atomic_init(&s->engine_diag_valid, false);
	atomic_init(&s->engine_diag.idx, 0u);
	atomic_init(&s->engine_diag.readers, 0u);
	atomic_init(&s->lines, 0u);
	for (unsigned i = 0; i < UDSP_LINE_MAX; i++) {
		atomic_init(&s->lec_ready[i], false);
		atomic_init(&s->lec_delay[i], 0u);
	}
	atomic_init(&s->lec_loop, 0u);
	atomic_init(&s->ack_drop_op, 0u);
	atomic_init(&s->prep_fail, 0u);
	atomic_init(&s->debug, 0u);
	jb_trace_init(&s->trace);
	atomic_init(&s->pkt_tx, NULL);
	atomic_init(&s->pkt_rx, NULL);
	atomic_init(&s->lec_cap, NULL);
	atomic_init(&s->fatal, false);
	atomic_init(&s->started, false);
	atomic_init(&s->engine_live, false);
	atomic_init(&s->dead, false);
	atomic_init(&s->op_errors, 0u);
	atomic_init(&s->stop_timeouts, 0u);
	atomic_init(&s->tdm_stalls, 0u);
	atomic_init(&s->holds, 0u);
	atomic_init(&s->capture_drops, 0u);
	atomic_init(&s->trim_drops, 0u);
	atomic_init(&s->playback_expired, 0u);
	atomic_init(&s->hook_gaps, 0u);
	atomic_init(&s->hook_moved_gaps, 0u);
	atomic_init(&s->hook_gap_max_ms, 0u);
	atomic_init(&s->job_inflight, 0u);
	atomic_init(&s->line_test_jobs, 0u);
	atomic_init(&s->recover_jobs, 0u);
	atomic_init(&s->recover_ack_drop, false);
	atomic_init(&s->slic_armed, false);
	atomic_init(&s->slic_policy, -1);
	atomic_init(&s->slic_wants_tick, true);
	atomic_init(&s->slic_mode, SLIC_MODE_POLL);
	atomic_init(&s->slic_quit, false);
	atomic_init(&s->region_idx, 0u);
	atomic_init(&s->region_valid, false);
	atomic_init(&s->clock.tick, 0u);
	atomic_init(&s->clock.mono_us, 0u);
	atomic_init(&s->clock.paced, false);
	pthread_mutex_init(&s->ctl_lock, NULL);
	pthread_mutex_init(&s->engine_lock, NULL);
	cmd_ring_init(&s->ctl_ring);
	slic_ring_init(&s->ring);
	slic_up_ring_init(&s->up);
	slic_jobq_init(&s->jobs);

	return s;
}

/* The enum crosses the ABI, which is why this is not the cli's string
 * mapper: the cap carries a number, the log and the tool name it. */
static uint8_t udsp_rt_policy_word(int policy)
{
	switch (policy) {
	case SCHED_FIFO:
		return UDSP_RT_POLICY_FIFO;
	case SCHED_RR:
		return UDSP_RT_POLICY_RR;
	case -1:
		return UDSP_RT_POLICY_UNKNOWN;
	default:
		return UDSP_RT_POLICY_OTHER;
	}
}

/* After net_start, so the net thread's policy exists; from the snapshot
 * udsp_open took under engine_lock, whose rt is settled because the worker
 * posts ready after rt_apply. */
static void udsp_cap_rt_fill(struct udsp *s, const struct engine_stats *st)
{
	const struct rt_report *rt = &st->rt;

	s->cap.rt_policy = udsp_rt_policy_word(rt->policy);
	s->cap.rt_net_policy = udsp_rt_policy_word(s->net.thread.policy);
	s->cap.rt_mlock = rt->mlock_applied;
	s->cap.rt_cpu = (int16_t)rt->cpu;
	s->cap.rt_prio = rt->prio;
	s->cap.rt_sched_errno = rt->sched_errno;
	s->cap.rt_mlock_errno = rt->mlock_errno;
	s->cap.rt_affinity_errno = rt->affinity_errno;
}

static const char *udsp_rt_policy_name(uint8_t policy)
{
	switch (policy) {
	case UDSP_RT_POLICY_FIFO:
		return "fifo";
	case UDSP_RT_POLICY_RR:
		return "rr";
	case UDSP_RT_POLICY_OTHER:
		return "other";
	default:
		return "unknown";
	}
}

void udsp_rt_line_format(const struct udsp_cap *cap, int engine_cpu,
			 uint8_t slic_policy, enum udsp_log_level *level,
			 char *buf, size_t len)
{
	bool refused = cap->rt_sched_errno || cap->rt_mlock_errno ||
		       cap->rt_affinity_errno;
	char ask[64], cpu[64], lock[64];

	if (cap->rt_sched_errno)
		snprintf(ask, sizeof(ask), "fifo %d refused: %s", cap->rt_prio,
			 strerror(cap->rt_sched_errno));
	else if (cap->rt_prio > 0)
		snprintf(ask, sizeof(ask), "fifo %d granted", cap->rt_prio);
	else
		snprintf(ask, sizeof(ask), "not requested");
	if (cap->rt_affinity_errno)
		snprintf(cpu, sizeof(cpu), "cpu %d refused: %s", engine_cpu,
			 strerror(cap->rt_affinity_errno));
	else if (cap->rt_cpu >= 0)
		snprintf(cpu, sizeof(cpu), "cpu %d", cap->rt_cpu);
	else
		snprintf(cpu, sizeof(cpu), "unpinned");
	if (cap->rt_mlock_errno)
		snprintf(lock, sizeof(lock), "mlock refused: %s",
			 strerror(cap->rt_mlock_errno));
	else
		snprintf(lock, sizeof(lock), "mlock %s",
			 cap->rt_mlock ? "yes" : "no");

	*level = refused ? UDSP_LOG_WARN : UDSP_LOG_INFO;
	snprintf(buf, len, "open: rt %s, engine %s, net %s, slic %s, %s, %s",
		 ask, udsp_rt_policy_name(cap->rt_policy),
		 udsp_rt_policy_name(cap->rt_net_policy),
		 udsp_rt_policy_name(slic_policy), cpu, lock);
}

/* The slic thread's policy is not in the cap: the thread moves it around
 * every SLOW vendor op (slic_thread.c slic_slow_enter), so a cap field would
 * name one instant of a value that changes. The log names the instant of the
 * open, after the attach job ran and restored it. */
static void udsp_open_rt_log(struct udsp *s)
{
	enum udsp_log_level level;
	char line[UDSP_LOG_MAX];
	int policy = atomic_load_explicit(&s->slic_policy, memory_order_acquire);

	udsp_rt_line_format(&s->cap, s->cfg.engine_cpu,
			    udsp_rt_policy_word(policy), &level, line,
			    sizeof(line));
	udsp_log(s, level, "%s", line);
}

/*
 * What the open settled on, in four records so that two plug-in paths and
 * three device names never truncate one another. The fourth is the real-time
 * outcome, and the only one that can be a WARN, because a cgroup EPERM has no
 * other report.
 */
static void udsp_open_log(struct udsp *s, const struct engine_stats *st)
{
	udsp_log(s, UDSP_LOG_INFO,
		 "open: tdm %s %s %s, %u ch, %u Hz, %u frames/period, prefill %u",
		 s->cfg.tdm_backend,
		 s->cfg.tdm_playback[0] ? s->cfg.tdm_playback : "-",
		 s->cfg.tdm_capture[0] ? s->cfg.tdm_capture : "-",
		 (unsigned)st->channels, s->rate, (unsigned)st->frames_per_period,
		 (unsigned)st->prefill_periods);
	udsp_log(s, UDSP_LOG_INFO,
		 "open: slic %s %s, %u ch, %u line(s) of %u port(s), %s",
		 s->cap.backend, s->cap.slic, s->attach_channels, s->cap.lines,
		 s->cfg.populated_ports,
		 s->slic_irq_fd >= 0 ? "interrupt edge" : "level poll");
	udsp_log(s, UDSP_LOG_INFO, "open: slic plug-in %s, fax plug-in %s",
		 s->cfg.slic_plugin[0] ? s->cfg.slic_plugin : "(fake)",
		 s->fax_ops ? s->cfg.fax_plugin : "none");
	udsp_open_rt_log(s);
}

/* The refusals that come before a handle exists take the caller's sink
 * straight from the configuration. */
static void cfg_log_err(const struct udsp_config *cfg, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void cfg_log_err(const struct udsp_config *cfg, const char *fmt, ...)
{
	char buf[128];
	va_list ap;

	if (!cfg || !cfg->log)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	cfg->log(cfg->log_ctx, UDSP_LOG_ERR, buf);
}

udsp_t *udsp_open(const struct udsp_config *cfg, int event_fd)
{
	const char *refused = cfg_refused(cfg);
	struct engine_stats st;
	struct udsp *s;
	int rc;

	if (refused) {
		cfg_log_err(cfg, "the configuration is refused: %s", refused);
		return NULL;
	}
	if (event_fd < 0) {
		cfg_log_err(cfg, "no event descriptor");
		return NULL;
	}
	if (atomic_exchange_explicit(&udsp_live, true, memory_order_acq_rel)) {
		cfg_log_err(cfg, "a handle is open already in this process");
		return NULL;
	}

	s = udsp_alloc(cfg, event_fd);
	if (!s) {
		atomic_store_explicit(&udsp_live, false, memory_order_release);
		cfg_log_err(cfg, "out of memory");
		return NULL;
	}
	rc = udsp_fds_init(s);
	if (rc) {
		udsp_log(s, UDSP_LOG_ERR, "control descriptors: %s",
			 strerror(-rc));
		goto fail;
	}
	/* The caller configured the region, so a parse failure fails the open:
	 * a silent fallback would run the wrong tone plan. */
	if (cfg->region[0] && udsp_region_open_parse(s, cfg->region))
		goto fail;
	s->max_ticks = max_ticks_get();
	s->dead_retry_ms = dead_retry_ms_get();
	/* The lost-end bound of a rendered telephone-event is read once, and
	 * this is the control thread, where a getenv is safe. Every later read
	 * belongs to the engine tick and takes the cached value. */
	s->tev_play_timeout_ms = telev_play_timeout_ms();
	/* The handler works without it, only less well on a stack overflow, so
	 * a failure is counted and the open continues. */
	if (udsp_thread_sigstack_install(&s->sig_stack, &s->sig_stack_prev))
		udsp_op_error(s);
	else
		s->sig_stack_live = true;

	/* libsrtp initialises once per process and yields while it runs, so it
	 * belongs on the control thread before any stream can ask for a session
	 * (libuvsrtp). A failure only costs the SRTP capability: a stream that
	 * asks for a suite is then refused, and one that does not still runs. */
	s->srtp_ready = uvsrtp_init() == 0;
	/* The emitter writes libudsp's own descriptor from here on. */
	s->event_dup = fcntl(event_fd, F_DUPFD_CLOEXEC, 0);
	if (s->event_dup < 0) {
		udsp_log(s, UDSP_LOG_ERR, "event descriptor: %s",
			 strerror(errno));
		goto fail;
	}
	if (udsp_emitter_init(&s->emitter, s->event_dup, &s->clock,
			      udsp_watchdog_ms_get(), udsp_watchdog_run, s)) {
		udsp_log(s, UDSP_LOG_ERR, "the event emitter did not initialise");
		goto fail;
	}
	if (rate_settle_open(s))
		goto fail;
	if (udsp_emitter_start(&s->emitter)) {
		udsp_log(s, UDSP_LOG_ERR, "the event emitter did not start");
		goto fail;
	}
	if (udsp_host_open(s))
		goto fail;
	udsp_slic_irq_open(s);
	udsp_fax_open(s);
	if (udsp_reset_release(s))
		goto fail;

	/* Every ops call runs on the slic thread (slic.h rule 1), so the
	 * attach is posted to it and udsp_open blocks on its completion: the
	 * one blocking call of the contract. */
	/* Two below the engine, one below the net thread, off the engine CPU;
	 * prio <= 0 is the test mode and asks for no real-time setup, as in
	 * net_start. The thread drops to SCHED_OTHER for a SLOW vendor op. */
	s->slic_prio = s->cfg.engine_prio > 2 ? s->cfg.engine_prio - 2 : 0;
	rc = udsp_thread_start(&s->slic_thread, "udsp-slic", udsp_slic_loop, s,
			       UDSP_SLIC_STACK_BYTES, -1, s->cfg.engine_cpu,
			       s->slic_prio);
	if (rc) {
		udsp_log(s, UDSP_LOG_ERR, "slic thread: %s", strerror(-rc));
		goto fail;
	}
	atomic_store_explicit(&s->slic_policy, s->slic_thread.policy,
			      memory_order_release);
	rc = udsp_attach_run(s);
	if (rc) {
		udsp_log(s, UDSP_LOG_ERR, "slic attach failed: %s", strerror(-rc));
		goto fail;
	}

	/* The rate the lines run at is fixed here, before the channels are
	 * reset against it and before the capability record is built. The
	 * channels carry no state yet: cap.lines is zero, so the slic thread
	 * walks no line and the tick returns before its first per-line pass.
	 * The line count travels as an argument because cap.lines must stay
	 * zero until the reset is done. */
	if (rate_settle_caps(s))
		goto fail;
	udsp_chan_all_reset(s, udsp_lines_count(s), s->rate);

	pthread_mutex_lock(&s->engine_lock);
	if (s->engine)
		engine_stats_get(s->engine, &st);
	else
		memset(&st, 0, sizeof(st));
	pthread_mutex_unlock(&s->engine_lock);

	udsp_cap_fill(s);
	if (!s->cap.lines) {
		udsp_log(s, UDSP_LOG_ERR,
			 "no line: the slic reports %u channel(s), the board %u port(s)",
			 s->attach_channels, s->cfg.populated_ports);
		goto fail;
	}
	if (s->cap.lines > st.channels) {
		udsp_log(s, UDSP_LOG_ERR,
			 "%u line(s) but the tdm geometry carries %u channel(s)",
			 s->cap.lines, (unsigned)st.channels);
		goto fail;
	}
	if (stream_pool_alloc(s)) {
		udsp_log(s, UDSP_LOG_ERR, "the stream pool did not allocate");
		goto fail;
	}
	if (chan_loop_pool_alloc(s)) {
		udsp_log(s, UDSP_LOG_ERR, "the loop pool did not allocate");
		goto fail;
	}
	/* The tick may already be running; this is what lets it see the lines
	 * and the stream pool the steps above built. */
	atomic_store_explicit(&s->lines, s->cap.lines, memory_order_release);
	if (net_init(s) || net_start(s)) {
		udsp_log(s, UDSP_LOG_ERR, "the net thread did not start");
		goto fail;
	}
	udsp_cap_rt_fill(s, &st);

	/* Last step: the engine ticks silence from here, so the ring stays
	 * healthy between open and start. */
	udsp_engine_gate(s);
	udsp_open_log(s, &st);

	return s;
fail:
	udsp_close(s);

	return NULL;
}

/*
 * A metallic line test carries no cancel, so a close that arrives during one
 * has to let it finish. The plug-in stops its poll loops at 90 seconds for the
 * run and 5 seconds for the abort. A few register transfers follow them, so
 * this budget covers the call with a margin. The bounded stop that follows
 * would otherwise abandon the slic thread, leak the whole handle and lose the
 * completion. The queued case waits too, because the loop runs one more job
 * step after it is told to quit. Every other reason to hang keeps the ordinary
 * budget.
 */
static void udsp_line_test_wait(struct udsp *s)
{
	uint64_t end = udsp_mono_us() + (uint64_t)UDSP_STOP_TEST_MS * 1000u;

	while (atomic_load_explicit(&s->line_test_jobs, memory_order_acquire)) {
		struct timespec nap = { .tv_sec = 0, .tv_nsec = 1000000L };

		if (udsp_mono_us() >= end)
			return;
		nanosleep(&nap, NULL);
	}
}

/* A recovery op holds the slic thread and carries no cancel, so a close waits
 * for it before the ordinary thread stop. The op returns, sees the quit flag
 * and releases the line (core/slic_thread.c). */
static void udsp_recover_wait(struct udsp *s)
{
	uint64_t end = udsp_mono_us() + (uint64_t)UDSP_STOP_RECOVER_MS * 1000u;

	while (atomic_load_explicit(&s->recover_jobs, memory_order_acquire)) {
		struct timespec nap = { .tv_sec = 0, .tv_nsec = 1000000L };

		if (udsp_mono_us() >= end)
			return;
		nanosleep(&nap, NULL);
	}
}

static void udsp_release(struct udsp *s)
{
	/* Every thread that could touch a stream has stopped, so a stream the
	 * caller never stopped releases its descriptors and keys here. */
	stream_pool_shutdown(s);
	chan_loop_pool_free(s);
	/* The engine tick and the net thread are the two producers of the
	 * packet trace, and both are joined by now. */
	free(atomic_exchange_explicit(&s->pkt_tx, NULL, memory_order_acquire));
	free(atomic_exchange_explicit(&s->pkt_rx, NULL, memory_order_acquire));
	/* The tick, joined too, is the one producer of the capture. */
	free(atomic_exchange_explicit(&s->lec_cap, NULL, memory_order_acquire));
	/* The unload goes here, after the teardown above: see rule 1 of
	 * core/fax_plugin.h. */
	fax_plugin_unload(s->fax_plugin);
	s->fax_plugin = NULL;
	s->fax_ops = NULL;
	net_release(s);
	cmd_ack_release(&s->ctl_ack);
	cmd_ack_release(&s->slic_ack);
	if (s->slic_cmd_fd >= 0)
		close(s->slic_cmd_fd);
	if (s->slic_tick_fd >= 0)
		close(s->slic_tick_fd);
	udsp_emitter_release(&s->emitter);
	/* Reached only when the emitter thread was joined, so nothing can hold
	 * this number any more. */
	if (s->event_dup >= 0)
		close(s->event_dup);
	s->event_dup = -1;
	if (s->sig_stack_live)
		udsp_thread_sigstack_release(&s->sig_stack, &s->sig_stack_prev);
	pthread_mutex_destroy(&s->ctl_lock);
	pthread_mutex_destroy(&s->engine_lock);
	free(s);
}

/*
 * Release only and safe in any prior state: a half-open failure path, a stopped
 * handle, a fatal one. It is not a second call on the same pointer, which is
 * undefined exactly as free() is. The safe linefeed belongs to udsp_stop.
 */
void udsp_close(udsp_t *s)
{
	bool leak;

	if (!s)
		return;

	if (udsp_engine_destroy(s, UDSP_EV_PROD_CTL))
		atomic_store_explicit(&s->dead, true, memory_order_release);

	net_stop(s);

	if (s->slic_thread.live) {
		atomic_store_explicit(&s->slic_quit, true, memory_order_release);
		slic_wake(s);
		udsp_line_test_wait(s);
		udsp_recover_wait(s);
		if (udsp_thread_stop_bounded(&s->slic_thread, UDSP_STOP_SLIC_MS))
			atomic_store_explicit(&s->dead, true,
					      memory_order_release);
	}
	udsp_safe_state_disarm();

	leak = atomic_load_explicit(&s->dead, memory_order_acquire);
	if (!leak) {
		/* Only a stopped slic thread may lose the code it runs in. */
		if (s->plugin)
			slic_plugin_unload(s->plugin);
		if (s->host)
			slic_host_close(s->host);
	}
	if (s->emitter.thread.live &&
	    udsp_emitter_stop(&s->emitter, UDSP_STOP_EMITTER_MS))
		leak = true;

	if (leak) {
		/* The guard stays taken. A thread nobody could stop still
		 * reaches into this graph and still holds the one plug-in
		 * instance, so a second handle would drive the same hardware
		 * beside it. The backend fails every later op HAL_FATAL and the
		 * daemon exits for procd, which is the recovery path. */
		atomic_store_explicit(&s->dead, true, memory_order_release);
		return;
	}

	udsp_release(s);
	atomic_store_explicit(&udsp_live, false, memory_order_release);
}
