// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * css HAL backend for the Snom D735 (DSP Group DVF99). The CSS co-processor
 * runs the media plane behind /dev/css-voice; this file is the hal_ops table on
 * top of it, with the socket ownership, the ALSA tone and audio plane, the LED
 * sysfs and the evdev inputs of a desk phone. The approved spec is
 * /src/snom/UVOICE-D735.md sections 2, 4, 5 and 8; the voice ABI is
 * dvf99_voice.h.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <libubox/utils.h>

#include "css_int.h"

#include "../../posix.h"
#include "../../str.h"
#include "../../trace.h"

const char *css_rproc_state_path = CSS_RPROC_STATE;
const char *css_firmware_path = CSS_FIRMWARE;
const char *css_voice_dev_path = CSS_VOICE_DEV;

/* --- the device behind an ops table --------------------------------------- */

static int css_dev_open(void *ctx, unsigned int line)
{
	return open(css_voice_dev_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
}

static int css_dev_ioctl(void *ctx, int fd, unsigned long req, void *arg)
{
	return ioctl(fd, req, arg);
}

static long css_dev_read(void *ctx, int fd, void *buf, unsigned long len)
{
	return read(fd, buf, len);
}

static long css_dev_write(void *ctx, int fd, const void *buf, unsigned long len)
{
	return write(fd, buf, len);
}

static void css_dev_close(void *ctx, int fd)
{
	if (fd >= 0)
		close(fd);
}

static const struct css_devops css_real = {
	.open = css_dev_open,
	.xioctl = css_dev_ioctl,
	.xread = css_dev_read,
	.xwrite = css_dev_write,
	.xclose = css_dev_close,
	.ctx = NULL,
};

const struct css_devops *css_devops_real(void)
{
	return &css_real;
}

/* --- the hardware-event emitter ------------------------------------------- */

void css_event_emit(struct hal *h, struct hal_event *ev)
{
	ssize_t n;
	uint16_t seq;

	pthread_mutex_lock(&h->emit_lock);
	seq = ++h->emit_seq;
	if (!seq)			/* seq counts from 1 and skips 0 on wrap */
		seq = ++h->emit_seq;
	ev->seq = seq;
	ev->stamp_ms = uvoice_mono_ms();
	n = send(h->relay.lib_fd, ev, sizeof(*ev), MSG_DONTWAIT | MSG_NOSIGNAL);
	/* A hardware event is dropped on a full fd and counted (hal.h section 2
	 * (a)); the gap the core sees in seq is the fault indicator. The counter
	 * stays under the lock, so no 64-bit atomic is needed. */
	if (n != (ssize_t)sizeof(*ev))
		h->emit_dropped++;
	pthread_mutex_unlock(&h->emit_lock);
}

/* --- lifecycle ------------------------------------------------------------ */

/* The CSS firmware runs on remoteproc0. "running" in the state file means the
 * co-processor is up and /dev/css-voice is served. */
static bool css_rproc_running(void)
{
	char buf[32];
	int fd;
	ssize_t n;

	fd = open(css_rproc_state_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;

	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return false;

	buf[n] = 0;

	return strstr(buf, "running") != NULL;
}

static int css_rproc_start(void)
{
	int fd = open(css_rproc_state_path, O_WRONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -1;

	n = write(fd, "start\n", 6);
	close(fd);

	return n == 6 ? 0 : -1;
}

static struct hal *css_open(int event_fd)
{
	struct hal *h = calloc(1, sizeof(*h));
	unsigned int line, stream;

	if (!h)
		return NULL;

	h->dev = css_devops_real();
	h->start_poll.cb = NULL;
	for (line = 0; line < CSS_LINES; line++)
		for (stream = 0; stream < CSS_STREAMS_PER_CHAN; stream++) {
			struct css_stream *s = &h->stream[line][stream];

			hal_media_sock_init(&s->sock);
			s->dev_fd = -1;
		}

	snprintf(h->cap.backend, sizeof(h->cap.backend), "css");
	str_copy(h->cap.version, sizeof(h->cap.version), UVOICE_VERSION);
	snprintf(h->cap.slic, sizeof(h->cap.slic), "none");
	h->cap.lines = CSS_LINES;
	h->cap.streams_per_chan = CSS_STREAMS_PER_CHAN;
	h->cap.features = HAL_FEAT_DESK | HAL_FEAT_CONF | HAL_FEAT_SRTP |
			  HAL_FEAT_WIDEBAND | HAL_FEAT_G729 | HAL_FEAT_G726 |
			  HAL_FEAT_IPV6_MEDIA | HAL_FEAT_RTCP_ADDR;
	h->cap.codecs = (1u << HAL_CODEC_PCMU) | (1u << HAL_CODEC_PCMA) |
			(1u << HAL_CODEC_G722) | (1u << HAL_CODEC_G729) |
			(1u << HAL_CODEC_G726_32);

	/* The hal_relay drains the packet thread's lib_fd and carries the
	 * backend-posted completions (dsp_start/stop, stats). The css backend
	 * has no bring-up chain, so it registers no intercept. */
	if (hal_relay_open(&h->relay, event_fd, "css", NULL, h)) {
		ULOG_ERR("css: event relay: %s\n", strerror(errno));
		free(h);
		return NULL;
	}

	/* The emitter must be ready before the input plane, which reports the
	 * hook state at open through css_event_emit. The three planes are
	 * conveniences: a failure to open one costs that function, not the
	 * voice path, so it logs and the open continues. */
	pthread_mutex_init(&h->emit_lock, NULL);
	if (css_pkt_start(h))
		ULOG_ERR("css: the packet thread did not start; no media flows\n");
	if (css_input_open(h))
		ULOG_ERR("css: the input plane did not open; keys are absent\n");
	if (css_audio_open(h))
		ULOG_ERR("css: the audio plane did not open; the mixer is absent\n");
	if (css_tone_open(h))
		ULOG_ERR("css: the tone plane did not open; tones are absent\n");

	h->start_poll.cb = NULL;
	ULOG_INFO("css hal attached: %u lines, %u streams per channel\n",
		  h->cap.lines, h->cap.streams_per_chan);

	return h;
}

static void css_close(struct hal *h)
{
	unsigned int line, stream;

	if (!h)
		return;

	uloop_timeout_cancel(&h->start_poll);
	/* Release every stream: the CSS stop, the device and the media sockets.
	 * Nothing else closes them, so a leak here keeps the local ports bound
	 * for the life of the process. */
	for (line = 0; line < CSS_LINES; line++)
		for (stream = 0; stream < CSS_STREAMS_PER_CHAN; stream++)
			css_stream_stop(h, line, stream);
	css_pkt_stop(h);
	css_tone_close(h);
	css_audio_close(h);
	css_input_close(h);
	hal_relay_close(&h->relay);
	pthread_mutex_destroy(&h->emit_lock);
	free(h);
}

static enum hal_status css_tune_set(struct hal *h,
				    const struct hal_audio_tune *tune)
{
	enum hal_status st;

	st = css_tone_ring_sound_set(h, tune->ring_sound);
	if (st != HAL_OK)
		return st;

	return css_audio_tune_set(h, tune);
}

static const struct hal_cap *css_cap_get(struct hal *h)
{
	return &h->cap;
}

static void css_counters_get(struct hal *h, struct hal_counters *out)
{
	uint64_t emit_dropped;

	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&h->emit_lock);
	emit_dropped = h->emit_dropped;
	pthread_mutex_unlock(&h->emit_lock);
	out->events_dropped = h->relay.dropped + emit_dropped;
	out->completions_lost = h->relay.comp_lost;
}

/*
 * dsp_start (SLOW). The CSS carries the whole media plane, so the daemon cannot
 * serve a call until the co-processor runs and /dev/css-voice answers. If the
 * core is already running the start completes at once; if it is offline and the
 * firmware is present the backend asks remoteproc to load it and polls for the
 * device node; with no firmware it fails with a clear line.
 */
static void css_start_poll(struct uloop_timeout *t)
{
	struct hal *h = container_of(t, struct hal, start_poll);

	if (css_rproc_running() && access(css_voice_dev_path, F_OK) == 0) {
		h->start_armed = false;
		hal_relay_completion_post(&h->relay, h->start_token, HAL_OK);
		return;
	}

	if (++h->start_tries >= CSS_START_POLL_MAX) {
		h->start_armed = false;
		ULOG_ERR("css: the CSS did not come up %d ms after start\n",
			 CSS_START_POLL_MS * CSS_START_POLL_MAX);
		hal_relay_completion_post(&h->relay, h->start_token, HAL_FATAL);
		return;
	}

	uloop_timeout_set(&h->start_poll, CSS_START_POLL_MS);
}

static enum hal_status css_dsp_start(struct hal *h, struct hal_job *job)
{
	if (!job)
		return HAL_FATAL;

	/* A backend-completed SLOW op: the token comes from the relay's reserved
	 * high half, so it never collides with another producer. */
	job->token = hal_relay_stats_token_next(&h->relay);

	if (css_rproc_running()) {
		hal_relay_completion_post(&h->relay, job->token, HAL_OK);
		return HAL_OK;
	}

	if (access(css_firmware_path, F_OK) != 0) {
		ULOG_ERR("css: the CSS is offline and %s is absent, no voice "
			 "hardware\n", css_firmware_path);
		return HAL_FATAL;
	}

	if (css_rproc_start()) {
		ULOG_ERR("css: could not start the CSS through %s: %s\n",
			 css_rproc_state_path, strerror(errno));
		return HAL_FATAL;
	}

	h->start_token = job->token;
	h->start_tries = 0;
	h->start_armed = true;
	h->start_poll.cb = css_start_poll;
	uloop_timeout_set(&h->start_poll, CSS_START_POLL_MS);

	return HAL_OK;
}

static enum hal_status css_dsp_stop(struct hal *h, struct hal_job *job)
{
	if (!job)
		return HAL_FATAL;

	job->token = hal_relay_stats_token_next(&h->relay);
	/* The streams are stopped by the media path (added in a later change);
	 * the CSS itself stays running, so a later call needs no reload. */
	hal_relay_completion_post(&h->relay, job->token, HAL_OK);

	return HAL_OK;
}

/* Refused here, not in a plane file: the CSS carries no T.38 or modem
 * passthrough, and the call-waiting tone is not synthesised yet. They answer
 * rather than stay NULL, because the core calls both unconditionally. */
static enum hal_status css_fax_profile_apply(struct hal *h, unsigned int line,
					     unsigned int stream,
					     const struct hal_fax_profile *fax)
{
	return HAL_UNSUPPORTED;
}

static enum hal_status css_cw_tone_play(struct hal *h, unsigned int line,
					unsigned int variant,
					uint32_t duration_ms)
{
	return HAL_UNSUPPORTED;
}

const struct hal_ops hal_backend_ops = {
	.name = "css",
	.open = css_open,
	.close = css_close,
	.cap_get = css_cap_get,
	.counters_get = css_counters_get,
	.dsp_start = css_dsp_start,
	.dsp_stop = css_dsp_stop,
	/* The line plane (css_line.c): answered or refused, never NULL, because
	 * the core calls these unconditionally. */
	.line_state_set = css_line_state_set,
	.line_polarity_set = css_line_polarity_set,
	.line_hook_timing_set = css_line_hook_timing_set,
	.line_cid_send = css_line_cid_send,
	.line_mwi_set = css_line_mwi_set,
	.line_test_start = css_line_test_start,
	.line_test_result_get = css_line_test_result_get,
	.channel_config_set = css_channel_config_set,
	.debug_set = css_debug_set,
	.fax_profile_apply = css_fax_profile_apply,
	.cw_tone_play = css_cw_tone_play,
	/* The input plane (css_input.c). */
	.line_hook_query = css_input_hook_query,
	/* The audio plane (css_audio.c). */
	.audio_path_set = css_audio_path_set,
	.volume_set = css_volume_set,
	.mute_set = css_mute_set,
	.led_set = css_led_set,
	.audio_tune_set = css_tune_set,
	/* The tone plane (css_tone.c). */
	.tone_play = css_tone_play,
	.tone_stop = css_tone_stop,
	.digit_play = css_digit_play,
	.line_ring_start = css_ring_start,
	.line_ring_stop = css_ring_stop,
	.region_apply = css_region_apply,
	/* The media plane (css_media.c). */
	.stream_config_set = css_stream_config_set,
	.stream_start = css_stream_start,
	.stream_stop = css_stream_stop,
	.stream_dtmf_send = css_stream_dtmf_send,
	.stream_stats_get = css_stream_stats_get,
	.stream_stats_start = css_stream_stats_start,
	.stream_stats_result_get = css_stream_stats_result_get,
	.conf_set = css_conf_set,
};
