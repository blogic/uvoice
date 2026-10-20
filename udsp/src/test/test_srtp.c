// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../rtp/srtp.h"
#include "util.h"

#define TEST_PACKET_CAP 256U
#define TEST_PAYLOAD_LEN 8U
#define TEST_PACKET(name) _Alignas(uint32_t) uint8_t name[TEST_PACKET_CAP]

static uint64_t counter_get(const _Atomic uint_least64_t *counter)
{
	return atomic_load_explicit(counter, memory_order_relaxed);
}

static void keys_set(uint8_t *key, uint8_t seed)
{
	size_t i;

	for (i = 0; i < 30U; i++)
		key[i] = (uint8_t)(seed + i * 7U);
}

static struct udsp_media media_make(uint8_t suite, uint8_t tx_seed,
				    uint8_t rx_seed)
{
	struct udsp_media media;

	memset(&media, 0, sizeof(media));
	media.srtp.suite = suite;
	keys_set(media.srtp.key_tx, tx_seed);
	keys_set(media.srtp.key_rx, rx_seed);
	return media;
}

static size_t rtp_packet_make(uint8_t *packet, uint16_t sequence,
			      uint32_t ssrc)
{
	static const uint8_t payload[TEST_PAYLOAD_LEN] = {
		0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
	};

	memset(packet, 0, 12U + sizeof(payload));
	packet[0] = 0x80;
	packet[1] = 0;
	packet[2] = (uint8_t)(sequence >> 8);
	packet[3] = (uint8_t)sequence;
	packet[4] = 0x01;
	packet[5] = 0x02;
	packet[6] = 0x03;
	packet[7] = 0x04;
	packet[8] = (uint8_t)(ssrc >> 24);
	packet[9] = (uint8_t)(ssrc >> 16);
	packet[10] = (uint8_t)(ssrc >> 8);
	packet[11] = (uint8_t)ssrc;
	memcpy(packet + 12U, payload, sizeof(payload));
	return 12U + sizeof(payload);
}

static size_t rtcp_packet_make(uint8_t *packet, uint32_t ssrc)
{
	memset(packet, 0, 8U);
	packet[0] = 0x80;
	packet[1] = 201;
	packet[3] = 1;
	packet[4] = (uint8_t)(ssrc >> 24);
	packet[5] = (uint8_t)(ssrc >> 16);
	packet[6] = (uint8_t)(ssrc >> 8);
	packet[7] = (uint8_t)ssrc;
	return 8U;
}

static void tx_direction_round_trip(const char *name, uint8_t suite,
				    size_t tag_len)
{
	struct udsp_media sender_media = media_make(suite, 0x10U, 0x20U);
	struct udsp_media receiver_media = media_make(suite, 0x30U, 0x10U);
	struct udsp_srtp sender;
	struct udsp_srtp receiver;
	TEST_PACKET(packet);
	uint8_t plain[TEST_PACKET_CAP];
	size_t plain_len;
	size_t protected_len;
	size_t result_len = 0;

	CASE(name);
	CHECK(udsp_srtp_create(&sender, &sender_media) == 0);
	CHECK(udsp_srtp_create(&receiver, &receiver_media) == 0);
	plain_len = rtp_packet_make(packet, 100U, 0x11111111U);
	memcpy(plain, packet, plain_len);
	CHECK(udsp_srtp_protect(&sender, packet, plain_len, sizeof(packet),
				&protected_len) == 0);
	CHECK_UINT(protected_len, plain_len + tag_len);
	CHECK(udsp_srtp_unprotect(&receiver, packet, protected_len,
				  sizeof(packet), &result_len) == 0);
	CHECK_UINT(result_len, plain_len);
	CHECK(memcmp(packet, plain, plain_len) == 0);
	CHECK_UINT(counter_get(&sender.stats.protect_ok), 1);
	CHECK_UINT(counter_get(&receiver.stats.unprotect_ok), 1);
	udsp_srtp_destroy(&sender);
	udsp_srtp_destroy(&receiver);
}

static void rx_direction_round_trip(const char *name, uint8_t suite,
				    size_t tag_len)
{
	struct udsp_media sender_media = media_make(suite, 0x40U, 0x50U);
	struct udsp_media wrong_media = media_make(suite, 0x60U, 0x70U);
	struct udsp_media matching_media = media_make(suite, 0x80U, 0x40U);
	struct udsp_srtp sender;
	struct udsp_srtp wrong_receiver;
	struct udsp_srtp matching_receiver;
	TEST_PACKET(packet);
	TEST_PACKET(matching_packet);
	uint8_t plain[TEST_PACKET_CAP];
	size_t plain_len;
	size_t protected_len;
	size_t result_len = 0;

	CASE(name);
	CHECK(udsp_srtp_create(&sender, &sender_media) == 0);
	CHECK(udsp_srtp_create(&wrong_receiver, &wrong_media) == 0);
	CHECK(udsp_srtp_create(&matching_receiver, &matching_media) == 0);
	plain_len = rtp_packet_make(packet, 200U, 0x22222222U);
	memcpy(plain, packet, plain_len);
	CHECK(udsp_srtp_protect(&sender, packet, plain_len, sizeof(packet),
				&protected_len) == 0);
	CHECK_UINT(protected_len, plain_len + tag_len);
	memcpy(matching_packet, packet, protected_len);
	CHECK(udsp_srtp_unprotect(&wrong_receiver, packet, protected_len,
				  sizeof(packet), &result_len) == -EBADMSG);
	CHECK_UINT(counter_get(&wrong_receiver.stats.unprotect_auth_fail), 1);
	CHECK(udsp_srtp_unprotect(&matching_receiver, matching_packet,
				  protected_len, sizeof(matching_packet),
				  &result_len) == 0);
	CHECK_UINT(result_len, plain_len);
	CHECK(memcmp(matching_packet, plain, plain_len) == 0);
	CHECK_UINT(counter_get(&sender.stats.protect_ok), 1);
	CHECK_UINT(counter_get(&matching_receiver.stats.unprotect_ok), 1);
	udsp_srtp_destroy(&sender);
	udsp_srtp_destroy(&wrong_receiver);
	udsp_srtp_destroy(&matching_receiver);
}

static void test_init(void)
{
	CASE("srtp_init_idempotent");
	CHECK(udsp_srtp_init() == 0);
	CHECK(udsp_srtp_init() == 0);
}

static void test_round_trips(void)
{
	tx_direction_round_trip("srtp_sha1_80_tx_direction_round_trip",
				UDSP_SRTP_AES_CM_128_SHA1_80, 10U);
	rx_direction_round_trip("srtp_sha1_80_rx_direction_round_trip",
				UDSP_SRTP_AES_CM_128_SHA1_80, 10U);
	tx_direction_round_trip("srtp_sha1_32_tx_direction_round_trip",
				UDSP_SRTP_AES_CM_128_SHA1_32, 4U);
	rx_direction_round_trip("srtp_sha1_32_rx_direction_round_trip",
				UDSP_SRTP_AES_CM_128_SHA1_32, 4U);
}

static void fail_closed_check(const char *name, uint8_t suite,
			      int expected_result)
{
	struct udsp_media media = media_make(suite, 1, 2);
	struct udsp_srtp state;
	struct udsp_srtp zero;

	CASE(name);
	memset(&state, 0xa5, sizeof(state));
	memset(&zero, 0, sizeof(zero));
	CHECK(udsp_srtp_create(&state, &media) == expected_result);
	CHECK(memcmp(&state, &zero, sizeof(state)) == 0);
	udsp_srtp_destroy(&state);
	udsp_srtp_destroy(&state);
	CHECK(memcmp(&state, &zero, sizeof(state)) == 0);
}

static void test_fail_closed(void)
{
	fail_closed_check("srtp_unknown_suite_3_fails_closed", 3,
			  -EPROTONOSUPPORT);
	fail_closed_check("srtp_unknown_suite_255_fails_closed", 255,
			  -EPROTONOSUPPORT);
	fail_closed_check("srtp_none_reports_bypass", UDSP_SRTP_NONE, -ENOENT);
}

static void test_auth_ciphertext(void)
{
	struct udsp_media sender_media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x12, 0x23);
	struct udsp_media receiver_media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x34, 0x12);
	struct udsp_srtp sender;
	struct udsp_srtp receiver;
	TEST_PACKET(packet);
	size_t plain_len;
	size_t protected_len;
	size_t result_len = 0xdeadU;

	CASE("srtp_ciphertext_tamper_auth_failure");
	CHECK(udsp_srtp_create(&sender, &sender_media) == 0);
	CHECK(udsp_srtp_create(&receiver, &receiver_media) == 0);
	plain_len = rtp_packet_make(packet, 300, 0x33333333U);
	CHECK(udsp_srtp_protect(&sender, packet, plain_len, sizeof(packet),
				&protected_len) == 0);
	packet[12] ^= 0x80U;
	CHECK(udsp_srtp_unprotect(&receiver, packet, protected_len,
				  sizeof(packet), &result_len) == -EBADMSG);
	CHECK_UINT(counter_get(&receiver.stats.unprotect_auth_fail), 1);
	CHECK_UINT(counter_get(&receiver.stats.unprotect_ok), 0);
	CHECK_UINT(result_len, 0xdeadU);
	udsp_srtp_destroy(&sender);
	udsp_srtp_destroy(&receiver);
}

static void test_auth_tag(void)
{
	struct udsp_media sender_media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x13, 0x24);
	struct udsp_media receiver_media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x35, 0x13);
	struct udsp_srtp sender;
	struct udsp_srtp receiver;
	TEST_PACKET(packet);
	size_t plain_len;
	size_t protected_len;
	size_t result_len = 0;

	CASE("srtp_auth_tag_tamper_failure");
	CHECK(udsp_srtp_create(&sender, &sender_media) == 0);
	CHECK(udsp_srtp_create(&receiver, &receiver_media) == 0);
	plain_len = rtp_packet_make(packet, 301, 0x34343434U);
	CHECK(udsp_srtp_protect(&sender, packet, plain_len, sizeof(packet),
				&protected_len) == 0);
	packet[protected_len - 1U] ^= 0x01U;
	CHECK(udsp_srtp_unprotect(&receiver, packet, protected_len,
				  sizeof(packet), &result_len) == -EBADMSG);
	CHECK_UINT(counter_get(&receiver.stats.unprotect_auth_fail), 1);
	CHECK_UINT(counter_get(&receiver.stats.unprotect_ok), 0);
	udsp_srtp_destroy(&sender);
	udsp_srtp_destroy(&receiver);
}

static void test_replay(void)
{
	struct udsp_media sender_media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x14, 0x25);
	struct udsp_media receiver_media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x36, 0x14);
	struct udsp_srtp sender;
	struct udsp_srtp receiver;
	TEST_PACKET(packet);
	TEST_PACKET(replay);
	size_t plain_len;
	size_t protected_len;
	size_t result_len;

	CASE("srtp_replay_counted_distinctly");
	CHECK(udsp_srtp_create(&sender, &sender_media) == 0);
	CHECK(udsp_srtp_create(&receiver, &receiver_media) == 0);
	plain_len = rtp_packet_make(packet, 302, 0x35353535U);
	CHECK(udsp_srtp_protect(&sender, packet, plain_len, sizeof(packet),
				&protected_len) == 0);
	memcpy(replay, packet, protected_len);
	CHECK(udsp_srtp_unprotect(&receiver, packet, protected_len,
				  sizeof(packet), &result_len) == 0);
	CHECK(udsp_srtp_unprotect(&receiver, replay, protected_len,
				  sizeof(replay), &result_len) == -EALREADY);
	CHECK_UINT(counter_get(&receiver.stats.unprotect_replay), 1);
	CHECK_UINT(counter_get(&receiver.stats.unprotect_auth_fail), 0);
	CHECK_UINT(counter_get(&receiver.stats.unprotect_ok), 1);
	udsp_srtp_destroy(&sender);
	udsp_srtp_destroy(&receiver);
}

static void test_same_key_rekey(void)
{
	struct udsp_media sender_media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x42, 0x52);
	struct udsp_media receiver_media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x62, 0x42);
	struct udsp_srtp sender;
	struct udsp_srtp receiver;
	TEST_PACKET(packet);
	TEST_PACKET(replay);
	size_t plain_len;
	size_t protected_len;
	size_t result_len;

	CASE("srtp_same_key_rekey_preserves_replay_window");
	CHECK(udsp_srtp_create(&sender, &sender_media) == 0);
	CHECK(udsp_srtp_create(&receiver, &receiver_media) == 0);
	plain_len = rtp_packet_make(packet, 350U, 0x37373737U);
	CHECK(udsp_srtp_protect(&sender, packet, plain_len, sizeof(packet),
				&protected_len) == 0);
	memcpy(replay, packet, protected_len);
	CHECK(udsp_srtp_unprotect(&receiver, packet, protected_len,
				  sizeof(packet), &result_len) == 0);
	CHECK(udsp_srtp_rekey(&receiver, &receiver_media) == 0);
	CHECK(udsp_srtp_unprotect(&receiver, replay, protected_len,
				  sizeof(replay), &result_len) == -EALREADY);
	CHECK_UINT(counter_get(&receiver.stats.unprotect_ok), 1);
	CHECK_UINT(counter_get(&receiver.stats.unprotect_replay), 1);
	udsp_srtp_destroy(&sender);
	udsp_srtp_destroy(&receiver);
}

static void test_ssrc_change(void)
{
	struct udsp_media sender_media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x43, 0x53);
	struct udsp_media receiver_media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x63, 0x43);
	struct udsp_srtp sender;
	struct udsp_srtp receiver;
	TEST_PACKET(packet);
	size_t plain_len;
	size_t protected_len;
	size_t result_len;

	CASE("srtp_rx_accepts_mid_session_ssrc_change");
	CHECK(udsp_srtp_create(&sender, &sender_media) == 0);
	CHECK(udsp_srtp_create(&receiver, &receiver_media) == 0);
	plain_len = rtp_packet_make(packet, 360U, 0x38383838U);
	CHECK(udsp_srtp_protect(&sender, packet, plain_len, sizeof(packet),
				&protected_len) == 0);
	CHECK(udsp_srtp_unprotect(&receiver, packet, protected_len,
				  sizeof(packet), &result_len) == 0);
	plain_len = rtp_packet_make(packet, 360U, 0x39393939U);
	CHECK(udsp_srtp_protect(&sender, packet, plain_len, sizeof(packet),
				&protected_len) == 0);
	CHECK(udsp_srtp_unprotect(&receiver, packet, protected_len,
				  sizeof(packet), &result_len) == 0);
	CHECK_UINT(counter_get(&sender.stats.protect_ok), 2);
	CHECK_UINT(counter_get(&receiver.stats.unprotect_ok), 2);
	udsp_srtp_destroy(&sender);
	udsp_srtp_destroy(&receiver);
}

static void test_capacity(void)
{
	struct udsp_media media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x15, 0x26);
	struct udsp_srtp state;
	TEST_PACKET(packet);
	uint8_t canary[TEST_PACKET_CAP];
	size_t plain_len;
	size_t result_len = 0xbeefU;
	size_t cap;

	CASE("srtp_protect_small_capacity_no_write");
	CHECK(udsp_srtp_create(&state, &media) == 0);
	memset(packet, 0xa5, sizeof(packet));
	plain_len = rtp_packet_make(packet, 303, 0x36363636U);
	memcpy(canary, packet, sizeof(canary));
	cap = plain_len + SRTP_MAX_TRAILER_LEN - 1U;
	CHECK(udsp_srtp_protect(&state, packet, plain_len, cap,
				&result_len) == -EMSGSIZE);
	CHECK(memcmp(packet, canary, sizeof(packet)) == 0);
	CHECK_UINT(result_len, 0xbeefU);
	CHECK_UINT(counter_get(&state.stats.protect_errors), 1);
	udsp_srtp_destroy(&state);
}

static void test_rekey(void)
{
	struct udsp_media old_media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x11, 0x21);
	struct udsp_media new_media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x51, 0x61);
	struct udsp_media old_receiver_media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x31, 0x11);
	struct udsp_media new_receiver_media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x71, 0x51);
	struct udsp_media old_sender_media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x21, 0x41);
	struct udsp_media new_sender_media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x61, 0x81);
	struct udsp_srtp local;
	struct udsp_srtp old_receiver;
	struct udsp_srtp new_receiver;
	struct udsp_srtp old_sender;
	struct udsp_srtp new_sender;
	TEST_PACKET(packet);
	TEST_PACKET(packet_copy);
	TEST_PACKET(old_inbound);
	TEST_PACKET(new_replay);
	size_t plain_len;
	size_t protected_len;
	size_t old_inbound_len;
	size_t result_len;

	CASE("srtp_live_rekey_replaces_keys_preserves_stats");
	CHECK(udsp_srtp_create(&local, &old_media) == 0);
	CHECK(udsp_srtp_create(&old_receiver, &old_receiver_media) == 0);
	CHECK(udsp_srtp_create(&new_receiver, &new_receiver_media) == 0);
	CHECK(udsp_srtp_create(&old_sender, &old_sender_media) == 0);
	CHECK(udsp_srtp_create(&new_sender, &new_sender_media) == 0);

	plain_len = rtp_packet_make(packet, 400, 0x41414141U);
	CHECK(udsp_srtp_protect(&local, packet, plain_len, sizeof(packet),
				&protected_len) == 0);
	CHECK(udsp_srtp_unprotect(&old_receiver, packet, protected_len,
				  sizeof(packet), &result_len) == 0);

	plain_len = rtp_packet_make(packet, 500, 0x51515151U);
	CHECK(udsp_srtp_protect(&old_sender, packet, plain_len, sizeof(packet),
				&protected_len) == 0);
	CHECK(udsp_srtp_unprotect(&local, packet, protected_len, sizeof(packet),
				  &result_len) == 0);
	plain_len = rtp_packet_make(old_inbound, 501, 0x51515151U);
	CHECK(udsp_srtp_protect(&old_sender, old_inbound, plain_len,
				sizeof(old_inbound), &old_inbound_len) == 0);

	CHECK(udsp_srtp_rekey(&local, &new_media) == 0);
	CHECK_UINT(counter_get(&local.stats.protect_ok), 1);
	CHECK_UINT(counter_get(&local.stats.unprotect_ok), 1);

	plain_len = rtp_packet_make(packet, 401, 0x41414141U);
	CHECK(udsp_srtp_protect(&local, packet, plain_len, sizeof(packet),
				&protected_len) == 0);
	memcpy(packet_copy, packet, protected_len);
	CHECK(udsp_srtp_unprotect(&old_receiver, packet, protected_len,
				  sizeof(packet), &result_len) == -EBADMSG);
	CHECK(udsp_srtp_unprotect(&new_receiver, packet_copy, protected_len,
				  sizeof(packet_copy), &result_len) == 0);
	CHECK(udsp_srtp_unprotect(&local, old_inbound, old_inbound_len,
				  sizeof(old_inbound), &result_len) == -EBADMSG);
	plain_len = rtp_packet_make(packet, 500, 0x51515151U);
	CHECK(udsp_srtp_protect(&new_sender, packet, plain_len, sizeof(packet),
				&protected_len) == 0);
	memcpy(new_replay, packet, protected_len);
	CHECK(udsp_srtp_unprotect(&local, packet, protected_len, sizeof(packet),
				  &result_len) == 0);
	CHECK(udsp_srtp_unprotect(&local, new_replay, protected_len,
				  sizeof(new_replay), &result_len) == -EALREADY);
	CHECK_UINT(counter_get(&local.stats.protect_ok), 2);
	CHECK_UINT(counter_get(&local.stats.unprotect_ok), 2);
	CHECK_UINT(counter_get(&local.stats.unprotect_auth_fail), 1);
	CHECK_UINT(counter_get(&local.stats.unprotect_replay), 1);

	udsp_srtp_destroy(&local);
	udsp_srtp_destroy(&old_receiver);
	udsp_srtp_destroy(&new_receiver);
	udsp_srtp_destroy(&old_sender);
	udsp_srtp_destroy(&new_sender);
}

static void test_unsupported_rekey(void)
{
	struct udsp_media local_media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x48, 0x58);
	struct udsp_media peer_media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x58, 0x48);
	struct udsp_media unsupported_media = media_make(3U, 0x68, 0x78);
	struct udsp_srtp local;
	struct udsp_srtp peer;
	TEST_PACKET(packet);
	size_t plain_len;
	size_t protected_len;
	size_t result_len;

	CASE("srtp_unsupported_rekey_preserves_live_session");
	CHECK(udsp_srtp_create(&local, &local_media) == 0);
	CHECK(udsp_srtp_create(&peer, &peer_media) == 0);
	CHECK(udsp_srtp_rekey(&local, &unsupported_media) ==
	      -EPROTONOSUPPORT);
	plain_len = rtp_packet_make(packet, 700U, 0x71717171U);
	CHECK(udsp_srtp_protect(&local, packet, plain_len, sizeof(packet),
				&protected_len) == 0);
	CHECK(udsp_srtp_unprotect(&peer, packet, protected_len, sizeof(packet),
				  &result_len) == 0);
	plain_len = rtp_packet_make(packet, 701U, 0x72727272U);
	CHECK(udsp_srtp_protect(&peer, packet, plain_len, sizeof(packet),
				&protected_len) == 0);
	CHECK(udsp_srtp_unprotect(&local, packet, protected_len, sizeof(packet),
				  &result_len) == 0);
	CHECK_UINT(counter_get(&local.stats.protect_ok), 1);
	CHECK_UINT(counter_get(&local.stats.unprotect_ok), 1);
	udsp_srtp_destroy(&local);
	udsp_srtp_destroy(&peer);
}

static void test_zeroization(void)
{
	struct udsp_media media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x17, 0x28);
	struct udsp_srtp state;
	struct udsp_srtp zero;

	CASE("srtp_destroy_zeroizes_policy_keys");
	memset(&zero, 0, sizeof(zero));
	CHECK(udsp_srtp_create(&state, &media) == 0);
	CHECK(memcmp(state.key_tx, zero.key_tx, sizeof(state.key_tx)) != 0);
	CHECK(memcmp(state.key_rx, zero.key_rx, sizeof(state.key_rx)) != 0);
	udsp_srtp_destroy(&state);
	CHECK(memcmp(&state, &zero, sizeof(state)) == 0);
}

static void rtcp_round_trip_check(const char *name, uint8_t suite,
				  uint8_t traffic_key, uint32_t ssrc)
{
	struct udsp_media sender_media =
		media_make(suite, traffic_key, (uint8_t)(traffic_key + 1U));
	struct udsp_media receiver_media =
		media_make(suite, (uint8_t)(traffic_key + 2U), traffic_key);
	struct udsp_srtp sender;
	struct udsp_srtp receiver;
	TEST_PACKET(packet);
	uint8_t plain[TEST_PACKET_CAP];
	size_t plain_len;
	size_t protected_len;
	size_t result_len;

	CASE(name);
	CHECK(udsp_srtp_create(&sender, &sender_media) == 0);
	CHECK(udsp_srtp_create(&receiver, &receiver_media) == 0);
	plain_len = rtcp_packet_make(packet, ssrc);
	memcpy(plain, packet, plain_len);
	CHECK(udsp_srtp_protect_rtcp(&sender, packet, plain_len, sizeof(packet),
				     &protected_len) == 0);
	CHECK_UINT(protected_len, plain_len + 14U);
	CHECK(udsp_srtp_unprotect_rtcp(&receiver, packet, protected_len,
				       sizeof(packet), &result_len) == 0);
	CHECK_UINT(result_len, plain_len);
	CHECK(memcmp(packet, plain, plain_len) == 0);
	CHECK_UINT(counter_get(&sender.stats.protect_ok), 1);
	CHECK_UINT(counter_get(&receiver.stats.unprotect_ok), 1);
	udsp_srtp_destroy(&sender);
	udsp_srtp_destroy(&receiver);
}

static void test_rtcp_round_trips(void)
{
	rtcp_round_trip_check("srtcp_round_trip",
			      UDSP_SRTP_AES_CM_128_SHA1_80, 0x18U,
			      0x61616161U);
	rtcp_round_trip_check("srtcp_sha1_32_round_trip_uses_80_bit_tag",
			      UDSP_SRTP_AES_CM_128_SHA1_32, 0x28U,
			      0x62626262U);
}

static void test_rtcp_tamper(void)
{
	struct udsp_media sender_media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x19, 0x2a);
	struct udsp_media receiver_media =
		media_make(UDSP_SRTP_AES_CM_128_SHA1_80, 0x3b, 0x19);
	struct udsp_srtp sender;
	struct udsp_srtp receiver;
	TEST_PACKET(packet);
	size_t plain_len;
	size_t protected_len;
	size_t result_len;

	CASE("srtcp_auth_tag_tamper_failure");
	CHECK(udsp_srtp_create(&sender, &sender_media) == 0);
	CHECK(udsp_srtp_create(&receiver, &receiver_media) == 0);
	plain_len = rtcp_packet_make(packet, 0x62626262U);
	CHECK(udsp_srtp_protect_rtcp(&sender, packet, plain_len, sizeof(packet),
				     &protected_len) == 0);
	packet[protected_len - 1U] ^= 0x01U;
	CHECK(udsp_srtp_unprotect_rtcp(&receiver, packet, protected_len,
				       sizeof(packet), &result_len) == -EBADMSG);
	CHECK_UINT(counter_get(&receiver.stats.unprotect_auth_fail), 1);
	CHECK_UINT(counter_get(&receiver.stats.unprotect_ok), 0);
	udsp_srtp_destroy(&sender);
	udsp_srtp_destroy(&receiver);
}

int main(void)
{
	test_init();
	test_round_trips();
	test_fail_closed();
	test_auth_ciphertext();
	test_auth_tag();
	test_replay();
	test_same_key_rekey();
	test_ssrc_change();
	test_capacity();
	test_rekey();
	test_unsupported_rekey();
	test_zeroization();
	test_rtcp_round_trips();
	test_rtcp_tamper();
	return tu_summary("srtp");
}
