// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include <libubox/ulog.h>
#include <libubox/utils.h>

#include "css_int.h"

#define CSS_CARD		"hw:dvf99css"
#define CSS_LED_DIR		"/sys/class/leds"
#define CSS_BLINK_SLOW_MS	"500"
#define CSS_BLINK_FAST_MS	"125"
#define CSS_MIC_GAIN_MIN_DB	-4
#define CSS_MIC_GAIN_STEP_DB	2
#define CSS_OUT_GAIN_MAX_DB	6
#define CSS_OUT_GAIN_STEP_DB	6

const char *css_card_name = CSS_CARD;
const char *css_led_dir = CSS_LED_DIR;

struct css_gain {
	long handset_mic;
	long headset_mic;
	long speaker_mic;
	long handset_out;
	long headset_out;
};

struct css_audio {
	snd_ctl_t *ctl;
	struct css_gain gain;
	enum hal_audio_path path;
	bool ring_route;
};

struct css_mix {
	const char *name;
	const char *item;
	long value;
};

static const struct css_mix css_mix_base[] = {
	{ .name = "Output Mux0", .item = "DAC0" },
	{ .name = "Output Mux1", .item = "DAC0_INV" },
	{ .name = "Output Mux2", .item = "DAC1" },
	{ .name = "Output Mux3", .item = "DAC1_INV" },
	{ .name = "VREF_LVL", .item = "VCCA/2" },
	{ .name = "DAC0_EN", .item = NULL, .value = 1 },
	{ .name = "DAC1_EN", .item = NULL, .value = 1 },
	{ .name = "ADC0_EN", .item = NULL, .value = 1 },
	{ .name = "MICPWR0_EN", .item = NULL, .value = 1 },
	{ .name = "MICPWR1_EN", .item = NULL, .value = 1 },
	{ .name = "VREF_EN", .item = NULL, .value = 1 },
	{ .name = "DIFFIN0_EN", .item = NULL, .value = 1 },
	{ .name = "DIFFIN1_EN", .item = NULL, .value = 1 },
	{ .name = "SINGIN0_EN", .item = NULL, .value = 1 },
	{ .name = "SINGIN1_EN", .item = NULL, .value = 1 },
	{ .name = "AMPOUT0_EN", .item = NULL, .value = 1 },
	{ .name = "AMPOUT1_EN", .item = NULL, .value = 1 },
	{ .name = "AMPOUT2_EN", .item = NULL, .value = 1 },
	{ .name = "AMPOUT3_EN", .item = NULL, .value = 1 },
};

static const char *const css_wbhf_modes[] = {
	[HAL_AUDIO_NONE] = "Idle",
	[HAL_AUDIO_HANDSET] = "Handset",
	[HAL_AUDIO_HEADSET] = "Handsfree",
	[HAL_AUDIO_SPEAKER] = "D-Class",
	[HAL_AUDIO_HANDSET_SPEAKER] = "Group Handset",
	[HAL_AUDIO_HEADSET_SPEAKER] = "Group Headset",
};

static const char *const css_led_names[] = {
	[HAL_LED_MUTE] = ":mute",
	[HAL_LED_SPEAKER] = ":speaker",
	[HAL_LED_HEADSET] = ":headset",
	[HAL_LED_MESSAGE] = ":message",
	[HAL_LED_CALL] = ":call",
};

static int ctl_info(snd_ctl_t *ctl, const char *name, snd_ctl_elem_id_t *id,
		    snd_ctl_elem_info_t *info)
{
	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, name);
	snd_ctl_elem_info_set_id(info, id);

	return snd_ctl_elem_info(ctl, info);
}

static int ctl_item_index(snd_ctl_t *ctl, snd_ctl_elem_info_t *info,
			  const char *item)
{
	unsigned int i, n = snd_ctl_elem_info_get_items(info);

	for (i = 0; i < n; i++) {
		snd_ctl_elem_info_set_item(info, i);
		if (snd_ctl_elem_info(ctl, info) < 0)
			return -1;
		if (!strcmp(snd_ctl_elem_info_get_item_name(info), item))
			return (int)i;
	}

	return -1;
}

static int ctl_set(snd_ctl_t *ctl, const struct css_mix *m)
{
	snd_ctl_elem_value_t *val;
	snd_ctl_elem_info_t *info;
	snd_ctl_elem_id_t *id;
	int idx, ret;

	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_value_alloca(&val);

	ret = ctl_info(ctl, m->name, id, info);
	if (ret < 0)
		return ret;

	snd_ctl_elem_value_set_id(val, id);

	switch (snd_ctl_elem_info_get_type(info)) {
	case SND_CTL_ELEM_TYPE_ENUMERATED:
		idx = ctl_item_index(ctl, info, m->item);
		if (idx < 0)
			return -EINVAL;
		snd_ctl_elem_value_set_enumerated(val, 0, (unsigned int)idx);
		break;
	case SND_CTL_ELEM_TYPE_BOOLEAN:
		snd_ctl_elem_value_set_boolean(val, 0, m->value);
		break;
	case SND_CTL_ELEM_TYPE_INTEGER:
		snd_ctl_elem_value_set_integer(val, 0, m->value);
		break;
	default:
		return -EINVAL;
	}

	return snd_ctl_elem_write(ctl, val);
}

static int ctl_set_list(snd_ctl_t *ctl, const struct css_mix *m, size_t n)
{
	size_t i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = ctl_set(ctl, &m[i]);
		if (ret < 0) {
			ULOG_ERR("css: mixer %s: %s\n", m[i].name,
				 snd_strerror(ret));
			return ret;
		}
	}

	return 0;
}

static int handset_mix_set(struct css_audio *a)
{
	const struct css_mix m[] = {
		{ .name = "Input Mux0", .item = "DIFFIN1" },
		{ .name = "DIFFIN1 Gain", .value = a->gain.handset_mic },
		{ .name = "Output0 Gain", .value = a->gain.handset_out },
		{ .name = "Output1 Gain", .value = a->gain.handset_out },
	};

	return ctl_set_list(a->ctl, m, ARRAY_SIZE(m));
}

static int headset_mix_set(struct css_audio *a)
{
	const struct css_mix m[] = {
		{ .name = "Input Mux0", .item = "SINGIN0-SINGIN1" },
		{ .name = "SINGIN0 Gain", .value = a->gain.headset_mic },
		{ .name = "SINGIN1 Gain", .value = a->gain.headset_mic },
		{ .name = "Output2 Gain", .value = a->gain.headset_out },
		{ .name = "Output3 Gain", .value = a->gain.headset_out },
	};

	return ctl_set_list(a->ctl, m, ARRAY_SIZE(m));
}

static int speaker_mix_set(struct css_audio *a)
{
	const struct css_mix m[] = {
		{ .name = "Input Mux0", .item = "DIFFIN0" },
		{ .name = "DIFFIN0 Gain", .value = a->gain.speaker_mic },
	};

	return ctl_set_list(a->ctl, m, ARRAY_SIZE(m));
}

static int path_mix_set(struct css_audio *a, enum hal_audio_path path)
{
	switch (path) {
	case HAL_AUDIO_HANDSET:
	case HAL_AUDIO_HANDSET_SPEAKER:
		return handset_mix_set(a);
	case HAL_AUDIO_HEADSET:
	case HAL_AUDIO_HEADSET_SPEAKER:
		return headset_mix_set(a);
	case HAL_AUDIO_SPEAKER:
		return speaker_mix_set(a);
	case HAL_AUDIO_NONE:
		break;
	}

	return 0;
}

static long mic_gain_reg(int db)
{
	return (db - CSS_MIC_GAIN_MIN_DB) / CSS_MIC_GAIN_STEP_DB;
}

static long out_gain_reg(int db)
{
	return (CSS_OUT_GAIN_MAX_DB - db) / CSS_OUT_GAIN_STEP_DB;
}

static enum hal_status ctl_status(int ret)
{
	return ret < 0 ? HAL_FATAL : HAL_OK;
}

static snd_ctl_t *audio_ctl(struct css_audio *a)
{
	int ret;

	if (!a)
		return NULL;
	if (a->ctl)
		return a->ctl;

	ret = snd_ctl_open(&a->ctl, css_card_name, 0);
	if (ret < 0) {
		ULOG_ERR("css: %s: %s\n", css_card_name, snd_strerror(ret));
		a->ctl = NULL;
		return NULL;
	}

	if (ctl_set_list(a->ctl, css_mix_base, ARRAY_SIZE(css_mix_base)) < 0) {
		snd_ctl_close(a->ctl);
		a->ctl = NULL;
	}

	return a->ctl;
}

int css_audio_open(struct hal *h)
{
	struct css_audio *a;

	a = calloc(1, sizeof(*a));
	if (!a)
		return -1;

	h->audio = a;
	audio_ctl(a);

	return 0;
}

void css_audio_close(struct hal *h)
{
	struct css_audio *a = h->audio;

	if (!a)
		return;

	if (a->ctl)
		snd_ctl_close(a->ctl);
	free(a);
	h->audio = NULL;
}

enum hal_status css_audio_path_set(struct hal *h, enum hal_audio_path path)
{
	struct css_audio *a = h->audio;
	struct css_mix mode = { .name = "WBHF Mode", .item = NULL };
	struct css_mix dclass = { .name = "DCLASS_EN", .item = NULL, .value = 0 };
	int ret = 0;

	if (path > HAL_AUDIO_HEADSET_SPEAKER)
		return HAL_UNSUPPORTED;

	if (!audio_ctl(a))
		return HAL_FATAL;

	ret = path_mix_set(a, path);
	if (ret < 0)
		return HAL_FATAL;

	dclass.value = path == HAL_AUDIO_SPEAKER ||
		       path == HAL_AUDIO_HANDSET_SPEAKER ||
		       path == HAL_AUDIO_HEADSET_SPEAKER;
	ret = ctl_set(a->ctl, &dclass);
	if (ret < 0)
		return HAL_FATAL;

	mode.item = css_wbhf_modes[path];
	ret = ctl_set(a->ctl, &mode);
	if (ret < 0)
		return HAL_FATAL;

	a->path = path;
	a->ring_route = false;

	return HAL_OK;
}

enum hal_status css_audio_tune_set(struct hal *h,
				   const struct hal_audio_tune *tune)
{
	struct css_audio *a = h->audio;

	if (!a)
		return HAL_FATAL;

	a->gain.handset_mic = mic_gain_reg(tune->handset_mic_gain);
	a->gain.headset_mic = mic_gain_reg(tune->headset_mic_gain);
	a->gain.speaker_mic = mic_gain_reg(tune->speaker_mic_gain);
	a->gain.handset_out = out_gain_reg(tune->handset_out_gain);
	a->gain.headset_out = out_gain_reg(tune->headset_out_gain);

	if (!audio_ctl(a))
		return HAL_FATAL;

	return ctl_status(path_mix_set(a, a->path));
}

void css_audio_ring_route(struct hal *h, bool on)
{
	struct css_audio *a = h->audio;
	struct css_mix mode = { .name = "WBHF Mode", .item = NULL };
	struct css_mix dclass = { .name = "DCLASS_EN", .item = NULL, .value = on };

	if (!audio_ctl(a) || a->path != HAL_AUDIO_NONE || a->ring_route == on)
		return;

	mode.item = on ? css_wbhf_modes[HAL_AUDIO_SPEAKER] :
			 css_wbhf_modes[HAL_AUDIO_NONE];
	if (ctl_set(a->ctl, &dclass) < 0 || ctl_set(a->ctl, &mode) < 0) {
		ULOG_ERR("css: ring route %s failed\n", on ? "on" : "off");
		return;
	}

	a->ring_route = on;
}

enum hal_status css_volume_set(struct hal *h, unsigned int level)
{
	struct css_audio *a = h->audio;
	struct css_mix m = { .name = "Receive Volume", .item = NULL, .value = level };

	if (level > HAL_VOLUME_MAX)
		return HAL_UNSUPPORTED;

	if (!audio_ctl(a))
		return HAL_FATAL;

	return ctl_status(ctl_set(a->ctl, &m));
}

enum hal_status css_mute_set(struct hal *h, bool mute)
{
	struct css_audio *a = h->audio;
	struct css_mix m = { .name = "Mic Mute", .item = NULL, .value = mute };

	if (!audio_ctl(a))
		return HAL_FATAL;

	return ctl_status(ctl_set(a->ctl, &m));
}

static int led_write(const char *led, const char *attr, const char *value)
{
	char path[PATH_MAX];
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), "%s/%s/%s", css_led_dir, led, attr);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	n = write(fd, value, strlen(value));
	close(fd);

	return n == (ssize_t)strlen(value) ? 0 : -1;
}

enum hal_status css_led_set(struct hal *h, enum hal_led led,
			    enum hal_led_mode mode)
{
	const char *name, *ms;
	int ret;

	if (led > HAL_LED_CALL || mode > HAL_LED_HEARTBEAT)
		return HAL_UNSUPPORTED;

	name = css_led_names[led];

	switch (mode) {
	case HAL_LED_OFF:
	case HAL_LED_ON:
		ret = led_write(name, "trigger", "none");
		if (!ret)
			ret = led_write(name, "brightness",
					mode == HAL_LED_ON ? "1" : "0");
		break;
	case HAL_LED_HEARTBEAT:
		ret = led_write(name, "trigger", "heartbeat");
		break;
	default:
		ms = mode == HAL_LED_BLINK_FAST ? CSS_BLINK_FAST_MS :
						  CSS_BLINK_SLOW_MS;
		ret = led_write(name, "trigger", "timer");
		if (!ret)
			ret = led_write(name, "delay_on", ms);
		if (!ret)
			ret = led_write(name, "delay_off", ms);
		break;
	}

	return ret ? HAL_FATAL : HAL_OK;
}
