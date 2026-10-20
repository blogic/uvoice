// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include "clock.h"
#include "cmd.h"

/* A claim is three stores wide, so a hundred microseconds of spin covers a
 * preempted completer without turning the wait into a busy loop. */
#define CMD_CLAIM_SPINS		1000u

static pthread_once_t cmd_sync_tick_once = PTHREAD_ONCE_INIT;
static unsigned cmd_sync_tick_ms = UDSP_SYNC_TICK_MS;

/* A zero or malformed value keeps the production deadline, so the override
 * never disables the ack wait. */
static void cmd_sync_tick_ms_init(void)
{
	const char *v = getenv("UDSP_SYNC_TICK_MS");
	char *end = NULL;
	unsigned long n;

	if (!v || *v < '0' || *v > '9')
		return;
	errno = 0;
	n = strtoul(v, &end, 10);
	if (errno || end == v || (end && *end) || n == 0 || n > UINT_MAX)
		return;

	cmd_sync_tick_ms = (unsigned)n;
}

unsigned cmd_sync_tick_ms_get(void)
{
	pthread_once(&cmd_sync_tick_once, cmd_sync_tick_ms_init);

	return cmd_sync_tick_ms;
}

int cmd_ack_init(struct cmd_ack *a)
{
	memset(a, 0, sizeof(*a));
	atomic_init(&a->token, CMD_TOKEN_FREE);
	atomic_init(&a->status, 0);
	a->efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

	return a->efd < 0 ? -errno : 0;
}

void cmd_ack_release(struct cmd_ack *a)
{
	if (!a || a->efd < 0)
		return;

	close(a->efd);
	a->efd = -1;
}

static void cmd_ack_drain(const struct cmd_ack *a)
{
	uint64_t sink;

	while (read(a->efd, &sink, sizeof(sink)) == (ssize_t)sizeof(sink))
		continue;
}

void cmd_ack_arm(struct cmd_ack *a, uint32_t tok)
{
	cmd_ack_drain(a);
	atomic_store_explicit(&a->status, 0, memory_order_relaxed);
	atomic_store_explicit(&a->token, tok, memory_order_release);
}

bool cmd_ack_post(struct cmd_ack *a, uint32_t tok, int32_t status)
{
	uint_least32_t expect = tok;
	uint64_t one = 1;

	if (!a || a->efd < 0 || tok == CMD_TOKEN_FREE)
		return false;
	/* Claim first, so a completion for a waiter that already left is
	 * discarded and no second completer can interleave. */
	if (!atomic_compare_exchange_strong_explicit(&a->token, &expect,
						     CMD_TOKEN_BUSY,
						     memory_order_acq_rel,
						     memory_order_relaxed))
		return false;

	/* The status is written before DONE is published: a release store of
	 * DONE cannot carry a store that follows it, and a waiter that woke on
	 * DONE would then read the previous command's status. */
	atomic_store_explicit(&a->status, status, memory_order_relaxed);
	atomic_store_explicit(&a->token, CMD_TOKEN_DONE, memory_order_release);
	if (write(a->efd, &one, sizeof(one)) != (ssize_t)sizeof(one))
		return true;

	return true;
}

/* True once the slot holds a published completion; BUSY means a completer is
 * between its claim and its publish, so the caller keeps waiting. */
static bool cmd_ack_take(struct cmd_ack *a, int32_t *status)
{
	if (atomic_load_explicit(&a->token, memory_order_acquire) !=
	    CMD_TOKEN_DONE)
		return false;

	if (status)
		*status = (int32_t)atomic_load_explicit(&a->status,
						       memory_order_relaxed);
	cmd_ack_drain(a);

	return true;
}

/* The shared tail of cmd_ack_wait and slic_ack_wait once the deadline passed.
 * One acquire load per turn decides the state, so a DONE the completer publishes
 * mid-turn is taken, not misread as a timeout. */
int cmd_ack_finish(struct cmd_ack *a, uint32_t tok, int32_t *status)
{
	uint_least32_t expect = tok;

	/* The command is not withdrawn from the ring: every executor compares
	 * the token the record carries with the token the slot holds and skips
	 * the work when they differ. A cancel that races an execution already
	 * in flight still performs the action; its ack then loses the claim
	 * and is discarded. */
	if (atomic_compare_exchange_strong_explicit(&a->token, &expect,
						    CMD_TOKEN_CANCELLED,
						    memory_order_acq_rel,
						    memory_order_relaxed))
		return -ETIMEDOUT;

	/* The exchange lost to a completer that had already claimed the slot,
	 * so the completion is ours; wait the short window it needs to publish
	 * rather than report a timeout for work that succeeded. */
	for (unsigned i = 0; i < CMD_CLAIM_SPINS; i++) {
		struct timespec nap = { .tv_sec = 0, .tv_nsec = 100000L };
		uint_least32_t t = atomic_load_explicit(&a->token,
							memory_order_acquire);

		if (t == CMD_TOKEN_DONE) {
			if (status)
				*status = (int32_t)atomic_load_explicit(
					&a->status, memory_order_relaxed);
			cmd_ack_drain(a);
			return 0;
		}
		if (t != CMD_TOKEN_BUSY)
			break;
		nanosleep(&nap, NULL);
	}

	return -ETIMEDOUT;
}

int cmd_ack_wait(struct cmd_ack *a, uint32_t tok, unsigned timeout_ms,
		 int32_t *status)
{
	uint64_t end = udsp_mono_us() + (uint64_t)timeout_ms * 1000u;

	for (;;) {
		struct pollfd pfd = { .fd = a->efd, .events = POLLIN };
		struct timespec rel;
		uint64_t now = udsp_mono_us();
		int rc;

		if (cmd_ack_take(a, status))
			return 0;
		if (now >= end)
			break;

		rel.tv_sec = (time_t)((end - now) / 1000000u);
		rel.tv_nsec = (long)((end - now) % 1000000u) * 1000L;
		rc = ppoll(&pfd, 1, &rel, NULL);
		if (rc < 0 && errno != EINTR)
			break;
		if (!rc)
			break;
		cmd_ack_drain(a);
	}

	return cmd_ack_finish(a, tok, status);
}

enum udsp_status cmd_post_sync(struct cmd_ring *r, struct cmd_ack *ack,
			       uint32_t token, struct udsp_cmd *cmd,
			       unsigned timeout_ms, int32_t *status)
{
	int32_t got = 0;
	int rc;

	cmd->token = token;
	cmd->ack = ack;
	cmd_ack_arm(ack, token);
	if (!cmd_ring_push(r, cmd))
		return UDSP_TRANSIENT;

	rc = cmd_ack_wait(ack, token, timeout_ms, &got);
	if (status)
		*status = got;
	if (rc)
		return UDSP_TRANSIENT;

	return cmd_status_map(got);
}

enum udsp_status cmd_status_map(int32_t err)
{
	if (!err)
		return UDSP_OK;
	if (err == -EINVAL || err == -ENOTSUP || err == -ENOSYS)
		return UDSP_UNSUPPORTED;
	/* The tick refused the command because the line died after the guard
	 * admitted it (chan.c chan_cmd_run); the answer the guard gives for a
	 * dead line is fatal, and a transient here would invite a retry the
	 * line can never satisfy. */
	if (err == -ENODEV)
		return UDSP_FATAL;

	return UDSP_TRANSIENT;
}

enum udsp_status cmd_job_status_map(int32_t err)
{
	switch (err) {
	case 0:
		return UDSP_OK;
	case -ETIMEDOUT:
	case -EAGAIN:
	case -EBUSY:
		return UDSP_TRANSIENT;
	/* A start a stop superseded before it ran. Nothing
	 * failed and the caller may start again, so TRANSIENT is the truthful
	 * signal; the FATAL arm below would strand a line that is healthy. */
	case -ECANCELED:
		return UDSP_TRANSIENT;
	case -EINVAL:
	case -ENOTSUP:
	/* A plug-in answers -ENOSYS for a part it does not cover, which is
	 * an unsupported request and not a broken line. */
	case -ENOSYS:
	case -ENOENT:
		return UDSP_UNSUPPORTED;
	default:
		break;
	}

	/* A job that failed for any other reason failed on the hardware: the
	 * SLIC did not initialise, the bus did not answer, the engine did not
	 * stop. The consumer must not retry those (hal.h class b). */
	return UDSP_FATAL;
}
