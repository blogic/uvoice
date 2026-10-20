// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The slic thread. It blocks in ppoll over the command eventfd and the tick
 * eventfd, and per wake it executes the SLIC records the engine produced, runs
 * at most one SLOW job step, samples the hook of every live line once and
 * polls the fault sources.
 *
 * Time. Its clock is the engine's tick counter, never its own: every stamp it
 * puts on a hook edge is in the engine time base, so the line machines see one
 * monotonic millisecond scale (line.h section 1).
 *
 * Missed passes are never replayed. After a seconds-long init the tick delta
 * is large and exactly one periodic pass follows, which is what the debounce
 * expects: the machines are stamp-driven and a wide gap is a wide interval,
 * not a burst of ticks.
 */
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include "udsp_priv.h"

#define SLIC_WAIT_SLICE_MS	5u
/* The smallest record a metallic line test can carry. A run that ends early
 * reports why in the head alone, so a shorter record carries no reason at all.
 * libudsp decodes none of those bytes (slic.h); it only refuses a length that
 * cannot hold a head. */
#define SLIC_MLT_HEAD_BYTES	8u

/*
 * The thread runs SCHED_FIFO below the net thread (udsp.slic_prio), so a hook
 * sample wakes with real-time latency instead of the fair scheduler's. A SLOW
 * vendor op drops to SCHED_OTHER for its whole run: a bit-banged init holds
 * the CPU for seconds, and a FIFO thread doing that would starve the CPU's
 * other work. Both changes are best effort, as rt_sched_apply is (a
 * RT_GROUP_SCHED cgroup refuses them with EPERM): a refusal is counted and
 * logged once per handle, and the policy the thread holds after every change
 * is published for the open log. The thread takes no mutex, so the elevation
 * cannot invert priority against the control thread through the library's
 * own locks. pthread_setschedparam and not sched_setscheduler: musl stubs the
 * latter with ENOSYS.
 */
static void slic_policy_set(struct udsp *s, int policy, int prio)
{
	struct sched_param param = { .sched_priority = prio };
	int got = -1;
	int rc;

	if (s->slic_prio <= 0)
		return;
	rc = pthread_setschedparam(pthread_self(), policy, &param);
	if (rc) {
		udsp_op_error(s);
		if (!s->slic_policy_logged) {
			s->slic_policy_logged = true;
			udsp_log(s, UDSP_LOG_WARN,
				 "slic thread policy change refused: %s",
				 strerror(rc));
		}
	}
	if (pthread_getschedparam(pthread_self(), &got, &param))
		got = -1;
	atomic_store_explicit(&s->slic_policy, got, memory_order_release);
}

static void slic_slow_enter(struct udsp *s)
{
	slic_policy_set(s, SCHED_OTHER, 0);
}

static void slic_slow_leave(struct udsp *s)
{
	slic_policy_set(s, SCHED_FIFO, s->slic_prio);
}

void slic_jobq_init(struct slic_jobq *q)
{
	memset(q, 0, sizeof(*q));
	atomic_init(&q->head, 0u);
	atomic_init(&q->tail, 0u);
	atomic_init(&q->cancel_gen, 0u);
}

bool slic_jobq_push(struct slic_jobq *q, const struct slic_job *j)
{
	unsigned head = atomic_load_explicit(&q->head, memory_order_relaxed);
	unsigned next = (head + 1u) % SLIC_JOB_DEPTH;

	if (next == atomic_load_explicit(&q->tail, memory_order_acquire))
		return false;

	q->slot[head] = *j;
	q->slot[head].cancel_seen = (uint32_t)atomic_load_explicit(
		&q->cancel_gen, memory_order_acquire);
	atomic_store_explicit(&q->head, next, memory_order_release);

	return true;
}

bool slic_jobq_pop(struct slic_jobq *q, struct slic_job *out)
{
	unsigned tail = atomic_load_explicit(&q->tail, memory_order_relaxed);

	if (tail == atomic_load_explicit(&q->head, memory_order_acquire))
		return false;

	*out = q->slot[tail];
	atomic_store_explicit(&q->tail, (tail + 1u) % SLIC_JOB_DEPTH,
			      memory_order_release);

	return true;
}

void slic_jobq_supersede(struct slic_jobq *q)
{
	atomic_fetch_add_explicit(&q->cancel_gen, 1u, memory_order_acq_rel);
}

/*
 * A queued line test is superseded like a start: a stop asks the lines to go
 * safe, and a test that has not begun would instead drive one of them for
 * seconds afterwards. A test already inside the plug-in is not reached here,
 * because the check runs once, when the job is taken from the queue.
 */
bool slic_job_superseded(const struct slic_jobq *q, const struct slic_job *j)
{
	if (j->kind != SLIC_JOB_START && j->kind != SLIC_JOB_LINE_TEST)
		return false;

	return j->cancel_seen != (uint32_t)atomic_load_explicit(&q->cancel_gen,
							memory_order_acquire);
}

/* --- record execution ---------------------------------------------------- */

/*
 * The push and the arm are a store-buffer pair: the engine stores the ring
 * head and then reads slic_armed, the thread stores slic_armed and then reads
 * the ring head. Under acquire and release alone both reads may miss both
 * stores, and in IDLE nothing repairs a missed wake before the safety
 * deadline, so both sides carry a sequentially consistent fence between their
 * store and their read.
 */
void udsp_slic_tick_wake(struct udsp *s)
{
	uint64_t one = 1;
	bool pushed = s->slic_ring_pushed;

	s->slic_ring_pushed = false;
	if (!pushed && !atomic_load_explicit(&s->slic_wants_tick,
					     memory_order_acquire))
		return;
	atomic_thread_fence(memory_order_seq_cst);
	if (!atomic_exchange_explicit(&s->slic_armed, false,
				      memory_order_acq_rel))
		return;
	if (write(s->slic_tick_fd, &one, sizeof(one)) != (ssize_t)sizeof(one))
		return;
}

static int slic_rec_run(struct udsp *s, const struct slic_rec *rec)
{
	const struct udsp_slic_ops *ops = s->ops;
	unsigned line = rec->line;

	switch (rec->type) {
	case SLIC_REC_LINEFEED:
	case SLIC_REC_POLARITY:
		return ops->linefeed_set(s->slic, line,
					 (enum udsp_linefeed)rec->arg);
	case SLIC_REC_RING_RELAY:
		return rec->arg ? ops->ring_start(s->slic, line) :
				  ops->ring_stop(s->slic, line);
	case SLIC_REC_MWI_LAMP:
		return ops->mwi_lamp ? ops->mwi_lamp(s->slic, line, rec->arg) :
				       -ENOTSUP;
	case SLIC_REC_POWERSAVE:
		return ops->powersave ? ops->powersave(s->slic, line, rec->arg) :
					-ENOTSUP;
	case SLIC_REC_GAIN:
		return ops->gain_set(s->slic, line, rec->gain_tx, rec->gain_rx);
	default:
		return 0;
	}
}

/* Only a write the part actually took advances the state the engine suppresses
 * duplicate writes against. */
static void slic_rec_confirm(struct udsp *s, const struct slic_rec *rec)
{
	struct udsp_chan *c = &s->chan[rec->line];

	c->rec_fail_logged = false;
	switch (rec->type) {
	case SLIC_REC_LINEFEED:
	case SLIC_REC_POLARITY:
		atomic_store_explicit(&c->lf_confirmed, rec->arg,
				      memory_order_release);
		return;
	case SLIC_REC_RING_RELAY:
		/* The vendor ring stop leaves the active feed on the part:
		 * ProSLIC_RingStop writes LF_FWD_ACTIVE. The confirmed feed
		 * follows it, so a held line's OPEN after it is retried. */
		if (!rec->arg)
			atomic_store_explicit(&c->lf_confirmed,
					      UDSP_LF_FWD_ACTIVE,
					      memory_order_release);
		return;
	case SLIC_REC_POWERSAVE:
		atomic_store_explicit(&c->ps_confirmed, rec->arg != 0,
				      memory_order_release);
		return;
	case SLIC_REC_GAIN:
		atomic_store_explicit(&c->gain_confirmed,
				      udsp_gain_confirmed_pack(rec->gain_tx,
							       rec->gain_rx),
				      memory_order_release);
		return;
	default:
		return;
	}
}

/* The engine tracks a feed write in flight by the record's generation. The
 * mark advances on a failed or dropped write too, or the engine's clear and
 * hold lift would wait for it for ever. */
static void slic_rec_seq_mark(struct udsp *s, const struct slic_rec *rec)
{
	if (!slic_rec_seq_carried(rec->type))
		return;
	atomic_store_explicit(&s->chan[rec->line].lf_seq_done, rec->lf_seq,
			      memory_order_release);
}

static int slic_rec_exec(struct udsp *s, const struct slic_rec *rec)
{
	int rc;

	if (atomic_load_explicit(&s->chan[rec->line].dead,
				 memory_order_acquire)) {
		slic_rec_seq_mark(s, rec);
		return -EIO;
	}

	rc = slic_rec_run(s, rec);
	if (!rc)
		slic_rec_confirm(s, rec);
	slic_rec_seq_mark(s, rec);

	return rc;
}

/* A machine record carries no ack, so the only way its caller hears about a
 * failure is the asynchronous completion of a caller identity sequence. Only
 * a feed write can end one, so only a feed record is reported. The report
 * carries the write generation, and the engine aborts only a sequence that
 * armed before the write was queued (chan.c chan_up_rec_fail). */
static void slic_rec_fail_report(struct udsp *s, const struct slic_rec *rec,
				 int err)
{
	struct slic_up_rec up;

	if (!slic_rec_seq_carried(rec->type))
		return;
	memset(&up, 0, sizeof(up));
	up.type = SLIC_UP_REC_FAIL;
	up.line = rec->line;
	up.rc = err;
	up.bits = rec->lf_seq;
	if (!slic_up_ring_push(&s->up, &up))
		udsp_op_error(s);
}

static const char *slic_rec_name(uint8_t type)
{
	switch (type) {
	case SLIC_REC_LINEFEED:
		return "linefeed";
	case SLIC_REC_POLARITY:
		return "polarity";
	case SLIC_REC_RING_RELAY:
		return "ring relay";
	case SLIC_REC_MWI_LAMP:
		return "lamp";
	case SLIC_REC_POWERSAVE:
		return "powersave";
	case SLIC_REC_GAIN:
		return "gain";
	default:
		return "record";
	}
}

/* A ring cadence on a dead bus fails a record several times a second, so one
 * line per failure run: the next record the line confirms opens a new run. */
static void slic_rec_fail_log(struct udsp *s, const struct slic_rec *rec, int rc)
{
	struct udsp_chan *c = &s->chan[rec->line];

	if (c->rec_fail_logged)
		return;
	c->rec_fail_logged = true;
	udsp_log(s, UDSP_LOG_WARN, "slic %s failed on line %u: %s",
		 slic_rec_name(rec->type), rec->line, strerror(-rc));
}

/*
 * One error group per command. The records of one command are contiguous and
 * share its token, so the first failing errno of that group goes with its
 * ack. A machine record carries token zero: its failure is
 * counted and reported, and it never contaminates the ack of whichever command
 * happens to follow it.
 */
static void slic_records_run(struct udsp *s)
{
	struct slic_rec rec;

	while (slic_ring_pop(&s->ring, &rec)) {
		bool grouped = rec.token != 0;
		int rc;

		if (grouped && rec.token != s->rec_token) {
			s->rec_token = rec.token;
			s->rec_err = 0;
		}
		/* The waiter left, or a later command took its slot, so this
		 * record must not run. A dropped feed write changed no
		 * hardware, and the engine must not wait for it. */
		if (cmd_ack_stale(rec.ack, rec.token)) {
			slic_rec_seq_mark(s, &rec);
			if (rec.last)
				s->rec_token = 0;
			continue;
		}

		rc = slic_rec_exec(s, &rec);
		if (rc && grouped && !s->rec_err)
			s->rec_err = rc;
		if (rc && !grouped) {
			udsp_op_error(s);
			slic_rec_fail_report(s, &rec, rc);
			slic_rec_fail_log(s, &rec, rc);
		}
		if (!rec.last)
			continue;
		cmd_ack_post(rec.ack, rec.token, s->rec_err);
		s->rec_err = 0;
		s->rec_token = 0;
	}
}

static void slic_fd_drain(int fd)
{
	uint64_t sink;

	while (read(fd, &sink, sizeof(sink)) == (ssize_t)sizeof(sink))
		continue;
}

/* The ack of a command the slic thread itself posts goes with a record the
 * slic thread must still execute, so the wait drains while it waits. */
static int slic_ack_wait(struct udsp *s, uint32_t token, unsigned ms,
			 int32_t *status)
{
	uint64_t end = udsp_mono_us() + (uint64_t)ms * 1000u;

	for (;;) {
		struct pollfd pfd[2] = {
			{ .fd = s->slic_ack.efd, .events = POLLIN },
			{ .fd = s->slic_cmd_fd, .events = POLLIN },
		};
		struct timespec rel = { .tv_sec = 0,
					.tv_nsec = SLIC_WAIT_SLICE_MS * 1000000L };

		slic_records_run(s);
		if (atomic_load_explicit(&s->slic_ack.token,
					 memory_order_acquire) == CMD_TOKEN_DONE) {
			if (status)
				*status = (int32_t)atomic_load_explicit(
					&s->slic_ack.status,
					memory_order_relaxed);
			slic_fd_drain(s->slic_ack.efd);
			return 0;
		}
		/* A destroyed engine never acks, so a post placed during a
		 * close bails at once instead of eating its whole bound. */
		if (atomic_load_explicit(&s->slic_quit, memory_order_acquire))
			break;
		if (udsp_mono_us() >= end)
			break;
		if (ppoll(pfd, 2, &rel, NULL) > 0) {
			slic_fd_drain(s->slic_ack.efd);
			slic_fd_drain(s->slic_cmd_fd);
		}
	}

	return cmd_ack_finish(&s->slic_ack, token, status);
}

/* The line a one-line mask names; the drain resolves its channel from the
 * record's line, not from the mask. A multi-line or empty mask (LINES_UP,
 * LINES_SAFE) names line 0, which those arms never read. */
static uint8_t slic_up_line_of(uint32_t bits)
{
	uint8_t line = 0;

	while (bits > 1u) {
		bits >>= 1;
		line++;
	}

	return line;
}

static int slic_up_post_wait(struct udsp *s, enum slic_up_type type,
			     uint32_t bits, int32_t rc, unsigned ms,
			     int32_t *status)
{
	struct slic_up_rec rec;
	uint32_t token = ++s->slic_token;

	if (!token)
		token = ++s->slic_token;

	memset(&rec, 0, sizeof(rec));
	rec.type = (uint8_t)type;
	rec.line = slic_up_line_of(bits);
	rec.bits = bits;
	rec.rc = rc;
	rec.token = token;
	rec.ack = &s->slic_ack;
	cmd_ack_arm(&s->slic_ack, token);
	if (!slic_up_ring_push(&s->up, &rec)) {
		udsp_op_error(s);
		return -ENOSPC;
	}

	return slic_ack_wait(s, token, ms, status);
}

int slic_up_post(struct udsp *s, enum slic_up_type type, uint32_t bits,
			unsigned ms, int32_t *status)
{
	return slic_up_post_wait(s, type, bits, 0, ms, status);
}

int slic_up_post_rc(struct udsp *s, enum slic_up_type type, uint32_t bits,
		    int32_t rc, unsigned ms, int32_t *status)
{
	return slic_up_post_wait(s, type, bits, rc, ms, status);
}

/* --- hook sampling and faults -------------------------------------------- */

static void slic_line_dead(struct udsp *s, struct udsp_chan *c,
			   const char *reason)
{
	struct slic_up_rec rec;
	struct udsp_event ev;

	atomic_store_explicit(&c->dead, true, memory_order_release);
	c->dead_retry_us = udsp_mono_us() + (uint64_t)s->dead_retry_ms * 1000u;
	udsp_log(s, UDSP_LOG_ERR, "slic line %u dead %s", c->line, reason);

	memset(&rec, 0, sizeof(rec));
	rec.type = SLIC_UP_LINE_DEAD;
	rec.line = (uint8_t)c->line;
	slic_up_ring_push(&s->up, &rec);

	/* event.h has no fault bit for a dead control bus and none is
	 * invented: the line is reported down instead. */
	memset(&ev, 0, sizeof(ev));
	ev.type = UDSP_EV_LINE;
	ev.line = (uint16_t)c->line;
	ev.stamp_ms = udsp_now_ms(s);
	ev.u.line.state = UDSP_LINE_DOWN;
	udsp_emit(&s->emitter, UDSP_EV_PROD_SLIC, &ev);
}

#define SLIC_READ_FAIL_LOG_US	(10u * 1000u * 1000u)
#define SLIC_READ_FAIL_CLEAR_US	(1000u * 1000u)

/*
 * A bus that answers nothing fails every read of every pass, and one that
 * flaps fails and recovers by turns, so the failures of one line log once per
 * SLIC_READ_FAIL_LOG_US whatever the read rate and whatever recovered in
 * between, and the recovery once per logged failure, after
 * SLIC_READ_FAIL_CLEAR_US of clean reads.
 */
static void slic_read_fail_log(struct udsp *s, struct udsp_chan *c,
			       const char *what, int rc)
{
	uint64_t now = udsp_mono_us();

	c->read_fail_last_us = now;
	if (c->read_fail_log_us && now - c->read_fail_log_us < SLIC_READ_FAIL_LOG_US)
		return;
	c->read_fail_open = true;
	c->read_fail_log_us = now;
	udsp_log(s, UDSP_LOG_ERR, "slic %s read failed on line %u: %s", what,
		 c->line, strerror(-rc));
}

static void slic_read_ok_log(struct udsp *s, struct udsp_chan *c)
{
	if (!c->read_fail_open)
		return;
	if (udsp_mono_us() - c->read_fail_last_us < SLIC_READ_FAIL_CLEAR_US)
		return;
	c->read_fail_open = false;
	udsp_log(s, UDSP_LOG_INFO, "slic reads on line %u recovered", c->line);
}

/*
 * The hook sampling record of udsp_counters. The hook machine measures a
 * pulse between sample stamps, so a sample this thread took late carries
 * that lateness into the break or the make it closes. A sample more than
 * SLIC_HOOK_GAP_MS (two ticks) after the previous one, while the thread
 * samples per tick, is therefore counted as a gap and its width kept, on the
 * wall clock: the engine stamp is clamped to the tick and would hide a stall
 * the engine shares. The first sample after sampling starts, after a hold and
 * after an IDLE cadence is the cadence itself and arms the record without
 * counting.
 * The WARN names a gap wide enough to move a pulse out of its window, once
 * per SLIC_READ_FAIL_LOG_US per line.
 */
#define SLIC_HOOK_GAP_MS	20u
#define SLIC_HOOK_GAP_LOG_MS	50u

static void slic_hook_gap_log(struct udsp *s, struct udsp_chan *c,
			      uint64_t now_us, uint64_t gap_ms)
{
	if (c->hook_gap_log_us &&
	    now_us - c->hook_gap_log_us < SLIC_READ_FAIL_LOG_US)
		return;
	c->hook_gap_log_us = now_us;
	udsp_log(s, UDSP_LOG_WARN, "slic hook sample on line %u came %llu ms late",
		 c->line, (unsigned long long)gap_ms);
}

/* The wall gap past which an IDLE pass did not run when its deadline came:
 * the safety cadence plus the slack ppoll adds under load. A pass this late
 * left the level unconfirmed for longer than the cadence, whatever woke it. */
#define SLIC_IDLE_STALL_MS	(SLIC_IDLE_SAMPLE_MS + 100u)

/* True when this sample is a stall: a gap past the counter's bound between
 * two per-tick samples, else past the IDLE cadence and its slack, which is
 * the most any cadence leaves between samples; so the first per-tick sample
 * after an IDLE one, which the arm exempts from the counter, is still a
 * stall when a job ran between them. The first sample of a line has no
 * previous one; the first after a hold reads the hold as a stall, which it
 * is for the machine. */
static bool slic_hook_gap_note(struct udsp *s, struct udsp_chan *c, bool timed)
{
	uint64_t now_us = udsp_mono_us();
	uint64_t gap_ms = (now_us - c->hook_sample_us) / 1000u;
	bool armed = c->hook_gap_armed;
	bool first = !c->hook_sample_us;
	bool stall;

	c->hook_sample_us = now_us;
	c->hook_gap_armed = timed;
	if (first)
		return false;
	if (timed && armed)
		stall = gap_ms > SLIC_HOOK_GAP_MS;
	else
		stall = gap_ms > SLIC_IDLE_STALL_MS;
	if (!armed || !timed || gap_ms <= SLIC_HOOK_GAP_MS)
		return stall;
	atomic_fetch_add_explicit(&s->hook_gaps, 1u, memory_order_relaxed);
	if (gap_ms > atomic_load_explicit(&s->hook_gap_max_ms,
					  memory_order_relaxed))
		atomic_store_explicit(&s->hook_gap_max_ms,
				      (uint_least32_t)(gap_ms > UINT32_MAX ?
						       UINT32_MAX : gap_ms),
				      memory_order_relaxed);
	if (gap_ms >= SLIC_HOOK_GAP_LOG_MS)
		slic_hook_gap_log(s, c, now_us, gap_ms);
	return stall;
}

static enum slic_mode slic_mode_get(const struct udsp *s);

static const char *slic_mode_name(const struct udsp *s)
{
	switch (slic_mode_get(s)) {
	case SLIC_MODE_POLL:
		return "poll";
	case SLIC_MODE_BURST:
		return "burst";
	case SLIC_MODE_IDLE:
		return "idle";
	}
	return "?";
}

/* A record that will not fit is dropped and remembered: the next record that
 * pushes carries LOST, so the machine learns that the raw level it holds may
 * be stale (chan.h). A silent drop would desynchronize the machine until
 * the next change. The DEBUG line is the record half of the hook trace
 * (chan.h struct udsp_hook_trace): the stamp and the gap the machine measures
 * with, and the wall clock a bench instrument is read against. */
static void slic_hook_push(struct udsp *s, struct udsp_chan *c, uint32_t stamp,
			   bool level, uint8_t flags, uint16_t gap_ms)
{
	if (c->edge_lost)
		flags |= UDSP_HOOK_EDGE_LOST;
	if (!udsp_hook_edge_push(&c->edges, stamp, level, flags, gap_ms)) {
		c->edge_lost = true;
		udsp_op_error(s);
		return;
	}
	c->edge_lost = false;
	udsp_log(s, UDSP_LOG_DEBUG,
		 "hook record line %u stamp %u level %u gap %u flags 0x%02x mode %s wall %llu",
		 c->line, stamp, level ? 1u : 0u, gap_ms, flags, slic_mode_name(s),
		 (unsigned long long)udsp_mono_us());
}

/*
 * The candidate the previous sample held: the part recorded a transition the
 * level did not show. Two things look like that. A pair of transitions inside
 * the sample interval, a lost pulse, which the machine must hear about. Or one
 * transition that fell between the plug-in's level read and its register
 * read (slic.h hook_read_moved), which this sample then shows as a level
 * change the part has already reported: that candidate is dropped and the
 * edge goes out as usual, one pass late. Any other next sample confirms the
 * pair, and the GAP record goes out before this sample's own record so the
 * machine sees it at the candidate's stamp.
 */
static void slic_gap_resolve(struct udsp *s, struct udsp_chan *c, bool changed,
			     bool moved)
{
	if (!c->gap_pending)
		return;
	c->gap_pending = false;
	if (changed && !moved)
		return;
	atomic_fetch_add_explicit(&s->hook_moved_gaps, 1u, memory_order_relaxed);
	slic_hook_push(s, c, c->gap_stamp_ms, c->slic_level,
		       UDSP_HOOK_EDGE_GAP |
		       (c->gap_stall ? UDSP_HOOK_EDGE_STALL : 0u) |
		       (c->gap_irq ? UDSP_HOOK_EDGE_IRQ : 0u),
		       c->gap_gap_ms);
}

/* True when the level moved since the last sample the thread took. timed says
 * the thread samples per tick, so a gap to the previous sample is a stall;
 * irq says the interrupt edge started this pass (chan.h UDSP_HOOK_EDGE_IRQ). */
static bool slic_hook_sample(struct udsp *s, struct udsp_chan *c, uint32_t stamp,
			     bool timed, bool irq)
{
	uint64_t pub;
	uint64_t old;
	uint32_t gap = 0;
	bool level = false;
	bool moved = false;
	bool changed;
	bool stall;
	int rc;

	if (s->ops->hook_read_moved)
		rc = s->ops->hook_read_moved(s->slic, c->line, &level, &moved);
	else
		rc = s->ops->hook_read(s->slic, c->line, &level);
	if (rc < 0) {
		udsp_op_error(s);
		slic_read_fail_log(s, c, "hook", rc);
		if (++c->hook_fail_run >= UDSP_HOOK_FAIL_LIMIT) {
			char reason[48];

			snprintf(reason, sizeof(reason),
				 "after %u failed hook reads",
				 c->hook_fail_run);
			slic_line_dead(s, c, reason);
		}
		return false;
	}
	c->hook_fail_run = 0;
	slic_read_ok_log(s, c);
	stall = slic_hook_gap_note(s, c, timed);
	if (c->slic_primed) {
		gap = stamp - c->hook_stamp_prev_ms;
		if (gap > UINT16_MAX)
			gap = UINT16_MAX;
	}
	c->hook_stamp_prev_ms = stamp;

	/* The STALL mark is sticky until the tick consumes it (chan.c
	 * chan_hook_known): the next sample may publish before the tick reads
	 * this one, and a stall the tick never saw would let it credit the
	 * jump to the make it interrupted. The exchange keeps a mark the tick
	 * cleared between this load and the store from coming back. */
	old = atomic_load_explicit(&c->hook_pub, memory_order_relaxed);
	do {
		pub = UDSP_HOOK_PUB_VALID | stamp;
		if (level)
			pub |= UDSP_HOOK_PUB_LEVEL;
		if (stall || (old & UDSP_HOOK_PUB_STALL))
			pub |= UDSP_HOOK_PUB_STALL;
	} while (!atomic_compare_exchange_weak_explicit(&c->hook_pub, &old, pub,
							 memory_order_release,
							 memory_order_relaxed));

	changed = !c->slic_primed || level != c->slic_level;
	slic_gap_resolve(s, c, changed, moved);
	if (!changed) {
		if (moved) {
			c->gap_pending = true;
			c->gap_stall = stall;
			c->gap_irq = irq;
			c->gap_stamp_ms = stamp;
			c->gap_gap_ms = (uint16_t)gap;
		}
		return false;
	}
	c->slic_primed = true;
	c->slic_level = level;
	slic_hook_push(s, c, stamp, level,
		       (stall ? UDSP_HOOK_EDGE_STALL : 0u) |
		       (irq ? UDSP_HOOK_EDGE_IRQ : 0u),
		       (uint16_t)gap);
	return true;
}

/* Samples every line that is sampling; true when any level moved. */
static bool slic_hooks_sample(struct udsp *s, uint32_t stamp, bool timed,
			      bool irq)
{
	bool changed = false;

	for (unsigned i = 0; i < s->cap.lines; i++) {
		struct udsp_chan *c = &s->chan[i];

		if (!atomic_load_explicit(&c->sampling, memory_order_acquire) ||
		    atomic_load_explicit(&c->dead, memory_order_acquire))
			continue;
		changed |= slic_hook_sample(s, c, stamp, timed, irq);
	}
	return changed;
}

static const char *hook_open_class_name(uint8_t cls)
{
	switch ((enum line_hook_open_class)cls) {
	case LINE_HOOK_OPEN_NONE:
		return "none";
	case LINE_HOOK_OPEN_FLASH:
		return "flash";
	case LINE_HOOK_OPEN_PULSE:
		return "pulse";
	}
	return "?";
}

/* The tick's half of the hook trace (chan.h struct udsp_hook_trace), logged
 * here so the RT tick writes no syslog line. Drained on every wake before
 * the pass, whatever the debug mask: the tick produces nothing while the
 * mask is clear, and a test that steps one tick reads the line once the
 * pass behind that tick has sampled. The published count, not cap.lines:
 * this runs on every wake, the attach among them, while udsp_open still
 * fills the capability record. */
static void slic_hook_trace_drain(struct udsp *s)
{
	unsigned lines = udsp_lines(s);

	for (unsigned i = 0; i < lines; i++) {
		struct udsp_chan *c = &s->chan[i];
		struct udsp_hook_trace t;

		while (udsp_hook_traces_pop(&c->hook_traces, &t)) {
			if (t.kind == UDSP_HOOK_TRACE_STALL) {
				udsp_log(s, UDSP_LOG_DEBUG,
					 "hook stall line %u at %u jump %u",
					 c->line, t.stamp_ms, t.open_lo_ms);
				continue;
			}
			udsp_log(s, UDSP_LOG_DEBUG,
				 "hook open line %u at %u lo %u hi %u gap %u/%u %s",
				 c->line, t.stamp_ms, t.open_lo_ms, t.open_hi_ms,
				 t.open_gap_ms, t.close_gap_ms,
				 hook_open_class_name(t.open_class));
		}
	}
}

/* A power ratio in dB for the verdict trace, and -127.0 for a ratio of 0. The
 * tick stores the powers, so every log10 of the trace runs on this thread. */
static double slic_lec_db(double ratio)
{
	if (!(ratio > 0.0))
		return -127.0;
	return 10.0 * log10(ratio);
}

/* A mean square in int16 units against the square of full scale. */
static double slic_lec_level(float power)
{
	return slic_lec_db((double)power / (32767.0 * 32767.0));
}

static unsigned slic_lec_flag(const struct udsp_lec_trace *t, unsigned bit)
{
	return (t->flags & bit) ? 1u : 0u;
}

/* The fields that a test parses stand first, because a test sink keeps the
 * head of a line only. */
static void slic_lec_verdict_log(struct udsp *s, unsigned line,
				 const struct udsp_lec_trace *t)
{
	double excess = t->bar > 0.0f ?
			slic_lec_db((double)t->mic_power / t->bar) : 99.9;

	udsp_log(s, UDSP_LOG_DEBUG,
		 "lec verdict line %u gen %u at %u frame %u excess %.1f "
		 "hold %u arm %s trusted %u tail %u since %u nlp %s "
		 "probing %u mic %.1f ref %.1f win %.1f est %.1f noise %.1f "
		 "erle %.1f run %u stale %u",
		 line, t->gen, t->stamp_ms, t->frames, excess, t->hold_left,
		 (t->flags & UDSP_LEC_TRACE_CLASSIC) ? "classic" : "est",
		 slic_lec_flag(t, UDSP_LEC_TRACE_TRUSTED),
		 slic_lec_flag(t, UDSP_LEC_TRACE_TAIL), t->window_run - 1u,
		 udsp_ec_nlms_verdict_name(t->nlp_verdict),
		 slic_lec_flag(t, UDSP_LEC_TRACE_PROBING),
		 slic_lec_level(t->mic_power), slic_lec_level(t->ref_power),
		 slic_lec_level(t->window_power),
		 slic_lec_level(t->estimate_power),
		 slic_lec_level(t->noise_power), (double)t->erle_db, t->dt_run,
		 t->stale_run);
}

static void slic_lec_second_log(struct udsp *s, unsigned line,
				const struct udsp_lec_trace *t)
{
	udsp_log(s, UDSP_LOG_DEBUG,
		 "lec second line %u gen %u at %u frame %u fired %u dt %u "
		 "probes %u lost %u gain %.1f",
		 line, t->gen, t->stamp_ms, t->frames, t->fired, t->dt,
		 t->probes, t->lost, slic_lec_db(t->echo_gain));
}

/*
 * The tick's half of the verdict trace (chan.h struct udsp_lec_trace), logged
 * here so the tick runs no log10 and writes no syslog line. It runs after the
 * periodic pass, so the hook sample and the fault poll of a wake come before
 * any line of it. The thread can run SCHED_FIFO, and a log write is
 * synchronous. A wake therefore takes at most UDSP_LEC_TRACE_DRAIN_MAX
 * records per line and leaves the rest for the next wake. The loop runs over
 * the published line count, udsp_lines(), for the reason that
 * slic_hook_trace_drain gives.
 */
static void slic_lec_trace_drain(struct udsp *s)
{
	unsigned lines = udsp_lines(s);

	for (unsigned i = 0; i < lines; i++) {
		struct udsp_chan *c = &s->chan[i];
		struct udsp_lec_trace t;
		unsigned n = 0;

		while (n++ < UDSP_LEC_TRACE_DRAIN_MAX &&
		       udsp_lec_traces_pop(&c->lec_traces, &t)) {
			if (t.kind == UDSP_LEC_TRACE_SECOND)
				slic_lec_second_log(s, c->line, &t);
			else
				slic_lec_verdict_log(s, c->line, &t);
		}
	}
}

/* The converter and loop-status decode, the meanings AN867 assigns and
 * udsp.h carries. The converter powered down, the over-voltage lock-out, the
 * loop closed and the ringer tripped. DCDC_STATUS bit 23 is outside the bits
 * AN868 documents; AN340 (Si3217x) names it the under-voltage lock-out
 * status, and a cold boot on AN7581 reads it set with the converter down. It
 * is named provisionally and reaches the fault-edge log alone (the plug-in's
 * mpi.h carries the provenance). */
#define SLIC_PD_DCDC_DOWN	(1u << 20)
#define SLIC_DCDC_OVLO		(1u << 22)
#define SLIC_DCDC_UV		(1u << 23)
#define SLIC_LCRRTP_LCR		(1u << 1)
#define SLIC_LCRRTP_RTP		(1u << 0)

/* --- the line monitor ---------------------------------------------------- */

static void slic_monitor_log(struct udsp *s, struct udsp_chan *c,
			     const struct udsp_slic_monitor *m)
{
	int32_t d = m->vbat_mv - m->vbat_tgt_mv;

	udsp_log(s, UDSP_LOG_WARN,
		 "line %u fault 0x%x: vbat %d mV tgt %d mV vtr %d mV itr %d uA "
		 "p_hvic %d mW pd_dcdc 0x%06x dcdc_status 0x%06x%s lcrrtp 0x%02x "
		 "linefeed 0x%02x",
		 c->line, c->fault_last_read, m->vbat_mv, m->vbat_tgt_mv,
		 m->vtr_mv, m->itr_ua, m->p_hvic_mw, m->pd_dcdc & 0xffffffu,
		 m->dcdc_status & 0xffffffu,
		 (m->dcdc_status & SLIC_DCDC_UV) ? " (uv-status)" : "",
		 m->lcrrtp, m->linefeed);
	/* A sag of a regulating converter is logged, never acted on. The part
	 * tracks its own target and reduces V_VLIM, so a power cycle would
	 * make it worse. A converter in shutdown reads no battery at all. */
	if (m->pd_dcdc & SLIC_PD_DCDC_DOWN)
		return;
	if (m->vbat_tgt_mv && (d < 0 ? -d : d) > m->vbat_irq_th_mv)
		udsp_log(s, UDSP_LOG_WARN,
			 "line %u battery sag: vbat %d mV tgt %d mV th %d mV",
			 c->line, m->vbat_mv, m->vbat_tgt_mv,
			 m->vbat_irq_th_mv);
}

/* A reader holds the slot for one copy, so a few naps see it through. Past
 * that the sample is dropped and counted rather than stalling the thread. */
#define SLIC_MONITOR_PUBLISH_NAPS	50u

/*
 * The single writer of the line-monitor publish. A periodic sample that finds
 * the slot held skips, as chan_lec_publish does. The fault-edge sample is the
 * reading this part exists for, so it retries, bounded.
 */
static void slic_monitor_publish(struct udsp *s, struct udsp_chan *c,
				 const struct udsp_monitor_rec *rec,
				 bool fault_edge)
{
	struct monitor_snap *snap = &s->monitor_snap[c->line];
	unsigned naps = fault_edge ? SLIC_MONITOR_PUBLISH_NAPS : 0u;

	while (!monitor_snap_publish(snap, rec)) {
		struct timespec nap = { .tv_sec = 0, .tv_nsec = 1000000L };

		if (!naps--) {
			if (fault_edge)
				udsp_op_error(s);
			return;
		}
		if (atomic_load_explicit(&s->slic_quit, memory_order_acquire))
			return;
		nanosleep(&nap, NULL);
	}
	atomic_store_explicit(&s->monitor_valid[c->line], true,
			      memory_order_release);
}

static void slic_monitor_sample(struct udsp *s, struct udsp_chan *c,
				bool fault_edge)
{
	struct udsp_slic_monitor m;
	struct udsp_monitor_rec rec;
	int rc;

	if (!s->ops->monitor_read)
		return;
	rc = s->ops->monitor_read(s->slic, c->line, &m);
	memset(&rec, 0, sizeof(rec));
	if (rc == -ENODATA) {
		if (!c->monitor_have)
			return;
		rec.m = c->monitor_last;
		rec.stamp_ms = c->monitor_stamp_ms;
		rec.stale = 1u;
	} else if (rc < 0) {
		return;
	} else {
		c->monitor_last = m;
		c->monitor_stamp_ms = (uint32_t)(udsp_mono_us() / 1000u);
		c->monitor_have = true;
		rec.m = m;
		rec.stamp_ms = c->monitor_stamp_ms;
	}
	slic_monitor_publish(s, c, &rec, fault_edge);
	if (fault_edge)
		slic_monitor_log(s, c, &rec.m);
}

static void slic_monitor_periodic(struct udsp *s)
{
	for (unsigned i = 0; i < s->cap.lines; i++) {
		struct udsp_chan *c = &s->chan[i];

		if (!atomic_load_explicit(&c->sampling, memory_order_acquire) ||
		    atomic_load_explicit(&c->dead, memory_order_acquire))
			continue;
		slic_monitor_sample(s, c, false);
	}
}

/* --- the recovery ladder ------------------------------------------------- */

/* True when the hold reached the engine's ring; a push that fails is counted
 * and the next reading tries again. */
static bool slic_line_hold_post(struct udsp *s, struct udsp_chan *c,
				uint8_t reason)
{
	struct slic_up_rec up;

	memset(&up, 0, sizeof(up));
	up.type = SLIC_UP_LINE_HOLD;
	up.line = (uint8_t)c->line;
	up.reason = reason;
	up.bits = 1u << c->line;
	c->recover_want = UDSP_RECOVER_NONE;
	if (slic_up_ring_push(&s->up, &up))
		return true;
	udsp_op_error(s);

	return false;
}

/*
 * The Skyworks severity ladder for a converter that read down. It runs on a
 * fresh converter read, independent of the monitor cache and of the vendor
 * channel enable. An over-voltage lock-out inhibits an attempt at once. Two
 * consecutive lock-out readings hold the line, posted once, and the ladder
 * refuses every later attempt on its own. The loop closed or the ringer
 * tripped is a suspected short, re-read at the next deadline. A converter
 * down with neither selects the converter op.
 */
static void slic_converter_ladder(struct udsp *s, struct udsp_chan *c)
{
	struct udsp_slic_converter cv;

	c->recover_want = UDSP_RECOVER_NONE;
	if (c->ovlo_seen >= 2u)
		return;
	if (!s->ops->converter_read || !s->ops->converter_restart)
		return;
	if (s->ops->converter_read(s->slic, c->line, &cv) < 0)
		return;
	if (cv.dcdc_status & SLIC_DCDC_OVLO) {
		if (c->ovlo_seen == 0u)
			c->ovlo_seen = 1u;
		else if (slic_line_hold_post(s, c, UDSP_FAULT_HELD_OVLO))
			c->ovlo_seen = 2u;
		return;
	}
	c->ovlo_seen = 0;
	if (!(cv.pd_dcdc & SLIC_PD_DCDC_DOWN)) {
		c->fault_inhibit = false;
		return;
	}
	if (cv.lcrrtp & (SLIC_LCRRTP_LCR | SLIC_LCRRTP_RTP)) {
		c->fault_inhibit = true;
		return;
	}
	c->fault_inhibit = false;
	c->recover_want = UDSP_RECOVER_CONVERTER;
}

/*
 * The op a fault read selects. A DCDC bit runs the ladder and takes the
 * converter op; the ladder never escalates to a reinit. The converter read
 * behind it is one RAM word on a slow bus. While the fault stands it runs
 * on the edge and then once per holdoff, never on every pass. REINIT alone
 * takes the reinit op. A read without the DCDC bit ends the converter
 * ladder's own refusals.
 */
static void slic_recover_want_set(struct udsp *s, struct udsp_chan *c,
				  uint32_t bits, bool edge)
{
	uint32_t now_ms = udsp_clock_stamp_ms(&s->clock);

	if (bits & UDSP_FAULT_SLIC_DCDC) {
		if (!edge && (int32_t)(now_ms - c->ladder_next_ms) < 0)
			return;
		c->ladder_next_ms = now_ms + SLIC_RECOVER_HOLDOFF_MS;
		slic_converter_ladder(s, c);
		return;
	}
	c->fault_inhibit = false;
	c->ovlo_seen = 0;
	if ((bits & UDSP_FAULT_SLIC_REINIT) && s->ops->reinit)
		c->recover_want = UDSP_RECOVER_REINIT;
	else
		c->recover_want = UDSP_RECOVER_NONE;
}

static void slic_fault_lines_scan(struct udsp *s)
{
	for (unsigned i = 0; i < s->cap.lines; i++) {
		struct udsp_chan *c = &s->chan[i];
		struct slic_up_rec rec;
		uint32_t prev;
		uint32_t bits = 0;
		int rc;

		if (!atomic_load_explicit(&c->sampling, memory_order_acquire))
			continue;
		rc = s->ops->fault_read(s->slic, i, &bits);
		if (rc < 0) {
			udsp_op_error(s);
			slic_read_fail_log(s, c, "fault", rc);
			continue;
		}
		slic_read_ok_log(s, c);
		/* A dead line's fault is read for the pin it shares with the
		 * live ones (the read is the read-to-clear) and reported to
		 * no one: the line is already down. */
		if (atomic_load_explicit(&c->dead, memory_order_acquire))
			continue;
		memset(&rec, 0, sizeof(rec));
		rec.type = SLIC_UP_FAULT;
		rec.line = (uint8_t)i;
		rec.bits = bits;
		if (!slic_up_ring_push(&s->up, &rec))
			udsp_op_error(s);

		prev = c->fault_last_read;
		c->fault_last_read = bits;
		slic_recover_want_set(s, c, bits, bits != prev);
		/* The snapshot the part exists for: one reading at the edge to
		 * a nonzero fault set. */
		if (bits && bits != prev)
			slic_monitor_sample(s, c, true);
	}
}

/* The per-tick modes' fault poll: on every pass while the pin is asserted,
 * once per SLIC_FAULT_PERIOD passes otherwise. */
/* The pin is active low (it idles high) and shared
 * by every channel's interrupt sources, so a level still asserted after the
 * fault read means a source nothing cleared holds it, and no further edge
 * can come until it drops. */
static bool slic_irq_asserted(struct udsp *s)
{
	const struct udsp_slic_host *h;
	int value = 1;

	if (!s->cfg.gpio_interrupt[0] || !s->host)
		return false;
	h = slic_host_hooks(s->host);
	if (h->gpio_get(h->ctx, "slic_interrupt", &value) < 0) {
		udsp_op_error(s);
		return false;
	}
	return value == 0;
}

static void slic_fault_lines_read(struct udsp *s)
{
	slic_fault_lines_scan(s);
	s->slic_last_fault_us = udsp_mono_us();
}

/* The per-tick modes' fault poll: on every pass while the pin is asserted,
 * once per SLIC_FAULT_PERIOD passes otherwise. True while the pin is still
 * asserted after it. */
static bool slic_fault_scan(struct udsp *s)
{
	bool asserted = slic_irq_asserted(s);

	if (!asserted && s->periodic_pass % SLIC_FAULT_PERIOD)
		return false;
	slic_fault_lines_read(s);
	return asserted && slic_irq_asserted(s);
}

/* --- the sampling modes -------------------------------------------------- */

static enum slic_mode slic_mode_get(const struct udsp *s)
{
	return (enum slic_mode)atomic_load_explicit(&s->slic_mode,
						    memory_order_relaxed);
}

static void slic_mode_set(struct udsp *s, enum slic_mode mode)
{
	atomic_store_explicit(&s->slic_mode, (uint_least8_t)mode,
			      memory_order_relaxed);
}

/* The burst window runs on the engine clock, the clock the hook machine
 * measures its windows in, so it outlasts them whatever the tick rate. */
static void slic_burst_enter(struct udsp *s)
{
	if (s->slic_irq_fd < 0)
		return;
	slic_mode_set(s, SLIC_MODE_BURST);
	s->slic_burst_until_ms = udsp_clock_stamp_ms(&s->clock) + SLIC_BURST_MS;
	atomic_store_explicit(&s->slic_wants_tick, true, memory_order_release);
}

static bool slic_burst_over(const struct udsp *s, uint32_t stamp_ms)
{
	return (int32_t)(stamp_ms - s->slic_burst_until_ms) >= 0;
}

/* The fault cadence carries across the modes: the next read is due one
 * period after the last one, whichever pass ran it. */
static void slic_idle_enter(struct udsp *s, uint64_t now_us)
{
	slic_mode_set(s, SLIC_MODE_IDLE);
	s->slic_next_sample_us = now_us + (uint64_t)SLIC_IDLE_SAMPLE_MS * 1000u;
	s->slic_next_fault_us = s->slic_last_fault_us +
				(uint64_t)SLIC_IDLE_FAULT_MS * 1000u;
	s->slic_next_monitor_us = now_us +
				  (uint64_t)SLIC_IDLE_MONITOR_MS * 1000u;
	atomic_store_explicit(&s->slic_wants_tick, false, memory_order_release);
}

/* POLL and BURST: one pass per engine tick. A level change restarts the burst
 * window. */
static bool slic_tick_pass(struct udsp *s)
{
	uint64_t tick = udsp_clock_tick(&s->clock);
	uint32_t stamp;
	bool changed;
	bool held;
	bool edge = s->slic_irq_edge;

	if (tick == s->last_periodic_tick && !edge)
		return false;

	s->slic_irq_edge = false;
	s->last_periodic_tick = tick;
	stamp = udsp_clock_stamp_ms(&s->clock);
	changed = slic_hooks_sample(s, stamp, true, edge);
	held = slic_fault_scan(s);
	s->periodic_pass++;
	/* The monitor cadence in the per-tick modes: once every ten seconds of
	 * ticks, the same reach as the IDLE wall-clock deadline. */
	if (++s->slic_monitor_pass >= SLIC_MONITOR_PERIOD) {
		s->slic_monitor_pass = 0;
		slic_monitor_periodic(s);
	}
	if (slic_mode_get(s) != SLIC_MODE_BURST)
		return true;

	/* A pin a source keeps asserted (a dead channel's alarm, which the
	 * fault read skips) can raise no edge for the other lines, so the
	 * per-tick pass stays until it drops. */
	if (changed || edge || held)
		slic_burst_enter(s);
	else if (slic_burst_over(s, stamp))
		slic_idle_enter(s, udsp_mono_us());
	return true;
}

/*
 * IDLE: nothing per tick. An interrupt edge samples every line, runs the fault
 * read on every line and enters BURST. The two reads clear the sources and so
 * drop the pin. A hook_read_moved sample clears a hook source itself (slic.h),
 * and the fault read clears the rest. Between edges the SAFETY sample catches
 * a level whose edge was lost, and the fault poll keeps the alarms' cadence.
 * The stamp is the engine clock's, as on every other sample, so a hook edge
 * found here carries the tick it was found in.
 */
static bool slic_idle_pass(struct udsp *s)
{
	uint64_t now_us = udsp_mono_us();
	bool edge = s->slic_irq_edge;
	bool sample = edge || now_us >= s->slic_next_sample_us;
	bool fault = edge || now_us >= s->slic_next_fault_us;
	bool monitor = now_us >= s->slic_next_monitor_us;
	bool changed = false;

	s->slic_irq_edge = false;
	if (!sample && !fault && !monitor)
		return false;
	if (monitor) {
		uint64_t period = (uint64_t)SLIC_IDLE_MONITOR_MS * 1000u;

		slic_monitor_periodic(s);
		s->slic_next_monitor_us = now_us + period;
	}
	if (sample) {
		changed = slic_hooks_sample(s, udsp_clock_stamp_ms(&s->clock),
					    false, edge);
		s->slic_next_sample_us = now_us +
					 (uint64_t)SLIC_IDLE_SAMPLE_MS * 1000u;
	}
	if (fault) {
		slic_fault_lines_read(s);
		s->slic_next_fault_us = s->slic_last_fault_us +
					(uint64_t)SLIC_IDLE_FAULT_MS * 1000u;
		/* A pin still asserted after the read raises no edge for the
		 * next transition; the per-tick pass watches it until it
		 * drops. */
		if (slic_irq_asserted(s))
			edge = true;
	}
	s->last_periodic_tick = udsp_clock_tick(&s->clock);
	if (edge || changed)
		slic_burst_enter(s);
	return true;
}

static void slic_dead_retry_step(struct udsp *s);

static bool slic_periodic(struct udsp *s)
{
	bool ran;

	if (slic_mode_get(s) == SLIC_MODE_IDLE)
		ran = slic_idle_pass(s);
	else
		ran = slic_tick_pass(s);
	slic_dead_retry_step(s);

	return ran;
}

/* The wait until the next IDLE deadline, in the shape ppoll takes; one second
 * in the per-tick modes, where the engine's tick wake ends every wait. */
static struct timespec slic_wait_get(const struct udsp *s)
{
	struct timespec rel = { .tv_sec = 1, .tv_nsec = 0 };
	uint64_t now_us;
	uint64_t next;

	if (slic_mode_get(s) != SLIC_MODE_IDLE)
		return rel;
	now_us = udsp_mono_us();
	next = s->slic_next_sample_us < s->slic_next_fault_us ?
	       s->slic_next_sample_us : s->slic_next_fault_us;
	if (next <= now_us + 1000u)
		next = now_us + 1000u;
	rel.tv_sec = (time_t)((next - now_us) / 1000000u);
	rel.tv_nsec = (long)((next - now_us) % 1000000u) * 1000L;
	return rel;
}

/* The edge acknowledgement: a read at offset 0 clears the kernfs event of a
 * sysfs value node and the count of an eventfd alike. The level itself is
 * read again by the pass that follows. */
static void slic_irq_ack(struct udsp *s)
{
	uint8_t buf[8];

	lseek(s->slic_irq_fd, 0, SEEK_SET);
	if (read(s->slic_irq_fd, buf, sizeof(buf)) < 0 && errno != EAGAIN)
		udsp_op_error(s);
	s->slic_irq_edge = true;
}

/* The edge source went away under the wait (a closed descriptor reports
 * POLLNVAL on every ppoll and would spin the loop): the thread goes back to
 * the per-tick pass for the life of the handle. */
static void slic_irq_lost(struct udsp *s)
{
	udsp_op_error(s);
	s->slic_irq_fd = -1;
	s->slic_irq_edge = false;
	slic_mode_set(s, SLIC_MODE_POLL);
	atomic_store_explicit(&s->slic_wants_tick, true, memory_order_release);
}

/* The host may reopen its value node behind a read error (a pin exported
 * again), so the number the wait uses is the host's current one.
 * A host that holds none is the lost edge source, and a host that holds one
 * again gets its per-tick pass back until the burst window closes. */
static void slic_irq_fd_refresh(struct udsp *s)
{
	int fd;

	if (!s->slic_irq_host)
		return;
	fd = slic_host_irq_fd(s->host);
	if (fd == s->slic_irq_fd)
		return;
	if (fd < 0) {
		slic_irq_lost(s);
		return;
	}
	s->slic_irq_fd = fd;
	if (slic_mode_get(s) == SLIC_MODE_POLL)
		slic_burst_enter(s);
}

/* --- jobs ---------------------------------------------------------------- */

/*
 * A completion is never discarded (event.h: the drop policy excludes it). The
 * emitter can be held for a long time on a blocked event fd, so the ring can
 * fill; this thread is the SLOW worker and may wait, so it retries the
 * insertion with a backoff until space appears. The only exit
 * is udsp_close stopping the thread, which is exactly when nobody is left to
 * receive the record.
 */
static void slic_job_done(struct udsp *s, const struct slic_job *job, int status)
{
	if (job->ack) {
		cmd_ack_post(job->ack, job->token, status);
		return;
	}

	for (;;) {
		struct timespec nap = { .tv_sec = 0, .tv_nsec = 1000000L };

		if (udsp_emit_completion(&s->emitter, UDSP_EV_PROD_JOB_SLIC,
					 job->token, status))
			return;
		if (atomic_load_explicit(&s->slic_quit, memory_order_acquire))
			return;
		nanosleep(&nap, NULL);
	}
}

static int slic_job_attach(struct udsp *s)
{
	const struct udsp_slic_host *hooks = s->host ? slic_host_hooks(s->host) :
						       NULL;
	unsigned channels = 0;

	slic_slow_enter(s);
	s->slic = s->ops->attach(hooks, &channels);
	slic_slow_leave(s);
	if (!s->slic)
		return -EIO;
	s->attach_channels = channels;
	/* part_name is a plug-in op, so it runs here and not on the control
	 * thread that builds the capability record (slic.h rule 1). */
	if (s->ops->part_name) {
		const char *name = s->ops->part_name(s->slic);

		if (name)
			snprintf(s->part_name, sizeof(s->part_name), "%s", name);
	}

	return udsp_safe_state_arm(s->slic, s->ops, s->host);
}

/*
 * region_set runs ProSLIC_ZsynthSetup, which resets the gain RAM to the
 * impedance preset's 0 dB plan; a failed gain re-apply inside region_set
 * leaves the RAM at that default while gain_confirmed still claims the old
 * pair, so a later chan_config with the matching pair would be wrongly
 * suppressed. The pair is therefore invalidated after every region_set call,
 * success or failure, the same
 * invalidation slic_job_start does before init; the next chan_config
 * re-emits it (uvoice pushes one at every seize).
 */
static int slic_region_line_set(struct udsp *s, struct udsp_chan *c,
				const char *iso3)
{
	int rc = s->ops->region_set(s->slic, c->line, iso3);

	atomic_store_explicit(&c->gain_confirmed, 0u, memory_order_release);

	return rc;
}

static void slic_region_fail_log(struct udsp *s, const char *iso3, unsigned line,
				 int rc)
{
	udsp_log(s, UDSP_LOG_WARN, "slic region %s failed on line %u: %s", iso3,
		 line, strerror(-rc));
}

/*
 * A region_set failure never enters the region job's status. The tone-table
 * swap already happened and is not rolled back, and uvoice reads a non-OK
 * region status as "no tone profile" (region_applied blanks and cw_cid_defer
 * then drops every CW caller-ID record), so a SLIC impedance write failure
 * must not masquerade as a tone-table failure. Each failing line is counted
 * and logged instead, and the later lines still run.
 */
static void slic_region_program(struct udsp *s)
{
	const struct udsp_region *r = udsp_region_active(s);

	if (!r || !s->ops->region_set)
		return;

	for (unsigned i = 0; i < s->cap.lines; i++) {
		struct udsp_chan *c = &s->chan[i];
		int rc;

		if (!c->live ||
		    atomic_load_explicit(&c->dead, memory_order_acquire))
			continue;
		rc = slic_region_line_set(s, c, r->iso3);
		if (!rc)
			continue;
		udsp_op_error(s);
		slic_region_fail_log(s, r->iso3, i, rc);
	}
}

/* The start-time region write. The part is inited when it fails, so the error
 * is counted and warned and the line stays up; what the part holds after a
 * write that failed midway is the plug-in's to report. */
static void slic_region_line_start(struct udsp *s, struct udsp_chan *c,
				   const struct udsp_region *region)
{
	int rc;

	if (!region || !s->ops->region_set)
		return;
	rc = slic_region_line_set(s, c, region->iso3);
	if (!rc)
		return;
	udsp_op_error(s);
	slic_region_fail_log(s, region->iso3, c->line, rc);
}

/*
 * The per-line half of a successful init, shared by the start job and the
 * dead-line retry.
 *
 * ProSLIC_ZsynthSetup rewrites TXACGAIN and RXACGAIN from the impedance
 * preset's 0 dB plan on every init. Init therefore makes the hardware gain
 * state unknown. The confirmed pair is invalidated, so the LINES_UP emit is
 * never suppressed against a pre-init confirmation.
 *
 * A region is active here only behind an open-time udsp_config.region or an
 * earlier region load (uvoice loads the country on the START completion).
 * Init just reset the presets to the profile defaults, so the country is
 * re-programmed per line. The region failure arm follows the bring-up gain
 * precedent, not the init verdict: a region job answers with its own status.
 */
static void slic_line_up(struct udsp *s, struct udsp_chan *c)
{
	atomic_store_explicit(&c->gain_confirmed, 0u, memory_order_release);
	atomic_store_explicit(&c->dead, false, memory_order_release);
	c->hook_fail_run = 0;
	c->slic_primed = false;
	c->hook_gap_armed = false;
	c->gap_pending = false;
	c->gap_stall = false;
	c->edge_lost = false;
	c->live = true;
	slic_region_line_start(s, c, udsp_region_active(s));
}

static int slic_job_start(struct udsp *s)
{
	uint32_t ready = 0;
	int32_t status = 0;
	int rc;

	rc = udsp_engine_create(s);
	if (rc) {
		udsp_engine_fail_log(s);
		return rc;
	}
	udsp_engine_gate(s);

	slic_slow_enter(s);
	for (unsigned i = 0; i < s->cap.lines; i++) {
		struct udsp_chan *c = &s->chan[i];

		rc = s->ops->init(s->slic, i);
		if (rc < 0) {
			char reason[48];

			/* A half-populated dual part is a legitimate board, so
			 * the verdict is per channel. A line that was live
			 * before this start is energised, and once it is dead
			 * no later write reaches it, so the feed is opened best
			 * effort first. */
			if (c->live) {
				if (!s->ops->linefeed_set(s->slic, i,
							  UDSP_LF_OPEN))
					atomic_store_explicit(&c->lf_confirmed,
							      UDSP_LF_OPEN,
							      memory_order_release);
				c->live = false;
			}
			udsp_op_error(s);
			snprintf(reason, sizeof(reason),
				 "after a failed init: %s", strerror(-rc));
			slic_line_dead(s, c, reason);
			continue;
		}
		slic_line_up(s, c);
		ready |= 1u << i;
	}
	slic_slow_leave(s);
	/* A device-wide failure needs the reset pulse only a fresh udsp_open
	 * supplies, so the caller's respawn stays the recovery. The store
	 * covers a repeated start on a started handle: no dead-line retry
	 * runs behind an all-dead verdict. */
	if (!ready) {
		atomic_store_explicit(&s->started, false, memory_order_release);
		return -EIO;
	}

	/* The AN7581 back end deferred its DMA start so the SLIC init ran with
	 * no capture stream that could reset the PCM block mid-init; bring the
	 * media plane up now, before LINES_UP waits on the tick to ack. A no-op
	 * on a back end that already streams. */
	rc = udsp_engine_start_streaming(s);
	if (rc) {
		udsp_log(s, UDSP_LOG_ERR, "engine start failed: %s",
			 strerror(-rc));
		atomic_store_explicit(&s->started, false, memory_order_release);
		return rc;
	}

	rc = slic_up_post(s, SLIC_UP_LINES_UP, ready, SLIC_ACK_WAIT_MS, &status);
	if (rc)
		return rc;

	for (unsigned i = 0; i < s->cap.lines; i++) {
		if (ready & (1u << i))
			atomic_store_explicit(&s->chan[i].sampling, true,
					      memory_order_release);
	}
	/* The lines' first samples, the boot report of the hook machine among
	 * them, come at tick rate and not at the SAFETY cadence. */
	slic_burst_enter(s);
	atomic_store_explicit(&s->started, true, memory_order_release);

	return status ? status : 0;
}

static void slic_recover_pending_finish(struct udsp *s, unsigned lines);

static int slic_job_stop(struct udsp *s)
{
	int32_t status = 0;
	int rc;

	/* A DONE still due would arrive on a stopped line after the safe pass
	 * and bring it up again; it is finished here instead. */
	slic_recover_pending_finish(s, udsp_lines(s));
	for (unsigned i = 0; i < s->cap.lines; i++) {
		struct udsp_chan *c = &s->chan[i];

		atomic_store_explicit(&c->sampling, false,
				      memory_order_release);
		/* The recovery ladder is slic-thread state; the stop clears it,
		 * so a start does not resume a recovery the stop cut. */
		c->recover_want = UDSP_RECOVER_NONE;
		c->recover_tries = 0;
		c->ovlo_seen = 0;
		c->fault_inhibit = false;
	}

	/* An engine that stopped ticking can never ack, so the safe pass is
	 * bounded by the engine's own stop deadline. A parked tick (a deferred
	 * engine that never started) has no line up and cannot ack either, so the
	 * pass is skipped. A live tick that never reached started, an init that
	 * energised a line then lost the LINES_UP ack among them, still safes its
	 * feeds. */
	if (!atomic_load_explicit(&s->tick_parked, memory_order_acquire)) {
		rc = slic_up_post(s, SLIC_UP_LINES_SAFE, 0, UDSP_STOP_ENGINE_MS,
				  &status);
		if (rc)
			status = rc;
	}
	atomic_store_explicit(&s->started, false, memory_order_release);

	rc = udsp_engine_destroy(s, UDSP_EV_PROD_SLIC);

	return rc ? rc : status;
}

/*
 * The line belongs to the plug-in for the whole test, and two marks say so.
 *
 * Hook sampling stops, because the plug-in opens the linefeed and rings the
 * line at 16 Vrms for the ringer measurement: a sampled level would reach the
 * core as a hook edge and seize a call on a line nobody lifted. The published
 * pair is invalidated with it, so udsp_line_hook_query answers UDSP_TRANSIENT
 * instead of a level the test produced. The previous sampling flag is
 * returned, because a line that was not being sampled must not start being
 * sampled here.
 *
 * The second mark refuses every op that drives the line until the test
 * returns (core/udsp.c line_under_test). The gate this thread just re-tested
 * only proves the line was free when the job started.
 */
static bool slic_line_hold(struct udsp *s, struct udsp_chan *c)
{
	bool was = atomic_exchange_explicit(&c->sampling, false,
					    memory_order_acq_rel);

	/* A candidate the next sample would have resolved is confirmed: the
	 * hold has no next sample, and a lost pair must not vanish. */
	slic_gap_resolve(s, c, false, true);
	atomic_store_explicit(&c->hook_pub, 0u, memory_order_release);
	atomic_store_explicit(&c->under_test, true, memory_order_release);

	return was;
}

/* One fresh sample before the line is handed back, so the published level is
 * the level the line holds now and not the one it held before the test. */
static void slic_line_release(struct udsp *s, struct udsp_chan *c,
				   bool was)
{
	atomic_store_explicit(&c->sampling, was, memory_order_release);
	/* A level that moved during the test is a hook change like any
	 * other: the machine gets tick-rate samples behind it. */
	if (was && slic_hook_sample(s, c, udsp_clock_stamp_ms(&s->clock), false,
				    false))
		slic_burst_enter(s);
	atomic_store_explicit(&c->under_test, false, memory_order_release);
}

static void slic_test_result_clear(struct udsp *s, unsigned line)
{
	atomic_store_explicit(&s->line_test[line].gen, 0u,
			      memory_order_release);
}

/*
 * The record first, the generation after it, so a reader that sees a nonzero
 * generation sees a whole record (udsp_priv.h struct udsp_line_test_slot).
 *
 * The publisher steps aside while a reader copies (core/snap.h). This thread
 * is the SLOW worker and may wait, so it waits for that reader instead of
 * dropping the result of a test that took seconds. The only exit is a handle
 * that is closing, which is exactly when nobody is left to read the record.
 */
static void slic_test_result_store(struct udsp *s, unsigned line,
				   const struct udsp_slic_mlt_result *rec)
{
	struct udsp_line_test_slot *slot = &s->line_test[line];

	while (!line_test_snap_publish(&slot->snap, rec)) {
		struct timespec nap = { .tv_sec = 0, .tv_nsec = 1000000L };

		if (atomic_load_explicit(&s->slic_quit, memory_order_acquire))
			return;
		nanosleep(&nap, NULL);
	}
	if (!++s->line_test_gen)
		s->line_test_gen = 1u;
	atomic_store_explicit(&slot->gen, s->line_test_gen,
			      memory_order_release);
}

/* The one claim libudsp can check, because it decodes none of the bytes
 * (slic.h): a length that fits the record the plug-in wrote. */
static bool slic_test_len_ok(const struct udsp_slic_mlt_result *rec)
{
	return rec->len && rec->len <= sizeof(rec->data);
}

/*
 * A measurement that did not finish is not a broken line, and the caller must
 * not read it as one: the core it serves shuts a line down on a fatal op. The
 * plug-in reports the code that ended the run in the status byte of a head, so
 * a record that can hold a head is kept and the job completes -EBUSY, which
 * the caller reads as a transient answer it may ask again. An argument the
 * plug-in refuses and a part it does not carry keep their own errno, because
 * asking again cannot change either.
 *
 * A length outside the record is checked before that: a plug-in that reports
 * one broke its contract whatever errno came with it, and that answer is not
 * a request to try again.
 */
static int slic_test_fail(struct udsp *s, unsigned line, int rc,
			  const struct udsp_slic_mlt_result *rec)
{
	if (rc == -EINVAL || rc == -ENOSYS)
		return rc;
	if (!slic_test_len_ok(rec))
		return -EPROTO;
	if (rec->len >= SLIC_MLT_HEAD_BYTES)
		slic_test_result_store(s, line, rec);

	return -EBUSY;
}

/*
 * One metallic line test. The line gate is tested again here, because the
 * state can change between the submission and this thread: a line that took a
 * call, started a ring session, armed a burst or armed a delayed loop
 * meanwhile completes -EBUSY and the plug-in is never called. That completion
 * leaves the result slot cleared, which is what tells it apart from a test
 * that ran and failed.
 *
 * The op blocks for the whole test, seconds on real hardware, and has no
 * cancel. This thread runs one job step before it samples the hook, so every
 * other line's hook sampling and fault poll stall for that time as well. That
 * is the cost of the SLOW job shape, and it is why only one job runs at a
 * time.
 *
 * A record is stored only when its length fits the record the plug-in wrote.
 * libudsp never decodes the bytes (slic.h), so the length is the only thing it
 * can check, and a successful run that reports an impossible length is a
 * plug-in defect the caller must not read as a result.
 */
/*
 * One fresh hook sample before the hold, and whether it reads off hook. The
 * hold stops sampling for the whole test, so a handset lifted shortly before
 * it, inside the debounce of the machine above, would hear the metallic test
 * and no dial tone until the test ends. The sample posts its edge like any
 * other sample does, so the machine gets the seize at once.
 */
static bool slic_line_test_offhook(struct udsp *s, struct udsp_chan *c)
{
	if (!atomic_load_explicit(&c->sampling, memory_order_acquire))
		return false;
	if (slic_hook_sample(s, c, udsp_clock_stamp_ms(&s->clock), false,
			     false))
		slic_burst_enter(s);

	return c->slic_primed && c->slic_level;
}

static int slic_job_line_test(struct udsp *s, const struct slic_job *job)
{
	struct udsp_chan *c = &s->chan[job->line];
	struct udsp_slic_mlt_result rec;
	bool was;
	int rc;

	slic_test_result_clear(s, job->line);
	/* The line can take a call, a session or a delayed loop between the
	 * submission and this step, and the test drives the wire all three
	 * play into. An off-hook line is refused the same way. */
	if (udsp_line_busy(s, job->line) || udsp_line_loop_armed(s, job->line))
		return -EBUSY;
	if (slic_line_test_offhook(s, c))
		return -EBUSY;

	memset(&rec, 0, sizeof(rec));
	was = slic_line_hold(s, c);
	slic_slow_enter(s);
	rc = s->ops->mlt_run(s->slic, job->line, job->test, &rec);
	slic_slow_leave(s);
	slic_line_release(s, c, was);
	if (rc)
		return slic_test_fail(s, job->line, rc, &rec);
	if (!slic_test_len_ok(&rec))
		return -EPROTO;
	slic_test_result_store(s, job->line, &rec);

	return 0;
}

/* --- the per-channel recovery -------------------------------------------- */

static bool slic_recover_op_present(const struct udsp *s,
				    const struct udsp_chan *c)
{
	if (c->recover_want == UDSP_RECOVER_CONVERTER)
		return s->ops->converter_restart != NULL;
	if (c->recover_want == UDSP_RECOVER_REINIT)
		return s->ops->reinit != NULL;

	return false;
}

static bool slic_jobq_empty(const struct udsp *s)
{
	return atomic_load_explicit(&s->jobs.head, memory_order_acquire) ==
	       atomic_load_explicit(&s->jobs.tail, memory_order_relaxed);
}

static bool slic_line_recover_eligible(struct udsp *s, struct udsp_chan *c)
{
	if (c->recover_want == UDSP_RECOVER_NONE || c->fault_inhibit)
		return false;
	/* The engine's hold stands until the next start; the published word
	 * is the one engine field this thread reads. A success here would
	 * lift it through chan_lines_up. */
	if (atomic_load_explicit(&c->fault_pub, memory_order_acquire) &
	    UDSP_FAULT_PUB_HELD)
		return false;
	if (!atomic_load_explicit(&s->started, memory_order_acquire))
		return false;
	if (!atomic_load_explicit(&c->sampling, memory_order_acquire))
		return false;
	if (!c->live || atomic_load_explicit(&c->dead, memory_order_acquire))
		return false;
	if (atomic_load_explicit(&c->under_test, memory_order_acquire))
		return false;
	if (c->recover_tries >= SLIC_RECOVER_TRY_MAX)
		return false;
	if ((int32_t)(udsp_clock_stamp_ms(&s->clock) -
		      c->recover_holdoff_ms) < 0)
		return false;
	if (!slic_recover_op_present(s, c))
		return false;
	/* A queued stop takes priority, because slic_job_step pops one job
	 * per pass. */
	return slic_jobq_empty(s);
}

static void slic_recover_holdoff_arm(struct udsp *s, struct udsp_chan *c)
{
	c->recover_holdoff_ms = udsp_clock_stamp_ms(&s->clock) +
				SLIC_RECOVER_HOLDOFF_MS;
}

/* The line is out of the fault scan while its DONE is due. It is back in
 * once the DONE is acked or the re-post budget runs out. A lost DONE
 * therefore cannot keep it out of the scan for ever. */
static void slic_recover_finish(struct udsp *s, struct udsp_chan *c)
{
	slic_line_release(s, c, c->recover_done_was);
	atomic_fetch_sub_explicit(&s->recover_jobs, 1u, memory_order_acq_rel);
	atomic_fetch_sub_explicit(&s->job_inflight, 1u, memory_order_acq_rel);
}

static void slic_recover_done_send(struct udsp *s, struct udsp_chan *c,
				   int32_t rc)
{
	int32_t status = 0;

	if (slic_up_post_rc(s, SLIC_UP_LINE_RECOVER_DONE, 1u << c->line, rc,
			    SLIC_ACK_WAIT_MS, &status)) {
		c->recover_done_pending = true;
		c->recover_done_rc = rc;
		c->recover_done_tries = 0;
		return;
	}
	slic_recover_finish(s, c);
}

static void slic_recover_done_repost(struct udsp *s, struct udsp_chan *c)
{
	int32_t status = 0;

	if (!slic_up_post_rc(s, SLIC_UP_LINE_RECOVER_DONE, 1u << c->line,
			     c->recover_done_rc, SLIC_ACK_WAIT_MS, &status)) {
		c->recover_done_pending = false;
		slic_recover_finish(s, c);
		return;
	}
	if (++c->recover_done_tries >= SLIC_DONE_RETRY_MAX) {
		c->recover_done_pending = false;
		slic_recover_finish(s, c);
	}
}

/*
 * One recovery attempt of one line, modelled on slic_job_start and
 * slic_job_line_test. The hold comes first, then a re-check of the line's
 * traffic, then the inflight count so udsp_guard answers a sibling's
 * SYNC-SLIC op UDSP_TRANSIENT. A refused BEGIN undoes both and retries after
 * the holdoff, with no vendor call.
 */
static void slic_line_recover(struct udsp *s, struct udsp_chan *c)
{
	bool converter = c->recover_want == UDSP_RECOVER_CONVERTER;
	int32_t status = 0;
	bool was;
	int rc;

	/* The words the scan decided on are stale by now. A lock-out latched
	 * since would be erased by the vendor power-up, so the ladder runs
	 * again on a fresh read before every converter attempt. */
	if (converter) {
		slic_converter_ladder(s, c);
		if (c->recover_want != UDSP_RECOVER_CONVERTER)
			return;
	}
	was = slic_line_hold(s, c);
	c->recover_done_was = was;
	if (udsp_line_busy(s, c->line)) {
		slic_line_release(s, c, was);
		slic_recover_holdoff_arm(s, c);
		return;
	}
	atomic_fetch_add_explicit(&s->job_inflight, 1u, memory_order_acq_rel);
	atomic_fetch_add_explicit(&s->recover_jobs, 1u, memory_order_acq_rel);
	if (slic_up_post(s, SLIC_UP_LINE_RECOVER_BEGIN, 1u << c->line,
			 SLIC_ACK_WAIT_MS, &status)) {
		atomic_fetch_sub_explicit(&s->recover_jobs, 1u,
					  memory_order_acq_rel);
		atomic_fetch_sub_explicit(&s->job_inflight, 1u,
					  memory_order_acq_rel);
		slic_line_release(s, c, was);
		slic_recover_holdoff_arm(s, c);
		return;
	}

	/* The engine entered the recovery on the ack, but its open write
	 * failed on the bus. The line is not open, so the op does not run.
	 * The attempt counts as failed, with the ordinary holdoff. */
	if (status < 0) {
		rc = status;
	} else {
		slic_slow_enter(s);
		rc = converter ? s->ops->converter_restart(s->slic, c->line) :
				 s->ops->reinit(s->slic, c->line);
		slic_slow_leave(s);
	}
	/* A close arrived during the op: the engine is gone, so nothing reads a
	 * DONE. Release the line and drop the counts so udsp_close joins. */
	if (atomic_load_explicit(&s->slic_quit, memory_order_acquire)) {
		slic_line_release(s, c, was);
		atomic_fetch_sub_explicit(&s->recover_jobs, 1u,
					  memory_order_acq_rel);
		atomic_fetch_sub_explicit(&s->job_inflight, 1u,
					  memory_order_acq_rel);
		return;
	}
	if (!rc) {
		slic_region_line_start(s, c, udsp_region_active(s));
		atomic_store_explicit(&c->gain_confirmed, 0u,
				      memory_order_release);
		c->recover_want = UDSP_RECOVER_NONE;
		c->recover_tries = 0;
		slic_recover_done_send(s, c, 0);
		return;
	}

	c->recover_tries++;
	slic_recover_holdoff_arm(s, c);
	if (c->recover_tries >= SLIC_RECOVER_TRY_MAX) {
		char reason[48];

		snprintf(reason, sizeof(reason), "after %u failed %s attempts",
			 c->recover_tries, converter ? "converter" : "reinit");
		udsp_log(s, UDSP_LOG_ERR, "slic line %u held %s", c->line,
			 reason);
		c->recover_want = UDSP_RECOVER_NONE;
		slic_recover_done_send(s, c, -ENOTRECOVERABLE);
		return;
	}
	/* An intermediate failure leaves the feed open and the line eligible;
	 * the engine keeps the reported bits standing. */
	slic_recover_done_send(s, c, rc);
}

/* A DONE that can no longer reach a running engine (a close, a stop) is
 * finished here: the line is released and the counts drop. */
static void slic_recover_pending_finish(struct udsp *s, unsigned lines)
{
	for (unsigned i = 0; i < lines; i++) {
		struct udsp_chan *c = &s->chan[i];

		if (!c->recover_done_pending)
			continue;
		c->recover_done_pending = false;
		slic_recover_finish(s, c);
	}
}

/* The engine gave a recovery up as lost. Log it and drop a DONE still due.
 * Exhaust the tries, so no later scan starts a recovery that would lift the
 * hold before the next start. True when the line was in that state. */
static bool slic_recover_lost_ack(struct udsp *s, struct udsp_chan *c)
{
	if (!atomic_exchange_explicit(&c->recover_lost, false,
				      memory_order_acq_rel))
		return false;
	udsp_log(s, UDSP_LOG_WARN,
		 "slic line %u recovery result lost, line held", c->line);
	if (c->recover_done_pending) {
		c->recover_done_pending = false;
		slic_recover_finish(s, c);
	}
	c->recover_want = UDSP_RECOVER_NONE;
	c->recover_tries = SLIC_RECOVER_TRY_MAX;

	return true;
}

/* Called once per loop pass. A lost result and a pending DONE re-post come
 * first, then one eligible line. A recovery therefore never starves the
 * hook sampling of the others by more than one op at a time. */
static void slic_recover_step(struct udsp *s)
{
	/* The published count, not cap.lines: this runs on every pass, the
	 * open included, where the control thread still writes the record. */
	unsigned lines = udsp_lines(s);

	if (atomic_load_explicit(&s->slic_quit, memory_order_acquire)) {
		slic_recover_pending_finish(s, lines);
		return;
	}
	for (unsigned i = 0; i < lines; i++) {
		struct udsp_chan *c = &s->chan[i];

		if (slic_recover_lost_ack(s, c))
			return;
		/* A queued job comes first: a stop finishes a pending DONE
		 * itself, and must not wait out its ack timeouts. */
		if (c->recover_done_pending && slic_jobq_empty(s)) {
			slic_recover_done_repost(s, c);
			return;
		}
	}
	for (unsigned i = 0; i < lines; i++) {
		struct udsp_chan *c = &s->chan[i];

		if (slic_line_recover_eligible(s, c)) {
			slic_line_recover(s, c);
			return;
		}
	}
}

/*
 * The retried line came back: the engine takes it through the same LINES_UP
 * record the start posts, with this line's mask alone. The ACTIVE event is
 * posted here, beside the DOWN event slic_line_dead posts, so the two edges
 * share one producer. A lost ack is a death: the engine may already serve
 * the line, so the DEAD record and the DOWN event reconcile it.
 *
 * A negative ack status is a bring-up write that failed on the bus.
 * slic_job_start reports that status to its caller. The retry has no caller,
 * so the line dies the same way and the next deadline retries.
 */
static void slic_line_return(struct udsp *s, struct udsp_chan *c)
{
	struct udsp_event ev;
	int32_t status = 0;

	slic_line_up(s, c);
	if (slic_up_post(s, SLIC_UP_LINES_UP, 1u << c->line, SLIC_ACK_WAIT_MS,
			 &status)) {
		slic_line_dead(s, c, "after a lost bring-up ack");
		return;
	}
	if (status < 0) {
		slic_line_dead(s, c, "after a failed bring-up write");
		return;
	}
	atomic_store_explicit(&c->sampling, true, memory_order_release);
	/* The first samples come at tick rate, as after a start: the boot
	 * report of the hook machine is among them. */
	slic_burst_enter(s);
	udsp_log(s, UDSP_LOG_INFO, "slic line %u back after init retry",
		 c->line);
	memset(&ev, 0, sizeof(ev));
	ev.type = UDSP_EV_LINE;
	ev.line = (uint16_t)c->line;
	ev.stamp_ms = udsp_now_ms(s);
	ev.u.line.state = UDSP_LINE_ACTIVE;
	udsp_emit(&s->emitter, UDSP_EV_PROD_SLIC, &ev);
}

/*
 * One init retry of one dead line. The plug-in's init blocks the thread the
 * way a recovery op does, so the counts a recovery holds are held here too:
 * a sibling's SYNC-SLIC op answers UDSP_TRANSIENT instead of eating its
 * bound, and udsp_close waits for the op before the ordinary thread stop.
 * A failed attempt changes nothing: the line stays dead and silent, and the
 * state-change logs stay the death line and the return line.
 */
static void slic_line_retry(struct udsp *s, struct udsp_chan *c)
{
	int rc;

	atomic_fetch_add_explicit(&s->job_inflight, 1u, memory_order_acq_rel);
	atomic_fetch_add_explicit(&s->recover_jobs, 1u, memory_order_acq_rel);
	slic_slow_enter(s);
	rc = s->ops->init(s->slic, c->line);
	slic_slow_leave(s);
	/* A close arrived during the op: the engine is gone, so nothing reads a
	 * LINE_RETURN. The counts drop and udsp_close joins. */
	if (rc >= 0 &&
	    !atomic_load_explicit(&s->slic_quit, memory_order_acquire))
		slic_line_return(s, c);
	/* Armed after the op, so a slow failing init on one line cannot hold
	 * a sibling overdue for its own turn on every pass. */
	c->dead_retry_us = udsp_mono_us() + (uint64_t)s->dead_retry_ms * 1000u;
	atomic_fetch_sub_explicit(&s->recover_jobs, 1u, memory_order_acq_rel);
	atomic_fetch_sub_explicit(&s->job_inflight, 1u, memory_order_acq_rel);
}

/*
 * The dead-line retry of the periodic pass, the slic_recover_step shape: at
 * most one line per pass. A queued job wins, so a stop never waits out an
 * init. No retry runs before the first start or after a stop, because
 * started is false then. None runs during a close, because slic_quit is
 * set. None runs on a line the plug-in holds for a metallic line test.
 *
 * A busy line waits. A hook-fail death can fall inside a call, a ring
 * session or a caller identity burst. A vendor init must not run under any
 * of them. The deadline stays due, so the first pass that finds the line
 * free retries it.
 */
static void slic_dead_retry_step(struct udsp *s)
{
	unsigned lines = udsp_lines(s);
	uint64_t now_us;

	if (atomic_load_explicit(&s->slic_quit, memory_order_acquire))
		return;
	if (!atomic_load_explicit(&s->started, memory_order_acquire))
		return;
	if (!slic_jobq_empty(s))
		return;
	now_us = udsp_mono_us();
	for (unsigned i = 0; i < lines; i++) {
		struct udsp_chan *c = &s->chan[i];

		if (!atomic_load_explicit(&c->dead, memory_order_acquire))
			continue;
		if (atomic_load_explicit(&c->under_test, memory_order_acquire))
			continue;
		if (now_us < c->dead_retry_us)
			continue;
		if (udsp_line_busy(s, c->line))
			continue;
		slic_line_retry(s, c);
		return;
	}
}

/* The counts drop before the completion is published, or the caller that
 * reacts to JOB_DONE would still be refused UDSP_TRANSIENT by a job that has
 * already finished. udsp_close reads the second count to choose how long it
 * waits (udsp_priv.h UDSP_STOP_TEST_MS). */
static void slic_job_retire(struct udsp *s, const struct slic_job *job)
{
	if (job->kind == SLIC_JOB_LINE_TEST)
		atomic_fetch_sub_explicit(&s->line_test_jobs, 1u,
					  memory_order_acq_rel);
	atomic_fetch_sub_explicit(&s->job_inflight, 1u, memory_order_acq_rel);
}

static void slic_job_step(struct udsp *s)
{
	struct slic_job job;
	int rc;

	if (!slic_jobq_pop(&s->jobs, &job))
		return;

	if (slic_job_superseded(&s->jobs, &job)) {
		slic_job_retire(s, &job);
		slic_job_done(s, &job, -ECANCELED);
		return;
	}

	switch (job.kind) {
	case SLIC_JOB_ATTACH:
		rc = slic_job_attach(s);
		break;
	case SLIC_JOB_START:
		rc = slic_job_start(s);
		break;
	case SLIC_JOB_STOP:
		rc = slic_job_stop(s);
		break;
	case SLIC_JOB_REGION:
		/* A file parse and its allocations, then a write of every
		 * line: not real-time work. */
		slic_slow_enter(s);
		rc = udsp_region_job_run(s, job.path);
		/* Only after the run returned 0 is the swap engine-acked and
		 * the new region visible to this thread, so the plug-in is
		 * never programmed for a country the engine does not run yet.
		 * The job status reports the table swap alone. */
		if (!rc)
			slic_region_program(s);
		slic_slow_leave(s);
		break;
	case SLIC_JOB_LINE_TEST:
		rc = slic_job_line_test(s, &job);
		break;
	default:
		rc = -ENOTSUP;
		break;
	}

	slic_job_retire(s, &job);
	slic_job_done(s, &job, rc);
}

/* --- the loop ------------------------------------------------------------ */

static bool slic_pending(struct udsp *s)
{
	if (atomic_load_explicit(&s->slic_quit, memory_order_acquire))
		return true;
	if (atomic_load_explicit(&s->ring.head, memory_order_acquire) !=
	    atomic_load_explicit(&s->ring.tail, memory_order_relaxed))
		return true;
	if (atomic_load_explicit(&s->jobs.head, memory_order_acquire) !=
	    atomic_load_explicit(&s->jobs.tail, memory_order_relaxed))
		return true;
	/* IDLE needs no clock wake per tick: its wake sources are the edge,
	 * a pushed record, a job and its own deadlines, and a clock term here
	 * would spin the thread at tick rate. */
	if (slic_mode_get(s) == SLIC_MODE_IDLE)
		return s->slic_irq_edge;

	return udsp_clock_tick(&s->clock) != s->last_periodic_tick;
}

void *udsp_slic_loop(void *arg)
{
	struct udsp *s = arg;

	/* A FIFO thread that bypasses rt_apply would otherwise take its stack
	 * faults on the first pass under MCL_ONFAULT, as net_loop notes. */
	if (s->slic_prio > 0)
		rt_stack_prefault();

	for (;;) {
		struct pollfd pfd[3] = {
			{ .fd = s->slic_cmd_fd, .events = POLLIN },
			{ .fd = s->slic_tick_fd, .events = POLLIN },
		};
		struct timespec rel;

		slic_records_run(s);
		slic_job_step(s);
		slic_recover_step(s);
		slic_hook_trace_drain(s);
		slic_periodic(s);
		slic_lec_trace_drain(s);
		if (atomic_load_explicit(&s->slic_quit, memory_order_acquire))
			break;

		slic_irq_fd_refresh(s);
		pfd[2].fd = s->slic_irq_fd;
		pfd[2].events = s->slic_irq_events;

		atomic_store_explicit(&s->slic_armed, true, memory_order_release);
		atomic_thread_fence(memory_order_seq_cst);
		/* Re-check after arming, so the final tick of a bounded run is
		 * never lost between the last poll and the arm (and see
		 * udsp_slic_tick_wake for the fence). */
		if (slic_pending(s)) {
			atomic_store_explicit(&s->slic_armed, false,
					      memory_order_release);
			continue;
		}
		rel = slic_wait_get(s);
		if (ppoll(pfd, s->slic_irq_fd >= 0 ? 3 : 2, &rel, NULL) > 0) {
			slic_fd_drain(s->slic_cmd_fd);
			slic_fd_drain(s->slic_tick_fd);
			if (pfd[2].revents & (POLLNVAL | POLLHUP))
				slic_irq_lost(s);
			else if (pfd[2].revents & (s->slic_irq_events | POLLERR))
				slic_irq_ack(s);
		}
		atomic_store_explicit(&s->slic_armed, false, memory_order_release);
	}

	slic_records_run(s);
	if (s->slic && s->ops->detach) {
		slic_slow_enter(s);
		s->ops->detach(s->slic);
	}
	s->slic = NULL;

	return NULL;
}
