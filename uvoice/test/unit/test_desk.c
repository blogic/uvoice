// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "proto/sip/desk.h"
#include "test_util.h"

#define FAKE_TIMER_MAX		8

static struct {
	struct sip_txn_env env;
	uint64_t now;
	struct sip_timer *timer[FAKE_TIMER_MAX];
	unsigned int timers;
	unsigned int fired;
} fk;

#include "test_clock.h"

static struct desk d;
static struct dialplan dp;
static char log_buf[4096];
static bool line_ready[DESK_LINES] = { true, true };

static void act(const char *fmt, ...)
{
	size_t len = strlen(log_buf);
	va_list ap;

	if (len + 2 >= sizeof(log_buf))
		return;

	va_start(ap, fmt);
	vsnprintf(log_buf + len, sizeof(log_buf) - len - 1, fmt, ap);
	va_end(ap);
	strcat(log_buf, " ");
}

static bool did(const char *needle)
{
	return strstr(log_buf, needle) != NULL;
}

static void log_reset(void)
{
	log_buf[0] = 0;
}

static bool op_slot_free(void *priv, unsigned int line, unsigned int *slot)
{
	unsigned int s;

	for (s = 0; s < DESK_SLOTS; s++) {
		if (desk_call_get(&d, line, s)->state == DESK_CALL_IDLE) {
			*slot = s;
			return true;
		}
	}

	return false;
}

static bool op_dial_ready(void *priv, unsigned int line)
{
	return line_ready[line];
}

static void op_call_dial(void *priv, unsigned int line, unsigned int slot,
			 const char *number)
{
	act("dial(%u,%u,%s)", line, slot, number);
}

static void op_call_ring(void *priv, unsigned int line, unsigned int slot)
{
	act("180(%u,%u)", line, slot);
}

static void op_call_answer(void *priv, unsigned int line, unsigned int slot)
{
	act("answer(%u,%u)", line, slot);
}

static void op_call_reject(void *priv, unsigned int line, unsigned int slot,
			   uint32_t code)
{
	act("reject(%u,%u,%u)", line, slot, code);
}

static void op_call_hangup(void *priv, unsigned int line, unsigned int slot)
{
	act("hangup(%u,%u)", line, slot);
}

static void op_call_hold(void *priv, unsigned int line, unsigned int slot)
{
	act("hold(%u,%u)", line, slot);
}

static void op_call_resume(void *priv, unsigned int line, unsigned int slot)
{
	act("resume(%u,%u)", line, slot);
}

static void op_call_dtmf(void *priv, unsigned int line, unsigned int slot,
			 char digit, uint16_t duration_ms)
{
	act("dtmf(%u,%u,%c)", line, slot, digit);
}

static void op_call_xfer(void *priv, unsigned int line, unsigned int slot,
			 const char *target)
{
	act("xfer(%u,%u,%s)", line, slot, target);
}

static void op_call_xfer_consult(void *priv, unsigned int line,
				 unsigned int referee, unsigned int replaced)
{
	act("consult(%u,%u,%u)", line, referee, replaced);
}

static void op_conf_set(void *priv, unsigned int line, bool on)
{
	act("conf(%u,%d)", line, on);
}

static void op_tone(void *priv, enum hal_tone tone)
{
	act("tone(%d)", tone);
}

static void op_key_tone(void *priv, char digit)
{
	act("key(%c)", digit);
}

static void op_ring_start(void *priv, unsigned int line, unsigned int slot)
{
	act("ring(%u,%u)", line, slot);
}

static void op_ring_stop(void *priv)
{
	act("ringstop");
}

static void op_ring_preview(void *priv, bool on)
{
	act("preview(%d)", on);
}

static void op_audio_path(void *priv, enum hal_audio_path path)
{
	act("path(%s)", desk_path_name(path));
}

static void op_volume(void *priv, unsigned int level)
{
	act("vol(%u)", level);
}

static void op_mute(void *priv, bool on)
{
	act("mute(%d)", on);
}

static void op_led(void *priv, enum hal_led led, enum hal_led_mode mode)
{
	act("led(%d,%d)", led, mode);
}

static void op_call_changed(void *priv, unsigned int line, unsigned int slot)
{
	const struct desk_call *c = desk_call_get(&d, line, slot);

	act("call(%u,%u,%s)", line, slot,
	    c ? desk_call_state_name(c->state) : "?");
}

static void op_device_changed(void *priv)
{
	act("device(%s)", desk_state_name(desk_state_get(&d)));
}

static uint32_t op_now_s(void *priv)
{
	return (uint32_t)(fk.now / 1000);
}

static const struct desk_ops ops = {
	.slot_free = op_slot_free,
	.dial_ready = op_dial_ready,
	.call_dial = op_call_dial,
	.call_ring = op_call_ring,
	.call_answer = op_call_answer,
	.call_reject = op_call_reject,
	.call_hangup = op_call_hangup,
	.call_hold = op_call_hold,
	.call_resume = op_call_resume,
	.call_dtmf = op_call_dtmf,
	.call_xfer = op_call_xfer,
	.call_xfer_consult = op_call_xfer_consult,
	.conf_set = op_conf_set,
	.tone = op_tone,
	.key_tone = op_key_tone,
	.ring_start = op_ring_start,
	.ring_stop = op_ring_stop,
	.ring_preview = op_ring_preview,
	.audio_path = op_audio_path,
	.volume = op_volume,
	.mute = op_mute,
	.led = op_led,
	.call_changed = op_call_changed,
	.device_changed = op_device_changed,
	.now_s = op_now_s,
};

static void reset(void)
{
	struct desk_cfg cfg = {
		.lines = 2,
		.interdigit_short_ms = 4000,
		.interdigit_long_ms = 16000,
		.start_digit_ms = 16000,
		.clearing_ms = 3000,
		.dtmf_ms = 100,
	};

	memset(&fk, 0, sizeof(fk));
	fk.now = 100000;
	fake_clock_bind(&fk.env);
	dialplan_compile("xxx", &dp);
	cfg.dp[0] = &dp;
	cfg.dp[1] = &dp;
	line_ready[0] = line_ready[1] = true;
	desk_init(&d, &fk.env, &cfg, &ops, NULL);
	log_reset();
}

static void ev(enum desk_ev e, unsigned int line, unsigned int slot,
	       uint32_t arg)
{
	desk_event_push(&d, e, line, slot, arg);
}

static void key(enum hal_key k)
{
	ev(DESK_EV_KEY, DESK_NONE, DESK_NONE, k);
}

static void digits(const char *s)
{
	while (*s)
		ev(DESK_EV_DIGIT, DESK_NONE, DESK_NONE, (unsigned char)*s++);
}

static enum desk_call_state st(unsigned int line, unsigned int slot)
{
	return desk_call_get(&d, line, slot)->state;
}

static void incoming(unsigned int line, unsigned int slot)
{
	desk_caller_set(&d, line, slot, "200", "Bob");
	ev(DESK_EV_CALL_INCOMING, line, slot, 0);
}

static void talk_out(void)
{
	ev(DESK_EV_HOOK_UP, DESK_NONE, DESK_NONE, 0);
	digits("123");
	ev(DESK_EV_CALL_ANSWERED, 0, 0, 0);
}

static void cases_idle(void)
{
	CASE("IDLE x hook up: handset path, dial tone, dialling call");
	reset();
	ev(DESK_EV_HOOK_UP, DESK_NONE, DESK_NONE, 0);
	CHECK(did("path(handset)"));
	CHECK(did("tone(1)"));
	CHECK(desk_state_get(&d) == DESK_DIAL);
	CHECK(desk_call_get(&d, 0, DESK_DIAL_SLOT)->state == DESK_CALL_DIALLING);

	CASE("IDLE x hook down: nothing");
	reset();
	ev(DESK_EV_HOOK_DOWN, DESK_NONE, DESK_NONE, 0);
	CHECK(!did("path("));
	CHECK(desk_state_get(&d) == DESK_IDLE);

	CASE("IDLE x speaker key: speaker path and dial tone");
	reset();
	key(HAL_KEY_SPEAKER);
	CHECK(did("path(speaker)"));
	CHECK(did("led(1,1)"));
	CHECK(desk_state_get(&d) == DESK_DIAL);

	CASE("IDLE x headset key: headset path and dial tone");
	reset();
	key(HAL_KEY_HEADSET);
	CHECK(did("path(headset)"));
	CHECK(did("led(2,1)"));
	CHECK(desk_state_get(&d) == DESK_DIAL);

	CASE("IDLE x digit: ignored");
	reset();
	digits("5");
	CHECK(!did("dial(") && !did("dtmf("));
	CHECK(desk_state_get(&d) == DESK_IDLE);

	CASE("IDLE x hold, transfer, conference, mute: ignored");
	reset();
	key(HAL_KEY_HOLD);
	key(HAL_KEY_TRANSFER);
	key(HAL_KEY_CONFERENCE);
	key(HAL_KEY_MUTE);
	CHECK(!did("hold(") && !did("mute(") && !did("conf("));
	CHECK(desk_state_get(&d) == DESK_IDLE);

	CASE("IDLE x volume up: ring volume");
	reset();
	key(HAL_KEY_VOL_UP);
	CHECK(did("vol(6)"));

	CASE("IDLE x DND key then incoming: rejected 486");
	reset();
	key(HAL_KEY_DND);
	incoming(0, 0);
	CHECK(did("reject(0,0,486)"));
	CHECK(st(0, 0) == DESK_CALL_IDLE);

	CASE("IDLE x incoming: 180, ringer, call LED blinks");
	reset();
	incoming(0, 0);
	CHECK(did("180(0,0)"));
	CHECK(did("ring(0,0)"));
	CHECK(did("led(4,3)"));
	CHECK(desk_state_get(&d) == DESK_ALERT);
	CHECK(!strcmp(desk_call_get(&d, 0, 0)->number, "200"));
}

static void cases_dial(void)
{
	CASE("DIAL x digits then #: dial now, without the #");
	reset();
	ev(DESK_EV_HOOK_UP, DESK_NONE, DESK_NONE, 0);
	log_reset();
	digits("12#");
	CHECK(did("key(#)"));
	CHECK(did("dial(0,0,12)"));
	CHECK(!did("tone(6)"));
	CHECK(st(0, 0) == DESK_CALL_CALLING);

	CASE("DIAL x # first: no digits to dial, reorder");
	reset();
	ev(DESK_EV_HOOK_UP, DESK_NONE, DESK_NONE, 0);
	log_reset();
	digits("#");
	CHECK(!did("dial("));
	CHECK(did("tone(6)"));

	CASE("DIAL x digits: dial plan match places the call");
	reset();
	ev(DESK_EV_HOOK_UP, DESK_NONE, DESK_NONE, 0);
	digits("12");
	CHECK(did("key(1)") && did("key(2)"));
	CHECK(!did("dial("));
	digits("3");
	CHECK(did("dial(0,0,123)"));
	CHECK(st(0, 0) == DESK_CALL_CALLING);
	CHECK(desk_state_get(&d) == DESK_ACTIVE);

	CASE("DIAL x hook down: cancel, path none");
	reset();
	ev(DESK_EV_HOOK_UP, DESK_NONE, DESK_NONE, 0);
	log_reset();
	ev(DESK_EV_HOOK_DOWN, DESK_NONE, DESK_NONE, 0);
	CHECK(did("path(none)"));
	CHECK(desk_state_get(&d) == DESK_IDLE);

	CASE("DIAL x speaker key (handset up): switch to speaker");
	reset();
	ev(DESK_EV_HOOK_UP, DESK_NONE, DESK_NONE, 0);
	log_reset();
	key(HAL_KEY_SPEAKER);
	CHECK(did("path(speaker)"));
	CHECK(desk_state_get(&d) == DESK_DIAL);

	CASE("DIAL on speaker x speaker key: cancel, path none");
	reset();
	key(HAL_KEY_SPEAKER);
	log_reset();
	key(HAL_KEY_SPEAKER);
	CHECK(did("path(none)"));
	CHECK(desk_state_get(&d) == DESK_IDLE);

	CASE("DIAL x start digit timeout: reorder, clearing, then idle");
	reset();
	key(HAL_KEY_SPEAKER);
	fake_advance(16000);
	CHECK(did("tone(6)"));
	fake_advance(3000);
	CHECK(desk_state_get(&d) == DESK_IDLE);
	CHECK(did("path(none)"));

	CASE("DIAL x incoming: call waiting tone, no ringer");
	reset();
	ev(DESK_EV_HOOK_UP, DESK_NONE, DESK_NONE, 0);
	log_reset();
	incoming(0, 0);
	CHECK(!did("ring(0,0)"));
	CHECK(desk_state_get(&d) == DESK_DIAL);

	CASE("DIAL x no free slot: reorder");
	reset();
	incoming(0, 0);
	incoming(0, 1);
	log_reset();
	CHECK(desk_dial(&d, 0, "123") < 0);
	CHECK(did("tone(6)"));
	CHECK(!did("dial("));
}

static void cases_alert(void)
{
	CASE("ALERT x hook up: answer on handset");
	reset();
	incoming(0, 0);
	log_reset();
	ev(DESK_EV_HOOK_UP, DESK_NONE, DESK_NONE, 0);
	CHECK(did("answer(0,0)"));
	CHECK(did("path(handset)"));
	CHECK(did("ringstop"));
	CHECK(st(0, 0) == DESK_CALL_CONNECTED);

	CASE("ALERT x speaker key: answer on speaker");
	reset();
	incoming(0, 0);
	key(HAL_KEY_SPEAKER);
	CHECK(did("answer(0,0)"));
	CHECK(did("path(speaker)"));

	CASE("ALERT x headset key: answer on headset");
	reset();
	incoming(0, 0);
	key(HAL_KEY_HEADSET);
	CHECK(did("answer(0,0)"));
	CHECK(did("path(headset)"));

	CASE("ALERT x caller cancels: idle, ringer stops");
	reset();
	incoming(0, 0);
	log_reset();
	ev(DESK_EV_CALL_REMOTE_CANCEL, 0, 0, 0);
	CHECK(did("ringstop"));
	CHECK(desk_state_get(&d) == DESK_IDLE);

	CASE("ALERT x ubus reject: 486, idle");
	reset();
	incoming(0, 0);
	CHECK(desk_reject(&d, 0, 0) == 0);
	CHECK(did("reject(0,0,486)"));
	CHECK(desk_state_get(&d) == DESK_IDLE);

	CASE("ALERT x ubus answer without path: speaker");
	reset();
	incoming(0, 0);
	CHECK(desk_answer(&d, 0, 0, HAL_AUDIO_NONE) == 0);
	CHECK(did("path(speaker)"));

	CASE("ALERT x volume key: ring volume");
	reset();
	incoming(0, 0);
	log_reset();
	key(HAL_KEY_VOL_DOWN);
	CHECK(did("vol(4)"));
}

static void cases_active(void)
{
	CASE("ACTIVE x ringback and answer");
	reset();
	ev(DESK_EV_HOOK_UP, DESK_NONE, DESK_NONE, 0);
	digits("123");
	ev(DESK_EV_CALL_PROCEEDING, 0, 0, 0);
	CHECK(did("tone(4)"));
	CHECK(st(0, 0) == DESK_CALL_RINGBACK);
	ev(DESK_EV_CALL_ANSWERED, 0, 0, 0);
	CHECK(st(0, 0) == DESK_CALL_CONNECTED);

	CASE("ACTIVE x busy: busy tone, then silence on the handset");
	reset();
	ev(DESK_EV_HOOK_UP, DESK_NONE, DESK_NONE, 0);
	digits("123");
	ev(DESK_EV_CALL_FAILED, 0, 0, DESK_FAIL_BUSY);
	CHECK(did("tone(5)"));
	CHECK(st(0, 0) == DESK_CALL_CLEARING);
	log_reset();
	fake_advance(3000);
	CHECK(st(0, 0) == DESK_CALL_IDLE);
	CHECK(desk_state_get(&d) == DESK_ENDED);
	CHECK(did("tone(0)"));
	CHECK(!did("tone(1)"));
	CHECK(!did("path("));

	CASE("ACTIVE x hook down: hang up, path none");
	reset();
	talk_out();
	log_reset();
	ev(DESK_EV_HOOK_DOWN, DESK_NONE, DESK_NONE, 0);
	CHECK(did("hangup(0,0)"));
	CHECK(did("path(none)"));
	CHECK(desk_state_get(&d) == DESK_IDLE);

	CASE("ACTIVE on handset x speaker key: move to speaker");
	reset();
	talk_out();
	log_reset();
	key(HAL_KEY_SPEAKER);
	CHECK(did("path(speaker)"));
	CHECK(!did("hangup("));
	log_reset();
	ev(DESK_EV_HOOK_DOWN, DESK_NONE, DESK_NONE, 0);
	CHECK(!did("hangup("));
	CHECK(st(0, 0) == DESK_CALL_CONNECTED);

	CASE("ACTIVE on speaker x hook up: move to handset");
	reset();
	key(HAL_KEY_SPEAKER);
	digits("123");
	ev(DESK_EV_CALL_ANSWERED, 0, 0, 0);
	log_reset();
	ev(DESK_EV_HOOK_UP, DESK_NONE, DESK_NONE, 0);
	CHECK(did("path(handset)"));

	CASE("ACTIVE x digit: DTMF and key tone");
	reset();
	talk_out();
	log_reset();
	digits("5");
	CHECK(did("dtmf(0,0,5)"));
	CHECK(did("key(5)"));

	CASE("ACTIVE x mute key: toggles, LED, ends with the call");
	reset();
	talk_out();
	key(HAL_KEY_MUTE);
	CHECK(did("mute(1)"));
	CHECK(did("led(0,1)"));
	log_reset();
	ev(DESK_EV_HOOK_DOWN, DESK_NONE, DESK_NONE, 0);
	CHECK(did("mute(0)"));

	CASE("ACTIVE x volume up: path volume");
	reset();
	talk_out();
	log_reset();
	key(HAL_KEY_VOL_UP);
	CHECK(did("vol(6)"));

	CASE("ACTIVE x far end BYE: busy tone, then silence on the handset");
	reset();
	talk_out();
	log_reset();
	ev(DESK_EV_CALL_REMOTE_BYE, 0, 0, 0);
	CHECK(did("tone(5)"));
	CHECK(st(0, 0) == DESK_CALL_CLEARING);
	log_reset();
	fake_advance(3000);
	CHECK(desk_state_get(&d) == DESK_ENDED);
	CHECK(did("device(ended)"));
	CHECK(!did("tone(1)"));
	CHECK(!did("call(0,2,dialling)"));

	CASE("silence after a call x digits, speaker, headset, dial: ignored");
	log_reset();
	digits("5");
	key(HAL_KEY_SPEAKER);
	key(HAL_KEY_HEADSET);
	CHECK(desk_dial(&d, 0, "999") == -EBUSY);
	CHECK(!did("tone(1)") && !did("dial(") && !did("dtmf(") &&
	      !did("path("));
	CHECK(desk_state_get(&d) == DESK_ENDED);

	CASE("silence after a call x incoming: answer on the speaker key");
	incoming(0, 1);
	log_reset();
	key(HAL_KEY_SPEAKER);
	CHECK(did("answer(0,1)"));
	CHECK(st(0, 1) == DESK_CALL_CONNECTED);
	ev(DESK_EV_CALL_REMOTE_BYE, 0, 1, 0);
	fake_advance(3000);
	CHECK(desk_state_get(&d) == DESK_ENDED);

	CASE("silence after a call x hook down, hook up: dial tone again");
	reset();
	talk_out();
	ev(DESK_EV_CALL_REMOTE_BYE, 0, 0, 0);
	fake_advance(3000);
	log_reset();
	ev(DESK_EV_HOOK_DOWN, DESK_NONE, DESK_NONE, 0);
	CHECK(did("path(none)"));
	CHECK(did("device(idle)"));
	CHECK(desk_state_get(&d) == DESK_IDLE);
	log_reset();
	ev(DESK_EV_HOOK_UP, DESK_NONE, DESK_NONE, 0);
	CHECK(did("tone(1)"));
	CHECK(desk_state_get(&d) == DESK_DIAL);

	CASE("ACTIVE on speaker x far end BYE: path none after clearing");
	reset();
	key(HAL_KEY_SPEAKER);
	digits("123");
	ev(DESK_EV_CALL_ANSWERED, 0, 0, 0);
	ev(DESK_EV_CALL_REMOTE_BYE, 0, 0, 0);
	log_reset();
	fake_advance(3000);
	CHECK(did("path(none)"));
	CHECK(desk_state_get(&d) == DESK_IDLE);

	CASE("ACTIVE x incoming: call waiting tone, no ringer");
	reset();
	talk_out();
	log_reset();
	incoming(0, 1);
	CHECK(did("tone(8)"));
	CHECK(!did("ring(0,1)"));

	CASE("ACTIVE x ubus answer of the waiting call: hold, answer");
	reset();
	talk_out();
	incoming(0, 1);
	log_reset();
	CHECK(desk_answer(&d, 0, 1, HAL_AUDIO_NONE) == 0);
	CHECK(did("hold(0,0)"));
	CHECK(did("answer(0,1)"));
	CHECK(did("call(0,0,connected)"));
	ev(DESK_EV_CALL_REINVITE_OK, 0, 0, 0);
	CHECK(st(0, 0) == DESK_CALL_HELD);
	CHECK(st(0, 1) == DESK_CALL_CONNECTED);
}

static void cases_hold(void)
{
	uint32_t connected;

	CASE("ACTIVE x hold key: hold, PARKED on the OK");
	reset();
	talk_out();
	log_reset();
	key(HAL_KEY_HOLD);
	CHECK(did("hold(0,0)"));
	CHECK(did("call(0,0,connected)"));
	ev(DESK_EV_CALL_REINVITE_OK, 0, 0, 0);
	CHECK(st(0, 0) == DESK_CALL_HELD);
	CHECK(desk_state_get(&d) == DESK_PARKED);

	CASE("PARKED x hold key: resume");
	connected = desk_call_get(&d, 0, 0)->connected;
	CHECK(connected != 0);
	fake_advance(5000);
	log_reset();
	key(HAL_KEY_HOLD);
	CHECK(did("resume(0,0)"));
	ev(DESK_EV_CALL_REINVITE_OK, 0, 0, 0);
	CHECK(st(0, 0) == DESK_CALL_CONNECTED);
	CHECK(desk_state_get(&d) == DESK_ACTIVE);
	CHECK(desk_call_get(&d, 0, 0)->connected == connected);
	CHECK(desk_call_get(&d, 0, 0)->since == connected + 5);

	CASE("ACTIVE x hold refused: stays connected");
	reset();
	talk_out();
	key(HAL_KEY_HOLD);
	ev(DESK_EV_CALL_REINVITE_FAIL, 0, 0, 0);
	CHECK(st(0, 0) == DESK_CALL_CONNECTED);
	CHECK(desk_state_get(&d) == DESK_ACTIVE);

	CASE("PARKED x hook down: held call stays");
	reset();
	talk_out();
	key(HAL_KEY_HOLD);
	ev(DESK_EV_CALL_REINVITE_OK, 0, 0, 0);
	log_reset();
	ev(DESK_EV_HOOK_DOWN, DESK_NONE, DESK_NONE, 0);
	CHECK(!did("hangup("));
	CHECK(st(0, 0) == DESK_CALL_HELD);

	CASE("PARKED x hook up: new dial");
	reset();
	talk_out();
	key(HAL_KEY_HOLD);
	ev(DESK_EV_CALL_REINVITE_OK, 0, 0, 0);
	ev(DESK_EV_HOOK_DOWN, DESK_NONE, DESK_NONE, 0);
	log_reset();
	ev(DESK_EV_HOOK_UP, DESK_NONE, DESK_NONE, 0);
	CHECK(desk_state_get(&d) == DESK_DIAL);

	CASE("PARKED x held call cleared by the far end: idle");
	reset();
	talk_out();
	key(HAL_KEY_HOLD);
	ev(DESK_EV_CALL_REINVITE_OK, 0, 0, 0);
	ev(DESK_EV_HOOK_DOWN, DESK_NONE, DESK_NONE, 0);
	ev(DESK_EV_CALL_REMOTE_BYE, 0, 0, 0);
	CHECK(st(0, 0) == DESK_CALL_IDLE);
	CHECK(desk_state_get(&d) == DESK_IDLE);
}

static void cases_xfer_conf(void)
{
	CASE("ACTIVE x transfer key, digits, transfer key: blind transfer");
	reset();
	talk_out();
	log_reset();
	key(HAL_KEY_TRANSFER);
	CHECK(did("hold(0,0)"));
	CHECK(desk_state_get(&d) == DESK_DIAL);
	ev(DESK_EV_CALL_REINVITE_OK, 0, 0, 0);
	digits("45");
	key(HAL_KEY_TRANSFER);
	CHECK(did("xfer(0,0,45)"));
	ev(DESK_EV_CALL_XFER_OK, 0, 0, 0);
	CHECK(did("hangup(0,0)"));
	CHECK(st(0, 0) == DESK_CALL_IDLE);

	CASE("ACTIVE x transfer key, consult call, transfer key: attended");
	reset();
	talk_out();
	key(HAL_KEY_TRANSFER);
	ev(DESK_EV_CALL_REINVITE_OK, 0, 0, 0);
	digits("456");
	CHECK(did("dial(0,1,456)"));
	ev(DESK_EV_CALL_ANSWERED, 0, 1, 0);
	log_reset();
	key(HAL_KEY_TRANSFER);
	CHECK(did("consult(0,0,1)"));
	CHECK(st(0, 0) == DESK_CALL_IDLE && st(0, 1) == DESK_CALL_IDLE);

	CASE("ACTIVE x ubus transfer refused: the call stays active");
	reset();
	talk_out();
	CHECK(desk_transfer(&d, 0, 0, "77") == 0);
	CHECK(did("xfer(0,0,77)"));
	ev(DESK_EV_CALL_XFER_FAIL, 0, 0, 0);
	CHECK(!did("resume(0,0)"));
	CHECK(st(0, 0) == DESK_CALL_CONNECTED);
	CHECK(desk_state_get(&d) == DESK_ACTIVE);

	CASE("ACTIVE x blind transfer by key refused: held call resumes");
	reset();
	talk_out();
	key(HAL_KEY_TRANSFER);
	ev(DESK_EV_CALL_REINVITE_OK, 0, 0, 0);
	digits("45");
	key(HAL_KEY_TRANSFER);
	log_reset();
	ev(DESK_EV_CALL_XFER_FAIL, 0, 0, 0);
	CHECK(did("resume(0,0)"));

	CASE("ACTIVE + HELD x conference key: resume, join, conference");
	reset();
	talk_out();
	incoming(0, 1);
	desk_answer(&d, 0, 1, HAL_AUDIO_NONE);
	ev(DESK_EV_CALL_REINVITE_OK, 0, 0, 0);
	log_reset();
	key(HAL_KEY_CONFERENCE);
	CHECK(did("resume(0,0)"));
	ev(DESK_EV_CALL_REINVITE_OK, 0, 0, 0);
	CHECK(did("conf(0,1)"));
	CHECK(st(0, 0) == DESK_CALL_CONFERENCE);
	CHECK(st(0, 1) == DESK_CALL_CONFERENCE);

	CASE("CONFERENCE x one far end leaves: the other stays connected");
	log_reset();
	ev(DESK_EV_CALL_REMOTE_BYE, 0, 0, 0);
	CHECK(did("conf(0,0)"));
	CHECK(st(0, 1) == DESK_CALL_CONNECTED);
	CHECK(desk_state_get(&d) == DESK_ACTIVE);

	CASE("CONFERENCE x hook down: both legs hung up");
	reset();
	talk_out();
	incoming(0, 1);
	desk_answer(&d, 0, 1, HAL_AUDIO_HANDSET);
	ev(DESK_EV_CALL_REINVITE_OK, 0, 0, 0);
	key(HAL_KEY_CONFERENCE);
	ev(DESK_EV_CALL_REINVITE_OK, 0, 0, 0);
	log_reset();
	ev(DESK_EV_HOOK_DOWN, DESK_NONE, DESK_NONE, 0);
	CHECK(did("hangup(0,0)") && did("hangup(0,1)"));
	CHECK(desk_state_get(&d) == DESK_IDLE);
}

static void cases_verbs(void)
{
	CASE("ubus dial while idle: speaker path, call placed");
	reset();
	CHECK(desk_dial(&d, 1, "999") == 0);
	CHECK(did("path(speaker)"));
	CHECK(did("dial(1,0,999)"));
	CHECK(st(1, 0) == DESK_CALL_CALLING);
	CHECK(!did("call(1,2,dialling)"));
	CHECK(!did("tone(1)"));

	CASE("ubus dial while the focused call is calling: -EBUSY, one call");
	reset();
	CHECK(desk_dial(&d, 0, "999") == 0);
	log_reset();
	CHECK(desk_dial(&d, 0, "999") == -EBUSY);
	CHECK(!did("dial(") && !did("hold("));
	CHECK(st(0, 0) == DESK_CALL_CALLING);
	CHECK(st(0, 1) == DESK_CALL_IDLE);

	CASE("ubus dial after hook up: the dialling call, on the handset");
	reset();
	ev(DESK_EV_HOOK_UP, DESK_NONE, DESK_NONE, 0);
	{
		uint32_t id = desk_call_get(&d, 0, DESK_DIAL_SLOT)->id;

		log_reset();
		CHECK(desk_dial(&d, 0, "999") == 0);
		CHECK(did("dial(0,0,999)"));
		CHECK(!did("path("));
		CHECK(desk_call_get(&d, 0, 0)->id == id);
		CHECK(desk_state_get(&d) == DESK_ACTIVE);
	}

	CASE("ubus dial keeps the call id of the dialling record");
	reset();
	ev(DESK_EV_HOOK_UP, DESK_NONE, DESK_NONE, 0);
	{
		uint32_t id = desk_call_get(&d, 0, DESK_DIAL_SLOT)->id;

		digits("123");
		CHECK(desk_call_get(&d, 0, 0)->id == id);
	}

	CASE("ubus hangup of a connected call on speaker: path none");
	reset();
	CHECK(desk_dial(&d, 0, "999") == 0);
	ev(DESK_EV_CALL_ANSWERED, 0, 0, 0);
	CHECK(desk_hangup(&d, 0, 0) == 0);
	CHECK(did("hangup(0,0)"));
	CHECK(desk_state_get(&d) == DESK_IDLE);

	CASE("ubus hold and resume");
	reset();
	talk_out();
	CHECK(desk_hold(&d, 0, 0) == 0);
	ev(DESK_EV_CALL_REINVITE_OK, 0, 0, 0);
	CHECK(st(0, 0) == DESK_CALL_HELD);
	CHECK(desk_resume(&d, 0, 0) == 0);
	ev(DESK_EV_CALL_REINVITE_OK, 0, 0, 0);
	CHECK(st(0, 0) == DESK_CALL_CONNECTED);

	CASE("ubus verbs on an unknown call: -ENOENT");
	reset();
	CHECK(desk_hangup(&d, 0, 1) < 0);
	CHECK(desk_answer(&d, 0, 0, HAL_AUDIO_NONE) < 0);
	CHECK(desk_hold(&d, 1, 1) < 0);

	CASE("ubus mute only with a focused call");
	reset();
	CHECK(desk_mute_set(&d, true) < 0);
	talk_out();
	CHECK(desk_mute_set(&d, true) == 0);
	CHECK(did("mute(1)"));

	CASE("ubus audio and volume");
	reset();
	talk_out();
	CHECK(desk_audio_set(&d, HAL_AUDIO_HEADSET) == 0);
	CHECK(did("path(headset)"));
	CHECK(desk_volume_set(&d, 8) == 0);
	CHECK(did("vol(8)"));
	CHECK(desk_volume_set(&d, 9) < 0);

	CASE("ring volume: set during a call, pushed only at idle");
	reset();
	talk_out();
	log_reset();
	CHECK(desk_ring_volume_set(&d, 2) == 0);
	CHECK(!did("vol("));
	CHECK(desk_ring_volume_get(&d) == 2);
	CHECK(desk_ring_volume_set(&d, 9) == -EINVAL);
	reset();
	CHECK(desk_ring_volume_set(&d, 3) == 0);
	CHECK(did("vol(3)"));

	CASE("ring preview at idle plays at the ring volume, then ends");
	reset();
	desk_ring_volume_set(&d, 4);
	log_reset();
	CHECK(desk_ring_preview(&d, 3000) == 0);
	CHECK(did("vol(4)"));
	CHECK(did("preview(1)"));
	log_reset();
	fake_advance(3000);
	CHECK(did("preview(0)"));

	CASE("ring preview: a new preview replaces the running one");
	reset();
	CHECK(desk_ring_preview(&d, 3000) == 0);
	log_reset();
	CHECK(desk_ring_preview(&d, 3000) == 0);
	CHECK(did("preview(0) vol(5) preview(1)"));
	desk_ring_preview_stop(&d);
	CHECK(did("preview(0) vol(5) preview(1) preview(0)"));

	CASE("ring preview refused when the phone is not idle");
	reset();
	talk_out();
	CHECK(desk_ring_preview(&d, 3000) == -EBUSY);
	reset();
	incoming(0, 0);
	CHECK(desk_ring_preview(&d, 3000) == -EBUSY);

	CASE("ring preview ends at hook up and at an incoming call");
	reset();
	CHECK(desk_ring_preview(&d, 3000) == 0);
	log_reset();
	ev(DESK_EV_HOOK_UP, DESK_NONE, DESK_NONE, 0);
	CHECK(did("preview(0)"));
	reset();
	CHECK(desk_ring_preview(&d, 3000) == 0);
	log_reset();
	incoming(0, 0);
	CHECK(did("preview(0)"));
	CHECK(did("ring(0,0)"));
	log_reset();
	fake_advance(3000);
	CHECK(!did("preview(") && !did("ringstop"));

	CASE("MWI: stutter dial tone and message LED");
	reset();
	desk_mwi_set(&d, true);
	CHECK(did("led(3,2)"));
	ev(DESK_EV_HOOK_UP, DESK_NONE, DESK_NONE, 0);
	CHECK(did("tone(2)"));

	CASE("missed calls: the message LED beats ahead of MWI");
	reset();
	desk_mwi_set(&d, true);
	log_reset();
	desk_missed_set(&d, true);
	CHECK(did("led(3,4)"));
	log_reset();
	desk_missed_set(&d, false);
	CHECK(did("led(3,2)"));
	desk_mwi_set(&d, false);
	CHECK(did("led(3,0)"));
}

int main(void)
{
	cases_idle();
	cases_dial();
	cases_alert();
	cases_active();
	cases_hold();
	cases_xfer_conf();
	cases_verbs();

	return tu_summary("test_desk");
}
