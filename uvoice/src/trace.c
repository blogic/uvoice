// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "trace.h"

#define TRACE_LINE_MAX	512
/* The widest domain list either surface carries; cfg_debug.trace is 128. */
#define TRACE_LIST_MAX	128

static const char * const domain_names[TRACE_DOMAIN_MAX] = {
	[TRACE_SIP] = "sip",
	[TRACE_FSM] = "fsm",
	[TRACE_HOOK] = "hook",
	[TRACE_HAL] = "hal",
	[TRACE_DIALPLAN] = "dialplan",
	[TRACE_MEDIA] = "media",
	[TRACE_JB] = "jb",
	[TRACE_MGCP] = "mgcp",
};

/*
 * The two operator masks of the ownership rule in trace.h. A domain is on for
 * syslog when either bit is set, so neither source can turn the other one off
 * by accident.
 */
static uint32_t uci_mask;
static uint32_t runtime_mask;
/* The controller's mask, and the sink it feeds. */
static uint32_t ring_mask;
static void (*domain_sink)(enum trace_domain dom, const char *msg);
static void (*domain_hook)(enum trace_domain dom, bool on);

/* The two mirrors of trace.h, read from the backend's own threads. */
static _Atomic uint32_t mask_mirror;
static _Atomic uint32_t ring_mirror;

_Static_assert(TRACE_DOMAIN_MAX <= 32, "the trace masks are 32 bits wide");

static uint32_t domain_bit(enum trace_domain dom)
{
	return 1u << (unsigned int)dom;
}

static uint32_t domain_all(void)
{
	return (1u << TRACE_DOMAIN_MAX) - 1u;
}

/* What syslog follows. */
static uint32_t effective_mask(void)
{
	return uci_mask | runtime_mask;
}

/* What the TRACE macro and the direct guards follow. */
static uint32_t active_mask(void)
{
	return uci_mask | runtime_mask | ring_mask;
}

/* One notification per domain whose active state moved, and none at all for a
 * change that left it where it was (trace.h hook contract). */
static void hook_run(uint32_t before)
{
	uint32_t moved = before ^ active_mask();
	int dom;

	if (!domain_hook || !moved)
		return;

	for (dom = 0; dom < TRACE_DOMAIN_MAX; dom++) {
		if (moved & domain_bit((enum trace_domain)dom))
			domain_hook((enum trace_domain)dom,
				    (active_mask() & domain_bit(dom)) != 0);
	}
}

/*
 * The one exit of every mask change: it publishes both mirrors and then runs
 * the hook. Writing a mask without it would leave a backend thread reading the
 * state before the change for the rest of the run.
 */
static void masks_commit(uint32_t before)
{
	atomic_store_explicit(&mask_mirror, effective_mask(),
			      memory_order_relaxed);
	atomic_store_explicit(&ring_mirror, ring_mask, memory_order_relaxed);
	hook_run(before);
}

void trace_hook_set(void (*hook)(enum trace_domain dom, bool on))
{
	domain_hook = hook;
}

void trace_sink_set(void (*sink)(enum trace_domain dom, const char *msg))
{
	domain_sink = sink;
}

uint32_t trace_mask_atomic(void)
{
	return atomic_load_explicit(&mask_mirror, memory_order_relaxed);
}

uint32_t trace_ring_mask_atomic(void)
{
	return atomic_load_explicit(&ring_mirror, memory_order_relaxed);
}

void trace_init(const char *ident, bool syslog_only)
{
	static char ident_pid[32];
	int channels = ULOG_SYSLOG;

	/* procd captures stderr into syslog at ERR severity. A second stdio sink
	 * duplicates each trace line and bypasses syslog severity filtering.
	 * Interactive and test harness runs retain stderr through the default. */
	if (!syslog_only)
		channels |= ULOG_STDIO;

	/*
	 * ulog opens syslog without LOG_PID and keeps the ident pointer, and
	 * the platform's syslog-ng template prints PROGRAM[PID], so a bare
	 * ident renders as "uvoiced[]". The pid rides in the ident instead,
	 * the shape procd gives the stdio captures of its own instances.
	 */
	snprintf(ident_pid, sizeof(ident_pid), "%s[%ld]", ident, (long)getpid());
	ulog_open(channels, LOG_DAEMON, ident_pid);
	ulog_threshold(LOG_DEBUG);
}

void trace_done(void)
{
	ulog_close();
}

bool trace_domain_get(enum trace_domain dom)
{
	if (dom >= TRACE_DOMAIN_MAX)
		return false;

	return (effective_mask() & domain_bit(dom)) != 0;
}

bool trace_domain_active(enum trace_domain dom)
{
	if (dom >= TRACE_DOMAIN_MAX)
		return false;

	return (active_mask() & domain_bit(dom)) != 0;
}

/* The bits above the domain set are dropped, so a mask the controller builds
 * from a wider word cannot make a domain that does not exist read active. */
void trace_ring_set(uint32_t mask)
{
	uint32_t before = active_mask();

	ring_mask = mask & domain_all();
	masks_commit(before);
}

void trace_domain_set(enum trace_domain dom, bool on)
{
	uint32_t before = active_mask();

	if (dom >= TRACE_DOMAIN_MAX)
		return;

	if (on) {
		runtime_mask |= domain_bit(dom);
	} else {
		/* Off from an operator means off, so the uci bit goes with it
		 * (trace.h ownership rule); the next reload restores it. */
		runtime_mask &= ~domain_bit(dom);
		uci_mask &= ~domain_bit(dom);
	}

	masks_commit(before);
}

const char *trace_domain_name(enum trace_domain dom)
{
	if (dom >= TRACE_DOMAIN_MAX)
		return "invalid";

	return domain_names[dom];
}

int trace_domain_find(const char *name)
{
	int i;

	for (i = 0; i < TRACE_DOMAIN_MAX; i++) {
		if (!strcmp(name, domain_names[i]))
			return i;
	}

	return -1;
}

/*
 * The one tokeniser both surfaces use. It names no mask and touches none: the
 * caller decides which of the two the answer belongs to, which is what keeps
 * the ownership rule of trace.h in one place instead of two parsers.
 *
 * "all" is every domain. An unknown name warns and is skipped, because a typo
 * in a config or on a command line must not cost the domains beside it. The
 * count is the number of domains named, so a caller can tell an empty list
 * from a list of typos.
 */
static int list_mask(const char *list, uint32_t *out)
{
	char buf[TRACE_LIST_MAX];
	char *tok, *save;
	int count = 0;
	int dom;

	*out = 0;
	if (!list)
		return 0;

	snprintf(buf, sizeof(buf), "%s", list);
	for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		if (!strcmp(tok, "all")) {
			*out = domain_all();
			count += TRACE_DOMAIN_MAX;
			continue;
		}

		dom = trace_domain_find(tok);
		if (dom < 0) {
			ULOG_WARN("unknown trace domain '%s'\n", tok);
			continue;
		}

		*out |= domain_bit((enum trace_domain)dom);
		count++;
	}

	return count;
}

/* The -d option and nothing else: it adds to the runtime mask, so two -d
 * lists accumulate. */
int trace_domains_parse(const char *list)
{
	uint32_t before = active_mask();
	uint32_t mask = 0;
	int count = list_mask(list, &mask);

	runtime_mask |= mask;
	masks_commit(before);

	return count;
}

/* The uci list replaces its own mask, so a domain the new list drops goes off
 * unless the runtime mask still holds it (trace.h ownership rule). */
void trace_uci_apply(const char *list)
{
	uint32_t before = active_mask();
	uint32_t mask = 0;

	list_mask(list, &mask);
	uci_mask = mask;
	masks_commit(before);
}

/*
 * The two destinations of one line. The operator masks own syslog and the ring
 * mask owns the sink, so a trace an operator did not ask for adds nothing to
 * the log and a domain an operator turned on keeps its log line while a trace
 * runs.
 */
static void trace_emit(enum trace_domain dom, const char *msg)
{
	uint32_t bit = domain_bit(dom);

	if (effective_mask() & bit)
		ulog(LOG_DEBUG, "[%s] %s\n", trace_domain_name(dom), msg);

	if ((ring_mask & bit) && domain_sink)
		domain_sink(dom, msg);
}

void trace_write(enum trace_domain dom, const char *fmt, ...)
{
	char buf[TRACE_LINE_MAX];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	trace_emit(dom, buf);
}
