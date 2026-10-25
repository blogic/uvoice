// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Private contract of the css HAL backend for the Snom D735 (DSP Group DVF99).
 * Nothing here leaves the backend: the core sees struct hal_ops and an opaque
 * struct hal only.
 *
 * The CSS co-processor runs the media plane (RTP/RTCP, codecs, jitter buffer,
 * RFC 2833, mixing) behind /dev/css-voice, one open per stream. This backend
 * owns the UDP sockets (hal.h rule 1), protects and unprotects them with
 * libuvsrtp, and shuttles datagrams between the sockets and the device on one
 * packet thread. Tones, the ringer and the DTMF key sounds are synthesised here
 * and played through ALSA; the analogue path, the receive volume, the
 * microphone mute and the LEDs are ALSA mixer and sysfs writes. The keypad and
 * the hook switch are evdev inputs forwarded to the core as HAL events.
 *
 * Threads: the uloop thread runs every op and the input and relay callbacks;
 * one packet thread owns the media sockets and the device fds. The packet
 * thread only ever writes records into the relay's lib_fd (hal_relay.h); it
 * calls no op.
 */
#ifndef UVOICE_HAL_CSS_INT_H
#define UVOICE_HAL_CSS_INT_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <libubox/uloop.h>

#include "../hal.h"
#include "../hal_media.h"
#include "../hal_relay.h"

#include "dvf99_voice.h"

/* One desk phone: two lines, two streams each (a call and its consult). */
#define CSS_LINES		2
#define CSS_STREAMS_PER_CHAN	2

/* The device the CSS voice service exposes, one open per stream. */
#define CSS_VOICE_DEV		"/dev/css-voice"
/* The remoteproc the CSS firmware runs on, and the firmware the loader wants.
 * dsp_start reads the state and, when the core is offline, asks for a load. */
#define CSS_RPROC_STATE		"/sys/class/remoteproc/remoteproc0/state"
#define CSS_FIRMWARE		"/lib/firmware/css-loader"
/* How long dsp_start waits for the device node after it asked for a load, in
 * one poll per step. */
#define CSS_START_POLL_MS	200
#define CSS_START_POLL_MAX	50

/*
 * The device behind an ops table, so a socketpair fake can stand in for
 * /dev/css-voice in test_hal_css. The real table calls open/ioctl/read/write/
 * close on the character device; the fake drives the record protocol over a
 * pair. A backend holds one table for the life of the open.
 */
struct css_devops {
	int (*open)(void *ctx, unsigned int line);
	int (*xioctl)(void *ctx, int fd, unsigned long req, void *arg);
	long (*xread)(void *ctx, int fd, void *buf, unsigned long len);
	long (*xwrite)(void *ctx, int fd, const void *buf, unsigned long len);
	void (*xclose)(void *ctx, int fd);
	void *ctx;
};

const struct css_devops *css_devops_real(void);

/* One media stream: its device fd, the backend's socket pair and the SRTP
 * context. The CSS carries the RTP/RTCP engine behind dev_fd, one open per
 * stream; the backend owns the UDP sockets and protects them with libuvsrtp.
 * cfg is the last config handed to the device, kept for DVF99_VOICE_UPDATE and
 * for the start that follows a config. */
struct css_stream {
	bool configured;
	bool running;
	int dev_fd;
	struct hal_media_sock sock;
	struct uvsrtp *srtp;
	bool srtp_live;
	struct dvf99_voice_config cfg;
	struct hal_media_stats stats;
	_Atomic uint32_t jit;
	int32_t jit_transit;
	uint32_t jit_ssrc;
	bool jit_have;
};

/* The stream ops, defined in css_media.c and placed in hal_backend_ops by
 * css.c. The packet thread that carries the datagrams is added with them. */
enum hal_status css_stream_config_set(struct hal *h, unsigned int line,
				      unsigned int stream,
				      const struct hal_media *media);
enum hal_status css_stream_start(struct hal *h, unsigned int line,
				 unsigned int stream);
enum hal_status css_stream_stop(struct hal *h, unsigned int line,
				unsigned int stream);
enum hal_status css_stream_dtmf_send(struct hal *h, unsigned int line,
				     unsigned int stream, char digit,
				     uint32_t duration_ms);
enum hal_status css_stream_stats_get(struct hal *h, unsigned int line,
				     unsigned int stream,
				     struct hal_media_stats *out);
enum hal_status css_stream_stats_start(struct hal *h, unsigned int line,
				       unsigned int stream, struct hal_job *job);
enum hal_status css_stream_stats_result_get(struct hal *h, unsigned int line,
					    unsigned int stream,
					    struct hal_media_stats *out);
enum hal_status css_conf_set(struct hal *h, unsigned int line,
			     enum hal_conf_mode mode);

/* The media packet thread (css_media.c), started after the relay in css_open
 * and stopped before the streams are released in css_close. */
int css_pkt_start(struct hal *h);
void css_pkt_stop(struct hal *h);

void css_rtp_jitter_update(struct css_stream *s, uint32_t rtp_ts,
			   uint32_t arrival_units);

/* The line plane (css_line.c): the hook timing and the line-level ops a desk
 * phone answers or refuses so no core path dereferences a NULL op. */
enum hal_status css_line_state_set(struct hal *h, unsigned int line,
				   enum hal_line_state state);
enum hal_status css_line_polarity_set(struct hal *h, unsigned int line,
				      enum hal_polarity pol);
enum hal_status css_line_hook_timing_set(struct hal *h, unsigned int line,
					 const struct hal_hook_timing *ts);
enum hal_status css_line_cid_send(struct hal *h, unsigned int line,
				  enum hal_cid_kind kind,
				  const struct hal_cid *cid);
enum hal_status css_line_mwi_set(struct hal *h, unsigned int line,
				 enum hal_mwi mwi);
enum hal_status css_line_test_start(struct hal *h, unsigned int line,
				    enum hal_line_test test, struct hal_job *job);
enum hal_status css_line_test_result_get(struct hal *h, unsigned int line,
					 void *buf, size_t buf_len,
					 size_t *out_len);
enum hal_status css_channel_config_set(struct hal *h, unsigned int line,
				       const struct hal_channel_cfg *cfg);
enum hal_status css_debug_set(struct hal *h, uint32_t flags);

/* line, stream -> the UT_VOIP instance and the device line index (spec 8.3). */
static inline unsigned int css_dev_line(unsigned int line, unsigned int stream)
{
	return CSS_STREAMS_PER_CHAN * line + stream;
}

/*
 * The input, audio and tone planes live in their own files (css_input.c,
 * css_audio.c, css_tone.c). Their state is opaque to this file: each allocates
 * its own object at open and clears the pointer at close.
 */
struct css_input;
struct css_audio;
struct css_tone;

struct hal {
	struct hal_cap cap;
	struct hal_relay relay;
	const struct css_devops *dev;

	struct css_stream stream[CSS_LINES][CSS_STREAMS_PER_CHAN];

	struct css_input *input;
	struct css_audio *audio;
	struct css_tone *tone;

	/*
	 * The single hardware-event emitter (css_event_emit). The packet thread
	 * and the uloop thread both post through it, so a lock keeps the seq
	 * order on the wire (hal.h section 2). emit_seq counts from 1;
	 * emit_dropped counts the records a full relay fd refused.
	 */
	pthread_mutex_t emit_lock;
	uint16_t emit_seq;
	/* Written and read under emit_lock, not an atomic: arm926ej-s (ARMv5)
	 * has no 64-bit atomic and would pull in libatomic. */
	uint64_t emit_dropped;

	/*
	 * The media packet thread. It polls every running stream's device fd
	 * and media sockets, protects datagrams from the CSS and unprotects
	 * datagrams from the wire with libuvsrtp, and turns the device's DTMF
	 * and event records into HAL events. The uloop thread clears a stream's
	 * running flag under pkt_lock, wakes the thread through pkt_wake and
	 * waits on pkt_cond until the thread has rebuilt its poll set (pkt_acked
	 * reaches pkt_gen), so no descriptor is closed while the thread polls it.
	 */
	pthread_t pkt_tid;
	bool pkt_alive;
	bool pkt_quit;
	int pkt_wake[2];
	pthread_mutex_t pkt_lock;
	pthread_cond_t pkt_cond;
	uint32_t pkt_gen;
	uint32_t pkt_acked;

	/* The configured region, resolved by region_apply; picks the tone set. */
	char region[HAL_REGION_ISO3_MAX];

	/* The bring-up poll: dsp_start asked for a load and waits for the device
	 * node, posting the completion when it appears or the budget runs out. */
	struct uloop_timeout start_poll;
	uint32_t start_token;
	unsigned int start_tries;
	bool start_armed;
};

/*
 * The one emitter for a hardware event (hal.h section 2). It stamps ev->seq
 * from a per-backend counter starting at 1, stamps ev->stamp_ms, and writes the
 * record into the relay under emit_lock so the two producer threads keep the
 * seq order. On a full relay fd the record is dropped and counted; it is never
 * retried. Both the packet thread and the input plane use it.
 */
void css_event_emit(struct hal *h, struct hal_event *ev);

/* The input plane (css_input.c): evdev hook and keys forwarded as HAL events. */
int css_input_open(struct hal *h);
void css_input_close(struct hal *h);
enum hal_status css_input_hook_query(struct hal *h, unsigned int line,
				     bool *offhook);

/* The audio plane (css_audio.c): the analogue path, volume, mute and LEDs. */
int css_audio_open(struct hal *h);
void css_audio_close(struct hal *h);
enum hal_status css_audio_path_set(struct hal *h, enum hal_audio_path path);
enum hal_status css_audio_tune_set(struct hal *h,
				   const struct hal_audio_tune *tune);
enum hal_status css_volume_set(struct hal *h, unsigned int level);
enum hal_status css_mute_set(struct hal *h, bool mute);
enum hal_status css_led_set(struct hal *h, enum hal_led led,
			    enum hal_led_mode mode);
/* Routes the ringer onto the loudspeaker while a ring plays (css_tone.c uses
 * it around a ring). */
void css_audio_ring_route(struct hal *h, bool on);

/* The tone plane (css_tone.c): synthesised tones, DTMF key sounds and the
 * ringer, played through ALSA, and the region tone-set selection. */
int css_tone_open(struct hal *h);
void css_tone_close(struct hal *h);
enum hal_status css_tone_play(struct hal *h, unsigned int line,
			      enum hal_tone tone, uint32_t duration_ms);
enum hal_status css_tone_stop(struct hal *h, unsigned int line);
enum hal_status css_digit_play(struct hal *h, unsigned int line, char digit,
			       uint32_t duration_ms);
enum hal_status css_ring_start(struct hal *h, unsigned int line,
			       const struct hal_ring *ring);
enum hal_status css_ring_stop(struct hal *h, unsigned int line);
enum hal_status css_tone_ring_sound_set(struct hal *h,
					enum hal_ring_sound sound);
enum hal_status css_region_apply(struct hal *h, const char *iso3,
				 struct hal_job *job);

/* Overridable in the suites, defined in the plane files. */
extern const char *css_input_dir;
extern const char *css_card_name;
extern const char *css_led_dir;
extern const char *css_pcm_name;

/* Overridable in test_hal_css so the lifecycle runs without the real sysfs. */
extern const char *css_rproc_state_path;
extern const char *css_firmware_path;
extern const char *css_voice_dev_path;

#endif
