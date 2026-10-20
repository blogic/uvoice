// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the udsp HAL backend for analog lines (src/hal/udsp/).
 *
 * Isolation. The real backend and the real libudsp sources are compiled into
 * this binary. A case therefore drives hal_backend_ops end to end. The op
 * goes through the backend and the control plane of the library to the fake
 * SLIC. The answer comes back as a status or as a record on the event
 * socketpair that the core owns.
 *
 * Nothing between the two contracts, hal.h and udsp.h, is a stub. This unit
 * is the translation between them, and a fake in the middle would test only
 * the fake.
 *
 * The suite injects a paced TDM backend, exactly as the test_core suite of
 * libudsp does. udsp_tdm_override() is the injection point for a test backend
 * (udsp_priv.h). The shipped file backend cannot serve these cases, because
 * it is unpaced. Its virtual clock runs three orders of magnitude faster than
 * wall time. The hook sampler then sees one level per thousand ticks, and a
 * case cannot express any hook, ring or CID timing.
 *
 * The backend below clocks one 10 ms period every millisecond. A case
 * therefore waits in the time base of the engine, at ten times wall speed.
 *
 * The configuration is a UCI fixture. hal_ops.open() takes only the event
 * fd, so the backend loads /etc/config/voip itself. UVOICE_UCI_CONFDIR points
 * that load at a temporary directory, the same aid that test_cfg uses.
 *
 * libudsp allows one live handle per process, so every case closes its probe
 * before the next case opens one.
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hal/hal.h"
#include "hal/udsp/udsp_int.h"
/* The log sink hands the library's lines to the relay of the call trace. */
#include "trace_ring.h"

/* the library private object, for udsp_tdm_override() and the fake instance
 * the hook injector needs; test_core reaches both the same way */
#include "core/udsp_priv.h"
#include "slic/fake.h"
#include "test/fax_fake.h"

#include "test_uloop.h"
#include "test_util.h"
#include "test_warn.h"

#define TS_PACE_US		1000u
#define TS_EV_MAX		256u

/* --- the paced tdm backend ------------------------------------------------ */

static struct {
	struct udsp_tdm_geom geom;
	_Atomic uint_least64_t reads;
	uint64_t deadline_us;
	unsigned int grant_rate;
	unsigned int asked_rate[2];
	unsigned int open_count;
	int token;
	/* The peak of a near-end triangle wave in every captured frame, 0 for
	 * silence. The canceller capture case sets it, so the microphone and
	 * the output planes of a replayed record carry a signal. */
	_Atomic int near_peak;
	unsigned int near_phase;
} ts_be;

static uint64_t mono_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* The engine time base: one period is 10 ms whatever the wall clock did. */
static uint32_t ts_now_ms(void)
{
	return (uint32_t)(atomic_load_explicit(&ts_be.reads, memory_order_acquire) *
			  10u);
}

static struct udsp_tdm *ts_open(const char *play, const char *cap,
				const struct udsp_tdm_geom *want,
				struct udsp_tdm_geom *got,
				struct udsp_tdm_fail *fail)
{
	if (ts_be.open_count < 2)
		ts_be.asked_rate[ts_be.open_count] = want->rate;
	ts_be.open_count++;
	*got = *want;
	if (ts_be.grant_rate)
		got->rate = ts_be.grant_rate;
	got->paced = true;
	ts_be.geom = *got;

	return (struct udsp_tdm *)&ts_be.token;
}

/* A triangle of 64 samples a period on every channel, so each line hears the
 * same near end. The engine thread alone reads and moves the phase. */
static void ts_near_fill(int16_t *buf)
{
	int peak = atomic_load_explicit(&ts_be.near_peak, memory_order_relaxed);
	unsigned int f, c;

	if (!peak)
		return;

	for (f = 0; f < ts_be.geom.frames_per_period; f++) {
		unsigned int step = ts_be.near_phase++ % 64u;
		int level = step < 32u ? (int)step - 16 : 48 - (int)step;
		int16_t sample = (int16_t)(level * peak / 16);

		for (c = 0; c < ts_be.geom.channels; c++)
			buf[f * ts_be.geom.channels + c] = sample;
	}
}

static void ts_close(struct udsp_tdm *t) { }
static int ts_recover(struct udsp_tdm *t) { return 0; }
static int ts_start(struct udsp_tdm *t) { return 0; }
static int ts_stop(struct udsp_tdm *t) { return 0; }

static int ts_read(struct udsp_tdm *t, int16_t *buf)
{
	uint64_t reads = atomic_load_explicit(&ts_be.reads, memory_order_relaxed);
	uint64_t now = mono_us();

	if (!ts_be.deadline_us)
		ts_be.deadline_us = now;
	ts_be.deadline_us += TS_PACE_US;
	if (ts_be.deadline_us > now) {
		struct timespec rel = {
			.tv_sec = 0,
			.tv_nsec = (long)(ts_be.deadline_us - now) * 1000L,
		};

		nanosleep(&rel, NULL);
	}

	memset(buf, 0, (size_t)ts_be.geom.frames_per_period *
	       ts_be.geom.channels * sizeof(*buf));
	ts_near_fill(buf);
	atomic_store_explicit(&ts_be.reads, reads + 1u, memory_order_release);

	return 0;
}

static int ts_write(struct udsp_tdm *t, const int16_t *buf)
{
	return 0;
}

static long ts_delay(struct udsp_tdm *t)
{
	return (long)(ts_be.geom.prefill_periods - 1) *
	       ts_be.geom.frames_per_period;
}

static const struct udsp_tdm_ops ts_ops = {
	.name = "ts",
	.open = ts_open,
	.close = ts_close,
	.recover = ts_recover,
	.start = ts_start,
	.stop = ts_stop,
	.read_period = ts_read,
	.write_period = ts_write,
	.delay = ts_delay,
};

/* --- the configuration fixture -------------------------------------------- */

/* A fixture path is short by construction; the two derived buffers are
 * wider than the directory so no name can be truncated. */
static char fixture_dir[128];
static char fixture_path[160];
static char fxsnum_path[160];

/* The base region is the one field a case rewrites, so it is a format slot:
 * the region must never reach the library config, and a region libudsp does
 * not ship must still attach. */
static const char fixture_fmt[] =
	"config base 'base'\n"
	"\toption enabled '1'\n"
	"\toption channels '2'\n"
	"\toption region '%s'\n"
	"config hal 'hal'\n"
	"\toption tdm_backend 'file'\n"
	"\toption tdm_playback '/dev/null'\n"
	"\toption tdm_capture '/dev/null'\n"
	"\toption slic_plugin '%s'\n"
	"\toption fax_plugin '%s'\n"
	"\toption spidev '/dev/null'\n"
	"\toption gpio_reset 'slic_reset'\n"
	"\toption gpio_interrupt 'none'\n"
	"\toption populated_ports '2'\n"
	"\toption channels '2'\n"
	"\toption prefill_periods '3'\n"
	"\toption engine_cpu '-1'\n"
	"\toption pcm_offset_bits '2'\n"
	"\toption sample_rate '%u'\n"
	/* 0 is the library's test mode: no SCHED_FIFO and no mlock inside the
	 * engine, which a host runner has no privilege for. */
	"\toption engine_prio '0'\n";

/* An iso3 name, so the bring-up chain of the backend can load it; the
 * alpha-2 form the schema defaults to has its own case below. */
#define FIXTURE_REGION_DEFAULT	"USA"

/* 'none' is the schema sentinel for "no plug-in", which selects the in-tree
 * fake; every other value is a path the library's loader has to open. */
static bool fixture_write_plugin_fax_rate(const char *region,
					  const char *plugin,
					  const char *fax_plugin,
					  unsigned int sample_rate)
{
	char text[1024];
	FILE *f;
	int len;

	len = snprintf(text, sizeof(text), fixture_fmt, region, plugin,
		       fax_plugin, sample_rate);
	if (len < 0 || (size_t)len >= sizeof(text))
		return false;

	f = fopen(fixture_path, "w");
	if (!f)
		return false;

	fputs(text, f);
	fclose(f);

	return true;
}

static bool fixture_write_plugin_rate(const char *region, const char *plugin,
				      unsigned int sample_rate)
{
	return fixture_write_plugin_fax_rate(region, plugin, "", sample_rate);
}

static bool fixture_write_plugin(const char *region, const char *plugin)
{
	return fixture_write_plugin_rate(region, plugin, 8000);
}

static bool fixture_write(const char *region)
{
	return fixture_write_plugin(region, "none");
}

static bool fixture_make(void)
{
	const char *tmp = getenv("TMPDIR");

	if (!tmp || !*tmp)
		tmp = "/tmp";

	snprintf(fixture_dir, sizeof(fixture_dir), "%s/uvoice-udsp-XXXXXX", tmp);
	if (!mkdtemp(fixture_dir))
		return false;

	snprintf(fixture_path, sizeof(fixture_path), "%s/voip", fixture_dir);
	if (!fixture_write(FIXTURE_REGION_DEFAULT))
		return false;

	setenv("UVOICE_UCI_CONFDIR", fixture_dir, 1);

	/* The backend publishes the line count for the platform scripts; a
	 * host runner owns no /var/run, so the write is aimed at the fixture. */
	snprintf(fxsnum_path, sizeof(fxsnum_path), "%s/voip.fxsnum", fixture_dir);
	hal_udsp_fxsnum_path = fxsnum_path;

	return true;
}

static void fixture_drop(void)
{
	unlink(fxsnum_path);
	unlink(fixture_path);
	rmdir(fixture_dir);
}

/* An empty region directory that UDSP_REGION_DIR names until the leave, so
 * a case can make a region file and its fallback absent together. The probe
 * and the load read the variable when the backend submits the load. */
static char region_dir[sizeof(fixture_dir) + 16];

static bool region_dir_enter(void)
{
	snprintf(region_dir, sizeof(region_dir), "%s/regions", fixture_dir);
	if (mkdir(region_dir, 0700))
		return false;

	setenv("UDSP_REGION_DIR", region_dir, 1);

	return true;
}

static void region_dir_leave(void)
{
	unsetenv("UDSP_REGION_DIR");
	rmdir(region_dir);
}

/* --- the probe ------------------------------------------------------------ */

struct probe {
	struct hal *h;
	int rx_fd;
	int tx_fd;
	struct hal_event ev[TS_EV_MAX];
	unsigned int ev_len;
};

static void probe_poll(struct probe *p)
{
	struct hal_event ev;

	while (recv(p->rx_fd, &ev, sizeof(ev), MSG_DONTWAIT) == (ssize_t)sizeof(ev)) {
		if (p->ev_len < TS_EV_MAX)
			p->ev[p->ev_len++] = ev;
	}
}

/*
 * The backend's event relay runs on the ULOOP thread, so nothing reaches the
 * core fd unless the loop turns. Every wait in this suite therefore runs uloop
 * for a bounded window instead of sleeping, which is how test_line drives the
 * line shell.
 */

/* Waits in engine milliseconds, so a case is written in the units the line
 * machines use; the wall clock ceiling only bounds a broken run. */
static void probe_wait(struct probe *p, uint32_t ms)
{
	uint32_t end = ts_now_ms() + ms;
	uint64_t stop = mono_us() + (uint64_t)ms * 1000u + 5000000u;

	while (ts_now_ms() < end && mono_us() < stop) {
		probe_poll(p);
		uloop_pump(1);
	}

	probe_poll(p);
}

/*
 * Turns the loop for a wall clock window. probe_wait() counts engine
 * milliseconds, which is what a case about a running line wants; a case about a
 * bring-up that failed cannot use it, because the engine of a board with no
 * line ready may never advance that clock again.
 */
static void probe_settle(struct probe *p, unsigned int ms)
{
	uint64_t stop = mono_us() + (uint64_t)ms * 1000u;

	while (mono_us() < stop) {
		probe_poll(p);
		uloop_pump(5);
	}

	probe_poll(p);
}

static const struct hal_event *ev_find(struct probe *p, unsigned int type)
{
	unsigned int i;

	for (i = 0; i < p->ev_len; i++) {
		if (p->ev[i].type == type)
			return &p->ev[i];
	}

	return NULL;
}

static unsigned int ev_count(struct probe *p, unsigned int type)
{
	unsigned int i, n = 0;

	for (i = 0; i < p->ev_len; i++) {
		if (p->ev[i].type == type)
			n++;
	}

	return n;
}

static const struct hal_event *ev_wait(struct probe *p, unsigned int type,
				       unsigned int ms)
{
	uint64_t stop = mono_us() + (uint64_t)ms * 1000u;
	const struct hal_event *ev;

	for (;;) {
		probe_poll(p);
		ev = ev_find(p, type);
		if (ev || mono_us() >= stop)
			return ev;

		uloop_pump(1);
	}
}

static void stream_dead_event_check(struct probe *p, unsigned int line,
				    unsigned int stream)
{
	const struct hal_event *ev = ev_find(p, HAL_EVENT_SLIC_FAULT);

	CHECK(ev != NULL);
	if (!ev)
		return;

	CHECK_UINT(ev->line, line);
	CHECK_UINT(ev->stream, stream);
	CHECK_UINT(ev->u.fault.code, HAL_FAULT_STREAM_DEAD);
	CHECK_UINT(ev->seq, 0);
}

static bool job_wait(struct probe *p, uint32_t token, unsigned int ms,
		     int32_t *status, uint16_t *seq)
{
	uint64_t stop = mono_us() + (uint64_t)ms * 1000u;
	unsigned int i;

	while (mono_us() < stop) {
		probe_poll(p);
		for (i = 0; i < p->ev_len; i++) {
			if (p->ev[i].type != HAL_EVENT_JOB_DONE ||
			    p->ev[i].u.job.token != token)
				continue;

			if (status)
				*status = p->ev[i].u.job.status;
			if (seq)
				*seq = p->ev[i].seq;

			return true;
		}
		uloop_pump(1);
	}

	return false;
}

/* slic_opts selects which nullable plug-in ops the fake exposes, which is how
 * a case reaches the capability gating: a cleared bit is a SLIC that does not
 * carry that op at all (slic.h). channels is what the part reports at attach,
 * which the fixture's populated_ports then bounds. */
static bool probe_open_opts(struct probe *p, uint32_t slic_opts,
			    unsigned int channels)
{
	int fd[2];

	memset(p, 0, sizeof(*p));
	p->rx_fd = p->tx_fd = -1;

	atomic_store(&ts_be.reads, 0u);
	atomic_store(&ts_be.near_peak, 0);
	ts_be.near_phase = 0;
	ts_be.deadline_us = 0;
	memset(ts_be.asked_rate, 0, sizeof(ts_be.asked_rate));
	ts_be.open_count = 0;
	udsp_slic_fake_reset();
	udsp_slic_fake_channels_set(channels);
	udsp_slic_fake_optional_set(slic_opts);
	udsp_tdm_override(&ts_ops);

	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fd))
		return false;

	p->rx_fd = fd[0];
	p->tx_fd = fd[1];
	p->h = hal_backend_ops.open(p->tx_fd);

	return p->h != NULL;
}

static bool probe_open(struct probe *p)
{
	return probe_open_opts(p, UDSP_SLIC_FAKE_OPT_ALL, 2);
}

/*
 * The core FD filled from the test side. The probe owns both ends, so a case
 * that has to see what the backend does with a full fd writes filler records
 * into the same write end until the socket refuses one; nothing drains the
 * read end until the case says so.
 */
static unsigned int probe_fill(struct probe *p)
{
	struct hal_event pad;
	unsigned int n = 0;

	memset(&pad, 0, sizeof(pad));
	pad.type = HAL_EVENT_NONE;

	while (n < 100000u &&
	       send(p->tx_fd, &pad, sizeof(pad), MSG_DONTWAIT | MSG_NOSIGNAL) ==
	       (ssize_t)sizeof(pad))
		n++;

	return n;
}

/* Empties the core fd without keeping anything: the filler above must not
 * take the room the recorded events need. */
static void probe_discard(struct probe *p)
{
	struct hal_event ev;

	while (recv(p->rx_fd, &ev, sizeof(ev), MSG_DONTWAIT) == (ssize_t)sizeof(ev))
		continue;

	p->ev_len = 0;
}

/* Frees exactly n records of room on the core fd, which is how a case decides
 * how much room the backend is about to compete for. */
static unsigned int probe_take(struct probe *p, unsigned int n)
{
	struct hal_event ev;
	unsigned int i;

	for (i = 0; i < n; i++) {
		if (recv(p->rx_fd, &ev, sizeof(ev), MSG_DONTWAIT) !=
		    (ssize_t)sizeof(ev))
			break;
	}

	return i;
}

/* Drains the core fd and keeps everything except the filler, so a thousand pad
 * records cannot push the records under test out of the store. */
static void probe_collect(struct probe *p)
{
	struct hal_event ev;

	while (recv(p->rx_fd, &ev, sizeof(ev), MSG_DONTWAIT) == (ssize_t)sizeof(ev)) {
		if (ev.type == HAL_EVENT_NONE)
			continue;
		if (p->ev_len < TS_EV_MAX)
			p->ev[p->ev_len++] = ev;
	}
}

/*
 * A hardware record delivered straight into the relay's read end, which is the
 * end libudsp writes. The priority cases are about who takes the room on the
 * core fd, so the record has to arrive exactly when the case says; an engine
 * event arrives on the library's own schedule instead. The shape is what the
 * emitter writes: a hardware type and a seq from 1.
 */
static bool hw_inject(struct probe *p, uint16_t seq)
{
	struct hal_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = HAL_EVENT_HOOK;
	ev.line = 1;
	ev.seq = seq;
	ev.u.hook.state = HAL_HOOK_FLASH;

	return send(p->h->lib_fd, &ev, sizeof(ev), MSG_DONTWAIT | MSG_NOSIGNAL) ==
	       (ssize_t)sizeof(ev);
}

static int hw_index(struct probe *p, uint16_t seq)
{
	unsigned int i;

	for (i = 0; i < p->ev_len; i++) {
		if (p->ev[i].type == HAL_EVENT_HOOK && p->ev[i].seq == seq)
			return (int)i;
	}

	return -1;
}

/* The watcher a case parks on the core fd to take the registration the backend
 * wants. It answers nothing; holding the fd is the whole job. */
static void pump_nothing(struct uloop_fd *ufd, unsigned int events)
{
}

/*
 * Takes the backend's write watcher away without telling the backend. A
 * priority case needs the queue to move through the path under test and not
 * through a watcher that happens to be ready in the same loop turn; this is
 * white box, on the same footing as reading comp_len.
 */
static void probe_watch_drop(struct probe *p)
{
	if (p->h->core_out.registered)
		uloop_fd_delete(&p->h->core_out);
}

/* Turns the loop, without reading the core fd, until the backend holds want
 * completions it could not pass on. */
static void queue_wait(struct probe *p, unsigned int want, unsigned int ms)
{
	uint64_t stop = mono_us() + (uint64_t)ms * 1000u;

	while (p->h->comp_len < want && mono_us() < stop)
		uloop_pump(5);
}

/* The other direction: turns the loop until the backend has passed everything
 * on, or the window ends. */
static void queue_empty_wait(struct probe *p, unsigned int ms)
{
	uint64_t stop = mono_us() + (uint64_t)ms * 1000u;

	while (p->h->comp_len && mono_us() < stop)
		uloop_pump(5);
}

/*
 * Turns the loop, without reading the core fd, until the backend reports a
 * completion it could not hold. The oracle is the published counter and not
 * the private field, because that counter is what the ubus surface carries;
 * the wait is bounded so a fold back into events_dropped ends the window
 * instead of hanging the suite.
 */
static void lost_wait(struct probe *p, uint64_t want, unsigned int ms)
{
	uint64_t stop = mono_us() + (uint64_t)ms * 1000u;
	struct hal_counters c;

	while (mono_us() < stop) {
		hal_backend_ops.counters_get(p->h, &c);
		if (c.completions_lost >= want)
			return;

		uloop_pump(5);
	}
}

static void probe_close(struct probe *p)
{
	if (p->h)
		hal_backend_ops.close(p->h);
	p->h = NULL;

	if (p->rx_fd >= 0)
		close(p->rx_fd);
	if (p->tx_fd >= 0)
		close(p->tx_fd);
	p->rx_fd = p->tx_fd = -1;
	udsp_tdm_override(NULL);
}

static bool probe_start(struct probe *p)
{
	struct hal_job job = { 0 };
	int32_t status = -1;

	if (hal_backend_ops.dsp_start(p->h, &job) != HAL_OK)
		return false;

	return job_wait(p, job.token, 5000, &status, NULL) && status == HAL_OK;
}

/* An open that failed leaves no handle, and hal.h lets a backend assume the
 * core never calls an op without one (main.c guards on uv.hal), so the guard
 * belongs here: a case that reports a failed bring-up must not crash the
 * suite on the way out. */
static void probe_stop(struct probe *p)
{
	struct hal_job job = { 0 };

	if (!p->h)
		return;

	if (hal_backend_ops.dsp_stop(p->h, &job) == HAL_OK)
		job_wait(p, job.token, 5000, NULL, NULL);
}

static bool probe_region(struct probe *p, const char *iso3)
{
	struct hal_job job = { 0 };
	int32_t status = -1;

	if (hal_backend_ops.region_apply(p->h, iso3, &job) != HAL_OK)
		return false;

	return job_wait(p, job.token, 5000, &status, NULL) && status == HAL_OK;
}

/*
 * The real bring-up order. Nothing names a region until region_apply
 * runs, so a tone or a caller identity before it answers unsupported: the
 * library needs a tone plan to render either. main.c submits dsp_start and
 * line/line.c submits the region from lines_start on its completion, which is
 * the sequence this helper reproduces.
 */
static bool probe_up(struct probe *p)
{
	return probe_open(p) && probe_start(p) && probe_region(p, "USA");
}

static void probe_down(struct probe *p)
{
	probe_stop(p);
	probe_close(p);
}

/* --- the fake slic oracles ------------------------------------------------- */

/*
 * Quiescence (fake.h). The journal and the per-channel record belong to the
 * slic thread while it samples, so every read below happens after the dsp_stop
 * completion has arrived: from that point nothing else touches either.
 *
 * The journal keeps its first UDSP_SLIC_FAKE_JOURNAL_MAX rows and drops the
 * rest (fake.c journal_push counts them in *lost), and the hook sampler
 * writes one row per tick, so a row only survives when the action that made
 * it comes early in the probe's life. Every case that reads a row therefore
 * acts right after its probe_up and stops; the per-channel counters survive
 * any distance and carry the rest.
 */
static struct udsp_slic_fake_entry fake_jr[UDSP_SLIC_FAKE_JOURNAL_MAX];
static unsigned int fake_jr_len;
static struct udsp_slic_fake_chan_state fake_st[2];

static void fake_take(struct probe *p)
{
	unsigned int lost = 0;
	unsigned int i;

	fake_jr_len = udsp_slic_fake_journal_get(p->h->s->slic, fake_jr,
						 UDSP_SLIC_FAKE_JOURNAL_MAX,
						 &lost);
	if (fake_jr_len > UDSP_SLIC_FAKE_JOURNAL_MAX)
		fake_jr_len = UDSP_SLIC_FAKE_JOURNAL_MAX;

	for (i = 0; i < ARRAY_SIZE(fake_st); i++) {
		memset(&fake_st[i], 0, sizeof(fake_st[i]));
		udsp_slic_fake_chan_state_get(p->h->s->slic, i, &fake_st[i]);
	}
}

/* arg0 < 0 matches any argument. */
static int fake_jr_find(unsigned int call, unsigned int chan, int32_t arg0)
{
	unsigned int i;

	for (i = 0; i < fake_jr_len; i++) {
		if (fake_jr[i].call != call || fake_jr[i].chan != chan)
			continue;
		if (arg0 >= 0 && fake_jr[i].arg0 != arg0)
			continue;

		return (int)i;
	}

	return -1;
}

/* --- the media wire ------------------------------------------------ */

/*
 * The far end is a real bound UDP pair on loopback, and not the socketpair
 * libudsp's own test_media.c hands straight to udsp_stream_config. This unit
 * is the bind, the mark and the connect, so a wire that skipped them would
 * test nothing that belongs to it: a case here names a local port, the backend
 * binds it, and the far end below is what proves a packet reached the wire.
 *
 * Every packet is built and parsed byte by byte here, so the oracle shares no
 * code with rtp/rtp.c, which is the rule libudsp's own media suite follows.
 */
#define WIRE_PKT_MAX		1500u
/* u-law silence, so an injected frame is a legal G.711 payload. */
#define WIRE_ULAW_SILENCE	0xffu
/* RFC 3551: PCMU is payload type 0 and 8 kHz, so one millisecond is 8 samples
 * and one G.711 sample is one octet. */
#define WIRE_SAMPLES_PER_MS	8u
/* RFC 3550 5.1: the fixed header with no CSRC and no extension. */
#define WIRE_RTP_HDR		12u
/* RFC 4568 6.2: the _80 suite appends a 10 byte SRTP authentication tag. */
#define WIRE_SRTP_80_TAG	10u

/*
 * RFC 3550 a.1 source validation: a new SSRC stays in probation until this
 * many packets have arrived with consecutive sequence numbers, and nothing is
 * reported to the core until it leaves. A case that sent one packet would be
 * asserting the probation rule, not the receive path.
 */
#define WIRE_PROBATION_PKTS	4u

/*
 * A second IPv4 loopback address. 127.0.0.0/8 is all local, so this is an
 * address the media endpoint is not, which is what tells an RTCP address the
 * backend honoured from one it ignored: the port alone cannot, because
 * rtcp_port_of() already carries a named RTCP port to either address.
 */
#define WIRE_ALT_ADDR		"127.0.0.2"

struct wire {
	/* AF_INET or AF_INET6. Both endpoints and every description a case
	 * builds from this wire carry that family. */
	int family;
	int rtp;			/* the far end's own two sockets */
	int rtcp;
	/*
	 * The local port pair stays reserved until the case hands it to the
	 * backend. Probing it and releasing it at once would let a second wire
	 * probe the very same number back, and the two would then look like one
	 * local endpoint to every addr_same() decision in the backend.
	 */
	int hold_rtp;
	int hold_rtcp;
	uint16_t port;			/* what the far end bound, rtcp is +1 */
	uint16_t local;			/* what the backend is told to bind */
	uint16_t seq;
	uint32_t ts;
	uint32_t ssrc;
};

union wire_sa {
	struct sockaddr sa;
	struct sockaddr_in v4;
	struct sockaddr_in6 v6;
};

/* The loopback address of the family and the port, ready for a bind or a
 * sendto. Answers the length that address takes. */
static socklen_t wire_sa_fill(union wire_sa *sa, int family, uint16_t port)
{
	memset(sa, 0, sizeof(*sa));

	if (family == AF_INET6) {
		sa->v6.sin6_family = AF_INET6;
		sa->v6.sin6_port = htons(port);
		sa->v6.sin6_addr = in6addr_loopback;

		return (socklen_t)sizeof(sa->v6);
	}

	sa->v4.sin_family = AF_INET;
	sa->v4.sin_port = htons(port);
	sa->v4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	return (socklen_t)sizeof(sa->v4);
}

/* The bound port of either family, read off the socket itself, so a caller
 * that holds a descriptor needs no family of its own. */
static uint16_t sock_port(int fd)
{
	union wire_sa sa;
	socklen_t len = sizeof(sa);

	if (getsockname(fd, &sa.sa, &len))
		return 0;

	if (sa.sa.sa_family == AF_INET6)
		return ntohs(sa.v6.sin6_port);

	return ntohs(sa.v4.sin_port);
}

static int udp_bind(int family, uint16_t port)
{
	union wire_sa sa;
	socklen_t len = wire_sa_fill(&sa, family, port);
	int fd = socket(family, SOCK_DGRAM | SOCK_NONBLOCK, 0);

	if (fd < 0)
		return -1;

	if (!bind(fd, &sa.sa, len))
		return fd;

	close(fd);

	return -1;
}

/* A far end on WIRE_ALT_ADDR, which is the second loopback address the RTCP
 * cases send to. */
static int udp_bind_alt(uint16_t port)
{
	struct sockaddr_in sa;
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);

	if (fd < 0)
		return -1;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	if (inet_pton(AF_INET, WIRE_ALT_ADDR, &sa.sin_addr) == 1 &&
	    !bind(fd, (struct sockaddr *)&sa, sizeof(sa)))
		return fd;

	close(fd);

	return -1;
}

/* A consecutive pair, which is what an RTP and RTCP port pair is: the kernel
 * picks the first and the second is asked for by number, so a port whose
 * successor is taken is dropped and the search runs again. */
static bool port_pair_take(int family, int *rtp, int *rtcp)
{
	unsigned int attempt;

	*rtp = *rtcp = -1;
	for (attempt = 0; attempt < 64u; attempt++) {
		*rtp = udp_bind(family, 0);
		if (*rtp < 0)
			return false;

		*rtcp = udp_bind(family, (uint16_t)(sock_port(*rtp) + 1u));
		if (*rtcp >= 0)
			return true;

		close(*rtp);
		*rtp = -1;
	}

	return false;
}

/*
 * The local port is reserved and held. The backend binds it itself, so the
 * case has to let go of it before the first stream_config_set; until then the
 * reservation is what stops a second wire from probing the same number.
 */
static bool wire_open_af(struct wire *w, int family)
{
	memset(w, 0, sizeof(*w));
	w->family = family;
	w->rtp = w->rtcp = w->hold_rtp = w->hold_rtcp = -1;
	w->seq = 1000;
	w->ts = 160000;
	w->ssrc = 0x51501234u;

	if (!port_pair_take(family, &w->rtp, &w->rtcp))
		return false;

	w->port = sock_port(w->rtp);
	if (!port_pair_take(family, &w->hold_rtp, &w->hold_rtcp))
		return false;

	w->local = sock_port(w->hold_rtp);

	return w->port != 0 && w->local != 0 && w->port != w->local;
}

static bool wire_open(struct wire *w)
{
	return wire_open_af(w, AF_INET);
}

/* The same wire over ::1. A caller runs it only where tu_v6_loopback() says
 * the host can bind that address. */
static bool wire_open6(struct wire *w)
{
	return wire_open_af(w, AF_INET6);
}

/* Hands the local pair back to the kernel so the backend can bind it. */
static void wire_local_release(struct wire *w)
{
	if (w->hold_rtp >= 0)
		close(w->hold_rtp);
	if (w->hold_rtcp >= 0)
		close(w->hold_rtcp);
	w->hold_rtp = w->hold_rtcp = -1;
}

static void wire_close(struct wire *w)
{
	wire_local_release(w);
	if (w->rtp >= 0)
		close(w->rtp);
	if (w->rtcp >= 0)
		close(w->rtcp);
	w->rtp = w->rtcp = -1;
}

/* The loopback of the family, in the network byte order hal_addr carries:
 * 127.0.0.1 in the first four bytes, or ::1 in the sixteenth. */
static void addr_loopback_set(struct hal_addr *a, int family)
{
	if (family == AF_INET6) {
		a->ipver = 6;
		a->addr[15] = 1;
		return;
	}

	a->ipver = 4;
	a->addr[0] = 127;
	a->addr[3] = 1;
}

static void media_build(struct hal_media *m, const struct wire *w, uint8_t codec,
			uint8_t pt, uint8_t ptime_ms)
{
	memset(m, 0, sizeof(*m));
	addr_loopback_set(&m->local, w->family);
	m->local.port = w->local;
	addr_loopback_set(&m->remote, w->family);
	m->remote.port = w->port;
	m->codec = codec;
	m->codec_pt = pt;
	m->direction = HAL_MEDIA_SENDRECV;
	m->ptime_ms = ptime_ms;
	m->jb_mode = HAL_JB_ADAPTIVE;
	m->rtcp_enable = true;
	/* The EF code point the shipped rtp_tos 184 carries: the core converts
	 * the uci TOS byte with tos >> 2 before it fills this field. */
	m->rtp_dscp = 46;
	m->rtcp_dscp = 46;
	m->rtcp_interval_ms = 5000;
}

/*
 * What the far end saw. The per payload type counter is the point: a digit
 * burst is a handful of RFC 4733 packets between the audio packets on either
 * side of it, so an oracle that read the last packet would read the audio that
 * resumed after the digit ended.
 */
struct wire_seen {
	unsigned int packets;
	unsigned int of_pt[128];
	uint8_t last_pt;
	size_t last_len;
};

static void wire_drain(struct wire *w, struct wire_seen *seen)
{
	uint8_t pkt[WIRE_PKT_MAX];
	struct wire_seen local;
	ssize_t len;

	if (!seen)
		seen = &local;

	memset(seen, 0, sizeof(*seen));
	while ((len = recv(w->rtp, pkt, sizeof(pkt), MSG_DONTWAIT)) >= 0) {
		/* An empty datagram is the punch that holds the tuple open;
		 * a receiver discards it and keeps reading. */
		if (len == 0)
			continue;
		if (len < (ssize_t)WIRE_RTP_HDR)
			break;
		seen->packets++;
		seen->last_pt = pkt[1] & 0x7fu;
		seen->last_len = (size_t)len;
		seen->of_pt[seen->last_pt]++;
	}
}

/* The first datagram with bytes on fd, or -1 when none is queued: the empty
 * punches ahead of it are read past, as a receiver does. */
static ssize_t wire_recv_bytes(int fd, uint8_t *pkt, size_t cap)
{
	for (;;) {
		ssize_t len = recv(fd, pkt, cap, MSG_DONTWAIT);

		if (len != 0)
			return len;
	}
}

/* One G.711 packet from the far end with the caller's own sequence number and
 * timestamp, so a case can hold a packet back and deliver it out of order. */
static void wire_send_at(struct wire *w, uint8_t pt, unsigned int ptime_ms,
			 uint16_t seq, uint32_t ts)
{
	uint8_t pkt[WIRE_RTP_HDR + 240u];
	unsigned int n = ptime_ms * WIRE_SAMPLES_PER_MS;
	union wire_sa sa;
	socklen_t len;

	if (n > 240u)
		n = 240u;

	memset(pkt, 0, sizeof(pkt));
	pkt[0] = 0x80u;
	pkt[1] = pt;
	pkt[2] = (uint8_t)(seq >> 8);
	pkt[3] = (uint8_t)seq;
	pkt[4] = (uint8_t)(ts >> 24);
	pkt[5] = (uint8_t)(ts >> 16);
	pkt[6] = (uint8_t)(ts >> 8);
	pkt[7] = (uint8_t)ts;
	pkt[8] = (uint8_t)(w->ssrc >> 24);
	pkt[9] = (uint8_t)(w->ssrc >> 16);
	pkt[10] = (uint8_t)(w->ssrc >> 8);
	pkt[11] = (uint8_t)w->ssrc;
	memset(pkt + WIRE_RTP_HDR, WIRE_ULAW_SILENCE, n);

	len = wire_sa_fill(&sa, w->family, w->local);
	if (sendto(w->rtp, pkt, WIRE_RTP_HDR + n, MSG_DONTWAIT | MSG_NOSIGNAL,
		   &sa.sa, len) < 0)
		return;
}

/* One G.711 packet from the far end toward the port the backend bound. The
 * sequence number advances by one per call, which is what carries a source
 * through the RFC 3550 probation. */
static void wire_send(struct wire *w, uint8_t pt, unsigned int ptime_ms)
{
	unsigned int n = ptime_ms * WIRE_SAMPLES_PER_MS;

	if (n > 240u)
		n = 240u;

	wire_send_at(w, pt, ptime_ms, w->seq, w->ts);
	w->seq++;
	w->ts += n;
}

/*
 * One RFC 4733 telephone-event packet from the far end. The marker bit marks
 * the first packet of an event and the E bit its last (RFC 4733 section 2.3).
 * Every packet of one event carries the timestamp of its start, so this
 * advances the sequence number alone.
 */
static void wire_send_telev(struct wire *w, uint8_t pt, uint8_t event,
			    bool start, bool end, uint16_t duration)
{
	uint8_t pkt[WIRE_RTP_HDR + 4u];
	union wire_sa sa;
	socklen_t len;

	memset(pkt, 0, sizeof(pkt));
	pkt[0] = 0x80u;
	pkt[1] = start ? (uint8_t)(pt | 0x80u) : pt;
	pkt[2] = (uint8_t)(w->seq >> 8);
	pkt[3] = (uint8_t)w->seq;
	pkt[4] = (uint8_t)(w->ts >> 24);
	pkt[5] = (uint8_t)(w->ts >> 16);
	pkt[6] = (uint8_t)(w->ts >> 8);
	pkt[7] = (uint8_t)w->ts;
	pkt[8] = (uint8_t)(w->ssrc >> 24);
	pkt[9] = (uint8_t)(w->ssrc >> 16);
	pkt[10] = (uint8_t)(w->ssrc >> 8);
	pkt[11] = (uint8_t)w->ssrc;
	pkt[12] = event;
	/* the volume byte, 10 dBm0 below full scale, with the end bit above it */
	pkt[13] = (uint8_t)((end ? 0x80u : 0u) | 10u);
	pkt[14] = (uint8_t)(duration >> 8);
	pkt[15] = (uint8_t)duration;

	len = wire_sa_fill(&sa, w->family, w->local);
	if (sendto(w->rtp, pkt, sizeof(pkt), MSG_DONTWAIT | MSG_NOSIGNAL,
		   &sa.sa, len) < 0)
		return;

	w->seq++;
}

/*
 * The two halves of "is the descriptor still this backend's".
 *
 * fd_live answers the number. It is white box, on the same footing as reading
 * comp_len, and it is not enough on its own: a closed number comes back, so a
 * live answer after a close could name somebody else's socket.
 *
 * port_free answers the resource, and it is the one that cannot be fooled. A
 * media port nothing holds any more binds again; a leak on either side, this
 * backend's original or libudsp's dup, keeps it and fails this.
 */
static bool fd_live(int fd)
{
	return fd >= 0 && fcntl(fd, F_GETFD) >= 0;
}

static bool port_free(int family, uint16_t port)
{
	int fd = udp_bind(family, port);

	if (fd < 0)
		return false;

	close(fd);

	return true;
}

/*
 * How many descriptors this process holds. It is the oracle for a refusal that
 * must make no socket in a family the case holds no port of: an AF_INET6 pair
 * binds nothing port_free() can ask about, so the count is what says the pair
 * was never made. The directory handle itself is counted on both reads, so the
 * two numbers compare.
 */
static unsigned int fd_count(void)
{
	struct dirent *e;
	unsigned int n = 0;
	DIR *d;

	d = opendir("/proc/self/fd");
	if (!d)
		return 0;

	while ((e = readdir(d)) != NULL)
		if (e->d_name[0] != '.')
			n++;

	closedir(d);

	return n;
}

static int tos_get(int fd, int family)
{
	int level = family == AF_INET6 ? IPPROTO_IPV6 : IPPROTO_IP;
	int opt = family == AF_INET6 ? IPV6_TCLASS : IP_TOS;
	socklen_t len = sizeof(int);
	int tos = -1;

	if (getsockopt(fd, level, opt, &tos, &len))
		return -1;

	return tos;
}

/* 1 when the socket takes no IPv4 mapped address, 0 when it does, -1 when the
 * option cannot be read at all. */
static int v6only_get(int fd)
{
	socklen_t len = sizeof(int);
	int on = -1;

	if (getsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, &len))
		return -1;

	return on;
}

/* The far end a connected socket names. It is the oracle for a connect,
 * because the address is what a description carries and the descriptor number
 * says nothing about it. */
static bool peer_is(int fd, const char *addr, uint16_t port)
{
	char text[INET6_ADDRSTRLEN];
	socklen_t len = sizeof(union wire_sa);
	union wire_sa sa;

	if (getpeername(fd, &sa.sa, &len))
		return false;

	if (sa.sa.sa_family == AF_INET6) {
		if (!inet_ntop(AF_INET6, &sa.v6.sin6_addr, text, sizeof(text)))
			return false;

		return ntohs(sa.v6.sin6_port) == port && !strcmp(text, addr);
	}

	if (!inet_ntop(AF_INET, &sa.v4.sin_addr, text, sizeof(text)))
		return false;

	return ntohs(sa.v4.sin_port) == port && !strcmp(text, addr);
}

/*
 * Answers whether the open case can run. A host may have no IPv6 loopback. The
 * caller passes the reason the probe gave, and this skips the case with it.
 * The group below names every case either way, so the case count holds. Under
 * a network namespace with lo up the host can bind ::1, and no case skips.
 */
static bool v6_run(const char *why)
{
	if (!why)
		return true;

	tu_skip(why);

	return false;
}

/* The stream record of the backend, white box for the same reason the queue
 * cases read comp_len: the descriptor ownership has no other observer. */
static struct hal_udsp_stream *stream_rec(struct probe *p, unsigned int line,
					  unsigned int stream)
{
	return &p->h->stream[line][stream];
}

/* Brings one stream up on the wire and leaves it running. The local pair is
 * released here, because this is the moment the backend asks for it. */
static bool media_up(struct probe *p, struct wire *w, const struct hal_media *m)
{
	wire_local_release(w);

	return hal_backend_ops.stream_config_set(p->h, 0, 0, m) == HAL_OK &&
	       hal_backend_ops.stream_start(p->h, 0, 0) == HAL_OK;
}

/* Enough consecutive packets to carry the far end out of probation. */
static void wire_talk(struct probe *p, struct wire *w, uint8_t pt,
		      unsigned int ptime_ms)
{
	unsigned int i;

	for (i = 0; i < WIRE_PROBATION_PKTS; i++) {
		wire_send(w, pt, ptime_ms);
		probe_wait(p, ptime_ms);
	}
}

/* --- the pure translation cases ------------------------------------------- */

static void test_status_map(void)
{
	CASE("the status map carries all four values");
	CHECK(hal_udsp_status_map(UDSP_OK) == HAL_OK);
	CHECK(hal_udsp_status_map(UDSP_TRANSIENT) == HAL_TRANSIENT);
	CHECK(hal_udsp_status_map(UDSP_UNSUPPORTED) == HAL_UNSUPPORTED);
	CHECK(hal_udsp_status_map(UDSP_FATAL) == HAL_FATAL);

	CASE("an unknown library status maps to fatal");
	CHECK(hal_udsp_status_map((enum udsp_status)99) == HAL_FATAL);
}

/*
 * The capability is still a subset, and this group checks it. A library that
 * reports all features and codecs must reach the core as exactly the set
 * that a hal op can honor end to end. Every assert below is
 * an exact mask, so a bit that appears without an op behind it fails here.
 */
static void test_cap_fill(void)
{
	struct udsp_cap src;
	struct hal_cap dst;

	memset(&src, 0, sizeof(src));
	snprintf(src.backend, sizeof(src.backend), "proslic");
	snprintf(src.version, sizeof(src.version), "libudsp 0.1.0 proslic");
	snprintf(src.slic, sizeof(src.slic), "si32282");
	src.lines = 2;
	src.streams_per_chan = 2;
	src.features = UDSP_FEAT_G729 | UDSP_FEAT_G726 | UDSP_FEAT_T38 |
		       UDSP_FEAT_LINE_TEST | UDSP_FEAT_POLARITY |
		       UDSP_FEAT_MWI_LAMP | UDSP_FEAT_CONF | UDSP_FEAT_SRTP |
		       UDSP_FEAT_WIDEBAND | UDSP_FEAT_DTMF_RELAY |
		       UDSP_FEAT_DTMF_PLAYOUT;
	src.codecs = (1u << UDSP_SEL_PCMU) | (1u << UDSP_SEL_PCMA) |
		     (1u << UDSP_SEL_G729) | (1u << UDSP_SEL_G726_16) |
		     (1u << UDSP_SEL_G726_24) | (1u << UDSP_SEL_G726_32) |
		     (1u << UDSP_SEL_G726_40) | (1u << UDSP_SEL_T38) |
		     (1u << UDSP_SEL_G722) | (1u << UDSP_SEL_OPUS);

	hal_udsp_cap_fill(&dst, &src);

	CASE("the capability names this backend, not the slic plug-in");
	CHECK(!strcmp(dst.backend, "udsp"));
	CHECK(!strcmp(dst.slic, "si32282"));
	CHECK(!strcmp(dst.version, "libudsp 0.1.0 proslic"));
	CHECK_UINT(dst.lines, 2);
	CHECK_UINT(dst.streams_per_chan, 2);

	/*
	 * src above reports every feature the library names, so this asserts
	 * the mapping and not the board: each bit travels when its op forwards.
	 */
	CASE("every shared feature with a forwarding op translates");
	CHECK_UINT(dst.features, HAL_FEAT_POLARITY | HAL_FEAT_MWI_LAMP |
		   HAL_FEAT_G726 | HAL_FEAT_G729 | HAL_FEAT_G729B | HAL_FEAT_T38 |
		   HAL_FEAT_LINE_TEST | HAL_FEAT_SRTP | HAL_FEAT_CONF |
		   HAL_FEAT_WIDEBAND | HAL_FEAT_IPV6_MEDIA |
		   HAL_FEAT_RTCP_ADDR |
		   HAL_FEAT_REGION_HOOK | HAL_FEAT_DTMF_RELAY |
		   HAL_FEAT_DTMF_PLAYOUT);

	CASE("the line test bit follows the library because both ops forward");
	CHECK((dst.features & HAL_FEAT_LINE_TEST) != 0);
	CHECK((dst.features & HAL_FEAT_CONF) != 0);

	/*
	 * The Annex B bit has no library pair and rides the G.729 one: the
	 * in-tree coder libudsp carries is an Annex B coder, and it is the only
	 * G.729 the library holds. A board that reports no G.729 at all reports
	 * no Annex B either, which is what the SDP layer needs to write
	 * annexb=no there.
	 */
	CASE("annex B rides the G.729 bit and leaves with it");
	CHECK((dst.features & HAL_FEAT_G729B) != 0);
	src.features &= ~(uint32_t)UDSP_FEAT_G729;
	hal_udsp_cap_fill(&dst, &src);
	CHECK(!(dst.features & HAL_FEAT_G729));
	CHECK(!(dst.features & HAL_FEAT_G729B));
	src.features |= UDSP_FEAT_G729;
	hal_udsp_cap_fill(&dst, &src);

	/*
	 * The relay bit is the library's own answer and the call layer acts on
	 * it: a leg on a backend that carries it sends no event of its own for
	 * a digit the detector measured.
	 */
	CASE("the native digit relay travels with the library bit");
	CHECK((dst.features & HAL_FEAT_DTMF_RELAY) != 0);
	src.features &= ~(uint32_t)UDSP_FEAT_DTMF_RELAY;
	hal_udsp_cap_fill(&dst, &src);
	CHECK(!(dst.features & HAL_FEAT_DTMF_RELAY));
	src.features |= UDSP_FEAT_DTMF_RELAY;
	hal_udsp_cap_fill(&dst, &src);

	/* The delayed loop the manufacturing test plays. The core reads the bit
	 * before it arms, so a library without the loop must leave it clear. */
	CASE("the delayed loop travels with the library bit");
	CHECK(!(dst.features & HAL_FEAT_LOOP));
	src.features |= UDSP_FEAT_LOOP;
	hal_udsp_cap_fill(&dst, &src);
	CHECK((dst.features & HAL_FEAT_LOOP) != 0);
	src.features &= ~(uint32_t)UDSP_FEAT_LOOP;
	hal_udsp_cap_fill(&dst, &src);

	CASE("HAL_FEAT_KERNEL_RTP stays clear: the packet loop is in user space");
	CHECK(!(dst.features & HAL_FEAT_KERNEL_RTP));

	/*
	 * The two bits this backend decides on its own. libudsp takes connected
	 * descriptors and owns no address (udsp.h section 3), so no library
	 * feature can answer for the family a stream carries or for where its
	 * RTCP goes; the socket code of the backend answers instead, and a
	 * library that reports nothing at all still leaves both set. They sit
	 * behind the null guard, so a capability the library could not report
	 * at all still promises nothing.
	 */
	CASE("the backend advertises HAL_FEAT_IPV6_MEDIA and "
	     "HAL_FEAT_RTCP_ADDR");
	CHECK((dst.features & HAL_FEAT_IPV6_MEDIA) != 0);
	CHECK((dst.features & HAL_FEAT_RTCP_ADDR) != 0);
	{
		struct udsp_cap bare;
		struct hal_cap own;

		memset(&bare, 0, sizeof(bare));
		hal_udsp_cap_fill(&own, &bare);
		CHECK_UINT(own.features,
			   HAL_FEAT_IPV6_MEDIA | HAL_FEAT_RTCP_ADDR |
			   HAL_FEAT_REGION_HOOK);
		hal_udsp_cap_fill(&own, NULL);
		CHECK_UINT(own.features, 0);

		CASE("a library that reports no outcome leaves the record at none");
		CHECK_UINT(own.rt_policy, HAL_RT_POLICY_NONE);
		CHECK_UINT(own.rt_net_policy, HAL_RT_POLICY_NONE);
		CHECK(!own.rt_mlock);
	}

	/* 0xff first, so a field the fill never wrote reads as garbage rather
	 * than as the zero a case could mistake for a mapped value. */
	CASE("the real-time outcome travels field for field");
	src.rt_policy = UDSP_RT_POLICY_OTHER;
	src.rt_net_policy = UDSP_RT_POLICY_FIFO;
	src.rt_mlock = true;
	src.rt_cpu = 3;
	src.rt_prio = 50;
	src.rt_sched_errno = EPERM;
	memset(&dst, 0xff, sizeof(dst));
	hal_udsp_cap_fill(&dst, &src);
	CHECK_UINT(dst.rt_policy, HAL_RT_POLICY_OTHER);
	CHECK_UINT(dst.rt_net_policy, HAL_RT_POLICY_FIFO);
	CHECK(dst.rt_mlock);
	CHECK(dst.rt_cpu == 3);
	CHECK_UINT(dst.rt_prio, 50);
	CHECK_UINT(dst.rt_sched_errno, EPERM);

	CASE("a policy the library could not read stays unknown");
	src.rt_policy = UDSP_RT_POLICY_UNKNOWN;
	src.rt_net_policy = UDSP_RT_POLICY_RR;
	hal_udsp_cap_fill(&dst, &src);
	CHECK_UINT(dst.rt_policy, HAL_RT_POLICY_UNKNOWN);
	CHECK_UINT(dst.rt_net_policy, HAL_RT_POLICY_RR);
	src.rt_policy = 0;
	src.rt_net_policy = 0;
	src.rt_mlock = false;
	src.rt_cpu = 0;
	src.rt_prio = 0;
	src.rt_sched_errno = 0;
	hal_udsp_cap_fill(&dst, &src);

	/* hal.h names one G.726 rate, so the other three the library carries
	 * cannot be expressed in a hal_media and are dropped with them. */
	CASE("every codec hal.h can name translates, the three unnamable rates do not");
	CHECK_UINT(dst.codecs, (1u << HAL_CODEC_PCMU) | (1u << HAL_CODEC_PCMA) |
		   (1u << HAL_CODEC_G726_32) | (1u << HAL_CODEC_G729) |
		   (1u << HAL_CODEC_T38) | (1u << HAL_CODEC_G722) |
		   (1u << HAL_CODEC_OPUS));

	CASE("a library without a lamp op clears the lamp feature");
	src.features &= ~(uint32_t)UDSP_FEAT_MWI_LAMP;
	hal_udsp_cap_fill(&dst, &src);
	CHECK_UINT(dst.features, HAL_FEAT_POLARITY | HAL_FEAT_G726 | HAL_FEAT_G729 |
		   HAL_FEAT_G729B | HAL_FEAT_T38 | HAL_FEAT_LINE_TEST |
		   HAL_FEAT_SRTP | HAL_FEAT_CONF | HAL_FEAT_WIDEBAND |
		   HAL_FEAT_IPV6_MEDIA | HAL_FEAT_RTCP_ADDR |
		   HAL_FEAT_REGION_HOOK | HAL_FEAT_DTMF_RELAY |
		   HAL_FEAT_DTMF_PLAYOUT);

	CASE("a library built without srtp clears the srtp feature");
	src.features &= ~(uint32_t)UDSP_FEAT_SRTP;
	hal_udsp_cap_fill(&dst, &src);
	CHECK_UINT(dst.features, HAL_FEAT_POLARITY | HAL_FEAT_G726 | HAL_FEAT_G729 |
		   HAL_FEAT_G729B | HAL_FEAT_T38 | HAL_FEAT_LINE_TEST |
		   HAL_FEAT_CONF | HAL_FEAT_WIDEBAND | HAL_FEAT_IPV6_MEDIA |
		   HAL_FEAT_RTCP_ADDR |
		   HAL_FEAT_REGION_HOOK | HAL_FEAT_DTMF_RELAY |
		   HAL_FEAT_DTMF_PLAYOUT);
	src.features |= UDSP_FEAT_SRTP | UDSP_FEAT_MWI_LAMP;

	CASE("a line count above the hal maximum is clamped");
	src.lines = UDSP_LINE_MAX;
	src.streams_per_chan = UDSP_STREAM_MAX + 1;
	hal_udsp_cap_fill(&dst, &src);
	CHECK_UINT(dst.lines, HAL_LINE_MAX);
	CHECK_UINT(dst.streams_per_chan, HAL_STREAM_MAX);
}

/*
 * The region name map is on the region_apply path only. line/line.c resolves
 * the config into a directory name before the name reaches this backend. There
 * is therefore no second alpha-2 table and no default: only an upper case
 * guard and the one YUG exception. The fallback for a name with no region
 * file belongs to the resolver, which test_region_fallback() covers.
 */
static void test_region_name(void)
{
	char name[UDSP_ISO3_MAX];

	CASE("the name the core derived goes through in upper case");
	hal_udsp_region_name("usa", name, sizeof(name));
	CHECK(!strcmp(name, "USA"));
	hal_udsp_region_name("GBR", name, sizeof(name));
	CHECK(!strcmp(name, "GBR"));
	hal_udsp_region_name("ets", name, sizeof(name));
	CHECK(!strcmp(name, "ETS"));

	CASE("the YUG name the core derives selects the shipped SRB file");
	hal_udsp_region_name("YUG", name, sizeof(name));
	CHECK(!strcmp(name, "SRB"));
	hal_udsp_region_name("yug", name, sizeof(name));
	CHECK(!strcmp(name, "SRB"));

	CASE("no other name is rewritten and none takes a default");
	hal_udsp_region_name("ZZZ", name, sizeof(name));
	CHECK(!strcmp(name, "ZZZ"));
	hal_udsp_region_name("IND_SK", name, sizeof(name));
	CHECK(!strcmp(name, "IND_SK"));

	CASE("an empty or oversized name yields nothing to load");
	hal_udsp_region_name("", name, sizeof(name));
	CHECK(!name[0]);
	hal_udsp_region_name(NULL, name, sizeof(name));
	CHECK(!name[0]);
	hal_udsp_region_name("TOOLONGNAME", name, sizeof(name));
	CHECK(!name[0]);
}

/*
 * The prefetch map is the answer of the core, composed. Each row is what
 * line/line.c region_iso3_get derives, put through the one libudsp spelling
 * that differs. The map keeps only the names that reach a shipped region file.
 * A longer name takes the resolver of region_apply, so a name with no file
 * prefetches its fallback. The resolver reads only its warning record, so a
 * zeroed backend record serves it.
 */
static void test_region_prefetch(void)
{
	char name[UDSP_ISO3_MAX];
	struct hal *h = calloc(1, sizeof(*h));

	SUITE_CHECK(h != NULL);
	if (!h)
		return;

	CASE("every alpha-2 name that reaches a shipped file resolves");
	hal_udsp_region_prefetch(h, "US", name, sizeof(name));
	CHECK(!strcmp(name, "USA"));
	hal_udsp_region_prefetch(h, "gb", name, sizeof(name));
	CHECK(!strcmp(name, "GBR"));
	hal_udsp_region_prefetch(h, "UK", name, sizeof(name));
	CHECK(!strcmp(name, "GBR"));
	hal_udsp_region_prefetch(h, "EU", name, sizeof(name));
	CHECK(!strcmp(name, "ETS"));
	hal_udsp_region_prefetch(h, "AT", name, sizeof(name));
	CHECK(!strcmp(name, "AUT"));
	hal_udsp_region_prefetch(h, "AU", name, sizeof(name));
	CHECK(!strcmp(name, "AUS"));
	hal_udsp_region_prefetch(h, "NZ", name, sizeof(name));
	CHECK(!strcmp(name, "NZL"));

	/* line/line.c sends RS to the YUG directory and libudsp ships that plan
	 * as SRB.conf, so the composed row is RS to SRB. */
	CASE("the serbian name composes both maps in one row");
	hal_udsp_region_prefetch(h, "RS", name, sizeof(name));
	CHECK(!strcmp(name, "SRB"));

	CASE("an alpha-2 name with no shipped file prefetches nothing");
	hal_udsp_region_prefetch(h, "DE", name, sizeof(name));
	CHECK(!name[0]);
	hal_udsp_region_prefetch(h, "ZZ", name, sizeof(name));
	CHECK(!name[0]);

	CASE("a three letter name still takes the region_apply path");
	hal_udsp_region_prefetch(h, "usa", name, sizeof(name));
	CHECK(!strcmp(name, "USA"));
	hal_udsp_region_prefetch(h, "yug", name, sizeof(name));
	CHECK(!strcmp(name, "SRB"));

	CASE("a three letter name with no file prefetches its fallback");
	hal_udsp_region_prefetch(h, "DEU", name, sizeof(name));
	CHECK(!strcmp(name, "ETS"));
	hal_udsp_region_prefetch(h, "ZZZ", name, sizeof(name));
	CHECK(!strcmp(name, "USA"));

	CASE("an empty region prefetches nothing");
	hal_udsp_region_prefetch(h, "", name, sizeof(name));
	CHECK(!name[0]);
	hal_udsp_region_prefetch(h, NULL, name, sizeof(name));
	CHECK(!name[0]);

	free(h);
}

static void test_config_fill(void)
{
	static const char fax_path[] = "/usr/lib/udsp/uvoice-fax-fixture.so";
	struct udsp_config scfg;
	struct cfg *cfg;

	CASE("a configured fax plug-in fixture loads");
	CHECK(fixture_write_plugin_fax_rate(FIXTURE_REGION_DEFAULT, "none",
					    fax_path, 8000));
	cfg = cfg_load();
	CHECK(cfg != NULL);
	if (!cfg) {
		CHECK(fixture_write(FIXTURE_REGION_DEFAULT));
		return;
	}

	cfg->hal.sample_rate = 16000;
	hal_udsp_config_fill(&scfg, cfg);

	CASE("every voip.hal field reaches the library config");
	CHECK(!strcmp(scfg.tdm_backend, "file"));
	CHECK(!strcmp(scfg.tdm_playback, "/dev/null"));
	CHECK(!strcmp(scfg.tdm_capture, "/dev/null"));
	/* the 'none' sentinel of the schema is an empty path, which selects
	 * the in-tree fake (udsp.h struct udsp_config) */
	CHECK(!scfg.slic_plugin[0]);
	CHECK(!scfg.gpio_interrupt[0]);
	CHECK(!strcmp(scfg.fax_plugin, fax_path));
	CHECK(!strcmp(scfg.spidev, "/dev/null"));
	CHECK(!strcmp(scfg.gpio_reset, "slic_reset"));
	CHECK_UINT(scfg.populated_ports, 2);
	CHECK_UINT(scfg.channels, 2);
	CHECK_UINT(scfg.prefill_periods, 3);
	CHECK_UINT(scfg.pcm_offset_bits, 2);
	CHECK(scfg.engine_cpu == -1);
	CHECK(scfg.engine_prio == 0);
	CHECK_UINT(scfg.sample_rate, 16000);
	/* not a voip.hal field: the core raises each line with its service
	 * state, so the library must leave every line open at the start */
	CHECK(scfg.lines_start_down);

	/* udsp_open() fails on a region it cannot parse, and libudsp
	 * ships seven of the thirty three names the tone loader derives, so
	 * the region travels the region_apply job alone. */
	CASE("the base region does not reach the library config");
	CHECK(cfg->region[0] != 0);
	CHECK(!scfg.region[0]);

	cfg_free(cfg);
	CHECK(fixture_write(FIXTURE_REGION_DEFAULT));
}

/*
 * The visual indicator flag is the only field that changes shape:
 * hal_cid.mwi is an enum, and udsp_cid.mwi is a bool. Everything else is
 * copied. The off burst is an empty identity with the flag clear, which the
 * library reads as the record that turns the indicator off.
 * No event can tell the two bursts apart, so the case drives the translation
 * directly.
 */
static void test_cid_translation(void)
{
	struct udsp_cid on, off;
	struct hal_cid src;

	memset(&src, 0, sizeof(src));
	src.format = HAL_CID_ETSI_FSK;
	src.number_private = true;
	src.name_private = false;
	snprintf(src.number, sizeof(src.number), "5551234");
	snprintf(src.name, sizeof(src.name), "A CALLER");
	snprintf(src.datetime, sizeof(src.datetime), "01021530");
	snprintf(src.blockinfo, sizeof(src.blockinfo), "P");

	src.mwi = HAL_MWI_ON;
	hal_udsp_cid_fill(&on, &src);
	src.mwi = HAL_MWI_OFF;
	hal_udsp_cid_fill(&off, &src);

	CASE("the mwi enum becomes the library's bool, on and off");
	CHECK(on.mwi == true);
	CHECK(off.mwi == false);

	CASE("every other cid field is copied unchanged");
	CHECK_UINT(on.format, HAL_CID_ETSI_FSK);
	CHECK(on.number_private == true);
	CHECK(on.name_private == false);
	CHECK(!strcmp(on.number, "5551234"));
	CHECK(!strcmp(on.name, "A CALLER"));
	CHECK(!strcmp(on.datetime, "01021530"));
	CHECK(!strcmp(on.blockinfo, "P"));

	CASE("the indicator flag is the only difference between the two records");
	on.mwi = off.mwi;
	CHECK(!memcmp(&on, &off, sizeof(on)));

	CASE("the off burst keeps the all-empty identity the library reads");
	memset(&src, 0, sizeof(src));
	src.mwi = HAL_MWI_OFF;
	hal_udsp_cid_fill(&off, &src);
	CHECK(off.mwi == false);
	CHECK(!off.number[0] && !off.name[0] && !off.blockinfo[0]);
}

static void test_ring_translation(void)
{
	struct udsp_ring dst;
	struct hal_ring src;
	unsigned int i;

	memset(&src, 0, sizeof(src));
	for (i = 0; i < HAL_CADENCE_MAX; i++) {
		src.cadence[i].on_ms = (uint16_t)(1000 + i);
		src.cadence[i].off_ms = (uint16_t)(2000 + i);
		src.first[i].on_ms = (uint16_t)(3000 + i);
		src.first[i].off_ms = (uint16_t)(4000 + i);
	}
	src.cadence_count = 2;
	src.first_count = 1;
	src.cid_at_ring = 1;
	src.cid_wait_ms = 500;
	src.duration_ms = 60000;
	src.cid_present = true;
	src.cid.format = HAL_CID_BELLCORE_FSK;
	snprintf(src.cid.number, sizeof(src.cid.number), "5559999");

	hal_udsp_ring_fill(&dst, &src);

	CASE("every ring cadence pair reaches the library record");
	for (i = 0; i < HAL_CADENCE_MAX; i++) {
		CHECK_UINT(dst.cadence[i].on_ms, 1000 + i);
		CHECK_UINT(dst.cadence[i].off_ms, 2000 + i);
		CHECK_UINT(dst.first[i].on_ms, 3000 + i);
		CHECK_UINT(dst.first[i].off_ms, 4000 + i);
	}

	CASE("the ring counts, the cid window and the duration are copied");
	CHECK_UINT(dst.cadence_count, 2);
	CHECK_UINT(dst.first_count, 1);
	CHECK_UINT(dst.cid_at_ring, 1);
	CHECK_UINT(dst.cid_wait_ms, 500);
	CHECK_UINT(dst.duration_ms, 60000);

	CASE("a ring with caller identity carries the translated record");
	CHECK(dst.cid_present == true);
	CHECK(!strcmp(dst.cid.number, "5559999"));

	CASE("a ring without caller identity carries an empty one");
	src.cid_present = false;
	hal_udsp_ring_fill(&dst, &src);
	CHECK(dst.cid_present == false);
	CHECK(!dst.cid.number[0]);
}

static void test_region_view_translation(void)
{
	struct udsp_region_view src;
	struct hal_region dst;

	memset(&src, 0, sizeof(src));
	snprintf(src.iso3, sizeof(src.iso3), "GBR");
	src.cid_format = UDSP_CIDF_ETSI_LR_DTAS;
	src.mwi_format = UDSP_CIDF_ETSI_DTAS;
	src.ring_count = 2;
	src.hook_flash_min_ms = 35;
	src.hook_flash_max_ms = 145;
	src.hook_release_min_ms = 590;
	src.cw_sas_total_ms = 8200;
	src.ring[0].cadence_count = 2;
	src.ring[0].cadence[0].on_ms = 400;
	src.ring[0].cadence[0].off_ms = 200;
	src.ring[0].cadence[1].on_ms = 400;
	src.ring[0].cadence[1].off_ms = 2000;
	src.ring[1].cadence_count = 1;
	src.ring[1].cadence[0].on_ms = 800;
	src.ring[1].cadence[0].off_ms = 4000;
	src.flash_service = UDSP_FLASH_SERVICE_SPLIT;
	snprintf(src.describe, sizeof(src.describe), "United Kingdom");

	hal_udsp_region_fill(&dst, &src);

	CASE("the region view translation is field for field");
	CHECK(!strcmp(dst.iso3, "GBR"));
	CHECK_UINT(dst.flash_service, HAL_FLASH_SERVICE_SPLIT);
	CHECK(!strcmp(dst.describe, "United Kingdom"));
	CHECK_UINT(dst.cid_format, HAL_CID_ETSI_LR_DTAS);
	CHECK_UINT(dst.mwi_format, HAL_CID_ETSI_DTAS);
	CHECK_UINT(dst.hook_flash_min_ms, 35);
	CHECK_UINT(dst.hook_flash_max_ms, 145);
	CHECK_UINT(dst.hook_release_min_ms, 590);
	CHECK_UINT(dst.cw_sas_total_ms, 8200);
	CHECK_UINT(dst.ring[0].cadence_count, 2);
	CHECK_UINT(dst.ring[0].cadence[1].on_ms, 400);
	CHECK_UINT(dst.ring[0].cadence[1].off_ms, 2000);
	CHECK_UINT(dst.ring[1].cadence_count, 1);
	CHECK_UINT(dst.ring[1].cadence[0].on_ms, 800);
	CHECK_UINT(dst.ring[2].cadence_count, 0);
	CHECK_UINT(dst.cw_count, 0);
	CHECK_UINT(dst.pulse_invert, 0);

	CASE("the decadic inversion of the view reaches the record");
	src.pulse_invert = 1;
	hal_udsp_region_fill(&dst, &src);
	CHECK_UINT(dst.pulse_invert, 1);
	src.pulse_invert = 0;

	CASE("a view confirm length of 0 reaches the record as endless");
	CHECK_UINT(dst.confirm_total_ms, 0);
	CHECK_UINT(dst.confirm_endless, 1);

	CASE("a view confirm length reaches the record as its bound");
	src.confirm_total_ms = 3000;
	hal_udsp_region_fill(&dst, &src);
	CHECK_UINT(dst.confirm_total_ms, 3000);
	CHECK_UINT(dst.confirm_endless, 0);
	src.confirm_total_ms = 0;

	CASE("the call waiting variants map by index and raise the alert bound");
	src.cw_count = 2;
	src.cw[0].present = 1;
	src.cw[0].lead_ms = 100;
	src.cw[0].total_ms = 8200;
	src.cw[1].present = 1;
	src.cw[1].lead_ms = 300;
	src.cw[1].total_ms = 9000;
	src.cw[2].present = 1;
	src.cw[2].lead_ms = 500;
	src.cw[2].total_ms = 21000;
	hal_udsp_region_fill(&dst, &src);
	CHECK_UINT(dst.cw_count, 2);
	CHECK_UINT(dst.cw_lead_in_ms[0], 100);
	CHECK_UINT(dst.cw_lead_in_ms[1], 300);
	CHECK_UINT(dst.cw_lead_in_ms[2], 500);
	/* cw_sas_total_ms is the maximum over the base and every variant. */
	CHECK_UINT(dst.cw_sas_total_ms, 21000);
}

static void test_region_get(void)
{
	struct hal_region reg;
	struct probe p;

	if (!probe_up(&p)) {
		CHECK(false);
		probe_down(&p);
		return;
	}

	CASE("the region read-back answers the loaded country data");
	memset(&reg, 0, sizeof(reg));
	CHECK(hal_backend_ops.region_get != NULL);
	CHECK(hal_backend_ops.region_get(p.h, &reg) == HAL_OK);
	CHECK(!strcmp(reg.iso3, "USA"));
	CHECK_UINT(reg.cid_format, HAL_CID_BELLCORE_FSK);
	CHECK_UINT(reg.mwi_format, HAL_CID_BELLCORE_FSK);
	CHECK_UINT(reg.hook_flash_min_ms, 300);
	CHECK_UINT(reg.hook_flash_max_ms, 1300);
	CHECK_UINT(reg.hook_release_min_ms, 1301);
	/* The maximum over the base (20600) and c4 (21400), so the offer window
	 * covers the longest distinctive variant. */
	CHECK_UINT(reg.cw_sas_total_ms, 21400);
	CHECK_UINT(reg.cw_count, 4);
	CHECK_UINT(reg.cw_lead_in_ms[0], 300);	/* the plain tone */
	CHECK_UINT(reg.cw_lead_in_ms[1], 300);	/* c1 */
	CHECK_UINT(reg.cw_lead_in_ms[2], 300);	/* c2 */
	CHECK_UINT(reg.cw_lead_in_ms[3], 500);	/* c3 */
	CHECK_UINT(reg.cw_lead_in_ms[4], 700);	/* c4 */
	CHECK_UINT(reg.ring[0].cadence_count, 1);
	CHECK_UINT(reg.ring[0].cadence[0].on_ms, 2000);
	CHECK_UINT(reg.ring[0].cadence[0].off_ms, 4000);
	CHECK_UINT(reg.ring[7].cadence_count, 1);
	CHECK_UINT(reg.flash_service, HAL_FLASH_SERVICE_DROP);
	CHECK(!strcmp(reg.describe, "United States"));
	CHECK_UINT(reg.pulse_invert, 0);
	CHECK_UINT(reg.confirm_total_ms, 600);
	CHECK_UINT(reg.confirm_endless, 0);

	CASE("the read-back carries the decadic inversion of NZL.conf");
	CHECK(probe_region(&p, "NZL"));
	CHECK(hal_backend_ops.region_get(p.h, &reg) == HAL_OK);
	CHECK(!strcmp(reg.iso3, "NZL"));
	CHECK_UINT(reg.pulse_invert, 1);

	CASE("the read-back carries the 3000 ms confirm tone of NZL.conf");
	CHECK_UINT(reg.confirm_total_ms, 3000);
	CHECK_UINT(reg.confirm_endless, 0);

	CASE("the read-back carries the endless confirm tone of AUS.conf");
	CHECK(probe_region(&p, "AUS"));
	CHECK(hal_backend_ops.region_get(p.h, &reg) == HAL_OK);
	CHECK(!strcmp(reg.iso3, "AUS"));
	CHECK_UINT(reg.confirm_total_ms, 0);
	CHECK_UINT(reg.confirm_endless, 1);

	CASE("the read-back follows a region swap to the divergent pair");
	CHECK(probe_region(&p, "SRB"));
	CHECK(hal_backend_ops.region_get(p.h, &reg) == HAL_OK);
	CHECK(!strcmp(reg.iso3, "SRB"));
	CHECK_UINT(reg.cid_format, HAL_CID_DTMF_DTAS);
	CHECK_UINT(reg.mwi_format, HAL_CID_ETSI_DTAS);
	CHECK_UINT(reg.cw_sas_total_ms, 70000);
	CHECK_UINT(reg.hook_flash_min_ms, 35);
	CHECK_UINT(reg.flash_service, HAL_FLASH_SERVICE_DROP);
	CHECK(!strcmp(reg.describe, "Serbia"));
	CHECK_UINT(reg.pulse_invert, 0);
	probe_down(&p);
}

static void test_fxsnum(void)
{
	char buf[32] = { 0 };
	FILE *f;

	hal_udsp_fxsnum_write(2);

	CASE("the fxs count file carries the line count");
	f = fopen(fxsnum_path, "r");
	CHECK(f != NULL);
	if (!f)
		return;

	CHECK(fgets(buf, sizeof(buf), f) != NULL);
	fclose(f);
	CHECK(!strcmp(buf, "2\n"));

	CASE("an unwritable path costs a trace and no more");
	hal_udsp_fxsnum_path = "/proc/uvoice-does-not-exist/fxsnum";
	hal_udsp_fxsnum_write(1);
	hal_udsp_fxsnum_path = fxsnum_path;
	CHECK(access("/proc/uvoice-does-not-exist/fxsnum", F_OK) != 0);
}

/* --- the end to end cases -------------------------------------------------- */

static void test_attach(void)
{
	const struct hal_cap *cap;
	struct hal_counters counters;
	struct probe p;
	char buf[32] = { 0 };
	FILE *f;

	CASE("open attaches the library and the ops table names udsp");
	CHECK(!strcmp(hal_backend_ops.name, "udsp"));
	if (!probe_open(&p)) {
		CHECK(false);
		probe_close(&p);
		return;
	}

	cap = hal_backend_ops.cap_get(p.h);

	CASE("the probed capability reaches the core");
	CHECK(cap != NULL);
	CHECK(!strcmp(cap->backend, "udsp"));
	/* the fake reports two channels and the fixture populates two ports */
	CHECK_UINT(cap->lines, 2);
	CHECK(cap->streams_per_chan >= 1);
	CHECK(cap->version[0] != 0);

	/*
	 * The real library reporting through the real fake plug-in, which is
	 * the half the pure cap case cannot reach: the lamp bit is the
	 * plug-in's answer and the srtp bit is libsrtp2's. The pure case pins
	 * the mapping; this one pins what the shipped library actually claims
	 * today, and T.38 and Opus are absent from it. No fax_plugin is
	 * configured in this fixture, so no gateway is loaded and the library's
	 * T.38 capability stays clear. libudsp carries no Opus coder. G.722 is
	 * absent too: the fixture line runs at 8 kHz, and the library
	 * advertises G.722 only on a 16 kHz line. The two media socket bits and
	 * the region hook bit are this file's own and travel whatever the
	 * library reports. The loop bit is always set in the library, as the
	 * conference bit is: the loop runs inside the engine and asks the
	 * plug-in for nothing.
	 */
	CASE("the attached capability advertises what the shipped library carries");
	CHECK_UINT(cap->features, HAL_FEAT_POLARITY | HAL_FEAT_MWI_LAMP |
		   HAL_FEAT_G726 | HAL_FEAT_G729 | HAL_FEAT_G729B |
		   HAL_FEAT_LINE_TEST | HAL_FEAT_SRTP | HAL_FEAT_CONF |
		   HAL_FEAT_IPV6_MEDIA | HAL_FEAT_RTCP_ADDR |
		   HAL_FEAT_REGION_HOOK | HAL_FEAT_DTMF_RELAY |
		   HAL_FEAT_DTMF_PLAYOUT | HAL_FEAT_LOOP);
	CHECK_UINT(cap->codecs, (1u << HAL_CODEC_PCMU) | (1u << HAL_CODEC_PCMA) |
		   (1u << HAL_CODEC_G726_32) | (1u << HAL_CODEC_G729));
	CHECK_UINT(ts_be.geom.rate, 8000);
	CHECK_UINT(ts_be.geom.frames_per_period, 80);

	/* No fax_plugin is configured in this fixture, so no gateway is loaded
	 * and the library's T.38 capability stays clear. call.c offers T.38 on
	 * this bit alone, so a bit set with no coder would drop every fax call
	 * G.711 passthrough carries. */
	CASE("t38 is clear on the shipped library, so no fax upspeed is offered");
	CHECK(!(cap->features & HAL_FEAT_T38));

	CASE("the attach publishes the line count for the platform scripts");
	f = fopen(fxsnum_path, "r");
	CHECK(f != NULL);
	if (f) {
		CHECK(fgets(buf, sizeof(buf), f) != NULL);
		fclose(f);
		CHECK(!strcmp(buf, "2\n"));
	}

	CASE("counters answer from the library");
	memset(&counters, 0xff, sizeof(counters));
	hal_backend_ops.counters_get(p.h, &counters);
	CHECK_UINT(counters.events_dropped, 0);
	CHECK(counters.events_sent < UINT64_MAX);

	probe_close(&p);
}

static void test_sample_rate_attach(void)
{
	const struct hal_cap *cap;
	struct probe p;

	CASE("a 16 kHz HAL config reaches the library open path");
	CHECK(fixture_write_plugin_rate(FIXTURE_REGION_DEFAULT, "none", 16000));
	if (!probe_open(&p)) {
		CHECK(false);
		probe_close(&p);
		CHECK(fixture_write(FIXTURE_REGION_DEFAULT));
		return;
	}

	cap = hal_backend_ops.cap_get(p.h);
	CHECK(cap != NULL);
	CHECK(cap->features & HAL_FEAT_WIDEBAND);
	CHECK(cap->codecs & (1u << HAL_CODEC_G722));
	CHECK_UINT(ts_be.geom.rate, 16000);
	CHECK_UINT(ts_be.geom.frames_per_period, 160);
	probe_close(&p);
	CHECK(fixture_write(FIXTURE_REGION_DEFAULT));
}

static void test_sample_rate_degrade(void)
{
	const struct hal_cap *cap;
	struct probe p;

	CASE("a refused 16 kHz TDM grant reopens and attaches at 8 kHz");
	ts_be.grant_rate = 8000;
	CHECK(fixture_write_plugin_rate(FIXTURE_REGION_DEFAULT, "none", 16000));
	if (!probe_open(&p)) {
		CHECK(false);
		probe_close(&p);
		ts_be.grant_rate = 0;
		CHECK(fixture_write(FIXTURE_REGION_DEFAULT));
		return;
	}

	cap = hal_backend_ops.cap_get(p.h);
	CHECK(cap != NULL);
	CHECK_UINT(ts_be.open_count, 2);
	CHECK_UINT(ts_be.asked_rate[0], 16000);
	CHECK_UINT(ts_be.asked_rate[1], 8000);
	CHECK_UINT(ts_be.geom.rate, 8000);
	CHECK_UINT(ts_be.geom.frames_per_period, 80);

	CASE("the TDM rate degrade clears the wideband feature and G.722");
	CHECK(!(cap->features & HAL_FEAT_WIDEBAND));
	CHECK(!(cap->codecs & (1u << HAL_CODEC_G722)));

	probe_close(&p);
	ts_be.grant_rate = 0;
	CHECK(fixture_write(FIXTURE_REGION_DEFAULT));
}

/*
 * The region cannot take the HAL down. libudsp ships seven region files, and
 * the uvoice tone loader can name thirty three. A config that names one of
 * the other twenty six must still attach. The tone table is a region_apply
 * job that loads the file or its fallback and reports its own failure, and
 * the attach does not wait for it.
 */
static void test_region_absent_attaches(void)
{
	struct probe p;
	bool up;

	CASE("a base region the image does not ship still attaches");
	CHECK(fixture_write("DE"));
	up = probe_open(&p);
	CHECK(up);
	if (up) {
		const struct hal_cap *cap = hal_backend_ops.cap_get(p.h);

		CHECK_UINT(cap->lines, 2);
	}
	probe_close(&p);
	CHECK(fixture_write(FIXTURE_REGION_DEFAULT));
}

static void test_dsp_start_job(void)
{
	struct hal_job job = { 0 };
	int32_t status = -1;
	uint16_t seq = 0xffff;
	struct probe p;

	if (!probe_open(&p)) {
		CASE("dsp_start needs an attached backend");
		CHECK(false);
		probe_close(&p);
		return;
	}

	CASE("dsp_start is accepted and fills the token the library assigned");
	CHECK(hal_backend_ops.dsp_start(p.h, &job) == HAL_OK);
	CHECK(job.token != 0);

	CASE("the bring-up completes as a job done record");
	CHECK(job_wait(&p, job.token, 5000, &status, &seq));
	CHECK_UINT(status, HAL_OK);

	CASE("a completion carries seq 0, outside the gap accounting");
	CHECK_UINT(seq, 0);

	probe_down(&p);
}

static unsigned int job_done_count(struct probe *p, uint32_t token)
{
	unsigned int i, n = 0;

	for (i = 0; i < p->ev_len; i++) {
		if (p->ev[i].type == HAL_EVENT_JOB_DONE &&
		    p->ev[i].u.job.token == token)
			n++;
	}

	return n;
}

/* Where the completion for one token sits among the records the core read;
 * -1 when the core never got it. */
static int job_done_index(struct probe *p, uint32_t token)
{
	unsigned int i;

	for (i = 0; i < p->ev_len; i++) {
		if (p->ev[i].type == HAL_EVENT_JOB_DONE &&
		    p->ev[i].u.job.token == token)
			return (int)i;
	}

	return -1;
}

/*
 * A bring-up that failed says so, once. The chain consumes the completion of
 * the library for the start token and posts its own, and the case checks two
 * lies. A chain that reported HAL_OK would leave main.c serving lines that
 * never came up. A chain that went on to the region half would post a second
 * record for a token that the core already finished.
 *
 * The case uses one channel: slic_job_start fails the whole start only when
 * no line came ready (libudsp), and the fake injects one failure. That one
 * init failure is therefore the whole board.
 */
static void test_dsp_start_failed(void)
{
	struct hal_job job = { 0 };
	int32_t status = -1;
	struct probe p;

	if (!probe_open_opts(&p, UDSP_SLIC_FAKE_OPT_ALL, 1)) {
		CASE("the failed bring-up case needs an attached backend");
		CHECK(false);
		probe_close(&p);
		return;
	}

	udsp_slic_fake_error_set(p.h->s->slic, UDSP_SLIC_FAKE_INIT, -EIO);

	CASE("a bring-up whose slic init fails completes fatal");
	CHECK(hal_backend_ops.dsp_start(p.h, &job) == HAL_OK);
	CHECK(job.token != 0);
	CHECK(job_wait(&p, job.token, 5000, &status, NULL));
	CHECK_UINT(status, HAL_FATAL);

	/* Long enough for the region half to have run and reported, if the
	 * chain had taken it. */
	probe_settle(&p, 300);

	CASE("the failed bring-up completes exactly once");
	CHECK_UINT(job_done_count(&p, job.token), 1);

	/* The tone plan is the second half of the chain and it must not have
	 * been started at all: the library still holds no region. */
	CASE("a failed bring-up loads no region");
	CHECK(!atomic_load_explicit(&p.h->s->region_valid, memory_order_acquire));

	probe_close(&p);
}

static const struct hal_event *hook_find(struct probe *p, uint8_t state)
{
	unsigned int i;

	for (i = 0; i < p->ev_len; i++) {
		if (p->ev[i].type == HAL_EVENT_HOOK &&
		    p->ev[i].u.hook.state == state)
			return &p->ev[i];
	}

	return NULL;
}

/*
 * Two bring-ups at once. main.c retries a transient bring-up, so two shapes
 * matter. A second start can arrive while the first is still queued, and the
 * library refuses it without disturbing the first. The retry after the first
 * start completes must work. A chain that kept the old token would strand the
 * retry on the 30 s backstop of the core.
 */
static void test_concurrent_start(void)
{
	struct hal_job first = { 0 };
	struct hal_job second = { 0 };
	struct hal_job retry = { 0 };
	int32_t status = -1;
	struct probe p;

	if (!probe_open(&p)) {
		CASE("the concurrent start case needs an attached backend");
		CHECK(false);
		probe_close(&p);
		return;
	}

	CASE("the first bring-up is accepted");
	CHECK(hal_backend_ops.dsp_start(p.h, &first) == HAL_OK);

	/* Before the completion: the job is queued and the library is busy. */
	CASE("a second bring-up during the first answers transient, never fatal");
	CHECK(hal_backend_ops.dsp_start(p.h, &second) == HAL_TRANSIENT);
	CHECK_UINT(second.token, 0);

	CASE("the first bring-up completes untouched by the refusal");
	CHECK(job_wait(&p, first.token, 5000, &status, NULL));
	CHECK_UINT(status, HAL_OK);

	CASE("the retry after it is accepted and completes on its own token");
	status = -1;
	CHECK(hal_backend_ops.dsp_start(p.h, &retry) == HAL_OK);
	CHECK(retry.token != first.token);
	CHECK(job_wait(&p, retry.token, 5000, &status, NULL));
	CHECK_UINT(status, HAL_OK);

	probe_down(&p);
}

static void test_hook_events(void)
{
	const struct hal_event *seize;
	const struct hal_event *release;
	struct probe p;
	uint16_t seize_seq = 0;
	bool offhook = true;

	if (!probe_up(&p)) {
		CASE("the hook cases need a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	/* The bring-up publishes the resting hook level of every line, so the
	 * window under test starts after those records are drained. */
	probe_wait(&p, 200);
	p.ev_len = 0;

	CASE("the line reads on hook before anything is injected");
	CHECK(hal_backend_ops.line_hook_query(p.h, 0, &offhook) == HAL_OK);
	CHECK(!offhook);

	udsp_slic_fake_hook_set(p.h->s->slic, 0, true);
	probe_wait(&p, 400);

	CASE("a seize reaches the core as HAL_EVENT_HOOK");
	seize = hook_find(&p, HAL_HOOK_SEIZE);
	CHECK(seize != NULL);
	if (seize) {
		CHECK_UINT(seize->line, 0);
		seize_seq = seize->seq;
	}

	CASE("a hardware event carries a seq from 1");
	CHECK(seize_seq >= 1);

	CASE("the cached hook state follows the injected level");
	CHECK(hal_backend_ops.line_hook_query(p.h, 0, &offhook) == HAL_OK);
	CHECK(offhook);

	/* release_min_ms is 1301 in the USA plan, so the release edge needs
	 * more than a second of engine time to be reported. */
	udsp_slic_fake_hook_set(p.h->s->slic, 0, false);
	probe_wait(&p, 2200);

	CASE("a release follows the seize with the next seq");
	release = hook_find(&p, HAL_HOOK_RELEASE);
	CHECK(release != NULL);
	if (release && seize)
		CHECK(release->seq > seize_seq);

	CASE("a seize and a release carry the direct decadic coding");
	if (seize)
		CHECK_UINT(seize->u.hook.pulse_invert, 0);
	if (release)
		CHECK_UINT(release->u.hook.pulse_invert, 0);

	probe_down(&p);
}

static void test_ring(void)
{
	struct hal_ring ring;
	struct probe p;

	if (!probe_up(&p)) {
		CASE("the ring case needs a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	p.ev_len = 0;
	memset(&ring, 0, sizeof(ring));
	ring.cadence_count = 1;
	ring.cadence[0].on_ms = 2000;
	ring.cadence[0].off_ms = 4000;

	CASE("ring start is accepted and raises HAL_EVENT_RING_START");
	CHECK(hal_backend_ops.line_ring_start(p.h, 0, &ring) == HAL_OK);
	probe_wait(&p, 300);
	CHECK(ev_find(&p, HAL_EVENT_RING_START) != NULL);

	CASE("ring stop is accepted and raises HAL_EVENT_RING_END");
	CHECK(hal_backend_ops.line_ring_stop(p.h, 0) == HAL_OK);
	probe_wait(&p, 300);
	CHECK(ev_find(&p, HAL_EVENT_RING_END) != NULL);

	probe_down(&p);
}

static void test_cid(void)
{
	const struct hal_event *ev;
	struct hal_cid cid;
	struct probe p;

	if (!probe_up(&p)) {
		CASE("the cid case needs a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	p.ev_len = 0;
	memset(&cid, 0, sizeof(cid));
	cid.format = HAL_CID_BELLCORE_FSK;
	snprintf(cid.number, sizeof(cid.number), "5551234");
	snprintf(cid.name, sizeof(cid.name), "TEST");

	CASE("a type 1 caller identity is accepted");
	CHECK(hal_backend_ops.line_cid_send(p.h, 0, HAL_CID_TYPE1, &cid) == HAL_OK);

	CASE("the burst completes as HAL_EVENT_CID_SENT");
	probe_wait(&p, 4000);
	ev = ev_find(&p, HAL_EVENT_CID_SENT);
	CHECK(ev != NULL);
	if (ev) {
		CHECK_UINT(ev->u.cid_sent.kind, HAL_CID_TYPE1);
		CHECK(ev->u.cid_sent.status == 0);
	}

	probe_down(&p);
}

/*
 * The visual indicator travels in the CID record. The off burst is an
 * identity with every field empty and mwi clear. The on burst is the same
 * record with mwi set, which is exactly what mwi_fsk_send() of the core
 * builds.
 */
static void test_mwi(void)
{
	struct hal_cid cid;
	struct probe p;

	if (!probe_up(&p)) {
		CASE("the mwi cases need a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	p.ev_len = 0;
	memset(&cid, 0, sizeof(cid));
	cid.format = HAL_CID_BELLCORE_FSK;
	cid.mwi = HAL_MWI_ON;

	CASE("the visual indicator on is a cid record the library accepts");
	CHECK(hal_backend_ops.line_cid_send(p.h, 0, HAL_CID_TYPE1, &cid) == HAL_OK);
	probe_wait(&p, 4000);
	CHECK(ev_count(&p, HAL_EVENT_CID_SENT) >= 1);

	CASE("the lamp op is accepted while the plug-in carries one");
	CHECK(hal_backend_ops.line_mwi_set(p.h, 0, HAL_MWI_ON) == HAL_OK);
	CHECK(hal_backend_ops.line_mwi_set(p.h, 0, HAL_MWI_OFF) == HAL_OK);

	probe_down(&p);
}

static void test_mwi_without_lamp(void)
{
	const struct hal_cap *cap;
	struct probe p;
	bool up;

	up = probe_open_opts(&p, UDSP_SLIC_FAKE_OPT_ALL &
			     ~(uint32_t)UDSP_SLIC_FAKE_OPT_MWI_LAMP, 2);

	CASE("a plug-in without a lamp op clears HAL_FEAT_MWI_LAMP");
	CHECK(up);
	if (!up) {
		probe_close(&p);
		return;
	}

	cap = hal_backend_ops.cap_get(p.h);
	CHECK(!(cap->features & HAL_FEAT_MWI_LAMP));

	CASE("line_mwi_set answers unsupported without a lamp op");
	CHECK(probe_start(&p));
	CHECK(hal_backend_ops.line_mwi_set(p.h, 0, HAL_MWI_ON) == HAL_UNSUPPORTED);

	probe_down(&p);
}

static void test_tone_and_digit(void)
{
	struct hal_channel_cfg ccfg, dir;
	struct hal_hook_timing hook;
	struct probe p;
	unsigned int i;
	bool gain_row;

	if (!probe_up(&p)) {
		CASE("the tone cases need a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	p.ev_len = 0;

	CASE("the dial tone of the loaded region plays");
	CHECK(hal_backend_ops.tone_play(p.h, 0, HAL_TONE_DIAL, 0) == HAL_OK);
	probe_wait(&p, 100);

	CASE("tone stop is accepted");
	CHECK(hal_backend_ops.tone_stop(p.h, 0) == HAL_OK);

	CASE("the number-unobtainable tone of the shipped USA file plays");
	CHECK(hal_backend_ops.tone_play(p.h, 0, HAL_TONE_NUMBER_UNOBTAINABLE,
					0) == HAL_OK);
	probe_wait(&p, 100);
	CHECK(hal_backend_ops.tone_stop(p.h, 0) == HAL_OK);

	CASE("a tone the region does not carry answers unsupported");
	CHECK(hal_backend_ops.tone_play(p.h, 0, HAL_TONE_NONE, 0) == HAL_UNSUPPORTED);

	CASE("a finite digit playout ends with HAL_EVENT_AUDIO_END");
	p.ev_len = 0;
	CHECK(hal_backend_ops.digit_play(p.h, 0, '5', 100) == HAL_OK);
	probe_wait(&p, 600);
	CHECK(ev_find(&p, HAL_EVENT_AUDIO_END) != NULL);

	CASE("the detector mask, the gains and the tone options are accepted");
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.detect_mask = HAL_DETECT_DTMF | HAL_DETECT_PULSE;
	ccfg.ec_enable = true;
	ccfg.ec_nlp_enable = true;
	ccfg.ec_tail_ms = 48;
	ccfg.slic_gain_tx = -6;
	ccfg.slic_gain_rx = -18;
	/* The policy the two disabler options pick is pinned in libudsp's own
	 * test_media; this case pins the forwarding, read back from the
	 * channel the tick configured (udsp_priv.h, the slic fake pattern). */
	ccfg.ec_tone_latch = true;
	ccfg.ec_tone_force_disable = true;
	CHECK(hal_backend_ops.channel_config_set(p.h, 0, &ccfg) == HAL_OK);
	probe_wait(&p, 50);
	CHECK(p.h->s->chan[0].cfg.ec_tone_latch);
	CHECK(p.h->s->chan[0].cfg.ec_tone_force_disable);
	/* The receive half reaches the channel whole, and libudsp applies the
	 * loss ahead of the SLIC (core/chan.c chan_rx_split_apply). */
	CHECK(p.h->s->chan[0].cfg.slic_gain_rx == -18);
	CHECK(p.h->s->chan[0].dl_voice_gain == -18);

	/* The HAL pair names each direction from the interface (hal.h struct
	 * hal_channel_cfg), and libudsp applies its gain_tx to the uplink
	 * (core/chan.c chan_ul) and its gain_rx to the downlink (chan_dl,
	 * through dl_voice_gain). Each case starts from the state the case
	 * before it left, so a view the tick never applied fails a check. */
	CASE("a HAL transmit gain reaches the downlink gain of libudsp alone");
	memset(&dir, 0, sizeof(dir));
	dir.gain_tx = -1;
	CHECK(hal_backend_ops.channel_config_set(p.h, 0, &dir) == HAL_OK);
	probe_wait(&p, 50);
	CHECK(p.h->s->chan[0].cfg.gain_rx == -1);
	CHECK(p.h->s->chan[0].cfg.gain_tx == 0);
	CHECK(p.h->s->chan[0].dl_voice_gain == -1);

	CASE("a HAL receive gain reaches the uplink gain of libudsp alone");
	memset(&dir, 0, sizeof(dir));
	dir.gain_rx = -1;
	CHECK(hal_backend_ops.channel_config_set(p.h, 0, &dir) == HAL_OK);
	probe_wait(&p, 50);
	CHECK(p.h->s->chan[0].cfg.gain_tx == -1);
	CHECK(p.h->s->chan[0].cfg.gain_rx == 0);
	CHECK(p.h->s->chan[0].dl_voice_gain == 0);

	CASE("the three hook windows are accepted");
	memset(&hook, 0, sizeof(hook));
	hook.flash_min_ms = 300;
	hook.flash_max_ms = 1300;
	hook.release_min_ms = 1301;
	CHECK(hal_backend_ops.line_hook_timing_set(p.h, 0, &hook) == HAL_OK);

	CASE("the line state and the polarity are accepted");
	CHECK(hal_backend_ops.line_state_set(p.h, 0, HAL_LINE_ACTIVE) == HAL_OK);
	CHECK(hal_backend_ops.line_polarity_set(p.h, 0, HAL_POL_REVERSE) == HAL_OK);
	CHECK(hal_backend_ops.line_polarity_set(p.h, 0, HAL_POL_FORWARD) == HAL_OK);

	probe_down(&p);

	/*
	 * The journal oracle for the pair above needs a fresh short probe:
	 * journal_push (fake.c) drops rows once the 64-row journal is full,
	 * so only an action early in a probe's life keeps its row, and the
	 * tone cases above have long since filled it with hook samples.
	 */
	if (!probe_up(&p)) {
		CASE("the gain journal case needs a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	CASE("the analog pair is accepted right after bring-up");
	CHECK(hal_backend_ops.channel_config_set(p.h, 0, &ccfg) == HAL_OK);

	/* the journal read obeys the quiescence rule of the oracle block: the
	 * slic thread stops sampling at the dsp stop, and only then is either
	 * record safe to read */
	probe_stop(&p);
	fake_take(&p);

	/* libudsp keeps the receive loss in the engine, so the SLIC takes its
	 * 0 dB receive preset. */
	CASE("the fake slic journal holds GAIN_SET(-6, 0) for line 0");
	gain_row = false;
	for (i = 0; i < fake_jr_len; i++) {
		if (fake_jr[i].call != UDSP_SLIC_FAKE_GAIN_SET ||
		    fake_jr[i].chan != 0)
			continue;
		if (fake_jr[i].arg0 == -6 && fake_jr[i].arg1 == 0)
			gain_row = true;
	}
	CHECK(gain_row);

	CASE("and the slic holds the pair as its last written gains");
	CHECK(fake_st[0].gain_tx_half_db == -6);
	CHECK(fake_st[0].gain_rx_half_db == 0);

	probe_close(&p);
}

static void test_region_job(void)
{
	struct hal_job job = { 0 };
	int32_t status = -1;
	uint16_t seq = 0xffff;
	struct probe p;

	if (!probe_up(&p)) {
		CASE("the region case needs a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	CASE("region_apply is accepted and fills a token");
	CHECK(hal_backend_ops.region_apply(p.h, "GBR", &job) == HAL_OK);
	CHECK(job.token != 0);

	CASE("the region load completes as a job done with seq 0");
	CHECK(job_wait(&p, job.token, 5000, &status, &seq));
	CHECK_UINT(status, HAL_OK);
	CHECK_UINT(seq, 0);

	CASE("a region with no name is refused before the library sees it");
	CHECK(hal_backend_ops.region_apply(p.h, "", &job) == HAL_FATAL);
	CHECK(hal_backend_ops.region_apply(p.h, NULL, &job) == HAL_FATAL);

	/* GBR is the table before the job, so the read-back of USA can only
	 * come from the fallback. */
	CASE("a region the image does not ship loads the USA fallback");
	{
		struct hal_job fallback = { 0 };
		struct hal_region reg;

		status = -1;
		CHECK(hal_backend_ops.region_apply(p.h, "ZZZ", &fallback) ==
		      HAL_OK);
		CHECK(job_wait(&p, fallback.token, 5000, &status, NULL));
		CHECK_UINT(status, HAL_OK);
		memset(&reg, 0, sizeof(reg));
		CHECK(hal_backend_ops.region_get(p.h, &reg) == HAL_OK);
		CHECK(!strcmp(reg.iso3, "USA"));
	}

	/* A uci region of YUG names the table that ships as SRB.conf, so the
	 * exception has to reach a file that really loads. */
	CASE("the YUG name the core derives loads the shipped SRB table");
	{
		struct hal_job srb = { 0 };

		status = -1;
		CHECK(hal_backend_ops.region_apply(p.h, "YUG", &srb) == HAL_OK);
		CHECK(job_wait(&p, srb.token, 5000, &status, NULL));
		CHECK_UINT(status, HAL_OK);
	}

	probe_down(&p);
}

#define WARN_DEU_ETS	"udsp: region DEU has no file, loading ETS"
#define WARN_CHN_USA	"udsp: region CHN has no file, loading USA"

/* A ring with caller identity in the transport of the region read-back, the
 * shape that line/line.c builds for an incoming call. */
static void ring_cid_fill(struct hal_ring *ring, const struct hal_region *reg)
{
	memset(ring, 0, sizeof(*ring));
	ring->cadence_count = 1;
	ring->cadence[0].on_ms = 2000;
	ring->cadence[0].off_ms = 4000;
	ring->cid_present = true;
	ring->cid.format = reg->cid_format;
	snprintf(ring->cid.number, sizeof(ring->cid.number), "5551234");
}

/*
 * A name with no region file loads a fallback, because libudsp refuses a ring
 * with caller identity until a tone plan is loaded. A European name loads ETS
 * and every other name loads USA. The read-back names the file that loaded,
 * and the warning names the configured name once.
 */
static void test_region_fallback(void)
{
	struct hal_job job = { 0 };
	struct hal_region reg;
	struct hal_ring ring;
	int32_t status = -1;
	struct probe p;

	if (!probe_up(&p)) {
		CASE("the fallback cases need a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	warn_capture_begin();

	CASE("a european name with no file loads the ETS fallback");
	CHECK(probe_region(&p, "DEU"));
	memset(&reg, 0, sizeof(reg));
	CHECK(hal_backend_ops.region_get(p.h, &reg) == HAL_OK);
	CHECK(!strcmp(reg.iso3, "ETS"));

	CASE("a ring with caller identity starts on the fallback plan");
	ring_cid_fill(&ring, &reg);
	CHECK(hal_backend_ops.line_ring_start(p.h, 0, &ring) == HAL_OK);
	CHECK(hal_backend_ops.line_ring_stop(p.h, 0) == HAL_OK);

	CASE("any other name with no file loads the USA fallback");
	CHECK(probe_region(&p, "CHN"));
	CHECK(hal_backend_ops.region_get(p.h, &reg) == HAL_OK);
	CHECK(!strcmp(reg.iso3, "USA"));

	CASE("each name warns once, and a repeat of the name stays quiet");
	CHECK(probe_region(&p, "CHN"));
	CHECK_UINT(warn_capture_count(WARN_DEU_ETS), 1);
	CHECK_UINT(warn_capture_count(WARN_CHN_USA), 1);
	CHECK_UINT(warn_capture_count("has no file, loading"), 2);
	warn_capture_end();

	/* The exact enum, not merely a failure. libudsp maps a job result at
	 * the origin, so the -ENOENT of an absent region file reaches the core
	 * as HAL_UNSUPPORTED. An oracle that asked only for "not ok" would pass
	 * on the raw -2 of a backend that forwarded the errno instead. */
	CASE("an absent fallback file fails the job and keeps the region");
	CHECK(region_dir_enter());
	CHECK(hal_backend_ops.region_apply(p.h, "DEU", &job) == HAL_OK);
	region_dir_leave();
	CHECK(job_wait(&p, job.token, 5000, &status, NULL));
	CHECK_UINT(status, HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.region_get(p.h, &reg) == HAL_OK);
	CHECK(!strcmp(reg.iso3, "USA"));
	/* the handle is still live and still answers ops */
	CHECK(hal_backend_ops.line_ring_stop(p.h, 0) == HAL_OK);

	probe_down(&p);
}

/*
 * The completion carries the mapped enum, never an ERRNO. libudsp maps a job
 * result at the origin, and the core casts the field straight to enum
 * hal_status. The three failure codes must therefore arrive as 1, 2 and 3,
 * not as -16, -2 and -5. The absent fallback file above checks
 * HAL_UNSUPPORTED; the two below inject on the same op, so only the errno
 * differs and the mapping separates the answers.
 *
 * The injection point is the safe pass of the stop, which writes the open
 * linefeed on every live line. The ack of a job carries the first errno of
 * the slic thread, so an injected linefeed failure becomes the result of the
 * stop job.
 */
static void test_completion_status_map(void)
{
	struct hal_job job = { 0 };
	int32_t status = -1;
	struct probe p;

	if (!probe_up(&p)) {
		CASE("the completion status cases need a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	/* The library brings every line up open for this backend, so the
	 * safe pass has a feed to open only on a line the core raised. */
	CHECK(hal_backend_ops.line_state_set(p.h, 0, HAL_LINE_ACTIVE) == HAL_OK);

	/* -EBUSY is a job result the caller may retry (libudsp
	 * cmd_job_status_map), so it reaches the core as HAL_TRANSIENT. */
	udsp_slic_fake_error_set(p.h->s->slic, UDSP_SLIC_FAKE_LINEFEED_SET,
				 -EBUSY);

	CASE("an injected busy stop completes exactly transient");
	CHECK(hal_backend_ops.dsp_stop(p.h, &job) == HAL_OK);
	CHECK(job_wait(&p, job.token, 5000, &status, NULL));
	CHECK_UINT(status, HAL_TRANSIENT);
	probe_close(&p);

	if (!probe_up(&p)) {
		CASE("the fatal completion case needs a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	/* Everything else failed on the hardware and must not be retried, so
	 * the same op with -EIO reaches the core as HAL_FATAL. */
	memset(&job, 0, sizeof(job));
	status = -1;
	CHECK(hal_backend_ops.line_state_set(p.h, 0, HAL_LINE_ACTIVE) == HAL_OK);
	udsp_slic_fake_error_set(p.h->s->slic, UDSP_SLIC_FAKE_LINEFEED_SET,
				 -EIO);

	CASE("an injected io error on the same stop completes exactly fatal");
	CHECK(hal_backend_ops.dsp_stop(p.h, &job) == HAL_OK);
	CHECK(job_wait(&p, job.token, 5000, &status, NULL));
	CHECK_UINT(status, HAL_FATAL);
	probe_close(&p);
}

/*
 * A completion is never dropped and never blocks the loop (hal.h section 2
 * class (b)). The reader of the core is the uloop thread that this relay runs
 * on, so a full core fd cannot be waited out. The backend holds the record in
 * its queue and writes it after the core reads. The case checks both
 * producers: the job completion of the library, and the one record that the
 * bring-up chain posts. It also checks the class (a) half beside them: a
 * hardware event under the same full fd is still dropped and still counted.
 */
static void test_completion_queue(void)
{
	struct hal_counters before, after;
	struct hal_job region = { 0 };
	struct hal_job first = { 0 };
	struct hal_job second = { 0 };
	struct hal_job start = { 0 };
	int32_t status = -1;
	uint32_t stamp = 0;
	struct probe p;
	int done;

	if (!probe_up(&p)) {
		CASE("the completion queue cases need a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	probe_discard(&p);

	CASE("the core fd fills");
	CHECK(probe_fill(&p) > 0);

	CASE("a library completion the full core fd refuses is retained");
	CHECK(hal_backend_ops.region_apply(p.h, "GBR", &region) == HAL_OK);
	queue_wait(&p, 1, 5000);
	CHECK_UINT(p.h->comp_len, 1);

	CASE("and it reaches the core once the core has read");
	probe_discard(&p);
	CHECK(job_wait(&p, region.token, 5000, &status, NULL));
	CHECK_UINT(status, HAL_OK);
	CHECK_UINT(p.h->comp_len, 0);

	/* The order is the jobs' own. The core matches a completion to a
	 * pending job by token, but a queue that drained newest first would
	 * hand it the results of two jobs the wrong way round, and the second
	 * region would be the tone plan the line keeps. */
	probe_discard(&p);
	CHECK(probe_fill(&p) > 0);

	CASE("two completions queue in the order the jobs finished");
	CHECK(hal_backend_ops.region_apply(p.h, "USA", &first) == HAL_OK);
	queue_wait(&p, 1, 5000);
	CHECK(hal_backend_ops.region_apply(p.h, "GBR", &second) == HAL_OK);
	queue_wait(&p, 2, 5000);
	CHECK_UINT(p.h->comp_len, 2);

	CASE("and the core reads them oldest first");
	probe_discard(&p);
	CHECK(job_wait(&p, second.token, 5000, NULL, NULL));
	CHECK(job_done_index(&p, first.token) >= 0);
	CHECK(job_done_index(&p, first.token) <
	      job_done_index(&p, second.token));

	probe_discard(&p);
	CHECK(probe_fill(&p) > 0);

	CASE("the bring-up completion the chain posts is retained too");
	CHECK(hal_backend_ops.dsp_start(p.h, &start) == HAL_OK);
	queue_wait(&p, 1, 5000);
	CHECK_UINT(p.h->comp_len, 1);
	stamp = p.h->comp[p.h->comp_head].stamp_ms;

	/* The stamp is the moment the completion was made, and the queue is
	 * not part of that moment: a record restamped on the way out would
	 * date the bring-up from whenever the core got round to reading. */
	CASE("and the core reads it with the status and stamp it was made with");
	status = -1;
	probe_discard(&p);
	CHECK(job_wait(&p, start.token, 5000, &status, NULL));
	CHECK_UINT(status, HAL_OK);
	CHECK_UINT(p.h->comp_len, 0);
	done = job_done_index(&p, start.token);
	CHECK(done >= 0);
	if (done >= 0)
		CHECK_UINT(p.ev[done].stamp_ms, stamp);

	/* The other half of the rule. A hook edge under the same full fd is a
	 * class (a) record: it is dropped and counted, and nothing holds it. */
	probe_discard(&p);
	hal_backend_ops.counters_get(p.h, &before);
	CHECK(probe_fill(&p) > 0);
	udsp_slic_fake_hook_set(p.h->s->slic, 0, true);
	uloop_pump(300);
	hal_backend_ops.counters_get(p.h, &after);

	CASE("a hardware event on the same full fd is dropped and counted");
	CHECK(after.events_dropped > before.events_dropped);
	/* The two loss classes have two counters, so a class (a) drop
	 * reaches events_dropped and completions_lost stands still. */
	CHECK_UINT(after.completions_lost, before.completions_lost);

	probe_discard(&p);
	probe_wait(&p, 500);

	CASE("and nothing replays it once the core has read");
	CHECK(hook_find(&p, HAL_HOOK_SEIZE) == NULL);

	probe_down(&p);
}

/*
 * A hardware event never takes the room that a completion waits for. The
 * reader of the core and this relay run on one thread, so room on the core fd
 * goes to whoever writes next. A hardware record that took the room would
 * hold a job until the 30 s backstop of the core. A burst of them would hold
 * it there for good. The forward path therefore drains the queue first and
 * drops its own record while anything is queued, as the emitter of libudsp
 * does.
 *
 * Both cases remove the watcher, so the queue moves only through the path
 * under test and no race between two ready fds decides the answer.
 */
static void test_completion_priority(void)
{
	struct hal_counters before, after;
	struct hal_job first = { 0 };
	struct hal_job second = { 0 };
	struct probe p;

	if (!probe_up(&p)) {
		CASE("the priority cases need a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	probe_discard(&p);
	CASE("the core fd fills for the priority cases");
	CHECK(probe_fill(&p) > 0);

	CHECK(hal_backend_ops.region_apply(p.h, "GBR", &first) == HAL_OK);
	queue_wait(&p, 1, 5000);
	CHECK_UINT(p.h->comp_len, 1);
	probe_watch_drop(&p);

	/* Room for both records, and a hardware event that arrives wanting
	 * it: the completion has to be the one in front. */
	CASE("the queued completion takes the freed room before a hardware event");
	CHECK_UINT(probe_take(&p, 2), 2);
	CHECK(hw_inject(&p, 4242));
	uloop_pump(50);
	CHECK_UINT(p.h->comp_len, 0);
	probe_collect(&p);
	CHECK(job_done_index(&p, first.token) >= 0);
	CHECK(hw_index(&p, 4242) >= 0);
	CHECK(job_done_index(&p, first.token) < hw_index(&p, 4242));

	/* Room for one record and two completions waiting: the queue cannot
	 * empty, so the hardware event is dropped and counted instead. */
	p.ev_len = 0;
	CHECK(probe_fill(&p) > 0);
	CHECK(hal_backend_ops.region_apply(p.h, "USA", &first) == HAL_OK);
	queue_wait(&p, 1, 5000);
	CHECK(hal_backend_ops.region_apply(p.h, "GBR", &second) == HAL_OK);
	queue_wait(&p, 2, 5000);
	CHECK_UINT(p.h->comp_len, 2);
	probe_watch_drop(&p);
	hal_backend_ops.counters_get(p.h, &before);

	CASE("a hardware event is dropped while the queue is still not empty");
	CHECK_UINT(probe_take(&p, 1), 1);
	CHECK(hw_inject(&p, 4343));
	uloop_pump(50);
	CHECK_UINT(p.h->comp_len, 1);
	hal_backend_ops.counters_get(p.h, &after);
	CHECK(after.events_dropped > before.events_dropped);
	/* The queue held both completions rather than losing one, so the class
	 * (b) counter must not have moved with the class (a) one. */
	CHECK_UINT(after.completions_lost, before.completions_lost);
	probe_collect(&p);
	CHECK(hw_index(&p, 4343) < 0);

	probe_down(&p);
}

/*
 * A watcher that cannot register must not strand the queue. With no watcher
 * and no retry, a queued completion would sit until the 30 s job backstop of
 * the core, although the fd has room. That is the same lost job that the
 * queue exists to prevent. The case makes the registration fail as uloop
 * makes it fail: the fd is already in the loop under another watcher.
 */
static void test_completion_watch_failure(void)
{
	struct uloop_fd blocker = { .cb = pump_nothing };
	struct hal_job region = { 0 };
	int32_t status = -1;
	struct probe p;

	if (!probe_up(&p)) {
		CASE("the watcher failure case needs a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	probe_discard(&p);
	blocker.fd = p.tx_fd;

	CASE("the core fd is already watched, so the backend cannot register");
	CHECK(uloop_fd_add(&blocker, ULOOP_WRITE) == 0);
	CHECK(probe_fill(&p) > 0);
	CHECK(hal_backend_ops.region_apply(p.h, "GBR", &region) == HAL_OK);
	queue_wait(&p, 1, 5000);
	CHECK_UINT(p.h->comp_len, 1);
	CHECK(!p.h->core_out.registered);

	CASE("the retry timer carries the completion the watcher could not");
	probe_discard(&p);
	queue_empty_wait(&p, 3000);
	CHECK_UINT(p.h->comp_len, 0);
	CHECK(job_wait(&p, region.token, 5000, &status, NULL));
	CHECK_UINT(status, HAL_OK);

	uloop_fd_delete(&blocker);
	probe_down(&p);
}

/*
 * The two counters in hal.h struct hal_counters.
 *
 * tdm_stalls is the library's own audio ring diagnostic and this backend only
 * carries it across, so the case injects a known value and reads it back: the
 * engine of a healthy paced run reports no stall at all, and a case that
 * waited for a real one would be waiting for the fault it cannot cause.
 *
 * completions_lost is this backend's own, and it keeps apart the two loss
 * classes of hal.h section 2: a hardware event the core fd refused is class
 * (a), a completion the queue could not hold is class (b), and each loss
 * reaches exactly one counter. The drop cases of test_completion_queue and
 * test_completion_priority pin the class (a) half.
 */
/* The fixture asks for no real time (engine_prio 0, engine_cpu -1), so the
 * outcome an attached backend reports is the host test-mode shape. */
static void test_rt_surfaces(void)
{
	const struct hal_cap *cap;
	struct probe p;

	if (!probe_open(&p)) {
		CASE("the rt case needs an attached backend");
		CHECK(false);
		probe_close(&p);
		return;
	}

	cap = hal_backend_ops.cap_get(p.h);
	CASE("an attached backend reports the host test-mode outcome");
	CHECK_UINT(cap->rt_policy, HAL_RT_POLICY_OTHER);
	CHECK_UINT(cap->rt_net_policy, HAL_RT_POLICY_OTHER);
	CHECK_UINT(cap->rt_prio, 0);
	CHECK_UINT(cap->rt_sched_errno, 0);
	CHECK(cap->rt_cpu == -1);
	CHECK(!cap->rt_mlock);
	probe_close(&p);
}

static void test_counter_surfaces(void)
{
	struct hal_job region[HAL_UDSP_COMP_MAX + 1];
	struct hal_counters before, after;
	struct udsp_counters lib_before, lib_after;
	unsigned int i;
	struct probe p;

	if (!probe_open(&p)) {
		CASE("the counter cases need an attached backend");
		CHECK(false);
		probe_close(&p);
		return;
	}

	/* 0xff first, so a field counters_get never wrote reads as garbage
	 * rather than as the zero the case is looking for: the op fills fields
	 * and the core hands it a record it does not clear. */
	memset(&after, 0xff, sizeof(after));
	hal_backend_ops.counters_get(p.h, &after);

	CASE("a fresh open reports no tdm stall and no lost completion");
	CHECK_UINT(after.tdm_stalls, 0);
	CHECK_UINT(after.completions_lost, 0);
	probe_close(&p);

	if (!probe_up(&p)) {
		CASE("the tdm stall case needs a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	/*
	 * The injection happens after the stop, and that is what makes the
	 * value the case wrote the value counters_get has to report. The
	 * library watchdog republishes the engine's stall count once a second
	 * (udsp.h: the number is up to 1 s stale by design), and it stores
	 * nothing once the stop has taken the engine down (core udsp.c
	 * udsp_watchdog_run returns on a NULL engine). The private atomic is
	 * white box on the p.h->s->slic precedent.
	 */
	probe_stop(&p);
	hal_backend_ops.counters_get(p.h, &before);
	atomic_store_explicit(&p.h->s->tdm_stalls, (uint_least64_t)4242u,
			      memory_order_relaxed);
	hal_backend_ops.counters_get(p.h, &after);

	CASE("the library's tdm stall count reaches hal_counters unchanged");
	CHECK_UINT(after.tdm_stalls, 4242);

	CASE("and a tdm stall is neither an event drop nor a lost completion");
	CHECK_UINT(after.events_dropped, before.events_dropped);
	CHECK_UINT(after.completions_lost, before.completions_lost);
	probe_close(&p);

	if (!probe_up(&p)) {
		CASE("the completion overflow case needs a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	probe_discard(&p);

	CASE("the core fd fills and every job of the overflow run is accepted");
	CHECK(probe_fill(&p) > 0);
	hal_backend_ops.counters_get(p.h, &before);
	udsp_counters(p.h->s, &lib_before);

	/* One job at a time, each one waited into the queue, so nothing stands
	 * between this case and the overflow: every submission finds the one
	 * before it already completed, whatever depth the library's own job
	 * queue has. */
	memset(region, 0, sizeof(region));
	for (i = 0; i < HAL_UDSP_COMP_MAX; i++) {
		CHECK(hal_backend_ops.region_apply(p.h, "USA", &region[i]) ==
		      HAL_OK);
		queue_wait(&p, i + 1, 5000);
	}

	CASE("the completion queue fills to its whole depth");
	CHECK_UINT(p.h->comp_len, HAL_UDSP_COMP_MAX);

	CASE("the completion the full queue cannot hold is counted lost");
	CHECK(hal_backend_ops.region_apply(p.h, "GBR",
					   &region[HAL_UDSP_COMP_MAX]) == HAL_OK);
	lost_wait(&p, before.completions_lost + 1u, 5000);
	hal_backend_ops.counters_get(p.h, &after);
	udsp_counters(p.h->s, &lib_after);
	CHECK_UINT(after.completions_lost, before.completions_lost + 1u);

	/* The core fd stays full for the whole window, so every hardware event
	 * the library posted in it is dropped and counted by the class (a)
	 * rule of hal.h section 2. Those are the only drops the window may
	 * carry: the lost completion adds none. */
	CASE("and the lost completion never reaches the class (a) drop count");
	CHECK(after.events_dropped - before.events_dropped <=
	      lib_after.events_sent - lib_before.events_sent);

	CASE("and the queue still holds every completion it accepted");
	CHECK_UINT(p.h->comp_len, HAL_UDSP_COMP_MAX);

	probe_discard(&p);
	queue_empty_wait(&p, 5000);
	probe_down(&p);
}

/*
 * Every media op exists and refuses. line/line.c and call.c call them
 * without consulting the capability, so a missing entry would be a null
 * dereference on the first call and not a build error.
 */
/*
 * The bring-up includes the region. hal_ops.dsp_start reports ready only once
 * a line can carry a call. An incoming call is a ring with caller identity,
 * which libudsp refuses without a tone plan. The completion that the core
 * reads must therefore arrive with the region already loaded. No window opens
 * between "dsp ready" and the region_apply that line/line.c submits from
 * lines_start.
 */
static void test_bringup_embraces_region(void)
{
	struct hal_ring ring;
	struct probe p;
	bool up;

	up = probe_open(&p) && probe_start(&p);

	CASE("the bring-up completes with the configured region loaded");
	CHECK(up);
	if (!up) {
		probe_down(&p);
		return;
	}

	memset(&ring, 0, sizeof(ring));
	ring.cadence_count = 1;
	ring.cadence[0].on_ms = 2000;
	ring.cadence[0].off_ms = 4000;
	ring.cid_present = true;
	ring.cid.format = HAL_CID_BELLCORE_FSK;
	snprintf(ring.cid.number, sizeof(ring.cid.number), "5551234");

	/* No region_apply of this suite's own: whatever the ring needs has to
	 * be there already, which is the whole point of the chain. */
	CASE("an incoming call ring is accepted the moment the bring-up completes");
	CHECK(hal_backend_ops.line_ring_start(p.h, 0, &ring) == HAL_OK);

	CASE("and so is the tone the machine plays on the other line");
	CHECK(hal_backend_ops.tone_play(p.h, 1, HAL_TONE_DIAL, 0) == HAL_OK);

	probe_down(&p);
}

/*
 * The soft half of the same rule. A tone table that does not load makes a
 * silent phone; a bring-up that fails makes no phone at all. A region whose
 * file and fallback file are both absent therefore still completes the
 * start, and only the tone plan is missing. The empty region directory
 * stands for the open and the start, so the USA fallback of ZZZ is absent
 * too.
 */
static void test_bringup_region_missing(void)
{
	struct hal_ring ring;
	struct probe p;
	bool up;

	CHECK(fixture_write("ZZZ"));
	CHECK(region_dir_enter());
	up = probe_open(&p) && probe_start(&p);
	region_dir_leave();

	CASE("a region with no file or fallback still completes the bring-up");
	CHECK(up);
	if (!up) {
		probe_down(&p);
		CHECK(fixture_write(FIXTURE_REGION_DEFAULT));
		return;
	}

	memset(&ring, 0, sizeof(ring));
	ring.cadence_count = 1;
	ring.cadence[0].on_ms = 2000;
	ring.cid_present = true;

	CASE("only the tone plan is missing, and the line is otherwise up");
	CHECK(hal_backend_ops.line_ring_start(p.h, 0, &ring) == HAL_UNSUPPORTED);
	ring.cid_present = false;
	CHECK(hal_backend_ops.line_ring_start(p.h, 0, &ring) == HAL_OK);
	CHECK(hal_backend_ops.line_ring_stop(p.h, 0) == HAL_OK);

	probe_down(&p);
	CHECK(fixture_write(FIXTURE_REGION_DEFAULT));
}

/*
 * The default board closes the window. voip.base.region carries an ISO 3166
 * alpha-2 name by default ("US", cfg/schema.h). The prefetch map resolves
 * exactly the alpha-2 names that reach a shipped region file. It composes
 * what line/line.c derives with the one spelling on which libudsp differs. The
 * shipped configuration therefore reaches "dsp ready" with the tone plan
 * already loaded, and carries an incoming call at once.
 */
static void test_bringup_default_region(void)
{
	struct hal_ring ring;
	struct probe p;
	bool up;

	CHECK(fixture_write("US"));
	up = probe_open(&p) && probe_start(&p);

	CASE("the default board's alpha-2 region completes the bring-up");
	CHECK(up);
	if (!up) {
		probe_down(&p);
		CHECK(fixture_write(FIXTURE_REGION_DEFAULT));
		return;
	}

	memset(&ring, 0, sizeof(ring));
	ring.cadence_count = 1;
	ring.cadence[0].on_ms = 2000;
	ring.cid_present = true;
	ring.cid.format = HAL_CID_BELLCORE_FSK;
	snprintf(ring.cid.number, sizeof(ring.cid.number), "5551234");

	/* No region_apply of this suite's own: the prefetch is what makes an
	 * alpha-2 configuration carry a call at the completion. */
	CASE("an incoming call ring is carried on the default board at once");
	CHECK(hal_backend_ops.line_ring_start(p.h, 0, &ring) == HAL_OK);
	CHECK(hal_backend_ops.tone_play(p.h, 1, HAL_TONE_DIAL, 0) == HAL_OK);

	probe_down(&p);
	CHECK(fixture_write(FIXTURE_REGION_DEFAULT));
}

/*
 * The remaining gap, recorded and not hidden. An alpha-2 name outside the
 * prefetch rows resolves to nothing at open. The chain then takes its soft
 * path, and the tone plan waits for the region_apply that line/line.c submits,
 * whose own table is the wider one. "DE" is such a name: only line/line.c maps
 * it to DEU, and the region_apply of DEU then loads the ETS fallback
 * (test_region_fallback).
 */
static void test_bringup_unmapped_region(void)
{
	struct hal_ring ring;
	struct probe p;
	bool up;

	CHECK(fixture_write("DE"));
	up = probe_open(&p) && probe_start(&p);

	CASE("an unmapped alpha-2 region still completes the bring-up");
	CHECK(up);
	if (!up) {
		probe_down(&p);
		CHECK(fixture_write(FIXTURE_REGION_DEFAULT));
		return;
	}

	memset(&ring, 0, sizeof(ring));
	ring.cadence_count = 1;
	ring.cadence[0].on_ms = 2000;
	ring.cid_present = true;

	CASE("an unmapped alpha-2 region keeps the soft path until region_apply");
	CHECK(hal_backend_ops.line_ring_start(p.h, 0, &ring) == HAL_UNSUPPORTED);
	CHECK(probe_region(&p, "USA"));
	CHECK(hal_backend_ops.line_ring_start(p.h, 0, &ring) == HAL_OK);

	probe_down(&p);
	CHECK(fixture_write(FIXTURE_REGION_DEFAULT));
}

/*
 * A configured name with no region file loads its fallback at open. The
 * prefetch resolves a three letter name the way region_apply does, so the
 * bring-up carries a call at once. The region_apply that lines_start()
 * submits for the same name adds no second warning.
 */
static void test_bringup_fallback_region(void)
{
	struct hal_region reg;
	struct hal_ring ring;
	struct probe p;
	bool up;

	CHECK(fixture_write("DEU"));
	warn_capture_begin();
	up = probe_open(&p) && probe_start(&p);

	CASE("a configured name with no file completes the bring-up on ETS");
	CHECK(up);
	if (!up) {
		warn_capture_end();
		probe_down(&p);
		CHECK(fixture_write(FIXTURE_REGION_DEFAULT));
		return;
	}
	memset(&reg, 0, sizeof(reg));
	CHECK(hal_backend_ops.region_get(p.h, &reg) == HAL_OK);
	CHECK(!strcmp(reg.iso3, "ETS"));

	/* No region_apply of this suite's own before the ring: the prefetch
	 * is what carries the call at the completion. */
	CASE("an incoming call ring is carried at once on the fallback plan");
	ring_cid_fill(&ring, &reg);
	CHECK(hal_backend_ops.line_ring_start(p.h, 0, &ring) == HAL_OK);
	CHECK(hal_backend_ops.line_ring_stop(p.h, 0) == HAL_OK);

	CASE("the region_apply of the same name adds no second warning");
	CHECK(probe_region(&p, "DEU"));
	CHECK_UINT(warn_capture_count(WARN_DEU_ETS), 1);
	warn_capture_end();

	probe_down(&p);
	CHECK(fixture_write(FIXTURE_REGION_DEFAULT));
}

/*
 * The configured plug-in path really travels. An empty slic_plugin selects
 * the in-tree fake, and every other case runs on it, so a copy that never
 * happened would look exactly like success. A real file that is not a
 * plug-in makes the difference visible. The loader of the library refuses it
 * and the attach fails, which the loader can do only with the path in hand.
 */
static void test_slic_plugin_path(void)
{
	char plugin[192];
	struct probe p;
	FILE *f;

	snprintf(plugin, sizeof(plugin), "%s/notaplugin.so", fixture_dir);
	f = fopen(plugin, "w");
	SUITE_CHECK(f != NULL);
	if (!f)
		return;
	fputs("not an object file\n", f);
	fclose(f);

	CHECK(fixture_write_plugin(FIXTURE_REGION_DEFAULT, plugin));

	CASE("a configured slic plug-in path reaches the library loader");
	CHECK(!probe_open(&p));
	probe_close(&p);

	unlink(plugin);
	CHECK(fixture_write(FIXTURE_REGION_DEFAULT));
}

/*
 * What the config did to the hardware, read off the fake after the stop. The
 * translation cases above prove that the fields reach struct udsp_config.
 * These cases prove that the two fields that describe the board reach the
 * SLIC itself.
 */
static void test_config_reaches_hardware(void)
{
	struct probe p;
	bool up;

	up = probe_up(&p);
	SUITE_CHECK(up);
	if (!up) {
		probe_down(&p);
		return;
	}

	probe_stop(&p);
	fake_take(&p);

	CASE("populated_ports and channels reached the slic, both lines inited");
	CHECK(fake_st[0].inited);
	CHECK(fake_st[1].inited);
	CHECK(fake_st[0].init_count >= 1);
	CHECK(fake_st[1].init_count >= 1);

	CASE("the hook sampler ran on every configured line");
	CHECK(fake_st[0].hook_read_count >= 1);
	CHECK(fake_st[1].hook_read_count >= 1);

	CASE("the dsp stop left every line at the safe linefeed");
	CHECK_UINT(fake_st[0].linefeed, UDSP_LF_OPEN);
	CHECK_UINT(fake_st[1].linefeed, UDSP_LF_OPEN);

	CASE("a line op after the stop answers transient, the engine is gone");
	CHECK(hal_backend_ops.line_state_set(p.h, 0, HAL_LINE_ACTIVE) ==
	      HAL_TRANSIENT);

	probe_close(&p);
}

/*
 * The three translated values that a status code cannot show. A polarity
 * reaches the SLIC as the reverse linefeed, and a lamp reaches it as the lamp
 * op. A type 1 caller identity takes the on-hook transmission linefeed, which
 * a type 2 never touches. Each action sits right in front of the stop, so its
 * journal row survives the hook sampler.
 */
static void test_translated_values(void)
{
	struct hal_cid cid;
	struct probe p;

	if (!probe_up(&p)) {
		CASE("the translated value cases need a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	CHECK(hal_backend_ops.line_state_set(p.h, 0, HAL_LINE_ACTIVE) == HAL_OK);
	CHECK(hal_backend_ops.line_polarity_set(p.h, 0, HAL_POL_REVERSE) == HAL_OK);
	CHECK(hal_backend_ops.line_mwi_set(p.h, 0, HAL_MWI_ON) == HAL_OK);
	probe_stop(&p);
	fake_take(&p);

	CASE("a reversed polarity reached the slic as the reverse linefeed");
	CHECK(fake_jr_find(UDSP_SLIC_FAKE_LINEFEED_SET, 0, UDSP_LF_REV_ACTIVE) >= 0);

	CASE("the lamp op carried the on state");
	CHECK(fake_jr_find(UDSP_SLIC_FAKE_MWI_LAMP, 0, 1) >= 0);
	CHECK(fake_st[0].mwi_on);
	CHECK(fake_st[0].mwi_count >= 1);

	CASE("the other line was never touched by either op");
	CHECK(fake_jr_find(UDSP_SLIC_FAKE_MWI_LAMP, 1, -1) < 0);
	CHECK(!fake_st[1].mwi_on);

	probe_close(&p);

	/* The kind is the second half: a type 1 record is sent on hook, so the
	 * sequence takes the line to the on-hook transmission linefeed, and a
	 * type 2 record travels a live call and never moves it. */
	if (!probe_up(&p)) {
		CASE("the cid kind case needs a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	memset(&cid, 0, sizeof(cid));
	cid.format = HAL_CID_BELLCORE_FSK;
	snprintf(cid.number, sizeof(cid.number), "5551234");

	CHECK(hal_backend_ops.line_cid_send(p.h, 0, HAL_CID_TYPE1, &cid) == HAL_OK);
	probe_wait(&p, 300);
	probe_stop(&p);
	fake_take(&p);

	CASE("a type 1 caller identity took the on-hook transmission linefeed");
	CHECK(fake_jr_find(UDSP_SLIC_FAKE_LINEFEED_SET, 0, UDSP_LF_FWD_OHT) >= 0);

	probe_close(&p);
}

/*
 * The pure media translations. The codec and the fax mode are the two enum
 * pairs that the contracts do not share. A cast would be wrong in both
 * directions, and only a case that drives the map can show that the switch is
 * right.
 */
static void test_media_translation(void)
{
	struct udsp_media dst;
	struct hal_media src;

	CASE("every codec hal.h names maps onto the library member or nothing");
	CHECK(hal_udsp_codec_sel(HAL_CODEC_PCMU) == UDSP_SEL_PCMU);
	CHECK(hal_udsp_codec_sel(HAL_CODEC_PCMA) == UDSP_SEL_PCMA);
	CHECK(hal_udsp_codec_sel(HAL_CODEC_G729) == UDSP_SEL_G729);
	CHECK(hal_udsp_codec_sel(HAL_CODEC_T38) == UDSP_SEL_T38);
	CHECK(hal_udsp_codec_sel(HAL_CODEC_G722) == UDSP_SEL_G722);
	CHECK(hal_udsp_codec_sel(HAL_CODEC_OPUS) == UDSP_SEL_OPUS);

	/* The two that a cast would get wrong: hal.h numbers G726_32 fourth and
	 * the library numbers it seventh, T38 fifth against ninth. */
	CASE("the two codecs the numbering disagrees on take the map, not a cast");
	CHECK(hal_udsp_codec_sel(HAL_CODEC_G726_32) == UDSP_SEL_G726_32);
	CHECK((int)HAL_CODEC_G726_32 != UDSP_SEL_G726_32);
	CHECK((int)HAL_CODEC_T38 != UDSP_SEL_T38);

	CASE("a codec with no library member answers nothing, never a member");
	CHECK(hal_udsp_codec_sel(HAL_CODEC_NONE) < 0);
	CHECK(hal_udsp_codec_sel(HAL_CODEC_VBD_PCMU) < 0);
	CHECK(hal_udsp_codec_sel(HAL_CODEC_VBD_PCMA) < 0);
	CHECK(hal_udsp_codec_sel(200) < 0);

	CASE("the fax modes map and the v.152 mode has no member at all");
	CHECK(hal_udsp_fax_mode(HAL_FAX_OFF) == UDSP_FAX_OFF);
	CHECK(hal_udsp_fax_mode(HAL_FAX_PASSTHROUGH) == UDSP_FAX_PASSTHROUGH);
	CHECK(hal_udsp_fax_mode(HAL_FAX_T38) == UDSP_FAX_T38);
	CHECK((int)HAL_FAX_T38 != UDSP_FAX_T38);
	CHECK(hal_udsp_fax_mode(HAL_FAX_VBD) < 0);
	CHECK(hal_udsp_fax_mode(200) < 0);

	/* Distinctive values in every field, so a copy that crossed two fields
	 * cannot read as correct. */
	memset(&src, 0, sizeof(src));
	src.local.ipver = 4;
	src.local.addr[0] = 10;
	src.local.addr[3] = 7;
	src.local.port = 5004;
	src.remote.ipver = 4;
	src.remote.addr[0] = 192;
	src.remote.addr[3] = 9;
	src.remote.port = 6006;
	src.remote.rtcp_port = 6011;
	src.codec = HAL_CODEC_PCMA;
	src.codec_pt = 8;
	src.dtmf_pt = 101;
	src.dtmf_mode = HAL_DTMF_RFC2833;
	src.direction = HAL_MEDIA_RECVONLY;
	src.ptime_ms = 30;
	src.jb_mode = HAL_JB_FIXED;
	src.vad = true;
	src.cng = true;
	src.rtcp_enable = true;
	src.rtp_dscp = 46;
	src.rtcp_dscp = 34;
	src.jb_init_ms = 60;
	src.jb_max_ms = 140;
	src.rtcp_interval_ms = 7000;
	src.punch_interval_ms = 4500;
	snprintf(src.bind_dev, sizeof(src.bind_dev), "br-voice");
	src.t38.max_bit_rate = 14400;
	src.t38.max_datagram = 272;
	src.t38.ec = HAL_T38_EC_REDUNDANCY;
	src.opus.bitrate = 32000;
	src.opus.complexity = 7;
	/* the hal vocabulary on the hal record and the library's on the library
	 * one, so the copy crosses two enums and not one number */
	src.opus.max_bandwidth = HAL_OPUS_BW_WB;
	src.opus.fec = true;
	src.tx_silence = true;
	/* the kind the two contracts pin against each other by name, so the
	 * fax one proves the byte and not merely a non-zero */
	src.passthrough = HAL_PASSTHROUGH_FAX;
	/* the one field whose name differs on the two sides: hal.h calls the
	 * codec-native suppression annexb and libudsp calls it dtx */
	src.annexb = true;

	hal_udsp_media_fill(&dst, &src);

	CASE("both endpoints reach the library record with their address bytes");
	CHECK_UINT(dst.local.ipver, 4);
	CHECK(!memcmp(dst.local.addr, src.local.addr, sizeof(dst.local.addr)));
	CHECK_UINT(dst.local.port, 5004);
	CHECK_UINT(dst.remote.ipver, 4);
	CHECK(!memcmp(dst.remote.addr, src.remote.addr, sizeof(dst.remote.addr)));
	CHECK_UINT(dst.remote.port, 6006);

	/* The rtcp port is derived where the caller named none, so the library
	 * and the socket the backend binds agree on one number. */
	CASE("an unnamed rtcp port becomes the rtp port plus one, a named one stands");
	CHECK_UINT(dst.local.rtcp_port, 5005);
	CHECK_UINT(dst.remote.rtcp_port, 6011);

	CASE("every scalar of the description is copied unchanged");
	CHECK_UINT(dst.codec, UDSP_SEL_PCMA);
	CHECK_UINT(dst.codec_pt, 8);
	CHECK_UINT(dst.dtmf_pt, 101);
	CHECK_UINT(dst.dtmf_mode, UDSP_DTMF_RFC2833);
	CHECK_UINT(dst.direction, UDSP_MEDIA_RECVONLY);
	CHECK_UINT(dst.ptime_ms, 30);
	CHECK_UINT(dst.jb_mode, UDSP_JB_FIXED);
	CHECK(dst.vad && dst.cng && dst.rtcp_enable);
	CHECK_UINT(dst.rtp_dscp, 46);
	CHECK_UINT(dst.rtcp_dscp, 34);
	CHECK_UINT(dst.jb_init_ms, 60);
	CHECK_UINT(dst.jb_max_ms, 140);
	CHECK_UINT(dst.rtcp_interval_ms, 7000);
	CHECK(!strcmp(dst.bind_dev, "br-voice"));
	/* the hold silence, which the library spells the same way */
	CHECK(dst.tx_silence);

	/* The media punch cadence (hal.h, hal_media.punch_interval_ms), which
	 * the library spells the same way: it must cross as it stands, because
	 * the call layer derived it from the conntrack bound of the moment. */
	CASE("the media punch cadence reaches the library record unchanged");
	CHECK_UINT(dst.punch_interval_ms, 4500);
	/* and the passthrough kind, which it spells the same way too */
	CHECK_UINT(dst.passthrough, UDSP_PASSTHROUGH_FAX);

	/* The rename that carries the negotiated Annex B into the library. This
	 * translation states the negotiation and settles nothing else: the
	 * generic pair above still reads as the caller set it, and the library
	 * is what forces vad and cng off behind an armed dtx. */
	CASE("the negotiated annexb reaches the library as its dtx flag");
	CHECK(dst.dtx);
	src.annexb = false;
	hal_udsp_media_fill(&dst, &src);
	CHECK(!dst.dtx);
	CHECK(dst.vad && dst.cng);
	src.annexb = true;
	hal_udsp_media_fill(&dst, &src);

	CASE("the t38 block travels whole");
	CHECK_UINT(dst.t38.max_bit_rate, 14400);
	CHECK_UINT(dst.t38.max_datagram, 272);
	CHECK_UINT(dst.t38.ec, UDSP_T38_EC_REDUNDANCY);

	CASE("the Opus parameter block reaches the library before the SRTP return");
	CHECK_UINT(dst.opus.bitrate, 32000);
	CHECK_UINT(dst.opus.complexity, 7);
	CHECK_UINT(dst.opus.max_bandwidth, UDSP_OPUS_BW_WB);
	CHECK(dst.opus.fec);

	/* hal.h: the two keys are meaningless on a plain stream and a backend
	 * must not read them there. */
	CASE("a plain stream carries no key even when the record holds bytes");
	memset(src.srtp.key_tx, 0xa5, sizeof(src.srtp.key_tx));
	memset(src.srtp.key_rx, 0x5a, sizeof(src.srtp.key_rx));
	src.srtp.suite = HAL_SRTP_NONE;
	hal_udsp_media_fill(&dst, &src);
	CHECK_UINT(dst.srtp.suite, UDSP_SRTP_NONE);
	CHECK(dst.srtp.key_tx[0] == 0 && dst.srtp.key_rx[0] == 0);
	CHECK(dst.srtp.key_tx[29] == 0 && dst.srtp.key_rx[29] == 0);

	CASE("a named suite carries both keys whole and in the right direction");
	src.srtp.suite = HAL_SRTP_AES_CM_128_SHA1_80;
	hal_udsp_media_fill(&dst, &src);
	CHECK_UINT(dst.srtp.suite, UDSP_SRTP_AES_CM_128_SHA1_80);
	CHECK(!memcmp(dst.srtp.key_tx, src.srtp.key_tx, sizeof(dst.srtp.key_tx)));
	CHECK(!memcmp(dst.srtp.key_rx, src.srtp.key_rx, sizeof(dst.srtp.key_rx)));

	CASE("the shorter tag suite is the other named one and keeps its number");
	src.srtp.suite = HAL_SRTP_AES_CM_128_SHA1_32;
	hal_udsp_media_fill(&dst, &src);
	CHECK_UINT(dst.srtp.suite, UDSP_SRTP_AES_CM_128_SHA1_32);
	CHECK(!memcmp(dst.srtp.key_tx, src.srtp.key_tx, sizeof(dst.srtp.key_tx)));

	/* Defence in depth: media_validate refuses this one step earlier, and a
	 * suite that was never validated must still be unable to key a stream. */
	CASE("a suite outside the enum keys nothing, whatever the record holds");
	src.srtp.suite = 99;
	hal_udsp_media_fill(&dst, &src);
	CHECK_UINT(dst.srtp.suite, UDSP_SRTP_NONE);
	CHECK(dst.srtp.key_tx[0] == 0 && dst.srtp.key_rx[0] == 0);

	/*
	 * An IPv6 description crosses whole. udsp_addr holds the same sixteen
	 * bytes, so nothing is truncated and no version byte is rewritten on
	 * the way. rtcp_remote has no library field and needs none: this
	 * backend owns the connect, so the RTCP address never leaves it.
	 */
	CASE("a media fill copies ipver 6 and all sixteen address bytes");
	{
		unsigned int i;

		memset(&src, 0, sizeof(src));
		src.local.ipver = 6;
		src.remote.ipver = 6;
		for (i = 0; i < sizeof(src.local.addr); i++) {
			src.local.addr[i] = (uint8_t)(0x20u + i);
			src.remote.addr[i] = (uint8_t)(0x40u + i);
		}
		src.local.port = 5004;
		src.remote.port = 6006;
		src.codec = HAL_CODEC_PCMU;
		src.rtcp_remote.ipver = 6;
		src.rtcp_remote.addr[15] = 9;

		hal_udsp_media_fill(&dst, &src);
		CHECK_UINT(dst.local.ipver, 6);
		CHECK_UINT(dst.remote.ipver, 6);
		CHECK(!memcmp(dst.local.addr, src.local.addr,
			      sizeof(dst.local.addr)));
		CHECK(!memcmp(dst.remote.addr, src.remote.addr,
			      sizeof(dst.remote.addr)));
		CHECK_UINT(dst.local.rtcp_port, 5005);
		CHECK_UINT(dst.remote.rtcp_port, 6007);
	}

	explicit_bzero(&src, sizeof(src));
	explicit_bzero(&dst, sizeof(dst));
}

/*
 * The statistics are copied one for one. Every field gets a value that no
 * other field has, so a copy that crossed two of them cannot read as correct.
 * A scaling would spoil the two floats with no trace.
 */
static void test_stats_translation(void)
{
	struct udsp_media_stats src;
	struct hal_media_stats dst;

	memset(&src, 0, sizeof(src));
	src.packets_sent = 1001;
	src.packets_recv = 1002;
	src.packets_lost = 1003;
	src.packets_discarded = 1004;
	src.loss_rate = 1005;
	src.loss_rate_far = 1006;
	src.jitter_avg = 1007;
	src.jitter_max = 1008;
	src.jitter_far = 1009;
	src.rtt_avg = 1010;
	src.rtt_max = 1011;
	src.bytes_sent = 1012;
	src.bytes_recv = 1013;
	src.mos_lq = 4.25f;
	src.mos_cq = 3.75f;
	src.rfactor = 82;

	hal_udsp_stats_fill(&dst, &src);

	CASE("every counter of the media snapshot lands in its own field");
	CHECK_UINT(dst.packets_sent, 1001);
	CHECK_UINT(dst.packets_recv, 1002);
	CHECK_UINT(dst.packets_lost, 1003);
	CHECK_UINT(dst.packets_discarded, 1004);
	CHECK_UINT(dst.loss_rate, 1005);
	CHECK_UINT(dst.loss_rate_far, 1006);
	CHECK_UINT(dst.jitter_avg, 1007);
	CHECK_UINT(dst.jitter_max, 1008);
	CHECK_UINT(dst.jitter_far, 1009);
	CHECK_UINT(dst.rtt_avg, 1010);
	CHECK_UINT(dst.rtt_max, 1011);
	CHECK_UINT(dst.bytes_sent, 1012);
	CHECK_UINT(dst.bytes_recv, 1013);
	CHECK_UINT(dst.rfactor, 82);

	/* The two decimal presentation is the consumer's, so nothing is rounded here:
	 * a value this layer reshaped could not be told from a measured one. */
	CASE("the two mos values arrive unrounded and unscaled");
	CHECK(dst.mos_lq == 4.25f);
	CHECK(dst.mos_cq == 3.75f);
}

/*
 * The wire mark, on sockets that this case makes itself. Both families also
 * run end to end below, on the descriptors that the backend binds for a
 * stream of that family. This case is the narrow check in front of them, and
 * needs no bound address of its own.
 */
static void test_dscp_mark(void)
{
	int v6 = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	int v6_errno = errno;
	int v4 = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);

	CASE("the two sockets the mark cases need exist");
	CHECK(v4 >= 0);

	if (v4 >= 0) {
		/* DSCP 46 is EF and the TOS byte is 0xb8, which is the 184 the
		 * shipped rtp_tos option carries. */
		CASE("an AF_INET socket takes the dscp as IP_TOS, dscp shifted by two");
		CHECK(hal_udsp_dscp_mark(v4, AF_INET, 46) == 0);
		CHECK_UINT(tos_get(v4, AF_INET), 46 << 2);
		CHECK_UINT(tos_get(v4, AF_INET), 184);

		CASE("the two low bits of the tos byte are never set from a dscp");
		CHECK(hal_udsp_dscp_mark(v4, AF_INET, 63) == 0);
		CHECK_UINT(tos_get(v4, AF_INET), 0xfc);
		CHECK(hal_udsp_dscp_mark(v4, AF_INET, 0) == 0);
		CHECK_UINT(tos_get(v4, AF_INET), 0);
	}

	CASE("an AF_INET6 socket takes the same dscp as IPV6_TCLASS");
	if (v6 >= 0) {
		CHECK(hal_udsp_dscp_mark(v6, AF_INET6, 46) == 0);
		CHECK_UINT(tos_get(v6, AF_INET6), 184);
	} else {
		/* A host with no IPv6 at all cannot answer this. The skip
		 * names the failed step and never reads as a pass. */
		char why[64];

		snprintf(why, sizeof(why), "socket(AF_INET6): %s",
			 strerror(v6_errno));
		tu_skip(why);
	}

	CASE("a descriptor that is not a socket refuses the mark");
	CHECK(hal_udsp_dscp_mark(-1, AF_INET, 46) != 0);

	if (v4 >= 0)
		close(v4);
	if (v6 >= 0)
		close(v6);
}

/*
 * One stream, end to end, through the real library and a real loopback pair.
 * The far end checks the wire, and the two port helpers check the ownership
 * of the descriptors.
 */
static void test_media_stream(void)
{
	struct wire_seen seen;
	struct hal_media m;
	struct probe p;
	struct wire w;
	int rtp_fd;
	int rtcp_fd;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the media stream case needs a started backend and a wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);
	wire_local_release(&w);

	CASE("a stream configures through the library with backend-owned sockets");
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);

	rtp_fd = stream_rec(&p, 0, 0)->rtp_fd;
	rtcp_fd = stream_rec(&p, 0, 0)->rtcp_fd;

	CASE("the backend owns both descriptors and bound the advertised pair");
	CHECK(fd_live(rtp_fd) && fd_live(rtcp_fd));
	CHECK_UINT(sock_port(rtp_fd), w.local);
	CHECK_UINT(sock_port(rtcp_fd), w.local + 1u);

	/* The wire mark on the socket that actually carries the media, which is
	 * the half the pure mark case cannot reach. */
	CASE("both descriptors carry the dscp of the description as IP_TOS");
	CHECK_UINT(tos_get(rtp_fd, AF_INET), 184);
	CHECK_UINT(tos_get(rtcp_fd, AF_INET), 184);

	/*
	 * The DSCP record, the part a case can reach. The record must say what
	 * is on the socket and not what was asked for, because the same-value
	 * early return of media_tos_apply reads it back: a value recorded for a
	 * mark that failed would be read as "already applied" and never
	 * retried.
	 * The unmarked state is a sentinel outside the 0..63 a DSCP can hold,
	 * so it can never collide with a value some description asked for.
	 *
	 * The failure branch itself is not reachable from this suite: a
	 * setsockopt IP_TOS on a bound UDP socket does not fail here, so
	 * "recorded only on success" is verified by inspection and the shape
	 * below is what a case can hold.
	 */
	CASE("the recorded mark is the value that reached the socket");
	CHECK_UINT(stream_rec(&p, 0, 0)->sock_rtp_dscp, 46);
	CHECK_UINT(stream_rec(&p, 0, 0)->sock_rtcp_dscp, 46);

	CASE("the unmarked sentinel cannot be mistaken for a dscp a caller asked for");
	CHECK(HAL_UDSP_DSCP_NONE > 63);

	/* hal.h stream_start: the daemon holds the sockets open for the whole
	 * life of the stream, so the port cannot come back while it runs. */
	CASE("the media ports are held for the life of the stream");
	CHECK(!port_free(w.family, w.local));
	CHECK(!port_free(w.family, (uint16_t)(w.local + 1u)));

	CASE("the stream starts");
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_OK);

	probe_wait(&p, 300);

	CASE("rtp reaches the far end on the socket the backend bound");
	wire_drain(&w, &seen);
	CHECK(seen.packets > 0);

	CASE("the packets carry the negotiated payload type and ptime");
	CHECK_UINT(seen.last_pt, 0);
	CHECK_UINT(seen.of_pt[0], seen.packets);
	CHECK_UINT(seen.last_len, WIRE_RTP_HDR + 20u * WIRE_SAMPLES_PER_MS);

	CASE("the first transmission is reported to the core");
	CHECK(ev_find(&p, HAL_EVENT_RTP_FIRST_TX) != NULL);

	CASE("a packet from the far end is taken and reported");
	wire_talk(&p, &w, 0, 20);
	probe_wait(&p, 300);
	CHECK(ev_find(&p, HAL_EVENT_RTP_FIRST_RX) != NULL);

	CASE("the stop is accepted");
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);

	/*
	 * udsp.h section 3: the library drops its dups before the stop returns,
	 * so both answers below hold the moment the op comes back. The port
	 * half is the one a reused descriptor number cannot fool.
	 */
	CASE("the originals close behind the library stop, and the record clears");
	CHECK(!fd_live(rtp_fd));
	CHECK(!fd_live(rtcp_fd));
	CHECK(stream_rec(&p, 0, 0)->rtp_fd == -1);
	CHECK(stream_rec(&p, 0, 0)->rtcp_fd == -1);

	CASE("and the media ports come back, so neither side leaked a dup");
	CHECK(port_free(w.family, w.local));
	CHECK(port_free(w.family, (uint16_t)(w.local + 1u)));

	/* A released pair carries no mark, so the next pair on this stream
	 * marks from scratch instead of trusting a number the old socket had. */
	CASE("the released pair goes back to the unmarked sentinel");
	CHECK_UINT(stream_rec(&p, 0, 0)->sock_rtp_dscp, HAL_UDSP_DSCP_NONE);
	CHECK_UINT(stream_rec(&p, 0, 0)->sock_rtcp_dscp, HAL_UDSP_DSCP_NONE);

	CASE("a stopped stream must be configured again before it can start");
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_FATAL);

	wire_close(&w);
	probe_down(&p);
}

/*
 * The same stream over ::1. libudsp is family agnostic and creates no socket
 * of its own (udsp.h section 3), so the backend decides everything that
 * depends on the family. It opens the pair in AF_INET6, refuses an IPv4
 * mapped source with IPV6_V6ONLY, marks the pair with IPV6_TCLASS and
 * connects it there.
 *
 * The whole group runs behind one tu_v6_loopback() probe. A host with no IPv6
 * loopback skips every case by name, with the reason that the probe gave, so
 * the count holds either way. The group must run in the namespace leg.
 */
static void test_media_v6_stream(void)
{
	struct wire_seen seen;
	const char *why = NULL;
	struct hal_media m;
	char reason[96];
	struct probe p;
	struct wire w;
	struct wire w2;
	int rtp_fd = -1;
	int rtcp_fd = -1;

	if (!tu_v6_loopback(reason, sizeof(reason)))
		why = reason;

	if (!why && (!probe_up(&p) || !wire_open6(&w) || !wire_open6(&w2))) {
		CASE("the v6 media stream case needs a started backend and "
		     "two wires");
		CHECK(false);
		probe_down(&p);
		return;
	}

	CASE("a v6 stream configures through the library with backend-owned "
	     "v6 sockets");
	if (v6_run(why)) {
		media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);
		CHECK_UINT(m.local.ipver, 6);
		CHECK_UINT(m.remote.ipver, 6);
		wire_local_release(&w);
		CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) ==
		      HAL_OK);
		rtp_fd = stream_rec(&p, 0, 0)->rtp_fd;
		rtcp_fd = stream_rec(&p, 0, 0)->rtcp_fd;
	}

	CASE("the backend owns both v6 descriptors and bound the advertised "
	     "pair");
	if (v6_run(why)) {
		CHECK(fd_live(rtp_fd) && fd_live(rtcp_fd));
		CHECK_UINT(sock_port(rtp_fd), w.local);
		CHECK_UINT(sock_port(rtcp_fd), w.local + 1u);
		CHECK(peer_is(rtp_fd, "::1", w.port));
		CHECK(peer_is(rtcp_fd, "::1", (uint16_t)(w.port + 1u)));
	}

	/* The two options take the same byte, so the EF code point of the
	 * description reads back as the same 184 an AF_INET pair carries. */
	CASE("both v6 descriptors carry the dscp of the description as "
	     "IPV6_TCLASS");
	if (v6_run(why)) {
		CHECK_UINT(tos_get(rtp_fd, AF_INET6), 184);
		CHECK_UINT(tos_get(rtcp_fd, AF_INET6), 184);
		CHECK_UINT(stream_rec(&p, 0, 0)->sock_rtp_dscp, 46);
		CHECK_UINT(stream_rec(&p, 0, 0)->sock_rtcp_dscp, 46);
	}

	/*
	 * A mapped peer would be an address the description never named, and
	 * rule 4 refuses a description that names two families.
	 *
	 * The wildcard half is what holds the backend to it. The kernel sets
	 * the flag by itself on every bind to a real IPv6 address, so a pair on
	 * ::1 reads 1 whatever this file does; a pair bound to :: reads 1 only
	 * because this backend asked for it, and :: is the local address a held
	 * leg carries.
	 */
	CASE("IPV6_V6ONLY is set on both v6 media sockets");
	if (v6_run(why)) {
		uint8_t named[16];

		CHECK_UINT(v6only_get(rtp_fd), 1);
		CHECK_UINT(v6only_get(rtcp_fd), 1);

		memcpy(named, m.local.addr, sizeof(named));
		memset(m.local.addr, 0, sizeof(m.local.addr));
		CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) ==
		      HAL_OK);
		CHECK_UINT(v6only_get(stream_rec(&p, 0, 0)->rtp_fd), 1);
		CHECK_UINT(v6only_get(stream_rec(&p, 0, 0)->rtcp_fd), 1);

		/* Back to the advertised address, so the cases below run on
		 * the pair the description names. */
		memcpy(m.local.addr, named, sizeof(named));
		CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) ==
		      HAL_OK);
		rtp_fd = stream_rec(&p, 0, 0)->rtp_fd;
		rtcp_fd = stream_rec(&p, 0, 0)->rtcp_fd;
		CHECK(peer_is(rtp_fd, "::1", w.port));
	}

	CASE("rtp reaches the far end over ::1 on the socket the backend "
	     "bound");
	if (v6_run(why)) {
		CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_OK);
		probe_wait(&p, 300);
		wire_drain(&w, &seen);
		CHECK(seen.packets > 0);
		CHECK_UINT(seen.of_pt[0], seen.packets);
		CHECK_UINT(seen.last_len,
			   WIRE_RTP_HDR + 20u * WIRE_SAMPLES_PER_MS);
		CHECK(ev_find(&p, HAL_EVENT_RTP_FIRST_TX) != NULL);
	}

	CASE("a packet from the v6 far end is taken and reported");
	if (v6_run(why)) {
		wire_talk(&p, &w, 0, 20);
		probe_wait(&p, 300);
		CHECK(ev_find(&p, HAL_EVENT_RTP_FIRST_RX) != NULL);
	}

	/*
	 * A hold or a resume arrives as a bare re-program: the far end moves
	 * and the local pair stands, so the same two descriptors reconnect and
	 * the audio follows. The wire is the oracle, because a record that
	 * moved without a connect behind it reads the same as one that did.
	 */
	CASE("a moved v6 far end reconnects the pair");
	if (v6_run(why)) {
		m.remote.port = w2.port;
		CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) ==
		      HAL_OK);
		CHECK_UINT(stream_rec(&p, 0, 0)->rtp_fd, rtp_fd);
		CHECK_UINT(stream_rec(&p, 0, 0)->rtcp_fd, rtcp_fd);
		CHECK(peer_is(rtp_fd, "::1", w2.port));
		CHECK(peer_is(rtcp_fd, "::1", (uint16_t)(w2.port + 1u)));

		wire_drain(&w, NULL);
		wire_drain(&w2, NULL);
		probe_wait(&p, 300);
		wire_drain(&w2, &seen);
		CHECK(seen.packets > 0);
		CHECK_UINT(seen.of_pt[0], seen.packets);
		wire_drain(&w, &seen);
		CHECK_UINT(seen.packets, 0);
	}

	/* hal.h stream_start: the daemon holds the pair open for the whole life
	 * of the stream, so the ports come back only after the stop. */
	CASE("the v6 media ports are held for the life of the stream");
	if (v6_run(why)) {
		CHECK(!port_free(w.family, w.local));
		CHECK(!port_free(w.family, (uint16_t)(w.local + 1u)));
		CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
		CHECK(port_free(w.family, w.local));
		CHECK(port_free(w.family, (uint16_t)(w.local + 1u)));
	}

	if (why)
		return;

	wire_close(&w);
	wire_close(&w2);
	probe_down(&p);
}

/*
 * The own RTCP address of the far end (RFC 3605 section 2.1). IPv4 answers
 * the question: 127.0.0.0/8 is all local, so WIRE_ALT_ADDR is an address that
 * the media endpoint is not. Only a backend that read rtcp_remote sends the
 * report there. The port cannot show it, because rtcp_port_of() already
 * carries a named RTCP port to either address.
 */
static void test_media_rtcp_addr(void)
{
	uint8_t pkt[WIRE_PKT_MAX];
	struct hal_media m;
	struct probe p;
	struct wire w;
	uint16_t alt_port;
	int alt = -1;
	int rtp_fd;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the rtcp address case needs a started backend and a "
		     "wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);
	alt = udp_bind_alt(0);
	alt_port = sock_port(alt);
	/* The port the far end named in its a=rtcp. It travels on
	 * remote.rtcp_port whether or not an address came with it, so the
	 * address is the whole difference between the two descriptions. */
	m.remote.rtcp_port = alt_port;

	/*
	 * The stream comes up with no RTCP address, so the report goes to the
	 * media address on that port. Then the address alone moves: both
	 * endpoints and both ports stand, so a backend that compared the media
	 * endpoint alone would answer HAL_OK and keep the old peer.
	 */
	CASE("a moved rtcp_remote alone reconnects the pair");
	CHECK(alt >= 0);
	CHECK(alt_port != 0);
	CHECK(media_up(&p, &w, &m));
	rtp_fd = stream_rec(&p, 0, 0)->rtp_fd;
	CHECK(peer_is(stream_rec(&p, 0, 0)->rtcp_fd, "127.0.0.1", alt_port));
	m.rtcp_remote.ipver = 4;
	CHECK(inet_pton(AF_INET, WIRE_ALT_ADDR, m.rtcp_remote.addr) == 1);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(peer_is(stream_rec(&p, 0, 0)->rtcp_fd, WIRE_ALT_ADDR, alt_port));
	/* An RTCP move is not a local move, so the same pair carries it. */
	CHECK_UINT(stream_rec(&p, 0, 0)->rtp_fd, rtp_fd);

	/*
	 * The wire half. The library clamps the interval to UDSP_RTCP_MIN_MS,
	 * which is 5000 engine milliseconds, and the first report goes out one
	 * interval after the stream started.
	 */
	CASE("the rtcp descriptor connects to rtcp_remote when the peer named "
	     "one");
	CHECK(peer_is(rtp_fd, "127.0.0.1", w.port));
	probe_wait(&p, 6000);
	CHECK(wire_recv_bytes(alt, pkt, sizeof(pkt)) > 0);

	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	if (alt >= 0)
		close(alt);
	wire_close(&w);
	probe_down(&p);
}

/*
 * The negotiated Annex B, from a hal_media that the call layer could build to
 * the description that the library settled on. The pure translation case
 * cannot make this round: hal_udsp_media_fill() states the negotiation, and
 * media_prepare() of the library applies the forcing rules. Only a real
 * configure therefore shows the pair.
 *
 * The case reads the prepared description off the library object directly,
 * as the descriptor cases read the stream record. Nothing else publishes it,
 * and the read runs on the control thread that just wrote it.
 */
static void test_media_dtx(void)
{
	struct hal_media m;
	struct probe p;
	struct wire w;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the annex B case needs a started backend and a wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	media_build(&m, &w, HAL_CODEC_G729, 18, 20);
	/* A description the call layer does not build for G.729: it clears
	 * vad on an unarmed leg (media_fill). Below that layer the
	 * backend copies what it is given and the library keeps the generic
	 * pair beside its comfort noise. */
	m.vad = true;
	m.cng = true;
	wire_local_release(&w);

	CASE("an unarmed G.729 stream keeps the generic suppression pair");
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(!stream_at(p.h->s, 0, 0)->media.dtx);
	CHECK(stream_at(p.h->s, 0, 0)->media.vad);
	CHECK(stream_at(p.h->s, 0, 0)->media.cng);

	CASE("an armed one reaches the library as dtx and takes that pair down");
	m.annexb = true;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(stream_at(p.h->s, 0, 0)->media.dtx);
	CHECK(!stream_at(p.h->s, 0, 0)->media.vad);
	CHECK(!stream_at(p.h->s, 0, 0)->media.cng);

	/* udsp core/stream.c media_prepare: the flag means nothing on a codec
	 * with no SID frame of its own, so it is cleared rather than stored and
	 * the generic pair stands. */
	CASE("the same description on G.711 clears it and keeps the generic pair");
	m.codec = HAL_CODEC_PCMU;
	m.codec_pt = 0;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(!stream_at(p.h->s, 0, 0)->media.dtx);
	CHECK(stream_at(p.h->s, 0, 0)->media.vad);
	CHECK(stream_at(p.h->s, 0, 0)->media.cng);

	wire_close(&w);
	probe_down(&p);
}

/*
 * The re-INVITE. call.c calls stream_config_set again on a live stream with
 * no stop before it, so this case never stops one. It moves the far end and
 * the mark under a running stream, and asks the two wires which one the audio
 * went to.
 */
static void test_media_reconfig(void)
{
	struct wire_seen seen;
	struct hal_media m;
	struct probe p;
	struct wire w;
	struct wire w2;
	int rtp_fd;
	int rtcp_fd;

	if (!probe_up(&p) || !wire_open(&w) || !wire_open(&w2)) {
		CASE("the reconfiguration case needs a started backend and two wires");
		CHECK(false);
		probe_down(&p);
		return;
	}

	/* The two wires must name two different local pairs, or the local move
	 * below would not be a move at all. The reservation of wire_open is
	 * what guarantees it, and this is the assert on that guarantee. */
	CASE("the two wires reserved two different local port pairs");
	CHECK(w.local != w2.local);
	CHECK(w.local + 1u != w2.local && w2.local + 1u != w.local);

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);

	CASE("the stream comes up on the first far end");
	CHECK(media_up(&p, &w, &m));
	rtp_fd = stream_rec(&p, 0, 0)->rtp_fd;
	rtcp_fd = stream_rec(&p, 0, 0)->rtcp_fd;
	probe_wait(&p, 300);
	wire_drain(&w, &seen);
	CHECK(seen.packets > 0);

	/* The same local pair, a new far end and a new mark: the hold and
	 * resume shape, which arrives as a bare reconfiguration. */
	m.remote.port = w2.port;
	m.codec = HAL_CODEC_PCMA;
	m.codec_pt = 8;
	m.rtp_dscp = 26;
	m.rtcp_dscp = 26;

	CASE("a running stream reconfigures with no stop in front of it");
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);

	CASE("the same descriptors carry it, because the local pair did not move");
	CHECK_UINT(stream_rec(&p, 0, 0)->rtp_fd, rtp_fd);
	CHECK_UINT(stream_rec(&p, 0, 0)->rtcp_fd, rtcp_fd);
	CHECK(fd_live(rtp_fd) && fd_live(rtcp_fd));

	CASE("the new mark reaches the descriptors the library already dup'd");
	CHECK_UINT(tos_get(rtp_fd, AF_INET), 26 << 2);
	CHECK_UINT(tos_get(rtcp_fd, AF_INET), 26 << 2);

	wire_drain(&w, NULL);
	wire_drain(&w2, NULL);
	probe_wait(&p, 300);

	CASE("the audio follows the far end to its new port");
	wire_drain(&w2, &seen);
	CHECK(seen.packets > 0);

	CASE("and the codec the reconfiguration named is what is on the wire");
	CHECK_UINT(seen.of_pt[8], seen.packets);

	CASE("the old far end hears nothing more");
	wire_drain(&w, &seen);
	CHECK_UINT(seen.packets, 0);

	/*
	 * libudsp never binds anything, so hal_media.bind_dev reaches the
	 * wire only through the SO_BINDTODEVICE this backend set when it made
	 * the socket. A description that changed the device alone therefore has
	 * to take a new pair, or the stream would keep sending out of the old
	 * netdev for the rest of the call with the status still HAL_OK.
	 *
	 * The device named here does not exist, so the setsockopt fails and is
	 * traced; what is under test is the identity decision in front of it,
	 * which is why the assert is on the descriptors and not on the wire.
	 */
	CASE("a bind device change alone takes a new pair, like any local move");
	{
		int before = stream_rec(&p, 0, 0)->rtp_fd;
		uint16_t port = sock_port(before);

		snprintf(m.bind_dev, sizeof(m.bind_dev), "uvoice-nodev0");
		CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
		/* The port is the oracle, not the number: the close and the
		 * socket() that replaces it run inside one op, so the number
		 * comes straight back. */
		CHECK_UINT(sock_port(stream_rec(&p, 0, 0)->rtp_fd), port);
		CHECK(!memcmp(stream_rec(&p, 0, 0)->sock_bind_dev, m.bind_dev,
			      sizeof(m.bind_dev)));

		CASE("and the same description again keeps the pair it just made");
		{
			int kept = stream_rec(&p, 0, 0)->rtp_fd;

			CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) ==
			      HAL_OK);
			CHECK_UINT(stream_rec(&p, 0, 0)->rtp_fd, kept);
		}

		memset(m.bind_dev, 0, sizeof(m.bind_dev));
		CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
		CHECK_UINT(stream_rec(&p, 0, 0)->sock_bind_dev[0], 0);
	}

	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	wire_close(&w);
	wire_close(&w2);
	probe_down(&p);
}

/*
 * The local move, as a case of its own and not a tail of the reconfiguration
 * case, on its own probe and wires. The property is that a running stream is
 * still running after the move. The case owns its running state, because an
 * inherited state would let a defect in the restart cancel out. The first
 * inherited move would leave the stream stopped. The next move would find
 * nothing running to put back, and the check would pass on a backend that
 * lost the restart.
 *
 * A local address or port move stops the stream to rebind (hal.h), and
 * nothing above this layer starts it again. call.c media_program calls
 * stream_start only for a leg whose media has not started yet. A backend that
 * left the stream halted answers HAL_OK, and the call stays silent behind a
 * healthy status that only the wire contradicts.
 */
static void test_media_local_move(void)
{
	struct wire_seen seen;
	struct hal_media m;
	struct probe p;
	struct wire w;
	struct wire w2;

	if (!probe_up(&p) || !wire_open(&w) || !wire_open(&w2)) {
		CASE("the local move case needs a started backend and two wires");
		CHECK(false);
		probe_down(&p);
		return;
	}

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);

	CASE("the stream comes up running on its first local pair");
	CHECK(media_up(&p, &w, &m));
	CHECK(stream_rec(&p, 0, 0)->running);
	probe_wait(&p, 300);
	wire_drain(&w, &seen);
	CHECK(seen.packets > 0);

	/*
	 * A local move cannot rebind a live socket, so hal.h has that pass stop
	 * the stream, close the pair and make a new one.
	 *
	 * The oracle is the port and never the descriptor number. The close and
	 * the socket() that replaces it happen inside one op, so the new
	 * descriptor takes the number the old one just gave back: a case that
	 * compared numbers would read "the same socket" and be wrong, which is
	 * the reason udsp.h forbids libudsp from comparing them.
	 */
	CASE("a local port move takes a new pair and frees the old one");
	wire_local_release(&w2);
	m.local.port = w2.local;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK_UINT(sock_port(stream_rec(&p, 0, 0)->rtp_fd), w2.local);
	CHECK_UINT(sock_port(stream_rec(&p, 0, 0)->rtcp_fd), w2.local + 1u);
	CHECK(port_free(w.family, w.local));
	CHECK(port_free(w.family, (uint16_t)(w.local + 1u)));

	CASE("the moved stream is still running, with no start from the core");
	CHECK(stream_rec(&p, 0, 0)->running);

	CASE("and it carries media again, out of the new local port");
	wire_drain(&w, NULL);
	probe_wait(&p, 400);
	wire_drain(&w, &seen);
	CHECK(seen.packets > 0);

	/* A stream that was not running when the move happened must not be
	 * started by it: the restart puts back what the halt took, no more. */
	CASE("a move on a configured but stopped stream leaves it stopped");
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	wire_local_release(&w);
	m.local.port = w.local;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(!stream_rec(&p, 0, 0)->running);

	CASE("and it starts from there on the core's own start");
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_OK);
	CHECK(stream_rec(&p, 0, 0)->running);

	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	wire_close(&w);
	wire_close(&w2);
	probe_down(&p);
}

/*
 * RFC 4733 (RFC 2833 as hal.h still spells it). The relay belongs to the
 * library, and this case asks the wire whether the digit left on the
 * telephone-event payload type that the description negotiated.
 */
static void test_media_dtmf(void)
{
	const struct hal_event *dig;
	struct wire_seen seen;
	struct hal_media m;
	struct probe p;
	struct wire w;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the dtmf case needs a started backend and a wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);
	m.dtmf_mode = HAL_DTMF_RFC2833;
	m.dtmf_pt = 101;

	CASE("the stream carrying the relay comes up");
	CHECK(media_up(&p, &w, &m));
	probe_wait(&p, 200);
	wire_drain(&w, NULL);

	CASE("a digit is accepted on a running rfc 2833 stream");
	CHECK(hal_backend_ops.stream_dtmf_send(p.h, 0, 0, '5', 100) == HAL_OK);

	probe_wait(&p, 400);

	/* The audio resumes when the digit ends, so the burst is counted by its
	 * payload type rather than read off the last packet. */
	CASE("the digit leaves as rfc 4733 packets on the negotiated payload type");
	wire_drain(&w, &seen);
	CHECK(seen.packets > 0);
	CHECK(seen.of_pt[101] > 0);

	CASE("the audio payload type is untouched by the digit");
	CHECK_UINT(seen.of_pt[101] + seen.of_pt[0], seen.packets);

	CASE("a digit the event table does not carry is refused");
	CHECK(hal_backend_ops.stream_dtmf_send(p.h, 0, 0, 'Z', 100) ==
	      HAL_UNSUPPORTED);

	CASE("a media op on a line the board does not carry is refused");
	CHECK(hal_backend_ops.stream_dtmf_send(p.h, HAL_LINE_MAX, 0, '1', 100) ==
	      HAL_UNSUPPORTED);

	/*
	 * The other direction of the same payload type. libudsp reports the
	 * event at its end (RFC 4733 section 2.5.1.2) and renders the tone
	 * toward the line itself from the first packet, which is what the
	 * playout bit tells the core: the report is a report, and the core
	 * plays no digit of its own for it.
	 */
	CASE("the backend advertises that it plays a wire digit itself");
	CHECK((hal_backend_ops.cap_get(p.h)->features & HAL_FEAT_DTMF_PLAYOUT) != 0);

	CASE("a telephone-event from the far end is reported when it ends");
	p.ev_len = 0;
	wire_talk(&p, &w, 0, 20);
	wire_send_telev(&w, 101, 5, true, false, 160);
	probe_wait(&p, 60);
	CHECK(ev_find(&p, HAL_EVENT_DIGIT) == NULL);
	wire_send_telev(&w, 101, 5, false, true, 640);
	probe_wait(&p, 200);
	CHECK(ev_find(&p, HAL_EVENT_DIGIT) != NULL);

	CASE("and it names the digit, the line and the wire it came from");
	dig = ev_find(&p, HAL_EVENT_DIGIT);
	CHECK(dig != NULL);
	if (dig) {
		CHECK(dig->u.digit.digit == '5');
		CHECK_UINT(dig->u.digit.remote, 1);
		CHECK_UINT(dig->line, 0);
	}

	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);

	/* hal.h suppresses the relay in the core when the mode is INFO, and the
	 * library refuses it as well: a stream with no telephone-event payload
	 * type has no wire to put the packet on. */
	CASE("a stream that negotiated no relay refuses the digit");
	m.dtmf_mode = HAL_DTMF_INBAND;
	m.dtmf_pt = 0;
	CHECK(media_up(&p, &w, &m));
	CHECK(hal_backend_ops.stream_dtmf_send(p.h, 0, 0, '5', 100) ==
	      HAL_UNSUPPORTED);

	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	wire_close(&w);
	probe_down(&p);
}

/*
 * The SLOW statistics job. libudsp answers from a FASTPATH snapshot, so the
 * backend reads it on its own path and posts the completion. The contract
 * that the core sees does not change, and this case asserts that.
 */
static void test_media_stats(void)
{
	struct hal_media_stats stats;
	struct hal_job job = { 0 };
	struct hal_media m;
	int32_t status = -1;
	struct probe p;
	uint16_t seq = 9;
	struct wire w;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the stats case needs a started backend and a wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);

	CASE("the stream the snapshot describes comes up");
	CHECK(media_up(&p, &w, &m));
	probe_wait(&p, 500);
	wire_talk(&p, &w, 0, 20);
	probe_wait(&p, 500);

	CASE("the slow stats op is accepted and fills a token");
	CHECK(hal_backend_ops.stream_stats_start(p.h, 0, 0, &job) == HAL_OK);
	CHECK(job.token != 0);

	/* The token space of the note in udsp_int.h: the library counts up from
	 * 1 and this backend runs in the half it can never reach. */
	CASE("the backend's own token cannot meet a library job token");
	CHECK(job.token > HAL_UDSP_STATS_TOKEN_BASE);

	CASE("it completes as a job done record, with seq 0 like every completion");
	CHECK(job_wait(&p, job.token, 2000, &status, &seq));
	CHECK_UINT(status, HAL_OK);
	CHECK_UINT(seq, 0);

	CASE("the result reads back with plausible numbers");
	memset(&stats, 0, sizeof(stats));
	CHECK(hal_backend_ops.stream_stats_result_get(p.h, 0, 0, &stats) == HAL_OK);
	CHECK(stats.packets_sent > 0);
	CHECK(stats.bytes_sent > 0);
	CHECK(stats.packets_recv > 0);

	CASE("a second snapshot takes a token of its own");
	{
		struct hal_job job2 = { 0 };

		CHECK(hal_backend_ops.stream_stats_start(p.h, 0, 0, &job2) == HAL_OK);
		CHECK(job2.token != job.token);
		CHECK(job_wait(&p, job2.token, 2000, NULL, NULL));
	}

	/*
	 * The counter is driven to the last value of its half, white box on
	 * the same footing as reading comp_len, so the next token is the one
	 * that wraps. A counter that simply climbed would carry out of the
	 * reserved half here and start colliding with the library's own job
	 * tokens, which count up from 1; 2^31 real snapshots is unreachable in
	 * a test and reachable in a long-lived daemon.
	 */
	CASE("the token counter wraps inside its reserved half, never out of it");
	{
		struct hal_job wrap = { 0 };
		struct hal_job after = { 0 };

		p.h->stats_token = HAL_UDSP_STATS_TOKEN_MASK;
		CHECK(hal_backend_ops.stream_stats_start(p.h, 0, 0, &wrap) == HAL_OK);
		CHECK_UINT(wrap.token, HAL_UDSP_STATS_TOKEN_BASE);
		CHECK(wrap.token >= HAL_UDSP_STATS_TOKEN_BASE);
		/* Never 0, which is what the core reads as "no token". */
		CHECK(wrap.token != 0);
		CHECK(job_wait(&p, wrap.token, 2000, NULL, NULL));

		CASE("and the token after the wrap is still inside the half");
		CHECK(hal_backend_ops.stream_stats_start(p.h, 0, 0, &after) == HAL_OK);
		CHECK(after.token >= HAL_UDSP_STATS_TOKEN_BASE);
		CHECK(after.token != wrap.token);
		CHECK(job_wait(&p, after.token, 2000, NULL, NULL));
	}

	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);

	/*
	 * The snapshot outlives the stream (call-records RR2). line/line.c
	 * takes it in front of the teardown and reads it back after, so a stop
	 * that cleared it would empty every call record.
	 */
	CASE("the snapshot survives the stop it was taken in front of");
	memset(&stats, 0, sizeof(stats));
	CHECK(hal_backend_ops.stream_stats_result_get(p.h, 0, 0, &stats) == HAL_OK);
	CHECK(stats.packets_sent > 0);

	CASE("a stream with nothing behind it answers unsupported, not a token");
	{
		struct hal_job job3 = { 0 };

		CHECK(hal_backend_ops.stream_stats_start(p.h, 0, 0, &job3) ==
		      HAL_UNSUPPORTED);
		CHECK_UINT(job3.token, 0);
	}

	CASE("and a line the board does not carry answers unsupported too");
	{
		struct hal_job job4 = { 0 };

		CHECK(hal_backend_ops.stream_stats_start(p.h, HAL_LINE_MAX, 0,
							 &job4) == HAL_UNSUPPORTED);
		CHECK(hal_backend_ops.stream_stats_result_get(p.h, HAL_LINE_MAX, 0,
							      &stats) ==
		      HAL_UNSUPPORTED);
	}

	wire_close(&w);
	probe_down(&p);
}

/* --------------------------------- the fast diagnostics */

/*
 * The trace capture, the warn-capture idiom of test_cfg.c with the domain
 * turned on for the length of one case: stderr goes to a temporary file,
 * ULOG_STDIO writes there, and both are put back at the first count. The
 * trace line is the only observable of the drain path, because the records
 * leave the library ring and nothing else keeps them.
 */
static FILE *jbtrace_file;
static int jbtrace_stderr_fd = -1;

static void jbtrace_stderr_restore(void)
{
	if (jbtrace_stderr_fd < 0)
		return;

	fflush(stderr);
	ulog_open(ULOG_SYSLOG, LOG_DAEMON, "test_hal_udsp");
	dup2(jbtrace_stderr_fd, STDERR_FILENO);
	close(jbtrace_stderr_fd);
	jbtrace_stderr_fd = -1;
}

static void jbtrace_end(void)
{
	jbtrace_stderr_restore();
	trace_domain_set(TRACE_JB, false);

	if (!jbtrace_file)
		return;

	fclose(jbtrace_file);
	jbtrace_file = NULL;
}

static void jbtrace_begin(void)
{
	jbtrace_end();

	jbtrace_file = tmpfile();
	if (!jbtrace_file)
		return;

	fflush(stderr);
	jbtrace_stderr_fd = dup(STDERR_FILENO);
	dup2(fileno(jbtrace_file), STDERR_FILENO);
	ulog_open(ULOG_STDIO, LOG_DAEMON, NULL);
	ulog_threshold(LOG_DEBUG);
	trace_domain_set(TRACE_JB, true);
}

static unsigned int jbtrace_count(const char *needle)
{
	char line[512];
	unsigned int hits = 0;

	if (!jbtrace_file)
		return 0;

	jbtrace_stderr_restore();

	rewind(jbtrace_file);
	while (fgets(line, sizeof(line), jbtrace_file)) {
		if (strstr(line, needle))
			hits++;
	}

	return hits;
}

/* Unlike jbtrace_count(), this read keeps the capture open. pread() leaves
 * the file offset alone, and stderr shares that offset for its next line. */
static bool jbtrace_seen(const char *needle)
{
	size_t keep = strlen(needle);
	char buf[4096];
	size_t have = 0;
	off_t off = 0;
	ssize_t n;

	if (!jbtrace_file)
		return false;

	fflush(stderr);
	while ((n = pread(fileno(jbtrace_file), buf + have,
			  sizeof(buf) - 1 - have, off)) > 0) {
		off += n;
		have += (size_t)n;
		buf[have] = '\0';
		if (strstr(buf, needle))
			return true;
		if (have <= keep)
			continue;

		memmove(buf, buf + have - keep, keep);
		have = keep;
	}

	return false;
}

static void jbtrace_wait(struct probe *p, const char *const *needles,
			 unsigned int count, unsigned int ms)
{
	uint64_t stop = mono_us() + (uint64_t)ms * 1000u;
	unsigned int i = 0;

	while (i < count && mono_us() < stop) {
		if (jbtrace_seen(needles[i])) {
			i++;
			continue;
		}

		probe_poll(p);
		uloop_pump(5);
	}
}

static void test_fax_plugin_missing(void)
{
	char fax_path[192];
	char log_line[256];
	struct hal_counters counters;
	const struct hal_cap *cap;
	struct probe p;
	bool up;

	CASE("a missing fax plug-in leaves t.38 unavailable");
	snprintf(fax_path, sizeof(fax_path), "%s/missing-fax-plugin.so",
		 fixture_dir);
	CHECK(access(fax_path, F_OK) != 0);
	if (!fixture_write_plugin_fax_rate(FIXTURE_REGION_DEFAULT, "none",
					  fax_path, 8000)) {
		CHECK(false);
		CHECK(fixture_write(FIXTURE_REGION_DEFAULT));
		return;
	}

	jbtrace_begin();
	up = probe_up(&p);
	CHECK(up);
	if (up) {
		cap = hal_backend_ops.cap_get(p.h);
		CHECK(!(cap->features & HAL_FEAT_T38));
		CHECK(!(cap->codecs & (1u << HAL_CODEC_T38)));
		memset(&counters, 0, sizeof(counters));
		hal_backend_ops.counters_get(p.h, &counters);
		CHECK_UINT(counters.op_errors, 1);
	}
	snprintf(log_line, sizeof(log_line), "fax plug-in %s unavailable",
		 fax_path);
	CHECK_UINT(jbtrace_count(log_line), 1);
	/* The library's own line carries the loader's reason; the open
	 * record then names no fax plug-in. */
	CHECK_UINT(jbtrace_count("udsp: fax plug-in: "), 1);
	CHECK_UINT(jbtrace_count("fax plug-in none"), 1);
	jbtrace_end();
	probe_down(&p);

	CHECK(fixture_write(FIXTURE_REGION_DEFAULT));
	jbtrace_begin();
	up = probe_up(&p);
	CHECK(up);
	CHECK_UINT(jbtrace_count("unavailable"), 0);
	CHECK_UINT(jbtrace_count("udsp: fax plug-in: "), 0);
	CHECK_UINT(jbtrace_count("fax plug-in none"), 1);
	jbtrace_end();
	probe_down(&p);
	CHECK(fixture_write(FIXTURE_REGION_DEFAULT));
}

static void test_t38_stream(void)
{
	struct fax_fake_stats *fs = fax_fake_stats_get();
	const struct hal_cap *cap;
	struct hal_media m;
	struct probe p;
	struct wire w = {
		.rtp = -1,
		.rtcp = -1,
		.hold_rtp = -1,
		.hold_rtcp = -1,
	};
	enum hal_status st;
	unsigned int creates;
	bool feature;
	bool codec;

	CASE("a fake fax gateway configures and runs a t.38 stream");
	fax_fake_reset();
	udsp_fax_override(&fax_fake_ops);
	if (!probe_up(&p)) {
		CHECK(false);
		goto out;
	}
	if (!wire_open(&w)) {
		CHECK(false);
		goto out;
	}

	cap = hal_backend_ops.cap_get(p.h);
	feature = (cap->features & HAL_FEAT_T38) != 0;
	codec = (cap->codecs & (1u << HAL_CODEC_T38)) != 0;
	CHECK(feature && codec);

	media_build(&m, &w, HAL_CODEC_T38, 0, 20);
	m.t38.max_bit_rate = 9600;
	m.t38.max_datagram = 400;
	m.t38.ec = HAL_T38_EC_REDUNDANCY;
	wire_local_release(&w);
	st = hal_backend_ops.stream_config_set(p.h, 0, 0, &m);
	CHECK(st == HAL_OK);
	if (st != HAL_OK)
		goto out;
	CHECK(stream_rec(&p, 0, 0)->configured);
	CHECK_UINT(fs->creates, 1);
	CHECK_UINT(fs->bit_rate, 9600);
	CHECK_UINT(fs->datagram, 400);
	CHECK_UINT(fs->ecc, HAL_T38_EC_REDUNDANCY);
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_OK);
	CHECK(stream_rec(&p, 0, 0)->running);
	probe_wait(&p, 20);
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	/* The stop quiesces the engine, so the fake record is stable here. */
	CHECK(fs->rx_frames > 0u);
	CHECK(fs->tx_frames > 0u);
	CHECK_UINT(fs->destroys, 1);

	m.t38.max_datagram = 4000;
	st = hal_backend_ops.stream_config_set(p.h, 0, 0, &m);
	CHECK(st == HAL_OK);
	if (st != HAL_OK)
		goto out;
	CHECK_UINT(fs->creates, 2);
	CHECK_UINT(fs->datagram, 576);
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	CHECK_UINT(fs->destroys, 2);

	creates = fs->creates;
	m.srtp.suite = HAL_SRTP_AES_CM_128_SHA1_80;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) ==
	      HAL_UNSUPPORTED);
	CHECK_UINT(fs->creates, creates);
	CHECK(!stream_rec(&p, 0, 0)->configured);
out:
	udsp_fax_override(NULL);
	wire_close(&w);
	probe_down(&p);
	CHECK_UINT(fs->destroys, fs->creates);
}

static void test_t38_profile(void)
{
	struct hal_fax_profile fax;
	const struct hal_cap *cap;
	struct hal_media m;
	struct probe p;
	struct wire w = {
		.rtp = -1,
		.rtcp = -1,
		.hold_rtp = -1,
		.hold_rtcp = -1,
	};

	CASE("a t.38 profile stays unsupported with a fax gateway");
	fax_fake_reset();
	udsp_fax_override(&fax_fake_ops);
	if (!probe_up(&p)) {
		CHECK(false);
		goto out;
	}
	if (!wire_open(&w)) {
		CHECK(false);
		goto out;
	}

	cap = hal_backend_ops.cap_get(p.h);
	CHECK((cap->features & HAL_FEAT_T38) != 0);
	CHECK((cap->codecs & (1u << HAL_CODEC_T38)) != 0);
	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);
	if (!media_up(&p, &w, &m)) {
		CHECK(false);
		goto out;
	}
	memset(&fax, 0, sizeof(fax));
	fax.mode = HAL_FAX_T38;
	fax.codec = HAL_CODEC_PCMU;
	CHECK(hal_backend_ops.fax_profile_apply(p.h, 0, 0, &fax) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
out:
	udsp_fax_override(NULL);
	wire_close(&w);
	probe_down(&p);
}

/*
 * The name tables belong to rtp/jb.h, and the case checks them against it.
 * The decision names index enum udsp_jb_decision, which that header exports,
 * so the case asserts each one by its own enum member. The state numbers are
 * private to rtp/jb.c (enum jb_state), and udsp.h documents them as 0 idle,
 * 1 prefill, 2 play. The case therefore checks the three by position against
 * that documented order. A table that slipped by one would name every
 * decision of every line wrong, and no status code could show it.
 */
static void test_jb_name_tables(void)
{
	CASE("the decision names index enum udsp_jb_decision");
	CHECK(!strcmp(hal_udsp_jb_decision_name(UDSP_JB_FRAME), "frame"));
	CHECK(!strcmp(hal_udsp_jb_decision_name(UDSP_JB_PLC), "plc"));
	CHECK(!strcmp(hal_udsp_jb_decision_name(UDSP_JB_CN), "cn"));
	CHECK(!strcmp(hal_udsp_jb_decision_name(UDSP_JB_SILENCE), "silence"));
	CHECK(!strcmp(hal_udsp_jb_decision_name(UDSP_JB_SILENCE + 1u), "?"));

	CASE("the state names follow the order udsp.h documents");
	CHECK(!strcmp(hal_udsp_jb_state_name(0), "idle"));
	CHECK(!strcmp(hal_udsp_jb_state_name(1), "prefill"));
	CHECK(!strcmp(hal_udsp_jb_state_name(2), "play"));
	CHECK(!strcmp(hal_udsp_jb_state_name(3), "?"));

	CASE("the put result names carry the errno set udsp_jb_put answers with");
	CHECK(!strcmp(hal_udsp_jb_rc_name(0), "ok"));
	CHECK(!strcmp(hal_udsp_jb_rc_name(-ETIME), "late"));
	CHECK(!strcmp(hal_udsp_jb_rc_name(-EEXIST), "dup"));
	CHECK(!strcmp(hal_udsp_jb_rc_name(-ENOBUFS), "overflow"));
	CHECK(!strcmp(hal_udsp_jb_rc_name(-EMSGSIZE), "oversize"));
	CHECK(!strcmp(hal_udsp_jb_rc_name(-EINVAL), "invalid"));
	CHECK(!strcmp(hal_udsp_jb_rc_name(-1), "?"));
}

/*
 * The case drives the copy on a record of its own, with nineteen distinct
 * values, for the reason that udsp_int.h gives. A pair of swapped fields
 * would carry the wrong quantity for the life of the interface. No end to end
 * case could see it, because both fields are counters of the same shape.
 */
static void test_jb_stats_translation(void)
{
	struct udsp_jb_stats src;
	struct hal_jb_stats dst;

	CASE("every jitter buffer counter travels to its own field");
	memset(&src, 0, sizeof(src));
	memset(&dst, 0xa5, sizeof(dst));
	src.frames_out = 101;
	src.plc = 102;
	src.cn = 103;
	src.silence = 104;
	src.late_drop = 105;
	src.dup_drop = 106;
	src.overflow_drop = 107;
	src.hold_drop = 114;
	src.resync_discard_ms = 115;
	src.resyncs = 108;
	src.starve_resyncs = 109;
	src.cn_reanchors = 110;
	src.underruns = 111;
	src.skew_drops = 112;
	src.skew_inserts = 113;
	src.target_ms = 51;
	src.depth_ms = 52;
	src.jitter_ms = 53;
	src.pool_free = 54;

	hal_udsp_jb_stats_fill(&dst, &src);
	CHECK_UINT(dst.frames_out, 101);
	CHECK_UINT(dst.plc, 102);
	CHECK_UINT(dst.cn, 103);
	CHECK_UINT(dst.silence, 104);
	CHECK_UINT(dst.late_drop, 105);
	CHECK_UINT(dst.dup_drop, 106);
	CHECK_UINT(dst.overflow_drop, 107);
	CHECK_UINT(dst.hold_drop, 114);
	CHECK_UINT(dst.resync_discard_ms, 115);
	CHECK_UINT(dst.resyncs, 108);
	CHECK_UINT(dst.starve_resyncs, 109);
	CHECK_UINT(dst.cn_reanchors, 110);
	CHECK_UINT(dst.underruns, 111);
	CHECK_UINT(dst.skew_drops, 112);
	CHECK_UINT(dst.skew_inserts, 113);
	CHECK_UINT(dst.target_ms, 51);
	CHECK_UINT(dst.depth_ms, 52);
	CHECK_UINT(dst.jitter_ms, 53);
	CHECK_UINT(dst.pool_free, 54);
}

/* One pass of the canceller copy. first is the value of the first flag, and
 * the flags alternate from it in field order, so two neighbours never carry
 * the same value in a pass. The passthrough kind follows the pass too. */
static void lec_translation_check(uint8_t first)
{
	uint8_t second = !first;
	struct udsp_lec_stats src;
	struct hal_lec_stats dst;

	memset(&src, 0, sizeof(src));
	memset(&dst, 0xa5, sizeof(dst));
	src.tail_ms = 301;
	src.taps = 302;
	src.frames = 303;
	src.hold_frames = 304;
	src.diverge_resets = 305;
	src.probes = 306;
	src.erle_mdb = -307;
	src.coef_energy_micro = 308;
	src.echo_gain_micro = 309;
	src.ref_delay_samples = 310;
	src.skip_frames = 311;
	src.retimes = 312;
	src.nlp_active_frames = 313;
	src.nlp_wait_frames = 314;
	src.nlp_erle_frames = 315;
	src.nlp_unlike_frames = 316;
	src.nlp_dt_frames = 317;
	src.nlp_quiet_frames = 318;
	src.coef_peak_tap = 319;
	src.double_talk = first;
	src.nlp_active = second;
	src.ref_ready = first;
	src.nlp_enabled = second;
	src.ec_active = first;
	src.loop_open = second;
	src.passthrough = first ? UDSP_PASSTHROUGH_FAX : UDSP_PASSTHROUGH_MODEM;

	hal_udsp_lec_stats_fill(&dst, &src);
	CHECK_UINT(dst.tail_ms, 301);
	CHECK_UINT(dst.taps, 302);
	CHECK_UINT(dst.frames, 303);
	CHECK_UINT(dst.hold_frames, 304);
	CHECK_UINT(dst.diverge_resets, 305);
	CHECK_UINT(dst.probes, 306);
	CHECK(dst.erle_mdb == -307);
	CHECK_UINT(dst.coef_energy_micro, 308);
	CHECK_UINT(dst.echo_gain_micro, 309);
	CHECK_UINT(dst.ref_delay_samples, 310);
	CHECK_UINT(dst.skip_frames, 311);
	CHECK_UINT(dst.retimes, 312);
	CHECK_UINT(dst.nlp_active_frames, 313);
	CHECK_UINT(dst.nlp_wait_frames, 314);
	CHECK_UINT(dst.nlp_erle_frames, 315);
	CHECK_UINT(dst.nlp_unlike_frames, 316);
	CHECK_UINT(dst.nlp_dt_frames, 317);
	CHECK_UINT(dst.nlp_quiet_frames, 318);
	CHECK_UINT(dst.coef_peak_tap, 319);
	CHECK_UINT(dst.double_talk, first);
	CHECK_UINT(dst.nlp_active, second);
	CHECK_UINT(dst.ref_ready, first);
	CHECK_UINT(dst.nlp_enabled, second);
	CHECK_UINT(dst.ec_active, first);
	CHECK_UINT(dst.loop_open, second);
	CHECK_UINT(dst.passthrough,
		   first ? HAL_PASSTHROUGH_FAX : HAL_PASSTHROUGH_MODEM);
}

/*
 * The case drives the canceller copy on a record of its own, for the reason
 * that udsp_int.h gives. The nineteen numeric fields carry 301 to 319, with
 * erle_mdb negative, so a pair of swapped fields cannot read as correct. The
 * six flags alternate inside a pass and invert between the two passes, so
 * every flag reads 1 once and 0 once. The engine cases of
 * test_lec_stats_get() run a silent line, so this case is also the only proof
 * of the copy of skip_frames, double_talk, nlp_active and the processor
 * verdicts other than the quiet count.
 */
static void test_lec_stats_translation(void)
{
	CASE("every canceller field travels to its own field");
	lec_translation_check(1);

	CASE("and every canceller flag travels with its value inverted");
	lec_translation_check(0);
}

static void test_jb_stats_get(void)
{
	struct hal_media_stats fast, slow;
	struct hal_jb_stats jb;
	struct hal_job job = { 0 };
	struct hal_media m;
	struct probe p;
	struct wire w;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the jb stats case needs a started backend and a wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	memset(&jb, 0, sizeof(jb));

	CASE("a stream that was never configured has no counters to answer");
	CHECK(hal_backend_ops.stream_jb_stats_get(p.h, 0, 0, &jb) ==
	      HAL_UNSUPPORTED);

	CASE("jb stats: a line the board does not carry answers unsupported");
	CHECK(hal_backend_ops.stream_jb_stats_get(p.h, HAL_LINE_MAX, 0, &jb) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.stream_jb_stats_get(p.h, 0, HAL_STREAM_MAX, &jb) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.stream_jb_stats_get(p.h, 0, 0, NULL) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.stream_stats_get(p.h, HAL_LINE_MAX, 0, &fast) ==
	      HAL_UNSUPPORTED);

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);

	/*
	 * A configured stream that was never started runs no tick, so the
	 * library has published no counters for this configuration yet and
	 * refuses (udsp.h udsp_stream_jb_stats). That refusal is the reason
	 * the northbound skips a stream instead of showing zeros.
	 */
	CASE("a configured stream that never ticked still answers unsupported");
	wire_local_release(&w);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.stream_jb_stats_get(p.h, 0, 0, &jb) ==
	      HAL_UNSUPPORTED);

	CASE("a running stream that carried audio answers its counters");
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_OK);
	probe_wait(&p, 300);
	wire_talk(&p, &w, 0, 20);
	probe_wait(&p, 300);
	CHECK(hal_backend_ops.stream_jb_stats_get(p.h, 0, 0, &jb) == HAL_OK);
	CHECK(jb.frames_out > 0);
	/* The gauge travels too, and a buffer that is not
	 * leaking slots reports free ones. */
	CHECK(jb.pool_free > 0);
	/* The description asked for the adaptive playout and nothing moved it,
	 * so the record names that mode. */
	CHECK_UINT(jb.mode, HAL_JB_ADAPTIVE);

	CASE("a passthrough description moves the record to the fixed mode");
	m.passthrough = HAL_PASSTHROUGH_MODEM;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	probe_wait(&p, 300);
	wire_talk(&p, &w, 0, 20);
	probe_wait(&p, 300);
	CHECK(hal_backend_ops.stream_jb_stats_get(p.h, 0, 0, &jb) == HAL_OK);
	/* The engine pins the fixed mode at the depth the line adapted to, so
	 * the mode moves without the description naming a new one. */
	CHECK_UINT(jb.mode, HAL_JB_FIXED);
	m.passthrough = HAL_PASSTHROUGH_OFF;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	probe_wait(&p, 300);

	/*
	 * The FAST getter and the SLOW one read the same snapshot. The slow op
	 * copies it into the stream record and the fast one reads it live, so
	 * a fast read taken right after the completion has to agree with what
	 * the result getter answers.
	 */
	CASE("the fast media getter agrees with the completed slow query");
	memset(&fast, 0, sizeof(fast));
	memset(&slow, 0, sizeof(slow));
	CHECK(hal_backend_ops.stream_stats_start(p.h, 0, 0, &job) == HAL_OK);
	CHECK(job_wait(&p, job.token, 2000, NULL, NULL));
	CHECK(hal_backend_ops.stream_stats_result_get(p.h, 0, 0, &slow) == HAL_OK);
	CHECK(hal_backend_ops.stream_stats_get(p.h, 0, 0, &fast) == HAL_OK);
	CHECK(slow.packets_recv > 0);
	CHECK(fast.packets_recv >= slow.packets_recv);
	CHECK_UINT(fast.packets_sent >= slow.packets_sent, 1);
	CHECK_UINT(fast.bytes_recv >= slow.bytes_recv, 1);

	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	wire_close(&w);
	probe_down(&p);
}

/*
 * The processor verdicts of a silent line. The far end sends mu-law silence,
 * which decodes to 0 and leaves the reference window inactive, so every frame
 * the filter ran with the processor on counts as quiet and no other verdict
 * moves.
 */
static void lec_quiet_check(const struct hal_lec_stats *lec)
{
	CHECK(lec->nlp_quiet_frames > 0);
	CHECK_UINT(lec->nlp_quiet_frames, lec->frames);
	CHECK_UINT(lec->nlp_active_frames, 0);
	CHECK_UINT(lec->nlp_wait_frames, 0);
	CHECK_UINT(lec->nlp_erle_frames, 0);
	CHECK_UINT(lec->nlp_unlike_frames, 0);
	CHECK_UINT(lec->nlp_dt_frames, 0);
}

/* The fields the instance owns. The reference pair, retimes and the kind
 * belong to the line and stay live while the filter stands. */
static bool lec_instance_same(const struct hal_lec_stats *a,
			      const struct hal_lec_stats *b)
{
	return a->tail_ms == b->tail_ms && a->taps == b->taps &&
	       a->frames == b->frames && a->hold_frames == b->hold_frames &&
	       a->diverge_resets == b->diverge_resets &&
	       a->probes == b->probes && a->erle_mdb == b->erle_mdb &&
	       a->coef_energy_micro == b->coef_energy_micro &&
	       a->echo_gain_micro == b->echo_gain_micro &&
	       a->double_talk == b->double_talk &&
	       a->nlp_active == b->nlp_active &&
	       a->skip_frames == b->skip_frames &&
	       a->nlp_active_frames == b->nlp_active_frames &&
	       a->nlp_wait_frames == b->nlp_wait_frames &&
	       a->nlp_erle_frames == b->nlp_erle_frames &&
	       a->nlp_unlike_frames == b->nlp_unlike_frames &&
	       a->nlp_dt_frames == b->nlp_dt_frames &&
	       a->nlp_quiet_frames == b->nlp_quiet_frames &&
	       a->coef_peak_tap == b->coef_peak_tap;
}

/*
 * The line canceller getter takes a line and no stream, and the library
 * decides every refusal that it gives. This case drives the two refusals that
 * a caller meets: no canceller yet, and a line that the board does not carry.
 * It then reads the idle filter of an on-hook line, one live reading off hook
 * and the processor verdicts of a silent line. It also reads a channel that
 * turns the processor off and the reading that a hang-up leaves. Last, it
 * reads the bypassed reading that a channel leaves after it switches the
 * canceller off.
 *
 * The case reads the state against a real libudsp canceller. The fake TDM
 * captures silence, so the filter has no echo to learn, and its energy stays
 * where a fresh instance leaves it. The case proves that the
 * record that the engine published reaches the ops table field for field.
 */
static void test_lec_stats_get(void)
{
	struct hal_channel_cfg ccfg;
	struct hal_lec_stats lec;
	struct hal_lec_stats again;
	struct hal_media m;
	struct probe p;
	struct wire w;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the lec stats case needs a started backend and a wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	memset(&lec, 0xa5, sizeof(lec));

	CASE("a line that runs no canceller has no state to answer");
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) ==
	      HAL_UNSUPPORTED);

	CASE("lec stats: a line the board does not carry answers unsupported");
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, HAL_LINE_MAX, &lec) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, NULL) ==
	      HAL_UNSUPPORTED);

	/*
	 * The library makes the instance at stream config and takes the tail
	 * from the channel, so the channel is programmed first and the tail it
	 * asks for is the tail the row must carry.
	 */
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.detect_mask = HAL_DETECT_DTMF;
	ccfg.ec_enable = true;
	ccfg.ec_nlp_enable = true;
	ccfg.ec_tail_ms = 32;
	CHECK(hal_backend_ops.channel_config_set(p.h, 0, &ccfg) == HAL_OK);

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);

	/*
	 * The fake line rests on hook, and the library runs no canceller on an
	 * open loop. The instance exists all the same, so the line answers with
	 * a filter that ran no frame.
	 */
	CASE("an on-hook line holds a canceller that runs nothing");
	CHECK(media_up(&p, &w, &m));
	/* Long enough for the engine's delay calibration to settle. */
	probe_wait(&p, 300);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	CHECK_UINT(lec.ec_active, 0);
	CHECK_UINT(lec.loop_open, 1);
	CHECK_UINT(lec.frames, 0);
	CHECK_UINT(lec.nlp_enabled, 0);
	CHECK_UINT(lec.tail_ms, 32);

	/* Off hook for every case below. The line is silent, so nothing
	 * adapts while the cases wait. */
	udsp_slic_fake_hook_set(p.h->s->slic, 0, true);
	probe_wait(&p, 400);

	CASE("a fresh canceller answers with the state it was created with");
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	CHECK_UINT(lec.tail_ms, 32);
	CHECK_UINT(lec.taps, 256);
	/* The two scaled figures start apart, so a pair that swapped on the
	 * way through this backend cannot read as correct: the filter carries
	 * no energy and the learned gain bound stands at 0.25, which is
	 * 250000 micro-units. */
	CHECK_UINT(lec.coef_energy_micro, 0);
	CHECK_UINT(lec.echo_gain_micro, 250000);
	/* no energy, so no peak: a fresh filter must not pass for one aligned
	 * on its first tap */
	CHECK_UINT(lec.coef_peak_tap, 0);
	CHECK(lec.erle_mdb == 0);
	CHECK_UINT(lec.diverge_resets, 0);
	CHECK_UINT(lec.probes, 0);
	/* The first engine of this handle is the one the canceller was
	 * created under, so no reference moved under it yet. */
	CHECK_UINT(lec.retimes, 0);
	/* The reference alignment belongs to the line and not to the
	 * canceller. The line takes the engine's measurement on the first frame
	 * of its canceller, or when the first measurement after that frame
	 * completes. The reference therefore stands here. This fake TDM reports
	 * three periods of loop, which is 240 samples at 8 kHz, and the line
	 * applies one period under it. */
	CHECK_UINT(lec.ref_ready, 1);
	CHECK_UINT(lec.ref_delay_samples, 160);
	/* the channel asked for the processor and no policy took it away, so
	 * the switch the library reports is the one the config named */
	CHECK_UINT(lec.nlp_enabled, 1);
	/* the stream runs and no kind stands, so the filter is on the frame
	 * under the voice policy */
	CHECK_UINT(lec.ec_active, 1);
	CHECK_UINT(lec.loop_open, 0);
	CHECK_UINT(lec.passthrough, HAL_PASSTHROUGH_OFF);

	CASE("the frames of a running line advance and the holds stay under");
	wire_talk(&p, &w, 0, 20);
	probe_wait(&p, 300);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &again) == HAL_OK);
	CHECK(again.frames > 0);
	CHECK(again.frames >= lec.frames);
	CHECK(again.hold_frames <= again.frames);
	CHECK_UINT(again.tail_ms, 32);
	CHECK_UINT(again.ref_ready, 1);
	CHECK_UINT(again.ref_delay_samples, 160);
	lec_quiet_check(&again);

	CASE("a line without a canceller is skipped while another answers");
	/* Line 1 of this board carries no stream, so it runs no canceller
	 * while line 0 does: an op that indexed the wrong line would answer
	 * for both. */
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 1, &lec) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);

	CASE("a replacement engine re-times the canceller and the count copies");
	/* The stop destroys the engine and the start makes another. The line
	 * keeps the instance it holds, because only a stream release gives it
	 * up. The new engine re-times the reference, so the library resets
	 * that filter and counts the reset. A zero expectation would not
	 * prove the copy of this field. */
	CHECK_UINT(lec.retimes, 0);
	probe_stop(&p);
	CHECK(probe_start(&p));
	probe_wait(&p, 300);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	CHECK_UINT(lec.retimes, 1);
	/* the reset zeroed the verdicts with frames, so they count again from
	 * the first frame after it */
	lec_quiet_check(&lec);

	/*
	 * The filter keeps running with the processor off, and a frame the
	 * processor was off for takes no verdict. The frames then run past the
	 * six counts, and that remainder is what a reader subtracts.
	 */
	CASE("a channel that turns the processor off stops its counts");
	ccfg.ec_nlp_enable = false;
	CHECK(hal_backend_ops.channel_config_set(p.h, 0, &ccfg) == HAL_OK);
	probe_wait(&p, 100);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	probe_wait(&p, 300);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &again) == HAL_OK);
	CHECK_UINT(again.nlp_enabled, 0);
	CHECK_UINT(again.ec_active, 1);
	CHECK(again.frames > lec.frames);
	CHECK_UINT(again.nlp_quiet_frames, lec.nlp_quiet_frames);
	CHECK(again.frames > again.nlp_active_frames + again.nlp_wait_frames +
			     again.nlp_erle_frames + again.nlp_unlike_frames +
			     again.nlp_dt_frames + again.nlp_quiet_frames);

	/*
	 * A hang-up opens the loop, and the library holds the filter from the
	 * next tick on. The wait passes release_min_ms, so the reading is the
	 * one a last-call row takes after the release is reported.
	 */
	CASE("a hang-up holds the filter with the counters of the call");
	udsp_slic_fake_hook_set(p.h->s->slic, 0, false);
	probe_wait(&p, 2200);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	probe_wait(&p, 200);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &again) == HAL_OK);
	CHECK(lec_instance_same(&lec, &again));
	CHECK(again.frames > 0);
	CHECK_UINT(again.ec_active, 0);
	CHECK_UINT(again.loop_open, 1);
	udsp_slic_fake_hook_set(p.h->s->slic, 0, true);
	probe_wait(&p, 400);

	CASE("a channel that switches the canceller off reads an idle filter");
	/* The instance outlives the switch, so the line answers and the row
	 * says the filter ran nothing. The processor switch goes back on, so
	 * nlp_enabled reads 0 because of the canceller switch alone, and the
	 * closed loop tells this idle filter from the one a hang-up leaves. */
	ccfg.ec_enable = false;
	ccfg.ec_nlp_enable = true;
	CHECK(hal_backend_ops.channel_config_set(p.h, 0, &ccfg) == HAL_OK);
	probe_wait(&p, 100);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	CHECK_UINT(lec.ec_active, 0);
	CHECK_UINT(lec.loop_open, 0);
	CHECK_UINT(lec.nlp_enabled, 0);
	/* The switch is no kind of its own, so the row keeps the kind the tick
	 * ran under, which this stream leaves at the off kind. */
	CHECK_UINT(lec.passthrough, HAL_PASSTHROUGH_OFF);

	/*
	 * The caller's own release of the tone disabler (hal.h
	 * line_lec_latch_clear). The library owns what a release does, and the
	 * pin here is the crossing: the op exists, it forwards the line, and a
	 * line with no latch is an answer and not an error.
	 */
	CASE("the caller's own disabler release forwards to the library");
	CHECK(hal_backend_ops.line_lec_latch_clear != NULL);
	CHECK(hal_backend_ops.line_lec_latch_clear(p.h, 0) == HAL_OK);

	CASE("and a line the board does not carry answers unsupported");
	CHECK(hal_backend_ops.line_lec_latch_clear(p.h, HAL_LINE_MAX) ==
	      HAL_UNSUPPORTED);

	/* the reason the release posts, which travels as a raw byte in the
	 * record both contracts share */
	CASE("and the caller reason names the same value on both sides");
	CHECK_UINT((unsigned int)HAL_LEC_REASON_RELEASE_CALLER,
		   (unsigned int)UDSP_LEC_REASON_RELEASE_CALLER);

	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	wire_close(&w);
	probe_down(&p);
}

/* The two cells the monitor cases sample. Every field carries its own value.
 * The six decoded bits alternate inside a cell and invert between the two.
 * Every boolean is read at 1 in one case and at 0 in another. A field that
 * swapped or was dropped on the way through this backend then cannot read as
 * correct. */
static void monitor_cell_build(struct udsp_slic_monitor *m, bool flip)
{
	memset(m, 0, sizeof(*m));
	if (flip) {
		m->vtr_mv = -2001;
		m->vtip_mv = 2002;
		m->vring_mv = -2003;
		m->vlong_mv = 2004;
		m->vbat_mv = 2005;
		m->vdc_mv = -2006;
		m->itr_ua = 2007;
		m->itip_ua = -2008;
		m->iring_ua = 2009;
		m->ilong_ua = -2010;
		m->p_hvic_mw = 2011;
		m->vbat_tgt_mv = 2012;
		m->vbat_irq_th_mv = -2013;
		/* the converter runs, the lock-out stands, the feed is in a
		 * transition, and the loop reads the other two status bits */
		m->pd_dcdc = 0x400024u;
		m->dcdc_status = 0x400025u;
		m->lcrrtp = 0x09u;
		m->linefeed = 0x12u;
		return;
	}

	m->vtr_mv = 1001;
	m->vtip_mv = -1002;
	m->vring_mv = 1003;
	m->vlong_mv = -1004;
	m->vbat_mv = -48005;
	m->vdc_mv = 1006;
	m->itr_ua = -1007;
	m->itip_ua = 1008;
	m->iring_ua = -1009;
	m->ilong_ua = 1010;
	m->p_hvic_mw = 1011;
	m->vbat_tgt_mv = -48012;
	m->vbat_irq_th_mv = 5013;
	/* bit 20 set is the converter in shutdown; bit 22 of the status word
	 * is clear, so the two booleans read 1 and 0 and cannot swap. */
	m->pd_dcdc = 0x100014u;
	m->dcdc_status = 0x000015u;
	m->lcrrtp = 0x02u;		/* loop closure alone */
	m->linefeed = 0x11u;		/* requested and actual both active */
}

static void monitor_cell_check(const struct hal_monitor *mon, bool flip)
{
	if (flip) {
		CHECK(mon->vtr_mv == -2001);
		CHECK(mon->vtip_mv == 2002);
		CHECK(mon->vring_mv == -2003);
		CHECK(mon->vlong_mv == 2004);
		CHECK(mon->vbat_mv == 2005);
		CHECK(mon->vdc_mv == -2006);
		CHECK(mon->itr_ua == 2007);
		CHECK(mon->itip_ua == -2008);
		CHECK(mon->iring_ua == 2009);
		CHECK(mon->ilong_ua == -2010);
		CHECK(mon->p_hvic_mw == 2011);
		CHECK(mon->vbat_tgt_mv == 2012);
		CHECK(mon->vbat_irq_th_mv == -2013);
		CHECK_UINT(mon->pd_dcdc, 0x400024u);
		CHECK_UINT(mon->dcdc_status, 0x400025u);
		CHECK_UINT(mon->lcrrtp, 0x09u);
		CHECK_UINT(mon->linefeed, 0x12u);
		CHECK_UINT(mon->dcdc_down, 0);
		CHECK_UINT(mon->ovlo, 1);
		CHECK_UINT(mon->settled, 0);
		CHECK_UINT(mon->voc_err, 1);
		CHECK_UINT(mon->lcr, 0);
		CHECK_UINT(mon->rtp, 1);
		return;
	}

	CHECK(mon->vtr_mv == 1001);
	CHECK(mon->vtip_mv == -1002);
	CHECK(mon->vring_mv == 1003);
	CHECK(mon->vlong_mv == -1004);
	CHECK(mon->vbat_mv == -48005);
	CHECK(mon->vdc_mv == 1006);
	CHECK(mon->itr_ua == -1007);
	CHECK(mon->itip_ua == 1008);
	CHECK(mon->iring_ua == -1009);
	CHECK(mon->ilong_ua == 1010);
	CHECK(mon->p_hvic_mw == 1011);
	CHECK(mon->vbat_tgt_mv == -48012);
	CHECK(mon->vbat_irq_th_mv == 5013);
	CHECK_UINT(mon->pd_dcdc, 0x100014u);
	CHECK_UINT(mon->dcdc_status, 0x000015u);
	CHECK_UINT(mon->lcrrtp, 0x02u);
	CHECK_UINT(mon->linefeed, 0x11u);
	CHECK_UINT(mon->dcdc_down, 1);
	CHECK_UINT(mon->ovlo, 0);
	CHECK_UINT(mon->settled, 1);
	CHECK_UINT(mon->voc_err, 0);
	CHECK_UINT(mon->lcr, 1);
	CHECK_UINT(mon->rtp, 0);
}

/*
 * The line monitor getter takes a line and no stream, and the library decides
 * every refusal. A line that the board does not carry answers unsupported. A
 * line with no reading yet answers HAL_OK with valid 0.
 *
 * The reading comes from the monitor cell of the fake plug-in. The slic
 * thread samples it once at every fault edge, which is the sample point that
 * a case can drive. The periodic cadence is ten seconds of engine time.
 */
static void test_monitor_get(void)
{
	struct udsp_slic_monitor cell;
	struct udsp_monitor lib;
	struct hal_monitor mon;
	struct probe p;
	uint32_t stamp;

	if (!probe_up(&p)) {
		CASE("the monitor cases need a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	CASE("a line with no reading yet answers valid zero and no instant");
	memset(&mon, 0xa5, sizeof(mon));
	CHECK(hal_backend_ops.line_monitor_get(p.h, 0, &mon) == HAL_OK);
	CHECK_UINT(mon.valid, 0);
	CHECK_UINT(mon.stale, 0);
	CHECK_UINT(mon.stamp_ms, 0);
	/* The three fault fields are defined before the first sample. */
	CHECK_UINT(mon.fault_bits, 0);
	CHECK_UINT(mon.fault_open, 0);
	CHECK_UINT(mon.fault_held, 0);
	CHECK(mon.vbat_mv == 0);

	CASE("monitor: a line the board does not carry answers unsupported");
	CHECK(hal_backend_ops.line_monitor_get(p.h, HAL_LINE_MAX, &mon) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.line_monitor_get(p.h, 0, NULL) ==
	      HAL_UNSUPPORTED);

	monitor_cell_build(&cell, false);
	udsp_slic_fake_monitor_set(p.h->s->slic, 0, &cell);
	udsp_slic_fake_fault_set(p.h->s->slic, 0, UDSP_FAULT_SLIC_THERMAL);
	probe_wait(&p, 1500);

	CASE("the fault edge samples the cell and every field reaches the record");
	memset(&mon, 0xa5, sizeof(mon));
	CHECK(hal_backend_ops.line_monitor_get(p.h, 0, &mon) == HAL_OK);
	CHECK_UINT(mon.valid, 1);
	CHECK_UINT(mon.stale, 0);
	monitor_cell_check(&mon, false);

	/* The library record of the same snapshot is the oracle for the stamp:
	 * a copy that carried another field there would differ from it. */
	CASE("and the instant is the one the library published");
	memset(&lib, 0, sizeof(lib));
	CHECK(udsp_line_monitor(p.h->s, 0, &lib) == UDSP_OK);
	CHECK(lib.stamp_ms != 0);
	CHECK_UINT(mon.stamp_ms, lib.stamp_ms);

	CASE("and the row carries the engine's fault state of the line");
	CHECK_UINT(mon.fault_bits, UDSP_FAULT_SLIC_THERMAL);
	CHECK_UINT(mon.fault_open, 1);
	CHECK_UINT(mon.fault_held, 0);

	CASE("a line the fault never reached keeps its own empty row");
	memset(&mon, 0xa5, sizeof(mon));
	CHECK(hal_backend_ops.line_monitor_get(p.h, 1, &mon) == HAL_OK);
	CHECK_UINT(mon.valid, 0);
	CHECK_UINT(mon.fault_bits, 0);

	/* The second cell inverts every decoded bit, so each of the six is read
	 * at both values across the two samples. */
	monitor_cell_build(&cell, true);
	udsp_slic_fake_monitor_set(p.h->s->slic, 0, &cell);
	udsp_slic_fake_fault_set(p.h->s->slic, 0,
				 UDSP_FAULT_SLIC_THERMAL | UDSP_FAULT_SLIC_DC);
	probe_wait(&p, 1500);

	CASE("the next edge samples the second cell with the other decode");
	memset(&mon, 0xa5, sizeof(mon));
	CHECK(hal_backend_ops.line_monitor_get(p.h, 0, &mon) == HAL_OK);
	CHECK_UINT(mon.valid, 1);
	CHECK_UINT(mon.stale, 0);
	monitor_cell_check(&mon, true);
	stamp = mon.stamp_ms;

	/* One armed refusal, spent by the read the next fault edge runs. A
	 * clock fault holds the feed, which is the fault_held reading. */
	udsp_slic_fake_error_set(p.h->s->slic, UDSP_SLIC_FAKE_MONITOR_READ,
				 -ENODATA);
	udsp_slic_fake_fault_set(p.h->s->slic, 0, UDSP_FAULT_SLIC_CLOCK);
	probe_wait(&p, 1500);

	CASE("a read that answers no data marks the row stale and keeps the values");
	memset(&mon, 0xa5, sizeof(mon));
	CHECK(hal_backend_ops.line_monitor_get(p.h, 0, &mon) == HAL_OK);
	CHECK_UINT(mon.valid, 1);
	CHECK_UINT(mon.stale, 1);
	/* The instant is the one of THAT reading, not of the failed read. */
	CHECK_UINT(mon.stamp_ms, stamp);
	monitor_cell_check(&mon, true);

	CASE("and a held feed reaches the record beside the open one");
	CHECK_UINT(mon.fault_bits, UDSP_FAULT_SLIC_CLOCK);
	CHECK_UINT(mon.fault_open, 1);
	CHECK_UINT(mon.fault_held, 1);

	udsp_slic_fake_fault_set(p.h->s->slic, 0, 0);
	probe_down(&p);
}

/* A SLIC plug-in that carries no monitor op at all. The library refuses the
 * whole line, which is the answer that the stub backend gives when it leaves
 * the op NULL. */
static void test_monitor_without_op(void)
{
	struct hal_monitor mon;
	struct probe p;
	bool up;

	up = probe_open_opts(&p, UDSP_SLIC_FAKE_OPT_ALL &
			     ~(uint32_t)UDSP_SLIC_FAKE_OPT_MONITOR, 2);

	CASE("a plug-in without a monitor op answers unsupported");
	CHECK(up);
	if (!up) {
		probe_close(&p);
		return;
	}

	CHECK(probe_start(&p));
	memset(&mon, 0xa5, sizeof(mon));
	CHECK(hal_backend_ops.line_monitor_get(p.h, 0, &mon) ==
	      HAL_UNSUPPORTED);

	probe_down(&p);
}

static void test_jb_trace(void)
{
	struct hal_media m;
	uint32_t hold_ts = 0;
	uint16_t hold_seq = 0;
	unsigned int i;
	struct probe p;
	struct wire w;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the jb trace case needs a started backend and a wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);

	CASE("the stream the trace describes comes up");
	CHECK(media_up(&p, &w, &m));
	probe_wait(&p, 300);

	CASE("arming the trace is accepted and arms the drain timer");
	jbtrace_begin();
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_JB_TRACE) == HAL_OK);
	CHECK(p.h->jb_trace.pending);
	CHECK_UINT(p.h->debug, HAL_DEBUG_JB_TRACE);

	/*
	 * A reordered packet later than the playout point. One packet of the
	 * run is held back while every successor is delivered, so the media
	 * clock has passed its timestamp by the time it arrives and the
	 * library drops it late. The queue is still deep, which is what keeps
	 * the clock from parking and turning the drop into a refill.
	 */
	for (i = 0; i < 24u; i++) {
		if (i == 5u) {
			hold_seq = w.seq++;
			hold_ts = w.ts;
			w.ts += 20u * WIRE_SAMPLES_PER_MS;
			continue;
		}

		wire_send(&w, 0, 20);
	}
	probe_wait(&p, 200);
	wire_send_at(&w, 0, 20, hold_seq, hold_ts);
	probe_wait(&p, 200);
	/*
	 * The drain runs on a wall clock timer and the paced backend turns
	 * ten engine milliseconds per wall millisecond, so the waits above
	 * are worth a few wall milliseconds and would never reach the first
	 * firing. jbtrace_wait turns the loop in the units the timer counts.
	 */
	{
		static const char *const want[] = {
			"[jb]", "put late", "anchor",
		};

		jbtrace_wait(&p, want, sizeof(want) / sizeof(want[0]), 2000);
	}

	CASE("the drain writes the decisions of the run to the jb domain");
	CHECK(jbtrace_count("[jb]") > 0);

	CASE("the late drop reaches the log as a put with its reason named");
	CHECK(jbtrace_count("put late") > 0);

	CASE("and the anchor of the stream is named too");
	CHECK(jbtrace_count("anchor") > 0);

	CASE("clearing the mask cancels the timer and stops the trace");
	CHECK(hal_backend_ops.debug_set(p.h, 0) == HAL_OK);
	CHECK(!p.h->jb_trace.pending);
	CHECK_UINT(p.h->debug, 0);
	/* The capture starts after the disarm, because the disarm itself
	 * drains what the ring still held and those records belong to the run
	 * that is over. Nothing may arrive after that. */
	jbtrace_begin();
	wire_talk(&p, &w, 0, 20);
	probe_settle(&p, 400);
	CHECK_UINT(jbtrace_count("[jb]"), 0);
	jbtrace_end();

	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	wire_close(&w);
	probe_down(&p);
}

/*
 * The arm discards what the previous session left. The engine reads the
 * debug word once per tick, on its own thread. A tick inside its receive path
 * can push one more record after the disarm drain returns, and the canceled
 * timer strands that record. Without the discard, the next arm would open
 * with a line that carries a stamp from before the trace was off.
 *
 * No case reaches that race, so the case builds the state that the race
 * leaves. It drives the library flag past the backend, which leaves records in
 * the ring with its timer canceled. That state is exactly what a stranded
 * record looks like. The case then stops the stream, so every line that the
 * drain writes must come from the session that is over.
 */
static void test_jb_trace_arm_discard(void)
{
	struct hal_media m;
	struct probe p;
	struct wire w;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the jb discard case needs a started backend and a wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);

	CASE("the stream the discard case runs on comes up");
	CHECK(media_up(&p, &w, &m));
	probe_wait(&p, 300);

	/* The backend is off, so its timer is cancelled and nothing drains. */
	CHECK(hal_backend_ops.debug_set(p.h, 0) == HAL_OK);
	CHECK(!p.h->jb_trace.pending);

	/* White box, the way this suite already reads the stream record: the
	 * library produces while the backend does not consume. */
	udsp_debug_set(p.h->s, UDSP_DEBUG_JB_TRACE);
	wire_talk(&p, &w, 0, 20);
	probe_wait(&p, 400);
	udsp_debug_set(p.h->s, 0);
	probe_wait(&p, 100);

	/* No stream, no new records: everything a drain could write from here
	 * belongs to the run above. */
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	probe_wait(&p, 100);

	CASE("arming again writes nothing the previous session left behind");
	jbtrace_begin();
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_JB_TRACE) == HAL_OK);
	CHECK(p.h->jb_trace.pending);
	probe_settle(&p, 400);
	CHECK_UINT(jbtrace_count("[jb]"), 0);
	jbtrace_end();

	CHECK(hal_backend_ops.debug_set(p.h, 0) == HAL_OK);
	wire_close(&w);
	probe_down(&p);
}

/* The sentinel of the close case below: it says the loop really turned for the
 * window the case claims, so the absence of a drain is evidence and not the
 * silence of a loop that never ran. */
static struct uloop_timeout jbtrace_beat;
static unsigned int jbtrace_sentinel;

static void jbtrace_beat_expire(struct uloop_timeout *t)
{
	jbtrace_sentinel++;
	uloop_timeout_set(t, HAL_UDSP_TRACE_MS / 2);
}

/*
 * The debug bit of the backend and its log sink. The hal domain sets
 * HAL_DEBUG_SLIC_LOG, which reaches the library as UDSP_DEBUG_SLIC_LOG and
 * leaves the drain timer of the jitter buffer alone. The sink maps each level
 * to one syslog priority, and gives a DEBUG line the trace shape by hand.
 */
static void test_slic_log(void)
{
	struct trace_ring_counts counts;
	uint64_t dropped;
	struct probe p;

	if (!probe_up(&p)) {
		CASE("the slic log case needs a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	CASE("the slic log bit reaches the library and arms no drain timer");
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_SLIC_LOG) == HAL_OK);
	CHECK_UINT(p.h->debug, HAL_DEBUG_SLIC_LOG);
	CHECK_UINT(atomic_load(&p.h->s->debug), UDSP_DEBUG_SLIC_LOG);
	CHECK(!p.h->jb_trace.pending);

	CASE("both bits arm the drain timer, and jb off alone stops it");
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_SLIC_LOG |
					      HAL_DEBUG_JB_TRACE) == HAL_OK);
	CHECK(p.h->jb_trace.pending);
	CHECK_UINT(atomic_load(&p.h->s->debug),
		   UDSP_DEBUG_SLIC_LOG | UDSP_DEBUG_JB_TRACE);
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_SLIC_LOG) == HAL_OK);
	CHECK(!p.h->jb_trace.pending);
	CHECK(hal_backend_ops.debug_set(p.h, 0) == HAL_OK);
	CHECK_UINT(atomic_load(&p.h->s->debug), 0);

	CASE("each level maps to one syslog priority");
	CHECK_UINT(hal_udsp_log_prio(UDSP_LOG_ERR), LOG_ERR);
	CHECK_UINT(hal_udsp_log_prio(UDSP_LOG_WARN), LOG_WARNING);
	CHECK_UINT(hal_udsp_log_prio(UDSP_LOG_INFO), LOG_INFO);
	CHECK_UINT(hal_udsp_log_prio(UDSP_LOG_DEBUG), LOG_DEBUG);

	/*
	 * The two masks decide the two destinations. An operator who asked for
	 * the hal domain gets the debug line in the log; a call trace, which
	 * holds the ring mask alone, leaves the log untouched and hands the
	 * line to the relay instead. The relay keeps it, so its drop count
	 * does not move.
	 */
	CASE("a debug line takes the trace shape and an info line the plain one");
	jbtrace_begin();
	trace_domain_set(TRACE_HAL, true);
	hal_udsp_log(p.h, UDSP_LOG_DEBUG, "probe debug");
	hal_udsp_log(p.h, UDSP_LOG_INFO, "probe info");
	CHECK_UINT(jbtrace_count("[hal] udsp: probe debug"), 1);
	CHECK_UINT(jbtrace_count("udsp: probe info"), 1);
	CHECK_UINT(jbtrace_count("[hal] udsp: probe info"), 0);
	trace_domain_set(TRACE_HAL, false);
	jbtrace_end();

	dropped = trace_relay_dropped();
	jbtrace_begin();
	trace_ring_set(1u << TRACE_HAL);
	hal_udsp_log(p.h, UDSP_LOG_DEBUG, "ring only debug");
	CHECK_UINT(jbtrace_count("ring only debug"), 0);
	CHECK_UINT(trace_relay_dropped(), dropped);
	trace_ring_set(0);
	jbtrace_end();

	/*
	 * The line the archive keeps. A debug line follows the hal bit of the
	 * ring mask; a warning follows the open rings alone, because a fault
	 * belongs in the archive whether or not an operator named the domain.
	 * The relay carries all three onto the loop, where the drain writes
	 * them to the log ring.
	 */
	CASE("the log ring takes the library's lines while a trace runs");
	/* The rings need their udebug context, which the daemon creates at
	 * start; this case is the only one here that opens them. */
	trace_ring_init();
	if (trace_ring_open(false, false) != 0) {
		tu_skip("the build carries no udebug support");
		probe_down(&p);
		return;
	}

	jbtrace_begin();
	trace_ring_set(1u << TRACE_HAL);
	hal_udsp_log(p.h, UDSP_LOG_DEBUG, "ring debug");
	hal_udsp_log(p.h, UDSP_LOG_WARN, "ring warn");
	trace_ring_set(0);
	hal_udsp_log(p.h, UDSP_LOG_WARN, "ring warn with no mask");
	uloop_pump(TRACE_RELAY_DRAIN_MS + 50);
	trace_ring_counts(TRACE_RING_LOG, &counts);
	CHECK_UINT(counts.written, 3);
	CHECK_UINT(trace_relay_dropped(), 0);
	CHECK_UINT(jbtrace_count("ring debug"), 0);
	CHECK_UINT(jbtrace_count("udsp: ring warn"), 2);
	jbtrace_end();
	trace_ring_close();

	probe_down(&p);
}

/*
 * The close path with the trace still armed. main.c closes the backend before
 * uloop_done(). A drain timer left armed here would fire on the next turn of
 * the running loop and read a freed handle. The ASan leg proves the
 * free itself; this case proves that the timer left the loop.
 */
static void test_jb_trace_close(void)
{
	struct probe p;

	if (!probe_up(&p)) {
		CASE("the trace close case needs a started backend");
		CHECK(false);
		return;
	}

	CASE("a close with the trace armed leaves no timer behind");
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_JB_TRACE) == HAL_OK);
	CHECK(p.h->jb_trace.pending);
	probe_stop(&p);
	hal_backend_ops.close(p.h);
	p.h = NULL;

	/*
	 * The oracle is the loop, not the freed record. Reading jb_trace back
	 * after the free would be the very defect this case exists for, so the
	 * loop is turned for longer than the drain period instead: a timer the
	 * close left armed fires here, against a handle that is gone, and the
	 * sanitizer leg names it. The sentinel is what proves the window was
	 * really turned, so a case that armed nothing cannot pass by accident.
	 */
	jbtrace_sentinel = 0;
	jbtrace_beat.cb = jbtrace_beat_expire;
	uloop_timeout_set(&jbtrace_beat, HAL_UDSP_TRACE_MS / 2);
	uloop_pump(HAL_UDSP_TRACE_MS * 3u);
	uloop_timeout_cancel(&jbtrace_beat);
	CHECK(jbtrace_sentinel > 0);
	probe_down(&p);
}

/* --- the plaintext packet trace ------------------------------------------- */

/*
 * The exported file, read back for the fields these cases check: the block
 * walk, the enhanced packet blocks and the bytes of each. It decodes no
 * option and no timestamp resolution, because the datagram the backend
 * synthesized is the whole oracle.
 *
 * The export writes one interface per open ring in ring id order, so the media
 * ring is the fourth of the four a call trace opens.
 */
#define PKT_BTYPE_EPB		6u
#define PKT_IFACE_RTP		3u
#define PKT_FILE_MAX		(1024 * 1024)
#define PKT_SEEN_MAX		512u
/* The wall clock instant every crafted record carries, in nanoseconds, so the
 * entry stamp is that value in microseconds. */
#define PKT_STAMP_NS		1700000000123456000ull
#define PKT_STAMP_US		1700000000123456ull
/* One and a half seconds of clock offset, in both units, so a case can drive
 * the conversion in either direction. */
#define PKT_OFFSET_NS		1500000000ll
#define PKT_OFFSET_US		1500000ull
/* RFC 791 and RFC 768: the headers the ring writes before a payload. */
#define PKT_IPV4_HDR		20u
#define PKT_IPV6_HDR		40u
#define PKT_UDP_HDR		8u

struct pkt_entry {
	uint32_t iface;
	uint64_t stamp;
	uint32_t len;
	const uint8_t *data;
};

static uint8_t pkt_file[PKT_FILE_MAX];
static struct pkt_entry pkt_seen[PKT_SEEN_MAX];
static unsigned int pkt_seen_len;
/* Sized for any path, because a case can export to a path the build names. */
static char pkt_path[PATH_MAX];

static uint32_t pkt_rd32(const uint8_t *p)
{
	uint32_t val;

	memcpy(&val, p, sizeof(val));

	return val;
}

static uint16_t pkt_be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static bool pkt_file_parse(const char *path)
{
	size_t len, pos = 0;
	FILE *f;

	pkt_seen_len = 0;
	f = fopen(path, "r");
	if (!f)
		return false;

	len = fread(pkt_file, 1, sizeof(pkt_file), f);
	fclose(f);

	while (pos + 12u <= len) {
		uint32_t type = pkt_rd32(&pkt_file[pos]);
		uint32_t blen = pkt_rd32(&pkt_file[pos + 4u]);
		const uint8_t *body = &pkt_file[pos + 8u];

		if (blen < 12u || (blen & 3u) || pos + blen > len)
			return false;

		if (type == PKT_BTYPE_EPB && pkt_seen_len < PKT_SEEN_MAX) {
			struct pkt_entry *e = &pkt_seen[pkt_seen_len++];

			e->iface = pkt_rd32(body);
			e->stamp = ((uint64_t)pkt_rd32(&body[4]) << 32) |
				   pkt_rd32(&body[8]);
			e->len = pkt_rd32(&body[12]);
			e->data = &body[20];
		}

		pos += blen;
	}

	return pos == len;
}

static unsigned int pkt_count(uint32_t iface)
{
	unsigned int i, n = 0;

	for (i = 0; i < pkt_seen_len; i++) {
		if (pkt_seen[i].iface == iface)
			n++;
	}

	return n;
}

/* The entries of one interface that carry one direction. The direction sits in
 * the top two bits of the IPv4 type of service byte, which is where the ring
 * puts it; every case that counts by direction is IPv4. */
static unsigned int pkt_count_dir(uint32_t iface, enum trace_pkt_dir dir)
{
	unsigned int i, n = 0;

	for (i = 0; i < pkt_seen_len; i++) {
		if (pkt_seen[i].iface != iface || pkt_seen[i].len < 2u)
			continue;
		if ((pkt_seen[i].data[1] >> 6) == (unsigned int)dir)
			n++;
	}

	return n;
}

/* The nth entry of one interface, or an empty one, so a missing entry fails an
 * assertion instead of the process. */
static const uint8_t pkt_none_data[PKT_IPV6_HDR + PKT_UDP_HDR];
static const struct pkt_entry pkt_none = { .data = pkt_none_data };

static const struct pkt_entry *pkt_nth(uint32_t iface, unsigned int nth)
{
	unsigned int seen = 0;
	unsigned int i;

	for (i = 0; i < pkt_seen_len; i++) {
		if (pkt_seen[i].iface != iface)
			continue;
		if (seen++ == nth)
			return &pkt_seen[i];
	}

	return &pkt_none;
}

/* The direction and the kind of one entry, the byte trace_ring.h documents:
 * the direction in bits 7..6 and the kind in bits 5..2. */
static uint8_t pkt_tos(enum trace_pkt_dir dir, enum trace_pkt_kind kind)
{
	return (uint8_t)(((unsigned int)dir << 6) | ((unsigned int)kind << 2));
}

static void pkt_v4_check(const struct pkt_entry *e, uint8_t tos,
			 const char *src, uint16_t sport, const char *dst,
			 uint16_t dport, const uint8_t *data, size_t len)
{
	uint8_t want_src[4];
	uint8_t want_dst[4];

	CHECK(inet_pton(AF_INET, src, want_src) == 1);
	CHECK(inet_pton(AF_INET, dst, want_dst) == 1);
	CHECK_UINT(e->len, PKT_IPV4_HDR + PKT_UDP_HDR + len);
	CHECK_UINT(e->data[0], 0x45);
	CHECK_UINT(e->data[1], tos);
	CHECK_UINT(pkt_be16(&e->data[2]), PKT_IPV4_HDR + PKT_UDP_HDR + len);
	CHECK_UINT(e->data[9], 17);
	CHECK(!memcmp(&e->data[12], want_src, sizeof(want_src)));
	CHECK(!memcmp(&e->data[16], want_dst, sizeof(want_dst)));
	CHECK_UINT(pkt_be16(&e->data[20]), sport);
	CHECK_UINT(pkt_be16(&e->data[22]), dport);
	CHECK_UINT(pkt_be16(&e->data[24]), PKT_UDP_HDR + len);
	CHECK(!memcmp(&e->data[PKT_IPV4_HDR + PKT_UDP_HDR], data, len));
}

/* The version nibble and the traffic class straddle the first two bytes of an
 * IPv6 header, so the direction and kind byte is read across them. */
static void pkt_v6_check(const struct pkt_entry *e, uint8_t tos,
			 const char *src, uint16_t sport, const char *dst,
			 uint16_t dport, const uint8_t *data, size_t len)
{
	uint8_t want_src[16];
	uint8_t want_dst[16];

	CHECK(inet_pton(AF_INET6, src, want_src) == 1);
	CHECK(inet_pton(AF_INET6, dst, want_dst) == 1);
	CHECK_UINT(e->len, PKT_IPV6_HDR + PKT_UDP_HDR + len);
	CHECK_UINT(e->data[0] >> 4, 6);
	CHECK_UINT(((e->data[0] & 0x0fu) << 4) | (e->data[1] >> 4), tos);
	CHECK_UINT(pkt_be16(&e->data[4]), PKT_UDP_HDR + len);
	CHECK_UINT(e->data[6], 17);
	CHECK(!memcmp(&e->data[8], want_src, sizeof(want_src)));
	CHECK(!memcmp(&e->data[24], want_dst, sizeof(want_dst)));
	CHECK_UINT(pkt_be16(&e->data[40]), sport);
	CHECK_UINT(pkt_be16(&e->data[42]), dport);
	CHECK_UINT(pkt_be16(&e->data[44]), PKT_UDP_HDR + len);
	CHECK(!memcmp(&e->data[PKT_IPV6_HDR + PKT_UDP_HDR], data, len));
}

/*
 * One record as the library's producers write it. The payload bytes name the
 * record, so an entry that carried another record's data cannot read as
 * correct.
 */
static void pkt_rec_make(struct udsp_pkt_trace *rec, uint32_t gen, uint8_t dir,
			 uint8_t kind, uint8_t mark, uint16_t len)
{
	unsigned int i;

	memset(rec, 0, sizeof(*rec));
	rec->stamp_ns = PKT_STAMP_NS;
	rec->gen = gen;
	rec->len = len;
	rec->dir = dir;
	rec->kind = kind;
	for (i = 0; i < len; i++)
		rec->data[i] = (uint8_t)(mark + i);
}

/* The rings a call trace opens, aimed at the fixture directory. The media ring
 * is the one under test and the export file is the oracle. */
static bool pkt_rings_open(void)
{
	snprintf(pkt_path, sizeof(pkt_path), "%s/rings.pcapng", fixture_dir);
	trace_ring_init();

	return trace_ring_open(true, false) == 0;
}

static void pkt_rings_close(void)
{
	trace_ring_close();
	unlink(pkt_path);
}

/* Turns the loop until a drain moves the media ring past floor. The drain
 * runs on this thread, so a count read here is never mid-drain. */
static void rtp_ring_wait(struct probe *p, uint64_t floor, unsigned int ms)
{
	uint64_t stop = mono_us() + (uint64_t)ms * 1000u;
	struct trace_ring_counts c;

	for (;;) {
		trace_ring_counts(TRACE_RING_RTP, &c);
		if (c.written > floor || mono_us() >= stop)
			return;

		probe_poll(p);
		uloop_pump(5);
	}
}

/*
 * A crafted record onto the ring, and the datagram that it became. The
 * endpoints come from the description that the backend recorded at
 * stream_config_set, not from the record. A record and a table entry
 * therefore meet in this case.
 *
 * The transmit record of an SRTP call can be read only here: the producer
 * takes it before the library protects the packet.
 */
static void test_pkt_trace_render(void)
{
	struct udsp_pkt_trace rtp;
	struct udsp_pkt_trace rtcp;
	const struct pkt_entry *e;
	struct hal_media m;
	uint16_t alt_port;
	bool done = false;
	uint32_t gen;
	struct probe p;
	struct wire w;
	int alt = -1;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the packet trace render case needs a started backend and "
		     "a wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	CASE("a configured stream records the description the trace renders "
	     "with");
	if (!pkt_rings_open()) {
		tu_skip("the build carries no udebug support");
		wire_close(&w);
		probe_down(&p);
		return;
	}

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);
	alt = udp_bind_alt(0);
	alt_port = sock_port(alt);
	/* The far end named an RTCP address and port of its own (RFC 3605
	 * section 2.1), so the RTCP entries below must carry them and not the
	 * media endpoint. */
	m.remote.rtcp_port = alt_port;
	m.rtcp_remote.ipver = 4;
	CHECK(inet_pton(AF_INET, WIRE_ALT_ADDR, m.rtcp_remote.addr) == 1);
	CHECK(alt >= 0);
	CHECK(media_up(&p, &w, &m));
	gen = udsp_stream_gen(p.h->s, 0, 0);
	CHECK(gen != 0);
	CHECK_UINT(p.h->pkt_gen[0][0].count, 1);
	CHECK_UINT(p.h->pkt_gen[0][0].entry[0].gen, gen);

	CASE("a transmit rtp record renders as the datagram it was");
	CHECK_UINT(trace_ring_export_open(pkt_path), 0);
	pkt_rec_make(&rtp, gen, UDSP_PKT_TX, UDSP_PKT_RTP, 0x40, 172);
	pkt_rec_make(&rtcp, gen, UDSP_PKT_RX, UDSP_PKT_RTCP, 0x90, 64);
	pkt_trace_record(p.h, &rtp, 0);
	pkt_trace_record(p.h, &rtcp, 0);
	CHECK_UINT(trace_ring_export_step(PKT_SEEN_MAX, &done), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(pkt_file_parse(pkt_path));
	CHECK_UINT(pkt_count(PKT_IFACE_RTP), 2);
	e = pkt_nth(PKT_IFACE_RTP, 0);
	CHECK_UINT(e->stamp, PKT_STAMP_US);
	pkt_v4_check(e, pkt_tos(TRACE_PKT_TX, TRACE_PKT_RTP), "127.0.0.1",
		     w.local, "127.0.0.1", w.port, rtp.data, rtp.len);

	/* The local side of an RTCP entry is the derived port, the far side the
	 * address and the port the peer named. */
	CASE("a receive rtcp record renders with the rtcp endpoints reversed");
	e = pkt_nth(PKT_IFACE_RTP, 1);
	pkt_v4_check(e, pkt_tos(TRACE_PKT_RX, TRACE_PKT_RTCP), WIRE_ALT_ADDR,
		     alt_port, "127.0.0.1", (uint16_t)(w.local + 1u), rtcp.data,
		     rtcp.len);

	/* The record carries a monotonic instant and the ring a wall clock one,
	 * so the drain's offset is what converts it, in either direction. */
	CASE("the drain's clock offset moves the stamp both ways");
	CHECK_UINT(trace_ring_export_open(pkt_path), 0);
	pkt_rec_make(&rtp, gen, UDSP_PKT_TX, UDSP_PKT_RTP, 0x60, 32);
	pkt_trace_record(p.h, &rtp, PKT_OFFSET_NS);
	pkt_trace_record(p.h, &rtp, -PKT_OFFSET_NS);
	CHECK_UINT(trace_ring_export_step(PKT_SEEN_MAX, &done), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(pkt_file_parse(pkt_path));
	CHECK_UINT(pkt_count(PKT_IFACE_RTP), 2);
	CHECK_UINT(pkt_nth(PKT_IFACE_RTP, 0)->stamp,
		   PKT_STAMP_US + PKT_OFFSET_US);
	CHECK_UINT(pkt_nth(PKT_IFACE_RTP, 1)->stamp,
		   PKT_STAMP_US - PKT_OFFSET_US);

	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	pkt_rings_close();
	if (alt >= 0)
		close(alt);
	wire_close(&w);
	probe_down(&p);
}

/*
 * The same render over ::1. The endpoints belong to this backend, so the
 * description that it recorded decides the family of the entry, and nothing
 * in the record does.
 */
static void test_pkt_trace_render_v6(void)
{
	struct udsp_pkt_trace rec;
	const char *why = NULL;
	struct hal_media m;
	bool done = false;
	char reason[96];
	uint32_t gen = 0;
	struct probe p;
	struct wire w;

	if (!tu_v6_loopback(reason, sizeof(reason)))
		why = reason;

	if (!why && (!probe_up(&p) || !wire_open6(&w))) {
		CASE("the v6 packet trace case needs a started backend and a "
		     "wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	CASE("a receive rtp record over ::1 renders as a v6 datagram");
	if (v6_run(why)) {
		if (!pkt_rings_open()) {
			tu_skip("the build carries no udebug support");
			wire_close(&w);
			probe_down(&p);
			return;
		}

		media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);
		CHECK(media_up(&p, &w, &m));
		gen = udsp_stream_gen(p.h->s, 0, 0);
		CHECK_UINT(trace_ring_export_open(pkt_path), 0);
		pkt_rec_make(&rec, gen, UDSP_PKT_RX, UDSP_PKT_RTP, 0x20, 172);
		pkt_trace_record(p.h, &rec, 0);
		CHECK_UINT(trace_ring_export_step(PKT_SEEN_MAX, &done), 0);
		CHECK_UINT(trace_ring_export_close(NULL), 0);
		CHECK(pkt_file_parse(pkt_path));
		CHECK_UINT(pkt_count(PKT_IFACE_RTP), 1);
		pkt_v6_check(pkt_nth(PKT_IFACE_RTP, 0),
			     pkt_tos(TRACE_PKT_RX, TRACE_PKT_RTP), "::1",
			     w.port, "::1", w.local, rec.data, rec.len);

		CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
		pkt_rings_close();
		wire_close(&w);
		probe_down(&p);
	}
}

/*
 * The generation table, which is the whole reason that a record carries a
 * generation. A record drained after a re-INVITE still belongs to the
 * description that it traveled under. Only a table that kept that description
 * can show where the packet went.
 */
static void test_pkt_trace_generations(void)
{
	struct udsp_pkt_trace rec;
	uint32_t gen_old, gen_new;
	struct hal_media m;
	uint64_t miss_before;
	uint16_t alt_port;
	bool done = false;
	struct probe p;
	struct wire w;
	int alt = -1;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the generation case needs a started backend and a wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	CASE("a re-invite keeps the description the first generation named");
	if (!pkt_rings_open()) {
		tu_skip("the build carries no udebug support");
		wire_close(&w);
		probe_down(&p);
		return;
	}

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);
	CHECK(media_up(&p, &w, &m));
	gen_old = udsp_stream_gen(p.h->s, 0, 0);

	alt = udp_bind(AF_INET, 0);
	alt_port = sock_port(alt);
	CHECK(alt >= 0);
	CHECK(alt_port != 0);
	m.remote.port = alt_port;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	gen_new = udsp_stream_gen(p.h->s, 0, 0);
	CHECK(gen_new != gen_old);
	CHECK_UINT(p.h->pkt_gen[0][0].count, 2);

	CASE("a record of the first generation renders the first far end");
	CHECK_UINT(trace_ring_export_open(pkt_path), 0);
	miss_before = p.h->pkt_gen_miss;
	pkt_rec_make(&rec, gen_old, UDSP_PKT_TX, UDSP_PKT_RTP, 0x11, 32);
	pkt_trace_record(p.h, &rec, 0);
	CHECK_UINT(trace_ring_export_step(PKT_SEEN_MAX, &done), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(pkt_file_parse(pkt_path));
	CHECK_UINT(pkt_count(PKT_IFACE_RTP), 1);
	pkt_v4_check(pkt_nth(PKT_IFACE_RTP, 0),
		     pkt_tos(TRACE_PKT_TX, TRACE_PKT_RTP), "127.0.0.1", w.local,
		     "127.0.0.1", w.port, rec.data, rec.len);
	CHECK_UINT(p.h->pkt_gen_miss, miss_before);

	CASE("a record of the live generation renders the moved far end");
	CHECK_UINT(trace_ring_export_open(pkt_path), 0);
	pkt_rec_make(&rec, gen_new, UDSP_PKT_TX, UDSP_PKT_RTP, 0x22, 32);
	pkt_trace_record(p.h, &rec, 0);
	CHECK_UINT(trace_ring_export_step(PKT_SEEN_MAX, &done), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(pkt_file_parse(pkt_path));
	CHECK_UINT(pkt_count(PKT_IFACE_RTP), 1);
	pkt_v4_check(pkt_nth(PKT_IFACE_RTP, 0),
		     pkt_tos(TRACE_PKT_TX, TRACE_PKT_RTP), "127.0.0.1", w.local,
		     "127.0.0.1", alt_port, rec.data, rec.len);
	CHECK_UINT(p.h->pkt_gen_miss, miss_before);

	/* A generation older than the table can hold still belongs on the
	 * ring: the newest description is the best answer there is, and the
	 * counter is what says the entry is not exact. */
	CASE("a record of an unknown generation takes the newest and counts");
	CHECK_UINT(trace_ring_export_open(pkt_path), 0);
	pkt_rec_make(&rec, gen_new + 1000u, UDSP_PKT_TX, UDSP_PKT_RTP, 0x33, 32);
	pkt_trace_record(p.h, &rec, 0);
	CHECK_UINT(trace_ring_export_step(PKT_SEEN_MAX, &done), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(pkt_file_parse(pkt_path));
	CHECK_UINT(pkt_count(PKT_IFACE_RTP), 1);
	pkt_v4_check(pkt_nth(PKT_IFACE_RTP, 0),
		     pkt_tos(TRACE_PKT_TX, TRACE_PKT_RTP), "127.0.0.1", w.local,
		     "127.0.0.1", alt_port, rec.data, rec.len);
	CHECK_UINT(p.h->pkt_gen_miss, miss_before + 1u);

	/* The stop closes the sockets and the table stands, which is the case
	 * the table exists for: a record drained after it still renders. */
	CASE("a record of a stopped stream still renders its endpoints");
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	CHECK_UINT(trace_ring_export_open(pkt_path), 0);
	pkt_rec_make(&rec, gen_new, UDSP_PKT_RX, UDSP_PKT_RTP, 0x44, 32);
	pkt_trace_record(p.h, &rec, 0);
	CHECK_UINT(trace_ring_export_step(PKT_SEEN_MAX, &done), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(pkt_file_parse(pkt_path));
	CHECK_UINT(pkt_count(PKT_IFACE_RTP), 1);
	pkt_v4_check(pkt_nth(PKT_IFACE_RTP, 0),
		     pkt_tos(TRACE_PKT_RX, TRACE_PKT_RTP), "127.0.0.1", alt_port,
		     "127.0.0.1", w.local, rec.data, rec.len);

	/* A stream that never took a description has no entry at all, so the
	 * record is dropped and counted the same way. */
	CASE("a record of a stream with no description is dropped");
	miss_before = p.h->pkt_gen_miss;
	CHECK_UINT(trace_ring_export_open(pkt_path), 0);
	pkt_rec_make(&rec, gen_new, UDSP_PKT_TX, UDSP_PKT_RTP, 0x55, 32);
	rec.stream = 1;
	pkt_trace_record(p.h, &rec, 0);
	CHECK_UINT(trace_ring_export_step(PKT_SEEN_MAX, &done), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(pkt_file_parse(pkt_path));
	CHECK_UINT(pkt_count(PKT_IFACE_RTP), 0);
	CHECK_UINT(p.h->pkt_gen_miss, miss_before + 1u);

	pkt_rings_close();
	if (alt >= 0)
		close(alt);
	wire_close(&w);
	probe_down(&p);
}

/*
 * The live drain. The bit reaches the library through debug_set, the producers
 * fill the two rings, and the timer moves what they wrote onto the media ring.
 * The count of that ring is the check, because an export reads the records of
 * that ring.
 */
static void test_pkt_trace_live(void)
{
	struct trace_ring_counts before;
	struct trace_ring_counts after;
	struct trace_ring_counts rest;
	struct hal_media m;
	bool done = false;
	unsigned int i;
	struct probe p;
	struct wire w;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the live drain case needs a started backend and a wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	CASE("the packet trace bit arms the drain timer and the producers");
	if (!pkt_rings_open()) {
		tu_skip("the build carries no udebug support");
		wire_close(&w);
		probe_down(&p);
		return;
	}

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);
	CHECK(media_up(&p, &w, &m));
	probe_wait(&p, 300);
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_PKT_TRACE) == HAL_OK);
	CHECK_UINT(p.h->debug, HAL_DEBUG_PKT_TRACE);
	CHECK(p.h->pkt_trace.pending);
	CHECK(!p.h->jb_trace.pending);
	CHECK_UINT(atomic_load(&p.h->s->debug), UDSP_DEBUG_PKT_TRACE);

	CASE("the drain writes both directions of the run to the media ring");
	trace_ring_counts(TRACE_RING_RTP, &before);
	CHECK_UINT(trace_ring_export_open(pkt_path), 0);
	for (i = 0; i < 20u; i++) {
		wire_send(&w, 0, 20);
		probe_wait(&p, 20);
	}
	/* The first drain after the run can come before the library records
	 * the last received packet, so the case waits for a second drain one
	 * period later. */
	trace_ring_counts(TRACE_RING_RTP, &after);
	rtp_ring_wait(&p, after.written, 2000);
	trace_ring_counts(TRACE_RING_RTP, &after);
	rtp_ring_wait(&p, after.written, 2000);
	trace_ring_counts(TRACE_RING_RTP, &after);
	CHECK(after.written - before.written >= 40u);
	CHECK_UINT(p.h->pkt_ring_fail, 0);
	/* Each direction on its own, because a drain that lost one of them
	 * still grows the count by the other. */
	CHECK_UINT(trace_ring_export_step(PKT_SEEN_MAX, &done), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(pkt_file_parse(pkt_path));
	CHECK(pkt_count_dir(PKT_IFACE_RTP, TRACE_PKT_TX) >= 20u);
	CHECK(pkt_count_dir(PKT_IFACE_RTP, TRACE_PKT_RX) >= 20u);

	/*
	 * udsp_debug_set writes the whole library mask with one store, so a
	 * push made while the trace runs has to carry its bit: without it the
	 * producers stop behind a timer that still turns and a record of the
	 * daemon that still says armed.
	 */
	CASE("a repeated push and an added domain leave the producers running");
	trace_ring_counts(TRACE_RING_RTP, &before);
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_PKT_TRACE) == HAL_OK);
	CHECK(p.h->pkt_trace.pending);
	CHECK_UINT(atomic_load(&p.h->s->debug), UDSP_DEBUG_PKT_TRACE);
	for (i = 0; i < 10u; i++) {
		wire_send(&w, 0, 20);
		probe_wait(&p, 20);
	}
	rtp_ring_wait(&p, before.written, 2000);
	trace_ring_counts(TRACE_RING_RTP, &after);
	CHECK(after.written > before.written);

	CASE("and a push that adds the jitter buffer domain keeps them too");
	trace_ring_counts(TRACE_RING_RTP, &before);
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_PKT_TRACE |
					      HAL_DEBUG_JB_TRACE) == HAL_OK);
	CHECK(p.h->pkt_trace.pending);
	CHECK(p.h->jb_trace.pending);
	CHECK_UINT(atomic_load(&p.h->s->debug),
		   UDSP_DEBUG_PKT_TRACE | UDSP_DEBUG_JB_TRACE);
	for (i = 0; i < 10u; i++) {
		wire_send(&w, 0, 20);
		probe_wait(&p, 20);
	}
	rtp_ring_wait(&p, before.written, 2000);
	trace_ring_counts(TRACE_RING_RTP, &after);
	CHECK(after.written > before.written);

	CASE("clearing the bit cancels the timer and stops the trace");
	CHECK(hal_backend_ops.debug_set(p.h, 0) == HAL_OK);
	CHECK(!p.h->pkt_trace.pending);
	CHECK_UINT(p.h->debug, 0);
	CHECK_UINT(atomic_load(&p.h->s->debug), 0);
	trace_ring_counts(TRACE_RING_RTP, &after);
	for (i = 0; i < 20u; i++) {
		wire_send(&w, 0, 20);
		probe_wait(&p, 20);
	}
	probe_settle(&p, 400);
	trace_ring_counts(TRACE_RING_RTP, &rest);
	CHECK_UINT(rest.written, after.written);

	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	pkt_rings_close();
	wire_close(&w);
	probe_down(&p);
}

/*
 * The discard that the arm makes, and the backlog that it exists for. A
 * disarm keeps the memory of the library (udsp.h udsp_pkt_trace_enable), so
 * the records that a session left sit in the rings until something empties
 * them. A bound that covered one drain period would carry them into the next
 * trace, with a stamp from long before it.
 *
 * The case cancels the backend timer white box, as the decision trace case
 * does, so the library produces while nothing consumes. The stream then
 * stops, so every record that a drain could write after that belongs to the
 * session that is over.
 */
static void test_pkt_trace_arm_discard(void)
{
	struct trace_ring_counts before;
	struct trace_ring_counts after;
	struct hal_media m;
	unsigned int i;
	struct probe p;
	struct wire w;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the pkt discard case needs a started backend and a wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	CASE("a stranded session fills both rings past one drain period");
	if (!pkt_rings_open()) {
		tu_skip("the build carries no udebug support");
		wire_close(&w);
		probe_down(&p);
		return;
	}

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);
	CHECK(media_up(&p, &w, &m));
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_PKT_TRACE) == HAL_OK);
	CHECK(p.h->pkt_trace.pending);
	uloop_timeout_cancel(&p.h->pkt_trace);
	for (i = 0; i < 480u; i++) {
		wire_send(&w, 0, 20);
		probe_wait(&p, 20);
	}
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	probe_wait(&p, 100);
	CHECK(!p.h->pkt_trace.pending);

	/* The disarm drains what its own round bound allows and leaves the
	 * rest, which is what the arm below has to throw away. */
	CASE("the arm that follows writes nothing of that backlog");
	CHECK(hal_backend_ops.debug_set(p.h, 0) == HAL_OK);
	trace_ring_counts(TRACE_RING_RTP, &before);
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_PKT_TRACE) == HAL_OK);
	CHECK(p.h->pkt_trace.pending);
	probe_settle(&p, 400);
	trace_ring_counts(TRACE_RING_RTP, &after);
	CHECK_UINT(after.written, before.written);

	CHECK(hal_backend_ops.debug_set(p.h, 0) == HAL_OK);
	pkt_rings_close();
	wire_close(&w);
	probe_down(&p);
}

/* --- the canceller capture ------------------------------------------------ */

/*
 * The arm of the library, wrapped by the link of this suite
 * (-Wl,--wrap=udsp_lec_capture_enable). Every call passes through. An arm
 * records how many records the library ring held at the moment the bit rose,
 * because the backend must discard the leftovers before that moment and
 * nothing after the arm can tell the two orders apart. A disarm is counted,
 * so a case can see the close disarm the capture. A case can replace the
 * answer of a disarm, to drive the answer to a tick that never confirmed.
 */
int __real_udsp_lec_capture_enable(udsp_t *s, bool on);
int __wrap_udsp_lec_capture_enable(udsp_t *s, bool on);

static unsigned int lec_enable_arms;
static unsigned int lec_enable_disarms;
static unsigned int lec_depth_at_arm;
static int lec_disarm_answer;

/* The discard and the final drain of the backend size their bound from its
 * own copy of the depth of the library ring. */
_Static_assert(HAL_UDSP_LEC_SLOTS == UDSP_LEC_CAPTURE_SLOTS,
	       "the backend and the library disagree on the ring depth");

/* The records the library ring holds, read without taking one. The backend
 * drains on the uloop thread and so does every case, so the read is never
 * inside a drain. */
static unsigned int lec_ring_depth(udsp_t *s)
{
	struct lec_capture_ring *r = atomic_load(&s->lec_cap);

	if (!r)
		return 0;

	return (atomic_load(&r->head) - atomic_load(&r->tail)) & r->mask;
}

int __wrap_udsp_lec_capture_enable(udsp_t *s, bool on)
{
	int rc;

	if (on && s) {
		lec_enable_arms++;
		lec_depth_at_arm = lec_ring_depth(s);
	}
	if (!on)
		lec_enable_disarms++;

	rc = __real_udsp_lec_capture_enable(s, on);
	if (!on && lec_disarm_answer)
		return lec_disarm_answer;

	return rc;
}

/* The fifth ring of five, so the fifth interface of the export. */
#define LEC_IFACE		4u
#define LEC_HDR_BYTES		64u

static uint16_t lec_rd16(const uint8_t *p)
{
	uint16_t val;

	memcpy(&val, p, sizeof(val));

	return val;
}

static uint64_t lec_rd64(const uint8_t *p)
{
	uint64_t val;

	memcpy(&val, p, sizeof(val));

	return val;
}

/* The rings a call trace with the lec option opens. The canceller ring is
 * the one under test, and the cases read its entries back from the export
 * file. */
static bool lec_rings_open(void)
{
	snprintf(pkt_path, sizeof(pkt_path), "%s/rings.pcapng", fixture_dir);
	trace_ring_init();

	return trace_ring_open(true, true) == 0;
}

/* Every field takes a value of its own, and the multi-byte ones differ in
 * each byte, so an entry that swapped two fields or dropped one cannot read
 * as correct. */
static void lec_rec_make(struct udsp_lec_capture *rec, uint8_t kind,
			 uint16_t samples)
{
	unsigned int i;

	memset(rec, 0, sizeof(*rec));
	rec->stamp_ns = PKT_STAMP_NS;
	rec->tick = 0x0102030405060708ull;
	rec->lost_before = 0x11121314u;
	rec->frames = 0x21222324u;
	rec->hold_frames = 0x31323334u;
	rec->erle_mdb = -234567;
	rec->echo_gain_micro = 0x41424344u;
	rec->coef_energy_micro = 0x51525354u;
	rec->ref_delay_samples = 0x61626364u;
	rec->gen = 0x7172;
	rec->flags = UDSP_LEC_CAPTURE_NEW | UDSP_LEC_CAPTURE_ACTIVE |
		     UDSP_LEC_CAPTURE_SKIPPED;
	rec->samples = samples;
	rec->tail_ms = 0x8182;
	rec->noise_ddbov = 0x9192;
	rec->probes = 0xa1a2;
	rec->diverge_resets = 0xb1b2;
	rec->line = 1;
	rec->kind = kind;
	rec->nlp_verdict = UDSP_LEC_CAPTURE_VERDICT_UNLIKE;
	rec->hold_left = 0xc1;
	rec->skipped = 0xd1d2;
	for (i = 0; i < UDSP_LEC_CAPTURE_SAMPLES; i++) {
		rec->ref[i] = (int16_t)(i + 7);
		rec->mic[i] = (int16_t)(2000 - (int)i);
		rec->out[i] = (int16_t)(-3 * (int)i);
	}
}

/* The header of one entry against the record it came from, at the offsets of
 * trace_ring.h, which are the offsets udsp/src/tools/lec-capture.py reads. */
static void lec_entry_check(const struct pkt_entry *e,
			    const struct udsp_lec_capture *rec,
			    unsigned int kind)
{
	const uint8_t *d = e->data;

	CHECK(e->len >= LEC_HDR_BYTES);
	if (e->len < LEC_HDR_BYTES)
		return;

	CHECK_UINT(lec_rd64(&d[0]), rec->stamp_ns);
	CHECK_UINT(lec_rd64(&d[8]), rec->tick);
	CHECK_UINT(pkt_rd32(&d[16]), rec->lost_before);
	CHECK_UINT(pkt_rd32(&d[20]), rec->frames);
	CHECK_UINT(pkt_rd32(&d[24]), rec->hold_frames);
	CHECK((int32_t)pkt_rd32(&d[28]) == rec->erle_mdb);
	CHECK_UINT(pkt_rd32(&d[32]), rec->echo_gain_micro);
	CHECK_UINT(pkt_rd32(&d[36]), rec->coef_energy_micro);
	CHECK_UINT(pkt_rd32(&d[40]), rec->ref_delay_samples);
	CHECK_UINT(lec_rd16(&d[44]), rec->gen);
	CHECK_UINT(lec_rd16(&d[46]), rec->flags);
	CHECK_UINT(lec_rd16(&d[48]), rec->samples);
	CHECK_UINT(lec_rd16(&d[50]), rec->tail_ms);
	CHECK_UINT(lec_rd16(&d[52]), rec->noise_ddbov);
	CHECK_UINT(lec_rd16(&d[54]), rec->probes);
	CHECK_UINT(lec_rd16(&d[56]), rec->diverge_resets);
	CHECK_UINT(d[58], rec->line);
	CHECK_UINT(d[59], kind);
	CHECK_UINT(d[60], rec->nlp_verdict);
	CHECK_UINT(d[61], rec->hold_left);
	CHECK_UINT(lec_rd16(&d[62]), rec->skipped);
}

static bool lec_plane_is(const struct pkt_entry *e, unsigned int nth,
			 const int16_t *want, uint16_t samples)
{
	size_t bytes = (size_t)samples * sizeof(int16_t);
	size_t at = LEC_HDR_BYTES + nth * bytes;

	if (e->len < at + bytes)
		return false;

	return !memcmp(&e->data[at], want, bytes);
}

/* Turns the loop until a drain moves the canceller ring past floor, the rule
 * of rtp_ring_wait. */
static void lec_ring_wait(struct probe *p, uint64_t floor, unsigned int ms)
{
	uint64_t stop = mono_us() + (uint64_t)ms * 1000u;
	struct trace_ring_counts c;

	for (;;) {
		trace_ring_counts(TRACE_RING_LEC, &c);
		if (c.written > floor || mono_us() >= stop)
			return;

		probe_poll(p);
		uloop_pump(5);
	}
}

/* Turns the loop until the canceller ring took at least want records. */
static void lec_written_wait(struct probe *p, uint64_t want, unsigned int ms)
{
	uint64_t stop = mono_us() + (uint64_t)ms * 1000u;
	struct trace_ring_counts c;

	for (;;) {
		trace_ring_counts(TRACE_RING_LEC, &c);
		if (c.written >= want || mono_us() >= stop)
			return;

		probe_poll(p);
		uloop_pump(5);
	}
}

/* Turns the loop until the library ring holds at least want records, for a
 * case that stopped the drain to leave a backlog. */
static void lec_backlog_wait(struct probe *p, unsigned int want,
			     unsigned int ms)
{
	uint64_t stop = mono_us() + (uint64_t)ms * 1000u;

	while (lec_ring_depth(p->h->s) < want && mono_us() < stop) {
		probe_poll(p);
		uloop_pump(5);
	}
}

/* A running canceller on line 0: the channel asks for one, the stream that
 * makes the instance runs, and the line is off hook so the filter runs on
 * every frame. The fake line is silent. */
static bool lec_canceller_up(struct probe *p, struct wire *w)
{
	struct hal_channel_cfg ccfg;
	struct hal_media m;

	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.detect_mask = HAL_DETECT_DTMF;
	ccfg.ec_enable = true;
	ccfg.ec_nlp_enable = true;
	ccfg.ec_tail_ms = 32;
	if (hal_backend_ops.channel_config_set(p->h, 0, &ccfg) != HAL_OK)
		return false;

	media_build(&m, w, HAL_CODEC_PCMU, 0, 20);
	if (!media_up(p, w, &m))
		return false;

	udsp_slic_fake_hook_set(p->h->s->slic, 0, true);
	probe_wait(p, 400);

	return true;
}

/*
 * A crafted record onto the canceller ring, and the entry it became. The
 * copy is field by field between two types. Only a record whose fields all
 * differ can show a pair that swapped or a field the copy left out.
 */
static void test_lec_capture_render(void)
{
	struct udsp_lec_capture rec;
	const struct pkt_entry *e;
	uint64_t fail_before;
	bool done = false;
	struct probe p;

	if (!probe_up(&p)) {
		CASE("the canceller render case needs a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	CASE("a RUN record reaches the canceller ring field by field");
	if (!lec_rings_open()) {
		tu_skip("the build carries no udebug support");
		probe_down(&p);
		return;
	}

	CHECK_UINT(trace_ring_export_open(pkt_path), 0);
	lec_rec_make(&rec, UDSP_LEC_CAPTURE_RUN, 160);
	lec_capture_record(p.h, &rec, 0);
	CHECK_UINT(trace_ring_export_step(PKT_SEEN_MAX, &done), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(pkt_file_parse(pkt_path));
	CHECK_UINT(pkt_count(LEC_IFACE), 1);
	e = pkt_nth(LEC_IFACE, 0);
	CHECK_UINT(e->len, 1024);
	CHECK_UINT(e->stamp, PKT_STAMP_US);
	lec_entry_check(e, &rec, 0);

	CASE("and its three planes follow the header in order");
	CHECK(lec_plane_is(e, 0, rec.ref, 160));
	CHECK(lec_plane_is(e, 1, rec.mic, 160));
	CHECK(lec_plane_is(e, 2, rec.out, 160));

	CASE("a FEED record carries ref and mic, and an END the header alone");
	CHECK_UINT(trace_ring_export_open(pkt_path), 0);
	lec_rec_make(&rec, UDSP_LEC_CAPTURE_FEED, 80);
	lec_capture_record(p.h, &rec, 0);
	lec_rec_make(&rec, UDSP_LEC_CAPTURE_END, 80);
	lec_capture_record(p.h, &rec, 0);
	CHECK_UINT(trace_ring_export_step(PKT_SEEN_MAX, &done), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(pkt_file_parse(pkt_path));
	CHECK_UINT(pkt_count(LEC_IFACE), 2);
	e = pkt_nth(LEC_IFACE, 0);
	CHECK_UINT(e->len, 64 + 2 * 2 * 80);
	lec_rec_make(&rec, UDSP_LEC_CAPTURE_FEED, 80);
	lec_entry_check(e, &rec, 1);
	CHECK(lec_plane_is(e, 0, rec.ref, 80));
	CHECK(lec_plane_is(e, 1, rec.mic, 80));
	e = pkt_nth(LEC_IFACE, 1);
	CHECK_UINT(e->len, 64);
	lec_rec_make(&rec, UDSP_LEC_CAPTURE_END, 80);
	lec_entry_check(e, &rec, 2);

	/* The record carries a monotonic instant and the ring a wall clock one,
	 * so the drain's offset converts it, in either direction. */
	CASE("the drain's clock offset moves the canceller stamp both ways");
	CHECK_UINT(trace_ring_export_open(pkt_path), 0);
	lec_rec_make(&rec, UDSP_LEC_CAPTURE_END, 160);
	lec_capture_record(p.h, &rec, PKT_OFFSET_NS);
	lec_capture_record(p.h, &rec, -PKT_OFFSET_NS);
	CHECK_UINT(trace_ring_export_step(PKT_SEEN_MAX, &done), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(pkt_file_parse(pkt_path));
	CHECK_UINT(pkt_count(LEC_IFACE), 2);
	CHECK_UINT(pkt_nth(LEC_IFACE, 0)->stamp, PKT_STAMP_US + PKT_OFFSET_US);
	CHECK_UINT(pkt_nth(LEC_IFACE, 1)->stamp, PKT_STAMP_US - PKT_OFFSET_US);
	/* the header keeps the monotonic instant of the tick */
	CHECK_UINT(lec_rd64(pkt_nth(LEC_IFACE, 1)->data), PKT_STAMP_NS);

	CASE("a record the ring refuses is counted and writes nothing");
	fail_before = p.h->lec_ring_fail;
	CHECK_UINT(trace_ring_export_open(pkt_path), 0);
	lec_rec_make(&rec, UDSP_LEC_CAPTURE_RUN, 120);
	lec_capture_record(p.h, &rec, 0);
	CHECK_UINT(p.h->lec_ring_fail, fail_before + 1u);

	CASE("a record of a kind the backend cannot name is dropped");
	lec_rec_make(&rec, 9, 80);
	lec_capture_record(p.h, &rec, 0);
	CHECK_UINT(p.h->lec_ring_fail, fail_before + 1u);
	CHECK_UINT(trace_ring_export_step(PKT_SEEN_MAX, &done), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(pkt_file_parse(pkt_path));
	CHECK_UINT(pkt_count(LEC_IFACE), 0);

	pkt_rings_close();
	probe_down(&p);
}

/* A backlog past the HAL_UDSP_LEC_ROUNDS x HAL_UDSP_LEC_BATCH records that
 * one timer period drains, and under the 1023 the library ring holds. */
#define LEC_BACKLOG_WIDE	600u
_Static_assert(LEC_BACKLOG_WIDE > HAL_UDSP_LEC_ROUNDS * HAL_UDSP_LEC_BATCH &&
	       LEC_BACKLOG_WIDE < HAL_UDSP_LEC_SLOTS - 1u,
	       "the wide backlog must pass one drain period and fit the ring");

/*
 * The live drain. The bit reaches the library through debug_set. The
 * canceller of a running line fills the library ring, and the timer moves
 * the records onto the canceller ring.
 */
static void test_lec_capture_live(void)
{
	struct trace_ring_counts before, after, rest;
	unsigned int i, runs = 0, wide = 0;
	unsigned int depth, arms, disarms;
	bool done = false;
	struct probe p;
	struct wire w;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the canceller live case needs a started backend and a "
		     "wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	CASE("the canceller capture bit arms the drain timer and the library "
	     "bit");
	if (!lec_rings_open()) {
		tu_skip("the build carries no udebug support");
		wire_close(&w);
		probe_down(&p);
		return;
	}

	CHECK(lec_canceller_up(&p, &w));
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_LEC_CAPTURE) == HAL_OK);
	CHECK_UINT(p.h->debug, HAL_DEBUG_LEC_CAPTURE);
	CHECK(p.h->lec_capture.pending);
	CHECK(!p.h->pkt_trace.pending);
	CHECK(!p.h->jb_trace.pending);
	CHECK_UINT(atomic_load(&p.h->s->debug), UDSP_DEBUG_LEC_CAPTURE);

	CASE("the drain writes the RUN records of the running canceller");
	trace_ring_counts(TRACE_RING_LEC, &before);
	CHECK_UINT(trace_ring_export_open(pkt_path), 0);
	lec_ring_wait(&p, before.written, 2000);
	trace_ring_counts(TRACE_RING_LEC, &after);
	CHECK(after.written - before.written >= 10u);
	CHECK_UINT(p.h->lec_ring_fail, 0);
	CHECK_UINT(trace_ring_export_step(PKT_SEEN_MAX, &done), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(pkt_file_parse(pkt_path));
	CHECK(pkt_count(LEC_IFACE) >= 10u);
	for (i = 0; i < pkt_count(LEC_IFACE); i++) {
		const struct pkt_entry *e = pkt_nth(LEC_IFACE, i);

		if (e->len == 64 + 3 * 2 * 80 && e->data[59] == 0 &&
		    e->data[58] == 0)
			runs++;
		if (e->len > 64 + 3 * 2 * 80)
			wide++;
	}
	/* The line runs at 8 kHz and is off hook with its stream running, so
	 * every tick runs the filter on line 0. */
	CHECK_UINT(runs, pkt_count(LEC_IFACE));
	CHECK_UINT(wide, 0);

	/*
	 * udsp_debug_set writes the whole library mask with one store, so a
	 * push made while the capture runs has to carry its bit: without it
	 * the producer stops behind a timer that still turns.
	 */
	CASE("a repeated push leaves the canceller producer running");
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_LEC_CAPTURE) == HAL_OK);
	CHECK(p.h->lec_capture.pending);
	CHECK_UINT(atomic_load(&p.h->s->debug), UDSP_DEBUG_LEC_CAPTURE);
	trace_ring_counts(TRACE_RING_LEC, &before);
	lec_ring_wait(&p, before.written, 2000);
	trace_ring_counts(TRACE_RING_LEC, &after);
	CHECK(after.written > before.written);

	CASE("and a push that adds the jitter buffer domain keeps it too");
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_LEC_CAPTURE |
					      HAL_DEBUG_JB_TRACE) == HAL_OK);
	CHECK(p.h->lec_capture.pending);
	CHECK(p.h->jb_trace.pending);
	CHECK_UINT(atomic_load(&p.h->s->debug),
		   UDSP_DEBUG_LEC_CAPTURE | UDSP_DEBUG_JB_TRACE);
	trace_ring_counts(TRACE_RING_LEC, &before);
	lec_ring_wait(&p, before.written, 2000);
	trace_ring_counts(TRACE_RING_LEC, &after);
	CHECK(after.written > before.written);

	/* The timer stops white box, so the records the drain did not take yet
	 * wait in the library ring. A push while armed must not run the arm
	 * again: its discard would throw those records away. */
	CASE("a push while armed arms the library once and loses no record");
	uloop_timeout_cancel(&p.h->lec_capture);
	lec_backlog_wait(&p, 50, 2000);
	depth = lec_ring_depth(p.h->s);
	arms = lec_enable_arms;
	CHECK(depth >= 50u);
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_LEC_CAPTURE) == HAL_OK);
	CHECK_UINT(lec_enable_arms, arms);
	CHECK(lec_ring_depth(p.h->s) >= depth);

	/*
	 * The records of the last periods wait in the library ring as the last
	 * records of a call do at a freeze. The backlog passes the 256 records
	 * that one timer period drains, so a final drain bounded by one period
	 * leaves records behind. The clear must take every one of them onto
	 * the ring.
	 */
	CASE("a clear stops the drain and the final drain empties the ring");
	lec_backlog_wait(&p, LEC_BACKLOG_WIDE, 4000);
	CHECK(lec_ring_depth(p.h->s) >= LEC_BACKLOG_WIDE);
	trace_ring_counts(TRACE_RING_LEC, &before);
	CHECK(hal_backend_ops.debug_set(p.h, 0) == HAL_OK);
	CHECK(!p.h->lec_capture.pending);
	CHECK_UINT(p.h->debug, 0);
	CHECK_UINT(atomic_load(&p.h->s->debug), 0);
	trace_ring_counts(TRACE_RING_LEC, &after);
	CHECK(after.written - before.written >= LEC_BACKLOG_WIDE);
	CHECK_UINT(lec_ring_depth(p.h->s), 0);

	CASE("and nothing reaches either ring after the clear");
	probe_settle(&p, 400);
	trace_ring_counts(TRACE_RING_LEC, &rest);
	CHECK_UINT(rest.written, after.written);
	CHECK_UINT(lec_ring_depth(p.h->s), 0);

	/* A tick that never confirms the clear leaves the backend unsure that
	 * no record is half written, and the backlog still belongs to the
	 * trace. */
	CASE("a disarm the tick never confirmed warns once and still drains");
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_LEC_CAPTURE) == HAL_OK);
	CHECK(p.h->lec_capture.pending);
	uloop_timeout_cancel(&p.h->lec_capture);
	lec_backlog_wait(&p, 50, 2000);
	trace_ring_counts(TRACE_RING_LEC, &before);
	jbtrace_begin();
	lec_disarm_answer = -ETIMEDOUT;
	CHECK(hal_backend_ops.debug_set(p.h, 0) == HAL_OK);
	lec_disarm_answer = 0;
	CHECK_UINT(jbtrace_count("did not confirm the end of the canceller "
				 "capture"), 1);
	jbtrace_end();
	trace_ring_counts(TRACE_RING_LEC, &after);
	CHECK(after.written - before.written >= 50u);
	CHECK_UINT(lec_ring_depth(p.h->s), 0);

	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	pkt_rings_close();
	wire_close(&w);
	probe_down(&p);

	/*
	 * The allocation runs only for a handle that holds no ring, so the
	 * refusal needs a fresh one. A refused arm takes the bit out of the
	 * stored mask, so the next push tries again.
	 */
	CASE("a refused arm clears the bit, counts and warns");
	setenv("UDSP_TEST_LEC_CAPTURE_ALLOC_FAIL", "1", 1);
	if (!probe_up(&p)) {
		CHECK(false);
		unsetenv("UDSP_TEST_LEC_CAPTURE_ALLOC_FAIL");
		probe_down(&p);
		return;
	}
	jbtrace_begin();
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_LEC_CAPTURE) == HAL_OK);
	CHECK_UINT(p.h->debug, 0);
	CHECK_UINT(p.h->lec_arm_fail, 1);
	CHECK(!p.h->lec_capture.pending);
	CHECK_UINT(atomic_load(&p.h->s->debug), 0);
	CHECK_UINT(jbtrace_count("the canceller capture found no memory"), 1);
	jbtrace_end();
	unsetenv("UDSP_TEST_LEC_CAPTURE_ALLOC_FAIL");

	CASE("and the next push arms it");
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_LEC_CAPTURE) == HAL_OK);
	CHECK_UINT(p.h->debug, HAL_DEBUG_LEC_CAPTURE);
	CHECK(p.h->lec_capture.pending);
	CHECK_UINT(p.h->lec_arm_fail, 1);
	CHECK_UINT(atomic_load(&p.h->s->debug), UDSP_DEBUG_LEC_CAPTURE);

	/*
	 * The close runs with the capture armed. It must disarm the library
	 * and cancel the drain timer before it frees the handle. The loop then
	 * turns for longer than the drain period, the way test_jb_trace_close
	 * turns it: a timer the close left armed fires there, against a handle
	 * that is gone, and the sanitizer run names it. The sentinel proves the
	 * window was turned.
	 */
	CASE("a close with the capture armed disarms it and leaves no timer");
	disarms = lec_enable_disarms;
	probe_down(&p);
	CHECK(lec_enable_disarms > disarms);
	jbtrace_sentinel = 0;
	jbtrace_beat.cb = jbtrace_beat_expire;
	uloop_timeout_set(&jbtrace_beat, HAL_UDSP_TRACE_MS / 2);
	uloop_pump(HAL_UDSP_TRACE_MS * 3u);
	uloop_timeout_cancel(&jbtrace_beat);
	CHECK(jbtrace_sentinel > 0);
}

/*
 * The discard that the arm makes, and the order it takes. The library keeps
 * its ring across a disarm and discards nothing on an arm (udsp.h
 * udsp_lec_capture_enable), so the records of an earlier session wait there.
 * The backend must throw them away before the bit rises. After it, the ring
 * can hold the NEW record of the call, the one point a replay starts from.
 *
 * The earlier session runs the library white box, so its records stay in the
 * ring with nothing to drain them. It ends with the release of its canceller.
 * Its backlog passes the records of one drain period, so a discard bounded by
 * one period leaves records behind.
 */
static void test_lec_capture_arm_discard(void)
{
	struct trace_ring_counts before, after;
	unsigned int i, ends = 0, foreign = 0;
	bool done = false;
	uint16_t gen;
	struct probe p;
	struct wire w;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the canceller discard case needs a started backend and a "
		     "wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	CASE("an earlier session leaves its records in the library ring");
	if (!lec_rings_open()) {
		tu_skip("the build carries no udebug support");
		wire_close(&w);
		probe_down(&p);
		return;
	}

	CHECK(lec_canceller_up(&p, &w));
	CHECK_UINT(udsp_lec_capture_enable(p.h->s, true), 0);
	lec_backlog_wait(&p, LEC_BACKLOG_WIDE, 4000);
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	probe_wait(&p, 200);
	CHECK_UINT(udsp_lec_capture_enable(p.h->s, false), 0);
	CHECK(lec_ring_depth(p.h->s) >= LEC_BACKLOG_WIDE);
	CHECK(!p.h->lec_capture.pending);

	CASE("the arm empties the library ring before the bit rises");
	CHECK_UINT(trace_ring_export_open(pkt_path), 0);
	trace_ring_counts(TRACE_RING_LEC, &before);
	lec_enable_arms = 0;
	lec_depth_at_arm = ~0u;
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_LEC_CAPTURE) == HAL_OK);
	CHECK_UINT(lec_enable_arms, 1);
	CHECK_UINT(lec_depth_at_arm, 0);
	trace_ring_counts(TRACE_RING_LEC, &after);
	CHECK_UINT(after.written, before.written);

	/* A second stream configuration makes a new canceller, so its first
	 * record carries NEW. */
	CASE("the first record of the new session is the NEW of the new call");
	wire_close(&w);
	CHECK(wire_open(&w));
	CHECK(lec_canceller_up(&p, &w));
	lec_ring_wait(&p, before.written, 2000);
	CHECK_UINT(trace_ring_export_step(PKT_SEEN_MAX, &done), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(pkt_file_parse(pkt_path));
	CHECK(pkt_count(LEC_IFACE) >= 1u);
	CHECK(pkt_nth(LEC_IFACE, 0)->len >= LEC_HDR_BYTES);
	CHECK(lec_rd16(&pkt_nth(LEC_IFACE, 0)->data[46]) &
	      UDSP_LEC_CAPTURE_NEW);

	/*
	 * The END of the earlier canceller and every record before the arm
	 * belong to the session that is over. The generation of the line moves
	 * on each new canceller (udsp.h struct udsp_lec_capture), so a record
	 * of the earlier session carries another generation than the NEW. The
	 * check reads no clock, so a loaded host cannot move it.
	 */
	CASE("and no record of the earlier session reaches the trace");
	gen = lec_rd16(&pkt_nth(LEC_IFACE, 0)->data[44]);
	for (i = 0; i < pkt_count(LEC_IFACE); i++) {
		const struct pkt_entry *e = pkt_nth(LEC_IFACE, i);

		if (e->len < LEC_HDR_BYTES)
			continue;
		if (e->data[59] == 2)
			ends++;
		if (lec_rd16(&e->data[44]) != gen)
			foreign++;
	}
	CHECK_UINT(ends, 0);
	CHECK_UINT(foreign, 0);

	CHECK(hal_backend_ops.debug_set(p.h, 0) == HAL_OK);
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	pkt_rings_close();
	wire_close(&w);
	probe_down(&p);
}

/*
 * One whole lineage of a live canceller, exported the way a call trace
 * exports it. The ctest entry lec_capture_e2e reads the file with
 * udsp/src/tools/lec-capture.py and replays the line with lec_replay, which
 * runs the same canceller code. The arm comes before the stream makes the
 * instance, so the first record is NEW, and the stop releases the instance,
 * so the last record is END. The body of a replay starts 15 s into the
 * lineage, and this lineage runs past it. The near end carries a triangle
 * wave, so the microphone and the output planes are not silent. A dial tone
 * plays for six seconds inside the body: the tone enters the reference of the
 * canceller, so the replay compares verdicts other than quiet.
 *
 * The limit of the case: the library ring holds 1023 records, which is about
 * one wall second of this paced line. A host that stalls the uloop thread for
 * longer loses records, and the extractor then reports the loss and fails
 * the entry. The waits count records and engine periods, not wall time, so a
 * slow host alone moves nothing.
 *
 * UVOICE_LEC_CAPTURE_EXPORT names the file to keep. Without it the case
 * writes into the fixture directory and removes the file.
 */
#define LEC_EXPORT_TONE_FROM	1600u
#define LEC_EXPORT_TONE_TO	2200u
#define LEC_EXPORT_RECORDS	2400u
#define LEC_EXPORT_WAIT_MS	20000u

static void test_lec_capture_export(void)
{
	const char *keep = getenv("UVOICE_LEC_CAPTURE_EXPORT");
	struct trace_ring_counts c;
	struct probe p;
	struct wire w;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the canceller export case needs a started backend and a "
		     "wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	CASE("a whole lineage of a live canceller reaches the export");
	if (!lec_rings_open()) {
		tu_skip("the build carries no udebug support");
		wire_close(&w);
		probe_down(&p);
		return;
	}

	/* A path the buffer cannot hold would export somewhere else. */
	if (keep && *keep &&
	    (size_t)snprintf(pkt_path, sizeof(pkt_path), "%s", keep) >=
	    sizeof(pkt_path)) {
		CHECK(false);
		trace_ring_close();
		wire_close(&w);
		probe_down(&p);
		return;
	}
	CHECK_UINT(trace_ring_export_open(pkt_path), 0);
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_LEC_CAPTURE) == HAL_OK);
	atomic_store(&ts_be.near_peak, 4000);
	CHECK(lec_canceller_up(&p, &w));
	lec_written_wait(&p, LEC_EXPORT_TONE_FROM, LEC_EXPORT_WAIT_MS);
	CHECK(hal_backend_ops.tone_play(p.h, 0, HAL_TONE_DIAL, 0) == HAL_OK);
	lec_written_wait(&p, LEC_EXPORT_TONE_TO, LEC_EXPORT_WAIT_MS);
	CHECK(hal_backend_ops.tone_stop(p.h, 0) == HAL_OK);
	lec_written_wait(&p, LEC_EXPORT_RECORDS, LEC_EXPORT_WAIT_MS);
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	probe_wait(&p, 100);
	CHECK(hal_backend_ops.debug_set(p.h, 0) == HAL_OK);
	atomic_store(&ts_be.near_peak, 0);
	trace_ring_counts(TRACE_RING_LEC, &c);
	CHECK(c.written >= LEC_EXPORT_RECORDS);
	CHECK_UINT(p.h->lec_ring_fail, 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);

	CASE("and the export holds every record the ring took");
	trace_ring_counts(TRACE_RING_LEC, &c);
	CHECK_UINT(c.exported, c.written);
	CHECK_UINT(c.overwritten, 0);
	CHECK_UINT(c.truncated, 0);

	trace_ring_close();
	if (!keep || !*keep)
		unlink(pkt_path);
	wire_close(&w);
	probe_down(&p);
}

/*
 * The stream diagnostic getter. The counters belong to the library. This case
 * checks the ladder: which stream answers, which one refuses, and that a
 * plain RTP call carries no encryption figure at all.
 */
static void test_stream_diag_get(void)
{
	struct hal_stream_diag diag;
	struct hal_media m;
	struct probe p;
	struct wire w;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the stream diag case needs a started backend and a wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);

	CASE("a running stream answers its counters and keys nothing");
	CHECK(media_up(&p, &w, &m));
	probe_wait(&p, 300);
	memset(&diag, 0xa5, sizeof(diag));
	CHECK(hal_backend_ops.stream_diag_get(p.h, 0, 0, &diag) == HAL_OK);
	CHECK_UINT(diag.srtp_protect_ok, 0);
	CHECK_UINT(diag.srtp_unprotect_ok, 0);
	CHECK_UINT(diag.srtp_unprotect_auth_fail, 0);
	CHECK_UINT(diag.srtp_unprotect_replay, 0);

	CASE("a healthy exchange fails no send and refuses no datagram");
	wire_talk(&p, &w, 0, 20);
	probe_wait(&p, 300);
	CHECK(hal_backend_ops.stream_diag_get(p.h, 0, 0, &diag) == HAL_OK);
	CHECK_UINT(diag.tx_fail, 0);
	CHECK_UINT(diag.net_drops, 0);
	CHECK_UINT(diag.net_stale, 0);
	CHECK_UINT(diag.net_trunc, 0);

	CASE("an idle stream is refused with the record cleared");
	memset(&diag, 0xa5, sizeof(diag));
	CHECK(hal_backend_ops.stream_diag_get(p.h, 0, 1, &diag) ==
	      HAL_UNSUPPORTED);
	CHECK_UINT(diag.net_drops, 0);
	CHECK_UINT(diag.punch_sent, 0);
	CHECK_UINT(diag.srtp_unprotect_replay, 0);

	CASE("a line the board does not carry is refused the same way");
	memset(&diag, 0xa5, sizeof(diag));
	CHECK(hal_backend_ops.stream_diag_get(p.h, HAL_LINE_MAX, 0, &diag) ==
	      HAL_UNSUPPORTED);
	CHECK_UINT(diag.net_drops, 0);

	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	wire_close(&w);
	probe_down(&p);
}

static enum hal_status engine_diag_wait(struct probe *p,
					struct hal_engine_diag *eng,
					unsigned int ms)
{
	uint64_t stop = mono_us() + (uint64_t)ms * 1000u;
	enum hal_status st;

	for (;;) {
		st = hal_backend_ops.engine_diag_get(p->h, eng);
		if (st == HAL_OK || mono_us() >= stop)
			return st;

		probe_poll(p);
		uloop_pump(5);
	}
}

/*
 * The engine snapshot. The library publishes it once per watchdog period.
 * The two answers are the two states of that publication: a backend with no
 * engine yet, and one that ran a period with it. The case polls on the wall
 * clock, because the watchdog counts wall milliseconds and not engine
 * milliseconds.
 */
static void test_engine_diag_get(void)
{
	struct hal_engine_diag eng;
	struct probe p;
	bool opened;

	/* udsp_open() copies the cadence into its emitter, so the short
	 * cadence reaches this handle alone. test_core of libudsp checks the
	 * production default. */
	setenv("UDSP_WATCHDOG_MS", "100", 1);
	opened = probe_open(&p);
	unsetenv("UDSP_WATCHDOG_MS");
	if (!opened) {
		CASE("the engine diag case needs an attached backend");
		CHECK(false);
		return;
	}

	CASE("a backend with no engine yet answers no snapshot");
	memset(&eng, 0xa5, sizeof(eng));
	CHECK(hal_backend_ops.engine_diag_get(p.h, &eng) == HAL_UNSUPPORTED);
	CHECK_UINT(eng.ticks, 0);
	CHECK_UINT(eng.running, 0);
	CHECK_UINT(eng.period_us, 0);

	CASE("the engine answers one published snapshot after a watchdog "
	     "period");
	CHECK(probe_start(&p));
	CHECK(engine_diag_wait(&p, &eng, 5000) == HAL_OK);
	CHECK_UINT(eng.running, 1);
	CHECK_UINT(eng.stopped_fatal, 0);
	CHECK(eng.ticks > 0);
	CHECK(eng.period_us > 0);
	CHECK(eng.serial > 0);

	probe_down(&p);
}

/*
 * SDES SRTP on the wire. The check is the packet length: the _80 suite
 * appends a ten byte authentication tag to every packet (RFC 4568 6.2). A
 * stream that keyed nothing is ten bytes shorter, and reads as plain RTP.
 * That is exactly the silent downgrade that hal.h names as the hazard.
 */
static void test_media_srtp(void)
{
	static const size_t plain = WIRE_RTP_HDR + 20u * WIRE_SAMPLES_PER_MS;
	uint8_t key_tx[30];
	uint8_t key_rx[30];
	struct wire_seen seen;
	struct hal_media m;
	struct probe p;
	struct wire w;
	unsigned int i;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the srtp case needs a started backend and a wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	CASE("the attached library carries srtp at all");
	CHECK((hal_backend_ops.cap_get(p.h)->features & HAL_FEAT_SRTP) != 0);

	/* Distinct, non-zero key material, so a stream keyed with the wrong one
	 * of the two would still be visible as a length change. */
	for (i = 0; i < sizeof(key_tx); i++) {
		key_tx[i] = (uint8_t)(0x10u + i);
		key_rx[i] = (uint8_t)(0x90u + i);
	}

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);
	m.srtp.suite = HAL_SRTP_AES_CM_128_SHA1_80;
	memcpy(m.srtp.key_tx, key_tx, sizeof(key_tx));
	memcpy(m.srtp.key_rx, key_rx, sizeof(key_rx));

	CASE("a keyed stream configures and starts");
	CHECK(media_up(&p, &w, &m));
	probe_wait(&p, 300);

	CASE("the packets on the wire carry the srtp authentication tag");
	wire_drain(&w, &seen);
	CHECK(seen.packets > 0);
	CHECK_UINT(seen.last_len, plain + WIRE_SRTP_80_TAG);

	/*
	 * The half this backend can be held to. It keeps no copy of the
	 * description at all, so no key can outlive the call in it; the stack
	 * copy the library call is made from is wiped where it dies.
	 */
	CASE("no key byte survives anywhere in the backend's own state");
	CHECK(memmem(p.h, sizeof(*p.h), key_tx, sizeof(key_tx)) == NULL);
	CHECK(memmem(p.h, sizeof(*p.h), key_rx, sizeof(key_rx)) == NULL);

	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);

	CASE("a plain stream on the same pair is ten bytes shorter again");
	m.srtp.suite = HAL_SRTP_NONE;
	memset(m.srtp.key_tx, 0, sizeof(m.srtp.key_tx));
	memset(m.srtp.key_rx, 0, sizeof(m.srtp.key_rx));
	CHECK(media_up(&p, &w, &m));
	probe_wait(&p, 300);
	wire_drain(&w, &seen);
	CHECK(seen.packets > 0);
	CHECK_UINT(seen.last_len, plain);

	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	explicit_bzero(&m, sizeof(m));
	wire_close(&w);
	probe_down(&p);
}

/*
 * Every refusal, and the one property that they all share. hal.h
 * stream_config_set rule 4 says that a refusal leaves a running stream
 * exactly as it was. The last case of the group therefore refuses on a live
 * stream, and asks the wire whether the stream survived.
 */
static void test_media_refusals(void)
{
	struct hal_channel_cfg ccfg;
	struct hal_fax_profile fax;
	struct hal_lec_stats lec;
	struct wire_seen seen;
	struct hal_media m;
	struct probe p;
	struct wire w;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the media refusal cases need a started backend and a wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);
	/* The port_free assert below asks whether the backend took the pair, so
	 * the case must not be holding it itself. */
	wire_local_release(&w);

	CASE("a suite outside the enum is refused as a capability, not a fault");
	m.srtp.suite = 99;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_UNSUPPORTED);
	m.srtp.suite = HAL_SRTP_NONE;

	CASE("a codec the library carries no coder for is refused");
	m.codec = HAL_CODEC_T38;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_UNSUPPORTED);
	m.codec = HAL_CODEC_OPUS;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_UNSUPPORTED);
	m.codec = HAL_CODEC_VBD_PCMU;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_UNSUPPORTED);
	m.codec = HAL_CODEC_PCMU;

	/* The kind selects the canceller policy of the line, so a number the
	 * enum does not name has no policy at all; the library refuses it too,
	 * but only once the sockets are made. */
	CASE("a passthrough kind the enum does not name is refused as a capability");
	m.passthrough = HAL_PASSTHROUGH_FAX + 1;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_UNSUPPORTED);
	m.passthrough = HAL_PASSTHROUGH_OFF;

	/*
	 * The backend carries IPv6 now, so what rule 4 still refuses is a shape
	 * no socket can hold and never the version itself. One socket has one
	 * family, so a description whose two ends disagree has no pair that can
	 * carry it.
	 */
	CASE("a mixed-family description is refused as a capability");
	m.local.ipver = 6;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) ==
	      HAL_UNSUPPORTED);
	m.local.ipver = 4;
	m.remote.ipver = 6;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) ==
	      HAL_UNSUPPORTED);
	m.remote.ipver = 4;

	/* hal_addr defines 4 and 6 and nothing else, so any other value names
	 * no family at all. */
	CASE("an ipver that is neither 4 nor 6 is refused");
	m.local.ipver = 0;
	m.remote.ipver = 0;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) ==
	      HAL_UNSUPPORTED);
	m.local.ipver = 99;
	m.remote.ipver = 99;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) ==
	      HAL_UNSUPPORTED);
	m.local.ipver = 4;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) ==
	      HAL_UNSUPPORTED);
	m.remote.ipver = 4;

	/* The RTCP address is a third endpoint of the same stream (RFC 3605
	 * section 2.1), and it travels on the same socket as the report, so it
	 * carries the family of the description or none at all. */
	CASE("a v6 description with a v4 rtcp_remote is refused");
	m.local.ipver = 6;
	m.remote.ipver = 6;
	m.rtcp_remote.ipver = 4;
	m.rtcp_remote.addr[0] = 127;
	m.rtcp_remote.addr[3] = 1;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) ==
	      HAL_UNSUPPORTED);
	m.local.ipver = 4;
	m.remote.ipver = 4;
	m.rtcp_remote.ipver = 6;
	memset(m.rtcp_remote.addr, 0, sizeof(m.rtcp_remote.addr));
	m.rtcp_remote.addr[15] = 1;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) ==
	      HAL_UNSUPPORTED);
	memset(&m.rtcp_remote, 0, sizeof(m.rtcp_remote));

	/*
	 * Rule 4 is an order as much as an answer, and the descriptor count is
	 * the oracle a family refusal needs: the pair a v6 description would
	 * take binds no port this case holds, so the port oracle below cannot
	 * see it. The first refusal runs before the count, so a diagnostic that
	 * opens something of its own does it outside the measurement.
	 */
	CASE("the capability test runs in front of the v6 bind, not behind it");
	{
		unsigned int before;

		m.local.ipver = 6;
		CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) ==
		      HAL_UNSUPPORTED);
		before = fd_count();
		CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) ==
		      HAL_UNSUPPORTED);
		CHECK_UINT(fd_count(), before);
		CHECK(stream_rec(&p, 0, 0)->rtp_fd == -1);
		m.local.ipver = 4;
	}

	CASE("a missing port on either end is a fault, not a missing capability");
	m.local.port = 0;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_FATAL);
	m.local.port = w.local;
	m.remote.port = 0;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_FATAL);
	m.remote.port = w.port;

	/*
	 * The same refusal with the coincidence taken away. A local port of 0
	 * derives an rtcp port of 1, which is privileged, so a backend that had
	 * lost this refusal would still fail the second bind and still answer
	 * HAL_FATAL. Naming the rtcp port removes that accident: both binds
	 * would then succeed and the call would run on an ephemeral port the
	 * far end was never told about.
	 */
	CASE("a local port of zero is refused even when the rtcp port is namable");
	{
		int spare_rtp = -1;
		int spare_rtcp = -1;
		uint16_t spare;

		CHECK(port_pair_take(AF_INET, &spare_rtp, &spare_rtcp));
		spare = sock_port(spare_rtp);
		close(spare_rtp);
		close(spare_rtcp);

		m.local.port = 0;
		m.local.rtcp_port = spare;
		CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_FATAL);
		CHECK(port_free(AF_INET, spare));
		m.local.port = w.local;
		m.local.rtcp_port = 0;
	}

	/*
	 * The rtcp port is derived as the rtp port plus one, and 65535
	 * derives 65536, which the uint16 truncates to 0. The local side would
	 * bind an ephemeral rtcp socket instead of the advertised one and the
	 * remote side would connect to port 0, which disconnects a UDP socket
	 * on Linux rather than failing, so both halves would be silently wrong.
	 */
	CASE("an rtp port whose derived rtcp port wraps to zero is refused");
	m.local.port = 65535;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_FATAL);
	m.local.port = w.local;
	m.remote.port = 65535;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_FATAL);
	m.remote.port = w.port;

	/* The same rtp port with the rtcp port named is fine, because nothing
	 * is derived: the refusal is of the wrap and not of the number. */
	CASE("the same rtp port with a named rtcp port is not refused for the wrap");
	m.remote.port = 65535;
	m.remote.rtcp_port = w.port;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) != HAL_FATAL);
	/* That one is accepted, so it left a configured stream and a bound pair
	 * behind it. Every refusal case below asserts on a line with neither,
	 * so the precondition is put back here rather than assumed. */
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	m.remote.port = w.port;
	m.remote.rtcp_port = 0;

	/*
	 * Rule 4 is an order, not just an answer: what the backend can program
	 * is tested before anything is touched. Occupying the local port makes
	 * that order visible, because the two answers then differ. A codec the
	 * library carries no coder for is still HAL_UNSUPPORTED, which can only
	 * happen if no bind was attempted; the very same description with a
	 * good codec is the control, and it reaches the bind and fails there.
	 */
	CASE("the capability test runs in front of the bind, not behind it");
	{
		int taken = udp_bind(w.family, w.local);

		CHECK(taken >= 0);
		m.codec = HAL_CODEC_OPUS;
		CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) ==
		      HAL_UNSUPPORTED);
		m.codec = HAL_CODEC_PCMU;
		CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_FATAL);
		if (taken >= 0)
			close(taken);
	}

	/* Every refusal so far ran before anything was touched, which is the
	 * whole of rule 4: no descriptor exists yet and the port is still
	 * free. */
	CASE("not one of those refusals made a socket");
	CHECK(stream_rec(&p, 0, 0)->rtp_fd == -1);
	CHECK(port_free(w.family, w.local));

	CASE("a line or a stream outside the capability is refused");
	CHECK(hal_backend_ops.stream_config_set(p.h, HAL_LINE_MAX, 0, &m) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.stream_start(p.h, HAL_LINE_MAX, 0) == HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.stream_stop(p.h, HAL_LINE_MAX, 0) == HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, HAL_STREAM_MAX, &m) ==
	      HAL_UNSUPPORTED);
	/* The capability's own number, which is below the hal maximum, so this
	 * one is answered by the population gate and not by the array bound. */
	CHECK(hal_backend_ops.stream_config_set(
		      p.h, 0, hal_backend_ops.cap_get(p.h)->streams_per_chan, &m) ==
	      HAL_UNSUPPORTED);

	CASE("a start with no program behind it is a fault");
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_FATAL);

	/* The library's own rule: the mix is a property of the line's media
	 * plane, and that plane exists only while a stream is configured on it
	 * (core stream_conf). Asserted before the stream comes up. */
	CASE("a conference on a line with no configured stream is refused");
	CHECK(hal_backend_ops.conf_set(p.h, 0, HAL_CONF_LOCAL_MIX) ==
	      HAL_UNSUPPORTED);

	/* The canceller is created at stream config and takes its tail from the
	 * channel, so the channel is programmed first. The kind cases below
	 * read the canceller through line_lec_stats_get. */
	CASE("the channel arms the canceller for the kind cases");
	memset(&ccfg, 0, sizeof(ccfg));
	ccfg.detect_mask = HAL_DETECT_DTMF;
	ccfg.ec_enable = true;
	ccfg.ec_nlp_enable = true;
	ccfg.ec_tail_ms = 32;
	CHECK(hal_backend_ops.channel_config_set(p.h, 0, &ccfg) == HAL_OK);
	/* Off hook, because the library runs no canceller on an open loop. */
	udsp_slic_fake_hook_set(p.h->s->slic, 0, true);
	probe_wait(&p, 400);

	CASE("the stream comes up for the live refusal cases");
	CHECK(media_up(&p, &w, &m));
	probe_wait(&p, 200);
	wire_drain(&w, NULL);

	/* The family trigger is the mismatch and not the version: the remote
	 * end stays at 4, so the two ends disagree and no pair can carry it. */
	CASE("a codec and a mixed-family refusal leave a live stream on the "
	     "wire");
	m.codec = HAL_CODEC_OPUS;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_UNSUPPORTED);
	m.codec = HAL_CODEC_PCMU;
	m.local.ipver = 6;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_UNSUPPORTED);
	m.local.ipver = 4;
	probe_wait(&p, 300);
	wire_drain(&w, &seen);
	CHECK(seen.packets > 0);
	CHECK(fd_live(stream_rec(&p, 0, 0)->rtp_fd));

	/*
	 * The two ops that now apply. Both were refusals once: the fax profile
	 * forwarded into a library that answered unsupported, and the conference
	 * refused in the backend beside a cleared HAL_FEAT_CONF. The library
	 * implements both now.
	 */
	CASE("the fax passthrough profile applies on a running stream");
	memset(&fax, 0, sizeof(fax));
	fax.mode = HAL_FAX_PASSTHROUGH;
	fax.codec = HAL_CODEC_PCMU;
	fax.codec_pt = 0;
	CHECK(hal_backend_ops.fax_profile_apply(p.h, 0, 0, &fax) == HAL_OK);

	CASE("and the stream carries audio through the profile");
	wire_drain(&w, NULL);
	probe_wait(&p, 300);
	wire_drain(&w, &seen);
	CHECK(seen.packets > 0);

	CASE("turning the profile off restores the stream and it still carries");
	fax.mode = HAL_FAX_OFF;
	CHECK(hal_backend_ops.fax_profile_apply(p.h, 0, 0, &fax) == HAL_OK);
	wire_drain(&w, NULL);
	probe_wait(&p, 300);
	wire_drain(&w, &seen);
	CHECK(seen.packets > 0);

	/*
	 * The kind on the description: the upspeed answer replaces the
	 * description the profile displaced, so a backend that kept the
	 * treatment in a profile record alone would put the canceller back on
	 * the carrier here. The canceller state is the
	 * oracle, because the policy is the only thing the kind selects. The
	 * modem kind skips the canceller whole, so the row reads ec_active 0
	 * and names that kind. The fax kind runs the filter with its processor
	 * off.
	 */
	CASE("the canceller answers before the kind reaches the description");
	probe_wait(&p, 300);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);

	CASE("the profile goes back on for the kind cases");
	fax.mode = HAL_FAX_PASSTHROUGH;
	fax.codec = HAL_CODEC_PCMU;
	CHECK(hal_backend_ops.fax_profile_apply(p.h, 0, 0, &fax) == HAL_OK);

	CASE("a description with the modem kind reads a bypassed filter");
	m.passthrough = HAL_PASSTHROUGH_MODEM;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	probe_wait(&p, 200);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	CHECK_UINT(lec.ec_active, 0);
	CHECK_UINT(lec.nlp_enabled, 0);
	CHECK_UINT(lec.passthrough, HAL_PASSTHROUGH_MODEM);

	CASE("a description that carries the fax kind runs it, so the line answers");
	m.passthrough = HAL_PASSTHROUGH_FAX;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	probe_wait(&p, 200);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	/* the fax policy is the canceller on with its processor off, and
	 * nlp_enabled is where the row says so */
	CHECK_UINT(lec.nlp_enabled, 0);
	CHECK_UINT(lec.ec_active, 1);
	CHECK_UINT(lec.passthrough, HAL_PASSTHROUGH_FAX);

	CASE("and a description that carries no kind puts the voice policy back");
	m.passthrough = HAL_PASSTHROUGH_OFF;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	probe_wait(&p, 200);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	CHECK_UINT(lec.nlp_enabled, 1);
	CHECK_UINT(lec.ec_active, 1);
	CHECK_UINT(lec.passthrough, HAL_PASSTHROUGH_OFF);

	/* udsp.h names three fax modes and hal.h four, so
	 * the V.152 request has no library member to travel on. */
	CASE("a v.152 profile is refused before the library, for want of a member");
	fax.mode = HAL_FAX_VBD;
	CHECK(hal_backend_ops.fax_profile_apply(p.h, 0, 0, &fax) == HAL_UNSUPPORTED);

	/* No fax_plugin is configured in this fixture, so no gateway is loaded
	 * and the library's T.38 capability stays clear. The library refuses
	 * this profile. */
	CASE("a t.38 profile forwards and the library's capability gate refuses it");
	fax.mode = HAL_FAX_T38;
	fax.codec = HAL_CODEC_T38;
	CHECK(hal_backend_ops.fax_profile_apply(p.h, 0, 0, &fax) == HAL_UNSUPPORTED);

	CASE("the conference is advertised and both modes forward to the library");
	CHECK((hal_backend_ops.cap_get(p.h)->features & HAL_FEAT_CONF) != 0);
	CHECK(hal_backend_ops.conf_set(p.h, 0, HAL_CONF_LOCAL_MIX) == HAL_OK);
	CHECK(hal_backend_ops.conf_set(p.h, 0, HAL_CONF_OFF) == HAL_OK);

	CASE("a conference on a port the board does not carry is still refused");
	CHECK(hal_backend_ops.conf_set(p.h, HAL_LINE_MAX, HAL_CONF_LOCAL_MIX) ==
	      HAL_UNSUPPORTED);

	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	wire_close(&w);
	probe_down(&p);
}

/*
 * The population gate, which only a board narrower than the hal maximum
 * makes visible. The default fixture populates both ports, so line 1 is
 * inside every bound there. A gate written against the wrong number reads
 * exactly the same. This case therefore brings up a one line board, where
 * cap.lines is 1 and HAL_LINE_MAX is still 2.
 */
static void test_media_population_gate(void)
{
	struct hal_media_stats stats;
	const struct hal_cap *cap;
	struct hal_job job = { 0 };
	struct hal_media m;
	struct probe p;
	struct wire w;

	if (!probe_open_opts(&p, UDSP_SLIC_FAKE_OPT_ALL, 1) || !wire_open(&w)) {
		CASE("the population gate case needs a one line board");
		CHECK(false);
		probe_close(&p);
		return;
	}

	cap = hal_backend_ops.cap_get(p.h);

	CASE("the one line board reports one line, inside the hal maximum of two");
	CHECK_UINT(cap->lines, 1);
	CHECK(cap->lines < HAL_LINE_MAX);

	CASE("and its stream count is inside the hal maximum too");
	CHECK(cap->streams_per_chan < HAL_STREAM_MAX);

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);
	wire_local_release(&w);
	memset(&stats, 0, sizeof(stats));

	/* Line 1 exists in hal.h and not on this board, so a gate written
	 * against HAL_LINE_MAX would let every op below through. */
	CASE("every media op on a port the board does not carry is refused");
	CHECK(hal_backend_ops.stream_config_set(p.h, 1, 0, &m) == HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.stream_start(p.h, 1, 0) == HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.stream_stop(p.h, 1, 0) == HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.stream_dtmf_send(p.h, 1, 0, '1', 100) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.stream_stats_start(p.h, 1, 0, &job) == HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.stream_stats_result_get(p.h, 1, 0, &stats) ==
	      HAL_UNSUPPORTED);

	CASE("a refused slow op on that port leaves the caller's token alone");
	CHECK_UINT(job.token, 0);

	CASE("a stream the channel does not carry is refused the same way");
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, cap->streams_per_chan, &m) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.stream_stats_result_get(p.h, 0, cap->streams_per_chan,
						      &stats) == HAL_UNSUPPORTED);

	CASE("not one of those refusals bound the port it was given");
	CHECK(port_free(w.family, w.local));

	/* The port the board does carry still works, so the gate refuses what
	 * the board lacks and nothing else. */
	CASE("the one port the board does carry still programs");
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);

	wire_close(&w);
	probe_close(&p);
}

static void test_line_test(void)
{
	uint8_t record[512];
	uint8_t short_record[4];
	struct hal_job job = { 0 };
	struct hal_media media;
	struct probe p;
	struct wire wire;
	int32_t status = -1;
	size_t len = 0;

	if (!probe_up(&p)) {
		CASE("the line test case needs a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	CASE("a result read before a test answers unsupported");
	CHECK(hal_backend_ops.line_test_result_get(
		p.h, 0, record, sizeof(record), &len) ==
	      HAL_UNSUPPORTED);
	CHECK_UINT(len, 0);

	CASE("an idle line accepts a gr909 test and assigns its token");
	CHECK(hal_backend_ops.line_test_start(
		p.h, 0, HAL_LINE_TEST_GR909, &job) == HAL_OK);
	CHECK(job.token != 0);

	CASE("the line test completion carries an ok status");
	CHECK(job_wait(&p, job.token, 5000, &status, NULL));
	CHECK_UINT(status, HAL_OK);

	memset(record, 0xa5, sizeof(record));
	CASE("the result getter copies the fake record and its full length");
	CHECK(hal_backend_ops.line_test_result_get(
		p.h, 0, record, sizeof(record), &len) == HAL_OK);
	CHECK_UINT(len, 12);
	CHECK_UINT(record[0], 0);
	CHECK_UINT(record[1], HAL_LINE_TEST_GR909);
	CHECK_UINT(record[2], 0);
	CHECK_UINT(record[8], 1);
	CHECK_UINT(record[9], 0);
	CHECK_UINT(record[10], 0);
	CHECK_UINT(record[11], 0);

	memset(short_record, 0xa5, sizeof(short_record));
	len = 0;
	CASE("a short buffer copies its cap and reports the full length");
	CHECK(hal_backend_ops.line_test_result_get(
		p.h, 0, short_record, sizeof(short_record), &len) == HAL_OK);
	CHECK_UINT(len, 12);
	CHECK(!memcmp(short_record, record, sizeof(short_record)));

	if (!wire_open(&wire)) {
		CASE("the live stream refusal needs a wire");
		CHECK(false);
		probe_down(&p);
		return;
	}
	media_build(&media, &wire, HAL_CODEC_PCMU, 0, 20);
	CASE("a media stream runs before the busy line test request");
	CHECK(media_up(&p, &wire, &media));

	memset(&job, 0, sizeof(job));
	CASE("a line test while a stream runs answers transient");
	CHECK(hal_backend_ops.line_test_start(
		p.h, 0, HAL_LINE_TEST_ROH, &job) == HAL_TRANSIENT);
	CHECK_UINT(job.token, 0);
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	wire_close(&wire);

	udsp_slic_fake_error_set(p.h->s->slic, UDSP_SLIC_FAKE_MLT_RUN, -EIO);
	memset(&job, 0, sizeof(job));
	status = -1;
	CASE("an injected slic failure completes the accepted test transient");
	CHECK(hal_backend_ops.line_test_start(
		p.h, 0, HAL_LINE_TEST_ROH, &job) == HAL_OK);
	CHECK(job_wait(&p, job.token, 5000, &status, NULL));
	CHECK_UINT(status, HAL_TRANSIENT);

	memset(record, 0xa5, sizeof(record));
	len = 0;
	CASE("a failed measurement leaves its head-only result readable");
	CHECK(hal_backend_ops.line_test_result_get(
		p.h, 0, record, sizeof(record), &len) == HAL_OK);
	CHECK_UINT(len, 8);
	CHECK_UINT(record[0], 0);
	CHECK_UINT(record[1], HAL_LINE_TEST_ROH);
	CHECK_UINT(record[2], EIO);

	probe_down(&p);
}

/*
 * The delayed loop, translated. libudsp owns the refusal order, and its own
 * suite checks the audio that the loop carries. This case holds the backend
 * to the translation. It checks the line bound of this file, the three
 * arguments to the engine, and the exclusion that the library applies to the
 * metallic test. The case reads the engine record through the library
 * object that the probe holds, as it reads the channel configuration.
 */
static void test_line_loop(void)
{
	struct hal_job job = { 0 };
	struct hal_loop loop;
	struct probe p;

	if (!probe_up(&p)) {
		CASE("the loop case needs a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	CASE("the backend reports the loop feature and carries the op");
	CHECK((hal_backend_ops.cap_get(p.h)->features & HAL_FEAT_LOOP) != 0);
	CHECK(hal_backend_ops.line_loop_set != NULL);

	CASE("a self arm reaches the engine with the delay in whole ticks");
	memset(&loop, 0, sizeof(loop));
	loop.mode = HAL_LOOP_SELF;
	loop.delay_ms = 200;
	CHECK(hal_backend_ops.line_loop_set(p.h, 0, &loop) == HAL_OK);
	CHECK_UINT(p.h->s->chan[0].loop_mode, UDSP_LOOP_SELF);
	CHECK_UINT(p.h->s->chan[0].loop_delay, 20);

	CASE("a peer arm reaches the engine with the line it plays");
	loop.mode = HAL_LOOP_PEER;
	loop.peer = 1;
	loop.delay_ms = 0;
	CHECK(hal_backend_ops.line_loop_set(p.h, 0, &loop) == HAL_OK);
	CHECK_UINT(p.h->s->chan[0].loop_mode, UDSP_LOOP_PEER);
	CHECK_UINT(p.h->s->chan[0].loop_peer, 1);
	/* a delay of 0 asks for one tick, which is what the library floors it
	 * at */
	CHECK_UINT(p.h->s->chan[0].loop_delay, 1);

	CASE("an armed line refuses a metallic test");
	CHECK(hal_backend_ops.line_test_start(p.h, 0, HAL_LINE_TEST_ROH,
					      &job) == HAL_TRANSIENT);
	CHECK_UINT(job.token, 0);

	CASE("off disarms the line and the test is taken again");
	loop.mode = HAL_LOOP_OFF;
	loop.peer = 7;
	loop.delay_ms = 4000;
	CHECK(hal_backend_ops.line_loop_set(p.h, 0, &loop) == HAL_OK);
	CHECK_UINT(p.h->s->chan[0].loop_mode, UDSP_LOOP_OFF);
	CHECK(hal_backend_ops.line_test_start(p.h, 0, HAL_LINE_TEST_ROH,
					      &job) == HAL_OK);
	CHECK(job_wait(&p, job.token, 5000, NULL, NULL));

	CASE("a line outside the capability and a null record are refused");
	loop.mode = HAL_LOOP_SELF;
	loop.delay_ms = 0;
	CHECK(hal_backend_ops.line_loop_set(p.h, p.h->cap.lines, &loop) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.line_loop_set(p.h, HAL_LINE_MAX, &loop) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.line_loop_set(p.h, 0, NULL) == HAL_UNSUPPORTED);
	/* the engine record of the line is untouched by any of them */
	CHECK_UINT(p.h->s->chan[0].loop_mode, UDSP_LOOP_OFF);

	CASE("the arguments the library refuses travel to it");
	loop.mode = HAL_LOOP_PEER;
	loop.peer = 0;
	CHECK(hal_backend_ops.line_loop_set(p.h, 0, &loop) == HAL_UNSUPPORTED);
	loop.mode = HAL_LOOP_SELF;
	loop.delay_ms = 1001;
	CHECK(hal_backend_ops.line_loop_set(p.h, 0, &loop) == HAL_UNSUPPORTED);
	CHECK_UINT(p.h->s->chan[0].loop_mode, UDSP_LOOP_OFF);

	probe_down(&p);
}

/*
 * The backend half of the stall branch. The fault record reaches the core
 * with no translation in this backend. The case therefore checks one thing: a
 * relay byte for byte of the type, the line and the whole fault mask, stall
 * bit included. The daemon-exit half lives in event_dispatch() of
 * src/event_dispatch.c. The test_dispatch suite compiles that file, and its
 * fault_gate_tests() covers the decision from the stall bit to the exit.
 */
static void test_fault_relay(void)
{
	const struct hal_event *ev;
	struct udsp_event fault;
	struct probe p;

	if (!probe_up(&p)) {
		CASE("the fault relay case needs a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	memset(&fault, 0, sizeof(fault));
	fault.type = UDSP_EV_SLIC_FAULT;
	fault.line = 1;
	fault.seq = 4242;
	fault.u.fault.code = UDSP_FAULT_ENGINE_STALL | UDSP_FAULT_SLIC_VBAT;

	CASE("the library's fault record is accepted by the relay");
	CHECK(send(p.h->lib_fd, &fault, sizeof(fault), MSG_DONTWAIT | MSG_NOSIGNAL) ==
	      (ssize_t)sizeof(fault));

	CASE("it reaches the core as HAL_EVENT_SLIC_FAULT on the same line");
	ev = ev_wait(&p, HAL_EVENT_SLIC_FAULT, 1000);
	CHECK(ev != NULL);

	if (ev) {
		/* The whole mask, not just the bit event_dispatch.c decides on:
		 * the granularity is preserved for diagnostics. */
		CASE("the fault mask arrives whole, stall bit and slic bit together");
		CHECK_UINT(ev->u.fault.code,
			   UDSP_FAULT_ENGINE_STALL | UDSP_FAULT_SLIC_VBAT);
		CHECK_UINT(ev->line, 1);
		CHECK_UINT(ev->seq, 4242);

		/* This is the literal event_dispatch.c spells out, and the
		 * value the asserts of udsp_events.c pin it to. */
		CASE("the stall bit event_dispatch.c decides on is bit 0 of that mask");
		CHECK_UINT(ev->u.fault.code & (1u << 0), 1);
	}

	/*
	 * The fault gate, the half this suite can hold. event_dispatch.c
	 * scopes the whole fault arm to the backend whose fault bits are
	 * pinned, and it asks hal_backend_ops.name for that. The suite links
	 * the udsp backend, so the name it would read is asserted here. Only
	 * the udsp backend defines the bits of that field, so bit 0 must not
	 * be decoded for another backend.
	 */
	CASE("the linked backend names itself, which is what event_dispatch.c gates on");
	CHECK(!strcmp(hal_backend_ops.name, "udsp"));

	/* A fault with no stall bit is the line hand-off half:
	 * event_dispatch.c reports it and passes it to the line shell, which
	 * forces the line out of service. The relay must carry that mask whole
	 * too. */
	CASE("a slic fault with no stall bit reaches the core with its own bits");
	probe_discard(&p);
	memset(&fault, 0, sizeof(fault));
	fault.type = UDSP_EV_SLIC_FAULT;
	fault.line = 0;
	fault.seq = 4243;
	fault.u.fault.code = UDSP_FAULT_SLIC_THERMAL;
	CHECK(send(p.h->lib_fd, &fault, sizeof(fault), MSG_DONTWAIT | MSG_NOSIGNAL) ==
	      (ssize_t)sizeof(fault));
	ev = ev_wait(&p, HAL_EVENT_SLIC_FAULT, 1000);
	CHECK(ev != NULL);
	if (ev) {
		CHECK_UINT(ev->u.fault.code, UDSP_FAULT_SLIC_THERMAL);
		CHECK_UINT(ev->u.fault.code & (1u << 0), 0);
	}

	/* The clear of that fault (event.h, a code of 0). It is the record the
	 * line shell answers with a service derive. The relay must carry it
	 * whole and must not read an empty mask as nothing to forward. */
	CASE("a clear record relays whole, with its line and its seq");
	probe_discard(&p);
	memset(&fault, 0, sizeof(fault));
	fault.type = UDSP_EV_SLIC_FAULT;
	fault.line = 0;
	fault.seq = 4244;
	fault.u.fault.code = 0;
	CHECK(send(p.h->lib_fd, &fault, sizeof(fault),
		   MSG_DONTWAIT | MSG_NOSIGNAL) == (ssize_t)sizeof(fault));
	ev = ev_wait(&p, HAL_EVENT_SLIC_FAULT, 1000);
	CHECK(ev != NULL);
	if (ev) {
		CHECK_UINT(ev->u.fault.code, 0);
		CHECK_UINT(ev->line, 0);
		CHECK_UINT(ev->seq, 4244);
	}

	probe_down(&p);
}

static void test_dead_stream_fault(void)
{
	struct hal_media m;
	struct probe p;
	struct wire w;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the dead-stream case needs a started backend and a wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);
	SUITE_CHECK(media_up(&p, &w, &m));
	p.ev_len = 0;
	atomic_store(&p.h->s->ack_drop_op,
		     (uint_least32_t)STREAM_OP_DEACTIVATE + 1u);

	CASE("a dead-marked stream stop answers fatal");
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_FATAL);
	probe_poll(&p);

	CASE("the fatal stop posts a seq-0 stream-dead fault on its line");
	stream_dead_event_check(&p, 0, 0);

	probe_discard(&p);
	CASE("a second stop on a dead-marked stream is fatal and posts again");
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_FATAL);
	probe_poll(&p);
	stream_dead_event_check(&p, 0, 0);

	wire_close(&w);
	probe_down(&p);

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the successful-stop check needs a started backend and a wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);
	CASE("a normal stream stop succeeds");
	CHECK(media_up(&p, &w, &m));
	p.ev_len = 0;
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	probe_poll(&p);

	CASE("a successful stop posts no fault");
	CHECK_UINT(ev_count(&p, HAL_EVENT_SLIC_FAULT), 0);

	wire_close(&w);
	probe_down(&p);
}

static void test_dead_stream_config_and_start_fault(void)
{
	struct hal_media m;
	struct probe p;
	struct wire w;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the reconfiguration case needs a backend and a wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);
	SUITE_CHECK(media_up(&p, &w, &m));
	probe_discard(&p);
	atomic_store(&p.h->s->ack_drop_op,
		     (uint_least32_t)STREAM_OP_DEACTIVATE + 1u);

	CASE("a dead mark inside a bare reconfiguration posts the fault");
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_FATAL);
	probe_poll(&p);
	stream_dead_event_check(&p, 0, 0);

	probe_discard(&p);
	CASE("a start on a dead-marked stream posts the stream-dead fault");
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_FATAL);
	probe_poll(&p);
	stream_dead_event_check(&p, 0, 0);

	wire_close(&w);
	probe_down(&p);
}

static void test_lost_start_ack(void)
{
	struct hal_media m;
	struct probe p;
	struct wire w;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the lost start ack case needs a backend and a wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);
	wire_local_release(&w);
	SUITE_CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	probe_discard(&p);
	atomic_store(&p.h->s->ack_drop_op,
		     (uint_least32_t)STREAM_OP_ACTIVATE + 1u);

	CASE("a lost start acknowledgement is transient and posts no fault");
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_TRANSIENT);
	probe_poll(&p);
	CHECK_UINT(ev_count(&p, HAL_EVENT_SLIC_FAULT), 0);

	wire_close(&w);
	probe_down(&p);
}

static bool line_dead_inject(struct probe *p, unsigned int line,
			     unsigned int timeout_ms)
{
	struct timespec nap = { .tv_sec = 0, .tv_nsec = 100000L };
	uint64_t stop = mono_us() + (uint64_t)timeout_ms * 1000u;

	while (mono_us() < stop) {
		if (atomic_load_explicit(&p->h->s->chan[line].dead,
					 memory_order_acquire))
			break;
		udsp_slic_fake_error_set(p->h->s->slic,
					 UDSP_SLIC_FAKE_HOOK_READ, -EIO);
		nanosleep(&nap, NULL);
	}
	udsp_slic_fake_error_set(p->h->s->slic, UDSP_SLIC_FAKE_HOOK_READ, 0);

	return atomic_load_explicit(&p->h->s->chan[line].dead,
				    memory_order_acquire);
}

/*
 * A dead line and the monitor getter. hal.h promises HAL_FATAL with the
 * record of the caller untouched, which is the answer that the canceller
 * getter gives. The case fills a sentinel pattern and asserts that it
 * survives the call.
 */
static void test_monitor_dead_line(void)
{
	struct hal_monitor mon;
	struct probe p;

	if (!probe_up(&p)) {
		CASE("the dead line monitor case needs a started backend");
		CHECK(false);
		probe_down(&p);
		return;
	}

	/* Only line 0 samples while the fake returns hook errors, so the other
	 * line stays live, the shape the dead-line stream case uses. */
	atomic_store_explicit(&p.h->s->chan[1].sampling, false,
			      memory_order_release);
	CASE("consecutive hook read failures mark the line dead");
	CHECK(line_dead_inject(&p, 0, 1000));

	CASE("a dead line answers fatal and leaves the record untouched");
	memset(&mon, 0xa5, sizeof(mon));
	CHECK(hal_backend_ops.line_monitor_get(p.h, 0, &mon) == HAL_FATAL);
	CHECK_UINT(mon.valid, 0xa5);
	CHECK_UINT(mon.stale, 0xa5);
	CHECK_UINT(mon.fault_open, 0xa5);
	CHECK_UINT(mon.stamp_ms, 0xa5a5a5a5u);

	CASE("and the live line beside it still answers its own row");
	memset(&mon, 0xa5, sizeof(mon));
	CHECK(hal_backend_ops.line_monitor_get(p.h, 1, &mon) == HAL_OK);
	CHECK_UINT(mon.valid, 0);

	probe_down(&p);
}

static void test_dead_line_stream_fault(void)
{
	struct hal_monitor mon;
	struct hal_media m;
	struct probe p;
	struct wire w;

	if (!probe_up(&p) || !wire_open(&w)) {
		CASE("the dead line case needs a started backend and a wire");
		CHECK(false);
		probe_down(&p);
		return;
	}

	media_build(&m, &w, HAL_CODEC_PCMU, 0, 20);
	SUITE_CHECK(media_up(&p, &w, &m));

	/* Only line 0 samples while the fake returns consecutive hook errors.
	 * This keeps the other line live while the SLIC thread reaches
	 * UDSP_HOOK_FAIL_LIMIT for the line under test. */
	atomic_store_explicit(&p.h->s->chan[1].sampling, false,
			      memory_order_release);
	CASE("consecutive hook read failures mark one line dead");
	CHECK(line_dead_inject(&p, 0, 1000));
	CHECK(!atomic_load_explicit(&p.h->s->chan[1].dead,
				    memory_order_acquire));

	probe_discard(&p);
	/* libudsp admits the stream stop on a dead line as a release-only
	 * op (udsp.h udsp_stream_stop), so the stream a call left on the line
	 * is returned and the line's init retry is not held by it; every other
	 * op keeps the dead line's fatal answer. */
	CASE("a dead line takes the stream stop and posts no stream-dead fault");
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	CHECK(hal_backend_ops.line_monitor_get(p.h, 0, &mon) == HAL_FATAL);
	probe_poll(&p);
	CHECK_UINT(ev_count(&p, HAL_EVENT_SLIC_FAULT), 0);

	wire_close(&w);
	probe_down(&p);
}

static void test_lifecycle(void)
{
	struct hal_channel_cfg ccfg;
	struct probe p;

	if (!probe_open(&p)) {
		CASE("the lifecycle case needs an attached backend");
		CHECK(false);
		probe_close(&p);
		return;
	}

	CASE("a slic op before the bring-up answers transient, never fatal");
	CHECK(hal_backend_ops.line_state_set(p.h, 0, HAL_LINE_ACTIVE) ==
	      HAL_TRANSIENT);

	/* open names no region, so the library holds no tone plan until
	 * the first region_apply job completes. */
	CASE("a tone before the region job answers unsupported");
	CHECK(hal_backend_ops.tone_play(p.h, 0, HAL_TONE_DIAL, 0) ==
	      HAL_UNSUPPORTED);

	CASE("a line outside the capability is refused");
	memset(&ccfg, 0, sizeof(ccfg));
	CHECK(hal_backend_ops.channel_config_set(p.h, HAL_LINE_MAX, &ccfg) !=
	      HAL_OK);

	CASE("close releases the handle and a second open succeeds");
	probe_close(&p);
	CHECK(probe_open(&p));
	probe_close(&p);
}

int main(void)
{
	uloop_init();

	if (!fixture_make()) {
		fprintf(stderr, "test_hal_udsp: no fixture directory\n");
		return 1;
	}

	test_status_map();
	test_cap_fill();
	test_region_name();
	test_region_prefetch();
	test_config_fill();
	test_cid_translation();
	test_ring_translation();
	test_region_view_translation();
	test_fxsnum();

	test_attach();
	test_fax_plugin_missing();
	test_sample_rate_attach();
	test_sample_rate_degrade();
	test_region_absent_attaches();
	test_region_get();
	test_dsp_start_job();
	test_dsp_start_failed();
	test_concurrent_start();
	test_hook_events();
	test_ring();
	test_cid();
	test_mwi();
	test_mwi_without_lamp();
	test_tone_and_digit();
	test_region_job();
	test_region_fallback();
	test_completion_status_map();
	test_completion_queue();
	test_completion_priority();
	test_completion_watch_failure();
	test_rt_surfaces();
	test_counter_surfaces();
	test_bringup_embraces_region();
	test_bringup_region_missing();
	test_bringup_default_region();
	test_bringup_unmapped_region();
	test_bringup_fallback_region();
	test_slic_plugin_path();
	test_config_reaches_hardware();
	test_translated_values();

	/* the media plane */
	test_media_translation();
	test_stats_translation();
	test_dscp_mark();
	test_media_stream();
	test_media_v6_stream();
	test_media_rtcp_addr();
	test_media_dtx();
	test_media_reconfig();
	test_media_local_move();
	test_media_dtmf();
	test_media_stats();
	test_jb_name_tables();
	test_jb_stats_translation();
	test_lec_stats_translation();
	test_jb_stats_get();
	test_lec_stats_get();
	test_monitor_get();
	test_monitor_without_op();
	test_jb_trace();
	test_jb_trace_arm_discard();
	test_slic_log();
	test_jb_trace_close();
	test_pkt_trace_render();
	test_pkt_trace_render_v6();
	test_pkt_trace_generations();
	test_pkt_trace_live();
	test_pkt_trace_arm_discard();
	test_lec_capture_render();
	test_lec_capture_live();
	test_lec_capture_arm_discard();
	test_lec_capture_export();
	test_stream_diag_get();
	test_engine_diag_get();
	test_media_srtp();
	test_t38_stream();
	test_media_refusals();
	test_t38_profile();
	test_media_population_gate();
	test_line_test();
	test_line_loop();
	test_fault_relay();
	test_dead_stream_fault();
	test_dead_stream_config_and_start_fault();
	test_lost_start_ack();
	test_monitor_dead_line();
	test_dead_line_stream_fault();

	test_lifecycle();

	fixture_drop();
	uloop_done();

	return tu_summary("test_hal_udsp");
}
