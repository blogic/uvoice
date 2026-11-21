// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The XMPP account layer (src/proto/xmpp/account.c): one account per line
 * whose channel names account_type xmpp. It owns the stream of the line
 * (xmpp/stream.h), the TLS context the stream verifies with, the roster of
 * the account, the STUN and TURN servers the calls use and the environment
 * the Jingle legs run in (xmpp/call.h).
 *
 * What it does once the stream is ready: it sends the presence with the
 * entity capabilities, fetches the roster, asks the server for its media
 * relays (XEP-0215) and posts the registration state the desk shows. Every
 * stanza the stream does not answer itself goes to the legs: a Jingle iq, a
 * Jingle Message Initiation. The rest is dropped.
 *
 * Relays. The server's STUN and TURN services and the ones the channel
 * names are resolved to addresses through the shared resolver, because the
 * transport library takes addresses. A call that fails its connectivity
 * checks while no TURN relay was known reports a line fault, so the user
 * learns that a relay is missing (host->line_fault).
 *
 * The SIP account layer leaves an xmpp line alone (account.c
 * acct_cfg_derive), so one line registers with one layer.
 */
#ifndef UVOICE_XMPP_ACCOUNT_H
#define UVOICE_XMPP_ACCOUNT_H

#include <stdbool.h>
#include <stdint.h>

#include "cfg/cfg.h"
#include "proto/proto.h"
#include "proto/sip/call.h"

#define XMPP_ROSTER_MAX		32U
#define XMPP_SERVICES_MAX	4U

struct blob_buf;

/**
 * xmpp_accounts_init() - wire the layer and the legs
 * @host: the host table
 * @cb: the callbacks of the line shell the legs report through
 */
void xmpp_accounts_init(const struct proto_host *host,
			const struct call_cb *cb);

/**
 * xmpp_accounts_start() - build the account of every xmpp line
 * @cfg: the running configuration
 */
void xmpp_accounts_start(const struct cfg *cfg);

/**
 * xmpp_accounts_reload() - follow a configuration change
 * @old_cfg: the configuration the accounts were built from
 * @new_cfg: the new one
 *
 * An account whose stream options moved is rebuilt. The relays and the
 * speed dial of an account move in place.
 */
void xmpp_accounts_reload(const struct cfg *old_cfg, const struct cfg *new_cfg);

/**
 * xmpp_accounts_hal_set() - the capability of the backend
 * @features: the enum hal_feature bits
 * @codecs: 1 << enum hal_codec per codec
 */
void xmpp_accounts_hal_set(uint32_t features, uint32_t codecs);

/**
 * xmpp_accounts_shutdown_begin() - end the calls and close the streams
 */
void xmpp_accounts_shutdown_begin(void);

/**
 * xmpp_accounts_done() - release everything
 */
void xmpp_accounts_done(void);

/**
 * xmpp_account_active() - whether a line runs an XMPP account
 * @line: the line
 *
 * Return: true when the account is built.
 */
bool xmpp_account_active(unsigned int line);

/**
 * xmpp_account_ready() - whether the stream of a line carries stanzas
 * @line: the line
 *
 * Return: true in the ready state.
 */
bool xmpp_account_ready(unsigned int line);

/**
 * xmpp_account_label() - the JID of the account of a line
 * @line: the line
 *
 * Return: the bare JID, "" for a line without an account.
 */
const char *xmpp_account_label(unsigned int line);

/**
 * xmpp_accounts_status_dump() - the xmpp table of the status verb
 * @b: the blob buffer
 * @want_line: one line, or -1 for every one
 */
void xmpp_accounts_status_dump(struct blob_buf *b, int want_line);

/**
 * xmpp_roster_dump() - the contacts of every account, for the roster verb
 * @b: the blob buffer
 */
void xmpp_roster_dump(struct blob_buf *b);

#endif
