// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Helper process start. See spawn_run.h for the contract.
 */
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stddef.h>
#include <unistd.h>

#include "spawn_run.h"

extern char **environ;

#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 34)
#define SPAWN_CLOSE_INHERITED	1
#endif
#endif

/*
 * On glibc the child closes the inherited descriptors before the exec. glibc
 * resumes this caller at the vfork wake, which the kernel signals in
 * exec_mmap() before do_close_on_exec() runs. The CLOEXEC descriptors of the
 * daemon therefore outlive the return by a few microseconds, and a bind that
 * follows can still meet the old listener. musl needs no arm: its caller
 * blocks on a CLOEXEC pipe whose EOF arrives inside do_close_on_exec(), and
 * its spawn has no addclosefrom.
 *
 * The close from 3 upward is safe for every helper this daemon runs. They read
 * no inherited descriptor and speak over 0, 1 and 2 alone. Every descriptor
 * the daemon opens is CLOEXEC, so the exec would close the same set a moment
 * later.
 *
 * The child's stdout goes to /dev/null. procd captures the daemon's stdio and
 * logs a helper's chatter under the daemon's own name at info; the firewall
 * helper prints "<rule> not found" for every delete of a rule that is not
 * there. stderr stays the daemon's, for a helper's real complaint.
 */
static int spawn_actions_init(posix_spawn_file_actions_t *fa)
{
	int err = posix_spawn_file_actions_init(fa);

	if (!err)
		err = posix_spawn_file_actions_addopen(fa, STDOUT_FILENO,
						       "/dev/null", O_WRONLY, 0);
#ifdef SPAWN_CLOSE_INHERITED
	if (!err)
		err = posix_spawn_file_actions_addclosefrom_np(fa, 3);
#endif

	return err;
}

pid_t spawn_run(char *const argv[], enum spawn_group group)
{
	posix_spawn_file_actions_t fa;
	posix_spawnattr_t attr;
	/* POSIX leaves *pid untouched on a failure, so the caller must not read
	 * one: a garbage pid registered with uloop_process_add() is a reap that
	 * never comes. */
	pid_t pid = -1;
	int err;

	err = posix_spawnattr_init(&attr);
	if (err) {
		errno = err;
		return -1;
	}

	err = spawn_actions_init(&fa);
	if (err) {
		posix_spawnattr_destroy(&attr);
		errno = err;
		return -1;
	}

	/* A pgroup of 0 is the CHILD's own pid, and posix_spawn takes it before
	 * the exec, so nothing on this side has to beat the child to it. */
	if (group == SPAWN_GROUP_OWN) {
		err = posix_spawnattr_setpgroup(&attr, 0);
		if (!err)
			err = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
	}

	if (!err)
		err = posix_spawn(&pid, argv[0], &fa, &attr, argv, environ);

	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&attr);

	if (err) {
		errno = err;
		return -1;
	}

	return pid;
}
