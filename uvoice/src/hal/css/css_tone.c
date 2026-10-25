// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <alsa/asoundlib.h>
#include <libubox/ulog.h>
#include <libubox/utils.h>

#include "css_int.h"

#define CSS_PCM			"hw:dvf99css,1"
#define CSS_RATE		16000
#define CSS_PERIOD		320
#define CSS_PERIODS		4
#define CSS_SEGS		12
#define CSS_SINE_BITS		8
#define CSS_SINE_LEN		(1 << CSS_SINE_BITS)
#define CSS_PHASE_SHIFT		(32 - CSS_SINE_BITS)
#define CSS_IDLE_CLOSE_MS	2000
#define CSS_LEVEL_TONE		6000
#define CSS_LEVEL_DIGIT		5000
#define CSS_LEVEL_RING		9000
#define CSS_RING_DEFAULT_ON_MS	1000
#define CSS_RING_DEFAULT_OFF_MS	4000
#define CSS_DIGIT_MS		100
#define CSS_FOREVER		0

const char *css_pcm_name = CSS_PCM;

enum css_slot {
	CSS_SLOT_TONE = 0,
	CSS_SLOT_DIGIT,
	CSS_SLOT_RING,
	CSS_SLOTS,
};

struct css_seg {
	uint16_t f1;
	uint16_t f2;
	uint16_t ms;
	uint16_t warble_ms;
	bool silent;
};

struct css_prog {
	struct css_seg seg[CSS_SEGS];
	unsigned int count;
	unsigned int loop_from;
	bool loop;
	int16_t level;
	uint32_t limit_ms;
};

struct css_voice {
	bool active;
	struct css_prog prog;
	unsigned int seg;
	uint32_t seg_pos;
	uint32_t elapsed;
	uint32_t ph1;
	uint32_t ph2;
};

struct css_tone {
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool run;
	bool started;
	struct css_voice voice[CSS_SLOTS];
	struct hal *h;
	enum hal_ring_sound ring_sound;
	bool usa;
};

static int16_t css_sine[CSS_SINE_LEN];

static const struct {
	uint16_t f1;
	uint16_t f2;
	uint16_t warble_ms;
} css_ring_sounds[__HAL_RING_SOUND_MAX] = {
	[HAL_RING_CLASSIC] = { 1000, 1250, 50 },
	[HAL_RING_HIGH] = { 1400, 1800, 50 },
	[HAL_RING_LOW] = { 500, 650, 50 },
	[HAL_RING_FAST] = { 1000, 1250, 25 },
	[HAL_RING_SLOW] = { 800, 1000, 100 },
	[HAL_RING_DUAL] = { 440, 480, 0 },
};

static const struct {
	char digit;
	uint16_t low;
	uint16_t high;
} css_dtmf[] = {
	{ '1', 697, 1209 }, { '2', 697, 1336 }, { '3', 697, 1477 },
	{ 'A', 697, 1633 }, { '4', 770, 1209 }, { '5', 770, 1336 },
	{ '6', 770, 1477 }, { 'B', 770, 1633 }, { '7', 852, 1209 },
	{ '8', 852, 1336 }, { '9', 852, 1477 }, { 'C', 852, 1633 },
	{ '*', 941, 1209 }, { '0', 941, 1336 }, { '#', 941, 1477 },
	{ 'D', 941, 1633 },
};

static void sine_init(void)
{
	static const int16_t quarter[CSS_SINE_LEN / 4 + 1] = {
		0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179,
		7962, 8739, 9512, 10278, 11039, 11793, 12539, 13279, 14010,
		14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519,
		20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279,
		24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105,
		28510, 28898, 29268, 29621, 29956, 30273, 30571, 30852,
		31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412,
		32521, 32609, 32678, 32728, 32757, 32767,
	};
	unsigned int i, q = CSS_SINE_LEN / 4;

	for (i = 0; i < CSS_SINE_LEN; i++) {
		unsigned int k = i % (2 * q);
		int16_t v = k <= q ? quarter[k] : quarter[2 * q - k];

		css_sine[i] = i < 2 * q ? v : (int16_t)-v;
	}
}

static uint32_t phase_step(uint16_t hz)
{
	return (uint32_t)(((uint64_t)hz << 32) / CSS_RATE);
}

static int32_t voice_sample(struct css_voice *v)
{
	const struct css_seg *s = &v->prog.seg[v->seg];
	int32_t out = 0;

	if (!s->silent && s->warble_ms) {
		bool second = (v->seg_pos / (s->warble_ms *
					     (CSS_RATE / 1000))) & 1;

		out = css_sine[v->ph1 >> CSS_PHASE_SHIFT];
		v->ph1 += phase_step(second ? s->f2 : s->f1);
		out = out * v->prog.level / 32768;
	} else if (!s->silent) {
		if (s->f1) {
			out += css_sine[v->ph1 >> CSS_PHASE_SHIFT];
			v->ph1 += phase_step(s->f1);
		}
		if (s->f2) {
			out += css_sine[v->ph2 >> CSS_PHASE_SHIFT];
			v->ph2 += phase_step(s->f2);
		}
		out = out * v->prog.level / (s->f2 ? 2 * 32768 : 32768);
	}

	return out;
}

static void voice_advance(struct css_voice *v, uint32_t samples)
{
	uint32_t seg_samples;

	v->seg_pos += samples;
	v->elapsed += samples;

	if (v->prog.limit_ms &&
	    v->elapsed >= v->prog.limit_ms * (CSS_RATE / 1000)) {
		v->active = false;
		return;
	}

	seg_samples = v->prog.seg[v->seg].ms * (CSS_RATE / 1000);
	if (!v->prog.seg[v->seg].ms || v->seg_pos < seg_samples)
		return;

	v->seg_pos = 0;
	if (++v->seg < v->prog.count)
		return;

	if (!v->prog.loop) {
		v->active = false;
		return;
	}

	v->seg = v->prog.loop_from;
}

static bool tone_fill(struct css_tone *t, int16_t *buf)
{
	unsigned int i, s;
	bool any = false;

	for (s = 0; s < CSS_SLOTS; s++)
		any |= t->voice[s].active;
	if (!any)
		return false;

	for (i = 0; i < CSS_PERIOD; i++) {
		int32_t sum = 0;

		for (s = 0; s < CSS_SLOTS; s++) {
			if (!t->voice[s].active)
				continue;
			sum += voice_sample(&t->voice[s]);
			voice_advance(&t->voice[s], 1);
		}

		if (sum > INT16_MAX)
			sum = INT16_MAX;
		if (sum < INT16_MIN)
			sum = INT16_MIN;
		buf[i] = (int16_t)sum;
	}

	return true;
}

static snd_pcm_t *pcm_open(void)
{
	snd_pcm_t *pcm;
	int ret;

	ret = snd_pcm_open(&pcm, css_pcm_name, SND_PCM_STREAM_PLAYBACK, 0);
	if (ret < 0) {
		ULOG_ERR("css: %s: %s\n", css_pcm_name, snd_strerror(ret));
		return NULL;
	}

	ret = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
				 SND_PCM_ACCESS_RW_INTERLEAVED, 1, CSS_RATE, 0,
				 CSS_PERIOD * CSS_PERIODS * 1000000ULL / CSS_RATE);
	if (ret < 0) {
		ULOG_ERR("css: %s params: %s\n", css_pcm_name,
			 snd_strerror(ret));
		snd_pcm_close(pcm);
		return NULL;
	}

	return pcm;
}

static void *tone_thread(void *arg)
{
	struct css_tone *t = arg;
	int16_t buf[CSS_PERIOD];
	snd_pcm_t *pcm = NULL;
	unsigned int idle = 0;
	snd_pcm_sframes_t n;
	bool have;

	pthread_mutex_lock(&t->lock);
	while (t->run) {
		have = tone_fill(t, buf);

		if (!have && !pcm) {
			pthread_cond_wait(&t->cond, &t->lock);
			continue;
		}

		if (!have) {
			memset(buf, 0, sizeof(buf));
			idle += CSS_PERIOD * 1000 / CSS_RATE;
		} else {
			idle = 0;
		}

		pthread_mutex_unlock(&t->lock);

		if (!pcm)
			pcm = pcm_open();

		if (pcm && idle >= CSS_IDLE_CLOSE_MS) {
			snd_pcm_drop(pcm);
			snd_pcm_close(pcm);
			pcm = NULL;
			idle = 0;
		} else if (pcm) {
			n = snd_pcm_writei(pcm, buf, CSS_PERIOD);
			if (n < 0)
				snd_pcm_recover(pcm, (int)n, 1);
		} else {
			struct timespec ts = { 0, 100 * 1000 * 1000 };

			nanosleep(&ts, NULL);
		}

		pthread_mutex_lock(&t->lock);
	}
	pthread_mutex_unlock(&t->lock);

	if (pcm) {
		snd_pcm_drop(pcm);
		snd_pcm_close(pcm);
	}

	return NULL;
}

static void voice_set(struct css_tone *t, enum css_slot slot,
		      const struct css_prog *prog)
{
	struct css_voice *v = &t->voice[slot];

	pthread_mutex_lock(&t->lock);
	memset(v, 0, sizeof(*v));
	if (prog) {
		v->prog = *prog;
		v->active = prog->count > 0;
	}
	pthread_cond_signal(&t->cond);
	pthread_mutex_unlock(&t->lock);
}

static void prog_add(struct css_prog *p, uint16_t f1, uint16_t f2, uint16_t ms)
{
	if (p->count >= CSS_SEGS)
		return;

	p->seg[p->count++] = (struct css_seg){ f1, f2, ms, 0, !f1 && !f2 };
}

static bool tone_prog_etsi(enum hal_tone tone, struct css_prog *p)
{
	unsigned int i;

	switch (tone) {
	case HAL_TONE_DIAL:
	case HAL_TONE_SECOND_DIAL:
		prog_add(p, 425, 0, 0);
		return true;
	case HAL_TONE_STUTTER:
	case HAL_TONE_SHORT_STUTTER_DIAL:
		for (i = 0; i < 3; i++) {
			prog_add(p, 425, 0, 100);
			prog_add(p, 0, 0, 100);
		}
		prog_add(p, 425, 0, 0);
		return true;
	case HAL_TONE_RINGBACK:
		prog_add(p, 425, 0, 1000);
		prog_add(p, 0, 0, 4000);
		p->loop = true;
		return true;
	case HAL_TONE_BUSY:
		prog_add(p, 425, 0, 480);
		prog_add(p, 0, 0, 480);
		p->loop = true;
		return true;
	case HAL_TONE_REORDER:
	case HAL_TONE_NUMBER_UNOBTAINABLE:
		prog_add(p, 425, 0, 240);
		prog_add(p, 0, 0, 240);
		p->loop = true;
		return true;
	case HAL_TONE_SIT:
		prog_add(p, 950, 0, 330);
		prog_add(p, 1400, 0, 330);
		prog_add(p, 1800, 0, 330);
		prog_add(p, 0, 0, 1000);
		p->loop = true;
		return true;
	case HAL_TONE_CALL_WAITING:
		prog_add(p, 425, 0, 200);
		prog_add(p, 0, 0, 200);
		prog_add(p, 425, 0, 200);
		prog_add(p, 0, 0, 5000);
		p->loop = true;
		return true;
	case HAL_TONE_CONFIRM:
		prog_add(p, 425, 0, 100);
		prog_add(p, 0, 0, 100);
		prog_add(p, 425, 0, 100);
		return true;
	case HAL_TONE_HOWLER:
		prog_add(p, 1400, 2060, 100);
		prog_add(p, 0, 0, 100);
		p->loop = true;
		return true;
	default:
		return false;
	}
}

static bool tone_prog_usa(enum hal_tone tone, struct css_prog *p)
{
	unsigned int i;

	switch (tone) {
	case HAL_TONE_DIAL:
	case HAL_TONE_SECOND_DIAL:
		prog_add(p, 350, 440, 0);
		return true;
	case HAL_TONE_STUTTER:
	case HAL_TONE_SHORT_STUTTER_DIAL:
		for (i = 0; i < 3; i++) {
			prog_add(p, 350, 440, 100);
			prog_add(p, 0, 0, 100);
		}
		prog_add(p, 350, 440, 0);
		return true;
	case HAL_TONE_RINGBACK:
		prog_add(p, 440, 480, 2000);
		prog_add(p, 0, 0, 4000);
		p->loop = true;
		return true;
	case HAL_TONE_BUSY:
		prog_add(p, 480, 620, 500);
		prog_add(p, 0, 0, 500);
		p->loop = true;
		return true;
	case HAL_TONE_REORDER:
	case HAL_TONE_NUMBER_UNOBTAINABLE:
		prog_add(p, 480, 620, 250);
		prog_add(p, 0, 0, 250);
		p->loop = true;
		return true;
	case HAL_TONE_CALL_WAITING:
		prog_add(p, 440, 0, 300);
		prog_add(p, 0, 0, 10000);
		p->loop = true;
		return true;
	case HAL_TONE_CONFIRM:
		for (i = 0; i < 3; i++) {
			prog_add(p, 350, 440, 100);
			prog_add(p, 0, 0, 100);
		}
		return true;
	default:
		return tone_prog_etsi(tone, p);
	}
}

int css_tone_open(struct hal *h)
{
	struct css_tone *t;

	t = calloc(1, sizeof(*t));
	if (!t)
		return -1;

	sine_init();
	t->h = h;
	t->run = true;
	pthread_mutex_init(&t->lock, NULL);
	pthread_cond_init(&t->cond, NULL);
	h->tone = t;

	if (pthread_create(&t->thread, NULL, tone_thread, t)) {
		ULOG_ERR("css: tone thread: %s\n", strerror(errno));
		return -1;
	}
	t->started = true;

	return 0;
}

void css_tone_close(struct hal *h)
{
	struct css_tone *t = h->tone;

	if (!t)
		return;

	if (t->started) {
		pthread_mutex_lock(&t->lock);
		t->run = false;
		pthread_cond_signal(&t->cond);
		pthread_mutex_unlock(&t->lock);
		pthread_join(t->thread, NULL);
	}

	pthread_cond_destroy(&t->cond);
	pthread_mutex_destroy(&t->lock);
	free(t);
	h->tone = NULL;
}

enum hal_status css_tone_play(struct hal *h, unsigned int line,
			      enum hal_tone tone, uint32_t duration_ms)
{
	struct css_tone *t = h->tone;
	struct css_prog p = { .level = CSS_LEVEL_TONE };
	bool ok;

	if (!t)
		return HAL_FATAL;

	if (tone == HAL_TONE_NONE) {
		voice_set(t, CSS_SLOT_TONE, NULL);
		return HAL_OK;
	}

	ok = t->usa ? tone_prog_usa(tone, &p) : tone_prog_etsi(tone, &p);
	if (!ok)
		return HAL_UNSUPPORTED;

	p.limit_ms = duration_ms;
	voice_set(t, CSS_SLOT_TONE, &p);

	return HAL_OK;
}

enum hal_status css_tone_stop(struct hal *h, unsigned int line)
{
	if (!h->tone)
		return HAL_FATAL;

	voice_set(h->tone, CSS_SLOT_TONE, NULL);

	return HAL_OK;
}

enum hal_status css_digit_play(struct hal *h, unsigned int line, char digit,
			       uint32_t duration_ms)
{
	struct css_prog p = { .level = CSS_LEVEL_DIGIT };
	unsigned int i;

	if (!h->tone)
		return HAL_FATAL;

	for (i = 0; i < ARRAY_SIZE(css_dtmf); i++) {
		if (css_dtmf[i].digit != digit)
			continue;
		prog_add(&p, css_dtmf[i].low, css_dtmf[i].high,
			 duration_ms ? duration_ms : CSS_DIGIT_MS);
		voice_set(h->tone, CSS_SLOT_DIGIT, &p);
		return HAL_OK;
	}

	return HAL_UNSUPPORTED;
}

static void ring_add(struct css_prog *p, enum hal_ring_sound sound,
		     uint16_t on_ms, uint16_t off_ms)
{
	if (on_ms && p->count < CSS_SEGS)
		p->seg[p->count++] = (struct css_seg){
			css_ring_sounds[sound].f1, css_ring_sounds[sound].f2,
			on_ms, css_ring_sounds[sound].warble_ms, false
		};
	if (off_ms)
		prog_add(p, 0, 0, off_ms);
}

enum hal_status css_ring_start(struct hal *h, unsigned int line,
			       const struct hal_ring *ring)
{
	struct css_prog p = { .level = CSS_LEVEL_RING, .loop = true };
	struct css_tone *t = h->tone;
	unsigned int i;

	if (!t)
		return HAL_FATAL;

	for (i = 0; i < ring->cadence_count && i < HAL_CADENCE_MAX; i++)
		ring_add(&p, t->ring_sound, ring->cadence[i].on_ms,
			 ring->cadence[i].off_ms);
	if (!p.count)
		ring_add(&p, t->ring_sound, CSS_RING_DEFAULT_ON_MS,
			 CSS_RING_DEFAULT_OFF_MS);
	p.limit_ms = ring->duration_ms;

	css_audio_ring_route(h, true);
	voice_set(t, CSS_SLOT_RING, &p);

	return HAL_OK;
}

enum hal_status css_ring_stop(struct hal *h, unsigned int line)
{
	if (!h->tone)
		return HAL_FATAL;

	voice_set(h->tone, CSS_SLOT_RING, NULL);
	css_audio_ring_route(h, false);

	return HAL_OK;
}

enum hal_status css_tone_ring_sound_set(struct hal *h,
					enum hal_ring_sound sound)
{
	if (!h->tone)
		return HAL_FATAL;
	if (sound >= __HAL_RING_SOUND_MAX)
		return HAL_UNSUPPORTED;

	h->tone->ring_sound = sound;

	return HAL_OK;
}

enum hal_status css_region_apply(struct hal *h, const char *iso3,
				 struct hal_job *job)
{
	if (!h->tone || !job)
		return HAL_FATAL;

	h->tone->usa = iso3 && !strcmp(iso3, "USA");
	job->token = hal_relay_stats_token_next(&h->relay);
	hal_relay_completion_post(&h->relay, job->token, HAL_OK);

	return HAL_OK;
}
