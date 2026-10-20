// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The runtime trace domains. trace.h holds the masks and the functions that
 * use them. The enum stands alone in this header, so a signaling protocol can
 * name a domain without the trace functions of the core (proto_trace.h).
 */
#ifndef UVOICE_TRACE_DOMAIN_H
#define UVOICE_TRACE_DOMAIN_H

enum trace_domain {
	TRACE_SIP = 0,
	TRACE_FSM,
	TRACE_HOOK,
	TRACE_HAL,
	TRACE_DIALPLAN,
	/* The two media domains. TRACE_MEDIA is a periodic per-stream summary
	 * the daemon polls; TRACE_JB is the per-decision trace the HAL backend
	 * drains from its engine. */
	TRACE_MEDIA,
	TRACE_JB,
	TRACE_MGCP,
	TRACE_DOMAIN_MAX,
};

#endif
