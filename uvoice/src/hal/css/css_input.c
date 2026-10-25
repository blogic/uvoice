// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <libubox/ulog.h>
#include <libubox/utils.h>

#include "css_int.h"

#define CSS_INPUT_DIR		"/dev/input"
#define CSS_INPUT_MAX		4
#define CSS_INPUT_LINE		0
#define CSS_BITS_LONG		(sizeof(unsigned long) * 8)
#define CSS_BITS_LEN(n)		(((n) + CSS_BITS_LONG - 1) / CSS_BITS_LONG)

const char *css_input_dir = CSS_INPUT_DIR;

struct css_input_dev {
	struct uloop_fd ufd;
	struct hal *h;
};

struct css_input {
	struct css_input_dev dev[CSS_INPUT_MAX];
	unsigned int count;
	int hook_fd;
};

static const struct {
	uint16_t code;
	char digit;
} css_digit_keys[] = {
	{ KEY_NUMERIC_0, '0' }, { KEY_NUMERIC_1, '1' }, { KEY_NUMERIC_2, '2' },
	{ KEY_NUMERIC_3, '3' }, { KEY_NUMERIC_4, '4' }, { KEY_NUMERIC_5, '5' },
	{ KEY_NUMERIC_6, '6' }, { KEY_NUMERIC_7, '7' }, { KEY_NUMERIC_8, '8' },
	{ KEY_NUMERIC_9, '9' }, { KEY_NUMERIC_STAR, '*' },
	{ KEY_NUMERIC_POUND, '#' },
};

static const struct {
	uint16_t code;
	enum hal_key key;
} css_func_keys[] = {
	{ KEY_SOUND, HAL_KEY_SPEAKER },
	{ KEY_PROG4, HAL_KEY_HEADSET },
	{ KEY_MUTE, HAL_KEY_MUTE },
	{ KEY_VOLUMEUP, HAL_KEY_VOL_UP },
	{ KEY_VOLUMEDOWN, HAL_KEY_VOL_DOWN },
	{ KEY_PAUSE, HAL_KEY_HOLD },
	{ KEY_FORWARD, HAL_KEY_TRANSFER },
	{ KEY_PROG2, HAL_KEY_CONFERENCE },
	{ KEY_PROG3, HAL_KEY_DND },
	{ KEY_MESSENGER, HAL_KEY_MESSAGE },
};

static bool bit_test(const unsigned long *bits, unsigned int nr)
{
	return bits[nr / CSS_BITS_LONG] & (1UL << (nr % CSS_BITS_LONG));
}

static void input_hook_emit(struct hal *h, bool offhook)
{
	struct hal_event ev = {
		.type = HAL_EVENT_HOOK,
		.line = CSS_INPUT_LINE,
	};

	ev.u.hook.state = offhook ? HAL_HOOK_SEIZE : HAL_HOOK_RELEASE;
	css_event_emit(h, &ev);
}

static void input_digit_emit(struct hal *h, char digit)
{
	struct hal_event ev = {
		.type = HAL_EVENT_DIGIT,
		.line = CSS_INPUT_LINE,
	};

	ev.u.digit.digit = digit;
	css_event_emit(h, &ev);
}

static void input_key_emit(struct hal *h, enum hal_key key, bool pressed)
{
	struct hal_event ev = {
		.type = HAL_EVENT_INPUT,
		.line = CSS_INPUT_LINE,
	};

	ev.u.input.key = (uint8_t)key;
	ev.u.input.pressed = pressed;
	css_event_emit(h, &ev);
}

static void input_key_take(struct hal *h, uint16_t code, int32_t value)
{
	unsigned int i;

	if (value == 2)
		return;

	if (code == KEY_PHONE) {
		input_hook_emit(h, value);
		return;
	}

	for (i = 0; i < ARRAY_SIZE(css_digit_keys); i++) {
		if (css_digit_keys[i].code != code)
			continue;
		if (value)
			input_digit_emit(h, css_digit_keys[i].digit);
		return;
	}

	for (i = 0; i < ARRAY_SIZE(css_func_keys); i++) {
		if (css_func_keys[i].code == code) {
			input_key_emit(h, css_func_keys[i].key, value);
			return;
		}
	}
}

static void input_read(struct uloop_fd *u, unsigned int events)
{
	struct css_input_dev *d = container_of(u, struct css_input_dev, ufd);
	struct input_event ev[16];
	ssize_t n;
	size_t i;

	for (;;) {
		n = read(u->fd, ev, sizeof(ev));
		if (n <= 0)
			return;

		for (i = 0; i < (size_t)n / sizeof(ev[0]); i++)
			if (ev[i].type == EV_KEY)
				input_key_take(d->h, ev[i].code, ev[i].value);
	}
}

static bool input_wanted(int fd, bool *hook)
{
	unsigned long keys[CSS_BITS_LEN(KEY_CNT)] = { 0 };
	unsigned int i;

	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0)
		return false;

	*hook = bit_test(keys, KEY_PHONE);
	if (*hook)
		return true;

	for (i = 0; i < ARRAY_SIZE(css_digit_keys); i++)
		if (bit_test(keys, css_digit_keys[i].code))
			return true;

	return false;
}

int css_input_open(struct hal *h)
{
	struct css_input *in;
	struct dirent *de;
	char path[PATH_MAX];
	bool offhook;
	DIR *dir;

	in = calloc(1, sizeof(*in));
	if (!in)
		return -1;

	in->hook_fd = -1;
	h->input = in;

	dir = opendir(css_input_dir);
	if (!dir) {
		ULOG_ERR("css: %s: %s\n", css_input_dir, strerror(errno));
		return -1;
	}

	while ((de = readdir(dir)) && in->count < CSS_INPUT_MAX) {
		struct css_input_dev *d = &in->dev[in->count];
		bool hook = false;
		int fd;

		if (strncmp(de->d_name, "event", 5))
			continue;

		snprintf(path, sizeof(path), "%s/%s", css_input_dir, de->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;

		if (!input_wanted(fd, &hook)) {
			close(fd);
			continue;
		}

		d->h = h;
		d->ufd.fd = fd;
		d->ufd.cb = input_read;
		if (uloop_fd_add(&d->ufd, ULOOP_READ)) {
			close(fd);
			continue;
		}

		if (hook)
			in->hook_fd = fd;
		in->count++;
	}
	closedir(dir);

	ULOG_INFO("css: %u input devices\n", in->count);

	if (in->hook_fd < 0) {
		ULOG_WARN("css: no input device carries the hook switch\n");
		return 0;
	}

	if (css_input_hook_query(h, CSS_INPUT_LINE, &offhook) == HAL_OK)
		input_hook_emit(h, offhook);

	return 0;
}

void css_input_close(struct hal *h)
{
	struct css_input *in = h->input;
	unsigned int i;

	if (!in)
		return;

	for (i = 0; i < in->count; i++) {
		uloop_fd_delete(&in->dev[i].ufd);
		close(in->dev[i].ufd.fd);
	}

	free(in);
	h->input = NULL;
}

enum hal_status css_input_hook_query(struct hal *h, unsigned int line,
				     bool *offhook)
{
	unsigned long keys[CSS_BITS_LEN(KEY_CNT)] = { 0 };
	struct css_input *in = h->input;

	if (line != CSS_INPUT_LINE) {
		*offhook = false;
		return HAL_OK;
	}

	if (!in || in->hook_fd < 0)
		return HAL_UNSUPPORTED;

	if (ioctl(in->hook_fd, EVIOCGKEY(sizeof(keys)), keys) < 0)
		return HAL_FATAL;

	*offhook = bit_test(keys, KEY_PHONE);

	return HAL_OK;
}
