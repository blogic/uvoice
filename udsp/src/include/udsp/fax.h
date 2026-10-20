// SPDX-License-Identifier: BSD-2-Clause
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * This file stays BSD-2-Clause so a SLIC or fax plug-in and its tools
 * compile against it without LGPL obligations. The rest of libudsp is
 * LGPL-2.1-only.
 *
 * udsp T.38 fax gateway contract. The shape is stable. The udsp-fax-spandsp
 * plug-in implements it with spandsp, and libudsp dlopens it like the SLIC
 * plug-in (core/fax_plugin.h). libudsp itself ships G.711 passthrough with
 * CED/CNG/V.21 detection in dsp/fax_det.c and no fax_ops; UDSP_FEAT_T38, and
 * with it HAL_FEAT_T38, stays clear until a plug-in loads.
 *
 * A gateway instance bridges one audio channel to one UDPTL session. rx_pcm
 * takes one 80-sample frame from the phone; tx_pcm fills one toward the phone;
 * udptl_in/out move the UDPTL datagrams the backend's socket carries. V.34 fax
 * is out of scope and stays G.711 passthrough.
 */
#ifndef UDSP_FAX_H
#define UDSP_FAX_H

#include <stddef.h>
#include <stdint.h>

struct udsp_fax;

struct udsp_fax_ops {
	const char *name;
	struct udsp_fax *(*create)(int max_bit_rate, int max_datagram, int ecc);
	void (*destroy)(struct udsp_fax *f);
	void (*rx_pcm)(struct udsp_fax *f, const int16_t *pcm);
	void (*tx_pcm)(struct udsp_fax *f, int16_t *pcm);
	int (*udptl_in)(struct udsp_fax *f, const uint8_t *pkt, size_t len);
	int (*udptl_out)(struct udsp_fax *f, uint8_t *pkt, size_t cap);
};

#define UDSP_FAX_ABI_VERSION	1
struct udsp_fax_descriptor {
	uint32_t abi_version;			/* == UDSP_FAX_ABI_VERSION */
	uint32_t struct_size;			/* == sizeof(struct udsp_fax_ops) */
	const struct udsp_fax_ops *ops;
};

extern const struct udsp_fax_descriptor udsp_fax_descriptor;

#endif
