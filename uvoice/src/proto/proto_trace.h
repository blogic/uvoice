// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The trace macro of a signaling protocol. It writes through the host table
 * and not through trace.c, so a protocol suite fakes the sink with its own
 * host table. The core keeps one set of trace masks, and the domains are the
 * compile-time enum trace_domain (trace_domain.h).
 */
#ifndef UVOICE_PROTO_TRACE_H
#define UVOICE_PROTO_TRACE_H

#include "proto/proto.h"

/*
 * Write one trace line in domain dom when the domain is active. host is a
 * const struct proto_host pointer, evaluated once. The arguments are not
 * evaluated while the domain is off.
 */
#define PROTO_TRACE(host, dom, fmt, ...)				\
	do {								\
		const struct proto_host *proto_trace_h_ = (host);	\
									\
		if (proto_trace_h_->trace_active(proto_trace_h_->ctx,	\
						 (dom)))		\
			proto_trace_h_->trace_write(			\
				proto_trace_h_->ctx, (dom), fmt,	\
				##__VA_ARGS__);				\
	} while (0)

#endif
