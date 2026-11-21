// SPDX-License-Identifier: LGPL-2.1-only
/*
 * uvtransport - unit suite
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "uvtransport/uvtransport.h"

#define STEP_MS		20U
#define HANDSHAKE_MS	15000U
#define MEDIA_MS	3000U
#define PKT_MAX		256U
#define RTP_LEN		32U
#define RTCP_RR_LEN	8U

#define CASE(n) do { current_case = (n); } while (0)

#define CHECK(expr) do {						\
	tests_run++;							\
	if (!(expr)) {							\
		tests_failed++;						\
		fprintf(stderr, "not ok - %s: %s (%s:%d)\n",		\
			current_case, #expr, __FILE__, __LINE__);	\
	}								\
} while (0)

struct side {
	struct uvt_stream *stream;
	pthread_mutex_t lock;
	unsigned int candidate_events;
	bool gathering_done;
	bool done_forwarded;
	int ice_state;
	bool dtls_up;
	bool dtls_fail;
	bool dtls_closed;
	uint8_t rtp[PKT_MAX];
	size_t rtp_len;
	unsigned int rtp_count;
	uint8_t rtcp[PKT_MAX];
	size_t rtcp_len;
	unsigned int rtcp_count;
};

typedef bool (*pair_done_fn)(struct side *caller, struct side *callee);

static unsigned int tests_run;
static unsigned int tests_failed;
static const char *current_case;

/**
 * side_rtp() - record the plaintext of one received RTP packet
 * @user: the side
 * @buf: the packet
 * @len: its length
 */
static void side_rtp(void *user, const uint8_t *buf, size_t len)
{
	struct side *side = user;

	pthread_mutex_lock(&side->lock);
	if (len <= sizeof(side->rtp)) {
		memcpy(side->rtp, buf, len);
		side->rtp_len = len;
	}
	side->rtp_count++;
	pthread_mutex_unlock(&side->lock);
}

/**
 * side_rtcp() - record the plaintext of one received RTCP packet
 * @user: the side
 * @buf: the packet
 * @len: its length
 */
static void side_rtcp(void *user, const uint8_t *buf, size_t len)
{
	struct side *side = user;

	pthread_mutex_lock(&side->lock);
	if (len <= sizeof(side->rtcp)) {
		memcpy(side->rtcp, buf, len);
		side->rtcp_len = len;
	}
	side->rtcp_count++;
	pthread_mutex_unlock(&side->lock);
}

/**
 * side_event() - record one transport event
 * @user: the side
 * @event: the event
 * @arg: its argument
 */
static void side_event(void *user, enum uvt_event event, int arg)
{
	struct side *side = user;

	pthread_mutex_lock(&side->lock);
	switch (event) {
	case UVT_EV_CANDIDATE:
		side->candidate_events++;
		break;
	case UVT_EV_GATHERING_DONE:
		side->gathering_done = true;
		break;
	case UVT_EV_ICE_STATE:
		side->ice_state = arg;
		break;
	case UVT_EV_DTLS_UP:
		side->dtls_up = true;
		break;
	case UVT_EV_DTLS_FAIL:
		side->dtls_fail = true;
		break;
	case UVT_EV_DTLS_CLOSED:
		side->dtls_closed = true;
		break;
	}
	pthread_mutex_unlock(&side->lock);
}

/**
 * side_open() - make one side with host candidates alone
 * @side: the side
 * @role: its ICE role
 * @setup: its DTLS setup
 *
 * Return: true when the stream exists.
 */
static bool side_open(struct side *side, enum uvt_role role,
		      enum uvt_setup setup)
{
	struct uvt_cfg cfg;

	memset(side, 0, sizeof(*side));
	pthread_mutex_init(&side->lock, NULL);
	side->ice_state = -1;

	memset(&cfg, 0, sizeof(cfg));
	cfg.role = role;
	cfg.setup = setup;
	cfg.on_rtp = side_rtp;
	cfg.on_rtcp = side_rtcp;
	cfg.on_event = side_event;
	cfg.user = side;
	side->stream = uvt_create(&cfg);

	return side->stream != NULL;
}

/**
 * side_close() - release one side
 * @side: the side
 */
static void side_close(struct side *side)
{
	uvt_destroy(side->stream);
	side->stream = NULL;
	pthread_mutex_destroy(&side->lock);
}

/**
 * step_sleep() - wait one polling step
 */
static void step_sleep(void)
{
	struct timespec step = { 0, (long)STEP_MS * 1000000L };

	nanosleep(&step, NULL);
}

/**
 * candidates_forward() - move the gathered candidates of one side to the other
 * @from: the side that gathered
 * @to: the side that learns them
 */
static void candidates_forward(struct side *from, struct side *to)
{
	char line[UVT_CANDIDATE_MAX];
	bool done;

	while (uvt_candidate_next(from->stream, line, sizeof(line)) == 1)
		uvt_candidate_add(to->stream, line);

	pthread_mutex_lock(&from->lock);
	done = from->gathering_done && !from->done_forwarded;
	if (done)
		from->done_forwarded = true;
	pthread_mutex_unlock(&from->lock);

	if (done)
		uvt_remote_done(to->stream);
}

/**
 * flag_read() - read one flag of a side under its lock
 * @side: the side
 * @flag: the flag inside it
 *
 * Return: the flag.
 */
static bool flag_read(struct side *side, const bool *flag)
{
	bool value;

	pthread_mutex_lock(&side->lock);
	value = *flag;
	pthread_mutex_unlock(&side->lock);

	return value;
}

/**
 * count_read() - read one counter of a side under its lock
 * @side: the side
 * @count: the counter inside it
 *
 * Return: the counter.
 */
static unsigned int count_read(struct side *side, const unsigned int *count)
{
	unsigned int value;

	pthread_mutex_lock(&side->lock);
	value = *count;
	pthread_mutex_unlock(&side->lock);

	return value;
}

/**
 * pair_pump() - exchange candidates and tick both sides until done
 * @caller: the controlling side
 * @callee: the controlled side
 * @done: the condition to wait for
 * @timeout_ms: how long to wait at most
 *
 * Return: the condition at the end.
 */
static bool pair_pump(struct side *caller, struct side *callee,
		      pair_done_fn done, unsigned int timeout_ms)
{
	unsigned int waited = 0;

	while (waited < timeout_ms) {
		candidates_forward(caller, callee);
		candidates_forward(callee, caller);
		uvt_tick(caller->stream);
		uvt_tick(callee->stream);
		if (done(caller, callee))
			return true;
		step_sleep();
		waited += STEP_MS;
	}

	return done(caller, callee);
}

/**
 * both_up() - both handshakes completed
 * @caller: the controlling side
 * @callee: the controlled side
 *
 * Return: the condition.
 */
static bool both_up(struct side *caller, struct side *callee)
{
	return flag_read(caller, &caller->dtls_up) &&
	       flag_read(callee, &callee->dtls_up);
}

/**
 * both_fail() - both handshakes failed
 * @caller: the controlling side
 * @callee: the controlled side
 *
 * Return: the condition.
 */
static bool both_fail(struct side *caller, struct side *callee)
{
	return flag_read(caller, &caller->dtls_fail) &&
	       flag_read(callee, &callee->dtls_fail);
}

/**
 * callee_closed() - the callee saw the close of the caller
 * @caller: the controlling side
 * @callee: the controlled side
 *
 * Return: the condition.
 */
static bool callee_closed(struct side *caller, struct side *callee)
{
	return flag_read(callee, &callee->dtls_closed);
}

/**
 * callee_has_rtp() - the callee received an RTP packet
 * @caller: the controlling side
 * @callee: the controlled side
 *
 * Return: the condition.
 */
static bool callee_has_rtp(struct side *caller, struct side *callee)
{
	return count_read(callee, &callee->rtp_count) >= 1;
}

/**
 * caller_has_rtcp() - the caller received an RTCP packet
 * @caller: the controlling side
 * @callee: the controlled side
 *
 * Return: the condition.
 */
static bool caller_has_rtcp(struct side *caller, struct side *callee)
{
	return count_read(caller, &caller->rtcp_count) >= 1;
}

/**
 * pair_credentials() - exchange the ICE credentials in the order of a call
 * @caller: the controlling side, which describes itself first
 * @callee: the controlled side, which learns the caller before it describes
 * @fp_for_caller: the fingerprint the caller verifies the callee against
 * @fp_for_callee: the fingerprint the callee verifies the caller against
 *
 * Return: true when every call succeeded.
 */
static bool pair_credentials(struct side *caller, struct side *callee,
			     const uint8_t *fp_for_caller,
			     const uint8_t *fp_for_callee)
{
	char caller_ufrag[UVT_UFRAG_MAX], caller_pwd[UVT_PWD_MAX];
	char callee_ufrag[UVT_UFRAG_MAX], callee_pwd[UVT_PWD_MAX];

	if (uvt_local_get(caller->stream, caller_ufrag, caller_pwd))
		return false;
	if (uvt_remote_set(callee->stream, caller_ufrag, caller_pwd,
			   fp_for_callee, UVT_SETUP_ACTPASS))
		return false;
	if (uvt_local_get(callee->stream, callee_ufrag, callee_pwd))
		return false;

	return uvt_remote_set(caller->stream, callee_ufrag, callee_pwd,
			      fp_for_caller, UVT_SETUP_ACTIVE) == 0;
}

/**
 * rtp_build() - one PCMU packet with a known payload
 * @pkt: receives RTP_LEN bytes
 * @seq: the sequence number
 *
 * Return: RTP_LEN.
 */
static size_t rtp_build(uint8_t *pkt, uint16_t seq)
{
	size_t i;

	memset(pkt, 0, RTP_LEN);
	pkt[0] = 0x80;
	pkt[1] = 0;
	pkt[2] = (uint8_t)(seq >> 8);
	pkt[3] = (uint8_t)seq;
	pkt[7] = 0xa0;
	pkt[8] = 0x12;
	pkt[9] = 0x34;
	pkt[10] = 0x56;
	pkt[11] = 0x78;
	for (i = 12; i < RTP_LEN; i++)
		pkt[i] = (uint8_t)(0xd5 ^ i);

	return RTP_LEN;
}

/**
 * rtcp_rr_build() - one empty receiver report
 * @pkt: receives RTCP_RR_LEN bytes
 *
 * Return: RTCP_RR_LEN.
 */
static size_t rtcp_rr_build(uint8_t *pkt)
{
	memset(pkt, 0, RTCP_RR_LEN);
	pkt[0] = 0x80;
	pkt[1] = 201;
	pkt[3] = 1;
	pkt[4] = 0x12;
	pkt[5] = 0x34;
	pkt[6] = 0x56;
	pkt[7] = 0x78;

	return RTCP_RR_LEN;
}

/**
 * test_local() - the process identity and the refusals of one stream
 */
static void test_local(void)
{
	char ufrag[UVT_UFRAG_MAX], pwd[UVT_PWD_MAX];
	uint8_t fingerprint[UVT_FINGERPRINT_LEN];
	uint8_t again[UVT_FINGERPRINT_LEN];
	struct side side;

	CASE("local");
	CHECK(uvt_init() == 0);
	CHECK(uvt_fingerprint_get(fingerprint) == 0);
	CHECK(uvt_fingerprint_get(again) == 0);
	CHECK(!memcmp(fingerprint, again, sizeof(fingerprint)));

	CHECK(side_open(&side, UVT_ROLE_CONTROLLED, UVT_SETUP_ACTIVE));
	CHECK(uvt_local_get(side.stream, ufrag, pwd) == 0);
	CHECK(strlen(ufrag) >= 4);
	CHECK(strlen(pwd) >= 22);
	CHECK(uvt_up(side.stream) == false);
	CHECK(uvt_send_rtp(side.stream, fingerprint, sizeof(fingerprint)) ==
	      -EAGAIN);
	CHECK(uvt_remote_set(side.stream, "ab", pwd, fingerprint,
			     UVT_SETUP_ACTPASS) == -EINVAL);
	CHECK(uvt_remote_set(side.stream, ufrag, "short", fingerprint,
			     UVT_SETUP_ACTPASS) == -EINVAL);
	CHECK(uvt_remote_set(side.stream, ufrag, pwd, fingerprint,
			     UVT_SETUP_ACTPASS) == 0);
	CHECK(uvt_remote_set(side.stream, ufrag, pwd, fingerprint,
			     UVT_SETUP_ACTPASS) == -EALREADY);
	side_close(&side);
}

/**
 * test_handshake_and_media() - two streams connect, handshake and carry media
 */
static void test_handshake_and_media(void)
{
	uint8_t fingerprint[UVT_FINGERPRINT_LEN];
	uint8_t pkt[PKT_MAX];
	struct side caller, callee;
	size_t len;

	CASE("handshake");
	CHECK(uvt_fingerprint_get(fingerprint) == 0);
	CHECK(side_open(&caller, UVT_ROLE_CONTROLLING, UVT_SETUP_ACTPASS));
	CHECK(side_open(&callee, UVT_ROLE_CONTROLLED, UVT_SETUP_ACTIVE));
	CHECK(pair_credentials(&caller, &callee, fingerprint, fingerprint));
	CHECK(pair_pump(&caller, &callee, both_up, HANDSHAKE_MS));
	CHECK(uvt_up(caller.stream));
	CHECK(uvt_up(callee.stream));
	CHECK(count_read(&caller, &caller.candidate_events) >= 1);
	CHECK(count_read(&callee, &callee.candidate_events) >= 1);
	CHECK(uvt_ice_state(caller.stream) == UVT_ICE_CONNECTED ||
	      uvt_ice_state(caller.stream) == UVT_ICE_COMPLETED);
	CHECK(!flag_read(&caller, &caller.dtls_fail));
	CHECK(!flag_read(&callee, &callee.dtls_fail));

	CASE("rtp");
	len = rtp_build(pkt, 1);
	CHECK(uvt_send_rtp(caller.stream, pkt, len) == 0);
	CHECK(pair_pump(&caller, &callee, callee_has_rtp, MEDIA_MS));
	pthread_mutex_lock(&callee.lock);
	CHECK(callee.rtp_len == len);
	CHECK(!memcmp(callee.rtp, pkt, len));
	pthread_mutex_unlock(&callee.lock);

	CASE("rtcp");
	len = rtcp_rr_build(pkt);
	CHECK(uvt_send_rtcp(callee.stream, pkt, len) == 0);
	CHECK(pair_pump(&caller, &callee, caller_has_rtcp, MEDIA_MS));
	pthread_mutex_lock(&caller.lock);
	CHECK(caller.rtcp_len == len);
	CHECK(!memcmp(caller.rtcp, pkt, len));
	pthread_mutex_unlock(&caller.lock);

	side_close(&caller);
	side_close(&callee);
}

/**
 * test_fingerprint_mismatch() - a wrong fingerprint fails both sides
 */
static void test_fingerprint_mismatch(void)
{
	uint8_t fingerprint[UVT_FINGERPRINT_LEN];
	uint8_t wrong[UVT_FINGERPRINT_LEN];
	struct side caller, callee;

	CASE("fingerprint");
	CHECK(uvt_fingerprint_get(fingerprint) == 0);
	memcpy(wrong, fingerprint, sizeof(wrong));
	wrong[0] ^= 0xff;
	CHECK(side_open(&caller, UVT_ROLE_CONTROLLING, UVT_SETUP_ACTPASS));
	CHECK(side_open(&callee, UVT_ROLE_CONTROLLED, UVT_SETUP_ACTIVE));
	CHECK(pair_credentials(&caller, &callee, fingerprint, wrong));
	CHECK(pair_pump(&caller, &callee, both_fail, HANDSHAKE_MS));
	CHECK(!uvt_up(callee.stream));
	CHECK(!uvt_up(caller.stream));
	side_close(&caller);
	side_close(&callee);
}

/**
 * test_close() - a close_notify ends the media of both sides without a failure
 */
static void test_close(void)
{
	uint8_t fingerprint[UVT_FINGERPRINT_LEN];
	uint8_t pkt[PKT_MAX];
	struct side caller, callee;
	size_t len;

	CASE("close");
	CHECK(uvt_fingerprint_get(fingerprint) == 0);
	CHECK(side_open(&caller, UVT_ROLE_CONTROLLING, UVT_SETUP_ACTPASS));
	CHECK(side_open(&callee, UVT_ROLE_CONTROLLED, UVT_SETUP_ACTIVE));
	CHECK(uvt_close(caller.stream) == -ENOTCONN);
	CHECK(pair_credentials(&caller, &callee, fingerprint, fingerprint));
	CHECK(pair_pump(&caller, &callee, both_up, HANDSHAKE_MS));

	CHECK(uvt_close(caller.stream) == 0);
	CHECK(uvt_close(caller.stream) == -ENOTCONN);
	CHECK(!uvt_up(caller.stream));
	CHECK(pair_pump(&caller, &callee, callee_closed, MEDIA_MS));
	CHECK(!uvt_up(callee.stream));
	CHECK(!flag_read(&callee, &callee.dtls_fail));
	CHECK(!flag_read(&caller, &caller.dtls_fail));
	CHECK(!flag_read(&caller, &caller.dtls_closed));

	len = rtp_build(pkt, 2);
	CHECK(uvt_send_rtp(caller.stream, pkt, len) == -EPIPE);
	CHECK(uvt_send_rtp(callee.stream, pkt, len) == -EPIPE);
	len = rtcp_rr_build(pkt);
	CHECK(uvt_send_rtcp(callee.stream, pkt, len) == -EPIPE);

	side_close(&caller);
	side_close(&callee);
}

int main(void)
{
	test_local();
	test_handshake_and_media();
	test_fingerprint_mismatch();
	test_close();

	printf("%u checks, %u failed\n", tests_run, tests_failed);

	return tests_failed ? 1 : 0;
}
