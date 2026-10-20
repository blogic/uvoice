// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * VoIP firewall reconcile. See fw.h for the contract.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libubox/uloop.h>

#include "fw.h"
#include "spawn_run.h"
#include "str.h"
#include "trace.h"

#define FW_SCRIPT	"/usr/srg/scripts/firewall_exception"
#define FW_SIP_RULE	"Allow_VoIP_SIP"
#define FW_RTP_RULE	"Allow_VoIP_RTP_CH%u"
#define FW_PROXY_RULE	"Allow_VoIP_PROXY%u"

/* FW_RULE_MAX is 1 SIP rule, one RTP rule per channel (cfg.h CFG_CHANNEL_MAX)
 * and FW_PROXY_MAX proxy rules; fw.h owns the numbers so this file needs no
 * cfg.h. */
#define FW_RTP_MAX	(FW_RULE_MAX - 1 - FW_PROXY_MAX)

#define FW_IFACE_MAX	16
#define FW_NAME_MAX	32
/* "10000-10030" for a range, "5060 5062" for a SIP port list */
#define FW_PORTS_MAX	24
#define FW_ARGV_MAX	16
#define FW_CMD_MAX	160
/* the ERR cadence once the backoff sits at the cap */
#define FW_RETRY_LOG_EVERY	10

/* the longest step: script, -a name, -p ports, -P proto, -s src, -i iface, -r
 * and the NULL the spawn needs */
_Static_assert(FW_ARGV_MAX >= 13, "argv too small for a source restricted add step");

struct fw_rule {
	char name[FW_NAME_MAX];
	char ports[FW_PORTS_MAX];
	char src[FW_SRC_MAX];		/* "" for no source restriction */
	bool udp;
};

/*
 * Three sets. The whole coalesce contract is that they stay apart:
 *
 *   want     what the caller last asked for. It may change at any moment.
 *   run      the immutable snapshot the chain in flight executes. It is
 *            taken at kick time and never touched again until the chain ends,
 *            because the steps already spawned were built from it.
 *   applied  what the last completed chain installed, that is, a copy of
 *            run, never of want.
 *
 * A set that changes mid chain therefore leaves want != applied at completion,
 * and the completion starts a second chain for the difference. A snapshot of
 * want instead would record rules that were never installed and skip them.
 */
static struct {
	struct uloop_process proc;
	struct uloop_timeout retry;
	bool proc_active;
	unsigned int del_retries;

	/* consecutive failed chains, and the delay the armed retry carries:
	 * stored here because uloop_timeout_remaining() is deprecated and the
	 * unit test asserts the exact delay without a clock */
	unsigned int fails;
	unsigned int retry_delay;

	/* desired state */
	char want_iface[FW_IFACE_MAX];
	struct fw_rule want[FW_RULE_MAX];
	unsigned int wants;

	/* last successfully applied state */
	bool applied_valid;
	char applied_iface[FW_IFACE_MAX];
	struct fw_rule applied[FW_RULE_MAX];
	unsigned int applied_rules;

	/* the chain in flight: every known name is deleted, then the snapshot's
	 * rules are added, so a stale rule from another config never survives */
	char run_iface[FW_IFACE_MAX];
	struct fw_rule run[FW_RULE_MAX];
	unsigned int runs;
	char del[FW_RULE_MAX][FW_NAME_MAX];
	unsigned int dels;
	unsigned int steps;
	unsigned int cur;

	/* test mode: no spawn, the fixture completes the steps by hand */
	bool test_mode;
	unsigned int spawned;
} fw;

bool fw_available(void)
{
	return access(FW_SCRIPT, X_OK) == 0;
}

/*
 * The argument list is built from controlled values (a rule name, a port list,
 * a proto and an interface), so it is passed to the spawn directly and never
 * through a shell. The helper re-execs itself under the firewall lock.
 */
static pid_t fw_spawn(char *const argv[])
{
	pid_t pid = spawn_run(argv, SPAWN_GROUP_DAEMON);

	if (pid < 0)
		ULOG_ERR("firewall spawn failed: %s\n", strerror(errno));

	return pid;
}

static void fw_kick(void);
static void fw_step_run(void);

static bool fw_rule_eq(const struct fw_rule *a, const struct fw_rule *b)
{
	return a->udp == b->udp && !strcmp(a->name, b->name) &&
	       !strcmp(a->ports, b->ports) && !strcmp(a->src, b->src);
}

/* whether the given set is the desired one; the iface only matters while
 * rules exist, an empty set has nothing bound to an interface */
static bool fw_want_eq(const char *iface, const struct fw_rule *rules, unsigned int count)
{
	unsigned int i;

	if (count != fw.wants)
		return false;
	if (fw.wants && strcmp(iface, fw.want_iface))
		return false;

	for (i = 0; i < fw.wants; i++) {
		if (!fw_rule_eq(&rules[i], &fw.want[i]))
			return false;
	}

	return true;
}

static bool fw_desired_applied(void)
{
	return fw.applied_valid &&
	       fw_want_eq(fw.applied_iface, fw.applied, fw.applied_rules);
}

/* The ask a setter replaces, taken before it rebuilds fw.want. */
struct fw_ask {
	char iface[FW_IFACE_MAX];
	struct fw_rule rules[FW_RULE_MAX];
	unsigned int count;
};

static void fw_ask_take(struct fw_ask *out)
{
	str_copy(out->iface, sizeof(out->iface), fw.want_iface);
	memcpy(out->rules, fw.want, sizeof(out->rules));
	out->count = fw.wants;
}

/*
 * Whether the ask differs from what the firewall holds or is about to hold,
 * read before the kick that acts on it. While a chain runs, its snapshot is
 * what the firewall will hold when it ends, and fw_chain_done() runs another
 * chain for the difference; while a retry is armed, the failed chain's
 * snapshot is what the timer re-runs, and its failure line already reported
 * it; without the helper nothing is ever applied and the kick's snapshot of
 * the last ask stands in. In each of those states an ask that repeats the one
 * already waiting is no news either. The applied set is the reference
 * otherwise.
 */
static bool fw_want_pending(const struct fw_ask *prev)
{
	if (fw.proc_active || fw.retry.pending ||
	    (!fw.test_mode && !fw_available()))
		return !fw_want_eq(fw.run_iface, fw.run, fw.runs) &&
		       !fw_want_eq(prev->iface, prev->rules, prev->count);

	return !fw_desired_applied();
}

/* what the chain that just finished installed, never what is wanted now */
static void fw_snapshot(void)
{
	fw.applied_valid = true;
	fw.applied_rules = fw.runs;
	memcpy(fw.applied, fw.run, sizeof(fw.applied));
	str_copy(fw.applied_iface, sizeof(fw.applied_iface), fw.run_iface);
}

/*
 * The delete list is the whole rule namespace, not only the set we believe is
 * applied: a channel that was enabled before a reload, or before a restart,
 * leaves its rule behind and nothing else would ever remove it. The proxy names
 * are deleted unconditionally for the same reason, and one more: a config that
 * leaves restrict_source, or one whose desired address set shrank, would
 * otherwise keep admitting a source the current config does not name. The
 * dns_linger retention widens the desired set upstream, inside its own bounded
 * window; it changes nothing here, because a lingering address that expired
 * simply stops being asked for and this delete removes its rule.
 */
static void fw_del_list_build(void)
{
	unsigned int n = 0;
	unsigned int i;

	str_copy(fw.del[n], sizeof(fw.del[n]), FW_SIP_RULE);
	n++;

	for (i = 0; i < FW_RTP_MAX; i++, n++)
		snprintf(fw.del[n], sizeof(fw.del[n]), FW_RTP_RULE, i + 1);

	for (i = 0; i < FW_PROXY_MAX; i++, n++)
		snprintf(fw.del[n], sizeof(fw.del[n]), FW_PROXY_RULE, i + 1);

	fw.dels = n;
}

/*
 * The argv of one step, used by the runner and by the trace alike, so the
 * trace reports what runs. Every step but the last carries -r, which suppresses
 * the fw4 reload: the uci edits accumulate and the running ruleset changes
 * once, at the end of the chain.
 */
static unsigned int fw_step_argv(unsigned int idx, char *argv[FW_ARGV_MAX])
{
	unsigned int n = 0;

	argv[n++] = (char *)FW_SCRIPT;

	if (idx < fw.dels) {
		argv[n++] = (char *)"-d";
		argv[n++] = fw.del[idx];
	} else {
		struct fw_rule *r = &fw.run[idx - fw.dels];

		argv[n++] = (char *)"-a";
		argv[n++] = r->name;
		argv[n++] = (char *)"-p";
		argv[n++] = r->ports;
		argv[n++] = (char *)"-P";
		argv[n++] = r->udp ? (char *)"udp" : (char *)"tcp";

		/* -s becomes the rule's src_ip, so only that source reaches the
		 * open ports. The helper parses with getopts and takes any
		 * order. */
		if (r->src[0]) {
			argv[n++] = (char *)"-s";
			argv[n++] = r->src;
		}

		argv[n++] = (char *)"-i";
		argv[n++] = fw.run_iface;
	}

	if (idx + 1 < fw.steps)
		argv[n++] = (char *)"-r";

	argv[n] = NULL;

	return n;
}

unsigned int fw_plan_count(void)
{
	return fw.steps;
}

const char *fw_plan_step(unsigned int idx, char *out, size_t olen)
{
	char *argv[FW_ARGV_MAX];
	unsigned int i, n;
	size_t used = 0;

	if (!out || !olen)
		return NULL;

	out[0] = 0;
	if (idx >= fw.steps)
		return NULL;

	n = fw_step_argv(idx, argv);
	for (i = 0; i < n && used < olen; i++)
		used += (size_t)snprintf(out + used, olen - used, i ? " %s" : "%s", argv[i]);

	return out;
}

static bool fw_step_is_add(unsigned int idx)
{
	return idx >= fw.dels;
}

static const char *fw_step_name(unsigned int idx)
{
	return fw_step_is_add(idx) ? fw.run[idx - fw.dels].name : fw.del[idx];
}

static void fw_retry_cb(struct uloop_timeout *t)
{
	fw_kick();
}

static unsigned int fw_retry_delay(void)
{
	unsigned int shift;
	unsigned int ms;

	if (!fw.fails)
		return FW_RETRY_MS;

	shift = fw.fails < 5 ? fw.fails - 1 : 4;
	ms = FW_RETRY_MS << shift;

	return ms > FW_RETRY_MAX_MS ? FW_RETRY_MAX_MS : ms;
}

static void fw_chain_done(bool ok)
{
	unsigned int delay;

	fw.proc_active = false;
	fw.steps = 0;
	fw.cur = 0;

	if (ok) {
		fw.fails = 0;
		fw_snapshot();

		/*
		 * Coalesce. The comparison is against what the chain
		 * installed, so a set that arrived while it ran is picked up
		 * here even though the chain could not carry it.
		 */
		if (!fw_desired_applied())
			fw_kick();
		return;
	}

	/*
	 * The failed chain committed its deletes before it died, so what is
	 * installed is unknown and the applied set must not vouch for it. No
	 * synchronous kick here: a failure never snapshots, so want never
	 * equals applied and an immediate re-kick would re-run the chain
	 * without pause, which is the hot loop this timer exists to prevent.
	 */
	fw.applied_valid = false;
	fw.fails++;
	delay = fw_retry_delay();

	if (fw.fails == 1 || delay != fw.retry_delay || !(fw.fails % FW_RETRY_LOG_EVERY))
		ULOG_ERR("firewall reconcile failed %u time(s); next attempt in %u ms\n",
			 fw.fails, delay);

	fw.retry_delay = delay;
	fw.retry.cb = fw_retry_cb;
	uloop_timeout_set(&fw.retry, delay);
}

static void fw_step_complete(int code)
{
	const char *name = fw_step_name(fw.cur);

	/*
	 * 2 is the helper's "committed but the fw4 reload failed". Only the
	 * last step reloads, and a clear chain ends on a delete, so this must
	 * come before the delete retry: that retry would re-run the reload at
	 * once and burn itself on a rule that is already gone.
	 */
	if (code == 2 && fw.cur + 1 == fw.steps) {
		ULOG_ERR("firewall rules committed but the fw4 reload failed (%d); "
			 "ruleset stale, retrying\n", code);
		fw_chain_done(false);
		return;
	}

	if (code != 0 && !fw_step_is_add(fw.cur)) {
		/*
		 * rule_add is a no op when the rule already exists, so a failed
		 * delete would leave the stale rule and the add would silently
		 * keep the old ports. Retry the delete once, then report rather
		 * than add on top of a rule we could not remove.
		 */
		if (fw.del_retries < 1) {
			fw.del_retries++;
			ULOG_WARN("firewall delete of %s failed (%d), retrying\n", name, code);
			fw_step_run();
			return;
		}

		ULOG_ERR("firewall delete of %s failed (%d); not adding to avoid a stale "
			 "rule\n", name, code);
		fw_chain_done(false);
		return;
	}

	if (code != 0) {
		/* the last step carries the fw4 reload, so its status can be
		 * the reload's, not the add's; say so */
		bool last = fw.cur + 1 == fw.steps;

		ULOG_ERR("firewall add of %s %sfailed (%d)\n", name,
			 last ? "or the final fw4 reload " : "", code);
		fw_chain_done(false);
		return;
	}

	if (fw_step_is_add(fw.cur)) {
		struct fw_rule *r = &fw.run[fw.cur - fw.dels];

		ULOG_INFO("firewall: %s open, %s ports '%s' iface '%s' source '%s'\n", r->name,
			  r->udp ? "udp" : "tcp", r->ports, fw.run_iface,
			  r->src[0] ? r->src : "any");
	}

	fw.del_retries = 0;
	fw.cur++;

	if (fw.cur < fw.steps) {
		fw_step_run();
		return;
	}

	if (!fw.runs)
		ULOG_INFO("firewall: VoIP rules removed\n");

	fw_chain_done(true);
}

static void fw_proc_cb(struct uloop_process *p, int ret)
{
	fw_step_complete(WIFEXITED(ret) ? WEXITSTATUS(ret) : -1);
}

static void fw_step_run(void)
{
	char *argv[FW_ARGV_MAX];
	char line[FW_CMD_MAX];
	pid_t pid;

	fw_step_argv(fw.cur, argv);
	TRACE(TRACE_FSM, "firewall: %s", fw_plan_step(fw.cur, line, sizeof(line)));

	fw.spawned++;

	/* the fixture completes the step by hand; nothing is spawned */
	if (fw.test_mode) {
		fw.proc_active = true;
		return;
	}

	pid = fw_spawn(argv);
	if (pid < 0) {
		fw_chain_done(false);
		return;
	}

	fw.proc.pid = pid;
	fw.proc.cb = fw_proc_cb;
	fw.proc_active = true;
	uloop_process_add(&fw.proc);
}

void fw_test_mode(bool on)
{
	/* a pending retry is linked into uloop's static timeout list; zeroing
	 * the struct would leave a dangling node behind for the test classes
	 * that run a live uloop afterwards */
	uloop_timeout_cancel(&fw.retry);

	memset(&fw, 0, sizeof(fw));
	fw.test_mode = on;
}

void fw_test_step_done(int code)
{
	if (fw.proc_active)
		fw_step_complete(code);
}

unsigned int fw_test_spawned(void)
{
	return fw.spawned;
}

unsigned int fw_test_retry_ms(void)
{
	return fw.retry.pending ? fw.retry_delay : 0;
}

bool fw_test_retry_fire(void)
{
	if (!fw.retry.pending)
		return false;

	/* what the uloop dispatch does: clear pending, then run the cb */
	uloop_timeout_cancel(&fw.retry);
	fw.retry.cb(&fw.retry);

	return true;
}

/* the snapshot the chain will execute, fixed from here on */
static void fw_run_snapshot(void)
{
	memcpy(fw.run, fw.want, sizeof(fw.run));
	fw.runs = fw.wants;
	str_copy(fw.run_iface, sizeof(fw.run_iface), fw.want_iface);

	fw_del_list_build();
	fw.steps = fw.dels + fw.runs;
	fw.cur = 0;
}

static void fw_kick(void)
{
	char line[FW_CMD_MAX];
	unsigned int i;

	/*
	 * A chain is running. It keeps executing the snapshot it started with,
	 * untouched; fw_chain_done() compares the desired set against what that
	 * chain installed and starts another one for the difference.
	 */
	if (fw.proc_active)
		return;

	/* before the retry guard: cancelling the retry and then returning
	 * here would leave nothing armed at all */
	if (!fw.test_mode && !fw_available()) {
		fw_run_snapshot();
		TRACE(TRACE_FSM, "firewall: %s absent, %u VoIP rule steps skipped", FW_SCRIPT,
		      fw.steps);
		for (i = 0; i < fw.steps; i++)
			TRACE(TRACE_FSM, "firewall: would run %s",
			      fw_plan_step(i, line, sizeof(line)));
		return;
	}

	/*
	 * A retry is pending and fw.run still holds the set the failed chain
	 * ran. That same set re-asserted (the ~30 s DNS TTL redelivery) must
	 * wait out the backoff or the event rate defeats it. A new set earns
	 * one immediate attempt, but the ladder resets on success alone: a
	 * voice reload legitimately delivers three distinct sets within a
	 * second, and a per-set reset would hold the backoff at its bottom
	 * rung through a persistent reload failure.
	 */
	if (fw.retry.pending) {
		if (fw_want_eq(fw.run_iface, fw.run, fw.runs))
			return;

		uloop_timeout_cancel(&fw.retry);
	}

	fw_run_snapshot();

	if (fw_desired_applied()) {
		fw.steps = 0;
		return;
	}

	fw.del_retries = 0;
	fw_step_run();
}

static void fw_rule_set(struct fw_rule *r, const char *name, const char *ports, bool udp,
			const char *src)
{
	str_copy(r->name, sizeof(r->name), name);
	str_copy(r->ports, sizeof(r->ports), ports);
	str_copy(r->src, sizeof(r->src), src);
	r->udp = udp;
}

/*
 * The signalling rules: the open one, or one per proxy address. They carry the
 * same ports and the same proto, because they describe the same local SIP
 * sockets; only the admitted source differs.
 */
static void fw_sig_rules_set(const char *sip_ports, bool proto_udp,
			     const char (*proxy)[FW_SRC_MAX], unsigned int nproxy, bool sip_open)
{
	char name[FW_NAME_MAX];
	unsigned int i;

	if (sip_open) {
		fw_rule_set(&fw.want[fw.wants++], FW_SIP_RULE, sip_ports, proto_udp, NULL);
		return;
	}

	for (i = 0; i < nproxy && i < FW_PROXY_MAX && fw.wants < FW_RULE_MAX; i++) {
		/* an empty entry would carry no -s, which is the open rule
		 * under a proxy name: the ports would face the whole wan */
		if (!proxy[i][0])
			continue;

		snprintf(name, sizeof(name), FW_PROXY_RULE, i + 1);
		fw_rule_set(&fw.want[fw.wants++], name, sip_ports, proto_udp, proxy[i]);
	}
}

bool fw_voip_set(const char *iface, const char *sip_ports, bool proto_udp,
		 const struct fw_rtp_range *rtp, unsigned int rtp_count,
		 const char (*proxy)[FW_SRC_MAX], unsigned int nproxy, bool sip_open)
{
	char ports[FW_PORTS_MAX];
	char name[FW_NAME_MAX];
	struct fw_ask prev;
	unsigned int i;
	bool pending;

	if (!sip_ports || !*sip_ports)
		return fw_voip_clear();

	fw_ask_take(&prev);
	memset(fw.want, 0, sizeof(fw.want));
	fw.wants = 0;
	str_copy(fw.want_iface, sizeof(fw.want_iface), (iface && *iface) ? iface : "wan");

	fw_sig_rules_set(sip_ports, proto_udp, proxy, nproxy, sip_open);

	/*
	 * One rule per enabled channel, named by its 1-based channel number
	 * (fw.h). RTP is always udp, whatever transport the signalling uses.
	 */
	for (i = 0; i < rtp_count && fw.wants < FW_RULE_MAX; i++) {
		unsigned int top;

		if (!rtp[i].channel || !rtp[i].min || rtp[i].max < rtp[i].min)
			continue;

		/*
		 * The rule admits the pair, not the bare range. The allocator
		 * hands out even RTP ports across the whole range, and the RTCP
		 * port is always the odd one above. A range that ends on an
		 * even port therefore puts the RTCP of its top pair one past
		 * max. A rule that stops at max would drop the RTCP of that
		 * call while its audio flowed.
		 */
		top = rtp[i].max + 1u;
		if (top > 65535)
			top = 65535;

		snprintf(name, sizeof(name), FW_RTP_RULE, rtp[i].channel);
		snprintf(ports, sizeof(ports), "%u-%u", rtp[i].min, top);
		fw_rule_set(&fw.want[fw.wants++], name, ports, true, NULL);
	}

	pending = fw_want_pending(&prev);
	fw_kick();

	return pending;
}

bool fw_voip_clear(void)
{
	struct fw_ask prev;
	bool pending;

	fw_ask_take(&prev);
	memset(fw.want, 0, sizeof(fw.want));
	fw.wants = 0;
	pending = fw_want_pending(&prev);
	fw_kick();

	return pending;
}

void fw_shutdown(void)
{
	char *argv[FW_ARGV_MAX];
	unsigned int i;
	pid_t pid;

	uloop_timeout_cancel(&fw.retry);

	if (fw.proc_active) {
		uloop_process_delete(&fw.proc);
		waitpid(fw.proc.pid, NULL, 0);
		fw.proc_active = false;
	}

	if (!fw_available())
		return;

	/* the whole namespace, synchronously, with the reload on the last one */
	fw.wants = 0;
	fw.runs = 0;
	fw_del_list_build();
	fw.steps = fw.dels;

	for (i = 0; i < fw.steps; i++) {
		fw_step_argv(i, argv);
		pid = fw_spawn(argv);
		if (pid > 0)
			waitpid(pid, NULL, 0);
	}

	fw.steps = 0;
	ULOG_INFO("firewall: VoIP rules removed at exit\n");
}
