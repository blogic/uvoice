// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The call trace controller: one armed trace of one FXS line at a time.
 *
 * A support person arms a trace on one line. Capture starts at the arm: the
 * udebug rings open, the SIP tap installs, the HAL debug bits set and the
 * wire capture starts. The window runs from the next transition of the line
 * out of its idle rest (line/line.h line_rest_of) until the line rests again
 * plus a grace period, or until the timeout. The controller then freezes the
 * capture, exports the rings, writes the manifest and spawns the finisher,
 * which packs one archive under /tmp/uvoice/calltrace/.
 *
 * Every entry point runs on the uloop thread. The notification entries
 * (calltrace_line_state, calltrace_call_started, calltrace_event,
 * calltrace_config_applied) are cheap while no trace is armed: one state
 * load and a return.
 */
#ifndef UVOICE_CALLTRACE_H
#define UVOICE_CALLTRACE_H

#include <stdbool.h>
#include <stdint.h>

#include <libubox/blobmsg.h>

struct calltrace_options {
	unsigned int timeout_s;
	unsigned int grace_s;
	unsigned int arm_timeout_s;
	unsigned int wire_max_mb;
	unsigned int rings_max_mb;
	unsigned int lec_max_mb;
	bool wire;
	/* The canceller capture: every frame of every line canceller while the
	 * window runs, the near-end room in the clear among them. */
	bool lec;
};

/*
 * The defaults and the bounds of the call trace, shared with the ubus layer
 * so one set of numbers serves the reply and the refusal.
 *
 * calltrace_arm takes the default for a zero timeout, grace or arm timeout,
 * so a caller that zeroes the struct gets the shipped window. The three size
 * caps have no such reading: a cap of no megabytes asks for a capture that
 * can hold nothing, and it is refused as a bad range. The ubus layer fills
 * all six for an argument nobody named.
 */
#define CALLTRACE_TIMEOUT_DEFAULT_S	300
#define CALLTRACE_TIMEOUT_MAX_S		1800
#define CALLTRACE_GRACE_DEFAULT_S	5
#define CALLTRACE_GRACE_MAX_S		30
#define CALLTRACE_ARM_TIMEOUT_DEF_S	3600
/* One day. A trace nobody used by then is a trace nobody is waiting for, and
 * the bound is what keeps every timer of this surface inside an int of
 * milliseconds. */
#define CALLTRACE_ARM_TIMEOUT_MAX_S	86400
#define CALLTRACE_WIRE_MAX_MB_DEF	32
#define CALLTRACE_RINGS_MAX_MB_DEF	64
#define CALLTRACE_LEC_MAX_MB_DEF	32

/* 0, or -errno: -EINVAL for a bad line or range, -EBUSY while a trace is
 * armed, running or finishing, -ENOSPC, -ENOENT and -ENOSYS for the named
 * preflight refusals, each recorded in last_error for the status reply. */
int calltrace_arm(unsigned int line, const struct calltrace_options *o);
/* 0, or -ENOENT when no trace is armed or running. */
int calltrace_cancel(void);
void calltrace_status_dump(struct blob_buf *b);

/*
 * The notifications. The protocol calls the first three through the host
 * table (proto/proto.h calltrace_state, calltrace_call_started,
 * calltrace_event); main.c the fourth. before and after are the protocol's
 * names of the two states of one move; where the line rests comes from
 * line_rest_of().
 */
void calltrace_line_state(unsigned int line, const char *before,
			  const char *after);
void calltrace_call_started(unsigned int line, uint32_t cdr_seq);
void calltrace_event(unsigned int line, const char *reason);
void calltrace_config_applied(void);

/* Runs the recovery of the work dirs an earlier daemon left behind. */
void calltrace_init(void);
void calltrace_shutdown(void);
bool calltrace_capturing(void);		/* armed or running */
/*
 * Running with the lec option, and the canceller ring not yet at its cap. An
 * armed trace captures no canceller: every line in a call is captured, so an
 * arm that waits for its seize would fill the cap with a call that comes
 * before the traced one. After the cap nothing exports the frames, so the
 * capture stops there too.
 */
bool calltrace_lec_capturing(void);

/*
 * Test control, on the fw_test_mode() precedent. Test mode resets the
 * controller, starts no child and skips the capture tool checks: every spawn
 * becomes a recorded argument list the suite reads back, and the suite
 * completes each child by hand. Nothing here is compiled out, so the daemon
 * carries one code path and the suite drives the production one.
 */
#define CALLTRACE_CHILD_WIRE	0
#define CALLTRACE_CHILD_FINISH	1

#define CALLTRACE_CADENCE_SNAPSHOT	0
#define CALLTRACE_CADENCE_EXPORT	1
#define CALLTRACE_CADENCE_WIRE_POLL	2
#define CALLTRACE_CADENCE_KILL		3
#define CALLTRACE_CADENCE_FINISH	4
#define CALLTRACE_CADENCE_EXPORT_LEC	5

void calltrace_test_mode(bool on);
/* The cadence in force, so one case pins the shipped value and the next pins
 * the environment override. Test mode reads the environment again. */
unsigned int calltrace_test_cadence_ms(int which);
/* The work root, in place of /tmp/uvoice/calltrace. NULL restores it. */
void calltrace_test_root(const char *dir);
/* The free space the preflight reads. 0 restores the statvfs answer. */
void calltrace_test_space(uint64_t free_bytes);
/* The term per MiB of work dir that a finisher budget adds to its base, so a
 * case reaches the deadline of a large dir inside a pumped window. 0 restores
 * the shipped term. */
void calltrace_test_finish_mib_ms(unsigned int ms);
/* The budget that the last finisher start took, live or recovery. */
unsigned int calltrace_test_finish_budget_ms(void);
/* The number of CPUs the capture mask is built from. 0 restores the real
 * count. */
void calltrace_test_cpus(long cpus);
/* Make every start of a child answer as a spawn that failed. */
void calltrace_test_spawn_fail(bool on);
/* Move the wall clock the snapshots read, so a case can produce the step a
 * clock correction makes under a running capture. */
void calltrace_test_clock_shift(int64_t ms);
/* Argument i of the last start of one child, or NULL past its list. */
const char *calltrace_test_argv(int child, unsigned int i);
/* The signals one child was sent, "T" for a stop and "K" for a kill, in the
 * order they went. */
const char *calltrace_test_signals(int child);
/* The exit status of a child the suite completes. */
void calltrace_test_child_done(int child, int code);

#endif
