// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Control-to-engine commands and the bounded SYNC ack (udsp.h section 1).
 *
 * A command is a fixed POD copied by value into a single-producer ring the
 * engine drains at the start of every tick, so nothing the caller owns is read
 * after the call returns. The slot is 256 bytes and carries the media members
 * already, so adding streams never moves an offset.
 *
 * Ack protocol. One slot per direction, guarded by the caller's control mutex.
 * The waiter publishes its expected token first, then posts the command. The
 * completer claims the slot: it compare-exchanges that token to
 * CMD_TOKEN_BUSY, writes the status, then publishes CMD_TOKEN_DONE with a
 * release store. A waiter that reads DONE therefore always reads its own
 * status. If DONE came first, a preempted completer could hand the waiter the
 * status of the previous command, and a failed register write would read as
 * UDSP_OK.
 *
 * Cancellation per command generation. On a timeout the waiter exchanges the
 * token for CMD_TOKEN_CANCELLED, and every executor compares the token the
 * record carries against the token the slot holds: a mismatch means the waiter
 * left, whatever the slot now holds, so a later command reusing the slot can
 * never revive the one that timed out. A waiter whose cancellation exchange
 * loses to a completer rechecks DONE before it reports a timeout.
 *
 * ppoll with a relative CLOCK_MONOTONIC timeout is the only wait: musl's
 * sem_timedwait takes an absolute CLOCK_REALTIME deadline, which a wall clock
 * step would move, and a condvar would put a mutex on the engine thread.
 */
#ifndef UDSP_CORE_CMD_H
#define UDSP_CORE_CMD_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "udsp/udsp.h"

#include "spsc.h"

#define UDSP_CMD_BYTES		256u
#define UDSP_CMD_PAYLOAD_BYTES	224u
#define UDSP_CMD_RING_DEPTH	8u

#define CMD_TOKEN_FREE		0u
#define CMD_TOKEN_BUSY		0xfffffffdu	/* claimed, status being written */
#define CMD_TOKEN_DONE		0xfffffffeu
#define CMD_TOKEN_CANCELLED	0xffffffffu

/* SYNC-TICK is one period plus the wakeup; SYNC-SLIC adds the register writes
 * of a bit-banged spi-gpio bus on AN7581 (udsp.h Worst-case bound). */
#define UDSP_SYNC_TICK_MS	100u
#define UDSP_SYNC_SLIC_MS	250u

enum udsp_cmd_type {
	UDSP_CMD_NONE = 0,
	UDSP_CMD_LINE_STATE,
	UDSP_CMD_POLARITY,
	UDSP_CMD_HOOK_TIMING,
	UDSP_CMD_RING_START,
	UDSP_CMD_RING_STOP,
	UDSP_CMD_CID_SEND,
	UDSP_CMD_MWI_LAMP,
	UDSP_CMD_TONE_PLAY,
	UDSP_CMD_TONE_STOP,
	UDSP_CMD_DIGIT_PLAY,
	UDSP_CMD_CHAN_CONFIG,
	UDSP_CMD_LINES_UP,	/* the start job: base_lf per ready line */
	UDSP_CMD_LINES_SAFE,	/* the stop job: ring off, cid abort, feed open */
	UDSP_CMD_STREAM,	/* media, rtp_fd, rtcp_fd */
	UDSP_CMD_LEC_RELEASE,	/* the tick retires chan.ec so control frees it */
	UDSP_CMD_CW_TONE_PLAY,	/* arg0 = cw variant, arg1 = duration_ms */
	UDSP_CMD_LEC_LATCH_CLEAR,	/* tick clears caller's tone latch */
	UDSP_CMD_TICK_SYNC,	/* no action: the ack says a tick took it */
};

struct cmd_ack {
	int efd;
	_Atomic uint_least32_t token;
	_Atomic int_least32_t status;
};

struct udsp_cmd {
	uint8_t type;		/* enum udsp_cmd_type */
	uint8_t line;
	uint8_t stream;
	uint8_t flags;
	uint32_t arg0;
	uint32_t arg1;
	int32_t arg2;
	uint32_t token;
	struct cmd_ack *ack;
	union {
		struct udsp_ring ring;
		struct udsp_chan_cfg chan;
		struct { struct udsp_cid cid; uint8_t kind; } cid;
		/* Declared here so the slot never grows; a member added later
		 * would move every following offset. */
		struct { struct udsp_media media; int rtp_fd, rtcp_fd; } stream;
		uint8_t reserved[UDSP_CMD_PAYLOAD_BYTES];
	} u;
};

_Static_assert(sizeof(struct udsp_cmd) <= UDSP_CMD_BYTES,
	       "udsp_cmd outgrew its 256-byte slot");

UDSP_SPSC_DECLARE(cmd_ring, struct udsp_cmd, UDSP_CMD_RING_DEPTH);

unsigned cmd_sync_tick_ms_get(void);

int cmd_ack_init(struct cmd_ack *a);
void cmd_ack_release(struct cmd_ack *a);
/* Publishes tok and drains a stale wake; call under the control mutex. */
void cmd_ack_arm(struct cmd_ack *a, uint32_t tok);
/* False when the slot no longer expects tok (its waiter left). */
bool cmd_ack_post(struct cmd_ack *a, uint32_t tok, int32_t status);
/* 0 on a completion, -ETIMEDOUT on expiry (the slot is left CANCELLED). */
int cmd_ack_wait(struct cmd_ack *a, uint32_t tok, unsigned timeout_ms,
		 int32_t *status);
/* The shared tail of a bounded ack wait once its deadline passed: cancel the
 * slot, or take a completion a completer already claimed. 0 on that completion,
 * -ETIMEDOUT on a clean cancel. */
int cmd_ack_finish(struct cmd_ack *a, uint32_t tok, int32_t *status);

/*
 * True when the slot no longer belongs to the command that carries tok: its
 * waiter timed out, or a later command already took the slot. Either way the
 * work must not run, and comparing the exact token is what stops slot reuse
 * from reviving a cancelled command.
 */
static inline bool cmd_ack_stale(const struct cmd_ack *a, uint32_t tok)
{
	if (!a || !tok)
		return false;

	return atomic_load_explicit(&a->token, memory_order_acquire) != tok;
}

/* The status an ack carries is the plug-in's own errno. */
enum udsp_status cmd_status_map(int32_t err);

/*
 * The status a SLOW job completion carries. A completion crosses the event
 * fd into uvoice, whose main.c casts
 * udsp_event.u.job.status straight to enum hal_status, so a raw errno there
 * reads as a nonsense status: the mapping happens at the emission point and no
 * errno ever reaches the wire. The table is wider than cmd_status_map's because
 * a job can fail terminally, while a SYNC op always leaves a retry open.
 */
enum udsp_status cmd_job_status_map(int32_t err);

/* Post one command and block on its bounded ack. The caller holds the control
 * mutex for the whole call and owns the token sequence, so the single slot has
 * exactly one waiter (udsp.h section 1). *status takes the completer's errno.
 * UDSP_TRANSIENT on a full ring or an expired bound. */
enum udsp_status cmd_post_sync(struct cmd_ring *r, struct cmd_ack *ack,
			       uint32_t token, struct udsp_cmd *cmd,
			       unsigned timeout_ms, int32_t *status);

#endif
