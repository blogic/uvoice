// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "net/fw_reconcile.h"
#include "net/ports.h"
#include "str.h"
#include "trace.h"

/* dest_port list for the firewall rule; 2 ports plus a space and a NUL. */
#define FW_PORTS_MAX		16

/*
 * What the core knows of the rule set, taken from the running config
 * (fw_reconcile_cfg_take): the voice interface, restrict_source (0 open, 1
 * per proxy source, 2 pinhole) and the effective RTP range of each channel. It
 * is refreshed wherever a config arrives, so it never outlives the struct cfg
 * it was read from.
 */
static struct {
	char iface[CFG_STR_MAX];
	unsigned int restrict_source;
	struct {
		uint16_t min;
		uint16_t max;
	} rtp[CFG_CHANNEL_MAX];
} fr;

void fw_reconcile_cfg_take(const struct cfg *cfg)
{
	unsigned int i;

	str_copy(fr.iface, sizeof(fr.iface), cfg ? cfg->local_interface : "");
	fr.restrict_source = cfg ? cfg->restrict_source : 0;

	for (i = 0; i < CFG_CHANNEL_MAX; i++)
		net_port_range(cfg ? &cfg->channel[i] : NULL, &fr.rtp[i].min,
			       &fr.rtp[i].max);
}

static void ports_append(char *ports, size_t len, unsigned int port)
{
	char one[8];
	size_t used = strlen(ports);
	size_t olen;
	const char *p = ports;

	olen = (size_t)snprintf(one, sizeof(one), "%u", port);

	/*
	 * Deduplicate on whole space delimited tokens: two lines on 5060 open
	 * one port, but "15060" must not make "5060" look already present (a
	 * strstr would).
	 */
	while (*p) {
		const char *sp = strchr(p, ' ');
		size_t tlen = sp ? (size_t)(sp - p) : strlen(p);

		if (tlen == olen && !memcmp(p, one, olen))
			return;
		if (!sp)
			break;
		p = sp + 1;
	}

	snprintf(ports + used, len - used, "%s%s", used ? " " : "", one);
}

/*
 * One distinct proxy source address per rule. A repeat is dropped, so two
 * lines behind one proxy hold one rule and the rule numbering stays 1-based
 * and dense. Past FW_PROXY_MAX one address gets no rule at all, which silently
 * drops that proxy's traffic, so it is reported.
 *
 * The list is sorted, and that is the whole answer to the SRV redraw. RFC 2782
 * weight selection reorders equal priority SRV answers by design, so the
 * resolver redraws the same servers into another order on every delivery
 * (shared/resolve.h). A list in answer order then moves those addresses between
 * Allow_VoIP_PROXY1 and PROXY2 once per TTL cycle, fw_desired_applied()
 * compares the sets unequal, and a full delete and add chain plus an fw4 reload
 * runs for no change in what is admitted. A sorted list turns the same set into
 * the same list, so the comparison holds and a same-set redraw is a no-op. The
 * list must be one this function built: it is the only writer, and both the
 * dedup scan and the bound rely on the order it keeps.
 *
 * The bound follows the sorted order too. A full list whose largest entry is
 * larger than addr gives that entry up instead of refusing addr, so the set
 * that keeps its rules is always the FW_PROXY_MAX lexicographically smallest
 * addresses, whatever order they arrived in. Which proxy loses its rule is then
 * a property of the answer alone.
 */
unsigned int fw_reconcile_proxy_add(char (*list)[FW_SRC_MAX],
				    unsigned int count, const char *addr)
{
	unsigned int pos, i;

	if (!addr || !*addr)
		return count;

	for (pos = 0; pos < count; pos++) {
		int cmp = strcmp(list[pos], addr);

		if (!cmp)
			return count;
		if (cmp > 0)
			break;
	}

	if (count >= FW_PROXY_MAX) {
		bool addr_is_largest = pos >= FW_PROXY_MAX;

		ULOG_WARN("firewall: proxy source %s dropped, only %u rules exist\n",
			  addr_is_largest ? addr : list[FW_PROXY_MAX - 1],
			  (unsigned int)FW_PROXY_MAX);

		if (addr_is_largest)
			return count;

		count = FW_PROXY_MAX - 1;
	}

	for (i = count; i > pos; i--)
		str_copy(list[i], sizeof(list[i]), list[i - 1]);

	str_copy(list[pos], sizeof(list[pos]), addr);

	return count + 1;
}

/*
 * Which signaling rule the config asks for: true is the open Allow_VoIP_SIP
 * rule, false is one restricted rule per resolved proxy address. Mode 0 never
 * restricts. Mode 1 restricts as soon as one address is known, and keeps the
 * open rule until then; a restricted mode with nothing resolved has no address
 * to name, and no rule at all would black out signaling. Mode 2 installs no
 * rule and never reaches this predicate (fw_reconcile_ingress_rules_needed).
 */
bool fw_reconcile_sip_open_needed(unsigned int restrict_source,
				  unsigned int nproxy)
{
	return restrict_source == 0 || nproxy == 0;
}

/*
 * Whether the config asks for a VoIP ingress rule at all. restrict_source 2 is
 * the keepalive pinhole form and installs none: the signaling socket pings the
 * proxy and the media sockets transmit from the ports the offer advertises, so
 * conntrack holds an established entry for each tuple and the fw4 input chain
 * admits the answer on its built-in ct state established,related accept.
 *
 * It depends on rule 1 of stream_config_set in hal.h: the backend owns the
 * media sockets, binds them to the advertised ports and connects them. A media
 * socket on an ephemeral port leaves no conntrack entry for the advertised RTP
 * port, and this form would then take the inbound audio down.
 */
bool fw_reconcile_ingress_rules_needed(unsigned int restrict_source)
{
	return restrict_source != 2;
}

static void fw_reconcile_log(bool chain, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void fw_reconcile_log(bool chain, const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (chain) {
		ULOG_INFO("%s\n", buf);
		return;
	}
	TRACE(TRACE_FSM, "%s", buf);
}

/*
 * The whole restrict_source policy, from the inputs one reconcile collected.
 *
 * Mode 2 asks for no rule at all: the chain deletes the VoIP namespace exactly
 * as the disable path does, and conntrack admits the inbound service. Modes 0
 * and 1 hand the set to fw.c, and fw_reconcile_sip_open_needed() picks between
 * the open rule and one restricted rule per proxy address. Mode 1 with nothing
 * resolved keeps the open rule, because dropping the rule would black out
 * signaling.
 *
 * Every chain deletes the whole namespace, so a move from mode 1 to mode 2
 * leaves no proxy rule behind. A box that kept the mode 1 rules would keep
 * admitting proxy sources the running config no longer names. The dns_linger
 * retention admits only addresses the current proxy name resolved to, inside
 * a bounded expiring window, and the namespace-wide delete still runs.
 *
 * The desired set is logged at info on every reconcile that changes what the
 * firewall holds or has under way, so a delete chain without adds always has
 * the record of what was asked for and by which reconcile. The record follows
 * the kick that fw_voip_set() runs, so a line the kick writes stands above
 * it. A reconcile that changes nothing, the DNS refresh every 30 s among
 * them, traces instead; so does the ask a failed chain's retry already
 * carries (fw.h).
 */
void fw_reconcile_apply(const char *iface, unsigned int restrict_source,
			unsigned int active, const char *sip_ports,
			bool proto_udp, const struct fw_rtp_range *rtp,
			unsigned int rtp_count, const char (*proxy)[FW_SRC_MAX],
			unsigned int nproxy)
{
	bool chain;

	if (!fw_reconcile_ingress_rules_needed(restrict_source)) {
		chain = fw_voip_clear();
		fw_reconcile_log(chain, "firewall reconcile: %u line(s), iface %s, "
				 "restrict_source %u, no VoIP rule, the keepalive "
				 "pinholes carry the inbound service", active,
				 iface, restrict_source);
		return;
	}

	chain = fw_voip_set(iface, sip_ports, proto_udp, rtp, rtp_count, proxy,
			    nproxy,
			    fw_reconcile_sip_open_needed(restrict_source,
							 nproxy));
	fw_reconcile_log(chain, "firewall reconcile: %u line(s), iface %s, sip "
			 "'%s', %u rtp rule(s), first %u-%u, restrict_source %u, "
			 "%u proxy rule(s)", active, iface, sip_ports, rtp_count,
			 rtp_count ? rtp[0].min : 0, rtp_count ? rtp[0].max : 0,
			 restrict_source, nproxy);
}

/*
 * The single desired-rule-set computation. The protocol asks on every event
 * that can move the set: a build, the start, a teardown, a reload, a completed
 * resolution and the expiry of a lingering address. At exit, fw_shutdown()
 * deletes the rules instead (fw_reconcile_clear).
 *
 * The signaling ports and the proxy sources come from the protocol, and the
 * RTP range from the current config of the channel: a rule describes what the
 * box should accept now, so unlike a call leg it must not run on a snapshot
 * taken when some earlier call started.
 *
 * One RTP rule per active line, named by its 1-based channel number. Without
 * it the inbound stream of a working call is dropped at the wan firewall and
 * the handset hears nothing. restrict_source never touches these: the rules
 * are the posture of modes 0 and 1, which admit the range from any source. An
 * asymmetric peer, whose media arrives from an address the signaling never
 * named, is not a reason on its own: the sockets are connected, so the kernel
 * drops that traffic at the socket after the firewall. The ranges do not carry
 * the proxy sources: the media host is not the SIP proxy, so that filter
 * breaks early media and leading audio.
 *
 * Only mode 1 names a source, so the other two modes take no proxy rule.
 */
void fw_reconcile_ask(const struct fw_sig_ask *ask)
{
	struct fw_rtp_range rtp[CFG_CHANNEL_MAX];
	char ports[FW_PORTS_MAX] = "";
	const char *iface = fr.iface[0] ? fr.iface : "wan";
	unsigned int mode = fr.restrict_source;
	unsigned int i, active = 0, nrtp = 0;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		if (!(ask->active_mask & (1u << i)))
			continue;

		ports_append(ports, sizeof(ports), ask->port[i]);
		active++;

		rtp[nrtp].channel = i + 1;
		rtp[nrtp].min = fr.rtp[i].min;
		rtp[nrtp].max = fr.rtp[i].max;
		nrtp++;
	}

	if (!active) {
		fw_reconcile_clear();
		return;
	}

	fw_reconcile_apply(iface, mode, active, ports, ask->proto_udp, rtp,
			   nrtp, (const char (*)[FW_SRC_MAX])ask->proxy,
			   mode == 1 ? ask->nproxy : 0);
}

void fw_reconcile_clear(void)
{
	fw_reconcile_log(fw_voip_clear(),
			 "firewall reconcile: no active line, VoIP rules cleared");
}
