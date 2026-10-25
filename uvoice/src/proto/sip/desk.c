// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "proto/sip/desk.h"

#define DESK_DIAL_IDX		DESK_CALLS

static unsigned int idx_of(unsigned int line, unsigned int slot)
{
	return line * DESK_SLOTS + slot;
}

static unsigned int line_of(unsigned int i)
{
	return i == DESK_DIAL_IDX ? DESK_NONE : i / DESK_SLOTS;
}

static unsigned int slot_of(unsigned int i)
{
	return i % DESK_SLOTS;
}

static bool call_valid(unsigned int line, unsigned int slot)
{
	return line < DESK_LINES && slot < DESK_SLOTS;
}

static struct desk_call *dial_call(struct desk *d)
{
	return &d->calls[DESK_DIAL_IDX];
}

static bool dialling(const struct desk *d)
{
	return d->calls[DESK_DIAL_IDX].state != DESK_CALL_IDLE;
}

static void notify_call(struct desk *d, unsigned int i)
{
	struct desk_call *c = &d->calls[i];

	if (!d->ops->call_changed)
		return;

	if (i == DESK_DIAL_IDX)
		d->ops->call_changed(d->priv, c->line, DESK_DIAL_SLOT);
	else
		d->ops->call_changed(d->priv, line_of(i), slot_of(i));
}

static void notify_device(struct desk *d)
{
	if (d->ops->device_changed)
		d->ops->device_changed(d->priv);
}

static uint32_t now_s(struct desk *d)
{
	return d->ops->now_s ? d->ops->now_s(d->priv) : 0;
}

static void call_state_set(struct desk *d, unsigned int i,
			   enum desk_call_state state)
{
	struct desk_call *c = &d->calls[i];

	if (c->state == state)
		return;

	c->state = state;
	c->since = now_s(d);
	if (state == DESK_CALL_CONNECTED && !c->connected)
		c->connected = c->since;
	notify_call(d, i);
}

static unsigned int any_in_state(const struct desk *d,
				 enum desk_call_state state)
{
	unsigned int i, found = DESK_NONE;
	uint32_t since = 0;

	for (i = 0; i < DESK_CALLS; i++) {
		if (d->calls[i].state != state)
			continue;
		if (found == DESK_NONE || d->calls[i].since >= since) {
			found = i;
			since = d->calls[i].since;
		}
	}

	return found;
}

static bool any_call(const struct desk *d)
{
	unsigned int i;

	for (i = 0; i <= DESK_CALLS; i++)
		if (d->calls[i].state != DESK_CALL_IDLE)
			return true;

	return false;
}

enum desk_state desk_state_get(const struct desk *d)
{
	if (dialling(d))
		return DESK_DIAL;
	if (d->focus != DESK_NONE)
		return DESK_ACTIVE;
	if (any_in_state(d, DESK_CALL_RINGING) != DESK_NONE)
		return DESK_ALERT;
	if (any_in_state(d, DESK_CALL_HELD) != DESK_NONE)
		return DESK_PARKED;
	if (d->lockout)
		return DESK_ENDED;

	return DESK_IDLE;
}

static void tone_set(struct desk *d, enum hal_tone tone)
{
	if (d->tone == tone)
		return;

	d->tone = tone;
	d->ops->tone(d->priv, tone);
}

static void led_set(struct desk *d, enum hal_led led, enum hal_led_mode mode)
{
	if (d->leds[led] == mode)
		return;

	d->leds[led] = (uint8_t)mode;
	if (d->ops->led)
		d->ops->led(d->priv, led, mode);
}

static bool path_has_speaker(enum hal_audio_path path)
{
	return path == HAL_AUDIO_SPEAKER || path == HAL_AUDIO_HANDSET_SPEAKER ||
	       path == HAL_AUDIO_HEADSET_SPEAKER;
}

static bool path_has_headset(enum hal_audio_path path)
{
	return path == HAL_AUDIO_HEADSET || path == HAL_AUDIO_HEADSET_SPEAKER;
}

static bool path_has_handset(enum hal_audio_path path)
{
	return path == HAL_AUDIO_HANDSET || path == HAL_AUDIO_HANDSET_SPEAKER;
}

static void leds_update(struct desk *d)
{
	enum hal_led_mode call = HAL_LED_OFF;

	if (any_in_state(d, DESK_CALL_RINGING) != DESK_NONE)
		call = HAL_LED_BLINK_FAST;
	else if (any_call(d))
		call = HAL_LED_ON;

	led_set(d, HAL_LED_MUTE, d->mute ? HAL_LED_ON : HAL_LED_OFF);
	led_set(d, HAL_LED_SPEAKER,
		path_has_speaker(d->path) ? HAL_LED_ON : HAL_LED_OFF);
	led_set(d, HAL_LED_HEADSET,
		path_has_headset(d->path) ? HAL_LED_ON : HAL_LED_OFF);
	led_set(d, HAL_LED_MESSAGE, d->missed ? HAL_LED_HEARTBEAT :
				    d->mwi ? HAL_LED_BLINK_SLOW : HAL_LED_OFF);
	led_set(d, HAL_LED_CALL, call);
}

static void mute_apply(struct desk *d, bool on)
{
	if (d->mute == on)
		return;

	d->mute = on;
	if (d->ops->mute)
		d->ops->mute(d->priv, on);
	notify_device(d);
}

static void lockout_set(struct desk *d, bool on)
{
	if (d->lockout == on)
		return;

	d->lockout = on;
	notify_device(d);
}

static void path_set(struct desk *d, enum hal_audio_path path)
{
	if (d->path == path)
		return;

	d->path = path;
	if (d->ops->audio_path)
		d->ops->audio_path(d->priv, path);
	if (path != HAL_AUDIO_NONE && d->ops->volume)
		d->ops->volume(d->priv, d->volume[path]);
	if (path == HAL_AUDIO_NONE)
		mute_apply(d, false);
	notify_device(d);
}

static void preview_end(struct desk *d)
{
	if (!d->previewing)
		return;

	d->previewing = false;
	sip_timer_cancel(d->env, &d->t_preview);
	if (d->ops->ring_preview)
		d->ops->ring_preview(d->priv, false);
}

static bool desk_quiet(const struct desk *d)
{
	return desk_state_get(d) == DESK_IDLE && !d->hook_up && !d->ringing &&
	       d->path == HAL_AUDIO_NONE;
}

static void ringer_update(struct desk *d)
{
	unsigned int ring = any_in_state(d, DESK_CALL_RINGING);
	bool busy = d->focus != DESK_NONE || dialling(d);
	bool want = ring != DESK_NONE && !busy;

	if (!desk_quiet(d))
		preview_end(d);

	if (want && !d->ringing) {
		d->ringing = true;
		d->ops->ring_start(d->priv, line_of(ring), slot_of(ring));
		if (d->ops->volume && d->path == HAL_AUDIO_NONE)
			d->ops->volume(d->priv, d->ring_volume);
	} else if (!want && d->ringing) {
		d->ringing = false;
		d->ops->ring_stop(d->priv);
	}

	if (ring != DESK_NONE && busy && d->focus != DESK_NONE)
		tone_set(d, HAL_TONE_CALL_WAITING);
	else if (d->tone == HAL_TONE_CALL_WAITING)
		tone_set(d, HAL_TONE_NONE);
}

static void focus_notify(struct desk *d)
{
	unsigned int was = d->focus_sent;

	if (d->focus == was)
		return;

	d->focus_sent = d->focus;
	if (was != DESK_NONE && d->calls[was].state != DESK_CALL_IDLE)
		notify_call(d, was);
	if (d->focus != DESK_NONE)
		notify_call(d, d->focus);
}

static void settle(struct desk *d)
{
	focus_notify(d);
	ringer_update(d);
	leds_update(d);
}

static void call_clear(struct desk *d, unsigned int i,
		       enum desk_end_reason reason)
{
	struct desk_call *c = &d->calls[i];

	if (c->state == DESK_CALL_IDLE)
		return;

	c->end_reason = (uint8_t)reason;
	c->state = DESK_CALL_IDLE;
	c->since = now_s(d);
	notify_call(d, i);
	memset(c, 0, sizeof(*c));

	if (d->focus == i)
		d->focus = DESK_NONE;
	if (d->xfer_from == i)
		d->xfer_from = DESK_NONE;
	if (d->clearing_call == i) {
		d->clearing_call = DESK_NONE;
		sip_timer_cancel(d->env, &d->t_clearing);
	}
}

static void focus_lost(struct desk *d)
{
	if (d->focus != DESK_NONE || dialling(d))
		return;

	tone_set(d, HAL_TONE_NONE);
	mute_apply(d, false);

	if (any_in_state(d, DESK_CALL_HELD) != DESK_NONE)
		return;

	if (!d->hook_up) {
		path_set(d, HAL_AUDIO_NONE);
		return;
	}

	if (!path_has_handset(d->path))
		path_set(d, HAL_AUDIO_HANDSET);
	lockout_set(d, true);
}

static void clearing_enter(struct desk *d, unsigned int i, enum hal_tone tone,
			   enum desk_end_reason reason)
{
	d->calls[i].end_reason = (uint8_t)reason;
	call_state_set(d, i, DESK_CALL_CLEARING);
	tone_set(d, tone);
	d->clearing_call = i;
	sip_timer_arm(d->env, &d->t_clearing, d->cfg.clearing_ms);
}

/* ----------------------------------------------------------------- dialling */

static unsigned int dial_line_pick(struct desk *d, unsigned int line)
{
	unsigned int l;

	if (line < d->cfg.lines)
		return line;

	for (l = 0; l < d->cfg.lines; l++)
		if (!d->ops->dial_ready || d->ops->dial_ready(d->priv, l))
			return l;

	return 0;
}

static void dial_prepare(struct desk *d, unsigned int line)
{
	struct desk_call *c = dial_call(d);

	memset(c, 0, sizeof(*c));
	c->id = ++d->next_id;
	c->line = (uint8_t)line;
}

static void dial_start(struct desk *d, unsigned int line)
{
	if (dialling(d))
		return;

	dial_prepare(d, dial_line_pick(d, line));
	d->digits[0] = 0;
	d->ndigits = 0;
	call_state_set(d, DESK_DIAL_IDX, DESK_CALL_DIALLING);
	tone_set(d, d->mwi ? HAL_TONE_STUTTER : HAL_TONE_DIAL);
	sip_timer_arm(d->env, &d->t_digit, d->cfg.start_digit_ms);
}

static void dial_cancel(struct desk *d)
{
	if (!dialling(d))
		return;

	sip_timer_cancel(d->env, &d->t_digit);
	call_clear(d, DESK_DIAL_IDX, DESK_END_LOCAL);
	tone_set(d, HAL_TONE_NONE);
}

static void dial_reorder(struct desk *d)
{
	sip_timer_cancel(d->env, &d->t_digit);
	clearing_enter(d, DESK_DIAL_IDX, HAL_TONE_REORDER, DESK_END_FAILED);
}

static int dial_place(struct desk *d, const char *number)
{
	struct desk_call *c = dial_call(d);
	unsigned int line = c->line, slot, i;

	if (!number[0])
		return -EINVAL;

	if (!d->ops->slot_free(d->priv, line, &slot) ||
	    (d->ops->dial_ready && !d->ops->dial_ready(d->priv, line))) {
		dial_reorder(d);
		return -EBUSY;
	}

	sip_timer_cancel(d->env, &d->t_digit);
	i = idx_of(line, slot);
	d->calls[i] = *c;
	d->calls[i].inbound = false;
	snprintf(d->calls[i].number, sizeof(d->calls[i].number), "%s", number);
	memset(c, 0, sizeof(*c));

	d->focus = i;
	d->calls[i].state = DESK_CALL_IDLE;
	call_state_set(d, i, DESK_CALL_CALLING);
	tone_set(d, HAL_TONE_NONE);
	d->ops->call_dial(d->priv, line, slot, number);

	return 0;
}

static void dial_commit(struct desk *d)
{
	char number[DESK_DIGITS_MAX + 1];

	dialplan_digits_strip(d->digits, number, sizeof(number));
	if (dial_place(d, number) == -EINVAL)
		dial_reorder(d);
}

static enum dp_action digits_verdict(struct desk *d)
{
	struct dp_match m;
	const struct dialplan *dp = d->cfg.dp[dial_call(d)->line];

	if (!dp)
		return DP_ACT_WAIT_SHORT;

	dialplan_match(dp, d->digits, d->ndigits, &m);

	return dp_action_of(&m);
}

static void digits_append(struct desk *d, char digit)
{
	if (d->ndigits >= DESK_DIGITS_MAX)
		return;

	d->digits[d->ndigits++] = digit;
	d->digits[d->ndigits] = 0;
}

static void dial_digit(struct desk *d, char digit)
{
	struct desk_call *c = dial_call(d);

	if (c->state != DESK_CALL_DIALLING)
		return;

	tone_set(d, HAL_TONE_NONE);
	if (d->ops->key_tone)
		d->ops->key_tone(d->priv, digit);

	if (digit == '#' && d->ndigits) {
		dial_commit(d);
		return;
	}

	digits_append(d, digit);
	dialplan_digits_strip(d->digits, c->number, sizeof(c->number));
	notify_call(d, DESK_DIAL_IDX);

	switch (digits_verdict(d)) {
	case DP_ACT_DIAL:
		dial_commit(d);
		return;
	case DP_ACT_WAIT_SHORT:
		sip_timer_arm(d->env, &d->t_digit, d->cfg.interdigit_short_ms);
		return;
	case DP_ACT_WAIT_LONG:
		sip_timer_arm(d->env, &d->t_digit, d->cfg.interdigit_long_ms);
		return;
	default:
		dial_reorder(d);
		return;
	}
}

static void dial_timer(struct desk *d)
{
	struct dp_match m;
	const struct dialplan *dp;
	enum dp_action act;

	if (dial_call(d)->state != DESK_CALL_DIALLING)
		return;

	if (!d->ndigits) {
		dial_reorder(d);
		return;
	}

	dp = d->cfg.dp[dial_call(d)->line];
	if (!dp) {
		dial_commit(d);
		return;
	}

	dialplan_match(dp, d->digits, d->ndigits, &m);
	act = dp_action_of(&m);
	if (act != DP_ACT_DIAL && (m.complete_on_timer || m.timer_next)) {
		digits_append(d, 'T');
		act = digits_verdict(d);
	}

	if (act == DP_ACT_DIAL || act == DP_ACT_WAIT_SHORT)
		dial_commit(d);
	else
		dial_reorder(d);
}

/* -------------------------------------------------------------- call verbs */

static void hold_send(struct desk *d, unsigned int i)
{
	struct desk_call *c = &d->calls[i];

	if (c->state != DESK_CALL_CONNECTED || c->pending)
		return;

	c->pending = DESK_PEND_HOLD;
	if (d->focus == i)
		d->focus = DESK_NONE;
	d->ops->call_hold(d->priv, line_of(i), slot_of(i));
}

static void resume_send(struct desk *d, unsigned int i, bool conf)
{
	struct desk_call *c = &d->calls[i];

	if (c->state != DESK_CALL_HELD || c->pending)
		return;

	c->pending = DESK_PEND_RESUME;
	c->conf_join = conf;
	if (!conf) {
		if (d->focus != DESK_NONE && d->focus != i)
			hold_send(d, d->focus);
		d->focus = i;
	}
	d->ops->call_resume(d->priv, line_of(i), slot_of(i));
}

static void conference_end(struct desk *d, unsigned int keep)
{
	unsigned int line = line_of(keep), s, i;

	d->ops->conf_set(d->priv, line, false);
	for (s = 0; s < DESK_SLOTS; s++) {
		i = idx_of(line, s);
		if (d->calls[i].state == DESK_CALL_CONFERENCE)
			call_state_set(d, i, DESK_CALL_CONNECTED);
	}
	d->focus = keep;
}

static unsigned int conference_peer(const struct desk *d, unsigned int i)
{
	unsigned int line = line_of(i), s, j;

	for (s = 0; s < DESK_SLOTS; s++) {
		j = idx_of(line, s);
		if (j != i && d->calls[j].state == DESK_CALL_CONFERENCE)
			return j;
	}

	return DESK_NONE;
}

static void hangup_call(struct desk *d, unsigned int i)
{
	struct desk_call *c = &d->calls[i];
	unsigned int peer;

	switch (c->state) {
	case DESK_CALL_DIALLING:
		dial_cancel(d);
		return;
	case DESK_CALL_RINGING:
		d->ops->call_reject(d->priv, line_of(i), slot_of(i),
				    DESK_REJECT_BUSY);
		call_clear(d, i, DESK_END_REJECTED);
		return;
	case DESK_CALL_CLEARING:
		if (i == DESK_DIAL_IDX)
			sip_timer_cancel(d->env, &d->t_digit);
		call_clear(d, i, c->end_reason);
		return;
	case DESK_CALL_CONFERENCE:
		peer = conference_peer(d, i);
		d->ops->conf_set(d->priv, line_of(i), false);
		d->ops->call_hangup(d->priv, line_of(i), slot_of(i));
		call_clear(d, i, DESK_END_LOCAL);
		if (peer != DESK_NONE) {
			d->ops->call_hangup(d->priv, line_of(peer),
					    slot_of(peer));
			call_clear(d, peer, DESK_END_LOCAL);
		}
		return;
	case DESK_CALL_IDLE:
		return;
	default:
		d->ops->call_hangup(d->priv, line_of(i), slot_of(i));
		call_clear(d, i, DESK_END_LOCAL);
		return;
	}
}

static void answer_call(struct desk *d, unsigned int i,
			enum hal_audio_path path)
{
	if (d->calls[i].state != DESK_CALL_RINGING)
		return;

	if (d->focus != DESK_NONE && d->focus != i)
		hold_send(d, d->focus);
	dial_cancel(d);

	lockout_set(d, false);
	d->focus = i;
	d->ops->call_answer(d->priv, line_of(i), slot_of(i));
	call_state_set(d, i, DESK_CALL_CONNECTED);
	tone_set(d, HAL_TONE_NONE);

	if (path != HAL_AUDIO_NONE)
		path_set(d, path);
	else if (d->path == HAL_AUDIO_NONE)
		path_set(d, HAL_AUDIO_SPEAKER);
}

static void transfer_key(struct desk *d)
{
	char number[DESK_DIGITS_MAX + 1];
	unsigned int from = d->xfer_from, i = d->focus;

	if (dialling(d) && from != DESK_NONE) {
		dialplan_digits_strip(d->digits, number, sizeof(number));
		if (!number[0])
			return;
		d->calls[from].pending = DESK_PEND_XFER;
		d->ops->call_xfer(d->priv, line_of(from), slot_of(from), number);
		dial_cancel(d);
		return;
	}

	if (i == DESK_NONE || d->calls[i].state != DESK_CALL_CONNECTED)
		return;

	if (from != DESK_NONE && from != i && line_of(from) == line_of(i) &&
	    d->calls[from].state == DESK_CALL_HELD) {
		d->ops->call_xfer_consult(d->priv, line_of(i), slot_of(from),
					  slot_of(i));
		call_clear(d, from, DESK_END_TRANSFERRED);
		call_clear(d, i, DESK_END_TRANSFERRED);
		return;
	}

	d->xfer_from = i;
	hold_send(d, i);
	dial_start(d, line_of(i));
}

static void conference_key(struct desk *d)
{
	unsigned int i = d->focus, line, s, j;

	if (i == DESK_NONE || d->calls[i].state != DESK_CALL_CONNECTED)
		return;

	line = line_of(i);
	for (s = 0; s < DESK_SLOTS; s++) {
		j = idx_of(line, s);
		if (j != i && d->calls[j].state == DESK_CALL_HELD) {
			resume_send(d, j, true);
			return;
		}
	}
}

static void volume_step(struct desk *d, int step)
{
	uint8_t *level = d->path == HAL_AUDIO_NONE ? &d->ring_volume :
						     &d->volume[d->path];
	int v = *level + step;

	if (v < 0 || v > HAL_VOLUME_MAX)
		return;

	*level = (uint8_t)v;
	if (d->ops->volume)
		d->ops->volume(d->priv, *level);
	notify_device(d);
}

/* --------------------------------------------------------------- the inputs */

static void path_open(struct desk *d, enum hal_audio_path path)
{
	unsigned int ring;

	switch (desk_state_get(d)) {
	case DESK_ALERT:
		ring = any_in_state(d, DESK_CALL_RINGING);
		answer_call(d, ring, path);
		return;
	case DESK_DIAL:
	case DESK_ACTIVE:
		path_set(d, path);
		return;
	case DESK_IDLE:
	case DESK_PARKED:
		path_set(d, path);
		dial_start(d, DESK_NONE);
		return;
	case DESK_ENDED:
		return;
	}
}

static void path_close(struct desk *d)
{
	if (dialling(d))
		dial_cancel(d);
	else if (d->focus != DESK_NONE)
		hangup_call(d, d->focus);

	if (d->focus == DESK_NONE && !dialling(d))
		path_set(d, HAL_AUDIO_NONE);
	focus_lost(d);
}

static void hook_up(struct desk *d)
{
	d->hook_up = true;
	path_open(d, HAL_AUDIO_HANDSET);
}

static void hook_down(struct desk *d)
{
	d->hook_up = false;
	lockout_set(d, false);

	if (!path_has_handset(d->path))
		return;

	path_close(d);
}

static void path_key(struct desk *d, enum hal_audio_path path)
{
	if (d->path == path) {
		if (d->hook_up && desk_state_get(d) != DESK_IDLE)
			path_set(d, HAL_AUDIO_HANDSET);
		else
			path_close(d);
		return;
	}

	path_open(d, path);
}

static void key_take(struct desk *d, enum hal_key key)
{
	unsigned int held;

	switch (key) {
	case HAL_KEY_SPEAKER:
		path_key(d, HAL_AUDIO_SPEAKER);
		return;
	case HAL_KEY_HEADSET:
		path_key(d, HAL_AUDIO_HEADSET);
		return;
	case HAL_KEY_MUTE:
		if (d->focus != DESK_NONE)
			mute_apply(d, !d->mute);
		return;
	case HAL_KEY_VOL_UP:
		volume_step(d, 1);
		return;
	case HAL_KEY_VOL_DOWN:
		volume_step(d, -1);
		return;
	case HAL_KEY_HOLD:
		if (d->focus != DESK_NONE) {
			hold_send(d, d->focus);
			return;
		}
		held = any_in_state(d, DESK_CALL_HELD);
		if (held != DESK_NONE && !dialling(d))
			resume_send(d, held, false);
		return;
	case HAL_KEY_TRANSFER:
		transfer_key(d);
		return;
	case HAL_KEY_CONFERENCE:
		conference_key(d);
		return;
	case HAL_KEY_DND:
		d->dnd = !d->dnd;
		notify_device(d);
		return;
	case HAL_KEY_MESSAGE:
		return;
	}
}

static void digit_take(struct desk *d, char digit)
{
	unsigned int i = d->focus, peer;

	if (dialling(d)) {
		dial_digit(d, digit);
		return;
	}

	if (i == DESK_NONE)
		return;

	if (d->ops->key_tone)
		d->ops->key_tone(d->priv, digit);
	d->ops->call_dtmf(d->priv, line_of(i), slot_of(i), digit,
			  d->cfg.dtmf_ms);
	peer = conference_peer(d, i);
	if (d->calls[i].state == DESK_CALL_CONFERENCE && peer != DESK_NONE)
		d->ops->call_dtmf(d->priv, line_of(peer), slot_of(peer), digit,
				  d->cfg.dtmf_ms);
}

/* --------------------------------------------------------------- call events */

static enum desk_end_reason fail_reason(uint32_t cls)
{
	switch (cls) {
	case DESK_FAIL_BUSY:
		return DESK_END_BUSY;
	case DESK_FAIL_REJECTED:
		return DESK_END_REJECTED;
	case DESK_FAIL_UNAVAILABLE:
		return DESK_END_UNAVAILABLE;
	default:
		return DESK_END_FAILED;
	}
}

static void call_incoming(struct desk *d, unsigned int i)
{
	struct desk_call *c = &d->calls[i];

	if (d->dnd) {
		d->ops->call_reject(d->priv, line_of(i), slot_of(i),
				    DESK_REJECT_BUSY);
		memset(c, 0, sizeof(*c));
		return;
	}

	c->id = ++d->next_id;
	c->line = (uint8_t)line_of(i);
	c->inbound = true;
	c->state = DESK_CALL_IDLE;
	call_state_set(d, i, DESK_CALL_RINGING);
	d->ops->call_ring(d->priv, line_of(i), slot_of(i));
}

static void call_proceeding(struct desk *d, unsigned int i, bool early)
{
	struct desk_call *c = &d->calls[i];

	if (c->state != DESK_CALL_CALLING && c->state != DESK_CALL_RINGBACK)
		return;

	c->early_media = early;
	call_state_set(d, i, DESK_CALL_RINGBACK);
	notify_call(d, i);
	if (d->focus == i)
		tone_set(d, early ? HAL_TONE_NONE : HAL_TONE_RINGBACK);
}

static void call_answered(struct desk *d, unsigned int i)
{
	struct desk_call *c = &d->calls[i];

	if (c->state != DESK_CALL_CALLING && c->state != DESK_CALL_RINGBACK)
		return;

	call_state_set(d, i, DESK_CALL_CONNECTED);
	if (d->focus == i)
		tone_set(d, HAL_TONE_NONE);
}

static void call_failed(struct desk *d, unsigned int i, uint32_t cls)
{
	struct desk_call *c = &d->calls[i];
	enum desk_end_reason reason = fail_reason(cls);

	if (c->state == DESK_CALL_IDLE)
		return;

	if (d->focus == i && (c->state == DESK_CALL_CALLING ||
			      c->state == DESK_CALL_RINGBACK)) {
		clearing_enter(d, i, reason == DESK_END_BUSY ? HAL_TONE_BUSY :
							      HAL_TONE_REORDER,
			       reason);
		return;
	}

	call_clear(d, i, reason);
	focus_lost(d);
}

static void call_remote_end(struct desk *d, unsigned int i,
			    enum desk_end_reason reason)
{
	struct desk_call *c = &d->calls[i];
	unsigned int peer;

	switch (c->state) {
	case DESK_CALL_IDLE:
	case DESK_CALL_CLEARING:
		return;
	case DESK_CALL_CONFERENCE:
		peer = conference_peer(d, i);
		call_clear(d, i, reason);
		if (peer != DESK_NONE)
			conference_end(d, peer);
		return;
	default:
		break;
	}

	if (d->focus == i && c->state == DESK_CALL_CONNECTED) {
		clearing_enter(d, i, HAL_TONE_BUSY, reason);
		return;
	}

	call_clear(d, i, reason);
	focus_lost(d);
}

static void call_reinvite(struct desk *d, unsigned int i, bool ok)
{
	struct desk_call *c = &d->calls[i];
	uint8_t op = c->pending;
	unsigned int line = line_of(i), s, j;

	if (op != DESK_PEND_HOLD && op != DESK_PEND_RESUME)
		return;

	c->pending = DESK_PEND_NONE;

	if (op == DESK_PEND_HOLD) {
		if (ok) {
			call_state_set(d, i, DESK_CALL_HELD);
			focus_lost(d);
		} else if (d->focus == DESK_NONE) {
			d->focus = i;
		}
		return;
	}

	if (!ok) {
		if (d->focus == i)
			d->focus = DESK_NONE;
		c->conf_join = false;
		focus_lost(d);
		return;
	}

	if (!c->conf_join) {
		call_state_set(d, i, DESK_CALL_CONNECTED);
		return;
	}

	c->conf_join = false;
	d->ops->conf_set(d->priv, line, true);
	for (s = 0; s < DESK_SLOTS; s++) {
		j = idx_of(line, s);
		if (d->calls[j].state == DESK_CALL_CONNECTED ||
		    d->calls[j].state == DESK_CALL_HELD)
			call_state_set(d, j, DESK_CALL_CONFERENCE);
	}
}

static void call_xfer_done(struct desk *d, unsigned int i, bool ok)
{
	struct desk_call *c = &d->calls[i];

	if (c->pending != DESK_PEND_XFER)
		return;

	c->pending = DESK_PEND_NONE;
	if (ok) {
		d->ops->call_hangup(d->priv, line_of(i), slot_of(i));
		call_clear(d, i, DESK_END_TRANSFERRED);
		focus_lost(d);
		return;
	}

	if (c->state == DESK_CALL_HELD)
		resume_send(d, i, false);
	else if (d->focus == DESK_NONE)
		d->focus = i;
}

static void clearing_timer(struct desk *d)
{
	unsigned int i = d->clearing_call;

	if (i == DESK_NONE)
		return;

	d->clearing_call = DESK_NONE;
	call_clear(d, i, d->calls[i].end_reason);
	tone_set(d, HAL_TONE_NONE);
	focus_lost(d);
}

/* ------------------------------------------------------------------- the API */

static void t_fire(struct sip_timer *t)
{
	struct desk *d = t->owner;

	enum desk_ev ev = DESK_EV_T_CLEARING;

	if (t == &d->t_digit)
		ev = DESK_EV_T_DIGIT;
	else if (t == &d->t_preview)
		ev = DESK_EV_T_PREVIEW;

	desk_event_push(d, ev, DESK_NONE, DESK_NONE, 0);
}

void desk_init(struct desk *d, struct sip_txn_env *env,
	       const struct desk_cfg *cfg, const struct desk_ops *ops,
	       void *priv)
{
	unsigned int i;

	memset(d, 0, sizeof(*d));
	d->env = env;
	d->cfg = *cfg;
	d->ops = ops;
	d->priv = priv;
	d->focus = DESK_NONE;
	d->focus_sent = DESK_NONE;
	d->xfer_from = DESK_NONE;
	d->clearing_call = DESK_NONE;
	d->ring_volume = DESK_VOLUME_DEFAULT;
	for (i = 0; i <= HAL_AUDIO_HEADSET_SPEAKER; i++)
		d->volume[i] = DESK_VOLUME_DEFAULT;
	memset(d->leds, 0xff, sizeof(d->leds));

	sip_timer_init(&d->t_digit, d, SIP_TIMER_NONE, t_fire);
	sip_timer_init(&d->t_clearing, d, SIP_TIMER_NONE, t_fire);
	sip_timer_init(&d->t_preview, d, SIP_TIMER_NONE, t_fire);
}

void desk_cfg_update(struct desk *d, const struct desk_cfg *cfg)
{
	d->cfg = *cfg;
}

void desk_timers_stop(struct desk *d)
{
	sip_timer_cancel(d->env, &d->t_digit);
	sip_timer_cancel(d->env, &d->t_clearing);
	sip_timer_cancel(d->env, &d->t_preview);
}

void desk_caller_set(struct desk *d, unsigned int line, unsigned int slot,
		     const char *number, const char *name)
{
	struct desk_call *c;

	if (!call_valid(line, slot))
		return;

	c = &d->calls[idx_of(line, slot)];
	snprintf(c->number, sizeof(c->number), "%s", number ? number : "");
	snprintf(c->name, sizeof(c->name), "%s", name ? name : "");
}

void desk_mwi_set(struct desk *d, bool on)
{
	d->mwi = on;
	leds_update(d);
}

void desk_missed_set(struct desk *d, bool on)
{
	d->missed = on;
	leds_update(d);
}

void desk_event_push(struct desk *d, enum desk_ev ev, unsigned int line,
		     unsigned int slot, uint32_t arg)
{
	unsigned int i = DESK_NONE;

	if (ev >= DESK_EV_CALL_INCOMING && ev <= DESK_EV_CALL_ENDED) {
		if (!call_valid(line, slot))
			return;
		i = idx_of(line, slot);
	}

	switch (ev) {
	case DESK_EV_HOOK_UP:
		hook_up(d);
		break;
	case DESK_EV_HOOK_DOWN:
		hook_down(d);
		break;
	case DESK_EV_KEY:
		key_take(d, (enum hal_key)arg);
		break;
	case DESK_EV_DIGIT:
		digit_take(d, (char)arg);
		break;
	case DESK_EV_CALL_INCOMING:
		call_incoming(d, i);
		break;
	case DESK_EV_CALL_PROCEEDING:
		call_proceeding(d, i, arg & 1);
		break;
	case DESK_EV_CALL_ANSWERED:
		call_answered(d, i);
		break;
	case DESK_EV_CALL_FAILED:
		call_failed(d, i, arg);
		break;
	case DESK_EV_CALL_REMOTE_BYE:
		call_remote_end(d, i, DESK_END_REMOTE);
		break;
	case DESK_EV_CALL_REMOTE_CANCEL:
		if (d->calls[i].state != DESK_CALL_IDLE) {
			call_clear(d, i, DESK_END_CANCELLED);
			focus_lost(d);
		}
		break;
	case DESK_EV_CALL_REINVITE_OK:
	case DESK_EV_CALL_REINVITE_FAIL:
		call_reinvite(d, i, ev == DESK_EV_CALL_REINVITE_OK);
		break;
	case DESK_EV_CALL_XFER_OK:
	case DESK_EV_CALL_XFER_FAIL:
		call_xfer_done(d, i, ev == DESK_EV_CALL_XFER_OK);
		break;
	case DESK_EV_CALL_ENDED:
		if (d->calls[i].state != DESK_CALL_IDLE &&
		    d->calls[i].state != DESK_CALL_CLEARING) {
			call_clear(d, i, DESK_END_REMOTE);
			focus_lost(d);
		}
		break;
	case DESK_EV_T_DIGIT:
		dial_timer(d);
		break;
	case DESK_EV_T_CLEARING:
		clearing_timer(d);
		break;
	case DESK_EV_T_PREVIEW:
		preview_end(d);
		break;
	case DESK_EV_COUNT:
		return;
	}

	settle(d);
}

const struct desk_call *desk_call_get(const struct desk *d, unsigned int line,
				      unsigned int slot)
{
	const struct desk_call *c;

	if (slot == DESK_DIAL_SLOT) {
		c = &d->calls[DESK_DIAL_IDX];
		return c->line == line ? c : NULL;
	}

	if (!call_valid(line, slot))
		return NULL;

	return &d->calls[idx_of(line, slot)];
}

static int call_index(const struct desk *d, unsigned int line,
		      unsigned int slot, unsigned int *out)
{
	if (slot == DESK_DIAL_SLOT) {
		if (!dialling(d) || d->calls[DESK_DIAL_IDX].line != line)
			return -ENOENT;
		*out = DESK_DIAL_IDX;
		return 0;
	}

	if (!call_valid(line, slot) ||
	    d->calls[idx_of(line, slot)].state == DESK_CALL_IDLE)
		return -ENOENT;

	*out = idx_of(line, slot);

	return 0;
}

int desk_dial(struct desk *d, unsigned int line, const char *number)
{
	int ret;

	if (line >= d->cfg.lines || !number || !number[0])
		return -EINVAL;

	if (d->lockout)
		return -EBUSY;

	if (d->focus != DESK_NONE &&
	    d->calls[d->focus].state != DESK_CALL_CONNECTED)
		return -EBUSY;

	if (d->focus != DESK_NONE)
		hold_send(d, d->focus);

	if (dialling(d))
		dial_call(d)->line = (uint8_t)line;
	else
		dial_prepare(d, line);

	if (d->path == HAL_AUDIO_NONE)
		path_set(d, HAL_AUDIO_SPEAKER);

	ret = dial_place(d, number);
	settle(d);

	return ret;
}

int desk_answer(struct desk *d, unsigned int line, unsigned int slot,
		enum hal_audio_path path)
{
	unsigned int i;

	if (call_index(d, line, slot, &i) ||
	    d->calls[i].state != DESK_CALL_RINGING)
		return -ENOENT;

	answer_call(d, i, path);
	settle(d);

	return 0;
}

int desk_reject(struct desk *d, unsigned int line, unsigned int slot)
{
	unsigned int i;

	if (call_index(d, line, slot, &i) ||
	    d->calls[i].state != DESK_CALL_RINGING)
		return -ENOENT;

	hangup_call(d, i);
	focus_lost(d);
	settle(d);

	return 0;
}

int desk_hangup(struct desk *d, unsigned int line, unsigned int slot)
{
	unsigned int i;

	if (call_index(d, line, slot, &i))
		return -ENOENT;

	hangup_call(d, i);
	focus_lost(d);
	settle(d);

	return 0;
}

int desk_hold(struct desk *d, unsigned int line, unsigned int slot)
{
	unsigned int i;

	if (call_index(d, line, slot, &i) ||
	    d->calls[i].state != DESK_CALL_CONNECTED)
		return -ENOENT;

	hold_send(d, i);
	settle(d);

	return 0;
}

int desk_resume(struct desk *d, unsigned int line, unsigned int slot)
{
	unsigned int i;

	if (call_index(d, line, slot, &i) ||
	    d->calls[i].state != DESK_CALL_HELD)
		return -ENOENT;

	dial_cancel(d);
	resume_send(d, i, false);
	if (d->path == HAL_AUDIO_NONE)
		path_set(d, HAL_AUDIO_SPEAKER);
	settle(d);

	return 0;
}

int desk_transfer(struct desk *d, unsigned int line, unsigned int slot,
		  const char *target)
{
	unsigned int i;

	if (!target || !target[0])
		return -EINVAL;

	if (call_index(d, line, slot, &i) || i == DESK_DIAL_IDX ||
	    (d->calls[i].state != DESK_CALL_CONNECTED &&
	     d->calls[i].state != DESK_CALL_HELD))
		return -ENOENT;

	if (d->focus == i)
		d->focus = DESK_NONE;
	d->calls[i].pending = DESK_PEND_XFER;
	d->ops->call_xfer(d->priv, line, slot, target);
	settle(d);

	return 0;
}

int desk_transfer_attended(struct desk *d, unsigned int line,
			   unsigned int slot, unsigned int slot2)
{
	unsigned int i, j;

	if (call_index(d, line, slot, &i) || call_index(d, line, slot2, &j) ||
	    i == j || i == DESK_DIAL_IDX || j == DESK_DIAL_IDX)
		return -ENOENT;

	d->ops->call_xfer_consult(d->priv, line, slot, slot2);
	call_clear(d, i, DESK_END_TRANSFERRED);
	call_clear(d, j, DESK_END_TRANSFERRED);
	focus_lost(d);
	settle(d);

	return 0;
}

int desk_conference(struct desk *d, unsigned int line, unsigned int slot,
		    unsigned int slot2)
{
	unsigned int i, j;

	if (call_index(d, line, slot, &i) || call_index(d, line, slot2, &j) ||
	    i == j || i == DESK_DIAL_IDX || j == DESK_DIAL_IDX)
		return -ENOENT;

	if (d->calls[i].state == DESK_CALL_HELD) {
		unsigned int t = i;

		i = j;
		j = t;
	}

	if (d->calls[i].state != DESK_CALL_CONNECTED ||
	    d->calls[j].state != DESK_CALL_HELD)
		return -EINVAL;

	d->focus = i;
	resume_send(d, j, true);
	settle(d);

	return 0;
}

int desk_dtmf(struct desk *d, char digit)
{
	if (d->focus == DESK_NONE && !dialling(d))
		return -ENOENT;

	digit_take(d, digit);
	settle(d);

	return 0;
}

int desk_mute_set(struct desk *d, bool on)
{
	if (on && d->focus == DESK_NONE)
		return -ENOENT;

	mute_apply(d, on);
	settle(d);

	return 0;
}

int desk_audio_set(struct desk *d, enum hal_audio_path path)
{
	if (path > HAL_AUDIO_HEADSET_SPEAKER)
		return -EINVAL;

	if (path == HAL_AUDIO_NONE)
		path_close(d);
	else
		path_open(d, path);
	settle(d);

	return 0;
}

int desk_volume_set(struct desk *d, unsigned int level)
{
	uint8_t *v;

	if (level > HAL_VOLUME_MAX)
		return -EINVAL;

	v = d->path == HAL_AUDIO_NONE ? &d->ring_volume : &d->volume[d->path];
	*v = (uint8_t)level;
	if (d->ops->volume)
		d->ops->volume(d->priv, level);
	notify_device(d);

	return 0;
}

int desk_ring_volume_set(struct desk *d, unsigned int level)
{
	if (level > HAL_VOLUME_MAX)
		return -EINVAL;

	d->ring_volume = (uint8_t)level;
	if (d->path == HAL_AUDIO_NONE && d->ops->volume)
		d->ops->volume(d->priv, level);
	notify_device(d);

	return 0;
}

unsigned int desk_ring_volume_get(const struct desk *d)
{
	return d->ring_volume;
}

int desk_ring_preview(struct desk *d, uint32_t duration_ms)
{
	if (!desk_quiet(d))
		return -EBUSY;

	preview_end(d);
	if (!d->ops->ring_preview)
		return -ENOTSUP;

	d->previewing = true;
	if (d->ops->volume)
		d->ops->volume(d->priv, d->ring_volume);
	d->ops->ring_preview(d->priv, true);
	sip_timer_arm(d->env, &d->t_preview, duration_ms);

	return 0;
}

void desk_ring_preview_stop(struct desk *d)
{
	preview_end(d);
}

int desk_dnd_set(struct desk *d, bool on)
{
	d->dnd = on;
	notify_device(d);

	return 0;
}

/* ------------------------------------------------------------------- names */

const char *desk_state_name(enum desk_state state)
{
	static const char *const names[] = {
		"idle", "dial", "alert", "active", "parked", "ended",
	};

	return state < sizeof(names) / sizeof(names[0]) ? names[state] :
							  "unknown";
}

const char *desk_call_state_name(enum desk_call_state state)
{
	static const char *const names[] = {
		"idle", "dialling", "calling", "ringback", "ringing",
		"connected", "held", "conference", "clearing",
	};

	return state < sizeof(names) / sizeof(names[0]) ? names[state] :
							  "unknown";
}

const char *desk_end_reason_name(enum desk_end_reason reason)
{
	static const char *const names[] = {
		"local", "remote_bye", "busy", "rejected", "unavailable",
		"failed", "cancelled", "transferred",
	};

	return reason < sizeof(names) / sizeof(names[0]) ? names[reason] :
							   "unknown";
}

const char *desk_path_name(enum hal_audio_path path)
{
	static const char *const names[] = {
		"none", "handset", "headset", "speaker", "group", "group",
	};

	return path < sizeof(names) / sizeof(names[0]) ? names[path] :
							 "unknown";
}
