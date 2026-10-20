// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Unit tests for the raw ALSA TDM backend (tdm/alsa_raw.c). The backend talks
 * to the kernel through seven system calls, so the cases drive it against the
 * model of the PCM core in test/tdm_raw_fake.h. No card and no sound library
 * take part, so the whole backend runs on the build host. The same model
 * serves test/test_core.c, which runs the library end to end on this backend.
 *
 * Each case drives the ops of udsp_tdm_alsa_raw directly, and reads the
 * journal and the counters of the model. The model carries four driver
 * profiles. With them, one case covers the AN7581 and both MediaTek front ends
 * at both line rates.
 *
 * The byte checks build the expected ring content by hand for each layout,
 * not through the address formula of the backend. A wrong formula therefore
 * cannot pass. The checks follow the S16_LE byte order, so they assume a
 * little-endian host. The CI host and both targets are little-endian.
 */
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "udsp/tdm.h"
#include "alsa_raw.h"
#include "tdm_raw_fake.h"
#include "util.h"

/* The refusal record of the last fk_pair_open, zeroed before every open. */
static struct udsp_tdm_fail fk_refusal;

static struct udsp_tdm *fk_pair_open(unsigned rate, unsigned frames,
				     unsigned channels, unsigned prefill,
				     struct udsp_tdm_geom *got)
{
	struct udsp_tdm_geom want = {
		.frames_per_period = frames,
		.channels = channels,
		.prefill_periods = prefill,
		.layout = UDSP_TDM_INTERLEAVED,
		.rate = rate,
	};

	memset(got, 0, sizeof(*got));
	memset(&fk_refusal, 0, sizeof(fk_refusal));

	return udsp_tdm_alsa_raw.open("hw:0,0", "hw:0,1", &want, got,
				      &fk_refusal);
}

static void fk_refusal_check(bool geometry, int err, enum udsp_tdm_dir stream,
			     const char *what)
{
	CHECK(fk_refusal.geometry == geometry);
	CHECK(fk_refusal.err == err);
	CHECK(fk_refusal.stream == stream);
	CHECK(fk_refusal.what && !strcmp(fk_refusal.what, what));
}

static bool fk_prefill_start(struct udsp_tdm *t, unsigned periods)
{
	int16_t silence[FK_CHANNELS_MAX * 1024];
	bool ok = true;

	memset(silence, 0, sizeof(silence));
	for (unsigned i = 0; i < periods; i++)
		ok = udsp_tdm_alsa_raw.write_period(t, silence) == 0 && ok;

	return udsp_tdm_alsa_raw.start(t) == 0 && ok;
}

#define FK_MARKER	((int16_t)0x5a5a)

static void fk_journal_clear(void)
{
	memset(fk_journal, 0, sizeof(fk_journal));
	memset(fk_journal_dir, 0, sizeof(fk_journal_dir));
	fk_journal_len = 0;
}

static void fk_marker_fill(int16_t *buf, unsigned samples)
{
	for (unsigned i = 0; i < samples; i++)
		buf[i] = FK_MARKER;
}

static bool fk_marker_intact(const int16_t *buf, unsigned samples)
{
	for (unsigned i = 0; i < samples; i++) {
		if (buf[i] != FK_MARKER)
			return false;
	}

	return true;
}

/* The expected capture period, built from the model's own ramp. */
static void fk_expect_capture(unsigned long first_frame, unsigned frames,
			      unsigned channels, int16_t *out)
{
	for (unsigned ch = 0; ch < channels; ch++) {
		for (unsigned f = 0; f < frames; f++)
			out[ch * frames + f] = fk_sample(first_frame + f, ch);
	}
}

/* The playback oracle: the ring content at one application pointer, addressed
 * by hand for the layout the profile grants. */
static bool fk_ring_holds(const struct fk_stream *s, unsigned long appl,
			  const int16_t *planar)
{
	unsigned slot = (unsigned)(appl % s->buffer_frames);

	for (unsigned ch = 0; ch < s->channels; ch++) {
		for (unsigned f = 0; f < s->period_frames; f++) {
			unsigned at = (slot + f) % s->buffer_frames;

			if (fk_sample_read(s, ch, at) !=
			    planar[ch * s->period_frames + f])
				return false;
		}
	}

	return true;
}

static void fk_ramp_make(int16_t *buf, unsigned frames, unsigned channels,
			 unsigned tick)
{
	for (unsigned ch = 0; ch < channels; ch++) {
		for (unsigned f = 0; f < frames; f++)
			buf[ch * frames + f] =
				(int16_t)(0x0400 + (int)(tick % 100u) * 3 +
					  (int)ch * 7 + (int)(f % 13u));
	}
}

/* --- the tick loop case -------------------------------------------------- */

struct fk_loop_result {
	bool bytes_ok;
	bool capture_ok;
	bool avail_ok;
	bool delay_ok;
	bool counts_ok;
	unsigned wraps;
};

/* What one tick of the loop must cost and report. */
struct fk_loop_want {
	bool counts;		/* check the per-tick call counts */
	unsigned syncs;
	unsigned polls;
	unsigned sleeps;
	bool avail_zero;	/* else nought up to one period */
	bool delay_exact;	/* else above nought and at most the prefill */
};

static void fk_loop_run(struct udsp_tdm *t, const struct udsp_tdm_geom *geom,
			unsigned ticks, unsigned prefill,
			const struct fk_loop_want *w, struct fk_loop_result *r)
{
	unsigned frames = geom->frames_per_period;
	unsigned channels = geom->channels;
	int16_t rx[FK_CHANNELS_MAX * 1024], tx[FK_CHANNELS_MAX * 1024];
	int16_t want[FK_CHANNELS_MAX * 1024];
	unsigned long read_frames = 0;
	unsigned long last_appl = 0;

	memset(r, 0, sizeof(*r));
	r->bytes_ok = true;
	r->capture_ok = true;
	r->avail_ok = true;
	r->delay_ok = true;
	r->counts_ok = true;

	for (unsigned n = 0; n < ticks; n++) {
		unsigned sync0 = fk_sync_n[0] + fk_sync_n[1];
		unsigned poll0 = fk_poll_n[0] + fk_poll_n[1];
		unsigned sleep0 = fk_sleep_n;
		unsigned long appl;
		long avail, queued;

		fk_advance(1);
		if (udsp_tdm_alsa_raw.read_period(t, rx) != 0) {
			r->capture_ok = false;
			return;
		}
		fk_expect_capture(read_frames, frames, channels, want);
		if (memcmp(rx, want, (size_t)frames * channels * sizeof(*rx)))
			r->capture_ok = false;
		read_frames += frames;

		avail = udsp_tdm_alsa_raw.avail(t);
		if (w->avail_zero ? avail != 0 :
				    avail < 0 || avail >= (long)frames)
			r->avail_ok = false;

		fk_ramp_make(tx, frames, channels, n);
		appl = fk[FK_PLAY].appl_ptr;
		if (udsp_tdm_alsa_raw.write_period(t, tx) != 0) {
			r->bytes_ok = false;
			return;
		}
		if (!fk_ring_holds(&fk[FK_PLAY], appl, tx))
			r->bytes_ok = false;
		if (fk[FK_PLAY].appl_ptr < last_appl)
			r->wraps++;
		last_appl = fk[FK_PLAY].appl_ptr;

		/* A wait that carries an extra interrupt period shares it with
		 * the whole pair, so the playback queue can settle one period
		 * below the prefill. */
		queued = udsp_tdm_alsa_raw.delay(t);
		if (w->delay_exact ? queued != (long)(prefill * frames) :
				     queued <= 0 ||
				     queued > (long)(prefill * frames))
			r->delay_ok = false;
		if (udsp_tdm_alsa_raw.avail(t) < 0)
			r->avail_ok = false;

		if (!w->counts)
			continue;
		if (fk_sync_n[0] + fk_sync_n[1] - sync0 != w->syncs)
			r->counts_ok = false;
		if (fk_poll_n[0] + fk_poll_n[1] - poll0 != w->polls)
			r->counts_ok = false;
		if (fk_sleep_n - sleep0 != w->sleeps)
			r->counts_ok = false;
	}
}

static void fk_loop_check(const struct fk_profile *prof, unsigned rate,
			  unsigned frames)
{
	/* a device that clears BATCH pays the completion read */
	struct fk_loop_want w = {
		.counts = true,
		.syncs = prof->batch ? 3u : 4u,
		.polls = 1,
		.sleeps = 0,
		.avail_zero = true,
		.delay_exact = true,
	};
	struct udsp_tdm_geom got;
	struct fk_loop_result r;
	struct udsp_tdm *t;

	fk_reset(prof);
	t = fk_pair_open(rate, frames, 2, 4, &got);
	CHECK(t != NULL);
	if (!t)
		return;

	CHECK_UINT(got.prefill_periods, 4);
	CHECK(fk_prefill_start(t, got.prefill_periods - 1));
	fk_loop_run(t, &got, 200, got.prefill_periods - 1, &w, &r);
	CHECK(r.capture_ok);
	CHECK(r.bytes_ok);
	CHECK(r.avail_ok);
	CHECK(r.delay_ok);
	CHECK(r.counts_ok);
	CHECK(r.wraps >= 2);
	CHECK_UINT(fk_avail_min_unset, 0);
	CHECK_UINT(fk[FK_CAPTURE].avail_min,
		   prof->batch ? frames : frames - frames / 4);
	CHECK_UINT(fk[FK_PLAY].avail_min, frames);
	/* a wake that carries a whole period needs no sleep, and no wait had
	 * to take a second interrupt */
	CHECK_UINT(fk_sleep_n, 0);
	CHECK_UINT(fk_poll_steps_n, 0);
	udsp_tdm_alsa_raw.close(t);
}

/* The MediaTek front end holds a fraction of a period in its own buffer, so a
 * wake can carry less than the wake threshold and the wait must run on. */
static void fk_lag_check(const struct fk_profile *prof, unsigned rate,
			 unsigned frames)
{
	struct fk_loop_want w = { 0 };
	struct udsp_tdm_geom got;
	struct fk_loop_result r;
	struct udsp_tdm *t;
	unsigned long moved;
	unsigned polls;

	fk_reset(prof);
	t = fk_pair_open(rate, frames, 2, 4, &got);
	CHECK(t != NULL);
	if (!t)
		return;
	CHECK(fk_prefill_start(t, 3));
	fk_lag[FK_CAPTURE] = frames / 2;
	polls = fk_poll_n[FK_CAPTURE];
	moved = fk[FK_CAPTURE].dma_frames;
	fk_loop_run(t, &got, 50, 3, &w, &r);
	CHECK(r.capture_ok);
	CHECK(r.bytes_ok);
	CHECK(r.avail_ok);
	CHECK(r.delay_ok);
	CHECK(fk_poll_n[FK_CAPTURE] - polls >= 50);
	/* one wait carried less than one period and waited again, so the DMA
	 * ran further than the case drove it */
	CHECK(fk[FK_CAPTURE].dma_frames - moved > 50ul * frames);
	/* the published pointer sits inside a period */
	CHECK(fk[FK_CAPTURE].hw_ptr % frames != 0);
	udsp_tdm_alsa_raw.close(t);
}

static void fk_running_check(void)
{
	CHECK_UINT(fk[FK_CAPTURE].state, (unsigned)SNDRV_PCM_STATE_RUNNING);
	CHECK_UINT(fk[FK_PLAY].state, (unsigned)SNDRV_PCM_STATE_RUNNING);
	CHECK(fk[FK_CAPTURE].dma_on);
	CHECK(fk[FK_PLAY].dma_on);
}

static void fk_policy_check(const struct fk_profile *prof)
{
	struct udsp_tdm_geom got;
	struct udsp_tdm *t;
	int16_t tx[160];

	fk_reset(prof);
	CHECK(fk_jit_playback == prof->tx_ack);
	fk_silence_fill = true;
	t = fk_pair_open(8000, 80, 2, 4, &got);
	CHECK(t != NULL);
	if (!t)
		return;
	for (unsigned i = 0; i <= FK_CAPTURE; i++) {
		unsigned long boundary =
			tdm_raw_boundary_get(fk[i].buffer_frames);

		CHECK_UINT(fk[i].stop_threshold, boundary);
		CHECK_UINT(fk[i].silence_threshold, 0);
		CHECK_UINT(fk[i].silence_size,
			   i == FK_PLAY && !prof->batch ? boundary : 0);
	}
	fk_marker_fill(tx, 160);
	CHECK(udsp_tdm_alsa_raw.write_period(t, tx) == 0);
	CHECK(udsp_tdm_alsa_raw.start(t) == 0);
	fk_advance(1);
	if (!prof->batch)
		memset(tx, 0, sizeof(tx));
	CHECK(fk_ring_holds(&fk[FK_PLAY], 0, tx));
	fk_running_check();
	udsp_tdm_alsa_raw.close(t);
}

static void fk_capture_depth_check(const struct fk_profile *prof,
				   unsigned depth, bool to_floor)
{
	struct udsp_tdm_geom got;
	struct udsp_tdm *t;

	fk_reset(prof);
	if (to_floor)
		fk_periods_floor_refuse = 5;
	else
		fk_reject_periods = depth;
	t = fk_pair_open(8000, 80, 2, 4, &got);
	CHECK(t != NULL);
	if (!t)
		return;
	CHECK_UINT(got.prefill_periods, 4);
	CHECK_UINT(got.capture_periods, to_floor ? 4 : depth - 1);
	CHECK_UINT(fk_hw_params_n[FK_PLAY], 1);
	CHECK_UINT(fk_hw_params_n[FK_CAPTURE], to_floor ? depth - 3 : 2);
	udsp_tdm_alsa_raw.close(t);
}

static void fk_capture_large_check(const struct fk_profile *prof,
				   unsigned depth)
{
	struct udsp_tdm_geom got;
	struct udsp_tdm *t;

	fk_reset(prof);
	t = fk_pair_open(8000, 80, 2, 17, &got);
	CHECK(t != NULL);
	if (!t)
		return;
	CHECK_UINT(got.prefill_periods, depth);
	CHECK_UINT(got.capture_periods, depth);
	CHECK_UINT(fk_hw_params_n[FK_PLAY], 1);
	CHECK_UINT(fk_hw_params_n[FK_CAPTURE], 1);
	udsp_tdm_alsa_raw.close(t);
}

static void fk_capture_small_check(void)
{
	const struct fk_loop_want want = {
		.counts = true,
		.syncs = 3,
		.polls = 1,
		.sleeps = 0,
		.avail_zero = true,
		.delay_exact = true,
	};
	struct udsp_tdm_geom got;
	struct fk_loop_result result;
	struct udsp_tdm *t;

	fk_reset(&fk_an7581);
	fk_periods_floor_refuse = 3;
	t = fk_pair_open(8000, 80, 2, 2, &got);
	CHECK(t != NULL);
	if (!t)
		return;
	CHECK_UINT(got.capture_periods, 2);
	CHECK_UINT(got.prefill_periods, 2);
	CHECK(fk_prefill_start(t, 1));
	fk_loop_run(t, &got, 200, 1, &want, &result);
	CHECK(result.capture_ok);
	CHECK(result.bytes_ok);
	CHECK(result.avail_ok);
	CHECK(result.delay_ok);
	CHECK(result.counts_ok);
	fk_running_check();
	udsp_tdm_alsa_raw.close(t);
}

static void fk_capture_memory_check(void)
{
	struct udsp_tdm_geom got;
	struct udsp_tdm *t;

	fk_reset(&fk_an7581);
	fk_reject_periods = 15;
	fk_reject_errno = ENOMEM;
	t = fk_pair_open(8000, 80, 2, 4, &got);
	CHECK(t != NULL);
	if (t) {
		CHECK_UINT(got.capture_periods, 14);
		CHECK_UINT(got.prefill_periods, 4);
		CHECK_UINT(fk_hw_params_n[FK_CAPTURE], 2);
		udsp_tdm_alsa_raw.close(t);
	}
	fk_reset(&fk_an7581);
	fk_periods_floor_refuse = 5;
	fk_reject_errno = ENOMEM;
	t = fk_pair_open(8000, 80, 2, 4, &got);
	CHECK(t != NULL);
	if (t) {
		CHECK_UINT(got.capture_periods, 4);
		CHECK_UINT(fk_hw_params_n[FK_CAPTURE], 12);
		udsp_tdm_alsa_raw.close(t);
	}
	fk_reset(&fk_an7581);
	fk_reject_periods = 4;
	fk_reject_errno = ENOMEM;
	CHECK(fk_pair_open(8000, 80, 2, 4, &got) == NULL);
	CHECK_UINT(fk_hw_params_n[FK_PLAY], 1);
	fk_refusal_check(false, -ENOMEM, UDSP_TDM_DIR_PLAYBACK, "hw_params");
}

static void fk_fresh_full_check(void)
{
	struct udsp_tdm_geom got;
	struct udsp_tdm *t;
	int16_t tx[160] = {0};
	unsigned syncs, polls;

	fk_reset(&fk_an7581);
	t = fk_pair_open(8000, 80, 2, 4, &got);
	CHECK(t != NULL);
	if (!t)
		return;
	CHECK(fk_prefill_start(t, 4));
	CHECK(udsp_tdm_alsa_raw.playback_room_fresh(t) == 0);
	syncs = fk_sync_n[FK_PLAY];
	polls = fk_poll_n[FK_PLAY];
	CHECK(udsp_tdm_alsa_raw.write_period(t, tx) == 0);
	CHECK_UINT(fk_sync_n[FK_PLAY] - syncs, 3);
	CHECK_UINT(fk_poll_n[FK_PLAY] - polls, 1);
	CHECK_UINT(fk[FK_PLAY].appl_ptr, 400);
	CHECK(udsp_tdm_alsa_raw.playback_room_fresh(t) == 0);
	fk_poll_timeouts[FK_PLAY] = 1;
	syncs = fk_sync_n[FK_PLAY];
	polls = fk_poll_n[FK_PLAY];
	CHECK(udsp_tdm_alsa_raw.write_period(t, tx) == -EAGAIN);
	CHECK_UINT(fk_sync_n[FK_PLAY] - syncs, 2);
	CHECK_UINT(fk_poll_n[FK_PLAY] - polls, 1);
	CHECK_UINT(fk[FK_PLAY].appl_ptr, 400);
	udsp_tdm_alsa_raw.close(t);
}

static void fk_silence_params_check(void)
{
	const struct {
		unsigned long size;
		unsigned long threshold;
		bool valid;
	} cases[] = {
		{ 0, 0, true },
		{ 80, 80, true },
		{ 320, 320, true },
		{ 640, 0, true },
		{ 641, 0, true },
		{ 80, 40, false },
		{ 0, 321, false },
		{ 639, 0, false },
		{ 640, 1, false },
		{ 641, 1, false },
	};
	struct udsp_tdm_geom got;
	struct udsp_tdm *t;

	fk_reset(&fk_mt7986);
	t = fk_pair_open(8000, 80, 2, 4, &got);
	CHECK(t != NULL);
	if (!t)
		return;
	CHECK_UINT(fk[FK_PLAY].buffer_frames, 320);
	CHECK_UINT(fk[FK_PLAY].boundary, 640);
	for (unsigned n = 0; n < sizeof(cases) / sizeof(cases[0]); n++) {
		struct snd_pcm_sw_params sw = {
			.avail_min = 80,
			.silence_size = cases[n].size,
			.silence_threshold = cases[n].threshold,
		};
		int rc = fk_sw_params(&fk[FK_PLAY], &sw);

		CHECK(rc == (cases[n].valid ? 0 : -1));
		if (cases[n].valid) {
			CHECK_UINT(fk[FK_PLAY].silence_size, cases[n].size);
			continue;
		}
		CHECK(errno == EINVAL);
	}
	udsp_tdm_alsa_raw.close(t);
}

static void fk_hold_capture_check(unsigned frames, unsigned hold)
{
	struct udsp_tdm_geom got;
	struct udsp_tdm *t;
	int16_t rx[320], want[320];
	unsigned dropped = hold > 13 ? hold - 13 : 0;
	unsigned reads = hold - dropped;
	unsigned polls, steps, syncs;

	fk_reset(&fk_an7581);
	t = fk_pair_open(frames * 100, frames, 2, 4, &got);
	CHECK(t != NULL);
	if (!t)
		return;
	CHECK(fk_prefill_start(t, 3));
	polls = fk_poll_n[FK_CAPTURE];
	steps = fk_poll_steps_n;
	syncs = fk_sync_n[FK_CAPTURE];
	fk_hold_periods[FK_CAPTURE] = hold;
	for (unsigned n = 0; n < reads; n++) {
		CHECK(udsp_tdm_alsa_raw.read_period(t, rx) ==
		      (int)(n ? 0 : dropped));
		fk_expect_capture((unsigned long)(dropped + n) * frames,
				  frames, 2, want);
		CHECK(!memcmp(rx, want, frames * 2 * sizeof(*rx)));
		CHECK_UINT(fk[FK_CAPTURE].appl_ptr % frames, 0);
		CHECK_UINT(fk[FK_CAPTURE].appl_ptr, (dropped + n + 1) * frames);
		CHECK_UINT(fk[FK_PLAY].dma_frames, 3 * frames);
		CHECK_UINT(fk[FK_PLAY].hw_ptr, 3 * frames);
		CHECK_UINT(fk[FK_PLAY].armed, 0);
		fk_running_check();
	}
	CHECK_UINT(fk[FK_CAPTURE].dma_frames, hold * frames);
	CHECK_UINT(fk_poll_n[FK_CAPTURE] - polls, reads);
	CHECK_UINT(fk_poll_steps_n - steps, 0);
	CHECK_UINT(fk_sync_n[FK_CAPTURE] - syncs, reads + (dropped != 0));
	CHECK_UINT(fk_hold_periods[FK_CAPTURE], 0);
	CHECK(udsp_tdm_alsa_raw.playback_room_fresh(t) == 4L * frames);
	CHECK(udsp_tdm_alsa_raw.delay(t) == 0);
	CHECK(udsp_tdm_alsa_raw.write_period(t, rx) == 0);
	CHECK_UINT(fk[FK_PLAY].armed, 1);
	fk_advance(1);
	CHECK_UINT(fk[FK_PLAY].dma_frames, 4 * frames);
	fk_running_check();
	udsp_tdm_alsa_raw.close(t);
}

static void fk_blackout_check(const struct fk_profile *prof)
{
	struct udsp_tdm_geom got;
	struct udsp_tdm *t;
	int16_t rx[160], want[160];
	unsigned syncs;

	fk_reset(prof);
	t = fk_pair_open(8000, 80, 2, 4, &got);
	CHECK(t != NULL);
	if (!t)
		return;
	CHECK(fk_prefill_start(t, 3));
	fk_advance(2);
	fk_overrun(15);
	CHECK_UINT(fk[FK_CAPTURE].hw_ptr, 160);
	CHECK_UINT(fk[FK_CAPTURE].dma_frames, 17 * 80);
	fk_running_check();
	syncs = fk_sync_n[FK_CAPTURE];
	CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == 3);
	fk_expect_capture(3 * 80, 80, 2, want);
	CHECK(!memcmp(rx, want, sizeof(rx)));
	CHECK_UINT(fk[FK_CAPTURE].appl_ptr, 4 * 80);
	CHECK_UINT(fk_sync_n[FK_CAPTURE] - syncs, prof->batch ? 2 : 3);
	fk_running_check();
	udsp_tdm_alsa_raw.close(t);
}

static void fk_position_set(struct fk_stream *s, unsigned long hw,
			    unsigned long appl)
{
	s->dma_frames = hw;
	s->hw_ptr = hw % s->boundary;
	s->hw_base = s->hw_ptr - s->hw_ptr % s->buffer_frames;
	s->appl_ptr = appl % s->boundary;
	s->tx_fill = s->appl_ptr;
}

static void fk_capture_wrap_check(void)
{
	struct udsp_tdm_geom got;
	struct udsp_tdm *t;
	struct fk_stream *s = &fk[FK_CAPTURE];
	int16_t rx[160], want[160];

	fk_reset(&fk_an7581);
	t = fk_pair_open(8000, 80, 2, 4, &got);
	CHECK(t != NULL);
	if (!t)
		return;
	CHECK(fk_prefill_start(t, 3));
	fk_position_set(s, s->boundary - 80, s->boundary - 160);
	CHECK(udsp_tdm_alsa_raw.capture_avail_fresh(t) == 80);
	fk_hold_periods[FK_CAPTURE] = 16;
	CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == 4);
	fk_expect_capture(s->boundary + 160, 80, 2, want);
	CHECK(!memcmp(rx, want, sizeof(rx)));
	CHECK_UINT(s->appl_ptr, 240);
	CHECK_UINT(s->appl_ptr % 80, 0);
	fk_running_check();
	udsp_tdm_alsa_raw.close(t);
}

static void fk_fractional_capture_check(void)
{
	struct udsp_tdm_geom got;
	struct udsp_tdm *t;
	struct fk_stream *s = &fk[FK_CAPTURE];
	int16_t rx[160], want[160];

	fk_reset(&fk_mt7986);
	t = fk_pair_open(8000, 80, 2, 4, &got);
	CHECK(t != NULL);
	if (!t)
		return;
	CHECK(fk_prefill_start(t, 3));
	fk_advance(2);
	fk_dma_step_quiet(s, 15 * 80 + 10);
	CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == 4);
	CHECK_UINT(s->hw_ptr, 17 * 80 + 10);
	CHECK_UINT(s->appl_ptr, 5 * 80);
	fk_expect_capture(4 * 80, 80, 2, want);
	CHECK(!memcmp(rx, want, sizeof(rx)));
	fk_running_check();
	udsp_tdm_alsa_raw.close(t);
}

static void fk_fractional_hold_check(void)
{
	struct udsp_tdm_geom got;
	struct udsp_tdm *t;
	int16_t rx[160], tx[160], silence[160] = {0};
	unsigned syncs;

	fk_reset(&fk_mt7986);
	fk_silence_fill = true;
	fk_lag[FK_PLAY] = 20;
	t = fk_pair_open(8000, 80, 2, 4, &got);
	CHECK(t != NULL);
	if (!t)
		return;
	fk_marker_fill(tx, 160);
	for (unsigned n = 0; n < 3; n++)
		CHECK(udsp_tdm_alsa_raw.write_period(t, tx) == 0);
	CHECK(udsp_tdm_alsa_raw.start(t) == 0);
	fk_hold_periods[FK_CAPTURE] = 6;
	CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == 0);
	CHECK_UINT(fk[FK_PLAY].hw_ptr, 6 * 80 - 20);
	CHECK_UINT(fk[FK_PLAY].hw_ptr % 80, 60);
	for (unsigned n = 0; n < 4; n++)
		CHECK(fk_ring_holds(&fk[FK_PLAY], n * 80, silence));
	CHECK(udsp_tdm_alsa_raw.playback_room_fresh(t) == 7 * 80 - 20);
	CHECK(udsp_tdm_alsa_raw.delay(t) == 0);
	syncs = fk_sync_n[FK_PLAY];
	CHECK(udsp_tdm_alsa_raw.write_period(t, tx) == 3);
	CHECK_UINT(fk_sync_n[FK_PLAY] - syncs, 2);
	CHECK_UINT(fk[FK_PLAY].appl_ptr, 7 * 80);
	CHECK_UINT(fk[FK_PLAY].appl_ptr % 80, 0);
	CHECK(fk_ring_holds(&fk[FK_PLAY], 6 * 80, tx));
	CHECK(udsp_tdm_alsa_raw.write_period(t, tx) == 0);
	CHECK_UINT(fk[FK_PLAY].appl_ptr, 0);
	CHECK(fk_ring_holds(&fk[FK_PLAY], 7 * 80, tx));
	CHECK(fk_ring_holds(&fk[FK_PLAY], 0, silence));
	fk_running_check();
	udsp_tdm_alsa_raw.close(t);
}

static void fk_dry_playback_check(void)
{
	struct udsp_tdm_geom got;
	struct udsp_tdm *t;
	struct fk_stream *s = &fk[FK_PLAY];
	int16_t tx[160];
	unsigned syncs;

	fk_reset(&fk_mt7986);
	t = fk_pair_open(8000, 80, 2, 4, &got);
	CHECK(t != NULL);
	if (!t)
		return;
	CHECK(fk_prefill_start(t, 1));
	fk_marker_fill(tx, 160);
	fk_dma_step(s, 160);
	CHECK(udsp_tdm_alsa_raw.write_period(t, tx) == 0);
	CHECK(udsp_tdm_alsa_raw.delay(t) == 0);
	fk_dma_step_quiet(s, 20);
	syncs = fk_sync_n[FK_PLAY];
	CHECK(udsp_tdm_alsa_raw.write_period(t, tx) == 1);
	CHECK_UINT(fk_sync_n[FK_PLAY] - syncs, 2);
	CHECK_UINT(s->hw_ptr, 180);
	CHECK_UINT(s->appl_ptr, 320);
	CHECK(fk_ring_holds(s, 240, tx));
	syncs = fk_sync_n[FK_PLAY];
	CHECK(udsp_tdm_alsa_raw.write_period(t, tx) == 0);
	CHECK_UINT(fk_sync_n[FK_PLAY] - syncs, 1);
	fk_running_check();
	udsp_tdm_alsa_raw.close(t);
}

static void fk_fresh_capture_check(void)
{
	struct udsp_tdm_geom got;
	struct udsp_tdm *t;
	struct fk_stream *s = &fk[FK_CAPTURE];
	int16_t rx[160], want[160];
	unsigned syncs;

	fk_reset(&fk_mt7986);
	t = fk_pair_open(8000, 80, 2, 4, &got);
	CHECK(t != NULL);
	if (!t)
		return;
	CHECK(fk_prefill_start(t, 3));
	syncs = fk_sync_n[FK_CAPTURE];
	CHECK(udsp_tdm_alsa_raw.capture_avail_fresh(t) == 0);
	CHECK_UINT(fk_sync_n[FK_CAPTURE] - syncs, 1);
	fk_dma_step_quiet(s, 160);
	CHECK(udsp_tdm_alsa_raw.avail(t) == 0);
	syncs = fk_sync_n[FK_CAPTURE];
	CHECK(udsp_tdm_alsa_raw.capture_avail_fresh(t) == 160);
	CHECK_UINT(fk_sync_n[FK_CAPTURE] - syncs, 1);
	CHECK(udsp_tdm_alsa_raw.capture_skip(t, 1) == 0);
	CHECK_UINT(fk_sync_n[FK_CAPTURE] - syncs, 2);
	CHECK_UINT(s->appl_ptr, 80);
	CHECK(udsp_tdm_alsa_raw.avail(t) == 80);
	CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == 0);
	fk_expect_capture(80, 80, 2, want);
	CHECK(!memcmp(rx, want, sizeof(rx)));

	fk_position_set(s, s->boundary - 80, s->boundary - 160);
	CHECK(udsp_tdm_alsa_raw.capture_avail_fresh(t) == 80);
	fk_dma_step(s, 160);
	CHECK_UINT(s->hw_ptr, 80);
	syncs = fk_sync_n[FK_CAPTURE];
	CHECK(udsp_tdm_alsa_raw.capture_avail_fresh(t) == 240);
	CHECK_UINT(fk_sync_n[FK_CAPTURE] - syncs, 1);
	CHECK(udsp_tdm_alsa_raw.capture_skip(t, 2) == 0);
	CHECK_UINT(s->appl_ptr, 0);
	CHECK_UINT(fk_sync_n[FK_CAPTURE] - syncs, 2);
	CHECK(udsp_tdm_alsa_raw.capture_skip(t, 0) == 0);
	CHECK_UINT(s->appl_ptr, 0);
	CHECK_UINT(fk_sync_n[FK_CAPTURE] - syncs, 3);
	CHECK(udsp_tdm_alsa_raw.avail(t) == 80);
	CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == 0);
	fk_expect_capture(s->boundary, 80, 2, want);
	CHECK(!memcmp(rx, want, sizeof(rx)));
	CHECK_UINT(s->appl_ptr, 80);
	fk_state_force(SNDRV_PCM_STATE_XRUN, false);
	syncs = fk_sync_n[FK_CAPTURE];
	CHECK(udsp_tdm_alsa_raw.capture_avail_fresh(t) == -EPIPE);
	CHECK(udsp_tdm_alsa_raw.capture_skip(t, 1) == -EPIPE);
	CHECK_UINT(fk_sync_n[FK_CAPTURE] - syncs, 2);
	CHECK_UINT(s->appl_ptr, 80);
	udsp_tdm_alsa_raw.close(t);
}

static void fk_fresh_playback_check(void)
{
	struct udsp_tdm_geom got;
	struct udsp_tdm *t;
	struct fk_stream *s = &fk[FK_PLAY];
	int16_t tx[160], silence[160] = {0};
	unsigned syncs;

	fk_reset(&fk_mt7986);
	t = fk_pair_open(8000, 80, 2, 4, &got);
	CHECK(t != NULL);
	if (!t)
		return;
	CHECK(fk_prefill_start(t, 3));
	fk_marker_fill(tx, 160);
	fk_position_set(s, s->boundary - 80, 0);
	syncs = fk_sync_n[FK_PLAY];
	CHECK(udsp_tdm_alsa_raw.playback_room_fresh(t) == 240);
	CHECK_UINT(fk_sync_n[FK_PLAY] - syncs, 1);
	fk_dma_step_quiet(s, 120);
	CHECK(udsp_tdm_alsa_raw.write_period(t, tx) == 1);
	CHECK_UINT(fk_sync_n[FK_PLAY] - syncs, 3);
	CHECK_UINT(s->hw_ptr, 40);
	CHECK_UINT(s->appl_ptr, 160);
	CHECK(fk_ring_holds(s, 80, tx));
	CHECK(fk_ring_holds(s, 0, silence));

	fk_position_set(s, s->boundary - 240, s->boundary - 160);
	CHECK(udsp_tdm_alsa_raw.playback_room_fresh(t) == 240);
	fk_dma_step_quiet(s, 120);
	CHECK(udsp_tdm_alsa_raw.write_period(t, tx) == 1);
	CHECK_UINT(s->appl_ptr, 0);
	CHECK(fk_ring_holds(s, s->boundary - 80, tx));
	CHECK(fk_ring_holds(s, 0, silence));
	CHECK(udsp_tdm_alsa_raw.write_period(t, tx) == 0);
	CHECK_UINT(s->appl_ptr, 80);
	fk_position_set(s, s->boundary - 40, s->boundary - 160);
	CHECK(udsp_tdm_alsa_raw.playback_room_fresh(t) == 440);
	CHECK(udsp_tdm_alsa_raw.write_period(t, tx) == 2);
	CHECK_UINT(s->appl_ptr, 80);
	CHECK(fk_ring_holds(s, 0, tx));
	fk_running_check();
	fk_state_force(SNDRV_PCM_STATE_XRUN, true);
	syncs = fk_sync_n[FK_PLAY];
	CHECK(udsp_tdm_alsa_raw.playback_room_fresh(t) == -EPIPE);
	CHECK_UINT(fk_sync_n[FK_PLAY] - syncs, 1);
	udsp_tdm_alsa_raw.close(t);
}

int main(void)
{
	CASE("raw device string parse accepts hw, plughw and a pcm node");
	{
		char path[64];

		CHECK(tdm_raw_path_build("hw:0,1", true, path, sizeof(path)));
		CHECK(!strcmp(path, "/dev/snd/pcmC0D1c"));
		CHECK(tdm_raw_path_build("plughw:0,0", false, path,
					 sizeof(path)));
		CHECK(!strcmp(path, "/dev/snd/pcmC0D0p"));
		CHECK(tdm_raw_path_build("hw:2,3", false, path, sizeof(path)));
		CHECK(!strcmp(path, "/dev/snd/pcmC2D3p"));
		CHECK(tdm_raw_path_build("/dev/snd/pcmC0D1c", true, path,
					 sizeof(path)));
		CHECK(!strcmp(path, "/dev/snd/pcmC0D1c"));

		CHECK(!tdm_raw_path_build("default", true, path, sizeof(path)));
		CHECK(!tdm_raw_path_build("hw:0", true, path, sizeof(path)));
		CHECK(!tdm_raw_path_build("plug:hw:0,0", true, path,
					  sizeof(path)));
		CHECK(!tdm_raw_path_build("hw:0,0,1", true, path,
					  sizeof(path)));
		CHECK(!tdm_raw_path_build("hw:,0", true, path, sizeof(path)));
		CHECK(!tdm_raw_path_build("hw:-1,0", true, path, sizeof(path)));
		/* the wrong direction on an explicit node */
		CHECK(!tdm_raw_path_build("/dev/snd/pcmC0D1c", false, path,
					  sizeof(path)));
		CHECK(!tdm_raw_path_build("/dev/snd/pcmC0D0p", true, path,
					  sizeof(path)));
		CHECK(!tdm_raw_path_build("/dev/snd/pcmC0D0", false, path,
					  sizeof(path)));
		CHECK(!tdm_raw_path_build(NULL, false, path, sizeof(path)));
		CHECK(!tdm_raw_path_build("hw:0,0", false, path, 8));
	}

	CASE("raw hw_params builder pins the request and opens the rest");
	{
		const unsigned mmap_i =
			(unsigned)SNDRV_PCM_ACCESS_MMAP_INTERLEAVED;
		const unsigned mmap_n =
			(unsigned)SNDRV_PCM_ACCESS_MMAP_NONINTERLEAVED;
		const unsigned rw_i =
			(unsigned)SNDRV_PCM_ACCESS_RW_INTERLEAVED;
		struct snd_pcm_hw_params p;

		tdm_raw_params_build(&p, 2, 16000, 160, 0);
		CHECK_UINT(p.rmask, ~0u);
		CHECK(fkp_mask_has(&p, SNDRV_PCM_HW_PARAM_ACCESS, mmap_i));
		CHECK(fkp_mask_has(&p, SNDRV_PCM_HW_PARAM_ACCESS, mmap_n));
		CHECK(!fkp_mask_has(&p, SNDRV_PCM_HW_PARAM_ACCESS, rw_i));
		CHECK(fkp_mask_has(&p, SNDRV_PCM_HW_PARAM_FORMAT,
				   (unsigned)SNDRV_PCM_FORMAT_S16_LE));
		CHECK(!fkp_mask_has(&p, SNDRV_PCM_HW_PARAM_FORMAT,
				    (unsigned)SNDRV_PCM_FORMAT_S32_LE));
		CHECK(fkp_mask_has(&p, SNDRV_PCM_HW_PARAM_SUBFORMAT,
				   (unsigned)SNDRV_PCM_SUBFORMAT_STD));
		{
			unsigned lo, hi;

			fkp_range_get(&p, SNDRV_PCM_HW_PARAM_CHANNELS, &lo,
				      &hi);
			CHECK(lo == 2 && hi == 2);
			fkp_range_get(&p, SNDRV_PCM_HW_PARAM_RATE, &lo, &hi);
			CHECK(lo == 16000 && hi == 16000);
			fkp_range_get(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, &lo,
				      &hi);
			CHECK(lo == 160 && hi == 160);
			CHECK(FKP_INT(&p,
				      SNDRV_PCM_HW_PARAM_PERIOD_SIZE).integer);
			fkp_range_get(&p, SNDRV_PCM_HW_PARAM_PERIODS, &lo, &hi);
			CHECK(lo == 0 && hi == UINT_MAX);
			fkp_range_get(&p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, &lo,
				      &hi);
			CHECK(lo == 0 && hi == UINT_MAX);

			tdm_raw_params_build(&p, 1, 8000, 80, 4);
			fkp_range_get(&p, SNDRV_PCM_HW_PARAM_PERIODS, &lo, &hi);
			CHECK(lo == 4 && hi == 4);
		}
	}

	CASE("raw ring pointer helpers fold at the boundary");
	{
		const unsigned long buffer = 640, boundary = 1280;

		CHECK_UINT(tdm_raw_capture_avail_get(200, 40, boundary), 160);
		/* only the hardware pointer wrapped */
		CHECK_UINT(tdm_raw_capture_avail_get(40, 1240, boundary), 80);
		CHECK_UINT(tdm_raw_capture_avail_get(0, 0, boundary), 0);

		CHECK_UINT(tdm_raw_playback_avail_get(0, 0, buffer, boundary),
			   buffer);
		CHECK_UINT(tdm_raw_playback_avail_get(160, 480, buffer,
						      boundary), 320);
		/* only the application pointer wrapped */
		CHECK_UINT(tdm_raw_playback_avail_get(1200, 80, buffer,
						      boundary), 480);
		CHECK_UINT(tdm_raw_playback_queued_get(1200, 80, buffer,
						       boundary), 160);
		CHECK_UINT(tdm_raw_playback_queued_get(160, 480, buffer,
						       boundary), 320);
		CHECK(tdm_raw_playback_queued_get(0, 0, buffer, boundary) == 0);
		/* the hardware ran past the writer: an underrun reads back
		 * negative and the delay op clamps it */
		CHECK(tdm_raw_playback_queued_get(700, 100, buffer, boundary) ==
		      -600);

		CHECK_UINT(tdm_raw_boundary_get(640),
			   (unsigned long)640 << 53);
		CHECK(tdm_raw_boundary_get(640) * 2 >
		      (unsigned long)LONG_MAX - 640);
		CHECK_UINT(tdm_raw_boundary_get(0), 0);

		CHECK_UINT(tdm_raw_sample_offset_get(0, 4, 10), 40);
		CHECK_UINT(tdm_raw_sample_offset_get(2, 4, 10), 42);
		CHECK_UINT(tdm_raw_sample_offset_get(1280, 2, 3), 1286);
	}

	CASE("raw open grants a geometry on each driver profile");
	{
		struct udsp_tdm_geom got;
		struct udsp_tdm *t;

		fk_reset(&fk_an7581);
		t = fk_pair_open(16000, 160, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			CHECK_UINT(got.rate, 16000);
			CHECK_UINT(got.frames_per_period, 160);
			CHECK_UINT(got.channels, 2);
			CHECK_UINT(got.prefill_periods, 4);
			CHECK_UINT(got.layout, UDSP_TDM_NONINTERLEAVED);
			CHECK(got.paced);
			CHECK_UINT(fk[FK_PLAY].periods, 4);
			CHECK_UINT(fk[FK_CAPTURE].periods, 15);
			CHECK_UINT(got.capture_periods, 15);
			udsp_tdm_alsa_raw.close(t);
		}

		fk_reset(&fk_mt7986);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			CHECK_UINT(got.layout, UDSP_TDM_INTERLEAVED);
			CHECK_UINT(got.prefill_periods, 4);
			udsp_tdm_alsa_raw.close(t);
		}

		fk_reset(&fk_mt2701);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			CHECK_UINT(got.layout, UDSP_TDM_INTERLEAVED);
			udsp_tdm_alsa_raw.close(t);
		}

		/* Playback depth must not reduce the capture reserve. */
		fk_reset(&fk_mt7986);
		t = fk_pair_open(8000, 80, 2, 2, &got);
		CHECK(t != NULL);
		if (t) {
			CHECK_UINT(fk[FK_PLAY].periods, 2);
			CHECK_UINT(fk[FK_CAPTURE].periods, 16);
			CHECK_UINT(got.capture_periods, 16);
			CHECK_UINT(got.prefill_periods, 2);
			udsp_tdm_alsa_raw.close(t);
		}

		/* the AN7581 descriptor ring caps the request */
		fk_reset(&fk_an7581);
		t = fk_pair_open(8000, 80, 2, 16, &got);
		CHECK(t != NULL);
		if (t) {
			CHECK_UINT(got.prefill_periods, 15);
			udsp_tdm_alsa_raw.close(t);
		}

		fk_reset(&fk_an7581);
		CHECK(fk_pair_open(12000, 120, 2, 4, &got) == NULL);
		fk_reset(&fk_an7581);
		CHECK(fk_pair_open(16000, 81, 2, 4, &got) == NULL);
		fk_reset(&fk_an7581);
		CHECK(fk_pair_open(8000, 80, 3, 4, &got) == NULL);
		fk_reset(&fk_an7581);
		CHECK(fk_pair_open(8000, 0, 2, 4, &got) == NULL);

		/* the shared AN7581 registers refuse a second geometry */
		fk_reset(&fk_an7581);
		fk_peer_busy = true;
		CHECK(fk_pair_open(8000, 80, 2, 4, &got) == NULL);
		CHECK_UINT(fk_hw_params_n[FK_CAPTURE], 1);
	}

	CASE("raw open follows the protocol version the card reports");
	{
		struct udsp_tdm_geom got;
		struct udsp_tdm *t;

		/* 2.0.14 brought USER_PVERSION, so an older 2.0 kernel must
		 * still open. */
		fk_reset(&fk_an7581);
		fk_pversion = SNDRV_PROTOCOL_VERSION(2, 0, 13);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			CHECK_UINT(got.frames_per_period, 80);
			CHECK(fk_prefill_start(t, 3));
			udsp_tdm_alsa_raw.close(t);
		}

		fk_reset(&fk_an7581);
		fk_pversion = SNDRV_PROTOCOL_VERSION(2, 0, 18);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t)
			udsp_tdm_alsa_raw.close(t);

		/* Another major or minor changes the structure layout. */
		fk_reset(&fk_an7581);
		fk_pversion = SNDRV_PROTOCOL_VERSION(2, 1, 0);
		CHECK(fk_pair_open(8000, 80, 2, 4, &got) == NULL);
		fk_reset(&fk_an7581);
		fk_pversion = SNDRV_PROTOCOL_VERSION(3, 0, 0);
		CHECK(fk_pair_open(8000, 80, 2, 4, &got) == NULL);
	}

	CASE("raw open journals prepare, prefill and start in order");
	{
		struct udsp_tdm_geom got;
		struct udsp_tdm *t;

		fk_reset(&fk_an7581);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			int16_t rx[FK_CHANNELS_MAX * 1024];

			CHECK(!strcmp(fk_journal, "LP"));
			/* every group action rides the capture descriptor */
			CHECK_UINT(fk_journal_dir_get('L'), FK_CAPTURE);
			CHECK_UINT(fk_journal_dir_get('P'), FK_CAPTURE);
			for (unsigned i = 0; i <= FK_CAPTURE; i++) {
				CHECK_UINT(fk_start_threshold_seen[i],
					   tdm_raw_boundary_get(
						   fk[i].buffer_frames));
				CHECK_UINT(fk_sw_proto[i], SNDRV_PCM_VERSION);
				CHECK_UINT(fk_sw_period_step[i], 1);
			}
			/* the capture ring is never written through */
			CHECK(fk_map_prot[FK_CAPTURE] == PROT_READ);
			CHECK(fk_map_prot[FK_PLAY] ==
			      (PROT_READ | PROT_WRITE));
			/* no DMA runs before the start, so a read waits out
			 * its bound and reports the transient */
			CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == -EAGAIN);
			CHECK(fk_prefill_start(t, 3));
			CHECK(!strcmp(fk_journal, "LPpppSaaaeE"));
			CHECK_UINT(fk_journal_dir_get('S'), FK_CAPTURE);
			CHECK_UINT(fk[FK_PLAY].armed, 3);
			CHECK(fk[FK_PLAY].dma_on);
			CHECK_UINT(fk_start_unprepared, 0);
			udsp_tdm_alsa_raw.close(t);
			CHECK(strchr(fk_journal, 'U') != NULL);
			CHECK_UINT(fk_journal_dir_get('U'), FK_CAPTURE);
			CHECK_UINT(fk_fd_live, 0);
			CHECK_UINT(fk_map_live, 0);
		}

		/* the descriptor argument of LINK triggers first */
		CHECK_UINT(fk_group[0], FK_PLAY);
		CHECK_UINT(fk_group[1], FK_CAPTURE);
	}

	CASE("raw tick loop moves periods at both rates on each profile");
	{
		fk_loop_check(&fk_an7581, 8000, 80);
		fk_loop_check(&fk_an7581, 16000, 160);
		fk_loop_check(&fk_mt7986, 8000, 80);
		fk_loop_check(&fk_mt7986, 16000, 160);
		fk_loop_check(&fk_mt2701, 8000, 80);
		fk_loop_check(&fk_mt2701, 16000, 160);
	}

	CASE("raw tick loop tolerates a sub-period pointer lag");
	{
		fk_lag_check(&fk_mt7986, 8000, 80);
		fk_lag_check(&fk_mt7986, 16000, 160);
		fk_lag_check(&fk_mt2701, 8000, 80);
		fk_lag_check(&fk_mt2701, 16000, 160);
	}

	CASE("raw injected xrun recovers through drop and prepare");
	{
		struct udsp_tdm_geom got;
		struct udsp_tdm *t;
		int16_t rx[FK_CHANNELS_MAX * 1024];

		fk_reset(&fk_an7581);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			unsigned syncs;

			CHECK(fk_prefill_start(t, 3));
			fk_journal_clear();
			fk[FK_PLAY].stop_threshold = fk[FK_PLAY].buffer_frames;
			fk_advance(5);
			CHECK_UINT(fk[FK_CAPTURE].state,
				   (unsigned)SNDRV_PCM_STATE_XRUN);
			CHECK_UINT(fk[FK_PLAY].state,
				   (unsigned)SNDRV_PCM_STATE_XRUN);
			/* Every period raises its own interrupt, so the pair
			 * stops at the period that reached a stop threshold
			 * and not at the end of the run. The playback ring
			 * drains first: three prefilled periods. */
			CHECK_UINT(fk[FK_CAPTURE].dma_frames, 3 * 80);
			CHECK_UINT(fk[FK_PLAY].dma_frames, 3 * 80);
			fk_marker_fill(rx, 160);
			syncs = fk_sync_n[FK_CAPTURE];
			CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == -EPIPE);
			/* the wait carried the ready bit beside POLLERR, so
			 * the read asked for the pointers and left the ring
			 * and the engine buffer alone */
			CHECK(fk_marker_intact(rx, 160));
			CHECK_UINT(fk_sync_n[FK_CAPTURE] - syncs, 1);
			CHECK(fk_sync_flags[FK_CAPTURE] &
			      SNDRV_PCM_SYNC_PTR_APPL);
			CHECK(udsp_tdm_alsa_raw.recover(t) == 0);
			CHECK(strstr(fk_journal, "DP") != NULL);
			CHECK_UINT(fk_journal_dir_get('D'), FK_CAPTURE);
			CHECK_UINT(fk_journal_dir_get('P'), FK_CAPTURE);
			CHECK_UINT(fk[FK_CAPTURE].state,
				   (unsigned)SNDRV_PCM_STATE_PREPARED);
			CHECK_UINT(fk[FK_PLAY].hw_ptr, 0);
			CHECK_UINT(fk[FK_PLAY].appl_ptr, 0);
			CHECK(udsp_tdm_alsa_raw.delay(t) == 0);
			CHECK(fk_prefill_start(t, 3));
			CHECK(udsp_tdm_alsa_raw.delay(t) == 3 * 80);
			fk_advance(1);
			CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == 0);
			udsp_tdm_alsa_raw.close(t);
		}
	}

	CASE("raw interrupt blackout returns skipped periods without an xrun");
	{
		fk_blackout_check(&fk_mt7986);
		fk_blackout_check(&fk_batch_mmap);
	}

	CASE("raw read timeout returns EAGAIN and moves no pointer");
	{
		struct udsp_tdm_geom got;
		struct udsp_tdm *t;
		int16_t rx[FK_CHANNELS_MAX * 1024];

		fk_reset(&fk_an7581);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			unsigned syncs;

			CHECK(fk_prefill_start(t, 3));
			fk_poll_timeouts[FK_CAPTURE] = 1;
			syncs = fk_sync_n[FK_CAPTURE];
			CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == -EAGAIN);
			CHECK_UINT(fk_sync_n[FK_CAPTURE] - syncs, 0);
			CHECK_UINT(fk[FK_CAPTURE].appl_ptr, 0);
			CHECK_UINT(fk[FK_CAPTURE].hw_ptr, 0);
			/* the bound the contract of tdm.h asks for */
			CHECK(fk_poll_timeout_sec == 0);
			CHECK(fk_poll_timeout_nsec == 100L * 1000000L);
			udsp_tdm_alsa_raw.close(t);
		}
	}

	CASE("raw write back-pressure returns EAGAIN after one wait");
	{
		struct udsp_tdm_geom got;
		struct udsp_tdm *t;
		int16_t tx[FK_CHANNELS_MAX * 1024];

		memset(tx, 0, sizeof(tx));
		fk_reset(&fk_an7581);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			unsigned long appl;
			unsigned polls;

			/* fill the ring: four periods with no DMA progress */
			CHECK(fk_prefill_start(t, 4));
			appl = fk[FK_PLAY].appl_ptr;
			polls = fk_poll_n[FK_PLAY];
			fk_poll_timeouts[FK_PLAY] = 1;
			CHECK(udsp_tdm_alsa_raw.write_period(t, tx) ==
			      -EAGAIN);
			CHECK_UINT(fk_poll_n[FK_PLAY] - polls, 1);
			CHECK_UINT(fk[FK_PLAY].appl_ptr, appl);
			udsp_tdm_alsa_raw.close(t);
		}
	}

	CASE("raw second avail refreshes only after a write");
	{
		struct udsp_tdm_geom got;
		struct udsp_tdm *t;
		int16_t rx[FK_CHANNELS_MAX * 1024];
		int16_t tx[FK_CHANNELS_MAX * 1024];

		memset(tx, 0, sizeof(tx));
		fk_reset(&fk_an7581);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			unsigned syncs;

			CHECK(fk_prefill_start(t, 3));
			fk_advance(1);
			CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == 0);
			syncs = fk_sync_n[FK_CAPTURE];
			CHECK(udsp_tdm_alsa_raw.avail(t) == 0);
			CHECK_UINT(fk_sync_n[FK_CAPTURE] - syncs, 0);
			CHECK(udsp_tdm_alsa_raw.write_period(t, tx) == 0);
			fk_advance(1);
			CHECK(udsp_tdm_alsa_raw.avail(t) == 80);
			CHECK_UINT(fk_sync_n[FK_CAPTURE] - syncs, 1);
			udsp_tdm_alsa_raw.close(t);
		}
	}

	CASE("raw rate zero opens at 8000");
	{
		struct udsp_tdm_geom got;
		struct udsp_tdm *t;

		fk_reset(&fk_an7581);
		t = fk_pair_open(0, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			CHECK_UINT(got.rate, 8000);
			CHECK_UINT(got.frames_per_period, 80);
			CHECK_UINT(fk[FK_CAPTURE].rate, 8000);
			udsp_tdm_alsa_raw.close(t);
		}
	}

	CASE("raw error states map to the contract errnos");
	{
		struct udsp_tdm_geom got;
		struct udsp_tdm *t;
		int16_t rx[FK_CHANNELS_MAX * 1024];
		int16_t tx[FK_CHANNELS_MAX * 1024];

		memset(tx, 0, sizeof(tx));

		/* a start from SETUP never leaves PREPARED behind it */
		fk_reset(&fk_an7581);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			CHECK(fk_prefill_start(t, 3));
			fk_journal_clear();
			CHECK(udsp_tdm_alsa_raw.stop(t) == 0);
			CHECK_UINT(fk_journal_dir_get('D'), FK_CAPTURE);
			CHECK_UINT(fk[FK_CAPTURE].state,
				   (unsigned)SNDRV_PCM_STATE_SETUP);
			CHECK_UINT(fk[FK_PLAY].state,
				   (unsigned)SNDRV_PCM_STATE_SETUP);
			CHECK(udsp_tdm_alsa_raw.start(t) == -EBADFD);
			CHECK_UINT(fk_start_unprepared, 1);
			udsp_tdm_alsa_raw.close(t);
		}

		/* the group start aborts on an empty playback ring */
		fk_reset(&fk_an7581);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			CHECK(udsp_tdm_alsa_raw.start(t) == -EPIPE);
			CHECK_UINT(fk[FK_CAPTURE].state,
				   (unsigned)SNDRV_PCM_STATE_PREPARED);
			udsp_tdm_alsa_raw.close(t);
		}

		/* a period count the refine allowed and the driver refused */
		fk_reset(&fk_an7581);
		fk_reject_periods = 4;
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			CHECK_UINT(got.prefill_periods, 5);
			CHECK_UINT(fk_hw_params_n[FK_PLAY], 2);
			udsp_tdm_alsa_raw.close(t);
		}

		/* An xrun inside the back-pressure branch is not transient.
		 * The ring is full, so the write enters the branch, and its
		 * pointer read reports the stopped stream before any wait. */
		fk_reset(&fk_an7581);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			unsigned syncs, polls;
			unsigned long appl;

			CHECK(fk_prefill_start(t, 4));
			fk_state_force(SNDRV_PCM_STATE_XRUN, true);
			appl = fk[FK_PLAY].appl_ptr;
			syncs = fk_sync_n[FK_PLAY];
			polls = fk_poll_n[FK_PLAY];
			CHECK(udsp_tdm_alsa_raw.write_period(t, tx) == -EPIPE);
			CHECK_UINT(fk_sync_n[FK_PLAY] - syncs, 1);
			CHECK_UINT(fk_poll_n[FK_PLAY] - polls, 0);
			CHECK(fk_sync_flags[FK_PLAY] &
			      SNDRV_PCM_SYNC_PTR_APPL);
			CHECK_UINT(fk[FK_PLAY].appl_ptr, appl);
			udsp_tdm_alsa_raw.close(t);
		}

		/* the ready bit arrives with POLLERR and the error wins */
		fk_reset(&fk_an7581);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			unsigned syncs;

			CHECK(fk_prefill_start(t, 3));
			fk_advance(1);
			fk_state_force(SNDRV_PCM_STATE_XRUN, false);
			fk_marker_fill(rx, 160);
			syncs = fk_sync_n[FK_CAPTURE];
			CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == -EPIPE);
			CHECK(fk_marker_intact(rx, 160));
			CHECK_UINT(fk_sync_n[FK_CAPTURE] - syncs, 1);
			CHECK(fk_sync_flags[FK_CAPTURE] &
			      SNDRV_PCM_SYNC_PTR_APPL);
			udsp_tdm_alsa_raw.close(t);
		}

		/* a removed card answers -EBADFD from the ioctl itself */
		fk_reset(&fk_an7581);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			CHECK(fk_prefill_start(t, 3));
			fk[FK_CAPTURE].state = SNDRV_PCM_STATE_DISCONNECTED;
			CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == -EBADFD);
			CHECK(udsp_tdm_alsa_raw.recover(t) == -EBADFD);
			udsp_tdm_alsa_raw.close(t);
		}

		/* a refused device string never reaches the card, and the
		 * record names the direction whose name was refused */
		fk_reset(&fk_an7581);
		{
			struct udsp_tdm_geom want = {
				.frames_per_period = 80,
				.channels = 2,
				.prefill_periods = 4,
				.rate = 8000,
			};

			memset(&fk_refusal, 0, sizeof(fk_refusal));
			CHECK(udsp_tdm_alsa_raw.open("default", "hw:0,1",
						     &want, &got,
						     &fk_refusal) == NULL);
			CHECK(!fk[FK_PLAY].open);
			CHECK_UINT(fk_fd_live, 0);
			CHECK_UINT(fk_map_live, 0);
			fk_refusal_check(false, -EINVAL, UDSP_TDM_DIR_PLAYBACK,
					 "device name");

			memset(&fk_refusal, 0, sizeof(fk_refusal));
			CHECK(udsp_tdm_alsa_raw.open("hw:0,0", "default",
						     &want, &got,
						     &fk_refusal) == NULL);
			CHECK_UINT(fk_fd_live, 0);
			fk_refusal_check(false, -EINVAL, UDSP_TDM_DIR_CAPTURE,
					 "device name");
		}
	}

	CASE("raw write back-pressure clears without a wait once room exists");
	{
		struct udsp_tdm_geom got;
		struct udsp_tdm *t;
		int16_t tx[FK_CHANNELS_MAX * 1024];

		memset(tx, 0, sizeof(tx));

		/* the DMA moved, so the pointer read alone finds the room */
		fk_reset(&fk_an7581);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			unsigned syncs, polls;

			CHECK(fk_prefill_start(t, 4));
			fk_advance(1);
			syncs = fk_sync_n[FK_PLAY];
			polls = fk_poll_n[FK_PLAY];
			CHECK(udsp_tdm_alsa_raw.write_period(t, tx) == 0);
			CHECK_UINT(fk_poll_n[FK_PLAY] - polls, 0);
			CHECK_UINT(fk_sync_n[FK_PLAY] - syncs, 2);
			udsp_tdm_alsa_raw.close(t);
		}

		/* the DMA stood still, so the wait carries the next period */
		fk_reset(&fk_an7581);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			unsigned polls;

			CHECK(fk_prefill_start(t, 4));
			polls = fk_poll_n[FK_PLAY];
			CHECK(udsp_tdm_alsa_raw.write_period(t, tx) == 0);
			CHECK_UINT(fk_poll_n[FK_PLAY] - polls, 1);
			udsp_tdm_alsa_raw.close(t);
		}
	}

	CASE("raw second avail sees what the hardware sync reveals");
	{
		struct udsp_tdm_geom got;
		struct udsp_tdm *t;
		int16_t rx[FK_CHANNELS_MAX * 1024];
		int16_t tx[FK_CHANNELS_MAX * 1024];

		memset(tx, 0, sizeof(tx));
		fk_reset(&fk_mt7986);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			CHECK(fk_prefill_start(t, 3));
			fk_advance(1);
			CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == 0);
			CHECK(udsp_tdm_alsa_raw.avail(t) == 0);
			CHECK(udsp_tdm_alsa_raw.write_period(t, tx) == 0);
			/* the DMA runs on and raises no interrupt, so only the
			 * HWSYNC of the refresh finds the new period */
			fk_overrun(1);
			CHECK_UINT(fk[FK_CAPTURE].hw_ptr, 80);
			CHECK(udsp_tdm_alsa_raw.avail(t) == 80);
			CHECK_UINT(fk[FK_CAPTURE].hw_ptr, 160);
			udsp_tdm_alsa_raw.close(t);
		}
	}

	CASE("raw capture avail folds across the wrap point");
	{
		struct udsp_tdm_geom got;
		struct udsp_tdm *t;
		int16_t rx[FK_CHANNELS_MAX * 1024];
		int16_t tx[FK_CHANNELS_MAX * 1024];

		memset(tx, 0, sizeof(tx));
		fk_reset(&fk_an7581);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			unsigned long edge;

			CHECK(fk_prefill_start(t, 3));
			edge = fk[FK_CAPTURE].boundary -
			       fk[FK_CAPTURE].period_frames;
			/* run the pair up to the last period before the wrap */
			for (unsigned n = 0; n < 64; n++) {
				if (fk[FK_CAPTURE].appl_ptr == edge)
					break;
				fk_advance(1);
				CHECK(udsp_tdm_alsa_raw.read_period(t, rx) ==
				      0);
				CHECK(udsp_tdm_alsa_raw.write_period(t, tx) ==
				      0);
			}
			CHECK_UINT(fk[FK_CAPTURE].appl_ptr, edge);
			fk_advance(1);
			/* the hardware pointer wrapped and the application
			 * pointer did not, so plain subtraction underflows */
			CHECK(fk[FK_CAPTURE].hw_ptr <
			      fk[FK_CAPTURE].appl_ptr);
			CHECK(udsp_tdm_alsa_raw.avail(t) == 80);
			CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == 0);
			CHECK(udsp_tdm_alsa_raw.avail(t) == 0);
			CHECK_UINT(fk[FK_CAPTURE].appl_ptr, 0);
			udsp_tdm_alsa_raw.close(t);
		}
	}

	/* Every refusal writes the caller's record (tdm.h Refusal): none of
	 * these is the device's answer about the geometry, so none may move
	 * the line rate, and each names its own call or check. */
	CASE("raw open refuses a card that breaks the open contract");
	{
		struct udsp_tdm_geom got;

		/* the call the open depends on is missing on a card that
		 * claims to carry it */
		fk_reset(&fk_an7581);
		fk_user_pversion_fail = true;
		CHECK(fk_pair_open(8000, 80, 2, 4, &got) == NULL);
		CHECK_UINT(fk_fd_live, 0);
		CHECK_UINT(fk_map_live, 0);
		fk_refusal_check(false, -ENOTTY, UDSP_TDM_DIR_PLAYBACK,
				 "protocol version");

		fk_reset(&fk_an7581);
		fk_periods_max_open = true;
		CHECK(fk_pair_open(8000, 80, 2, 4, &got) == NULL);
		CHECK_UINT(fk_fd_live, 0);
		fk_refusal_check(false, -EINVAL, UDSP_TDM_DIR_PLAYBACK,
				 "period count range");

		fk_reset(&fk_an7581);
		fk_channel_info_fault = FK_CI_OFFSET;
		CHECK(fk_pair_open(8000, 80, 2, 4, &got) == NULL);
		CHECK_UINT(fk_fd_live, 0);
		CHECK_UINT(fk_map_live, 0);
		fk_refusal_check(false, -EINVAL, UDSP_TDM_DIR_PLAYBACK,
				 "channel area");

		fk_reset(&fk_an7581);
		fk_channel_info_fault = FK_CI_STEP;
		CHECK(fk_pair_open(8000, 80, 2, 4, &got) == NULL);
		CHECK_UINT(fk_fd_live, 0);
		fk_refusal_check(false, -EINVAL, UDSP_TDM_DIR_PLAYBACK,
				 "channel area");

		fk_reset(&fk_an7581);
		fk_channel_info_fault = FK_CI_OUTSIDE;
		CHECK(fk_pair_open(8000, 80, 2, 4, &got) == NULL);
		CHECK_UINT(fk_fd_live, 0);
		fk_refusal_check(false, -EINVAL, UDSP_TDM_DIR_PLAYBACK,
				 "channel area");

		/* a channel layout the wide copy cannot serve, on each of the
		 * two shapes the back end knows */
		fk_reset(&fk_an7581);
		fk_channel_info_fault = FK_CI_SHAPE;
		CHECK(fk_pair_open(8000, 80, 2, 4, &got) == NULL);
		CHECK_UINT(fk_fd_live, 0);
		CHECK_UINT(fk_map_live, 0);
		fk_refusal_check(false, -EINVAL, UDSP_TDM_DIR_PLAYBACK,
				 "channel area shape");

		fk_reset(&fk_mt7986);
		fk_channel_info_fault = FK_CI_SHAPE;
		CHECK(fk_pair_open(8000, 80, 2, 4, &got) == NULL);
		CHECK_UINT(fk_fd_live, 0);
		CHECK_UINT(fk_map_live, 0);
		fk_refusal_check(false, -EINVAL, UDSP_TDM_DIR_PLAYBACK,
				 "channel area shape");

		fk_reset(&fk_an7581);
		fk_boundary_odd = true;
		CHECK(fk_pair_open(8000, 80, 2, 4, &got) == NULL);
		CHECK_UINT(fk_fd_live, 0);
		fk_refusal_check(false, -EINVAL, UDSP_TDM_DIR_PLAYBACK,
				 "boundary");

		fk_reset(&fk_an7581);
		fk_info_no_mmap = true;
		CHECK(fk_pair_open(8000, 80, 2, 4, &got) == NULL);
		CHECK_UINT(fk_fd_live, 0);
		fk_refusal_check(false, -EINVAL, UDSP_TDM_DIR_PLAYBACK,
				 "no mmap access");

		/* a ring that is not a whole number of periods cannot serve
		 * the AN7581 transmit copy */
		fk_reset(&fk_an7581);
		fk_buffer_extra = 8;
		CHECK(fk_pair_open(8000, 80, 2, 4, &got) == NULL);
		CHECK_UINT(fk_fd_live, 0);
		CHECK_UINT(fk_map_live, 0);
		fk_refusal_check(false, -EINVAL, UDSP_TDM_DIR_PLAYBACK,
				 "ring not a whole number of periods");

		/* a device without S16_LE or mmap access refuses the access
		 * and format probe, which runs before the geometry is pinned */
		fk_reset(&fk_an7581);
		fk_refine_format_fail = true;
		CHECK(fk_pair_open(8000, 80, 2, 4, &got) == NULL);
		CHECK_UINT(fk_fd_live, 0);
		CHECK_UINT(fk_hw_params_n[FK_PLAY], 0);
		fk_refusal_check(false, -EINVAL, UDSP_TDM_DIR_PLAYBACK,
				 "access or format");

		/* every period count in the search window refused: the window
		 * does not prove the device declines the rate or the period */
		fk_reset(&fk_an7581);
		fk_periods_floor_refuse = 2;
		CHECK(fk_pair_open(8000, 80, 2, 4, &got) == NULL);
		CHECK_UINT(fk_fd_live, 0);
		CHECK(fk_hw_params_n[FK_PLAY] > 1);
		fk_refusal_check(false, -EINVAL, UDSP_TDM_DIR_PLAYBACK,
				 "hw_params");

		/* the AN7581 refuses the second direction with -EBUSY when the
		 * first one runs another geometry: not a rate answer */
		fk_reset(&fk_an7581);
		fk_peer_busy = true;
		CHECK(fk_pair_open(8000, 80, 2, 4, &got) == NULL);
		CHECK_UINT(fk_fd_live, 0);
		CHECK_UINT(fk_map_live, 0);
		fk_refusal_check(false, -EBUSY, UDSP_TDM_DIR_CAPTURE,
				 "hw_params");

		/* the node is missing, or another process holds it */
		fk_reset(&fk_an7581);
		fk_open_errno = ENOENT;
		CHECK(fk_pair_open(8000, 80, 2, 4, &got) == NULL);
		CHECK_UINT(fk_fd_live, 0);
		fk_refusal_check(false, -ENOENT, UDSP_TDM_DIR_PLAYBACK, "open");

		fk_reset(&fk_an7581);
		{
			struct udsp_tdm *first = fk_pair_open(8000, 80, 2, 4,
							      &got);

			CHECK(first != NULL);
			CHECK(fk_pair_open(8000, 80, 2, 4, &got) == NULL);
			fk_refusal_check(false, -EBUSY, UDSP_TDM_DIR_PLAYBACK,
					 "open");
			CHECK_UINT(fk_fd_live, 2);
			if (first)
				udsp_tdm_alsa_raw.close(first);
			CHECK_UINT(fk_fd_live, 0);
		}
	}

	/* The device's own answer about the geometry: the pinned refine
	 * refuses a rate or a period the card cannot run, and only that
	 * answer may move the line rate (engine.h enum engine_open_fail). */
	CASE("raw open marks a refused rate or period as the geometry answer");
	{
		struct udsp_tdm_geom got;

		fk_reset(&fk_an7581);
		CHECK(fk_pair_open(44100, 441, 2, 4, &got) == NULL);
		CHECK_UINT(fk_fd_live, 0);
		CHECK_UINT(fk_hw_params_n[FK_PLAY], 0);
		fk_refusal_check(true, -EINVAL, UDSP_TDM_DIR_PLAYBACK,
				 "hw_refine");

		/* 2048 frames of two channels is past the 4080-byte period
		 * ceiling of the AN7581 */
		fk_reset(&fk_an7581);
		CHECK(fk_pair_open(8000, 2048, 2, 4, &got) == NULL);
		CHECK_UINT(fk_fd_live, 0);
		fk_refusal_check(true, -EINVAL, UDSP_TDM_DIR_PLAYBACK,
				 "hw_refine");

		/* the request itself is outside the contract, which is not
		 * the device's answer */
		fk_reset(&fk_an7581);
		CHECK(fk_pair_open(8000, 80, 3, 4, &got) == NULL);
		CHECK_UINT(fk_fd_live, 0);
		fk_refusal_check(false, -EINVAL, UDSP_TDM_DIR_NONE,
				 "geometry request");

		/* a NULL record is allowed */
		fk_reset(&fk_an7581);
		{
			struct udsp_tdm_geom want = {
				.frames_per_period = 441,
				.channels = 2,
				.prefill_periods = 4,
				.rate = 44100,
			};

			CHECK(udsp_tdm_alsa_raw.open("hw:0,0", "hw:0,1", &want,
						     &got, NULL) == NULL);
			CHECK_UINT(fk_fd_live, 0);
		}
	}

	CASE("raw suspended streams report ESTRPIPE and recover");
	{
		struct udsp_tdm_geom got;
		struct udsp_tdm *t;
		int16_t rx[FK_CHANNELS_MAX * 1024];
		int16_t tx[FK_CHANNELS_MAX * 1024];

		memset(tx, 0, sizeof(tx));
		fk_reset(&fk_an7581);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			CHECK(fk_prefill_start(t, 3));
			fk_advance(1);
			fk_state_force(SNDRV_PCM_STATE_SUSPENDED, true);
			fk_marker_fill(rx, 160);
			CHECK(udsp_tdm_alsa_raw.read_period(t, rx) ==
			      -ESTRPIPE);
			CHECK(fk_marker_intact(rx, 160));
			CHECK(udsp_tdm_alsa_raw.write_period(t, tx) ==
			      -ESTRPIPE);
			CHECK(udsp_tdm_alsa_raw.avail(t) == -ESTRPIPE);
			CHECK(udsp_tdm_alsa_raw.recover(t) == 0);
			CHECK_UINT(fk[FK_CAPTURE].state,
				   (unsigned)SNDRV_PCM_STATE_PREPARED);
			CHECK(fk_prefill_start(t, 3));
			fk_advance(1);
			CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == 0);
			CHECK(udsp_tdm_alsa_raw.write_period(t, tx) == 0);
			CHECK(udsp_tdm_alsa_raw.delay(t) == 3 * 80);
			udsp_tdm_alsa_raw.close(t);
		}
	}

	CASE("raw playback pointer lag costs one wait per tick");
	{
		struct udsp_tdm_geom got;
		struct udsp_tdm *t;
		int16_t rx[FK_CHANNELS_MAX * 1024];
		int16_t tx[FK_CHANNELS_MAX * 1024];

		memset(tx, 0, sizeof(tx));
		fk_reset(&fk_mt7986);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			CHECK(fk_prefill_start(t, 3));
			fk_lag[FK_PLAY] = 40;
			for (unsigned n = 0; n < 20; n++) {
				unsigned cap = fk_sync_n[FK_CAPTURE];
				unsigned play = fk_sync_n[FK_PLAY];
				unsigned polls = fk_poll_n[0] + fk_poll_n[1];
				unsigned sleeps = fk_sleep_n;

				fk_advance(1);
				CHECK(udsp_tdm_alsa_raw.read_period(t, rx) ==
				      0);
				CHECK(udsp_tdm_alsa_raw.avail(t) == 0);
				CHECK(udsp_tdm_alsa_raw.write_period(t, tx) ==
				      0);
				CHECK(udsp_tdm_alsa_raw.delay(t) > 0);
				CHECK(udsp_tdm_alsa_raw.avail(t) >= 0);
				if (!n)
					continue;
				/* the write reads the pointer once and finds
				 * the room, so the wait stays with the read */
				CHECK_UINT(fk_poll_n[0] + fk_poll_n[1] - polls,
					   1);
				CHECK_UINT(fk_sync_n[FK_CAPTURE] - cap, 3);
				CHECK_UINT(fk_sync_n[FK_PLAY] - play, 2);
				CHECK_UINT(fk_sleep_n - sleeps, 0);
			}
			udsp_tdm_alsa_raw.close(t);
		}

		/* Both directions lagging by the same amount inside the wake
		 * margin spares the write its own read: the capture completion
		 * carries the tick and the cached playback pair still holds a
		 * period of room. */
		fk_reset(&fk_mt7986);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			CHECK(fk_prefill_start(t, 3));
			fk_lag[FK_PLAY] = 10;
			fk_lag[FK_CAPTURE] = 10;
			for (unsigned n = 0; n < 20; n++) {
				unsigned cap = fk_sync_n[FK_CAPTURE];
				unsigned play = fk_sync_n[FK_PLAY];
				unsigned polls = fk_poll_n[0] + fk_poll_n[1];
				unsigned sleeps = fk_sleep_n;

				fk_advance(1);
				CHECK(udsp_tdm_alsa_raw.read_period(t, rx) ==
				      0);
				CHECK(udsp_tdm_alsa_raw.avail(t) == 0);
				CHECK(udsp_tdm_alsa_raw.write_period(t, tx) ==
				      0);
				CHECK(udsp_tdm_alsa_raw.delay(t) > 0);
				CHECK(udsp_tdm_alsa_raw.avail(t) >= 0);
				if (!n)
					continue;
				CHECK_UINT(fk_poll_n[0] + fk_poll_n[1] - polls,
					   1);
				CHECK_UINT(fk_sync_n[FK_CAPTURE] - cap, 4);
				CHECK_UINT(fk_sync_n[FK_PLAY] - play, 1);
				CHECK_UINT(fk_sleep_n - sleeps, 1);
			}
			udsp_tdm_alsa_raw.close(t);
		}
	}

	CASE("raw open gives the period count a bounded number of tries");
	{
		struct udsp_tdm_geom got;

		/* every count the refine allows is refused, so the open runs
		 * the retry out and fails */
		fk_reset(&fk_mt7986);
		fk_periods_floor_refuse = 4;
		CHECK(fk_pair_open(8000, 80, 2, 4, &got) == NULL);
		CHECK_UINT(fk_hw_params_n[FK_PLAY], 17);
		CHECK_UINT(fk_fd_live, 0);
		CHECK_UINT(fk_map_live, 0);
	}

	CASE("raw write reports a fault that lands during its wait");
	{
		struct udsp_tdm_geom got;
		struct udsp_tdm *t;
		int16_t tx[FK_CHANNELS_MAX * 1024];

		memset(tx, 0, sizeof(tx));
		fk_reset(&fk_an7581);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			unsigned syncs, polls;
			unsigned long appl;

			/* the ring is full and the peer overruns while the
			 * writer waits, so the group stops under the wait */
			CHECK(fk_prefill_start(t, 4));
			fk_poll_group_xrun[FK_PLAY] = 1;
			appl = fk[FK_PLAY].appl_ptr;
			syncs = fk_sync_n[FK_PLAY];
			polls = fk_poll_n[FK_PLAY];
			CHECK(udsp_tdm_alsa_raw.write_period(t, tx) == -EPIPE);
			CHECK_UINT(fk_poll_n[FK_PLAY] - polls, 1);
			CHECK_UINT(fk_sync_n[FK_PLAY] - syncs, 2);
			CHECK(fk_sync_flags[FK_PLAY] &
			      SNDRV_PCM_SYNC_PTR_APPL);
			CHECK_UINT(fk[FK_PLAY].appl_ptr, appl);
			udsp_tdm_alsa_raw.close(t);
		}
	}

	CASE("raw read reports an interrupted wait and moves no pointer");
	{
		struct udsp_tdm_geom got;
		struct udsp_tdm *t;
		int16_t rx[FK_CHANNELS_MAX * 1024];

		fk_reset(&fk_an7581);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			unsigned syncs;

			CHECK(fk_prefill_start(t, 3));
			fk_advance(1);
			fk_poll_eintr[FK_CAPTURE] = 1;
			fk_marker_fill(rx, 160);
			syncs = fk_sync_n[FK_CAPTURE];
			CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == -EINTR);
			CHECK(fk_marker_intact(rx, 160));
			CHECK_UINT(fk_sync_n[FK_CAPTURE] - syncs, 0);
			CHECK_UINT(fk[FK_CAPTURE].appl_ptr, 0);
			/* the retry the engine makes still finds the period */
			CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == 0);
			CHECK_UINT(fk[FK_CAPTURE].appl_ptr, 80);
			udsp_tdm_alsa_raw.close(t);
		}
	}

	CASE("raw injected xrun across the wrap point recovers");
	{
		struct udsp_tdm_geom got;
		struct udsp_tdm *t;
		int16_t rx[FK_CHANNELS_MAX * 1024];
		int16_t tx[FK_CHANNELS_MAX * 1024];

		memset(tx, 0, sizeof(tx));
		fk_reset(&fk_an7581);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			unsigned long edge;

			CHECK(fk_prefill_start(t, 3));
			edge = fk[FK_CAPTURE].boundary -
			       2 * fk[FK_CAPTURE].period_frames;
			for (unsigned n = 0; n < 64; n++) {
				if (fk[FK_CAPTURE].appl_ptr == edge)
					break;
				fk_advance(1);
				CHECK(udsp_tdm_alsa_raw.read_period(t, rx) ==
				      0);
				CHECK(udsp_tdm_alsa_raw.write_period(t, tx) ==
				      0);
			}
			CHECK_UINT(fk[FK_CAPTURE].appl_ptr, edge);
			/* the xrun falls with the pair straddling the wrap */
			fk[FK_PLAY].stop_threshold = fk[FK_PLAY].buffer_frames;
			fk_advance(5);
			CHECK(fk[FK_CAPTURE].hw_ptr <
			      fk[FK_CAPTURE].appl_ptr);
			CHECK_UINT(fk[FK_CAPTURE].state,
				   (unsigned)SNDRV_PCM_STATE_XRUN);
			CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == -EPIPE);
			CHECK(udsp_tdm_alsa_raw.recover(t) == 0);
			CHECK_UINT(fk[FK_CAPTURE].appl_ptr, 0);
			CHECK(fk_prefill_start(t, 3));
			fk_advance(1);
			CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == 0);
			CHECK(udsp_tdm_alsa_raw.avail(t) == 0);
			udsp_tdm_alsa_raw.close(t);
		}
	}

	CASE("raw sample-accurate wake completes the period with a sleep");
	{
		struct fk_loop_want w = {
			.counts = true,
			/* two completion reads, the capture push, the playback
			 * push and the capture read beside the delay */
			.syncs = 5,
			.polls = 1,
			.sleeps = 1,
			.avail_zero = true,
			.delay_exact = false,
		};
		struct udsp_tdm_geom got;
		struct fk_loop_result r;
		struct udsp_tdm *t;

		fk_reset(&fk_mt7986);
		t = fk_pair_open(16000, 160, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			CHECK_UINT(fk[FK_CAPTURE].avail_min, 120);
			CHECK(fk_prefill_start(t, 3));
			fk_lag[FK_CAPTURE] = 20;
			fk_loop_run(t, &got, 200, 3, &w, &r);
			CHECK(r.capture_ok);
			CHECK(r.bytes_ok);
			CHECK(r.avail_ok);
			CHECK(r.delay_ok);
			CHECK(r.counts_ok);
			CHECK(r.wraps >= 2);
			/* The completion sleep moves the phase of the tick by
			 * the frames it waited, and the playback queue holds
			 * that much less for the whole run. */
			CHECK(udsp_tdm_alsa_raw.delay(t) ==
			      (long)(3 * 160 - 20));
			/* every wait took the interrupt that was already
			 * there, so no tick arrived paired with another */
			CHECK_UINT(fk_poll_steps_n, 0);
			udsp_tdm_alsa_raw.close(t);
		}
	}

	CASE("raw sample-accurate wake carries a self-paced run");
	{
		struct udsp_tdm_geom got;
		struct udsp_tdm *t;
		int16_t rx[FK_CHANNELS_MAX * 1024];
		int16_t tx[FK_CHANNELS_MAX * 1024];

		memset(tx, 0, sizeof(tx));
		fk_reset(&fk_mt7986);
		t = fk_pair_open(16000, 160, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			bool ok = true;

			CHECK(fk_prefill_start(t, 3));
			fk_lag[FK_CAPTURE] = 20;
			/* No case drives the clock here. The wait takes the
			 * interrupt it finds, so the run paces itself and a
			 * paired wake would show as a step inside a wait. */
			for (unsigned n = 0; n < 100; n++) {
				unsigned sleeps = fk_sleep_n;

				fk_clock_step();
				ok = udsp_tdm_alsa_raw.read_period(t, rx) ==
				     0 && ok;
				ok = udsp_tdm_alsa_raw.avail(t) == 0 && ok;
				ok = udsp_tdm_alsa_raw.write_period(t, tx) ==
				     0 && ok;
				ok = udsp_tdm_alsa_raw.avail(t) >= 0 && ok;
				ok = fk_sleep_n - sleeps == 1 && ok;
			}
			CHECK(ok);
			CHECK_UINT(fk_poll_steps_n, 0);
			CHECK_UINT(fk_sleep_n, 100);
			udsp_tdm_alsa_raw.close(t);
		}
	}

	CASE("raw completion sleep covers the frames the period owes");
	{
		struct udsp_tdm_geom got;
		struct udsp_tdm *t;
		int16_t rx[FK_CHANNELS_MAX * 1024];
		const uint64_t frame_ns = 1000000000u / 16000u;

		fk_reset(&fk_mt7986);
		t = fk_pair_open(16000, 160, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			CHECK(fk_prefill_start(t, 3));
			fk_lag[FK_CAPTURE] = 20;
			fk_advance(1);
			CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == 0);
			CHECK_UINT(fk_sleep_n, 1);
			CHECK(fk_sleep_ns >= 20u * frame_ns);
			CHECK(fk_sleep_ns - 20u * frame_ns <= frame_ns);
			/* a relative sleep on the clock a wall clock step
			 * cannot move */
			CHECK(fk_sleep_clock == CLOCK_MONOTONIC);
			CHECK_UINT(fk_sleep_flags, 0);
			udsp_tdm_alsa_raw.close(t);
		}
	}

	CASE("raw period-granular devices keep one wake and no sleep");
	{
		struct fk_loop_want w = {
			.counts = true,
			.syncs = 3,
			.polls = 1,
			.sleeps = 0,
			.avail_zero = true,
			.delay_exact = true,
		};
		struct udsp_tdm_geom got;
		struct fk_loop_result r;
		struct udsp_tdm *t;

		/* The AN7581 counts whole periods, so the internal buffer of
		 * the part cannot show in its pointer and the lag knob changes
		 * nothing. */
		fk_reset(&fk_an7581);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			CHECK_UINT(fk[FK_CAPTURE].avail_min, 80);
			CHECK(fk_prefill_start(t, 3));
			fk_lag[FK_CAPTURE] = 20;
			fk_lag[FK_PLAY] = 20;
			fk_loop_run(t, &got, 200, 3, &w, &r);
			CHECK(r.capture_ok);
			CHECK(r.counts_ok);
			CHECK_UINT(fk_sleep_n, 0);
			CHECK_UINT(fk_poll_steps_n, 0);
			udsp_tdm_alsa_raw.close(t);
		}

		/* The BATCH bit decides, not the access layout: this device is
		 * interleaved and still moves in period steps. */
		fk_reset(&fk_batch_mmap);
		t = fk_pair_open(8000, 80, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			CHECK_UINT(got.layout, UDSP_TDM_INTERLEAVED);
			CHECK_UINT(fk[FK_CAPTURE].avail_min, 80);
			CHECK(fk_prefill_start(t, 3));
			/* the pointer counts periods, so it discards the lag
			 * here too */
			fk_lag[FK_CAPTURE] = 20;
			fk_loop_run(t, &got, 200, 3, &w, &r);
			CHECK(r.capture_ok);
			CHECK(r.bytes_ok);
			CHECK(r.counts_ok);
			CHECK_UINT(fk_sleep_n, 0);
			udsp_tdm_alsa_raw.close(t);
		}
	}

	CASE("raw lag past the wake margin falls back without a fault");
	{
		struct fk_loop_want w = { 0 };
		struct udsp_tdm_geom got;
		struct fk_loop_result r;
		struct udsp_tdm *t;

		fk_reset(&fk_mt7986);
		t = fk_pair_open(16000, 160, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			CHECK(fk_prefill_start(t, 3));
			/* past the quarter period the threshold allows, so the
			 * first wait takes two interrupts and the reader then
			 * runs one period behind for good */
			fk_lag[FK_CAPTURE] = 60;
			fk_loop_run(t, &got, 200, 3, &w, &r);
			CHECK(r.capture_ok);
			CHECK(r.bytes_ok);
			CHECK(r.avail_ok);
			CHECK(r.delay_ok);
			/* the fallback ran: a wait took a second interrupt */
			CHECK(fk_poll_steps_n >= 1);
			CHECK(fk_poll_steps_n <= 2);
			CHECK_UINT(fk_sleep_n, 0);
			udsp_tdm_alsa_raw.close(t);
		}
	}

	CASE("raw completion rounds are bounded when the ring stands still");
	{
		struct udsp_tdm_geom got;
		struct udsp_tdm *t;
		int16_t rx[FK_CHANNELS_MAX * 1024];

		fk_reset(&fk_mt7986);
		t = fk_pair_open(16000, 160, 2, 4, &got);
		CHECK(t != NULL);
		if (t) {
			unsigned syncs;

			CHECK(fk_prefill_start(t, 3));
			fk_lag[FK_CAPTURE] = 20;
			fk_advance(1);
			/* the sleep passes and the ring does not move */
			fk_sleep_deaf = true;
			fk_marker_fill(rx, 320);
			syncs = fk_sync_n[FK_CAPTURE];
			CHECK(udsp_tdm_alsa_raw.read_period(t, rx) == -EAGAIN);
			CHECK(fk_marker_intact(rx, 320));
			CHECK_UINT(fk_sync_n[FK_CAPTURE] - syncs, 4);
			CHECK_UINT(fk_sleep_n, 3);
			CHECK_UINT(fk[FK_CAPTURE].appl_ptr, 0);
			udsp_tdm_alsa_raw.close(t);
		}
	}

	CASE("raw no-stop thresholds and silence follow the device profile");
	{
		fk_policy_check(&fk_an7581);
		fk_policy_check(&fk_mt7986);
		fk_policy_check(&fk_mt2701);
		fk_policy_check(&fk_batch_mmap);
	}

	CASE("raw capture depth steps down after a refused period count");
	{
		fk_capture_depth_check(&fk_an7581, 15, false);
		fk_capture_depth_check(&fk_mt7986, 16, false);
		fk_capture_depth_check(&fk_an7581, 15, true);
		fk_capture_depth_check(&fk_mt7986, 16, true);
	}

	CASE("raw capture depth follows playback requests above sixteen");
	{
		fk_capture_large_check(&fk_an7581, 15);
		fk_capture_large_check(&fk_mt7986, 17);
	}

	CASE("raw JIT playback freezes during a six-period capture hold");
	{
		fk_hold_capture_check(80, 6);
		fk_hold_capture_check(160, 6);
	}

	CASE("raw capture hold returns the oldest safe period on the grid");
	{
		fk_hold_capture_check(80, 13);
		fk_hold_capture_check(80, 14);
		fk_hold_capture_check(80, 16);
		fk_hold_capture_check(160, 16);
	}

	CASE("raw capture soft overrun preserves the grid across the boundary");
	fk_capture_wrap_check();

	CASE("raw fractional capture soft overrun rounds to a whole period");
	fk_fractional_capture_check();

	CASE("raw cyclic playback reconciles a fractional hold once");
	fk_fractional_hold_check();

	CASE("raw dry playback refreshes without a fresh-room request");
	fk_dry_playback_check();

	CASE("raw fresh capture and skip preserve the boundary grid");
	fk_fresh_capture_check();

	CASE("raw fresh playback rechecks progress across the boundary");
	fk_fresh_playback_check();

	CASE("raw two-period capture runs without false soft overruns");
	fk_capture_small_check();

	CASE("raw capture depth steps down after allocation failures");
	fk_capture_memory_check();

	CASE("raw fresh full playback avoids a duplicate pointer read");
	fk_fresh_full_check();

	CASE("raw fake validates kernel silence parameter bounds");
	fk_silence_params_check();

	fk_release();

	return tu_summary("tdm_raw");
}
