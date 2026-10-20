// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The handle rule of the fixed pools: the SIP transaction layer and the
 * shared DNS resolver (shared/resolve.h) both issue (index << 16) | generation.
 */
#ifndef UVOICE_SIP_HANDLE_H
#define UVOICE_SIP_HANDLE_H

#include <stdint.h>

/*
 * The handle rule. Generation 0 is never issued, so a live handle is never 0
 * and SIP_HANDLE_NONE stays free as "no owner".
 */
#define SIP_HANDLE_NONE		0u

static inline uint32_t sip_handle_make(unsigned int index, uint16_t gen)
{
	return ((uint32_t)index << 16) | gen;
}

static inline unsigned int sip_handle_index(uint32_t handle)
{
	return handle >> 16;
}

static inline uint16_t sip_handle_gen(uint32_t handle)
{
	return handle & 0xffffu;
}

#endif
