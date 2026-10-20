// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The firewall reconcile of the VoIP rules (src/net/fw_reconcile.c). It joins
 * what the core knows (the voice interface, restrict_source and the RTP range
 * of each channel) with what the protocol asks for (struct fw_sig_ask of
 * proto/proto.h), applies the restrict_source policy and hands the set to fw.c.
 */
#ifndef UVOICE_NET_FW_RECONCILE_H
#define UVOICE_NET_FW_RECONCILE_H

#include <stdbool.h>

#include "cfg/cfg.h"
#include "fw.h"
#include "proto/proto.h"

_Static_assert(PROTO_FW_PROXY_MAX == FW_PROXY_MAX,
	       "struct fw_sig_ask holds FW_PROXY_MAX proxy sources");
_Static_assert(PROTO_FW_SRC_MAX == FW_SRC_MAX,
	       "struct fw_sig_ask holds sources of FW_SRC_MAX bytes");

/*
 * Record the voice interface, restrict_source and the effective RTP range of
 * every channel of cfg. It applies nothing; the next fw_reconcile_ask() does.
 */
void fw_reconcile_cfg_take(const struct cfg *cfg);

/*
 * Reconcile the firewall with ask. The core computes the RTP ranges from the
 * config it took, through net_port_range(); the protocol supplies the
 * signaling ports, the proto and the sorted proxy list.
 */
void fw_reconcile_ask(const struct fw_sig_ask *ask);

/*
 * Remove every VoIP rule, through the asynchronous chain of fw.c. A protocol
 * with no active line asks for it through host->fw_clear. The daemon stop
 * runs fw_shutdown() instead (proto_done), because no loop runs a chain after
 * the stop.
 */
void fw_reconcile_clear(void);

/*
 * Add one proxy source address to a sorted list and answer the new count. A
 * repeat is dropped. Past FW_PROXY_MAX the list keeps the lexicographically
 * smallest addresses and warns about the one that leaves. The list must be
 * one this function built.
 */
unsigned int fw_reconcile_proxy_add(char (*list)[FW_SRC_MAX],
				    unsigned int count, const char *addr);

/*
 * Whether restrict_source asks for the open signaling rule: mode 0 always,
 * mode 1 until a proxy address is known.
 */
bool fw_reconcile_sip_open_needed(unsigned int restrict_source,
				  unsigned int nproxy);

/*
 * Whether restrict_source asks for a VoIP ingress rule at all. Mode 2 asks
 * for none: conntrack admits the answers to what the sockets sent.
 */
bool fw_reconcile_ingress_rules_needed(unsigned int restrict_source);

/*
 * Apply the restrict_source policy to the inputs one reconcile collected:
 * mode 2 clears the whole VoIP rule namespace, modes 0 and 1 install the set
 * with the signaling form fw_reconcile_sip_open_needed() picks. active is the
 * number of lines behind the set and reaches the log line alone. Exported so
 * the suite drives the production decision instead of a copy of it.
 */
void fw_reconcile_apply(const char *iface, unsigned int restrict_source,
			unsigned int active, const char *sip_ports,
			bool proto_udp, const struct fw_rtp_range *rtp,
			unsigned int rtp_count, const char (*proxy)[FW_SRC_MAX],
			unsigned int nproxy);

#endif
