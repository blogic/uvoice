// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The order of the classes in main() matters. Every class up to
 * test_early_reload() runs in the pre-start window, before the first
 * accounts_start(). test_early_reload() checks that a command in that window
 * builds nothing. test_tls_cert_trigger() runs next. It calls accounts_start()
 * and sets vr.started with it, but its config declares no channel and builds
 * nothing.
 *
 * The live classes bring lines up on loopback registrars. test_view_device(),
 * test_view_service_route() and test_grace_reload() run last, in that order.
 * Each of the three gives its lines back and closes its uloop.
 * test_grace_reload() must be last: it ends with accounts_shutdown_begin(),
 * and accounts_reload() refuses every reload after that call.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubox/ulog.h>
/* the enum ubus_msg_status values the netifd retry predicate reads */
#include <ubusmsg.h>

#include "proto/sip/account.h"
#include "proto/sip/call.h"
#include "calltrace.h"
#include "cfg/cfg.h"
#include "event.h"
#include "fw.h"
#include "hal/hal.h"
#include "net/conntrack.h"
#include "net/fw_reconcile.h"
#include "net/iface.h"
#include "net/ports.h"
#include "proto/proto.h"
#include "proto/sip/keepalive.h"
#include "proto/sip/line.h"
#include "proto/sip/reg.h"
#include "proto/shared/resolve.h"
#include "proto/sip/stream.h"
#include "proto/sip/sub.h"
#include "proto/sip/tap.h"
#include "proto_host_fake.h"
#include "test_util.h"
#include "test_warn.h"

/*
 * The SIP line machine is not linked into this suite (src/CMakeLists.txt
 * builds it from account.c and call.c alone), so the one machine entry point
 * account.c reaches for is recorded here, the way test_line.c records the call
 * layer.
 */
static unsigned int mwi_pushes;
static unsigned int mwi_push_line;

void sip_line_mwi_push(unsigned int line)
{
	mwi_pushes++;
	mwi_push_line = line;
}

/*
 * The call trace controller is not linked into this suite either, so its one
 * notification is recorded here. It counts the calls and keeps the line and
 * the reason word of the last one, so a case can say that a registration edge
 * asked for a snapshot and named the right line.
 */
static unsigned int calltrace_events;
static unsigned int calltrace_line;
static char calltrace_reason[32];

void calltrace_event(unsigned int line, const char *reason)
{
	calltrace_events++;
	calltrace_line = line;
	snprintf(calltrace_reason, sizeof(calltrace_reason), "%s",
		 reason ? reason : "");
}

static void calltrace_record_reset(void)
{
	calltrace_events = 0;
	calltrace_line = 0;
	calltrace_reason[0] = '\0';
}

static struct cfg base;

static void base_default(void)
{
	memset(&base, 0, sizeof(base));
	base.enabled = true;
	base.channels = 2;
	snprintf(base.local_interface, sizeof(base.local_interface), "wan");
	snprintf(base.transport, sizeof(base.transport), "udp");
	base.dns_cache_update_mode = true;
	/* the schema default, so the live classes below run the shipped
	 * behaviour and not the option the memset turned off */
	base.dns_move_unregister = true;
}

/*
 * The start and the reload of the account layer in the order main.c runs them
 * around the core services: the tracker and the firewall take the config
 * first, then the protocol does.
 */
static void core_start(const struct cfg *cfg)
{
	net_iface_init(cfg);
	fw_reconcile_cfg_take(cfg);
	accounts_start(cfg);
}

static void core_reload(const struct cfg *old_cfg, const struct cfg *new_cfg)
{
	net_iface_iface_set(new_cfg->local_interface);
	fw_reconcile_cfg_take(new_cfg);
	accounts_reload(old_cfg, new_cfg);
}

static void chan_default(struct cfg_channel *ch, unsigned int id)
{
	memset(ch, 0, sizeof(*ch));
	ch->present = true;
	ch->enabled = true;
	ch->id = id;
	ch->sip_port = 5060;
	ch->sip_local_port = 5060;
	ch->sip_tos = 104;
	ch->sip_registration_expiry = 3600;
	ch->sip_register_retry = 5;
	ch->timers.t1 = 500;
	ch->timers.t2 = 4000;
	ch->timers.t4 = 5000;
}

/* --------------------------------------------------------------- derive */

static void test_derive(void)
{
	struct cfg_channel ch;
	struct acct_cfg c;

	base_default();

	CASE("derive: proxy only builds aor, registrar and resolve host");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "5551000");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "sip.example.com");
	snprintf(ch.sip_secret, sizeof(ch.sip_secret), "s3cr3t");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.enabled);
	CHECK(!strcmp(c.aor, "sip:5551000@sip.example.com"));
	CHECK(!strcmp(c.registrar_uri, "sip:sip.example.com"));
	CHECK(!strcmp(c.resolve_host, "sip.example.com"));
	CHECK(c.resolve_port == 5060);
	CHECK(!strcmp(c.contact_user, "5551000"));
	CHECK(!strcmp(c.auth_user, "5551000"));
	CHECK(!strcmp(c.auth_pass, "s3cr3t"));
	CHECK(c.transport == ACCT_TP_UDP);

	CASE("derive: explicit domain differs from proxy in the aor");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "proxy.example.com");
	snprintf(ch.sip_domain, sizeof(ch.sip_domain), "domain.example.com");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.aor, "sip:u@domain.example.com"));
	/* registrar defaults to domain */
	CHECK(!strcmp(c.registrar_uri, "sip:domain.example.com"));
	/* resolve host is the outbound proxy, which defaults to proxy */
	CHECK(!strcmp(c.resolve_host, "proxy.example.com"));

	CASE("derive: outbound proxy becomes the resolve host");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "proxy.example.com");
	snprintf(ch.sip_outbound_proxy, sizeof(ch.sip_outbound_proxy), "edge.example.com");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.resolve_host, "edge.example.com"));

	CASE("derive: empty proxy inherits domain");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_domain, sizeof(ch.sip_domain), "only.example.com");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.resolve_host, "only.example.com"));
	CHECK(!strcmp(c.aor, "sip:u@only.example.com"));

	CASE("derive: sip_port feeds the resolve port through the chain");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	ch.sip_port = 5080;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.resolve_port == 5080);

	CASE("derive: explicit outbound proxy port wins over sip_port");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	ch.sip_port = 5080;
	ch.sip_outbound_proxy_port = 5090;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.resolve_port == 5090);

	/* The schema inheritance rules apply to each dependent option. */
	CASE("derive: absent dependent ports both inherit sip_port");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	ch.sip_port = 5080;
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(c.resolve_port, 5080);
	CHECK(!strcmp(c.registrar_uri, "sip:p.example.com:5080"));

	CASE("derive: sip_auth_username overrides the username for auth");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "line1");
	snprintf(ch.sip_auth_username, sizeof(ch.sip_auth_username), "authid");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.auth_user, "authid"));
	CHECK(!strcmp(c.contact_user, "line1"));

	CASE("derive: explicit registrar builds the request URI");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	snprintf(ch.sip_registrar, sizeof(ch.sip_registrar), "reg.example.com");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.registrar_uri, "sip:reg.example.com"));

	CASE("derive: callerid name becomes the display name");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	snprintf(ch.sip_callerid_name, sizeof(ch.sip_callerid_name), "Front Desk");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.display, "Front Desk"));

	CASE("derive: channel local_interface overrides the base");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	snprintf(ch.local_interface, sizeof(ch.local_interface), "voice");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.iface, "voice"));

	CASE("derive: base local_interface used when the channel is silent");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.iface, "wan"));

	CASE("derive: local port and tos carry through");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	ch.sip_local_port = 5062;
	ch.sip_tos = 96;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.local_port == 5062);
	CHECK(c.sip_tos == 96);

	CASE("derive: a channel with no proxy or domain is not registrable");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!c.enabled);

	CASE("derive: a disabled channel is not registrable");
	chan_default(&ch, 0);
	ch.enabled = false;
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!c.enabled);

	CASE("derive: the NAT keepalive comes from the base section");
	base.keepalive_enabled = true;
	base.keepalive_interval = 60;
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.ka_enabled);
	CHECK_UINT(c.ka_interval, 60);

	CASE("derive: a disabled NAT keepalive carries through");
	base.keepalive_enabled = false;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!c.ka_enabled);
	base.keepalive_enabled = true;

	/* the SUBSCRIBE client is base scope; the unsolicited intake runs
	 * whatever it says, so nothing here gates the NOTIFY path */
	CASE("derive: the mwi subscription comes from the base section");
	base.mwi_subscribe = true;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.mwi_subscribe);
	base.mwi_subscribe = false;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!c.mwi_subscribe);

	CASE("derive: the base options_interval applies when the channel is silent");
	base.options_interval = 90;
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(c.options_interval, 90);

	CASE("derive: a nonzero channel options_interval wins over the base");
	ch.options_interval = 120;
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(c.options_interval, 120);

	CASE("derive: a channel options_interval of 0 takes the base value");
	ch.options_interval = 0;
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(c.options_interval, 90);

	CASE("derive: both at 0 leaves the OPTIONS ping off");
	base.options_interval = 0;
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(c.options_interval, 0);

	CASE("derive: a channel value alone still reaches the account");
	ch.options_interval = 45;
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(c.options_interval, 45);
	base.options_interval = 0;

	CASE("derive: dns_cache_update_mode comes from the base");
	base.dns_cache_update_mode = false;
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!c.dns_cache_update_mode);
	base.dns_cache_update_mode = true;

	CASE("derive: dns_move_unregister comes from the base");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.dns_move_unregister);
	base.dns_move_unregister = false;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!c.dns_move_unregister);
	base.dns_move_unregister = true;

	/*
	 * A derive of line 2 must leave line 1's own derived view
	 * untouched. Two full struct cfg snapshots, old and new, differ in
	 * channel 2 alone. acct_cfg_derive(new_cfg,
	 * &new_cfg->channel[i], &nc) is exactly the call accounts_reload() makes
	 * per line, so cls below is the class a real reload would compute for
	 * line 1, not two derives of one channel that a reload never builds
	 * side by side.
	 */
	CASE("derive: a change to line 2 leaves the line 1 view identical");
	{
		static struct cfg old_cfg, new_cfg;
		struct acct_cfg c0a, c0b;
		enum acct_apply cls;

		base_default();
		old_cfg = base;
		old_cfg.channels = 2;
		chan_default(&old_cfg.channel[0], 0);
		snprintf(old_cfg.channel[0].sip_username,
			 sizeof(old_cfg.channel[0].sip_username), "5551000");
		snprintf(old_cfg.channel[0].sip_proxy,
			 sizeof(old_cfg.channel[0].sip_proxy), "p.example.com");
		chan_default(&old_cfg.channel[1], 1);
		snprintf(old_cfg.channel[1].sip_username,
			 sizeof(old_cfg.channel[1].sip_username), "5551001");
		snprintf(old_cfg.channel[1].sip_proxy,
			 sizeof(old_cfg.channel[1].sip_proxy), "p.example.com");

		new_cfg = old_cfg;
		snprintf(new_cfg.channel[1].sip_proxy,
			 sizeof(new_cfg.channel[1].sip_proxy), "q.example.com");

		acct_cfg_derive(&old_cfg, &old_cfg.channel[0], &c0a);
		acct_cfg_derive(&new_cfg, &new_cfg.channel[0], &c0b);

		CHECK(!memcmp(&c0a, &c0b, sizeof(c0a)));
		cls = acct_apply_class(&c0a, &c0b);
		CHECK(cls == ACCT_APPLY_NONE);

		CASE("derive: line 1 takes no reload action when only line 2 moved");
		CHECK(acct_reload_action(true, true, cls) == ACCT_ROP_NONE);
		base_default();
	}

	/* acct_cfg_derive() maps a sip_port of 0 to
	 * 5060 on its own (ch->sip_port ? ... : 5060); this case pins that
	 * mapping directly, apart from the loader's own presence rule. */
	CASE("derive: a sip_port of 0 on a udp line resolves with the 5060 fallback");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	ch.sip_port = 0;
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(c.resolve_port, 5060);
}

/*
 * A URI wraps an IPv6 host in square brackets (RFC 3261 19.1.1). Two of the
 * three derived strings are URIs and take them. The third is the name that
 * the resolver looks up, and the resolver takes a bare literal
 * (shared/resolve.h "Numeric targets").
 */
static void test_derive_v6(void)
{
	struct cfg_channel ch;
	struct acct_cfg c;

	base_default();

	CASE("derive: an IPv6 domain is bracketed in the address of record");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "5551000");
	snprintf(ch.sip_domain, sizeof(ch.sip_domain), "2001:db8::1");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "2001:db8::1");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.aor, "sip:5551000@[2001:db8::1]"));
	CHECK(!strcmp(c.registrar_uri, "sip:[2001:db8::1]"));
	CHECK(!strcmp(c.resolve_host, "2001:db8::1"));

	CASE("derive: an IPv6 domain with no user is bracketed too");
	chan_default(&ch, 0);
	snprintf(ch.sip_domain, sizeof(ch.sip_domain), "2001:db8::1");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "2001:db8::1");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.aor, "sip:[2001:db8::1]"));

	CASE("derive: an IPv6 registrar is bracketed with and without a port");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	snprintf(ch.sip_registrar, sizeof(ch.sip_registrar), "2001:db8::2");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.registrar_uri, "sip:[2001:db8::2]"));
	snprintf(ch.sip_registrar, sizeof(ch.sip_registrar),
		 "[2001:db8::2]:5070");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.registrar_uri, "sip:[2001:db8::2]:5070"));

	CASE("derive: the host the resolver chases stays bare, port and all");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "2001:db8::3");
	snprintf(ch.sip_outbound_proxy, sizeof(ch.sip_outbound_proxy),
		 "[2001:db8::4]:5062");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.resolve_host, "2001:db8::4"));
	CHECK_UINT(c.resolve_port, 5062);

	CASE("derive: an IPv4 host takes no brackets");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "192.0.2.10");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.aor, "sip:u@192.0.2.10"));
	CHECK(!strcmp(c.registrar_uri, "sip:192.0.2.10"));
	CHECK(!strcmp(c.resolve_host, "192.0.2.10"));

	CASE("derive: a name takes no brackets either");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "sip.example.com");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.aor, "sip:u@sip.example.com"));
	CHECK(!strcmp(c.registrar_uri, "sip:sip.example.com"));
}

/* ----------------------------------------------------------- apply class */

static void account_of(struct acct_cfg *c, const char *proxy)
{
	struct cfg_channel ch;

	base_default();
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "%s", proxy);
	snprintf(ch.sip_secret, sizeof(ch.sip_secret), "pass");
	acct_cfg_derive(&base, &ch, c);
}

static void test_apply_class(void)
{
	struct acct_cfg a, b;
	struct cfg_channel ch;

	CASE("apply: identical configs need no work");
	account_of(&a, "p.example.com");
	b = a;
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_NONE);

	CASE("apply: a timer move is live");
	account_of(&a, "p.example.com");
	b = a;
	b.t1 = 250;
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_LIVE);

	CASE("apply: an expiry change is a re-register");
	account_of(&a, "p.example.com");
	b = a;
	b.expires = 1800;
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_REREG);

	CASE("apply: a dns_move_unregister move is a re-register");
	account_of(&a, "p.example.com");
	b = a;
	b.dns_move_unregister = !a.dns_move_unregister;
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_REREG);

	CASE("apply: a password change is a re-register");
	account_of(&a, "p.example.com");
	b = a;
	snprintf(b.auth_pass, sizeof(b.auth_pass), "other");
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_REREG);

	CASE("apply: a proxy change is a re-register");
	account_of(&a, "p.example.com");
	account_of(&b, "q.example.com");
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_REREG);

	/* the subscription is built beside the registration, so the schema
	 * REREG class of mwi_subscribe rebuilds both */
	CASE("apply: turning the mwi subscription on is a re-register");
	account_of(&a, "p.example.com");
	b = a;
	b.mwi_subscribe = !a.mwi_subscribe;
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_REREG);

	CASE("apply: a local port change restarts the line");
	account_of(&a, "p.example.com");
	b = a;
	b.local_port = 5062;
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_RESTART);

	CASE("apply: a tos change restarts the line");
	account_of(&a, "p.example.com");
	b = a;
	b.sip_tos = 96;
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_RESTART);

	CASE("apply: an interface change restarts the line");
	account_of(&a, "p.example.com");
	b = a;
	snprintf(b.iface, sizeof(b.iface), "voice");
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_RESTART);

	CASE("apply: an enable toggle restarts the line");
	account_of(&a, "p.example.com");
	b = a;
	b.enabled = false;
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_RESTART);

	CASE("apply: a transport change restarts the line");
	account_of(&a, "p.example.com");
	b = a;
	b.transport = ACCT_TP_TCP;
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_RESTART);

	/* the two reliable transports are not one class: a line that moves
	 * from tcp to tls rebuilds the socket, the listener and the keys */
	CASE("apply: a move between the two stream transports restarts too");
	account_of(&a, "p.example.com");
	a.transport = ACCT_TP_TCP;
	b = a;
	b.transport = ACCT_TP_TLS;
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_RESTART);

	CASE("apply: the highest class wins when several fields move");
	account_of(&a, "p.example.com");
	b = a;
	b.t1 = 250;			/* live */
	b.expires = 1800;		/* rereg */
	b.local_port = 5062;		/* restart */
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_RESTART);

	CASE("apply: a keepalive enable toggle alone is live");
	account_of(&a, "p.example.com");
	b = a;
	b.ka_enabled = !a.ka_enabled;
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_LIVE);

	CASE("apply: a keepalive interval alone is live");
	account_of(&a, "p.example.com");
	b = a;
	b.ka_interval = a.ka_interval + 30;
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_LIVE);

	CASE("apply: an options_interval alone is live");
	account_of(&a, "p.example.com");
	b = a;
	b.options_interval = 120;
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_LIVE);

	CASE("apply: all three keepalive fields together are still live");
	account_of(&a, "p.example.com");
	b = a;
	b.ka_enabled = !a.ka_enabled;
	b.ka_interval = a.ka_interval + 30;
	b.options_interval = 120;
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_LIVE);

	CASE("apply: a keepalive move under an identity move keeps the higher class");
	account_of(&a, "p.example.com");
	b = a;
	b.options_interval = 120;	/* live */
	b.expires = 1800;		/* rereg */
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_REREG);

	CASE("apply: and under a transport move it keeps restart");
	account_of(&a, "p.example.com");
	b = a;
	b.ka_interval = 15;		/* live */
	b.local_port = 5062;		/* restart */
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_RESTART);

	/* the line resolves with the kind and expires the same key with it, so
	 * a move of the question rebuilds the line and registers again */
	CASE("apply: a move of the next-hop question is a re-register");
	account_of(&a, "p.example.com");
	b = a;
	b.dns_kind = (uint8_t)SIP_DNS_ADDR;
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_REREG);

	/*
	 * The comparison reads the derived kind and not the base word, so a
	 * mode move that leaves this line's question where it was costs the
	 * line nothing. The channel is built once and derived twice, because
	 * the word is a base field and account_of() puts the base back.
	 */
	CASE("apply: a mode move that leaves the question alone is no work");
	base_default();
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	base.dns_srv_mode = CFG_DNS_SRV_ALWAYS;
	acct_cfg_derive(&base, &ch, &a);
	base.dns_srv_mode = CFG_DNS_SRV_AUTO;
	acct_cfg_derive(&base, &ch, &b);
	CHECK(a.dns_kind == SIP_DNS_SRV);
	CHECK(b.dns_kind == SIP_DNS_SRV);
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_NONE);

	/* never asks no SRV even for a name with no address, so the question
	 * of a port-named line moves */
	CASE("apply: a move from auto to never on a port-named line "
	     "is a re-register");
	ch.sip_port_set = true;
	base.dns_srv_mode = CFG_DNS_SRV_AUTO;
	acct_cfg_derive(&base, &ch, &a);
	base.dns_srv_mode = CFG_DNS_SRV_NEVER;
	acct_cfg_derive(&base, &ch, &b);
	CHECK(a.dns_kind == SIP_DNS_ADDR_SRV);
	CHECK(b.dns_kind == SIP_DNS_ADDR);
	CHECK(acct_apply_class(&a, &b) == ACCT_APPLY_REREG);

	base_default();
}

/* ---------------------------------- the restrict_source 2 refresher bound */

static unsigned int ct_file_read(const char *path, unsigned int def)
{
	FILE *f = fopen(path, "r");
	unsigned int v;
	int n;

	if (!f)
		return def;

	n = fscanf(f, "%u", &v);
	fclose(f);

	return n == 1 ? v : def;
}

/* what the procfs read must answer on this host: the smaller of the two
 * values, with the shipped default in place of a file that is not there */
static unsigned int ct_host_bound(void)
{
	unsigned int t = ct_file_read("/proc/sys/net/netfilter/"
				      "nf_conntrack_udp_timeout", 60);
	unsigned int s = ct_file_read("/proc/sys/net/netfilter/"
				      "nf_conntrack_udp_timeout_stream", 180);

	return t < s ? t : s;
}

/*
 * restrict_source 2 installs no firewall rule, so the inbound service depends
 * on conntrack alone. A udp line therefore refreshes its SIP tuple inside half
 * the conntrack udp bound, whatever the keepalive options say
 * (acct_ka_interval_effective). The test hook fixes the bound, so the
 * conntrack values of the host play no part. The derive log line is the
 * second observable, because derive returns a config and not a diagnosis.
 */
static void test_refresher_bound(void)
{
	struct cfg_channel ch;
	struct acct_cfg c, d;
	bool forced;

	net_ct_udp_bound_test_set(60, 180);
	base_default();
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "5551000");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "sip.example.com");

	CASE("bound: mode 2 with a disabled keepalive is forced on at half "
	     "the bound");
	base.restrict_source = 2;
	base.keepalive_enabled = false;
	base.keepalive_interval = 0;
	base.options_interval = 0;
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.pinhole);
	CHECK_UINT(warn_capture_count("sip keepalive forced to 30 s"), 1);
	CHECK_UINT(warn_capture_count("60 s conntrack udp timeout"), 1);
	CHECK_UINT(acct_ka_interval_effective(&c, 60, &forced), 30);
	CHECK(forced);

	CASE("bound: a faster keepalive is kept");
	base.keepalive_enabled = true;
	base.keepalive_interval = 15;
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(warn_capture_count("sip keepalive forced"), 0);
	CHECK_UINT(acct_ka_interval_effective(&c, 60, &forced), 15);
	CHECK(!forced);

	CASE("bound: a slower keepalive is clamped to half the bound");
	base.keepalive_interval = 45;
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(warn_capture_count("sip keepalive forced to 30 s"), 1);
	CHECK_UINT(acct_ka_interval_effective(&c, 60, &forced), 30);
	CHECK(forced);

	CASE("bound: half the bound itself is no force");
	base.keepalive_interval = 30;
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(warn_capture_count("sip keepalive forced"), 0);
	CHECK_UINT(acct_ka_interval_effective(&c, 60, &forced), 30);
	CHECK(!forced);

	CASE("bound: the clamp follows the bound it is given");
	CHECK_UINT(acct_ka_interval_effective(&c, 180, &forced), 30);
	CHECK(!forced);
	CHECK_UINT(acct_ka_interval_effective(&c, 20, &forced), 10);
	CHECK(forced);

	/* the OPTIONS ping reaches the sticky target alone, so a fast one no
	 * longer stands in for the round */
	CASE("bound: a fast OPTIONS ping does not stand in for the keepalive");
	base.keepalive_enabled = false;
	base.options_interval = 20;
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(warn_capture_count("sip keepalive forced to 30 s"), 1);
	CHECK_UINT(c.options_interval, 20);
	base.options_interval = 0;

	CASE("bound: modes 0 and 1 keep the configured pair and log nothing");
	base.restrict_source = 1;
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!c.pinhole);
	CHECK(!c.ka_enabled);
	CHECK_UINT(warn_capture_count("keepalive"), 0);
	CHECK_UINT(warn_capture_count("restrict_source 2"), 0);
	base.restrict_source = 0;
	base.keepalive_enabled = true;
	base.keepalive_interval = 300;
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!c.pinhole);
	CHECK(c.ka_enabled);
	CHECK_UINT(c.ka_interval, 300);
	CHECK_UINT(warn_capture_count("keepalive"), 0);
	warn_capture_end();

	/*
	 * A stream line holds the pinhole with the connection itself and
	 * refreshes it with SO_KEEPALIVE, so the udp bound does not apply and
	 * the configured pair stands under every mode.
	 */
	CASE("bound: a tcp line under mode 2 keeps the pair off and says why");
	base.restrict_source = 2;
	base.keepalive_enabled = false;
	base.keepalive_interval = 0;
	snprintf(base.transport, sizeof(base.transport), "tcp");
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.pinhole);
	CHECK(!c.ka_enabled);
	CHECK_UINT(warn_capture_count("sip keepalive forced"), 0);
	CHECK_UINT(warn_capture_count("keepalive refreshes it"), 1);

	CASE("bound: a tls line keeps a slow pair and reads the same");
	snprintf(base.transport, sizeof(base.transport), "tls");
	base.keepalive_enabled = true;
	base.keepalive_interval = 300;
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(c.ka_interval, 300);
	CHECK_UINT(warn_capture_count("sip keepalive forced"), 0);
	CHECK_UINT(warn_capture_count("over tls"), 1);

	CASE("bound: a stream line under mode 1 says nothing at all");
	base.restrict_source = 1;
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(warn_capture_count("restrict_source 2"), 0);
	CHECK_UINT(warn_capture_count("keepalive"), 0);
	warn_capture_end();
	snprintf(base.transport, sizeof(base.transport), "udp");

	/* the mode reaches the account as one field, and a move of it is the
	 * live apply class: the running keepalive is re-armed and nothing is
	 * rebuilt */
	CASE("bound: a mode move alone is the live apply class");
	base.restrict_source = 2;
	base.keepalive_enabled = false;
	base.keepalive_interval = 0;
	acct_cfg_derive(&base, &ch, &c);
	base.restrict_source = 1;
	acct_cfg_derive(&base, &ch, &d);
	CHECK(c.pinhole && !d.pinhole);
	CHECK(acct_apply_class(&c, &d) == ACCT_APPLY_LIVE);
	CHECK(acct_apply_class(&d, &c) == ACCT_APPLY_LIVE);

	CASE("bound: 1 s floors the keepalive at 1 s and warns");
	net_ct_udp_bound_test_set(1, 180);
	base.restrict_source = 2;
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(warn_capture_count("under a 1 s conntrack udp timeout"), 1);
	CHECK_UINT(warn_capture_count("sip keepalive forced to 1 s"), 1);
	CHECK_UINT(acct_ka_interval_effective(&c, 1, &forced), 1);
	CHECK(forced);

	CASE("bound: 2 s is the shortest bound the floor can meet");
	net_ct_udp_bound_test_set(2, 180);
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(warn_capture_count("conntrack udp timeout:"), 0);
	CHECK_UINT(warn_capture_count("sip keepalive forced to 1 s"), 1);
	warn_capture_end();

	CASE("bound: the smaller of the two timeouts is the bound");
	net_ct_udp_bound_test_set(60, 20);
	CHECK_UINT(net_ct_udp_bound_s(), 20);
	net_ct_udp_bound_test_set(60, 180);
	CHECK_UINT(net_ct_udp_bound_s(), 60);

	CASE("bound: a timeout that cannot be read takes the shipped value");
	net_ct_udp_bound_test_set(0, 500);
	CHECK_UINT(net_ct_udp_bound_s(), 60);
	net_ct_udp_bound_test_set(500, 0);
	CHECK_UINT(net_ct_udp_bound_s(), 180);
	net_ct_udp_bound_test_set(0, 500);
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(warn_capture_count("sip keepalive forced to 30 s"), 1);
	warn_capture_end();

	CASE("bound: the procfs read answers the files of this host");
	net_ct_udp_bound_test_set(0, 0);
	CHECK_UINT(net_ct_udp_bound_s(), ct_host_bound());

	base_default();
}

/*
 * SDES puts the master keys in the SDP, so srtp_enabled over a transport that
 * does not encrypt the signaling exposes them (RFC 4568 7.2). The config stays
 * expressible, and derive says so once. The warning is therefore the only
 * observable: the derived account itself does not change.
 */
static void test_srtp_transport_warn(void)
{
	struct cfg_channel ch;
	struct acct_cfg plain;
	struct acct_cfg c;

	base_default();
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "5551000");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "sip.example.com");

	CASE("srtp warn: srtp over udp warns once and names udp");
	base.srtp_enabled = true;
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(warn_capture_count("srtp_enabled"), 1);
	CHECK_UINT(warn_capture_count("RFC 4568 7.2"), 1);
	/* the message names the transport it warns about, not a constant */
	CHECK_UINT(warn_capture_count("over udp"), 1);

	CASE("srtp warn: srtp over tcp warns once and names tcp");
	snprintf(base.transport, sizeof(base.transport), "tcp");
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(warn_capture_count("srtp_enabled"), 1);
	CHECK_UINT(warn_capture_count("over tcp"), 1);

	CASE("srtp warn: srtp over tls says nothing");
	snprintf(base.transport, sizeof(base.transport), "tls");
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(warn_capture_count("srtp_enabled"), 0);

	CASE("srtp warn: a line without srtp says nothing over udp");
	snprintf(base.transport, sizeof(base.transport), "udp");
	base.srtp_enabled = false;
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(warn_capture_count("srtp_enabled"), 0);

	/* the transport is base only, so a channel value never reaches the
	 * check */
	CASE("srtp warn: a channel transport of tls does not silence the warning");
	base.srtp_enabled = true;
	snprintf(ch.transport, sizeof(ch.transport), "tls");
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(warn_capture_count("srtp_enabled"), 1);
	ch.transport[0] = '\0';

	CASE("srtp warn: the warning leaves the derived account unchanged");
	base.srtp_enabled = false;
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &plain);
	base.srtp_enabled = true;
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(warn_capture_count("srtp_enabled"), 1);
	CHECK(!memcmp(&plain, &c, sizeof(c)));
	warn_capture_end();

	base_default();
}

/* ------------------------------------------------------- endpoint host:port */

static void test_host_port(void)
{
	char host[128];
	uint16_t port;

	CASE("split: a bare host yields port 0 and the whole host");
	CHECK(acct_host_port_split("sip.example.com", host, sizeof(host), &port));
	CHECK(!strcmp(host, "sip.example.com"));
	CHECK(port == 0);

	CASE("split: host:port separates the two");
	CHECK(acct_host_port_split("sip.example.com:5080", host, sizeof(host), &port));
	CHECK(!strcmp(host, "sip.example.com"));
	CHECK(port == 5080);

	CASE("split: an IPv4 address with a port");
	CHECK(acct_host_port_split("192.0.2.10:5062", host, sizeof(host), &port));
	CHECK(!strcmp(host, "192.0.2.10"));
	CHECK(port == 5062);

	CASE("split: a bracketed IPv6 literal with a port");
	CHECK(acct_host_port_split("[2001:db8::1]:5060", host, sizeof(host), &port));
	CHECK(!strcmp(host, "2001:db8::1"));
	CHECK(port == 5060);

	CASE("split: a bare IPv6 literal stays whole with no port");
	CHECK(acct_host_port_split("2001:db8::1", host, sizeof(host), &port));
	CHECK(!strcmp(host, "2001:db8::1"));
	CHECK(port == 0);

	CASE("split: a non-numeric or out of range port is not a port");
	CHECK(acct_host_port_split("host:99999", host, sizeof(host), &port));
	CHECK(!strcmp(host, "host:99999"));
	CHECK(port == 0);

	CASE("split: an empty input returns false");
	CHECK(!acct_host_port_split("", host, sizeof(host), &port));
	CHECK(port == 0);
}

static void test_derive_endpoint(void)
{
	struct cfg_channel ch;
	struct acct_cfg c;

	base_default();

	CASE("derive: an embedded proxy port never reaches the resolver whole");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "sip.example.com:5080");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.resolve_host, "sip.example.com"));
	CHECK(c.resolve_port == 5080);

	CASE("derive: the embedded port wins over sip_port");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "sip.example.com:5080");
	ch.sip_port = 5060;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.resolve_port == 5080);

	CASE("derive: sip_registrar_port is honored in the request URI");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	snprintf(ch.sip_registrar, sizeof(ch.sip_registrar), "reg.example.com");
	ch.sip_registrar_port = 5070;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.registrar_uri, "sip:reg.example.com:5070"));

	CASE("derive: a default registrar port is omitted from the request URI");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.registrar_uri, "sip:p.example.com"));

	CASE("derive: an embedded registrar port lands in the request URI");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	snprintf(ch.sip_registrar, sizeof(ch.sip_registrar), "reg.example.com:5090");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.resolve_host, "p.example.com"));
	CHECK(!strcmp(c.registrar_uri, "sip:reg.example.com:5090"));

	CASE("derive: an embedded domain port never appears in the aor host");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_domain, sizeof(ch.sip_domain), "domain.example.com:5061");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.aor, "sip:u@domain.example.com"));
}

/* --------------------------------------------------- the transport of a line */

/*
 * Transport is base only (account.h). The firewall reconcile writes one proto
 * for the whole rule set. Two lines on two transports would therefore take
 * the inbound SIP of one line down with no report. The base value is the only
 * one that reaches an account, and a channel that names another is reported
 * and ignored. It is the one option of the schema whose channel row is not an
 * alias, so the derive reports it.
 */
static void test_derive_transport(void)
{
	struct cfg_channel ch;
	struct acct_cfg c;

	base_default();
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "5551000");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "sip.example.com");

	CASE("transport: the base udp token derives udp");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.transport == ACCT_TP_UDP);

	CASE("transport: the base tcp token derives tcp");
	snprintf(base.transport, sizeof(base.transport), "tcp");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.transport == ACCT_TP_TCP);

	CASE("transport: the base tls token derives tls");
	snprintf(base.transport, sizeof(base.transport), "tls");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.transport == ACCT_TP_TLS);

	/*
	 * The empty token is the guard the four Via writers depend on: they
	 * default on a NULL token alone, so an empty string must never travel
	 * as one.
	 */
	CASE("transport: an empty base token is udp and is no fault");
	base.transport[0] = 0;
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.transport == ACCT_TP_UDP);
	CHECK_UINT(warn_capture_count("is not one of udp"), 0);

	CASE("transport: an unknown base token is udp and says so once");
	snprintf(base.transport, sizeof(base.transport), "sctp");
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.transport == ACCT_TP_UDP);
	CHECK_UINT(warn_capture_count("is not one of udp"), 1);

	CASE("transport: a channel value that differs is ignored and warns once");
	snprintf(base.transport, sizeof(base.transport), "udp");
	snprintf(ch.transport, sizeof(ch.transport), "tls");
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.transport == ACCT_TP_UDP);
	CHECK_UINT(warn_capture_count("channel transport 'tls' ignored"), 1);
	CHECK_UINT(warn_capture_count("names 'udp'"), 1);

	CASE("transport: the warning names the base value that won");
	snprintf(base.transport, sizeof(base.transport), "tcp");
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.transport == ACCT_TP_TCP);
	CHECK_UINT(warn_capture_count("names 'tcp'"), 1);

	CASE("transport: a channel value equal to the base is silent");
	snprintf(ch.transport, sizeof(ch.transport), "tcp");
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.transport == ACCT_TP_TCP);
	CHECK_UINT(warn_capture_count("ignored"), 0);

	/* "" and "udp" are one value, so a channel that spells the base value
	 * out is not a difference */
	CASE("transport: a channel udp against an empty base is not a difference");
	base.transport[0] = 0;
	snprintf(ch.transport, sizeof(ch.transport), "udp");
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.transport == ACCT_TP_UDP);
	CHECK_UINT(warn_capture_count("ignored"), 0);

	CASE("transport: an unknown channel token is ignored like any other");
	snprintf(base.transport, sizeof(base.transport), "udp");
	snprintf(ch.transport, sizeof(ch.transport), "sctp");
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.transport == ACCT_TP_UDP);
	CHECK_UINT(warn_capture_count("channel transport 'sctp' ignored"), 1);
	CHECK_UINT(warn_capture_count("is not one of udp"), 0);
	warn_capture_end();
	ch.transport[0] = 0;

	/*
	 * No transport gate. A tcp or tls line with a proxy and a user
	 * registers: the two reliable transports run on the stream pool of
	 * sip/stream.h, so nothing gates enabled on udp.
	 */
	CASE("transport: a tcp line with a proxy and a user is registrable");
	snprintf(base.transport, sizeof(base.transport), "tcp");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.transport == ACCT_TP_TCP);
	CHECK(c.enabled);

	CASE("transport: a tls line with a proxy and a user is registrable");
	snprintf(base.transport, sizeof(base.transport), "tls");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.transport == ACCT_TP_TLS);
	CHECK(c.enabled);

	CASE("transport: a tls line with nothing to register against is not");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "5551000");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.transport == ACCT_TP_TLS);
	CHECK(!c.enabled);

	base_default();
}

/*
 * RFC 3263 4.2: the default remote port of a sips target is 5061. Only the
 * absence of every configured port reaches it. sip_port_set is the one place
 * where that absence is visible (cfg.h). The schema default 5060 always
 * loads, so the value alone cannot tell a configured 5060 from an unset
 * option.
 */
static void test_derive_tls_port(void)
{
	struct cfg_channel ch;
	struct acct_cfg c;

	base_default();
	snprintf(base.transport, sizeof(base.transport), "tls");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");

	CASE("tls port: nothing configured anywhere takes 5061");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.resolve_port == 5061);

	/* the request URI omits the default of its transport, so the common
	 * case stays "sip:<domain>" over tls too */
	CASE("tls port: the request URI omits the 5061 default");
	CHECK(!strcmp(c.registrar_uri, "sip:p.example.com"));

	CASE("tls port: tcp with nothing configured keeps 5060");
	snprintf(base.transport, sizeof(base.transport), "tcp");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.resolve_port == 5060);
	CHECK(!strcmp(c.registrar_uri, "sip:p.example.com"));

	CASE("tls port: udp with nothing configured keeps 5060");
	snprintf(base.transport, sizeof(base.transport), "udp");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.resolve_port == 5060);

	CASE("tls port: an explicit sip_port wins over the 5061 default");
	snprintf(base.transport, sizeof(base.transport), "tls");
	ch.sip_port = 5080;
	ch.sip_port_set = true;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.resolve_port == 5080);

	/* the presence flag is the whole rule: without it an explicit 5060 and
	 * an absent option are the same 5060 */
	CASE("tls port: sip_port_set makes an explicit 5060 observable");
	ch.sip_port = 5060;
	ch.sip_port_set = true;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.resolve_port == 5060);
	CHECK(!strcmp(c.registrar_uri, "sip:p.example.com"));

	CASE("tls port: an embedded proxy port wins over the 5061 default");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com:5080");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.resolve_port == 5080);

	CASE("tls port: an explicit outbound proxy port wins too");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	ch.sip_outbound_proxy_port = 5070;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.resolve_port == 5070);

	/*
	 * The rule is all or nothing, and these two cases are the whole of it.
	 * A line that names one port has an operator behind it who thinks in
	 * 5060, so the port it did not name stays 5060 too: moving the other
	 * half to 5061 would resolve or register somewhere nobody configured.
	 */
	CASE("tls port: an explicit registrar port leaves the resolve port at 5060");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	ch.sip_registrar_port = 5070;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.registrar_uri, "sip:p.example.com:5070"));
	CHECK(c.resolve_port == 5060);

	CASE("tls port: an explicit outbound port leaves the registrar at 5060");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	ch.sip_outbound_proxy_port = 5070;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.resolve_port == 5070);
	CHECK(!strcmp(c.registrar_uri, "sip:p.example.com"));

	CASE("tls port: an embedded registrar port reaches it as well");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	snprintf(ch.sip_registrar, sizeof(ch.sip_registrar), "reg.example.com:5090");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.registrar_uri, "sip:reg.example.com:5090"));

	CASE("tls port: and takes the resolve port down to 5060 with it");
	CHECK(c.resolve_port == 5060);

	CASE("tls port: an embedded domain port counts as a named port too");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	snprintf(ch.sip_domain, sizeof(ch.sip_domain), "d.example.com:5090");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.resolve_port == 5060);

	/*
	 * A field shape, not a new code path: tls_def reads
	 * only the presence flag, so this takes the same route as "tls port:
	 * nothing configured anywhere takes 5061" above. The udp case in
	 * test_derive() is the oracle for the 0-to-5060 mapping itself,
	 * because a udp line's remote_def is sip_port, not tls_def's 5061.
	 */
	CASE("tls port: a sip_port of 0 reads as absent, so the tls line takes 5061");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	ch.sip_port = 0;
	ch.sip_port_set = false;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.resolve_port == 5061);

	base_default();
}

/*
 * RFC 3263 4.2: a next hop that names a port is asked for its address, and one
 * that names none is asked for SRV. auto asks a port-named next hop
 * SIP_DNS_ADDR_SRV: the address first, and SRV only when the name has no
 * address. The predicate reads the three sources of resolve_port and nothing
 * else. A registrar port therefore never reaches it.
 * A domain port reaches it only where the fallback chain made the domain
 * string the outbound string. The option keeps the other two answers: always
 * asks SRV whether a port is named or not, and never is the support switch.
 */
static void test_derive_dns_kind(void)
{
	struct cfg_channel ch;
	struct acct_cfg c;

	CASE("dns kind: always asks SRV whether a port is named or not");
	CHECK(acct_dns_kind(CFG_DNS_SRV_ALWAYS, false) == SIP_DNS_SRV);
	CHECK(acct_dns_kind(CFG_DNS_SRV_ALWAYS, true) == SIP_DNS_SRV);

	CASE("dns kind: never asks the address whether a port is named or not");
	CHECK(acct_dns_kind(CFG_DNS_SRV_NEVER, false) == SIP_DNS_ADDR);
	CHECK(acct_dns_kind(CFG_DNS_SRV_NEVER, true) == SIP_DNS_ADDR);

	/* not SIP_DNS_A: the clause asks for "an A or AAAA record lookup", and
	 * SIP_DNS_ADDR_SRV asks AAAA beside A as SIP_DNS_ADDR does */
	CASE("dns kind: auto with a port asks the address first, then SRV");
	CHECK(acct_dns_kind(CFG_DNS_SRV_AUTO, false) == SIP_DNS_SRV);
	CHECK(acct_dns_kind(CFG_DNS_SRV_AUTO, true) == SIP_DNS_ADDR_SRV);

	/* the three words the status surface and its schema carry */
	CASE("dns kind: the three kinds this derive produces name themselves");
	CHECK(!strcmp(sip_dns_kind_name(SIP_DNS_SRV), "srv"));
	CHECK(!strcmp(sip_dns_kind_name(SIP_DNS_ADDR_SRV), "addr_srv"));
	CHECK(!strcmp(sip_dns_kind_name(SIP_DNS_ADDR), "addr"));

	base_default();
	base.dns_srv_mode = CFG_DNS_SRV_ALWAYS;

	CASE("dns kind: always keeps SRV on a line that names no port");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.dns_kind == SIP_DNS_SRV);

	CASE("dns kind: always keeps SRV on a line that names one");
	ch.sip_port_set = true;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.dns_kind == SIP_DNS_SRV);

	base.dns_srv_mode = CFG_DNS_SRV_AUTO;

	CASE("dns kind: auto with nothing named asks SRV");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.dns_kind == SIP_DNS_SRV);
	CHECK(c.resolve_port == 5060);

	/* the presence flag is the whole rule here too: the schema default
	 * 5060 loads into sip_port on every config, named or not */
	CASE("dns kind: the sip_port presence flag decides, not its value");
	ch.sip_port = 5060;
	ch.sip_port_set = true;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.dns_kind == SIP_DNS_ADDR_SRV);
	CHECK(c.resolve_port == 5060);

	CASE("dns kind: an embedded outbound port asks the address first");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	snprintf(ch.sip_outbound_proxy, sizeof(ch.sip_outbound_proxy),
		 "ob.example.com:5080");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.dns_kind == SIP_DNS_ADDR_SRV);
	CHECK(c.resolve_port == 5080);

	CASE("dns kind: an explicit outbound proxy port asks the address "
	     "first");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	ch.sip_outbound_proxy_port = 5070;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.dns_kind == SIP_DNS_ADDR_SRV);
	CHECK(c.resolve_port == 5070);

	/* the domain string reaches the address of record, not the next hop,
	 * so its port leaves the question alone while a proxy stands in front */
	CASE("dns kind: an embedded domain port behind a proxy keeps SRV");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	snprintf(ch.sip_domain, sizeof(ch.sip_domain), "d.example.com:5090");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.dns_kind == SIP_DNS_SRV);
	CHECK(c.resolve_port == 5060);

	CASE("dns kind: an explicit registrar port keeps SRV");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	ch.sip_registrar_port = 5070;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.dns_kind == SIP_DNS_SRV);
	CHECK(c.resolve_port == 5060);

	CASE("dns kind: an embedded registrar port keeps SRV");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	snprintf(ch.sip_registrar, sizeof(ch.sip_registrar), "reg.example.com:5090");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.dns_kind == SIP_DNS_SRV);
	CHECK(c.resolve_port == 5060);

	/* with no proxy and no outbound proxy the chain makes the domain
	 * string the string the line resolves, and then its port counts */
	CASE("dns kind: a domain port the chain promoted asks the address "
	     "first");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_domain, sizeof(ch.sip_domain), "d.example.com:5090");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.resolve_host, "d.example.com"));
	CHECK(c.dns_kind == SIP_DNS_ADDR_SRV);
	CHECK(c.resolve_port == 5090);

	base.dns_srv_mode = CFG_DNS_SRV_NEVER;

	CASE("dns kind: never asks the address with nothing named");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.dns_kind == SIP_DNS_ADDR);
	CHECK(c.resolve_port == 5060);

	CASE("dns kind: never asks the address with a port named");
	ch.sip_port_set = true;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.dns_kind == SIP_DNS_ADDR);
	CHECK(c.resolve_port == 5060);

	base.dns_srv_mode = CFG_DNS_SRV_AUTO;
	snprintf(base.transport, sizeof(base.transport), "tls");

	/* the 5061 default names no port of its own: a tls line with nothing
	 * configured still asks SRV and carries 5061 for the fallback */
	CASE("dns kind: a tls line with nothing configured asks SRV at 5061");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.dns_kind == SIP_DNS_SRV);
	CHECK(c.resolve_port == 5061);

	CASE("dns kind: a tls line with a sip_port asks the address at it "
	     "first");
	ch.sip_port = 5080;
	ch.sip_port_set = true;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.dns_kind == SIP_DNS_ADDR_SRV);
	CHECK(c.resolve_port == 5080);

	base_default();

	/* the resolver ignores the kind for a literal (shared/resolve.h
	 * "Numeric targets"), and the derive makes no exception for one
	 * either */
	CASE("dns kind: a literal next hop derives its kind like any host");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "2001:db8::1");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.dns_kind == SIP_DNS_SRV);
	ch.sip_port_set = true;
	acct_cfg_derive(&base, &ch, &c);
	CHECK(c.dns_kind == SIP_DNS_ADDR_SRV);

	CASE("dns kind: the derive says once why a named port asks the address "
	     "first");
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "u");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "p.example.com");
	ch.sip_port_set = true;
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(warn_capture_count("RFC 3263 4.2"), 1);

	CASE("dns kind: and says nothing for a line that keeps SRV");
	ch.sip_port_set = false;
	warn_capture_begin();
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(warn_capture_count("RFC 3263 4.2"), 0);
	warn_capture_end();

	base_default();
}

/*
 * The token the four Via and Contact writers take (sip/reg.h, sip/keepalive.h,
 * sip/sub.h, account.h acct_call_view). NULL is UDP and is what a zeroed cfg
 * carries, which is what keeps the udp wire byte identical.
 */
static void test_transport_token(void)
{
	CASE("token: udp names no transport at all");
	CHECK(acct_transport_token(ACCT_TP_UDP) == NULL);

	CASE("token: tcp is the uppercase form the Via carries");
	CHECK(acct_transport_token(ACCT_TP_TCP) != NULL);
	CHECK(!strcmp(acct_transport_token(ACCT_TP_TCP), "TCP"));

	CASE("token: tls is the uppercase form the Via carries");
	CHECK(acct_transport_token(ACCT_TP_TLS) != NULL);
	CHECK(!strcmp(acct_transport_token(ACCT_TP_TLS), "TLS"));

	/* the writers keep the pointer, so it has to outlive the account that
	 * handed it out: a static literal does, a buffer would not */
	CASE("token: the token is one static literal per transport");
	CHECK(acct_transport_token(ACCT_TP_TCP) == acct_transport_token(ACCT_TP_TCP));
	CHECK(acct_transport_token(ACCT_TP_TLS) != acct_transport_token(ACCT_TP_TCP));
}

/* ------------------------------------------------------------ reload action */

static void test_reload_action(void)
{
	CASE("reload: an inactive line that is not wanted does nothing");
	CHECK(acct_reload_action(false, false, ACCT_APPLY_NONE) == ACCT_ROP_NONE);

	CASE("reload: a newly wanted line is built");
	CHECK(acct_reload_action(false, true, ACCT_APPLY_NONE) == ACCT_ROP_BUILD);

	CASE("reload: a line no longer wanted is gracefully removed");
	CHECK(acct_reload_action(true, false, ACCT_APPLY_RESTART) == ACCT_ROP_REMOVE);

	CASE("reload: no change is a no-op");
	CHECK(acct_reload_action(true, true, ACCT_APPLY_NONE) == ACCT_ROP_NONE);

	CASE("reload: a timer-only change updates in place");
	CHECK(acct_reload_action(true, true, ACCT_APPLY_LIVE) == ACCT_ROP_TIMERS);

	CASE("reload: an identity change gracefully replaces the registration");
	CHECK(acct_reload_action(true, true, ACCT_APPLY_REREG) == ACCT_ROP_REPLACE);

	CASE("reload: a transport change gracefully replaces the registration");
	CHECK(acct_reload_action(true, true, ACCT_APPLY_RESTART) == ACCT_ROP_REPLACE);
}

/*
 * The base restart set. The schema RESTART_ALL class has no reader, so this
 * comparison is what gives the class effect. A base option outside the set
 * never reaches a running line, whatever the schema row says.
 */
static void test_base_restart(void)
{
	static struct cfg old_cfg;
	static struct cfg new_cfg;

	base_default();
	old_cfg = base;

	CASE("base restart: two identical configs rebuild nothing");
	new_cfg = old_cfg;
	CHECK(!acct_base_restart(&old_cfg, &new_cfg));

	CASE("base restart: enabled moved rebuilds every line");
	new_cfg = old_cfg;
	new_cfg.enabled = !old_cfg.enabled;
	CHECK(acct_base_restart(&old_cfg, &new_cfg));

	CASE("base restart: channels moved rebuilds every line");
	new_cfg = old_cfg;
	new_cfg.channels = old_cfg.channels + 1;
	CHECK(acct_base_restart(&old_cfg, &new_cfg));

	CASE("base restart: local_interface moved rebuilds every line");
	new_cfg = old_cfg;
	snprintf(new_cfg.local_interface, sizeof(new_cfg.local_interface), "wan6");
	CHECK(acct_base_restart(&old_cfg, &new_cfg));

	CASE("base restart: transport moved rebuilds every line");
	new_cfg = old_cfg;
	snprintf(new_cfg.transport, sizeof(new_cfg.transport), "tls");
	CHECK(acct_base_restart(&old_cfg, &new_cfg));

	CASE("base restart: srtp_enabled moved rebuilds every line");
	new_cfg = old_cfg;
	new_cfg.srtp_enabled = !old_cfg.srtp_enabled;
	CHECK(acct_base_restart(&old_cfg, &new_cfg));

	/*
	 * The gate decides the family of every socket, Via, Contact and SDP
	 * c= line of a line, and a running line carries all four, so nothing
	 * short of a rebuild moves it (APPLY_RESTART_ALL).
	 */
	CASE("base restart: ipv6_enabled moved rebuilds every line");
	new_cfg = old_cfg;
	new_cfg.ipv6_enabled = !old_cfg.ipv6_enabled;
	CHECK(acct_base_restart(&old_cfg, &new_cfg));

	/*
	 * The negative half, which is what makes the set a set. A base option
	 * outside it reaches the running lines by another route, and a term
	 * added here would rebuild every line for a change that needs none.
	 */
	CASE("base restart: keepalive_interval moved rebuilds nothing");
	new_cfg = old_cfg;
	new_cfg.keepalive_interval = old_cfg.keepalive_interval + 30;
	CHECK(!acct_base_restart(&old_cfg, &new_cfg));

	CASE("base restart: restrict_source moved rebuilds nothing");
	new_cfg = old_cfg;
	new_cfg.restrict_source = old_cfg.restrict_source + 1;
	CHECK(!acct_base_restart(&old_cfg, &new_cfg));

	/*
	 * The five TLS options are RESTART_ALL in the schema and they are in
	 * the set: three of them name the certificate bundle the fetch brings
	 * in, and the two others shape the contexts every TLS line runs on, so
	 * a line that kept running would be using the bundle before the move.
	 */
	CASE("base restart: tls_cert_path moved rebuilds every line");
	new_cfg = old_cfg;
	snprintf(new_cfg.tls_cert_path, sizeof(new_cfg.tls_cert_path),
		 "https://certs.example.test/bundle.tgz");
	CHECK(acct_base_restart(&old_cfg, &new_cfg));

	CASE("base restart: tls_cert_username moved rebuilds every line");
	new_cfg = old_cfg;
	snprintf(new_cfg.tls_cert_username, sizeof(new_cfg.tls_cert_username),
		 "certuser");
	CHECK(acct_base_restart(&old_cfg, &new_cfg));

	CASE("base restart: tls_cert_password moved rebuilds every line");
	new_cfg = old_cfg;
	snprintf(new_cfg.tls_cert_password, sizeof(new_cfg.tls_cert_password),
		 "certpass");
	CHECK(acct_base_restart(&old_cfg, &new_cfg));

	CASE("base restart: tls_server_verification_enabled moved rebuilds every line");
	new_cfg = old_cfg;
	new_cfg.tls_server_verification_enabled =
		!old_cfg.tls_server_verification_enabled;
	CHECK(acct_base_restart(&old_cfg, &new_cfg));

	CASE("base restart: tls_keylog_enabled moved rebuilds every line");
	new_cfg = old_cfg;
	new_cfg.tls_keylog_enabled = !old_cfg.tls_keylog_enabled;
	CHECK(acct_base_restart(&old_cfg, &new_cfg));

	base_default();
}

/* ---------------------------------------------------------- the bind device */

/* NULL tolerant: an unscoped answer must compare, not crash the process. */
static bool bind_is(const char *got, const char *want)
{
	if (!got || !want)
		return got == want;

	return !strcmp(got, want);
}

/* ------------------------------------------------------------ status dump */

static bool dump_has_key(struct blob_buf *b, const char *name)
{
	struct blob_attr *cur;
	size_t rem;		/* blob_for_each_attr compares it against sizes */

	blob_for_each_attr(cur, b->head, rem) {
		if (!strcmp(blobmsg_name(cur), name))
			return true;
	}

	return false;
}

static void test_status_dump(void)
{
	struct blob_buf b;

	CASE("dump: the lines array is emitted with no account at all");
	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	accounts_status_dump(&b, -1);
	CHECK(dump_has_key(&b, "lines"));

	/* An empty table would tell the status page nothing, so there is none. */
	CASE("dump: nothing tracked yet emits no network table");
	CHECK(!dump_has_key(&b, "network"));
	blob_buf_free(&b);
}

/* --------------------------------------------------------- status and dns */

static void test_status(void)
{
	CASE("status: unregistered maps to unregistered");
	CHECK(!strcmp(acct_status_name(SIP_REG_UNREGISTERED), "unregistered"));

	CASE("status: resolving maps to trying");
	CHECK(!strcmp(acct_status_name(SIP_REG_RESOLVING), "trying"));

	CASE("status: trying maps to trying");
	CHECK(!strcmp(acct_status_name(SIP_REG_TRYING), "trying"));

	CASE("status: registered maps to registered");
	CHECK(!strcmp(acct_status_name(SIP_REG_REGISTERED), "registered"));

	CASE("status: refreshing maps to registered");
	CHECK(!strcmp(acct_status_name(SIP_REG_REFRESHING), "registered"));

	CASE("status: backoff maps to failed");
	CHECK(!strcmp(acct_status_name(SIP_REG_BACKOFF), "failed"));

	/* the IDLE SEIZE cell: the line dials only through a live binding */
	CASE("reg live: registered and refreshing hold a live binding");
	CHECK(acct_reg_live(SIP_REG_REGISTERED));
	CHECK(acct_reg_live(SIP_REG_REFRESHING));

	CASE("reg live: unregistered, resolving, trying and backoff hold none");
	CHECK(!acct_reg_live(SIP_REG_UNREGISTERED));
	CHECK(!acct_reg_live(SIP_REG_RESOLVING));
	CHECK(!acct_reg_live(SIP_REG_TRYING));
	CHECK(!acct_reg_live(SIP_REG_BACKOFF));

	/* the tone a tester hears follows the status word that he reads */
	CASE("reg live: the answer agrees with the status word in every state");
	for (int st = SIP_REG_UNREGISTERED; st <= SIP_REG_BACKOFF; st++)
		CHECK(acct_reg_live(st) == !strcmp(acct_status_name(st), "registered"));
}

static void test_reregister(void)
{
	CASE("reregister: mode 1 and a changed answer trigger a re-register");
	CHECK(acct_reregister_needed(true, true));

	CASE("reregister: mode 1 and an unchanged answer do not");
	CHECK(!acct_reregister_needed(true, false));

	CASE("reregister: mode 0 never re-registers on a changed answer");
	CHECK(!acct_reregister_needed(false, true));

	CASE("reregister: mode 0 and an unchanged answer do not");
	CHECK(!acct_reregister_needed(false, false));
}

/* ---------------------------------------------------- the DNS target reload */

/*
 * account_dns_result() applies every answer to a live registration, and the
 * per account TTL timer re-resolves the same name every 30 to 3600 s. The
 * resolver reshuffles equal priority SRV records on every delivery
 * (shared/resolve.h L17), so the same two servers come back in the other order,
 * and the sticky selection has to stay on the server it was on.
 *
 * The registration uses the same scripted environment as the reg suite: a
 * fixed virtual clock and a capture instead of a socket. The failover case
 * drives sip_reg_txn_done() and expires its retry timer. Only
 * acct_targets_load() and the three sip_reg calls account_dns_result() makes
 * are scripted here; the resolver, uloop and ubus sides belong to the live
 * harness.
 */

static struct {
	struct sip_txn_env env;
	uint64_t now;
	char host[64];
	unsigned int sends;
} rk;

static struct sip_txn_mgr rk_mgr;
static struct sip_reg rk_reg;

/* the buffer cfg.local_host points at: a move rewrites it first and the
 * Contact follows it (sip/reg.h sip_reg_local_host_set) */
static char rk_host[SIP_REG_HOST_MAX];

static int rk_send(struct sip_txn_env *env, const void *buf, size_t len,
		   const struct sip_txn_dst *dst)
{
	rk.sends++;
	rk.host[0] = 0;
	sip_dst_host(dst, rk.host, sizeof(rk.host));

	return (int)len;
}

static uint64_t rk_now(struct proto_timer_env *env)
{
	return rk.now;
}

static void rk_timer_set(struct proto_timer_env *env, struct sip_timer *t,
			 unsigned int ms)
{
	t->due_ms = rk.now + ms;
}

static void rk_timer_cancel(struct proto_timer_env *env, struct sip_timer *t)
{
}

static void rk_reset(void)
{
	struct sip_reg_cfg rc;
	struct sip_timer_cfg tc;

	sip_reg_done(&rk_reg);
	sip_txn_mgr_done(&rk_mgr);

	memset(&rk, 0, sizeof(rk));
	rk.now = 100000;
	rk.env.send = rk_send;
	rk.env.base.now_ms = rk_now;
	rk.env.base.timer_set = rk_timer_set;
	rk.env.base.timer_cancel = rk_timer_cancel;

	memset(&rc, 0, sizeof(rc));
	rc.aor = "sip:5551000@sip.example.com";
	rc.registrar = "sip:sip.example.com";
	rc.contact_user = "5551000";
	snprintf(rk_host, sizeof(rk_host), "10.0.0.5");
	rc.local_host = rk_host;
	rc.local_port = 5060;

	sip_timer_cfg_default(&tc);
	sip_txn_mgr_init(&rk_mgr, &rk.env, &tc);
	sip_reg_init(&rk_reg, &rk_mgr, &rk.env, 0x00010001, &rc, NULL, NULL);
}

/* One SRV answer, in the shape sip_dns_resolve() hands to the account. */
static void answer_make(struct sip_dns_result *res, const char *a, const char *b)
{
	memset(res, 0, sizeof(*res));
	res->status = SIP_DNS_OK;
	res->ttl = 300;

	snprintf(res->target[0].host, sizeof(res->target[0].host), "%s", a);
	res->target[0].port = 5060;
	res->count = 1;

	if (!b)
		return;

	snprintf(res->target[1].host, sizeof(res->target[1].host), "%s", b);
	res->target[1].port = 5060;
	res->count = 2;
}

/* The same answer with an SRV priority per target. */
static void answer_make_prio(struct sip_dns_result *res,
			     const char *a, uint16_t pa,
			     const char *b, uint16_t pb)
{
	answer_make(res, a, b);
	res->target[0].prio = pa;
	if (b)
		res->target[1].prio = pb;
}

static bool current_host_is(const char *host)
{
	const struct sip_reg_target *t;
	char buf[64];

	t = sip_reg_target_get(&rk_reg, sip_reg_target_current(&rk_reg));
	if (!t || !sip_dst_host(&t->dst, buf, sizeof(buf)))
		return false;

	return !strcmp(buf, host);
}

#define DNS_A	"198.51.100.10"
#define DNS_B	"198.51.100.11"
#define DNS_C	"198.51.100.12"

static void test_dns_reload(void)
{
	struct sip_dns_result res;

	/* account_dns_result(): the client is started first and the first
	 * answer fills the empty list it was left with. */
	CASE("dns reload: the first answer starts the attempt on the first server");
	rk_reset();
	sip_reg_start(&rk_reg);
	CHECK_UINT(sip_reg_state(&rk_reg), SIP_REG_RESOLVING);
	answer_make(&res, DNS_A, DNS_B);
	acct_targets_load(&rk_reg, &res);
	CHECK_UINT(sip_reg_target_count(&rk_reg), 2);
	CHECK_UINT(rk.sends, 1);
	CHECK(!strcmp(rk.host, DNS_A));
	CHECK(current_host_is(DNS_A));

	CASE("dns reload: the TTL answer in swapped order keeps the same server");
	answer_make(&res, DNS_B, DNS_A);
	acct_targets_load(&rk_reg, &res);
	CHECK_UINT(sip_reg_target_count(&rk_reg), 2);
	CHECK_UINT(sip_reg_target_current(&rk_reg), 1);
	CHECK(current_host_is(DNS_A));

	CASE("dns reload: and it survives cycle after cycle");
	answer_make(&res, DNS_A, DNS_B);
	acct_targets_load(&rk_reg, &res);
	CHECK(current_host_is(DNS_A));
	answer_make(&res, DNS_B, DNS_A);
	acct_targets_load(&rk_reg, &res);
	CHECK(current_host_is(DNS_A));
	CHECK_UINT(rk.sends, 1);

	/*
	 * The zone operator's routing policy, not a redraw: the line follows
	 * the lowest priority group that holds a server with an empty penalty
	 * box. The endpoint set is the same both times, so the moved selection
	 * is the whole of the change the caller reads.
	 */
	CASE("dns reload: a priority swap moves the line to the new best "
	     "server and reports a change");
	rk_reset();
	sip_reg_start(&rk_reg);
	answer_make_prio(&res, DNS_A, 10, DNS_B, 20);
	CHECK(acct_targets_load(&rk_reg, &res));
	CHECK(current_host_is(DNS_A));
	answer_make_prio(&res, DNS_A, 20, DNS_B, 10);
	CHECK(acct_targets_load(&rk_reg, &res));
	CHECK(current_host_is(DNS_B));

	CASE("dns reload: a recovered primary is re-taken at the next answer");
	rk_reset();
	sip_reg_start(&rk_reg);
	answer_make_prio(&res, DNS_A, 10, DNS_B, 20);
	CHECK(acct_targets_load(&rk_reg, &res));
	CHECK(current_host_is(DNS_A));
	sip_reg_txn_done(&rk_reg, rk_reg.txn, SIP_TXN_DONE_TIMEOUT);
	CHECK(current_host_is(DNS_B));
	/* the primary is still in its penalty box, so the answer keeps the
	 * line on the secondary and reports nothing */
	answer_make_prio(&res, DNS_A, 10, DNS_B, 20);
	CHECK(!acct_targets_load(&rk_reg, &res));
	CHECK(current_host_is(DNS_B));
	/* the box ran out: the next answer takes the primary back */
	rk.now += 61 * 1000;
	answer_make_prio(&res, DNS_A, 10, DNS_B, 20);
	CHECK(acct_targets_load(&rk_reg, &res));
	CHECK(current_host_is(DNS_A));

	CASE("dns reload: a penalized sticky hands the line to an unpenalized "
	     "peer");
	rk_reset();
	sip_reg_start(&rk_reg);
	answer_make_prio(&res, DNS_A, 10, NULL, 0);
	CHECK(acct_targets_load(&rk_reg, &res));
	CHECK(current_host_is(DNS_A));
	/* one target, so the failure penalizes it and advances nowhere */
	sip_reg_txn_done(&rk_reg, rk_reg.txn, SIP_TXN_DONE_TIMEOUT);
	CHECK(current_host_is(DNS_A));
	answer_make_prio(&res, DNS_A, 10, DNS_B, 10);
	CHECK(acct_targets_load(&rk_reg, &res));
	CHECK(current_host_is(DNS_B));

	/*
	 * account_dns_result() mode 1: a moved answer retargets in place,
	 * right after the reload. A stop and start pair would send a second
	 * REGISTER even when the selection stays put, and report the line down
	 * in between.
	 */
	CASE("dns reload: a set change that keeps the sticky server sends nothing");
	rk_reset();
	sip_reg_start(&rk_reg);
	answer_make(&res, DNS_A, DNS_B);
	CHECK(acct_targets_load(&rk_reg, &res));
	CHECK_UINT(rk.sends, 1);
	CHECK(!strcmp(rk.host, DNS_A));
	/* B leaves under the REGISTER in flight; the selection stays on A,
	 * so the retarget neither sends nor defers */
	answer_make(&res, DNS_A, DNS_C);
	CHECK(acct_targets_load(&rk_reg, &res));
	sip_reg_retarget(&rk_reg);
	CHECK_UINT(rk.sends, 1);
	CHECK(!rk_reg.retarget_pending);
	CHECK(current_host_is(DNS_A));

	CASE("dns reload: a moved sticky retargets with one send");
	rk_reset();
	sip_reg_start(&rk_reg);
	answer_make(&res, DNS_A, NULL);
	CHECK(acct_targets_load(&rk_reg, &res));
	CHECK_UINT(rk.sends, 1);
	CHECK(!strcmp(rk.host, DNS_A));
	sip_reg_txn_done(&rk_reg, rk_reg.txn, SIP_TXN_DONE_TIMEOUT);
	CHECK_UINT(rk.sends, 1);
	answer_make(&res, DNS_B, NULL);
	CHECK(acct_targets_load(&rk_reg, &res));
	sip_reg_retarget(&rk_reg);
	CHECK_UINT(rk.sends, 2);
	CHECK(!strcmp(rk.host, DNS_B));
	CHECK(current_host_is(DNS_B));

	CASE("dns reload: a disjoint single-A rotation retargets on one Call-ID");
	rk_reset();
	sip_reg_start(&rk_reg);
	answer_make(&res, DNS_A, NULL);
	CHECK(acct_targets_load(&rk_reg, &res));
	CHECK_UINT(rk.sends, 1);
	{
		char call_id[SIP_REG_ID_MAX];

		snprintf(call_id, sizeof(call_id), "%s", sip_reg_call_id(&rk_reg));
		sip_reg_txn_done(&rk_reg, rk_reg.txn, SIP_TXN_DONE_TIMEOUT);
		answer_make(&res, DNS_B, NULL);
		CHECK(acct_targets_load(&rk_reg, &res));
		sip_reg_retarget(&rk_reg);
		CHECK_UINT(rk.sends, 2);
		CHECK(!strcmp(rk.host, DNS_B));
		sip_reg_txn_done(&rk_reg, rk_reg.txn, SIP_TXN_DONE_TIMEOUT);
		answer_make(&res, DNS_C, NULL);
		CHECK(acct_targets_load(&rk_reg, &res));
		sip_reg_retarget(&rk_reg);
		CHECK_UINT(rk.sends, 3);
		CHECK(!strcmp(rk.host, DNS_C));
		CHECK(!strcmp(sip_reg_call_id(&rk_reg), call_id));
		CHECK_UINT(sip_reg_cseq(&rk_reg), 3);
	}

	CASE("dns reload: the changed answer is the line's own list, not the cache flag");
	rk_reset();
	sip_reg_start(&rk_reg);
	answer_make(&res, DNS_A, DNS_B);
	res.changed = false;			/* a shared cache hit reads false */
	CHECK(acct_targets_load(&rk_reg, &res));
	answer_make(&res, DNS_B, DNS_A);
	res.changed = true;
	CHECK(!acct_targets_load(&rk_reg, &res));	/* a redraw is no move */

	CASE("exhausted: an empty list is exhausted");
	rk_reset();
	CHECK(acct_targets_exhausted(&rk_reg, rk.now));

	CASE("exhausted: a single target is, answering or not");
	sip_reg_target_add(&rk_reg, DNS_A, 5060);
	CHECK(acct_targets_exhausted(&rk_reg, rk.now));

	CASE("exhausted: two targets with one penalty box free are not");
	sip_reg_target_add(&rk_reg, DNS_B, 5060);
	rk_reg.target[0].penalty_until = rk.now + 60000;
	CHECK(!acct_targets_exhausted(&rk_reg, rk.now));

	CASE("exhausted: every target penalized is");
	rk_reg.target[1].penalty_until = rk.now + 60000;
	CHECK(acct_targets_exhausted(&rk_reg, rk.now));

	CASE("exhausted: a penalty that ran out frees the list again");
	CHECK(!acct_targets_exhausted(&rk_reg, rk.now + 61000));

	CASE("dns reload: mode 0 leaves a changed answer to the sticky selection");
	rk_reset();
	sip_reg_start(&rk_reg);
	answer_make(&res, DNS_A, DNS_B);
	acct_targets_load(&rk_reg, &res);
	answer_make(&res, DNS_B, DNS_A);
	res.changed = true;
	CHECK(!acct_reregister_needed(false, res.changed));
	acct_targets_load(&rk_reg, &res);
	CHECK_UINT(rk.sends, 1);
	CHECK(current_host_is(DNS_A));

	CASE("dns reload: a server that left the answer hands the line to another");
	rk_reset();
	sip_reg_start(&rk_reg);
	answer_make(&res, DNS_A, DNS_B);
	acct_targets_load(&rk_reg, &res);
	CHECK(current_host_is(DNS_A));
	answer_make(&res, DNS_B, DNS_C);
	acct_targets_load(&rk_reg, &res);
	CHECK_UINT(sip_reg_target_count(&rk_reg), 2);
	CHECK(current_host_is(DNS_B));

	/*
	 * Two SRV rows of a zone can name one host and port, and the resolver
	 * collects the same address under each of them. The account walks the
	 * answer without a duplicate test of its own (sip/reg.h).
	 */
	CASE("dns reload: two SRV rows on one address are one target");
	rk_reset();
	sip_reg_start(&rk_reg);
	answer_make(&res, DNS_A, DNS_A);
	acct_targets_load(&rk_reg, &res);
	CHECK_UINT(sip_reg_target_count(&rk_reg), 1);
	CHECK_UINT(rk.sends, 1);
	CHECK(current_host_is(DNS_A));
	answer_make(&res, DNS_B, DNS_A);
	snprintf(res.target[2].host, sizeof(res.target[2].host), "%s", DNS_A);
	res.target[2].port = 5060;
	res.count = 3;
	acct_targets_load(&rk_reg, &res);
	CHECK_UINT(sip_reg_target_count(&rk_reg), 2);
	CHECK_UINT(sip_reg_target_current(&rk_reg), 1);
	CHECK(current_host_is(DNS_A));

	CASE("dns reload: an answer that grew keeps the server the line is on");
	rk_reset();
	sip_reg_start(&rk_reg);
	answer_make(&res, DNS_A, NULL);
	acct_targets_load(&rk_reg, &res);
	CHECK(current_host_is(DNS_A));
	answer_make(&res, DNS_C, DNS_B);
	snprintf(res.target[2].host, sizeof(res.target[2].host), "%s", DNS_A);
	res.target[2].port = 5060;
	res.count = 3;
	acct_targets_load(&rk_reg, &res);
	CHECK_UINT(sip_reg_target_count(&rk_reg), 3);
	CHECK_UINT(sip_reg_target_current(&rk_reg), 2);
	CHECK(current_host_is(DNS_A));

	sip_reg_done(&rk_reg);
	sip_txn_mgr_done(&rk_mgr);
}

/*
 * The dns_linger retention, pure: virtual clocks in the arguments, no uloop
 * and no line. The live halves, the round membership and the rule set, run in
 * the round and dns move classes.
 */
static void test_linger(void)
{
	struct acct_linger lg;
	struct sip_txn_dst dst, dst2;
	char list[FW_PROXY_MAX][FW_SRC_MAX];
	unsigned int n;
	unsigned int i;

	CASE("linger: a noted address refreshes its last seen on re-appearance");
	memset(&lg, 0, sizeof(lg));
	CHECK(sip_dst_set(&dst, DNS_A, 5060));
	acct_linger_note(&lg, &dst, 1000);
	CHECK_UINT(lg.count, 1);
	acct_linger_note(&lg, &dst, 5000);
	CHECK_UINT(lg.count, 1);
	CHECK(lg.entry[0].last_seen_ms == 5000);

	CASE("linger: the window expires an address that stayed away");
	CHECK(sip_dst_set(&dst2, DNS_B, 5060));
	acct_linger_note(&lg, &dst2, 6000);
	acct_linger_expire(&lg, 5000 + 10 * 1000, 10);
	CHECK_UINT(lg.count, 1);
	CHECK(acct_linger_covers(&lg, &dst2));
	CHECK(!acct_linger_covers(&lg, &dst));

	CASE("linger: identity is the address and the port");
	CHECK(sip_dst_set(&dst, DNS_B, 5080));
	acct_linger_note(&lg, &dst, 7000);
	CHECK_UINT(lg.count, 2);

	CASE("linger: a window of zero keeps nothing");
	acct_linger_expire(&lg, 8000, 0);
	CHECK_UINT(lg.count, 0);
	acct_linger_note(&lg, &dst, 8000);
	acct_linger_expire(&lg, 8000, 0);
	CHECK_UINT(lg.count, 0);

	CASE("linger: a full list evicts the oldest address");
	memset(&lg, 0, sizeof(lg));
	for (i = 0; i < ACCT_LINGER_MAX; i++) {
		char host[32];

		snprintf(host, sizeof(host), "198.51.100.%u", 10 + i);
		CHECK(sip_dst_set(&dst, host, 5060));
		acct_linger_note(&lg, &dst, 1000 + i);
	}
	CHECK_UINT(lg.count, ACCT_LINGER_MAX);
	CHECK(sip_dst_set(&dst, "203.0.113.1", 5060));
	acct_linger_note(&lg, &dst, 99000);
	CHECK_UINT(lg.count, ACCT_LINGER_MAX);
	CHECK(sip_dst_set(&dst2, "198.51.100.10", 5060));
	CHECK(!acct_linger_covers(&lg, &dst2));
	CHECK(acct_linger_covers(&lg, &dst));

	CASE("linger: the deadline is the oldest entry's expiry");
	CHECK(acct_linger_deadline(&lg, 10) == 1001 + 10 * 1000);
	CHECK(acct_linger_deadline(&lg, 0) == 0);

	CASE("linger: collect lands in the sorted proxy order and inside the bound");
	memset(&lg, 0, sizeof(lg));
	CHECK(sip_dst_set(&dst, "198.51.100.30", 5060));
	acct_linger_note(&lg, &dst, 1000);
	CHECK(sip_dst_set(&dst, "198.51.100.20", 5060));
	acct_linger_note(&lg, &dst, 1000);
	memset(list, 0, sizeof(list));
	n = fw_reconcile_proxy_add(list, 0, "198.51.100.25");
	n = acct_linger_collect(&lg, list, n);
	CHECK_UINT(n, 3);
	CHECK(!strcmp(list[0], "198.51.100.20"));
	CHECK(!strcmp(list[1], "198.51.100.25"));
	CHECK(!strcmp(list[2], "198.51.100.30"));

	CASE("linger: collect never evicts a current proxy");
	memset(list, 0, sizeof(list));
	for (n = 0, i = 0; i < FW_PROXY_MAX; i++) {
		char host[32];

		snprintf(host, sizeof(host), "203.0.113.%u", 100 + i);
		n = fw_reconcile_proxy_add(list, n, host);
	}
	CHECK_UINT(n, FW_PROXY_MAX);
	/* both lingering addresses sort before every current one and still
	 * get no slot */
	warn_capture_begin();
	n = acct_linger_collect(&lg, list, n);
	CHECK_UINT(warn_capture_count("lingering proxy source"), 2);
	warn_capture_end();
	CHECK_UINT(n, FW_PROXY_MAX);
	CHECK(!strcmp(list[0], "203.0.113.100"));
}

/*
 * What a family move keeps. The move rewrites the local host and rebuilds the
 * Contact from it. The line must keep three things that a re-init would
 * discard (sip/reg.h). They are the resolved targets, the penalty box of a
 * failed target, and the sticky selection that the failure moved. Without
 * them, a line would lose its failover state at every change of family.
 */
static void test_local_host_move(void)
{
	struct sip_dns_result res;
	uint32_t first;

	CASE("move: the targets, the penalties and the selection survive it");
	rk_reset();
	sip_reg_start(&rk_reg);
	answer_make(&res, DNS_A, DNS_B);
	acct_targets_load(&rk_reg, &res);
	CHECK(current_host_is(DNS_A));

	sip_reg_txn_done(&rk_reg, rk_reg.txn, SIP_TXN_DONE_TIMEOUT);
	CHECK(current_host_is(DNS_B));
	CHECK(rk_reg.target[0].penalty_until != 0);
	CHECK_UINT(rk_reg.target[0].failures, 1);

	snprintf(rk_host, sizeof(rk_host), "2001:db8::5");
	CHECK(sip_reg_local_host_set(&rk_reg));
	sip_reg_refresh_now(&rk_reg);

	CHECK_UINT(sip_reg_target_count(&rk_reg), 2);
	CHECK_UINT(sip_reg_target_current(&rk_reg), 1);
	CHECK(current_host_is(DNS_B));
	CHECK(rk_reg.target[0].penalty_until != 0);
	CHECK_UINT(rk_reg.target[0].failures, 1);

	CASE("move: and the Contact carries the new address, bracketed");
	CHECK(strstr(sip_reg_contact(&rk_reg), "@[2001:db8::5]:5060") != NULL);

	/*
	 * Why the move that follows a failed attempt sends nothing. The retry
	 * delay doubles per consecutive failure, and a refresh clears that run,
	 * so a move that refreshed would hold a line whose targets of both
	 * families all refuse at the first delay for ever.
	 */
	CASE("move: a move that sends nothing leaves the backoff climbing");
	rk_reset();
	sip_reg_start(&rk_reg);
	answer_make(&res, DNS_A, NULL);
	acct_targets_load(&rk_reg, &res);
	CHECK_UINT(rk.sends, 1);

	sip_reg_txn_done(&rk_reg, rk_reg.txn, SIP_TXN_DONE_TIMEOUT);
	CHECK_UINT(rk_reg.fail_run, 1);
	first = rk_reg.next_retry_s;
	CHECK(first != 0);

	snprintf(rk_host, sizeof(rk_host), "2001:db8::7");
	CHECK(sip_reg_local_host_set(&rk_reg));
	rk.now = rk_reg.timer.due_ms;
	sip_timer_expire(&rk_reg.timer);
	sip_reg_txn_done(&rk_reg, rk_reg.txn, SIP_TXN_DONE_TIMEOUT);
	CHECK_UINT(rk_reg.fail_run, 2);
	CHECK(rk_reg.next_retry_s > first);

	CASE("move: and a refresh would put that run back to nothing");
	sip_reg_refresh_now(&rk_reg);
	CHECK_UINT(rk_reg.fail_run, 0);

	sip_reg_done(&rk_reg);
	sip_txn_mgr_done(&rk_mgr);
}

/* ------------------------------------------------- the proxy source collector */

static struct sip_txn_mgr rk2_mgr;
static struct sip_reg rk2_reg;

/*
 * A second line on the same scripted environment. The reconcile walks every
 * active line with one list and one running count, so the collector needs two
 * registrations to be exercised the way production calls it.
 */
static void rk2_reset(void)
{
	struct sip_reg_cfg rc;
	struct sip_timer_cfg tc;

	sip_reg_done(&rk2_reg);
	sip_txn_mgr_done(&rk2_mgr);

	memset(&rc, 0, sizeof(rc));
	rc.aor = "sip:5551001@sip.example.com";
	rc.registrar = "sip:sip.example.com";
	rc.contact_user = "5551001";
	rc.local_host = "10.0.0.5";
	rc.local_port = 5062;

	sip_timer_cfg_default(&tc);
	sip_txn_mgr_init(&rk2_mgr, &rk.env, &tc);
	sip_reg_init(&rk2_reg, &rk2_mgr, &rk.env, 0x00020001, &rc, NULL, NULL);
}

static void test_proxy_collect(void)
{
	char proxy[FW_PROXY_MAX][FW_SRC_MAX];
	struct sip_dns_result res;
	struct sip_reg fresh;
	unsigned int n;

	memset(proxy, 0, sizeof(proxy));

	CASE("collect: one line fills the list in sorted order");
	rk_reset();
	sip_reg_start(&rk_reg);
	answer_make(&res, DNS_A, DNS_B);
	acct_targets_load(&rk_reg, &res);
	n = acct_proxy_collect(&rk_reg, true, proxy, 0);
	CHECK_UINT(n, 2);
	CHECK(!strcmp(proxy[0], DNS_A));
	CHECK(!strcmp(proxy[1], DNS_B));

	/*
	 * The second line collects on top of the first: that running count is
	 * how one bounded list covers both lines, a proxy they share holds one
	 * rule, and a proxy only the second line uses gets its own.
	 */
	CASE("collect: a second line appends to the running count and shares a slot");
	rk2_reset();
	sip_reg_start(&rk2_reg);
	answer_make(&res, DNS_B, DNS_C);
	acct_targets_load(&rk2_reg, &res);
	n = acct_proxy_collect(&rk2_reg, true, proxy, n);
	CHECK_UINT(n, 3);
	CHECK(!strcmp(proxy[0], DNS_A));
	CHECK(!strcmp(proxy[1], DNS_B));
	CHECK(!strcmp(proxy[2], DNS_C));

	CASE("collect: a client that never started contributes nothing");
	memset(&fresh, 0, sizeof(fresh));
	CHECK_UINT(acct_proxy_collect(&fresh, true, proxy, n), 3);
	CHECK_UINT(acct_proxy_collect(&fresh, false, proxy, n), 3);

	/*
	 * sip_reg_done() stops the client and leaves the target list, so the
	 * started gate is the only thing between a torn down line and a rule
	 * that still names its proxy.
	 */
	CASE("collect: a stopped client keeps its targets, and the gate holds them back");
	sip_reg_done(&rk_reg);
	memset(proxy, 0, sizeof(proxy));
	CHECK_UINT(acct_proxy_collect(&rk_reg, true, proxy, 0), 2);
	memset(proxy, 0, sizeof(proxy));
	CHECK_UINT(acct_proxy_collect(&rk_reg, false, proxy, 0), 0);
	CHECK(!proxy[0][0]);

	sip_reg_done(&rk2_reg);
	sip_txn_mgr_done(&rk2_mgr);
	sip_reg_done(&rk_reg);
	sip_txn_mgr_done(&rk_mgr);
}

/* --------------------------------------------- the subscription status key */

/*
 * The SUBSCRIBE machine of a line reaches "voice status" through
 * acct_sub_status_dump(), and presence is the message: a line that never
 * subscribes emits no key at all, the way the registrar keys stay absent until
 * a target resolves. The consumers read an absent key as "this line does not
 * subscribe", so a key that is always there says nothing.
 *
 * The dump computes no state of its own: it reads the machine through the
 * sip/sub.h accessors. The fixture is therefore one client on a scripted
 * environment of the reg shape, a fixed clock and a capture instead of a
 * socket, moved by the very calls the account layer makes on it:
 * sip_sub_start() from the registered callback, sip_sub_txn_done() from the
 * transaction upcall, sip_sub_stop() from the teardown.
 *
 * account.c defines the dump non-static for this suite, because a live account
 * only exists behind a socket registration; the prototype lives in account.h.
 */
static struct {
	struct sip_txn_env env;
	uint64_t now;
	unsigned int sends;
} sk;

static struct sip_txn_mgr sk_mgr;
static struct sip_sub sk_sub;
static struct sip_txn_dst sk_dst;

static int sk_send(struct sip_txn_env *env, const void *buf, size_t len,
		   const struct sip_txn_dst *dst)
{
	sk.sends++;

	return (int)len;
}

static uint64_t sk_now(struct proto_timer_env *env)
{
	return sk.now;
}

static void sk_timer_set(struct proto_timer_env *env, struct sip_timer *t,
			 unsigned int ms)
{
	t->due_ms = sk.now + ms;
}

static void sk_timer_cancel(struct proto_timer_env *env, struct sip_timer *t)
{
}

/* the sticky registration target the account hands the subscription */
static const struct sip_txn_dst *sk_dst_get(struct sip_sub *sub, void *priv)
{
	return &sk_dst;
}

/* the two upcalls acct_env_resp() and acct_env_done() route to this client */
static void sk_resp(struct sip_txn_env *env, uint32_t owner, uint32_t txn,
		    const struct sip_msg *msg)
{
	sip_sub_txn_resp(&sk_sub, txn, msg);
}

static void sk_done(struct sip_txn_env *env, uint32_t owner, uint32_t txn,
		    enum sip_txn_reason reason)
{
	sip_sub_txn_done(&sk_sub, txn, reason);
}

static void sk_reset(void)
{
	struct sip_sub_cfg sc;
	struct sip_timer_cfg tc;

	sip_sub_done(&sk_sub);
	sip_txn_mgr_done(&sk_mgr);

	memset(&sk, 0, sizeof(sk));
	sk.now = 100000;
	sk.env.send = sk_send;
	sk.env.base.now_ms = sk_now;
	sk.env.base.timer_set = sk_timer_set;
	sk.env.base.timer_cancel = sk_timer_cancel;
	sk.env.resp = sk_resp;
	sk.env.done = sk_done;

	memset(&sc, 0, sizeof(sc));
	sc.aor = "sip:5551000@sip.example.com";
	sc.registrar = "sip:sip.example.com";
	sc.contact = "sip:5551000@10.0.0.5:5060";
	sc.local_host = "10.0.0.5";
	sc.local_port = 5060;
	sc.expires = 3600;

	sip_dst_set(&sk_dst, DNS_A, 5060);
	sip_timer_cfg_default(&tc);
	sip_txn_mgr_init(&sk_mgr, &sk.env, &tc);
	sip_sub_init(&sk_sub, &sk_mgr, &sk.env, acct_owner_make(ACCT_OWNER_SUB, 1), &sc,
		     sk_dst_get, NULL);
}

/*
 * The notifier's answer to the SUBSCRIBE in flight, delivered the way the
 * socket delivers one: through the transaction layer, which matches it on the
 * Via branch and the CSeq method (sip/txn.c resp_recv) and hands it up to
 * sip_sub_txn_resp(). The identity comes from the client itself, because
 * sip/sub.h exposes the whole instance and the wire capture would only say the
 * same thing twice. The parse keeps spans into the buffer, so the buffer is
 * static and outlives the delivery.
 */
static void sk_reply(uint32_t code, const char *extra)
{
	static char text[1024];
	static struct sip_msg parsed;
	int len;

	len = snprintf(text, sizeof(text),
		       "SIP/2.0 %u %s\r\n"
		       "Via: SIP/2.0/UDP 10.0.0.5:5060;branch=%s\r\n"
		       "From: <sip:5551000@sip.example.com>;tag=%s\r\n"
		       "To: <sip:5551000@sip.example.com>;tag=notifier-tag\r\n"
		       "Call-ID: %s\r\n"
		       "CSeq: %u SUBSCRIBE\r\n"
		       "%s"
		       "Content-Length: 0\r\n\r\n",
		       code, sip_status_reason(code), sk_sub.branch, sk_sub.from_tag,
		       sip_sub_call_id(&sk_sub), sip_sub_cseq(&sk_sub), extra ? extra : "");

	if (len < 0 || (size_t)len >= sizeof(text))
		return;

	if (msg_parse(&parsed, text, (size_t)len) != SIP_PARSE_OK)
		return;

	sip_txn_msg_recv(&sk_mgr, &parsed, NULL);
}

static const char *dump_str(struct blob_buf *b, const char *name)
{
	struct blob_attr *cur;
	size_t rem;

	blob_for_each_attr(cur, b->head, rem) {
		if (strcmp(blobmsg_name(cur), name))
			continue;

		if (blobmsg_type(cur) != BLOBMSG_TYPE_STRING)
			return NULL;

		return blobmsg_get_string(cur);
	}

	return NULL;
}

/* NULL tolerant: a missing key must fail the case, not the process */
static bool dump_str_is(struct blob_buf *b, const char *name, const char *want)
{
	const char *got = dump_str(b, name);

	return got && !strcmp(got, want);
}

static unsigned int dump_count(struct blob_buf *b)
{
	struct blob_attr *cur;
	size_t rem;
	unsigned int n = 0;

	blob_for_each_attr(cur, b->head, rem)
		n++;

	return n;
}

/* the name at one position, so a key that moved fails as loudly as one that
 * left */
static bool dump_name_is(struct blob_buf *b, unsigned int idx, const char *want)
{
	struct blob_attr *cur;
	size_t rem;
	unsigned int n = 0;

	blob_for_each_attr(cur, b->head, rem) {
		if (n++ != idx)
			continue;

		return !strcmp(blobmsg_name(cur), want);
	}

	return false;
}

static void test_sub_status(void)
{
	struct blob_buf b;

	memset(&b, 0, sizeof(b));
	sk_reset();

	CASE("sub status: a line that does not subscribe emits no key");
	blob_buf_init(&b, 0);
	acct_sub_status_dump(&b, false, &sk_sub);
	CHECK(!dump_has_key(&b, "mwi_subscription"));

	/* the account builds the client beside the registration and starts it
	 * only for mwi_subscribe 1, so both halves of the gate are real */
	CASE("sub status: a line whose client is not built emits no key either");
	acct_sub_status_dump(&b, true, NULL);
	CHECK(!dump_has_key(&b, "mwi_subscription"));
	CHECK_UINT(dump_count(&b), 0);

	CASE("sub status: an armed line reads off before its first attempt");
	acct_sub_status_dump(&b, true, &sk_sub);
	CHECK(dump_str_is(&b, "mwi_subscription", "off"));

	CASE("sub status: the start a registration triggers reads trying");
	blob_buf_init(&b, 0);
	sip_sub_start(&sk_sub);
	CHECK_UINT(sk.sends, 1);
	acct_sub_status_dump(&b, true, &sk_sub);
	CHECK(dump_str_is(&b, "mwi_subscription", "trying"));

	CASE("sub status: an attempt that timed out reads retry");
	blob_buf_init(&b, 0);
	sip_sub_txn_done(&sk_sub, sk_sub.txn, SIP_TXN_DONE_TIMEOUT);
	acct_sub_status_dump(&b, true, &sk_sub);
	CHECK(dump_str_is(&b, "mwi_subscription", "retry"));

	/*
	 * Presence follows the configuration and the state follows the machine.
	 * A stopped client still carries the key and reads off; only a line that
	 * never subscribes says nothing at all.
	 */
	CASE("sub status: a stopped client keeps the key and reads off");
	blob_buf_init(&b, 0);
	sip_sub_stop(&sk_sub, false);
	acct_sub_status_dump(&b, true, &sk_sub);
	CHECK(dump_str_is(&b, "mwi_subscription", "off"));

	CASE("sub status: the key joins the line keys and moves none of them");
	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "line", 0);
	blobmsg_add_string(&b, "aor", "sip:5551000@sip.example.com");
	blobmsg_add_string(&b, "registrar", DNS_A);
	acct_sub_status_dump(&b, true, &sk_sub);
	CHECK_UINT(dump_count(&b), 4);
	CHECK(dump_name_is(&b, 0, "line"));
	CHECK(dump_name_is(&b, 1, "aor"));
	CHECK(dump_name_is(&b, 2, "registrar"));
	CHECK(dump_name_is(&b, 3, "mwi_subscription"));

	CASE("sub status: a line that does not subscribe leaves them untouched");
	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "line", 0);
	blobmsg_add_string(&b, "aor", "sip:5551000@sip.example.com");
	blobmsg_add_string(&b, "registrar", DNS_A);
	acct_sub_status_dump(&b, false, &sk_sub);
	CHECK_UINT(dump_count(&b), 3);
	CHECK(dump_name_is(&b, 0, "line"));
	CHECK(dump_name_is(&b, 1, "aor"));
	CHECK(dump_name_is(&b, 2, "registrar"));

	/*
	 * The whole vocabulary is pinned here, not just the states an attempt
	 * that never gets an answer walks through. The mapper branches on these
	 * words: "active" is a live subscription and "refused" is the terminal
	 * class that no retry timer ever leaves, so a rename in sip/sub.h has to
	 * fail a case here (the one vocabulary rule).
	 */
	CASE("sub status: the notifier's 200 reads active");
	sk_reset();
	sip_sub_start(&sk_sub);
	sk_reply(200, "Expires: 1800\r\n");
	CHECK(sip_sub_status_get(&sk_sub) == SIP_SUB_ACTIVE);
	blob_buf_init(&b, 0);
	acct_sub_status_dump(&b, true, &sk_sub);
	CHECK(dump_str_is(&b, "mwi_subscription", "active"));

	CASE("sub status: a notifier that refuses the package reads refused");
	sk_reset();
	sip_sub_start(&sk_sub);
	sk_reply(403, NULL);
	CHECK(sip_sub_status_get(&sk_sub) == SIP_SUB_REFUSED);
	blob_buf_init(&b, 0);
	acct_sub_status_dump(&b, true, &sk_sub);
	CHECK(dump_str_is(&b, "mwi_subscription", "refused"));

	blob_buf_free(&b);
	sip_sub_done(&sk_sub);
	sip_txn_mgr_done(&sk_mgr);
}

/* ------------------------------------------------- the transport status keys */

static struct blob_attr *dump_table(struct blob_buf *b, const char *name)
{
	struct blob_attr *cur;
	size_t rem;

	blob_for_each_attr(cur, b->head, rem) {
		if (strcmp(blobmsg_name(cur), name))
			continue;

		return blobmsg_type(cur) == BLOBMSG_TYPE_TABLE ? cur : NULL;
	}

	return NULL;
}

/* NULL tolerant on both counts: a missing table and a missing key fail the
 * case, they do not fail the process */
static bool table_str_is(struct blob_attr *tbl, const char *name, const char *want)
{
	struct blob_attr *cur;
	size_t rem;

	if (!tbl)
		return false;

	blobmsg_for_each_attr(cur, tbl, rem) {
		if (strcmp(blobmsg_name(cur), name))
			continue;
		if (blobmsg_type(cur) != BLOBMSG_TYPE_STRING)
			return false;

		return !strcmp(blobmsg_get_string(cur), want);
	}

	return false;
}

static bool table_u32_is(struct blob_attr *tbl, const char *name, uint32_t want)
{
	struct blob_attr *cur;
	size_t rem;

	if (!tbl)
		return false;

	blobmsg_for_each_attr(cur, tbl, rem) {
		if (strcmp(blobmsg_name(cur), name))
			continue;
		if (blobmsg_type(cur) != BLOBMSG_TYPE_INT32)
			return false;

		return blobmsg_get_u32(cur) == want;
	}

	return false;
}

static bool dump_bool_is(struct blob_buf *b, const char *name, bool want)
{
	struct blob_attr *cur;
	size_t rem;

	blob_for_each_attr(cur, b->head, rem) {
		if (strcmp(blobmsg_name(cur), name))
			continue;
		if (blobmsg_type(cur) != BLOBMSG_TYPE_INT8)
			return false;

		return blobmsg_get_u8(cur) == (want ? 1 : 0);
	}

	return false;
}

/*
 * One pool connection, filled by hand. struct sip_conn is public and the two
 * accessors the dump reads (sip_conn_state_name, sip_stream_conn_queued) are
 * pure, so the presence rules are pinned without a pool, a socket or a peer,
 * which is the acct_sub_status_dump() precedent one layer down.
 */
static void conn_fake(struct sip_conn *c, enum sip_conn_state state,
		      enum sip_stream_kind kind, const char *peer, uint16_t port,
		      size_t queued)
{
	memset(c, 0, sizeof(*c));
	c->in_use = true;
	c->state = (uint8_t)state;
	c->key.kind = (uint8_t)kind;
	c->key.local_port = 5060;
	snprintf(c->key.iface, sizeof(c->key.iface), "wan");
	sip_dst_set(&c->key.peer, peer, port);
	c->tx_len = queued;
}

/*
 * The ubus transport contract. Every line carries "transport". A stream line
 * carries a "connection" table only while it has one, and carries "listener"
 * in every case. Reason: a tls listener that stays down is the client-only
 * state that a support case asks about. A tcp listener that did not come back
 * after an interface move is down just as silently. Only a tls line carries
 * "tls_error".
 */
static void test_conn_status(void)
{
	static struct sip_conn conn;
	struct blob_buf b;

	memset(&b, 0, sizeof(b));

	CASE("conn status: a udp line carries the transport and nothing else");
	blob_buf_init(&b, 0);
	acct_conn_status_dump(&b, ACCT_TP_UDP, NULL, false, "");
	CHECK(dump_str_is(&b, "transport", "udp"));
	CHECK_UINT(dump_count(&b), 1);

	/* a udp line has no pool connection to report, whatever it is handed */
	CASE("conn status: a udp line reports no connection even with one");
	conn_fake(&conn, SIP_CONN_UP, SIP_STREAM_TCP, "192.0.2.10", 5060, 0);
	blob_buf_init(&b, 0);
	acct_conn_status_dump(&b, ACCT_TP_UDP, &conn, true, "boom");
	CHECK(dump_str_is(&b, "transport", "udp"));
	CHECK_UINT(dump_count(&b), 1);

	CASE("conn status: a tcp line with no connection emits no connection table");
	blob_buf_init(&b, 0);
	acct_conn_status_dump(&b, ACCT_TP_TCP, NULL, true, "");
	CHECK(dump_str_is(&b, "transport", "tcp"));
	CHECK(!dump_has_key(&b, "connection"));
	CHECK(dump_bool_is(&b, "listener", true));
	CHECK_UINT(dump_count(&b), 2);

	/* a tcp listener that did not come back is the case this key exists
	 * for: nothing else in the status says the line takes no calls */
	CASE("conn status: a tcp listener that is down says so");
	blob_buf_init(&b, 0);
	acct_conn_status_dump(&b, ACCT_TP_TCP, NULL, false, "");
	CHECK(dump_bool_is(&b, "listener", false));

	CASE("conn status: a tcp connection reports its state, peer and queue");
	blob_buf_init(&b, 0);
	acct_conn_status_dump(&b, ACCT_TP_TCP, &conn, true, "");
	CHECK(dump_str_is(&b, "transport", "tcp"));
	CHECK(table_str_is(dump_table(&b, "connection"), "state", "up"));
	CHECK(table_str_is(dump_table(&b, "connection"), "peer", "192.0.2.10:5060"));
	CHECK(table_u32_is(dump_table(&b, "connection"), "queued", 0));

	CASE("conn status: the staged bytes are the queue depth");
	conn_fake(&conn, SIP_CONN_CONNECTING, SIP_STREAM_TCP, "192.0.2.10", 5060, 512);
	blob_buf_init(&b, 0);
	acct_conn_status_dump(&b, ACCT_TP_TCP, &conn, true, "");
	CHECK(table_str_is(dump_table(&b, "connection"), "state", "connecting"));
	CHECK(table_u32_is(dump_table(&b, "connection"), "queued", 512));

	/* the handshake state is the one a tls interop case reads first */
	CASE("conn status: a tls connection in the handshake reads handshake");
	conn_fake(&conn, SIP_CONN_HANDSHAKE, SIP_STREAM_TLS, "192.0.2.11", 5061, 0);
	blob_buf_init(&b, 0);
	acct_conn_status_dump(&b, ACCT_TP_TLS, &conn, true, "");
	CHECK(dump_str_is(&b, "transport", "tls"));
	CHECK(table_str_is(dump_table(&b, "connection"), "state", "handshake"));
	CHECK(table_str_is(dump_table(&b, "connection"), "peer", "192.0.2.11:5061"));

	CASE("conn status: a tls listener that is up says so");
	CHECK(dump_bool_is(&b, "listener", true));

	/*
	 * See sip/tls.h "Client only is a normal state": the line registers
	 * over tls with no certificate of its own and takes no inbound
	 * connection, and this key is the only place that is visible.
	 */
	CASE("conn status: a tls listener that stayed down says so with no connection");
	blob_buf_init(&b, 0);
	acct_conn_status_dump(&b, ACCT_TP_TLS, NULL, false, "");
	CHECK(dump_str_is(&b, "transport", "tls"));
	CHECK(dump_bool_is(&b, "listener", false));
	CHECK(!dump_has_key(&b, "connection"));
	CHECK_UINT(dump_count(&b), 2);

	CASE("conn status: a tls error is carried when there is one");
	blob_buf_init(&b, 0);
	acct_conn_status_dump(&b, ACCT_TP_TLS, NULL, false, "certificate verify failed");
	CHECK(dump_str_is(&b, "tls_error", "certificate verify failed"));

	CASE("conn status: an empty tls error emits no key");
	blob_buf_init(&b, 0);
	acct_conn_status_dump(&b, ACCT_TP_TLS, NULL, true, "");
	CHECK(!dump_has_key(&b, "tls_error"));

	CASE("conn status: a NULL tls error emits none either");
	blob_buf_init(&b, 0);
	acct_conn_status_dump(&b, ACCT_TP_TLS, NULL, true, NULL);
	CHECK(!dump_has_key(&b, "tls_error"));

	/*
	 * A client only TLS line is no fault (sip/tls.h). The load succeeds
	 * with no server context, and the context keeps a reason for the half
	 * it could not build; publishing that residue would put a red error on
	 * every line that is simply waiting for its bundle, so the caller
	 * passes a context error only when the client context failed.
	 */
	CASE("conn status: a client only tls line carries the listener and no error");
	blob_buf_init(&b, 0);
	acct_conn_status_dump(&b, ACCT_TP_TLS, NULL, false, "");
	CHECK(dump_bool_is(&b, "listener", false));
	CHECK(!dump_has_key(&b, "tls_error"));
	CHECK_UINT(dump_count(&b), 2);

	/* tls_error belongs to tls alone: a tcp line has no record layer to
	 * fail and no certificate to be missing */
	CASE("conn status: a tcp line carries no tls error");
	blob_buf_init(&b, 0);
	acct_conn_status_dump(&b, ACCT_TP_TCP, NULL, false, "certificate verify failed");
	CHECK(!dump_has_key(&b, "tls_error"));
	CHECK(dump_bool_is(&b, "listener", false));
	CHECK_UINT(dump_count(&b), 2);

	CASE("conn status: the keys join the line keys and move none of them");
	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "line", 0);
	blobmsg_add_string(&b, "aor", "sip:5551000@sip.example.com");
	acct_conn_status_dump(&b, ACCT_TP_TCP, &conn, true, "");
	CHECK_UINT(dump_count(&b), 5);
	CHECK(dump_name_is(&b, 0, "line"));
	CHECK(dump_name_is(&b, 1, "aor"));
	CHECK(dump_name_is(&b, 2, "transport"));
	CHECK(dump_name_is(&b, 3, "listener"));
	CHECK(dump_name_is(&b, 4, "connection"));

	blob_buf_free(&b);
}

/* ---------------------------------------------------- the stream owner hint */

/*
 * A stream connection names its line through the local identity of its key
 * (sip/stream.h "The connection key"). That identity is the port that the
 * listener bound and the device of the connection scope. It is the owner hint
 * of the dispatch.
 * For a datagram, the same question is which line owns the socket that it
 * arrived on.
 */
static void test_conn_line(void)
{
	static const uint16_t ports[3] = { 5060, 5062, 0 };
	static const char *const ifaces[3] = { "wan", "wan", NULL };
	static const int fam4[3] = { AF_INET, AF_INET, 0 };
	static const uint16_t shared[2] = { 5060, 5060 };
	static const char *const both[2] = { "wan", "wan" };
	static const int both4[2] = { AF_INET, AF_INET };
	static const int split[2] = { AF_INET, AF_INET6 };
	static const uint16_t one[1] = { 5060 };
	static const char *const unscoped[1] = { "" };
	static const int one4[1] = { AF_INET };

	CASE("conn line: the port and the device together name the line");
	CHECK_UINT(acct_conn_line(ports, ifaces, fam4, 3, 5062, "wan", AF_INET),
		   1);
	CHECK_UINT(acct_conn_line(ports, ifaces, fam4, 3, 5060, "wan", AF_INET),
		   0);

	CASE("conn line: a port no line binds names none");
	CHECK_UINT(acct_conn_line(ports, ifaces, fam4, 3, 5070, "wan", AF_INET),
		   ACCT_LINE_NONE);

	CASE("conn line: the same port on another device is another line's");
	CHECK_UINT(acct_conn_line(ports, ifaces, fam4, 3, 5060, "wan6",
				  AF_INET),
		   ACCT_LINE_NONE);

	CASE("conn line: a line that runs no stream is no candidate");
	CHECK_UINT(acct_conn_line(ports, ifaces, fam4, 3, 0, "wan", AF_INET),
		   ACCT_LINE_NONE);

	/*
	 * A listener is opened with IPV6_V6ONLY and a connect names one family,
	 * so the peer family of a key is the family its line signals in. Two
	 * lines on one port and one device in two families are two identities,
	 * and a connection of one of them belongs to that one alone.
	 */
	CASE("conn line: two lines that differ in family alone are two lines");
	CHECK_UINT(acct_conn_line(shared, both, split, 2, 5060, "wan", AF_INET),
		   0);
	CHECK_UINT(acct_conn_line(shared, both, split, 2, 5060, "wan",
				  AF_INET6), 1);

	CASE("conn line: a family no line signals in names none");
	CHECK_UINT(acct_conn_line(shared, both, both4, 2, 5060, "wan",
				  AF_INET6),
		   ACCT_LINE_NONE);

	/*
	 * Two lines that share the identity share the connection, and the first
	 * of them is the hint, which is the rule a shared UDP socket already
	 * follows (acct_sock_line). Answering "no line" instead would drop the
	 * last resort of the dispatch, and the shipped two line configuration is
	 * exactly this case: one port, one device, both lines. A request that
	 * reaches the last resort over a reliable transport gets no
	 * retransmission to save it (RFC 3261 17.2.4).
	 */
	CASE("conn line: an identity two lines share names the first");
	CHECK_UINT(acct_conn_line(shared, both, both4, 2, 5060, "wan", AF_INET),
		   0);

	CASE("conn line: an unscoped bind matches the empty device alone");
	CHECK_UINT(acct_conn_line(one, unscoped, one4, 1, 5060, "", AF_INET),
		   0);
	CHECK_UINT(acct_conn_line(one, unscoped, one4, 1, 5060, NULL, AF_INET),
		   0);
	CHECK_UINT(acct_conn_line(one, unscoped, one4, 1, 5060, "wan", AF_INET),
		   ACCT_LINE_NONE);
}

/* ------------------------------------------ the reconnect that re-registers */

/*
 * A reconnect re-registers (sip/stream.h "Reconnect re-registers", the cb.conn
 * paragraph). The binding at the registrar names the ephemeral five tuple of
 * the connection that carried the REGISTER. After a connection dies and comes
 * back, the binding points at nothing until a refresh gives it the new tuple.
 */
static void test_reconnect(void)
{
	CASE("reconnect: the first connection of a registration is not one");
	CHECK(!acct_reconnect_needed(false, SIP_HANDLE_NONE, sip_handle_make(0, 1),
				     false));

	CASE("reconnect: the same connection reported twice is not one");
	CHECK(!acct_reconnect_needed(true, sip_handle_make(0, 1), sip_handle_make(0, 1),
				     false));

	/* the close cleared the handle, so the up that follows differs */
	CASE("reconnect: an up after the close of that connection is one");
	CHECK(acct_reconnect_needed(true, SIP_HANDLE_NONE, sip_handle_make(0, 1), false));

	CASE("reconnect: another connection under a live one is one too");
	CHECK(acct_reconnect_needed(true, sip_handle_make(0, 1), sip_handle_make(1, 1),
				    false));

	/* the pool reuses a slot with a new generation, which is a new
	 * connection and a new source port (sip/stream.h "Ephemeral source
	 * ports") */
	CASE("reconnect: the same slot in a new generation is one");
	CHECK(acct_reconnect_needed(true, sip_handle_make(3, 1), sip_handle_make(3, 2),
				    false));

	CASE("reconnect: a close before any up leaves the next up the first one");
	CHECK(!acct_reconnect_needed(false, SIP_HANDLE_NONE, sip_handle_make(2, 7),
				     false));

	/*
	 * The peer opened this one from the registrar's address and port, so it
	 * matches the registration key. A refresh would drop the binding while
	 * the request that the peer sent on it waits to be read.
	 */
	CASE("reconnect: a connection the peer opened after a close is not one");
	CHECK(!acct_reconnect_needed(true, SIP_HANDLE_NONE, sip_handle_make(0, 1), true));

	CASE("reconnect: a connection the peer opened under a live one is not one");
	CHECK(!acct_reconnect_needed(true, sip_handle_make(0, 1), sip_handle_make(1, 1),
				     true));

	/*
	 * The seeded registration. A line that starts on a connection another
	 * line already holds up never sees the upcall that opened it, so
	 * account.c seeds the pair from the pool instead; from there it is the
	 * ordinary state machine, and the reopen after the close of that
	 * connection is a reconnect like any other.
	 */
	CASE("reconnect: a seeded registration treats the next connection as one");
	CHECK(acct_reconnect_needed(true, sip_handle_make(4, 1), sip_handle_make(5, 1),
				    false));

	CASE("reconnect: and the seeded connection itself is not one");
	CHECK(!acct_reconnect_needed(true, sip_handle_make(4, 1), sip_handle_make(4, 1),
				     false));
}

/*
 * The refresh that a reconnect forces waits for a registration that has a
 * request in flight. The send that opened the connection can be that request,
 * because a connection reports up before its staged bytes drain (sip/stream.h
 * "Queued send semantics"). A stop and start at that point would abandon the
 * transaction and put a second REGISTER on the wire.
 */
static void test_reconnect_refresh_ok(void)
{
	CASE("refresh gate: a REGISTER in flight holds the refresh back");
	CHECK(!acct_reconnect_refresh_ok(SIP_REG_TRYING));

	CASE("refresh gate: a refresh in flight holds it back too");
	CHECK(!acct_reconnect_refresh_ok(SIP_REG_REFRESHING));

	/* every state with nothing on the wire refreshes: the binding names a
	 * five tuple that is gone and only a REGISTER can move it */
	CASE("refresh gate: a live binding with nothing in flight refreshes");
	CHECK(acct_reconnect_refresh_ok(SIP_REG_REGISTERED));

	CASE("refresh gate: a backoff refreshes");
	CHECK(acct_reconnect_refresh_ok(SIP_REG_BACKOFF));

	CASE("refresh gate: a resolving client refreshes");
	CHECK(acct_reconnect_refresh_ok(SIP_REG_RESOLVING));

	CASE("refresh gate: an unregistered client refreshes");
	CHECK(acct_reconnect_refresh_ok(SIP_REG_UNREGISTERED));
}

/* ------------------------------------------------- the per transport tables */

/*
 * Two tables that the compiler cannot check and the wire does not report on a
 * host. The first is the RFC 3263 4.1 service label of a line, which is part
 * of the cache identity of the resolver. A wrong label serves the targets of
 * another transport. The second is the stream kind that a line opens. A wrong
 * kind gives a tls line a cleartext connection.
 */
static void test_transport_maps(void)
{
	CASE("service: udp resolves _sip._udp");
	CHECK(!strcmp(acct_dns_service(ACCT_TP_UDP), SIP_DNS_SERVICE_DEFAULT));
	CHECK(!strcmp(acct_dns_service(ACCT_TP_UDP), "_sip._udp"));

	CASE("service: tcp resolves _sip._tcp");
	CHECK(!strcmp(acct_dns_service(ACCT_TP_TCP), SIP_DNS_SERVICE_TCP));
	CHECK(!strcmp(acct_dns_service(ACCT_TP_TCP), "_sip._tcp"));

	CASE("service: tls resolves _sips._tcp");
	CHECK(!strcmp(acct_dns_service(ACCT_TP_TLS), SIP_DNS_SERVICE_TLS));
	CHECK(!strcmp(acct_dns_service(ACCT_TP_TLS), "_sips._tcp"));

	CASE("kind: tcp opens a tcp connection");
	CHECK(acct_stream_kind(ACCT_TP_TCP) == SIP_STREAM_TCP);

	CASE("kind: tls opens a tls connection");
	CHECK(acct_stream_kind(ACCT_TP_TLS) == SIP_STREAM_TLS);

	/* udp opens no stream at all, and the value it maps to is what a
	 * listener would bind if one ever asked */
	CASE("kind: udp maps to the tcp value and opens nothing");
	CHECK(acct_stream_kind(ACCT_TP_UDP) == SIP_STREAM_TCP);
}

/* ------------------------------------------------------------- the SNI rule */

/*
 * A TLS line sends server_name for a name target and none for an address:
 * RFC 6066 section 3 forbids a literal in the extension. The connection reuse
 * match reads the same name (sip/stream.h "SNI splits a TLS connection"). A
 * wrong answer here therefore fails silently: it gives the connection of one
 * carrier to another name.
 */
static void test_sni(void)
{
	struct acct_cfg c;

	memset(&c, 0, sizeof(c));
	snprintf(c.resolve_host, sizeof(c.resolve_host), "sip.example.com");

	CASE("sni: a udp line never sends one");
	c.transport = ACCT_TP_UDP;
	CHECK(acct_sni_of(&c) == NULL);

	CASE("sni: a tcp line never sends one either");
	c.transport = ACCT_TP_TCP;
	CHECK(acct_sni_of(&c) == NULL);

	CASE("sni: a tls line sends the name it resolved");
	c.transport = ACCT_TP_TLS;
	CHECK(acct_sni_of(&c) != NULL);
	CHECK(!strcmp(acct_sni_of(&c), "sip.example.com"));

	CASE("sni: a tls line with an IPv4 literal target sends none");
	snprintf(c.resolve_host, sizeof(c.resolve_host), "192.0.2.10");
	CHECK(acct_sni_of(&c) == NULL);

	CASE("sni: an IPv6 literal is a literal too");
	snprintf(c.resolve_host, sizeof(c.resolve_host), "2001:db8::1");
	CHECK(acct_sni_of(&c) == NULL);

	CASE("sni: a line with no resolve host at all sends none");
	c.resolve_host[0] = 0;
	CHECK(acct_sni_of(&c) == NULL);
}

/* ------------------------------------------------ the source address probe */

/*
 * A stream line has no datagram socket to ask for its own address. The Via
 * and the Contact therefore come from the route: a transient connected
 * datagram socket that sends nothing. Loopback is the one destination that a
 * host always has.
 */
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

/*
 * The argv entries a step needs, counted from the rendered command: one per
 * space separated token plus the NULL execv() takes. It is only exact while no
 * single argument holds a space, which a one port rule satisfies.
 */
static unsigned int step_argc(unsigned int idx)
{
	char cmd[256];
	const char *p = fw_plan_step(idx, cmd, sizeof(cmd));
	unsigned int n = 1;

	if (!p || !*p)
		return 0;

	while (*p) {
		if (*p == ' ')
			n++;
		p++;
	}

	return n + 1;
}

/* The open form, which is what every rule set looked like before mode 1. */
static void fw_open_set(const char *iface, const char *sip_ports, bool proto_udp,
			const struct fw_rtp_range *rtp, unsigned int rtp_count)
{
	fw_voip_set(iface, sip_ports, proto_udp, rtp, rtp_count, NULL, 0, true);
}

/*
 * The firewall rule set. The firewall helper is absent off target, so the
 * observable is the argument vector that the reconcile would run. The plan is
 * built before the script is looked up, and fw_plan_step() formats exactly
 * what fw_spawn() receives.
 *
 * The RTP rules matter: with the SIP rule alone, the wan firewall drops the
 * inbound media stream, so the far end hears the handset and the handset
 * hears silence.
 */
static void test_firewall(void)
{
	struct fw_rtp_range rtp[2];
	char cmd[256];

	memset(rtp, 0, sizeof(rtp));
	rtp[0].channel = 1;
	rtp[0].min = 10000;
	rtp[0].max = 10030;
	rtp[1].channel = 2;
	rtp[1].min = 10000;
	rtp[1].max = 10030;

	CASE("firewall: one line opens SIP and its own RTP range");
	fw_open_set("wan", "5060", true, rtp, 1);
	CHECK_UINT(fw_plan_count(), ADD(2));

	CASE("firewall: every known rule name is deleted first");
	CHECK(step_is(0, SCRIPT " -d Allow_VoIP_SIP -r"));
	CHECK(step_is(1, SCRIPT " -d Allow_VoIP_RTP_CH1 -r"));
	CHECK(step_is(2, SCRIPT " -d Allow_VoIP_RTP_CH2 -r"));

	/*
	 * The proxy names are deleted whatever the mode: a box that ran
	 * restrict_source 1 and then went open keeps its Allow_VoIP_PROXY<n>
	 * rules, and nothing else on the system removes them.
	 */
	CASE("firewall: the proxy rule names are deleted too, in every mode");
	CHECK(step_is(3, SCRIPT " -d Allow_VoIP_PROXY1 -r"));
	CHECK(step_is(DELS - 1, SCRIPT " -d Allow_VoIP_PROXY16 -r"));
	CHECK_UINT(DELS, 19);

	CASE("firewall: the SIP rule keeps its shape");
	CHECK(step_is(ADD(0), SCRIPT " -a Allow_VoIP_SIP -p 5060 -P udp -i wan -r"));

	CASE("firewall: the RTP rule opens the range as a udp dest_port range");
	CHECK(step_is(ADD(1), SCRIPT " -a Allow_VoIP_RTP_CH1 -p 10000-10031 -P udp -i wan"));

	/*
	 * The allocator hands out even RTP ports across the whole range, so the
	 * top pair of a range ending on an even port has its RTCP one past max.
	 * The rule has to admit the pair, not the bare range.
	 */
	CASE("firewall: the rule admits the top RTP pair, not the bare range");
	{
		struct cfg_channel ch;
		uint16_t lo = 0, hi = 0, p = 0, last = 0;

		memset(&ch, 0, sizeof(ch));
		net_port_range(&ch, &lo, &hi);
		CHECK_UINT(lo, 10000);
		CHECK_UINT(hi, 10030);

		net_port_reset();
		while (net_port_alloc(lo, hi, &p))
			last = p;
		net_port_reset();

		/* the top even port is allocatable, and its RTCP is above max */
		CHECK_UINT(last, 10030);
		CHECK_UINT(last + 1, 10031);

		rtp[0].min = lo;
		rtp[0].max = hi;
		fw_open_set("wan", "5060", true, rtp, 1);
		CHECK(step_has(ADD(1), "-p 10000-10031"));
	}

	CASE("firewall: a range ending on an odd port covers its pair too");
	rtp[0].min = 30000;
	rtp[0].max = 30011;
	fw_open_set("wan", "5060", true, rtp, 1);
	CHECK(step_has(ADD(1), "-p 30000-30012"));
	rtp[0].min = 10000;
	rtp[0].max = 10030;

	CASE("firewall: a range at the top of the port space does not wrap");
	rtp[0].min = 65500;
	rtp[0].max = 65535;
	fw_open_set("wan", "5060", true, rtp, 1);
	CHECK(step_has(ADD(1), "-p 65500-65535"));
	rtp[0].min = 10000;
	rtp[0].max = 10030;
	fw_open_set("wan", "5060", true, rtp, 1);

	CASE("firewall: only the last step reloads fw4 (-r batching)");
	CHECK(step_has(ADD(0), " -r"));
	CHECK(!step_has(ADD(1), " -r"));

	CASE("firewall: a second line adds its own rule");
	fw_open_set("wan", "5060 5062", true, rtp, 2);
	CHECK_UINT(fw_plan_count(), ADD(3));
	CHECK(step_is(ADD(0), SCRIPT " -a Allow_VoIP_SIP -p 5060 5062 -P udp -i wan -r"));
	CHECK(step_is(ADD(1), SCRIPT " -a Allow_VoIP_RTP_CH1 -p 10000-10031 -P udp -i wan -r"));
	CHECK(step_is(ADD(2), SCRIPT " -a Allow_VoIP_RTP_CH2 -p 10000-10031 -P udp -i wan"));

	CASE("firewall: a per channel range reaches its own rule");
	rtp[1].min = 20000;
	rtp[1].max = 20040;
	fw_open_set("wan", "5060 5062", true, rtp, 2);
	CHECK(step_is(ADD(2), SCRIPT " -a Allow_VoIP_RTP_CH2 -p 20000-20041 -P udp -i wan"));

	CASE("firewall: the voice interface follows the config");
	fw_open_set("wan2", "5060", true, rtp, 1);
	CHECK(step_has(ADD(1), "-i wan2"));

	CASE("firewall: an empty interface takes wan");
	fw_open_set(NULL, "5060", true, rtp, 1);
	CHECK(step_has(ADD(1), "-i wan"));

	CASE("firewall: tcp signalling still opens RTP over udp");
	fw_open_set("wan", "5060", false, rtp, 1);
	CHECK(step_has(ADD(0), "-P tcp"));
	CHECK(step_has(ADD(1), "-P udp"));

	CASE("firewall: an incoherent range is dropped, never opened wrong");
	rtp[0].min = 0;
	fw_open_set("wan", "5060", true, rtp, 1);
	CHECK_UINT(fw_plan_count(), ADD(1));
	CHECK(step_has(ADD(0), "Allow_VoIP_SIP"));
	rtp[0].min = 10000;

	CASE("firewall: no line at all deletes every rule and adds none");
	fw_voip_clear();
	CHECK_UINT(fw_plan_count(), DELS);
	CHECK(step_is(DELS - 1, SCRIPT " -d Allow_VoIP_PROXY16"));

	CASE("firewall: empty SIP ports are a clear, not an empty rule");
	fw_open_set("wan", "", true, rtp, 1);
	CHECK_UINT(fw_plan_count(), DELS);

	CASE("firewall: a step past the end reports nothing");
	CHECK(fw_plan_step(99, cmd, sizeof(cmd)) == NULL);
	CHECK(fw_plan_step(0, NULL, 0) == NULL);
}

/*
 * restrict_source 1. One Allow_VoIP_PROXY<n> per
 * distinct resolved proxy address replaces the open Allow_VoIP_SIP rule, and
 * each carries that address as its source. The RTP rules do not change,
 * because the media of an asymmetric peer arrives from an address that the
 * signaling never named. Before the first address resolves, the open rule
 * stays.
 */
static void test_firewall_restrict(void)
{
	char proxy[FW_PROXY_MAX][FW_SRC_MAX];
	struct fw_rtp_range rtp[2];
	struct sip_dns_result res;
	unsigned int i, n;

	memset(proxy, 0, sizeof(proxy));
	memset(rtp, 0, sizeof(rtp));
	rtp[0].channel = 1;
	rtp[0].min = 10000;
	rtp[0].max = 10030;
	rtp[1].channel = 2;
	rtp[1].min = 10000;
	rtp[1].max = 10030;

	CASE("restrict: two proxies replace the SIP rule with one rule each");
	n = 0;
	n = fw_reconcile_proxy_add(proxy, n, "198.51.100.7");
	n = fw_reconcile_proxy_add(proxy, n, "203.0.113.9");
	CHECK_UINT(n, 2);
	fw_voip_set("wan", "5060 5062", true, rtp, 2, proxy, n, fw_reconcile_sip_open_needed(1, n));
	CHECK_UINT(fw_plan_count(), ADD(4));
	CHECK(step_is(ADD(0), SCRIPT " -a Allow_VoIP_PROXY1 -p 5060 5062 -P udp "
		      "-s 198.51.100.7 -i wan -r"));
	CHECK(step_is(ADD(1), SCRIPT " -a Allow_VoIP_PROXY2 -p 5060 5062 -P udp "
		      "-s 203.0.113.9 -i wan -r"));
	CHECK(!plan_has("-a Allow_VoIP_SIP"));

	CASE("restrict: the RTP rules follow the proxy rules, unrestricted");
	CHECK(step_is(ADD(2), SCRIPT " -a Allow_VoIP_RTP_CH1 -p 10000-10031 -P udp -i wan -r"));
	CHECK(step_is(ADD(3), SCRIPT " -a Allow_VoIP_RTP_CH2 -p 10000-10031 -P udp -i wan"));

	CASE("restrict: the source add step fits the argv the runner builds");
	fw_voip_set("wan", "5060", true, rtp, 1, proxy, 1, false);
	CHECK(step_is(ADD(0), SCRIPT " -a Allow_VoIP_PROXY1 -p 5060 -P udp "
		      "-s 198.51.100.7 -i wan -r"));
	CHECK_UINT(step_argc(ADD(0)), 13);

	CASE("restrict: nothing resolved yet keeps the open rule");
	n = 0;
	CHECK(fw_reconcile_sip_open_needed(1, n));
	CHECK(fw_reconcile_sip_open_needed(2, n));
	fw_voip_set("wan", "5060", true, rtp, 1, proxy, n, fw_reconcile_sip_open_needed(1, n));
	CHECK_UINT(fw_plan_count(), ADD(2));
	CHECK(step_is(ADD(0), SCRIPT " -a Allow_VoIP_SIP -p 5060 -P udp -i wan -r"));
	CHECK(!plan_has("-a Allow_VoIP_PROXY"));

	CASE("restrict: mode 0 opens the SIP rule whatever was resolved");
	n = fw_reconcile_proxy_add(proxy, 0, "198.51.100.7");
	CHECK(fw_reconcile_sip_open_needed(0, n));
	fw_voip_set("wan", "5060", true, rtp, 1, proxy, n, fw_reconcile_sip_open_needed(0, n));
	CHECK_UINT(fw_plan_count(), ADD(2));
	CHECK(step_is(ADD(0), SCRIPT " -a Allow_VoIP_SIP -p 5060 -P udp -i wan -r"));
	CHECK(!plan_has("-s "));

	CASE("restrict: two lines behind one proxy hold one rule");
	n = 0;
	n = fw_reconcile_proxy_add(proxy, n, "198.51.100.7");	/* line 1 target */
	n = fw_reconcile_proxy_add(proxy, n, "198.51.100.7");	/* line 2, same target */
	CHECK_UINT(n, 1);
	fw_voip_set("wan", "5060 5062", true, rtp, 2, proxy, n, fw_reconcile_sip_open_needed(1, n));
	CHECK_UINT(fw_plan_count(), ADD(3));
	CHECK(step_is(ADD(0), SCRIPT " -a Allow_VoIP_PROXY1 -p 5060 5062 -P udp "
		      "-s 198.51.100.7 -i wan -r"));

	/* A target whose address does not print must not eat a rule slot, or the
	 * rule after it would carry the wrong number. */
	CASE("restrict: an empty address is not an address");
	{
		char empty[FW_PROXY_MAX][FW_SRC_MAX];

		memset(empty, 0, sizeof(empty));
		CHECK_UINT(fw_reconcile_proxy_add(empty, 0, ""), 0);
		CHECK_UINT(fw_reconcile_proxy_add(empty, 0, NULL), 0);
		CHECK(!empty[0][0]);
	}

	/*
	 * The rule namespace is fixed, so the ninth address gets no rule. It is
	 * dropped and reported rather than renumbered over an existing rule.
	 */
	CASE("restrict: an address past the bound is dropped, not renumbered");
	n = 0;
	for (i = 0; i < FW_PROXY_MAX + 1; i++) {
		char addr[FW_SRC_MAX];

		snprintf(addr, sizeof(addr), "198.51.100.%u", i + 1);
		n = fw_reconcile_proxy_add(proxy, n, addr);
	}
	CHECK_UINT(n, FW_PROXY_MAX);
	CHECK(!strcmp(proxy[FW_PROXY_MAX - 1], "198.51.100.8"));

	CASE("restrict: the bounded set is the whole rule set the chain adds");
	fw_voip_set("wan", "5060", true, rtp, 2, proxy, n, fw_reconcile_sip_open_needed(1, n));
	CHECK_UINT(fw_plan_count(), ADD(FW_PROXY_MAX + 2));
	CHECK(step_has(ADD(0),
		       "-a Allow_VoIP_PROXY1 -p 5060 -P udp -s 198.51.100.1"));
	CHECK(step_has(ADD(FW_PROXY_MAX - 1),
		       "-a Allow_VoIP_PROXY16 -p 5060 -P udp -s 198.51.100.8"));
	CHECK(!plan_has("198.51.100.9"));
	CHECK(step_is(ADD(FW_PROXY_MAX + 1), SCRIPT " -a Allow_VoIP_RTP_CH2 -p 10000-10031 "
		      "-P udp -i wan"));

	/*
	 * Mode 1 keeps its whole machinery when mode 2 becomes the pinhole
	 * form: the open-rule fallback answers for both modes, and only the
	 * ingress-rule question separates them (test_firewall_pinhole).
	 */
	CASE("restrict: mode 2 leaves the mode 1 fallback rule untouched");
	n = fw_reconcile_proxy_add(proxy, 0, "198.51.100.7");
	CHECK(!fw_reconcile_sip_open_needed(1, n));
	CHECK(!fw_reconcile_sip_open_needed(2, n));
	fw_voip_set("wan", "5060", true, rtp, 1, proxy, n, fw_reconcile_sip_open_needed(1, n));
	CHECK_UINT(fw_plan_count(), ADD(2));
	CHECK(step_is(ADD(0), SCRIPT " -a Allow_VoIP_PROXY1 -p 5060 -P udp "
		      "-s 198.51.100.7 -i wan -r"));
	CHECK(step_has(ADD(1), "-a Allow_VoIP_RTP_CH1"));

	/* Every SRV target belongs in the restricted source set. */
	CASE("restrict: SRV failover reaches a target covered by the rule set");
	rk_reset();
	sip_reg_start(&rk_reg);
	answer_make(&res, DNS_A, DNS_B);
	acct_targets_load(&rk_reg, &res);
	CHECK(current_host_is(DNS_A));
	sip_reg_txn_done(&rk_reg, rk_reg.txn, SIP_TXN_DONE_TIMEOUT);
	CHECK(current_host_is(DNS_B));
	rk.now = rk_reg.timer.due_ms;
	sip_timer_expire(&rk_reg.timer);
	CHECK(!strcmp(rk.host, DNS_B));
	memset(proxy, 0, sizeof(proxy));
	n = acct_proxy_collect(&rk_reg, true, proxy, 0);
	CHECK_UINT(n, 2);
	fw_voip_set("wan", "5060", true, rtp, 1, proxy, n, fw_reconcile_sip_open_needed(1, n));
	CHECK(plan_has("-s " DNS_A));
	CHECK(plan_has("-s " DNS_B));
	CHECK(!plan_has("-a Allow_VoIP_SIP"));

	/*
	 * A zone with records of both families fills the answer, and the
	 * registration accepts every endpoint of it, so the rule set has to
	 * admit every one: a target with no rule is a proxy whose traffic the
	 * firewall drops.
	 */
	CASE("restrict: a sixteen endpoint answer loads whole and rules whole");
	rk_reset();
	sip_reg_start(&rk_reg);
	memset(&res, 0, sizeof(res));
	res.status = SIP_DNS_OK;
	res.ttl = 300;
	res.count = FW_PROXY_MAX;
	for (i = 0; i < FW_PROXY_MAX; i++) {
		char host[FW_SRC_MAX];

		if (i < FW_PROXY_MAX / 2)
			snprintf(host, sizeof(host), "198.51.100.%u", i + 10);
		else
			snprintf(host, sizeof(host), "2001:db8:1::%u", i + 10);

		snprintf(res.target[i].host, sizeof(res.target[i].host), "%s",
			 host);
		res.target[i].port = 5060;
		CHECK(sip_dst_set(&res.target[i].dst, host, 5060));
	}

	acct_targets_load(&rk_reg, &res);
	CHECK_UINT(sip_reg_target_count(&rk_reg), FW_PROXY_MAX);
	memset(proxy, 0, sizeof(proxy));
	n = acct_proxy_collect(&rk_reg, true, proxy, 0);
	CHECK_UINT(n, FW_PROXY_MAX);
	fw_voip_set("wan", "5060", true, rtp, 1, proxy, n,
		    fw_reconcile_sip_open_needed(1, n));
	CHECK_UINT(fw_plan_count(), ADD(FW_PROXY_MAX + 1));

	CASE("restrict: and every one of the sixteen holds a rule of its own");
	for (i = 0; i < FW_PROXY_MAX; i++) {
		char want[FW_SRC_MAX + 8];

		snprintf(want, sizeof(want), "-s %s", res.target[i].host);
		CHECK(plan_has(want));
	}
	CHECK(!plan_has("-a Allow_VoIP_SIP"));

	sip_reg_done(&rk_reg);
	sip_txn_mgr_done(&rk_mgr);
}

/*
 * restrict_source 2, the keepalive pinhole. The reconcile installs no rule
 * for it: it deletes the whole namespace, as the disable
 * path does, and adds nothing. Inbound service uses conntrack: the SIP socket
 * pings the proxy, and the media sockets are bound to the advertised ports
 * and connected. Every tuple that the far end answers on therefore has an
 * established entry.
 *
 * The cases drive fw_reconcile_apply(), the production policy. The static
 * fw_reconcile() collects the inputs from the account table and passes them
 * on, and nothing here implements a decision again.
 */
static void test_firewall_pinhole(void)
{
	char cleared[DELS][160];
	char proxy[FW_PROXY_MAX][FW_SRC_MAX];
	struct fw_rtp_range rtp[1];
	struct sip_dns_result res;
	unsigned int i, n;

	memset(proxy, 0, sizeof(proxy));
	memset(rtp, 0, sizeof(rtp));
	rtp[0].channel = 1;
	rtp[0].min = 10000;
	rtp[0].max = 10030;

	CASE("pinhole: mode 2 asks for no rule although two addresses resolved");
	rk_reset();
	sip_reg_start(&rk_reg);
	answer_make(&res, DNS_A, DNS_B);
	acct_targets_load(&rk_reg, &res);
	n = acct_proxy_collect(&rk_reg, true, proxy, 0);
	CHECK_UINT(n, 2);
	CHECK(!fw_reconcile_ingress_rules_needed(2));
	CHECK(fw_reconcile_ingress_rules_needed(0));
	CHECK(fw_reconcile_ingress_rules_needed(1));

	CASE("pinhole: the reconcile after a resolution deletes and adds nothing");
	fw_test_mode(true);
	fw_reconcile_apply("wan", 2, 1, "5060", true, rtp, 1, proxy, n);
	CHECK_UINT(fw_plan_count(), DELS);
	CHECK(!plan_has(" -a "));
	for (i = 0; i < DELS; i++) {
		CHECK(step_has(i, " -d "));
		fw_test_step_done(0);
	}
	CHECK_UINT(fw_test_spawned(), DELS);

	CASE("pinhole: no add chain follows the deletes");
	fw_test_step_done(0);
	CHECK_UINT(fw_test_spawned(), DELS);
	CHECK_UINT(fw_plan_count(), 0);

	/*
	 * The mode is live (schema APPLY_LIVE), so a reload can move it either
	 * way with the registration untouched. Both directions have to reach the
	 * firewall on the next reconcile.
	 */
	CASE("pinhole: a flip to mode 1 installs the proxy rules");
	n = acct_proxy_collect(&rk_reg, true, proxy, 0);
	CHECK_UINT(n, 2);
	fw_reconcile_apply("wan", 1, 1, "5060", true, rtp, 1, proxy, n);
	CHECK_UINT(fw_plan_count(), ADD(3));
	CHECK(step_has(ADD(0), "-a Allow_VoIP_PROXY1 -p 5060 -P udp -s " DNS_A));
	CHECK(step_has(ADD(1), "-a Allow_VoIP_PROXY2 -p 5060 -P udp -s " DNS_B));
	CHECK(step_has(ADD(2), "-a Allow_VoIP_RTP_CH1"));
	for (i = 0; i < ADD(3); i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_test_spawned(), DELS + ADD(3));

	CASE("pinhole: a flip back to mode 2 deletes them again");
	fw_reconcile_apply("wan", 2, 1, "5060", true, rtp, 1, proxy, n);
	CHECK_UINT(fw_plan_count(), DELS);
	CHECK(!plan_has(" -a "));
	for (i = 0; i < DELS; i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_test_spawned(), DELS + ADD(3) + DELS);

	/*
	 * Nothing resolved is where mode 1 falls back to the open rule. Mode 2
	 * has no such fallback: the pinhole form is the whole answer, so a line
	 * that never resolved still installs nothing.
	 */
	CASE("pinhole: mode 2 with nothing resolved installs nothing either");
	rk_reset();
	fw_test_mode(true);
	memset(proxy, 0, sizeof(proxy));
	n = acct_proxy_collect(&rk_reg, true, proxy, 0);
	CHECK_UINT(n, 0);
	CHECK(fw_reconcile_sip_open_needed(2, n));
	CHECK(!fw_reconcile_ingress_rules_needed(2));
	fw_reconcile_apply("wan", 2, 1, "5060", true, rtp, 1, proxy, n);
	CHECK_UINT(fw_plan_count(), DELS);
	CHECK(!plan_has("-a Allow_VoIP_SIP"));
	CHECK(!plan_has(" -a "));
	for (i = 0; i < DELS; i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_test_spawned(), DELS);

	/*
	 * The mode 2 chain and the disable chain are one chain. A step that
	 * differed, the fw4 reload on the last one above all, would make the
	 * pinhole mode a second teardown path with its own failure modes.
	 */
	CASE("pinhole: the mode 2 chain is the disable chain, step for step");
	fw_test_mode(false);
	fw_reconcile_apply("wan", 2, 1, "5060", true, rtp, 1, proxy, n);
	CHECK_UINT(fw_plan_count(), DELS);
	for (i = 0; i < DELS; i++) {
		fw_plan_step(i, cleared[i], sizeof(cleared[i]));
		CHECK(has_str(cleared[i], " -d "));
	}
	fw_open_set("wan", "", true, rtp, 1);		/* the disable path */
	CHECK_UINT(fw_plan_count(), DELS);
	for (i = 0; i < DELS; i++)
		CHECK(step_is(i, cleared[i]));
	CHECK(step_is(DELS - 1, SCRIPT " -d Allow_VoIP_PROXY16"));

	sip_reg_done(&rk_reg);
	sip_txn_mgr_done(&rk_mgr);
}

/*
 * A DNS address change updates the restricted firewall rules. The DNS
 * completion is the sixth reconcile call site
 * (account.c account_dns_result), and these cases run its production stages.
 * acct_targets_load() puts the answer on the target list, acct_proxy_collect()
 * takes the rule sources off that list, and fw_reconcile_apply() composes the
 * desired set. A moved address must move the rules. An answer that moved
 * nothing must not run a chain again, because the reconcile runs on every
 * completion. A chain per TTL cycle would reload fw4 for nothing.
 */
static void test_firewall_dns_move(void)
{
	char proxy[FW_PROXY_MAX][FW_SRC_MAX];
	struct fw_rtp_range rtp[1];
	struct sip_dns_result res;
	unsigned int i, n;

	memset(rtp, 0, sizeof(rtp));
	rtp[0].channel = 1;
	rtp[0].min = 10000;
	rtp[0].max = 10030;

	CASE("dns move: the first completion opens one rule per resolved address");
	rk_reset();
	sip_reg_start(&rk_reg);
	answer_make(&res, DNS_A, DNS_B);
	acct_targets_load(&rk_reg, &res);
	fw_test_mode(true);
	memset(proxy, 0, sizeof(proxy));
	n = acct_proxy_collect(&rk_reg, true, proxy, 0);
	CHECK_UINT(n, 2);
	fw_reconcile_apply("wan", 1, 1, "5060", true, rtp, 1, proxy, n);
	CHECK_UINT(fw_plan_count(), ADD(3));
	CHECK(step_has(ADD(0), "-a Allow_VoIP_PROXY1 -p 5060 -P udp -s " DNS_A));
	CHECK(step_has(ADD(1), "-a Allow_VoIP_PROXY2 -p 5060 -P udp -s " DNS_B));
	for (i = 0; i < ADD(3); i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_test_spawned(), ADD(3));

	CASE("dns move: a completion carrying the same answer runs no chain");
	answer_make(&res, DNS_A, DNS_B);
	acct_targets_load(&rk_reg, &res);
	memset(proxy, 0, sizeof(proxy));
	n = acct_proxy_collect(&rk_reg, true, proxy, 0);
	CHECK_UINT(n, 2);
	fw_reconcile_apply("wan", 1, 1, "5060", true, rtp, 1, proxy, n);
	CHECK_UINT(fw_test_spawned(), ADD(3));
	CHECK_UINT(fw_plan_count(), 0);

	/*
	 * A changed set must not leave the stale source its rule and the new
	 * one none, or the firewall blocks the proxy that answers.
	 * The rule namespace is deleted whole before the adds, so the stale rule
	 * goes even though its name is reused.
	 */
	CASE("dns move: a changed address set moves the rules to the new addresses");
	answer_make(&res, DNS_B, DNS_C);
	res.changed = true;
	acct_targets_load(&rk_reg, &res);
	memset(proxy, 0, sizeof(proxy));
	n = acct_proxy_collect(&rk_reg, true, proxy, 0);
	CHECK_UINT(n, 2);
	CHECK(!strcmp(proxy[0], DNS_B));
	CHECK(!strcmp(proxy[1], DNS_C));
	fw_reconcile_apply("wan", 1, 1, "5060", true, rtp, 1, proxy, n);
	CHECK_UINT(fw_plan_count(), ADD(3));
	CHECK(step_is(1 + CFG_CHANNEL_MAX, SCRIPT " -d Allow_VoIP_PROXY1 -r"));
	CHECK(step_is(2 + CFG_CHANNEL_MAX, SCRIPT " -d Allow_VoIP_PROXY2 -r"));
	CHECK(step_has(ADD(0), "-a Allow_VoIP_PROXY1 -p 5060 -P udp -s " DNS_B));
	CHECK(step_has(ADD(1), "-a Allow_VoIP_PROXY2 -p 5060 -P udp -s " DNS_C));
	CHECK(!plan_has("-s " DNS_A));

	CASE("dns move: the moved rules are what the chain runs");
	for (i = 0; i < ADD(3); i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_test_spawned(), ADD(3) * 2);

	/*
	 * The resolver redraws equal priority SRV records on every delivery
	 * (shared/resolve.h L17), so the same two servers come back in the
	 * other order once per TTL cycle. The sorted source list turns that
	 * answer into the same desired set, the comparison absorbs it and no
	 * chain runs. A list in answer order would swap the sources between
	 * PROXY1 and PROXY2 and reload fw4 every cycle for no change in what is
	 * admitted.
	 */
	CASE("dns move: a redraw of the same servers runs no chain");
	answer_make(&res, DNS_C, DNS_B);
	acct_targets_load(&rk_reg, &res);
	memset(proxy, 0, sizeof(proxy));
	n = acct_proxy_collect(&rk_reg, true, proxy, 0);
	CHECK_UINT(n, 2);
	CHECK(!strcmp(proxy[0], DNS_B));
	CHECK(!strcmp(proxy[1], DNS_C));
	fw_reconcile_apply("wan", 1, 1, "5060", true, rtp, 1, proxy, n);
	CHECK_UINT(fw_test_spawned(), ADD(3) * 2);
	CHECK_UINT(fw_plan_count(), 0);

	/*
	 * The retention in the rule set: the member the rotation left keeps
	 * its rule for the window, through the same two-pass collect the
	 * reconcile runs, and the expiry then removes it.
	 */
	CASE("dns move: a rotated address keeps its rule for the linger window");
	{
		struct acct_linger lg;
		struct sip_txn_dst d;

		memset(&lg, 0, sizeof(lg));
		CHECK(sip_dst_set(&d, DNS_C, 5060));	/* the member that left */
		acct_linger_note(&lg, &d, 1000);
		CHECK(sip_dst_set(&d, DNS_A, 5060));	/* the current member */
		acct_linger_note(&lg, &d, 2000);
		answer_make(&res, DNS_A, NULL);
		acct_targets_load(&rk_reg, &res);
		memset(proxy, 0, sizeof(proxy));
		n = acct_proxy_collect(&rk_reg, true, proxy, 0);
		CHECK_UINT(n, 1);
		n = acct_linger_collect(&lg, proxy, n);
		CHECK_UINT(n, 2);
		fw_reconcile_apply("wan", 1, 1, "5060", true, rtp, 1, proxy, n);
		CHECK_UINT(fw_plan_count(), ADD(3));
		CHECK(step_has(ADD(0), "-s " DNS_A));
		CHECK(step_has(ADD(1), "-s " DNS_C));
		CHECK(!plan_has("-s " DNS_B));
		for (i = 0; i < ADD(3); i++)
			fw_test_step_done(0);

		CASE("dns move: the expiry then removes the rotated address");
		acct_linger_expire(&lg, 1000 + 10 * 1000, 10);
		memset(proxy, 0, sizeof(proxy));
		n = acct_proxy_collect(&rk_reg, true, proxy, 0);
		n = acct_linger_collect(&lg, proxy, n);
		CHECK_UINT(n, 1);
		fw_reconcile_apply("wan", 1, 1, "5060", true, rtp, 1, proxy, n);
		CHECK_UINT(fw_plan_count(), ADD(2));
		CHECK(step_has(ADD(0), "-s " DNS_A));
		CHECK(!plan_has("-s " DNS_C));
		for (i = 0; i < ADD(2); i++)
			fw_test_step_done(0);
	}

	fw_test_mode(false);
	sip_reg_done(&rk_reg);
	sip_txn_mgr_done(&rk_mgr);
}

/*
 * One overflow build under the warning capture, which the helper ends. A
 * needle therefore costs its own build, and the build is pure, so repeating
 * it for each needle costs nothing but the calls.
 */
static unsigned int proxy_overflow_warns(const char *const *addrs, unsigned int count,
					 bool reverse, const char *needle)
{
	char list[FW_PROXY_MAX][FW_SRC_MAX];
	unsigned int i, n = 0, hits;

	memset(list, 0, sizeof(list));
	warn_capture_begin();

	for (i = 0; i < count; i++)
		n = fw_reconcile_proxy_add(list, n, addrs[reverse ? count - 1 - i : i]);

	hits = warn_capture_count(needle);
	warn_capture_end();

	return hits;
}

/*
 * RFC 2782 weight selection reorders equal priority SRV answers by design, and
 * the resolver draws again on every delivery (shared/resolve.h L17). A TTL
 * re-resolution therefore gives the account the same servers in another
 * order. The proxy source list is sorted for that reason: the same set must
 * give the same list. Otherwise the rule numbering follows the draw, and every
 * cycle costs a whole delete and add chain plus an fw4 reload.
 *
 * The cases drive the production stages, never a copy. acct_targets_load()
 * takes the scripted answer, acct_proxy_collect() reads the targets off it,
 * and fw_reconcile_apply() composes the desired set.
 */
static void test_firewall_redraw(void)
{
	/* two addresses more than the rule namespace holds, all of one
	 * width, so the sorted order and the numeric order are one list */
	static const char *const many[] = {
		"198.51.100.27", "198.51.100.13", "198.51.100.20",
		"198.51.100.11", "198.51.100.26", "198.51.100.14",
		"198.51.100.22", "198.51.100.12", "198.51.100.19",
		"198.51.100.10", "198.51.100.24", "198.51.100.16",
		"198.51.100.21", "198.51.100.15", "198.51.100.25",
		"198.51.100.17", "198.51.100.23", "198.51.100.18",
	};
	const unsigned int manys = sizeof(many) / sizeof(many[0]);
	char first[ADD(FW_PROXY_MAX + 1)][160];
	char proxy[FW_PROXY_MAX][FW_SRC_MAX];
	char other[FW_PROXY_MAX][FW_SRC_MAX];
	struct fw_rtp_range rtp[1];
	struct sip_dns_result res;
	unsigned int i, n, m;

	memset(rtp, 0, sizeof(rtp));
	rtp[0].channel = 1;
	rtp[0].min = 10000;
	rtp[0].max = 10030;

	/*
	 * Two independent registrations take the two draws, so nothing but the
	 * answer order differs between the lists they produce.
	 */
	CASE("redraw: the same address set in either order collects one list");
	rk_reset();
	sip_reg_start(&rk_reg);
	answer_make(&res, DNS_B, DNS_A);
	acct_targets_load(&rk_reg, &res);
	memset(proxy, 0, sizeof(proxy));
	n = acct_proxy_collect(&rk_reg, true, proxy, 0);

	rk_reset();
	sip_reg_start(&rk_reg);
	answer_make(&res, DNS_A, DNS_B);
	acct_targets_load(&rk_reg, &res);
	memset(other, 0, sizeof(other));
	m = acct_proxy_collect(&rk_reg, true, other, 0);

	CHECK_UINT(n, 2);
	CHECK_UINT(m, 2);
	CHECK(!strcmp(proxy[0], DNS_A));
	CHECK(!strcmp(proxy[1], DNS_B));
	CHECK(!strcmp(other[0], DNS_A));
	CHECK(!strcmp(other[1], DNS_B));

	CASE("redraw: and one rule list, step for step, numbering included");
	fw_test_mode(true);
	fw_reconcile_apply("wan", 1, 1, "5060", true, rtp, 1, proxy, n);
	CHECK_UINT(fw_plan_count(), ADD(3));
	for (i = 0; i < ADD(3); i++)
		fw_plan_step(i, first[i], sizeof(first[i]));

	fw_test_mode(true);
	fw_reconcile_apply("wan", 1, 1, "5060", true, rtp, 1, other, m);
	CHECK_UINT(fw_plan_count(), ADD(3));
	for (i = 0; i < ADD(3); i++)
		CHECK(step_is(i, first[i]));

	/*
	 * The control. A set that really moved must still rewrite, or the sort
	 * would have bought the no-op by making every answer look unchanged.
	 */
	CASE("redraw: a set that really changed still rewrites the rules");
	fw_test_mode(true);
	rk_reset();
	sip_reg_start(&rk_reg);
	answer_make(&res, DNS_A, DNS_B);
	acct_targets_load(&rk_reg, &res);
	memset(proxy, 0, sizeof(proxy));
	n = acct_proxy_collect(&rk_reg, true, proxy, 0);
	fw_reconcile_apply("wan", 1, 1, "5060", true, rtp, 1, proxy, n);
	CHECK_UINT(fw_plan_count(), ADD(3));
	for (i = 0; i < ADD(3); i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_test_spawned(), ADD(3));

	answer_make(&res, DNS_C, DNS_B);
	res.changed = true;
	acct_targets_load(&rk_reg, &res);
	memset(proxy, 0, sizeof(proxy));
	n = acct_proxy_collect(&rk_reg, true, proxy, 0);
	CHECK_UINT(n, 2);
	CHECK(!strcmp(proxy[0], DNS_B));
	CHECK(!strcmp(proxy[1], DNS_C));
	fw_reconcile_apply("wan", 1, 1, "5060", true, rtp, 1, proxy, n);
	CHECK_UINT(fw_plan_count(), ADD(3));
	CHECK(step_has(ADD(0), "-s " DNS_B));
	CHECK(step_has(ADD(1), "-s " DNS_C));
	CHECK(!plan_has("-s " DNS_A));
	CHECK_UINT(fw_test_spawned(), ADD(3) + 1);
	for (i = 1; i < ADD(3); i++)
		fw_test_step_done(0);

	/*
	 * The rule namespace is smaller than the answer can be, so somebody
	 * loses a rule. Which one must not depend on the draw either: the list
	 * keeps the FW_PROXY_MAX lexicographically smallest addresses, so the
	 * same answer always blacks out the same proxy.
	 */
	CASE("redraw: an overflowing set drops the same addresses in either order");
	memset(proxy, 0, sizeof(proxy));
	memset(other, 0, sizeof(other));
	n = 0;
	for (i = 0; i < manys; i++)
		n = fw_reconcile_proxy_add(proxy, n, many[i]);
	m = 0;
	for (i = manys; i > 0; i--)
		m = fw_reconcile_proxy_add(other, m, many[i - 1]);

	CHECK_UINT(n, FW_PROXY_MAX);
	CHECK_UINT(m, FW_PROXY_MAX);
	for (i = 0; i < FW_PROXY_MAX; i++)
		CHECK(!strcmp(proxy[i], other[i]));
	CHECK(!strcmp(proxy[0], "198.51.100.10"));
	CHECK(!strcmp(proxy[FW_PROXY_MAX - 1], "198.51.100.25"));

	CASE("redraw: the overflowing set is one rule list too");
	fw_test_mode(true);
	fw_reconcile_apply("wan", 1, 1, "5060", true, rtp, 1, proxy, n);
	CHECK_UINT(fw_plan_count(), ADD(FW_PROXY_MAX + 1));
	for (i = 0; i < ADD(FW_PROXY_MAX + 1); i++)
		fw_plan_step(i, first[i], sizeof(first[i]));

	fw_test_mode(true);
	fw_reconcile_apply("wan", 1, 1, "5060", true, rtp, 1, other, m);
	CHECK_UINT(fw_plan_count(), ADD(FW_PROXY_MAX + 1));
	for (i = 0; i < ADD(FW_PROXY_MAX + 1); i++)
		CHECK(step_is(i, first[i]));
	CHECK(step_has(ADD(0),
		       "-a Allow_VoIP_PROXY1 -p 5060 -P udp -s 198.51.100.10"));
	CHECK(step_has(ADD(FW_PROXY_MAX - 1),
		       "-a Allow_VoIP_PROXY16 -p 5060 -P udp "
		       "-s 198.51.100.25"));
	CHECK(!plan_has("-s 198.51.100.26"));
	CHECK(!plan_has("-s 198.51.100.27"));

	/*
	 * The warn is the only sign an operator gets that a proxy holds no rule
	 * and has its traffic dropped. Naming a surviving address instead would
	 * read as a working rule set with a spurious complaint, and would send
	 * the diagnosis at the proxy that still works. The addresses it names
	 * must be the ones that lost their rule, in either draw.
	 */
	CASE("redraw: the overflow warn names the addresses that lost their rule");
	CHECK_UINT(proxy_overflow_warns(many, manys, false, "198.51.100.27"),
		   1);
	CHECK_UINT(proxy_overflow_warns(many, manys, false, "198.51.100.26"),
		   1);
	CHECK_UINT(proxy_overflow_warns(many, manys, true, "198.51.100.27"), 1);
	CHECK_UINT(proxy_overflow_warns(many, manys, true, "198.51.100.26"), 1);
	CHECK_UINT(proxy_overflow_warns(many, manys, false, "198.51.100.25"),
		   0);
	CHECK_UINT(proxy_overflow_warns(many, manys, true, "198.51.100.10"), 0);

	fw_test_mode(false);
	sip_reg_done(&rk_reg);
	sip_txn_mgr_done(&rk_mgr);
}

/*
 * The chain in flight executes an immutable snapshot. The steps already
 * spawned cannot carry a desired set that changes while a child runs. The
 * completion must therefore notice the change and start a second chain. The
 * completion records that snapshot as the applied set. A copy of the desired
 * set instead would record rules that were never installed, and the
 * coalescing pass would then skip them for good.
 */
static void test_firewall_inflight(void)
{
	char proxy[FW_PROXY_MAX][FW_SRC_MAX];
	struct fw_rtp_range rtp[2];
	unsigned int i;

	memset(proxy, 0, sizeof(proxy));
	memset(rtp, 0, sizeof(rtp));
	rtp[0].channel = 1;
	rtp[0].min = 10000;
	rtp[0].max = 10030;
	rtp[1].channel = 2;
	rtp[1].min = 20000;
	rtp[1].max = 20040;

	CASE("in flight: a chain starts for the first desired set");
	fw_test_mode(true);
	fw_open_set("wan", "5060", true, rtp, 1);
	CHECK_UINT(fw_plan_count(), ADD(2));
	CHECK_UINT(fw_test_spawned(), 1);

	CASE("in flight: a set that arrives mid chain does not rewrite it");
	fw_test_step_done(0);				/* the SIP delete completed */
	fw_open_set("wan", "5060 5062", true, rtp, 2);	/* the line 2 rule appears */
	CHECK_UINT(fw_plan_count(), ADD(2));		/* still the old chain */
	CHECK(step_is(ADD(0), SCRIPT " -a Allow_VoIP_SIP -p 5060 -P udp -i wan -r"));
	CHECK(step_is(ADD(1), SCRIPT " -a Allow_VoIP_RTP_CH1 -p 10000-10031 -P udp -i wan"));

	CASE("in flight: the completion starts a chain for what is wanted now");
	for (i = 1; i < ADD(2); i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_plan_count(), ADD(3));
	CHECK_UINT(fw_test_spawned(), ADD(2) + 1);
	CHECK_UINT(fw_test_retry_ms(), 0);	/* success coalesces, no timer */

	CASE("in flight: the second chain carries the rule the first could not");
	CHECK(step_is(ADD(0), SCRIPT " -a Allow_VoIP_SIP -p 5060 5062 -P udp -i wan -r"));
	CHECK(step_is(ADD(2), SCRIPT " -a Allow_VoIP_RTP_CH2 -p 20000-20041 -P udp -i wan"));

	CASE("in flight: it settles and nothing more runs");
	for (i = 1; i < ADD(3); i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_test_spawned(), ADD(2) + ADD(3));		/* 13 steps, then 14 */
	fw_test_step_done(0);
	fw_open_set("wan", "5060 5062", true, rtp, 2);
	CHECK_UINT(fw_test_spawned(), ADD(2) + ADD(3));

	CASE("in flight: a set identical to the one running starts no second chain");
	fw_test_mode(true);
	fw_open_set("wan", "5060", true, rtp, 1);
	fw_test_step_done(0);
	fw_open_set("wan", "5060", true, rtp, 1);
	for (i = 1; i < ADD(2); i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_test_spawned(), ADD(2));

	CASE("in flight: a clear mid chain is picked up at the completion");
	fw_test_mode(true);
	fw_open_set("wan", "5060", true, rtp, 1);
	fw_test_step_done(0);
	fw_voip_clear();
	for (i = 1; i < ADD(2); i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_plan_count(), DELS);
	CHECK_UINT(fw_test_spawned(), ADD(2) + 1);

	/*
	 * The live order of a daemon start: the first reconcile runs before any
	 * account is active, so it clears, and the populated set follows once
	 * the line comes up. Both arrival points have to end with the adds on
	 * the wire, because a clear chain alone leaves no voice rule at all.
	 */
	CASE("live order: a populated set after the clear chain runs its adds");
	fw_test_mode(true);
	fw_voip_clear();
	CHECK_UINT(fw_plan_count(), DELS);
	for (i = 0; i < DELS; i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_test_spawned(), DELS);
	fw_open_set("wan", "5060", true, rtp, 1);
	CHECK_UINT(fw_plan_count(), ADD(2));
	CHECK(step_has(ADD(0), "-a Allow_VoIP_SIP -p 5060"));
	CHECK(step_has(ADD(1), "-a Allow_VoIP_RTP_CH1 -p 10000-10031"));
	CHECK_UINT(fw_test_spawned(), DELS + 1);

	CASE("live order: a populated set DURING the clear chain runs its adds");
	fw_test_mode(true);
	fw_voip_clear();
	fw_test_step_done(0);				/* the clear chain is running */
	fw_open_set("wan", "5060", true, rtp, 1);	/* the account came up */
	CHECK_UINT(fw_plan_count(), DELS);		/* the chain is untouched */
	for (i = 1; i < DELS; i++)
		fw_test_step_done(0);

	CASE("live order: the completion notices the clear installed nothing");
	CHECK_UINT(fw_plan_count(), ADD(2));
	CHECK(step_has(ADD(0), "-a Allow_VoIP_SIP -p 5060"));
	CHECK(step_has(ADD(1), "-a Allow_VoIP_RTP_CH1 -p 10000-10031"));
	CHECK_UINT(fw_test_spawned(), DELS + 1);

	CASE("live order: the add chain then settles and stays settled");
	for (i = 1; i < ADD(2); i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_test_spawned(), DELS + ADD(2));
	fw_open_set("wan", "5060", true, rtp, 1);
	CHECK_UINT(fw_test_spawned(), DELS + ADD(2));

	/*
	 * A proxy that moved is the same rule name with a different source. The
	 * chain comparison has to see the source, or the moved address would
	 * never reach the firewall and the box would keep admitting the old one.
	 */
	CASE("in flight: a proxy address that moves alone re-chains");
	fw_test_mode(true);
	snprintf(proxy[0], sizeof(proxy[0]), "198.51.100.7");
	fw_voip_set("wan", "5060", true, rtp, 1, proxy, 1, false);
	for (i = 0; i < ADD(2); i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_test_spawned(), ADD(2));
	snprintf(proxy[0], sizeof(proxy[0]), "198.51.100.8");
	fw_voip_set("wan", "5060", true, rtp, 1, proxy, 1, false);
	CHECK_UINT(fw_test_spawned(), ADD(2) + 1);
	CHECK(step_has(ADD(0), "-a Allow_VoIP_PROXY1 -p 5060 -P udp -s 198.51.100.8"));

	CASE("in flight: a source change mid chain is carried by the completion");
	fw_test_mode(true);
	snprintf(proxy[0], sizeof(proxy[0]), "198.51.100.7");
	fw_voip_set("wan", "5060", true, rtp, 1, proxy, 1, false);
	fw_test_step_done(0);
	snprintf(proxy[0], sizeof(proxy[0]), "198.51.100.8");
	fw_voip_set("wan", "5060", true, rtp, 1, proxy, 1, false);
	CHECK(step_has(ADD(0), "-s 198.51.100.7"));	/* the chain is untouched */
	for (i = 1; i < ADD(2); i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_test_spawned(), ADD(2) + 1);
	CHECK(step_has(ADD(0), "-s 198.51.100.8"));

	CASE("in flight: the re-chained source settles");
	for (i = 1; i < ADD(2); i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_test_spawned(), ADD(2) * 2);
	fw_test_step_done(0);
	fw_voip_set("wan", "5060", true, rtp, 1, proxy, 1, false);
	CHECK_UINT(fw_test_spawned(), ADD(2) * 2);

	fw_test_mode(false);
}

/*
 * The answer that fw_voip_set() and fw_voip_clear() give the reconcile log.
 * It is true while the ask differs from what the firewall holds or has under
 * way, so a chain runs for it. It is false for an ask that the firewall holds
 * or already carries. The comparison covers the whole rule set, so a proxy
 * address that the log line does not show still counts. A failed chain arms a
 * retry of the same ask, which its failure line reports. That ask answers
 * false until the retry runs; a changed ask answers true and runs at once.
 */
static void test_firewall_change_report(void)
{
	char proxy[FW_PROXY_MAX][FW_SRC_MAX];
	struct fw_rtp_range rtp[1];
	unsigned int i;

	memset(proxy, 0, sizeof(proxy));
	memset(rtp, 0, sizeof(rtp));
	rtp[0].channel = 1;
	rtp[0].min = 10000;
	rtp[0].max = 10030;

	CASE("report: the first ask differs from an empty install");
	fw_test_mode(true);
	CHECK(fw_voip_set("wan", "5060", true, rtp, 1, proxy, 0, true));
	for (i = 0; i < ADD(2); i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_plan_count(), 0);

	CASE("report: the same ask again runs no chain");
	CHECK(!fw_voip_set("wan", "5060", true, rtp, 1, proxy, 0, true));
	CHECK_UINT(fw_plan_count(), 0);

	CASE("report: a proxy address the log line does not show still counts");
	snprintf(proxy[0], sizeof(proxy[0]), "192.0.2.10");
	CHECK(fw_voip_set("wan", "5060", true, rtp, 1, proxy, 1, false));
	for (i = 0; i < ADD(2); i++)
		fw_test_step_done(0);
	CHECK(!fw_voip_set("wan", "5060", true, rtp, 1, proxy, 1, false));
	snprintf(proxy[0], sizeof(proxy[0]), "192.0.2.11");
	CHECK(fw_voip_set("wan", "5060", true, rtp, 1, proxy, 1, false));
	for (i = 0; i < ADD(2); i++)
		fw_test_step_done(0);
	CHECK(!fw_voip_set("wan", "5060", true, rtp, 1, proxy, 1, false));

	CASE("report: the retry carries a failed ask, a changed ask runs at once");
	snprintf(proxy[0], sizeof(proxy[0]), "192.0.2.12");
	CHECK(fw_voip_set("wan", "5060", true, rtp, 1, proxy, 1, false));
	for (i = 0; i < DELS; i++)
		fw_test_step_done(0);
	fw_test_step_done(1);
	CHECK_UINT(fw_test_retry_ms(), FW_RETRY_MS);
	CHECK(!fw_voip_set("wan", "5060", true, rtp, 1, proxy, 1, false));
	CHECK_UINT(fw_test_retry_ms(), FW_RETRY_MS);
	snprintf(proxy[0], sizeof(proxy[0]), "192.0.2.13");
	CHECK(fw_voip_set("wan", "5060", true, rtp, 1, proxy, 1, false));
	CHECK_UINT(fw_test_retry_ms(), 0);
	for (i = 0; i < ADD(2); i++)
		fw_test_step_done(0);
	CHECK(!fw_voip_set("wan", "5060", true, rtp, 1, proxy, 1, false));

	CASE("report: clearing an installed set is a chain, clearing again is not");
	CHECK(fw_voip_clear());
	for (i = 0; i < DELS; i++)
		fw_test_step_done(0);
	CHECK(!fw_voip_clear());

	/*
	 * While a chain runs, the reference is what that chain installs, not
	 * the set it replaces: an ask that differs from the running snapshot
	 * gets its own chain from fw_chain_done() and must be logged for it,
	 * and the ask the chain already carries must not.
	 */
	CASE("report: an ask that repeats the running chain is no new chain");
	CHECK(fw_voip_set("wan", "5060", true, rtp, 1, proxy, 0, true));
	CHECK(!fw_voip_set("wan", "5060", true, rtp, 1, proxy, 0, true));

	CASE("report: a clear under a running add chain is a chain of its own");
	CHECK(fw_voip_clear());
	CHECK(!fw_voip_clear());
	for (i = 0; i < ADD(2); i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_plan_count(), DELS);
	CHECK(!fw_voip_clear());
	for (i = 0; i < DELS; i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_plan_count(), 0);
	CHECK(!fw_voip_clear());
	fw_test_mode(false);
}

/*
 * The failure backoff. A chain whose step exits nonzero must never start
 * again synchronously. A synchronous restart runs the whole chain and a fw4
 * reload again, with no backoff, for as long as the step fails. The fixture
 * fails steps by hand and fires the retry as the uloop dispatch would, so no
 * clock is involved.
 */
static void test_firewall_retry(void)
{
	static const unsigned int walk[] = {
		2 * FW_RETRY_MS, 4 * FW_RETRY_MS, 8 * FW_RETRY_MS,
		FW_RETRY_MAX_MS, FW_RETRY_MAX_MS
	};
	struct fw_rtp_range rtp[1];
	unsigned int i, w;

	memset(rtp, 0, sizeof(rtp));
	rtp[0].channel = 1;
	rtp[0].min = 10000;
	rtp[0].max = 10030;

	CASE("retry: a failed add arms the backoff instead of a re-kick");
	fw_test_mode(true);
	fw_open_set("wan", "5060", true, rtp, 1);
	for (i = 0; i < DELS; i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_test_spawned(), DELS + 1);	/* the SIP add is running */
	fw_test_step_done(1);
	CHECK_UINT(fw_test_spawned(), DELS + 1);	/* nothing re-ran */
	CHECK_UINT(fw_plan_count(), 0);
	CHECK_UINT(fw_test_retry_ms(), FW_RETRY_MS);

	CASE("retry: the fired timer re-runs the whole chain");
	CHECK(fw_test_retry_fire());
	CHECK_UINT(fw_plan_count(), ADD(2));
	CHECK_UINT(fw_test_spawned(), DELS + 2);
	CHECK_UINT(fw_test_retry_ms(), 0);

	CASE("retry: consecutive failures escalate to the cap");
	for (w = 0; w < 5; w++) {
		for (i = 0; i < DELS; i++)
			fw_test_step_done(0);
		fw_test_step_done(1);
		CHECK_UINT(fw_test_retry_ms(), walk[w]);
		CHECK(fw_test_retry_fire());
	}

	CASE("retry: a completed chain resets the ladder");
	for (i = 0; i < ADD(2); i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_test_retry_ms(), 0);
	fw_open_set("wan", "5061", true, rtp, 1);
	for (i = 0; i < DELS; i++)
		fw_test_step_done(0);
	fw_test_step_done(1);
	CHECK_UINT(fw_test_retry_ms(), FW_RETRY_MS);	/* the first rung again */

	CASE("retry: a failed delete retries once inside the chain, no timer");
	fw_test_mode(true);
	fw_open_set("wan", "5060", true, rtp, 1);
	CHECK_UINT(fw_test_spawned(), 1);
	fw_test_step_done(3);
	CHECK_UINT(fw_test_spawned(), 2);		/* the same step, at once */
	CHECK_UINT(fw_test_retry_ms(), 0);
	CHECK_UINT(fw_plan_count(), ADD(2));
	fw_test_step_done(0);
	CHECK_UINT(fw_test_spawned(), 3);		/* the chain advanced */

	CASE("retry: a delete that fails twice aborts the chain to the timer");
	fw_test_step_done(3);
	fw_test_step_done(3);
	CHECK_UINT(fw_test_spawned(), 4);		/* only the one retry ran */
	CHECK_UINT(fw_test_retry_ms(), FW_RETRY_MS);

	CASE("retry: the same set re-asserted during the backoff waits");
	fw_open_set("wan", "5060", true, rtp, 1);
	CHECK_UINT(fw_test_spawned(), 4);
	CHECK_UINT(fw_test_retry_ms(), FW_RETRY_MS);

	CASE("retry: a new set during the backoff runs at once");
	fw_open_set("wan", "5062", true, rtp, 1);
	CHECK_UINT(fw_test_spawned(), 5);
	CHECK_UINT(fw_test_retry_ms(), 0);		/* the timer went with it */

	CASE("retry: the ladder survives the new set");
	for (i = 0; i < DELS; i++)
		fw_test_step_done(0);
	fw_test_step_done(1);
	CHECK_UINT(fw_test_retry_ms(), 2 * FW_RETRY_MS);

	CASE("retry: a failure forgets the applied set");
	fw_test_mode(true);
	fw_open_set("wan", "5060", true, rtp, 1);
	for (i = 0; i < ADD(2); i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_test_spawned(), ADD(2));		/* the set is applied */
	fw_open_set("wan", "5062", true, rtp, 1);
	for (i = 0; i < DELS; i++)
		fw_test_step_done(0);
	fw_test_step_done(1);
	CHECK_UINT(fw_test_retry_ms(), FW_RETRY_MS);
	/* the failed chain deleted the old rules, so the old set must not
	 * look applied: re-asserting it has to start a chain right away */
	fw_open_set("wan", "5060", true, rtp, 1);
	CHECK_UINT(fw_test_spawned(), ADD(2) + DELS + 2);
	CHECK_UINT(fw_test_retry_ms(), 0);

	CASE("retry: a set changed mid chain rides the fired retry");
	fw_test_mode(true);
	fw_open_set("wan", "5060", true, rtp, 1);
	fw_test_step_done(0);
	fw_open_set("wan", "5062", true, rtp, 1);	/* mid chain move */
	for (i = 1; i < DELS; i++)
		fw_test_step_done(0);
	fw_test_step_done(1);
	CHECK_UINT(fw_test_spawned(), DELS + 1);	/* no immediate kick */
	CHECK_UINT(fw_test_retry_ms(), FW_RETRY_MS);
	CHECK(fw_test_retry_fire());
	CHECK(step_has(ADD(0), "-p 5062"));		/* the retry carries it */

	CASE("retry: a reload failure (2) on the last add takes the backoff");
	fw_test_mode(true);
	fw_open_set("wan", "5060", true, rtp, 1);
	for (i = 0; i < ADD(2) - 1; i++)
		fw_test_step_done(0);
	fw_test_step_done(2);				/* the reload-carrying add */
	CHECK_UINT(fw_test_spawned(), ADD(2));
	CHECK_UINT(fw_test_retry_ms(), FW_RETRY_MS);

	CASE("retry: a reload failure (2) on a clear chain burns no delete retry");
	fw_test_mode(true);
	fw_voip_clear();
	CHECK_UINT(fw_plan_count(), DELS);
	for (i = 0; i < DELS - 1; i++)
		fw_test_step_done(0);
	fw_test_step_done(2);				/* the last delete reloads */
	CHECK_UINT(fw_test_spawned(), DELS);		/* no same-step retry ran */
	CHECK_UINT(fw_test_retry_ms(), FW_RETRY_MS);

	fw_test_mode(false);
}

/* ------------------------------------------------------- the NOTIFY routing */

/*
 * Which line a NOTIFY names. The request URI answers it for every other
 * method, but a NOTIFY is addressed to the Contact the registration published
 * and that URI need not carry the subscriber's user part at all, so the To
 * user is the fallback (account.h). It is a
 * fallback for NOTIFY alone: reading To for an in-dialog method would hand a
 * request to the line the far end happens to name.
 */
static char route_buf[512];
static struct sip_msg route_msg;

static const struct sip_msg *request_take(const char *text)
{
	size_t len = strlen(text);

	if (len >= sizeof(route_buf))
		return NULL;

	memcpy(route_buf, text, len);
	if (msg_parse(&route_msg, route_buf, len) != SIP_PARSE_OK)
		return NULL;

	return &route_msg;
}

static void test_notify_route(void)
{
	const struct sip_msg *m;
	struct sip_str user;

	CASE("route: a NOTIFY whose request URI names no user is routed by To");
	m = request_take("NOTIFY sip:203.0.113.9:5060 SIP/2.0\r\n"
			 "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
			 "From: <sip:sw@example.net>;tag=a\r\n"
			 "To: <sip:5551000@example.net>\r\n"
			 "Call-ID: c1\r\n"
			 "CSeq: 1 NOTIFY\r\n"
			 "Event: message-summary\r\n"
			 "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(m && acct_notify_to_user(m, &user));
	CHECK_STR(user, "5551000");

	CASE("route: the To user of a two line device names the second line");
	m = request_take("NOTIFY sip:203.0.113.9:5060 SIP/2.0\r\n"
			 "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
			 "From: <sip:sw@example.net>;tag=a\r\n"
			 "To: \"Line Two\" <sip:5551001@example.net>;tag=b\r\n"
			 "Call-ID: c1\r\n"
			 "CSeq: 1 NOTIFY\r\n"
			 "Event: message-summary\r\n"
			 "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(m && acct_notify_to_user(m, &user));
	CHECK_STR(user, "5551001");

	CASE("route: the fallback is NOTIFY only");
	m = request_take("INVITE sip:203.0.113.9:5060 SIP/2.0\r\n"
			 "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
			 "From: <sip:sw@example.net>;tag=a\r\n"
			 "To: <sip:5551000@example.net>\r\n"
			 "Call-ID: c1\r\n"
			 "CSeq: 1 INVITE\r\n"
			 "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(m && !acct_notify_to_user(m, &user));

	CASE("route: a To with no user names no line");
	m = request_take("NOTIFY sip:203.0.113.9:5060 SIP/2.0\r\n"
			 "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
			 "From: <sip:sw@example.net>;tag=a\r\n"
			 "To: <sip:example.net>\r\n"
			 "Call-ID: c1\r\n"
			 "CSeq: 1 NOTIFY\r\n"
			 "Event: message-summary\r\n"
			 "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(m && !acct_notify_to_user(m, &user));

	CASE("route: a response names no line");
	m = request_take("SIP/2.0 200 OK\r\n"
			 "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
			 "From: <sip:sw@example.net>;tag=a\r\n"
			 "To: <sip:5551000@example.net>;tag=b\r\n"
			 "Call-ID: c1\r\n"
			 "CSeq: 1 NOTIFY\r\n"
			 "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(m && !acct_notify_to_user(m, &user));

	/*
	 * Which line an ambiguous name selects. Two lines can carry one user,
	 * and this path writes a line's message waiting store, so the first
	 * table entry is not a good enough answer: the socket the datagram
	 * arrived on is evidence that table order is not.
	 */
	CASE("route: one line carrying the name takes it whatever socket it came on");
	{
		static const char *const users[2] = { "5551000", "5551001" };

		CHECK_UINT(acct_user_line(users, 2, sip_str_cstr("5551001"), 0), 1);
		CHECK_UINT(acct_user_line(users, 2, sip_str_cstr("5551001"),
					  ACCT_LINE_NONE), 1);
	}

	CASE("route: two lines sharing the name hand it to the socket owner");
	{
		static const char *const users[2] = { "5551000", "5551000" };

		CHECK_UINT(acct_user_line(users, 2, sip_str_cstr("5551000"), 1), 1);
	}

	CASE("route: and to the other line when the socket is the other way round");
	{
		static const char *const users[2] = { "5551000", "5551000" };

		CHECK_UINT(acct_user_line(users, 2, sip_str_cstr("5551000"), 0), 0);
	}

	CASE("route: an ambiguous name with no socket owner keeps the first line");
	{
		static const char *const users[2] = { "5551000", "5551000" };

		CHECK_UINT(acct_user_line(users, 2, sip_str_cstr("5551000"),
					  ACCT_LINE_NONE), 0);
	}

	CASE("route: a socket owner carrying another name does not steal it");
	{
		static const char *const users[2] = { "5551000", "5551001" };

		CHECK_UINT(acct_user_line(users, 2, sip_str_cstr("5551000"), 1), 0);
	}

	CASE("route: a line that is not active carries no name");
	{
		static const char *const users[2] = { NULL, "5551000" };

		CHECK_UINT(acct_user_line(users, 2, sip_str_cstr("5551000"), 0), 1);
		CHECK_UINT(acct_user_line(users, 2, sip_str_cstr("5552000"), 0),
			   ACCT_LINE_NONE);
	}

	/*
	 * The NOTIFY routing by event class. The intake of this file answers
	 * every event package the daemon serves before the call layer sees the
	 * request. A transfer NOTIFY answered 489 there never reaches the leg
	 * that waits for that outcome. The class is the whole routing key, and
	 * the two rows below are the two answers it has to keep apart on
	 * messages of exactly this shape.
	 *
	 * The request intake of account.c is static, so what is pinned here is
	 * the key it reads; the leg side of the same NOTIFY is test_call's.
	 */
	CASE("route: a transfer NOTIFY names the class the call layer owns");
	m = request_take("NOTIFY sip:5551000@10.0.0.5 SIP/2.0\r\n"
			 "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
			 "From: <sip:1002@example.net>;tag=a\r\n"
			 "To: <sip:5551000@example.net>;tag=b\r\n"
			 "Call-ID: c1\r\n"
			 "CSeq: 4 NOTIFY\r\n"
			 "Event: refer;id=2\r\n"
			 "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(m && sip_sub_event_class(m) == SIP_SUB_EV_REFER);

	CASE("route: while a message summary still names the MWI store");
	m = request_take("NOTIFY sip:5551000@10.0.0.5 SIP/2.0\r\n"
			 "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
			 "From: <sip:sw@example.net>;tag=a\r\n"
			 "To: <sip:5551000@example.net>\r\n"
			 "Call-ID: c1\r\n"
			 "CSeq: 1 NOTIFY\r\n"
			 "Event: message-summary\r\n"
			 "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(m && sip_sub_event_class(m) == SIP_SUB_EV_MWI);

	CASE("route: and a transfer NOTIFY is routed by To like every other one");
	m = request_take("NOTIFY sip:203.0.113.9:5060 SIP/2.0\r\n"
			 "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
			 "From: <sip:1002@example.net>;tag=a\r\n"
			 "To: <sip:5551001@example.net>;tag=b\r\n"
			 "Call-ID: c1\r\n"
			 "CSeq: 4 NOTIFY\r\n"
			 "Event: refer;id=2\r\n"
			 "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(m && acct_notify_to_user(m, &user));
	CHECK_STR(user, "5551001");
}

/* -------------------------------------------------- the Replaces admission */

/*
 * Which INVITE the capacity refusal answers: the intake of this file answers
 * a full line 486 before the call layer sees the request. A Replaces INVITE
 * frees the leg that it names, and test_call covers that leg side. A refusal
 * before the match would answer three cases with one wrong 486: an attended
 * transfer completion, an unknown dialog and an unreadable header. The intake
 * therefore reads only the presence of the header, and call.c answers such an
 * INVITE from the match: 400, 481 or 486.
 *
 * An unreadable header routes like a readable one, because the call layer
 * must answer it 400, not 486. The suite drives the predicate that the intake
 * reads (account.h, acct_invite_replaces). The NOTIFY routing cases above
 * check the class key of the static intake in the same way.
 */
static void test_replaces_admission(void)
{
	const struct sip_msg *m;

	CASE("admission: an INVITE carrying Replaces belongs to the call layer");
	m = request_take("INVITE sip:5551000@10.0.0.5 SIP/2.0\r\n"
			 "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
			 "From: <sip:1002@example.net>;tag=a\r\n"
			 "To: <sip:5551000@example.net>\r\n"
			 "Call-ID: c1\r\n"
			 "CSeq: 1 INVITE\r\n"
			 "Replaces: c9;to-tag=t9;from-tag=f9\r\n"
			 "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(m && acct_invite_replaces(m));

	CASE("admission: an INVITE without one is still the capacity refusal's");
	m = request_take("INVITE sip:5551000@10.0.0.5 SIP/2.0\r\n"
			 "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
			 "From: <sip:1002@example.net>;tag=a\r\n"
			 "To: <sip:5551000@example.net>\r\n"
			 "Call-ID: c1\r\n"
			 "CSeq: 1 INVITE\r\n"
			 "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(m && !acct_invite_replaces(m));

	/*
	 * Presence is not count. An empty value is a header this layer sees and
	 * call.c's hdr_val_count() does not: the comma walk skips an empty item,
	 * so the admission there reads no Replaces at all and answers the INVITE
	 * as an ordinary one. No 400 is owed for it, and the case below is the
	 * one that reaches that 400.
	 */
	CASE("admission: an empty Replaces is present here and counts zero in call.c");
	m = request_take("INVITE sip:5551000@10.0.0.5 SIP/2.0\r\n"
			 "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
			 "From: <sip:1002@example.net>;tag=a\r\n"
			 "To: <sip:5551000@example.net>\r\n"
			 "Call-ID: c1\r\n"
			 "CSeq: 1 INVITE\r\n"
			 "Replaces: \r\n"
			 "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(m && acct_invite_replaces(m));

	/*
	 * The 400 a 486 here would hide. A Replaces with no parameters at all
	 * names a call-id and neither tag, which hdr_replaces_parse() refuses;
	 * call.c answers it 400. It has to travel to reach that answer, so a
	 * full line must not refuse it here.
	 */
	CASE("admission: a Replaces with no tags routes, and call.c answers it 400");
	m = request_take("INVITE sip:5551000@10.0.0.5 SIP/2.0\r\n"
			 "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
			 "From: <sip:1002@example.net>;tag=a\r\n"
			 "To: <sip:5551000@example.net>\r\n"
			 "Call-ID: c1\r\n"
			 "CSeq: 1 INVITE\r\n"
			 "Replaces: c9\r\n"
			 "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(m && acct_invite_replaces(m));

	CASE("admission: two of them are one presence, and the 400 is call.c's");
	m = request_take("INVITE sip:5551000@10.0.0.5 SIP/2.0\r\n"
			 "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
			 "From: <sip:1002@example.net>;tag=a\r\n"
			 "To: <sip:5551000@example.net>\r\n"
			 "Call-ID: c1\r\n"
			 "CSeq: 1 INVITE\r\n"
			 "Replaces: c8;to-tag=t8;from-tag=f8\r\n"
			 "Replaces: c9;to-tag=t9;from-tag=f9\r\n"
			 "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(m && acct_invite_replaces(m));

	CASE("admission: a method that is not INVITE reads no Replaces at all");
	m = request_take("BYE sip:5551000@10.0.0.5 SIP/2.0\r\n"
			 "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
			 "From: <sip:1002@example.net>;tag=a\r\n"
			 "To: <sip:5551000@example.net>;tag=b\r\n"
			 "Call-ID: c1\r\n"
			 "CSeq: 2 BYE\r\n"
			 "Replaces: c9;to-tag=t9;from-tag=f9\r\n"
			 "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(m && !acct_invite_replaces(m));

	/*
	 * A response is not a request, which the CSeq of the one below names an
	 * INVITE without changing. msg_parse() leaves method unset for a
	 * response, so the method gate alone already answers this; the pin
	 * holds the contract rather than that parser detail, because a
	 * predicate the header says takes a request must answer for one it is
	 * handed.
	 */
	CASE("admission: a response carrying Replaces is no INVITE of ours");
	m = request_take("SIP/2.0 200 OK\r\n"
			 "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
			 "From: <sip:1002@example.net>;tag=a\r\n"
			 "To: <sip:5551000@example.net>;tag=b\r\n"
			 "Call-ID: c1\r\n"
			 "CSeq: 1 INVITE\r\n"
			 "Replaces: c9;to-tag=t9;from-tag=f9\r\n"
			 "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(m && !acct_invite_replaces(m));
}

/* ------------------------------------------------ the registration events */

/*
 * The three sip_reg callbacks account.c installs post the voip
 * registration events (event.h). They are static, and each one needs a built
 * account, so the suite drives the same calls they make instead, on the values
 * acct_cfg_derive() produced: the 0 based line index and the configured next
 * hop that becomes the Proxy.
 *
 * What this does not cover. The callback bodies are pinned by inspection only.
 * The e2e run does not cover them either: event-post.sh is absent on the build
 * host, so the soft gate is off for the whole e2e registration and no post is
 * ever built there. Reaching the bodies from a test needs a built struct
 * account, which is private to account.c and only exists behind a live socket
 * registration; an exported hook would reach them.
 */
static const char *evt_arg(unsigned int rec, unsigned int idx)
{
	const char *s = event_test_arg(rec, idx);

	return s ? s : "(none)";
}

/*
 * What reg_registered_cb() hands over: the configured next hop, never the
 * resolved address. The configured string keeps the field in one shape for
 * the cloud consumers, and it does not churn when the sticky selection moves.
 * acct_cfg_derive() owns the fallback chain that picks it, so the replica
 * reads the derived config and cannot desynchronize from a buffer size or a
 * lookup of its own.
 */
static void reg_event_up(unsigned int line, const struct acct_cfg *c)
{
	event_reg_post(line, true, c->resolve_host);
}

static void test_reg_events(void)
{
	struct cfg_channel ch;
	struct sip_dns_result res;
	struct acct_cfg c;

	base_default();

	CASE("reg event: the line index the edge passes is the derived 0 based one");
	event_test_mode(true);
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "5551000");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "sip.example.com");
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(c.line, 0);
	event_reg_post(c.line, false, NULL);
	CHECK(!strcmp(evt_arg(0, 4), "voip/nodeMAC/line-1/reg/failed"));
	CHECK(!strcmp(evt_arg(0, 2), "crit"));
	CHECK(!strcmp(evt_arg(0, 5), "{}"));

	CASE("reg event: the second channel publishes line-2");
	chan_default(&ch, 1);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "5551001");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "sip.example.com");
	acct_cfg_derive(&base, &ch, &c);
	CHECK_UINT(c.line, 1);
	event_reg_post(c.line, false, NULL);
	CHECK(!strcmp(evt_arg(1, 4), "voip/nodeMAC/line-2/reg/failed"));

	/*
	 * reg_failed_cb runs on every backoff attempt, and the dedup exists for
	 * that: one crit event per down transition, not one per attempt.
	 */
	CASE("reg event: a failed retry storm on one line posts one crit event");
	event_test_mode(true);
	event_reg_post(0, false, NULL);
	event_test_reap(0, 0);
	event_reg_post(0, false, NULL);
	event_reg_post(0, false, NULL);
	event_reg_post(0, false, NULL);
	CHECK_UINT(event_test_posts(), 1);
	CHECK_UINT(event_test_suppressed(), 3);

	CASE("reg event: the up edge carries the CONFIGURED proxy, not an address");
	event_test_mode(true);
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "5551000");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "sip.example.com");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!strcmp(c.resolve_host, "sip.example.com"));
	reg_event_up(c.line, &c);
	CHECK_UINT(event_test_posts(), 1);
	CHECK(!strcmp(evt_arg(0, 2), "info"));
	CHECK(!strcmp(evt_arg(0, 4), "voip/nodeMAC/line-1/reg/successful"));
	CHECK(!strcmp(evt_arg(0, 5), "{\"Proxy\":\"sip.example.com\"}"));

	/* the outbound proxy is the next hop, so it is what the event names */
	CASE("reg event: an outbound proxy overrides the published Proxy");
	event_test_mode(true);
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "5551000");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "sip.example.com");
	snprintf(ch.sip_outbound_proxy, sizeof(ch.sip_outbound_proxy), "sbc.example.net");
	acct_cfg_derive(&base, &ch, &c);
	reg_event_up(c.line, &c);
	CHECK(!strcmp(evt_arg(0, 5), "{\"Proxy\":\"sbc.example.net\"}"));

	/*
	 * A failover moves the sticky target to another address. The published
	 * Proxy is the configured name, so it does not move with it: that is the
	 * whole reason the configured string is published instead of the target.
	 */
	CASE("reg event: a failover does not change the published Proxy");
	event_test_mode(true);
	rk_reset();
	sip_reg_start(&rk_reg);
	answer_make(&res, DNS_A, DNS_B);
	acct_targets_load(&rk_reg, &res);
	CHECK(current_host_is(DNS_A));
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "5551000");
	snprintf(ch.sip_proxy, sizeof(ch.sip_proxy), "sip.example.com");
	acct_cfg_derive(&base, &ch, &c);
	reg_event_up(c.line, &c);
	event_test_reap(0, 0);
	CHECK(!strcmp(evt_arg(0, 5), "{\"Proxy\":\"sip.example.com\"}"));
	answer_make(&res, DNS_B, DNS_C);
	acct_targets_load(&rk_reg, &res);
	CHECK(current_host_is(DNS_B));
	reg_event_up(c.line, &c);
	CHECK_UINT(event_test_posts(), 1);
	CHECK_UINT(event_test_suppressed(), 1);
	CHECK(!strcmp(evt_arg(0, 5), "{\"Proxy\":\"sip.example.com\"}"));

	/*
	 * The reload path: Expires 0 completes (the unregistered edge), the
	 * account is rebuilt and registers again (the registered edge). Two
	 * events, in that order, and no repeat of either.
	 */
	CASE("reg event: an unregister and re-register cycle fires down then up");
	event_test_mode(true);
	reg_event_up(c.line, &c);
	event_test_reap(0, 0);
	event_reg_post(0, false, NULL);
	event_test_reap(0, 0);
	reg_event_up(c.line, &c);
	event_test_reap(0, 0);
	CHECK_UINT(event_test_posts(), 3);
	CHECK(!strcmp(evt_arg(0, 4), "voip/nodeMAC/line-1/reg/successful"));
	CHECK(!strcmp(evt_arg(1, 4), "voip/nodeMAC/line-1/reg/failed"));
	CHECK(!strcmp(evt_arg(2, 4), "voip/nodeMAC/line-1/reg/successful"));

	CASE("reg event: a shutdown before the first 200 still posts failed");
	event_test_mode(true);
	rk_reset();
	sip_reg_start(&rk_reg);
	event_reg_post(0, false, NULL);
	CHECK_UINT(event_test_posts(), 1);
	CHECK(!strcmp(evt_arg(0, 4), "voip/nodeMAC/line-1/reg/failed"));

	/* a channel with no proxy and no domain derives nothing to publish; it
	 * also derives enabled=false, so the edge cannot run, but the event must
	 * survive the empty string either way */
	CASE("reg event: an up edge with no configured proxy drops only the Proxy");
	event_test_mode(true);
	chan_default(&ch, 0);
	snprintf(ch.sip_username, sizeof(ch.sip_username), "5551000");
	acct_cfg_derive(&base, &ch, &c);
	CHECK(!c.resolve_host[0]);
	reg_event_up(c.line, &c);
	CHECK_UINT(event_test_posts(), 1);
	CHECK(!strcmp(evt_arg(0, 4), "voip/nodeMAC/line-1/reg/successful"));
	CHECK(!strcmp(evt_arg(0, 5), "{}"));

	CASE("reg event: the absent script refuses the edges instead of posting");
	event_test_mode(true);
	setenv("UVOICE_EVENT_POST", "/nonexistent/uvoice/event-post.sh", 1);
	event_init();
	event_reg_post(0, false, NULL);
	event_reg_post(1, true, DNS_A);
	CHECK_UINT(event_test_posts(), 0);
	CHECK_UINT(event_test_refused(), 2);
	unsetenv("UVOICE_EVENT_POST");
	event_test_mode(false);

	/*
	 * The three registration callbacks report their edge through
	 * acct_reg_state_post(), which serves two consumers: the event layer
	 * that publishes the move, and an armed call trace that wants a
	 * snapshot of the line at that moment. A trace reads the network table
	 * from that snapshot, so an edge that reached only the event layer
	 * would leave the archive without the reason the line moved.
	 */
	CASE("reg state: an up edge posts the event and asks for a snapshot");
	event_test_mode(true);
	calltrace_record_reset();
	acct_reg_state_post(0, true, DNS_A, true);
	CHECK_UINT(event_test_posts(), 1);
	CHECK(!strcmp(evt_arg(0, 4), "voip/nodeMAC/line-1/reg/successful"));
	CHECK_UINT(calltrace_events, 1);
	CHECK_UINT(calltrace_line, 0);
	CHECK(!strcmp(calltrace_reason, "registration"));

	CASE("reg state: a down edge of the second line names that line");
	event_test_mode(true);
	calltrace_record_reset();
	acct_reg_state_post(1, false, NULL, true);
	CHECK_UINT(event_test_posts(), 1);
	CHECK(!strcmp(evt_arg(0, 4), "voip/nodeMAC/line-2/reg/failed"));
	CHECK_UINT(calltrace_events, 1);
	CHECK_UINT(calltrace_line, 1);
	CHECK(!strcmp(calltrace_reason, "registration"));

	/*
	 * A registered answer arrives about every half expiry and reports the
	 * state the line already held. The reason word names a move, so a
	 * refresh must not carry it; the event layer reaches its own verdict
	 * on the same report and suppresses the repeat for its own reason.
	 */
	CASE("reg state: a repeated up edge asks for no snapshot");
	event_test_mode(true);
	acct_reg_state_post(0, true, DNS_A, true);
	calltrace_record_reset();
	acct_reg_state_post(0, true, DNS_A, false);
	CHECK_UINT(event_test_suppressed(), 1);
	CHECK_UINT(calltrace_events, 0);
	CHECK(!calltrace_reason[0]);
	event_test_mode(false);
}

/* ----------------------------------------------------- the pre-start window */

/*
 * The daemon connects to ubus first and starts the accounts after, because
 * accounts_start() needs the netifd answer to scope the first bind. A command
 * in that window must build nothing: accounts_start() runs next on the config
 * that main.c holds. A build here would repeat over the same struct and drop
 * the first socket.
 *
 * The observable is the firewall plan: a reload that reaches the accounts
 * ends in a reconcile, which rewrites the plan from the active state. The
 * first loaded set names a port and an interface that no config here
 * produces, so any reconcile is visible, whether it builds or clears. The
 * case is the last class of the pre-start window. The next class,
 * test_tls_cert_trigger(), calls accounts_start(), and no call clears the
 * vr.started flag that it sets.
 */
static void test_early_reload(void)
{
	static struct cfg new_cfg;
	struct fw_rtp_range rtp[1];
	unsigned int before;

	base_default();
	new_cfg = base;
	new_cfg.channels = 1;
	chan_default(&new_cfg.channel[0], 0);
	snprintf(new_cfg.channel[0].sip_username, sizeof(new_cfg.channel[0].sip_username), "u");
	snprintf(new_cfg.channel[0].sip_proxy, sizeof(new_cfg.channel[0].sip_proxy),
		 "p.example.com");

	memset(rtp, 0, sizeof(rtp));
	rtp[0].channel = 1;
	rtp[0].min = 30000;
	rtp[0].max = 30030;

	CASE("pre-start: a reload before the start reconciles nothing");
	fw_open_set("wan2", "5555", true, rtp, 1);
	before = fw_plan_count();
	CHECK(step_has(ADD(0), "-a Allow_VoIP_SIP -p 5555 -P udp -i wan2"));
	core_reload(&base, &new_cfg);
	CHECK_UINT(fw_plan_count(), before);
	CHECK(step_has(ADD(0), "-a Allow_VoIP_SIP -p 5555 -P udp -i wan2"));

	/* The ubus contract of the window is a refusal, never a crash. */
	CASE("pre-start: the register commands are refused");
	CHECK(!account_register_cmd(0));
	CHECK(!account_unregister_cmd(0));
}

static void test_tls_cert_trigger(void)
{
	static struct cfg tls_cfg;
	static struct cfg empty_cfg;
	static struct cfg udp_cfg;

	base_default();
	tls_cfg = base;
	tls_cfg.enabled = false;
	tls_cfg.channels = 0;
	tls_cfg.tls_server_verification_enabled = true;
	snprintf(tls_cfg.transport, sizeof(tls_cfg.transport), "tls");
	snprintf(tls_cfg.tls_cert_path, sizeof(tls_cfg.tls_cert_path),
		 "https://cert.example/voice.tgz");
	snprintf(tls_cfg.tls_cert_username, sizeof(tls_cfg.tls_cert_username), "alice");
	snprintf(tls_cfg.tls_cert_password, sizeof(tls_cfg.tls_cert_password), "secret");

	event_test_mode(true);
	fw_test_mode(true);

	CASE("tls cert trigger: a TLS path starts the helper with the base credentials");
	core_start(&tls_cfg);
	CHECK(event_test_tls_armed());
	CHECK(event_test_tls_busy());
	CHECK_UINT(event_test_tls_starts(), 1);
	CHECK(!strcmp(event_test_tls_arg(1), "https://cert.example/voice.tgz"));
	CHECK(!strcmp(event_test_tls_arg(2), "alice"));
	CHECK(!strcmp(event_test_tls_arg(3), "secret"));
	CHECK(!strcmp(event_test_tls_arg(4), "1"));

	CASE("tls cert trigger: an empty TLS path still starts the refusal helper");
	empty_cfg = tls_cfg;
	empty_cfg.tls_cert_path[0] = 0;
	core_reload(&tls_cfg, &empty_cfg);
	event_test_tls_reap(0);
	CHECK(event_test_tls_armed());
	CHECK(event_test_tls_busy());
	CHECK_UINT(event_test_tls_starts(), 2);
	CHECK(!strcmp(event_test_tls_arg(1), ""));

	CASE("tls cert trigger: a non-TLS reload serializes the clear helper");
	udp_cfg = empty_cfg;
	snprintf(udp_cfg.transport, sizeof(udp_cfg.transport), "udp");
	core_reload(&empty_cfg, &udp_cfg);
	CHECK(!event_test_tls_armed());
	CHECK(!event_test_tls_retry_pending());
	event_test_tls_reap(4);
	CHECK(event_test_tls_busy());
	CHECK(!strcmp(event_test_tls_arg(1), "clear"));
	event_test_tls_reap(0);
	CHECK(!event_test_tls_busy());
}

/* ------------------------------------------ the netifd IPv6 address tracker */

/*
 * The global IPv6 address of the voice netdev. It comes from netifd, from the
 * voice interface's own status answer or from the dump of every interface, and
 * it drives two things: the address6 key of "voice status" and the family the
 * resolver delivers first.
 *
 * The tracker is driven through the three inject twins of account.h, because
 * the suite runs no netifd on ubus: no query of a live line is ever answered
 * and no interface event is ever delivered (acct_netifd_status_inject).
 *
 * The voice interface is "wan", the name base_default() configures, and the
 * netdevs below are names no host carries: nothing here binds a socket, and a
 * name the kernel knew would scope one.
 */

#define V6_IFACE	"wan"
#define V6_DEV		"uvoice-v6dev0"
#define V6_DEV2		"uvoice-v6dev1"
#define V6_DEV3		"uvoice-v6dev2"
#define V6_OTHERDEV	"uvoice-v6dev9"

/* the two arguments every builder below takes, from one array */
#define ROWS(a)		(a), (unsigned int)(sizeof(a) / sizeof((a)[0]))

/*
 * One entry of a netifd ipv6-address array. netifd writes the address, its
 * mask, an optional preferred lifetime and an optional valid one; a preferred
 * lifetime below 0 here leaves the key out, which is netifd's "never expires".
 */
struct addr6_row {
	const char *addr;
	int preferred;
};

static void addr6_array_add(struct blob_buf *b, const struct addr6_row *row,
			    unsigned int count)
{
	void *arr = blobmsg_open_array(b, "ipv6-address");
	unsigned int i;

	for (i = 0; i < count; i++) {
		void *tbl = blobmsg_open_table(b, NULL);

		blobmsg_add_string(b, "address", row[i].addr);
		blobmsg_add_u32(b, "mask", 64);
		if (row[i].preferred >= 0)
			blobmsg_add_u32(b, "preferred",
					(uint32_t)row[i].preferred);
		blobmsg_add_u32(b, "valid", 7200);
		blobmsg_close_table(b, tbl);
	}

	blobmsg_close_array(b, arr);
}

/*
 * The status answer of the voice interface: its netdev, one IPv4 address when
 * the caller names one, and its IPv6 addresses. A NULL netdev leaves the key
 * out, which is what netifd answers for an interface that is not up. up writes
 * the "up" flag netifd carries; false leaves the key out, which is the answer
 * shape every case predating the flag drives.
 */
static void status_inject(const char *l3_device, const char *addr4,
			  const struct addr6_row *row, unsigned int count,
			  bool up)
{
	struct blob_buf b;

	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	if (l3_device)
		blobmsg_add_string(&b, "l3_device", l3_device);

	if (up)
		blobmsg_add_u8(&b, "up", 1);

	if (addr4) {
		void *arr = blobmsg_open_array(&b, "ipv4-address");
		void *tbl = blobmsg_open_table(&b, NULL);

		blobmsg_add_string(&b, "address", addr4);
		blobmsg_add_u32(&b, "mask", 24);
		blobmsg_close_table(&b, tbl);
		blobmsg_close_array(&b, arr);
	}

	addr6_array_add(&b, row, count);
	net_iface_test_status_inject(b.head);
	blob_buf_free(&b);
}

static void status46_inject(const char *l3_device, const char *addr4,
			    const struct addr6_row *row, unsigned int count)
{
	status_inject(l3_device, addr4, row, count, false);
}

/* the same answer with the "up" flag, which is what an interface that came
 * back reports and what the IPv6 half of the down flag reads */
static void status46_up_inject(const char *l3_device, const char *addr4,
			       const struct addr6_row *row, unsigned int count)
{
	status_inject(l3_device, addr4, row, count, true);
}

/* the same answer without an IPv4 address, which is what most cases drive */
static void status6_inject(const char *l3_device, const struct addr6_row *row,
			   unsigned int count)
{
	status46_inject(l3_device, NULL, row, count);
}

/* one interface of a dump answer, which is the whole shape the reader walks:
 * the name and the up flag in front of an ordinary status table */
struct dump_row {
	const char *iface;
	bool up;
	const char *l3_device;
	const struct addr6_row *addr;
	unsigned int count;
};

static void dump_inject(const struct dump_row *row, unsigned int count)
{
	struct blob_buf b;
	void *arr;
	unsigned int i;

	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	arr = blobmsg_open_array(&b, "interface");

	for (i = 0; i < count; i++) {
		void *tbl = blobmsg_open_table(&b, NULL);

		blobmsg_add_string(&b, "interface", row[i].iface);
		blobmsg_add_u8(&b, "up", row[i].up);
		if (row[i].l3_device)
			blobmsg_add_string(&b, "l3_device", row[i].l3_device);
		addr6_array_add(&b, row[i].addr, row[i].count);
		blobmsg_close_table(&b, tbl);
	}

	blobmsg_close_array(&b, arr);
	net_iface_test_dump_inject(b.head);
	blob_buf_free(&b);
}

/* the same answer with one interface in it, which is what most cases drive */
static void dump6_inject(const char *iface, bool up, const char *l3_device,
			 const struct addr6_row *row, unsigned int count)
{
	struct dump_row one[] = { { iface, up, l3_device, row, count } };

	dump_inject(one, 1);
}

static void event6_inject(const char *iface, const char *action)
{
	struct blob_buf b;

	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "interface", iface);
	blobmsg_add_string(&b, "action", action);
	net_iface_test_event_inject(b.head);
	blob_buf_free(&b);
}

/*
 * One string key of the status network table, copied into the caller's buffer,
 * or NULL when the table or the key is absent. The dump is built and freed
 * here, so the value has to leave in a buffer of the caller's.
 */
static const char *net_key(const char *key, char *out, size_t olen)
{
	struct blob_buf b;
	struct blob_attr *tbl;
	struct blob_attr *cur;
	const char *got = NULL;
	size_t rem;

	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	net_iface_status_dump(&b);

	tbl = dump_table(&b, "network");
	if (!tbl) {
		blob_buf_free(&b);

		return NULL;
	}

	blobmsg_for_each_attr(cur, tbl, rem) {
		if (strcmp(blobmsg_name(cur), key))
			continue;
		if (blobmsg_type(cur) != BLOBMSG_TYPE_STRING)
			continue;

		snprintf(out, olen, "%s", blobmsg_get_string(cur));
		got = out;
	}

	blob_buf_free(&b);

	return got;
}

static bool net_key_is(const char *key, const char *want)
{
	char buf[64];
	const char *got = net_key(key, buf, sizeof(buf));

	return got && !strcmp(got, want);
}

static bool net_key_gone(const char *key)
{
	char buf[64];

	return net_key(key, buf, sizeof(buf)) == NULL;
}

static void test_netifd_addr6(void)
{
	static const struct addr6_row gua[] = { { "2001:db8:10::6", 3600 } };
	static const struct addr6_row forever[] = { { "2001:db8:10::5", -1 } };
	static const struct addr6_row deprecated[] = {
		{ "2001:db8:10::dead", 0 },
		{ "2001:db8:10::7", 1800 },
	};
	static const struct addr6_row no_source[] = {
		{ "fe80::1", -1 },
		{ "ff02::1:2", -1 },
		{ "::", -1 },
		{ "::1", -1 },
		{ "::ffff:192.0.2.1", -1 },
	};
	static const struct addr6_row no_source_then[] = {
		{ "fe80::1", -1 },
		{ "ff02::1:2", -1 },
		{ "::", -1 },
		{ "::1", -1 },
		{ "::ffff:192.0.2.1", -1 },
		{ "2001:db8:10::8", -1 },
	};
	static const struct addr6_row ula_first[] = {
		{ "fd00:5150::2", -1 },
		{ "2001:db8:10::9", -1 },
	};
	static const struct addr6_row gua_first[] = {
		{ "2001:db8:10::a", -1 },
		{ "fd00:5150::3", -1 },
	};
	static const struct addr6_row ula_only[] = { { "fd00:5150::4", -1 } };
	static const struct addr6_row junk[] = {
		{ "not-an-address", -1 },
		{ "192.0.2.10", -1 },
	};
	static const struct addr6_row junk_then[] = {
		{ "not-an-address", -1 },
		{ "2001:db8:10::b", -1 },
	};
	static const struct addr6_row other[] = { { "2001:db8:20::5", -1 } };
	static const struct addr6_row other2[] = { { "2001:db8:20::6", -1 } };
	static const struct addr6_row other3[] = { { "2001:db8:20::7", -1 } };
	static const struct addr6_row moved6[] = { { "2001:db8:20::8", -1 } };
	static const struct addr6_row dead6[] = { { "2001:db8:20::9", 0 } };

	/* two interfaces of the netdev with an address each */
	static const struct dump_row both[] = {
		{ "wan6", true, V6_DEV, other, 1 },
		{ "wan7", true, V6_DEV, other2, 1 },
	};
	/* the owner's entry sits behind another eligible one */
	static const struct dump_row ahead[] = {
		{ "wan7", true, V6_DEV, other2, 1 },
		{ "wan6", true, V6_DEV, moved6, 1 },
	};
	/* the owner is up and carries nothing a line can send from */
	static const struct dump_row stale[] = {
		{ "wan6", true, V6_DEV, dead6, 1 },
	};
	/* the owner is not in the answer, and nothing else qualifies */
	static const struct dump_row absent[] = {
		{ "wan8", true, V6_OTHERDEV, other2, 1 },
	};
	/* the owner gives up and another interface of the netdev takes over */
	static const struct dump_row handover[] = {
		{ "wan6", true, V6_DEV, dead6, 1 },
		{ "wan7", true, V6_DEV, other2, 1 },
	};
	static struct cfg live;
	static struct cfg moved;

	base_default();
	live = base;
	live.enabled = false;
	live.channels = 0;

	event_test_mode(true);
	fw_test_mode(true);
	core_start(&live);

	CASE("addr6: a global address of the voice interface is taken");
	status6_inject(V6_DEV, ROWS(gua));
	CHECK(net_key_is("address6", "2001:db8:10::6"));

	/* RFC 6724: a deprecated address is no source address, and netifd
	 * reports the deprecation as a preferred lifetime of 0 */
	CASE("addr6: a deprecated address is skipped and the next one taken");
	status6_inject(V6_DEV, ROWS(deprecated));
	CHECK(net_key_is("address6", "2001:db8:10::7"));

	/* an absent preferred lifetime is netifd's "never expires", not a 0 */
	CASE("addr6: an address with no preferred lifetime is taken");
	status6_inject(V6_DEV, ROWS(forever));
	CHECK(net_key_is("address6", "2001:db8:10::5"));

	/* RFC 4291: none of these is a source address, and the IPv4-mapped form
	 * of section 2.5.5.2 is no exception although inet_pton() parses it */
	CASE("addr6: link-local, multicast, ::, ::1 and v4-mapped are refused");
	status6_inject(V6_DEV, ROWS(no_source));
	CHECK(net_key_gone("address6"));

	/* the walk passes them rather than ending at the first refusal */
	CASE("addr6: and a global address behind them is still found");
	status6_inject(V6_DEV, ROWS(no_source_then));
	CHECK(net_key_is("address6", "2001:db8:10::8"));

	CASE("addr6: a global unicast address wins over a unique local one");
	status6_inject(V6_DEV, ROWS(ula_first));
	CHECK(net_key_is("address6", "2001:db8:10::9"));
	status6_inject(V6_DEV, ROWS(gua_first));
	CHECK(net_key_is("address6", "2001:db8:10::a"));

	/* a lab and a carrier deployment both run on a unique local prefix */
	CASE("addr6: a unique local address alone is taken");
	status6_inject(V6_DEV, ROWS(ula_only));
	CHECK(net_key_is("address6", "fd00:5150::4"));

	CASE("addr6: a text that is no IPv6 literal is refused");
	status6_inject(V6_DEV, ROWS(junk));
	CHECK(net_key_gone("address6"));
	status6_inject(V6_DEV, ROWS(junk_then));
	CHECK(net_key_is("address6", "2001:db8:10::b"));

	/* the voice interface gives its address up, so the record is free */
	status6_inject(V6_DEV, NULL, 0);

	CASE("dump: an interface on another netdev carries no address of ours");
	CHECK(net_key_gone("address6"));
	dump6_inject("wan6", true, V6_OTHERDEV, ROWS(other));
	CHECK(net_key_gone("address6"));

	CASE("dump: an interface that is not up carries none either");
	dump6_inject("wan6", false, V6_DEV, ROWS(other));
	CHECK(net_key_gone("address6"));

	/* the address of the voice interface arrives through its own status
	 * answer, so its dump entry has nothing to add */
	CASE("dump: the voice interface's own entry is skipped");
	dump6_inject(V6_IFACE, true, V6_DEV, ROWS(other));
	CHECK(net_key_gone("address6"));

	CASE("dump: an interface of the voice netdev carries the address");
	dump6_inject("wan6", true, V6_DEV, ROWS(other));
	CHECK(net_key_is("address6", "2001:db8:20::5"));

	/* both interfaces of the netdev carry an address in one answer, which
	 * is the shape netifd sends: the one that holds the record keeps it */
	CASE("owner: a second interface does not displace the first");
	dump_inject(ROWS(both));
	CHECK(net_key_is("address6", "2001:db8:20::5"));

	CASE("owner: another interface's ifdown leaves the address");
	event6_inject("wan7", "ifdown");
	CHECK(net_key_is("address6", "2001:db8:20::5"));

	CASE("owner: the ifdown of the interface that holds it takes it away");
	event6_inject("wan6", "ifdown");
	CHECK(net_key_gone("address6"));

	CASE("owner: the voice interface gives up the address it holds itself");
	status6_inject(V6_DEV, ROWS(gua));
	CHECK(net_key_is("address6", "2001:db8:10::6"));
	event6_inject(V6_IFACE, "ifdown");
	CHECK(net_key_gone("address6"));

	/*
	 * The IPv4 side of a WAN can go down while the interface that holds the
	 * netdev's IPv6 address stays up, so an ifdown asks the dump again
	 * wherever it came from. The inject below is the answer to that query.
	 */
	CASE("owner: after the voice ifdown the dump finds the other address");
	status6_inject(V6_DEV, ROWS(gua));
	CHECK(net_key_is("address6", "2001:db8:10::6"));
	event6_inject(V6_IFACE, "ifdown");
	CHECK(net_key_gone("address6"));
	dump6_inject("wan6", true, V6_DEV, ROWS(other));
	CHECK(net_key_is("address6", "2001:db8:20::5"));

	/*
	 * The answer is the state of the moment, so the entry of the interface
	 * that holds the record decides that record before any other entry is
	 * read. Here its address moved while another eligible entry sits ahead
	 * of it in the list.
	 */
	CASE("dump: an entry ahead of the owner hides no move of the owner");
	dump_inject(ROWS(ahead));
	CHECK(net_key_is("address6", "2001:db8:20::8"));

	/* the address is up and deprecated, so no line can send from it */
	CASE("dump: an owner entry with no usable address gives it up");
	dump_inject(ROWS(stale));
	CHECK(net_key_gone("address6"));

	CASE("dump: an owner missing from the answer gives it up");
	dump6_inject("wan6", true, V6_DEV, ROWS(other));
	CHECK(net_key_is("address6", "2001:db8:20::5"));
	dump_inject(ROWS(absent));
	CHECK(net_key_gone("address6"));

	CASE("dump: with the owner gone the next entry's address is taken");
	dump6_inject("wan6", true, V6_DEV, ROWS(other));
	CHECK(net_key_is("address6", "2001:db8:20::5"));
	dump_inject(ROWS(handover));
	CHECK(net_key_is("address6", "2001:db8:20::6"));

	/* the cases below start from an empty record */
	event6_inject("wan7", "ifdown");

	CASE("device: a netdev move gives the address up, the dump finds it");
	dump6_inject("wan6", true, V6_DEV, ROWS(other));
	CHECK(net_key_is("address6", "2001:db8:20::5"));
	status6_inject(V6_DEV2, NULL, 0);
	CHECK(net_key_gone("address6"));
	dump6_inject("wan6", true, V6_DEV2, ROWS(other2));
	CHECK(net_key_is("address6", "2001:db8:20::6"));

	/*
	 * The other event order. The dump answer of the new netdev arrives
	 * before the status answer that names it, so the interface that holds
	 * the record is on a netdev this daemon does not know yet: the record
	 * goes and the new address is not taken. The status answer runs the
	 * dump query again for exactly this reason, and the second inject is
	 * the answer to that query.
	 */
	CASE("device: a dump before the netdev move is asked again");
	dump6_inject("wan6", true, V6_DEV3, ROWS(other3));
	CHECK(net_key_gone("address6"));
	status6_inject(V6_DEV3, NULL, 0);
	CHECK(net_key_gone("address6"));
	dump6_inject("wan6", true, V6_DEV3, ROWS(other3));
	CHECK(net_key_is("address6", "2001:db8:20::7"));

	CASE("status: address6 stands beside the netdev and leaves without it");
	CHECK(net_key_is("l3_device", V6_DEV3));
	CHECK(net_key_is("address6", "2001:db8:20::7"));
	event6_inject("wan6", "ifdown");
	CHECK(net_key_gone("address6"));
	CHECK(net_key_is("l3_device", V6_DEV3));

	/*
	 * Both families of one answer, which is the shipped case: the IPv4
	 * address feeds the Via and the Contact of every line today and the
	 * IPv6 one is what the resolver preference reads.
	 */
	CASE("status: the IPv4 and the IPv6 address stand side by side");
	status46_inject(V6_DEV3, "192.0.2.55", ROWS(other3));
	CHECK(net_key_is("address", "192.0.2.55"));
	CHECK(net_key_is("address6", "2001:db8:20::7"));
	CHECK(net_key_is("l3_device", V6_DEV3));

	CASE("reload: a new voice interface gives up address and netdev");
	moved = live;
	snprintf(moved.local_interface, sizeof(moved.local_interface), "wan2");
	core_reload(&live, &moved);
	CHECK(net_key_gone("address"));
	CHECK(net_key_gone("address6"));
	CHECK(net_key_gone("l3_device"));

	/*
	 * The whole table is omitted while nothing is known, and the IPv6
	 * address alone opens it: a voice interface that runs IPv6 and no IPv4
	 * has that address and nothing else to report.
	 */
	CASE("status: the IPv6 address alone opens the network table");
	status6_inject(NULL, ROWS(gua));
	CHECK(net_key_is("address6", "2001:db8:10::6"));
	CHECK(net_key_gone("address"));
	CHECK(net_key_gone("l3_device"));

	/* nothing of this class reaches the ones below it */
	status6_inject(NULL, NULL, 0);
}

/* ------------------------------------------ the resolver's two live inputs */

/*
 * The AAAA questions follow the ipv6_enabled operator gate. The family that
 * the resolver delivers first follows the address of the voice netdev. The
 * resolver reads both when it delivers an answer (shared/resolve.h). This layer
 * therefore pushes them wherever either half changes: at a start, at a
 * reload, and on every address that arrives or leaves.
 *
 * Neither input reaches the status, so the resolver itself is the observable
 * (account.h acct_dns).
 */
static void test_ipv6_gate(void)
{
	static const struct addr6_row gua[] = { { "2001:db8:30::5", -1 } };
	static struct cfg off;
	static struct cfg on;

	base_default();
	off = base;
	off.enabled = false;
	off.channels = 0;
	on = off;
	on.ipv6_enabled = true;

	event_test_mode(true);
	fw_test_mode(true);

	/* The third half of the preference: the media of a call runs in the
	 * family of its signalling, so the backend has to carry IPv6 media
	 * before a target of that family may go first (RFC 6157 section 5). */
	accounts_hal_features_set(HAL_FEAT_IPV6_MEDIA);

	CASE("gate: ipv6_enabled 1 asks AAAA, IPv4 first with no address");
	core_start(&on);
	CHECK(acct_dns()->cfg.ipv6);
	CHECK_UINT(sip_dns_prefer_family(acct_dns()), AF_INET);

	CASE("gate: an address puts the IPv6 targets first");
	status6_inject(V6_DEV, ROWS(gua));
	CHECK(net_key_is("address6", "2001:db8:30::5"));
	CHECK_UINT(sip_dns_prefer_family(acct_dns()), AF_INET6);

	CASE("gate: a backend that carries no IPv6 media keeps IPv4 in front");
	accounts_hal_features_set(0);
	CHECK_UINT(sip_dns_prefer_family(acct_dns()), AF_INET);
	CHECK(net_key_is("address6", "2001:db8:30::5"));

	CASE("gate: the backend that carries it puts IPv6 back in front");
	accounts_hal_features_set(HAL_FEAT_IPV6_MEDIA);
	CHECK_UINT(sip_dns_prefer_family(acct_dns()), AF_INET6);

	/* The reload puts the preference where the address says, so the
	 * departure below is observed on its own and not on the push the case
	 * above made. */
	CASE("gate: an address that leaves puts IPv4 back in front");
	core_reload(&on, &on);
	CHECK_UINT(sip_dns_prefer_family(acct_dns()), AF_INET6);
	status6_inject(V6_DEV, NULL, 0);
	CHECK(net_key_gone("address6"));
	CHECK_UINT(sip_dns_prefer_family(acct_dns()), AF_INET);

	/* the address is reported whatever ipv6_enabled says: it is what the
	 * netdev holds, and the gate alone says what uvoice does with it */
	CASE("gate: ipv6_enabled 0 reports the address and asks no AAAA");
	core_start(&off);
	CHECK(!acct_dns()->cfg.ipv6);
	status6_inject(V6_DEV, ROWS(gua));
	CHECK(net_key_is("address6", "2001:db8:30::5"));
	CHECK(!acct_dns()->cfg.ipv6);
	CHECK_UINT(sip_dns_prefer_family(acct_dns()), AF_INET);

	CASE("reload: ipv6_enabled turned on asks AAAA and takes the address");
	core_reload(&off, &on);
	CHECK(acct_dns()->cfg.ipv6);
	CHECK_UINT(sip_dns_prefer_family(acct_dns()), AF_INET6);

	CASE("reload: ipv6_enabled turned off stops asking and puts IPv4 back");
	core_reload(&on, &off);
	CHECK(!acct_dns()->cfg.ipv6);
	CHECK_UINT(sip_dns_prefer_family(acct_dns()), AF_INET);

	/* nothing of this class reaches the ones below it */
	status6_inject(V6_DEV, NULL, 0);
	accounts_hal_features_set(0);
}

/* ------------------------------------- the grace timer across a reload pair */

/*
 * A live line class. Everything above drives the pure helpers, the reload
 * dispatch or a scripted registration with no socket underneath. This class
 * starts real accounts through accounts_start(), lets a fake registrar answer
 * over loopback, and drives the two reloads that would zero an armed grace
 * timer inside uloop's timeout list if account_teardown() did not cancel it.
 *
 * It needs no ubus and no HAL. netifd_query() returns at once without a ubus
 * context, accounts_init() builds only the transport, the TLS holder, the
 * stream pool and the resolver, and account.c names no hal_ symbol anywhere.
 * The event publisher and the firewall run in their own no-spawn test modes,
 * and the proxy is a literal, so the resolver delivers it without a lookup.
 *
 * uloop is driven one pass at a time. uloop_run_timeout() with a non-negative
 * argument runs one timeout phase and one event phase and then returns (its
 * do-while ends unless the argument is negative), which is what makes the
 * window observable at all: the unregister answer arms the grace timer at 0
 * during an event phase, and the pass returns before the timeout phase that
 * would fire it.
 *
 * The observable is the head of the timeout list, read without walking it.
 * uloop_get_next_timeout() looks at the first entry alone and answers 0 when it
 * is due, so a zeroed node, whose time is {0,0} and therefore always due, reads
 * 0 while a healthy list reads a real remaining time. A case that ran the walk
 * instead would not fail on the defect, it would hang inside it.
 */

static int registrar_fd = -1;
static struct uloop_fd registrar_ufd;
static unsigned int registrar_answers;

static uint16_t udp_loopback_open(int *fd_out)
{
	struct sockaddr_in sa;
	socklen_t len = sizeof(sa);
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return 0;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (bind(fd, (const struct sockaddr *)&sa, sizeof(sa)) < 0 ||
	    getsockname(fd, (struct sockaddr *)&sa, &len) < 0) {
		close(fd);
		return 0;
	}

	*fd_out = fd;

	return ntohs(sa.sin_port);
}

/* one header line of the request, its name included, without the CRLF */
static bool req_line_get(const char *msg, const char *name, char *out, size_t olen)
{
	char pat[32];
	const char *start;
	const char *end;
	size_t len;

	snprintf(pat, sizeof(pat), "\r\n%s:", name);
	start = strstr(msg, pat);
	if (!start)
		return false;

	start += 2;
	end = strstr(start, "\r\n");
	if (!end)
		return false;

	len = (size_t)(end - start);
	if (len >= olen)
		return false;

	memcpy(out, start, len);
	out[len] = 0;

	return true;
}

/*
 * Every REGISTER is answered 200 OK, and the answer needs no SIP stack: the
 * transaction is matched by the Via branch it carries back, and the expiry the
 * registration reads comes from a Contact of its own, an Expires header or,
 * failing both, what the request asked for (proto/sip/reg.c accepted_expires).
 * So the answer is the request's own Via, From, To, Call-ID and CSeq, a To tag,
 * and nothing else. The un-REGISTER is answered the same way and completes the
 * unregistering state whatever expiry it names.
 */
/* the last request a fake registrar answered, for the cases that read the
 * headers it carried */
static char registrar_last[2048];

/*
 * The answer of one fake registrar, built from the text of the request alone
 * and sent to the peer the caller names. rport nonzero adds the RFC 3581 pair
 * to the Via that goes back, which is how a registrar reports the mapping it
 * sees; the request's own ";rport" is cut first, so the answer carries the
 * parameter once.
 *
 * The request is a parameter, so a case can also answer a request it saved
 * earlier, which is how the late answer of an exchange the daemon gave up on
 * reaches it (fam_stray_answer).
 *
 * status is the status line after "SIP/2.0 " and extra is one header line, or
 * NULL for none: a case that needs a refusal with a delay the server names
 * (RFC 3261 21.5.4) answers through the same builder as every success.
 */
static bool registrar_reply(int fd, const char *req, unsigned int rport,
			    const struct sockaddr *peer, socklen_t plen,
			    const char *status, const char *extra)
{
	char resp[2048];
	char via[640];
	char from[256];
	char to[256];
	char callid[256];
	char cseq[64];
	char *tail;

	if (!req_line_get(req, "Via", via, sizeof(via)) ||
	    !req_line_get(req, "From", from, sizeof(from)) ||
	    !req_line_get(req, "To", to, sizeof(to)) ||
	    !req_line_get(req, "Call-ID", callid, sizeof(callid)) ||
	    !req_line_get(req, "CSeq", cseq, sizeof(cseq)))
		return false;

	tail = rport ? strstr(via, ";rport") : NULL;
	if (tail) {
		*tail = 0;
		snprintf(via + strlen(via), sizeof(via) - strlen(via),
			 ";received=127.0.0.1;rport=%u", rport);
	}

	snprintf(resp, sizeof(resp),
		 "SIP/2.0 %s\r\n%s\r\n%s\r\n%s;tag=graceregistrar\r\n%s\r\n%s\r\n"
		 "%s%sContent-Length: 0\r\n\r\n", status, via, from, to, callid,
		 cseq, extra ? extra : "", extra ? "\r\n" : "");

	return sendto(fd, resp, strlen(resp), 0, peer, plen) > 0;
}

/*
 * One header line every fake registrar adds to the answers it builds, without
 * its CRLF, or NULL for none. It is a global on the fam_rport precedent: the
 * answer path is shared by every class, and a case that needs a granted
 * Service-Route sets it for the window it owns and clears it after.
 */
static const char *registrar_extra;

/* the request one fake registrar has waiting, read and answered */
static bool registrar_answer(int fd, unsigned int rport)
{
	struct sockaddr_storage peer;
	socklen_t plen = sizeof(peer);
	ssize_t n;

	n = recvfrom(fd, registrar_last, sizeof(registrar_last) - 1, 0,
		     (struct sockaddr *)&peer, &plen);
	if (n <= 0)
		return false;

	registrar_last[n] = 0;

	return registrar_reply(fd, registrar_last, rport,
			       (const struct sockaddr *)&peer, plen, "200 OK",
			       registrar_extra);
}

static void registrar_cb(struct uloop_fd *u, unsigned int events)
{
	if (registrar_answer(u->fd, 0))
		registrar_answers++;
}

/* passes until the publisher recorded the edge, or until the bound gives up */
static bool posts_wait(unsigned int want)
{
	unsigned int i;

	for (i = 0; i < 400; i++) {
		if (event_test_posts() >= want)
			return true;

		uloop_run_timeout(20);
	}

	return event_test_posts() >= want;
}

/*
 * What the public status dump says about line 0: how many lines it carries, and
 * the state string of the first one when the caller wants it. vr is private to
 * account.c, so the dump is the only reachable answer to "is the line still
 * there" and "did its registration restart" (accounts_status_dump).
 */
static unsigned int status_line0(char *state, size_t slen)
{
	struct blob_buf b;
	struct blob_attr *cur;
	struct blob_attr *arr = NULL;
	struct blob_attr *entry;
	struct blob_attr *field;
	size_t rem;
	unsigned int n = 0;

	if (state)
		snprintf(state, slen, "%s", "(none)");

	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	accounts_status_dump(&b, -1);

	blob_for_each_attr(cur, b.head, rem) {
		if (!strcmp(blobmsg_name(cur), "lines"))
			arr = cur;
	}

	if (arr) {
		size_t left;

		blobmsg_for_each_attr(entry, arr, left) {
			size_t inner;

			n++;
			if (!state || n > 1)
				continue;

			blobmsg_for_each_attr(field, entry, inner) {
				if (!strcmp(blobmsg_name(field), "state"))
					snprintf(state, slen, "%s",
						 blobmsg_get_string(field));
			}
		}
	}

	blob_buf_free(&b);

	return n;
}

/*
 * Whether a datagram is waiting at the registrar, read without a uloop pass.
 * The window's timer is armed at 0, so a pass would fire it and close the
 * window; a peek answers "did anything leave the daemon" on the spot.
 */
static bool registrar_pending(void)
{
	char peek[64];

	return recv(registrar_fd, peek, sizeof(peek), MSG_PEEK | MSG_DONTWAIT) > 0;
}

/*
 * The window itself. Both shapes below open it the same way. The caller
 * hands over a line that is active with its registration in flight, and has
 * reset the publisher so the two windows count alike. On return the line has
 * finished its Expires 0, its expiry is 0, and its grace timer is armed, linked
 * and unfired.
 */
static bool grace_window_arm(const struct cfg *cur, const struct cfg *flip)
{
	if (!posts_wait(1))
		return false;

	/* the publisher runs one post at a time and this fixture is its reap:
	 * without it the unregistered edge is only queued, and the wait for it
	 * would run the timeout pass that closes the window */
	event_test_reap(0, 0);
	core_reload(cur, flip);
	if (!posts_wait(2))
		return false;

	return uloop_get_next_timeout() == 0;
}

/* true when the child exits 0 within 2 s; a child still alive then is killed */
static bool child_wait(pid_t pid)
{
	unsigned int i;
	int status;

	if (pid < 0)
		return false;

	for (i = 0; i < 200; i++) {
		if (waitpid(pid, &status, WNOHANG) == pid)
			return WIFEXITED(status) && WEXITSTATUS(status) == 0;

		usleep(10000);
	}

	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);

	return false;
}

/*
 * A reload and the pass behind it, run in a child with the parent as its
 * bound. Both consequences of the defect are fatal to the process that meets
 * them: an insertion that walks into the zeroed node dereferences a NULL link,
 * and a walk that reaches it never returns. Neither can run in the process that
 * has to report the result, so the child does the work and exits, and a child
 * that crashed, was killed, or is still alive at the deadline is the defect.
 *
 * The parent keeps its own copy of every object, so the window it armed is
 * still armed when the child is gone: the two shapes below are asserted
 * against the same window, and neither can hide the other.
 *
 * A NULL from runs the pass alone, with no reload ahead of it.
 */
static bool child_pass_returns(const struct cfg *from, const struct cfg *to)
{
	pid_t pid = fork();

	if (pid == 0) {
		if (from)
			core_reload(from, to);

		uloop_run_timeout(0);
		_exit(0);
	}

	return child_wait(pid);
}

/* --------------------------------------- the family a live line signals in */

/*
 * A line takes the address family of its registrar target and moves its socket,
 * its local host, its Via, its Contact and its media with it: one line signals
 * in one family at a time, and its media follows its signalling (RFC 6157
 * section 5). The classes below bring real lines up on loopback registrars of
 * both families and move them.
 *
 * The moves are driven with acct_dns_result_inject(), because an answer that
 * carries two families needs a zone and the suite has none: the injected answer
 * is the shape sip_dns_resolve() delivers and every stage behind it is
 * production code. A case that reads its observable before a uloop pass reads
 * it while the answer of the line's own literal query still waits on the
 * delivery timer, the way the grace class reads its window.
 *
 * Every case that opens an IPv6 socket needs an IPv6 loopback and skips by name
 * without one, which is what a run in a network namespace with IPv6 loopback
 * exists for.
 */

static int fam_fd4 = -1;
static int fam_fd6 = -1;
static struct uloop_fd fam_ufd4;
static struct uloop_fd fam_ufd6;
static unsigned int fam_regs4;
static unsigned int fam_regs6;

/* nonzero: the IPv4 registrar reports the mapping it sees (RFC 3581) */
static unsigned int fam_rport;

static bool fam_last_is_register(void)
{
	return !strncmp(registrar_last, "REGISTER ", 9);
}

/* REGISTERs with Expires 0 the IPv4 registrar answered: the removal of a
 * server move is one, and so is the unregister of a stop */
static unsigned int fam_removals4;

static void fam_cb4(struct uloop_fd *u, unsigned int events)
{
	if (!registrar_answer(u->fd, fam_rport) || !fam_last_is_register())
		return;

	fam_regs4++;
	if (strstr(registrar_last, "\r\nExpires: 0\r\n"))
		fam_removals4++;
}

/* a second IPv4 registrar with its own count, the other member of a pool */
static struct uloop_fd fam_ufd4b;
static unsigned int fam_regs4b;

static void fam_cb4b(struct uloop_fd *u, unsigned int events)
{
	if (registrar_answer(u->fd, 0) && fam_last_is_register())
		fam_regs4b++;
}

static void fam_cb6(struct uloop_fd *u, unsigned int events)
{
	if (registrar_answer(u->fd, 0) && fam_last_is_register())
		fam_regs6++;
}

static uint16_t udp6_loopback_open(int *fd_out)
{
	struct sockaddr_in6 sa;
	socklen_t len = sizeof(sa);
	int fd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return 0;

	memset(&sa, 0, sizeof(sa));
	sa.sin6_family = AF_INET6;
	sa.sin6_addr = in6addr_loopback;

	if (bind(fd, (const struct sockaddr *)&sa, sizeof(sa)) < 0 ||
	    getsockname(fd, (struct sockaddr *)&sa, &len) < 0) {
		close(fd);

		return 0;
	}

	*fd_out = fd;

	return ntohs(sa.sin6_port);
}

/*
 * Hold the wildcard address of one family on one port, so the daemon cannot
 * bind it: neither transport sets SO_REUSEPORT, and SO_REUSEADDR does not let
 * two live sockets share an address and port. -1 when the hold itself failed.
 */
static int port_hold(int family, int type, uint16_t port)
{
	struct sockaddr_in sa4;
	struct sockaddr_in6 sa6;
	struct sockaddr *sa;
	socklen_t len;
	int fd = socket(family, type | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;

	memset(&sa4, 0, sizeof(sa4));
	memset(&sa6, 0, sizeof(sa6));
	sa4.sin_family = AF_INET;
	sa4.sin_port = htons(port);
	sa6.sin6_family = AF_INET6;
	sa6.sin6_port = htons(port);
	sa = family == AF_INET6 ? (struct sockaddr *)&sa6 :
				  (struct sockaddr *)&sa4;
	len = family == AF_INET6 ? sizeof(sa6) : sizeof(sa4);

	/* the same option the transports set: without it the v6 wildcard also
	 * holds the v4 one, and the hold would collide with the daemon's own
	 * IPv4 socket instead of the family under test */
	if (family == AF_INET6) {
		int on = 1;

		if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on,
			       sizeof(on)) < 0) {
			close(fd);

			return -1;
		}
	}

	if (bind(fd, sa, len) < 0 ||
	    (type == SOCK_STREAM && listen(fd, 4) < 0)) {
		close(fd);

		return -1;
	}

	return fd;
}

/* UDP allocation does not exclude TCP listeners or TCP TIME_WAIT. */
static uint16_t tcp_port_hold(int *fd4_out, int *fd6_out)
{
	struct sockaddr_in sa;
	unsigned int i;

	for (i = 0; i < 64; i++) {
		socklen_t len = sizeof(sa);
		int fd4 = port_hold(AF_INET, SOCK_STREAM, 0);
		int fd6, err;
		uint16_t port;

		if (fd4 < 0)
			return 0;

		if (getsockname(fd4, (struct sockaddr *)&sa, &len) < 0) {
			close(fd4);
			return 0;
		}

		port = ntohs(sa.sin_port);
		fd6 = port_hold(AF_INET6, SOCK_STREAM, port);
		if (fd6 >= 0) {
			*fd4_out = fd4;
			*fd6_out = fd6;
			return port;
		}

		err = errno;
		close(fd4);
		if (err != EADDRINUSE)
			return 0;
	}

	return 0;
}

/*
 * A TCP peer on the loopback of one family that accepts and answers nothing.
 * The stream cases read the listener the line holds and the family its
 * connection went out in, so a peer that completes the connect is enough.
 */
static int tcp_peer_open(int family, uint16_t *port)
{
	struct sockaddr_in sa4;
	struct sockaddr_in6 sa6;
	struct sockaddr *sa;
	socklen_t len;
	int fd = socket(family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

	if (fd < 0)
		return -1;

	memset(&sa4, 0, sizeof(sa4));
	memset(&sa6, 0, sizeof(sa6));
	sa4.sin_family = AF_INET;
	sa4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa6.sin6_family = AF_INET6;
	sa6.sin6_addr = in6addr_loopback;
	sa = family == AF_INET6 ? (struct sockaddr *)&sa6 :
				  (struct sockaddr *)&sa4;
	len = family == AF_INET6 ? sizeof(sa6) : sizeof(sa4);

	if (bind(fd, sa, len) < 0 || listen(fd, 4) < 0 ||
	    getsockname(fd, sa, &len) < 0) {
		close(fd);

		return -1;
	}

	*port = ntohs(family == AF_INET6 ? sa6.sin6_port : sa4.sin_port);

	return fd;
}

/* a port nothing listens on: bound to read the number, then given back */
static uint16_t fam_free_port(void)
{
	int fd = -1;
	uint16_t port = udp_loopback_open(&fd);

	if (fd >= 0)
		close(fd);

	return port;
}

/*
 * The datagram waiting at a registrar, read without consuming it and without a
 * uloop pass: a pass would answer it, and several cases have to read what left
 * the daemon before anything answers.
 */
static bool fam_peek(int fd, char *out, size_t olen)
{
	ssize_t n = recv(fd, out, olen - 1, MSG_PEEK | MSG_DONTWAIT);

	if (n <= 0)
		return false;

	out[n] = 0;

	return true;
}

/*
 * A registrar that answers, or one that has gone silent. A silent registrar
 * keeps its socket and leaves the loop, so every request it does not answer
 * waits in its socket buffer and the cases read them with fam_peek() and
 * fam_drain(). Nothing in the daemon learns of the outage: the requests are
 * delivered, and only the answers are missing.
 */
static void fam_registrar_answers(struct uloop_fd *u, bool on)
{
	if (!on) {
		uloop_fd_delete(u);

		return;
	}

	CHECK(uloop_fd_add(u, ULOOP_READ) == 0);
}

/*
 * Every datagram waiting at a socket, read and discarded, and how many of them
 * carried the text the caller asks about. A NULL needle counts them all. Both
 * lines of a fixture send to one registrar, so the Contact user is what says
 * which line a request came from.
 */
static unsigned int fam_drain_user(int fd, const char *user)
{
	char buf[2048];
	unsigned int hits = 0;
	ssize_t n;

	while ((n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT)) > 0) {
		buf[n] = 0;
		if (!user || strstr(buf, user))
			hits++;
	}

	return hits;
}

/* every datagram waiting at a socket, read and discarded, and how many */
static unsigned int fam_drain(int fd)
{
	return fam_drain_user(fd, NULL);
}

/*
 * Passes until a datagram waits at the socket of a silent registrar, or until
 * the bound gives up. It is fam_count_wait() for a registrar that answers
 * nothing: the request that arrives is the one the case reads.
 */
static bool fam_inflight_wait(int fd)
{
	char peek[8];
	unsigned int i;

	for (i = 0; i < 400; i++) {
		if (recv(fd, peek, sizeof(peek), MSG_PEEK | MSG_DONTWAIT) > 0)
			return true;

		uloop_run_timeout(20);
	}

	return recv(fd, peek, sizeof(peek), MSG_PEEK | MSG_DONTWAIT) > 0;
}

/*
 * Passes until a request that carries the text waits at a silent registrar, and
 * that request read into the buffer of the caller with the peer that sent it.
 * Every datagram ahead of it is read away. Both lines of a fixture send to one
 * registrar, so the Contact user is what says which line a request came from,
 * and the line retransmits whatever a case reads away and does not answer.
 */
static bool fam_request_wait(int fd, const char *text, char *out, size_t olen,
			     struct sockaddr_storage *peer, socklen_t *plen)
{
	unsigned int i;

	for (i = 0; i < 400; i++) {
		ssize_t n;

		*plen = sizeof(*peer);
		n = recvfrom(fd, out, olen - 1, MSG_DONTWAIT,
			     (struct sockaddr *)peer, plen);
		if (n <= 0) {
			uloop_run_timeout(20);
			continue;
		}

		out[n] = 0;
		if (strstr(out, text))
			return true;
	}

	return false;
}

/*
 * The next waiting datagram that carries the text, read with no loop pass.
 * False once the socket is empty, so a case walks a whole window with it. Every
 * other request is answered 200 on the way, so the line the case does not watch
 * registers on the request it sent and waits for no retransmission.
 */
static bool fam_request_next(int fd, const char *text, char *out, size_t olen)
{
	struct sockaddr_storage peer;
	socklen_t plen = sizeof(peer);
	ssize_t n;

	while ((n = recvfrom(fd, out, olen - 1, MSG_DONTWAIT,
			     (struct sockaddr *)&peer, &plen)) > 0) {
		out[n] = 0;
		if (strstr(out, text))
			return true;

		registrar_reply(fd, out, fam_rport,
				(const struct sockaddr *)&peer, plen, "200 OK",
				NULL);
		plen = sizeof(peer);
	}

	return false;
}

/* fam_request_wait() for a case that reads the request and answers nothing */
static bool fam_request_read(int fd, const char *text, char *out, size_t olen)
{
	struct sockaddr_storage peer;
	socklen_t plen;

	return fam_request_wait(fd, text, out, olen, &peer, &plen);
}

/*
 * The request of one line, refused with the status and the header the caller
 * names. Every request ahead of it is read away: the case that needs this one
 * answers nothing else.
 */
static bool fam_refuse(int fd, const char *user, const char *status,
		       const char *extra)
{
	struct sockaddr_storage peer;
	socklen_t plen;
	char req[2048];

	if (!fam_request_wait(fd, user, req, sizeof(req), &peer, &plen))
		return false;

	return registrar_reply(fd, req, 0, (const struct sockaddr *)&peer, plen,
			       status, extra);
}

/*
 * What the requests of one line said about one Contact URI while a case
 * answered them by hand: how many REGISTERs the line sent, how many of them
 * removed the URI, and whether one of them carried the URI as its own Contact
 * and as the removal at once, which RFC 3261 10.3 step 7 forbids and which
 * costs the line its binding at a registrar that accepts it.
 */
struct fam_seen {
	unsigned int regs;
	unsigned int removals;
	unsigned int expires0;	/* requests that removed any Contact at all */
	bool twice;
	char last[2048];
};

static void fam_seen_note(struct fam_seen *seen, const char *req,
			  const char *user, const char *uri)
{
	char own[256];
	char removal[256];

	if (strncmp(req, "REGISTER ", 9) || !strstr(req, user))
		return;

	snprintf(own, sizeof(own), "Contact: <%s>\r\n", uri);
	snprintf(removal, sizeof(removal), "Contact: <%s>;expires=0\r\n", uri);

	seen->regs++;
	snprintf(seen->last, sizeof(seen->last), "%s", req);
	if (strstr(req, ";expires=0"))
		seen->expires0++;
	if (!strstr(req, removal))
		return;

	seen->removals++;
	if (strstr(req, own))
		seen->twice = true;
}

/*
 * Passes until the monotonic clock reaches the deadline. A case that has to
 * outlast a retransmission timer counts milliseconds and not passes, because a
 * pass can return early on an unrelated event.
 */
static void fam_pump_ms(unsigned int ms)
{
	struct timespec ts;
	uint64_t until;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	until = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000 + ms;

	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &ts);
		if ((uint64_t)ts.tv_sec * 1000 +
		    (uint64_t)ts.tv_nsec / 1000000 >= until)
			return;

		uloop_run_timeout(20);
	}
}

/*
 * The branch parameter of the top Via of a request (RFC 3261 8.1.1.7). It
 * names one client transaction, so it is what tells a retransmission of a
 * given exchange from the request of any other one, whatever the Call-ID, the
 * Contact and the addresses do.
 */
static bool via_branch(const char *req, char *out, size_t olen)
{
	char via[640];
	const char *start;
	const char *end;
	size_t len;

	if (!req_line_get(req, "Via", via, sizeof(via)))
		return false;

	start = strstr(via, ";branch=");
	if (!start)
		return false;

	start += 8;
	end = strchr(start, ';');
	len = end ? (size_t)(end - start) : strlen(start);
	if (!len || len >= olen)
		return false;

	memcpy(out, start, len);
	out[len] = 0;

	return true;
}

/*
 * The late answer of an exchange the daemon has given up on: the registrar
 * sends the 200 of a request it holds a copy of, to the signalling port of the
 * lines. A dropped transaction matches nothing, so this must move nothing.
 */
static bool fam_stray_answer(int fd, const char *req, uint16_t port)
{
	struct sockaddr_in to;

	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	to.sin_port = htons(port);

	return registrar_reply(fd, req, 0, (const struct sockaddr *)&to,
			       sizeof(to), "200 OK", NULL);
}

/* one key of one status table as text: a string verbatim, a number rendered */
static const char *entry_text(struct blob_attr *tbl, const char *key, char *out,
			      size_t olen)
{
	struct blob_attr *cur;
	size_t rem;

	blobmsg_for_each_attr(cur, tbl, rem) {
		if (strcmp(blobmsg_name(cur), key))
			continue;

		if (blobmsg_type(cur) == BLOBMSG_TYPE_STRING)
			snprintf(out, olen, "%s", blobmsg_get_string(cur));
		else if (blobmsg_type(cur) == BLOBMSG_TYPE_INT32)
			snprintf(out, olen, "%u", blobmsg_get_u32(cur));
		else if (blobmsg_type(cur) == BLOBMSG_TYPE_INT8)
			snprintf(out, olen, "%u", blobmsg_get_u8(cur) ? 1 : 0);
		else
			continue;

		return out;
	}

	return NULL;
}

/*
 * One key of the lines[] entry of one line, copied into the caller's buffer, or
 * NULL when the line or the key is absent. The dump is built and freed here, so
 * the value has to leave in a buffer of the caller's (net_key above).
 */
static const char *fam_key(unsigned int line, const char *key, char *out,
			   size_t olen)
{
	struct blob_buf b;
	struct blob_attr *arr = NULL;
	struct blob_attr *cur;
	struct blob_attr *entry;
	const char *got = NULL;
	size_t rem;

	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	accounts_status_dump(&b, -1);

	blob_for_each_attr(cur, b.head, rem) {
		if (!strcmp(blobmsg_name(cur), "lines"))
			arr = cur;
	}

	if (!arr) {
		blob_buf_free(&b);

		return NULL;
	}

	blobmsg_for_each_attr(entry, arr, rem) {
		if (!table_u32_is(entry, "line", line))
			continue;

		got = entry_text(entry, key, out, olen);
	}

	blob_buf_free(&b);

	return got;
}

static bool fam_is(unsigned int line, const char *key, const char *want)
{
	char buf[192];
	const char *got = fam_key(line, key, buf, sizeof(buf));

	return got && !strcmp(got, want);
}

static bool fam_has(unsigned int line, const char *key, const char *needle)
{
	char buf[192];
	const char *got = fam_key(line, key, buf, sizeof(buf));

	return got && strstr(got, needle) != NULL;
}

/*
 * Passes until the transport counted another refused send, or until the bound
 * gives up. A send to a destination of another family than the socket is the
 * refusal these cases wait for (sip/transport.h).
 */
static bool fam_tx_err_wait(uint64_t mark)
{
	unsigned int i;

	for (i = 0; i < 100; i++) {
		if (sip_transport_stat(acct_transport())->tx_err > mark)
			return true;

		uloop_run_timeout(20);
	}

	return sip_transport_stat(acct_transport())->tx_err > mark;
}

/* passes until the counter reaches the mark, or until the bound gives up */
static bool fam_count_wait(const unsigned int *counter, unsigned int want)
{
	unsigned int i;

	for (i = 0; i < 400; i++) {
		if (*counter >= want)
			return true;

		uloop_run_timeout(20);
	}

	return *counter >= want;
}

/*
 * Passes until the key of the line reads want, or until the bound gives up. The
 * status key is the one to wait on for a live binding, because it folds the
 * refresh of a registered line back into "registered" (acct_status_name).
 */
static bool fam_wait(unsigned int line, const char *key, const char *want)
{
	unsigned int i;

	for (i = 0; i < 400; i++) {
		if (fam_is(line, key, want))
			return true;

		uloop_run_timeout(20);
	}

	return fam_is(line, key, want);
}

/*
 * Every request that reaches a registrar out of the loop, answered by hand
 * until the line reads registered with nothing left waiting. The answers carry
 * the mapping of the fixture (fam_rport), so a Contact correction runs to its
 * end inside the window.
 */
static void fam_answer_until_up(int fd, unsigned int line, const char *user,
				const char *uri, struct fam_seen *seen)
{
	unsigned int i;

	memset(seen, 0, sizeof(*seen));

	for (i = 0; i < 400; i++) {
		if (registrar_answer(fd, fam_rport)) {
			fam_seen_note(seen, registrar_last, user, uri);
			continue;
		}

		if (seen->regs && fam_is(line, "state", "registered"))
			return;

		uloop_run_timeout(20);
	}
}

/*
 * The same registrar by hand, until it answered a request that carries both
 * texts and not the third. False when the bound gives up first.
 */
static bool fam_answer_until_text(int fd, const char *a, const char *b,
				  const char *without)
{
	unsigned int i;

	for (i = 0; i < 400; i++) {
		if (!registrar_answer(fd, fam_rport)) {
			uloop_run_timeout(20);
			continue;
		}

		if (strstr(registrar_last, a) && strstr(registrar_last, b) &&
		    !strstr(registrar_last, without))
			return true;
	}

	return false;
}

/*
 * One line against a loopback registrar. The proxy is a literal, so the
 * resolver delivers its target without a lookup and the line comes up in one
 * uloop pass. T1 is 10 ms, the floor of the timer clamp, because 64*T1 is the
 * bound on a REGISTER nobody answers (RFC 3261 17.1.2.2) and the failover case
 * waits it out.
 */
static void fam_channel(struct cfg *cfg, unsigned int id, const char *proxy,
			uint16_t reg_port, uint16_t local_port)
{
	struct cfg_channel *ch = &cfg->channel[id];

	chan_default(ch, id);
	snprintf(ch->sip_username, sizeof(ch->sip_username), "555100%u", id);
	snprintf(ch->sip_domain, sizeof(ch->sip_domain), "%s", proxy);
	snprintf(ch->sip_proxy, sizeof(ch->sip_proxy), "%s", proxy);
	snprintf(ch->sip_registrar, sizeof(ch->sip_registrar), "%s", proxy);
	snprintf(ch->sip_outbound_proxy, sizeof(ch->sip_outbound_proxy), "%s",
		 proxy);
	ch->sip_outbound_proxy_port = reg_port;
	ch->sip_registrar_port = reg_port;
	ch->sip_port = reg_port;
	ch->sip_local_port = local_port;
	ch->timers.t1 = 10;
	ch->timers.t2 = 40;
	ch->timers.t4 = 50;
	/* the first retry delay: a move after a failed attempt sends nothing,
	 * so the case that waits for the next attempt waits for this */
	ch->sip_register_retry = 1;
}

struct fam_target {
	const char *host;
	uint16_t port;
};

/* one resolved answer, in the shape sip_dns_resolve() hands to the account */
static void fam_answer(struct sip_dns_result *res, const struct fam_target *t,
		       unsigned int count, bool changed)
{
	unsigned int i;

	memset(res, 0, sizeof(*res));
	res->status = SIP_DNS_OK;
	res->ttl = 300;
	res->changed = changed;
	res->count = count;

	for (i = 0; i < count; i++) {
		snprintf(res->target[i].host, sizeof(res->target[i].host), "%s",
			 t[i].host);
		res->target[i].port = t[i].port;
		/* one priority and one group: a plain address answer is a
		 * single run, and the family sort partitions it whole
		 * (shared/resolve.h sip_dns_family_sort) */
		sip_dst_set(&res->target[i].dst, t[i].host, t[i].port);
	}
}

/*
 * The SRV priority of each target of an answer fam_answer() built, one value
 * per target in the order they were added. fam_answer() leaves them all 0,
 * which is what a plain address answer carries.
 */
static void fam_answer_prio(struct sip_dns_result *res, const uint16_t *prio)
{
	unsigned int i;

	for (i = 0; i < res->count; i++)
		res->target[i].prio = prio[i];
}

/*
 * A resolution that ended in an error, in the shape sip_dns_resolve() hands to
 * the account. fam_answer() builds the successful form only, and a failed
 * answer carries no target at all (shared/resolve.h).
 */
static void fam_answer_status(struct sip_dns_result *res,
			      enum sip_dns_status status)
{
	memset(res, 0, sizeof(*res));
	res->status = status;
}

/* Every case of a class that binds ::1 is named, and skipped by name, so a
 * host without an IPv6 loopback reports what it did not run. */
static void fam_class_skip(const char *const *titles, unsigned int count,
			   const char *why)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		CASE(titles[i]);
		tu_skip(why);
	}
}

/*
 * The build family and the two gates in front of a move. Only the last case
 * opens an IPv6 socket. The others run on any host, because a line that the
 * gates hold stays on the IPv4 socket that it was built with.
 */
static void test_family_gate(void)
{
	static const struct addr6_row gua[] = { { "2001:db8:40::5", -1 } };
	static struct cfg live;
	static struct cfg gate_on;
	static struct cfg unreachable;
	static struct cfg none;
	static struct cfg parked;
	char why[128];
	uint64_t err;
	uint16_t local_port;
	uint16_t dead_port;
	unsigned int i;
	bool v6 = tu_v6_loopback(why, sizeof(why));

	CASE("gate: an IPv6 proxy literal with the gate off is an ipv4 line");
	CHECK(uloop_init() == 0);
	local_port = fam_free_port();
	dead_port = fam_free_port();
	CHECK(local_port != 0 && dead_port != 0);

	event_test_mode(true);
	fw_test_mode(true);
	accounts_hal_features_set(HAL_FEAT_IPV6_MEDIA);

	base_default();
	live = base;
	live.channels = 1;
	fam_channel(&live, 0, "::1", dead_port, local_port);
	gate_on = live;
	gate_on.ipv6_enabled = true;
	unreachable = gate_on;
	/* a documentation prefix (RFC 3849): the address parses, and no host
	 * has a route to it, so the source address probe has no answer */
	fam_channel(&unreachable, 0, "2001:db8:40::9", dead_port, local_port);
	none = live;
	none.channels = 0;

	/* the address every Via and Contact of an IPv4 line carries: without it
	 * a line with an IPv6 target has no local host and never sends */
	status46_inject(V6_DEV, "192.0.2.55", NULL, 0);

	core_start(&live);
	CHECK_UINT(status_line0(NULL, 0), 1);
	CHECK(fam_is(0, "family", "ipv4"));

	/* the key stands from the build on, before a registration exists */
	CASE("status: a line reports its family before it registers");
	CHECK(fam_is(0, "state", "resolving"));
	CHECK(fam_is(0, "family", "ipv4"));

	/* the same rule for the question: this fixture names an outbound proxy
	 * port, so the default mode asks the address of the next hop first */
	CASE("status: a line reports its next-hop question too");
	CHECK(fam_is(0, "dns_lookup", "addr_srv"));

	/*
	 * literal_deliver() hands an IPv6 literal over whatever the gate says
	 * (shared/resolve.h "Numeric targets"), so the target arrives and the
	 * IPv4 socket refuses to send to it and counts the refusal.
	 */
	CASE("gate: the send to the IPv6 target of a gated line is counted");
	err = sip_transport_stat(acct_transport())->tx_err;
	CHECK(fam_tx_err_wait(err));
	CHECK(fam_is(0, "family", "ipv4"));
	CHECK(!fam_is(0, "state", "registered"));

	CASE("gate: the gate and an address6 without IPv6 media stay ipv4");
	accounts_hal_features_set(0);
	status6_inject(V6_DEV, ROWS(gua));
	CHECK(net_key_is("address6", "2001:db8:40::5"));
	core_reload(&live, &gate_on);
	CHECK_UINT(status_line0(NULL, 0), 1);
	CHECK(fam_is(0, "family", "ipv4"));

	/* and the move stays shut for the same reason, so the target of the
	 * other family keeps being refused */
	CASE("gate: a backend without IPv6 media never moves the line");
	err = sip_transport_stat(acct_transport())->tx_err;
	CHECK(fam_tx_err_wait(err));
	CHECK(fam_is(0, "family", "ipv4"));

	CASE("gate: the gate, an address6 and IPv6 media build an ipv6 line");
	if (!v6) {
		tu_skip(why);
	} else {
		accounts_hal_features_set(HAL_FEAT_IPV6_MEDIA);
		core_reload(&gate_on, &none);
		core_reload(&none, &gate_on);
		CHECK_UINT(status_line0(NULL, 0), 1);
		CHECK(fam_is(0, "family", "ipv6"));
	}

	/*
	 * The move derives the local host of the new family first, and with no
	 * address of that family the route toward the target is the only
	 * answer left. A target with no route has none, so the line keeps its
	 * socket, its family and the address its Contact carries.
	 */
	CASE("move: a move with no address of the new family leaves the line");
	status6_inject(V6_DEV, NULL, 0);
	CHECK(net_key_gone("address6"));
	core_reload(&gate_on, &none);
	core_reload(&none, &unreachable);
	CHECK(fam_is(0, "family", "ipv4"));
	err = sip_transport_stat(acct_transport())->tx_err;
	CHECK(fam_tx_err_wait(err));
	CHECK(fam_is(0, "family", "ipv4"));
	CHECK(fam_has(0, "contact", "@192.0.2.55:"));

	parked = none;
	snprintf(parked.local_interface, sizeof(parked.local_interface),
		 "wan2");
	core_reload(&unreachable, &parked);
	for (i = 0; i < 400 && status_line0(NULL, 0); i++)
		uloop_run_timeout(20);
	CHECK_UINT(status_line0(NULL, 0), 0);
	accounts_hal_features_set(0);
	uloop_done();
}

static const char *const fam_move_titles[] = {
	"move: two lines come up on the IPv4 registrar and share one socket",
	"move: an IPv6 target opens a socket and releases the IPv4 one",
	"move: the local host and the Contact follow the line into IPv6",
	"move: the REGISTER of the move carries the old Contact with expires=0",
	"move: the subscription is rebuilt and starts again on the next 2xx",
	"move: the socket the other line shares stays open for that line",
	"move: a NAT learned Contact port is given up with the family",
	"move: a failed IPv4 target that advances to IPv6 moves the line",
	"move: an IPv4 address change does not re-register an IPv6 line",
	"move: a move after a failed attempt sends nothing itself",
	"move: and the retry timer carries the attempt in the new family",
	"move: a line draining its Expires 0 stays where it is",
	"move: a transport the new family cannot open leaves the Contact alone",
	"move: and the REGISTER after it carries no removal Contact",
	"move: a changed answer that moves the line sends one REGISTER",
	"priority: a move to the other family follows the family path and "
	"skips the removal",
	"move: a cross-family move that fails takes the line back to the "
	"binding's family",
	"answer: the line reports which answer built its targets",
};

static void test_family_move(void)
{
	static struct cfg live;
	static struct cfg moved;
	static struct cfg none;
	static struct cfg parked;
	struct sip_dns_result res;
	static const uint16_t v6_first[2] = { 10, 20 };
	struct fam_target v6only[1];
	struct fam_target v4only[1];
	struct fam_target dead6[2];
	struct fam_target failover[2];
	char why[128];
	char wire[2048];
	char want[192];
	char contact[192];
	unsigned int recs;
	int held = -1;
	uint16_t reg4, reg6, local_port, dead_port;
	unsigned int mark;
	unsigned int mark4;
	unsigned int i;
	unsigned int t = 0;
	bool v6 = tu_v6_loopback(why, sizeof(why));

	if (!v6) {
		fam_class_skip(fam_move_titles,
			       (unsigned int)(sizeof(fam_move_titles) /
					      sizeof(fam_move_titles[0])),
			       why);
		return;
	}

	CASE(fam_move_titles[t++]);
	CHECK(uloop_init() == 0);
	reg4 = udp_loopback_open(&fam_fd4);
	reg6 = udp6_loopback_open(&fam_fd6);
	local_port = fam_free_port();
	dead_port = fam_free_port();
	CHECK(reg4 && reg6 && local_port && dead_port);
	if (!reg4 || !reg6)
		return;

	fam_ufd4.fd = fam_fd4;
	fam_ufd4.cb = fam_cb4;
	fam_ufd6.fd = fam_fd6;
	fam_ufd6.cb = fam_cb6;
	CHECK(uloop_fd_add(&fam_ufd4, ULOOP_READ) == 0);
	CHECK(uloop_fd_add(&fam_ufd6, ULOOP_READ) == 0);

	v6only[0].host = "::1";
	v6only[0].port = reg6;
	v4only[0].host = "127.0.0.1";
	v4only[0].port = reg4;
	failover[0].host = "127.0.0.1";
	failover[0].port = dead_port;
	failover[1].host = "::1";
	failover[1].port = reg6;

	event_test_mode(true);
	fw_test_mode(true);
	accounts_hal_features_set(HAL_FEAT_IPV6_MEDIA);

	base_default();
	live = base;
	live.channels = 2;
	live.ipv6_enabled = true;
	live.mwi_subscribe = true;
	fam_channel(&live, 0, "127.0.0.1", reg4, local_port);
	fam_channel(&live, 1, "127.0.0.1", reg4, local_port);
	/* the line the exact REGISTER count is read on keeps the ordinary T1,
	 * so no retransmission of its own lands inside a uloop pass */
	live.channel[1].timers.t1 = 500;
	live.channel[1].timers.t2 = 4000;
	live.channel[1].timers.t4 = 5000;
	none = live;
	none.channels = 0;

	core_start(&live);
	CHECK(fam_count_wait(&fam_regs4, 2));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));
	/* one key, one socket: the family is part of it and both lines are on
	 * the same port, device and family (sip/transport.h) */
	CHECK_UINT(sip_sock_count(acct_transport()), 1);
	CHECK(fam_is(0, "family", "ipv4"));
	CHECK(fam_is(1, "family", "ipv4"));
	CHECK(!fam_is(0, "mwi_subscription", "off"));

	/*
	 * The answer is unchanged, so the dns_cache_update_mode hook stays out
	 * of it (acct_reregister_needed): the REGISTER that follows is the
	 * move's own.
	 */
	CASE(fam_move_titles[t++]);
	fam_answer(&res, v6only, 1, false);
	acct_dns_result_inject(0, &res);
	CHECK(fam_is(0, "family", "ipv6"));
	CHECK_UINT(sip_sock_count(acct_transport()), 2);
	CHECK(fam_peek(fam_fd6, wire, sizeof(wire)));

	CASE(fam_move_titles[t++]);
	CHECK(fam_has(0, "contact", "@[::1]:"));
	CHECK(!fam_has(0, "contact", "127.0.0.1"));

	CASE(fam_move_titles[t++]);
	snprintf(want, sizeof(want), "<sip:5551000@127.0.0.1:%u>;expires=0",
		 local_port);
	CHECK(has_str(wire, want));
	snprintf(want, sizeof(want), "<sip:5551000@[::1]:%u>", local_port);
	CHECK(has_str(wire, want));

	/*
	 * The subscription holds the Contact the registrar had, so the move
	 * gives it up and builds a fresh one, which is "off" until a 2xx starts
	 * it again (reg_registered_cb).
	 */
	CASE(fam_move_titles[t++]);
	CHECK(fam_is(0, "mwi_subscription", "off"));
	CHECK(fam_count_wait(&fam_regs6, 1));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(!fam_is(0, "mwi_subscription", "off"));

	/* the second line never asked to move, and the socket it shares with
	 * nobody now still carries its registration */
	CASE(fam_move_titles[t++]);
	CHECK(fam_is(1, "family", "ipv4"));
	CHECK_UINT(sip_sock_count(acct_transport()), 2);
	mark = fam_regs4;
	CHECK(account_unregister_cmd(1));
	CHECK(fam_wait(1, "state", "unregistered"));
	CHECK(account_register_cmd(1));
	CHECK(fam_wait(1, "state", "registered"));
	CHECK(fam_regs4 >= mark + 2);
	CHECK(fam_has(1, "contact", "@127.0.0.1:"));

	/*
	 * A registrar that reports received and rport moves the Contact to the
	 * mapping it sees (RFC 3581). That mapping belongs to the old family,
	 * so the move gives the port back to the configured one (sip/reg.h).
	 */
	CASE(fam_move_titles[t++]);
	fam_rport = 40000;
	CHECK(account_unregister_cmd(1));
	CHECK(fam_wait(1, "state", "unregistered"));
	CHECK(account_register_cmd(1));
	CHECK(fam_wait(1, "state", "registered"));
	CHECK(fam_has(1, "contact", ":40000"));
	fam_rport = 0;
	fam_answer(&res, v6only, 1, false);
	acct_dns_result_inject(1, &res);
	CHECK(fam_is(1, "family", "ipv6"));
	CHECK(fam_has(1, "contact", "@[::1]:"));
	CHECK(!fam_has(1, "contact", ":40000"));
	CHECK(fam_wait(1, "state", "registered"));

	/*
	 * The failure walk. The answer replaces the target set, so the sticky
	 * selection falls on the first target, which nothing answers on. Its
	 * timeout penalizes it and advances to the IPv6 target before the
	 * failure callback runs (reg.c reg_fail), and the line follows.
	 */
	CASE(fam_move_titles[t++]);
	fam_answer(&res, v4only, 1, false);
	acct_dns_result_inject(0, &res);
	CHECK(fam_is(0, "family", "ipv4"));
	CHECK(fam_wait(0, "state", "registered"));
	mark = fam_regs6;
	fam_answer(&res, failover, 2, true);
	acct_dns_result_inject(0, &res);
	CHECK(fam_is(0, "family", "ipv4"));
	CHECK(fam_count_wait(&fam_regs6, mark + 1));
	CHECK(fam_is(0, "family", "ipv6"));
	CHECK(fam_is(0, "targets", "2"));
	CHECK(fam_is(0, "registrar", "::1"));
	CHECK(fam_wait(0, "state", "registered"));

	/*
	 * The IPv4 address moved, so the IPv4 line rebinds and refreshes its
	 * binding in place, and the IPv6 line keeps its registration: its
	 * socket is reopened by the transport-wide rebind and nothing of its
	 * binding moved.
	 */
	CASE(fam_move_titles[t++]);
	fam_answer(&res, v4only, 1, false);
	acct_dns_result_inject(1, &res);
	CHECK(fam_is(1, "family", "ipv4"));
	CHECK(fam_wait(1, "state", "registered"));
	mark = fam_regs6;
	status46_inject(V6_DEV, "192.0.2.66", NULL, 0);
	CHECK(fam_is(0, "family", "ipv6"));
	CHECK(fam_is(0, "status", "registered"));
	CHECK(fam_has(0, "contact", "@[::1]:"));
	CHECK(!fam_peek(fam_fd6, wire, sizeof(wire)));
	CHECK_UINT(fam_regs6, mark);
	CHECK(fam_is(1, "state", "refreshing"));
	CHECK(fam_wait(1, "state", "registered"));

	/*
	 * The recovery of a refused send, without a REGISTER of its own. The
	 * mask is cleared, so the IPv6 target of the answer lands on a line
	 * that may not move into that family; with the mask back the next
	 * attempt is refused by the transport, and the failure that follows
	 * moves the line and leaves the attempt to the retry timer reg.c
	 * already armed. A refresh here would clear that backoff on every move.
	 */
	CASE(fam_move_titles[t++]);
	accounts_hal_features_set(0);
	fam_answer(&res, v6only, 1, false);
	acct_dns_result_inject(1, &res);
	CHECK(fam_is(1, "family", "ipv4"));
	accounts_hal_features_set(HAL_FEAT_IPV6_MEDIA);
	CHECK(fam_is(1, "family", "ipv4"));
	CHECK(account_unregister_cmd(1));
	CHECK(fam_wait(1, "state", "unregistered"));

	/* two passes so the SUBSCRIBE the other line sent behind its own 2xx is
	 * answered and gone: the peek below asks what this line put on the
	 * wire, and every registrar here is shared */
	uloop_run_timeout(20);
	uloop_run_timeout(20);
	CHECK(!fam_peek(fam_fd6, wire, sizeof(wire)));
	mark = fam_regs6;
	CHECK(account_register_cmd(1));
	CHECK(fam_is(1, "family", "ipv6"));
	CHECK(fam_is(1, "state", "backoff"));
	CHECK(!fam_peek(fam_fd4, wire, sizeof(wire)));
	CHECK(!fam_peek(fam_fd6, wire, sizeof(wire)));
	CHECK_UINT(fam_regs6, mark);

	CASE(fam_move_titles[t++]);
	CHECK(fam_count_wait(&fam_regs6, mark + 1));
	CHECK(fam_wait(1, "state", "registered"));
	CHECK(fam_is(1, "family", "ipv6"));
	CHECK(fam_has(1, "contact", "@[::1]:"));

	/*
	 * A line that is draining an Expires 0 keeps everything the grace path
	 * is about to discard. The rebuild behind it resolves afresh and reads
	 * the preference, so nothing is lost by standing still here.
	 */
	CASE(fam_move_titles[t++]);
	CHECK(fam_is(0, "family", "ipv6"));
	CHECK(fam_key(0, "contact", contact, sizeof(contact)) != NULL);
	moved = live;
	moved.srtp_enabled = !live.srtp_enabled;
	core_reload(&live, &moved);
	CHECK(fam_is(0, "state", "trying"));
	mark = sip_sock_count(acct_transport());
	fam_answer(&res, v4only, 1, true);
	acct_dns_result_inject(0, &res);
	CHECK(fam_is(0, "family", "ipv6"));
	CHECK(fam_is(0, "contact", contact));
	CHECK_UINT(sip_sock_count(acct_transport()), mark);

	/* and the line the drain rebuilds resolves afresh behind it */
	CHECK(fam_wait(0, "state", "registered"));

	/*
	 * The rollback of a refused move. The transport of the new family opens
	 * first, so a bind that fails leaves the Contact the registrar reported
	 * through received and rport standing: a rebuild would replace that
	 * address with the configured one and leave the address the registrar
	 * knows to be removed on the next REGISTER (RFC 3261 10.2.2).
	 */
	CASE(fam_move_titles[t++]);
	fam_rport = 40001;
	CHECK(account_unregister_cmd(1));
	CHECK(fam_wait(1, "state", "unregistered"));
	CHECK(account_register_cmd(1));
	CHECK(fam_wait(1, "state", "registered"));
	CHECK(fam_has(1, "contact", ":40001"));
	fam_rport = 0;

	held = port_hold(AF_INET6, SOCK_DGRAM, local_port);
	CHECK(held >= 0);
	mark = sip_sock_count(acct_transport());
	fam_answer(&res, v6only, 1, false);
	acct_dns_result_inject(1, &res);
	CHECK(fam_is(1, "family", "ipv4"));
	CHECK(fam_has(1, "contact", ":40001"));
	CHECK_UINT(sip_sock_count(acct_transport()), mark);

	/* an answer that returns to the standing binding sends nothing
	 * through the ordinary hook; one that moves the endpoint registers,
	 * and that REGISTER names the Contact the registrar knows and no
	 * other */
	CASE(fam_move_titles[t++]);
	uloop_run_timeout(20);
	uloop_run_timeout(20);
	CHECK(!fam_peek(fam_fd4, wire, sizeof(wire)));
	fam_answer(&res, v4only, 1, true);
	acct_dns_result_inject(1, &res);
	CHECK(!fam_peek(fam_fd4, wire, sizeof(wire)));
	{
		struct fam_target probe[1];
		uint16_t probe_port;
		int probe_fd = -1;

		probe_port = udp_loopback_open(&probe_fd);
		CHECK(probe_port != 0);
		probe[0].host = "127.0.0.1";
		probe[0].port = probe_port;
		fam_answer(&res, probe, 1, true);
		acct_dns_result_inject(1, &res);
		CHECK(fam_peek(probe_fd, wire, sizeof(wire)));
		CHECK(has_str(wire, ":40001"));
		CHECK(!has_str(wire, ";expires=0"));
		/* answer it by hand, so the exchange concludes and the return
		 * inject below retargets instead of waiting behind it */
		CHECK(registrar_answer(probe_fd, 0));
		CHECK(fam_wait(1, "state", "registered"));
		close(probe_fd);
	}
	if (held >= 0)
		close(held);
	fam_answer(&res, v4only, 1, true);
	acct_dns_result_inject(1, &res);
	CHECK(fam_wait(1, "state", "registered"));

	/*
	 * A changed answer that moves the line registers once. The move sends
	 * the REGISTER itself and keeps the targets, the selection and the
	 * Call-ID, so the re-registration the dns_cache_update_mode hook asks
	 * for would put a second one on the wire with the line reported down in
	 * between: the state would read trying, not refreshing.
	 */
	CASE(fam_move_titles[t++]);
	uloop_run_timeout(20);
	uloop_run_timeout(20);
	CHECK(!fam_peek(fam_fd6, wire, sizeof(wire)));
	mark = fam_regs6;
	mark4 = fam_removals4;
	/* the publisher runs one post at a time, so it is reaped first: a down
	 * edge the stop and start posts would then start and be recorded */
	event_test_reap(1, 0);
	recs = event_test_recs();
	fam_answer(&res, v6only, 1, true);
	acct_dns_result_inject(1, &res);
	CHECK(fam_is(1, "family", "ipv6"));
	CHECK(fam_is(1, "state", "refreshing"));
	CHECK(fam_wait(1, "state", "registered"));
	CHECK_UINT(fam_regs6, mark + 1);

	/* and no line went down for the registrar or for the cloud: the stop
	 * and start would have posted the unregistered edge */
	for (i = recs; i < event_test_recs(); i++)
		CHECK(!has_str(evt_arg(i, 4), "/reg/failed"));

	/*
	 * The family path sent that REGISTER, so it is a move and its 2xx
	 * would remove the old binding. The line gave the IPv4 transport up
	 * with the family, so there is nothing to send the Expires 0 over and
	 * the old binding lapses on its own.
	 */
	CASE(fam_move_titles[t++]);
	for (i = 0; i < 10; i++)
		uloop_run_timeout(20);
	CHECK_UINT(fam_removals4, mark4);
	CHECK(fam_is(1, "state", "registered"));

	/*
	 * A cross-family move that fails. The answer hands the selection to an
	 * IPv6 target of a better priority, the family sync takes the line
	 * there before the REGISTER leaves, and nothing answers. The rollback
	 * puts the selection back on the IPv4 server that holds the binding,
	 * and the account follows it back through the rolled_back report: the
	 * line reads registered on its IPv4 Contact again and posts nothing.
	 * Line 0 keeps the short T1 of fam_channel(), so the transaction ends
	 * inside this window.
	 */
	CASE(fam_move_titles[t++]);
	fam_answer(&res, v4only, 1, true);
	acct_dns_result_inject(0, &res);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_is(0, "family", "ipv4"));
	CHECK(fam_key(0, "contact", contact, sizeof(contact)) != NULL);
	event_test_reap(0, 0);
	recs = event_test_posts();
	dead6[0].host = "::1";
	dead6[0].port = dead_port;
	dead6[1].host = "127.0.0.1";
	dead6[1].port = reg4;
	fam_answer(&res, dead6, 2, true);
	fam_answer_prio(&res, v6_first);
	acct_dns_result_inject(0, &res);
	CHECK(fam_is(0, "family", "ipv6"));
	CHECK(fam_wait(0, "family", "ipv4"));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_is(0, "contact", contact));
	CHECK_UINT(event_test_posts(), recs);

	/* the flag rides on the answer, so the line reports the word of the
	 * answer it holds now and no earlier one */
	CASE(fam_move_titles[t++]);
	fam_answer(&res, v4only, 1, true);
	res.srv = true;
	acct_dns_result_inject(0, &res);
	CHECK(fam_is(0, "dns_answer", "srv"));
	fam_answer(&res, v4only, 1, true);
	acct_dns_result_inject(0, &res);
	CHECK(fam_is(0, "dns_answer", "addr"));

	parked = none;
	snprintf(parked.local_interface, sizeof(parked.local_interface),
		 "wan2");
	core_reload(&moved, &parked);
	for (i = 0; i < 400 && status_line0(NULL, 0); i++)
		uloop_run_timeout(20);
	CHECK_UINT(status_line0(NULL, 0), 0);
	accounts_hal_features_set(0);

	uloop_fd_delete(&fam_ufd4);
	uloop_fd_delete(&fam_ufd6);
	close(fam_fd4);
	close(fam_fd6);
	fam_fd4 = -1;
	fam_fd6 = -1;
	uloop_done();
}

static const char *const fam_flip_titles[] = {
	"flip: two lines come up, one on IPv6 and one on IPv4",
	"flip: the address6 that arrives rebuilds the IPv4 line and moves the "
	"IPv6 line in place",
	"flip: the answer of that resolve lands on the preferred family",
	"flip: a zone of one family alone takes the line back to IPv4",
	"flip: an address6 that moves inside the family moves that line in place",
	"flip: and its Contact carries the address netifd now holds",
	"flip: the address6 that leaves rebuilds the IPv6 line alone",
	"flip: the same answer lands on IPv4 once IPv4 is preferred again",
	"flip: an address6 without IPv6 media moves no preference",
	"flip: the mask that opens IPv6 media rebuilds the IPv4 line",
	"flip: and the answer of that resolve lands it on IPv6",
	"flip: a backend that loses IPv6 media lets the line fall back",
	"flip: a netdev move with a new address6 acts on each line once",
	"flip: an answer that moves both addresses acts on each line once",
};

/*
 * The preferred family follows the IPv6 address of the voice netdev and the
 * backend feature mask. Every line that signals in the other family resolves
 * again at once. Without that, the line would keep its family until its DNS
 * refresh timer fires, up to an hour later. The answer of that resolution
 * decides the family. The preference puts a target of one family in front of
 * the other.
 */
static void test_family_flip(void)
{
	static const struct addr6_row gua[] = { { "2001:db8:40::5", -1 } };
	static const struct addr6_row gua2[] = { { "2001:db8:40::6", -1 } };
	static struct cfg live;
	static struct cfg none;
	static struct cfg parked;
	struct sip_dns_result res;
	struct fam_target both[2];
	char why[128];
	char wire[2048];
	uint64_t qmark;
	uint16_t reg4, reg6, port0, port1;
	unsigned int mark;
	unsigned int i;
	unsigned int t = 0;
	bool v6 = tu_v6_loopback(why, sizeof(why));

	if (!v6) {
		fam_class_skip(fam_flip_titles,
			       (unsigned int)(sizeof(fam_flip_titles) /
					      sizeof(fam_flip_titles[0])),
			       why);
		return;
	}

	CASE(fam_flip_titles[t++]);
	CHECK(uloop_init() == 0);
	reg4 = udp_loopback_open(&fam_fd4);
	reg6 = udp6_loopback_open(&fam_fd6);
	port0 = fam_free_port();
	port1 = fam_free_port();
	CHECK(reg4 && reg6 && port0 && port1);
	if (!reg4 || !reg6)
		return;

	fam_ufd4.fd = fam_fd4;
	fam_ufd4.cb = fam_cb4;
	fam_ufd6.fd = fam_fd6;
	fam_ufd6.cb = fam_cb6;
	CHECK(uloop_fd_add(&fam_ufd4, ULOOP_READ) == 0);
	CHECK(uloop_fd_add(&fam_ufd6, ULOOP_READ) == 0);

	both[0].host = "127.0.0.1";
	both[0].port = reg4;
	both[1].host = "::1";
	both[1].port = reg6;

	event_test_mode(true);
	fw_test_mode(true);
	accounts_hal_features_set(HAL_FEAT_IPV6_MEDIA);

	base_default();
	live = base;
	live.channels = 2;
	live.ipv6_enabled = true;
	fam_channel(&live, 0, "::1", reg6, port0);
	fam_channel(&live, 1, "127.0.0.1", reg4, port1);
	none = live;
	none.channels = 0;

	core_start(&live);
	CHECK(fam_count_wait(&fam_regs6, 1));
	CHECK(fam_count_wait(&fam_regs4, 1));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));
	CHECK(fam_is(0, "family", "ipv6"));
	CHECK(fam_is(1, "family", "ipv4"));

	/* The IPv4 line resolves again because the preference left its family.
	 * The IPv6 line keeps its binding and its family, because only the
	 * address it carries moved. */
	CASE(fam_flip_titles[t++]);
	status6_inject(V6_DEV, ROWS(gua));
	CHECK(net_key_is("address6", "2001:db8:40::5"));
	CHECK(fam_is(1, "state", "resolving"));
	CHECK(fam_is(0, "state", "refreshing"));
	CHECK(fam_is(0, "status", "registered"));
	CHECK(fam_is(0, "family", "ipv6"));

	/*
	 * The answer of the resolution the flip started, in the order the
	 * resolver delivers it under the preference of the moment
	 * (sip_dns_family_sort). The line takes the family of the first target.
	 */
	CASE(fam_flip_titles[t++]);
	mark = fam_regs4;
	fam_answer(&res, both, 2, false);
	sip_dns_family_sort(res.target, res.count,
			    sip_dns_prefer_family(acct_dns()));
	acct_dns_result_inject(1, &res);
	CHECK(fam_is(1, "family", "ipv6"));
	CHECK(fam_peek(fam_fd6, wire, sizeof(wire)));

	/* the line's own literal query answers behind it with one family, and
	 * the line follows that answer back */
	CASE(fam_flip_titles[t++]);
	CHECK(fam_count_wait(&fam_regs4, mark + 1));
	CHECK(fam_is(1, "family", "ipv4"));
	CHECK(fam_wait(1, "state", "registered"));

	/*
	 * An address6 that moves from one global address to another leaves the
	 * preference where it is, and every IPv6 line still carries the old
	 * address in its Via, its Contact and its media.
	 */
	CASE(fam_flip_titles[t++]);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_has(0, "contact", "@[2001:db8:40::5]:"));
	qmark = sip_dns_stat(acct_dns())->queries;
	status6_inject(V6_DEV, ROWS(gua2));
	CHECK(net_key_is("address6", "2001:db8:40::6"));
	CHECK_UINT(sip_dns_prefer_family(acct_dns()), AF_INET6);
	CHECK(fam_is(0, "state", "refreshing"));
	CHECK(fam_is(0, "status", "registered"));
	CHECK_UINT(sip_dns_stat(acct_dns())->queries, qmark);
	CHECK(fam_is(1, "status", "registered"));
	CHECK(fam_is(1, "family", "ipv4"));

	CASE(fam_flip_titles[t++]);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_is(0, "family", "ipv6"));
	CHECK(fam_has(0, "contact", "@[2001:db8:40::6]:"));

	CASE(fam_flip_titles[t++]);
	status6_inject(V6_DEV, NULL, 0);
	CHECK(net_key_gone("address6"));
	CHECK(fam_is(0, "state", "resolving"));
	CHECK(fam_is(1, "status", "registered"));
	CHECK(fam_is(1, "family", "ipv4"));

	CASE(fam_flip_titles[t++]);
	fam_answer(&res, both, 2, false);
	sip_dns_family_sort(res.target, res.count,
			    sip_dns_prefer_family(acct_dns()));
	acct_dns_result_inject(0, &res);
	CHECK(fam_is(0, "family", "ipv4"));
	CHECK(fam_peek(fam_fd4, wire, sizeof(wire)));
	/* and its own zone, which holds one family, takes it back */
	CHECK(fam_wait(0, "state", "registered"));

	/*
	 * The third input of the preference. The daemon starts its lines before
	 * the DSP reports what it carries (main.c starts the accounts and
	 * pushes the probed mask from a later callback), so a mask that opens
	 * IPv6 media arrives under lines that are already up. Without the mask
	 * the address alone moves nothing, because a line that registered over
	 * IPv6 would answer every call with congestion.
	 */
	CASE(fam_flip_titles[t++]);
	accounts_hal_features_set(0);
	status6_inject(V6_DEV, ROWS(gua));
	CHECK(net_key_is("address6", "2001:db8:40::5"));
	CHECK_UINT(sip_dns_prefer_family(acct_dns()), AF_INET);
	CHECK(fam_is(1, "status", "registered"));
	CHECK(fam_is(1, "family", "ipv4"));
	/* the IPv6 line takes the address that arrived and stays where it is */
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_is(0, "family", "ipv6"));

	/* the line that signals in the preferred family already is left alone,
	 * and the one that does not resolves again at once */
	CASE(fam_flip_titles[t++]);
	accounts_hal_features_set(HAL_FEAT_IPV6_MEDIA);
	CHECK_UINT(sip_dns_prefer_family(acct_dns()), AF_INET6);
	CHECK(fam_is(1, "state", "resolving"));
	CHECK(fam_is(0, "status", "registered"));
	CHECK(fam_is(0, "family", "ipv6"));

	CASE(fam_flip_titles[t++]);
	fam_answer(&res, both, 2, false);
	sip_dns_family_sort(res.target, res.count,
			    sip_dns_prefer_family(acct_dns()));
	acct_dns_result_inject(1, &res);
	CHECK(fam_is(1, "family", "ipv6"));
	CHECK(fam_peek(fam_fd6, wire, sizeof(wire)));
	CHECK(fam_wait(1, "state", "registered"));

	/*
	 * The way back out of IPv6 is not gated. A backend that loses the
	 * feature, like an operator gate that closes, leaves the line on a
	 * family the box no longer serves, so the preference push rebuilds it
	 * and the IPv4 target of the next answer takes it there.
	 */
	CASE(fam_flip_titles[t++]);
	CHECK(fam_is(0, "family", "ipv6"));
	accounts_hal_features_set(0);
	CHECK_UINT(sip_dns_prefer_family(acct_dns()), AF_INET);
	CHECK(fam_is(0, "state", "resolving"));
	fam_answer(&res, both, 1, false);
	acct_dns_result_inject(0, &res);
	CHECK(fam_is(0, "family", "ipv4"));
	CHECK(fam_wait(0, "state", "registered"));

	/*
	 * One netifd answer acts on each line once. The answer below moves the
	 * netdev and carries an address6, so the record is given up and taken
	 * again inside it. A line rebuilt twice would cancel and restart its own
	 * resolution twice, and a line moved in place twice would send two
	 * REGISTERs. The preference left the family of the IPv4 line, so that
	 * line rebuilds and the resolver counts its one resolution. The IPv6
	 * line keeps its family, so it moves in place: it asks the resolver
	 * nothing, and its one REGISTER waits at the registrar before any pass.
	 */
	status6_inject(V6_DEV, NULL, 0);
	accounts_hal_features_set(HAL_FEAT_IPV6_MEDIA);
	fam_answer(&res, &both[1], 1, false);
	acct_dns_result_inject(0, &res);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_is(0, "family", "ipv6"));
	fam_answer(&res, &both[0], 1, false);
	acct_dns_result_inject(1, &res);
	CHECK(fam_wait(1, "state", "registered"));
	CHECK(fam_is(1, "family", "ipv4"));

	CASE(fam_flip_titles[t++]);
	qmark = sip_dns_stat(acct_dns())->queries;
	status46_inject(V6_DEV2, NULL, ROWS(gua));
	CHECK(net_key_is("address6", "2001:db8:40::5"));
	CHECK(fam_is(0, "state", "refreshing"));
	CHECK(fam_is(1, "state", "resolving"));
	CHECK_UINT(sip_dns_stat(acct_dns())->queries, qmark + 1);
	CHECK_UINT(fam_drain_user(fam_fd6, "REGISTER sip:"), 1);

	CASE(fam_flip_titles[t++]);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));
	status6_inject(V6_DEV2, NULL, 0);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));
	qmark = sip_dns_stat(acct_dns())->queries;
	status46_inject(V6_DEV2, "192.0.2.44", ROWS(gua));
	CHECK(net_key_is("address", "192.0.2.44"));
	CHECK(net_key_is("address6", "2001:db8:40::5"));
	/* the IPv4 address of the IPv4 line moved too, and the preference move
	 * still wins: only a resolution gives it a target of the new family */
	CHECK(fam_is(0, "state", "refreshing"));
	CHECK(fam_is(1, "state", "resolving"));
	CHECK_UINT(sip_dns_stat(acct_dns())->queries, qmark + 1);
	CHECK_UINT(fam_drain_user(fam_fd6, "REGISTER sip:"), 1);

	parked = none;
	snprintf(parked.local_interface, sizeof(parked.local_interface),
		 "wan2");
	core_reload(&live, &parked);
	for (i = 0; i < 400 && status_line0(NULL, 0); i++)
		uloop_run_timeout(20);
	CHECK_UINT(status_line0(NULL, 0), 0);
	accounts_hal_features_set(0);

	uloop_fd_delete(&fam_ufd4);
	uloop_fd_delete(&fam_ufd6);
	close(fam_fd4);
	close(fam_fd6);
	fam_fd4 = -1;
	fam_fd6 = -1;
	uloop_done();
}

static const char *const fam_stream_titles[] = {
	"stream: a tcp line comes up on a listener of its own family",
	"stream: an IPv6 target takes a listener of the new family",
	"stream: and the REGISTER of the move leaves over IPv6",
	"stream: a listener the new family cannot bind leaves the line alone",
	"stream: a gated line asked for an IPv4 host toward IPv6 gets none",
};

/*
 * A stream line moves family as a datagram line does, and its listener moves
 * with it. The identity of a listener carries the family, so the line takes
 * one in the new family and gives up the old one (sip/stream.h). The peers
 * here accept the connection and answer nothing. The cases read the listener
 * of the line and the family of its outgoing connection, not a registration.
 */
static void test_family_stream(void)
{
	static struct cfg live;
	static struct cfg none;
	static struct cfg parked;
	struct sip_dns_result res;
	struct fam_target v6only[1];
	struct fam_target v4only[1];
	struct sockaddr_storage peer;
	socklen_t plen;
	char why[128];
	char contact[192];
	uint16_t tcp4 = 0, tcp6 = 0, local_port;
	int fd4 = -1, fd6 = -1, held = -1, taken;
	int probe4 = -1, probe6 = -1;
	int rc;
	unsigned int i;
	unsigned int t = 0;
	bool v6 = tu_v6_loopback(why, sizeof(why));

	if (!v6) {
		fam_class_skip(ROWS(fam_stream_titles), why);
		return;
	}

	CASE(fam_stream_titles[t++]);
	rc = uloop_init();
	CHECK(rc == 0);
	if (rc != 0)
		goto out;

	fd4 = tcp_peer_open(AF_INET, &tcp4);
	fd6 = tcp_peer_open(AF_INET6, &tcp6);
	local_port = tcp_port_hold(&probe4, &probe6);
	CHECK(fd4 >= 0 && fd6 >= 0 && local_port != 0);
	if (fd4 < 0 || fd6 < 0 || !local_port)
		goto out;

	v4only[0].host = "127.0.0.1";
	v4only[0].port = tcp4;
	v6only[0].host = "::1";
	v6only[0].port = tcp6;

	event_test_mode(true);
	fw_test_mode(true);
	accounts_hal_features_set(HAL_FEAT_IPV6_MEDIA);

	base_default();
	live = base;
	live.channels = 1;
	live.ipv6_enabled = true;
	snprintf(live.transport, sizeof(live.transport), "tcp");
	fam_channel(&live, 0, "127.0.0.1", tcp4, local_port);
	none = live;
	none.channels = 0;

	warn_capture_begin();
	CHECK(warn_file != NULL && warn_stderr_fd >= 0);
	close(probe4);
	close(probe6);
	core_start(&live);
	probe4 = -1;
	probe6 = -1;
	warn_stderr_restore();
	if (!status_line0(NULL, 0)) {
		printf("    stream startup failed: local=%u peer4=%u peer6=%u\n",
		       local_port, tcp4, tcp6);
		warn_capture_print();
	}
	warn_capture_end();
	CHECK_UINT(status_line0(NULL, 0), 1);
	CHECK(fam_is(0, "family", "ipv4"));
	CHECK(fam_is(0, "listener", "1"));
	CHECK(fam_is(0, "transport", "tcp"));
	/* the answer of its own literal query builds the registration, and the
	 * Contact the cases below read comes with it */
	CHECK(fam_wait(0, "state", "trying"));
	CHECK(fam_has(0, "contact", "@127.0.0.1:"));

	CASE(fam_stream_titles[t++]);
	CHECK(fam_key(0, "contact", contact, sizeof(contact)) != NULL);
	fam_answer(&res, v6only, 1, false);
	acct_dns_result_inject(0, &res);
	CHECK(fam_is(0, "family", "ipv6"));
	CHECK(fam_is(0, "listener", "1"));
	CHECK(fam_has(0, "contact", "@[::1]:"));

	CASE(fam_stream_titles[t++]);
	for (i = 0; i < 100; i++) {
		plen = sizeof(peer);
		taken = accept(fd6, (struct sockaddr *)&peer, &plen);
		if (taken >= 0)
			break;

		uloop_run_timeout(20);
	}
	CHECK(taken >= 0);
	if (taken >= 0)
		close(taken);

	/*
	 * The listener of the family the line would move to is the first thing
	 * the move opens, so a port another socket holds refuses the move
	 * before anything the far end sees has changed.
	 */
	CASE(fam_stream_titles[t++]);
	held = port_hold(AF_INET, SOCK_STREAM, local_port);
	CHECK(held >= 0);
	CHECK(fam_key(0, "contact", contact, sizeof(contact)) != NULL);
	warn_capture_begin();
	fam_answer(&res, v4only, 1, false);
	acct_dns_result_inject(0, &res);
	CHECK_UINT(warn_capture_count("listen on ipv4 tcp"), 1);
	warn_capture_end();
	CHECK(fam_is(0, "family", "ipv6"));
	CHECK(fam_is(0, "listener", "1"));
	CHECK(fam_is(0, "contact", contact));

	if (held >= 0)
		close(held);

	/*
	 * A line the gate holds in IPv4 asks for an IPv4 local host, and its
	 * IPv6 target has none to give: the probe answers in the family it is
	 * asked for, so the line keeps retrying the resolve, which is what an
	 * unreachable target has always done. The alternative would put an IPv6
	 * address in the Via and the Contact of an IPv4 listener.
	 */
	CASE(fam_stream_titles[t++]);
	accounts_hal_features_set(0);
	core_reload(&live, &none);
	core_reload(&none, &live);
	CHECK(fam_is(0, "family", "ipv4"));
	CHECK(fam_is(0, "state", "resolving"));
	fam_answer(&res, v6only, 1, false);
	acct_dns_result_inject(0, &res);
	CHECK(fam_is(0, "family", "ipv4"));
	CHECK(fam_is(0, "listener", "1"));
	CHECK(fam_is(0, "state", "resolving"));
	CHECK(fam_key(0, "contact", contact, sizeof(contact)) == NULL);

	parked = none;
	snprintf(parked.local_interface, sizeof(parked.local_interface),
		 "wan2");
	core_reload(&live, &parked);
	for (i = 0; i < 400 && status_line0(NULL, 0); i++)
		uloop_run_timeout(20);
	CHECK_UINT(status_line0(NULL, 0), 0);
	accounts_hal_features_set(0);

out:
	if (probe4 >= 0)
		close(probe4);
	if (probe6 >= 0)
		close(probe6);
	if (fd4 >= 0)
		close(fd4);
	if (fd6 >= 0)
		close(fd6);
	uloop_done();
}

/* ------------------------- the connection that is not the binding's */

/*
 * A stream registrar on the IPv4 loopback. It accepts one connection at a
 * time, answers every REGISTER that arrives on it 200 OK and counts what it
 * answered. The answer needs no SIP stack, for the reason registrar_answer()
 * states; the only difference is the transport, so the request arrives on a
 * connection and the answer goes back on the same one. A CRLF keepalive ping
 * carries no request line, so it is read and dropped.
 */
#define TCP_REG_MAX	2

struct tcp_reg {
	struct uloop_fd listen;
	struct uloop_fd conn;
	unsigned int answers;
	char last[2048];
};

static struct tcp_reg tcp_reg[TCP_REG_MAX];

static struct tcp_reg *tcp_reg_of(const struct uloop_fd *u)
{
	unsigned int i;

	for (i = 0; i < TCP_REG_MAX; i++) {
		if (&tcp_reg[i].listen == u || &tcp_reg[i].conn == u)
			return &tcp_reg[i];
	}

	return NULL;
}

static void tcp_reg_conn_close(struct tcp_reg *r)
{
	if (r->conn.fd < 0)
		return;

	uloop_fd_delete(&r->conn);
	close(r->conn.fd);
	r->conn.fd = -1;
}

static void tcp_reg_conn_cb(struct uloop_fd *u, unsigned int events)
{
	struct tcp_reg *r = tcp_reg_of(u);
	char via[640], from[256], to[256], callid[256], cseq[64];
	char resp[2048];
	ssize_t n;

	if (!r)
		return;

	n = recv(u->fd, r->last, sizeof(r->last) - 1, 0);
	if (n <= 0) {
		tcp_reg_conn_close(r);
		return;
	}

	r->last[(size_t)n] = 0;
	if (!req_line_get(r->last, "Via", via, sizeof(via)) ||
	    !req_line_get(r->last, "From", from, sizeof(from)) ||
	    !req_line_get(r->last, "To", to, sizeof(to)) ||
	    !req_line_get(r->last, "Call-ID", callid, sizeof(callid)) ||
	    !req_line_get(r->last, "CSeq", cseq, sizeof(cseq)))
		return;

	snprintf(resp, sizeof(resp),
		 "SIP/2.0 200 OK\r\n%s\r\n%s\r\n%s;tag=tcpreg\r\n%s\r\n%s\r\n"
		 "Content-Length: 0\r\n\r\n", via, from, to, callid, cseq);

	if (send(u->fd, resp, strlen(resp), MSG_NOSIGNAL) > 0)
		r->answers++;
}

static void tcp_reg_listen_cb(struct uloop_fd *u, unsigned int events)
{
	struct tcp_reg *r = tcp_reg_of(u);
	int taken = accept(u->fd, NULL, NULL);

	if (!r || taken < 0)
		return;

	/* one connection at a time: the line under test opens one per server */
	tcp_reg_conn_close(r);
	r->conn.fd = taken;
	r->conn.cb = tcp_reg_conn_cb;
	uloop_fd_add(&r->conn, ULOOP_READ);
}

static uint16_t tcp_reg_open(struct tcp_reg *r)
{
	struct sockaddr_in sa;
	socklen_t len = sizeof(sa);
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

	memset(r, 0, sizeof(*r));
	r->listen.fd = -1;
	r->conn.fd = -1;
	if (fd < 0)
		return 0;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
	    listen(fd, 4) < 0 ||
	    getsockname(fd, (struct sockaddr *)&sa, &len) < 0) {
		close(fd);
		return 0;
	}

	r->listen.fd = fd;
	r->listen.cb = tcp_reg_listen_cb;
	uloop_fd_add(&r->listen, ULOOP_READ);

	return ntohs(sa.sin_port);
}

static void tcp_reg_close(struct tcp_reg *r)
{
	tcp_reg_conn_close(r);
	if (r->listen.fd < 0)
		return;

	uloop_fd_delete(&r->listen);
	close(r->listen.fd);
	r->listen.fd = -1;
}

static const char *const conn_bind_titles[] = {
	"conn binding: a tcp line comes up on the first server",
	"conn binding: a connection toward another server is no reconnect",
};

/*
 * The keepalive round pings the sticky target. Under dns_cache_update_mode 0,
 * an answer that moves the selection sends nothing else. On a stream line,
 * that ping opens a connection to the new server while the binding stays on
 * the old one. A connection to a peer other than the server of the binding
 * does not belong to the registration, so it is no reconnect. The line keeps
 * its binding, posts no event and sends no REGISTER.
 */
static void test_conn_binding(void)
{
	static struct cfg live;
	static struct cfg none;
	static struct cfg parked;
	struct sip_dns_result res;
	struct fam_target second[1];
	uint16_t port_a, port_b, local_port;
	int probe4 = -1, probe6 = -1;
	unsigned int posts, answers;
	unsigned int i;
	unsigned int t = 0;

	CASE(conn_bind_titles[t++]);
	CHECK(uloop_init() == 0);
	port_a = tcp_reg_open(&tcp_reg[0]);
	port_b = tcp_reg_open(&tcp_reg[1]);
	local_port = tcp_port_hold(&probe4, &probe6);
	CHECK(port_a && port_b && local_port);
	if (!port_a || !port_b || !local_port)
		goto out;

	event_test_mode(true);
	fw_test_mode(true);

	base_default();
	live = base;
	live.channels = 1;
	snprintf(live.transport, sizeof(live.transport), "tcp");
	/* mode 0: the answer re-points the selection and sends nothing, so the
	 * ping is the only thing that reaches the new server */
	live.dns_cache_update_mode = false;
	live.keepalive_enabled = true;
	live.keepalive_interval = 30;
	live.restrict_source = 0;
	fam_channel(&live, 0, "127.0.0.1", port_a, local_port);
	none = live;
	none.channels = 0;

	close(probe4);
	close(probe6);
	probe4 = -1;
	probe6 = -1;
	core_start(&live);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK_UINT(tcp_reg[0].answers, 1);
	event_test_reap(0, 0);
	posts = event_test_posts();

	CASE(conn_bind_titles[t++]);
	answers = tcp_reg[0].answers;
	second[0].host = "127.0.0.1";
	second[0].port = port_b;
	fam_answer(&res, second, 1, true);
	acct_dns_result_inject(0, &res);
	for (i = 0; i < 40; i++)
		uloop_run_timeout(20);
	/* the ping opened a connection to the new server, which is what the
	 * rule has to see before it can answer "no reconnect" */
	CHECK(tcp_reg[1].conn.fd >= 0);
	CHECK(fam_is(0, "state", "registered"));
	CHECK_UINT(event_test_posts(), posts);
	CHECK_UINT(tcp_reg[0].answers, answers);
	CHECK_UINT(tcp_reg[1].answers, 0);

	parked = none;
	snprintf(parked.local_interface, sizeof(parked.local_interface),
		 "wan2");
	core_reload(&live, &parked);
	for (i = 0; i < 400 && status_line0(NULL, 0); i++)
		uloop_run_timeout(20);
	CHECK_UINT(status_line0(NULL, 0), 0);

out:
	if (probe4 >= 0)
		close(probe4);
	if (probe6 >= 0)
		close(probe6);
	tcp_reg_close(&tcp_reg[0]);
	tcp_reg_close(&tcp_reg[1]);
	uloop_done();
}

/* ------------------------------------ the CRLF round of a mode 2 udp line */

static struct blob_attr *table_find(struct blob_attr *tbl, const char *name)
{
	struct blob_attr *cur;
	size_t rem;

	if (!tbl)
		return NULL;

	blobmsg_for_each_attr(cur, tbl, rem) {
		if (!strcmp(blobmsg_name(cur), name))
			return cur;
	}

	return NULL;
}

/* one counter of the keepalive table of one line (account_ka_dump), 0 when
 * the line or the table is absent */
/* a u32 member of the keepalive table, the shape interval and ct_bound use */
static uint32_t ka_u32(unsigned int line, const char *key)
{
	struct blob_buf b;
	struct blob_attr *cur;
	struct blob_attr *entry;
	struct blob_attr *arr = NULL;
	uint32_t got = 0;
	size_t rem;

	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	accounts_status_dump(&b, -1);

	blob_for_each_attr(cur, b.head, rem) {
		if (!strcmp(blobmsg_name(cur), "lines"))
			arr = cur;
	}

	if (!arr) {
		blob_buf_free(&b);

		return 0;
	}

	blobmsg_for_each_attr(entry, arr, rem) {
		struct blob_attr *f;

		if (!table_u32_is(entry, "line", line))
			continue;

		f = table_find(table_find(entry, "keepalive"), key);
		if (f && blobmsg_type(f) == BLOBMSG_TYPE_INT32)
			got = blobmsg_get_u32(f);
	}

	blob_buf_free(&b);

	return got;
}

/* how many entries a named array of the line's status entry holds */
static unsigned int status_array_count(unsigned int line, const char *key)
{
	struct blob_buf b;
	struct blob_attr *cur;
	struct blob_attr *entry;
	struct blob_attr *arr = NULL;
	unsigned int count = 0;
	size_t rem;

	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	accounts_status_dump(&b, -1);

	blob_for_each_attr(cur, b.head, rem) {
		if (!strcmp(blobmsg_name(cur), "lines"))
			arr = cur;
	}

	if (!arr) {
		blob_buf_free(&b);

		return 0;
	}

	blobmsg_for_each_attr(entry, arr, rem) {
		struct blob_attr *f, *item;
		size_t inner;

		if (!table_u32_is(entry, "line", line))
			continue;

		f = table_find(entry, key);
		if (!f)
			continue;

		blobmsg_for_each_attr(item, f, inner)
			count++;
	}

	blob_buf_free(&b);

	return count;
}

static uint64_t ka_stat(unsigned int line, const char *key)
{
	struct blob_buf b;
	struct blob_attr *cur;
	struct blob_attr *entry;
	struct blob_attr *arr = NULL;
	uint64_t got = 0;
	size_t rem;

	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	accounts_status_dump(&b, -1);

	blob_for_each_attr(cur, b.head, rem) {
		if (!strcmp(blobmsg_name(cur), "lines"))
			arr = cur;
	}

	if (!arr) {
		blob_buf_free(&b);

		return 0;
	}

	blobmsg_for_each_attr(entry, arr, rem) {
		struct blob_attr *f;

		if (!table_u32_is(entry, "line", line))
			continue;

		f = table_find(table_find(entry, "keepalive"), key);
		if (f && blobmsg_type(f) == BLOBMSG_TYPE_INT64)
			got = blobmsg_get_u64(f);
	}

	blob_buf_free(&b);

	return got;
}

/* passes until the counter reaches the mark, or until the bound gives up */
static bool ka_stat_wait(unsigned int line, const char *key, uint64_t want)
{
	unsigned int i;

	for (i = 0; i < 400; i++) {
		if (ka_stat(line, key) >= want)
			return true;

		uloop_run_timeout(20);
	}

	return ka_stat(line, key) >= want;
}

/*
 * Whether a CRLF ping waits at a socket nobody else reads, and everything
 * waiting there is consumed: the responses of a leg land on the same socket,
 * so the answer is about the whole queue and not its head.
 */
static bool ping_waiting(int fd)
{
	char buf[2048];
	bool found = false;
	ssize_t n;

	while ((n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT)) >= 0) {
		if (n == SIP_KA_PING_LEN &&
		    !memcmp(buf, SIP_KA_PING, SIP_KA_PING_LEN))
			found = true;
	}

	return found;
}

/*
 * An INVITE with an offer to the user of line 0, sent from fd to the line's
 * local port. id is the Call-ID, the branch and the From tag, so two calls from
 * one socket stay apart, and extra is header text or NULL. rport makes the
 * responses, and with them the destination the leg's dialog records
 * (sip_dialog_uas_create), name fd's own port.
 */
static bool loopback_invite_send(int fd, uint16_t from_port, uint16_t local_port,
				 const char *id, const char *extra)
{
	static const char body[] =
		"v=0\r\n"
		"o=- 42 42 IN IP4 127.0.0.1\r\n"
		"s=-\r\n"
		"c=IN IP4 127.0.0.1\r\n"
		"t=0 0\r\n"
		"m=audio 30000 RTP/AVP 0 8 101\r\n"
		"a=rtpmap:0 PCMU/8000\r\n"
		"a=rtpmap:8 PCMA/8000\r\n"
		"a=rtpmap:101 telephone-event/8000\r\n";
	char text[2048];
	struct sockaddr_in to;
	int n;

	n = snprintf(text, sizeof(text),
		     "INVITE sip:5551000@127.0.0.1 SIP/2.0\r\n"
		     "Via: SIP/2.0/UDP 127.0.0.1:%u;rport"
		     ";branch=z9hG4bK-%s\r\n"
		     "Max-Forwards: 70\r\n"
		     "From: <sip:1002@127.0.0.1>;tag=%s\r\n"
		     "To: <sip:5551000@127.0.0.1>\r\n"
		     "Call-ID: %s@127.0.0.1\r\n"
		     "CSeq: 1 INVITE\r\n"
		     "Contact: <sip:1002@127.0.0.1:%u>\r\n"
		     "%s"
		     "Content-Type: application/sdp\r\n"
		     "Content-Length: %u\r\n"
		     "\r\n%s",
		     from_port, id, id, id, from_port, extra ? extra : "",
		     (unsigned int)strlen(body), body);
	if (n < 0 || (size_t)n >= sizeof(text))
		return false;

	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	to.sin_port = htons(local_port);

	return sendto(fd, text, (size_t)n, 0, (const struct sockaddr *)&to,
		      sizeof(to)) == n;
}

static const char *const round_titles[] = {
	"round: a mode 2 line comes up and its first answer primes the round",
	"round: a mixed answer pings the targets of the line's family only",
	"round: and keeps the sticky selection where it was",
	"round: a moved answer pings at once and a redraw does not",
	"round: a live dialog destination outside the set joins the round",
	"round: and leaves it when the call ends",
	"round: a bound that moves under the line re-arms inside the new one",
	"round: an address change primes the udp line",
	"round: a live move to mode 1 stops the round and the move back "
	"re-arms it",
};

/*
 * The round of a restrict_source 2 udp line on the wire. The fixture has real
 * sockets, a loopback registrar of each family, a second IPv4 target and a
 * proxy that the answer never named. The test hook fixes the bound, so the
 * conntrack values of the host play no part. The changed answers that prepare the round
 * carry no re-register (dns_cache_update_mode off), so a REGISTER never sits
 * beside a ping. The mixed answer binds ::1, so the class skips by name
 * without an IPv6 loopback.
 */
static void test_refresher_round(void)
{
	static struct cfg live;
	static struct cfg mode1;
	static struct cfg none;
	static struct cfg parked;
	struct sip_dns_result res;
	struct fam_target mixed[3];
	struct fam_target v4pair[2];
	struct fam_target v4only[1];
	char why[128];
	char wire[2048];
	uint64_t sent;
	uint16_t reg4, reg6, second_port, proxy_port, local_port;
	int second_fd = -1;
	int proxy_fd = -1;
	unsigned int answers;
	unsigned int i;
	unsigned int t = 0;
	bool v6 = tu_v6_loopback(why, sizeof(why));

	if (!v6) {
		fam_class_skip(round_titles,
			       (unsigned int)(sizeof(round_titles) /
					      sizeof(round_titles[0])),
			       why);
		return;
	}

	CASE(round_titles[t++]);
	CHECK(uloop_init() == 0);
	reg4 = udp_loopback_open(&fam_fd4);
	reg6 = udp6_loopback_open(&fam_fd6);
	second_port = udp_loopback_open(&second_fd);
	proxy_port = udp_loopback_open(&proxy_fd);
	local_port = fam_free_port();
	CHECK(reg4 && reg6 && second_port && proxy_port && local_port);
	if (!reg4 || !reg6)
		return;

	fam_ufd4.fd = fam_fd4;
	fam_ufd4.cb = fam_cb4;
	fam_ufd6.fd = fam_fd6;
	fam_ufd6.cb = fam_cb6;
	CHECK(uloop_fd_add(&fam_ufd4, ULOOP_READ) == 0);
	CHECK(uloop_fd_add(&fam_ufd6, ULOOP_READ) == 0);

	v4only[0].host = "127.0.0.1";
	v4only[0].port = reg4;
	mixed[0] = v4only[0];
	mixed[1].host = "127.0.0.1";
	mixed[1].port = second_port;
	mixed[2].host = "::1";
	mixed[2].port = reg6;
	v4pair[0] = mixed[0];
	v4pair[1] = mixed[1];

	event_test_mode(true);
	fw_test_mode(true);
	accounts_hal_features_set(HAL_FEAT_IPV6_MEDIA);
	net_ct_udp_bound_test_set(60, 180);

	base_default();
	live = base;
	live.channels = 1;
	live.ipv6_enabled = true;
	live.restrict_source = 2;
	live.dns_cache_update_mode = false;
	fam_channel(&live, 0, "127.0.0.1", reg4, local_port);
	/* the ordinary T1, so no REGISTER retransmission of the line's own
	 * lands beside the datagrams the cases read */
	live.channel[0].timers.t1 = 500;
	live.channel[0].timers.t2 = 4000;
	live.channel[0].timers.t4 = 5000;
	none = live;
	none.channels = 0;

	core_start(&live);
	CHECK(fam_count_wait(&fam_regs4, 1));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK_UINT(ka_stat(0, "crlf_sent"), 1);
	CHECK_UINT(ka_stat(0, "crlf_skipped"), 0);

	/*
	 * The answer keeps the sticky target and adds one of each family. The
	 * round runs from the inject itself, so both counts are read without a
	 * pass: the second IPv4 target gets its ping, the IPv6 one gets none,
	 * and the line stays in the family of the target it is on.
	 */
	CASE(round_titles[t++]);
	sent = ka_stat(0, "crlf_sent");
	fam_answer(&res, mixed, 3, true);
	acct_dns_result_inject(0, &res);
	CHECK_UINT(ka_stat(0, "crlf_sent"), sent + 2);
	/* the socket refuses the other family, so a ping toward the IPv6
	 * target would count a failure and not a datagram */
	CHECK_UINT(ka_stat(0, "crlf_failed"), 0);
	CHECK(ping_waiting(second_fd));
	CHECK(!fam_peek(fam_fd6, wire, sizeof(wire)));
	CHECK(fam_is(0, "targets", "3"));

	CASE(round_titles[t++]);
	CHECK(fam_is(0, "family", "ipv4"));
	CHECK(fam_is(0, "registrar", "127.0.0.1"));
	CHECK(fam_is(0, "state", "registered"));

	/*
	 * The change signal is the line's own list, whatever the resolver's
	 * cache flag says: the shrink below carries changed false and still
	 * pings at once, the repeated answer carries true and pings nothing.
	 */
	CASE(round_titles[t++]);
	sent = ka_stat(0, "crlf_sent");
	fam_answer(&res, v4only, 1, false);
	acct_dns_result_inject(0, &res);
	CHECK_UINT(ka_stat(0, "crlf_sent"), sent + 1);
	CHECK(!ping_waiting(second_fd));
	fam_answer(&res, v4only, 1, true);
	acct_dns_result_inject(0, &res);
	CHECK_UINT(ka_stat(0, "crlf_sent"), sent + 1);
	CHECK(!ping_waiting(second_fd));

	/*
	 * A leg the call layer built from an INVITE off a proxy the answer
	 * never named: the dialog its 180 creates records that proxy as its
	 * destination, and the round pings it beside the target for as long as
	 * the leg lives. The call layer runs with no media ops and no FSM
	 * callbacks, so the ring and the reject are asked for here.
	 */
	CASE(round_titles[t++]);
	calls_init(NULL, NULL);
	calls_host_bind(proto_host_fake());
	calls_reload(&live);
	calls_hal_codecs_set(~0u);
	CHECK(loopback_invite_send(proxy_fd, proxy_port, local_port,
				   "proxy-call-1", NULL));
	for (i = 0; i < 50 && !calls_line_busy(0); i++)
		uloop_run_timeout(20);
	CHECK(calls_line_busy(0));
	/* the 180 the FSM would ask for is what creates the UAS dialog */
	CHECK(call_ring(0, 0));
	CHECK(calls_line_dialog_dst(0, 0) != NULL);
	CHECK(calls_line_dialog_dst(0, 1) == NULL);
	CHECK(!ping_waiting(proxy_fd));
	sent = ka_stat(0, "crlf_sent");
	/* the answer grows, so the round runs from the inject itself */
	fam_answer(&res, v4pair, 2, true);
	acct_dns_result_inject(0, &res);
	CHECK_UINT(ka_stat(0, "crlf_sent"), sent + 3);
	CHECK(ping_waiting(second_fd));
	CHECK(ping_waiting(proxy_fd));

	CASE(round_titles[t++]);
	CHECK(call_reject(0, 0, 486));
	for (i = 0; i < 50 && calls_line_dialog_dst(0, 0); i++)
		uloop_run_timeout(20);
	CHECK(calls_line_dialog_dst(0, 0) == NULL);
	CHECK(!ping_waiting(proxy_fd));
	sent = ka_stat(0, "crlf_sent");
	fam_answer(&res, v4only, 1, true);
	acct_dns_result_inject(0, &res);
	CHECK_UINT(ka_stat(0, "crlf_sent"), sent + 1);
	CHECK(!ping_waiting(proxy_fd));
	CHECK(!ping_waiting(second_fd));

	/*
	 * The bound moves under the running line, the way a conntrack reload
	 * moves it. The round the next inject primes reads the new bound at
	 * its index 0, says so, and re-arms one second out instead of thirty.
	 */
	CASE(round_titles[t++]);
	net_ct_udp_bound_test_set(2, 180);
	sent = ka_stat(0, "crlf_sent");
	warn_capture_begin();
	fam_answer(&res, v4pair, 2, true);	/* the growth primes the round */
	acct_dns_result_inject(0, &res);
	CHECK_UINT(warn_capture_count("conntrack udp timeout 60 s -> 2 s"), 1);
	CHECK_UINT(warn_capture_count("sip keepalive every 1 s"), 1);
	warn_capture_end();
	CHECK_UINT(ka_stat(0, "crlf_sent"), sent + 2);
	CHECK(ka_stat_wait(0, "crlf_sent", sent + 4));
	/* the bound goes back, and the round after the next re-read runs
	 * thirty seconds out again; the shrink then primes one more round and
	 * puts the list back to the single target the cases below expect */
	net_ct_udp_bound_test_set(60, 180);
	sent = ka_stat(0, "crlf_sent");
	CHECK(ka_stat_wait(0, "crlf_sent", sent + 2));
	sent = ka_stat(0, "crlf_sent");
	fam_answer(&res, v4only, 1, true);
	acct_dns_result_inject(0, &res);
	CHECK_UINT(ka_stat(0, "crlf_sent"), sent + 1);

	/*
	 * The IPv4 address moves. The rebind puts the socket on the new address
	 * and the round runs at once, ahead of the REGISTER that refreshes the
	 * binding of the line in place: the registrar reads the ping first.
	 */
	CASE(round_titles[t++]);
	uloop_run_timeout(20);
	uloop_run_timeout(20);
	CHECK(!fam_peek(fam_fd4, wire, sizeof(wire)));
	sent = ka_stat(0, "crlf_sent");
	status46_inject(V6_DEV, "192.0.2.66", NULL, 0);
	CHECK_UINT(ka_stat(0, "crlf_sent"), sent + 1);
	CHECK(fam_peek(fam_fd4, wire, sizeof(wire)));
	CHECK(!strcmp(wire, SIP_KA_PING));
	CHECK(fam_is(0, "state", "refreshing"));
	CHECK(fam_wait(0, "state", "registered"));

	/*
	 * The mode moves live. Its apply class is LIVE, so the running
	 * keepalive is re-armed and nothing rebuilds: under mode 1 the
	 * configured pair stands, which is off here, and a changed answer
	 * then primes nothing. The bound moves while the line sits in mode 1,
	 * where nothing reads it; the move back takes it fresh, so the round
	 * it sends in the same frame finds nothing left to re-read, and the
	 * timer behind that round sits inside the bound of the moment.
	 */
	CASE(round_titles[t++]);
	mode1 = live;
	mode1.restrict_source = 1;
	answers = fam_regs4;
	core_reload(&live, &mode1);
	CHECK(fam_is(0, "state", "registered"));
	CHECK_UINT(fam_regs4, answers);
	sent = ka_stat(0, "crlf_sent");
	fam_answer(&res, v4only, 1, true);
	acct_dns_result_inject(0, &res);
	CHECK_UINT(ka_stat(0, "crlf_sent"), sent);
	net_ct_udp_bound_test_set(2, 180);
	warn_capture_begin();
	core_reload(&mode1, &live);
	CHECK_UINT(warn_capture_count("conntrack udp timeout 60 s -> 2 s"), 0);
	warn_capture_end();
	CHECK(fam_is(0, "state", "registered"));
	CHECK_UINT(fam_regs4, answers);
	CHECK_UINT(ka_stat(0, "crlf_sent"), sent + 1);
	CHECK(ka_stat_wait(0, "crlf_sent", sent + 2));
	net_ct_udp_bound_test_set(60, 180);
	sent = ka_stat(0, "crlf_sent");
	CHECK(ka_stat_wait(0, "crlf_sent", sent + 1));

	parked = none;
	snprintf(parked.local_interface, sizeof(parked.local_interface),
		 "wan2");
	core_reload(&live, &parked);
	for (i = 0; i < 400 && status_line0(NULL, 0); i++)
		uloop_run_timeout(20);
	CHECK_UINT(status_line0(NULL, 0), 0);
	calls_done();
	accounts_hal_features_set(0);
	net_ct_udp_bound_test_set(0, 0);

	uloop_fd_delete(&fam_ufd4);
	uloop_fd_delete(&fam_ufd6);
	close(fam_fd4);
	close(fam_fd6);
	close(second_fd);
	close(proxy_fd);
	fam_fd4 = -1;
	fam_fd6 = -1;
	uloop_done();
}

/*
 * A registrar pool that answers one address per query and rotates it, on two
 * answering loopback registrars. The rotation must move the registration with
 * no down edge. Each move sends one REGISTER on the standing Call-ID, and
 * nothing more goes to the member that left. No event post follows the first
 * up.
 */
static void test_rotation_events(void)
{
	static const uint16_t best_first[2] = { 10, 20 };
	static struct cfg live;
	static struct cfg fast;
	static struct cfg nomove;
	static struct cfg none;
	struct sip_dns_result res;
	struct fam_target pool[1];
	struct fam_target pair[2];
	char call_id[192];
	char moved_id[192];
	uint16_t reg_a, reg_b, local_port, dead_port;
	int fd_b = -1;
	unsigned int posts, answers, mark;
	unsigned int i;

	CASE("rotation: a line comes up on the first pool member");
	CHECK(uloop_init() == 0);
	reg_a = udp_loopback_open(&fam_fd4);
	reg_b = udp_loopback_open(&fd_b);
	local_port = fam_free_port();
	dead_port = fam_free_port();
	CHECK(reg_a && reg_b && local_port && dead_port);
	if (!reg_a || !reg_b)
		return;

	/* the counters are cumulative across the classes of this binary */
	fam_regs4 = 0;
	fam_regs4b = 0;
	fam_ufd4.fd = fam_fd4;
	fam_ufd4.cb = fam_cb4;
	fam_ufd4b.fd = fd_b;
	fam_ufd4b.cb = fam_cb4b;
	CHECK(uloop_fd_add(&fam_ufd4, ULOOP_READ) == 0);
	CHECK(uloop_fd_add(&fam_ufd4b, ULOOP_READ) == 0);

	event_test_mode(true);
	fw_test_mode(true);

	base_default();
	live = base;
	live.channels = 1;
	/* mode 0 with the keepalive off: nothing but REGISTERs reaches the
	 * registrar sockets, so registrar_last always holds the last one */
	live.restrict_source = 0;
	live.dns_cache_update_mode = true;
	fam_channel(&live, 0, "127.0.0.1", reg_a, local_port);
	live.channel[0].timers.t1 = 500;
	live.channel[0].timers.t2 = 4000;
	live.channel[0].timers.t4 = 5000;
	none = live;
	none.channels = 0;

	core_start(&live);
	CHECK(fam_count_wait(&fam_regs4, 1));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(req_line_get(registrar_last, "Call-ID", call_id, sizeof(call_id)));
	event_test_reap(0, 0);
	posts = event_test_posts();

	CASE("rotation: the moved answer re-registers with no down edge");
	answers = fam_regs4;
	pool[0].host = "127.0.0.1";
	pool[0].port = reg_b;
	fam_answer(&res, pool, 1, true);
	acct_dns_result_inject(0, &res);
	CHECK(fam_count_wait(&fam_regs4b, 1));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK_UINT(event_test_posts(), posts);
	CHECK(req_line_get(registrar_last, "Call-ID", moved_id, sizeof(moved_id)));
	CHECK(!strcmp(moved_id, call_id));

	/*
	 * The REGISTER to the new member went first and its 2xx moved the
	 * binding. The Expires 0 that follows takes the binding at the member
	 * the line left away, on the same Call-ID, and the line never reports
	 * itself down for it.
	 */
	CASE("rotation: the 200 is followed by an Expires 0 to the old member");
	CHECK(fam_count_wait(&fam_regs4, answers + 1));
	CHECK(strstr(registrar_last, "Expires: 0") != NULL);
	CHECK(req_line_get(registrar_last, "Call-ID", moved_id,
			   sizeof(moved_id)));
	CHECK(!strcmp(moved_id, call_id));
	CHECK(fam_is(0, "state", "registered"));
	CHECK_UINT(event_test_posts(), posts);

	CASE("rotation: the next move rides the same registration");
	answers = fam_regs4;
	pool[0].port = reg_a;
	fam_answer(&res, pool, 1, true);
	acct_dns_result_inject(0, &res);
	CHECK(fam_count_wait(&fam_regs4, answers + 1));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK_UINT(event_test_posts(), posts);
	CHECK(req_line_get(registrar_last, "Call-ID", moved_id, sizeof(moved_id)));
	CHECK(!strcmp(moved_id, call_id));

	/*
	 * The order the move runs in. The primary is dead, so the REGISTER a
	 * better priority earns it times out; the binding at the secondary
	 * never moved, so the line stays registered, sends nothing to the
	 * secondary and posts no event. A removal that ran first would take
	 * the registration down for the length of that transaction.
	 */
	CASE("move: a dead primary at a better priority leaves the "
	     "secondary's binding alone and posts no event");
	/* a timer move is the live apply class, so the line keeps its binding
	 * and the transaction to the dead member ends inside this window */
	fast = live;
	fast.channel[0].timers.t1 = 10;
	fast.channel[0].timers.t2 = 40;
	fast.channel[0].timers.t4 = 50;
	core_reload(&live, &fast);
	CHECK(fam_wait(0, "state", "registered"));
	/* the Expires 0 that the move above sends to the member it left ends
	 * first, so the move below does not wait behind it */
	for (i = 0; i < 20; i++)
		uloop_run_timeout(20);
	answers = fam_regs4;
	pair[0].host = "127.0.0.1";
	pair[0].port = dead_port;
	pair[1].host = "127.0.0.1";
	pair[1].port = reg_a;
	fam_answer(&res, pair, 2, true);
	fam_answer_prio(&res, best_first);
	acct_dns_result_inject(0, &res);
	CHECK(fam_is(0, "state", "refreshing"));
	CHECK(fam_wait(0, "state", "registered"));
	/* a REGISTER sent at the rollback needs a pass to reach the registrar
	 * and be counted below */
	fam_pump_ms(100);
	CHECK_UINT(event_test_posts(), posts);
	CHECK_UINT(fam_regs4, answers);
	core_reload(&fast, &live);
	CHECK(fam_wait(0, "state", "registered"));

	/*
	 * With dns_move_unregister off the binding the move left lapses on its
	 * own, which is the behaviour before the option existed. The reload
	 * rebuilds the line, so the Call-ID below is a fresh one.
	 */
	CASE("rotation: the option off leaves the member the line left alone");
	nomove = live;
	nomove.dns_move_unregister = false;
	core_reload(&live, &nomove);
	CHECK(fam_wait(0, "state", "registered"));
	for (i = 0; i < 20; i++)
		uloop_run_timeout(20);
	event_test_reap(0, 0);
	posts = event_test_posts();
	answers = fam_regs4;
	mark = fam_regs4b;
	pool[0].port = reg_b;
	fam_answer(&res, pool, 1, true);
	acct_dns_result_inject(0, &res);
	CHECK(fam_count_wait(&fam_regs4b, mark + 1));
	for (i = 0; i < 20; i++)
		uloop_run_timeout(20);
	CHECK_UINT(fam_regs4, answers);
	CHECK(fam_is(0, "state", "registered"));
	CHECK(!strstr(registrar_last, "Expires: 0"));
	CHECK_UINT(event_test_posts(), posts);

	core_reload(&nomove, &none);
	for (i = 0; i < 400 && status_line0(NULL, 0); i++)
		uloop_run_timeout(20);
	CHECK_UINT(status_line0(NULL, 0), 0);

	uloop_fd_delete(&fam_ufd4);
	uloop_fd_delete(&fam_ufd4b);
	close(fam_fd4);
	close(fd_b);
	fam_fd4 = -1;
	uloop_done();
}

/*
 * The literal guard of the failure-driven re-resolve. The answer of a literal
 * proxy cannot move, so a failed line on one asks DNS nothing, however often
 * the retry ladder fails. The name path cannot run here, because the host
 * suite has no DNS server. The pure exhausted cases check the predicate, and
 * the bench rotation checks cover the name path.
 */
static void test_fail_resolve(void)
{
	static struct cfg live;
	static struct cfg none;
	uint64_t queries;
	uint16_t dead_port, local_port;
	unsigned int i;

	CASE("resolve again: a failed literal proxy never re-resolves");
	CHECK(uloop_init() == 0);
	dead_port = fam_free_port();
	local_port = fam_free_port();
	CHECK(dead_port && local_port);

	event_test_mode(true);
	fw_test_mode(true);

	base_default();
	live = base;
	live.channels = 1;
	fam_channel(&live, 0, "127.0.0.1", dead_port, local_port);
	none = live;
	none.channels = 0;

	/* the resolver stat is cumulative across the classes of this binary,
	 * so every read below is a delta from here */
	queries = acct_dns()->stat.queries;
	core_start(&live);
	for (i = 0; i < 400 && !fam_is(0, "status", "failed"); i++)
		uloop_run_timeout(20);
	CHECK(fam_is(0, "status", "failed"));

	/* the 1 s retry ladder fails more attempts in this window; the one
	 * query is the resolve of the build */
	for (i = 0; i < 100; i++)
		uloop_run_timeout(20);
	CHECK_UINT((unsigned int)(acct_dns()->stat.queries - queries), 1);

	core_reload(&live, &none);
	for (i = 0; i < 400 && status_line0(NULL, 0); i++)
		uloop_run_timeout(20);
	CHECK_UINT(status_line0(NULL, 0), 0);
	uloop_done();
}

/*
 * One INVITE from fd to the line, and whether its first final answer starts
 * with want. A provisional answer is skipped, and so is a datagram of another
 * call: the socket also carries the retransmitted finals of the calls before
 * this one.
 */
static bool view_invite_answer(int fd, uint16_t from_port, uint16_t line_port,
			       const char *id, const char *extra,
			       const char *want)
{
	char buf[2048];
	unsigned int i;
	ssize_t n;

	if (!loopback_invite_send(fd, from_port, line_port, id, extra))
		return false;

	for (i = 0; i < 200; i++) {
		uloop_run_timeout(20);
		while ((n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT)) > 0) {
			buf[n] = 0;
			if (strncmp(buf, "SIP/2.0 ", 8) || buf[8] == '1' ||
			    !strstr(buf, id))
				continue;

			return !strncmp(buf, want, strlen(want));
		}
	}

	return false;
}

/* one count of the refused table of the line (call.c refused_dump), 0 while
 * the line turned nothing away */
static uint32_t refused_u32(unsigned int line, const char *key)
{
	struct blob_buf b;
	struct blob_attr *cur;
	struct blob_attr *f;
	uint32_t got = 0;
	size_t rem;

	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	calls_status_dump(&b, line);

	blob_for_each_attr(cur, b.head, rem) {
		if (strcmp(blobmsg_name(cur), "refused"))
			continue;

		f = table_find(cur, key);
		if (f && blobmsg_type(f) == BLOBMSG_TYPE_INT32)
			got = blobmsg_get_u32(f);
	}

	blob_buf_free(&b);

	return got;
}

/*
 * The view that the call layer reads for the seize and the dial, on a real
 * registration. The registrar grants 4 s, so the refresh leaves 2 s later
 * (sip_reg_refresh_secs) and the case can hold it in flight. A 403 then ends
 * the binding. Two calls ring before it, so the INVITEs after it find both
 * slots busy. Every INVITE comes from a socket of its own, so the registrar
 * socket carries only the REGISTERs that the cases read.
 */
static void test_view_registered(void)
{
	static struct cfg live;
	static struct cfg rereg;
	static struct cfg none;
	struct acct_call_view view;
	uint16_t reg_port, local_port, call_port;
	unsigned int i, slot;
	int fd = -1, call_fd = -1;

	CASE("view: a line that waits for its first answer has a target and no registration");
	CHECK(uloop_init() == 0);
	reg_port = udp_loopback_open(&fd);
	local_port = fam_free_port();
	CHECK(reg_port && local_port);

	event_test_mode(true);
	fw_test_mode(true);

	base_default();
	live = base;
	live.channels = 1;
	fam_channel(&live, 0, "127.0.0.1", reg_port, local_port);
	none = live;
	none.channels = 0;

	core_start(&live);
	CHECK(fam_inflight_wait(fd));
	CHECK(account_call_view(0, &view));
	CHECK(view.dst_valid);
	CHECK(!view.registered);

	CASE("view: the 200 gives the line a live registration");
	CHECK(fam_refuse(fd, "5551000", "200 OK", "Expires: 4"));
	CHECK(fam_wait(0, "status", "registered"));
	CHECK(account_call_view(0, &view));
	CHECK(view.registered);

	/* the retransmissions of the first REGISTER that arrived before its
	 * answer, so the next datagram is the refresh */
	fam_drain(fd);

	CASE("intake: two calls ring on a line with a live registration");
	calls_init(NULL, NULL);
	calls_host_bind(proto_host_fake());
	calls_reload(&live);
	calls_hal_codecs_set(~0u);
	call_port = udp_loopback_open(&call_fd);
	CHECK(call_port);
	CHECK(loopback_invite_send(call_fd, call_port, local_port, "view-busy-1",
				   NULL));
	CHECK(loopback_invite_send(call_fd, call_port, local_port, "view-busy-2",
				   NULL));
	for (i = 0; i < 50 && call_slot_free(0, &slot); i++)
		uloop_run_timeout(20);
	CHECK(!call_slot_free(0, &slot));

	CASE("intake: a registered line with both slots busy answers 486 and "
	     "counts it as busy");
	CHECK(view_invite_answer(call_fd, call_port, local_port, "view-486", NULL,
				 "SIP/2.0 486"));
	CHECK_UINT(refused_u32(0, "busy"), 1);
	CHECK_UINT(refused_u32(0, "last_code"), 486);

	CASE("view: a refresh in flight keeps the registration live");
	CHECK(fam_inflight_wait(fd));
	CHECK(fam_is(0, "state", "refreshing"));
	CHECK(account_call_view(0, &view));
	CHECK(view.registered);

	CASE("view: a refused refresh leaves the target and no registration");
	CHECK(fam_refuse(fd, "5551000", "403 Forbidden", NULL));
	CHECK(fam_wait(0, "status", "failed"));
	CHECK(account_call_view(0, &view));
	CHECK(view.dst_valid);
	CHECK(!view.registered);

	/* with both slots busy the call layer never sees the INVITE, so only
	 * the order of the request intake tells 480 from 486 (account.c
	 * account_request_recv) */
	CASE("intake: with both slots busy, a new INVITE to a line with no live "
	     "registration is answered 480, not 486");
	CHECK(view_invite_answer(call_fd, call_port, local_port, "view-480", NULL,
				 "SIP/2.0 480"));

	CASE("intake: that 480 counts as unavailable");
	CHECK_UINT(refused_u32(0, "unavailable"), 1);
	CHECK_UINT(refused_u32(0, "last_code"), 480);

	/* the intake passes a Replaces INVITE on to call.c, whose match finds
	 * no call of the line */
	CASE("intake: a Replaces INVITE that names no call is answered 481, "
	     "not 480");
	CHECK(view_invite_answer(call_fd, call_port, local_port, "view-481",
				 "Replaces: view-none@127.0.0.1;to-tag=a;"
				 "from-tag=b\r\n", "SIP/2.0 481"));

	/* the intake passes an empty Replaces too, and call.c counts no value
	 * in it (account.h acct_invite_replaces) */
	CASE("intake: an empty Replaces header is no exemption and is answered "
	     "480");
	CHECK(view_invite_answer(call_fd, call_port, local_port, "view-empty",
				 "Replaces: \r\n", "SIP/2.0 480"));
	CHECK_UINT(refused_u32(0, "unavailable"), 2);

	CHECK(call_reject(0, 0, 486));
	CHECK(call_reject(0, 1, 486));
	for (i = 0; i < 50 && calls_line_busy(0); i++)
		uloop_run_timeout(20);
	CHECK(!calls_line_busy(0));

	CASE("view: a new password leaves no registration until the next 200");
	rereg = live;
	snprintf(rereg.channel[0].sip_secret, sizeof(rereg.channel[0].sip_secret),
		 "changed");
	/* the retransmissions of the refused refresh, so the next datagram is
	 * the REGISTER of the new password */
	fam_drain(fd);
	core_reload(&live, &rereg);
	CHECK(fam_inflight_wait(fd));
	CHECK(account_call_view(0, &view));
	CHECK(!view.registered);
	CHECK(fam_refuse(fd, "5551000", "200 OK", "Expires: 3600"));
	CHECK(fam_wait(0, "status", "registered"));
	CHECK(account_call_view(0, &view));
	CHECK(view.registered);

	core_reload(&rereg, &none);
	for (i = 0; i < 400 && status_line0(NULL, 0); i++)
		uloop_run_timeout(20);
	CHECK_UINT(status_line0(NULL, 0), 0);
	close(call_fd);
	close(fd);
	uloop_done();
}

/*
 * The linger membership of the round, on real loopback sockets. A rotated
 * pool member keeps its ping for the window, and both members expire once
 * they stay out of the answers. A live change to linger 0 gives the
 * replace-at-once shape, and a dialog destination that the retention covers
 * gets one ping. The registrar sockets consume their pings in their answer
 * callback, so the counts show the result, and the plain sockets carry the
 * checks per destination. dns_cache_update_mode is off, so no REGISTER
 * arrives beside the counted datagrams.
 */
static void test_linger_round(void)
{
	static struct cfg live;
	static struct cfg off;
	static struct cfg none;
	struct sip_dns_result res;
	struct fam_target pool[1];
	uint64_t sent;
	uint16_t reg_a, port_b, port_c, local_port;
	int fd_b = -1;
	int fd_c = -1;
	unsigned int i;

	CASE("round: a static answer under linger pings nothing extra");
	CHECK(uloop_init() == 0);
	reg_a = udp_loopback_open(&fam_fd4);
	port_b = udp_loopback_open(&fd_b);
	port_c = udp_loopback_open(&fd_c);
	local_port = fam_free_port();
	CHECK(reg_a && port_b && port_c && local_port);

	fam_regs4 = 0;		/* the counter is cumulative across classes */
	fam_ufd4.fd = fam_fd4;
	fam_ufd4.cb = fam_cb4;
	CHECK(uloop_fd_add(&fam_ufd4, ULOOP_READ) == 0);

	event_test_mode(true);
	fw_test_mode(true);
	net_ct_udp_bound_test_set(60, 180);

	base_default();
	live = base;
	live.channels = 1;
	live.restrict_source = 2;
	live.dns_cache_update_mode = false;
	live.dns_linger = 2;
	fam_channel(&live, 0, "127.0.0.1", reg_a, local_port);
	live.channel[0].timers.t1 = 500;
	live.channel[0].timers.t2 = 4000;
	live.channel[0].timers.t4 = 5000;
	off = live;
	off.dns_linger = 0;
	none = live;
	none.channels = 0;

	core_start(&live);
	CHECK(fam_count_wait(&fam_regs4, 1));
	CHECK(fam_wait(0, "state", "registered"));

	/* a static answer keeps the retention equal to the target set: no
	 * move, no extra ping, the shipped-default no-op */
	pool[0].host = "127.0.0.1";
	pool[0].port = reg_a;
	sent = ka_stat(0, "crlf_sent");
	fam_answer(&res, pool, 1, false);
	acct_dns_result_inject(0, &res);
	CHECK_UINT(ka_stat(0, "crlf_sent"), sent);

	CASE("status: the cadence, the bound and both lists reach the dump");
	CHECK_UINT(ka_u32(0, "interval"), 30);
	CHECK_UINT(ka_u32(0, "ct_bound"), 60);
	CHECK_UINT(status_array_count(0, "resolved"), 1);
	CHECK_UINT(status_array_count(0, "linger"), 1);

	CASE("round: a rotated answer still pings the address that left it");
	pool[0].port = port_b;
	sent = ka_stat(0, "crlf_sent");
	fam_answer(&res, pool, 1, true);
	acct_dns_result_inject(0, &res);
	/* the new sticky member, and the one that left inside its window; the
	 * registrar socket eats the second ping, so the count carries it */
	CHECK_UINT(ka_stat(0, "crlf_sent"), sent + 2);
	CHECK(ping_waiting(fd_b));

	CASE("round: the linger window ends and the departed addresses go quiet");
	/* The pump is bounded by the clock the window runs on, because a loop
	 * pass can return early on an unrelated event. */
	{
		struct timespec ts;
		uint64_t now_ms, until_ms;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		until_ms = (uint64_t)ts.tv_sec * 1000 +
			   (uint64_t)ts.tv_nsec / 1000000 + 2800;
		for (;;) {
			clock_gettime(CLOCK_MONOTONIC, &ts);
			now_ms = (uint64_t)ts.tv_sec * 1000 +
				 (uint64_t)ts.tv_nsec / 1000000;
			if (now_ms >= until_ms)
				break;
			uloop_run_timeout(20);
		}
	}
	pool[0].port = port_c;
	sent = ka_stat(0, "crlf_sent");
	fam_answer(&res, pool, 1, true);
	acct_dns_result_inject(0, &res);
	/* both earlier members are past the 2 s window: the sticky alone */
	CHECK_UINT(ka_stat(0, "crlf_sent"), sent + 1);
	CHECK(ping_waiting(fd_c));
	CHECK(!ping_waiting(fd_b));

	CASE("round: linger off drops the departed address at once");
	core_reload(&live, &off);
	pool[0].port = port_b;
	sent = ka_stat(0, "crlf_sent");
	fam_answer(&res, pool, 1, true);
	acct_dns_result_inject(0, &res);
	CHECK_UINT(ka_stat(0, "crlf_sent"), sent + 1);
	CHECK(ping_waiting(fd_b));
	CHECK(!ping_waiting(fd_c));

	CASE("round: a dialog destination the linger set covers is pinged once");
	core_reload(&off, &live);	/* the window is back on */
	calls_init(NULL, NULL);
	calls_host_bind(proto_host_fake());
	calls_reload(&live);
	calls_hal_codecs_set(~0u);
	pool[0].port = port_c;
	fam_answer(&res, pool, 1, true);
	acct_dns_result_inject(0, &res);	/* sticky c; the list starts fresh */
	CHECK(loopback_invite_send(fd_b, port_b, local_port, "proxy-call-1",
				   NULL));
	for (i = 0; i < 50 && !calls_line_busy(0); i++)
		uloop_run_timeout(20);
	CHECK(calls_line_busy(0));
	CHECK(call_ring(0, 0));
	CHECK(calls_line_dialog_dst(0, 0) != NULL);
	/* b enters the answers, so the retention learns the address the
	 * dialog runs on */
	pool[0].port = port_b;
	fam_answer(&res, pool, 1, true);
	acct_dns_result_inject(0, &res);
	ping_waiting(fd_b);			/* drain before the count */
	ping_waiting(fd_c);
	pool[0].port = port_c;
	sent = ka_stat(0, "crlf_sent");
	fam_answer(&res, pool, 1, true);
	acct_dns_result_inject(0, &res);
	/* the sticky c and the lingering b; the dialog on b is covered by the
	 * retention and gets no second ping */
	CHECK_UINT(ka_stat(0, "crlf_sent"), sent + 2);
	CHECK(ping_waiting(fd_b));
	CHECK(ping_waiting(fd_c));
	CHECK(call_reject(0, 0, 486));
	for (i = 0; i < 50 && calls_line_dialog_dst(0, 0); i++)
		uloop_run_timeout(20);

	calls_done();
	core_reload(&live, &none);
	for (i = 0; i < 400 && status_line0(NULL, 0); i++)
		uloop_run_timeout(20);
	CHECK_UINT(status_line0(NULL, 0), 0);
	net_ct_udp_bound_test_set(0, 0);

	uloop_fd_delete(&fam_ufd4);
	close(fam_fd4);
	close(fd_b);
	close(fd_c);
	fam_fd4 = -1;
	uloop_done();
}

/*
 * The independent expiry deadline. A mode 1 rule goes away when its window
 * ends, even when no answer arrives and no round runs. The linger timer alone
 * runs the reconcile then. The chains complete by hand, so the plan that the
 * cases read is always the chain that the last reconcile asked for.
 */
static void test_linger_fw_timer(void)
{
	static struct cfg live;
	static struct cfg none;
	struct sip_dns_result res;
	struct fam_target pool[1];
	uint16_t reg_a, local_port;
	unsigned int i;
	bool saw1, saw2;

	CASE("dns move: the expiry deadline removes the rule with no answer and no round");
	CHECK(uloop_init() == 0);
	reg_a = udp_loopback_open(&fam_fd4);
	local_port = fam_free_port();
	CHECK(reg_a && local_port);

	fam_regs4 = 0;		/* the counter is cumulative across classes */
	fam_ufd4.fd = fam_fd4;
	fam_ufd4.cb = fam_cb4;
	CHECK(uloop_fd_add(&fam_ufd4, ULOOP_READ) == 0);

	event_test_mode(true);
	fw_test_mode(true);
	net_ct_udp_bound_test_set(60, 180);

	base_default();
	live = base;
	live.channels = 1;
	live.restrict_source = 1;
	live.dns_cache_update_mode = false;
	live.dns_linger = 6;
	fam_channel(&live, 0, "127.0.0.1", reg_a, local_port);
	none = live;
	none.channels = 0;

	core_start(&live);
	CHECK(fam_count_wait(&fam_regs4, 1));
	CHECK(fam_wait(0, "state", "registered"));
	for (i = 0; i < 200 && fw_plan_count(); i++)
		fw_test_step_done(0);
	CHECK_UINT(fw_plan_count(), 0);

	/* The rotation holds one rule per member: the current one and the one
	 * inside its window. A chain can coalesce behind whatever ran last,
	 * so the sources are read off every chain the drain completes, never
	 * off one transient plan. */
	pool[0].host = "127.0.0.2";
	pool[0].port = 5060;
	fam_answer(&res, pool, 1, true);
	acct_dns_result_inject(0, &res);
	saw1 = false;
	saw2 = false;
	for (i = 0; i < 400 && fw_plan_count(); i++) {
		if (plan_has("-s 127.0.0.1"))
			saw1 = true;
		if (plan_has("-s 127.0.0.2"))
			saw2 = true;
		fw_test_step_done(0);
	}
	CHECK(saw1);
	CHECK(saw2);

	/* No answer and no round: the timer alone expires the member that
	 * left and reconciles the rules once the window ends. The pump is
	 * bounded by the clock the window runs on, because a loop pass can
	 * return early on an unrelated event. */
	saw1 = false;
	saw2 = false;
	{
		struct timespec ts;
		uint64_t now_ms, until_ms;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		until_ms = (uint64_t)ts.tv_sec * 1000 +
			   (uint64_t)ts.tv_nsec / 1000000 + 8000;
		for (;;) {
			clock_gettime(CLOCK_MONOTONIC, &ts);
			now_ms = (uint64_t)ts.tv_sec * 1000 +
				 (uint64_t)ts.tv_nsec / 1000000;
			if (now_ms >= until_ms)
				break;
			uloop_run_timeout(20);
			while (fw_plan_count()) {
				if (plan_has("-s 127.0.0.1"))
					saw1 = true;
				if (plan_has("-s 127.0.0.2"))
					saw2 = true;
				fw_test_step_done(0);
			}
		}
	}
	CHECK(saw2);
	CHECK(!saw1);
	/* only the timer compacts the retention here: mode 1 runs no round
	 * and no further answer arrives */
	CHECK_UINT(status_array_count(0, "linger"), 0);

	core_reload(&live, &none);
	for (i = 0; i < 400 && status_line0(NULL, 0); i++)
		uloop_run_timeout(20);
	net_ct_udp_bound_test_set(0, 0);

	uloop_fd_delete(&fam_ufd4);
	close(fam_fd4);
	fam_fd4 = -1;
	uloop_done();
}

/* ------------------------------------ the WAN address change of an outage */

/*
 * A field sequence: a line refreshes on a short expiry, the
 * registrar stops answering, the interface goes down and comes back on another
 * address, and the line has to register again from the new address with no
 * help from the operator. The registrar goes silent by leaving the loop, so
 * every request it does not answer waits in its socket buffer and the cases
 * read it there.
 *
 * Line 0 carries the short expiry and the ordinary T1, so the refresh the
 * outage swallows stays in flight for the length of the class: timer F is 32 s
 * (RFC 3261 17.1.2.2). Line 1 keeps the 10 ms T1 of fam_channel(), so its
 * attempt ends 640 ms after it leaves, and its retry delay is 20 s, which is
 * the wait the address change has to cut short.
 *
 * The addresses netifd reports are documentation addresses (RFC 5737). The
 * sockets bind the wildcard, so what they name is the Via, the Contact and the
 * local host of the line and nothing else.
 */

struct flap_fix {
	struct cfg live;
	struct cfg parked;
	uint16_t reg_port;
	uint16_t local_port;
};

/*
 * Two lines on one loopback registrar that answers, with the IPv4 address in
 * place before the lines are built. rport nonzero makes the registrar report
 * the mapping it sees (RFC 3581), which is the second pass of this fixture.
 * False means the sockets could not be taken and the caller stops there.
 */
static bool flap_start(struct flap_fix *fx, unsigned int rport)
{
	memset(fx, 0, sizeof(*fx));

	CHECK(uloop_init() == 0);
	fx->reg_port = udp_loopback_open(&fam_fd4);
	fx->local_port = fam_free_port();
	CHECK(fx->reg_port && fx->local_port);
	if (!fx->reg_port || !fx->local_port)
		return false;

	/* the counters are cumulative across the classes of this binary */
	fam_regs4 = 0;
	fam_removals4 = 0;
	fam_rport = rport;
	fam_ufd4.fd = fam_fd4;
	fam_ufd4.cb = fam_cb4;
	fam_registrar_answers(&fam_ufd4, true);

	event_test_mode(true);
	fw_test_mode(true);

	base_default();
	fx->live = base;
	fx->live.channels = 2;
	/* a changed answer must not put a REGISTER of its own beside the ones
	 * the address changes send */
	fx->live.dns_cache_update_mode = false;
	fam_channel(&fx->live, 0, "127.0.0.1", fx->reg_port, fx->local_port);
	fam_channel(&fx->live, 1, "127.0.0.1", fx->reg_port, fx->local_port);

	/* the refresh at 2 s the outage swallows, and the ordinary
	 * retransmission ladder behind it, which keeps that attempt in flight
	 * for the whole class */
	fx->live.channel[0].sip_registration_expiry = 5;
	fx->live.channel[0].timers.t1 = 500;
	fx->live.channel[0].timers.t2 = 4000;
	fx->live.channel[0].timers.t4 = 5000;

	/* the delay the line in backoff would otherwise wait out */
	fx->live.channel[1].sip_register_retry = 20;

	fx->parked = fx->live;
	fx->parked.channels = 0;
	snprintf(fx->parked.local_interface, sizeof(fx->parked.local_interface),
		 "wan2");

	status46_inject(V6_DEV, "192.0.2.10", NULL, 0);
	core_start(&fx->live);

	return true;
}

static void flap_end(struct flap_fix *fx)
{
	unsigned int i;

	core_reload(&fx->live, &fx->parked);
	for (i = 0; i < 400 && status_line0(NULL, 0); i++)
		uloop_run_timeout(20);
	CHECK_UINT(status_line0(NULL, 0), 0);

	fam_rport = 0;
	fam_registrar_answers(&fam_ufd4, false);
	close(fam_fd4);
	fam_fd4 = -1;
	uloop_done();
}

/* the port of the public mapping the registrar of the second pass reports */
#define FLAP_RPORT	40100

/*
 * The second pass of the fixture: the registrar reports the mapping that it
 * sees (RFC 3581). Each line therefore registers with a public Contact, and a
 * change of the local address must remove that Contact. The mapping stays the
 * same across a change unless a case moves fam_rport. That is the common case
 * behind one NAT, and the case where the removal Contact and the corrected
 * Contact are one URI. The cases read the wire of line 1, which refreshes an
 * hour later, so every REGISTER of its own belongs to the change under test.
 */
static void flap_nat_cases(struct flap_fix *fx)
{
	static const struct addr6_row gua6[] = { { "2001:db8:71::5", -1 } };
	static struct fam_seen seen;
	static struct cfg v6on;
	struct sockaddr_storage peer;
	socklen_t plen;
	char wire[2048];
	char mapped[128];
	char want[256];

	CASE("flap: behind NAT a flap on the same address never sends its own "
	     "Contact with expires=0");
	if (!flap_start(fx, FLAP_RPORT))
		return;
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));
	snprintf(mapped, sizeof(mapped), "sip:5551001@127.0.0.1:%u", FLAP_RPORT);
	CHECK(fam_has(1, "contact", mapped));
	fam_registrar_answers(&fam_ufd4, false);
	fam_drain(fam_fd4);
	event6_inject(V6_IFACE, "ifdown");
	event6_inject(V6_IFACE, "ifup");
	status46_inject(V6_DEV, "192.0.2.10", NULL, 0);
	fam_answer_until_up(fam_fd4, 1, "5551001", mapped, &seen);
	CHECK(fam_is(1, "state", "registered"));
	CHECK(fam_has(1, "contact", mapped));
	/*
	 * The public Contact is the only binding that reaches the line, and the
	 * local address did not move, so the flap sends one REGISTER with that
	 * Contact and removes nothing. The window behind the answer shows that
	 * no second REGISTER follows.
	 */
	fam_pump_ms(100);
	CHECK_UINT(fam_drain_user(fam_fd4, "5551001"), 0);
	CHECK_UINT(seen.regs, 1);
	CHECK_UINT(seen.expires0, 0);
	CHECK_UINT(seen.removals, 0);
	CHECK(!seen.twice);
	snprintf(want, sizeof(want), "Contact: <%s>\r\n", mapped);
	CHECK(has_str(seen.last, want));

	/*
	 * The same flap, and the NAT hands out another mapping behind it. The
	 * answer moves the Contact (RFC 3581), and the correction carries the
	 * new public Contact alone: the line recorded no removal, because its
	 * local address did not move. The old public Contact stays at the
	 * registrar until it expires, which is the accepted cost of a removal
	 * that is best effort.
	 */
	CASE("flap: behind NAT a flap that moves the public mapping leaves the "
	     "old public Contact to expire");
	fam_rport = FLAP_RPORT + 1;
	snprintf(mapped, sizeof(mapped), "sip:5551001@127.0.0.1:%u", fam_rport);
	event6_inject(V6_IFACE, "ifdown");
	event6_inject(V6_IFACE, "ifup");
	status46_inject(V6_DEV, "192.0.2.10", NULL, 0);
	fam_answer_until_up(fam_fd4, 1, "5551001", mapped, &seen);
	CHECK(fam_is(1, "state", "registered"));
	CHECK(fam_has(1, "contact", mapped));
	CHECK_UINT(seen.regs, 2);
	CHECK_UINT(seen.expires0, 0);
	snprintf(want, sizeof(want), "Contact: <%s>\r\n", mapped);
	CHECK(has_str(seen.last, want));

	/*
	 * The registrar holds the public Contact, and the REGISTER of the change
	 * removes it beside the Contact of the new address. The answer reports
	 * the same mapping, so the correction carries that URI as its Contact
	 * and must not carry it as a removal too.
	 */
	CASE("flap: behind NAT a new address removes the old public Contact once");
	status46_inject(V6_DEV, "192.0.2.20", NULL, 0);
	fam_answer_until_up(fam_fd4, 1, "5551001", mapped, &seen);
	CHECK(fam_is(1, "state", "registered"));
	CHECK(fam_has(1, "contact", mapped));
	CHECK_UINT(seen.regs, 2);
	CHECK_UINT(seen.removals, 1);
	CHECK(!seen.twice);
	CHECK(has_str(seen.last, want));
	CHECK(!has_str(seen.last, ";expires=0"));
	/* line 0 went through the same changes, and whatever it still has in
	 * flight ends here, so the next case starts from its public Contact */
	fam_registrar_answers(&fam_ufd4, true);
	CHECK(fam_wait(0, "state", "registered"));
	snprintf(want, sizeof(want), "sip:5551000@127.0.0.1:%u", fam_rport);
	CHECK(fam_has(0, "contact", want));
	fam_registrar_answers(&fam_ufd4, false);

	/*
	 * Line 0 retries one second after a failure. The registrar refuses its
	 * REGISTER of the change, which carries two Contacts, and answers line 1
	 * as before. The removal is best effort: the retry carries the Contact
	 * of the new address alone, so a registrar that refuses the form does
	 * not see it again, and the old binding expires there.
	 */
	CASE("flap: a registrar that refuses the two Contact REGISTER gets the "
	     "new Contact alone next");
	fam_drain(fam_fd4);
	status46_inject(V6_DEV, "192.0.2.30", NULL, 0);
	CHECK(fam_request_wait(fam_fd4, "5551000", wire, sizeof(wire), &peer,
			       &plen));
	snprintf(want, sizeof(want),
		 "Contact: <sip:5551000@127.0.0.1:%u>;expires=0\r\n", fam_rport);
	CHECK(has_str(wire, want));
	CHECK(registrar_reply(fam_fd4, wire, 0, (const struct sockaddr *)&peer,
			      plen, "500 Server Error", NULL));
	fam_answer_until_up(fam_fd4, 1, "5551001", mapped, &seen);
	CHECK(fam_wait(0, "state", "backoff"));
	CHECK(fam_request_wait(fam_fd4, "5551000", wire, sizeof(wire), &peer,
			       &plen));
	snprintf(want, sizeof(want), "Contact: <sip:5551000@192.0.2.30:%u>\r\n",
		 fx->local_port);
	CHECK(has_str(wire, want));
	CHECK(!has_str(wire, ";expires=0"));
	CHECK(registrar_reply(fam_fd4, wire, fam_rport,
			      (const struct sockaddr *)&peer, plen, "200 OK",
			      NULL));
	fam_registrar_answers(&fam_ufd4, true);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));

	/*
	 * One netifd answer moves the IPv4 address and brings the address6 that
	 * moves the preference to IPv6. Both lines signal in IPv4, so the
	 * address of their family moved and the preference left it. The
	 * preference wins: a move in place keeps the targets, and only a
	 * resolution gives a line a target of the other family. The operator
	 * gate and the backend feature open IPv6 for the length of the case.
	 */
	CASE("flap: an answer that moves the address and the family preference "
	     "resolves again");
	v6on = fx->live;
	v6on.ipv6_enabled = true;
	accounts_hal_features_set(HAL_FEAT_IPV6_MEDIA);
	core_reload(&fx->live, &v6on);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));
	status46_inject(V6_DEV, "192.0.2.40", ROWS(gua6));
	CHECK_UINT(sip_dns_prefer_family(acct_dns()), AF_INET6);
	CHECK(fam_is(0, "state", "resolving"));
	CHECK(fam_is(1, "state", "resolving"));
	CHECK_UINT(sip_dns_query_count(acct_dns()), 2);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));
	core_reload(&v6on, &fx->live);
	accounts_hal_features_set(0);
	status46_inject(V6_DEV, "192.0.2.40", NULL, 0);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));

	flap_end(fx);
}

/*
 * A stream line through the same change. The transport is one setting for the
 * process, so the line has its own fixture: one tcp line and a stream
 * registrar on the loopback. The rebind closes the connections of the device
 * and opens the listener again. The REGISTER of the change opens a connection
 * of its own. That connection is the first one of the move and no reconnect,
 * so the registrar reads one REGISTER and the line reports no edge.
 */
static void flap_tcp_case(void)
{
	static struct cfg live;
	static struct cfg parked;
	char callid[256];
	char callid_was[256];
	char want[256];
	uint16_t reg_port, local_port;
	int probe4 = -1, probe6 = -1;
	unsigned int answers, posts;
	unsigned int i;

	CASE("flap: a tcp line keeps its registration through the address change");
	CHECK(uloop_init() == 0);
	reg_port = tcp_reg_open(&tcp_reg[0]);
	local_port = tcp_port_hold(&probe4, &probe6);
	CHECK(reg_port && local_port);
	if (probe4 >= 0)
		close(probe4);
	if (probe6 >= 0)
		close(probe6);
	if (!reg_port || !local_port)
		goto out;

	event_test_mode(true);
	fw_test_mode(true);

	base_default();
	live = base;
	live.channels = 1;
	snprintf(live.transport, sizeof(live.transport), "tcp");
	live.dns_cache_update_mode = false;
	fam_channel(&live, 0, "127.0.0.1", reg_port, local_port);
	parked = live;
	parked.channels = 0;
	snprintf(parked.local_interface, sizeof(parked.local_interface), "wan2");

	status46_inject(V6_DEV, "192.0.2.10", NULL, 0);
	core_start(&live);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_has(0, "contact", "@192.0.2.10:"));
	CHECK(req_line_get(tcp_reg[0].last, "Call-ID", callid_was,
			   sizeof(callid_was)));
	event_test_reap(0, 0);
	posts = event_test_posts();
	answers = tcp_reg[0].answers;

	status46_inject(V6_DEV, "192.0.2.20", NULL, 0);
	CHECK(fam_is(0, "state", "refreshing"));
	CHECK(fam_is(0, "status", "registered"));
	CHECK_UINT(sip_dns_query_count(acct_dns()), 0);
	CHECK(fam_wait(0, "state", "registered"));
	for (i = 0; i < 10; i++)
		uloop_run_timeout(20);
	CHECK(fam_is(0, "state", "registered"));
	CHECK(fam_is(0, "listener", "1"));
	CHECK_UINT(tcp_reg[0].answers, answers + 1);
	CHECK_UINT(event_test_posts(), posts);
	CHECK(has_str(tcp_reg[0].last, "SIP/2.0/TCP 192.0.2.20:"));
	CHECK(has_str(tcp_reg[0].last, "Contact: <sip:5551000@192.0.2.20:"));
	snprintf(want, sizeof(want),
		 "Contact: <sip:5551000@192.0.2.10:%u;transport=tcp>;expires=0\r\n",
		 local_port);
	CHECK(has_str(tcp_reg[0].last, want));
	CHECK(req_line_get(tcp_reg[0].last, "Call-ID", callid, sizeof(callid)));
	CHECK(!strcmp(callid, callid_was));

	core_reload(&live, &parked);
	for (i = 0; i < 400 && status_line0(NULL, 0); i++)
		uloop_run_timeout(20);
	CHECK_UINT(status_line0(NULL, 0), 0);

out:
	tcp_reg_close(&tcp_reg[0]);
	uloop_done();
}

static void test_flap_outage(void)
{
	static struct flap_fix fx;
	static struct cfg mwi;
	struct sip_dns_result res;
	struct fam_target back[1];
	char stale[2048];
	char wire[2048];
	char want[256];
	char callid[256];
	char callid_was[256];
	char branch[128];
	char state[32];
	char cseq[64];
	uint64_t rebinds;
	uint64_t rx;
	unsigned int posts;
	unsigned int recs;
	unsigned int mark;
	unsigned int i;
	bool removal;
	bool saw;

	CASE("flap: a line registers on a short expiry and its refresh goes "
	     "unanswered");
	if (!flap_start(&fx, 0))
		return;
	CHECK(fam_count_wait(&fam_regs4, 2));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));
	CHECK(fam_has(0, "contact", "@192.0.2.10:"));

	fam_registrar_answers(&fam_ufd4, false);
	CHECK(fam_inflight_wait(fam_fd4));
	CHECK(fam_peek(fam_fd4, stale, sizeof(stale)));
	CHECK(has_str(stale, "REGISTER sip:"));
	snprintf(want, sizeof(want), "<sip:5551000@192.0.2.10:%u>",
		 fx.local_port);
	CHECK(has_str(stale, want));
	/* the exchange in flight, named by its own transaction */
	CHECK(via_branch(stale, branch, sizeof(branch)));
	/* the binding stands while the refresh of it is in flight */
	CHECK(fam_is(0, "state", "refreshing"));
	CHECK(fam_is(0, "status", "registered"));

	/*
	 * The interface goes down and comes back on another address. The
	 * retransmissions of the attempt in flight are read away first, so the
	 * registrar holds nothing of that exchange when the change runs. The
	 * window then outlasts the 500 ms retransmission deadline of the
	 * exchange, and nothing of it arrives: the abort took it away. Its
	 * branch is what names it, because the Call-ID and the old address can
	 * both appear in the REGISTER the change sends.
	 */
	CASE("flap: the ifdown and the ifup on a new address drop the REGISTER "
	     "in flight");
	CHECK(fam_drain(fam_fd4) > 0);
	event_test_reap(0, 0);
	recs = event_test_recs();
	event6_inject(V6_IFACE, "ifdown");
	/* the ifup reaches netifd_query(), which returns with no ubus of its
	 * own, so the status answer below is what the query would deliver; the
	 * e2e flow with a netifd stub covers the query itself */
	event6_inject(V6_IFACE, "ifup");
	status46_inject(V6_DEV, "192.0.2.20", NULL, 0);
	CHECK(fam_inflight_wait(fam_fd4));
	CHECK(fam_peek(fam_fd4, wire, sizeof(wire)));
	CHECK(has_str(wire, "REGISTER sip:"));
	fam_pump_ms(700);
	CHECK_UINT(fam_drain_user(fam_fd4, branch), 0);

	/*
	 * The abort of that exchange clears the transaction before it runs, so
	 * the callback of the abort reaches no registration (reg.c txn_drop),
	 * and the line never reports itself down for an exchange it gave up on
	 * itself. Line 1 is a line of its own and fails in this window on
	 * purpose, so the topic of line 0 is what the records are read for.
	 */
	CASE("flap: no late failure reaches the line from the dropped exchange");
	for (i = recs; i < event_test_recs(); i++)
		CHECK(!has_str(evt_arg(i, 4), "line-1/reg/failed"));
	CHECK(!fam_is(0, "status", "failed"));

	CASE("flap: the new REGISTER carries the new address in its Via and "
	     "its Contact");
	snprintf(want, sizeof(want), "SIP/2.0/UDP 192.0.2.20:%u", fx.local_port);
	CHECK(has_str(wire, want));
	snprintf(want, sizeof(want), "<sip:5551000@192.0.2.20:%u>",
		 fx.local_port);
	CHECK(has_str(wire, want));
	CHECK(req_line_get(wire, "CSeq", cseq, sizeof(cseq)));
	CHECK(has_str(cseq, "REGISTER"));

	/*
	 * The registrar holds the Contact of the old address, and nothing
	 * answers there any more. The REGISTER of the change names it as a
	 * second Contact with expires=0 (RFC 3261 10.2.2), so the registrar
	 * drops it now and not when it expires.
	 */
	CASE("flap: the REGISTER after the change carries the old Contact with "
	     "expires=0");
	snprintf(want, sizeof(want),
		 "Contact: <sip:5551000@192.0.2.10:%u>;expires=0\r\n",
		 fx.local_port);
	CHECK(has_str(wire, want));

	/* one registration, so the registrar reads the two Contacts as a move
	 * of one binding set and not as a second device */
	CASE("flap: and the new Contact beside it, on the Call-ID the line "
	     "already had");
	snprintf(want, sizeof(want), "Contact: <sip:5551000@192.0.2.20:%u>\r\n",
		 fx.local_port);
	CHECK(has_str(wire, want));
	CHECK(req_line_get(stale, "Call-ID", callid_was, sizeof(callid_was)));
	CHECK(req_line_get(wire, "Call-ID", callid, sizeof(callid)));
	CHECK(!strcmp(callid, callid_was));

	/* the window above read the requests away, so the line registers on
	 * the retransmission of the attempt it has running */
	CASE("flap: the registrar answers it and the line reads registered");
	mark = fam_regs4;
	fam_registrar_answers(&fam_ufd4, true);
	CHECK(fam_count_wait(&fam_regs4, mark + 1));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_has(0, "contact", "@192.0.2.20:"));

	/*
	 * The answer the registrar owed the exchange the line dropped, sent
	 * late and by hand. It belongs to a transaction that no longer exists,
	 * so it matches nothing and the binding the line holds now stands
	 * untouched. The registrar is out of the loop for the length of the
	 * window and answers nothing else, so the receive counter of the
	 * transport is the proof that this datagram reached the daemon.
	 */
	CASE("flap: the stray 200 of the dropped exchange changes nothing");
	fam_registrar_answers(&fam_ufd4, false);
	event_test_reap(0, 0);
	recs = event_test_recs();
	rx = sip_transport_stat(acct_transport())->rx;
	CHECK(fam_stray_answer(fam_fd4, stale, fx.local_port));
	for (i = 0; i < 10 &&
		    sip_transport_stat(acct_transport())->rx == rx; i++)
		uloop_run_timeout(20);
	CHECK_UINT(sip_transport_stat(acct_transport())->rx, rx + 1);
	CHECK(fam_is(0, "state", "registered"));
	CHECK(fam_has(0, "contact", "@192.0.2.20:"));
	CHECK(!fam_has(0, "contact", "192.0.2.10"));
	for (i = recs; i < event_test_recs(); i++)
		CHECK(!has_str(evt_arg(i, 4), "line-1/reg/failed"));

	/*
	 * The 2xx ended the removal: the registrar dropped the old Contact with
	 * the request it answered. The refresh of the short expiry is the next
	 * REGISTER of the line, and the registrar is still out of the loop, so
	 * the request waits where the case reads it.
	 */
	CASE("flap: after the 2xx the next refresh carries the new Contact alone");
	CHECK(fam_request_read(fam_fd4, "5551000", wire, sizeof(wire)));
	snprintf(want, sizeof(want), "Contact: <sip:5551000@192.0.2.20:%u>\r\n",
		 fx.local_port);
	CHECK(has_str(wire, want));
	CHECK(!has_str(wire, ";expires=0"));
	fam_registrar_answers(&fam_ufd4, true);

	/* netifd reports an address change of its own, with no down edge in
	 * front of it, and both lines follow it */
	CASE("flap: a new address with no ifdown first recovers the same way");
	mark = fam_regs4;
	status46_inject(V6_DEV, "192.0.2.30", NULL, 0);
	CHECK(fam_count_wait(&fam_regs4, mark + 2));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_has(0, "contact", "@192.0.2.30:"));
	CHECK(fam_wait(1, "state", "registered"));
	CHECK(fam_has(1, "contact", "@192.0.2.30:"));

	/*
	 * Both lines hold a binding now, so the change moves both in place. The
	 * binding stands at the registrar while the REGISTER of the change is
	 * in flight, and the line says so: the state reads refreshing and the
	 * status registered, from the frame of the change on. Nothing asks the
	 * resolver, and the publisher posts no edge, because the line never
	 * went down.
	 */
	CASE("flap: the line reads registered throughout the change");
	event_test_reap(0, 0);
	event_test_reap(1, 0);
	posts = event_test_posts();
	status46_inject(V6_DEV, "192.0.2.31", NULL, 0);
	CHECK(fam_is(0, "state", "refreshing"));
	CHECK(fam_is(0, "status", "registered"));
	CHECK(fam_is(1, "state", "refreshing"));
	CHECK(fam_is(1, "status", "registered"));
	CHECK_UINT(sip_dns_query_count(acct_dns()), 0);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));
	CHECK(fam_has(0, "contact", "@192.0.2.31:"));
	CHECK(fam_has(1, "contact", "@192.0.2.31:"));
	CHECK(!fam_is(0, "status", "failed"));
	CHECK_UINT(event_test_posts(), posts);

	/*
	 * The operator unregisters line 1 and the address moves before the
	 * registrar answers. The line is on its way out, so the change sends no
	 * REGISTER for it: a registration here would undo the command. The
	 * Expires 0 in flight carries the old Via, nothing answers it, and
	 * timer F ends it 640 ms after it left. The branch of that exchange is
	 * what tells its retransmissions from a request of the change.
	 */
	CASE("flap: an address change during an unregister sends nothing and "
	     "the unregister still ends");
	fam_registrar_answers(&fam_ufd4, false);
	fam_drain(fam_fd4);
	CHECK(account_unregister_cmd(1));
	CHECK(fam_request_read(fam_fd4, "5551001", wire, sizeof(wire)));
	CHECK(has_str(wire, "\r\nExpires: 0\r\n"));
	CHECK(via_branch(wire, branch, sizeof(branch)));
	status46_inject(V6_DEV, "192.0.2.32", NULL, 0);
	fam_pump_ms(700);
	recs = 0;
	while (fam_request_next(fam_fd4, "5551001", wire, sizeof(wire))) {
		if (!has_str(wire, branch))
			recs++;
	}
	CHECK_UINT(recs, 0);
	CHECK(fam_wait(1, "state", "unregistered"));
	fam_registrar_answers(&fam_ufd4, true);
	CHECK(account_register_cmd(1));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));
	CHECK(fam_has(1, "contact", "@192.0.2.32:"));

	/*
	 * A flap that hands the same address back. The Contact cannot say
	 * whether the lines registered again, so the wire does: line 1
	 * refreshes an hour out and sends nothing else, so a REGISTER of its
	 * own is the one the flap sent. The requests are answered by hand
	 * instead of read away, so both lines come up on the attempt they just
	 * sent and wait for no retransmission.
	 */
	CASE("flap: an ifdown and an ifup on the same address register again");
	fam_registrar_answers(&fam_ufd4, false);
	fam_drain(fam_fd4);
	event6_inject(V6_IFACE, "ifdown");
	event6_inject(V6_IFACE, "ifup");
	status46_inject(V6_DEV, "192.0.2.32", NULL, 0);
	/* the whole window: the removal check below counts every request that
	 * the flap sends */
	for (i = 0; i < 5; i++)
		uloop_run_timeout(20);
	snprintf(want, sizeof(want), "<sip:5551001@192.0.2.32:%u>",
		 fx.local_port);
	saw = false;
	removal = false;
	for (i = 0; i < 16 && registrar_answer(fam_fd4, fam_rport); i++) {
		if (has_str(registrar_last, want))
			saw = true;
		if (has_str(registrar_last, ";expires=0"))
			removal = true;
	}
	CHECK(saw);
	fam_registrar_answers(&fam_ufd4, true);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));

	/* the Contact the registrar holds is the one the line still carries, so
	 * there is no old Contact to remove */
	CASE("flap: an ifdown and an ifup on the same address send no removal "
	     "Contact");
	CHECK(!removal);

	/*
	 * A failover hands the address to another netdev. The sockets are
	 * scoped to the one the answer names, so the move alone drives the
	 * rebind, and the lines register from the address beside it.
	 */
	CASE("flap: a netdev move beside the new address rebinds and registers");
	mark = fam_regs4;
	rebinds = sip_transport_stat(acct_transport())->rebinds;
	status46_inject(V6_DEV2, "192.0.2.40", NULL, 0);
	CHECK(net_key_is("l3_device", V6_DEV2));
	CHECK(sip_transport_stat(acct_transport())->rebinds > rebinds);
	CHECK(fam_count_wait(&fam_regs4, mark + 2));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_has(0, "contact", "@192.0.2.40:"));
	CHECK(fam_wait(1, "state", "registered"));
	CHECK(fam_has(1, "contact", "@192.0.2.40:"));
	/* one key, one socket: both lines are on the same port, device and
	 * family (sip/transport.h) */
	CHECK_UINT(sip_sock_count(acct_transport()), 1);

	/*
	 * The answer that repeats the state of the moment. Nothing moved, so
	 * the sockets keep their bind and the lines keep their registration:
	 * a rebind here would drop the conntrack tuple of every line, and a
	 * rebuild would take the registration down for the length of a
	 * REGISTER. The queue is read away first, so a request that was
	 * already waiting cannot read as one this answer sent.
	 */
	CASE("flap: a status answer on the same address with no ifdown leaves "
	     "the registration alone");
	fam_drain(fam_fd4);
	rebinds = sip_transport_stat(acct_transport())->rebinds;
	status46_inject(V6_DEV2, "192.0.2.40", NULL, 0);
	CHECK(!fam_peek(fam_fd4, wire, sizeof(wire)));
	CHECK_UINT(sip_transport_stat(acct_transport())->rebinds, rebinds);
	CHECK(fam_is(0, "state", "registered"));
	CHECK(fam_is(1, "state", "registered"));

	/*
	 * The outage puts line 1 in backoff: its attempt ends 640 ms after it
	 * leaves and the ladder holds the next one for 20 s. The failure took
	 * the binding with it, so the address change on top of that rebuilds
	 * the line, and the attempt leaves with the answer of its resolution,
	 * long before the ladder would have sent it.
	 */
	CASE("flap: an address change while the line waits out its backoff "
	     "registers at once");
	fam_registrar_answers(&fam_ufd4, false);
	fam_drain(fam_fd4);
	status46_inject(V6_DEV2, "192.0.2.50", NULL, 0);
	CHECK(fam_wait(1, "state", "backoff"));
	fam_drain(fam_fd4);
	status46_inject(V6_DEV2, "192.0.2.60", NULL, 0);
	snprintf(want, sizeof(want), "<sip:5551001@192.0.2.60:%u>",
		 fx.local_port);
	CHECK(fam_request_read(fam_fd4, want, wire, sizeof(wire)));

	/*
	 * The registrar holds no binding the line knows of: the failure dropped
	 * it, and with it the Contact a removal would name. The registration is
	 * a new one, so the request in hand carries one Contact. Line 0 holds
	 * its binding through the same two changes, and its requests are the
	 * contrast two cases down.
	 */
	CASE("flap: a line with no granted binding sends no removal Contact");
	CHECK(!has_str(wire, ";expires=0"));
	snprintf(want, sizeof(want), "Contact: <sip:5551001@192.0.2.60:%u>\r\n",
		 fx.local_port);
	CHECK(has_str(wire, want));

	/*
	 * Two answers in one frame, with no loop pass between them, while the
	 * registrar is still silent. Line 1 has no binding yet, so each answer
	 * rebuilds it, and the second one meets the resolution the first one
	 * started: the second rebuild released the first query before it asked
	 * for its own. Line 0 holds a binding and moves in place, which asks the
	 * resolver nothing, so the one query is the one of line 1.
	 */
	CASE("flap: an address change while the resolve is pending leaves one "
	     "query running");
	status46_inject(V6_DEV2, "192.0.2.70", NULL, 0);
	CHECK(fam_is(1, "state", "resolving"));
	status46_inject(V6_DEV2, "192.0.2.80", NULL, 0);
	CHECK(fam_is(1, "state", "resolving"));
	CHECK(fam_is(0, "state", "refreshing"));
	CHECK_UINT(sip_dns_query_count(acct_dns()), 1);

	/*
	 * Line 0 moved four times since the last 2xx, and the registrar holds
	 * the Contact of the address before the first of them. That Contact is
	 * the one to remove, whatever the line carried in between: the
	 * registrar never granted any of those.
	 */
	CASE("flap: two changes before a 2xx keep the first Contact the "
	     "registrar holds");
	snprintf(want, sizeof(want), "<sip:5551000@192.0.2.80:%u>",
		 fx.local_port);
	CHECK(fam_request_read(fam_fd4, want, wire, sizeof(wire)));
	snprintf(want, sizeof(want),
		 "Contact: <sip:5551000@192.0.2.40:%u>;expires=0\r\n",
		 fx.local_port);
	CHECK(has_str(wire, want));
	CHECK(!has_str(wire, "192.0.2.50"));
	CHECK(!has_str(wire, "192.0.2.60"));
	CHECK(!has_str(wire, "192.0.2.70"));

	CASE("flap: two status answers in a row leave the line on the last address");
	fam_registrar_answers(&fam_ufd4, true);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_has(0, "contact", "@192.0.2.80:"));
	CHECK(!fam_has(0, "contact", "192.0.2.70"));
	CHECK(fam_wait(1, "state", "registered"));
	CHECK(fam_has(1, "contact", "@192.0.2.80:"));

	/*
	 * The resolver of a registered line fails. The answer says so, arms
	 * the retry of the resolution and leaves the binding where it is: the
	 * warning is what the line did with the answer, and no request leaves
	 * the daemon. Every read below runs before a loop pass, so the line's
	 * own resolution is not what any of it observes.
	 */
	CASE("flap: a failed resolver answer leaves the registration standing");
	fam_drain(fam_fd4);
	warn_capture_begin();
	fam_answer_status(&res, SIP_DNS_ERR_NXDOMAIN);
	acct_dns_result_inject(0, &res);
	CHECK_UINT(warn_capture_count("line 0 resolve of 127.0.0.1 failed"), 1);
	warn_capture_end();
	CHECK(fam_is(0, "state", "registered"));
	CHECK(fam_has(0, "contact", "@192.0.2.80:"));
	CHECK(!fam_peek(fam_fd4, wire, sizeof(wire)));

	/*
	 * The same failure meets a line that resolves again behind an address
	 * change, with that resolution still on its delivery timer. Only a line
	 * without a binding resolves there, so the registrar refuses the
	 * REGISTER line 1 sends for one change, and the next change rebuilds
	 * the line. A failed answer builds nothing and sends nothing, and the
	 * good answer behind it is the one the line acts on: its REGISTER is on
	 * the wire before any pass, so the line's own answer, which needs one,
	 * did not send it. Line 0 moves in place through both changes, and the
	 * reads below name the user of line 1 for that reason.
	 */
	CASE("flap: and the good answer behind it brings the line up");
	fam_registrar_answers(&fam_ufd4, false);
	status46_inject(V6_DEV2, "192.0.2.85", NULL, 0);
	CHECK(fam_refuse(fam_fd4, "5551001", "500 Server Error", NULL));
	CHECK(fam_wait(1, "state", "backoff"));
	status46_inject(V6_DEV2, "192.0.2.90", NULL, 0);
	CHECK(fam_key(1, "state", state, sizeof(state)) != NULL);
	CHECK(!strcmp(state, "resolving"));
	fam_drain(fam_fd4);
	fam_answer_status(&res, SIP_DNS_ERR_NXDOMAIN);
	acct_dns_result_inject(1, &res);
	CHECK(fam_is(1, "state", state));
	CHECK_UINT(fam_drain_user(fam_fd4, "5551001"), 0);
	back[0].host = "127.0.0.1";
	back[0].port = fx.reg_port;
	fam_answer(&res, back, 1, true);
	acct_dns_result_inject(1, &res);
	snprintf(want, sizeof(want), "<sip:5551001@192.0.2.90:%u>",
		 fx.local_port);
	CHECK_UINT(fam_drain_user(fam_fd4, want), 1);
	fam_registrar_answers(&fam_ufd4, true);
	CHECK(fam_wait(1, "state", "registered"));
	CHECK(fam_has(1, "contact", "@192.0.2.90:"));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_has(0, "contact", "@192.0.2.90:"));

	/*
	 * The message waiting subscription borrows the Contact of the
	 * registration, and the notifier sends its NOTIFY there. The move in
	 * place builds the subscription again, so the 2xx of the REGISTER
	 * starts a SUBSCRIBE with the new Contact; a subscription left running
	 * would keep the notifier on the old address until its own refresh.
	 * The lines rebuild once for the option, and the registrar answers
	 * every request by hand from the change on.
	 */
	CASE("flap: the subscription follows the line to the new Contact");
	mwi = fx.live;
	mwi.mwi_subscribe = true;
	core_reload(&fx.live, &mwi);
	fx.live = mwi;
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));
	fam_pump_ms(100);
	fam_registrar_answers(&fam_ufd4, false);
	fam_drain(fam_fd4);
	status46_inject(V6_DEV2, "192.0.2.95", NULL, 0);
	snprintf(want, sizeof(want), "<sip:5551001@192.0.2.95:%u>",
		 fx.local_port);
	/* the Expires 0 that ends the old subscription names the Contact of
	 * the moment too, so the one to find is the one that asks for time */
	CHECK(fam_answer_until_text(fam_fd4, "SUBSCRIBE sip:", want,
				    "\r\nExpires: 0\r\n"));
	fam_registrar_answers(&fam_ufd4, true);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));

	flap_end(&fx);

	flap_nat_cases(&fx);
	flap_tcp_case();
}

/* --------------------------------- the failure ladder of one line's resolve */

/*
 * The deadline the resolve timer of a line carries now, against the rung it
 * must hold. The hook reads the timer and not the record of the arm, so real
 * time passes between the two: the read runs in the same frame as the arm, a
 * few milliseconds at most, and the window carries that. A want of 0 is exact,
 * because no armed delay reads 0.
 */
#define BACKOFF_SLACK_MS	50

/* the refresh a good answer of fam_answer() arms: its TTL of 300 s, which the
 * clamp of account.c leaves alone and no rung of the ladder comes near */
#define BACKOFF_TTL_MS		300000

static bool backoff_armed(unsigned int line, unsigned int want)
{
	unsigned int got = acct_test_dns_retry_ms(line);

	if (!want)
		return got == 0;

	return got <= want && got + BACKOFF_SLACK_MS >= want;
}

/*
 * One of the three retries follows no DNS failure. The answer named a
 * target, but the line has no address of its own. It asks again at the fixed
 * delay; on the ladder, a first boot with a late WAN address would wait up to
 * a minute.
 *
 * First, netifd names no address: the teardown of the class above moved the
 * layer to another interface name. Second, the first answer of the line
 * carries one IPv6 target while the line signals IPv4. account_local_host()
 * then has nothing to answer with, because the source probe refuses a
 * destination of the other family. Nothing answers at the registrar port,
 * because the line must reach no registration in the window.
 */
static void backoff_local_host_case(void)
{
	static struct cfg live;
	static struct cfg none;
	struct sip_dns_result res;
	struct fam_target v6[1];
	uint64_t queries;
	uint16_t reg_port, local_port;
	unsigned int i;

	/*
	 * The class above parked its lines, so no line holds a timer and no
	 * line is active. The hook says so instead of running an attempt of its
	 * own, which is what keeps a case from reading a quiet line as one that
	 * retried. A line index past the array answers the same way.
	 */
	CASE("backoff: a fire with no attempt armed does nothing");
	CHECK(uloop_init() == 0);
	reg_port = fam_free_port();
	local_port = fam_free_port();
	CHECK(reg_port && local_port);
	if (!reg_port || !local_port) {
		uloop_done();

		return;
	}
	queries = acct_dns()->stat.queries;
	CHECK(!acct_test_dns_retry_fire(0));
	CHECK(!acct_test_dns_retry_fire(1));
	CHECK(!acct_test_dns_retry_fire(CFG_CHANNEL_MAX));
	CHECK_UINT(acct_dns()->stat.queries, queries);
	CHECK_UINT(acct_test_dns_retry_ms(0), 0);

	CASE("backoff: a missing local address keeps the fixed five second "
	     "retry");
	event_test_mode(true);
	fw_test_mode(true);

	base_default();
	live = base;
	live.channels = 1;
	fam_channel(&live, 0, "127.0.0.1", reg_port, local_port);
	none = live;
	none.channels = 0;

	CHECK(net_key_gone("address"));
	core_start(&live);

	/*
	 * The answer of the line's own literal waits on a delivery timer and
	 * needs a loop pass, so the injected one is the first answer it sees,
	 * and every read below runs before a pass.
	 */
	v6[0].host = "::1";
	v6[0].port = reg_port;
	warn_capture_begin();
	for (i = 0; i < 4; i++) {
		fam_answer(&res, v6, 1, false);
		acct_dns_result_inject(0, &res);
		/* the same five seconds every time, where a rung doubles */
		CHECK(backoff_armed(0, 5000));
	}
	CHECK_UINT(warn_capture_count("line 0 no local address yet"), 4);
	warn_capture_end();

	/* the four waits left the ladder where it stood, so the failed answer
	 * behind them arms the first rung and not the fifth */
	fam_answer_status(&res, SIP_DNS_ERR_NXDOMAIN);
	acct_dns_result_inject(0, &res);
	CHECK(backoff_armed(0, 5000));

	core_reload(&live, &none);
	for (i = 0; i < 400 && status_line0(NULL, 0); i++)
		uloop_run_timeout(20);
	CHECK_UINT(status_line0(NULL, 0), 0);
	uloop_done();
}

/*
 * A field shape: the SRV answer is good, but every glue
 * question of it ends NXDOMAIN. The box then repeats the whole question every
 * five seconds for as long as the fault stands. The ladder doubles that wait
 * up to a minute, and a good answer, an address change or a reload resets it
 * to the first step.
 *
 * The class runs on the fixture of the outage: two registered lines and a
 * registrar that answers. Line 1 carries the ladder, and line 0 carries the
 * registration checks. Reason: line 0 refreshes near half of its five second
 * expiry, and line 1 refreshes an hour later.
 */
static void test_dns_backoff(void)
{
	/* the delays of account.c, DNS_RETRY_MS doubled per attempt and held at
	 * DNS_RETRY_MAX_MS; both constants are private to that file */
	static const unsigned int walk[] = {
		5000, 10000, 20000, 40000, 60000, 60000
	};
	static struct flap_fix fx;
	const char *failed = "line 1 resolve of 127.0.0.1 failed";
	struct sip_dns_result res;
	struct fam_target back[1];
	char registrar[192];
	char contact[192];
	char targets[32];
	char wire[2048];
	char want[256];
	uint64_t queries;
	unsigned int i;

	CASE("backoff: a failed resolve retries on a ladder, not a fixed beat");
	if (!flap_start(&fx, 0))
		return;
	CHECK(fam_count_wait(&fam_regs4, 2));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));
	back[0].host = "127.0.0.1";
	back[0].port = fx.reg_port;

	/*
	 * One good answer of a known TTL first, so the timer of the line starts
	 * this class on a deadline the case knows. Every failed answer behind
	 * it is one attempt, and no loop pass runs between them, so each read
	 * sees the deadline the answer before it armed.
	 */
	fam_answer(&res, back, 1, false);
	acct_dns_result_inject(1, &res);
	CHECK(backoff_armed(1, BACKOFF_TTL_MS));
	for (i = 0; i < 3; i++) {
		fam_answer_status(&res, SIP_DNS_ERR_NXDOMAIN);
		acct_dns_result_inject(1, &res);
		CHECK(backoff_armed(1, walk[i]));
	}

	CASE("backoff: the ladder stops at the cap");
	for (i = 3; i < 6; i++) {
		fam_answer_status(&res, SIP_DNS_ERR_NXDOMAIN);
		acct_dns_result_inject(1, &res);
		CHECK(backoff_armed(1, walk[i]));
	}
	/* two more attempts hold it there */
	for (i = 0; i < 2; i++) {
		fam_answer_status(&res, SIP_DNS_ERR_NXDOMAIN);
		acct_dns_result_inject(1, &res);
		CHECK(backoff_armed(1, walk[5]));
	}

	/*
	 * An answer with targets and no change is the one a cache hit delivers
	 * (shared/resolve.h "Cache"), and it ends the ladder as any other good
	 * answer does. The timer that stands from there is the TTL refresh.
	 */
	CASE("backoff: a good answer resets the ladder");
	fam_answer(&res, back, 1, false);
	acct_dns_result_inject(1, &res);
	/* the rung of a minute gave way to the refresh of the answer */
	CHECK(backoff_armed(1, BACKOFF_TTL_MS));
	fam_answer_status(&res, SIP_DNS_ERR_NXDOMAIN);
	acct_dns_result_inject(1, &res);
	CHECK(backoff_armed(1, walk[0]));

	CASE("backoff: the warning is logged once per delay step");
	fam_answer(&res, back, 1, false);
	acct_dns_result_inject(1, &res);
	warn_capture_begin();
	for (i = 0; i < 5; i++) {
		fam_answer_status(&res, SIP_DNS_ERR_NXDOMAIN);
		acct_dns_result_inject(1, &res);
	}
	CHECK_UINT(warn_capture_count(failed), 5);
	/* the count above ended that window, so the attempts at the cap are
	 * read in one of their own */
	warn_capture_begin();
	for (i = 5; i < 9; i++) {
		fam_answer_status(&res, SIP_DNS_ERR_NXDOMAIN);
		acct_dns_result_inject(1, &res);
	}
	CHECK_UINT(warn_capture_count(failed), 0);
	warn_capture_end();

	CASE("backoff: and again at every tenth attempt once the cap holds it "
	     "flat");
	warn_capture_begin();
	fam_answer_status(&res, SIP_DNS_ERR_NXDOMAIN);
	acct_dns_result_inject(1, &res);		/* the tenth */
	CHECK_UINT(warn_capture_count(failed), 1);
	warn_capture_begin();
	for (i = 10; i < 19; i++) {
		fam_answer_status(&res, SIP_DNS_ERR_NXDOMAIN);
		acct_dns_result_inject(1, &res);
	}
	CHECK_UINT(warn_capture_count(failed), 0);
	warn_capture_begin();
	fam_answer_status(&res, SIP_DNS_ERR_NXDOMAIN);
	acct_dns_result_inject(1, &res);		/* the twentieth */
	CHECK_UINT(warn_capture_count(failed), 1);
	warn_capture_end();

	/*
	 * A line with no binding rebuilds on an address change, and the rebuild
	 * asks the question again from the first rung: a first boot whose WAN
	 * address arrives before its DNS must not wait out a minute. The
	 * unregister is what takes the binding away.
	 */
	CASE("backoff: an address change resets the ladder to the first rung");
	CHECK(account_unregister_cmd(1));
	CHECK(fam_wait(1, "state", "unregistered"));
	status46_inject(V6_DEV, "192.0.2.20", NULL, 0);
	/* the rebuild cancelled the timer and asked at once */
	CHECK_UINT(acct_test_dns_retry_ms(1), 0);
	fam_answer_status(&res, SIP_DNS_ERR_NXDOMAIN);
	acct_dns_result_inject(1, &res);
	CHECK(backoff_armed(1, walk[0]));
	CHECK(fam_wait(1, "state", "registered"));

	/* an operator changed the configuration, so every line asks again from
	 * the first rung, the line the reload leaves in place included */
	CASE("backoff: a reload resets it too");
	for (i = 0; i < 3; i++) {
		fam_answer_status(&res, SIP_DNS_ERR_NXDOMAIN);
		acct_dns_result_inject(1, &res);
	}
	CHECK(backoff_armed(1, walk[2]));
	core_reload(&fx.live, &fx.live);
	fam_answer_status(&res, SIP_DNS_ERR_NXDOMAIN);
	acct_dns_result_inject(1, &res);
	CHECK(backoff_armed(1, walk[0]));

	/*
	 * The TTL re-resolve of a registered line fails. The binding, the
	 * targets and the sticky selection all came from the last good answer
	 * and none of them is this answer's business, so the line keeps them
	 * and sends nothing. Every read runs before a loop pass.
	 */
	CASE("backoff: a registered line whose TTL re-resolve ends NXDOMAIN "
	     "keeps its registration");
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_key(0, "registrar", registrar, sizeof(registrar)) != NULL);
	CHECK(fam_key(0, "contact", contact, sizeof(contact)) != NULL);
	CHECK(fam_key(0, "targets", targets, sizeof(targets)) != NULL);
	fam_drain(fam_fd4);
	fam_answer_status(&res, SIP_DNS_ERR_NXDOMAIN);
	acct_dns_result_inject(0, &res);
	CHECK(fam_is(0, "state", "registered"));
	CHECK(fam_is(0, "status", "registered"));
	CHECK(fam_is(0, "contact", contact));
	CHECK(fam_is(0, "registrar", registrar));
	CHECK(fam_is(0, "targets", targets));
	CHECK(!fam_peek(fam_fd4, wire, sizeof(wire)));
	CHECK(backoff_armed(0, walk[0]));
	/* the attempt that deadline holds asks the question again and leaves
	 * the registration where it is */
	queries = acct_dns()->stat.queries;
	CHECK(acct_test_dns_retry_fire(0));
	CHECK(acct_dns()->stat.queries > queries);
	CHECK_UINT(acct_test_dns_retry_ms(0), 0);
	CHECK(fam_is(0, "state", "registered"));

	/*
	 * The refresh of that line goes where it went before: the request
	 * arrives at the socket of the registrar the status still names. The
	 * registrar answers nothing for the length of the read, so the request
	 * waits there and the case reads it.
	 */
	CASE("backoff: and its refresh REGISTER still reaches the standing "
	     "target");
	fam_registrar_answers(&fam_ufd4, false);
	fam_drain(fam_fd4);
	CHECK(fam_request_read(fam_fd4, "5551000", wire, sizeof(wire)));
	CHECK(has_str(wire, "REGISTER sip:127.0.0.1"));
	snprintf(want, sizeof(want), "%u", fx.reg_port);
	CHECK(fam_is(0, "registrar", registrar));
	CHECK(fam_is(0, "registrar_port", want));
	fam_registrar_answers(&fam_ufd4, true);
	CHECK(fam_wait(0, "state", "registered"));

	flap_end(&fx);

	backoff_local_host_case();
}

/* --------------------------------------------------- two lines, one reload */

/*
 * A field fault needs two enabled lines, and the canonical fixture
 * (flap_start()) parks line 2 off. This group runs a reload that touches line
 * 2 alone while both lines are live. It sits beside the other two-channel
 * live groups, before the account recovery cases of test_netifd_retry().
 */

static unsigned int tlr_regs1;
static unsigned int tlr_regs2;

/*
 * Both lines answer through the one registrar of the fixture. The Contact
 * header alone is read, not the whole datagram: a branch or a Call-ID could
 * coincide with a user string by chance, and the registrar keys a binding on
 * Contact, not on the bytes around it. Line 1's user is the one count that
 * matters to every case below; everything else is counted as line 2's,
 * whatever digits its own reload gave it.
 */
static void tlr_cb(struct uloop_fd *u, unsigned int events)
{
	char contact[256];

	if (!registrar_answer(u->fd, 0) || !fam_last_is_register())
		return;
	if (!req_line_get(registrar_last, "Contact", contact, sizeof(contact)))
		return;

	if (strstr(contact, "5551000"))
		tlr_regs1++;
	else
		tlr_regs2++;
}

static void test_two_line_reload(void)
{
	static struct cfg live;
	static struct cfg step;
	static struct cfg gone;
	char aor0[192];
	char lp0[192];
	char rh0[192];
	uint16_t reg_port, local_port;
	unsigned int mark1, mark2, recs;
	unsigned int i;

	CASE("two lines: a reload that disables line 2 sends no REGISTER from line 1");
	CHECK(uloop_init() == 0);
	reg_port = udp_loopback_open(&fam_fd4);
	local_port = fam_free_port();
	CHECK(reg_port && local_port);
	if (!reg_port || !local_port)
		return;

	tlr_regs1 = 0;
	tlr_regs2 = 0;
	registrar_extra = NULL;
	fam_ufd4.fd = fam_fd4;
	fam_ufd4.cb = tlr_cb;
	CHECK(uloop_fd_add(&fam_ufd4, ULOOP_READ) == 0);

	event_test_mode(true);
	fw_test_mode(true);

	base_default();
	live = base;
	live.channels = 2;
	fam_channel(&live, 0, "127.0.0.1", reg_port, local_port);
	fam_channel(&live, 1, "127.0.0.1", reg_port, local_port);

	core_start(&live);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));
	CHECK_UINT(tlr_regs1, 1);
	CHECK_UINT(status_line0(NULL, 0), 2);
	CHECK(fam_key(0, "aor", aor0, sizeof(aor0)) != NULL);
	CHECK(fam_key(0, "local_port", lp0, sizeof(lp0)) != NULL);
	CHECK(fam_key(0, "resolve_host", rh0, sizeof(rh0)) != NULL);

	/*
	 * event_fire() leaves l->busy set for the length of the test: in test
	 * mode only event_test_reap() clears it, so a later event_reg_post()
	 * on the same line only parks in l->pend and neither event_test_posts()
	 * nor event_test_recs() moves for it. Reaping line 1 here before it is
	 * quiescent makes a later post on it observable in the record.
	 */
	event_test_reap(0, 0);
	recs = event_test_recs();
	mark1 = tlr_regs1;
	mark2 = tlr_regs2;
	step = live;
	step.channel[1].enabled = false;
	core_reload(&live, &step);
	for (i = 0; i < 400 && status_line0(NULL, 0) != 1; i++)
		uloop_run_timeout(20);
	CHECK_UINT(status_line0(NULL, 0), 1);
	/* line 2's own Expires 0 removal, the reload's own traffic */
	CHECK(tlr_regs2 > mark2);
	/* the bounded window a rebuild of line 1 would need to send its own
	 * REGISTER in, past the removal of line 2 that the reload itself owes */
	fam_pump_ms(300);
	CHECK_UINT(tlr_regs1, mark1);

	CASE("two lines: line 1 stays registered and posts no down event");
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_is(0, "aor", aor0));
	CHECK(fam_is(0, "local_port", lp0));
	CHECK(fam_is(0, "resolve_host", rh0));
	/* a plain post count is dead here (event_fire() left line 1 busy at
	 * the reload above), and a plain post count elsewhere would be wrong:
	 * line 2's own down edge from the reload above is legitimate. The
	 * record naming line 1 at all, success or failure, is the oracle. */
	for (; recs < event_test_recs(); recs++)
		CHECK(!has_str(evt_arg(recs, 4), "line-1/reg/"));

	CASE("two lines: a reload that enables line 2 again leaves line 1 untouched");
	event_test_reap(0, 0);
	recs = event_test_recs();
	mark1 = tlr_regs1;
	mark2 = tlr_regs2;
	core_reload(&step, &live);
	CHECK(fam_wait(1, "state", "registered"));
	CHECK(tlr_regs2 > mark2);
	fam_pump_ms(300);
	CHECK_UINT(tlr_regs1, mark1);
	for (; recs < event_test_recs(); recs++)
		CHECK(!has_str(evt_arg(recs, 4), "line-1/reg/"));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_is(0, "aor", aor0));
	CHECK(fam_is(0, "local_port", lp0));
	CHECK(fam_is(0, "resolve_host", rh0));
	CHECK_UINT(status_line0(NULL, 0), 2);

	CASE("two lines: a reload that moves the line 2 user leaves line 1 untouched");
	event_test_reap(0, 0);
	recs = event_test_recs();
	mark1 = tlr_regs1;
	mark2 = tlr_regs2;
	step = live;
	snprintf(step.channel[1].sip_username, sizeof(step.channel[1].sip_username),
		 "5551099");
	core_reload(&live, &step);
	CHECK(fam_wait(1, "state", "registered"));
	CHECK(tlr_regs2 > mark2);
	fam_pump_ms(300);
	CHECK_UINT(tlr_regs1, mark1);
	for (; recs < event_test_recs(); recs++)
		CHECK(!has_str(evt_arg(recs, 4), "line-1/reg/"));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_is(0, "aor", aor0));
	CHECK(fam_is(0, "local_port", lp0));
	CHECK(fam_is(0, "resolve_host", rh0));

	/*
	 * The final drain: longer than GRACE_MS (3 s, account.c) plus the
	 * fixture's sip_register_retry (1 s), so a rebuild of line 1 that a
	 * timer delays past the reload that triggered it still lands inside a
	 * window this group reads. Line 1's own registration_expiry is one
	 * hour, so nothing of its own refreshes here: exactly the one
	 * REGISTER of its start must stand for the whole group.
	 */
	event_test_reap(0, 0);
	recs = event_test_recs();
	fam_pump_ms(4500);
	CHECK_UINT(tlr_regs1, 1);
	for (; recs < event_test_recs(); recs++)
		CHECK(!has_str(evt_arg(recs, 4), "line-1/reg/"));

	gone = step;
	gone.channels = 0;
	core_reload(&step, &gone);
	for (i = 0; i < 400 && status_line0(NULL, 0); i++)
		uloop_run_timeout(20);
	CHECK_UINT(status_line0(NULL, 0), 0);

	uloop_fd_delete(&fam_ufd4);
	close(fam_fd4);
	fam_fd4 = -1;
	uloop_done();
}

/* ------------------------------------- the retry of the local transport */

/*
 * The account level recovery after a WAN address change. Three faults leave
 * a line unable to reach its registrar with nothing behind them to try
 * again: a socket a
 * failed rebind left down, a stream listener that did not come back, and a
 * netifd query ubus refused. One timer for the process retries them, and the
 * cases below drive each fault and read the timer through the two hooks of
 * account.h.
 *
 * The fixture is the outage one: two lines on one loopback registrar, with the
 * netifd answers injected. A socket is put down the way test_sip_transport.c
 * does it, by pointing its configured port at a port another socket holds, and
 * the pool is reached through acct_transport_test().
 */

/*
 * A wildcard UDP port held by a socket of the case, and the number of it. The
 * transports set no SO_REUSEADDR, so a bind of the daemon on this port fails
 * with EADDRINUSE and the failure is genuine, not a privilege artefact. The
 * caller closes the descriptor to give the port back.
 */
static uint16_t udp_port_take(int *fd)
{
	struct sockaddr_in sa;
	socklen_t slen = sizeof(sa);

	*fd = port_hold(AF_INET, SOCK_DGRAM, 0);
	if (*fd < 0)
		return 0;

	if (getsockname(*fd, (struct sockaddr *)&sa, &slen)) {
		close(*fd);
		*fd = -1;

		return 0;
	}

	return ntohs(sa.sin_port);
}

/*
 * The datagram socket the two lines of the fixture share. No line hands its
 * handle out, so the pool is walked; both lines are on one port, one device and
 * one family, so it holds one socket (sip/transport.h).
 */
static struct sip_sock *retry_line_sock(void)
{
	struct sip_transport *tp = acct_transport_test();
	unsigned int i;

	for (i = 0; i < SIP_SOCK_MAX; i++) {
		if (tp->sock[i].in_use)
			return &tp->sock[i];
	}

	return NULL;
}

/*
 * The end of the account layer, run in a child with the parent as its bound:
 * the shutdown drain, or the final teardown, each in the order main.c runs it
 * around the tracker. vr.shutting_down is never cleared and accounts_reload()
 * returns at once while it stands, and the teardown frees the transport and
 * the resolver, so the process that runs either can run no further class. The
 * child reports the timer through its exit status, the child_pass_returns()
 * precedent.
 */
static bool child_teardown_disarms(bool shutdown)
{
	pid_t pid = fork();

	if (pid == 0) {
		if (shutdown) {
			net_iface_quiesce();
			accounts_shutdown_begin();
		} else {
			accounts_done();
			net_iface_done();
		}

		_exit(net_iface_test_recover_ms() ? 1 : 0);
	}

	return child_wait(pid);
}

static void test_netifd_retry(void)
{
	static struct flap_fix fx;
	static struct cfg unknown;
	static struct cfg moved;
	static struct cfg v6on;
	static const struct addr6_row gua6[] = { { "2001:db8:70::5", -1 } };
	struct sip_transport *tp = acct_transport_test();
	struct sip_sock_cfg sc;
	struct sip_sock *sk;
	uint32_t other;
	uint64_t rebinds;
	uint64_t tx_err;
	uint16_t port_other;
	uint16_t held;
	int hold_a = -1;
	int hold_b = -1;
	unsigned int mark;

	CASE("retry: an ifup answer with no address of either family arms the "
	     "retry and keeps the down flag");
	if (!flap_start(&fx, 0))
		return;
	CHECK(fam_count_wait(&fam_regs4, 2));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));
	/* the address answer of the fixture ran the decision point before the
	 * lines were built, so the class starts settled */
	CHECK_UINT(net_iface_test_recover_ms(), 0);

	/*
	 * The interface came back and its answer carries neither family. The
	 * lines hold an address that may be gone, and nothing else asks
	 * netifd again, so the attempt is armed and the down flag stands.
	 */
	event6_inject(V6_IFACE, "ifdown");
	event6_inject(V6_IFACE, "ifup");
	status46_inject(V6_DEV, NULL, NULL, 0);
	CHECK_UINT(net_iface_test_recover_ms(), NET_IFACE_RECOVER_MS);
	/* no socket and no listener is down: the interface is the whole reason */
	CHECK_UINT(sip_transport_down_count(acct_transport()), 0);

	/*
	 * The attempt that waits covers every reason that arrives before it
	 * runs. A second faulty answer must not climb the ladder behind it,
	 * because the state it reports is the state the attempt was armed for.
	 */
	CASE("retry: a second faulty answer while armed leaves the pending "
	     "delay alone");
	status46_inject(V6_DEV, NULL, NULL, 0);
	CHECK_UINT(net_iface_test_recover_ms(), NET_IFACE_RECOVER_MS);
	status46_inject(V6_DEV, NULL, NULL, 0);
	CHECK_UINT(net_iface_test_recover_ms(), NET_IFACE_RECOVER_MS);

	/*
	 * The address the lines already hold. The down flag the case above kept
	 * is what makes it a rebind and a re-registration: without the flag an
	 * unchanged address moves nothing.
	 */
	CASE("retry: the answer that carries the address disarms it and the "
	     "line registers");
	mark = fam_regs4;
	rebinds = sip_transport_stat(acct_transport())->rebinds;
	status46_inject(V6_DEV, "192.0.2.10", NULL, 0);
	CHECK(sip_transport_stat(acct_transport())->rebinds > rebinds);
	CHECK_UINT(net_iface_test_recover_ms(), 0);
	CHECK(fam_count_wait(&fam_regs4, mark + 2));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));

	/*
	 * An interface that came back on IPv6 alone is in service, and the IPv4
	 * half is the only other place that clears the down flag, so the answer
	 * of the voice interface clears it here. IPv6 service means something
	 * to a line only behind the operator gate and a backend that carries
	 * IPv6 media, which is what acct_family_prefer() reads, so the window
	 * below runs with both. The proof of the clear is the answer behind it:
	 * on an address the lines hold, with the flag clear, nothing rebinds.
	 */
	CASE("retry: an IPv6 only interface that came back disarms the retry "
	     "and clears the down flag");
	v6on = fx.live;
	v6on.ipv6_enabled = true;
	accounts_hal_features_set(HAL_FEAT_IPV6_MEDIA);
	core_reload(&fx.live, &v6on);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));
	event6_inject(V6_IFACE, "ifdown");
	status46_inject(V6_DEV, NULL, NULL, 0);
	CHECK_UINT(net_iface_test_recover_ms(), NET_IFACE_RECOVER_MS);
	status46_up_inject(V6_DEV, NULL, ROWS(gua6));
	CHECK(net_key_is("address6", "2001:db8:70::5"));
	CHECK_UINT(net_iface_test_recover_ms(), 0);
	rebinds = sip_transport_stat(acct_transport())->rebinds;
	status46_inject(V6_DEV, "192.0.2.10", NULL, 0);
	CHECK_UINT(sip_transport_stat(acct_transport())->rebinds, rebinds);
	CHECK(fam_wait(0, "state", "registered"));

	/*
	 * The same address record, held by another interface of the netdev
	 * (the wan6 of a split WAN), which stays up while the voice interface
	 * is down. It says nothing about the voice interface, so the flag
	 * stands and the attempt stays armed. Without that rule the IPv4 line
	 * that comes back on its old address would never rebind, which is the
	 * whole reason the flag exists.
	 */
	CASE("retry: an address6 of another interface does not clear the down "
	     "flag");
	event6_inject(V6_IFACE, "ifdown");
	dump6_inject("wan6", true, V6_DEV, ROWS(gua6));
	CHECK(net_key_is("address6", "2001:db8:70::5"));
	status46_inject(V6_DEV, NULL, NULL, 0);
	CHECK_UINT(net_iface_test_recover_ms(), NET_IFACE_RECOVER_MS);
	/* and the flag is still set: the address the lines hold rebinds */
	rebinds = sip_transport_stat(acct_transport())->rebinds;
	mark = fam_regs4;
	status46_inject(V6_DEV, "192.0.2.10", NULL, 0);
	CHECK(sip_transport_stat(acct_transport())->rebinds > rebinds);
	CHECK_UINT(net_iface_test_recover_ms(), 0);
	CHECK(fam_count_wait(&fam_regs4, mark + 2));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));

	/* the window ends: the cases below run with the IPv4 only gate the
	 * fixture starts from */
	core_reload(&v6on, &fx.live);
	accounts_hal_features_set(0);
	status46_inject(V6_DEV, "192.0.2.10", NULL, 0);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));

	/*
	 * A local_interface no netifd interface carries. The netdev name and
	 * the address stay empty for as long as the name stands, and neither
	 * says the transport is broken, so an attempt would repeat for ever
	 * against a state nothing can heal. The inject is that state: an answer
	 * with no netdev and no address.
	 */
	CASE("retry: an interface name netifd does not know arms nothing");
	unknown = fx.live;
	snprintf(unknown.local_interface, sizeof(unknown.local_interface),
		 "wan9");
	core_reload(&fx.live, &unknown);
	status46_inject(NULL, NULL, NULL, 0);
	CHECK_UINT(net_iface_test_recover_ms(), 0);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));
	CHECK_UINT(net_iface_test_recover_ms(), 0);
	core_reload(&unknown, &fx.live);
	status46_inject(V6_DEV, "192.0.2.10", NULL, 0);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));

	/*
	 * The registrar goes silent and an address change puts line 1 in its
	 * backoff: its attempt ends 640 ms after it leaves and the ladder holds
	 * the next one for 20 s. The interface goes down behind that, so an
	 * attempt is armed with nothing of the transport broken. The fire heals
	 * nothing, so it must kick no line: a kick would spend one step of the
	 * ladder and one penalty of the target for nothing.
	 */
	CASE("retry: a timer fire with nothing healed kicks no line");
	fam_registrar_answers(&fam_ufd4, false);
	status46_inject(V6_DEV, "192.0.2.20", NULL, 0);
	CHECK(fam_wait(1, "state", "backoff"));
	event6_inject(V6_IFACE, "ifdown");
	status46_inject(V6_DEV, NULL, NULL, 0);
	CHECK_UINT(net_iface_test_recover_ms(), NET_IFACE_RECOVER_MS);
	fam_drain(fam_fd4);
	CHECK(net_iface_test_recover_fire());
	CHECK_UINT(fam_drain(fam_fd4), 0);
	CHECK(fam_is(1, "state", "backoff"));

	/* the fire above climbed one rung, and the state that armed it stands */
	CASE("retry: consecutive attempts escalate to the cap");
	CHECK_UINT(net_iface_test_recover_ms(), 2 * NET_IFACE_RECOVER_MS);
	CHECK(net_iface_test_recover_fire());
	CHECK_UINT(net_iface_test_recover_ms(), 4 * NET_IFACE_RECOVER_MS);
	CHECK(net_iface_test_recover_fire());
	CHECK_UINT(net_iface_test_recover_ms(), 8 * NET_IFACE_RECOVER_MS);
	CHECK(net_iface_test_recover_fire());
	CHECK_UINT(net_iface_test_recover_ms(), NET_IFACE_RECOVER_MAX_MS);
	CHECK(net_iface_test_recover_fire());
	CHECK_UINT(net_iface_test_recover_ms(), NET_IFACE_RECOVER_MAX_MS);

	/*
	 * Two lines are up and one ladder serves both. The answer that settles
	 * the fault takes the ladder back to its first delay, and the next
	 * fault arms one attempt at that delay: a timer of each line would arm
	 * two and stand at the second rung.
	 */
	CASE("retry: one timer serves the process, whatever the line count");
	fam_registrar_answers(&fam_ufd4, true);
	status46_inject(V6_DEV, "192.0.2.30", NULL, 0);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));
	CHECK_UINT(net_iface_test_recover_ms(), 0);
	event6_inject(V6_IFACE, "ifdown");
	status46_inject(V6_DEV, NULL, NULL, 0);
	CHECK_UINT(net_iface_test_recover_ms(), NET_IFACE_RECOVER_MS);

	/*
	 * The port of the lines' socket is taken by another socket, so the
	 * rebind of the next address change cannot have it back. The socket
	 * stays down, every send of the lines is refused, and the attempt is
	 * armed on the transport and not on the interface.
	 */
	CASE("retry: a rebind that could not take its port arms the retry and "
	     "the socket is down");
	status46_inject(V6_DEV, "192.0.2.40", NULL, 0);
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(fam_wait(1, "state", "registered"));
	CHECK_UINT(net_iface_test_recover_ms(), 0);
	sk = retry_line_sock();
	CHECK(sk != NULL);
	if (!sk) {
		flap_end(&fx);

		return;
	}
	held = udp_port_take(&hold_a);
	CHECK(held != 0);
	sk->cfg.port = held;
	status46_inject(V6_DEV, "192.0.2.50", NULL, 0);
	CHECK(!sip_sock_up(sk));
	CHECK_UINT(sip_transport_down_count(acct_transport()), 1);
	CHECK_UINT(net_iface_test_recover_ms(), NET_IFACE_RECOVER_MS);
	/* the REGISTER of the address change leaves from a socket that is
	 * down, so the send is refused and both lines end in their backoff */
	CHECK(fam_wait(1, "state", "backoff"));
	CHECK(net_iface_test_recover_ms() != 0);

	/*
	 * A second socket of the pool, down as well and free to come back
	 * first. The attempt that brings it back reports a recovery and kicks
	 * the lines, and the lines are on the socket that is still down: a kick
	 * of one of them would be refused at the send and would cost it a step
	 * of its ladder. Then the port of the lines' socket comes back too, and
	 * that attempt rebinds it alone: a rebind of the healthy one would cost
	 * it its conntrack tuple.
	 */
	CASE("retry: the retry rebinds only that socket and the line in backoff "
	     "registers at once");
	memset(&sc, 0, sizeof(sc));
	sc.family = AF_INET;
	sc.addr = "";
	sc.iface = "";
	port_other = fam_free_port();
	sc.port = port_other;
	other = sip_sock_open(tp, &sc);
	CHECK(port_other != 0);
	CHECK(other != SIP_HANDLE_NONE);
	held = udp_port_take(&hold_b);
	CHECK(held != 0);
	sip_sock_get(tp, other)->cfg.port = held;
	CHECK(!sip_sock_rebind(tp, other));
	CHECK_UINT(sip_transport_down_count(acct_transport()), 2);
	close(hold_b);
	hold_b = -1;
	sip_sock_get(tp, other)->cfg.port = port_other;
	tx_err = sip_transport_stat(acct_transport())->tx_err;
	fam_drain(fam_fd4);
	CHECK(net_iface_test_recover_fire());
	CHECK(sip_sock_up(sip_sock_get(tp, other)));
	CHECK(!sip_sock_up(sk));
	CHECK_UINT(sip_transport_stat(acct_transport())->tx_err, tx_err);
	CHECK_UINT(fam_drain(fam_fd4), 0);
	/* one socket is still down, so another attempt is due */
	CHECK(net_iface_test_recover_ms() != 0);

	/*
	 * The other half of the same rule. The second socket goes down again
	 * and the lines' one comes back, so the pass heals one of two: a
	 * recovery is what it is even when the pass leaves a socket down, and
	 * the line on the socket that came back takes its attempt now. Its own
	 * ladder holds the next one for 20 s, so a registration inside the
	 * window below is the kick and nothing else.
	 */
	held = udp_port_take(&hold_b);
	CHECK(held != 0);
	sip_sock_get(tp, other)->cfg.port = held;
	CHECK(!sip_sock_rebind(tp, other));
	close(hold_a);
	hold_a = -1;
	sk->cfg.port = fx.local_port;
	mark = fam_regs4;
	CHECK(net_iface_test_recover_fire());
	CHECK(sip_sock_up(sk));
	CHECK(!sip_sock_up(sip_sock_get(tp, other)));
	CHECK_UINT(sip_transport_down_count(acct_transport()), 1);
	CHECK(fam_count_wait(&fam_regs4, mark + 1));
	CHECK(fam_wait(1, "state", "registered"));

	/*
	 * And the last socket. The lines' socket is healthy now, so the pass
	 * must leave it alone: a rebind of a working socket costs it the
	 * conntrack tuple of its flow.
	 */
	rebinds = sk->stat.rebinds;
	close(hold_b);
	hold_b = -1;
	sip_sock_get(tp, other)->cfg.port = port_other;
	CHECK(net_iface_test_recover_fire());
	CHECK(sip_sock_up(sip_sock_get(tp, other)));
	CHECK_UINT(sk->stat.rebinds, rebinds);
	CHECK_UINT(net_iface_test_recover_ms(), 0);
	sip_sock_close(tp, other);

	/*
	 * A delay the server named is not ours to cut short (sip/reg.h). An
	 * address change sends the REGISTER of both lines, the registrar
	 * refuses line 1 with such a delay, and the socket then goes down and
	 * comes back under it. The socket is put down by hand: the refusal took
	 * the binding, so another address change would rebuild the registration
	 * and take the delay with it.
	 */
	CASE("retry: a line waiting out a Retry-After is left where it is");
	fam_registrar_answers(&fam_ufd4, false);
	fam_drain(fam_fd4);
	status46_inject(V6_DEV, "192.0.2.60", NULL, 0);
	CHECK(fam_refuse(fam_fd4, "5551001", "503 Service Unavailable",
			 "Retry-After: 30"));
	CHECK(fam_wait(1, "state", "backoff"));
	held = udp_port_take(&hold_a);
	CHECK(held != 0);
	sk->cfg.port = held;
	CHECK(!sip_sock_rebind(tp, sip_sock_handle(sk)));
	/* an answer that moves nothing, so the decision point is all that runs */
	status46_inject(V6_DEV, "192.0.2.60", NULL, 0);
	CHECK_UINT(net_iface_test_recover_ms(), NET_IFACE_RECOVER_MS);
	close(hold_a);
	hold_a = -1;
	sk->cfg.port = fx.local_port;
	fam_drain(fam_fd4);
	CHECK(net_iface_test_recover_fire());
	CHECK(sip_sock_up(sk));
	CHECK_UINT(fam_drain_user(fam_fd4, "5551001"), 0);
	CHECK(fam_is(1, "state", "backoff"));

	/*
	 * The interface the attempt was armed for is not the interface of the
	 * line any more: the address, the netdev and the down flag all go with
	 * the old name, so the attempt goes with them.
	 */
	CASE("retry: a voice interface that moves cancels the pending retry");
	fam_registrar_answers(&fam_ufd4, true);
	event6_inject(V6_IFACE, "ifdown");
	status46_inject(V6_DEV, NULL, NULL, 0);
	CHECK_UINT(net_iface_test_recover_ms(), NET_IFACE_RECOVER_MS);
	moved = fx.live;
	snprintf(moved.local_interface, sizeof(moved.local_interface), "wan3");
	mark = fam_removals4;
	core_reload(&fx.live, &moved);
	CHECK_UINT(net_iface_test_recover_ms(), 0);
	/* Line 0 held its binding through the silent window above, so the
	 * reload unregisters it ahead of the rebuild. That exchange ends here,
	 * and the registrar then goes silent so the rebuilt lines take no new
	 * binding: the children below tear down a copy of the daemon, which
	 * takes the sockets out of the epoll set they share with this process,
	 * and the Expires 0 of the last reload would then find no answer and
	 * wait out its grace period. */
	CHECK(fam_count_wait(&fam_removals4, mark + 1));
	fam_registrar_answers(&fam_ufd4, false);
	fam_pump_ms(100);

	/*
	 * The drain owns the sockets from the shutdown on, so no attempt may
	 * rebind one under an unregister that is leaving.
	 */
	CASE("retry: the shutdown cancels it");
	event6_inject("wan3", "ifdown");
	status46_inject(NULL, NULL, NULL, 0);
	CHECK_UINT(net_iface_test_recover_ms(), NET_IFACE_RECOVER_MS);
	CHECK(child_teardown_disarms(true));
	fam_drain(fam_fd4);

	/* the same for the teardown at process exit, which frees the transport
	 * the attempt would rebind */
	CASE("retry: the final teardown cancels it");
	CHECK_UINT(net_iface_test_recover_ms(), NET_IFACE_RECOVER_MS);
	CHECK(child_teardown_disarms(false));
	fam_drain(fam_fd4);

	flap_end(&fx);
}

static void status_lines_check(struct blob_buf *buf, unsigned int count,
			       unsigned int first)
{
	struct blobmsg_policy policy = { "lines", BLOBMSG_TYPE_ARRAY };
	struct blob_attr *array[1] = {};
	struct blob_attr *row, *field;
	size_t rem;
	unsigned int n = 0;

	blobmsg_parse(&policy, 1, array, blob_data(buf->head),
		      blob_len(buf->head));
	CHECK(array[0] != NULL);
	if (!array[0])
		return;

	blobmsg_for_each_attr(row, array[0], rem) {
		field = table_find(row, "line");
		CHECK(field && blobmsg_get_u32(field) == first + n);
		n++;
	}
	CHECK_UINT(n, count);
}

/*
 * The one resolver, read from the call side. The SIP sockets scope themselves
 * through net_iface_bind_device(), and the media fork reads the same answer
 * out of the call view. The two therefore can never bind to two netdevs.
 *
 * The suite runs no netifd on ubus, so no query of a live line gets an
 * answer, and the tracked device would stay empty. The suite therefore drives
 * the tracker through net_iface_test_status_inject(). The base interface is a
 * name that no host carries: the lab case of the shared resolver (logical wan
 * on netdev lan4). Channel 1 names "lo", a device that every host has.
 */
static void test_view_device(void)
{
	static struct cfg live;
	static struct cfg gone;
	struct acct_call_view view;
	struct blob_buf b;
	uint16_t reg_port;
	uint16_t local_port[2];
	int probe_fd[2] = { -1, -1 };
	unsigned int answers;
	unsigned int i;

	CASE("view: two lines come up on a loopback registrar");
	CHECK(uloop_init() == 0);
	reg_port = udp_loopback_open(&registrar_fd);
	CHECK(reg_port != 0);
	if (!reg_port)
		return;

	registrar_ufd.fd = registrar_fd;
	registrar_ufd.cb = registrar_cb;
	CHECK(uloop_fd_add(&registrar_ufd, ULOOP_READ) == 0);

	/*
	 * Two distinct signalling ports: acct_cfg_derive() turns an unset
	 * sip_local_port into 5060, so both lines would ask for one port.
	 * sip/transport.c sets neither SO_REUSEADDR nor SO_REUSEPORT, and
	 * SO_BINDTODEVICE does not change the bound tuple, so the second bind
	 * fails and that line is never built. Both probes stay open until both
	 * ports are known, so the pair cannot repeat.
	 */
	local_port[0] = udp_loopback_open(&probe_fd[0]);
	local_port[1] = udp_loopback_open(&probe_fd[1]);
	CHECK(local_port[0] != 0 && local_port[1] != 0);
	for (i = 0; i < 2; i++)
		close(probe_fd[i]);

	base_default();
	live = base;
	live.channels = 2;
	snprintf(live.local_interface, sizeof(live.local_interface), "uvoice-nodev0");

	for (i = 0; i < 2; i++) {
		struct cfg_channel *ch = &live.channel[i];

		chan_default(ch, i);
		snprintf(ch->sip_username, sizeof(ch->sip_username), "555100%u", i);
		snprintf(ch->sip_domain, sizeof(ch->sip_domain), "127.0.0.1");
		snprintf(ch->sip_proxy, sizeof(ch->sip_proxy), "127.0.0.1");
		snprintf(ch->sip_registrar, sizeof(ch->sip_registrar), "127.0.0.1");
		snprintf(ch->sip_outbound_proxy, sizeof(ch->sip_outbound_proxy), "127.0.0.1");
		ch->sip_outbound_proxy_port = reg_port;
		ch->sip_registrar_port = reg_port;
		ch->sip_port = reg_port;
		ch->sip_local_port = local_port[i];
	}
	snprintf(live.channel[1].local_interface, sizeof(live.channel[1].local_interface),
		 "lo");

	event_test_mode(true);
	fw_test_mode(true);
	core_start(&live);
	for (i = 0; i < 400 && registrar_answers < 2; i++)
		uloop_run_timeout(20);
	CHECK_UINT(registrar_answers, 2);
	CHECK_UINT(status_line0(NULL, 0), 2);

	CASE("view: a line on the base interface carries the tracked netdev");
	answers = registrar_answers;
	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "l3_device", "lo");
	net_iface_test_status_inject(b.head);
	blob_buf_free(&b);
	CHECK(account_call_view(0, &view));
	CHECK(bind_is(view.bind_dev, "lo"));

	CASE("net bind device: the base interface resolves to the tracked "
	     "netdev");
	CHECK(bind_is(net_iface_bind_device(live.local_interface), "lo"));

	/* The status reply composes the two tables: the lines of the protocol
	 * and the network table of the core tracker (ubus.c voice_status). */
	CASE("status dump: all lines and the core network table");
	blob_buf_init(&b, 0);
	accounts_status_dump(&b, -1);
	net_iface_status_dump(&b);
	status_lines_check(&b, 2, 0);
	CHECK(table_str_is(dump_table(&b, "network"), "l3_device", "lo"));

	CASE("account dump: minus two includes all lines and no network table");
	blob_buf_init(&b, 0);
	accounts_status_dump(&b, -2);
	status_lines_check(&b, 2, 0);
	CHECK(!dump_has_key(&b, "network"));

	CASE("account dump: out-of-range line emits no rows or network");
	blob_buf_init(&b, 0);
	accounts_status_dump(&b, CFG_CHANNEL_MAX);
	status_lines_check(&b, 0, 0);
	CHECK(!dump_has_key(&b, "network"));

	CASE("account dump: line zero excludes other lines and network");
	blob_buf_init(&b, 0);
	accounts_status_dump(&b, 0);
	status_lines_check(&b, 1, 0);
	CHECK(!dump_has_key(&b, "network"));

	CASE("account dump: line one excludes other lines and network");
	blob_buf_init(&b, 0);
	accounts_status_dump(&b, 1);
	status_lines_check(&b, 1, 1);
	CHECK(!dump_has_key(&b, "network"));

	CASE("network dump: resolved network emits its own table");
	blob_buf_init(&b, 0);
	net_iface_status_dump(&b);
	CHECK(table_str_is(dump_table(&b, "network"), "l3_device", "lo"));
	CHECK(!dump_has_key(&b, "lines"));
	blob_buf_free(&b);

	/* netifd_status_cb() returns on the missing ipv4-address before the
	 * rebind, so the answer stores a netdev and moves nothing. */
	CASE("view: a device-only netifd answer restarts no registration");
	uloop_run_timeout(20);
	CHECK_UINT(registrar_answers, answers);
	CHECK(!registrar_pending());

	CASE("view: a netdev the kernel does not know leaves the media unscoped");
	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "l3_device", "uvoice-nodev1");
	net_iface_test_status_inject(b.head);
	blob_buf_free(&b);
	CHECK(account_call_view(0, &view));
	CHECK(bind_is(view.bind_dev, NULL));

	CASE("net bind device: an unknown tracked netdev leaves no device");
	CHECK(bind_is(net_iface_bind_device(live.local_interface), NULL));

	CASE("view: the second line resolves its own interface");
	CHECK(account_call_view(1, &view));
	CHECK(bind_is(view.bind_dev, "lo"));

	gone = live;
	gone.channels = 0;
	core_reload(&live, &gone);
	for (i = 0; i < 400 && status_line0(NULL, 0); i++)
		uloop_run_timeout(20);
	CHECK_UINT(status_line0(NULL, 0), 0);

	CASE("account dump: inactive selected line emits an empty array");
	blob_buf_init(&b, 0);
	accounts_status_dump(&b, 0);
	status_lines_check(&b, 0, 0);
	CHECK(!dump_has_key(&b, "network"));
	blob_buf_free(&b);

	/*
	 * No accounts_shutdown_begin() here, unlike the class below. The reload
	 * above already gave both lines back, so the call would do nothing but
	 * set vr.shutting_down, and accounts_reload() returns at once on that
	 * flag (account.c): every reload of test_grace_reload() would be
	 * refused.
	 */
	uloop_fd_delete(&registrar_ufd);
	close(registrar_fd);
	registrar_fd = -1;
	uloop_done();
	registrar_answers = 0;
}

/* ------------------------------------------ the service route (RFC 3608) */

/* what the registrar grants, and the block the account re-emits from it */
#define SR_LINE		"Service-Route: <sip:p1.example.net;lr>, " \
			"<sip:p2.example.net;lr>"
#define SR_BLOCK	"Route: <sip:p1.example.net;lr>\r\n" \
			"Route: <sip:p2.example.net;lr>\r\n"
/* what the second registrar grants, so a stale route names its own host */
#define SR_STALE	"Service-Route: <sip:pb.stale.example;lr>"

static unsigned int sr_regs_a;
static unsigned int sr_regs_b;
static unsigned int sr_subs;
static char sr_sub_last[2048];

/*
 * The request a fake registrar of the class below just answered, counted. A
 * REGISTER and the SUBSCRIBE of the subscription both reach these sockets and
 * the cases read them apart, so the SUBSCRIBE is kept whole: the case that
 * reads it reads its header block.
 */
static void sr_req_take(unsigned int *regs)
{
	if (!strncmp(registrar_last, "REGISTER ", 9)) {
		(*regs)++;

		return;
	}

	if (strncmp(registrar_last, "SUBSCRIBE ", 10))
		return;

	sr_subs++;
	snprintf(sr_sub_last, sizeof(sr_sub_last), "%s", registrar_last);
}

static void sr_cb_a(struct uloop_fd *u, unsigned int events)
{
	if (registrar_answer(u->fd, 0))
		sr_req_take(&sr_regs_a);
}

static void sr_cb_b(struct uloop_fd *u, unsigned int events)
{
	if (registrar_answer(u->fd, 0))
		sr_req_take(&sr_regs_b);
}

/*
 * fam_count_wait() over a window wide enough for a transaction at a port
 * nothing answers to time out. The ladder reaches the next priority only
 * after that end, and one fam_count_wait() window is shorter than it.
 */
static bool sr_count_wait(const unsigned int *counter, unsigned int want)
{
	unsigned int i;

	for (i = 0; i < 4 && !fam_count_wait(counter, want); i++)
		;

	return *counter >= want;
}

/* the sender of the last SUBSCRIBE sr_sub_take() read */
static struct sockaddr_storage sr_sub_peer;
static socklen_t sr_sub_plen;

/*
 * Every datagram waiting at a silent registrar, read and discarded, and how
 * many of them were a SUBSCRIBE. The last one is kept whole with its sender:
 * the case that counts them reads its header block, and it answers it so the
 * subscription leaves its restart run behind.
 */
static unsigned int sr_sub_take(int fd)
{
	char buf[2048];
	struct sockaddr_storage peer;
	socklen_t plen;
	unsigned int subs = 0;
	ssize_t n;

	for (;;) {
		plen = sizeof(peer);
		n = recvfrom(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT,
			     (struct sockaddr *)&peer, &plen);
		if (n <= 0)
			break;

		buf[n] = 0;
		if (strncmp(buf, "SUBSCRIBE ", 10))
			continue;

		subs++;
		snprintf(sr_sub_last, sizeof(sr_sub_last), "%s", buf);
		sr_sub_peer = peer;
		sr_sub_plen = plen;
	}

	return subs;
}

/* passes until a SUBSCRIBE waits at a silent registrar, or the bound gives up */
static unsigned int sr_sub_wait(int fd)
{
	unsigned int i, subs = 0;

	for (i = 0; i < 400 && !subs; i++) {
		uloop_run_timeout(20);
		subs += sr_sub_take(fd);
	}

	return subs;
}

/*
 * The answer the silent registrar owes that SUBSCRIBE. RFC 6665 bounds the
 * fresh identities a client mints in a row (SIP_SUB_RESTART_MAX), and a 2xx
 * is what ends the run, so a case that leaves one unanswered would cost the
 * next case its restart.
 */
static bool sr_sub_answer(int fd)
{
	return registrar_reply(fd, sr_sub_last, 0,
			       (const struct sockaddr *)&sr_sub_peer,
			       sr_sub_plen, "200 OK", NULL);
}

/* the value of one header line the reader returned, name and colon cut */
static const char *sr_hdr_value(const char *line)
{
	const char *p = strchr(line, ':');

	if (!p)
		return line;

	for (p++; *p == ' ' || *p == '\t'; p++)
		;

	return p;
}

/*
 * The notifier ends the subscription. RFC 6665 makes that state final, so
 * sub.c mints a fresh identity and sends an initial SUBSCRIBE at once
 * (sub_restart), which is the only request of that shape a case can ask for
 * at a moment of its own choosing.
 *
 * The identity is the one the last SUBSCRIBE carried with the roles swapped,
 * and the notifier tag is the one registrar_reply() puts in every answer. The
 * sender is a socket of its own: the 200 the daemon writes back goes to the
 * Via sent-by, and no registrar of this class may read it.
 */
static bool sr_notify_send(int fd, uint16_t from_port, uint16_t to_port,
			   const char *user)
{
	/* a branch and a CSeq of its own per NOTIFY: two requests that shared
	 * a branch would be one server transaction, and the second would earn
	 * the retained answer of the first instead of reaching this file */
	static unsigned int seq;
	char call_id[256];
	char from[256];
	char msg[1024];
	struct sockaddr_in sa;
	const char *tag;

	if (!req_line_get(sr_sub_last, "Call-ID", call_id, sizeof(call_id)) ||
	    !req_line_get(sr_sub_last, "From", from, sizeof(from)))
		return false;

	tag = strstr(from, ";tag=");
	if (!tag)
		return false;

	seq++;
	snprintf(msg, sizeof(msg),
		 "NOTIFY sip:%s@127.0.0.1:%u SIP/2.0\r\n"
		 "Via: SIP/2.0/UDP 127.0.0.1:%u;branch=z9hG4bK-sr-notify-%u\r\n"
		 "Max-Forwards: 70\r\n"
		 "From: <sip:%s@127.0.0.1>;tag=graceregistrar\r\n"
		 "To: <sip:%s@127.0.0.1>%s\r\n"
		 "Call-ID: %s\r\n"
		 "CSeq: %u NOTIFY\r\n"
		 "Event: message-summary\r\n"
		 "Subscription-State: terminated;reason=noresource\r\n"
		 "Content-Length: 0\r\n\r\n",
		 user, to_port, from_port, seq, user, user, tag,
		 sr_hdr_value(call_id), seq);

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(to_port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	return sendto(fd, msg, strlen(msg), 0, (const struct sockaddr *)&sa,
		      sizeof(sa)) > 0;
}

/* An unsolicited message summary for user, the shape every shipped switch
 * sends: no subscription dialog, a body that says a message waits. */
static bool sr_mwi_send(int fd, uint16_t from_port, uint16_t to_port,
			const char *user)
{
	static const char body[] = "Messages-Waiting: yes\r\n"
				   "Voice-Message: 2/1\r\n";
	char msg[1024];
	struct sockaddr_in sa;

	snprintf(msg, sizeof(msg),
		 "NOTIFY sip:%s@127.0.0.1:%u SIP/2.0\r\n"
		 "Via: SIP/2.0/UDP 127.0.0.1:%u;branch=z9hG4bK-sr-mwi-1\r\n"
		 "Max-Forwards: 70\r\n"
		 "From: <sip:%s@127.0.0.1>;tag=mwiswitch\r\n"
		 "To: <sip:%s@127.0.0.1>\r\n"
		 "Call-ID: sr-mwi-1\r\n"
		 "CSeq: 1 NOTIFY\r\n"
		 "Event: message-summary\r\n"
		 "Content-Type: application/simple-message-summary\r\n"
		 "Content-Length: %zu\r\n\r\n%s",
		 user, to_port, from_port, user, user, strlen(body), body);

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(to_port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	return sendto(fd, msg, strlen(msg), 0, (const struct sockaddr *)&sa,
		      sizeof(sa)) > 0;
}

/* the view's service route, or NULL, for the line this class builds */
static const struct sip_route_hdrs *sr_view_route(void)
{
	struct acct_call_view view;

	if (!account_call_view(0, &view))
		return NULL;

	return view.service_route;
}

/* passes until the view carries a service route, or until the bound gives up */
static bool sr_view_wait(void)
{
	unsigned int i;

	for (i = 0; i < 400; i++) {
		if (sr_view_route())
			return true;

		uloop_run_timeout(20);
	}

	return sr_view_route() != NULL;
}

/*
 * RFC 3608 through the account layer, and the priority order that a field
 * defect turned on. The route that the registrar granted
 * reaches the call view and the subscription. It stops as soon as the
 * selection leaves the server that granted the binding (account.c
 * acct_service_route_of). A Route that names one SBC on an INVITE to another
 * SBC is that defect.
 *
 * The line starts on a port with no answer, so the priority pair has no
 * binding to roll back to and uses the plain ladder. The class runs after
 * test_view_device(), which closed its own uloop, and gives its line back
 * before test_grace_reload() takes over.
 */
static void test_view_service_route(void)
{
	static const uint16_t best_first[2] = { 10, 20 };
	static struct cfg live;
	static struct cfg mode0;
	static struct cfg none;
	struct acct_call_view view;
	struct sip_dns_result res;
	struct fam_target pair[2];
	uint16_t reg_a, reg_b, local_port, dead_port, notify_port;
	int fd_b = -1;
	int notify_fd = -1;
	unsigned int i, mark;

	CASE("view: a priority 0 timeout registers at priority 1");
	CHECK(uloop_init() == 0);
	reg_a = udp_loopback_open(&fam_fd4);
	reg_b = udp_loopback_open(&fd_b);
	local_port = fam_free_port();
	dead_port = fam_free_port();
	/* the sender of the terminating NOTIFY, out of the loop so nothing
	 * reads the answer the daemon writes back to it */
	notify_port = udp_loopback_open(&notify_fd);
	CHECK(reg_a && reg_b && local_port && dead_port && notify_port);
	if (!reg_a || !reg_b || !local_port || !dead_port || !notify_port)
		return;

	sr_regs_a = 0;
	sr_regs_b = 0;
	sr_subs = 0;
	registrar_extra = SR_LINE;
	fam_ufd4.fd = fam_fd4;
	fam_ufd4.cb = sr_cb_a;
	fam_ufd4b.fd = fd_b;
	fam_ufd4b.cb = sr_cb_b;
	CHECK(uloop_fd_add(&fam_ufd4, ULOOP_READ) == 0);
	CHECK(uloop_fd_add(&fam_ufd4b, ULOOP_READ) == 0);

	event_test_mode(true);
	fw_test_mode(true);

	base_default();
	live = base;
	live.channels = 1;
	/* the registration and the subscription are all that reaches these
	 * sockets: no source restriction to reconcile and no keepalive */
	live.restrict_source = 0;
	/* a moved answer re-registers at once, which every case below the
	 * last one needs */
	live.dns_cache_update_mode = true;
	live.mwi_subscribe = true;
	fam_channel(&live, 0, "127.0.0.1", dead_port, local_port);
	none = live;
	none.channels = 0;

	core_start(&live);
	CHECK(fam_wait(0, "state", "trying"));
	pair[0].host = "127.0.0.1";
	pair[0].port = dead_port;
	pair[1].host = "127.0.0.1";
	pair[1].port = reg_b;
	fam_answer(&res, pair, 2, true);
	fam_answer_prio(&res, best_first);
	acct_dns_result_inject(0, &res);
	CHECK(sr_count_wait(&sr_regs_b, 1));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK_UINT(sr_regs_a, 0);

	CASE("view: and the call view destination is the priority 1 address");
	CHECK(account_call_view(0, &view));
	CHECK(view.dst_valid);
	CHECK_UINT(sip_dst_port(view.dst), reg_b);

	CASE("view: the service route the registrar granted reaches the call "
	     "view");
	CHECK(sr_view_route() != NULL);
	if (sr_view_route()) {
		CHECK_UINT(sr_view_route()->hops, 2);
		CHECK(!strcmp(sr_view_route()->text, SR_BLOCK));
	}

	/*
	 * account_reg_start() builds the subscription's configuration before
	 * the first 2xx, and sip_sub_init() copies it by value, so a pointer
	 * read once at that moment would leave this SUBSCRIBE without a route
	 * for ever (sip/sub.h).
	 */
	CASE("view: the first SUBSCRIBE after the first 200 carries the "
	     "granted service route");
	CHECK(fam_count_wait(&sr_subs, 1));
	CHECK(strstr(sr_sub_last, SR_BLOCK) != NULL);

	CASE("view: two priorities register at priority 0");
	mark = sr_regs_a;
	pair[0].port = reg_a;
	pair[1].port = reg_b;
	fam_answer(&res, pair, 2, true);
	fam_answer_prio(&res, best_first);
	acct_dns_result_inject(0, &res);
	CHECK(fam_count_wait(&sr_regs_a, mark + 1));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(account_call_view(0, &view));
	CHECK(view.dst_valid);
	CHECK_UINT(sip_dst_port(view.dst), reg_a);

	/*
	 * A better priority the answer brought moves the selection at once,
	 * while the binding stays where the 2xx left it. The view is read with
	 * no uloop pass, so it is read inside that window.
	 */
	CASE("view: a selection that left the binding server carries no "
	     "service route");
	pair[0].port = dead_port;
	pair[1].port = reg_a;
	fam_answer(&res, pair, 2, true);
	fam_answer_prio(&res, best_first);
	acct_dns_result_inject(0, &res);
	CHECK(account_call_view(0, &view));
	CHECK(view.service_route == NULL);

	/* the attempt at the dead member fails, the binding's server is still
	 * in the answer, and the rollback takes the selection back to it */
	CASE("view: the service route returns with the selection");
	CHECK(sr_view_wait());
	CHECK(sr_view_route() != NULL);
	if (sr_view_route())
		CHECK_UINT(sr_view_route()->hops, 2);
	CHECK(fam_is(0, "state", "registered"));

	/*
	 * The notifier ends the subscription, so sub.c mints a fresh identity
	 * and the SUBSCRIBE that follows is out of dialog again. It goes to
	 * the server the binding belongs to, so it takes the route.
	 */
	CASE("view: a SUBSCRIBE restarted after the rollback carries the "
	     "route again");
	mark = sr_subs;
	CHECK(sr_notify_send(notify_fd, notify_port, local_port, "5551000"));
	CHECK(fam_count_wait(&sr_subs, mark + 1));
	CHECK(strstr(sr_sub_last, SR_BLOCK) != NULL);

	/* the store is the core's (host->mwi_set), and the machine of the line
	 * takes the audible choice from it (sip_line_mwi_push) */
	CASE("view: a message summary reaches the store and the line machine");
	mark = mwi_pushes;
	phf.n[PHF_MWI_SET] = 0;
	CHECK(sr_mwi_send(notify_fd, notify_port, local_port, "5551000"));
	CHECK(fam_count_wait(&mwi_pushes, mark + 1));
	CHECK_UINT(phf.n[PHF_MWI_SET], 1);
	CHECK_UINT(mwi_push_line, 0);

	/*
	 * The window no owner callback reports. resp_ok() of reg.c stores the
	 * new binding and the new route of the answering server and then
	 * returns through the Contact correction with no callback at all, so
	 * an account that watched the callbacks would still be holding the
	 * route of the server the selection left. Both registrars go silent:
	 * the case owns every answer, and the corrective REGISTER cannot
	 * complete and repair the state under the SUBSCRIBE.
	 */
	CASE("view: a Contact correction at the new server leaves the "
	     "SUBSCRIBE toward the old one without a route");
	{
		char rbuf[2048];
		struct sockaddr_storage rpeer;
		socklen_t rplen = sizeof(rpeer);
		ssize_t rn = -1;

		fam_registrar_answers(&fam_ufd4, false);
		fam_registrar_answers(&fam_ufd4b, false);
		fam_drain(fam_fd4);
		fam_drain(fd_b);

		/* the selection moves to B and its move REGISTER waits */
		pair[0].port = reg_b;
		pair[1].port = reg_a;
		fam_answer(&res, pair, 2, true);
		fam_answer_prio(&res, best_first);
		acct_dns_result_inject(0, &res);
		for (i = 0; i < 200 && rn <= 0; i++) {
			uloop_run_timeout(20);
			rplen = sizeof(rpeer);
			rn = recvfrom(fd_b, rbuf, sizeof(rbuf) - 1,
				      MSG_DONTWAIT, (struct sockaddr *)&rpeer,
				      &rplen);
			if (rn > 0 && strncmp(rbuf, "REGISTER ", 9))
				rn = -1;
		}
		CHECK(rn > 0);
		if (rn > 0)
			rbuf[rn] = 0;

		/* the next answer puts the selection back on A, under the
		 * request B still owes */
		pair[0].port = reg_a;
		pair[1].port = reg_b;
		fam_answer(&res, pair, 2, true);
		fam_answer_prio(&res, best_first);
		acct_dns_result_inject(0, &res);
		CHECK(account_call_view(0, &view));
		CHECK_UINT(sip_dst_port(view.dst), reg_a);

		/* B grants the binding, a route of its own, and an rport that
		 * moves the Contact, which is the arm that reports nothing */
		CHECK(registrar_reply(fd_b, rbuf, 40123,
				      (const struct sockaddr *)&rpeer, rplen,
				      "200 OK", SR_STALE));
		for (i = 0; i < 10; i++)
			uloop_run_timeout(20);

		CHECK(sr_notify_send(notify_fd, notify_port, local_port,
				     "5551000"));
		CHECK(sr_sub_wait(fam_fd4) > 0);
		CHECK(strstr(sr_sub_last, "pb.stale.example") == NULL);
		CHECK(strstr(sr_sub_last, "\r\nRoute:") == NULL);
		CHECK(sr_sub_answer(fam_fd4));

		/* the 2xx above moved the binding to B, so both registrars
		 * answer again and the removal B owes A runs out here */
		fam_registrar_answers(&fam_ufd4, true);
		fam_registrar_answers(&fam_ufd4b, true);
		CHECK(fam_wait(0, "state", "registered"));
	}

	/*
	 * The one request shape that can carry the route toward a server the
	 * binding does not belong to. The silent registrar keeps the
	 * SUBSCRIBE in its socket, so the case reads what left the daemon.
	 */
	CASE("view: an initial SUBSCRIBE toward another server carries no "
	     "service route");
	/* the case owns its starting point: the answer above left the binding
	 * at B, so the selection joins it there and the route is B's */
	mark = sr_regs_b;
	pair[0].port = reg_b;
	pair[1].port = reg_a;
	fam_answer(&res, pair, 2, true);
	fam_answer_prio(&res, best_first);
	acct_dns_result_inject(0, &res);
	CHECK(fam_count_wait(&sr_regs_b, mark + 1));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(sr_view_route() != NULL);

	/* the selection alone leaves for A, which answers nothing, so the
	 * binding stays at B for as long as the case needs it there */
	fam_registrar_answers(&fam_ufd4, false);
	fam_drain(fam_fd4);
	pair[0].port = reg_a;
	pair[1].port = reg_b;
	fam_answer(&res, pair, 2, true);
	fam_answer_prio(&res, best_first);
	acct_dns_result_inject(0, &res);
	CHECK(sr_notify_send(notify_fd, notify_port, local_port, "5551000"));
	CHECK(sr_sub_wait(fam_fd4) > 0);
	CHECK(strstr(sr_sub_last, "SUBSCRIBE ") == sr_sub_last);
	CHECK(strstr(sr_sub_last, "\r\nRoute:") == NULL);
	CHECK(sr_sub_answer(fam_fd4));

	/* A answers the move it owes, and grants no route with it */
	CASE("view: a 200 without one clears the view's service route");
	registrar_extra = NULL;
	mark = sr_regs_a;
	fam_registrar_answers(&fam_ufd4, true);
	CHECK(sr_count_wait(&sr_regs_a, mark + 1));
	/* the binding follows the answer, not the first request A reads: the
	 * removal B owes travels the same window */
	CHECK(sr_view_wait());
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(sr_view_route() != NULL);
	if (sr_view_route())
		CHECK_UINT(sr_view_route()->hops, 0);

	/*
	 * Rule 7a without a failure anywhere. With the cache update mode off
	 * a moved answer sends no REGISTER, so the selection sits at the new
	 * member and the binding stays at the old one for as long as the
	 * answer stands. The reload is a REREG class, so the line is rebuilt
	 * and resolves its literal again: the literal names A here.
	 */
	CASE("view: with the cache update mode off a moved selection carries "
	     "no service route");
	registrar_extra = SR_LINE;
	mode0 = live;
	mode0.dns_cache_update_mode = false;
	fam_channel(&mode0, 0, "127.0.0.1", reg_a, local_port);
	mark = sr_regs_a;
	core_reload(&live, &mode0);
	CHECK(fam_count_wait(&sr_regs_a, mark + 1));
	CHECK(fam_wait(0, "state", "registered"));
	CHECK(sr_view_route() != NULL);
	mark = sr_regs_b;
	pair[0].port = reg_b;
	pair[1].port = reg_a;
	fam_answer(&res, pair, 2, true);
	fam_answer_prio(&res, best_first);
	acct_dns_result_inject(0, &res);
	CHECK(account_call_view(0, &view));
	CHECK_UINT(sip_dst_port(view.dst), reg_b);
	CHECK(view.service_route == NULL);
	/* and it stays that way: no attempt fails and nothing rolls back */
	for (i = 0; i < 20; i++)
		uloop_run_timeout(20);
	CHECK(sr_view_route() == NULL);
	CHECK_UINT(sr_regs_b, mark);

	core_reload(&mode0, &none);
	for (i = 0; i < 400 && status_line0(NULL, 0); i++)
		uloop_run_timeout(20);
	CHECK_UINT(status_line0(NULL, 0), 0);

	uloop_fd_delete(&fam_ufd4);
	uloop_fd_delete(&fam_ufd4b);
	close(fam_fd4);
	close(fd_b);
	close(notify_fd);
	fam_fd4 = -1;
	uloop_done();
}

static void test_grace_reload(void)
{
	static struct cfg live;
	static struct cfg moved;
	static struct cfg gone;
	struct sip_dns_result res;
	struct cfg_channel *ch;
	char state[32];
	uint16_t reg_port;
	unsigned int answers;
	int next;

	CASE("grace: the live line registers against a loopback registrar");
	CHECK(uloop_init() == 0);
	reg_port = udp_loopback_open(&registrar_fd);
	CHECK(reg_port != 0);
	if (!reg_port)
		return;

	registrar_ufd.fd = registrar_fd;
	registrar_ufd.cb = registrar_cb;
	CHECK(uloop_fd_add(&registrar_ufd, ULOOP_READ) == 0);

	base_default();
	live = base;
	live.channels = 1;
	ch = &live.channel[0];
	chan_default(ch, 0);
	snprintf(ch->sip_username, sizeof(ch->sip_username), "5551000");
	snprintf(ch->sip_domain, sizeof(ch->sip_domain), "127.0.0.1");
	snprintf(ch->sip_proxy, sizeof(ch->sip_proxy), "127.0.0.1");
	snprintf(ch->sip_registrar, sizeof(ch->sip_registrar), "127.0.0.1");
	snprintf(ch->sip_outbound_proxy, sizeof(ch->sip_outbound_proxy), "127.0.0.1");
	ch->sip_outbound_proxy_port = reg_port;
	ch->sip_registrar_port = reg_port;
	ch->sip_port = reg_port;
	/* an ephemeral local bind: the socket layer reads the real port back
	 * (sip/transport.c), so no fixed port can be lost to another run */
	ch->sip_local_port = 0;

	event_test_mode(true);
	fw_test_mode(true);
	core_start(&live);
	CHECK(posts_wait(1));
	CHECK_UINT(registrar_answers, 1);
	CHECK(!strcmp(evt_arg(0, 4), "voip/nodeMAC/line-1/reg/successful"));

	/* nothing of the cases above is left due, or the reads below would be
	 * answering about somebody else's timer */
	next = uloop_get_next_timeout();
	if (next <= 0)
		printf("    fail a stale timeout is due before the window: %d\n", next);
	CHECK(next > 0);

	/*
	 * The line is registered, so account_graceful_replace() takes its grace
	 * arm: pending, an Expires 0 on the wire, and the grace timer at
	 * GRACE_MS. The registrar answers that REGISTER, stop_finish() drops
	 * the expiry to 0 and calls back, and reg_unregistered_cb() re-arms the
	 * same timer at 0 because no call is up. All of it lands in an event
	 * phase, and uloop_run_timeout() returns after one phase of each, so
	 * the timer is armed, linked and unfired when the wait ends.
	 */
	CASE("grace: the unregister of a reload leaves the grace timer due");
	moved = live;
	moved.srtp_enabled = !live.srtp_enabled;
	CHECK(grace_window_arm(&live, &moved));
	CHECK_UINT(registrar_answers, 2);
	CHECK(!strcmp(evt_arg(1, 4), "voip/nodeMAC/line-1/reg/failed"));

	/*
	 * A DNS answer inside the window. account_dns_result()'s re-register
	 * pair is gated on the pending action like every other registration
	 * restart in the file, and an answer can still arrive here because the
	 * grace arm cancels the resolve timer without releasing the query. A
	 * restart here would leave a running client with an expiry of 0, and
	 * the teardown behind it would then call back into
	 * reg_unregistered_cb() from inside itself and re-arm the very timer
	 * the teardown exists to cancel. The pending gate keeps that shut.
	 * Both observables are read without a pass, because a pass would fire
	 * the 0 timer and close the window: nothing may leave for the
	 * registrar, and the registration must still read unregistered.
	 */
	CASE("grace: a DNS answer inside the window restarts no registration");
	CHECK(live.dns_cache_update_mode);
	answers = registrar_answers;
	memset(&res, 0, sizeof(res));
	res.status = SIP_DNS_OK;
	res.count = 1;
	res.ttl = 60;
	/* changed is the pair's own trigger beside dns_cache_update_mode */
	res.changed = true;
	snprintf(res.target[0].host, sizeof(res.target[0].host), "127.0.0.1");
	res.target[0].port = reg_port;
	CHECK(sip_dst_set(&res.target[0].dst, "127.0.0.1", reg_port));
	acct_dns_result_inject(0, &res);
	CHECK_UINT(registrar_answers, answers);
	CHECK(!registrar_pending());
	status_line0(state, sizeof(state));
	if (strcmp(state, "unregistered"))
		printf("    fail the registration restarted: state %s\n", state);
	CHECK(!strcmp(state, "unregistered"));
	/* and the window is still open behind it */
	next = uloop_get_next_timeout();
	CHECK(next == 0);

	/*
	 * Shape one, the rebuild. The reload finds an expiry of 0, takes the
	 * immediate arm of account_graceful_replace() and runs
	 * account_teardown() and then account_build(), whose memset would zero
	 * that armed timer where it lay if the teardown did not cancel it. The
	 * very next insertion, the resolver's own timer three frames down
	 * inside the same build, would then walk into the node's NULL link.
	 */
	CASE("grace: the reload behind it leaves a timeout list uloop can walk");
	CHECK(child_pass_returns(&moved, &live));

	/*
	 * Shape two, the removal, against the same window: the reload above ran
	 * in a child, so the parent's timer is still armed. A removal takes
	 * account_graceful_replace(a, NULL), which tears the line down and
	 * builds nothing, so there is no memset here and no list to corrupt.
	 * What is left behind is the armed timer itself, still carrying the
	 * pending action: the unregister that opened the window armed it at 0
	 * on its own completion, well before this reload. A teardown that
	 * cancels nothing leaves the pass behind the removal to run
	 * account_grace_cb(), which finds the pending action and rebuilds the
	 * line that was just removed, from the config the first reload parked.
	 * The failure reads as the line coming back.
	 */
	CASE("grace: a removal during the window leaves the line removed");
	gone = live;
	gone.channels = 0;
	core_reload(&moved, &gone);
	CHECK_UINT(status_line0(NULL, 0), 0);
	uloop_run_timeout(0);
	CHECK_UINT(status_line0(NULL, 0), 0);
	CHECK(child_pass_returns(NULL, NULL));

	uloop_fd_delete(&registrar_ufd);
	close(registrar_fd);
	registrar_fd = -1;
	accounts_shutdown_begin();
	uloop_done();
}

int main(void)
{
	/* The end of the graceful stop ends the drain loop, the way
	 * proto/proto.c ends proto_stop_wait(). */
	proto_host_fake_reset();
	phf.shutdown_ends_loop = true;
	accounts_host_bind(proto_host_fake());
	sip_wire_host = proto_host_fake();

	test_derive();
	test_derive_v6();
	test_derive_transport();
	test_derive_tls_port();
	test_derive_dns_kind();
	test_transport_token();
	test_refresher_bound();
	test_srtp_transport_warn();
	test_host_port();
	test_derive_endpoint();
	test_apply_class();
	test_reload_action();
	test_base_restart();
	test_status_dump();
	test_status();
	test_reregister();
	test_dns_reload();
	test_linger();
	test_local_host_move();
	test_proxy_collect();
	test_sub_status();
	test_conn_status();
	test_conn_line();
	test_reconnect();
	test_reconnect_refresh_ok();
	test_transport_maps();
	test_sni();
	test_firewall();
	test_firewall_restrict();
	test_firewall_pinhole();
	test_firewall_dns_move();
	test_firewall_redraw();
	test_firewall_inflight();
	test_firewall_retry();
	test_firewall_change_report();
	test_notify_route();
	test_replaces_admission();
	test_reg_events();
	test_early_reload();
	test_tls_cert_trigger();
	/* Both classes below start accounts, so they run after the pre-start
	 * window above and before the three live classes at the end. */
	test_netifd_addr6();
	test_ipv6_gate();
	/* the live family classes: each one starts lines on loopback
	 * registrars, gives them back and closes its uloop */
	test_family_gate();
	test_family_move();
	test_family_flip();
	test_family_stream();
	test_conn_binding();
	test_refresher_round();
	test_rotation_events();
	test_fail_resolve();
	test_view_registered();
	test_linger_round();
	test_linger_fw_timer();
	test_flap_outage();
	test_dns_backoff();
	/* beside the other two-channel live groups, before the account
	 * recovery cases of test_netifd_retry() */
	test_two_line_reload();
	test_netifd_retry();
	/* The three live classes run last, in this order. Each one starts live
	 * accounts and a uloop, gives its lines back and closes its uloop.
	 * test_grace_reload() must be last: it ends with
	 * accounts_shutdown_begin(), and accounts_reload() returns at once
	 * after that call. */
	test_view_device();
	test_view_service_route();
	test_grace_reload();

	return tu_summary("test_account");
}
