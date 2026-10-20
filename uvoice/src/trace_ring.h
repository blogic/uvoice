// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The udebug rings of a call trace, the relay that carries the backend's log
 * lines onto the uloop thread, and the exporter that writes the rings into
 * one pcapng file.
 *
 * The rings exist only while a trace is armed or running. They are the live
 * buffer between two exports, not the archive: the controller exports them
 * every few seconds, so each ring holds more than one export period of its
 * worst case. Four counts per ring say what happened to it: written,
 * exported, overwritten before an export reached them, and truncated.
 *
 * A ring the daemon cannot register with udebugd still works locally. The
 * registration only decides whether a live reader ("udebug stream") can see
 * the ring while the trace runs; the export reads the daemon's own memory.
 *
 * One thread. Every udebug call runs on the uloop thread, which is what makes
 * the daemon the single producer udebug asks for. Four entries are the
 * exception and may be called from any thread: trace_relay_put,
 * trace_ring_open_atomic, and the two mask mirrors of trace.h
 * (trace_ring_mask_atomic and trace_mask_atomic). Nothing else here is safe
 * outside the loop.
 *
 * Without libubox udebug support (UVOICE_HAVE_UDEBUG unset) the ring half
 * answers -ENOSYS and the relay still takes lines, so a build without it
 * keeps the daemon's syslog behaviour and refuses an arm.
 */
#ifndef UVOICE_TRACE_RING_H
#define UVOICE_TRACE_RING_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

/* struct trace_endpoint and enum trace_pkt_kind, which the protocol fills */
#include "proto/proto_tap.h"
#include "trace_domain.h"

enum trace_ring_id {
	TRACE_RING_LOG,
	TRACE_RING_SIP,
	TRACE_RING_STATUS,
	TRACE_RING_RTP,
	TRACE_RING_LEC,
	__TRACE_RING_MAX
};

/*
 * The ring geometry. Each pair is the entry count and the data size of one
 * ring. udebug rounds both up to a power of two and may take more entries
 * than it was asked for, so these are the floor and not the answer.
 */
#define TRACE_RING_LOG_ENTRIES		8192
#define TRACE_RING_LOG_SIZE		(1024 * 1024)
#define TRACE_RING_SIP_ENTRIES		4096
#define TRACE_RING_SIP_SIZE		(2 * 1024 * 1024)
#define TRACE_RING_STATUS_ENTRIES	256
#define TRACE_RING_STATUS_SIZE		(1024 * 1024)
#define TRACE_RING_RTP_ENTRIES		8192
#define TRACE_RING_RTP_SIZE		(4 * 1024 * 1024)
/* Two lines at 16 kHz write 200 records a second, so the ring holds about
 * 20 s of them between two exports. */
#define TRACE_RING_LEC_ENTRIES		4096
#define TRACE_RING_LEC_SIZE		(4 * 1024 * 1024)

/*
 * The widest payload one entry carries. A SIP message can reach the 64 KiB the
 * receive buffer allows, and four of those would take a fifth of the ring; the
 * entry is cut at the cap instead and counted in truncated. An RTP payload
 * above the media cap is a malformed datagram. A snapshot follows the same
 * rule with a cap of its own, which also keeps it under the half-ring udebug
 * takes for one entry (512 KiB of the status ring's 1 MiB).
 */
#define CALLTRACE_SIP_ENTRY_MAX		(16 * 1024)
#define CALLTRACE_RTP_ENTRY_MAX		2048
#define CALLTRACE_STATUS_ENTRY_MAX	(256 * 1024)
/* A canceller entry is never cut: a record without its planes cannot be
 * replayed, so trace_ring_lec refuses a frame wider than the widest line and
 * this is the entry that frame makes. */
#define CALLTRACE_LEC_ENTRY_MAX		1024

/* The relay: 256 lines of 256 bytes, drained every 100 ms. */
#define TRACE_RELAY_LINES		256
#define TRACE_RELAY_LINE_MAX		256
#define TRACE_RELAY_DRAIN_MS		100

struct trace_ring_counts {
	uint64_t written;
	uint64_t exported;
	uint64_t overwritten;
	uint64_t truncated;
};

/*
 * One endpoint from a socket address. A family other than AF_INET and
 * AF_INET6 leaves the record zero. Inline, because the host table of
 * proto/proto.c and the recorder host of the suites both fill it, and a suite
 * links no ring.
 */
static inline void trace_endpoint_set(struct trace_endpoint *out,
				      const struct sockaddr *sa)
{
	memset(out, 0, sizeof(*out));
	if (sa->sa_family == AF_INET) {
		const struct sockaddr_in *v4 = (const struct sockaddr_in *)sa;

		out->ipver = 4;
		memcpy(out->addr, &v4->sin_addr, sizeof(v4->sin_addr));
		out->port = ntohs(v4->sin_port);
	} else if (sa->sa_family == AF_INET6) {
		const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)sa;

		out->ipver = 6;
		memcpy(out->addr, &v6->sin6_addr, sizeof(v6->sin6_addr));
		out->port = ntohs(v6->sin6_port);
	}
}

enum trace_pkt_dir {
	TRACE_PKT_TX,
	TRACE_PKT_RX,
};

/* The kind of a canceller entry, and the planes that follow its header. */
enum trace_lec_kind {
	TRACE_LEC_RUN,		/* ref, mic, out */
	TRACE_LEC_FEED,		/* ref, mic */
	TRACE_LEC_END,		/* none */
};

/* The two line frames a canceller entry carries: 8 kHz and 16 kHz. */
#define TRACE_LEC_SAMPLES_NB		80
#define TRACE_LEC_SAMPLES_WB		160

/*
 * The header of one canceller entry, the record header of
 * udsp/src/test/bench/lec_replay.h and of the udsp capture record, field for
 * field. udsp/src/tools/lec-capture.py reads it at these offsets, in the byte
 * order of the pcapng section, which is the byte order of the host:
 *
 *    offset  width  field
 *    0       8      stamp_ns, CLOCK_MONOTONIC of the tick
 *    8       8      tick, the engine tick index
 *    16      4      lost_before, records of the line lost before this one
 *    20      4      frames of the instance after the record
 *    24      4      hold_frames of the instance after the record
 *    28      4      erle_mdb, signed
 *    32      4      echo_gain_micro
 *    36      4      coef_energy_micro
 *    40      4      ref_delay_samples
 *    44      2      gen
 *    46      2      flags
 *    48      2      samples, 80 or 160
 *    50      2      tail_ms
 *    52      2      noise_ddbov, tenths of dB; 1270 is unmeasured
 *    54      2      probes
 *    56      2      diverge_resets
 *    58      1      line
 *    59      1      kind: 0 RUN, 1 FEED, 2 END
 *    60      1      nlp_verdict: 0 off, 1 quiet, 2 dt, 3 unlike, 4 erle, 5 pass
 *    61      1      hold_left
 *    62      2      skipped, the periods the engine skipped before this
 *                   record, 0 when none
 *
 * The flags and the verdict are the words of the udsp capture record, carried
 * as they are. This is a type of its own, and the backend copies into it one
 * field at a time, because the core includes no udsp header.
 */
struct trace_lec_hdr {
	uint64_t stamp_ns;
	uint64_t tick;
	uint32_t lost_before;
	uint32_t frames;
	uint32_t hold_frames;
	int32_t erle_mdb;
	uint32_t echo_gain_micro;
	uint32_t coef_energy_micro;
	uint32_t ref_delay_samples;
	uint16_t gen;
	uint16_t flags;
	uint16_t samples;
	uint16_t tail_ms;
	uint16_t noise_ddbov;
	uint16_t probes;
	uint16_t diverge_resets;
	uint8_t line;
	uint8_t kind;			/* enum trace_lec_kind */
	uint8_t nlp_verdict;
	uint8_t hold_left;
	uint16_t skipped;
};
_Static_assert(sizeof(struct trace_lec_hdr) == 64,
	       "the canceller entry header changed its size");
_Static_assert(offsetof(struct trace_lec_hdr, samples) == 48 &&
	       offsetof(struct trace_lec_hdr, line) == 58 &&
	       offsetof(struct trace_lec_hdr, skipped) == 62,
	       "the canceller entry header moved a field");

/*
 * Creates the udebug context, starts its connect, and installs trace_ring_log
 * as the trace sink for the life of the daemon. The sink stays installed
 * because it costs one load while the ring mask is empty, so the controller
 * only has to set the ring mask to make the lines flow.
 */
void trace_ring_init(void);
void trace_ring_done(void);

/*
 * Creates the rings, with the RTP one and the canceller one only when the
 * caller asks for each. 0, or -errno: -EBUSY while the rings are open,
 * -ENOSYS without udebug support, and the errno of the failed mapping
 * otherwise.
 *
 * The open empties the relay and the counts together, so a line a backend
 * thread queued after the last trace ended cannot land in this one, and the
 * status reply reads the counts of this trace alone. The counts stand after
 * the close, so the reply after a freeze still reads them.
 */
int trace_ring_open(bool with_rtp, bool with_lec);
/* Drains the relay once more and frees the rings. Run the final export
 * before it: the exporter reads these rings and nothing else. */
void trace_ring_close(void);
bool trace_ring_is_open(void);
/* The open state, for a thread outside the loop. Relaxed, like the two mask
 * mirrors of trace.h. */
bool trace_ring_open_atomic(void);
bool trace_ring_registered(void);

/* The sink trace.c calls for a domain the ring mask carries. One entry,
 * "[dom] msg". A line that arrives while the rings are closed is dropped:
 * the controller clears the ring mask before it closes them. */
void trace_ring_log(enum trace_domain dom, const char *msg);

/*
 * The relay. Any thread may call trace_relay_put: it copies the line under a
 * mutex held for the copy alone, and counts a drop when the 256 lines are
 * full. A 100 ms uloop timer moves the lines into the log ring as
 * "[hal] msg", with the priority word in front of a warning or an error.
 * prio is a syslog priority.
 */
void trace_relay_put(int prio, const char *msg);
uint64_t trace_relay_dropped(void);
/* Moves what the relay holds into the log ring now, on the uloop thread. The
 * export runs it before its final walk, so the last lines of a trace reach
 * the file. It does nothing while the rings are closed. */
void trace_relay_drain(void);

/*
 * One packet entry on the SIP or the RTP ring: a synthesized IPv4 or IPv6
 * header, a UDP header, then the payload. The endpoints give the addresses,
 * the ports and the version, which both ends must share. A TCP or a TLS
 * message rides UDP framing with its real ports, so one decoder serves every
 * SIP transport; the kind below says what it really was.
 *
 * The DSCP byte of the header (the IPv4 type of service, the IPv6 traffic
 * class) carries that metadata, because a udebug flag belongs to the ring and
 * not to one entry:
 *
 *   bits 7..6   the direction, 0 for tx and 1 for rx (enum trace_pkt_dir)
 *   bits 5..2   the kind (enum trace_pkt_kind)
 *   bits 1..0   zero
 *
 * stamp_us is the entry timestamp in microseconds since the epoch, and 0
 * means now. A payload above the cap of the ring is cut there and counted.
 * 0, or -errno: -ENOENT when the ring is not open, -EINVAL for a ring that
 * carries no packets or an address version that is not 4 or 6, -ENOSYS
 * without udebug support.
 */
int trace_ring_packet(enum trace_ring_id ring, enum trace_pkt_dir dir,
		      enum trace_pkt_kind kind,
		      const struct trace_endpoint *src,
		      const struct trace_endpoint *dst,
		      uint64_t stamp_us, const void *data, size_t len);

/* One snapshot, as the JSON text of it, on the status ring. Text above
 * CALLTRACE_STATUS_ENTRY_MAX is cut there and counted, the rule a packet
 * entry follows. 0, or -errno the way trace_ring_packet answers it. */
int trace_ring_status(const char *json);

/*
 * One entry on the canceller ring: the header, then the planes its kind
 * carries (ref, mic and out for a RUN, ref and mic for a FEED, none for an
 * END), each of hdr->samples int16 in host order. The entry is never cut, so
 * a frame that is not one of the two line frames is refused. stamp_us
 * follows the rule of trace_ring_packet. 0, or -errno: -EINVAL for samples
 * other than 80 or 160, a kind past END or a plane the kind needs and the
 * caller left NULL, -ENOENT when the ring is not open, -ENOSYS without udebug
 * support.
 */
int trace_ring_lec(const struct trace_lec_hdr *hdr, uint64_t stamp_us,
		   const int16_t *ref, const int16_t *mic, const int16_t *out);

void trace_ring_counts(enum trace_ring_id ring, struct trace_ring_counts *out);

/*
 * The exporter. It walks the daemon's own rings and writes one pcapng file:
 * one interface per open ring, one packet block per entry, with the entry's
 * own timestamp.
 *
 * trace_ring_export_open writes the header blocks and records each ring's
 * cursor at its current head, so the export starts at the entries that follow
 * it. trace_ring_export_step walks at most max_entries entries over all rings
 * and sets done when every ring is caught up; an entry the loss scan below
 * inspects counts against the same budget, so one walk never holds the loop.
 * trace_ring_export_close drains the relay, runs a last walk with no budget,
 * and closes the file. That walk is bounded by the geometry: it reads at most
 * the entry count of each open ring.
 *
 * A producer that outruns the export loses the oldest entries, by the slot
 * the head passed or by the data bytes the ring reused. The walk skips to the
 * oldest entry that survived and counts the loss in overwritten, so the file
 * states what it is missing.
 *
 * Every answer is 0 or -errno, and the errno of a failed write closes the
 * file. trace_ring_export_bytes is the size the file has reached, and
 * trace_ring_export_ring_bytes the part of it that the blocks of one ring
 * take. The controller stops the canceller ring against its own part and the
 * RTP ring against the rest.
 */
int trace_ring_export_open(const char *path);
int trace_ring_export_step(unsigned int max_entries, bool *done);
int trace_ring_export_close(uint64_t *file_bytes);
uint64_t trace_ring_export_bytes(void);
uint64_t trace_ring_export_ring_bytes(enum trace_ring_id ring);

/*
 * Stop or resume the export of one ring while the file stays open. A caller
 * that has to bound the file stops the ring whose entries it can spare and
 * lets the others run, so the cheap evidence survives the expensive one.
 *
 * A stopped ring keeps its cursor where it stands: the step and the final
 * walk both skip it, and what it holds past that cursor is counted into its
 * truncated count when the file closes. The file can therefore still grow
 * past the caller's bound by what the running rings hold, which is at most
 * the entry count of each of them.
 *
 * Every ring exports until a caller says otherwise, and an open of a new file
 * starts them all again.
 */
void trace_ring_export_ring_enable(enum trace_ring_id ring, bool on);

#endif
