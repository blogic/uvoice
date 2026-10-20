// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Name tables the udsp tools share: the event types, hook states, line
 * states, detected tones and their directions, passthrough kinds and
 * canceller policy reasons the tools print, the ring end reasons, the tones
 * their --tone options accept, and the caller identity formats their
 * --cid-format options accept. The tone and format strings are the spelling
 * the region files use (line/region.c), so a name a tool accepts is a name a
 * region file can carry. The default spidev node of their board options is
 * here too.
 */
#ifndef UDSP_TOOL_NAMES_H
#define UDSP_TOOL_NAMES_H

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "udsp/event.h"
#include "udsp/udsp.h"

#include "../line/line.h"

/* Every voice board enables exactly one SPI master, and the board device trees
 * carry no spi alias, so the SLIC is spidev 0.0 on each of them. */
#define TOOL_SPIDEV_DEFAULT	"/dev/spidev0.0"

static inline bool tool_num_parse(const char *text, long min, long max,
				  long *out)
{
	char *end = NULL;
	long value;

	errno = 0;
	value = strtol(text, &end, 10);
	if (errno || end == text || *end)
		return false;
	if (value < min || value > max)
		return false;
	*out = value;

	return true;
}

static inline uint64_t tool_mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static inline const char *tool_event_name(uint16_t type)
{
	static const char *const names[] = {
		"NONE", "HOOK", "DIGIT", "TONE", "LINE", "RING_START",
		"RING_END", "CID_SENT", "CID_RECV", "CID2_TIMEOUT",
		"RTP_FIRST_RX", "RTP_FIRST_TX", "JB_UPDATE", "T38",
		"SLIC_FAULT", "AUDIO_END", "JOB_DONE", "LEC_POLICY",
	};

	if (type >= sizeof(names) / sizeof(names[0]))
		return "UNKNOWN";

	return names[type];
}

static inline const char *tool_hook_name(uint8_t state)
{
	static const char *const names[] = {
		"SEIZE", "RELEASE", "FLASH", "PULSE", "ERROR",
	};

	if (state >= sizeof(names) / sizeof(names[0]))
		return "UNKNOWN";

	return names[state];
}

static inline const char *tool_line_state_name(uint8_t state)
{
	static const char *const names[] = {
		"DOWN", "ACTIVE", "OHT", "BUSY", "SLEEP", "HOWLER",
	};

	if (state >= sizeof(names) / sizeof(names[0]))
		return "UNKNOWN";

	return names[state];
}

static inline const char *tool_tone_detect_name(uint16_t code)
{
	static const char *const names[] = {
		"OTHER", "CNG", "CED", "ANS", "ANSAM", "ANSBAR",
		"V21_PREAMBLE", "FAX_CM", "FAX_TIMEOUT", "MODEM",
	};

	if (code >= sizeof(names) / sizeof(names[0]))
		return "UNKNOWN";

	return names[code];
}

static inline const char *tool_tone_direction_name(uint8_t direction)
{
	if (direction == UDSP_TONE_DIR_LOCAL)
		return "LOCAL";
	if (direction == UDSP_TONE_DIR_REMOTE)
		return "REMOTE";

	return "UNKNOWN";
}

static inline const char *tool_passthrough_name(uint8_t kind)
{
	static const char *const names[] = {
		"OFF", "MODEM", "FAX",
	};

	if (kind >= sizeof(names) / sizeof(names[0]))
		return "UNKNOWN";

	return names[kind];
}

static inline const char *tool_lec_reason_name(uint8_t reason)
{
	static const char *const names[] = {
		"LATCH", "DEMOTE", "RELEASE_QUIET", "RELEASE_TOPOLOGY",
		"RELEASE_CALLER",
	};

	if (reason >= sizeof(names) / sizeof(names[0]))
		return "UNKNOWN";

	return names[reason];
}

static inline bool tool_tone_parse(const char *name, enum udsp_tone *out)
{
	static const struct {
		const char *name;
		enum udsp_tone tone;
	} tones[] = {
		{ "dial", UDSP_TONE_DIAL },
		{ "stutter", UDSP_TONE_STUTTER },
		{ "second_dial", UDSP_TONE_SECOND_DIAL },
		{ "ringback", UDSP_TONE_RINGBACK },
		{ "busy", UDSP_TONE_BUSY },
		{ "reorder", UDSP_TONE_REORDER },
		{ "sit", UDSP_TONE_SIT },
		{ "call_waiting", UDSP_TONE_CALL_WAITING },
		{ "confirm", UDSP_TONE_CONFIRM },
		{ "howler", UDSP_TONE_HOWLER },
		{ "short_stutter_dial", UDSP_TONE_SHORT_STUTTER_DIAL },
		{ "number_unobtainable", UDSP_TONE_NUMBER_UNOBTAINABLE },
	};

	for (size_t i = 0; i < sizeof(tones) / sizeof(tones[0]); i++) {
		if (strcmp(tones[i].name, name))
			continue;
		*out = tones[i].tone;
		return true;
	}

	return false;
}

static inline const char *tool_ring_reason_name(uint8_t state)
{
	switch ((enum line_ring_reason)state) {
	case LINE_RING_STOPPED:
		return "STOPPED";
	case LINE_RING_ANSWERED:
		return "ANSWERED";
	case LINE_RING_EXPIRED:
		return "EXPIRED";
	case LINE_RING_RESTARTED:
		return "RESTARTED";
	case LINE_RING_DEAD:
		return "DEAD";
	}

	return "UNKNOWN";
}

/* Index-aligned with enum udsp_cid_format; NULL past the last format. */
static inline const char *tool_cid_format_name(unsigned index)
{
	static const char *const names[] = {
		"bellcore-fsk", "etsi-fsk", "etsi-rpas", "etsi-dtas",
		"dtmf", "ntt", "etsi-lr-dtas", "dtmf-dtas",
	};

	if (index >= sizeof(names) / sizeof(names[0]))
		return NULL;

	return names[index];
}

static inline bool tool_cid_format_parse(const char *name, uint8_t *out)
{
	for (unsigned i = 0; tool_cid_format_name(i); i++) {
		if (strcmp(tool_cid_format_name(i), name))
			continue;
		*out = (uint8_t)i;
		return true;
	}

	return false;
}

#endif
