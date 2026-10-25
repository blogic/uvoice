/* SPDX-License-Identifier: LGPL-2.1-only */
/*
 * uvsrtp - test-only fault injection and state inspection
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Not installed. Used by the uvsrtp suite and by libudsp's media test to drive
 * the rekey failure and restore paths. A shipped run never sets a fault.
 */
#ifndef UVSRTP_TEST_H
#define UVSRTP_TEST_H

#include <stdbool.h>
#include <stdint.h>

#include "uvsrtp/uvsrtp.h"

/*
 * Arm a rekey failure. The low 30 bits are a positive errno that fails a real
 * rekey phase. Bit 31 fails the receive update; bit 30 also fails the transmit
 * restore. Zero disarms it. rekey reads this only after its same-key short
 * circuit, so an armed fault leaves that no-op result unchanged. A rekey that
 * reads a non-zero fault clears it to zero.
 */
#define UVSRTP_TEST_FAIL_RX		(UINT32_C(1) << 31)
#define UVSRTP_TEST_FAIL_RESTORE	(UINT32_C(1) << 30)

void uvsrtp_test_fault_set(struct uvsrtp *s, uint32_t fault);
uint32_t uvsrtp_test_fault_get(const struct uvsrtp *s);
bool uvsrtp_test_sessions_cleared(const struct uvsrtp *s);
const uint8_t *uvsrtp_test_key_tx(const struct uvsrtp *s);

#endif
