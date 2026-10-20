// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * VoIP firewall reconcile.
 *
 * The device firewall is fw4, driven through /usr/srg/scripts/firewall_exception
 * (a helper that serializes on a lock). This layer keeps the VoIP ingress
 * rules in step with the running accounts: Allow_VoIP_SIP for the configured
 * local SIP ports, one Allow_VoIP_PROXY<n> per resolved proxy address when the
 * caller restricts the source, and one Allow_VoIP_RTP_CH<n> per enabled
 * channel for that channel's RTP port range, all on the voice interface.
 * Without the RTP rules a call comes up and the far end hears the handset
 * while the handset hears nothing: the inbound stream is dropped at the wan
 * firewall.
 *
 * One desired set. The caller computes the whole rule set in one place and
 * hands it over; this layer diffs it against what it last applied and drives
 * the difference. Nothing here decides which rules should exist.
 *
 * Asynchronous. firewall_exception can wait up to 30 s on the firewall lock and
 * then reload fw4, so it must not run on the uloop thread. The helper spawns
 * the script and reaps it through uloop_process, runs the delete then add chain
 * one child at a time, and coalesces rapid updates into a single reconcile. The
 * set and clear entry points only record the desired state and return at once.
 *
 * One reload. Every step but the last carries -r, which suppresses the fw4
 * reload. The uci edits accumulate and the running ruleset changes once, at
 * the end, so re-adding the SIP rule for an unrelated RTP change never opens a
 * window with no SIP rule in force.
 *
 * Failure backoff. A failed chain already committed its deletes, so what is
 * installed is unknown: the failure invalidates the applied set and arms a
 * retry that doubles from FW_RETRY_MS to FW_RETRY_MAX_MS. Only a completed
 * chain resets the ladder. A new desired set attempts immediately without
 * resetting it, the same set re-asserted (the ~30 s DNS TTL redelivery) waits
 * out the backoff, and a set changed while a chain runs rides that chain's
 * completion: a success coalesces at once, a failure hands it to the retry.
 *
 * Off target. The script is absent on the x86 build host and on any bench that
 * is not a device. Every entry point detects that and becomes a no op with a
 * trace, so the daemon and the host live registration test run without it.
 *
 * restrict_source lives in the caller. This layer takes a proxy address list
 * and a sip_open flag, and materializes what it is given: the open
 * Allow_VoIP_SIP rule, or one Allow_VoIP_PROXY<n> rule per address, that
 * address being the rule's source. net/fw_reconcile.c maps the
 * restrict_source option onto those two arguments (fw_reconcile_apply), so no
 * policy lives here.
 *
 * Whole namespace delete. Every chain deletes every name this layer can write,
 * the FW_PROXY_MAX proxy names included, before it adds anything. A config that
 * drops from three proxy addresses to one, or that goes back to the open rule,
 * leaves rules behind that nothing else would remove. The helper commits each
 * delete, so a chain that only deletes is durable. Its last step carries the
 * fw4 reload even when the rule it names is absent (firewall_exception
 * del_rule and firewall_reload_carry).
 */
#ifndef UVOICE_FW_H
#define UVOICE_FW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Distinct proxy addresses that can hold a rule. The delete list carries every
 * one of these names on every chain, so raising it costs a step per name. A
 * resolved answer carries up to sixteen endpoints and the registration accepts
 * all of them (sip/reg.h SIP_REG_TARGET_MAX), so sixteen rules cover every
 * source a restricted rule set has to admit. A seventeenth address gets no
 * rule, and the caller reports it.
 */
#define FW_PROXY_MAX		16

/* One SIP rule, CFG_CHANNEL_MAX RTP rules, FW_PROXY_MAX proxy rules */
#define FW_RULE_MAX		19

/* An IPv4 or IPv6 literal, the source address of a proxy rule. */
#define FW_SRC_MAX		48

/* Failure backoff: the first retry, doubling to the cap. */
#define FW_RETRY_MS		5000
#define FW_RETRY_MAX_MS		60000

/*
 * One channel's RTP range. channel is the 1-based channel number of the
 * rule name (Allow_VoIP_RTP_CH1 for line 0), min and max are the effective
 * range from net_port_range().
 */
struct fw_rtp_range {
	unsigned int channel;
	uint16_t min;
	uint16_t max;
};

/*
 * Ask for the whole VoIP rule set. sip_ports is the dest_port value of the SIP
 * and the proxy rules, a single port or a space separated list ("5060" or
 * "5060 5062"); proto_udp selects udp against tcp for them. rtp names one range
 * per enabled channel and each becomes its own udp rule. A NULL or empty iface
 * takes "wan". Empty sip_ports is treated as a clear. The reconcile runs
 * asynchronously; this call returns at once.
 *
 * sip_open true installs the open Allow_VoIP_SIP rule and ignores proxy;
 * sip_open false installs one Allow_VoIP_PROXY<n> rule per proxy entry, n being
 * the 1-based index in the list, each restricted to that source address. Up to
 * FW_PROXY_MAX entries are read. The two forms are exclusive by construction:
 * the caller decides which one describes the config.
 *
 * Returns true when the ask differs from what the firewall holds, or from what
 * the running chain installs while one runs, or from what the armed retry will
 * re-run after a failure, so a chain runs for this ask now or when the running
 * one ends; false for an ask the firewall holds or already has under way, and
 * for an ask that repeats the one already waiting behind the running chain.
 * The caller logs the ask at info on true and traces it on false: the failure
 * line of a chain already names the retry, so the ask it repeats is not news.
 */
bool fw_voip_set(const char *iface, const char *sip_ports, bool proto_udp,
		 const struct fw_rtp_range *rtp, unsigned int rtp_count,
		 const char (*proxy)[FW_SRC_MAX], unsigned int nproxy, bool sip_open);

/* Ask to remove every VoIP rule. Runs asynchronously; absent script no op.
 * Answers as fw_voip_set() does. */
bool fw_voip_clear(void);

/*
 * Final teardown at process exit. Cancels a pending retry, reaps a running
 * child, and removes the rules synchronously so they do not outlive the
 * daemon. uloop must still be valid. Absent script is a no op.
 */
void fw_shutdown(void);

/* Whether the firewall_exception script is present and executable. */
bool fw_available(void);

/*
 * Test mode, never used by the daemon. fw_test_mode() resets the layer and
 * makes it believe the helper exists while suppressing the spawn, so the
 * fixture completes each step by hand with fw_test_step_done() and can change
 * the desired set while a chain is in flight. fw_test_retry_ms() reads the
 * delay of the pending failure retry (0 when none) and fw_test_retry_fire()
 * fires a pending retry the way the uloop dispatch would.
 */
void fw_test_mode(bool on);
void fw_test_step_done(int code);
unsigned int fw_test_spawned(void);
unsigned int fw_test_retry_ms(void);
bool fw_test_retry_fire(void);

/*
 * The argument vector a step would run, for the trace and for the unit test:
 * step 0 upward, NULL once the list ends. It is the desired set as it stands,
 * so it is only meaningful right after fw_voip_set(). Exported because the
 * script is absent off target and the commands are the only observable.
 */
unsigned int fw_plan_count(void);
const char *fw_plan_step(unsigned int idx, char *out, size_t olen);

#endif
