// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Asynchronous DNS (src/proto/shared/resolve.c). The SIP unit resolves its
 * servers with it, and the MGCP unit its call agent.
 *
 * Trace. The resolver writes its wire lines through the host table and the
 * domain that struct sip_dns_cfg names (trace_host, trace_dom), so each
 * protocol traces in its own domain. A NULL trace_host traces nothing.
 *
 * Why it is hand rolled. musl exports no usable asynchronous resolver: res_*
 * and getaddrinfo() both block the calling thread. A blocking lookup on the
 * uloop thread stalls every call, every timer and every registration of the
 * other line, so the queries are built and parsed here and travel on a non
 * blocking UDP socket inside uloop. No threads.
 *
 * What it answers. A, AAAA and SRV, with CNAME chased inside a response and
 * across responses up to SIP_DNS_CNAME_MAX hops. SIP_DNS_SRV runs the two
 * stage lookup the product needs: the SRV set, then an address for every
 * target that the additional section did not already carry, and it falls back
 * to a plain address lookup when the zone has no SRV records (RFC 3263 4.2).
 *
 * SIP_DNS_ADDR_SRV runs the two stages the other way round. It asks the
 * address of the name first, as RFC 3263 4.2 asks for a next hop that names a
 * port. It asks SRV only when every address question got a definitive
 * negative: NXDOMAIN, or a complete NOERROR answer that holds no record and no
 * CNAME to follow. A timeout, a server failure, a cut CNAME chain or a
 * truncated answer of any rcode can hide an address, so none of them starts
 * the SRV stage. "No address" means none of the families the query asks: with
 * cfg.ipv6 clear it asks A alone, so a name with AAAA records only gets the
 * SRV question too. The SRV stage of this kind has no address fallback,
 * because the address already failed. The SRV question is product policy
 * beyond RFC 3263 4.2: a zone that names its service only in SRV records is
 * otherwise unreachable for a line that names a port.
 *
 * Which kind to ask is the caller's choice and no policy of this layer. The
 * account layer derives it per line from the configured dns_srv_mode word and
 * the ports the line names (account.h acct_dns_kind()), the posture the
 * dns_cache_update_mode paragraph below takes for its own hook. struct
 * sip_dns_result carries `srv` to report which answer arrived: true when SRV
 * records built the targets, false for the address fallback, a plain address
 * answer and a literal. It joins the cache entry, so a cache hit reports the
 * same value, and it stays out of the target comparison, so it never makes an
 * answer read as changed.
 *
 * Numeric targets. A name that is already an address, "127.0.0.1" or
 * "2001:db8::1", is used directly and never asked for (RFC 3263 4). It answers
 * with one target, the caller's port and a fixed TTL, for every kind including
 * SIP_DNS_SRV, because a literal has no zone to hold an SRV record. Both
 * families pass whatever cfg.ipv6 holds: cfg.ipv6 picks the question for a
 * name, and the family of an answer travels in the target. The cache holds no
 * literal, in either direction, so `changed` is always false for one.
 *
 * What it does not do. No NAPTR (the transport comes from the config, not from
 * NAPTR discovery), no TCP retry after a truncated response, no DNSSEC, no
 * negative caching, no search list and no ndots processing: a SIP proxy name
 * from uci is used exactly as configured.
 *
 * Servers. Read from /etc/resolv.conf at the start of every resolution cycle
 * that is not served from the cache, so a DHCP renew that rewrites the file is
 * picked up without a reload. Each server gets 1 + SIP_DNS_RETRIES
 * transmissions before the next one is tried.
 *
 * Cache. Keyed by (name, kind, port, service label) with the smallest TTL of
 * the records that built the answer, clamped into
 * [SIP_DNS_TTL_MIN, SIP_DNS_TTL_MAX]. The service label is in the key because
 * one name answers differently per transport: "_sip._tcp.example.net" and
 * "_sips._tcp.example.net" are two questions with two answers, and a cache
 * that held only the name would serve one transport's targets to the other.
 * The label is the empty string for every kind but SIP_DNS_SRV and
 * SIP_DNS_ADDR_SRV, because only those two can ask an SRV question. The cache
 * holds the answer in a canonical order (priority, then the address bytes),
 * so "did the answer change" is a comparison and not a race with the weight
 * randomization. Every delivery, from the wire or from the cache, applies the
 * RFC 2782 weight shuffle to its own copy.
 *
 * The dns_cache_update_mode hook. struct sip_dns_result carries `changed`,
 * which is true when this answer differs from the one the cache held. That is
 * the whole hook: mode 1 re-registers and recomputes the firewall rules on a
 * changed answer, mode 0 does not, and the decision belongs to the account
 * layer, not here.
 *
 * Lifetime. A query is named by a handle ((index << 16) | generation, the
 * shared/handle.h rule) and carries an opaque owner handle that this layer
 * never dereferences. sip_dns_owner_release() cancels every query of an owner
 * that went away. That differs from sip_txn_owner_release(), which keeps its
 * transactions running: a transaction owes the peer a protocol end, a lookup
 * owes nobody anything.
 *
 * Callback order. done() never runs inside sip_dns_resolve(), not even for a
 * cache hit or an immediate failure. Those complete through a zero millisecond
 * uloop timeout, so a caller can store the handle it receives before the answer
 * arrives.
 */
#ifndef UVOICE_SIP_RESOLVE_H
#define UVOICE_SIP_RESOLVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libubox/uloop.h>

#include "proto/proto.h"
#include "proto/shared/dst.h"
#include "proto/shared/handle.h"

/* presentation form, trailing NUL included; the root is the empty string */
#define SIP_DNS_NAME_MAX	256
#define SIP_DNS_HOST_MAX	48

#define SIP_DNS_SERVER_MAX	4
#define SIP_DNS_TARGET_MAX	16
#define SIP_DNS_QUERY_MAX	4
#define SIP_DNS_CACHE_MAX	8

#define SIP_DNS_CNAME_MAX	4	/* CNAME hops, in and across responses */
#define SIP_DNS_RETRIES		2	/* retransmissions per server */
#define SIP_DNS_TIMEOUT_MS	2000
#define SIP_DNS_PORT		53

/*
 * 1500 is the receive buffer. Without EDNS a server truncates at 512, and an
 * SRV answer with its glue passes that easily, so uvoice advertises an EDNS0
 * payload size and keeps the buffer above it.
 */
#define SIP_DNS_PKT_MAX		1500
#define SIP_DNS_EDNS_SIZE	1232

#define SIP_DNS_TTL_MIN		10
#define SIP_DNS_TTL_MAX		86400

/*
 * The three RFC 3263 4.1 service labels, one per transport. The default is UDP
 * and stays the one a SIP_DNS_SRV caller that names none takes. The label
 * lives in SIP_DNS_SERVICE_MAX bytes, so a longer one is truncated into that
 * store, exactly as a longer name is bounded downstream; a label that long is
 * not a real SIP service and never reaches this path from the account layer.
 */
#define SIP_DNS_SERVICE_DEFAULT	"_sip._udp"
#define SIP_DNS_SERVICE_TCP	"_sip._tcp"
#define SIP_DNS_SERVICE_TLS	"_sips._tcp"
#define SIP_DNS_SERVICE_MAX	16

#define SIP_DNS_RESOLV_PATH	"/etc/resolv.conf"

/* ------------------------------------------------------------- wire format */

#define DNS_HDR_LEN		12
#define DNS_LABEL_MAX		63
#define DNS_NAME_WIRE_MAX	255
#define DNS_PTR_MAX		128	/* compression jumps before a refusal */

enum dns_type {
	DNS_T_A = 1,
	DNS_T_NS = 2,
	DNS_T_CNAME = 5,
	DNS_T_SOA = 6,
	DNS_T_PTR = 12,
	DNS_T_MX = 15,
	DNS_T_TXT = 16,
	DNS_T_AAAA = 28,
	DNS_T_SRV = 33,
	DNS_T_OPT = 41,
};

#define DNS_CLASS_IN		1

#define DNS_F_QR		(1u << 15)
#define DNS_F_AA		(1u << 10)
#define DNS_F_TC		(1u << 9)
#define DNS_F_RD		(1u << 8)
#define DNS_F_RA		(1u << 7)

#define DNS_RCODE(flags)	((flags) & 0xfu)

enum dns_rcode {
	DNS_RCODE_OK = 0,
	DNS_RCODE_FORMERR = 1,
	DNS_RCODE_SERVFAIL = 2,
	DNS_RCODE_NXDOMAIN = 3,
	DNS_RCODE_NOTIMP = 4,
	DNS_RCODE_REFUSED = 5,
};

enum dns_section {
	DNS_SECTION_ANSWER = 0,
	DNS_SECTION_AUTHORITY,
	DNS_SECTION_ADDITIONAL,
	DNS_SECTION_MAX,
};

struct dns_pkt {
	const uint8_t *buf;
	size_t len;

	uint16_t id;
	uint16_t flags;
	uint16_t count[DNS_SECTION_MAX];

	char qname[SIP_DNS_NAME_MAX];
	uint16_t qtype;
	uint16_t qclass;
	bool has_question;

	size_t rr_off;
};

struct dns_rr {
	char name[SIP_DNS_NAME_MAX];
	uint16_t type;
	uint16_t rclass;
	uint32_t ttl;
	size_t rdata;			/* offset into pkt->buf */
	uint16_t rdlen;
	uint8_t section;
};

struct dns_rr_iter {
	size_t off;
	uint16_t left[DNS_SECTION_MAX];
	uint8_t section;
};

/*
 * Decode one name. *next receives the offset just past the name as encoded at
 * off, which is what a resource record walk needs; a compression pointer
 * therefore ends the encoded name after two bytes.
 *
 * A compression pointer must point strictly backwards. With the 255 byte wire
 * limit and the jump cap that makes a compression loop impossible to build:
 * every reachable input either decodes or is refused, and neither hangs nor
 * reads outside the buffer. Returns 0 or a negative value.
 */
int dns_name_read(const void *buf, size_t len, size_t off, char *out, size_t olen,
		  size_t *next);

/* Encode name at off. Returns the number of bytes written or a negative value. */
int dns_name_write(void *buf, size_t blen, size_t off, const char *name);

/* case insensitive, and a trailing dot on either side is ignored */
bool dns_name_eq(const char *a, const char *b);

/*
 * Build a query with RD set. edns adds the OPT record that raises the server
 * side UDP limit to SIP_DNS_EDNS_SIZE. Returns the length or a negative value.
 */
int dns_query_build(void *buf, size_t blen, uint16_t id, const char *name, uint16_t type,
		    bool edns);

/*
 * Parse a response far enough to walk it. Refuses a short header, a question
 * count above one (uvoice never asks more than one question, so a response
 * that carries two is not an answer to us) and an unreadable question.
 */
int dns_pkt_parse(struct dns_pkt *p, const void *buf, size_t len);

/*
 * Walk answer, authority and additional in that order. A count that lies ends
 * the walk when the buffer runs out, so a hostile header costs nothing.
 */
bool dns_rr_first(const struct dns_pkt *p, struct dns_rr_iter *it, struct dns_rr *rr);
bool dns_rr_next(const struct dns_pkt *p, struct dns_rr_iter *it, struct dns_rr *rr);

bool dns_rr_addr(const struct dns_pkt *p, const struct dns_rr *rr, uint16_t port,
		 struct sip_txn_dst *dst);
bool dns_rr_name(const struct dns_pkt *p, const struct dns_rr *rr, char *out, size_t olen);
bool dns_rr_srv(const struct dns_pkt *p, const struct dns_rr *rr, uint16_t *prio,
		uint16_t *weight, uint16_t *port, char *target, size_t tlen);

/* ---------------------------------------------------------------- results */

enum sip_dns_kind {
	SIP_DNS_ADDR = 0,	/* A, and AAAA too when cfg.ipv6 is set */
	SIP_DNS_A,
	SIP_DNS_AAAA,
	SIP_DNS_SRV,		/* <service>.<name> SRV, then the addresses */
	SIP_DNS_ADDR_SRV,	/* the address, then SRV if the name has none */
	SIP_DNS_KIND_MAX,
};

const char *sip_dns_kind_name(enum sip_dns_kind kind);

enum sip_dns_status {
	SIP_DNS_OK = 0,
	SIP_DNS_ERR_NXDOMAIN,	/* the name does not exist */
	SIP_DNS_ERR_NO_SERVICE,	/* SRV target ".": the zone refuses the service */
	SIP_DNS_ERR_TIMEOUT,	/* every server ran out of attempts */
	SIP_DNS_ERR_SERVFAIL,	/* every server refused or failed */
	SIP_DNS_ERR_FORMAT,	/* answers arrived, none of them usable */
	SIP_DNS_ERR_NO_SERVER,	/* resolv.conf named none */
	SIP_DNS_ERR_CANCELLED,
	SIP_DNS_ERR_INTERNAL,	/* socket or pool */
};

const char *sip_dns_status_name(enum sip_dns_status st);

/*
 * The getrandom() entropy source with the urandom fallback the resolver uses
 * for its transaction ids, exported so the daemon can feed the same entropy
 * into the transaction layer's rand32 hook: the layer's own fallback
 * generator seeds from the millisecond clock, so two managers created in the
 * same millisecond draw identical branch and identifier streams, and the
 * shared-socket routing then delivers one account's responses to the other.
 * Returns false only when both sources fail.
 */
bool sip_os_random(void *buf, size_t len);

/*
 * One resolved next hop. dst is ready for sip_txn_client_start() and host is
 * the same address in presentation form, which is what sip_reg_target_add()
 * takes. prio and weight are 0 for anything that did not come from SRV.
 *
 * group ties the addresses of one SRV record together, so the RFC 2782 weight
 * selection draws per SRV record and not per address: a target with two A
 * records must not get twice the selection chance. It is the SRV record ordinal
 * for an SRV result and unique per target otherwise. A caller that only needs
 * the next hop ignores it.
 */
struct sip_dns_target {
	struct sip_txn_dst dst;
	char host[SIP_DNS_HOST_MAX];
	uint16_t port;
	uint16_t prio;
	uint16_t weight;
	uint16_t group;
};

struct sip_dns_result {
	enum sip_dns_status status;
	struct sip_dns_target target[SIP_DNS_TARGET_MAX];
	unsigned int count;
	uint32_t ttl;			/* seconds the answer stays valid */
	bool changed;			/* differs from the cached answer */
	bool cached;			/* served without a query */
	bool srv;			/* the targets came from SRV records */
	bool truncated;			/* a response had TC set */
};

/*
 * RFC 2782 ordering, exported because it is a rule worth pinning in a test:
 * ascending priority, and inside one priority a weighted random draw. rand32
 * may be NULL, in which case the order inside a priority is the input order.
 */
void sip_dns_srv_sort(struct sip_dns_target *t, unsigned int count,
		      uint32_t (*rand32)(void *priv), void *priv);

/*
 * Move the targets of the preferred family to the front of each (priority,
 * group) run. The partition is stable, so both families keep the order they
 * arrived in. It runs behind sip_dns_srv_sort() and never carries a target
 * across a priority or across an SRV record: RFC 2782 priority and weight are
 * the zone operator's routing policy, and a host preference must not overrule
 * them (RFC 6724 orders destination addresses, not services).
 *
 * family is AF_INET6, AF_INET or 0, and 0 leaves the order exactly as it was.
 * Every target of a plain address answer carries priority 0 and group 0, so
 * there the whole list is one run and partitions whole.
 */
void sip_dns_family_sort(struct sip_dns_target *t, unsigned int count,
			 int family);

/* the canonical order the cache holds: priority, then the raw address bytes */
void sip_dns_canon_sort(struct sip_dns_target *t, unsigned int count);

/* --------------------------------------------------------------- resolver */

struct sip_dns;

/*
 * done() reports one finished query. res is valid for the call only. query is
 * the handle sip_dns_resolve() returned and is already dead when done() runs,
 * exactly like a transaction handle in done().
 *
 * rand32 feeds the RFC 2782 SRV weight draw only. NULL takes an internal
 * generator. The DNS transaction id does not come from here: it comes from OS
 * randomness (getrandom, /dev/urandom), because a predictable id is a cache
 * poisoning vector for a remote resolver.
 */
struct sip_dns_cb {
	void (*done)(struct sip_dns *r, uint32_t query, uint32_t owner,
		     const struct sip_dns_result *res, void *priv);
	uint32_t (*rand32)(void *priv);
};

/*
 * resolv_path  NULL takes SIP_DNS_RESOLV_PATH.
 * service      the default SRV service and protocol, "_sip._udp" style, used
 *              when a resolve names none. NULL or "" takes
 *              SIP_DNS_SERVICE_DEFAULT. It is held in SIP_DNS_SERVICE_MAX
 *              bytes per query, so a longer string is truncated there.
 * timeout_ms   per transmission, 0 takes SIP_DNS_TIMEOUT_MS.
 * retries      retransmissions per server, 0 takes SIP_DNS_RETRIES.
 * ipv6         query AAAA next to A for SIP_DNS_ADDR and SIP_DNS_ADDR_SRV, take
 *              AAAA glue out of an SRV additional section, and ask AAAA beside
 *              A for an SRV target whose glue did not carry that family.
 * edns         advertise SIP_DNS_EDNS_SIZE.
 * srv_fallback SIP_DNS_SRV with no SRV record falls back to the address of the
 *              plain name with the port the caller asked for. It has no effect
 *              on SIP_DNS_ADDR_SRV, whose SRV stage follows a failed address.
 *
 * A zero in a numeric field takes the default above it. The three booleans have
 * no such rule, because false is a real answer for all three: fill the struct
 * from sip_dns_cfg_default() and change what the config asks for. Passing NULL
 * to sip_dns_init() takes the defaults whole.
 */
struct sip_dns_cfg {
	const char *resolv_path;
	const char *service;
	uint32_t timeout_ms;
	uint32_t retries;
	uint32_t ttl_min;
	uint32_t ttl_max;
	bool ipv6;
	bool edns;
	bool srv_fallback;
	/* the host table and domain of the wire trace; NULL traces nothing */
	const struct proto_host *trace_host;
	enum trace_domain trace_dom;
};

struct sip_dns_stat {
	uint64_t queries;		/* resolutions started */
	uint64_t cache_hits;
	uint64_t tx;			/* questions put on the wire */
	uint64_t rx;			/* responses accepted */
	uint64_t dropped;		/* responses that matched nothing */
	uint64_t retries;
	uint64_t timeouts;		/* per transmission */
	uint64_t failures;		/* resolutions that ended in an error */
	uint64_t changed;		/* answers that differed from the cache */
	uint64_t truncated;
	uint64_t denied;		/* pool full */
};

struct sip_dns_query {
	/*
	 * The uloop transport owns both of these: timer is the retransmission
	 * and completion timer, ufd is the one socket this query sends on and
	 * receives on. A socket per outstanding query gives a random ephemeral
	 * source port per lookup, which is part of the anti spoofing defence
	 * beside the random transaction id (struct sip_dns_cb). A test
	 * transport leaves both untouched.
	 */
	struct uloop_timeout timer;
	struct uloop_fd ufd;
	int fd_family;
	struct sip_dns *r;

	char name[SIP_DNS_NAME_MAX];		/* what the caller asked for */
	char qname[SIP_DNS_NAME_MAX];		/* the question in flight */
	char srv_name[SIP_DNS_TARGET_MAX][SIP_DNS_NAME_MAX];

	/*
	 * The SRV service label this query asked with, resolved once at the
	 * start: the caller's label, else cfg.service, else
	 * SIP_DNS_SERVICE_DEFAULT. Empty for every kind but SIP_DNS_SRV and
	 * SIP_DNS_ADDR_SRV. It is held here and not read from cfg again, so a
	 * configuration change mid query cannot move the answer to another
	 * cache identity.
	 */
	char service[SIP_DNS_SERVICE_MAX];

	struct sip_dns_target target[SIP_DNS_TARGET_MAX];
	unsigned int count;

	/*
	 * The server list this query uses, snapshotted at the start so a
	 * resolv.conf reread or a sip_dns_servers_set() override on a later
	 * resolution never moves an in flight query onto a server it did not
	 * ask, which would drop its answer.
	 */
	struct sip_txn_dst qserver[SIP_DNS_SERVER_MAX];
	unsigned int qservers;

	uint32_t owner;
	uint32_t ttl;
	uint16_t id;
	uint16_t qtype;
	uint16_t port;

	unsigned int index;
	unsigned int server;
	unsigned int attempt;
	unsigned int cname_depth;
	unsigned int glue_idx;			/* SRV target being resolved */
	unsigned int glue_scan;			/* first target still to look at */
	unsigned int glue_count;		/* SRV records in the walk */

	/*
	 * One bit per address family per SRV record, set when the question
	 * leaves. A family the additional section already carried is never
	 * asked, and a family whose question failed is never asked twice, so
	 * the walk always moves forward.
	 */
	uint8_t glue_asked[SIP_DNS_TARGET_MAX];

	/*
	 * The address stage of SIP_DNS_ADDR_SRV, one bit per family in the
	 * glue_asked encoding. A bit of addr_asked is set when the question of
	 * that family leaves, and the same bit of addr_absent when it gets a
	 * definitive negative. The SRV stage starts only when the two match.
	 * q->fail cannot decide it: it keeps one reason for both families.
	 */
	uint8_t addr_asked;
	uint8_t addr_absent;

	uint16_t gen;
	uint8_t kind;				/* enum sip_dns_kind */
	uint8_t stage;
	uint8_t fail;				/* enum sip_dns_status so far */
	bool in_use;
	bool truncated;
	bool ttl_set;				/* a real TTL was seen, 0 included */
	bool aaaa_done;				/* the A half of ADDR is done */
	bool literal;				/* the name was numeric */
	bool changed;				/* differs from the cached answer */
	bool cached;				/* served without a query */
	bool srv;				/* the targets came from SRV records */
	bool complete;				/* result ready, timer pending */
};

struct sip_dns_cache {
	char name[SIP_DNS_NAME_MAX];
	char service[SIP_DNS_SERVICE_MAX];	/* part of the identity, see "Cache" */
	struct sip_dns_target target[SIP_DNS_TARGET_MAX];
	unsigned int count;
	uint64_t expire_ms;
	uint32_t ttl;
	uint16_t port;
	uint8_t kind;
	bool srv;				/* the targets came from SRV records */
	bool valid;
};

/*
 * The transport the resolver core sends and waits on. The production
 * implementation drives uloop sockets and timers; a test replaces it with a
 * scripted socket and a virtual clock, which is how the state machine (retries,
 * server failover, partial stage failure, stale replies) is driven
 * deterministically, the same shape as struct sip_txn_env.
 *
 * send returns the byte count on success or a negative value, which the core
 * treats as a transport failure and fails over to the next server. release
 * frees any per query resource (the socket) and may be NULL. now_ms, timer_set
 * and timer_cancel are the clock and the one timer per query. A query whose
 * timer fires reaches the core through sip_dns_query_fire().
 */
struct sip_dns_io {
	int (*send)(struct sip_dns *r, struct sip_dns_query *q, const void *buf,
		    size_t len, const struct sip_txn_dst *server);
	void (*release)(struct sip_dns *r, struct sip_dns_query *q);
	uint64_t (*now_ms)(struct sip_dns *r);
	void (*timer_set)(struct sip_dns *r, struct sip_dns_query *q, unsigned int ms);
	void (*timer_cancel)(struct sip_dns *r, struct sip_dns_query *q);
	void *priv;
};

struct sip_dns {
	struct sip_dns_cfg cfg;
	struct sip_dns_cb cb;
	struct sip_dns_io io;
	void *priv;

	struct sip_txn_dst server[SIP_DNS_SERVER_MAX];
	unsigned int servers;
	bool servers_pinned;		/* sip_dns_servers_set(): do not reread */

	struct sip_dns_query query[SIP_DNS_QUERY_MAX];
	struct sip_dns_cache cache[SIP_DNS_CACHE_MAX];

	int prefer_family;		/* sip_dns_prefer_family_set() */
	uint32_t rand_state;
	uint8_t rx[SIP_DNS_PKT_MAX];

	struct sip_dns_stat stat;
};

void sip_dns_cfg_default(struct sip_dns_cfg *cfg);

void sip_dns_init(struct sip_dns *r, const struct sip_dns_cfg *cfg,
		  const struct sip_dns_cb *cb, void *priv);
void sip_dns_done(struct sip_dns *r);

/*
 * Read the servers from resolv_path. Returns how many were found. An empty or
 * missing file leaves one server, 127.0.0.1, which is what resolver(3) does and
 * what an OpenWrt unit with dnsmasq needs.
 */
unsigned int sip_dns_servers_load(struct sip_dns *r);

/* parse the nameserver lines of a resolv.conf text; used by the loader above */
unsigned int sip_dns_servers_parse(const char *text, struct sip_txn_dst *out,
				   unsigned int max);

/* replace the server list by hand, for a configured DNS server or a test */
void sip_dns_servers_set(struct sip_dns *r, const struct sip_txn_dst *srv, unsigned int count);
unsigned int sip_dns_server_count(const struct sip_dns *r);

/*
 * Start a resolution. port is the port every target carries when the answer
 * does not name one: an address answer of any kind, and the fallback of an
 * SRV lookup that finds no record. An SRV answer, of SIP_DNS_SRV or of the
 * SRV stage of SIP_DNS_ADDR_SRV, carries the ports of its records.
 * SIP_HANDLE_NONE means the pool is full or the name is unusable; done() does
 * not run in that case.
 *
 * A numeric name takes the direct path: no wire traffic, no cache, one target
 * from the timer ("Numeric targets" above).
 */
uint32_t sip_dns_resolve(struct sip_dns *r, const char *name, enum sip_dns_kind kind,
			 uint16_t port, uint32_t owner);

/*
 * The same resolution with an explicit SRV service label (RFC 3263 4.1), which
 * is what a per line transport needs: SIP_DNS_SERVICE_TCP for TCP and
 * SIP_DNS_SERVICE_TLS for TLS. NULL and "" both take cfg.service, and a
 * cfg.service that is itself NULL or "" takes SIP_DNS_SERVICE_DEFAULT, so
 * sip_dns_resolve() is exactly this call with a NULL label. Both take the
 * default because "" is the natural UDP mapping of a transport, and an SRV
 * question built from "" would be ".name", which the wire encoder refuses.
 *
 * The label joins the cache identity, so two transports that resolve one name
 * hold two entries and neither serves the other's targets. It is ignored for
 * every kind but SIP_DNS_SRV and SIP_DNS_ADDR_SRV, because only those two can
 * ask a question that carries a service.
 * SIP_HANDLE_NONE means the pool is full or the name is unusable, the same
 * synchronous refusals as sip_dns_resolve(); an over-long label or an
 * over-long combined SRV name is not one of them, because the wire encoder
 * bounds it and reports it through the normal done() path.
 */
uint32_t sip_dns_resolve_svc(struct sip_dns *r, const char *name, enum sip_dns_kind kind,
			     uint16_t port, uint32_t owner, const char *service);

void sip_dns_cancel(struct sip_dns *r, uint32_t query);
unsigned int sip_dns_owner_release(struct sip_dns *r, uint32_t owner);
unsigned int sip_dns_query_count(const struct sip_dns *r);

void sip_dns_cache_flush(struct sip_dns *r);

/*
 * Expire one cache entry, so the next lookup for its key reaches the wire.
 * The entry stays valid: the next answer still has a stored answer to compare
 * against, so result.changed keeps its meaning. The service label defaults
 * exactly as in sip_dns_resolve_svc(). A key the cache does not hold is a
 * no-op.
 */
void sip_dns_cache_expire(struct sip_dns *r, const char *name, enum sip_dns_kind kind,
			  uint16_t port, const char *service);

unsigned int sip_dns_cache_count(const struct sip_dns *r);

/*
 * The family the delivery order puts first, AF_INET6, AF_INET or 0 for the
 * order the answer already has. It is a live input and not a sip_dns_cfg field,
 * because the account layer takes it from the addresses netifd reports and
 * those move without a reload.
 *
 * The order is applied when done() runs, not when the answer is built, so a
 * preference that arrives while an answer waits on the delivery timer still
 * reaches that answer. It reorders the delivered copy alone: the cache keeps
 * the canonical order of sip_dns_canon_sort(), so a preference change never
 * makes an unchanged answer read as changed.
 */
void sip_dns_prefer_family_set(struct sip_dns *r, int family);
int sip_dns_prefer_family(const struct sip_dns *r);

/*
 * Turn the AAAA questions on and off after init. sip_dns_init() still takes the
 * first value from the config.
 *
 * A change flushes the cache, because an entry built under one question set
 * does not answer the other. A move to true also sends the SRV glue chase of
 * every query in flight back to its first record, because a record the chase
 * already passed owes an AAAA question now.
 *
 * A false value drops every AF_INET6 target of an answer that is already
 * built, the one in the cache write and the one waiting on the delivery timer.
 * A numeric target keeps its family whatever the value is ("Numeric targets"
 * above).
 */
void sip_dns_ipv6_set(struct sip_dns *r, bool ipv6);

const struct sip_dns_stat *sip_dns_stat(const struct sip_dns *r);

/* ------------------------------------------------- transport injection */

/*
 * Replace the uloop transport with a scripted one. Call it right after
 * sip_dns_init() and before the first resolve. These are the injected ops a
 * deterministic test drives, exactly as the transaction layer swaps its
 * struct sip_txn_env.
 */
void sip_dns_io_set(struct sip_dns *r, const struct sip_dns_io *io);

/*
 * Feed one received datagram to a query. The uloop transport calls the internal
 * form from its socket read; a test calls this with a datagram it built by
 * hand and the source address it wants the query to see. A datagram that does
 * not match the query id, the question or the current server is dropped.
 */
void sip_dns_input(struct sip_dns *r, uint32_t query, const void *buf, size_t len,
		   const struct sip_txn_dst *src);

/*
 * Run the timeout or completion of a query. The transport calls it when the
 * query timer fires: the uloop cb from its uloop_timeout, a test from its
 * virtual clock.
 */
void sip_dns_query_fire(struct sip_dns_query *q);

#endif
