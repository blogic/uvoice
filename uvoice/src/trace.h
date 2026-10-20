// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Runtime trace domains. Lifecycle messages (registration up and down, call
 * events, faults, reload) go straight to syslog with ULOG_*; the per-domain
 * trace below is the verbose channel that "ubus call voice.trace set" turns on
 * and off at runtime.
 *
 * A trace line has two destinations and three masks decide them. The operator
 * masks send the line to syslog; the ring mask sends it to the sink, which is
 * the udebug ring writer of trace_ring.c. This file holds neither udebug nor
 * a ring: it holds one function pointer and the masks that select it.
 */
#ifndef UVOICE_TRACE_H
#define UVOICE_TRACE_H

#include <stdbool.h>
#include <stdint.h>

#include <libubox/ulog.h>

#include "trace_domain.h"

void trace_init(const char *ident, bool syslog_only);
void trace_done(void);

/*
 * Two sources own one domain for an operator, and the rule is written here
 * because both surfaces are user visible.
 *
 *   the uci mask      voip.debug.trace, rewritten whole on load and on reload
 *   the runtime mask  the -d option and "ubus call voice.trace set"
 *
 * An operator has the domain on when either bit is set, which is what
 * trace_domain_get answers and what sends the line to syslog.
 * trace_domain_set(dom, true) sets the runtime bit alone, so a later reload of
 * the same uci list cannot turn a domain an operator asked for back off.
 * trace_domain_set(dom, false) clears both, because "off" from an operator has
 * to mean off even against a config that names the domain; the next reload of
 * that config turns it on again, which is the operator's own undo.
 *
 * Neither mask is atomic and neither needs to be: every writer and every
 * reader runs on the uloop thread. The two atomic mirrors below are the one
 * exception, for the backend threads that log outside the loop.
 */
bool trace_domain_get(enum trace_domain dom);
void trace_domain_set(enum trace_domain dom, bool on);
const char *trace_domain_name(enum trace_domain dom);
int trace_domain_find(const char *name);
int trace_domains_parse(const char *list);

/* Rewrites the uci mask from a comma list. NULL and an empty list clear it,
 * "all" sets every domain. It never touches the runtime mask. */
void trace_uci_apply(const char *list);

/*
 * The third mask, and the one an operator never sees: the call trace
 * controller owns it and holds it for the length of one trace.
 *
 * A domain is active when any of the three masks carries it, and that is what
 * the TRACE macro and every direct guard test. A domain the ring mask alone
 * carries produces lines for the sink and none for syslog, so an armed trace
 * costs the operator's log nothing.
 *
 * trace_ring_set replaces the whole mask, so the controller clears its own
 * bits with trace_ring_set(0) at the freeze.
 */
bool trace_domain_active(enum trace_domain dom);
void trace_ring_set(uint32_t mask);
void trace_sink_set(void (*sink)(enum trace_domain dom, const char *msg));

/*
 * The two mirrors a thread outside the uloop may read. Both are relaxed
 * atomic loads of the mask they name, so a reader gets the value of one store
 * and never a half-written word; neither carries an ordering promise beyond
 * that, because a log line one tick late costs nothing.
 *
 * trace_mask_atomic mirrors the two operator masks (the syslog decision) and
 * trace_ring_mask_atomic the ring mask (the sink decision). The udsp backend
 * log sink reads both from the library's control, slic and emitter threads.
 */
uint32_t trace_mask_atomic(void);
uint32_t trace_ring_mask_atomic(void);

/*
 * The hook fires once per real change of the active state of one domain, from
 * trace_domain_set, trace_uci_apply and trace_ring_set. A set that changes
 * nothing fires nothing, so a consumer may act on every call it gets. One
 * hook, on the uloop thread, installed before the first change.
 */
void trace_hook_set(void (*hook)(enum trace_domain dom, bool on));

void trace_write(enum trace_domain dom, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

#define TRACE(dom, fmt, ...)						\
	do {								\
		if (trace_domain_active(dom))				\
			trace_write(dom, fmt, ##__VA_ARGS__);		\
	} while (0)

#endif
