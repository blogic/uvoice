// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * uvoice hardware abstraction layer contract.
 *
 * One backend translation unit is linked into uvoiced. The udsp backend wraps
 * libudsp, the software voice DSP. The stub backend runs the same table on
 * x86 for CI. Every type here exists to keep backend types out of the core.
 *
 *
 * 1. Speed classes
 *
 * Every op is FAST or SLOW. The two classes live in separate blocks of struct
 * hal_ops and every op carries a "fast:" or "slow:" doc line.
 *
 *   FAST  Runs on the uloop thread. Must not block longer than a single
 *         round trip to the backend's driver or engine.
 *   SLOW  Must not run on the uloop thread. Takes a struct hal_job. The op
 *         returns as soon as the work is accepted; the result arrives later
 *         as an event record (see section 2) and the core then calls
 *         job->cb on the uloop thread.
 *
 * A slow op is the only op that takes a struct hal_job pointer, so the class
 * is visible in the signature as well as in the block and the doc line.
 *
 * An op is SLOW when a backend can need seconds or a bulk copy for it: the
 * GR-909 sweep fills a large result buffer over several seconds, and the DSP
 * bring-up loads firmware or starts an engine. Every other op is FAST.
 *
 * A bulk diagnostic read from the device joins the SLOW block with its own
 * completion payload. It does not go into a fast op. A read from state that
 * the backend already caches is FAST (see the diagnostic getters).
 *
 * stream_stats_start is SLOW as well. The CDR path reads it at call teardown,
 * where a deferred ubus reply costs nothing, so the class costs a fast backend
 * nothing and leaves room for a backend that must query the device.
 *
 *
 * 2. Event transport
 *
 * The core owns the transport, not the backend:
 *
 *   socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fd)
 *   fd[0]  core read end, registered with uloop_fd, ULOOP_READ
 *   fd[1]  backend write end, O_NONBLOCK, handed to ops->open()
 *
 * One struct hal_event per send(). The record is fixed size POD; SOCK_SEQPACKET
 * preserves message boundaries, so a partial record can never be read. The
 * backend must not close the write end; the core closes both ends after
 * ops->close().
 *
 * Two classes of record travel the same socket and follow different rules.
 *
 * (a) Hardware events, written by the event pump. They carry a per-backend
 *     monotonic seq starting at 1. EAGAIN policy: the pump must never block,
 *     because the thread that writes a hardware event can also drive the
 *     device, and a blocked write stalls it. On EAGAIN or ENOBUFS the backend
 *     drops the record, increments hal_counters.events_dropped and returns.
 *     It must not retry, sleep or grow a private queue. A gap in seq tells
 *     the core that a drop happened even before the counter is read. The
 *     core logs one rate-limited warning per drop burst and publishes both
 *     numbers in "ubus call voice status". A dropped hook event can strand a
 *     line, so the drop counter is a fault indicator, not statistics.
 *
 * (b) Job completions (HAL_EVENT_JOB_DONE), whichever backend thread writes
 *     them. They carry seq = 0, they are excluded from seq and gap
 *     accounting, and the drop policy of (a) does not apply to them: a
 *     lost completion would strand a call. A writer of a completion
 *     waits for room. It either polls the write end and retries, or it
 *     queues the record and drains the queue when the write end has room.
 *     Rationale for either form instead of a blocking write: the write end
 *     is O_NONBLOCK for the pump, and both classes share one file
 *     description. A completion that the writer still cannot deliver counts
 *     in hal_counters.completions_lost, and the backstop below then fails
 *     the job.
 *     The udsp backend's synthetic HAL_EVENT_SLIC_FAULT for the stream-dead bit
 *     in src/hal/udsp/udsp_int.h also carries seq = 0 and stays outside gap
 *     accounting.
 *
 * Backstop. The core arms a timer per pending job (30 s by default). If no
 * completion arrives the core fails the job with HAL_TRANSIENT, removes it from
 * the pending list and calls job->cb. A late completion for a timed-out token
 * is then unknown to the core and is logged and discarded.
 *
 * Threads. ops->* are called from the uloop thread only. The shape of the pump
 * is the backend's, and only its two rules are contract: the writer builds one
 * hal_event and writes it, and it calls no other op.
 *
 * A backend with one producer thread writes from that thread, one record per
 * source event. A backend with several producer threads puts one emitter in
 * front of the fd instead, and every producer posts to it. The udsp backend
 * does that, because an atomic seq alone does not preserve socket write order
 * across producers, and event_seq_check() would then log gaps that never
 * happened and inflate events_lost. Either shape gives the core one record per
 * datagram with a monotonic seq.
 *
 * Stream ids. HAL_EVENT_JB_UPDATE and HAL_EVENT_T38 carry in hal_event.stream
 * the stream that the event reports. A translator must take that id from the
 * part of its source event that describes the stream. It must not take it
 * from a general field that the source can leave unset for these two events.
 *
 *
 * 3. Error mapping
 *
 * A backend maps each answer of its driver or engine onto enum hal_status:
 *
 *   HAL_OK           done, or for a slow op, accepted
 *   HAL_TRANSIENT    the device is busy; retry, do not tear down
 *   HAL_UNSUPPORTED  the feature is absent; log once
 *   HAL_FATAL        an invalid parameter (our bug, not the device's), a
 *                    device failure, or an answer the backend cannot name
 *
 * The core treats HAL_TRANSIENT as "retry once after 50 ms, then fail the
 * operation".
 */
#ifndef UVOICE_HAL_H
#define UVOICE_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libubox/list.h>
#include <libubox/uloop.h>

/* The shipping boards are 1-FXS or 2-FXS. */
#define HAL_LINE_MAX		2
/* Streams per line at most; hal_cap.streams_per_chan carries the real value. */
#define HAL_STREAM_MAX		4
/* Cadence pairs per ring template at most; see struct hal_ring. */
#define HAL_CADENCE_MAX		5
/* 32 characters, +1 so HAL strings stay NUL terminated. */
#define HAL_CID_STR_MAX		33
#define HAL_IFNAME_MAX		16
/* Backstop for a slow op whose completion never arrives, see section 2. The
 * default covers firmware load, DSP stop, region apply, and media queries.
 * Line tests use LINE_TEST_JOB_MS. */
#define HAL_JOB_TIMEOUT_MS	30000
/* HAL_TRANSIENT retry delay, see section 3. */
#define HAL_RETRY_MS		50

enum hal_status {
	HAL_OK = 0,
	HAL_TRANSIENT,
	HAL_UNSUPPORTED,
	HAL_FATAL,
};

enum hal_feature {
	HAL_FEAT_G729		= (1 << 0),
	HAL_FEAT_G726		= (1 << 1),
	HAL_FEAT_T38		= (1 << 2),
	HAL_FEAT_VBD		= (1 << 3),	/* V.152 voice band data; no
						 * shipped backend sets it */
	HAL_FEAT_LINE_TEST	= (1 << 4),
	HAL_FEAT_POLARITY	= (1 << 5),	/* forward disconnect */
	HAL_FEAT_MWI_LAMP	= (1 << 6),	/* see line_mwi_set */
	HAL_FEAT_METERING	= (1 << 7),	/* no shipped backend sets it */
	HAL_FEAT_KERNEL_RTP	= (1 << 8),	/* the kernel makes and reads the
						 * RTP packets, on the sockets
						 * the backend owns and binds.
						 * A clear bit is not a missing
						 * capability: it says the
						 * packets are made and read in
						 * user space, by the backend
						 * itself, on those same
						 * sockets. The core reads the
						 * bit to know who owns the
						 * packet loop, never to decide
						 * whether media works. No
						 * shipped backend sets it */
	HAL_FEAT_CONF		= (1 << 9),	/* see conf_set */
	HAL_FEAT_SRTP		= (1 << 10),	/* SDES SRTP on the media stream,
						 * see enum hal_srtp_suite and
						 * the srtp block of struct
						 * hal_media. A backend sets it
						 * only where its SRTP path is
						 * proven: the udsp backend
						 * maps it from the library;
						 * the stub sets it always,
						 * which is the CI precedent
						 * HAL_FEAT_CONF and
						 * HAL_FEAT_T38 already carry */
	HAL_FEAT_WIDEBAND	= (1 << 11),
	HAL_FEAT_G729B		= (1 << 12),	/* G.729 Annex B on the G.729 coder
						 * this backend carries: the SID
						 * frame and the untransmitted gap,
						 * decode and encode. It is a
						 * narrower bit than HAL_FEAT_G729
						 * and never implies it; a backend
						 * whose G.729 is Annex A alone
						 * sets G729 and leaves this clear.
						 *
						 * The SDP layer reads it twice for
						 * two different questions. It
						 * states annexb=yes on our own
						 * G.729 rtpmap, which is a claim
						 * about what this side decodes, and
						 * it lets a codec section with
						 * silence suppression arm the
						 * transmit DTX that hal_media
						 * carries as annexb. A backend that
						 * leaves the bit clear therefore
						 * writes annexb=no and never arms,
						 * which is the truthful answer for
						 * a coder whose Annex B is
						 * unverified */
	HAL_FEAT_IPV6_MEDIA	= (1 << 13),	/* the backend carries a media
						 * stream whose addresses are
						 * IPv6. It opens the sockets
						 * in AF_INET6 with
						 * IPV6_V6ONLY, marks them
						 * with IPV6_TCLASS and
						 * connects them in that
						 * family.
						 *
						 * A clear bit means
						 * stream_config_set rule 4
						 * refuses every IPv6
						 * address, so a leg whose
						 * own media address is IPv6
						 * is refused before it
						 * rings.
						 *
						 * The udsp backend sets it
						 * from its own socket code
						 * and not from a library
						 * bit, because libudsp takes
						 * connected descriptors and
						 * owns no address. The stub
						 * sets it on the CI
						 * precedent HAL_FEAT_CONF
						 * and HAL_FEAT_T38 carry */
	HAL_FEAT_RTCP_ADDR	= (1 << 14),	/* the backend connects the
						 * RTCP socket to
						 * hal_media.rtcp_remote when
						 * that names an address. A
						 * backend without the bit
						 * never sees the field: the
						 * call layer leaves it zero
						 * for it (RFC 3605 section
						 * 2.1) */
	HAL_FEAT_REGION_HOOK	= (1 << 15),	/* the backend programs each
						 * line's hook windows from its
						 * own region data, so the core
						 * never calls
						 * line_hook_timing_set on it: a
						 * push would only latch the
						 * carried windows against the
						 * next region swap. libudsp
						 * seeds line/hook.c from the
						 * region file per line; the
						 * stub takes the push. */
	HAL_FEAT_DTMF_RELAY	= (1 << 16),	/* the backend relays a digit it
						 * detects on the phone as an
						 * RFC 4733 event itself, from
						 * the detection to the end of
						 * the tone, on every stream in
						 * RFC2833 mode.
						 *
						 * The core then makes no
						 * stream_dtmf_send call for a
						 * digit a detector measured,
						 * because the event is already
						 * on the wire. It still makes
						 * one for a pulse digit and for
						 * an injected digit, which the
						 * backend never heard.
						 *
						 * A backend without the bit
						 * relays nothing of its own,
						 * and the core sends the digit
						 * at the end of the tone with
						 * the measured duration. */
	HAL_FEAT_DTMF_PLAYOUT	= (1 << 17),	/* the backend renders a
						 * telephone-event it receives
						 * toward the phone itself,
						 * from the first packet of the
						 * event to its end or its
						 * timeout.
						 *
						 * The core then plays no digit
						 * of its own for a wire digit
						 * that backend reports, and the
						 * HAL_EVENT_DIGIT record with
						 * remote set stays a report.
						 *
						 * A backend without the bit
						 * reports the digit and plays
						 * nothing, and the core plays
						 * it with digit_play. */
	HAL_FEAT_LOOP		= (1 << 18),	/* the backend plays a line the
						 * capture of a line, its own
						 * or another, a fixed delay
						 * later (line_loop_set). The
						 * audio stays inside the
						 * backend: the loop carries no
						 * packet and needs no stream.
						 *
						 * A backend without the bit
						 * leaves line_loop_set NULL,
						 * and the core then runs no
						 * loop test on it. */
	HAL_FEAT_DESK		= (1 << 19),
};

enum hal_codec {
	HAL_CODEC_NONE = 0,
	HAL_CODEC_PCMU,
	HAL_CODEC_PCMA,
	HAL_CODEC_G729,
	HAL_CODEC_G726_32,	/* the dynamic RTP payload type comes from
				 * g726_ptype */
	HAL_CODEC_T38,
	HAL_CODEC_VBD_PCMU,
	HAL_CODEC_VBD_PCMA,
	HAL_CODEC_G722,
	HAL_CODEC_OPUS,
};

/*
 * The Opus bandwidth ceiling (hal_media.opus.max_bandwidth). The values are
 * the ones libudsp defines (enum udsp_opus_bw), and hal/udsp/udsp_events.c
 * pins the two vocabularies against each other by name, so the backend copies
 * the field raw. The enum is appended, like every other member of this header:
 * a renumber breaks that pin and the e2e asserts the numbers.
 *
 * AUTO follows the line rate: wideband on a 16 kHz line, narrowband on an
 * 8 kHz one. Super-wideband and fullband have no member, because the codec
 * path caps at 16 kHz: a peer that can render more is offered wideband.
 */
enum hal_opus_bw {
	HAL_OPUS_BW_AUTO = 0,
	HAL_OPUS_BW_NB,
	HAL_OPUS_BW_MB,
	HAL_OPUS_BW_WB,
};

/* The scheduling policy a backend thread runs under (hal_cap.rt_policy,
 * rt_net_policy). NONE: the backend reports no outcome; the stub.
 * UNKNOWN: the backend could not read the policy back. */
enum hal_rt_policy {
	HAL_RT_POLICY_NONE = 0,
	HAL_RT_POLICY_UNKNOWN,
	HAL_RT_POLICY_OTHER,
	HAL_RT_POLICY_FIFO,
	HAL_RT_POLICY_RR,
};

/*
 * Filled by the backend probe. lines and streams_per_chan come from the device
 * or the backend's engine, never from config, because the config can lie
 * about a 2-FXS board.
 */
struct hal_cap {
	char backend[16];
	char version[32];
	char slic[16];
	uint8_t lines;
	uint8_t streams_per_chan;
	uint32_t features;		/* enum hal_feature bits */
	uint32_t codecs;		/* 1 << enum hal_codec */
	/*
	 * Appended, so a backend built against the older record keeps its
	 * layout. What the backend's real-time setup achieved, read back
	 * after the request. rt_policy HAL_RT_POLICY_NONE means the backend
	 * reports nothing and every field below is 0: the stub backend
	 * callocs the record and leaves it there. The udsp backend
	 * copies udsp_cap: the tick thread's policy, the packet thread's, the
	 * SCHED_FIFO priority the config asked for (0 = no request), the
	 * refusal of that request (0 = granted or not made; EPERM is the
	 * RT_GROUP_SCHED gate), the pinned cpu (-1 = unpinned) and whether
	 * the library's own mlockall held. The record names the engine of the
	 * open, a recovery that recreates the engine does not rewrite it, and
	 * the lock is the library's at open, not the daemon's own.
	 */
	uint8_t rt_policy;		/* enum hal_rt_policy */
	uint8_t rt_net_policy;		/* enum hal_rt_policy */
	bool rt_mlock;
	int16_t rt_cpu;
	int32_t rt_prio;
	int32_t rt_sched_errno;
	/*
	 * Appended after rt_sched_errno, by the same rule. The largest T.38
	 * UDPTL datagram the backend accepts, in bytes. The SDP layer announces
	 * it as T38FaxMaxDatagram (sdp.c sdp_t38_max_datagram_set). 0 means
	 * the backend reports none, and the SDP layer keeps its own default.
	 */
	uint16_t t38_max_datagram;
};

struct hal_counters {
	uint64_t events_sent;
	uint64_t events_dropped;	/* EAGAIN policy, section 2 */
	uint64_t op_errors;
	/*
	 * Appended, so a backend built against the older record keeps its
	 * layout. Every backend fills both. A backend with nothing to report
	 * writes 0 rather than leaving the field alone, because counters_get
	 * fills fields and the core does not have to clear the record it
	 * passes in.
	 *
	 * A zero completions_lost does not prove no completion was lost, and
	 * the reading is per backend. The udsp backend is the one that splits
	 * the two classes: a completion its relay queue could not hold is
	 * counted here and nowhere else. The stub backend does not split its
	 * counting: it can abandon a completion after its retry budget runs
	 * out (stub.c event_push_completion), it counts that loss into
	 * events_dropped, and it reports 0 here. That is a known gap and not
	 * a promise about its transport.
	 */
	uint64_t tdm_stalls;		/* engine-detected TDM capture and
					 * playback resyncs; 0 on a backend with
					 * no TDM engine of its own */
	uint64_t completions_lost;	/* class (b) completion records the
					 * backend failed to deliver, section 2;
					 * 0 where a completion cannot be lost,
					 * and 0 on a backend that has not split
					 * its counting yet, see above */
	/*
	 * Appended after completions_lost, by the same rule, so a backend
	 * built against the older record keeps its layout. They are the
	 * catch-up record of a late engine. A hold is one episode in which
	 * the engine found two or more capture periods waiting. The three
	 * drop counters are in periods. A backend with no TDM engine of its
	 * own writes 0 in all four.
	 */
	uint64_t holds;
	uint64_t capture_drops;		/* periods the ring overwrote unread */
	uint64_t trim_drops;		/* periods the engine skipped to
					 * catch up after a hold */
	uint64_t playback_expired;	/* periods the hardware ran past */
	/*
	 * Appended after playback_expired, by the same rule. The hook
	 * sampling record of the backend's SLIC thread, in wall-clock terms:
	 * the hook machine measures a dial pulse between samples, so a
	 * sample taken late mismeasures the pulse. hook_gaps counts the
	 * samples that came more than one tick after the previous one while
	 * the thread sampled per tick, hook_gap_max_ms is the widest such
	 * gap, and hook_moved_gaps counts the samples on which the part
	 * recorded a hook change the level did not show: a lost pair of
	 * transitions. A backend with no SLIC thread of its own writes 0 in
	 * all three.
	 */
	uint64_t hook_gaps;
	uint64_t hook_moved_gaps;
	uint32_t hook_gap_max_ms;
};

enum hal_line_state {
	HAL_LINE_DOWN = 0,
	HAL_LINE_ACTIVE,		/* normal battery feed */
	HAL_LINE_OHT,			/* on-hook transmission, for type 1 CID */
	HAL_LINE_BUSY,
	HAL_LINE_SLEEP,
	HAL_LINE_HOWLER,
};

enum hal_polarity {
	HAL_POL_FORWARD = 0,
	HAL_POL_REVERSE,
};

enum hal_hook {
	HAL_HOOK_SEIZE = 0,		/* off hook */
	HAL_HOOK_RELEASE,		/* on hook */
	HAL_HOOK_FLASH,
	HAL_HOOK_PULSE,			/* hal_event.u.hook.pulses carries the count */
	HAL_HOOK_ERROR,
};

enum hal_tone {
	HAL_TONE_NONE = 0,
	HAL_TONE_DIAL,
	HAL_TONE_STUTTER,		/* message waiting dial tone */
	HAL_TONE_SECOND_DIAL,		/* recall dial tone, SPRE second stage */
	HAL_TONE_RINGBACK,
	HAL_TONE_BUSY,
	HAL_TONE_REORDER,		/* fast busy */
	HAL_TONE_SIT,
	HAL_TONE_CALL_WAITING,
	HAL_TONE_CONFIRM,
	HAL_TONE_HOWLER,		/* receiver off hook, end of the ROH chain */
	HAL_TONE_SHORT_STUTTER_DIAL,	/* 3WC flash: short stutter then dial, region-defined */
	HAL_TONE_NUMBER_UNOBTAINABLE,	/* the unobtainable clearing */
};

/* The format selects modulation and alerting together; MDMF vs SDMF follows
 * from which fields are populated. The two members at the end serve the uci
 * cid_format map:
 *   HAL_CID_ETSI_LR_DTAS  the UK SIN 227 sequence, line reversal then DTAS
 *                         then V.23 FSK, all before the first ring.
 *   HAL_CID_DTMF_DTAS     DTMF prior to ringing, with DTAS as the alert. */
enum hal_cid_format {
	HAL_CID_BELLCORE_FSK = 0,
	HAL_CID_ETSI_FSK,
	HAL_CID_ETSI_RPAS,
	HAL_CID_ETSI_DTAS,
	HAL_CID_DTMF,
	HAL_CID_NTT,
	HAL_CID_ETSI_LR_DTAS,
	HAL_CID_DTMF_DTAS,
};

enum hal_cid_kind {
	HAL_CID_TYPE1 = 0,		/* on hook, before or between rings */
	HAL_CID_TYPE2,			/* off hook, call waiting */
};

enum hal_mwi {
	HAL_MWI_OFF = 0,
	HAL_MWI_ON,
};

struct hal_cid {
	uint8_t format;			/* enum hal_cid_format */
	uint8_t mwi;			/* enum hal_mwi, FSK VMWI path */
	bool number_private;		/* Privacy: id, drives blockinfo */
	bool name_private;
	char number[HAL_CID_STR_MAX];
	char name[HAL_CID_STR_MAX];
	char datetime[HAL_CID_STR_MAX];	/* MMDDHHMM, empty means "let the DSP fill" */
	char blockinfo[HAL_CID_STR_MAX];	/* single free-text absence reason */
};

struct hal_cadence {
	uint16_t on_ms;
	uint16_t off_ms;
};

/*
 * One power-ringing request. A backend that carries fewer cadence pairs than
 * HAL_CADENCE_MAX clamps cadence_count and first_count to its own limit.
 */
struct hal_ring {
	struct hal_cadence cadence[HAL_CADENCE_MAX];
	struct hal_cadence first[HAL_CADENCE_MAX];
	uint8_t cadence_count;
	uint8_t first_count;
	uint8_t cid_at_ring;		/* send CID after this ring break, 0 = before */
	uint32_t cid_wait_ms;
	uint32_t duration_ms;		/* 0 = ring until stopped */
	bool cid_present;
	struct hal_cid cid;
};

/*
 * The hook timing subset that /etc/config/voip actually drives: these three
 * options and no more. The flash vs on-hook decision happens in the backend, so
 * they are pushed once per line at bring-up and are not retunable per call
 * state. The seize threshold and pulse detection are not here: no config
 * option drives them, so they stay the hardware defaults.
 */
struct hal_hook_timing {
	uint32_t flash_min_ms;
	uint32_t flash_max_ms;
	uint32_t release_min_ms;
};

/* The ring templates a backend can report, and the widest region name it can
 * carry (libudsp UDSP_ISO3_MAX and the line/line.c region names both fit seven
 * characters plus the terminator). */
#define HAL_REGION_RING_MAX	8
#define HAL_REGION_ISO3_MAX	8
/* The region's display name with its terminator, the libudsp describe bound. */
#define HAL_REGION_DESCRIBE_MAX	64
/* Call-waiting lead-ins, indexed by variant: index 0 is the plain tone and
 * 1..4 the distinctive rows, so one past the libudsp UDSP_REGION_CW_VIEW_MAX. */
#define HAL_REGION_CW_MAX	5

/* What a hook flash does with two calls, the region's own answer. The numbers
 * are those of the libudsp flash service vocabulary, and the udsp backend
 * asserts each pair. */
enum hal_flash_service {
	HAL_FLASH_SERVICE_OFF = 0,	/* the flash is a plain network event */
	HAL_FLASH_SERVICE_SPLIT,	/* the flash splits a conference in two */
	HAL_FLASH_SERVICE_DROP,		/* the flash drops the last call */
};

/* One power-ringing template, the shape struct hal_ring's cadence takes.
 * cadence_count 0 is a template the region does not define, and the caller
 * falls back to its own table for it. */
struct hal_region_ring {
	struct hal_cadence cadence[HAL_CADENCE_MAX];
	uint8_t cadence_count;
};

/*
 * The region the backend actually holds, read back after a region_apply
 * completes. It is the country data's own answer and it outranks any config
 * surface that carries the same numbers: on a udsp image the region files
 * under /usr/share/udsp/regions are the source of truth and the voip config
 * region sections are deleted at seed time. A backend that cannot introspect
 * its own tone set leaves region_get NULL, and the core then uses its
 * compiled default region (REGION_HAL_DEFAULT in line/line.c).
 *
 * cid_format and mwi_format are enum hal_cid_format, not the four-word uci
 * vocabulary: the region grammar carries all eight formats, so the read-back
 * is strictly more expressive than the option it replaces.
 *
 * The three hook windows are the region's own, in milliseconds. The core
 * pushes them to every line through line_hook_timing_set as they stand,
 * except a release window below the flash maximum, which it raises to that
 * maximum (hook_timing_get() in line/line.c).
 *
 * cw_sas_total_ms is the whole call-waiting subscriber alerting signal in
 * milliseconds, so the call waiting offer can be made to outlast it. It is the
 * MAXIMUM over the plain tone and every distinctive variant, so the offer
 * window covers whichever variant plays. 0 means the alert tone bounds nothing
 * (it repeats until stopped) and the core keeps its own timeout.
 *
 * ring[i] is ring template i, the join the Alert-Info "Bellcore-dr<n>" token
 * and the region cadence rows both use, so ring[0] is the plain ring.
 *
 * cw_count is the number of distinctive call-waiting variants the region
 * defines, 0 to HAL_REGION_CW_MAX - 1. cw_lead_in_ms is indexed by variant:
 * index 0 is the plain tone and 1..cw_count the distinctive rows. The value is
 * the first-presentation length, the point after which the type 2 CAS may
 * inject; the core defers the caller-id record by it plus its own gap. A
 * backend that cannot introspect its variants leaves cw_count 0.
 *
 * flash_service is enum hal_flash_service, the region's hook flash policy;
 * describe is the region's display name and may be empty (the libudsp key is
 * optional). Both are appended after ring[]; cw_count and cw_lead_in_ms after
 * describe. The records of this header are append only.
 *
 * pulse_invert is the decadic digit coding of the region. At 1 a dialed N
 * arrives as 10 - N pulses and the digit 0 as 10; 0 is the direct coding. A
 * zeroed record therefore keeps the direct coding. The core shows it in the
 * status read-back, and maps a pulse count with the coding of the hook event
 * (struct hal_event u.hook). Appended after cw_lead_in_ms.
 *
 * confirm_total_ms and confirm_endless describe the length of the region
 * confirm tone. The core copies both when it starts the confirm tone, and
 * starts the dial tone when that tone ends. confirm_total_ms is the whole tone
 * in milliseconds, and 0 means that no bound is known: the core then keeps its
 * own deferral. confirm_endless at 1 means the tone repeats until stopped, and
 * the core then plays no dial tone after it. A zeroed record therefore keeps
 * the deferral of the core. Both are appended after pulse_invert.
 */
struct hal_region {
	char iso3[HAL_REGION_ISO3_MAX];
	uint8_t cid_format;		/* enum hal_cid_format */
	uint8_t mwi_format;		/* enum hal_cid_format */
	uint32_t hook_flash_min_ms;
	uint32_t hook_flash_max_ms;
	uint32_t hook_release_min_ms;
	uint32_t cw_sas_total_ms;
	struct hal_region_ring ring[HAL_REGION_RING_MAX];
	uint8_t flash_service;		/* enum hal_flash_service */
	char describe[HAL_REGION_DESCRIBE_MAX];
	uint8_t cw_count;
	uint16_t cw_lead_in_ms[HAL_REGION_CW_MAX];
	uint8_t pulse_invert;
	uint32_t confirm_total_ms;
	uint8_t confirm_endless;
};

enum hal_detect {
	HAL_DETECT_DTMF		= (1 << 0),
	/* On libudsp the [hook] pulse_enable of the region file decides pulse
	 * classification. A clear bit turns pulses off on the line; a set bit
	 * keeps the region value. */
	HAL_DETECT_PULSE	= (1 << 1),
	HAL_DETECT_MODEM	= (1 << 2),
	HAL_DETECT_JB_EVENT	= (1 << 3),
};

struct hal_channel_cfg {
	uint32_t detect_mask;		/* enum hal_detect bits */
	/*
	 * 0.5 dB steps. gain_tx is the gain toward the interface, what the
	 * subscriber hears. gain_rx is the gain from the interface, what the
	 * subscriber sends. A backend maps the pair onto the directions of its
	 * DSP.
	 */
	int8_t gain_tx;
	int8_t gain_rx;
	bool ec_enable;
	bool ec_nlp_enable;
	uint16_t ec_tail_ms;		/* 0 = backend default */
	/*
	 * 0.5 dB steps, 0 = unity. slic_gain_tx is the SLIC analog transmit
	 * gain. slic_gain_rx is the receive gain of the port: the udsp backend
	 * applies a negative value as a loss ahead of the SLIC, so a local tone
	 * plays at its region level.
	 */
	int8_t slic_gain_tx, slic_gain_rx;
	/*
	 * The answer-tone disabler of the canceller, appended after the SLIC
	 * gain pair, which is the order libudsp's struct udsp_chan_cfg carries
	 * them in. A backend that has a G.168 disabler answers an answer tone
	 * from either direction whatever these two hold: the phase-reversed
	 * tone takes the canceller off whole, the plain tone keeps it on with
	 * the nonlinear processor off. The two options only pick the rules it
	 * applies, and false is the recommendation's own behaviour in both.
	 *
	 * ec_tone_latch false releases the disabler once the holding band falls
	 * quiet (G.168 7.8); true holds it until the line's streams end.
	 *
	 * ec_tone_force_disable false takes the phase-reversed tone to the modem
	 * policy and the plain tone to the fax policy, the mapping of G.168 7.1;
	 * true takes the plain tone to the modem policy too, for gear that
	 * expects a full disable on a plain 2100 Hz.
	 *
	 * A backend with no disabler ignores both.
	 */
	bool ec_tone_latch;
	bool ec_tone_force_disable;
};

enum hal_media_dir {
	HAL_MEDIA_INACTIVE = 0,
	HAL_MEDIA_SENDONLY,
	HAL_MEDIA_RECVONLY,
	HAL_MEDIA_SENDRECV,
};

/*
 * Where a digit the line hears goes, and what the uplink audio carries with
 * it.
 *
 * INBAND keeps the tone in the audio and relays nothing.
 *
 * RFC2833 and INFO remove the tone from the uplink, because the digit travels
 * out of band and a far end that runs an inband detector of its own would
 * report the same digit twice.
 *
 * RFC2833 relays the digit as an RFC 4733 event. A backend that advertises
 * HAL_FEAT_DTMF_RELAY relays it itself, while the tone sounds. A backend
 * without the bit relays nothing of its own, and the core sends the event at
 * the end of the digit through stream_dtmf_send with the measured duration.
 *
 * INFO puts no event on the wire. The core sends the INFO at the end of the
 * digit.
 */
enum hal_dtmf_mode {
	HAL_DTMF_INBAND = 0,
	HAL_DTMF_RFC2833,
	HAL_DTMF_INFO,
};

enum hal_jb_mode {
	HAL_JB_ADAPTIVE = 0,
	HAL_JB_FIXED,
	HAL_JB_OFF,
};

/*
 * What a G.711 passthrough carries (hal_media.passthrough). The values are the
 * ones libudsp defines (enum udsp_passthrough), and hal/udsp/udsp_events.c pins
 * the two vocabularies against each other by name, so that backend copies the
 * field raw. The enum is appended, like every other member of this header.
 */
enum hal_passthrough {
	HAL_PASSTHROUGH_OFF = 0,
	HAL_PASSTHROUGH_MODEM,
	HAL_PASSTHROUGH_FAX,
};

/*
 * The T.38 error correction mode. FEC has no member, because no backend
 * programs it: libudsp carries redundancy alone (enum udsp_t38_ec), so only
 * these two are expressible here.
 */
enum hal_t38_ec {
	HAL_T38_EC_NONE = 0,
	HAL_T38_EC_REDUNDANCY,
};

/*
 * The two SDES suites uvoice offers (RFC 4568 6.2). One value here is one
 * crypto attribute suite on the wire: HAL_SRTP_AES_CM_128_SHA1_80 is
 * AES_CM_128_HMAC_SHA1_80 and HAL_SRTP_AES_CM_128_SHA1_32 is
 * AES_CM_128_HMAC_SHA1_32. Both key the stream with the same 30 bytes and
 * differ in the SRTP authentication tag length alone: RFC 4568 6.2 gives the
 * _32 suite a 32 bit SRTP tag and keeps the SRTCP tag at 80 bits for both, so
 * SRTCP carries a 10 byte tag for both suites.
 *
 * The backend must refuse every other value, before it touches the stream
 * (stream_config_set rule 4), and the refusal is HAL_UNSUPPORTED: a suite the
 * backend cannot carry is a capability it lacks, which is the code that rule
 * already gives IPv6 media, and not the HAL_FATAL of a missing parameter. That
 * refusal is the only defense against a silent downgrade, because an SRTP
 * engine can fail open on a (cipher, authentication) pair it does not carry.
 * The stream then runs with the encryption off and no error.
 *
 * The stub backend is exempt by design. It validates nothing, it records what
 * the core asked for, and the "unknown" arm of its dump is what makes a bad
 * suite visible in CI. The refusal rule binds a backend that programs real
 * hardware.
 */
enum hal_srtp_suite {
	HAL_SRTP_NONE = 0,
	HAL_SRTP_AES_CM_128_SHA1_80 = 1,
	HAL_SRTP_AES_CM_128_SHA1_32 = 2,
};

/*
 * addr is network byte order: addr[0..3] when ipver is 4, all sixteen
 * bytes when it is 6. rtcp_port 0 means the port is port + 1, which is
 * the pair RFC 3550 section 11 asks for; a nonzero value is the port the
 * peer named in an RFC 3605 a=rtcp attribute. A backend carries the IPv6
 * form only when it sets HAL_FEAT_IPV6_MEDIA (stream_config_set rule 4).
 */
struct hal_addr {
	uint8_t ipver;			/* 4 or 6 */
	uint8_t addr[16];
	uint16_t port;
	uint16_t rtcp_port;		/* 0 = port + 1 */
};

/*
 * An RTCP endpoint that is not the media endpoint (RFC 3605 2.1). ipver
 * 0 is the common case: the peer named no address of its own and RTCP
 * goes to the media address.
 */
struct hal_rtcp_addr {
	uint8_t ipver;			/* 0, 4 or 6 */
	uint8_t addr[16];
};

struct hal_media {
	struct hal_addr local;
	/* A backend refuses a zero remote port, because a media socket it
	 * cannot connect would take any source. */
	struct hal_addr remote;
	uint8_t codec;			/* enum hal_codec */
	uint8_t codec_pt;		/* payload type on the wire */
	uint8_t dtmf_pt;		/* telephone-event PT, 0 = not negotiated */
	uint8_t dtmf_mode;		/* enum hal_dtmf_mode */
	uint8_t direction;		/* enum hal_media_dir */
	uint8_t ptime_ms;		/* 10, 20, 30 or 40 */
	uint8_t jb_mode;		/* enum hal_jb_mode */
	bool vad;
	bool cng;
	bool rtcp_enable;
	/*
	 * DSCP, 0..63, not a TOS byte. The uci options carry a TOS byte
	 * (rtp_tos 184), so the core converts with tos >> 2 before it fills
	 * these fields, and the backend computes the TOS byte back as
	 * (dscp << 2) & 0xFC. A backend applies it with setsockopt IP_TOS on
	 * an AF_INET media socket and with IPV6_TCLASS on an AF_INET6 one; the
	 * two options take the same byte.
	 */
	uint8_t rtp_dscp;
	uint8_t rtcp_dscp;
	uint16_t jb_init_ms;
	uint16_t jb_max_ms;
	uint16_t rtcp_interval_ms;
	char bind_dev[HAL_IFNAME_MAX];	/* SO_BINDTODEVICE, empty = no bind */
	/*
	 * Appended, so a backend built against the older record keeps its
	 * layout.
	 *
	 * Meaningful only when codec is HAL_CODEC_T38, and in that mode a
	 * backend ignores the jb_*, dtmf_*, codec_pt, vad and cng fields above:
	 * the audio jitter settings do not apply to fax, a UDPTL stream carries
	 * neither a telephone event nor a payload type of its own, and the
	 * three suppression switches are forced off. A backend reads the
	 * addresses, the direction and the DSCP pair as it does for audio.
	 * The udsp backend paces IFP packets from T.30 over UDPTL and carries
	 * no RTCP on that stream: it sends no sender report, receiver report
	 * or BYE, and it drops and counts an arriving RTCP datagram. Its
	 * gateway runs at 8 kHz behind rate conversion on a wideband line.
	 *
	 * max_bit_rate is bits per second. The backend programs the highest fax
	 * rate that does not exceed it, with 14400 as the ceiling; a request of
	 * 0, and a request below the lowest rate the DSP carries, both take
	 * 14400 as the default. max_datagram is the largest UDPTL datagram the
	 * far end accepts, and 0 means the peer named none.
	 *
	 * Two T.38 parameters are not negotiable through this record. The
	 * backend pins the T.38 version at 0 and the rate management at
	 * transferredTCF whatever the peer asked for. The SDP
	 * layer answers transferredTCF, so the wire and the stream agree.
	 */
	struct {
		uint32_t max_bit_rate;
		uint16_t max_datagram;
		uint8_t ec;		/* enum hal_t38_ec */
	} t38;
	/*
	 * Appended after the t38 block, by the same rule, so a backend built
	 * against the older record keeps its layout.
	 *
	 * suite is enum hal_srtp_suite. HAL_SRTP_NONE means the stream carries
	 * plain RTP, and the two keys are then all zero and meaningless; a
	 * backend must not read them in that mode. Any other value means SDES
	 * SRTP with the suite named, and the backend refuses a value the suite
	 * enum does not define.
	 *
	 * key_tx protects what this stream sends, key_rx opens what it
	 * receives. Each is the 30 byte concatenation SDES carries for
	 * AES_CM_128 (RFC 4568 6.1): the 16 byte master key of RFC 3711
	 * followed by its 14 byte master salt, raw. The base64 form of the
	 * crypto attribute stays in the SDP layer and never reaches this record.
	 *
	 * The block stays inside the memset-then-fill discipline that the media
	 * comparison depends on (call.c media_program: "Both descriptions were
	 * built by the same memset-then-fill, so the padding compares equal
	 * too"). A writer clears the whole record and then fills it, so a stream
	 * that keeps its keys compares equal and a rekey is a changed
	 * description.
	 *
	 * A changed key or suite on a running stream is a stop, a configure and
	 * a start, never a bare re-configuration, so the key continuity holds.
	 * An SRTP engine can move the transmit key at once on a bare
	 * re-configuration while the receiving side keeps the old key, and the
	 * stream then goes silently one way. libudsp can rekey in place, but
	 * the caller stops the stream first on every backend, the way a T.38
	 * mode move already does. An SRTP engine can also keep its context and
	 * roll-over counter on an identical re-push, so a backend stamps the
	 * key material on every push.
	 *
	 * A backend zeroizes the key material it retains when the stream stops:
	 * the suite goes back to HAL_SRTP_NONE and both keys go to zero, so no
	 * key outlives the leg that used it. The stub does it in stream_stop,
	 * and its dump then carries the suite alone; a post-call latch can be
	 * added if the end to end suite needs the digests to stay readable
	 * after a call.
	 *
	 * A T.38 stream forces HAL_SRTP_NONE. The image carries plain UDPTL,
	 * so an upspeed on an SRTP leg would drop the encryption without
	 * saying so. The SDP and call layers own that rule (the T.38 upspeed
	 * is gated off on an SRTP leg); nothing in the HAL enforces it.
	 *
	 * The keys never reach a trace, a log or a dump, not even in part. A
	 * backend may trace the suite name only, never key bytes.
	 */
	struct {
		uint8_t suite;		/* enum hal_srtp_suite */
		uint8_t key_tx[30];
		uint8_t key_rx[30];
	} srtp;
	struct {
		uint32_t bitrate;	/* bits/s; 0 = library default */
		uint8_t complexity;	/* 1..10; 0 = library default */
		uint8_t max_bandwidth;	/* enum hal_opus_bw, pinned against
					 * enum udsp_opus_bw by the backend */
		bool fec;
	} opus;
	/*
	 * Appended after the opus block, by the same rule, so a backend built
	 * against the older record keeps its layout.
	 *
	 * The local hold silence. Set means the backend must encode silence on
	 * this stream instead of the captured uplink, on every codec and at
	 * every rate. RFC 3264 asks a holder to send silence or music and not
	 * the room; on an FXS line the room also carries the dial tone the
	 * hybrid leaks back, which the held party would otherwise hear.
	 *
	 * It is not a direction. direction still says who may send, so a stream
	 * that does not send puts nothing on the wire whatever this field
	 * holds.
	 *
	 * It silences the relayed telephone events too. A held leg puts nothing
	 * of its own on the wire and a digit is as much its own as the room is,
	 * so a backend refuses stream_dtmf_send while this is set and cuts an
	 * event already in flight when a description turns it on.
	 *
	 * The call layer owns it and follows the leg's local hold state, so a
	 * hold sets it and a resume clears it. A backend that cannot encode
	 * silence ignores it, and the stream then carries the uplink.
	 */
	bool tx_silence;
	/*
	 * Appended after tx_silence, by the same rule, so a backend built
	 * against the older record keeps its layout.
	 *
	 * enum hal_passthrough: the leg's own record of a G.711 passthrough,
	 * carried through every later programming of the stream. The call layer
	 * applies the profile once, through fax_profile_apply, and a re-INVITE,
	 * a session refresh or a far-end hold then pushes a new description
	 * under it. A backend that keeps the passthrough in a record of its own
	 * loses it at that push, so this field is what a backend reads instead.
	 *
	 * The kind selects the canceller policy of the line, and nothing else
	 * reads it. A modem exchange is full duplex, so the canceller goes off
	 * whole: a converged filter subtracts its estimate from the very
	 * carrier the far end demodulates. A fax exchange is half duplex, so
	 * the canceller runs and only its non-linear processor goes off: the
	 * processor mutes the direction it reads as idle, which a half-duplex
	 * turn cannot afford, and the filter gets clean single talk to adapt
	 * on.
	 *
	 * It is not HAL_FAX_VBD. That mode negotiates a payload type change on
	 * the wire (V.152); this field describes what the stream carries and
	 * asks for no signalling of its own.
	 *
	 * The call layer sets the modem kind when the backend takes the
	 * profile, and the fax kind on the first local V.21 preamble of the
	 * leg. A backend with no canceller policy of its own ignores the field,
	 * as it ignores the hold silence above.
	 */
	uint8_t passthrough;
	/*
	 * Appended after passthrough, by the same rule, so a backend built
	 * against the older record keeps its layout.
	 *
	 * The codec's own in-band discontinuous transmission for what this
	 * stream sends: G.729 Annex B, and no other codec of this record
	 * carries such a mode. Set means the encoder runs the codec's own voice
	 * detector, sends a SID frame to close a talkspurt, and sends nothing
	 * through the silence that follows.
	 *
	 * It replaces the generic pair rather than joining it. A backend that
	 * honours the field must take vad and cng down with it, because a
	 * generic detector would close a talkspurt the codec still describes
	 * and RFC 3389 comfort noise would describe a silence the SID already
	 * describes. Receiving is not gated by it at all: a backend decodes an
	 * arriving SID frame whether or not this is set.
	 *
	 * The call layer sets it from the negotiated result, which arms only
	 * when the codec section asked for silence suppression AND the peer
	 * wrote an explicit annexb=yes (uvoice/src/proto/shared/sdp.c). On G.729
	 * it is the only suppression there is: an unarmed G.729 leg reaches the
	 * backend with vad clear as well (media_fill), because the generic
	 * detector leaves a backward-adaptive decoder out of step with the
	 * encoder at every talkspurt. The fax override clears it beside vad and
	 * cng: a data carrier has no talkspurt. A backend with no such mode
	 * ignores the field, as it ignores the canceller policy above.
	 */
	bool annexb;
	/*
	 * Appended after annexb, by the same rule, so a backend built against
	 * the older record keeps its layout.
	 *
	 * The far end's own RTCP address, when its SDP named one (RFC 3605 2.1
	 * a=rtcp with an address). ipver 0 means the peer named no address, and
	 * the backend then sends RTCP to remote.addr. The port is not repeated
	 * here: remote.rtcp_port carries it either way.
	 *
	 * A nonzero ipver must equal remote.ipver. One stream cannot straddle
	 * two families, so rule 4 refuses the mixed description before any
	 * socket moves. Only a backend with HAL_FEAT_RTCP_ADDR ever receives a
	 * nonzero ipver here.
	 */
	struct hal_rtcp_addr rtcp_remote;
	/*
	 * Appended after rtcp_remote, by the same rule, so a backend built
	 * against the older record keeps its layout.
	 *
	 * The longest a media descriptor stays silent, in milliseconds, before
	 * the backend sends an empty datagram on it (RFC 6263 4.1: a receiver
	 * discards it on the RFC 3550 minimum length). Inbound media under
	 * restrict_source 2 is admitted by connection tracking alone, and an
	 * entry exists only after this box sent on the tuple, so the empty
	 * datagram is what makes the entry exist before the far end's first
	 * packet, on a stream that receives before it sends, and what keeps it
	 * from aging out on a stream that stays silent. The call layer passes
	 * half the conntrack udp bound of the moment, capped at the RFC 6263
	 * section 7 interval (account.h, acct_call_view). 0 takes the backend
	 * default. A backend with no such mechanism ignores it: the stub.
	 */
	uint16_t punch_interval_ms;
};

/*
 * The media counters used by CDR and ubus. Units are backend-raw: MOS values
 * are floats of no fixed scale, rfactor is a raw uint32. The core reads them
 * once at call teardown. The HAL has no read-and-clear op, because a
 * read-and-clear that is not atomic loses the counts between its two halves.
 */
struct hal_media_stats {
	uint64_t packets_sent;
	uint64_t packets_recv;
	uint64_t packets_lost;
	uint64_t packets_discarded;
	uint64_t loss_rate;
	uint64_t loss_rate_far;
	uint64_t jitter_avg;
	uint64_t jitter_max;
	uint64_t jitter_far;
	uint64_t rtt_avg;
	uint64_t rtt_max;
	float mos_lq;
	float mos_cq;
	uint32_t rfactor;
	uint64_t bytes_sent;
	uint64_t bytes_recv;
};

/*
 * The receive jitter buffer counters of one stream. The field set, the names,
 * the types and the order are those of libudsp's struct udsp_jb_stats, and
 * the udsp backend copies them one by one: the core includes no udsp header,
 * so the two records stay separate types and no layout assertion binds them
 * (the hal_udsp_stats_fill precedent).
 *
 * What a backend without them answers is HAL_UNSUPPORTED, which is also what
 * a live backend answers for a stream that has not run a tick since its last
 * configuration. A caller therefore never shows that answer as measured
 * zeros: no counters and counters that are all zero are different answers.
 * "voice status" keeps the last cached reading of such a stream, or emits a
 * never row that names no instant (status.c).
 *
 * frames_out, plc, cn, silence and skew_inserts partition the 10 ms output
 * units the buffer produced; late_drop, dup_drop, overflow_drop and
 * hold_drop count the packets it refused. Every time, including
 * resync_discard_ms, is in milliseconds.
 * pool_free is a gauge and not a total: a value that decays across a call is a
 * slot leak.
 */
struct hal_jb_stats {
	uint64_t frames_out;
	uint64_t plc;
	uint64_t cn;
	uint64_t silence;
	uint64_t late_drop;
	uint64_t dup_drop;
	uint64_t overflow_drop;
	uint64_t hold_drop;		/* far-ahead refusals on a parked clock;
					 * distinct from late_drop */
	uint64_t resync_discard_ms;	/* reset-discarded playable audio, in
					 * milliseconds, not packets */
	uint64_t resyncs;		/* SSRC change or timestamp jump */
	uint64_t starve_resyncs;	/* far end resumed behind a parked clock */
	uint64_t cn_reanchors;		/* audio resumed behind comfort noise */
	uint64_t underruns;
	uint64_t skew_drops;
	uint64_t skew_inserts;
	uint16_t target_ms;
	uint16_t depth_ms;
	uint16_t jitter_ms;
	uint16_t pool_free;		/* free payload slots, a gauge */
	/*
	 * enum hal_jb_mode, appended: the playout mode the buffer runs, as the
	 * engine accepted it (libudsp struct udsp_jb_stats mode; the udsp
	 * backend pins the two enums by name). Under a passthrough the udsp
	 * engine pins HAL_JB_FIXED at the depth the line adapted to, so
	 * target_ms then differs from the configured jb_init_ms on purpose.
	 * hal_jb_mode_name() renders it.
	 */
	uint8_t mode;
};

/*
 * The line echo canceller state of one line. Per line and not per stream,
 * because the canceller belongs to the analog line. The field set, the names,
 * the types and the order are those of libudsp's struct udsp_lec_stats, and
 * the udsp backend copies them one by one for the reason struct hal_jb_stats
 * gives: the core includes no udsp header, so the two records stay separate
 * types and no layout assertion binds them.
 *
 * What a backend without one answers is HAL_UNSUPPORTED, which is also what a
 * live backend answers for a line that holds NO canceller instance: on udsp
 * the instance is made at stream config and released with the last stream of
 * the line, so a line between calls has none. Every line that holds an
 * instance answers HAL_OK, whatever ran on the frame: a canceller the modem
 * policy skips whole, or one the channel configuration switched off, reads
 * ec_active 0 with its counters standing still, and a canceller that has run
 * no frame yet reads frames 0 and the tail it applied. The op itself makes no
 * row: no canceller and a canceller that ran nothing are different answers.
 * (What "voice status" does with a HAL_UNSUPPORTED line is a northbound choice
 * made in status.c: it keeps the last cached reading if there is one, else
 * emits a never row, so the lec array is a constant grid. That is
 * presentation, not this contract.)
 *
 * Every field is an integer and every scaled field carries its unit in its
 * name, because the canceller holds these quantities as floats and a fixed
 * point integer never rounds twice. hold_frames against frames is the hold
 * ratio; neither is a rate on its own. erle_mdb is the tracked echo return
 * loss enhancement in milli-decibels, so 20 dB reads 20000, and it is signed.
 * coef_energy_micro is the energy of the filter times 1000000: a filter that
 * never adapted reads 0, one converged against a 30 dB hybrid reads 1000, and
 * the divergence bound of the library is 4000000. echo_gain_micro is the
 * learned lower bound of the hybrid's power gain on the same scale; it is a
 * minimum statistic and under-reads by design.
 *
 * ref_delay_samples and ref_ready are the reference alignment of the line and
 * not of one canceller. The backend measures one loop delay per device. Each
 * line applies that measurement on its own, so two lines can report different
 * values. ref_delay_samples is the delay the line applied, in samples at the
 * line rate. On udsp it is the loop the line stands on, less one period,
 * inside the reach of the canceller. A loop of 240 at 8 kHz (30 ms)
 * therefore reads 160.
 *
 * ref_ready is 1 once the reference of the line stands. While it is 0 the
 * engine holds adaptation, because a filter that adapts against a reference
 * timed wrong diverges instead of converging. A line whose canceller starts
 * during a measurement reports 0 and 0 until the measurement completes. If the
 * measurement restarts past the allowance of the backend, the line applies the
 * last completed one, or a nominal delay before the first. A backend that
 * measures no loop delay reports 0 and 0.
 *
 * nlp_enabled is the one field that measures nothing: it is the switch the
 * policy asks of the nonlinear processor, 1 normally and 0 under a fax policy,
 * under a channel configuration that asks for no processor, and on any frame
 * the filter did not run. That is what tells it apart from nlp_active above,
 * which is the per-frame gate: a row with nlp_enabled 1 and nlp_active 0 ran a
 * frame the processor was on for and did not act on. The modem policy, a
 * channel configuration that switched the canceller off and an open loop each
 * stop the filter, and loop_open names the open loop.
 *
 * ec_active and passthrough are appended. ec_active is MEASURED: 1 when the
 * filter ran on the frame the record describes, 0 when the modem policy
 * skipped it whole, the channel configuration switched it off, the loop was
 * open (loop_open reads 1 there), or the line's media did not run.
 * passthrough is the enum hal_passthrough kind the engine applied to that
 * frame, its own reading of the policy, which the northbound prefers over the
 * daemon's resolution while the reading is live. A T.38
 * stream reads HAL_PASSTHROUGH_MODEM there, the canceller policy of an image
 * session and not the call's mode.
 */
struct hal_lec_stats {
	uint32_t tail_ms;		/* the tail the canceller applied */
	uint32_t taps;
	uint32_t frames;
	uint32_t hold_frames;		/* frames whose update did not stand */
	uint32_t diverge_resets;
	uint32_t probes;		/* adaptation trials out of a hold */
	int32_t erle_mdb;		/* milli-decibels */
	uint32_t coef_energy_micro;	/* filter energy times 1e6 */
	uint32_t echo_gain_micro;	/* learned hybrid gain times 1e6 */
	uint8_t double_talk;		/* 0 or 1 */
	uint8_t nlp_active;		/* 0 or 1 */
	uint32_t ref_delay_samples;	/* applied loop delay, in samples */
	uint8_t ref_ready;		/* 1 once the measurement stands */
	uint8_t nlp_enabled;		/* the switch the policy asks for */
	uint8_t ec_active;		/* 1 when the filter ran on this frame */
	uint8_t passthrough;		/* enum hal_passthrough the frame ran under */
	/* Appended: the frames the filter did not run its taps on because
	 * the whole reference window was digital silence. Each one is also a
	 * hold frame, so skip_frames against hold_frames tells a silent far
	 * end from a quiet or a refused one. */
	uint32_t skip_frames;
	/* Appended: the resets the line's filter took from the reference. Two
	 * causes ask for a reset. The first is a measured loop delay that
	 * moved beyond one period of the applied one, compared before the
	 * reach clamp. The engine re-latches that delay after every catch-up
	 * episode, and a re-latch inside the tolerance counts nothing. The
	 * second is a replacement engine, whatever it goes on to measure.
	 * Several moves decided while the filter did not run make one reset
	 * and one count on the frame it runs again. A canceller created after
	 * a move starts at 0 and is not reset by it. The count belongs to the
	 * line. It survives the reset it names, like diverge_resets. It reads
	 * 0 for a canceller the line took after a release. */
	uint32_t retimes;
	/* Appended: the verdicts of the nonlinear processor, one count per
	 * frame the filter ran with the processor on. A refused frame counts
	 * under the first condition that refused it, in this order: a quiet
	 * reference window, double talk, a frame the echo estimate does not
	 * explain, the ERLE gate. An eligible frame counts as a wait until the
	 * engage run completes, and as active from the frame the processor
	 * acts on. The six add up to frames less the frames the processor was
	 * off for. A re-time zeroes them with frames, and a release ends them
	 * with the instance. */
	uint32_t nlp_active_frames;
	uint32_t nlp_wait_frames;
	uint32_t nlp_erle_frames;
	uint32_t nlp_unlike_frames;
	uint32_t nlp_dt_frames;
	uint32_t nlp_quiet_frames;
	/* Appended: the index of the largest coefficient, counted from tap 0
	 * of the applied reference (ref_delay_samples). The reference stands
	 * one period under the loop, so an echo that arrives at the loop reads
	 * one period in: 80 at 8 kHz, 160 at 16 kHz. A live line reads later
	 * by the lag of the path outside the engine rings. It reads 0 while
	 * the filter holds no energy. Read it beside coef_energy_micro,
	 * because a filter aligned on its first tap also reads 0. */
	uint32_t coef_peak_tap;
	/* Appended: 1 when the tick held the filter because the loop was open.
	 * The run reads the level that the previous tick consumed, so the flag
	 * follows the loop one tick late in both directions. It is the reason
	 * a record reads ec_active 0 with the counters of a call behind it,
	 * and it tells that record apart from a policy or a channel
	 * configuration that switched the filter off. 0 or 1. */
	uint8_t loop_open;
};

/*
 * The newest line-monitor reading of one analog line: the line voltages and
 * currents, the battery, the HVIC power and the DC-DC converter state. The
 * field set, the names, the types and the order are those of libudsp's
 * struct udsp_monitor, copied one by one for the reason struct hal_lec_stats
 * gives. A backend without a monitor answers HAL_UNSUPPORTED, which is also
 * the answer for a line outside hal_cap; a dead line answers HAL_FATAL with
 * the record untouched, as line_lec_stats_get does; every other line answers
 * HAL_OK, and valid 0 says no reading landed yet (the measurement fields and
 * stamp_ms are then unset, the three fault fields are always defined). stale
 * 1 says the last read answered no data and the values are the previous
 * reading. Units: mV, uA, mW; stamp_ms is CLOCK_MONOTONIC milliseconds
 * modulo 2^32, the clock every other stamp_ms of the daemon uses (uvoice.h),
 * so age_text of the CLI applies. The decoded booleans read the raw words
 * the row also carries: dcdc_down is PD_DCDC bit 20, ovlo is DCDC_STATUS bit
 * 22, settled says the requested and actual linefeed states agree in an
 * active or on-hook-transmission state, voc_err, lcr and rtp are the
 * loop-status bits. fault_bits, fault_open and fault_held are the backend
 * engine's fault state of the line at the call: the last fault set read,
 * whether the feed is held open for a fault trial or a recovery, whether a
 * hold is in force that no fault trial ends (it lifts on the next start, or
 * for a clock fault when the clocks return).
 */
struct hal_monitor {
	uint32_t stamp_ms;
	uint8_t valid;
	uint8_t stale;
	int32_t vtr_mv;
	int32_t vtip_mv;
	int32_t vring_mv;
	int32_t vlong_mv;
	int32_t vbat_mv;
	int32_t vdc_mv;
	int32_t itr_ua;
	int32_t itip_ua;
	int32_t iring_ua;
	int32_t ilong_ua;
	int32_t p_hvic_mw;
	int32_t vbat_tgt_mv;
	int32_t vbat_irq_th_mv;
	uint32_t pd_dcdc;
	uint32_t dcdc_status;
	uint8_t lcrrtp;
	uint8_t linefeed;
	uint8_t dcdc_down;
	uint8_t ovlo;
	uint8_t settled;
	uint8_t voc_err;
	uint8_t lcr;
	uint8_t rtp;
	uint32_t fault_bits;
	uint8_t fault_open;
	uint8_t fault_held;
};

/*
 * The runtime diagnostic mask. debug_set stores it in the
 * backend, which turns the bits into whatever its own engine understands. A
 * clear mask is the shipping state and costs the media path nothing.
 */
enum hal_debug_flag {
	HAL_DEBUG_JB_TRACE	= (1u << 0),	/* one log line per jitter
						 * buffer decision that
						 * changed something */
	HAL_DEBUG_SLIC_LOG	= (1u << 1),	/* the backend's debug lines:
						 * the SLIC plug-in's trace
						 * and its SDK's messages */
	HAL_DEBUG_PKT_TRACE	= (1u << 2),	/* every media datagram of every
						 * stream in the clear, taken
						 * before the backend protects a
						 * transmit packet and after it
						 * opens a receive one, so an
						 * SRTP call reads here and
						 * nowhere else. The backend
						 * drains its engine and writes
						 * the records to the call
						 * trace rings. A backend with
						 * no such trace ignores the
						 * bit. */
	HAL_DEBUG_LEC_CAPTURE	= (1u << 3),	/* every frame of every line
						 * canceller: the reference,
						 * the microphone before the
						 * cancel, the output after the
						 * processor, and the control
						 * and verdict state. The
						 * backend drains its engine and
						 * writes the records to the
						 * call trace rings. A backend
						 * with no such capture ignores
						 * the bit. */
};

/*
 * The transport and encryption counters of one stream, the field set, the
 * names and the types of libudsp's struct udsp_stream_diag. The udsp backend
 * copies them one by one, for the reason struct hal_jb_stats gives: the core
 * includes no udsp header, so the two records stay separate types and no
 * layout assertion binds them.
 *
 * net_drops, net_stale and net_trunc are the receive path's refusals, tx_fail
 * every failed send and punch_sent the empty datagrams that hold the tuple
 * open. The five SRTP counters read 0 on a plain RTP stream. An adoption
 * restarts the five transport counters, a new session restarts the five SRTP
 * ones, and a rekey does not.
 *
 * A backend without them answers HAL_UNSUPPORTED, which is also what a live
 * backend answers for an idle stream. A caller skips that stream rather than
 * showing a row of zeros.
 */
struct hal_stream_diag {
	uint64_t net_drops;		/* full ring: newest datagrams refused */
	uint64_t net_stale;		/* full ring: oldest datagrams dropped */
	uint64_t net_trunc;		/* datagrams too long or refused by SRTP */
	uint64_t tx_fail;
	uint64_t punch_sent;
	uint64_t srtp_protect_ok;
	uint64_t srtp_protect_fail;
	uint64_t srtp_unprotect_ok;
	uint64_t srtp_unprotect_auth_fail;
	uint64_t srtp_unprotect_replay;
};

/*
 * One snapshot of the backend engine, the field set, the names and the types
 * of libudsp's struct udsp_engine_diag, copied the way struct hal_stream_diag
 * is.
 *
 * interval is the tick interval histogram. Its upper edges are 9, 11, 12, 15,
 * 20, 30 and 50 milliseconds, and the last bucket holds everything above
 * 50 ms. Both records carry eight buckets and the backend asserts that.
 *
 * The backend publishes the snapshot once per watchdog period, so a getter
 * answers HAL_UNSUPPORTED before the first publication and HAL_OK after it.
 */
struct hal_engine_diag {
	uint64_t serial;
	uint64_t ticks;
	uint64_t interval[8];
	uint64_t interval_min_us;
	uint64_t interval_max_us;
	uint64_t capture_stalls;
	uint64_t delay_stalls;
	uint64_t xruns;
	uint64_t holds;
	uint64_t capture_drops;
	uint64_t trim_drops;
	uint64_t playback_expired;
	uint64_t recover_failures;
	uint64_t read_errors;
	uint64_t write_errors;
	uint64_t cal_restarts;
	int32_t delay_ref;
	int32_t loop_ref;
	int32_t delay_drift_min;
	int32_t delay_drift_max;
	uint32_t granted_periods;
	uint32_t period_us;
	bool running;
	bool stopped_fatal;
	bool delay_calibrated;
};

/*
 * A fax transition changes stream state (VAD, CNG and silence compression)
 * and channel state (the echo canceller). A backend can hold the two in
 * separate records that it programs with separate calls, so the transition is
 * not atomic there. One op applies both, so no call can sit in a
 * half-converted state. Both V.152 VBD and manual toggling are expressible
 * here.
 */
enum hal_fax_mode {
	HAL_FAX_OFF = 0,
	HAL_FAX_PASSTHROUGH,		/* G.711 passthrough, the shipping mode */
	HAL_FAX_VBD,			/* V.152, needs HAL_FEAT_VBD */
	/* Unused by design. A T.38 transition programs the stream alone,
	 * through stream_config_set with codec HAL_CODEC_T38 and the hal_media
	 * t38 block, and it touches no channel profile at all. */
	HAL_FAX_T38,
};

/* The V.152 data kind; only meaningful when mode is HAL_FAX_VBD. */
enum hal_modem_fax {
	HAL_MODEM_FAX_NONE = 0,
	HAL_MODEM_FAX_MODEM,
	HAL_MODEM_FAX_FAX,
};

/*
 * One fax or modem profile. The udsp backend hands mode, codec and codec_pt to
 * libudsp, which applies the whole profile under its own lock. That backend
 * reads no other field: libudsp fixes the echo canceller, the suppression
 * switches, silence compression, the jitter buffer and the data kind per
 * mode.
 */
struct hal_fax_profile {
	uint8_t mode;			/* enum hal_fax_mode */
	uint8_t codec;			/* enum hal_codec to switch to */
	uint8_t codec_pt;
	uint8_t modem_fax_flag;		/* enum hal_modem_fax */
	bool ec_disable;
	bool vad_disable;
	bool cng_disable;
	bool silence_comp_disable;
	/*
	 * Appended, so a backend built against the older record keeps its
	 * layout. An adaptive jitter buffer re-times the carrier while the page
	 * runs, so a backend that reads this field holds the buffer fixed for
	 * the length of the fax.
	 *
	 * The depth of the fixed buffer is the backend's choice and is not
	 * expressible here. The udsp backend carries the depth the buffer
	 * adapted to at the switch, because that depth is what the line
	 * measured and the configured target is not.
	 */
	bool jb_fixed;
};

/*
 * conf_set arms or clears a local three-way conference on the line. The udsp
 * backend implements it with a mix-minus matrix and advertises HAL_FEAT_CONF.
 * A backend without a mixer returns HAL_UNSUPPORTED and does not advertise
 * HAL_FEAT_CONF, and the core then keeps the two-call swap. The core joins the
 * two legs from the two-call flash and calls conf_set on the join and on every
 * conference exit. conf_set returns HAL_UNSUPPORTED for an off on a line with
 * no configured stream, which the core treats as done.
 */
enum hal_conf_mode {
	HAL_CONF_OFF = 0,
	HAL_CONF_LOCAL_MIX,
};

/*
 * The delayed loop of line_loop_set: SELF plays the line its own capture,
 * PEER plays it the capture of another line, OFF plays neither.
 */
enum hal_loop_mode {
	HAL_LOOP_OFF = 0,
	HAL_LOOP_SELF,
	HAL_LOOP_PEER,
};

/*
 * One loop request. It is a record rather than an argument list, the
 * struct hal_ring and struct hal_hook_timing precedent: a record can grow at
 * its end, and the records of this header are append only.
 *
 * mode is enum hal_loop_mode. peer is the line SELF and OFF do not read.
 * delay_ms is the playout delay a backend rounds up to whole ticks, so 0 asks
 * for the shortest delay the backend carries.
 */
struct hal_loop {
	uint8_t mode;			/* enum hal_loop_mode */
	uint8_t peer;
	uint16_t delay_ms;
};

enum hal_line_test {
	HAL_LINE_TEST_GR909 = 0,
	HAL_LINE_TEST_ROH,
	HAL_LINE_TEST_CAPACITANCE,
	HAL_LINE_TEST_LOOP,
};

enum hal_event_type {
	HAL_EVENT_NONE = 0,
	HAL_EVENT_HOOK,			/* seize, release, flash, pulse */
	HAL_EVENT_DIGIT,		/* DTMF detected, local or from the wire */
	HAL_EVENT_TONE,			/* non-DTMF tone detected, see hal_tone_detect */
	HAL_EVENT_LINE,			/* line state changed under us */
	HAL_EVENT_RING_START,
	HAL_EVENT_RING_END,
	HAL_EVENT_CID_SENT,		/* type 1 or type 2 transmission finished */
	HAL_EVENT_CID_RECV,		/* FXO path; the widest payload */
	HAL_EVENT_CID2_TIMEOUT,		/* type 2 CID got no terminal ACK */
	HAL_EVENT_RTP_FIRST_RX,
	HAL_EVENT_RTP_FIRST_TX,
	HAL_EVENT_JB_UPDATE,
	HAL_EVENT_T38,
	HAL_EVENT_SLIC_FAULT,
	HAL_EVENT_AUDIO_END,
	HAL_EVENT_JOB_DONE,		/* completion of a SLOW op, carries a token */
	HAL_EVENT_LEC_POLICY,		/* the answer-tone disabler moved the
					 * canceller policy of a line */
	HAL_EVENT_INPUT,
};

enum hal_key {
	HAL_KEY_SPEAKER = 0,
	HAL_KEY_HEADSET,
	HAL_KEY_MUTE,
	HAL_KEY_VOL_UP,
	HAL_KEY_VOL_DOWN,
	HAL_KEY_HOLD,
	HAL_KEY_TRANSFER,
	HAL_KEY_CONFERENCE,
	HAL_KEY_DND,
	HAL_KEY_MESSAGE,
};

enum hal_audio_path {
	HAL_AUDIO_NONE = 0,
	HAL_AUDIO_HANDSET,
	HAL_AUDIO_HEADSET,
	HAL_AUDIO_SPEAKER,
	HAL_AUDIO_HANDSET_SPEAKER,
	HAL_AUDIO_HEADSET_SPEAKER,
};

#define HAL_VOLUME_MAX		8

enum hal_led {
	HAL_LED_MUTE = 0,
	HAL_LED_SPEAKER,
	HAL_LED_HEADSET,
	HAL_LED_MESSAGE,
	HAL_LED_CALL,
};

enum hal_led_mode {
	HAL_LED_OFF = 0,
	HAL_LED_ON,
	HAL_LED_BLINK_SLOW,
	HAL_LED_BLINK_FAST,
	HAL_LED_HEARTBEAT,
};

enum hal_ring_sound {
	HAL_RING_CLASSIC = 0,
	HAL_RING_HIGH,
	HAL_RING_LOW,
	HAL_RING_FAST,
	HAL_RING_SLOW,
	HAL_RING_DUAL,
	__HAL_RING_SOUND_MAX,
};

struct hal_audio_tune {
	int handset_mic_gain;
	int headset_mic_gain;
	int speaker_mic_gain;
	int handset_out_gain;
	int headset_out_gain;
	enum hal_ring_sound ring_sound;
};

/*
 * Detected tones that are not DTMF. A backend reports a DTMF digit as
 * HAL_EVENT_DIGIT and every tone below as HAL_EVENT_TONE. The modem and fax
 * tones are what triggers the fax passthrough switch, so they must reach the
 * core rather than being dropped in the backend.
 */
enum hal_tone_detect {
	HAL_TONE_DET_OTHER = 0,
	HAL_TONE_DET_CNG,		/* calling fax */
	HAL_TONE_DET_CED,		/* answering fax or modem */
	HAL_TONE_DET_ANS,
	HAL_TONE_DET_ANSAM,
	HAL_TONE_DET_ANSBAR,
	HAL_TONE_DET_V21_PREAMBLE,
	HAL_TONE_DET_FAX_CM,
	HAL_TONE_DET_FAX_TIMEOUT,
	HAL_TONE_DET_MODEM,
};

enum hal_tone_dir {
	HAL_TONE_DIR_LOCAL = 0,		/* from the phone */
	HAL_TONE_DIR_REMOTE,		/* from the wire */
};

/*
 * Why the answer-tone disabler of a backend moved the canceller policy of a
 * line (HAL_EVENT_LEC_POLICY). LATCH is a kind the disabler took where the line
 * held none, or a stronger kind a later answer tone asked for. DEMOTE is the
 * one weaker kind a standing latch admits: a V.21 preamble takes a modem latch
 * to the fax kind. Two releases are the disabler's own rules, the holding band
 * that fell quiet (G.168 7.8) and the governing stream count of the line that
 * moved. RELEASE_CALLER is the third, and it is the only one a caller asks for:
 * line_lec_latch_clear below dropped a latch the call layer decided was not a
 * fax or a modem after all.
 *
 * The reason also says which of the record's fields carry anything: a LATCH and
 * a DEMOTE name the tone and the feed it arrived on, a release names neither.
 *
 * Only the udsp backend produces these records (udsp/src/core/chan.c
 * chan_fax_bypass_tick).
 */
enum hal_lec_reason {
	HAL_LEC_REASON_LATCH = 0,
	HAL_LEC_REASON_DEMOTE,
	HAL_LEC_REASON_RELEASE_QUIET,
	HAL_LEC_REASON_RELEASE_TOPOLOGY,
	HAL_LEC_REASON_RELEASE_CALLER,
};

/*
 * Fixed size POD, one per SOCK_SEQPACKET datagram. The union is sized by
 * HAL_EVENT_CID_RECV: three HAL_CID_STR_MAX strings, the widest payload of
 * any event. Nothing variable length may enter this record; bulk results
 * (line test data, stream config dumps) stay in the backend and are fetched
 * with a fast op after the completion arrives.
 */
struct hal_event {
	uint16_t type;			/* enum hal_event_type */
	uint16_t line;			/* 0 based, < hal_cap.lines */
	uint16_t stream;		/* 0 based, < hal_cap.streams_per_chan */
	uint16_t seq;			/* hardware events only, from 1; wraps. 0 on
					 * HAL_EVENT_JOB_DONE, which is outside seq
					 * accounting */
	uint32_t stamp_ms;		/* backend tick, monotonic within one run */
	union {
		struct {
			uint8_t state;		/* enum hal_hook */
			uint8_t pulses;		/* pulse count, 1..20 */
			uint8_t pulse_invert;	/* 1 when the region that
						 * counted a HAL_HOOK_PULSE
						 * train codes a dialed N as
						 * 10 - N pulses, else 0, and
						 * 0 on every other state. A
						 * region swap can complete
						 * before a queued train
						 * arrives, so the core maps
						 * the count with this byte */
		} hook;
		struct {
			char digit;
			uint8_t remote;		/* 0 = from the phone, 1 = from the wire */
			uint16_t duration_ms;
		} digit;
		struct {
			uint16_t code;		/* enum hal_tone_detect */
			uint8_t direction;	/* enum hal_tone_dir */
			uint8_t end;		/* 0 on a start row, 1 on an end
						 * row. It sits in the padding
						 * byte after direction, so no
						 * offset moves and a producer
						 * that zero-fills the record
						 * keeps reporting start rows */
			uint16_t duration_ms;
		} tone;
		struct {
			uint8_t state;		/* enum hal_line_state */
			uint16_t ring_count;
		} line;
		struct {
			char number[HAL_CID_STR_MAX];
			char name[HAL_CID_STR_MAX];
			char datetime[HAL_CID_STR_MAX];
		} cid;
		struct {
			uint8_t kind;		/* enum hal_cid_kind */
			int8_t status;		/* 0 = delivered, < 0 = the
						 * backend's failure code */
		} cid_sent;
		struct {
			uint64_t total;
			uint64_t drop;
			uint32_t size_ms;
			uint32_t jitter_avg;
		} jb;
		struct {
			/* 0 = disconnect, 1 = payload type change */
			uint8_t state;
			uint8_t payload_type;
		} t38;
		struct {
			uint32_t code;		/* opaque producer word; the
						 * udsp backend pins its bits
						 * in udsp_events.c. 0 says
						 * the fault on this line
						 * CLEARED and the line may
						 * return to service */
		} fault;
		struct {
			uint32_t token;		/* matches hal_job.token */
			int32_t status;		/* enum hal_status */
		} job;
		struct {
			uint8_t kind;		/* enum hal_passthrough, as the
						 * disabler's latch holds it
						 * after the move; a committed
						 * description still wins over
						 * the latch */
			uint8_t reason;		/* enum hal_lec_reason */
			uint16_t tone;		/* enum hal_tone_detect that
						 * latched, 0 on a release */
			uint8_t direction;	/* enum hal_tone_dir the tone
						 * arrived on, 0 on a release;
						 * read the reason first, 0 is
						 * also HAL_TONE_DIR_LOCAL */
		} lec;
		struct {
			uint8_t key;
			uint8_t pressed;
		} input;
	} u;
};

/* Guards against a payload sneaking into the record that would make the pump
 * write more than a socket buffer can absorb in one datagram. */
_Static_assert(sizeof(struct hal_event) <= 160, "hal_event outgrew its budget");

struct hal;
struct hal_job;

/*
 * Completion callback for a SLOW op. Called on the uloop thread only, from the
 * event reader, when the matching HAL_EVENT_JOB_DONE record arrives. A backend
 * worker never calls it directly: it writes the record and the core dispatches.
 * ev is the completion record, for ops that carry payload in it.
 */
typedef void (*hal_job_cb)(struct hal_job *job, enum hal_status status,
			   const struct hal_event *ev);

/*
 * Caller-owned. The core allocates it, fills cb and owner, and passes it to a
 * SLOW op. The op fills token before it returns. The core keeps the job on its
 * pending list until the completion record with that token arrives, or until
 * the backstop timer fires. The backend stores neither the pointer nor the
 * callback.
 *
 * owner is an opaque handle, never a pointer. A slow op can outlive the line,
 * call or account that started it, so a raw pointer would be a use after free
 * waiting for a slow GR-909 sweep. The handle carries an index and a
 * generation; the callback looks it up in the core owner table and drops the
 * result when the generation no longer matches. 0 means the job belongs to the
 * daemon itself and has nothing to validate, as for DSP bring-up.
 *
 * list, timeout, and budget_ms are core private. The backend must not touch
 * them. A job may carry its own budget. The core uses the 30 s default unless
 * the caller supplies one.
 */
struct hal_job {
	struct list_head list;
	struct uloop_timeout timeout;
	hal_job_cb cb;
	uint64_t owner;
	uint32_t budget_ms;
	uint32_t token;
};

struct hal_ops {
	const char *name;

	/*
	 * Lifecycle
	 *
	 * fast: open() attaches to the device, probes hal_cap and runs the
	 *   start-time recovery pass: stop every stream, stop every ring,
	 *   force every line idle. It runs once before
	 *   uloop_run(), so it may block for the device attach; no other op
	 *   may. event_fd is the socketpair write end, already O_NONBLOCK; the
	 *   backend keeps it for the pump and never closes it.
	 */
	struct hal *(*open)(int event_fd);
	/* fast: stops the pump thread and detaches. Idempotent. */
	void (*close)(struct hal *h);
	/* fast: probed values, valid until close(). Never NULL. */
	const struct hal_cap *(*cap_get)(struct hal *h);
	/* fast: pump counters, including the EAGAIN drop count. */
	void (*counters_get)(struct hal *h, struct hal_counters *out);

	/*
	 * Fast ops
	 * One short call into the driver or engine each. Called from the uloop
	 * thread only.
	 */

	/* fast: battery feed and ring state. HAL_LINE_DOWN takes the battery
	 * off the port. The core writes it for a port the config disables and
	 * at close, and the forward disconnect window writes it for its
	 * removal; HAL_LINE_ACTIVE is the raise for a port the config enables,
	 * made before the line enters service. The udsp backend brings every
	 * line up open, so the core's raise is what feeds a port. */
	enum hal_status (*line_state_set)(struct hal *h, unsigned int line,
					  enum hal_line_state state);
	/* fast: forward disconnect; needs HAL_FEAT_POLARITY. */
	enum hal_status (*line_polarity_set)(struct hal *h, unsigned int line,
					     enum hal_polarity pol);
	/* fast: current hook state, for start-up and for resynchronising after
	 * a dropped event. */
	enum hal_status (*line_hook_query)(struct hal *h, unsigned int line,
					   bool *offhook);
	/* fast: pushed once per line at bring-up; the backend decides flash vs
	 * on-hook from these numbers. */
	enum hal_status (*line_hook_timing_set)(struct hal *h, unsigned int line,
						const struct hal_hook_timing *ts);
	/* fast: cadence plus optional type 1 CID; ring runs until
	 * line_ring_stop or hal_ring.duration_ms expires. */
	enum hal_status (*line_ring_start)(struct hal *h, unsigned int line,
					   const struct hal_ring *ring);
	/* fast: also used by the crash recovery pass. */
	enum hal_status (*line_ring_stop)(struct hal *h, unsigned int line);
	/* fast: type 1 between rings, type 2 during a call. Completion arrives
	 * as HAL_EVENT_CID_SENT. */
	enum hal_status (*line_cid_send)(struct hal *h, unsigned int line,
					 enum hal_cid_kind kind,
					 const struct hal_cid *cid);
	/* fast: lamp path, needs HAL_FEAT_MWI_LAMP. The FSK VMWI path is
	 * line_cid_send with hal_cid.mwi set instead; which one a given SLIC
	 * profile honors is not measured. */
	enum hal_status (*line_mwi_set)(struct hal *h, unsigned int line,
					enum hal_mwi mwi);
	/* fast: copies the result of the last completed line_test_start into
	 * buf. Valid only between the completion and the next test. */
	enum hal_status (*line_test_result_get)(struct hal *h, unsigned int line,
						void *buf, size_t buf_len,
						size_t *out_len);

	/* fast: country tone table entry, played toward the phone. */
	enum hal_status (*tone_play)(struct hal *h, unsigned int line,
				     enum hal_tone tone, uint32_t duration_ms);
	/* fast: stops whatever tone_play started on this line. */
	enum hal_status (*tone_stop)(struct hal *h, unsigned int line);
	/* fast: single DTMF digit toward the phone, local generation. */
	enum hal_status (*digit_play)(struct hal *h, unsigned int line, char digit,
				      uint32_t duration_ms);
	/* fast: gains, detector mask and echo canceller. The detector mask
	 * gates DTMF collection, so dial tone waits for this to be applied.
	 * The SLIC pair reaches the analog front end through the backend. */
	enum hal_status (*channel_config_set)(struct hal *h, unsigned int line,
					      const struct hal_channel_cfg *cfg);

	/* fast: configure a stream without starting it; safe to call again on a
	 * running stream for a re-INVITE.
	 * Rules for a backend that programs real hardware, all mandatory:
	 *   1. the backend owns the two media sockets. It creates them on this
	 *      op, binds them to the RTP and RTCP ports of hal_media.local,
	 *      sets IP_TOS or IPV6_TCLASS from the two DSCP fields itself,
	 *      hands the descriptors to its DSP or engine, and connects them
	 *      to hal_media.remote before the start. A re-application keeps
	 *      the same sockets and connects again only when the far end
	 *      moved; a local address or port move stops the stream, closes
	 *      the sockets and makes new ones.
	 *   2. the DSCP of rule 1 is the one that reaches the wire. The
	 *      backend asks no lower layer to mark the packets a second time.
	 *   3. a refusal from the DSP or engine maps onto enum hal_status by
	 *      section 3: HAL_UNSUPPORTED for a parameter the backend cannot
	 *      program, HAL_TRANSIENT for a recoverable failure and HAL_FATAL
	 *      for the rest. A refusal that arrives in an out field while the
	 *      call itself reports success is still a refusal.
	 *   4. what the backend can program is tested before anything is
	 *      touched, so a refusal leaves a running stream exactly as it was.
	 *      Four shapes answer HAL_UNSUPPORTED: an ipver that is neither 4
	 *      nor 6; a description whose local and remote families differ, or
	 *      whose rtcp_remote names a third family; an IPv6 address on a
	 *      backend that leaves HAL_FEAT_IPV6_MEDIA clear; and an srtp suite
	 *      outside enum hal_srtp_suite. Media with no local port or no
	 *      remote port answers HAL_FATAL, none of them stopping a live
	 *      stream. A failure after the backend hands the sockets over
	 *      keeps every socket that the DSP or engine can still hold, for
	 *      the next ordered stop to close. */
	enum hal_status (*stream_config_set)(struct hal *h, unsigned int line,
					     unsigned int stream,
					     const struct hal_media *media);
	/* fast: starts a configured stream. Media then flows without the uloop
	 * thread, on the sockets the backend bound and handed down. Who makes
	 * and reads the packets is the backend's business and
	 * HAL_FEAT_KERNEL_RTP is where it says so: a backend that hands the
	 * descriptors to a kernel DSP sets the bit, and a user space backend
	 * such as udsp runs the packet loop on its own threads and leaves it
	 * clear. Either way the daemon holds those sockets open for the whole
	 * life of the stream and never reads them. */
	enum hal_status (*stream_start)(struct hal *h, unsigned int line,
					unsigned int stream);
	/* fast: stops one stream; the start-time recovery pass stops all of
	 * them. The backend closes the two media sockets of the stream after
	 * the stop, so the stream must be configured again before it can start
	 * again. */
	enum hal_status (*stream_stop)(struct hal *h, unsigned int line,
				       unsigned int stream);
	/* fast: RFC 2833 relay of one digit. Suppressed by the core when the
	 * DTMF mode is INFO so a digit is never sent twice. */
	enum hal_status (*stream_dtmf_send)(struct hal *h, unsigned int line,
					    unsigned int stream, char digit,
					    uint32_t duration_ms);
	/* fast: copies the result of the last completed stream_stats_start.
	 * Valid until the next stream_stats_start on the same stream. */
	enum hal_status (*stream_stats_result_get)(struct hal *h, unsigned int line,
						   unsigned int stream,
						   struct hal_media_stats *out);
	/* fast: combined stream plus channel change, see struct
	 * hal_fax_profile. The udsp backend forwards the profile, and libudsp
	 * applies both halves under its own lock. A backend that programs the
	 * stream half and the channel half with separate calls snapshots the
	 * pre-fax stream and channel state, applies the stream change first and
	 * the channel change second. If the channel step fails it restores the
	 * stream step and returns the channel error. If the restore also fails
	 * it returns HAL_FATAL and the core tears the call down, because the
	 * media state is then unknown.
	 *
	 * HAL_FAX_PASSTHROUGH arms the modem kind (hal_media.passthrough),
	 * because a CED answer says a data device answered and nothing yet says
	 * which kind it is. The fax kind travels on the description instead,
	 * and so does every later re-assertion of either kind. A backend that
	 * records the passthrough on the description therefore leaves
	 * HAL_FAX_OFF nothing to restore once a later stream_config_set
	 * replaced that description; the core clears the kind through the
	 * description as well. */
	enum hal_status (*fax_profile_apply)(struct hal *h, unsigned int line,
					     unsigned int stream,
					     const struct hal_fax_profile *fax);
	/* fast: three-way audio topology, see enum hal_conf_mode. Returns
	 * HAL_UNSUPPORTED when HAL_FEAT_CONF is clear. */
	enum hal_status (*conf_set)(struct hal *h, unsigned int line,
				    enum hal_conf_mode mode);

	/*
	 * The diagnostic getters, with the line canceller getter beside them.
	 * They sit with the FAST block, beside the ops they belong with, and
	 * that placement shifts every SLOW member below them: it carries no
	 * layout stability and none is claimed. There is no cross-version ABI
	 * to protect here. One build produces one uvoiced, the linker picks the
	 * single backend translation unit, and every backend in the tree
	 * compiles against this header, so a member added anywhere in this
	 * table reaches every backend at once or the build fails. The append
	 * rule governs the records a backend may be handed, not this table.
	 *
	 * All of them are FAST and all of them answer HAL_UNSUPPORTED where
	 * the backend has no path for them; a caller skips what it cannot
	 * read.
	 */

	/* fast: the receive jitter buffer counters of one stream, read from
	 * cached engine state. HAL_UNSUPPORTED for a backend without a jitter
	 * buffer of its own, for a line or stream outside hal_cap, and for a
	 * stream that has not run a tick since its last configuration. */
	enum hal_status (*stream_jb_stats_get)(struct hal *h, unsigned int line,
					       unsigned int stream,
					       struct hal_jb_stats *out);
	/* fast: the media counters stream_stats_start fetches slowly, where
	 * the backend can answer them from cached state instead. A backend
	 * whose numbers need the slow op answers HAL_UNSUPPORTED and the
	 * caller uses stream_stats_start. */
	enum hal_status (*stream_stats_get)(struct hal *h, unsigned int line,
					    unsigned int stream,
					    struct hal_media_stats *out);
	/* fast: the line echo canceller state of one line, read from cached
	 * engine state. HAL_UNSUPPORTED for a backend with no canceller of its
	 * own, for a line outside hal_cap, and for a line that holds no
	 * canceller instance (no stream configured since the last release).
	 * A line with an instance answers HAL_OK whatever the frame ran under:
	 * the modem passthrough, a channel configuration that switched the
	 * canceller off and an open loop all read ec_active 0 with frozen
	 * counters, and loop_open 1 tells the open loop apart. The fax kind
	 * reads ec_active 1 with nlp_enabled 0 for as long as the page
	 * lasts. */
	enum hal_status (*line_lec_stats_get)(struct hal *h, unsigned int line,
					      struct hal_lec_stats *out);
	/* fast: the newest line-monitor reading of one line, from the
	 * backend's cached engine state. Nullable, like line_lec_stats_get:
	 * NULL on a backend that measures no line. HAL_UNSUPPORTED for a line
	 * outside hal_cap, HAL_FATAL for a dead line (record untouched);
	 * HAL_OK otherwise, and valid 0 in the record says no reading landed
	 * yet. */
	enum hal_status (*line_monitor_get)(struct hal *h, unsigned int line,
					    struct hal_monitor *out);
	/* fast: the region the backend holds, from its own cached country
	 * data. Nullable, like line_monitor_get: NULL on a backend that
	 * cannot introspect its tone set, and the core then reads the config
	 * region section instead. Valid only after a region_apply completed;
	 * HAL_UNSUPPORTED before that, with the record untouched. The core
	 * applies every value it carries to each line as it stands; no
	 * per-channel option overrides it. */
	enum hal_status (*region_get)(struct hal *h, struct hal_region *out);
	/* fast: store the enum hal_debug_flag mask. The backend turns the bits
	 * into its own diagnostics and answers HAL_UNSUPPORTED when it carries
	 * none of them; a caller that gets that answer stops asking and
	 * changes nothing else. */
	enum hal_status (*debug_set)(struct hal *h, uint32_t flags);

	/*
	 * Slow ops
	 * Never called on the uloop thread by the backend. Each returns HAL_OK
	 * once the work is accepted and fills job->token; the result arrives as
	 * HAL_EVENT_JOB_DONE and the core then calls job->cb.
	 */

	/* slow: DSP firmware or engine load and channel bring-up. It can take
	 * seconds. The backend owns the order. */
	enum hal_status (*dsp_start)(struct hal *h, struct hal_job *job);
	/* slow: orderly DSP teardown for a clean shutdown. */
	enum hal_status (*dsp_stop)(struct hal *h, struct hal_job *job);
	/* slow: GR-909 sweep, seconds, fills a result buffer in the backend.
	 * Fetch the payload with line_test_result_get after the completion. */
	enum hal_status (*line_test_start)(struct hal *h, unsigned int line,
					   enum hal_line_test test,
					   struct hal_job *job);
	/* slow: media counter read for CDR and ubus (struct hal_media_stats).
	 * Fetch the payload with stream_stats_result_get after the completion.
	 * Callers are CDR finalization and deferred ubus replies, so the
	 * latency is free. */
	enum hal_status (*stream_stats_start)(struct hal *h, unsigned int line,
					      unsigned int stream,
					      struct hal_job *job);
	/* slow: push a country tone set and SLIC hardware profile. iso3 names
	 * the region, and the backend selects its own country data for it. */
	enum hal_status (*region_apply)(struct hal *h, const char *iso3,
					struct hal_job *job);
	/*
	 * fast: play a call-waiting variant toward the phone. variant 0 is the
	 * plain HAL_TONE_CALL_WAITING tone, the unchanged default; 1..N select a
	 * distinctive row of the region's own call-waiting set. A backend
	 * without a variant set may leave this op NULL, and the core plays the
	 * plain tone through tone_play. Stop it through tone_stop, like any tone.
	 * It is a fast tone op and belongs with tone_play. Its place at the end
	 * of the table is no layout rule; the table carries none (see the
	 * diagnostic getters).
	 */
	enum hal_status (*cw_tone_play)(struct hal *h, unsigned int line,
					unsigned int variant, uint32_t duration_ms);
	/*
	 * fast: drop the tone-disabler latch of one line. The call layer asks
	 * for it when it reads an answer tone as a false one and takes the leg
	 * back to voice: the latch otherwise stands for seconds of quiet and
	 * holds the DTMF detector with it. A backend that answers HAL_OK posts
	 * HAL_EVENT_LEC_POLICY with HAL_LEC_REASON_RELEASE_CALLER, and a line
	 * that holds no latch changes nothing and posts nothing. The next
	 * answer tone can latch again. It changes no stream description.
	 *
	 * Nullable, like line_lec_stats_get: NULL on a backend with no
	 * disabler of its own. HAL_UNSUPPORTED for a line outside hal_cap.
	 * It is a fast canceller op and belongs with line_lec_stats_get. Its
	 * place at the end of the table is no layout rule (see the diagnostic
	 * getters).
	 */
	enum hal_status (*line_lec_latch_clear)(struct hal *h,
						unsigned int line);
	/*
	 * fast: the transport and encryption counters of one stream, read
	 * from cached engine state, and the backend engine's own snapshot.
	 * Both are diagnostic getters and belong with line_lec_stats_get.
	 * Their place at the end of the table is no layout rule (see the
	 * diagnostic getters).
	 *
	 * Nullable, like line_lec_stats_get: NULL on a backend that counts
	 * neither, and the caller then renders no diag table at all.
	 * HAL_UNSUPPORTED means the backend has no answer for this call: an
	 * idle stream, a line or stream outside hal_cap, and, for the engine,
	 * a backend that has published no snapshot yet. A refusal clears the
	 * output. A caller skips what it cannot read.
	 */
	enum hal_status (*stream_diag_get)(struct hal *h, unsigned int line,
					   unsigned int stream,
					   struct hal_stream_diag *out);
	enum hal_status (*engine_diag_get)(struct hal *h,
					   struct hal_engine_diag *out);
	/*
	 * fast: arm or clear the delayed loop of one line, the audio path of
	 * the manufacturing test. SELF plays the line its own capture
	 * loop->delay_ms later. PEER plays it the capture of loop->peer, so
	 * two lines armed at each other carry a conversation between the two
	 * ports. HAL_FEAT_LOOP gates it.
	 *
	 * The loop carries the raw capture, taken before the transmit gain,
	 * and only the receive gain applies on the way out. delay_ms rounds up
	 * to whole backend ticks and takes one tick at the low end, so 0 asks
	 * for one tick.
	 *
	 * Refusals, in this order. HAL_UNSUPPORTED for a line outside
	 * hal_cap and for a mode above PEER; then, for SELF and PEER,
	 * HAL_UNSUPPORTED for a delay the backend cannot carry, and under PEER
	 * for a peer outside hal_cap or equal to line. Then HAL_FATAL for a
	 * dead line and HAL_TRANSIENT while the backend engine is not live.
	 * Then, for SELF and PEER alone, HAL_TRANSIENT while the line or the
	 * peer carries a stream, a ring, a caller identity burst or a metallic
	 * line test.
	 *
	 * OFF reads neither peer nor delay_ms and takes no busy refusal. It
	 * can still fail on the lifecycle or on the backend's own transport.
	 *
	 * While a loop stands on a line, line_test_start and stream_config_set
	 * on that line answer HAL_TRANSIENT, so the two exclude each other in
	 * both directions.
	 *
	 * Nullable, like line_lec_latch_clear: NULL on a backend with no loop
	 * of its own, and the core then runs no loop test. It is a fast line
	 * op and belongs with conf_set. Its place at the end of the table is
	 * no layout rule (see the diagnostic getters).
	 */
	enum hal_status (*line_loop_set)(struct hal *h, unsigned int line,
					 const struct hal_loop *loop);
	enum hal_status (*audio_path_set)(struct hal *h,
					  enum hal_audio_path path);
	enum hal_status (*volume_set)(struct hal *h, unsigned int level);
	enum hal_status (*mute_set)(struct hal *h, bool mute);
	enum hal_status (*led_set)(struct hal *h, enum hal_led led,
				   enum hal_led_mode mode);
	enum hal_status (*audio_tune_set)(struct hal *h,
					  const struct hal_audio_tune *tune);
};

/*
 * Defined by exactly one backend translation unit, picked at build time
 * (UVOICE_HAL_STUB or UVOICE_HAL_UDSP). No registration
 * machinery: the linker is the selector.
 */
extern const struct hal_ops hal_backend_ops;

/*
 * The detected tone by name. Every trace that carries a tone code renders it
 * through this, because a number in a log tells a field engineer nothing about
 * the fax that did or did not start. The spelling is the one udsp-cli prints
 * (udsp/src/tools/udsp-cli.c tone_detect_name), so one token names the tone
 * across the daemon and the library's own tool.
 */
static inline const char *hal_tone_detect_name(uint16_t code)
{
	static const char *const names[] = {
		"OTHER", "CNG", "CED", "ANS", "ANSAM", "ANSBAR",
		"V21_PREAMBLE", "FAX_CM", "FAX_TIMEOUT", "MODEM",
	};

	if (code >= sizeof(names) / sizeof(names[0]))
		return "UNKNOWN";

	return names[code];
}

/*
 * The disabler's move and the feed it answered, by name, on the spelling rule
 * of hal_tone_detect_name() above: these are the words udsp-cli prints
 * (udsp/src/tools/udsp-cli.c lec_reason_name and tone_direction_name), so one
 * token names the thing across the daemon's northbound and the library's own
 * tool. The prose forms of event_dispatch.c are a different surface: they read
 * as a sentence in a log and these read as a value on the wire.
 */
static inline const char *hal_lec_reason_name(uint8_t reason)
{
	static const char *const names[] = {
		"LATCH", "DEMOTE", "RELEASE_QUIET", "RELEASE_TOPOLOGY",
		"RELEASE_CALLER",
	};

	if (reason >= sizeof(names) / sizeof(names[0]))
		return "UNKNOWN";

	return names[reason];
}

static inline const char *hal_tone_dir_name(uint8_t direction)
{
	static const char *const names[] = {
		"LOCAL", "REMOTE",
	};

	if (direction >= sizeof(names) / sizeof(names[0]))
		return "UNKNOWN";

	return names[direction];
}

/*
 * The playout mode of a jitter buffer by name, the lower spelling every other
 * word of the status reply uses; the cdr renderer and the media trace both
 * read it, so one token names the mode across the daemon's northbound.
 */
static inline const char *hal_jb_mode_name(uint8_t mode)
{
	static const char *const names[] = { "adaptive", "fixed", "off" };

	if (mode >= sizeof(names) / sizeof(names[0]))
		return "unknown";

	return names[mode];
}

/*
 * The caller identity transport and the flash service by name, the spellings
 * the libudsp region grammar uses (udsp/src/line/region.c cid_format_names and
 * flash_service_names), so the status reply names them the way the region
 * file does.
 */
static inline const char *hal_cid_format_name(uint8_t format)
{
	static const char *const names[] = {
		"bellcore-fsk", "etsi-fsk", "etsi-rpas", "etsi-dtas",
		"dtmf", "ntt", "etsi-lr-dtas", "dtmf-dtas",
	};

	if (format >= sizeof(names) / sizeof(names[0]))
		return "unknown";

	return names[format];
}

static inline const char *hal_flash_service_name(uint8_t service)
{
	static const char *const names[] = { "off", "split", "drop" };

	if (service >= sizeof(names) / sizeof(names[0]))
		return "unknown";

	return names[service];
}

static inline const char *hal_status_name(enum hal_status st)
{
	switch (st) {
	case HAL_OK:
		return "ok";
	case HAL_TRANSIENT:
		return "transient";
	case HAL_UNSUPPORTED:
		return "unsupported";
	case HAL_FATAL:
		return "fatal";
	}

	return "unknown";
}

#endif
