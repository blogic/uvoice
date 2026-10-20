// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#ifndef UDSP_RTP_PRIVATE_H
#define UDSP_RTP_PRIVATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "udsp/udsp.h"

/* Big-endian wire accessors, shared by rtp.c, rtcp.c and telev.c. */
static inline uint16_t rtp_u16_read(const uint8_t *p)
{
	return ((uint16_t)p[0] << 8) | p[1];
}

static inline uint32_t rtp_u32_read(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static inline void rtp_u16_write(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)(value >> 8);
	p[1] = (uint8_t)value;
}

static inline void rtp_u32_write(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)(value >> 24);
	p[1] = (uint8_t)(value >> 16);
	p[2] = (uint8_t)(value >> 8);
	p[3] = (uint8_t)value;
}

static inline void rtp_u24_write(uint8_t *p, int32_t value)
{
	uint32_t wire = (uint32_t)value & 0x00ffffffU;

	p[0] = (uint8_t)(wire >> 16);
	p[1] = (uint8_t)(wire >> 8);
	p[2] = (uint8_t)wire;
}

static inline int32_t rtp_i24_read(const uint8_t *p)
{
	uint32_t value = ((uint32_t)p[0] << 16) |
			 ((uint32_t)p[1] << 8) | p[2];

	if (value & 0x00800000U)
		value |= 0xff000000U;
	return (int32_t)value;
}

#define RTP_HEADER_SIZE		12U
/* The narrowband clock: G.711, G.726, G.722 and G.729 all stamp at 8000 (RFC
 * 3551), and it is what the init calls that state no clock take. A stream
 * states its own through the _cfg calls below; Opus stamps at 48000 (codec.h
 * Rates). */
#define RTP_CLOCK_RATE		8000U
/* A transit delta of this size is a timeline step, not jitter. It mirrors the
 * jitter buffer's timestamp jump bound (jb.c jb_resync_jump, the same 10 s).
 * At that bound the buffer discards its queue and re-anchors, so the estimator
 * has no delay change to smooth. A shorter stall stays RFC 3550 6.4.1 jitter.
 * Ten seconds is the rule at every clock, so the bound scales with it. */
#define RTP_TRANSIT_STEP_SEC	10U
#define RTP_TRANSIT_STEP_TS(clock_rate)	((clock_rate) * RTP_TRANSIT_STEP_SEC)
/* A marked audio packet whose timestamp ran ahead of its arrival by more than
 * this is a sender timeline step as well (RFC 3551 4.1 lets the marker start a
 * new talkspurt), and the estimator re-anchors on it without an observation.
 * A real suppressed silence advances the timestamp and the arrival together,
 * and a delayed packet runs the other way, so both stay 6.4.1 jitter. The
 * value is a policy bound, not a queue: the widest window of any jitter
 * buffer (jb.h UDSP_JB_DEPTH_MAX_MS plus UDSP_JB_PTIME_MAX_MS, which jb.c
 * asserts). It does not follow the stream's own buffer settings, so two
 * units on one line report the same jitter. */
#define RTP_MARKED_STEP_MS	530U
#define RTP_MARKED_STEP_TS(clock_rate)	\
	((clock_rate) * RTP_MARKED_STEP_MS / 1000U)
/* RFC 3550 11: one UDP datagram may carry several RTP packets, so the receive
 * loop iterates. The bound stops a crafted datagram from turning one tick into
 * an unbounded run of puts; a 1500 byte PDU of minimum packets would otherwise
 * reach a hundred. */
#define RTP_PDU_PACKETS_MAX	8U
/* The ceiling on rtp_receiver_pdu_residual_discard's walk. A 1500 byte
 * datagram of payload-free packets holds 125 of them, which no trunk sends;
 * the walk stops here so a crafted PDU cannot buy quadratic work on the
 * engine thread, and the count saturates rather than growing without bound. */
#define RTP_PDU_RESIDUAL_MAX	64U
#define RTP_SEQ_MOD		65536U
#define RTP_MAX_DROPOUT		3000U
#define RTP_MAX_MISORDER	100U
#define RTP_MIN_SEQUENTIAL	2U
/* The payload bound of the packet that probation holds. It is the jitter
 * buffer's slot (jb.h UDSP_JB_PAYLOAD_MAX, which jb.c asserts), because the
 * buffer refuses a larger payload anyway. */
#define RTP_HELD_PAYLOAD_MAX	512U
#define RTP_PT_DISABLED		255U
#define RTCP_CNAME_MAX		255U
#define TELEV_RECENT_COUNT	4U
/* The RFC 4733 duration field, and with it the segment bound of 2.5.1.3. */
#define TELEV_DURATION_MAX	65535U
/* The event ceiling stays 8000 ms at every clock. The RFC 4733 duration field
 * holds 16 bits of the stream's ticks, which 8 s outruns above 8 kHz, so
 * stream_tx_telev segments the event instead of clamping it. The receive half
 * bounds the event it renders toward the line by the same ceiling. */
#define UDSP_TELEV_MAX_MS	8000u
/* The lost-end bound of an event the receive half renders. RFC 4733 2.5.1.2
 * asks a sender to update a running event at least every 50 ms, so an event
 * that carried neither an update nor an end for this long is over. A test
 * shrinks it through UDSP_TEV_PLAY_TIMEOUT_MS (README, test timing). */
#define UDSP_TEV_PLAY_TIMEOUT_MS	120u

enum rtp_payload_kind {
	RTP_PAYLOAD_AUDIO = 0,
	RTP_PAYLOAD_TELEPHONE_EVENT,
	RTP_PAYLOAD_COMFORT_NOISE,
};

struct rtp_entropy {
	/* The caller supplies three independent CSPRNG draws. */
	uint32_t ssrc;
	uint32_t sequence;
	uint32_t timestamp;
};

struct rtp_stats {
	uint64_t packets_sent;
	uint64_t packets_recv;
	uint64_t packets_discarded;
	uint64_t bytes_sent;
	uint64_t bytes_recv;
	/* SR octet counts include payload only. RTP header bytes are excluded. */
	uint64_t octets_sent;
	/* Internal jitter values use the stream clock's timestamp units, which
	 * clock_rate below states; the snapshot converts them to milliseconds. */
	uint64_t jitter_sum;
	uint64_t jitter_samples;
	uint64_t rtt_sum_ms;
	uint64_t rtt_samples;
	uint32_t jitter_max;
	uint32_t ts_steps;	/* transit re-anchors: one outlier counts twice */
	uint32_t jitter_far;
	uint32_t rtt_max_ms;
	uint32_t model_delay_ms;
	uint32_t clock_rate;	/* the stream's RTP clock; the wrapper takes 8000 */
	uint8_t loss_fraction;
	uint8_t loss_fraction_far;
};

struct rtp_sender {
	struct rtp_stats *stats;
	uint32_t ssrc;
	uint32_t timestamp;
	uint16_t sequence;
	uint8_t payload_type;
	uint8_t event_pt;
	uint8_t codec;
	uint8_t ptime_ms;
	uint8_t direction;
	bool marker_pending;
};

enum rtp_held_state {
	RTP_HELD_NONE = 0,
	RTP_HELD_PROBATION,	/* the first packet of a source on probation */
	RTP_HELD_RELEASED,	/* its successor proved the source */
};

/* RFC 3550 A.1 lets a receiver hold the packets of a source on probation
 * instead of discarding them. The receiver keeps a copy of the source's first
 * packet here, because the datagram that carried it does not stay. */
struct rtp_held {
	uint64_t arrival_ns;
	uint32_t timestamp;
	uint32_t bytes;		/* the packet's own length, header included */
	uint16_t sequence;
	uint16_t len;
	uint8_t payload_type;
	uint8_t csrc_count;
	uint8_t state;		/* enum rtp_held_state */
	bool marker;
	uint8_t payload[RTP_HELD_PAYLOAD_MAX];
};

struct rtp_receiver {
	struct rtp_stats *stats;
	uint32_t clock_rate;	/* the stream's RTP clock (RFC 3550 6.4.1 units) */
	uint32_t ssrc;
	uint32_t cycles;
	uint32_t base_seq;
	uint32_t bad_seq;
	uint32_t received;
	uint32_t expected_prior;
	uint32_t received_prior;
	uint32_t jitter_q4;
	int32_t transit;
	uint16_t max_seq;
	uint8_t probation;
	uint8_t audio_pt;
	uint8_t event_pt;
	bool source_valid;
	bool transit_valid;
	bool cng_enabled;
	struct rtp_held held;
};

struct rtp_packet {
	const uint8_t *payload;
	size_t payload_len;
	/* RFC 3550 11: the rest of the lower-layer PDU when the datagram held
	 * more than one RTP packet of this stream, else NULL and 0. The caller
	 * feeds it back to rtp_packet_parse to read the packets after this one,
	 * at most RTP_PDU_PACKETS_MAX times. Valid only when the parse
	 * succeeded; a rejected packet says nothing about what follows it. */
	const uint8_t *tail;
	size_t tail_len;
	uint32_t ssrc;
	uint32_t timestamp;
	uint16_t sequence;
	uint8_t payload_type;
	uint8_t csrc_count;
	enum rtp_payload_kind kind;
	bool marker;
	uint64_t arrival_ns;
};

struct rtcp_report {
	uint32_t ssrc;
	uint32_t highest_seq;
	uint32_t jitter;	/* the stream clock's units, on the wire. */
	uint32_t lsr;
	uint32_t dlsr;
	int32_t cumulative_lost;
	uint8_t fraction_lost;
};

struct rtcp_sender_info {
	uint32_t ssrc;
	uint32_t ntp_msw;
	uint32_t ntp_lsw;
	uint32_t rtp_timestamp;
	uint32_t packet_count;
	uint32_t octet_count;
};

struct rtcp_parse_result {
	struct rtcp_sender_info sender;
	struct rtcp_report report;
	char cname[RTCP_CNAME_MAX + 1U];
	uint32_t receiver_ssrc;
	uint32_t cname_ssrc;
	uint32_t bye_ssrc;
	bool has_sr;
	bool has_rr;
	bool has_report;
	bool has_cname;
	bool has_bye;
};

struct rtcp_state {
	struct rtp_stats *stats;
	uint32_t local_ssrc;
	uint32_t remote_ssrc;
	uint32_t last_sr_lsr;
	uint32_t last_sr_arrival_mono;	/* Monotonic 16.16 time. */
	bool last_sr_valid;
};

struct telev_sender {
	struct rtp_sender *rtp;
	uint32_t timestamp;
	uint16_t duration;
	uint8_t event;
	uint8_t volume;
	uint8_t end_packets;
	bool active;
	bool marker_pending;
	bool ending;
};

struct telev_reported {
	uint32_t end_timestamp;
	uint8_t event;
	bool valid;
};

/* One chain whose digit the timeout already reported, and the timestamp its
 * continuation must start at. The ring holds several because two events can
 * interleave: one identity would evict the other and the evicted chain's late
 * continuation would report its digit a second time. */
struct telev_flushed {
	uint64_t arrival_ns;
	uint32_t next_ts;
	uint8_t event;
	bool valid;
};

/*
 * RFC 4733 2.5.1.3 segmentation, receive half. An event that outlives the
 * 16-bit duration field is cut into segments. The last packet of an
 * intermediate segment carries the duration field at its maximum with the E
 * bit clear, and the next segment of the same event starts at that end, so N
 * segments would otherwise report N digits. Such a packet opens or extends a
 * chain instead of reporting: seg_total accumulates the segments, seg_end_ts
 * is where the continuation must start, and the report goes out on the packet
 * that carries the E bit. A chain whose continuation never arrives reports on
 * the timeout, so a lost final segment costs the duration, not the digit.
 */
struct telev_receiver {
	struct telev_reported recent[TELEV_RECENT_COUNT];
	uint64_t active_arrival_ns;
	uint64_t seg_arrival_ns;
	uint32_t clock_rate;
	uint32_t active_timestamp;
	uint32_t seg_total;
	uint32_t seg_end_ts;
	uint16_t active_duration;
	uint8_t active_event;
	uint8_t seg_event;
	uint8_t recent_next;
	uint8_t flushed_next;
	bool active_valid;
	bool seg_valid;
	struct telev_flushed flushed[TELEV_RECENT_COUNT];
};

struct telev_event {
	char digit;
	uint16_t duration_ms;
};

/*
 * The wire event the receive half renders toward the line (core/stream.c
 * stream_rx_run). It tracks what to render and for how long; it decodes
 * nothing the receiver above already decodes and reports nothing.
 *
 * The identity is the event code together with the RTP timestamp, the key the
 * receiver chains its segments on. A same-digit event under a new timestamp is
 * a new event, and the generator restarts on it. A packet that carries the
 * identity already held extends it, an end included, so the three end packets
 * of RFC 4733 2.5.1.4 end one event once.
 *
 * volume is the RFC 4733 volume of the packet that started the event. A later
 * packet of the event does not change it, a segment continuation included,
 * because the caller builds its generator once per event.
 *
 * target and rendered count the stream's clock units. The render runs while
 * the event stands and while what it announced is not rendered yet, so an
 * event whose first accepted packet already carries the end bit still renders
 * the duration that packet announced. That is what a lost start, or a whole
 * event drained inside one tick, leaves behind.
 */
struct telev_play {
	uint64_t last_ns;		/* the newest packet of the identity */
	uint64_t start_ns;		/* the first, for the ceiling */
	uint32_t timeout_ms;
	uint32_t event_ts;
	uint32_t seg_units;		/* the closed segments of the chain */
	uint32_t target;
	uint32_t rendered;
	uint8_t event;
	uint8_t volume;
	char digit;
	bool active;			/* an identity is held */
	bool ended;
	bool done;			/* nothing more renders for it */
};

void rtp_stats_init_cfg(struct rtp_stats *stats, uint32_t clock_rate);
/* The narrowband wrapper: 8000 (RFC 3551). */
void rtp_stats_init(struct rtp_stats *stats);
void rtp_stats_delay_set(struct rtp_stats *stats, uint32_t delay_ms);
void rtp_stats_rtcp_report(struct rtp_stats *stats,
			   const struct rtcp_report *report,
			   uint32_t now_ntp_middle);
/* Snapshot jitter fields use milliseconds. RTCP report jitter uses timestamp units. */
void rtp_stats_snapshot(const struct rtp_stats *stats,
			const struct rtp_receiver *receiver,
			struct udsp_media_stats *out);
uint32_t rtp_receiver_expected_get(const struct rtp_receiver *receiver);
double rtp_stats_rfactor(double loss_percent, uint32_t delay_ms);
void rtp_stats_quality(double loss_percent, uint32_t delay_ms,
		       float *mos_lq, float *mos_cq, uint32_t *rfactor);

/* The payload type an RFC 4733 event travels on, or RTP_PT_DISABLED when the
 * description carries none. The sender reads it, and core/stream.c asks the
 * same question before it removes the tone or relays a digit, so one
 * description cannot remove the tone and refuse the relay. */
uint8_t media_event_pt_get(const struct udsp_media *media);

int rtp_sender_init(struct rtp_sender *sender,
		    const struct udsp_media *media,
		    const struct rtp_entropy *entropy,
		    struct rtp_stats *stats);
int rtp_sender_reconfigure(struct rtp_sender *sender,
			   const struct udsp_media *media);
void rtp_sender_restart(struct rtp_sender *sender);
int rtp_header_build(struct rtp_sender *sender, uint8_t payload_type,
		     bool marker, uint32_t timestamp, uint8_t *packet,
		     size_t capacity);
int rtp_packet_build(struct rtp_sender *sender, const uint8_t *payload,
		     size_t payload_len, uint32_t samples, uint8_t *packet,
		     size_t capacity, size_t *packet_len);

int rtp_receiver_init_cfg(struct rtp_receiver *receiver,
			  const struct udsp_media *media, uint32_t clock_rate,
			  struct rtp_stats *stats);
/* The narrowband wrapper: 8000 (RFC 3551). */
int rtp_receiver_init(struct rtp_receiver *receiver,
		      const struct udsp_media *media, struct rtp_stats *stats);
int rtp_receiver_reconfigure(struct rtp_receiver *receiver,
			     const struct udsp_media *media);
/* Parses one RTP packet, the first of a lower-layer PDU. payload_len covers
 * that packet alone. out->tail carries whatever follows it (RFC 3550 11). */
int rtp_packet_parse(struct rtp_receiver *receiver, const uint8_t *packet,
		     size_t packet_len, uint64_t arrival_ns,
		     struct rtp_packet *out);
/* The first packet of a source, which RFC 3550 A.1 probation held, once the
 * parse of its successor proved the source. The caller takes it after that
 * parse, whatever the parse returned, and delivers it ahead of the successor.
 * out->payload points into the receiver and stays valid until the next parse.
 * Returns false when no packet waits. */
bool rtp_receiver_held_take(struct rtp_receiver *receiver,
			    struct rtp_packet *out);
/* The caller stopped walking a PDU with packets still inside it. Records them
 * as this box's discards, so the loss report does not blame the network for a
 * refusal that happened here. Reads framing only. */
void rtp_receiver_pdu_residual_discard(struct rtp_receiver *receiver,
				       const uint8_t *data, size_t len);
void rtp_receiver_report(struct rtp_receiver *receiver, uint32_t lsr,
			 uint32_t dlsr, struct rtcp_report *report);

/* A NULL report emits an RC=0 report for an unvalidated source. */
int rtcp_sr_build(const struct rtcp_sender_info *sender,
		  const struct rtcp_report *report, uint8_t *packet,
		  size_t capacity, size_t *packet_len);
int rtcp_rr_build(uint32_t sender_ssrc, const struct rtcp_report *report,
		  uint8_t *packet, size_t capacity, size_t *packet_len);
int rtcp_sdes_build(uint32_t ssrc, const char *cname, uint8_t *packet,
		    size_t capacity, size_t *packet_len);
int rtcp_bye_build(uint32_t ssrc, const char *reason, uint8_t *packet,
		   size_t capacity, size_t *packet_len);
void rtcp_sender_info_get(const struct rtp_sender *rtp, uint32_t ntp_msw,
			  uint32_t ntp_lsw,
			  struct rtcp_sender_info *sender);
int rtcp_packet_parse(const uint8_t *packet, size_t packet_len,
		      uint32_t report_ssrc,
		      struct rtcp_parse_result *out);
int rtcp_compound_parse(const uint8_t *packet, size_t packet_len,
			uint32_t report_ssrc,
			struct rtcp_parse_result *out);
void rtcp_state_init(struct rtcp_state *state, uint32_t local_ssrc,
		     struct rtp_stats *stats);
/* SR arrival and DLSR use monotonic 16.16 time. RTT uses wall NTP-middle time. */
void rtcp_state_receive(struct rtcp_state *state,
			const struct rtcp_parse_result *packet,
			uint32_t arrival_mono_16_16,
			uint32_t now_ntp_middle);
void rtcp_state_report_timing(const struct rtcp_state *state,
			      uint32_t now_mono_16_16, uint32_t *lsr,
			      uint32_t *dlsr);

void telev_sender_init(struct telev_sender *sender, struct rtp_sender *rtp);
int telev_sender_start(struct telev_sender *sender, char digit, uint8_t volume);
int telev_sender_update(struct telev_sender *sender, uint16_t duration);
int telev_sender_end(struct telev_sender *sender, uint16_t duration);
/* RFC 4733 2.5.1.3: the event outlived the 16-bit duration field. Call this
 * once the segment's closing packet (the duration field at its maximum, the E
 * bit clear) is on the wire; the next segment starts where that one ended, on
 * the same event and without a new marker. */
int telev_sender_segment(struct telev_sender *sender);
int telev_sender_packet_build(struct telev_sender *sender, uint8_t *packet,
			      size_t capacity, size_t *packet_len);
void telev_receiver_init_cfg(struct telev_receiver *receiver,
			     uint32_t clock_rate);
/* The narrowband wrapper: 8000 (RFC 3551). */
void telev_receiver_init(struct telev_receiver *receiver);
/* Call this for each accepted RTP packet so audio packets advance event timeouts. */
int telev_packet_parse(struct telev_receiver *receiver,
		       const struct rtp_packet *packet, struct telev_event *event);

/* The lost-end bound in milliseconds, UDSP_TEV_PLAY_TIMEOUT_MS unless the
 * environment overrides it. It is read once, on the first call; udsp_open
 * makes that call from the control thread, where a getenv is safe. */
uint32_t telev_play_timeout_ms(void);
void telev_play_init(struct telev_play *play);
/* 1 when this packet started a new event, 0 otherwise. The caller builds its
 * generator on the 1 and reads play->digit and play->volume to build it. */
int telev_play_packet(struct telev_play *play, const struct rtp_packet *packet);
/* Whether the event renders on this tick. left takes the clock units the event
 * has still to render, or UINT32_MAX while the far end has not ended it, so a
 * caller renders a whole frame until the last one. */
bool telev_play_tick(struct telev_play *play, uint64_t now_ns, uint32_t *left);
/* The clock units the caller rendered. */
void telev_play_advance(struct telev_play *play, uint32_t units);

#endif
