// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * One tracker event, one report. Every entry point below (a status answer, a
 * dump answer, an interface event, a move of the IPv6 served gate) collects
 * what it moved into a struct net_iface_change, with the old addresses and
 * the old gate taken before the first write, and hands the subscriber that
 * report once at its end. One netifd answer can move the netdev, an address of
 * each family and the down flag at once, and the subscriber rebuilds each line
 * at most once per answer.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubus.h>

#include "net/iface.h"
#include "posix.h"
#include "str.h"
#include "trace.h"

static struct {
	struct ubus_context *ubus;
	struct ubus_event_handler netifd_ev;

	struct net_iface_state st;
	struct net_iface_sub sub;
	bool sub_set;

	/* the report of the event in progress, and whether one is open */
	struct net_iface_change ch;
	bool ch_open;

	/*
	 * The one recovery timer of the process and its ladder. The two flags
	 * are reasons of their own, one per netifd query: the query that ended
	 * on ubus instead of on an answer arms the timer, the attempt repeats
	 * that query and not the other one, and the settle behind it must not
	 * disarm what the query just recorded. recover_settled says the settle
	 * already ran inside the attempt, so one attempt runs the recovery pass
	 * once.
	 */
	struct uloop_timeout recover;
	unsigned int recover_fails;
	unsigned int recover_delay;
	bool recover_ubus;		/* the status query of the voice iface */
	bool recover_dump;		/* the dump query of every interface */
	bool recover_settled;

	/* net_iface_init() ran: the interface events of netifd count */
	bool inited;
	/* net_iface_quiesce() ran: the shutdown drain owns the sockets */
	bool quiesced;
} ni;

/* ----------------------------------------------------------- pure helpers */

bool net_iface_rebind_needed(bool addr_changed, bool iface_was_down,
			     bool dev_changed)
{
	return addr_changed || iface_was_down || dev_changed;
}

/*
 * Whether the kernel carries a netdev of this name, read from sysfs and not
 * from a netdev ioctl. SIOCGIFINDEX on an absent name makes the kernel ask
 * modprobe for "netdev-<name>" and then for "<name>", synchronously, and the
 * logical uci name is normally no netdev at all. Each such lookup blocks the
 * uloop thread on that modprobe, once per inbound request, because the call
 * view runs the bind rule on every one. A sysfs lookup loads nothing.
 *
 * The name rules are the kernel's dev_valid_name(), so no name this refuses
 * could have been a netdev: the two dot names would pass the lookup as
 * directories, and a colon names an address alias, which SO_BINDTODEVICE
 * never accepted either. A plain file in the class directory
 * (bonding_masters) is not a device, which is what the directory test is for.
 *
 * One name the ioctl knows and sysfs does not: an alternate interface name
 * (ip link property add altname) registers no class entry, so a line that
 * names its device by one reads it as absent and binds unscoped. A netlink
 * RTM_GETLINK lookup would answer it without the autoload; nothing here
 * configures such a name, so the limit is recorded and not met.
 */
bool net_iface_netdev_present(const char *name)
{
	char path[sizeof("/sys/class/net/") + IF_NAMESIZE];
	struct stat st;
	size_t len, i;

	if (!name || !name[0])
		return false;

	len = strlen(name);
	if (len >= IF_NAMESIZE || !strcmp(name, ".") || !strcmp(name, ".."))
		return false;

	for (i = 0; i < len; i++) {
		if (name[i] == '/' || name[i] == ':' ||
		    isspace((unsigned char)name[i]))
			return false;
	}

	snprintf(path, sizeof(path), "/sys/class/net/%s", name);

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/*
 * The netdev SO_BINDTODEVICE gets, or NULL for an unscoped bind.
 *
 * iface holds the uci logical interface name, which the kernel usually does
 * not know: a unit can run logical wan on netdev lan4, and SO_BINDTODEVICE
 * hard fails on a name no netdev carries, which would leave the socket
 * unscoped. netifd answers the mapping with l3_device, and that answer wins
 * whenever it applies. It applies to the base interface only, because that is
 * the one the tracker queries; a channel with its own local_interface has no
 * tracked device and keeps the logical name. Either name is used only while
 * the kernel knows it, so an absent or stale device falls back to an unscoped
 * bind instead of failing the bind.
 */
const char *net_iface_bind_device_pick(const char *iface,
				       const char *base_iface,
				       const char *l3_device, bool iface_known,
				       bool l3_known)
{
	if (!iface || !iface[0])
		return NULL;

	if (l3_known && l3_device && l3_device[0] && base_iface &&
	    !strcmp(iface, base_iface))
		return l3_device;

	if (iface_known)
		return iface;

	return NULL;
}

const char *net_iface_bind_device(const char *iface)
{
	return net_iface_bind_device_pick(iface, ni.st.iface, ni.st.l3_device,
					  iface && iface[0] &&
					  net_iface_netdev_present(iface),
					  ni.st.l3_device[0] &&
					  net_iface_netdev_present(ni.st.l3_device));
}

/*
 * The source address the kernel would send from toward sa, scoped to dev,
 * from a transient connected datagram socket that carries no traffic:
 * connect() on a datagram socket only fixes the route. A stream line asks it,
 * because it holds no datagram socket to ask and its own connection does not
 * exist until the first send, and so does a line that moves family, because
 * the socket it still holds answers about the family it leaves.
 *
 * The answer is an address of family, the family the caller signals in. A
 * destination of the other family has no answer, exactly as it has none from
 * a bound datagram socket: an address of one family in the Via and the Contact
 * of a line that serves the other reaches nobody.
 */
bool net_iface_source_host_probe(const char *dev, int family,
				 const struct sockaddr *sa, socklen_t salen,
				 char *out, size_t olen)
{
	struct sockaddr_storage local;
	socklen_t slen = sizeof(local);
	bool ok = false;
	int fd;

	if (!sa || !salen)
		return false;
	if (family != AF_INET && family != AF_INET6)
		return false;
	if (sa->sa_family != family)
		return false;

	fd = socket(family, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
	if (fd < 0)
		return false;

	memset(&local, 0, sizeof(local));

	/* the same scope the connections of this line carry, so the answer is
	 * the address they would send from and not the one another netdev has */
	if (dev && dev[0] &&
	    setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, dev, strlen(dev) + 1) < 0)
		goto out;

	/* connect() on a datagram socket picks the route and binds a source
	 * address; nothing is sent, and the socket is gone before the return */
	if (connect(fd, sa, salen) < 0)
		goto out;

	if (getsockname(fd, (struct sockaddr *)&local, &slen) < 0)
		goto out;

	ok = sa_text(&local, out, olen);

out:
	close(fd);

	return ok;
}

/*
 * Whether a failed netifd lookup asks for another attempt. A status of
 * UBUS_STATUS_NOT_FOUND does not: the object is absent and no attempt can
 * make it appear, so the attempt would repeat for as long as the daemon runs.
 * Two deployments hold that state for ever: a local_interface that names no
 * netifd interface, which uvoice tolerates, and a box whose ubusd carries no
 * netifd at all. Nothing is lost by staying quiet. netifd publishes
 * network.interface.<name> when the interface comes up, and its ifup event
 * runs the query then. The event handler is registered against the event name
 * on the ubusd event object and not against the interface object
 * (net_iface_ubus_attach), so the event arrives whether that object exists now
 * or not.
 *
 * Every other status is a fault of the path itself and asks for an attempt.
 */
bool net_iface_retry_needed(int status)
{
	return status && status != UBUS_STATUS_NOT_FOUND;
}

/* ------------------------------------------------------------ the report */

static void change_open(void)
{
	if (ni.ch_open)
		return;

	memset(&ni.ch, 0, sizeof(ni.ch));
	str_copy(ni.ch.addr4_old, sizeof(ni.ch.addr4_old), ni.st.addr4);
	str_copy(ni.ch.addr6_old, sizeof(ni.ch.addr6_old), ni.st.addr6);
	ni.ch.ipv6_served_old = ni.st.ipv6_served;
	ni.ch_open = true;
}

static void state_known_sync(void)
{
	ni.st.addr4_known = ni.st.addr4[0] != 0;
	ni.st.addr6_known = ni.st.addr6[0] != 0;
}

/* The report goes out once, after the record write, when anything moved. */
static void change_close(void)
{
	struct net_iface_change ch = ni.ch;

	if (!ni.ch_open)
		return;

	ni.ch_open = false;
	state_known_sync();

	if (!ch.addr4 && !ch.addr6 && !ch.dev && !ch.force && !ch.served)
		return;
	if (!ni.sub_set || !ni.sub.changed)
		return;

	ni.sub.changed(ni.sub.priv, &ni.st, &ch);
}

/* -------------------------------------------------------------- recovery */

static void recover_cb(struct uloop_timeout *t);
static void netifd_query(const char *iface);
static void netifd_dump_query(void);

/*
 * Arm the next attempt. why names the state that asks for it and reaches the
 * log once per delay: a settled daemon is silent, and a fault that stays logs
 * the ladder it climbs and nothing more.
 *
 * An arm that meets a pending attempt changes nothing. The attempt that waits
 * covers every reason that arrives before it runs, so the ladder grows once per
 * attempt and not once per reason.
 */
static void recover_arm(const char *why)
{
	unsigned int delay;

	if (ni.quiesced || ni.recover.pending)
		return;

	ni.recover_fails++;
	delay = retry_delay_ms(ni.recover_fails, NET_IFACE_RECOVER_MS,
			       NET_IFACE_RECOVER_MAX_MS);

	if (delay != ni.recover_delay)
		ULOG_INFO("voice recovery armed (%s), next attempt in %u ms\n",
			  why, delay);

	ni.recover_delay = delay;
	ni.recover.cb = recover_cb;
	uloop_timeout_set(&ni.recover, delay);
}

/* Recovery complete, or no longer this layer's business: the ladder starts
 * from the first delay whenever the next fault arms it. */
static void recover_disarm(void)
{
	uloop_timeout_cancel(&ni.recover);
	ni.recover_fails = 0;
	ni.recover_delay = 0;
	ni.recover_ubus = false;
	ni.recover_dump = false;
}

/*
 * A netifd query that ended on ubus and not on an answer. The tracker learned
 * nothing, so the attempt asks again. reason is the flag of that query, and
 * the attempt repeats the query whose flag stands: a status answer says
 * nothing about the dump, whose answer carries the IPv6 address of an
 * interface of its own (netifd_dump_cb).
 */
static void recover_query_failed(bool *reason)
{
	*reason = true;
	recover_arm("netifd did not answer");
}

/*
 * The end of a netifd lookup that gave no id. A status the retry predicate
 * refuses is proof of the opposite of a broken path: ubus answered, and the
 * object is absent. It clears the query reason, because an attempt whose own
 * lookup reports an absent object would otherwise arm for ever on the reason
 * that an earlier failure recorded.
 */
static void recover_lookup_failed(bool *reason, int status)
{
	if (net_iface_retry_needed(status)) {
		recover_query_failed(reason);

		return;
	}

	*reason = false;
}

/*
 * Repair what can be repaired through the subscriber, and then decide whether
 * another attempt is due.
 *
 * Three states ask for one: a socket or a listener of the subscriber that is
 * still down; a voice interface that went down and came back with no address
 * of either family (the two answers that carry one clear the flag); and a
 * netifd query that ubus refused, which is the path itself and never an absent
 * object (net_iface_retry_needed).
 *
 * Two states ask for nothing. An empty netdev name is permanent while
 * local_interface names no netifd interface, which uvoice tolerates, and it is
 * the state of every harness with a bare ubusd. An empty address says the
 * interface carries none, not that the transport is broken.
 */
void net_iface_recover_settle(void)
{
	if (ni.quiesced)
		return;

	/* the attempt reads this to run the pass once: the status answer it
	 * asks for arrives here first (recover_cb) */
	ni.recover_settled = true;

	if (ni.sub_set && ni.sub.recover)
		ni.sub.recover(ni.sub.priv);

	if (ni.sub_set && ni.sub.down_count && ni.sub.down_count(ni.sub.priv)) {
		recover_arm("a local socket or listener is down");
		return;
	}

	if (ni.st.down) {
		recover_arm("the voice interface named no address");
		return;
	}

	if (ni.recover_ubus || ni.recover_dump) {
		recover_arm("netifd did not answer");
		return;
	}

	recover_disarm();
}

/*
 * One attempt. It repeats the query that failed. The status query runs every
 * time, because the address of every line rides on it. The dump query runs
 * only while its own reason stands: it costs a second synchronous invoke, and
 * a box with no interface of its own for the IPv6 address never needs one.
 *
 * The status query is synchronous and its answer runs the whole tracker, the
 * settle at its end included, so the settle here runs only when no answer
 * arrived: one attempt, one recovery pass.
 */
static void recover_cb(struct uloop_timeout *t)
{
	bool dump = ni.recover_dump;

	ni.recover_settled = false;
	netifd_query(ni.st.iface);

	if (dump)
		netifd_dump_query();

	if (!ni.recover_settled)
		net_iface_recover_settle();
}

unsigned int net_iface_test_recover_ms(void)
{
	return ni.recover.pending ? ni.recover_delay : 0;
}

bool net_iface_test_recover_fire(void)
{
	if (!ni.recover.pending)
		return false;

	/* what the uloop dispatch does: clear pending, then run the cb */
	uloop_timeout_cancel(&ni.recover);
	ni.recover.cb(&ni.recover);

	return true;
}

/* -------------------------------------------------------------- the record */

/*
 * One netifd interface, whether it arrives as the answer of a status invoke or
 * as one entry of a dump. The two shapes are the same table: netifd builds both
 * from netifd_dump_status(), and the dump adds the interface name in front of
 * it. A status answer therefore carries neither "interface" nor "up", and the
 * dump reader is the only caller that reads them.
 */
enum {
	NETIF_ADDR_LIST,
	NETIF_ADDR6_LIST,
	NETIF_L3_DEV,
	NETIF_IFACE,
	NETIF_UP,
	__NETIF_MAX,
};

static const struct blobmsg_policy netif_pol[__NETIF_MAX] = {
	[NETIF_ADDR_LIST] = { "ipv4-address", BLOBMSG_TYPE_ARRAY },
	[NETIF_ADDR6_LIST] = { "ipv6-address", BLOBMSG_TYPE_ARRAY },
	[NETIF_L3_DEV] = { "l3_device", BLOBMSG_TYPE_STRING },
	[NETIF_IFACE] = { "interface", BLOBMSG_TYPE_STRING },
	[NETIF_UP] = { "up", BLOBMSG_TYPE_BOOL },
};

/*
 * One address of an ipv4-address or ipv6-address array. netifd writes the
 * address, its mask, an optional preferred lifetime and an optional valid one;
 * an absent preferred lifetime means the address never expires and a value of 0
 * means it is deprecated. There is no scope key, and a link-local address is
 * not listed at all.
 */
enum {
	NETIF_A_ADDR,
	NETIF_A_PREFERRED,
	__NETIF_A_MAX,
};

static const struct blobmsg_policy netif_a_pol[__NETIF_A_MAX] = {
	[NETIF_A_ADDR] = { "address", BLOBMSG_TYPE_STRING },
	[NETIF_A_PREFERRED] = { "preferred", BLOBMSG_TYPE_INT32 },
};

/* the dump of every interface netifd holds */
enum {
	NETIF_DUMP_LIST,
	__NETIF_DUMP_MAX,
};

static const struct blobmsg_policy netif_dump_pol[__NETIF_DUMP_MAX] = {
	[NETIF_DUMP_LIST] = { "interface", BLOBMSG_TYPE_ARRAY },
};

/*
 * Whether an address can be a source address of the voice netdev, and whether
 * it is a unique local one (RFC 4193 fc00::/7). Refused: a link-local address
 * (fe80::/10), a multicast address (ff00::/8), the unspecified address, the
 * loopback address and an IPv4-mapped address (RFC 4291 sections 2.5.6, 2.7,
 * 2.5.2, 2.5.3 and 2.5.5.2). A unique local address passes, because a lab and
 * a carrier deployment both run on one.
 *
 * The text is parsed, so anything that is no IPv6 literal is refused as well.
 */
static bool netif_addr6_global(const char *addr, bool *ula)
{
	struct in6_addr a;

	if (!addr || inet_pton(AF_INET6, addr, &a) != 1)
		return false;

	if (IN6_IS_ADDR_LINKLOCAL(&a) || IN6_IS_ADDR_MULTICAST(&a) ||
	    IN6_IS_ADDR_UNSPECIFIED(&a) || IN6_IS_ADDR_LOOPBACK(&a) ||
	    IN6_IS_ADDR_V4MAPPED(&a))
		return false;

	*ula = (a.s6_addr[0] & 0xfe) == 0xfc;

	return true;
}

/*
 * The address of one ipv6-address array the voice netdev can send from, or NULL
 * for a list that holds none. A deprecated address is not a source address
 * (RFC 6724), so an entry whose preferred lifetime is present and 0 is skipped.
 * A global unicast address wins over a unique local one, because a peer outside
 * the site can answer only the first; inside one class the first entry wins.
 *
 * The answer points into the message and lives as long as it does.
 */
static const char *netif_addr6_take(struct blob_attr *list)
{
	const char *best = NULL;
	struct blob_attr *cur;
	bool best_ula = false;
	size_t rem;		/* the walk compares it against sizes */

	if (!list)
		return NULL;

	blobmsg_for_each_attr(cur, list, rem) {
		struct blob_attr *a[__NETIF_A_MAX];
		const char *addr;
		bool ula = false;

		blobmsg_parse(netif_a_pol, __NETIF_A_MAX, a, blobmsg_data(cur),
			      blobmsg_data_len(cur));
		if (!a[NETIF_A_ADDR])
			continue;
		if (a[NETIF_A_PREFERRED] &&
		    !blobmsg_get_u32(a[NETIF_A_PREFERRED]))
			continue;

		addr = blobmsg_get_string(a[NETIF_A_ADDR]);
		if (!netif_addr6_global(addr, &ula))
			continue;
		if (best && (ula || !best_ula))
			continue;

		best = addr;
		best_ula = ula;

		/* no later entry can beat a global unicast address */
		if (!ula)
			break;
	}

	return best;
}

/*
 * The one place the IPv6 address of the voice netdev arrives and leaves. iface
 * is the netifd interface that reports it and an empty address gives the record
 * up.
 *
 * The first interface with an address holds the record until it reports none:
 * several interfaces of one netdev can carry a global address, and a second one
 * must not take the Via and the Contact of a running line somewhere else. Only
 * the interface that holds the record can clear it.
 */
static void netifd_addr6_set(const char *iface, const char *addr6)
{
	const char *val = addr6 ? addr6 : "";

	if (!iface || !iface[0])
		return;

	if (ni.st.addr6_iface[0] && strcmp(iface, ni.st.addr6_iface)) {
		if (val[0])
			TRACE(TRACE_SIP,
			      "interface %s address6 %s ignored, %s holds %s",
			      iface, val, ni.st.addr6_iface, ni.st.addr6);
		return;
	}

	if (!strcmp(val, ni.st.addr6))
		return;

	ULOG_INFO("voice interface %s address6 %s -> %s\n", iface,
		  ni.st.addr6[0] ? ni.st.addr6 : "(none)",
		  val[0] ? val : "(none)");

	if (val[0]) {
		str_copy(ni.st.addr6, sizeof(ni.st.addr6), val);
		str_copy(ni.st.addr6_iface, sizeof(ni.st.addr6_iface), iface);
	} else {
		ni.st.addr6[0] = 0;
		ni.st.addr6_iface[0] = 0;
	}

	/* the address every IPv6 line carries moved, and with it the family a
	 * line prefers */
	ni.ch.addr6 = true;
}

/*
 * The netdev of the voice interface, taken from every answer, including one
 * that carries no address yet, because the bind needs it as soon as a line is
 * built. An answer without the field leaves the last one standing; the bind
 * checks the name against the kernel, so a stale one costs a scope, not a
 * socket. True when the netdev moved.
 */
static bool netif_l3_dev_take(struct blob_attr *const *tb)
{
	const char *dev;

	if (!tb[NETIF_L3_DEV])
		return false;

	dev = blobmsg_get_string(tb[NETIF_L3_DEV]);
	if (!dev || !*dev || !strcmp(dev, ni.st.l3_device))
		return false;

	ULOG_INFO("voice interface %s device %s -> %s\n", ni.st.iface,
		  ni.st.l3_device[0] ? ni.st.l3_device : "(none)", dev);
	str_copy(ni.st.l3_device, sizeof(ni.st.l3_device), dev);
	ni.ch.dev = true;

	return true;
}

/*
 * The IPv4 half of one status answer: the address every Via and Contact of an
 * IPv4 line carries. A move of it, an address that came back after an ifdown
 * and a netdev move all ask the subscriber for a rebind (ch.addr4).
 */
static void netif_addr4_take(struct blob_attr *const *tb, bool dev_changed)
{
	struct blob_attr *cur;
	const char *addr = NULL;
	bool changed;
	size_t rem;		/* the walk compares it against sizes */

	if (!tb[NETIF_ADDR_LIST])
		return;

	blobmsg_for_each_attr(cur, tb[NETIF_ADDR_LIST], rem) {
		struct blob_attr *a[__NETIF_A_MAX];

		blobmsg_parse(netif_a_pol, __NETIF_A_MAX, a, blobmsg_data(cur),
			      blobmsg_data_len(cur));
		if (a[NETIF_A_ADDR]) {
			addr = blobmsg_get_string(a[NETIF_A_ADDR]);
			break;
		}
	}

	if (!addr || !*addr)
		return;

	changed = strcmp(addr, ni.st.addr4) != 0;

	/*
	 * An ifdown then ifup on the same address still needs a rebind, because
	 * the old socket went stale while the interface was down. Without the
	 * down flag the unchanged address would be suppressed and nothing
	 * would recover until the next refresh. A netdev move needs one for the
	 * same reason, and it can arrive on an unchanged address.
	 */
	if (!net_iface_rebind_needed(changed, ni.st.down, dev_changed))
		return;

	/* an ifdown that came back on the address it had: the lines hold that
	 * address already and still have to register through the fresh socket */
	ni.ch.force = ni.st.down;
	ni.st.down = false;

	if (changed)
		ULOG_INFO("voice interface %s address %s -> %s\n", ni.st.iface,
			  ni.st.addr4[0] ? ni.st.addr4 : "(none)", addr);
	else
		ULOG_INFO("voice interface %s back up on %s\n", ni.st.iface,
			  addr);
	str_copy(ni.st.addr4, sizeof(ni.st.addr4), addr);

	ni.ch.addr4 = true;
}

/*
 * The IPv6 half of the down flag. An interface that came back with IPv6
 * service alone is in service again, and the IPv4 half never runs for such an
 * answer, so without this the flag would stand for ever and the recovery would
 * never settle.
 *
 * All three conditions come from the answer of the voice interface itself: it
 * reports up, it carries an address of its own, and the served gate lets a
 * line signal in IPv6 at all. The address record is not enough: a second
 * interface of the same netdev can hold it (netifd_addr6_set) and stay up
 * while the voice interface is down, and an IPv4 line that comes back on its
 * old address would then never rebind.
 */
static void netif_iface_up_take(struct blob_attr *const *tb)
{
	if (!ni.st.down || !tb[NETIF_UP] || !blobmsg_get_u8(tb[NETIF_UP]))
		return;

	if (!ni.st.ipv6_served)
		return;

	if (!netif_addr6_take(tb[NETIF_ADDR6_LIST]))
		return;

	ULOG_INFO("voice interface %s back up on IPv6 alone\n", ni.st.iface);
	ni.st.down = false;
}

static void netifd_status_cb(struct ubus_request *req, int type,
			     struct blob_attr *msg)
{
	struct blob_attr *tb[__NETIF_MAX];
	bool dev_changed;

	if (!msg || ni.quiesced)
		return;

	/* an answer in hand is the proof that the status query works; the dump
	 * query has an answer and a reason of its own */
	ni.recover_ubus = false;

	blobmsg_parse(netif_pol, __NETIF_MAX, tb, blob_data(msg),
		      blob_len(msg));

	change_open();
	dev_changed = netif_l3_dev_take(tb);

	/*
	 * A netdev move takes the address of the old netdev with it, whichever
	 * interface reported it. The answer in hand describes the new netdev,
	 * so the record goes before its own address is read and not after.
	 */
	if (dev_changed)
		netifd_addr6_set(ni.st.addr6_iface, "");

	/* Before the IPv4 half, which returns on an answer that carries no IPv4
	 * address: a voice interface that runs IPv6 alone still reports one. */
	netifd_addr6_set(ni.st.iface, netif_addr6_take(tb[NETIF_ADDR6_LIST]));

	netif_addr4_take(tb, dev_changed);

	/* after the IPv4 half, which clears the down flag for an answer that
	 * carries an IPv4 address */
	netif_iface_up_take(tb);

	/* one answer, one report */
	change_close();

	/* what this answer healed, what it left broken, and what it says about
	 * an interface that went down: the whole decision is there */
	net_iface_recover_settle();

	/* Another interface of the new netdev may hold its address, and netifd
	 * reports the two moves in whichever order it likes. */
	if (dev_changed)
		netifd_dump_query();
}

void net_iface_test_status_inject(struct blob_attr *msg)
{
	netifd_status_cb(NULL, 0, msg);
}

/*
 * The IPv6 address of the voice netdev, read off every interface netifd holds.
 * The voice interface answers through netifd_status_cb() and is skipped here,
 * and every other interface qualifies by its netdev alone: the v6 side of a WAN
 * is an interface of its own on the same netdev, and no uci option names it.
 *
 * An interface that is not up carries its addresses under an "inactive" table
 * and none at this level, so the "up" test is what keeps a released address out
 * of the record.
 *
 * The whole answer is walked, because it is the state of the moment and the
 * interface that holds the record is part of it. That interface's own entry
 * decides its address first: an entry that carries none, and an owner that is
 * not in the answer at all, give the record up. Only then does the first other
 * interface with an address take it. A reader that stopped at the first address
 * would keep a stale record and would hide the owner's own move behind any
 * entry ahead of it.
 */
static void netifd_dump_cb(struct ubus_request *req, int type,
			   struct blob_attr *msg)
{
	struct blob_attr *tb[__NETIF_DUMP_MAX];
	const char *owner_name = NULL;
	const char *owner_addr = NULL;
	const char *first_name = NULL;
	const char *first_addr = NULL;
	struct blob_attr *cur;
	bool owner_other;
	size_t rem;		/* the walk compares it against sizes */

	if (!msg || ni.quiesced)
		return;

	/* the answer of the dump query, which retires its own reason alone: a
	 * status answer says nothing about this one */
	ni.recover_dump = false;

	blobmsg_parse(netif_dump_pol, __NETIF_DUMP_MAX, tb, blob_data(msg),
		      blob_len(msg));
	if (!tb[NETIF_DUMP_LIST])
		return;

	/* The voice interface holds its own record through its status answer
	 * and its dump entry is skipped, so this answer says nothing about it
	 * and must not take its address away. */
	owner_other = ni.st.addr6_iface[0] &&
		      strcmp(ni.st.addr6_iface, ni.st.iface);

	blobmsg_for_each_attr(cur, tb[NETIF_DUMP_LIST], rem) {
		struct blob_attr *e[__NETIF_MAX];
		const char *name;
		const char *addr6;

		blobmsg_parse(netif_pol, __NETIF_MAX, e, blobmsg_data(cur),
			      blobmsg_data_len(cur));
		if (!e[NETIF_IFACE] || !e[NETIF_UP] ||
		    !blobmsg_get_u8(e[NETIF_UP]))
			continue;
		if (!e[NETIF_L3_DEV] ||
		    strcmp(blobmsg_get_string(e[NETIF_L3_DEV]),
			   ni.st.l3_device))
			continue;

		name = blobmsg_get_string(e[NETIF_IFACE]);
		if (!strcmp(name, ni.st.iface))
			continue;

		addr6 = netif_addr6_take(e[NETIF_ADDR6_LIST]);

		if (owner_other && !strcmp(name, ni.st.addr6_iface)) {
			owner_name = name;
			owner_addr = addr6;
			continue;
		}

		if (addr6 && !first_addr) {
			first_name = name;
			first_addr = addr6;
		}
	}

	change_open();

	/* The name comes from the message, not from the record it is about to
	 * rewrite: the two are the same text and one must not be copied over
	 * the other. */
	if (owner_other && owner_name)
		netifd_addr6_set(owner_name, owner_addr ? owner_addr : "");
	else if (owner_other)
		netifd_addr6_set(ni.st.addr6_iface, "");

	if (!ni.st.addr6[0] && first_addr)
		netifd_addr6_set(first_name, first_addr);

	/* one answer, one report: the owner giving the record up and the next
	 * interface taking it are one event */
	change_close();
}

void net_iface_test_dump_inject(struct blob_attr *msg)
{
	netifd_dump_cb(NULL, 0, msg);
}

static void netifd_query(const char *iface)
{
	char obj[64];
	uint32_t id;
	int status;

	if (!ni.ubus || !iface || !*iface)
		return;

	snprintf(obj, sizeof(obj), "network.interface.%s", iface);
	status = ubus_lookup_id(ni.ubus, obj, &id);
	if (status) {
		recover_lookup_failed(&ni.recover_ubus, status);

		return;
	}

	if (ubus_invoke(ni.ubus, id, "status", NULL, netifd_status_cb, NULL,
			2000))
		recover_query_failed(&ni.recover_ubus);
}

/* Ask every interface at once, which is the only way to find the one that
 * carries the IPv6 address of the voice netdev (netifd_dump_cb). */
static void netifd_dump_query(void)
{
	uint32_t id;
	int status;

	/* The reader takes an entry by the voice netdev, so a query made before
	 * that netdev is known can take nothing. The status query ahead of this
	 * one is synchronous, so the netdev is there by the time it runs. */
	if (!ni.ubus || !ni.st.l3_device[0])
		return;

	status = ubus_lookup_id(ni.ubus, "network.interface", &id);
	if (status) {
		recover_lookup_failed(&ni.recover_dump, status);

		return;
	}

	if (ubus_invoke(ni.ubus, id, "dump", NULL, netifd_dump_cb, NULL, 2000))
		recover_query_failed(&ni.recover_dump);
}

enum {
	NETEV_IFACE,
	NETEV_ACTION,
	__NETEV_MAX,
};

static const struct blobmsg_policy netev_pol[__NETEV_MAX] = {
	[NETEV_IFACE] = { "interface", BLOBMSG_TYPE_STRING },
	[NETEV_ACTION] = { "action", BLOBMSG_TYPE_STRING },
};

/*
 * An event of an interface that is not the voice one. Such an interface can
 * still hold the IPv6 address of the voice netdev, so its events move that
 * address and nothing else: the IPv4 address, the netdev and the sockets of
 * every line belong to the voice interface alone.
 */
static void netifd_event_other(const char *iface, const char *action)
{
	if (!strcmp(action, "ifdown")) {
		change_open();
		netifd_addr6_set(iface, "");
		change_close();
	} else if (strcmp(action, "ifup") && strcmp(action, "update")) {
		return;
	}

	/* after a down too: another interface of the netdev may carry one */
	netifd_dump_query();
}

static void netifd_event_cb(struct ubus_context *ctx,
			    struct ubus_event_handler *ev, const char *type,
			    struct blob_attr *msg)
{
	struct blob_attr *tb[__NETEV_MAX];
	const char *iface;
	const char *action;

	if (!msg || !ni.st.iface[0] || !ni.inited || ni.quiesced)
		return;

	blobmsg_parse(netev_pol, __NETEV_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[NETEV_IFACE] || !tb[NETEV_ACTION])
		return;

	iface = blobmsg_get_string(tb[NETEV_IFACE]);
	action = blobmsg_get_string(tb[NETEV_ACTION]);

	if (strcmp(iface, ni.st.iface)) {
		netifd_event_other(iface, action);
		return;
	}

	if (!strcmp(action, "ifup") || !strcmp(action, "update")) {
		netifd_query(iface);
	} else if (!strcmp(action, "ifdown")) {
		/* Remember the down so a later ifup on the same address still
		 * forces a rebind. */
		ni.st.down = true;
		change_open();
		netifd_addr6_set(iface, "");
		change_close();

		/* the interface that holds the netdev's IPv6 address can stay
		 * up while this one goes down, so every ifdown asks again */
		netifd_dump_query();
		TRACE(TRACE_SIP, "voice interface %s down", iface);
	}
}

void net_iface_test_event_inject(struct blob_attr *msg)
{
	netifd_event_cb(NULL, NULL, "network.interface", msg);
}

/* ------------------------------------------------------------- lifecycle */

void net_iface_init(const struct cfg *cfg)
{
	str_copy(ni.st.iface, sizeof(ni.st.iface),
		 cfg ? cfg->local_interface : "");
	ni.inited = true;
	ni.quiesced = false;

	/*
	 * Ask netifd before the first bind. The invoke is synchronous, so the
	 * address and the l3 device are known when the protocol opens its
	 * sockets and can scope them; without it the first sockets of a boot
	 * are always unscoped, and nothing re-opens them. The query lives here
	 * and not in net_iface_ubus_attach() alone because the interface name
	 * arrives with the config. It is a no-op until ubus is connected.
	 */
	netifd_query(ni.st.iface);

	/* the interface that holds the IPv6 address of that netdev is another
	 * one whenever the WAN runs a v6 side of its own (netifd_dump_cb) */
	netifd_dump_query();
}

void net_iface_ubus_attach(struct ubus_context *ctx)
{
	ni.ubus = ctx;
	ni.netifd_ev.cb = netifd_event_cb;

	if (ubus_register_event_handler(ctx, &ni.netifd_ev, "network.interface"))
		ULOG_WARN("could not subscribe to network.interface events\n");

	/* Prime the current addresses so the first registration uses them. */
	netifd_query(ni.st.iface);
	netifd_dump_query();
}

void net_iface_subscribe(const struct net_iface_sub *sub)
{
	ni.sub_set = sub != NULL;

	if (sub)
		ni.sub = *sub;
	else
		memset(&ni.sub, 0, sizeof(ni.sub));
}

const struct net_iface_state *net_iface_state_get(void)
{
	state_known_sync();

	return &ni.st;
}

void net_iface_ipv6_served_set(bool served)
{
	if (ni.st.ipv6_served == served)
		return;

	change_open();
	ni.st.ipv6_served = served;
	ni.ch.served = true;
	change_close();
}

void net_iface_quiesce(void)
{
	ni.quiesced = true;
	recover_disarm();
}

/*
 * A reload that names another voice interface. The old address, netdev and
 * down flag belong to the old interface, and so does the state that armed the
 * recovery. The IPv6 record goes with the netdev. No report goes out for these
 * drops: a moved local_interface rebuilds every line of the protocol
 * (proto/sip/account.h acct_base_restart). The two queries are synchronous,
 * so their answers report the new interface as any other answer does.
 */
void net_iface_iface_set(const char *iface)
{
	const char *name = iface ? iface : "";

	if (!strcmp(name, ni.st.iface))
		return;

	str_copy(ni.st.iface, sizeof(ni.st.iface), name);
	ni.st.addr4[0] = 0;
	ni.st.l3_device[0] = 0;
	ni.st.down = false;
	ni.st.addr6[0] = 0;
	ni.st.addr6_iface[0] = 0;
	state_known_sync();
	recover_disarm();

	netifd_query(ni.st.iface);
	netifd_dump_query();
}

/*
 * What the voice interface resolved to, for the status page: the IPv4 address
 * every Via and Contact carries, the global IPv6 address of the same netdev,
 * and the netdev the sockets are scoped to. All three are empty until netifd
 * answers, and an empty table would say nothing, so the whole table is omitted
 * then. A key is present only while its own value is known.
 */
void net_iface_status_dump(struct blob_buf *b)
{
	void *tbl;

	if (!ni.st.addr4[0] && !ni.st.addr6[0] && !ni.st.l3_device[0])
		return;

	tbl = blobmsg_open_table(b, "network");
	if (ni.st.addr4[0])
		blobmsg_add_string(b, "address", ni.st.addr4);
	if (ni.st.addr6[0])
		blobmsg_add_string(b, "address6", ni.st.addr6);
	if (ni.st.l3_device[0])
		blobmsg_add_string(b, "l3_device", ni.st.l3_device);
	blobmsg_close_table(b, tbl);
}

/*
 * The call trace asks for it when a datagram socket bound to the wildcard
 * reports no local address of its own, so the signaling entry it synthesizes
 * carries the address the message left from.
 */
int net_iface_address(uint8_t ipver, uint8_t addr[16])
{
	const char *text;
	int family;

	if (!addr)
		return -EINVAL;

	if (ipver == 4) {
		family = AF_INET;
		text = ni.st.addr4;
	} else if (ipver == 6) {
		family = AF_INET6;
		text = ni.st.addr6;
	} else {
		return -EINVAL;
	}

	if (!text[0] || inet_pton(family, text, addr) != 1)
		return -ENOENT;

	return 0;
}

void net_iface_done(void)
{
	recover_disarm();
	ni.st.addr6[0] = 0;
	ni.st.addr6_iface[0] = 0;
	ni.st.ipv6_served = false;
	state_known_sync();
}
