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
 * udsp event record. Stable contract: the hal/udsp backend pins the field
 * correspondence, so a change here changes uvoice.
 *
 * The record is fixed-size POD, one per SOCK_SEQPACKET datagram, and is
 * identical in meaning to uvoice struct hal_event: the hal/udsp backend
 * translates one udsp_event into one hal_event with no information added or
 * lost. The two structs are kept separate so libudsp carries no libubox
 * dependency; a static_assert in the backend pins the field correspondence.
 *
 * Single emitter. One thread inside libudsp owns every write to the event
 * fd. The engine, net and slic threads never write the fd; they post
 * a filled udsp_event to an internal MPSC queue, and the emitter thread assigns
 * seq and sends. An atomic seq alone does not preserve socket write order
 * across producers, and uvoice main.c event_seq_check() then logs false gaps
 * and inflates events_lost. seq is per-run monotonic from 1 and wraps in 16
 * bits; a JOB_DONE completion carries seq 0 and is outside seq accounting, as
 * in hal.h section 2. On a full fd the emitter drops a hardware event and
 * counts it (never a completion).
 */
#ifndef UDSP_EVENT_H
#define UDSP_EVENT_H

#include <stddef.h>
#include <stdint.h>

#define UDSP_CID_STR_MAX	33
#define UDSP_EVENT_MAX_BYTES	160

enum udsp_event_type {
	UDSP_EV_NONE = 0,
	UDSP_EV_HOOK,		/* seize, release, flash, pulse */
	UDSP_EV_DIGIT,		/* DTMF, local or from the wire, duration at end */
	UDSP_EV_TONE,		/* non-DTMF detected tone, carries direction */
	UDSP_EV_LINE,		/* line state changed under us */
	UDSP_EV_RING_START,
	UDSP_EV_RING_END,
	UDSP_EV_CID_SENT,
	UDSP_EV_CID_RECV,
	UDSP_EV_CID2_TIMEOUT,
	UDSP_EV_RTP_FIRST_RX,
	UDSP_EV_RTP_FIRST_TX,
	UDSP_EV_JB_UPDATE,
	UDSP_EV_T38,
	UDSP_EV_SLIC_FAULT,	/* SLIC fault, engine stall, or a clear (code 0) */
	UDSP_EV_AUDIO_END,
	UDSP_EV_JOB_DONE,	/* completion of a SLOW op, carries a token */
	UDSP_EV_LEC_POLICY,	/* the tone disabler moved the canceller policy */
};

enum udsp_hook {
	UDSP_HOOK_SEIZE = 0,
	UDSP_HOOK_RELEASE,
	UDSP_HOOK_FLASH,
	UDSP_HOOK_PULSE,	/* u.hook.pulses carries the pulse count, 1..20 */
	UDSP_HOOK_ERROR,
};

enum udsp_tone_detect {
	UDSP_TONE_DET_OTHER = 0,
	UDSP_TONE_DET_CNG,
	UDSP_TONE_DET_CED,
	UDSP_TONE_DET_ANS,
	UDSP_TONE_DET_ANSAM,
	UDSP_TONE_DET_ANSBAR,
	UDSP_TONE_DET_V21_PREAMBLE,
	UDSP_TONE_DET_FAX_CM,
	UDSP_TONE_DET_FAX_TIMEOUT,
	UDSP_TONE_DET_MODEM,
};

enum udsp_tone_dir {
	UDSP_TONE_DIR_LOCAL = 0,	/* from the phone */
	UDSP_TONE_DIR_REMOTE,		/* from the wire */
};

/*
 * Why the tone disabler moved the canceller policy of a line
 * (UDSP_EV_LEC_POLICY, core/chan.c). LATCH is a kind the disabler took where it
 * held none, or a stronger kind a later answer tone asked for. DEMOTE is the
 * one weaker kind a standing latch admits: a V.21 preamble takes a modem latch
 * to the fax kind. RELEASE_QUIET follows a quiet holding band (G.168 7.8).
 * RELEASE_TOPOLOGY follows a change in the governing stream count.
 * RELEASE_CALLER follows udsp_line_lec_latch_clear on a standing latch.
 */
enum udsp_lec_reason {
	UDSP_LEC_REASON_LATCH = 0,
	UDSP_LEC_REASON_DEMOTE,
	UDSP_LEC_REASON_RELEASE_QUIET,
	UDSP_LEC_REASON_RELEASE_TOPOLOGY,
	UDSP_LEC_REASON_RELEASE_CALLER,
};

/* UDSP_EV_SLIC_FAULT source bits, carried in u.fault.code. Engine stall is a
 * distinct bit so the backend fails HAL_FATAL and exits for procd.
 * A SLIC fault sets one or more of the SLIC bits; the core does not decode
 * them, and the granularity serves diagnostics.
 *
 * A code of 0 is a CLEAR: the line's fault bits read clean, no fault hold or
 * recovery is in force, its base feed is a powered state (never the open feed
 * a DOWN or safed line holds) and that feed is confirmed on the line. It is
 * posted once per fault, only on a line that reported a fault since the
 * engine opened, and it survives a stop and start in between. A consumer
 * that took the line out of service on the fault puts it back on the clear.
 *
 * Bit 8 is reserved, and no library bit takes it, so a consumer that reads
 * bit 8 as the uvoice stream-dead bit cannot misread a library fault. The
 * uvoice backend's own stream-dead bit is bit 16. */
enum udsp_fault_bit {
	UDSP_FAULT_ENGINE_STALL	= (1u << 0),	/* the RT tick stopped advancing */
	UDSP_FAULT_SLIC_THERMAL	= (1u << 1),
	UDSP_FAULT_SLIC_DC	= (1u << 2),
	UDSP_FAULT_SLIC_PQ	= (1u << 3),	/* PQ1..PQ6 power alarm */
	UDSP_FAULT_SLIC_VBAT	= (1u << 4),
	UDSP_FAULT_SLIC_LONGSTAT= (1u << 5),
	/* The plug-in's latched verdict: the part needs a reinit before the
	 * line is trustworthy. Stands until the reinit op succeeds; the
	 * engine holds the feed open meanwhile. */
	UDSP_FAULT_SLIC_REINIT	= (1u << 6),
	/* The DC-DC converter is powered down (PD_DCDC bit 20). Follows the
	 * hardware: it drops when the converter is up again. */
	UDSP_FAULT_SLIC_DCDC	= (1u << 7),
	/* The device's master status reads a clock fault: after the sticky
	 * fault bits were cleared and re-read twice, the live status bits
	 * (MSTRSTAT [4:0]: PCLK, FSYNC and PLL presence and lock) do not read
	 * the healthy value 0x1F. Reported on every line of the device and
	 * held; the clear (code 0) follows when the live bits read healthy
	 * again. */
	UDSP_FAULT_SLIC_CLOCK	= (1u << 9),
	/* Terminal. The live status bits read healthy 0x1F but the sticky
	 * fault bits (MSTRSTAT [7:5]) survived two write-1-to-clear verifies:
	 * the device needs a reset pulse and a full init, which only a fresh
	 * udsp_open does. Reported on line 0; never with CLOCK. */
	UDSP_FAULT_DEVICE_RESET	= (1u << 10),
};

struct udsp_event {
	uint16_t type;		/* enum udsp_event_type */
	uint16_t line;
	uint16_t stream;
	uint16_t seq;		/* from 1, wraps; 0 on JOB_DONE */
	uint32_t stamp_ms;	/* monotonic within one run */
	union {
		/*
		 * pulse_invert is the decadic coding of the region that counted
		 * a UDSP_HOOK_PULSE train. It is 1 when that region codes a
		 * dialed N as 10 - N pulses, else 0, and 0 on every other hook
		 * state. A region swap can complete while a counted train is
		 * still in the queue. The caller therefore maps the count with
		 * this byte, not with the region view.
		 */
		struct {
			uint8_t state;
			uint8_t pulses;
			uint8_t pulse_invert;
		} hook;
		struct { char digit; uint8_t remote; uint16_t duration_ms; } digit;
		/* end is 0 on a start row and 1 on an end row. */
		struct {
			uint16_t code;
			uint8_t direction;
			uint8_t end;
			uint16_t duration_ms;
		} tone;
		struct { uint8_t state; uint16_t ring_count; } line;
		struct {
			char number[UDSP_CID_STR_MAX];
			char name[UDSP_CID_STR_MAX];
			char datetime[UDSP_CID_STR_MAX];
		} cid;
		struct { uint8_t kind; int8_t status; } cid_sent;
		struct {
			uint64_t total;
			uint64_t drop;
			uint32_t size_ms;
			uint32_t jitter_avg;
		} jb;
		struct { uint8_t state; uint8_t payload_type; } t38;
		struct { uint32_t code; } fault;	/* enum udsp_fault_bit mask; 0 clears */
		struct { uint32_t token; int32_t status; } job;
		/*
		 * kind is enum udsp_passthrough (udsp.h) as the LATCH holds it
		 * after the move, and reason is enum udsp_lec_reason. It is the
		 * disabler's ask and not always the line's effective policy: a
		 * committed stream description wins over the latch
		 * (core/chan.c chan_passthrough_get), so a line whose call
		 * control named a kind keeps that kind whatever this record
		 * says.
		 *
		 * tone and direction describe the row that moved the policy:
		 * the enum udsp_tone_detect the detector classified and the
		 * enum udsp_tone_dir feed it arrived on, which is the same pair
		 * the u.tone row of that detection carries. They are meaningful
		 * on a LATCH and a DEMOTE alone. A release answers no tone, so
		 * both read 0 there, and a reader must take the reason first:
		 * direction 0 is also UDSP_TONE_DIR_LOCAL.
		 */
		struct {
			uint8_t kind;
			uint8_t reason;
			uint16_t tone;
			uint8_t direction;
		} lec;
	} u;
};

_Static_assert(offsetof(struct udsp_event, u.tone.code) ==
	       offsetof(struct udsp_event, u) &&
	       offsetof(struct udsp_event, u.tone.direction) ==
	       offsetof(struct udsp_event, u) + 2 &&
	       offsetof(struct udsp_event, u.tone.end) ==
	       offsetof(struct udsp_event, u) + 3 &&
	       offsetof(struct udsp_event, u.tone.duration_ms) ==
	       offsetof(struct udsp_event, u) + 4 &&
	       sizeof(((struct udsp_event *)0)->u.tone) == 6,
	       "udsp_event tone layout changed");

_Static_assert(offsetof(struct udsp_event, u.hook.state) ==
	       offsetof(struct udsp_event, u) &&
	       offsetof(struct udsp_event, u.hook.pulses) ==
	       offsetof(struct udsp_event, u) + 1 &&
	       offsetof(struct udsp_event, u.hook.pulse_invert) ==
	       offsetof(struct udsp_event, u) + 2 &&
	       sizeof(((struct udsp_event *)0)->u.hook) == 3,
	       "udsp_event hook layout changed");

_Static_assert(sizeof(struct udsp_event) <= UDSP_EVENT_MAX_BYTES,
	       "udsp_event outgrew its budget");

#endif
