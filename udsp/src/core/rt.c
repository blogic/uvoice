// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Real-time setup for the engine tick thread. Three independent steps, all
 * best effort and all reported:
 *
 *   mlockall + stack prefault  keeps the tick out of the page-fault path,
 *   SCHED_FIFO                 the priority the config asks for,
 *   affinity                   the isolated engine CPU.
 *
 * None of them is fatal. A kernel with RT_GROUP_SCHED and without
 * RT_GROUP_SCHED_DEFAULT_DISABLED gives a uvoiced in a procd cgroup EPERM
 * from the scheduler call until cpu.rt_runtime_us is granted.
 * That case has to be measured, not failed on, so the engine keeps its
 * SCHED_OTHER policy and the errno reaches the counters snapshot.
 *
 * Every call here runs once, on the tick thread, before the first tick.
 *
 * Who locks the process. mlockall above is per process, not per thread, and
 * it runs only when the config asks for real time, so this path is what
 * udsp-cli gets. The uvoice udsp backend calls mlockall itself, so uvoiced is
 * locked whatever engine_prio says.
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>

#include "engine.h"

/* Some target headers gate MCL_ONFAULT behind a feature macro; define it if the
 * build did not surface it. An old kernel rejects the flag and the caller falls
 * back to the eager lock. */
#ifndef MCL_ONFAULT
#define MCL_ONFAULT 4
#endif

#define RT_PREFAULT_BYTES 65536
/* The smallest page any supported target uses; a larger page only makes some
 * of these writes redundant. */
#define RT_PREFAULT_STRIDE 4096

/* Every store is through a volatile lvalue, so the compiler must emit it: a
 * plain local array with a memset is dead and GCC removes it at -Os, which
 * leaves the stack unfaulted and the first tick taking the page faults this
 * exists to avoid. */
void rt_stack_prefault(void)
{
	unsigned char page[RT_PREFAULT_BYTES];
	volatile unsigned char *touch = page;

	for (size_t i = 0; i < RT_PREFAULT_BYTES; i += RT_PREFAULT_STRIDE)
		touch[i] = 0;

	touch[RT_PREFAULT_BYTES - 1] = 0;
}

/* Fault in a heap buffer, one store per page, so a real-time thread that later
 * writes it does not take the fault. Under MCL_ONFAULT the page is locked as it
 * arrives. The read-back store keeps the current bytes, so an in-use buffer is
 * unharmed; the volatile lvalue stops -Os from dropping the loop. */
void rt_range_prefault(volatile void *base, size_t len)
{
	volatile unsigned char *touch = base;

	for (size_t i = 0; i < len; i += RT_PREFAULT_STRIDE)
		touch[i] = touch[i];

	if (len)
		touch[len - 1] = touch[len - 1];
}

static void rt_mlock_apply(struct rt_report *rep)
{
	/*
	 * MCL_ONFAULT locks each page as it faults in, not all at once, so cold
	 * library text and untouched .bss stay out of RSS. The tick's own
	 * working set is made resident ahead of the first tick by rt_stack_prefault
	 * below and by the stream-buffer prefault in stream_activate, so the tick
	 * still never faults. A kernel without the flag rejects it, so fall back
	 * to the eager lock, which also makes every page resident.
	 */
	if (mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT) == 0 ||
	    mlockall(MCL_CURRENT | MCL_FUTURE) == 0) {
		rep->mlock_applied = true;
		return;
	}

	rep->mlock_errno = errno;
}

/* Not sched_setscheduler(): musl ships it as a deliberate ENOSYS stub, so a
 * musl target reports rt_errno 38 and stays SCHED_OTHER while a glibc host
 * looks fine. pthread_setschedparam reaches the kernel
 * on both libcs. It returns the error number and leaves errno alone. */
static void rt_sched_apply(struct rt_report *rep, int prio)
{
	struct sched_param param = { .sched_priority = prio };
	int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

	if (rc == 0) {
		rep->sched_applied = true;
		return;
	}

	rep->sched_errno = rc;
}

/* sched_getscheduler() is an ENOSYS stub on musl for the same reason, so the
 * effective policy is read back through the pthread call as well. */
static int rt_policy_get(void)
{
	struct sched_param param;
	int policy = 0;

	if (pthread_getschedparam(pthread_self(), &policy, &param))
		return -1;

	return policy;
}

static void rt_affinity_apply(struct rt_report *rep, int cpu)
{
	cpu_set_t set;

	if (cpu >= CPU_SETSIZE) {
		rep->affinity_errno = EINVAL;
		return;
	}

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set) == 0) {
		rep->affinity_applied = true;
		rep->cpu = cpu;
		return;
	}

	rep->affinity_errno = errno;
}

void rt_apply(int cpu, int prio, struct rt_report *rep)
{
	memset(rep, 0, sizeof(*rep));
	rep->prio = prio;
	rep->cpu = -1;

	/* MCL_FUTURE first, so the pages the prefault touches are locked as
	 * they arrive. */
	if (prio > 0) {
		rt_mlock_apply(rep);
		rt_stack_prefault();
		rt_sched_apply(rep, prio);
	}

	if (cpu >= 0)
		rt_affinity_apply(rep, cpu);

	rep->policy = rt_policy_get();
}
