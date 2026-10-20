// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The MGCP package tables of a residential gateway (RFC 3660 L, G and D
 * packages; the NCS 1.5 line package, Table 19) under the dialect of
 * voip.mgcp.dialect. The tables map:
 *   - a signal name to its type (OO, TO or BR), its default time-out and the
 *     host primitive that plays it;
 *   - a requested or detected event name to a compiled pattern;
 *   - a core line event (proto/proto.h struct proto_line_event) to an observed
 *     event under the dialect;
 *   - a ring signal to a host ring template;
 *   - the parameters of ci(time, number, name) to a struct proto_cid.
 *
 * Pure tables and parsers: no state, no socket, no timer. The one host
 * entry this file calls is ring_template_match, through the table the
 * caller hands in.
 *
 * Dialects. Under rfc3435 the digits and the timer T are D package events
 * and the fax and modem tones are G package events (RFC 3660 pages 7 and
 * 11). Under ncs every event and signal is in the L package (NCS Table 19,
 * page 109), and the G and D packages are unknown.
 *
 * Default package. A name without a package takes L, the default package of
 * a line endpoint. A digit, X or T name without a package takes the digit
 * package of the dialect, so "[0-9#*T](D)" reads the same under both.
 */
#ifndef UVOICE_MGCP_SIG_H
#define UVOICE_MGCP_SIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cfg/cfg.h"
#include "hal/hal.h"
#include "proto/mgcp/msg.h"
#include "proto/proto.h"

/* RFC 3660 page 14 and NCS 7.1.5 page 24: Tcrit and Tpar defaults */
#define MGCP_SIG_TCRIT_MS	4000
#define MGCP_SIG_TPAR_MS	16000

/* GR-506-CORE 14: one ring cycle of the standard cadence, 2 s on, 4 s off */
#define MGCP_SIG_RING_CYCLE_MS	6000

/* RFC 3660 page 30: a ringsplash is a 0.5 s burst of power ringing */
#define MGCP_SIG_SPLASH_MS	500

/* The length of one DTMF digit toward the phone (RFC 3660 page 12: 50 ms of
 * tone at least, then 45 ms of silence). */
#define MGCP_SIG_DIGIT_MS	100

/* The longest signal name the tables hold, "L/vmwi" plus a connection id. */
#define MGCP_SIG_NAME_MAX	48

enum mgcp_pkg {
	MGCP_PKG_NONE = 0,
	MGCP_PKG_L,
	MGCP_PKG_G,
	MGCP_PKG_D,
};

/* "L", "G", "D" or "" */
const char *mgcp_pkg_name(enum mgcp_pkg pkg);

/* The package of name under the dialect, MGCP_PKG_NONE for one the dialect
 * does not carry (518). */
enum mgcp_pkg mgcp_pkg_of(enum cfg_mgcp_dialect dialect, struct sip_str name);

/* The package the dialect reports digits and the timer T in. */
enum mgcp_pkg mgcp_pkg_digits(enum cfg_mgcp_dialect dialect);

/* The package the dialect reports ft and mt in. */
enum mgcp_pkg mgcp_pkg_tones(enum cfg_mgcp_dialect dialect);

/* ------------------------------------------------------------- signals */

enum mgcp_sig_type {
	MGCP_SIG_BR = 0,		/* brief: stops on its own */
	MGCP_SIG_TO,			/* time-out: stops on an event */
	MGCP_SIG_OO,			/* on/off: stops on request */
};

/* The host primitive that plays a signal. NONE is a signal the package
 * defines and this gateway does not generate: 513. */
enum mgcp_sig_prim {
	MGCP_PRIM_NONE = 0,
	MGCP_PRIM_TONE,			/* tone_play(arg) */
	MGCP_PRIM_RING,			/* ring_start, arg is the pattern */
	MGCP_PRIM_SPLASH,		/* ring_start for MGCP_SIG_SPLASH_MS */
	MGCP_PRIM_CW,			/* cw_alert, arg is the variant */
	MGCP_PRIM_CID,			/* ci: the caller of a ring or alert */
	MGCP_PRIM_VMWI,			/* mwi_set */
	MGCP_PRIM_OSI,			/* battery_set(DOWN), then ACTIVE */
	MGCP_PRIM_LSA,			/* battery_set(REVERSE) */
	MGCP_PRIM_DIGIT,		/* digit_play(arg) */
};

/* A signal that the phone must be off hook for: 402 on hook. */
#define MGCP_SIG_F_OFFHOOK	(1u << 0)
/* A signal that the phone must be on hook for: 401 off hook. */
#define MGCP_SIG_F_ONHOOK	(1u << 1)
/* A signal the package also defines on a connection (rt@<conn>). */
#define MGCP_SIG_F_CONN		(1u << 2)

/* The ring pattern that takes the template of the region or of the
 * distinctive ring options (rg, r0, r6, r7). */
#define MGCP_SIG_RING_DEFAULT	0xff

struct mgcp_sig_def {
	const char *name;
	uint8_t pkg;			/* enum mgcp_pkg */
	uint8_t type;			/* enum mgcp_sig_type */
	uint8_t prim;			/* enum mgcp_sig_prim */
	uint8_t arg;
	uint8_t flags;			/* MGCP_SIG_F_* */
	uint32_t to_ms;			/* TO default, 0 is infinite */
};

/*
 * The definition of pkg/name under the dialect. NULL sets *code: 518 for a
 * package the dialect does not carry, 522 for a name the package does not
 * define (RFC 3661 page 14).
 */
const struct mgcp_sig_def *mgcp_sig_lookup(enum cfg_mgcp_dialect dialect,
					   const struct mgcp_event *ev,
					   uint32_t *code);

/*
 * The parameters of one signal (RFC 3435 3.2.2.4, RFC 3660 pages 6, 28 and
 * 29). to is the TO time-out in milliseconds, rep the ring cycles of a ring
 * signal, off the "-" of an OO signal.
 */
struct mgcp_sig_params {
	uint32_t to_ms;
	uint32_t rep;
	bool to_set;
	bool rep_set;
	bool off;
};

/* false on a parameter the signal does not take or a bad value: 538. A
 * to= over LINE_TIMER_MAX_S seconds is a bad value. */
bool mgcp_sig_params_parse(const struct mgcp_sig_def *def,
			   const struct mgcp_event *ev,
			   struct mgcp_sig_params *out);

/*
 * The time-out a TO signal runs for, 0 for infinite. rep, where a ring
 * signal names it, wins over to (RFC 3660 page 28); one cycle is
 * cycle_ms, the ring template of the line, or MGCP_SIG_RING_CYCLE_MS for 0.
 */
uint32_t mgcp_sig_timeout(const struct mgcp_sig_def *def,
			  const struct mgcp_sig_params *p, uint32_t cycle_ms);

/*
 * The ring template of a ring signal: the pattern of r1 to r5 (RFC 3660 page
 * 29, the GR-506-CORE distinctive patterns), and for rg, r0, r6 and r7 the
 * template the distinctive ring options give number, 0 being the region
 * pattern.
 */
unsigned int mgcp_sig_ring_template(const struct proto_host *host,
				    unsigned int line,
				    const struct mgcp_sig_def *def,
				    const char *number);

/*
 * ci(time, number, name) into a caller id (RFC 3660 pages 25 and 26). Every
 * character of the time that is not a digit goes, so "09/14/17/26" gives
 * the MMDDHHMM datetime "09141726". A "P" field is private, an "O" field
 * is absent, and white space inside the number is dropped. A field of any
 * length, quoted or not, keeps the head that its record holds. false on a
 * list that does not hold three fields: 538.
 */
bool mgcp_sig_cid_parse(struct sip_str params, struct proto_cid *out);

/* ------------------------------------------------------------- events */

enum mgcp_evk {
	MGCP_EVK_NONE = 0,
	MGCP_EVK_HD,			/* off-hook transition */
	MGCP_EVK_HU,			/* on-hook transition */
	MGCP_EVK_HF,			/* flash hook */
	MGCP_EVK_DIGIT,			/* one DTMF or pulse digit */
	MGCP_EVK_T,			/* the interdigit timer */
	MGCP_EVK_FT,			/* fax tone */
	MGCP_EVK_MT,			/* modem tone */
	MGCP_EVK_OC,			/* operation complete */
	MGCP_EVK_OF,			/* operation failure */
};

/*
 * One observed event: the unit of the O: list, of the quarantine buffer and
 * of the match against a request. sig names the signal of an oc or of
 * report, with its package and its connection, as in "L/dl".
 */
struct mgcp_obs {
	uint8_t kind;			/* enum mgcp_evk */
	char digit;			/* MGCP_EVK_DIGIT: 0-9, *, #, A-D */
	char sig[MGCP_SIG_NAME_MAX];
};

/*
 * A requested or detect event compiled from R: or T:. digits holds one bit
 * per digit (mgcp_sig_digit_bit) for a digit set or X; timer says the set
 * names T. all is the "all" event of a package.
 */
struct mgcp_ev_pat {
	uint8_t pkg;			/* enum mgcp_pkg */
	uint8_t kind;			/* enum mgcp_evk; DIGIT for a set */
	uint16_t digits;
	bool timer;
	bool all;
};

/* The bit of a digit in struct mgcp_ev_pat digits, -1 for no digit. */
int mgcp_sig_digit_bit(char digit);

/*
 * Compile one requested or detect event name. 0, or the return code of the
 * refusal: 518 for a package the dialect does not carry, 522 for a name the
 * package does not define, 512 for an event the package defines and this
 * gateway does not detect (RFC 3661 pages 11 and 14). A connection
 * qualifier gets 512, because no event is detected on a connection, and an
 * event parameter gets 538, because no event of these packages takes one.
 */
uint32_t mgcp_ev_pat_compile(enum cfg_mgcp_dialect dialect,
			     const struct mgcp_event *ev,
			     struct mgcp_ev_pat *out);

/* Whether the pattern names the observed event. */
bool mgcp_ev_pat_match(enum cfg_mgcp_dialect dialect,
		       const struct mgcp_ev_pat *p, const struct mgcp_obs *o);

/* The enum hal_detect bits a pattern needs; "all" of a package arms what
 * the package holds under the dialect. */
uint32_t mgcp_ev_pat_detect_mask(enum cfg_mgcp_dialect dialect,
				 const struct mgcp_ev_pat *p);

/*
 * The observed event of one core line event under the dialect: a seize,
 * release or flash, a digit, or the start row of a fax or modem tone.
 * false for an event that maps to none.
 *
 * Tones (RFC 3660 pages 8 and 9): ft is the V.21 preamble, the CNG tone
 * and the V.8 CM fax call; mt is the answer tone, with or without phase
 * reversal, ANSam, and a detected modem. The CED of a fax answer is the
 * V.25 answer tone, so it reports mt.
 */
bool mgcp_obs_of_line_event(const struct proto_line_event *ev,
			    struct mgcp_obs *out);

/* The O: token of an observed event under the dialect, as "L/hd", "D/5",
 * "L/5" or "L/oc(L/dl)". false when the buffer ran out. */
bool mgcp_obs_write(enum cfg_mgcp_dialect dialect, const struct mgcp_obs *o,
		    struct sip_buf *b);

/* The package of an oc or of report on sig: the package of the signal. */
enum mgcp_pkg mgcp_obs_sig_pkg(enum cfg_mgcp_dialect dialect,
			       const struct mgcp_obs *o);

#endif
