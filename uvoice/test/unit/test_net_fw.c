// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the firewall reconcile of the core (src/net/fw_reconcile.c).
 *
 * Isolation. The suite compiles fw_reconcile.c, fw.c, spawn_run.c, ports.c and
 * trace.c into itself. fw.c runs in its test mode (fw.h fw_test_mode), which
 * believes the helper exists and spawns nothing, so the observable is the
 * argument vector of every step the chain would run (fw_plan_step). The
 * protocol side is a struct fw_sig_ask the cases build by hand.
 */
#include <stdio.h>
#include <string.h>

#include <libubox/uloop.h>

#include "cfg/cfg.h"
#include "fw.h"
#include "net/fw_reconcile.h"
#include "net/ports.h"
#include "test_util.h"
#include "test_warn.h"

/*
 * Every chain deletes the whole rule namespace first, so an add step sits at
 * DELS + its index in the desired set.
 */
#define DELS		(1 + CFG_CHANNEL_MAX + FW_PROXY_MAX)
#define ADD(n)		(DELS + (n))

#define SCRIPT		"/usr/srg/scripts/firewall_exception"

static bool has_str(const char *hay, const char *needle)
{
	return hay && strstr(hay, needle) != NULL;
}

/* NULL tolerant: a step past the end must fail the case, not the process */
static bool step_is(unsigned int idx, const char *want)
{
	char cmd[256];
	const char *got = fw_plan_step(idx, cmd, sizeof(cmd));

	return got && !strcmp(got, want);
}

static bool step_has(unsigned int idx, const char *needle)
{
	char cmd[256];

	return has_str(fw_plan_step(idx, cmd, sizeof(cmd)), needle);
}

/* Whether any step of the plan carries the text, for what must not be there. */
static bool plan_has(const char *needle)
{
	unsigned int i;

	for (i = 0; i < fw_plan_count(); i++) {
		if (step_has(i, needle))
			return true;
	}

	return false;
}

/* Run the chain in flight to its end, step by step. */
static void chain_finish(void)
{
	unsigned int guard = 0;

	while (fw_plan_count() && guard++ < 4 * DELS)
		fw_test_step_done(0);
}

/* ---------------------------------------------------------- the proxy list */

static void test_proxy_list(void)
{
	static const char *const many[] = {
		"198.51.100.27", "198.51.100.13", "198.51.100.20",
		"198.51.100.11", "198.51.100.26", "198.51.100.14",
		"198.51.100.22", "198.51.100.12", "198.51.100.19",
		"198.51.100.10", "198.51.100.24", "198.51.100.16",
		"198.51.100.21", "198.51.100.15", "198.51.100.25",
		"198.51.100.17", "198.51.100.23", "198.51.100.18",
	};
	char list[FW_PROXY_MAX][FW_SRC_MAX];
	unsigned int i, n;

	memset(list, 0, sizeof(list));

	CASE("proxy list: addresses are kept sorted");
	n = fw_reconcile_proxy_add(list, 0, "203.0.113.9");
	n = fw_reconcile_proxy_add(list, n, "198.51.100.7");
	CHECK_UINT(n, 2);
	CHECK(!strcmp(list[0], "198.51.100.7"));
	CHECK(!strcmp(list[1], "203.0.113.9"));

	CASE("proxy list: a repeat is dropped");
	n = fw_reconcile_proxy_add(list, n, "198.51.100.7");
	CHECK_UINT(n, 2);

	CASE("proxy list: an empty or missing address is not an address");
	CHECK_UINT(fw_reconcile_proxy_add(list, n, ""), 2);
	CHECK_UINT(fw_reconcile_proxy_add(list, n, NULL), 2);

	/* the set that keeps its rules is the smallest FW_PROXY_MAX, whatever
	 * order the answer drew them in, and the one that leaves is named */
	CASE("proxy list: an overflow keeps the smallest addresses and warns");
	memset(list, 0, sizeof(list));
	warn_capture_begin();
	n = 0;
	for (i = 0; i < sizeof(many) / sizeof(many[0]); i++)
		n = fw_reconcile_proxy_add(list, n, many[i]);
	CHECK_UINT(n, FW_PROXY_MAX);
	CHECK(!strcmp(list[0], "198.51.100.10"));
	CHECK(!strcmp(list[FW_PROXY_MAX - 1], "198.51.100.25"));
	CHECK_UINT(warn_capture_count("198.51.100.27"), 1);
	CHECK_UINT(warn_capture_count("198.51.100.26"), 1);
	warn_capture_end();
}

/* ------------------------------------------------------- the two predicates */

static void test_predicates(void)
{
	CASE("open: mode 0 always opens the signaling rule");
	CHECK(fw_reconcile_sip_open_needed(0, 0));
	CHECK(fw_reconcile_sip_open_needed(0, 3));

	CASE("open: mode 1 opens it until a proxy address is known");
	CHECK(fw_reconcile_sip_open_needed(1, 0));
	CHECK(!fw_reconcile_sip_open_needed(1, 1));

	CASE("ingress: modes 0 and 1 ask for VoIP rules, mode 2 for none");
	CHECK(fw_reconcile_ingress_rules_needed(0));
	CHECK(fw_reconcile_ingress_rules_needed(1));
	CHECK(!fw_reconcile_ingress_rules_needed(2));
}

/* ------------------------------------------------------------- the policy */

static void test_apply(void)
{
	char proxy[FW_PROXY_MAX][FW_SRC_MAX];
	struct fw_rtp_range rtp[1];
	unsigned int n;

	memset(proxy, 0, sizeof(proxy));
	memset(rtp, 0, sizeof(rtp));
	rtp[0].channel = 1;
	rtp[0].min = 10000;
	rtp[0].max = 10030;

	CASE("apply: mode 0 installs the open signaling rule and the RTP rule");
	fw_test_mode(true);
	fw_reconcile_apply("wan", 0, 1, "5060", true, rtp, 1, proxy, 0);
	CHECK_UINT(fw_plan_count(), ADD(2));
	CHECK(step_is(ADD(0), SCRIPT " -a Allow_VoIP_SIP -p 5060 -P udp -i wan "
				     "-r"));
	CHECK(step_is(ADD(1), SCRIPT " -a Allow_VoIP_RTP_CH1 -p 10000-10031 -P "
				     "udp -i wan"));
	chain_finish();

	CASE("apply: mode 1 names one rule per proxy source");
	n = fw_reconcile_proxy_add(proxy, 0, "198.51.100.7");
	n = fw_reconcile_proxy_add(proxy, n, "203.0.113.9");
	fw_reconcile_apply("wan", 1, 1, "5060", true, rtp, 1,
			   (const char (*)[FW_SRC_MAX])proxy, n);
	CHECK_UINT(fw_plan_count(), ADD(3));
	CHECK(step_has(ADD(0), "-a Allow_VoIP_PROXY1 -p 5060 -P udp -s "
			       "198.51.100.7"));
	CHECK(step_has(ADD(1), "-a Allow_VoIP_PROXY2 -p 5060 -P udp -s "
			       "203.0.113.9"));
	CHECK(!plan_has("-a Allow_VoIP_SIP"));
	chain_finish();

	CASE("apply: mode 2 deletes the namespace and adds nothing");
	fw_reconcile_apply("wan", 2, 1, "5060", true, rtp, 1,
			   (const char (*)[FW_SRC_MAX])proxy, n);
	CHECK_UINT(fw_plan_count(), DELS);
	CHECK(!plan_has(" -a "));
	chain_finish();

	CASE("apply: an ask the firewall already holds runs no chain");
	fw_reconcile_apply("wan", 2, 1, "5060", true, rtp, 1,
			   (const char (*)[FW_SRC_MAX])proxy, n);
	CHECK_UINT(fw_plan_count(), 0);
	fw_test_mode(false);
}

/* ------------------------------------------------ the config and the ask */

static void cfg_two_lines(struct cfg *c, unsigned int restrict_source)
{
	memset(c, 0, sizeof(*c));
	snprintf(c->local_interface, sizeof(c->local_interface), "wan2");
	c->restrict_source = restrict_source;
	c->channel[1].rtp_port_min = 20000;
	c->channel[1].rtp_port_max = 20040;
}

static void ask_two_lines(struct fw_sig_ask *ask)
{
	memset(ask, 0, sizeof(*ask));
	ask->active_mask = 0x3;
	ask->port[0] = 5060;
	ask->port[1] = 5062;
	ask->proto_udp = true;
	ask->nproxy = fw_reconcile_proxy_add(ask->proxy, 0, "198.51.100.7");
}

static void test_ask(void)
{
	static struct cfg c;
	struct fw_sig_ask ask;

	/* the core joins what it took from the config with what the protocol
	 * names: the interface and the RTP ranges are the config's */
	CASE("ask: the config names the interface and the RTP range of each "
	     "line");
	fw_test_mode(true);
	cfg_two_lines(&c, 0);
	fw_reconcile_cfg_take(&c);
	ask_two_lines(&ask);
	fw_reconcile_ask(&ask);
	CHECK_UINT(fw_plan_count(), ADD(3));
	CHECK(step_is(ADD(0), SCRIPT " -a Allow_VoIP_SIP -p 5060 5062 -P udp "
				     "-i wan2 -r"));
	CHECK(step_is(ADD(1), SCRIPT " -a Allow_VoIP_RTP_CH1 -p 10000-10031 -P "
				     "udp -i wan2 -r"));
	CHECK(step_is(ADD(2), SCRIPT " -a Allow_VoIP_RTP_CH2 -p 20000-20041 -P "
				     "udp -i wan2"));

	CASE("ask: mode 0 takes no proxy rule although the protocol named one");
	CHECK(!plan_has("Allow_VoIP_PROXY1 -p"));
	chain_finish();

	CASE("ask: mode 1 takes the proxy sources of the ask");
	cfg_two_lines(&c, 1);
	fw_reconcile_cfg_take(&c);
	fw_reconcile_ask(&ask);
	CHECK(step_has(ADD(0), "-a Allow_VoIP_PROXY1 -p 5060 5062 -P udp -s "
			       "198.51.100.7 -i wan2"));
	CHECK(!plan_has("-a Allow_VoIP_SIP"));
	chain_finish();

	CASE("ask: two lines on one port open it once");
	cfg_two_lines(&c, 0);
	fw_reconcile_cfg_take(&c);
	ask.port[1] = 5060;
	fw_reconcile_ask(&ask);
	CHECK(step_is(ADD(0), SCRIPT " -a Allow_VoIP_SIP -p 5060 -P udp -i "
				     "wan2 -r"));
	chain_finish();

	CASE("ask: a line outside the mask takes no rule");
	ask.active_mask = 0x1;
	fw_reconcile_ask(&ask);
	CHECK_UINT(fw_plan_count(), ADD(2));
	CHECK(!plan_has("Allow_VoIP_RTP_CH2 -p"));
	chain_finish();

	CASE("ask: an empty interface takes wan");
	cfg_two_lines(&c, 0);
	c.local_interface[0] = 0;
	fw_reconcile_cfg_take(&c);
	fw_reconcile_ask(&ask);
	CHECK(step_has(ADD(0), "-i wan "));
	chain_finish();

	CASE("ask: no active line clears every rule");
	ask.active_mask = 0;
	fw_reconcile_ask(&ask);
	CHECK_UINT(fw_plan_count(), DELS);
	CHECK(!plan_has(" -a "));
	chain_finish();

	CASE("clear: the chain deletes the whole namespace");
	ask.active_mask = 0x1;
	fw_reconcile_ask(&ask);
	chain_finish();
	fw_reconcile_clear();
	CHECK_UINT(fw_plan_count(), DELS);
	CHECK(step_is(DELS - 1, SCRIPT " -d Allow_VoIP_PROXY16"));
	chain_finish();
	fw_test_mode(false);
}

int main(void)
{
	uloop_init();

	test_proxy_list();
	test_predicates();
	test_apply();
	test_ask();

	uloop_done();

	return tu_summary("test_net_fw");
}
