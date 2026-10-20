// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * CID FSK modulation and message framing, the boundary between the CID
 * sequencer and the DSP. Stable contract: dsp/fsk_mod.c and dsp/cid.c
 * implement it and line/cidseq.c consumes it, so a change here changes both
 * sides. Treat this file like the include/udsp headers.
 *
 * The state structs are fixed-size opaque arenas so the consumer embeds them
 * without seeing the implementation: the implementing .c defines its real
 * struct privately and _Static_asserts that it fits. uint64_t alignment.
 *
 * Threading: *_init and *_data run on the control path; *_run runs on the
 * engine tick (RT: no allocation, no locking, no logging; handoff coding
 * rules). One instance serves one line at a time.
 */
#ifndef UDSP_CID_H
#define UDSP_CID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "udsp/udsp.h"

#define UDSP_CID_MSG_MAX	256U	/* framed message, checksum included */

enum udsp_fsk_std {
	UDSP_FSK_BELL202 = 0,	/* mark 1200 Hz, space 2200 Hz, 1200 baud */
	UDSP_FSK_V23,		/* mark 1300 Hz, space 2100 Hz, 1200 baud */
};

struct udsp_fsk_mod { uint64_t opaque[96]; };

/* seizure_bits alternating 0/1 then mark_bits of mark precede the data; pass 0
 * for a format without that phase (ETSI after DTAS, type 2). Data bytes go out
 * 8N1 LSB-first (start 0, stop 1). level_dbm0 <= 0. rate_hz is the line rate
 * the modulator runs at, which sets the phase step and the baud clock. */
void udsp_fsk_mod_init(struct udsp_fsk_mod *m, enum udsp_fsk_std std,
		       int level_dbm0, unsigned seizure_bits, unsigned mark_bits,
		       unsigned rate_hz);
/* Queue message bytes; callable until the first run. Returns bytes taken. */
size_t udsp_fsk_mod_data(struct udsp_fsk_mod *m, const uint8_t *bytes, size_t len);
/* Add one frame of n samples of modulated signal into frame. The add
 * saturates, so the caller mixes. n is the line's frame count, rate_hz/100.
 * Returns false once the burst finished; frame is then untouched. */
bool udsp_fsk_mod_run(struct udsp_fsk_mod *m, int16_t *frame, unsigned n);

/* Render one CID payload into an FSK message (MDMF/SDMF or the NTT frame,
 * chosen by fmt), checksum included, ready for udsp_fsk_mod_data. type2 picks
 * the call-waiting variant where the format has one. datetime empty = caller
 * already filled per udsp_cid contract. Returns the byte count or 0 when the
 * format is not FSK-framed (DTMF formats). Call setup only: cid->mwi never
 * selects the payload here, it stays transport context for the sequencer. A
 * VMWI message comes from udsp_cid_vmwi_build below. */
size_t udsp_cid_msg_build(const struct udsp_cid *cid, enum udsp_cid_format fmt,
			  bool type2, uint8_t *out, size_t cap);

/* Render a VMWI message: on/off is explicit because struct udsp_cid carries
 * no off state. MDMF-family formats render type 0x82 with the 0x0B visual
 * indicator (0xFF on, 0x00 off) plus the date when datetime is non-empty; the
 * Bellcore format renders the SDMF type 0x06 body; DTMF formats return 0.
 * datetime as in udsp_cid. */
size_t udsp_cid_vmwi_build(bool on, const char *datetime,
			   enum udsp_cid_format fmt, uint8_t *out, size_t cap);

/* Render the DTMF-format CID digit string (start/stop markers per fmt) for
 * playout through the DTMF generator. Returns digits written or 0 when fmt
 * is not a DTMF format. */
size_t udsp_cid_dtmf_string(const struct udsp_cid *cid, enum udsp_cid_format fmt,
			    char *out, size_t cap);

#endif
