// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The slic thread: executor, hook sampler, fault poller and SLOW job worker.
 * Every slic_ops call in libudsp runs here (slic.h rule 1: the ProSLIC API
 * mutates global preset arrays and register access over a bit-banged spi-gpio
 * is slow).
 *
 * The engine is the sole producer into the command ring below, so line-machine
 * commands and FAST line ops share one total order of register writes; the
 * slic thread posts the ack of the command that produced the last record it
 * executed, and the ack carries the first errno that failed inside that
 * command, not the last.
 *
 * The slic thread is also a producer: faults, dead lines and the region swap
 * travel its own ring toward the engine with its own ack slot. It never takes
 * the control mutex, because it completes SYNC-SLIC acks and a control thread
 * waiting for one of those inside the mutex would deadlock.
 */
#ifndef UDSP_CORE_SLIC_THREAD_H
#define UDSP_CORE_SLIC_THREAD_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "udsp/slic.h"

#include "cmd.h"
#include "spsc.h"

/* At most three SLIC records per line per tick (LINE_OUT_MAX is 12), so a
 * cadence-paced load cannot reach this depth; an overflow is an engine defect
 * and latches the fatal flag. */
#define SLIC_RING_DEPTH		128u
#define SLIC_UP_DEPTH		32u
#define SLIC_JOB_DEPTH		8u
#define SLIC_JOB_PATH_MAX	192u
/* In the per-tick modes the fault poll runs on every pass while the interrupt
 * is asserted and once every hundred passes otherwise. */
#define SLIC_FAULT_PERIOD	100u
/*
 * The three sampling modes of the thread. POLL is the per-tick pass on a
 * board with no edge source. BURST is the same pass for SLIC_BURST_MS after
 * the last hook level change or interrupt edge: it outlasts release_min_ms
 * (1301) and the pulse windows, so line_hook_tick sees the tick-resolution
 * samples it measures a flash, a pulse train or a release with. IDLE runs no
 * per-tick pass: the thread sleeps on the interrupt edge, on the tick eventfd
 * the engine writes only when it pushed a record, on the command eventfd,
 * and on the timeout to the next SAFETY sample (SLIC_IDLE_SAMPLE_MS, which
 * catches a level change whose edge was lost) or fault poll
 * (SLIC_IDLE_FAULT_MS). An edge or a changed level in IDLE enters BURST.
 */
enum slic_mode {
	SLIC_MODE_POLL = 0,
	SLIC_MODE_BURST,
	SLIC_MODE_IDLE,
};
#define SLIC_BURST_MS		2000u
#define SLIC_IDLE_SAMPLE_MS	250u
#define SLIC_IDLE_FAULT_MS	1000u
/* The engine acks a slic-thread request at a tick boundary; two seconds covers
 * a heavily loaded tick and still bounds a job that would otherwise hang. */
#define SLIC_ACK_WAIT_MS	2000u
/*
 * The per-channel recovery. A line gets SLIC_RECOVER_TRY_MAX attempts of its
 * recovery op, SLIC_RECOVER_HOLDOFF_MS apart (the fault trial cadence of
 * core/chan.h). Then the engine holds it open until the next start. A DONE
 * the engine did not accept is posted again on the following passes, at most
 * SLIC_DONE_RETRY_MAX times. Each post waits SLIC_ACK_WAIT_MS. A lost result
 * therefore costs at most 8 s of the thread after the op itself. The op is
 * about 4.3 s for a converter restart and about 1 s for a si3218x reinit on
 * the AN7581 bus.
 */
#define SLIC_RECOVER_TRY_MAX	3u
#define SLIC_RECOVER_HOLDOFF_MS	5000u
#define SLIC_DONE_RETRY_MAX	3u
/*
 * The init retry of a dead line, run from the periodic pass once the start
 * brought at least one line up. One attempt per pass, every
 * SLIC_DEAD_RETRY_MS of CLOCK_MONOTONIC wall clock, the line monitor's
 * clock. Attempts are unbounded: on AN7581, a patch verify failure
 * clears after about a minute of settling.
 *
 * This is the production cadence. A test shortens it through the env var
 * UDSP_SLIC_DEAD_RETRY_MS, read once at open into udsp.dead_retry_ms.
 */
#define SLIC_DEAD_RETRY_MS	10000u
/*
 * The engine holds a line whose recovery DONE never came. It
 * does so after two deadlines of half this window pass with the recovery
 * still in flight. The window outlasts the slic thread's DONE re-post budget
 * of SLIC_DONE_RETRY_MAX posts, each up to SLIC_ACK_WAIT_MS. A re-post that
 * heals a dropped completion therefore always wins the race.
 */
#define SLIC_RECOVER_LOST_MS	40000u
/* The line monitor cadence: every SLIC_IDLE_MONITOR_MS of wall clock in
 * IDLE, every SLIC_MONITOR_PERIOD passes in BURST and POLL (the same ten
 * seconds at tick rate). */
#define SLIC_IDLE_MONITOR_MS	10000u
#define SLIC_MONITOR_PERIOD	1000u

enum slic_rec_type {
	SLIC_REC_NONE = 0,
	SLIC_REC_LINEFEED,
	SLIC_REC_POLARITY,	/* the engine resolves the variant, it owns base_lf */
	SLIC_REC_RING_RELAY,
	SLIC_REC_MWI_LAMP,
	SLIC_REC_POWERSAVE,
	SLIC_REC_GAIN,		/* carries the signed pair below */
};

struct slic_rec {
	uint8_t type;		/* enum slic_rec_type */
	uint8_t line;
	uint8_t arg;		/* linefeed state, or the boolean of the op */
	bool last;		/* the ack of token goes with this record */
	/* SLIC_REC_GAIN only: both values, 0.5 dB steps. arg cannot carry
	 * them because each spans the full signed byte. */
	int8_t gain_tx;
	int8_t gain_rx;
	/* The feed records of slic_rec_seq_carried only: the write generation
	 * the engine stamps. It tells a queued feed write from one the slic
	 * thread already ran (core/chan.h lf_seq_emit). */
	uint32_t lf_seq;
	uint32_t token;
	struct cmd_ack *ack;
};

/* The records that carry a write generation: the records that write the feed.
 * A ring relay record writes it too: the vendor ProSLIC_RingStart writes the
 * ringing feed, and ProSLIC_RingStop the active feed. */
static inline bool slic_rec_seq_carried(uint8_t type)
{
	return type == SLIC_REC_LINEFEED || type == SLIC_REC_POLARITY ||
	       type == SLIC_REC_RING_RELAY;
}

UDSP_SPSC_DECLARE(slic_ring, struct slic_rec, SLIC_RING_DEPTH);

enum slic_up_type {
	SLIC_UP_NONE = 0,
	SLIC_UP_FAULT,
	SLIC_UP_LINE_DEAD,
	SLIC_UP_REGION_SWAP,
	SLIC_UP_LINES_UP,	/* the start job: base_lf per ready line */
	SLIC_UP_LINES_SAFE,	/* the stop job: ring off, cid abort, feed open */
	/* A feed record (slic_rec_seq_carried) failed on the bus. rc carries
	 * the errno and bits the write generation of the record. */
	SLIC_UP_REC_FAIL,
	/* The recovery of one line. BEGIN safes it and marks the recovery in
	 * flight; DONE carries the op's result in rc. Both carry the line
	 * mask in bits, as LINES_UP does, and both are acked. */
	SLIC_UP_LINE_RECOVER_BEGIN,
	SLIC_UP_LINE_RECOVER_DONE,
	/* A hold the ladder decided without an attempt (an over-voltage
	 * lock-out read twice); reason names it. Not acked. */
	SLIC_UP_LINE_HOLD,
};

struct slic_up_rec {
	uint8_t type;		/* enum slic_up_type */
	uint8_t line;
	uint8_t reason;		/* LINE_HOLD: enum udsp_fault_held_reason */
	int32_t rc;		/* LINE_RECOVER_DONE and REC_FAIL: the result */
	uint32_t bits;
	uint32_t token;
	struct cmd_ack *ack;
};

UDSP_SPSC_DECLARE(slic_up_ring, struct slic_up_rec, SLIC_UP_DEPTH);

enum slic_job_kind {
	SLIC_JOB_NONE = 0,
	SLIC_JOB_ATTACH,
	SLIC_JOB_START,
	SLIC_JOB_STOP,
	SLIC_JOB_REGION,
	SLIC_JOB_LINE_TEST,
};

struct slic_job {
	uint8_t kind;		/* enum slic_job_kind */
	uint8_t line;		/* SLIC_JOB_LINE_TEST only */
	uint8_t test;		/* SLIC_JOB_LINE_TEST only: udsp.h line test */
	uint32_t token;
	uint32_t cancel_seen;	/* the queue generation at enqueue */
	struct cmd_ack *ack;	/* ATTACH only: udsp_open blocks on it */
	char path[SLIC_JOB_PATH_MAX];
};

struct slic_jobq {
	struct slic_job slot[SLIC_JOB_DEPTH];
	atomic_uint head;	/* control thread, under the control mutex */
	atomic_uint tail;	/* slic thread */
	_Atomic uint_least32_t cancel_gen;
};

void slic_jobq_init(struct slic_jobq *q);
bool slic_jobq_push(struct slic_jobq *q, const struct slic_job *j);
bool slic_jobq_pop(struct slic_jobq *q, struct slic_job *out);
/* Supersedes every START and every line test still in the queue; one already
 * executing completes first, because a running metallic line test carries no
 * cancel. The generation counter is what the consumer compares. */
void slic_jobq_supersede(struct slic_jobq *q);
bool slic_job_superseded(const struct slic_jobq *q, const struct slic_job *j);

struct udsp;

void *udsp_slic_loop(void *arg);
/* Posts one record toward the engine on the slic thread's own ring and waits
 * for the engine's ack, draining the command ring meanwhile: the ack of a
 * multi-record request goes with a record this very thread must still execute.
 * 0 or a negative errno. */
int slic_up_post(struct udsp *s, enum slic_up_type type, uint32_t bits,
		 unsigned ms, int32_t *status);
/* The same post with a result for the record: slic_up_post has no input for
 * one, its status is the ack out-parameter. */
int slic_up_post_rc(struct udsp *s, enum slic_up_type type, uint32_t bits,
		    int32_t rc, unsigned ms, int32_t *status);
/* The engine writes the tick eventfd only while the slic thread armed a wake
 * and either pushed a record this tick or the thread asked for every tick
 * (every mode but IDLE), so the RT tick pays one release store and at most
 * one write per tick, and an idle thread sleeps through a quiet tick. */
void udsp_slic_tick_wake(struct udsp *s);

#endif
