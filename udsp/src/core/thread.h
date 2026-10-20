// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The libudsp thread trampoline. Every thread but the engine tick starts
 * here: a fixed stack, a blocked signal set, its own 64 KiB signal stack
 * (sigaltstack is per thread and the crash handler runs with SA_ONSTACK) and
 * a done eventfd it writes as its last act, so a stopper can wait on a
 * relative monotonic deadline instead of pthread_timedjoin_np, which is
 * CLOCK_REALTIME on both libcs.
 */
#ifndef UDSP_CORE_THREAD_H
#define UDSP_CORE_THREAD_H

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>

struct udsp_thread {
	pthread_t id;
	int done_fd;
	bool live;		/* the stopper's view, control side only */
	void *(*fn)(void *);
	void *arg;
	void *sig_stack;
	stack_t sig_prev;
	int cpu;
	int not_cpu;
	int policy;		/* the policy the thread got, read back after the
				 * create; -1 when the read failed. Control side. */
};

/* cpu >= 0 pins to that cpu; otherwise not_cpu >= 0 runs on every cpu except
 * that one (the slic thread is kept off the engine cpu because spidev on
 * AN7581 drives spi-gpio, which bit-bangs the bus in the calling thread).
 * prio > 0 requests SCHED_FIFO, best effort;
 * t->policy says what the thread got. Returns 0 or a negative errno. */
int udsp_thread_start(struct udsp_thread *t, const char *name,
		      void *(*fn)(void *), void *arg, size_t stack, int cpu,
		      int not_cpu, int prio);
/* 0 when the thread finished and was joined, -ETIMEDOUT when the deadline
 * expired; on -ETIMEDOUT the caller must free nothing the thread can touch. */
int udsp_thread_stop_bounded(struct udsp_thread *t, unsigned deadline_ms);
/* The trampoline's last act, also callable by a thread that leaves early. */
void udsp_thread_done(struct udsp_thread *t);
/*
 * Installs a 64 KiB signal stack for the calling thread, so the crash handler
 * of core/safe_state.c can still run when the fault is the stack. prev takes
 * whatever stack the thread had, and the release puts it back rather than
 * disabling the caller's own. 0 or a negative errno; the caller decides
 * whether a failure matters.
 */
int udsp_thread_sigstack_install(void **out, stack_t *prev);
void udsp_thread_sigstack_release(void **slot, const stack_t *prev);

#endif
