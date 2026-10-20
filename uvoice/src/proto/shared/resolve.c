// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Asynchronous DNS. See shared/resolve.h for the contracts.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "posix.h"
#include "str.h"
#include "proto/proto_trace.h"
#include "proto/shared/resolve.h"

#define DNS_TRACE(r, fmt, ...)						\
	do {								\
		if ((r)->cfg.trace_host)				\
			PROTO_TRACE((r)->cfg.trace_host,		\
				    (r)->cfg.trace_dom, fmt,		\
				    ##__VA_ARGS__);			\
	} while (0)

enum dns_stage {
	DNS_STAGE_SRV = 0,	/* the SRV question itself */
	DNS_STAGE_ADDR,		/* A or AAAA of the requested name */
	DNS_STAGE_GLUE,		/* A of an SRV target the answer did not carry */
};

/*
 * The TTL of a numeric target. Nothing in it can expire, so the value only
 * decides how often the caller repeats a resolution that cannot change. One
 * hour is the usual TTL of an A record, which keeps the account layer on its
 * normal refresh schedule instead of at the SIP_DNS_TTL_MIN floor.
 */
#define SIP_DNS_TTL_LITERAL	3600

static void query_send(struct sip_dns_query *q);
static void query_finish(struct sip_dns_query *q, enum sip_dns_status st);
static void stage_failed(struct sip_dns_query *q, enum sip_dns_status fail);

/* ------------------------------------------------------------------ names */

static uint16_t be16_get(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t be32_get(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void be16_put(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

int dns_name_read(const void *buf, size_t len, size_t off, char *out, size_t olen,
		  size_t *next)
{
	const uint8_t *b = buf;
	size_t used = 0;
	size_t wire = 0;
	size_t limit = off;
	unsigned int jumps = 0;
	bool jumped = false;

	if (!out || olen < 2)
		return -1;

	out[0] = 0;

	while (off < len) {
		uint8_t l = b[off];
		size_t target;

		if ((l & 0xc0) == 0xc0) {
			if (off + 1 >= len)
				return -1;

			target = (size_t)((l & 0x3f) << 8) | b[off + 1];

			/*
			 * A pointer must point strictly backwards. Together
			 * with the 255 byte wire limit that makes every input
			 * terminate: a loop can only be built out of backward
			 * jumps that each consume at least one label, and the
			 * limit runs out.
			 */
			if (target >= limit || ++jumps > DNS_PTR_MAX)
				return -1;

			if (!jumped) {
				jumped = true;
				if (next)
					*next = off + 2;
			}

			limit = target;
			off = target;
			continue;
		}

		if (l & 0xc0)
			return -1;

		if (!l) {
			if (!jumped && next)
				*next = off + 1;
			out[used] = 0;
			return 0;
		}

		if (l > DNS_LABEL_MAX || off + 1 + l > len)
			return -1;

		/*
		 * The terminal root octet counts toward the 255 wire limit
		 * (RFC 1035 3.1). Without the +1 a name of 255 label octets plus
		 * the root would decode at 256.
		 */
		wire += (size_t)l + 1;
		if (wire + 1 > DNS_NAME_WIRE_MAX)
			return -1;

		if (used + (size_t)l + 2 > olen)
			return -1;

		/*
		 * A label byte of '.' or a control byte (NUL included) would make
		 * the joined presentation form ambiguous, so a later dns_name_eq
		 * could read two different wire names as one. Reject it; a SIP
		 * host label is letters, digits, hyphen and the SRV underscore.
		 */
		for (target = 0; target < l; target++) {
			uint8_t c = b[off + 1 + target];

			if (c == '.' || c < 0x20)
				return -1;
		}

		if (used)
			out[used++] = '.';

		memcpy(out + used, b + off + 1, l);
		used += l;
		off += (size_t)l + 1;
	}

	return -1;
}

int dns_name_write(void *buf, size_t blen, size_t off, const char *name)
{
	uint8_t *b = buf;
	size_t start = off;
	const char *p = name ? name : "";

	while (*p) {
		const char *dot = strchr(p, '.');
		size_t l = dot ? (size_t)(dot - p) : strlen(p);

		if (!l) {
			if (dot && !dot[1])
				break;

			return -1;
		}

		if (l > DNS_LABEL_MAX)
			return -1;
		if (off + 1 + l >= blen)
			return -1;
		/* +1 reserves the terminal root octet inside the 255 wire limit */
		if (off + 1 + l - start + 1 > DNS_NAME_WIRE_MAX)
			return -1;

		b[off++] = (uint8_t)l;
		memcpy(b + off, p, l);
		off += l;

		if (!dot)
			break;

		p = dot + 1;
	}

	if (off >= blen)
		return -1;

	b[off++] = 0;

	return (int)(off - start);
}

bool dns_name_eq(const char *a, const char *b)
{
	size_t la, lb;

	if (!a || !b)
		return false;

	la = strlen(a);
	lb = strlen(b);
	if (la && a[la - 1] == '.')
		la--;
	if (lb && b[lb - 1] == '.')
		lb--;

	if (la != lb)
		return false;

	return !strncasecmp(a, b, la);
}

/* ---------------------------------------------------------------- packets */

int dns_query_build(void *buf, size_t blen, uint16_t id, const char *name, uint16_t type,
		    bool edns)
{
	uint8_t *b = buf;
	size_t off = DNS_HDR_LEN;
	int n;

	if (blen < DNS_HDR_LEN)
		return -1;

	memset(b, 0, DNS_HDR_LEN);
	be16_put(b, id);
	be16_put(b + 2, DNS_F_RD);
	be16_put(b + 4, 1);
	be16_put(b + 10, edns ? 1 : 0);

	n = dns_name_write(b, blen, off, name);
	if (n < 0)
		return -1;

	off += (size_t)n;
	if (off + 4 > blen)
		return -1;

	be16_put(b + off, type);
	be16_put(b + off + 2, DNS_CLASS_IN);
	off += 4;

	if (!edns)
		return (int)off;

	/* OPT (RFC 6891): root name, class carries the UDP payload size */
	if (off + 11 > blen)
		return -1;

	b[off++] = 0;
	be16_put(b + off, DNS_T_OPT);
	be16_put(b + off + 2, SIP_DNS_EDNS_SIZE);
	memset(b + off + 4, 0, 4);
	be16_put(b + off + 8, 0);
	off += 10;

	return (int)off;
}

int dns_pkt_parse(struct dns_pkt *p, const void *buf, size_t len)
{
	const uint8_t *b = buf;
	size_t off;

	memset(p, 0, sizeof(*p));

	if (len < DNS_HDR_LEN || len > 65535)
		return -1;

	p->buf = b;
	p->len = len;
	p->id = be16_get(b);
	p->flags = be16_get(b + 2);
	p->count[DNS_SECTION_ANSWER] = be16_get(b + 6);
	p->count[DNS_SECTION_AUTHORITY] = be16_get(b + 8);
	p->count[DNS_SECTION_ADDITIONAL] = be16_get(b + 10);

	/* uvoice asks one question, so an answer to two is not an answer to us */
	if (be16_get(b + 4) > 1)
		return -1;

	off = DNS_HDR_LEN;
	if (be16_get(b + 4) == 1) {
		if (dns_name_read(b, len, off, p->qname, sizeof(p->qname), &off) < 0)
			return -1;
		if (off + 4 > len)
			return -1;

		p->qtype = be16_get(b + off);
		p->qclass = be16_get(b + off + 2);
		p->has_question = true;
		off += 4;
	}

	p->rr_off = off;

	return 0;
}

bool dns_rr_next(const struct dns_pkt *p, struct dns_rr_iter *it, struct dns_rr *rr)
{
	size_t off;

	while (it->section < DNS_SECTION_MAX && !it->left[it->section])
		it->section++;

	if (it->section >= DNS_SECTION_MAX)
		return false;

	if (dns_name_read(p->buf, p->len, it->off, rr->name, sizeof(rr->name), &off) < 0)
		return false;

	if (off + 10 > p->len)
		return false;

	rr->type = be16_get(p->buf + off);
	rr->rclass = be16_get(p->buf + off + 2);
	rr->ttl = be32_get(p->buf + off + 4);
	rr->rdlen = be16_get(p->buf + off + 8);
	rr->rdata = off + 10;
	rr->section = it->section;

	if (rr->rdata + rr->rdlen > p->len)
		return false;

	it->off = rr->rdata + rr->rdlen;
	it->left[it->section]--;

	return true;
}

bool dns_rr_first(const struct dns_pkt *p, struct dns_rr_iter *it, struct dns_rr *rr)
{
	unsigned int i;

	memset(it, 0, sizeof(*it));
	it->off = p->rr_off;
	for (i = 0; i < DNS_SECTION_MAX; i++)
		it->left[i] = p->count[i];

	return dns_rr_next(p, it, rr);
}

bool dns_rr_addr(const struct dns_pkt *p, const struct dns_rr *rr, uint16_t port,
		 struct sip_txn_dst *dst)
{
	struct sockaddr_in *v4 = (struct sockaddr_in *)&dst->sa;
	struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&dst->sa;

	memset(dst, 0, sizeof(*dst));

	if (rr->type == DNS_T_A && rr->rdlen == 4) {
		dst->sa.ss_family = AF_INET;
		dst->sa_len = sizeof(*v4);
		v4->sin_port = htons(port);
		memcpy(&v4->sin_addr, p->buf + rr->rdata, 4);
		return true;
	}

	if (rr->type == DNS_T_AAAA && rr->rdlen == 16) {
		dst->sa.ss_family = AF_INET6;
		dst->sa_len = sizeof(*v6);
		v6->sin6_port = htons(port);
		memcpy(&v6->sin6_addr, p->buf + rr->rdata, 16);
		return true;
	}

	return false;
}

bool dns_rr_name(const struct dns_pkt *p, const struct dns_rr *rr, char *out, size_t olen)
{
	size_t next = 0;

	if (!rr->rdlen)
		return false;

	if (dns_name_read(p->buf, p->len, rr->rdata, out, olen, &next) < 0)
		return false;

	/*
	 * The encoded name must finish inside this record's rdata. A name that
	 * runs past rdlen would read a target out of the bytes of the following
	 * record, which is how a malformed record forges an answer.
	 */
	return next <= rr->rdata + rr->rdlen;
}

bool dns_rr_srv(const struct dns_pkt *p, const struct dns_rr *rr, uint16_t *prio,
		uint16_t *weight, uint16_t *port, char *target, size_t tlen)
{
	size_t next = 0;

	if (rr->type != DNS_T_SRV || rr->rdlen < 7)
		return false;

	if (dns_name_read(p->buf, p->len, rr->rdata + 6, target, tlen, &next) < 0)
		return false;

	if (next > rr->rdata + rr->rdlen)
		return false;

	*prio = be16_get(p->buf + rr->rdata);
	*weight = be16_get(p->buf + rr->rdata + 2);
	*port = be16_get(p->buf + rr->rdata + 4);

	return true;
}

/* ---------------------------------------------------------------- sorting */

static int addr_cmp(const struct sip_dns_target *a, const struct sip_dns_target *b)
{
	const struct sockaddr_in *a4 = (const struct sockaddr_in *)&a->dst.sa;
	const struct sockaddr_in *b4 = (const struct sockaddr_in *)&b->dst.sa;
	const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)&a->dst.sa;
	const struct sockaddr_in6 *b6 = (const struct sockaddr_in6 *)&b->dst.sa;

	if (a->dst.sa.ss_family != b->dst.sa.ss_family)
		return a->dst.sa.ss_family < b->dst.sa.ss_family ? -1 : 1;

	if (a->dst.sa.ss_family == AF_INET6)
		return memcmp(&a6->sin6_addr, &b6->sin6_addr, sizeof(a6->sin6_addr));

	return memcmp(&a4->sin_addr, &b4->sin_addr, sizeof(a4->sin_addr));
}

static int canon_cmp(const struct sip_dns_target *a, const struct sip_dns_target *b)
{
	if (a->prio != b->prio)
		return a->prio < b->prio ? -1 : 1;
	if (a->weight != b->weight)
		return a->weight < b->weight ? -1 : 1;
	if (a->port != b->port)
		return a->port < b->port ? -1 : 1;

	return addr_cmp(a, b);
}

void sip_dns_canon_sort(struct sip_dns_target *t, unsigned int count)
{
	unsigned int i, j;

	for (i = 1; i < count; i++) {
		struct sip_dns_target key = t[i];

		for (j = i; j && canon_cmp(&key, &t[j - 1]) < 0; j--)
			t[j] = t[j - 1];

		t[j] = key;
	}
}

/*
 * One SRV record inside a priority: a contiguous run of targets that share a
 * group. The weight selection draws over these, not over the individual
 * addresses, so a record with two A records keeps its single weight.
 */
struct srv_run {
	unsigned int start;
	unsigned int len;
	uint32_t weight;
};

static int prio_group_cmp(const struct sip_dns_target *a, const struct sip_dns_target *b)
{
	if (a->prio != b->prio)
		return a->prio < b->prio ? -1 : 1;
	if (a->group != b->group)
		return a->group < b->group ? -1 : 1;

	return 0;
}

/* stable, so the address order inside one group is the order it arrived */
static void prio_group_sort(struct sip_dns_target *t, unsigned int count)
{
	unsigned int i, j;

	for (i = 1; i < count; i++) {
		struct sip_dns_target key = t[i];

		for (j = i; j && prio_group_cmp(&key, &t[j - 1]) < 0; j--)
			t[j] = t[j - 1];

		t[j] = key;
	}
}

static unsigned int run_collect(const struct sip_dns_target *t, unsigned int from,
				unsigned int to, struct srv_run *runs)
{
	unsigned int n = 0;
	unsigned int i = from;

	while (i < to) {
		unsigned int k = i;

		while (k < to && t[k].group == t[i].group)
			k++;

		runs[n].start = i;
		runs[n].len = k - i;
		runs[n].weight = t[i].weight;
		n++;
		i = k;
	}

	return n;
}

/*
 * RFC 2782 selection over the SRV records of one priority. A record is removed
 * from the pool by shifting, never by a swap, so the relative order of the
 * records not yet drawn is preserved. Weight 0 records are moved to the
 * front of the pool first, which is the placement the RFC asks for.
 */
static void prio_weight_select(struct sip_dns_target *t, unsigned int from, unsigned int to,
			       uint32_t (*rand32)(void *priv), void *priv)
{
	struct sip_dns_target scratch[SIP_DNS_TARGET_MAX];
	struct srv_run runs[SIP_DNS_TARGET_MAX];
	struct srv_run pool[SIP_DNS_TARGET_MAX];
	unsigned int n = to - from;
	unsigned int nruns;
	unsigned int npool = 0;
	unsigned int out = from;
	unsigned int i;

	if (n < 2)
		return;

	memcpy(scratch, t + from, n * sizeof(*t));
	nruns = run_collect(scratch, 0, n, runs);
	if (nruns < 2)
		return;

	for (i = 0; i < nruns; i++) {
		if (!runs[i].weight)
			pool[npool++] = runs[i];
	}
	for (i = 0; i < nruns; i++) {
		if (runs[i].weight)
			pool[npool++] = runs[i];
	}

	while (npool) {
		uint32_t total = 0;
		uint32_t pick;
		uint32_t run = 0;
		unsigned int sel = 0;
		unsigned int k;

		for (k = 0; k < npool; k++)
			total += pool[k].weight;

		pick = total ? rand32(priv) % (total + 1) : 0;
		for (k = 0; k < npool; k++) {
			run += pool[k].weight;
			if (run >= pick) {
				sel = k;
				break;
			}
		}

		memcpy(t + out, scratch + pool[sel].start, pool[sel].len * sizeof(*t));
		out += pool[sel].len;

		for (k = sel; k + 1 < npool; k++)
			pool[k] = pool[k + 1];
		npool--;
	}
}

void sip_dns_srv_sort(struct sip_dns_target *t, unsigned int count,
		      uint32_t (*rand32)(void *priv), void *priv)
{
	unsigned int i = 0;

	prio_group_sort(t, count);

	if (!rand32)
		return;

	while (i < count) {
		unsigned int j = i;

		while (j < count && t[j].prio == t[i].prio)
			j++;

		prio_weight_select(t, i, j, rand32, priv);
		i = j;
	}
}

/*
 * A stable partition by shifting: each target of the preferred family moves in
 * front of the targets of the other family that precede it, and no other pair
 * changes places.
 */
static void family_partition(struct sip_dns_target *t, unsigned int from,
			     unsigned int to, int family)
{
	unsigned int i;

	for (i = from + 1; i < to; i++) {
		struct sip_dns_target key;
		unsigned int j = i;

		if (t[i].dst.sa.ss_family != family)
			continue;

		key = t[i];
		while (j > from && t[j - 1].dst.sa.ss_family != family) {
			t[j] = t[j - 1];
			j--;
		}

		t[j] = key;
	}
}

void sip_dns_family_sort(struct sip_dns_target *t, unsigned int count,
			 int family)
{
	unsigned int i = 0;

	if (family != AF_INET && family != AF_INET6)
		return;

	while (i < count) {
		unsigned int j = i;

		while (j < count && !prio_group_cmp(&t[j], &t[i]))
			j++;

		family_partition(t, i, j, family);
		i = j;
	}
}

/* ------------------------------------------------------------------ names */

const char *sip_dns_kind_name(enum sip_dns_kind kind)
{
	switch (kind) {
	case SIP_DNS_ADDR:
		return "addr";
	case SIP_DNS_A:
		return "a";
	case SIP_DNS_AAAA:
		return "aaaa";
	case SIP_DNS_SRV:
		return "srv";
	case SIP_DNS_ADDR_SRV:
		return "addr_srv";
	case SIP_DNS_KIND_MAX:
		break;
	}

	return "?";
}

const char *sip_dns_status_name(enum sip_dns_status st)
{
	switch (st) {
	case SIP_DNS_OK:
		return "ok";
	case SIP_DNS_ERR_NXDOMAIN:
		return "nxdomain";
	case SIP_DNS_ERR_NO_SERVICE:
		return "no-service";
	case SIP_DNS_ERR_TIMEOUT:
		return "timeout";
	case SIP_DNS_ERR_SERVFAIL:
		return "servfail";
	case SIP_DNS_ERR_FORMAT:
		return "format";
	case SIP_DNS_ERR_NO_SERVER:
		return "no-server";
	case SIP_DNS_ERR_CANCELLED:
		return "cancelled";
	case SIP_DNS_ERR_INTERNAL:
		return "internal";
	}

	return "?";
}

/* ---------------------------------------------------------------- servers */

unsigned int sip_dns_servers_parse(const char *text, struct sip_txn_dst *out,
				   unsigned int max)
{
	unsigned int count = 0;
	const char *p = text;

	while (p && *p && count < max) {
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		char line[128];
		char *tok, *save;

		if (len >= sizeof(line))
			len = sizeof(line) - 1;

		memcpy(line, p, len);
		line[len] = 0;
		p = eol ? eol + 1 : NULL;

		tok = strtok_r(line, " \t\r", &save);
		if (!tok || strcmp(tok, "nameserver"))
			continue;

		tok = strtok_r(NULL, " \t\r", &save);
		if (!tok)
			continue;

		/*
		 * A scoped link local server ("fe80::1%br-lan") needs an
		 * interface index this layer does not carry, so it is skipped
		 * rather than sent to the wrong link.
		 */
		if (strchr(tok, '%'))
			continue;

		if (!sip_dst_set(&out[count], tok, SIP_DNS_PORT))
			continue;

		count++;
	}

	return count;
}

unsigned int sip_dns_servers_load(struct sip_dns *r)
{
	const char *path = r->cfg.resolv_path ? r->cfg.resolv_path : SIP_DNS_RESOLV_PATH;
	char text[1024];
	size_t used = 0;
	FILE *f;

	f = fopen(path, "re");
	if (f) {
		used = fread(text, 1, sizeof(text) - 1, f);
		fclose(f);
	}

	text[used] = 0;
	r->servers = sip_dns_servers_parse(text, r->server, SIP_DNS_SERVER_MAX);

	if (!r->servers && sip_dst_set(&r->server[0], "127.0.0.1", SIP_DNS_PORT))
		r->servers = 1;

	return r->servers;
}

void sip_dns_servers_set(struct sip_dns *r, const struct sip_txn_dst *srv, unsigned int count)
{
	unsigned int i;

	if (count > SIP_DNS_SERVER_MAX)
		count = SIP_DNS_SERVER_MAX;

	for (i = 0; i < count; i++)
		r->server[i] = srv[i];

	r->servers = count;

	/* an explicit list is authoritative; a later resolve does not reread
	 * resolv.conf over it. */
	r->servers_pinned = true;
}

unsigned int sip_dns_server_count(const struct sip_dns *r)
{
	return r->servers;
}

/* ------------------------------------------------------------------ clock */

/* the weight draw only; the query id never uses this (see dns_txid) */
static uint32_t dns_rand(struct sip_dns *r)
{
	uint32_t x = r->rand_state;

	if (r->cb.rand32)
		return r->cb.rand32(r->priv);

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	r->rand_state = x ? x : 0x2545f491u;

	return r->rand_state;
}

static uint32_t dns_rand_cb(void *priv)
{
	return dns_rand(priv);
}

bool sip_os_random(void *buf, size_t len)
{
	ssize_t n = getrandom(buf, len, GRND_NONBLOCK);
	int fd;

	if (n == (ssize_t)len)
		return true;

	/* urandom never blocks; it is the fallback when the getrandom pool is
	 * not yet ready, which only happens very early in boot. */
	fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		n = read(fd, buf, len);
		close(fd);
		if (n == (ssize_t)len)
			return true;
	}

	return false;
}

static bool id_in_flight(const struct sip_dns *r, uint16_t id)
{
	unsigned int i;

	for (i = 0; i < SIP_DNS_QUERY_MAX; i++) {
		const struct sip_dns_query *q = &r->query[i];

		if (q->in_use && !q->complete && q->id == id)
			return true;
	}

	return false;
}

/*
 * A DNS transaction id from OS randomness, not reused by another query in
 * flight. A predictable id lets a remote attacker forge an answer, so the
 * xorshift generator is a last resort only, used if both getrandom and urandom
 * fail.
 */
static uint16_t dns_txid(struct sip_dns *r)
{
	uint16_t id = 0;
	unsigned int tries;

	for (tries = 0; tries < 8; tries++) {
		if (!sip_os_random(&id, sizeof(id)))
			id = (uint16_t)dns_rand(r);
		if (!id_in_flight(r, id))
			break;
	}

	return id;
}

/* ------------------------------------------------------------------ cache */

/*
 * The SRV service label of a query, part of its cache identity. NULL and ""
 * both take the default, because "" is the natural UDP mapping of a per line
 * transport, and an SRV question built from "" would be ".name", which
 * dns_name_write() refuses. A kind that never asks SRV carries no label.
 */
static const char *service_label_get(const struct sip_dns *r,
				     const char *service,
				     enum sip_dns_kind kind)
{
	if (kind != SIP_DNS_SRV && kind != SIP_DNS_ADDR_SRV)
		return "";
	if (service && *service)
		return service;
	if (r->cfg.service && *r->cfg.service)
		return r->cfg.service;

	return SIP_DNS_SERVICE_DEFAULT;
}

static struct sip_dns_cache *cache_find(struct sip_dns *r, const char *name,
					enum sip_dns_kind kind, uint16_t port,
					const char *service)
{
	unsigned int i;

	for (i = 0; i < SIP_DNS_CACHE_MAX; i++) {
		struct sip_dns_cache *c = &r->cache[i];

		if (!c->valid || c->kind != kind || c->port != port)
			continue;
		if (!dns_name_eq(c->service, service))
			continue;
		if (!dns_name_eq(c->name, name))
			continue;

		return c;
	}

	return NULL;
}

/*
 * A free slot first, and only then the entry nearest its end. An expired
 * entry still holds the answer the next one for its key is compared against
 * (sip_dns_cache_expire), so it must not go while a free slot exists.
 */
static struct sip_dns_cache *cache_slot(struct sip_dns *r)
{
	struct sip_dns_cache *oldest = &r->cache[0];
	unsigned int i;

	for (i = 0; i < SIP_DNS_CACHE_MAX; i++) {
		struct sip_dns_cache *c = &r->cache[i];

		if (!c->valid)
			return c;

		if (c->expire_ms < oldest->expire_ms)
			oldest = c;
	}

	return oldest;
}

static bool target_set_eq(const struct sip_dns_target *a, unsigned int an,
			  const struct sip_dns_target *b, unsigned int bn)
{
	unsigned int i;

	if (an != bn)
		return false;

	for (i = 0; i < an; i++) {
		if (a[i].port != b[i].port || a[i].prio != b[i].prio ||
		    a[i].weight != b[i].weight)
			return false;
		if (!sip_dst_eq(&a[i].dst, &b[i].dst))
			return false;
	}

	return true;
}

void sip_dns_cache_flush(struct sip_dns *r)
{
	unsigned int i;

	for (i = 0; i < SIP_DNS_CACHE_MAX; i++)
		r->cache[i].valid = false;
}

/*
 * Only the remaining TTL goes: the entry keeps valid and its targets, because
 * query_finish() compares the next answer against them and a cleared entry
 * would make every address move after a failure read as a first answer.
 */
void sip_dns_cache_expire(struct sip_dns *r, const char *name,
			  enum sip_dns_kind kind, uint16_t port,
			  const char *service)
{
	struct sip_dns_cache *c;
	const char *svc;

	if (!name || !*name || kind >= SIP_DNS_KIND_MAX)
		return;

	svc = service_label_get(r, service, kind);
	c = cache_find(r, name, kind, port, svc);
	if (c)
		c->expire_ms = 0;
}

unsigned int sip_dns_cache_count(const struct sip_dns *r)
{
	unsigned int count = 0;
	unsigned int i;

	for (i = 0; i < SIP_DNS_CACHE_MAX; i++) {
		if (r->cache[i].valid)
			count++;
	}

	return count;
}

/* ---------------------------------------------------------------- queries */

static uint32_t query_handle(const struct sip_dns_query *q)
{
	if (!q || !q->in_use)
		return SIP_HANDLE_NONE;

	return sip_handle_make(q->index, q->gen);
}

static struct sip_dns_query *query_get(struct sip_dns *r, uint32_t handle)
{
	struct sip_dns_query *q;
	unsigned int idx;

	if (handle == SIP_HANDLE_NONE)
		return NULL;

	idx = sip_handle_index(handle);
	if (idx >= SIP_DNS_QUERY_MAX)
		return NULL;

	q = &r->query[idx];
	if (!q->in_use || q->gen != sip_handle_gen(handle))
		return NULL;

	return q;
}

static void query_free(struct sip_dns_query *q)
{
	struct sip_dns *r = q->r;

	r->io.timer_cancel(r, q);
	if (r->io.release)
		r->io.release(r, q);

	q->in_use = false;
	q->gen++;
	if (!q->gen)
		q->gen = 1;
}

unsigned int sip_dns_query_count(const struct sip_dns *r)
{
	unsigned int count = 0;
	unsigned int i;

	for (i = 0; i < SIP_DNS_QUERY_MAX; i++) {
		if (r->query[i].in_use)
			count++;
	}

	return count;
}

/* ----------------------------------------------------- uloop transport */

static void query_deliver_in(struct sip_dns_query *q, const void *buf, size_t len,
			     const struct sip_txn_dst *src);

static void io_uloop_release(struct sip_dns *r, struct sip_dns_query *q)
{
	if (q->ufd.registered)
		uloop_fd_delete(&q->ufd);

	if (q->ufd.fd >= 0)
		close(q->ufd.fd);

	q->ufd.fd = -1;
}

/*
 * One datagram per readiness. The query talks to one server at a time and
 * expects one answer, so a burst read would only pick up duplicates, and a
 * second answer that arrives while the first is being processed can reopen this
 * very socket (a family change on failover). Reading one keeps that off the
 * re entrant path; the socket stays readable and uloop calls again.
 */
static void io_uloop_recv(struct uloop_fd *ufd, unsigned int events)
{
	struct sip_dns_query *q = container_of(ufd, struct sip_dns_query, ufd);
	struct sip_dns *r = q->r;
	struct sip_txn_dst src;
	socklen_t slen;
	ssize_t n;

	memset(&src, 0, sizeof(src));
	slen = sizeof(src.sa);
	do {
		n = recvfrom(ufd->fd, r->rx, sizeof(r->rx), MSG_DONTWAIT | MSG_TRUNC,
			     (struct sockaddr *)&src.sa, &slen);
	} while (n < 0 && errno == EINTR);

	if (n < 0)
		return;

	src.sa_len = slen;

	if ((size_t)n > sizeof(r->rx)) {
		r->stat.dropped++;
		return;
	}

	query_deliver_in(q, r->rx, (size_t)n, &src);
}

static int io_uloop_send(struct sip_dns *r, struct sip_dns_query *q, const void *buf,
			 size_t len, const struct sip_txn_dst *server)
{
	int family = server->sa.ss_family ? server->sa.ss_family : AF_INET;
	ssize_t n;

	if (q->ufd.fd >= 0 && q->fd_family != family)
		io_uloop_release(r, q);

	if (q->ufd.fd < 0) {
		int fd = socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
				IPPROTO_UDP);

		if (fd < 0)
			return -1;

		q->ufd.fd = fd;
		q->ufd.cb = io_uloop_recv;
		q->fd_family = family;
		if (uloop_fd_add(&q->ufd, ULOOP_READ) < 0) {
			close(fd);
			q->ufd.fd = -1;
			return -1;
		}
	}

	do {
		n = sendto(q->ufd.fd, buf, len, MSG_DONTWAIT,
			   (const struct sockaddr *)&server->sa, server->sa_len);
	} while (n < 0 && errno == EINTR);

	return n < 0 ? -1 : (int)n;
}

static void io_uloop_timer_cb(struct uloop_timeout *t)
{
	struct sip_dns_query *q = container_of(t, struct sip_dns_query, timer);

	sip_dns_query_fire(q);
}

static void io_uloop_timer_set(struct sip_dns *r, struct sip_dns_query *q, unsigned int ms)
{
	q->timer.cb = io_uloop_timer_cb;
	uloop_timeout_set(&q->timer, (int)ms);
}

static void io_uloop_timer_cancel(struct sip_dns *r, struct sip_dns_query *q)
{
	uloop_timeout_cancel(&q->timer);
}

static uint64_t io_uloop_now(struct sip_dns *r)
{
	return mono_ms();
}

static const struct sip_dns_io io_uloop = {
	.send = io_uloop_send,
	.release = io_uloop_release,
	.now_ms = io_uloop_now,
	.timer_set = io_uloop_timer_set,
	.timer_cancel = io_uloop_timer_cancel,
};

void sip_dns_io_set(struct sip_dns *r, const struct sip_dns_io *io)
{
	r->io = *io;
}

/* ------------------------------------------------------------- completion */

/*
 * Drop every AF_INET6 target. cfg.ipv6 is the operator gate, so an answer built
 * while the AAAA questions were on must not reach a caller once they are off.
 */
static void target_v6_drop(struct sip_dns_query *q)
{
	unsigned int keep = 0;
	unsigned int i;

	for (i = 0; i < q->count; i++) {
		if (q->target[i].dst.sa.ss_family == AF_INET6)
			continue;

		q->target[keep++] = q->target[i];
	}

	q->count = keep;
}

/*
 * Both live inputs are read here and not where the answer is built, because
 * either can move inside the zero millisecond delivery window: the gate through
 * sip_dns_ipv6_set(), the family order through sip_dns_prefer_family_set(). A
 * numeric target keeps its family whatever the gate holds, because the account
 * layer gates a v6 literal, not the resolver.
 */
static void result_deliver(struct sip_dns_query *q)
{
	struct sip_dns *r = q->r;
	struct sip_dns_result res;
	uint32_t handle = query_handle(q);
	uint32_t owner = q->owner;
	unsigned int i;

	if (!r->cfg.ipv6 && !q->literal)
		target_v6_drop(q);

	/* an answer the drop emptied fails the way any empty answer fails */
	if (q->fail == SIP_DNS_OK && !q->count) {
		q->fail = SIP_DNS_ERR_FORMAT;
		r->stat.failures++;
	}

	sip_dns_family_sort(q->target, q->count, r->prefer_family);

	memset(&res, 0, sizeof(res));
	res.status = q->fail;
	res.ttl = q->ttl;
	res.truncated = q->truncated;
	res.changed = q->changed;
	res.cached = q->cached;
	res.srv = q->srv;
	res.count = q->count;
	for (i = 0; i < q->count; i++)
		res.target[i] = q->target[i];

	query_free(q);

	if (r->cb.done)
		r->cb.done(r, handle, owner, &res, r->priv);
}

static void deliver_arm(struct sip_dns_query *q)
{
	struct sip_dns *r = q->r;

	q->complete = true;
	r->io.timer_set(r, q, 0);
}

static void ttl_clamp(struct sip_dns_query *q)
{
	const struct sip_dns_cfg *cfg = &q->r->cfg;

	if (q->ttl < cfg->ttl_min)
		q->ttl = cfg->ttl_min;
	if (q->ttl > cfg->ttl_max)
		q->ttl = cfg->ttl_max;
}

/*
 * Everything a finished query has to do: drop the targets that never got an
 * address, clamp the TTL, compare against the cache, update it, and hand the
 * caller a copy in RFC 2782 order. The delivery itself runs from the zero
 * millisecond timer, so no callback ever runs inside sip_dns_resolve().
 *
 * changed is true only when a previous answer existed and differed. A first
 * answer changes nothing: there was no binding built on the old one.
 */
static void query_finish(struct sip_dns_query *q, enum sip_dns_status st)
{
	struct sip_dns *r = q->r;
	struct sip_dns_cache *c;
	uint64_t now = r->io.now_ms(r);
	unsigned int keep = 0;
	unsigned int i;

	for (i = 0; i < q->count; i++) {
		if (!q->target[i].dst.sa_len)
			continue;

		q->target[keep++] = q->target[i];
	}
	q->count = keep;

	/*
	 * A query that took AAAA glue before the operator turned IPv6 off must
	 * not put a v6 target back into the flushed cache.
	 */
	if (!r->cfg.ipv6)
		target_v6_drop(q);

	if (st == SIP_DNS_OK && !q->count)
		st = SIP_DNS_ERR_FORMAT;

	q->fail = (uint8_t)st;

	if (st != SIP_DNS_OK) {
		q->count = 0;
		r->stat.failures++;
		deliver_arm(q);
		return;
	}

	ttl_clamp(q);

	/* answer_srv() sets the count when the zone holds SRV records and
	 * srv_fallback() puts it back to 0, so it says which of the two built
	 * these targets. It stays out of target_set_eq(), so it never makes an
	 * answer read as changed. */
	q->srv = q->glue_count > 0;

	sip_dns_canon_sort(q->target, q->count);

	c = cache_find(r, q->name, q->kind, q->port, q->service);
	if (c && !target_set_eq(c->target, c->count, q->target, q->count)) {
		q->changed = true;
		r->stat.changed++;
	}

	if (!c)
		c = cache_slot(r);

	memset(c, 0, sizeof(*c));
	str_copy(c->name, sizeof(c->name), q->name);
	str_copy(c->service, sizeof(c->service), q->service);
	c->kind = q->kind;
	c->port = q->port;
	c->srv = q->srv;
	c->ttl = q->ttl;
	c->expire_ms = now + (uint64_t)q->ttl * 1000;
	c->count = q->count;
	for (i = 0; i < q->count; i++)
		c->target[i] = q->target[i];
	c->valid = true;

	sip_dns_srv_sort(q->target, q->count, dns_rand_cb, r);
	deliver_arm(q);
}

/* --------------------------------------------------------------- answers */

static bool rr_wanted(const struct dns_rr *rr, uint16_t type, const char *name)
{
	return rr->rclass == DNS_CLASS_IN && rr->type == type && dns_name_eq(rr->name, name);
}

static bool type_present(const struct dns_pkt *p, uint16_t type, const char *name)
{
	struct dns_rr_iter it;
	struct dns_rr rr;
	bool more;

	for (more = dns_rr_first(p, &it, &rr); more; more = dns_rr_next(p, &it, &rr)) {
		if (rr.section != DNS_SECTION_ANSWER)
			break;
		if (rr_wanted(&rr, type, name))
			return true;
	}

	return false;
}

static bool cname_target(const struct dns_pkt *p, const char *name, char *out, size_t olen,
			 uint32_t *ttl)
{
	struct dns_rr_iter it;
	struct dns_rr rr;
	bool more;

	for (more = dns_rr_first(p, &it, &rr); more; more = dns_rr_next(p, &it, &rr)) {
		if (rr.section != DNS_SECTION_ANSWER)
			break;
		if (!rr_wanted(&rr, DNS_T_CNAME, name))
			continue;

		if (!dns_rr_name(p, &rr, out, olen))
			return false;

		*ttl = rr.ttl;
		return true;
	}

	return false;
}

static void ttl_take(struct sip_dns_query *q, uint32_t ttl)
{
	/*
	 * The initialization is a separate flag, not "q->ttl == 0", so a real
	 * TTL of 0 (do not cache) is kept as the minimum instead of being
	 * treated as unset and overwritten by a later larger TTL.
	 */
	if (!q->ttl_set || ttl < q->ttl) {
		q->ttl = ttl;
		q->ttl_set = true;
	}
}

/*
 * Follow the CNAMEs of name inside p until a record of type shows up. advanced
 * reports that the name moved, which is what tells the caller to ask again for
 * the new name instead of giving up. Every CNAME hop the chase accepts bounds
 * the answer TTL: a short lived CNAME must re-resolve on its own schedule.
 */
static bool cname_chase(struct sip_dns_query *q, const struct dns_pkt *p, uint16_t type,
			char *name, size_t nlen, bool *advanced)
{
	char next[SIP_DNS_NAME_MAX];
	uint32_t cttl = 0;
	unsigned int hop;

	*advanced = false;

	for (hop = 0; hop <= SIP_DNS_CNAME_MAX; hop++) {
		if (type_present(p, type, name))
			return true;

		if (!cname_target(p, name, next, sizeof(next), &cttl))
			return false;

		if (q->cname_depth >= SIP_DNS_CNAME_MAX)
			return false;

		ttl_take(q, cttl);
		q->cname_depth++;
		str_copy(name, nlen, next);
		*advanced = true;
	}

	return false;
}

/*
 * Take every address of name into the target set. slot is the SRV target the
 * first address belongs to, or SIP_DNS_TARGET_MAX to append everything; a
 * second address of one SRV target becomes another target with the same
 * priority, weight, port and group, so the RFC 2782 draw counts that record
 * once and not once per address. any_section lets an SRV answer read the
 * glue out of the additional section.
 */
static unsigned int addr_collect(struct sip_dns_query *q, const struct dns_pkt *p,
				 const char *name, uint16_t port, uint16_t prio,
				 uint16_t weight, uint16_t group, bool any_section,
				 unsigned int slot)
{
	struct dns_rr_iter it;
	struct dns_rr rr;
	unsigned int added = 0;
	bool more;

	for (more = dns_rr_first(p, &it, &rr); more; more = dns_rr_next(p, &it, &rr)) {
		struct sip_dns_target *t;
		struct sip_txn_dst dst;

		if (!any_section && rr.section != DNS_SECTION_ANSWER)
			break;
		if (rr.rclass != DNS_CLASS_IN || !dns_name_eq(rr.name, name))
			continue;
		if (rr.type != DNS_T_A && rr.type != DNS_T_AAAA)
			continue;
		if (rr.type == DNS_T_AAAA && !q->r->cfg.ipv6)
			continue;
		if (!dns_rr_addr(p, &rr, port, &dst))
			continue;

		if (!added && slot < SIP_DNS_TARGET_MAX) {
			t = &q->target[slot];
		} else {
			if (q->count >= SIP_DNS_TARGET_MAX)
				break;

			t = &q->target[q->count];
			str_copy(q->srv_name[q->count], SIP_DNS_NAME_MAX, name);
			q->count++;
		}

		memset(t, 0, sizeof(*t));
		t->dst = dst;
		t->port = port;
		t->prio = prio;
		t->weight = weight;
		t->group = group;
		sip_dst_host(&dst, t->host, sizeof(t->host));
		ttl_take(q, rr.ttl);
		added++;
	}

	return added;
}

static unsigned int srv_collect(struct sip_dns_query *q, const struct dns_pkt *p,
				const char *name, bool *no_service)
{
	struct dns_rr_iter it;
	struct dns_rr rr;
	unsigned int added = 0;
	bool more;

	*no_service = false;

	for (more = dns_rr_first(p, &it, &rr); more; more = dns_rr_next(p, &it, &rr)) {
		char target[SIP_DNS_NAME_MAX];
		uint16_t prio, weight, port;
		struct sip_dns_target *t;

		if (rr.section != DNS_SECTION_ANSWER)
			break;
		if (!rr_wanted(&rr, DNS_T_SRV, name))
			continue;
		if (!dns_rr_srv(p, &rr, &prio, &weight, &port, target, sizeof(target)))
			continue;

		/* RFC 2782: a single "." target means the service is refused */
		if (!target[0]) {
			*no_service = true;
			return 0;
		}

		if (q->count >= SIP_DNS_TARGET_MAX)
			break;

		t = &q->target[q->count];
		memset(t, 0, sizeof(*t));
		t->port = port;
		t->prio = prio;
		t->weight = weight;
		t->group = (uint16_t)q->count;		/* the SRV record ordinal */
		str_copy(q->srv_name[q->count], SIP_DNS_NAME_MAX, target);
		q->count++;
		ttl_take(q, rr.ttl);
		added++;
	}

	return added;
}

/*
 * One bit per address family: the two glue questions of one SRV record, and
 * the two address questions of SIP_DNS_ADDR_SRV
 */
#define GLUE_A		(1u << 0)
#define GLUE_AAAA	(1u << 1)

/* the families a record already has, from its glue and from the chase */
static uint8_t glue_have(const struct sip_dns_query *q, unsigned int idx)
{
	uint16_t group = q->target[idx].group;
	uint8_t have = 0;
	unsigned int i;

	for (i = 0; i < q->count; i++) {
		const struct sip_txn_dst *dst = &q->target[i].dst;

		if (q->target[i].group != group || !dst->sa_len)
			continue;

		have |= dst->sa.ss_family == AF_INET6 ? GLUE_AAAA : GLUE_A;
	}

	return have;
}

/*
 * The families of one SRV record that are still worth a question: A always,
 * AAAA when cfg.ipv6 is set, minus what the record already has and minus what
 * this query already asked for it.
 */
static uint8_t glue_owed(const struct sip_dns_query *q, unsigned int idx)
{
	uint8_t want = q->r->cfg.ipv6 ? (GLUE_A | GLUE_AAAA) : GLUE_A;

	return want & ~glue_have(q, idx) & ~q->glue_asked[idx];
}

/*
 * The next SRV record that still owes a question, or SIP_DNS_TARGET_MAX. Only
 * the records the SRV answer named are walked: an address the chase appended
 * belongs to a record that is already covered. The scan cursor moves past a
 * record that owes nothing, and glue_asked retires a question that failed, so
 * the walk always moves forward.
 */
static unsigned int glue_next(const struct sip_dns_query *q)
{
	unsigned int i;

	for (i = q->glue_scan; i < q->glue_count; i++) {
		if (glue_owed(q, i))
			return i;
	}

	return SIP_DNS_TARGET_MAX;
}

/*
 * Did any target of the walk get an address. q->count is no answer to that: an
 * SRV answer sets it to the number of records, and a record keeps its slot
 * until a question fills it in.
 */
static bool glue_any_addressed(const struct sip_dns_query *q)
{
	unsigned int i;

	for (i = 0; i < q->count; i++) {
		if (q->target[i].dst.sa_len)
			return true;
	}

	return false;
}

static void question_set(struct sip_dns_query *q, const char *name, uint16_t type)
{
	str_copy(q->qname, sizeof(q->qname), name);
	q->qtype = type;
	q->server = 0;
	q->attempt = 0;
}

/* a truncated question would ask for a different name */
static bool srv_question_set(struct sip_dns_query *q)
{
	char question[SIP_DNS_NAME_MAX];
	int n;

	n = snprintf(question, sizeof(question), "%s.%s", q->service, q->name);
	if (n < 0 || (size_t)n >= sizeof(question))
		return false;

	question_set(q, question, DNS_T_SRV);
	return true;
}

static uint8_t addr_bit(uint16_t qtype)
{
	return qtype == DNS_T_AAAA ? GLUE_AAAA : GLUE_A;
}

/*
 * Did the whole answer section parse. dns_rr_next() ends a walk on a malformed
 * record the way it ends one on the last record, and a record that did not
 * parse can be the address.
 */
static bool answer_section_whole(const struct dns_pkt *p)
{
	struct dns_rr_iter it;
	struct dns_rr rr;
	bool more = dns_rr_first(p, &it, &rr);

	while (more && rr.section == DNS_SECTION_ANSWER)
		more = dns_rr_next(p, &it, &rr);

	return !it.left[DNS_SECTION_ANSWER];
}

/*
 * A NOERROR answer that proves name has no record of the asked type: complete,
 * and with no CNAME left to follow. A CNAME that cname_chase() did not take is
 * a chain at its hop limit or a record that did not parse.
 */
static bool addr_negative(const struct dns_pkt *p, const char *name)
{
	if (!answer_section_whole(p))
		return false;

	return !type_present(p, DNS_T_CNAME, name);
}

/* ------------------------------------------------------------- transitions */

/*
 * The follow up lookup for an SRV target, A first and then AAAA. With cfg.ipv6
 * set the chase asks both families for every target whose glue carried only one
 * of them, so the registration layer can walk from a dead v6 address of a host
 * to the v4 address of the same host without leaving the RFC 2782 order. With
 * cfg.ipv6 clear it asks A alone. A question that fails after the other family
 * answered leaves that answer standing.
 *
 * A walk that ends with no address at all reports the reason the answers and
 * the failures of its own questions left behind, so an operator reads "the
 * zone says the target does not exist" and not the empty answer format error.
 */
static void stage_glue_run(struct sip_dns_query *q)
{
	unsigned int idx = glue_next(q);
	bool ask_a;

	if (idx >= SIP_DNS_TARGET_MAX) {
		query_finish(q, glue_any_addressed(q) ? SIP_DNS_OK :
				(enum sip_dns_status)q->fail);
		return;
	}

	ask_a = (glue_owed(q, idx) & GLUE_A) != 0;
	q->stage = DNS_STAGE_GLUE;
	q->glue_idx = idx;
	q->glue_asked[idx] |= ask_a ? GLUE_A : GLUE_AAAA;
	q->cname_depth = 0;
	question_set(q, q->srv_name[idx], ask_a ? DNS_T_A : DNS_T_AAAA);
	query_send(q);
}

static void glue_advance(struct sip_dns_query *q)
{
	if (!glue_owed(q, q->glue_idx))
		q->glue_scan = q->glue_idx + 1;

	stage_glue_run(q);
}

static uint16_t addr_next_type(const struct sip_dns_query *q)
{
	if (!q->r->cfg.ipv6 || q->aaaa_done)
		return 0;
	if (q->kind == SIP_DNS_A || q->kind == SIP_DNS_AAAA)
		return 0;
	if (q->qtype != DNS_T_A)
		return 0;

	return DNS_T_AAAA;
}

/*
 * Any truncated answer of the address stage blocks SRV, whatever its rcode:
 * the stage runs first, so q->truncated describes it alone here.
 */
static bool addr_srv_owed(const struct sip_dns_query *q)
{
	if (q->kind != SIP_DNS_ADDR_SRV || q->count || q->truncated)
		return false;

	return q->addr_asked && q->addr_absent == q->addr_asked;
}

/*
 * The SRV stage of SIP_DNS_ADDR_SRV. The reason starts again, as in front of
 * the glue walk, so it describes the SRV stage alone. The TTL is kept, as
 * srv_fallback() keeps it, so a CNAME hop of the address stage still bounds it.
 */
static void stage_srv_run(struct sip_dns_query *q)
{
	q->stage = DNS_STAGE_SRV;
	q->cname_depth = 0;
	q->count = 0;
	q->glue_scan = 0;
	q->glue_count = 0;
	q->fail = SIP_DNS_ERR_TIMEOUT;

	DNS_TRACE(q->r, "dns: %s holds no address, asking SRV", q->name);

	if (!srv_question_set(q)) {
		query_finish(q, SIP_DNS_ERR_INTERNAL);
		return;
	}

	query_send(q);
}

static void stage_addr_done(struct sip_dns_query *q)
{
	uint16_t next = addr_next_type(q);

	if (next) {
		q->aaaa_done = true;
		q->cname_depth = 0;
		question_set(q, q->name, next);
		query_send(q);
		return;
	}

	if (q->count) {
		query_finish(q, SIP_DNS_OK);
		return;
	}

	if (addr_srv_owed(q)) {
		stage_srv_run(q);
		return;
	}

	query_finish(q, (enum sip_dns_status)q->fail);
}

/* SIP_DNS_ADDR_SRV reaches the SRV stage only after its address failed */
static void srv_fallback(struct sip_dns_query *q)
{
	if (!q->r->cfg.srv_fallback || q->kind == SIP_DNS_ADDR_SRV) {
		query_finish(q, (enum sip_dns_status)q->fail);
		return;
	}

	q->stage = DNS_STAGE_ADDR;
	q->cname_depth = 0;
	q->count = 0;
	q->glue_scan = 0;
	q->glue_count = 0;
	question_set(q, q->name, DNS_T_A);
	query_send(q);
}

/* ---------------------------------------------------------------- receive */

static void answer_srv(struct sip_dns_query *q, const struct dns_pkt *p)
{
	char name[SIP_DNS_NAME_MAX];
	bool advanced = false;
	bool no_service = false;
	unsigned int i;

	str_copy(name, sizeof(name), q->qname);

	if (!cname_chase(q, p, DNS_T_SRV, name, sizeof(name), &advanced)) {
		if (advanced) {
			question_set(q, name, DNS_T_SRV);
			query_send(q);
			return;
		}

		srv_fallback(q);
		return;
	}

	if (!srv_collect(q, p, name, &no_service)) {
		if (no_service) {
			query_finish(q, SIP_DNS_ERR_NO_SERVICE);
			return;
		}

		srv_fallback(q);
		return;
	}

	/*
	 * The bound is taken before the walk: addr_collect() appends a target
	 * for a second address, and those already carry their address. It is
	 * also the number of records the glue walk covers.
	 */
	q->glue_count = q->count;
	for (i = 0; i < q->glue_count; i++)
		addr_collect(q, p, q->srv_name[i], q->target[i].port, q->target[i].prio,
			     q->target[i].weight, q->target[i].group, true, i);

	/*
	 * The reason starts again for the glue walk. answer_process() reads
	 * this NOERROR answer as "records arrived, none usable" and writes a
	 * format error, and fail_note() never goes back to a timeout, so
	 * without the reset a walk where no target answers reports the wrong
	 * reason.
	 */
	q->fail = SIP_DNS_ERR_TIMEOUT;
	stage_glue_run(q);
}

static void answer_addr(struct sip_dns_query *q, const struct dns_pkt *p)
{
	char name[SIP_DNS_NAME_MAX];
	bool advanced = false;

	str_copy(name, sizeof(name), q->qname);

	if (!cname_chase(q, p, q->qtype, name, sizeof(name), &advanced)) {
		if (advanced) {
			question_set(q, name, q->qtype);
			query_send(q);
			return;
		}

		if (q->stage == DNS_STAGE_GLUE) {
			glue_advance(q);
			return;
		}

		if (addr_negative(p, name))
			q->addr_absent |= addr_bit(q->qtype);

		stage_addr_done(q);
		return;
	}

	if (q->stage == DNS_STAGE_GLUE) {
		const struct sip_dns_target *g = &q->target[q->glue_idx];
		unsigned int slot = q->glue_idx;

		/*
		 * The record owns its own slot until it holds an address. The
		 * second family appends, so it does not overwrite the first.
		 */
		if (g->dst.sa_len)
			slot = SIP_DNS_TARGET_MAX;

		addr_collect(q, p, name, g->port, g->prio, g->weight, g->group,
			     false, slot);
		glue_advance(q);
		return;
	}

	addr_collect(q, p, name, q->port, 0, 0, 0, false, SIP_DNS_TARGET_MAX);
	stage_addr_done(q);
}

static void fail_note(struct sip_dns_query *q, enum sip_dns_status fail)
{
	/*
	 * Keep the first specific reason. TIMEOUT is the placeholder every query
	 * starts with, so a later, more informative failure (NXDOMAIN, a format
	 * error) replaces it, but a real reason is never downgraded back to
	 * TIMEOUT.
	 */
	if (q->fail == SIP_DNS_ERR_TIMEOUT || fail != SIP_DNS_ERR_TIMEOUT)
		q->fail = (uint8_t)fail;
}

/*
 * A question that ran out of servers is a terminal failure of that question,
 * not of the whole resolution. It is routed back into the stage machine so the
 * targets already gathered survive: an AAAA that times out after A answered
 * keeps the A targets, and an SRV glue lookup that fails drops only its own
 * target. Only a stage with nothing left to try reaches query_finish.
 */
static void stage_failed(struct sip_dns_query *q, enum sip_dns_status fail)
{
	fail_note(q, fail);

	switch (q->stage) {
	case DNS_STAGE_SRV:
		srv_fallback(q);
		return;
	case DNS_STAGE_GLUE:
		glue_advance(q);
		return;
	default:
		stage_addr_done(q);
		return;
	}
}

static void server_next(struct sip_dns_query *q, enum sip_dns_status fail)
{
	q->attempt = 0;
	q->server++;

	if (q->server >= q->qservers) {
		stage_failed(q, fail);
		return;
	}

	query_send(q);
}

static void answer_process(struct sip_dns_query *q, const struct dns_pkt *p)
{
	unsigned int rcode = DNS_RCODE(p->flags);

	if (p->flags & DNS_F_TC) {
		q->truncated = true;
		q->r->stat.truncated++;
	}

	/* the name does not exist: terminal for this question, route by stage */
	if (rcode == DNS_RCODE_NXDOMAIN) {
		if (q->stage == DNS_STAGE_ADDR)
			q->addr_absent |= addr_bit(q->qtype);
		stage_failed(q, SIP_DNS_ERR_NXDOMAIN);
		return;
	}

	if (rcode != DNS_RCODE_OK) {
		server_next(q, SIP_DNS_ERR_SERVFAIL);
		return;
	}

	/*
	 * A NOERROR answer that carries nothing usable is not a timeout: keep
	 * the distinction so an operator sees "the zone has no record" and not
	 * "the server never answered". A more specific reason already recorded
	 * (NXDOMAIN from another server) is kept.
	 */
	if (q->fail == SIP_DNS_ERR_TIMEOUT)
		q->fail = SIP_DNS_ERR_FORMAT;

	if (q->stage == DNS_STAGE_SRV) {
		answer_srv(q, p);
		return;
	}

	answer_addr(q, p);
}

/*
 * A datagram is this query's answer only if it echoes the id, the question and
 * comes from the server the current attempt asked. The id is OS random and the
 * server list is the per query snapshot, so a spoofer has to guess the id and
 * the ephemeral source port of this one lookup.
 */
static bool query_accept(const struct sip_dns_query *q, const struct dns_pkt *p,
			 const struct sip_txn_dst *src)
{
	if (q->id != p->id || q->qtype != p->qtype)
		return false;
	if (!p->has_question || !dns_name_eq(q->qname, p->qname))
		return false;
	if (q->server >= q->qservers || !sip_dst_eq(&q->qserver[q->server], src))
		return false;

	return true;
}

static void query_deliver_in(struct sip_dns_query *q, const void *buf, size_t len,
			     const struct sip_txn_dst *src)
{
	struct sip_dns *r = q->r;
	struct dns_pkt p;

	if (q->complete)
		return;

	if (dns_pkt_parse(&p, buf, len) < 0 || !(p.flags & DNS_F_QR)) {
		r->stat.dropped++;
		return;
	}

	if (!query_accept(q, &p, src)) {
		r->stat.dropped++;
		return;
	}

	r->io.timer_cancel(r, q);
	r->stat.rx++;
	DNS_TRACE(r, "dns: answer for %s type %u rcode %u", p.qname, p.qtype,
		  DNS_RCODE(p.flags));
	answer_process(q, &p);
}

void sip_dns_input(struct sip_dns *r, uint32_t query, const void *buf, size_t len,
		   const struct sip_txn_dst *src)
{
	struct sip_dns_query *q = query_get(r, query);

	if (q)
		query_deliver_in(q, buf, len, src);
}

/* ------------------------------------------------------------------ send */

void sip_dns_query_fire(struct sip_dns_query *q)
{
	struct sip_dns *r = q->r;

	if (q->complete) {
		result_deliver(q);
		return;
	}

	r->stat.timeouts++;

	if (q->attempt < r->cfg.retries) {
		q->attempt++;
		r->stat.retries++;
		query_send(q);
		return;
	}

	server_next(q, SIP_DNS_ERR_TIMEOUT);
}

static void query_send(struct sip_dns_query *q)
{
	struct sip_dns *r = q->r;
	uint8_t pkt[SIP_DNS_PKT_MAX];
	int len;
	int n;

	if (q->server >= q->qservers) {
		query_finish(q, SIP_DNS_ERR_NO_SERVER);
		return;
	}

	if (q->stage == DNS_STAGE_ADDR)
		q->addr_asked |= addr_bit(q->qtype);

	q->id = dns_txid(r);
	len = dns_query_build(pkt, sizeof(pkt), q->id, q->qname, q->qtype, r->cfg.edns);
	if (len < 0) {
		query_finish(q, SIP_DNS_ERR_INTERNAL);
		return;
	}

	n = r->io.send(r, q, pkt, (size_t)len, &q->qserver[q->server]);
	if (n < 0) {
		server_next(q, SIP_DNS_ERR_INTERNAL);
		return;
	}

	r->stat.tx++;
	DNS_TRACE(r, "dns: ask server %u for %s type %u", q->server, q->qname,
		  q->qtype);

	r->io.timer_set(r, q, r->cfg.timeout_ms);
}

/* ------------------------------------------------------------------- api */

static void cache_deliver(struct sip_dns *r, struct sip_dns_query *q,
			  const struct sip_dns_cache *c)
{
	uint64_t now = r->io.now_ms(r);
	uint32_t rem;
	unsigned int i;

	q->count = c->count;
	for (i = 0; i < c->count; i++)
		q->target[i] = c->target[i];

	/*
	 * The caller schedules its next lookup from this TTL, so it must be the
	 * time the entry has left, not the value it was cached with. The floor
	 * of 1 keeps a caller from rescheduling at 0 in the last second.
	 */
	rem = c->expire_ms > now ? (uint32_t)((c->expire_ms - now) / 1000) : 0;
	q->ttl = rem ? rem : 1;
	q->fail = SIP_DNS_OK;
	q->cached = true;
	q->srv = c->srv;
	sip_dns_srv_sort(q->target, q->count, dns_rand_cb, r);

	r->stat.cache_hits++;
	deliver_arm(q);
}

/*
 * RFC 3263 section 4: a target that is already a numeric address is used
 * directly. No NAPTR, no SRV, no A. A question for "127.0.0.1" can only come
 * back NXDOMAIN, so without this path a lab config and every peer addressed
 * by IP would fail to resolve. Returns false when name is not numeric, and the
 * caller then runs the normal resolution.
 *
 * Both families, whatever cfg.ipv6 holds. cfg.ipv6 decides which question the
 * resolver asks for a name, an AAAA next to the A, and a literal asks nothing.
 * The family travels with the answer: struct sip_dns_target carries it in
 * dst.sa.ss_family and in the presentation form in host, and every consumer
 * reads the family from the target it got. sip_dns_servers_parse() takes an
 * IPv6 nameserver literal on the same grounds.
 *
 * The kind is ignored. A literal has no zone, so SIP_DNS_SRV has nothing to
 * ask, and the plain address with the caller's port is the RFC 3263 answer for
 * it. SIP_DNS_A and SIP_DNS_AAAA name a record type, not a family filter.
 *
 * The cache is not involved, in either direction. A literal cannot go stale, so
 * a lookup in the cache gains nothing, and an entry for it would evict a real
 * name out of the SIP_DNS_CACHE_MAX slots. changed therefore stays false: the
 * address is inside the name, so no earlier answer for this name can differ.
 *
 * Delivery goes through deliver_arm(), the zero millisecond timer that
 * cache_deliver() uses, because done() must never run inside
 * sip_dns_resolve() (shared/resolve.h "Callback order").
 */
static bool literal_deliver(struct sip_dns_query *q)
{
	struct sip_dns_target *t = &q->target[0];
	struct sip_txn_dst dst;

	if (!sip_dst_set(&dst, q->name, q->port))
		return false;

	memset(t, 0, sizeof(*t));
	t->dst = dst;
	t->port = q->port;
	sip_dst_host(&dst, t->host, sizeof(t->host));

	q->count = 1;
	q->ttl = SIP_DNS_TTL_LITERAL;
	ttl_clamp(q);
	q->fail = SIP_DNS_OK;
	q->literal = true;		/* the delivery keeps both families */
	q->cached = true;		/* served without a query */
	q->srv = false;			/* a literal has no zone to hold a record */

	DNS_TRACE(q->r, "dns: %s is a literal, no lookup", q->name);
	deliver_arm(q);

	return true;
}

uint32_t sip_dns_resolve(struct sip_dns *r, const char *name, enum sip_dns_kind kind,
			 uint16_t port, uint32_t owner)
{
	return sip_dns_resolve_svc(r, name, kind, port, owner, NULL);
}

uint32_t sip_dns_resolve_svc(struct sip_dns *r, const char *name, enum sip_dns_kind kind,
			     uint16_t port, uint32_t owner, const char *service)
{
	struct sip_dns_query *q = NULL;
	struct sip_dns_cache *c;
	unsigned int i;

	if (!name || !*name || strlen(name) >= SIP_DNS_NAME_MAX || kind >= SIP_DNS_KIND_MAX)
		return SIP_HANDLE_NONE;

	for (i = 0; i < SIP_DNS_QUERY_MAX; i++) {
		if (r->query[i].in_use)
			continue;

		q = &r->query[i];
		break;
	}

	if (!q) {
		r->stat.denied++;
		return SIP_HANDLE_NONE;
	}

	memset(&q->timer, 0, sizeof(q->timer));
	q->r = r;
	q->owner = owner;
	q->kind = (uint8_t)kind;
	q->port = port;
	q->count = 0;
	q->ttl = 0;
	q->cname_depth = 0;
	q->glue_idx = 0;
	q->glue_scan = 0;
	q->glue_count = 0;
	memset(q->glue_asked, 0, sizeof(q->glue_asked));
	q->addr_asked = 0;
	q->addr_absent = 0;
	q->fail = SIP_DNS_ERR_TIMEOUT;
	q->truncated = false;
	q->ttl_set = false;
	q->aaaa_done = false;
	q->literal = false;
	q->changed = false;
	q->cached = false;
	q->srv = false;
	q->complete = false;
	q->in_use = true;
	str_copy(q->name, sizeof(q->name), name);
	str_copy(q->service, sizeof(q->service),
		 service_label_get(r, service, kind));

	r->stat.queries++;

	if (literal_deliver(q))
		return query_handle(q);

	c = cache_find(r, name, kind, port, q->service);
	if (c && c->expire_ms > r->io.now_ms(r)) {
		cache_deliver(r, q, c);
		return query_handle(q);
	}

	/*
	 * Reread resolv.conf at the start of every cycle that reaches the wire,
	 * so a DHCP renew is picked up without a reload; a sip_dns_servers_set()
	 * override pins the list and is not reread. Then snapshot the list into
	 * the query, so a later reread or override never moves this query onto a
	 * server it did not ask, which would drop its answer.
	 */
	if (!r->servers_pinned)
		sip_dns_servers_load(r);

	if (!r->servers) {
		q->fail = SIP_DNS_ERR_NO_SERVER;
		r->stat.failures++;
		deliver_arm(q);
		return query_handle(q);
	}

	q->qservers = r->servers > SIP_DNS_SERVER_MAX ? SIP_DNS_SERVER_MAX : r->servers;
	for (i = 0; i < q->qservers; i++)
		q->qserver[i] = r->server[i];

	if (kind == SIP_DNS_SRV) {
		q->stage = DNS_STAGE_SRV;
		if (!srv_question_set(q)) {
			query_finish(q, SIP_DNS_ERR_INTERNAL);
			return query_handle(q);
		}
	} else {
		q->stage = DNS_STAGE_ADDR;
		question_set(q, name, kind == SIP_DNS_AAAA ? DNS_T_AAAA : DNS_T_A);
	}

	query_send(q);

	return query_handle(q);
}

void sip_dns_cancel(struct sip_dns *r, uint32_t query)
{
	struct sip_dns_query *q = query_get(r, query);

	if (!q)
		return;

	query_free(q);
}

unsigned int sip_dns_owner_release(struct sip_dns *r, uint32_t owner)
{
	unsigned int count = 0;
	unsigned int i;

	for (i = 0; i < SIP_DNS_QUERY_MAX; i++) {
		struct sip_dns_query *q = &r->query[i];

		if (!q->in_use || q->owner != owner)
			continue;

		query_free(q);
		count++;
	}

	return count;
}

const struct sip_dns_stat *sip_dns_stat(const struct sip_dns *r)
{
	return &r->stat;
}

void sip_dns_prefer_family_set(struct sip_dns *r, int family)
{
	r->prefer_family = family;
}

int sip_dns_prefer_family(const struct sip_dns *r)
{
	return r->prefer_family;
}

void sip_dns_ipv6_set(struct sip_dns *r, bool ipv6)
{
	unsigned int i;

	if (r->cfg.ipv6 == ipv6)
		return;

	r->cfg.ipv6 = ipv6;
	sip_dns_cache_flush(r);

	if (!ipv6)
		return;

	/*
	 * A record the glue chase passed owes an AAAA question now, so the
	 * cursor of every query in flight goes back to the first record.
	 * glue_asked keeps a family that was already asked from going out
	 * twice, so a record that owes nothing costs one comparison.
	 */
	for (i = 0; i < SIP_DNS_QUERY_MAX; i++) {
		struct sip_dns_query *q = &r->query[i];

		if (q->in_use && !q->complete)
			q->glue_scan = 0;
	}
}

void sip_dns_cfg_default(struct sip_dns_cfg *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->timeout_ms = SIP_DNS_TIMEOUT_MS;
	cfg->retries = SIP_DNS_RETRIES;
	cfg->ttl_min = SIP_DNS_TTL_MIN;
	cfg->ttl_max = SIP_DNS_TTL_MAX;
	cfg->edns = true;
	cfg->srv_fallback = true;
}

void sip_dns_init(struct sip_dns *r, const struct sip_dns_cfg *cfg,
		  const struct sip_dns_cb *cb, void *priv)
{
	unsigned int i;

	memset(r, 0, sizeof(*r));

	if (cfg)
		r->cfg = *cfg;
	else
		sip_dns_cfg_default(&r->cfg);

	if (cb)
		r->cb = *cb;
	r->priv = priv;
	r->io = io_uloop;

	if (!r->cfg.timeout_ms)
		r->cfg.timeout_ms = SIP_DNS_TIMEOUT_MS;
	if (!r->cfg.retries)
		r->cfg.retries = SIP_DNS_RETRIES;
	if (!r->cfg.ttl_min)
		r->cfg.ttl_min = SIP_DNS_TTL_MIN;
	if (!r->cfg.ttl_max)
		r->cfg.ttl_max = SIP_DNS_TTL_MAX;
	if (r->cfg.ttl_max < r->cfg.ttl_min)
		r->cfg.ttl_max = r->cfg.ttl_min;

	r->rand_state = (uint32_t)(mono_ms() ^ ((uint64_t)getpid() << 16));
	if (!r->rand_state)
		r->rand_state = 0x2545f491u;

	for (i = 0; i < SIP_DNS_QUERY_MAX; i++) {
		r->query[i].index = i;
		r->query[i].gen = 1;
		r->query[i].r = r;
		r->query[i].ufd.fd = -1;
	}
}

void sip_dns_done(struct sip_dns *r)
{
	unsigned int i;

	for (i = 0; i < SIP_DNS_QUERY_MAX; i++) {
		if (!r->query[i].in_use)
			continue;

		query_free(&r->query[i]);
	}
}
