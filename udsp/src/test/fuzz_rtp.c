// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../rtp/jb.h"
#include "../rtp/rtp.h"

/* One fuzzed receive stream: the buffer plus the modeled sender's clock, so
 * timestamps, sequence numbers and silences stay coherent across inputs and
 * the SID machinery's armed states survive from one input to the next. */
struct jb_fuzz {
	struct udsp_jb jb;
	uint64_t now_ns;
	uint32_t ts;
	uint16_t seq;
	uint16_t drought;
};

struct fuzz_context {
	struct udsp_media media;
	struct rtp_stats stats;
	struct rtp_receiver receiver;
	struct telev_receiver telev;
	/* Two streams: one configured for a codec-native SID frame so the
	 * slicing, refresh, exemption, park and restamp machinery all see
	 * attacker-controlled input, and one at the 0/0 configuration so the
	 * narrowband path stays fuzzed too. */
	struct jb_fuzz jb_sid;
	struct jb_fuzz jb_plain;
	uint64_t rtp_accepted;
	uint64_t telev_called;
	uint64_t rtcp_ok;
	uint64_t compound_ok;
	uint64_t jb_out;
	/* Reach probes: heuristics over the buffer's public state, printed at
	 * the end so a shaping change that stops reaching a mechanism is
	 * visible. */
	uint64_t jb_refresh;
	uint64_t jb_exempt;
	uint64_t jb_park;
	uint64_t jb_restamp;
	uint32_t iteration;
	uint16_t sequence;
	bool initialized;
};

/*
 * Feed one fuzz input through a jitter-buffer stream. The input's first bytes
 * select the packet kind, the marker, timestamp jumps, sequence replays,
 * droughts (a modeled sender silence: gets run, no put) and a bias toward
 * SID-shaped lengths, so armed SID comfort noise survives across puts and the
 * refresh, jump-exemption, clock-park and first-learn-restamp mechanisms are
 * all reachable, not only the slicing arithmetic. Every emitted slice is read
 * end to end so a bad slice pointer or length is caught by the sanitizers.
 */
static void jb_fuzz_feed(struct fuzz_context *c, struct jb_fuzz *fz,
			 const uint8_t *data, size_t size, bool sid_cfg)
{
	struct udsp_jb_packet in;
	struct udsp_jb_out out;
	uint8_t pay[UDSP_JB_PAYLOAD_MAX];
	uint8_t b[8] = { 0 };
	bool learned = fz->jb.span_learned;
	bool armed = fz->jb.cn_armed && fz->jb.cn_sid;
	bool put_done = false;
	uint64_t resyncs_before = fz->jb.total.resyncs;
	uint32_t jump;
	size_t len = size;
	unsigned gets;
	unsigned i;
	int rc = -1;

	for (i = 0; i < 8u && i < size; i++)
		b[i] = data[i];
	/* A fresh configuration now and then, half from the input: the store,
	 * restamp and slicing arithmetic see every frame_bytes/sid_len shape,
	 * with the G.729 Annex B shape kept dominant. The plain stream never
	 * re-inits, so 0/0 stays covered for the whole run. */
	if (sid_cfg && c->iteration && !(c->iteration % 1999u)) {
		uint16_t fb = (b[6] & 1u) ? 10u : (uint16_t)(b[6] >> 1);
		uint16_t sl = (b[6] & 1u) ? 2u : (uint16_t)(b[7] & 0x3fu);

		udsp_jb_init_cfg(&fz->jb, UDSP_JB_ADAPTIVE, 40, 200,
				 UDSP_JB_TS_PER_MS, UDSP_JB_FRAME_TS, fb, sl,
				 false);
	}
	if (len > UDSP_JB_PAYLOAD_MAX)
		len = UDSP_JB_PAYLOAD_MAX;
	if (fz->jb.sid_len && fz->jb.frame_bytes && !(b[0] & 3u))
		len = (size_t)(b[4] % 4u) * fz->jb.frame_bytes +
		      fz->jb.sid_len;
	if (len > UDSP_JB_PAYLOAD_MAX)
		len = UDSP_JB_PAYLOAD_MAX;
	memset(pay, b[3], len ? len : 1u);
	for (i = 0; i < len && i < size; i++)
		pay[i] = data[i];

	/* A mis-stamp belongs to a packet, so no injection falls inside a
	 * drought: the silence keeps its clean DTX timeline and the resume
	 * exercises the jump exemption instead of a guaranteed resync. */
	if (!fz->drought) {
		if (!(b[1] & 0x1fu))
			fz->ts += (uint32_t)b[2] << 10;
		else if ((b[1] & 0x1fu) == 1u)
			fz->ts -= (uint32_t)b[2] << 8;
	}
	jump = fz->ts - fz->jb.last_rx_ts;
	if (jump > 0x80000000u)
		jump = 0u - jump;

	memset(&in, 0, sizeof(in));
	in.payload = pay;
	in.len = len;
	in.arrival_ns = fz->now_ns;
	in.ssrc = (b[1] & 0xfcu) == 0x40u ? 0x5150ffffu : 0x51501234u;
	in.kind = (b[0] & 0xe0u) == 0xe0u ? RTP_PAYLOAD_COMFORT_NOISE :
		  RTP_PAYLOAD_AUDIO;
	in.marker = (b[0] & 4u) != 0;
	in.seq = fz->seq;
	in.rtp_timestamp = fz->ts;
	if ((b[1] & 0xc0u) == 0xc0u) {
		in.seq = (uint16_t)(fz->seq - 1u);
		in.rtp_timestamp = fz->ts - 160u;
	}
	if (fz->drought) {
		/* A DTX silence: no packet, no sequence number, and the
		 * sender's clock runs at the drought's own get rate (8 ticks
		 * of 10 ms below), so a long silence opens the >10 s window
		 * the jump exemption and the clock park live in. */
		fz->drought--;
		fz->ts += 8u * 80u;
	} else {
		if (b[5] == 0xffu)
			fz->drought = b[2];
		put_done = true;
		rc = udsp_jb_put(&fz->jb, &in);
		if ((b[1] & 0xc0u) != 0xc0u)
			fz->seq++;
		fz->ts += 160u;
	}

	if (sid_cfg && put_done) {
		if (rc == 0 && armed && in.kind == RTP_PAYLOAD_AUDIO &&
		    fz->jb.sid_len && in.len == fz->jb.sid_len)
			c->jb_refresh++;
		/* Surviving a >10 s jump without a resync is the exemption
		 * (or the refresh classification); the packet itself may
		 * still be held by the parked-clock machinery. */
		if (armed && in.kind == RTP_PAYLOAD_AUDIO && jump > 80000u &&
		    fz->jb.total.resyncs == resyncs_before)
			c->jb_exempt++;
	}
	if (sid_cfg && !learned && fz->jb.span_learned && fz->jb.count > 1u)
		c->jb_restamp++;

	gets = 1u + (b[4] & 3u);
	if ((b[4] & 0xc0u) == 0xc0u)
		gets = 16u + (b[4] & 31u);
	if (fz->drought)
		gets = 8u;
	for (i = 0; i < gets; i++) {
		fz->now_ns += 10000000ULL;
		if (udsp_jb_get(&fz->jb, fz->now_ns, &out))
			continue;
		if (out.payload && out.len) {
			volatile uint8_t sink = out.payload[0] ^
						out.payload[out.len - 1u];

			(void)sink;
		}
		if (out.decision == UDSP_JB_FRAME)
			c->jb_out++;
	}
	if (sid_cfg && fz->jb.cn_sid && fz->jb.cn_run_ms >= 10000u)
		c->jb_park++;
}

static uint64_t stamp_read(const uint8_t *data, size_t size)
{
	uint64_t value = 0;
	size_t i;

	for (i = 0; i < size && i < 8U; i++)
		value = (value << 8) | data[i];
	return value;
}

static void context_init(struct fuzz_context *context)
{
	memset(context, 0, sizeof(*context));
	context->media.codec_pt = 0;
	context->media.dtmf_pt = 101;
	context->media.dtmf_mode = UDSP_DTMF_RFC2833;
	context->media.cng = true;
	rtp_stats_init(&context->stats);
	(void)rtp_receiver_init(&context->receiver, &context->media,
				&context->stats);
	telev_receiver_init(&context->telev);
	/* The G.729 Annex B shape: a 10-byte speech frame and a 2-byte SID, so
	 * a SID-tailed or SID-only length drives the derivation and slicing. */
	udsp_jb_init_cfg(&context->jb_sid.jb, UDSP_JB_ADAPTIVE, 40, 200,
			 UDSP_JB_TS_PER_MS, UDSP_JB_FRAME_TS, 10, 2, false);
	udsp_jb_init(&context->jb_plain.jb, UDSP_JB_ADAPTIVE, 40, 200);
	context->jb_sid.ts = 160000u;
	context->jb_plain.ts = 160000u;
	context->jb_sid.seq = 100u;
	context->jb_plain.seq = 100u;
	context->sequence = 1;
	context->initialized = true;
}

static void rtp_result_count(struct fuzz_context *context, int result,
			     const struct rtp_packet *packet)
{
	struct telev_event event;

	if (result < 0)
		return;
	context->rtp_accepted++;
	context->telev_called++;
	(void)telev_packet_parse(&context->telev, packet, &event);
}

static void structured_rtp_test(struct fuzz_context *context,
				const uint8_t *data, size_t size)
{
	struct rtp_packet parsed;
	uint8_t packet[512];
	uint8_t payload_type;
	size_t payload_len = size;
	size_t packet_len;
	uint32_t timestamp = context->iteration * 160U;
	uint64_t arrival_ns = (uint64_t)context->iteration * 20000000ULL;
	unsigned pass;

	if (payload_len > sizeof(packet) - RTP_HEADER_SIZE)
		payload_len = sizeof(packet) - RTP_HEADER_SIZE;
	payload_type = context->iteration & 1U ? context->media.dtmf_pt :
		       context->media.codec_pt;
	if (payload_type == context->media.dtmf_pt && payload_len < 4U)
		payload_len = 4U;
	packet_len = RTP_HEADER_SIZE + payload_len;
	memset(packet, 0, packet_len);
	if (size) {
		size_t copy_len = size < payload_len ? size : payload_len;

		memcpy(packet + RTP_HEADER_SIZE, data, copy_len);
	}
	packet[0] = 0x80U;
	packet[1] = payload_type;
	packet[4] = (uint8_t)(timestamp >> 24);
	packet[5] = (uint8_t)(timestamp >> 16);
	packet[6] = (uint8_t)(timestamp >> 8);
	packet[7] = (uint8_t)timestamp;
	packet[8] = 0x5a;
	packet[9] = 0xa5;
	packet[10] = 0x12;
	packet[11] = 0x34;
	if (payload_type == context->media.dtmf_pt) {
		packet[12] = (uint8_t)(context->iteration % 16U);
		packet[13] &= 0x3fU;
		packet[14] = 0;
		packet[15] = 160;
	}
	for (pass = 0; pass < 2U; pass++) {
		uint16_t sequence = context->sequence++;

		packet[2] = (uint8_t)(sequence >> 8);
		packet[3] = (uint8_t)sequence;
		rtp_result_count(context,
			rtp_packet_parse(&context->receiver, packet, packet_len,
					 arrival_ns + pass * 1000000ULL, &parsed),
			&parsed);
	}
}

static size_t rtcp_part_write(uint8_t *packet, uint8_t type, size_t length)
{
	memset(packet, 0, length);
	packet[0] = type == 203U ? 0x81U : 0x80U;
	packet[1] = type;
	packet[2] = (uint8_t)((length / 4U - 1U) >> 8);
	packet[3] = (uint8_t)(length / 4U - 1U);
	if (length >= 8U) {
		packet[4] = 0x5a;
		packet[5] = 0xa5;
		packet[6] = 0x12;
		packet[7] = 0x34;
	}
	return length;
}

static void structured_rtcp_test(struct fuzz_context *context,
				 const uint8_t *data, size_t size)
{
	struct rtcp_parse_result parsed;
	uint8_t packet[32];
	size_t offsets[3];
	size_t lengths[3];
	size_t packet_len = 0;
	unsigned part_count = 2U + (context->iteration & 1U);
	unsigned i;

	offsets[0] = packet_len;
	lengths[0] = rtcp_part_write(packet + packet_len, 201, 8);
	packet_len += lengths[0];
	offsets[1] = packet_len;
	lengths[1] = rtcp_part_write(packet + packet_len, 203, 8);
	packet_len += lengths[1];
	if (part_count == 3U) {
		offsets[2] = packet_len;
		lengths[2] = rtcp_part_write(packet + packet_len, 204, 4);
		packet_len += lengths[2];
	}
	if ((context->iteration & 3U) && size) {
		unsigned selected = 1U + data[0] % (part_count - 1U);

		packet[offsets[selected] + 2U] = size > 1U ? data[1] : data[0];
		packet[offsets[selected] + 3U] = size > 2U ? data[2] : data[0];
	}
	for (i = 0; i < part_count; i++) {
		if (rtcp_packet_parse(packet + offsets[i], lengths[i], 0,
				      &parsed) == 0)
			context->rtcp_ok++;
	}
	if (rtcp_compound_parse(packet, packet_len, 0, &parsed) == 0)
		context->compound_ok++;
}

static void input_test(struct fuzz_context *context,
		       const uint8_t *data, size_t size)
{
	struct rtp_packet rtp;
	struct rtcp_parse_result rtcp;
	uint64_t stamp;

	if (!context->initialized)
		context_init(context);
	stamp = stamp_read(data, size);
	rtp_result_count(context,
		rtp_packet_parse(&context->receiver, data, size, stamp, &rtp),
		&rtp);
	if (rtcp_packet_parse(data, size, 0, &rtcp) == 0)
		context->rtcp_ok++;
	if (rtcp_compound_parse(data, size, 0, &rtcp) == 0)
		context->compound_ok++;
	structured_rtp_test(context, data, size);
	structured_rtcp_test(context, data, size);
	jb_fuzz_feed(context, &context->jb_sid, data, size, true);
	jb_fuzz_feed(context, &context->jb_plain, data, size, false);
	context->iteration++;
}

#ifdef UDSP_LIBFUZZER
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static struct fuzz_context context;

	input_test(&context, data, size);
	return 0;
}
#else
static uint32_t random_next(uint32_t *state)
{
	uint32_t value = *state;

	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	*state = value;
	return value;
}

int main(void)
{
	struct fuzz_context context = { 0 };
	uint8_t data[512];
	uint32_t state = 0x5d5f4733U;
	unsigned iteration;

	for (iteration = 0; iteration < 10000U; iteration++) {
		size_t size = random_next(&state) % (sizeof(data) + 1U);
		size_t i;

		for (i = 0; i < size; i++)
			data[i] = (uint8_t)random_next(&state);
		input_test(&context, data, size);
	}
	printf("# fuzz_rtp: 10000 cases\n");
	printf("# rtp_accepted=%llu telev_called=%llu rtcp_ok=%llu "
	       "compound_ok=%llu jb_out=%llu\n",
	       (unsigned long long)context.rtp_accepted,
	       (unsigned long long)context.telev_called,
	       (unsigned long long)context.rtcp_ok,
	       (unsigned long long)context.compound_ok,
	       (unsigned long long)context.jb_out);
	printf("# jb_refresh=%llu jb_exempt=%llu jb_park=%llu jb_restamp=%llu\n",
	       (unsigned long long)context.jb_refresh,
	       (unsigned long long)context.jb_exempt,
	       (unsigned long long)context.jb_park,
	       (unsigned long long)context.jb_restamp);
	assert(context.rtp_accepted > 0);
	return 0;
}
#endif
