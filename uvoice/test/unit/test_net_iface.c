// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the netifd tracker of the core (src/net/iface.c).
 *
 * Isolation. The suite compiles iface.c and trace.c into itself and connects
 * to no ubus: every netifd answer and event reaches the tracker through the
 * three injects of iface.h, the way the ubus completion hands one up. A
 * recorder subscriber stands in for the protocol and counts what each event
 * reports.
 *
 * Time. The recovery timer is driven through its test pair (iface.h
 * net_iface_test_recover_ms and net_iface_test_recover_fire), so no case
 * waits on a clock.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
/* the enum ubus_msg_status values the netifd retry predicate reads */
#include <ubusmsg.h>

#include "net/iface.h"
#include "test_util.h"

/* ------------------------------------------------------------ the recorder */

/*
 * The one subscriber: what each report said, the last state it carried, and
 * the answers the tracker asks for.
 */
static struct {
	unsigned int changed;
	unsigned int recover;
	unsigned int down;
	struct net_iface_change ch;
	struct net_iface_state st;
} rec;

static void rec_changed(void *priv, const struct net_iface_state *st,
			const struct net_iface_change *ch)
{
	rec.changed++;
	rec.ch = *ch;
	rec.st = *st;
}

static void rec_recover(void *priv)
{
	rec.recover++;
}

static unsigned int rec_down_count(void *priv)
{
	return rec.down;
}

static const struct net_iface_sub rec_sub = {
	.changed = rec_changed,
	.recover = rec_recover,
	.down_count = rec_down_count,
};

static void rec_reset(void)
{
	memset(&rec, 0, sizeof(rec));
}

/*
 * A tracker with no record: the interface moves away and back, which drops
 * the address, the netdev, the down flag and the recovery (iface_set), and the
 * subscriber hears nothing of it.
 */
static void tracker_reset(void)
{
	static struct cfg c;

	memset(&c, 0, sizeof(c));
	snprintf(c.local_interface, sizeof(c.local_interface), "wan");
	net_iface_iface_set("reset0");
	net_iface_init(&c);
	net_iface_iface_set("wan");
	net_iface_ipv6_served_set(false);
	net_iface_subscribe(&rec_sub);
	rec_reset();
}

/* ------------------------------------------------------------ the injects */

static void status_inject(const char *l3_device, const char *addr4,
			  const char *addr6, bool up)
{
	struct blob_buf b;
	void *arr, *tbl;

	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	if (l3_device)
		blobmsg_add_string(&b, "l3_device", l3_device);
	if (up)
		blobmsg_add_u8(&b, "up", 1);

	if (addr4) {
		arr = blobmsg_open_array(&b, "ipv4-address");
		tbl = blobmsg_open_table(&b, NULL);
		blobmsg_add_string(&b, "address", addr4);
		blobmsg_add_u32(&b, "mask", 24);
		blobmsg_close_table(&b, tbl);
		blobmsg_close_array(&b, arr);
	}

	if (addr6) {
		arr = blobmsg_open_array(&b, "ipv6-address");
		tbl = blobmsg_open_table(&b, NULL);
		blobmsg_add_string(&b, "address", addr6);
		blobmsg_add_u32(&b, "mask", 64);
		blobmsg_close_table(&b, tbl);
		blobmsg_close_array(&b, arr);
	}

	net_iface_test_status_inject(b.head);
	blob_buf_free(&b);
}

static void dump_inject(const char *iface, const char *l3_device,
			const char *addr6)
{
	struct blob_buf b;
	void *list, *entry, *arr, *tbl;

	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	list = blobmsg_open_array(&b, "interface");
	entry = blobmsg_open_table(&b, NULL);
	blobmsg_add_string(&b, "interface", iface);
	blobmsg_add_u8(&b, "up", 1);
	blobmsg_add_string(&b, "l3_device", l3_device);
	arr = blobmsg_open_array(&b, "ipv6-address");
	tbl = blobmsg_open_table(&b, NULL);
	blobmsg_add_string(&b, "address", addr6);
	blobmsg_add_u32(&b, "mask", 64);
	blobmsg_close_table(&b, tbl);
	blobmsg_close_array(&b, arr);
	blobmsg_close_table(&b, entry);
	blobmsg_close_array(&b, list);
	net_iface_test_dump_inject(b.head);
	blob_buf_free(&b);
}

static void event_inject(const char *iface, const char *action)
{
	struct blob_buf b;

	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "interface", iface);
	blobmsg_add_string(&b, "action", action);
	net_iface_test_event_inject(b.head);
	blob_buf_free(&b);
}

/* One string key of the network table, or NULL when the table or the key is
 * absent. The value is copied out, because the dump is freed here. */
static const char *net_key(const char *key, char *out, size_t olen)
{
	struct blob_attr *cur, *inner;
	const char *got = NULL;
	struct blob_buf b;
	size_t rem, irem;

	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	net_iface_status_dump(&b);

	blob_for_each_attr(cur, b.head, rem) {
		if (strcmp(blobmsg_name(cur), "network"))
			continue;

		blobmsg_for_each_attr(inner, cur, irem) {
			if (strcmp(blobmsg_name(inner), key))
				continue;

			snprintf(out, olen, "%s", blobmsg_get_string(inner));
			got = out;
		}
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

static bool net_table_gone(void)
{
	struct blob_buf b;
	bool gone;

	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	net_iface_status_dump(&b);
	gone = blob_len(b.head) == 0;
	blob_buf_free(&b);

	return gone;
}

/* ------------------------------------------------------- the pure helpers */

/* NULL tolerant: an unscoped answer must compare, not crash the process. */
static bool bind_is(const char *got, const char *want)
{
	if (!got || !want)
		return got == want;

	return !strcmp(got, want);
}

static void test_bind_device(void)
{
	CASE("bind: a logical name the kernel knows scopes the socket");
	CHECK(bind_is(net_iface_bind_device_pick("wan", "wan", "", true, false),
		      "wan"));

	CASE("bind: an unknown logical name takes the tracked netdev");
	CHECK(bind_is(net_iface_bind_device_pick("wan", "wan", "lan4", false,
						 true), "lan4"));

	CASE("bind: the tracked netdev is authoritative over the logical name");
	CHECK(bind_is(net_iface_bind_device_pick("wan", "wan", "lan4", true,
						 true), "lan4"));

	CASE("bind: neither name known leaves the socket unscoped");
	CHECK(bind_is(net_iface_bind_device_pick("wan", "wan", "lan4", false,
						 false), NULL));

	CASE("bind: a tracked netdev the kernel lost falls back to the logical "
	     "name");
	CHECK(bind_is(net_iface_bind_device_pick("wan", "wan", "lan4", true,
						 false), "wan"));

	CASE("bind: an empty tracked netdev is not a device");
	CHECK(bind_is(net_iface_bind_device_pick("wan", "wan", "", true, true),
		      "wan"));
	CHECK(bind_is(net_iface_bind_device_pick("wan", "wan", "", false, true),
		      NULL));

	/*
	 * The tracker follows the base interface only, so its netdev says
	 * nothing about a channel that names its own local_interface: that
	 * channel keeps its logical name.
	 */
	CASE("bind: a per channel interface keeps the logical name");
	CHECK(bind_is(net_iface_bind_device_pick("voice", "wan", "lan4", true,
						 true), "voice"));

	CASE("bind: a per channel interface the kernel does not know is "
	     "unscoped");
	CHECK(bind_is(net_iface_bind_device_pick("voice", "wan", "lan4", false,
						 true), NULL));

	CASE("bind: no interface at all is never scoped");
	CHECK(bind_is(net_iface_bind_device_pick("", "wan", "lan4", true, true),
		      NULL));
	CHECK(bind_is(net_iface_bind_device_pick(NULL, "wan", "lan4", true,
						 true), NULL));
}

/*
 * The presence test behind the bind lookup. It reads sysfs, so the one
 * positive case needs a netdev every host carries and skips where the class
 * directory is not mounted.
 */
static void test_netdev_present(void)
{
	char longname[IF_NAMESIZE + 1];

	CASE("netdev: the loopback device is present");
	if (access("/sys/class/net", F_OK) != 0)
		tu_skip("/sys/class/net is not mounted");
	else
		CHECK(net_iface_netdev_present("lo"));

	CASE("netdev: a name no device carries is absent");
	CHECK(!net_iface_netdev_present("uvoice-nodev0"));

	CASE("netdev: an empty or missing name is absent");
	CHECK(!net_iface_netdev_present(""));
	CHECK(!net_iface_netdev_present(NULL));

	CASE("netdev: the two dot names are not devices even though the lookup "
	     "finds them");
	CHECK(!net_iface_netdev_present("."));
	CHECK(!net_iface_netdev_present(".."));

	CASE("netdev: a path separator, an alias colon or whitespace names no "
	     "device");
	CHECK(!net_iface_netdev_present("../net/lo"));
	CHECK(!net_iface_netdev_present("lo:1"));
	CHECK(!net_iface_netdev_present("lo x"));

	CASE("netdev: a name of IF_NAMESIZE characters is too long");
	memset(longname, 'a', sizeof(longname) - 1);
	longname[sizeof(longname) - 1] = 0;
	CHECK(!net_iface_netdev_present(longname));
}

static void test_iface_rebind(void)
{
	CASE("rebind: a changed address always rebinds");
	CHECK(net_iface_rebind_needed(true, false, false));

	CASE("rebind: an unchanged address after a flap still rebinds");
	CHECK(net_iface_rebind_needed(false, true, false));

	CASE("rebind: an unchanged address with no flap does not rebind");
	CHECK(!net_iface_rebind_needed(false, false, false));

	CASE("rebind: a changed address after a flap rebinds");
	CHECK(net_iface_rebind_needed(true, true, false));

	/*
	 * A failover can hand the same address back on another netdev. The
	 * sockets are scoped to the old one and the rebind reuses that scope,
	 * so the move alone has to drive the rebind; nothing else in the
	 * answer says anything moved.
	 */
	CASE("rebind: a netdev move on an unchanged address rebinds");
	CHECK(net_iface_rebind_needed(false, false, true));

	CASE("rebind: a netdev move with the address moving too rebinds");
	CHECK(net_iface_rebind_needed(true, false, true));

	CASE("rebind: a netdev move during a flap rebinds");
	CHECK(net_iface_rebind_needed(false, true, true));
}

/*
 * The other half of the netifd decision: what a lookup that gave no id says
 * about a recovery attempt. The daemon reaches it through ubus, which this
 * suite does not have, so the cases check the predicate on its own.
 */
static void test_netifd_retry_status(void)
{
	/* ubus answered: the object is absent and no attempt can make it
	 * appear, and netifd runs the query again through its ifup event */
	CASE("retry status: an absent netifd object asks for no attempt");
	CHECK(!net_iface_retry_needed(UBUS_STATUS_NOT_FOUND));
	CHECK(!net_iface_retry_needed(UBUS_STATUS_OK));

	CASE("retry status: a timeout, a refused connection and a system error "
	     "all ask for one");
	CHECK(net_iface_retry_needed(UBUS_STATUS_TIMEOUT));
	CHECK(net_iface_retry_needed(UBUS_STATUS_CONNECTION_FAILED));
	CHECK(net_iface_retry_needed(UBUS_STATUS_SYSTEM_ERROR));
	CHECK(net_iface_retry_needed(UBUS_STATUS_PERMISSION_DENIED));
	CHECK(net_iface_retry_needed(UBUS_STATUS_UNKNOWN_ERROR));
}

/* A destination of family on a port, or a zeroed one for "no address". */
static socklen_t sa_set(struct sockaddr_storage *ss, const char *text,
			uint16_t port)
{
	struct sockaddr_in *in4 = (struct sockaddr_in *)ss;
	struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)ss;

	memset(ss, 0, sizeof(*ss));

	if (inet_pton(AF_INET, text, &in4->sin_addr) == 1) {
		in4->sin_family = AF_INET;
		in4->sin_port = htons(port);
		return sizeof(*in4);
	}

	if (inet_pton(AF_INET6, text, &in6->sin6_addr) == 1) {
		in6->sin6_family = AF_INET6;
		in6->sin6_port = htons(port);
		return sizeof(*in6);
	}

	return 0;
}

static void test_source_probe(void)
{
	struct sockaddr_storage ss;
	const struct sockaddr *sa = (const struct sockaddr *)&ss;
	char why[128];
	char host[64] = "";
	bool v6 = tu_v6_loopback(why, sizeof(why));
	socklen_t len;

	CASE("probe: the route toward loopback answers with a loopback "
	     "address");
	len = sa_set(&ss, "127.0.0.1", 5060);
	CHECK(net_iface_source_host_probe(NULL, AF_INET, sa, len, host,
					  sizeof(host)));
	CHECK(!strcmp(host, "127.0.0.1"));

	CASE("probe: an empty device name is an unscoped probe, not a failure");
	host[0] = 0;
	CHECK(net_iface_source_host_probe("", AF_INET, sa, len, host,
					  sizeof(host)));
	CHECK(!strcmp(host, "127.0.0.1"));

	/* SO_BINDTODEVICE hard fails on a name no netdev carries, which is the
	 * same refusal a scoped udp bind takes */
	CASE("probe: a device the kernel does not know refuses the answer");
	CHECK(!net_iface_source_host_probe("uvoice-no-such-dev0", AF_INET, sa,
					   len, host, sizeof(host)));

	/*
	 * The answer is of the family the caller asked for, so a destination of
	 * the other family has none: an address of one family in the Via and
	 * the Contact of a line that serves the other reaches nobody.
	 */
	CASE("probe: a destination of another family than the one asked for");
	host[0] = 0;
	len = sa_set(&ss, "::1", 5060);
	CHECK(!net_iface_source_host_probe(NULL, AF_INET, sa, len, host,
					   sizeof(host)));
	len = sa_set(&ss, "127.0.0.1", 5060);
	CHECK(!net_iface_source_host_probe(NULL, AF_INET6, sa, len, host,
					   sizeof(host)));
	CHECK(!host[0]);

	CASE("probe: the route toward the v6 loopback answers in IPv6");
	if (!v6) {
		tu_skip(why);
	} else {
		host[0] = 0;
		len = sa_set(&ss, "::1", 5060);
		CHECK(net_iface_source_host_probe(NULL, AF_INET6, sa, len, host,
						  sizeof(host)));
		CHECK(!strcmp(host, "::1"));
	}

	CASE("probe: a destination with no address is refused");
	memset(&ss, 0, sizeof(ss));
	CHECK(!net_iface_source_host_probe(NULL, AF_INET, sa, sizeof(ss), host,
					   sizeof(host)));

	CASE("probe: and so is no destination at all");
	CHECK(!net_iface_source_host_probe(NULL, AF_INET, NULL, 0, host,
					   sizeof(host)));
}

/* ------------------------------------------------------------ the record */

static void test_status_table(void)
{
	uint8_t addr[16];

	tracker_reset();

	CASE("network dump: unknown network emits no table");
	CHECK(net_table_gone());

	CASE("status: an answer records the netdev and both addresses");
	status_inject("eth1", "192.0.2.10", "2001:db8:1::10", false);
	CHECK(net_key_is("l3_device", "eth1"));
	CHECK(net_key_is("address", "192.0.2.10"));
	CHECK(net_key_is("address6", "2001:db8:1::10"));

	CASE("status: the state carries the record and the known flags");
	CHECK(!strcmp(net_iface_state_get()->iface, "wan"));
	CHECK(net_iface_state_get()->addr4_known);
	CHECK(net_iface_state_get()->addr6_known);
	CHECK(!strcmp(net_iface_state_get()->addr6_iface, "wan"));

	CASE("address: the IPv4 address in network byte order");
	memset(addr, 0, sizeof(addr));
	CHECK_UINT(net_iface_address(4, addr), 0);
	CHECK(addr[0] == 192 && addr[1] == 0 && addr[2] == 2 && addr[3] == 10);

	CASE("address: the IPv6 address in network byte order");
	memset(addr, 0, sizeof(addr));
	CHECK_UINT(net_iface_address(6, addr), 0);
	CHECK(addr[0] == 0x20 && addr[1] == 0x01 && addr[15] == 0x10);

	CASE("address: another version and no buffer are invalid");
	CHECK(net_iface_address(5, addr) == -EINVAL);
	CHECK(net_iface_address(4, NULL) == -EINVAL);

	CASE("address: a family with no address answers none");
	tracker_reset();
	CHECK(net_iface_address(4, addr) == -ENOENT);
	CHECK(net_iface_address(6, addr) == -ENOENT);

	CASE("bind device: the base interface takes the tracked netdev the "
	     "kernel knows");
	if (access("/sys/class/net/lo", F_OK) != 0) {
		tu_skip("/sys/class/net is not mounted");
	} else {
		status_inject("lo", NULL, NULL, false);
		CHECK(bind_is(net_iface_bind_device("wan"), "lo"));
	}

	CASE("bind device: a netdev the kernel does not know scopes nothing");
	status_inject("uvoice-nodev1", NULL, NULL, false);
	CHECK(bind_is(net_iface_bind_device("wan"), NULL));
}

/*
 * One event, one report. An answer that moves the netdev and both addresses
 * reaches the subscriber once, with the old addresses beside the new state,
 * and an answer that moves nothing reaches it not at all.
 */
static void test_reports(void)
{
	tracker_reset();

	CASE("report: the first answer reports both addresses and the netdev "
	     "once");
	status_inject("eth1", "192.0.2.10", "2001:db8:1::10", false);
	CHECK_UINT(rec.changed, 1);
	CHECK(rec.ch.addr4);
	CHECK(rec.ch.addr6);
	CHECK(rec.ch.dev);
	CHECK(!rec.ch.force);
	CHECK(!strcmp(rec.ch.addr4_old, ""));
	CHECK(!strcmp(rec.st.addr4, "192.0.2.10"));

	CASE("report: the same answer again reports nothing");
	status_inject("eth1", "192.0.2.10", "2001:db8:1::10", false);
	CHECK_UINT(rec.changed, 1);

	CASE("report: a moved IPv4 address carries the old one");
	status_inject("eth1", "192.0.2.11", "2001:db8:1::10", false);
	CHECK_UINT(rec.changed, 2);
	CHECK(rec.ch.addr4);
	CHECK(!rec.ch.addr6);
	CHECK(!rec.ch.dev);
	CHECK(!strcmp(rec.ch.addr4_old, "192.0.2.10"));
	CHECK(!strcmp(rec.st.addr4, "192.0.2.11"));

	CASE("report: an ifdown drops the IPv6 address of the voice interface");
	event_inject("wan", "ifdown");
	CHECK_UINT(rec.changed, 3);
	CHECK(rec.ch.addr6);
	CHECK(!strcmp(rec.ch.addr6_old, "2001:db8:1::10"));
	CHECK(net_iface_state_get()->down);
	CHECK(!net_iface_state_get()->addr6_known);

	CASE("report: the same address after an ifdown forces a rebind");
	status_inject("eth1", "192.0.2.11", NULL, false);
	CHECK_UINT(rec.changed, 4);
	CHECK(rec.ch.addr4);
	CHECK(rec.ch.force);
	CHECK(!net_iface_state_get()->down);

	CASE("report: another interface of the netdev lends its IPv6 address");
	dump_inject("wan6", "eth1", "2001:db8:2::5");
	CHECK_UINT(rec.changed, 5);
	CHECK(rec.ch.addr6);
	CHECK(!strcmp(net_iface_state_get()->addr6_iface, "wan6"));
	CHECK(net_key_is("address6", "2001:db8:2::5"));

	CASE("report: a move of the IPv6 served gate carries the old gate");
	net_iface_ipv6_served_set(true);
	CHECK_UINT(rec.changed, 6);
	CHECK(rec.ch.served);
	CHECK(!rec.ch.ipv6_served_old);
	CHECK(rec.st.ipv6_served);

	CASE("report: an unmoved gate reports nothing");
	net_iface_ipv6_served_set(true);
	CHECK_UINT(rec.changed, 6);
	net_iface_ipv6_served_set(false);

	/* no line can signal in IPv6 while the gate is clear, so an interface
	 * that came back on IPv6 alone is not back in service */
	CASE("down flag: with the IPv6 gate clear an IPv6-only up answer keeps "
	     "it");
	event_inject("wan", "ifdown");
	status_inject("eth1", NULL, "2001:db8:1::20", true);
	CHECK(net_iface_state_get()->down);
	net_iface_ipv6_served_set(true);
	event_inject("wan", "ifdown");
	status_inject("eth1", NULL, "2001:db8:1::20", true);
	CHECK(!net_iface_state_get()->down);
	net_iface_ipv6_served_set(false);
}

/*
 * The recovery ladder. An interface that came back with no address arms the
 * one timer of the process. An attempt that heals nothing climbs the ladder.
 * The answer that carries an address disarms the timer, and a socket that the
 * subscriber still holds down keeps it armed.
 */
static void test_recovery(void)
{
	tracker_reset();
	status_inject("eth1", "192.0.2.10", NULL, false);

	CASE("recovery: an ifup answer with no address arms the first delay");
	event_inject("wan", "ifdown");
	status_inject("eth1", NULL, NULL, true);
	CHECK_UINT(net_iface_test_recover_ms(), NET_IFACE_RECOVER_MS);

	CASE("recovery: an attempt that heals nothing climbs the ladder");
	CHECK(net_iface_test_recover_fire());
	CHECK(net_iface_test_recover_ms() > NET_IFACE_RECOVER_MS);
	CHECK(rec.recover >= 1);

	CASE("recovery: the ladder stops at its cap");
	while (net_iface_test_recover_ms() &&
	       net_iface_test_recover_ms() < NET_IFACE_RECOVER_MAX_MS)
		CHECK(net_iface_test_recover_fire());
	CHECK_UINT(net_iface_test_recover_ms(), NET_IFACE_RECOVER_MAX_MS);
	CHECK(net_iface_test_recover_fire());
	CHECK_UINT(net_iface_test_recover_ms(), NET_IFACE_RECOVER_MAX_MS);

	CASE("recovery: the answer that carries the address disarms it");
	status_inject("eth1", "192.0.2.10", NULL, false);
	CHECK_UINT(net_iface_test_recover_ms(), 0);
	CHECK(!net_iface_test_recover_fire());

	CASE("recovery: a socket the subscriber holds down arms it");
	rec.down = 1;
	net_iface_recover_settle();
	CHECK_UINT(net_iface_test_recover_ms(), NET_IFACE_RECOVER_MS);

	CASE("recovery: and the settle that finds it up disarms it");
	rec.down = 0;
	net_iface_recover_settle();
	CHECK_UINT(net_iface_test_recover_ms(), 0);

	CASE("recovery: a new voice interface disarms the pending attempt");
	rec.down = 1;
	net_iface_recover_settle();
	CHECK(net_iface_test_recover_ms() != 0);
	net_iface_iface_set("wan2");
	CHECK_UINT(net_iface_test_recover_ms(), 0);
	rec.down = 0;
}

/*
 * The shutdown drain and the reload of another interface: both change the
 * record without telling the subscriber, because the protocol is leaving or
 * rebuilds every line anyway.
 */
static void test_quiesce_iface_set(void)
{
	tracker_reset();
	status_inject("eth1", "192.0.2.10", "2001:db8:1::10", false);
	rec_reset();

	CASE("iface_set: the same interface changes nothing");
	net_iface_iface_set("wan");
	CHECK(net_key_is("address", "192.0.2.10"));
	CHECK_UINT(rec.changed, 0);

	CASE("iface_set: another interface drops the record and reports "
	     "nothing");
	net_iface_iface_set("wan2");
	CHECK(net_table_gone());
	CHECK(!strcmp(net_iface_state_get()->iface, "wan2"));
	CHECK_UINT(rec.changed, 0);

	CASE("iface_set: an answer then reports the new interface");
	status_inject("eth2", "198.51.100.4", NULL, false);
	CHECK_UINT(rec.changed, 1);
	CHECK(net_key_is("address", "198.51.100.4"));

	CASE("quiesce: an answer after the quiesce changes nothing");
	rec_reset();
	net_iface_quiesce();
	status_inject("eth3", "203.0.113.7", NULL, false);
	dump_inject("wan6", "eth2", "2001:db8:3::7");
	CHECK(net_key_is("address", "198.51.100.4"));
	CHECK_UINT(rec.changed, 0);

	CASE("quiesce: and an event neither");
	event_inject("wan2", "ifdown");
	CHECK(!net_iface_state_get()->down);
	CHECK_UINT(rec.changed, 0);

	CASE("quiesce: nothing arms the recovery");
	rec.down = 1;
	net_iface_recover_settle();
	CHECK_UINT(net_iface_test_recover_ms(), 0);
	rec.down = 0;

	CASE("done: the IPv6 record and the gate go");
	tracker_reset();
	status_inject("eth1", "192.0.2.10", "2001:db8:1::10", false);
	net_iface_ipv6_served_set(true);
	net_iface_done();
	CHECK(!net_iface_state_get()->addr6_known);
	CHECK(!net_iface_state_get()->ipv6_served);
	CHECK(net_key_is("address", "192.0.2.10"));
}

int main(void)
{
	uloop_init();

	test_bind_device();
	test_netdev_present();
	test_iface_rebind();
	test_netifd_retry_status();
	test_source_probe();
	test_status_table();
	test_reports();
	test_recovery();
	test_quiesce_iface_set();

	uloop_done();

	return tu_summary("test_net_iface");
}
