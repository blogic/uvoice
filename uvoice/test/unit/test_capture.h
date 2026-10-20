// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The transport capture of the suites that drive a SIP layer through struct
 * sip_txn_env. A capture array replaces the socket: each send is kept with
 * its destination, and a test reads the wire text back by index.
 *
 * The suite declares its fixture state as a static named fk before it
 * includes this header. fk holds the capture fields: struct fake_send
 * send[], unsigned int sends and bool send_fail. Each struct fake_send holds
 * char buf[], size_t len, char host[64] and uint16_t port. The sizes of the
 * two arrays set the capture limits.
 */
#ifndef UVOICE_TEST_CAPTURE_H
#define UVOICE_TEST_CAPTURE_H

#include <string.h>

#include "proto/sip/txn.h"

#define FAKE_SEND_SLOTS		(sizeof(fk.send) / sizeof(fk.send[0]))
#define FAKE_SEND_TEXT_MAX	(sizeof(fk.send[0].buf) - 1)

static inline int fake_send_fn(struct sip_txn_env *env, const void *buf,
			       size_t len, const struct sip_txn_dst *dst)
{
	unsigned int i = fk.sends;
	size_t kept = len < FAKE_SEND_TEXT_MAX ? len : FAKE_SEND_TEXT_MAX;

	if (fk.send_fail)
		return -1;

	/* The counter keeps counting past the capture array, so a test that
	 * floods the transport never turns into an accidental send failure. */
	if (i < FAKE_SEND_SLOTS) {
		fk.send[i].len = kept;
		memcpy(fk.send[i].buf, buf, kept);
		fk.send[i].buf[kept] = 0;
		fk.send[i].host[0] = 0;
		sip_dst_host(dst, fk.send[i].host, sizeof(fk.send[i].host));
		fk.send[i].port = sip_dst_port(dst);
	}
	fk.sends++;

	return (int)len;
}

/* "" for a send that never happened or that fell past the capture array */
static inline const char *send_text(unsigned int idx)
{
	if (idx >= fk.sends || idx >= FAKE_SEND_SLOTS)
		return "";

	return fk.send[idx].buf;
}

static inline const char *last_text(void)
{
	return fk.sends ? send_text(fk.sends - 1) : "";
}

/* The parse runs on a copy, so the strings of m stay valid until the next
 * call and the capture itself is never changed. */
static inline bool send_parse(unsigned int idx, struct sip_msg *m)
{
	static char copy[sizeof(fk.send[0].buf)];
	size_t len;

	if (idx >= fk.sends || idx >= FAKE_SEND_SLOTS)
		return false;

	len = fk.send[idx].len;
	memcpy(copy, fk.send[idx].buf, len);

	return msg_parse(m, copy, len) == SIP_PARSE_OK;
}

/* the last reply that reply_recv() parsed, which a case can deliver again */
static char reply_store[sizeof(fk.send[0].buf)];
static struct sip_msg reply_stored;

/* A text that does not fit the store is dropped, never truncated, so a case
 * cannot pass on half a reply. */
static inline void reply_recv(struct sip_txn_mgr *mgr, const char *text,
			      size_t len)
{
	if (len >= sizeof(reply_store))
		return;

	memcpy(reply_store, text, len);
	if (msg_parse(&reply_stored, reply_store, len) != SIP_PARSE_OK)
		return;

	sip_txn_msg_recv(mgr, &reply_stored, NULL);
}

#endif
