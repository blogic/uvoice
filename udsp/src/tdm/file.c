// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Virtual-clock tdm back end for host CI. No real device: the caller drives the
 * tick count. Capture reads one period per call from a raw int16 file, or
 * synthesises a known ramp when no input path is given; playback appends one
 * period per call to an output file. The engine-facing buffer is the fixed
 * planar layout of tdm.h (channel 0 first); on disk the period is in the device
 * access layout of geom.layout, so the interleaved conversion is exercised on
 * the host without ALSA. Raw files hold host-endian int16, which is S16_LE on
 * the little-endian host and target.
 *
 * The back end models the playback queue so delay() means what the contract
 * says and the engine's stall estimator sees a real prefill depth instead of a
 * constant zero. write_period queues one period; one virtual tick, that is one
 * successful read_period while started, drains one.
 *
 * What avail() means here. A virtual clock never blocks, so a capture backlog
 * cannot build up behind a read: the honest answer is the number of frames the
 * next read can still take. With an input file that is what is left of it,
 * capped at one period, so the engine sees a full period ready on every tick
 * that will succeed and zero on the tick that will hit the end and raise
 * -EPIPE. With the synthetic ramp the source is inexhaustible and the answer is
 * one period, always. Either way the backlog never exceeds one period, so this
 * backend reports no capture stall, which is exactly right: the engine, not the
 * file, sets the pace.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "udsp/tdm.h"
#include "tdm_common.h"

struct udsp_tdm {
	struct udsp_tdm_geom geom;
	FILE *in;
	FILE *out;
	int16_t *scratch;	/* one period in the on-disk access layout */
	unsigned period_samples;
	unsigned queued_frames;	/* playback frames not yet drained by a tick */
	unsigned in_frames;	/* capture frames the input file still holds */
	uint32_t ramp;		/* synthetic capture counter when in == NULL */
	bool started;
};

static void tdm_file_close(struct udsp_tdm *t)
{
	if (!t)
		return;
	if (t->in)
		fclose(t->in);
	if (t->out)
		fclose(t->out);
	free(t->scratch);
	free(t);
}

static struct udsp_tdm *tdm_file_open(const char *play_dev,
				      const char *capture_dev,
				      const struct udsp_tdm_geom *want,
				      struct udsp_tdm_geom *got,
				      struct udsp_tdm_fail *fail)
{
	enum udsp_tdm_dir dir = UDSP_TDM_DIR_NONE;
	const char *what = "allocation";
	int err = -ENOMEM;

	if (!want || !got) {
		tdm_fail_set(fail, dir, false, -EINVAL, "arguments");
		return NULL;
	}
	if (want->channels < 1 || want->channels > TDM_MAX_CHANNELS ||
	    want->frames_per_period == 0) {
		tdm_fail_set(fail, dir, false, -EINVAL, "geometry request");
		return NULL;
	}

	struct udsp_tdm *t = calloc(1, sizeof(*t));
	if (!t) {
		tdm_fail_set(fail, dir, false, err, what);
		return NULL;
	}

	t->geom = *want;
	t->geom.capture_periods = want->prefill_periods;
	t->geom.paced = false;	/* virtual clock */
	t->geom.tx_restarts = false;	/* got-only: no playback DMA */
	/* A virtual clock runs at any rate, so the grant is the request with
	 * the zero normalised to 8000. */
	t->geom.rate = want->rate ? want->rate : 8000u;
	t->period_samples = want->channels * want->frames_per_period;
	t->scratch = calloc(t->period_samples, sizeof(*t->scratch));
	if (!t->scratch)
		goto fail;

	if (capture_dev && capture_dev[0]) {
		long bytes;

		dir = UDSP_TDM_DIR_CAPTURE;
		what = "open";
		t->in = fopen(capture_dev, "rb");
		err = -errno;
		if (!t->in)
			goto fail;
		what = "seek";
		err = fseek(t->in, 0, SEEK_END) ? -errno : 0;
		if (err)
			goto fail;
		bytes = ftell(t->in);
		rewind(t->in);
		if (bytes > 0)
			t->in_frames = (unsigned)((size_t)bytes /
						  (sizeof(int16_t) *
						   t->geom.channels));
	}
	if (play_dev && play_dev[0]) {
		dir = UDSP_TDM_DIR_PLAYBACK;
		what = "open";
		t->out = fopen(play_dev, "wb");
		err = -errno;
		if (!t->out)
			goto fail;
	}

	*got = t->geom;
	return t;
fail:
	tdm_fail_set(fail, dir, false, err, what);
	tdm_file_close(t);
	return NULL;
}

static void tdm_file_capture_fill(struct udsp_tdm *t)
{
	for (unsigned i = 0; i < t->period_samples; i++)
		t->scratch[i] = (int16_t)(t->ramp++ & 0x7fff);
}

static void tdm_file_to_planar(const struct udsp_tdm *t, int16_t *planar)
{
	if (t->geom.layout == UDSP_TDM_INTERLEAVED) {
		tdm_interleaved_to_planar(planar, t->scratch, t->geom.channels,
					  t->geom.frames_per_period);
		return;
	}
	memcpy(planar, t->scratch, t->period_samples * sizeof(*planar));
}

static void tdm_file_from_planar(const struct udsp_tdm *t, const int16_t *planar)
{
	if (t->geom.layout == UDSP_TDM_INTERLEAVED) {
		tdm_planar_to_interleaved(t->scratch, planar, t->geom.channels,
					  t->geom.frames_per_period);
		return;
	}
	memcpy(t->scratch, planar, t->period_samples * sizeof(*planar));
}

static void tdm_file_tick_drain(struct udsp_tdm *t)
{
	if (!t->started)
		return;
	if (t->queued_frames < t->geom.frames_per_period) {
		t->queued_frames = 0;
		return;
	}
	t->queued_frames -= t->geom.frames_per_period;
}

static int tdm_file_capture_load(struct udsp_tdm *t)
{
	if (!t->in) {
		tdm_file_capture_fill(t);
		return 0;
	}
	if (fread(t->scratch, sizeof(*t->scratch), t->period_samples, t->in) !=
	    t->period_samples)
		return -EPIPE;
	t->in_frames -= t->in_frames < t->geom.frames_per_period ?
			t->in_frames : t->geom.frames_per_period;
	return 0;
}

static int tdm_file_read(struct udsp_tdm *t, int16_t *buf)
{
	int err = tdm_file_capture_load(t);
	if (err)
		return err;

	tdm_file_to_planar(t, buf);
	tdm_file_tick_drain(t);
	return 0;
}

static int tdm_file_write(struct udsp_tdm *t, const int16_t *buf)
{
	if (t->out) {
		tdm_file_from_planar(t, buf);
		if (fwrite(t->scratch, sizeof(*t->scratch), t->period_samples,
			   t->out) != t->period_samples)
			return -EIO;
	}

	t->queued_frames += t->geom.frames_per_period;
	return 0;
}

static int tdm_file_recover(struct udsp_tdm *t)
{
	if (t->in) {
		clearerr(t->in);
		if (fseek(t->in, 0, SEEK_SET) != 0)
			return -errno;
	}
	if (t->out)
		clearerr(t->out);
	t->ramp = 0;
	t->queued_frames = 0;
	if (t->in) {
		long bytes;

		if (fseek(t->in, 0, SEEK_END) != 0)
			return -errno;
		bytes = ftell(t->in);
		rewind(t->in);
		t->in_frames = bytes > 0 ?
			       (unsigned)((size_t)bytes / (sizeof(int16_t) *
							   t->geom.channels)) : 0;
	}
	return 0;
}

static int tdm_file_start(struct udsp_tdm *t)
{
	t->started = true;
	return 0;
}

static int tdm_file_stop(struct udsp_tdm *t)
{
	t->started = false;
	t->queued_frames = 0;
	if (t->out)
		fflush(t->out);
	return 0;
}

static long tdm_file_delay(struct udsp_tdm *t)
{
	return (long)t->queued_frames;
}

static long tdm_file_avail(struct udsp_tdm *t)
{
	unsigned period = t->geom.frames_per_period;

	if (!t->in)
		return (long)period;

	return (long)(t->in_frames < period ? t->in_frames : period);
}

const struct udsp_tdm_ops udsp_tdm_file = {
	.name = "file",
	.open = tdm_file_open,
	.close = tdm_file_close,
	.recover = tdm_file_recover,
	.start = tdm_file_start,
	.stop = tdm_file_stop,
	.read_period = tdm_file_read,
	.write_period = tdm_file_write,
	.delay = tdm_file_delay,
	.avail = tdm_file_avail,
};
