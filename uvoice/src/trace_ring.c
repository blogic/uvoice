// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The udebug rings, the relay and the pcapng export. See trace_ring.h for the
 * contracts and the thread rule.
 *
 * The relay is built in every configuration, because a backend thread calls
 * it and a build without udebug support must still link. Everything that
 * touches a ring sits behind UVOICE_HAVE_UDEBUG and answers -ENOSYS without
 * it.
 */
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libubox/ulog.h>
#include <libubox/uloop.h>

#include "trace.h"
#include "trace_ring.h"

#ifdef UVOICE_HAVE_UDEBUG
#include <libubox/udebug.h>
#include <libubox/udebug-proto.h>

#include "pcapng.h"
#endif

/* --- the relay ----------------------------------------------------------- */

/*
 * The library logs from its control, slic and emitter threads, and a udebug
 * ring takes one producer. The lines land here instead and the 100 ms drain
 * below writes them from the uloop thread.
 *
 * The mutex covers the copy and the two indexes, nothing else. A slic thread
 * already makes a syscall to log, so this much is inside its budget; the
 * engine tick and the net thread never log at all.
 */
struct relay_line {
	int prio;
	char text[TRACE_RELAY_LINE_MAX];
};

static struct {
	pthread_mutex_t lock;
	struct relay_line line[TRACE_RELAY_LINES];
	unsigned int head;
	unsigned int tail;
	uint64_t dropped;
} relay = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
};

void trace_relay_put(int prio, const char *msg)
{
	struct relay_line *slot;

	pthread_mutex_lock(&relay.lock);
	if (relay.head - relay.tail >= TRACE_RELAY_LINES) {
		relay.dropped++;
		pthread_mutex_unlock(&relay.lock);
		return;
	}

	slot = &relay.line[relay.head % TRACE_RELAY_LINES];
	slot->prio = prio;
	snprintf(slot->text, sizeof(slot->text), "%s", msg);
	relay.head++;
	pthread_mutex_unlock(&relay.lock);
}

uint64_t trace_relay_dropped(void)
{
	uint64_t dropped;

	pthread_mutex_lock(&relay.lock);
	dropped = relay.dropped;
	pthread_mutex_unlock(&relay.lock);

	return dropped;
}

#ifdef UVOICE_HAVE_UDEBUG

/* The queue and the drop count of one trace. A line a backend thread left
 * behind after the last close belongs to no trace, so the open drops it here
 * rather than let it open the next archive. */
static void relay_reset(void)
{
	pthread_mutex_lock(&relay.lock);
	relay.head = 0;
	relay.tail = 0;
	relay.dropped = 0;
	pthread_mutex_unlock(&relay.lock);
}

static bool relay_take(struct relay_line *out)
{
	bool got;

	pthread_mutex_lock(&relay.lock);
	got = relay.head != relay.tail;
	if (got) {
		*out = relay.line[relay.tail % TRACE_RELAY_LINES];
		relay.tail++;
	}
	pthread_mutex_unlock(&relay.lock);

	return got;
}

/* The priority in front of the line, for the two levels a reader looks for.
 * An informational line carries none, so the common case stays short. */
static const char *relay_prio_word(int prio)
{
	if (prio <= LOG_ERR)
		return "err: ";
	if (prio == LOG_WARNING)
		return "warn: ";

	return "";
}

/* --- the rings ----------------------------------------------------------- */

static struct udebug ctx;
static struct udebug_buf rings[__TRACE_RING_MAX];
static struct trace_ring_counts counts[__TRACE_RING_MAX];
static struct uloop_timeout relay_timer;
static bool rings_open;
/* The open state for the backend's own threads, the shape of the two mask
 * mirrors of trace.h. */
static _Atomic bool open_mirror;

/*
 * A SIP message rides UDP framing with its real ports whatever transport
 * carried it, so one decoder serves all three and the entry says which it
 * was (trace_ring.h). The two string rings export under DLT 147, and the
 * canceller ring under DLT 148 (pcapng.h).
 */
static const struct udebug_buf_meta ring_meta[__TRACE_RING_MAX] = {
	[TRACE_RING_LOG] = {
		.name = "uvoiced_log",
		.format = UDEBUG_FORMAT_STRING,
	},
	[TRACE_RING_SIP] = {
		.name = "uvoiced_sip",
		.format = UDEBUG_FORMAT_PACKET,
		.sub_format = UDEBUG_DLT_RAW_IP,
	},
	[TRACE_RING_STATUS] = {
		.name = "uvoiced_status",
		.format = UDEBUG_FORMAT_STRING,
	},
	[TRACE_RING_RTP] = {
		.name = "uvoiced_rtp",
		.format = UDEBUG_FORMAT_PACKET,
		.sub_format = UDEBUG_DLT_RAW_IP,
	},
	[TRACE_RING_LEC] = {
		.name = "uvoiced_lec",
		.format = UDEBUG_FORMAT_PACKET,
		.sub_format = PCAPNG_LINKTYPE_LEC,
	},
};

static const struct {
	unsigned int entries;
	unsigned int size;
} ring_geom[__TRACE_RING_MAX] = {
	[TRACE_RING_LOG] = { TRACE_RING_LOG_ENTRIES, TRACE_RING_LOG_SIZE },
	[TRACE_RING_SIP] = { TRACE_RING_SIP_ENTRIES, TRACE_RING_SIP_SIZE },
	[TRACE_RING_STATUS] = { TRACE_RING_STATUS_ENTRIES,
				TRACE_RING_STATUS_SIZE },
	[TRACE_RING_RTP] = { TRACE_RING_RTP_ENTRIES, TRACE_RING_RTP_SIZE },
	[TRACE_RING_LEC] = { TRACE_RING_LEC_ENTRIES, TRACE_RING_LEC_SIZE },
};

static bool ring_live(enum trace_ring_id id)
{
	if ((unsigned int)id >= __TRACE_RING_MAX)
		return false;

	return udebug_buf_valid(&rings[id]);
}

static uint32_t ring_head(enum trace_ring_id id)
{
	return rings[id].hdr->head;
}

/* One entry of len bytes. */
static void ring_bytes(enum trace_ring_id id, const void *data, size_t len)
{
	struct udebug_buf *buf = &rings[id];

	if (!ring_live(id))
		return;

	udebug_entry_init(buf);
	udebug_entry_append(buf, data, (uint32_t)len);
	udebug_entry_add(buf);
	counts[id].written++;
}

/* One string entry from up to three pieces. The pieces go straight into the
 * ring, so a line needs no buffer of its own and loses no character to one. */
static void ring_text(enum trace_ring_id id, const char *a, const char *b,
		      const char *c)
{
	struct udebug_buf *buf = &rings[id];

	if (!ring_live(id))
		return;

	udebug_entry_init(buf);
	udebug_entry_append(buf, a, (uint32_t)strlen(a));
	if (b)
		udebug_entry_append(buf, b, (uint32_t)strlen(b));
	if (c)
		udebug_entry_append(buf, c, (uint32_t)strlen(c));
	udebug_entry_add(buf);
	counts[id].written++;
}

void trace_ring_log(enum trace_domain dom, const char *msg)
{
	char prefix[32];

	snprintf(prefix, sizeof(prefix), "[%s] ", trace_domain_name(dom));
	ring_text(TRACE_RING_LOG, prefix, msg, NULL);
}

static void relay_drain(void)
{
	struct relay_line line;
	unsigned int i;

	/* The relay holds at most its own depth, so the bound is reached only
	 * when a producer fills it while this loop runs. */
	for (i = 0; i < TRACE_RELAY_LINES; i++) {
		if (!relay_take(&line))
			return;

		ring_text(TRACE_RING_LOG, "[hal] ", relay_prio_word(line.prio),
			  line.text);
	}
}

static void relay_expire(struct uloop_timeout *t)
{
	relay_drain();
	uloop_timeout_set(&relay_timer, TRACE_RELAY_DRAIN_MS);
}

void trace_relay_drain(void)
{
	/* A drain with no ring to write to would consume the lines and lose
	 * them; they stay queued for the open that follows instead. */
	if (!rings_open)
		return;

	relay_drain();
}

void trace_ring_init(void)
{
	relay_timer.cb = relay_expire;
	udebug_init(&ctx);
	udebug_auto_connect(&ctx, NULL);
	trace_sink_set(trace_ring_log);
}

static void rings_free(void)
{
	unsigned int id;

	for (id = 0; id < __TRACE_RING_MAX; id++) {
		if (udebug_buf_valid(&rings[id]))
			udebug_buf_free(&rings[id]);
	}
}

static int ring_create(enum trace_ring_id id)
{
	struct udebug_buf *buf = &rings[id];

	/* udebug_buf_init answers -1 for a failed shared mapping and leaves
	 * the cause in errno; a zero errno means the geometry itself was
	 * refused, which is a memory answer too. */
	errno = 0;
	if (udebug_buf_init(buf, ring_geom[id].entries, ring_geom[id].size))
		return errno ? -errno : -ENOMEM;

	if (udebug_buf_add(&ctx, buf, &ring_meta[id])) {
		udebug_buf_free(buf);
		return -ENOMEM;
	}

	return 0;
}

int trace_ring_open(bool with_rtp, bool with_lec)
{
	unsigned int id;

	if (rings_open)
		return -EBUSY;

	relay_reset();
	memset(counts, 0, sizeof(counts));
	for (id = 0; id < __TRACE_RING_MAX; id++) {
		int err;

		if (id == TRACE_RING_RTP && !with_rtp)
			continue;
		if (id == TRACE_RING_LEC && !with_lec)
			continue;

		err = ring_create((enum trace_ring_id)id);
		if (!err)
			continue;

		rings_free();
		return err;
	}

	rings_open = true;
	atomic_store_explicit(&open_mirror, true, memory_order_relaxed);
	uloop_timeout_set(&relay_timer, TRACE_RELAY_DRAIN_MS);

	return 0;
}

void trace_ring_close(void)
{
	if (!rings_open)
		return;

	/* The lines the backend threads pushed since the last drain belong to
	 * this trace, and the rings are still open to take them. The mirror
	 * falls after the drain, so no thread writes into a freed ring. */
	relay_drain();
	uloop_timeout_cancel(&relay_timer);
	rings_free();
	rings_open = false;
	atomic_store_explicit(&open_mirror, false, memory_order_relaxed);
}

bool trace_ring_is_open(void)
{
	return rings_open;
}

bool trace_ring_open_atomic(void)
{
	return atomic_load_explicit(&open_mirror, memory_order_relaxed);
}

bool trace_ring_registered(void)
{
	return udebug_is_connected(&ctx);
}

void trace_ring_counts(enum trace_ring_id ring, struct trace_ring_counts *out)
{
	if (!out)
		return;

	memset(out, 0, sizeof(*out));
	if ((unsigned int)ring >= __TRACE_RING_MAX)
		return;

	*out = counts[ring];
}

/* --- the synthesized datagram -------------------------------------------- */

#define IP_PROTO_UDP		17
#define DGRAM_TTL		64
#define IPV4_HDR_LEN		20
#define IPV6_HDR_LEN		40
#define UDP_HDR_LEN		8

/* The direction and the kind of one entry, in the byte trace_ring.h
 * documents. */
static uint8_t dscp_byte(enum trace_pkt_dir dir, enum trace_pkt_kind kind)
{
	unsigned int val = ((unsigned int)dir & 0x3u) << 6;

	return (uint8_t)(val | (((unsigned int)kind & 0xfu) << 2));
}

/* The ones-complement sum of RFC 1071 over a header whose checksum field is
 * still zero. */
static uint16_t ip_checksum(const uint8_t *hdr, size_t len)
{
	uint32_t sum = 0;
	size_t i;

	for (i = 0; i + 1 < len; i += 2)
		sum += ((uint32_t)hdr[i] << 8) | hdr[i + 1];

	while (sum >> 16)
		sum = (sum & 0xffffu) + (sum >> 16);

	return (uint16_t)~sum;
}

static void be16_put(uint8_t *out, uint16_t val)
{
	out[0] = (uint8_t)(val >> 8);
	out[1] = (uint8_t)val;
}

static size_t ipv4_hdr_build(uint8_t *out, uint8_t tos,
			     const struct trace_endpoint *src,
			     const struct trace_endpoint *dst, size_t payload)
{
	uint16_t total = (uint16_t)(IPV4_HDR_LEN + UDP_HDR_LEN + payload);

	memset(out, 0, IPV4_HDR_LEN);
	out[0] = 0x45;			/* version 4, five words of header */
	out[1] = tos;
	be16_put(&out[2], total);
	out[8] = DGRAM_TTL;
	out[9] = IP_PROTO_UDP;
	memcpy(&out[12], src->addr, 4);
	memcpy(&out[16], dst->addr, 4);
	be16_put(&out[10], ip_checksum(out, IPV4_HDR_LEN));

	return IPV4_HDR_LEN;
}

static size_t ipv6_hdr_build(uint8_t *out, uint8_t tos,
			     const struct trace_endpoint *src,
			     const struct trace_endpoint *dst, size_t payload)
{
	memset(out, 0, IPV6_HDR_LEN);
	/* Version 6 and the traffic class straddle the first two bytes; the
	 * flow label below them stays zero. */
	out[0] = (uint8_t)(0x60u | (tos >> 4));
	out[1] = (uint8_t)((tos & 0x0fu) << 4);
	be16_put(&out[4], (uint16_t)(UDP_HDR_LEN + payload));
	out[6] = IP_PROTO_UDP;
	out[7] = DGRAM_TTL;
	memcpy(&out[8], src->addr, 16);
	memcpy(&out[24], dst->addr, 16);

	return IPV6_HDR_LEN;
}

/* The checksum stays zero: the payload is the message as it was, and a
 * capture tool treats a zero UDP checksum as unchecked. */
static void udp_hdr_build(uint8_t *out, uint16_t sport, uint16_t dport,
			  size_t payload)
{
	be16_put(&out[0], sport);
	be16_put(&out[2], dport);
	be16_put(&out[4], (uint16_t)(UDP_HDR_LEN + payload));
	be16_put(&out[6], 0);
}

static size_t pkt_hdr_build(uint8_t *out, uint8_t tos,
			    const struct trace_endpoint *src,
			    const struct trace_endpoint *dst, size_t payload)
{
	size_t len;

	if (src->ipver == 4)
		len = ipv4_hdr_build(out, tos, src, dst, payload);
	else
		len = ipv6_hdr_build(out, tos, src, dst, payload);

	udp_hdr_build(out + len, src->port, dst->port, payload);

	return len + UDP_HDR_LEN;
}

static size_t entry_cap(enum trace_ring_id ring)
{
	if (ring == TRACE_RING_RTP)
		return CALLTRACE_RTP_ENTRY_MAX;

	return CALLTRACE_SIP_ENTRY_MAX;
}

int trace_ring_packet(enum trace_ring_id ring, enum trace_pkt_dir dir,
		      enum trace_pkt_kind kind,
		      const struct trace_endpoint *src,
		      const struct trace_endpoint *dst,
		      uint64_t stamp_us, const void *data, size_t len)
{
	uint8_t hdr[IPV6_HDR_LEN + UDP_HDR_LEN];
	struct udebug_buf *buf = &rings[ring];
	size_t hlen;

	if (ring != TRACE_RING_SIP && ring != TRACE_RING_RTP)
		return -EINVAL;
	if (!src || !dst || src->ipver != dst->ipver)
		return -EINVAL;
	if (src->ipver != 4 && src->ipver != 6)
		return -EINVAL;
	if (!ring_live(ring))
		return -ENOENT;

	/* The header describes what the entry carries, so a cut message reads
	 * as the datagram it became and the count says one was cut. */
	if (len > entry_cap(ring)) {
		len = entry_cap(ring);
		counts[ring].truncated++;
	}

	hlen = pkt_hdr_build(hdr, dscp_byte(dir, kind), src, dst, len);
	udebug_entry_init_ts(buf, stamp_us ? stamp_us : udebug_timestamp());
	udebug_entry_append(buf, hdr, (uint32_t)hlen);
	udebug_entry_append(buf, data, (uint32_t)len);
	udebug_entry_add(buf);
	counts[ring].written++;

	return 0;
}

int trace_ring_status(const char *json)
{
	size_t len = strlen(json);

	if (!ring_live(TRACE_RING_STATUS))
		return -ENOENT;

	if (len > CALLTRACE_STATUS_ENTRY_MAX) {
		len = CALLTRACE_STATUS_ENTRY_MAX;
		counts[TRACE_RING_STATUS].truncated++;
	}

	ring_bytes(TRACE_RING_STATUS, json, len);

	return 0;
}

_Static_assert(sizeof(struct trace_lec_hdr) +
	       3 * TRACE_LEC_SAMPLES_WB * sizeof(int16_t) ==
	       CALLTRACE_LEC_ENTRY_MAX,
	       "a RUN entry of the widest line is the canceller entry cap");

static unsigned int lec_planes(uint8_t kind)
{
	switch (kind) {
	case TRACE_LEC_RUN:
		return 3;
	case TRACE_LEC_FEED:
		return 2;
	}

	return 0;
}

int trace_ring_lec(const struct trace_lec_hdr *hdr, uint64_t stamp_us,
		   const int16_t *ref, const int16_t *mic, const int16_t *out)
{
	const int16_t *plane[3] = { ref, mic, out };
	struct udebug_buf *buf = &rings[TRACE_RING_LEC];
	unsigned int planes, i;
	uint32_t bytes;

	if (!hdr || hdr->kind > TRACE_LEC_END)
		return -EINVAL;
	if (hdr->samples != TRACE_LEC_SAMPLES_NB &&
	    hdr->samples != TRACE_LEC_SAMPLES_WB)
		return -EINVAL;

	planes = lec_planes(hdr->kind);
	for (i = 0; i < planes; i++) {
		if (!plane[i])
			return -EINVAL;
	}
	if (!ring_live(TRACE_RING_LEC))
		return -ENOENT;

	/* The header goes out as the host lays it: the struct has no padding,
	 * and the host order is the order the section header declares
	 * (pcapng.h), which is the order the reader follows. */
	bytes = (uint32_t)hdr->samples * (uint32_t)sizeof(int16_t);

	udebug_entry_init_ts(buf, stamp_us ? stamp_us : udebug_timestamp());
	udebug_entry_append(buf, hdr, (uint32_t)sizeof(*hdr));
	for (i = 0; i < planes; i++)
		udebug_entry_append(buf, plane[i], bytes);
	udebug_entry_add(buf);
	counts[TRACE_RING_LEC].written++;

	return 0;
}

/* --- the export ---------------------------------------------------------- */

struct export_ring {
	uint32_t iface;
	uint32_t cursor;
	uint64_t bytes;		/* what the blocks of this ring added */
	bool present;
	bool stopped;
};

static struct {
	struct pcapng_writer w;
	struct export_ring ring[__TRACE_RING_MAX];
	bool open;
} xp;

static uint16_t ring_link_type(enum trace_ring_id id)
{
	if (ring_meta[id].format == UDEBUG_FORMAT_PACKET)
		return (uint16_t)ring_meta[id].sub_format;

	return PCAPNG_LINKTYPE_STRING;
}

/*
 * What the producer overwrote since the last walk. Two things are lost when a
 * trace outruns its export: the entry slot the head passed, and the data of an
 * entry whose bytes the ring has reused under it. Both are counted and
 * skipped, so the file goes on from the oldest entry that is still whole and
 * the count says how many it is missing.
 *
 * The slot loss is one subtraction whatever it covers. The data loss is a
 * scan, so each entry it reads costs the caller's budget and the scan stops
 * when the budget is spent; the next walk carries on from there. The oldest
 * surviving byte is data_used minus the ring size, because udebug_entry_add
 * leaves data_used one past the newest entry, so an entry that starts there
 * is whole.
 */
static void export_skip_lost(enum trace_ring_id id, uint32_t head,
			     unsigned int *budget)
{
	struct export_ring *xr = &xp.ring[id];
	struct udebug_buf *buf = &rings[id];
	struct udebug_hdr *hdr = buf->hdr;
	uint32_t behind = head - xr->cursor;
	uint32_t min_data;

	if (behind > buf->ring_size) {
		counts[id].overwritten += behind - (uint32_t)buf->ring_size;
		xr->cursor = head - (uint32_t)buf->ring_size;
	}

	min_data = hdr->data_used - (uint32_t)buf->data_size;
	while (xr->cursor != head && *budget) {
		struct udebug_ptr *ptr = udebug_ring_ptr(hdr, xr->cursor);

		/* The entry that ends the scan is whole and the walk below
		 * exports it in this same step, so a skip is what costs the
		 * budget and a healthy ring pays nothing here. */
		if ((int32_t)(ptr->start - min_data) >= 0)
			return;

		counts[id].overwritten++;
		xr->cursor++;
		(*budget)--;
	}
}

static int ring_walk(enum trace_ring_id id, unsigned int *budget)
{
	struct export_ring *xr = &xp.ring[id];
	struct udebug_buf *buf = &rings[id];
	struct udebug_hdr *hdr = buf->hdr;
	uint32_t head = ring_head(id);

	export_skip_lost(id, head, budget);
	while (xr->cursor != head && *budget) {
		struct udebug_ptr *ptr = udebug_ring_ptr(hdr, xr->cursor);
		uint64_t before = xp.w.bytes;
		int err = pcapng_packet_write(&xp.w, xr->iface, ptr->timestamp,
					      udebug_buf_ptr(buf, ptr->start),
					      ptr->len);

		if (err)
			return err;

		xr->bytes += xp.w.bytes - before;
		xr->cursor++;
		counts[id].exported++;
		(*budget)--;
	}

	return 0;
}

int trace_ring_export_open(const char *path)
{
	unsigned int id;
	int err;

	if (xp.open)
		return -EBUSY;
	if (!rings_open)
		return -ENOENT;

	memset(&xp, 0, sizeof(xp));
	err = pcapng_open(&xp.w, path, "uvoiced");
	if (err)
		return err;

	/* The export starts at the head each ring has now. What the rings
	 * already hold belongs to the export that ran before this one. */
	for (id = 0; id < __TRACE_RING_MAX; id++) {
		enum trace_ring_id rid = (enum trace_ring_id)id;

		if (!ring_live(rid))
			continue;

		err = pcapng_iface_write(&xp.w, ring_link_type(rid),
					 ring_meta[id].name,
					 &xp.ring[id].iface);
		if (err) {
			pcapng_close(&xp.w, NULL);
			return err;
		}

		xp.ring[id].present = true;
		xp.ring[id].cursor = ring_head(rid);
	}

	xp.open = true;

	return 0;
}

int trace_ring_export_step(unsigned int max_entries, bool *done)
{
	unsigned int budget = max_entries;
	unsigned int id;
	bool caught = true;

	if (done)
		*done = false;
	if (!xp.open)
		return -ENOENT;

	for (id = 0; id < __TRACE_RING_MAX; id++) {
		enum trace_ring_id rid = (enum trace_ring_id)id;
		int err;

		if (!xp.ring[id].present || xp.ring[id].stopped ||
		    !ring_live(rid))
			continue;

		err = ring_walk(rid, &budget);
		if (err)
			return err;

		if (xp.ring[id].cursor != ring_head(rid))
			caught = false;
	}

	if (done)
		*done = caught;

	return 0;
}

int trace_ring_export_close(uint64_t *file_bytes)
{
	unsigned int id;
	int err = 0;

	if (!xp.open) {
		if (file_bytes)
			*file_bytes = 0;
		return -ENOENT;
	}

	/* The lines the relay still holds belong to this file, and no drain
	 * tick follows the close. */
	trace_relay_drain();

	/* The last walk takes everything that is left, because no tick
	 * follows it. The geometry bounds it: at most the entry count of each
	 * open ring. */
	for (id = 0; id < __TRACE_RING_MAX && !err; id++) {
		enum trace_ring_id rid = (enum trace_ring_id)id;
		unsigned int budget = UINT_MAX;

		if (!xp.ring[id].present || !ring_live(rid))
			continue;

		/*
		 * A ring a caller stopped keeps its cursor, and what it holds
		 * past it never reaches the file. The producer went on filling
		 * it, so the loss is reconciled first: what the head passed is
		 * overwritten and what survived is truncated, and one entry is
		 * counted under one name.
		 */
		if (xp.ring[id].stopped) {
			uint32_t head = ring_head(rid);

			export_skip_lost(rid, head, &budget);
			counts[id].truncated += head - xp.ring[id].cursor;
			continue;
		}

		err = ring_walk(rid, &budget);
	}

	xp.open = false;
	if (err) {
		pcapng_close(&xp.w, file_bytes);
		return err;
	}

	return pcapng_close(&xp.w, file_bytes);
}

void trace_ring_export_ring_enable(enum trace_ring_id ring, bool on)
{
	if ((unsigned int)ring >= __TRACE_RING_MAX || !xp.open)
		return;

	xp.ring[ring].stopped = !on;
}

uint64_t trace_ring_export_bytes(void)
{
	return xp.w.bytes;
}

uint64_t trace_ring_export_ring_bytes(enum trace_ring_id ring)
{
	if ((unsigned int)ring >= __TRACE_RING_MAX)
		return 0;

	return xp.ring[ring].bytes;
}

void trace_ring_done(void)
{
	if (xp.open)
		trace_ring_export_close(NULL);

	trace_ring_close();
	udebug_free(&ctx);
}

#else /* UVOICE_HAVE_UDEBUG */

/*
 * No udebug in libubox. The daemon keeps its syslog behaviour, the relay above
 * still takes the backend's lines, and every ring entry is refused with the
 * status the arm preflight reports.
 */
void trace_ring_init(void)
{
	/* The sink is installed here too, so the daemon has one wiring in
	 * both configurations; without a ring it drops every line. */
	trace_sink_set(trace_ring_log);
}

void trace_ring_done(void)
{
}

int trace_ring_open(bool with_rtp, bool with_lec)
{
	return -ENOSYS;
}

void trace_ring_close(void)
{
}

bool trace_ring_is_open(void)
{
	return false;
}

bool trace_ring_open_atomic(void)
{
	return false;
}

bool trace_ring_registered(void)
{
	return false;
}

void trace_ring_log(enum trace_domain dom, const char *msg)
{
}

void trace_relay_drain(void)
{
}

int trace_ring_packet(enum trace_ring_id ring, enum trace_pkt_dir dir,
		      enum trace_pkt_kind kind,
		      const struct trace_endpoint *src,
		      const struct trace_endpoint *dst,
		      uint64_t stamp_us, const void *data, size_t len)
{
	return -ENOSYS;
}

int trace_ring_status(const char *json)
{
	return -ENOSYS;
}

int trace_ring_lec(const struct trace_lec_hdr *hdr, uint64_t stamp_us,
		   const int16_t *ref, const int16_t *mic, const int16_t *out)
{
	return -ENOSYS;
}

void trace_ring_counts(enum trace_ring_id ring, struct trace_ring_counts *out)
{
	if (out)
		memset(out, 0, sizeof(*out));
}

int trace_ring_export_open(const char *path)
{
	return -ENOSYS;
}

int trace_ring_export_step(unsigned int max_entries, bool *done)
{
	if (done)
		*done = true;

	return -ENOSYS;
}

int trace_ring_export_close(uint64_t *file_bytes)
{
	if (file_bytes)
		*file_bytes = 0;

	return -ENOSYS;
}

void trace_ring_export_ring_enable(enum trace_ring_id ring, bool on)
{
}

uint64_t trace_ring_export_bytes(void)
{
	return 0;
}

uint64_t trace_ring_export_ring_bytes(enum trace_ring_id ring)
{
	return 0;
}

#endif /* UVOICE_HAVE_UDEBUG */
