// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#ifndef UDSP_SRTP_PRIVATE_H
#define UDSP_SRTP_PRIVATE_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include <srtp2/srtp.h>

#include "udsp/udsp.h"

#define UDSP_SRTP_AES_CM_128_KEY_LEN	30U

_Static_assert(UDSP_SRTP_AES_CM_128_KEY_LEN ==
	       SRTP_AES_ICM_128_KEY_LEN_WSALT,
	       "AES-CM-128 key and salt length changed");
_Static_assert(sizeof(((struct udsp_media *)0)->srtp.key_tx) ==
	       UDSP_SRTP_AES_CM_128_KEY_LEN,
	       "udsp_media transmit SRTP key length changed");
_Static_assert(sizeof(((struct udsp_media *)0)->srtp.key_rx) ==
	       UDSP_SRTP_AES_CM_128_KEY_LEN,
	       "udsp_media receive SRTP key length changed");

/* Atomic fields let the control thread read while both packet threads run. */
struct udsp_srtp_stats {
	_Atomic uint_least64_t protect_errors;
	_Atomic uint_least64_t unprotect_auth_fail;
	_Atomic uint_least64_t unprotect_replay;
	_Atomic uint_least64_t protect_ok;
	_Atomic uint_least64_t unprotect_ok;
};

#define UDSP_SRTP_REKEY_FAIL_RX		(UINT32_C(1) << 31)
#define UDSP_SRTP_REKEY_FAIL_RESTORE	(UINT32_C(1) << 30)

struct udsp_srtp {
	srtp_t tx;
	srtp_t rx;
	struct udsp_srtp_stats stats;
	uint8_t key_tx[UDSP_SRTP_AES_CM_128_KEY_LEN];
	uint8_t key_rx[UDSP_SRTP_AES_CM_128_KEY_LEN];
	uint8_t suite;
	/*
	 * Test-only fault injection (ack_drop/prep_fail class). A positive
	 * errno in the low 30 bits fails a real rekey phase. Bit 31 selects the
	 * receive update; bit 30 also fails the transmit restore. Zero, the
	 * value of every shipped run, disarms it. The rekey reads this only
	 * after its same-key short circuit: an armed hook must not change that
	 * no-op result. The errno field must be non-zero; a zero errno would
	 * report success for an rx-phase failure. Create's memset erases an arm
	 * set before the stream's first configuration.
	 */
	_Atomic uint_least32_t rekey_fail;
};

/*
 * Packet buffers must start on a 32-bit boundary, as libsrtp srtp.h requires.
 * RTP protect requires cap >= len + SRTP_MAX_TRAILER_LEN, which adds 144 bytes.
 * SRTCP protect requires cap >= len + SRTP_MAX_SRTCP_TRAILER_LEN, which adds
 * 148 bytes.
 *
 * Protect touches only the transmit session on the engine thread. Unprotect
 * touches only the receive session on the net thread. Rekey and destroy must
 * not overlap a packet call. The caller quiesces both threads at the tick
 * boundary before either operation (udsp.h Media sockets).
 *
 * The first protect for a new outbound SSRC clones the wildcard stream inside
 * libsrtp. The engine thread pays one measured, bounded 240-byte allocation per
 * SSRC.
 */
int udsp_srtp_init(void);
int udsp_srtp_create(struct udsp_srtp *s, const struct udsp_media *media);
int udsp_srtp_protect(struct udsp_srtp *s, uint8_t *buf, size_t len,
		      size_t cap, size_t *out_len);
int udsp_srtp_unprotect(struct udsp_srtp *s, uint8_t *buf, size_t len,
			size_t cap, size_t *out_len);
int udsp_srtp_protect_rtcp(struct udsp_srtp *s, uint8_t *buf, size_t len,
			   size_t cap, size_t *out_len);
int udsp_srtp_unprotect_rtcp(struct udsp_srtp *s, uint8_t *buf, size_t len,
			     size_t cap, size_t *out_len);
int udsp_srtp_rekey(struct udsp_srtp *s, const struct udsp_media *media);
void udsp_srtp_destroy(struct udsp_srtp *s);

#endif
