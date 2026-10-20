// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <string.h>

#include "proto/sip/tap.h"

const struct proto_host *sip_wire_host;

sip_tap_fn sip_tap_cb;

void sip_tap_set(sip_tap_fn fn)
{
	sip_tap_cb = fn;
}

void sip_tap_endpoint_set(struct trace_endpoint *out,
			  const struct sockaddr *sa)
{
	if (!sip_wire_host) {
		memset(out, 0, sizeof(*out));
		return;
	}

	sip_wire_host->calltrace_tap_endpoint_set(sip_wire_host->ctx, out, sa);
}
