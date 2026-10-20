// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../rtp/rtp.h"
#include "util.h"

static struct udsp_media media_make(void)
{
	struct udsp_media media;

	memset(&media, 0, sizeof(media));
	media.codec = UDSP_SEL_PCMU;
	media.codec_pt = 0;
	media.dtmf_pt = 101;
	media.dtmf_mode = UDSP_DTMF_RFC2833;
	media.ptime_ms = 20;
	media.direction = UDSP_MEDIA_SENDRECV;
	return media;
}

static struct rtp_entropy entropy_make(uint32_t value)
{
	struct rtp_entropy entropy = {
		.ssrc = value ^ 0x10203040U,
		.sequence = value ^ 0x50607080U,
		.timestamp = value ^ 0x90a0b0c0U,
	};

	return entropy;
}

static size_t packet_make(uint8_t *packet, uint16_t sequence,
			  uint32_t timestamp, uint32_t ssrc, uint8_t pt,
			  uint8_t flags, uint8_t csrc_count,
			  const uint8_t *payload, size_t payload_len)
{
	size_t header_len = RTP_HEADER_SIZE + (size_t)csrc_count * 4U;
	size_t i;

	memset(packet, 0, header_len + payload_len);
	packet[0] = 0x80U | flags | csrc_count;
	packet[1] = pt;
	packet[2] = (uint8_t)(sequence >> 8);
	packet[3] = (uint8_t)sequence;
	packet[4] = (uint8_t)(timestamp >> 24);
	packet[5] = (uint8_t)(timestamp >> 16);
	packet[6] = (uint8_t)(timestamp >> 8);
	packet[7] = (uint8_t)timestamp;
	packet[8] = (uint8_t)(ssrc >> 24);
	packet[9] = (uint8_t)(ssrc >> 16);
	packet[10] = (uint8_t)(ssrc >> 8);
	packet[11] = (uint8_t)ssrc;
	for (i = RTP_HEADER_SIZE; i < header_len; i++)
		packet[i] = (uint8_t)i;
	/* A header-only packet is legal and passes NULL here; memcpy forbids a
	 * null source even for a zero length (C11 7.24.1p2). */
	if (payload_len)
		memcpy(packet + header_len, payload, payload_len);
	return header_len + payload_len;
}

static uint32_t uint32_test_read(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static void test_header_round_trip(void)
{
	struct udsp_media media = media_make();
	struct rtp_sender sender;
	struct rtp_entropy entropy = entropy_make(1);
	struct rtp_receiver receiver;
	struct rtp_packet parsed;
	uint8_t payload[] = { 1, 2, 3, 4 };
	uint8_t packet[64];
	size_t length;

	CASE("rtp_header_round_trip");
	CHECK(rtp_sender_init(&sender, &media, &entropy, NULL) == 0);
	sender.ssrc = 0x12345678U;
	sender.sequence = 40;
	sender.timestamp = 9000;
	CHECK(rtp_packet_build(&sender, payload, sizeof(payload), 160, packet,
			       sizeof(packet), &length) == 0);
	CHECK_UINT(length, 16);
	CHECK_UINT(packet[0], 0x80);
	CHECK_UINT(packet[1], 0x80);
	CHECK_UINT(sender.timestamp, 9160);
	rtp_receiver_init(&receiver, &media, NULL);
	CHECK(rtp_packet_parse(&receiver, packet, length, 0, &parsed) < 0);
	CHECK(rtp_packet_build(&sender, payload, sizeof(payload), 160, packet,
			       sizeof(packet), &length) == 0);
	CHECK(!(packet[1] & 0x80U));
	CHECK(rtp_packet_parse(&receiver, packet, length, 20000000, &parsed) == 0);
	CHECK_UINT(parsed.sequence, 41);
	CHECK_UINT(parsed.timestamp, 9160);
	CHECK_UINT(parsed.ssrc, 0x12345678U);
	CHECK_UINT(parsed.payload_len, sizeof(payload));
	CHECK(memcmp(parsed.payload, payload, sizeof(payload)) == 0);
	rtp_sender_restart(&sender);
	CHECK(rtp_packet_build(&sender, payload, sizeof(payload), 160, packet,
			       sizeof(packet), &length) == 0);
	CHECK(packet[1] & 0x80U);
	CHECK(rtp_packet_build(&sender, payload, sizeof(payload), 160, packet,
			       sizeof(packet), &length) == 0);
	CHECK(!(packet[1] & 0x80U));
}

static void test_header_options(void)
{
	struct udsp_media media = media_make();
	struct rtp_receiver receiver;
	struct rtp_packet parsed;
	uint8_t payload[] = { 9, 8 };
	uint8_t padded[] = { 9, 8, 0, 2 };
	uint8_t extended[] = { 0xbe, 0xde, 0, 1, 1, 2, 3, 4, 9, 8 };
	uint8_t both[] = { 0x10, 0, 0, 0, 9, 8, 0, 2 };
	uint8_t packet[64];
	size_t length;

	CASE("rtp_padding_extension_and_csrc");
	rtp_receiver_init(&receiver, &media, NULL);
	length = packet_make(packet, 10, 100, 7, 0, 0, 0, payload,
			     sizeof(payload));
	CHECK(rtp_packet_parse(&receiver, packet, length, 0, &parsed) < 0);
	length = packet_make(packet, 11, 200, 7, 0, 0x20, 0, padded,
			     sizeof(padded));
	CHECK(rtp_packet_parse(&receiver, packet, length, 20000000, &parsed) == 0);
	CHECK_UINT(parsed.payload_len, 2);
	CHECK(memcmp(parsed.payload, payload, sizeof(payload)) == 0);
	length = packet_make(packet, 12, 300, 7, 0, 0x10, 0, extended,
			     sizeof(extended));
	CHECK(rtp_packet_parse(&receiver, packet, length, 40000000, &parsed) == 0);
	CHECK_UINT(parsed.payload_len, 2);
	CHECK(memcmp(parsed.payload, payload, sizeof(payload)) == 0);
	length = packet_make(packet, 13, 400, 7, 0, 0x30, 0, both,
			     sizeof(both));
	CHECK(rtp_packet_parse(&receiver, packet, length, 60000000, &parsed) == 0);
	CHECK_UINT(parsed.payload_len, 2);
	CHECK(memcmp(parsed.payload, payload, sizeof(payload)) == 0);
	length = packet_make(packet, 14, 500, 7, 0, 0, 1, payload,
			     sizeof(payload));
	CHECK(rtp_packet_parse(&receiver, packet, length, 80000000, &parsed) == 0);
	CHECK_UINT(parsed.csrc_count, 1);
	CHECK_UINT(parsed.payload_len, 2);
	CHECK(memcmp(parsed.payload, payload, sizeof(payload)) == 0);

	CASE("rtp_padding_extension_bounds_rejected");
	padded[3] = 0;
	length = packet_make(packet, 15, 600, 7, 0, 0x20, 0, padded,
			     sizeof(padded));
	CHECK(rtp_packet_parse(&receiver, packet, length, 100000000, &parsed) < 0);
	extended[3] = 2;
	length = packet_make(packet, 16, 700, 7, 0, 0x10, 0, extended,
			     sizeof(extended));
	CHECK(rtp_packet_parse(&receiver, packet, length, 120000000, &parsed) < 0);
	both[7] = 9;
	length = packet_make(packet, 17, 800, 7, 0, 0x30, 0, both,
			     sizeof(both));
	CHECK(rtp_packet_parse(&receiver, packet, length, 140000000, &parsed) < 0);
}

static void test_sequence_order(void)
{
	struct udsp_media media = media_make();
	struct rtp_receiver receiver;
	struct rtp_packet parsed;
	uint8_t packet[32];
	uint8_t payload = 0;
	size_t length;

	CASE("rtp_sequence_in_order_reorder");
	rtp_receiver_init(&receiver, &media, NULL);
	length = packet_make(packet, 10, 0, 1, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 0, &parsed) < 0);
	length = packet_make(packet, 11, 160, 1, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 20000000, &parsed) == 0);
	length = packet_make(packet, 12, 320, 1, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 40000000, &parsed) == 0);
	length = packet_make(packet, 11, 160, 1, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 41000000, &parsed) == 0);
	CHECK_UINT(receiver.max_seq, 12);
	CHECK_UINT(receiver.received, 3);
}

static void test_sequence_wrap_loss(void)
{
	struct udsp_media media = media_make();
	struct rtp_stats stats;
	struct rtp_receiver receiver;
	struct rtp_packet parsed;
	struct rtcp_report report;
	uint8_t packet[32];
	uint8_t payload = 0;
	size_t length;
	uint16_t sequences[] = { 65534U, 65535U, 0U, 2U };
	unsigned i;

	CASE("rtp_sequence_wrap_a3_loss");
	rtp_stats_init(&stats);
	rtp_receiver_init(&receiver, &media, &stats);
	for (i = 0; i < sizeof(sequences) / sizeof(sequences[0]); i++) {
		length = packet_make(packet, sequences[i], i * 160U, 2, 0, 0, 0,
				     &payload, 1);
		if (!i)
			CHECK(rtp_packet_parse(&receiver, packet, length,
					   i * 20000000ULL, &parsed) < 0);
		else
			CHECK(rtp_packet_parse(&receiver, packet, length,
					   i * 20000000ULL, &parsed) == 0);
	}
	rtp_receiver_report(&receiver, 3, 4, &report);
	CHECK_UINT(receiver.cycles, RTP_SEQ_MOD);
	CHECK_UINT(report.highest_seq, 65538U);
	CHECK_UINT(report.cumulative_lost, 1);
	CHECK_UINT(report.fraction_lost, 64);
	CHECK_UINT(report.lsr, 3);
	CHECK_UINT(report.dlsr, 4);
}

static void test_new_ssrc(void)
{
	struct udsp_media media = media_make();
	struct rtp_receiver receiver;
	struct rtp_packet parsed;
	uint8_t packet[32];
	uint8_t payload = 0;
	size_t length;

	CASE("rtp_new_ssrc_probation");
	rtp_receiver_init(&receiver, &media, NULL);
	length = packet_make(packet, 1, 0, 10, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 0, &parsed) < 0);
	length = packet_make(packet, 2, 160, 10, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 20000000, &parsed) == 0);
	length = packet_make(packet, 100, 0, 11, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 40000000, &parsed) < 0);
	length = packet_make(packet, 101, 160, 11, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 60000000, &parsed) == 0);
	CHECK_UINT(receiver.ssrc, 11);
	CHECK_UINT(receiver.base_seq, 101);
}

static void test_jitter(void)
{
	struct udsp_media media = media_make();
	struct rtp_receiver receiver;
	struct rtp_packet parsed;
	struct rtcp_report report;
	uint8_t packet[32];
	uint8_t payload = 0;
	uint32_t transits[] = { 3, 10, 14, 8, 20 };
	uint32_t expected_q4 = 0;
	uint32_t prior = 0;
	unsigned i;

	CASE("rtp_jitter_rfc3550_a8");
	rtp_receiver_init(&receiver, &media, NULL);
	for (i = 0; i < sizeof(transits) / sizeof(transits[0]); i++) {
		uint32_t timestamp = i * 160U;
		uint64_t arrival_ns =
			(uint64_t)(timestamp + transits[i]) * 125000ULL;
		size_t length = packet_make(packet, (uint16_t)(100U + i),
					    timestamp, 22, 0, 0, 0,
					    &payload, 1);

		if (!i) {
			CHECK(rtp_packet_parse(&receiver, packet, length, arrival_ns,
					   &parsed) < 0);
			continue;
		}
		CHECK(rtp_packet_parse(&receiver, packet, length, arrival_ns,
				       &parsed) == 0);
		if (i > 1U) {
			uint32_t delta = transits[i] > prior ?
				transits[i] - prior : prior - transits[i];

			expected_q4 += delta - ((expected_q4 + 8U) >> 4);
		}
		prior = transits[i];
	}
	rtp_receiver_report(&receiver, 0, 0, &report);
	CHECK_UINT(receiver.jitter_q4, expected_q4);
	CHECK_UINT(report.jitter, expected_q4 >> 4);
}

static void test_reconfiguration(void)
{
	struct udsp_media media = media_make();
	struct rtp_sender sender;
	struct rtp_entropy entropy = entropy_make(9);
	struct rtp_receiver receiver;
	struct rtp_packet parsed;
	uint8_t packet[64];
	uint8_t payload[4] = { 0 };
	size_t length;
	uint32_t start;
	uint32_t ssrc;

	CASE("rtp_marker_timestamp_reconfiguration");
	CHECK(rtp_sender_init(&sender, &media, &entropy, NULL) == 0);
	sender.sequence = 200;
	start = sender.timestamp;
	ssrc = sender.ssrc;
	CHECK(rtp_packet_build(&sender, payload, sizeof(payload), 160, packet,
			       sizeof(packet), &length) == 0);
	CHECK(packet[1] & 0x80U);
	media.ptime_ms = 30;
	CHECK(rtp_sender_reconfigure(&sender, &media) == 0);
	CHECK(rtp_packet_build(&sender, payload, sizeof(payload), 240, packet,
			       sizeof(packet), &length) == 0);
	CHECK(packet[1] & 0x80U);
	CHECK(rtp_packet_build(&sender, payload, sizeof(payload), 240, packet,
			       sizeof(packet), &length) == 0);
	CHECK(!(packet[1] & 0x80U));
	CHECK_UINT(((uint32_t)packet[4] << 24) | ((uint32_t)packet[5] << 16) |
		   ((uint32_t)packet[6] << 8) | packet[7], start + 400U);
	media.codec = UDSP_SEL_PCMA;
	media.codec_pt = 8;
	CHECK(rtp_sender_reconfigure(&sender, &media) == 0);
	CHECK(rtp_packet_build(&sender, payload, sizeof(payload), 240, packet,
			       sizeof(packet), &length) == 0);
	CHECK_UINT(packet[1], 0x88);
	CHECK_UINT(sender.ssrc, ssrc);
	CHECK_UINT(sender.sequence, 204);
	CHECK_UINT(sender.timestamp, start + 880U);
	rtp_receiver_init(&receiver, &media, NULL);
	packet_make(packet, 10, 0, 77, 8, 0, 0, payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, 13, 0, &parsed) < 0);
	packet_make(packet, 11, 160, 77, 8, 0, 0, payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, 13, 20000000, &parsed) == 0);
	media.codec_pt = 0;
	CHECK(rtp_receiver_reconfigure(&receiver, &media) == 0);
	packet_make(packet, 12, 320, 77, 0, 0, 0, payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, 13, 40000000, &parsed) == 0);
	CHECK_UINT(receiver.base_seq, 11);
}

static void test_init_validation(void)
{
	struct udsp_media media = media_make();
	struct rtp_entropy entropy = {
		.ssrc = 0x01020304U,
		.sequence = 0x55667788U,
		.timestamp = 0x90abcdefU,
	};
	struct rtp_sender sender;
	struct rtp_receiver receiver;
	struct telev_sender telev;

	CASE("rtp_init_entropy_codec_and_event_validation");
	CHECK(rtp_sender_init(&sender, &media, &entropy, NULL) == 0);
	CHECK_UINT(sender.ssrc, entropy.ssrc);
	CHECK_UINT(sender.sequence, (uint16_t)entropy.sequence);
	CHECK_UINT(sender.timestamp, entropy.timestamp);
	CHECK_UINT(sender.event_pt, 101);
	media.codec_pt = 128;
	CHECK(rtp_sender_init(&sender, &media, &entropy, NULL) == -EINVAL);
	CHECK(rtp_receiver_init(&receiver, &media, NULL) == -EINVAL);
	media = media_make();
	media.codec_pt = 8;
	media.dtmf_pt = 0;
	CHECK(rtp_sender_init(&sender, &media, &entropy, NULL) == 0);
	CHECK_UINT(sender.event_pt, RTP_PT_DISABLED);
	telev_sender_init(&telev, &sender);
	CHECK(telev_sender_start(&telev, '1', 10) == -EINVAL);
	media.dtmf_pt = media.codec_pt;
	CHECK(rtp_receiver_init(&receiver, &media, NULL) == 0);
	CHECK_UINT(receiver.event_pt, RTP_PT_DISABLED);
	media.dtmf_pt = 128;
	CHECK(rtp_receiver_reconfigure(&receiver, &media) == 0);
	CHECK_UINT(receiver.event_pt, RTP_PT_DISABLED);
	media.dtmf_pt = 101;
	media.dtmf_mode = UDSP_DTMF_INBAND;
	CHECK(rtp_receiver_reconfigure(&receiver, &media) == 0);
	CHECK_UINT(receiver.event_pt, RTP_PT_DISABLED);
}

static void test_direction_marker(void)
{
	struct udsp_media media = media_make();
	struct rtp_entropy entropy = entropy_make(31);
	struct rtp_sender sender;
	uint8_t packet[32];
	uint8_t payload = 0;
	size_t length;

	CASE("rtp_direction_restart_marker_clears");
	media.direction = UDSP_MEDIA_INACTIVE;
	CHECK(rtp_sender_init(&sender, &media, &entropy, NULL) == 0);
	CHECK(rtp_packet_build(&sender, &payload, 1, 160, packet,
			       sizeof(packet), &length) == 0);
	CHECK(rtp_sender_reconfigure(&sender, &media) == 0);
	CHECK(!sender.marker_pending);
	media.direction = UDSP_MEDIA_SENDONLY;
	CHECK(rtp_sender_reconfigure(&sender, &media) == 0);
	CHECK(rtp_packet_build(&sender, &payload, 1, 160, packet,
			       sizeof(packet), &length) == 0);
	CHECK(packet[1] & 0x80U);
	CHECK(rtp_packet_build(&sender, &payload, 1, 160, packet,
			       sizeof(packet), &length) == 0);
	CHECK(!(packet[1] & 0x80U));
}

static void test_cn_sequence_accounting(void)
{
	struct udsp_media media = media_make();
	struct rtp_stats stats;
	struct rtp_receiver receiver;
	struct rtp_packet parsed;
	struct rtcp_report report;
	struct udsp_media_stats snapshot;
	uint8_t packet[32];
	uint8_t payload = 0;
	size_t length;
	uint16_t sequence;

	CASE("rtp_cn_and_unknown_pt_count_as_received");
	media.cng = true;
	rtp_stats_init(&stats);
	CHECK(rtp_receiver_init(&receiver, &media, &stats) == 0);
	for (sequence = 100; sequence <= 108; sequence++) {
		length = packet_make(packet, sequence, (sequence - 100U) * 160U,
				     44, 13, 0, 0, &payload, 1);
		if (sequence == 100U) {
			CHECK(rtp_packet_parse(&receiver, packet, length, 0,
					       &parsed) < 0);
			continue;
		}
		CHECK(rtp_packet_parse(&receiver, packet, length,
				       (sequence - 100U) * 20000000ULL,
				       &parsed) == 0);
		CHECK_UINT(parsed.kind, RTP_PAYLOAD_COMFORT_NOISE);
	}
	length = packet_make(packet, 109, 1440, 44, 77, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 180000000, &parsed) < 0);
	length = packet_make(packet, 110, 1600, 44, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 200000000, &parsed) == 0);
	rtp_receiver_report(&receiver, 0, 0, &report);
	CHECK_UINT(receiver.received, 10);
	CHECK_UINT(report.cumulative_lost, 0);
	CHECK_UINT(report.fraction_lost, 0);
	CHECK_UINT(stats.packets_recv, 9);
	rtp_stats_snapshot(&stats, &receiver, &snapshot);
	CHECK_UINT(snapshot.packets_lost, 0);
	CHECK_UINT(snapshot.rfactor, 93);
	CHECK(snapshot.mos_cq > 4.4f);
}

static void test_jitter_wrap_and_reset(void)
{
	struct udsp_media media = media_make();
	struct rtp_stats stats;
	struct rtp_receiver receiver;
	struct rtp_packet parsed;
	uint8_t packet[32];
	uint8_t payload = 0;
	size_t length;
	uint64_t tick_ns = 125000ULL;

	CASE("rtp_jitter_wraps_in_32_bits");
	rtp_receiver_init(&receiver, &media, NULL);
	length = packet_make(packet, 1, 0, 60, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length,
			       0x7ffffffeULL * tick_ns, &parsed) < 0);
	length = packet_make(packet, 2, 0, 60, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length,
			       0x7fffffffULL * tick_ns, &parsed) == 0);
	length = packet_make(packet, 3, 0, 60, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length,
			       0x80000000ULL * tick_ns, &parsed) == 0);
	CHECK_UINT(receiver.jitter_q4, 1);

	CASE("rtp_jitter_resets_on_ssrc_change");
	rtp_stats_init(&stats);
	rtp_receiver_init(&receiver, &media, &stats);
	length = packet_make(packet, 1, 0, 61, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 0, &parsed) < 0);
	length = packet_make(packet, 2, 160, 61, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 160 * tick_ns,
			       &parsed) == 0);
	length = packet_make(packet, 3, 320, 61, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 360 * tick_ns,
			       &parsed) == 0);
	CHECK(receiver.jitter_q4 > 0);
	length = packet_make(packet, 10, 0, 62, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 500 * tick_ns,
			       &parsed) < 0);
	CHECK_UINT(receiver.jitter_q4, 0);
	CHECK_UINT(stats.ts_steps, 0);
}

static void test_jitter_timestamp_step(void)
{
	struct udsp_media media = media_make();
	struct rtp_stats stats;
	struct rtp_receiver receiver;
	struct rtp_packet parsed;
	struct rtcp_report report;
	uint8_t packet[32];
	uint8_t payload = 0;
	uint32_t expected_q4 = 0;
	uint32_t jitter_before;
	uint32_t max_before;
	uint64_t sum_before;
	uint64_t samples_before;
	uint64_t tick_ns = 125000ULL;
	uint32_t timestamp;
	size_t length;
	unsigned i;

	CASE("rtp_jitter_ignores_timestamp_step");
	rtp_stats_init(&stats);
	rtp_receiver_init(&receiver, &media, &stats);
	length = packet_make(packet, 100, 0, 63, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 0, &parsed) < 0);
	length = packet_make(packet, 101, 160, 63, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 160 * tick_ns,
			       &parsed) == 0);
	length = packet_make(packet, 102, 320, 63, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 320 * tick_ns,
			       &parsed) == 0);
	length = packet_make(packet, 103, 480, 63, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 520 * tick_ns,
			       &parsed) == 0);
	expected_q4 += 40U - ((expected_q4 + 8U) >> 4);
	CHECK_UINT(receiver.jitter_q4, expected_q4);
	jitter_before = receiver.jitter_q4;
	max_before = stats.jitter_max;
	sum_before = stats.jitter_sum;
	samples_before = stats.jitter_samples;
	timestamp = 640U + 0x08000000U;
	length = packet_make(packet, 104, timestamp, 63, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 640 * tick_ns,
			       &parsed) == 0);
	CHECK_UINT(receiver.jitter_q4, jitter_before);
	CHECK_UINT(stats.ts_steps, 1);
	CHECK_UINT(stats.jitter_max, max_before);
	CHECK_UINT(stats.jitter_sum, sum_before);
	CHECK_UINT(stats.jitter_samples, samples_before);
	rtp_receiver_report(&receiver, 0, 0, &report);
	CHECK_UINT(report.jitter, jitter_before >> 4);
	for (i = 0; i < 2U; i++) {
		timestamp += 160U;
		length = packet_make(packet, (uint16_t)(105U + i), timestamp,
				     63, 0, 0, 0, &payload, 1);
		CHECK(rtp_packet_parse(&receiver, packet, length,
				       (800U + i * 160U) * tick_ns,
				       &parsed) == 0);
		expected_q4 -= (expected_q4 + 8U) >> 4;
		CHECK_UINT(receiver.jitter_q4, expected_q4);
		CHECK_UINT(stats.jitter_samples, samples_before + i + 1U);
	}
	CHECK_UINT(stats.ts_steps, 1);
	CHECK_UINT(stats.jitter_max, max_before);
}

static void test_jitter_outlier(void)
{
	struct udsp_media media = media_make();
	struct rtp_stats stats;
	struct rtp_receiver receiver;
	struct rtp_packet parsed;
	uint8_t packet[32];
	uint8_t payload = 0;
	uint64_t tick_ns = 125000ULL;
	size_t length;

	CASE("rtp_jitter_re_anchors_twice_for_one_outlier");
	rtp_stats_init(&stats);
	rtp_receiver_init(&receiver, &media, &stats);
	length = packet_make(packet, 500, 0, 67, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 0, &parsed) < 0);
	length = packet_make(packet, 501, 160, 67, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 160 * tick_ns,
			       &parsed) == 0);
	length = packet_make(packet, 502, 320, 67, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 320 * tick_ns,
			       &parsed) == 0);
	length = packet_make(packet, 503, 480U + 0x08000000U, 67, 0, 0, 0,
			     &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 480 * tick_ns,
			       &parsed) == 0);
	CHECK_UINT(stats.ts_steps, 1);
	length = packet_make(packet, 504, 640, 67, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 640 * tick_ns,
			       &parsed) == 0);
	CHECK_UINT(stats.ts_steps, 2);
	CHECK_UINT(receiver.jitter_q4, 0);
	CHECK_UINT(stats.jitter_max, 0);
	CHECK_UINT(stats.jitter_samples, 1);
}

static void test_jitter_stall(void)
{
	struct udsp_media media = media_make();
	struct rtp_stats stats;
	struct rtp_receiver receiver;
	struct rtp_packet parsed;
	uint8_t packet[32];
	uint8_t payload = 0;
	uint32_t expected_q4 = 0;
	uint64_t tick_ns = 125000ULL;
	size_t length;

	CASE("rtp_jitter_counts_a_stall_below_the_step_bound");
	rtp_stats_init(&stats);
	rtp_receiver_init(&receiver, &media, &stats);
	length = packet_make(packet, 200, 0, 64, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 0, &parsed) < 0);
	length = packet_make(packet, 201, 160, 64, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 160 * tick_ns,
			       &parsed) == 0);
	length = packet_make(packet, 202, 320, 64, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 320 * tick_ns,
			       &parsed) == 0);
	length = packet_make(packet, 203, 480, 64, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length,
			       (480U + 40000U) * tick_ns, &parsed) == 0);
	expected_q4 += 40000U - ((expected_q4 + 8U) >> 4);
	CHECK_UINT(receiver.jitter_q4, expected_q4);
	CHECK_UINT(stats.ts_steps, 0);
	length = packet_make(packet, 204, 640, 64, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 640 * tick_ns,
			       &parsed) == 0);
	expected_q4 += 40000U - ((expected_q4 + 8U) >> 4);
	CHECK_UINT(receiver.jitter_q4, expected_q4);
	CHECK_UINT(stats.ts_steps, 0);
	length = packet_make(packet, 205, 800, 64, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 800 * tick_ns,
			       &parsed) == 0);
	expected_q4 -= (expected_q4 + 8U) >> 4;
	CHECK_UINT(receiver.jitter_q4, expected_q4);
	CHECK_UINT(stats.ts_steps, 0);
}

static void test_jitter_timestamp_step_boundary(void)
{
	struct udsp_media media = media_make();
	struct rtp_stats stats;
	struct rtp_receiver receiver;
	struct rtp_packet parsed;
	uint8_t packet[32];
	uint8_t payload = 0;
	uint64_t tick_ns = 125000ULL;
	uint32_t step_ts = RTP_TRANSIT_STEP_TS(RTP_CLOCK_RATE);
	size_t length;

	CASE("rtp_jitter_timestamp_step_boundary");
	rtp_stats_init(&stats);
	rtp_receiver_init(&receiver, &media, &stats);
	length = packet_make(packet, 300, 0, 65, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 0, &parsed) < 0);
	length = packet_make(packet, 301, 160, 65, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 160 * tick_ns,
			       &parsed) == 0);
	length = packet_make(packet, 302, 320, 65, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length,
			       (320U + step_ts) * tick_ns, &parsed) == 0);
	CHECK_UINT(receiver.jitter_q4, 0);
	CHECK_UINT(stats.jitter_samples, 0);
	CHECK_UINT(stats.ts_steps, 1);

	rtp_stats_init(&stats);
	rtp_receiver_init(&receiver, &media, &stats);
	length = packet_make(packet, 400, 0, 66, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 0, &parsed) < 0);
	length = packet_make(packet, 401, 160, 66, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 160 * tick_ns,
			       &parsed) == 0);
	length = packet_make(packet, 402, 320, 66, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length,
			       (320U + step_ts - 1U) * tick_ns, &parsed) == 0);
	CHECK_UINT(receiver.jitter_q4, step_ts - 1U);
	CHECK_UINT(stats.jitter_samples, 1);
	CHECK_UINT(stats.ts_steps, 0);
}

static struct rtcp_report report_make(void)
{
	struct rtcp_report report;

	memset(&report, 0, sizeof(report));
	report.ssrc = 0x10203040U;
	report.fraction_lost = 37;
	report.cumulative_lost = -2;
	report.highest_seq = 0x12345678U;
	report.jitter = 91;
	report.lsr = 0x22334455U;
	report.dlsr = 0x01020304U;
	return report;
}

static void test_rtcp_sr(void)
{
	struct rtcp_sender_info sender = {
		.ssrc = 0x55667788U,
		.ntp_msw = 0x01020304U,
		.ntp_lsw = 0xa0b0c0d0U,
		.rtp_timestamp = 8000,
		.packet_count = 20,
		.octet_count = 3200,
	};
	struct rtcp_report report = report_make();
	struct rtcp_parse_result parsed;
	struct rtcp_state state;
	uint8_t packet[128];
	size_t length;

	CASE("rtcp_sr_field_round_trip");
	CHECK(rtcp_sr_build(&sender, &report, packet, sizeof(packet), &length) == 0);
	CHECK_UINT(length, 52);
	CHECK(rtcp_packet_parse(packet, length, report.ssrc, &parsed) == 0);
	CHECK(parsed.has_sr && parsed.has_report);
	CHECK(memcmp(&parsed.sender, &sender, sizeof(sender)) == 0);
	CHECK_UINT(parsed.report.ssrc, report.ssrc);
	CHECK_UINT(parsed.report.fraction_lost, report.fraction_lost);
	CHECK(parsed.report.cumulative_lost == report.cumulative_lost);
	CHECK_UINT(parsed.report.highest_seq, report.highest_seq);
	CHECK_UINT(parsed.report.jitter, report.jitter);
	CHECK_UINT(parsed.report.lsr, report.lsr);
	CHECK_UINT(parsed.report.dlsr, report.dlsr);
	rtcp_state_init(&state, report.ssrc, NULL);
	rtcp_state_receive(&state, &parsed, 0x20000000U, 0x70000000U);
	rtcp_state_report_timing(&state, 0x20008000U, &report.lsr, &report.dlsr);
	CHECK_UINT(report.lsr, 0x0304a0b0U);
	CHECK_UINT(report.dlsr, 0x8000U);
}

static void test_rtcp_rr_rtt(void)
{
	struct rtcp_report report = report_make();
	struct rtcp_parse_result parsed;
	struct rtp_stats stats;
	struct rtcp_state state;
	uint8_t packet[64];
	size_t length;
	uint32_t now;

	CASE("rtcp_rr_loss_fraction_and_rtt");
	CHECK(rtcp_rr_build(0x55667788U, &report, packet, sizeof(packet),
			    &length) == 0);
	CHECK(rtcp_packet_parse(packet, length, report.ssrc, &parsed) == 0);
	CHECK(parsed.has_rr && parsed.has_report);
	CHECK_UINT(parsed.receiver_ssrc, 0x55667788U);
	CHECK_UINT(parsed.report.fraction_lost, 37);
	memcpy(packet + 32, packet + 8, 24);
	packet[0] = 0x82;
	packet[3] = 13;
	packet[8] = 0xaa;
	length = 56;
	CHECK(rtcp_packet_parse(packet, length, report.ssrc, &parsed) == 0);
	CHECK(parsed.has_report);
	CHECK_UINT(parsed.report.ssrc, report.ssrc);
	rtp_stats_init(&stats);
	rtcp_state_init(&state, report.ssrc, &stats);
	report.lsr = 0x00100000U;
	report.dlsr = 16384U;
	parsed.report = report;
	now = report.lsr + report.dlsr + 6554U;
	rtcp_state_receive(&state, &parsed, 0x90000000U, now);
	CHECK_UINT(stats.rtt_samples, 1);
	CHECK_UINT(stats.rtt_sum_ms, 100);
	CHECK_UINT(stats.loss_fraction_far, 37);
	CHECK_UINT(stats.jitter_far, report.jitter);
}

static void test_rtcp_raw_compounds(void)
{
	static const uint8_t sr_compound[] = {
		0x81, 0xc8, 0x00, 0x0c, 0x55, 0x66, 0x77, 0x88,
		0x01, 0x02, 0x03, 0x04, 0xa0, 0xb0, 0xc0, 0xd0,
		0x00, 0x00, 0x1f, 0x40, 0x00, 0x00, 0x00, 0x14,
		0x00, 0x00, 0x0c, 0x80, 0x10, 0x20, 0x30, 0x40,
		0x25, 0xff, 0xff, 0xfe, 0x12, 0x34, 0x56, 0x78,
		0x00, 0x00, 0x00, 0x5b, 0x22, 0x33, 0x44, 0x55,
		0x01, 0x02, 0x03, 0x04,
		0x81, 0xca, 0x00, 0x02, 0x55, 0x66, 0x77, 0x88,
		0x01, 0x01, 0x78, 0x00,
		0x81, 0xcb, 0x00, 0x01, 0x55, 0x66, 0x77, 0x88,
	};
	static const uint8_t rr_compound[] = {
		0x81, 0xc9, 0x00, 0x07, 0x55, 0x66, 0x77, 0x88,
		0x10, 0x20, 0x30, 0x40, 0x25, 0xff, 0xff, 0xfe,
		0x12, 0x34, 0x56, 0x78, 0x00, 0x00, 0x00, 0x5b,
		0x22, 0x33, 0x44, 0x55, 0x01, 0x02, 0x03, 0x04,
		0x81, 0xcb, 0x00, 0x01, 0x55, 0x66, 0x77, 0x88,
	};
	struct rtcp_sender_info sender = {
		.ssrc = 0x55667788U,
		.ntp_msw = 0x01020304U,
		.ntp_lsw = 0xa0b0c0d0U,
		.rtp_timestamp = 8000,
		.packet_count = 20,
		.octet_count = 3200,
	};
	struct rtcp_report report = report_make();
	struct rtcp_parse_result parsed;
	uint8_t built[128];
	size_t length;
	size_t offset;

	CASE("rtcp_raw_sr_sdes_bye_offsets");
	CHECK_UINT(uint32_test_read(sr_compound + 4), sender.ssrc);
	CHECK_UINT(uint32_test_read(sr_compound + 8), sender.ntp_msw);
	CHECK_UINT(uint32_test_read(sr_compound + 12), sender.ntp_lsw);
	CHECK_UINT(uint32_test_read(sr_compound + 16), sender.rtp_timestamp);
	CHECK_UINT(uint32_test_read(sr_compound + 20), sender.packet_count);
	CHECK_UINT(uint32_test_read(sr_compound + 24), sender.octet_count);
	CHECK_UINT(uint32_test_read(sr_compound + 28), report.ssrc);
	CHECK_UINT(sr_compound[32], report.fraction_lost);
	CHECK_UINT(uint32_test_read(sr_compound + 36), report.highest_seq);
	CHECK_UINT(uint32_test_read(sr_compound + 40), report.jitter);
	CHECK_UINT(uint32_test_read(sr_compound + 44), report.lsr);
	CHECK_UINT(uint32_test_read(sr_compound + 48), report.dlsr);
	CHECK(rtcp_sr_build(&sender, &report, built, sizeof(built), &length) == 0);
	CHECK_UINT(length, 52);
	CHECK(memcmp(built, sr_compound, length) == 0);
	offset = length;
	CHECK(rtcp_sdes_build(sender.ssrc, "x", built + offset,
			      sizeof(built) - offset, &length) == 0);
	CHECK(memcmp(built + offset, sr_compound + offset, length) == 0);
	offset += length;
	CHECK(rtcp_bye_build(sender.ssrc, NULL, built + offset,
			     sizeof(built) - offset, &length) == 0);
	CHECK(memcmp(built + offset, sr_compound + offset, length) == 0);
	CHECK(rtcp_compound_parse(sr_compound, sizeof(sr_compound), report.ssrc,
				  &parsed) == 0);
	CHECK(parsed.has_sr && parsed.has_report && parsed.has_cname &&
	      parsed.has_bye);
	CHECK(memcmp(&parsed.sender, &sender, sizeof(sender)) == 0);
	CHECK_UINT(parsed.report.highest_seq, report.highest_seq);
	CHECK_UINT(parsed.report.jitter, report.jitter);
	CHECK_UINT(parsed.report.lsr, report.lsr);
	CHECK_UINT(parsed.report.dlsr, report.dlsr);
	CHECK(strcmp(parsed.cname, "x") == 0);
	CHECK_UINT(parsed.bye_ssrc, sender.ssrc);

	CASE("rtcp_raw_rr_bye_without_sdes_offsets");
	CHECK_UINT(uint32_test_read(rr_compound + 4), sender.ssrc);
	CHECK_UINT(uint32_test_read(rr_compound + 8), report.ssrc);
	CHECK_UINT(rr_compound[12], report.fraction_lost);
	CHECK_UINT(uint32_test_read(rr_compound + 16), report.highest_seq);
	CHECK_UINT(uint32_test_read(rr_compound + 20), report.jitter);
	CHECK_UINT(uint32_test_read(rr_compound + 24), report.lsr);
	CHECK_UINT(uint32_test_read(rr_compound + 28), report.dlsr);
	CHECK(rtcp_rr_build(sender.ssrc, &report, built, sizeof(built),
			    &length) == 0);
	CHECK_UINT(length, 32);
	CHECK(memcmp(built, rr_compound, length) == 0);
	CHECK(rtcp_compound_parse(rr_compound, sizeof(rr_compound), report.ssrc,
				  &parsed) == 0);
	CHECK(parsed.has_rr && parsed.has_report && parsed.has_bye);
	CHECK(!parsed.has_cname);
	CHECK_UINT(parsed.receiver_ssrc, sender.ssrc);
	CHECK_UINT(parsed.bye_ssrc, sender.ssrc);
}

static void test_rtcp_relaxed_forms(void)
{
	static const uint8_t multi_sdes[] = {
		0x80, 0xc9, 0x00, 0x01, 0x00, 0x00, 0x00, 0x09,
		0x82, 0xca, 0x00, 0x04,
		0x00, 0x00, 0x00, 0x08, 0x01, 0x01, 0x78, 0x00,
		0x00, 0x00, 0x00, 0x09, 0x01, 0x01, 0x79, 0x00,
	};
	struct rtcp_sender_info sender = {
		.ssrc = 7,
		.ntp_msw = 1,
		.ntp_lsw = 2,
		.rtp_timestamp = 3,
		.packet_count = 4,
		.octet_count = 5,
	};
	struct rtcp_report report = report_make();
	struct rtcp_parse_result parsed;
	uint8_t packet[128];
	size_t length;

	CASE("rtcp_zero_report_blocks");
	CHECK(rtcp_rr_build(9, NULL, packet, sizeof(packet), &length) == 0);
	CHECK_UINT(length, 8);
	CHECK_UINT(packet[0], 0x80);
	CHECK_UINT(packet[1], 201);
	CHECK(rtcp_packet_parse(packet, length, 0, &parsed) == 0);
	CHECK(parsed.has_rr && !parsed.has_report);
	CHECK(rtcp_sr_build(&sender, NULL, packet, sizeof(packet), &length) == 0);
	CHECK_UINT(length, 28);
	CHECK_UINT(packet[0], 0x80);
	CHECK(rtcp_packet_parse(packet, length, 0, &parsed) == 0);
	CHECK(parsed.has_sr && !parsed.has_report);

	CASE("rtcp_multi_sdes_selects_primary_ssrc");
	CHECK(rtcp_compound_parse(multi_sdes, sizeof(multi_sdes), 0, &parsed) == 0);
	CHECK(parsed.has_rr && parsed.has_cname);
	CHECK_UINT(parsed.cname_ssrc, 9);
	CHECK(strcmp(parsed.cname, "y") == 0);

	CASE("rtcp_report_extension_and_ssrc_zero");
	report.ssrc = 0;
	CHECK(rtcp_rr_build(9, &report, packet, sizeof(packet), &length) == 0);
	memcpy(packet + 32, packet + 8, 24);
	packet[8] = 0;
	packet[9] = 0;
	packet[10] = 0;
	packet[11] = 5;
	packet[0] = 0x82;
	length = 56;
	packet[length++] = 0xde;
	packet[length++] = 0xad;
	packet[length++] = 0xbe;
	packet[length++] = 0xef;
	packet[2] = 0;
	packet[3] = 14;
	CHECK(rtcp_packet_parse(packet, length, 0, &parsed) == 0);
	CHECK(parsed.has_report);
	CHECK_UINT(parsed.report.ssrc, 0);
	CHECK_UINT(parsed.report.highest_seq, report.highest_seq);

	CASE("rtcp_malformed_trailing_packet_keeps_prefix");
	CHECK(rtcp_rr_build(9, NULL, packet, sizeof(packet), &length) == 0);
	packet[length] = 0x80;
	packet[length + 1U] = 202;
	packet[length + 2U] = 0xff;
	packet[length + 3U] = 0xff;
	CHECK(rtcp_compound_parse(packet, length + 4U, 0, &parsed) == 0);
	CHECK(parsed.has_rr);
	CHECK_UINT(parsed.receiver_ssrc, 9);
}

static void test_rtcp_padding(void)
{
	struct rtcp_parse_result parsed;
	uint8_t packet[64];
	size_t length;
	size_t rr_len;
	size_t bye_len;

	CASE("rtcp_padded_packet_strips_padding");
	CHECK(rtcp_rr_build(0x12345678U, NULL, packet, sizeof(packet), &length) == 0);
	packet[0] |= 0x20U;
	packet[2] = 0;
	packet[3] = 2;
	packet[8] = 0xaa;
	packet[9] = 0xbb;
	packet[10] = 0xcc;
	packet[11] = 4;
	length = 12;
	CHECK(rtcp_packet_parse(packet, length, 0, &parsed) == 0);
	CHECK(parsed.has_rr && !parsed.has_report);
	CHECK_UINT(parsed.receiver_ssrc, 0x12345678U);
	packet[11] = 2;
	CHECK(rtcp_packet_parse(packet, length, 0, &parsed) < 0);
	memset(packet, 0, 8);
	packet[0] = 0xa0;
	packet[1] = 204;
	packet[3] = 1;
	packet[7] = 2;
	CHECK(rtcp_packet_parse(packet, 8, 0, &parsed) < 0);
	CHECK(rtcp_rr_build(7, NULL, packet, sizeof(packet), &rr_len) == 0);
	CHECK(rtcp_bye_build(7, NULL, packet + rr_len,
			     sizeof(packet) - rr_len, &bye_len) == 0);
	packet[rr_len] |= 0x20U;
	packet[rr_len + 3U] = 2;
	packet[rr_len + bye_len] = 0xaa;
	packet[rr_len + bye_len + 1U] = 0xbb;
	packet[rr_len + bye_len + 2U] = 0xcc;
	packet[rr_len + bye_len + 3U] = 4;
	CHECK(rtcp_compound_parse(packet, rr_len + bye_len + 4U, 0,
				  &parsed) == 0);
	CHECK(parsed.has_rr && parsed.has_bye);
}

static void test_sender_octets(void)
{
	struct udsp_media media = media_make();
	struct rtp_entropy entropy = entropy_make(55);
	struct rtp_stats stats;
	struct rtp_sender rtp;
	struct telev_sender telev;
	struct rtcp_sender_info sender;
	uint8_t packet[64];
	uint8_t payload[5] = { 0 };
	size_t length;

	CASE("rtcp_sr_uses_payload_octet_count");
	rtp_stats_init(&stats);
	CHECK(rtp_sender_init(&rtp, &media, &entropy, &stats) == 0);
	CHECK(rtp_packet_build(&rtp, payload, sizeof(payload), 160, packet,
			       sizeof(packet), &length) == 0);
	telev_sender_init(&telev, &rtp);
	CHECK(telev_sender_start(&telev, '2', 10) == 0);
	CHECK(telev_sender_packet_build(&telev, packet, sizeof(packet),
					&length) == 0);
	rtcp_sender_info_get(&rtp, 0x01020304U, 0x50607080U, &sender);
	CHECK_UINT(stats.packets_sent, 2);
	CHECK_UINT(stats.octets_sent, 9);
	CHECK_UINT(sender.packet_count, 2);
	CHECK_UINT(sender.octet_count, 9);
	CHECK_UINT(sender.ssrc, rtp.ssrc);
	CHECK_UINT(sender.rtp_timestamp, rtp.timestamp);
}

static void test_rtcp_compound(void)
{
	struct rtcp_report report = report_make();
	struct rtcp_parse_result parsed;
	uint8_t compound[512];
	size_t rr_len;
	size_t sdes_len;
	size_t bye_len;

	CASE("rtcp_sdes_cname_and_bye");
	CHECK(rtcp_rr_build(0x55667788U, &report, compound, sizeof(compound),
			    &rr_len) == 0);
	CHECK(rtcp_sdes_build(0x55667788U, "line1@host", compound + rr_len,
			      sizeof(compound) - rr_len, &sdes_len) == 0);
	CHECK(rtcp_bye_build(0x55667788U, "done", compound + rr_len + sdes_len,
			     sizeof(compound) - rr_len - sdes_len, &bye_len) == 0);
	CHECK(rtcp_compound_parse(compound, rr_len + sdes_len + bye_len,
				  report.ssrc,
				  &parsed) == 0);
	CHECK(parsed.has_rr && parsed.has_cname && parsed.has_bye);
	CHECK(strcmp(parsed.cname, "line1@host") == 0);
	CHECK_UINT(parsed.bye_ssrc, 0x55667788U);
}

static void test_rtcp_malformed(void)
{
	struct rtcp_report report = report_make();
	struct rtcp_parse_result parsed;
	uint8_t compound[128];
	size_t rr_len;
	size_t sdes_len;

	CASE("rtcp_malformed_compounds_rejected");
	CHECK(rtcp_rr_build(9, &report, compound, sizeof(compound), &rr_len) == 0);
	CHECK(rtcp_compound_parse(compound, rr_len, report.ssrc, &parsed) < 0);
	CHECK(rtcp_sdes_build(9, "a", compound + rr_len,
			      sizeof(compound) - rr_len, &sdes_len) == 0);
	CHECK(rtcp_compound_parse(compound, rr_len + sdes_len - 1U,
				  report.ssrc, &parsed) == 0);
	CHECK(parsed.has_rr && !parsed.has_cname);
	compound[0] = 0x40U | (compound[0] & 0x1fU);
	CHECK(rtcp_compound_parse(compound, rr_len + sdes_len, report.ssrc,
				  &parsed) < 0);
	compound[0] = 0xa0U | (compound[0] & 0x1fU);
	CHECK(rtcp_compound_parse(compound, rr_len + sdes_len, report.ssrc,
				  &parsed) < 0);
	compound[0] = 0x80U | (compound[0] & 0x1fU);
	compound[rr_len + sdes_len - 1U] = 1;
	CHECK(rtcp_compound_parse(compound, rr_len + sdes_len, report.ssrc,
				  &parsed) == 0);
	CHECK(parsed.has_rr && !parsed.has_cname);
}

static void test_telev(void)
{
	struct udsp_media media = media_make();
	struct rtp_stats stats;
	struct rtp_sender rtp;
	struct rtp_entropy entropy = entropy_make(3);
	struct telev_sender sender;
	struct rtp_receiver rtp_receiver;
	struct telev_receiver receiver;
	struct rtp_packet parsed;
	struct telev_event event;
	uint8_t packets[5][RTP_HEADER_SIZE + 4U];
	size_t lengths[5];
	int reports = 0;
	unsigned i;

	CASE("telev_start_update_three_end_dedup");
	rtp_stats_init(&stats);
	CHECK(rtp_sender_init(&rtp, &media, &entropy, &stats) == 0);
	rtp.sequence = 500;
	rtp.timestamp = 10000;
	telev_sender_init(&sender, &rtp);
	CHECK(telev_sender_start(&sender, '5', 10) == 0);
	CHECK(telev_sender_packet_build(&sender, packets[0], sizeof(packets[0]),
					&lengths[0]) == 0);
	CHECK(packets[0][1] & 0x80U);
	CHECK(!(packets[0][13] & 0x80U));
	CHECK(telev_sender_update(&sender, 160) == 0);
	CHECK(telev_sender_packet_build(&sender, packets[1], sizeof(packets[1]),
					&lengths[1]) == 0);
	CHECK(!(packets[1][1] & 0x80U));
	CHECK(telev_sender_end(&sender, 400) == 0);
	for (i = 2; i < 5; i++) {
		CHECK(telev_sender_packet_build(&sender, packets[i], sizeof(packets[i]),
						&lengths[i]) == 0);
		CHECK(packets[i][13] & 0x80U);
		CHECK(!(packets[i][1] & 0x80U));
		CHECK_UINT(((uint16_t)packets[i][14] << 8) | packets[i][15], 400);
	}
	CHECK(telev_sender_packet_build(&sender, packets[0], sizeof(packets[0]),
					&lengths[0]) < 0);
	CHECK_UINT(stats.packets_sent, 5);
	CHECK_UINT(stats.octets_sent, 20);
	CHECK_UINT(rtp.timestamp, 10400);
	rtp_receiver_init(&rtp_receiver, &media, NULL);
	telev_receiver_init(&receiver);
	for (i = 0; i < 5; i++) {
		int result = rtp_packet_parse(&rtp_receiver, packets[i], lengths[i],
					  i * 20000000ULL, &parsed);

		if (!i) {
			CHECK(result < 0);
			continue;
		}
		CHECK(result == 0);
		result = telev_packet_parse(&receiver, &parsed, &event);
		CHECK(result >= 0);
		if (result > 0)
			reports++;
	}
	CHECK_UINT(reports, 1);
	CHECK_UINT(event.digit, '5');
	CHECK_UINT(event.duration_ms, 50);
}

static void test_telev_receive_recovery(void)
{
	struct telev_receiver receiver;
	struct telev_event event;
	struct rtp_packet packet;
	uint8_t payload[6];
	unsigned i;

	CASE("telev_recent_end_ring_and_reserved_bit");
	telev_receiver_init(&receiver);
	memset(&packet, 0, sizeof(packet));
	packet.kind = RTP_PAYLOAD_TELEPHONE_EVENT;
	packet.payload = payload;
	packet.payload_len = sizeof(payload);
	for (i = 0; i < 5U; i++) {
		payload[0] = (uint8_t)i;
		payload[1] = 0xc0U;
		payload[2] = 0;
		payload[3] = 80;
		packet.timestamp = i * 1000U;
		CHECK(telev_packet_parse(&receiver, &packet, &event) == 1);
		CHECK_UINT(event.digit, (uint8_t)('0' + i));
	}
	for (i = 1; i < 5U; i++) {
		payload[0] = (uint8_t)i;
		packet.timestamp = i * 1000U;
		CHECK(telev_packet_parse(&receiver, &packet, &event) == 0);
	}
	payload[0] = 0;
	packet.timestamp = 0;
	CHECK(telev_packet_parse(&receiver, &packet, &event) == 1);
	telev_receiver_init(&receiver);
	payload[0] = 6;
	payload[2] = 0;
	payload[3] = 80;
	packet.timestamp = 7000;
	CHECK(telev_packet_parse(&receiver, &packet, &event) == 1);
	payload[3] = 160;
	CHECK(telev_packet_parse(&receiver, &packet, &event) == 1);

	CASE("telev_reports_start_when_all_ends_lost");
	telev_receiver_init(&receiver);
	payload[0] = 5;
	payload[1] = 0;
	payload[2] = 0;
	payload[3] = 0;
	packet.timestamp = 10000;
	packet.arrival_ns = 0;
	packet.payload_len = 4;
	CHECK(telev_packet_parse(&receiver, &packet, &event) == 0);
	payload[1] = 0x40U;
	payload[2] = 1;
	payload[3] = 144;
	packet.payload_len = sizeof(payload);
	packet.arrival_ns = 50000000ULL;
	CHECK(telev_packet_parse(&receiver, &packet, &event) == 0);
	packet.kind = RTP_PAYLOAD_AUDIO;
	packet.arrival_ns = 100000000ULL;
	CHECK(telev_packet_parse(&receiver, &packet, &event) == 1);
	CHECK_UINT(event.digit, '5');
	CHECK_UINT(event.duration_ms, 50);
}

static void play_packet_set(struct rtp_packet *packet, uint8_t *payload,
			    uint8_t code, uint8_t volume, bool ending,
			    uint16_t duration, uint32_t timestamp,
			    uint64_t arrival_ns)
{
	payload[0] = code;
	payload[1] = (uint8_t)(volume | (ending ? 0x80U : 0U));
	payload[2] = (uint8_t)(duration >> 8);
	payload[3] = (uint8_t)duration;
	memset(packet, 0, sizeof(*packet));
	packet->kind = RTP_PAYLOAD_TELEPHONE_EVENT;
	packet->payload = payload;
	packet->payload_len = 4U;
	packet->timestamp = timestamp;
	packet->arrival_ns = arrival_ns;
}

/* One 10 ms tick of a narrowband stream, which covers 80 clock units. Answers
 * the units it rendered, 0 for a tick after the event ends. */
static uint32_t play_tick_run(struct telev_play *play, uint64_t now_ns)
{
	uint32_t left;
	uint32_t units;

	if (!telev_play_tick(play, now_ns, &left))
		return 0;
	units = left < 80U ? left : 80U;
	telev_play_advance(play, units);

	return units;
}

static void test_telev_play(void)
{
	struct rtp_packet packet;
	struct telev_play play;
	uint8_t payload[4];
	uint32_t total = 0;
	unsigned i;

	CASE("telev_play_renders_from_the_first_packet");
	/* The production bound, so a shrunk one in the environment cannot
	 * hide a wrong constant (README, test timing). */
	CHECK_UINT(telev_play_timeout_ms(), 120);
	telev_play_init(&play);
	CHECK_UINT(play.timeout_ms, 120);
	play_packet_set(&packet, payload, 5U, 10U, false, 0, 1000U, 0);
	CHECK(telev_play_packet(&play, &packet) == 1);
	CHECK_UINT(play.digit, '5');
	for (i = 1; i <= 5U; i++)
		total += play_tick_run(&play, i * 10000000ULL);
	CHECK_UINT(total, 400);
	/* The duration the far end announces is what the event renders. */
	play_packet_set(&packet, payload, 5U, 10U, false, 800, 1000U,
			60000000ULL);
	CHECK(telev_play_packet(&play, &packet) == 0);
	CHECK_UINT(play.target, 800);
	play_packet_set(&packet, payload, 5U, 10U, true, 800, 1000U,
			70000000ULL);
	CHECK(telev_play_packet(&play, &packet) == 0);
	CHECK(play.ended);
	for (i = 8; i <= 30U; i++)
		total += play_tick_run(&play, i * 10000000ULL);
	CHECK_UINT(total, 800);
	CHECK(play.done);

	CASE("telev_play_first_seen_end_renders_its_duration_once");
	telev_play_init(&play);
	play_packet_set(&packet, payload, 5U, 10U, true, 400, 2000U, 0);
	CHECK(telev_play_packet(&play, &packet) == 1);
	total = 0;
	for (i = 1; i <= 10U; i++)
		total += play_tick_run(&play, i * 10000000ULL);
	CHECK_UINT(total, 400);
	/* The second and third end packets of RFC 4733 2.5.1.4 carry the
	 * identity the tracker already holds, so they start nothing. */
	play_packet_set(&packet, payload, 5U, 10U, true, 400, 2000U,
			30000000ULL);
	CHECK(telev_play_packet(&play, &packet) == 0);
	for (i = 11; i <= 15U; i++)
		total += play_tick_run(&play, i * 10000000ULL);
	CHECK_UINT(total, 400);

	CASE("telev_play_a_first_seen_end_outlasts_the_lost_end_bound");
	/* The end came, so the bound for an end that never comes does not cut
	 * what that end announced: 2400 units is 300 ms of render behind one
	 * packet, well past the 120 ms bound. */
	telev_play_init(&play);
	play_packet_set(&packet, payload, 5U, 10U, true, 2400, 2000U, 0);
	CHECK(telev_play_packet(&play, &packet) == 1);
	total = 0;
	for (i = 1; i <= 40U; i++)
		total += play_tick_run(&play, i * 10000000ULL);
	CHECK_UINT(total, 2400);
	CHECK(play.done);

	CASE("telev_play_a_segment_continuation_keeps_the_event");
	/* A 48 kHz clock fills the duration field at 1365 ms, so the sender
	 * closes the segment there and the next one starts at that end. */
	telev_play_init(&play);
	play_packet_set(&packet, payload, 5U, 10U, false, 0, 1000U, 0);
	CHECK(telev_play_packet(&play, &packet) == 1);
	CHECK(play_tick_run(&play, 10000000ULL) == 80U);
	play_packet_set(&packet, payload, 5U, 10U, false, 65535, 1000U,
			20000000ULL);
	CHECK(telev_play_packet(&play, &packet) == 0);
	play_packet_set(&packet, payload, 5U, 10U, true, 800, 1000U + 65535U,
			30000000ULL);
	CHECK(telev_play_packet(&play, &packet) == 0);
	CHECK_UINT(play.event_ts, 1000U + 65535U);
	CHECK_UINT(play.target, 65535U + 800U);
	CHECK_UINT(play.rendered, 80);
	CHECK(play.ended);
	CHECK(play_tick_run(&play, 40000000ULL) == 80U);
	/* The ceiling counts from the first packet of the chain, not from the
	 * continuation. */
	CHECK(play_tick_run(&play, 8000000000ULL) == 0);
	CHECK(play.done);

	CASE("telev_play_a_reordered_start_of_an_older_event_is_ignored");
	telev_play_init(&play);
	play_packet_set(&packet, payload, 6U, 10U, false, 0, 5000U, 0);
	CHECK(telev_play_packet(&play, &packet) == 1);
	play_packet_set(&packet, payload, 5U, 10U, false, 160, 1000U,
			10000000ULL);
	CHECK(telev_play_packet(&play, &packet) == 0);
	CHECK_UINT(play.digit, '6');
	CHECK_UINT(play.event_ts, 5000U);

	CASE("telev_play_late_end_of_an_older_event_is_ignored");
	telev_play_init(&play);
	play_packet_set(&packet, payload, 5U, 10U, false, 0, 1000U, 0);
	CHECK(telev_play_packet(&play, &packet) == 1);
	play_packet_set(&packet, payload, 6U, 10U, false, 0, 5000U,
			20000000ULL);
	CHECK(telev_play_packet(&play, &packet) == 1);
	CHECK_UINT(play.digit, '6');
	play_packet_set(&packet, payload, 5U, 10U, true, 800, 1000U,
			30000000ULL);
	CHECK(telev_play_packet(&play, &packet) == 0);
	CHECK_UINT(play.digit, '6');
	CHECK_UINT(play.event_ts, 5000U);
	CHECK(!play.ended);

	CASE("telev_play_a_new_timestamp_restarts_the_same_digit");
	telev_play_init(&play);
	play_packet_set(&packet, payload, 5U, 10U, false, 0, 1000U, 0);
	CHECK(telev_play_packet(&play, &packet) == 1);
	CHECK(play_tick_run(&play, 10000000ULL) == 80U);
	play_packet_set(&packet, payload, 5U, 10U, false, 0, 3000U,
			20000000ULL);
	CHECK(telev_play_packet(&play, &packet) == 1);
	CHECK_UINT(play.event_ts, 3000U);
	CHECK_UINT(play.rendered, 0);

	CASE("telev_play_the_ceiling_ends_it");
	telev_play_init(&play);
	play_packet_set(&packet, payload, 5U, 10U, false, 0, 1000U, 0);
	CHECK(telev_play_packet(&play, &packet) == 1);
	/* The far end keeps the event open, so the ceiling is the only bound
	 * that can end it. */
	play_packet_set(&packet, payload, 5U, 10U, false, 63920, 1000U,
			7990000000ULL);
	CHECK(telev_play_packet(&play, &packet) == 0);
	CHECK(play_tick_run(&play, 7990000000ULL) == 80U);
	CHECK(play_tick_run(&play, 8000000000ULL) == 0);
	CHECK(play.done);

	CASE("telev_play_the_lost_end_stops_at_the_timeout");
	telev_play_init(&play);
	play_packet_set(&packet, payload, 5U, 10U, false, 0, 1000U, 0);
	CHECK(telev_play_packet(&play, &packet) == 1);
	CHECK(play_tick_run(&play, 110000000ULL) == 80U);
	CHECK(play_tick_run(&play, 120000000ULL) == 0);
	CHECK(play.done);

	CASE("telev_play_init_drops_the_event_of_the_old_source");
	telev_play_init(&play);
	play_packet_set(&packet, payload, 5U, 10U, false, 0, 1000U, 0);
	CHECK(telev_play_packet(&play, &packet) == 1);
	CHECK(play_tick_run(&play, 10000000ULL) == 80U);
	telev_play_init(&play);
	CHECK(play_tick_run(&play, 20000000ULL) == 0);
	CHECK(!play.active);

	/* The caller builds the generator on a 1 alone, so every later packet
	 * of the event must answer 0 and keep the level the event started at,
	 * whatever volume byte it carries. */
	CASE("telev_play_keeps_the_volume_of_the_first_packet");
	telev_play_init(&play);
	play_packet_set(&packet, payload, 5U, 20U, false, 0, 1000U, 0);
	CHECK(telev_play_packet(&play, &packet) == 1);
	CHECK_UINT(play.volume, 20);
	play_packet_set(&packet, payload, 5U, 30U, false, 400, 1000U,
			10000000ULL);
	CHECK(telev_play_packet(&play, &packet) == 0);
	CHECK_UINT(play.volume, 20);
	play_packet_set(&packet, payload, 5U, 31U, false, 65535, 1000U,
			20000000ULL);
	CHECK(telev_play_packet(&play, &packet) == 0);
	play_packet_set(&packet, payload, 5U, 40U, false, 160,
			1000U + 65535U, 30000000ULL);
	CHECK(telev_play_packet(&play, &packet) == 0);
	CHECK_UINT(play.event_ts, 1000U + 65535U);
	CHECK_UINT(play.volume, 20);
	for (i = 0; i < 3U; i++) {
		play_packet_set(&packet, payload, 5U, 50U, true, 800,
				1000U + 65535U, (40U + i) * 1000000ULL);
		CHECK(telev_play_packet(&play, &packet) == 0);
	}
	CHECK(play.ended);
	CHECK_UINT(play.volume, 20);

	CASE("telev_play_a_first_seen_end_keeps_its_own_volume");
	telev_play_init(&play);
	play_packet_set(&packet, payload, 5U, 7U, true, 400, 2000U, 0);
	CHECK(telev_play_packet(&play, &packet) == 1);
	CHECK_UINT(play.volume, 7);
	play_packet_set(&packet, payload, 5U, 9U, true, 400, 2000U,
			10000000ULL);
	CHECK(telev_play_packet(&play, &packet) == 0);
	CHECK_UINT(play.volume, 7);

	CASE("telev_play_a_new_event_takes_its_own_volume");
	telev_play_init(&play);
	play_packet_set(&packet, payload, 5U, 20U, false, 0, 1000U, 0);
	CHECK(telev_play_packet(&play, &packet) == 1);
	play_packet_set(&packet, payload, 5U, 12U, false, 0, 3000U,
			20000000ULL);
	CHECK(telev_play_packet(&play, &packet) == 1);
	CHECK_UINT(play.volume, 12);
	play_packet_set(&packet, payload, 6U, 63U, false, 0, 5000U,
			40000000ULL);
	CHECK(telev_play_packet(&play, &packet) == 1);
	CHECK_UINT(play.volume, 63);
}

static void test_payload_demux(void)
{
	struct udsp_media media = media_make();
	struct rtp_receiver receiver;
	struct rtp_packet parsed;
	uint8_t packet[32];
	uint8_t audio = 1;
	uint8_t event[] = { 2, 0, 0, 80 };
	size_t length;

	CASE("rtp_audio_event_pt_demux");
	rtp_receiver_init(&receiver, &media, NULL);
	length = packet_make(packet, 1, 0, 6, 0, 0, 0, &audio, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 0, &parsed) < 0);
	length = packet_make(packet, 2, 160, 6, 0, 0, 0, &audio, 1);
	CHECK(rtp_packet_parse(&receiver, packet, length, 20000000, &parsed) == 0);
	CHECK_UINT(parsed.kind, RTP_PAYLOAD_AUDIO);
	length = packet_make(packet, 3, 160, 6, 101, 0, 0, event, sizeof(event));
	CHECK(rtp_packet_parse(&receiver, packet, length, 21000000, &parsed) == 0);
	CHECK_UINT(parsed.kind, RTP_PAYLOAD_TELEPHONE_EVENT);
}

static double double_abs(double value)
{
	return value < 0.0 ? -value : value;
}

static void test_g107(void)
{
	float mos_lq;
	float mos_cq;
	uint32_t rfactor;
	double expected;
	double score;

	CASE("stats_g107_zero_and_loss_delay");
	rtp_stats_quality(0.0, 0, &mos_lq, &mos_cq, &rfactor);
	CHECK_UINT(rfactor, 93);
	CHECK(double_abs(mos_lq - 4.406f) < 0.02);
	CHECK(double_abs(mos_cq - 4.406f) < 0.02);
	expected = 93.2 - 0.024 * 150.0 - 95.0 * 5.0 / (5.0 + 25.1);
	score = rtp_stats_rfactor(5.0, 150);
	CHECK(double_abs(score - expected) < 0.1);
	rtp_stats_quality(5.0, 150, &mos_lq, &mos_cq, &rfactor);
	CHECK_UINT(rfactor, 74);
	CHECK(double_abs(mos_cq - 3.7706) < 0.005);
	CHECK(double_abs(mos_lq - 3.9228) < 0.005);
}

static void test_stats_snapshot(void)
{
	struct rtp_stats stats;
	struct rtp_receiver receiver;
	struct udsp_media_stats out;

	CASE("stats_media_fields_snapshot");
	rtp_stats_init(&stats);
	memset(&receiver, 0, sizeof(receiver));
	stats.packets_sent = 4;
	stats.packets_recv = 8;
	stats.packets_discarded = 2;
	stats.bytes_sent = 400;
	stats.bytes_recv = 800;
	stats.loss_fraction = 51;
	stats.loss_fraction_far = 20;
	stats.jitter_sum = 80;
	stats.jitter_samples = 2;
	stats.jitter_max = 64;
	stats.jitter_far = 72;
	stats.rtt_sum_ms = 220;
	stats.rtt_samples = 2;
	stats.rtt_max_ms = 120;
	stats.model_delay_ms = 50;
	receiver.source_valid = true;
	receiver.base_seq = 100;
	receiver.max_seq = 109;
	receiver.received = 8;
	rtp_stats_snapshot(&stats, &receiver, &out);
	CHECK_UINT(out.packets_sent, 4);
	CHECK_UINT(out.packets_recv, 8);
	CHECK_UINT(out.packets_lost, 2);
	CHECK_UINT(out.packets_discarded, 2);
	CHECK_UINT(out.loss_rate, 51);
	CHECK_UINT(out.loss_rate_far, 20);
	CHECK_UINT(out.jitter_avg, 5);
	CHECK_UINT(out.jitter_max, 8);
	CHECK_UINT(out.jitter_far, 9);
	CHECK_UINT(out.rtt_avg, 110);
	CHECK_UINT(out.rtt_max, 120);
	CHECK_UINT(out.bytes_sent, 400);
	CHECK_UINT(out.bytes_recv, 800);
	CHECK(out.mos_lq > 1.0f && out.mos_cq > 1.0f);
}

/* --- the 48 kHz stream --------------------------------------------------- */

/* The stream clock's tick is not a whole number of nanoseconds above 8 kHz, so
 * every instant here is built from a tick count and rounded, not truncated.
 * rtp.c floors the conversion back, and a truncated instant would fall one tick
 * short of the transit the case states. */
static uint64_t wb_arrival_ns(uint32_t ticks, uint32_t clock_rate)
{
	return ((uint64_t)ticks * 1000000000ULL + clock_rate / 2U) / clock_rate;
}

/*
 * RFC 3550 6.4.1 measures the transit in the units of the stream. The same
 * wall-clock jitter therefore reads six times larger at 48 kHz than at 8 kHz.
 * The estimator, the report block and the millisecond conversion must all
 * count in the same clock.
 */
static void test_wb_jitter_at_clock(void)
{
	struct udsp_media media = media_make();
	struct udsp_media_stats snapshot;
	struct rtp_receiver receiver;
	struct rtp_packet parsed;
	struct rtcp_report report;
	struct rtp_stats stats;
	uint8_t packet[32];
	uint8_t payload = 0;
	uint32_t transits[] = { 480U, 960U, 480U, 2400U };
	uint32_t expected_q4 = 0;
	uint32_t prior = 0;
	unsigned i;

	CASE("rtp48_jitter_estimator_at_clock");
	rtp_stats_init_cfg(&stats, 48000U);
	rtp_receiver_init_cfg(&receiver, &media, 48000U, &stats);
	CHECK_UINT(receiver.clock_rate, 48000);
	CHECK_UINT(stats.clock_rate, 48000);
	for (i = 0; i < sizeof(transits) / sizeof(transits[0]); i++) {
		uint32_t timestamp = i * 960U;
		uint64_t arrival_ns = wb_arrival_ns(timestamp + transits[i],
						    48000U);
		size_t length = packet_make(packet, (uint16_t)(600U + i),
					    timestamp, 77, 0, 0, 0, &payload, 1);

		if (!i) {
			CHECK(rtp_packet_parse(&receiver, packet, length,
					       arrival_ns, &parsed) < 0);
			continue;
		}
		CHECK(rtp_packet_parse(&receiver, packet, length, arrival_ns,
				       &parsed) == 0);
		if (i > 1U) {
			uint32_t delta = transits[i] > prior ?
					 transits[i] - prior : prior - transits[i];

			expected_q4 += delta - ((expected_q4 + 8U) >> 4);
		}
		prior = transits[i];
	}
	rtp_receiver_report(&receiver, 0, 0, &report);
	CHECK_UINT(receiver.jitter_q4, expected_q4);
	/* The wire carries the stream clock's units (RFC 7160: one clock per
	 * SSRC), and only the snapshot converts to milliseconds. */
	CHECK_UINT(report.jitter, expected_q4 >> 4);
	stats.jitter_far = 4800U;
	rtp_stats_snapshot(&stats, &receiver, &snapshot);
	CHECK_UINT(snapshot.jitter_max, stats.jitter_max / 48U);
	CHECK_UINT(snapshot.jitter_far, 100);
	CHECK_UINT(snapshot.jitter_avg,
		   stats.jitter_sum / stats.jitter_samples / 48U);
}

/*
 * The transit re-anchor bound is ten seconds at every clock, not a fixed tick
 * count. The 80000 ticks that are a step at 8 kHz are 1.7 s of jitter at
 * 48 kHz, so the estimator must smooth them and must not discard them.
 */
static void wb_transit_probe(uint32_t clock_rate, uint32_t transit,
			     struct rtp_stats *stats,
			     struct rtp_receiver *receiver)
{
	struct udsp_media media = media_make();
	struct rtp_packet parsed;
	uint8_t packet[32];
	uint8_t payload = 0;
	uint32_t step = clock_rate / 50U;
	size_t length;

	rtp_stats_init_cfg(stats, clock_rate);
	rtp_receiver_init_cfg(receiver, &media, clock_rate, stats);
	length = packet_make(packet, 800, 0, 91, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(receiver, packet, length, 0, &parsed) < 0);
	length = packet_make(packet, 801, step, 91, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(receiver, packet, length,
			       wb_arrival_ns(step, clock_rate), &parsed) == 0);
	length = packet_make(packet, 802, 2U * step, 91, 0, 0, 0, &payload, 1);
	CHECK(rtp_packet_parse(receiver, packet, length,
			       wb_arrival_ns(2U * step + transit, clock_rate),
			       &parsed) == 0);
}

static void test_wb_transit_step(void)
{
	struct rtp_receiver receiver;
	struct rtp_stats stats;

	CASE("rtp48_transit_step_at_clock");
	CHECK_UINT(RTP_TRANSIT_STEP_TS(48000U), 480000);
	wb_transit_probe(48000U, RTP_TRANSIT_STEP_TS(48000U), &stats, &receiver);
	CHECK_UINT(stats.ts_steps, 1);
	CHECK_UINT(stats.jitter_samples, 0);
	CHECK_UINT(receiver.jitter_q4, 0);

	wb_transit_probe(48000U, RTP_TRANSIT_STEP_TS(48000U) - 1U, &stats,
			 &receiver);
	CHECK_UINT(stats.ts_steps, 0);
	CHECK_UINT(stats.jitter_samples, 1);
	CHECK_UINT(receiver.jitter_q4, RTP_TRANSIT_STEP_TS(48000U) - 1U);

	/* Two seconds of transit change: jitter at 48 kHz, and past the 8 kHz
	 * bound, so a step test that kept the narrowband constant would
	 * discard this sample instead of smoothing it. */
	CHECK(96000U > RTP_TRANSIT_STEP_TS(RTP_CLOCK_RATE));
	wb_transit_probe(48000U, 96000U, &stats, &receiver);
	CHECK_UINT(stats.ts_steps, 0);
	CHECK_UINT(stats.jitter_samples, 1);
	CHECK_UINT(receiver.jitter_q4, 96000);
}

static int marked_parse(struct rtp_receiver *receiver, uint16_t sequence,
			uint32_t timestamp, uint32_t arrival_ts, bool marker)
{
	struct rtp_packet parsed;
	uint8_t packet[32];
	uint8_t payload = 0;
	size_t length;

	length = packet_make(packet, sequence, timestamp, 93,
			     marker ? 0x80U : 0U, 0, 0, &payload, 1);
	return rtp_packet_parse(receiver, packet, length,
				wb_arrival_ns(arrival_ts, receiver->clock_rate),
				&parsed);
}

/* The probation packet and one on-time packet 20 ms later seed the transit.
 * The probe follows 20 ms after that, ahead_ts past its due timestamp and
 * late_ts after its due arrival, so its transit delta is late_ts - ahead_ts. */
static void marked_probe(uint32_t clock_rate, uint32_t ahead_ts,
			 uint32_t late_ts, bool marker, struct rtp_stats *stats,
			 struct rtp_receiver *receiver)
{
	struct udsp_media media = media_make();
	uint32_t step = clock_rate / 50U;

	rtp_stats_init_cfg(stats, clock_rate);
	rtp_receiver_init_cfg(receiver, &media, clock_rate, stats);
	CHECK(marked_parse(receiver, 900, 0, 0, false) < 0);
	CHECK(marked_parse(receiver, 901, step, step, false) == 0);
	CHECK(marked_parse(receiver, 902, 2U * step + ahead_ts,
			   2U * step + late_ts, marker) == 0);
}

/*
 * The wire shape of a field call. Probation holds the
 * first packet, the second seeds the transit, and the third arrives 20 ms
 * later, marked, with a timestamp 2180 ms on. Its transit delta of -17280
 * ticks is a sender timeline step: no observation, one re-anchor. The next
 * packet measures against it, so the estimate follows the real line from
 * there.
 */
static void test_jitter_marked_forward_step(void)
{
	struct udsp_media media = media_make();
	struct rtp_receiver receiver;
	struct rtp_stats stats;

	CASE("rtp_jitter_skips_a_marked_forward_step");
	rtp_stats_init(&stats);
	rtp_receiver_init(&receiver, &media, &stats);
	CHECK(marked_parse(&receiver, 8064, 160, 160, true) < 0);
	CHECK(marked_parse(&receiver, 8065, 320, 320, false) == 0);
	CHECK(marked_parse(&receiver, 8066, 17760, 480, true) == 0);
	CHECK_UINT(receiver.jitter_q4, 0);
	CHECK_UINT(stats.jitter_samples, 0);
	CHECK_UINT(stats.jitter_max, 0);
	CHECK_UINT(stats.ts_steps, 1);
	CHECK(marked_parse(&receiver, 8067, 17920, 640, false) == 0);
	CHECK_UINT(receiver.jitter_q4, 0);
	CHECK_UINT(stats.jitter_samples, 1);
	CHECK(marked_parse(&receiver, 8068, 18080, 805, false) == 0);
	CHECK_UINT(receiver.jitter_q4, 5);
	CHECK_UINT(stats.jitter_samples, 2);
	CHECK_UINT(stats.ts_steps, 1);
}

/* A skipped marked step keeps a nonzero estimate as it was, and the step
 * packet is the transit reference. The second accepted packet is 5 ms late,
 * 40 ticks, and the step leaves that estimate. The next packet is on time
 * against the step packet: one observation of 0, and the estimate decays to
 * 37. */
static void test_jitter_marked_step_keeps_estimate(void)
{
	struct udsp_media media = media_make();
	struct rtp_receiver receiver;
	struct rtp_stats stats;

	CASE("rtp_jitter_marked_step_keeps_estimate");
	rtp_stats_init(&stats);
	rtp_receiver_init(&receiver, &media, &stats);
	CHECK(marked_parse(&receiver, 900, 0, 0, false) < 0);
	CHECK(marked_parse(&receiver, 901, 160, 160, false) == 0);
	CHECK(marked_parse(&receiver, 902, 320, 360, false) == 0);
	CHECK_UINT(receiver.jitter_q4, 40);
	CHECK(marked_parse(&receiver, 903, 480U + 16000U, 480, true) == 0);
	CHECK_UINT(receiver.jitter_q4, 40);
	CHECK_UINT(stats.jitter_samples, 1);
	CHECK_UINT(stats.ts_steps, 1);
	CHECK(marked_parse(&receiver, 904, 640U + 16000U, 640, false) == 0);
	CHECK_UINT(receiver.jitter_q4, 40U - ((40U + 8U) >> 4));
	CHECK_UINT(stats.jitter_samples, 2);
}

/*
 * The rule is narrow. A marked resume after a real 1 s silence, 5 ms late,
 * stays an observation (+40 ticks). So does a marked packet delayed 600 ms,
 * beyond the 530 ms bound (+4800). So does an unmarked 2 s forward step,
 * below the 10 s bound (-16000).
 */
static void test_jitter_marked_rule_narrow(void)
{
	struct rtp_receiver receiver;
	struct rtp_stats stats;

	CASE("rtp_jitter_counts_a_marked_resume");
	marked_probe(RTP_CLOCK_RATE, 8000U, 8040U, true, &stats, &receiver);
	CHECK_UINT(receiver.jitter_q4, 40);
	CHECK_UINT(stats.jitter_samples, 1);
	CHECK_UINT(stats.ts_steps, 0);

	CASE("rtp_jitter_counts_a_marked_delay");
	marked_probe(RTP_CLOCK_RATE, 0, 4800U, true, &stats, &receiver);
	CHECK_UINT(receiver.jitter_q4, 4800);
	CHECK_UINT(stats.jitter_samples, 1);
	CHECK_UINT(stats.ts_steps, 0);

	CASE("rtp_jitter_counts_an_unmarked_step_below_the_bound");
	marked_probe(RTP_CLOCK_RATE, 16000U, 0, false, &stats, &receiver);
	CHECK_UINT(receiver.jitter_q4, 16000);
	CHECK_UINT(stats.jitter_samples, 1);
	CHECK_UINT(stats.ts_steps, 0);
}

/* A sender that marks every packet, on a line with 30 ms of transit jitter.
 * Every delta is below the bound, so every packet after the seed is an
 * observation, with the exact A.8 value. */
static void test_jitter_marker_every_packet(void)
{
	struct udsp_media media = media_make();
	struct rtp_receiver receiver;
	struct rtp_stats stats;
	uint32_t expected_q4 = 0;
	unsigned i;

	CASE("rtp_jitter_marker_every_packet");
	rtp_stats_init(&stats);
	rtp_receiver_init(&receiver, &media, &stats);
	for (i = 0; i < 20U; i++) {
		uint32_t transit = (i % 2U) ? 240U : 0U;
		int rc = marked_parse(&receiver, (uint16_t)(700U + i), i * 160U,
				      i * 160U + transit, true);

		CHECK(i ? rc == 0 : rc < 0);
		if (i > 1U)
			expected_q4 += 240U - ((expected_q4 + 8U) >> 4);
	}
	CHECK_UINT(receiver.jitter_q4, expected_q4);
	CHECK_UINT(stats.jitter_samples, 18);
	CHECK_UINT(stats.ts_steps, 0);
}

/* The bound is RTP_MARKED_STEP_MS at the stream clock, and it is strict. A
 * marked step of exactly 530 ms is still an observation, and one tick more is
 * a re-anchor. The case runs at 8 kHz and at 48 kHz. */
static void test_jitter_marked_bound_edges(void)
{
	static const uint32_t clocks[2] = { RTP_CLOCK_RATE, 48000U };
	struct rtp_receiver receiver;
	struct rtp_stats stats;
	uint32_t bound;
	unsigned k;

	CASE("rtp_jitter_marked_bound_edges");
	CHECK_UINT(RTP_MARKED_STEP_TS(RTP_CLOCK_RATE), 4240);
	CHECK_UINT(RTP_MARKED_STEP_TS(48000U), 25440);
	for (k = 0; k < 2U; k++) {
		bound = RTP_MARKED_STEP_TS(clocks[k]);
		marked_probe(clocks[k], bound, 0, true, &stats, &receiver);
		CHECK_UINT(receiver.jitter_q4, bound);
		CHECK_UINT(stats.jitter_samples, 1);
		CHECK_UINT(stats.ts_steps, 0);
		marked_probe(clocks[k], bound + 1U, 0, true, &stats, &receiver);
		CHECK_UINT(receiver.jitter_q4, 0);
		CHECK_UINT(stats.jitter_samples, 0);
		CHECK_UINT(stats.ts_steps, 1);
	}
}

/* --- RFC 4733 2.5.1.3 long events ---------------------------------------- */

#define WB_TELEV_MAX	200U
/* stream.h UDSP_TELEV_REPEAT, which this suite cannot include. */
#define WB_TELEV_REPEAT	3U

struct wb_telev_run {
	uint8_t packet[WB_TELEV_MAX][RTP_HEADER_SIZE + 4U];
	unsigned count;
	unsigned segments;
	unsigned markers;
	unsigned closes;		/* the field at its maximum, no E */
	unsigned ends;			/* the E bit set */
	uint16_t duration_max;
	uint32_t segment_ts[8];
};

static uint16_t wb_telev_duration(const uint8_t *packet)
{
	return (uint16_t)(((uint16_t)packet[14] << 8) | packet[15]);
}

static void wb_telev_emit(struct wb_telev_run *run, struct telev_sender *sender)
{
	uint16_t duration;
	size_t length;

	while (sender->active && run->count < WB_TELEV_MAX) {
		if (telev_sender_packet_build(sender, run->packet[run->count],
					      sizeof(run->packet[0]), &length))
			return;
		if (run->packet[run->count][1] & 0x80U)
			run->markers++;
		if (run->packet[run->count][13] & 0x80U)
			run->ends++;
		duration = wb_telev_duration(run->packet[run->count]);
		if (duration == TELEV_DURATION_MAX &&
		    !(run->packet[run->count][13] & 0x80U))
			run->closes++;
		if (duration > run->duration_max)
			run->duration_max = duration;
		run->count++;
		if (!sender->ending)
			return;
	}
}

/*
 * The transmit ladder stream_tx_telev runs, at the stream clock: one packet
 * per ptime with the duration counted from the current segment's start, not
 * from the event onset, and a closing packet the moment that duration passes
 * the field. The closing packet reports the field's maximum with the E bit
 * clear (RFC 4733 2.5.1.3), and the next segment starts at its end.
 */
static void wb_telev_send(struct wb_telev_run *run, struct telev_sender *sender,
			  uint32_t clock_rate, uint32_t ptime_ms,
			  uint32_t event_ms)
{
	uint32_t ts_per_ms = clock_rate / 1000U;
	uint32_t elapsed_ms;
	uint32_t seg_ts = 0;
	uint32_t duration;

	run->segment_ts[0] = sender->timestamp;
	run->segments = 1;
	for (elapsed_ms = ptime_ms; elapsed_ms <= event_ms;
	     elapsed_ms += ptime_ms) {
		duration = elapsed_ms * ts_per_ms - seg_ts;
		if (duration > TELEV_DURATION_MAX) {
			unsigned copy;

			CHECK(telev_sender_update(sender,
						  TELEV_DURATION_MAX) == 0);
			/* stream_telev_segment: the close is retransmitted
			 * like the last packet of the event. */
			for (copy = 0; copy < WB_TELEV_REPEAT; copy++)
				wb_telev_emit(run, sender);
			CHECK(telev_sender_segment(sender) == 0);
			seg_ts += TELEV_DURATION_MAX;
			if (run->segments < 8U)
				run->segment_ts[run->segments] =
					sender->timestamp;
			run->segments++;
			duration = elapsed_ms * ts_per_ms - seg_ts;
		}
		if (elapsed_ms < event_ms) {
			CHECK(telev_sender_update(sender,
						  (uint16_t)duration) == 0);
			wb_telev_emit(run, sender);
			continue;
		}
		CHECK(telev_sender_end(sender, (uint16_t)duration) == 0);
		wb_telev_emit(run, sender);
	}
}

/*
 * The wire walk: every rule of RFC 4733 2.5.1.3 and 2.5.1.4 that one pass over
 * the packets can decide.
 *
 *   the event code, the volume and the reserved bit hold for the whole event;
 *   the marker marks the onset and nothing else;
 *   the sequence number advances by one on every packet, copies included;
 *   the timestamp is constant inside a segment and moves by exactly the field
 *     maximum at a boundary, which only a closing packet may open;
 *   a duration either advances by one packet step or repeats, and a repeat is
 *     a retransmission of a closing or a final packet;
 *   a closing packet and a final packet are each sent three times.
 */
static void wb_telev_chain_check(const struct wb_telev_run *run, uint8_t code,
				 uint8_t volume, uint32_t step_ts)
{
	uint32_t seg_ts = uint32_test_read(run->packet[0] + 4);
	uint32_t prev_duration = 0;
	uint16_t prev_seq = 0;
	unsigned copies = 0;
	unsigned closes = 0;
	unsigned ends = 0;
	bool prev_repeatable = false;
	unsigned i;

	for (i = 0; i < run->count; i++) {
		const uint8_t *packet = run->packet[i];
		uint32_t ts = uint32_test_read(packet + 4);
		uint16_t seq = (uint16_t)(((uint16_t)packet[2] << 8) | packet[3]);
		uint16_t duration = wb_telev_duration(packet);
		bool ending = (packet[13] & 0x80U) != 0;
		bool full = duration == TELEV_DURATION_MAX;

		CHECK_UINT(packet[12], code);
		CHECK_UINT(packet[13] & 0x3fU, volume);
		CHECK_UINT(packet[13] & 0x40U, 0);
		CHECK_UINT((packet[1] & 0x80U) != 0, i == 0);
		if (i)
			CHECK_UINT(seq, (uint16_t)(prev_seq + 1U));
		prev_seq = seq;
		if (ts != seg_ts) {
			/* Only a closed segment moves the timestamp, and only
			 * once its three copies are on the wire. */
			CHECK_UINT(ts, seg_ts + TELEV_DURATION_MAX);
			CHECK_UINT(copies, WB_TELEV_REPEAT);
			seg_ts = ts;
			prev_duration = 0;
			copies = 0;
			prev_repeatable = false;
			CHECK(duration > 0 && !full && !ending);
		} else if (duration == prev_duration && i) {
			/* A repeat, which only an ending packet may be. */
			CHECK(prev_repeatable);
			copies++;
		} else if (full) {
			CHECK(!ending);
			copies = 1;
		} else {
			CHECK_UINT(duration, prev_duration + step_ts);
			copies = 1;
		}
		if (full && !ending)
			closes++;
		if (ending)
			ends++;
		prev_repeatable = full || ending;
		prev_duration = duration;
	}
	/* The event ends on three copies of its final packet. */
	CHECK_UINT(copies, WB_TELEV_REPEAT);
	CHECK_UINT(closes, run->closes);
	CHECK_UINT(ends, run->ends);
	CHECK_UINT(closes, (run->segments - 1U) * WB_TELEV_REPEAT);
}

static int wb_telev_receive(struct telev_receiver *receiver,
			    const struct wb_telev_run *run,
			    struct telev_event *event)
{
	struct rtp_packet packet;
	int reports = 0;
	unsigned i;

	for (i = 0; i < run->count; i++) {
		memset(&packet, 0, sizeof(packet));
		packet.kind = RTP_PAYLOAD_TELEPHONE_EVENT;
		packet.payload = run->packet[i] + RTP_HEADER_SIZE;
		packet.payload_len = 4U;
		packet.timestamp = uint32_test_read(run->packet[i] + 4);
		packet.arrival_ns = (uint64_t)i * 20000000ULL;
		if (telev_packet_parse(receiver, &packet, event) == 1)
			reports++;
	}
	return reports;
}

static void test_wb_telev_segments(void)
{
	struct udsp_media media = media_make();
	struct rtp_entropy entropy = entropy_make(9);
	struct telev_receiver receiver;
	struct telev_sender sender;
	struct wb_telev_run run;
	struct telev_event event;
	struct rtp_stats stats;
	struct rtp_sender rtp;

	CASE("telev48_long_event_segments_send");
	memset(&run, 0, sizeof(run));
	memset(&event, 0, sizeof(event));
	rtp_stats_init_cfg(&stats, 48000U);
	CHECK(rtp_sender_init(&rtp, &media, &entropy, &stats) == 0);
	rtp.sequence = 700;
	rtp.timestamp = 240000U;
	telev_sender_init(&sender, &rtp);
	CHECK(telev_sender_start(&sender, '7', 10) == 0);
	wb_telev_send(&run, &sender, 48000U, 20U, 3000U);
	/* A segment carries 65535 ticks, which is 1365.3 ms at 48 kHz, so a 3 s
	 * event is three segments and two boundaries, each closed three times
	 * (RFC 4733 2.5.1.4). */
	CHECK_UINT(run.segments, 3);
	CHECK_UINT(run.closes, 2U * WB_TELEV_REPEAT);
	CHECK_UINT(run.count, 158);
	CHECK_UINT(run.duration_max, TELEV_DURATION_MAX);
	CHECK_UINT(run.segment_ts[0], 240000);
	CHECK_UINT(run.segment_ts[1], 240000U + TELEV_DURATION_MAX);
	CHECK_UINT(run.segment_ts[2], 240000U + 2U * TELEV_DURATION_MAX);
	/* Only the last packet of the last segment ends the event, three times
	 * (RFC 4733 2.5.1.4). */
	CHECK_UINT(run.ends, WB_TELEV_REPEAT);
	/* Only the first packet of the whole event is a new event onset, not the
	 * first packet of a segment. A segment that carried the marker would
	 * report as a second digit. */
	CHECK_UINT(run.markers, 1);
	wb_telev_chain_check(&run, 7U, 10U, 960U);
	CHECK_UINT(rtp.timestamp, 240000U + 3000U * 48U);
	CHECK(!sender.active);

	CASE("telev48_long_event_segments_receive");
	telev_receiver_init_cfg(&receiver, 48000U);
	CHECK_UINT(wb_telev_receive(&receiver, &run, &event), 1);
	CHECK_UINT(event.digit, '7');
	CHECK_UINT(event.duration_ms, 3000);
	CHECK(!receiver.seg_valid);
}

static int wb_telev_feed(struct telev_receiver *receiver,
			 struct telev_event *event, uint8_t code, uint32_t ts,
			 uint16_t duration, bool ending, uint64_t arrival_ns)
{
	struct rtp_packet packet;
	uint8_t payload[4];

	payload[0] = code;
	payload[1] = ending ? 0x8aU : 0x0aU;
	payload[2] = (uint8_t)(duration >> 8);
	payload[3] = (uint8_t)duration;
	memset(&packet, 0, sizeof(packet));
	packet.kind = RTP_PAYLOAD_TELEPHONE_EVENT;
	packet.payload = payload;
	packet.payload_len = sizeof(payload);
	packet.timestamp = ts;
	packet.arrival_ns = arrival_ns;
	return telev_packet_parse(receiver, &packet, event);
}

static int wb_audio_feed(struct telev_receiver *receiver,
			 struct telev_event *event, uint64_t arrival_ns)
{
	struct rtp_packet packet;

	memset(&packet, 0, sizeof(packet));
	packet.kind = RTP_PAYLOAD_AUDIO;
	packet.arrival_ns = arrival_ns;
	return telev_packet_parse(receiver, &packet, event);
}

/*
 * The receive discriminator of RFC 4733 2.5.1.3, packet by packet. The
 * duration field at its maximum with E clear closes a segment and reports
 * nothing. A value below the maximum is an ordinary interim packet. The E bit
 * reports the whole chain.
 */
static void test_wb_telev_segment_chain(void)
{
	struct telev_receiver receiver;
	struct telev_event event;
	uint32_t ts = 700000U;

	CASE("telev48_segment_close_holds_chain");
	telev_receiver_init_cfg(&receiver, 48000U);
	memset(&event, 0, sizeof(event));
	CHECK_UINT(wb_telev_feed(&receiver, &event, 7, ts, 960U, false, 0), 0);
	/* One tick below the field is an ordinary interim packet. */
	CHECK_UINT(wb_telev_feed(&receiver, &event, 7, ts,
				 TELEV_DURATION_MAX - 1U, false, 0), 0);
	CHECK(!receiver.seg_valid);
	CHECK_UINT(wb_telev_feed(&receiver, &event, 7, ts, TELEV_DURATION_MAX,
				 false, 0), 0);
	CHECK(receiver.seg_valid);
	CHECK_UINT(receiver.seg_total, TELEV_DURATION_MAX);
	CHECK_UINT(receiver.seg_end_ts, ts + TELEV_DURATION_MAX);
	CHECK(!receiver.active_valid);
	/* The other two copies of the close (RFC 4733 2.5.1.4) are
	 * retransmissions and extend the chain by nothing. */
	CHECK_UINT(wb_telev_feed(&receiver, &event, 7, ts, TELEV_DURATION_MAX,
				 false, 0), 0);
	CHECK_UINT(wb_telev_feed(&receiver, &event, 7, ts, TELEV_DURATION_MAX,
				 false, 0), 0);
	CHECK_UINT(receiver.seg_total, TELEV_DURATION_MAX);
	CHECK_UINT(receiver.seg_end_ts, ts + TELEV_DURATION_MAX);

	/* The continuation starts at the closed segment's end. */
	CHECK_UINT(wb_telev_feed(&receiver, &event, 7, ts + TELEV_DURATION_MAX,
				 960U, false, 0), 0);
	CHECK(receiver.seg_valid);
	CHECK_UINT(wb_telev_feed(&receiver, &event, 7, ts + TELEV_DURATION_MAX,
				 1920U, true, 0), 1);
	CHECK_UINT(event.digit, '7');
	/* 65535 + 1920 ticks at 48 kHz. */
	CHECK_UINT(event.duration_ms, 1405);
	CHECK(!receiver.seg_valid);
	/* RFC 4733 2.5.1.4 repeats the final packet. */
	CHECK_UINT(wb_telev_feed(&receiver, &event, 7, ts + TELEV_DURATION_MAX,
				 1920U, true, 0), 0);

	CASE("telev48_segment_retransmit_no_duplicate");
	/* A closing packet delayed across the short final segment arrives after
	 * the digit is reported. It must not open a second chain, and no later
	 * packet may turn it into a second digit. */
	CHECK_UINT(wb_telev_feed(&receiver, &event, 7, ts, TELEV_DURATION_MAX,
				 false, 0), 0);
	CHECK(!receiver.seg_valid);
	CHECK_UINT(wb_audio_feed(&receiver, &event, 10000000000ULL), 0);
	CHECK_UINT(wb_audio_feed(&receiver, &event, 20000000000ULL), 0);
	CHECK(!receiver.seg_valid);
}

/*
 * A chain whose continuation never arrives, in the two shapes that can lose
 * it. In one, nothing at all follows the closing packet. In the other, the
 * final packets of the continuation are lost. Both report the digit once, on
 * the timeout.
 */
static void test_wb_telev_segment_lost(void)
{
	struct telev_receiver receiver;
	struct telev_event event;
	uint32_t ts = 900000U;

	CASE("telev48_segment_orphan_flushes");
	telev_receiver_init_cfg(&receiver, 48000U);
	memset(&event, 0, sizeof(event));
	CHECK_UINT(wb_telev_feed(&receiver, &event, 3, ts, 960U, false, 0), 0);
	CHECK_UINT(wb_telev_feed(&receiver, &event, 3, ts, TELEV_DURATION_MAX,
				 false, 0), 0);
	CHECK(receiver.seg_valid);
	/* The bound is twice the segment, which is 2.73 s at 48 kHz. */
	CHECK_UINT(wb_audio_feed(&receiver, &event, 1000000000ULL), 0);
	CHECK_UINT(wb_audio_feed(&receiver, &event, 3000000000ULL), 1);
	CHECK_UINT(event.digit, '3');
	CHECK_UINT(event.duration_ms, 1365);
	CHECK(!receiver.seg_valid);
	CHECK_UINT(wb_audio_feed(&receiver, &event, 9000000000ULL), 0);

	CASE("telev48_flushed_chain_suppresses_continuation");
	/*
	 * The four steps that can report the digit twice: the chain is held,
	 * the timeout reports it, the far end's continuation arrives late, and
	 * its final packet ends an event the caller already has. The end
	 * timestamp of that packet is one no ring entry could have predicted,
	 * so the flush leaves the chain's identity behind instead.
	 */
	telev_receiver_init_cfg(&receiver, 48000U);
	memset(&event, 0, sizeof(event));
	ts = 300000U;
	CHECK_UINT(wb_telev_feed(&receiver, &event, 4, ts, 960U, false, 0), 0);
	CHECK_UINT(wb_telev_feed(&receiver, &event, 4, ts, TELEV_DURATION_MAX,
				 false, 0), 0);
	CHECK(receiver.seg_valid);
	CHECK_UINT(wb_audio_feed(&receiver, &event, 3000000000ULL), 1);
	CHECK_UINT(event.digit, '4');
	CHECK_UINT(event.duration_ms, 1365);
	CHECK(!receiver.seg_valid);
	CHECK_UINT(wb_telev_feed(&receiver, &event, 4, ts + TELEV_DURATION_MAX,
				 960U, false, 3100000000ULL), 0);
	CHECK_UINT(wb_telev_feed(&receiver, &event, 4, ts + TELEV_DURATION_MAX,
				 1920U, true, 3200000000ULL), 0);
	CHECK_UINT(wb_audio_feed(&receiver, &event, 9000000000ULL), 0);

	CASE("telev48_flushed_chain_follows_late_close");
	/* The late continuation may close a segment of its own; the digit is
	 * still the one the caller has. */
	CHECK_UINT(wb_telev_feed(&receiver, &event, 4, ts + TELEV_DURATION_MAX,
				 TELEV_DURATION_MAX, false, 3300000000ULL), 0);
	CHECK_UINT(wb_telev_feed(&receiver, &event, 4,
				 ts + 2U * TELEV_DURATION_MAX, 1920U, true,
				 3400000000ULL), 0);
	CHECK_UINT(wb_audio_feed(&receiver, &event, 9000000000ULL), 0);
	CHECK(!receiver.seg_valid);
	/* A different event is not the reported chain and reports normally. */
	CHECK_UINT(wb_telev_feed(&receiver, &event, 9, 900000U, 960U, true,
				 3500000000ULL), 1);
	CHECK_UINT(event.digit, '9');

	CASE("telev48_flushed_keys_interleave");
	/*
	 * Two events whose chains overlap: the first reports on its timeout and
	 * its continuation then streams in while the second's chain is still
	 * waiting for one. One suppression identity cannot hold both, and
	 * whichever is evicted reports its digit a second time, so the
	 * identities live in a ring of their own.
	 */
	telev_receiver_init_cfg(&receiver, 48000U);
	memset(&event, 0, sizeof(event));
	CHECK_UINT(wb_telev_feed(&receiver, &event, 1, 100000U,
				 TELEV_DURATION_MAX, false, 0), 0);
	CHECK_UINT(wb_audio_feed(&receiver, &event, 2800000000ULL), 1);
	CHECK_UINT(event.digit, '1');
	/* The second chain opens while the first identity stands. A single
	 * suppression identity would evict the first identity here. */
	CHECK_UINT(wb_telev_feed(&receiver, &event, 2, 400000U,
				 TELEV_DURATION_MAX, false, 2850000000ULL), 0);
	CHECK(receiver.seg_valid);
	/* The first event's continuation streams in across the second chain's
	 * whole wait, and every packet of it is the event the caller has. */
	CHECK_UINT(wb_telev_feed(&receiver, &event, 1,
				 100000U + TELEV_DURATION_MAX, 960U, false,
				 3000000000ULL), 0);
	CHECK_UINT(wb_telev_feed(&receiver, &event, 1,
				 100000U + TELEV_DURATION_MAX, 1920U, false,
				 4000000000ULL), 0);
	CHECK_UINT(wb_telev_feed(&receiver, &event, 1,
				 100000U + TELEV_DURATION_MAX, 2880U, false,
				 5000000000ULL), 0);
	CHECK_UINT(wb_telev_feed(&receiver, &event, 1,
				 100000U + TELEV_DURATION_MAX, 3840U, false,
				 5600000000ULL), 0);
	/* The second chain reports on its own timeout and takes an identity of
	 * its own, beside the first one and not over it. */
	CHECK_UINT(wb_audio_feed(&receiver, &event, 5700000000ULL), 1);
	CHECK_UINT(event.digit, '2');
	CHECK_UINT(wb_telev_feed(&receiver, &event, 1,
				 100000U + TELEV_DURATION_MAX, 4800U, true,
				 5750000000ULL), 0);
	CHECK_UINT(wb_telev_feed(&receiver, &event, 2,
				 400000U + TELEV_DURATION_MAX, 960U, false,
				 5800000000ULL), 0);
	CHECK_UINT(wb_telev_feed(&receiver, &event, 2,
				 400000U + TELEV_DURATION_MAX, 1920U, true,
				 5850000000ULL), 0);
	/* A fresh event is never suppressed, and neither is one that repeats a
	 * reported event's code on a timeline of its own. */
	CHECK_UINT(wb_telev_feed(&receiver, &event, 3, 800000U, 960U, true,
				 5900000000ULL), 1);
	CHECK_UINT(event.digit, '3');
	CHECK_UINT(wb_telev_feed(&receiver, &event, 1, 900000U, 960U, true,
				 5950000000ULL), 1);
	CHECK_UINT(event.digit, '1');

	CASE("telev48_flushed_key_expires");
	/* The identity claims a continuation for as long as the chain waited
	 * for one and not past it: a packet past that window is a new event by
	 * every test the wire offers, and the key set cannot decide forever. */
	telev_receiver_init_cfg(&receiver, 48000U);
	memset(&event, 0, sizeof(event));
	CHECK_UINT(wb_telev_feed(&receiver, &event, 8, 200000U,
				 TELEV_DURATION_MAX, false, 0), 0);
	CHECK_UINT(wb_audio_feed(&receiver, &event, 2800000000ULL), 1);
	CHECK_UINT(event.digit, '8');
	CHECK_UINT(wb_telev_feed(&receiver, &event, 8,
				 200000U + TELEV_DURATION_MAX, 960U, true,
				 9000000000ULL), 1);
	CHECK_UINT(event.digit, '8');

	CASE("telev48_lost_final_segment_folds_chain");
	telev_receiver_init_cfg(&receiver, 48000U);
	memset(&event, 0, sizeof(event));
	/* An event that runs before the chain, so the active record carries a
	 * duration of its own that the continuation must not inherit. */
	CHECK_UINT(wb_telev_feed(&receiver, &event, 5, 100000U, 30000U, false,
				 0), 0);
	CHECK_UINT(wb_telev_feed(&receiver, &event, 5, 100000U, 30000U, true,
				 0), 1);
	ts = 500000U;
	CHECK_UINT(wb_telev_feed(&receiver, &event, 6, ts, TELEV_DURATION_MAX,
				 false, 0), 0);
	CHECK_UINT(wb_telev_feed(&receiver, &event, 6, ts + TELEV_DURATION_MAX,
				 960U, false, 1000000ULL), 0);
	CHECK_UINT(receiver.active_duration, 960);
	/* Every final packet of the continuation is lost. The active event
	 * times out at twice its own duration, 40 ms, and folds the chain, so
	 * the digit reports once with the whole event. */
	CHECK_UINT(wb_audio_feed(&receiver, &event, 20000000ULL), 0);
	CHECK_UINT(wb_audio_feed(&receiver, &event, 60000000ULL), 1);
	CHECK_UINT(event.digit, '6');
	CHECK_UINT(event.duration_ms, 1385);
	CHECK(!receiver.seg_valid);
	CHECK_UINT(wb_audio_feed(&receiver, &event, 9000000000ULL), 0);
}


/*
 * ---------------------------------------------------------------------------
 * More than one RTP packet in one lower-layer PDU (RFC 3550 11).
 * ---------------------------------------------------------------------------
 * The vectors come from a field capture on an AN7581 line: 344 bytes of
 * UDP payload holding two 20 ms packets, the second header at offset 172 with
 * the successor sequence number, the next timestamp and the same SSRC.
 */
#define JB1_SSRC		0xd476114dU
#define JB1_SEQ			14258U
#define JB1_TS			294720U
#define JB1_FRAME		160U
#define JB1_SPLIT		(RTP_HEADER_SIZE + JB1_FRAME)

/* Walks a receiver into RFC 3550 A.1 probation, which holds the first packet
 * of every source until its successor. These cases never take that packet. */
static void jb1_receiver_warm(struct rtp_receiver *receiver,
			      struct rtp_stats *stats, struct udsp_media *media)
{
	struct rtp_packet parsed;
	uint8_t packet[64];
	uint8_t payload = 0;
	size_t len;

	rtp_stats_init(stats);
	rtp_receiver_init(receiver, media, stats);
	len = packet_make(packet, JB1_SEQ - 1U, JB1_TS - JB1_FRAME, JB1_SSRC, 0,
			  0, 0, &payload, 1);
	rtp_packet_parse(receiver, packet, len, 0, &parsed);
}

static void jb1_payload_fill(uint8_t *payload, size_t len, uint8_t seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		payload[i] = (uint8_t)(seed + i);
}

/*
 * The field shape. Read as one packet, the datagram gives a 332-byte payload.
 * The buffer slices that payload into halves that no codec accepts. The audio
 * of the first packet is then concealed away, as well as the second. Split at
 * the header of the successor, both packets come out whole.
 */
static void test_coalesced_pdu_field_shape(void)
{
	struct udsp_media media = media_make();
	struct rtp_receiver receiver;
	struct rtp_stats stats;
	struct rtp_packet parsed;
	uint8_t payload[JB1_FRAME];
	uint8_t pdu[512];
	const uint8_t *tail;
	size_t tail_len;
	size_t len;

	CASE("rtp_coalesced_pdu_field_shape");
	jb1_receiver_warm(&receiver, &stats, &media);
	jb1_payload_fill(payload, sizeof(payload), 0xa0U);
	len = packet_make(pdu, JB1_SEQ, JB1_TS, JB1_SSRC, 0, 0, 0, payload,
			  sizeof(payload));
	len += packet_make(pdu + len, JB1_SEQ + 1U, JB1_TS + JB1_FRAME,
			   JB1_SSRC, 0, 0, 0, payload, sizeof(payload));
	CHECK_UINT(len, 344U);
	CHECK_UINT(JB1_SPLIT, 172U);

	memset(&parsed, 0, sizeof(parsed));
	CHECK_UINT(rtp_packet_parse(&receiver, pdu, len, 0, &parsed), 0);
	CHECK_UINT(parsed.sequence, JB1_SEQ);
	CHECK_UINT(parsed.timestamp, JB1_TS);
	CHECK_UINT(parsed.payload_len, JB1_FRAME);
	CHECK(parsed.payload == pdu + RTP_HEADER_SIZE);
	CHECK(parsed.tail == pdu + JB1_SPLIT);
	CHECK_UINT(parsed.tail_len, len - JB1_SPLIT);
	tail = parsed.tail;
	tail_len = parsed.tail_len;
	if (!tail)
		return;

	CHECK_UINT(rtp_packet_parse(&receiver, tail, tail_len, 0, &parsed), 0);
	CHECK_UINT(parsed.sequence, JB1_SEQ + 1U);
	CHECK_UINT(parsed.timestamp, JB1_TS + JB1_FRAME);
	CHECK_UINT(parsed.payload_len, JB1_FRAME);
	CHECK(parsed.tail == NULL);
	CHECK_UINT(parsed.tail_len, 0U);

	/* Two receptions of their own bytes, not two of the whole datagram. */
	CHECK_UINT(stats.packets_recv, 2U);
	CHECK_UINT(stats.bytes_recv, 344U);
}

/*
 * A payload that happens to look like a header must not split an ordinary
 * datagram. Two decoys cover the two exact fields that the guard tests beyond
 * the version. One decoy fails on the sequence number, and one on the SSRC.
 */
static void test_single_packet_keeps_whole_payload(void)
{
	struct udsp_media media = media_make();
	struct rtp_receiver receiver;
	struct rtp_stats stats;
	struct rtp_packet parsed;
	uint8_t payload[JB1_FRAME];
	uint8_t pdu[512];
	size_t len;

	CASE("rtp_single_packet_keeps_whole_payload");
	jb1_receiver_warm(&receiver, &stats, &media);
	jb1_payload_fill(payload, sizeof(payload), 0x11U);
	/* right version, right SSRC, wrong sequence (a skip, not a successor) */
	packet_make(payload + 20, JB1_SEQ + 2U, JB1_TS, JB1_SSRC, 0, 0, 0,
		    NULL, 0);
	/* right version, right sequence, wrong SSRC */
	packet_make(payload + 60, JB1_SEQ + 1U, JB1_TS, JB1_SSRC ^ 1U, 0, 0, 0,
		    NULL, 0);
	/* right sequence, right SSRC, wrong version: RFC 3550 5.1 puts the
	 * version in the top two bits, and this stack speaks 2 only */
	packet_make(payload + 100, JB1_SEQ + 1U, JB1_TS, JB1_SSRC, 0, 0, 0,
		    NULL, 0);
	payload[100] = 0x40U;
	len = packet_make(pdu, JB1_SEQ, JB1_TS, JB1_SSRC, 0, 0, 0, payload,
			  sizeof(payload));

	CHECK_UINT(rtp_packet_parse(&receiver, pdu, len, 0, &parsed), 0);
	CHECK_UINT(parsed.payload_len, JB1_FRAME);
	CHECK(parsed.tail == NULL);
	CHECK_UINT(parsed.tail_len, 0U);
	CHECK_UINT(stats.bytes_recv, len);
}

/*
 * The padding count byte belongs to the packet that set the P bit. It
 * therefore sits at the end of that packet, not at the end of the PDU. A
 * parser that reads the count from the last byte of the PDU strips the wrong
 * count from the first packet. In this case that byte is zero, an invalid
 * count under RFC 3550 5.1, so such a parser discards the whole datagram.
 */
static void test_coalesced_pdu_padding_per_packet(void)
{
	struct udsp_media media = media_make();
	struct rtp_receiver receiver;
	struct rtp_stats stats;
	struct rtp_packet parsed;
	uint8_t first[JB1_FRAME];
	uint8_t second[JB1_FRAME];
	uint8_t pdu[512];
	const uint8_t *tail;
	size_t tail_len;
	size_t len;

	CASE("rtp_coalesced_pdu_padding_per_packet");
	jb1_receiver_warm(&receiver, &stats, &media);
	jb1_payload_fill(first, sizeof(first), 0x30U);
	jb1_payload_fill(second, sizeof(second), 0x70U);
	first[JB1_FRAME - 1U] = 4U;		/* the first packet's own count */
	second[JB1_FRAME - 1U] = 0U;		/* not a count, and never read */
	len = packet_make(pdu, JB1_SEQ, JB1_TS, JB1_SSRC, 0, 0x20U, 0, first,
			  sizeof(first));
	len += packet_make(pdu + len, JB1_SEQ + 1U, JB1_TS + JB1_FRAME,
			   JB1_SSRC, 0, 0, 0, second, sizeof(second));

	memset(&parsed, 0, sizeof(parsed));
	CHECK_UINT(rtp_packet_parse(&receiver, pdu, len, 0, &parsed), 0);
	CHECK_UINT(parsed.payload_len, JB1_FRAME - 4U);
	CHECK(parsed.tail == pdu + JB1_SPLIT);
	tail = parsed.tail;
	tail_len = parsed.tail_len;
	if (!tail)
		return;
	CHECK_UINT(rtp_packet_parse(&receiver, tail, tail_len, 0, &parsed), 0);
	CHECK_UINT(parsed.payload_len, JB1_FRAME);
}

/* A UDP receive offload merge is not limited to two segments, so the walk
 * must keep going. RTP_PDU_PACKETS_MAX stops it. */
static void test_coalesced_pdu_three_packets(void)
{
	struct udsp_media media = media_make();
	struct rtp_receiver receiver;
	struct rtp_stats stats;
	struct rtp_packet parsed;
	uint8_t payload[JB1_FRAME];
	uint8_t pdu[3U * JB1_SPLIT];
	const uint8_t *at;
	size_t left;
	size_t len = 0;
	unsigned seen = 0;
	unsigned i;

	CASE("rtp_coalesced_pdu_three_packets");
	jb1_receiver_warm(&receiver, &stats, &media);
	jb1_payload_fill(payload, sizeof(payload), 0x55U);
	for (i = 0; i < 3U; i++)
		len += packet_make(pdu + len, (uint16_t)(JB1_SEQ + i),
				   JB1_TS + i * JB1_FRAME, JB1_SSRC, 0, 0, 0,
				   payload, sizeof(payload));
	CHECK_UINT(len, 3U * JB1_SPLIT);

	at = pdu;
	left = len;
	for (i = 0; i < RTP_PDU_PACKETS_MAX && left; i++) {
		if (rtp_packet_parse(&receiver, at, left, 0, &parsed))
			break;
		CHECK_UINT(parsed.sequence, JB1_SEQ + seen);
		CHECK_UINT(parsed.payload_len, JB1_FRAME);
		seen++;
		at = parsed.tail;
		left = parsed.tail_len;
	}
	CHECK_UINT(seen, 3U);
	CHECK_UINT(left, 0U);
	CHECK_UINT(stats.packets_recv, 3U);
}

/*
 * A packet with no payload is legal: RFC 3550 5.1 puts no floor on the
 * payload. Two such packets in one PDU must still come apart. The second
 * header starts at the first payload byte of the first packet, which is where
 * the scan starts.
 */
static void test_coalesced_pdu_empty_payloads(void)
{
	struct udsp_media media = media_make();
	struct rtp_receiver receiver;
	struct rtp_stats stats;
	struct rtp_packet parsed;
	uint8_t pdu[64];
	const uint8_t *tail;
	size_t tail_len;
	size_t len;

	CASE("rtp_coalesced_pdu_empty_payloads");
	jb1_receiver_warm(&receiver, &stats, &media);
	len = packet_make(pdu, JB1_SEQ, JB1_TS, JB1_SSRC, 0, 0, 0, NULL, 0);
	len += packet_make(pdu + len, JB1_SEQ + 1U, JB1_TS + JB1_FRAME,
			   JB1_SSRC, 0, 0, 0, NULL, 0);
	CHECK_UINT(len, 2U * RTP_HEADER_SIZE);

	memset(&parsed, 0, sizeof(parsed));
	CHECK_UINT(rtp_packet_parse(&receiver, pdu, len, 0, &parsed), 0);
	CHECK_UINT(parsed.sequence, JB1_SEQ);
	CHECK_UINT(parsed.payload_len, 0U);
	CHECK(parsed.tail == pdu + RTP_HEADER_SIZE);
	tail = parsed.tail;
	tail_len = parsed.tail_len;
	if (!tail)
		return;
	CHECK_UINT(rtp_packet_parse(&receiver, tail, tail_len, 0, &parsed), 0);
	CHECK_UINT(parsed.sequence, JB1_SEQ + 1U);
	CHECK_UINT(parsed.payload_len, 0U);
	CHECK(parsed.tail == NULL);
	CHECK_UINT(stats.packets_recv, 2U);
}

/*
 * The walk stops at RTP_PDU_PACKETS_MAX, so a datagram with more packets than
 * that leaves a remainder. Those packets arrived and were dropped at this
 * receiver, so packets_discarded must count them. Without the count, they
 * would come back later as cumulative_lost against the far end, which never
 * lost them.
 */
static void test_pdu_residual_discard_counts(void)
{
	struct udsp_media media = media_make();
	struct rtp_receiver receiver;
	struct rtp_stats stats;
	struct rtp_packet parsed;
	uint8_t payload[JB1_FRAME];
	uint8_t pdu[10U * JB1_SPLIT];
	uint64_t before;
	const uint8_t *at;
	size_t left;
	size_t len = 0;
	unsigned walked = 0;
	unsigned i;

	CASE("rtp_pdu_residual_discard_counts");
	jb1_receiver_warm(&receiver, &stats, &media);
	jb1_payload_fill(payload, sizeof(payload), 0x22U);
	for (i = 0; i < 10U; i++)
		len += packet_make(pdu + len, (uint16_t)(JB1_SEQ + i),
				   JB1_TS + i * JB1_FRAME, JB1_SSRC, 0, 0, 0,
				   payload, sizeof(payload));

	at = pdu;
	left = len;
	for (i = 0; i < RTP_PDU_PACKETS_MAX && left; i++) {
		if (rtp_packet_parse(&receiver, at, left, 0, &parsed))
			break;
		walked++;
		at = parsed.tail;
		left = parsed.tail_len;
	}
	CHECK_UINT(walked, RTP_PDU_PACKETS_MAX);
	CHECK_UINT(left, 2U * JB1_SPLIT);

	before = stats.packets_discarded;
	rtp_receiver_pdu_residual_discard(&receiver, at, left);
	CHECK_UINT(stats.packets_discarded - before, 2U);

	/* Nothing to refuse, nothing to count. */
	before = stats.packets_discarded;
	rtp_receiver_pdu_residual_discard(&receiver, NULL, 0);
	rtp_receiver_pdu_residual_discard(&receiver, pdu, RTP_HEADER_SIZE - 1U);
	CHECK_UINT(stats.packets_discarded - before, 0U);
}

/*
 * The upstream misreport. If the parser never sees a packet, the RFC 3550 A.3
 * accounting never receives its sequence number. The coalesced half of the
 * PDU is then reported to the far end and to the CDR as network loss that
 * never happened.
 */
static void test_coalesced_pdu_no_phantom_loss(void)
{
	struct udsp_media media = media_make();
	struct rtp_receiver receiver;
	struct rtp_stats stats;
	struct rtp_packet parsed;
	struct rtcp_report report;
	uint8_t payload[JB1_FRAME];
	uint8_t pdu[512];
	const uint8_t *at;
	size_t left;
	size_t len;
	unsigned i;

	CASE("rtp_coalesced_pdu_no_phantom_loss");
	jb1_receiver_warm(&receiver, &stats, &media);
	jb1_payload_fill(payload, sizeof(payload), 0x0cU);
	len = packet_make(pdu, JB1_SEQ, JB1_TS, JB1_SSRC, 0, 0, 0, payload,
			  sizeof(payload));
	len += packet_make(pdu + len, JB1_SEQ + 1U, JB1_TS + JB1_FRAME,
			   JB1_SSRC, 0, 0, 0, payload, sizeof(payload));
	at = pdu;
	left = len;
	for (i = 0; i < RTP_PDU_PACKETS_MAX && left; i++) {
		if (rtp_packet_parse(&receiver, at, left, 0, &parsed))
			break;
		at = parsed.tail;
		left = parsed.tail_len;
	}
	/* the stream carries on, which is what exposes a hole in the count */
	for (i = 2U; i < 4U; i++) {
		size_t one = packet_make(pdu, (uint16_t)(JB1_SEQ + i),
					 JB1_TS + i * JB1_FRAME, JB1_SSRC, 0, 0,
					 0, payload, sizeof(payload));

		CHECK_UINT(rtp_packet_parse(&receiver, pdu, one, 0, &parsed), 0);
	}
	CHECK_UINT(rtp_receiver_expected_get(&receiver), 4U);
	CHECK_UINT(receiver.received, 4U);
	rtp_receiver_report(&receiver, 0, 0, &report);
	CHECK_UINT(report.cumulative_lost, 0U);
	CHECK_UINT(report.fraction_lost, 0U);
}

/* One audio packet of one payload byte at 20 ms per sequence number. */
static int held_parse(struct rtp_receiver *receiver, uint16_t sequence,
		      uint32_t ssrc, uint8_t pt, uint8_t flags, uint8_t byte)
{
	struct rtp_packet parsed;
	uint8_t packet[32];
	size_t length;

	length = packet_make(packet, sequence, sequence * 160U, ssrc, pt, 0, 0,
			     &byte, 1);
	packet[1] |= flags;
	return rtp_packet_parse(receiver, packet, length,
				sequence * 20000000ULL, &parsed);
}

/*
 * RFC 3550 A.1 probation holds a new source's first packet. The receiver
 * releases it when the successor proves the source, so a stream plays from
 * its first packet. A gap or a new source during probation discards the held
 * packet, and so does a parse while a released packet waits untaken. The held
 * packet gives no A.3 reception, as the RFC's probation packet does not.
 */
static void test_probation_hold(void)
{
	struct udsp_media media = media_make();
	struct rtp_receiver receiver;
	struct rtp_packet held;
	struct rtp_stats stats;

	CASE("rtp_probation_holds_the_first_packet");
	rtp_stats_init(&stats);
	rtp_receiver_init(&receiver, &media, &stats);
	CHECK(held_parse(&receiver, 10, 7, 0, 0x80U, 0x5aU) < 0);
	CHECK(!rtp_receiver_held_take(&receiver, &held));
	CHECK_UINT(stats.packets_discarded, 0);
	CHECK(held_parse(&receiver, 11, 7, 0, 0, 0x5bU) == 0);
	CHECK(rtp_receiver_held_take(&receiver, &held));
	CHECK_UINT(held.sequence, 10);
	CHECK_UINT(held.timestamp, 1600);
	CHECK_UINT(held.ssrc, 7);
	CHECK_UINT(held.arrival_ns, 200000000ULL);
	CHECK_UINT(held.kind, RTP_PAYLOAD_AUDIO);
	CHECK(held.marker);
	CHECK_UINT(held.payload_len, 1);
	CHECK_UINT(held.payload[0], 0x5aU);
	CHECK(held.tail == NULL);
	CHECK(!rtp_receiver_held_take(&receiver, &held));
	CHECK_UINT(stats.packets_recv, 2);
	CHECK_UINT(stats.bytes_recv, 2U * (RTP_HEADER_SIZE + 1U));
	CHECK_UINT(stats.packets_discarded, 0);
	CHECK_UINT(receiver.received, 1);
	CHECK_UINT(receiver.base_seq, 11);

	CASE("rtp_probation_gap_discards_the_held_packet");
	CHECK(held_parse(&receiver, 100, 8, 0, 0, 1) < 0);
	CHECK(held_parse(&receiver, 300, 8, 0, 0, 2) < 0);
	CHECK_UINT(stats.packets_discarded, 1);
	CHECK(held_parse(&receiver, 301, 8, 0, 0, 3) == 0);
	CHECK(rtp_receiver_held_take(&receiver, &held));
	CHECK_UINT(held.sequence, 300);
	CHECK_UINT(held.payload[0], 2);

	CASE("rtp_probation_new_source_discards_the_held_packet");
	CHECK(held_parse(&receiver, 500, 9, 0, 0, 4) < 0);
	CHECK(held_parse(&receiver, 600, 12, 0, 0, 5) < 0);
	CHECK_UINT(stats.packets_discarded, 2);
	CHECK(held_parse(&receiver, 501, 9, 0, 0, 6) < 0);
	CHECK_UINT(stats.packets_discarded, 3);
	CHECK(held_parse(&receiver, 502, 9, 0, 0, 7) == 0);
	CHECK(rtp_receiver_held_take(&receiver, &held));
	CHECK_UINT(held.sequence, 501);
	CHECK_UINT(held.ssrc, 9);

	CASE("rtp_probation_untaken_release_is_discarded");
	CHECK(held_parse(&receiver, 700, 13, 0, 0, 8) < 0);
	CHECK(held_parse(&receiver, 701, 13, 0, 0, 9) == 0);
	CHECK(held_parse(&receiver, 702, 13, 0, 0, 10) == 0);
	CHECK_UINT(stats.packets_discarded, 4);
	CHECK(!rtp_receiver_held_take(&receiver, &held));

	CASE("rtp_probation_release_keeps_the_payload_type_rule");
	CHECK(held_parse(&receiver, 800, 14, 101, 0, 11) < 0);
	CHECK(held_parse(&receiver, 801, 14, 99, 0, 12) < 0);
	CHECK(rtp_receiver_held_take(&receiver, &held));
	CHECK_UINT(held.kind, RTP_PAYLOAD_TELEPHONE_EVENT);
	CHECK(held_parse(&receiver, 900, 15, 99, 0, 13) < 0);
	CHECK(held_parse(&receiver, 901, 15, 0, 0, 14) == 0);
	CHECK(!rtp_receiver_held_take(&receiver, &held));
	CHECK_UINT(stats.packets_discarded, 6);
}

int main(void)
{
	test_header_round_trip();
	test_header_options();
	test_sequence_order();
	test_sequence_wrap_loss();
	test_new_ssrc();
	test_jitter();
	test_reconfiguration();
	test_init_validation();
	test_direction_marker();
	test_cn_sequence_accounting();
	test_jitter_wrap_and_reset();
	test_jitter_timestamp_step();
	test_jitter_outlier();
	test_jitter_stall();
	test_jitter_timestamp_step_boundary();
	test_rtcp_sr();
	test_rtcp_rr_rtt();
	test_rtcp_raw_compounds();
	test_rtcp_relaxed_forms();
	test_rtcp_padding();
	test_sender_octets();
	test_rtcp_compound();
	test_rtcp_malformed();
	test_telev();
	test_telev_receive_recovery();
	test_telev_play();
	test_payload_demux();
	test_coalesced_pdu_field_shape();
	test_single_packet_keeps_whole_payload();
	test_coalesced_pdu_padding_per_packet();
	test_coalesced_pdu_three_packets();
	test_coalesced_pdu_empty_payloads();
	test_pdu_residual_discard_counts();
	test_coalesced_pdu_no_phantom_loss();
	test_g107();
	test_stats_snapshot();
	test_wb_jitter_at_clock();
	test_wb_transit_step();
	test_jitter_marked_forward_step();
	test_jitter_marked_step_keeps_estimate();
	test_jitter_marked_rule_narrow();
	test_jitter_marker_every_packet();
	test_jitter_marked_bound_edges();
	test_wb_telev_segments();
	test_wb_telev_segment_chain();
	test_wb_telev_segment_lost();
	test_probation_hold();
	return tu_summary("rtp");
}
