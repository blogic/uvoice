// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>

#include "srtp.h"

#define UDSP_SRTP_REKEY_FAIL_ERRNO_MASK	((UINT32_C(1) << 30) - 1u)

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
	 * suite_check maps UDSP_SRTP_NONE to -ENOENT. libsrtp srtp.h defines
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

static int suite_check(uint8_t suite)
{
	if (suite == UDSP_SRTP_NONE)
		return -ENOENT;
	if (suite == UDSP_SRTP_AES_CM_128_SHA1_80 ||
	    suite == UDSP_SRTP_AES_CM_128_SHA1_32)
		return 0;
	return -EPROTONOSUPPORT;
}

static void policy_set(srtp_policy_t *policy, uint8_t suite,
		       uint8_t *key, srtp_ssrc_type_t direction)
{
	memset(policy, 0, sizeof(*policy));
	if (suite == UDSP_SRTP_AES_CM_128_SHA1_80)
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

int udsp_srtp_init(void)
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

int udsp_srtp_create(struct udsp_srtp *s, const struct udsp_media *media)
{
	srtp_policy_t policy;
	srtp_err_status_t status;
	int result;

	if (!s)
		return -EINVAL;
	memset(s, 0, sizeof(*s));
	if (!media)
		return -EINVAL;
	result = suite_check(media->srtp.suite);
	if (result)
		return result;
	if (atomic_load_explicit(&srtp_init_state,
				 memory_order_acquire) != SRTP_INIT_READY)
		return -EAGAIN;

	memcpy(s->key_tx, media->srtp.key_tx, sizeof(s->key_tx));
	memcpy(s->key_rx, media->srtp.key_rx, sizeof(s->key_rx));
	s->suite = media->srtp.suite;
	policy_set(&policy, s->suite, s->key_tx, ssrc_any_outbound);
	status = srtp_create(&s->tx, &policy);
	if (status != srtp_err_status_ok) {
		result = status_errno(status);
		memory_zero(s, sizeof(*s));
		return result;
	}
	policy_set(&policy, s->suite, s->key_rx, ssrc_any_inbound);
	status = srtp_create(&s->rx, &policy);
	if (status == srtp_err_status_ok)
		return 0;

	result = status_errno(status);
	srtp_dealloc(s->tx);
	memory_zero(s, sizeof(*s));
	return result;
}

static int protect_run(struct udsp_srtp *s, uint8_t *buf, size_t len,
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

static int unprotect_run(struct udsp_srtp *s, uint8_t *buf, size_t len,
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

int udsp_srtp_protect(struct udsp_srtp *s, uint8_t *buf, size_t len,
		      size_t cap, size_t *out_len)
{
	return protect_run(s, buf, len, cap, SRTP_MAX_TRAILER_LEN, out_len,
			   false);
}

int udsp_srtp_unprotect(struct udsp_srtp *s, uint8_t *buf, size_t len,
			size_t cap, size_t *out_len)
{
	return unprotect_run(s, buf, len, cap, out_len, false);
}

int udsp_srtp_protect_rtcp(struct udsp_srtp *s, uint8_t *buf, size_t len,
			   size_t cap, size_t *out_len)
{
	return protect_run(s, buf, len, cap, SRTP_MAX_SRTCP_TRAILER_LEN,
			   out_len, true);
}

int udsp_srtp_unprotect_rtcp(struct udsp_srtp *s, uint8_t *buf, size_t len,
			     size_t cap, size_t *out_len)
{
	return unprotect_run(s, buf, len, cap, out_len, true);
}

int udsp_srtp_rekey(struct udsp_srtp *s, const struct udsp_media *media)
{
	srtp_policy_t new_policy;
	srtp_policy_t old_policy;
	srtp_err_status_t status;
	uint_least32_t fail;
	int result;

	if (!s || !media || !s->tx || !s->rx)
		return -EINVAL;
	result = suite_check(media->srtp.suite);
	if (result)
		return result;
	if (media->srtp.suite == s->suite &&
	    memcmp(media->srtp.key_tx, s->key_tx, sizeof(s->key_tx)) == 0 &&
	    memcmp(media->srtp.key_rx, s->key_rx, sizeof(s->key_rx)) == 0)
		return 0;
	fail = atomic_load_explicit(&s->rekey_fail, memory_order_relaxed);
	if (fail)
		atomic_store_explicit(&s->rekey_fail, 0u, memory_order_relaxed);
	if (fail && !(fail & UDSP_SRTP_REKEY_FAIL_RX))
		return -(int)(fail & UDSP_SRTP_REKEY_FAIL_ERRNO_MASK);
	policy_set(&new_policy, media->srtp.suite,
		   (uint8_t *)media->srtp.key_tx, ssrc_any_outbound);
	status = srtp_update(s->tx, &new_policy);
	if (status != srtp_err_status_ok)
		return status_errno(status);

	policy_set(&new_policy, media->srtp.suite,
		   (uint8_t *)media->srtp.key_rx, ssrc_any_inbound);
	status = srtp_err_status_ok;
	if (!(fail & UDSP_SRTP_REKEY_FAIL_RX))
		status = srtp_update(s->rx, &new_policy);
	if ((fail & UDSP_SRTP_REKEY_FAIL_RX) ||
	    status != srtp_err_status_ok) {
		result = fail & UDSP_SRTP_REKEY_FAIL_RX ?
			 -(int)(fail & UDSP_SRTP_REKEY_FAIL_ERRNO_MASK) :
			 status_errno(status);
		policy_set(&old_policy, s->suite, s->key_tx,
			   ssrc_any_outbound);
		if ((fail & UDSP_SRTP_REKEY_FAIL_RESTORE) ||
		    srtp_update(s->tx, &old_policy) != srtp_err_status_ok)
			udsp_srtp_destroy(s);
		return result;
	}

	memory_zero(s->key_tx, sizeof(s->key_tx));
	memory_zero(s->key_rx, sizeof(s->key_rx));
	memcpy(s->key_tx, media->srtp.key_tx, sizeof(s->key_tx));
	memcpy(s->key_rx, media->srtp.key_rx, sizeof(s->key_rx));
	s->suite = media->srtp.suite;
	return 0;
}

void udsp_srtp_destroy(struct udsp_srtp *s)
{
	if (!s)
		return;
	if (s->tx)
		srtp_dealloc(s->tx);
	if (s->rx)
		srtp_dealloc(s->rx);
	memory_zero(s, sizeof(*s));
}
