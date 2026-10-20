// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Private contract between the two udsp backend translation units. Nothing
 * here leaves the backend: the core sees struct hal_ops and an opaque struct
 * hal only.
 *
 *
 * Threads
 *
 * one thread touches this backend: the uloop thread, which calls every op in
 * hal_ops and drains the event relay below. libudsp owns every thread of its
 * own (engine tick, slic, emitter) and none of them enters this file.
 *
 *
 * The event relay, and the one thing it exists for
 *
 * libudsp writes struct udsp_event records, and udsp_event is identical to
 * struct hal_event in meaning and in layout (event.h, hal.h section 2), so a
 * record needs no translation on its way to the core. The backend does not
 * hand libudsp the core's fd directly, though: it keeps a socketpair of its
 * own, gives libudsp the write end, and registers the read end with uloop.
 * Every record is then forwarded byte for byte to the core, with exactly two
 * exceptions, and the static asserts of udsp_events.c are what make a
 * byte-for-byte forward legal.
 *
 * The two exceptions are the halves of the bring-up. hal_ops.dsp_start must
 * report "ready" only once the line can actually carry a call, and a line with
 * no region carries no tone, no caller identity and no visual indicator
 * (libudsp refuses each of them without a tone plan). The region is a SLOW job
 * of its own, so the backend chains it: it consumes the JOB_DONE of
 * udsp_start, submits udsp_region_load for the configured region, consumes
 * that JOB_DONE too, and only then posts one HAL_EVENT_JOB_DONE to the core
 * under the token dsp_start returned. A region that fails still completes the
 * start successfully, with a warning: a missing tone table is a silent phone,
 * a failed bring-up is no phone at all (line/line.c region_done applies the
 * same rule to its own later load).
 *
 * No thread is added. The relay is a uloop_fd callback on the uloop thread,
 * which is where every op already runs, so the single-threaded rule of hal.h
 * section 2 stands and no lock exists in this backend.
 *
 * The core's own reader runs on that same thread, so nothing here may wait for
 * a full core fd. A wait would block the very loop that has to turn for the
 * core to drain that fd, so the two record classes take the two paths hal.h
 * section 2 gives them:
 *
 *   (a) a hardware event is dropped and counted, and nothing is retried.
 *   (b) a completion is never dropped, and it does not wait either. The record
 *       goes into the bounded queue below and the backend arms a ULOOP_WRITE
 *       watcher on the core fd; the loop keeps turning, the watcher fires when
 *       the core has read, and the queue drains oldest first. Both completion
 *       producers take that path: the records libudsp writes for its own jobs,
 *       and the one record the bring-up chain posts under the start token.
 *
 * A queued completion outranks a hardware event. Room that appears on the core
 * fd belongs to the record already waiting for it, so the forward path drains
 * the queue first and drops its own record while anything remains queued.
 * libudsp's own emitter orders its producers the same way (core event.c
 * ev_drain: "Completions first: a hardware burst must not delay one").
 *
 * The watcher itself can fail to register, and a queue with no watcher would
 * wait out the core's 30 s job backstop although the fd has room. The retry
 * timer covers that: it drains from the loop every HAL_UDSP_COMP_RETRY_MS
 * until the watcher registers or the queue empties.
 *
 * The queue is 16 deep, which the job model cannot fill: the core carries one
 * bring-up job, one region job and one stop job at a time, and each of them
 * completes before its successor is submitted. An overflow is therefore a
 * defect and not back pressure, so it is reported and dropped.
 *
 * The media plane, on top of the libudsp streams
 *
 * libudsp never creates a socket (udsp.h section 3). This backend
 * owns the RTP and RTCP descriptors of every stream: it makes them, binds them
 * to the advertised local pair, marks them from the two DSCP fields, connects
 * them to the far end and hands the numbers to udsp_stream_config, which dups
 * them for the library's net and engine threads. struct hal_udsp_stream below
 * is that ownership record.
 *
 * Both families and both RTCP peers are this backend's own work. It opens an
 * AF_INET6 pair with IPV6_V6ONLY for an IPv6 description, and it connects the
 * RTCP descriptor to the far end's own RTCP address when the description named
 * one. HAL_FEAT_IPV6_MEDIA and HAL_FEAT_RTCP_ADDR are the two capability bits
 * that state it.
 *
 * One additional thread rule applies and it is the library's, not this file's:
 * the descriptors are handed over at a tick boundary and libudsp holds its own
 * dups, so nothing here ever reads or writes a media socket. The single-thread
 * rule above is unchanged.
 */
#ifndef UVOICE_HAL_UDSP_INT_H
#define UVOICE_HAL_UDSP_INT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libubox/utils.h>

#include <udsp/udsp.h>

#include "../../uvoice.h"
#include "../hal.h"

/*
 * The line count file, which this backend writes at attach. The consumer is
 * /usr/srg/scripts and the ubus surface of the platform, which ask "how many
 * FXS ports does this unit really have" without talking to uvoiced (udsp.h:
 * the SLIC part number describes the silicon, the platform DB describes
 * the board, and cap.lines is the minimum of the two).
 */
#define HAL_UDSP_FXSNUM_RUN	"/var/run/voip.fxsnum"

/*
 * The region name the core derives is at most seven characters plus the
 * terminator (line/line.c LINE_REGION_MAX is 8), and UDSP_ISO3_MAX is the width
 * libudsp accepts. The two agree, so region_apply cannot truncate a name.
 */
#define HAL_UDSP_ISO3_MAX	UDSP_ISO3_MAX

/* The completion queue depth, see the relay note above. */
#define HAL_UDSP_COMP_MAX	16

/* The backend owns this bit for a dead stream. libudsp never sets it, and
 * udsp_events.c pins every library fault bit below it. Bit 8 stays unassigned
 * on both sides, so a consumer that reads bit 8 as a dead stream cannot
 * misread a library fault. */
#define HAL_FAULT_STREAM_DEAD	(1u << 16)

/*
 * The decision trace drain. libudsp writes its records into a
 * lock-free ring the engine tick fills and one consumer empties (udsp.h
 * udsp_jb_trace_read). This backend is that consumer, and it drains on a timer
 * rather than on an event, because the ring has no readable descriptor.
 *
 * The period is short enough that the 511 usable slots of the library ring
 * cannot fill at the worst case the library documents, and the round bound
 * stops a flood from holding the uloop: a burst that outruns both is reported
 * as lost by the record that follows it, which is the whole point of the
 * lost_before field.
 */
#define HAL_UDSP_TRACE_MS	100
#define HAL_UDSP_TRACE_BATCH	64u
#define HAL_UDSP_TRACE_ROUNDS	8u
/*
 * The packet trace drains on the same cadence and needs its own batch: one
 * record is 600 bytes, so the array is static and not a stack frame of the
 * uloop thread. The batch and the round bound together drain far more than one
 * period of every configured stream in both directions; a burst that outruns
 * them is reported as lost by the record that follows it, the rule the
 * decision trace follows.
 */
#define HAL_UDSP_PKT_BATCH	32u
#define HAL_UDSP_PKT_ROUNDS	8u
/*
 * The pre-arm discard has a bound of its own, and it is the capacity and not
 * the cadence. A disarm keeps the library's memory, so whatever the producers
 * left after the last drain is still there when the next trace arms, and a
 * bound that empties one period would export the backlog of the session before
 * it. The rings hold 1024 slots per direction (udsp/src/core/pkt_trace.h
 * UDSP_PKT_TRACE_SLOTS, of which 1023 are usable), so the bound covers both
 * directions plus the one short read that ends the loop.
 */
#define HAL_UDSP_PKT_SLOTS	1024u
#define HAL_UDSP_PKT_DISCARD_ROUNDS \
	((2u * HAL_UDSP_PKT_SLOTS) / HAL_UDSP_PKT_BATCH + 1u)
/*
 * The canceller capture drains on the same cadence, with a static batch for
 * the packet trace's reason: one record is 1024 bytes. One round drains 32
 * records and a period of two lines writes 20, so the round bound ends only
 * a flood, and a flood reports its losses in lost_before.
 */
#define HAL_UDSP_LEC_BATCH	32u
#define HAL_UDSP_LEC_ROUNDS	8u
/*
 * The discard before an arm and the drain after a disarm take the whole ring
 * and not one period, both for the reason the packet discard gives: the ring
 * can hold a backlog of its whole capacity. It holds 1024 slots
 * (udsp/src/core/lec_capture.h UDSP_LEC_CAPTURE_SLOTS, of which 1023 are
 * usable), so the bound covers it plus the one short read that ends the loop.
 */
#define HAL_UDSP_LEC_SLOTS	1024u
#define HAL_UDSP_LEC_RING_ROUNDS \
	(HAL_UDSP_LEC_SLOTS / HAL_UDSP_LEC_BATCH + 1u)
/*
 * The descriptions one stream keeps for the trace. A record carries the
 * generation the description had at the tap, and a record drained after a stop
 * names a description the stream no longer holds, so the endpoints of the last
 * four live here and the table outlives every socket. Four covers a call that
 * re-INVITEs three times inside one drain period.
 */
#define HAL_UDSP_PKT_GEN_MAX	4u
/* Every flag word of udsp.h plus its separator fits twice over. */
#define HAL_UDSP_TRACE_WORDS	96
/* G.722 carries 16 kHz audio on an 8 kHz RTP clock, so its jitter buffer
 * lateness keeps the same timestamp-to-millisecond conversion as G.711.
 * Opus uses a 48 kHz RTP clock, and the trace converts its lateness with this
 * same constant, so it reports Opus lateness six times too large. */
#define HAL_UDSP_TS_PER_MS	8

/*
 * The token space of the one job this backend completes itself.
 *
 * hal stream_stats_start is SLOW and libudsp has no slow op behind it: the
 * numbers are a FASTPATH double-buffer snapshot (udsp.h udsp_stream_stats),
 * so the backend reads them and posts the completion under a
 * token of its own. That token travels the same core list as the library's, so
 * the two spaces must not meet. libudsp counts its job tokens up from 1 and
 * only start, stop and region_load consume one, so the high half is
 * unreachable for it; this counter runs there and never leaves it.
 */
#define HAL_UDSP_STATS_TOKEN_BASE	0x80000000u
/*
 * The counter runs in the 31 bits below that base and wraps there, so every
 * token this backend emits keeps the reserved bit set however many snapshots a
 * long-lived daemon takes. A counter that simply climbed would leave the half
 * after 2^31 requests and start colliding with the library's own tokens.
 */
#define HAL_UDSP_STATS_TOKEN_MASK	0x7fffffffu
/* How long a queue with no write watcher waits before it tries again. Short
 * enough to be invisible against the core's 30 s job backstop, long enough not
 * to poll a healthy loop: the timer only ever runs when uloop_fd_add failed. */
#define HAL_UDSP_COMP_RETRY_MS	50

/*
 * A DSCP this backend has not successfully put on a socket yet. It is outside
 * the 0..63 a DSCP can hold, so "nothing is on this socket" can never read as
 * a value some description asked for: a mark that failed must leave the record
 * saying so, or the same-value early return of media_tos_apply would decide
 * the socket is already marked and never try again.
 */
#define HAL_UDSP_DSCP_NONE	0xffu

/*
 * The media sockets of one stream, and why the record is split in two.
 *
 * rtp_fd and rtcp_fd are the descriptors this backend made. A stream with no
 * sockets holds -1 in both: the calloc zero would name descriptor 0, which is
 * the daemon's stdin, so udsp_backend_open sets them before any op runs.
 *
 * sock_local, the two remote records and the two DSCP values say what the
 * sockets carry; configured says what the last successful program asked for. A
 * program that fails after the library call keeps its sockets, so the two must
 * be free to disagree, and every decision about rebinding, reconnecting or
 * re-marking reads the socket record and never the program.
 *
 * stats is the last snapshot stream_stats_start took. It outlives the stream
 * on purpose: the core reads it back with stream_stats_result_get after the
 * stop the snapshot ran in front of (line/line.c media_stats_done).
 */
struct hal_udsp_stream {
	bool configured;
	bool running;
	int rtp_fd;
	int rtcp_fd;
	struct hal_addr sock_local;	/* what the pair is bound to */
	struct hal_addr sock_remote;	/* what the rtp socket connects to */
	/*
	 * What the rtcp socket connects to. It is the far end's own RTCP
	 * address when the description named one (RFC 3605 section 2.1) and
	 * the media address otherwise, so a description that moved that
	 * address alone still reads as a moved far end.
	 */
	struct hal_addr sock_rtcp_remote;
	/*
	 * Part of the local identity, not of the program. libudsp never binds
	 * anything, so SO_BINDTODEVICE reaches the wire only through the
	 * socket this backend made: a description that changed the device
	 * alone would otherwise keep sending out of the old one.
	 */
	char sock_bind_dev[HAL_IFNAME_MAX];
	/* HAL_UDSP_DSCP_NONE until a mark actually succeeds. */
	uint8_t sock_rtp_dscp;
	uint8_t sock_rtcp_dscp;
	struct hal_media_stats stats;
};

/*
 * One description of one stream, as the packet trace renders it: the
 * generation the library raised for it and the three endpoints the backend
 * bound and connected under it. The rtcp record carries the far end's own RTCP
 * address when the description named one (RFC 3605 section 2.1).
 */
struct hal_udsp_gen {
	uint32_t gen;
	struct hal_addr local;
	struct hal_addr remote;
	struct hal_addr rtcp_remote;
};

/*
 * The descriptions of one stream, newest replacing oldest. count is how many
 * of the entries carry a description and next is the slot the following one
 * takes, so the newest entry is the slot before next.
 */
struct hal_udsp_gen_table {
	struct hal_udsp_gen entry[HAL_UDSP_PKT_GEN_MAX];
	uint8_t count;
	uint8_t next;
};

struct hal {
	udsp_t *s;
	struct hal_cap cap;

	/* The core's write end. hal.h: the backend never closes it. */
	int core_fd;
	/* The write end libudsp got, and the read end this backend drains. */
	int lib_fd;
	struct uloop_fd relay;
	/* The same core fd, watched for writability while comp_len is not 0. */
	struct uloop_fd core_out;
	/* Armed only while that watcher could not be registered. */
	struct uloop_timeout comp_retry;

	/* Completions the core fd could not take, oldest at comp_head. */
	struct hal_event comp[HAL_UDSP_COMP_MAX];
	unsigned int comp_head;
	unsigned int comp_len;

	/* The bring-up chain. region is the name the initial load carries,
	 * empty when the config named none. */
	char region[HAL_UDSP_ISO3_MAX];
	/* The last name that the resolver sent to a fallback. A name with its
	 * own file empties it. Every bring-up resolves the configured name
	 * again, so only a change of name warns again. */
	char region_warned[HAL_UDSP_ISO3_MAX];
	uint32_t start_token;
	uint32_t region_token;
	bool start_armed;
	bool region_armed;

	/* Class (a) records the relay could not pass on, added to the library's
	 * own drop count in counters_get. */
	uint64_t relay_dropped;
	/* Class (b) records the completion queue could not hold. The two
	 * classes are counted apart: this one reaches the core
	 * as hal_counters.completions_lost and never joins events_dropped. */
	uint64_t comp_lost;

	/* The media plane. Indexed by the HAL widths, which cap_get clamps the
	 * library's own numbers to, so no op can reach past either array. */
	struct hal_udsp_stream stream[HAL_LINE_MAX][HAL_STREAM_MAX];
	/* Runs inside HAL_UDSP_STATS_TOKEN_BASE, see the note there. */
	uint32_t stats_token;

	/* The enum hal_debug_flag mask in force, and the timer that drains the
	 * library ring while its jitter buffer bit is set. The mask follows
	 * what the backend achieved: a packet trace or a canceller capture the
	 * library could not allocate leaves its bit clear, so the next push
	 * tries again. */
	uint32_t debug;
	struct uloop_timeout jb_trace;

	/*
	 * The packet trace. Its timer is its own, because the two bits arm
	 * independently: an operator who asks for the decision trace alone
	 * must not start the media drain with it.
	 */
	struct uloop_timeout pkt_trace;
	struct hal_udsp_gen_table pkt_gen[HAL_LINE_MAX][HAL_STREAM_MAX];
	/* Records whose generation no entry of their stream carries, and
	 * records whose stream carries no entry at all. The first kind renders
	 * with the newest description and the second is dropped. */
	uint64_t pkt_gen_miss;
	/* Records the ring refused. The drain goes on: one entry lost costs
	 * the archive one packet and the rest of the drain is still evidence. */
	uint64_t pkt_ring_fail;
	/* Arms the library refused for want of memory. The warning beside each
	 * one is the evidence; no diagnostic row carries this. */
	uint64_t pkt_arm_fail;

	/* The canceller capture, on a timer of its own for the reason the
	 * packet trace gives. The two counters follow the packet trace rules:
	 * records the ring refused, and arms the library refused for want of
	 * memory. */
	struct uloop_timeout lec_capture;
	uint64_t lec_ring_fail;
	uint64_t lec_arm_fail;
};

/*
 * udsp.c. The translations below are non-static so the host suite can drive
 * each one on its own record: the visual indicator flag and the capability
 * subset have no observable effect a host test can read back through the
 * library, and a translation no test checks can quietly invert.
 */
enum hal_status hal_udsp_status_map(enum udsp_status st);
void hal_udsp_cap_fill(struct hal_cap *dst, const struct udsp_cap *src);
void hal_udsp_config_fill(struct udsp_config *dst, const struct cfg *cfg);
void hal_udsp_region_name(const char *iso3, char *out, size_t len);
void hal_udsp_region_resolve(struct hal *h, const char *iso3, char *out,
			     size_t len);
void hal_udsp_region_prefetch(struct hal *h, const char *region, char *out,
			      size_t len);
void hal_udsp_cid_fill(struct udsp_cid *dst, const struct hal_cid *src);
void hal_udsp_ring_fill(struct udsp_ring *dst, const struct hal_ring *src);
void hal_udsp_region_fill(struct hal_region *dst,
			  const struct udsp_region_view *src);
/* The library's text sink (udsp.h udsp_config.log) and its level map. */
void hal_udsp_log(void *ctx, enum udsp_log_level level, const char *msg);
int hal_udsp_log_prio(enum udsp_log_level level);
void hal_udsp_fxsnum_write(unsigned int lines);

/*
 * The media translations, non-static for the same reason and for one
 * more: the codec and fax selectors are the two enum pairs the two contracts
 * do not share (see the media notes in udsp_events.c), so a cast would be
 * wrong and only a case that drives the map can say the switch is right.
 *
 * hal_udsp_codec_sel and hal_udsp_fax_mode answer -1 for a hal value libudsp
 * names no member for; every other answer is the enum member of the library.
 * hal_udsp_media_fill writes key material into dst, so a caller zeroizes it
 * after the library call (udsp.h struct udsp_media).
 */
int hal_udsp_codec_sel(uint8_t codec);
int hal_udsp_fax_mode(uint8_t mode);
void hal_udsp_media_fill(struct udsp_media *dst, const struct hal_media *src);
void hal_udsp_stats_fill(struct hal_media_stats *dst,
			 const struct udsp_media_stats *src);
/*
 * The jitter buffer counters, one field at a time for the same reason: the two
 * records are separate types by design and no layout assertion binds them.
 * Non-static so a case can drive the copy on a record of its
 * own, which is the only way to catch a pair of fields that swapped.
 */
void hal_udsp_jb_stats_fill(struct hal_jb_stats *dst,
			    const struct udsp_jb_stats *src);
/*
 * The line canceller state, copied the same way and non-static for the same
 * reason. The fake line of the suite is silent, so a record the engine
 * publishes leaves most of the processor verdicts at 0, and only a record of
 * the case's own can tell every field apart.
 */
void hal_udsp_lec_stats_fill(struct hal_lec_stats *dst,
			     const struct udsp_lec_stats *src);
/*
 * The three name tables the decision trace renders with. They are non-static
 * so the suite can pin them against rtp/jb.h, which is where the numbers are
 * defined: a table that slipped by one would name every decision wrong and no
 * status could say so.
 */
const char *hal_udsp_jb_state_name(uint8_t state);
const char *hal_udsp_jb_decision_name(uint8_t decision);
const char *hal_udsp_jb_rc_name(int8_t rc);
/*
 * The wire mark: IP_TOS for AF_INET, IPV6_TCLASS for AF_INET6,
 * both computed as (dscp << 2) from the 0..63 DSCP hal_media carries. Returns
 * 0 when the mark reached the socket. Non-static so a case can assert the
 * AF_INET6 arm on a socket of its own; the IPv6 stream cases reach that arm
 * through the media path as well.
 */
int hal_udsp_dscp_mark(int fd, int family, uint8_t dscp);

/*
 * One packet trace record onto the call trace ring. It is non-static so a case
 * can drive one record of its own: the endpoints a record renders with come
 * from the generation table and not from the record, and a live drain cannot
 * produce a record of an old description on demand. offset_ns is the
 * CLOCK_REALTIME minus CLOCK_MONOTONIC difference the drain read once, which
 * is what turns the record's monotonic stamp into the wall clock the ring
 * carries.
 */
void pkt_trace_record(struct hal *h, const struct udsp_pkt_trace *rec,
		      int64_t offset_ns);

/*
 * One canceller capture record onto the canceller ring, copied one field at a
 * time into the header of trace_ring.h. It is non-static so a case can drive
 * a record of its own: the silent fake line of the suite leaves most fields
 * of a live record at one value, so only a crafted record tells every field
 * apart. offset_ns is the difference pkt_trace_record takes.
 */
void lec_capture_record(struct hal *h, const struct udsp_lec_capture *rec,
			int64_t offset_ns);

/*
 * The file the line count goes to. Production never moves it off
 * HAL_UDSP_FXSNUM_RUN; it is non-static so the host suite can point the write
 * at a temporary directory.
 */
extern const char *hal_udsp_fxsnum_path;

#endif
