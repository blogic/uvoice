// SPDX-License-Identifier: LGPL-2.1-only
/*
 * uvsrtp - unit suite
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The known-answer vectors (kat_*) are the reference octets of libsrtp's
 * srtp_validate() (test/srtp_driver.c, cisco/libsrtp 2.7.0), derived from the
 * RFC 3711 test vectors and distributed under the 3-clause BSD license.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "uvsrtp/uvsrtp.h"
#include "uvsrtp/uvsrtp_test.h"

#define PKT_CAP 256U
#define PAYLOAD_LEN 8U
#define PKT(name) _Alignas(uint32_t) uint8_t name[PKT_CAP]

static unsigned tests_run;
static unsigned tests_failed;
static const char *current_case;

#define CASE(n) do { current_case = (n); } while (0)

#define CHECK(expr) do {						\
	tests_run++;							\
	if (!(expr)) {							\
		tests_failed++;						\
		fprintf(stderr, "not ok - %s: %s (%s:%d)\n",		\
			current_case, #expr, __FILE__, __LINE__);	\
	}								\
} while (0)

#define CHECK_UINT(a, b) do {						\
	unsigned long long _a = (unsigned long long)(a);		\
	unsigned long long _b = (unsigned long long)(b);		\
	tests_run++;							\
	if (_a != _b) {							\
		tests_failed++;						\
		fprintf(stderr, "not ok - %s: %s = %llu != %llu (%s:%d)\n", \
			current_case, #a, _a, _b, __FILE__, __LINE__);	\
	}								\
} while (0)

static uint64_t protect_ok(struct uvsrtp *s)
{
	struct uvsrtp_stats st;

	uvsrtp_stats_get(s, &st);
	return st.protect_ok;
}

static uint64_t protect_fail(struct uvsrtp *s)
{
	struct uvsrtp_stats st;

	uvsrtp_stats_get(s, &st);
	return st.protect_fail;
}

static uint64_t unprotect_ok(struct uvsrtp *s)
{
	struct uvsrtp_stats st;

	uvsrtp_stats_get(s, &st);
	return st.unprotect_ok;
}

static uint64_t unprotect_auth_fail(struct uvsrtp *s)
{
	struct uvsrtp_stats st;

	uvsrtp_stats_get(s, &st);
	return st.unprotect_auth_fail;
}

static uint64_t unprotect_replay(struct uvsrtp *s)
{
	struct uvsrtp_stats st;

	uvsrtp_stats_get(s, &st);
	return st.unprotect_replay;
}

static void keys_set(uint8_t *key, uint8_t seed)
{
	size_t i;

	for (i = 0; i < UVSRTP_KEY_LEN; i++)
		key[i] = (uint8_t)(seed + i * 7U);
}

static size_t rtp_packet_make(uint8_t *packet, uint16_t sequence, uint32_t ssrc)
{
	static const uint8_t payload[PAYLOAD_LEN] = {
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

static struct uvsrtp *make(uint8_t suite, uint8_t tx_seed, uint8_t rx_seed)
{
	uint8_t key_tx[UVSRTP_KEY_LEN];
	uint8_t key_rx[UVSRTP_KEY_LEN];
	struct uvsrtp *s = NULL;

	keys_set(key_tx, tx_seed);
	keys_set(key_rx, rx_seed);
	CHECK(uvsrtp_create(&s, suite, key_tx, key_rx) == 0);
	return s;
}

static void tx_direction_round_trip(const char *name, uint8_t suite,
				    size_t tag_len)
{
	struct uvsrtp *sender = make(suite, 0x10U, 0x20U);
	struct uvsrtp *receiver = make(suite, 0x30U, 0x10U);
	PKT(packet);
	uint8_t plain[PKT_CAP];
	size_t plain_len;
	size_t protected_len;
	size_t result_len = 0;

	CASE(name);
	plain_len = rtp_packet_make(packet, 100U, 0x11111111U);
	memcpy(plain, packet, plain_len);
	CHECK(uvsrtp_protect(sender, packet, plain_len, sizeof(packet),
			     &protected_len) == 0);
	CHECK_UINT(protected_len, plain_len + tag_len);
	CHECK(uvsrtp_unprotect(receiver, packet, protected_len, sizeof(packet),
			       &result_len) == 0);
	CHECK_UINT(result_len, plain_len);
	CHECK(memcmp(packet, plain, plain_len) == 0);
	CHECK_UINT(protect_ok(sender), 1);
	CHECK_UINT(unprotect_ok(receiver), 1);
	uvsrtp_destroy(sender);
	uvsrtp_destroy(receiver);
}

static void rx_direction_round_trip(const char *name, uint8_t suite,
				    size_t tag_len)
{
	struct uvsrtp *sender = make(suite, 0x40U, 0x50U);
	struct uvsrtp *wrong_receiver = make(suite, 0x60U, 0x70U);
	struct uvsrtp *matching_receiver = make(suite, 0x80U, 0x40U);
	PKT(packet);
	PKT(matching_packet);
	uint8_t plain[PKT_CAP];
	size_t plain_len;
	size_t protected_len;
	size_t result_len = 0;

	CASE(name);
	plain_len = rtp_packet_make(packet, 200U, 0x22222222U);
	memcpy(plain, packet, plain_len);
	CHECK(uvsrtp_protect(sender, packet, plain_len, sizeof(packet),
			     &protected_len) == 0);
	CHECK_UINT(protected_len, plain_len + tag_len);
	memcpy(matching_packet, packet, protected_len);
	CHECK(uvsrtp_unprotect(wrong_receiver, packet, protected_len,
			       sizeof(packet), &result_len) == -EBADMSG);
	CHECK_UINT(unprotect_auth_fail(wrong_receiver), 1);
	CHECK(uvsrtp_unprotect(matching_receiver, matching_packet,
			       protected_len, sizeof(matching_packet),
			       &result_len) == 0);
	CHECK_UINT(result_len, plain_len);
	CHECK(memcmp(matching_packet, plain, plain_len) == 0);
	CHECK_UINT(protect_ok(sender), 1);
	CHECK_UINT(unprotect_ok(matching_receiver), 1);
	uvsrtp_destroy(sender);
	uvsrtp_destroy(wrong_receiver);
	uvsrtp_destroy(matching_receiver);
}

static void test_init(void)
{
	CASE("init_idempotent");
	CHECK(uvsrtp_init() == 0);
	CHECK(uvsrtp_init() == 0);
}

static void test_round_trips(void)
{
	tx_direction_round_trip("sha1_80_tx_round_trip",
				UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 10U);
	rx_direction_round_trip("sha1_80_rx_round_trip",
				UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 10U);
	tx_direction_round_trip("sha1_32_tx_round_trip",
				UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_32, 4U);
	rx_direction_round_trip("sha1_32_rx_round_trip",
				UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_32, 4U);
}

static void fail_closed_check(const char *name, uint8_t suite, int expected)
{
	uint8_t key[UVSRTP_KEY_LEN];
	struct uvsrtp *s = (struct uvsrtp *)0x1;

	CASE(name);
	keys_set(key, 1);
	CHECK(uvsrtp_create(&s, suite, key, key) == expected);
	CHECK(s == NULL);
	uvsrtp_destroy(s);
	uvsrtp_destroy(s);
}

static void test_fail_closed(void)
{
	fail_closed_check("unknown_suite_3_fails_closed", 3, -EPROTONOSUPPORT);
	fail_closed_check("unknown_suite_255_fails_closed", 255,
			  -EPROTONOSUPPORT);
	fail_closed_check("none_reports_bypass", UVSRTP_SUITE_NONE, -ENOENT);
}

static void test_auth_ciphertext(void)
{
	struct uvsrtp *sender =
		make(UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x12, 0x23);
	struct uvsrtp *receiver =
		make(UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x34, 0x12);
	PKT(packet);
	size_t plain_len;
	size_t protected_len;
	size_t result_len = 0xdeadU;

	CASE("ciphertext_tamper_auth_failure");
	plain_len = rtp_packet_make(packet, 300, 0x33333333U);
	CHECK(uvsrtp_protect(sender, packet, plain_len, sizeof(packet),
			     &protected_len) == 0);
	packet[12] ^= 0x80U;
	CHECK(uvsrtp_unprotect(receiver, packet, protected_len, sizeof(packet),
			       &result_len) == -EBADMSG);
	CHECK_UINT(unprotect_auth_fail(receiver), 1);
	CHECK_UINT(unprotect_ok(receiver), 0);
	CHECK_UINT(result_len, 0xdeadU);
	uvsrtp_destroy(sender);
	uvsrtp_destroy(receiver);
}

static void test_auth_tag(void)
{
	struct uvsrtp *sender =
		make(UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x13, 0x24);
	struct uvsrtp *receiver =
		make(UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x35, 0x13);
	PKT(packet);
	size_t plain_len;
	size_t protected_len;
	size_t result_len = 0;

	CASE("auth_tag_tamper_failure");
	plain_len = rtp_packet_make(packet, 301, 0x34343434U);
	CHECK(uvsrtp_protect(sender, packet, plain_len, sizeof(packet),
			     &protected_len) == 0);
	packet[protected_len - 1U] ^= 0x01U;
	CHECK(uvsrtp_unprotect(receiver, packet, protected_len, sizeof(packet),
			       &result_len) == -EBADMSG);
	CHECK_UINT(unprotect_auth_fail(receiver), 1);
	CHECK_UINT(unprotect_ok(receiver), 0);
	uvsrtp_destroy(sender);
	uvsrtp_destroy(receiver);
}

static void test_replay(void)
{
	struct uvsrtp *sender =
		make(UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x14, 0x25);
	struct uvsrtp *receiver =
		make(UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x36, 0x14);
	PKT(packet);
	PKT(replay);
	size_t plain_len;
	size_t protected_len;
	size_t result_len;

	CASE("replay_counted_distinctly");
	plain_len = rtp_packet_make(packet, 302, 0x35353535U);
	CHECK(uvsrtp_protect(sender, packet, plain_len, sizeof(packet),
			     &protected_len) == 0);
	memcpy(replay, packet, protected_len);
	CHECK(uvsrtp_unprotect(receiver, packet, protected_len, sizeof(packet),
			       &result_len) == 0);
	CHECK(uvsrtp_unprotect(receiver, replay, protected_len, sizeof(replay),
			       &result_len) == -EALREADY);
	CHECK_UINT(unprotect_replay(receiver), 1);
	CHECK_UINT(unprotect_auth_fail(receiver), 0);
	CHECK_UINT(unprotect_ok(receiver), 1);
	uvsrtp_destroy(sender);
	uvsrtp_destroy(receiver);
}

static void test_same_key_rekey(void)
{
	uint8_t tx[UVSRTP_KEY_LEN];
	uint8_t rx[UVSRTP_KEY_LEN];
	struct uvsrtp *sender =
		make(UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x42, 0x52);
	struct uvsrtp *receiver =
		make(UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x62, 0x42);
	PKT(packet);
	PKT(replay);
	size_t plain_len;
	size_t protected_len;
	size_t result_len;

	CASE("same_key_rekey_preserves_replay_window");
	keys_set(tx, 0x62);
	keys_set(rx, 0x42);
	plain_len = rtp_packet_make(packet, 350U, 0x37373737U);
	CHECK(uvsrtp_protect(sender, packet, plain_len, sizeof(packet),
			     &protected_len) == 0);
	memcpy(replay, packet, protected_len);
	CHECK(uvsrtp_unprotect(receiver, packet, protected_len, sizeof(packet),
			       &result_len) == 0);
	CHECK(uvsrtp_rekey(receiver, UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, tx,
			   rx) == 0);
	CHECK(uvsrtp_unprotect(receiver, replay, protected_len, sizeof(replay),
			       &result_len) == -EALREADY);
	CHECK_UINT(unprotect_ok(receiver), 1);
	CHECK_UINT(unprotect_replay(receiver), 1);
	uvsrtp_destroy(sender);
	uvsrtp_destroy(receiver);
}

static void test_ssrc_change(void)
{
	struct uvsrtp *sender =
		make(UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x43, 0x53);
	struct uvsrtp *receiver =
		make(UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x63, 0x43);
	PKT(packet);
	size_t plain_len;
	size_t protected_len;
	size_t result_len;

	CASE("rx_accepts_mid_session_ssrc_change");
	plain_len = rtp_packet_make(packet, 360U, 0x38383838U);
	CHECK(uvsrtp_protect(sender, packet, plain_len, sizeof(packet),
			     &protected_len) == 0);
	CHECK(uvsrtp_unprotect(receiver, packet, protected_len, sizeof(packet),
			       &result_len) == 0);
	plain_len = rtp_packet_make(packet, 360U, 0x39393939U);
	CHECK(uvsrtp_protect(sender, packet, plain_len, sizeof(packet),
			     &protected_len) == 0);
	CHECK(uvsrtp_unprotect(receiver, packet, protected_len, sizeof(packet),
			       &result_len) == 0);
	CHECK_UINT(protect_ok(sender), 2);
	CHECK_UINT(unprotect_ok(receiver), 2);
	uvsrtp_destroy(sender);
	uvsrtp_destroy(receiver);
}

static void test_capacity(void)
{
	struct uvsrtp *s =
		make(UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x15, 0x26);
	PKT(packet);
	uint8_t canary[PKT_CAP];
	size_t plain_len;
	size_t result_len = 0xbeefU;
	size_t cap;

	CASE("protect_small_capacity_no_write");
	memset(packet, 0xa5, sizeof(packet));
	plain_len = rtp_packet_make(packet, 303, 0x36363636U);
	memcpy(canary, packet, sizeof(canary));
	cap = plain_len + UVSRTP_MAX_TRAILER - 1U;
	CHECK(uvsrtp_protect(s, packet, plain_len, cap, &result_len) ==
	      -EMSGSIZE);
	CHECK(memcmp(packet, canary, sizeof(packet)) == 0);
	CHECK_UINT(result_len, 0xbeefU);
	CHECK_UINT(protect_fail(s), 1);
	uvsrtp_destroy(s);
}

static void test_rekey(void)
{
	uint8_t new_tx[UVSRTP_KEY_LEN];
	uint8_t new_rx[UVSRTP_KEY_LEN];
	struct uvsrtp *local =
		make(UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x11, 0x21);
	struct uvsrtp *old_receiver =
		make(UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x31, 0x11);
	struct uvsrtp *new_receiver =
		make(UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x71, 0x51);
	struct uvsrtp *old_sender =
		make(UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x21, 0x41);
	struct uvsrtp *new_sender =
		make(UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x61, 0x81);
	PKT(packet);
	PKT(packet_copy);
	PKT(old_inbound);
	PKT(new_replay);
	size_t plain_len;
	size_t protected_len;
	size_t old_inbound_len;
	size_t result_len;

	CASE("live_rekey_replaces_keys_preserves_stats");
	keys_set(new_tx, 0x51);
	keys_set(new_rx, 0x61);

	plain_len = rtp_packet_make(packet, 400, 0x41414141U);
	CHECK(uvsrtp_protect(local, packet, plain_len, sizeof(packet),
			     &protected_len) == 0);
	CHECK(uvsrtp_unprotect(old_receiver, packet, protected_len,
			       sizeof(packet), &result_len) == 0);

	plain_len = rtp_packet_make(packet, 500, 0x51515151U);
	CHECK(uvsrtp_protect(old_sender, packet, plain_len, sizeof(packet),
			     &protected_len) == 0);
	CHECK(uvsrtp_unprotect(local, packet, protected_len, sizeof(packet),
			       &result_len) == 0);
	plain_len = rtp_packet_make(old_inbound, 501, 0x51515151U);
	CHECK(uvsrtp_protect(old_sender, old_inbound, plain_len,
			     sizeof(old_inbound), &old_inbound_len) == 0);

	CHECK(uvsrtp_rekey(local, UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, new_tx,
			   new_rx) == 0);
	CHECK_UINT(protect_ok(local), 1);
	CHECK_UINT(unprotect_ok(local), 1);

	plain_len = rtp_packet_make(packet, 401, 0x41414141U);
	CHECK(uvsrtp_protect(local, packet, plain_len, sizeof(packet),
			     &protected_len) == 0);
	memcpy(packet_copy, packet, protected_len);
	CHECK(uvsrtp_unprotect(old_receiver, packet, protected_len,
			       sizeof(packet), &result_len) == -EBADMSG);
	CHECK(uvsrtp_unprotect(new_receiver, packet_copy, protected_len,
			       sizeof(packet_copy), &result_len) == 0);
	CHECK(uvsrtp_unprotect(local, old_inbound, old_inbound_len,
			       sizeof(old_inbound), &result_len) == -EBADMSG);
	plain_len = rtp_packet_make(packet, 500, 0x51515151U);
	CHECK(uvsrtp_protect(new_sender, packet, plain_len, sizeof(packet),
			     &protected_len) == 0);
	memcpy(new_replay, packet, protected_len);
	CHECK(uvsrtp_unprotect(local, packet, protected_len, sizeof(packet),
			       &result_len) == 0);
	CHECK(uvsrtp_unprotect(local, new_replay, protected_len,
			       sizeof(new_replay), &result_len) == -EALREADY);
	CHECK_UINT(protect_ok(local), 2);
	CHECK_UINT(unprotect_ok(local), 2);
	CHECK_UINT(unprotect_auth_fail(local), 1);
	CHECK_UINT(unprotect_replay(local), 1);

	uvsrtp_destroy(local);
	uvsrtp_destroy(old_receiver);
	uvsrtp_destroy(new_receiver);
	uvsrtp_destroy(old_sender);
	uvsrtp_destroy(new_sender);
}

static void test_unsupported_rekey(void)
{
	uint8_t bad_tx[UVSRTP_KEY_LEN];
	uint8_t bad_rx[UVSRTP_KEY_LEN];
	struct uvsrtp *local =
		make(UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x48, 0x58);
	struct uvsrtp *peer =
		make(UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x58, 0x48);
	PKT(packet);
	size_t plain_len;
	size_t protected_len;
	size_t result_len;

	CASE("unsupported_rekey_preserves_live_session");
	keys_set(bad_tx, 0x68);
	keys_set(bad_rx, 0x78);
	CHECK(uvsrtp_rekey(local, 3U, bad_tx, bad_rx) == -EPROTONOSUPPORT);
	plain_len = rtp_packet_make(packet, 700U, 0x71717171U);
	CHECK(uvsrtp_protect(local, packet, plain_len, sizeof(packet),
			     &protected_len) == 0);
	CHECK(uvsrtp_unprotect(peer, packet, protected_len, sizeof(packet),
			       &result_len) == 0);
	plain_len = rtp_packet_make(packet, 701U, 0x72727272U);
	CHECK(uvsrtp_protect(peer, packet, plain_len, sizeof(packet),
			     &protected_len) == 0);
	CHECK(uvsrtp_unprotect(local, packet, protected_len, sizeof(packet),
			       &result_len) == 0);
	CHECK_UINT(protect_ok(local), 1);
	CHECK_UINT(unprotect_ok(local), 1);
	uvsrtp_destroy(local);
	uvsrtp_destroy(peer);
}

static void test_destroy_null_safe(void)
{
	CASE("destroy_null_safe");
	uvsrtp_destroy(NULL);
	uvsrtp_destroy(NULL);
	CHECK(true);
}

static void test_fault_same_key_no_op(void)
{
	uint8_t tx[UVSRTP_KEY_LEN];
	uint8_t rx[UVSRTP_KEY_LEN];
	struct uvsrtp *s =
		make(UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x36, 0x69);

	CASE("fault_injection_keeps_same_key_no_op");
	keys_set(tx, 0x36);
	keys_set(rx, 0x69);
	uvsrtp_test_fault_set(s, (uint32_t)ENOMEM);
	CHECK(uvsrtp_rekey(s, UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, tx, rx) ==
	      0);
	CHECK_UINT(uvsrtp_test_fault_get(s), ENOMEM);
	uvsrtp_test_fault_set(s, 0);
	uvsrtp_destroy(s);
}

static void fault_restore_case(const char *name, bool restore_fails)
{
	uint8_t tx[UVSRTP_KEY_LEN];
	uint8_t rx[UVSRTP_KEY_LEN];
	uint8_t next_tx[UVSRTP_KEY_LEN];
	uint8_t next_rx[UVSRTP_KEY_LEN];
	struct uvsrtp *session;
	struct uvsrtp *peer = NULL;
	PKT(packet);
	uint8_t plain[PKT_CAP];
	uint32_t fault = UVSRTP_TEST_FAIL_RX | (uint32_t)EIO;
	size_t plain_len;
	size_t protected_len = 0;
	size_t recovered_len = 0;

	CASE(name);
	keys_set(tx, 0x29);
	keys_set(rx, 0x61);
	session = NULL;
	CHECK(uvsrtp_create(&session, UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, tx,
			    rx) == 0);
	if (!restore_fails)
		/* peer.rx is keyed to the session's unchanged transmit key. */
		CHECK(uvsrtp_create(&peer,
				    UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, rx,
				    tx) == 0);

	keys_set(next_tx, 0x73);
	keys_set(next_rx, 0x17);
	if (restore_fails)
		fault |= UVSRTP_TEST_FAIL_RESTORE;
	uvsrtp_test_fault_set(session, fault);
	CHECK(uvsrtp_rekey(session, UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80,
			   next_tx, next_rx) == -EIO);

	plain_len = rtp_packet_make(packet, 9100, 0x19375571U);
	memcpy(plain, packet, plain_len);
	if (restore_fails) {
		CHECK(uvsrtp_test_sessions_cleared(session));
		CHECK(uvsrtp_protect(session, packet, plain_len,
				     sizeof(packet), &protected_len) ==
		      -EINVAL);
	} else {
		CHECK(!uvsrtp_test_sessions_cleared(session));
		CHECK_UINT(uvsrtp_test_fault_get(session), 0);
		CHECK(uvsrtp_protect(session, packet, plain_len,
				     sizeof(packet), &protected_len) == 0);
		CHECK(uvsrtp_unprotect(peer, packet, protected_len,
				       sizeof(packet), &recovered_len) == 0);
		CHECK_UINT(recovered_len, plain_len);
		CHECK(memcmp(packet, plain, plain_len) == 0);
		uvsrtp_destroy(peer);
	}
	uvsrtp_destroy(session);
}

static void test_fault(void)
{
	test_fault_same_key_no_op();
	fault_restore_case("receive_rekey_failure_restores_transmit", false);
	fault_restore_case("failed_transmit_restore_clears_sessions", true);
}

static void rtcp_round_trip_check(const char *name, uint8_t suite,
				  uint8_t traffic_key, uint32_t ssrc)
{
	struct uvsrtp *sender =
		make(suite, traffic_key, (uint8_t)(traffic_key + 1U));
	struct uvsrtp *receiver =
		make(suite, (uint8_t)(traffic_key + 2U), traffic_key);
	PKT(packet);
	uint8_t plain[PKT_CAP];
	size_t plain_len;
	size_t protected_len;
	size_t result_len;

	CASE(name);
	plain_len = rtcp_packet_make(packet, ssrc);
	memcpy(plain, packet, plain_len);
	CHECK(uvsrtp_protect_rtcp(sender, packet, plain_len, sizeof(packet),
				  &protected_len) == 0);
	CHECK_UINT(protected_len, plain_len + 14U);
	CHECK(uvsrtp_unprotect_rtcp(receiver, packet, protected_len,
				    sizeof(packet), &result_len) == 0);
	CHECK_UINT(result_len, plain_len);
	CHECK(memcmp(packet, plain, plain_len) == 0);
	CHECK_UINT(protect_ok(sender), 1);
	CHECK_UINT(unprotect_ok(receiver), 1);
	uvsrtp_destroy(sender);
	uvsrtp_destroy(receiver);
}

static void test_rtcp_round_trips(void)
{
	rtcp_round_trip_check("srtcp_round_trip",
			      UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x18U,
			      0x61616161U);
	rtcp_round_trip_check("srtcp_sha1_32_round_trip_uses_80_bit_tag",
			      UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_32, 0x28U,
			      0x62626262U);
}

static void test_rtcp_tamper(void)
{
	struct uvsrtp *sender =
		make(UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x19, 0x2a);
	struct uvsrtp *receiver =
		make(UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80, 0x3b, 0x19);
	PKT(packet);
	size_t plain_len;
	size_t protected_len;
	size_t result_len;

	CASE("srtcp_auth_tag_tamper_failure");
	plain_len = rtcp_packet_make(packet, 0x62626262U);
	CHECK(uvsrtp_protect_rtcp(sender, packet, plain_len, sizeof(packet),
				  &protected_len) == 0);
	packet[protected_len - 1U] ^= 0x01U;
	CHECK(uvsrtp_unprotect_rtcp(receiver, packet, protected_len,
				    sizeof(packet), &result_len) == -EBADMSG);
	CHECK_UINT(unprotect_auth_fail(receiver), 1);
	CHECK_UINT(unprotect_ok(receiver), 0);
	uvsrtp_destroy(sender);
	uvsrtp_destroy(receiver);
}

/* libsrtp srtp_validate() reference octets (RFC 3711 test vectors). */
static const uint8_t kat_key[UVSRTP_KEY_LEN] = {
	0xe1, 0xf9, 0x7a, 0x0d, 0x3e, 0x01, 0x8b, 0xe0,
	0xd6, 0x4f, 0xa3, 0x2c, 0x06, 0xde, 0x41, 0x39,
	0x0e, 0xc6, 0x75, 0xad, 0x49, 0x8a, 0xfe, 0xeb,
	0xb6, 0x96, 0x0b, 0x3a, 0xab, 0xe6,
};

static void test_kat_rtp(void)
{
	static const uint8_t plaintext_ref[28] = {
		0x80, 0x0f, 0x12, 0x34, 0xde, 0xca, 0xfb, 0xad,
		0xca, 0xfe, 0xba, 0xbe, 0xab, 0xab, 0xab, 0xab,
		0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab,
		0xab, 0xab, 0xab, 0xab,
	};
	static const uint8_t ciphertext[38] = {
		0x80, 0x0f, 0x12, 0x34, 0xde, 0xca, 0xfb, 0xad,
		0xca, 0xfe, 0xba, 0xbe, 0x4e, 0x55, 0xdc, 0x4c,
		0xe7, 0x99, 0x78, 0xd8, 0x8c, 0xa4, 0xd2, 0x15,
		0x94, 0x9d, 0x24, 0x02, 0xb7, 0x8d, 0x6a, 0xcc,
		0x99, 0xea, 0x17, 0x9b, 0x8d, 0xbb,
	};
	struct uvsrtp *snd = NULL;
	struct uvsrtp *recv = NULL;
	PKT(packet);
	size_t len;
	size_t out = 0;

	CASE("kat_rtp_aes_cm_128_sha1_80");
	CHECK(uvsrtp_create(&snd, UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80,
			    kat_key, kat_key) == 0);
	CHECK(uvsrtp_create(&recv, UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80,
			    kat_key, kat_key) == 0);
	memcpy(packet, plaintext_ref, sizeof(plaintext_ref));
	CHECK(uvsrtp_protect(snd, packet, sizeof(plaintext_ref),
			     sizeof(packet), &out) == 0);
	CHECK_UINT(out, sizeof(ciphertext));
	CHECK(memcmp(packet, ciphertext, sizeof(ciphertext)) == 0);
	len = out;
	CHECK(uvsrtp_unprotect(recv, packet, len, sizeof(packet), &out) == 0);
	CHECK_UINT(out, sizeof(plaintext_ref));
	CHECK(memcmp(packet, plaintext_ref, sizeof(plaintext_ref)) == 0);
	uvsrtp_destroy(snd);
	uvsrtp_destroy(recv);
}

static void test_kat_rtcp(void)
{
	static const uint8_t plaintext_ref[24] = {
		0x81, 0xc8, 0x00, 0x0b, 0xca, 0xfe, 0xba, 0xbe,
		0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab,
		0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab,
	};
	static const uint8_t ciphertext[38] = {
		0x81, 0xc8, 0x00, 0x0b, 0xca, 0xfe, 0xba, 0xbe,
		0x71, 0x28, 0x03, 0x5b, 0xe4, 0x87, 0xb9, 0xbd,
		0xbe, 0xf8, 0x90, 0x41, 0xf9, 0x77, 0xa5, 0xa8,
		0x80, 0x00, 0x00, 0x01, 0x99, 0x3e, 0x08, 0xcd,
		0x54, 0xd6, 0xc1, 0x23, 0x07, 0x98,
	};
	struct uvsrtp *snd = NULL;
	struct uvsrtp *recv = NULL;
	PKT(packet);
	size_t len;
	size_t out = 0;

	CASE("kat_rtcp_aes_cm_128_sha1_80");
	CHECK(uvsrtp_create(&snd, UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80,
			    kat_key, kat_key) == 0);
	CHECK(uvsrtp_create(&recv, UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80,
			    kat_key, kat_key) == 0);
	memcpy(packet, plaintext_ref, sizeof(plaintext_ref));
	CHECK(uvsrtp_protect_rtcp(snd, packet, sizeof(plaintext_ref),
				  sizeof(packet), &out) == 0);
	CHECK_UINT(out, sizeof(ciphertext));
	CHECK(memcmp(packet, ciphertext, sizeof(ciphertext)) == 0);
	len = out;
	CHECK(uvsrtp_unprotect_rtcp(recv, packet, len, sizeof(packet), &out) ==
	      0);
	CHECK_UINT(out, sizeof(plaintext_ref));
	CHECK(memcmp(packet, plaintext_ref, sizeof(plaintext_ref)) == 0);
	uvsrtp_destroy(snd);
	uvsrtp_destroy(recv);
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
	test_destroy_null_safe();
	test_fault();
	test_rtcp_round_trips();
	test_rtcp_tamper();
	test_kat_rtp();
	test_kat_rtcp();

	if (tests_failed)
		fprintf(stderr, "uvsrtp: %u/%u checks failed\n", tests_failed,
			tests_run);
	else
		printf("uvsrtp: %u checks passed\n", tests_run);
	return tests_failed ? 1 : 0;
}
