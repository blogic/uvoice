// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include "thread.h"

#define THREAD_SIGSTACK_BYTES	(64 * 1024)

static void thread_sigmask_block(sigset_t *prev)
{
	sigset_t block;

	sigemptyset(&block);
	sigaddset(&block, SIGINT);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGHUP);
	sigaddset(&block, SIGQUIT);
	sigaddset(&block, SIGUSR1);
	sigaddset(&block, SIGUSR2);
	sigaddset(&block, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &block, prev);
}

int udsp_thread_sigstack_install(void **out, stack_t *prev)
{
	stack_t ss, old;
	void *mem;

	if (!out)
		return -EINVAL;

	mem = calloc(1, THREAD_SIGSTACK_BYTES);
	if (!mem)
		return -ENOMEM;

	memset(&ss, 0, sizeof(ss));
	memset(&old, 0, sizeof(old));
	ss.ss_sp = mem;
	ss.ss_size = THREAD_SIGSTACK_BYTES;
	if (sigaltstack(&ss, &old) < 0) {
		int rc = -errno;

		free(mem);
		return rc;
	}

	if (prev)
		*prev = old;
	*out = mem;

	return 0;
}

void udsp_thread_sigstack_release(void **slot, const stack_t *prev)
{
	stack_t ss;

	if (!slot || !*slot)
		return;

	/* Put back whatever the caller had; disabling unconditionally would
	 * take away an application's own alternate stack. */
	if (prev && !(prev->ss_flags & SS_DISABLE) && prev->ss_sp &&
	    prev->ss_size) {
		ss = *prev;
	} else {
		memset(&ss, 0, sizeof(ss));
		ss.ss_flags = SS_DISABLE;
	}
	sigaltstack(&ss, NULL);
	free(*slot);
	*slot = NULL;
}

void udsp_thread_done(struct udsp_thread *t)
{
	uint64_t one = 1;

	if (!t || t->done_fd < 0)
		return;
	if (write(t->done_fd, &one, sizeof(one)) != sizeof(one))
		return;
}

/* The thread pins itself: musl has no pthread_attr_setaffinity_np, and
 * sched_setaffinity(0, ...) is the portable form rt.c already uses. */
static void thread_affinity_apply(int cpu, int not_cpu)
{
	cpu_set_t set;
	long online;

	if (cpu < 0 && not_cpu < 0)
		return;
	if (cpu >= CPU_SETSIZE || not_cpu >= CPU_SETSIZE)
		return;

	CPU_ZERO(&set);
	if (cpu >= 0) {
		CPU_SET(cpu, &set);
		sched_setaffinity(0, sizeof(set), &set);
		return;
	}

	online = sysconf(_SC_NPROCESSORS_ONLN);
	if (online < 2)
		return;
	for (long i = 0; i < online && i < CPU_SETSIZE; i++) {
		if (i != not_cpu)
			CPU_SET((int)i, &set);
	}
	if (CPU_COUNT(&set))
		sched_setaffinity(0, sizeof(set), &set);
}

static void *thread_trampoline(void *arg)
{
	struct udsp_thread *t = arg;
	void *ret;

	if (udsp_thread_sigstack_install(&t->sig_stack, &t->sig_prev) < 0)
		t->sig_stack = NULL;
	thread_affinity_apply(t->cpu, t->not_cpu);

	ret = t->fn(t->arg);

	/* The signal stack belongs to this thread: only this thread may disable
	 * it, so the release cannot wait for the stopper. */
	udsp_thread_sigstack_release(&t->sig_stack, &t->sig_prev);
	udsp_thread_done(t);

	return ret;
}

int udsp_thread_start(struct udsp_thread *t, const char *name,
		      void *(*fn)(void *), void *arg, size_t stack, int cpu,
		      int not_cpu, int prio)
{
	pthread_attr_t attr;
	struct sched_param got_param;
	sigset_t prev;
	int got_policy;
	int rc;

	if (!t || !fn)
		return -EINVAL;

	memset(t, 0, sizeof(*t));
	t->fn = fn;
	t->arg = arg;
	t->cpu = cpu;
	t->not_cpu = not_cpu;
	t->done_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (t->done_fd < 0)
		return -errno;

	if (pthread_attr_init(&attr)) {
		close(t->done_fd);
		t->done_fd = -1;
		return -ENOMEM;
	}
	if (stack)
		pthread_attr_setstacksize(&attr, stack);
	if (prio > 0) {
		struct sched_param param = { .sched_priority = prio };

		/* Best effort, exactly as rt.c treats it: RT_GROUP_SCHED
		 * without a cpu.rt_runtime_us grant refuses the request and
		 * the thread must still run. */
		if (!pthread_attr_setschedpolicy(&attr, SCHED_FIFO) &&
		    !pthread_attr_setschedparam(&attr, &param))
			pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	}

	thread_sigmask_block(&prev);
	rc = pthread_create(&t->id, &attr, thread_trampoline, t);
	if (rc && prio > 0) {
		pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
		rc = pthread_create(&t->id, &attr, thread_trampoline, t);
	}
	pthread_sigmask(SIG_SETMASK, &prev, NULL);
	pthread_attr_destroy(&attr);
	if (rc) {
		close(t->done_fd);
		t->done_fd = -1;
		return -rc;
	}

	t->live = true;
	if (name)
		pthread_setname_np(t->id, name);

	/* The inherit fallback above returns 0 like the explicit create, so
	 * the outcome of a request is only known by reading it back. */
	t->policy = -1;
	if (pthread_getschedparam(t->id, &got_policy, &got_param) == 0)
		t->policy = got_policy;

	return 0;
}

/* ppoll takes a relative timeout against CLOCK_MONOTONIC, which is why the
 * deadline is recomputed here instead of handed to pthread_timedjoin_np: both
 * libcs measure that one against CLOCK_REALTIME and musl has no
 * pthread_clockjoin_np at all. */
int udsp_thread_stop_bounded(struct udsp_thread *t, unsigned deadline_ms)
{
	uint64_t end;

	if (!t || !t->live)
		return 0;

	end = 0;
	for (;;) {
		struct pollfd pfd = { .fd = t->done_fd, .events = POLLIN };
		struct timespec rel;
		uint64_t now = 0;
		int rc;

		{
			struct timespec ts;

			clock_gettime(CLOCK_MONOTONIC, &ts);
			now = (uint64_t)ts.tv_sec * 1000000u +
			      (uint64_t)ts.tv_nsec / 1000u;
		}
		if (!end)
			end = now + (uint64_t)deadline_ms * 1000u;
		if (now >= end)
			return -ETIMEDOUT;

		rel.tv_sec = (time_t)((end - now) / 1000000u);
		rel.tv_nsec = (long)((end - now) % 1000000u) * 1000L;
		rc = ppoll(&pfd, 1, &rel, NULL);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (!rc)
			return -ETIMEDOUT;
		break;
	}

	pthread_join(t->id, NULL);
	t->live = false;
	close(t->done_fd);
	t->done_fd = -1;

	return 0;
}
