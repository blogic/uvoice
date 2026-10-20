// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the ring mask of src/trace.c, the rings, the relay, the
 * packet builder and the canceller entry of src/trace_ring.c, and the writer
 * of src/pcapng.c.
 *
 * What the suite reads. The rings are private to trace_ring.c, so every check
 * of an entry goes through the exporter. The suite exports into a file of its
 * own run under $TMPDIR, or /tmp when it is unset. It parses the file back
 * block by block with a parser of its own, so it checks the file format as
 * well as the content. The parser reads native endian words: the writer emits
 * them, and the byte-order magic of the section header declares them.
 *
 * The log. The suite opens the trace with the stdio channel on, so a line
 * that goes to syslog also goes to stderr. The capture below sends stderr to
 * a file for the length of one case. That is how a case tells apart the two
 * destinations of trace_emit().
 *
 * Without udebug support in libubox, the ring half of trace_ring.c is a set
 * of stubs. The mask cases of t_mask() and the geometry cases of t_defaults()
 * then run unchanged. Two groups replace the others: t_relay_stub() runs the
 * relay with no ring behind it, and t_stub() checks the stub contract.
 *
 * The suite follows the timing rule of the package. It drives the relay drain
 * with a uloop run to a deadline, never with a sleep.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libubox/uloop.h>

#include "test_util.h"
#include "test_uloop.h"
#include "trace.h"
#include "trace_ring.h"

static char work_pcap[PATH_MAX];

/* 300 lines into a relay of 256 leaves 44 drops. */
#define RELAY_PUTS	300

/* --------------------------------------------------------- the log capture */

static FILE *cap_file;
static int cap_fd = -1;

static void cap_restore(void)
{
	if (cap_fd < 0)
		return;

	fflush(stderr);
	dup2(cap_fd, STDERR_FILENO);
	close(cap_fd);
	cap_fd = -1;
}

static void cap_end(void)
{
	cap_restore();
	if (!cap_file)
		return;

	fclose(cap_file);
	cap_file = NULL;
}

/* stderr goes to a temporary file, which is where ulog's stdio channel then
 * writes. The first count puts stderr back. */
static void cap_begin(void)
{
	cap_end();
	cap_file = tmpfile();
	if (!cap_file)
		return;

	fflush(stderr);
	cap_fd = dup(STDERR_FILENO);
	dup2(fileno(cap_file), STDERR_FILENO);
}

static unsigned int cap_count(const char *needle)
{
	char line[512];
	unsigned int hits = 0;

	if (!cap_file)
		return 0;

	cap_restore();
	rewind(cap_file);
	while (fgets(line, sizeof(line), cap_file)) {
		if (strstr(line, needle))
			hits++;
	}

	return hits;
}

/* ------------------------------------------------------- the test's sink */

static unsigned int sink_calls;
static enum trace_domain sink_dom;
static char sink_msg[256];

static void sink_take(enum trace_domain dom, const char *msg)
{
	sink_calls++;
	sink_dom = dom;
	snprintf(sink_msg, sizeof(sink_msg), "%s", msg);
}

static unsigned int hook_calls;
static enum trace_domain hook_dom;
static bool hook_on;

static void hook_take(enum trace_domain dom, bool on)
{
	hook_calls++;
	hook_dom = dom;
	hook_on = on;
}

static void *relay_thread(void *arg)
{
	unsigned int i;

	for (i = 0; i < RELAY_PUTS; i++) {
		char line[64];

		snprintf(line, sizeof(line), "relay %u", i);
		trace_relay_put(LOG_DEBUG, line);
	}

	return NULL;
}

/* ------------------------------------------------------------- the masks */

static void t_mask(void)
{
	CASE("a ring-only domain is active and not on for an operator");
	trace_ring_set(1u << TRACE_SIP);
	CHECK(trace_domain_active(TRACE_SIP));
	CHECK(!trace_domain_get(TRACE_SIP));
	CHECK(trace_ring_mask_atomic() == (1u << TRACE_SIP));
	CHECK(trace_mask_atomic() == 0);

	CASE("a ring-only line reaches the sink alone");
	trace_sink_set(sink_take);
	sink_calls = 0;
	TRACE(TRACE_SIP, "hello %d", 7);
	CHECK_UINT(sink_calls, 1);
	CHECK_UINT(sink_dom, TRACE_SIP);
	CHECK(!strcmp(sink_msg, "hello 7"));

	CASE("a ring-only line stays out of the log");
	cap_begin();
	TRACE(TRACE_SIP, "ring only line");
	trace_domain_set(TRACE_FSM, true);
	TRACE(TRACE_FSM, "operator line");
	CHECK_UINT(cap_count("ring only line"), 0);
	CHECK_UINT(cap_count("[fsm] operator line"), 1);
	cap_end();
	trace_domain_set(TRACE_FSM, false);

	CASE("a domain no mask carries reaches nothing");
	sink_calls = 0;
	cap_begin();
	TRACE(TRACE_FSM, "quiet");
	CHECK_UINT(cap_count("quiet"), 0);
	cap_end();
	CHECK_UINT(sink_calls, 0);

	CASE("trace_ring_set(0) stops the sink");
	trace_ring_set(0);
	sink_calls = 0;
	TRACE(TRACE_SIP, "hello again");
	CHECK_UINT(sink_calls, 0);
	CHECK(!trace_domain_active(TRACE_SIP));
	CHECK(trace_ring_mask_atomic() == 0);

	CASE("an operator domain reaches the sink and the log together");
	trace_domain_set(TRACE_SIP, true);
	trace_ring_set(1u << TRACE_SIP);
	sink_calls = 0;
	cap_begin();
	TRACE(TRACE_SIP, "both");
	CHECK_UINT(cap_count("[sip] both"), 1);
	cap_end();
	CHECK_UINT(sink_calls, 1);
	CHECK(trace_domain_get(TRACE_SIP));
	CHECK(trace_mask_atomic() == (1u << TRACE_SIP));

	CASE("the ring mask released leaves the operator domain on");
	trace_ring_set(0);
	CHECK(trace_domain_active(TRACE_SIP));
	sink_calls = 0;
	cap_begin();
	TRACE(TRACE_SIP, "operator only");
	CHECK_UINT(cap_count("[sip] operator only"), 1);
	cap_end();
	CHECK_UINT(sink_calls, 0);
	trace_domain_set(TRACE_SIP, false);
	trace_sink_set(trace_ring_log);

	CASE("the hook fires once per domain the ring mask moved");
	trace_hook_set(hook_take);
	trace_domain_set(TRACE_FSM, true);
	hook_calls = 0;
	trace_ring_set((1u << TRACE_SIP) | (1u << TRACE_FSM));
	/* TRACE_FSM was active through the runtime mask, so it did not move. */
	CHECK_UINT(hook_calls, 1);
	CHECK_UINT(hook_dom, TRACE_SIP);
	CHECK(hook_on);

	CASE("the hook fires again when the ring mask goes away");
	hook_calls = 0;
	trace_ring_set(0);
	CHECK_UINT(hook_calls, 1);
	CHECK_UINT(hook_dom, TRACE_SIP);
	CHECK(!hook_on);

	CASE("a ring mask that changes nothing fires nothing");
	trace_ring_set(1u << TRACE_FSM);
	hook_calls = 0;
	trace_ring_set(1u << TRACE_FSM);
	CHECK_UINT(hook_calls, 0);
	trace_ring_set(0);
	trace_domain_set(TRACE_FSM, false);
	trace_hook_set(NULL);

	CASE("trace_domain_active answers on each of the four domain bits");
	trace_ring_set((1u << TRACE_SIP) | (1u << TRACE_MEDIA) |
		       (1u << TRACE_JB) | (1u << TRACE_HAL));
	CHECK(trace_domain_active(TRACE_SIP));
	CHECK(trace_domain_active(TRACE_MEDIA));
	CHECK(trace_domain_active(TRACE_JB));
	CHECK(trace_domain_active(TRACE_HAL));
	CHECK(!trace_domain_get(TRACE_SIP));
	CHECK(!trace_domain_get(TRACE_MEDIA));
	CHECK(!trace_domain_get(TRACE_JB));
	CHECK(!trace_domain_get(TRACE_HAL));
	trace_ring_set(0);

	CASE("a bit above the domain set is dropped");
	trace_ring_set(0xffffffffu);
	CHECK(trace_ring_mask_atomic() == (1u << TRACE_DOMAIN_MAX) - 1u);
	CHECK(!trace_domain_active(TRACE_DOMAIN_MAX));
	trace_ring_set(0);
}

/* ----------------------------------------------------------- the defaults */

static void t_defaults(void)
{
	CASE("the log ring holds 8192 entries in 1 MiB");
	CHECK_UINT(TRACE_RING_LOG_ENTRIES, 8192);
	CHECK_UINT(TRACE_RING_LOG_SIZE, 1024 * 1024);

	CASE("the sip ring holds 4096 entries in 2 MiB");
	CHECK_UINT(TRACE_RING_SIP_ENTRIES, 4096);
	CHECK_UINT(TRACE_RING_SIP_SIZE, 2 * 1024 * 1024);

	CASE("the status ring holds 256 entries in 1 MiB");
	CHECK_UINT(TRACE_RING_STATUS_ENTRIES, 256);
	CHECK_UINT(TRACE_RING_STATUS_SIZE, 1024 * 1024);

	CASE("the rtp ring holds 8192 entries in 4 MiB");
	CHECK_UINT(TRACE_RING_RTP_ENTRIES, 8192);
	CHECK_UINT(TRACE_RING_RTP_SIZE, 4 * 1024 * 1024);

	CASE("the lec ring holds 4096 entries in 4 MiB");
	CHECK_UINT(TRACE_RING_LEC_ENTRIES, 4096);
	CHECK_UINT(TRACE_RING_LEC_SIZE, 4 * 1024 * 1024);

	CASE("the entry caps are 16 KiB, 2048 and 256 KiB");
	CHECK_UINT(CALLTRACE_SIP_ENTRY_MAX, 16 * 1024);
	CHECK_UINT(CALLTRACE_RTP_ENTRY_MAX, 2048);
	CHECK_UINT(CALLTRACE_STATUS_ENTRY_MAX, 256 * 1024);

	/* The header and three planes of the widest line, which is why no
	 * canceller entry is ever cut. */
	CASE("the lec entry cap is 1024");
	CHECK_UINT(CALLTRACE_LEC_ENTRY_MAX, 1024);

	CASE("the relay is 256 lines of 256 bytes, drained every 100 ms");
	CHECK_UINT(TRACE_RELAY_LINES, 256);
	CHECK_UINT(TRACE_RELAY_LINE_MAX, 256);
	CHECK_UINT(TRACE_RELAY_DRAIN_MS, 100);
}

#ifdef UVOICE_HAVE_UDEBUG

/* The pcapng block and option codes the parser below knows. */
#define BTYPE_SHB	0x0a0d0d0au
#define BTYPE_IDB	1u
#define BTYPE_EPB	6u
#define BYTE_ORDER_MAGIC 0x1a2b3c4du
#define OPT_END		0u
#define OPT_IF_NAME	2u
#define OPT_TSRESOL	9u

#define IFACE_MAX	8
#define PKT_MAX		1024

/* The two entry widths the overrun cases write. Each entry costs its length
 * plus the terminator udebug adds, so 4096 and 8192 bytes of the ring. */
#define FILL_4K		4095
#define FILL_8K		8191

/* ------------------------------------------------------- the file parser */

struct pcap_iface {
	uint16_t link_type;
	uint8_t tsresol;
	uint32_t snaplen;
	char name[32];
};

struct pcap_pkt {
	uint32_t iface;
	uint64_t stamp;
	uint32_t len;
	const uint8_t *data;
};

static uint8_t *fbuf;
static size_t flen;
static struct pcap_iface ifaces[IFACE_MAX];
static unsigned int n_ifaces;
static struct pcap_pkt pkts[PKT_MAX];
static unsigned int n_pkts;
static bool have_shb;
static uint64_t shb_section_len;

static uint16_t rd_u16(const uint8_t *p)
{
	uint16_t val;

	memcpy(&val, p, sizeof(val));

	return val;
}

static uint32_t rd_u32(const uint8_t *p)
{
	uint32_t val;

	memcpy(&val, p, sizeof(val));

	return val;
}

static uint64_t rd_u64(const uint8_t *p)
{
	uint64_t val;

	memcpy(&val, p, sizeof(val));

	return val;
}

static bool pad_is_zero(const uint8_t *p, size_t len, size_t pad)
{
	size_t i;

	for (i = len; i < pad; i++) {
		if (p[i])
			return false;
	}

	return true;
}

/* Every option is a four byte header, its value and the padding to the next
 * four byte boundary. An option that runs past the block, or padding that is
 * not zero, fails the parse instead of ending it quietly. */
static bool opts_parse(const uint8_t *p, size_t len, struct pcap_iface *ifc)
{
	size_t at = 0;

	while (at + 4 <= len) {
		uint16_t code = rd_u16(&p[at]);
		uint16_t olen = rd_u16(&p[at + 2]);
		size_t pad = ((size_t)olen + 3u) & ~(size_t)3u;

		if (code == OPT_END)
			return olen == 0;

		if (at + 4 + pad > len)
			return false;
		if (!pad_is_zero(&p[at + 4], olen, pad))
			return false;

		if (code == OPT_IF_NAME && olen && olen < sizeof(ifc->name))
			memcpy(ifc->name, &p[at + 4], olen);
		if (code == OPT_TSRESOL && olen == 1)
			ifc->tsresol = p[at + 4];

		at += 4 + pad;
	}

	return at == len;
}

static bool block_take(uint32_t type, const uint8_t *body, size_t len)
{
	struct pcap_iface *ifc;
	struct pcap_pkt *pkt;
	size_t pad;

	if (type == BTYPE_SHB) {
		have_shb = len >= 16 && rd_u32(body) == BYTE_ORDER_MAGIC &&
			   rd_u16(&body[4]) == 1 && rd_u16(&body[6]) == 0;
		if (have_shb)
			shb_section_len = rd_u64(&body[8]);
		return have_shb;
	}

	if (type == BTYPE_IDB) {
		if (len < 8 || n_ifaces >= IFACE_MAX)
			return false;

		ifc = &ifaces[n_ifaces++];
		memset(ifc, 0, sizeof(*ifc));
		ifc->link_type = rd_u16(body);
		ifc->snaplen = rd_u32(&body[4]);
		return opts_parse(&body[8], len - 8, ifc);
	}

	if (type != BTYPE_EPB)
		return true;

	if (len < 20 || n_pkts >= PKT_MAX)
		return false;

	pkt = &pkts[n_pkts++];
	pkt->iface = rd_u32(body);
	pkt->stamp = ((uint64_t)rd_u32(&body[4]) << 32) | rd_u32(&body[8]);
	pkt->len = rd_u32(&body[12]);
	pkt->data = &body[20];

	/* The captured and the original length are one number here: the
	 * producer cuts an entry before the writer sees it. */
	if (rd_u32(&body[16]) != pkt->len)
		return false;

	pad = ((size_t)pkt->len + 3u) & ~(size_t)3u;
	if (20 + pad > len)
		return false;

	return pad_is_zero(&body[20], pkt->len, pad);
}

static bool file_parse(const char *path)
{
	size_t pos = 0;
	long size;
	FILE *f;

	free(fbuf);
	fbuf = NULL;
	flen = 0;
	n_ifaces = 0;
	n_pkts = 0;
	have_shb = false;
	shb_section_len = 0;

	f = fopen(path, "r");
	if (!f)
		return false;

	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
		fclose(f);
		return false;
	}

	rewind(f);
	flen = (size_t)size;
	fbuf = malloc(flen ? flen : 1);
	if (!fbuf || (flen && fread(fbuf, 1, flen, f) != flen)) {
		fclose(f);
		return false;
	}
	fclose(f);

	while (pos + 12 <= flen) {
		uint32_t type = rd_u32(&fbuf[pos]);
		uint32_t len = rd_u32(&fbuf[pos + 4]);

		if (len < 12 || (len & 3u) || pos + len > flen)
			return false;
		if (rd_u32(&fbuf[pos + len - 4]) != len)
			return false;
		if (!block_take(type, &fbuf[pos + 8], len - 12))
			return false;

		pos += len;
	}

	return pos == flen;
}

/* The nth entry of one interface. A packet that is not in the file answers
 * this empty one, so a missing entry fails an assertion rather than the
 * process. */
static const uint8_t pkt_none_data[64];
static const struct pcap_pkt pkt_none = { .data = pkt_none_data };

static const struct pcap_pkt *pkt_of_iface(unsigned int iface, unsigned int nth)
{
	unsigned int seen = 0;
	unsigned int i;

	for (i = 0; i < n_pkts; i++) {
		if (pkts[i].iface != iface)
			continue;
		if (seen++ == nth)
			return &pkts[i];
	}

	return &pkt_none;
}

static bool pkt_text_is(const struct pcap_pkt *pkt, const char *want)
{
	size_t len = strlen(want);

	if (pkt->len != len)
		return false;

	return memcmp(pkt->data, want, len) == 0;
}

/* ------------------------------------------------- the synthesized headers */

static const uint8_t v4_src[4] = { 192, 0, 2, 1 };
static const uint8_t v4_dst[4] = { 198, 51, 100, 7 };

static void endpoint_v4(struct trace_endpoint *ep, const uint8_t addr[4],
			uint16_t port)
{
	memset(ep, 0, sizeof(*ep));
	ep->ipver = 4;
	ep->port = port;
	memcpy(ep->addr, addr, 4);
}

static void endpoint_v6(struct trace_endpoint *ep, uint8_t last, uint16_t port)
{
	memset(ep, 0, sizeof(*ep));
	ep->ipver = 6;
	ep->port = port;
	/* 2001:db8::<last>, the documentation prefix of RFC 3849. */
	ep->addr[0] = 0x20;
	ep->addr[1] = 0x01;
	ep->addr[2] = 0x0d;
	ep->addr[3] = 0xb8;
	ep->addr[15] = last;
}

/* The suite's own ones-complement sum. Over a whole header, checksum field
 * included, a correct header answers zero (RFC 1071). */
static uint16_t sum16(const uint8_t *p, size_t len)
{
	uint32_t sum = 0;
	size_t i;

	for (i = 0; i + 1 < len; i += 2)
		sum += ((uint32_t)p[i] << 8) | p[i + 1];

	while (sum >> 16)
		sum = (sum & 0xffffu) + (sum >> 16);

	return (uint16_t)~sum;
}

static uint16_t be16_of(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* --------------------------------------------- the sink the init installs */

/*
 * The first group of the run, and nothing has installed a sink of its own
 * when it starts: what it measures is the wiring trace_ring_init left behind.
 */
static void t_default_sink(void)
{
	CASE("the init installs the ring writer as the trace sink");
	CHECK_UINT(trace_ring_open(false, false), 0);
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	trace_ring_set(1u << TRACE_FSM);
	TRACE(TRACE_FSM, "state %s -> %s", "IDLE", "SEIZED");
	trace_ring_set(0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 1);
	CHECK(pkt_text_is(pkt_of_iface(0, 0), "[fsm] state IDLE -> SEIZED"));
	trace_ring_close();
}

/* --------------------------------------------------------- the ring entry */

static void t_rings(void)
{
	struct trace_ring_counts counts;
	struct trace_endpoint src, dst;
	char *big_json;

	CASE("the rings open, and a second open answers busy");
	CHECK(!trace_ring_is_open());
	CHECK(!trace_ring_open_atomic());
	CHECK_UINT(trace_ring_open(false, false), 0);
	CHECK(trace_ring_is_open());
	CHECK(trace_ring_open_atomic());
	CHECK_UINT(trace_ring_open(false, false), -EBUSY);

	CASE("the sink writes one log entry per line with its domain");
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	trace_ring_set(1u << TRACE_SIP);
	TRACE(TRACE_SIP, "tx %d bytes", 42);
	TRACE(TRACE_SIP, "rx keepalive");
	trace_ring_set(0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 2);
	CHECK(pkt_text_is(pkt_of_iface(0, 0), "[sip] tx 42 bytes"));
	CHECK(pkt_text_is(pkt_of_iface(0, 1), "[sip] rx keepalive"));
	trace_ring_counts(TRACE_RING_LOG, &counts);
	CHECK_UINT(counts.written, 2);
	CHECK_UINT(counts.exported, 2);
	CHECK_UINT(counts.overwritten, 0);

	CASE("a status snapshot is one entry of the status ring");
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	CHECK_UINT(trace_ring_status("{\"kind\":\"start\"}"), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 1);
	CHECK(pkt_text_is(pkt_of_iface(2, 0), "{\"kind\":\"start\"}"));
	trace_ring_counts(TRACE_RING_STATUS, &counts);
	CHECK_UINT(counts.written, 1);

	CASE("a snapshot above the entry cap is cut there and counted");
	big_json = malloc(CALLTRACE_STATUS_ENTRY_MAX + 101);
	CHECK(big_json != NULL);
	memset(big_json, 'j', CALLTRACE_STATUS_ENTRY_MAX + 100);
	big_json[CALLTRACE_STATUS_ENTRY_MAX + 100] = '\0';
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	CHECK_UINT(trace_ring_status(big_json), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 1);
	CHECK_UINT(pkt_of_iface(2, 0)->len, CALLTRACE_STATUS_ENTRY_MAX);
	trace_ring_counts(TRACE_RING_STATUS, &counts);
	CHECK_UINT(counts.written, 2);
	CHECK_UINT(counts.truncated, 1);
	free(big_json);

	CASE("the rtp ring stays closed unless the arm asks for it");
	endpoint_v4(&src, v4_src, 4000);
	endpoint_v4(&dst, v4_dst, 4002);
	CHECK_UINT(trace_ring_packet(TRACE_RING_RTP, TRACE_PKT_TX,
				     TRACE_PKT_RTP, &src, &dst, 0, "ab", 2),
		   -ENOENT);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_ifaces, 3);

	CASE("close frees the rings and the next open works");
	trace_ring_close();
	CHECK(!trace_ring_is_open());
	CHECK(!trace_ring_open_atomic());
	CHECK_UINT(trace_ring_open(true, false), 0);
	CHECK(trace_ring_is_open());
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_ifaces, 4);
	trace_ring_close();

	CASE("a line that arrives while the rings are closed is dropped");
	trace_ring_log(TRACE_SIP, "nobody hears this");
	trace_ring_counts(TRACE_RING_LOG, &counts);
	CHECK_UINT(counts.written, 0);
	CHECK_UINT(trace_ring_status("{}"), -ENOENT);

	CASE("the open drops what the last trace left in the relay");
	trace_relay_put(LOG_ERR, "line of the trace that ended");
	trace_relay_drain();
	CHECK_UINT(trace_ring_open(false, false), 0);
	CHECK_UINT(trace_relay_dropped(), 0);
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	trace_relay_drain();
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 0);
	trace_ring_counts(TRACE_RING_LOG, &counts);
	CHECK_UINT(counts.written, 0);
	trace_ring_close();
}

/* ------------------------------------------------------------- the relay */

static void t_relay(void)
{
	struct trace_ring_counts counts;
	uint64_t dropped_before;
	pthread_t thread;
	unsigned int i;
	bool ordered = true;

	CASE("the relay takes lines from another thread and drops the rest");
	CHECK_UINT(trace_ring_open(false, false), 0);
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	dropped_before = trace_relay_dropped();
	CHECK_UINT(pthread_create(&thread, NULL, relay_thread, NULL), 0);
	CHECK_UINT(pthread_join(thread, NULL), 0);
	CHECK_UINT(trace_relay_dropped() - dropped_before,
		   RELAY_PUTS - TRACE_RELAY_LINES);

	CASE("the drain lands the lines it took in order in the log ring");
	/* The drain is a 100 ms uloop timer; two periods leave it room. */
	uloop_pump(250);
	CHECK_UINT(trace_ring_export_step(TRACE_RELAY_LINES + 8, NULL), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, TRACE_RELAY_LINES);
	for (i = 0; i < TRACE_RELAY_LINES; i++) {
		char want[64];

		snprintf(want, sizeof(want), "[hal] relay %u", i);
		if (!pkt_text_is(pkt_of_iface(0, i), want))
			ordered = false;
	}
	CHECK(ordered);
	trace_ring_counts(TRACE_RING_LOG, &counts);
	CHECK_UINT(counts.written, TRACE_RELAY_LINES);

	CASE("a warning and an error carry their priority word");
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	trace_relay_put(LOG_WARNING, "engine hold");
	trace_relay_put(LOG_ERR, "slic fault");
	trace_relay_put(LOG_INFO, "plain");
	uloop_pump(150);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 3);
	CHECK(pkt_text_is(pkt_of_iface(0, 0), "[hal] warn: engine hold"));
	CHECK(pkt_text_is(pkt_of_iface(0, 1), "[hal] err: slic fault"));
	CHECK(pkt_text_is(pkt_of_iface(0, 2), "[hal] plain"));

	/*
	 * The line the export takes with it. A backend thread can queue a
	 * line between the last drain tick and the freeze, and the file must
	 * carry it: the close drains before it walks.
	 */
	CASE("the export close takes the last line of the relay");
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	trace_relay_put(LOG_ERR, "engine stopped");
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 1);
	CHECK(pkt_text_is(pkt_of_iface(0, 0), "[hal] err: engine stopped"));

	CASE("close drains the relay once more before it frees the rings");
	trace_relay_put(LOG_DEBUG, "last line of the trace");
	trace_ring_counts(TRACE_RING_LOG, &counts);
	CHECK_UINT(counts.written, TRACE_RELAY_LINES + 4);
	trace_ring_close();
	trace_ring_counts(TRACE_RING_LOG, &counts);
	CHECK_UINT(counts.written, TRACE_RELAY_LINES + 5);
	CHECK_UINT(trace_relay_dropped() - dropped_before,
		   RELAY_PUTS - TRACE_RELAY_LINES);
}

/* ---------------------------------------------------------- the packets */

static void t_packet(void)
{
	struct trace_ring_counts counts;
	struct trace_endpoint src, dst;
	const struct pcap_pkt *pkt;
	static uint8_t big[CALLTRACE_SIP_ENTRY_MAX + 100];
	const char *invite = "INVITE sip:1@example.net SIP/2.0\r\n\r\n";
	const uint8_t *ip;

	CASE("a tx sip message over ipv4 carries a valid header and its ports");
	CHECK_UINT(trace_ring_open(true, false), 0);
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	endpoint_v4(&src, v4_src, 5060);
	endpoint_v4(&dst, v4_dst, 5070);
	CHECK_UINT(trace_ring_packet(TRACE_RING_SIP, TRACE_PKT_TX,
				     TRACE_PKT_SIP_UDP, &src, &dst,
				     1700000000123456ull, invite,
				     strlen(invite)), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 1);
	pkt = pkt_of_iface(1, 0);
	CHECK_UINT(pkt->stamp, 1700000000123456ull);
	CHECK_UINT(pkt->len, 20 + 8 + strlen(invite));
	ip = pkt->data;
	CHECK_UINT(ip[0], 0x45);
	CHECK_UINT(ip[1], 0x00);		/* tx, sip over udp */
	CHECK_UINT(be16_of(&ip[2]), 20 + 8 + strlen(invite));
	CHECK_UINT(ip[8], 64);			/* ttl */
	CHECK_UINT(ip[9], 17);			/* udp */
	CHECK_UINT(sum16(ip, 20), 0);		/* the header checks out */
	CHECK(!memcmp(&ip[12], v4_src, 4));
	CHECK(!memcmp(&ip[16], v4_dst, 4));
	CHECK_UINT(be16_of(&ip[20]), 5060);
	CHECK_UINT(be16_of(&ip[22]), 5070);
	CHECK_UINT(be16_of(&ip[24]), 8 + strlen(invite));
	CHECK_UINT(be16_of(&ip[26]), 0);	/* no udp checksum */
	CHECK(!memcmp(&ip[28], invite, strlen(invite)));

	CASE("the dscp byte carries the direction and the kind");
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	CHECK_UINT(trace_ring_packet(TRACE_RING_SIP, TRACE_PKT_RX,
				     TRACE_PKT_SIP_TLS, &src, &dst, 0,
				     invite, strlen(invite)), 0);
	CHECK_UINT(trace_ring_packet(TRACE_RING_RTP, TRACE_PKT_RX,
				     TRACE_PKT_RTCP, &src, &dst, 0,
				     invite, strlen(invite)), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 2);
	/* rx is 1 in bits 7..6, the kind sits in bits 5..2 */
	CHECK_UINT(pkt_of_iface(1, 0)->data[1], (1u << 6) | (2u << 2));
	CHECK_UINT(pkt_of_iface(3, 0)->data[1], (1u << 6) | (4u << 2));

	CASE("a packet stamp of zero takes the clock of the entry");
	CHECK(pkt_of_iface(1, 0)->stamp > 1700000000000000ull);

	CASE("an ipv6 message carries the version, class and hop limit");
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	endpoint_v6(&src, 1, 5060);
	endpoint_v6(&dst, 2, 5070);
	CHECK_UINT(trace_ring_packet(TRACE_RING_SIP, TRACE_PKT_RX,
				     TRACE_PKT_SIP_TCP, &src, &dst, 0,
				     invite, strlen(invite)), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 1);
	ip = pkt_of_iface(1, 0)->data;
	CHECK_UINT(pkt_of_iface(1, 0)->len, 40 + 8 + strlen(invite));
	/* version 6 and the traffic class straddle the first two bytes */
	CHECK_UINT(ip[0] >> 4, 6);
	CHECK_UINT(((ip[0] & 0x0fu) << 4) | (ip[1] >> 4),
		   (1u << 6) | (1u << 2));
	/* the flow label, the twenty bits under the class */
	CHECK_UINT(ip[1] & 0x0fu, 0);
	CHECK_UINT(be16_of(&ip[2]), 0);
	CHECK_UINT(be16_of(&ip[4]), 8 + strlen(invite));
	CHECK_UINT(ip[6], 17);			/* next header udp */
	CHECK_UINT(ip[7], 64);			/* hop limit */
	CHECK_UINT(ip[8], 0x20);
	CHECK_UINT(ip[23], 1);
	CHECK_UINT(ip[39], 2);
	CHECK_UINT(be16_of(&ip[40]), 5060);
	CHECK_UINT(be16_of(&ip[42]), 5070);
	CHECK_UINT(be16_of(&ip[44]), 8 + strlen(invite));

	CASE("a sip message above the entry cap is cut there and counted");
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	memset(big, 'A', sizeof(big));
	endpoint_v4(&src, v4_src, 5060);
	endpoint_v4(&dst, v4_dst, 5070);
	CHECK_UINT(trace_ring_packet(TRACE_RING_SIP, TRACE_PKT_TX,
				     TRACE_PKT_SIP_TCP, &src, &dst, 0, big,
				     sizeof(big)), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 1);
	CHECK_UINT(pkt_of_iface(1, 0)->len, 20 + 8 + CALLTRACE_SIP_ENTRY_MAX);
	CHECK_UINT(be16_of(&pkt_of_iface(1, 0)->data[2]),
		   20 + 8 + CALLTRACE_SIP_ENTRY_MAX);
	trace_ring_counts(TRACE_RING_SIP, &counts);
	CHECK_UINT(counts.truncated, 1);

	CASE("the rtp ring cuts at its own cap");
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	CHECK_UINT(trace_ring_packet(TRACE_RING_RTP, TRACE_PKT_TX,
				     TRACE_PKT_RTP, &src, &dst, 0, big,
				     sizeof(big)), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 1);
	CHECK_UINT(pkt_of_iface(3, 0)->len, 20 + 8 + CALLTRACE_RTP_ENTRY_MAX);
	trace_ring_counts(TRACE_RING_RTP, &counts);
	CHECK_UINT(counts.truncated, 1);

	CASE("a bad ring and a bad address version are refused");
	CHECK_UINT(trace_ring_packet(TRACE_RING_LOG, TRACE_PKT_TX,
				     TRACE_PKT_RTP, &src, &dst, 0, big, 4),
		   -EINVAL);
	CHECK_UINT(trace_ring_packet(TRACE_RING_STATUS, TRACE_PKT_TX,
				     TRACE_PKT_RTP, &src, &dst, 0, big, 4),
		   -EINVAL);
	src.ipver = 5;
	CHECK_UINT(trace_ring_packet(TRACE_RING_SIP, TRACE_PKT_TX,
				     TRACE_PKT_RTP, &src, &dst, 0, big, 4),
		   -EINVAL);
	src.ipver = 6;
	CHECK_UINT(trace_ring_packet(TRACE_RING_SIP, TRACE_PKT_TX,
				     TRACE_PKT_RTP, &src, &dst, 0, big, 4),
		   -EINVAL);

	CASE("a packet for a closed ring answers not found");
	endpoint_v4(&src, v4_src, 5060);
	trace_ring_close();
	CHECK_UINT(trace_ring_packet(TRACE_RING_SIP, TRACE_PKT_TX,
				     TRACE_PKT_SIP_UDP, &src, &dst, 0, big, 4),
		   -ENOENT);
}

/* ------------------------------------------------------------ the export */

/* One snapshot of exactly len bytes: the sequence first, filler to the end.
 * The entry costs len + 1 bytes of the ring, so the two widths below divide
 * the 1 MiB status ring evenly. */
static void json_fill(char *buf, size_t len, unsigned int seq)
{
	int n = snprintf(buf, len + 1, "{\"seq\":%u,\"pad\":\"", seq);

	memset(&buf[n], 'p', len - (size_t)n);
	buf[len - 2] = '"';
	buf[len - 1] = '}';
	buf[len] = '\0';
}

static bool pkt_seq_is(const struct pcap_pkt *pkt, unsigned int seq)
{
	char want[24];
	size_t len;

	snprintf(want, sizeof(want), "{\"seq\":%u,", seq);
	len = strlen(want);

	return pkt->len > len && !memcmp(pkt->data, want, len);
}

static void t_export(void)
{
	static char fill[FILL_8K + 1];
	struct trace_ring_counts before;
	struct trace_ring_counts counts;
	bool over_budget = false;
	uint64_t bytes = 0;
	unsigned int i;
	bool done;

	CASE("the file names one interface per open ring, with its link type");
	CHECK_UINT(trace_ring_open(true, false), 0);
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	CHECK_UINT(trace_ring_export_close(&bytes), 0);
	CHECK(bytes > 0);
	CHECK(file_parse(work_pcap));
	CHECK(have_shb);
	/* The section length is unknown while the file grows. */
	CHECK_UINT(shb_section_len, ~0ull);
	CHECK_UINT(flen, bytes);
	CHECK_UINT(n_ifaces, 4);
	CHECK_UINT(n_pkts, 0);
	CHECK_UINT(ifaces[0].link_type, 147);
	CHECK_UINT(ifaces[1].link_type, 101);
	CHECK_UINT(ifaces[2].link_type, 147);
	CHECK_UINT(ifaces[3].link_type, 101);
	CHECK(!strcmp(ifaces[0].name, "uvoiced_log"));
	CHECK(!strcmp(ifaces[1].name, "uvoiced_sip"));
	CHECK(!strcmp(ifaces[2].name, "uvoiced_status"));
	CHECK(!strcmp(ifaces[3].name, "uvoiced_rtp"));
	CHECK_UINT(ifaces[0].tsresol, 6);
	CHECK_UINT(ifaces[3].tsresol, 6);

	CASE("every interface declares no snapshot limit");
	CHECK_UINT(ifaces[0].snaplen, 0);
	CHECK_UINT(ifaces[1].snaplen, 0);
	CHECK_UINT(ifaces[2].snaplen, 0);
	CHECK_UINT(ifaces[3].snaplen, 0);

	CASE("a second export while one is open answers busy");
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	CHECK_UINT(trace_ring_export_open(work_pcap), -EBUSY);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK_UINT(trace_ring_export_close(NULL), -ENOENT);
	CHECK_UINT(trace_ring_export_step(8, &done), -ENOENT);

	CASE("a walk stops at its budget and the next one carries on");
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	for (i = 0; i < 10; i++)
		trace_ring_log(TRACE_FSM, "one line");
	CHECK_UINT(trace_ring_export_step(4, &done), 0);
	CHECK(!done);
	trace_ring_counts(TRACE_RING_LOG, &counts);
	CHECK_UINT(counts.exported, 4);
	CHECK_UINT(trace_ring_export_step(64, &done), 0);
	CHECK(done);
	trace_ring_counts(TRACE_RING_LOG, &counts);
	CHECK_UINT(counts.exported, 10);
	CHECK_UINT(trace_ring_export_close(&bytes), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 10);
	CHECK_UINT(flen, bytes);

	/*
	 * The data the ring reused. 300 entries of 4 KiB are 1200 KiB through
	 * a 1 MiB ring, so the 44 oldest lose their bytes and the 45th is the
	 * first that is still whole. The count does not depend on the page
	 * size the ring was rounded to, because the loss is measured in bytes.
	 */
	CASE("entries the producer overwrote are counted and skipped");
	trace_ring_close();
	CHECK_UINT(trace_ring_open(false, false), 0);
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	for (i = 0; i < 300; i++) {
		json_fill(fill, FILL_4K, i);
		CHECK_UINT(trace_ring_status(fill), 0);
	}
	CHECK_UINT(trace_ring_export_step(4096, &done), 0);
	CHECK(done);
	trace_ring_counts(TRACE_RING_STATUS, &counts);
	CHECK_UINT(counts.written, 300);
	CHECK_UINT(counts.overwritten, 44);
	CHECK_UINT(counts.exported, 256);
	CHECK_UINT(counts.truncated, 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 256);
	CHECK(pkt_seq_is(pkt_of_iface(2, 0), 44));
	CHECK(pkt_seq_is(pkt_of_iface(2, 1), 45));
	CHECK_UINT(pkt_of_iface(2, 0)->len, FILL_4K);

	/*
	 * The loss is a scan and the budget bounds it. 200 entries of 8 KiB
	 * lose 72 to the same 1 MiB ring and leave every slot in place, so
	 * each step of one entry inspects exactly one.
	 */
	CASE("a step of one entry skips at most one lost entry");
	trace_ring_close();
	CHECK_UINT(trace_ring_open(false, false), 0);
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	for (i = 0; i < 200; i++) {
		json_fill(fill, FILL_8K, i);
		CHECK_UINT(trace_ring_status(fill), 0);
	}
	for (i = 0; i < 10; i++) {
		trace_ring_counts(TRACE_RING_STATUS, &before);
		CHECK_UINT(trace_ring_export_step(1, &done), 0);
		CHECK(!done);
		trace_ring_counts(TRACE_RING_STATUS, &counts);
		if (counts.overwritten + counts.exported >
		    before.overwritten + before.exported + 1)
			over_budget = true;
	}
	CHECK(!over_budget);
	CHECK_UINT(counts.overwritten, 10);
	CHECK_UINT(counts.exported, 0);
	CHECK_UINT(trace_ring_export_step(4096, &done), 0);
	CHECK(done);
	trace_ring_counts(TRACE_RING_STATUS, &counts);
	CHECK_UINT(counts.overwritten, 72);
	CHECK_UINT(counts.exported, 128);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 128);
	CHECK(pkt_seq_is(pkt_of_iface(2, 0), 72));

	CASE("an export into an unwritable path fails with its errno");
	CHECK_UINT(trace_ring_export_open("/tmp"), -EISDIR);
	CHECK_UINT(trace_ring_export_open("/dev/null/x.pcapng"), -ENOTDIR);
	trace_ring_counts(TRACE_RING_STATUS, &counts);
	CHECK_UINT(counts.written, 200);
	CHECK_UINT(counts.exported, 128);

	CASE("a device with no room fails the export");
	CHECK_UINT(trace_ring_export_open("/dev/full"), 0);
	CHECK_UINT(trace_ring_status("{\"seq\":\"nowhere\"}"), 0);
	CHECK(trace_ring_export_close(&bytes) < 0);
	trace_ring_counts(TRACE_RING_STATUS, &counts);
	CHECK_UINT(counts.written, 201);

	CASE("the export works again after a failed one");
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	CHECK_UINT(trace_ring_status("{\"seq\":\"after\"}"), 0);
	CHECK_UINT(trace_ring_export_close(&bytes), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 1);
	CHECK(pkt_text_is(pkt_of_iface(2, 0), "{\"seq\":\"after\"}"));
	CHECK_UINT(flen, bytes);
	CHECK_UINT(trace_ring_export_bytes(), bytes);
	trace_ring_close();

	/*
	 * A caller that has to bound the file stops one ring and lets the
	 * others run. The stopped ring keeps its cursor, so nothing it holds
	 * past that point reaches the file, and the close counts what it left
	 * behind as truncated.
	 */
	CASE("a stopped ring exports nothing more and the others go on");
	CHECK_UINT(trace_ring_open(false, false), 0);
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	CHECK_UINT(trace_ring_status("{\"seq\":\"before the stop\"}"), 0);
	CHECK_UINT(trace_ring_export_step(64, &done), 0);
	CHECK(done);

	trace_ring_export_ring_enable(TRACE_RING_STATUS, false);
	CHECK_UINT(trace_ring_status("{\"seq\":\"after the stop\"}"), 0);
	trace_ring_set(1u << TRACE_FSM);
	TRACE(TRACE_FSM, "a line the log ring still exports");
	trace_ring_set(0);
	CHECK_UINT(trace_ring_export_step(64, &done), 0);
	CHECK(done);
	CHECK_UINT(trace_ring_export_close(&bytes), 0);

	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 2);
	CHECK(pkt_text_is(pkt_of_iface(2, 0), "{\"seq\":\"before the stop\"}"));
	CHECK(pkt_text_is(pkt_of_iface(0, 0),
			  "[fsm] a line the log ring still exports"));

	CASE("and the close counts what the stopped ring kept");
	trace_ring_counts(TRACE_RING_STATUS, &counts);
	CHECK_UINT(counts.truncated, 1);
	CHECK_UINT(counts.exported, 1);
	CHECK_UINT(counts.written, 2);

	CASE("a new file starts every ring again");
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	CHECK_UINT(trace_ring_status("{\"seq\":\"a new file\"}"), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 1);
	CHECK(pkt_text_is(pkt_of_iface(2, 0), "{\"seq\":\"a new file\"}"));
	trace_ring_close();

	/*
	 * A producer goes on filling a ring whose export stopped, so the
	 * close has two things to say about it: what the head passed is
	 * overwritten and what survived is truncated. One entry is counted
	 * under one name, and the two together are what the producer wrote.
	 */
	CASE("a stopped ring separates what it lost from what it kept");
	CHECK_UINT(trace_ring_open(false, false), 0);
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	trace_ring_export_ring_enable(TRACE_RING_STATUS, false);
	for (i = 0; i < 300; i++) {
		json_fill(fill, 64, i);
		CHECK_UINT(trace_ring_status(fill), 0);
	}
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	trace_ring_counts(TRACE_RING_STATUS, &counts);
	CHECK_UINT(counts.written, 300);
	CHECK_UINT(counts.exported, 0);
	CHECK_UINT(counts.overwritten, 300 - TRACE_RING_STATUS_ENTRIES);
	CHECK_UINT(counts.truncated, TRACE_RING_STATUS_ENTRIES);
	trace_ring_close();

	CASE("a ring started again inside one file exports from there on");
	CHECK_UINT(trace_ring_open(false, false), 0);
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	trace_ring_export_ring_enable(TRACE_RING_STATUS, false);
	CHECK_UINT(trace_ring_status("{\"seq\":\"while stopped\"}"), 0);
	CHECK_UINT(trace_ring_export_step(64, &done), 0);
	trace_ring_export_ring_enable(TRACE_RING_STATUS, true);
	CHECK_UINT(trace_ring_status("{\"seq\":\"after the resume\"}"), 0);
	CHECK_UINT(trace_ring_export_step(64, &done), 0);
	CHECK(done);
	CHECK_UINT(trace_ring_export_close(NULL), 0);

	/* The entry written while it was stopped is behind the cursor the
	 * resume kept, so the file carries both and neither is lost. */
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 2);
	CHECK(pkt_text_is(pkt_of_iface(2, 0), "{\"seq\":\"while stopped\"}"));
	CHECK(pkt_text_is(pkt_of_iface(2, 1), "{\"seq\":\"after the resume\"}"));
	trace_ring_counts(TRACE_RING_STATUS, &counts);
	CHECK_UINT(counts.truncated, 0);
	trace_ring_close();
}

/* ------------------------------------------------------- the canceller ring */

/*
 * The offsets of the entry header: the table of trace_ring.h, which is the
 * table of udsp/src/tools/lec-capture.py. The suite reads each field at its
 * offset and not through the struct, so a field that moved or a header in
 * another byte order fails here.
 */
#define LEC_OFF_TICK		8
#define LEC_OFF_LOST		16
#define LEC_OFF_FRAMES		20
#define LEC_OFF_HOLD		24
#define LEC_OFF_ERLE		28
#define LEC_OFF_GAIN		32
#define LEC_OFF_ENERGY		36
#define LEC_OFF_DELAY		40
#define LEC_OFF_GEN		44
#define LEC_OFF_FLAGS		46
#define LEC_OFF_SAMPLES		48
#define LEC_OFF_TAIL		50
#define LEC_OFF_NOISE		52
#define LEC_OFF_PROBES		54
#define LEC_OFF_RESETS		56
#define LEC_OFF_LINE		58
#define LEC_OFF_KIND		59
#define LEC_OFF_VERDICT		60
#define LEC_OFF_HOLD_LEFT	61
#define LEC_OFF_SKIPPED		62
#define LEC_HDR_BYTES		64

/* The fifth ring of five, so the fifth interface of the file. */
#define LEC_IFACE		4u
#define LEC_STAMP_US		1700000000654321ull

static int16_t lec_ref[TRACE_LEC_SAMPLES_WB];
static int16_t lec_mic[TRACE_LEC_SAMPLES_WB];
static int16_t lec_out[TRACE_LEC_SAMPLES_WB];

/* Every field takes a value of its own, and the multi-byte ones differ in
 * each byte, so a swap of two fields or of the byte order cannot read as
 * correct. */
static void lec_hdr_make(struct trace_lec_hdr *hdr, uint8_t kind,
			 uint16_t samples)
{
	memset(hdr, 0, sizeof(*hdr));
	hdr->stamp_ns = 0x0102030405060708ull;
	hdr->tick = 0x1112131415161718ull;
	hdr->lost_before = 0x21222324u;
	hdr->frames = 0x31323334u;
	hdr->hold_frames = 0x41424344u;
	hdr->erle_mdb = -123456;
	hdr->echo_gain_micro = 0x51525354u;
	hdr->coef_energy_micro = 0x61626364u;
	hdr->ref_delay_samples = 0x71727374u;
	hdr->gen = 0x8182;
	hdr->flags = 0x1041;
	hdr->samples = samples;
	hdr->tail_ms = 0x9192;
	hdr->noise_ddbov = 1270;
	hdr->probes = 0xa1a2;
	hdr->diverge_resets = 0xb1b2;
	hdr->line = 1;
	hdr->kind = kind;
	hdr->nlp_verdict = 5;
	hdr->hold_left = 0xc1;
	hdr->skipped = 0xd1d2;
}

static void lec_planes_fill(void)
{
	unsigned int i;

	for (i = 0; i < TRACE_LEC_SAMPLES_WB; i++) {
		lec_ref[i] = (int16_t)(i + 1);
		lec_mic[i] = (int16_t)(1000 + i);
		lec_out[i] = (int16_t)(-(int)i - 1);
	}
}

static size_t lec_entry_bytes(unsigned int planes, uint16_t samples)
{
	return LEC_HDR_BYTES + (size_t)planes * samples * sizeof(int16_t);
}

static int32_t rd_i32(const uint8_t *p)
{
	int32_t val;

	memcpy(&val, p, sizeof(val));

	return val;
}

/* One entry against the header it was written from, field by field at the
 * offsets of the table. */
static void lec_hdr_check(const struct pcap_pkt *pkt,
			  const struct trace_lec_hdr *want)
{
	const uint8_t *e = pkt->data;

	CHECK(pkt->len >= LEC_HDR_BYTES);
	if (pkt->len < LEC_HDR_BYTES)
		return;

	CHECK_UINT(rd_u64(e), want->stamp_ns);
	CHECK_UINT(rd_u64(&e[LEC_OFF_TICK]), want->tick);
	CHECK_UINT(rd_u32(&e[LEC_OFF_LOST]), want->lost_before);
	CHECK_UINT(rd_u32(&e[LEC_OFF_FRAMES]), want->frames);
	CHECK_UINT(rd_u32(&e[LEC_OFF_HOLD]), want->hold_frames);
	CHECK(rd_i32(&e[LEC_OFF_ERLE]) == want->erle_mdb);
	CHECK_UINT(rd_u32(&e[LEC_OFF_GAIN]), want->echo_gain_micro);
	CHECK_UINT(rd_u32(&e[LEC_OFF_ENERGY]), want->coef_energy_micro);
	CHECK_UINT(rd_u32(&e[LEC_OFF_DELAY]), want->ref_delay_samples);
	CHECK_UINT(rd_u16(&e[LEC_OFF_GEN]), want->gen);
	CHECK_UINT(rd_u16(&e[LEC_OFF_FLAGS]), want->flags);
	CHECK_UINT(rd_u16(&e[LEC_OFF_SAMPLES]), want->samples);
	CHECK_UINT(rd_u16(&e[LEC_OFF_TAIL]), want->tail_ms);
	CHECK_UINT(rd_u16(&e[LEC_OFF_NOISE]), want->noise_ddbov);
	CHECK_UINT(rd_u16(&e[LEC_OFF_PROBES]), want->probes);
	CHECK_UINT(rd_u16(&e[LEC_OFF_RESETS]), want->diverge_resets);
	CHECK_UINT(e[LEC_OFF_LINE], want->line);
	CHECK_UINT(e[LEC_OFF_KIND], want->kind);
	CHECK_UINT(e[LEC_OFF_VERDICT], want->nlp_verdict);
	CHECK_UINT(e[LEC_OFF_HOLD_LEFT], want->hold_left);
	CHECK_UINT(rd_u16(&e[LEC_OFF_SKIPPED]), want->skipped);
}

/* The planes follow the header in the order ref, mic, out. */
static bool lec_plane_is(const struct pcap_pkt *pkt, unsigned int nth,
			 const int16_t *want, uint16_t samples)
{
	size_t bytes = (size_t)samples * sizeof(int16_t);
	size_t at = LEC_HDR_BYTES + nth * bytes;

	if (pkt->len < at + bytes)
		return false;

	return !memcmp(&pkt->data[at], want, bytes);
}

static void t_lec(void)
{
	static const uint16_t bad_samples[] = { 0, 79, 81, 120, 159, 161, 320 };
	struct trace_ring_counts counts;
	struct trace_lec_hdr hdr;
	const struct pcap_pkt *pkt;
	uint64_t lec_bytes = 0;
	unsigned int i;
	bool refused = true;
	bool done;

	lec_planes_fill();

	CASE("the lec ring stays closed unless the open asks for it");
	CHECK_UINT(trace_ring_open(true, false), 0);
	lec_hdr_make(&hdr, TRACE_LEC_RUN, TRACE_LEC_SAMPLES_WB);
	CHECK_UINT(trace_ring_lec(&hdr, LEC_STAMP_US, lec_ref, lec_mic,
				  lec_out), -ENOENT);
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_ifaces, 4);
	trace_ring_counts(TRACE_RING_LEC, &counts);
	CHECK_UINT(counts.written, 0);
	trace_ring_close();

	CASE("the export names five interfaces, the fifth uvoiced_lec at DLT "
	     "148");
	CHECK_UINT(trace_ring_open(true, true), 0);
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_ifaces, 5);
	CHECK_UINT(ifaces[LEC_IFACE].link_type, 148);
	CHECK(!strcmp(ifaces[LEC_IFACE].name, "uvoiced_lec"));
	CHECK_UINT(ifaces[LEC_IFACE].tsresol, 6);
	CHECK_UINT(ifaces[LEC_IFACE].snaplen, 0);
	CHECK(!strcmp(ifaces[3].name, "uvoiced_rtp"));

	CASE("a RUN entry carries the header and three planes");
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	lec_hdr_make(&hdr, TRACE_LEC_RUN, TRACE_LEC_SAMPLES_WB);
	CHECK_UINT(trace_ring_lec(&hdr, LEC_STAMP_US, lec_ref, lec_mic,
				  lec_out), 0);
	hdr.samples = TRACE_LEC_SAMPLES_NB;
	CHECK_UINT(trace_ring_lec(&hdr, LEC_STAMP_US, lec_ref, lec_mic,
				  lec_out), 0);
	CHECK_UINT(trace_ring_export_step(64, &done), 0);
	lec_bytes = trace_ring_export_ring_bytes(TRACE_RING_LEC);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 2);
	CHECK_UINT(pkt_of_iface(LEC_IFACE, 0)->len, lec_entry_bytes(3, 160));
	CHECK_UINT(pkt_of_iface(LEC_IFACE, 0)->len, 1024);
	CHECK_UINT(pkt_of_iface(LEC_IFACE, 1)->len, lec_entry_bytes(3, 80));
	CHECK_UINT(pkt_of_iface(LEC_IFACE, 1)->len, 544);

	/* Each block is its fixed part, the entry padded to four bytes and
	 * the trailer (pcapng section 4.3). */
	CASE("the export counts the bytes of the canceller blocks apart");
	CHECK_UINT(lec_bytes, (28 + 1024 + 4) + (28 + 544 + 4));

	CASE("the entry reads back field by field against the offset table");
	pkt = pkt_of_iface(LEC_IFACE, 0);
	lec_hdr_make(&hdr, TRACE_LEC_RUN, TRACE_LEC_SAMPLES_WB);
	lec_hdr_check(pkt, &hdr);
	CHECK_UINT(pkt->stamp, LEC_STAMP_US);

	CASE("the three planes follow the header in order");
	CHECK(lec_plane_is(pkt, 0, lec_ref, TRACE_LEC_SAMPLES_WB));
	CHECK(lec_plane_is(pkt, 1, lec_mic, TRACE_LEC_SAMPLES_WB));
	CHECK(lec_plane_is(pkt, 2, lec_out, TRACE_LEC_SAMPLES_WB));
	pkt = pkt_of_iface(LEC_IFACE, 1);
	CHECK(lec_plane_is(pkt, 2, lec_out, TRACE_LEC_SAMPLES_NB));

	CASE("a FEED entry carries the header and two planes");
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	lec_hdr_make(&hdr, TRACE_LEC_FEED, TRACE_LEC_SAMPLES_WB);
	CHECK_UINT(trace_ring_lec(&hdr, LEC_STAMP_US, lec_ref, lec_mic, NULL),
		   0);
	hdr.samples = TRACE_LEC_SAMPLES_NB;
	CHECK_UINT(trace_ring_lec(&hdr, LEC_STAMP_US, lec_ref, lec_mic,
				  lec_out), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 2);
	pkt = pkt_of_iface(LEC_IFACE, 0);
	CHECK_UINT(pkt->len, 704);
	lec_hdr_make(&hdr, TRACE_LEC_FEED, TRACE_LEC_SAMPLES_WB);
	lec_hdr_check(pkt, &hdr);
	CHECK(lec_plane_is(pkt, 0, lec_ref, TRACE_LEC_SAMPLES_WB));
	CHECK(lec_plane_is(pkt, 1, lec_mic, TRACE_LEC_SAMPLES_WB));
	CHECK_UINT(pkt_of_iface(LEC_IFACE, 1)->len, 384);

	CASE("an END entry carries the header alone");
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	lec_hdr_make(&hdr, TRACE_LEC_END, TRACE_LEC_SAMPLES_NB);
	CHECK_UINT(trace_ring_lec(&hdr, LEC_STAMP_US, NULL, NULL, NULL), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK_UINT(n_pkts, 1);
	pkt = pkt_of_iface(LEC_IFACE, 0);
	CHECK_UINT(pkt->len, 64);
	lec_hdr_check(pkt, &hdr);

	CASE("samples other than 80 or 160 are refused");
	trace_ring_counts(TRACE_RING_LEC, &counts);
	for (i = 0; i < sizeof(bad_samples) / sizeof(bad_samples[0]); i++) {
		lec_hdr_make(&hdr, TRACE_LEC_RUN, bad_samples[i]);
		if (trace_ring_lec(&hdr, 0, lec_ref, lec_mic, lec_out) !=
		    -EINVAL)
			refused = false;
		lec_hdr_make(&hdr, TRACE_LEC_END, bad_samples[i]);
		if (trace_ring_lec(&hdr, 0, NULL, NULL, NULL) != -EINVAL)
			refused = false;
	}
	CHECK(refused);

	CASE("a kind past END and a missing plane are refused");
	lec_hdr_make(&hdr, TRACE_LEC_END + 1, TRACE_LEC_SAMPLES_NB);
	CHECK_UINT(trace_ring_lec(&hdr, 0, lec_ref, lec_mic, lec_out),
		   -EINVAL);
	lec_hdr_make(&hdr, TRACE_LEC_RUN, TRACE_LEC_SAMPLES_NB);
	CHECK_UINT(trace_ring_lec(&hdr, 0, lec_ref, lec_mic, NULL), -EINVAL);
	lec_hdr_make(&hdr, TRACE_LEC_FEED, TRACE_LEC_SAMPLES_NB);
	CHECK_UINT(trace_ring_lec(&hdr, 0, NULL, lec_mic, NULL), -EINVAL);
	CHECK_UINT(trace_ring_lec(NULL, 0, lec_ref, lec_mic, lec_out),
		   -EINVAL);

	CASE("a refused entry writes nothing");
	{
		struct trace_ring_counts after;

		trace_ring_counts(TRACE_RING_LEC, &after);
		CHECK_UINT(after.written, counts.written);
	}

	CASE("a lec stamp of zero takes the clock of the entry");
	CHECK_UINT(trace_ring_export_open(work_pcap), 0);
	lec_hdr_make(&hdr, TRACE_LEC_END, TRACE_LEC_SAMPLES_WB);
	CHECK_UINT(trace_ring_lec(&hdr, 0, NULL, NULL, NULL), 0);
	CHECK_UINT(trace_ring_export_close(NULL), 0);
	CHECK(file_parse(work_pcap));
	CHECK(pkt_of_iface(LEC_IFACE, 0)->stamp > 1700000000000000ull);

	CASE("an entry for a closed ring answers not found");
	trace_ring_close();
	lec_hdr_make(&hdr, TRACE_LEC_RUN, TRACE_LEC_SAMPLES_NB);
	CHECK_UINT(trace_ring_lec(&hdr, LEC_STAMP_US, lec_ref, lec_mic,
				  lec_out), -ENOENT);
}

#else /* UVOICE_HAVE_UDEBUG */

/* ------------------------------------------------------ the stub contract */

static void t_relay_stub(void)
{
	uint64_t dropped;
	pthread_t thread;

	CASE("without udebug the relay still takes lines and drops the rest");
	dropped = trace_relay_dropped();
	CHECK_UINT(pthread_create(&thread, NULL, relay_thread, NULL), 0);
	CHECK_UINT(pthread_join(thread, NULL), 0);
	CHECK_UINT(trace_relay_dropped() - dropped,
		   RELAY_PUTS - TRACE_RELAY_LINES);

	CASE("a drain with no ring to write to does not crash");
	trace_relay_drain();
	trace_relay_put(LOG_ERR, "and this one too");
}

static void t_stub(void)
{
	struct trace_ring_counts counts;
	struct trace_endpoint ep;
	uint64_t bytes = 1;
	bool done = false;

	memset(&ep, 0, sizeof(ep));
	ep.ipver = 4;
	ep.port = 5060;

	CASE("without udebug the rings refuse to open");
	CHECK_UINT(trace_ring_open(false, false), -ENOSYS);
	CHECK(!trace_ring_is_open());
	CHECK(!trace_ring_open_atomic());
	CHECK(!trace_ring_registered());

	CASE("without udebug every entry answers not implemented");
	CHECK_UINT(trace_ring_packet(TRACE_RING_SIP, TRACE_PKT_TX,
				     TRACE_PKT_SIP_UDP, &ep, &ep, 0, "x", 1),
		   -ENOSYS);
	CHECK_UINT(trace_ring_status("{}"), -ENOSYS);
	CHECK_UINT(trace_ring_lec(NULL, 0, NULL, NULL, NULL), -ENOSYS);
	CHECK_UINT(trace_ring_export_open(work_pcap), -ENOSYS);
	CHECK_UINT(trace_ring_export_step(8, &done), -ENOSYS);
	CHECK(done);
	CHECK_UINT(trace_ring_export_close(&bytes), -ENOSYS);
	CHECK_UINT(bytes, 0);
	CHECK_UINT(trace_ring_export_bytes(), 0);

	CASE("without udebug the counts read zero and the close is safe");
	trace_ring_log(TRACE_SIP, "nobody hears this");
	trace_ring_close();
	trace_ring_counts(TRACE_RING_LOG, &counts);
	CHECK_UINT(counts.written, 0);
	CHECK_UINT(counts.exported, 0);
	CHECK_UINT(counts.overwritten, 0);
	CHECK_UINT(counts.truncated, 0);
}

#endif /* UVOICE_HAVE_UDEBUG */

/*
 * The export path. Two build trees can run this suite at the same time, so
 * each run writes a file of its own: its name holds the pid and the mkstemp()
 * suffix. A name that does not fit its buffer fails with its own errno.
 */
static void t_work_path(void)
{
	const char *tmp = getenv("TMPDIR");
	char keep[PATH_MAX] = "";
	char longdir[128];
	char path[64];
	char pid[32];
	bool had = tmp != NULL;
	bool ok;

	CASE("the export file belongs to this run");
	snprintf(pid, sizeof(pid), "-%ld-", (long)getpid());
	CHECK(strstr(work_pcap, pid) != NULL);
	CHECK(strstr(work_pcap, "XXXXXX") == NULL);
	CHECK(access(work_pcap, F_OK) == 0);

	CASE("a TMPDIR longer than the buffer fails with ENAMETOOLONG");
	if (had)
		snprintf(keep, sizeof(keep), "%s", tmp);
	memset(longdir, 'a', sizeof(longdir) - 1);
	longdir[0] = '/';
	longdir[sizeof(longdir) - 1] = 0;
	setenv("TMPDIR", longdir, 1);
	errno = 0;
	ok = tu_tmp_file_make(path, sizeof(path), "uvoice-test-trace-ring");
	CHECK(!ok);
	CHECK_UINT(errno, ENAMETOOLONG);
	if (had)
		setenv("TMPDIR", keep, 1);
	else
		unsetenv("TMPDIR");
}

int main(void)
{
	if (!tu_tmp_file_make(work_pcap, sizeof(work_pcap),
			      "uvoice-test-trace-ring")) {
		printf("Bail out! no scratch file: %s\n", strerror(errno));
		return 1;
	}

	/* A ring walk that never ends must fail the suite, not stall ctest. */
	alarm(60);
	uloop_init();
	/* The stdio channel is what the log capture above reads. */
	trace_init("test_trace_ring", false);
	trace_ring_init();

	t_work_path();
#ifdef UVOICE_HAVE_UDEBUG
	t_default_sink();
#endif
	t_mask();
#ifdef UVOICE_HAVE_UDEBUG
	t_rings();
	t_relay();
	t_packet();
	t_export();
	t_lec();
#else
	t_relay_stub();
	t_stub();
#endif
	t_defaults();

	trace_ring_done();
	uloop_done();
	trace_done();
#ifdef UVOICE_HAVE_UDEBUG
	free(fbuf);
#endif
	unlink(work_pcap);

	return tu_summary("test_trace_ring");
}
