// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Test access to the MGCP unit (mgcp/proto.c), never used by the daemon. The
 * unit is reached through mgcp_proto_ops (proto/proto.h) alone; these entries
 * follow acct_dns() and acct_test_dns_retry_ms() of proto/sip/account.h.
 */
#ifndef UVOICE_MGCP_PROTO_H
#define UVOICE_MGCP_PROTO_H

struct proto_timer_env;
struct sip_dns;

/* The resolver of the unit, so a suite can pin its servers
 * (sip_dns_servers_set()). Valid between ops->init and ops->done. */
struct sip_dns *mgcp_proto_test_dns(void);

/*
 * The timer environment of the unit: the transaction layer, the endpoints
 * and the retry of the call agent name run on it. A suite binds a virtual
 * clock to it after ops->init and before ops->start.
 */
struct proto_timer_env *mgcp_proto_test_timer_env(void);

/* The time left until the call agent name may be asked again after a failed
 * query, 0 when its last query did not fail. */
unsigned int mgcp_proto_test_ca_retry_ms(void);

#endif
