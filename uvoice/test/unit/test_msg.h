// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The parsed message fixture of the SIP suites. msg_take() copies a text into
 * one of eight rotating buffers and parses it there, so a test can hold the
 * last seven messages while it builds the next one.
 */
#ifndef UVOICE_TEST_MSG_H
#define UVOICE_TEST_MSG_H

#include <string.h>

#include "proto/shared/msg.h"

#define MSG_TAKE_SLOTS		8
#define MSG_TAKE_TEXT_MAX	4096

static char msg_store[MSG_TAKE_SLOTS][MSG_TAKE_TEXT_MAX];
static struct sip_msg msg_stored[MSG_TAKE_SLOTS];
static unsigned int msg_store_next;

/* NULL for a text that does not fit a buffer or does not parse */
static inline struct sip_msg *msg_take(const char *text)
{
	unsigned int i = msg_store_next++ % MSG_TAKE_SLOTS;
	struct sip_msg *m = &msg_stored[i];
	size_t len = strlen(text);

	if (len >= MSG_TAKE_TEXT_MAX)
		return NULL;

	memcpy(msg_store[i], text, len + 1);
	if (msg_parse(m, msg_store[i], len) != SIP_PARSE_OK)
		return NULL;

	return m;
}

#endif
