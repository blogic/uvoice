// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The MGCP protocol behind struct proto_ops (proto/proto.h): the voip.mgcp
 * section, the one UDP socket, the addresses of the call agent, the
 * transaction layer (mgcp/txn.h) and the endpoints (mgcp/ep.h) on it, the
 * status dumps and the wire hooks of the call trace.
 *
 * Endpoint names. {host} of endpoint_pattern is the domain option, or the
 * local IPv4 address when the option is empty: the address of the voice
 * interface, else the source address toward the call agent. Without a
 * domain the endpoints come up when that address is known, and a new
 * address restarts the unit, because every endpoint name changes.
 *
 * Socket. One UDP socket on local_port at the IPv4 wildcard address, bound
 * to the device of the voice interface. A socket that cannot bind holds the
 * endpoints in the disconnected procedure until the recovery of the tracker
 * opens it. A bind that fails when the endpoints come up settles the
 * recovery, so the tracker arms it with no interface event.
 *
 * Restart. Every option of the section restarts the unit on a reload: the
 * endpoints leave service on the wire, and the unit starts again with the
 * new section.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>

#include "posix.h"
#include "proto/mgcp/ep.h"
#include "proto/mgcp/proto.h"
#include "proto/proto.h"
#include "proto/proto_tap.h"
#include "proto/proto_trace.h"
#include "proto/shared/dst.h"
#include "proto/shared/resolve.h"
#include "proto/shared/sdp.h"
#include "str.h"

#define MP_TRACE(fmt, ...) \
	PROTO_TRACE(mp.host, TRACE_MGCP, "mgcp " fmt, ##__VA_ARGS__)

/* The domains the notified entities name at one time: the configured call
 * agent and the ones its redirects name. */
#define MP_NAME_MAX		4
#define MP_ADDR_MAX		8

/* RFC 3435 4.3 p. 125: a name resolved this recently is not asked again. */
#define MP_RESOLVE_RECENT_MS	5000
/* A failed name is asked again on a ladder from the floor above, so a name
 * that stays dead costs one query a minute. */
#define MP_RESOLVE_RETRY_MAX_MS	60000

/* The RD: of the graceful RSIP: the drain bound of the daemon stop
 * (proto/proto.c PROTO_STOP_TIMEOUT_MS). */
#define MP_STOP_DELAY_S		3

/* One domain of a notified entity and the IPv4 addresses it resolved to. cur
 * is the address the commands go to; a Max1 or Max2 moves it on. resolved_ms
 * is the end of the last query, an answer or a refusal. fails counts the
 * failed queries in a row, and retry_ms is the earliest next query after
 * them. */
struct mp_name {
	char domain[CFG_MGCP_DOMAIN_MAX];
	struct sip_txn_dst addr[MP_ADDR_MAX];
	unsigned int count;
	unsigned int cur;
	uint32_t query;
	uint64_t resolved_ms;
	uint64_t expires_ms;
	unsigned int fails;
	uint64_t retry_ms;
	bool used;
};

struct mp_stat {
	uint64_t rx;
	uint64_t rx_bytes;
	uint64_t tx;
	uint64_t tx_bytes;
	uint64_t tx_err;
	uint64_t rx_err;
	uint64_t truncated;
};

static struct {
	const struct proto_host *host;
	struct cfg_mgcp cfg;
	enum cfg_mgcp_dialect dialect;
	unsigned int lines;
	uint32_t enabled;		/* the lines whose channel is enabled */
	char iface[CFG_STR_MAX];

	struct mgcp_txn_env env;
	struct mgcp_txn_mgr mgr;
	bool mgr_up;
	/* RFC 3435 3.2.1.2 p. 81: an id does not come again within three
	 * minutes, so a new manager goes on from the last one, and the first
	 * one of the daemon starts at a random id. */
	uint32_t txid_first;
	struct mgcp_gw gw;
	bool gw_up;
	struct sip_dns dns;
	bool dns_up;
	struct mp_name name[MP_NAME_MAX];
	struct proto_timer ca_retry;

	struct uloop_fd ufd;
	bool sock_up;
	bool sock_failed;		/* bind failed, the recovery retries */
	struct sockaddr_storage local;
	char addr4[NET_IFACE_ADDR_MAX];

	uint32_t svc_off;		/* lines out of service by the core */
	uint32_t land;			/* staged config that waits for rest */
	bool started;
	bool stopping;
	bool stop_wait;			/* shutdown_pending() answered true */
	proto_tap_fn tap;
	uint32_t disc_attempts;
	struct mp_stat stat;
	char rx[MGCP_TXN_WIRE_DEFAULT];
} mp = {
	.ufd = { .fd = -1 },
};

static uint64_t mp_now_ms(void)
{
	return mp.env.base.now_ms(&mp.env.base);
}

/* ------------------------------------------------------- the config view */

static void cfg_take(const struct cfg *cfg)
{
	unsigned int i;

	mp.cfg = cfg->mgcp;
	mp.dialect = cfg_mgcp_dialect_of(cfg);
	mp.lines = cfg->channels < MGCP_EP_MAX ? cfg->channels : MGCP_EP_MAX;
	str_copy(mp.iface, sizeof(mp.iface), cfg->local_interface);

	mp.enabled = 0;
	for (i = 0; i < mp.lines; i++) {
		if (cfg->channel[i].enabled)
			mp.enabled |= 1u << i;
	}
}

static const char *bind_dev(void)
{
	const char *dev = mp.host->net_iface_bind_device(mp.host->ctx,
							 mp.iface);

	return dev ? dev : "";
}

/* addr_pton() takes an empty text as the wildcard, which is no literal. */
static bool ip_literal(int family, const char *text)
{
	struct sockaddr_storage sa;

	return text[0] && addr_pton(family, text, &sa);
}

static bool entity_split(const char *entity, char *domain, size_t dlen,
			 uint16_t *port);
static void entity_text(char *out, size_t olen);
static struct mp_name *name_find(const char *domain);

/*
 * The IPv4 address of this side: the tracked address of the voice interface,
 * else the source address the kernel picks toward the configured call agent,
 * as the SIP unit does on a box where netifd never answers
 * (proto/sip/account.c account_local_host()).
 */
static bool local_addr_get(char *out, size_t olen)
{
	char entity[MGCP_ENTITY_MAX], domain[CFG_MGCP_DOMAIN_MAX];
	struct sip_txn_dst d;
	struct mp_name *n;
	uint16_t port;

	if (mp.addr4[0]) {
		str_copy(out, olen, mp.addr4);
		return true;
	}

	entity_text(entity, sizeof(entity));
	if (!entity[0] || !entity_split(entity, domain, sizeof(domain), &port))
		return false;
	if (!sip_dst_set(&d, domain, port)) {
		n = name_find(domain);
		if (!n || !n->count)
			return false;
		d = n->addr[0];
	}
	if (d.sa.ss_family != AF_INET)
		return false;

	return mp.host->net_iface_source_host_probe(mp.host->ctx, bind_dev(),
						    AF_INET,
						    (struct sockaddr *)&d.sa,
						    d.sa_len, out, olen);
}

/* {host} of the endpoint names: the domain option, else the local address;
 * bracketed_ip writes an address as [a.b.c.d]. */
static bool host_text(char *out, size_t olen)
{
	char addr[NET_IFACE_ADDR_MAX];
	const char *h = mp.cfg.domain;

	if (!h[0]) {
		if (!local_addr_get(addr, sizeof(addr)))
			return false;
		h = addr;
	}
	if (mp.cfg.bracketed_ip && ip_literal(AF_INET, h))
		return snprintf(out, olen, "[%s]", h) < (int)olen;

	str_copy(out, olen, h);

	return true;
}

/* The notified entity at start: the call agent with its port (RFC 3435
 * 3.2.1.3); an IPv6 literal takes its brackets. */
static void entity_text(char *out, size_t olen)
{
	const char *ca = mp.cfg.call_agent;

	if (!ca[0]) {
		out[0] = '\0';
		return;
	}

	if (ip_literal(AF_INET6, ca))
		snprintf(out, olen, "[%s]:%u", ca, mp.cfg.call_agent_port);
	else
		snprintf(out, olen, "%s:%u", ca, mp.cfg.call_agent_port);
}

static void txn_cfg_fill(struct mgcp_txn_cfg *tc)
{
	mgcp_txn_cfg_default(tc);
	tc->max1 = mp.cfg.max1;
	tc->max2 = mp.cfg.max2;
	tc->rto_init_ms = mp.cfg.rto_init_ms;
	tc->rto_max_ms = mp.cfg.rto_max_ms;
	tc->t_max_ms = mp.cfg.t_max_s * 1000u;
	tc->t_hist_ms = mp.cfg.t_hist_s * 1000u;
	tc->txid_first = mp.txid_first;
	if (mp.dialect != CFG_MGCP_NCS)
		return;

	/* NCS 7.4.2, 7.4.3.6; 8.8 limits the provisional response */
	tc->flags = MGCP_TXN_CFG_MAX2_QUIET | MGCP_TXN_CFG_TD_RANDOM;
	tc->prov_verbs = (1u << MGCP_VERB_CRCX) | (1u << MGCP_VERB_MDCX);
}

static bool gw_cfg_fill(struct mgcp_ep_cfg *ec)
{
	memset(ec, 0, sizeof(*ec));
	if (!host_text(ec->host, sizeof(ec->host)))
		return false;

	ec->dialect = mp.dialect;
	ec->lines = mp.lines;
	str_copy(ec->pattern, sizeof(ec->pattern), mp.cfg.endpoint_pattern);
	ec->line_base = mp.cfg.line_base;
	entity_text(ec->entity, sizeof(ec->entity));
	str_copy(ec->persistent, sizeof(ec->persistent),
		 mp.cfg.persistent_events);
	ec->quarantine_loop =
		mp.cfg.quarantine_default == CFG_MGCP_QUARANTINE_LOOP;
	ec->ptime_omit = !mp.cfg.send_ptime;
	ec->restart_delay_max_ms = mp.cfg.restart_delay_max_s * 1000u;
	if (!local_addr_get(ec->media_addr, sizeof(ec->media_addr)))
		ec->media_addr[0] = '\0';
	str_copy(ec->bind_dev, sizeof(ec->bind_dev), bind_dev());

	return true;
}

/* ---------------------------------------------------- the call agent names */

static struct mp_name *name_find(const char *domain)
{
	unsigned int i;

	for (i = 0; i < MP_NAME_MAX; i++) {
		if (mp.name[i].used && !strcasecmp(mp.name[i].domain, domain))
			return &mp.name[i];
	}

	return NULL;
}

/* A free entry, else the one resolved longest ago with no query out. */
static struct mp_name *name_slot(void)
{
	struct mp_name *pick = NULL;
	unsigned int i;

	for (i = 0; i < MP_NAME_MAX; i++) {
		struct mp_name *n = &mp.name[i];

		if (!n->used)
			return n;
		if (n->query != SIP_HANDLE_NONE)
			continue;
		if (!pick || n->resolved_ms < pick->resolved_ms)
			pick = n;
	}

	return pick;
}

/* The domain of the configured call agent, when that is a name. */
static bool ca_domain_get(char *domain, size_t dlen)
{
	char entity[MGCP_ENTITY_MAX];
	uint16_t port;

	entity_text(entity, sizeof(entity));

	return entity[0] && entity_split(entity, domain, dlen, &port) &&
	       !ip_literal(AF_INET, domain) && !ip_literal(AF_INET6, domain);
}

static bool name_is_ca(const struct mp_name *n)
{
	char domain[CFG_MGCP_DOMAIN_MAX];

	return ca_domain_get(domain, sizeof(domain)) &&
	       !strcasecmp(domain, n->domain);
}

/*
 * A failed query sets the earliest next one of its name, on a ladder from
 * the floor. The endpoints need the call agent name: its address carries the
 * restart, and without a domain option it gives the local address. A query
 * of it that leaves no address therefore arms the next one.
 */
static void name_failed(struct mp_name *n)
{
	unsigned int delay;

	n->fails++;
	delay = retry_delay_ms(n->fails, MP_RESOLVE_RECENT_MS,
			       MP_RESOLVE_RETRY_MAX_MS);
	n->retry_ms = n->resolved_ms + delay;
	if (!n->count && name_is_ca(n))
		proto_timer_arm(&mp.env.base, &mp.ca_retry, delay);
}

static void name_answered(struct mp_name *n)
{
	n->fails = 0;
	n->retry_ms = 0;
	if (name_is_ca(n))
		proto_timer_cancel(&mp.env.base, &mp.ca_retry);
}

/* A failed name waits for its retry, an answered one for the floor
 * (RFC 3435 4.3 p. 126). */
static bool name_due(const struct mp_name *n)
{
	uint64_t now = mp_now_ms();

	if (n->fails)
		return now >= n->retry_ms;

	return !n->resolved_ms || now - n->resolved_ms >= MP_RESOLVE_RECENT_MS;
}

static void name_query(struct mp_name *n)
{
	if (!mp.dns_up || n->query != SIP_HANDLE_NONE)
		return;

	sip_dns_cache_expire(&mp.dns, n->domain, SIP_DNS_ADDR, 0, NULL);
	n->query = sip_dns_resolve(&mp.dns, n->domain, SIP_DNS_ADDR, 0,
				   (uint32_t)(n - mp.name) + 1);
	MP_TRACE("resolve %s%s", n->domain,
		 n->query == SIP_HANDLE_NONE ? " refused" : "");
	if (n->query != SIP_HANDLE_NONE)
		return;

	n->resolved_ms = mp_now_ms();
	name_failed(n);
}

static void name_resolve(struct mp_name *n)
{
	if (name_due(n))
		name_query(n);
}

static struct mp_name *name_get(const char *domain)
{
	struct mp_name *n = name_find(domain);

	if (!n) {
		n = name_slot();
		if (!n)
			return NULL;
		memset(n, 0, sizeof(*n));
		n->used = true;
		str_copy(n->domain, sizeof(n->domain), domain);
	}

	if (!n->count || mp_now_ms() >= n->expires_ms)
		name_resolve(n);

	return n;
}

static void fw_ask_send(void);
static void unit_up(void);

static void dns_done(struct sip_dns *r, uint32_t query, uint32_t owner,
		     const struct sip_dns_result *res, void *priv)
{
	struct mp_name *n;
	unsigned int i;

	if (!owner || owner > MP_NAME_MAX)
		return;
	n = &mp.name[owner - 1];
	if (!n->used || n->query != query)
		return;

	n->query = SIP_HANDLE_NONE;
	n->resolved_ms = mp_now_ms();
	if (res->status != SIP_DNS_OK) {
		MP_TRACE("resolve %s: %s", n->domain,
			 sip_dns_status_name(res->status));
		name_failed(n);
		return;
	}

	n->count = 0;
	n->cur = 0;
	for (i = 0; i < res->count && n->count < MP_ADDR_MAX; i++) {
		if (res->target[i].dst.sa.ss_family == AF_INET)
			n->addr[n->count++] = res->target[i].dst;
	}
	n->expires_ms = n->resolved_ms + (uint64_t)res->ttl * 1000u;
	MP_TRACE("resolve %s: %u addresses", n->domain, n->count);
	fw_ask_send();
	if (!n->count) {
		name_failed(n);
		return;
	}

	name_answered(n);
	/* the address of the call agent can be what names the endpoints */
	unit_up();
}

/* The domain of an entity without its brackets, and its port. */
static bool entity_split(const char *entity, char *domain, size_t dlen,
			 uint16_t *port)
{
	struct mgcp_entity e;
	struct sip_str d;

	if (!mgcp_entity_parse(sip_str_cstr(entity), &e))
		return false;

	d = e.domain;
	if (d.len >= 2 && d.p[0] == '[' && d.p[d.len - 1] == ']')
		d = sip_str_init(d.p + 1, d.len - 2);
	*port = e.port ? (uint16_t)e.port : (uint16_t)mp.cfg.call_agent_port;

	return sip_str_copy(domain, dlen, d);
}

/* struct mgcp_ep_env dst: a literal at once, a name from its entry. */
static bool mp_dst(void *priv, const char *entity,
		   struct sockaddr_storage *out, socklen_t *len)
{
	char domain[CFG_MGCP_DOMAIN_MAX];
	struct sip_txn_dst d;
	struct mp_name *n;
	uint16_t port;

	if (!entity_split(entity, domain, sizeof(domain), &port))
		return false;

	if (!sip_dst_set(&d, domain, port)) {
		n = name_get(domain);
		if (!n || !n->count)
			return false;
		d = n->addr[n->cur < n->count ? n->cur : 0];
		sa_port_set(&d.sa, port);
	}

	*out = d.sa;
	*len = d.sa_len;

	return true;
}

static const struct mgcp_ep_env mp_ep_env = { .dst = mp_dst };

static bool addr_same(const struct sockaddr_storage *a,
		      const struct sockaddr_storage *b)
{
	const struct sockaddr_in *x = (const struct sockaddr_in *)a;
	const struct sockaddr_in *y = (const struct sockaddr_in *)b;

	return a->ss_family == AF_INET && b->ss_family == AF_INET &&
	       x->sin_addr.s_addr == y->sin_addr.s_addr;
}

static void name_next_take(struct mp_name *n, unsigned int k,
			   const struct mgcp_txn *t, uint32_t handle)
{
	struct sip_txn_dst next = n->addr[k];

	sa_port_set(&next.sa, sa_port_get(&t->dst));
	n->cur = k;
	mgcp_txn_redirect_one(&mp.mgr, handle, (struct sockaddr *)&next.sa,
			      next.sa_len);
	MP_TRACE("txn %u to the next address of %s", t->txid, n->domain);
}

/*
 * Max1 or Max2 of one transaction (RFC 3435 4.3 p. 125 to 126): the next
 * address of its name that was not tried takes the retransmissions of that
 * transaction alone. With none left, the name is asked again unless it was
 * asked recently; its answer serves the commands that follow, because the
 * query does not end inside this upcall.
 */
static void name_retry(uint32_t handle)
{
	struct mgcp_txn *t = mgcp_txn_get(&mp.mgr, handle);
	unsigned int i, k;

	if (!t)
		return;

	for (i = 0; i < MP_NAME_MAX; i++) {
		struct mp_name *n = &mp.name[i];

		for (k = 0; n->used && k < n->count; k++) {
			if (!addr_same(&n->addr[k].sa, &t->dst))
				continue;
			if (k + 1 < n->count)
				name_next_take(n, k + 1, t, handle);
			else
				name_resolve(n);
			return;
		}
	}
}

/* ------------------------------------------------------------- the wire */

static void tap_out(bool tx, const struct sockaddr_storage *peer,
		    const void *buf, size_t len)
{
	struct proto_tap_endpoints ep;

	if (!mp.tap)
		return;

	memset(&ep, 0, sizeof(ep));
	ep.kind = TRACE_PKT_MGCP;
	mp.host->calltrace_tap_endpoint_set(mp.host->ctx, &ep.local,
					    (const struct sockaddr *)&mp.local);
	mp.host->calltrace_tap_endpoint_set(mp.host->ctx, &ep.peer,
					    (const struct sockaddr *)peer);
	mp.tap(tx, &ep, buf, len);
}

/* A datagram that holds a response: one that starts with its code, or one
 * where an RSIP rides in front of it (RFC 3435 3.5.5). */
static bool wire_is_response(const char *buf, size_t len)
{
	return (len && buf[0] >= '0' && buf[0] <= '9') ||
	       memmem(buf, len, "\r\n.\r\n", 5) ||
	       memmem(buf, len, "\n.\n", 3);
}

static int mp_send(struct mgcp_txn_env *env, const void *buf, size_t len,
		   const struct sockaddr *dst, socklen_t dst_len)
{
	struct sockaddr_storage to;
	ssize_t n;

	if (!mp.sock_up) {
		mp.stat.tx_err++;
		return -ENOTCONN;
	}
	if (dst_len > sizeof(to))
		return -EINVAL;

	memset(&to, 0, sizeof(to));
	memcpy(&to, dst, dst_len);
	/* reply_method mgc_port: the responses go to the call agent port */
	if (mp.cfg.reply_method == CFG_MGCP_REPLY_MGC_PORT &&
	    wire_is_response(buf, len))
		sa_port_set(&to, (uint16_t)mp.cfg.call_agent_port);

	tap_out(true, &to, buf, len);
	n = sendto(mp.ufd.fd, buf, len, 0, (struct sockaddr *)&to, dst_len);
	if (n < 0) {
		int err = errno;

		mp.stat.tx_err++;
		MP_TRACE("send: %s", strerror(err));
		return -err;
	}

	mp.stat.tx++;
	mp.stat.tx_bytes += (uint64_t)n;

	return (int)n;
}

static void land_check(void);
static const struct mgcp_conn *conn_of(unsigned int line, unsigned int slot);

static void sock_read(struct uloop_fd *ufd, unsigned int events)
{
	struct sockaddr_storage src;
	socklen_t slen;
	ssize_t n;

	for (;;) {
		slen = sizeof(src);
		n = recvfrom(ufd->fd, mp.rx, sizeof(mp.rx), MSG_TRUNC,
			     (struct sockaddr *)&src, &slen);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				mp.stat.rx_err++;
			break;
		}
		if ((size_t)n > sizeof(mp.rx)) {
			mp.stat.truncated++;
			continue;
		}

		mp.stat.rx++;
		mp.stat.rx_bytes += (uint64_t)n;
		tap_out(false, &src, mp.rx, (size_t)n);
		mgcp_txn_recv(&mp.mgr, mp.rx, (size_t)n,
			      (struct sockaddr *)&src, slen);
		land_check();
	}
}

static void sock_close(void)
{
	if (mp.ufd.fd >= 0) {
		uloop_fd_delete(&mp.ufd);
		close(mp.ufd.fd);
	}
	mp.ufd.fd = -1;
	mp.sock_up = false;
}

/*
 * The socket on local_port at the IPv4 wildcard address, scoped to the device
 * of the voice interface, the bind of the SIP datagram socket
 * (proto/sip/account.c acct_sock_take()). A bind that fails sets
 * sock_failed, which down_count reports to the tracker.
 */
static bool sock_open(void)
{
	struct sockaddr_storage sa;
	socklen_t slen = sizeof(mp.local);
	int fd, err, tos = (int)((mp.cfg.signaling_dscp & 0x3f) << 2);

	sock_close();
	mp.sock_failed = false;
	addr_pton(AF_INET, "", &sa);
	sa_port_set(&sa, (uint16_t)mp.cfg.local_port);

	fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
		    IPPROTO_UDP);
	if (fd < 0)
		goto fail;
	if (!sock_iface_bind(fd, bind_dev()) ||
	    bind(fd, (struct sockaddr *)&sa, sa_len_of(AF_INET)) < 0 ||
	    getsockname(fd, (struct sockaddr *)&mp.local, &slen) < 0)
		goto fail_fd;
	if (sock_tos_set(fd, AF_INET, tos) < 0)
		MP_TRACE("signaling DSCP %u refused: %s",
			 mp.cfg.signaling_dscp, strerror(errno));

	mp.ufd.fd = fd;
	mp.ufd.cb = sock_read;
	if (uloop_fd_add(&mp.ufd, ULOOP_READ) < 0) {
		mp.ufd.fd = -1;
		goto fail_fd;
	}

	mp.sock_up = true;
	MP_TRACE("socket on port %u, device %s", mp.cfg.local_port,
		 bind_dev()[0] ? bind_dev() : "-");

	return true;

fail_fd:
	err = errno;
	close(fd);
	errno = err;
fail:
	mp.sock_failed = true;
	mp.host->log(mp.host->ctx, LOG_WARNING, "mgcp: socket on port %u: %s",
		     mp.cfg.local_port, strerror(errno));

	return false;
}

/* ---------------------------------------------------- the firewall ask */

/*
 * The signaling port of every enabled line and the addresses of the
 * configured call agent as the proxy sources (net/fw_reconcile.h).
 */
static void fw_ask_send(void)
{
	char domain[CFG_MGCP_DOMAIN_MAX + 2];
	char host[PROTO_FW_SRC_MAX];
	struct fw_sig_ask ask;
	struct mp_name *n;
	unsigned int i;

	if (!mp.started || mp.stopping)
		return;

	memset(&ask, 0, sizeof(ask));
	ask.proto_udp = true;
	for (i = 0; i < mp.lines; i++) {
		if (!(mp.enabled & (1u << i)))
			continue;
		ask.active_mask |= 1u << i;
		ask.port[i] = (uint16_t)mp.cfg.local_port;
	}

	str_copy(domain, sizeof(domain), mp.cfg.call_agent);
	n = domain[0] ? name_find(domain) : NULL;
	if (ip_literal(AF_INET, domain) || ip_literal(AF_INET6, domain))
		ask.nproxy = mp.host->fw_proxy_add(mp.host->ctx, ask.proxy,
						   ask.nproxy, domain);
	for (i = 0; n && i < n->count; i++) {
		if (sip_dst_host(&n->addr[i], host, sizeof(host)))
			ask.nproxy = mp.host->fw_proxy_add(mp.host->ctx,
							   ask.proxy,
							   ask.nproxy, host);
	}

	mp.host->fw_ask(mp.host->ctx, &ask);
}

/* ---------------------------------------------------- the transaction env */

static uint32_t mp_rand32(struct proto_timer_env *env)
{
	uint32_t v;

	if (mp.host->random_bytes(mp.host->ctx, &v, sizeof(v)))
		return v;

	/* no entropy source answered; the draw still moves */
	return (uint32_t)uvoice_mono_ms() * 2654435761u;
}

static void mp_txn_trace(struct proto_timer_env *env, const char *msg)
{
	PROTO_TRACE(mp.host, TRACE_MGCP, "%s", msg);
}

static void stop_check(void)
{
	if (!mp.stop_wait || mgcp_txn_count(&mp.mgr))
		return;

	mp.stop_wait = false;
	mp.host->shutdown_done(mp.host->ctx);
}

static void up_command(struct mgcp_txn_env *env, const struct mgcp_msg *msg,
		       const struct sockaddr *src, socklen_t src_len)
{
	if (mp.gw_up)
		mgcp_ep_command(&mp.gw, msg, src, src_len);
}

static void up_response(struct mgcp_txn_env *env, uint32_t owner,
			uint32_t txid, const struct mgcp_msg *msg)
{
	if (mp.gw_up)
		mgcp_ep_response(&mp.gw, owner, txid, msg);
}

static void up_done(struct mgcp_txn_env *env, uint32_t owner, uint32_t txid,
		    enum mgcp_txn_reason reason)
{
	if (mp.gw_up)
		mgcp_ep_txn_done(&mp.gw, owner, txid, reason);
	stop_check();
}

static void up_disconnected(struct mgcp_txn_env *env)
{
	if (mp.gw_up)
		mgcp_ep_disconnected(&mp.gw);
}

static void up_max(struct mgcp_txn_env *env, uint32_t owner, uint32_t txid,
		   uint32_t handle)
{
	name_retry(handle);
}

static void up_disc_attempt(struct mgcp_txn_env *env)
{
	mp.disc_attempts++;
	if (mp.gw_up)
		mgcp_ep_disc_attempt(&mp.gw);
}

static void up_disc_ended(struct mgcp_txn_env *env)
{
	mp.disc_attempts = 0;
	if (mp.gw_up)
		mgcp_ep_disc_ended(&mp.gw);
}

static bool mgr_up(void)
{
	struct mgcp_txn_cfg tc;

	txn_cfg_fill(&tc);
	if (mgcp_txn_mgr_init(&mp.mgr, &mp.env, &tc)) {
		mp.host->log(mp.host->ctx, LOG_ERR,
			     "mgcp: the transaction layer did not start");
		return false;
	}
	mp.mgr_up = true;

	return true;
}

/* ------------------------------------------------------------- the unit */

/*
 * The endpoints come up when their names are known: they take the service
 * state the core holds, the socket opens and the restart starts.
 */
static void unit_up(void)
{
	struct mgcp_ep_cfg ec;
	unsigned int i;

	if (mp.gw_up || !mp.started || mp.stopping || !mp.mgr_up)
		return;
	if (!gw_cfg_fill(&ec)) {
		MP_TRACE("no domain and no address: the endpoints wait");
		return;
	}
	if (mgcp_ep_init(&mp.gw, mp.host, &mp.mgr, &mp_ep_env, &ec)) {
		mp.host->log(mp.host->ctx, LOG_ERR,
			     "mgcp: endpoint pattern %s does not expand",
			     mp.cfg.endpoint_pattern);
		return;
	}
	mp.gw_up = true;

	for (i = 0; i < mp.lines; i++) {
		if (mp.svc_off & (1u << i))
			mgcp_ep_service(&mp.gw, i, false, MGCP_EP_REASON_OOS);
	}

	sock_open();
	mgcp_ep_start(&mp.gw);
	if (!mp.sock_failed)
		return;

	mgcp_ep_disc_all(&mp.gw);
	/* the tracker arms its recovery at a settle alone (net/iface.h) */
	mp.host->net_iface_recover_settle(mp.host->ctx);
}

/* Every endpoint leaves service on the wire, then the unit lets go of its
 * transactions, its socket and its names. */
static void unit_down(void)
{
	unsigned int i;

	if (mp.gw_up) {
		for (i = 0; i < mp.lines; i++)
			mgcp_ep_service(&mp.gw, i, false, MGCP_EP_REASON_OOS);
		mgcp_ep_done(&mp.gw);
		mp.gw_up = false;
	}
	if (mp.mgr_up) {
		mp.txid_first = mp.mgr.txid_next;
		mgcp_txn_mgr_done(&mp.mgr);
		mp.mgr_up = false;
	}
	sock_close();
	for (i = 0; i < MP_NAME_MAX; i++) {
		if (mp.name[i].used && mp.name[i].query != SIP_HANDLE_NONE)
			sip_dns_cancel(&mp.dns, mp.name[i].query);
	}
	memset(mp.name, 0, sizeof(mp.name));
	proto_timer_cancel(&mp.env.base, &mp.ca_retry);
	mp.disc_attempts = 0;
}

/* The name of the call agent resolves while the restart delay runs; a
 * literal needs no query. */
static void ca_name_kick(void)
{
	char domain[CFG_MGCP_DOMAIN_MAX];

	if (ca_domain_get(domain, sizeof(domain)))
		name_get(domain);
}

/* The timer runs out at the retry of the name, so it asks without the gate
 * of name_due(). */
static void ca_retry_fire(struct proto_timer *t)
{
	char domain[CFG_MGCP_DOMAIN_MAX];
	struct mp_name *n;

	if (!mp.started || mp.stopping ||
	    !ca_domain_get(domain, sizeof(domain)))
		return;

	n = name_find(domain);
	if (!n)
		ca_name_kick();
	else if (!n->count)
		name_query(n);
}

static void unit_restart(const struct cfg *cfg)
{
	MP_TRACE("restart of the unit");
	unit_down();
	if (cfg)
		cfg_take(cfg);
	mgr_up();
	ca_name_kick();
	unit_up();
	fw_ask_send();
}

/* A disconnected procedure that waits for the socket tries at once. */
static void disc_kick(void)
{
	if (mp.gw_up && mgcp_txn_disc_active(&mp.mgr) &&
	    mgcp_txn_disc_kick(&mp.mgr, false))
		mgcp_ep_disc_attempt(&mp.gw);
}

static void iface_take(const struct net_iface_state *st)
{
	str_copy(mp.addr4, sizeof(mp.addr4),
		 st && st->addr4_known ? st->addr4 : "");
}

/*
 * A new local address restarts the unit when the endpoint names hold it (no
 * domain option), else it moves the address of the descriptors to come. The
 * wildcard socket moves with its device alone.
 */
static void iface_changed(void *priv, const struct net_iface_state *st,
			  const struct net_iface_change *ch)
{
	char before[NET_IFACE_ADDR_MAX], after[NET_IFACE_ADDR_MAX];

	if (!mp.started || mp.stopping || (!ch->addr4 && !ch->dev))
		return;

	if (!local_addr_get(before, sizeof(before)))
		before[0] = '\0';
	iface_take(st);
	if (!local_addr_get(after, sizeof(after)))
		after[0] = '\0';
	MP_TRACE("local address %s", after[0] ? after : "-");

	if (!mp.cfg.domain[0] && strcmp(before, after)) {
		unit_restart(NULL);
		return;
	}
	if (!mp.gw_up) {
		unit_up();
		return;
	}

	str_copy(mp.gw.cfg.media_addr, sizeof(mp.gw.cfg.media_addr), after);
	str_copy(mp.gw.cfg.bind_dev, sizeof(mp.gw.cfg.bind_dev), bind_dev());
	if (!ch->dev)
		return;
	if (sock_open())
		disc_kick();
	else
		mgcp_ep_disc_all(&mp.gw);
}

static void iface_recover(void *priv)
{
	if (!mp.sock_failed || mp.stopping)
		return;
	if (sock_open())
		disc_kick();
}

static unsigned int iface_down_count(void *priv)
{
	return mp.sock_failed ? 1 : 0;
}

static const struct net_iface_sub mp_iface_sub = {
	.changed = iface_changed,
	.recover = iface_recover,
	.down_count = iface_down_count,
};

/* ------------------------------------------------------------- lifecycle */

static void mgcp_init(const struct proto_host *host, const struct cfg *cfg)
{
	struct sip_dns_cb dcb = { .done = dns_done };
	struct sip_dns_cfg dcfg;

	memset(&mp, 0, sizeof(mp));
	mp.ufd.fd = -1;
	proto_timer_init(&mp.ca_retry, NULL, 0, ca_retry_fire);
	mp.host = host;
	cfg_take(cfg);

	proto_timer_env_uloop_init(&mp.env.base);
	mp.env.base.rand32 = mp_rand32;
	mp.env.base.trace = mp_txn_trace;
	mp.env.send = mp_send;
	mp.env.command = up_command;
	mp.env.response = up_response;
	mp.env.done = up_done;
	mp.env.disconnected = up_disconnected;
	mp.env.max1 = up_max;
	mp.env.max2 = up_max;
	mp.env.disc_attempt = up_disc_attempt;
	mp.env.disc_ended = up_disc_ended;
	mp.txid_first = mp_rand32(&mp.env.base) % MGCP_TXID_MAX + 1;
	mgr_up();

	sip_dns_cfg_default(&dcfg);
	dcfg.trace_host = host;
	dcfg.trace_dom = TRACE_MGCP;
	sip_dns_init(&mp.dns, &dcfg, &dcb, NULL);
	mp.dns_up = true;
}

static void mgcp_start(const struct cfg *cfg)
{
	mp.started = true;
	mp.host->net_iface_subscribe(mp.host->ctx, &mp_iface_sub);
	iface_take(mp.host->net_iface_state_get(mp.host->ctx));
	ca_name_kick();
	unit_up();
	fw_ask_send();
}

static void mgcp_dsp_ready(uint32_t features, uint32_t codecs)
{
	sdp_t38_max_datagram_set(
		mp.host->hal_cap(mp.host->ctx)->t38_max_datagram);
}

/*
 * Every voip.mgcp option restarts the unit (APPLY_RESTART_ALL), and so do
 * the line count and the voice interface. A line whose RTP range moved
 * leaves service and comes back, which ends its connections.
 */
static void mgcp_reload(const struct cfg *old_cfg, const struct cfg *new_cfg)
{
	unsigned int i;

	if (memcmp(&old_cfg->mgcp, &new_cfg->mgcp, sizeof(new_cfg->mgcp)) ||
	    old_cfg->channels != new_cfg->channels ||
	    strcmp(old_cfg->local_interface, new_cfg->local_interface)) {
		unit_restart(new_cfg);
		return;
	}

	cfg_take(new_cfg);
	for (i = 0; mp.gw_up && i < mp.lines; i++) {
		if (mp.host->net_port_range_eq(mp.host->ctx,
					       &old_cfg->channel[i],
					       &new_cfg->channel[i]) ||
		    (mp.svc_off & (1u << i)) ||
		    (!conn_of(i, 0) && !conn_of(i, 1)))
			continue;
		mgcp_ep_service(&mp.gw, i, false, MGCP_EP_REASON_OOS);
		mgcp_ep_service(&mp.gw, i, true, 0);
	}
	fw_ask_send();
}

static void mgcp_shutdown_begin(void)
{
	mp.stopping = true;
	if (mp.gw_up)
		mgcp_ep_stop_begin(&mp.gw, MP_STOP_DELAY_S);
}

static bool mgcp_shutdown_pending(void)
{
	mp.stop_wait = mp.mgr_up && mgcp_txn_count(&mp.mgr);

	return mp.stop_wait;
}

/* The lines left service before this call (lines_done), so the streams
 * stopped on an open backend and only the memory and the socket go. */
static void mgcp_done(void)
{
	unsigned int i;

	mp.host->net_iface_subscribe(mp.host->ctx, NULL);
	if (mp.gw_up)
		mgcp_ep_done(&mp.gw);
	mp.gw_up = false;
	if (mp.mgr_up)
		mgcp_txn_mgr_done(&mp.mgr);
	mp.mgr_up = false;
	sock_close();
	for (i = 0; i < MP_NAME_MAX; i++) {
		if (mp.name[i].used && mp.name[i].query != SIP_HANDLE_NONE)
			sip_dns_cancel(&mp.dns, mp.name[i].query);
	}
	proto_timer_cancel(&mp.env.base, &mp.ca_retry);
	if (mp.dns_up)
		sip_dns_done(&mp.dns);
	mp.dns_up = false;
	mp.started = false;
	mp.tap = NULL;
}

/* ------------------------------------------------------------- the lines */

/* The staged config of a line lands at its rest (proto/proto.h
 * line_cfg_changed); the endpoints read the channel and the region through the
 * host table. */
static void land_check(void)
{
	unsigned int i;

	for (i = 0; mp.land && i < CFG_CHANNEL_MAX; i++) {
		if (!(mp.land & (1u << i)))
			continue;
		if (mp.gw_up && mgcp_ep_line_flags(&mp.gw, i))
			continue;
		mp.land &= ~(1u << i);
		mp.host->line_cfg_landed(mp.host->ctx, i);
	}
}

static void mgcp_line_event(unsigned int line,
			    const struct proto_line_event *ev)
{
	if (mp.gw_up)
		mgcp_ep_line_event(&mp.gw, line, ev);
	land_check();
}

static void mgcp_line_service(unsigned int line, bool in_service,
			      enum proto_service_reason reason)
{
	if (line >= CFG_CHANNEL_MAX)
		return;

	if (in_service)
		mp.svc_off &= ~(1u << line);
	else
		mp.svc_off |= 1u << line;

	if (mp.gw_up)
		mgcp_ep_service(&mp.gw, line, in_service,
				reason == PROTO_SERVICE_FAULT ?
				MGCP_EP_REASON_FAULT : MGCP_EP_REASON_OOS);
	land_check();
}

static void mgcp_line_reset(unsigned int line)
{
	if (mp.gw_up)
		mgcp_ep_line_reset(&mp.gw, line);
}

static void mgcp_line_cfg_changed(unsigned int line)
{
	if (line >= CFG_CHANNEL_MAX)
		return;

	mp.land |= 1u << line;
	land_check();
}

static void mgcp_region_changed(unsigned int line,
				const struct hal_region *region)
{
	mgcp_line_cfg_changed(line);
}

/* A held ring keeps the TO signal running: no event of the line package
 * reports the hold. */
static void mgcp_ring_held(unsigned int line)
{
	MP_TRACE("line %u ring held", line);
}

static void mgcp_ring_started(unsigned int line)
{
	MP_TRACE("line %u ring started", line);
}

static void mgcp_ring_failed(unsigned int line)
{
	if (mp.gw_up)
		mgcp_ep_ring_failed(&mp.gw, line);
	land_check();
}

static void mgcp_signal_result(unsigned int line, enum proto_signal sig,
			       bool ok)
{
	if (mp.gw_up)
		mgcp_ep_signal_result(&mp.gw, line, sig, ok);
	land_check();
}

static uint32_t mgcp_line_flags(unsigned int line)
{
	return mp.gw_up ? mgcp_ep_line_flags(&mp.gw, line) : 0;
}

static const char *mgcp_state_name(unsigned int line)
{
	return mp.gw_up ? mgcp_ep_state_name(&mp.gw, line) : "down";
}

static bool mgcp_digit_play_allowed(unsigned int line, unsigned int stream)
{
	return mp.gw_up && mgcp_ep_digit_play_allowed(&mp.gw, line);
}

static const struct mgcp_conn *conn_of(unsigned int line, unsigned int slot)
{
	if (!mp.gw_up || line >= mp.gw.cfg.lines || slot >= MGCP_CONN_MAX ||
	    !mp.gw.ep[line].conn[slot].used)
		return NULL;

	return &mp.gw.ep[line].conn[slot];
}

static unsigned int mgcp_stream_of_slot(unsigned int line, unsigned int slot)
{
	const struct mgcp_conn *c = conn_of(line, slot);

	return c ? c->stream : HAL_STREAM_MAX;
}

/* The rule of the SIP legs (proto/sip/call.c calls_line_fax_kind()): a T.38
 * stream counts as the modem kind. */
static uint8_t mgcp_line_fax_kind(unsigned int line, bool *media_running)
{
	uint8_t kind = HAL_PASSTHROUGH_OFF;
	unsigned int i;

	*media_running = false;
	for (i = 0; i < MGCP_CONN_MAX; i++) {
		const struct mgcp_conn *c = conn_of(line, i);

		if (!c || !c->media_started)
			continue;
		*media_running = true;
		if (c->media.codec == HAL_CODEC_T38 ||
		    c->fax_kind == HAL_PASSTHROUGH_MODEM)
			return HAL_PASSTHROUGH_MODEM;
		if (c->fax_kind == HAL_PASSTHROUGH_FAX)
			kind = HAL_PASSTHROUGH_FAX;
	}

	return kind;
}

/* ------------------------------------------------------------ the status */

static void mgcp_status_dump_call(struct blob_buf *b, unsigned int line)
{
	blobmsg_add_string(b, "state", mgcp_state_name(line));
}

static void remote_add(struct blob_buf *b, const struct hal_addr *a)
{
	char text[INET6_ADDRSTRLEN];

	if (!inet_ntop(a->ipver == 6 ? AF_INET6 : AF_INET, a->addr, text,
		       sizeof(text)))
		return;

	blobmsg_printf(b, "remote", a->ipver == 6 ? "[%s]:%u" : "%s:%u",
		       text, a->port);
}

static void conns_dump(struct blob_buf *b, const struct mgcp_ep *ep)
{
	void *arr = blobmsg_open_array(b, "connections");
	unsigned int i;

	for (i = 0; i < MGCP_CONN_MAX; i++) {
		const struct mgcp_conn *c = &ep->conn[i];
		void *tbl;

		if (!c->used)
			continue;
		tbl = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "id", c->id);
		blobmsg_add_string(b, "call_id", c->call_id);
		blobmsg_add_string(b, "mode",
				   mgcp_mode_name((enum mgcp_mode)c->mode));
		blobmsg_add_u32(b, "stream", c->stream);
		if (c->media_set)
			remote_add(b, &c->media.remote);
		blobmsg_close_table(b, tbl);
	}
	blobmsg_close_array(b, arr);
}

/* The service word of an endpoint, with the disconnected procedure. */
static const char *service_word(const struct mgcp_ep *ep)
{
	if (ep->disc)
		return "disconnected";

	return mgcp_ep_svc_name((enum mgcp_ep_svc)ep->svc);
}

static void mgcp_status_dump_lines(struct blob_buf *b, int want_line)
{
	void *arr = blobmsg_open_array(b, "lines");
	unsigned int i;

	for (i = 0; mp.gw_up && i < mp.gw.cfg.lines; i++) {
		const struct mgcp_ep *ep = &mp.gw.ep[i];
		void *tbl;

		if (want_line >= 0 && (unsigned int)want_line != i)
			continue;
		tbl = blobmsg_open_table(b, NULL);
		blobmsg_add_u32(b, "line", i);
		blobmsg_add_string(b, "endpoint", ep->name);
		blobmsg_add_string(b, "service", service_word(ep));
		blobmsg_add_string(b, "state", mgcp_ep_state_name(&mp.gw, i));
		blobmsg_add_string(b, "notified_entity", ep->entity);
		blobmsg_add_string(b, "request_id", ep->rid);
		conns_dump(b, ep);
		blobmsg_close_table(b, tbl);
	}
	blobmsg_close_array(b, arr);
}

static const char *socket_word(void)
{
	if (mp.sock_up)
		return "bound";

	return mp.sock_failed ? "failed" : "waiting";
}

static void txn_dump(struct blob_buf *b)
{
	const struct mgcp_txn_stat *s = &mp.mgr.stat;
	void *tbl = blobmsg_open_table(b, "transactions");

	blobmsg_add_u32(b, "open", mp.mgr_up ? mgcp_txn_count(&mp.mgr) : 0);
	blobmsg_add_u64(b, "created", s->created);
	blobmsg_add_u64(b, "retransmitted", s->retransmitted);
	blobmsg_add_u64(b, "timeout", s->timeout);
	blobmsg_add_u64(b, "transport", s->transport);
	blobmsg_add_u64(b, "denied", s->denied);
	blobmsg_add_u64(b, "absorbed", s->absorbed);
	blobmsg_add_u64(b, "stray", s->stray);
	blobmsg_add_u64(b, "malformed", s->malformed);
	blobmsg_close_table(b, tbl);
}

/* The top-level "mgcp" table of voice status (proto/proto.h
 * status_dump_top). */
static void mgcp_status_dump_top(struct blob_buf *b)
{
	char entity[MGCP_ENTITY_MAX];
	void *tbl = blobmsg_open_table(b, "mgcp");

	blobmsg_add_string(b, "dialect",
			   mp.dialect == CFG_MGCP_NCS ? "ncs" : "rfc3435");
	blobmsg_add_string(b, "call_agent", mp.cfg.call_agent);
	if (mp.gw_up && mp.gw.cfg.lines)
		str_copy(entity, sizeof(entity), mp.gw.ep[0].entity);
	else
		entity_text(entity, sizeof(entity));
	blobmsg_add_string(b, "notified_entity", entity);
	blobmsg_add_u32(b, "local_port", mp.cfg.local_port);
	blobmsg_add_string(b, "socket", socket_word());
	blobmsg_add_u8(b, "disconnected",
		       mp.mgr_up && mgcp_txn_disc_active(&mp.mgr));
	blobmsg_add_u32(b, "disc_attempts", mp.disc_attempts);
	txn_dump(b);
	blobmsg_add_u64(b, "commands", mp.gw.stat.commands);
	blobmsg_add_u64(b, "errors", mp.gw.stat.errors);
	blobmsg_add_u64(b, "ntfy", mp.gw.stat.ntfy);
	blobmsg_add_u64(b, "rsip", mp.gw.stat.rsip);
	blobmsg_add_u64(b, "quar_dropped", mp.gw.stat.quar_dropped);
	blobmsg_close_table(b, tbl);
}

/* --------------------------------------------------- the call trace hooks */

/* The capture filter ports: the local port catches every exchange, and the
 * call agent port catches a reply method that answers there. */
static unsigned int mgcp_wire_ports(unsigned int line, uint16_t *ports,
				    unsigned int max)
{
	unsigned int n = 0;

	if (n < max)
		ports[n++] = (uint16_t)mp.cfg.local_port;
	if (mp.cfg.call_agent_port != mp.cfg.local_port && n < max)
		ports[n++] = (uint16_t)mp.cfg.call_agent_port;

	return n;
}

static void mgcp_tap_set(proto_tap_fn fn)
{
	mp.tap = fn;
}

/* The counters of the one socket, which serves every line. */
static void mgcp_wire_stats(struct blob_buf *b)
{
	void *tbl = blobmsg_open_table(b, "mgcp");

	blobmsg_add_u64(b, "rx", mp.stat.rx);
	blobmsg_add_u64(b, "rx_bytes", mp.stat.rx_bytes);
	blobmsg_add_u64(b, "tx", mp.stat.tx);
	blobmsg_add_u64(b, "tx_bytes", mp.stat.tx_bytes);
	blobmsg_add_u64(b, "tx_err", mp.stat.tx_err);
	blobmsg_add_u64(b, "rx_err", mp.stat.rx_err);
	blobmsg_add_u64(b, "truncated", mp.stat.truncated);
	blobmsg_add_u64(b, "parse_err", mp.mgr.stat.malformed);
	blobmsg_close_table(b, tbl);
}

const struct proto_ops mgcp_proto_ops = {
	.name = "mgcp",
	.init = mgcp_init,
	.start = mgcp_start,
	.dsp_ready = mgcp_dsp_ready,
	.reload = mgcp_reload,
	.shutdown_begin = mgcp_shutdown_begin,
	.shutdown_pending = mgcp_shutdown_pending,
	.done = mgcp_done,
	.line_event = mgcp_line_event,
	.line_service = mgcp_line_service,
	.line_reset = mgcp_line_reset,
	.line_cfg_changed = mgcp_line_cfg_changed,
	.region_changed = mgcp_region_changed,
	.ring_held = mgcp_ring_held,
	.ring_started = mgcp_ring_started,
	.ring_failed = mgcp_ring_failed,
	.signal_result = mgcp_signal_result,
	.line_flags = mgcp_line_flags,
	.state_name = mgcp_state_name,
	.digit_play_allowed = mgcp_digit_play_allowed,
	.stream_of_slot = mgcp_stream_of_slot,
	.line_fax_kind = mgcp_line_fax_kind,
	.status_dump_call = mgcp_status_dump_call,
	.status_dump_lines = mgcp_status_dump_lines,
	.status_dump_top = mgcp_status_dump_top,
	.wire_ports = mgcp_wire_ports,
	.tap_set = mgcp_tap_set,
	.wire_stats = mgcp_wire_stats,
};

/* ------------------------------------------------------------ test access */

struct sip_dns *mgcp_proto_test_dns(void)
{
	return &mp.dns;
}

struct proto_timer_env *mgcp_proto_test_timer_env(void)
{
	return &mp.env.base;
}

unsigned int mgcp_proto_test_ca_retry_ms(void)
{
	char domain[CFG_MGCP_DOMAIN_MAX];
	const struct mp_name *n;
	uint64_t now = mp_now_ms();

	if (!ca_domain_get(domain, sizeof(domain)))
		return 0;
	n = name_find(domain);
	if (!n || !n->fails || n->retry_ms <= now)
		return 0;

	return (unsigned int)(n->retry_ms - now);
}
