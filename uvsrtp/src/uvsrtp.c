// SPDX-License-Identifier: LGPL-2.1-only
/*
 * uvsrtp - SRTP/SRTCP per-stream protection over libsrtp2
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <srtp2/srtp.h>

#include "uvsrtp/uvsrtp.h"
#include "uvsrtp/uvsrtp_test.h"

#define UVSRTP_REKEY_FAIL_ERRNO_MASK	((UINT32_C(1) << 30) - 1u)

_Static_assert(UVSRTP_KEY_LEN == SRTP_AES_ICM_128_KEY_LEN_WSALT,
	       "AES-CM-128 key and salt length changed");
_Static_assert(UVSRTP_MAX_TRAILER == SRTP_MAX_TRAILER_LEN,
	       "SRTP trailer length changed");
_Static_assert(UVSRTP_MAX_SRTCP_TRAILER == SRTP_MAX_SRTCP_TRAILER_LEN,
	       "SRTCP trailer length changed");

/*
 * These are per-stream packet counters, created with the context and read at
 * teardown, so 32 bits is ample: 2^32 packets is years of one call leg at
 * 20 ms ptime, and the public struct uvsrtp_stats stays 64-bit, into which
 * uvsrtp_stats_get widens each counter. 64 bits would be overkill here and
 * would need a 64-bit atomic, which arm926ej-s (ARMv5) lacks: it pulls
 * __atomic_*_8 and libatomic, absent under musl, while a 32-bit atomic is
 * native (the rekey_fail field below already links that way).
 */
struct udsp_srtp_stats {
	_Atomic uint32_t protect_errors;
	_Atomic uint32_t unprotect_auth_fail;
	_Atomic uint32_t unprotect_replay;
	_Atomic uint32_t protect_ok;
	_Atomic uint32_t unprotect_ok;
};

struct uvsrtp {
	srtp_t tx;
	srtp_t rx;
	struct udsp_srtp_stats stats;
	uint8_t key_tx[UVSRTP_KEY_LEN];
	uint8_t key_rx[UVSRTP_KEY_LEN];
	uint8_t suite;
	_Atomic uint_least32_t rekey_fail;
};

enum srtp_init_state {
	SRTP_INIT_PENDING = 0,
	SRTP_INIT_RUNNING,
	SRTP_INIT_READY,
	SRTP_INIT_FAILED,
};

static atomic_int srtp_init_state = SRTP_INIT_PENDING;
static atomic_int srtp_init_result = -EAGAIN;

static void memory_zero(void *ptr, size_t len)
{
	volatile uint8_t *byte = ptr;

	while (len--)
		*byte++ = 0;
}

static void ctx_wipe(struct uvsrtp *s)
{
	if (s->tx)
		srtp_dealloc(s->tx);
	if (s->rx)
		srtp_dealloc(s->rx);
	memory_zero(s, sizeof(*s));
}

static bool status_is_replay(srtp_err_status_t status)
{
	return status == srtp_err_status_replay_fail ||
	       status == srtp_err_status_replay_old ||
	       status == srtp_err_status_pkt_idx_old;
}

static int status_errno(srtp_err_status_t status)
{
	switch (status) {
	case srtp_err_status_ok:
		return 0;
	case srtp_err_status_bad_param:
		return -EINVAL;
	case srtp_err_status_alloc_fail:
		return -ENOMEM;
	case srtp_err_status_terminus:
		return -EMSGSIZE;
	case srtp_err_status_auth_fail:
	case srtp_err_status_nonce_bad:
	case srtp_err_status_bad_mki:
		return -EBADMSG;
	case srtp_err_status_replay_fail:
	case srtp_err_status_replay_old:
	case srtp_err_status_pkt_idx_old:
		return -EALREADY;
	case srtp_err_status_no_such_op:
		return -EOPNOTSUPP;
	/*
	 * suite_check maps UVSRTP_SUITE_NONE to -ENOENT. libsrtp srtp.h defines
	 * no_ctx, which wildcard templates make unreachable.
	 */
	case srtp_err_status_no_ctx:
		return -ENOENT;
	case srtp_err_status_cant_check:
		return -EAGAIN;
	case srtp_err_status_key_expired:
		return -EKEYEXPIRED;
	default:
		return -EIO;
	}
}

static int suite_check(enum uvsrtp_suite suite)
{
	if (suite == UVSRTP_SUITE_NONE)
		return -ENOENT;
	if (suite == UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80 ||
	    suite == UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_32)
		return 0;
	return -EPROTONOSUPPORT;
}

static void policy_set(srtp_policy_t *policy, enum uvsrtp_suite suite,
		       uint8_t *key, srtp_ssrc_type_t direction)
{
	memset(policy, 0, sizeof(*policy));
	if (suite == UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80)
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy->rtp);
	else
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(&policy->rtp);
	/*
	 * RFC 4568 defines SHA1_32 for SRTP. libsrtp srtp.h warns against it
	 * for SRTCP.
	 */
	srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy->rtcp);
	policy->ssrc.type = direction;
	policy->key = key;
	policy->window_size = 128;
}

int uvsrtp_init(void)
{
	int expected = SRTP_INIT_PENDING;
	int result;
	int state;

	if (atomic_compare_exchange_strong_explicit(
		    &srtp_init_state, &expected, SRTP_INIT_RUNNING,
		    memory_order_acq_rel, memory_order_acquire)) {
		result = status_errno(srtp_init());
		atomic_store_explicit(&srtp_init_result, result,
				      memory_order_relaxed);
		atomic_store_explicit(&srtp_init_state,
				      result ? SRTP_INIT_FAILED : SRTP_INIT_READY,
				      memory_order_release);
		return result;
	}
	for (;;) {
		state = atomic_load_explicit(&srtp_init_state,
					     memory_order_acquire);
		if (state != SRTP_INIT_RUNNING)
			break;
		/*
		 * Startup is single-threaded. A pinned RT caller could starve
		 * the initializer without this yield.
		 */
		sched_yield();
	}
	if (state == SRTP_INIT_READY)
		return 0;
	return atomic_load_explicit(&srtp_init_result, memory_order_relaxed);
}

int uvsrtp_create(struct uvsrtp **out, enum uvsrtp_suite suite,
		  const uint8_t key_tx[UVSRTP_KEY_LEN],
		  const uint8_t key_rx[UVSRTP_KEY_LEN])
{
	srtp_policy_t policy;
	srtp_err_status_t status;
	struct uvsrtp *s;
	int result;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!key_tx || !key_rx)
		return -EINVAL;
	result = suite_check(suite);
	if (result)
		return result;
	if (atomic_load_explicit(&srtp_init_state,
				 memory_order_acquire) != SRTP_INIT_READY)
		return -EAGAIN;

	s = calloc(1, sizeof(*s));
	if (!s)
		return -ENOMEM;
	memcpy(s->key_tx, key_tx, sizeof(s->key_tx));
	memcpy(s->key_rx, key_rx, sizeof(s->key_rx));
	s->suite = (uint8_t)suite;
	policy_set(&policy, suite, s->key_tx, ssrc_any_outbound);
	status = srtp_create(&s->tx, &policy);
	if (status != srtp_err_status_ok) {
		result = status_errno(status);
		ctx_wipe(s);
		free(s);
		return result;
	}
	policy_set(&policy, suite, s->key_rx, ssrc_any_inbound);
	status = srtp_create(&s->rx, &policy);
	if (status != srtp_err_status_ok) {
		result = status_errno(status);
		ctx_wipe(s);
		free(s);
		return result;
	}
	*out = s;
	return 0;
}

static int protect_run(struct uvsrtp *s, uint8_t *buf, size_t len,
		       size_t cap, size_t trailer_len, size_t *out_len,
		       bool rtcp)
{
	srtp_err_status_t status;
	int packet_len;

	if (!s || !buf || !out_len || !s->tx)
		return -EINVAL;
	/* libsrtp srtp.h requires the full maximum trailer before protect. */
	if (len > INT_MAX || cap < len + trailer_len) {
		atomic_fetch_add_explicit(&s->stats.protect_errors, 1u,
					  memory_order_relaxed);
		return -EMSGSIZE;
	}
	packet_len = (int)len;
	if (rtcp)
		status = srtp_protect_rtcp(s->tx, buf, &packet_len);
	else
		status = srtp_protect(s->tx, buf, &packet_len);
	if (status != srtp_err_status_ok) {
		atomic_fetch_add_explicit(&s->stats.protect_errors, 1u,
					  memory_order_relaxed);
		return status_errno(status);
	}
	atomic_fetch_add_explicit(&s->stats.protect_ok, 1u,
				  memory_order_relaxed);
	*out_len = (size_t)packet_len;
	return 0;
}

static int unprotect_run(struct uvsrtp *s, uint8_t *buf, size_t len,
			 size_t cap, size_t *out_len, bool rtcp)
{
	srtp_err_status_t status;
	int packet_len;

	if (!s || !buf || !out_len || !s->rx)
		return -EINVAL;
	if (len > INT_MAX || cap < len)
		return -EMSGSIZE;
	packet_len = (int)len;
	if (rtcp)
		status = srtp_unprotect_rtcp(s->rx, buf, &packet_len);
	else
		status = srtp_unprotect(s->rx, buf, &packet_len);
	if (status == srtp_err_status_auth_fail)
		atomic_fetch_add_explicit(&s->stats.unprotect_auth_fail, 1u,
					  memory_order_relaxed);
	if (status_is_replay(status))
		atomic_fetch_add_explicit(&s->stats.unprotect_replay, 1u,
					  memory_order_relaxed);
	if (status != srtp_err_status_ok)
		return status_errno(status);
	atomic_fetch_add_explicit(&s->stats.unprotect_ok, 1u,
				  memory_order_relaxed);
	*out_len = (size_t)packet_len;
	return 0;
}

int uvsrtp_protect(struct uvsrtp *s, uint8_t *buf, size_t len,
		   size_t cap, size_t *out_len)
{
	return protect_run(s, buf, len, cap, SRTP_MAX_TRAILER_LEN, out_len,
			   false);
}

int uvsrtp_unprotect(struct uvsrtp *s, uint8_t *buf, size_t len,
		     size_t cap, size_t *out_len)
{
	return unprotect_run(s, buf, len, cap, out_len, false);
}

int uvsrtp_protect_rtcp(struct uvsrtp *s, uint8_t *buf, size_t len,
			size_t cap, size_t *out_len)
{
	return protect_run(s, buf, len, cap, SRTP_MAX_SRTCP_TRAILER_LEN,
			   out_len, true);
}

int uvsrtp_unprotect_rtcp(struct uvsrtp *s, uint8_t *buf, size_t len,
			  size_t cap, size_t *out_len)
{
	return unprotect_run(s, buf, len, cap, out_len, true);
}

int uvsrtp_rekey(struct uvsrtp *s, enum uvsrtp_suite suite,
		 const uint8_t key_tx[UVSRTP_KEY_LEN],
		 const uint8_t key_rx[UVSRTP_KEY_LEN])
{
	srtp_policy_t new_policy;
	srtp_policy_t old_policy;
	srtp_err_status_t status;
	uint_least32_t fail;
	int result;

	if (!s || !key_tx || !key_rx || !s->tx || !s->rx)
		return -EINVAL;
	result = suite_check(suite);
	if (result)
		return result;
	if ((uint8_t)suite == s->suite &&
	    memcmp(key_tx, s->key_tx, sizeof(s->key_tx)) == 0 &&
	    memcmp(key_rx, s->key_rx, sizeof(s->key_rx)) == 0)
		return 0;
	fail = atomic_load_explicit(&s->rekey_fail, memory_order_relaxed);
	if (fail)
		atomic_store_explicit(&s->rekey_fail, 0u, memory_order_relaxed);
	if (fail && !(fail & UVSRTP_TEST_FAIL_RX))
		return -(int)(fail & UVSRTP_REKEY_FAIL_ERRNO_MASK);
	policy_set(&new_policy, suite, (uint8_t *)key_tx, ssrc_any_outbound);
	status = srtp_update(s->tx, &new_policy);
	if (status != srtp_err_status_ok)
		return status_errno(status);

	policy_set(&new_policy, suite, (uint8_t *)key_rx, ssrc_any_inbound);
	status = srtp_err_status_ok;
	if (!(fail & UVSRTP_TEST_FAIL_RX))
		status = srtp_update(s->rx, &new_policy);
	if ((fail & UVSRTP_TEST_FAIL_RX) ||
	    status != srtp_err_status_ok) {
		result = fail & UVSRTP_TEST_FAIL_RX ?
			 -(int)(fail & UVSRTP_REKEY_FAIL_ERRNO_MASK) :
			 status_errno(status);
		policy_set(&old_policy, s->suite, s->key_tx,
			   ssrc_any_outbound);
		if ((fail & UVSRTP_TEST_FAIL_RESTORE) ||
		    srtp_update(s->tx, &old_policy) != srtp_err_status_ok)
			ctx_wipe(s);
		return result;
	}

	memory_zero(s->key_tx, sizeof(s->key_tx));
	memory_zero(s->key_rx, sizeof(s->key_rx));
	memcpy(s->key_tx, key_tx, sizeof(s->key_tx));
	memcpy(s->key_rx, key_rx, sizeof(s->key_rx));
	s->suite = (uint8_t)suite;
	return 0;
}

void uvsrtp_stats_get(const struct uvsrtp *s, struct uvsrtp_stats *out)
{
	if (!s || !out)
		return;
	out->protect_ok = atomic_load_explicit(&s->stats.protect_ok,
					       memory_order_relaxed);
	out->protect_fail = atomic_load_explicit(&s->stats.protect_errors,
						 memory_order_relaxed);
	out->unprotect_ok = atomic_load_explicit(&s->stats.unprotect_ok,
						 memory_order_relaxed);
	out->unprotect_auth_fail = atomic_load_explicit(
		&s->stats.unprotect_auth_fail, memory_order_relaxed);
	out->unprotect_replay = atomic_load_explicit(
		&s->stats.unprotect_replay, memory_order_relaxed);
}

void uvsrtp_destroy(struct uvsrtp *s)
{
	if (!s)
		return;
	ctx_wipe(s);
	free(s);
}

void uvsrtp_test_fault_set(struct uvsrtp *s, uint32_t fault)
{
	if (s)
		atomic_store_explicit(&s->rekey_fail, fault,
				      memory_order_relaxed);
}

uint32_t uvsrtp_test_fault_get(const struct uvsrtp *s)
{
	if (!s)
		return 0;
	return atomic_load_explicit(&s->rekey_fail, memory_order_relaxed);
}

bool uvsrtp_test_sessions_cleared(const struct uvsrtp *s)
{
	return s && !s->tx && !s->rx;
}

const uint8_t *uvsrtp_test_key_tx(const struct uvsrtp *s)
{
	return s ? s->key_tx : NULL;
}
