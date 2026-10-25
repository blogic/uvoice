// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The event relay a backend puts in front of the core's event fd. A backend
 * that has one or more producer threads gives the producers the relay's lib_fd
 * and lets the uloop side forward every record to the core, under the two
 * record classes of hal.h section 2: a hardware event is dropped and counted
 * on a full fd, a job completion is queued and never lost.
 *
 * The relay deals only in struct hal_event, the core's own wire record. It
 * knows nothing of any backend engine type. A backend whose engine writes a
 * record of an identical layout (the udsp backend, whose udsp_event is pinned
 * to hal_event) hands that engine the lib_fd directly; any other backend writes
 * hal_event records into it.
 *
 * Threads. Every entry point here runs on the uloop thread, like every hal op.
 * The producers only ever write bytes into lib_fd; they call nothing here.
 */
#ifndef UVOICE_HAL_RELAY_H
#define UVOICE_HAL_RELAY_H

#include <stdbool.h>
#include <stdint.h>

#include <libubox/uloop.h>

#include "hal.h"

/*
 * The completion queue depth. The job model cannot fill it: the core carries a
 * bounded set of pending jobs and each completes before its successor starts,
 * so an overflow is a defect, reported and dropped (hal.h section 2 (b)).
 */
#define HAL_RELAY_COMP_MAX	16

/*
 * The token space of a completion a backend posts itself, for a SLOW op with no
 * engine job behind it. libudsp and the css engine count their own job tokens
 * up from 1, so the reserved high half never meets them. The counter runs in
 * the low 31 bits and wraps there, so the reserved bit stays set however many
 * tokens a long-lived daemon emits, and the result is never 0 ("no token").
 */
#define HAL_RELAY_STATS_TOKEN_BASE	0x80000000u
#define HAL_RELAY_STATS_TOKEN_MASK	0x7fffffffu

/* How long a queue with no write watcher waits before it tries again, short
 * against the core's 30 s job backstop and only ever armed when uloop_fd_add
 * failed. */
#define HAL_RELAY_COMP_RETRY_MS		50

/*
 * A record pulled off lib_fd, before it is forwarded. The backend returns true
 * when the record belonged to it and must not reach the core (the bring-up
 * chain, which turns several engine completions into one the core waits for).
 * A false return forwards the record under its class.
 */
typedef bool (*hal_relay_intercept_fn)(void *ctx, const struct hal_event *ev);

struct hal_relay {
	/* The core's write end. The core owns it and closes it; the relay only
	 * ever writes it and watches it for room. */
	int core_fd;
	/* The producer write end handed to the engine, and the read end the
	 * relay drains on the uloop thread. */
	int lib_fd;
	struct uloop_fd read_fd;
	/* The same core fd, watched for writability while comp_len is not 0. */
	struct uloop_fd core_out;
	/* Armed only while that watcher could not be registered. */
	struct uloop_timeout comp_retry;

	/* Completions the core fd could not take, oldest at comp_head. */
	struct hal_event comp[HAL_RELAY_COMP_MAX];
	unsigned int comp_head;
	unsigned int comp_len;

	/* Class (a) records dropped on a full fd, and class (b) completions the
	 * queue could not hold. The core reads them apart (events_dropped vs
	 * completions_lost). */
	uint64_t dropped;
	uint64_t comp_lost;

	/* Runs inside HAL_RELAY_STATS_TOKEN_BASE. */
	uint32_t stats_token;

	hal_relay_intercept_fn intercept;
	void *ctx;
	/* The backend name, for the log lines. */
	const char *tag;
};

/**
 * hal_relay_open() - make the backend's transport and register the read side.
 * @r: relay, zeroed by the caller.
 * @core_fd: the core's write end, from hal_ops.open.
 * @tag: the backend name, used in log lines; must outlive the relay.
 * @intercept: called for each record before it is forwarded, or NULL.
 * @ctx: passed to @intercept.
 *
 * On success @r->lib_fd is the producer end the backend hands to its engine.
 *
 * Return: 0 on success, -1 with errno set on failure (nothing is registered).
 */
int hal_relay_open(struct hal_relay *r, int core_fd, const char *tag,
		   hal_relay_intercept_fn intercept, void *ctx);

/**
 * hal_relay_close() - drop the read watcher, the write watcher and the fds.
 * @r: relay.
 *
 * The core fd is not closed; the core owns it.
 */
void hal_relay_close(struct hal_relay *r);

/**
 * hal_relay_completion_send() - forward a class (b) record, queueing on a full
 * fd.
 * @r: relay.
 * @ev: the completion record (seq 0).
 */
void hal_relay_completion_send(struct hal_relay *r, const struct hal_event *ev);

/**
 * hal_relay_completion_post() - post a HAL_EVENT_JOB_DONE the backend owns.
 * @r: relay.
 * @token: the token the core is waiting on.
 * @st: the job outcome.
 */
void hal_relay_completion_post(struct hal_relay *r, uint32_t token,
			       enum hal_status st);

/**
 * hal_relay_stats_token_next() - a fresh token in the reserved high half.
 * @r: relay.
 *
 * Return: a token that never collides with an engine job token and is never 0.
 */
uint32_t hal_relay_stats_token_next(struct hal_relay *r);

#endif
