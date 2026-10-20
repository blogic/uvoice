// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The event path of the udsp backend, which carries no code.
 *
 * libudsp writes struct udsp_event records straight to the socketpair write
 * end that open() handed it, and struct hal_event is the same record: the same
 * size, the same header fields at the same offsets, the same union members and
 * the same enum values (event.h header comment, hal.h section 2). The core
 * therefore reads exactly what the library wrote, and this backend runs no
 * pump thread, allocates no queue and translates nothing at run time.
 *
 * That identity is the contract this file enforces. Everything below is a
 * static assert, one per field and one per enum member by name, so a field
 * that moves on either side is a build failure here rather than a hook edge
 * that arrives as a digit. The two records are kept as separate types on
 * purpose: libudsp carries no libubox dependency and uvoice carries no libudsp
 * types in its core.
 *
 *
 * The fault consumer (event.h UDSP_EV_SLIC_FAULT)
 *
 * UDSP_FAULT_ENGINE_STALL says the real time tick stopped advancing, so the
 * daemon exits for the procd respawn rather than serve a line with no audio.
 * The record reaches the core without passing through this backend, so the
 * backend cannot latch on it.
 *
 * Two paths carry the stall and both are live.
 *
 * The return code path is one: libudsp latches the fault before
 * it emits, every op after the latch answers UDSP_FATAL (core udsp_guard), the
 * backend maps that to HAL_FATAL, and line/line.c forces the line out of
 * service on a failed detector arm while main.c dsp_start_fail() exits the
 * daemon on a bring-up that completes anything other than HAL_OK.
 *
 * The direct arm is the other, and it does not wait for the next op.
 * event_dispatch.c routes HAL_EVENT_SLIC_FAULT to its own handler. That
 * handler reads u.fault.code, exits for the procd respawn on the stall bit and
 * reports every other bit. The bit values it reads are pinned below and
 * nowhere else. hal.h carries no fault enum, so event_dispatch.c spells each
 * bit it decides on as a literal with a reference to this file. The note under
 * the asserts says why.
 *
 * Two more library bits reach that handler, and one record carries no bit at
 * all. UDSP_FAULT_DEVICE_RESET says the SLIC device needs a reset pulse and a
 * full init. Only a fresh udsp_open performs that, so the handler takes the
 * stall exit for it. UDSP_FAULT_SLIC_CLOCK is an ordinary per line fault: the
 * line goes out of service and its clear brings it back. A code of 0 is that
 * clear (event.h, the rule under enum udsp_fault_bit). The handler logs it and
 * hands the record to the line shell. The shell drops the fault reason and
 * derives the service state again.
 *
 * The backend owns one bit that the library never sets. A dead-marked stream
 * strands its local port until udsp_close, so udsp.c emits bit 16 on its own
 * seq-0 fault record. event_dispatch.c reads that bit and exits for the procd
 * respawn, which lets stream_pool_shutdown release the library's descriptors.
 */
#include <stddef.h>

#include "udsp_int.h"

/* --- the record ---------------------------------------------------------- */

_Static_assert(sizeof(struct hal_event) == sizeof(struct udsp_event),
	       "the hal and udsp event records are not the same size");
_Static_assert(_Alignof(struct hal_event) == _Alignof(struct udsp_event),
	       "the hal and udsp event records are not aligned alike");

/*
 * Offset, width and type. The first two alone would let a field keep its place
 * and change meaning: an int8_t status that became a uint8_t, or a char digit
 * that became a uint8_t, reads back wrong for exactly the values that matter
 * (a negative cid_sent.status, a signed char) while every arithmetic assert
 * still passes. __builtin_types_compatible_p is a GCC and clang builtin and
 * both toolchains this daemon builds with carry it.
 */
#define ABI_FIELD(field, msg)						\
	_Static_assert(offsetof(struct hal_event, field) ==		\
		       offsetof(struct udsp_event, field), msg);	\
	_Static_assert(sizeof(((struct hal_event *)0)->field) ==		\
		       sizeof(((struct udsp_event *)0)->field), msg);	\
	_Static_assert(__builtin_types_compatible_p(			\
			       __typeof__(((struct hal_event *)0)->field),	\
			       __typeof__(((struct udsp_event *)0)->field)),	\
		       msg " (type)")

/* the header, read by main.c event_read, event_seq_check and event_dispatch */
ABI_FIELD(type, "event type moved");
ABI_FIELD(line, "event line moved");
ABI_FIELD(stream, "event stream moved");
ABI_FIELD(seq, "event seq moved");
ABI_FIELD(stamp_ms, "event stamp moved");

/* every union member the two contracts define, each field by name */
ABI_FIELD(u.hook.state, "hook state moved");
ABI_FIELD(u.hook.pulses, "hook pulses moved");
ABI_FIELD(u.hook.pulse_invert, "hook pulse coding moved");
ABI_FIELD(u.digit.digit, "digit char moved");
ABI_FIELD(u.digit.remote, "digit remote flag moved");
ABI_FIELD(u.digit.duration_ms, "digit duration moved");
ABI_FIELD(u.tone.code, "tone code moved");
ABI_FIELD(u.tone.direction, "tone direction moved");
ABI_FIELD(u.tone.end, "tone end flag moved");
ABI_FIELD(u.tone.duration_ms, "tone duration moved");
ABI_FIELD(u.line.state, "line state moved");
ABI_FIELD(u.line.ring_count, "line ring count moved");
ABI_FIELD(u.cid.number, "cid number moved");
ABI_FIELD(u.cid.name, "cid name moved");
ABI_FIELD(u.cid.datetime, "cid datetime moved");
ABI_FIELD(u.cid_sent.kind, "cid sent kind moved");
ABI_FIELD(u.cid_sent.status, "cid sent status moved");
ABI_FIELD(u.jb.total, "jb total moved");
ABI_FIELD(u.jb.drop, "jb drop moved");
ABI_FIELD(u.jb.size_ms, "jb size moved");
ABI_FIELD(u.jb.jitter_avg, "jb jitter moved");
ABI_FIELD(u.t38.state, "t38 state moved");
ABI_FIELD(u.t38.payload_type, "t38 payload type moved");
ABI_FIELD(u.fault.code, "fault code moved");
ABI_FIELD(u.job.token, "job token moved");
ABI_FIELD(u.job.status, "job status moved");
ABI_FIELD(u.lec.kind, "lec policy kind moved");
ABI_FIELD(u.lec.reason, "lec policy reason moved");
ABI_FIELD(u.lec.tone, "lec policy tone moved");
ABI_FIELD(u.lec.direction, "lec policy direction moved");

/* --- the event types ----------------------------------------------------- */

#define ABI_ENUM(hal_name, udsp_name)					\
	_Static_assert((int)hal_name == (int)udsp_name,			\
		       #hal_name " and " #udsp_name " disagree")

ABI_ENUM(HAL_EVENT_NONE, UDSP_EV_NONE);
ABI_ENUM(HAL_EVENT_HOOK, UDSP_EV_HOOK);
ABI_ENUM(HAL_EVENT_DIGIT, UDSP_EV_DIGIT);
ABI_ENUM(HAL_EVENT_TONE, UDSP_EV_TONE);
ABI_ENUM(HAL_EVENT_LINE, UDSP_EV_LINE);
ABI_ENUM(HAL_EVENT_RING_START, UDSP_EV_RING_START);
ABI_ENUM(HAL_EVENT_RING_END, UDSP_EV_RING_END);
ABI_ENUM(HAL_EVENT_CID_SENT, UDSP_EV_CID_SENT);
ABI_ENUM(HAL_EVENT_CID_RECV, UDSP_EV_CID_RECV);
ABI_ENUM(HAL_EVENT_CID2_TIMEOUT, UDSP_EV_CID2_TIMEOUT);
ABI_ENUM(HAL_EVENT_RTP_FIRST_RX, UDSP_EV_RTP_FIRST_RX);
ABI_ENUM(HAL_EVENT_RTP_FIRST_TX, UDSP_EV_RTP_FIRST_TX);
ABI_ENUM(HAL_EVENT_JB_UPDATE, UDSP_EV_JB_UPDATE);
ABI_ENUM(HAL_EVENT_T38, UDSP_EV_T38);
ABI_ENUM(HAL_EVENT_SLIC_FAULT, UDSP_EV_SLIC_FAULT);
ABI_ENUM(HAL_EVENT_AUDIO_END, UDSP_EV_AUDIO_END);
ABI_ENUM(HAL_EVENT_JOB_DONE, UDSP_EV_JOB_DONE);
ABI_ENUM(HAL_EVENT_LEC_POLICY, UDSP_EV_LEC_POLICY);

/* --- the payload codes --------------------------------------------------- */

ABI_ENUM(HAL_HOOK_SEIZE, UDSP_HOOK_SEIZE);
ABI_ENUM(HAL_HOOK_RELEASE, UDSP_HOOK_RELEASE);
ABI_ENUM(HAL_HOOK_FLASH, UDSP_HOOK_FLASH);
ABI_ENUM(HAL_HOOK_PULSE, UDSP_HOOK_PULSE);
ABI_ENUM(HAL_HOOK_ERROR, UDSP_HOOK_ERROR);

ABI_ENUM(HAL_TONE_DET_OTHER, UDSP_TONE_DET_OTHER);
ABI_ENUM(HAL_TONE_DET_CNG, UDSP_TONE_DET_CNG);
ABI_ENUM(HAL_TONE_DET_CED, UDSP_TONE_DET_CED);
ABI_ENUM(HAL_TONE_DET_ANS, UDSP_TONE_DET_ANS);
ABI_ENUM(HAL_TONE_DET_ANSAM, UDSP_TONE_DET_ANSAM);
ABI_ENUM(HAL_TONE_DET_ANSBAR, UDSP_TONE_DET_ANSBAR);
ABI_ENUM(HAL_TONE_DET_V21_PREAMBLE, UDSP_TONE_DET_V21_PREAMBLE);
ABI_ENUM(HAL_TONE_DET_FAX_CM, UDSP_TONE_DET_FAX_CM);
ABI_ENUM(HAL_TONE_DET_FAX_TIMEOUT, UDSP_TONE_DET_FAX_TIMEOUT);
ABI_ENUM(HAL_TONE_DET_MODEM, UDSP_TONE_DET_MODEM);

ABI_ENUM(HAL_TONE_DIR_LOCAL, UDSP_TONE_DIR_LOCAL);
ABI_ENUM(HAL_TONE_DIR_REMOTE, UDSP_TONE_DIR_REMOTE);

/* The reason of a HAL_EVENT_LEC_POLICY record. Its two other coded fields
 * borrow pairs pinned elsewhere in this file: the direction is the enum
 * hal_tone_dir of the two asserts above, and the kind is the enum
 * hal_passthrough of the media record below. Both are read here as well, so
 * the trace of a policy move and the policy itself cannot drift apart. */
ABI_ENUM(HAL_LEC_REASON_LATCH, UDSP_LEC_REASON_LATCH);
ABI_ENUM(HAL_LEC_REASON_DEMOTE, UDSP_LEC_REASON_DEMOTE);
ABI_ENUM(HAL_LEC_REASON_RELEASE_QUIET, UDSP_LEC_REASON_RELEASE_QUIET);
ABI_ENUM(HAL_LEC_REASON_RELEASE_TOPOLOGY, UDSP_LEC_REASON_RELEASE_TOPOLOGY);
ABI_ENUM(HAL_LEC_REASON_RELEASE_CALLER, UDSP_LEC_REASON_RELEASE_CALLER);

/* The line state of an unsolicited HAL_EVENT_LINE is the same enum the
 * line_state op takes, and the record carries it as a raw byte. */
ABI_ENUM(HAL_LINE_DOWN, UDSP_LINE_DOWN);
ABI_ENUM(HAL_LINE_ACTIVE, UDSP_LINE_ACTIVE);
ABI_ENUM(HAL_LINE_OHT, UDSP_LINE_OHT);
ABI_ENUM(HAL_LINE_BUSY, UDSP_LINE_BUSY);
ABI_ENUM(HAL_LINE_SLEEP, UDSP_LINE_SLEEP);
ABI_ENUM(HAL_LINE_HOWLER, UDSP_LINE_HOWLER);

/* The completion status travels as a raw int32 and the core casts it to
 * enum hal_status, so the four values have to agree as well. */
ABI_ENUM(HAL_OK, UDSP_OK);
ABI_ENUM(HAL_TRANSIENT, UDSP_TRANSIENT);
ABI_ENUM(HAL_UNSUPPORTED, UDSP_UNSUPPORTED);
ABI_ENUM(HAL_FATAL, UDSP_FATAL);

/*
 * The two payloads hal.h does not name.
 *
 * u.fault.code is a bit mask and hal.h carries no enum for it at all: hal.h
 * names it an opaque producer word, so the core reads it as an opaque number.
 * libudsp does name its bits (enum udsp_fault_bit), and this backend's whole
 * engine-stall story rests on one of them, so the values are pinned here
 * against the library's own enum rather than against a hal enum that does not
 * exist. The HAL_EVENT_SLIC_FAULT arm of event_dispatch.c therefore spells
 * each bit it decides on as a literal, and the asserts below are what pin
 * them.
 */
_Static_assert(UDSP_FAULT_ENGINE_STALL == (1u << 0), "the engine stall bit moved");
_Static_assert(UDSP_FAULT_SLIC_THERMAL == (1u << 1), "the thermal fault bit moved");
_Static_assert(UDSP_FAULT_SLIC_DC == (1u << 2), "the dc fault bit moved");
_Static_assert(UDSP_FAULT_SLIC_PQ == (1u << 3), "the power alarm bit moved");
_Static_assert(UDSP_FAULT_SLIC_VBAT == (1u << 4), "the vbat fault bit moved");
_Static_assert(UDSP_FAULT_SLIC_LONGSTAT == (1u << 5), "the longstat fault bit moved");
_Static_assert(UDSP_FAULT_SLIC_REINIT == (1u << 6),
	       "the reinit fault bit moved");
_Static_assert(UDSP_FAULT_SLIC_DCDC == (1u << 7),
	       "the converter fault bit moved");
_Static_assert(UDSP_FAULT_SLIC_CLOCK == (1u << 9), "the clock fault bit moved");
_Static_assert(UDSP_FAULT_DEVICE_RESET == (1u << 10),
	       "the device reset bit moved");
_Static_assert((UDSP_FAULT_ENGINE_STALL | UDSP_FAULT_SLIC_THERMAL |
		UDSP_FAULT_SLIC_DC | UDSP_FAULT_SLIC_PQ |
		UDSP_FAULT_SLIC_VBAT | UDSP_FAULT_SLIC_LONGSTAT |
		UDSP_FAULT_SLIC_REINIT | UDSP_FAULT_SLIC_DCDC |
		UDSP_FAULT_SLIC_CLOCK | UDSP_FAULT_DEVICE_RESET) <
	       HAL_FAULT_STREAM_DEAD,
	       "a library fault bit reached the backend fault range");

/*
 * The Opus bandwidth ceiling, pinned twice. hal.h names the values
 * (enum hal_opus_bw) and hal_udsp_media_fill() copies the field raw, so the
 * pair is pinned by name below. The four absolute asserts stay beside it,
 * because a rename that renumbered both enums by the same step would keep
 * every pair equal and still move the bandwidth an operator configured.
 */
_Static_assert(UDSP_OPUS_BW_AUTO == 0, "the automatic Opus bandwidth moved");
_Static_assert(UDSP_OPUS_BW_NB == 1, "the narrowband Opus bandwidth moved");
_Static_assert(UDSP_OPUS_BW_MB == 2, "the medium-band Opus bandwidth moved");
_Static_assert(UDSP_OPUS_BW_WB == 3, "the wideband Opus bandwidth moved");

ABI_ENUM(HAL_OPUS_BW_AUTO, UDSP_OPUS_BW_AUTO);
ABI_ENUM(HAL_OPUS_BW_NB, UDSP_OPUS_BW_NB);
ABI_ENUM(HAL_OPUS_BW_MB, UDSP_OPUS_BW_MB);
ABI_ENUM(HAL_OPUS_BW_WB, UDSP_OPUS_BW_WB);

/*
 * u.t38.state is the second one: hal.h documents it as 0 = disconnect,
 * 1 = payload type change, and neither contract defines an enum for it, so
 * there is no pair to assert and the field travels as the raw byte both
 * sides document. The T.38 enum both contracts do name is the error
 * correction mode of the media record, and it is asserted here beside the
 * event codes so the media translation inherits a checked pair.
 */
ABI_ENUM(HAL_T38_EC_NONE, UDSP_T38_EC_NONE);
ABI_ENUM(HAL_T38_EC_REDUNDANCY, UDSP_T38_EC_REDUNDANCY);

/*
 * --- the media record ----------------------------------------------
 *
 * hal_udsp_media_fill() copies struct hal_media onto struct udsp_media field
 * by field, so every enum the two records share is pinned here and a field
 * that only looks like a pair is translated by hand in udsp.c instead. The
 * event record above is forwarded byte for byte; this one is not, because
 * libudsp takes the two descriptors beside it and the two records were never
 * declared to share a layout.
 */

ABI_ENUM(HAL_DTMF_INBAND, UDSP_DTMF_INBAND);
ABI_ENUM(HAL_DTMF_RFC2833, UDSP_DTMF_RFC2833);
ABI_ENUM(HAL_DTMF_INFO, UDSP_DTMF_INFO);

ABI_ENUM(HAL_MEDIA_INACTIVE, UDSP_MEDIA_INACTIVE);
ABI_ENUM(HAL_MEDIA_SENDONLY, UDSP_MEDIA_SENDONLY);
ABI_ENUM(HAL_MEDIA_RECVONLY, UDSP_MEDIA_RECVONLY);
ABI_ENUM(HAL_MEDIA_SENDRECV, UDSP_MEDIA_SENDRECV);

/* The playout mode, which hal_udsp_media_fill() writes and
 * hal_udsp_jb_stats_fill() reads back raw. A pair that disagreed would report
 * a fixed buffer as adaptive on the status surface. */
ABI_ENUM(HAL_JB_ADAPTIVE, UDSP_JB_ADAPTIVE);
ABI_ENUM(HAL_JB_FIXED, UDSP_JB_FIXED);
ABI_ENUM(HAL_JB_OFF, UDSP_JB_OFF);

/* The passthrough kind, which hal_udsp_media_fill() copies raw. The kind is
 * what selects the canceller policy of the line, so a pair that disagreed
 * would run a fax exchange under the modem policy and mute half of it. */
ABI_ENUM(HAL_PASSTHROUGH_OFF, UDSP_PASSTHROUGH_OFF);
ABI_ENUM(HAL_PASSTHROUGH_MODEM, UDSP_PASSTHROUGH_MODEM);
ABI_ENUM(HAL_PASSTHROUGH_FAX, UDSP_PASSTHROUGH_FAX);

/* The suite value decides whether the stream is encrypted at all. An SRTP
 * engine can fail open on a suite it does not carry, and the stream then runs
 * with the encryption off and no error (hal.h enum hal_srtp_suite). A
 * renumbering that turned _80 into _32 would halve the authentication tag
 * with no error, so the three members are pinned by name. */
ABI_ENUM(HAL_SRTP_NONE, UDSP_SRTP_NONE);
ABI_ENUM(HAL_SRTP_AES_CM_128_SHA1_80, UDSP_SRTP_AES_CM_128_SHA1_80);
ABI_ENUM(HAL_SRTP_AES_CM_128_SHA1_32, UDSP_SRTP_AES_CM_128_SHA1_32);

/* udsp_conf_set casts the mode onto the library enum, so a member that moved
 * on one side would ask the library for the wrong mix. */
ABI_ENUM(HAL_CONF_OFF, UDSP_CONF_OFF);
ABI_ENUM(HAL_CONF_LOCAL_MIX, UDSP_CONF_LOCAL_MIX);

/* The loop mode of the manufacturing test. line_loop_set casts the byte of
 * struct hal_loop onto the library enum, so a member that moved on one side
 * would arm the wrong mode on a line the test holds. */
ABI_ENUM(HAL_LOOP_OFF, UDSP_LOOP_OFF);
ABI_ENUM(HAL_LOOP_SELF, UDSP_LOOP_SELF);
ABI_ENUM(HAL_LOOP_PEER, UDSP_LOOP_PEER);

/*
 * The two enum pairs that do not agree, asserted as disagreements so the hand
 * written switches in udsp.c cannot be replaced by a cast on a later reading.
 *
 * The codec selector agrees for the first four members and diverges after
 * them: hal.h numbers G726_32 fourth and T38 fifth, while libudsp carries four
 * G.726 rates and puts T38 last. The four that agree are pinned by name, and
 * the rest travel through hal_udsp_codec_sel().
 */
ABI_ENUM(HAL_CODEC_NONE, UDSP_SEL_NONE);
ABI_ENUM(HAL_CODEC_PCMU, UDSP_SEL_PCMU);
ABI_ENUM(HAL_CODEC_PCMA, UDSP_SEL_PCMA);
ABI_ENUM(HAL_CODEC_G729, UDSP_SEL_G729);
_Static_assert((int)HAL_CODEC_G726_32 != (int)UDSP_SEL_G726_32,
	       "the codec selectors now agree on G726_32; hal_udsp_codec_sel "
	       "may become a cast and this assert must go");
_Static_assert((int)HAL_CODEC_T38 != (int)UDSP_SEL_T38,
	       "the codec selectors now agree on T38; hal_udsp_codec_sel may "
	       "become a cast and this assert must go");
_Static_assert((int)HAL_CODEC_G722 != (int)UDSP_SEL_G722,
	       "the codec selectors now agree on G722; hal_udsp_codec_sel may "
	       "become a cast and this assert must go");
_Static_assert((int)HAL_CODEC_OPUS != (int)UDSP_SEL_OPUS,
	       "the codec selectors now agree on OPUS; hal_udsp_codec_sel may "
	       "become a cast and this assert must go");

/*
 * The fax mode agrees on off and passthrough. hal.h carries a V.152 VBD mode
 * libudsp names no member for at all, which pushes its T38 member one place
 * further along, so those two travel through hal_udsp_fax_mode().
 */
ABI_ENUM(HAL_FAX_OFF, UDSP_FAX_OFF);
ABI_ENUM(HAL_FAX_PASSTHROUGH, UDSP_FAX_PASSTHROUGH);
_Static_assert((int)HAL_FAX_T38 != (int)UDSP_FAX_T38,
	       "the fax modes now agree on T38; hal_udsp_fax_mode may become a "
	       "cast and this assert must go");

/* The four copies hal_udsp_media_fill makes with memcpy. A width that moved on
 * one side alone would truncate an address, a key or an interface name. */
_Static_assert(sizeof(((struct hal_media *)0)->local.addr) ==
	       sizeof(((struct udsp_media *)0)->local.addr),
	       "the media address widths differ");
_Static_assert(HAL_IFNAME_MAX == UDSP_IFNAME_MAX,
	       "the media bind device widths differ");
_Static_assert(sizeof(((struct hal_media *)0)->srtp.key_tx) ==
	       sizeof(((struct udsp_media *)0)->srtp.key_tx) &&
	       sizeof(((struct hal_media *)0)->srtp.key_rx) ==
	       sizeof(((struct udsp_media *)0)->srtp.key_rx),
	       "the SDES key widths differ");
/* RFC 4568 6.1: the 16 byte master key followed by its 14 byte salt, raw. */
_Static_assert(sizeof(((struct hal_media *)0)->srtp.key_tx) == 30,
	       "the SDES key is no longer the 30 byte AES_CM_128 concatenation");
