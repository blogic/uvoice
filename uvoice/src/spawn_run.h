// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The one place uvoiced starts a helper process.
 *
 * No child holds a descriptor of ours once the call returns, and the two C
 * libraries reach that differently. fork() hands the child a copy of the whole
 * descriptor table and the copy lives until the exec. That window is long
 * enough to lose a SIP listener: account.c releases a socket and binds a new
 * one on the same port in one frame, and Linux refuses the bind while a second
 * holder of the old socket sits in TCP_LISTEN, whatever SO_REUSEADDR says on
 * either side. Both libraries build posix_spawn(3) on
 * clone(CLONE_VM | CLONE_VFORK), but the wake is not the same moment:
 *
 *   musl blocks this caller on a CLOEXEC pipe until the child's execve fails
 *   or succeeds, and that pipe reaches its EOF inside do_close_on_exec(), so
 *   every CLOEXEC descriptor is already gone when the call returns
 *   (src/process/posix_spawn.c).
 *
 *   glibc wakes this caller at the vfork wake, which the kernel signals in
 *   exec_mmap(), before do_close_on_exec() runs (fs/exec.c). The daemon's
 *   descriptors therefore survive the return by a few microseconds, which
 *   costs about one spawn in 1300 its rebind, so the spawn closes them in
 *   the child itself through its file actions (spawn_run.c).
 *
 * The exec would drop the same set a moment later, because every descriptor
 * this daemon opens is CLOEXEC. What the file actions buy is the order.
 *
 * An exec failure comes back here, not at the reap. fork and execv report a
 * missing binary or a dead interpreter line as a child that exits 127, which
 * the caller reads when it reaps. posix_spawn reports the same failure as the
 * return of the spawn, with errno, and reaps the child itself. A caller that
 * gets -1 gets no completion callback at all.
 */
#ifndef UVOICE_SPAWN_RUN_H
#define UVOICE_SPAWN_RUN_H

#include <sys/types.h>

/*
 * Where the child's process group comes from. SPAWN_GROUP_DAEMON leaves it in
 * the daemon's group, which is what a helper that ends by itself wants.
 * SPAWN_GROUP_OWN gives the child a group of its own whose id is the child pid,
 * so kill(-pid) reaches the child and everything it started. The TLS
 * certificate helper needs that: a stop has to take its curl down too
 * (event.h).
 */
enum spawn_group {
	SPAWN_GROUP_DAEMON = 0,
	SPAWN_GROUP_OWN,
};

/*
 * Start argv[0] with argv and the daemon's environment. The child's stdout is
 * /dev/null; its stderr is the daemon's, which procd captures at error level.
 * Returns the child pid, or -1 with errno set: ENOENT for a missing binary or
 * a dead interpreter line, EACCES for a file that is not executable, EAGAIN
 * when the process limit refuses the child. The caller reaps a live child
 * through uloop_process; a -1 leaves nothing to reap.
 */
pid_t spawn_run(char *const argv[], enum spawn_group group);

#endif
