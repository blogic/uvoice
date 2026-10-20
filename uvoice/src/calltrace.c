// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The call trace controller. calltrace.h carries the contract; this file holds
 * the state machine, the preflight, the capture, the snapshots, the freeze,
 * the manifest journal and the recovery.
 *
 * One trace at a time. The arm opens the rings, installs the SIP tap, sets
 * the ring mask over every domain, opens the pcapng export and starts the
 * wire capture, so an INVITE that arrives before the first hook edge is
 * already in the archive. The window itself runs from the first move of the
 * line out of its idle rest until the line rests again plus the grace, and ends
 * early at the window timeout, at a cancel, or when the line leaves service.
 *
 * A child is owned from the moment it starts to the moment it is reaped. No
 * state change abandons one, nothing that holds a live child is overwritten,
 * and a second child of the same kind never starts while the first is alive:
 * an arm is refused as busy while any child of the trace before it still
 * runs. That is what lets the freeze take the wire capture's exit status into
 * the archive, and what keeps a stop signal from reaching a process that has
 * already gone and been replaced.
 *
 * The work dir is the journal. manifest.json is rewritten at every stage
 * change through a temp file and a rename, so a dir the daemon left behind
 * always says what it was doing. The next start finds such a dir, marks it
 * interrupted and runs the finisher on it.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <libubox/blobmsg_json.h>
#include <libubox/uloop.h>

#include "uvoice.h"
#include "calltrace.h"
#include "cdr.h"
#include "line/line.h"
#include "media_trace.h"
#include "net/iface.h"
#include "net/ports.h"
#include "proto/proto.h"
#include "proto/proto_tap.h"
#include "spawn_run.h"
#include "status.h"
#include "trace_ring.h"

#define CT_ROOT_DEFAULT		"/tmp/uvoice/calltrace"
#define CT_WIRE_SCRIPT		"/usr/share/uvoice/calltrace-wire.sh"
#define CT_FINISH_SCRIPT	"/usr/share/uvoice/calltrace-finish.sh"
#define CT_TCPDUMP		"/usr/bin/tcpdump"
#define CT_TASKSET		"/usr/bin/taskset"
#define CT_BOOT_ID_PATH		"/proc/sys/kernel/random/boot_id"
/* the signaling ports of one line the capture filter takes */
#define CT_WIRE_PORTS_MAX	4
#define CT_STATM_PATH		"/proc/self/statm"
#define CT_SPACE_PATH		"/tmp"

/* The headroom the preflight asks for on top of the caps, so a trace that
 * fills them still leaves /tmp usable. */
#define CT_HEADROOM_MB		16

/* The cadences. Each one reads its environment override once, so a suite
 * shrinks a window without moving the production value. The term per MiB of
 * the finisher budget has no override, because a large one would carry the
 * budget past an int of milliseconds. Test mode sets it instead. */
#define CT_SNAPSHOT_MS		10000
#define CT_EXPORT_MS		5000
#define CT_WIRE_POLL_MS		1000
#define CT_KILL_MS		3000
#define CT_WIRE_READY_MS	250
#define CT_WIRE_READY_TRIES	12
#define CT_EVENT_COALESCE_MS	1000
/*
 * The finisher budget: the base, plus the term per MiB of the work dir that
 * the run packs, MiB rounded up. The base covers the phases that carry a time
 * limit of their own: the collectors, the stop of an orphan capture and the
 * 30 s of the summary. The finisher script gives the summary one more second
 * per MiB of the ring file and the wire capture, both files of the work dir,
 * and the gzip of the slowest board packs about 2 MiB a second. Two seconds
 * per MiB therefore cover that allowance plus a pack time of about half a
 * second per MiB. A capture of 300 MiB, which a board with 400 MB of /tmp
 * can hold, gets a budget of about eleven minutes.
 */
#define CT_FINISH_BUDGET_MS	60000
#define CT_FINISH_MIB_MS	2000
#define CT_EXPORT_BATCH		2048
/* The export cadence while the canceller ring is open. Two lines at 16 kHz
 * write about 200 KiB a second there, so a step at the cadence of the other
 * rings would write about 1 MiB on the uloop thread at once. */
#define CT_EXPORT_LEC_MS	1000

/* A pcap file header is 24 bytes, so a file that reached them is a capture
 * whose interface opened. */
#define CT_PCAP_HDR_BYTES	24
#define CT_MIB			(1024ull * 1024ull)

/* What one read of a small file may take: a procfs line, or a manifest an
 * earlier daemon wrote. */
#define CT_SMALL_READ		4096
#define CT_JSON_MAX		(256 * 1024)

/* The descriptors one walk of a work dir holds at once. On musl the count is
 * also the depth limit of the walk: musl reads a directory only while the
 * count it passes down is not zero. A work dir nests three levels of
 * directories, so eight cover them with room. */
#define CT_WALK_FDS		8

/* The record sequences of one window. It matches the depth of the call
 * record ring, so no leg the ring still holds can fall out of the window's
 * own list. */
#define CT_SEQ_MAX		64
#define CT_REASONS_MAX		32
#define CT_RECOVER_MAX		16
#define CT_SIGNALS_MAX		4
#define CT_WORD_MAX		40
#define CT_ERROR_MAX		96
#define CT_FILTER_MAX		224
#define CT_CPUMASK_MAX		96
#define CT_ARGV_MAX		6
#define CT_ARG_MAX		256

/*
 * The path widths, as a chain: a work root, an id under it, and one file name
 * under that. Each destination is wide enough for the parts it joins, which
 * is what lets the compiler prove that no join truncates.
 */
#define CT_ROOT_MAX		128
#define CT_ID_MAX		64
#define CT_NAME_MAX		32
#define CT_DIR_MAX		(CT_ROOT_MAX + CT_ID_MAX + 2)
#define CT_FILE_MAX		(CT_DIR_MAX + CT_NAME_MAX)

enum ct_state {
	CT_IDLE = 0,
	CT_ARMED,
	CT_RUNNING,
	CT_FINISHING,
	CT_DONE,
	CT_FAILED,
};

/*
 * One helper process. It is owned from the spawn to the reap: running says a
 * process exists, stopping says a stop is already under way and its deadline
 * is set, and the exit status stands once the reap delivered it. Test mode
 * keeps the argument list and the signals, starts nothing, and leaves the
 * completion to the suite.
 */
struct ct_child {
	struct uloop_process proc;
	struct uloop_timeout kill;
	char argv[CT_ARGV_MAX][CT_ARG_MAX];
	unsigned int argc;
	char signals[CT_SIGNALS_MAX + 1];
	unsigned int nsignals;
	int exit;
	bool running;
	bool stopping;
	bool exit_valid;
};

struct ct_wire {
	struct ct_child child;
	char iface[IF_NAMESIZE + 8];
	char filter[CT_FILTER_MAX];
	uint64_t bytes;
	unsigned int restarts;
	unsigned int ready_tries;
	bool enabled;
	bool ready;
	bool truncated;
	bool restart_pending;
	bool failed;
};

/* The work dirs an earlier daemon left behind, finished one at a time. */
struct ct_recover {
	struct ct_child child;
	struct uloop_timeout budget;
	char dir[CT_RECOVER_MAX][CT_ID_MAX];
	unsigned int pid[CT_RECOVER_MAX];
	char cur[CT_DIR_MAX];
	char archive[CT_FILE_MAX];
	unsigned int count;
	unsigned int next;
};

struct ct {
	enum ct_state state;
	struct calltrace_options opt;
	unsigned int line;

	char id[CT_ID_MAX];
	char dir[CT_DIR_MAX];
	char archive[CT_FILE_MAX];
	char cpumask[CT_CPUMASK_MAX];
	char last_error[CT_ERROR_MAX];
	/* What failed the trace. last_error takes the same text at the failure,
	 * and a refused arm then writes its word over last_error alone. */
	char failure[CT_ERROR_MAX];
	char reason[CT_WORD_MAX];
	const char *stage;
	/* The budget the live finisher runs under. The reply names it, so a
	 * caller that waits for the archive waits as long as the daemon
	 * does. */
	int finish_budget_ms;

	/* false while the line must rest before the window may start */
	bool waiting_call;
	/* where the traced line rested after its last move, the "before" of
	 * the next one */
	enum line_rest rest;
	bool have_id;
	bool have_dir;
	bool have_archive;
	bool rings_truncated;
	bool lec_truncated;
	bool export_open;
	bool local_substituted;
	/* a failure already under way, so a write that fails inside it does
	 * not start a second one */
	bool failing;

	uint64_t armed_at;
	uint64_t capture_ready_at;
	uint64_t started_at;
	uint64_t ended_at;
	uint64_t started_mono_ms;
	int64_t clock_offset_ms;
	bool clock_offset_valid;

	unsigned int calls;
	unsigned int snapshots;
	uint32_t seq;
	uint32_t cdr_seq[CT_SEQ_MAX];
	unsigned int cdr_seqs;

	char reasons[CT_REASONS_MAX][CT_WORD_MAX];
	unsigned int reason_count;
	unsigned int reasons_dropped;

	uint64_t file_bytes;

	struct ct_wire wire;
	struct ct_child fin;
	struct uloop_timeout t_arm;
	struct uloop_timeout t_window;
	struct uloop_timeout t_grace;
	struct uloop_timeout t_snap;
	struct uloop_timeout t_export;
	struct uloop_timeout t_wire;
	struct uloop_timeout t_ready;
	struct uloop_timeout t_event;
	struct uloop_timeout t_fin;
};

static struct ct ct;
static struct ct_recover rc;
/* The document under construction, and the scratch its record filter and its
 * manifest reader parse into. Neither is live while the other is. */
static struct blob_buf cb;
static struct blob_buf xb;
static char ct_root[CT_ROOT_MAX] = CT_ROOT_DEFAULT;
static char ct_boot_id[48];
static uint64_t ct_space_test;
static unsigned int ct_finish_mib_test;
static int ct_finish_last_ms;
static long ct_cpus_test;
static int64_t ct_clock_test_ms;
static bool ct_test_mode;
static bool ct_spawn_fail_test;
static unsigned int ct_snapshot_ms;
static unsigned int ct_export_ms;
static unsigned int ct_export_lec_ms;
static unsigned int ct_wire_poll_ms;
static unsigned int ct_kill_ms;
static unsigned int ct_finish_ms;

static void ct_freeze(const char *reason);
static void ct_fail(const char *reason, const char *what, int err);
static void ct_manifest_write(void);
static void ct_snapshot(const char *kind);
static bool ct_wire_start(void);
static void ct_finish_start(void);
static void ct_recover_next(void);
static void ct_teardown(const char *reason);

/* ------------------------------------------------------------------ basics */

static const char *ct_state_name(enum ct_state state)
{
	switch (state) {
	case CT_IDLE:
		return "idle";
	case CT_ARMED:
		return "armed";
	case CT_RUNNING:
		return "running";
	case CT_FINISHING:
		return "finishing";
	case CT_DONE:
		return "done";
	case CT_FAILED:
		return "failed";
	}

	return "idle";
}

/*
 * The one writer of the controller's state, and why it is not an assignment.
 *
 * The backend debug mask carries the plaintext media datagrams of a running
 * trace, and media_trace_sync() builds that mask from calltrace_capturing(),
 * which reads this state. The mask is pushed on a trace domain change alone
 * (media_trace.c media_trace_hook), and the ring mask this file sets moves
 * before the state does at every edge: the arm sets the mask while the state
 * is still idle and the freeze clears it while the trace still runs. A
 * capture that assigned the state would therefore arm the backend on no edge
 * at all and disarm it on none either.
 *
 * The push follows the assignment, so the mask the backend gets is the one
 * this state asks for. Every push is one idempotent op (hal.h debug_set), so
 * a state change that asks for nothing new costs one call.
 */
static void ct_state_set(enum ct_state state)
{
	ct.state = state;
	media_trace_sync();
}

static uint64_t ct_wall_ms(void)
{
	struct timespec ts;
	int64_t ms;

	clock_gettime(CLOCK_REALTIME, &ts);
	ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

	return (uint64_t)(ms + ct_clock_test_ms);
}

static uint64_t ct_wall_s(void)
{
	return ct_wall_ms() / 1000;
}

static uint64_t ct_mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t ct_mono_ms(void)
{
	return ct_mono_ns() / 1000000ull;
}

/* Seconds as the milliseconds a uloop timer takes. Every option is bounded so
 * the product stays inside an int. */
static int ct_timer_ms(unsigned int seconds)
{
	return (int)(seconds * 1000u);
}

/*
 * A cadence and the environment variable that shrinks it. The value is read
 * once, so a case that moves the variable after the first read gets what the
 * daemon already holds, which is what a production daemon gets too.
 */
static unsigned int ct_ms_env(const char *name, unsigned int def)
{
	const char *val = getenv(name);
	unsigned long ms;
	char *end;

	if (!val || !*val)
		return def;

	ms = strtoul(val, &end, 10);
	if (*end || !ms || ms > 3600000)
		return def;

	return (unsigned int)ms;
}

static void ct_cadences_load(void)
{
	if (ct_snapshot_ms)
		return;

	ct_snapshot_ms = ct_ms_env("UVOICE_CALLTRACE_SNAPSHOT_MS",
				   CT_SNAPSHOT_MS);
	ct_export_ms = ct_ms_env("UVOICE_CALLTRACE_EXPORT_MS", CT_EXPORT_MS);
	ct_export_lec_ms = ct_ms_env("UVOICE_CALLTRACE_EXPORT_LEC_MS",
				     CT_EXPORT_LEC_MS);
	ct_wire_poll_ms = ct_ms_env("UVOICE_CALLTRACE_WIRE_POLL_MS",
				    CT_WIRE_POLL_MS);
	ct_kill_ms = ct_ms_env("UVOICE_CALLTRACE_KILL_MS", CT_KILL_MS);
	ct_finish_ms = ct_ms_env("UVOICE_CALLTRACE_FINISH_MS",
				 CT_FINISH_BUDGET_MS);
}

static void ct_word_set(char *dst, const char *src)
{
	snprintf(dst, CT_WORD_MAX, "%s", src ? src : "");
}

/*
 * Join a directory and a file name. The compiler cannot bound a const char *
 * inside a format, so each part carries the precision its destination is
 * sized for and the join is provably whole.
 */
static void ct_join(char out[CT_FILE_MAX], const char *dir, const char *name)
{
	snprintf(out, CT_FILE_MAX, "%.*s/%.*s", CT_DIR_MAX - 1, dir,
		 CT_NAME_MAX - 1, name);
}

static void ct_path(char out[CT_FILE_MAX], const char *name)
{
	ct_join(out, ct.dir, name);
}

/* 0, or -errno. Every file of a work dir is 0600, because the SIP messages
 * and the wire capture hold what any packet capture holds. */
static int ct_fd_write(int fd, const char *data, size_t len)
{
	ssize_t wrote = write(fd, data, len);
	int err = 0;

	if (wrote < 0)
		err = -errno;
	else if ((size_t)wrote != len)
		err = -EIO;

	if (close(fd) < 0 && !err)
		err = -errno;

	return err;
}

static int ct_file_write(const char *path, const char *data, size_t len)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	return fd < 0 ? -errno : ct_fd_write(fd, data, len);
}

static int ct_file_append(const char *path, const char *data, size_t len)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);

	return fd < 0 ? -errno : ct_fd_write(fd, data, len);
}

static uint64_t ct_file_size(const char *path)
{
	struct stat st;

	if (stat(path, &st) < 0)
		return 0;

	return (uint64_t)st.st_size;
}

/* Up to max bytes of a file as a heap string, or NULL. The caller names the
 * size its own content needs, so a procfs line costs a page and a manifest
 * costs what a manifest can be. */
static char *ct_file_read(const char *path, size_t max)
{
	char *text;
	ssize_t got;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;

	text = malloc(max + 1);
	if (!text) {
		close(fd);
		return NULL;
	}

	got = read(fd, text, max);
	close(fd);
	if (got <= 0) {
		free(text);
		return NULL;
	}

	text[got] = '\0';

	return text;
}

static void ct_boot_id_load(void)
{
	char *text, *nl;

	if (ct_boot_id[0])
		return;

	text = ct_file_read(CT_BOOT_ID_PATH, CT_SMALL_READ);
	if (!text) {
		snprintf(ct_boot_id, sizeof(ct_boot_id), "unknown");
		return;
	}

	nl = strchr(text, '\n');
	if (nl)
		*nl = '\0';
	snprintf(ct_boot_id, sizeof(ct_boot_id), "%s", text);
	free(text);
}

/*
 * The four hex digits of an id. A trace is named by the boot it ran on and by
 * the daemon that took it, so two traces of the same line in the same second
 * across a restart cannot share a name. FNV-1a over the boot id text and the
 * pid, folded to sixteen bits.
 */
static uint16_t ct_id_hash(void)
{
	uint32_t h = 2166136261u;
	uint32_t pid = (uint32_t)getpid();
	const char *p;
	unsigned int i;

	ct_boot_id_load();
	for (p = ct_boot_id; *p; p++) {
		h ^= (uint8_t)*p;
		h *= 16777619u;
	}
	for (i = 0; i < sizeof(pid); i++) {
		h ^= (uint8_t)((pid >> (i * 8)) & 0xff);
		h *= 16777619u;
	}

	return (uint16_t)(h & 0xffff);
}

static void ct_id_make(unsigned int line)
{
	time_t now = (time_t)ct_wall_s();
	char stamp[24];
	struct tm tm;

	gmtime_r(&now, &tm);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
	snprintf(ct.id, sizeof(ct.id), "%s-line%u-%04x", stamp, line,
		 ct_id_hash());
	ct.have_id = true;
}

/* --------------------------------------------------------------- the dirs */

/* The work root and every directory above it. A root a suite pointed
 * somewhere else is created the same way. */
static int ct_root_make(void)
{
	char work[CT_ROOT_MAX];
	char *slash;

	memcpy(work, ct_root, sizeof(work));
	for (slash = work + 1; *slash; slash++) {
		if (*slash != '/')
			continue;

		*slash = '\0';
		if (mkdir(work, 0700) < 0 && errno != EEXIST)
			return -errno;
		*slash = '/';
	}

	if (mkdir(work, 0700) < 0 && errno != EEXIST)
		return -errno;

	return 0;
}

/*
 * The one walk of a work dir tree. It follows no symbolic link, so a link in
 * a dir reaches nothing outside it. nftw() hands its callback no context, so
 * a callback keeps what it counts in a static; the uloop thread is the one
 * caller.
 */
typedef int (*ct_walk_fn)(const char *path, const struct stat *st, int type,
			  struct FTW *ftw);

static uint64_t ct_walk_bytes;

static void ct_tree_walk(const char *path, ct_walk_fn fn, int flags)
{
	nftw(path, fn, CT_WALK_FDS, FTW_PHYS | flags);
}

static int ct_walk_size(const char *path, const struct stat *st, int type,
			struct FTW *ftw)
{
	if (type == FTW_F)
		ct_walk_bytes += (uint64_t)st->st_size;

	return 0;
}

static uint64_t ct_dir_bytes(const char *path)
{
	ct_walk_bytes = 0;
	ct_tree_walk(path, ct_walk_size, 0);

	return ct_walk_bytes;
}

static int ct_walk_remove(const char *path, const struct stat *st, int type,
			  struct FTW *ftw)
{
	if (type == FTW_DP || type == FTW_DNR)
		rmdir(path);
	else
		unlink(path);

	return 0;
}

/* A dir that a finisher touched holds the collector dirs .finish/, system/
 * and tools/, so the removal walks the whole tree, deepest entries first. A
 * dir the finisher already removed is not an error. */
static void ct_dir_remove(const char *path)
{
	ct_tree_walk(path, ct_walk_remove, FTW_DEPTH);
}

/*
 * One finished archive plus the one in progress. A published archive is the
 * newest answer, so every older one goes: a board with 400 MB of /tmp cannot
 * hold a history of them.
 */
static void ct_retention_sweep(const char *archive)
{
	const char *keep = strrchr(archive, '/');
	struct dirent *ent;
	DIR *dir;

	if (!keep)
		return;
	keep++;

	dir = opendir(ct_root);
	if (!dir)
		return;

	while ((ent = readdir(dir))) {
		size_t len = strlen(ent->d_name);

		if (len < 8 || strcmp(ent->d_name + len - 7, ".tar.gz"))
			continue;
		if (!strcmp(ent->d_name, keep))
			continue;

		unlinkat(dirfd(dir), ent->d_name, 0);
	}
	closedir(dir);
}

/* ------------------------------------------------------------ the manifest */

static const char * const ct_ring_names[__TRACE_RING_MAX] = {
	"log", "sip", "status", "rtp", "lec"
};

/* The signaling ring takes the name of the protocol that runs; under the
 * null table it keeps "sip". */
static const char *ct_ring_name(unsigned int id)
{
	if (id == TRACE_RING_SIP && proto_running())
		return proto_ops_get()->name;

	return ct_ring_names[id];
}

static void ct_ring_rows_dump(struct blob_buf *b)
{
	unsigned int id;

	for (id = 0; id < __TRACE_RING_MAX; id++) {
		struct trace_ring_counts c;
		void *row;

		trace_ring_counts((enum trace_ring_id)id, &c);
		row = blobmsg_open_table(b, ct_ring_name(id));
		blobmsg_add_u64(b, "written", c.written);
		blobmsg_add_u64(b, "exported", c.exported);
		blobmsg_add_u64(b, "overwritten", c.overwritten);
		blobmsg_add_u64(b, "truncated", c.truncated);
		blobmsg_close_table(b, row);
	}
}

static uint64_t ct_export_bytes(void)
{
	return ct.export_open ? trace_ring_export_bytes() : ct.file_bytes;
}

static void ct_options_dump(struct blob_buf *b)
{
	void *tbl = blobmsg_open_table(b, "options");

	blobmsg_add_u32(b, "timeout_s", ct.opt.timeout_s);
	blobmsg_add_u32(b, "grace_s", ct.opt.grace_s);
	blobmsg_add_u32(b, "arm_timeout_s", ct.opt.arm_timeout_s);
	blobmsg_add_u8(b, "wire", ct.opt.wire);
	blobmsg_add_u32(b, "wire_max_mb", ct.opt.wire_max_mb);
	blobmsg_add_u32(b, "rings_max_mb", ct.opt.rings_max_mb);
	blobmsg_add_u8(b, "lec", ct.opt.lec);
	blobmsg_add_u32(b, "lec_max_mb", ct.opt.lec_max_mb);
	blobmsg_close_table(b, tbl);
}

static void ct_times_dump(struct blob_buf *b)
{
	void *tbl = blobmsg_open_table(b, "times");

	if (ct.armed_at)
		blobmsg_add_u64(b, "armed_at", ct.armed_at);
	if (ct.capture_ready_at)
		blobmsg_add_u64(b, "capture_ready_at", ct.capture_ready_at);
	if (ct.started_at)
		blobmsg_add_u64(b, "started_at", ct.started_at);
	if (ct.ended_at)
		blobmsg_add_u64(b, "ended_at", ct.ended_at);
	blobmsg_close_table(b, tbl);
}

/* What the capture was pointed at, so a reader of the archive can tell a
 * missing packet from a packet the filter never admitted. */
static void ct_wire_counts_dump(struct blob_buf *b)
{
	void *tbl = blobmsg_open_table(b, "wire");

	blobmsg_add_u64(b, "bytes", ct.wire.bytes);
	blobmsg_add_u32(b, "restarts", ct.wire.restarts);
	blobmsg_add_u8(b, "truncated", ct.wire.truncated);
	if (ct.wire.iface[0])
		blobmsg_add_string(b, "iface", ct.wire.iface);
	if (ct.wire.filter[0])
		blobmsg_add_string(b, "filter", ct.wire.filter);
	if (ct.cpumask[0])
		blobmsg_add_string(b, "cpumask", ct.cpumask);
	if (ct.wire.child.exit_valid)
		blobmsg_add_u32(b, "exit", (uint32_t)ct.wire.child.exit);
	blobmsg_close_table(b, tbl);
}

static void ct_counts_dump(struct blob_buf *b)
{
	void *tbl, *sub;

	tbl = blobmsg_open_table(b, "counts");
	blobmsg_add_u32(b, "calls", ct.calls);
	blobmsg_add_u32(b, "snapshots", ct.snapshots);

	sub = blobmsg_open_table(b, "rings");
	ct_ring_rows_dump(b);
	blobmsg_add_u64(b, "relay_dropped", trace_relay_dropped());
	blobmsg_add_u64(b, "file_bytes", ct_export_bytes());
	blobmsg_add_u8(b, "truncated", ct.rings_truncated);
	blobmsg_close_table(b, sub);

	ct_wire_counts_dump(b);
	blobmsg_close_table(b, tbl);
}

/*
 * What the archive holds, so a reader knows what to treat it as before the
 * first packet is opened. The SIP ring and the wire capture carry the digest
 * responses and the SDP crypto lines of any call, so both words stand on
 * every trace; the key log word follows the option that writes that file.
 */
static void ct_contains_dump(struct blob_buf *b)
{
	void *arr = blobmsg_open_array(b, "contains");

	blobmsg_add_string(b, NULL, "sip_plaintext");
	blobmsg_add_string(b, NULL, "srtp_keys_in_sdp");
	/* The media ring records every datagram before protect and after
	 * unprotect, so an SRTP call reads in the clear in rings.pcapng. */
	blobmsg_add_string(b, NULL, "rtp_plaintext");
	if (uv.cfg && uv.cfg->tls_keylog_enabled)
		blobmsg_add_string(b, NULL, "tls_keylog");
	/* The microphone before the canceller is the near-end room in the
	 * clear, of every line in a call on the unit, not the traced line
	 * alone. */
	if (ct.opt.lec)
		blobmsg_add_string(b, NULL, "lec_audio");
	blobmsg_close_array(b, arr);
}

static void ct_notes_dump(struct blob_buf *b)
{
	void *arr = blobmsg_open_array(b, "notes");

	if (ct.wire.enabled && !strcmp(ct.cpumask, "all"))
		blobmsg_add_string(b, NULL, "the wire capture runs unpinned");
	if (ct.wire.restarts)
		blobmsg_add_string(b, NULL,
				   "the wire capture restarted at its cap and "
				   "holds a gap");
	if (ct.wire.truncated)
		blobmsg_add_string(b, NULL,
				   "the wire capture stopped at its cap");
	if (ct.wire.failed)
		blobmsg_add_string(b, NULL,
				   "the wire capture would not start, so this "
				   "trace holds no packets of its own");
	if (ct.rings_truncated)
		blobmsg_add_string(b, NULL,
				   "the media ring stopped at the export cap");
	if (ct.lec_truncated)
		blobmsg_add_string(b, NULL,
				   "the canceller ring stopped at its cap");
	if (ct.local_substituted)
		blobmsg_add_string(b, NULL,
				   "a SIP entry whose local address was "
				   "unspecified carries the voice interface "
				   "address instead");
	if (ct.wire.enabled)
		blobmsg_add_string(b, NULL,
				   "the wire capture does not follow an "
				   "interface change");
	blobmsg_close_array(b, arr);
}

static void ct_children_dump(struct blob_buf *b)
{
	void *tbl = blobmsg_open_table(b, "children");

	if (ct.wire.child.running)
		blobmsg_add_u32(b, "wire_pid",
				(uint32_t)ct.wire.child.proc.pid);
	if (ct.fin.running)
		blobmsg_add_u32(b, "finisher_pid", (uint32_t)ct.fin.proc.pid);
	blobmsg_close_table(b, tbl);
}

static void ct_versions_dump(struct blob_buf *b)
{
	void *tbl = blobmsg_open_table(b, "versions");

	blobmsg_add_string(b, "uvoice", UVOICE_VERSION);
	blobmsg_add_string(b, "schema", UVOICE_SCHEMA_VERSION);
	blobmsg_close_table(b, tbl);
}

/* The rename is what makes a reader of the dir always find a whole document.
 * The md5 of the binary, the board model and the serial come from the
 * finisher, which has the tools for them. */
static int ct_json_publish(const char *dir, const char *json)
{
	char path[CT_FILE_MAX];
	char tmp[CT_FILE_MAX];
	int err;

	ct_join(tmp, dir, ".manifest.json.tmp");
	ct_join(path, dir, "manifest.json");
	err = ct_file_write(tmp, json, strlen(json));
	if (err)
		return err;

	return rename(tmp, path) < 0 ? -errno : 0;
}

/* 0, or the errno of a publication that did not land. */
static int ct_manifest_publish(void)
{
	char *json;
	int err;

	/* A finisher that published the archive removed the dir with it, so
	 * the stage word moves and nothing is written. */
	if (!ct.have_dir || access(ct.dir, F_OK))
		return 0;

	ct_boot_id_load();
	blob_buf_init(&cb, 0);
	blobmsg_add_string(&cb, "id", ct.id);
	blobmsg_add_u32(&cb, "line", ct.line);
	blobmsg_add_string(&cb, "stage", ct.stage ? ct.stage : "armed");
	if (ct.reason[0])
		blobmsg_add_string(&cb, "reason", ct.reason);
	ct_times_dump(&cb);
	ct_options_dump(&cb);
	ct_counts_dump(&cb);
	ct_versions_dump(&cb);
	blobmsg_add_string(&cb, "boot_id", ct_boot_id);
	blobmsg_add_u32(&cb, "daemon_pid", (uint32_t)getpid());
	ct_children_dump(&cb);
	ct_contains_dump(&cb);
	ct_notes_dump(&cb);

	json = blobmsg_format_json(cb.head, true);
	if (!json)
		return -ENOMEM;

	err = ct_json_publish(ct.dir, json);
	free(json);

	return err;
}

/*
 * The journal of a running trace. It is the evidence a recovery reads, so a
 * dir that cannot take it is not a dir this trace can finish in and the trace
 * ends here. The first publication of all runs before the arm commits, where
 * a failure is still a refusal; this is every one after it.
 */
static void ct_manifest_write(void)
{
	int err = ct_manifest_publish();

	if (!err)
		return;

	ULOG_ERR("call trace %s: manifest write failed: %s\n", ct.id,
		 strerror(-err));
	ct_fail("export_failed", "manifest.json", err);
}

static void ct_stage_set(const char *stage)
{
	ct.stage = stage;
	ct_manifest_write();
}

/* ----------------------------------------------------------- the snapshots */

static uint64_t ct_rss_kb(void)
{
	unsigned long long size, resident;
	uint64_t kb = 0;
	char *text;

	text = ct_file_read(CT_STATM_PATH, CT_SMALL_READ);
	if (!text)
		return 0;

	if (sscanf(text, "%llu %llu", &size, &resident) == 2)
		kb = resident * (uint64_t)sysconf(_SC_PAGESIZE) / 1024;
	free(text);

	return kb;
}

/*
 * The clock pair of every snapshot. A reader that lines a ring entry up with
 * a syslog line needs both domains and the moment the offset moved: an NTP
 * step retimes the wall clock under a capture the monotonic clock stamped.
 */
static void ct_clock_dump(struct blob_buf *b, uint64_t wall_ms,
			  uint64_t mono_ns)
{
	int64_t offset = (int64_t)wall_ms - (int64_t)(mono_ns / 1000000ull);
	bool step = false;
	void *tbl;

	if (ct.clock_offset_valid) {
		int64_t moved = offset - ct.clock_offset_ms;

		step = moved > 100 || moved < -100;
	}
	ct.clock_offset_ms = offset;
	ct.clock_offset_valid = true;

	tbl = blobmsg_open_table(b, "clock");
	blobmsg_add_u64(b, "realtime_us", wall_ms * 1000);
	blobmsg_add_u64(b, "monotonic_ns", mono_ns);
	blobmsg_add_u8(b, "step_detected", step);
	blobmsg_close_table(b, tbl);
}

static bool ct_seq_known(uint32_t seq)
{
	unsigned int i;

	for (i = 0; i < ct.cdr_seqs; i++) {
		if (ct.cdr_seq[i] == seq)
			return true;
	}

	return false;
}

/* Whether one record of the ring belongs to this window: a leg the window
 * admitted, or a leg that took a call over from one of them. */
static bool ct_record_wanted(struct blob_attr *rec)
{
	enum { R_SEQ, R_FROM, __R_MAX };
	static const struct blobmsg_policy pol[__R_MAX] = {
		[R_SEQ] = { "seq", BLOBMSG_TYPE_INT32 },
		[R_FROM] = { "transferred_from", BLOBMSG_TYPE_INT32 },
	};
	struct blob_attr *tb[__R_MAX] = {};

	blobmsg_parse(pol, __R_MAX, tb, blobmsg_data(rec), blobmsg_len(rec));
	if (tb[R_SEQ] && ct_seq_known(blobmsg_get_u32(tb[R_SEQ])))
		return true;

	return tb[R_FROM] && ct_seq_known(blobmsg_get_u32(tb[R_FROM]));
}

static struct blob_attr *ct_member(struct blob_attr *table, const char *want)
{
	struct blob_attr *cur;
	size_t rem;

	blobmsg_for_each_attr(cur, table, rem) {
		const char *name = blobmsg_name(cur);

		if (name && !strcmp(name, want))
			return cur;
	}

	return NULL;
}

/*
 * The records of this window and no other. cdr_dump() renders the whole ring,
 * so the ring goes into the scratch buffer and the wanted records are copied
 * across. Other lines and earlier calls stay out.
 */
static void ct_cdr_dump(struct blob_buf *b)
{
	struct blob_attr *cur, *list;
	size_t rem;
	void *arr;

	blob_buf_init(&xb, 0);
	cdr_dump(&xb);
	list = ct_member(xb.head, "cdr");

	arr = blobmsg_open_array(b, "cdr");
	blobmsg_for_each_attr(cur, list, rem) {
		if (ct_record_wanted(cur))
			blobmsg_add_blob(b, cur);
	}
	blobmsg_close_array(b, arr);
}

/* The transport counters of the protocol, under its own key (ops->wire_stats).
 * The transport serves every line, so these are the daemon's totals and not
 * the traced line's. */
static void ct_wire_dump(struct blob_buf *b)
{
	const struct proto_ops *ops = proto_ops_get();

	if (ops->wire_stats)
		ops->wire_stats(b);
}

static bool ct_reason_listed(const char *reason)
{
	unsigned int i;

	for (i = 0; i < ct.reason_count; i++) {
		if (!strcmp(ct.reasons[i], reason))
			return true;
	}

	return false;
}

static void ct_reasons_dump(struct blob_buf *b)
{
	void *arr = blobmsg_open_array(b, "reasons");
	unsigned int i;

	for (i = 0; i < ct.reason_count; i++)
		blobmsg_add_string(b, NULL, ct.reasons[i]);
	blobmsg_close_array(b, arr);

	/* The list is bounded, and a second that overran it says so rather
	 * than let a reader believe it holds every reason. */
	if (ct.reasons_dropped)
		blobmsg_add_u32(b, "reasons_dropped", ct.reasons_dropped);
}

static void ct_config_dump(struct blob_buf *b)
{
	void *tbl;

	if (!uv.cfg)
		return;

	tbl = blobmsg_open_table(b, "config");
	cfg_dump(uv.cfg, b);
	blobmsg_close_table(b, tbl);
}

/* The keys the first snapshot alone carries. The HAL cap is not among them:
 * status_hal_dump() already renders the lines, the streams and the feature
 * words of the cap inside the hal table. */
static void ct_start_keys_dump(struct blob_buf *b)
{
	blobmsg_add_string(b, "version", UVOICE_VERSION);
	lines_region_status_dump(b);
	ct_config_dump(b);
}

/* The protocol's name of the state of the traced line. A line that names no
 * state is out of service, the one state such a line can be in. */
static const char *ct_line_state_name(void)
{
	const char *name = line_state_name(ct.line);

	return name ? name : "OOS";
}

static void ct_snapshot_body(struct blob_buf *b, const char *kind, bool start,
			     bool end)
{
	const struct proto_ops *ops = proto_ops_get();
	bool event = !strcmp(kind, "event");
	uint64_t wall_ms = ct_wall_ms();

	blobmsg_add_u64(b, "t_wall_ms", wall_ms);
	blobmsg_add_u64(b, "t_mono_ms", ct_mono_ms());
	blobmsg_add_u32(b, "seq", ct.seq++);
	blobmsg_add_string(b, "kind", kind);
	/* The reason list belongs to the snapshot the events asked for and to
	 * the end, which is the last one a queued reason can reach; a periodic
	 * that runs inside the same second claims none of them. */
	if (event || (end && ct.reason_count))
		ct_reasons_dump(b);
	ct_clock_dump(b, wall_ms, ct_mono_ns());

	blobmsg_add_u32(b, "line", ct.line);
	blobmsg_add_string(b, "fsm_state", ct_line_state_name());
	blobmsg_add_u32(b, "config_generation", uv.config_generation);

	status_hal_dump(b);
	ops->status_dump_lines(b, (int)ct.line);
	/* The network table answers a registration question, so it rides the
	 * first snapshot and the ones a registration move asked for. */
	if (start || ct_reason_listed("registration"))
		net_iface_status_dump(b);
	/* the protocol keys of the traced line, the machine and its calls */
	ops->status_dump_call(b, ct.line);
	status_line_rows_dump(b, ct.line);
	status_diag_dump(b, (int)ct.line);
	cdr_stats_dump(b, (int)ct.line);
	ct_wire_dump(b);
	blobmsg_add_u64(b, "rss_kb", ct_rss_kb());

	if (start)
		ct_start_keys_dump(b);
	else if (ct_reason_listed("config"))
		ct_config_dump(b);

	if (end)
		ct_cdr_dump(b);
}

/*
 * One snapshot: the same JSON object on the status ring and on
 * snapshots.jsonl, so a reader of either file sees the same document. The
 * ring copy rides the pcapng export beside the SIP messages and the trace
 * lines; the file copy survives an export that stopped at its cap.
 */
static void ct_snapshot(const char *kind)
{
	bool start = !strcmp(kind, "start");
	bool end = !strcmp(kind, "end");
	char path[CT_FILE_MAX];
	char *json;
	size_t len;
	int err;

	if (!ct.have_dir)
		return;

	blob_buf_init(&cb, 0);
	ct_snapshot_body(&cb, kind, start, end);
	json = blobmsg_format_json(cb.head, true);
	if (!json)
		return;

	len = strlen(json);
	trace_ring_status(json);
	json[len] = '\n';
	ct_path(path, "snapshots.jsonl");
	err = ct_file_append(path, json, len + 1);
	free(json);

	/* A snapshot that reached neither file is not a snapshot, so the
	 * count follows the write and a failed write ends the trace. */
	if (err) {
		ULOG_ERR("call trace %s: snapshot write failed: %s\n", ct.id,
			 strerror(-err));
		ct_fail("export_failed", "snapshots.jsonl", err);
		return;
	}

	ct.snapshots++;
}

/* --------------------------------------------------------------- the export */

static unsigned int ct_export_period(void)
{
	return ct.opt.lec ? ct_export_lec_ms : ct_export_ms;
}

/*
 * The canceller ring at its cap. The export of the ring stops before the
 * push, so the ring ends at the size the check read. The push then takes the
 * capture bit down (calltrace_lec_capturing() reads lec_truncated), and the
 * backend stops copying frames that nothing exports. Its final drain goes
 * into a ring that no longer exports, and the close counts those records as
 * truncated.
 */
static void ct_lec_stop(void)
{
	ct.lec_truncated = true;
	trace_ring_export_ring_enable(TRACE_RING_LEC, false);
	media_trace_sync();
	ct_manifest_write();
}

static void ct_export_step(struct uloop_timeout *t)
{
	uint64_t lec_bytes = trace_ring_export_ring_bytes(TRACE_RING_LEC);
	bool done = false;
	int err;

	if (!ct.export_open)
		return;
	if (ct.state != CT_ARMED && ct.state != CT_RUNNING)
		return;

	uloop_timeout_set(t, (int)ct_export_period());
	/* The canceller ring stops alone at a cap of its own, so a long
	 * capture cannot end the media evidence early. A journal that fails
	 * there ends the trace and closes the export, so nothing below may
	 * run. */
	if (ct.opt.lec && !ct.lec_truncated &&
	    lec_bytes >= (uint64_t)ct.opt.lec_max_mb * CT_MIB) {
		ct_lec_stop();
		if (ct.state != CT_ARMED && ct.state != CT_RUNNING)
			return;
	}

	/* The file stops growing for the media ring alone. The log, the SIP
	 * and the status rings are the cheap evidence and keep exporting, so
	 * the file may pass the cap by what their geometry holds. The bytes of
	 * the canceller ring count against its own cap and not against this
	 * one. A journal that fails at this cap also ends the trace and closes
	 * the export. */
	if (!ct.rings_truncated &&
	    trace_ring_export_bytes() - lec_bytes >=
	    (uint64_t)ct.opt.rings_max_mb * CT_MIB) {
		ct.rings_truncated = true;
		trace_ring_export_ring_enable(TRACE_RING_RTP, false);
		ct_manifest_write();
		if (ct.state != CT_ARMED && ct.state != CT_RUNNING)
			return;
	}

	err = trace_ring_export_step(CT_EXPORT_BATCH, &done);
	if (!err)
		return;

	ULOG_ERR("call trace %s: the ring export failed: %s\n", ct.id,
		 strerror(-err));
	ct_fail("export_failed", "rings.pcapng", err);
}

/* ------------------------------------------------------------- the children */

static void ct_child_record(struct ct_child *c, char *const argv[],
			    unsigned int argc)
{
	unsigned int i;

	memset(c->argv, 0, sizeof(c->argv));
	c->argc = argc;
	for (i = 0; i < argc && i < CT_ARGV_MAX; i++)
		snprintf(c->argv[i], CT_ARG_MAX, "%.*s", CT_ARG_MAX - 1,
			 argv[i]);
}

/*
 * Start one helper in a process group of its own, so a stop reaches it and
 * everything it started. Test mode keeps the argument list, starts nothing,
 * and leaves the completion to calltrace_test_child_done().
 */
static bool ct_child_start(struct ct_child *c, char *const argv[],
			   unsigned int argc, uloop_process_handler cb)
{
	pid_t pid;

	ct_child_record(c, argv, argc);
	c->stopping = false;
	c->exit_valid = false;
	c->nsignals = 0;
	c->signals[0] = '\0';
	if (ct_test_mode) {
		c->running = !ct_spawn_fail_test;
		return c->running;
	}

	pid = spawn_run(argv, SPAWN_GROUP_OWN);
	if (pid < 0) {
		ULOG_ERR("call trace %s: cannot start %s: %s\n", ct.id, argv[0],
			 strerror(errno));
		return false;
	}

	c->proc.pid = pid;
	c->proc.cb = cb;
	if (uloop_process_add(&c->proc)) {
		ULOG_ERR("call trace %s: cannot reap %s\n", ct.id, argv[0]);
		kill(-pid, SIGTERM);
		waitpid(pid, NULL, 0);
		return false;
	}

	c->running = true;

	return true;
}

static void ct_child_signal(struct ct_child *c, int sig)
{
	if (!c->running)
		return;

	if (ct_test_mode) {
		if (c->nsignals < CT_SIGNALS_MAX) {
			c->signals[c->nsignals++] = sig == SIGKILL ? 'K' : 'T';
			c->signals[c->nsignals] = '\0';
		}
		return;
	}

	kill(-c->proc.pid, sig);
}

static void ct_child_kill_cb(struct uloop_timeout *t)
{
	struct ct_child *c = container_of(t, struct ct_child, kill);

	ct_child_signal(c, SIGKILL);
}

/*
 * Ask one child to go. A stop of a child that is already stopping changes
 * nothing: the deadline is set once, so a second stop cannot push the kill
 * out and leave the daemon waiting on a process that ignores SIGTERM.
 */
static void ct_child_stop(struct ct_child *c)
{
	if (!c->running || c->stopping)
		return;

	c->stopping = true;
	ct_child_signal(c, SIGTERM);
	c->kill.cb = ct_child_kill_cb;
	uloop_timeout_set(&c->kill, (int)ct_kill_ms);
}

/* The reap, the one place a child stops being owned. */
static void ct_child_reaped(struct ct_child *c, int code)
{
	uloop_timeout_cancel(&c->kill);
	c->running = false;
	c->stopping = false;
	c->exit = code;
	c->exit_valid = true;
}

/* Drop the reap registration without waiting. The daemon stop uses it and
 * nothing else: procd gives five seconds and a child that ignores SIGTERM
 * must not spend them. */
static void ct_child_abandon(struct ct_child *c)
{
	if (!c->running)
		return;

	uloop_timeout_cancel(&c->kill);
	c->running = false;
	c->stopping = false;
	if (!ct_test_mode && c->proc.pending)
		uloop_process_delete(&c->proc);
}

static bool ct_children_alive(void)
{
	return ct.wire.child.running || ct.fin.running || rc.child.running;
}

/* --------------------------------------------------------- the wire capture */

/*
 * The CPU list the wire capture runs on: every online CPU except the one the
 * media engine is pinned to. A heavy reader on the engine CPU costs the
 * engine its deadline, which is what the list keeps it off. A single CPU
 * board, or a config with no pinning, answers "all" and the script then runs
 * tcpdump unpinned, which the manifest notes.
 */
static void ct_cpumask_make(void)
{
	long online = ct_cpus_test ? ct_cpus_test :
		sysconf(_SC_NPROCESSORS_ONLN);
	int engine = uv.cfg ? uv.cfg->hal.engine_cpu : -1;
	size_t used = 0;
	long cpu;

	ct.cpumask[0] = '\0';
	if (engine < 0 || online < 2) {
		snprintf(ct.cpumask, sizeof(ct.cpumask), "all");
		return;
	}

	for (cpu = 0; cpu < online; cpu++) {
		int add;

		if (cpu == engine)
			continue;

		add = snprintf(ct.cpumask + used, sizeof(ct.cpumask) - used,
			       "%s%ld", used ? "," : "", cpu);
		if (add < 0 || (size_t)add >= sizeof(ct.cpumask) - used)
			break;
		used += (size_t)add;
	}

	if (!ct.cpumask[0])
		snprintf(ct.cpumask, sizeof(ct.cpumask), "all");
}

/*
 * The capture filter. The signaling ports the protocol names catch every
 * exchange of the line (ops->wire_ports), and the media range catches the
 * call itself. DNS and ICMP ride along, because a call that never reached
 * the proxy usually failed in one of them.
 */
static void ct_filter_make(char *out, size_t len)
{
	const struct proto_ops *ops = proto_ops_get();
	uint16_t ports[CT_WIRE_PORTS_MAX];
	char terms[CT_WIRE_PORTS_MAX * sizeof("port 65535 or ")] = "";
	uint16_t rtp_min = 0, rtp_max = 0;
	unsigned int n = 0, i;
	size_t at = 0;

	if (uv.cfg && ct.line < CFG_CHANNEL_MAX)
		net_port_range(&uv.cfg->channel[ct.line], &rtp_min, &rtp_max);
	if (ops->wire_ports)
		n = ops->wire_ports(ct.line, ports, CT_WIRE_PORTS_MAX);

	for (i = 0; i < n; i++)
		at += (size_t)snprintf(terms + at, sizeof(terms) - at,
				       "port %u or ", ports[i]);

	snprintf(out, len,
		 "((udp or tcp) and (%sportrange %u-%u)) "
		 "or icmp or icmp6 or port 53", terms, rtp_min, rtp_max);
}

/*
 * The netdev the line binds to: the channel's voice interface, else the base
 * one, through the rule of net_iface_bind_device(). A name the kernel does
 * not know, or one that does not fit, captures on every device.
 */
static void ct_wire_iface(char *out, size_t len)
{
	const struct cfg_channel *ch = NULL;
	const char *iface = "";
	const char *dev;

	if (uv.cfg && ct.line < CFG_CHANNEL_MAX) {
		ch = &uv.cfg->channel[ct.line];
		iface = ch->local_interface[0] ? ch->local_interface :
			uv.cfg->local_interface;
	}

	dev = net_iface_bind_device(iface);
	if (!dev || (size_t)snprintf(out, len, "%s", dev) >= len)
		snprintf(out, len, "any");
}

static void ct_wire_proc_cb(struct uloop_process *p, int ret);

static bool ct_wire_start(void)
{
	char *argv[CT_ARGV_MAX];

	ct_filter_make(ct.wire.filter, sizeof(ct.wire.filter));
	ct_wire_iface(ct.wire.iface, sizeof(ct.wire.iface));

	argv[0] = (char *)CT_WIRE_SCRIPT;
	argv[1] = ct.dir;
	argv[2] = ct.cpumask;
	argv[3] = ct.wire.iface;
	argv[4] = ct.wire.filter;
	argv[5] = NULL;

	ct.wire.ready_tries = 0;
	ct.wire.restart_pending = false;
	if (!ct_child_start(&ct.wire.child, argv, 5, ct_wire_proc_cb))
		return false;

	uloop_timeout_set(&ct.t_ready, CT_WIRE_READY_MS);

	return true;
}

/*
 * The wire capture is reaped. The freeze waits here for it, because the
 * archive names the capture's own exit status and the finisher must not pack
 * a dir a running tcpdump still writes into. A restart the cap asked for
 * starts here too, so two captures never share the file.
 */
static void ct_wire_reaped(int code)
{
	ct_child_reaped(&ct.wire.child, code);

	if (ct.state == CT_FINISHING && !ct.fin.running && !ct.fin.exit_valid) {
		ct_finish_start();
		return;
	}

	if ((ct.state == CT_ARMED || ct.state == CT_RUNNING) &&
	    ct.wire.restart_pending) {
		char path[CT_FILE_MAX];

		/* The file the capped capture filled goes with it, so the new
		 * one starts from nothing and the gap is one count. */
		ct_path(path, "wire.pcap");
		unlink(path);
		ct.wire.bytes = 0;
		ct.wire.ready = false;
		ct.wire.restarts++;
		ct_wire_start();
		ct_manifest_write();
	}
}

static void ct_wire_proc_cb(struct uloop_process *p, int ret)
{
	ct_wire_reaped(WIFEXITED(ret) ? WEXITSTATUS(ret) : -1);
}

/* tcpdump writes the pcap file header when it opens the interface, so a file
 * that reached those bytes is a capture that started. */
static void ct_wire_ready_cb(struct uloop_timeout *t)
{
	char path[CT_FILE_MAX];

	if (ct.wire.ready || !ct.wire.enabled || !ct.have_dir)
		return;
	if (ct.state != CT_ARMED && ct.state != CT_RUNNING)
		return;

	ct_path(path, "wire.pcap");
	if (ct_file_size(path) >= CT_PCAP_HDR_BYTES) {
		ct.wire.ready = true;
		ct.capture_ready_at = ct_wall_s();
		return;
	}

	if (++ct.wire.ready_tries >= CT_WIRE_READY_TRIES)
		return;

	uloop_timeout_set(t, CT_WIRE_READY_MS);
}

/*
 * The size poll and the cap. A capture that reaches its cap while the window
 * runs is stopped and the trace says so. One that reaches it while the trace
 * only waits is stopped and started again from its reap, because an hour of
 * registration traffic must not use up the budget of the call the operator is
 * waiting for.
 */
static void ct_wire_poll_cb(struct uloop_timeout *t)
{
	char path[CT_FILE_MAX];
	uint64_t cap;

	if (!ct.wire.enabled || !ct.have_dir)
		return;
	if (ct.state != CT_ARMED && ct.state != CT_RUNNING)
		return;

	uloop_timeout_set(t, (int)ct_wire_poll_ms);
	ct_path(path, "wire.pcap");
	ct.wire.bytes = ct_file_size(path);
	cap = (uint64_t)ct.opt.wire_max_mb * CT_MIB;
	if (ct.wire.bytes <= cap || !ct.wire.child.running)
		return;

	/* A capture over its cap stays over it until it goes, so this runs
	 * every tick until the reap; the stop is what holds the one deadline
	 * and the repeat costs nothing. */
	if (ct.state == CT_RUNNING)
		ct.wire.truncated = true;
	else
		ct.wire.restart_pending = true;

	ct_child_stop(&ct.wire.child);
	ct_manifest_write();
}

/* ------------------------------------------------------- the signaling tap */

static bool ct_addr_unspecified(const struct trace_endpoint *ep)
{
	unsigned int len = ep->ipver == 6 ? 16 : 4;
	unsigned int i;

	for (i = 0; i < len; i++) {
		if (ep->addr[i])
			return false;
	}

	return true;
}

/*
 * The local endpoint always names a family. An all-zero address under it
 * means the address is unspecified, which a wildcard datagram bind reports
 * and so does a stream message staged while its connection was still coming
 * up. An entry with no source address decodes as nothing, so the voice
 * interface address of that family stands in for it and the manifest says so.
 * A getter that knows no address of the family leaves the zeros in place.
 */
static void ct_wire_tap(bool tx, const struct proto_tap_endpoints *ep,
			const void *buf, size_t len)
{
	struct trace_endpoint local = ep->local;
	const struct trace_endpoint *peer = &ep->peer;

	if (ct_addr_unspecified(&local) &&
	    !net_iface_address(local.ipver, local.addr))
		ct.local_substituted = true;

	trace_ring_packet(TRACE_RING_SIP,
			  tx ? TRACE_PKT_TX : TRACE_PKT_RX,
			  (enum trace_pkt_kind)ep->kind,
			  tx ? &local : peer, tx ? peer : &local,
			  0, buf, len);
}

/* Install or remove the tap in the protocol that runs (ops->tap_set). */
static void ct_tap_set(proto_tap_fn fn)
{
	const struct proto_ops *ops = proto_ops_get();

	if (ops->tap_set)
		ops->tap_set(fn);
}

/* ------------------------------------------------------------- the timers */

static void ct_timers_cancel(void)
{
	uloop_timeout_cancel(&ct.t_arm);
	uloop_timeout_cancel(&ct.t_window);
	uloop_timeout_cancel(&ct.t_grace);
	uloop_timeout_cancel(&ct.t_snap);
	uloop_timeout_cancel(&ct.t_export);
	uloop_timeout_cancel(&ct.t_wire);
	uloop_timeout_cancel(&ct.t_ready);
	uloop_timeout_cancel(&ct.t_event);
}

static void ct_arm_expire_cb(struct uloop_timeout *t);
static void ct_window_expire_cb(struct uloop_timeout *t);
static void ct_grace_cb(struct uloop_timeout *t);
static void ct_snap_cb(struct uloop_timeout *t);
static void ct_event_cb(struct uloop_timeout *t);
static void ct_fin_expire_cb(struct uloop_timeout *t);
static void ct_recover_expire_cb(struct uloop_timeout *t);

static void ct_timers_init(void)
{
	ct.t_arm.cb = ct_arm_expire_cb;
	ct.t_window.cb = ct_window_expire_cb;
	ct.t_grace.cb = ct_grace_cb;
	ct.t_snap.cb = ct_snap_cb;
	ct.t_export.cb = ct_export_step;
	ct.t_wire.cb = ct_wire_poll_cb;
	ct.t_ready.cb = ct_wire_ready_cb;
	ct.t_event.cb = ct_event_cb;
	ct.t_fin.cb = ct_fin_expire_cb;
	ct.wire.child.kill.cb = ct_child_kill_cb;
	ct.fin.kill.cb = ct_child_kill_cb;
	rc.child.kill.cb = ct_child_kill_cb;
	rc.budget.cb = ct_recover_expire_cb;
}

/* ------------------------------------------------------------- the teardown */

static void ct_capture_stop(void)
{
	/* The tap and the mask first: the hook trace_ring_set() runs drops the
	 * backend debug bits, so the library's last lines reach a ring that is
	 * still open. */
	ct_tap_set(NULL);
	trace_ring_set(0);
	trace_relay_drain();
}

/* 0, or the errno of a close that could not finish the file. */
static int ct_export_finish(void)
{
	int err;

	if (!ct.export_open)
		return 0;

	ct.export_open = false;
	err = trace_ring_export_close(&ct.file_bytes);

	return err == -ENOENT ? 0 : err;
}

/*
 * Everything one trace holds, except the children: a child is owned until its
 * reap, and an arm is refused while one is alive, so nothing here can run
 * over a process that still exists.
 */
static void ct_trace_clear(void)
{
	ct.wire.bytes = 0;
	ct.wire.restarts = 0;
	ct.wire.ready_tries = 0;
	ct.wire.enabled = false;
	ct.wire.ready = false;
	ct.wire.truncated = false;
	ct.wire.restart_pending = false;
	ct.wire.failed = false;
	ct.wire.iface[0] = '\0';
	ct.wire.filter[0] = '\0';
	ct.wire.child.exit_valid = false;
	ct.fin.exit_valid = false;
	ct.fin.argc = 0;
	ct.have_id = false;
	ct.have_dir = false;
	ct.have_archive = false;
	ct.rings_truncated = false;
	ct.lec_truncated = false;
	ct.local_substituted = false;
	ct.waiting_call = false;
	ct.failing = false;
	ct.armed_at = 0;
	ct.capture_ready_at = 0;
	ct.started_at = 0;
	ct.ended_at = 0;
	ct.started_mono_ms = 0;
	ct.clock_offset_valid = false;
	ct.calls = 0;
	ct.snapshots = 0;
	ct.seq = 0;
	ct.cdr_seqs = 0;
	ct.reason_count = 0;
	ct.reasons_dropped = 0;
	ct.file_bytes = 0;
	ct.id[0] = '\0';
	ct.dir[0] = '\0';
	ct.archive[0] = '\0';
	ct.cpumask[0] = '\0';
	ct.failure[0] = '\0';
	ct.finish_budget_ms = 0;
	ct.stage = NULL;
}

static void ct_idle_reset(const char *reason)
{
	char keep[CT_WORD_MAX];

	ct_word_set(keep, reason);
	ct_trace_clear();
	ct_state_set(CT_IDLE);
	/* The word of the last refusal stands until the next arm clears it. */
	ct_word_set(ct.reason, keep);
}

/*
 * The arm that never became a window: the capture stops, the rings close and
 * the work dir goes, because it holds no call. The wire child is asked to go
 * and stays owned until its reap; the next arm is refused as busy until then.
 */
static void ct_teardown(const char *reason)
{
	int err;

	ct_timers_cancel();
	ct_capture_stop();
	ct_child_stop(&ct.wire.child);
	err = ct_export_finish();
	trace_ring_close();

	/* An arm that collected nothing still wrote a file, and a file that
	 * could not be finished is a fault worth keeping the dir for. */
	if (err) {
		ULOG_ERR("call trace %s: the export could not close: %s\n",
			 ct.id, strerror(-err));
		ct_fail("export_failed", "rings.pcapng", err);
		return;
	}

	ct_word_set(ct.reason, reason);
	ct_stage_set("cancelled");
	if (ct.state == CT_FAILED)
		return;

	ct_dir_remove(ct.dir);
	ct_idle_reset(reason);
}

/*
 * A trace that cannot go on. The children stay owned, so the stop reaches
 * them and the reap still lands; what ends here is the capture and the file.
 * The work dir stays until the next arm removes it, because it is the
 * evidence of the failure.
 */
static void ct_fail(const char *reason, const char *what, int err)
{
	if (ct.failing || ct.state == CT_FAILED)
		return;

	ct.failing = true;
	ct_timers_cancel();
	uloop_timeout_cancel(&ct.t_fin);
	uloop_timeout_cancel(&rc.budget);
	ct_capture_stop();
	/* Every child of this daemon's trace work goes, not the capture
	 * alone: a finisher packing a dir this trace can no longer stand
	 * behind, and a recovery run beside it, are both work nobody wants
	 * the answer to now. Each stays owned until its reap. */
	ct_child_stop(&ct.wire.child);
	ct_child_stop(&ct.fin);
	ct_child_stop(&rc.child);
	/* The freeze order, for the freeze reason: the push this makes takes
	 * the backend's packet trace and canceller capture down, and their
	 * last drains must reach the rings that the export below reads. The
	 * file of a failed trace is evidence too, so it keeps them. */
	ct_state_set(CT_FAILED);
	ct_export_finish();
	trace_ring_close();
	ct.ended_at = ct_wall_s();
	ct_word_set(ct.reason, reason);
	if (what) {
		snprintf(ct.failure, sizeof(ct.failure), "%s: %s", what,
			 strerror(-err));
		snprintf(ct.last_error, sizeof(ct.last_error), "%s",
			 ct.failure);
	}
	ct_stage_set("failed");
	ct.failing = false;
}

/* ------------------------------------------------------------- the finisher */

/* The budget of one finisher run (CT_FINISH_MIB_MS). Each caller takes it
 * before the spawn, so it follows the dir the run is handed and not the
 * collector output that the run adds. */
static int ct_finish_budget_ms(const char *dir)
{
	uint64_t mib = (ct_dir_bytes(dir) + CT_MIB - 1) / CT_MIB;
	unsigned int mib_ms = ct_finish_mib_test ? ct_finish_mib_test :
		CT_FINISH_MIB_MS;

	ct_finish_last_ms = (int)(ct_finish_ms + mib * mib_ms);

	return ct_finish_last_ms;
}

static void ct_finish_done(int code)
{
	uloop_timeout_cancel(&ct.t_fin);

	if (code) {
		ct_state_set(CT_FAILED);
		ct_word_set(ct.reason, "finisher_failed");
		snprintf(ct.failure, sizeof(ct.failure), "exit %d", code);
		snprintf(ct.last_error, sizeof(ct.last_error), "%s",
			 ct.failure);
		ct_stage_set("failed");
		return;
	}

	/* The archive is named only once the journal that records it landed:
	 * a reply that named an archive and a failure together would say two
	 * things about the same trace. */
	ct_state_set(CT_DONE);
	ct_stage_set("done");
	if (ct.state != CT_DONE)
		return;

	ct.have_archive = true;
	/* The finisher removes the work dir once the archive is published, so
	 * the reply stops naming a dir that is no longer there. */
	if (access(ct.dir, F_OK))
		ct.have_dir = false;
	ct_retention_sweep(ct.archive);
}

static void ct_fin_proc_cb(struct uloop_process *p, int ret)
{
	int code = WIFEXITED(ret) ? WEXITSTATUS(ret) : -1;

	ct_child_reaped(&ct.fin, code);
	/* A trace that already failed keeps its reason: this reap is the exit
	 * of a run whose answer nobody wants, and it publishes no archive. */
	if (ct.state == CT_FINISHING)
		ct_finish_done(code);
}

/* The budget asks the finisher to go and the escalation of the child takes it
 * from there; the reap is what decides the trace. */
static void ct_fin_expire_cb(struct uloop_timeout *t)
{
	ULOG_WARN("call trace %s: the finisher outran its budget\n", ct.id);
	ct_child_stop(&ct.fin);
}

static void ct_finish_start(void)
{
	char *argv[CT_ARGV_MAX];
	char pid[16];
	int budget;

	snprintf(ct.archive, sizeof(ct.archive), "%s/%s.tar.gz", ct_root,
		 ct.id);
	snprintf(pid, sizeof(pid), "%u", (unsigned int)getpid());

	argv[0] = (char *)CT_FINISH_SCRIPT;
	argv[1] = ct.dir;
	argv[2] = ct.archive;
	argv[3] = ct.cpumask;
	argv[4] = pid;
	argv[5] = NULL;

	/*
	 * The finisher packs the dir as it stands when it starts, so the
	 * journal it packs is written first: it carries the capture's exit
	 * status and names no child that has already gone. A dir that cannot
	 * take it ends the trace here, with nothing spawned.
	 */
	ct_manifest_write();
	if (ct.state != CT_FINISHING)
		return;

	budget = ct_finish_budget_ms(ct.dir);
	if (!ct_child_start(&ct.fin, argv, 5, ct_fin_proc_cb)) {
		ct_finish_done(-1);
		return;
	}

	ct.finish_budget_ms = budget;
	uloop_timeout_set(&ct.t_fin, budget);
}

/*
 * The freeze, in the order the archive needs: the producers stop, the last
 * lines drain, the end snapshot is written while the rings still take it, the
 * export closes, the rings close, the journal records the stage, and the wire
 * capture is asked to go. The finisher starts at that capture's reap, so the
 * dir it packs is one nothing still writes into and the archive carries the
 * capture's own exit status.
 */
static void ct_freeze(const char *reason)
{
	int err;

	if (ct.state != CT_RUNNING)
		return;

	ct_timers_cancel();
	ct_capture_stop();
	ct_snapshot("end");
	if (ct.state == CT_FAILED)
		return;

	/* Before the final export and before the rings go: the push this makes
	 * takes the backend's packet trace and canceller capture down, and
	 * their last drains must reach the rings that the export below reads.
	 * The backend drains inside the push, so the records are in the rings
	 * when the export closes. */
	ct_state_set(CT_FINISHING);
	ct.ended_at = ct_wall_s();
	ct_word_set(ct.reason, reason);
	ct.reason_count = 0;
	ct.reasons_dropped = 0;

	err = ct_export_finish();
	trace_ring_close();
	if (err) {
		ULOG_ERR("call trace %s: the export could not close: %s\n",
			 ct.id, strerror(-err));
		ct_state_set(CT_RUNNING);
		ct_fail("export_failed", "rings.pcapng", err);
		return;
	}

	ct_stage_set("finishing");
	if (ct.state == CT_FAILED)
		return;

	if (ct.wire.child.running) {
		ct_child_stop(&ct.wire.child);
		return;
	}

	ct_finish_start();
}

/* ------------------------------------------------------------- the window */

static void ct_window_start(void)
{
	uloop_timeout_cancel(&ct.t_arm);
	ct_state_set(CT_RUNNING);
	ct.started_at = ct_wall_s();
	ct.started_mono_ms = ct_mono_ms();
	ct.calls++;
	ct_stage_set("running");
	ct_snapshot("start");

	/* Either write can fail the trace, and a failed trace arms nothing:
	 * a window timer on a trace that ended would freeze it a second
	 * time. */
	if (ct.state != CT_RUNNING)
		return;

	uloop_timeout_set(&ct.t_window, ct_timer_ms(ct.opt.timeout_s));
	uloop_timeout_set(&ct.t_snap, (int)ct_snapshot_ms);
}

static void ct_armed_move(enum line_rest before, enum line_rest after)
{
	/* A line that leaves service before the window opened is a line this
	 * trace cannot follow: the arm ends rather than wait for a call the
	 * restarted line will not place on it. */
	if (after == LINE_REST_OOS) {
		ct_teardown("line_restart");
		return;
	}

	/* A line that was off hook at the arm waits for its next rest, so the
	 * window starts on a seize and never in the middle of a call. */
	if (!ct.waiting_call) {
		if (after != LINE_REST_IDLE)
			return;

		/* The call that was already up when the arm landed is not this
		 * window's, and neither are the records it opened. */
		ct.waiting_call = true;
		ct.cdr_seqs = 0;
		return;
	}

	if (before == LINE_REST_IDLE)
		ct_window_start();
}

static void ct_running_move(enum line_rest before, enum line_rest after)
{
	/* A line that leaves service is not resting: the grace is there for a
	 * subscriber who lifts the handset again, and a restart ends the
	 * window on the spot. */
	if (after == LINE_REST_OOS) {
		ct_freeze("line_restart");
		return;
	}

	if (after == LINE_REST_IDLE) {
		uloop_timeout_set(&ct.t_grace, ct_timer_ms(ct.opt.grace_s));
		return;
	}

	if (before != LINE_REST_IDLE || !ct.t_grace.pending)
		return;

	uloop_timeout_cancel(&ct.t_grace);
	ct.calls++;
}

static void ct_reason_add(const char *reason)
{
	if (!ct_reason_listed(reason)) {
		if (ct.reason_count < CT_REASONS_MAX)
			ct_word_set(ct.reasons[ct.reason_count++], reason);
		else
			ct.reasons_dropped++;
	}

	if (!ct.t_event.pending)
		uloop_timeout_set(&ct.t_event, CT_EVENT_COALESCE_MS);
}

/*
 * The state names are upper case tokens of the machine, and a reason word is
 * lower case, so a reader of reasons[] has one vocabulary and not two.
 */
static void ct_word_lower(char *word)
{
	for (; *word; word++) {
		if (*word >= 'A' && *word <= 'Z')
			*word += 'a' - 'A';
	}
}

static void ct_fsm_reason(const char *before, const char *after)
{
	char reason[CT_WORD_MAX];

	if (ct.state != CT_RUNNING)
		return;

	snprintf(reason, sizeof(reason), "fsm_%s_%s", before ? before : "",
		 after ? after : "");
	ct_word_lower(reason);
	ct_reason_add(reason);
}

static void ct_arm_expire_cb(struct uloop_timeout *t)
{
	if (ct.state != CT_ARMED)
		return;

	ct_teardown("arm_expired");
}

static void ct_window_expire_cb(struct uloop_timeout *t)
{
	ct_freeze("timeout");
}

static void ct_grace_cb(struct uloop_timeout *t)
{
	ct_freeze("call_ended");
}

static void ct_snap_cb(struct uloop_timeout *t)
{
	if (ct.state != CT_RUNNING)
		return;

	uloop_timeout_set(t, (int)ct_snapshot_ms);
	ct_snapshot("periodic");
}

static void ct_event_cb(struct uloop_timeout *t)
{
	if (ct.state != CT_RUNNING || !ct.reason_count)
		return;

	ct_snapshot("event");
	ct.reason_count = 0;
	ct.reasons_dropped = 0;
}

/* ------------------------------------------------------------ the preflight */

static int ct_refuse(const char *word, int err)
{
	snprintf(ct.last_error, sizeof(ct.last_error), "%s", word);

	return err;
}

static bool ct_line_ok(unsigned int line)
{
	const struct hal_cap *cap;

	if (!uv.hal || line >= CFG_CHANNEL_MAX || !uv.cfg)
		return false;

	cap = hal_backend_ops.cap_get(uv.hal);
	if (!cap || line >= cap->lines)
		return false;

	return uv.cfg->channel[line].enabled;
}

static void ct_options_take(const struct calltrace_options *o,
			    struct calltrace_options *out)
{
	*out = *o;
	if (!out->timeout_s)
		out->timeout_s = CALLTRACE_TIMEOUT_DEFAULT_S;
	if (!out->grace_s)
		out->grace_s = CALLTRACE_GRACE_DEFAULT_S;
	if (!out->arm_timeout_s)
		out->arm_timeout_s = CALLTRACE_ARM_TIMEOUT_DEF_S;
}

static bool ct_range_ok(const struct calltrace_options *o)
{
	if (o->timeout_s > CALLTRACE_TIMEOUT_MAX_S)
		return false;
	if (o->grace_s > CALLTRACE_GRACE_MAX_S)
		return false;
	if (o->arm_timeout_s > CALLTRACE_ARM_TIMEOUT_MAX_S)
		return false;

	return o->wire_max_mb && o->rings_max_mb && o->lec_max_mb;
}

static uint64_t ct_space_free(void)
{
	struct statvfs vfs;

	if (ct_space_test)
		return ct_space_test;

	if (statvfs(CT_SPACE_PATH, &vfs) < 0)
		return 0;

	return (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
}

/* The work dir of a trace that ended. A failed trace keeps it as evidence. A
 * done trace keeps one only when the dir outlived its finisher, which
 * ct_finish_done() checks. */
static bool ct_dir_reclaimable(void)
{
	return (ct.state == CT_DONE || ct.state == CT_FAILED) && ct.have_dir;
}

/* /tmp is RAM, and while this daemon runs nothing else removes the dir of a
 * trace that ended: only a recovery finisher of the next daemon start packs
 * and removes it. The arm removes it once the preflight passed, so its bytes
 * count as free. */
static bool ct_space_ok(const struct calltrace_options *o)
{
	uint64_t want = (uint64_t)o->wire_max_mb + o->rings_max_mb +
			CT_HEADROOM_MB;
	uint64_t free_bytes = ct_space_free();

	if (o->lec)
		want += o->lec_max_mb;
	if (ct_dir_reclaimable())
		free_bytes += ct_dir_bytes(ct.dir);

	return free_bytes >= want * CT_MIB;
}

/* The capture tools the wire child needs. Test mode starts no child, so it
 * asks for neither. */
static const char *ct_tool_missing(const struct calltrace_options *o)
{
	if (ct_test_mode || !o->wire)
		return NULL;

	if (access(CT_TCPDUMP, X_OK))
		return "no_tcpdump";
	if (access(CT_TASKSET, X_OK))
		return "no_taskset";

	return NULL;
}

static int ct_workdir_make(void)
{
	int err = ct_root_make();

	if (err)
		return err;

	snprintf(ct.dir, sizeof(ct.dir), "%s/%s", ct_root, ct.id);
	if (mkdir(ct.dir, 0700) < 0)
		return -errno;

	ct.have_dir = true;

	return 0;
}

static int ct_config_file_write(void)
{
	char path[CT_FILE_MAX];
	char *json;
	int err;

	if (!uv.cfg)
		return 0;

	blob_buf_init(&cb, 0);
	cfg_dump(uv.cfg, &cb);
	json = blobmsg_format_json(cb.head, true);
	if (!json)
		return -ENOMEM;

	ct_path(path, "config.json");
	err = ct_file_write(path, json, strlen(json));
	free(json);

	return err;
}

/*
 * Everything the capture needs, in the order the archive needs it: the
 * configuration the trace runs on, the rings, the ring mask over every domain
 * so the backend debug bits go on with it, the SIP tap, the export file, and
 * the wire child.
 */
static int ct_capture_start(void)
{
	char path[CT_FILE_MAX];
	int err;

	/* The media ring takes the plaintext media datagrams the backend
	 * drains, so it is open for the length of the trace. The canceller
	 * ring opens only for a trace that asked for it. */
	err = trace_ring_open(true, ct.opt.lec);
	if (err)
		return err;

	trace_ring_set((1u << TRACE_DOMAIN_MAX) - 1);
	ct_tap_set(ct_wire_tap);

	ct_path(path, "rings.pcapng");
	err = trace_ring_export_open(path);
	if (err) {
		ct_tap_set(NULL);
		trace_ring_set(0);
		trace_ring_close();
		return err;
	}
	ct.export_open = true;

	ct_cpumask_make();
	ct.wire.enabled = ct.opt.wire;
	/*
	 * A capture that will not start costs the trace its packets and
	 * nothing else: the rings, the snapshots and the records are what
	 * most questions are answered from, so the arm stands, the reply says
	 * the capture is off, and the word and the note say why.
	 */
	if (ct.wire.enabled && !ct_wire_start()) {
		ct.wire.enabled = false;
		ct.wire.failed = true;
		snprintf(ct.last_error, sizeof(ct.last_error), "wire");
	}

	return 0;
}

/*
 * Everything an arm can refuse. It runs before anything of the trace before
 * this one is touched, so a caller that is refused still reads that trace's
 * id, options and counts from the reply. The options it validates go into
 * want and reach the controller only if every check passed.
 */
static int ct_preflight(unsigned int line, const struct calltrace_options *o,
			struct calltrace_options *want)
{
	const char *tool;

	if (ct.state == CT_ARMED || ct.state == CT_RUNNING ||
	    ct.state == CT_FINISHING || ct_children_alive())
		return ct_refuse("busy", -EBUSY);
	if (!ct_line_ok(line))
		return ct_refuse("bad_line", -EINVAL);

	ct_options_take(o, want);
	if (!ct_range_ok(want))
		return ct_refuse("bad_range", -EINVAL);

	tool = ct_tool_missing(want);
	if (tool)
		return ct_refuse(tool, -ENOENT);

	if (!ct_space_ok(want))
		return ct_refuse("no_space", -ENOSPC);

	return 0;
}

/*
 * The work dir and the two files a trace needs before it captures anything:
 * the configuration it runs on and the first manifest. Each failure is the
 * dir refusing to hold the trace, so each answers the one word, and each
 * leaves the dir removed and nothing of the trace behind.
 */
static int ct_workdir_open(void)
{
	int err = ct_workdir_make();

	if (err)
		return err;

	err = ct_config_file_write();
	if (!err) {
		ct.stage = "armed";
		err = ct_manifest_publish();
	}

	if (err) {
		ct_dir_remove(ct.dir);
		ct.have_dir = false;
	}

	return err;
}

/*
 * The acquisition, and the one place this controller cannot go back from.
 * Before it, every failure is a refusal and the trace before this one still
 * stands in the reply. After it, that record is gone: the work dir of the
 * trace before goes, the id, the dir and the two files are this trace's, and
 * a failure from here leaves no trace at all rather than a mix of the two.
 */
int calltrace_arm(unsigned int line, const struct calltrace_options *o)
{
	struct calltrace_options want;
	int err;

	if (!o)
		return -EINVAL;

	ct_cadences_load();
	ct_timers_init();
	ct.last_error[0] = '\0';

	err = ct_preflight(line, o, &want);
	if (err)
		return err;

	/* The space check counted these bytes as free. */
	if (ct_dir_reclaimable())
		ct_dir_remove(ct.dir);

	ct_trace_clear();
	ct_state_set(CT_IDLE);
	ct.reason[0] = '\0';
	ct.opt = want;
	ct.line = line;
	ct_id_make(line);

	err = ct_workdir_open();
	if (err) {
		ct.have_id = false;
		return ct_refuse("workdir", err);
	}

	err = ct_capture_start();
	if (err) {
		ct_dir_remove(ct.dir);
		ct.have_id = false;
		ct.have_dir = false;
		return ct_refuse(err == -ENOSYS ? "no_udebug" : "rings", err);
	}

	/* After ct_capture_start(), so the push this makes arms the backend's
	 * packet trace against rings that are already open and the first drain
	 * finds the media ring there. */
	ct_state_set(CT_ARMED);
	ct.armed_at = ct_wall_s();
	ct.rest = line_rest_of(line);
	ct.waiting_call = ct.rest == LINE_REST_IDLE;
	ct_stage_set("armed");

	uloop_timeout_set(&ct.t_arm, ct_timer_ms(ct.opt.arm_timeout_s));
	uloop_timeout_set(&ct.t_export, (int)ct_export_period());
	uloop_timeout_set(&ct.t_wire, (int)ct_wire_poll_ms);
	ULOG_INFO("call trace %s armed on line %u\n", ct.id, line);

	return 0;
}

int calltrace_cancel(void)
{
	if (ct.state == CT_ARMED) {
		ct_teardown("cancelled");
		return 0;
	}

	if (ct.state == CT_RUNNING) {
		ct_freeze("cancelled");
		return 0;
	}

	return -ENOENT;
}

/* -------------------------------------------------------- the notifications */

/*
 * The rest of the line after the move comes from the line flags and the oos
 * bit (line_rest_of), which the protocol updated before it named the move;
 * the rest before it is the one this trace kept from the last move.
 */
void calltrace_line_state(unsigned int line, const char *before,
			  const char *after)
{
	enum line_rest was, now;

	if (ct.state != CT_ARMED && ct.state != CT_RUNNING)
		return;
	if (line != ct.line)
		return;

	was = ct.rest;
	now = line_rest_of(line);
	ct.rest = now;

	if (ct.state == CT_ARMED)
		ct_armed_move(was, now);
	else
		ct_running_move(was, now);

	ct_fsm_reason(before, after);
}

/*
 * A leg of the traced line opened its record. An inbound call opens one
 * before the machine moves, so the arm takes sequences too: a record the
 * window would otherwise miss is the record of the call it was armed for.
 */
void calltrace_call_started(unsigned int line, uint32_t cdr_seq)
{
	if (ct.state != CT_ARMED && ct.state != CT_RUNNING)
		return;
	if (line != ct.line || !cdr_seq || ct_seq_known(cdr_seq))
		return;

	/* The record ring keeps the newest of what it holds, so a window that
	 * outran this list keeps the newest too: the oldest leaves. */
	if (ct.cdr_seqs == CT_SEQ_MAX) {
		memmove(ct.cdr_seq, ct.cdr_seq + 1,
			sizeof(ct.cdr_seq) - sizeof(ct.cdr_seq[0]));
		ct.cdr_seqs--;
	}

	ct.cdr_seq[ct.cdr_seqs++] = cdr_seq;
}

void calltrace_event(unsigned int line, const char *reason)
{
	if (ct.state != CT_RUNNING || line != ct.line)
		return;
	if (!reason || !*reason)
		return;

	ct_reason_add(reason);
}

void calltrace_config_applied(void)
{
	if (ct.state != CT_RUNNING)
		return;

	ct_reason_add("config");
}

bool calltrace_capturing(void)
{
	return ct.state == CT_ARMED || ct.state == CT_RUNNING;
}

bool calltrace_lec_capturing(void)
{
	return ct.state == CT_RUNNING && ct.opt.lec && !ct.lec_truncated;
}

/* --------------------------------------------------------------- the reply */

static void ct_wire_status_dump(struct blob_buf *b)
{
	void *tbl = blobmsg_open_table(b, "wire");

	blobmsg_add_u8(b, "enabled", ct.wire.enabled);
	blobmsg_add_u8(b, "ready", ct.wire.ready);
	blobmsg_add_u8(b, "running", ct.wire.child.running);
	blobmsg_add_u64(b, "bytes", ct.wire.bytes);
	blobmsg_add_u8(b, "truncated", ct.wire.truncated);
	blobmsg_add_u32(b, "restarts", ct.wire.restarts);
	if (ct.wire.child.exit_valid)
		blobmsg_add_u32(b, "exit", (uint32_t)ct.wire.child.exit);
	blobmsg_close_table(b, tbl);
}

static void ct_rings_status_dump(struct blob_buf *b)
{
	void *tbl = blobmsg_open_table(b, "rings");

	blobmsg_add_u8(b, "open", trace_ring_is_open());
	blobmsg_add_u8(b, "registered", trace_ring_registered());
	ct_ring_rows_dump(b);
	blobmsg_add_u64(b, "relay_dropped", trace_relay_dropped());
	blobmsg_add_u64(b, "file_bytes", ct_export_bytes());
	blobmsg_add_u8(b, "truncated", ct.rings_truncated);
	blobmsg_close_table(b, tbl);
}

static void ct_instants_dump(struct blob_buf *b)
{
	if (ct.armed_at)
		blobmsg_add_u64(b, "armed_at", ct.armed_at);
	if (ct.capture_ready_at)
		blobmsg_add_u64(b, "capture_ready_at", ct.capture_ready_at);
	if (ct.started_at)
		blobmsg_add_u64(b, "started_at", ct.started_at);
	if (ct.ended_at)
		blobmsg_add_u64(b, "ended_at", ct.ended_at);
}

void calltrace_status_dump(struct blob_buf *b)
{
	bool terminal = ct.state == CT_DONE || ct.state == CT_FAILED;

	blobmsg_add_string(b, "state", ct_state_name(ct.state));
	if (ct.have_id) {
		blobmsg_add_string(b, "id", ct.id);
		blobmsg_add_u32(b, "line", ct.line);
		ct_options_dump(b);
	}

	if (ct.state == CT_ARMED)
		blobmsg_add_string(b, "waiting_for",
				   ct.waiting_call ? "call" : "line_idle");

	ct_instants_dump(b);
	if (ct.state == CT_RUNNING || ct.state == CT_FINISHING)
		blobmsg_add_u64(b, "elapsed_ms",
				ct_mono_ms() - ct.started_mono_ms);
	if (ct.started_at) {
		blobmsg_add_u32(b, "calls", ct.calls);
		blobmsg_add_u32(b, "snapshots", ct.snapshots);
	}

	if (ct.have_id) {
		ct_wire_status_dump(b);
		ct_rings_status_dump(b);
	}
	if (ct.have_dir)
		blobmsg_add_string(b, "workdir", ct.dir);
	if (ct.have_archive)
		blobmsg_add_string(b, "archive", ct.archive);

	if (ct.reason[0] && (ct.state == CT_IDLE || terminal))
		blobmsg_add_string(b, "reason", ct.reason);
	/* The word of the last refusal stands until the next arm clears it,
	 * whatever state the trace that is already running is in: a caller
	 * that was refused reads why from the reply it got. */
	if (ct.last_error[0])
		blobmsg_add_string(b, "last_error", ct.last_error);
	/* Only a failure writes the text, and only the next arm leaves the
	 * failed state and clears it, so the key stands in failed alone. */
	if (ct.failure[0])
		blobmsg_add_string(b, "failure", ct.failure);
	/* The value stays set after the reap, so the state decides: once the
	 * trace is done or failed, no finisher runs under it. */
	if (ct.state == CT_FINISHING && ct.finish_budget_ms)
		blobmsg_add_u32(b, "finish_budget_ms",
				(uint32_t)ct.finish_budget_ms);
}

/* ------------------------------------------------------------- the recovery */

static int ct_json_load(struct blob_buf *b, const char *path)
{
	char *text = ct_file_read(path, CT_JSON_MAX);
	bool ok;

	if (!text)
		return -ENOENT;

	blob_buf_init(b, 0);
	ok = blobmsg_add_json_from_string(b, text);
	free(text);

	return ok ? 0 : -EINVAL;
}

/*
 * Rewrite the stage and the reason of a manifest an earlier daemon left, and
 * keep every other key it carries: the times, the options and the counts of
 * that trace are the evidence the archive is taken for.
 */
static int ct_manifest_patch(const char *dir, const char *stage,
			     const char *reason)
{
	struct blob_attr *cur;
	char path[CT_FILE_MAX];
	char *json;
	size_t rem;
	int err;

	ct_join(path, dir, "manifest.json");
	err = ct_json_load(&xb, path);
	if (err)
		return err;

	blob_buf_init(&cb, 0);
	blobmsg_add_string(&cb, "stage", stage);
	blobmsg_add_string(&cb, "reason", reason);
	blobmsg_for_each_attr(cur, xb.head, rem) {
		const char *name = blobmsg_name(cur);

		if (!name || !strcmp(name, "stage") || !strcmp(name, "reason"))
			continue;

		blobmsg_add_blob(&cb, cur);
	}

	json = blobmsg_format_json(cb.head, true);
	if (!json)
		return -ENOMEM;

	err = ct_json_publish(dir, json);
	free(json);

	return err;
}

/* The reason a recovered manifest already carries, so a stop that wrote one
 * keeps it and a crash that wrote none is named for what it was. */
static bool ct_recover_wanted(const char *dir, unsigned int *pid,
			      bool *stopped)
{
	enum { M_STAGE, M_REASON, M_BOOT, M_PID, __M_MAX };
	static const struct blobmsg_policy pol[__M_MAX] = {
		[M_STAGE] = { "stage", BLOBMSG_TYPE_STRING },
		[M_REASON] = { "reason", BLOBMSG_TYPE_STRING },
		[M_BOOT] = { "boot_id", BLOBMSG_TYPE_STRING },
		[M_PID] = { "daemon_pid", BLOBMSG_TYPE_INT32 },
	};
	struct blob_attr *tb[__M_MAX] = {};
	char path[CT_FILE_MAX];
	const char *stage;

	ct_join(path, dir, "manifest.json");
	if (ct_json_load(&xb, path))
		return false;

	blobmsg_parse(pol, __M_MAX, tb, blob_data(xb.head), blob_len(xb.head));
	if (!tb[M_STAGE])
		return false;

	stage = blobmsg_get_string(tb[M_STAGE]);
	if (!strcmp(stage, "done"))
		return false;

	*pid = tb[M_PID] ? blobmsg_get_u32(tb[M_PID]) : 0;
	ct_boot_id_load();
	if (tb[M_BOOT] && !strcmp(blobmsg_get_string(tb[M_BOOT]), ct_boot_id) &&
	    *pid && !kill((pid_t)*pid, 0))
		return false;

	*stopped = tb[M_REASON] &&
		   !strcmp(blobmsg_get_string(tb[M_REASON]), "daemon_stop");

	return true;
}

static void ct_recover_scan(void)
{
	struct dirent *ent;
	DIR *root;

	root = opendir(ct_root);
	if (!root)
		return;

	while ((ent = readdir(root)) && rc.count < CT_RECOVER_MAX) {
		char name[CT_ID_MAX];
		char dir[CT_DIR_MAX];
		unsigned int pid = 0;
		bool stopped = false;
		size_t len = strlen(ent->d_name);

		/* A name no id can have is no work dir of this daemon. */
		if (ent->d_name[0] == '.' || len >= sizeof(name))
			continue;

		memcpy(name, ent->d_name, len + 1);
		snprintf(dir, sizeof(dir), "%s/%s", ct_root, name);
		if (!ct_recover_wanted(dir, &pid, &stopped))
			continue;

		ct_manifest_patch(dir, "interrupted",
				  stopped ? "daemon_stop" : "recovered");
		memcpy(rc.dir[rc.count], name, len + 1);
		rc.pid[rc.count] = pid;
		rc.count++;
	}
	closedir(root);
}

/* A recovered dir reaches the same two outcomes a live trace does, so a
 * reader of the root cannot tell the two apart by what they left. */
static void ct_recover_done(int code)
{
	uloop_timeout_cancel(&rc.budget);
	ct_child_reaped(&rc.child, code);

	if (code) {
		ct_manifest_patch(rc.cur, "failed", "finisher_failed");
	} else {
		ct_manifest_patch(rc.cur, "done", "recovered");
		ct_retention_sweep(rc.archive);
	}

	ct_recover_next();
}

static void ct_recover_proc_cb(struct uloop_process *p, int ret)
{
	ct_recover_done(WIFEXITED(ret) ? WEXITSTATUS(ret) : -1);
}

static void ct_recover_expire_cb(struct uloop_timeout *t)
{
	ULOG_WARN("call trace recovery: the finisher outran its budget\n");
	ct_child_stop(&rc.child);
}

/* One at a time: a board that lost three traces must not run three tar runs
 * against the engine at once. */
static void ct_recover_next(void)
{
	char *argv[CT_ARGV_MAX];
	char pid[16];
	int budget;

	if (rc.child.running || rc.next >= rc.count)
		return;

	snprintf(rc.cur, sizeof(rc.cur), "%s/%s", ct_root, rc.dir[rc.next]);
	snprintf(rc.archive, sizeof(rc.archive), "%s/%s.tar.gz", ct_root,
		 rc.dir[rc.next]);
	snprintf(pid, sizeof(pid), "%u", rc.pid[rc.next]);
	rc.next++;

	ct_cpumask_make();
	argv[0] = (char *)CT_FINISH_SCRIPT;
	argv[1] = rc.cur;
	argv[2] = rc.archive;
	argv[3] = ct.cpumask;
	argv[4] = pid;
	argv[5] = NULL;

	budget = ct_finish_budget_ms(rc.cur);
	if (!ct_child_start(&rc.child, argv, 5, ct_recover_proc_cb)) {
		ct_recover_next();
		return;
	}

	uloop_timeout_set(&rc.budget, budget);
}

void calltrace_init(void)
{
	ct_cadences_load();
	ct_timers_init();
	ct_boot_id_load();
	ct_recover_scan();
	ct_recover_next();
}

/*
 * The daemon stop. procd gives five seconds, so the finisher is not started
 * here: the export is a bounded file write, the journal records the stage,
 * and the next start runs the finisher on the dir this leaves behind. This is
 * the one path that abandons a child, because the daemon is going with it.
 */
static void ct_shutdown_finishing(void)
{
	uloop_timeout_cancel(&ct.t_fin);
	ct_child_signal(&ct.fin, SIGTERM);
	ct_child_abandon(&ct.fin);
	ct_word_set(ct.reason, "daemon_stop");
	ct_stage_set("interrupted");
}

static void ct_shutdown_capture(void)
{
	ct_timers_cancel();
	ct_capture_stop();
	if (ct.state == CT_RUNNING)
		ct_snapshot("end");
	/* The freeze order, for the freeze reason: the push this makes takes
	 * the backend's packet trace and canceller capture down, and their
	 * last drains must reach the rings that the export below reads. An
	 * armed trace that never ran takes it too, because the backend has
	 * been producing packets since the arm. */
	ct_state_set(CT_FINISHING);
	ct_export_finish();
	trace_ring_close();
	ct.ended_at = ct_wall_s();
	ct_word_set(ct.reason, "daemon_stop");
	ct_stage_set("interrupted");
	ct_child_signal(&ct.wire.child, SIGTERM);
	ct_child_abandon(&ct.wire.child);
	ct_idle_reset("daemon_stop");
}

/* Ask one child to go and stop owning it. procd gives five seconds, and a
 * child that ignores the signal must not spend them. */
static void ct_shutdown_child(struct ct_child *c)
{
	if (!c->running)
		return;

	ct_child_signal(c, SIGTERM);
	ct_child_abandon(c);
}

void calltrace_shutdown(void)
{
	uloop_timeout_cancel(&rc.budget);

	if (ct.state == CT_FINISHING)
		ct_shutdown_finishing();
	else if (ct.state == CT_ARMED || ct.state == CT_RUNNING)
		ct_shutdown_capture();

	/*
	 * Whatever state the trace is in, a child of it is still this
	 * daemon's: a capture of a cancelled arm, a finisher of a trace that
	 * failed, and a recovery run beside either.
	 */
	ct_shutdown_child(&ct.wire.child);
	ct_shutdown_child(&ct.fin);
	ct_shutdown_child(&rc.child);

	blob_buf_free(&cb);
	blob_buf_free(&xb);
}

/* ------------------------------------------------------------ test control */

void calltrace_test_mode(bool on)
{
	ct_timers_cancel();
	uloop_timeout_cancel(&ct.t_fin);
	uloop_timeout_cancel(&ct.wire.child.kill);
	uloop_timeout_cancel(&ct.fin.kill);
	uloop_timeout_cancel(&rc.child.kill);
	uloop_timeout_cancel(&rc.budget);
	ct_tap_set(NULL);
	trace_ring_set(0);
	if (ct.export_open)
		trace_ring_export_close(NULL);
	if (trace_ring_is_open())
		trace_ring_close();

	memset(&ct, 0, sizeof(ct));
	memset(&rc, 0, sizeof(rc));
	ct_space_test = 0;
	ct_finish_mib_test = 0;
	ct_finish_last_ms = 0;
	ct_cpus_test = 0;
	ct_clock_test_ms = 0;
	ct_spawn_fail_test = false;
	ct_test_mode = on;
	ct_timers_init();
	/* The cadences are read once per daemon run, and a reset is a new run
	 * for the suite: one case pins the shipped value and the next pins
	 * what the environment asked for. */
	ct_snapshot_ms = 0;
	ct_cadences_load();
}

unsigned int calltrace_test_cadence_ms(int which)
{
	switch (which) {
	case CALLTRACE_CADENCE_SNAPSHOT:
		return ct_snapshot_ms;
	case CALLTRACE_CADENCE_EXPORT:
		return ct_export_ms;
	case CALLTRACE_CADENCE_WIRE_POLL:
		return ct_wire_poll_ms;
	case CALLTRACE_CADENCE_KILL:
		return ct_kill_ms;
	case CALLTRACE_CADENCE_FINISH:
		return ct_finish_ms;
	case CALLTRACE_CADENCE_EXPORT_LEC:
		return ct_export_lec_ms;
	}

	return 0;
}

void calltrace_test_root(const char *dir)
{
	snprintf(ct_root, sizeof(ct_root), "%s",
		 dir && *dir ? dir : CT_ROOT_DEFAULT);
}

void calltrace_test_space(uint64_t free_bytes)
{
	ct_space_test = free_bytes;
}

void calltrace_test_finish_mib_ms(unsigned int ms)
{
	ct_finish_mib_test = ms;
}

unsigned int calltrace_test_finish_budget_ms(void)
{
	return (unsigned int)ct_finish_last_ms;
}

void calltrace_test_cpus(long cpus)
{
	ct_cpus_test = cpus;
}

void calltrace_test_spawn_fail(bool on)
{
	ct_spawn_fail_test = on;
}

void calltrace_test_clock_shift(int64_t ms)
{
	ct_clock_test_ms += ms;
}

static struct ct_child *ct_test_child(int child)
{
	if (child == CALLTRACE_CHILD_WIRE)
		return &ct.wire.child;

	return ct.fin.argc ? &ct.fin : &rc.child;
}

const char *calltrace_test_argv(int child, unsigned int i)
{
	struct ct_child *c = ct_test_child(child);

	if (i >= CT_ARGV_MAX)
		return NULL;

	return i < c->argc ? c->argv[i] : NULL;
}

const char *calltrace_test_signals(int child)
{
	return ct_test_child(child)->signals;
}

void calltrace_test_child_done(int child, int code)
{
	if (child == CALLTRACE_CHILD_WIRE) {
		if (ct.wire.child.running)
			ct_wire_reaped(code);
		return;
	}

	if (ct.fin.running) {
		ct_child_reaped(&ct.fin, code);
		ct_finish_done(code);
		return;
	}

	if (rc.child.running)
		ct_recover_done(code);
}
