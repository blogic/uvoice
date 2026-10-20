// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * A deterministic fake T.38 gateway for the tests. Two modules include it:
 * test_media.c, which installs the vtable through udsp_fax_override and reads
 * the record below, and test/fax_plugin_fake.c, which exports the same vtable
 * as a real shared object for the loader and the unload order. One
 * implementation, so the two can never drift apart.
 *
 * What it does. rx_pcm keeps the frame it was given and counts it; tx_pcm
 * writes a 1600 Hz tone, which is five samples per cycle at 8000 Hz and needs
 * no library of its own; udptl_out hands out one datagram per frame it took,
 * carrying that frame count in four network-order bytes; udptl_in records the
 * same four bytes in arrival order. The record is the module's own, so the
 * copy inside a loaded shared object counts that copy's calls.
 */
#ifndef UDSP_TEST_FAX_FAKE_H
#define UDSP_TEST_FAX_FAKE_H

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "udsp/fax.h"

/* One 8 kHz frame, the whole contract of udsp/fax.h. */
#define FAX_FAKE_FRAME		80u
#define FAX_FAKE_TONE_HZ	1600.0
#define FAX_FAKE_CYCLE		5u
#define FAX_FAKE_DGRAM		4u
/* The ring depth of the shipped plug-in, so a stream that cannot send for a
 * while offers the same backlog the engine's own send bound expects. */
#define FAX_FAKE_RING		8u
/* The IPv4 minimum reassembly buffer, which is the bound the library clamps a
 * datagram size to. */
#define FAX_FAKE_DGRAM_MAX	576u
#define FAX_FAKE_SEQ_MAX	64u

struct fax_fake_stats {
	unsigned creates;
	unsigned destroys;
	unsigned rx_frames;
	unsigned tx_frames;
	unsigned in_count;
	unsigned out_count;
	unsigned in_errors;
	/* What create was given, so a test reads the values media_prepare
	 * settled instead of the ones it passed. */
	int bit_rate;
	int datagram;
	int ecc;
	int16_t rx_last[FAX_FAKE_FRAME];
	unsigned in_seq[FAX_FAKE_SEQ_MAX];
	unsigned in_seq_len;
};

struct udsp_fax {
	unsigned frames;
	unsigned pending;
	unsigned phase;
};

static struct fax_fake_stats fax_fake_record;

static inline struct fax_fake_stats *fax_fake_stats_get(void)
{
	return &fax_fake_record;
}

static inline void fax_fake_reset(void)
{
	memset(&fax_fake_record, 0, sizeof(fax_fake_record));
}

static inline struct udsp_fax *fax_fake_create(int max_bit_rate,
					       int max_datagram, int ecc)
{
	struct udsp_fax *f = calloc(1, sizeof(*f));

	if (!f)
		return NULL;
	fax_fake_record.creates++;
	fax_fake_record.bit_rate = max_bit_rate;
	fax_fake_record.datagram = max_datagram;
	fax_fake_record.ecc = ecc;

	return f;
}

static inline void fax_fake_destroy(struct udsp_fax *f)
{
	fax_fake_record.destroys++;
	free(f);
}

static inline void fax_fake_rx_pcm(struct udsp_fax *f, const int16_t *pcm)
{
	f->frames++;
	if (f->pending < FAX_FAKE_RING)
		f->pending++;
	fax_fake_record.rx_frames++;
	memcpy(fax_fake_record.rx_last, pcm, sizeof(fax_fake_record.rx_last));
}

/* sin(2 * pi * k / 5) at an amplitude of 8000, so the tone is exact at 8000 Hz
 * and stays inside the passband of the interpolation. */
static inline void fax_fake_tx_pcm(struct udsp_fax *f, int16_t *pcm)
{
	static const int16_t step[FAX_FAKE_CYCLE] = {
		0, 7608, 4702, -4702, -7608,
	};

	for (unsigned i = 0; i < FAX_FAKE_FRAME; i++) {
		pcm[i] = step[f->phase];
		f->phase = (f->phase + 1u) % FAX_FAKE_CYCLE;
	}
	fax_fake_record.tx_frames++;
}

static inline int fax_fake_udptl_in(struct udsp_fax *f, const uint8_t *pkt,
				    size_t len)
{
	unsigned v;

	if (len < FAX_FAKE_DGRAM || len > FAX_FAKE_DGRAM_MAX) {
		fax_fake_record.in_errors++;
		return -EMSGSIZE;
	}
	v = ((unsigned)pkt[0] << 24) | ((unsigned)pkt[1] << 16) |
	    ((unsigned)pkt[2] << 8) | pkt[3];
	if (fax_fake_record.in_seq_len < FAX_FAKE_SEQ_MAX)
		fax_fake_record.in_seq[fax_fake_record.in_seq_len++] = v;
	fax_fake_record.in_count++;

	return 0;
}

static inline int fax_fake_udptl_out(struct udsp_fax *f, uint8_t *pkt,
				     size_t cap)
{
	unsigned v = f->frames;

	if (!f->pending)
		return 0;
	if (cap < FAX_FAKE_DGRAM)
		return -EMSGSIZE;
	pkt[0] = (uint8_t)(v >> 24);
	pkt[1] = (uint8_t)(v >> 16);
	pkt[2] = (uint8_t)(v >> 8);
	pkt[3] = (uint8_t)v;
	f->pending--;
	fax_fake_record.out_count++;

	return (int)FAX_FAKE_DGRAM;
}

static const struct udsp_fax_ops fax_fake_ops = {
	.name = "fake",
	.create = fax_fake_create,
	.destroy = fax_fake_destroy,
	.rx_pcm = fax_fake_rx_pcm,
	.tx_pcm = fax_fake_tx_pcm,
	.udptl_in = fax_fake_udptl_in,
	.udptl_out = fax_fake_udptl_out,
};

#endif
