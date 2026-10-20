// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The SIP half of the line shell (src/proto/sip/line.c): one FXS state
 * machine per line (fsm.h), the vtable it runs against the leg layer
 * (call.h) and the core, the callbacks of the leg layer, the fax and T.38
 * tone policy, the forward disconnect policy and the derivation of the
 * machine view from the config. The ops table of proto/sip/proto.c wraps
 * these entries; each one keeps the contract of the struct proto_ops entry it
 * stands behind (proto/proto.h). line is the 0 based line index.
 *
 * Timers. The file owns one uloop-backed struct sip_txn_env (from
 * sip_txn_env_uloop_init) for every machine timer, because lines outlive
 * accounts: an account rebuild on reload must not cancel a ring or a clearing
 * chain. A test of the machine alone hands fsm_init() an env of its own
 * (test_fsm.c).
 *
 * The core. Every core service this half uses is an entry of sip_host, the
 * host table the SIP ops table took at ops->init.
 */
#ifndef UVOICE_SIP_LINE_H
#define UVOICE_SIP_LINE_H

#include <stdbool.h>
#include <stdint.h>

#include "cfg/cfg.h"
#include "hal/hal.h"
#include "proto/proto.h"

struct blob_buf;
struct call_cb;

/* The host table of the SIP unit, set by sip_lines_init(). */
extern const struct proto_host *sip_host;

/* Derive the view of every line from cfg and wire one machine per line. */
void sip_lines_init(const struct proto_host *host, const struct cfg *cfg);

/* The capability moved (ops->dsp_ready): the final one settles the
 * conference mode of every line. */
void sip_lines_dsp_ready(void);

/* The daemon stop began: a service move out ends the legs at once. */
void sip_lines_stop_begin(void);

/* The callbacks the leg layer reports into (call.h calls_init). */
const struct call_cb *sip_line_call_cb(void);

/* The entries of struct proto_ops for one line. */
void sip_line_event(unsigned int line, const struct proto_line_event *ev);
void sip_line_service(unsigned int line, bool in_service,
		      enum proto_service_reason reason);
void sip_line_reset(unsigned int line);
void sip_line_cfg_changed(unsigned int line);
void sip_line_region_changed(unsigned int line,
			     const struct hal_region *region);
void sip_line_ring_held(unsigned int line);
void sip_line_ring_started(unsigned int line);
void sip_line_ring_failed(unsigned int line);
uint32_t sip_line_flags(unsigned int line);
const char *sip_line_state_name(unsigned int line);
bool sip_line_digit_play_allowed(unsigned int line, unsigned int stream);
unsigned int sip_line_stream_of_slot(unsigned int line, unsigned int slot);

/* The machine keys of the call[] entry of the line. */
void sip_line_status_dump(struct blob_buf *b, unsigned int line);

/*
 * The account layer stored a NOTIFY through host->mwi_set: the machine takes
 * the audible choice from the store and the line's amwi option.
 */
void sip_line_mwi_push(unsigned int line);

#endif
