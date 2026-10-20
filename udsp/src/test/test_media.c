// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Unit tests for the media plane: the core stream, net and chan media chain.
 *
 * The wire is a socketpair(AF_UNIX, SOCK_DGRAM). One end goes to
 * udsp_stream_config, and the other is the far end that this file plays. This
 * file builds every RTP packet that a case sends byte by byte, and parses every
 * packet that the case receives byte by byte. The reference therefore never
 * shares code with rtp/rtp.c. The SRTP cases use the udsp_srtp wrapper as the
 * peer session, as test_srtp.c does.
 *
 * The backend shares its paced part with test_core.c through
 * engine_harness.h. It clocks read_period from CLOCK_MONOTONIC at a settable
 * period, and records every playback period for the Goertzel checks. It
 * sources capture from a settable generator. The capture can be an echo of
 * the playback, delayed by the measured loop delay of the engine, which the
 * canceller cases need.
 *
 * The quiescence rule of test_core.c applies. This file reads neither the
 * journal nor the channel record of the fake.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "udsp/udsp.h"

#include "../core/fax_plugin.h"
#include "../core/jb_trace.h"
#include "../core/net.h"
#include "../core/pkt_trace.h"
#include "../core/udsp_priv.h"
#include "../core/stream.h"
#include "../dsp/dsp.h"
#include "../dsp/lec.h"
#include "../rtp/srtp.h"
#include "../slic/fake.h"

#ifdef HAVE_OPUS
#include <opus/opus.h>
#endif

#define EH_WAIT_SLACK_US	10000000u

#include "engine_harness.h"
#include "fax_fake.h"
#include "util.h"

#define TM_ACK_MS		(EH_SANITIZED ? "1000" : "200")
#define TM_ECHO_MAX		4096u
#define TM_PKT_MAX		1500u
/* A codec selector past the whole udsp.h range, so no build ever carries it. */
#define TM_SEL_ABSENT		200u
#define TM_ROWS(a)		(unsigned)(sizeof(a) / sizeof((a)[0]))

/* --- the allocation oracle ----------------------------------------------- */

/*
 * The media tick allocates nothing. The link options --wrap=malloc and its
 * three siblings route every allocator call this binary's own objects make
 * through the stubs below. The engine tick thread names itself on its first
 * read_period, so a count taken over a window of ticks belongs to the tick
 * path and not to the control, net or slic threads.
 *
 * tm_alloc_wrapped() is what keeps the oracle honest: without the link
 * options the stubs are dead code and the count would read a clean zero for
 * the wrong reason, so the case proves the wrap is in force before it trusts
 * the count.
 *
 * Limit: a shared library resolves malloc for itself at load time, so an
 * allocation made inside libopus is invisible here, exactly as it is to the
 * address sanitizer. The oracle covers udsp's own code.
 */
extern void *__real_malloc(size_t size);
extern void *__real_calloc(size_t nmemb, size_t size);
extern void *__real_realloc(void *ptr, size_t size);
extern void __real_free(void *ptr);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t nmemb, size_t size);
void *__wrap_realloc(void *ptr, size_t size);
void __wrap_free(void *ptr);

static _Atomic uint_least64_t tm_alloc_seen;
static _Atomic bool tm_alloc_watch;
/* Scoped to one thread, unlike the watch: the probe counts the two calls this
 * thread is about to make, and a global flag would also count whatever the
 * control, net and slic threads did while it stood. The allocator prototypes
 * carry the leaf attribute, so without volatile the compiler can delete the
 * store across the wrapped calls, and the probe then measures nothing. */
static _Thread_local volatile bool tm_alloc_probe;
static _Atomic bool tm_tick_named;
static _Atomic uint_least32_t tm_alloc_size;
static pthread_t tm_tick_thread;
/* Declared volatile, so the take and the release both survive -Os: gcc pairs
 * an allocation with its release and deletes both when the pointer never
 * escapes, and a probe that was optimized away proves nothing. */
static void *volatile tm_alloc_sink;

static void tm_alloc_note(void)
{
	if (tm_alloc_probe) {
		atomic_fetch_add_explicit(&tm_alloc_seen, 1u,
					  memory_order_relaxed);
		return;
	}
	if (!atomic_load_explicit(&tm_alloc_watch, memory_order_relaxed))
		return;
	if (!atomic_load_explicit(&tm_tick_named, memory_order_acquire))
		return;
	if (!pthread_equal(pthread_self(), tm_tick_thread))
		return;
	atomic_fetch_add_explicit(&tm_alloc_seen, 1u, memory_order_relaxed);
}

void *__wrap_malloc(size_t size)
{
	tm_alloc_note();

	return __real_malloc(size);
}

void *__wrap_calloc(size_t nmemb, size_t size)
{
	tm_alloc_note();

	return __real_calloc(nmemb, size);
}

void *__wrap_realloc(void *ptr, size_t size)
{
	tm_alloc_note();

	return __real_realloc(ptr, size);
}

/* A release is an allocator call like any other: it takes the same lock and
 * can coalesce, so the tick path may not make one either. */
void __wrap_free(void *ptr)
{
	tm_alloc_note();
	__real_free(ptr);
}

/*
 * The size comes from an atomic so the pair cannot be constant folded away,
 * and the pointer escapes for the same reason. The count must move by two and
 * by no other value: one for the take and one for the release. A smaller step
 * says one of the link options is missing and the oracle is measuring
 * nothing, which is the failure a plain "greater than zero" would hide.
 */
static bool tm_alloc_wrapped(void)
{
	uint64_t before = atomic_load(&tm_alloc_seen);

	atomic_store(&tm_alloc_size, 32u);
	tm_alloc_probe = true;
	tm_alloc_sink = malloc((size_t)atomic_load(&tm_alloc_size));
	free(tm_alloc_sink);
	tm_alloc_probe = false;
	tm_alloc_sink = NULL;

	return atomic_load(&tm_alloc_seen) == before + 2u;
}

static void tm_alloc_window(bool on)
{
	if (on)
		atomic_store(&tm_alloc_seen, 0u);
	atomic_store(&tm_alloc_watch, on);
}

static uint64_t tm_alloc_count(void)
{
	return atomic_load(&tm_alloc_seen);
}

/* --- the paced backend --------------------------------------------------- */

enum tm_src {
	TM_SRC_SILENCE = 0,
	TM_SRC_TONE,
	TM_SRC_ECHO,
	/* Plays tm_pat once, frame-aligned, then silence: the deterministic
	 * capture the DTX replay oracle needs. The case fills the buffer and
	 * resets the cursor before it stores this source. */
	TM_SRC_PATTERN,
};

/* Long enough for the 8500 ms tone the event ceiling is measured over, at the
 * 8 kHz frame the pattern is counted in. */
#define TM_PAT_FRAMES	900u
#define TM_PAT_SAMPLES	(TM_PAT_FRAMES * 80u)

static int16_t tm_pat[TM_PAT_SAMPLES];
static _Atomic uint_least32_t tm_pat_len;	/* samples, set before src */
static _Atomic uint_least32_t tm_pat_pos;	/* tick thread advances */

/* The lead moves the tone onset inside the frame, which is the alignment the
 * leak bound is measured over. */
static void tm_dtmf_pat_add(char digit, int level_dbm0, unsigned rate,
			    unsigned lead_ms, unsigned tone_ms)
{
	struct udsp_dtmf_generator gen;
	unsigned at = (unsigned)atomic_load(&tm_pat_len) +
		      lead_ms * rate / 1000u;
	unsigned tone = tone_ms * rate / 1000u;

	if (at + tone > TM_PAT_SAMPLES)
		tone = TM_PAT_SAMPLES - at;
	udsp_dtmf_generator_init(&gen, digit, level_dbm0, rate);
	udsp_dtmf_generator_render(&gen, tm_pat + at, tone);
	atomic_store_explicit(&tm_pat_len, at + tone, memory_order_release);
}

static void tm_dtmf_pat(char digit, int level_dbm0, unsigned rate,
			unsigned lead_ms, unsigned tone_ms)
{
	memset(tm_pat, 0, sizeof(tm_pat));
	atomic_store(&tm_pat_pos, 0u);
	atomic_store_explicit(&tm_pat_len, 0u, memory_order_release);
	tm_dtmf_pat_add(digit, level_dbm0, rate, lead_ms, tone_ms);
}

/* The fields of the paced back end that test_core.c does not share. */
static struct {
	/* The MediaTek AFE overruns capture when the engine stops reading, and
	 * gates the PCM clock while the stream is in XRUN. A read that arrives
	 * later than this after the previous one (or after start) reports the
	 * overrun once. 0 never overruns. */
	_Atomic uint_least32_t xrun_gap_us;
	uint64_t last_read_us;
	_Atomic uint_least64_t first_gap_us;
	_Atomic int src;
	_Atomic uint_least32_t tone_hz;
	_Atomic uint_least32_t tone_amp;
	_Atomic uint_least32_t echo_delay;
	_Atomic uint_least32_t echo_gain;	/* Q10 */
	/* delay() fails every this many ticks, which resets the engine's
	 * calibration window and keeps delay_calibrated false (engine.c
	 * engine_resync). 0 never fails. */
	_Atomic uint_least32_t delay_fail;
	/* Claimed capture backlog: what avail() reports. The echo loop the
	 * engine calibrates is delay() + avail() (engine.h loop_ref), so a
	 * case that sets this must place echo_delay at their sum. */
	_Atomic uint_least32_t avail_frames;
	/* The next read reports this many periods the engine never ran, as a
	 * capture overwrite does (engine_capture), once. */
	_Atomic uint_least32_t skip_next;
	double phase;
	int16_t echo[TM_ECHO_MAX];
	unsigned echo_w;
	uint64_t delay_calls;
} tm_be;

static void tm_backend_reset(void)
{
	eh_be_reset(false);
	atomic_store(&tm_be.src, TM_SRC_SILENCE);
	atomic_store(&tm_be.tone_hz, 0u);
	atomic_store(&tm_be.tone_amp, 8000u);
	atomic_store(&tm_be.echo_delay, 0u);
	atomic_store(&tm_be.echo_gain, 512u);
	atomic_store(&tm_be.delay_fail, 0u);
	atomic_store(&tm_be.avail_frames, 0u);
	atomic_store(&tm_be.skip_next, 0u);
	atomic_store(&tm_be.xrun_gap_us, 0u);
	atomic_store(&tm_be.first_gap_us, 0u);
	atomic_store(&tm_pat_len, 0u);
	atomic_store(&tm_pat_pos, 0u);
	/* Runs before udsp_open, so no engine thread exists to race the
	 * single write read_period makes. */
	atomic_store(&tm_alloc_watch, false);
	tm_alloc_probe = false;
	atomic_store(&tm_alloc_seen, 0u);
	atomic_store(&tm_tick_named, false);
	tm_be.last_read_us = 0;
	tm_be.phase = 0.0;
	tm_be.echo_w = 0;
	tm_be.delay_calls = 0;
	memset(tm_be.echo, 0, sizeof(tm_be.echo));
}

static bool tm_gate_plc_wait(struct eh_gate *gate, struct udsp_stream *st,
			     uint64_t *before)
{
	struct udsp_jb_stats js;

	udsp_jb_stats(&st->jb, &js);
	*before = js.plc;
	for (unsigned i = 0; i < 100u; i++) {
		/* The first concealed frame belongs to the measured window. */
		eh_rec_mark();
		if (!eh_gate_step(gate, 1))
			return false;
		udsp_jb_stats(&st->jb, &js);
		if (js.plc > *before)
			return true;
	}
	CHECK(js.plc > *before);

	return false;
}

static struct udsp_tdm *tm_open(const char *play, const char *cap,
				const struct udsp_tdm_geom *want,
				struct udsp_tdm_geom *got,
				struct udsp_tdm_fail *fail)
{
	return eh_open_geom(want, got);
}

static int tm_start_be(struct udsp_tdm *t)
{
	tm_be.last_read_us = eh_mono_us();
	/* Re-anchor the pace: a start after a stopped gap must not spend the
	 * gap as a CPU-speed catch-up burst, because the engine-swap case
	 * observes the first ticks of a replacement engine. Single-owner like
	 * last_read_us: the opening thread writes before the gate opens, a
	 * resync writes on the tick thread itself. */
	eh_be.deadline_us = 0;

	return 0;
}

static void tm_capture_fill(int16_t *buf, unsigned frames)
{
	int src = atomic_load_explicit(&tm_be.src, memory_order_acquire);
	unsigned delay = (unsigned)atomic_load(&tm_be.echo_delay);
	int32_t gain = (int32_t)atomic_load(&tm_be.echo_gain);
	double hz = (double)atomic_load(&tm_be.tone_hz);
	double amp = (double)atomic_load(&tm_be.tone_amp);
	unsigned pos;

	if (src == TM_SRC_TONE) {
		/* The handset does not know the codec's rate: it plays at the
		 * line rate, which is what a wideband line captures. */
		for (unsigned i = 0; i < frames; i++) {
			buf[i] = (int16_t)(amp * sin(tm_be.phase));
			tm_be.phase += 2.0 * M_PI * hz / eh_be.geom.rate;
		}
		return;
	}
	if (src == TM_SRC_PATTERN) {
		uint32_t pos = (uint32_t)atomic_load_explicit(&tm_pat_pos,
							memory_order_relaxed);
		uint32_t len = (uint32_t)atomic_load_explicit(&tm_pat_len,
							memory_order_acquire);

		for (unsigned i = 0; i < frames && pos < len; i++)
			buf[i] = tm_pat[pos++];
		atomic_store_explicit(&tm_pat_pos, pos, memory_order_relaxed);
		return;
	}
	if (src != TM_SRC_ECHO)
		return;

	if (delay > TM_ECHO_MAX - frames)
		delay = TM_ECHO_MAX - frames;
	pos = (tm_be.echo_w + TM_ECHO_MAX - delay) % TM_ECHO_MAX;
	for (unsigned i = 0; i < frames; i++) {
		buf[i] = (int16_t)((int32_t)tm_be.echo[pos] * gain / 1024);
		pos = (pos + 1u) % TM_ECHO_MAX;
	}
}

static int tm_read(struct udsp_tdm *t, int16_t *buf)
{
	unsigned frames = eh_be.geom.frames_per_period;
	uint64_t reads = atomic_load_explicit(&eh_be.reads, memory_order_relaxed);
	uint64_t now = eh_mono_us();
	uint32_t gap = (uint32_t)atomic_load(&tm_be.xrun_gap_us);

	/* The engine tick thread names itself for the allocation oracle. One
	 * write, published once, and every later read of the identity happens
	 * after the acquire of the flag. */
	if (!atomic_load_explicit(&tm_tick_named, memory_order_relaxed)) {
		tm_tick_thread = pthread_self();
		atomic_store_explicit(&tm_tick_named, true,
				      memory_order_release);
	}

	if (eh_wedge(reads))
		return -EAGAIN;

	if (!atomic_load(&tm_be.first_gap_us))
		atomic_store(&tm_be.first_gap_us, now - tm_be.last_read_us);
	if (gap && now - tm_be.last_read_us > gap) {
		tm_be.last_read_us = now;
		return -EPIPE;
	}
	tm_be.last_read_us = now;
	eh_pace(now);

	memset(buf, 0, (size_t)frames * eh_be.geom.channels * sizeof(*buf));
	tm_capture_fill(buf, frames);
	atomic_store_explicit(&eh_be.reads, reads + 1u, memory_order_release);

	return (int)atomic_exchange(&tm_be.skip_next, 0u);
}

static int tm_write(struct udsp_tdm *t, const int16_t *buf)
{
	unsigned frames = eh_be.geom.frames_per_period;

	for (unsigned i = 0; i < frames; i++) {
		tm_be.echo[tm_be.echo_w] = buf[i];
		tm_be.echo_w = (tm_be.echo_w + 1u) % TM_ECHO_MAX;
	}
	eh_rec_add(buf);

	return 0;
}

static long tm_delay(struct udsp_tdm *t)
{
	uint32_t every = (uint32_t)atomic_load(&tm_be.delay_fail);

	tm_be.delay_calls++;
	if (every && !(tm_be.delay_calls % every))
		return -EIO;

	return eh_delay(t);
}

static long tm_avail(struct udsp_tdm *t)
{
	return (long)atomic_load(&tm_be.avail_frames);
}

static const struct udsp_tdm_ops tm_ops = {
	.name = "tm",
	.open = tm_open,
	.close = eh_close_be,
	.recover = eh_recover,
	.start = tm_start_be,
	.stop = eh_stop_be,
	.read_period = tm_read,
	.write_period = tm_write,
	.delay = tm_delay,
	.avail = tm_avail,
};

/* --- a dry transmit ring ------------------------------------------------- */

/*
 * The paced back end with the restart capability (tdm.h tx_restarts) and the
 * four fresh reads, and a dry hold on demand (engine.c engine_rephase_read).
 * At rest the playback queue reads what eh_delay gives, one period below the
 * ring, and the capture backlog reads empty. tm_rp.arm puts a hold on the
 * next read: the read returns hold_us of wall time late, with TM_RP_HOLD - 1
 * periods of backlog behind it, and the playback ring reads empty until the
 * next write. The capture skip clears the backlog. The read after the hold
 * returns drain_us late, the wait for a fresh completion.
 */
#define TM_RP_HOLD	6u

static struct {
	atomic_bool arm;
	atomic_bool in_hold;
	_Atomic uint_least32_t hold_us;
	_Atomic uint_least32_t drain_us;
	/* Tick thread only. */
	unsigned long backlog;		/* frames */
	bool dry;
	bool after;
} tm_rp;

/* Runs before udsp_open, so no engine thread reads the fields yet. */
static void tm_rp_reset(unsigned hold_us, unsigned drain_us)
{
	atomic_store(&tm_rp.arm, false);
	atomic_store(&tm_rp.in_hold, false);
	atomic_store(&tm_rp.hold_us, hold_us);
	atomic_store(&tm_rp.drain_us, drain_us);
	tm_rp.backlog = 0;
	tm_rp.dry = false;
	tm_rp.after = false;
}

static void tm_rp_sleep(unsigned us)
{
	struct timespec ts = { .tv_sec = us / 1000000u,
			       .tv_nsec = (long)(us % 1000000u) * 1000L };

	if (us)
		nanosleep(&ts, NULL);
}

static struct udsp_tdm *tm_rp_open(const char *play, const char *cap,
				   const struct udsp_tdm_geom *want,
				   struct udsp_tdm_geom *got,
				   struct udsp_tdm_fail *fail)
{
	struct udsp_tdm *t = eh_open_geom(want, got);

	got->tx_restarts = true;
	eh_be.geom.tx_restarts = true;

	return t;
}

static int tm_rp_read(struct udsp_tdm *t, int16_t *buf)
{
	unsigned frames = eh_be.geom.frames_per_period;
	int rc = tm_read(t, buf);

	if (rc < 0)
		return rc;
	if (tm_rp.after) {
		tm_rp.after = false;
		tm_rp_sleep((unsigned)atomic_load(&tm_rp.drain_us));
	}
	if (!atomic_exchange(&tm_rp.arm, false))
		return rc;
	atomic_store(&tm_rp.in_hold, true);
	tm_rp_sleep((unsigned)atomic_load(&tm_rp.hold_us));
	tm_rp.backlog = (unsigned long)(TM_RP_HOLD - 1u) * frames;
	tm_rp.dry = true;
	tm_rp.after = true;

	return rc;
}

static int tm_rp_write(struct udsp_tdm *t, const int16_t *buf)
{
	tm_rp.dry = false;

	return tm_write(t, buf);
}

static long tm_rp_avail(struct udsp_tdm *t)
{
	return tm_rp.backlog ? (long)tm_rp.backlog : tm_avail(t);
}

static long tm_rp_avail_fresh(struct udsp_tdm *t)
{
	return (long)tm_rp.backlog;
}

static long tm_rp_room_fresh(struct udsp_tdm *t)
{
	long ring = (long)eh_be.geom.prefill_periods *
		    (long)eh_be.geom.frames_per_period;

	return tm_rp.dry ? ring : ring - eh_delay(t);
}

static int tm_rp_skip(struct udsp_tdm *t, unsigned periods)
{
	unsigned long n = (unsigned long)periods *
			  eh_be.geom.frames_per_period;

	tm_rp.backlog -= n < tm_rp.backlog ? n : tm_rp.backlog;

	return 0;
}

static const struct udsp_tdm_ops tm_rp_ops = {
	.name = "tm-rp",
	.open = tm_rp_open,
	.close = eh_close_be,
	.recover = eh_recover,
	.start = tm_start_be,
	.stop = eh_stop_be,
	.read_period = tm_rp_read,
	.write_period = tm_rp_write,
	.delay = tm_delay,
	.avail = tm_rp_avail,
	.capture_avail_fresh = tm_rp_avail_fresh,
	.playback_room_fresh = tm_rp_room_fresh,
	.capture_skip = tm_rp_skip,
};

/* The back end the next open takes: tm_ops unless a case names another.
 * tm_down clears it. */
static const struct udsp_tdm_ops *tm_ops_open;

/* --- harness ------------------------------------------------------------- */

/* The first tone row carrying this code. A carrier reports several codes on
 * its way, so the case that waits for one of them names it. */
static const struct udsp_event *tm_tone_find(unsigned code)
{
	for (unsigned i = 0; i < eh_ev_len; i++) {
		if (eh_ev[i].type == UDSP_EV_TONE &&
		    eh_ev[i].u.tone.code == code)
			return &eh_ev[i];
	}

	return NULL;
}

/* The first policy row the tone disabler posted for this reason. One carrier
 * latches and releases inside one case, so the case that reads one of the two
 * names it (event.h enum udsp_lec_reason). */
static const struct udsp_event *tm_lec_find(unsigned reason)
{
	for (unsigned i = 0; i < eh_ev_len; i++) {
		if (eh_ev[i].type == UDSP_EV_LEC_POLICY &&
		    eh_ev[i].u.lec.reason == reason)
			return &eh_ev[i];
	}

	return NULL;
}

/*
 * The tick that begins a type 2 sequence is the tick that mutes the far end
 * (line.h), so the mute marks the start of the CAS alert. The acknowledgment
 * window opens LINE_CAS_MS after this stamp. Returns 0 when no sequence began.
 */
static uint32_t tm_cid2_cas_wait(struct eh *t, unsigned line)
{
	uint64_t stop = eh_mono_us() + 2000000u;

	while (eh_mono_us() < stop) {
		if (atomic_load(&t->s->chan[line].mute))
			return eh_now_ms();
		eh_ev_poll(t);
		eh_sleep_ms(1);
	}

	return 0;
}

/* The shared object udsp_open is to dlopen, or NULL for no fax plug-in at
 * all. A case sets it before it opens the handle and tm_down clears it. */
static const char *tm_fax_path;

/* rate 0 takes the narrowband default (udsp.h sample_rate). */
static bool tm_open_rate(struct eh *t, const char *region, unsigned pace_us,
			 unsigned xrun_gap_us, unsigned avail_frames,
			 unsigned rate)
{
	struct udsp_config cfg;
	int fd[2];

	tm_backend_reset();
	udsp_slic_fake_reset();
	udsp_tdm_override(tm_ops_open ? tm_ops_open : &tm_ops);
	if (pace_us)
		atomic_store(&eh_be.pace_us, pace_us);
	atomic_store(&tm_be.xrun_gap_us, xrun_gap_us);
	/* Before open: the engine calibrates its echo loop from the first
	 * ticks, which run while udsp_open is still probing, so a backlog
	 * stored after open straddles the calibration window. */
	atomic_store(&tm_be.avail_frames, avail_frames);

	memset(&cfg, 0, sizeof(cfg));
	snprintf(cfg.tdm_backend, sizeof(cfg.tdm_backend), "%s", "file");
	cfg.channels = 2;
	cfg.prefill_periods = 3;
	cfg.populated_ports = 2;
	cfg.engine_cpu = -1;
	cfg.engine_prio = 0;
	cfg.sample_rate = (uint16_t)rate;
	if (tm_fax_path)
		snprintf(cfg.fax_plugin, sizeof(cfg.fax_plugin), "%s",
			 tm_fax_path);
	if (region)
		snprintf(cfg.region, sizeof(cfg.region), "%s", region);

	memset(t, 0, sizeof(*t));
	t->rx_fd = t->tx_fd = -1;
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fd))
		return false;
	t->rx_fd = fd[0];
	t->tx_fd = fd[1];
	eh_ev_reset();
	t->s = udsp_open(&cfg, t->tx_fd);

	return t->s != NULL;
}

static bool tm_open_only(struct eh *t, const char *region, unsigned pace_us,
			 unsigned xrun_gap_us, unsigned avail_frames)
{
	return tm_open_rate(t, region, pace_us, xrun_gap_us, avail_frames, 0);
}

static bool tm_up_rate(struct eh *t, const char *region, unsigned rate)
{
	struct udsp_job job = { 0 };
	int32_t status = -1;

	if (!tm_open_rate(t, region, 0, 0, 0, rate))
		return false;
	if (udsp_start(t->s, &job) != UDSP_OK)
		return false;

	return eh_job_wait(t, job.token, 5000, &status) && status == 0;
}

static bool tm_up(struct eh *t, const char *region)
{
	return tm_up_rate(t, region, 0);
}

static void tm_down(struct eh *t)
{
	struct udsp_job job = { 0 };
	int32_t status = -1;

	if (t->s && !t->stopped && udsp_stop(t->s, &job) == UDSP_OK)
		eh_job_wait(t, job.token, 5000, &status);
	if (t->s)
		udsp_close(t->s);
	t->s = NULL;
	if (t->rx_fd >= 0)
		close(t->rx_fd);
	if (t->tx_fd >= 0)
		close(t->tx_fd);
	t->rx_fd = t->tx_fd = -1;
	udsp_tdm_override(NULL);
	tm_ops_open = NULL;
	udsp_fax_override(NULL);
	tm_fax_path = NULL;
}

/* --- the far end --------------------------------------------------------- */

struct tm_wire {
	int rtp_local, rtp_peer;
	int rtcp_local, rtcp_peer;
	uint16_t seq;
	uint32_t ts;
	uint32_t ssrc;
};

static bool tm_pair(int *a, int *b, int bufsize)
{
	int fd[2];

	if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, fd))
		return false;
	if (bufsize) {
		setsockopt(fd[0], SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
		setsockopt(fd[1], SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
	}
	*a = fd[0];
	*b = fd[1];

	return true;
}

static bool tm_wire_open(struct tm_wire *w, int bufsize)
{
	memset(w, 0, sizeof(*w));
	w->rtp_local = w->rtp_peer = w->rtcp_local = w->rtcp_peer = -1;
	w->seq = 1000;
	w->ts = 160000;
	w->ssrc = 0x51501234u;

	return tm_pair(&w->rtp_local, &w->rtp_peer, bufsize) &&
	       tm_pair(&w->rtcp_local, &w->rtcp_peer, bufsize);
}

static void tm_wire_close(struct tm_wire *w)
{
	int *fd[4] = { &w->rtp_local, &w->rtp_peer, &w->rtcp_local,
		       &w->rtcp_peer };

	for (unsigned i = 0; i < 4; i++) {
		if (*fd[i] >= 0)
			close(*fd[i]);
		*fd[i] = -1;
	}
}

/* --- the call ------------------------------------------------------------ */

static void tm_call_close(struct eh *t, struct tm_wire *w)
{
	tm_wire_close(w);
	tm_down(t);
}

/* The far end of the handle that the caller brought up; up is the result of
 * that bring-up. A failure of either fails the case and frees the handle. */
static bool tm_call_wire(struct eh *t, bool up, struct tm_wire *w, int bufsize)
{
	bool wired;

	CHECK(up);
	if (!up) {
		tm_down(t);
		return false;
	}
	wired = tm_wire_open(w, bufsize);
	CHECK(wired);
	if (!wired)
		tm_call_close(t, w);

	return wired;
}

/* A started handle on the region, and one far end. */
static bool tm_call_up(struct eh *t, const char *region, struct tm_wire *w,
		       int bufsize)
{
	return tm_call_wire(t, tm_up(t, region), w, bufsize);
}

static bool tm_call_up_rate(struct eh *t, const char *region, unsigned rate,
			    struct tm_wire *w)
{
	return tm_call_wire(t, tm_up_rate(t, region, rate), w, 0);
}

/* The teardown of a call whose stream 0 still runs. */
static void tm_call_down(struct eh *t, struct tm_wire *w)
{
	CHECK(udsp_stream_stop(t->s, 0, 0) == UDSP_OK);
	tm_call_close(t, w);
}

static void tm_stream_config(struct eh *t, struct tm_wire *w,
			     const struct udsp_media *m)
{
	CHECK(udsp_stream_config(t->s, 0, 0, m, w->rtp_local,
				 w->rtcp_local) == UDSP_OK);
}

static void tm_capture_tone(uint32_t hz)
{
	atomic_store(&tm_be.tone_hz, hz);
	atomic_store(&tm_be.src, TM_SRC_TONE);
}

/* An independent RTP writer and reader: the oracle must not share code with
 * the implementation it checks. */
static size_t tm_rtp_build(uint8_t *pkt, uint8_t pt, bool marker, uint16_t seq,
			   uint32_t ts, uint32_t ssrc, const uint8_t *payload,
			   size_t len)
{
	pkt[0] = 0x80;
	pkt[1] = (uint8_t)(pt | (marker ? 0x80u : 0u));
	pkt[2] = (uint8_t)(seq >> 8);
	pkt[3] = (uint8_t)seq;
	pkt[4] = (uint8_t)(ts >> 24);
	pkt[5] = (uint8_t)(ts >> 16);
	pkt[6] = (uint8_t)(ts >> 8);
	pkt[7] = (uint8_t)ts;
	pkt[8] = (uint8_t)(ssrc >> 24);
	pkt[9] = (uint8_t)(ssrc >> 16);
	pkt[10] = (uint8_t)(ssrc >> 8);
	pkt[11] = (uint8_t)ssrc;
	if (len)
		memcpy(pkt + 12, payload, len);

	return 12u + len;
}

struct tm_pkt {
	uint8_t pt;
	bool marker;
	uint16_t seq;
	uint32_t ts;
	uint32_t ssrc;
	const uint8_t *payload;
	size_t len;
};

static bool tm_rtp_parse(const uint8_t *p, size_t n, struct tm_pkt *out)
{
	if (n < 12u || (p[0] >> 6) != 2u || (p[0] & 0x0fu))
		return false;
	out->pt = p[1] & 0x7fu;
	out->marker = (p[1] & 0x80u) != 0;
	out->seq = (uint16_t)((p[2] << 8) | p[3]);
	out->ts = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
		  ((uint32_t)p[6] << 8) | p[7];
	out->ssrc = ((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16) |
		    ((uint32_t)p[10] << 8) | p[11];
	out->payload = p + 12;
	out->len = n - 12u;

	return true;
}

/* Walks a compound and lists its packet types; false when the framing of
 * RFC 3550 6.1 does not close exactly on the datagram. */
static bool tm_rtcp_walk(const uint8_t *p, size_t n, unsigned *types,
			 unsigned max, unsigned *count)
{
	size_t off = 0;

	*count = 0;
	while (off + 4u <= n) {
		size_t len = (((size_t)p[off + 2] << 8) | p[off + 3]) + 1u;

		len *= 4u;
		if (!len || off + len > n)
			return false;
		if (*count < max)
			types[(*count)++] = p[off + 1];
		off += len;
	}

	return off == n;
}

/* One datagram with bytes, or -1 when none is queued. An empty datagram is
 * the punch that holds the tuple open (core/stream.c stream_punch), and a
 * receiver discards it on the RFC 3550 minimum length, so this reads past it
 * as a receiver does. */
static ssize_t tm_recv(int fd, uint8_t *buf, size_t cap)
{
	for (;;) {
		ssize_t n = recv(fd, buf, cap, MSG_DONTWAIT);

		if (n != 0)
			return n;
	}
}

/* Waits up to ms of engine time for one datagram. */
static ssize_t tm_recv_wait(struct eh *t, int fd, uint8_t *buf, size_t cap,
			    uint32_t ms)
{
	uint32_t end = eh_now_ms() + ms;
	uint64_t stop = eh_mono_us() + (uint64_t)ms * 1000u + 10000000u;

	for (;;) {
		ssize_t n = tm_recv(fd, buf, cap);

		if (n > 0)
			return n;
		if (eh_now_ms() >= end || eh_mono_us() >= stop)
			return -1;
		eh_ev_poll(t);
		eh_sleep_ms(1);
	}
}

static void tm_media(struct udsp_media *m, uint8_t codec, uint8_t pt,
		     uint8_t ptime)
{
	memset(m, 0, sizeof(*m));
	m->local.ipver = 4;
	m->local.port = 4000;
	m->remote.ipver = 4;
	m->remote.port = 5000;
	m->codec = codec;
	m->codec_pt = pt;
	m->direction = UDSP_MEDIA_SENDRECV;
	m->ptime_ms = ptime;
	m->jb_mode = UDSP_JB_ADAPTIVE;
	m->jb_init_ms = 20;
	m->jb_max_ms = 200;
	m->rtcp_enable = true;
	m->rtcp_interval_ms = UDSP_RTCP_MIN_MS;
}

/* --- the fake T.38 gateway ----------------------------------------------- */

/*
 * Two ways in. udsp_fax_override installs the vtable of test/fax_fake.h
 * directly, which is what every engine case uses; tm_fax_path names the
 * test-only shared object instead, so udsp_open takes the dlopen the loader
 * cases and the unload order need. tm_down clears both.
 */
static bool tm_up_fax(struct eh *t, const char *region, unsigned rate)
{
	fax_fake_reset();
	udsp_fax_override(&fax_fake_ops);

	return tm_up_rate(t, region, rate);
}

static bool tm_call_up_fax(struct eh *t, const char *region, unsigned rate,
			   struct tm_wire *w)
{
	return tm_call_wire(t, tm_up_fax(t, region, rate), w, 0);
}

/* The T.38 description uvoice programs: the mode key, the resolved triple and
 * the addresses and direction it shares with an audio stream. */
static void tm_t38_media(struct udsp_media *m)
{
	tm_media(m, UDSP_SEL_T38, 0, 20);
	m->t38.max_bit_rate = 14400;
	m->t38.max_datagram = 200;
	m->t38.ec = UDSP_T38_EC_REDUNDANCY;
}

/* One UDPTL datagram from the far end, carrying the count the fake records. */
static void tm_udptl_send_fd(int fd, unsigned value)
{
	uint8_t pkt[FAX_FAKE_DGRAM];

	pkt[0] = (uint8_t)(value >> 24);
	pkt[1] = (uint8_t)(value >> 16);
	pkt[2] = (uint8_t)(value >> 8);
	pkt[3] = (uint8_t)value;
	if (send(fd, pkt, sizeof(pkt), MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
		return;
}

static void tm_udptl_send(struct tm_wire *w, unsigned value)
{
	tm_udptl_send_fd(w->rtp_peer, value);
}

static unsigned tm_udptl_value(const uint8_t *pkt)
{
	return ((unsigned)pkt[0] << 24) | ((unsigned)pkt[1] << 16) |
	       ((unsigned)pkt[2] << 8) | pkt[3];
}

/* --- payload helpers ----------------------------------------------------- */

static void tm_tone_frame(int16_t *pcm, unsigned n, double hz, double amp,
			  double *phase)
{
	for (unsigned i = 0; i < n; i++) {
		pcm[i] = (int16_t)(amp * sin(*phase));
		*phase += 2.0 * M_PI * hz / UDSP_SAMPLE_RATE;
	}
}

static void tm_ulaw_encode(const int16_t *pcm, unsigned n, uint8_t *out)
{
	for (unsigned i = 0; i < n; i++)
		out[i] = udsp_ulaw_encode(pcm[i]);
}

static void tm_alaw_encode(const int16_t *pcm, unsigned n, uint8_t *out)
{
	for (unsigned i = 0; i < n; i++)
		out[i] = udsp_alaw_encode(pcm[i]);
}

/* One packet of companded audio, sent from the far end. */
static void tm_pcm_send(struct tm_wire *w, uint8_t pt, const int16_t *pcm,
			unsigned n, bool alaw)
{
	uint8_t pkt[TM_PKT_MAX];
	uint8_t pay[240];
	size_t len;

	if (alaw)
		tm_alaw_encode(pcm, n, pay);
	else
		tm_ulaw_encode(pcm, n, pay);
	len = tm_rtp_build(pkt, pt, w->seq == 1000, w->seq, w->ts, w->ssrc, pay, n);
	w->seq++;
	w->ts += n;
	if (send(w->rtp_peer, pkt, len, MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
		return;
}

/* One packet of ptime milliseconds of a tone, sent from the far end. */
static void tm_tone_send_pt(struct tm_wire *w, uint8_t pt, unsigned ptime_ms,
			    double hz, double amp, double *phase, bool alaw)
{
	int16_t pcm[240];
	unsigned n = ptime_ms * 8u;

	tm_tone_frame(pcm, n, hz, amp, phase);
	tm_pcm_send(w, pt, pcm, n, alaw);
}

static void tm_tone_send(struct tm_wire *w, uint8_t pt, unsigned ptime_ms,
			 double hz, double amp, double *phase)
{
	tm_tone_send_pt(w, pt, ptime_ms, hz, amp, phase, false);
}

static bool tm_gate_ring_prepare(struct udsp_stream *st, unsigned datagrams,
				 unsigned *want)
{
	unsigned head = atomic_load_explicit(&st->ring.head,
					     memory_order_acquire);
	unsigned tail = atomic_load_explicit(&st->ring.tail,
					     memory_order_acquire);

	CHECK_UINT(head, tail);
	CHECK(datagrams < UDSP_NET_SLOTS);
	if (head != tail || datagrams >= UDSP_NET_SLOTS)
		return false;
	*want = head + datagrams;

	return true;
}

static bool tm_gate_ring_wait(struct udsp_stream *st, unsigned want)
{
	uint64_t end = eh_mono_us() + EH_GATE_WAIT_US;
	unsigned head;

	/* A socket send does not publish the receive ring the tick drains. */
	while (atomic_load_explicit(&st->ring.head,
				    memory_order_acquire) != want &&
	       eh_mono_us() < end)
		sched_yield();
	head = atomic_load_explicit(&st->ring.head, memory_order_acquire);
	CHECK_UINT(head, want);
	CHECK_UINT(atomic_load(&st->net_trunc), 0);
	CHECK_UINT(atomic_load(&st->net_drops), 0);
	CHECK_UINT(atomic_load(&st->net_stale), 0);

	return head == want;
}

static bool tm_gate_tone_publish(struct udsp_stream *st, struct tm_wire *w,
				 unsigned packets, const struct udsp_media *m,
				 double hz, double amp, double *phase)
{
	unsigned want;

	if (!tm_gate_ring_prepare(st, packets, &want))
		return false;
	for (unsigned i = 0; i < packets; i++)
		tm_tone_send_pt(w, m->codec_pt, m->ptime_ms, hz, amp, phase,
				m->codec == UDSP_SEL_PCMA);

	return tm_gate_ring_wait(st, want);
}

static bool tm_gate_tone_send(struct udsp_stream *st, struct tm_wire *w,
			      unsigned packets, double *phase)
{
	const struct udsp_media m = {
		.codec = UDSP_SEL_PCMU, .codec_pt = 0, .ptime_ms = 10,
	};

	return tm_gate_tone_publish(st, w, packets, &m, 1000.0, 8000.0, phase);
}

static bool tm_gate_tone_run_at(struct eh_gate *gate, struct udsp_stream *st,
				struct tm_wire *w, unsigned batches,
				double hz, double amp, double *phase)
{
	const struct udsp_media m = {
		.codec = UDSP_SEL_PCMU, .codec_pt = 0, .ptime_ms = 10,
	};

	/* The extra packet keeps the warm-up from ending in concealment. */
	for (unsigned i = 0; i < batches; i++) {
		if (!tm_gate_tone_publish(st, w, 10, &m, hz, amp, phase) ||
		    !eh_gate_step(gate, 9))
			return false;
	}

	return true;
}

static bool tm_gate_tone_run(struct eh_gate *gate, struct udsp_stream *st,
			     struct tm_wire *w, unsigned batches, double *phase)
{
	return tm_gate_tone_run_at(gate, st, w, batches, 1000.0, 8000.0, phase);
}

static bool tm_gate_tone_window(struct eh_gate *gate, struct udsp_stream *st,
				struct tm_wire *w, unsigned warm,
				unsigned batches, double *phase)
{
	CHECK(batches > 0u);
	if (!batches)
		return false;
	if (!tm_gate_tone_run(gate, st, w, warm, phase) ||
	    !tm_gate_tone_send(st, w, 10, phase))
		return false;
	eh_rec_mark();

	return eh_gate_step(gate, 9) &&
	       tm_gate_tone_run(gate, st, w, batches - 1u, phase);
}

/* The tone power of a companded payload, decoded by the G.711 code of the
 * library. A size check alone cannot tell a full packet from a zeroed one. */
static double tm_payload_power(const uint8_t *pay, size_t len, bool alaw,
			       double hz)
{
	struct udsp_goertzel_bank bank;
	int16_t pcm[240];

	if (!len || len > 240u)
		return 0.0;
	for (size_t i = 0; i < len; i++)
		pcm[i] = alaw ? udsp_alaw_decode(pay[i]) : udsp_ulaw_decode(pay[i]);
	udsp_goertzel_init(&bank, &hz, 1);
	udsp_goertzel_process(&bank, pcm, (unsigned)len);

	return udsp_goertzel_power(&bank, 0);
}

/*
 * The digit the recorded playback carries, from eh_rec_mark on, or 0. The
 * engine's own detector reads the uplink, and these frames are the downlink
 * the line played, so the detector runs offline here (the test_core case_digit
 * pattern). It reports a digit only once the tone ends, so a tone still
 * sounding at the end of the record answers through the active digit.
 */
static char tm_rec_digit(void)
{
	struct udsp_dtmf_detector det;
	unsigned have = eh_rec_have();
	struct udsp_event ev;
	char seen = 0;

	udsp_dtmf_detector_init(&det, false);
	for (unsigned i = 0; i + UDSP_FRAME_SAMPLES <= have;
	     i += UDSP_FRAME_SAMPLES) {
		if (udsp_dtmf_detector_process(&det,
					       eh_be.rec + eh_be.rec_base + i,
					       &ev))
			seen = ev.u.digit.digit;
	}
	if (!seen)
		seen = udsp_dtmf_detector_active(&det);

	return seen;
}

/* How many 10 ms frames of the recorded playback carry a signal, from
 * eh_rec_mark on. The measured length of a rendered digit. */
static unsigned tm_rec_loud_frames(void)
{
	unsigned have = eh_rec_have();
	unsigned n = 0;

	for (unsigned i = 0; i + UDSP_FRAME_SAMPLES <= have;
	     i += UDSP_FRAME_SAMPLES) {
		if (eh_energy(i, UDSP_FRAME_SAMPLES) > 1e5)
			n++;
	}

	return n;
}

/* --- jitter buffer diagnostics helpers ----------------------------------- */

/* One packet of ptime milliseconds with the caller's own sequence number and
 * timestamp, so a case can hold a packet back and deliver it out of order. */
static void tm_pkt_send_at(struct tm_wire *w, uint8_t pt, unsigned ptime_ms,
			   uint16_t seq, uint32_t ts)
{
	uint8_t pkt[TM_PKT_MAX];
	uint8_t pay[240];
	unsigned n = ptime_ms * 8u;
	size_t len;

	memset(pay, 0xffu, n);
	len = tm_rtp_build(pkt, pt, false, seq, ts, w->ssrc, pay, n);
	if (send(w->rtp_peer, pkt, len, MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
		return;
}

static void tm_burst_send(struct tm_wire *w, unsigned count)
{
	for (unsigned i = 0; i < count; i++) {
		tm_pkt_send_at(w, 0, 10, w->seq, w->ts);
		w->seq++;
		w->ts += 80;
	}
}

/* The five output counters partition the 10 ms units and the three drop
 * counters partition the rejected packets (jb.h Event mapping). */
static uint64_t tm_jb_units(const struct udsp_jb_stats *js)
{
	return js->frames_out + js->plc + js->cn + js->silence + js->skew_inserts;
}

static uint64_t tm_jb_drops(const struct udsp_jb_stats *js)
{
	return js->late_drop + js->dup_drop + js->overflow_drop + js->hold_drop;
}

#define TM_TRACE_MAX	600u

static struct udsp_jb_trace tm_trace[TM_TRACE_MAX];

static unsigned tm_trace_read(struct eh *t)
{
	return udsp_jb_trace_read(t->s, tm_trace, TM_TRACE_MAX);
}

/* Empties the ring, so a following read reports only what came after it. */
static void tm_trace_flush(struct eh *t)
{
	while (tm_trace_read(t))
		continue;
}

static const struct udsp_jb_trace *tm_trace_rc(unsigned n, unsigned kind, int rc)
{
	for (unsigned i = 0; i < n; i++) {
		if (tm_trace[i].kind == kind && tm_trace[i].rc == rc)
			return &tm_trace[i];
	}

	return NULL;
}

static const struct udsp_jb_trace *tm_trace_flag(unsigned n, unsigned kind,
						 uint16_t flag)
{
	for (unsigned i = 0; i < n; i++) {
		if (tm_trace[i].kind == kind && (tm_trace[i].flags & flag))
			return &tm_trace[i];
	}

	return NULL;
}

/* Every record names its own stream and the stamps never go backwards: the
 * producer is one thread and it stamps with the tick it runs on. */
static bool tm_trace_shape(unsigned n, uint8_t line, uint8_t stream)
{
	uint32_t last = 0;

	for (unsigned i = 0; i < n; i++) {
		if (tm_trace[i].line != line || tm_trace[i].stream != stream)
			return false;
		if (i && (int32_t)(tm_trace[i].stamp_ms - last) < 0)
			return false;
		last = tm_trace[i].stamp_ms;
	}

	return n > 0;
}

/* --- cases: the audio path ----------------------------------------------- */

static void case_audio(void)
{
	struct tm_wire w;
	struct udsp_media m;
	struct eh t;
	double phase = 0.0;

	CASE("a G.711 stream decodes the far end into the playback path");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct udsp_stream *st;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st || !eh_gate_park(&gate) ||
		    !tm_gate_tone_send(st, &w, 20, &phase) ||
		    !eh_gate_step(&gate, 10) ||
		    !tm_gate_tone_send(st, &w, 20, &phase))
			goto audio_out;
		eh_rec_mark();
		if (!eh_gate_step(&gate, 20))
			goto audio_out;
		CHECK_UINT(eh_rec_have(), 1600);
		if (eh_rec_have() != 1600u)
			goto audio_out;
		CHECK(eh_power(0, 800, 1000.0) > 1e6);
		CHECK(eh_power(0, 800, 1900.0) * 100.0 < eh_power(0, 800, 1000.0));
	audio_out:
		eh_gate_disarm(&gate);
		tm_call_down(&t, &w);
		eh_gate_close(&gate);
	}

	CASE("captured audio leaves as G.711 RTP on the wire");
	{
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p;
		ssize_t n;
		int16_t pcm[240];
		double got;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		/* Two ticks exclude a stale capture and an in-flight fill. */
		eh_wait_ms(&t, 20);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);

		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 200);
		CHECK(n > 0);
		if (n > 0 && tm_rtp_parse(buf, (size_t)n, &p)) {
			struct udsp_goertzel_bank bank;
			double hz = 1000.0;

			CHECK_UINT(p.pt, 0);
			CHECK_UINT(p.len, 80);
			for (unsigned i = 0; i < p.len && i < 240; i++)
				pcm[i] = udsp_ulaw_decode(p.payload[i]);
			udsp_goertzel_init(&bank, &hz, 1);
			udsp_goertzel_process(&bank, pcm, 80);
			got = udsp_goertzel_power(&bank, 0);
			CHECK(got > 1e6);
		} else {
			CHECK(false);
		}
		tm_call_down(&t, &w);
	}

	CASE("the first packet each way is reported once");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(400u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		for (unsigned i = 0; i < 20; i++)
			tm_tone_send(&w, 0, 10, 400.0, 6000.0, &phase);
		eh_wait_ms(&t, 300);
		CHECK_UINT(eh_ev_count(UDSP_EV_RTP_FIRST_TX), 1);
		CHECK_UINT(eh_ev_count(UDSP_EV_RTP_FIRST_RX), 1);
		{
			const struct udsp_event *e =
				eh_ev_find(UDSP_EV_RTP_FIRST_RX, 0);

			CHECK(e != NULL);
			if (e) {
				CHECK_UINT(e->line, 0);
				CHECK_UINT(e->stream, 0);
			}
		}
		tm_call_down(&t, &w);
	}
}

/*
 * One window of the call waiting tone over a running stream at one receive
 * value. A cushion of packets keeps the jitter buffer fed while the gate is
 * open for the two SYNC-TICK ops. Each packet holds ten whole cycles of the
 * 1000 Hz far end, so a packet the buffer drops moves no phase. The window
 * of 75 ms holds whole cycles of both tones, so neither leaks into the bin
 * of the other.
 */
static bool tm_cw_level_window(struct eh_gate *gate, struct eh *t,
			       struct udsp_stream *st, struct tm_wire *w,
			       int slic_gain_rx, double *phase)
{
	struct udsp_chan_cfg ccfg;

	if (!tm_gate_tone_send(st, w, 10, phase))
		return false;
	eh_gate_disarm(gate);
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.slic_gain_rx = (int8_t)slic_gain_rx;
	CHECK(udsp_chan_config(t->s, 0, &ccfg) == UDSP_OK);
	CHECK(udsp_cw_tone_play(t->s, 0, 0, 0) == UDSP_OK);

	return eh_gate_park(gate) &&
	       tm_gate_tone_window(gate, st, w, 0, 1, phase);
}

static void tm_cw_level_run(unsigned rate)
{
	static const int slic_rx[2] = { 0, -18 };
	struct eh_gate gate = { .fd = { -1, -1 } };
	const unsigned span = 75u * (rate / 1000u);
	const struct udsp_region_tone *cw = NULL;
	const struct udsp_region *r;
	double voice[2] = { 0.0, 0.0 };
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	double phase = 0.0;
	struct eh t;

	if (!tm_call_up_rate(&t, "USA", rate, &w))
		return;
	r = udsp_region_active(t.s);
	if (r)
		cw = &r->tone[UDSP_TONE_CALL_WAITING];
	tm_media(&m, UDSP_SEL_PCMU, 0, 10);
	tm_stream_config(&t, &w, &m);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL && cw != NULL);
	if (!st || !cw || !eh_gate_park(&gate) ||
	    !tm_gate_tone_run(&gate, st, &w, 2, &phase))
		goto out;
	for (unsigned i = 0; i < 2u; i++) {
		double tone;

		if (!tm_cw_level_window(&gate, &t, st, &w, slic_rx[i], &phase))
			goto out;
		voice[i] = eh_tone_dbm0(0, span, 1000.0, rate);
		tone = eh_tone_dbm0(0, span, cw->desc.frequency_hz[0], rate);
		printf("# level cw %u Hz slic_gain_rx %d: tone %.2f dBm0, "
		       "want %d; stream %.2f dBm0\n", rate, slic_rx[i], tone,
		       cw->desc.level_dbm0[0], voice[i]);
		CHECK(fabs(tone - cw->desc.level_dbm0[0]) <= 0.2);
	}
	/* The stream drops by the 9 dB loss of the port. */
	CHECK(fabs(voice[1] - voice[0] + 9.0) <= 0.3);
out:
	eh_gate_disarm(&gate);
	tm_call_down(&t, &w);
	eh_gate_close(&gate);
}

/*
 * The stream audio takes the receive gain of the port ahead of the playout
 * sum (core/chan.c chan_dl). The call waiting tone therefore plays at its
 * region level whatever that gain. The far end plays 1000 Hz. The case reads
 * both tones of one window, at a receive value of 0 and of -18 half-dB, at
 * both rates.
 */
static void case_cw_level(void)
{
	CASE("the call waiting tone keeps its level over a stream");
	tm_cw_level_run(UDSP_SAMPLE_RATE);
	tm_cw_level_run(UDSP_SAMPLE_RATE_WB);
}

static void case_ptime(void)
{
	struct tm_wire w;
	struct udsp_media m;
	struct eh t;
	double phase = 0.0;

	CASE("ptime 20 aggregates two frames per packet in both directions");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct udsp_stream *st;
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p = { 0 };
		struct tm_pkt q = { 0 };
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		/* Two ticks exclude a stale capture and an in-flight fill. */
		eh_wait_ms(&t, 20);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);

		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 200);
		CHECK(n == 12 + 160);
		CHECK(n > 0 && tm_rtp_parse(buf, (size_t)n, &p));
		/* The payload has to carry the tone: a zeroed aggregate of the
		 * right size passes a length check and nothing else. */
		if (n > 0)
			CHECK(tm_payload_power(p.payload, p.len, false, 1000.0) >
			      1e6);
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 200);
		CHECK(n > 0 && tm_rtp_parse(buf, (size_t)n, &q));
		if (n > 0) {
			CHECK_UINT((uint16_t)(q.seq - p.seq), 1);
			CHECK_UINT(q.ts - p.ts, 160);
			CHECK_UINT(q.ssrc, p.ssrc);
			CHECK_UINT(q.len, 160);
			CHECK(tm_payload_power(q.payload, q.len, false, 1000.0) >
			      1e6);
		}
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st || !eh_gate_park(&gate) ||
		    !tm_gate_tone_publish(st, &w, 10, &m, 1000.0, 8000.0,
					  &phase) ||
		    !eh_gate_step(&gate, 10) ||
		    !tm_gate_tone_publish(st, &w, 10, &m, 1000.0, 8000.0,
					  &phase))
			goto ptime20_out;
		eh_rec_mark();
		if (!eh_gate_step(&gate, 20))
			goto ptime20_out;
		CHECK_UINT(eh_rec_have(), 1600);
		if (eh_rec_have() != 1600u)
			goto ptime20_out;
		CHECK(eh_power(0, 800, 1000.0) > 1e6);
	ptime20_out:
		eh_gate_disarm(&gate);
		tm_call_down(&t, &w);
		eh_gate_close(&gate);
	}

	CASE("ptime 30 aggregates three frames per packet");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct udsp_stream *st;
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p = { 0 };
		struct tm_pkt q = { 0 };
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMA, 8, 30);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		/* Two ticks exclude a stale capture and an in-flight fill. */
		eh_wait_ms(&t, 20);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);

		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 200);
		CHECK(n == 12 + 240);
		CHECK(n > 0 && tm_rtp_parse(buf, (size_t)n, &p));
		if (n > 0) {
			CHECK_UINT(p.pt, 8);
			CHECK(tm_payload_power(p.payload, p.len, true, 1000.0) >
			      1e6);
		}
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 200);
		CHECK(n > 0 && tm_rtp_parse(buf, (size_t)n, &q));
		if (n > 0) {
			CHECK_UINT(q.ts - p.ts, 240);
			CHECK_UINT(q.len, 240);
			CHECK(tm_payload_power(q.payload, q.len, true, 1000.0) >
			      1e6);
		}

		/* The receive half of ptime 30: three frames per packet have to
		 * reach the playback path as audio, not only leave as one. */
		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st || !eh_gate_park(&gate) ||
		    !tm_gate_tone_publish(st, &w, 15, &m, 400.0, 8000.0,
					  &phase) ||
		    !eh_gate_step(&gate, 15) ||
		    !tm_gate_tone_publish(st, &w, 15, &m, 400.0, 8000.0,
					  &phase))
			goto ptime30_out;
		eh_rec_mark();
		if (!eh_gate_step(&gate, 30))
			goto ptime30_out;
		CHECK_UINT(eh_rec_have(), 2400);
		if (eh_rec_have() != 2400u)
			goto ptime30_out;
		CHECK(eh_power(0, 800, 400.0) > 1e6);
		CHECK(eh_power(0, 800, 1900.0) * 100.0 < eh_power(0, 800, 400.0));
	ptime30_out:
		eh_gate_disarm(&gate);
		tm_call_down(&t, &w);
		eh_gate_close(&gate);
	}

	CASE("a zero jitter buffer request takes the defaults");
	{
		struct udsp_stream *st;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.jb_init_ms = 0;
		m.jb_max_ms = 0;
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			/* The defaults: jitter_target 50, buffer max 100. */
			CHECK_UINT(st->media.jb_init_ms, 50);
			CHECK_UINT(st->media.jb_max_ms, 100);
		}
		/* A maximum below the target is raised to it, never below. */
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.jb_init_ms = 80;
		m.jb_max_ms = 40;
		tm_stream_config(&t, &w, &m);
		if (st) {
			CHECK_UINT(st->media.jb_init_ms, 80);
			CHECK_UINT(st->media.jb_max_ms, 80);
		}
		tm_call_down(&t, &w);
	}

	CASE("a stream refuses a codec and a ptime it cannot carry");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		/* The refusal fixture is a selector no build carries. Every
		 * build carries G.729, and a build with libopus carries Opus,
		 * so the fixture is a value past the whole selector range. */
		tm_media(&m, TM_SEL_ABSENT, 96, 20);
		CHECK(udsp_stream_config(t.s, 0, 0, &m, w.rtp_local,
					 w.rtcp_local) == UDSP_UNSUPPORTED);
#ifndef HAVE_OPUS
		/* Without libopus the Opus selector is one of those too. */
		tm_media(&m, UDSP_SEL_OPUS, 96, 20);
		CHECK(udsp_stream_config(t.s, 0, 0, &m, w.rtp_local,
					 w.rtcp_local) == UDSP_UNSUPPORTED);
#endif
		tm_media(&m, UDSP_SEL_PCMU, 0, 25);
		CHECK(udsp_stream_config(t.s, 0, 0, &m, w.rtp_local,
					 w.rtcp_local) == UDSP_UNSUPPORTED);
		/* A 40 ms offer is clamped to 30 and accepted. */
		tm_media(&m, UDSP_SEL_PCMU, 0, 40);
		tm_stream_config(&t, &w, &m);
		{
			struct udsp_stream *st = stream_at(t.s, 0, 0);

			CHECK(st != NULL);
			if (st)
				CHECK_UINT(st->media.ptime_ms, 30);
		}
		tm_call_down(&t, &w);
	}
}

/* --- cases: reconfiguration ---------------------------------------------- */

static void case_reconfig(void)
{
	struct tm_wire w;
	struct udsp_media m;
	struct eh t;
	double phase = 0.0;

	CASE("a reconfiguration on a running stream keeps the source and marks it");
	{
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt before, after;
		bool got_new = false;
		uint32_t skip_end;
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 60);
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 200);
		CHECK(n > 0 && tm_rtp_parse(buf, (size_t)n, &before));
		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;

		/* The same descriptors, a new description: the common
		 * re-INVITE (udsp.h Reconfiguring a running stream). */
		tm_media(&m, UDSP_SEL_PCMA, 8, 20);
		tm_stream_config(&t, &w, &m);
		eh_wait_ms(&t, 60);
		/*
		 * The drain above ran before udsp_stream_config, so packets
		 * the engine sent between the drain and the tick that applies
		 * the new description are legitimately old-config (PCMU pt 0,
		 * no marker). Under an ASan or TSan load that window spans one
		 * or more ptime-10 packets, as measured. Skip them and stop at
		 * the first pt-8 packet, bounded by the same 200 engine-ms
		 * class budget the single receive used. A skipped packet must
		 * still be old-config on the same SSRC: a third payload type or
		 * a foreign SSRC is a real failure.
		 */
		skip_end = eh_now_ms() + 200u;
		while (!got_new) {
			uint32_t now = eh_now_ms();
			bool parsed;

			n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf),
					 now < skip_end ? skip_end - now : 0u);
			if (n <= 0)
				break;
			parsed = tm_rtp_parse(buf, (size_t)n, &after);
			CHECK(parsed);
			if (!parsed)
				break;
			if (after.pt == 8u) {
				got_new = true;
				break;
			}
			CHECK_UINT(after.pt, 0);
			CHECK_UINT(after.ssrc, before.ssrc);
			/* rtp_sender_reconfigure raises the marker at the
			 * apply tick together with the new pt, so no pt-0
			 * packet can carry the reconfig marker. */
			CHECK(!after.marker);
			if (after.pt != 0u || after.ssrc != before.ssrc)
				break;
		}
		CHECK(got_new);
		if (got_new) {
			CHECK_UINT(after.ssrc, before.ssrc);
			CHECK(after.marker);
			CHECK_UINT(after.pt, 8);
			CHECK(after.ts - before.ts >= 80u);
			CHECK((uint16_t)(after.seq - before.seq) < 100u);
			CHECK((uint16_t)(after.seq - before.seq) > 0u);
		}
		tm_call_down(&t, &w);
	}

	CASE("a reconfiguration re-inits the jitter buffer");
	{
		uint64_t before_total = 0;
		uint64_t after_total = 0;
		unsigned before_size = 0;
		unsigned after_size = 0;
		unsigned mark;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.jb_init_ms = 40;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		for (unsigned burst = 0; burst < 16u; burst++) {
			for (unsigned i = 0; i < 25u; i++)
				tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
			eh_wait_ms(&t, 250);
		}
		mark = eh_ev_len;
		for (unsigned i = 0; i < eh_ev_len; i++) {
			if (eh_ev[i].type != UDSP_EV_JB_UPDATE)
				continue;
			before_total = eh_ev[i].u.jb.total;
			before_size = eh_ev[i].u.jb.size_ms;
		}
		CHECK(before_total > 100u);
		CHECK_UINT(before_size, 40);

		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.jb_init_ms = 120;
		tm_stream_config(&t, &w, &m);
		for (unsigned burst = 0; burst < 6u; burst++) {
			for (unsigned i = 0; i < 25u; i++)
				tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
			eh_wait_ms(&t, 250);
		}
		for (unsigned i = mark; i < eh_ev_len; i++) {
			if (eh_ev[i].type != UDSP_EV_JB_UPDATE)
				continue;
			after_total = eh_ev[i].u.jb.total;
			after_size = eh_ev[i].u.jb.size_ms;
		}
		/* jb.h wiring contract: the buffer is re-inited, so its output
		 * counters restart and the new target takes hold. */
		CHECK(after_total > 0u);
		CHECK(after_total < before_total);
		CHECK_UINT(after_size, 120);

		tm_call_down(&t, &w);
	}
}

/* --- cases: SRTP --------------------------------------------------------- */

static void tm_key_set(uint8_t *key, uint8_t seed)
{
	for (unsigned i = 0; i < 30u; i++)
		key[i] = (uint8_t)(seed + i * 7u);
}

/* The far end's session is the mirror of the near end's: our transmit key is
 * its receive key (test_srtp.c pattern). */
static bool tm_peer_srtp(struct udsp_srtp *peer, const struct udsp_media *m)
{
	struct udsp_media mirror = *m;

	memcpy(mirror.srtp.key_tx, m->srtp.key_rx, sizeof(mirror.srtp.key_tx));
	memcpy(mirror.srtp.key_rx, m->srtp.key_tx, sizeof(mirror.srtp.key_rx));

	return udsp_srtp_create(peer, &mirror) == 0;
}

static void tm_srtp_case(const char *name, uint8_t suite, size_t tag)
{
	_Alignas(uint32_t) uint8_t buf[TM_PKT_MAX];
	uint8_t plain[TM_PKT_MAX];
	uint8_t pay[80];
	int16_t pcm[80];
	struct eh_gate gate = { .fd = { -1, -1 } };
	struct udsp_stream *st;
	struct udsp_srtp peer;
	struct udsp_media m;
	struct tm_wire w;
	struct tm_pkt p;
	struct eh t;
	double phase = 0.0;
	unsigned want = 0;
	size_t out = 0;
	ssize_t n;

	CASE(name);
	if (!tm_call_up(&t, "USA", &w, 0))
		return;
	tm_media(&m, UDSP_SEL_PCMU, 0, 10);
	m.srtp.suite = suite;
	tm_key_set(m.srtp.key_tx, 0x10);
	tm_key_set(m.srtp.key_rx, 0x40);
	CHECK(tm_peer_srtp(&peer, &m));
	tm_stream_config(&t, &w, &m);
	tm_capture_tone(1000u);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	eh_wait_ms(&t, 60);

	n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300);
	CHECK(n > 0);
	if (n > 0) {
		CHECK_UINT((size_t)n, 12u + 80u + tag);
		CHECK(udsp_srtp_unprotect(&peer, buf, (size_t)n, sizeof(buf),
					  &out) == 0);
		CHECK_UINT(out, 92);
		CHECK(tm_rtp_parse(buf, out, &p));
	}

	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL);
	if (!st || !eh_gate_park(&gate))
		goto srtp_out;
	tm_tone_frame(pcm, 80, 1000.0, 8000.0, &phase);
	tm_ulaw_encode(pcm, 80, pay);
	for (unsigned i = 0; i < 40; i++) {
		size_t len;
		int rc;

		if (!(i % 20u) && !tm_gate_ring_prepare(st, 20, &want))
			goto srtp_out;
		len = tm_rtp_build(plain, 0, i == 0, w.seq, w.ts, w.ssrc,
				   pay, 80);
		memcpy(buf, plain, len);
		w.seq++;
		w.ts += 80;
		rc = udsp_srtp_protect(&peer, buf, len, sizeof(buf), &out);
		CHECK_UINT(rc, 0);
		if (rc)
			goto srtp_out;
		n = send(w.rtp_peer, buf, out, MSG_DONTWAIT | MSG_NOSIGNAL);
		CHECK_UINT(n, out);
		if (n != (ssize_t)out)
			goto srtp_out;
		if ((i + 1u) % 20u)
			continue;
		if (!tm_gate_ring_wait(st, want))
			goto srtp_out;
		if (i >= 20u)
			eh_rec_mark();
		if (!eh_gate_step(&gate, i < 20u ? 10u : 20u))
			goto srtp_out;
	}
	CHECK_UINT(eh_rec_have(), 1600);
	CHECK(eh_power(0, 800, 1000.0) > 1e6);

srtp_out:
	eh_gate_disarm(&gate);
	CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
	udsp_srtp_destroy(&peer);
	tm_call_close(&t, &w);
	eh_gate_close(&gate);
}

static void tm_srtp_restore_case(const char *name, bool restore_fails)
{
	_Alignas(uint32_t) uint8_t buf[TM_PKT_MAX];
	uint8_t pay[80] = { 0 };
	struct udsp_srtp peer;
	struct udsp_srtp session;
	struct udsp_media media;
	struct udsp_media next;
	uint_least32_t fail = UDSP_SRTP_REKEY_FAIL_RX | (uint_least32_t)EIO;
	size_t plain_len;
	size_t protected_len = 0;
	size_t recovered_len = 0;

	CASE(name);
	CHECK(udsp_srtp_init() == 0);
	tm_media(&media, UDSP_SEL_PCMU, 0, 10);
	media.srtp.suite = UDSP_SRTP_AES_CM_128_SHA1_80;
	tm_key_set(media.srtp.key_tx, 0x29);
	tm_key_set(media.srtp.key_rx, 0x61);
	CHECK(udsp_srtp_create(&session, &media) == 0);
	if (!restore_fails)
		CHECK(tm_peer_srtp(&peer, &media));

	next = media;
	tm_key_set(next.srtp.key_tx, 0x73);
	tm_key_set(next.srtp.key_rx, 0x17);
	if (restore_fails)
		fail |= UDSP_SRTP_REKEY_FAIL_RESTORE;
	atomic_store(&session.rekey_fail, fail);
	CHECK(udsp_srtp_rekey(&session, &next) == -EIO);

	plain_len = tm_rtp_build(buf, 0, true, 9100, 810000,
				 0x19375571u, pay, sizeof(pay));
	if (restore_fails) {
		CHECK(session.tx == NULL);
		CHECK(session.rx == NULL);
		CHECK(udsp_srtp_protect(&session, buf, plain_len, sizeof(buf),
					&protected_len) == -EINVAL);
	} else {
		CHECK(session.tx != NULL);
		CHECK(session.rx != NULL);
		CHECK_UINT(atomic_load(&session.rekey_fail), 0);
		CHECK(udsp_srtp_protect(&session, buf, plain_len, sizeof(buf),
					&protected_len) == 0);
		CHECK(udsp_srtp_unprotect(&peer, buf, protected_len, sizeof(buf),
					  &recovered_len) == 0);
		CHECK_UINT(recovered_len, plain_len);

		plain_len = tm_rtp_build(buf, 0, false, 9101, 810080,
					 0x19375571u, pay, sizeof(pay));
		CHECK(udsp_srtp_protect(&peer, buf, plain_len, sizeof(buf),
					&protected_len) == 0);
		CHECK(udsp_srtp_unprotect(&session, buf, protected_len,
					  sizeof(buf), &recovered_len) == 0);
		CHECK_UINT(recovered_len, plain_len);
		udsp_srtp_destroy(&peer);
	}
	udsp_srtp_destroy(&session);
}

static void case_srtp(void)
{
	tm_srtp_case("SRTP carries the stream with the 80 bit tag",
		     UDSP_SRTP_AES_CM_128_SHA1_80, 10);
	tm_srtp_case("SRTP carries the stream with the 32 bit tag",
		     UDSP_SRTP_AES_CM_128_SHA1_32, 4);

	{
		struct udsp_srtp session;
		struct udsp_media m;

		CASE("the SRTP fault injection preserves the same-key no-op");
		CHECK(udsp_srtp_init() == 0);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.srtp.suite = UDSP_SRTP_AES_CM_128_SHA1_80;
		tm_key_set(m.srtp.key_tx, 0x36);
		tm_key_set(m.srtp.key_rx, 0x69);
		CHECK(udsp_srtp_create(&session, &m) == 0);
		atomic_store(&session.rekey_fail, (uint_least32_t)ENOMEM);
		CHECK(udsp_srtp_rekey(&session, &m) == 0);
		CHECK_UINT(atomic_load(&session.rekey_fail), ENOMEM);
		atomic_store(&session.rekey_fail, 0u);
		udsp_srtp_destroy(&session);
	}

	tm_srtp_restore_case(
		"an SRTP receive rekey failure restores transmit protection",
		false);
	tm_srtp_restore_case(
		"a failed SRTP transmit restore destroys both sessions", true);

	{
		_Alignas(uint32_t) uint8_t buf[TM_PKT_MAX];
		uint8_t replay[TM_PKT_MAX];
		uint8_t pay[80] = { 0 };
		struct udsp_media_stats stats;
		struct udsp_srtp peer;
		struct udsp_media m;
		struct tm_wire w;
		struct eh t;
		size_t out = 0;
		size_t len;

		CASE("a same key reconfiguration keeps the replay window closed");
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.srtp.suite = UDSP_SRTP_AES_CM_128_SHA1_80;
		tm_key_set(m.srtp.key_tx, 0x21);
		tm_key_set(m.srtp.key_rx, 0x55);
		CHECK(tm_peer_srtp(&peer, &m));
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);

		/* RFC 3550 A.1 holds a new source's first packet until the
		 * second sequential packet proves the source; both count. */
		len = tm_rtp_build(buf, 0, true, w.seq++, w.ts, w.ssrc, pay, 80);
		CHECK(udsp_srtp_protect(&peer, buf, len, sizeof(buf), &out) == 0);
		CHECK(send(w.rtp_peer, buf, out, MSG_DONTWAIT | MSG_NOSIGNAL) > 0);
		w.ts += 80;
		len = tm_rtp_build(buf, 0, false, w.seq++, w.ts, w.ssrc, pay, 80);
		CHECK(udsp_srtp_protect(&peer, buf, len, sizeof(buf), &out) == 0);
		memcpy(replay, buf, out);
		CHECK(send(w.rtp_peer, buf, out, MSG_DONTWAIT | MSG_NOSIGNAL) > 0);
		eh_wait_ms(&t, 200);
		CHECK(udsp_stream_stats(t.s, 0, 0, &stats) == UDSP_OK);
		CHECK_UINT(stats.packets_recv, 2);

		/* A bare reconfiguration with the same keys must never create a
		 * fresh session, or libsrtp would forget the replay window and
		 * accept this captured packet again. */
		tm_stream_config(&t, &w, &m);
		eh_wait_ms(&t, 40);
		CHECK(send(w.rtp_peer, replay, out, MSG_DONTWAIT | MSG_NOSIGNAL) > 0);
		eh_wait_ms(&t, 200);
		/* The replayed packet never became a received packet: the
		 * session still holds the window the first one closed. */
		CHECK(udsp_stream_stats(t.s, 0, 0, &stats) == UDSP_OK);
		CHECK_UINT(stats.packets_recv, 2);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		udsp_srtp_destroy(&peer);
		tm_call_close(&t, &w);
	}

	{
		_Alignas(uint32_t) uint8_t buf[TM_PKT_MAX];
		uint8_t pay[80] = { 0 };
		struct udsp_media_stats stats;
		struct udsp_srtp peer;
		struct udsp_media m;
		struct udsp_stream *st;
		struct tm_wire w;
		struct eh t;
		size_t out = 0;
		size_t len;

		CASE("a rekey moves the stream to the new keys");
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.srtp.suite = UDSP_SRTP_AES_CM_128_SHA1_80;
		tm_key_set(m.srtp.key_tx, 0x31);
		tm_key_set(m.srtp.key_rx, 0x62);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);

		tm_key_set(m.srtp.key_tx, 0x77);
		tm_key_set(m.srtp.key_rx, 0x11);
		tm_stream_config(&t, &w, &m);
		CHECK(tm_peer_srtp(&peer, &m));
		eh_wait_ms(&t, 40);

		for (unsigned i = 0; i < 2u; i++) {
			len = tm_rtp_build(buf, 0, !i, (uint16_t)(9000 + i),
					   900000u + i * 80u, 0x77aa11bbu, pay,
					   80);
			CHECK(udsp_srtp_protect(&peer, buf, len, sizeof(buf),
						&out) == 0);
			CHECK(send(w.rtp_peer, buf, out,
				   MSG_DONTWAIT | MSG_NOSIGNAL) > 0);
		}
		eh_wait_ms(&t, 200);
		/* The packets the far end protected with the new keys are
		 * accepted, which the old session could not have done. */
		CHECK(udsp_stream_stats(t.s, 0, 0, &stats) == UDSP_OK);
		CHECK_UINT(stats.packets_recv, 2);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		if (st) {
			uint8_t zero[30] = { 0 };

			/* stream_stop zeroizes the key material it kept. */
			CHECK(!memcmp(st->media.srtp.key_tx, zero, sizeof(zero)));
			CHECK(!memcmp(st->media.srtp.key_rx, zero, sizeof(zero)));
			CHECK(!memcmp(st->srtp.key_tx, zero, sizeof(zero)));
			CHECK_UINT(st->media.srtp.suite, UDSP_SRTP_NONE);
		}
		udsp_srtp_destroy(&peer);
		tm_call_close(&t, &w);
	}

	{
		const struct udsp_codec_ops *old_ops;
		struct udsp_codec *old_enc;
		struct udsp_codec *old_dec;
		struct udsp_media next;
		struct udsp_media m;
		struct udsp_stream *st;
		struct tm_wire w;
		struct eh t;

		CASE("a failed SRTP rekey frees its codec pair and preserves ready state");
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.srtp.suite = UDSP_SRTP_AES_CM_128_SHA1_80;
		tm_key_set(m.srtp.key_tx, 0x2a);
		tm_key_set(m.srtp.key_rx, 0x64);
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			tm_call_close(&t, &w);
			return;
		}
		old_ops = st->ops;
		old_enc = st->enc;
		old_dec = st->dec;

		next = m;
		next.codec = UDSP_SEL_PCMA;
		next.codec_pt = 8;
		tm_key_set(next.srtp.key_tx, 0x75);
		tm_key_set(next.srtp.key_rx, 0x19);
		atomic_store(&st->srtp.rekey_fail, (uint_least32_t)ENOMEM);
		CHECK(udsp_stream_config(t.s, 0, 0, &next, w.rtp_local,
					 w.rtcp_local) == UDSP_TRANSIENT);
		CHECK_UINT(atomic_load(&st->srtp.rekey_fail), 0);
		CHECK_UINT(atomic_load(&st->state), STREAM_READY);
		CHECK(!atomic_load(&st->dead));
		CHECK(st->srtp_live);
		CHECK(st->ops == old_ops);
		CHECK(st->enc == old_enc);
		CHECK(st->dec == old_dec);
		CHECK_UINT(st->media.codec, UDSP_SEL_PCMU);
		CHECK(!memcmp(st->media.srtp.key_tx, m.srtp.key_tx,
			      sizeof(m.srtp.key_tx)));
		CHECK(!memcmp(st->srtp.key_tx, m.srtp.key_tx,
			      sizeof(m.srtp.key_tx)));

		tm_stream_config(&t, &w, &next);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		tm_call_down(&t, &w);
	}

	{
		_Alignas(uint32_t) uint8_t buf[TM_PKT_MAX];
		const struct udsp_codec_ops *old_ops;
		struct udsp_codec *old_enc;
		struct udsp_codec *old_dec;
		struct udsp_srtp peer;
		struct udsp_media next;
		struct udsp_media media;
		struct udsp_stream *st;
		struct tm_wire w;
		struct eh t;
		size_t plain_len = 0;
		ssize_t protected_len;

		CASE("a running stream survives a failed SRTP rekey on its old keys");
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&media, UDSP_SEL_PCMU, 0, 10);
		media.srtp.suite = UDSP_SRTP_AES_CM_128_SHA1_80;
		tm_key_set(media.srtp.key_tx, 0x47);
		tm_key_set(media.srtp.key_rx, 0x83);
		CHECK(tm_peer_srtp(&peer, &media));
		tm_stream_config(&t, &w, &media);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			udsp_srtp_destroy(&peer);
			tm_call_close(&t, &w);
			return;
		}
		old_ops = st->ops;
		old_enc = st->enc;
		old_dec = st->dec;
		eh_wait_ms(&t, 60);
		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;

		next = media;
		next.codec = UDSP_SEL_PCMA;
		next.codec_pt = 8;
		tm_key_set(next.srtp.key_tx, 0x92);
		tm_key_set(next.srtp.key_rx, 0x1d);
		atomic_store(&st->srtp.rekey_fail,
			     UDSP_SRTP_REKEY_FAIL_RX | (uint_least32_t)EIO);
		CHECK(udsp_stream_config(t.s, 0, 0, &next, w.rtp_local,
					 w.rtcp_local) == UDSP_TRANSIENT);
		CHECK_UINT(atomic_load(&st->state), STREAM_RUNNING);
		CHECK(!atomic_load(&st->dead));
		CHECK(st->ops == old_ops);
		CHECK(st->enc == old_enc);
		CHECK(st->dec == old_dec);
		CHECK_UINT(st->media.codec, UDSP_SEL_PCMU);

		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;
		eh_wait_ms(&t, 60);
		protected_len = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300);
		CHECK(protected_len > 0);
		if (protected_len > 0) {
			CHECK(udsp_srtp_unprotect(&peer, buf,
						  (size_t)protected_len, sizeof(buf),
						  &plain_len) == 0);
			CHECK_UINT(plain_len, 92);
		}

		tm_stream_config(&t, &w, &next);
		CHECK_UINT(atomic_load(&st->state), STREAM_RUNNING);
		CHECK(!atomic_load(&st->dead));
		CHECK_UINT(st->media.codec, UDSP_SEL_PCMA);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		udsp_srtp_destroy(&peer);
		tm_call_close(&t, &w);
	}

	/*
	 * The SRTP session is the one in-place mutation the reconfiguration
	 * cannot stage, so it runs after every other step. A rekey ahead of
	 * the descriptor duplication would move a live stream to keys the far
	 * end never learns about when a later step fails.
	 */
	{
		_Alignas(uint32_t) uint8_t buf[TM_PKT_MAX];
		uint8_t pay[80] = { 0 };
		struct udsp_media_stats stats;
		struct udsp_srtp peer;
		struct udsp_media m;
		struct rlimit rl, saved;
		struct tm_wire w;
		struct eh t;
		size_t out = 0;
		size_t len;
		int probe;

		CASE("a refused reconfiguration never touches the live SRTP keys");
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.srtp.suite = UDSP_SRTP_AES_CM_128_SHA1_80;
		tm_key_set(m.srtp.key_tx, 0x13);
		tm_key_set(m.srtp.key_rx, 0x71);
		CHECK(tm_peer_srtp(&peer, &m));
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);

		/* RFC 3550 A.1 holds the first packet until the second proves
		 * the source, so both packets count. */
		for (unsigned i = 0; i < 2u; i++) {
			len = tm_rtp_build(buf, 0, !i, w.seq++, w.ts, w.ssrc,
					   pay, 80);
			w.ts += 80;
			CHECK(udsp_srtp_protect(&peer, buf, len, sizeof(buf),
						&out) == 0);
			CHECK(send(w.rtp_peer, buf, out,
				   MSG_DONTWAIT | MSG_NOSIGNAL) > 0);
		}
		eh_wait_ms(&t, 200);
		CHECK(udsp_stream_stats(t.s, 0, 0, &stats) == UDSP_OK);
		CHECK_UINT(stats.packets_recv, 2);

		/* A full descriptor table refuses the reconfiguration at its
		 * first step, so the new keys must never reach the session. */
		CHECK(getrlimit(RLIMIT_NOFILE, &saved) == 0);
		probe = dup(0);
		CHECK(probe >= 0);
		if (probe >= 0)
			close(probe);
		rl = saved;
		rl.rlim_cur = (rlim_t)probe;
		CHECK(setrlimit(RLIMIT_NOFILE, &rl) == 0);
		tm_key_set(m.srtp.key_tx, 0x24);
		tm_key_set(m.srtp.key_rx, 0x99);
		CHECK(udsp_stream_config(t.s, 0, 0, &m, w.rtp_local,
					 w.rtcp_local) == UDSP_TRANSIENT);
		CHECK(setrlimit(RLIMIT_NOFILE, &saved) == 0);

		/* The far end still holds the original keys and its packets
		 * still arrive; a rekeyed session would refuse every one. */
		for (unsigned i = 0; i < 2u; i++) {
			len = tm_rtp_build(buf, 0, false, w.seq++, w.ts, w.ssrc,
					   pay, 80);
			w.ts += 80;
			CHECK(udsp_srtp_protect(&peer, buf, len, sizeof(buf),
						&out) == 0);
			CHECK(send(w.rtp_peer, buf, out,
				   MSG_DONTWAIT | MSG_NOSIGNAL) > 0);
		}
		eh_wait_ms(&t, 300);
		CHECK(udsp_stream_stats(t.s, 0, 0, &stats) == UDSP_OK);
		CHECK_UINT(stats.packets_recv, 4);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		udsp_srtp_destroy(&peer);
		tm_call_close(&t, &w);
	}

	/*
	 * The goodbye leaves while the engine still holds the descriptors, so
	 * it is still protected. A session destroyed ahead of the deactivation
	 * would put the last RTCP of the call on the wire in the clear, or
	 * lose it.
	 */
	{
		_Alignas(uint32_t) uint8_t buf[TM_PKT_MAX];
		struct udsp_srtp peer;
		struct udsp_media m;
		struct tm_wire w;
		struct eh t;
		unsigned types[8];
		unsigned count = 0;
		size_t out = 0;
		ssize_t n;

		CASE("the goodbye of an SRTP stream is protected");
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.srtp.suite = UDSP_SRTP_AES_CM_128_SHA1_80;
		tm_key_set(m.srtp.key_tx, 0x44);
		tm_key_set(m.srtp.key_rx, 0x88);
		CHECK(tm_peer_srtp(&peer, &m));
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);

		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		n = tm_recv(w.rtcp_peer, buf, sizeof(buf));
		CHECK(n > 0);
		if (n > 0) {
			/* Plain RTCP would parse straight away; this must not. */
			CHECK(!tm_rtcp_walk(buf, (size_t)n, types, 8, &count) ||
			      count < 3u);
			CHECK(udsp_srtp_unprotect_rtcp(&peer, buf, (size_t)n,
						       sizeof(buf), &out) == 0);
			CHECK(tm_rtcp_walk(buf, out, types, 8, &count));
			CHECK_UINT(count, 3);
			CHECK_UINT(types[2], 203);	/* goodbye */
		}
		udsp_srtp_destroy(&peer);
		tm_call_close(&t, &w);
	}
}

/* --- cases: voice activity and mute -------------------------------------- */

static void case_vad(void)
{
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;

	CASE("the voice detector holds the talkspurt and sends comfort noise");
	{
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p;
		unsigned media_pkts = 0;
		unsigned cn_pkts = 0;
		unsigned cn_level = 0;
		bool marked = false;
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.vad = true;
		m.cng = true;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);

		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (!tm_rtp_parse(buf, (size_t)n, &p))
				continue;
			if (p.pt == 13u) {
				cn_pkts++;
				cn_level = p.payload[0];
				CHECK_UINT(p.len, 1);
				continue;
			}
			media_pkts++;
		}
		/* RFC 3389: one comfort-noise description opens the silence. */
		CHECK_UINT(cn_pkts, 1);
		CHECK_UINT(media_pkts, 0);
		CHECK(cn_level > 0u);

		tm_capture_tone(1000u);
		eh_wait_ms(&t, 150);
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (!tm_rtp_parse(buf, (size_t)n, &p) || p.pt != 0u)
				continue;
			if (!media_pkts)
				marked = p.marker;
			media_pkts++;
		}
		CHECK(media_pkts > 5u);
		/* RFC 3551: the packet that reopens the talkspurt is marked. */
		CHECK(marked);
		tm_call_down(&t, &w);
	}

	/*
	 * The silence branch counts frames into the same aggregate the voice
	 * branch uses. A talkspurt that reopens on a stale count builds its
	 * first packet from fewer frames than the timestamp step it carries: a
	 * ptime 30 packet of 20 ms of audio under a 240 sample step. The four
	 * cycles below differ by one tick each, so the resume falls on every
	 * phase of the ptime 30 cycle.
	 */
	CASE("a talkspurt reopens on a full packet, never a partial one");
	{
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p;
		unsigned media = 0;
		unsigned first_seen = 0;
		uint32_t prev_ts = 0;
		bool have_prev = false;
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 1 << 20))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 30);
		m.vad = true;
		m.cng = true;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		atomic_store(&tm_be.tone_hz, 1000u);

		for (unsigned i = 0; i < 4u; i++) {
			atomic_store(&tm_be.src, TM_SRC_SILENCE);
			eh_wait_ms(&t, 200u + i * 10u);
			while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
				continue;
			atomic_store(&tm_be.src, TM_SRC_TONE);
			eh_wait_ms(&t, 300);
			first_seen = 0;
			while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
				if (!tm_rtp_parse(buf, (size_t)n, &p) || p.pt != 0)
					continue;
				/* Full ptime of real audio, every packet. */
				CHECK_UINT(p.len, 240);
				CHECK(tm_payload_power(p.payload, p.len, false,
						       1000.0) > 1e6);
				if (have_prev && !first_seen)
					CHECK(p.ts - prev_ts >= 240u);
				if (have_prev && first_seen)
					CHECK_UINT(p.ts - prev_ts, 240);
				prev_ts = p.ts;
				have_prev = true;
				first_seen++;
				media++;
			}
			CHECK(first_seen > 3u);
		}
		CHECK(media > 20u);
		tm_call_down(&t, &w);
	}

	CASE("mute silences the media in both directions");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct udsp_stream *st;
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p;
		double phase = 0.0;
		double loud = 0.0;
		double quiet = 0.0;
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st || !eh_gate_park(&gate) ||
		    !tm_gate_tone_send(st, &w, 30, &phase) ||
		    !eh_gate_step(&gate, 15))
			goto mute_out;
		eh_rec_mark();
		if (!eh_gate_step(&gate, 10))
			goto mute_out;
		CHECK_UINT(eh_rec_have(), 800);
		if (eh_rec_have() != 800u)
			goto mute_out;
		loud = eh_energy(0, 400);

		/* The far end goes quiet both ways while the local playout
		 * keeps its own path. */
		atomic_store(&t.s->chan[0].mute, true);
		if (!tm_gate_tone_send(st, &w, 30, &phase) ||
		    !eh_gate_step(&gate, 15))
			goto mute_out;
		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;
		eh_rec_mark();
		if (!eh_gate_step(&gate, 10))
			goto mute_out;
		CHECK_UINT(eh_rec_have(), 800);
		if (eh_rec_have() != 800u)
			goto mute_out;
		quiet = eh_energy(0, 400);

		CHECK(loud > 1e6);
		CHECK(quiet < 10.0);
		n = tm_recv(w.rtp_peer, buf, sizeof(buf));
		CHECK(n > 0);
		if (n > 0 && tm_rtp_parse(buf, (size_t)n, &p)) {
			double sum = 0.0;

			for (unsigned i = 0; i < p.len; i++) {
				int16_t v = udsp_ulaw_decode(p.payload[i]);

				sum += (double)v * v;
			}
			CHECK(sum / p.len < 100.0);
		}
	mute_out:
		atomic_store(&t.s->chan[0].mute, false);
		eh_gate_disarm(&gate);
		tm_call_down(&t, &w);
		eh_gate_close(&gate);
	}
}

/* --- cases: telephone events --------------------------------------------- */

/* The lost-end bound of a rendered event, shrunk from the production 120 ms
 * so a case can let it pass (README, test timing). main sets it in the
 * environment before the first open. */
#define TM_TEV_TIMEOUT_MS	40u
#define TM_TEV_STEP_GAP_US	25000u
#define TM_TEV_MARGIN_US	10000u
#define TM_TEV_POST_LIMIT_US	40000u
#define TM_TEV_ATTEMPTS		3u
#define TM_TEV_PRIME_PACKETS	4u

static void tm_ev_drain(struct eh *t, unsigned wall_ms);

/* PCMU silence, 10 ms per packet. RFC 3550 A.1 holds a new source's first
 * packet until its successor proves the source, so the first event packet of
 * a case that sends none of these waits, and the render starts one packet
 * late. */
static void tm_telev_prime(struct tm_wire *w, unsigned count)
{
	uint8_t pkt[TM_PKT_MAX];
	uint8_t pay[80];
	size_t len;

	memset(pay, 0xff, sizeof(pay));
	for (unsigned i = 0; i < count; i++) {
		len = tm_rtp_build(pkt, 0, !i, w->seq++, w->ts, w->ssrc, pay,
				   sizeof(pay));
		w->ts += 80u;
		if (send(w->rtp_peer, pkt, len,
			 MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
			return;
	}
}

/* One RFC 4733 packet of the far end's, at the event timestamp the wire keeps
 * for the whole event. */
static void tm_telev_send(struct tm_wire *w, uint8_t code, uint8_t volume,
			  bool ending, uint16_t duration, bool marker)
{
	uint8_t pkt[TM_PKT_MAX];
	uint8_t pay[4];
	size_t len;

	pay[0] = code;
	pay[1] = (uint8_t)(volume | (ending ? 0x80u : 0u));
	pay[2] = (uint8_t)(duration >> 8);
	pay[3] = (uint8_t)duration;
	len = tm_rtp_build(pkt, 101, marker, w->seq++, w->ts, w->ssrc, pay, 4);
	if (send(w->rtp_peer, pkt, len, MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
		return;
}

static bool tm_gate_telev_prime(struct eh_gate *gate, struct udsp_stream *st,
				struct tm_wire *w, unsigned frames)
{
	unsigned want;

	if (!tm_gate_ring_prepare(st, TM_TEV_PRIME_PACKETS, &want))
		return false;
	tm_telev_prime(w, TM_TEV_PRIME_PACKETS);

	return tm_gate_ring_wait(st, want) && eh_gate_step(gate, frames);
}

static bool tm_gate_telev_send(struct udsp_stream *st, struct tm_wire *w,
			       uint8_t code, uint8_t volume, bool ending,
			       uint16_t duration, bool marker, unsigned packets)
{
	unsigned want;

	if (!tm_gate_ring_prepare(st, packets, &want))
		return false;
	for (unsigned i = 0; i < packets; i++)
		tm_telev_send(w, code, volume, ending, duration, marker);

	return tm_gate_ring_wait(st, want);
}

static bool tm_gate_telev_step(struct eh_gate *gate, unsigned frames,
			       uint64_t *previous, bool *scheduled)
{
	uint64_t now;

	if (!eh_gate_step(gate, frames))
		return false;
	now = eh_mono_us();
	/* A late update changes the stimulus by expiring the wire event. */
	if (scheduled && now - *previous > TM_TEV_STEP_GAP_US)
		*scheduled = false;
	*previous = now;

	return true;
}

static bool tm_gate_telev_event_run(struct eh_gate *gate,
				    struct udsp_stream *st,
				    struct tm_wire *w, uint8_t code,
				    uint8_t volume, unsigned packets,
				    bool ending, bool *scheduled)
{
	uint64_t previous = eh_mono_us();

	for (unsigned i = 0; i < packets; i++) {
		if (!tm_gate_telev_send(st, w, code, volume, false,
					(uint16_t)(i * 160u), !i, 1) ||
		    !tm_gate_telev_step(gate, 2, &previous, scheduled))
			return false;
		if (scheduled && !*scheduled)
			return true;
	}
	if (!ending)
		return true;

	return tm_gate_telev_send(st, w, code, volume, true,
				  (uint16_t)(packets * 160u), false,
				  UDSP_TELEV_REPEAT) &&
	       tm_gate_telev_step(gate, 1, &previous, scheduled);
}

static bool tm_gate_telev_timeout_run(struct eh_gate *gate,
				      struct udsp_stream *st,
				      bool *scheduled)
{
	uint64_t deadline = st->tev_play.last_ns / 1000u +
			    TM_TEV_TIMEOUT_MS * 1000u;
	bool sampled = false;
	bool loud;

	/* Wall-clock checkpoints allow one gate step plus scheduling slop. */
	while (eh_mono_us() + TM_TEV_MARGIN_US < deadline) {
		eh_rec_mark();
		if (!eh_gate_step(gate, 1))
			return false;
		if (eh_mono_us() >= deadline) {
			*scheduled = false;
			return true;
		}
		sampled = true;
	}
	if (!sampled) {
		*scheduled = false;
		return true;
	}
	CHECK_UINT(eh_rec_have(), 80);
	if (eh_rec_have() != 80u)
		return false;
	loud = tm_rec_loud_frames() > 0u;
	while (eh_mono_us() < deadline + TM_TEV_MARGIN_US) {
		if (!eh_gate_step(gate, 1))
			return false;
	}
	eh_rec_mark();
	if (!eh_gate_step(gate, 20))
		return false;
	if (eh_mono_us() > deadline + TM_TEV_POST_LIMIT_US) {
		*scheduled = false;
		return true;
	}
	CHECK(loud);

	return true;
}

static bool tm_telev_up(struct eh *t, struct tm_wire *w, struct udsp_media *m)
{
	if (!tm_up(t, "USA") || !tm_wire_open(w, 0))
		return false;
	tm_media(m, UDSP_SEL_PCMU, 0, 10);
	m->dtmf_mode = UDSP_DTMF_RFC2833;
	m->dtmf_pt = 101;
	if (udsp_stream_config(t->s, 0, 0, m, w->rtp_local, w->rtcp_local) !=
	    UDSP_OK)
		return false;

	return udsp_stream_start(t->s, 0, 0) == UDSP_OK;
}

static bool tm_telev_render_attempt(void)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	bool scheduled = true;
	unsigned frames;

	if (!tm_telev_up(&t, &w, &m)) {
		CHECK(false);
		return true;
	}
	CHECK(udsp_cap(t.s)->features & UDSP_FEAT_DTMF_PLAYOUT);
	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL);
	if (!st || !eh_gate_park(&gate) ||
	    !tm_gate_telev_prime(&gate, st, &w, 6))
		goto out;
	eh_rec_mark();
	if (!tm_gate_telev_event_run(&gate, st, &w, 5, 10, 10, true,
				     &scheduled) ||
	    !scheduled || !eh_gate_step(&gate, 9))
		goto out;
	CHECK_UINT(eh_rec_have(), 2400);
	if (eh_rec_have() != 2400u)
		goto out;
	CHECK_UINT(tm_rec_digit(), '5');
	frames = tm_rec_loud_frames();
	CHECK(frames >= 15u);
	CHECK_UINT(frames, 20);

out:
	eh_gate_disarm(&gate);
	tm_call_down(&t, &w);
	eh_gate_close(&gate);

	return scheduled;
}

static bool tm_telev_end_attempt(void)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	const struct udsp_event *e;
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	bool scheduled = true;

	if (!tm_telev_up(&t, &w, &m)) {
		CHECK(false);
		return true;
	}
	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL);
	if (!st || !eh_gate_park(&gate) ||
	    !tm_gate_telev_prime(&gate, st, &w, 6) ||
	    !tm_gate_telev_event_run(&gate, st, &w, 5, 10, 10, true,
				     &scheduled) ||
	    !scheduled || !eh_gate_step(&gate, 19))
		goto out;
	eh_rec_mark();
	if (!eh_gate_step(&gate, 20))
		goto out;
	CHECK_UINT(eh_rec_have(), 1600);
	if (eh_rec_have() != 1600u)
		goto out;
	CHECK_UINT(tm_rec_loud_frames(), 0);
	CHECK_UINT(tm_rec_digit(), 0);
	eh_gate_disarm(&gate);
	tm_ev_drain(&t, 100);
	CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);
	e = eh_ev_find(UDSP_EV_DIGIT, 0);
	CHECK(e != NULL);
	if (e) {
		CHECK_UINT(e->u.digit.digit, '5');
		CHECK_UINT(e->u.digit.remote, 1);
	}

out:
	eh_gate_disarm(&gate);
	tm_call_down(&t, &w);
	eh_gate_close(&gate);

	return scheduled;
}

static bool tm_telev_timeout_attempt(void)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	bool scheduled = true;
	unsigned frames;
	char digit;

	if (!tm_telev_up(&t, &w, &m)) {
		CHECK(false);
		return true;
	}
	CHECK_UINT(t.s->tev_play_timeout_ms, TM_TEV_TIMEOUT_MS);
	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL);
	if (!st || !eh_gate_park(&gate) ||
	    !tm_gate_telev_prime(&gate, st, &w, 6))
		goto out;
	eh_rec_mark();
	if (!tm_gate_telev_event_run(&gate, st, &w, 5, 10, 5, false,
				     &scheduled) ||
	    !scheduled)
		goto out;
	CHECK_UINT(eh_rec_have(), 800);
	if (eh_rec_have() != 800u)
		goto out;
	digit = tm_rec_digit();
	frames = tm_rec_loud_frames();
	if (!tm_gate_telev_timeout_run(&gate, st, &scheduled) || !scheduled)
		goto out;
	CHECK_UINT(digit, '5');
	CHECK(frames > 0u);
	CHECK_UINT(eh_rec_have(), 1600);
	if (eh_rec_have() != 1600u)
		goto out;
	CHECK_UINT(tm_rec_loud_frames(), 0);

out:
	eh_gate_disarm(&gate);
	tm_call_down(&t, &w);
	eh_gate_close(&gate);

	return scheduled;
}

static bool tm_telev_hold_attempt(void)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	const struct udsp_event *e;
	struct udsp_chan_cfg ccfg;
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	struct tm_pkt p;
	struct eh t;
	uint8_t buf[TM_PKT_MAX];
	bool scheduled = true;
	unsigned telev = 0;
	ssize_t n;

	if (!tm_telev_up(&t, &w, &m)) {
		CHECK(false);
		return true;
	}
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.detect_mask = UDSP_DET_DTMF;
	CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
	atomic_store(&tm_be.echo_delay, 3u * (UDSP_SAMPLE_RATE / 100u));
	atomic_store(&tm_be.echo_gain, 1024u);
	atomic_store(&tm_be.src, TM_SRC_ECHO);
	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL);
	if (!st || !eh_gate_park(&gate) ||
	    !tm_gate_telev_prime(&gate, st, &w, 10))
		goto out;
	eh_rec_mark();
	while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
		continue;
	if (!tm_gate_telev_event_run(&gate, st, &w, 5, 10, 20, true,
				     &scheduled) ||
	    !scheduled || !eh_gate_step(&gate, 29))
		goto out;
	CHECK_UINT(eh_rec_have(), 5600);
	if (eh_rec_have() != 5600u)
		goto out;
	atomic_store(&tm_be.src, TM_SRC_SILENCE);
	/* A recorded digit excludes an empty feed as the reason for no echo. */
	CHECK_UINT(tm_rec_digit(), '5');
	eh_gate_disarm(&gate);
	tm_ev_drain(&t, 100);
	CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);
	e = eh_ev_find(UDSP_EV_DIGIT, 0);
	CHECK(e != NULL);
	if (e)
		CHECK_UINT(e->u.digit.remote, 1);
	while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
		if (tm_rtp_parse(buf, (size_t)n, &p) && p.pt == 101u)
			telev++;
	}
	CHECK_UINT(telev, 0);

out:
	eh_gate_disarm(&gate);
	tm_call_down(&t, &w);
	eh_gate_close(&gate);

	return scheduled;
}

static void tm_telev_retry(bool (*attempt)(void), const char *name)
{
	for (unsigned i = 0; i < TM_TEV_ATTEMPTS; i++) {
		if (attempt())
			return;
	}
	fprintf(stderr, "%s: three schedule misses; host too loaded\n", name);
	CHECK(false);
}

static void case_telev(void)
{
	struct tm_wire w;
	struct udsp_media m;
	struct eh t;

	CASE("a telephone event leaves with its three end packets");
	{
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p;
		unsigned events = 0;
		unsigned ends = 0;
		uint32_t ts = 0;
		uint16_t last_duration = 0;
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 40);
		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;

		CHECK(udsp_stream_dtmf(t.s, 0, 0, '5', 100) == UDSP_OK);
		eh_wait_ms(&t, 300);
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (!tm_rtp_parse(buf, (size_t)n, &p) || p.pt != 101)
				continue;
			CHECK_UINT(p.len, 4);
			events++;
			if (!ts)
				ts = p.ts;
			CHECK_UINT(p.ts, ts);
			CHECK_UINT(p.payload[0], 5);
			CHECK_UINT(p.payload[1] & 0x3fu, 10);
			last_duration = (uint16_t)((p.payload[2] << 8) |
						   p.payload[3]);
			if (p.payload[1] & 0x80u)
				ends++;
		}
		CHECK(events >= 4u);
		CHECK_UINT(ends, 3);
		CHECK_UINT(last_duration, 800);
		tm_call_down(&t, &w);
	}

	CASE("a telephone event from the wire reports the digit and its length");
	{
		uint8_t pkt[TM_PKT_MAX];
		uint8_t pay[4];
		size_t len;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 40);

		pay[0] = 11;			/* '#' */
		pay[1] = 10;
		pay[2] = 0;
		pay[3] = 80;
		len = tm_rtp_build(pkt, 101, true, w.seq++, w.ts, w.ssrc, pay, 4);
		CHECK(send(w.rtp_peer, pkt, len, MSG_DONTWAIT | MSG_NOSIGNAL) > 0);
		eh_wait_ms(&t, 30);
		pay[1] = 10 | 0x80u;
		pay[2] = (uint8_t)(1600u >> 8);
		pay[3] = (uint8_t)1600u;
		len = tm_rtp_build(pkt, 101, false, w.seq++, w.ts, w.ssrc, pay, 4);
		CHECK(send(w.rtp_peer, pkt, len, MSG_DONTWAIT | MSG_NOSIGNAL) > 0);
		eh_wait_ms(&t, 60);

		{
			const struct udsp_event *e =
				eh_ev_find(UDSP_EV_DIGIT, 0);

			CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);
			CHECK(e != NULL);
			if (e) {
				CHECK_UINT(e->u.digit.digit, '#');
				CHECK_UINT(e->u.digit.remote, 1);
				CHECK_UINT(e->u.digit.duration_ms, 200);
				CHECK_UINT(e->stream, 0);
			}
		}
		tm_call_down(&t, &w);
	}

	CASE("a digit still on the wire refuses the next one");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 40);
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '1', 200) == UDSP_OK);
		/* Back pressure, not a bad request: the event is still out. */
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '2', 200) == UDSP_TRANSIENT);
		eh_wait_ms(&t, 400);
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '2', 100) == UDSP_OK);
		tm_call_down(&t, &w);
	}

	CASE("a wire digit never acknowledges a type 2 caller identity");
	{
		struct udsp_chan_cfg ccfg;
		struct udsp_cid cid;
		uint8_t pkt[TM_PKT_MAX];
		uint8_t pay[4];
		size_t len;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_DTMF;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) == UDSP_OK);
		eh_wait_ms(&t, 50);

		memset(&cid, 0, sizeof(cid));
		snprintf(cid.number, sizeof(cid.number), "%s", "5559876");
		snprintf(cid.name, sizeof(cid.name), "%s", "WAITING");
		snprintf(cid.blockinfo, sizeof(cid.blockinfo), "%s", "O");
		CHECK(udsp_cid_send(t.s, 0, UDSP_CID_TYPE2, &cid) == UDSP_OK);

		/* 'D' is an acknowledgment digit, but only from the CPE on the
		 * UL path: the far end never heard the alerting tone. Four of
		 * them cross the whole acknowledgment window. */
		for (unsigned i = 0; i < 4u; i++) {
			eh_wait_ms(&t, 150);
			pay[0] = 15;			/* 'D' */
			pay[1] = 10 | 0x80u;		/* end bit */
			pay[2] = 0;
			pay[3] = 80;
			len = tm_rtp_build(pkt, 101, !i, w.seq++, w.ts, w.ssrc,
					   pay, 4);
			w.ts += 800;
			CHECK(send(w.rtp_peer, pkt, len,
				   MSG_DONTWAIT | MSG_NOSIGNAL) > 0);
		}
		eh_wait_ms(&t, 500);

		CHECK(eh_ev_count(UDSP_EV_DIGIT) >= 1u);
		{
			const struct udsp_event *d =
				eh_ev_find(UDSP_EV_DIGIT, 0);

			if (d) {
				CHECK_UINT(d->u.digit.remote, 1);
				CHECK_UINT(d->u.digit.digit, 'D');
			}
		}
		/* The sequence timed out exactly as it does with no digit at
		 * all, and no burst was ever sent. */
		CHECK_UINT(eh_ev_count(UDSP_EV_CID2_TIMEOUT), 1);
		CHECK_UINT(eh_ev_count(UDSP_EV_CID_SENT), 0);
		tm_call_down(&t, &w);
	}

	/* An RFC 4733 packet is media, so a direction that does not send has
	 * no wire to put one on. */
	CASE("a direction that does not send refuses the digit");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		m.direction = UDSP_MEDIA_RECVONLY;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 40);
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '5', 100) == UDSP_UNSUPPORTED);
		m.direction = UDSP_MEDIA_INACTIVE;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '5', 100) == UDSP_UNSUPPORTED);
		/* The same stream takes it once the direction sends again. */
		m.direction = UDSP_MEDIA_SENDRECV;
		tm_stream_config(&t, &w, &m);
		eh_wait_ms(&t, 40);
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '5', 100) == UDSP_OK);
		tm_call_down(&t, &w);
	}

	/*
	 * A reconfiguration to a non-sending direction cuts an event in flight.
	 * No further packet may leave, now or when the direction comes back,
	 * and the far end closes the digit on the ends it already saw (RFC 4733
	 * 2.5.1.4). An event left running would resume on the wire the moment
	 * the stream sends again.
	 */
	CASE("a reconfiguration to a non-sending direction cuts the event");
	{
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p;
		unsigned during = 0;
		unsigned after_cut = 0;
		unsigned after_back = 0;
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 40);
		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;

		/* Long enough that the event is still running at the cut. */
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '7', 4000) == UDSP_OK);
		eh_wait_ms(&t, 200);
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (tm_rtp_parse(buf, (size_t)n, &p) && p.pt == 101)
				during++;
		}
		CHECK(during > 5u);

		m.direction = UDSP_MEDIA_RECVONLY;
		tm_stream_config(&t, &w, &m);
		eh_wait_ms(&t, 100);
		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;
		eh_wait_ms(&t, 400);
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (tm_rtp_parse(buf, (size_t)n, &p) && p.pt == 101)
				after_cut++;
		}
		CHECK_UINT(after_cut, 0);

		m.direction = UDSP_MEDIA_SENDRECV;
		tm_stream_config(&t, &w, &m);
		eh_wait_ms(&t, 400);
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (tm_rtp_parse(buf, (size_t)n, &p) && p.pt == 101)
				after_back++;
		}
		CHECK_UINT(after_back, 0);
		/* Cut, not held: a fresh digit is accepted at once. */
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '3', 100) == UDSP_OK);
		tm_call_down(&t, &w);
	}

	/*
	 * telev_sender_start latches the sender timestamp, so the media clock
	 * has to step over the partial aggregate the digit discards before it
	 * latches. Without the step the digit is stamped up to two frames back
	 * inside audio the far end already played out. The six digits below are
	 * spaced one tick apart, so their aggregate phases cover the whole
	 * ptime 30 cycle whatever the constant ack latency is.
	 */
	CASE("a digit is stamped where the media clock stands");
	{
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p;
		unsigned ahead = 0;
		unsigned digits = 0;
		double phase = 0.0;
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 1 << 20))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 30);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		(void)phase;

		for (unsigned i = 0; i < 6u; i++) {
			uint32_t media_end = 0;
			bool have_media = false;

			eh_wait_ms(&t, 200u + i * 10u);
			while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
				if (!tm_rtp_parse(buf, (size_t)n, &p) || p.pt != 0)
					continue;
				CHECK_UINT(p.len, 240);
				media_end = p.ts + 240u;
				have_media = true;
			}
			CHECK(have_media);
			CHECK(udsp_stream_dtmf(t.s, 0, 0, '4', 100) == UDSP_OK);
			eh_wait_ms(&t, 300);
			while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
				uint32_t step;

				if (!tm_rtp_parse(buf, (size_t)n, &p))
					continue;
				/* A media packet can still race the digit out
				 * of the socket. It is audio the clock already
				 * carried, so it moves the mark the digit is
				 * measured against. Skipping it would measure
				 * the digit against a stale packet and fail a
				 * correct sender. */
				if (p.pt == 0 && have_media) {
					CHECK_UINT(p.len, 240);
					media_end = p.ts + 240u;
					continue;
				}
				if (p.pt != 101 || !have_media)
					continue;
				step = p.ts - media_end;
				/* Never behind the audio already sent, and at
				 * most the two frames an aggregate can hold. */
				CHECK(step <= 160u);
				if (step)
					ahead++;
				digits++;
				have_media = false;
			}
		}
		CHECK(digits >= 5u);
		/* At least one digit falls on a partial aggregate, which is
		 * exactly the case a missing settle stamps early. */
		CHECK(ahead > 0u);
		tm_call_down(&t, &w);
	}

	CASE("an unnegotiated event payload type refuses the digit");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 0;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '5', 100) == UDSP_UNSUPPORTED);
		tm_call_down(&t, &w);
	}

	/*
	 * The library renders the event the far end sends toward the line
	 * itself, so the subscriber hears the digit the far end pressed
	 * (udsp.h UDSP_FEAT_DTMF_PLAYOUT). The oracle is the playback the
	 * backend recorded, read by a detector of its own.
	 */
	CASE("a wire event renders the digit toward the line");
	tm_telev_retry(tm_telev_render_attempt, "wire digit");

	/* The end bit stops the render, and the caller still gets the report
	 * of a remote digit at the end of the event. */
	CASE("the end bit stops the render and the report still says remote");
	tm_telev_retry(tm_telev_end_attempt, "wire end bit");

	/* The override shortens the wait; test_rtp pins the default. */
	CASE("a lost end stops the render at the timeout");
	tm_telev_retry(tm_telev_timeout_attempt, "pulse bracket");

	/* The render follows the decode: a direction that does not receive and
	 * a muted line both play nothing of the far end's. */
	CASE("a stream that plays no far end audio renders no wire digit");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct udsp_stream *st;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		m.direction = UDSP_MEDIA_SENDONLY;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st || !eh_gate_park(&gate) ||
		    !tm_gate_telev_prime(&gate, st, &w, 6))
			goto telev_quiet_out;
		eh_rec_mark();
		if (!tm_gate_telev_event_run(&gate, st, &w, 5, 10, 10, true,
					     NULL) ||
		    !eh_gate_step(&gate, 9))
			goto telev_quiet_out;
		CHECK_UINT(eh_rec_have(), 2400);
		if (eh_rec_have() != 2400u)
			goto telev_quiet_out;
		CHECK_UINT(tm_rec_loud_frames(), 0);

		eh_gate_disarm(&gate);
		m.direction = UDSP_MEDIA_SENDRECV;
		tm_stream_config(&t, &w, &m);
		atomic_store(&t.s->chan[0].mute, true);
		if (!eh_gate_park(&gate) ||
		    !tm_gate_telev_prime(&gate, st, &w, 6))
			goto telev_quiet_out;
		eh_rec_mark();
		if (!tm_gate_telev_event_run(&gate, st, &w, 6, 10, 10, true,
					     NULL) ||
		    !eh_gate_step(&gate, 9))
			goto telev_quiet_out;
		CHECK_UINT(eh_rec_have(), 2400);
		if (eh_rec_have() != 2400u)
			goto telev_quiet_out;
		CHECK_UINT(tm_rec_loud_frames(), 0);

	telev_quiet_out:
		atomic_store(&t.s->chan[0].mute, false);
		eh_gate_disarm(&gate);
		tm_call_down(&t, &w);
		eh_gate_close(&gate);
	}

	/*
	 * The echo of the digit the library rendered reaches the uplink
	 * detector's feed. The line holds that detector, so the digit is
	 * reported once, as the far end's, and no event of this box's own
	 * goes back on the wire (core/chan.c chan_dtmf_hold).
	 */
	CASE("a rendered wire digit is held from the detector");
	tm_telev_retry(tm_telev_hold_attempt, "wire digit hold");
}

/* --- cases: the jitter buffer on the wire -------------------------------- */

/* One 20 ms PCMU packet of near-silence, built into buf. */
static size_t tm_jb1_pkt(uint8_t *buf, bool marker, uint16_t seq, uint32_t ts,
			 uint32_t ssrc)
{
	uint8_t pay[160];

	memset(pay, 0xff, sizeof(pay));
	return tm_rtp_build(buf, 0, marker, seq, ts, ssrc, pay, sizeof(pay));
}

/* A larger reserve can arm skew correction and hide the measured hole. */
#define TM_JB1_CUSHION_MS	60u
#define TM_JB1_CUSHION_MAX	6u

static bool tm_jb1_send(struct tm_wire *w, bool marker, uint16_t *seq,
			uint32_t *ts)
{
	uint8_t pkt[TM_PKT_MAX];
	size_t len = tm_jb1_pkt(pkt, marker, (*seq)++, *ts, w->ssrc);
	ssize_t sent;

	*ts += 160;
	sent = send(w->rtp_peer, pkt, len, MSG_DONTWAIT | MSG_NOSIGNAL);
	CHECK_UINT(sent, len);

	return sent == (ssize_t)len;
}

static bool tm_jb1_publish(struct eh *t, struct tm_wire *w, unsigned n,
			   bool mark_first, uint16_t *seq, uint32_t *ts)
{
	struct udsp_stream *st = stream_at(t->s, 0, 0);
	unsigned want;

	if (!tm_gate_ring_prepare(st, n, &want))
		return false;
	for (unsigned i = 0; i < n; i++) {
		if (!tm_jb1_send(w, mark_first && i == 0, seq, ts))
			return false;
	}

	return tm_gate_ring_wait(st, want);
}

/* Exact packet-duration steps preserve the reserve without a large burst. */
static bool tm_jb1_run(struct eh_gate *gate, struct eh *t, struct tm_wire *w,
		       unsigned n, bool mark_first, uint16_t *seq, uint32_t *ts)
{
	for (unsigned i = 0; i < n; i++) {
		if (!tm_jb1_publish(t, w, 1, mark_first && i == 0, seq, ts) ||
		    !eh_gate_step(gate, 2))
			return false;
	}

	return true;
}

static uint16_t tm_jb1_depth(struct eh *t)
{
	struct udsp_jb_stats js;

	if (udsp_stream_jb_stats(t->s, 0, 0, &js) != UDSP_OK)
		return 0;
	return js.depth_ms;
}

static bool tm_jb1_cushion(struct eh_gate *gate, struct eh *t,
			   struct tm_wire *w, uint16_t *seq, uint32_t *ts)
{
	for (unsigned i = 0; i < TM_JB1_CUSHION_MAX; i++) {
		if (tm_jb1_depth(t) >= TM_JB1_CUSHION_MS)
			return true;
		if (!tm_jb1_publish(t, w, 1, false, seq, ts) ||
		    !eh_gate_step(gate, 1))
			return false;
	}

	return true;
}

static uint64_t tm_jb1_plc(struct eh *t)
{
	struct udsp_jb_stats js;

	if (udsp_stream_jb_stats(t->s, 0, 0, &js) != UDSP_OK)
		return 0;
	return js.plc;
}

/*
 * The parked read follows the last completed tick's snapshot publication.
 * Its reserve cannot drain before the closing packet reaches the receive ring.
 * The floor requires one hole plus one frame, so unrelated starvation cannot
 * supply the measured concealment.
 */
static bool tm_jb1_hole(struct eh_gate *gate, struct eh *t, struct tm_wire *w,
			bool mark, uint16_t *seq, uint32_t *ts, uint64_t *cost)
{
	uint64_t before;

	if (!tm_jb1_cushion(gate, t, w, seq, ts))
		return false;
	CHECK(tm_jb1_depth(t) >= 30u);
	if (tm_jb1_depth(t) < 30u)
		return false;
	before = tm_jb1_plc(t);
	*ts += 160;
	if (!tm_jb1_run(gate, t, w, 4, mark, seq, ts))
		return false;
	*cost = tm_jb1_plc(t) - before;

	return true;
}

/*
 * The jitter buffer end to end: a coalesced datagram split and the marker must
 * survive the stream plane, not only rtp.c and jb.c.
 *
 * The datagram carries two 20 ms packets. Read as one packet, it gives a
 * 332-byte payload that the codec refuses, so both halves are lost. The
 * second is then reported to the far end as network loss; split, both play
 * and nothing is lost.
 *
 * The marked half leaves a timestamp hole whose queued far side carries the
 * RFC 3551 marker, so the clock steps over it and conceals nothing. The
 * unmarked control, the same hole without the bit, is concealed: an unmarked
 * hole cannot be told from loss.
 */
static void case_jb1_wire(void)
{
	struct udsp_media_stats stats;
	struct tm_wire w;
	struct udsp_media m;
	struct eh t;

	CASE("a coalesced datagram splits and a marked gap skips on the wire");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct udsp_stream *st;
		uint8_t pdu[TM_PKT_MAX];
		uint64_t marked, plain;
		uint16_t seq = 4000;
		uint32_t ts = 294720;
		unsigned want;
		size_t len;
		ssize_t sent;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		w.ssrc = 0xd476114du;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st || !eh_gate_park(&gate))
			goto jb1_out;

		/* Three packets queue 60 ms: probation holds the first and
		 * releases it with the second. */
		if (!tm_jb1_publish(&t, &w, 3, true, &seq, &ts) ||
		    !eh_gate_step(&gate, 1) ||
		    !tm_jb1_run(&gate, &t, &w, 6, false, &seq, &ts))
			goto jb1_out;

		/* Both packets share a datagram, so wait for one slot. */
		len = tm_jb1_pkt(pdu, false, seq, ts, w.ssrc);
		len += tm_jb1_pkt(pdu + len, false, (uint16_t)(seq + 1u),
				  ts + 160u, w.ssrc);
		CHECK_UINT(len, 344u);
		seq += 2;
		ts += 320;
		if (!tm_gate_ring_prepare(st, 1, &want))
			goto jb1_out;
		sent = send(w.rtp_peer, pdu, len, MSG_DONTWAIT | MSG_NOSIGNAL);
		CHECK_UINT(sent, len);
		if (sent != (ssize_t)len || !tm_gate_ring_wait(st, want) ||
		    !eh_gate_step(&gate, 4) ||
		    !tm_jb1_run(&gate, &t, &w, 6, false, &seq, &ts))
			goto jb1_out;

		CHECK(udsp_stream_stats(t.s, 0, 0, &stats) == UDSP_OK);
		CHECK_UINT(stats.packets_lost, 0);

		if (!tm_jb1_hole(&gate, &t, &w, true, &seq, &ts, &marked) ||
		    !tm_jb1_hole(&gate, &t, &w, false, &seq, &ts, &plain))
			goto jb1_out;

		CHECK_UINT(marked, 0);
		CHECK_UINT(plain, 2);

jb1_out:
		eh_gate_disarm(&gate);
		tm_call_down(&t, &w);
		eh_gate_close(&gate);
	}
}

/*
 * A far end on the wire whose third packet
 * steps the timestamp by 2180 ms, marked. Probation releases the held first
 * packet with the second, so the two fill the 30 ms target and the step
 * arrives in PLAY. The buffer must resync once at the successor, with no drop,
 * and the RTP estimator must not read the step as jitter. The line keeps the
 * uvoice buffer defaults of 30 and 250 ms; test_jb covers a step in PREFILL.
 * The gate paces the ticks, not the arrivals, so the packets read a jitter
 * maximum of 17 ms. Read as jitter, the step gives 136 ms, and the ceiling of
 * 100 ms sits between the two.
 */
static void case_jb_step_wire(void)
{
	struct udsp_media_stats stats;
	struct udsp_jb_stats js;
	struct tm_wire w;
	struct udsp_media m;
	struct eh t;

	CASE("a far-end timestamp step at call start resyncs once");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct udsp_stream *st;
		uint16_t seq = 8064;
		uint32_t ts = 160;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.jb_init_ms = 30;
		m.jb_max_ms = 250;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st || !eh_gate_park(&gate))
			goto step_out;

		if (!tm_jb1_publish(&t, &w, 2, true, &seq, &ts) ||
		    !eh_gate_step(&gate, 1))
			goto step_out;
		CHECK_UINT(st->jb.state, 2);
		ts = 17760;
		if (!tm_jb1_publish(&t, &w, 1, true, &seq, &ts) ||
		    !eh_gate_step(&gate, 1) ||
		    !tm_jb1_run(&gate, &t, &w, 250, false, &seq, &ts))
			goto step_out;

		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &js) == UDSP_OK);
		CHECK_UINT(js.resyncs, 1);
		CHECK_UINT(js.starve_resyncs, 0);
		CHECK_UINT(js.overflow_drop, 0);
		CHECK_UINT(js.late_drop, 0);
		CHECK(udsp_stream_stats(t.s, 0, 0, &stats) == UDSP_OK);
		/* Probation discarded nothing: it held the first packet. */
		CHECK_UINT(stats.packets_discarded, 0);
		CHECK(stats.jitter_max < 100u);

step_out:
		eh_gate_disarm(&gate);
		tm_call_down(&t, &w);
		eh_gate_close(&gate);
	}
}

/* --- cases: comfort noise ------------------------------------------------ */

#define TM_CN_BATCH_FRAMES	9u

static bool tm_gate_cn_publish(struct udsp_stream *st, struct tm_wire *w,
			       uint8_t level, unsigned packets)
{
	uint8_t pkt[TM_PKT_MAX];
	unsigned want;

	if (!tm_gate_ring_prepare(st, packets, &want))
		return false;
	for (unsigned i = 0; i < packets; i++) {
		size_t len = tm_rtp_build(pkt, 13, false, w->seq++, w->ts,
					  w->ssrc, &level, 1);
		ssize_t sent;

		w->ts += 80;
		sent = send(w->rtp_peer, pkt, len, MSG_DONTWAIT | MSG_NOSIGNAL);
		CHECK_UINT(sent, len);
		if (sent != (ssize_t)len)
			return false;
	}

	return tm_gate_ring_wait(st, want);
}

static bool tm_gate_cn_wait(struct eh_gate *gate, struct udsp_stream *st)
{
	struct udsp_jb_stats js;
	uint64_t before;

	udsp_jb_stats(&st->jb, &js);
	before = js.cn;
	for (unsigned i = 0; i < 100u; i++) {
		/* Audio can still drain after the CN publication. */
		eh_rec_mark();
		if (!eh_gate_step(gate, 1))
			return false;
		udsp_jb_stats(&st->jb, &js);
		if (js.cn > before)
			return true;
	}
	CHECK(js.cn > before);

	return false;
}

static bool tm_gate_cn_run(struct eh_gate *gate, struct udsp_stream *st,
			   struct tm_wire *w, uint8_t level)
{
	for (unsigned i = 0; i < 4u; i++) {
		if (!tm_gate_cn_publish(st, w, level, 1) ||
		    !tm_gate_cn_wait(gate, st) ||
		    !eh_gate_step(gate, TM_CN_BATCH_FRAMES - 1u))
			return false;
	}

	return true;
}

static void case_cn(void)
{
	struct udsp_media_stats stats;
	struct tm_wire w;
	struct udsp_media m;
	struct udsp_stream *st;
	struct eh t;
	double phase = 0.0;

	CASE("comfort noise from the wire is played and counted");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		uint8_t level = 40;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.cng = true;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st || !eh_gate_park(&gate) ||
		    !tm_gate_tone_send(st, &w, 20, &phase) ||
		    !eh_gate_step(&gate, 12) ||
		    !tm_gate_cn_run(&gate, st, &w, level))
			goto cn_out;
		/* The update is rate limited to one per second per stream. */
		if (!eh_gate_step(&gate, 120u - TM_CN_BATCH_FRAMES))
			goto cn_out;
		CHECK_UINT(eh_rec_have(), 9600);
		if (eh_rec_have() != 9600u)
			goto cn_out;

		CHECK(udsp_stream_stats(t.s, 0, 0, &stats) == UDSP_OK);
		/* Nothing was discarded: RFC 3550 A.1 probation held the first
		 * packet and released it with the second. */
		CHECK_UINT(stats.packets_discarded, 0);
		if (st) {
			struct udsp_jb_stats js;

			udsp_jb_stats(&st->jb, &js);
			CHECK(js.cn > 0);
		}
		/* Comfort noise is audible, so the playback is not silent. */
		CHECK(eh_energy(0, 800) > 100.0);
		eh_gate_disarm(&gate);
		tm_ev_drain(&t, 100);
		{
			const struct udsp_event *e =
				eh_ev_find(UDSP_EV_JB_UPDATE, 0);

			CHECK(e != NULL);
			if (e)
				CHECK(e->u.jb.total > 0);
		}
	cn_out:
		eh_gate_disarm(&gate);
		tm_call_down(&t, &w);
		eh_gate_close(&gate);
	}
}

/* --- rate conversion between the line and the codec ---------------------- */

static double tm_tone_power(const int16_t *pcm, unsigned n, double hz,
			    unsigned rate)
{
	return eh_pcm_tone_power(pcm, n, hz, rate);
}

/*
 * The far end's own G.722 instances. dsp/g722.c is proven against the spandsp
 * vectors in test_g722, so this file uses it as the peer codec exactly as it
 * uses the G.711 code: what is under test here is the rate conversion, not
 * the codec.
 */
#define TM_G722_FRAME	160u
#define TM_G722_BYTES	80u

struct tm_g722 {
	struct udsp_codec *enc;
	struct udsp_codec *dec;
};

static bool tm_g722_open(struct tm_g722 *g)
{
	struct udsp_codec_params p = { 0 };

	g->enc = udsp_codec_g722_ops.create(&p);
	g->dec = udsp_codec_g722_ops.create(&p);

	return g->enc && g->dec;
}

static void tm_g722_close(struct tm_g722 *g)
{
	if (g->enc)
		udsp_codec_g722_ops.destroy(g->enc);
	if (g->dec)
		udsp_codec_g722_ops.destroy(g->dec);
	g->enc = NULL;
	g->dec = NULL;
}

/*
 * Up to three 10 ms G.722 frames of a tone in one packet, at the codec's own
 * 16 kHz rate. RFC 3551 gives G.722 an 8000 Hz RTP clock while it samples at
 * 16000, so 10 ms of audio steps the timestamp by 80.
 */
static void tm_g722_send_n(struct tm_wire *w, struct tm_g722 *g, uint8_t pt,
			   unsigned frames, double hz, double amp,
			   double *phase)
{
	uint8_t pkt[TM_PKT_MAX];
	uint8_t pay[3u * TM_G722_BYTES];
	int16_t pcm[TM_G722_FRAME];
	size_t len;

	if (!frames || frames > 3u)
		return;
	for (unsigned f = 0; f < frames; f++) {
		for (unsigned i = 0; i < TM_G722_FRAME; i++) {
			pcm[i] = (int16_t)(amp * sin(*phase));
			*phase += 2.0 * M_PI * hz / (double)UDSP_SAMPLE_RATE_WB;
		}
		if (udsp_codec_g722_ops.encode(g->enc, pcm, TM_G722_FRAME,
					       pay + f * TM_G722_BYTES,
					       TM_G722_BYTES) !=
		    (int)TM_G722_BYTES)
			return;
	}
	len = tm_rtp_build(pkt, pt, w->seq == 1000, w->seq, w->ts, w->ssrc, pay,
			   frames * TM_G722_BYTES);
	w->seq++;
	w->ts += frames * 80u;
	if (send(w->rtp_peer, pkt, len, MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
		return;
}

static bool tm_gate_g722_publish(struct udsp_stream *st, struct tm_wire *w,
				 struct tm_g722 *g, unsigned packets,
				 const struct udsp_media *m, double hz,
				 double *phase)
{
	unsigned want;

	if (!tm_gate_ring_prepare(st, packets, &want))
		return false;
	for (unsigned i = 0; i < packets; i++)
		tm_g722_send_n(w, g, m->codec_pt, m->ptime_ms / 10u, hz, 8000.0,
				phase);

	return tm_gate_ring_wait(st, want);
}

static bool tm_gate_g722_window(struct eh_gate *gate, struct udsp_stream *st,
				struct tm_wire *w, struct tm_g722 *g,
				const struct udsp_media *m, double hz,
				double *phase)
{
	unsigned packets = m->ptime_ms == 30u ? 4u : 10u;
	unsigned frames = packets * m->ptime_ms / 10u - 1u;

	for (unsigned b = 0; b < 8u; b++) {
		if (!tm_gate_g722_publish(st, w, g, packets, m, hz, phase))
			return false;
		if (b == 3u)
			eh_rec_mark();
		if (!eh_gate_step(gate, frames))
			return false;
	}

	return true;
}

/* One G.722 payload into the far end's 16 kHz frame. The decoder is stateful,
 * so a payload is decoded once and every measurement reads that result. */
static bool tm_g722_decode(struct tm_g722 *g, const uint8_t *pay, size_t len,
			   int16_t *pcm)
{
	return len == TM_G722_BYTES &&
	       udsp_codec_g722_ops.decode(g->dec, pay, len, pcm,
					  TM_G722_FRAME) == (int)TM_G722_FRAME;
}

/* The energy a record carries in a band. Noise has no line to measure, so the
 * band is probed with a comb and the powers are summed. */
static double tm_band_power(unsigned from, unsigned len, double lo, double hi,
			    unsigned rate)
{
	double sum = 0.0;

	for (double hz = lo; hz <= hi; hz += 250.0)
		sum += eh_tone_power(from, len, hz, rate);

	return sum;
}

static double tm_ulaw_payload_power(const uint8_t *pay, size_t len, double hz)
{
	int16_t pcm[240];

	if (!len || len > 240u)
		return 0.0;
	for (size_t i = 0; i < len; i++)
		pcm[i] = udsp_ulaw_decode(pay[i]);

	return tm_tone_power(pcm, (unsigned)len, hz, UDSP_SAMPLE_RATE);
}

static bool tm_gate_config_run(struct eh_gate *gate, struct eh *t,
			       const struct udsp_media *media,
			       const struct tm_wire *wire);

static void case_resample(void)
{
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	struct tm_g722 g;
	struct eh t;
	double phase = 0.0;

	/*
	 * The narrowband anchor. A matched rate is the byte-identical path, so
	 * the shape the whole 8 kHz suite runs on is stated once: no
	 * resampler, the codec frame is the line frame, and the timestamp step
	 * is the frame count. The wire oracles for that path are
	 * case_audio, case_ptime and case_vad.
	 */
	CASE("a narrowband codec on an 8 kHz line instantiates no resampler");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			CHECK(!st->resample);
			CHECK_UINT(st->codec_rate, UDSP_SAMPLE_RATE);
			CHECK_UINT(st->codec_frame, UDSP_FRAME_SAMPLES);
			CHECK_UINT(st->line_frame, UDSP_FRAME_SAMPLES);
			CHECK_UINT(st->ts_per_frame, UDSP_FRAME_SAMPLES);
		}
		tm_media(&m, UDSP_SEL_G726_32, 96, 20);
		tm_stream_config(&t, &w, &m);
		if (st) {
			CHECK(!st->resample);
			CHECK_UINT(st->ts_per_frame, UDSP_FRAME_SAMPLES);
		}
		tm_call_close(&t, &w);
	}

	/*
	 * A narrowband codec on a wideband line: down2 before the encoder, up2
	 * after the decoder. The rate-error image is the discriminator both
	 * ways. A 1000 Hz tone the resampler carried reads as 1000 Hz at the
	 * rate on the far side of the conversion; the same samples handed over
	 * without the resampler read as 2000 Hz, because half the frames went
	 * missing.
	 */
	CASE("a narrowband codec on a 16 kHz line is resampled in both directions");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p = { 0 };
		struct tm_pkt q = { 0 };
		bool got;
		ssize_t n;

		if (!tm_up_rate(&t, "USA", UDSP_SAMPLE_RATE_WB) ||
		    !tm_wire_open(&w, 0)) {
			CHECK(false);
			return;
		}
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			CHECK(st->resample);
			CHECK_UINT(st->codec_rate, UDSP_SAMPLE_RATE);
			CHECK_UINT(st->codec_frame, UDSP_FRAME_SAMPLES);
			CHECK_UINT(st->line_frame, UDSP_FRAME_MAX);
			CHECK_UINT(st->ts_per_frame, UDSP_FRAME_SAMPLES);
		}
		tm_capture_tone(1000u);
		/* Two ticks exclude a stale capture and an in-flight fill. */
		eh_wait_ms(&t, 20);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 150);

		/* Transmit. The packet is one narrowband frame per tick under
		 * the narrowband timestamp step, whatever the line runs at. */
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 600);
		CHECK(n == 12 + 80);
		got = n > 0 && tm_rtp_parse(buf, (size_t)n, &p);
		CHECK(got);
		if (got) {
			double want = tm_ulaw_payload_power(p.payload, p.len,
							    1000.0);
			double image = tm_ulaw_payload_power(p.payload, p.len,
							     2000.0);

			CHECK(want > 1e6);
			CHECK(image * 100.0 < want);
		}
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 600);
		got = got && n > 0 && tm_rtp_parse(buf, (size_t)n, &q);
		CHECK(got);
		if (got)
			CHECK_UINT(q.ts - p.ts, 80);

		/* Receive. The far end plays a narrowband 1000 Hz tone and the
		 * 16 kHz playback has to carry it at 1000 Hz. */
		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		if (!st || !eh_gate_park(&gate) ||
		    !tm_gate_tone_window(&gate, st, &w, 3, 5, &phase))
			goto pcmu_wb_out;
		CHECK_UINT(eh_rec_have(), 7200);
		if (eh_rec_have() != 7200u)
			goto pcmu_wb_out;
		{
			double want = eh_tone_power(0, 1600, 1000.0,
						    UDSP_SAMPLE_RATE_WB);
			double image = eh_tone_power(0, 1600, 2000.0,
						     UDSP_SAMPLE_RATE_WB);

			CHECK(want > 1e6);
			CHECK(image * 100.0 < want);
		}
	pcmu_wb_out:
		eh_gate_disarm(&gate);
		tm_call_down(&t, &w);
		eh_gate_close(&gate);
	}

	/*
	 * G.722 on a wideband line is native: no resampler at all, and the
	 * 5 kHz tone is the proof that the whole chain is wideband, because
	 * nothing narrowband can carry it.
	 */
	CASE("G.722 on a 16 kHz line runs native and carries a 5 kHz tone");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		uint8_t buf[TM_PKT_MAX];
		int16_t pcm[TM_G722_FRAME] = { 0 };
		struct tm_pkt p = { 0 };
		struct tm_pkt q = { 0 };
		ssize_t n;

		if (!tm_up_rate(&t, "USA", UDSP_SAMPLE_RATE_WB) ||
		    !tm_wire_open(&w, 0) || !tm_g722_open(&g)) {
			CHECK(false);
			return;
		}
		tm_media(&m, UDSP_SEL_G722, 9, 10);
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			CHECK(!st->resample);
			CHECK_UINT(st->codec_rate, UDSP_SAMPLE_RATE_WB);
			CHECK_UINT(st->codec_frame, UDSP_FRAME_MAX);
			CHECK_UINT(st->line_frame, UDSP_FRAME_MAX);
			/* RFC 3551: the audio rate is 16000, the clock 8000. */
			CHECK_UINT(st->ts_per_frame, UDSP_FRAME_SAMPLES);
		}
		tm_capture_tone(5000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 150);

		/* Both codecs start cold, so the packet under test is one the
		 * far end's ADPCM has already settled on. */
		for (unsigned i = 0; i < 6u; i++) {
			n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 600);
			CHECK(n == 12 + 80);
			if (n <= 0 || !tm_rtp_parse(buf, (size_t)n, &p))
				break;
			CHECK_UINT(p.pt, 9);
			CHECK(tm_g722_decode(&g, p.payload, p.len, pcm));
		}
		CHECK(tm_tone_power(pcm, TM_G722_FRAME, 5000.0,
				    UDSP_SAMPLE_RATE_WB) > 1e6);
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 600);
		if (n > 0 && tm_rtp_parse(buf, (size_t)n, &q))
			CHECK_UINT(q.ts - p.ts, 80);
		else
			CHECK(false);

		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		if (!st || !eh_gate_park(&gate) ||
		    !tm_gate_g722_window(&gate, st, &w, &g, &m, 5000.0, &phase))
			goto g722_wb_out;
		CHECK_UINT(eh_rec_have(), 7200);
		if (eh_rec_have() != 7200u)
			goto g722_wb_out;
		{
			double want = eh_tone_power(0, 1600, 5000.0,
						    UDSP_SAMPLE_RATE_WB);
			double other = eh_tone_power(0, 1600, 3000.0,
						     UDSP_SAMPLE_RATE_WB);

			CHECK(want > 1e6);
			CHECK(other * 100.0 < want);
		}
	g722_wb_out:
		eh_gate_disarm(&gate);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_g722_close(&g);
		tm_call_close(&t, &w);
		eh_gate_close(&gate);
	}

	/*
	 * The other direction of mismatch: G.722 on a narrowband line, up2
	 * before the encoder and down2 after the decoder. The image test is
	 * the same one, read at the rate on the far side of the conversion.
	 */
	CASE("G.722 on an 8 kHz line is resampled in both directions");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		uint8_t buf[TM_PKT_MAX];
		int16_t pcm[TM_G722_FRAME] = { 0 };
		struct tm_pkt p = { 0 };
		struct tm_pkt q = { 0 };
		ssize_t n;

		if (!tm_up(&t, "USA") || !tm_wire_open(&w, 0) ||
		    !tm_g722_open(&g)) {
			CHECK(false);
			return;
		}
		tm_media(&m, UDSP_SEL_G722, 9, 10);
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			CHECK(st->resample);
			CHECK_UINT(st->codec_rate, UDSP_SAMPLE_RATE_WB);
			CHECK_UINT(st->codec_frame, UDSP_FRAME_MAX);
			CHECK_UINT(st->line_frame, UDSP_FRAME_SAMPLES);
			CHECK_UINT(st->ts_per_frame, UDSP_FRAME_SAMPLES);
		}
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 150);

		for (unsigned i = 0; i < 6u; i++) {
			n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 600);
			CHECK(n == 12 + 80);
			if (n <= 0 || !tm_rtp_parse(buf, (size_t)n, &p))
				break;
			CHECK_UINT(p.pt, 9);
			CHECK(tm_g722_decode(&g, p.payload, p.len, pcm));
		}
		{
			double want = tm_tone_power(pcm, TM_G722_FRAME, 1000.0,
						    UDSP_SAMPLE_RATE_WB);
			double image = tm_tone_power(pcm, TM_G722_FRAME, 2000.0,
						     UDSP_SAMPLE_RATE_WB);

			CHECK(want > 1e6);
			CHECK(image * 100.0 < want);
		}
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 600);
		if (n > 0 && tm_rtp_parse(buf, (size_t)n, &q))
			CHECK_UINT(q.ts - p.ts, 80);
		else
			CHECK(false);

		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		if (!st || !eh_gate_park(&gate) ||
		    !tm_gate_g722_window(&gate, st, &w, &g, &m, 1000.0, &phase))
			goto g722_nb_out;
		CHECK_UINT(eh_rec_have(), 3600);
		if (eh_rec_have() != 3600u)
			goto g722_nb_out;
		{
			double want = eh_tone_power(0, 800, 1000.0,
						    UDSP_SAMPLE_RATE);
			double image = eh_tone_power(0, 800, 2000.0,
						     UDSP_SAMPLE_RATE);

			CHECK(want > 1e6);
			CHECK(image * 100.0 < want);
		}
	g722_nb_out:
		eh_gate_disarm(&gate);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_g722_close(&g);
		tm_call_close(&t, &w);
		eh_gate_close(&gate);
	}

	/*
	 * The aggregate at the widest ptime on the wideband side: three 10 ms
	 * G.722 frames stage into one packet, and the timestamp step is three
	 * times the RTP clock's 10 ms, not the audio rate's.
	 */
	CASE("G.722 at ptime 30 stages three frames under one clock step");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		uint8_t buf[TM_PKT_MAX];
		int16_t pcm[TM_G722_FRAME] = { 0 };
		struct tm_pkt p = { 0 };
		struct tm_pkt q = { 0 };
		ssize_t n;

		if (!tm_up_rate(&t, "USA", UDSP_SAMPLE_RATE_WB) ||
		    !tm_wire_open(&w, 0) || !tm_g722_open(&g)) {
			CHECK(false);
			return;
		}
		tm_media(&m, UDSP_SEL_G722, 9, 30);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(3000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);

		for (unsigned i = 0; i < 4u; i++) {
			n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 600);
			CHECK(n == 12 + 240);
			if (n <= 0 || !tm_rtp_parse(buf, (size_t)n, &p))
				break;
			for (unsigned f = 0; f < 3u; f++)
				CHECK(tm_g722_decode(&g, p.payload + f * 80u,
						     80u, pcm));
		}
		CHECK(tm_tone_power(pcm, TM_G722_FRAME, 3000.0,
				    UDSP_SAMPLE_RATE_WB) > 1e6);
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 600);
		if (n > 0 && tm_rtp_parse(buf, (size_t)n, &q))
			CHECK_UINT(q.ts - p.ts, 240);
		else
			CHECK(false);

		/* The receive half: the buffer byte-slices the 240 byte packet
		 * into three 80 byte units and each decodes to a 10 ms frame. */
		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st || !eh_gate_park(&gate) ||
		    !tm_gate_g722_window(&gate, st, &w, &g, &m, 3000.0, &phase))
			goto g722_ptime_out;
		CHECK_UINT(eh_rec_have(), 8800);
		if (eh_rec_have() != 8800u)
			goto g722_ptime_out;
		{
			double want = eh_tone_power(0, 1600, 3000.0,
						    UDSP_SAMPLE_RATE_WB);
			double other = eh_tone_power(0, 1600, 6000.0,
						     UDSP_SAMPLE_RATE_WB);

			CHECK(want > 1e6);
			CHECK(other * 100.0 < want);
		}
	g722_ptime_out:
		eh_gate_disarm(&gate);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_g722_close(&g);
		tm_call_close(&t, &w);
		eh_gate_close(&gate);
	}

	/*
	 * A re-INVITE that changes the codec moves the rate conversion with
	 * it: the reconfiguration settles the rates again, so no tick decodes
	 * into a frame count the old codec left behind, and the resampler
	 * histories go back to zero. The path leaves the conversion and
	 * returns to it, because a reset that never happened only shows on
	 * re-entry: the far end loads the receive filter with a loud 3400 Hz
	 * tone, the G.722 leg leaves that filter idle, and the first frames
	 * after the return have to be silence and not the ring-out of a
	 * history from two descriptions ago. The stream keeps its source
	 * throughout.
	 */
	CASE("a codec change on a 16 kHz line settles the rate conversion again");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		uint8_t buf[TM_PKT_MAX];
		int16_t pcm[TM_G722_FRAME] = { 0 };
		struct tm_pkt p = { 0 };
		unsigned wide_pkts = 0;
		uint32_t ssrc = 0;
		bool have = false;
		ssize_t n;

		if (!tm_up_rate(&t, "USA", UDSP_SAMPLE_RATE_WB) ||
		    !tm_wire_open(&w, 0) || !tm_g722_open(&g)) {
			CHECK(false);
			return;
		}
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(3000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st)
			CHECK(st->resample);
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 600);
		CHECK(n == 12 + 80);
		have = n > 0 && tm_rtp_parse(buf, (size_t)n, &p);
		CHECK(have);
		if (have)
			ssrc = p.ssrc;
		/*
		 * The far end loads the receive filter, loud and near the top
		 * of the band the codec can carry, and the description changes
		 * while that audio is still playing: a reset that never
		 * happened is only visible if the history it kept is loud, and
		 * a drained buffer would have filtered it down to nothing
		 * before the change arrived.
		 */
		if (!st || !eh_gate_park(&gate) ||
		    !tm_gate_tone_run_at(&gate, st, &w, 4, 3400.0, 20000.0,
					 &phase) ||
		    !tm_gate_tone_publish(st, &w, 10, &m, 3400.0, 20000.0,
					  &phase))
			goto codec_change_out;
		eh_rec_mark();
		if (!eh_gate_step(&gate, 10))
			goto codec_change_out;
		CHECK_UINT(eh_rec_have(), 1600);
		if (eh_rec_have() != 1600u)
			goto codec_change_out;
		CHECK(eh_energy(0, 1600) > 1e6);

		tm_media(&m, UDSP_SEL_G722, 9, 10);
		if (!tm_gate_config_run(&gate, &t, &m, &w))
			goto codec_change_out;
		if (st) {
			CHECK(!st->resample);
			CHECK_UINT(st->codec_frame, UDSP_FRAME_MAX);
			CHECK_UINT(st->ts_per_frame, UDSP_FRAME_SAMPLES);
		}
		if (!eh_gate_step(&gate, 30))
			goto codec_change_out;
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (!tm_rtp_parse(buf, (size_t)n, &p) || p.pt != 9u)
				continue;
			CHECK_UINT(p.len, 80);
			CHECK(tm_g722_decode(&g, p.payload, p.len, pcm));
			/* The description changed, the source did not. */
			CHECK_UINT(p.ssrc, ssrc);
			wide_pkts++;
		}
		CHECK(wide_pkts > 5u);
		CHECK(tm_tone_power(pcm, TM_G722_FRAME, 3000.0,
				    UDSP_SAMPLE_RATE_WB) > 1e6);

		/*
		 * Return to it, with the far end silent. The mark goes before
		 * the change: the description is SYNC-TICK, so the first tick
		 * that runs the restored conversion has already happened by the
		 * time the call comes back, and that tick is the one a kept
		 * history rings out on.
		 */
		eh_rec_mark();
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		if (!tm_gate_config_run(&gate, &t, &m, &w))
			goto codec_change_out;
		if (st) {
			CHECK(st->resample);
			CHECK_UINT(st->codec_frame, UDSP_FRAME_SAMPLES);
			CHECK_UINT(st->line_frame, UDSP_FRAME_MAX);
		}
		/* The window spans the whole change, because the description
		 * costs a few ticks and the ring-out falls on the first one
		 * that runs the restored conversion. */
		if (!eh_gate_step(&gate, 12))
			goto codec_change_out;
		CHECK(eh_rec_have() >= 1920u);
		if (eh_rec_have() < 1920u)
			goto codec_change_out;
		CHECK(eh_energy(0, 1600) < 1.0);
		CHECK(eh_tone_power(0, 1600, 3400.0,
				    UDSP_SAMPLE_RATE_WB) < 1e3);

		if (!tm_gate_tone_window(&gate, st, &w, 2, 4, &phase))
			goto codec_change_out;
		CHECK_UINT(eh_rec_have(), 5760);
		if (eh_rec_have() != 5760u)
			goto codec_change_out;
		{
			double want = eh_tone_power(0, 1600, 1000.0,
						    UDSP_SAMPLE_RATE_WB);
			double image = eh_tone_power(0, 1600, 2000.0,
						     UDSP_SAMPLE_RATE_WB);

			CHECK(want > 1e6);
			CHECK(image * 100.0 < want);
		}
	codec_change_out:
		eh_gate_disarm(&gate);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_g722_close(&g);
		tm_call_close(&t, &w);
		eh_gate_close(&gate);
	}

	/*
	 * The silence decision is resampled like every other one. A buffer
	 * that ran dry emits silence units, and a unit that reached the line
	 * without the resampler would fill only half the wideband frame and
	 * leave the other half holding the audio that came before: a silence
	 * that buzzes at the frame rate. The stretch is measured while it
	 * runs, and the audio that follows it has to come back clean.
	 */
	CASE("a silence stretch on a 16 kHz line is resampled");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };

		if (!tm_up_rate(&t, "USA", UDSP_SAMPLE_RATE_WB) ||
		    !tm_wire_open(&w, 0)) {
			CHECK(false);
			return;
		}
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st || !eh_gate_park(&gate) ||
		    !tm_gate_tone_run_at(&gate, st, &w, 6, 1000.0, 20000.0,
					 &phase))
			goto silence_wb_out;
		/*
		 * A timestamp jump past the jb.h bound resyncs the buffer to
		 * prefill, so the units that follow are silence taken straight off a
		 * loud frame: no concealment in between to fade the frame out,
		 * and no reconfiguration to clear it. That is the only shape
		 * that can catch a silence unit which filled half the wideband
		 * frame and left the other half holding the audio.
		 */
		w.ts += 400000u;
		if (!tm_gate_tone_publish(st, &w, 1, &m, 1000.0, 20000.0,
					  &phase))
			goto silence_wb_out;
		eh_rec_mark();
		if (!eh_gate_step(&gate, 10))
			goto silence_wb_out;
		CHECK_UINT(eh_rec_have(), 1600);
		if (eh_rec_have() != 1600u)
			goto silence_wb_out;
		CHECK(eh_energy(320, 1280) < 1.0);

		/* The drain must cross concealment before it proves silence. */
		if (!eh_gate_step(&gate, 30))
			goto silence_wb_out;
		eh_rec_mark();
		if (!eh_gate_step(&gate, 4))
			goto silence_wb_out;
		CHECK_UINT(eh_rec_have(), 640);
		if (eh_rec_have() != 640u)
			goto silence_wb_out;
		CHECK(eh_energy(0, 320) < 1.0);

		if (!tm_gate_tone_window(&gate, st, &w, 2, 4, &phase))
			goto silence_wb_out;
		CHECK_UINT(eh_rec_have(), 5760);
		if (eh_rec_have() != 5760u)
			goto silence_wb_out;
		if (st) {
			struct udsp_jb_stats js;

			udsp_jb_stats(&st->jb, &js);
			CHECK(js.silence > 0);
		}
		{
			double want = eh_tone_power(0, 1600, 1000.0,
						    UDSP_SAMPLE_RATE_WB);
			double image = eh_tone_power(0, 1600, 2000.0,
						     UDSP_SAMPLE_RATE_WB);

			CHECK(want > 1e6);
			CHECK(image * 100.0 < want);
		}
	silence_wb_out:
		eh_gate_disarm(&gate);
		tm_call_down(&t, &w);
		eh_gate_close(&gate);
	}

	/*
	 * The fax profile is a codec change that costs one tick boundary, so
	 * it settles the rate conversion exactly as a reconfiguration does. A
	 * wideband line running G.722 natively goes to a narrowband
	 * passthrough codec, which puts the resampler in, and comes back out.
	 */
	CASE("a fax passthrough on a 16 kHz line settles the rate conversion");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		uint8_t buf[TM_PKT_MAX];
		int16_t pcm[TM_G722_FRAME] = { 0 };
		unsigned narrow_pkts = 0;
		unsigned wide_pkts = 0;
		struct tm_pkt p = { 0 };
		ssize_t n;

		if (!tm_up_rate(&t, "USA", UDSP_SAMPLE_RATE_WB) ||
		    !tm_wire_open(&w, 0) || !tm_g722_open(&g)) {
			CHECK(false);
			return;
		}
		tm_media(&m, UDSP_SEL_G722, 9, 10);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		/* Two ticks exclude a stale capture and an in-flight fill. */
		eh_wait_ms(&t, 20);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st)
			CHECK(!st->resample);

		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_OK);
		if (st) {
			CHECK(st->resample);
			CHECK_UINT(st->codec_frame, UDSP_FRAME_SAMPLES);
			CHECK_UINT(st->line_frame, UDSP_FRAME_MAX);
			CHECK_UINT(st->ts_per_frame, UDSP_FRAME_SAMPLES);
			/* The profile also fixes the buffer and drops the
			 * detector, which is what a modem carrier needs. */
			CHECK_UINT(st->media.jb_mode, UDSP_JB_FIXED);
			CHECK(!st->media.vad);
		}
		eh_wait_ms(&t, 200);
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			double want;
			double image;

			if (!tm_rtp_parse(buf, (size_t)n, &p) || p.pt != 0u)
				continue;
			CHECK_UINT(p.len, 80);
			want = tm_ulaw_payload_power(p.payload, p.len, 1000.0);
			image = tm_ulaw_payload_power(p.payload, p.len, 2000.0);
			CHECK(want > 1e6);
			CHECK(image * 100.0 < want);
			narrow_pkts++;
		}
		CHECK(narrow_pkts > 5u);

		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		if (!st || !eh_gate_park(&gate) ||
		    !tm_gate_tone_window(&gate, st, &w, 2, 4, &phase))
			goto fax_resample_out;
		CHECK_UINT(eh_rec_have(), 5760);
		if (eh_rec_have() != 5760u)
			goto fax_resample_out;
		{
			double want = eh_tone_power(0, 1600, 1000.0,
						    UDSP_SAMPLE_RATE_WB);
			double image = eh_tone_power(0, 1600, 2000.0,
						     UDSP_SAMPLE_RATE_WB);

			CHECK(want > 1e6);
			CHECK(image * 100.0 < want);
		}

		eh_gate_disarm(&gate);
		/* Out of the profile: the description that was displaced comes
		 * back and the rate conversion goes with it. */
		tm_capture_tone(5000u);
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_OFF, 0, 0) ==
		      UDSP_OK);
		if (st) {
			CHECK(!st->resample);
			CHECK_UINT(st->codec_frame, UDSP_FRAME_MAX);
			CHECK_UINT(st->ts_per_frame, UDSP_FRAME_SAMPLES);
		}
		eh_wait_ms(&t, 200);
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (!tm_rtp_parse(buf, (size_t)n, &p) || p.pt != 9u)
				continue;
			CHECK_UINT(p.len, 80);
			CHECK(tm_g722_decode(&g, p.payload, p.len, pcm));
			wide_pkts++;
		}
		CHECK(wide_pkts > 5u);
		/* Native again: the 5 kHz tone only survives a wideband path. */
		CHECK(tm_tone_power(pcm, TM_G722_FRAME, 5000.0,
				    UDSP_SAMPLE_RATE_WB) > 1e6);
	fax_resample_out:
		eh_gate_disarm(&gate);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_g722_close(&g);
		tm_call_close(&t, &w);
		eh_gate_close(&gate);
	}

	/*
	 * Comfort noise pairs with G.722: the description survives
	 * media_prepare, the packet carries payload type 13 with one level
	 * octet on the stream's 8000 Hz RTP clock, and the receive side
	 * synthesizes the noise at the codec's sample rate, so a wideband line
	 * hears wideband noise. The band split is the oracle: noise made at
	 * 16000 fills the upper band, and noise that reached the line through
	 * a narrowband codec cannot.
	 */
	CASE("comfort noise pairs with G.722 and fills the wideband band");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		uint8_t buf[TM_PKT_MAX];
		uint8_t level = 40;
		struct tm_pkt p = { 0 };
		unsigned cn_pkts = 0;
		ssize_t n;

		if (!tm_up_rate(&t, "USA", UDSP_SAMPLE_RATE_WB) ||
		    !tm_wire_open(&w, 0) || !tm_g722_open(&g)) {
			CHECK(false);
			return;
		}
		tm_media(&m, UDSP_SEL_G722, 9, 10);
		m.vad = true;
		m.cng = true;
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			CHECK(st->media.vad);
			CHECK(st->media.cng);
		}
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);

		/* RFC 3389: one description opens the silence, on PT 13. */
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (!tm_rtp_parse(buf, (size_t)n, &p) || p.pt != 13u)
				continue;
			CHECK_UINT(p.len, 1);
			CHECK(p.payload[0] > 0u);
			cn_pkts++;
		}
		CHECK_UINT(cn_pkts, 1);

		/* The buffer anchors on audio before it can play a comfort
		 * noise period, and jb.h refuses a comfort-noise packet that
		 * sits far ahead of the playout clock, so the queue is let
		 * drain before the description arrives. */
		if (!st || !eh_gate_park(&gate) ||
		    !tm_gate_g722_publish(st, &w, &g, 20, &m, 1000.0,
					  &phase) ||
		    !eh_gate_step(&gate, 30))
			goto cn_g722_out;
		if (!tm_gate_cn_run(&gate, st, &w, level))
			goto cn_g722_out;
		CHECK_UINT(eh_rec_have(), 1440);
		if (eh_rec_have() != 1440u)
			goto cn_g722_out;
		if (st) {
			struct udsp_jb_stats js;

			udsp_jb_stats(&st->jb, &js);
			CHECK(js.cn > 0);
		}
		{
			double low = tm_band_power(0, 640, 500.0, 3500.0,
						   UDSP_SAMPLE_RATE_WB);
			double high = tm_band_power(0, 640, 4500.0, 7500.0,
						    UDSP_SAMPLE_RATE_WB);

			CHECK(eh_energy(0, 640) > 100.0);
			/* White noise made at 16000 spreads over both halves of
			 * the band; measured ratio about 1.1 to 1. */
			CHECK(high * 4.0 > low);
		}
	cn_g722_out:
		eh_gate_disarm(&gate);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_g722_close(&g);
		tm_call_close(&t, &w);
		eh_gate_close(&gate);
	}

	/*
	 * The same comfort noise through the resampler. Every receive decision
	 * goes across rs_rx, comfort noise included, so a narrowband codec's
	 * noise reaches a 16 kHz line through the same half-band filter its
	 * audio does and carries nothing above 4 kHz. Noise written straight
	 * into the wideband frame would.
	 */
	CASE("comfort noise on a 16 kHz line is resampled and band limited");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		uint8_t level = 40;

		if (!tm_up_rate(&t, "USA", UDSP_SAMPLE_RATE_WB) ||
		    !tm_wire_open(&w, 0)) {
			CHECK(false);
			return;
		}
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.cng = true;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st || !eh_gate_park(&gate) ||
		    !tm_gate_tone_send(st, &w, 20, &phase) ||
		    !eh_gate_step(&gate, 20))
			goto cn_resample_out;
		if (!tm_gate_cn_run(&gate, st, &w, level))
			goto cn_resample_out;
		CHECK_UINT(eh_rec_have(), 1440);
		if (eh_rec_have() != 1440u)
			goto cn_resample_out;
		if (st) {
			struct udsp_jb_stats js;

			udsp_jb_stats(&st->jb, &js);
			CHECK(js.cn > 0);
		}
		{
			double low = tm_band_power(0, 640, 500.0, 3500.0,
						   UDSP_SAMPLE_RATE_WB);
			double high = tm_band_power(0, 640, 4500.0, 7500.0,
						    UDSP_SAMPLE_RATE_WB);

			int got = udsp_cn_level_get(eh_be.rec +
						    eh_be.rec_base, 640);

			CHECK(eh_energy(0, 640) > 100.0);
			/* The half-band filter of dsp/resamp.h holds 58 dB of
			 * stopband. Measured 1790 to 1 over this window. */
			CHECK(high * 50.0 < low);
			/* And the level the far end asked for is the level the
			 * line plays: a frame the resampler filled by halves
			 * reads about 6 dB hot, because the half it left behind
			 * is the audio that came before the silence. */
			CHECK(got >= (int)level - 2 && got <= (int)level + 2);
		}
	cn_resample_out:
		eh_gate_disarm(&gate);
		tm_call_down(&t, &w);
		eh_gate_close(&gate);
	}

	/*
	 * Concealment takes the same rate conversion. The G.711 concealer
	 * repeats the pitch of what it last heard, at the codec's rate, so a
	 * unit that reached the line without the resampler would play back at
	 * twice the frequency and fill only half the frame. The buffer is let
	 * run dry with the tone still established, which is the only way to
	 * put concealment inside a measured window.
	 */
	CASE("concealment on a 16 kHz line is resampled");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct udsp_jb_stats js;
		uint64_t plc_before;

		if (!tm_up_rate(&t, "USA", UDSP_SAMPLE_RATE_WB) ||
		    !tm_wire_open(&w, 0)) {
			CHECK(false);
			return;
		}
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st || !eh_gate_park(&gate))
			goto plc_out;
		if (!tm_gate_tone_run(&gate, st, &w, 6, &phase) ||
		    !tm_gate_plc_wait(&gate, st, &plc_before) ||
		    !eh_gate_step(&gate, 5))
			goto plc_out;
		udsp_jb_stats(&st->jb, &js);
		/* Six frames stay below JB_PLC_MAX_MS (400 ms). */
		CHECK_UINT(js.plc - plc_before, 6);
		CHECK_UINT(eh_rec_have(), 960);
		{
			double want = eh_tone_power(0, 960, 1000.0,
						    UDSP_SAMPLE_RATE_WB);
			double image = eh_tone_power(0, 960, 2000.0,
						     UDSP_SAMPLE_RATE_WB);

			CHECK(eh_energy(0, 960) > 100.0);
			CHECK(want > 1e6);
			CHECK(image * 100.0 < want);
		}
plc_out:
		eh_gate_disarm(&gate);
		tm_call_down(&t, &w);
		eh_gate_close(&gate);
	}

	/*
	 * The voice detector and the RFC 3389 level measure the frame the
	 * codec gets, not the frame the line captured (stream.h rs_vad). A 6 kHz
	 * near-end tone is the proof: a narrowband codec cannot carry it, the
	 * decimator takes it out, and what leaves is one comfort-noise
	 * description. A detector reading the line frame would call that tone
	 * speech and send packets of silence instead.
	 */
	CASE("the voice detector measures the frame the codec gets");
	{
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p = { 0 };
		unsigned media_pkts = 0;
		unsigned cn_pkts = 0;
		ssize_t n;

		if (!tm_up_rate(&t, "USA", UDSP_SAMPLE_RATE_WB) ||
		    !tm_wire_open(&w, 0)) {
			CHECK(false);
			return;
		}
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.vad = true;
		m.cng = true;
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(6000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);

		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (!tm_rtp_parse(buf, (size_t)n, &p))
				continue;
			if (p.pt == 13u) {
				cn_pkts++;
				continue;
			}
			media_pkts++;
		}
		CHECK_UINT(cn_pkts, 1);
		CHECK_UINT(media_pkts, 0);

		/* A 1 kHz tone the codec can carry reopens the talkspurt, so
		 * the silence is the band and not a stuck detector. */
		atomic_store(&tm_be.tone_hz, 1000u);
		eh_wait_ms(&t, 200);
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (!tm_rtp_parse(buf, (size_t)n, &p) || p.pt != 0u)
				continue;
			media_pkts++;
		}
		CHECK(media_pkts > 5u);
		tm_call_down(&t, &w);
	}

	/*
	 * The rule is scoped by codec.h. codec_pair_create short-circuits on
	 * the same vtable, so a description that only moves a codec parameter
	 * would otherwise reach a pair created against the old one. The
	 * parameters a fixed-frame codec is created with are the line rate and
	 * nothing else, because it reads nothing else: a refresh that moves a
	 * packet duration or an Opus field must leave its concealment history
	 * alone. The recreate arm therefore belongs to the whole-packet codecs,
	 * and the loop below runs it for every one the registry carries, which
	 * is none without libopus.
	 */
	CASE("a codec parameter reaches the pair only where the codec reads it");
	{
		const struct udsp_codec_ops *const *walk;
		unsigned packet_codecs = 0;
		uintptr_t enc;
		uintptr_t dec;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			tm_call_close(&t, &w);
			return;
		}
		enc = (uintptr_t)st->enc;
		dec = (uintptr_t)st->dec;
		CHECK(enc && dec);
		/* The line rate is unconditional; nothing else reaches a
		 * fixed-frame codec's create. */
		CHECK_UINT(st->params.line_rate, UDSP_SAMPLE_RATE);
		CHECK_UINT(st->params.ptime_ms, 0);
		CHECK_UINT(st->params.bitrate, 0);
		CHECK_UINT(st->params.complexity, 0);
		CHECK_UINT(st->params.max_bandwidth, 0);
		CHECK(!st->params.fec);

		/* The same description keeps the instances it runs. */
		tm_stream_config(&t, &w, &m);
		CHECK((uintptr_t)st->enc == enc);
		CHECK((uintptr_t)st->dec == dec);

		/* And so does a packet duration, which a fixed-frame codec
		 * never reads. */
		tm_media(&m, UDSP_SEL_PCMU, 0, 30);
		tm_stream_config(&t, &w, &m);
		CHECK((uintptr_t)st->enc == enc);
		CHECK((uintptr_t)st->dec == dec);
		CHECK_UINT(st->params.ptime_ms, 0);

		/* And so does an Opus field, which is the SDP refresh that
		 * would otherwise cost a live G.711 pair its history. */
		tm_media(&m, UDSP_SEL_PCMU, 0, 30);
		m.opus.bitrate = 24000;
		m.opus.complexity = 8;
		m.opus.max_bandwidth = 3;
		m.opus.fec = true;
		tm_stream_config(&t, &w, &m);
		CHECK((uintptr_t)st->enc == enc);
		CHECK((uintptr_t)st->dec == dec);
		CHECK_UINT(st->params.bitrate, 0);
		CHECK_UINT(st->params.complexity, 0);
		CHECK_UINT(st->params.max_bandwidth, 0);
		CHECK(!st->params.fec);

		/*
		 * The other half of the rule, for every whole-packet codec the
		 * build carries: none without libopus, and Opus alone with it.
		 * The fixed-frame arm above binds in either build.
		 */
		for (walk = udsp_codec_builtin.ops; walk && *walk; walk++) {
			uintptr_t was_enc;
			uintptr_t was_dec;

			if (!((*walk)->flags & UDSP_CODEC_F_PACKET))
				continue;
			packet_codecs++;
			tm_media(&m, (uint8_t)(*walk)->id, 96, 20);
			tm_stream_config(&t, &w, &m);
			was_enc = (uintptr_t)st->enc;
			was_dec = (uintptr_t)st->dec;
			CHECK_UINT(st->params.ptime_ms, 20);
			m.opus.bitrate = 24000;
			tm_stream_config(&t, &w, &m);
			CHECK_UINT(st->params.bitrate, 24000);
			/* The instances are built aside before the old pair is
			 * freed, so the new addresses cannot be the old ones. */
			CHECK((uintptr_t)st->enc != was_enc);
			CHECK((uintptr_t)st->dec != was_dec);
		}
#ifdef HAVE_OPUS
		CHECK_UINT(packet_codecs, 1);
#else
		CHECK_UINT(packet_codecs, 0);
#endif
		tm_call_down(&t, &w);
	}
}

/* --- cases: the local hold silence --------------------------------------- */

/*
 * Digital silence through G.711 mu-law is 0xff in every octet, so this oracle
 * is exact: one byte that is not says the captured room reached the wire. A
 * tone power measurement could not say the same, because a payload quiet
 * enough to fail a power threshold is still not the silence RFC 3264 asks a
 * holder to send.
 */
static bool tm_payload_silent(const uint8_t *pay, size_t len)
{
	if (!len)
		return false;

	for (size_t i = 0; i < len; i++) {
		if (pay[i] != 0xffu)
			return false;
	}

	return true;
}

/*
 * The local hold silence (udsp.h udsp_media.tx_silence, hold-tx). The capture
 * side plays a 1000 Hz tone throughout. A silent payload therefore shows the
 * flag working, and a payload that carries the tone is the room that the held
 * party must not hear.
 *
 * The first three cases share one stream, unlike the rest of this file. The
 * test is about the toggle on a live stream, and the third case reads the
 * source and the clock across it. A fresh fixture per case would draw a new
 * SSRC, and nothing would be left to compare.
 */
static void case_tx_silence(void)
{
	const struct udsp_codec_ops *ops = NULL;
	uint8_t buf[TM_PKT_MAX];
	struct tm_pkt p = { 0 };
	struct tm_pkt q = { 0 };
	struct udsp_stream *st = NULL;
	uintptr_t enc = 0, dec = 0;
	struct udsp_media m;
	struct tm_wire w;
	unsigned quiet = 0;
	unsigned audio = 0;
	struct eh t;
	ssize_t n;

	CASE("a held stream encodes silence in place of the captured room");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.tx_silence = true;
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);

		for (unsigned i = 0; i < 4u; i++) {
			n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300);
			if (n <= 0 || !tm_rtp_parse(buf, (size_t)n, &p))
				break;
			CHECK_UINT(p.pt, 0);
			CHECK_UINT(p.len, 160);
			if (tm_payload_silent(p.payload, p.len))
				quiet++;
		}
		/* Every packet, not merely one: a flag the tick read once would
		 * leave the rest of the call carrying the room. */
		CHECK_UINT(quiet, 4);
	}

	CASE("clearing the hold silence puts the captured room back on the wire");
	{
		/* Nothing of the held phase may still be queued when the
		 * toggle arrives, so the pair the next case compares is
		 * adjacent on the wire and its sequence step is a short one. */
		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300);
		CHECK(n > 0 && tm_rtp_parse(buf, (size_t)n, &p));
		CHECK(tm_payload_silent(p.payload, p.len));

		/* The instances the running stream holds, read before the
		 * reconfiguration: a description that moves only this field
		 * must not recreate them (the case_reconfig identity oracle). */
		if (st) {
			enc = (uintptr_t)st->enc;
			dec = (uintptr_t)st->dec;
			ops = st->ops;
		}

		m.tx_silence = false;
		tm_stream_config(&t, &w, &m);
		for (unsigned i = 0; i < 8u; i++) {
			n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300);
			if (n <= 0 || !tm_rtp_parse(buf, (size_t)n, &q))
				break;
			if (tm_payload_silent(q.payload, q.len))
				continue;
			audio++;
			break;
		}
		CHECK_UINT(audio, 1);
		CHECK(tm_payload_power(q.payload, q.len, false, 1000.0) > 1e6);
	}

	CASE("the hold silence and its release keep one RTP source and clock");
	{
		/* A sender the reconfiguration reset would draw a fresh SSRC,
		 * sequence and timestamp from the entropy stream_config already
		 * took, so the identity is what carries this. */
		CHECK_UINT(q.ssrc, p.ssrc);
		CHECK((uint16_t)(q.seq - p.seq) >= 1u);
		CHECK((uint16_t)(q.seq - p.seq) < 100u);
		CHECK((int32_t)(q.ts - p.ts) > 0);
		CHECK((int32_t)(q.ts - p.ts) < 100 * 160);
		/* The packets never stopped, so the release opens no talkspurt
		 * and RFC 3551 wants no marker on it; an adopted sender would
		 * mark its first packet, so this is a second witness that the
		 * description moved and the source did not. */
		CHECK(!q.marker);
		CHECK(st != NULL);
		if (st) {
			CHECK((uintptr_t)st->enc == enc);
			CHECK((uintptr_t)st->dec == dec);
			CHECK(st->ops == ops);
		}
		tm_call_down(&t, &w);
	}

	CASE("the hold silence is resampled, so a 16 kHz line holds quiet");
	{
		quiet = 0;
		audio = 0;
		if (!tm_up_rate(&t, "USA", UDSP_SAMPLE_RATE_WB) ||
		    !tm_wire_open(&w, 0)) {
			CHECK(false);
			return;
		}
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.tx_silence = true;
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		/* The 2:1 step is in the transmit path on this line, which is
		 * what makes the case a wideband one. */
		if (st)
			CHECK(st->resample);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 150);

		for (unsigned i = 0; i < 4u; i++) {
			n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 600);
			if (n <= 0 || !tm_rtp_parse(buf, (size_t)n, &p))
				break;
			CHECK_UINT(p.len, 160);
			if (tm_payload_silent(p.payload, p.len))
				quiet++;
		}
		CHECK_UINT(quiet, 4);

		/*
		 * The same line with the flag clear carries the tone at 1000 Hz
		 * and not at its 2000 Hz rate-error image. Without this the case
		 * would also pass on a wideband transmit path that sends nothing
		 * at all, which is a different fault with the same wire.
		 */
		m.tx_silence = false;
		tm_stream_config(&t, &w, &m);
		for (unsigned i = 0; i < 8u; i++) {
			n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 600);
			if (n <= 0 || !tm_rtp_parse(buf, (size_t)n, &q))
				break;
			if (tm_payload_silent(q.payload, q.len))
				continue;
			audio++;
			break;
		}
		CHECK_UINT(audio, 1);
		{
			double want = tm_ulaw_payload_power(q.payload, q.len,
							    1000.0);
			double image = tm_ulaw_payload_power(q.payload, q.len,
							     2000.0);

			CHECK(want > 1e6);
			CHECK(image * 100.0 < want);
		}
		tm_call_down(&t, &w);
	}

	/*
	 * The order inside the transmit chain, which the wire alone cannot
	 * show: a payload of 0xff reads the same whether the zeros went in
	 * ahead of the resampler or straight into the encoder. The voice
	 * detector is the discriminator, because it taps the frame before
	 * either. With the detector on, a substitution ahead of it closes the
	 * talkspurt and RFC 3389 comfort noise replaces the packets, and the
	 * level the description carries is 127, the reading of digital
	 * silence. A zeroing at the encoder would leave the detector looking
	 * at the 1000 Hz tone, so the talkspurt would stay open and media
	 * packets, not comfort noise, would be on the wire. The line runs at
	 * 16 kHz, so the detector's own feed is resampled and the case pins
	 * the resampler too.
	 */
	CASE("the hold silence reaches the voice detector, not only the encoder");
	{
		struct tm_pkt r = { 0 };
		unsigned media_pkts = 0;
		unsigned cn_pkts = 0;
		unsigned cn_level = 0;

		if (!tm_up_rate(&t, "USA", UDSP_SAMPLE_RATE_WB) ||
		    !tm_wire_open(&w, 0)) {
			CHECK(false);
			return;
		}
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.vad = true;
		m.cng = true;
		m.tx_silence = true;
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st)
			CHECK(st->resample);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);

		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (!tm_rtp_parse(buf, (size_t)n, &r))
				continue;
			if (r.pt == 13u) {
				cn_pkts++;
				cn_level = r.payload[0];
				CHECK_UINT(r.len, 1);
				continue;
			}
			media_pkts++;
		}
		/* RFC 3389: one description opens the silence, and 127 is the
		 * -dBov of a frame with no energy at all (dsp/cn.c). */
		CHECK_UINT(cn_pkts, 1);
		CHECK_UINT(media_pkts, 0);
		CHECK_UINT(cn_level, 127);

		/*
		 * The control arm. The same tone against the same detector with
		 * the flag clear has to reopen the talkspurt, or the case above
		 * would also pass on a detector that never says voice and a tone
		 * too quiet to be one.
		 */
		m.tx_silence = false;
		tm_stream_config(&t, &w, &m);
		eh_wait_ms(&t, 200);
		media_pkts = 0;
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (!tm_rtp_parse(buf, (size_t)n, &r) || r.pt != 0u)
				continue;
			media_pkts++;
		}
		CHECK(media_pkts > 3u);
		tm_call_down(&t, &w);
	}

	/*
	 * The telephone events are silenced too (udsp.h udsp_media.tx_silence).
	 * A held leg puts nothing of its own on the wire, and RFC 4733 2.5.1.4
	 * lets the far end close the digit the cut interrupted: the ends it
	 * already saw are what close it there, which is the rule a direction
	 * that stops sending already follows.
	 */
	CASE("a hold cuts a telephone event in flight");
	{
		struct tm_pkt r = { 0 };
		unsigned before = 0;
		unsigned after = 0;
		unsigned ends = 0;
		unsigned voice = 0;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 40);
		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;

		/* Long enough that the cut falls in the middle of it. */
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '5', 2000) == UDSP_OK);
		eh_wait_ms(&t, 60);
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (tm_rtp_parse(buf, (size_t)n, &r) && r.pt == 101u)
				before++;
		}
		/* The event must be in flight, or the cut proves nothing. */
		CHECK(before > 0u);

		/*
		 * The configuration blocks on the tick ack, so the engine has
		 * applied the cut before it returns: every packet still queued
		 * was built before it, and the window after the drain is the
		 * held stream alone.
		 */
		m.tx_silence = true;
		tm_stream_config(&t, &w, &m);
		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;
		eh_wait_ms(&t, 300);
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (!tm_rtp_parse(buf, (size_t)n, &r))
				continue;
			if (r.pt == 101u) {
				after++;
				if (r.payload[1] & 0x80u)
					ends++;
				continue;
			}
			if (r.pt == 0u && !tm_payload_silent(r.payload, r.len))
				voice++;
		}
		/* No further packet of the event, and no end packet trailing
		 * behind it either. */
		CHECK_UINT(after, 0);
		CHECK_UINT(ends, 0);
		/* And what the held stream does carry is the silence. */
		CHECK_UINT(voice, 0);
		tm_call_down(&t, &w);
	}

	CASE("a held stream refuses a new telephone event");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		m.tx_silence = true;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 40);
		/* UNSUPPORTED and not TRANSIENT: no retry can make a held leg
		 * send a digit, which is the answer the other refusals of this
		 * gate already give. */
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '5', 100) == UDSP_UNSUPPORTED);

		/* The release takes the refusal back with it. */
		m.tx_silence = false;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '5', 100) == UDSP_OK);
		tm_call_down(&t, &w);
	}
}

/* --- cases: RTCP --------------------------------------------------------- */

/*
 * A compound the oracle writes byte by byte: a receiver report naming the
 * stream's own SSRC plus the source description RFC 3550 6.1 requires. 48
 * bytes: 32 of report, 16 of description. A zero LSR asks for no round trip.
 */
static void tm_rr_sdes_build(uint8_t *pkt, uint32_t ours, uint8_t fraction,
			     uint32_t jitter)
{
	static const uint32_t far_ssrc = 0x0badc0deu;

	memset(pkt, 0, 48u);
	pkt[0] = 0x81u;			/* version 2, one report block */
	pkt[1] = 201u;			/* receiver report */
	pkt[3] = 7u;			/* 32 bytes */
	pkt[4] = (uint8_t)(far_ssrc >> 24);
	pkt[5] = (uint8_t)(far_ssrc >> 16);
	pkt[6] = (uint8_t)(far_ssrc >> 8);
	pkt[7] = (uint8_t)far_ssrc;
	pkt[8] = (uint8_t)(ours >> 24);
	pkt[9] = (uint8_t)(ours >> 16);
	pkt[10] = (uint8_t)(ours >> 8);
	pkt[11] = (uint8_t)ours;
	pkt[12] = fraction;
	pkt[20] = (uint8_t)(jitter >> 24);
	pkt[21] = (uint8_t)(jitter >> 16);
	pkt[22] = (uint8_t)(jitter >> 8);
	pkt[23] = (uint8_t)jitter;

	pkt[32] = 0x81u;
	pkt[33] = 202u;			/* source description */
	pkt[35] = 3u;			/* 16 bytes */
	pkt[36] = pkt[4];
	pkt[37] = pkt[5];
	pkt[38] = pkt[6];
	pkt[39] = pkt[7];
	pkt[40] = 1u;			/* CNAME */
	pkt[41] = 4u;
	memcpy(pkt + 42, "peer", 4);
}

static void case_rtcp(void)
{
	struct tm_wire w;
	struct udsp_media m;
	struct eh t;
	double phase = 0.0;

	CASE("an RTCP report leaves at the interval and a BYE ends the stream");
	{
		uint8_t buf[TM_PKT_MAX];
		unsigned types[8];
		unsigned count = 0;
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		for (unsigned i = 0; i < 20; i++)
			tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);

		/* The interval floor is five seconds of engine time, so no
		 * report may leave in the first two: an interval shortened to
		 * a tick or two would flood the far end. The empty punch of
		 * the first tick is not a report and tm_recv reads past it. */
		eh_wait_ms(&t, 2000);
		CHECK(tm_recv(w.rtcp_peer, buf, sizeof(buf)) < 0);
		n = tm_recv_wait(&t, w.rtcp_peer, buf, sizeof(buf), 6000);
		CHECK(n > 0);
		if (n > 0) {
			uint32_t ssrc = ((uint32_t)buf[4] << 24) |
					((uint32_t)buf[5] << 16) |
					((uint32_t)buf[6] << 8) | buf[7];
			uint32_t msw = ((uint32_t)buf[8] << 24) |
				       ((uint32_t)buf[9] << 16) |
				       ((uint32_t)buf[10] << 8) | buf[11];

			CHECK(tm_rtcp_walk(buf, (size_t)n, types, 8, &count));
			CHECK(count >= 2u);
			CHECK_UINT(types[0], 200);	/* sender report */
			CHECK_UINT(types[1], 202);	/* source description */
			CHECK_UINT(buf[0] & 0x1fu, 1);	/* one report block */
			CHECK(ssrc != 0);
			/* A plausible NTP stamp: after 2020 and inside the
			 * era that ends in 2036. */
			CHECK(msw > 3786825600u);
			CHECK(msw < 4290000000u);
		}

		/* One report in the window, never a burst: with the interval
		 * still standing the next one is five seconds out. */
		eh_wait_ms(&t, 2000);
		CHECK(tm_recv(w.rtcp_peer, buf, sizeof(buf)) < 0);

		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		n = tm_recv(w.rtcp_peer, buf, sizeof(buf));
		CHECK(n > 0);
		if (n > 0) {
			CHECK(tm_rtcp_walk(buf, (size_t)n, types, 8, &count));
			CHECK_UINT(count, 3);
			CHECK_UINT(types[2], 203);	/* goodbye */
		}
		tm_call_close(&t, &w);
	}

	/*
	 * The far end's view of this stream only ever arrives on the inbound
	 * RTCP path, so a stream that parses nothing reports a far loss and a
	 * far jitter of zero for the whole call and the backend has no MOS-CQ
	 * input.
	 */
	CASE("a receiver report from the far end reaches the statistics");
	{
		struct udsp_media_stats stats;
		uint8_t buf[TM_PKT_MAX];
		uint8_t pkt[64];
		struct tm_pkt p;
		uint32_t ours = 0;
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);

		/* The synchronization source comes off the wire, never out of a
		 * field the engine thread owns. */
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300);
		CHECK(n > 0);
		if (n > 0 && tm_rtp_parse(buf, (size_t)n, &p))
			ours = p.ssrc;
		CHECK(ours != 0);

		tm_rr_sdes_build(pkt, ours, 0x40u, 800u);
		CHECK(send(w.rtcp_peer, pkt, 48u, MSG_DONTWAIT | MSG_NOSIGNAL) > 0);
		eh_wait_ms(&t, 300);

		CHECK(udsp_stream_stats(t.s, 0, 0, &stats) == UDSP_OK);
		/* RFC 3550 A.3 fraction, in units of 1/256, straight through. */
		CHECK_UINT(stats.loss_rate_far, 0x40);
		/* 800 timestamp units at 8 kHz is 100 ms. */
		CHECK_UINT(stats.jitter_far, 100);
		tm_call_down(&t, &w);
	}

	CASE("a receive only stream answers with a receiver report");
	{
		uint8_t buf[TM_PKT_MAX];
		unsigned types[8];
		unsigned count = 0;
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.direction = UDSP_MEDIA_RECVONLY;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		for (unsigned i = 0; i < 20; i++)
			tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);

		n = tm_recv_wait(&t, w.rtcp_peer, buf, sizeof(buf), 6000);
		CHECK(n > 0);
		if (n > 0) {
			CHECK(tm_rtcp_walk(buf, (size_t)n, types, 8, &count));
			CHECK(count >= 2u);
			CHECK_UINT(types[0], 201);	/* receiver report */
			CHECK_UINT(types[1], 202);
		}
		tm_call_down(&t, &w);
	}
}

/* --- cases: the line chain ----------------------------------------------- */

/* The condition is the one the caller checks after the poll. The poll ends at
 * 20 s of engine time, so a canceller that does not converge still fails
 * those checks. each, when given, runs on every pass of the wait. */
static void tm_lec_converge(struct eh *t, uint32_t delay,
			    void (*each)(void *arg), void *arg)
{
	uint32_t end = eh_now_ms() + 20000u;
	uint64_t stop = eh_mono_us() + 20000000u + EH_WAIT_SLACK_US;
	struct udsp_lec_stats ls;

	while (eh_now_ms() < end && eh_mono_us() < stop) {
		eh_wait_each(t, 100, each, arg);
		if (!atomic_load(&t->s->lec_ready[0]) ||
		    atomic_load(&t->s->lec_delay[0]) != delay)
			continue;
		if (udsp_line_lec_stats(t->s, 0, &ls) == UDSP_OK &&
		    ls.frames > 1000u && ls.coef_energy_micro > 0u &&
		    ls.erle_mdb >= 6000)
			return;
	}
}

/* How far ahead of the playout the noise far end stays: the initial depth of
 * the jitter buffer in tm_media. */
#define TM_NOISE_CUSHION_MS	20u

/*
 * A broadband far end for the canceller: white noise at -10 dBm0, uniform, in
 * 10 ms packets paced against engine time. A dial tone follows a 4th-order
 * recurrence, so a causal filter predicts its echo ahead of tap 0; noise
 * cannot be predicted, so an echo before tap 0 stays in the residual.
 */
struct tm_noise_far {
	struct tm_wire *w;
	uint32_t state;
	uint32_t start;
	unsigned sent;
};

static void tm_noise_far_send(void *arg)
{
	/* 0 dBm0 is 3.14 dB under a full-scale sine (dsp/tone_gen.c), and a
	 * uniform source peaks at its rms times sqrt(3). */
	double amp = 32767.0 * pow(10.0, (-10.0 - 3.14) / 20.0) * sqrt(1.5);
	struct tm_noise_far *far = arg;

	while (far->sent * 10u <
	       eh_now_ms() - far->start + TM_NOISE_CUSHION_MS) {
		int16_t pcm[80];

		for (unsigned i = 0; i < 80u; i++) {
			far->state ^= far->state << 13;
			far->state ^= far->state >> 17;
			far->state ^= far->state << 5;
			pcm[i] = (int16_t)lrint(amp * (far->state /
						       4294967295.0 * 2.0 -
						       1.0));
		}
		tm_pcm_send(far->w, 0, pcm, 80u, false);
		far->sent++;
	}
}

/*
 * One call with the model hybrid at echo_delay samples, driven by the dial
 * tone or by the noise far end, until the canceller converges on the
 * reference at 160. Under noise the peak tap must name the echo within one
 * tap. The dial tone has two frequencies, so many taps fit its echo equally
 * well and its peak tap is not checked. false when the call does not come up.
 */
static bool tm_lec_echo_check(struct eh *t, struct udsp_chan_cfg *ccfg,
			      uint32_t echo_delay, bool noise)
{
	struct tm_noise_far far = { .state = 0x0f1e2d3cu };
	struct udsp_lec_stats ls;
	struct udsp_media m;
	struct tm_wire w;
	struct udsp_ec *ec;

	if (!tm_call_up(t, "USA", &w, 0))
		return false;
	CHECK(eh_hook_seize(t));
	CHECK(udsp_chan_config(t->s, 0, ccfg) == UDSP_OK);
	tm_media(&m, UDSP_SEL_PCMU, 0, 10);
	tm_stream_config(t, &w, &m);
	CHECK(udsp_stream_start(t->s, 0, 0) == UDSP_OK);
	/* The backend grants the ring the engine asked for, one period above
	 * its prefill, and reports that queue from delay(), so the calibrated
	 * loop is 240 frames whatever echo_delay is, and the reference stands
	 * one period under it. */
	atomic_store(&tm_be.echo_delay, echo_delay);
	atomic_store(&tm_be.echo_gain, 512u);
	atomic_store(&tm_be.src, TM_SRC_ECHO);
	if (noise) {
		far.w = &w;
		far.start = eh_now_ms();
		tm_lec_converge(t, 160u, tm_noise_far_send, &far);
	} else {
		CHECK(udsp_tone_play(t->s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
		tm_lec_converge(t, 160u, NULL, NULL);
		CHECK(udsp_tone_stop(t->s, 0) == UDSP_OK);
	}
	/*
	 * Turning the canceller off is a SYNC-TICK op. Its ack orders the reads
	 * of lec_ready and lec_delay below against the tick that wrote them.
	 * The record that eh_lec_state_wait takes describes the last frame the
	 * filter ran.
	 */
	ccfg->ec_enable = false;
	CHECK(udsp_chan_config(t->s, 0, ccfg) == UDSP_OK);

	ec = atomic_load(&t->s->chan[0].ec);
	CHECK(ec != NULL);
	CHECK(atomic_load(&t->s->lec_ready[0]));
	CHECK_UINT(atomic_load(&t->s->lec_delay[0]), 160);
	if (ec) {
		CHECK(eh_lec_state_wait(t, &ls, false, false));
		printf("# echo delay %u: lec delay %u, erle %.1f dB, peak tap "
		       "%u\n", echo_delay,
		       (unsigned)atomic_load(&t->s->lec_delay[0]),
		       (double)ls.erle_mdb / 1000.0, ls.coef_peak_tap);
		CHECK(ls.frames > 1000u);
		CHECK(ls.coef_energy_micro > 0u);
		CHECK(ls.erle_mdb >= 6000);
		if (noise)
			CHECK(ls.coef_peak_tap + 1u >= echo_delay - 160u &&
			      ls.coef_peak_tap <= echo_delay - 160u + 1u);
	}
	ccfg->ec_enable = true;
	tm_call_down(t, &w);

	return true;
}

static void case_lec(void)
{
	struct udsp_lec_stats ls;
	struct udsp_chan_cfg ccfg;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;

	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.ec_enable = true;
	ccfg.ec_tail_ms = 32;

	CASE("the echo canceller converges on the measured loop delay");
	if (!tm_lec_echo_check(&t, &ccfg, 240u, false))
		return;

	CASE("the capture backlog joins the canceller's reference");
	{
		struct udsp_job job = { 0 };
		int32_t status = -1;
		struct udsp_ec *ec;

		/* The echo crosses the playback queue and the capture
		 * backlog, and a reference built from the queue alone sits
		 * 160 samples early on the MT7987 (engine.h loop_ref). The
		 * backend claims one period of backlog from the first tick, the
		 * echo rides the sum, and the reference must follow it. */
		if (!tm_open_only(&t, "USA", 0, 0, 80u) ||
		    udsp_start(t.s, &job) != UDSP_OK ||
		    !eh_job_wait(&t, job.token, 5000, &status) || status != 0 ||
		    !tm_wire_open(&w, 0)) {
			CHECK(false);
			return;
		}
		CHECK(eh_hook_seize(&t));
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		atomic_store(&tm_be.echo_delay, 320u);
		atomic_store(&tm_be.echo_gain, 512u);
		atomic_store(&tm_be.src, TM_SRC_ECHO);
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
		tm_lec_converge(&t, 240u, NULL, NULL);
		CHECK(udsp_tone_stop(t.s, 0) == UDSP_OK);
		ccfg.ec_enable = false;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);

		ec = atomic_load(&t.s->chan[0].ec);
		CHECK(ec != NULL);
		CHECK(atomic_load(&t.s->lec_ready[0]));
		CHECK_UINT(atomic_load(&t.s->lec_delay[0]), 240);
		if (ec) {
			CHECK(eh_lec_state_wait(&t, &ls, false, false));
			CHECK(ls.frames > 1000u);
			CHECK(ls.coef_energy_micro > 0u);
			CHECK(ls.erle_mdb >= 6000);
		}
		ccfg.ec_enable = true;
		tm_call_down(&t, &w);
	}

	CASE("a loop delay past the reference reach clamps to the rate's bound");
	{
		struct udsp_job job = { 0 };
		struct engine_stats est;
		int32_t status = -1;
		struct engine *e;

		/*
		 * The ceiling of chan_lec_delay_clamp is the reach the rate
		 * affords, UDSP_LEC_HIST_MS less one frame, never the length
		 * of the ring: the ring is sized for the wideband frame, so
		 * reading the ceiling off the array would hand the 8 kHz line
		 * a reference the ring cannot hold in time. A claimed backlog
		 * of 2000 frames puts loop_ref past the bound from the first
		 * calibration window. The applied delay must then stand at
		 * 8000 * 256 / 1000 - 80. The reference is state of the line,
		 * so the line carries a canceller.
		 */
		if (!tm_open_only(&t, "USA", 0, 0, 2000u) ||
		    udsp_start(t.s, &job) != UDSP_OK ||
		    !eh_job_wait(&t, job.token, 5000, &status) || status != 0 ||
		    !tm_wire_open(&w, 0)) {
			CHECK(false);
			return;
		}
		CHECK(eh_hook_seize(&t));
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 500);
		CHECK(atomic_load(&t.s->lec_ready[0]));
		CHECK_UINT(atomic_load(&t.s->lec_delay[0]), 1968u);
		/* The hold of a played digit reads the measurement before the
		 * clamp and applies the clamp itself (chan.c
		 * chan_digit_hold_arm). */
		e = atomic_load(&t.s->engine_pub);
		CHECK(e != NULL);
		if (e) {
			engine_stats_get(e, &est);
			CHECK(est.delay_calibrated);
			CHECK(est.loop_ref > 1968);
			CHECK_UINT(atomic_load(&t.s->lec_loop), est.loop_ref);
		}
		tm_call_down(&t, &w);
	}

	CASE("the canceller stays frozen while the loop delay measurement is open");
	{
		struct engine_stats est;
		struct udsp_ec *ec;
		struct engine *e;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK(eh_hook_seize(&t));
		/* Every eighth playback delay fails, so the engine restarts its
		 * calibration window before it can ever complete. The first
		 * failure falls on tick 8 and the fallback needs three of them,
		 * so ticks 1..7 are the frozen window. */
		atomic_store(&tm_be.delay_fail, 8u);
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		atomic_store(&tm_be.echo_delay, 240u);
		atomic_store(&tm_be.src, TM_SRC_ECHO);
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
		eh_wait_ms(&t, 40);
		CHECK(!atomic_load(&t.s->lec_ready[0]));
		/* The open window still publishes the loop of the digit hold:
		 * the nominal prefill of this engine, not the zero of the
		 * engine start (chan.c chan_lec_delay_track). */
		e = atomic_load(&t.s->engine_pub);
		CHECK(e != NULL);
		if (e) {
			engine_stats_get(e, &est);
			CHECK(!est.delay_calibrated);
			CHECK_UINT(atomic_load(&t.s->lec_loop),
				   est.prefill_periods * est.frames_per_period);
		}
		CHECK(udsp_tone_stop(t.s, 0) == UDSP_OK);
		ccfg.ec_enable = false;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		atomic_store(&tm_be.delay_fail, 0u);

		ec = atomic_load(&t.s->chan[0].ec);
		CHECK(ec != NULL);
		if (ec) {
			CHECK(eh_lec_state_wait(&t, &ls, false, false));
			/* Frozen means the filter never moved (dsp/lec.h). */
			CHECK_UINT(ls.coef_energy_micro, 0);
			CHECK(ls.erle_mdb < 3000);
		}
		ccfg.ec_enable = true;
		tm_call_down(&t, &w);
	}

	/*
	 * The engine restarts its calibration on every xrun and this window
	 * never completes even once. A restart count that only sees the
	 * calibrated-to-uncalibrated edge stays at zero, and the canceller
	 * then stays frozen for the life of the run. The engine's own monotonic
	 * count is what reaches the fallback.
	 */
	CASE("three calibration restarts fall back to the nominal prefill");
	{
		struct engine_stats est;
		struct udsp_ec *ec;
		struct engine *e;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK(eh_hook_seize(&t));
		atomic_store(&tm_be.delay_fail, 8u);
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		atomic_store(&tm_be.echo_delay, 240u);
		atomic_store(&tm_be.echo_gain, 512u);
		atomic_store(&tm_be.src, TM_SRC_ECHO);
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
		eh_wait_ms(&t, 8000);
		CHECK(udsp_tone_stop(t.s, 0) == UDSP_OK);
		ccfg.ec_enable = false;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		atomic_store(&tm_be.delay_fail, 0u);

		e = atomic_load(&t.s->engine_pub);
		CHECK(e != NULL);
		if (e) {
			engine_stats_get(e, &est);
			CHECK(!est.delay_calibrated);
			CHECK(est.cal_restarts >= 3u);
		}
		/* prefill 3 periods of 80 frames is the nominal reference. */
		CHECK(atomic_load(&t.s->lec_ready[0]));
		CHECK_UINT(atomic_load(&t.s->lec_delay[0]), 160);
		ec = atomic_load(&t.s->chan[0].ec);
		CHECK(ec != NULL);
		if (ec) {
			CHECK(eh_lec_state_wait(&t, &ls, false, false));
			CHECK(ls.frames > 100u);
			/* The canceller adapts again once the reference stands
			 * in, which a permanent freeze denies it. */
			CHECK(ls.coef_energy_micro > 0u);
		}
		ccfg.ec_enable = true;
		tm_call_down(&t, &w);
	}

	/*
	 * The recalibration hand-off: a capture backlog that appears mid-run
	 * moves the echo loop, the engine measures the move, and the filter
	 * that adapted against the old timing would subtract a misaligned
	 * estimate from every frame after it. chan_lec_run must reset the
	 * canceller when the applied delay changes (dsp/lec.h
	 * udsp_ec_nlms_reset), which the restarted frame count shows.
	 */
	CASE("a recalibrated loop delay restarts the canceller");
	{
		struct udsp_ec *ec;
		uint32_t frames_first = 0;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK(eh_hook_seize(&t));
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		atomic_store(&tm_be.echo_delay, 240u);
		atomic_store(&tm_be.echo_gain, 512u);
		atomic_store(&tm_be.src, TM_SRC_ECHO);
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
		eh_wait_ms(&t, 12000);
		ccfg.ec_enable = false;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		ec = atomic_load(&t.s->chan[0].ec);
		CHECK(ec != NULL);
		CHECK_UINT(atomic_load(&t.s->lec_delay[0]), 160);
		if (ec) {
			CHECK(eh_lec_state_wait(&t, &ls, false, false));
			frames_first = ls.frames;
			CHECK(ls.frames > 1000u);
			CHECK(ls.coef_energy_micro > 0u);
		}
		ccfg.ec_enable = true;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);

		/* The claimed backlog appears mid-run and the echo rides the
		 * new sum (engine.h loop_ref). A delay() failure every 40
		 * ticks restarts the calibration window with room to complete
		 * between failures, so the engine measures the move. */
		atomic_store(&tm_be.avail_frames, 160u);
		atomic_store(&tm_be.echo_delay, 400u);
		atomic_store(&tm_be.delay_fail, 40u);
		eh_wait_ms(&t, 2000);
		atomic_store(&tm_be.delay_fail, 0u);
		eh_wait_ms(&t, 7000);
		CHECK(udsp_tone_stop(t.s, 0) == UDSP_OK);
		ccfg.ec_enable = false;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(atomic_load(&t.s->lec_ready[0]));
		CHECK_UINT(atomic_load(&t.s->lec_delay[0]), 320);
		if (ec) {
			CHECK(eh_lec_state_wait(&t, &ls, false, false));
			/* Restarted: counted from the last reset, not from
			 * stream start, and adapted again on the re-timed
			 * reference. */
			CHECK(ls.frames < frames_first);
			CHECK(ls.coef_energy_micro > 0u);
			CHECK(ls.erle_mdb >= 6000);
		}
		ccfg.ec_enable = true;
		tm_call_down(&t, &w);
	}

	/*
	 * The restarts-only half of the recalibration hand-off: the window
	 * restarts and completes at the same delay value. The alignment in
	 * samples is the history write position less the applied delay,
	 * against a history that gains one frame per tick. A re-latch at the
	 * same value is therefore the same alignment, and the taps stand. The
	 * filter keeps adapting across the restart and counts no re-time.
	 */
	CASE("a recalibration at the same delay keeps the canceller");
	{
		struct udsp_ec *ec;
		uint32_t frames_first = 0;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK(eh_hook_seize(&t));
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		atomic_store(&tm_be.echo_delay, 240u);
		atomic_store(&tm_be.echo_gain, 512u);
		atomic_store(&tm_be.src, TM_SRC_ECHO);
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
		eh_wait_ms(&t, 12000);
		ccfg.ec_enable = false;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		ec = atomic_load(&t.s->chan[0].ec);
		CHECK(ec != NULL);
		CHECK_UINT(atomic_load(&t.s->lec_delay[0]), 160);
		if (ec) {
			CHECK(eh_lec_state_wait(&t, &ls, false, false));
			frames_first = ls.frames;
			CHECK(ls.frames > 1000u);
			CHECK(ls.coef_energy_micro > 0u);
		}
		ccfg.ec_enable = true;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);

		/* Neither the claimed backlog nor the echo position move: the
		 * recalibration falls on the value already applied. */
		atomic_store(&tm_be.delay_fail, 40u);
		eh_wait_ms(&t, 2000);
		atomic_store(&tm_be.delay_fail, 0u);
		eh_wait_ms(&t, 7000);
		CHECK(udsp_tone_stop(t.s, 0) == UDSP_OK);
		ccfg.ec_enable = false;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(atomic_load(&t.s->lec_ready[0]));
		CHECK_UINT(atomic_load(&t.s->lec_delay[0]), 160);
		if (ec) {
			CHECK(eh_lec_state_wait(&t, &ls, false, false));
			/* Kept: the count runs on from the first half of the
			 * case instead of starting over. */
			CHECK(ls.frames > frames_first);
			CHECK(ls.coef_energy_micro > 0u);
			CHECK(ls.erle_mdb >= 6000);
		}
		/* The surface says the same thing: the restarts cost no
		 * re-time at all. */
		CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
		CHECK_UINT(ls.retimes, 0);
		ccfg.ec_enable = true;
		tm_call_down(&t, &w);
	}

	/*
	 * A udsp_stop/udsp_start pair replaces the engine while the stream and
	 * its canceller instance survive (stream.c
	 * stream_lec_release runs only when the line's last stream stops).
	 * The new engine measures the same 240-frame loop and counts
	 * cal_restarts from zero, so the value and restarts compares both
	 * miss the swap: only the serial compare re-times the reference
	 * (chan.c chan_lec_delay_track), and the filter that adapted against
	 * the old ring must start over on it.
	 */
	CASE("a restarted engine restarts the canceller");
	{
		struct udsp_job job = { 0 };
		struct engine_stats est;
		struct engine *e;
		struct udsp_ec *ec;
		int32_t status = -1;
		uint32_t frames_first = 0;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		/* The hook level survives the engine swap below. */
		CHECK(eh_hook_seize(&t));
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		atomic_store(&tm_be.echo_delay, 240u);
		atomic_store(&tm_be.echo_gain, 512u);
		atomic_store(&tm_be.src, TM_SRC_ECHO);
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
		eh_wait_ms(&t, 12000);
		CHECK(udsp_tone_stop(t.s, 0) == UDSP_OK);
		ccfg.ec_enable = false;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		ec = atomic_load(&t.s->chan[0].ec);
		CHECK(ec != NULL);
		CHECK_UINT(atomic_load(&t.s->lec_delay[0]), 160);
		if (ec) {
			CHECK(eh_lec_state_wait(&t, &ls, false, false));
			frames_first = ls.frames;
			CHECK(ls.frames > 1000u);
			CHECK(ls.coef_energy_micro > 0u);
		}
		ccfg.ec_enable = true;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);

		CHECK(udsp_stop(t.s, &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 5000, &status));
		CHECK_UINT((uint32_t)status, UDSP_OK);
		status = -1;
		CHECK(udsp_start(t.s, &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 5000, &status));
		CHECK_UINT((uint32_t)status, UDSP_OK);

		/* The same instance, not a fresh one: the restart proof below
		 * is its frame counter, so the counter must have lived through
		 * the swap. */
		CHECK(atomic_load(&t.s->chan[0].ec) == ec);
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
		eh_wait_ms(&t, 3000);
		CHECK(udsp_tone_stop(t.s, 0) == UDSP_OK);
		ccfg.ec_enable = false;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);

		e = atomic_load(&t.s->engine_pub);
		CHECK(e != NULL);
		if (e) {
			engine_stats_get(e, &est);
			CHECK_UINT(est.cal_restarts, 0);
		}
		CHECK(atomic_load(&t.s->lec_ready[0]));
		CHECK_UINT(atomic_load(&t.s->lec_delay[0]), 160);
		if (ec) {
			CHECK(eh_lec_state_wait(&t, &ls, false, false));
			CHECK(ls.frames < frames_first);
			CHECK(ls.coef_energy_micro > 0u);
		}
		/* The swap is a re-time of its own, whatever the new engine
		 * went on to measure: the new ring is not the one the taps
		 * were aligned to. */
		CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
		CHECK_UINT(ls.retimes, 1);
		ccfg.ec_enable = true;
		tm_call_down(&t, &w);
	}

	/*
	 * The serial bump sits ahead of the uncalibrated early return, so the
	 * swap resets the canceller while the new engine's measurement window
	 * is still open: a stale converged filter would otherwise keep
	 * subtracting a misaligned estimate against the new ring for the whole
	 * window. The delay_fail knob holds the window open, because the
	 * fallback needs three restarts, and the slow pace stretches it to
	 * hundreds of wall milliseconds. Both lec_ready reads come before the
	 * record read. The second read follows the ack of the op that switches
	 * the canceller off. So lec_ready did not stand at the tick that switched
	 * it off, and the counters stand still from that tick on. The record
	 * therefore shows that the reset came before lec_ready ever stood for
	 * this engine.
	 */
	CASE("an engine swap resets the canceller inside the open window");
	{
		struct udsp_job job = { 0 };
		struct udsp_ec *ec;
		int32_t status = -1;
		uint32_t frames_first = 0;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		/* The hook level survives the engine swap below. */
		CHECK(eh_hook_seize(&t));
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		atomic_store(&tm_be.echo_delay, 240u);
		atomic_store(&tm_be.echo_gain, 512u);
		atomic_store(&tm_be.src, TM_SRC_ECHO);
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
		eh_wait_ms(&t, 8000);
		CHECK(udsp_tone_stop(t.s, 0) == UDSP_OK);
		ccfg.ec_enable = false;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		ec = atomic_load(&t.s->chan[0].ec);
		CHECK(ec != NULL);
		if (ec) {
			CHECK(eh_lec_state_wait(&t, &ls, false, false));
			frames_first = ls.frames;
			CHECK(ls.frames > 500u);
		}
		ccfg.ec_enable = true;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);

		CHECK(udsp_stop(t.s, &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 5000, &status));
		CHECK_UINT((uint32_t)status, UDSP_OK);
		/* Every eighth delay() fails, so the new engine's window
		 * restarts before its 16 samples arrive and stays open until
		 * the third restart reaches the fallback; at 20 ms a tick
		 * that is a window of at least 300 wall ms (tm_start_be
		 * re-anchors the pace, so the stopped gap is not spent as a
		 * catch-up burst). */
		atomic_store(&tm_be.delay_fail, 8u);
		atomic_store(&eh_be.pace_us, 20000u);
		status = -1;
		CHECK(udsp_start(t.s, &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 5000, &status));
		CHECK_UINT((uint32_t)status, UDSP_OK);

		CHECK(!atomic_load(&t.s->lec_ready[0]));
		ccfg.ec_enable = false;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		/* Still inside the window after the sync ack above. The filter
		 * runs no frame from that tick on, so the record below carries
		 * the state before lec_ready ever returned true for this
		 * engine. */
		CHECK(!atomic_load(&t.s->lec_ready[0]));
		if (ec) {
			CHECK(eh_lec_state_wait(&t, &ls, false, false));
			CHECK(ls.frames < frames_first);
		}
		atomic_store(&tm_be.delay_fail, 0u);
		ccfg.ec_enable = true;
		tm_call_down(&t, &w);
	}
}

/*
 * The model hybrid returns the playback 8, 16 and 40 samples before the
 * measured loop of 240 samples. The reference stands one period under the
 * loop, at 160, so the echo sits at taps 72, 64 and 40. A reference at the
 * loop delay puts the echo before tap 0, where no filter cancels it. The far
 * end is white noise, because a causal filter predicts the echo of a dial tone
 * ahead of tap 0. Each run must converge on the reference at 160, with the
 * peak tap on the echo.
 */
static void case_lec_lead(void)
{
	static const uint32_t echo_delay[3] = { 232u, 224u, 200u };
	struct udsp_chan_cfg ccfg;
	struct eh t;

	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.ec_enable = true;
	ccfg.ec_tail_ms = 32;

	CASE("the echo canceller converges on an echo that leads the loop");
	for (unsigned i = 0; i < 3u; i++) {
		if (!tm_lec_echo_check(&t, &ccfg, echo_delay[i], true))
			return;
	}
}

static void case_fax(void)
{
	struct udsp_chan_cfg ccfg;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	double phase = 0.0;

	CASE("a fax answer tone from the wire is reported as a remote tone");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_MODEM;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);

		/* CED is 2100 Hz for at least 500 ms (dsp/fax_det.c). */
		for (unsigned burst = 0; burst < 8u; burst++) {
			for (unsigned i = 0; i < 30u; i++)
				tm_tone_send(&w, 0, 10, 2100.0, 8000.0, &phase);
			eh_wait_ms(&t, 250);
		}
		eh_wait_ms(&t, 300);

		{
			const struct udsp_event *e =
				eh_ev_find(UDSP_EV_TONE, 0);

			CHECK(e != NULL);
			if (e) {
				CHECK_UINT(e->u.tone.code, UDSP_TONE_DET_CED);
				CHECK_UINT(e->u.tone.direction,
					   UDSP_TONE_DIR_REMOTE);
				CHECK_UINT(e->line, 0);
			}
		}
		tm_call_down(&t, &w);
	}

	/*
	 * A local carrier freezes the canceller, and only the local detector
	 * can report the end of it. Once MODEM detection is disarmed there is
	 * no detector left, so a flag that survives the disarm freezes the
	 * canceller for the life of the line.
	 */
	CASE("disarming modem detection releases a frozen canceller");
	{
		struct udsp_lec_stats ls;
		struct udsp_ec *ec;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK(eh_hook_seize(&t));
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_MODEM;
		ccfg.ec_enable = true;
		ccfg.ec_tail_ms = 32;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);

		/* CED from the phone side: 2100 Hz for at least 500 ms. The
		 * playback path stays silent, so the canceller has no reference
		 * and cannot have adapted before the carrier raises the freeze. */
		tm_capture_tone(2100u);
		eh_wait_ms(&t, 2000);
		{
			const struct udsp_event *e =
				eh_ev_find(UDSP_EV_TONE, 0);

			CHECK(e != NULL);
			if (e) {
				CHECK_UINT(e->u.tone.code, UDSP_TONE_DET_CED);
				CHECK_UINT(e->u.tone.direction,
					   UDSP_TONE_DIR_LOCAL);
			}
		}
		/* The tone from the phone side carries the disabler's fax
		 * policy as much as one from the wire does. */
		CHECK_UINT(atomic_load(&t.s->chan[0].fax_bypass_kind),
			   UDSP_PASSTHROUGH_FAX);
		/* Turning the canceller off is a SYNC-TICK op, and the record
		 * that eh_lec_state_wait takes describes the last frame the
		 * filter ran (the case_lec pattern). */
		ccfg.ec_enable = false;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		ec = atomic_load(&t.s->chan[0].ec);
		CHECK(ec != NULL);
		if (ec) {
			CHECK(eh_lec_state_wait(&t, &ls, false, false));
			CHECK_UINT(ls.coef_energy_micro, 0);
		}

		/* The disarm is a SYNC-TICK op too, so its ack orders this read
		 * against the tick that wrote the flag, and no detector runs
		 * from here on to write it again. */
		ccfg.ec_enable = true;
		ccfg.detect_mask = 0;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(!t.s->chan[0].fax_active);
		/* The latch dies with the arm for the same reason: no detector
		 * is left to measure its release. */
		CHECK_UINT(atomic_load(&t.s->chan[0].fax_bypass_kind),
			   UDSP_PASSTHROUGH_OFF);

		/* A real echo and a real reference: the only thing that can
		 * still hold the filter is a freeze nobody can lift. */
		atomic_store(&tm_be.echo_delay, 240u);
		atomic_store(&tm_be.echo_gain, 512u);
		atomic_store(&tm_be.src, TM_SRC_ECHO);
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
		eh_wait_ms(&t, 6000);
		ccfg.ec_enable = false;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		if (ec) {
			CHECK(eh_lec_state_wait(&t, &ls, false, false));
			/* The canceller adapts again, which a flag that
			 * survives the disarm denies it. */
			CHECK(ls.coef_energy_micro > 0u);
		}
		CHECK(udsp_tone_stop(t.s, 0) == UDSP_OK);
		tm_call_down(&t, &w);
	}
}

/* --- the answer tone disabler -------------------------------------------- */

#define TM_ANS_HZ		2100.0
#define TM_ANS_AMP		8000.0
/* G.168 7.2.1 puts the phase reversals of the modem answer tone 450 ms
 * apart, which is 45 of the 10 ms frames the far end sends. */
#define TM_ANS_REVERSAL_FRAMES	45u
/* How far ahead of the playout the far end stays. The jitter buffer holds the
 * cushion, so every packet sent is a frame played and the frames the detector
 * times are the frames this sent: a concealment repeat would move the reversal
 * spacing the ANSBAR classification measures. */
#define TM_ANS_CUSHION_MS	60u

enum tm_ans_kind {
	TM_ANS_PLAIN = 0,
	TM_ANS_REVERSED,
	TM_ANS_V21,
};

/* What the far end carries across the packets of one carrier. */
struct tm_ans {
	double phase;
	unsigned frames;
	unsigned baud;
	uint64_t bit;
};

/* The V.21 flag octet, 0x7e least significant bit first. */
static bool tm_v21_bit(uint64_t index)
{
	static const bool flag[] = {
		false, true, true, true, true, true, true, false,
	};

	return flag[index % 8u];
}

/* One 10 ms packet of V.21 channel 2 flags, the preamble a called fax sends
 * after its answer tone (the synthesizer of test_fax_det.c on the wire). */
static void tm_v21_send(struct tm_wire *w, struct tm_ans *a)
{
	int16_t pcm[80];

	for (unsigned i = 0; i < 80u; i++) {
		double hz = tm_v21_bit(a->bit) ? 1650.0 : 1850.0;

		pcm[i] = (int16_t)lrint(TM_ANS_AMP * sin(a->phase));
		a->phase += 2.0 * M_PI * hz / UDSP_SAMPLE_RATE;
		a->baud += 300u;
		if (a->baud < UDSP_SAMPLE_RATE)
			continue;
		a->baud -= UDSP_SAMPLE_RATE;
		a->bit++;
	}
	tm_pcm_send(w, 0, pcm, 80u, false);
}

static void tm_ans_frame_send(struct tm_wire *w, struct tm_ans *a,
			      enum tm_ans_kind kind)
{
	if (kind == TM_ANS_V21) {
		tm_v21_send(w, a);
		a->frames++;
		return;
	}
	/* The generator takes its phase from the caller, so half a turn added
	 * between two packets is the reversal itself. */
	if (kind == TM_ANS_REVERSED && a->frames &&
	    !(a->frames % TM_ANS_REVERSAL_FRAMES))
		a->phase += M_PI;
	tm_tone_send(w, 0, 10, TM_ANS_HZ, TM_ANS_AMP, &a->phase);
	a->frames++;
}

/* The carrier for ms of engine time, paced against it. */
static void tm_ans_send(struct eh *t, struct tm_wire *w, struct tm_ans *a,
			enum tm_ans_kind kind, uint32_t ms)
{
	uint32_t start = eh_now_ms();
	unsigned sent = 0;

	while (eh_now_ms() - start < ms) {
		while (sent * 10u < eh_now_ms() - start + TM_ANS_CUSHION_MS) {
			tm_ans_frame_send(w, a, kind);
			sent++;
		}
		eh_ev_poll(t);
		eh_sleep_ms(1);
	}
}

/* A line off hook with the canceller, its processor and the modem detectors
 * armed, and one G.711 stream on it: what every case of the disabler starts
 * from. The tick runs no canceller on an open loop. */
static bool tm_disable_up(struct eh *t, struct tm_wire *w,
			  struct udsp_chan_cfg *ccfg, struct udsp_media *m)
{
	if (!tm_up(t, "USA") || !tm_wire_open(w, 0) || !eh_hook_seize(t))
		return false;

	memset(ccfg, 0, sizeof(*ccfg));
	ccfg->detect_mask = UDSP_DET_MODEM;
	ccfg->ec_enable = true;
	ccfg->ec_nlp_enable = true;
	ccfg->ec_tail_ms = 32;
	if (udsp_chan_config(t->s, 0, ccfg) != UDSP_OK)
		return false;
	tm_media(m, UDSP_SEL_PCMU, 0, 10);
	if (udsp_stream_config(t->s, 0, 0, m, w->rtp_local, w->rtcp_local) !=
	    UDSP_OK)
		return false;

	return udsp_stream_start(t->s, 0, 0) == UDSP_OK;
}

static uint8_t tm_latch_kind(struct eh *t)
{
	return (uint8_t)atomic_load(&t->s->chan[0].fax_bypass_kind);
}

/*
 * The line canceller is created at stream config. The control thread frees it
 * when the last stream of the line stops, while chan_lec_publish reads it
 * every tick. Repeated stops under a live engine prove that the release hands
 * the instance to the tick first (UDSP_CMD_LEC_RELEASE). No tick therefore
 * reads a freed canceller.
 */
static void case_lec_stop_stress(void)
{
	struct udsp_chan_cfg ccfg;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;

	CASE("stopping the canceller's stream frees it with no tick reading it");
	if (!tm_call_up(&t, "USA", &w, 0))
		return;
	CHECK(eh_hook_seize(&t));
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.ec_enable = true;
	ccfg.ec_nlp_enable = true;
	ccfg.ec_tail_ms = 32;
	CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
	tm_media(&m, UDSP_SEL_PCMU, 0, 10);

	/* Each pass dups the wire so the stream owns and closes its own copy;
	 * the originals stay open for the next pass and tm_wire_close below. */
	for (unsigned i = 0; i < 24u; i++) {
		int rtp = dup(w.rtp_local);
		int rtcp = dup(w.rtcp_local);

		if (rtp < 0 || rtcp < 0) {
			if (rtp >= 0)
				close(rtp);
			if (rtcp >= 0)
				close(rtcp);
			CHECK(false);
			break;
		}
		CHECK(udsp_stream_config(t.s, 0, 0, &m, rtp, rtcp) == UDSP_OK);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 20);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
	}

	tm_call_close(&t, &w);
}

/*
 * The answer tone disabler of G.168 clause 7 in either direction; the line
 * answers under either policy, and its canceller record is the reference. The
 * modem policy reads ec_active 0, no processor and the modem kind, because no
 * filter runs. The fax policy reads ec_active 1, nlp_enabled 0 and the fax
 * kind. The latch is read from the channel, which tells a release from a
 * policy that never took effect.
 *
 * Every move of the latch posts UDSP_EV_LEC_POLICY, the only record of the
 * move outside the library; the tick logs nothing. The cases read each row
 * beside its state, so a missing or wrong row fails here and not on a unit.
 */
static void case_fax_disable(void)
{
	struct udsp_chan_cfg ccfg;
	struct udsp_lec_stats lec;
	struct udsp_media m;
	struct tm_ans a;
	struct tm_wire w;
	struct eh t;

	{
		uint32_t frames = 0;
		uint32_t latched = 0;

		CASE("phase reversals from the wire disable the canceller");
		if (!tm_disable_up(&t, &w, &ccfg, &m)) {
			CHECK(false);
			return;
		}
		memset(&a, 0, sizeof(a));
		eh_wait_ms(&t, 200);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		frames = lec.frames;

		/* The second reversal validates the spacing, so the modem
		 * policy latches near 900 ms and the rest is margin. */
		tm_ans_send(&t, &w, &a, TM_ANS_REVERSED, 2000);
		{
			const struct udsp_event *e =
				tm_tone_find(UDSP_TONE_DET_ANSBAR);

			CHECK(e != NULL);
			if (e) {
				CHECK_UINT(e->u.tone.direction,
					   UDSP_TONE_DIR_REMOTE);
				CHECK_UINT(e->line, 0);
			}
		}
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_MODEM);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.ec_active, 0);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_MODEM);
		latched = lec.frames;
		{
			const struct udsp_event *e =
				tm_lec_find(UDSP_LEC_REASON_LATCH);
			const struct udsp_event *tone =
				tm_tone_find(UDSP_TONE_DET_ANSBAR);

			CHECK(e != NULL);
			if (e) {
				CHECK_UINT(e->line, 0);
				CHECK_UINT(e->u.lec.kind, UDSP_PASSTHROUGH_MODEM);
				CHECK_UINT(e->u.lec.tone, UDSP_TONE_DET_ANSBAR);
				/* The feed the tone arrived on is the datum
				 * the media switch drops and the disabler
				 * keeps, so the row has to carry it. */
				CHECK_UINT(e->u.lec.direction,
					   UDSP_TONE_DIR_REMOTE);
			}
			/* The tone row leaves first, so a reader gets the cause
			 * in front of the policy it moved, and both name the
			 * same feed. */
			if (e && tone) {
				CHECK(tone < e);
				CHECK_UINT(e->u.lec.direction,
					   tone->u.tone.direction);
			}
		}

		/* Disabled means the filter runs no frame. The line answers
		 * under the modem policy above, and its record reads ec_active
		 * 0 with the counters standing still. A freeze would not give
		 * that, because a frozen canceller still filters (dsp/lec.h).
		 * The frames it resumes after the release confirm it below. A
		 * second reversal holds the policy through the quiet window
		 * that follows, and the counters are read across that burst. A
		 * quiet window of the same length would let the release run,
		 * and the burst would then latch a second time. */
		tm_ans_send(&t, &w, &a, TM_ANS_REVERSED, 500);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.ec_active, 0);
		CHECK_UINT(lec.frames, latched);

		CASE("the disabler releases when the holding band falls quiet");
		/* G.168 7.8 asks for a release inside 250 plus or minus
		 * 150 ms of the drop, and the line is silent from here. */
		eh_wait_ms(&t, 1000);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_OFF);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK(lec.frames > frames + 10u);
		CHECK_UINT(lec.nlp_enabled, 1);
		CHECK_UINT(lec.ec_active, 1);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_OFF);
		{
			const struct udsp_event *e =
				tm_lec_find(UDSP_LEC_REASON_RELEASE_QUIET);

			CHECK(e != NULL);
			if (e) {
				CHECK_UINT(e->u.lec.kind, UDSP_PASSTHROUGH_OFF);
				/* No tone released the latch, so the row names
				 * neither a tone nor a feed. */
				CHECK_UINT(e->u.lec.tone, 0);
				CHECK_UINT(e->u.lec.direction, 0);
			}
		}
		/* The latch and the release are the whole account of this case.
		 * The second reversal burst asked for the kind the line already
		 * ran under, and a row that changes nothing is not reported. */
		CHECK_UINT(eh_ev_count(UDSP_EV_LEC_POLICY), 2);

		tm_call_down(&t, &w);
	}

	{
		uint32_t frames = 0;

		CASE("a plain answer tone from the wire takes the fax policy");
		if (!tm_disable_up(&t, &w, &ccfg, &m)) {
			CHECK(false);
			return;
		}
		memset(&a, 0, sizeof(a));
		eh_wait_ms(&t, 200);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.nlp_enabled, 1);
		frames = lec.frames;

		tm_ans_send(&t, &w, &a, TM_ANS_PLAIN, 1200);
		CHECK(tm_tone_find(UDSP_TONE_DET_CED) != NULL);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_FAX);
		{
			const struct udsp_event *e =
				tm_lec_find(UDSP_LEC_REASON_LATCH);

			CHECK(e != NULL);
			if (e) {
				CHECK_UINT(e->u.lec.kind, UDSP_PASSTHROUGH_FAX);
				CHECK_UINT(e->u.lec.tone, UDSP_TONE_DET_CED);
				CHECK_UINT(e->u.lec.direction,
					   UDSP_TONE_DIR_REMOTE);
			}
		}
		/* G.168 7.1 forbids a full disable on a tone with no
		 * reversals: the canceller stays on the frame and only its
		 * processor goes off, so the line still answers and the frames
		 * it ran say the filter kept running. */
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.ec_active, 1);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_FAX);
		CHECK(lec.frames > frames + 10u);

		CASE("the fax policy outlasts a T.30 silence");
		eh_wait_ms(&t, 500);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_FAX);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.ec_active, 1);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_FAX);
		/* Past the seven second window it releases like any other. */
		eh_wait_ms(&t, 7000);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_OFF);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.nlp_enabled, 1);
		{
			const struct udsp_event *e =
				tm_lec_find(UDSP_LEC_REASON_RELEASE_QUIET);

			CHECK(e != NULL);
			if (e)
				CHECK_UINT(e->u.lec.kind, UDSP_PASSTHROUGH_OFF);
		}

		tm_call_down(&t, &w);
	}

	CASE("the force disable option takes a plain answer tone to a full disable");
	{
		if (!tm_disable_up(&t, &w, &ccfg, &m)) {
			CHECK(false);
			return;
		}
		ccfg.ec_tone_force_disable = true;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		memset(&a, 0, sizeof(a));

		tm_ans_send(&t, &w, &a, TM_ANS_PLAIN, 1200);
		CHECK(tm_tone_find(UDSP_TONE_DET_CED) != NULL);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_MODEM);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.ec_active, 0);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_MODEM);
		{
			const struct udsp_event *e =
				tm_lec_find(UDSP_LEC_REASON_LATCH);

			CHECK(e != NULL);
			if (e) {
				/* The row names the tone that arrived, not the
				 * policy the option asked for it. */
				CHECK_UINT(e->u.lec.kind, UDSP_PASSTHROUGH_MODEM);
				CHECK_UINT(e->u.lec.tone, UDSP_TONE_DET_CED);
				CHECK_UINT(e->u.lec.direction,
					   UDSP_TONE_DIR_REMOTE);
			}
		}

		tm_call_down(&t, &w);
	}

	CASE("the latch option holds the disabler to the end of the line's streams");
	{
		if (!tm_disable_up(&t, &w, &ccfg, &m)) {
			CHECK(false);
			return;
		}
		ccfg.ec_tone_latch = true;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		memset(&a, 0, sizeof(a));

		tm_ans_send(&t, &w, &a, TM_ANS_REVERSED, 2000);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_MODEM);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.ec_active, 0);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_MODEM);
		/* Eight times the silence window the default releases on. */
		eh_wait_ms(&t, 2000);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_MODEM);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.ec_active, 0);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_MODEM);

		/* The stop is the release this option leaves. */
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_OFF);
		tm_call_close(&t, &w);
	}

	CASE("a committed fax description wins over a modem latch");
	{
		if (!tm_disable_up(&t, &w, &ccfg, &m)) {
			CHECK(false);
			return;
		}
		/* The latch option holds the disabler still for the whole
		 * case, so what the reads below see is the precedence rule
		 * alone and not a release that ran between them. */
		ccfg.ec_tone_latch = true;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		memset(&a, 0, sizeof(a));

		tm_ans_send(&t, &w, &a, TM_ANS_REVERSED, 2000);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_MODEM);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.ec_active, 0);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_MODEM);

		m.passthrough = UDSP_PASSTHROUGH_FAX;
		tm_stream_config(&t, &w, &m);
		eh_wait_ms(&t, 100);
		/* Call control said fax, so the fax policy runs even though
		 * the latch the tone set is the stronger kind. */
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_MODEM);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.ec_active, 1);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_FAX);

		tm_call_down(&t, &w);
	}

	CASE("the canceller record carries the processor's own switch");
	{
		if (!tm_disable_up(&t, &w, &ccfg, &m)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 200);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.nlp_enabled, 1);

		m.passthrough = UDSP_PASSTHROUGH_FAX;
		tm_stream_config(&t, &w, &m);
		eh_wait_ms(&t, 100);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.ec_active, 1);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_FAX);

		m.passthrough = UDSP_PASSTHROUGH_OFF;
		tm_stream_config(&t, &w, &m);
		eh_wait_ms(&t, 100);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.nlp_enabled, 1);

		/* And the channel's own switch reads here too, which is what
		 * tells a configured processor apart from a policy. */
		ccfg.ec_nlp_enable = false;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		eh_wait_ms(&t, 100);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.nlp_enabled, 0);

		tm_call_down(&t, &w);
	}

	CASE("a V.21 preamble demotes a modem latch to the fax policy");
	{
		if (!tm_disable_up(&t, &w, &ccfg, &m)) {
			CHECK(false);
			return;
		}
		memset(&a, 0, sizeof(a));

		tm_ans_send(&t, &w, &a, TM_ANS_REVERSED, 2000);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_MODEM);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.ec_active, 0);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_MODEM);

		/* The preamble follows the tone with no gap, so nothing but
		 * the fax confirming itself moves the policy. */
		tm_ans_send(&t, &w, &a, TM_ANS_V21, 1200);
		CHECK(tm_tone_find(UDSP_TONE_DET_V21_PREAMBLE) != NULL);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_FAX);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.ec_active, 1);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_FAX);
		{
			const struct udsp_event *e =
				tm_lec_find(UDSP_LEC_REASON_DEMOTE);

			CHECK(e != NULL);
			if (e) {
				CHECK_UINT(e->u.lec.kind, UDSP_PASSTHROUGH_FAX);
				CHECK_UINT(e->u.lec.tone,
					   UDSP_TONE_DET_V21_PREAMBLE);
				CHECK_UINT(e->u.lec.direction,
					   UDSP_TONE_DIR_REMOTE);
			}
		}
		/* The reversals latched first, so the two rows tell the whole
		 * course of the policy apart. */
		{
			const struct udsp_event *e =
				tm_lec_find(UDSP_LEC_REASON_LATCH);

			CHECK(e != NULL);
			if (e)
				CHECK_UINT(e->u.lec.kind, UDSP_PASSTHROUGH_MODEM);
		}

		tm_call_down(&t, &w);
	}

	CASE("a V.21 preamble on its own latches nothing");
	{
		if (!tm_disable_up(&t, &w, &ccfg, &m)) {
			CHECK(false);
			return;
		}
		memset(&a, 0, sizeof(a));

		tm_ans_send(&t, &w, &a, TM_ANS_V21, 1200);
		CHECK(tm_tone_find(UDSP_TONE_DET_V21_PREAMBLE) != NULL);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_OFF);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.nlp_enabled, 1);
		/* A policy that never moved is a policy nobody reports. */
		CHECK_UINT(eh_ev_count(UDSP_EV_LEC_POLICY), 0);

		tm_call_down(&t, &w);
	}

	CASE("an answer tone from the phone side latches too");
	{
		if (!tm_disable_up(&t, &w, &ccfg, &m)) {
			CHECK(false);
			return;
		}
		tm_capture_tone(2100u);
		eh_wait_ms(&t, 1500);
		{
			const struct udsp_event *e =
				tm_tone_find(UDSP_TONE_DET_CED);

			CHECK(e != NULL);
			if (e)
				CHECK_UINT(e->u.tone.direction,
					   UDSP_TONE_DIR_LOCAL);
		}
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_FAX);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.ec_active, 1);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_FAX);
		{
			const struct udsp_event *e =
				tm_lec_find(UDSP_LEC_REASON_LATCH);

			CHECK(e != NULL);
			if (e) {
				CHECK_UINT(e->u.lec.kind, UDSP_PASSTHROUGH_FAX);
				CHECK_UINT(e->u.lec.tone, UDSP_TONE_DET_CED);
				/* The one leg whose row names the phone: the
				 * field tells the two feeds apart or it says
				 * nothing at all. */
				CHECK_UINT(e->u.lec.direction,
					   UDSP_TONE_DIR_LOCAL);
			}
		}

		/* The release reads the phone side as well as the wire. */
		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		eh_wait_ms(&t, 7500);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_OFF);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.nlp_enabled, 1);
		{
			const struct udsp_event *e =
				tm_lec_find(UDSP_LEC_REASON_RELEASE_QUIET);

			CHECK(e != NULL);
			if (e)
				CHECK_UINT(e->u.lec.direction, 0);
		}

		tm_call_down(&t, &w);
	}

	/*
	 * The freeze the latch carries. A latch is set on a tone alone, so the
	 * filter must not adapt against it: every frame it runs under one is a
	 * held frame. The reference and the echo stand for the whole case, so
	 * the adaptation the disarm gives back is real.
	 */
	CASE("a latched line holds the canceller's adaptation");
	{
		uint32_t frames = 0;
		uint32_t held = 0;

		if (!tm_disable_up(&t, &w, &ccfg, &m)) {
			CHECK(false);
			return;
		}
		/*
		 * The hybrid stands for the whole case, and the playout is the
		 * reference it returns. The echo has to be there before the
		 * tone: the canceller learns the hybrid's gain as a minimum
		 * statistic, and a stretch of loud reference against a silent
		 * microphone teaches it the floor, so the echo that arrives
		 * later reads as a near talker and holds the filter for its own
		 * reasons (dsp/lec.h echo_gain, probes).
		 */
		atomic_store(&tm_be.echo_delay, 240u);
		atomic_store(&tm_be.echo_gain, 512u);
		atomic_store(&tm_be.src, TM_SRC_ECHO);
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
		eh_wait_ms(&t, 1000);

		memset(&a, 0, sizeof(a));
		tm_ans_send(&t, &w, &a, TM_ANS_PLAIN, 1200);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_FAX);

		/* The tone stops and the line carries the playout again, so the
		 * local detector reports the end of the carrier it heard in the
		 * echo and only the latch is left to hold the filter. The echo
		 * also keeps the holding band busy, which is what stops the
		 * silence release. */
		eh_wait_ms(&t, 1000);
		CHECK(!t.s->chan[0].fax_active);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		frames = lec.frames;
		held = lec.hold_frames;
		eh_wait_ms(&t, 1000);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_FAX);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK(lec.frames > frames + 10u);
		CHECK_UINT(lec.hold_frames - held, lec.frames - frames);

		/* The disarm drops the latch, and the filter that was held for
		 * the tone adapts on the echo again. */
		frames = lec.frames;
		held = lec.hold_frames;
		ccfg.detect_mask = 0;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_OFF);
		eh_wait_ms(&t, 2000);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK(lec.frames > frames + 10u);
		CHECK(lec.hold_frames - held < lec.frames - frames);

		CHECK(udsp_tone_stop(t.s, 0) == UDSP_OK);
		tm_call_down(&t, &w);
	}

	/*
	 * The topology guard. A latch describes the streams the line had when
	 * the tone arrived, so a line that gained or lost a governing stream is
	 * a line the latch does not speak for: a leg held or stopped under a
	 * carrier must not keep the canceller off the leg that is still
	 * talking. The latch option stands for both cases below, so the clear
	 * they read can only be the guard's.
	 */
	CASE("a stream that joins the line drops the latch");
	{
		struct udsp_media voice;
		struct tm_wire w2;

		if (!tm_disable_up(&t, &w, &ccfg, &m) || !tm_wire_open(&w2, 0)) {
			CHECK(false);
			return;
		}
		ccfg.ec_tone_latch = true;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		memset(&a, 0, sizeof(a));

		tm_ans_send(&t, &w, &a, TM_ANS_REVERSED, 2000);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_MODEM);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.ec_active, 0);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_MODEM);

		tm_media(&voice, UDSP_SEL_PCMU, 0, 10);
		CHECK(udsp_stream_config(t.s, 0, 1, &voice, w2.rtp_local,
					 w2.rtcp_local) == UDSP_OK);
		CHECK(udsp_stream_start(t.s, 0, 1) == UDSP_OK);
		eh_wait_ms(&t, 100);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_OFF);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		{
			const struct udsp_event *e =
				tm_lec_find(UDSP_LEC_REASON_RELEASE_TOPOLOGY);

			CHECK(e != NULL);
			if (e) {
				CHECK_UINT(e->u.lec.kind, UDSP_PASSTHROUGH_OFF);
				CHECK_UINT(e->u.lec.tone, 0);
				CHECK_UINT(e->u.lec.direction, 0);
			}
		}
		/* The silence release is off under this option, so the guard is
		 * the only release the row can have come from. */
		CHECK(tm_lec_find(UDSP_LEC_REASON_RELEASE_QUIET) == NULL);

		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_stream_stop(t.s, 0, 1) == UDSP_OK);
		tm_wire_close(&w);
		tm_call_close(&t, &w2);
	}

	CASE("a leg that goes on hold drops the latch");
	{
		struct udsp_media voice;
		struct tm_wire w2;

		if (!tm_disable_up(&t, &w, &ccfg, &m) || !tm_wire_open(&w2, 0)) {
			CHECK(false);
			return;
		}
		ccfg.ec_tone_latch = true;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&voice, UDSP_SEL_PCMU, 0, 10);
		CHECK(udsp_stream_config(t.s, 0, 1, &voice, w2.rtp_local,
					 w2.rtcp_local) == UDSP_OK);
		CHECK(udsp_stream_start(t.s, 0, 1) == UDSP_OK);
		memset(&a, 0, sizeof(a));

		/* Both legs govern the line while the tone arrives, so the
		 * count the latch records is two. */
		tm_ans_send(&t, &w, &a, TM_ANS_REVERSED, 2000);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_MODEM);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.ec_active, 0);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_MODEM);

		m.tx_silence = true;
		tm_stream_config(&t, &w, &m);
		eh_wait_ms(&t, 100);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_OFF);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		{
			const struct udsp_event *e =
				tm_lec_find(UDSP_LEC_REASON_RELEASE_TOPOLOGY);

			CHECK(e != NULL);
			if (e) {
				CHECK_UINT(e->u.lec.kind, UDSP_PASSTHROUGH_OFF);
				CHECK_UINT(e->u.lec.tone, 0);
				CHECK_UINT(e->u.lec.direction, 0);
			}
		}
		CHECK(tm_lec_find(UDSP_LEC_REASON_RELEASE_QUIET) == NULL);

		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_stream_stop(t.s, 0, 1) == UDSP_OK);
		tm_wire_close(&w);
		tm_call_close(&t, &w2);
	}

	CASE("a reconfiguration of a leg keeps the latch");
	{
		struct udsp_media voice;
		struct tm_wire w2;

		if (!tm_disable_up(&t, &w, &ccfg, &m) || !tm_wire_open(&w2, 0)) {
			CHECK(false);
			return;
		}
		ccfg.ec_tone_latch = true;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&voice, UDSP_SEL_PCMU, 0, 10);
		CHECK(udsp_stream_config(t.s, 0, 1, &voice, w2.rtp_local,
					 w2.rtcp_local) == UDSP_OK);
		CHECK(udsp_stream_start(t.s, 0, 1) == UDSP_OK);
		memset(&a, 0, sizeof(a));

		tm_ans_send(&t, &w, &a, TM_ANS_REVERSED, 2000);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_MODEM);

		/* The reconfiguration quiesces the voice leg for a tick or
		 * two and brings the same topology back, so the guard must
		 * read it as no move at all: a mid-call jitter update must
		 * not put the canceller back on a modem. */
		voice.jb_init_ms = 40;
		CHECK(udsp_stream_config(t.s, 0, 1, &voice, w2.rtp_local,
					 w2.rtcp_local) == UDSP_OK);
		eh_wait_ms(&t, 200);
		CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_MODEM);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.ec_active, 0);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_MODEM);
		/* The latch stood, so the one row of the case is the latch
		 * itself: a guard that fell to the quiesce would say so here. */
		CHECK_UINT(eh_ev_count(UDSP_EV_LEC_POLICY), 1);

		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_stream_stop(t.s, 0, 1) == UDSP_OK);
		tm_wire_close(&w);
		tm_call_close(&t, &w2);
	}
}

static void case_fax_latch_clear(void)
{
	struct udsp_chan_cfg ccfg;
	struct udsp_lec_stats lec;
	const struct udsp_event *e;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	struct udsp_job job = { 0 };
	int32_t status = -1;

	CASE("a caller clear releases a standing tone latch");
	if (!tm_disable_up(&t, &w, &ccfg, &m)) {
		CHECK(false);
		return;
	}
	tm_capture_tone(2100u);
	eh_wait_ms(&t, 800);
	CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_FAX);
	CHECK_UINT(eh_ev_count(UDSP_EV_LEC_POLICY), 1);
	CHECK(udsp_line_lec_latch_clear(t.s, 0) == UDSP_OK);
	eh_wait_ms(&t, 100);
	CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_OFF);
	/* the tone still sounds on the phone feed, so the carrier freeze
	 * stands until the detector's own end row */
	CHECK(t.s->chan[0].fax_active);
	CHECK_UINT(t.s->chan[0].fax_quiet_frames, 0);
	CHECK_UINT(t.s->chan[0].fax_topo_frames, 0);
	CHECK_UINT(t.s->chan[0].fax_latch_streams, 0);
	CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
	CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_OFF);
	CHECK_UINT(lec.nlp_enabled, 1);
	CHECK_UINT(eh_ev_count(UDSP_EV_LEC_POLICY), 2);
	e = tm_lec_find(UDSP_LEC_REASON_RELEASE_CALLER);
	CHECK(e != NULL);
	if (e) {
		CHECK_UINT(e->line, 0);
		CHECK_UINT(e->u.lec.kind, UDSP_PASSTHROUGH_OFF);
		CHECK_UINT(e->u.lec.tone, 0);
		CHECK_UINT(e->u.lec.direction, 0);
	}

	CASE("tone rows distinguish the start from the end");
	CHECK_UINT(eh_ev_count(UDSP_EV_TONE), 1);
	e = tm_tone_find(UDSP_TONE_DET_CED);
	CHECK(e != NULL);
	if (e)
		CHECK_UINT(e->u.tone.end, 0);
	atomic_store(&tm_be.src, TM_SRC_SILENCE);
	eh_wait_ms(&t, 100);
	CHECK_UINT(eh_ev_count(UDSP_EV_TONE), 2);
	for (unsigned i = 0, tones = 0; i < eh_ev_len; i++) {
		if (eh_ev[i].type != UDSP_EV_TONE)
			continue;
		CHECK_UINT(eh_ev[i].u.tone.code, UDSP_TONE_DET_CED);
		CHECK_UINT(eh_ev[i].u.tone.direction, UDSP_TONE_DIR_LOCAL);
		CHECK_UINT(eh_ev[i].u.tone.end, tones);
		tones++;
	}

	CASE("a caller clear with no latch returns OK and posts nothing");
	eh_ev_reset();
	CHECK(udsp_line_lec_latch_clear(t.s, 0) == UDSP_OK);
	CHECK(udsp_line_lec_latch_clear(t.s, 0) == UDSP_OK);
	eh_wait_ms(&t, 7500);
	CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_OFF);
	CHECK_UINT(eh_ev_count(UDSP_EV_LEC_POLICY), 0);

	CASE("the next answer tone latches after a caller clear");
	atomic_store(&tm_be.src, TM_SRC_TONE);
	eh_wait_ms(&t, 800);
	CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_FAX);
	CHECK_UINT(eh_ev_count(UDSP_EV_TONE), 1);
	e = tm_tone_find(UDSP_TONE_DET_CED);
	CHECK(e != NULL);
	if (e)
		CHECK_UINT(e->u.tone.end, 0);
	CHECK_UINT(eh_ev_count(UDSP_EV_LEC_POLICY), 1);
	e = tm_lec_find(UDSP_LEC_REASON_LATCH);
	CHECK(e != NULL);
	if (e) {
		CHECK_UINT(e->u.lec.kind, UDSP_PASSTHROUGH_FAX);
		CHECK_UINT(e->u.lec.tone, UDSP_TONE_DET_CED);
		CHECK_UINT(e->u.lec.direction, UDSP_TONE_DIR_LOCAL);
	}

	CASE("a caller clear refuses an invalid line");
	CHECK(udsp_line_lec_latch_clear(t.s, t.s->cap.lines) ==
	      UDSP_UNSUPPORTED);
	CHECK(udsp_line_lec_latch_clear(t.s, 256) == UDSP_UNSUPPORTED);
	CHECK(udsp_line_lec_latch_clear(NULL, 0) == UDSP_UNSUPPORTED);
	CHECK_UINT(tm_latch_kind(&t), UDSP_PASSTHROUGH_FAX);

	CASE("a caller clear refuses a stopped engine");
	CHECK(udsp_stop(t.s, &job) == UDSP_OK);
	CHECK(eh_job_wait(&t, job.token, 5000, &status));
	CHECK_UINT(status, UDSP_OK);
	t.stopped = true;
	CHECK(udsp_line_lec_latch_clear(t.s, 0) == UDSP_TRANSIENT);
	tm_call_close(&t, &w);
}

/* --- the detector feed points at both line rates ------------------------- */

/*
 * The echo of a digit that the line played. The canceller adapts on the digit
 * itself while detection is disarmed. Its filter therefore cancels exactly the
 * two tones that the detector would have to find. The detector reads the feed
 * before the canceller (chan.c chan_ul), so a converged filter alone does not
 * keep the echo out of the detector. The hold does (chan.h
 * UDSP_DL_DIGIT_HOLD_TICKS). The canceller measurement is the precondition,
 * and the detector count is the rule.
 */
static void tm_lec_digit_leg(unsigned rate)
{
	struct udsp_lec_stats ls;
	struct udsp_chan_cfg ccfg;
	struct udsp_media m;
	struct tm_wire w;
	unsigned line_rate = rate ? rate : UDSP_SAMPLE_RATE;
	struct udsp_ec *ec;
	struct eh t;
	char name[96];

	snprintf(name, sizeof(name),
		 "a converged canceller still holds a played digit at %u kHz",
		 line_rate / 1000u);
	CASE(name);
	{
		if (!tm_call_up_rate(&t, "USA", rate, &w))
			return;
		CHECK(eh_hook_seize(&t));
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.ec_enable = true;
		ccfg.ec_tail_ms = 32;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		/* The backend grants one period above the prefill and reports
		 * that queue from delay(), so the loop is three periods. */
		atomic_store(&tm_be.echo_delay, 3u * (line_rate / 100u));
		atomic_store(&tm_be.echo_gain, 1024u);
		atomic_store(&tm_be.src, TM_SRC_ECHO);

		/* Detection is disarmed, so the adaptation phase reports no
		 * digit of its own. */
		CHECK(udsp_digit_play(t.s, 0, '5', 8000) == UDSP_OK);
		eh_wait_ms(&t, 8200);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 0);

		ccfg.detect_mask = UDSP_DET_DTMF;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(udsp_digit_play(t.s, 0, '5', 300) == UDSP_OK);
		eh_wait_ms(&t, 800);

		/* Turning the canceller off is a SYNC-TICK op, and the record
		 * that eh_lec_state_wait takes describes the last frame the
		 * filter ran (the case_lec pattern). */
		ccfg.ec_enable = false;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		ec = atomic_load(&t.s->chan[0].ec);
		CHECK(ec != NULL);
		if (ec) {
			CHECK(eh_lec_state_wait(&t, &ls, false, false));
			/* The precondition: the digit really is canceled. */
			CHECK(ls.erle_mdb >= 20000);
			CHECK_UINT(ls.taps, 32u * (line_rate / 1000u));
		}
		/* The digit played toward the line and its echo came back, so
		 * the zero is the hold and not a feed that carried nothing. */
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 0);
		tm_call_down(&t, &w);
	}
}

static void case_resample_detect(void)
{
	struct udsp_chan_cfg ccfg;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	double phase = 0.0;

	tm_lec_digit_leg(0);
	tm_lec_digit_leg(UDSP_SAMPLE_RATE_WB);

	/*
	 * The rs_dl feed (chan.h). rs_rx makes the summed downlink a true
	 * 16 kHz signal, and the per-line decimator hands the 8 kHz detector
	 * the answer tone the far end actually sent. Without rs_rx, the
	 * decoded downlink of a 16 kHz line is 80 real samples and 80 zeros per
	 * tick, and the remote detector sees a signal at half the rate.
	 */
	CASE("a fax answer tone from the wire is reported on a 16 kHz line");
	{
		if (!tm_up_rate(&t, "USA", UDSP_SAMPLE_RATE_WB) ||
		    !tm_wire_open(&w, 0)) {
			CHECK(false);
			return;
		}
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_MODEM;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);

		/* CED is 2100 Hz for at least 500 ms (dsp/fax_det.c). */
		for (unsigned burst = 0; burst < 8u; burst++) {
			for (unsigned i = 0; i < 30u; i++)
				tm_tone_send(&w, 0, 10, 2100.0, 8000.0, &phase);
			eh_wait_ms(&t, 250);
		}
		eh_wait_ms(&t, 300);
		{
			const struct udsp_event *e =
				eh_ev_find(UDSP_EV_TONE, 0);

			CHECK(e != NULL);
			if (e) {
				CHECK_UINT(e->u.tone.code, UDSP_TONE_DET_CED);
				CHECK_UINT(e->u.tone.direction,
					   UDSP_TONE_DIR_REMOTE);
				CHECK_UINT(e->line, 0);
			}
		}
		tm_call_down(&t, &w);
	}
}

/* --- cases: the conference mixer ----------------------------------------- */

/*
 * Three tones, one per party. All three are exact multiples of the 50 Hz bin
 * of a 160 sample (ptime 20) payload, so a Goertzel at one of them sees
 * nothing of the other two: the leakage floor of the oracle is the companding
 * noise, not the spacing of the tones.
 */
#define TM_CONF_UL_HZ	1000.0
#define TM_CONF_A_HZ	1500.0
#define TM_CONF_B_HZ	1900.0
#define TM_CONF_AMP	8000.0

struct tm_conf_seen {
	unsigned packets;
	double ul;		/* the line's own tone */
	double own;		/* the tone this far end sends */
	double peer;		/* the tone the other far end sends */
};

static void tm_wire_drain(int fd)
{
	uint8_t buf[TM_PKT_MAX];

	while (tm_recv(fd, buf, sizeof(buf)) > 0)
		continue;
}

/* Paces both far ends at the engine's clock, one ptime 20 packet each. */
static void tm_conf_run(struct eh *t, struct tm_wire *a, struct tm_wire *b,
			double *pa, double *pb, uint32_t ms)
{
	for (uint32_t i = 0; i < ms; i += 20u) {
		if (a)
			tm_tone_send(a, 0, 20, TM_CONF_A_HZ, TM_CONF_AMP, pa);
		if (b)
			tm_tone_send(b, 0, 20, TM_CONF_B_HZ, TM_CONF_AMP, pb);
		eh_wait_ms(t, 20);
	}
}

static bool tm_gate_conf_run(struct eh_gate *gate, struct eh *t,
			     struct tm_wire *a, struct tm_wire *b,
			     double *pa, double *pb, unsigned frames)
{
	const struct udsp_media m = {
		.codec = UDSP_SEL_PCMU, .codec_pt = 0, .ptime_ms = 20,
	};
	struct udsp_stream *sa = stream_at(t->s, 0, 0);
	struct udsp_stream *sb = stream_at(t->s, 0, 1);

	CHECK(sa != NULL && sb != NULL);
	if (!sa || !sb)
		return false;
	for (unsigned i = 0; i < frames; i += 2u) {
		if (!tm_gate_tone_publish(sa, a, 1, &m, TM_CONF_A_HZ,
					  TM_CONF_AMP, pa) ||
		    !tm_gate_tone_publish(sb, b, 1, &m, TM_CONF_B_HZ,
					  TM_CONF_AMP, pb) ||
		    !eh_gate_step(gate, 2))
			return false;
	}

	return true;
}

/* The mean power of every full media packet one far end received, at the three
 * tones of the conference. */
static void tm_conf_seen_get(int fd, double own_hz, double peer_hz,
			     struct tm_conf_seen *out)
{
	uint8_t buf[TM_PKT_MAX];
	struct tm_pkt p;
	ssize_t n;

	memset(out, 0, sizeof(*out));
	while ((n = tm_recv(fd, buf, sizeof(buf))) > 0) {
		if (!tm_rtp_parse(buf, (size_t)n, &p) || p.pt != 0u ||
		    p.len != 160u)
			continue;
		out->packets++;
		out->ul += tm_payload_power(p.payload, p.len, false,
					    TM_CONF_UL_HZ);
		out->own += tm_payload_power(p.payload, p.len, false, own_hz);
		out->peer += tm_payload_power(p.payload, p.len, false, peer_hz);
	}
	if (!out->packets)
		return;
	out->ul /= out->packets;
	out->own /= out->packets;
	out->peer /= out->packets;
}

/* Two legs on line 0, each with its own far end, plus a third tone on the line
 * itself. */
static bool tm_conf_up(struct eh *t, struct tm_wire *a, struct tm_wire *b)
{
	struct udsp_media m;

	if (!tm_up(t, "USA") || !tm_wire_open(a, 1 << 20) ||
	    !tm_wire_open(b, 1 << 20))
		return false;

	tm_media(&m, UDSP_SEL_PCMU, 0, 20);
	if (udsp_stream_config(t->s, 0, 0, &m, a->rtp_local, a->rtcp_local) !=
	    UDSP_OK)
		return false;
	if (udsp_stream_config(t->s, 0, 1, &m, b->rtp_local, b->rtcp_local) !=
	    UDSP_OK)
		return false;
	if (udsp_stream_start(t->s, 0, 0) != UDSP_OK)
		return false;
	if (udsp_stream_start(t->s, 0, 1) != UDSP_OK)
		return false;
	atomic_store(&tm_be.tone_hz, (uint32_t)TM_CONF_UL_HZ);
	atomic_store(&tm_be.tone_amp, (uint32_t)TM_CONF_AMP);
	atomic_store(&tm_be.src, TM_SRC_TONE);

	return true;
}

static void case_conf(void)
{
	struct tm_wire a, b;
	struct udsp_media m;
	struct eh t;

	/*
	 * Each leg must hear the line and the other leg and never its own
	 * return: a mixer that forgets the subtraction sends every far end its
	 * own audio back, which is an echo, not a conference.
	 */
	CASE("a three way conference gives each leg the other leg and never itself");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct tm_conf_seen sa, sb;
		double pa = 0.0, pb = 0.0;

		if (!tm_conf_up(&t, &a, &b)) {
			CHECK(false);
			return;
		}
		CHECK(udsp_conf(t.s, 0, UDSP_CONF_LOCAL_MIX) == UDSP_OK);

		if (!eh_gate_park(&gate) ||
		    !tm_gate_conf_run(&gate, &t, &a, &b, &pa, &pb, 40))
			goto conf_out;
		tm_wire_drain(a.rtp_peer);
		tm_wire_drain(b.rtp_peer);
		eh_rec_mark();
		if (!tm_gate_conf_run(&gate, &t, &a, &b, &pa, &pb, 40))
			goto conf_out;
		CHECK_UINT(eh_rec_have(), 3200);
		if (eh_rec_have() != 3200u)
			goto conf_out;
		tm_conf_seen_get(a.rtp_peer, TM_CONF_A_HZ, TM_CONF_B_HZ, &sa);
		tm_conf_seen_get(b.rtp_peer, TM_CONF_B_HZ, TM_CONF_A_HZ, &sb);

		CHECK(sa.packets > 5u);
		CHECK(sb.packets > 5u);
		CHECK(sa.ul > 1e9);
		CHECK(sa.peer > 1e9);
		CHECK(sa.own * 100.0 < sa.peer);
		CHECK(sb.ul > 1e9);
		CHECK(sb.peer > 1e9);
		CHECK(sb.own * 100.0 < sb.peer);

		/* The phone still hears every leg: the local playout is the sum
		 * of all the streams, exactly as it is with no conference. */
		CHECK(eh_power(0, 800, TM_CONF_A_HZ) > 1e9);
		CHECK(eh_power(0, 800, TM_CONF_B_HZ) > 1e9);

	conf_out:
		eh_gate_disarm(&gate);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_stream_stop(t.s, 0, 1) == UDSP_OK);
		tm_wire_close(&a);
		tm_call_close(&t, &b);
		eh_gate_close(&gate);
	}

	CASE("conference off puts the two party path back");
	{
		struct tm_conf_seen sa, sb;
		double pa = 0.0, pb = 0.0;

		if (!tm_conf_up(&t, &a, &b)) {
			CHECK(false);
			return;
		}
		CHECK(udsp_conf(t.s, 0, UDSP_CONF_LOCAL_MIX) == UDSP_OK);
		tm_conf_run(&t, &a, &b, &pa, &pb, 400);
		CHECK(udsp_conf(t.s, 0, UDSP_CONF_OFF) == UDSP_OK);

		tm_conf_run(&t, &a, &b, &pa, &pb, 200);
		tm_wire_drain(a.rtp_peer);
		tm_wire_drain(b.rtp_peer);
		tm_conf_run(&t, &a, &b, &pa, &pb, 400);
		tm_conf_seen_get(a.rtp_peer, TM_CONF_A_HZ, TM_CONF_B_HZ, &sa);
		tm_conf_seen_get(b.rtp_peer, TM_CONF_B_HZ, TM_CONF_A_HZ, &sb);

		CHECK(sa.packets > 5u);
		CHECK(sb.packets > 5u);
		/* The line alone reaches each far end again. */
		CHECK(sa.ul > 1e9);
		CHECK(sa.peer * 100.0 < sa.ul);
		CHECK(sa.own * 100.0 < sa.ul);
		CHECK(sb.ul > 1e9);
		CHECK(sb.peer * 100.0 < sb.ul);
		CHECK(sb.own * 100.0 < sb.ul);

		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_stream_stop(t.s, 0, 1) == UDSP_OK);
		tm_wire_close(&a);
		tm_call_close(&t, &b);
	}

	/* With one leg the bus holds that leg alone, so the mix-minus cancels
	 * to the line frame and the far end must hear what it hears with no
	 * conference. */
	CASE("a single leg conference carries the line and nothing else");
	{
		struct tm_conf_seen off, on;
		double pa = 0.0;

		if (!tm_call_up(&t, "USA", &a, 1 << 20))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &a, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		atomic_store(&tm_be.tone_hz, (uint32_t)TM_CONF_UL_HZ);
		atomic_store(&tm_be.tone_amp, (uint32_t)TM_CONF_AMP);
		atomic_store(&tm_be.src, TM_SRC_TONE);

		tm_conf_run(&t, &a, NULL, &pa, NULL, 400);
		tm_wire_drain(a.rtp_peer);
		tm_conf_run(&t, &a, NULL, &pa, NULL, 300);
		tm_conf_seen_get(a.rtp_peer, TM_CONF_A_HZ, TM_CONF_B_HZ, &off);

		CHECK(udsp_conf(t.s, 0, UDSP_CONF_LOCAL_MIX) == UDSP_OK);
		tm_conf_run(&t, &a, NULL, &pa, NULL, 200);
		tm_wire_drain(a.rtp_peer);
		tm_conf_run(&t, &a, NULL, &pa, NULL, 300);
		tm_conf_seen_get(a.rtp_peer, TM_CONF_A_HZ, TM_CONF_B_HZ, &on);

		CHECK(off.packets > 5u);
		CHECK(on.packets > 5u);
		CHECK(on.ul > 1e9);
		/* Same line tone, same absence of its own return. */
		CHECK(on.ul > off.ul * 0.9 && on.ul < off.ul * 1.1);
		CHECK(on.own * 100.0 < on.ul);

		tm_call_down(&t, &a);
	}

	CASE("a conference on a line carrying no stream is refused");
	{
		if (!tm_call_up(&t, "USA", &a, 0))
			return;
		CHECK(udsp_conf(t.s, 0, UDSP_CONF_LOCAL_MIX) == UDSP_UNSUPPORTED);
		CHECK(udsp_conf(t.s, 0, UDSP_CONF_OFF) == UDSP_UNSUPPORTED);

		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &a, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_conf(t.s, 0, UDSP_CONF_LOCAL_MIX) == UDSP_OK);
		CHECK_UINT(t.s->chan[0].conf_mode, UDSP_CONF_LOCAL_MIX);

		/* The arm dies with the last stream of the line. */
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		CHECK_UINT(t.s->chan[0].conf_mode, UDSP_CONF_OFF);
		CHECK(udsp_conf(t.s, 0, UDSP_CONF_OFF) == UDSP_UNSUPPORTED);
		tm_call_close(&t, &a);
	}
}

/* --- cases: the delayed loop --------------------------------------------- */

static void case_loop(void)
{
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;

	CASE("a loop and a stream of one line exclude each other");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0, 500) ==
		      UDSP_TRANSIENT);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0, 500) ==
		      UDSP_OK);
		/* And the other way: the stream waits for the disarm. */
		CHECK(udsp_stream_config(t.s, 0, 0, &m, w.rtp_local,
					 w.rtcp_local) == UDSP_TRANSIENT);
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_OFF, 0, 0) == UDSP_OK);
		tm_stream_config(&t, &w, &m);
		tm_call_down(&t, &w);
	}

	/* The ring is taken at open and the tick only copies frames in and out
	 * of it, so an armed loop costs the tick no allocator call, in either
	 * mode. */
	CASE("an armed loop allocates nothing on the tick");
	{
		if (!tm_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK(tm_alloc_wrapped());
		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_SELF, 0, 500) ==
		      UDSP_OK);
		CHECK(udsp_line_loop(t.s, 1, UDSP_LOOP_PEER, 0, 500) ==
		      UDSP_OK);
		eh_wait_ms(&t, 600);

		tm_alloc_window(true);
		eh_wait_ms(&t, 600);
		tm_alloc_window(false);
		CHECK_UINT(tm_alloc_count(), 0);

		CHECK(udsp_line_loop(t.s, 0, UDSP_LOOP_OFF, 0, 0) == UDSP_OK);
		CHECK(udsp_line_loop(t.s, 1, UDSP_LOOP_OFF, 0, 0) == UDSP_OK);
		tm_down(&t);
	}
}

/* --- cases: the fax profile ---------------------------------------------- */

static bool tm_gate_call_run(struct eh_gate *gate, struct eh *t,
			     void *(*call)(void *), void *arg)
{
	uint64_t end = eh_mono_us() + EH_GATE_WAIT_US;
	pthread_t thread;
	int rc = pthread_create(&thread, NULL, call, arg);

	CHECK_UINT(rc, 0);
	if (rc)
		return false;
	while ((rc = pthread_tryjoin_np(thread, NULL)) == EBUSY &&
	       eh_mono_us() < end) {
		/* A tick before DEACTIVATE is queued could send an old
		 * frame. */
		if (atomic_load_explicit(&t->s->ctl_ring.head,
					memory_order_acquire) ==
		    atomic_load_explicit(&t->s->ctl_ring.tail,
					memory_order_acquire)) {
			sched_yield();
			continue;
		}
		if (!eh_gate_step(gate, 1))
			break;
	}
	CHECK_UINT(rc, 0);
	/* An unjoined worker still owns these stack addresses. */
	if (rc)
		exit(tu_summary("media"));

	return true;
}

struct tm_gate_config {
	struct eh *t;
	const struct udsp_media *media;
	const struct tm_wire *wire;
	enum udsp_status status;
};

static void *tm_gate_config_call(void *arg)
{
	struct tm_gate_config *call = arg;

	call->status = udsp_stream_config(call->t->s, 0, 0, call->media,
					  call->wire->rtp_local,
					  call->wire->rtcp_local);

	return NULL;
}

static bool tm_gate_config_run(struct eh_gate *gate, struct eh *t,
			       const struct udsp_media *media,
			       const struct tm_wire *wire)
{
	struct tm_gate_config call = { .t = t, .media = media, .wire = wire };

	if (!tm_gate_call_run(gate, t, tm_gate_config_call, &call))
		return false;
	CHECK(call.status == UDSP_OK);

	return call.status == UDSP_OK;
}

struct tm_gate_fax {
	struct eh *t;
	enum udsp_fax_mode mode;
	uint8_t codec;
	uint8_t codec_pt;
	enum udsp_status status;
};

static void *tm_gate_fax_call(void *arg)
{
	struct tm_gate_fax *call = arg;

	call->status = udsp_fax_profile(call->t->s, 0, 0, call->mode,
					call->codec, call->codec_pt);

	return NULL;
}

static bool tm_gate_fax_run(struct eh_gate *gate, struct eh *t,
			    enum udsp_fax_mode mode, uint8_t codec,
			    uint8_t codec_pt)
{
	struct tm_gate_fax call = {
		.t = t, .mode = mode, .codec = codec, .codec_pt = codec_pt,
	};

	if (!tm_gate_call_run(gate, t, tm_gate_fax_call, &call))
		return false;
	CHECK(call.status == UDSP_OK);

	return call.status == UDSP_OK;
}

static void tm_fax_packets_count(int fd, unsigned *media, unsigned *cn)
{
	uint8_t buf[TM_PKT_MAX];
	struct tm_pkt p;
	ssize_t n;

	*media = 0;
	*cn = 0;
	while ((n = tm_recv(fd, buf, sizeof(buf))) > 0) {
		if (!tm_rtp_parse(buf, (size_t)n, &p))
			continue;
		*media += p.pt == 0u;
		*cn += p.pt == 13u;
	}
}

static double tm_fax_loss_energy(bool passthrough)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	struct tm_wire w = {
		.rtp_local = -1, .rtp_peer = -1,
		.rtcp_local = -1, .rtcp_peer = -1,
	};
	struct udsp_jb_stats js;
	struct udsp_stream *st;
	struct udsp_media m;
	struct eh t;
	uint64_t plc_before;
	double phase = 0.0;
	double energy = -1.0;

	if (!tm_up(&t, "USA") || !tm_wire_open(&w, 0)) {
		CHECK(false);
		goto loss_down;
	}
	tm_media(&m, UDSP_SEL_PCMU, 0, 10);
	tm_stream_config(&t, &w, &m);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	if (passthrough)
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_OK);
	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL);
	if (!st || !eh_gate_park(&gate))
		goto loss_out;
	if (!tm_gate_tone_run(&gate, st, &w, 4, &phase) ||
	    !tm_gate_plc_wait(&gate, st, &plc_before) ||
	    !eh_gate_step(&gate, 4))
		goto loss_out;
	udsp_jb_stats(&st->jb, &js);
	CHECK_UINT(js.plc - plc_before, 5);
	CHECK_UINT(eh_rec_have(), 400);
	if (eh_rec_have() != 400u)
		goto loss_out;
	energy = eh_energy(0, 400);
loss_out:
	eh_gate_disarm(&gate);
	CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
loss_down:
	tm_call_close(&t, &w);
	eh_gate_close(&gate);

	return energy;
}

static double tm_dial_power_window(struct eh *t, int fd, unsigned *seen)
{
	uint8_t buf[TM_PKT_MAX];
	double sum = 0.0;
	struct tm_pkt p;
	ssize_t n;

	*seen = 0;
	tm_wire_drain(fd);
	eh_wait_ms(t, 400);
	while ((n = tm_recv(fd, buf, sizeof(buf))) > 0) {
		if (!tm_rtp_parse(buf, (size_t)n, &p) || p.pt != 0u)
			continue;
		sum += tm_payload_power(p.payload, p.len, false, 350.0);
		(*seen)++;
	}

	return *seen ? sum / *seen : 0.0;
}

static void case_fax_profile(void)
{
	struct udsp_chan_cfg ccfg;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;

	CASE("a fax passthrough keeps sending media through a silence");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct udsp_stream *st;
		unsigned media_pkts = 0;
		unsigned cn_pkts = 0;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.vad = true;
		m.cng = true;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st || !eh_gate_park(&gate))
			goto silence_out;
		tm_wire_drain(w.rtp_peer);
		if (!tm_gate_fax_run(&gate, &t, UDSP_FAX_PASSTHROUGH,
				     UDSP_SEL_PCMU, 0) ||
		    !eh_gate_step(&gate, 30))
			goto silence_out;
		tm_fax_packets_count(w.rtp_peer, &media_pkts, &cn_pkts);
		CHECK(media_pkts > 20u);
		CHECK_UINT(cn_pkts, 0);
		CHECK(!st->media.vad);
		CHECK(!st->media.cng);

		/* A parked drain excludes old-profile packets and preserves
		 * the first CN, which repeats only after 1000 ms. */
		media_pkts = 0;
		cn_pkts = 0;
		tm_wire_drain(w.rtp_peer);
		if (!tm_gate_fax_run(&gate, &t, UDSP_FAX_OFF, 0, 0))
			goto silence_out;
		CHECK(st->media.vad);
		CHECK(st->media.cng);
		if (!eh_gate_step(&gate, 30))
			goto silence_out;
		tm_fax_packets_count(w.rtp_peer, &media_pkts, &cn_pkts);
		CHECK_UINT(media_pkts, 0);
		CHECK(cn_pkts > 0u);

silence_out:
		eh_gate_disarm(&gate);
		tm_call_down(&t, &w);
		eh_gate_close(&gate);
	}

	CASE("a fax passthrough forces the jitter buffer to fixed");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		CHECK_UINT(stream_at(t.s, 0, 0)->jb.mode, UDSP_JB_ADAPTIVE);

		/* The ack of a SYNC-TICK op is the engine's own release, so
		 * these reads are ordered against the tick that wrote them. */
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_OK);
		CHECK_UINT(stream_at(t.s, 0, 0)->jb.mode, UDSP_JB_FIXED);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.jb_mode, UDSP_JB_FIXED);

		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_OFF, 0, 0) == UDSP_OK);
		CHECK_UINT(stream_at(t.s, 0, 0)->jb.mode, UDSP_JB_ADAPTIVE);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.jb_mode, UDSP_JB_ADAPTIVE);

		tm_call_down(&t, &w);
	}

	CASE("a fax passthrough clamps the stream to the passed codec");
	{
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p;
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		atomic_store(&tm_be.tone_hz, (uint32_t)TM_CONF_UL_HZ);
		atomic_store(&tm_be.tone_amp, (uint32_t)TM_CONF_AMP);
		atomic_store(&tm_be.src, TM_SRC_TONE);

		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMA, 8) == UDSP_OK);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.codec, UDSP_SEL_PCMA);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.codec_pt, 8);
		eh_wait_ms(&t, 150);
		tm_wire_drain(w.rtp_peer);
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300);
		CHECK(n > 0);
		if (n > 0 && tm_rtp_parse(buf, (size_t)n, &p)) {
			CHECK_UINT(p.pt, 8);
			CHECK(tm_payload_power(p.payload, p.len, true,
					       TM_CONF_UL_HZ) > 1e9);
		}

		/* An unknown codec displaces nothing. */
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_NONE, 0) == UDSP_UNSUPPORTED);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.codec, UDSP_SEL_PCMA);

		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_OFF, 0, 0) == UDSP_OK);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.codec, UDSP_SEL_PCMU);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.codec_pt, 0);
		eh_wait_ms(&t, 150);
		tm_wire_drain(w.rtp_peer);
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300);
		CHECK(n > 0);
		if (n > 0 && tm_rtp_parse(buf, (size_t)n, &p)) {
			CHECK_UINT(p.pt, 0);
			CHECK(tm_payload_power(p.payload, p.len, false,
					       TM_CONF_UL_HZ) > 1e9);
		}

		tm_call_down(&t, &w);
	}

	/*
	 * The profile forces a bypass, not the freeze of ec.h: a frozen filter
	 * keeps subtracting its converged estimate from the modem carrier. The
	 * canceller is armed before the echo starts and never runs a sample
	 * while the profile holds, so its filter is provably untouched.
	 */
	CASE("a fax passthrough bypasses the echo canceller");
	{
		struct udsp_lec_stats ls;
		double bypassed = 0.0;
		double cancelled = 0.0;
		uint32_t armed_frames;
		struct udsp_ec *ec;
		unsigned seen = 0;
		uint32_t end;
		uint64_t stop;

		memset(&ls, 0, sizeof(ls));

		if (!tm_call_up(&t, "USA", &w, 1 << 20))
			return;
		CHECK(eh_hook_seize(&t));
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.ec_enable = true;
		ccfg.ec_tail_ms = 32;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		ec = atomic_load(&t.s->chan[0].ec);
		CHECK(ec != NULL);

		/* Armed while the line is silent, so nothing could have adapted
		 * before the bypass takes hold. */
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_OK);
		/* The bypass holds from the tick of the profile ack on, so the
		 * filter runs no frame until UDSP_FAX_OFF. The record that
		 * eh_lec_state_wait takes is the baseline the echo below must
		 * not move. */
		if (ec)
			CHECK(eh_lec_state_wait(&t, &ls, false, false));
		armed_frames = ls.frames;
		atomic_store(&tm_be.echo_delay, 240u);
		atomic_store(&tm_be.echo_gain, 512u);
		atomic_store(&tm_be.src, TM_SRC_ECHO);
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
		eh_wait_ms(&t, 6000);

		bypassed = tm_dial_power_window(&t, w.rtp_peer, &seen);
		CHECK(seen > 5u);
		/* USA dial tone is 350 plus 440 Hz, and the echo of it comes
		 * back untouched. */
		CHECK(bypassed > 1e6);

		if (ec) {
			CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
			/* Bypassed means the filter ran no frame at all, which
			 * is what a freeze would not give: a frozen canceller
			 * still filters, it only stops adapting (ec.h). */
			CHECK_UINT(ls.frames, armed_frames);
			CHECK_UINT(ls.coef_energy_micro, 0);
		}

		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_OFF, 0, 0) == UDSP_OK);
		/* Windows follow each other until the checks below hold, or
		 * until 20 s of engine time, so a canceller that stays off
		 * still fails them. */
		end = eh_now_ms() + 20000u;
		stop = eh_mono_us() + 20000000u + EH_WAIT_SLACK_US;
		do {
			cancelled = tm_dial_power_window(&t, w.rtp_peer, &seen);
			if (seen > 5u && cancelled * 4.0 < bypassed &&
			    udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK &&
			    ls.frames > 1000u && ls.coef_energy_micro > 0u)
				break;
		} while (eh_now_ms() < end && eh_mono_us() < stop);
		CHECK(seen > 5u);
		/* The canceller is back on the frame it was taken off. */
		CHECK(cancelled * 4.0 < bypassed);

		ccfg.ec_enable = false;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		if (ec) {
			CHECK(eh_lec_state_wait(&t, &ls, false, false));
			CHECK(ls.frames > 1000u);
			CHECK(ls.coef_energy_micro > 0u);
		}
		CHECK(udsp_tone_stop(t.s, 0) == UDSP_OK);
		tm_call_down(&t, &w);
	}

	/* The cap gate: UDSP_FEAT_T38 stays clear while no fax plug-in exists,
	 * and the refusal displaces nothing. */
	CASE("a T.38 profile is refused while no fax plug-in exists");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK_UINT(udsp_cap(t.s)->features & UDSP_FEAT_T38, 0);
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);

		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_T38, UDSP_SEL_T38,
				       96) == UDSP_UNSUPPORTED);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.codec, UDSP_SEL_PCMU);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.codec_pt, 0);
		CHECK(!stream_at(t.s, 0, 0)->fax.armed);
		CHECK_UINT(stream_at(t.s, 0, 0)->passthrough_kind,
			   UDSP_PASSTHROUGH_OFF);

		/* The mode is what is refused, not the codec that rides with
		 * it: a T.38 request naming a codec libudsp does carry must
		 * not arm a passthrough the caller never asked for. */
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_T38, UDSP_SEL_PCMA,
				       8) == UDSP_UNSUPPORTED);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.codec, UDSP_SEL_PCMU);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.codec_pt, 0);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.jb_mode,
			   UDSP_JB_ADAPTIVE);
		CHECK(!stream_at(t.s, 0, 0)->fax.armed);
		CHECK_UINT(stream_at(t.s, 0, 0)->passthrough_kind,
			   UDSP_PASSTHROUGH_OFF);

		tm_call_down(&t, &w);
	}

	/* The same two arms with a gateway present. The mode is refused on its
	 * own account and not on the capability: T.38 reaches libudsp as a
	 * stream description, and this op has no channel profile for it. */
	CASE("a T.38 profile is refused while a fax plug-in exists");
	{
		if (!tm_call_up_fax(&t, "USA", 0, &w))
			return;
		CHECK(udsp_cap(t.s)->features & UDSP_FEAT_T38);
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);

		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_T38, UDSP_SEL_T38,
				       96) == UDSP_UNSUPPORTED);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.codec, UDSP_SEL_PCMU);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.codec_pt, 0);
		CHECK(!stream_at(t.s, 0, 0)->fax.armed);
		CHECK_UINT(stream_at(t.s, 0, 0)->passthrough_kind,
			   UDSP_PASSTHROUGH_OFF);

		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_T38, UDSP_SEL_PCMA,
				       8) == UDSP_UNSUPPORTED);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.codec, UDSP_SEL_PCMU);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.codec_pt, 0);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.jb_mode,
			   UDSP_JB_ADAPTIVE);
		CHECK(!stream_at(t.s, 0, 0)->fax.armed);
		CHECK_UINT(stream_at(t.s, 0, 0)->passthrough_kind,
			   UDSP_PASSTHROUGH_OFF);
		/* No gateway was made for either refusal. */
		CHECK_UINT(fax_fake_stats_get()->creates, 0);

		tm_call_down(&t, &w);
	}

	/* The other half of the same rule: a profile would install a G.711
	 * pair over the gateway and move the mode key with it. */
	CASE("a fax profile on a T.38 stream is refused whatever it asks for");
	{
		struct fax_fake_stats *fs = fax_fake_stats_get();
		uint8_t buf[TM_PKT_MAX];

		if (!tm_call_up_fax(&t, "USA", 0, &w))
			return;
		tm_t38_media(&m);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);

		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_UNSUPPORTED);
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_OFF, 0, 0) ==
		      UDSP_UNSUPPORTED);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.codec, UDSP_SEL_T38);
		CHECK_UINT(stream_at(t.s, 0, 0)->mode, STREAM_MODE_T38);
		CHECK_UINT(stream_at(t.s, 0, 0)->passthrough_kind,
			   UDSP_PASSTHROUGH_MODEM);
		CHECK(!stream_at(t.s, 0, 0)->fax.armed);
		CHECK_UINT(fs->creates, 1);
		CHECK_UINT(fs->destroys, 0);
		/* And the session it refused to displace carries on. */
		CHECK(tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300) ==
		      (ssize_t)FAX_FAKE_DGRAM);
		tm_call_down(&t, &w);
	}

	CASE("a fax profile on a stopped stream is refused");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_UNSUPPORTED);
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		/* Configured is not started: the profile follows the lifecycle
		 * guard every other stream op follows. */
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_UNSUPPORTED);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_OK);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_OFF, 0, 0) ==
		      UDSP_UNSUPPORTED);
		tm_call_close(&t, &w);
	}

	/* The caller's own description governs again, so nothing of the
	 * profile may survive the re-INVITE that replaced it. */
	CASE("a reconfiguration under a fax profile leaves no profile state");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.vad = true;
		m.cng = true;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMA, 8) == UDSP_OK);
		CHECK(stream_at(t.s, 0, 0)->fax.armed);
		CHECK_UINT(stream_at(t.s, 0, 0)->passthrough_kind,
			   UDSP_PASSTHROUGH_MODEM);

		tm_stream_config(&t, &w, &m);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.codec, UDSP_SEL_PCMU);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.jb_mode,
			   UDSP_JB_ADAPTIVE);
		CHECK(stream_at(t.s, 0, 0)->media.vad);
		CHECK(!stream_at(t.s, 0, 0)->media.passthrough);
		CHECK(!stream_at(t.s, 0, 0)->fax.armed);
		CHECK_UINT(stream_at(t.s, 0, 0)->passthrough_kind,
			   UDSP_PASSTHROUGH_OFF);
		/* Nothing was displaced any more, so OFF has nothing to do. */
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_OFF, 0, 0) == UDSP_OK);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.codec, UDSP_SEL_PCMU);

		tm_call_down(&t, &w);
	}

	CASE("a stop under a fax profile leaves no profile state");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.vad = true;
		m.cng = true;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMA, 8) == UDSP_OK);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		CHECK(!stream_at(t.s, 0, 0)->fax.armed);

		/* The next call takes the description it asked for. */
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.codec, UDSP_SEL_PCMU);
		CHECK_UINT(stream_at(t.s, 0, 0)->media.codec_pt, 0);
		CHECK_UINT(stream_at(t.s, 0, 0)->jb.mode, UDSP_JB_ADAPTIVE);
		CHECK(stream_at(t.s, 0, 0)->media.vad);
		CHECK_UINT(stream_at(t.s, 0, 0)->passthrough_kind,
			   UDSP_PASSTHROUGH_OFF);

		tm_call_down(&t, &w);
	}

	/* The kind carries the whole non-codec half of the profile, so a
	 * caller that writes it into its own description gets the treatment
	 * the profile gives, and one that drops it gets its own back. */
	CASE("a description that carries a kind clears vad and cng");
	{
		struct udsp_stream *st;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.vad = true;
		m.cng = true;
		m.passthrough = UDSP_PASSTHROUGH_FAX;
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			CHECK_UINT(st->media.passthrough, UDSP_PASSTHROUGH_FAX);
			CHECK(!st->media.vad);
			CHECK(!st->media.cng);
			/* The description asked for the adaptive buffer and a
			 * carrier cannot have one. */
			CHECK_UINT(st->media.jb_mode, UDSP_JB_FIXED);
			CHECK_UINT(st->jb.mode, UDSP_JB_FIXED);
		}

		m.passthrough = UDSP_PASSTHROUGH_OFF;
		tm_stream_config(&t, &w, &m);
		if (st) {
			CHECK_UINT(st->media.passthrough, UDSP_PASSTHROUGH_OFF);
			CHECK(st->media.vad);
			CHECK(st->media.cng);
			CHECK_UINT(st->media.jb_mode, UDSP_JB_ADAPTIVE);
			CHECK_UINT(st->jb.mode, UDSP_JB_ADAPTIVE);
		}
		tm_call_close(&t, &w);
	}

	/* The kind is a closed set, so a value outside it is refused where a
	 * bad jitter buffer mode is, and the stream keeps what it ran. */
	CASE("a description with an unknown kind is refused");
	{
		struct udsp_stream *st;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		m.passthrough = UDSP_PASSTHROUGH_FAX + 1u;
		CHECK(udsp_stream_config(t.s, 0, 0, &m, w.rtp_local,
					 w.rtcp_local) == UDSP_UNSUPPORTED);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			CHECK_UINT(st->media.passthrough, UDSP_PASSTHROUGH_OFF);
			CHECK_UINT(st->passthrough_kind, UDSP_PASSTHROUGH_OFF);
		}
		tm_call_down(&t, &w);
	}

	/* No profile is ever armed here: the description alone carries the
	 * kind, and the adoption derives the line policy from it. */
	CASE("a description that carries a kind alone bypasses the canceller");
	{
		struct udsp_lec_stats lec;
		struct udsp_stream *st;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK(eh_hook_seize(&t));
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.ec_enable = true;
		ccfg.ec_nlp_enable = true;
		ccfg.ec_tail_ms = 32;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.passthrough = UDSP_PASSTHROUGH_MODEM;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			CHECK_UINT(st->passthrough_kind,
				   UDSP_PASSTHROUGH_MODEM);
			CHECK(!st->fax.armed);
		}
		CHECK_UINT(stream_line_passthrough(t.s, 0),
			   UDSP_PASSTHROUGH_MODEM);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.ec_active, 0);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_MODEM);

		/* And the description that drops it puts the canceller back. */
		m.passthrough = UDSP_PASSTHROUGH_OFF;
		tm_stream_config(&t, &w, &m);
		eh_wait_ms(&t, 100);
		if (st)
			CHECK_UINT(st->passthrough_kind, UDSP_PASSTHROUGH_OFF);
		CHECK_UINT(stream_line_passthrough(t.s, 0),
			   UDSP_PASSTHROUGH_OFF);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);

		tm_call_down(&t, &w);
	}

	/*
	 * A fax exchange is half duplex, so the canceller stays on the frame
	 * and only its non-linear processor goes off: the processor mutes the
	 * direction it reads as idle, which a turn of the exchange cannot
	 * afford. The line therefore keeps answering for its canceller, and
	 * nlp_active is what says the policy took effect.
	 */
	CASE("a description that carries the fax kind runs the canceller with the nlp off");
	{
		struct udsp_lec_stats lec;
		struct udsp_stream *st;
		uint32_t frames = 0;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK(eh_hook_seize(&t));
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.ec_enable = true;
		ccfg.ec_nlp_enable = true;
		ccfg.ec_tail_ms = 32;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		/* An echo of the playback is what makes the processor act at
		 * all, so the line runs one for the whole case. */
		atomic_store(&tm_be.echo_delay, 240u);
		atomic_store(&tm_be.echo_gain, 512u);
		atomic_store(&tm_be.src, TM_SRC_ECHO);
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
		eh_wait_ms(&t, 2000);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.nlp_active, 1);
		frames = lec.frames;

		m.passthrough = UDSP_PASSTHROUGH_FAX;
		tm_stream_config(&t, &w, &m);
		eh_wait_ms(&t, 300);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st)
			CHECK_UINT(st->passthrough_kind, UDSP_PASSTHROUGH_FAX);
		CHECK_UINT(stream_line_passthrough(t.s, 0),
			   UDSP_PASSTHROUGH_FAX);
		/* The canceller is still on the frame, so the line answers,
		 * and the frames it ran under the fax kind are what says the
		 * filter kept running and not just that it once did. */
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.nlp_active, 0);
		CHECK(lec.frames > frames + 10u);

		/* The description that drops the kind gives the channel
		 * configuration its processor back. */
		m.passthrough = UDSP_PASSTHROUGH_OFF;
		tm_stream_config(&t, &w, &m);
		eh_wait_ms(&t, 2000);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.nlp_active, 1);

		CHECK(udsp_tone_stop(t.s, 0) == UDSP_OK);
		tm_call_down(&t, &w);
	}

	/*
	 * The same policy on a line whose canceller is created for a
	 * description that already carries the kind. The instance arrives from
	 * the control thread programmed with the channel's own setting, and
	 * the tick's tracker never sees that write, so this is the case where a
	 * tracker seeded by nothing leaves the processor on.
	 */
	CASE("a description that carries the fax kind from the first frame runs with the nlp off");
	{
		struct udsp_lec_stats lec;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK(eh_hook_seize(&t));
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.ec_enable = true;
		ccfg.ec_nlp_enable = true;
		ccfg.ec_tail_ms = 32;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.passthrough = UDSP_PASSTHROUGH_FAX;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		atomic_store(&tm_be.echo_delay, 240u);
		atomic_store(&tm_be.echo_gain, 512u);
		atomic_store(&tm_be.src, TM_SRC_ECHO);
		CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
		eh_wait_ms(&t, 2000);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.nlp_active, 0);
		CHECK(lec.frames > 0u);

		CHECK(udsp_tone_stop(t.s, 0) == UDSP_OK);
		tm_call_down(&t, &w);
	}

	/*
	 * The upspeed answer of a fax call: the caller programs the stream
	 * again while the carrier runs. The profile record dies with the
	 * description that call replaced, so the kind in the new description
	 * is the only thing that can keep the canceller off the carrier.
	 */
	CASE("a reconfiguration that carries the modem kind keeps the canceller bypassed");
	{
		struct udsp_lec_stats lec;
		struct udsp_stream *st;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK(eh_hook_seize(&t));
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.ec_enable = true;
		ccfg.ec_nlp_enable = true;
		ccfg.ec_tail_ms = 32;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);

		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.ec_active, 0);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_MODEM);

		m.passthrough = UDSP_PASSTHROUGH_MODEM;
		tm_stream_config(&t, &w, &m);
		eh_wait_ms(&t, 100);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			CHECK_UINT(st->media.passthrough,
				   UDSP_PASSTHROUGH_MODEM);
			CHECK_UINT(st->passthrough_kind,
				   UDSP_PASSTHROUGH_MODEM);
			CHECK(!st->fax.armed);
		}
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.ec_active, 0);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_MODEM);

		/* Nothing is displaced any more, so the profile off finds
		 * nothing to restore and leaves the description standing. */
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_OFF, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);
		if (st)
			CHECK_UINT(st->media.passthrough,
				   UDSP_PASSTHROUGH_MODEM);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.ec_active, 0);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_MODEM);

		/* The caller clears the carrier through the description. */
		m.passthrough = UDSP_PASSTHROUGH_OFF;
		tm_stream_config(&t, &w, &m);
		eh_wait_ms(&t, 100);
		if (st)
			CHECK_UINT(st->passthrough_kind, UDSP_PASSTHROUGH_OFF);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);

		tm_call_down(&t, &w);
	}

	/* The record holds what the description carried, so a profile applied
	 * over a description that already said fax gives the fax kind back,
	 * and does not leave the modem kind the profile itself arms. */
	CASE("a fax profile off restores the kind the description carried");
	{
		struct udsp_stream *st;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.passthrough = UDSP_PASSTHROUGH_FAX;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMA, 8) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			CHECK_UINT(st->media.codec, UDSP_SEL_PCMA);
			CHECK_UINT(st->media.passthrough,
				   UDSP_PASSTHROUGH_MODEM);
			CHECK_UINT(st->fax.passthrough, UDSP_PASSTHROUGH_FAX);
		}

		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_OFF, 0, 0) == UDSP_OK);
		if (st) {
			CHECK_UINT(st->media.codec, UDSP_SEL_PCMU);
			CHECK_UINT(st->media.passthrough,
				   UDSP_PASSTHROUGH_FAX);
			CHECK_UINT(st->passthrough_kind, UDSP_PASSTHROUGH_FAX);
		}

		tm_call_down(&t, &w);
	}

	/*
	 * A leg on local hold puts nothing of its own on the wire, so its
	 * carrier is not on the line and the other call of the line gets its
	 * canceller back for as long as the hold lasts.
	 */
	CASE("a held passthrough stream does not govern the line");
	{
		struct udsp_media voice;
		struct udsp_lec_stats lec;
		struct tm_wire w2;

		if (!tm_up(&t, "USA") || !tm_wire_open(&w, 0) ||
		    !tm_wire_open(&w2, 0)) {
			CHECK(false);
			return;
		}
		CHECK(eh_hook_seize(&t));
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.ec_enable = true;
		ccfg.ec_nlp_enable = true;
		ccfg.ec_tail_ms = 32;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.passthrough = UDSP_PASSTHROUGH_MODEM;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		tm_media(&voice, UDSP_SEL_PCMU, 0, 20);
		CHECK(udsp_stream_config(t.s, 0, 1, &voice, w2.rtp_local,
					 w2.rtcp_local) == UDSP_OK);
		CHECK(udsp_stream_start(t.s, 0, 1) == UDSP_OK);
		eh_wait_ms(&t, 100);
		CHECK_UINT(stream_line_passthrough(t.s, 0),
			   UDSP_PASSTHROUGH_MODEM);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.ec_active, 0);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_MODEM);

		m.tx_silence = true;
		tm_stream_config(&t, &w, &m);
		eh_wait_ms(&t, 100);
		CHECK_UINT(stream_line_passthrough(t.s, 0),
			   UDSP_PASSTHROUGH_OFF);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);

		/* The resume gives the line back to the carrier. */
		m.tx_silence = false;
		tm_stream_config(&t, &w, &m);
		eh_wait_ms(&t, 100);
		CHECK_UINT(stream_line_passthrough(t.s, 0),
			   UDSP_PASSTHROUGH_MODEM);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.ec_active, 0);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_MODEM);

		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_stream_stop(t.s, 0, 1) == UDSP_OK);
		tm_wire_close(&w);
		tm_call_close(&t, &w2);
	}

	/* One G.711 for another keeps the RTP clock, so the profile stays a
	 * reconfiguration and the source carries on. */
	CASE("a fax passthrough on the same clock keeps the source");
	{
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p = { 0 };
		uint32_t was_ssrc = 0;
		uint32_t now_ssrc = 0;
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 600);
		CHECK(n > 0 && tm_rtp_parse(buf, (size_t)n, &p));
		if (n > 0)
			was_ssrc = p.ssrc;

		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMA, 8) == UDSP_OK);
		tm_wire_drain(w.rtp_peer);
		eh_wait_ms(&t, 200);
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (tm_rtp_parse(buf, (size_t)n, &p) && p.pt == 8u)
				now_ssrc = p.ssrc;
		}
		CHECK(was_ssrc != 0u);
		CHECK_UINT(now_ssrc, was_ssrc);

		tm_call_down(&t, &w);
	}

	/*
	 * The three cases of the carried depth share one call, because the
	 * depth has to be adapted before any of them can read it: the far end
	 * arrives in bursts until the buffer holds a target of its own, well
	 * above the 20 ms the description asked for, and that is the figure
	 * every case below follows.
	 */
	{
		struct udsp_jb_stats js;
		double phase = 0.0;
		uint16_t adapted = 0;
		uint16_t pinned = 0;
		uint64_t resyncs = 0;
		struct udsp_stream *st;

		CASE("a fax passthrough pins the depth the buffer adapted to");
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		for (unsigned i = 0; i < 75u; i++) {
			tm_burst_send(&w, 4);
			eh_wait_ms(&t, 40);
		}
		/* The target grows on a jitter peak and sheds a frame per
		 * clean second, so a paced tail leaves it standing still while
		 * the case reads it. */
		for (unsigned i = 0; i < 30u; i++) {
			tm_burst_send(&w, 1);
			eh_wait_ms(&t, 10);
		}
		/* A full queue at the switch, so what survives is visible. */
		for (unsigned i = 0; i < 6u; i++)
			tm_burst_send(&w, 1);
		/* Read last, so no tick stands between the reading and the
		 * profile: the target grows only on a fresh jitter peak and
		 * sheds only after a clean second, and the paced tail above
		 * leaves it neither. */
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &js) == UDSP_OK);
		adapted = js.target_ms;
		resyncs = js.resyncs;
		CHECK(adapted > m.jb_init_ms);

		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			tm_call_close(&t, &w);
			return;
		}
		pinned = st->jb_pin_ms;
		CHECK_UINT(st->jb.mode, UDSP_JB_FIXED);
		/* The pin is the depth the buffer held, never the 20 ms the
		 * description named. */
		CHECK_UINT(pinned, adapted);
		CHECK(pinned > m.jb_init_ms);
		/* The reconfigure acks before the tick republishes the snapshot;
		 * stats become valid only after the first tick. */
		eh_wait_ms(&t, 10);
		/* The mode moved and nothing else did, so the audio the queue
		 * holds is still there to play. depth_ms reads zero only for an
		 * empty queue, so the published snapshot stands in for the raw
		 * count it does not carry. */
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &js) == UDSP_OK);
		CHECK(js.depth_ms > 0u);
		eh_wait_ms(&t, 20);
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &js) == UDSP_OK);
		CHECK_UINT(js.target_ms, pinned);
		CHECK_UINT(js.resyncs, resyncs);

		CASE("the upspeed reconfiguration keeps the pinned depth");
		m.passthrough = UDSP_PASSTHROUGH_MODEM;
		m.jb_mode = UDSP_JB_FIXED;
		tm_stream_config(&t, &w, &m);
		for (unsigned i = 0; i < 20u; i++) {
			tm_burst_send(&w, 1);
			eh_wait_ms(&t, 10);
		}
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &js) == UDSP_OK);
		/* The answer carries the configured target again and the
		 * buffer keeps the one it adapted to. */
		CHECK_UINT(js.target_ms, pinned);
		CHECK_UINT(st->jb_pin_ms, pinned);

		CASE("a fax passthrough that changes the codec re-inits at the pinned depth");
		m.codec = UDSP_SEL_PCMA;
		m.codec_pt = 8;
		tm_stream_config(&t, &w, &m);
		CHECK_UINT(st->jb.mode, UDSP_JB_FIXED);
		CHECK_UINT(st->jb.init_ms, pinned);
		for (unsigned i = 0; i < 20u; i++) {
			tm_tone_send_pt(&w, 8, 10, 1000.0, 8000.0, &phase,
					true);
			eh_wait_ms(&t, 10);
		}
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &js) == UDSP_OK);
		CHECK_UINT(js.target_ms, pinned);

		CASE("a stop gives the pinned depth back");
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		/* The next call starts a passthrough of its own, and the depth
		 * the call that ended adapted to is none of its business. */
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.passthrough = UDSP_PASSTHROUGH_MODEM;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		CHECK(pinned > m.jb_init_ms);
		CHECK_UINT(st->jb_pin_ms, m.jb_init_ms);
		CHECK_UINT(st->jb.init_ms, m.jb_init_ms);

		tm_call_down(&t, &w);
	}

	/*
	 * A carrier has no pitch to repeat, so every unit the buffer cannot
	 * fill is silence: the far end reads a dropout and retrains out of it.
	 * The second arm runs the same outage without the profile and hears
	 * the App I concealment, so the first arm's zero is not vacuous. Both
	 * windows open on the first concealed unit the counters report and run
	 * 50 ms, which is inside the 60 ms App I fades over.
	 */
	CASE("a fax passthrough fills a lost packet with silence");
	{
		double quiet = tm_fax_loss_energy(true);
		double loud = tm_fax_loss_energy(false);

		CHECK(quiet == 0.0);
		CHECK(loud > 100.0);
	}

	/*
	 * The passthrough moves the plc_off codec parameter, so the pair is
	 * recreated and the decoder that runs the carrier keeps no App I state
	 * at all. Without that the instance would stand: it holds the loss
	 * count of the concealment the profile arrives in, and cross-fades the
	 * first decoded frame into a pitch repeat of the speech, and it holds
	 * a 30 sample delay line that the first frame would come out behind.
	 * The first carrier frame has to reach the level the frames after it
	 * hold, sample for sample.
	 */
	CASE("a fax passthrough that lands mid-concealment plays a clean first frame");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct udsp_jb_stats js;
		struct udsp_stream *st;
		uint64_t plc_before;
		double phase = 0.0;
		double steady = 0.0;
		unsigned window = 0;
		unsigned first = 0;
		unsigned weak = 0;
		unsigned reference_end;
		bool found = false;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st || !eh_gate_park(&gate))
			goto carrier_out;
		if (!tm_gate_tone_run(&gate, st, &w, 4, &phase) ||
		    !tm_gate_plc_wait(&gate, st, &plc_before) ||
		    !eh_gate_step(&gate, 14))
			goto carrier_out;
		/* App I reaches zero before the profile, so a retained decoder
		 * would cross-fade the first carrier frame against silence. */
		udsp_jb_stats(&st->jb, &js);
		/* Fifteen frames stay below JB_PLC_MAX_MS (400 ms). */
		CHECK_UINT(js.plc - plc_before, 15);
		eh_gate_disarm(&gate);
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_OK);
		if (!eh_gate_park(&gate) ||
		    !tm_gate_tone_send(st, &w, 30, &phase))
			goto carrier_out;
		eh_rec_mark();
		/* The pinned depth plus four frames bounds the first carrier;
		 * a later frame would escape the per-frame comparisons. */
		window = ((unsigned)st->jb_pin_ms + 40u) * 8u;
		if (!eh_gate_step(&gate, (window + UDSP_FRAME_SAMPLES - 1u) /
					UDSP_FRAME_SAMPLES))
			goto carrier_out;
		CHECK(eh_rec_have() >= window);
		if (eh_rec_have() < window)
			goto carrier_out;
		/* The playback periods are frame aligned and the mark sits on
		 * one, so the first loud block is the first carrier frame the
		 * line played. */
		for (unsigned i = 0; i + UDSP_FRAME_SAMPLES <= window;
		     i += UDSP_FRAME_SAMPLES) {
			if (eh_energy(i, UDSP_FRAME_SAMPLES) < 1e5)
				continue;
			first = i;
			found = true;
			break;
		}
		CHECK(found);
		if (!found)
			goto carrier_out;
		reference_end = first + 11u * UDSP_FRAME_SAMPLES;
		if (eh_rec_have() < reference_end &&
		    !eh_gate_step(&gate, (reference_end - eh_rec_have() +
					  UDSP_FRAME_SAMPLES - 1u) /
					 UDSP_FRAME_SAMPLES))
			goto carrier_out;
		CHECK(eh_rec_have() >= reference_end);
		if (eh_rec_have() < reference_end)
			goto carrier_out;
		/* The steady level is read well past the first frame, and
		 * every frame from the first one on has to hold it: a first
		 * frame that came out short or half empty cannot hide behind a
		 * clean successor. */
		steady = eh_tone_power(first + 10u * UDSP_FRAME_SAMPLES,
				       UDSP_FRAME_SAMPLES, 1000.0,
				       UDSP_SAMPLE_RATE);
		CHECK(steady > 1e6);
		for (unsigned i = first; i + UDSP_FRAME_SAMPLES <= window;
		     i += UDSP_FRAME_SAMPLES) {
			if (eh_tone_power(i, UDSP_FRAME_SAMPLES, 1000.0,
					  UDSP_SAMPLE_RATE) <= 0.9 * steady)
				weak++;
		}
		CHECK_UINT(weak, 0);

carrier_out:
		eh_gate_disarm(&gate);
		if (st)
			CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_call_close(&t, &w);
		eh_gate_close(&gate);
	}

	/*
	 * The detector half of the same rule. A carrier is not a digit, so a
	 * decision the detector makes over one is talk-off, and the caller
	 * relays the digit it reads as an event whose packets replace carrier
	 * frames. The digit here is a real one the subscriber dialed, so the
	 * second arm is what proves the hold produced the zero and not a
	 * detector that hears nothing at all.
	 */
	CASE("a fax passthrough holds the DTMF detector");
	{
		const struct udsp_event *e;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_DTMF;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);

		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_OK);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 300);
		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		eh_wait_ms(&t, 600);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 0);

		/* The line takes the detector back with the kind. */
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_OFF, 0, 0) ==
		      UDSP_OK);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 300);
		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		eh_wait_ms(&t, 600);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);
		e = eh_ev_find(UDSP_EV_DIGIT, 0);
		CHECK(e != NULL);
		if (e) {
			CHECK_UINT(e->u.digit.digit, '5');
			CHECK_UINT(e->u.digit.remote, 0);
		}

		tm_call_down(&t, &w);
	}

	/*
	 * The flush the hold takes on its way in. A digit the detector already
	 * decided is a real digit, so the carrier that interrupts it does not
	 * swallow it: the line reports what the detector heard, which is
	 * shorter than what the line played.
	 */
	CASE("a fax passthrough reports the digit it interrupts");
	{
		const struct udsp_event *e;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_DTMF;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);

		/* The decision takes seven windows of tone, so 250 ms leaves
		 * the digit decided and 550 ms of it still to sound. */
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 800);
		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		eh_wait_ms(&t, 250);
		/* Nothing ended the digit yet, so the report below is the
		 * flush and not a digit that finished on its own. */
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 0);

		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);
		e = eh_ev_find(UDSP_EV_DIGIT, 0);
		CHECK(e != NULL);
		if (e) {
			CHECK_UINT(e->u.digit.digit, '5');
			CHECK_UINT(e->u.digit.remote, 0);
			/* The length is what the detector heard, never what
			 * the line carried. */
			CHECK(e->u.digit.duration_ms > 0u);
			CHECK(e->u.digit.duration_ms < 800u);
		}

		/* The rest of the digit sounds into a held detector, so no
		 * second report follows it. */
		eh_wait_ms(&t, 900);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);

		tm_call_down(&t, &w);
	}

	/*
	 * What that flush is worth to a live type 2 caller identity. The digit
	 * the carrier interrupts is the acknowledgment, so the sequence sends
	 * its burst instead of reporting UDSP_EV_CID2_TIMEOUT. The flush stamps
	 * the digit at its own start (line.h line_cidseq_dtmf_ack), so the
	 * acknowledgment holds even though the deciding tick comes later.
	 */
	CASE("a fax passthrough acknowledges the type 2 caller identity it interrupts");
	{
		const struct udsp_event *sent;
		struct udsp_cid cid;
		uint32_t cas_ms;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_DTMF;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(udsp_line_state(t.s, 0, UDSP_LINE_ACTIVE) == UDSP_OK);
		/* Call waiting runs mid-call, so the line goes off hook before
		 * the sequence starts: a hook edge under it aborts the run. */
		udsp_slic_fake_hook_set(t.s->slic, 0, true);
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		atomic_store(&tm_be.echo_delay, 240u);
		atomic_store(&tm_be.echo_gain, 1024u);
		atomic_store(&tm_be.src, TM_SRC_ECHO);
		eh_wait_ms(&t, 100);

		memset(&cid, 0, sizeof(cid));
		snprintf(cid.number, sizeof(cid.number), "%s", "5559876");
		snprintf(cid.name, sizeof(cid.name), "%s", "WAITING");
		CHECK(udsp_cid_send(t.s, 0, UDSP_CID_TYPE2, &cid) == UDSP_OK);
		cas_ms = tm_cid2_cas_wait(&t, 0);
		CHECK(cas_ms != 0);

		/*
		 * The CAS runs 80 ms and the USA ack_timeout_ms is 300 ms, so a
		 * sequence that starts at 200 ms acknowledges from 280 to
		 * 580 ms. Measured here: the digit plays from 310 ms, the
		 * detector stamps its start at 330 ms and the carrier flushes
		 * it at 490 ms, which leaves 90 ms of the window.
		 */
		eh_wait_ms(&t, 100);
		CHECK(udsp_digit_play(t.s, 0, 'D', 800) == UDSP_OK);
		eh_wait_ms(&t, 150);
		/* Nothing ended the digit yet, so the report below is the flush
		 * and not a digit that finished on its own. */
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 0);
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_OK);
		eh_wait_ms(&t, 900);

		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);
		{
			const struct udsp_event *e =
				eh_ev_find(UDSP_EV_DIGIT, 0);

			if (e) {
				CHECK_UINT(e->u.digit.digit, 'D');
				CHECK_UINT(e->u.digit.remote, 0);
			}
		}
		sent = eh_ev_find(UDSP_EV_CID_SENT, 0);
		CHECK(sent != NULL);
		if (sent) {
			CHECK_UINT((uint8_t)sent->u.cid_sent.status, 0);
			CHECK_UINT(sent->u.cid_sent.kind, UDSP_CID_TYPE2);
		}
		CHECK_UINT(eh_ev_count(UDSP_EV_CID2_TIMEOUT), 0);

		tm_call_down(&t, &w);
	}

	/*
	 * The sender half, on the description that turns the kind on: the cut
	 * and the refusal the hold silence already gives, for the reason the
	 * carrier gives (udsp.h udsp_media.passthrough).
	 */
	CASE("a fax passthrough cuts a telephone event in flight and refuses the next");
	{
		uint8_t buf[TM_PKT_MAX];
		struct udsp_stream *st;
		struct tm_pkt r = { 0 };
		unsigned before = 0;
		unsigned after = 0;
		unsigned ends = 0;
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 40);
		tm_wire_drain(w.rtp_peer);

		/* Long enough that the kind falls in the middle of it. */
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '5', 2000) == UDSP_OK);
		eh_wait_ms(&t, 60);
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (tm_rtp_parse(buf, (size_t)n, &r) && r.pt == 101u)
				before++;
		}
		CHECK(before > 0u);

		/* The profile blocks on the tick ack, so the cut is applied
		 * before it returns and the window after the drain belongs to
		 * the carrier alone. */
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_OK);
		tm_wire_drain(w.rtp_peer);
		eh_wait_ms(&t, 300);
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (!tm_rtp_parse(buf, (size_t)n, &r) || r.pt != 101u)
				continue;
			after++;
			if (r.payload[1] & 0x80u)
				ends++;
		}
		/* No further packet of the event, and no end packet trailing
		 * behind it either. */
		CHECK_UINT(after, 0);
		CHECK_UINT(ends, 0);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			CHECK(!st->tev_tx.active);
			CHECK_UINT(st->passthrough_kind,
				   UDSP_PASSTHROUGH_MODEM);
		}
		/* UNSUPPORTED and not TRANSIENT: no retry can make a carrier
		 * carry a digit. */
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '5', 100) ==
		      UDSP_UNSUPPORTED);

		/* The end of the carrier takes the refusal with it. */
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_OFF, 0, 0) ==
		      UDSP_OK);
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '5', 100) == UDSP_OK);

		tm_call_down(&t, &w);
	}

	/* No profile is armed here either: the kind the first description
	 * carries is what refuses the event. */
	CASE("a description that carries a kind refuses a telephone event");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		m.passthrough = UDSP_PASSTHROUGH_FAX;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 40);
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '5', 100) ==
		      UDSP_UNSUPPORTED);

		tm_call_down(&t, &w);
	}

	/*
	 * The hold follows the line policy, so a leg on local hold does not
	 * take the detector from the call that is still up: the held leg puts
	 * nothing of its own on the wire, so its carrier is not on the line.
	 */
	CASE("a held passthrough leg leaves the other call's detector armed");
	{
		const struct udsp_event *e;
		struct udsp_stream *st;
		struct udsp_media voice;
		struct tm_wire w2;

		if (!tm_up(&t, "USA") || !tm_wire_open(&w, 0) ||
		    !tm_wire_open(&w2, 0)) {
			CHECK(false);
			return;
		}
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_DTMF;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.passthrough = UDSP_PASSTHROUGH_MODEM;
		m.tx_silence = true;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		tm_media(&voice, UDSP_SEL_PCMU, 0, 20);
		CHECK(udsp_stream_config(t.s, 0, 1, &voice, w2.rtp_local,
					 w2.rtcp_local) == UDSP_OK);
		CHECK(udsp_stream_start(t.s, 0, 1) == UDSP_OK);
		eh_wait_ms(&t, 200);
		/* The held leg really carries the kind, so the line answers
		 * OFF because the hold rule says so and not because the
		 * carrier never arrived. */
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st)
			CHECK_UINT(st->passthrough_kind,
				   UDSP_PASSTHROUGH_MODEM);
		CHECK_UINT(stream_line_passthrough(t.s, 0),
			   UDSP_PASSTHROUGH_OFF);

		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 300);
		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		eh_wait_ms(&t, 600);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);
		e = eh_ev_find(UDSP_EV_DIGIT, 0);
		CHECK(e != NULL);
		if (e)
			CHECK_UINT(e->u.digit.digit, '5');

		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_stream_stop(t.s, 0, 1) == UDSP_OK);
		tm_wire_close(&w);
		tm_call_close(&t, &w2);
	}
}

/* --- cases: descriptors, drops and statistics ---------------------------- */

static unsigned tm_fd_count(void)
{
	unsigned n = 0;

	for (int fd = 0; fd < 512; fd++) {
		if (fcntl(fd, F_GETFD) >= 0)
			n++;
	}

	return n;
}

static void case_fds(void)
{
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;

	CASE("a hundred configure and stop cycles leak no descriptor");
	{
		unsigned before, once, after;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		/* Counted with no stream configured, so a dup that outlives
		 * its stop shows up instead of being closed by the next
		 * configuration. */
		before = tm_fd_count();
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		once = tm_fd_count();
		CHECK_UINT(once, before);
		for (unsigned i = 0; i < 100u; i++) {
			tm_stream_config(&t, &w, &m);
			CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
			CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		}
		after = tm_fd_count();
		CHECK_UINT(after, before);
		tm_call_close(&t, &w);
	}

	CASE("a stopped stream writes nothing to the numbers it gave back");
	{
		struct udsp_stream *st;
		int marker[2] = { -1, -1 };
		int dup_rtp, dup_rtcp;
		char sink[64];

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 60);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		dup_rtp = st ? st->rtp_fd : -1;
		dup_rtcp = st ? st->rtcp_fd : -1;
		CHECK(dup_rtp >= 0 && dup_rtcp >= 0);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);

		/* The dups are gone before the op returns (udsp.h Media
		 * sockets), so this pipe takes their exact numbers back, and
		 * nothing the engine still holds may write into them. */
		CHECK(pipe(marker) == 0);
		if (marker[0] >= 0) {
			CHECK_UINT(marker[0], dup_rtp);
			CHECK_UINT(marker[1], dup_rtcp);
			fcntl(marker[0], F_SETFL, O_NONBLOCK);
			eh_wait_ms(&t, 200);
			CHECK(read(marker[0], sink, sizeof(sink)) < 0);
			CHECK(errno == EAGAIN);
			close(marker[0]);
			close(marker[1]);
		}
		tm_call_close(&t, &w);
	}

	CASE("a full descriptor table leaves the running stream untouched");
	{
		struct rlimit rl, saved;
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p;
		int probe;
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 60);

		CHECK(getrlimit(RLIMIT_NOFILE, &saved) == 0);
		probe = dup(0);
		CHECK(probe >= 0);
		if (probe >= 0)
			close(probe);
		rl = saved;
		rl.rlim_cur = (rlim_t)probe;
		CHECK(setrlimit(RLIMIT_NOFILE, &rl) == 0);
		CHECK(udsp_stream_config(t.s, 0, 0, &m, w.rtp_local,
					 w.rtcp_local) == UDSP_TRANSIENT);
		CHECK(setrlimit(RLIMIT_NOFILE, &saved) == 0);

		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300);
		CHECK(n > 0);
		CHECK(n > 0 && tm_rtp_parse(buf, (size_t)n, &p));
		tm_call_down(&t, &w);
	}
}

/* --- cases: the quiesce protocol under a lost ack ------------------------ */

/*
 * Both injection hooks. A lost ack is the whole subject of these cases and
 * wall-clock timing cannot produce one on demand, so the tick and the net
 * thread each take an arm: the work runs exactly as it does on a target and
 * the ack alone goes missing, which is what the stale-token discard leaves
 * behind. See the notes on udsp.ack_drop_op and udsp_net.ack_drop.
 */
static void tm_arm_engine_drop(struct eh *t, enum stream_op op)
{
	atomic_store(&t->s->ack_drop_op, (uint_least32_t)op + 1u);
}

static void tm_arm_net_drop(struct eh *t, enum net_cmd_type type)
{
	atomic_store(&t->s->net.ack_drop, (uint_least32_t)type + 1u);
}

/* Arms the tick to run the next UDSP_CMD_LEC_RELEASE retire and then swallow its
 * ack, so the release's control-side sync times out with the canceller already
 * in the mailbox. */
static void tm_arm_lec_drop(struct eh *t)
{
	atomic_store(&t->s->lec_ack_drop, true);
}

/* The second thread of the race: it dead marks the stream from inside a
 * ctl_lock hold that lasts the whole ack expiry. */
static struct {
	udsp_t *s;
	atomic_bool entered;
	_Atomic int status;
} tm_race;

static void *tm_race_stop(void *arg)
{
	(void)arg;
	atomic_store(&tm_race.entered, true);
	atomic_store(&tm_race.status, (int)udsp_stream_stop(tm_race.s, 0, 0));

	return NULL;
}

/* Every public op of a dead-marked stream answers UDSP_FATAL: the stream is
 * gone from the caller's point of view and no retry can bring it back. */
static void tm_dead_ops_check(struct eh *t, const struct udsp_media *m,
			      struct tm_wire *w)
{
	struct udsp_media_stats stats;
	struct udsp_stream_diag diag;

	CHECK(udsp_stream_start(t->s, 0, 0) == UDSP_FATAL);
	CHECK(udsp_stream_stop(t->s, 0, 0) == UDSP_FATAL);
	CHECK(udsp_stream_dtmf(t->s, 0, 0, '1', 100) == UDSP_FATAL);
	CHECK(udsp_stream_stats(t->s, 0, 0, &stats) == UDSP_FATAL);
	CHECK(udsp_stream_diag(t->s, 0, 0, &diag) == UDSP_FATAL);
	CHECK(udsp_stream_config(t->s, 0, 0, m, w->rtp_local, w->rtcp_local) ==
	      UDSP_FATAL);
}

static void case_quiesce(void)
{
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;

	/*
	 * The deactivation is what proves the engine left the stream. Without
	 * it the engine may still be inside the codec instances and the SRTP
	 * session, so the teardown must never run. Freeing anyway is a
	 * use-after-free on the tick thread.
	 */
	CASE("a lost deactivate ack marks the stream dead and frees nothing");
	{
		struct udsp_stream *st;
		int rtp_dup = -1;
		int rtcp_dup = -1;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 60);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			tm_call_close(&t, &w);
			return;
		}
		rtp_dup = st->rtp_fd;
		rtcp_dup = st->rtcp_fd;

		tm_arm_engine_drop(&t, STREAM_OP_DEACTIVATE);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_FATAL);

		/* Nothing was released: the descriptors are still libudsp's and
		 * the codec instances are still installed. */
		CHECK_UINT(st->rtp_fd, (unsigned)rtp_dup);
		CHECK_UINT(st->rtcp_fd, (unsigned)rtcp_dup);
		CHECK(fcntl(rtp_dup, F_GETFD) >= 0);
		CHECK(fcntl(rtcp_dup, F_GETFD) >= 0);
		CHECK(st->ops != NULL);
		CHECK(st->enc != NULL && st->dec != NULL);
		tm_dead_ops_check(&t, &m, &w);

		/* udsp_close runs stream_pool_shutdown with every thread
		 * stopped, so the teardown happens in that call: a shutdown
		 * that skips a dead-marked stream leaks both dups for ever.
		 * Nothing opens a descriptor between the close and the check,
		 * so the numbers cannot have been recycled. */
		tm_call_close(&t, &w);
		CHECK(fcntl(rtp_dup, F_GETFD) < 0);
		CHECK(fcntl(rtcp_dup, F_GETFD) < 0);
	}

	/*
	 * An IDLE stream never activated anything, so a lost ADOPT ack undoes
	 * the whole configuration: the descriptors, the codec instances, the
	 * session and the key bytes. Leaving them installed under an IDLE state
	 * is a descriptor and session leak that stream_pool_shutdown then
	 * skips, and it leaves the key bytes in memory.
	 */
	CASE("a lost adopt ack undoes the whole configuration");
	{
		struct udsp_stream *st;
		uint8_t zero[30] = { 0 };
		unsigned before, after;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			tm_call_close(&t, &w);
			return;
		}
		before = tm_fd_count();
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.srtp.suite = UDSP_SRTP_AES_CM_128_SHA1_80;
		tm_key_set(m.srtp.key_tx, 0x35);
		tm_key_set(m.srtp.key_rx, 0x57);
		tm_arm_engine_drop(&t, STREAM_OP_ADOPT);
		CHECK(udsp_stream_config(t.s, 0, 0, &m, w.rtp_local,
					 w.rtcp_local) == UDSP_TRANSIENT);

		after = tm_fd_count();
		CHECK_UINT(after, before);
		CHECK(st->rtp_fd < 0);
		CHECK(st->rtcp_fd < 0);
		CHECK(!st->srtp_live);
		CHECK(st->ops == NULL);
		CHECK(st->enc == NULL && st->dec == NULL);
		CHECK(!atomic_load(&st->dead));
		CHECK_UINT(atomic_load(&st->state), STREAM_IDLE);
		/* The key bytes never outlive the failed configuration. */
		CHECK(!memcmp(st->media.srtp.key_tx, zero, sizeof(zero)));
		CHECK(!memcmp(st->media.srtp.key_rx, zero, sizeof(zero)));
		CHECK(!memcmp(st->srtp.key_tx, zero, sizeof(zero)));
		CHECK_UINT(st->media.srtp.suite, UDSP_SRTP_NONE);

		/* A clean IDLE stream is reusable, which is the whole point of
		 * undoing instead of dead marking here. */
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 60);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		CHECK_UINT(tm_fd_count(), before);
		tm_call_close(&t, &w);
	}

	/* A RUNNING stream has no clean state to fall back to: its quiesce
	 * protocol is already half spent, so it takes the dead mark instead. */
	CASE("a lost reconfigure ack marks a running stream dead");
	{
		struct udsp_stream *st;
		int rtp_dup = -1;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 60);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);

		tm_arm_engine_drop(&t, STREAM_OP_RECONFIG);
		tm_media(&m, UDSP_SEL_PCMA, 8, 20);
		CHECK(udsp_stream_config(t.s, 0, 0, &m, w.rtp_local,
					 w.rtcp_local) == UDSP_FATAL);
		if (st) {
			CHECK(atomic_load(&st->dead));
			CHECK(st->rtp_fd >= 0);
			CHECK(st->ops != NULL);
			rtp_dup = st->rtp_fd;
		}
		tm_dead_ops_check(&t, &m, &w);
		tm_call_close(&t, &w);
		CHECK(rtp_dup >= 0 && fcntl(rtp_dup, F_GETFD) < 0);
	}

	/*
	 * The undo of a lost ADOPT ack needs proof that the engine left the
	 * canceled command, and the proof is two published ticks. A wedged
	 * engine publishes none: the ack expires, the settle expires, and
	 * freeing then would hand the resumed tick a destroyed session. No
	 * proof means the dead mark, not the undo.
	 */
	CASE("an adopt the engine never answered and never left is not freed");
	{
		struct udsp_stream *st;
		int wedge[2] = { -1, -1 };
		int rtp_dup = -1;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		CHECK(pipe(wedge) == 0);
		if (!st || wedge[0] < 0) {
			tm_call_close(&t, &w);
			return;
		}

		/* From the next read on the tick blocks, so the clock stops
		 * publishing and the settle can prove nothing. */
		atomic_store(&eh_be.block_after, atomic_load(&eh_be.reads));
		atomic_store(&eh_be.block_fd, wedge[0]);
		eh_sleep_ms(30);

		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.srtp.suite = UDSP_SRTP_AES_CM_128_SHA1_80;
		tm_key_set(m.srtp.key_tx, 0x18);
		tm_key_set(m.srtp.key_rx, 0x81);
		CHECK(udsp_stream_config(t.s, 0, 0, &m, w.rtp_local,
					 w.rtcp_local) == UDSP_FATAL);
		CHECK(atomic_load(&st->dead));
		/* Nothing was released: the tick may still be inside the
		 * command that read this description. */
		CHECK(st->rtp_fd >= 0);
		CHECK(st->rtcp_fd >= 0);
		CHECK(st->ops != NULL);
		CHECK(st->enc != NULL && st->dec != NULL);
		CHECK(st->srtp_live);
		CHECK_UINT(st->media.srtp.suite, UDSP_SRTP_AES_CM_128_SHA1_80);
		rtp_dup = st->rtp_fd;
		tm_dead_ops_check(&t, &m, &w);

		/* Let the engine go, so the handle stops and closes normally
		 * and stream_pool_shutdown does the teardown. */
		{
			char one = 'x';

			atomic_store(&eh_be.block_fd, -1);
			CHECK(write(wedge[1], &one, 1) == 1);
		}
		eh_wait_ms(&t, 60);
		tm_call_close(&t, &w);
		close(wedge[0]);
		close(wedge[1]);
		CHECK(rtp_dup >= 0 && fcntl(rtp_dup, F_GETFD) < 0);
	}

	/*
	 * udsp_guard answers UDSP_TRANSIENT once the engine has stopped, which
	 * invites the retry a dead stream can never satisfy, so the mark has to
	 * be read before that answer is formed.
	 */
	CASE("a dead stream still answers fatal after the engine stops");
	{
		struct udsp_job job = { 0 };
		struct udsp_media_stats stats;
		int32_t status = -1;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 60);
		tm_arm_engine_drop(&t, STREAM_OP_DEACTIVATE);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_FATAL);

		/* The engine is gone from here on, so every op below would
		 * answer UDSP_TRANSIENT if the mark were read after it. */
		CHECK(udsp_stop(t.s, &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 5000, &status));
		t.stopped = true;
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_FATAL);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_FATAL);
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '1', 100) == UDSP_FATAL);
		CHECK(udsp_stream_stats(t.s, 0, 0, &stats) == UDSP_FATAL);
		CHECK(udsp_stream_config(t.s, 0, 0, &m, w.rtp_local,
					 w.rtcp_local) == UDSP_FATAL);
		tm_call_close(&t, &w);
	}

	/*
	 * net_cmd_attach installs the epoll entries before its ack, so a lost
	 * ack leaves the net side attached while only the engine rolls back:
	 * the descriptors stay in the epoll set of a stream the caller believes
	 * never started.
	 */
	CASE("a lost attach ack takes the net side back out of the epoll set");
	{
		struct udsp_stream *st;
		double phase = 0.0;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			tm_call_close(&t, &w);
			return;
		}

		tm_arm_net_drop(&t, NET_CMD_ATTACH);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_TRANSIENT);
		CHECK(!atomic_load(&st->dead));
		/* The detach acked, so its writes are visible here. */
		CHECK(!st->attached);

		/* The ring is the witness: a stream still in the epoll set
		 * takes these datagrams in, and the engine is inactive so
		 * nothing drains them again. */
		CHECK_UINT(atomic_load(&st->ring.head), 0);
		for (unsigned i = 0; i < 10u; i++)
			tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
		eh_wait_ms(&t, 200);
		CHECK_UINT(atomic_load(&st->ring.head), 0);

		/* The failure is TRANSIENT, so the start still has to work. */
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		for (unsigned i = 0; i < 10u; i++)
			tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
		eh_wait_ms(&t, 200);
		{
			struct udsp_media_stats stats;

			CHECK(udsp_stream_stats(t.s, 0, 0, &stats) == UDSP_OK);
			CHECK(stats.packets_recv > 0);
		}
		tm_call_down(&t, &w);
	}

	/*
	 * stream_guard runs before ctl_lock, so a call that passed it can sit
	 * on the lock while another call dead marks the stream. The mark that
	 * arrives in that window is the one the waiter must obey. The dead
	 * marking stop below holds the lock for its whole ack expiry, which is
	 * the window this case parks the second call in.
	 */
	CASE("a mark that lands while an op waits for the lock is obeyed");
	{
		struct udsp_stream *st;
		pthread_t killer;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 60);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);

		tm_race.s = t.s;
		atomic_store(&tm_race.entered, false);
		atomic_store(&tm_race.status, -1);
		tm_arm_engine_drop(&t, STREAM_OP_DEACTIVATE);
		CHECK(pthread_create(&killer, NULL, tm_race_stop, NULL) == 0);
		while (!atomic_load(&tm_race.entered))
			eh_sleep_ms(1);
		/* Inside the killer's lock hold, and well before its ack
		 * expires: this call passes the unlocked guard and parks. */
		eh_sleep_ms(20);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_FATAL);
		CHECK(pthread_join(killer, NULL) == 0);
		CHECK_UINT((uint32_t)atomic_load(&tm_race.status), UDSP_FATAL);
		if (st) {
			/* The waiter tore nothing down: the mark outranks the
			 * guard answer it was already holding. */
			CHECK(atomic_load(&st->dead));
			CHECK(st->rtp_fd >= 0);
			CHECK(st->ops != NULL);
		}
		tm_call_close(&t, &w);
	}

	/*
	 * A dead-marked stream stays ACTIVE and keeps its fax profile, because
	 * a lost ack releases nothing it left in doubt. The echo canceller
	 * belongs to the line, so a bypass scan that counts that stream takes
	 * the canceller away from every healthy stream on the line until the
	 * handle closes: one stream's lost ack would become the whole line's
	 * fault.
	 */
	CASE("a dead stream under a fax profile leaves the line canceller alone");
	{
		struct udsp_lec_stats ls;
		struct udsp_chan_cfg ccfg;
		struct udsp_stream *st;
		struct udsp_ec *ec;
		struct tm_wire w2;
		uint32_t held;

		memset(&ls, 0, sizeof(ls));
		if (!tm_up(&t, "USA") || !tm_wire_open(&w, 0) ||
		    !tm_wire_open(&w2, 0)) {
			CHECK(false);
			return;
		}
		CHECK(eh_hook_seize(&t));
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.ec_enable = true;
		ccfg.ec_tail_ms = 32;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_config(t.s, 0, 1, &m, w2.rtp_local,
					 w2.rtcp_local) == UDSP_OK);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_stream_start(t.s, 0, 1) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		ec = atomic_load(&t.s->chan[0].ec);
		CHECK(st != NULL);
		CHECK(ec != NULL);

		/* While the profile holds, the filter runs no frame. The record
		 * that eh_lec_state_wait takes after the profile ack holds the
		 * count that the 300 ms below must not move. */
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_OK);
		CHECK_UINT(stream_line_passthrough(t.s, 0),
			   UDSP_PASSTHROUGH_MODEM);
		if (ec)
			CHECK(eh_lec_state_wait(&t, &ls, false, false));
		held = ls.frames;
		eh_wait_ms(&t, 300);
		if (ec) {
			CHECK(udsp_line_lec_stats(t.s, 0, &ls) == UDSP_OK);
			CHECK_UINT(ls.frames, held);
		}

		/* The engine activates the stream and its ack alone goes
		 * missing, so the mark falls on a stream that is still active
		 * and still carries the profile: the hazard under test. */
		tm_arm_engine_drop(&t, STREAM_OP_ACTIVATE);
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_FATAL);
		if (st) {
			CHECK(atomic_load(&st->dead));
			CHECK(atomic_load(&st->active));
			CHECK_UINT(st->passthrough_kind,
				   UDSP_PASSTHROUGH_MODEM);
		}
		CHECK_UINT(stream_line_passthrough(t.s, 0),
			   UDSP_PASSTHROUGH_OFF);

		/* Stream 1 is healthy and still carries the line, so the
		 * canceller runs for it again. Turning it off is a SYNC-TICK
		 * op, and the record that eh_lec_state_wait takes describes the
		 * last frame the filter ran. */
		held = ls.frames;
		eh_wait_ms(&t, 300);
		ccfg.ec_enable = false;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		if (ec) {
			CHECK(eh_lec_state_wait(&t, &ls, false, false));
			CHECK(ls.frames > held + 10u);
		}

		CHECK(udsp_stream_stop(t.s, 0, 1) == UDSP_OK);
		tm_wire_close(&w);
		tm_call_close(&t, &w2);
	}
}

/* --- cases: the line canceller release whose ack is lost ----------------- */

/*
 * The tick answers UDSP_CMD_LEC_RELEASE: it moves ec to the mailbox and sets
 * ec to NULL. tm_arm_lec_drop then swallows the ack, so the sync of the stop
 * times out with the instance already retired. The reclaim must still harvest
 * the mailbox: ec is NULL (retired), and ec_retired is NULL (freed). A timeout
 * branch that returns without the harvest leaks the instance. The ec_retired
 * check has no ack edge, so it needs the release store
 * of the tick to be visible after the 100 ms wait. That holds on real hardware.
 */
static void case_lec_release_raced_timeout(void)
{
	struct udsp_chan_cfg ccfg;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;

	CASE("a released canceller is reclaimed when its ack times out");
	if (!tm_call_up(&t, "USA", &w, 0))
		return;
	CHECK(eh_hook_seize(&t));
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.ec_enable = true;
	ccfg.ec_nlp_enable = true;
	ccfg.ec_tail_ms = 32;
	CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
	tm_media(&m, UDSP_SEL_PCMU, 0, 10);

	tm_stream_config(&t, &w, &m);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	CHECK(atomic_load(&t.s->chan[0].ec) != NULL);

	tm_arm_lec_drop(&t);
	CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
	CHECK(atomic_load(&t.s->chan[0].ec) == NULL);
	CHECK(atomic_load(&t.s->chan[0].ec_retired) == NULL);

	/* The line runs a whole canceller lifecycle again after the raced stop. */
	tm_stream_config(&t, &w, &m);
	CHECK(atomic_load(&t.s->chan[0].ec) != NULL);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);

	tm_call_close(&t, &w);
}

/*
 * The same raced release under load. One stop in eight loses the LEC_RELEASE
 * ack after the tick retired the canceller, so the harvest runs on the timeout
 * path three times. The case checks that each start and stop succeeds. In a
 * sanitizer build, LSan reports a canceller that the harvest leaks, and TSan
 * reports a data race on the mailbox or the retire.
 */
static void case_lec_release_drop_stress(void)
{
	struct udsp_chan_cfg ccfg;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;

	CASE("repeated raced releases leak no canceller");
	if (!tm_call_up(&t, "USA", &w, 0))
		return;
	CHECK(eh_hook_seize(&t));
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.ec_enable = true;
	ccfg.ec_nlp_enable = true;
	ccfg.ec_tail_ms = 32;
	CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
	tm_media(&m, UDSP_SEL_PCMU, 0, 10);

	for (unsigned i = 0; i < 24u; i++) {
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 20);
		/* Arm the ack drop on every eighth pass so the harvest runs on
		 * the timeout path; the rest keep the case quick. */
		if (i % 8u == 0u)
			tm_arm_lec_drop(&t);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
	}

	tm_call_close(&t, &w);
}

/* --- cases: the line canceller a failed configuration made --------------- */

static void case_prep_lec(void)
{
	struct udsp_chan_cfg ccfg;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;

	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.ec_enable = true;
	ccfg.ec_tail_ms = 32;

	/*
	 * stream_lec_ensure publishes the canceller in the middle of the
	 * prepare, so a step after it that fails leaves the line running a
	 * canceller for a configuration that never happened, until the handle
	 * closes.
	 */
	CASE("a failed configuration takes back the canceller it published");
	{
		struct udsp_stream *st;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK(eh_hook_seize(&t));
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(atomic_load(&t.s->chan[0].ec) == NULL);

		atomic_store(&t.s->prep_fail, (uint_least32_t)ENOMEM);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		CHECK(udsp_stream_config(t.s, 0, 0, &m, w.rtp_local,
					 w.rtcp_local) == UDSP_TRANSIENT);
		CHECK(atomic_load(&t.s->chan[0].ec) == NULL);

		/* The stream itself is untouched and still usable, and the
		 * canceller comes back with the configuration that works. */
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			CHECK(st->rtp_fd < 0);
			CHECK(st->ops == NULL);
			CHECK(!atomic_load(&st->dead));
			CHECK_UINT(atomic_load(&st->state), STREAM_IDLE);
		}
		tm_stream_config(&t, &w, &m);
		CHECK(atomic_load(&t.s->chan[0].ec) != NULL);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		tm_call_down(&t, &w);
	}

	/* A canceller an earlier configuration made is not this one's to take:
	 * the line is still carrying a call on it. */
	CASE("a failed configuration keeps a canceller another stream uses");
	{
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p;
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK(eh_hook_seize(&t));
		/* Stream 0 comes up with no canceller, so the one stream 1
		 * publishes below is genuinely its own. */
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		CHECK(atomic_load(&t.s->chan[0].ec) == NULL);

		atomic_store(&t.s->prep_fail, (uint_least32_t)ENOMEM);
		CHECK(udsp_stream_config(t.s, 0, 1, &m, w.rtp_local,
					 w.rtcp_local) == UDSP_TRANSIENT);
		/* Stream 0 holds the line, so the canceller stays: releasing it
		 * would cut the echo canceller out of a live call. */
		CHECK(atomic_load(&t.s->chan[0].ec) != NULL);

		eh_wait_ms(&t, 60);
		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300);
		CHECK(n > 0);
		CHECK(n > 0 && tm_rtp_parse(buf, (size_t)n, &p));
		tm_call_down(&t, &w);
	}
}

static void case_flood(void)
{
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;

	CASE("a flooded ring drops the oldest datagrams and counts them");
	{
		struct udsp_media_stats stats;
		uint8_t pkt[TM_PKT_MAX];
		uint8_t pay[80] = { 0 };
		struct udsp_stream *st;
		unsigned sent = 0;

		if (!tm_call_up(&t, "USA", &w, 1 << 20))
			return;
		/* One tick every 20 ms of wall time, so a whole burst arrives
		 * between two drains of the ring. */
		atomic_store(&eh_be.pace_us, 20000u);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 30);

		for (unsigned i = 0; i < 400u; i++) {
			size_t len = tm_rtp_build(pkt, 0, i == 0, w.seq++, w.ts,
						  w.ssrc, pay, 80);

			w.ts += 80;
			if (send(w.rtp_peer, pkt, len,
				 MSG_DONTWAIT | MSG_NOSIGNAL) > 0)
				sent++;
		}
		CHECK(sent > 64u);
		eh_wait_ms(&t, 200);

		CHECK(udsp_stream_stats(t.s, 0, 0, &stats) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			uint64_t drops = atomic_load(&st->net_drops);
			uint64_t stale = atomic_load(&st->net_stale);

			/* The ring overflowed both ways: the net thread had to
			 * refuse datagrams and the tick shed the stale head. */
			CHECK(drops > 0);
			CHECK(stale > 0);
			CHECK(drops + stale + stats.packets_recv <= sent);
			CHECK(stats.packets_recv > 0);
		}
		tm_call_down(&t, &w);
	}
}

static void case_stats(void)
{
	struct udsp_media_stats a, b;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	double phase = 0.0;

	CASE("the statistics snapshot follows the stream");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK(udsp_stream_stats(t.s, 0, 0, &a) == UDSP_UNSUPPORTED);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		for (unsigned i = 0; i < 20; i++)
			tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
		eh_wait_ms(&t, 200);
		CHECK(udsp_stream_stats(t.s, 0, 0, &a) == UDSP_OK);
		for (unsigned i = 0; i < 20; i++)
			tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
		eh_wait_ms(&t, 300);
		CHECK(udsp_stream_stats(t.s, 0, 0, &b) == UDSP_OK);

		CHECK(a.packets_recv > 0);
		CHECK(b.packets_recv > a.packets_recv);
		CHECK(b.packets_sent > a.packets_sent);
		CHECK(b.bytes_recv > a.bytes_recv);
		CHECK(b.mos_lq > 3.0f);
		CHECK(b.rfactor > 50u);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_stream_stats(t.s, 0, 0, &b) == UDSP_UNSUPPORTED);
		tm_call_close(&t, &w);
	}

	/*
	 * The E-model costs a pow and a log per call, so the snapshot is
	 * republished at a diagnostic cadence, not every tick (stream.h
	 * UDSP_STATS_PERIOD_MS). The gap between two republishes is the period,
	 * measured in the engine's own time base: a shorter one puts the model
	 * back on the tick and a longer one blunts the diagnostic.
	 */
	CASE("the statistics snapshot republishes at its own period");
	{
		uint32_t min_gap = UINT32_MAX;
		uint32_t last_ms = 0;
		uint32_t end_ms = 0;
		uint64_t last_sent = 0;
		unsigned changes = 0;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);
		CHECK(udsp_stream_stats(t.s, 0, 0, &a) == UDSP_OK);
		last_sent = a.packets_sent;
		last_ms = eh_now_ms();
		end_ms = last_ms + 2000u;

		while (eh_now_ms() < end_ms) {
			uint32_t now;

			eh_sleep_ms(1);
			if (udsp_stream_stats(t.s, 0, 0, &a) != UDSP_OK)
				break;
			now = eh_now_ms();
			if (a.packets_sent == last_sent)
				continue;
			if (changes && now - last_ms < min_gap)
				min_gap = now - last_ms;
			last_sent = a.packets_sent;
			last_ms = now;
			changes++;
		}
		CHECK(changes > 4u);
		/* One packet leaves per tick at ptime 10, so the gap the
		 * sampler sees is the republish period itself. */
		CHECK(min_gap >= 50u);
		CHECK(min_gap <= 150u);
		tm_call_down(&t, &w);
	}

	/*
	 * The run clock starts at 0 at open and wraps at 2^32 ms. A stream
	 * adopted in the upper half of that range must keep its 100 ms cadence,
	 * and so must a call that runs across the wrap. The engine start takes
	 * its clock base from the tick it finds, so a stop, a stored tick and a
	 * start carry the clock there. On the second tick the case waits past
	 * the wrap before its first read, so the second read must come from a
	 * publish after the wrap.
	 */
	CASE("the statistics snapshot moves in the upper clock half");
	{
		static const uint64_t ticks[] = { 300000000u, 429496680u };

		for (unsigned k = 0; k < 2; k++) {
			struct udsp_job job = { 0 };
			int32_t status = -1;

			if (!tm_call_up(&t, "USA", &w, 0))
				return;
			CHECK(udsp_stop(t.s, &job) == UDSP_OK);
			CHECK(eh_job_wait(&t, job.token, 5000, &status));
			CHECK_UINT((uint32_t)status, UDSP_OK);
			atomic_store_explicit(&t.s->clock.tick, ticks[k],
					      memory_order_release);
			status = -1;
			CHECK(udsp_start(t.s, &job) == UDSP_OK);
			CHECK(eh_job_wait(&t, job.token, 5000, &status));
			CHECK_UINT((uint32_t)status, UDSP_OK);
			eh_wait_ms(&t, 20);
			CHECK(udsp_now_ms(t.s) >= 0x80000000u);

			tm_media(&m, UDSP_SEL_PCMU, 0, 10);
			tm_stream_config(&t, &w, &m);
			tm_capture_tone(1000u);
			CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
			for (unsigned i = 0; i < 20; i++)
				tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
			eh_wait_ms(&t, 200);
			for (unsigned i = 0; k && i < 100 &&
			     udsp_now_ms(t.s) >= 0x80000000u; i++)
				eh_wait_ms(&t, 10);
			CHECK((udsp_now_ms(t.s) >= 0x80000000u) == !k);
			CHECK(udsp_stream_stats(t.s, 0, 0, &a) == UDSP_OK);
			for (unsigned i = 0; i < 20; i++)
				tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
			eh_wait_ms(&t, 300);
			CHECK(udsp_stream_stats(t.s, 0, 0, &b) == UDSP_OK);
			CHECK((udsp_now_ms(t.s) >= 0x80000000u) == !k);

			CHECK(b.packets_recv > a.packets_recv);
			CHECK(b.packets_sent > a.packets_sent);
			tm_call_down(&t, &w);
		}
	}
}

/* --- cases: the jitter buffer snapshot ----------------------------------- */

static void case_jb_stats(void)
{
	struct udsp_jb_stats a, b;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	double phase = 0.0;

	CASE("the jitter buffer snapshot follows the stream");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &a) == UDSP_UNSUPPORTED);
		CHECK(udsp_stream_jb_stats(t.s, 1, 0, &a) == UDSP_UNSUPPORTED);
		CHECK(udsp_stream_jb_stats(t.s, 8, 0, &a) == UDSP_UNSUPPORTED);
		CHECK(udsp_stream_jb_stats(t.s, 0, UDSP_STREAM_MAX, &a) ==
		      UDSP_UNSUPPORTED);
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, NULL) == UDSP_UNSUPPORTED);

		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.jb_init_ms = 40;
		tm_stream_config(&t, &w, &m);
		/* Adopted but never ticked: the slot holds no counters of
		 * this configuration yet, so the getter refuses. */
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &a) == UDSP_UNSUPPORTED);

		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 40);
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &a) == UDSP_OK);
		CHECK_UINT(tm_jb_drops(&a), 0);
		CHECK_UINT(a.target_ms, 40);
		for (unsigned burst = 0; burst < 4u; burst++) {
			for (unsigned i = 0; i < 25u; i++)
				tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
			eh_wait_ms(&t, 250);
		}
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &b) == UDSP_OK);
		CHECK(b.frames_out > 0);
		CHECK(tm_jb_units(&b) > 0);

		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &b) == UDSP_UNSUPPORTED);
		tm_call_close(&t, &w);
	}

	/*
	 * A slot that already ran a call keeps that call's counters until a
	 * tick of the new configuration publishes, and snap.h can skip that
	 * publish. The engine may not spin, so the getter refuses until the
	 * publish arrives.
	 */
	CASE("a readopted stream serves no counters until its first tick");
	{
		uint64_t first_units = 0;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.jb_init_ms = 40;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		for (unsigned burst = 0; burst < 4u; burst++) {
			for (unsigned i = 0; i < 25u; i++)
				tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
			eh_wait_ms(&t, 250);
		}
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &a) == UDSP_OK);
		first_units = tm_jb_units(&a);
		/* One output unit per tick, so a second of engine time. */
		CHECK(first_units > 50u);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);

		/* The same slot, a new call, a different target. */
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.jb_init_ms = 80;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &b) == UDSP_UNSUPPORTED);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 40);
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &b) == UDSP_OK);
		/* The counters are this call's, not the previous call's. */
		CHECK(tm_jb_units(&b) < first_units);
		CHECK_UINT(b.target_ms, 80);
		tm_call_down(&t, &w);
	}

	/*
	 * Every udsp_jb_get produces exactly one output unit (jb.h Event
	 * mapping), so a snapshot published on every tick advances the
	 * partition sum by exactly one per tick. A slower cadence would show
	 * the sampler a step of several units instead.
	 */
	CASE("the jitter buffer snapshot advances one output unit per tick");
	{
		uint64_t last = 0;
		unsigned changes = 0;
		unsigned ones = 0;
		uint64_t stop;
		uint32_t end;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		/* One tick per 10 ms of wall time, so a sampler that polls
		 * every millisecond sees every publish. */
		atomic_store(&eh_be.pace_us, 10000u);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 40);
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &a) == UDSP_OK);
		last = tm_jb_units(&a);
		end = eh_now_ms() + 600u;
		stop = eh_mono_us() + 10000000u;

		while (eh_now_ms() < end && eh_mono_us() < stop) {
			uint64_t units;

			eh_sleep_ms(1);
			eh_ev_poll(&t);
			if (udsp_stream_jb_stats(t.s, 0, 0, &a) != UDSP_OK)
				break;
			units = tm_jb_units(&a);
			if (units == last)
				continue;
			changes++;
			if (units == last + 1u)
				ones++;
			last = units;
		}
		CHECK(changes > 20u);
		CHECK(ones > 20u);
		tm_call_down(&t, &w);
	}

	/*
	 * The event and the snapshot are filled from one statistics copy per
	 * tick, so an update the engine sent between two reads carries a total
	 * that lies between them. A second source for either would break the
	 * bracket as soon as the two drifted apart.
	 */
	CASE("the jitter buffer update event brackets between two snapshots");
	{
		uint64_t units_a, units_b, drops_a, drops_b;
		unsigned mark;
		bool inside = false;
		bool above = false;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		for (unsigned burst = 0; burst < 6u; burst++) {
			for (unsigned i = 0; i < 25u; i++)
				tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
			eh_wait_ms(&t, 250);
		}
		eh_ev_poll(&t);
		mark = eh_ev_len;
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &a) == UDSP_OK);
		units_a = tm_jb_units(&a);
		drops_a = tm_jb_drops(&a);

		for (unsigned burst = 0; burst < 10u; burst++) {
			for (unsigned i = 0; i < 25u; i++)
				tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
			eh_wait_ms(&t, 250);
		}
		eh_ev_poll(&t);
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &b) == UDSP_OK);
		units_b = tm_jb_units(&b);
		drops_b = tm_jb_drops(&b);
		/* The snapshot is republished while packets flow. */
		CHECK(units_b > units_a);

		for (unsigned i = mark; i < eh_ev_len; i++) {
			if (eh_ev[i].type != UDSP_EV_JB_UPDATE)
				continue;
			if (eh_ev[i].u.jb.total > units_b ||
			    eh_ev[i].u.jb.drop > drops_b)
				continue;
			inside = true;
			if (eh_ev[i].u.jb.total >= units_a &&
			    eh_ev[i].u.jb.drop >= drops_a)
				above = true;
		}
		CHECK(inside);
		CHECK(above);
		tm_call_down(&t, &w);
	}
}

/* --- cases: the decision trace ------------------------------------------- */

static void case_jb_trace(void)
{
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	double phase = 0.0;

	CASE("the trace stays empty while the debug flag is clear");
	{
		uint16_t hold_seq;
		uint32_t hold_ts;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		hold_seq = w.seq;
		hold_ts = w.ts;
		for (unsigned i = 0; i < 25u; i++) {
			if (i == 5u) {
				hold_seq = w.seq++;
				hold_ts = w.ts;
				w.ts += 80u;
				continue;
			}
			tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
		}
		eh_wait_ms(&t, 150);
		tm_pkt_send_at(&w, 0, 10, hold_seq, hold_ts);
		eh_wait_ms(&t, 100);
		CHECK_UINT(tm_trace_read(&t), 0);
		tm_call_down(&t, &w);
	}

	CASE("the first packet of a stream is traced as an anchor");
	{
		const struct udsp_jb_trace *rec;
		uint16_t seq;
		uint32_t ts;
		unsigned n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		udsp_debug_set(t.s, UDSP_DEBUG_JB_TRACE);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 30);
		/* RFC 3550 A.1 keeps the first packet of a source on
		 * probation until the second proves the source. The receiver
		 * then releases the held packet first, and the buffer anchors
		 * on it (rtp.c receiver_hold). */
		seq = w.seq;
		ts = w.ts;
		tm_burst_send(&w, 2);
		eh_wait_ms(&t, 60);

		n = tm_trace_read(&t);
		CHECK(n > 0);
		CHECK(tm_trace_shape(n, 0, 0));
		rec = tm_trace_rc(n, UDSP_JB_TRACE_PUT, 0);
		CHECK(rec != NULL);
		if (rec) {
			CHECK_UINT(rec->seq, seq);
			CHECK_UINT(rec->pkt_ts, ts);
			CHECK(rec->flags & UDSP_JB_TRACE_ANCHOR);
			CHECK_UINT(rec->flags & (UDSP_JB_TRACE_RESYNC |
						 UDSP_JB_TRACE_STARVE |
						 UDSP_JB_TRACE_REFILL |
						 UDSP_JB_TRACE_CN_REANCHOR), 0);
			CHECK_UINT(rec->state_before, 0);
			CHECK_UINT(rec->state_after, 1);
			CHECK_UINT(rec->count, 1);
		}
		udsp_debug_set(t.s, 0);
		tm_call_down(&t, &w);
	}

	CASE("a packet delivered after its successor is traced as a late drop");
	{
		const struct udsp_jb_trace *rec;
		uint16_t hold_seq = 0;
		uint32_t hold_ts = 0;
		unsigned n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		udsp_debug_set(t.s, UDSP_DEBUG_JB_TRACE);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		/* Hold one packet back and deliver every successor, so the
		 * playout point is past it by the time it arrives. */
		for (unsigned i = 0; i < 25u; i++) {
			if (i == 5u) {
				hold_seq = w.seq++;
				hold_ts = w.ts;
				w.ts += 80u;
				continue;
			}
			tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
		}
		eh_wait_ms(&t, 150);
		tm_trace_flush(&t);
		tm_pkt_send_at(&w, 0, 10, hold_seq, hold_ts);
		eh_wait_ms(&t, 60);

		n = tm_trace_read(&t);
		CHECK(n > 0);
		CHECK(tm_trace_shape(n, 0, 0));
		rec = tm_trace_rc(n, UDSP_JB_TRACE_PUT, -ETIME);
		CHECK(rec != NULL);
		if (rec) {
			CHECK_UINT(rec->seq, hold_seq);
			CHECK_UINT(rec->pkt_ts, hold_ts);
			CHECK(rec->lateness_ts > 0);
			CHECK_UINT(rec->flags & UDSP_JB_TRACE_ANCHOR, 0);
			CHECK_UINT(rec->state_before, 2);
			CHECK_UINT(rec->state_after, 2);
		}
		udsp_debug_set(t.s, 0);
		tm_call_down(&t, &w);
	}

	CASE("an outage parks the clock and the next packet refills the buffer");
	{
		const struct udsp_jb_trace *rec;
		unsigned n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		udsp_debug_set(t.s, UDSP_DEBUG_JB_TRACE);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		for (unsigned i = 0; i < 25u; i++)
			tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
		/* Long enough for the queue to run dry, short enough that the
		 * starvation re-anchor of jb.c (one second) never arms. */
		eh_wait_ms(&t, 450);

		n = tm_trace_read(&t);
		CHECK(n > 0);
		CHECK(tm_trace_shape(n, 0, 0));
		rec = tm_trace_flag(n, UDSP_JB_TRACE_GET, UDSP_JB_TRACE_PARKED);
		CHECK(rec != NULL);
		if (rec) {
			CHECK_UINT(rec->rc, UDSP_JB_PLC);
			CHECK(rec->flags & UDSP_JB_TRACE_UNDERRUN);
			CHECK_UINT(rec->count, 0);
		}

		tm_burst_send(&w, 1);
		eh_wait_ms(&t, 60);
		n = tm_trace_read(&t);
		CHECK(n > 0);
		rec = tm_trace_flag(n, UDSP_JB_TRACE_PUT, UDSP_JB_TRACE_REFILL);
		CHECK(rec != NULL);
		if (rec) {
			CHECK_UINT(rec->rc, 0);
			CHECK_UINT(rec->flags & (UDSP_JB_TRACE_STARVE |
						 UDSP_JB_TRACE_RESYNC |
						 UDSP_JB_TRACE_ANCHOR), 0);
			CHECK_UINT(rec->state_before, 2);
			CHECK_UINT(rec->state_after, 1);
		}
		udsp_debug_set(t.s, 0);
		tm_call_down(&t, &w);
	}

	CASE("steady playout after the first frame leaves the trace quiet");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		/* One tick per 10 ms of wall time, so one packet per wait is
		 * one packet per tick. */
		atomic_store(&eh_be.pace_us, 10000u);
		udsp_debug_set(t.s, UDSP_DEBUG_JB_TRACE);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		/* The prefill ends exactly at the target, because the queue
		 * grows one frame per tick, and the trickle then holds the
		 * depth there. So the clock skew rule of jb.c arms nothing
		 * and the queue never runs dry, and the only records are the
		 * ones the start of the stream makes. */
		m.jb_init_ms = 60;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		for (unsigned i = 0; i < 20u; i++) {
			tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
			eh_wait_ms(&t, 10);
		}
		tm_trace_flush(&t);
		for (unsigned i = 0; i < 40u; i++) {
			tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
			eh_wait_ms(&t, 10);
		}
		CHECK_UINT(tm_trace_read(&t), 0);
		udsp_debug_set(t.s, 0);
		tm_call_down(&t, &w);
	}

	/*
	 * The heartbeat belongs inside a concealment run. A buffer that has not
	 * started plays silence and a buffer the far end put into comfort noise
	 * plays comfort noise; jb.h calls neither concealment and both repeat
	 * one decision for as long as they last, so a beat there would be noise
	 * a reader has to filter out.
	 */
	CASE("a stream with no packets at all leaves the trace quiet");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		udsp_debug_set(t.s, UDSP_DEBUG_JB_TRACE);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);
		tm_trace_flush(&t);
		/* More than two heartbeat periods of idle silence. */
		eh_wait_ms(&t, 2500);
		CHECK_UINT(tm_trace_read(&t), 0);
		udsp_debug_set(t.s, 0);
		tm_call_down(&t, &w);
	}

	CASE("a run through comfort noise leaves the trace quiet");
	{
		uint8_t pkt[TM_PKT_MAX];
		uint8_t level = 40;
		size_t len;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		udsp_debug_set(t.s, UDSP_DEBUG_JB_TRACE);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.cng = true;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		for (unsigned i = 0; i < 25u; i++)
			tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
		eh_wait_ms(&t, 100);
		/* The far end closes the talkspurt: RFC 3389 comfort noise
		 * covers the silence and the media clock keeps running. */
		len = tm_rtp_build(pkt, 13, false, w.seq++, w.ts, w.ssrc,
				   &level, 1);
		w.ts += 80u;
		CHECK(send(w.rtp_peer, pkt, len, MSG_DONTWAIT | MSG_NOSIGNAL) > 0);
		eh_wait_ms(&t, 400);
		tm_trace_flush(&t);
		/* More than two heartbeat periods of comfort noise. */
		eh_wait_ms(&t, 2500);
		CHECK_UINT(tm_trace_read(&t), 0);
		{
			struct udsp_jb_stats js;

			/* The run really was comfort noise, not concealment. */
			CHECK(udsp_stream_jb_stats(t.s, 0, 0, &js) == UDSP_OK);
			CHECK(js.cn > 200u);
		}
		udsp_debug_set(t.s, 0);
		tm_call_down(&t, &w);
	}

	CASE("a long concealment run keeps beating once a second");
	{
		const struct udsp_jb_trace *rec;
		unsigned beats = 0;
		unsigned n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		udsp_debug_set(t.s, UDSP_DEBUG_JB_TRACE);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		for (unsigned i = 0; i < 25u; i++)
			tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
		/* The far end stops for good: the queue runs dry, the clock
		 * parks and the buffer conceals from there on. No comfort
		 * noise was ever stored, so the run ends in silence. */
		eh_wait_ms(&t, 3000);

		n = tm_trace_read(&t);
		CHECK(n > 0);
		CHECK(tm_trace_shape(n, 0, 0));
		rec = tm_trace_flag(n, UDSP_JB_TRACE_GET,
				    UDSP_JB_TRACE_HEARTBEAT);
		CHECK(rec != NULL);
		if (rec) {
			CHECK(rec->rc == UDSP_JB_PLC || rec->rc == UDSP_JB_SILENCE);
			CHECK(rec->conceal_run_ms > 0);
			CHECK(rec->flags & UDSP_JB_TRACE_PARKED);
		}
		for (unsigned i = 0; i < n; i++) {
			if (tm_trace[i].flags & UDSP_JB_TRACE_HEARTBEAT)
				beats++;
		}
		/* Roughly one per second of the concealed run. */
		CHECK(beats >= 2u);
		CHECK(beats <= 4u);
		udsp_debug_set(t.s, 0);
		tm_call_down(&t, &w);
	}

	/*
	 * A full ring refuses the new record. The sequence number of a put
	 * record is the identity that tells that apart from a ring that
	 * overwrites the oldest: the records that survive are the first ones
	 * pushed, and the ones the run made last are the ones missing. The
	 * loss count then rides the first record stored after the drain freed
	 * space, and no record in the middle of a run carries one.
	 */
	CASE("a full trace ring drops the newest record and counts it");
	{
		unsigned first, second;
		unsigned puts = 0;
		unsigned mid_loss = 0;
		unsigned tail_loss = 0;
		uint16_t seq_start, seq_end;
		uint16_t seq_low = 0, seq_high = 0;
		bool ordered = true;

		if (!tm_call_up(&t, "USA", &w, 1 << 20))
			return;
		/* One tick per 5 ms of wall time, so the net thread carries a
		 * whole drain bound of packets between two ticks. */
		atomic_store(&eh_be.pace_us, 5000u);
		udsp_debug_set(t.s, UDSP_DEBUG_JB_TRACE);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 40);
		tm_trace_flush(&t);
		seq_start = w.seq;
		/* UDSP_NET_DRAIN_MAX packets per tick fill the buffer's 64
		 * slots in a few ticks; every packet after that is refused
		 * and traced. */
		for (unsigned i = 0; i < 60u; i++) {
			tm_burst_send(&w, UDSP_NET_DRAIN_MAX);
			eh_wait_ms(&t, 10);
		}
		seq_end = w.seq;
		/* Disarm first: with the producer still running, a read could
		 * pop records the same run pushed behind it. */
		udsp_debug_set(t.s, 0);
		eh_wait_ms(&t, 40);
		first = tm_trace_read(&t);
		CHECK(first > 400u);
		CHECK(first <= UDSP_JB_TRACE_SLOTS - 1u);
		CHECK(tm_trace_shape(first, 0, 0));

		for (unsigned i = 0; i < first; i++) {
			if (tm_trace[i].lost_before)
				mid_loss++;
			if (tm_trace[i].kind != UDSP_JB_TRACE_PUT)
				continue;
			if (!puts)
				seq_low = tm_trace[i].seq;
			else if ((int16_t)(tm_trace[i].seq - seq_high) <= 0)
				ordered = false;
			seq_high = tm_trace[i].seq;
			puts++;
		}
		CHECK(puts > 400u);
		/* Oldest first, and every one of them from this run. */
		CHECK(ordered);
		CHECK((int16_t)(seq_low - seq_start) >= 0);
		/* The survivors sit at the start of the run: the newest ones
		 * are gone. An overwrite-oldest ring would leave seq_high at
		 * the very end of the run instead. */
		CHECK((int16_t)(seq_low - seq_start) < 400);
		CHECK((int16_t)(seq_end - seq_high) > 200);
		/* The ring filled from empty, so nothing inside it inherited
		 * a loss: the count waits for the next record that fits. */
		CHECK_UINT(mid_loss, 0);

		udsp_debug_set(t.s, UDSP_DEBUG_JB_TRACE);
		for (unsigned i = 0; i < 5u; i++) {
			tm_burst_send(&w, UDSP_NET_DRAIN_MAX);
			eh_wait_ms(&t, 10);
		}
		udsp_debug_set(t.s, 0);
		eh_wait_ms(&t, 40);
		second = tm_trace_read(&t);
		CHECK(second > 0);
		CHECK(second < UDSP_JB_TRACE_SLOTS - 1u);
		if (second) {
			CHECK(tm_trace[0].lost_before > 0);
			/* Every record the run dropped is accounted for on
			 * that one record and nowhere else. */
			CHECK(tm_trace[0].lost_before > 100u);
			CHECK((int16_t)(tm_trace[0].seq - seq_high) > 0);
		}
		for (unsigned i = 1; i < second; i++) {
			if (tm_trace[i].lost_before)
				tail_loss++;
		}
		CHECK_UINT(tail_loss, 0);
		tm_call_down(&t, &w);
	}

	CASE("clearing the debug flag stops the trace");
	{
		unsigned n;

		udsp_debug_set(NULL, UDSP_DEBUG_JB_TRACE);
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		udsp_debug_set(t.s, UDSP_DEBUG_JB_TRACE);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		for (unsigned i = 0; i < 25u; i++)
			tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
		eh_wait_ms(&t, 300);
		n = tm_trace_read(&t);
		CHECK(n > 0);

		udsp_debug_set(t.s, 0);
		eh_wait_ms(&t, 40);
		tm_trace_flush(&t);
		for (unsigned burst = 0; burst < 3u; burst++) {
			for (unsigned i = 0; i < 25u; i++)
				tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
			eh_wait_ms(&t, 250);
		}
		CHECK_UINT(tm_trace_read(&t), 0);
		tm_call_down(&t, &w);
	}
}

/* --- cases: the board's PCM offset --------------------------------------- */

static void case_pcm_offset(void)
{
	struct eh t;

	CASE("udsp_open keeps a zero PCM offset in the handle configuration");
	{
		/* Zero means "the plug-in profile default" (slic.h): the core
		 * must not replace it, or the an7581 profile could never
		 * reach its bit 0 slot. The handle keeps the board value the
		 * host block carries into the plug-in; the in-tree fake takes a
		 * NULL host and ignores it. test_slic_host pins the host block,
		 * test_plugin the resolution. */
		if (!tm_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK_UINT(t.s->cfg.pcm_offset_bits, 0);
		tm_down(&t);
	}
}

/* --- cases: job completion status ---------------------------------------- */

static void case_job_status(void)
{
	struct udsp_job job = { 0 };
	int32_t status = -1;
	struct eh t;

	CASE("a job completion carries a status, never an errno");
	{
		/* uvoice main.c casts this field to enum hal_status, so the
		 * whole table has to fall inside 0..3. */
		CHECK_UINT(cmd_job_status_map(0), UDSP_OK);
		CHECK_UINT(cmd_job_status_map(-ETIMEDOUT), UDSP_TRANSIENT);
		CHECK_UINT(cmd_job_status_map(-EAGAIN), UDSP_TRANSIENT);
		CHECK_UINT(cmd_job_status_map(-EBUSY), UDSP_TRANSIENT);
		CHECK_UINT(cmd_job_status_map(-EINVAL), UDSP_UNSUPPORTED);
		CHECK_UINT(cmd_job_status_map(-ENOTSUP), UDSP_UNSUPPORTED);
		CHECK_UINT(cmd_job_status_map(-ENOENT), UDSP_UNSUPPORTED);
		CHECK_UINT(cmd_job_status_map(-EIO), UDSP_FATAL);
		CHECK_UINT(cmd_job_status_map(-EPERM), UDSP_FATAL);
		/* A superseded start failed at nothing. */
		CHECK_UINT(cmd_job_status_map(-ECANCELED), UDSP_TRANSIENT);
	}

	CASE("a start a stop supersedes completes transient");
	{
		bool superseded = false;

		if (!tm_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		/*
		 * A stop supersedes a start still in the queue, and one already
		 * executing completes first. Which of the two happened is a
		 * scheduling question, so the loop runs until the queued case
		 * appears and every attempt is checked either way.
		 */
		for (unsigned i = 0; i < 20u && !superseded; i++) {
			struct udsp_job start = { 0 };
			struct udsp_job stop = { 0 };
			int32_t started = -1;
			int32_t stopped = -1;

			if (udsp_stop(t.s, &stop) != UDSP_OK ||
			    !eh_job_wait(&t, stop.token, 5000, &stopped)) {
				CHECK(false);
				break;
			}
			CHECK(udsp_start(t.s, &start) == UDSP_OK);
			CHECK(udsp_stop(t.s, &stop) == UDSP_OK);
			CHECK(eh_job_wait(&t, start.token, 5000, &started));
			CHECK(eh_job_wait(&t, stop.token, 5000, &stopped));
			/* The superseding stop finds the handle already safe, so
			 * it may answer TRANSIENT for the engine ack it cannot
			 * get; what it must never be is a hardware failure. */
			CHECK(stopped != UDSP_FATAL);
			if (started == UDSP_TRANSIENT)
				superseded = true;
			else
				CHECK_UINT((uint32_t)started, UDSP_OK);
		}
		CHECK(superseded);
		t.stopped = true;
		tm_down(&t);
	}

	CASE("a missing region file completes unsupported");
	{
		if (!tm_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		status = -1;
		CHECK(udsp_region_load(t.s, "/nonexistent/XXX.conf", &job) ==
		      UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 3000, &status));
		CHECK_UINT((uint32_t)status, UDSP_UNSUPPORTED);
		/* A job that worked still says so with the same enum. */
		status = -1;
		CHECK(udsp_region_load(t.s, "ETS", &job) == UDSP_OK);
		CHECK(eh_job_wait(&t, job.token, 3000, &status));
		CHECK_UINT((uint32_t)status, UDSP_OK);
		tm_down(&t);
	}
}

/* --- cases: the event sequence ------------------------------------------- */

/* One telephone event from the wire, reported at once by its end packet. */
static void tm_wire_digit(struct tm_wire *w, uint8_t event)
{
	uint8_t pkt[TM_PKT_MAX];
	uint8_t pay[4];
	size_t len;

	pay[0] = event;
	pay[1] = 10 | 0x80u;
	pay[2] = 0;
	pay[3] = 80;
	len = tm_rtp_build(pkt, 101, false, w->seq++, w->ts, w->ssrc, pay, 4);
	w->ts += 800;
	send(w->rtp_peer, pkt, len, MSG_DONTWAIT | MSG_NOSIGNAL);
}

/* The emitter thread runs on wall time while eh_wait_ms counts engine
 * milliseconds, so a case that waits for an event drains in wall time. */
static void tm_ev_drain(struct eh *t, unsigned wall_ms)
{
	for (unsigned i = 0; i < wall_ms; i++) {
		eh_ev_poll(t);
		eh_sleep_ms(1);
	}
	eh_ev_poll(t);
}

/* The gap the consumer's rule sees: every sequence number that never arrived,
 * counted in 16-bit space, completions excluded (they carry seq 0). */
static unsigned tm_seq_gaps(void)
{
	unsigned gaps = 0;
	unsigned prev = 0;
	bool have = false;

	for (unsigned i = 0; i < eh_ev_len; i++) {
		unsigned seq = eh_ev[i].seq;

		if (eh_ev[i].type == UDSP_EV_JOB_DONE)
			continue;
		if (have)
			gaps += (unsigned)((uint16_t)(seq - prev) - 1u);
		prev = seq;
		have = true;
	}

	return gaps;
}

static void case_event_seq(void)
{
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;

	CASE("hardware events carry consecutive sequence numbers");
	{
		struct udsp_counters ctr;

		if (!tm_call_up(&t, "USA", &w, 1 << 20))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 40);
		eh_ev_reset();

		for (unsigned i = 0; i < 40u; i++) {
			tm_wire_digit(&w, (uint8_t)(i % 10u));
			eh_wait_ms(&t, 20);
		}
		tm_ev_drain(&t, 200);

		/* The floor gives sequence and loss checks a nonempty sample;
		 * it does not prove that every ingress digit arrived. */
		CHECK(eh_ev_count(UDSP_EV_DIGIT) >= 30u);
		udsp_counters(t.s, &ctr);
		CHECK_UINT(ctr.events_dropped, 0);
		CHECK_UINT(tm_seq_gaps(), 0);
		tm_call_down(&t, &w);
	}

	CASE("a new source reports an event the old source's key would swallow");
	{
		unsigned first = 0;
		unsigned second = 0;
		double phase = 0.0;
		uint16_t seq;
		uint32_t ts;

		if (!tm_call_up(&t, "USA", &w, 1 << 20))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 40);
		eh_ev_reset();

		/* The far end's timeline, which the second source repeats. */
		seq = w.seq;
		ts = w.ts;
		tm_tone_send(&w, 0, 10, 1000.0, 0.2, &phase);
		tm_tone_send(&w, 0, 10, 1000.0, 0.2, &phase);
		tm_wire_digit(&w, 5);
		tm_ev_drain(&t, 150);
		for (unsigned i = 0; i < eh_ev_len; i++) {
			if (eh_ev[i].type == UDSP_EV_DIGIT)
				first++;
		}
		CHECK_UINT(first, 1);

		/*
		 * The same event code at the same timestamp from a different
		 * source. Every key the telephone-event receiver holds is
		 * (code, timestamp), so a state that outlived the old source
		 * reads this valid digit as one it already reported.
		 */
		eh_ev_reset();
		w.ssrc = 0x5a5a5a5au;
		w.seq = seq;
		w.ts = ts;
		tm_tone_send(&w, 0, 10, 1000.0, 0.2, &phase);
		tm_tone_send(&w, 0, 10, 1000.0, 0.2, &phase);
		tm_wire_digit(&w, 5);
		tm_ev_drain(&t, 150);
		for (unsigned i = 0; i < eh_ev_len; i++) {
			if (eh_ev[i].type == UDSP_EV_DIGIT)
				second++;
		}
		CHECK_UINT(second, 1);
		tm_call_down(&t, &w);
	}

	CASE("an event lost at a full producer ring still consumes its number");
	{
		struct udsp_counters ctr;
		struct udsp_job job = { 0 };
		int small = 2048;
		unsigned before;

		if (!tm_call_up(&t, "USA", &w, 1 << 20))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 40);
		eh_ev_reset();

		/* One event arrives cleanly first, so every later loss has a
		 * predecessor to be a gap against. */
		tm_wire_digit(&w, 1);
		tm_ev_drain(&t, 100);
		before = eh_ev_len;
		CHECK(before > 0);

		/*
		 * Fill the socket first, so the completion posted next cannot
		 * be written: the emitter then parks in its class (b) retry
		 * loop (hal.h) and the engine's own ring is what fills behind
		 * it. A completion posted before the socket is full would be
		 * delivered at once and the emitter would never park.
		 */
		setsockopt(t.tx_fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
		for (unsigned i = 0; i < 60u; i++)
			tm_wire_digit(&w, (uint8_t)(i % 10u));
		eh_sleep_ms(40);
		CHECK(udsp_region_load(t.s, "ETS", &job) == UDSP_OK);
		eh_sleep_ms(40);
		for (unsigned burst = 0; burst < 40u; burst++) {
			for (unsigned i = 0; i < 30u; i++)
				tm_wire_digit(&w, (uint8_t)(i % 10u));
			eh_sleep_ms(2);
		}
		eh_sleep_ms(200);

		/* Let the emitter go: the completion arrives, then the rest. */
		tm_ev_drain(&t, 400);
		udsp_counters(t.s, &ctr);
		CHECK(ctr.events_dropped > 0);
		/* The hal transport contract: what the core cannot see as a gap
		 * it cannot count as a loss. */
		CHECK_UINT(tm_seq_gaps(), ctr.events_dropped);
		tm_call_down(&t, &w);
	}
}

/* --- cases: the event descriptor ----------------------------------------- */

struct tm_child_page {
	bool opened;
	bool closed;
	bool dup_live;
	int dup_fd;
};

static struct tm_child_page *tm_child;
static int tm_wedge_pipe[2] = { -1, -1 };

/* The child abandons a wedged engine thread on purpose, so it leaves through
 * _exit and LeakSanitizer never runs on the leaked graph. */
static void tm_child_leak(void)
{
	struct udsp_job job = { 0 };
	int32_t status = 0;
	struct eh t;

	if (!tm_up(&t, "USA"))
		_exit(2);
	tm_child->opened = true;
	tm_child->dup_fd = t.s->event_dup;

	atomic_store(&eh_be.block_fd, tm_wedge_pipe[0]);
	atomic_store(&eh_be.block_after, atomic_load(&eh_be.reads));
	if (udsp_stop(t.s, &job) == UDSP_OK)
		eh_job_wait(&t, job.token, 4000, &status);
	udsp_close(t.s);
	tm_child->closed = true;
	/* The engine thread outlived the stop, so the descriptor it could still
	 * write must never be handed back to the process. */
	tm_child->dup_live = fcntl(tm_child->dup_fd, F_GETFD) >= 0;
	_exit(0);
}

static void case_event_fd(void)
{
	struct eh t;

	CASE("a closed handle gives the event descriptor number back");
	{
		int dup_fd;
		int marker[2] = { -1, -1 };
		char sink[64];

		if (!tm_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		dup_fd = t.s->event_dup;
		/* libudsp writes its own descriptor, never the caller's. */
		CHECK(dup_fd >= 0);
		CHECK(dup_fd != t.tx_fd);
		CHECK(t.s->event_fd == t.tx_fd);
		eh_wait_ms(&t, 40);
		tm_down(&t);

		CHECK(fcntl(dup_fd, F_GETFD) < 0);
		CHECK(errno == EBADF);
		if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0,
			       marker) == 0) {
			int placed = dup2(marker[1], dup_fd);

			CHECK_UINT(placed, (unsigned)dup_fd);
			eh_sleep_ms(100);
			CHECK(recv(marker[0], sink, sizeof(sink),
				   MSG_DONTWAIT) < 0);
			CHECK(errno == EAGAIN);
			if (placed >= 0)
				close(placed);
			close(marker[0]);
			close(marker[1]);
		} else {
			CHECK(false);
		}
	}

	CASE("a leaked emitter keeps its event descriptor for ever");
	{
		pid_t pid;
		int wst = 0;

		memset(tm_child, 0, sizeof(*tm_child));
		if (pipe(tm_wedge_pipe)) {
			CHECK(false);
			return;
		}
		/* The child must not inherit a buffered TAP result to flush. */
		CHECK_UINT(fflush(stdout), 0);
		pid = fork();
		if (!pid)
			tm_child_leak();
		CHECK(pid > 0);
		if (pid > 0) {
			CHECK(waitpid(pid, &wst, 0) == pid);
			CHECK(WIFEXITED(wst));
			CHECK_UINT(WEXITSTATUS(wst), 0);
			CHECK(tm_child->opened);
			CHECK(tm_child->closed);
			CHECK(tm_child->dup_fd >= 0);
			CHECK(tm_child->dup_live);
		}
		close(tm_wedge_pipe[0]);
		close(tm_wedge_pipe[1]);
		tm_wedge_pipe[0] = tm_wedge_pipe[1] = -1;
		udsp_tdm_override(NULL);
	}
}

/* --- cases: the gate ----------------------------------------------------- */

#ifdef HAVE_OPUS
/* --- the far end's Opus -------------------------------------------------- */

/*
 * libopus itself is the far end, not dsp/opus.c: the oracle must not share
 * code with the wrapper it checks, and test_opus already proves that wrapper
 * against the library. The peer runs at the line rate, because a codec whose
 * sample_rate is 0 is created against the line (codec.h Rates).
 */
#define TM_OPUS_PT	96u
#define TM_OPUS_MAX	512u
/* codec.h: the Opus RTP clock is 48000 whatever the audio rate. */
#define TM_OPUS_TS_MS	48u
/* 500 ms of decoded far-end audio at the wideband rate. */
#define TM_OPUS_REC	8000u

struct tm_opus {
	OpusEncoder *enc;
	OpusDecoder *dec;
	unsigned rate;
};

static bool tm_opus_open(struct tm_opus *o, unsigned rate)
{
	int err = OPUS_OK;

	memset(o, 0, sizeof(*o));
	o->rate = rate;
	o->enc = opus_encoder_create((opus_int32)rate, 1, OPUS_APPLICATION_VOIP,
				     &err);
	if (!o->enc || err != OPUS_OK)
		return false;
	if (opus_encoder_ctl(o->enc, OPUS_SET_BITRATE(32000)) != OPUS_OK)
		return false;
	o->dec = opus_decoder_create((opus_int32)rate, 1, &err);

	return o->dec && err == OPUS_OK;
}

static void tm_opus_close(struct tm_opus *o)
{
	if (o->enc)
		opus_encoder_destroy(o->enc);
	if (o->dec)
		opus_decoder_destroy(o->dec);
	o->enc = NULL;
	o->dec = NULL;
}

/*
 * One oscillator per tone, and the caller owns it, exactly as tm_tone_send
 * does: a shared accumulator would break the phase of one tone every time the
 * other one played, and the Goertzel oracles read coherence.
 */
static unsigned tm_opus_tone(unsigned rate, int16_t *pcm, unsigned ptime_ms,
			     double hz, double amp, double *phase)
{
	unsigned n = rate * ptime_ms / 1000u;

	for (unsigned i = 0; i < n; i++) {
		pcm[i] = (int16_t)(amp * sin(*phase));
		*phase += 2.0 * M_PI * hz / (double)rate;
	}

	return n;
}

/*
 * One packet whose 10 ms halves carry different tones. The second tone is the
 * whole oracle for the second slice of a unit: a playout that replays slice 0
 * never puts it on the line, and a playout that conceals it puts nothing there
 * either. Both oscillators free-run over the whole packet, so each tone stays
 * coherent from packet to packet and one Goertzel per tone measures it.
 */
static unsigned tm_opus_tone_2(unsigned rate, int16_t *pcm, unsigned ptime_ms,
			       double hz_a, double hz_b, double amp_a,
			       double amp_b, double *pa, double *pb)
{
	unsigned n = rate * ptime_ms / 1000u;
	unsigned first = rate / 100u;

	for (unsigned i = 0; i < n; i++) {
		bool head = i < first;

		pcm[i] = (int16_t)((head ? amp_a : amp_b) *
				   sin(head ? *pa : *pb));
		*pa += 2.0 * M_PI * hz_a / (double)rate;
		*pb += 2.0 * M_PI * hz_b / (double)rate;
	}

	return n;
}

/* deliver false encodes and stamps the packet and then discards it: the far
 * end's timeline keeps moving, so the hole reads as a loss and not as a far
 * end that went quiet, which is what makes the buffer conceal it. */
static void tm_opus_emit(struct tm_wire *w, struct tm_opus *o,
			 const int16_t *pcm, unsigned n, unsigned ptime_ms,
			 bool deliver)
{
	uint8_t pay[TM_OPUS_MAX];
	uint8_t pkt[TM_PKT_MAX];
	int bytes = opus_encode(o->enc, pcm, (int)n, pay,
				(opus_int32)sizeof(pay));
	size_t len;

	if (bytes <= 0)
		return;
	len = tm_rtp_build(pkt, TM_OPUS_PT, w->seq == 1000, w->seq, w->ts,
			   w->ssrc, pay, (size_t)bytes);
	w->seq++;
	w->ts += TM_OPUS_TS_MS * ptime_ms;
	if (!deliver)
		return;
	if (send(w->rtp_peer, pkt, len, MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
		return;
}

static void tm_opus_send(struct tm_wire *w, struct tm_opus *o,
			 unsigned ptime_ms, double hz, double amp,
			 double *phase)
{
	int16_t pcm[UDSP_PCM_STAGE_MAX];
	unsigned n = tm_opus_tone(o->rate, pcm, ptime_ms, hz, amp, phase);

	tm_opus_emit(w, o, pcm, n, ptime_ms, true);
}

static void tm_opus_send_2(struct tm_wire *w, struct tm_opus *o,
			   unsigned ptime_ms, double hz_a, double hz_b,
			   double amp_a, double amp_b, double *pa, double *pb)
{
	int16_t pcm[UDSP_PCM_STAGE_MAX];
	unsigned n = tm_opus_tone_2(o->rate, pcm, ptime_ms, hz_a, hz_b, amp_a,
				    amp_b, pa, pb);

	tm_opus_emit(w, o, pcm, n, ptime_ms, true);
}

static void tm_opus_drop(struct tm_wire *w, struct tm_opus *o,
			 unsigned ptime_ms, double hz, double amp,
			 double *phase)
{
	int16_t pcm[UDSP_PCM_STAGE_MAX];
	unsigned n = tm_opus_tone(o->rate, pcm, ptime_ms, hz, amp, phase);

	tm_opus_emit(w, o, pcm, n, ptime_ms, false);
}

static bool tm_gate_opus_send(struct udsp_stream *st, struct tm_wire *w,
			      struct tm_opus *o, unsigned packets,
			      unsigned ptime, double hz, double *phase)
{
	unsigned want;

	if (!tm_gate_ring_prepare(st, packets, &want))
		return false;
	for (unsigned i = 0; i < packets; i++)
		tm_opus_send(w, o, ptime, hz, 8000.0, phase);

	return tm_gate_ring_wait(st, want);
}

static bool tm_gate_opus_tone_run(struct eh_gate *gate, struct udsp_stream *st,
				  struct tm_wire *w, struct tm_opus *o,
				  unsigned ptime, double hz, double *phase)
{
	for (unsigned b = 0; b < 8u; b++) {
		if (!tm_gate_opus_send(st, w, o, 5, ptime, hz, phase))
			return false;
		if (b == 3u)
			eh_rec_mark();
		if (!eh_gate_step(gate, ptime / 2u))
			return false;
	}

	return true;
}

static bool tm_gate_opus_slices_send(struct udsp_stream *st, struct tm_wire *w,
				     struct tm_opus *o, double *low,
				     double *high)
{
	unsigned want;

	if (!tm_gate_ring_prepare(st, 5, &want))
		return false;
	for (unsigned i = 0; i < 5u; i++)
		tm_opus_send_2(w, o, 20, 800.0, 2200.0, 8000.0, 8000.0,
			       low, high);

	return tm_gate_ring_wait(st, want);
}

static bool tm_gate_opus_slices_run(struct eh_gate *gate,
				    struct udsp_stream *st, struct tm_wire *w,
				    struct tm_opus *o, double *low,
				    double *high)
{
	for (unsigned b = 0; b < 10u; b++) {
		if (!tm_gate_opus_slices_send(st, w, o, low, high))
			return false;
		if (b == 3u)
			eh_rec_mark();
		if (!eh_gate_step(gate, 10))
			return false;
	}

	return true;
}

static bool tm_gate_opus_alternate_send(struct udsp_stream *st,
					struct tm_wire *w, struct tm_opus *o,
					double *slow, double *fast,
					double *tail)
{
	unsigned want;

	if (!tm_gate_ring_prepare(st, 10, &want))
		return false;
	for (unsigned i = 0; i < 5u; i++) {
		/* Silent 10 ms packets hold the one-slice span, so only the
		 * oversized 20 ms packet can supply the measured tone. */
		tm_opus_send(w, o, 10, 1000.0, 0.0, slow);
		tm_opus_send_2(w, o, 20, 2600.0, 2600.0, 8000.0, 0.0,
			       fast, tail);
	}

	return tm_gate_ring_wait(st, want);
}

static bool tm_gate_opus_alternate_run(struct eh_gate *gate,
				       struct udsp_stream *st,
				       struct tm_wire *w,
				       struct tm_opus *o, double *slow,
				       double *fast, double *tail)
{
	for (unsigned b = 0; b < 10u; b++) {
		if (!tm_gate_opus_alternate_send(st, w, o, slow, fast, tail))
			return false;
		if (b == 3u)
			eh_rec_mark();
		if (!eh_gate_step(gate, 15))
			return false;
	}

	return true;
}

static bool tm_gate_opus_loss_send(struct udsp_stream *st, struct tm_wire *w,
				   struct tm_opus *o, unsigned *sent,
				   double *phase)
{
	unsigned drops = (*sent + 10u) / 4u - *sent / 4u;
	unsigned want;

	if (!tm_gate_ring_prepare(st, 10u - drops, &want))
		return false;
	for (unsigned i = 0; i < 10u; i++) {
		if (++*sent % 4u)
			tm_opus_send(w, o, 20, 1000.0, 8000.0, phase);
		else
			tm_opus_drop(w, o, 20, 1000.0, 8000.0, phase);
	}

	return tm_gate_ring_wait(st, want);
}

static bool tm_gate_opus_loss_run(struct eh_gate *gate, struct udsp_stream *st,
				  struct tm_wire *w, struct tm_opus *o,
				  double *phase, struct udsp_jb_stats *was)
{
	unsigned sent = 0;

	for (unsigned b = 0; b < 8u; b++) {
		if (!tm_gate_opus_loss_send(st, w, o, &sent, phase))
			return false;
		if (b == 3u) {
			eh_rec_mark();
			udsp_jb_stats(&st->jb, was);
		}
		if (!eh_gate_step(gate, 20))
			return false;
	}

	return true;
}

/* One outbound payload into the far end's frame. The decoder is stateful, so
 * a payload is decoded once and every measurement reads that result. */
static int tm_opus_decode(struct tm_opus *o, const uint8_t *pay, size_t len,
			  int16_t *pcm)
{
	int want = opus_packet_get_nb_samples(pay, (opus_int32)len,
					      (opus_int32)o->rate);

	if (want <= 0 || (unsigned)want > UDSP_PCM_STAGE_MAX)
		return -1;

	return opus_decode(o->dec, pay, (opus_int32)len, pcm, want, 0);
}

/*
 * The weakest block of a run of samples, which is what tells audio from a hole
 * in it: concealment that the codec generated keeps every block above the
 * floor and a path that plays zeros leaves one block at exactly nothing, and a
 * transmit staging that encodes the same tick twice leaves every second block
 * empty. A mean over the whole window hides both.
 */
static double tm_block_floor(const int16_t *pcm, unsigned n, unsigned block)
{
	double worst = -1.0;

	if (!block)
		return 0.0;
	for (unsigned off = 0; off + block <= n; off += block) {
		double sum = 0.0;

		for (unsigned i = 0; i < block; i++)
			sum += (double)pcm[off + i] * pcm[off + i];
		sum /= block;
		if (worst < 0.0 || sum < worst)
			worst = sum;
	}

	return worst < 0.0 ? 0.0 : worst;
}

/*
 * The Goertzel power a coherent sinusoid of this amplitude reaches over n
 * samples. A packet that carried every tick it staged, in the order it staged
 * them, reads near it; one that overwrote a staging slot or encoded a tick
 * twice cannot, whatever its mean energy says.
 */
static double tm_tone_ideal(double amp, unsigned n)
{
	double peak = amp * (double)n / 2.0;

	return peak * peak;
}

static double tm_rec_block_floor(unsigned from, unsigned len, unsigned block)
{
	if (from + len > eh_rec_have())
		return 0.0;

	return tm_block_floor(eh_be.rec + eh_be.rec_base + from, len, block);
}

static void tm_opus_media(struct udsp_media *m, uint8_t ptime)
{
	tm_media(m, UDSP_SEL_OPUS, TM_OPUS_PT, ptime);
}

/* Every outbound Opus packet of the peer socket, decoded into one record. */
static unsigned tm_opus_drain(struct tm_wire *w, struct tm_opus *o,
			      int16_t *pcm, unsigned cap, unsigned *packets,
			      uint32_t *ts_step)
{
	uint8_t buf[TM_PKT_MAX];
	struct tm_pkt p = { 0 };
	uint32_t last_ts = 0;
	bool have_ts = false;
	unsigned have = 0;
	ssize_t n;

	*packets = 0;
	*ts_step = 0;
	while ((n = tm_recv(w->rtp_peer, buf, sizeof(buf))) > 0) {
		int16_t frame[UDSP_PCM_STAGE_MAX];
		int got;

		if (!tm_rtp_parse(buf, (size_t)n, &p) || p.pt != TM_OPUS_PT)
			continue;
		got = tm_opus_decode(o, p.payload, p.len, frame);
		if (got <= 0)
			continue;
		(*packets)++;
		if (have_ts && !*ts_step)
			*ts_step = p.ts - last_ts;
		last_ts = p.ts;
		have_ts = true;
		if (have + (unsigned)got > cap)
			continue;
		memcpy(pcm + have, frame, (unsigned)got * sizeof(*pcm));
		have += (unsigned)got;
	}

	return have;
}

static void case_opus(void)
{
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	struct tm_opus o;
	struct eh t;

	/*
	 * The shape of an Opus stream. codec.h gives Opus sample_rate 0, so it
	 * follows the line and no resampler is instantiated at either rate;
	 * the RTP clock is 48000 whatever the audio rate, so one 10 ms tick is
	 * 480 ticks; frame_ms 0 makes the jitter buffer hand whole packets out,
	 * and the vtable says the same with frame_bytes 0. Opus carries its own
	 * discontinuous transmission rather than a native SID frame, so
	 * sid_bytes is 0 as well. This suite is the only one that links the
	 * codec and the registry together, so the pin lives here.
	 */
	CASE("an Opus stream follows the line rate and stamps at 48000");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_opus_media(&m, 20);
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			CHECK(!st->resample);
			CHECK(st->unit_packet);
			CHECK_UINT(st->codec_rate, UDSP_SAMPLE_RATE);
			CHECK_UINT(st->codec_frame, UDSP_FRAME_SAMPLES);
			CHECK_UINT(st->line_frame, UDSP_FRAME_SAMPLES);
			CHECK_UINT(st->ts_per_frame, 480);
			CHECK_UINT(st->frames_per_packet, 2);
			CHECK_UINT(st->jb.ts_per_ms, 48);
			CHECK_UINT(st->jb.frame_ts, 480);
			CHECK(st->jb.whole_packet);
			CHECK_UINT(st->params.line_rate, UDSP_SAMPLE_RATE);
			CHECK_UINT(st->params.ptime_ms, 20);
			CHECK(st->ops != NULL);
			if (st->ops) {
				CHECK_UINT(st->ops->frame_bytes, 0);
				CHECK_UINT(st->ops->sid_bytes, 0);
			}
		}
		tm_call_close(&t, &w);

		if (!tm_up_rate(&t, "USA", UDSP_SAMPLE_RATE_WB) ||
		    !tm_wire_open(&w, 0)) {
			CHECK(false);
			return;
		}
		tm_opus_media(&m, 10);
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			CHECK(!st->resample);
			CHECK(st->unit_packet);
			CHECK_UINT(st->codec_rate, UDSP_SAMPLE_RATE_WB);
			CHECK_UINT(st->codec_frame, UDSP_FRAME_MAX);
			CHECK_UINT(st->ts_per_frame, 480);
			CHECK_UINT(st->frames_per_packet, 1);
			CHECK_UINT(st->params.line_rate, UDSP_SAMPLE_RATE_WB);
			CHECK_UINT(st->params.ptime_ms, 10);
		}
		/* codec.h ptime_mask 0x03: 30 ms is not an Opus packet. */
		tm_opus_media(&m, 30);
		CHECK(udsp_stream_config(t.s, 0, 0, &m, w.rtp_local,
					 w.rtcp_local) == UDSP_UNSUPPORTED);
		tm_call_close(&t, &w);
	}

	/*
	 * UDSP_CODEC_F_NO_CN. Opus carries its own discontinuous transmission,
	 * so this layer must not gate a talkspurt on top of it: media_prepare
	 * takes the detector and the description down, and the receiver then
	 * refuses the far end's PT 13 as well, because a source that never
	 * negotiated comfort noise has none.
	 */
	CASE("Opus takes the voice detector and comfort noise down with it");
	{
		uint8_t pkt[TM_PKT_MAX];
		uint8_t pay[1];
		double phase = 0.0;
		size_t len;

		if (!tm_up(&t, "USA") || !tm_wire_open(&w, 0) ||
		    !tm_opus_open(&o, UDSP_SAMPLE_RATE)) {
			CHECK(false);
			return;
		}
		tm_opus_media(&m, 20);
		m.vad = true;
		m.cng = true;
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			CHECK(!st->media.vad);
			CHECK(!st->media.cng);
			CHECK(!st->receiver.cng_enabled);
			CHECK(st->ops != NULL);
			if (st->ops) {
				CHECK((st->ops->flags &
				       UDSP_CODEC_F_NO_CN) != 0);
				CHECK(st->ops->cn == NULL);
			}
		}
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		for (unsigned i = 0; i < 10u; i++) {
			tm_opus_send(&w, &o, 20, 1000.0, 8000.0, &phase);
			eh_wait_ms(&t, 20);
		}
		/* An RFC 3389 description on a stream that has no comfort
		 * noise is not audio: the receiver discards it. */
		pay[0] = 40;
		len = tm_rtp_build(pkt, 13u, false, w.seq++, w.ts, w.ssrc, pay, 1);
		CHECK(send(w.rtp_peer, pkt, len, MSG_DONTWAIT | MSG_NOSIGNAL) > 0);
		eh_wait_ms(&t, 60);
		{
			struct udsp_jb_stats js;
			struct udsp_media_stats ms;

			CHECK(udsp_stream_jb_stats(t.s, 0, 0, &js) == UDSP_OK);
			CHECK_UINT(js.cn, 0);
			/* The jitter buffer counters publish every tick; the
			 * media statistics publish every UDSP_STATS_PERIOD_MS.
			 * One full period after the discard, a publish carries
			 * it. */
			eh_wait_ms(&t, UDSP_STATS_PERIOD_MS);
			CHECK(udsp_stream_stats(t.s, 0, 0, &ms) == UDSP_OK);
			CHECK(ms.packets_discarded > 0);
		}
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_opus_close(&o);
		tm_call_close(&t, &w);
	}

	/*
	 * The round trip on a narrowband line. The near end's 1000 Hz tone
	 * leaves as one Opus packet per 20 ms under one 960-tick clock step,
	 * and the far end's tone comes back out of the line at 1000 Hz.
	 */
	CASE("an Opus round trip carries a tone on an 8 kHz line");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		int16_t pcm[TM_OPUS_REC];
		double phase = 0.0;
		unsigned packets = 0;
		uint32_t step = 0;
		unsigned got;

		if (!tm_up(&t, "USA") || !tm_wire_open(&w, 0) ||
		    !tm_opus_open(&o, UDSP_SAMPLE_RATE)) {
			CHECK(false);
			return;
		}
		tm_opus_media(&m, 20);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 400);

		got = tm_opus_drain(&w, &o, pcm, TM_OPUS_REC, &packets, &step);
		CHECK(packets > 10u);
		CHECK_UINT(step, 960);
		CHECK(got >= 1600u);
		if (got >= 1600u) {
			double want = tm_tone_power(pcm + 400, 1200, 1000.0,
						    UDSP_SAMPLE_RATE);
			double other = tm_tone_power(pcm + 400, 1200, 2500.0,
						     UDSP_SAMPLE_RATE);

			CHECK(want > 0.5 * tm_tone_ideal(8000.0, 1200));
			CHECK(other * 100.0 < want);
		}

		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		eh_wait_ms(&t, 60);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st || !eh_gate_park(&gate) ||
		    !tm_gate_opus_tone_run(&gate, st, &w, &o, 20, 1000.0,
					   &phase))
			goto opus_nb_out;
		CHECK_UINT(eh_rec_have(), 4000);
		if (eh_rec_have() != 4000u)
			goto opus_nb_out;
		{
			double want = eh_tone_power(0, 1600, 1000.0,
						    UDSP_SAMPLE_RATE);
			double other = eh_tone_power(0, 1600, 2500.0,
						     UDSP_SAMPLE_RATE);

			CHECK(eh_energy(0, 1600) > 1000.0);
			CHECK(want > 1e8);
			CHECK(other * 100.0 < want);
		}
	opus_nb_out:
		eh_gate_disarm(&gate);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_opus_close(&o);
		tm_call_close(&t, &w);
		eh_gate_close(&gate);
	}

	/*
	 * The round trip on a wideband line. A 5000 Hz tone is the
	 * discriminator: a codec created at 8000 could only alias it, so its
	 * arrival at 5000 Hz on both sides proves the instance follows the
	 * 16 kHz line (codec.h sample_rate 0).
	 */
	CASE("an Opus round trip carries a 5 kHz tone on a 16 kHz line");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		int16_t pcm[TM_OPUS_REC];
		double phase = 0.0;
		unsigned packets = 0;
		uint32_t step = 0;
		unsigned got;

		if (!tm_up_rate(&t, "USA", UDSP_SAMPLE_RATE_WB) ||
		    !tm_wire_open(&w, 0) ||
		    !tm_opus_open(&o, UDSP_SAMPLE_RATE_WB)) {
			CHECK(false);
			return;
		}
		tm_opus_media(&m, 20);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(5000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 400);

		got = tm_opus_drain(&w, &o, pcm, TM_OPUS_REC, &packets, &step);
		CHECK(packets > 10u);
		CHECK_UINT(step, 960);
		CHECK(got >= 3200u);
		if (got >= 3200u) {
			double want = tm_tone_power(pcm + 800, 2400, 5000.0,
						    UDSP_SAMPLE_RATE_WB);
			double alias = tm_tone_power(pcm + 800, 2400, 3000.0,
						     UDSP_SAMPLE_RATE_WB);

			CHECK(want > 0.5 * tm_tone_ideal(8000.0, 2400));
			CHECK(alias * 10.0 < want);
		}

		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		eh_wait_ms(&t, 60);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st || !eh_gate_park(&gate) ||
		    !tm_gate_opus_tone_run(&gate, st, &w, &o, 20, 5000.0,
					   &phase))
			goto opus_wb_out;
		CHECK_UINT(eh_rec_have(), 8000);
		if (eh_rec_have() != 8000u)
			goto opus_wb_out;
		{
			double want = eh_tone_power(0, 3200, 5000.0,
						    UDSP_SAMPLE_RATE_WB);
			double alias = eh_tone_power(0, 3200, 3000.0,
						     UDSP_SAMPLE_RATE_WB);

			CHECK(eh_energy(0, 3200) > 1000.0);
			CHECK(want > 1e8);
			CHECK(alias * 10.0 < want);
		}
	opus_wb_out:
		eh_gate_disarm(&gate);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_opus_close(&o);
		tm_call_close(&t, &w);
		eh_gate_close(&gate);
	}

	/*
	 * Both slices of the unit reach the line (jb.h Units). The packet is
	 * indivisible, so the buffer hands it over once and marks its second
	 * slice; the stream decodes once and plays 10 ms of the staging per
	 * tick. Each half of the packet carries its own tone, so a playout
	 * that repeats slice 0, or conceals slice 1, loses one of the two.
	 */
	CASE("an Opus packet plays both of its 10 ms slices");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		double low = 0.0;
		double high = 0.0;

		if (!tm_up(&t, "USA") || !tm_wire_open(&w, 0) ||
		    !tm_opus_open(&o, UDSP_SAMPLE_RATE)) {
			CHECK(false);
			return;
		}
		tm_opus_media(&m, 20);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st || !eh_gate_park(&gate) ||
		    !tm_gate_opus_slices_run(&gate, st, &w, &o, &low, &high))
			goto opus_slices_out;
		/* Concealment leaves the final decode's slice record intact. */
		if (!eh_gate_step(&gate, 40))
			goto opus_slices_out;
		CHECK_UINT(eh_rec_have(), 8800);
		if (eh_rec_have() != 8800u)
			goto opus_slices_out;
		{
			struct udsp_jb_stats js;

			CHECK(udsp_stream_jb_stats(t.s, 0, 0, &js) == UDSP_OK);
			CHECK(js.frames_out > 40u);
		}
		{
			double first = eh_tone_power(0, 2400, 800.0,
						     UDSP_SAMPLE_RATE);
			double second = eh_tone_power(0, 2400, 2200.0,
						      UDSP_SAMPLE_RATE);
			double none = eh_tone_power(0, 2400, 1500.0,
						    UDSP_SAMPLE_RATE);

			CHECK(first > 1e8);
			CHECK(second > 1e8);
			CHECK(none * 20.0 < first);
			CHECK(none * 20.0 < second);
		}
	opus_slices_out:
		eh_gate_disarm(&gate);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		/* The stop ack orders the read of the retained slice record. */
		if (st) {
			CHECK_UINT(st->rx_unit_slices, 2);
			CHECK(st->rx_unit_valid);
		}
		tm_opus_close(&o);
		tm_call_close(&t, &w);
		eh_gate_close(&gate);
	}

	/*
	 * The codec measures the unit. The buffer's slice count is its playout
	 * grid and the packet's length is the codec's: they disagree wherever
	 * the span learner has no confirmation yet, which UDSP_JB_OFF reaches
	 * on its first packet and an unsignaled packet-duration change reaches
	 * mid-call. A decode bounded by the buffer's count refuses the whole
	 * packet and conceals a unit that was perfectly good; a decode bounded
	 * by the staging plays what the packet holds. The far end proves it by
	 * moving from 10 ms to one 20 ms packet on a new tone and going quiet:
	 * the tone reaches the line and the concealment that follows continues
	 * that tone, which only a decoder that consumed the packet can do.
	 */
	CASE("an Opus packet longer than the buffer's slice count still plays");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		double slow_phase = 0.0;
		double fast_phase = 0.0;
		double tail_phase = 0.0;

		if (!tm_up(&t, "USA") || !tm_wire_open(&w, 0) ||
		    !tm_opus_open(&o, UDSP_SAMPLE_RATE)) {
			CHECK(false);
			return;
		}
		tm_opus_media(&m, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		/* A settled 10 ms span first, so the buffer's slice count is
		 * one and the disagreement below is the packet's alone. */
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st || !eh_gate_park(&gate) ||
		    !tm_gate_opus_tone_run(&gate, st, &w, &o, 10, 1000.0,
					   &slow_phase))
			goto opus_long_out;
		/* The settling loop above holds the span at one 10 ms slice. The
		 * live grid is tick-private, so the one-slice premise is proven
		 * by the tone reaching the line under it and by the final slice
		 * count read after the stop below, not by a mid-stream read.
		 *
		 * The far end now alternates 10 ms and 20 ms packets. jb.c
		 * changes an established span only on two agreeing
		 * observations, which an alternating stream never gives, so
		 * every 20 ms packet arrives under a one-slice grid for the
		 * rest of the call. Its tone is the oracle: a decode bounded
		 * by that grid refuses the packet whole and the tone never
		 * reaches the line, nor the decoder that would conceal it. */
		if (!tm_gate_opus_alternate_run(&gate, st, &w, &o, &slow_phase,
						&fast_phase, &tail_phase))
			goto opus_long_out;
		/*
		 * The far end stops and the queue empties, so the last unit
		 * the stream decoded is the last packet it sent, which is a
		 * 20 ms one. Sampling the staging while packets are still
		 * queued would read whichever of the two durations the engine
		 * happened to decode last, and a deep queue makes that
		 * unpredictable. Concealment writes rx_pcm, so the drain leaves
		 * the staging record standing.
		 */
		if (!eh_gate_step(&gate, 40))
			goto opus_long_out;
		CHECK_UINT(eh_rec_have(), 11600);
		if (eh_rec_have() != 11600u)
			goto opus_long_out;
		{
			double tone = eh_tone_power(0, 2400, 2600.0,
						    UDSP_SAMPLE_RATE);
			double none = eh_tone_power(0, 2400, 1800.0,
						    UDSP_SAMPLE_RATE);

			CHECK(eh_energy(0, 2400) > 1e6);
			CHECK(tone > 1e10);
			CHECK(none * 20.0 < tone);
		}
	opus_long_out:
		eh_gate_disarm(&gate);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		/* The last decoded unit is the final 20 ms packet; the grid it
		 * played survives teardown and the stop ack orders this read. */
		if (st) {
			CHECK(st->rx_unit_valid);
			CHECK_UINT(st->rx_unit_slices, 2);
		}
		tm_opus_close(&o);
		tm_call_close(&t, &w);
		eh_gate_close(&gate);
	}

	/*
	 * Ptime 10. One packet per tick, one 480-tick step, and the packet
	 * holds exactly 10 ms of audio, which is what the far end's own
	 * sample count reports.
	 */
	CASE("an Opus stream at ptime 10 sends one packet per tick");
	{
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p = { 0 };
		unsigned packets = 0;
		uint32_t last = 0;
		bool have = false;
		bool steps = true;
		bool sizes = true;
		ssize_t n;

		if (!tm_up(&t, "USA") || !tm_wire_open(&w, 0) ||
		    !tm_opus_open(&o, UDSP_SAMPLE_RATE)) {
			CHECK(false);
			return;
		}
		tm_opus_media(&m, 10);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 300);

		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			int samples;

			if (!tm_rtp_parse(buf, (size_t)n, &p) ||
			    p.pt != TM_OPUS_PT)
				continue;
			samples = opus_packet_get_nb_samples(p.payload,
					(opus_int32)p.len,
					(opus_int32)UDSP_SAMPLE_RATE);
			if (samples != (int)UDSP_FRAME_SAMPLES)
				sizes = false;
			if (have && p.ts - last != 480u)
				steps = false;
			last = p.ts;
			have = true;
			packets++;
		}
		CHECK(packets > 20u);
		CHECK(steps);
		CHECK(sizes);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st)
			CHECK_UINT(st->frames_per_packet, 1);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_opus_close(&o);
		tm_call_close(&t, &w);
	}

	/*
	 * Native concealment (codec.h PLC and CN). A whole-packet codec
	 * with UDSP_CODEC_F_PLC conceals its own losses, so a hole in the
	 * stream comes out as audio and never as a block of zeros. The
	 * weakest 10 ms block of the window is the oracle: dropping the
	 * codec's plc call leaves the memset behind, which reads exactly 0.
	 */
	CASE("a lost Opus packet is concealed by the codec, not by silence");
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct udsp_jb_stats was = { 0 };
		double phase = 0.0;

		if (!tm_up(&t, "USA") || !tm_wire_open(&w, 0) ||
		    !tm_opus_open(&o, UDSP_SAMPLE_RATE)) {
			CHECK(false);
			return;
		}
		tm_opus_media(&m, 20);
		/*
		 * A deep buffer on purpose. The oracle below is an absolute
		 * floor over every 10 ms block, so a queue that runs dry would
		 * read as a concealment fault: jb.c answers a run past
		 * JB_PLC_MAX_MS with silence, which is a real zero block and
		 * has nothing to do with the codec. 100 ms of standing depth
		 * and one 200 ms burst per step keep the far end far enough
		 * ahead that only the seeded losses conceal.
		 */
		m.jb_init_ms = 100;
		m.jb_max_ms = 200;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st || !eh_gate_park(&gate) ||
		    !tm_gate_opus_loss_run(&gate, st, &w, &o, &phase, &was))
			goto opus_loss_out;
		CHECK_UINT(eh_rec_have(), 8000);
		if (eh_rec_have() != 8000u)
			goto opus_loss_out;
		{
			struct udsp_jb_stats js;

			udsp_jb_stats(&st->jb, &js);
			/*
			 * The two preconditions of the floor, checked first so
			 * a starved harness names itself instead of pointing
			 * at the codec. The window holds ten seeded losses,
			 * which is twenty concealed units: every unit was the
			 * buffer asking the codec (silence is the buffer
			 * giving up, and the ticks before the first packet are
			 * silence by definition and sit outside the window),
			 * and the count stays near what was seeded, because a
			 * concealment run long enough for the codec's own fade
			 * to reach zero is a starved far end and not a
			 * concealment fault.
			 */
			CHECK(js.plc - was.plc > 4u);
			CHECK(js.plc - was.plc < 60u);
			CHECK_UINT(js.silence - was.silence, 0);
		}
		{
			double want = eh_tone_power(0, 2400, 1000.0,
						    UDSP_SAMPLE_RATE);

			CHECK(want > 1e8);
			/*
			 * Concealment output is audio. The discriminator is
			 * zero against not-zero and nothing finer: a codec
			 * that conceals writes samples, and the engine that
			 * plays silence instead writes an exactly empty block.
			 * A floor set at the level of speech would instead be
			 * measuring how fast the codec's fade decays, which is
			 * the codec's business and moves with the run length.
			 */
			CHECK(tm_rec_block_floor(0, 2400, 80) > 1.0);
		}
	opus_loss_out:
		eh_gate_disarm(&gate);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_opus_close(&o);
		tm_call_close(&t, &w);
		eh_gate_close(&gate);
	}

	/*
	 * The 512-byte bound. jb.h sizes one slot at UDSP_JB_PAYLOAD_MAX,
	 * which is codec.h's own packet bound, and a payload above it is
	 * counted and refused rather than truncated into the decoder. The
	 * datagram still fits the 576-byte net ring, so what the case
	 * exercises is the buffer's bound and not the ring's.
	 */
	CASE("an oversize Opus packet is refused at the 512-byte bound");
	{
		uint8_t pkt[TM_PKT_MAX];
		uint8_t pay[UDSP_JB_PAYLOAD_MAX + 1u];
		struct udsp_jb_stats js;
		double phase = 0.0;
		size_t len;

		if (!tm_up(&t, "USA") || !tm_wire_open(&w, 0) ||
		    !tm_opus_open(&o, UDSP_SAMPLE_RATE)) {
			CHECK(false);
			return;
		}
		tm_opus_media(&m, 20);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		for (unsigned i = 0; i < 6u; i++) {
			tm_opus_send(&w, &o, 20, 1000.0, 8000.0, &phase);
			eh_wait_ms(&t, 20);
		}
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			tm_opus_close(&o);
			tm_call_close(&t, &w);
			return;
		}
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &js) == UDSP_OK);
		CHECK_UINT(js.overflow_drop, 0);

		memset(pay, 0xa5, sizeof(pay));
		len = tm_rtp_build(pkt, TM_OPUS_PT, false, w.seq++, w.ts,
				   w.ssrc, pay, sizeof(pay));
		w.ts += TM_OPUS_TS_MS * 20u;
		CHECK_UINT(len, 12u + UDSP_JB_PAYLOAD_MAX + 1u);
		CHECK(len < UDSP_NET_DGRAM_MAX);
		CHECK(send(w.rtp_peer, pkt, len, MSG_DONTWAIT | MSG_NOSIGNAL) > 0);
		eh_wait_ms(&t, 60);
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &js) == UDSP_OK);
		CHECK_UINT(js.overflow_drop, 1);
		/* The stream survives it: the next packet plays. */
		for (unsigned i = 0; i < 6u; i++) {
			tm_opus_send(&w, &o, 20, 1000.0, 8000.0, &phase);
			eh_wait_ms(&t, 20);
		}
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &js) == UDSP_OK);
		CHECK(js.frames_out > 8u);
		CHECK_UINT(js.overflow_drop, 1);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_opus_close(&o);
		tm_call_close(&t, &w);
	}

	/*
	 * The clock-change adopt. RFC 7160 gives one clock per SSRC, so a
	 * re-INVITE that swaps G.711 for Opus cannot be the reconfiguration
	 * that keeps the timeline: the source ends and a new one begins. A
	 * ptime change inside Opus keeps the same clock and stays a
	 * reconfiguration, which is the other half of the rule.
	 */
	CASE("a codec change that moves the RTP clock adopts a new source");
	{
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p = { 0 };
		uint32_t was_ssrc = 0;
		uint32_t now_ssrc = 0;
		uint32_t opus_ssrc = 0;
		ssize_t n;

		if (!tm_up(&t, "USA") || !tm_wire_open(&w, 0) ||
		    !tm_opus_open(&o, UDSP_SAMPLE_RATE)) {
			CHECK(false);
			return;
		}
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 600);
		CHECK(n > 0 && tm_rtp_parse(buf, (size_t)n, &p));
		if (n > 0)
			was_ssrc = p.ssrc;
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st)
			CHECK_UINT(st->ts_per_frame, UDSP_FRAME_SAMPLES);

		tm_opus_media(&m, 20);
		tm_stream_config(&t, &w, &m);
		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;
		eh_wait_ms(&t, 200);
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (tm_rtp_parse(buf, (size_t)n, &p) &&
			    p.pt == TM_OPUS_PT)
				now_ssrc = p.ssrc;
		}
		CHECK(was_ssrc != 0u);
		CHECK(now_ssrc != 0u);
		CHECK(now_ssrc != was_ssrc);
		opus_ssrc = now_ssrc;
		if (st) {
			CHECK_UINT(st->ts_per_frame, 480);
			CHECK(st->unit_packet);
		}

		/* Same clock, new packet duration: the source stands. */
		tm_opus_media(&m, 10);
		tm_stream_config(&t, &w, &m);
		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;
		eh_wait_ms(&t, 200);
		now_ssrc = 0;
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (tm_rtp_parse(buf, (size_t)n, &p) &&
			    p.pt == TM_OPUS_PT)
				now_ssrc = p.ssrc;
		}
		CHECK_UINT(now_ssrc, opus_ssrc);

		/* And back to G.711, which moves the clock again. */
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;
		eh_wait_ms(&t, 200);
		now_ssrc = 0;
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (tm_rtp_parse(buf, (size_t)n, &p) && p.pt == 0u)
				now_ssrc = p.ssrc;
		}
		CHECK(now_ssrc != 0u);
		CHECK(now_ssrc != opus_ssrc);
		if (st) {
			CHECK_UINT(st->ts_per_frame, UDSP_FRAME_SAMPLES);
			CHECK(!st->unit_packet);
		}
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_opus_close(&o);
		tm_call_close(&t, &w);
	}

	/*
	 * RFC 4733 on the Opus clock. The event duration is carried in the
	 * stream's ticks, so a 100 ms digit on a 48000 stream reports 4800 and
	 * not the 800 an 8 kHz stream reports.
	 */
	CASE("a telephone event on an Opus stream runs at 48000");
	{
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p = { 0 };
		unsigned events = 0;
		unsigned ends = 0;
		uint16_t last = 0;
		uint32_t ts = 0;
		ssize_t n;

		if (!tm_up(&t, "USA") || !tm_wire_open(&w, 0) ||
		    !tm_opus_open(&o, UDSP_SAMPLE_RATE)) {
			CHECK(false);
			return;
		}
		tm_opus_media(&m, 20);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 40);
		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;

		CHECK(udsp_stream_dtmf(t.s, 0, 0, '5', 100) == UDSP_OK);
		eh_wait_ms(&t, 300);
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (!tm_rtp_parse(buf, (size_t)n, &p) || p.pt != 101u)
				continue;
			CHECK_UINT(p.len, 4);
			events++;
			if (!ts)
				ts = p.ts;
			CHECK_UINT(p.ts, ts);
			CHECK_UINT(p.payload[0], 5);
			last = (uint16_t)((p.payload[2] << 8) | p.payload[3]);
			if (p.payload[1] & 0x80u)
				ends++;
		}
		CHECK(events >= 4u);
		CHECK_UINT(ends, 3);
		CHECK_UINT(last, 4800);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_opus_close(&o);
		tm_call_close(&t, &w);
	}

	/*
	 * Segmentation (RFC 4733 2.5.1.3). The duration field holds 16 bits of
	 * the stream's ticks, which is 1365 ms at 48000 and 8191 ms at 8000:
	 * an event longer than that can only happen on the Opus clock, because
	 * UDSP_TELEV_MAX_MS caps the event at 8000 ms. A 3 s digit is
	 * therefore three segments, two closes at the field's maximum with the
	 * E bit clear, each sent three times, and one end. A close that set E
	 * would report the digit twice.
	 */
	CASE("a long telephone event on an Opus stream segments at 65535");
	{
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p = { 0 };
		uint32_t stamps[4] = { 0 };
		uint32_t prev_ts = 0;
		unsigned closes = 0;
		unsigned ends = 0;
		unsigned nstamp = 0;
		bool have_prev = false;
		uint16_t last = 0;
		bool codes = true;
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_opus_media(&m, 20);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 40);
		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;

		CHECK(udsp_stream_dtmf(t.s, 0, 0, '3', 3000) == UDSP_OK);
		for (unsigned b = 0; b < 18u; b++) {
			eh_wait_ms(&t, 200);
			while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
				uint16_t d;

				if (!tm_rtp_parse(buf, (size_t)n, &p) ||
				    p.pt != 101u || p.len != 4u)
					continue;
				if (p.payload[0] != 3u)
					codes = false;
				d = (uint16_t)((p.payload[2] << 8) |
					       p.payload[3]);
				/* The run of distinct stamps is counted with
				 * its own cursor: reading it back out of the
				 * bounded array would walk off the end the
				 * moment the count outran it. */
				if (!have_prev || prev_ts != p.ts) {
					if (nstamp < 4u)
						stamps[nstamp] = p.ts;
					nstamp++;
					prev_ts = p.ts;
					have_prev = true;
				}
				if (p.payload[1] & 0x80u) {
					ends++;
					last = d;
					continue;
				}
				if (d == 65535u)
					closes++;
			}
		}
		CHECK(codes);
		CHECK_UINT(closes, 6);
		CHECK_UINT(ends, 3);
		CHECK_UINT(nstamp, 3);
		/* Two closed segments carry 131070 ticks; the end reports what
		 * is left of 3000 ms at 48 ticks per millisecond. */
		CHECK_UINT(last, 144000u - 2u * 65535u);
		if (nstamp == 3u) {
			CHECK_UINT(stamps[1] - stamps[0], 65535);
			CHECK_UINT(stamps[2] - stamps[1], 65535);
		}
		tm_call_down(&t, &w);
	}

	/* The receive half on the same clock: the reported length is in
	 * milliseconds at every clock, so 9600 ticks of the Opus clock is the
	 * 200 ms that 1600 ticks of the 8 kHz clock is. */
	CASE("a telephone event from the wire reports at the Opus clock");
	{
		uint8_t pkt[TM_PKT_MAX];
		uint8_t pay[4];
		size_t len;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_opus_media(&m, 20);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 40);

		pay[0] = 11;			/* '#' */
		pay[1] = 10;
		pay[2] = 0;
		pay[3] = 0;
		len = tm_rtp_build(pkt, 101, true, w.seq++, w.ts, w.ssrc, pay, 4);
		CHECK(send(w.rtp_peer, pkt, len, MSG_DONTWAIT | MSG_NOSIGNAL) > 0);
		eh_wait_ms(&t, 30);
		pay[1] = 10 | 0x80u;
		pay[2] = (uint8_t)(9600u >> 8);
		pay[3] = (uint8_t)9600u;
		len = tm_rtp_build(pkt, 101, false, w.seq++, w.ts, w.ssrc, pay, 4);
		CHECK(send(w.rtp_peer, pkt, len, MSG_DONTWAIT | MSG_NOSIGNAL) > 0);
		eh_wait_ms(&t, 60);
		{
			const struct udsp_event *e =
				eh_ev_find(UDSP_EV_DIGIT, 0);

			CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);
			CHECK(e != NULL);
			if (e) {
				CHECK_UINT(e->u.digit.digit, '#');
				CHECK_UINT(e->u.digit.remote, 1);
				CHECK_UINT(e->u.digit.duration_ms, 200);
			}
		}
		tm_call_down(&t, &w);
	}

	/*
	 * The profile names G.711 here, as uvoice does, so it moves the RTP
	 * clock from 48000 to 8000. RFC 7160 gives one clock per SSRC, so the source
	 * ends and a new one begins, exactly as the same move through
	 * udsp_stream_config does, and the profile off moves it back.
	 */
	CASE("a fax passthrough from Opus adopts a new source");
	{
		struct udsp_jb_stats js;
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p = { 0 };
		uint32_t opus_ssrc = 0;
		uint32_t now_ssrc = 0;
		uint32_t back_ssrc = 0;
		uint16_t pinned = 0;
		double phase = 0.0;
		ssize_t n;

		if (!tm_up(&t, "USA") || !tm_wire_open(&w, 0) ||
		    !tm_opus_open(&o, UDSP_SAMPLE_RATE)) {
			CHECK(false);
			return;
		}
		tm_opus_media(&m, 20);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		/* The far end plays too, so the buffer reaches a target of its
		 * own and the pin below has a figure to carry. */
		for (unsigned i = 0; i < 25u; i++) {
			tm_opus_send(&w, &o, 20, 1000.0, 8000.0, &phase);
			eh_wait_ms(&t, 20);
		}
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 600);
		CHECK(n > 0 && tm_rtp_parse(buf, (size_t)n, &p));
		if (n > 0)
			opus_ssrc = p.ssrc;
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &js) == UDSP_OK);
		pinned = js.target_ms;
		CHECK(pinned > 0u);

		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_OK);
		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;
		eh_wait_ms(&t, 200);
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (tm_rtp_parse(buf, (size_t)n, &p) && p.pt == 0u)
				now_ssrc = p.ssrc;
		}
		CHECK(opus_ssrc != 0u);
		CHECK(now_ssrc != 0u);
		CHECK(now_ssrc != opus_ssrc);
		if (st) {
			CHECK_UINT(st->receiver.clock_rate, UDSP_SAMPLE_RATE);
			CHECK_UINT(st->stats.clock_rate, UDSP_SAMPLE_RATE);
			CHECK_UINT(st->ts_per_frame, UDSP_FRAME_SAMPLES);
			CHECK(!st->unit_packet);
			CHECK_UINT(st->passthrough_kind,
				   UDSP_PASSTHROUGH_MODEM);
			/* A new source plays none of the old queue and still
			 * carries the depth the call adapted to. */
			CHECK_UINT(st->jb_pin_ms, pinned);
			CHECK_UINT(st->jb.init_ms, pinned);
		}

		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_OFF, 0, 0) == UDSP_OK);
		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;
		eh_wait_ms(&t, 200);
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (tm_rtp_parse(buf, (size_t)n, &p) &&
			    p.pt == TM_OPUS_PT)
				back_ssrc = p.ssrc;
		}
		CHECK(back_ssrc != 0u);
		CHECK(back_ssrc != now_ssrc);
		if (st) {
			CHECK_UINT(st->receiver.clock_rate, 48000);
			CHECK_UINT(st->stats.clock_rate, 48000);
			CHECK_UINT(st->ts_per_frame, 480);
			CHECK(st->unit_packet);
			CHECK_UINT(st->passthrough_kind, UDSP_PASSTHROUGH_OFF);
			/* The pin goes back with the kind, so the caller's own
			 * target governs the buffer again. */
			CHECK_UINT(st->jb_pin_ms, 0);
			CHECK_UINT(st->jb.init_ms, m.jb_init_ms);
		}
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_opus_close(&o);
		tm_call_close(&t, &w);
	}
}
#endif /* HAVE_OPUS */

/* --- cases: the T.38 stream mode ----------------------------------------- */

/*
 * The gateway on the wire, at both line rates. The fake hands out one
 * datagram per frame that it takes, and puts the count of those frames in it.
 * The peer socket alone therefore shows that the transmit chain reached the
 * plug-in, once per tick. The receive half is the mirror: the datagrams of the
 * far end arrive at udptl_in in the order they were sent.
 *
 * The record of the fake is read after the stop, never during the run. The
 * engine writes it on the tick, and the deactivation orders the two.
 */
static void tm_t38_wire_leg(unsigned rate)
{
	struct fax_fake_stats *fs = fax_fake_stats_get();
	unsigned line_rate = rate ? rate : UDSP_SAMPLE_RATE;
	unsigned window = line_rate / 10u;
	uint8_t buf[TM_PKT_MAX];
	unsigned first = 0;
	unsigned second = 0;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	char name[96];
	ssize_t n;

	snprintf(name, sizeof(name),
		 "a T.38 stream carries UDPTL both ways at %u kHz",
		 line_rate / 1000u);
	CASE(name);
	if (!tm_call_up_fax(&t, "USA", rate, &w))
		return;
	tm_t38_media(&m);
	tm_stream_config(&t, &w, &m);
	CHECK_UINT(fs->creates, 1);
	CHECK_UINT(fs->destroys, 0);
	tm_capture_tone(1000u);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	eh_wait_ms(&t, 100);

	n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300);
	CHECK(n == (ssize_t)FAX_FAKE_DGRAM);
	if (n == (ssize_t)FAX_FAKE_DGRAM)
		first = tm_udptl_value(buf);
	n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300);
	CHECK(n == (ssize_t)FAX_FAKE_DGRAM);
	if (n == (ssize_t)FAX_FAKE_DGRAM)
		second = tm_udptl_value(buf);
	/* One frame per tick, so the count the datagrams carry only ever
	 * climbs, and by one at a time. */
	CHECK(first > 0u);
	CHECK_UINT(second, first + 1u);

	for (unsigned i = 1; i <= 5u; i++)
		tm_udptl_send(&w, i);
	eh_wait_ms(&t, 150);

	/* The downlink carries the tone the gateway writes at 8 kHz, at
	 * whatever rate the line runs. */
	eh_rec_mark();
	eh_wait_ms(&t, 200);
	{
		double want = eh_tone_power(0, window, FAX_FAKE_TONE_HZ,
					    line_rate);
		double other = eh_tone_power(0, window, 1000.0, line_rate);

		CHECK(want > 1e6);
		CHECK(other * 100.0 < want);
	}

	CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
	CHECK_UINT(fs->destroys, 1);
	CHECK(fs->rx_frames > 20u);
	CHECK(fs->tx_frames > 20u);
	/* The frame the gateway was handed is one 10 ms frame of the line at
	 * 8 kHz, whatever the line rate: a frame that skipped the conversion
	 * would carry the tone at half its frequency. */
	{
		double want = tm_tone_power(fs->rx_last, FAX_FAKE_FRAME, 1000.0,
					    UDSP_SAMPLE_RATE);
		double half = tm_tone_power(fs->rx_last, FAX_FAKE_FRAME, 500.0,
					    UDSP_SAMPLE_RATE);

		CHECK(want > 1e6);
		CHECK(half * 100.0 < want);
	}
	CHECK_UINT(fs->in_count, 5);
	CHECK_UINT(fs->in_errors, 0);
	CHECK_UINT(fs->in_seq_len, 5);
	for (unsigned i = 0; i < 5u && i < fs->in_seq_len; i++)
		CHECK_UINT(fs->in_seq[i], i + 1u);
	tm_call_close(&t, &w);
}

/*
 * A held T.38 stream feeds the gateway the silence that an audio stream would
 * encode. The substitution sits ahead of the mode branch, so the room that the
 * holder must not send never reaches the demodulator either.
 */
static void tm_t38_hold_leg(unsigned rate)
{
	struct fax_fake_stats *fs = fax_fake_stats_get();
	unsigned line_rate = rate ? rate : UDSP_SAMPLE_RATE;
	double loud = 0.0;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	char name[96];

	snprintf(name, sizeof(name),
		 "a held T.38 stream feeds the gateway silence at %u kHz",
		 line_rate / 1000u);
	CASE(name);
	if (!tm_call_up_fax(&t, "USA", rate, &w))
		return;
	tm_t38_media(&m);
	tm_stream_config(&t, &w, &m);
	tm_capture_tone(1000u);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	eh_wait_ms(&t, 200);
	CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
	/* The control arm: the same tone reaches the gateway with the hold
	 * clear, so the measurement below is the hold and not a silent line. */
	loud = tm_tone_power(fs->rx_last, FAX_FAKE_FRAME, 1000.0,
			     UDSP_SAMPLE_RATE);
	CHECK(loud > 1e6);

	m.tx_silence = true;
	tm_stream_config(&t, &w, &m);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	eh_wait_ms(&t, 200);
	CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
	{
		double quiet = 0.0;

		for (unsigned i = 0; i < FAX_FAKE_FRAME; i++)
			quiet += (double)fs->rx_last[i] * fs->rx_last[i];
		/* Digital silence, not merely a quiet frame. */
		CHECK_UINT((uint64_t)quiet, 0);
	}
	tm_call_close(&t, &w);
}

/*
 * The direction gate. A stream that does not send still feeds the
 * demodulator, because T.30 needs continuous audio to follow the far end. A
 * stream that does not receive drops what arrives, and counts it.
 */
static void tm_t38_direction_leg(unsigned rate)
{
	struct fax_fake_stats *fs = fax_fake_stats_get();
	unsigned line_rate = rate ? rate : UDSP_SAMPLE_RATE;
	struct udsp_media_stats stats;
	struct udsp_stream *st;
	uint8_t buf[TM_PKT_MAX];
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	char name[112];

	snprintf(name, sizeof(name),
		 "a T.38 stream obeys its direction each way at %u kHz",
		 line_rate / 1000u);
	CASE(name);
	if (!tm_call_up_fax(&t, "USA", rate, &w))
		return;
	tm_t38_media(&m);
	m.direction = UDSP_MEDIA_RECVONLY;
	tm_stream_config(&t, &w, &m);
	tm_capture_tone(1000u);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	for (unsigned i = 1; i <= 3u; i++)
		tm_udptl_send(&w, i);
	eh_wait_ms(&t, 200);
	CHECK(tm_recv(w.rtp_peer, buf, sizeof(buf)) < 0);
	CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
	CHECK(fs->rx_frames > 10u);
	CHECK_UINT(fs->out_count, 0);
	CHECK_UINT(fs->in_count, 3);

	fax_fake_reset();
	m.direction = UDSP_MEDIA_SENDONLY;
	tm_stream_config(&t, &w, &m);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	for (unsigned i = 1; i <= 3u; i++)
		tm_udptl_send(&w, i);
	eh_wait_ms(&t, 200);
	CHECK(tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 200) ==
	      (ssize_t)FAX_FAKE_DGRAM);
	/* What the direction refused is a discard, and the statistics say so
	 * while the stream still runs. */
	CHECK(udsp_stream_stats(t.s, 0, 0, &stats) == UDSP_OK);
	CHECK_UINT(stats.packets_discarded, 3);
	CHECK_UINT(stats.packets_recv, 0);
	CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
	CHECK_UINT(fs->in_count, 0);
	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL);
	if (st)
		CHECK(st->t38_dropped >= 3u);
	tm_call_close(&t, &w);
}

static void tm_t38_lec_leg(unsigned rate)
{
	unsigned line_rate = rate ? rate : UDSP_SAMPLE_RATE;
	struct udsp_chan_cfg ccfg;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	char name[128];

	snprintf(name, sizeof(name),
		 "the line canceller reads the modem kind under T.38 at %u kHz",
		 line_rate / 1000u);
	CASE(name);
	{
		struct udsp_lec_stats lec;

		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.ec_enable = true;
		ccfg.ec_nlp_enable = true;
		ccfg.ec_tail_ms = 32;
		if (!tm_call_up_fax(&t, "USA", rate, &w))
			return;
		CHECK(eh_hook_seize(&t));
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);

		tm_t38_media(&m);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);
		/* The image session runs the modem policy, so the record names
		 * that kind and the filter runs no frame. */
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.ec_active, 0);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_MODEM);

		/* A port move keeps the mode, so it keeps the bypass. */
		m.remote.port = 5002;
		tm_stream_config(&t, &w, &m);
		eh_wait_ms(&t, 100);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		CHECK_UINT(lec.ec_active, 0);
		CHECK_UINT(lec.nlp_enabled, 0);
		CHECK_UINT(lec.passthrough, UDSP_PASSTHROUGH_MODEM);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);

		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);
		CHECK(udsp_line_lec_stats(t.s, 0, &lec) == UDSP_OK);
		tm_call_down(&t, &w);
	}
}

static void tm_t38_instance_leg(unsigned rate)
{
	struct fax_fake_stats *fs = fax_fake_stats_get();
	unsigned line_rate = rate ? rate : UDSP_SAMPLE_RATE;
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	char name[128];

	snprintf(name, sizeof(name),
		 "a reconfiguration keeps or replaces the gateway at %u kHz",
		 line_rate / 1000u);
	CASE(name);
	{
		struct eh_gate gate = { .fd = { -1, -1 } };
		struct udsp_media_stats stats;
		uint8_t buf[TM_PKT_MAX];
		uint64_t before = 0;
		unsigned kept = 0;
		unsigned made = 0;
		unsigned first = 0;
		ssize_t n;

		if (!tm_call_up_fax(&t, "USA", rate, &w))
			return;
		tm_t38_media(&m);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 400);
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300);
		CHECK(n == (ssize_t)FAX_FAKE_DGRAM);
		if (n == (ssize_t)FAX_FAKE_DGRAM)
			first = tm_udptl_value(buf);
		CHECK(first > 0u);
		CHECK(udsp_stream_stats(t.s, 0, 0, &stats) == UDSP_OK);
		before = stats.packets_sent;
		CHECK(before > 20u);

		m.remote.port = 5002;
		tm_stream_config(&t, &w, &m);
		CHECK_UINT(fs->creates, 1);
		CHECK_UINT(fs->destroys, 0);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			CHECK_UINT(st->mode, STREAM_MODE_T38);
			CHECK_UINT(st->passthrough_kind,
				   UDSP_PASSTHROUGH_MODEM);
		}
		eh_wait_ms(&t, 100);
		if (!eh_gate_park(&gate))
			goto instance_out;
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (n == (ssize_t)FAX_FAKE_DGRAM)
				kept = tm_udptl_value(buf);
		}
		/* The same instance kept counting across the move, and so did
		 * the session: an adopt would have started both again. */
		CHECK(kept > first);
		CHECK(udsp_stream_stats(t.s, 0, 0, &stats) == UDSP_OK);
		CHECK(stats.packets_sent > before);

		m.t38.max_bit_rate = 9600;
		if (!tm_gate_config_run(&gate, &t, &m, &w))
			goto instance_out;
		CHECK_UINT(fs->creates, 2);
		CHECK_UINT(fs->destroys, 1);
		CHECK_UINT((unsigned)fs->bit_rate, 9600);
		n = tm_recv(w.rtp_peer, buf, sizeof(buf));
		CHECK(n == (ssize_t)FAX_FAKE_DGRAM);
		if (n == (ssize_t)FAX_FAKE_DGRAM)
			made = tm_udptl_value(buf);
		CHECK_UINT(made, 1);
		CHECK(made < kept);
		if (!eh_gate_step(&gate, 2))
			goto instance_out;
		for (unsigned counter = 2; counter <= 3; counter++) {
			n = tm_recv(w.rtp_peer, buf, sizeof(buf));
			CHECK(n == (ssize_t)FAX_FAKE_DGRAM);
			CHECK_UINT(n == (ssize_t)FAX_FAKE_DGRAM ?
				   tm_udptl_value(buf) : 0u, counter);
		}
instance_out:
		eh_gate_disarm(&gate);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		CHECK_UINT(fs->destroys, 2);
		tm_call_close(&t, &w);
		eh_gate_close(&gate);
	}
}

static void tm_t38_round_leg(unsigned rate)
{
	unsigned line_rate = rate ? rate : UDSP_SAMPLE_RATE;
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	char name[128];

	snprintf(name, sizeof(name),
		 "audio to T.38 and back keeps the audio healthy at %u kHz",
		 line_rate / 1000u);
	CASE(name);
	{
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p = { 0 };
		uint32_t ssrc = 0;
		ssize_t n;

		if (!tm_up_fax(&t, "USA", rate) ||
		    !tm_wire_open(&w, 0)) {
			CHECK(false);
			return;
		}
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300);
		CHECK(n > 0);
		if (n > 0 && tm_rtp_parse(buf, (size_t)n, &p))
			ssrc = p.ssrc;
		CHECK(ssrc != 0u);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		/* The socket still holds the audio the stop left in it. */
		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;

		tm_t38_media(&m);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);
		CHECK(tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300) ==
		      (ssize_t)FAX_FAKE_DGRAM);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;

		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			CHECK_UINT(st->mode, STREAM_MODE_AUDIO);
			/* The narrowband codec is converted only where the
			 * line runs wider than it does. */
			CHECK_UINT(st->resample,
				   line_rate != UDSP_SAMPLE_RATE);
			CHECK_UINT(st->codec_frame, UDSP_FRAME_SAMPLES);
			CHECK_UINT(st->line_frame, line_rate / 100u);
			CHECK_UINT(st->ts_per_frame, UDSP_FRAME_SAMPLES);
			CHECK_UINT(st->passthrough_kind, UDSP_PASSTHROUGH_OFF);
		}
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 600);
		CHECK(n == 12 + 160);
		if (n > 0 && tm_rtp_parse(buf, (size_t)n, &p)) {
			double want = tm_ulaw_payload_power(p.payload, p.len,
							    1000.0);
			double image = tm_ulaw_payload_power(p.payload, p.len,
							     2000.0);

			CHECK_UINT(p.pt, 0);
			/* A fresh source, and RFC 3551 marks its first
			 * packet. */
			CHECK(p.ssrc != ssrc);
			CHECK(p.marker);
			CHECK(want > 1e6);
			CHECK(image * 100.0 < want);
		}
		tm_call_down(&t, &w);
	}
}

static void tm_t38_move_leg(unsigned rate)
{
	struct fax_fake_stats *fs = fax_fake_stats_get();
	unsigned line_rate = rate ? rate : UDSP_SAMPLE_RATE;
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	char name[128];

	snprintf(name, sizeof(name),
		 "a mode move without a stop adopts a fresh source at %u kHz",
		 line_rate / 1000u);
	CASE(name);
	{
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p = { 0 };
		uint32_t ssrc = 0;
		ssize_t n;

		if (!tm_call_up_fax(&t, "USA", rate, &w))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300);
		CHECK(n > 0);
		if (n > 0 && tm_rtp_parse(buf, (size_t)n, &p))
			ssrc = p.ssrc;
		CHECK(ssrc != 0u);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			tm_call_close(&t, &w);
			return;
		}

		tm_t38_media(&m);
		tm_stream_config(&t, &w, &m);
		/* Nothing else frees the pair on this path. */
		CHECK(st->ops == NULL);
		CHECK(st->enc == NULL);
		CHECK(st->dec == NULL);
		CHECK_UINT(fs->creates, 1);
		CHECK_UINT(st->mode, STREAM_MODE_T38);
		CHECK_UINT(st->passthrough_kind, UDSP_PASSTHROUGH_MODEM);
		/* The socket still holds the audio of the description the
		 * move replaced. */
		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;
		eh_wait_ms(&t, 100);
		CHECK(tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300) ==
		      (ssize_t)FAX_FAKE_DGRAM);

		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		CHECK_UINT(fs->destroys, 1);
		CHECK(st->ops != NULL);
		CHECK(st->enc != NULL && st->dec != NULL);
		CHECK_UINT(st->mode, STREAM_MODE_AUDIO);
		CHECK_UINT(st->passthrough_kind, UDSP_PASSTHROUGH_OFF);
		while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
			continue;
		eh_wait_ms(&t, 100);
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300);
		CHECK(n == 12 + 160);
		if (n > 0 && tm_rtp_parse(buf, (size_t)n, &p))
			CHECK(p.ssrc != ssrc);
		/* The jitter buffer runs again, which it does not in the
		 * mode the stream just left. */
		{
			struct udsp_jb_stats js;

			CHECK(udsp_stream_jb_stats(t.s, 0, 0, &js) == UDSP_OK);
		}
		tm_call_down(&t, &w);
	}
}

static void tm_t38_stats_leg(unsigned rate)
{
	unsigned line_rate = rate ? rate : UDSP_SAMPLE_RATE;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	char name[128];

	snprintf(name, sizeof(name),
		 "a T.38 stream counts datagrams and publishes zeros at %u kHz",
		 line_rate / 1000u);
	CASE(name);
	{
		struct udsp_media_stats stats;
		struct udsp_jb_stats js;

		if (!tm_call_up_fax(&t, "USA", rate, &w))
			return;
		tm_t38_media(&m);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 300);
		for (unsigned i = 1; i <= 4u; i++)
			tm_udptl_send(&w, i);
		eh_wait_ms(&t, 300);

		CHECK(udsp_stream_stats(t.s, 0, 0, &stats) == UDSP_OK);
		CHECK(stats.packets_sent > 5u);
		CHECK_UINT(stats.bytes_sent,
			   stats.packets_sent * FAX_FAKE_DGRAM);
		CHECK_UINT(stats.packets_recv, 4);
		CHECK_UINT(stats.bytes_recv, 4u * FAX_FAKE_DGRAM);
		CHECK_UINT(stats.packets_lost, 0);
		CHECK_UINT(stats.packets_discarded, 0);
		CHECK_UINT(stats.jitter_avg, 0);
		CHECK_UINT(stats.rtt_avg, 0);
		CHECK(stats.mos_lq == 0.0f);
		CHECK(stats.mos_cq == 0.0f);
		CHECK_UINT(stats.rfactor, 0);
		/* No buffer ran, so the getter refuses exactly as it does for
		 * a stream that has not ticked yet. */
		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &js) == UDSP_UNSUPPORTED);
		/* UDPTL carries no RFC 4733 event and the mode runs no event
		 * sender, so the negotiated payload type reaches nothing. */
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '5', 100) ==
		      UDSP_UNSUPPORTED);
		tm_call_down(&t, &w);
	}
}

static void tm_t38_report_leg(unsigned rate)
{
	struct fax_fake_stats *fs = fax_fake_stats_get();
	unsigned line_rate = rate ? rate : UDSP_SAMPLE_RATE;
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	char name[128];

	snprintf(name, sizeof(name),
		 "no report leaves a T.38 stream at %u kHz or at its stop",
		 line_rate / 1000u);
	CASE(name);
	{
		uint8_t buf[TM_PKT_MAX];

		if (!tm_call_up_fax(&t, "USA", rate, &w))
			return;
		tm_t38_media(&m);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		/* The interval floor is five seconds of engine time, so a
		 * report that was going to leave has left by now. The empty
		 * punches on the report descriptor are not reports and tm_recv
		 * reads past them. */
		eh_wait_ms(&t, 6000);
		CHECK(tm_recv(w.rtcp_peer, buf, sizeof(buf)) < 0);
		/* A datagram that arrives on the report descriptor belongs to
		 * nothing in this mode: it is counted, never demodulated. */
		for (unsigned i = 1; i <= 3u; i++)
			tm_udptl_send_fd(w.rtcp_peer, i);
		eh_wait_ms(&t, 200);
		{
			struct udsp_media_stats stats;

			CHECK(udsp_stream_stats(t.s, 0, 0, &stats) == UDSP_OK);
			CHECK_UINT(stats.packets_discarded, 3);
			CHECK_UINT(stats.packets_recv, 0);
		}
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		CHECK(tm_recv(w.rtcp_peer, buf, sizeof(buf)) < 0);
		CHECK_UINT(fs->in_count, 0);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st)
			CHECK(st->t38_dropped >= 3u);
		tm_call_close(&t, &w);
	}
}

static void tm_t38_event_stop_leg(unsigned rate)
{
	unsigned line_rate = rate ? rate : UDSP_SAMPLE_RATE;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	char name[128];

	snprintf(name, sizeof(name),
		 "a T.38 session reports its end once on the stop at %u kHz",
		 line_rate / 1000u);
	CASE(name);
	{
		if (!tm_call_up_fax(&t, "USA", rate, &w))
			return;
		tm_t38_media(&m);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);
		eh_ev_reset();
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_ev_drain(&t, 200);
		CHECK_UINT(eh_ev_count(UDSP_EV_T38), 1);
		{
			const struct udsp_event *e = eh_ev_find(UDSP_EV_T38, 0);

			CHECK(e != NULL);
			if (e) {
				CHECK_UINT(e->u.t38.state, 0);
				CHECK_UINT(e->u.t38.payload_type, 0);
				CHECK_UINT(e->line, 0);
				CHECK_UINT(e->stream, 0);
			}
		}
		/* The audio call that follows starts and ends without one. */
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_ev_drain(&t, 200);
		CHECK_UINT(eh_ev_count(UDSP_EV_T38), 1);
		tm_call_close(&t, &w);
	}
}

static void tm_t38_event_move_leg(unsigned rate)
{
	unsigned line_rate = rate ? rate : UDSP_SAMPLE_RATE;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	char name[128];

	snprintf(name, sizeof(name),
		 "a T.38 session reports its end on a move to audio at %u kHz",
		 line_rate / 1000u);
	CASE(name);
	{
		if (!tm_call_up_fax(&t, "USA", rate, &w))
			return;
		tm_t38_media(&m);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);
		eh_ev_reset();
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		tm_ev_drain(&t, 200);
		CHECK_UINT(eh_ev_count(UDSP_EV_T38), 1);
		/* And the stop that follows adds none: the session it would
		 * report ended at the move. */
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_ev_drain(&t, 200);
		CHECK_UINT(eh_ev_count(UDSP_EV_T38), 1);
		tm_call_close(&t, &w);
	}
}

static void tm_t38_event_port_leg(unsigned rate)
{
	unsigned line_rate = rate ? rate : UDSP_SAMPLE_RATE;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	char name[128];

	snprintf(name, sizeof(name),
		 "a port move on a T.38 stream reports nothing at %u kHz",
		 line_rate / 1000u);
	CASE(name);
	{
		if (!tm_call_up_fax(&t, "USA", rate, &w))
			return;
		tm_t38_media(&m);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);
		eh_ev_reset();
		m.remote.port = 5002;
		tm_stream_config(&t, &w, &m);
		m.t38.max_bit_rate = 9600;
		tm_stream_config(&t, &w, &m);
		eh_wait_ms(&t, 200);
		tm_ev_drain(&t, 200);
		CHECK_UINT(eh_ev_count(UDSP_EV_T38), 0);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_ev_drain(&t, 200);
		CHECK_UINT(eh_ev_count(UDSP_EV_T38), 1);
		tm_call_close(&t, &w);
	}
}

static void case_t38(void)
{
	struct fax_fake_stats *fs = fax_fake_stats_get();
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;

	CASE("the two T.38 capability bits appear together or not at all");
	{
		if (!tm_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK_UINT(udsp_cap(t.s)->features & UDSP_FEAT_T38, 0);
		CHECK_UINT(udsp_cap(t.s)->codecs & (1u << UDSP_SEL_T38), 0);
		tm_down(&t);

		if (!tm_up_fax(&t, "USA", 0)) {
			CHECK(false);
			return;
		}
		/* The backend offers the upgrade on the feature bit and
		 * accepts the answer on the codec bit, so one without the
		 * other is an offer it would refuse itself. */
		CHECK(udsp_cap(t.s)->features & UDSP_FEAT_T38);
		CHECK(udsp_cap(t.s)->codecs & (1u << UDSP_SEL_T38));
		tm_down(&t);
	}

	tm_t38_wire_leg(0);
	tm_t38_wire_leg(UDSP_SAMPLE_RATE_WB);

	/*
	 * The gateway works at 8 kHz on a 16 kHz line. The frame it is handed
	 * is the decimated line frame, which the rate-error image tells from a
	 * frame that skipped the conversion: a 1000 Hz line tone handed over
	 * whole would read as 2000 Hz at the gateway's own rate.
	 */
	CASE("the gateway runs at 8 kHz on a 16 kHz line in both directions");
	{
		if (!tm_up_fax(&t, "USA", UDSP_SAMPLE_RATE_WB) ||
		    !tm_wire_open(&w, 0)) {
			CHECK(false);
			return;
		}
		tm_t38_media(&m);
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			CHECK(st->resample);
			CHECK_UINT(st->codec_rate, UDSP_SAMPLE_RATE);
			CHECK_UINT(st->codec_frame, UDSP_FRAME_SAMPLES);
			CHECK_UINT(st->line_frame, UDSP_FRAME_MAX);
			CHECK_UINT(st->ts_per_frame, 0);
			CHECK(!st->unit_packet);
			CHECK_UINT(st->mode, STREAM_MODE_T38);
		}
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);
		eh_rec_mark();
		eh_wait_ms(&t, 200);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);

		CHECK_UINT(fs->rx_frames, fs->tx_frames);
		{
			const int16_t *got = fs->rx_last;
			double want = tm_tone_power(got, FAX_FAKE_FRAME, 1000.0,
						    UDSP_SAMPLE_RATE);
			double image = tm_tone_power(got, FAX_FAKE_FRAME,
						     2000.0, UDSP_SAMPLE_RATE);

			CHECK(want > 1e6);
			CHECK(image * 100.0 < want);
		}
		{
			double want = eh_tone_power(0, 1600, FAX_FAKE_TONE_HZ,
						    UDSP_SAMPLE_RATE_WB);
			double image = eh_tone_power(0, 1600,
						     2.0 * FAX_FAKE_TONE_HZ,
						     UDSP_SAMPLE_RATE_WB);

			CHECK(want > 1e6);
			CHECK(image * 100.0 < want);
		}
		tm_call_close(&t, &w);
	}

	tm_t38_hold_leg(0);
	tm_t38_hold_leg(UDSP_SAMPLE_RATE_WB);
	tm_t38_direction_leg(0);
	tm_t38_direction_leg(UDSP_SAMPLE_RATE_WB);

	/* A UDPTL session carries no report session at all, and the goodbye
	 * of a stop goes through the same sender, so neither leaves. */
	tm_t38_report_leg(0);
	tm_t38_report_leg(UDSP_SAMPLE_RATE_WB);

	/* The line canceller is bypassed for as long as the mode stands, and
	 * a line whose canceller is bypassed has no reading to give. */
	tm_t38_lec_leg(0);
	tm_t38_lec_leg(UDSP_SAMPLE_RATE_WB);

	/*
	 * The gateway follows the codec pair's rule: a description that moves
	 * nothing it was created with keeps the instance, and one that moves a
	 * value builds a new one and destroys the old. The count the datagrams
	 * carry is the second witness, because a new instance counts its
	 * frames from the start.
	 */
	tm_t38_instance_leg(0);
	tm_t38_instance_leg(UDSP_SAMPLE_RATE_WB);

	/*
	 * The transition uvoice programs: stop, configure, start, both ways.
	 * The audio stream that comes back is a new source with a fresh rate
	 * conversion, which is what a 16 kHz line proves.
	 */
	tm_t38_round_leg(0);
	tm_t38_round_leg(UDSP_SAMPLE_RATE_WB);

	/*
	 * The same move without a stop. A clockless mode cannot carry an RTP
	 * timeline across it, so both directions adopt: the codec pair goes
	 * when the gateway comes and the gateway goes when the pair comes,
	 * and the audio that returns is a new source.
	 */
	tm_t38_move_leg(0);
	tm_t38_move_leg(UDSP_SAMPLE_RATE_WB);

	/*
	 * What a UDPTL session has to report: its datagrams and its bytes.
	 * There is no receiver, so the quality figures stay zero rather than
	 * describing a call the E-model never measured, and the jitter buffer
	 * has no counters of its own to serve.
	 */
	tm_t38_stats_leg(0);
	tm_t38_stats_leg(UDSP_SAMPLE_RATE_WB);

	/*
	 * hal.h asks for the highest fax rate that does not exceed the
	 * request, and the send bound is the peer's own receive limit clamped
	 * to what one datagram may be. The gateway is created with the
	 * settled values, never with the ones the caller passed.
	 */
	CASE("the fax rate and the datagram size settle before the create");
	{
		static const struct {
			uint32_t ask;
			uint32_t want;
		} rates[] = {
			{ 0u, 14400u },
			{ 1200u, 14400u },
			{ 2400u, 2400u },
			{ 7000u, 4800u },
			{ 10000u, 9600u },
			{ 14400u, 14400u },
			{ 33600u, 14400u },
		};
		static const struct {
			uint16_t ask;
			uint16_t want;
		} sizes[] = {
			{ 200u, 200u },
			{ 0u, 0u },
			{ 576u, 576u },
			{ 1000u, 576u },
		};

		if (!tm_call_up_fax(&t, "USA", 0, &w))
			return;
		for (unsigned i = 0; i < TM_ROWS(rates); i++) {
			tm_t38_media(&m);
			m.t38.max_bit_rate = rates[i].ask;
			tm_stream_config(&t, &w, &m);
			CHECK_UINT((unsigned)fs->bit_rate, rates[i].want);
		}
		for (unsigned i = 0; i < TM_ROWS(sizes); i++) {
			tm_t38_media(&m);
			m.t38.max_datagram = sizes[i].ask;
			tm_stream_config(&t, &w, &m);
			CHECK_UINT((unsigned)fs->datagram, sizes[i].want);
		}
		/* A description that names no report interval takes the floor
		 * the audio path takes, so the tick compares against a real
		 * instant and not against zero. */
		tm_t38_media(&m);
		m.rtcp_interval_ms = 0;
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			CHECK_UINT(st->media.rtcp_interval_ms,
				   UDSP_RTCP_MIN_MS);
			CHECK(!st->media.rtcp_enable);
		}

		/* The error correction reaches create as it was asked for. */
		tm_t38_media(&m);
		m.t38.ec = UDSP_T38_EC_NONE;
		tm_stream_config(&t, &w, &m);
		CHECK_UINT((unsigned)fs->ecc, UDSP_T38_EC_NONE);
		tm_t38_media(&m);
		tm_stream_config(&t, &w, &m);
		CHECK_UINT((unsigned)fs->ecc, UDSP_T38_EC_REDUNDANCY);
		tm_call_close(&t, &w);
	}

	CASE("a T.38 stream refuses the descriptions it cannot carry");
	{
		if (!tm_call_up_fax(&t, "USA", 0, &w))
			return;
		/* The net thread would unprotect a UDPTL datagram as if it
		 * were an SRTP packet. */
		tm_t38_media(&m);
		m.srtp.suite = UDSP_SRTP_AES_CM_128_SHA1_80;
		tm_key_set(m.srtp.key_tx, 0x21);
		tm_key_set(m.srtp.key_rx, 0x55);
		CHECK(udsp_stream_config(t.s, 0, 0, &m, w.rtp_local,
					 w.rtcp_local) == UDSP_UNSUPPORTED);
		tm_t38_media(&m);
		m.t38.ec = UDSP_T38_EC_REDUNDANCY + 1;
		CHECK(udsp_stream_config(t.s, 0, 0, &m, w.rtp_local,
					 w.rtcp_local) == UDSP_UNSUPPORTED);
		/* Nothing was made for either refusal. */
		CHECK_UINT(fs->creates, 0);
		tm_t38_media(&m);
		tm_stream_config(&t, &w, &m);
		CHECK_UINT(fs->creates, 1);
		tm_call_close(&t, &w);

		/* And with no gateway at all the mode is not a codec the
		 * library carries. */
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_t38_media(&m);
		CHECK(udsp_stream_config(t.s, 0, 0, &m, w.rtp_local,
					 w.rtcp_local) == UDSP_UNSUPPORTED);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			CHECK(st->fax_gate.inst == NULL);
			CHECK_UINT(atomic_load(&st->state), STREAM_IDLE);
		}
		tm_call_close(&t, &w);
	}

	/*
	 * The end of a session is reported once. The stop is one edge and the
	 * move back to audio is the other; a description that only moves the
	 * peer port continues the session and reports nothing, and the
	 * payload-type change of the record is never produced, because UDPTL
	 * has no payload type to change.
	 */
	tm_t38_event_stop_leg(0);
	tm_t38_event_stop_leg(UDSP_SAMPLE_RATE_WB);

	tm_t38_event_move_leg(0);
	tm_t38_event_move_leg(UDSP_SAMPLE_RATE_WB);

	tm_t38_event_port_leg(0);
	tm_t38_event_port_leg(UDSP_SAMPLE_RATE_WB);

}

/* --- cases: the fax plug-in loader --------------------------------------- */

#define TM_SO_PATH_MAX	256

/*
 * One stub plug-in, built where the suite runs. The stub restates the
 * descriptor layout instead of including udsp/fax.h, so a silent change to
 * either side shows up here; the slic loader's own suite builds its stubs the
 * same way. The seven slots are the name and the six ops of struct
 * udsp_fax_ops, written out in order, so a member added in the middle fails a
 * case instead of shifting quietly.
 */
static bool tm_stub_build(const char *dir, const char *name, unsigned abi,
			  size_t struct_size, bool with_ops, bool with_name,
			  char *so, size_t cap)
{
	char src[TM_SO_PATH_MAX], cmd[TM_SO_PATH_MAX * 3];
	char body[1024];
	FILE *f;

	if (snprintf(src, sizeof(src), "%s/%s.c", dir, name) < 0)
		return false;
	if (snprintf(so, cap, "%s/%s.so", dir, name) < 0)
		return false;

	snprintf(body, sizeof(body),
		 "static void stub_op(void) { }\n"
		 "static const char stub_name[] = \"stub\";\n"
		 "static const void *stub_ops[7] = {\n"
		 "\t%s,\n"
		 "\t%s, %s, %s, %s, %s, %s\n"
		 "};\n"
		 "struct stub_desc { unsigned abi; unsigned size;"
		 " const void *ops; };\n"
		 "const struct stub_desc udsp_fax_descriptor ="
		 " { %uu, %uu, stub_ops };\n",
		 with_name ? "stub_name" : "0",
		 with_ops ? "stub_op" : "0", with_ops ? "stub_op" : "0",
		 with_ops ? "stub_op" : "0", with_ops ? "stub_op" : "0",
		 with_ops ? "stub_op" : "0", with_ops ? "stub_op" : "0",
		 abi, (unsigned)struct_size);

	f = fopen(src, "w");
	if (!f)
		return false;
	fputs(body, f);
	if (fclose(f))
		return false;

	snprintf(cmd, sizeof(cmd), "cc -shared -fPIC -o '%s' '%s' 2>/dev/null",
		 so, src);

	return system(cmd) == 0;
}

static bool tm_stub_build_empty(const char *dir, char *so, size_t cap)
{
	char src[TM_SO_PATH_MAX], cmd[TM_SO_PATH_MAX * 3];
	FILE *f;

	if (snprintf(src, sizeof(src), "%s/nosym.c", dir) < 0)
		return false;
	if (snprintf(so, cap, "%s/nosym.so", dir) < 0)
		return false;
	f = fopen(src, "w");
	if (!f)
		return false;
	fputs("int udsp_unrelated_symbol;\n", f);
	if (fclose(f))
		return false;

	snprintf(cmd, sizeof(cmd), "cc -shared -fPIC -o '%s' '%s' 2>/dev/null",
		 so, src);

	return system(cmd) == 0;
}

static void case_t38_loader(void)
{
	char tmpl[] = "/tmp/udsp-media-XXXXXX";
	char so[TM_SO_PATH_MAX];
	char cmd[TM_SO_PATH_MAX + 32];
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;

	CASE("the fax plug-in loader answers the ladder the slic loader does");
	{
		char err[FAX_PLUGIN_ERR_MAX];
		struct fax_plugin *p = NULL;

		CHECK(mkdtemp(tmpl) != NULL);
		CHECK_UINT(fax_plugin_load(NULL, &p, err, sizeof(err)),
			   -EINVAL);
		CHECK_UINT(fax_plugin_load("", &p, err, sizeof(err)), -EINVAL);
		CHECK_UINT(fax_plugin_load("/x.so", NULL, err, sizeof(err)),
			   -EINVAL);

		err[0] = '\0';
		CHECK_UINT(fax_plugin_load("/nonexistent/fax.so", &p, err,
					   sizeof(err)), -ENOENT);
		CHECK(strlen(err) > 0);

		CHECK(tm_stub_build_empty(tmpl, so, sizeof(so)));
		err[0] = '\0';
		CHECK_UINT(fax_plugin_load(so, &p, err, sizeof(err)), -ENOEXEC);
		CHECK(strstr(err, "udsp_fax_descriptor") != NULL);

		/* The version gate is an inequality: a plug-in from either
		 * side of the version disagrees about the same layout. */
		CHECK(tm_stub_build(tmpl, "oldabi", UDSP_FAX_ABI_VERSION - 1,
				    sizeof(struct udsp_fax_ops), true, true,
				    so, sizeof(so)));
		p = NULL;
		CHECK_UINT(fax_plugin_load(so, &p, err, sizeof(err)), -EPROTO);
		CHECK(p == NULL);
		CHECK(tm_stub_build(tmpl, "newabi", UDSP_FAX_ABI_VERSION + 1,
				    sizeof(struct udsp_fax_ops), true, true,
				    so, sizeof(so)));
		CHECK_UINT(fax_plugin_load(so, &p, err, sizeof(err)), -EPROTO);
		CHECK(p == NULL);

		CHECK(tm_stub_build(tmpl, "badsize", UDSP_FAX_ABI_VERSION,
				    sizeof(struct udsp_fax_ops) / 2, true, true,
				    so, sizeof(so)));
		CHECK_UINT(fax_plugin_load(so, &p, err, sizeof(err)),
			   -EMSGSIZE);

		CHECK(tm_stub_build(tmpl, "noname", UDSP_FAX_ABI_VERSION,
				    sizeof(struct udsp_fax_ops), true, false,
				    so, sizeof(so)));
		err[0] = '\0';
		CHECK_UINT(fax_plugin_load(so, &p, err, sizeof(err)), -ENOSYS);
		CHECK(strstr(err, "name") != NULL);

		CHECK(tm_stub_build(tmpl, "noops", UDSP_FAX_ABI_VERSION,
				    sizeof(struct udsp_fax_ops), false, true,
				    so, sizeof(so)));
		CHECK_UINT(fax_plugin_load(so, &p, err, sizeof(err)), -ENOSYS);
		CHECK(p == NULL);

		/* And the test-only plug-in, which carries all six. */
		err[0] = '\0';
		CHECK_UINT(fax_plugin_load(UDSP_FAX_FAKE_SO, &p, err,
					   sizeof(err)), 0);
		CHECK(p != NULL);
		CHECK_UINT(strlen(err), 0);
		CHECK(fax_plugin_ops(p) != NULL);
		if (fax_plugin_ops(p)) {
			CHECK(fax_plugin_ops(p)->create != NULL);
			CHECK(fax_plugin_ops(p)->udptl_out != NULL);
			CHECK(!strcmp(fax_plugin_ops(p)->name, "fake"));
		}
		fax_plugin_unload(p);
		fax_plugin_unload(NULL);

		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpl);
		if (system(cmd))
			fprintf(stderr, "# could not remove %s\n", tmpl);
	}

	/*
	 * T.38 is optional, so a file that will not load costs the upgrade and
	 * nothing else: the handle opens, the two bits stay clear, one op
	 * error is counted and the backend's own failover carries the call.
	 */
	CASE("a fax plug-in that will not load leaves the handle open");
	{
		struct udsp_counters ctr;

		tm_fax_path = "/nonexistent/fax.so";
		if (!tm_call_up_rate(&t, "USA", 0, &w))
			return;
		CHECK_UINT(udsp_cap(t.s)->features & UDSP_FEAT_T38, 0);
		CHECK_UINT(udsp_cap(t.s)->codecs & (1u << UDSP_SEL_T38), 0);
		udsp_counters(t.s, &ctr);
		CHECK_UINT(ctr.op_errors, 1);
		tm_t38_media(&m);
		CHECK(udsp_stream_config(t.s, 0, 0, &m, w.rtp_local,
					 w.rtcp_local) == UDSP_UNSUPPORTED);
		/* An audio call on the same handle is untouched. */
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		tm_call_down(&t, &w);
	}

	CASE("the configured plug-in is loaded and carries a stream");
	{
		uint8_t buf[TM_PKT_MAX];
		struct udsp_counters ctr;

		fax_fake_reset();
		tm_fax_path = UDSP_FAX_FAKE_SO;
		if (!tm_call_up_rate(&t, "USA", 0, &w))
			return;
		CHECK(udsp_cap(t.s)->features & UDSP_FEAT_T38);
		CHECK(udsp_cap(t.s)->codecs & (1u << UDSP_SEL_T38));
		udsp_counters(t.s, &ctr);
		CHECK_UINT(ctr.op_errors, 0);
		tm_t38_media(&m);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);
		/* The datagrams come out of the loaded code, not out of the
		 * copy this suite links. */
		CHECK(tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300) ==
		      (ssize_t)FAX_FAKE_DGRAM);
		CHECK_UINT(fax_fake_stats_get()->out_count, 0);
		tm_call_down(&t, &w);
	}

	/*
	 * The unload order. The handle closes with the stream still running,
	 * so the pool shutdown destroys the gateway, and the code that runs
	 * destroy belongs to the loaded object: an unload moved ahead of the
	 * shutdown leaves the tear-down calling into an unmapped page, which
	 * the address sanitizer reports. Nothing else in this process holds a
	 * reference to the object, or the mapping would outlive the mistake.
	 */
	CASE("a live T.38 stream is destroyed before the plug-in is unloaded");
	{
		uint8_t buf[TM_PKT_MAX];

		tm_fax_path = UDSP_FAX_FAKE_SO;
		if (!tm_call_up_rate(&t, "USA", 0, &w))
			return;
		tm_t38_media(&m);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);
		/* The gateway is live: it made this datagram. */
		CHECK(tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300) ==
		      (ssize_t)FAX_FAKE_DGRAM);
		tm_wire_close(&w);
		/* No stream stop: the close is what tears the stream down. */
		tm_down(&t);
		CHECK(t.s == NULL);
	}
}

/* --- cases: the allocation oracle ---------------------------------------- */

/*
 * A window driver. It feeds the far end, lets the engine run, and classifies
 * everything both sockets give back, so a window can prove which branches of
 * the tick ran inside it instead of assuming they did. A window that reports
 * zero allocations over ticks that took no interesting branch proves nothing.
 */
struct tm_alloc_ctx {
	struct eh *t;
	struct tm_wire *w;
	void (*feed)(struct tm_wire *w, void *ctx);
	void *ctx;
	unsigned media;
	unsigned telev;
	unsigned cn;
	unsigned rtcp;
};

static void tm_alloc_step(struct tm_alloc_ctx *a, unsigned ms, bool feed)
{
	uint8_t buf[TM_PKT_MAX];
	struct tm_pkt p = { 0 };
	ssize_t n;

	if (feed && a->feed)
		for (unsigned i = 0; i < ms / 20u; i++)
			a->feed(a->w, a->ctx);
	eh_wait_ms(a->t, ms);
	while ((n = tm_recv(a->w->rtp_peer, buf, sizeof(buf))) > 0) {
		if (!tm_rtp_parse(buf, (size_t)n, &p))
			continue;
		if (p.pt == 101u)
			a->telev++;
		else if (p.pt == 13u)
			a->cn++;
		else
			a->media++;
	}
	while (tm_recv(a->w->rtcp_peer, buf, sizeof(buf)) > 0)
		a->rtcp++;
}

static void tm_alloc_drain(struct tm_alloc_ctx *a)
{
	uint8_t buf[TM_PKT_MAX];

	while (tm_recv(a->w->rtp_peer, buf, sizeof(buf)) > 0)
		continue;
	while (tm_recv(a->w->rtcp_peer, buf, sizeof(buf)) > 0)
		continue;
	a->media = 0;
	a->telev = 0;
	a->cn = 0;
	a->rtcp = 0;
}

/* Every path the tick can take is warmed before the first window opens,
 * because the first tick of a call still installs its description. */
static void tm_alloc_warm(struct tm_alloc_ctx *a)
{
	for (unsigned b = 0; b < 4u; b++)
		tm_alloc_step(a, 100, true);
	/* Without the link options the stubs are dead code and every count
	 * below would read zero for the wrong reason. */
	CHECK(tm_alloc_wrapped());
	tm_alloc_drain(a);
}

struct tm_alloc_g711 {
	double phase;
};

static void tm_alloc_feed_g711(struct tm_wire *w, void *ctx)
{
	struct tm_alloc_g711 *g = ctx;

	tm_tone_send(w, 0, 20, 1000.0, 8000.0, &g->phase);
}

#ifdef HAVE_OPUS
struct tm_alloc_opus {
	struct tm_opus *o;
	double phase;
};

static void tm_alloc_feed_opus(struct tm_wire *w, void *ctx)
{
	struct tm_alloc_opus *a = ctx;

	tm_opus_send(w, a->o, 20, 1000.0, 8000.0, &a->phase);
}
#endif

static void case_alloc(void)
{
	struct tm_alloc_ctx a;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;

	/*
	 * The tick path allocates nothing. Every buffer a tick touches was
	 * allocated when the stream was configured, on the control thread, and
	 * the real-time chain owns no allocator call of its own: not the
	 * receive drain, not the codec, not the jitter buffer, not the sender,
	 * and no release either.
	 *
	 * One window per branch. Steady playout is the cheapest tick there is,
	 * so a single steady window would leave the periodic and the
	 * state-changing paths unmeasured. Each window below drives one branch
	 * and proves it ran from the far end's own count.
	 */
	CASE("the media tick allocates nothing on a narrowband stream");
	{
		struct tm_alloc_g711 g = { 0.0 };

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		memset(&a, 0, sizeof(a));
		a.t = &t;
		a.w = &w;
		a.feed = tm_alloc_feed_g711;
		a.ctx = &g;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		tm_alloc_warm(&a);

		/* Steady media both ways: decode, playout and encode. */
		tm_alloc_window(true);
		for (unsigned b = 0; b < 6u; b++)
			tm_alloc_step(&a, 100, true);
		tm_alloc_window(false);
		CHECK(a.media > 10u);
		CHECK_UINT(tm_alloc_count(), 0);

		/* An RFC 4733 event: the send path builds and protects its own
		 * packets and suppresses the media encode while it runs. */
		tm_alloc_drain(&a);
		tm_alloc_window(true);
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '7', 200) == UDSP_OK);
		for (unsigned b = 0; b < 5u; b++)
			tm_alloc_step(&a, 100, false);
		tm_alloc_window(false);
		CHECK(a.telev >= 4u);
		CHECK_UINT(tm_alloc_count(), 0);

		/* The voice detector's two transitions and the RFC 3389
		 * description that rides the closing one. The backend's own
		 * source is what opens and closes the talkspurt: an out of
		 * band tone would not, because an 8 kHz line aliases it into
		 * the band instead of losing it. */
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.vad = true;
		m.cng = true;
		tm_stream_config(&t, &w, &m);
		tm_alloc_step(&a, 200, true);
		tm_alloc_drain(&a);
		tm_alloc_window(true);
		for (unsigned b = 0; b < 4u; b++) {
			atomic_store(&tm_be.src, b & 1u ? TM_SRC_TONE :
						          TM_SRC_SILENCE);
			tm_alloc_step(&a, 200, true);
		}
		atomic_store(&tm_be.src, TM_SRC_TONE);
		tm_alloc_window(false);
		CHECK(a.cn >= 1u);
		CHECK(a.media > 0u);
		CHECK_UINT(tm_alloc_count(), 0);

		tm_call_down(&t, &w);
	}

	/*
	 * The paths a 600 ms window never reaches: the five-second RTCP
	 * report, the compound the far end sends back, and the state changes
	 * that run on the tick at a quiesce boundary.
	 */
	CASE("no tick allocates through an RTCP report or a stream restart");
	{
		struct tm_alloc_g711 g = { 0.0 };
		struct udsp_stream *st;
		uint8_t rr[48];

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		memset(&a, 0, sizeof(a));
		a.t = &t;
		a.w = &w;
		a.feed = tm_alloc_feed_g711;
		a.ctx = &g;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			tm_call_close(&t, &w);
			return;
		}
		tm_rr_sdes_build(rr, st->sender.ssrc, 13u, 800u);
		tm_alloc_warm(&a);

		/* The interval is floored at five seconds of engine time, so
		 * the window is six and the far end's report goes in halfway,
		 * which puts the send and the receive halves in one window. */
		tm_alloc_window(true);
		for (unsigned b = 0; b < 60u; b++) {
			if (b == 30u)
				CHECK(send(w.rtcp_peer, rr, sizeof(rr),
					   MSG_DONTWAIT | MSG_NOSIGNAL) > 0);
			tm_alloc_step(&a, 100, true);
		}
		tm_alloc_window(false);
		CHECK(a.rtcp >= 1u);
		CHECK_UINT(tm_alloc_count(), 0);
		CHECK(st->stats.loss_fraction_far == 13u);

		/*
		 * A reconfiguration and then a whole restart. The engine half
		 * of both runs at a tick boundary: the deactivate, the BYE the
		 * stop asks for, the adopt that re-inits every machine, and
		 * the activate. The control thread does allocate here, and the
		 * teardown releases the old codec pair, so a window that
		 * counted the wrong thread would report both.
		 */
		tm_alloc_drain(&a);
		tm_alloc_window(true);
		tm_media(&m, UDSP_SEL_PCMA, 8, 30);
		tm_stream_config(&t, &w, &m);
		tm_alloc_step(&a, 200, true);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_alloc_step(&a, 100, false);
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		for (unsigned b = 0; b < 3u; b++)
			tm_alloc_step(&a, 100, true);
		tm_alloc_window(false);
		CHECK(a.media > 0u);
		CHECK_UINT(tm_alloc_count(), 0);

		tm_call_down(&t, &w);
	}

	/*
	 * And nothing on the T.38 path. The gateway's own block is made at the
	 * configuration, on the control thread and outside the window; every
	 * tick inside it runs the demodulator, the datagram intake and the
	 * send loop.
	 */
	CASE("the media tick allocates nothing on a T.38 stream");
	{
		static const unsigned rates[] = { 0u, UDSP_SAMPLE_RATE_WB };

		for (unsigned r = 0; r < 2u; r++) {
			uint8_t buf[TM_PKT_MAX];
			unsigned seq = 0;
			unsigned got = 0;
			ssize_t n;

			if (!tm_up_fax(&t, "USA", rates[r]) ||
			    !tm_wire_open(&w, 0)) {
				CHECK(false);
				return;
			}
			tm_t38_media(&m);
			tm_stream_config(&t, &w, &m);
			tm_capture_tone(1000u);
			CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
			eh_wait_ms(&t, 200);
			while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
				continue;
			CHECK(tm_alloc_wrapped());

			tm_alloc_window(true);
			for (unsigned b = 0; b < 6u; b++) {
				for (unsigned i = 0; i < 5u; i++)
					tm_udptl_send(&w, ++seq);
				eh_wait_ms(&t, 100);
				while ((n = tm_recv(w.rtp_peer, buf,
						    sizeof(buf))) > 0) {
					if (n == (ssize_t)FAX_FAKE_DGRAM)
						got++;
				}
			}
			tm_alloc_window(false);
			CHECK(got > 10u);
			CHECK_UINT(tm_alloc_count(), 0);
			CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
			CHECK(fax_fake_stats_get()->in_count >= 30u);
			tm_call_close(&t, &w);
		}
	}

#ifdef HAVE_OPUS
	/*
	 * And nothing on the whole-packet path either: the transmit staging,
	 * the receive staging and the codec instances are all the stream's
	 * own memory, and libopus allocates in create and destroy alone. The
	 * event window runs the 48 kHz telephone-event send beside it.
	 */
	CASE("the media tick allocates nothing on an Opus stream");
	{
		struct tm_alloc_opus ao;
		struct tm_opus o;

		if (!tm_up_rate(&t, "USA", UDSP_SAMPLE_RATE_WB) ||
		    !tm_wire_open(&w, 0) ||
		    !tm_opus_open(&o, UDSP_SAMPLE_RATE_WB)) {
			CHECK(false);
			return;
		}
		ao.o = &o;
		ao.phase = 0.0;
		memset(&a, 0, sizeof(a));
		a.t = &t;
		a.w = &w;
		a.feed = tm_alloc_feed_opus;
		a.ctx = &ao;
		tm_opus_media(&m, 20);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		tm_alloc_warm(&a);

		tm_alloc_window(true);
		for (unsigned b = 0; b < 6u; b++)
			tm_alloc_step(&a, 100, true);
		tm_alloc_window(false);
		CHECK(a.media > 10u);
		CHECK_UINT(tm_alloc_count(), 0);

		tm_alloc_drain(&a);
		tm_alloc_window(true);
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '7', 200) == UDSP_OK);
		for (unsigned b = 0; b < 5u; b++)
			tm_alloc_step(&a, 100, false);
		tm_alloc_window(false);
		CHECK(a.telev >= 4u);
		CHECK_UINT(tm_alloc_count(), 0);

		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_opus_close(&o);
		tm_call_close(&t, &w);
	}
#endif
}

/* --- cases: the inband tone on a relay stream ---------------------------- */

/*
 * A stream that relays the digit out of band carries no tone in its uplink.
 * Two digit sources drive the cases below. The backend echo loop plays the
 * line's own digit back into the capture, which is the feed the detector
 * reads (the case_fax_profile pattern). The pattern source starts the tone at
 * a sample the case chooses, which is what the leak bound is measured over.
 */

/* The sample value the leak counter reads as tone. A -25 dBm0 pair stands
 * above it and a decoded silent payload does not. */
#define TM_DTMF_BAR	500
#define TM_TELEV_PT	101u

static unsigned tm_ulaw_loud(const uint8_t *pay, size_t len)
{
	unsigned n = 0;

	for (size_t i = 0; i < len; i++) {
		int16_t v = udsp_ulaw_decode(pay[i]);

		if (v > TM_DTMF_BAR || v < -TM_DTMF_BAR)
			n++;
	}

	return n;
}

struct tm_ul_scan {
	unsigned packets;	/* media packets */
	unsigned loud;		/* media packets with a sample past the bar */
	unsigned quiet_run;	/* the longest run of silent media packets */
	unsigned span;		/* the leaked run, in samples */
	unsigned telev;		/* RFC 4733 packets */
	unsigned telev_end;	/* those with the E bit */
	unsigned telev_mark;	/* those with the marker */
	unsigned event_pay;	/* four-byte payloads, an event's own size */
	double pair_max;	/* the strongest 770 plus 1336 Hz packet */
};

/* What the scan carries from packet to packet: the sample index of the next
 * payload, the ends of the leaked run and the run of silent packets. */
struct tm_ul_cur {
	unsigned at;
	unsigned first;
	unsigned last;
	unsigned run;
	bool have;
	bool done;
};

/*
 * The leaked run: the first and the last sample past the bar inside the media
 * packets that carry tone, and nothing past the silent packet that ends them.
 * A count of the samples past the bar reads a quiet digit short, because a
 * -25 dBm0 pair stands past the bar for part of each cycle only.
 */
static void tm_ul_span_note(struct tm_ul_cur *cur, const struct tm_pkt *p,
			    bool silent)
{
	if (cur->done)
		return;
	if (silent) {
		cur->done = cur->have;
		return;
	}
	for (size_t i = 0; i < p->len; i++) {
		int16_t v = udsp_ulaw_decode(p->payload[i]);

		if (v <= TM_DTMF_BAR && v >= -TM_DTMF_BAR)
			continue;
		cur->last = cur->at + (unsigned)i;
		cur->first = cur->have ? cur->first : cur->last;
		cur->have = true;
	}
}

static void tm_ul_note(struct tm_ul_scan *o, const struct tm_pkt *p,
		       struct tm_ul_cur *cur)
{
	bool silent = tm_payload_silent(p->payload, p->len);
	double pair;

	pair = tm_payload_power(p->payload, p->len, false, 770.0) +
	       tm_payload_power(p->payload, p->len, false, 1336.0);
	if (pair > o->pair_max)
		o->pair_max = pair;
	o->packets++;
	o->loud += tm_ulaw_loud(p->payload, p->len) ? 1u : 0u;
	tm_ul_span_note(cur, p, silent);
	cur->at += (unsigned)p->len;
	if (!silent) {
		cur->run = 0;
		return;
	}
	cur->run++;
	if (cur->run > o->quiet_run)
		o->quiet_run = cur->run;
}

static void tm_ul_read(struct tm_wire *w, struct tm_ul_scan *o)
{
	uint8_t buf[TM_PKT_MAX];
	struct tm_ul_cur cur;
	struct tm_pkt p;
	ssize_t n;

	memset(o, 0, sizeof(*o));
	memset(&cur, 0, sizeof(cur));
	while ((n = tm_recv(w->rtp_peer, buf, sizeof(buf))) > 0) {
		if (!tm_rtp_parse(buf, (size_t)n, &p))
			continue;
		o->event_pay += p.len == 4u ? 1u : 0u;
		if (p.pt != TM_TELEV_PT) {
			tm_ul_note(o, &p, &cur);
			continue;
		}
		o->telev++;
		o->telev_mark += p.marker ? 1u : 0u;
		if (p.len >= 4u && (p.payload[1] & 0x80u))
			o->telev_end++;
	}
	o->span = cur.have ? cur.last - cur.first + 1u : 0u;
}

/*
 * The far end's record, in arrival order. The relay cases read the order of
 * the events against the media, which the counting scan above cannot answer.
 * The media fields hold for a G.711 payload alone.
 */
struct tm_rel_pkt {
	uint8_t pt;
	uint8_t event;
	uint8_t volume;
	bool marker;
	bool end;
	bool silent;
	uint16_t seq;
	uint16_t duration;
	uint32_t ts;
	unsigned loud;
};

#define TM_REL_MAX	800u

static struct {
	struct tm_rel_pkt p[TM_REL_MAX];
	unsigned count;
	unsigned dropped;
} tm_rel;

static void tm_rel_reset(void)
{
	memset(&tm_rel, 0, sizeof(tm_rel));
}

static void tm_rel_note(const struct tm_pkt *p)
{
	struct tm_rel_pkt *r;

	if (tm_rel.count >= TM_REL_MAX) {
		tm_rel.dropped++;
		return;
	}
	r = &tm_rel.p[tm_rel.count++];
	memset(r, 0, sizeof(*r));
	r->pt = p->pt;
	r->marker = p->marker;
	r->seq = p->seq;
	r->ts = p->ts;
	if (p->pt == TM_TELEV_PT) {
		r->event = p->len >= 4u ? p->payload[0] : 0xffu;
		r->volume = p->len >= 4u ? p->payload[1] & 0x3fu : 0xffu;
		r->end = p->len >= 4u && (p->payload[1] & 0x80u);
		r->duration = p->len >= 4u ?
			(uint16_t)((p->payload[2] << 8) | p->payload[3]) : 0u;
		return;
	}
	r->silent = tm_payload_silent(p->payload, p->len);
	r->loud = tm_ulaw_loud(p->payload, p->len);
}

static void tm_rel_drain(struct tm_wire *w)
{
	uint8_t buf[TM_PKT_MAX];
	struct tm_pkt p;
	ssize_t n;

	while ((n = tm_recv(w->rtp_peer, buf, sizeof(buf))) > 0) {
		if (tm_rtp_parse(buf, (size_t)n, &p))
			tm_rel_note(&p);
	}
}

/* Reads the far end often enough that a long event cannot outrun the
 * receive buffer. */
static void tm_rel_collect(struct eh *t, struct tm_wire *w, uint32_t ms)
{
	uint32_t end = eh_now_ms() + ms;

	while ((int32_t)(eh_now_ms() - end) < 0) {
		eh_wait_ms(t, 20);
		tm_rel_drain(w);
	}
}

/* A zeroed record for an index the log does not hold, so every read below
 * stays in range and the check that misses says so. */
static struct tm_rel_pkt tm_rel_at(int i)
{
	struct tm_rel_pkt none = { 0 };

	if (i < 0 || (unsigned)i >= tm_rel.count)
		return none;

	return tm_rel.p[i];
}

static int tm_rel_next(uint8_t pt, unsigned from)
{
	for (unsigned i = from; i < tm_rel.count; i++) {
		if (tm_rel.p[i].pt == pt)
			return (int)i;
	}

	return -1;
}

static int tm_rel_mark_next(uint8_t pt, unsigned from)
{
	for (unsigned i = from; i < tm_rel.count; i++) {
		if (tm_rel.p[i].pt == pt && tm_rel.p[i].marker)
			return (int)i;
	}

	return -1;
}

static int tm_rel_prev(uint8_t pt, unsigned before)
{
	for (unsigned i = before; i > 0u; i--) {
		if (tm_rel.p[i - 1u].pt == pt)
			return (int)(i - 1u);
	}

	return -1;
}

static int tm_rel_end_last(void)
{
	for (unsigned i = tm_rel.count; i > 0u; i--) {
		if (tm_rel.p[i - 1u].end)
			return (int)(i - 1u);
	}

	return -1;
}

static unsigned tm_rel_count(uint8_t pt, unsigned from)
{
	unsigned n = 0;

	for (unsigned i = from; i < tm_rel.count; i++)
		n += tm_rel.p[i].pt == pt ? 1u : 0u;

	return n;
}

static unsigned tm_rel_ends(unsigned from)
{
	unsigned n = 0;

	for (unsigned i = from; i < tm_rel.count; i++)
		n += tm_rel.p[i].end ? 1u : 0u;

	return n;
}

static unsigned tm_rel_marks(uint8_t pt, unsigned from)
{
	unsigned n = 0;

	for (unsigned i = from; i < tm_rel.count; i++)
		n += (tm_rel.p[i].pt == pt && tm_rel.p[i].marker) ? 1u : 0u;

	return n;
}

static unsigned tm_rel_loud(void)
{
	unsigned n = 0;

	for (unsigned i = 0; i < tm_rel.count; i++)
		n += (tm_rel.p[i].pt == 0u && tm_rel.p[i].loud) ? 1u : 0u;

	return n;
}

/* Packets of the payload type that closed a segment: the duration field at
 * its maximum with the E bit clear (RFC 4733 2.5.1.3). */
static unsigned tm_rel_segment_closes(void)
{
	unsigned n = 0;

	for (unsigned i = 0; i < tm_rel.count; i++)
		n += (tm_rel.p[i].pt == TM_TELEV_PT && !tm_rel.p[i].end &&
		      tm_rel.p[i].duration == 0xffffu) ? 1u : 0u;

	return n;
}

static void tm_dtmf_run(struct eh *t, struct tm_wire *w, uint32_t ms,
			struct tm_ul_scan *o)
{
	tm_wire_drain(w->rtp_peer);
	atomic_store(&tm_be.src, TM_SRC_PATTERN);
	eh_wait_ms(t, ms);
	atomic_store(&tm_be.src, TM_SRC_SILENCE);
	tm_ul_read(w, o);
}

/* rate 0 takes the narrowband default (udsp.h sample_rate). */
static bool tm_dtmf_up(struct eh *t, struct tm_wire *w, struct udsp_media *m,
		       uint8_t mode, uint8_t dtmf_pt, unsigned rate)
{
	struct udsp_chan_cfg ccfg;

	if (!tm_up_rate(t, "USA", rate) || !tm_wire_open(w, 0))
		return false;
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.detect_mask = UDSP_DET_DTMF;
	if (udsp_chan_config(t->s, 0, &ccfg) != UDSP_OK)
		return false;
	tm_media(m, UDSP_SEL_PCMU, 0, 20);
	m->dtmf_mode = mode;
	m->dtmf_pt = dtmf_pt;
	if (udsp_stream_config(t->s, 0, 0, m, w->rtp_local, w->rtcp_local) !=
	    UDSP_OK)
		return false;

	return udsp_stream_start(t->s, 0, 0) == UDSP_OK;
}

/* The worst leak over the levels and the onset alignments of the supported
 * envelope, which is what the bound is stated over. */
static unsigned tm_dtmf_envelope_worst(struct eh *t, struct tm_wire *w)
{
	static const int level[4] = { -3, -10, -18, -25 };
	static const unsigned lead_ms[4] = { 0, 3, 6, 9 };
	struct tm_ul_scan sc;
	unsigned worst = 0;

	for (unsigned i = 0; i < 16u; i++) {
		tm_dtmf_pat('5', level[i / 4u], UDSP_SAMPLE_RATE,
			    lead_ms[i % 4u], 200);
		tm_dtmf_run(t, w, 400, &sc);
		if (sc.span > worst)
			worst = sc.span;
	}

	return worst;
}

/* How many of the first UDSP_DTMF_MUTE_LOOKAHEAD_FRAMES packets of a restarted
 * stream are silent; power takes the last packet read. */
static unsigned tm_dtmf_restart_read(struct eh *t, struct tm_wire *w,
				     unsigned count, double *power)
{
	uint8_t buf[TM_PKT_MAX];
	unsigned silent = 0;
	struct tm_pkt p;
	ssize_t n;

	*power = 0.0;
	for (unsigned i = 0; i < count; i++) {
		n = tm_recv_wait(t, w->rtp_peer, buf, sizeof(buf), 300);
		if (n <= 0 || !tm_rtp_parse(buf, (size_t)n, &p))
			break;
		if (i < UDSP_DTMF_MUTE_LOOKAHEAD_FRAMES)
			silent += tm_payload_silent(p.payload, p.len) ? 1u : 0u;
		*power = tm_payload_power(p.payload, p.len, false, 1000.0);
	}

	return silent;
}

static bool tm_dtmf_packed_switch(struct eh_gate *gate, struct eh *t,
				  unsigned from, unsigned to)
{
	struct cmd_ack ack[2] = { { .efd = -1 }, { .efd = -1 } };
	struct udsp_cmd cmd[2] = {
		{ .type = UDSP_CMD_STREAM, .stream = (uint8_t)from,
		  .flags = 1, .arg0 = STREAM_OP_DEACTIVATE },
		{ .type = UDSP_CMD_STREAM, .stream = (uint8_t)to,
		  .arg0 = STREAM_OP_ACTIVATE },
	};
	unsigned head;
	unsigned tail;
	bool ok = false;

	/* Public calls serialize on ctl_lock. A direct batch excludes the
	 * idle media tick that would reset the tap between the commands. */
	pthread_mutex_lock(&t->s->ctl_lock);
	head = atomic_load_explicit(&t->s->ctl_ring.head, memory_order_acquire);
	tail = atomic_load_explicit(&t->s->ctl_ring.tail, memory_order_acquire);
	CHECK_UINT(head, tail);
	if (head != tail)
		goto packed_ack_out;
	for (unsigned i = 0; i < 2u; i++) {
		int rc = cmd_ack_init(&ack[i]);

		CHECK_UINT(rc, 0);
		if (rc)
			goto packed_ack_out;
	}
	for (unsigned i = 0; i < 2u; i++) {
		bool pushed;

		cmd[i].token = i + 1u;
		cmd[i].ack = &ack[i];
		cmd_ack_arm(&ack[i], cmd[i].token);
		pushed = cmd_ring_push(&t->s->ctl_ring, &cmd[i]);
		CHECK(pushed);
		if (!pushed)
			exit(tu_summary("media"));
	}
	/* The engine must finish with these stack-owned ack slots. */
	if (!eh_gate_step(gate, 1))
		exit(tu_summary("media"));
	ok = true;
	for (unsigned i = 0; i < 2u; i++) {
		int32_t status = -1;
		int rc = cmd_ack_wait(&ack[i], cmd[i].token, 0, &status);

		CHECK_UINT(rc, 0);
		CHECK(status == 0);
		ok = ok && !rc && !status;
	}
packed_ack_out:
	pthread_mutex_unlock(&t->s->ctl_lock);
	for (unsigned i = 0; i < 2u; i++)
		cmd_ack_release(&ack[i]);

	return ok;
}

static void tm_dtmf_packed_run(void)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	struct tm_wire w[2] = {
		{ .rtp_local = -1, .rtp_peer = -1,
		  .rtcp_local = -1, .rtcp_peer = -1 },
		{ .rtp_local = -1, .rtp_peer = -1,
		  .rtcp_local = -1, .rtcp_peer = -1 },
	};
	struct udsp_media m;
	struct eh t;

	if (!tm_up(&t, "USA") || !tm_wire_open(&w[0], 0) ||
	    !tm_wire_open(&w[1], 0)) {
		CHECK(false);
		goto packed_out;
	}
	tm_media(&m, UDSP_SEL_PCMU, 0, 10);
	m.dtmf_mode = UDSP_DTMF_INFO;
	for (unsigned i = 0; i < 2u; i++)
		CHECK(udsp_stream_config(t.s, 0, i, &m, w[i].rtp_local,
					 w[i].rtcp_local) == UDSP_OK);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	tm_capture_tone(1000u);
	eh_wait_ms(&t, 200);
	if (!eh_gate_park(&gate))
		goto packed_out;
	CHECK_UINT(t.s->chan[0].ul_delay_fill,
		   UDSP_DTMF_MUTE_LOOKAHEAD_FRAMES);
	CHECK_UINT(atomic_load(&stream_at(t.s, 0, 1)->state), STREAM_READY);
	for (unsigned target = 0; target < 2u; target++) {
		unsigned silent;
		double power;

		tm_wire_drain(w[target].rtp_peer);
		if (!tm_dtmf_packed_switch(&gate, &t, 0, target) ||
		    !eh_gate_step(&gate, 5))
			goto packed_out;
		silent = tm_dtmf_restart_read(&t, &w[target], 6u, &power);
		CHECK_UINT(silent, UDSP_DTMF_MUTE_LOOKAHEAD_FRAMES);
		CHECK(power > 1e6);
	}
packed_out:
	eh_gate_disarm(&gate);
	/* Stop the engine before releasing a raw-activated READY stream. */
	tm_down(&t);
	tm_wire_close(&w[0]);
	tm_wire_close(&w[1]);
	eh_gate_close(&gate);
}

/* The line plays its own digit and the echo loop carries it back into the
 * capture at unit gain, three periods behind the playout. */
static void tm_dtmf_echo_arm(struct eh *t, unsigned rate)
{
	unsigned line_rate = rate ? rate : UDSP_SAMPLE_RATE;

	atomic_store(&tm_be.echo_delay, 3u * (line_rate / 100u));
	atomic_store(&tm_be.echo_gain, 1024u);
	atomic_store(&tm_be.src, TM_SRC_ECHO);
	eh_wait_ms(t, 100);
}

static void case_dtmf_mute(void)
{
	struct udsp_chan_cfg ccfg;
	struct tm_ul_scan sc;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;

	/*
	 * The line plays the digit itself, so the detector is held and the
	 * mute comes from the hold alone (chan.c chan_relay_tap_run). The
	 * echo of that digit reaches the capture at unit gain, which is what
	 * the silent uplink is measured against.
	 */
	CASE("a SIP INFO stream leaves the uplink as silence");
	{
		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_INFO, 0, 0)) {
			CHECK(false);
			return;
		}
		tm_dtmf_echo_arm(&t, 0);
		tm_wire_drain(w.rtp_peer);

		CHECK(udsp_digit_play(t.s, 0, '5', 300) == UDSP_OK);
		eh_wait_ms(&t, 500);
		tm_ul_read(&w, &sc);
		CHECK(sc.packets > 20u);
		CHECK(sc.loud <= 1u);
		CHECK(sc.quiet_run >= 15u);
		CHECK_UINT(sc.telev, 0);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 0);

		tm_call_down(&t, &w);
	}

	/* The same hold on a stream that would relay: nothing is detected, so
	 * nothing is relayed and the echo still leaves as silence. */
	CASE("a played digit leaves the uplink as silence and relays nothing");
	{
		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_RFC2833, 101, 0)) {
			CHECK(false);
			return;
		}
		tm_dtmf_echo_arm(&t, 0);
		tm_wire_drain(w.rtp_peer);

		CHECK(udsp_digit_play(t.s, 0, '5', 300) == UDSP_OK);
		eh_wait_ms(&t, 500);
		tm_ul_read(&w, &sc);
		CHECK(sc.loud <= 1u);
		CHECK(sc.quiet_run >= 15u);
		CHECK_UINT(sc.telev, 0);
		CHECK_UINT(sc.telev_end, 0);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 0);
		/* No event ran, so the sender takes a digit of its own. */
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '5', 100) == UDSP_OK);

		tm_call_down(&t, &w);
	}

	/*
	 * The hold ends with the digit and its tail, and the detector it hands
	 * back is a fresh one: the dial digit that follows the played one is
	 * reported whole (chan.c chan_dtmf_arm).
	 */
	CASE("a digit dialled after a played one is reported");
	{
		const struct udsp_event *e;

		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_INFO, 0, 0)) {
			CHECK(false);
			return;
		}
		tm_dtmf_echo_arm(&t, 0);

		CHECK(udsp_digit_play(t.s, 0, '5', 300) == UDSP_OK);
		eh_wait_ms(&t, 600);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 0);

		tm_dtmf_pat('1', -10, UDSP_SAMPLE_RATE, 0, 300);
		tm_dtmf_run(&t, &w, 600, &sc);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);
		e = eh_ev_find(UDSP_EV_DIGIT, 0);
		CHECK(e != NULL);
		if (e) {
			CHECK_UINT(e->u.digit.digit, '1');
			CHECK_UINT(e->u.digit.remote, 0);
		}

		tm_call_down(&t, &w);
	}

	/* The uplink relay itself: a digit the subscriber dialed is detected,
	 * relayed as the event and removed from the media. */
	CASE("a relayed digit leaves the uplink as silence");
	{
		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_RFC2833, 101, 0)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 300);
		tm_dtmf_run(&t, &w, 600, &sc);
		CHECK(sc.loud <= 1u);
		/* The relay replaces the media while the tone sounds, so the
		 * media packets here are the ones around the event. */
		CHECK(sc.quiet_run >= 5u);
		CHECK(sc.telev > 10u);
		CHECK_UINT(sc.telev_end, 3);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);
		/* The tone ended, so the event sender is free again. */
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '5', 100) == UDSP_OK);

		tm_call_down(&t, &w);
	}

	/*
	 * The lookahead in frames, measured from the wire. Four decisions one
	 * frame apart validate the digit, the first of them needs about half
	 * the 205 sample window, and the ring holds three frames: one frame of
	 * the onset reaches the far end. Two frames would say the ring is one
	 * frame short and none would say it is one frame long.
	 */
	CASE("the onset leak is one frame");
	{
		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_INFO, 0, 0)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 300);
		tm_dtmf_run(&t, &w, 600, &sc);
		CHECK(sc.span >= 1u);
		CHECK(sc.span <= 100u);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);

		tm_call_down(&t, &w);
	}

	/*
	 * The supported envelope: the detector's own level range on a warm
	 * window, at every alignment of the onset inside the frame. The leak
	 * stays inside the 20 ms an ETSI ES 201 235-3 receiver may not report.
	 */
	CASE("the leak stays under 20 ms across the envelope");
	{
		unsigned worst;

		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_INFO, 0, 0)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 200);
		worst = tm_dtmf_envelope_worst(&t, &w);
		CHECK(worst >= 1u);
		CHECK(worst <= 160u);
		/* Every run validated, so every leak above is a leak the mute
		 * bounded and not a digit the detector missed. */
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 16);

		tm_call_down(&t, &w);
	}

	/*
	 * The control for the bound above. A burst of 25 ms is twice the leak
	 * the bound admits, and a quiet pair crosses the bar for part of each
	 * cycle only, so an oracle that counted the samples past the bar would
	 * read this burst as 17 ms and pass a leak that is not one.
	 */
	CASE("a 25 ms burst measures as a 25 ms leak");
	{
		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_INBAND, 0, 0)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -25, UDSP_SAMPLE_RATE, 0, 25);
		tm_dtmf_run(&t, &w, 300, &sc);
		CHECK(sc.span >= 180u);
		/* Four decisions validate a digit, so a burst this short is
		 * never one. */
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 0);

		tm_call_down(&t, &w);
	}

	/*
	 * The documented exception. A window the arm just emptied needs two
	 * frames before its first decision, so a digit that starts inside
	 * them validates two frames later and leaks that much more.
	 */
	CASE("a fresh window costs at most two frames more");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		memset(&ccfg, 0, sizeof(ccfg));
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.dtmf_mode = UDSP_DTMF_INFO;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 300);
		tm_wire_drain(w.rtp_peer);

		ccfg.detect_mask = UDSP_DET_DTMF;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		eh_wait_ms(&t, 600);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		tm_ul_read(&w, &sc);
		CHECK(sc.span >= 1u);
		CHECK(sc.span <= 240u);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);

		tm_call_down(&t, &w);
	}

	CASE("an inband stream keeps the tone");
	{
		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_INBAND, 0, 0)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 300);
		tm_dtmf_run(&t, &w, 600, &sc);
		CHECK(sc.loud >= 12u);
		CHECK(sc.pair_max > 1e6);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);

		tm_call_down(&t, &w);
	}

	/* Nothing would relay the digit, so the tone stays where it is. */
	CASE("an event mode with no payload type keeps the tone");
	{
		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_RFC2833, 0, 0)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 300);
		tm_dtmf_run(&t, &w, 600, &sc);
		CHECK(sc.loud >= 12u);
		CHECK(sc.pair_max > 1e6);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);

		tm_call_down(&t, &w);
	}

	CASE("two streams on one line follow their own mode");
	{
		struct tm_ul_scan sc2;
		struct udsp_media band;
		struct tm_wire w2;

		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_INFO, 0, 0) ||
		    !tm_wire_open(&w2, 0)) {
			CHECK(false);
			return;
		}
		tm_media(&band, UDSP_SEL_PCMU, 0, 20);
		band.dtmf_mode = UDSP_DTMF_INBAND;
		CHECK(udsp_stream_config(t.s, 0, 1, &band, w2.rtp_local,
					 w2.rtcp_local) == UDSP_OK);
		CHECK(udsp_stream_start(t.s, 0, 1) == UDSP_OK);
		eh_wait_ms(&t, 200);
		tm_wire_drain(w2.rtp_peer);

		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 300);
		tm_dtmf_run(&t, &w, 600, &sc);
		tm_ul_read(&w2, &sc2);
		CHECK(sc.loud <= 1u);
		CHECK(sc.quiet_run >= 15u);
		CHECK(sc2.loud >= 12u);
		CHECK(sc2.pair_max > 1e6);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);

		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_stream_stop(t.s, 0, 1) == UDSP_OK);
		tm_wire_close(&w);
		tm_call_close(&t, &w2);
	}

	CASE("the ring holds the lookahead while streams run");
	{
		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_INFO, 0, 0)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 200);
		CHECK_UINT(t.s->chan[0].ul_delay_fill,
			   UDSP_DTMF_MUTE_LOOKAHEAD_FRAMES);

		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);
		CHECK_UINT(t.s->chan[0].ul_delay_fill, 0);
		tm_call_close(&t, &w);
	}

	/* The line holds the detector under a carrier, so no digit is decided
	 * and nothing is removed either. */
	CASE("a passthrough stream keeps the digit");
	{
		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_INFO, 0, 0)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 200);
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_OK);

		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 300);
		tm_dtmf_run(&t, &w, 600, &sc);
		CHECK(sc.loud >= 12u);
		CHECK(sc.pair_max > 1e6);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 0);

		tm_call_down(&t, &w);
	}

	CASE("the removal runs on a 16 kHz line");
	{
		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_INFO, 0,
				UDSP_SAMPLE_RATE_WB)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE_WB, 0, 300);
		tm_dtmf_run(&t, &w, 600, &sc);
		CHECK(sc.packets > 20u);
		/* The line frame decimates into the codec frame, so the one
		 * leaked frame may straddle two packets. */
		CHECK(sc.loud <= 2u);
		CHECK(sc.quiet_run >= 15u);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);

		tm_call_down(&t, &w);
	}

	/* The ring empties with the line's last stream, so the first frames of
	 * the next call are its own silence and never the last call's tail. */
	CASE("the tap resets with the streams");
	{
		unsigned silent;
		double power;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.dtmf_mode = UDSP_DTMF_INFO;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		tm_capture_tone(1000u);
		eh_wait_ms(&t, 200);

		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		tm_wire_drain(w.rtp_peer);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		silent = tm_dtmf_restart_read(&t, &w, 6u, &power);
		CHECK_UINT(silent, UDSP_DTMF_MUTE_LOOKAHEAD_FRAMES);
		CHECK(power > 1e6);

		tm_call_down(&t, &w);
	}

	CASE("the tap resets when stop and restart share one drain");
	tm_dtmf_packed_run();

	/*
	 * A stream that becomes active in the middle of a digit never saw the
	 * start of it, so it relays nothing for that digit. The tone is gone
	 * from its uplink all the same: the mark covers the whole ring.
	 */
	CASE("a digit that starts before the stream is not marked as started");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_DTMF;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 800);
		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		eh_wait_ms(&t, 200);

		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 101;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 500);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		eh_wait_ms(&t, 200);
		tm_ul_read(&w, &sc);
		CHECK(sc.packets > 20u);
		CHECK_UINT(sc.loud, 0);
		CHECK_UINT(sc.telev, 0);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);

		tm_call_down(&t, &w);
	}
}

/*
 * One digit with the description pushed again at offset_ms past the end of
 * the tone. The reconfiguration deactivates the stream for about three ticks,
 * so an end the line raised inside that window reaches no transmit tick of
 * this stream.
 */
static void tm_dtmf_reconfig_run(struct eh *t, struct tm_wire *w,
				 struct udsp_media *m, unsigned offset_ms)
{
	struct tm_rel_pkt end;
	int last;

	tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 300);
	tm_rel_reset();
	tm_wire_drain(w->rtp_peer);
	atomic_store(&tm_be.src, TM_SRC_PATTERN);
	eh_wait_ms(t, 300u + offset_ms);
	CHECK(udsp_stream_config(t->s, 0, 0, m, w->rtp_local,
				 w->rtcp_local) == UDSP_OK);
	tm_rel_collect(t, w, 400);
	atomic_store(&tm_be.src, TM_SRC_SILENCE);

	last = tm_rel_end_last();
	end = tm_rel_at(last);
	CHECK_UINT(tm_rel_marks(TM_TELEV_PT, 0), 1);
	CHECK_UINT(tm_rel_ends(0), UDSP_TELEV_REPEAT);
	CHECK(end.duration >= (300u - 100u) * 8u);
	CHECK(end.duration <= (300u + 100u) * 8u);
	/* The audio came back, so no event held the wire to the ceiling. */
	CHECK(last >= 0 && tm_rel_next(0, (unsigned)(last + 1)) > last);
}

#ifdef HAVE_OPUS
/* The duration the last update of the closing segment carried. A close that
 * follows a segment boundary with no update between them answers 0. */
static uint16_t tm_rel_last_update(void)
{
	unsigned i;

	for (i = 0; i < tm_rel.count && !tm_rel.p[i].end; i++)
		continue;
	for (; i > 0u; i--) {
		if (tm_rel.p[i - 1u].pt != TM_TELEV_PT)
			continue;
		if (tm_rel.p[i - 1u].duration == 0xffffu)
			return 0;

		return tm_rel.p[i - 1u].duration;
	}

	return 0;
}

/*
 * What the far end reports for one native event on a 48 kHz clock: the closed
 * segments plus the duration the end packets carry. The event runs one frame
 * less than the tone, because it opens on the fourth decision and the
 * detector rides one frame of the window past the tone before it counts the
 * three missed frames.
 */
static void tm_dtmf_seg_check(unsigned tone_ms)
{
	struct tm_rel_pkt end = tm_rel_at(tm_rel_end_last());
	unsigned closes = tm_rel_segment_closes() / UDSP_TELEV_REPEAT;
	uint32_t total = closes * 0xffffu + end.duration;
	uint32_t want = (tone_ms - 10u) * TM_OPUS_TS_MS;

	CHECK_UINT(tm_rel_marks(TM_TELEV_PT, 0), 1);
	CHECK_UINT(tm_rel_ends(0), UDSP_TELEV_REPEAT);
	CHECK_UINT(tm_rel_segment_closes() % UDSP_TELEV_REPEAT, 0);
	CHECK(closes <= 1u);
	/* A duration the 16 bit field cannot hold reads shorter than the
	 * update before it. */
	CHECK(end.duration >= tm_rel_last_update());
	CHECK(total + 960u >= want);
	CHECK(total <= want + 960u);
}
#endif

/*
 * The relay that the library drives itself (udsp.h UDSP_FEAT_DTMF_RELAY). The
 * event follows the digit of the line. It opens where the detector validates
 * the digit, which is where the mute of case_dtmf_mute opens. It closes where
 * the detector ends the digit.
 */
static void case_dtmf_relay(void)
{
	struct udsp_chan_cfg ccfg;
	struct tm_rel_pkt ev, end;
	struct tm_ul_scan sc;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	int first, last;

	CASE("a detected digit is relayed while it sounds");
	{
		struct tm_rel_pkt pre, post;

		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_RFC2833,
				TM_TELEV_PT, 0)) {
			CHECK(false);
			return;
		}
		CHECK(udsp_cap(t.s)->features & UDSP_FEAT_DTMF_RELAY);
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 300);
		tm_rel_reset();
		tm_wire_drain(w.rtp_peer);
		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		tm_rel_collect(&t, &w, 700);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);

		first = tm_rel_next(TM_TELEV_PT, 0);
		last = tm_rel_end_last();
		ev = tm_rel_at(first);
		end = tm_rel_at(last);
		pre = tm_rel_at(tm_rel_prev(0, first < 0 ? 0u :
					       (unsigned)first));
		post = tm_rel_at(tm_rel_next(0, (unsigned)(last + 1)));
		CHECK_UINT(ev.pt, TM_TELEV_PT);
		CHECK(ev.marker);
		CHECK_UINT(ev.event, 5);
		/* The event opens one packet span past the last media packet,
		 * plus the frames the aggregate settle stepped over. */
		CHECK(ev.ts - pre.ts >= 160u);
		CHECK(ev.ts - pre.ts <= 240u);
		CHECK_UINT(tm_rel_ends(0), 3);
		CHECK(end.duration >= (300u - 40u) * 8u);
		CHECK(end.duration <= (300u + 40u) * 8u);
		/* No media travels while the event runs, and the clock resumes
		 * where the event left it. */
		CHECK_UINT(tm_rel_count(0, (unsigned)first) -
			   tm_rel_count(0, (unsigned)(last + 1)), 0);
		CHECK(post.marker);
		CHECK_UINT(post.ts, ev.ts + end.duration);
		CHECK(tm_rel_loud() <= 1u);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);

		tm_call_down(&t, &w);
	}

	/*
	 * Six 5 ms intervals separate the first decision from validation.
	 * This onset validates in the first half of a frame, so the relay
	 * starts 5 ms later; the end adds zero or 5 ms of frame delay.
	 * The detector duration exceeds the relay span by 30 to 35 ms.
	 * A 30 ms packet interval distinguishes an end on its own tick from
	 * an end delayed to the next packet boundary.
	 */
	CASE("the end lands on the detector's END tick");
	{
		const struct udsp_event *d;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_DTMF;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 30);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = TM_TELEV_PT;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 300);
		tm_rel_reset();
		tm_wire_drain(w.rtp_peer);
		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		tm_rel_collect(&t, &w, 700);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);

		end = tm_rel_at(tm_rel_end_last());
		d = eh_ev_find(UDSP_EV_DIGIT, 0);
		CHECK_UINT(tm_rel_ends(0), 3);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);
		CHECK(d != NULL);
		if (d) {
			CHECK(d->u.digit.duration_ms > 35u);
			CHECK(end.duration >=
			      (d->u.digit.duration_ms - 35u) * 8u);
			CHECK(end.duration <=
			      (d->u.digit.duration_ms - 30u) * 8u);
		}

		tm_call_down(&t, &w);
	}

	CASE("back-to-back digits each get their own event");
	{
		int second;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_DTMF;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 30);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = TM_TELEV_PT;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 200);
		tm_dtmf_pat_add('1', -10, UDSP_SAMPLE_RATE, 0, 300);
		tm_rel_reset();
		tm_wire_drain(w.rtp_peer);
		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		tm_rel_collect(&t, &w, 900);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);

		first = tm_rel_next(TM_TELEV_PT, 0);
		second = tm_rel_mark_next(TM_TELEV_PT,
					  (unsigned)(first + 1));
		CHECK(first >= 0);
		CHECK(second > first);
		CHECK_UINT(tm_rel_at(first).event, 5);
		CHECK_UINT(tm_rel_at(second).event, 1);
		CHECK_UINT(tm_rel_marks(TM_TELEV_PT, 0), 2);
		CHECK_UINT(tm_rel_ends(0), 6);
		/* The first event closed before the second opened. */
		CHECK_UINT(tm_rel_ends(0) - tm_rel_ends((unsigned)second), 3);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 2);

		tm_call_down(&t, &w);
	}

	/* The same digit after a 50 ms pause is a second key press, so it gets
	 * an event of its own, with its own timestamp. */
	CASE("a repeated digit gets its own event");
	{
		int second;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_DTMF;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = TM_TELEV_PT;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 70);
		tm_dtmf_pat_add('5', -10, UDSP_SAMPLE_RATE, 50, 70);
		tm_rel_reset();
		tm_wire_drain(w.rtp_peer);
		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		tm_rel_collect(&t, &w, 600);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);

		first = tm_rel_next(TM_TELEV_PT, 0);
		second = tm_rel_mark_next(TM_TELEV_PT,
					  (unsigned)(first + 1));
		CHECK(first >= 0);
		CHECK(second > first);
		CHECK_UINT(tm_rel_at(first).event, 5);
		CHECK_UINT(tm_rel_at(second).event, 5);
		CHECK(tm_rel_at(second).ts - tm_rel_at(first).ts >= 100u * 8u);
		CHECK_UINT(tm_rel_marks(TM_TELEV_PT, 0), 2);
		CHECK_UINT(tm_rel_ends(0), 6);
		CHECK_UINT(tm_rel_ends(0) - tm_rel_ends((unsigned)second), 3);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 2);

		tm_call_down(&t, &w);
	}

	CASE("the API digit waits for the native event");
	{
		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_RFC2833,
				TM_TELEV_PT, 0)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 400);
		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		eh_wait_ms(&t, 200);
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '7', 100) == UDSP_TRANSIENT);
		eh_wait_ms(&t, 500);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '7', 100) == UDSP_OK);

		tm_call_down(&t, &w);
	}

	/* The analog tone is the line's own digit, so it takes the wire from
	 * an event the caller started. */
	CASE("a detected digit pre-empts an API event");
	{
		int second;

		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_RFC2833,
				TM_TELEV_PT, 0)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 300);
		tm_rel_reset();
		tm_wire_drain(w.rtp_peer);
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '7', 2000) == UDSP_OK);
		eh_wait_ms(&t, 100);
		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		tm_rel_collect(&t, &w, 700);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);

		first = tm_rel_next(TM_TELEV_PT, 0);
		second = tm_rel_mark_next(TM_TELEV_PT,
					  (unsigned)(first + 1));
		CHECK(first >= 0);
		CHECK(second > first);
		CHECK_UINT(tm_rel_at(first).event, 7);
		CHECK_UINT(tm_rel_at(second).event, 5);
		CHECK_UINT(tm_rel_ends(0), 6);
		/* The API event closed where the digit validated, well inside
		 * the 2000 ms it asked for. */
		CHECK_UINT(tm_rel_ends(0) - tm_rel_ends((unsigned)second), 3);
		CHECK(tm_rel_at(second - 1).duration < 2000u * 8u);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);

		tm_call_down(&t, &w);
	}

	/* A held leg puts nothing of its own on the wire, and a digit is as
	 * much its own as the room is (udsp.h hold-tx). */
	CASE("a held stream relays nothing of its own");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_DTMF;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = TM_TELEV_PT;
		m.tx_silence = true;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 300);
		tm_rel_reset();
		tm_wire_drain(w.rtp_peer);
		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		tm_rel_collect(&t, &w, 600);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		CHECK_UINT(tm_rel_count(TM_TELEV_PT, 0), 0);
		CHECK_UINT(tm_rel_loud(), 0);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);

		tm_call_down(&t, &w);
	}

	/* The line holds the detector under a carrier, so there is no digit to
	 * relay and the carrier keeps the wire. */
	CASE("a passthrough stream relays nothing");
	{
		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_RFC2833,
				TM_TELEV_PT, 0)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 200);
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_OK);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 300);
		tm_rel_reset();
		tm_wire_drain(w.rtp_peer);
		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		tm_rel_collect(&t, &w, 600);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		CHECK_UINT(tm_rel_count(TM_TELEV_PT, 0), 0);
		CHECK(tm_rel_loud() >= 12u);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 0);

		tm_call_down(&t, &w);
	}

	CASE("an inband stream relays nothing");
	{
		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_INBAND, 0, 0)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 300);
		tm_rel_reset();
		tm_wire_drain(w.rtp_peer);
		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		tm_rel_collect(&t, &w, 600);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		CHECK_UINT(tm_rel_count(TM_TELEV_PT, 0), 0);
		CHECK(tm_rel_loud() >= 12u);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);

		tm_call_down(&t, &w);
	}

	CASE("two relay streams on one line both carry the event");
	{
		struct udsp_media second;
		struct tm_wire w2;

		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_RFC2833, TM_TELEV_PT,
				0) || !tm_wire_open(&w2, 0)) {
			CHECK(false);
			return;
		}
		tm_media(&second, UDSP_SEL_PCMU, 0, 20);
		second.dtmf_mode = UDSP_DTMF_RFC2833;
		second.dtmf_pt = TM_TELEV_PT;
		CHECK(udsp_stream_config(t.s, 0, 1, &second, w2.rtp_local,
					 w2.rtcp_local) == UDSP_OK);
		CHECK(udsp_stream_start(t.s, 0, 1) == UDSP_OK);
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 300);
		tm_wire_drain(w.rtp_peer);
		tm_wire_drain(w2.rtp_peer);

		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		eh_wait_ms(&t, 600);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		tm_rel_reset();
		tm_rel_drain(&w);
		CHECK_UINT(tm_rel_marks(TM_TELEV_PT, 0), 1);
		CHECK_UINT(tm_rel_ends(0), 3);
		CHECK_UINT(tm_rel_at(tm_rel_next(TM_TELEV_PT, 0)).event, 5);
		tm_rel_reset();
		tm_rel_drain(&w2);
		CHECK_UINT(tm_rel_marks(TM_TELEV_PT, 0), 1);
		CHECK_UINT(tm_rel_ends(0), 3);
		CHECK_UINT(tm_rel_at(tm_rel_next(TM_TELEV_PT, 0)).event, 5);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);

		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_stream_stop(t.s, 0, 1) == UDSP_OK);
		tm_wire_close(&w);
		tm_call_close(&t, &w2);
	}

	/* The ceiling ends the event and the mute stands until the tone does,
	 * so the tone never comes back on the wire behind the event. */
	CASE("the ceiling ends a long event");
	{
		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_RFC2833,
				TM_TELEV_PT, 0)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 8500);
		tm_rel_reset();
		tm_wire_drain(w.rtp_peer);
		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		tm_rel_collect(&t, &w, 9000);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		end = tm_rel_at(tm_rel_end_last());
		CHECK_UINT(tm_rel_marks(TM_TELEV_PT, 0), 1);
		CHECK_UINT(tm_rel_ends(0), 3);
		/* PCMU stamps 8 ticks per millisecond, so 8000 ms fits the
		 * 16 bit duration field and no segment closes. */
		CHECK_UINT(end.duration, UDSP_TELEV_MAX_MS * 8u);
		CHECK_UINT(tm_rel_segment_closes(), 0);
		CHECK(tm_rel_loud() <= 1u);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);

		tm_call_down(&t, &w);
	}

	/* The flush of the disarm ends the digit, so it ends the event with
	 * it. */
	CASE("a disarm ends the event");
	{
		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_RFC2833,
				TM_TELEV_PT, 0)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 800);
		tm_rel_reset();
		tm_wire_drain(w.rtp_peer);
		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		eh_wait_ms(&t, 250);
		tm_rel_drain(&w);
		CHECK(tm_rel_count(TM_TELEV_PT, 0) > 0u);
		CHECK_UINT(tm_rel_ends(0), 0);
		memset(&ccfg, 0, sizeof(ccfg));
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_rel_collect(&t, &w, 200);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		CHECK_UINT(tm_rel_ends(0), 3);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);

		tm_call_down(&t, &w);
	}

	/*
	 * A reconfiguration takes the stream off the tick for about three
	 * ticks, and the line raises its end once. The event must still end,
	 * so the stream closes it whenever the line does not hold the digit
	 * it carries, not on the one tick of the edge.
	 */
	CASE("a reconfiguration across the digit end still ends the event");
	{
		static const unsigned offset_ms[5] = { 0, 10, 20, 30, 40 };

		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_RFC2833,
				TM_TELEV_PT, 0)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 200);
		for (unsigned i = 0; i < 5u; i++)
			tm_dtmf_reconfig_run(&t, &w, &m, offset_ms[i]);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 5);

		tm_call_down(&t, &w);
	}

	/*
	 * rtp.c refuses an event payload type that the codec already uses, so
	 * such a description relays nothing. The removal follows it: a stream
	 * that cannot relay the digit keeps the tone.
	 */
	CASE("an event payload type the codec uses keeps the tone");
	{
		struct udsp_chan_cfg cfg;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		memset(&cfg, 0, sizeof(cfg));
		cfg.detect_mask = UDSP_DET_DTMF;
		CHECK(udsp_chan_config(t.s, 0, &cfg) == UDSP_OK);
		tm_media(&m, UDSP_SEL_PCMU, 96, 20);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = 96;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 300);
		tm_dtmf_run(&t, &w, 600, &sc);
		CHECK(sc.loud >= 12u);
		CHECK(sc.pair_max > 1e6);
		CHECK_UINT(sc.event_pay, 0);
		CHECK(udsp_stream_dtmf(t.s, 0, 0, '5', 100) ==
		      UDSP_UNSUPPORTED);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);

		tm_call_down(&t, &w);
	}

	/* A description that turns a carrier on cuts the event where it
	 * stands: the packets would replace the carrier frames the far end
	 * demodulates, so nothing more leaves, end packets included. */
	CASE("a passthrough profile cuts the event");
	{
		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_RFC2833,
				TM_TELEV_PT, 0)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 800);
		tm_rel_reset();
		tm_wire_drain(w.rtp_peer);
		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		eh_wait_ms(&t, 250);
		tm_rel_drain(&w);
		CHECK(tm_rel_count(TM_TELEV_PT, 0) > 0u);
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_OK);
		/* The return orders old sends before this drain. Later packets
		 * must remain visible as a cancellation failure. */
		tm_wire_drain(w.rtp_peer);
		eh_wait_ms(&t, 100);
		tm_rel_reset();
		tm_rel_collect(&t, &w, 400);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		CHECK_UINT(tm_rel_count(TM_TELEV_PT, 0), 0);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);

		tm_call_down(&t, &w);
	}

	/*
	 * A description that drops the event payload type cuts the event too.
	 * The sender can build no packet without one, so an event left running
	 * would never end: the stop below is what proves the tick still runs.
	 */
	CASE("a description that drops the payload type cuts the event");
	{
		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_RFC2833,
				TM_TELEV_PT, 0)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 800);
		tm_rel_reset();
		tm_wire_drain(w.rtp_peer);
		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		eh_wait_ms(&t, 250);
		tm_rel_drain(&w);
		CHECK(tm_rel_count(TM_TELEV_PT, 0) > 0u);
		m.dtmf_pt = 0;
		tm_stream_config(&t, &w, &m);
		/* The return orders old sends before this drain. Later packets
		 * must remain visible as a cancellation failure. */
		tm_wire_drain(w.rtp_peer);
		eh_wait_ms(&t, 100);
		tm_rel_reset();
		/* Past the end of the tone, so the detector reports the digit
		 * inside the window this reads. */
		tm_rel_collect(&t, &w, 800);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		CHECK_UINT(tm_rel_count(TM_TELEV_PT, 0), 0);
		CHECK(tm_rel_count(0, 0) > 10u);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);

		tm_call_close(&t, &w);
	}

#ifdef HAVE_OPUS
	/*
	 * A 48 kHz clock runs the 16 bit duration field out at 1365 ms, so a
	 * longer digit closes a segment and continues on the same event code
	 * (RFC 4733 2.5.1.3). The close carries the field at its maximum with
	 * the E bit clear, three times, and raises no marker: a receiver that
	 * read one would report the chain as two digits.
	 */
	CASE("a 48 kHz event segments");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_DTMF;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_opus_media(&m, 20);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = TM_TELEV_PT;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, 2000);
		tm_rel_reset();
		tm_wire_drain(w.rtp_peer);
		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		tm_rel_collect(&t, &w, 2400);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		CHECK_UINT(tm_rel_segment_closes(), 3);
		CHECK_UINT(tm_rel_marks(TM_TELEV_PT, 0), 1);
		CHECK_UINT(tm_rel_ends(0), 3);
		CHECK_UINT(tm_rel_at(tm_rel_end_last()).event, 5);
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 1);

		tm_call_down(&t, &w);
	}

	/*
	 * The close runs on the tick the detector ends the digit, which is no
	 * packet boundary, so the segment step runs there as well. A digit
	 * whose end falls between the crossing of the 16 bit field and the
	 * next boundary would otherwise hand the sender a duration it cannot
	 * hold. The pattern source places that end: the event runs one frame
	 * less than the tone, so 1380 ms of tone ends it at 65760 ticks, one
	 * tick past the crossing and one tick before the boundary that would
	 * have closed the segment.
	 */
	CASE("a 48 kHz event segments on the tick the digit ends");
	{
		static const unsigned tone_ms[3] = { 1370u, 1380u, 1390u };

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		memset(&ccfg, 0, sizeof(ccfg));
		ccfg.detect_mask = UDSP_DET_DTMF;
		CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
		tm_opus_media(&m, 20);
		m.dtmf_mode = UDSP_DTMF_RFC2833;
		m.dtmf_pt = TM_TELEV_PT;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);
		for (unsigned i = 0; i < 3u; i++) {
			tm_dtmf_pat('5', -10, UDSP_SAMPLE_RATE, 0, tone_ms[i]);
			tm_rel_reset();
			tm_wire_drain(w.rtp_peer);
			atomic_store(&tm_be.src, TM_SRC_PATTERN);
			tm_rel_collect(&t, &w, tone_ms[i] + 300u);
			atomic_store(&tm_be.src, TM_SRC_SILENCE);
			tm_dtmf_seg_check(tone_ms[i]);
		}
		CHECK_UINT(eh_ev_count(UDSP_EV_DIGIT), 3);

		tm_call_down(&t, &w);
	}
#endif
}

/* The index of the first frame of the record that carries a signal, or the
 * frame count when none does. */
static unsigned tm_rec_loud_first(void)
{
	unsigned frames = eh_rec_have() / UDSP_FRAME_SAMPLES;
	unsigned i;

	for (i = 0; i < frames; i++) {
		if (eh_energy(i * UDSP_FRAME_SAMPLES, UDSP_FRAME_SAMPLES) > 1e5)
			break;
	}

	return i;
}

/* 100 ms of the render, one frame past its first loud frame, so the window
 * holds neither the onset nor the end of the 200 ms event. */
#define TM_TEV_LEVEL_SPAN	(10u * UDSP_FRAME_SAMPLES)

/*
 * One received event of digit 5 at one volume and one SLIC receive value. The
 * case reads the row tone, 770 Hz, at the slot. The SLIC receive value is a
 * loss of the port, which the engine applies to the network audio before the
 * slot (core/chan.c chan_dl).
 */
static bool tm_telev_level_attempt(uint8_t volume, int slic_gain_rx,
				   double want)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	struct udsp_chan_cfg ccfg;
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	bool scheduled = true;
	unsigned from;
	double row;

	if (!tm_telev_up(&t, &w, &m)) {
		CHECK(false);
		return true;
	}
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.slic_gain_rx = (int8_t)slic_gain_rx;
	CHECK(udsp_chan_config(t.s, 0, &ccfg) == UDSP_OK);
	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL);
	if (!st || !eh_gate_park(&gate) ||
	    !tm_gate_telev_prime(&gate, st, &w, 6))
		goto out;
	eh_rec_mark();
	if (!tm_gate_telev_event_run(&gate, st, &w, 5, volume, 10, true,
				     &scheduled) ||
	    !scheduled || !eh_gate_step(&gate, 9))
		goto out;
	CHECK_UINT(eh_rec_have(), 2400);
	if (eh_rec_have() != 2400u)
		goto out;
	CHECK_UINT(tm_rec_loud_frames(), 20);
	from = (tm_rec_loud_first() + 1u) * UDSP_FRAME_SAMPLES;
	row = eh_tone_dbm0(from, TM_TEV_LEVEL_SPAN, 770.0, UDSP_SAMPLE_RATE);
	printf("# level telev volume %u slic_gain_rx %d: row %.2f dBm0, "
	       "want %.0f\n", volume, slic_gain_rx, row, want);
	CHECK(fabs(row - want) <= 0.2);

out:
	eh_gate_disarm(&gate);
	tm_call_down(&t, &w);
	eh_gate_close(&gate);

	return scheduled;
}

static bool tm_telev_volume20_attempt(void)
{
	return tm_telev_level_attempt(20, 0, -20.0);
}

/* The pair with its column 2 dB higher fits int16 only while the row is at
 * -4 dBm0 or lower, so a louder volume plays at -4. */
static bool tm_telev_volume2_attempt(void)
{
	return tm_telev_level_attempt(2, 0, -4.0);
}

/* A slic_gain_rx of -18 half-dB is a loss of 9 dB. */
static bool tm_telev_volume_loss_attempt(void)
{
	return tm_telev_level_attempt(20, -18, -29.0);
}

/*
 * The relay carries the level of a digit in both directions, as in-band audio
 * does. A received event plays its row at the volume the far end announced,
 * then takes the receive loss of the port. A detected digit leaves with the
 * row level the detector measured as its volume.
 */
static void case_telev_level(void)
{
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;

	CASE("a received event plays its row at the announced volume");
	tm_telev_retry(tm_telev_volume20_attempt, "volume 20");

	CASE("a received event louder than -4 dBm0 plays at -4 dBm0");
	tm_telev_retry(tm_telev_volume2_attempt, "volume 2");

	CASE("a received event follows the receive loss");
	tm_telev_retry(tm_telev_volume_loss_attempt, "volume 20 at a loss");

	CASE("a detected digit leaves with the row level it measured");
	{
		unsigned events = 0;
		unsigned bad = 0;

		if (!tm_dtmf_up(&t, &w, &m, UDSP_DTMF_RFC2833,
				TM_TELEV_PT, 0)) {
			CHECK(false);
			return;
		}
		eh_wait_ms(&t, 200);
		tm_dtmf_pat('5', -16, UDSP_SAMPLE_RATE, 0, 300);
		tm_rel_reset();
		tm_wire_drain(w.rtp_peer);
		atomic_store(&tm_be.src, TM_SRC_PATTERN);
		tm_rel_collect(&t, &w, 700);
		atomic_store(&tm_be.src, TM_SRC_SILENCE);

		for (unsigned i = 0; i < tm_rel.count; i++) {
			if (tm_rel.p[i].pt != TM_TELEV_PT)
				continue;
			events++;
			bad += tm_rel.p[i].volume != 16u;
		}
		printf("# relay volume of a -16 dBm0 row: %u event packets, "
		       "%u with another volume, first %u\n", events, bad,
		       tm_rel_at(tm_rel_next(TM_TELEV_PT, 0)).volume);
		CHECK(events >= 4u);
		CHECK_UINT(bad, 0);
		CHECK_UINT(tm_rel_ends(0), UDSP_TELEV_REPEAT);
		tm_call_down(&t, &w);
	}
}

/* --- cases: codec-native DTX (udsp_media.dtx) ---------------------------- */

/*
 * The scripted codec. stream_tx_media dispatches on the encode return alone,
 * so a vtable whose encode replays a script reaches the contract corners no
 * built-in codec produces on a live stream: an untransmitted frame over a
 * staged aggregate, and an encode error over one. Each frame's bytes carry a
 * value derived from its script position, so a flushed packet proves which
 * frames it carried.
 */
static const int *tm_sc_script;
static unsigned tm_sc_pos;

static int tm_sc_encode(struct udsp_codec *c, const int16_t *pcm, unsigned n,
			uint8_t *out, size_t out_cap)
{
	int r = tm_sc_script[tm_sc_pos++];

	(void)c;
	(void)pcm;
	CHECK_UINT(n, 80);
	if (r > 0) {
		CHECK((size_t)r <= out_cap);
		memset(out, (int)(0xa0u + tm_sc_pos), (size_t)r);
	}
	return r;
}

static const struct udsp_codec_ops tm_sc_dtx_ops = {
	.id = UDSP_CODEC_G729,
	.name = "scripted-dtx",
	.sample_rate = 8000,
	.clock_rate = 8000,
	.frame_ms = 10,
	.ptime_mask = 0x07,
	.max_payload = 30,
	.frame_bytes = 10,
	.sid_bytes = 2,
	.encode = tm_sc_encode,
};

static const struct udsp_codec_ops tm_sc_plain_ops = {
	.id = UDSP_CODEC_PCMU,
	.name = "scripted-plain",
	.sample_rate = 8000,
	.clock_rate = 8000,
	.frame_ms = 10,
	.ptime_mask = 0x07,
	.max_payload = 30,
	.frame_bytes = 10,
	.sid_bytes = 0,
	.encode = tm_sc_encode,
};

#define TM_SC_TS0	4000u
#define TM_SC_SEQ0	100u

/* The fabricated stream of the stream.h test hook: everything the fixed-frame
 * transmit arm reads, an open descriptor to catch its packets, and tx_seen so
 * stream_send never dereferences the handle. */
static struct udsp_stream *tm_sc_stream(const struct udsp_codec_ops *ops,
					unsigned fpp, int fd)
{
	struct udsp_stream *st = calloc(1, sizeof(*st));

	if (!st)
		return NULL;
	atomic_init(&st->srtp_armed, false);
	st->ops = ops;
	st->codec_frame = 80;
	st->line_frame = 80;
	st->ts_per_frame = 80;
	st->frames_per_packet = (uint8_t)fpp;
	st->media.direction = UDSP_MEDIA_SENDRECV;
	st->sender.payload_type = 18;
	st->sender.ssrc = 0x5150aa55u;
	st->sender.sequence = TM_SC_SEQ0;
	st->sender.timestamp = TM_SC_TS0;
	st->tx_seen = true;
	st->rtp_fd = fd;
	st->rtcp_fd = -1;
	return st;
}

/* One frame through the hook, then the timestamp invariant: the media clock
 * plus the staged frames advances exactly one frame step per frame, whether
 * the frame was sent, flushed early or suppressed. */
static void tm_sc_frame(struct udsp_stream *st, unsigned *frames)
{
	int16_t pcm[80] = { 0 };

	stream_tx_media(NULL, st, pcm, 80, 0);
	(*frames)++;
	CHECK_UINT(st->sender.timestamp +
		   (uint32_t)st->agg_frames * st->ts_per_frame,
		   TM_SC_TS0 + 80u * *frames);
}

/* The packet the last frames must have produced, byte for byte. */
static void tm_sc_expect(int fd, uint16_t seq, uint32_t ts, bool marker,
			 const uint8_t *pay, size_t len)
{
	uint8_t buf[TM_PKT_MAX];
	struct tm_pkt p;
	ssize_t n = tm_recv(fd, buf, sizeof(buf));

	CHECK(n > 0);
	if (n <= 0)
		return;
	CHECK(tm_rtp_parse(buf, (size_t)n, &p));
	CHECK_UINT(p.pt, 18);
	CHECK_UINT(p.seq, seq);
	CHECK_UINT(p.ts, ts);
	CHECK_UINT(p.marker ? 1u : 0u, marker ? 1u : 0u);
	CHECK_UINT(p.len, len);
	CHECK(p.len == len && !memcmp(p.payload, pay, len));
}

static void tm_sc_expect_none(int fd)
{
	uint8_t buf[TM_PKT_MAX];

	CHECK(tm_recv(fd, buf, sizeof(buf)) < 0);
}

/* The byte a scripted frame filled its output with: the script is 1-based
 * because tm_sc_encode increments before it fills. */
static uint8_t tm_sc_fill(unsigned frame)
{
	return (uint8_t)(0xa0u + frame);
}

/*
 * The close edge of the generic detector through the same hook. Zero PCM never
 * trips the detector, so the hangover alone holds the talkspurt open. The
 * frame count of a talkspurt is therefore a test input.
 */
static void case_vad_close(void)
{
	struct udsp_stream *st;
	unsigned frames = 0;
	uint8_t want[20];
	uint8_t buf[TM_PKT_MAX];
	struct tm_pkt p;
	ssize_t n;
	int fd[2];

	CASE("the generic detector's close flushes the staged frame");
	{
		static const int script[] = {
			10, 10, 10,	/* an odd talkspurt: a packet and a stage */
			10, 10,		/* the resume */
		};

		if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, fd)) {
			CHECK(false);
			return;
		}
		tm_sc_script = script;
		tm_sc_pos = 0;
		st = tm_sc_stream(&tm_sc_plain_ops, 2, fd[0]);
		CHECK(st != NULL);
		if (!st) {
			close(fd[0]);
			close(fd[1]);
			return;
		}
		st->media.vad = true;
		st->media.cng = true;
		st->vad.hangover = 3;

		for (unsigned i = 0; i < 3; i++)
			tm_sc_frame(st, &frames);
		memset(want + 0, tm_sc_fill(1), 10);
		memset(want + 10, tm_sc_fill(2), 10);
		tm_sc_expect(fd[1], TM_SC_SEQ0, TM_SC_TS0, false, want, 20);
		tm_sc_expect_none(fd[1]);
		CHECK(!st->silent);

		/* The close: the staged third frame leaves as its own packet
		 * before the clock steps over the silence. */
		tm_sc_frame(st, &frames);
		memset(want, tm_sc_fill(3), 10);
		tm_sc_expect(fd[1], TM_SC_SEQ0 + 1, TM_SC_TS0 + 160, false,
			     want, 10);
		tm_sc_expect_none(fd[1]);
		CHECK(st->silent);

		/* The silence closes its first packet with the RFC 3389
		 * description, on the timestamp the flush left. */
		tm_sc_frame(st, &frames);
		n = tm_recv(fd[1], buf, sizeof(buf));
		CHECK(n > 0 && tm_rtp_parse(buf, (size_t)n, &p));
		if (n > 0) {
			CHECK_UINT(p.pt, 13);
			CHECK_UINT(p.seq, TM_SC_SEQ0 + 2);
			CHECK_UINT(p.ts, TM_SC_TS0 + 240);
			CHECK_UINT(p.len, 1);
		}
		tm_sc_expect_none(fd[1]);

		/* The resume: marked, and contiguous over the gap. */
		st->vad.hangover = 2;
		tm_sc_frame(st, &frames);
		tm_sc_frame(st, &frames);
		memset(want + 0, tm_sc_fill(4), 10);
		memset(want + 10, tm_sc_fill(5), 10);
		tm_sc_expect(fd[1], TM_SC_SEQ0 + 3, TM_SC_TS0 + 400, true,
			     want, 20);
		CHECK(!st->silent);
		CHECK_UINT(tm_sc_pos, sizeof(script) / sizeof(script[0]));

		free(st);
		close(fd[0]);
		close(fd[1]);
	}
}

static void case_dtx_tx(void)
{
	struct udsp_stream *st;
	unsigned frames = 0;
	uint8_t want[36];
	int fd[2];

	CASE("the encode returns drive DTX: SID ends the packet, 0 flushes first");
	{
		static const int script[] = {
			10, 10, 10,	/* a full packet */
			10, 10, 0,	/* untransmitted over a staged pair */
			0,		/* silence holds */
			2,		/* a SID refresh travels alone */
			0,
			10, 10, 10,	/* resume: the marker packet */
			10, 2,		/* a SID flushes a partial packet */
			0,
		};

		if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, fd)) {
			CHECK(false);
			return;
		}
		tm_sc_script = script;
		tm_sc_pos = 0;
		st = tm_sc_stream(&tm_sc_dtx_ops, 3, fd[0]);
		CHECK(st != NULL);
		if (!st) {
			close(fd[0]);
			close(fd[1]);
			return;
		}

		for (unsigned i = 0; i < 3; i++)
			tm_sc_frame(st, &frames);
		memset(want + 0, tm_sc_fill(1), 10);
		memset(want + 10, tm_sc_fill(2), 10);
		memset(want + 20, tm_sc_fill(3), 10);
		tm_sc_expect(fd[1], TM_SC_SEQ0, TM_SC_TS0, false, want, 30);

		/* Frames 4 and 5 stage; frame 6 returns 0: the staged pair
		 * leaves as its own packet before the clock settles. */
		for (unsigned i = 0; i < 3; i++)
			tm_sc_frame(st, &frames);
		memset(want + 0, tm_sc_fill(4), 10);
		memset(want + 10, tm_sc_fill(5), 10);
		tm_sc_expect(fd[1], TM_SC_SEQ0 + 1, TM_SC_TS0 + 240, false,
			     want, 20);
		tm_sc_expect_none(fd[1]);
		CHECK(st->silent);

		/* An untransmitted frame sends nothing; the clock walked over
		 * it (the invariant check inside tm_sc_frame). */
		tm_sc_frame(st, &frames);
		tm_sc_expect_none(fd[1]);

		/* The SID refresh mid-silence: alone, unmarked, flushed at
		 * one frame. */
		tm_sc_frame(st, &frames);
		memset(want, tm_sc_fill(8), 2);
		tm_sc_expect(fd[1], TM_SC_SEQ0 + 2, TM_SC_TS0 + 560, false,
			     want, 2);
		CHECK(st->silent);

		tm_sc_frame(st, &frames);
		tm_sc_expect_none(fd[1]);

		/* Speech resumes: RFC 3551 marker on the packet that reopens
		 * the talkspurt, timestamps continuous over the gap. */
		for (unsigned i = 0; i < 3; i++)
			tm_sc_frame(st, &frames);
		memset(want + 0, tm_sc_fill(10), 10);
		memset(want + 10, tm_sc_fill(11), 10);
		memset(want + 20, tm_sc_fill(12), 10);
		tm_sc_expect(fd[1], TM_SC_SEQ0 + 3, TM_SC_TS0 + 720, true,
			     want, 30);
		CHECK(!st->silent);

		/* A SID over one staged speech frame closes the packet at two
		 * frames of a three-frame ptime. */
		tm_sc_frame(st, &frames);
		tm_sc_frame(st, &frames);
		memset(want + 0, tm_sc_fill(13), 10);
		memset(want + 10, tm_sc_fill(14), 2);
		tm_sc_expect(fd[1], TM_SC_SEQ0 + 4, TM_SC_TS0 + 960, false,
			     want, 12);
		CHECK(st->silent);

		tm_sc_frame(st, &frames);
		tm_sc_expect_none(fd[1]);
		CHECK_UINT(tm_sc_pos, sizeof(script) / sizeof(script[0]));

		free(st);
		close(fd[0]);
		close(fd[1]);
	}

	CASE("an encode error flushes the staged frames on a plain codec");
	{
		static const int script[] = {
			10, -EIO,	/* the error branch, flush-first */
			10, 10, 10,	/* and the stream carries on unmarked */
			10, 10, -EIO,	/* an error over a filling aggregate */
		};

		if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, fd)) {
			CHECK(false);
			return;
		}
		tm_sc_script = script;
		tm_sc_pos = 0;
		frames = 0;
		st = tm_sc_stream(&tm_sc_plain_ops, 3, fd[0]);
		CHECK(st != NULL);
		if (!st) {
			close(fd[0]);
			close(fd[1]);
			return;
		}

		tm_sc_frame(st, &frames);
		tm_sc_expect_none(fd[1]);
		tm_sc_frame(st, &frames);
		/* The error frame is suppressed, the staged frame is not. */
		memset(want, tm_sc_fill(1), 10);
		tm_sc_expect(fd[1], TM_SC_SEQ0, TM_SC_TS0, false, want, 10);
		tm_sc_expect_none(fd[1]);
		CHECK(!st->silent);

		for (unsigned i = 0; i < 3; i++)
			tm_sc_frame(st, &frames);
		memset(want + 0, tm_sc_fill(3), 10);
		memset(want + 10, tm_sc_fill(4), 10);
		memset(want + 20, tm_sc_fill(5), 10);
		/* No marker: an error is not a silence period. */
		tm_sc_expect(fd[1], TM_SC_SEQ0 + 1, TM_SC_TS0 + 160, false,
			     want, 30);

		/* An error on the frame that would have filled the packet
		 * still sends the two staged frames. */
		for (unsigned i = 0; i < 3; i++)
			tm_sc_frame(st, &frames);
		memset(want + 0, tm_sc_fill(6), 10);
		memset(want + 10, tm_sc_fill(7), 10);
		tm_sc_expect(fd[1], TM_SC_SEQ0 + 2, TM_SC_TS0 + 400, false,
			     want, 20);
		tm_sc_expect_none(fd[1]);
		CHECK_UINT(tm_sc_pos, sizeof(script) / sizeof(script[0]));

		free(st);
		close(fd[0]);
		close(fd[1]);
	}

	CASE("marker discipline across SID, error and stray-zero returns");
	{
		static const int script[] = {
			10, 10,		/* a full ptime-20 packet */
			2,		/* SID with a reconfiguration marker armed */
			10, 10,		/* the marker rides the resume packet */
			0,		/* silence */
			-EIO,		/* an error inside the silence */
			10, 10,		/* resume again */
		};

		if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, fd)) {
			CHECK(false);
			return;
		}
		tm_sc_script = script;
		tm_sc_pos = 0;
		frames = 0;
		st = tm_sc_stream(&tm_sc_dtx_ops, 2, fd[0]);
		CHECK(st != NULL);
		if (!st) {
			close(fd[0]);
			close(fd[1]);
			return;
		}

		tm_sc_frame(st, &frames);
		tm_sc_frame(st, &frames);
		memset(want + 0, tm_sc_fill(1), 10);
		memset(want + 10, tm_sc_fill(2), 10);
		tm_sc_expect(fd[1], TM_SC_SEQ0, TM_SC_TS0, false, want, 20);

		/* rtp_sender_reconfigure arms the marker on any description
		 * change; a SID inside the silence must not wear it. */
		st->sender.marker_pending = true;
		tm_sc_frame(st, &frames);
		memset(want, tm_sc_fill(3), 2);
		tm_sc_expect(fd[1], TM_SC_SEQ0 + 1, TM_SC_TS0 + 160, false,
			     want, 2);
		CHECK(st->sender.marker_pending);
		CHECK(st->silent);

		tm_sc_frame(st, &frames);
		tm_sc_frame(st, &frames);
		memset(want + 0, tm_sc_fill(4), 10);
		memset(want + 10, tm_sc_fill(5), 10);
		tm_sc_expect(fd[1], TM_SC_SEQ0 + 2, TM_SC_TS0 + 240, true,
			     want, 20);

		tm_sc_frame(st, &frames);
		tm_sc_expect_none(fd[1]);
		CHECK(st->silent);
		/* An error while the talkspurt is closed leaves it closed. */
		tm_sc_frame(st, &frames);
		tm_sc_expect_none(fd[1]);
		CHECK(st->silent);

		tm_sc_frame(st, &frames);
		tm_sc_frame(st, &frames);
		memset(want + 0, tm_sc_fill(8), 10);
		memset(want + 10, tm_sc_fill(9), 10);
		tm_sc_expect(fd[1], TM_SC_SEQ0 + 3, TM_SC_TS0 + 560, true,
			     want, 20);
		CHECK_UINT(tm_sc_pos, sizeof(script) / sizeof(script[0]));

		free(st);
		close(fd[0]);
		close(fd[1]);
	}

	CASE("error and zero shapes at ptime 10, and a stray zero stays quiet");
	{
		static const int dtx1[] = { -EIO, 0, 10 };
		static const int plain2[] = { 10, 0, 10, 10 };
		static const int plain1[] = { 0, 10 };

		/* ptime 10, dtx: an error on an empty aggregate sends
		 * nothing, an untransmitted frame closes the talkspurt, the
		 * resume packet is marked. */
		if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, fd)) {
			CHECK(false);
			return;
		}
		tm_sc_script = dtx1;
		tm_sc_pos = 0;
		frames = 0;
		st = tm_sc_stream(&tm_sc_dtx_ops, 1, fd[0]);
		CHECK(st != NULL);
		if (st) {
			tm_sc_frame(st, &frames);
			tm_sc_expect_none(fd[1]);
			CHECK(!st->silent);
			tm_sc_frame(st, &frames);
			tm_sc_expect_none(fd[1]);
			CHECK(st->silent);
			tm_sc_frame(st, &frames);
			memset(want, tm_sc_fill(3), 10);
			tm_sc_expect(fd[1], TM_SC_SEQ0, TM_SC_TS0 + 160, true,
				     want, 10);
			free(st);
		}
		close(fd[0]);
		close(fd[1]);

		/* ptime 20, plain codec: a contract-violating 0 flushes the
		 * staged frame and must not arm the silence state, so no
		 * later packet grows a marker from it. */
		if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, fd)) {
			CHECK(false);
			return;
		}
		tm_sc_script = plain2;
		tm_sc_pos = 0;
		frames = 0;
		st = tm_sc_stream(&tm_sc_plain_ops, 2, fd[0]);
		CHECK(st != NULL);
		if (st) {
			tm_sc_frame(st, &frames);
			tm_sc_frame(st, &frames);
			memset(want, tm_sc_fill(1), 10);
			tm_sc_expect(fd[1], TM_SC_SEQ0, TM_SC_TS0, false,
				     want, 10);
			CHECK(!st->silent);
			tm_sc_frame(st, &frames);
			tm_sc_frame(st, &frames);
			memset(want + 0, tm_sc_fill(3), 10);
			memset(want + 10, tm_sc_fill(4), 10);
			tm_sc_expect(fd[1], TM_SC_SEQ0 + 1, TM_SC_TS0 + 160,
				     false, want, 20);
			free(st);
		}
		close(fd[0]);
		close(fd[1]);

		/* ptime 10, plain codec: the same stray 0 on an empty
		 * aggregate sends nothing and marks nothing. */
		if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, fd)) {
			CHECK(false);
			return;
		}
		tm_sc_script = plain1;
		tm_sc_pos = 0;
		frames = 0;
		st = tm_sc_stream(&tm_sc_plain_ops, 1, fd[0]);
		CHECK(st != NULL);
		if (st) {
			tm_sc_frame(st, &frames);
			tm_sc_expect_none(fd[1]);
			CHECK(!st->silent);
			tm_sc_frame(st, &frames);
			memset(want, tm_sc_fill(2), 10);
			tm_sc_expect(fd[1], TM_SC_SEQ0, TM_SC_TS0 + 80, false,
				     want, 10);
			free(st);
		}
		close(fd[0]);
		close(fd[1]);
	}
}

static void case_dtx_conceal(void)
{
	const struct udsp_codec_ops *ops = &udsp_codec_g729_ops;
	struct udsp_codec_params cp;
	struct udsp_jb_out out;
	struct udsp_stream *st = calloc(1, sizeof(*st));
	uint8_t sid[2] = { 0 };
	int16_t pcm[80];
	int16_t twin_pcm[80];
	bool have_sid = false;

	CASE("a SID-source comfort unit reaches decode and falls back pre-SID");
	CHECK(st != NULL);
	if (!st)
		return;
	st->ops = ops;

	/* A SID that describes a LOUD noise, not silence: the comfort noise it
	 * decodes to must be audible, or every comparison below would also
	 * hold for a ladder that never reached decode, because the plc of a
	 * virgin decoder is as quiet as silence-SID noise. Stationary noise
	 * converges the Annex B detector to non-voice, and the last SID of
	 * the run carries the converged level. */
	{
		struct udsp_codec *enc;
		int16_t pcm_in[80];
		uint32_t lcg = 0x1d872b41u;
		uint8_t out[16];

		memset(&cp, 0, sizeof(cp));
		cp.dtx = true;
		enc = ops->create(&cp);
		CHECK(enc != NULL);
		for (unsigned f = 0; enc && f < 300u; f++) {
			int r;

			for (unsigned i = 0; i < 80u; i++) {
				lcg = lcg * 1664525u + 1013904223u;
				pcm_in[i] = (int16_t)((int)((lcg >> 16) &
							    0x1fffu) - 4096);
			}
			r = ops->encode(enc, pcm_in, 80, out, sizeof(out));
			CHECK(r == 10 || r == 2 || r == 0);
			if (r == 2) {
				memcpy(sid, out, sizeof(sid));
				have_sid = true;
			}
		}
		CHECK(have_sid);
		if (enc)
			ops->destroy(enc);
	}

	memset(&cp, 0, sizeof(cp));

	/* Pre-SID continuation: decode answers -EAGAIN and the unit falls to
	 * the plc ladder, byte for byte what a direct plc call produces. */
	{
		struct udsp_codec *dec = ops->create(&cp);
		struct udsp_codec *twin = ops->create(&cp);

		CHECK(dec && twin);
		if (dec && twin) {
			st->dec = dec;
			memset(&out, 0, sizeof(out));
			out.decision = UDSP_JB_CN;
			out.cn_sid = true;
			stream_conceal_unit(st, &out, pcm, 80);
			CHECK(ops->decode(twin, NULL, 0, twin_pcm, 80) ==
			      -EAGAIN);
			CHECK(ops->plc(twin, twin_pcm, 80, NULL, 0) == 80);
			CHECK(!memcmp(pcm, twin_pcm, sizeof(pcm)));
		}
		if (dec)
			ops->destroy(dec);
		if (twin)
			ops->destroy(twin);
	}

	/* A stored SID update, then the len-0 continuation: both reach the
	 * codec decode, never the RFC 3389 generator and never the ladder.
	 * The loud SID is what makes the dispatch observable: the unit is
	 * audible, equals the direct decode, and differs from what the plc
	 * of a virgin twin produces, so deleting the decode branch fails
	 * here. A decoder that consumed the SID also answers the len-0
	 * continuation differently from the fresh decoder's -EAGAIN
	 * fallback above, which is the two-state contrast of that arm. */
	if (have_sid) {
		struct udsp_codec *dec = ops->create(&cp);
		struct udsp_codec *twin = ops->create(&cp);
		struct udsp_codec *plc_twin = ops->create(&cp);
		int16_t plc_pcm[80];
		bool loud = false;

		CHECK(dec && twin && plc_twin);
		if (dec && twin && plc_twin) {
			st->dec = dec;
			memset(&out, 0, sizeof(out));
			out.decision = UDSP_JB_CN;
			out.cn_sid = true;
			out.payload = sid;
			out.len = 2;
			stream_conceal_unit(st, &out, pcm, 80);
			CHECK(ops->decode(twin, sid, 2, twin_pcm, 80) == 80);
			CHECK(!memcmp(pcm, twin_pcm, sizeof(pcm)));
			for (unsigned i = 0; i < 80u; i++)
				loud = loud || pcm[i];
			CHECK(loud);
			CHECK(ops->plc(plc_twin, plc_pcm, 80, NULL, 0) == 80);
			CHECK(memcmp(pcm, plc_pcm, sizeof(pcm)) != 0);

			out.payload = NULL;
			out.len = 0;
			stream_conceal_unit(st, &out, pcm, 80);
			CHECK(ops->decode(twin, NULL, 0, twin_pcm, 80) == 80);
			CHECK(!memcmp(pcm, twin_pcm, sizeof(pcm)));
			CHECK(memcmp(pcm, plc_pcm, sizeof(pcm)) != 0);
		}
		if (dec)
			ops->destroy(dec);
		if (twin)
			ops->destroy(twin);
		if (plc_twin)
			ops->destroy(plc_twin);
	}

	/* Without cn_sid the RFC 3389 branch stands untouched. */
	{
		struct udsp_codec *dec = ops->create(&cp);
		struct udsp_codec *twin = ops->create(&cp);
		uint8_t level = 0x28;

		CHECK(dec && twin);
		if (dec && twin) {
			st->dec = dec;
			memset(&out, 0, sizeof(out));
			out.decision = UDSP_JB_CN;
			out.payload = &level;
			out.len = 1;
			stream_conceal_unit(st, &out, pcm, 80);
			ops->cn(twin, level, twin_pcm, 80);
			CHECK(!memcmp(pcm, twin_pcm, sizeof(pcm)));
		}
		if (dec)
			ops->destroy(dec);
		if (twin)
			ops->destroy(twin);
	}
	free(st);
}

/*
 * The TX DTX golden. The capture is deterministic: silence, then tm_pat (two
 * voice bursts around an embedded 500 ms hush), then silence again, all
 * frame-aligned. The only unknown is how many silent frames ran before the
 * pattern was armed, so the oracle replays the identical PCM through direct
 * udsp_codec_g729_ops calls for each candidate count and demands one candidate
 * whose packet list matches the wire byte for byte: payloads, markers,
 * timestamp offsets and sequence steps. That one alignment is the proof that
 * the stream transmits exactly what the codec returned on the same PCM, that
 * the clock advanced one frame step per frame whether the frame was sent,
 * flushed early or suppressed, and that the gaps carried nothing.
 *
 * The burst lengths are tuned on the codec: 58 voice frames run 61 encoder
 * speech frames (three frames of hangover) and 45 run 48, stable for any
 * silent prefix past the 32-frame VAD initialization, so the SID after burst
 * one falls on one staged frame (a mixed 12-byte packet at ptime 30) and the
 * SID after burst two falls on an empty aggregate (a 2-byte packet below
 * frames_per_packet at ptime 20 and 30: the early flush of a partial packet).
 */
#define TM_DTX_B1		58u
#define TM_DTX_GAP		50u
#define TM_DTX_B2		45u
#define TM_DTX_PAT_FRAMES	(TM_DTX_B1 + TM_DTX_GAP + TM_DTX_B2)
#define TM_DTX_PKT_MAX		220u
#define TM_DTX_PAY_MAX		36u

static void tm_dtx_pattern_build(void)
{
	static const unsigned start[2] = { 0u, TM_DTX_B1 + TM_DTX_GAP };
	static const unsigned burst[2] = { TM_DTX_B1, TM_DTX_B2 };
	uint32_t lcg = 0x2f6e2b1u;
	unsigned bi = 0;

	memset(tm_pat, 0, sizeof(tm_pat));
	for (unsigned b = 0; b < 2u; b++) {
		for (unsigned f = 0; f < burst[b]; f++, bi++) {
			double env = 0.35 + 0.65 * fabs(sin((double)bi * 0.28));
			int16_t *pcm = tm_pat + (start[b] + f) * 80u;

			for (unsigned i = 0; i < 80u; i++) {
				double s = (double)(bi * 80u + i);
				int noise;
				double v;

				lcg = lcg * 1664525u + 1013904223u;
				noise = (int)((lcg >> 16) & 0x7fffu) / 4 - 4096;
				v = 0.6 * noise +
				    6000.0 * env *
				    sin(2.0 * M_PI * 210.0 * s / 8000.0) +
				    2500.0 * env *
				    sin(2.0 * M_PI * 850.0 * s / 8000.0);
				if (v > 30000.0)
					v = 30000.0;
				if (v < -30000.0)
					v = -30000.0;
				pcm[i] = (int16_t)v;
			}
		}
	}
	atomic_store_explicit(&tm_pat_len, TM_DTX_PAT_FRAMES * 80u,
			      memory_order_release);
}

struct tm_dtx_mpkt {
	uint32_t frame;			/* index of the packet's first frame */
	uint16_t len;
	bool marker;
	uint8_t pay[TM_DTX_PAY_MAX];
};

struct tm_dtx_model {
	struct tm_dtx_mpkt pkt[TM_DTX_PKT_MAX];
	unsigned count;
	unsigned sid_alone;		/* SID flushed on an empty aggregate */
	unsigned sid_mixed;		/* SID flushed onto staged speech */
	unsigned sid_partial;		/* SID flush below frames_per_packet */
	unsigned zero_staged;		/* encode 0 over staged frames */
	unsigned resume_markers;
};

static bool tm_dtx_emit(struct tm_dtx_model *m, uint32_t frame,
			const uint8_t *pay, unsigned len, bool *pending)
{
	struct tm_dtx_mpkt *p;

	if (m->count >= TM_DTX_PKT_MAX || len > TM_DTX_PAY_MAX)
		return false;
	p = &m->pkt[m->count++];
	p->frame = frame;
	p->len = (uint16_t)len;
	p->marker = *pending;
	*pending = false;
	memcpy(p->pay, pay, len);
	return true;
}

/*
 * The oracle's own run of the transmit rules over direct codec calls: k1
 * silent frames, the pattern, silence to the horizon.
 *
 * The uplink is the capture after the offset removal of chan.c
 * chan_ul_offset_remove. The run therefore passes each frame through the
 * production tracker (chan.h chan_ul_dc_frame) before it encodes, so the wire
 * matches bit for bit. The run replays the tracker and does not check it. The
 * silence before the pattern is exact zeros, which the tracker takes as no
 * reading, so the first frame of the pattern seeds the estimate in the line
 * and here.
 */
static bool tm_dtx_model_run(unsigned k1, unsigned fpp, unsigned total,
			     struct tm_dtx_model *m)
{
	static const int16_t quiet[80] = { 0 };
	const struct udsp_codec_ops *ops = &udsp_codec_g729_ops;
	unsigned pat = (unsigned)atomic_load(&tm_pat_len) / 80u;
	struct chan_ul_dc ofs = { 0 };
	struct udsp_codec_params cp;
	struct udsp_codec *enc;
	uint8_t agg[TM_DTX_PAY_MAX];
	int16_t ul[80];
	unsigned agg_len = 0;
	unsigned agg_frames = 0;
	uint32_t agg_frame = 0;
	/* rtp_sender_init raises the marker for a source's first packet. */
	bool pending = true;
	bool silent = false;
	bool ok = true;

	memset(m, 0, sizeof(*m));
	memset(&cp, 0, sizeof(cp));
	cp.dtx = true;
	enc = ops->create(&cp);
	if (!enc)
		return false;
	for (unsigned f = 0; f < total && ok; f++) {
		const int16_t *pcm = quiet;
		int r;

		if (f >= k1 && f - k1 < pat)
			pcm = tm_pat + (f - k1) * 80u;
		memcpy(ul, pcm, sizeof(ul));
		chan_ul_dc_frame(&ofs, ul, 80u, true);
		r = ops->encode(enc, ul, 80, agg + agg_len,
				sizeof(agg) - agg_len);
		if (r == 10) {
			if (silent) {
				silent = false;
				pending = true;
				m->resume_markers++;
			}
			if (!agg_frames)
				agg_frame = f;
			agg_len += 10u;
			agg_frames++;
			if (agg_frames < fpp)
				continue;
			ok = tm_dtx_emit(m, agg_frame, agg, agg_len, &pending);
			agg_len = 0;
			agg_frames = 0;
		} else if (r == 2) {
			/* A SID never wears the marker: it sits inside the
			 * silence and a pending marker rides the next speech
			 * packet, the save-and-restore of the SID arm. */
			bool sid_pending = false;

			if (!agg_frames)
				agg_frame = f;
			if (agg_frames + 1u < fpp)
				m->sid_partial++;
			if (agg_len)
				m->sid_mixed++;
			else
				m->sid_alone++;
			agg_len += 2u;
			ok = tm_dtx_emit(m, agg_frame, agg, agg_len,
					 &sid_pending);
			agg_len = 0;
			agg_frames = 0;
			silent = true;
		} else if (r == 0) {
			if (agg_len) {
				m->zero_staged++;
				ok = tm_dtx_emit(m, agg_frame, agg, agg_len,
						 &pending);
			}
			agg_len = 0;
			agg_frames = 0;
			silent = true;
		} else {
			ok = false;
		}
	}
	ops->destroy(enc);
	return ok;
}

struct tm_dtx_wpkt {
	uint32_t ts;
	uint16_t seq;
	uint16_t len;
	bool marker;
	uint8_t pay[TM_DTX_PAY_MAX];
};

static unsigned tm_dtx_drain(int fd, struct tm_dtx_wpkt *out, unsigned count,
			     unsigned cap, unsigned *bad)
{
	uint8_t buf[TM_PKT_MAX];
	struct tm_pkt p;
	ssize_t n;

	while ((n = tm_recv(fd, buf, sizeof(buf))) > 0) {
		if (!tm_rtp_parse(buf, (size_t)n, &p) || p.pt != 18u ||
		    p.len > TM_DTX_PAY_MAX) {
			(*bad)++;
			continue;
		}
		if (count >= cap) {
			(*bad)++;
			continue;
		}
		out[count].ts = p.ts;
		out[count].seq = p.seq;
		out[count].len = (uint16_t)p.len;
		out[count].marker = p.marker;
		memcpy(out[count].pay, p.payload, p.len);
		count++;
	}
	return count;
}

/* ts0 is the sender's starting timestamp (the entropy draw), so the
 * comparison is absolute: a clock slip before the first captured packet
 * cannot cancel out of every offset. */
static bool tm_dtx_match(const struct tm_dtx_model *m,
			 const struct tm_dtx_wpkt *w, unsigned wn, uint32_t ts0)
{
	if (wn > m->count)
		return false;
	for (unsigned i = 0; i < wn; i++) {
		if (w[i].len != m->pkt[i].len ||
		    memcmp(w[i].pay, m->pkt[i].pay, w[i].len))
			return false;
		if (w[i].marker != m->pkt[i].marker)
			return false;
		if (i && (uint16_t)(w[i].seq - w[i - 1u].seq) != 1u)
			return false;
		if (w[i].ts != ts0 + 80u * m->pkt[i].frame)
			return false;
	}
	return true;
}

static void tm_dtx_wire_run(uint8_t ptime)
{
	static struct tm_dtx_wpkt wpkt[TM_DTX_PKT_MAX];
	struct tm_dtx_model model;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	unsigned fpp = ptime / 10u;
	unsigned matched = 0;
	unsigned wn = 0;
	unsigned bad = 0;
	uint32_t ts0 = 0;
	bool found = false;

	if (!tm_call_up(&t, "USA", &w, 1 << 20))
		return;
	tm_dtx_pattern_build();
	tm_media(&m, UDSP_SEL_G729, 18, ptime);
	m.dtx = true;
	tm_stream_config(&t, &w, &m);
	/* The sender stamps its first frame with the entropy draw, read here
	 * before the stream runs, so the oracle compares absolute
	 * timestamps. */
	{
		const struct udsp_stream *stp = stream_at(t.s, 0, 0);

		CHECK(stp != NULL);
		if (stp)
			ts0 = stp->entropy.timestamp;
	}
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	/* Far enough past the VAD initialization that the encoder's speech
	 * runs are prefix-invariant; the replay still matches any prefix. */
	eh_wait_ms(&t, 500);
	wn = tm_dtx_drain(w.rtp_peer, wpkt, wn, TM_DTX_PKT_MAX, &bad);
	atomic_store(&tm_pat_pos, 0u);
	atomic_store(&tm_be.src, TM_SRC_PATTERN);
	for (unsigned i = 0; i < 11u; i++) {
		eh_wait_ms(&t, 200);
		wn = tm_dtx_drain(w.rtp_peer, wpkt, wn, TM_DTX_PKT_MAX, &bad);
	}
	CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
	wn = tm_dtx_drain(w.rtp_peer, wpkt, wn, TM_DTX_PKT_MAX, &bad);
	/* Nothing but the G.729 stream itself: no PT 13, no stray shape. */
	CHECK_UINT(bad, 0);
	CHECK(wn > 10u);

	for (unsigned k = 20; k <= 400u && !found; k++) {
		if (!tm_dtx_model_run(k, fpp, k + TM_DTX_PAT_FRAMES + 60u,
				      &model))
			continue;
		if (tm_dtx_match(&model, wpkt, wn, ts0)) {
			found = true;
			matched = k;
		}
	}
	CHECK(found);
	if (found) {
		/* The wire carries every packet the oracle predicted inside
		 * the horizon: the gaps carried nothing and nothing is
		 * missing, the flushed partials included. */
		CHECK_UINT(wn, model.count);
		CHECK(matched >= 33u);
		CHECK(model.resume_markers >= 2u);
		CHECK(model.sid_alone >= 2u);
		if (fpp >= 2u) {
			CHECK(model.sid_partial >= 1u);
			CHECK(model.sid_mixed >= 1u);
		}
		/* The G.729 encoder closes every talkspurt with a SID, so
		 * the untransmitted-over-staged flush never fires here; the
		 * scripted-codec case binds that branch. */
		CHECK_UINT(model.zero_staged, 0);
	}
	tm_call_close(&t, &w);
}

static void case_dtx_wire(void)
{
	CASE("g729 TX DTX golden at ptime 10: the wire equals the codec replay");
	tm_dtx_wire_run(10);
	CASE("g729 TX DTX golden at ptime 20: the SID flushes a partial packet");
	tm_dtx_wire_run(20);
	CASE("g729 TX DTX golden at ptime 30: the SID flushes a partial packet");
	tm_dtx_wire_run(30);
}

/* The next pt-18 packet of the asked shape (SID-tailed or plain speech),
 * skipping the other shape, within ms of engine time. */
static bool tm_dtx_next(struct eh *t, int fd, bool sid, struct tm_pkt *p,
			uint8_t *buf, size_t cap, uint32_t ms)
{
	uint32_t end = eh_now_ms() + ms;

	for (;;) {
		uint32_t now = eh_now_ms();
		ssize_t n;

		if ((int32_t)(end - now) <= 0)
			return false;
		n = tm_recv_wait(t, fd, buf, cap, end - now);
		if (n <= 0)
			return false;
		if (!tm_rtp_parse(buf, (size_t)n, p) || p->pt != 18u)
			continue;
		if ((p->len % 10u == 2u) == sid)
			return true;
	}
}

/* The two reconfiguration edges of the marker discipline. */
static void case_dtx_marker(void)
{
	uint8_t buf[TM_PKT_MAX];
	struct udsp_media m;
	struct tm_wire w;
	struct tm_pkt p;
	struct eh t;

	CASE("disabling dtx mid-silence marks the first continuous packet");
	{
		if (!tm_call_up(&t, "USA", &w, 1 << 20))
			return;
		tm_media(&m, UDSP_SEL_G729, 18, 20);
		m.dtx = true;
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 300);
		tm_wire_drain(w.rtp_peer);

		/* The talkspurt closes on an unmarked SID. */
		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		CHECK(tm_dtx_next(&t, w.rtp_peer, true, &p, buf, sizeof(buf),
				  2000));
		CHECK(!p.marker);
		eh_wait_ms(&t, 100);
		tm_wire_drain(w.rtp_peer);

		/* The sender sees no description field move on a dtx flip,
		 * so the engine's reconfig raises the marker itself: the
		 * suppression before it really happened. */
		m.dtx = false;
		tm_stream_config(&t, &w, &m);
		CHECK(tm_dtx_next(&t, w.rtp_peer, false, &p, buf, sizeof(buf),
				  1000));
		CHECK(p.marker);
		CHECK_UINT(p.len, 20);
		CHECK(tm_dtx_next(&t, w.rtp_peer, false, &p, buf, sizeof(buf),
				  1000));
		CHECK(!p.marker);

		tm_call_down(&t, &w);
	}

	CASE("a ptime move mid-silence keeps the marker for the resume packet");
	{
		if (!tm_call_up(&t, "USA", &w, 1 << 20))
			return;
		tm_media(&m, UDSP_SEL_G729, 18, 20);
		m.dtx = true;
		tm_stream_config(&t, &w, &m);
		tm_capture_tone(1000u);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 300);
		tm_wire_drain(w.rtp_peer);

		atomic_store(&tm_be.src, TM_SRC_SILENCE);
		CHECK(tm_dtx_next(&t, w.rtp_peer, true, &p, buf, sizeof(buf),
				  2000));
		CHECK(!p.marker);

		/* The ptime change arms the sender's own marker; every SID
		 * that follows must leave it standing for the resume. */
		tm_media(&m, UDSP_SEL_G729, 18, 30);
		m.dtx = true;
		tm_stream_config(&t, &w, &m);
		eh_wait_ms(&t, 200);
		{
			ssize_t n;

			while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
				if (!tm_rtp_parse(buf, (size_t)n, &p) ||
				    p.pt != 18u)
					continue;
				/* Anything in the silence is a SID and rides
				 * unmarked. */
				CHECK_UINT(p.len % 10u, 2);
				CHECK(!p.marker);
			}
		}

		atomic_store(&tm_be.src, TM_SRC_TONE);
		CHECK(tm_dtx_next(&t, w.rtp_peer, false, &p, buf, sizeof(buf),
				  1000));
		CHECK(p.marker);
		CHECK_UINT(p.len, 30);

		tm_call_down(&t, &w);
	}
}

/*
 * Every fixed-frame codec hands the buffer its frame size. A packet below the
 * learned ptime is therefore stamped by its payload (jb.h Units). An example
 * is the close of a talkspurt on an odd frame count. test_jb carries the
 * slicing check.
 */
static void case_jb_frame_bytes(void)
{
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;

	CASE("the buffer learns every fixed-frame codec's frame size");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			tm_call_close(&t, &w);
			return;
		}
		CHECK_UINT(st->jb.frame_bytes, 80);
		CHECK_UINT(st->jb.sid_len, 0);

		tm_media(&m, UDSP_SEL_G726_32, 111, 20);
		tm_stream_config(&t, &w, &m);
		CHECK_UINT(st->jb.frame_bytes, 40);
		CHECK_UINT(st->jb.sid_len, 0);

		tm_media(&m, UDSP_SEL_G729, 18, 20);
		tm_stream_config(&t, &w, &m);
		CHECK_UINT(st->jb.frame_bytes, 10);
		CHECK_UINT(st->jb.sid_len, 2);
		tm_call_close(&t, &w);
	}
}

/* The description rule of the generic pair: vad stays set only beside cng,
 * and the clear reaches the wire as continuous sending. */
static void case_vad_media(void)
{
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;

	CASE("the generic detector runs only beside comfort noise");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.vad = true;
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			tm_call_close(&t, &w);
			return;
		}
		CHECK(!st->media.vad);
		CHECK(!st->media.cng);

		/* A dtx that means nothing on the codec cannot stand in for
		 * the noise: the clear follows the dtx rules. */
		m.dtx = true;
		tm_stream_config(&t, &w, &m);
		CHECK(!st->media.vad);
		CHECK(!st->media.dtx);

		m.dtx = false;
		m.cng = true;
		tm_stream_config(&t, &w, &m);
		CHECK(st->media.vad);
		CHECK(st->media.cng);

		/* The library carries no codec policy: G.729 keeps the generic
		 * pair when the description asks for it beside the noise. */
		tm_media(&m, UDSP_SEL_G729, 18, 20);
		m.vad = true;
		m.cng = true;
		tm_stream_config(&t, &w, &m);
		CHECK(st->media.vad);
		CHECK(st->media.cng);
		CHECK(!st->media.dtx);

		/* The fax profile restores the prepared description, so a
		 * cleared vad stays cleared across it. */
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.vad = true;
		tm_stream_config(&t, &w, &m);
		CHECK(!st->media.vad);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_OK);
		CHECK(!st->media.vad);
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_OFF, 0, 0) ==
		      UDSP_OK);
		CHECK(!st->media.vad);
		CHECK(!st->media.cng);
		tm_call_down(&t, &w);
	}

	CASE("comfort noise withdrawn mid-silence reopens continuous sending");
	{
		uint8_t buf[TM_PKT_MAX];
		struct tm_pkt p;
		unsigned media_pkts = 0;
		unsigned cn_pkts = 0;
		bool marked = false;
		uintptr_t enc;
		uintptr_t dec;
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.vad = true;
		m.cng = true;
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			tm_call_close(&t, &w);
			return;
		}
		enc = (uintptr_t)st->enc;
		dec = (uintptr_t)st->dec;
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 200);
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (!tm_rtp_parse(buf, (size_t)n, &p))
				continue;
			if (p.pt == 13u)
				cn_pkts++;
			else
				media_pkts++;
		}
		CHECK(cn_pkts >= 1u);
		CHECK_UINT(media_pkts, 0);
		CHECK(st->silent);

		/* The same codec at the same clock: the pair stands, the
		 * detector goes, and the silence it held ends on a marked
		 * packet of a full ptime. */
		m.cng = false;
		tm_stream_config(&t, &w, &m);
		CHECK(!st->media.vad);
		CHECK((uintptr_t)st->enc == enc);
		CHECK((uintptr_t)st->dec == dec);
		eh_wait_ms(&t, 200);
		cn_pkts = 0;
		while ((n = tm_recv(w.rtp_peer, buf, sizeof(buf))) > 0) {
			if (!tm_rtp_parse(buf, (size_t)n, &p))
				continue;
			if (p.pt == 13u) {
				cn_pkts++;
				continue;
			}
			if (!media_pkts)
				marked = p.marker;
			else
				CHECK(!p.marker);
			CHECK_UINT(p.len, 160);
			media_pkts++;
		}
		CHECK_UINT(cn_pkts, 0);
		CHECK(media_pkts > 5u);
		CHECK(marked);
		tm_call_down(&t, &w);
	}
}

static void case_dtx_media(void)
{
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;

	CASE("dtx replaces the generic pair, dies without sid_bytes, survives fax");
	{
		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_G729, 18, 20);
		m.vad = true;
		m.cng = true;
		m.dtx = true;
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			tm_call_close(&t, &w);
			return;
		}
		/* The codec-native pair forces the generic one off. */
		CHECK(st->media.dtx);
		CHECK(!st->media.vad);
		CHECK(!st->media.cng);
		CHECK(st->params.dtx);

		/* A codec with no SID frame clears the flag and keeps the
		 * generic pair. */
		tm_media(&m, UDSP_SEL_PCMU, 0, 20);
		m.vad = true;
		m.cng = true;
		m.dtx = true;
		tm_stream_config(&t, &w, &m);
		CHECK(!st->media.dtx);
		CHECK(st->media.vad);
		CHECK(st->media.cng);
		CHECK(!st->params.dtx);

		/* A passthrough description clears it beside vad and cng. */
		tm_media(&m, UDSP_SEL_G729, 18, 20);
		m.dtx = true;
		m.passthrough = UDSP_PASSTHROUGH_MODEM;
		tm_stream_config(&t, &w, &m);
		CHECK(!st->media.dtx);

		/* The fax profile bypasses media_prepare, so the profile arm
		 * clears the flag itself and the off arm restores it. */
		tm_media(&m, UDSP_SEL_G729, 18, 20);
		m.dtx = true;
		tm_stream_config(&t, &w, &m);
		CHECK(st->media.dtx);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
				       UDSP_SEL_PCMU, 0) == UDSP_OK);
		CHECK(!st->media.dtx);
		CHECK(!st->media.vad);
		CHECK(!st->media.cng);
		CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_OFF, 0, 0) ==
		      UDSP_OK);
		CHECK(st->media.dtx);
		CHECK_UINT(st->media.codec, UDSP_SEL_G729);
		tm_call_down(&t, &w);
	}

	CASE("a dtx flip recreates the codec pair");
	{
		uintptr_t enc;
		uintptr_t dec;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_media(&m, UDSP_SEL_G729, 18, 20);
		tm_stream_config(&t, &w, &m);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			tm_call_close(&t, &w);
			return;
		}
		enc = (uintptr_t)st->enc;
		dec = (uintptr_t)st->dec;
		CHECK(enc && dec);

		/* The flip reaches the instances only through a new pair: the
		 * encoder consumed the flag at create. */
		m.dtx = true;
		tm_stream_config(&t, &w, &m);
		CHECK((uintptr_t)st->enc != enc);
		CHECK((uintptr_t)st->dec != dec);
		CHECK(st->params.dtx);
		enc = (uintptr_t)st->enc;
		dec = (uintptr_t)st->dec;

		/* The same description keeps the pair it runs. */
		tm_stream_config(&t, &w, &m);
		CHECK((uintptr_t)st->enc == enc);
		CHECK((uintptr_t)st->dec == dec);

		tm_call_down(&t, &w);
	}
}

/*
 * The Annex B far end. A g729 DTX encoder drives the wire by the same rules
 * the transmitter follows: speech frames aggregate to the ptime, a SID ends
 * the packet (below the ptime included), and an untransmitted frame flushes
 * the staged speech and then sends nothing. The DUT receives it, so the jitter
 * buffer's SID slicing and DTX-gap comfort noise run over real codec frames.
 */
struct tm_dtx_tx {
	struct udsp_codec *enc;
	uint8_t agg[TM_DTX_PAY_MAX];
	unsigned agg_len;
	unsigned agg_frames;
	unsigned fpp;
	uint16_t seq;
	uint32_t ts;
	bool silent;
	bool marker;
};

static bool tm_dtx_tx_open(struct tm_dtx_tx *tx, unsigned fpp)
{
	struct udsp_codec_params cp;

	memset(tx, 0, sizeof(*tx));
	memset(&cp, 0, sizeof(cp));
	cp.dtx = true;
	tx->enc = udsp_codec_g729_ops.create(&cp);
	tx->fpp = fpp;
	tx->seq = 2000;
	tx->ts = 320000;
	tx->marker = true;
	return tx->enc != NULL;
}

static void tm_dtx_tx_close(struct tm_dtx_tx *tx)
{
	if (tx->enc)
		udsp_codec_g729_ops.destroy(tx->enc);
	tx->enc = NULL;
}

static void tm_dtx_tx_send(struct tm_dtx_tx *tx, int fd, uint32_t frame)
{
	uint8_t pkt[TM_PKT_MAX];
	size_t len;

	if (!tx->agg_len)
		return;
	len = tm_rtp_build(pkt, 18, tx->marker, tx->seq, tx->ts - frame * 80u,
			   0x33445566u, tx->agg, tx->agg_len);
	tx->marker = false;
	tx->seq++;
	send(fd, pkt, len, MSG_DONTWAIT | MSG_NOSIGNAL);
	tx->agg_len = 0;
	tx->agg_frames = 0;
}

/* One 10 ms frame of PCM through the DTX encoder, dispatched by the encode
 * return exactly as stream_tx_media does. Returns the wire byte count sent (0,
 * 2, 10 or an aggregate), for the caller's own accounting. */
static void tm_dtx_tx_frame(struct tm_dtx_tx *tx, int fd, const int16_t *pcm)
{
	uint8_t frame_buf[16];
	uint32_t staged = tx->agg_frames;
	int r = udsp_codec_g729_ops.encode(tx->enc, pcm, 80, frame_buf,
					   sizeof(frame_buf));

	tx->ts += 80u;
	if (r == 0) {
		/* Untransmitted: flush the staged speech, then nothing. */
		if (tx->agg_len)
			tm_dtx_tx_send(tx, fd, staged);
		tx->silent = true;
		return;
	}
	if (r == 2) {
		/* A SID ends the packet, staged speech in front of it. */
		if (!tx->agg_len)
			staged = 0;
		memcpy(tx->agg + tx->agg_len, frame_buf, 2);
		tx->agg_len += 2u;
		tx->agg_frames++;
		tm_dtx_tx_send(tx, fd, staged);
		tx->silent = true;
		return;
	}
	if (r != 10)
		return;
	if (tx->silent) {
		tx->silent = false;
		tx->marker = true;
	}
	if (!tx->agg_len)
		staged = 0;
	memcpy(tx->agg + tx->agg_len, frame_buf, 10);
	tx->agg_len += 10u;
	tx->agg_frames++;
	if (tx->agg_frames >= tx->fpp)
		tm_dtx_tx_send(tx, fd, staged);
}

static void tm_dtx_tone_frame(int16_t *pcm, double *phase)
{
	for (unsigned i = 0; i < 80u; i++) {
		pcm[i] = (int16_t)(8000.0 * sin(*phase));
		*phase += 2.0 * M_PI * 350.0 / 8000.0;
	}
}

/*
 * The integration of the receive path. An Annex B far end sends SID-tailed
 * packets and DTX gaps at the DUT. The codec-native DTX of the DUT arms the
 * PT 13 gate of the receiver. The jitter buffer plays comfort noise across the
 * gap, not loss concealment, and the talkspurt resumes cleanly. A PT 13 packet
 * in mid-call still classifies while dtx is armed.
 */
static void case_dtx_rx(void)
{
	struct udsp_media m;
	struct tm_wire w;
	/* Zero so a short-circuited setup path still closes a NULL encoder. */
	struct tm_dtx_tx tx = { 0 };
	struct udsp_stream *st;
	struct eh t;
	int16_t pcm[80];
	double phase = 0.0;
	uint64_t cn_before = 0;
	uint64_t plc_before = 0;
	uint64_t cn_gap = 0;
	uint64_t frames_before = 0;
	uint8_t buf[TM_PKT_MAX];

	CASE("g729 dtx RX: SID gaps play comfort noise, PT 13 still classifies");
	if (!tm_call_up(&t, "USA", &w, 1 << 20))
		return;
	tm_media(&m, UDSP_SEL_G729, 18, 20);
	m.dtx = true;
	m.jb_mode = UDSP_JB_FIXED;
	tm_stream_config(&t, &w, &m);
	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL);
	if (!st || !tm_dtx_tx_open(&tx, 2)) {
		tm_dtx_tx_close(&tx);
		tm_call_close(&t, &w);
		return;
	}
	/* The receiver gate honors the armed dtx even with generic cng off. */
	CHECK(st->receiver.cng_enabled);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);

	/* A talkspurt: 60 frames of tone, past the VAD initialization. */
	for (unsigned f = 0; f < 60u; f++) {
		tm_dtx_tone_frame(pcm, &phase);
		tm_dtx_tx_frame(&tx, w.rtp_peer, pcm);
		if (f % 3u == 0u)
			eh_wait_ms(&t, 20);
	}
	eh_wait_ms(&t, 60);
	/* Mid-run samples come from the published snapshot, never from the
	 * live buffer the engine thread is writing. */
	{
		struct udsp_jb_stats js;

		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &js) == UDSP_OK);
		frames_before = js.frames_out;
		cn_before = js.cn;
		plc_before = js.plc;
	}

	/* Silence: the encoder closes with a SID and then goes dark. The jitter
	 * buffer must fill the gap with comfort noise, not loss concealment. */
	memset(pcm, 0, sizeof(pcm));
	for (unsigned f = 0; f < 120u; f++) {
		tm_dtx_tx_frame(&tx, w.rtp_peer, pcm);
		if (f % 3u == 0u)
			eh_wait_ms(&t, 20);
	}
	eh_wait_ms(&t, 100);

	/* The CN played so far is SID-sourced: the case's own PT 13 goes out
	 * only after this sample, so the gap assertion below cannot borrow
	 * RFC 3389 noise. */
	{
		struct udsp_jb_stats js;

		CHECK(udsp_stream_jb_stats(t.s, 0, 0, &js) == UDSP_OK);
		cn_gap = js.cn - cn_before;
	}

	/* A stray PT 13 comfort-noise packet mid-silence: the receiver gate is
	 * open under dtx, so it classifies instead of being discarded. */
	{
		struct udsp_media_stats before, after;
		uint8_t level = 40;
		size_t len;

		/* The media statistics publish every UDSP_STATS_PERIOD_MS.
		 * Each read follows a wait of one full period, so a publish
		 * carries every packet that arrived before the wait. */
		eh_wait_ms(&t, UDSP_STATS_PERIOD_MS);
		CHECK(udsp_stream_stats(t.s, 0, 0, &before) == UDSP_OK);
		len = tm_rtp_build(buf, 13, false, tx.seq++, tx.ts,
				   0x33445566u, &level, 1);
		tx.ts += 80u;
		send(w.rtp_peer, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
		eh_wait_ms(&t, 80);
		eh_wait_ms(&t, UDSP_STATS_PERIOD_MS);
		CHECK(udsp_stream_stats(t.s, 0, 0, &after) == UDSP_OK);
		/* Classified, not discarded: the discard count did not move for
		 * the PT 13 packet. */
		CHECK_UINT(after.packets_discarded, before.packets_discarded);
	}

	/* The talkspurt resumes; the buffer plays real audio again. */
	for (unsigned f = 0; f < 60u; f++) {
		tm_dtx_tone_frame(pcm, &phase);
		tm_dtx_tx_frame(&tx, w.rtp_peer, pcm);
		if (f % 3u == 0u)
			eh_wait_ms(&t, 20);
	}
	eh_wait_ms(&t, 80);

	CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
	/* The stop is acked by the tick, so the live buffer is final and
	 * safely readable here, the discipline every case follows. */
	if (st) {
		struct udsp_jb_stats js;

		udsp_jb_stats(&st->jb, &js);
		/* The DTX gap played SID-sourced comfort noise (cn_gap was
		 * sampled before the case's own PT 13 went out), and comfort
		 * noise dominates the loss ladder across the whole run. The
		 * residual concealment is the boundary the hand-paced far end
		 * leaves at each talkspurt edge; the margins are wide because
		 * the pacing runs on real engine time and can starve under
		 * concurrent load. */
		CHECK(cn_gap > 20u);
		CHECK((js.cn - cn_before) > (js.plc - plc_before));
		/* Both talkspurts played out. */
		CHECK(js.frames_out > frames_before + 30u);
	}
	tm_dtx_tx_close(&tx);
	tm_call_close(&t, &w);
}

static void case_gate(void)
{
	struct eh t;

	CASE("a paced backend ticks from engine start, not from the end of open");
	{
		unsigned ticked = 0;

		/*
		 * A 1 us pace makes read_period return at once, so any slice
		 * the tick thread gets while udsp_open is still probing shows
		 * up as a read. A held gate reads nothing before the open
		 * returns, every time and on any machine. A released one reads
		 * unless the tick thread is starved for the whole open, so the
		 * ticked count over five opens is what the case asserts.
		 *
		 * The case arms no xrun injection. An xrun_gap_us of 2000
		 * turns every tick-loop iteration over 2 ms of wall clock into
		 * -EPIPE, and a sanitized or preempted run exceeds that budget
		 * with the gate correctly released (ASan and TSan on a 32-core
		 * load). A wall budget therefore cannot judge the gate. The
		 * gap injection stays in the backend as an xrun-path tool; no
		 * case arms it, and it is not a gate oracle. With none armed
		 * this backend cannot fault, so the per-open zero-fault checks
		 * below are a cheap extra. The XRUN model itself is proved
		 * deterministically in test_engine.c (the read_fail_err -EPIPE
		 * script and the short-capture case pin xruns and last_errno
		 * EPIPE).
		 */
		for (unsigned i = 0; i < 5u; i++) {
			struct engine_stats st;
			struct engine *e;

			if (!tm_open_only(&t, "USA", 1u, 0u, 0)) {
				CHECK(false);
				return;
			}
			if (atomic_load(&eh_be.reads))
				ticked++;
			e = atomic_load(&t.s->engine_pub);
			CHECK(e != NULL);
			if (e) {
				engine_stats_get(e, &st);
				CHECK(st.paced);
				CHECK_UINT(st.xruns, 0);
				CHECK_UINT(st.recover_failures, 0);
				CHECK_UINT(st.read_errors, 0);
			}
			tm_down(&t);
		}
		CHECK(ticked > 0);
	}
}

/* --- the plaintext packet trace ------------------------------------------ */

#define TM_PKT_REC_MAX		64u
/* More than the 1023 records a ring holds, so the overrun case counts real
 * losses (core/pkt_trace.h). */
#define TM_PKT_OVERRUN		1056u
#define TM_PKT_BATCH		16u
/* Above UDSP_PKT_TRACE_MAX, so the cut has something to cut. */
#define TM_PKT_LONG		700u

static struct udsp_pkt_trace tm_pkt[TM_PKT_REC_MAX];

/*
 * One pass over both rings. It counts the records and the losses they carry per
 * direction, keeps the first record of one direction and kind, and counts the
 * records whose payload equals one datagram the far end saw.
 */
struct tm_pkt_scan {
	struct udsp_pkt_trace first;
	bool have;
	/* The first record of the other direction, so one pass can compare the
	 * two stamps of one exchange. */
	struct udsp_pkt_trace other;
	bool have_other;
	unsigned want_dir, want_kind;
	const uint8_t *want_data;
	size_t want_len;
	unsigned matched;
	unsigned count[2];		/* by enum udsp_pkt_dir */
	uint64_t lost[2];
	/* The generation range over every record of the pass. */
	uint32_t gen_min, gen_max;
};

static void tm_pkt_scan_init(struct tm_pkt_scan *sc, unsigned dir,
			     unsigned kind)
{
	memset(sc, 0, sizeof(*sc));
	sc->want_dir = dir;
	sc->want_kind = kind;
	sc->gen_min = UINT32_MAX;
}

static void tm_pkt_scan_want(struct tm_pkt_scan *sc, const void *data,
			     size_t len)
{
	sc->want_data = data;
	sc->want_len = len;
}

static void tm_pkt_scan_one(struct tm_pkt_scan *sc,
			    const struct udsp_pkt_trace *r)
{
	unsigned dir = r->dir == UDSP_PKT_RX ? UDSP_PKT_RX : UDSP_PKT_TX;

	sc->count[dir]++;
	sc->lost[dir] += r->lost_before;
	if (r->gen < sc->gen_min)
		sc->gen_min = r->gen;
	if (r->gen > sc->gen_max)
		sc->gen_max = r->gen;
	if (sc->want_data && r->len == sc->want_len &&
	    !memcmp(r->data, sc->want_data, r->len))
		sc->matched++;
	if (dir != sc->want_dir && !sc->have_other) {
		sc->other = *r;
		sc->have_other = true;
	}
	if (sc->have || r->dir != sc->want_dir || r->kind != sc->want_kind)
		return;
	sc->first = *r;
	sc->have = true;
}

static void tm_pkt_scan_run(struct eh *t, struct tm_pkt_scan *sc)
{
	unsigned n;

	while ((n = udsp_pkt_trace_read(t->s, tm_pkt, TM_PKT_REC_MAX)) > 0) {
		for (unsigned i = 0; i < n; i++)
			tm_pkt_scan_one(sc, &tm_pkt[i]);
	}
}

static unsigned tm_pkt_total(const struct tm_pkt_scan *sc)
{
	return sc->count[UDSP_PKT_TX] + sc->count[UDSP_PKT_RX];
}

/* Empties both rings, so a following pass reports only what came after it. */
static void tm_pkt_flush(struct eh *t)
{
	struct tm_pkt_scan sc;

	tm_pkt_scan_init(&sc, UDSP_PKT_TX, UDSP_PKT_RTP);
	tm_pkt_scan_run(t, &sc);
}

/* The media stream of these cases: one packet per tick, so a short wait leaves
 * records in both directions. */
static bool tm_pkt_stream_up(struct eh *t, struct tm_wire *w,
			     struct udsp_media *m)
{
	tm_media(m, UDSP_SEL_PCMU, 0, 10);
	if (udsp_stream_config(t->s, 0, 0, m, w->rtp_local, w->rtcp_local) !=
	    UDSP_OK)
		return false;

	return udsp_stream_start(t->s, 0, 0) == UDSP_OK;
}

static unsigned tm_pkt_ring_head(const struct udsp_stream *st)
{
	return atomic_load_explicit(&st->ring.head, memory_order_acquire);
}

/*
 * The receive record is taken before the head that publishes the slot
 * (core/net.c), so a head that moved is the proof the net thread ran. A wait on
 * engine time proves nothing: the two threads share no clock.
 */
static bool tm_pkt_ring_wait(const struct udsp_stream *st, unsigned want)
{
	uint64_t end = eh_mono_us() + EH_GATE_WAIT_US;

	while (tm_pkt_ring_head(st) != want && eh_mono_us() < end)
		sched_yield();
	CHECK_UINT(tm_pkt_ring_head(st), want);

	return tm_pkt_ring_head(st) == want;
}

/*
 * One batch of datagrams into the stream, waited out on the receive ring: the
 * head moved by the whole batch and nothing was refused, so each datagram of
 * the batch filled one slot and earned one record. The ring is empty first,
 * because its 32 slots cannot hold a second batch behind an engine that drains
 * 16 of them per tick.
 */
static bool tm_pkt_rx_batch(struct udsp_stream *st, struct tm_wire *w,
			    unsigned count, double *phase)
{
	uint64_t end = eh_mono_us() + EH_GATE_WAIT_US;
	unsigned head;

	while (tm_pkt_ring_head(st) !=
	       atomic_load_explicit(&st->ring.tail, memory_order_acquire) &&
	       eh_mono_us() < end)
		sched_yield();
	head = tm_pkt_ring_head(st);
	for (unsigned i = 0; i < count; i++)
		tm_tone_send(w, 0, 10, 1000.0, 8000.0, phase);
	if (!tm_pkt_ring_wait(st, head + count))
		return false;
	CHECK_UINT(atomic_load(&st->net_trunc), 0);
	CHECK_UINT(atomic_load(&st->net_drops), 0);

	return true;
}

/* which names the ring allocation of one arm that fails: 1 the first, 2 the
 * second, 0 disarms the hook. */
static void tm_pkt_alloc_fail_set(unsigned which)
{
	char v[2] = { (char)('0' + which), 0 };

	if (which)
		setenv("UDSP_TEST_PKT_TRACE_ALLOC_FAIL", v, 1);
	else
		unsetenv("UDSP_TEST_PKT_TRACE_ALLOC_FAIL");
}

static void case_pkt_trace(void)
{
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	double phase = 0.0;

	CASE("the packet trace records nothing while the debug bit is clear");
	{
		struct tm_pkt_scan sc;
		uint8_t buf[TM_PKT_MAX];

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), 0);
		/* udsp_debug_set writes the mask whole, so this lowers the bit
		 * the arm raised while the rings stand. */
		udsp_debug_set(t.s, 0);
		CHECK(tm_pkt_stream_up(&t, &w, &m));
		for (unsigned i = 0; i < 10u; i++)
			tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
		CHECK(tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300) > 0);
		eh_wait_ms(&t, 100);

		tm_pkt_scan_init(&sc, UDSP_PKT_TX, UDSP_PKT_RTP);
		tm_pkt_scan_run(&t, &sc);
		CHECK_UINT(tm_pkt_total(&sc), 0);
		tm_call_down(&t, &w);
	}

	CASE("a transmit record carries the RTP datagram the far end received");
	{
		struct tm_pkt_scan sc;
		uint8_t buf[TM_PKT_MAX];
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), 0);
		CHECK(tm_pkt_stream_up(&t, &w, &m));
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300);
		CHECK(n > 0);

		tm_pkt_scan_init(&sc, UDSP_PKT_TX, UDSP_PKT_RTP);
		if (n > 0)
			tm_pkt_scan_want(&sc, buf, (size_t)n);
		tm_pkt_scan_run(&t, &sc);
		CHECK(sc.matched > 0);
		CHECK(sc.have);
		if (sc.have) {
			CHECK_UINT(sc.first.line, 0);
			CHECK_UINT(sc.first.stream, 0);
			CHECK_UINT(sc.first.dir, UDSP_PKT_TX);
			CHECK_UINT(sc.first.kind, UDSP_PKT_RTP);
			CHECK_UINT(sc.first.gen, 1);
			CHECK(sc.first.stamp_ns > 0);
		}
		tm_call_down(&t, &w);
	}

	CASE("a receive record carries the RTP datagram the far end sent");
	{
		struct tm_pkt_scan sc;
		struct udsp_stream *st;
		uint8_t pkt[TM_PKT_MAX];
		uint8_t pay[80];
		int16_t pcm[80];
		unsigned head;
		size_t len;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), 0);
		CHECK(tm_pkt_stream_up(&t, &w, &m));
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			tm_call_close(&t, &w);
			return;
		}
		tm_tone_frame(pcm, 80, 1000.0, 8000.0, &phase);
		tm_ulaw_encode(pcm, 80, pay);
		len = tm_rtp_build(pkt, 0, false, w.seq, w.ts, w.ssrc, pay, 80);
		tm_pkt_flush(&t);
		head = tm_pkt_ring_head(st);
		CHECK(send(w.rtp_peer, pkt, len, MSG_DONTWAIT |
			   MSG_NOSIGNAL) == (ssize_t)len);
		CHECK(tm_pkt_ring_wait(st, head + 1u));
		/* A few ticks of the stream's own transmit side, so the stamp
		 * comparison below has a record of each direction. */
		eh_wait_ms(&t, 30);

		tm_pkt_scan_init(&sc, UDSP_PKT_RX, UDSP_PKT_RTP);
		tm_pkt_scan_want(&sc, pkt, len);
		tm_pkt_scan_run(&t, &sc);
		CHECK_UINT(sc.matched, 1);
		CHECK(sc.have);
		if (sc.have) {
			CHECK_UINT(sc.first.len, len);
			CHECK_UINT(sc.first.dir, UDSP_PKT_RX);
			CHECK_UINT(sc.first.gen, 1);
			CHECK(sc.first.stamp_ns > 0);
		}
		/*
		 * One clock for both directions: the transmit stamp is the
		 * CLOCK_MONOTONIC read of the tick and the receive stamp the
		 * one the net thread took, so two records of one exchange sit
		 * inside a second of each other. A transmit stamp counted from
		 * the engine's first tick would be a whole uptime away.
		 */
		CHECK(sc.have_other);
		if (sc.have && sc.have_other) {
			uint64_t rx = sc.first.stamp_ns;
			uint64_t tx = sc.other.stamp_ns;

			CHECK_UINT(sc.other.dir, UDSP_PKT_TX);
			CHECK((rx > tx ? rx - tx : tx - rx) < 1000000000ull);
		}
		tm_call_down(&t, &w);
	}

	/*
	 * The record of an SRTP stream holds what the far end decrypts, not
	 * what the wire carried: the tap sits ahead of the protect, which is
	 * the whole reason the trace exists.
	 */
	CASE("a transmit record of an SRTP stream carries the plaintext");
	{
		_Alignas(uint32_t) uint8_t buf[TM_PKT_MAX];
		struct tm_pkt_scan sc;
		struct udsp_srtp peer;
		size_t out = 0;
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), 0);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.srtp.suite = UDSP_SRTP_AES_CM_128_SHA1_80;
		tm_key_set(m.srtp.key_tx, 0x10);
		tm_key_set(m.srtp.key_rx, 0x40);
		CHECK(tm_peer_srtp(&peer, &m));
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300);
		CHECK(n > 0);
		/* The wire carries the 10 byte authentication tag; the record
		 * must not. */
		CHECK_UINT((size_t)n, 12u + 80u + 10u);
		if (n > 0)
			CHECK(udsp_srtp_unprotect(&peer, buf, (size_t)n,
						  sizeof(buf), &out) == 0);
		CHECK_UINT(out, 92);

		tm_pkt_scan_init(&sc, UDSP_PKT_TX, UDSP_PKT_RTP);
		tm_pkt_scan_want(&sc, buf, out);
		tm_pkt_scan_run(&t, &sc);
		CHECK(sc.matched > 0);
		CHECK(sc.have);
		if (sc.have)
			CHECK_UINT(sc.first.len, 92);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		udsp_srtp_destroy(&peer);
		tm_call_close(&t, &w);
	}

	CASE("the report socket is taped as RTCP in both directions");
	{
		const uint8_t rr[8] = { 0x80u, 0xc9u, 0x00u, 0x01u,
					0x51u, 0x50u, 0x12u, 0x34u };
		struct udsp_stream *st;
		struct tm_pkt_scan tx;
		struct tm_pkt_scan rx;
		uint8_t buf[TM_PKT_MAX];
		unsigned head;
		ssize_t n;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), 0);
		CHECK(tm_pkt_stream_up(&t, &w, &m));
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			tm_call_close(&t, &w);
			return;
		}
		head = tm_pkt_ring_head(st);
		CHECK(send(w.rtcp_peer, rr, sizeof(rr),
			   MSG_DONTWAIT | MSG_NOSIGNAL) == (ssize_t)sizeof(rr));
		CHECK(tm_pkt_ring_wait(st, head + 1u));
		tm_pkt_scan_init(&rx, UDSP_PKT_RX, UDSP_PKT_RTCP);
		tm_pkt_scan_want(&rx, rr, sizeof(rr));
		tm_pkt_scan_run(&t, &rx);
		CHECK_UINT(rx.matched, 1);
		CHECK(rx.have);
		if (rx.have)
			CHECK_UINT(rx.first.kind, UDSP_PKT_RTCP);

		/* The goodbye of the stop is the report this case reads: the
		 * interval floor is five seconds (RFC 3550 6.2). */
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		n = tm_recv_wait(&t, w.rtcp_peer, buf, sizeof(buf), 300);
		CHECK(n > 0);
		tm_pkt_scan_init(&tx, UDSP_PKT_TX, UDSP_PKT_RTCP);
		if (n > 0)
			tm_pkt_scan_want(&tx, buf, (size_t)n);
		tm_pkt_scan_run(&t, &tx);
		CHECK(tx.matched > 0);
		CHECK(tx.have);
		if (tx.have)
			CHECK_UINT(tx.first.kind, UDSP_PKT_RTCP);
		tm_call_close(&t, &w);
	}

	CASE("a T.38 session is taped as UDPTL in both directions");
	{
		const uint8_t in[FAX_FAKE_DGRAM] = { 0xa1u, 0xb2u, 0xc3u,
						     0xd4u };
		struct udsp_stream *st;
		struct tm_pkt_scan tx;
		struct tm_pkt_scan rx;
		uint8_t buf[TM_PKT_MAX];
		unsigned head;
		ssize_t n;

		if (!tm_call_up_fax(&t, "USA", 0, &w))
			return;
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), 0);
		tm_t38_media(&m);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		/* The gateway hands out one datagram per frame it took, so the
		 * transmit side runs on the tick alone. */
		n = tm_recv_wait(&t, w.rtp_peer, buf, sizeof(buf), 300);
		CHECK(n > 0);

		tm_pkt_scan_init(&tx, UDSP_PKT_TX, UDSP_PKT_UDPTL);
		if (n > 0)
			tm_pkt_scan_want(&tx, buf, (size_t)n);
		tm_pkt_scan_run(&t, &tx);
		CHECK(tx.matched > 0);
		CHECK(tx.have);
		if (tx.have) {
			CHECK_UINT(tx.first.kind, UDSP_PKT_UDPTL);
			CHECK_UINT(tx.first.len, FAX_FAKE_DGRAM);
		}
		tm_call_down(&t, &w);

		/* The receive record of the same session, read from a second
		 * handle so the pass above cannot have drained it. */
		if (!tm_call_up_fax(&t, "USA", 0, &w))
			return;
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), 0);
		tm_t38_media(&m);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			tm_call_close(&t, &w);
			return;
		}
		head = tm_pkt_ring_head(st);
		CHECK(send(w.rtp_peer, in, sizeof(in),
			   MSG_DONTWAIT | MSG_NOSIGNAL) == (ssize_t)sizeof(in));
		CHECK(tm_pkt_ring_wait(st, head + 1u));
		tm_pkt_scan_init(&rx, UDSP_PKT_RX, UDSP_PKT_UDPTL);
		tm_pkt_scan_want(&rx, in, sizeof(in));
		tm_pkt_scan_run(&t, &rx);
		CHECK_UINT(rx.matched, 1);
		CHECK(rx.have);
		if (rx.have) {
			CHECK_UINT(rx.first.kind, UDSP_PKT_UDPTL);
			CHECK_UINT(rx.first.len, FAX_FAKE_DGRAM);
		}
		tm_call_down(&t, &w);
	}

	/* The empty datagram that holds an idle tuple open is not media, so it
	 * earns no record (core/stream.c stream_punch). */
	CASE("the keepalive punch is not taped");
	{
		struct tm_pkt_scan sc;
		uint8_t buf[TM_PKT_MAX];

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), 0);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.direction = UDSP_MEDIA_RECVONLY;
		m.rtcp_enable = false;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		eh_wait_ms(&t, 100);
		/* The punch and nothing else: an empty datagram on each
		 * descriptor. */
		CHECK_UINT(recv(w.rtp_peer, buf, sizeof(buf), MSG_DONTWAIT), 0);
		CHECK_UINT(recv(w.rtcp_peer, buf, sizeof(buf), MSG_DONTWAIT),
			   0);

		tm_pkt_scan_init(&sc, UDSP_PKT_TX, UDSP_PKT_RTP);
		tm_pkt_scan_run(&t, &sc);
		CHECK_UINT(sc.count[UDSP_PKT_TX], 0);
		tm_call_down(&t, &w);
	}

	CASE("a second arm allocates nothing and answers 0");
	{
		struct tm_pkt_scan sc;
		struct pkt_trace_ring *first;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), 0);
		first = atomic_load(&t.s->pkt_tx);
		CHECK(first != NULL);
		/* The hook fails every allocation, so a second arm that still
		 * answers 0 made none. */
		tm_pkt_alloc_fail_set(1u);
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), 0);
		tm_pkt_alloc_fail_set(0u);
		CHECK(atomic_load(&t.s->pkt_tx) == first);

		CHECK(tm_pkt_stream_up(&t, &w, &m));
		eh_wait_ms(&t, 100);
		tm_pkt_scan_init(&sc, UDSP_PKT_TX, UDSP_PKT_RTP);
		tm_pkt_scan_run(&t, &sc);
		CHECK(sc.count[UDSP_PKT_TX] > 0);
		tm_call_down(&t, &w);
	}

	CASE("an arm that cannot allocate answers ENOMEM and stays off");
	{
		struct tm_pkt_scan sc;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		/* The bit raised by hand first: a failed arm takes it down
		 * again, because no ring stands behind it, and leaves every
		 * other flag of the mask alone. */
		udsp_debug_set(t.s, UDSP_DEBUG_JB_TRACE | UDSP_DEBUG_PKT_TRACE);
		tm_pkt_alloc_fail_set(1u);
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), -ENOMEM);
		tm_pkt_alloc_fail_set(0u);
		CHECK(atomic_load(&t.s->pkt_tx) == NULL);
		CHECK(atomic_load(&t.s->pkt_rx) == NULL);
		CHECK_UINT(atomic_load(&t.s->debug) & UDSP_DEBUG_PKT_TRACE, 0);
		CHECK(atomic_load(&t.s->debug) & UDSP_DEBUG_JB_TRACE);
		udsp_debug_set(t.s, 0);

		CHECK(tm_pkt_stream_up(&t, &w, &m));
		for (unsigned i = 0; i < 10u; i++)
			tm_tone_send(&w, 0, 10, 1000.0, 8000.0, &phase);
		eh_wait_ms(&t, 100);
		tm_pkt_scan_init(&sc, UDSP_PKT_TX, UDSP_PKT_RTP);
		tm_pkt_scan_run(&t, &sc);
		CHECK_UINT(tm_pkt_total(&sc), 0);
		tm_call_down(&t, &w);
	}

	CASE("a disarm keeps the rings and a second arm produces again");
	{
		struct pkt_trace_ring *first;
		struct tm_pkt_scan sc;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), 0);
		first = atomic_load(&t.s->pkt_tx);
		CHECK(tm_pkt_stream_up(&t, &w, &m));
		eh_wait_ms(&t, 100);
		tm_pkt_scan_init(&sc, UDSP_PKT_TX, UDSP_PKT_RTP);
		tm_pkt_scan_run(&t, &sc);
		CHECK(sc.count[UDSP_PKT_TX] > 0);

		CHECK_UINT(udsp_pkt_trace_enable(t.s, false), 0);
		CHECK(atomic_load(&t.s->pkt_tx) == first);
		eh_wait_ms(&t, 100);
		tm_pkt_flush(&t);
		eh_wait_ms(&t, 100);
		tm_pkt_scan_init(&sc, UDSP_PKT_TX, UDSP_PKT_RTP);
		tm_pkt_scan_run(&t, &sc);
		CHECK_UINT(tm_pkt_total(&sc), 0);

		/* The hook proves the second arm allocated nothing. */
		tm_pkt_alloc_fail_set(1u);
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), 0);
		tm_pkt_alloc_fail_set(0u);
		CHECK(atomic_load(&t.s->pkt_tx) == first);
		eh_wait_ms(&t, 100);
		tm_pkt_scan_init(&sc, UDSP_PKT_TX, UDSP_PKT_RTP);
		tm_pkt_scan_run(&t, &sc);
		CHECK(sc.count[UDSP_PKT_TX] > 0);
		tm_call_down(&t, &w);
	}

	/*
	 * The overrun rides the receive direction, where one datagram is one
	 * record and the batch wait proves every datagram reached a slot. The
	 * stream receives only, so the transmit ring stays out of the count.
	 */
	CASE("the ring counts the records it could not hold");
	{
		struct udsp_stream *st;
		struct tm_pkt_scan sc;
		unsigned sent = 0;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), 0);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.direction = UDSP_MEDIA_RECVONLY;
		m.rtcp_enable = false;
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		while (st && sent < TM_PKT_OVERRUN) {
			if (!tm_pkt_rx_batch(st, &w, TM_PKT_BATCH, &phase))
				break;
			sent += TM_PKT_BATCH;
		}
		CHECK_UINT(sent, TM_PKT_OVERRUN);

		/* The ring holds 1023 records and refuses the rest; the losses
		 * stay pending, because the count rides the next record that
		 * fits. */
		tm_pkt_scan_init(&sc, UDSP_PKT_RX, UDSP_PKT_RTP);
		tm_pkt_scan_run(&t, &sc);
		CHECK_UINT(sc.count[UDSP_PKT_RX], UDSP_PKT_TRACE_SLOTS - 1u);
		CHECK_UINT(sc.lost[UDSP_PKT_RX], 0);

		if (st)
			CHECK(tm_pkt_rx_batch(st, &w, 1u, &phase));
		sent += 1u;
		tm_pkt_scan_init(&sc, UDSP_PKT_RX, UDSP_PKT_RTP);
		tm_pkt_scan_run(&t, &sc);
		CHECK_UINT(sc.count[UDSP_PKT_RX], 1);
		CHECK_UINT(sc.lost[UDSP_PKT_RX],
			   sent - UDSP_PKT_TRACE_SLOTS);
		/* Every datagram is accounted for: the records read plus the
		 * losses they carry. */
		CHECK_UINT(UDSP_PKT_TRACE_SLOTS - 1u + 1u +
			   sc.lost[UDSP_PKT_RX], sent);
		tm_call_down(&t, &w);
	}

	/*
	 * No producer in this library can build a datagram above the bound:
	 * a codec payload stops at UDSP_CODEC_PAYLOAD_MAX, the gateway
	 * fills through a cap of UDSP_NET_DGRAM_MAX and the receive path
	 * refuses a longer datagram outright. The cut is therefore driven
	 * through the push every tap site calls, on a handle with no stream, so
	 * this thread is the ring's only producer.
	 */
	CASE("a record longer than the bound is cut and keeps the cut length");
	{
		struct pkt_trace_meta pm;
		struct pkt_trace_ring *r;
		uint8_t big[TM_PKT_LONG];
		unsigned n;

		if (!tm_up(&t, "USA")) {
			CHECK(false);
			return;
		}
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), 0);
		for (unsigned i = 0; i < sizeof(big); i++)
			big[i] = (uint8_t)(i * 7u + 1u);
		memset(&pm, 0, sizeof(pm));
		pm.stamp_ns = 12345u;
		pm.gen = 7u;
		pm.line = 1u;
		pm.stream = 2u;
		pm.dir = UDSP_PKT_TX;
		pm.kind = UDSP_PKT_UDPTL;
		r = atomic_load(&t.s->pkt_tx);
		CHECK(r != NULL);
		if (r)
			CHECK(pkt_trace_push(r, &pm, big, sizeof(big)));

		n = udsp_pkt_trace_read(t.s, tm_pkt, TM_PKT_REC_MAX);
		CHECK_UINT(n, 1);
		if (n == 1) {
			CHECK_UINT(tm_pkt[0].len, UDSP_PKT_TRACE_MAX);
			CHECK(!memcmp(tm_pkt[0].data, big,
				      UDSP_PKT_TRACE_MAX));
			CHECK_UINT(tm_pkt[0].gen, 7);
			CHECK_UINT(tm_pkt[0].line, 1);
			CHECK_UINT(tm_pkt[0].stream, 2);
			CHECK_UINT(tm_pkt[0].kind, UDSP_PKT_UDPTL);
			CHECK_UINT(tm_pkt[0].stamp_ns, 12345u);
		}
		tm_down(&t);
	}

	CASE("the records carry the generation of their own description");
	{
		struct udsp_stream *st;
		struct tm_pkt_scan sc;
		uint32_t gen;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), 0);
		CHECK(tm_pkt_stream_up(&t, &w, &m));
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			tm_call_close(&t, &w);
			return;
		}
		gen = udsp_stream_gen(t.s, 0, 0);
		CHECK_UINT(gen, 1);
		tm_pkt_flush(&t);
		CHECK(tm_pkt_rx_batch(st, &w, 1u, &phase));
		/* A few ticks, so the transmit side of this description has
		 * records of its own in the ring. */
		eh_wait_ms(&t, 30);

		/*
		 * No drain here. The records of this description stay queued
		 * across the reconfiguration below, which is the case a reader
		 * that drains once a second meets on every re-INVITE.
		 */
		tm_stream_config(&t, &w, &m);
		CHECK_UINT(udsp_stream_gen(t.s, 0, 0), gen + 1u);

		/* The queued records still carry the generation they left on,
		 * in both directions: the reader pops the transmit ring first,
		 * so the first record of each direction is the oldest one. */
		tm_pkt_scan_init(&sc, UDSP_PKT_RX, UDSP_PKT_RTP);
		tm_pkt_scan_run(&t, &sc);
		CHECK(sc.have);
		CHECK(sc.have_other);
		if (sc.have)
			CHECK_UINT(sc.first.gen, gen);
		if (sc.have_other) {
			CHECK_UINT(sc.other.dir, UDSP_PKT_TX);
			CHECK_UINT(sc.other.gen, gen);
		}

		/* Every record of the description that follows carries the new
		 * value, whichever way it traveled. */
		tm_pkt_flush(&t);
		CHECK(tm_pkt_rx_batch(st, &w, 1u, &phase));
		eh_wait_ms(&t, 30);
		tm_pkt_scan_init(&sc, UDSP_PKT_RX, UDSP_PKT_RTP);
		tm_pkt_scan_run(&t, &sc);
		CHECK(sc.have);
		CHECK(sc.have_other);
		CHECK(sc.count[UDSP_PKT_TX] > 0);
		CHECK_UINT(sc.gen_min, gen + 1u);
		CHECK_UINT(sc.gen_max, gen + 1u);
		tm_call_down(&t, &w);
	}

	/*
	 * A configuration that fails after it applied the description consumes
	 * a generation: the engine's acknowledgment of the adoption goes
	 * missing, the stream is undone, and the counter does not go back. The
	 * getter is what keeps a reader's table right across that, because it
	 * answers the value the live description carries and not a call count.
	 */
	CASE("a failed configuration consumes a generation the getter skips");
	{
		struct udsp_stream *st;
		struct tm_pkt_scan sc;
		uint32_t gen;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), 0);
		/* Before any description, and outside the pool. */
		CHECK_UINT(udsp_stream_gen(t.s, 0, 0), 0);
		CHECK_UINT(udsp_stream_gen(t.s, UDSP_LINE_MAX, 0), 0);
		CHECK_UINT(udsp_stream_gen(t.s, 0, UDSP_STREAM_MAX), 0);

		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK_UINT(udsp_stream_gen(t.s, 0, 0), 1);

		tm_arm_engine_drop(&t, STREAM_OP_ADOPT);
		CHECK(udsp_stream_config(t.s, 0, 0, &m, w.rtp_local,
					 w.rtcp_local) == UDSP_TRANSIENT);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (st) {
			CHECK(!atomic_load(&st->dead));
			CHECK_UINT(atomic_load(&st->state), STREAM_IDLE);
		}
		/* The failed attempt consumed one, and the getter says so. */
		CHECK_UINT(udsp_stream_gen(t.s, 0, 0), 2);

		tm_stream_config(&t, &w, &m);
		gen = udsp_stream_gen(t.s, 0, 0);
		CHECK_UINT(gen, 3);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		tm_pkt_flush(&t);
		if (st)
			CHECK(tm_pkt_rx_batch(st, &w, 1u, &phase));
		eh_wait_ms(&t, 30);

		/* The records of the retry carry what the getter answers, so a
		 * table keyed on it holds the right endpoints. */
		tm_pkt_scan_init(&sc, UDSP_PKT_RX, UDSP_PKT_RTP);
		tm_pkt_scan_run(&t, &sc);
		CHECK(sc.have);
		CHECK(sc.count[UDSP_PKT_TX] > 0);
		CHECK_UINT(sc.gen_min, gen);
		CHECK_UINT(sc.gen_max, gen);
		tm_call_down(&t, &w);
	}

	/*
	 * The kind of a received datagram follows the description the
	 * descriptors are registered for. A stop tells the engine the session
	 * ended while those descriptors are still in the epoll set, so a
	 * datagram that arrives in that window is still UDPTL. The window is a
	 * few microseconds of the control thread and no test hook opens it, so
	 * the ordering is pinned here instead: the value the net thread reads
	 * does not follow the engine's own mode.
	 */
	CASE("the receive kind follows the registration, not the engine stop");
	{
		struct tm_pkt_scan sc;
		struct udsp_stream *st;
		unsigned head;

		if (!tm_call_up_fax(&t, "USA", 0, &w))
			return;
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), 0);
		tm_t38_media(&m);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			tm_call_close(&t, &w);
			return;
		}
		CHECK_UINT(st->mode, STREAM_MODE_T38);
		CHECK_UINT(atomic_load(&st->net_mode), STREAM_MODE_T38);

		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		/* The engine ended the session; the registration's mode did
		 * not move with it. */
		CHECK_UINT(st->mode, STREAM_MODE_AUDIO);
		CHECK_UINT(atomic_load(&st->net_mode), STREAM_MODE_T38);

		/* The next description publishes before its own attach, so the
		 * first datagram of the audio stream is taped as RTP. */
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		CHECK_UINT(atomic_load(&st->net_mode), STREAM_MODE_AUDIO);
		tm_pkt_flush(&t);
		head = tm_pkt_ring_head(st);
		CHECK(tm_pkt_rx_batch(st, &w, 1u, &phase));
		CHECK(tm_pkt_ring_head(st) == head + 1u);
		tm_pkt_scan_init(&sc, UDSP_PKT_RX, UDSP_PKT_RTP);
		tm_pkt_scan_run(&t, &sc);
		CHECK(sc.have);
		if (sc.have)
			CHECK_UINT(sc.first.kind, UDSP_PKT_RTP);
		tm_call_down(&t, &w);
	}

	CASE("a receive record of an SRTP stream carries the plaintext");
	{
		_Alignas(uint32_t) uint8_t buf[TM_PKT_MAX];
		uint8_t plain[TM_PKT_MAX];
		uint8_t pay[80];
		int16_t pcm[80];
		struct tm_pkt_scan sc;
		struct udsp_stream *st;
		struct udsp_srtp peer;
		unsigned head;
		size_t out = 0;
		size_t len;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), 0);
		tm_media(&m, UDSP_SEL_PCMU, 0, 10);
		m.srtp.suite = UDSP_SRTP_AES_CM_128_SHA1_80;
		tm_key_set(m.srtp.key_tx, 0x10);
		tm_key_set(m.srtp.key_rx, 0x40);
		CHECK(tm_peer_srtp(&peer, &m));
		tm_stream_config(&t, &w, &m);
		CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			udsp_srtp_destroy(&peer);
			tm_call_close(&t, &w);
			return;
		}
		tm_tone_frame(pcm, 80, 1000.0, 8000.0, &phase);
		tm_ulaw_encode(pcm, 80, pay);
		len = tm_rtp_build(plain, 0, false, w.seq, w.ts, w.ssrc, pay,
				   80);
		memcpy(buf, plain, len);
		CHECK(udsp_srtp_protect(&peer, buf, len, sizeof(buf),
					&out) == 0);
		/* The wire carries the authentication tag, the record must
		 * not: the net thread unprotects in place before the tap. */
		CHECK_UINT(out, len + 10u);
		tm_pkt_flush(&t);
		head = tm_pkt_ring_head(st);
		CHECK(send(w.rtp_peer, buf, out, MSG_DONTWAIT |
			   MSG_NOSIGNAL) == (ssize_t)out);
		CHECK(tm_pkt_ring_wait(st, head + 1u));

		tm_pkt_scan_init(&sc, UDSP_PKT_RX, UDSP_PKT_RTP);
		tm_pkt_scan_want(&sc, plain, len);
		tm_pkt_scan_run(&t, &sc);
		CHECK_UINT(sc.matched, 1);
		CHECK(sc.have);
		if (sc.have)
			CHECK_UINT(sc.first.len, len);
		CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
		udsp_srtp_destroy(&peer);
		tm_call_close(&t, &w);
	}

	/* A datagram above the slot is counted and dropped with an empty slot,
	 * so the record says a datagram arrived and carried nothing this
	 * library could keep (core/net.c net_slot_fill). */
	CASE("a datagram the receive path refused is taped with no payload");
	{
		uint8_t big[TM_PKT_LONG];
		struct tm_pkt_scan sc;
		struct udsp_stream *st;
		unsigned head;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), 0);
		CHECK(tm_pkt_stream_up(&t, &w, &m));
		st = stream_at(t.s, 0, 0);
		CHECK(st != NULL);
		if (!st) {
			tm_call_close(&t, &w);
			return;
		}
		memset(big, 0x5a, sizeof(big));
		tm_pkt_flush(&t);
		head = tm_pkt_ring_head(st);
		CHECK(send(w.rtp_peer, big, sizeof(big), MSG_DONTWAIT |
			   MSG_NOSIGNAL) == (ssize_t)sizeof(big));
		CHECK(tm_pkt_ring_wait(st, head + 1u));

		tm_pkt_scan_init(&sc, UDSP_PKT_RX, UDSP_PKT_RTP);
		tm_pkt_scan_run(&t, &sc);
		CHECK(sc.have);
		if (sc.have) {
			CHECK_UINT(sc.first.len, 0);
			CHECK(sc.first.stamp_ns > 0);
		}
		CHECK_UINT(atomic_load(&st->net_trunc), 1);
		tm_call_down(&t, &w);
	}

	/* The second ring is where the partial path lives: the first one was
	 * allocated and has to go back. */
	CASE("an arm whose second ring fails frees the first and stays off");
	{
		struct tm_pkt_scan sc;

		if (!tm_call_up(&t, "USA", &w, 0))
			return;
		tm_pkt_alloc_fail_set(2u);
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), -ENOMEM);
		tm_pkt_alloc_fail_set(0u);
		CHECK(atomic_load(&t.s->pkt_tx) == NULL);
		CHECK(atomic_load(&t.s->pkt_rx) == NULL);
		CHECK_UINT(atomic_load(&t.s->debug) & UDSP_DEBUG_PKT_TRACE, 0);

		/* The arm after it works, so the failure left no half state. */
		CHECK_UINT(udsp_pkt_trace_enable(t.s, true), 0);
		CHECK(atomic_load(&t.s->pkt_tx) != NULL);
		CHECK(atomic_load(&t.s->pkt_rx) != NULL);
		CHECK(tm_pkt_stream_up(&t, &w, &m));
		eh_wait_ms(&t, 100);
		tm_pkt_scan_init(&sc, UDSP_PKT_TX, UDSP_PKT_RTP);
		tm_pkt_scan_run(&t, &sc);
		CHECK(sc.count[UDSP_PKT_TX] > 0);
		tm_call_down(&t, &w);
	}
}

static bool tm_diag_count_wait(const _Atomic uint_least64_t *counter,
			       uint64_t want)
{
	uint64_t end = eh_mono_us() + EH_GATE_WAIT_US;

	while (atomic_load_explicit(counter, memory_order_relaxed) < want &&
	       eh_mono_us() < end)
		sched_yield();

	return atomic_load_explicit(counter, memory_order_relaxed) == want;
}

static void case_stream_diag(void)
{
	struct udsp_stream_diag diag, zero = { 0 };
	struct eh_gate gate = { .fd = { -1, -1 } };
	uint8_t packet[UDSP_NET_DGRAM_MAX + 1u] = { 0 };
	struct udsp_stream *st;
	struct udsp_media media;
	struct tm_wire wire;
	struct eh t;
	unsigned head;

	CASE("stream diagnostics refuse unavailable streams");
	if (!tm_call_up(&t, "USA", &wire, 1 << 20))
		return;
	memset(&diag, 0xa5, sizeof(diag));
	CHECK(udsp_stream_diag(t.s, 0, 0, &diag) == UDSP_UNSUPPORTED);
	CHECK(!memcmp(&diag, &zero, sizeof(diag)));
	CHECK(udsp_stream_diag(t.s, t.s->cap.lines, 0, &diag) ==
	      UDSP_UNSUPPORTED);
	CHECK(udsp_stream_diag(t.s, 0, UDSP_STREAM_MAX, &diag) ==
	      UDSP_UNSUPPORTED);
	CHECK(udsp_stream_diag(NULL, 0, 0, &diag) == UDSP_UNSUPPORTED);
	CHECK(udsp_stream_diag(t.s, 0, 0, NULL) == UDSP_UNSUPPORTED);

	CASE("stream diagnostics read a configured stream before its start");
	tm_media(&media, UDSP_SEL_PCMU, 0, 10);
	media.direction = UDSP_MEDIA_RECVONLY;
	media.rtcp_enable = false;
	tm_stream_config(&t, &wire, &media);
	CHECK(udsp_stream_diag(t.s, 0, 0, &diag) == UDSP_OK);
	CHECK(!memcmp(&diag, &zero, sizeof(diag)));
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL);
	if (!st || !eh_gate_park(&gate))
		goto out;

	CASE("stream diagnostics count successful punches");
	CHECK(udsp_stream_diag(t.s, 0, 0, &diag) == UDSP_OK);
	CHECK_UINT(diag.punch_sent, 2);
	CHECK_UINT(diag.tx_fail, 0);
	CHECK_UINT(recv(wire.rtp_peer, packet, sizeof(packet),
			MSG_DONTWAIT), 0);
	CHECK_UINT(recv(wire.rtcp_peer, packet, sizeof(packet),
			MSG_DONTWAIT), 0);

	CASE("stream diagnostics count oversized received datagrams");
	head = tm_pkt_ring_head(st);
	CHECK(send(wire.rtp_peer, packet, sizeof(packet), MSG_DONTWAIT) ==
	      (ssize_t)sizeof(packet));
	CHECK(tm_pkt_ring_wait(st, head + 1u));
	CHECK(udsp_stream_diag(t.s, 0, 0, &diag) == UDSP_OK);
	CHECK_UINT(diag.net_trunc, 1);
	CHECK_UINT(diag.net_drops, 0);
	CHECK_UINT(diag.net_stale, 0);
	if (!eh_gate_step(&gate, 1))
		goto out;

	CASE("stream diagnostics count full ring drops and stale datagrams");
	head = tm_pkt_ring_head(st);
	for (unsigned i = 0; i < UDSP_NET_SLOTS; i++)
		CHECK_UINT(send(wire.rtp_peer, packet, 1, MSG_DONTWAIT), 1);
	CHECK(tm_pkt_ring_wait(st, head + UDSP_NET_SLOTS));
	for (unsigned i = 0; i < 3; i++)
		CHECK_UINT(send(wire.rtp_peer, packet, 1, MSG_DONTWAIT), 1);
	CHECK(tm_diag_count_wait(&st->net_drops, 3));
	CHECK(udsp_stream_diag(t.s, 0, 0, &diag) == UDSP_OK);
	CHECK_UINT(diag.net_drops, 3);
	CHECK_UINT(diag.net_stale, 0);
	if (!eh_gate_step(&gate, 1))
		goto out;
	CHECK(udsp_stream_diag(t.s, 0, 0, &diag) == UDSP_OK);
	CHECK_UINT(diag.net_drops, 3);
	CHECK_UINT(diag.net_stale, UDSP_NET_SLOTS - UDSP_NET_DRAIN_MAX);
	CHECK_UINT(diag.net_trunc, 1);

out:
	eh_gate_disarm(&gate);
	tm_call_down(&t, &wire);
	eh_gate_close(&gate);
}

static void tm_diag_send_case(bool media_send)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	struct udsp_stream_diag before, after;
	struct udsp_media media;
	struct tm_wire wire;
	struct eh t;

	CASE(media_send ? "stream diagnostics count failed media sends" :
	     "stream diagnostics count failed punch sends");
	if (!tm_call_up(&t, "USA", &wire, 0))
		return;
	tm_media(&media, UDSP_SEL_PCMU, 0, 10);
	media.rtcp_enable = false;
	if (!media_send) {
		media.direction = UDSP_MEDIA_RECVONLY;
		media.punch_interval_ms = 10;
	}
	tm_stream_config(&t, &wire, &media);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	if (!eh_gate_park(&gate))
		goto out;
	CHECK(udsp_stream_diag(t.s, 0, 0, &before) == UDSP_OK);
	CHECK_UINT(before.tx_fail, 0);
	CHECK(close(wire.rtp_peer) == 0);
	wire.rtp_peer = -1;
	if (!eh_gate_step(&gate, 3))
		goto out;
	CHECK(udsp_stream_diag(t.s, 0, 0, &after) == UDSP_OK);
	CHECK_UINT(after.tx_fail - before.tx_fail, 3);
	CHECK_UINT(after.punch_sent - before.punch_sent, media_send ? 0 : 3);

out:
	eh_gate_disarm(&gate);
	tm_call_down(&t, &wire);
	eh_gate_close(&gate);
}

static void case_stream_diag_srtp(void)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	struct udsp_stream_diag before, after;
	struct udsp_stream *st;
	struct udsp_srtp peer;
	struct udsp_media media;
	struct tm_wire wire;
	struct eh t;
	_Alignas(uint32_t) uint8_t packet[TM_PKT_MAX];
	uint8_t replay[TM_PKT_MAX], payload[80] = { 0 };
	size_t len, encrypted = 0, decoded = 0;
	unsigned head;
	ssize_t received;

	CASE("stream diagnostics count SRTP protect and unprotect success");
	if (!tm_call_up(&t, "USA", &wire, 0))
		return;
	tm_media(&media, UDSP_SEL_PCMU, 0, 10);
	media.srtp.suite = UDSP_SRTP_AES_CM_128_SHA1_80;
	tm_key_set(media.srtp.key_tx, 0x31);
	tm_key_set(media.srtp.key_rx, 0x62);
	CHECK(tm_peer_srtp(&peer, &media));
	tm_stream_config(&t, &wire, &media);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL);
	if (!st || !eh_gate_park(&gate))
		goto out;
	CHECK(udsp_stream_diag(t.s, 0, 0, &before) == UDSP_OK);
	if (!eh_gate_step(&gate, 3))
		goto out;
	CHECK(udsp_stream_diag(t.s, 0, 0, &after) == UDSP_OK);
	CHECK_UINT(after.srtp_protect_ok - before.srtp_protect_ok, 3);
	received = tm_recv(wire.rtp_peer, packet, sizeof(packet));
	CHECK(received > 0);
	if (received > 0)
		CHECK(udsp_srtp_unprotect(&peer, packet, (size_t)received,
					  sizeof(packet), &decoded) == 0);
	len = tm_rtp_build(packet, 0, true, wire.seq++, wire.ts, wire.ssrc,
			    payload, sizeof(payload));
	CHECK(udsp_srtp_protect(&peer, packet, len, sizeof(packet),
				&encrypted) == 0);
	memcpy(replay, packet, encrypted);
	head = tm_pkt_ring_head(st);
	CHECK(send(wire.rtp_peer, packet, encrypted, MSG_DONTWAIT) ==
	      (ssize_t)encrypted);
	CHECK(tm_pkt_ring_wait(st, head + 1u));
	CHECK(udsp_stream_diag(t.s, 0, 0, &after) == UDSP_OK);
	CHECK_UINT(after.srtp_unprotect_ok, 1);

	CASE("stream diagnostics count SRTP authentication failures");
	len = tm_rtp_build(packet, 0, false, wire.seq++, wire.ts + 80,
			    wire.ssrc, payload, sizeof(payload));
	CHECK(udsp_srtp_protect(&peer, packet, len, sizeof(packet),
				&encrypted) == 0);
	packet[encrypted - 1] ^= 1;
	head = tm_pkt_ring_head(st);
	CHECK(send(wire.rtp_peer, packet, encrypted, MSG_DONTWAIT) ==
	      (ssize_t)encrypted);
	CHECK(tm_pkt_ring_wait(st, head + 1u));
	CHECK(udsp_stream_diag(t.s, 0, 0, &after) == UDSP_OK);
	CHECK_UINT(after.srtp_unprotect_auth_fail, 1);
	CHECK_UINT(after.srtp_unprotect_ok, 1);
	CHECK_UINT(after.net_trunc, 1);

	CASE("stream diagnostics count SRTP replays separately");
	head = tm_pkt_ring_head(st);
	CHECK(send(wire.rtp_peer, replay, encrypted, MSG_DONTWAIT) ==
	      (ssize_t)encrypted);
	CHECK(tm_pkt_ring_wait(st, head + 1u));
	CHECK(udsp_stream_diag(t.s, 0, 0, &after) == UDSP_OK);
	CHECK_UINT(after.srtp_unprotect_replay, 1);
	CHECK_UINT(after.srtp_unprotect_auth_fail, 1);
	CHECK_UINT(after.srtp_unprotect_ok, 1);
	CHECK_UINT(after.net_trunc, 2);

	CASE("stream diagnostics map SRTP protect errors to protect failures");
	/* The tick is parked, so its transmit session has one caller here. */
	CHECK(udsp_stream_diag(t.s, 0, 0, &before) == UDSP_OK);
	len = tm_rtp_build(packet, 0, false, 1, 80, 0x17171717,
			    payload, sizeof(payload));
	for (unsigned i = 0; i < 2; i++)
		CHECK(udsp_srtp_protect(&st->srtp, packet, len, len,
					&encrypted) == -EMSGSIZE);
	CHECK(udsp_stream_diag(t.s, 0, 0, &after) == UDSP_OK);
	CHECK_UINT(before.srtp_protect_fail, 0);
	CHECK_UINT(after.srtp_protect_fail, 2);
	CHECK_UINT(after.srtp_protect_ok, before.srtp_protect_ok);

out:
	eh_gate_disarm(&gate);
	CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
	udsp_srtp_destroy(&peer);
	tm_call_close(&t, &wire);
	eh_gate_close(&gate);
}

#ifdef HAVE_OPUS
static void case_stream_diag_epochs(void)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	struct udsp_stream_diag before, after;
	struct udsp_media media;
	struct tm_wire wire, next;
	struct eh t;

	CASE("a clock change restarts transport counters "
	     "but keeps SRTP counters");
	if (!tm_up(&t, "USA") || !tm_wire_open(&wire, 0) ||
	    !tm_wire_open(&next, 0)) {
		CHECK(false);
		return;
	}
	tm_media(&media, UDSP_SEL_PCMU, 0, 10);
	media.rtcp_enable = false;
	media.srtp.suite = UDSP_SRTP_AES_CM_128_SHA1_80;
	tm_key_set(media.srtp.key_tx, 0x31);
	tm_key_set(media.srtp.key_rx, 0x62);
	tm_stream_config(&t, &wire, &media);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	if (!eh_gate_park(&gate))
		goto out;
	CHECK(close(wire.rtp_peer) == 0);
	wire.rtp_peer = -1;
	if (!eh_gate_step(&gate, 3))
		goto out;
	CHECK(udsp_stream_diag(t.s, 0, 0, &before) == UDSP_OK);
	CHECK(before.tx_fail >= 3);
	CHECK(before.srtp_protect_ok >= 3);

	media.codec = UDSP_SEL_OPUS;
	media.codec_pt = 111;
	/* Receive-only keeps the count fixed across activation. */
	media.direction = UDSP_MEDIA_RECVONLY;
	tm_key_set(media.srtp.key_tx, 0x43);
	tm_key_set(media.srtp.key_rx, 0x78);
	if (!tm_gate_config_run(&gate, &t, &media, &next))
		goto out;
	CHECK(udsp_stream_diag(t.s, 0, 0, &after) == UDSP_OK);
	CHECK_UINT(after.tx_fail, 0);
	CHECK_UINT(after.srtp_protect_ok, before.srtp_protect_ok);

out:
	eh_gate_disarm(&gate);
	CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
	tm_wire_close(&next);
	tm_call_close(&t, &wire);
	eh_gate_close(&gate);
}
#endif

/* One 20 ms PCMU packet of the far end, unmarked as the far-end test peer
 * sends it: a 1000 Hz tone, or digital silence. The gate holds the tick, so the
 * packet waits in the receive ring until the next step drains it. */
static bool tm_gate_burst_send(struct udsp_stream *st, struct tm_wire *w,
			       bool tone, double *phase)
{
	uint8_t pkt[TM_PKT_MAX];
	int16_t pcm[160];
	uint8_t pay[160];
	unsigned want;
	size_t len;

	if (!tm_gate_ring_prepare(st, 1, &want))
		return false;
	memset(pcm, 0, sizeof(pcm));
	if (tone)
		tm_tone_frame(pcm, 160, 1000.0, 8000.0, phase);
	tm_ulaw_encode(pcm, 160, pay);
	len = tm_rtp_build(pkt, 0, false, w->seq++, w->ts, w->ssrc, pay,
			   sizeof(pay));
	w->ts += 160u;
	if (send(w->rtp_peer, pkt, len, MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
		return false;

	return tm_gate_ring_wait(st, want);
}

/*
 * The length in ms of the tone that the record holds, from eh_rec_mark on:
 * the record's energy over the energy of one 10 ms window inside the tone.
 * A count of loud windows would also count the few samples that the 16 kHz
 * upsampler moves past a window edge. first returns the first loud window.
 */
static double tm_rec_tone_ms(unsigned rate, unsigned *first)
{
	unsigned frame = rate / 100u;
	unsigned have = eh_rec_have();
	double full;

	for (*first = 0; (*first + 1u) * frame <= have; (*first)++) {
		if (eh_energy(*first * frame, frame) > 1e5)
			break;
	}
	/* The tone is 100 ms long, so its third window is inside it. */
	full = eh_energy((*first + 2u) * frame, frame);
	if (full <= 0.0)
		return 0.0;

	return eh_energy(0, have) * have / full / frame * 10.0;
}

/* One leg of case_first_packet at one line rate. */
static void tm_first_packet_run(unsigned rate)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	double phase = 0.0;
	unsigned first = 0;
	double ms;
	struct eh t;

	if (!tm_call_up_rate(&t, "USA", rate, &w))
		return;
	tm_media(&m, UDSP_SEL_PCMU, 0, 20);
	m.jb_init_ms = 30;
	m.jb_max_ms = 250;
	tm_stream_config(&t, &w, &m);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL);
	if (!st || !eh_gate_park(&gate) || !eh_gate_step(&gate, 20))
		goto out;
	eh_rec_mark();
	for (unsigned i = 0; i < 15u; i++) {
		if (!tm_gate_burst_send(st, &w, i < 5u, &phase) ||
		    !eh_gate_step(&gate, 2))
			goto out;
	}
	if (!eh_gate_step(&gate, 20))
		goto out;
	ms = tm_rec_tone_ms(rate, &first);
	printf("# first packet %u Hz: tone %.1f ms from window %u\n", rate, ms,
	       first);
	CHECK(fabs(ms - 100.0) <= 1.0);
out:
	eh_gate_disarm(&gate);
	tm_call_down(&t, &w);
	eh_gate_close(&gate);
}

/*
 * A fresh downlink stream after silence. The far end sends nothing for
 * 200 ms, then a 100 ms tone in five 20 ms packets, one each 20 ms, then
 * silence. RFC 3550 A.1 keeps the first packet of a new source until its
 * successor proves the source. The line must play the whole tone, the first
 * packet's 160 samples at 8 kHz or 320 at 16 kHz included. A later start
 * passes; a lost packet fails. The buffer settings are the uvoice defaults of
 * 30 and 250 ms.
 */
static void case_first_packet(void)
{
	CASE("a fresh stream plays its first packet");
	tm_first_packet_run(UDSP_SAMPLE_RATE);
	tm_first_packet_run(UDSP_SAMPLE_RATE_WB);
}

/* --- the refill of a restart --------------------------------------------- */

/* A hold of 40 ms and a drain of 70 ms: the first hook after the hold comes
 * 110 ms after it began, past the sync wait of 100 ms. */
#define TM_RP_HOLD_US		40000u
#define TM_RP_DRAIN_US		70000u

static void tm_rp_open_next(unsigned hold_us, unsigned drain_us)
{
	tm_rp_reset(hold_us, drain_us);
	tm_ops_open = &tm_rp_ops;
}

static void tm_rp_stats(struct eh *t, struct engine_stats *st)
{
	struct engine *e = atomic_load(&t->s->engine_pub);

	memset(st, 0, sizeof(*st));
	if (e)
		engine_stats_get(e, st);
}

/* The written periods from the mark on, of count, that carry hz at level
 * dBm0 or more. */
static unsigned tm_rp_windows(unsigned count, double hz, double level)
{
	unsigned n = 0;

	for (unsigned i = 0; i < count; i++)
		n += eh_tone_dbm0(i * 80u, 80u, hz, UDSP_SAMPLE_RATE) >= level;

	return n;
}

static void tm_rp_record_check(const char *tag, struct eh *t, double hz,
			       double level)
{
	struct engine_stats st;
	unsigned tone = tm_rp_windows(12u, hz, level);

	tm_rp_stats(t, &st);
	printf("# rephase %s: %u periods written after the hold, %u of 12 "
	       "with the tone, %llu rephased, %llu rendered, %llu lead\n",
	       tag, eh_rec_have() / 80u, tone,
	       (unsigned long long)st.rephases,
	       (unsigned long long)st.rephase_rendered,
	       (unsigned long long)st.rephase_lead);
	CHECK_UINT(eh_rec_have(), 12u * 80u);
	CHECK_UINT(tone, 12);
	CHECK_UINT(st.rephases, 1);
	CHECK_UINT(st.rephase_rendered, 2);
	CHECK_UINT(st.rephase_lead, 0);
	CHECK_UINT(st.trim_drops, TM_RP_HOLD);
}

/* The timestamp of the first or the last RTP packet the near end sent to w
 * and the far end did not read yet, or false when none is queued. */
static bool tm_rp_uplink_ts(struct tm_wire *w, bool last, uint32_t *ts)
{
	uint8_t buf[TM_PKT_MAX];
	struct tm_pkt p;
	bool seen = false;
	ssize_t n;

	while ((n = tm_recv(w->rtp_peer, buf, sizeof(buf))) > 0) {
		if (!tm_rtp_parse(buf, (size_t)n, &p) || p.pt != 0u)
			continue;
		*ts = p.ts;
		seen = true;
		if (!last)
			break;
	}

	return seen;
}

/* A PCMU stream of a 1000 Hz tone, one packet a tick, with eight more queued
 * as the hold comes. The read of the hold is dropped, and the next one writes
 * the refill and the period of its tick, then one period a tick. The six
 * dropped periods reach the jitter buffer as four skipped units and two
 * played ones: its clock moves six units. On the uplink, the packet of the
 * restart stands seven periods after the last one before the hold: the six
 * dropped periods and its own. */
static void tm_rp_stream_run(void)
{
	const struct udsp_media m10 = {
		.codec = UDSP_SEL_PCMU, .codec_pt = 0, .ptime_ms = 10,
	};
	struct eh_gate gate = { .fd = { -1, -1 } };
	uint32_t playout, units, up_before = 0, up_after = 0;
	struct udsp_stream *st;
	bool up_seen = false;
	struct udsp_media m;
	struct tm_wire w;
	double phase = 0.0;
	struct eh t;

	tm_rp_open_next(0, 0);
	if (!tm_call_up(&t, "USA", &w, 0))
		return;
	tm_media(&m, UDSP_SEL_PCMU, 0, 10);
	tm_stream_config(&t, &w, &m);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL);
	if (!st || !eh_gate_park(&gate))
		goto out;
	/* The far end reads the uplink each tick, so its socket never fills. */
	for (unsigned i = 0; i < 80u; i++) {
		if (!tm_gate_tone_publish(st, &w, 1, &m10, 1000.0, 8000.0,
					  &phase) ||
		    !eh_gate_step(&gate, 1))
			goto out;
		up_seen = tm_rp_uplink_ts(&w, true, &up_before);
	}
	if (!tm_gate_tone_publish(st, &w, 8, &m10, 1000.0, 8000.0, &phase))
		goto out;
	eh_rec_mark();
	playout = st->jb.playout_ts;
	units = st->jb.skip_units;
	CHECK(up_seen);
	atomic_store(&tm_rp.arm, true);
	if (!eh_gate_step(&gate, 1))
		goto out;
	printf("# rephase stream: %u units skipped, clock %u units\n",
	       st->jb.skip_units - units, (st->jb.playout_ts - playout) / 80u);
	CHECK_UINT(st->jb.skip_units - units, TM_RP_HOLD - 2u);
	CHECK_UINT(st->jb.playout_ts - playout, TM_RP_HOLD * 80u);
	for (unsigned i = 0; i < 10u; i++) {
		if (!tm_gate_tone_publish(st, &w, 1, &m10, 1000.0, 8000.0,
					  &phase) ||
		    !eh_gate_step(&gate, 1))
			goto out;
		if (!i)
			CHECK(tm_rp_uplink_ts(&w, false, &up_after));
	}
	printf("# rephase stream: uplink step %u units\n",
	       (up_after - up_before) / 80u);
	CHECK_UINT(up_after - up_before, (TM_RP_HOLD + 1u) * 80u);
	tm_rp_record_check("stream", &t, 1000.0, -20.0);
out:
	eh_gate_disarm(&gate);
	tm_call_down(&t, &w);
	eh_gate_close(&gate);
}

/* The dial tone of the region on line 0, off hook. */
static void tm_rp_tone_run(void)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	struct eh t;

	tm_rp_open_next(0, 0);
	if (!tm_up(&t, "USA")) {
		CHECK(false);
		tm_down(&t);
		return;
	}
	CHECK(eh_hook_seize(&t));
	CHECK(udsp_tone_play(t.s, 0, UDSP_TONE_DIAL, 0) == UDSP_OK);
	if (!eh_gate_park(&gate) || !eh_gate_step(&gate, 20))
		goto out;
	eh_rec_mark();
	atomic_store(&tm_rp.arm, true);
	if (!eh_gate_step(&gate, 11))
		goto out;
	tm_rp_record_check("dial tone", &t, 440.0, -25.0);
out:
	eh_gate_disarm(&gate);
	tm_down(&t);
	eh_gate_close(&gate);
}

/*
 * A restart of a dry ring on a scripted back end (tm_rp_ops): a hold of six
 * periods, the ring empty after it. The engine drops the read of the hold and
 * renders the refill on that drop. So the periods it writes from the restart
 * on are the audio of their ticks. A decoded stream and a local dial tone
 * carry their tone in every written period. No period of silence goes between
 * the audio before the hold and the audio after it.
 */
static void case_rephase_refill(void)
{
	CASE("a rephase writes a decoded stream and a dial tone through its "
	     "refill");
	tm_rp_stream_run();
	tm_rp_tone_run();
}

/*
 * A command across a dry hold. The scripted back end holds the read 40 ms,
 * and the read after it waits 70 ms more for its completion. The engine
 * drops the read of the hold and runs no tick hook before the restart, 110 ms
 * after the hold began. A channel configuration posted as the hold begins
 * must still ack within the production sync wait (cmd.h UDSP_SYNC_TICK_MS).
 * The control hook drains the commands on the dropped read.
 */
static void case_rephase_command(void)
{
	struct udsp_chan_cfg ccfg;
	struct engine_stats st;
	uint64_t end, from, ms = 0;
	int rc = -1;
	struct eh t;

	CASE("a command posted as a dry hold begins acks on the read after "
	     "the hold");
	tm_rp_open_next(TM_RP_HOLD_US, TM_RP_DRAIN_US);
	if (!tm_up(&t, "USA")) {
		CHECK(false);
		tm_down(&t);
		return;
	}
	memset(&ccfg, 0, sizeof(ccfg));
	end = eh_mono_us() + 2000000u;
	do {
		eh_sleep_ms(5);
		tm_rp_stats(&t, &st);
	} while (!st.delay_calibrated && eh_mono_us() < end);
	CHECK(st.delay_calibrated);
	atomic_store(&tm_rp.arm, true);
	end = eh_mono_us() + 2000000u;
	while (!atomic_load(&tm_rp.in_hold) && eh_mono_us() < end)
		sched_yield();
	CHECK(atomic_load(&tm_rp.in_hold));
	from = eh_mono_us();
	rc = udsp_chan_config(t.s, 0, &ccfg);
	ms = (eh_mono_us() - from) / 1000u;
	end = eh_mono_us() + 2000000u;
	do {
		eh_sleep_ms(5);
		tm_rp_stats(&t, &st);
	} while (!st.rephases && eh_mono_us() < end);
	printf("# rephase command: acked in %llu ms, hold %u ms, drain %u ms, "
	       "%llu rephased\n", (unsigned long long)ms,
	       TM_RP_HOLD_US / 1000u, TM_RP_DRAIN_US / 1000u,
	       (unsigned long long)st.rephases);
	CHECK(rc == UDSP_OK);
	CHECK(ms < UDSP_SYNC_TICK_MS);
	CHECK(ms + 5u >= TM_RP_HOLD_US / 1000u);
	CHECK_UINT(st.rephases, 1);
	CHECK_UINT(st.rephase_rendered, 2);
	tm_down(&t);
}

/* --- skipped playout opportunities --------------------------------------- */

/* The tone of a skip case unit: 1000 Hz before the park, and a step of 300 Hz
 * from 500 Hz for each unit that arrives during it. */
#define TM_SKIP_UNITS		6u
#define TM_SKIP_PERIODS		5u
/* A unit plays at -6 dBm0 or so; a window under this level is not a unit. */
#define TM_SKIP_LEVEL_DBM0	(-20.0)

static double tm_skip_hz(unsigned unit)
{
	return 500.0 + 300.0 * unit;
}

/* The unit of the burst whose tone the 10 ms window from from carries, or -1
 * for the 1000 Hz tone of the steady stream, and the level of that tone. The
 * level is what tells a played unit from silence, which reads as whichever
 * tone is least quiet. */
static int tm_skip_unit_rate(unsigned from, unsigned n, unsigned rate,
			     double *dbm0)
{
	double best = eh_tone_dbm0(from, n, 1000.0, rate);
	int unit = -1;

	for (unsigned k = 0; k < TM_SKIP_UNITS; k++) {
		double level = eh_tone_dbm0(from, n, tm_skip_hz(k), rate);

		if (level > best) {
			best = level;
			unit = (int)k;
		}
	}
	*dbm0 = best;

	return unit;
}

static int tm_skip_unit_get(unsigned from, double *dbm0)
{
	return tm_skip_unit_rate(from, 80, UDSP_SAMPLE_RATE, dbm0);
}

/* The output partition of jb.h Event mapping. */
static uint64_t tm_jb_out_total(const struct udsp_jb_stats *st)
{
	return st->frames_out + st->plc + st->cn + st->silence +
	       st->skew_inserts;
}

/* A started PCMU stream at ptime 10 on a parked tick, warmed up on the steady
 * tone for ticks ticks, one packet a tick. */
static struct udsp_stream *tm_skip_up(struct eh *t, struct tm_wire *w,
				      struct eh_gate *gate, unsigned ticks,
				      double *phase)
{
	const struct udsp_media m10 = {
		.codec = UDSP_SEL_PCMU, .codec_pt = 0, .ptime_ms = 10,
	};
	struct udsp_stream *st;
	struct udsp_media m;

	tm_media(&m, UDSP_SEL_PCMU, 0, 10);
	tm_stream_config(t, w, &m);
	CHECK(udsp_stream_start(t->s, 0, 0) == UDSP_OK);
	st = stream_at(t->s, 0, 0);
	CHECK(st != NULL);
	if (!st || !eh_gate_park(gate))
		return NULL;
	for (unsigned i = 0; i < ticks; i++) {
		if (!tm_gate_tone_publish(st, w, 1, &m10, 1000.0, 8000.0,
					  phase) ||
		    !eh_gate_step(gate, 1))
			return NULL;
	}

	return st;
}

static bool tm_skip_steady(struct eh_gate *gate, struct udsp_stream *st,
			   struct tm_wire *w, unsigned ticks, double *phase)
{
	const struct udsp_media m10 = {
		.codec = UDSP_SEL_PCMU, .codec_pt = 0, .ptime_ms = 10,
	};

	for (unsigned i = 0; i < ticks; i++) {
		if (!tm_gate_tone_publish(st, w, 1, &m10, 1000.0, 8000.0,
					  phase) ||
		    !eh_gate_step(gate, 1))
			return false;
	}

	return true;
}

/* The burst a parked tick takes in: one packet per unit, each unit its own
 * tone, sent from the far end of w. */
static bool tm_skip_burst(struct udsp_stream *st, struct tm_wire *w,
			  unsigned units, double *phase)
{
	unsigned want_head;

	if (!tm_gate_ring_prepare(st, units, &want_head))
		return false;
	for (unsigned k = 0; k < units; k++)
		tm_tone_send_pt(w, 0, 10, tm_skip_hz(k), 8000.0, phase,
				false);

	return tm_gate_ring_wait(st, want_head);
}

/*
 * Playout opportunities that the engine never ran. The far end sends
 * TM_SKIP_UNITS packets while the tick is parked, and the engine reports
 * TM_SKIP_PERIODS periods that it never ran. The jitter buffer must pass
 * exactly the units whose playout instants those periods held, and keep its
 * depth. Each burst unit carries its own tone, so the unit after the skip
 * names the playout clock. The record: five queued units passed, the clock
 * six units on, and burst unit 4 on the tick at a unit's level. The output
 * counters grow by the tick alone, and neither the skew nor the depth moves,
 * at once or 40 ticks later.
 */
static void case_jb_skip(void)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	struct udsp_jb_stats before, skip, after;
	uint32_t playout, units, frames, waits;
	struct udsp_stream *st;
	struct tm_wire w;
	double phase = 0.0;
	double level;
	int unit, want;
	struct eh t;

	CASE("skipped playout opportunities advance the receive jitter buffer");
	if (!tm_call_up(&t, "USA", &w, 0))
		return;
	st = tm_skip_up(&t, &w, &gate, 79, &phase);
	if (!st)
		goto out;
	eh_rec_mark();
	if (!tm_skip_steady(&gate, st, &w, 1, &phase))
		goto out;
	unit = tm_skip_unit_get(0, &level);
	printf("# jb skip: before the park, unit %d at %.1f dBm0\n", unit,
	       level);
	CHECK(unit == -1 && level > TM_SKIP_LEVEL_DBM0);
	CHECK(udsp_stream_jb_stats(t.s, 0, 0, &before) == UDSP_OK);
	playout = st->jb.playout_ts;
	units = st->jb.skip_units;
	frames = st->jb.skip_frames;
	waits = st->jb.skip_waits;
	/* The tick is parked, so the burst stands in the receive ring
	 * until the step that runs after the skip. */
	if (!tm_skip_burst(st, &w, TM_SKIP_UNITS, &phase))
		goto out;
	atomic_store(&tm_be.skip_next, TM_SKIP_PERIODS);
	eh_rec_mark();
	if (!eh_gate_step(&gate, 1))
		goto out;
	unit = tm_skip_unit_get(0, &level);
	CHECK(udsp_stream_jb_stats(t.s, 0, 0, &skip) == UDSP_OK);
	/* The skipped periods take the queued units first, then the burst. */
	want = (int)TM_SKIP_PERIODS - (int)(before.depth_ms / 10u);
	printf("# jb skip: depth %u ms -> %u ms, first unit %d at %.1f dBm0 "
	       "(want %d), clock %u units, skipped %u/%u/%u\n",
	       before.depth_ms, skip.depth_ms, unit, level, want,
	       (st->jb.playout_ts - playout) / 80u, st->jb.skip_units - units,
	       st->jb.skip_frames - frames, st->jb.skip_waits - waits);
	CHECK_UINT(before.depth_ms, 10);
	CHECK(unit == want && level > TM_SKIP_LEVEL_DBM0);
	CHECK_UINT(st->jb.skip_units - units, TM_SKIP_PERIODS);
	CHECK_UINT(st->jb.skip_frames - frames, TM_SKIP_PERIODS);
	CHECK_UINT(st->jb.skip_waits - waits, 0);
	CHECK_UINT(st->jb.playout_ts - playout, (TM_SKIP_PERIODS + 1u) * 80u);
	CHECK_UINT(skip.depth_ms, before.depth_ms);
	CHECK_UINT(tm_jb_out_total(&skip), tm_jb_out_total(&before) + 1u);
	CHECK_UINT(skip.skew_drops, before.skew_drops);
	CHECK_UINT(skip.skew_inserts, before.skew_inserts);
	CHECK_UINT(skip.underruns, before.underruns);
	if (!tm_skip_steady(&gate, st, &w, 40, &phase))
		goto out;
	CHECK(udsp_stream_jb_stats(t.s, 0, 0, &after) == UDSP_OK);
	CHECK_UINT(after.depth_ms, before.depth_ms);
	CHECK_UINT(tm_jb_out_total(&after), tm_jb_out_total(&before) + 41u);
	CHECK_UINT(after.skew_drops, before.skew_drops);
	CHECK_UINT(after.skew_inserts, before.skew_inserts);
	CHECK_UINT(after.underruns, before.underruns);
	CHECK_UINT(after.late_drop, before.late_drop);
	CHECK_UINT(st->jb.skip_units - units, TM_SKIP_PERIODS);
out:
	eh_gate_disarm(&gate);
	tm_call_down(&t, &w);
	eh_gate_close(&gate);
}

/* One 10 ms PCMU packet of a tone from the far end of w. */
static void tm_skip_send(struct tm_wire *w, double hz, bool marker,
			 double *phase)
{
	uint8_t pkt[TM_PKT_MAX];
	int16_t pcm[80];
	uint8_t pay[80];
	size_t len;

	tm_tone_frame(pcm, 80, hz, 8000.0, phase);
	tm_ulaw_encode(pcm, 80, pay);
	len = tm_rtp_build(pkt, 0, marker, w->seq, w->ts, w->ssrc, pay, 80);
	w->seq++;
	w->ts += 80u;
	if (send(w->rtp_peer, pkt, len, MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
		return;
}

/* Units first to first + count - 1 of the burst tones, into a parked tick. */
static bool tm_skip_units_send(struct udsp_stream *st, struct tm_wire *w,
			       unsigned first, unsigned count, double *phase)
{
	unsigned want;

	if (!tm_gate_ring_prepare(st, count, &want))
		return false;
	for (unsigned k = first; k < first + count; k++)
		tm_skip_send(w, tm_skip_hz(k), false, phase);

	return tm_gate_ring_wait(st, want);
}

/* The skip of TM_SKIP_PERIODS periods and the tick after it, recorded. */
static bool tm_skip_tick(struct eh_gate *gate, int *unit, double *level)
{
	atomic_store(&tm_be.skip_next, TM_SKIP_PERIODS);
	eh_rec_mark();
	if (!eh_gate_step(gate, 1))
		return false;
	*unit = tm_skip_unit_get(0, level);

	return true;
}

/* The new source of case_jb_skip_source: a timeline of its own. */
static void tm_skip_source_new(struct tm_wire *w)
{
	w->ssrc = 0x0badf00du;
	w->seq = 3000;
	w->ts += 40000u;
}

/* The packets of way v of case_jb_skip_source, up to the skip. */
static bool tm_skip_source_send(struct eh_gate *gate, struct udsp_stream *st,
				struct tm_wire *w, unsigned v, double *phase)
{
	unsigned want;

	if (v == 0u) {
		tm_skip_source_new(w);
		return tm_skip_units_send(st, w, 0, TM_SKIP_UNITS, phase);
	}
	if (v == 1u) {
		/* The first packet of the new source, alone: the receiver
		 * holds it and the tick plays on. */
		tm_skip_source_new(w);
		return tm_skip_units_send(st, w, 0, 1, phase) &&
		       eh_gate_step(gate, 1) &&
		       tm_skip_units_send(st, w, 1, TM_SKIP_UNITS - 1u, phase);
	}
	/* The old source's burst and the new source's first packet, in one
	 * parked ring. */
	if (!tm_gate_ring_prepare(st, TM_SKIP_UNITS + 1u, &want))
		return false;
	for (unsigned k = 0; k < TM_SKIP_UNITS; k++)
		tm_skip_send(w, tm_skip_hz(k), false, phase);
	tm_skip_source_new(w);
	tm_skip_send(w, tm_skip_hz(5), false, phase);

	return tm_gate_ring_wait(st, want);
}

static void tm_skip_source_run(unsigned v)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	struct udsp_jb_stats before, skip;
	uint32_t timeline, units;
	struct udsp_stream *st;
	struct tm_wire w;
	double phase = 0.0;
	double level;
	int unit;
	struct eh t;

	if (!tm_call_up(&t, "USA", &w, 0))
		return;
	st = tm_skip_up(&t, &w, &gate, 80, &phase);
	if (!st)
		goto out;
	CHECK(udsp_stream_jb_stats(t.s, 0, 0, &before) == UDSP_OK);
	timeline = udsp_jb_timeline(&st->jb);
	units = st->jb.skip_units;
	if (!tm_skip_source_send(&gate, st, &w, v, &phase))
		goto out;
	if (v == 1u)
		CHECK_UINT(udsp_jb_timeline(&st->jb), timeline);
	if (!tm_skip_tick(&gate, &unit, &level))
		goto out;
	CHECK(udsp_stream_jb_stats(t.s, 0, 0, &skip) == UDSP_OK);
	printf("# jb skip source %u: unit %d at %.1f dBm0, timeline %u -> %u, "
	       "skipped %u\n", v, unit, level, timeline,
	       udsp_jb_timeline(&st->jb), st->jb.skip_units - units);
	CHECK(level > TM_SKIP_LEVEL_DBM0);
	if (v == 2u) {
		CHECK_UINT(unit, TM_SKIP_PERIODS - 1u);
		CHECK_UINT(udsp_jb_timeline(&st->jb), timeline);
		CHECK_UINT(st->jb.skip_units - units, TM_SKIP_PERIODS);
	} else {
		CHECK_UINT(unit, 0);
		CHECK_UINT(udsp_jb_timeline(&st->jb), timeline + 1u);
		CHECK_UINT(st->jb.skip_units, units);
		CHECK_UINT(skip.resyncs, before.resyncs + 1u);
	}
out:
	eh_gate_disarm(&gate);
	tm_call_down(&t, &w);
	eh_gate_close(&gate);
}

/*
 * A new source inside a hold. The skipped opportunities belong to the old
 * timeline, and a put that starts a timeline cancels them. So the new source
 * plays from its first unit. In the first way in, every packet of the new
 * source arrives inside the hold. In the second, its first packet waits on
 * the RFC 3550 A.1 probation, and a packet inside the hold releases it. In
 * the third, only its first packet arrives inside the hold, and the old
 * timeline pays the skip.
 */
static void case_jb_skip_source(void)
{
	CASE("a new source inside a hold starts its timeline unadvanced");
	tm_skip_source_run(0);
	CASE("a source its probation releases inside a hold starts "
	     "unadvanced");
	tm_skip_source_run(1);
	CASE("a new source held on probation leaves the hold to the old "
	     "timeline");
	tm_skip_source_run(2);
}

/* The rows of case_jb_skip_event. */
enum tm_tev_skip {
	TM_TEV_SKIP_THROUGH,
	TM_TEV_SKIP_MUTED,
	TM_TEV_SKIP_SEGMENT,
	TM_TEV_SKIP_ENDS,
	TM_TEV_SKIP_INSIDE,
	TM_TEV_SKIP_AFTER,
	TM_TEV_SKIP_TIMEOUT,
	TM_TEV_SKIP_REPLACE,
	TM_TEV_SKIP_FIRST_END,
	TM_TEV_SKIP_SOURCE,
	TM_TEV_SKIP_ROWS,
};

/* The rows whose event plays before the hold. */
static bool tm_tev_skip_playing(enum tm_tev_skip row)
{
	return row <= TM_TEV_SKIP_ENDS || row == TM_TEV_SKIP_TIMEOUT ||
	       row == TM_TEV_SKIP_REPLACE || row == TM_TEV_SKIP_SOURCE;
}

static bool tm_tev_skip_attempt(enum tm_tev_skip row)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	struct udsp_stream *st = NULL;
	struct udsp_media m;
	struct tm_wire w;
	struct eh t;
	uint64_t sent_us;
	bool scheduled = true;
	uint32_t r0 = 0;
	uint64_t start_ns;

	if (!tm_telev_up(&t, &w, &m)) {
		CHECK(false);
		return true;
	}
	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL);
	if (!st || !eh_gate_park(&gate) ||
	    !tm_gate_telev_prime(&gate, st, &w, 6))
		goto out;
	if (row == TM_TEV_SKIP_MUTED)
		atomic_store(&t.s->chan[0].mute, true);
	if (tm_tev_skip_playing(row)) {
		uint16_t first = row == TM_TEV_SKIP_SEGMENT ?
				 TELEV_DURATION_MAX : 160u;

		if (!tm_gate_telev_send(st, &w, 5, 10, false, first, true, 1) ||
		    !eh_gate_step(&gate, 2))
			goto out;
		r0 = st->tev_play.rendered;
		CHECK_UINT(r0, 160);
	}
	start_ns = st->tev_play.start_ns;
	switch (row) {
	case TM_TEV_SKIP_THROUGH:
	case TM_TEV_SKIP_MUTED:
		if (!tm_gate_telev_send(st, &w, 5, 10, false, 320, false, 1))
			goto out;
		break;
	case TM_TEV_SKIP_SEGMENT:
		/* RFC 4733 2.5.1.3: the next segment starts where the
		 * ceiling ended the first. */
		w.ts += TELEV_DURATION_MAX;
		if (!tm_gate_telev_send(st, &w, 5, 10, false, 160, false, 1))
			goto out;
		break;
	case TM_TEV_SKIP_ENDS:
		if (!tm_gate_telev_send(st, &w, 5, 10, true, 320, false,
					UDSP_TELEV_REPEAT))
			goto out;
		break;
	case TM_TEV_SKIP_INSIDE:
		if (!tm_gate_telev_send(st, &w, 5, 10, false, 160, true, 1))
			goto out;
		break;
	case TM_TEV_SKIP_TIMEOUT:
		eh_sleep_ms(TM_TEV_TIMEOUT_MS + 20u);
		break;
	case TM_TEV_SKIP_REPLACE:
		/* Another digit, at a timestamp of its own. */
		w.ts += 800u;
		if (!tm_gate_telev_send(st, &w, 6, 10, false, 160, true, 1))
			goto out;
		break;
	case TM_TEV_SKIP_FIRST_END:
		/* The start packets were lost: the end is the first packet of
		 * the event. */
		if (!tm_gate_telev_send(st, &w, 7, 10, true, 480, false, 1))
			goto out;
		break;
	case TM_TEV_SKIP_SOURCE: {
		unsigned want;
		double phase = 0.0;

		/* Two packets of a new source, so its probation ends. */
		if (!tm_gate_ring_prepare(st, 2, &want))
			goto out;
		tm_skip_source_new(&w);
		tm_skip_send(&w, 1000.0, true, &phase);
		tm_skip_send(&w, 1000.0, false, &phase);
		if (!tm_gate_ring_wait(st, want))
			goto out;
		break;
	}
	default:
		break;
	}
	sent_us = eh_mono_us();
	atomic_store(&tm_be.skip_next, TM_SKIP_PERIODS);
	if (!eh_gate_step(&gate, 1))
		goto out;
	/* The updates keep the event within the lost-end bound only while
	 * the wall time between them stays short. */
	if (row != TM_TEV_SKIP_TIMEOUT &&
	    eh_mono_us() - sent_us > TM_TEV_TIMEOUT_MS * 1000u / 2u) {
		scheduled = false;
		goto out;
	}
	if (row == TM_TEV_SKIP_AFTER) {
		if (!tm_gate_telev_send(st, &w, 5, 10, false, 160, true, 1) ||
		    !eh_gate_step(&gate, 1))
			goto out;
	}
	printf("# jb skip event %u: rendered %u -> %u, seg %u, done %u\n",
	       (unsigned)row, r0, st->tev_play.rendered,
	       st->tev_play.seg_units, st->tev_play.done);
	switch (row) {
	case TM_TEV_SKIP_THROUGH:
	case TM_TEV_SKIP_MUTED:
		CHECK_UINT(st->tev_play.rendered,
			   r0 + (TM_SKIP_PERIODS + 1u) * 80u);
		CHECK_UINT(st->tev_play.start_ns, start_ns);
		break;
	case TM_TEV_SKIP_SEGMENT:
		CHECK_UINT(st->tev_play.seg_units, TELEV_DURATION_MAX);
		CHECK_UINT(st->tev_play.rendered,
			   r0 + (TM_SKIP_PERIODS + 1u) * 80u);
		break;
	case TM_TEV_SKIP_ENDS:
		/* The end announces 320 units: the skip renders the 160
		 * left, and the tick finds the event done. */
		CHECK_UINT(st->tev_play.rendered, 320);
		CHECK(st->tev_play.done);
		CHECK_UINT(st->tev_play.start_ns, start_ns);
		break;
	case TM_TEV_SKIP_INSIDE:
	case TM_TEV_SKIP_AFTER:
		CHECK_UINT(st->tev_play.rendered, 80);
		CHECK(st->tev_play.active);
		break;
	case TM_TEV_SKIP_TIMEOUT:
		CHECK_UINT(st->tev_play.rendered, r0);
		CHECK(st->tev_play.done);
		break;
	case TM_TEV_SKIP_REPLACE:
		CHECK_UINT(st->tev_play.event, 6);
		CHECK_UINT(st->tev_play.rendered, 80);
		break;
	case TM_TEV_SKIP_FIRST_END:
		CHECK_UINT(st->tev_play.event, 7);
		CHECK(st->tev_play.ended);
		CHECK_UINT(st->tev_play.target, 480);
		CHECK_UINT(st->tev_play.rendered, 80);
		break;
	case TM_TEV_SKIP_SOURCE:
		/* The new source scopes the event state anew (stream.c
		 * stream_rtp_packet): nothing plays and nothing advances. */
		CHECK(!st->tev_play.active);
		CHECK_UINT(st->tev_play.rendered, 0);
		break;
	default:
		break;
	}

out:
	eh_gate_disarm(&gate);
	tm_call_down(&t, &w);
	eh_gate_close(&gate);

	return scheduled;
}

static bool tm_tev_skip_through(void)
{
	return tm_tev_skip_attempt(TM_TEV_SKIP_THROUGH);
}

static bool tm_tev_skip_muted(void)
{
	return tm_tev_skip_attempt(TM_TEV_SKIP_MUTED);
}

static bool tm_tev_skip_segment(void)
{
	return tm_tev_skip_attempt(TM_TEV_SKIP_SEGMENT);
}

static bool tm_tev_skip_ends(void)
{
	return tm_tev_skip_attempt(TM_TEV_SKIP_ENDS);
}

static bool tm_tev_skip_inside(void)
{
	return tm_tev_skip_attempt(TM_TEV_SKIP_INSIDE);
}

static bool tm_tev_skip_after(void)
{
	return tm_tev_skip_attempt(TM_TEV_SKIP_AFTER);
}

static bool tm_tev_skip_timeout(void)
{
	return tm_tev_skip_attempt(TM_TEV_SKIP_TIMEOUT);
}

static bool tm_tev_skip_replace(void)
{
	return tm_tev_skip_attempt(TM_TEV_SKIP_REPLACE);
}

static bool tm_tev_skip_first_end(void)
{
	return tm_tev_skip_attempt(TM_TEV_SKIP_FIRST_END);
}

static bool tm_tev_skip_source(void)
{
	return tm_tev_skip_attempt(TM_TEV_SKIP_SOURCE);
}

/*
 * The event that a line renders, across a hold. The tracker (rtp.h struct
 * telev_play) counts the clock units rendered, 80 per 8 kHz tick. An event
 * that played before the hold renders through it: the skip advances its
 * tracker by the five skipped periods, as on muted ticks. An update, a
 * segment continuation or repeated ends inside the hold keep the same event.
 * An event that starts inside or after the hold takes no skipped time. An
 * event with no update for the lost-end bound times out instead.
 */
static void case_jb_skip_event(void)
{
	CASE("an event that plays through a hold renders the skipped periods");
	tm_telev_retry(tm_tev_skip_through, "event through a hold");
	CASE("a muted line renders the skipped periods of its event");
	tm_telev_retry(tm_tev_skip_muted, "muted event through a hold");
	CASE("a segment that continues an event inside a hold keeps the "
	     "skipped periods");
	tm_telev_retry(tm_tev_skip_segment, "segmented event");
	CASE("repeated ends inside a hold end the event at its duration");
	tm_telev_retry(tm_tev_skip_ends, "repeated ends");
	CASE("an event that starts inside a hold takes none of it");
	tm_telev_retry(tm_tev_skip_inside, "event inside a hold");
	CASE("an event that starts after a hold takes none of it");
	tm_telev_retry(tm_tev_skip_after, "event after a hold");
	CASE("an event with no update for the lost-end bound times out over a "
	     "hold");
	tm_telev_retry(tm_tev_skip_timeout, "event timeout over a hold");
	CASE("a digit that replaces the event inside a hold takes none of it");
	tm_telev_retry(tm_tev_skip_replace, "event replaced in a hold");
	CASE("a first-seen end inside a hold renders its own duration");
	tm_telev_retry(tm_tev_skip_first_end, "first-seen end in a hold");
	CASE("a new source inside a hold ends the digit that played");
	tm_telev_retry(tm_tev_skip_source, "new source in a hold");
}

/* One 10 ms G.722 packet of a tone. The far end's decoder g->dec hears every
 * packet in order, so pcm takes what a receiver in step with it plays. */
static void tm_g722_send_ref(struct tm_wire *w, struct tm_g722 *g, double hz,
			     double *phase, int16_t *pcm)
{
	int16_t tone[TM_G722_FRAME];
	uint8_t pkt[TM_PKT_MAX];
	uint8_t pay[TM_G722_BYTES];
	size_t len;

	for (unsigned i = 0; i < TM_G722_FRAME; i++) {
		tone[i] = (int16_t)(8000.0 * sin(*phase));
		*phase += 2.0 * M_PI * hz / (double)UDSP_SAMPLE_RATE_WB;
	}
	if (udsp_codec_g722_ops.encode(g->enc, tone, TM_G722_FRAME, pay,
				       TM_G722_BYTES) != (int)TM_G722_BYTES ||
	    !tm_g722_decode(g, pay, TM_G722_BYTES, pcm))
		return;
	len = tm_rtp_build(pkt, 9, w->seq == 1000, w->seq, w->ts, w->ssrc, pay,
			   TM_G722_BYTES);
	w->seq++;
	w->ts += 80u;
	if (send(w->rtp_peer, pkt, len, MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
		return;
}

/* The normalized correlation of the recorded frame from from with pcm. */
static double tm_rec_corr(unsigned from, const int16_t *pcm, unsigned n)
{
	const int16_t *rec = eh_be.rec + eh_be.rec_base + from;
	double xy = 0.0, xx = 0.0, yy = 0.0;

	if (from + n > eh_rec_have())
		return 0.0;
	for (unsigned i = 0; i < n; i++) {
		xy += (double)rec[i] * pcm[i];
		xx += (double)rec[i] * rec[i];
		yy += (double)pcm[i] * pcm[i];
	}
	if (xx <= 0.0 || yy <= 0.0)
		return 0.0;

	return xy / sqrt(xx * yy);
}

/*
 * G.722 on a 16 kHz line. The buffer counts the 8000 Hz RTP clock of the
 * codec, 80 units per 10 ms, and its decoder writes 160 samples. So the skip
 * moves the clock 80 a period. The skipped units go through the decoder, and
 * the ADPCM state at the unit on the tick is the far end's own. The line then
 * plays what a decoder that heard every packet plays, up to the line gain.
 */
static void case_jb_skip_g722(void)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	int16_t ref[TM_G722_FRAME];
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	struct tm_g722 g = { 0 };
	double phase = 0.0;
	uint32_t playout, units;
	unsigned want;
	double corr;
	struct eh t;

	CASE("a G.722 skip moves the clock 80 a period and keeps the decoder "
	     "in step");
	if (!tm_up_rate(&t, "USA", UDSP_SAMPLE_RATE_WB) ||
	    !tm_wire_open(&w, 0) || !tm_g722_open(&g)) {
		CHECK(false);
		tm_g722_close(&g);
		tm_down(&t);
		return;
	}
	tm_media(&m, UDSP_SEL_G722, 9, 10);
	tm_stream_config(&t, &w, &m);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL);
	if (!st || !eh_gate_park(&gate))
		goto out;
	CHECK_UINT(st->ts_per_frame, 80);
	for (unsigned i = 0; i < 80u; i++) {
		if (!tm_gate_ring_prepare(st, 1, &want))
			goto out;
		tm_g722_send_ref(&w, &g, 1000.0, &phase, ref);
		if (!tm_gate_ring_wait(st, want) || !eh_gate_step(&gate, 1))
			goto out;
	}
	playout = st->jb.playout_ts;
	units = st->jb.skip_units;
	if (!tm_gate_ring_prepare(st, TM_SKIP_UNITS, &want))
		goto out;
	/* The unit the tick plays is burst unit 4; ref keeps its decode. */
	for (unsigned k = 0; k < TM_SKIP_UNITS; k++) {
		int16_t pcm[TM_G722_FRAME];

		tm_g722_send_ref(&w, &g, tm_skip_hz(k) * 2.0, &phase,
				 k == 4u ? ref : pcm);
	}
	if (!tm_gate_ring_wait(st, want))
		goto out;
	atomic_store(&tm_be.skip_next, TM_SKIP_PERIODS);
	eh_rec_mark();
	if (!eh_gate_step(&gate, 1))
		goto out;
	corr = tm_rec_corr(0, ref, TM_G722_FRAME);
	printf("# jb skip g722: clock %u units of 80, skipped %u, "
	       "correlation %.6f\n", (st->jb.playout_ts - playout) / 80u,
	       st->jb.skip_units - units, corr);
	CHECK_UINT(st->jb.playout_ts - playout, (TM_SKIP_PERIODS + 1u) * 80u);
	CHECK_UINT(st->jb.skip_units - units, TM_SKIP_PERIODS);
	CHECK_UINT(st->jb.skip_frames, TM_SKIP_PERIODS);
	CHECK(corr > 0.9999);
out:
	eh_gate_disarm(&gate);
	CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
	tm_g722_close(&g);
	tm_call_close(&t, &w);
	eh_gate_close(&gate);
}

/*
 * The fixed buffer of a fax passthrough keeps its timeline continuous. A skip
 * passes one frame per opportunity and never steps over a suppressed gap.
 * The burst holds two units, a gap of three frames and a marked unit. The
 * five opportunities pass the queued unit, the two units and two gap frames.
 * The tick then plays the third gap frame as silence, and the next tick plays
 * the marked unit. The skip discards the audio of the passed units, because
 * the line played nothing in those periods.
 */
static void case_jb_skip_fixed(void)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	double phase = 0.0;
	double level;
	uint32_t playout, units, frames;
	unsigned want;
	int unit;
	struct eh t;

	CASE("a fixed passthrough buffer passes a hold one frame per period");
	if (!tm_call_up(&t, "USA", &w, 0))
		return;
	tm_media(&m, UDSP_SEL_PCMU, 0, 10);
	tm_stream_config(&t, &w, &m);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	CHECK(udsp_fax_profile(t.s, 0, 0, UDSP_FAX_PASSTHROUGH,
			       UDSP_SEL_PCMU, 0) == UDSP_OK);
	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL);
	if (!st || !eh_gate_park(&gate) ||
	    !tm_skip_steady(&gate, st, &w, 80, &phase))
		goto out;
	CHECK_UINT(st->jb.mode, UDSP_JB_FIXED);
	playout = st->jb.playout_ts;
	units = st->jb.skip_units;
	frames = st->jb.skip_frames;
	if (!tm_gate_ring_prepare(st, 5, &want))
		goto out;
	tm_skip_send(&w, tm_skip_hz(0), false, &phase);
	tm_skip_send(&w, tm_skip_hz(1), false, &phase);
	w.ts += 3u * 80u;
	tm_skip_send(&w, tm_skip_hz(2), true, &phase);
	tm_skip_send(&w, tm_skip_hz(3), false, &phase);
	tm_skip_send(&w, tm_skip_hz(4), false, &phase);
	if (!tm_gate_ring_wait(st, want) || !tm_skip_tick(&gate, &unit, &level))
		goto out;
	printf("# jb skip fixed: tick unit %d at %.1f dBm0, clock %u units, "
	       "skipped %u, frames %u\n", unit, level,
	       (st->jb.playout_ts - playout) / 80u, st->jb.skip_units - units,
	       st->jb.skip_frames - frames);
	CHECK_UINT(st->jb.skip_units - units, TM_SKIP_PERIODS);
	CHECK_UINT(st->jb.skip_frames - frames, 3);
	CHECK_UINT(st->jb.playout_ts - playout, (TM_SKIP_PERIODS + 1u) * 80u);
	CHECK(level < -40.0);
	eh_rec_mark();
	if (!eh_gate_step(&gate, 1))
		goto out;
	unit = tm_skip_unit_get(0, &level);
	CHECK(unit == 2 && level > TM_SKIP_LEVEL_DBM0);
out:
	eh_gate_disarm(&gate);
	tm_call_down(&t, &w);
	eh_gate_close(&gate);
}

/* A parked ring that takes up to the whole of its UDSP_NET_SLOTS. */
static bool tm_gate_ring_prepare_all(struct udsp_stream *st, unsigned datagrams,
				     unsigned *want)
{
	unsigned head = atomic_load_explicit(&st->ring.head,
					     memory_order_acquire);
	unsigned tail = atomic_load_explicit(&st->ring.tail,
					     memory_order_acquire);

	CHECK_UINT(head, tail);
	CHECK(datagrams <= UDSP_NET_SLOTS);
	if (head != tail || datagrams > UDSP_NET_SLOTS)
		return false;
	*want = head + datagrams;

	return true;
}

/* The state of a stream's jitter buffer a skip case compares across it. */
struct tm_skip_mark {
	struct udsp_jb_stats jb;
	uint32_t playout;
	uint32_t units;
	uint32_t frames;
	uint32_t waits;
	uint64_t stale;
};

static void tm_skip_mark_take(struct eh *t, const struct udsp_stream *st,
			      struct tm_skip_mark *k)
{
	CHECK(udsp_stream_jb_stats(t->s, 0, 0, &k->jb) == UDSP_OK);
	k->playout = st->jb.playout_ts;
	k->units = st->jb.skip_units;
	k->frames = st->jb.skip_frames;
	k->waits = st->jb.skip_waits;
	k->stale = atomic_load(&st->net_stale);
}

/*
 * The record every burst case checks after the tick of a skip of periods
 * periods: each one passed queued audio, the clock moved one unit more with
 * the tick, the depth and the counters stand, and no slot of the ring went
 * stale. Then 40 steady ticks, after which the depth stands again.
 */
static bool tm_skip_record(struct eh_gate *gate, struct eh *t,
			   struct udsp_stream *st, struct tm_wire *w,
			   const struct tm_skip_mark *k, unsigned periods,
			   double *phase)
{
	struct udsp_jb_stats now;

	CHECK(udsp_stream_jb_stats(t->s, 0, 0, &now) == UDSP_OK);
	CHECK_UINT(st->jb.skip_units - k->units, periods);
	CHECK_UINT(st->jb.skip_frames - k->frames, periods);
	CHECK_UINT(st->jb.skip_waits - k->waits, 0);
	CHECK_UINT(st->jb.playout_ts - k->playout, (periods + 1u) * 80u);
	CHECK_UINT(now.depth_ms, k->jb.depth_ms);
	CHECK_UINT(tm_jb_out_total(&now), tm_jb_out_total(&k->jb) + 1u);
	CHECK_UINT(now.skew_drops, k->jb.skew_drops);
	CHECK_UINT(now.skew_inserts, k->jb.skew_inserts);
	CHECK_UINT(now.underruns, k->jb.underruns);
	CHECK_UINT(atomic_load(&st->net_stale), k->stale);
	if (!tm_skip_steady(gate, st, w, 40, phase))
		return false;
	CHECK(udsp_stream_jb_stats(t->s, 0, 0, &now) == UDSP_OK);
	CHECK_UINT(now.depth_ms, k->jb.depth_ms);
	CHECK_UINT(tm_jb_out_total(&now), tm_jb_out_total(&k->jb) + 41u);
	CHECK_UINT(now.late_drop, k->jb.late_drop);
	CHECK_UINT(now.underruns, k->jb.underruns);

	return true;
}

/* One ring size of case_jb_skip_ring: n datagrams, a skip of n - 1. */
static void tm_skip_ring_run(unsigned n)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	struct tm_skip_mark k;
	struct udsp_stream *st;
	struct tm_wire w;
	double phase = 0.0;
	double level;
	unsigned want;
	int unit;
	struct eh t;

	if (!tm_call_up(&t, "USA", &w, 0))
		return;
	st = tm_skip_up(&t, &w, &gate, 80, &phase);
	if (!st)
		goto out;
	tm_skip_mark_take(&t, st, &k);
	CHECK_UINT(k.jb.depth_ms, 10);
	if (!tm_gate_ring_prepare_all(st, n, &want))
		goto out;
	for (unsigned u = 0; u < n; u++)
		tm_skip_send(&w, tm_skip_hz(u % TM_SKIP_UNITS), false, &phase);
	if (!tm_gate_ring_wait(st, want))
		goto out;
	atomic_store(&tm_be.skip_next, n - 1u);
	eh_rec_mark();
	if (!eh_gate_step(&gate, 1))
		goto out;
	unit = tm_skip_unit_get(0, &level);
	printf("# jb skip ring %u: unit %d at %.1f dBm0 (want %u), clock %u "
	       "units, skipped %u, waits %u\n", n, unit, level,
	       (n - 2u) % TM_SKIP_UNITS, (st->jb.playout_ts - k.playout) / 80u,
	       st->jb.skip_units - k.units, st->jb.skip_waits - k.waits);
	CHECK(unit == (int)((n - 2u) % TM_SKIP_UNITS) &&
	      level > TM_SKIP_LEVEL_DBM0);
	(void)tm_skip_record(&gate, &t, st, &w, &k, n - 1u, &phase);
out:
	eh_gate_disarm(&gate);
	tm_call_down(&t, &w);
	eh_gate_close(&gate);
}

/*
 * The whole ring goes in before the skip pays. A burst of n datagrams reaches
 * the parked tick, past the 16 that a tick drains. The engine then reports
 * n - 1 periods that it never ran. The skip passes the queued unit and n - 2
 * burst units, so the tick plays unit n - 2 and the queue stands where it
 * stood. A full ring of 32 sheds nothing either.
 */
static void case_jb_skip_ring(void)
{
	CASE("a skip over a burst of 16 datagrams pays it whole");
	tm_skip_ring_run(16);
	CASE("a skip over a burst of 17 datagrams pays it whole");
	tm_skip_ring_run(17);
	CASE("a skip over a burst of 24 datagrams pays it whole");
	tm_skip_ring_run(24);
	CASE("a skip over a burst of 31 datagrams pays it whole");
	tm_skip_ring_run(31);
	CASE("a skip over a full ring of 32 datagrams pays it whole");
	tm_skip_ring_run(32);
}

/*
 * The arrival cutoff of a skip. Two packets of a new source stand last in the
 * parked ring. Their stamps fall after the skip hook's own stamp, as for a
 * packet that the net thread publishes while the hook runs. The skip leaves
 * them and pays its five periods to the old timeline. The tick's ordinary
 * drain then takes the new source.
 */
static void case_jb_skip_cutoff(void)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	struct udsp_jb_stats before, after;
	uint32_t timeline, units;
	struct udsp_stream *st;
	struct tm_wire w;
	double phase = 0.0;
	unsigned want, head;
	struct eh t;

	CASE("packets stamped after a skip's own stamp wait for the tick");
	if (!tm_call_up(&t, "USA", &w, 0))
		return;
	st = tm_skip_up(&t, &w, &gate, 80, &phase);
	if (!st)
		goto out;
	CHECK(udsp_stream_jb_stats(t.s, 0, 0, &before) == UDSP_OK);
	timeline = udsp_jb_timeline(&st->jb);
	units = st->jb.skip_units;
	if (!tm_gate_ring_prepare(st, TM_SKIP_UNITS + 2u, &want))
		goto out;
	for (unsigned k = 0; k < TM_SKIP_UNITS; k++)
		tm_skip_send(&w, tm_skip_hz(k), false, &phase);
	tm_skip_source_new(&w);
	tm_skip_send(&w, tm_skip_hz(0), true, &phase);
	tm_skip_send(&w, tm_skip_hz(1), false, &phase);
	if (!tm_gate_ring_wait(st, want))
		goto out;
	head = atomic_load_explicit(&st->ring.head, memory_order_acquire);
	for (unsigned i = 1; i <= 2u; i++)
		st->ring.slot[(head - i) % UDSP_NET_SLOTS].arrival_ns =
			eh_mono_us() * 1000u + 1000000000u;
	atomic_store(&tm_be.skip_next, TM_SKIP_PERIODS);
	if (!eh_gate_step(&gate, 1))
		goto out;
	CHECK(udsp_stream_jb_stats(t.s, 0, 0, &after) == UDSP_OK);
	printf("# jb skip cutoff: skipped %u, timeline %u -> %u\n",
	       st->jb.skip_units - units, timeline,
	       udsp_jb_timeline(&st->jb));
	CHECK_UINT(st->jb.skip_units - units, TM_SKIP_PERIODS);
	CHECK_UINT(udsp_jb_timeline(&st->jb), timeline + 1u);
	CHECK_UINT(after.resyncs, before.resyncs + 1u);
out:
	eh_gate_disarm(&gate);
	tm_call_down(&t, &w);
	eh_gate_close(&gate);
}

/*
 * A skip past the decode bound on a 16 kHz line, where the PCMU stream goes
 * through the receive resampler. The run passes 14 units without a decode
 * and restarts the decoder and the resampler, and the last ten go through
 * the decode. The tick plays burst unit 23 at a unit's level, read in the
 * second half of its frame, and the queue and the counters stand.
 */
static void case_jb_skip_long(void)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	struct tm_skip_mark k;
	struct udsp_stream *st;
	struct tm_wire w;
	double phase = 0.0;
	double level;
	unsigned want;
	int unit;
	struct eh t;

	CASE("a skip past the decode bound restarts the decode on a resampled "
	     "line");
	if (!tm_call_up_rate(&t, "USA", UDSP_SAMPLE_RATE_WB, &w))
		return;
	st = tm_skip_up(&t, &w, &gate, 80, &phase);
	if (!st)
		goto out;
	CHECK(st->resample);
	tm_skip_mark_take(&t, st, &k);
	if (!tm_gate_ring_prepare(st, 25, &want))
		goto out;
	for (unsigned u = 0; u < 25u; u++)
		tm_skip_send(&w, tm_skip_hz(u % TM_SKIP_UNITS), false, &phase);
	if (!tm_gate_ring_wait(st, want))
		goto out;
	atomic_store(&tm_be.skip_next, 24u);
	eh_rec_mark();
	if (!eh_gate_step(&gate, 1))
		goto out;
	/* The second half of the frame: the resampler's delay carries the
	 * end of the unit before into the first. */
	unit = tm_skip_unit_rate(80, 80, UDSP_SAMPLE_RATE_WB, &level);
	printf("# jb skip long: unit %d at %.1f dBm0 (want %u)\n", unit, level,
	       23u % TM_SKIP_UNITS);
	CHECK(unit == (int)(23u % TM_SKIP_UNITS) && level > TM_SKIP_LEVEL_DBM0);
	(void)tm_skip_record(&gate, &t, st, &w, &k, 24u, &phase);
out:
	eh_gate_disarm(&gate);
	tm_call_down(&t, &w);
	eh_gate_close(&gate);
}

/*
 * A RECVONLY stream pays its skip like any other: the receive half runs
 * before the guard of the sending half.
 */
static void case_jb_skip_recvonly(void)
{
	const struct udsp_media m10 = {
		.codec = UDSP_SEL_PCMU, .codec_pt = 0, .ptime_ms = 10,
	};
	struct eh_gate gate = { .fd = { -1, -1 } };
	uint8_t buf[TM_PKT_MAX];
	struct tm_skip_mark k;
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	double phase = 0.0;
	unsigned sent = 0;
	double level;
	int unit;
	struct eh t;

	CASE("a receive-only stream pays its skip");
	if (!tm_call_up(&t, "USA", &w, 0))
		return;
	tm_media(&m, UDSP_SEL_PCMU, 0, 10);
	m.direction = UDSP_MEDIA_RECVONLY;
	tm_stream_config(&t, &w, &m);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL);
	if (!st || !eh_gate_park(&gate))
		goto out;
	for (unsigned i = 0; i < 80u; i++) {
		if (!tm_gate_tone_publish(st, &w, 1, &m10, 1000.0, 8000.0,
					  &phase) ||
		    !eh_gate_step(&gate, 1))
			goto out;
	}
	while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
		continue;
	tm_skip_mark_take(&t, st, &k);
	if (!tm_skip_burst(st, &w, TM_SKIP_UNITS, &phase) ||
	    !tm_skip_tick(&gate, &unit, &level))
		goto out;
	CHECK(unit == (int)TM_SKIP_PERIODS - 1 && level > TM_SKIP_LEVEL_DBM0);
	(void)tm_skip_record(&gate, &t, st, &w, &k, TM_SKIP_PERIODS, &phase);
	while (tm_recv(w.rtp_peer, buf, sizeof(buf)) > 0)
		sent++;
	CHECK_UINT(sent, 0);
out:
	eh_gate_disarm(&gate);
	tm_call_down(&t, &w);
	eh_gate_close(&gate);
}

/*
 * A G.722 skip past the decode bound. The run passes 14 units and restarts
 * the decoder, and the ten decoded units bring the ADPCM state back to the far
 * end's. The unit on the tick then matches a decoder that heard every packet.
 */
static void case_jb_skip_g722_long(void)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	int16_t ref[TM_G722_FRAME];
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	struct tm_g722 g = { 0 };
	double phase = 0.0;
	uint32_t playout, units;
	unsigned want;
	double corr;
	struct eh t;

	CASE("a G.722 skip past the decode bound brings the decoder back in "
	     "step");
	if (!tm_up_rate(&t, "USA", UDSP_SAMPLE_RATE_WB) ||
	    !tm_wire_open(&w, 0) || !tm_g722_open(&g)) {
		CHECK(false);
		tm_g722_close(&g);
		tm_down(&t);
		return;
	}
	tm_media(&m, UDSP_SEL_G722, 9, 10);
	tm_stream_config(&t, &w, &m);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL);
	if (!st || !eh_gate_park(&gate))
		goto out;
	for (unsigned i = 0; i < 80u; i++) {
		if (!tm_gate_ring_prepare(st, 1, &want))
			goto out;
		tm_g722_send_ref(&w, &g, 1000.0, &phase, ref);
		if (!tm_gate_ring_wait(st, want) || !eh_gate_step(&gate, 1))
			goto out;
	}
	playout = st->jb.playout_ts;
	units = st->jb.skip_units;
	if (!tm_gate_ring_prepare(st, 25, &want))
		goto out;
	for (unsigned k = 0; k < 25u; k++) {
		int16_t pcm[TM_G722_FRAME];

		tm_g722_send_ref(&w, &g, tm_skip_hz(k % TM_SKIP_UNITS) * 2.0,
				 &phase, k == 23u ? ref : pcm);
	}
	if (!tm_gate_ring_wait(st, want))
		goto out;
	atomic_store(&tm_be.skip_next, 24u);
	eh_rec_mark();
	if (!eh_gate_step(&gate, 1))
		goto out;
	corr = tm_rec_corr(0, ref, TM_G722_FRAME);
	printf("# jb skip g722 long: clock %u units of 80, skipped %u, "
	       "correlation %.6f\n", (st->jb.playout_ts - playout) / 80u,
	       st->jb.skip_units - units, corr);
	CHECK_UINT(st->jb.playout_ts - playout, 25u * 80u);
	CHECK_UINT(st->jb.skip_units - units, 24u);
	CHECK(corr > 0.99);
out:
	eh_gate_disarm(&gate);
	CHECK(udsp_stream_stop(t.s, 0, 0) == UDSP_OK);
	tm_g722_close(&g);
	tm_call_close(&t, &w);
	eh_gate_close(&gate);
}

/* --- G.729 Annex B across a skip ---------------------------------------- */

/* One 10 ms frame of the Annex B far end into a parked tick, the packet it
 * sends, if any, waited into the ring. */
static bool tm_dtx_gate_frame(struct udsp_stream *st, struct tm_dtx_tx *tx,
			      int fd, const int16_t *pcm)
{
	unsigned head = atomic_load_explicit(&st->ring.head,
					     memory_order_acquire);
	uint64_t end = eh_mono_us() + EH_GATE_WAIT_US;
	uint16_t seq = tx->seq;

	tm_dtx_tx_frame(tx, fd, pcm);
	if (tx->seq == seq)
		return true;
	while (atomic_load_explicit(&st->ring.head, memory_order_acquire) ==
	       head && eh_mono_us() < end)
		sched_yield();

	return atomic_load_explicit(&st->ring.head, memory_order_acquire) !=
	       head;
}

/* A low background the Annex B detector reads as noise, so its SID carries a
 * level the comfort noise plays back. */
static void tm_dtx_noise_frame(int16_t *pcm, uint32_t *seed)
{
	for (unsigned i = 0; i < 80u; i++) {
		*seed ^= *seed << 13;
		*seed ^= *seed >> 17;
		*seed ^= *seed << 5;
		pcm[i] = (int16_t)((int32_t)(*seed % 601u) - 300);
	}
}

/*
 * The comfort noise of G.729 Annex B across a skip past the decode bound.
 * First way in: the SID went to the decoder before the hold and the run
 * passes comfort noise alone, so the decoder keeps its comfort noise state
 * and the tick plays comfort noise at the level that played before. Second
 * way in: the talkspurt closes inside the hold, and the run passes its speech
 * and its SID. It restarts the decoder and decodes that SID first, so the
 * empty continuations after it play comfort noise, not concealment.
 */
static void tm_skip_sid_run(bool closes_inside)
{
	struct eh_gate gate = { .fd = { -1, -1 } };
	struct tm_dtx_tx tx = { 0 };
	struct udsp_jb_stats before, after;
	struct udsp_stream *st;
	struct udsp_media m;
	struct tm_wire w;
	uint32_t seed = 0x1234567u;
	double phase = 0.0;
	double e_cn = 0.0, e_tick;
	unsigned sids = 0;
	int16_t pcm[80];
	struct eh t;

	if (!tm_call_up(&t, "USA", &w, 1 << 20))
		return;
	tm_media(&m, UDSP_SEL_G729, 18, 20);
	m.dtx = true;
	m.jb_mode = UDSP_JB_FIXED;
	tm_stream_config(&t, &w, &m);
	CHECK(udsp_stream_start(t.s, 0, 0) == UDSP_OK);
	st = stream_at(t.s, 0, 0);
	CHECK(st != NULL);
	if (!st || !tm_dtx_tx_open(&tx, 2) || !eh_gate_park(&gate))
		goto out;
	for (unsigned f = 0; f < 60u; f++) {
		tm_dtx_tone_frame(pcm, &phase);
		if (!tm_dtx_gate_frame(st, &tx, w.rtp_peer, pcm) ||
		    !eh_gate_step(&gate, 1))
			goto out;
	}
	if (!closes_inside) {
		for (unsigned f = 0; f < 120u; f++) {
			tm_dtx_noise_frame(pcm, &seed);
			if (!tm_dtx_gate_frame(st, &tx, w.rtp_peer, pcm))
				goto out;
			if (f == 110u)
				eh_rec_mark();
			if (!eh_gate_step(&gate, 1))
				goto out;
		}
		e_cn = eh_energy(0, 9u * 80u);
	} else {
		/* The detector closes the talkspurt at noise frame 68 of this
		 * seed. The frames from 64 on reach a parked tick: speech, the
		 * SID that closes the talkspurt, then nothing. */
		for (unsigned f = 0; f < 80u; f++) {
			uint16_t seq = tx.seq;

			tm_dtx_noise_frame(pcm, &seed);
			if (!tm_dtx_gate_frame(st, &tx, w.rtp_peer, pcm))
				goto out;
			sids += f >= 64u && tx.seq != seq && tx.silent;
			if (f < 64u && !eh_gate_step(&gate, 1))
				goto out;
		}
	}
	CHECK(udsp_stream_jb_stats(t.s, 0, 0, &before) == UDSP_OK);
	atomic_store(&tm_be.skip_next, 25u);
	eh_rec_mark();
	if (!eh_gate_step(&gate, 1))
		goto out;
	e_tick = eh_energy(0, 80);
	CHECK(udsp_stream_jb_stats(t.s, 0, 0, &after) == UDSP_OK);
	printf("# jb skip sid %s: comfort noise %.0f before, %.0f on the tick, "
	       "%u sids sent in the hold, cn %llu -> %llu\n",
	       closes_inside ? "inside" : "before", e_cn, e_tick, sids,
	       (unsigned long long)before.cn, (unsigned long long)after.cn);
	CHECK(st->jb.cn_armed && st->jb.cn_sid);
	CHECK_UINT(after.cn, before.cn + 1u);
	if (closes_inside) {
		CHECK(sids >= 1u);
		CHECK(e_tick > 1000.0);
	} else {
		CHECK(e_cn > 1000.0);
		CHECK(e_tick > e_cn / 4.0 && e_tick < e_cn * 4.0);
	}
out:
	eh_gate_disarm(&gate);
	tm_dtx_tx_close(&tx);
	tm_call_down(&t, &w);
	eh_gate_close(&gate);
}

static void case_jb_skip_sid(void)
{
	CASE("a long skip inside comfort noise keeps the decoder's comfort "
	     "noise");
	tm_skip_sid_run(false);
	CASE("a long skip that passes a SID decodes it before the comfort "
	     "noise");
	tm_skip_sid_run(true);
}

/*
 * The decode that a skip runs, measured for the largest pool: 32 streams,
 * UDSP_LINE_MAX x UDSP_STREAM_MAX. Each stream decodes at most
 * STREAM_SKIP_DECODE_MAX units, 10, so a skip costs at most 320 unit decodes
 * on top of the buffer's own steps. The case prints the host figure for
 * G.711, G.722 and G.729. A target runs them slower, and the case asserts no
 * bound.
 */
static void case_jb_skip_cost(void)
{
	const struct udsp_codec_ops *ops[3] = {
		&udsp_codec_pcmu_ops,
		&udsp_codec_g722_ops,
		&udsp_codec_g729_ops,
	};
	struct udsp_codec_params p;
	unsigned bad = 0;

	CASE("the decode of a skip on 32 streams is measured");
	memset(&p, 0, sizeof(p));
	for (unsigned c = 0; c < 3u; c++) {
		struct udsp_codec *dec[32];
		uint8_t pay[160];
		int16_t pcm[UDSP_PCM_STAGE_MAX];
		size_t len = ops[c]->frame_bytes;
		uint64_t t0, t1;

		memset(pay, 0x55, sizeof(pay));
		for (unsigned i = 0; i < 32u; i++)
			dec[i] = ops[c]->create(&p);
		t0 = eh_mono_us();
		for (unsigned i = 0; i < 32u; i++) {
			for (unsigned u = 0; u < 10u; u++)
				bad += !dec[i] ||
				       ops[c]->decode(dec[i], pay, len, pcm,
						      UDSP_PCM_STAGE_MAX) <= 0;
		}
		t1 = eh_mono_us();
		printf("# jb skip cost %s: 320 unit decodes in %llu us\n",
		       ops[c]->name, (unsigned long long)(t1 - t0));
		for (unsigned i = 0; i < 32u; i++) {
			if (dec[i])
				ops[c]->destroy(dec[i]);
		}
	}
	CHECK_UINT(bad, 0);
}

int main(void)
{
	char bound[16];

	tm_child = mmap(NULL, sizeof(*tm_child), PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (tm_child == MAP_FAILED)
		return 1;
	/* The library reads this once, at the first open, so it is set before
	 * any case runs. It only ends a rendered event sooner, and no case
	 * outside case_telev renders one. */
	snprintf(bound, sizeof(bound), "%u", TM_TEV_TIMEOUT_MS);
	setenv("UDSP_TEV_PLAY_TIMEOUT_MS", bound, 1);
	/* A starved tick or net thread can miss 100 wall ms under sanitizers,
	 * so a sanitizer build keeps 1000 ms. A plain build takes 200 ms,
	 * because each lost-ack case waits the whole deadline. Both getters
	 * read the variable once per process through pthread_once, so it is
	 * set before any case runs. An explicit caller override still wins. */
	if (setenv("UDSP_SYNC_TICK_MS", TM_ACK_MS, 0) ||
	    setenv("UDSP_NET_ACK_MS", TM_ACK_MS, 0))
		return 1;

	/* The forking case runs while the process is still single threaded, so
	 * no other thread can hold a libc lock across the fork. */
	case_event_fd();
	case_pcm_offset();
	case_job_status();
	case_event_seq();
	case_gate();
	case_audio();
	case_ptime();
	case_reconfig();
	case_srtp();
	case_vad();
	case_telev();
	case_jb1_wire();
	case_jb_step_wire();
	case_cn();
	case_resample();
	case_tx_silence();
	case_rtcp();
	case_lec();
	case_lec_stop_stress();
	case_fax();
	case_fax_disable();
	case_fax_latch_clear();
	case_resample_detect();
	case_conf();
	case_fax_profile();
	case_t38();
	case_t38_loader();
	case_fds();
	case_quiesce();
	case_prep_lec();
	case_lec_release_raced_timeout();
	case_lec_release_drop_stress();
	case_flood();
	case_stats();
	case_jb_stats();
	case_jb_trace();
	case_dtx_media();
	case_jb_frame_bytes();
	case_vad_media();
	case_vad_close();
	case_dtx_tx();
	case_dtx_conceal();
	case_dtx_marker();
	case_dtx_wire();
	case_dtx_rx();
	case_alloc();
	/* After the allocation guard, so the case numbers before this point
	 * stay the same. */
	case_dtmf_mute();
	case_dtmf_relay();
#ifdef HAVE_OPUS
	case_opus();
#endif
	case_pkt_trace();
	case_stream_diag();
	tm_diag_send_case(true);
	tm_diag_send_case(false);
	case_stream_diag_srtp();
#ifdef HAVE_OPUS
	case_stream_diag_epochs();
#endif
	case_loop();
	case_lec_lead();
	case_cw_level();
	case_telev_level();
	case_first_packet();
	case_rephase_refill();
	case_rephase_command();
	case_jb_skip();
	case_jb_skip_source();
	case_jb_skip_event();
	case_jb_skip_g722();
	case_jb_skip_fixed();
	case_jb_skip_ring();
	case_jb_skip_cutoff();
	case_jb_skip_long();
	case_jb_skip_recvonly();
	case_jb_skip_g722_long();
	case_jb_skip_sid();
	case_jb_skip_cost();

	free(eh_be.rec);

	return tu_summary("media");
}
