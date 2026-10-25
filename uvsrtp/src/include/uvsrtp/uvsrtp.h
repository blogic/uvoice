/* SPDX-License-Identifier: LGPL-2.1-only */
/*
 * uvsrtp - SRTP/SRTCP per-stream protection over libsrtp2
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#ifndef UVSRTP_H
#define UVSRTP_H

#include <stddef.h>
#include <stdint.h>

/* AES-CM-128 SDES key: 16 byte key followed by a 14 byte salt (RFC 4568). */
#define UVSRTP_KEY_LEN 30U

/*
 * Packet buffers must start on a 32-bit boundary. RTP protect needs
 * cap >= len + UVSRTP_MAX_TRAILER; SRTCP protect needs
 * cap >= len + UVSRTP_MAX_SRTCP_TRAILER. The values match libsrtp2
 * SRTP_MAX_TRAILER_LEN and SRTP_MAX_SRTCP_TRAILER_LEN, asserted in the
 * implementation.
 */
#define UVSRTP_MAX_TRAILER 144U
#define UVSRTP_MAX_SRTCP_TRAILER 148U

enum uvsrtp_suite {
	UVSRTP_SUITE_NONE = 0,
	UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80 = 1,
	UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_32 = 2,
};

struct uvsrtp;

struct uvsrtp_stats {
	uint64_t protect_ok;
	uint64_t protect_fail;
	uint64_t unprotect_ok;
	uint64_t unprotect_auth_fail;
	uint64_t unprotect_replay;
};

/*
 * Thread contract. Protect touches only the transmit session; unprotect only
 * the receive session. The two may run on different threads at the same time.
 * Rekey and destroy must not overlap a protect or unprotect call: the caller
 * quiesces both packet threads before either. uvsrtp_stats_get reads lock-free
 * counters and may run at any time.
 *
 * The first protect for a new outbound SSRC clones the wildcard stream inside
 * libsrtp2, one bounded allocation per SSRC.
 */

/**
 * uvsrtp_init() - initialise the process-global libsrtp2 state.
 *
 * Idempotent and safe to call from more than one thread. Runs the libsrtp2
 * initialiser once and yields while another caller runs it.
 *
 * Return: 0 on success, or a negative errno.
 */
int uvsrtp_init(void);

/**
 * uvsrtp_create() - allocate a protection context for one stream.
 * @out: receives the new context, or NULL on failure.
 * @suite: cipher suite; UVSRTP_SUITE_NONE is rejected with -ENOENT.
 * @key_tx: UVSRTP_KEY_LEN transmit key and salt.
 * @key_rx: UVSRTP_KEY_LEN receive key and salt.
 *
 * Return: 0 on success, -ENOENT for UVSRTP_SUITE_NONE, -EPROTONOSUPPORT for an
 * unknown suite, -EAGAIN before uvsrtp_init succeeded, or another negative
 * errno. On any failure @out is set to NULL.
 */
int uvsrtp_create(struct uvsrtp **out, enum uvsrtp_suite suite,
		  const uint8_t key_tx[UVSRTP_KEY_LEN],
		  const uint8_t key_rx[UVSRTP_KEY_LEN]);

/**
 * uvsrtp_rekey() - replace the keys of a live context.
 * @s: context from uvsrtp_create.
 * @suite: cipher suite.
 * @key_tx: UVSRTP_KEY_LEN transmit key and salt.
 * @key_rx: UVSRTP_KEY_LEN receive key and salt.
 *
 * The same suite and keys are a no-op that keeps the replay window. If the
 * receive update fails, the transmit side is restored to the old keys; if that
 * restore also fails, both sessions are released and the context rejects
 * further protect and unprotect calls until destroyed.
 *
 * Return: 0 on success or a negative errno.
 */
int uvsrtp_rekey(struct uvsrtp *s, enum uvsrtp_suite suite,
		 const uint8_t key_tx[UVSRTP_KEY_LEN],
		 const uint8_t key_rx[UVSRTP_KEY_LEN]);

/**
 * uvsrtp_protect() - protect an RTP packet in place.
 * @s: context from uvsrtp_create.
 * @buf: packet buffer, 32-bit aligned.
 * @len: RTP packet length.
 * @cap: capacity of @buf; must be at least @len + UVSRTP_MAX_TRAILER.
 * @out_len: receives the protected length.
 *
 * Return: 0 on success, -EMSGSIZE when @cap is too small, or a negative errno.
 */
int uvsrtp_protect(struct uvsrtp *s, uint8_t *buf, size_t len, size_t cap,
		   size_t *out_len);

/**
 * uvsrtp_unprotect() - unprotect an RTP packet in place.
 * @s: context from uvsrtp_create.
 * @buf: protected packet buffer.
 * @len: protected length.
 * @cap: capacity of @buf; must be at least @len.
 * @out_len: receives the plaintext length.
 *
 * Return: 0 on success, -EBADMSG on authentication failure, -EALREADY on a
 * replayed packet, or another negative errno.
 */
int uvsrtp_unprotect(struct uvsrtp *s, uint8_t *buf, size_t len, size_t cap,
		     size_t *out_len);

/**
 * uvsrtp_protect_rtcp() - protect an RTCP packet in place.
 * @s: context from uvsrtp_create.
 * @buf: packet buffer, 32-bit aligned.
 * @len: RTCP packet length.
 * @cap: capacity of @buf; must be at least @len + UVSRTP_MAX_SRTCP_TRAILER.
 * @out_len: receives the protected length.
 *
 * SRTCP always uses the 80-bit authentication tag (RFC 4568 6.2).
 *
 * Return: 0 on success, -EMSGSIZE when @cap is too small, or a negative errno.
 */
int uvsrtp_protect_rtcp(struct uvsrtp *s, uint8_t *buf, size_t len, size_t cap,
			size_t *out_len);

/**
 * uvsrtp_unprotect_rtcp() - unprotect an RTCP packet in place.
 * @s: context from uvsrtp_create.
 * @buf: protected packet buffer.
 * @len: protected length.
 * @cap: capacity of @buf; must be at least @len.
 * @out_len: receives the plaintext length.
 *
 * Return: 0 on success, -EBADMSG on authentication failure, -EALREADY on a
 * replayed packet, or another negative errno.
 */
int uvsrtp_unprotect_rtcp(struct uvsrtp *s, uint8_t *buf, size_t len,
			  size_t cap, size_t *out_len);

/**
 * uvsrtp_stats_get() - read the counters of a context.
 * @s: context from uvsrtp_create.
 * @out: receives the counter snapshot.
 *
 * The counters survive a rekey and restart only with a new context.
 */
void uvsrtp_stats_get(const struct uvsrtp *s, struct uvsrtp_stats *out);

/**
 * uvsrtp_destroy() - release a context and zeroise its key material.
 * @s: context from uvsrtp_create, or NULL.
 *
 * Safe to call with NULL.
 */
void uvsrtp_destroy(struct uvsrtp *s);

#endif
