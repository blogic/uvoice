// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#ifndef UVOICE_SIP_DESK_H
#define UVOICE_SIP_DESK_H

#include <stdbool.h>
#include <stdint.h>

#include "dialplan.h"
#include "hal/hal.h"
#include "proto/sip/txn.h"

#define DESK_LINES		2
#define DESK_SLOTS		2
#define DESK_CALLS		(DESK_LINES * DESK_SLOTS)
#define DESK_DIAL_SLOT		DESK_SLOTS
#define DESK_LEDS		(HAL_LED_CALL + 1)
#define DESK_NONE		255u
#define DESK_DIGITS_MAX		DIALPLAN_DIGITS_MAX
#define DESK_NAME_MAX		64
#define DESK_VOLUME_DEFAULT	5
#define DESK_REJECT_BUSY	486

enum desk_state {
	DESK_IDLE = 0,
	DESK_DIAL,
	DESK_ALERT,
	DESK_ACTIVE,
	DESK_PARKED,
	DESK_ENDED,
};

enum desk_call_state {
	DESK_CALL_IDLE = 0,
	DESK_CALL_DIALLING,
	DESK_CALL_CALLING,
	DESK_CALL_RINGBACK,
	DESK_CALL_RINGING,
	DESK_CALL_CONNECTED,
	DESK_CALL_HELD,
	DESK_CALL_CONFERENCE,
	DESK_CALL_CLEARING,
};

enum desk_end_reason {
	DESK_END_LOCAL = 0,
	DESK_END_REMOTE,
	DESK_END_BUSY,
	DESK_END_REJECTED,
	DESK_END_UNAVAILABLE,
	DESK_END_FAILED,
	DESK_END_CANCELLED,
	DESK_END_TRANSFERRED,
};

enum desk_ev {
	DESK_EV_HOOK_UP = 0,
	DESK_EV_HOOK_DOWN,
	DESK_EV_KEY,
	DESK_EV_DIGIT,
	DESK_EV_CALL_INCOMING,
	DESK_EV_CALL_PROCEEDING,
	DESK_EV_CALL_ANSWERED,
	DESK_EV_CALL_FAILED,
	DESK_EV_CALL_REMOTE_BYE,
	DESK_EV_CALL_REMOTE_CANCEL,
	DESK_EV_CALL_REINVITE_OK,
	DESK_EV_CALL_REINVITE_FAIL,
	DESK_EV_CALL_XFER_OK,
	DESK_EV_CALL_XFER_FAIL,
	DESK_EV_CALL_ENDED,
	DESK_EV_T_DIGIT,
	DESK_EV_T_CLEARING,
	DESK_EV_T_PREVIEW,
	DESK_EV_COUNT,
};

enum desk_fail {
	DESK_FAIL_BUSY = 0,
	DESK_FAIL_REJECTED,
	DESK_FAIL_UNAVAILABLE,
	DESK_FAIL_OTHER,
};

enum desk_pending {
	DESK_PEND_NONE = 0,
	DESK_PEND_HOLD,
	DESK_PEND_RESUME,
	DESK_PEND_XFER,
};

struct desk_call {
	uint32_t id;
	uint8_t line;
	uint8_t state;
	uint8_t pending;
	uint8_t end_reason;
	bool inbound;
	bool early_media;
	bool conf_join;
	uint32_t since;
	uint32_t connected;
	char number[DESK_DIGITS_MAX + 1];
	char name[DESK_NAME_MAX];
};

struct desk_ops {
	bool (*slot_free)(void *priv, unsigned int line, unsigned int *slot);
	bool (*dial_ready)(void *priv, unsigned int line);
	void (*call_dial)(void *priv, unsigned int line, unsigned int slot,
			  const char *number);
	void (*call_ring)(void *priv, unsigned int line, unsigned int slot);
	void (*call_answer)(void *priv, unsigned int line, unsigned int slot);
	void (*call_reject)(void *priv, unsigned int line, unsigned int slot,
			    uint32_t code);
	void (*call_hangup)(void *priv, unsigned int line, unsigned int slot);
	void (*call_hold)(void *priv, unsigned int line, unsigned int slot);
	void (*call_resume)(void *priv, unsigned int line, unsigned int slot);
	void (*call_dtmf)(void *priv, unsigned int line, unsigned int slot,
			  char digit, uint16_t duration_ms);
	void (*call_xfer)(void *priv, unsigned int line, unsigned int slot,
			  const char *target);
	void (*call_xfer_consult)(void *priv, unsigned int line,
				  unsigned int referee, unsigned int replaced);
	void (*conf_set)(void *priv, unsigned int line, bool on);
	void (*tone)(void *priv, enum hal_tone tone);
	void (*key_tone)(void *priv, char digit);
	void (*ring_start)(void *priv, unsigned int line, unsigned int slot);
	void (*ring_stop)(void *priv);
	void (*ring_preview)(void *priv, bool on);
	void (*audio_path)(void *priv, enum hal_audio_path path);
	void (*volume)(void *priv, unsigned int level);
	void (*mute)(void *priv, bool on);
	void (*led)(void *priv, enum hal_led led, enum hal_led_mode mode);
	void (*call_changed)(void *priv, unsigned int line, unsigned int slot);
	void (*device_changed)(void *priv);
	uint32_t (*now_s)(void *priv);
};

struct desk_cfg {
	const struct dialplan *dp[DESK_LINES];
	unsigned int lines;
	uint32_t interdigit_short_ms;
	uint32_t interdigit_long_ms;
	uint32_t start_digit_ms;
	uint32_t clearing_ms;
	uint16_t dtmf_ms;
};

struct desk {
	struct desk_call calls[DESK_CALLS + 1];
	uint32_t next_id;
	unsigned int focus;
	unsigned int focus_sent;
	unsigned int xfer_from;
	enum hal_audio_path path;
	bool hook_up;
	bool lockout;
	bool mute;
	bool dnd;
	bool mwi;
	bool missed;
	bool ringing;
	bool previewing;
	enum hal_tone tone;
	uint8_t leds[DESK_LEDS];
	uint8_t volume[HAL_AUDIO_HEADSET_SPEAKER + 1];
	uint8_t ring_volume;
	char digits[DESK_DIGITS_MAX + 1];
	unsigned int ndigits;
	struct sip_timer t_digit;
	struct sip_timer t_clearing;
	struct sip_timer t_preview;
	unsigned int clearing_call;
	struct sip_txn_env *env;
	struct desk_cfg cfg;
	const struct desk_ops *ops;
	void *priv;
};

void desk_init(struct desk *d, struct sip_txn_env *env,
	       const struct desk_cfg *cfg, const struct desk_ops *ops,
	       void *priv);
void desk_cfg_update(struct desk *d, const struct desk_cfg *cfg);
void desk_timers_stop(struct desk *d);
void desk_event_push(struct desk *d, enum desk_ev ev, unsigned int line,
		     unsigned int slot, uint32_t arg);
void desk_caller_set(struct desk *d, unsigned int line, unsigned int slot,
		     const char *number, const char *name);
void desk_mwi_set(struct desk *d, bool on);

/**
 * desk_missed_set() - mark that missed calls wait for the user
 * @d: the desk
 * @on: true while a missed call is unseen
 *
 * The message LED beats while @on is true, ahead of the message waiting blink.
 */
void desk_missed_set(struct desk *d, bool on);

enum desk_state desk_state_get(const struct desk *d);
const struct desk_call *desk_call_get(const struct desk *d, unsigned int line,
				      unsigned int slot);
const char *desk_state_name(enum desk_state state);
const char *desk_call_state_name(enum desk_call_state state);
const char *desk_end_reason_name(enum desk_end_reason reason);
const char *desk_path_name(enum hal_audio_path path);

int desk_dial(struct desk *d, unsigned int line, const char *number);
int desk_answer(struct desk *d, unsigned int line, unsigned int slot,
		enum hal_audio_path path);
int desk_reject(struct desk *d, unsigned int line, unsigned int slot);
int desk_hangup(struct desk *d, unsigned int line, unsigned int slot);
int desk_hold(struct desk *d, unsigned int line, unsigned int slot);
int desk_resume(struct desk *d, unsigned int line, unsigned int slot);
int desk_transfer(struct desk *d, unsigned int line, unsigned int slot,
		  const char *target);
int desk_transfer_attended(struct desk *d, unsigned int line,
			   unsigned int slot, unsigned int slot2);
int desk_conference(struct desk *d, unsigned int line, unsigned int slot,
		    unsigned int slot2);
int desk_dtmf(struct desk *d, char digit);
int desk_mute_set(struct desk *d, bool on);
int desk_audio_set(struct desk *d, enum hal_audio_path path);
int desk_volume_set(struct desk *d, unsigned int level);

/**
 * desk_ring_volume_set() - set the ring volume
 * @d: the desk
 * @level: the volume, 0 to HAL_VOLUME_MAX
 *
 * Sets the ring volume whatever the audio path is. The backend gets it at once
 * only while no path is open.
 *
 * Return: 0, or -EINVAL for a level above HAL_VOLUME_MAX.
 */
int desk_ring_volume_set(struct desk *d, unsigned int level);

/**
 * desk_ring_volume_get() - read the ring volume
 * @d: the desk
 *
 * Return: the ring volume, 0 to HAL_VOLUME_MAX.
 */
unsigned int desk_ring_volume_get(const struct desk *d);

/**
 * desk_ring_preview() - play the ring for a time
 * @d: the desk
 * @duration_ms: how long the ring plays
 *
 * Plays the ring at the ring volume. A new preview replaces a running one.
 * The preview ends when the phone leaves idle.
 *
 * Return: 0, or -EBUSY if the phone is not idle.
 */
int desk_ring_preview(struct desk *d, uint32_t duration_ms);

/**
 * desk_ring_preview_stop() - stop a ring preview
 * @d: the desk
 */
void desk_ring_preview_stop(struct desk *d);
int desk_dnd_set(struct desk *d, bool on);

#endif
