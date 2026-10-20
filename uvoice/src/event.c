// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The voip registration events. See event.h for the contract.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libubox/uloop.h>

#include "cfg/cfg.h"
#include "event.h"
#include "proto/shared/tls_paths.h"
#include "spawn_run.h"
#include "trace.h"

#define EVENT_SCRIPT	"/usr/srg/scripts/event-post.sh"
#define EVENT_TLS_HELPER	"/usr/sbin/uvoice_tls_cert.sh"
#define EVENT_TLS_RETRY_MS	30000
#define EVENT_PATH_MAX	128
#define EVENT_REC_MAX	8
#define EVENT_TLS_USER_MAX	64
#define EVENT_TLS_PASS_MAX	64

/* a record slot holds the WIDEST argument, which is the script path */
#define EVENT_REC_ARG_MAX	EVENT_PATH_MAX

/* script, -p, prio, -t, topic, json: the whole vector but the NULL */
#define EVENT_ARGC		6
#define EVENT_TLS_ARGC		5

_Static_assert(EVENT_ARGV_MAX > EVENT_ARGC, "argv too small for the vector and its NULL");
_Static_assert(EVENT_ARGV_MAX > EVENT_TLS_ARGC,
	       "argv too small for the TLS vector and its NULL");
_Static_assert(EVENT_DATA_MAX >= EVENT_PROXY_MAX + 13, "EventData cannot hold a full proxy");
_Static_assert(EVENT_REC_ARG_MAX >= EVENT_DATA_MAX, "a record slot cannot hold the EventData");
_Static_assert(EVENT_PATH_MAX >= CFG_TLS_PATH_MAX, "event path cannot hold tls_cert_path");
_Static_assert(EVENT_TLS_USER_MAX >= CFG_STR_MAX, "event user cannot hold tls_cert_username");
_Static_assert(EVENT_TLS_PASS_MAX >= CFG_STR_MAX, "event pass cannot hold tls_cert_password");

enum event_reg {
	EVENT_REG_UNKNOWN = 0,
	EVENT_REG_UP,
	EVENT_REG_DOWN,
};

enum event_tls_child {
	EVENT_TLS_CHILD_NONE = 0,
	EVENT_TLS_CHILD_FETCH,
	EVENT_TLS_CHILD_CLEAR,
};

/*
 * reported is the state the last post carried, and it is the dedup basis.
 * pend_state is the state a change asked for while that post was in flight.
 * It is one slot, and every further change overwrites it, so a line that
 * flaps during one spawn posts its final state once instead of every step of
 * the flap.
 */
struct event_line {
	struct uloop_process proc;
	bool busy;
	bool pend;
	enum event_reg reported;
	enum event_reg pend_state;
	char pend_proxy[EVENT_PROXY_MAX];
};

struct event_rec {
	unsigned int line;
	unsigned int argc;
	bool terminated;
	char arg[EVENT_ARGV_MAX][EVENT_REC_ARG_MAX];
};

static struct event_line ev_line[EVENT_LINE_MAX];

static struct {
	/* a copy, never the getenv() pointer: the environment can move under a
	 * later setenv() and this string outlives the lookup */
	char script[EVENT_PATH_MAX];
	bool inited;
	bool available;

	/* test mode: no spawn, the argv lands in rec and the fixture reaps */
	bool test_mode;
	unsigned int posts;
	unsigned int refused;
	unsigned int suppressed;
	unsigned int collapsed;
	unsigned int recs;
	struct event_rec rec[EVENT_REC_MAX];
} ev;

static struct {
	struct uloop_process proc;
	struct uloop_timeout retry;
	char helper[EVENT_PATH_MAX];
	char url[EVENT_PATH_MAX];
	char username[EVENT_TLS_USER_MAX];
	char password[EVENT_TLS_PASS_MAX];
	void (*done)(void);
	bool verify;
	bool armed;
	bool busy;
	bool restart;
	bool clear_pending;
	bool retry_pending;
	enum event_tls_child child;
	unsigned int starts;
	unsigned int refused;
	unsigned int argc;
	bool terminated;
	char arg[EVENT_ARGV_MAX][EVENT_REC_ARG_MAX];
} tls_cert;

/*
 * A directory answers X_OK, so access() alone would arm the gate on a path
 * that can never be exec'd. The regular file check is what makes the gate mean
 * "a script this daemon can run".
 */
static bool event_script_ok(const char *path)
{
	struct stat st;

	if (stat(path, &st) || !S_ISREG(st.st_mode))
		return false;

	return access(path, X_OK) == 0;
}

static void event_gate_eval(void)
{
	const char *path = getenv("UVOICE_EVENT_POST");

	snprintf(ev.script, sizeof(ev.script), "%s", (path && *path) ? path : EVENT_SCRIPT);
	ev.inited = true;
	ev.available = event_script_ok(ev.script);

	if (!ev.available)
		TRACE(TRACE_FSM, "event: %s absent, voip registration events off",
		      ev.script);
}

void event_init(void)
{
	event_gate_eval();
}

bool event_available(void)
{
	if (!ev.inited)
		event_gate_eval();

	return ev.available;
}

/*
 * A host name, an IPv4 literal or a bracketed IPv6 address with a scope, and
 * nothing else. The value lands inside a JSON string that a shell script
 * forwards to jq, so any other byte would either break the document or smuggle
 * a second field. The whole field is dropped rather than escaped: the state
 * change is the event, the address is decoration, and an escaped address that
 * no consumer can match is worth less than an absent one. An over-long value
 * is refused for the same reason, because a truncated address is a wrong one.
 */
static bool event_proxy_clean(const char *s)
{
	size_t i;

	if (!s || !*s)
		return false;

	for (i = 0; s[i]; i++) {
		char c = s[i];

		if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
		    (c >= 'A' && c <= 'Z') || c == '.' || c == ':' || c == '-' ||
		    c == '_' || c == '[' || c == ']' || c == '%')
			continue;

		return false;
	}

	return i < EVENT_PROXY_MAX;
}

/*
 * Validate first, then copy. Otherwise a value refused for its length would be
 * stored truncated, pass the check on the way out and publish an address that
 * is not the registrar.
 */
static void event_proxy_take(char *out, size_t olen, const char *proxy)
{
	if (!event_proxy_clean(proxy)) {
		out[0] = 0;
		return;
	}

	snprintf(out, olen, "%s", proxy);
}

/*
 * The count bounds the copy, never the terminator. A walk to the NULL would
 * read off the end of a vector that lost it, and turn a broken emitter into a
 * crash instead of a named failure. Whether the NULL the spawn needs is
 * present is recorded beside the arguments, as data.
 */
static void event_rec_add(unsigned int line, char *const argv[], unsigned int argc)
{
	struct event_rec *r;
	unsigned int i;

	if (ev.recs >= EVENT_REC_MAX || argc >= EVENT_ARGV_MAX)
		return;

	r = &ev.rec[ev.recs++];
	r->line = line;
	r->argc = argc;
	r->terminated = argv[argc] == NULL;

	for (i = 0; i < argc; i++)
		snprintf(r->arg[i], sizeof(r->arg[i]), "%s", argv[i]);
}

/*
 * The argument list is built from a fixed vocabulary and one validated host
 * string, so it goes to the spawn directly and never through a shell.
 */
static pid_t event_spawn(char *const argv[])
{
	pid_t pid = spawn_run(argv, SPAWN_GROUP_DAEMON);

	if (pid < 0)
		ULOG_ERR("event helper spawn failed: %s\n", strerror(errno));

	return pid;
}

/* The helper runs in a group of its own, so a stop kills its curl too. */
static pid_t event_tls_spawn(char *const argv[])
{
	pid_t pid = spawn_run(argv, SPAWN_GROUP_OWN);

	if (pid < 0)
		ULOG_ERR("TLS certificate helper spawn failed: %s\n", strerror(errno));

	return pid;
}

static bool event_tls_bundle_ok(void)
{
	char cert_path[EVENT_PATH_MAX];
	char key_path[EVENT_PATH_MAX];
	const char *dir = getenv("UVOICE_TLS_DIR");
	struct stat cert;
	struct stat key;

	if (!dir || !*dir)
		dir = UVOICE_TLS_DIR_DEFAULT;

	if (snprintf(cert_path, sizeof(cert_path), "%s/cert.pem", dir) >=
	    (int)sizeof(cert_path) ||
	    snprintf(key_path, sizeof(key_path), "%s/key.pem", dir) >=
	    (int)sizeof(key_path))
		return false;

	return !stat(cert_path, &cert) && S_ISREG(cert.st_mode) &&
	       !stat(key_path, &key) && S_ISREG(key.st_mode);
}

static void event_tls_retry_arm(void);
static void event_tls_run(void);
static void event_tls_clear_run(void);

static void event_tls_retry_cb(struct uloop_timeout *t)
{
	tls_cert.retry_pending = false;
	event_tls_run();
}

static void event_tls_retry_arm(void)
{
	if (!tls_cert.armed || event_tls_bundle_ok())
		return;

	tls_cert.retry_pending = true;
	if (ev.test_mode)
		return;

	tls_cert.retry.cb = event_tls_retry_cb;
	uloop_timeout_set(&tls_cert.retry, EVENT_TLS_RETRY_MS);
}

static void event_tls_complete(int code)
{
	void (*done)(void) = tls_cert.done;
	bool replaced = tls_cert.restart;
	enum event_tls_child child = tls_cert.child;

	tls_cert.busy = false;
	tls_cert.restart = false;
	tls_cert.child = EVENT_TLS_CHILD_NONE;

	if (tls_cert.clear_pending) {
		tls_cert.clear_pending = false;
		event_tls_clear_run();
		return;
	}

	if (child == EVENT_TLS_CHILD_CLEAR) {
		if (tls_cert.armed)
			event_tls_run();
		return;
	}

	if (!tls_cert.armed)
		return;

	if (replaced) {
		event_tls_run();
		return;
	}

	/* Exit 0 installed a bundle. Exit 3 removed an invalid bundle. */
	if ((code == 0 || code == 3) && done)
		done();

	if (code == 4)
		return;

	event_tls_retry_arm();
}

static void event_tls_proc_cb(struct uloop_process *p, int ret)
{
	event_tls_complete(WIFEXITED(ret) ? WEXITSTATUS(ret) : -1);
}

static void event_tls_record(char *const argv[], unsigned int argc)
{
	unsigned int i;

	tls_cert.argc = argc;
	tls_cert.terminated = argv[argc] == NULL;
	for (i = 0; i < argc; i++)
		snprintf(tls_cert.arg[i], sizeof(tls_cert.arg[i]), "%s", argv[i]);
}

static bool event_tls_process_start(char *const argv[], unsigned int argc,
				    enum event_tls_child child)
{
	pid_t pid;

	event_tls_record(argv, argc);
	tls_cert.starts++;
	tls_cert.child = child;

	if (ev.test_mode) {
		tls_cert.busy = true;
		return true;
	}

	pid = event_tls_spawn(argv);
	if (pid < 0) {
		tls_cert.child = EVENT_TLS_CHILD_NONE;
		return false;
	}

	tls_cert.proc.pid = pid;
	tls_cert.proc.cb = event_tls_proc_cb;
	if (uloop_process_add(&tls_cert.proc)) {
		ULOG_ERR("TLS certificate helper reap registration failed\n");
		kill(-pid, SIGTERM);
		waitpid(pid, NULL, 0);
		tls_cert.child = EVENT_TLS_CHILD_NONE;
		return false;
	}

	tls_cert.busy = true;
	return true;
}

static bool event_tls_helper_take(void)
{
	const char *path = getenv("UVOICE_TLS_CERT");

	snprintf(tls_cert.helper, sizeof(tls_cert.helper), "%s",
		 (path && *path) ? path : EVENT_TLS_HELPER);
	if (ev.test_mode || event_script_ok(tls_cert.helper))
		return true;

	tls_cert.refused++;
	TRACE(TRACE_FSM, "event: %s absent, TLS certificate helper refused",
	      tls_cert.helper);
	return false;
}

static void event_tls_run(void)
{
	char verify[2] = { tls_cert.verify ? '1' : '0', 0 };
	char *argv[EVENT_ARGV_MAX];

	if (!tls_cert.armed || tls_cert.busy)
		return;
	tls_cert.retry_pending = false;

	if (!event_tls_helper_take()) {
		event_tls_retry_arm();
		return;
	}

	argv[0] = tls_cert.helper;
	argv[1] = tls_cert.url;
	argv[2] = tls_cert.username;
	argv[3] = tls_cert.password;
	argv[4] = verify;
	argv[EVENT_TLS_ARGC] = NULL;

	if (!event_tls_process_start(argv, EVENT_TLS_ARGC, EVENT_TLS_CHILD_FETCH))
		event_tls_retry_arm();
}

static void event_tls_clear_run(void)
{
	char *argv[EVENT_ARGV_MAX];

	if (tls_cert.busy)
		return;
	if (!event_tls_helper_take())
		return;

	argv[0] = tls_cert.helper;
	argv[1] = (char *)"clear";
	argv[2] = NULL;
	event_tls_process_start(argv, 2, EVENT_TLS_CHILD_CLEAR);
}

void event_tls_cert_start(const char *url, const char *username, const char *password,
			  bool verify, void (*done)(void))
{
	bool changed = !tls_cert.armed || strcmp(tls_cert.url, url ? url : "") ||
		       strcmp(tls_cert.username, username ? username : "") ||
		       strcmp(tls_cert.password, password ? password : "") ||
		       tls_cert.verify != verify || tls_cert.done != done;

	tls_cert.armed = true;
	if (!changed)
		return;

	snprintf(tls_cert.url, sizeof(tls_cert.url), "%s", url ? url : "");
	snprintf(tls_cert.username, sizeof(tls_cert.username), "%s",
		 username ? username : "");
	snprintf(tls_cert.password, sizeof(tls_cert.password), "%s",
		 password ? password : "");
	tls_cert.verify = verify;
	tls_cert.done = done;
	tls_cert.retry_pending = false;
	if (!ev.test_mode)
		uloop_timeout_cancel(&tls_cert.retry);

	if (tls_cert.busy) {
		tls_cert.restart = true;
		return;
	}

	event_tls_run();
}

void event_tls_cert_stop(enum event_tls_stop_mode mode)
{
	tls_cert.armed = false;
	tls_cert.restart = false;
	tls_cert.clear_pending = false;
	tls_cert.retry_pending = false;
	if (!ev.test_mode)
		uloop_timeout_cancel(&tls_cert.retry);

	if (mode == EVENT_TLS_STOP_CONFIG && tls_cert.busy &&
	    tls_cert.child != EVENT_TLS_CHILD_CLEAR)
		tls_cert.clear_pending = true;
	if (tls_cert.busy) {
		if (mode == EVENT_TLS_STOP_CONFIG &&
		    tls_cert.child == EVENT_TLS_CHILD_CLEAR)
			return;
		if (!ev.test_mode)
			kill(-tls_cert.proc.pid, SIGTERM);
		return;
	}
	if (mode == EVENT_TLS_STOP_CONFIG)
		event_tls_clear_run();
}

static void event_proc_cb(struct uloop_process *p, int ret);

static void event_fire(struct event_line *l, enum event_reg st, const char *proxy)
{
	char topic[EVENT_TOPIC_MAX];
	char data[EVENT_DATA_MAX];
	char *argv[EVENT_ARGV_MAX];
	unsigned int line = (unsigned int)(l - ev_line);
	bool up = st == EVENT_REG_UP;
	pid_t pid;

	/* line + 1: the topic names the lines from 1 (event.h) */
	snprintf(topic, sizeof(topic), "voip/nodeMAC/line-%u/reg/%s", line + 1,
		 up ? "successful" : "failed");

	if (up && event_proxy_clean(proxy)) {
		snprintf(data, sizeof(data), "{\"Proxy\":\"%s\"}", proxy);
	} else {
		snprintf(data, sizeof(data), "{}");
		if (up)
			TRACE(TRACE_FSM, "event: line %u registrar unusable, Proxy omitted",
			      line + 1);
	}

	argv[0] = ev.script;
	argv[1] = (char *)"-p";
	argv[2] = (char *)(up ? "info" : "crit");
	argv[3] = (char *)"-t";
	argv[4] = topic;
	argv[5] = data;
	argv[EVENT_ARGC] = NULL;

	TRACE(TRACE_FSM, "event: %s -p %s -t %s %s", argv[0], argv[2], argv[4], argv[5]);

	if (!ev.test_mode) {
		pid = event_spawn(argv);

		/*
		 * reported stays where it was. The same state therefore posts
		 * again on the next edge; it is not suppressed as a duplicate
		 * of a post that never ran.
		 */
		if (pid < 0)
			return;

		l->proc.pid = pid;
		l->proc.cb = event_proc_cb;
		uloop_process_add(&l->proc);
	}

	l->busy = true;
	l->reported = st;
	ev.posts++;
	event_rec_add(line, argv, EVENT_ARGC);
}

/*
 * The exit status is deliberately not read. event-post.sh throttles, queues
 * and backgrounds on its own, and a non-zero status would say nothing this
 * layer could act on; treating it as a failure and re-posting would loop.
 */
static void event_reap(struct event_line *l)
{
	l->busy = false;

	if (!l->pend)
		return;

	l->pend = false;

	if (l->pend_state == l->reported) {
		ev.collapsed++;
		return;
	}

	event_fire(l, l->pend_state, l->pend_proxy);
}

static void event_proc_cb(struct uloop_process *p, int ret)
{
	event_reap(container_of(p, struct event_line, proc));
}

void event_reg_post(unsigned int line, bool up, const char *proxy)
{
	enum event_reg want = up ? EVENT_REG_UP : EVENT_REG_DOWN;
	struct event_line *l;

	if (!ev.inited)
		event_gate_eval();

	if (line >= EVENT_LINE_MAX) {
		ev.refused++;
		TRACE(TRACE_FSM, "event: line %u out of range, reg event dropped", line);
		return;
	}

	if (!ev.available) {
		ev.refused++;
		return;
	}

	l = &ev_line[line];

	/* what this line is on its way to, or what it last reported */
	if (want == (l->pend ? l->pend_state : l->reported)) {
		ev.suppressed++;
		return;
	}

	if (l->busy) {
		l->pend = true;
		l->pend_state = want;
		event_proxy_take(l->pend_proxy, sizeof(l->pend_proxy), proxy);
		return;
	}

	event_fire(l, want, proxy);
}

void event_done(void)
{
	unsigned int i;

	event_tls_cert_stop(EVENT_TLS_STOP_SHUTDOWN);
	if (tls_cert.busy) {
		tls_cert.busy = false;
		if (!ev.test_mode) {
			uloop_process_delete(&tls_cert.proc);
			waitpid(tls_cert.proc.pid, NULL, 0);
		}
	}

	for (i = 0; i < EVENT_LINE_MAX; i++) {
		struct event_line *l = &ev_line[i];

		l->pend = false;
		if (!l->busy)
			continue;

		l->busy = false;
		if (ev.test_mode)
			continue;

		uloop_process_delete(&l->proc);
		waitpid(l->proc.pid, NULL, 0);
	}
}

void event_test_mode(bool on)
{
	memset(ev_line, 0, sizeof(ev_line));
	memset(&ev, 0, sizeof(ev));
	memset(&tls_cert, 0, sizeof(tls_cert));

	snprintf(ev.script, sizeof(ev.script), "%s", EVENT_SCRIPT);
	ev.test_mode = on;
	ev.inited = on;
	ev.available = on;
}

void event_test_reap(unsigned int line, int code)
{
	if (line >= EVENT_LINE_MAX || !ev_line[line].busy)
		return;

	event_reap(&ev_line[line]);
}

unsigned int event_test_posts(void)
{
	return ev.posts;
}

unsigned int event_test_refused(void)
{
	return ev.refused;
}

unsigned int event_test_suppressed(void)
{
	return ev.suppressed;
}

unsigned int event_test_collapsed(void)
{
	return ev.collapsed;
}

unsigned int event_test_recs(void)
{
	return ev.recs;
}

unsigned int event_test_rec_line(unsigned int rec)
{
	return rec < ev.recs ? ev.rec[rec].line : 0;
}

unsigned int event_test_argc(unsigned int rec)
{
	return rec < ev.recs ? ev.rec[rec].argc : 0;
}

const char *event_test_arg(unsigned int rec, unsigned int idx)
{
	if (rec >= ev.recs || idx >= ev.rec[rec].argc)
		return NULL;

	return ev.rec[rec].arg[idx];
}

bool event_test_terminated(unsigned int rec)
{
	return rec < ev.recs && ev.rec[rec].terminated;
}

bool event_test_busy(unsigned int line)
{
	return line < EVENT_LINE_MAX && ev_line[line].busy;
}

void event_test_tls_reap(int code)
{
	if (!tls_cert.busy)
		return;

	event_tls_complete(code);
}

void event_test_tls_retry(void)
{
	if (!tls_cert.retry_pending)
		return;

	tls_cert.retry_pending = false;
	event_tls_run();
}

unsigned int event_test_tls_starts(void)
{
	return tls_cert.starts;
}

unsigned int event_test_tls_refused(void)
{
	return tls_cert.refused;
}

const char *event_test_tls_arg(unsigned int idx)
{
	return idx < tls_cert.argc ? tls_cert.arg[idx] : NULL;
}

bool event_test_tls_terminated(void)
{
	return tls_cert.terminated;
}

bool event_test_tls_busy(void)
{
	return tls_cert.busy;
}

bool event_test_tls_armed(void)
{
	return tls_cert.armed;
}

bool event_test_tls_retry_pending(void)
{
	return tls_cert.retry_pending;
}
