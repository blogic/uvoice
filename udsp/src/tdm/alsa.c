// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * ALSA tdm back end: two unidirectional PCM devices (playback, capture) linked
 * so one start() triggers both clocks together (tdm.h Linked start). MediaTek
 * mt7986 grants interleaved access with period_bytes_min 256, so the engine
 * opens two channels; AN7581 grants noninterleaved with a 64-byte floor.
 * open() reports the channel count and period the driver actually granted plus
 * the granted playback buffer depth in periods, which bounds the engine's
 * prefill. read_period/write_period always present the fixed planar
 * engine-facing layout (channel 0 first) and convert to or from each
 * direction's own access layout. An xrun surfaces as a negative errno so the
 * engine recovers with a prepare and re-prefill. The recovery drops a running
 * group before the prepare, because the engine also recovers from faults that
 * are not an xrun and the kernel refuses a prepare on a running stream.
 *
 * Each direction negotiates its own access layout. The AN7581 DCHENR/CHBFOSR
 * constraint binds the channel count and the period size, not the ALSA access
 * mode, and the access mode does not affect the linked start. got.layout
 * reports the capture direction and is informational only; both read_period
 * and write_period are planar whatever it says.
 *
 * This file builds only with HAVE_ALSA; the file back end and its host tests do
 * not need alsa-lib. It runs on the target only, because the CI host has no
 * card.
 *
 * Two paths are not host testable. snd_pcm_wait timing out, and
 * snd_pcm_avail_update returning an error, both need a real card: a stub that
 * returned canned values would test the stub, not the driver. Check both on a
 * live unit. Confirm that a stopped card makes read_period return -EAGAIN
 * inside TDM_ALSA_WAIT_MS and that the stop still completes. Confirm that an
 * xrun surfaces through avail() as a negative errno the engine counts.
 */
#ifdef HAVE_ALSA

#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "udsp/tdm.h"
#include "tdm_common.h"

/* Three times the worst interval measured under load (21.1 ms). */
#define TDM_ALSA_WAIT_MS	100

struct udsp_tdm {
	snd_pcm_t *play;
	snd_pcm_t *capture;
	struct udsp_tdm_geom geom;
	int16_t *rx_scratch;	/* device-layout capture staging, interleaved only */
	int16_t *tx_scratch;	/* device-layout playback staging, interleaved only */
	bool play_interleaved;
	bool capture_interleaved;
};

static enum udsp_tdm_layout tdm_alsa_layout(snd_pcm_access_t access)
{
	if (access == SND_PCM_ACCESS_RW_NONINTERLEAVED ||
	    access == SND_PCM_ACCESS_MMAP_NONINTERLEAVED)
		return UDSP_TDM_NONINTERLEAVED;
	return UDSP_TDM_INTERLEAVED;
}

static int tdm_alsa_configure(snd_pcm_t *pcm, const struct udsp_tdm_geom *want,
			      struct udsp_tdm_geom *got, enum udsp_tdm_dir dir,
			      struct udsp_tdm_fail *fail)
{
	snd_pcm_hw_params_t *hw;
	/* The token and the class of the call under way, so the record written
	 * at out names the step that refused (tdm.h Refusal). */
	const char *what = "hw_params";
	bool geometry = false;
	int err = snd_pcm_hw_params_malloc(&hw);
	if (err < 0)
		return tdm_fail_set(fail, dir, false, err, what);

	err = snd_pcm_hw_params_any(pcm, hw);
	if (err < 0)
		goto out;

	snd_pcm_access_t want_access =
		want->layout == UDSP_TDM_NONINTERLEAVED ?
		SND_PCM_ACCESS_RW_NONINTERLEAVED : SND_PCM_ACCESS_RW_INTERLEAVED;
	err = snd_pcm_hw_params_set_access(pcm, hw, want_access);
	if (err < 0) {
		/* Take whichever RW access this direction offers; the plug-in
		 * hides the difference from the engine. */
		snd_pcm_access_t alt =
			want_access == SND_PCM_ACCESS_RW_INTERLEAVED ?
			SND_PCM_ACCESS_RW_NONINTERLEAVED :
			SND_PCM_ACCESS_RW_INTERLEAVED;
		err = snd_pcm_hw_params_set_access(pcm, hw, alt);
		if (err < 0)
			goto out;
	}

	err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
	if (err < 0)
		goto out;

	/* From here the device answers about the geometry it was asked for. */
	geometry = true;
	what = "channels";
	err = snd_pcm_hw_params_set_channels(pcm, hw, want->channels);
	if (err < 0)
		goto out;

	/* The requested line rate, with the contract's zero normalised
	 * (tdm.h). */
	unsigned want_rate = want->rate ? want->rate : 8000;
	unsigned rate = want_rate;

	what = "rate";
	err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL);
	if (err < 0)
		goto out;

	snd_pcm_uframes_t period = want->frames_per_period;
	what = "period";
	err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL);
	if (err < 0)
		goto out;

	unsigned want_periods = want->prefill_periods ? want->prefill_periods : 1;
	unsigned periods = want_periods;

	what = "periods";
	err = snd_pcm_hw_params_set_periods_near(pcm, hw, &periods, NULL);
	if (err < 0)
		goto out;

	/* The mt7986 AFE grants its own period count whatever the request is (4
	 * periods for every requested value), which silently shortens the
	 * engine's prefill. A driver that refuses a period count can still
	 * honor a buffer size, so ask that way before accepting a shallower
	 * ring. A driver that refuses both keeps what it granted, and the engine
	 * reports the clamp. */
	if (periods < want_periods) {
		snd_pcm_uframes_t buffer = period * want_periods;

		(void)snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
	}

	/* alsa-lib runs the software configuration, the mapping and a prepare
	 * inside this call, so its errno does not prove a geometry refusal. */
	geometry = false;
	what = "hw_params";
	err = snd_pcm_hw_params(pcm, hw);
	if (err < 0)
		goto out;

	snd_pcm_access_t access;
	unsigned channels, got_rate;
	snd_pcm_uframes_t buffer;
	err = snd_pcm_hw_params_get_access(hw, &access);
	if (err < 0)
		goto out;
	err = snd_pcm_hw_params_get_channels(hw, &channels);
	if (err < 0)
		goto out;
	err = snd_pcm_hw_params_get_rate(hw, &got_rate, NULL);
	if (err < 0)
		goto out;
	err = snd_pcm_hw_params_get_period_size(hw, &period, NULL);
	if (err < 0)
		goto out;
	err = snd_pcm_hw_params_get_buffer_size(hw, &buffer);
	if (err < 0)
		goto out;

	/* The line rate is fixed at open and every consumer sizes its frame
	 * from it (tdm.h Geometry), so a driver that granted another rate breaks
	 * the tick and is refused here. The open itself never renegotiates:
	 * udsp_open settles the rate. */
	geometry = true;
	what = got_rate != want_rate ? "rate" : "period";
	if (got_rate != want_rate || period == 0) {
		err = -EINVAL;
		goto out;
	}

	got->frames_per_period = (unsigned)period;
	got->channels = channels;
	got->prefill_periods = (unsigned)(buffer / period);
	got->layout = tdm_alsa_layout(access);
	got->rate = got_rate;
	err = 0;
out:
	snd_pcm_hw_params_free(hw);
	return err < 0 ? tdm_fail_set(fail, dir, geometry, err, what) : 0;
}

static int tdm_alsa_no_autostart(snd_pcm_t *pcm)
{
	snd_pcm_sw_params_t *sw;
	int err = snd_pcm_sw_params_malloc(&sw);
	if (err < 0)
		return err;

	err = snd_pcm_sw_params_current(pcm, sw);
	if (err < 0)
		goto out;

	snd_pcm_uframes_t boundary;
	err = snd_pcm_sw_params_get_boundary(sw, &boundary);
	if (err < 0)
		goto out;

	/* The engine prefills playback before start(); a start threshold at the
	 * ring boundary stops ALSA auto-starting on the fill so start() alone
	 * triggers the linked pair (tdm.h Linked start). */
	err = snd_pcm_sw_params_set_start_threshold(pcm, sw, boundary);
	if (err < 0)
		goto out;

	err = snd_pcm_sw_params(pcm, sw);
out:
	snd_pcm_sw_params_free(sw);
	return err;
}

static void tdm_alsa_close(struct udsp_tdm *t)
{
	if (!t)
		return;
	if (t->capture) {
		snd_pcm_unlink(t->capture);
		snd_pcm_close(t->capture);
	}
	if (t->play)
		snd_pcm_close(t->play);
	free(t->rx_scratch);
	free(t->tx_scratch);
	free(t);
}

static struct udsp_tdm *tdm_alsa_open(const char *play_dev,
				      const char *capture_dev,
				      const struct udsp_tdm_geom *want,
				      struct udsp_tdm_geom *got,
				      struct udsp_tdm_fail *fail)
{
	int err;

	if (!play_dev || !capture_dev || !want || !got) {
		tdm_fail_set(fail, UDSP_TDM_DIR_NONE, false, -EINVAL,
			     "arguments");
		return NULL;
	}
	if (want->channels < 1 || want->channels > TDM_MAX_CHANNELS ||
	    want->frames_per_period == 0) {
		tdm_fail_set(fail, UDSP_TDM_DIR_NONE, false, -EINVAL,
			     "geometry request");
		return NULL;
	}

	struct udsp_tdm *t = calloc(1, sizeof(*t));
	if (!t) {
		tdm_fail_set(fail, UDSP_TDM_DIR_NONE, false, -ENOMEM,
			     "allocation");
		return NULL;
	}

	err = snd_pcm_open(&t->play, play_dev, SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) {
		t->play = NULL;
		tdm_fail_set(fail, UDSP_TDM_DIR_PLAYBACK, false, err, "open");
		goto fail;
	}
	err = snd_pcm_open(&t->capture, capture_dev, SND_PCM_STREAM_CAPTURE, 0);
	if (err < 0) {
		t->capture = NULL;
		tdm_fail_set(fail, UDSP_TDM_DIR_CAPTURE, false, err, "open");
		goto fail;
	}

	struct udsp_tdm_geom pg = {0}, cg = {0};
	if (tdm_alsa_configure(t->play, want, &pg, UDSP_TDM_DIR_PLAYBACK,
			       fail) < 0)
		goto fail;
	if (tdm_alsa_configure(t->capture, want, &cg, UDSP_TDM_DIR_CAPTURE,
			       fail) < 0)
		goto fail;

	/* AN7581 shares DCHENR between both DMA rings and rewrites CHBFOSR at
	 * every stream start, so the channel count, the period and the rate
	 * must match in both directions or the second start returns -EBUSY. The
	 * access layout is not part of that constraint. */
	if (pg.channels != cg.channels ||
	    pg.frames_per_period != cg.frames_per_period ||
	    pg.rate != cg.rate) {
		tdm_fail_set(fail, UDSP_TDM_DIR_NONE, true, -EINVAL,
			     "pair geometry");
		goto fail;
	}

	err = tdm_alsa_no_autostart(t->play);
	if (err < 0) {
		tdm_fail_set(fail, UDSP_TDM_DIR_PLAYBACK, false, err,
			     "sw_params");
		goto fail;
	}
	err = snd_pcm_link(t->capture, t->play);
	if (err < 0) {
		tdm_fail_set(fail, UDSP_TDM_DIR_NONE, false, err, "link");
		goto fail;
	}

	t->geom = cg;
	t->play_interleaved = pg.layout == UDSP_TDM_INTERLEAVED;
	t->capture_interleaved = cg.layout == UDSP_TDM_INTERLEAVED;

	unsigned samples = cg.channels * cg.frames_per_period;
	if (t->capture_interleaved) {
		t->rx_scratch = calloc(samples, sizeof(*t->rx_scratch));
		if (!t->rx_scratch) {
			tdm_fail_set(fail, UDSP_TDM_DIR_CAPTURE, false, -ENOMEM,
				     "allocation");
			goto fail;
		}
	}
	if (t->play_interleaved) {
		t->tx_scratch = calloc(samples, sizeof(*t->tx_scratch));
		if (!t->tx_scratch) {
			tdm_fail_set(fail, UDSP_TDM_DIR_PLAYBACK, false, -ENOMEM,
				     "allocation");
			goto fail;
		}
	}

	cg.paced = true;	/* the card paces the tick */
	*got = cg;
	/* Prefill fills the playback ring, so the playback buffer depth is the
	 * bound the engine must clamp to. The capture depth is unrelated, and
	 * reporting it would let the engine block on a write that never
	 * completes. */
	got->prefill_periods = pg.prefill_periods;
	got->capture_periods = cg.prefill_periods;
	return t;
fail:
	tdm_alsa_close(t);
	return NULL;
}

/* The engine recovers from every counted fault, not only from an xrun
 * (engine.c engine_fault), so the linked group may still run here. The kernel
 * refuses a prepare on a running stream with -EBUSY and stops only a paused or
 * suspended one itself (sound/core/pcm_native.c snd_pcm_prepare), so a running
 * group is dropped first. The drop is gated on the state rather than issued
 * always, because a suspended group refuses the drop with -EBADFD while the
 * prepare handles that state on its own. */
static int tdm_alsa_recover(struct udsp_tdm *t)
{
	int err;

	if (snd_pcm_state(t->capture) == SND_PCM_STATE_RUNNING) {
		err = snd_pcm_drop(t->capture);
		if (err < 0)
			return err;
	}
	err = snd_pcm_prepare(t->play);
	if (err < 0)
		return err;
	return snd_pcm_prepare(t->capture);
}

static int tdm_alsa_start(struct udsp_tdm *t)
{
	/* Linked: starting the capture group triggers playback in the same
	 * step so the two clocks share a defined phase (tdm.h). */
	return snd_pcm_start(t->capture);
}

static int tdm_alsa_stop(struct udsp_tdm *t)
{
	return snd_pcm_drop(t->capture);
}

/* Bounded read. snd_pcm_readi blocks until a period arrives, so a stop that
 * arrives while the card is wedged would wait forever. The wait is taken first,
 * with a ceiling well above the worst observed 27.5 ms interval. A timeout
 * returns -EAGAIN, which engine_fault treats as transient and counts no
 * strike, and the loop re-checks its stop flag. */
static int tdm_alsa_wait(struct udsp_tdm *t)
{
	int rc = snd_pcm_wait(t->capture, TDM_ALSA_WAIT_MS);

	if (rc > 0)
		return 0;
	if (!rc)
		return -EAGAIN;

	return rc;
}

static int tdm_alsa_read(struct udsp_tdm *t, int16_t *buf)
{
	snd_pcm_uframes_t frames = t->geom.frames_per_period;
	int rc = tdm_alsa_wait(t);

	if (rc < 0)
		return rc;

	if (t->capture_interleaved) {
		snd_pcm_sframes_t n =
			snd_pcm_readi(t->capture, t->rx_scratch, frames);
		if (n < 0)
			return (int)n;
		if ((snd_pcm_uframes_t)n != frames)
			return -EIO;
		tdm_interleaved_to_planar(buf, t->rx_scratch, t->geom.channels,
					  t->geom.frames_per_period);
		return 0;
	}

	void *chans[TDM_MAX_CHANNELS];
	for (unsigned ch = 0; ch < t->geom.channels; ch++)
		chans[ch] = buf + ch * t->geom.frames_per_period;
	snd_pcm_sframes_t n = snd_pcm_readn(t->capture, chans, frames);
	if (n < 0)
		return (int)n;
	if ((snd_pcm_uframes_t)n != frames)
		return -EIO;
	return 0;
}

static int tdm_alsa_write(struct udsp_tdm *t, const int16_t *buf)
{
	snd_pcm_uframes_t frames = t->geom.frames_per_period;

	if (t->play_interleaved) {
		tdm_planar_to_interleaved(t->tx_scratch, buf, t->geom.channels,
					  t->geom.frames_per_period);
		snd_pcm_sframes_t n =
			snd_pcm_writei(t->play, t->tx_scratch, frames);
		if (n < 0)
			return (int)n;
		if ((snd_pcm_uframes_t)n != frames)
			return -EIO;
		return 0;
	}

	void *chans[TDM_MAX_CHANNELS];
	for (unsigned ch = 0; ch < t->geom.channels; ch++)
		chans[ch] = (void *)(uintptr_t)(buf +
						ch * t->geom.frames_per_period);
	snd_pcm_sframes_t n = snd_pcm_writen(t->play, chans, frames);
	if (n < 0)
		return (int)n;
	if ((snd_pcm_uframes_t)n != frames)
		return -EIO;
	return 0;
}

/* The direct capture backlog: frames the driver already holds for the next
 * read. */
static long tdm_alsa_avail(struct udsp_tdm *t)
{
	snd_pcm_sframes_t n = snd_pcm_avail_update(t->capture);

	if (n < 0)
		return (long)n;
	return (long)n;
}

static long tdm_alsa_delay(struct udsp_tdm *t)
{
	snd_pcm_sframes_t d;
	int err = snd_pcm_delay(t->play, &d);
	if (err < 0)
		return err;

	/* After an underrun the driver can report a negative delay; the
	 * contract reserves a negative return for an errno, so a drained ring
	 * reports zero buffered frames. */
	if (d < 0)
		return 0;
	return (long)d;
}

const struct udsp_tdm_ops udsp_tdm_alsa = {
	.name = "alsa",
	.open = tdm_alsa_open,
	.close = tdm_alsa_close,
	.recover = tdm_alsa_recover,
	.start = tdm_alsa_start,
	.stop = tdm_alsa_stop,
	.read_period = tdm_alsa_read,
	.write_period = tdm_alsa_write,
	.delay = tdm_alsa_delay,
	.avail = tdm_alsa_avail,
};

#else /* HAVE_ALSA */

/* Keep the translation unit non-empty when alsa-lib is absent; the engine only
 * references udsp_tdm_alsa on an ALSA build. */
typedef int udsp_tdm_alsa_disabled;

#endif /* HAVE_ALSA */
