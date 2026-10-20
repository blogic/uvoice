// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "calltrace.h"
#include "media_trace.h"
#include "trace_ring.h"
#include "uvoice.h"
#include "event.h"
#include "event_dispatch.h"
#include "line/line.h"
#include "net/fw_reconcile.h"
#include "net/iface.h"
#include "proto/proto.h"

#define SHUTDOWN_TIMEOUT_MS	3000

struct uvoice uv;

static struct hal_job dsp_job;
static struct uloop_timeout dsp_retry_timer;
static struct uloop_timeout shutdown_timer;
static bool dsp_start_retried;
static bool lines_started;
static int exit_code;
/*
 * Which loop is running. The teardown after uloop_run() runs two nested
 * loops of its own, proto_stop_wait() for the graceful stop of the protocol
 * and dsp_stop_wait() inside hal_close(), and the HAL event reader stays
 * registered across both. A dsp_start job still in flight when a SIGTERM
 * ends the primary loop therefore completes inside one of them, and an
 * uloop_end() from there would cut that wait short: the registration would
 * lose its window, or the HAL would close before the dsp stop completed.
 * This flag is what tells the two apart.
 */
static bool stop_under_way;

/* dsp_start_submit, dsp_start_fail and dsp_start_retry form the retry cycle
 * described in hal.h section 3. */
static void dsp_start_fail(enum hal_status status, const char *what);
static void dsp_start_done(struct hal_job *job, enum hal_status status,
			   const struct hal_event *ev);

/*
 * The two answers event_dispatch.c asks the daemon for, see event_dispatch.h.
 * They are thin on purpose: the exit code and the teardown flag stay private to
 * this file, so the dispatch suite substitutes its own recorders and nothing of
 * uvoiced's shutdown rules travels into a test binary.
 */
void dispatch_exit_request(void)
{
	exit_code = EXIT_FAILURE;
	uloop_end();
}

bool dispatch_stop_pending(void)
{
	return stop_under_way;
}

const char *uvoice_hal_state_name(enum uvoice_hal_state state)
{
	switch (state) {
	case UVOICE_HAL_DOWN:
		return "down";
	case UVOICE_HAL_ATTACHED:
		return "attached";
	case UVOICE_HAL_STARTING:
		return "starting";
	case UVOICE_HAL_READY:
		return "ready";
	case UVOICE_HAL_FAILED:
		return "failed";
	}

	return "unknown";
}

static void job_expire(struct uloop_timeout *t)
{
	struct hal_job *job = container_of(t, struct hal_job, timeout);

	list_del(&job->list);
	ULOG_ERR("hal job token %u produced no completion in %u ms\n", job->token,
		 job->budget_ms);
	job->cb(job, HAL_TRANSIENT, NULL);
}

void job_add_budget(struct hal_job *job, unsigned int budget_ms)
{
	list_add_tail(&job->list, &uv.jobs);
	job->budget_ms = budget_ms;
	job->timeout.cb = job_expire;
	uloop_timeout_set(&job->timeout, budget_ms);
}

void job_add(struct hal_job *job)
{
	job_add_budget(job, HAL_JOB_TIMEOUT_MS);
}

struct hal_job *job_take(uint32_t token)
{
	struct hal_job *job;

	list_for_each_entry(job, &uv.jobs, list) {
		if (job->token != token)
			continue;

		uloop_timeout_cancel(&job->timeout);
		list_del(&job->list);
		return job;
	}

	return NULL;
}

static const char *protocol_word(const struct cfg *cfg)
{
	return cfg_protocol_of(cfg) == CFG_PROTO_MGCP ? "mgcp" : "sip";
}

int uvoice_reload(void)
{
	struct cfg *old = uv.cfg;
	struct cfg *cfg = cfg_load();

	if (!cfg) {
		ULOG_ERR("reload failed, keeping the running config\n");
		return -1;
	}

	/* The protocol is chosen once per daemon (APPLY_RESTART_DAEMON): the
	 * reload refuses the move, and the init script restarts the daemon. */
	if (cfg_protocol_of(cfg) != cfg_protocol_of(old)) {
		ULOG_ERR("reload refused: the protocol moves from %s to %s, "
			 "which needs a daemon restart\n", protocol_word(old),
			 protocol_word(cfg));
		cfg_free(cfg);
		return UVOICE_RELOAD_RESTART;
	}

	uv.cfg = cfg;
	uv.config_generation++;
	/* The uci half of the trace ownership rule (trace.h): the list is
	 * rewritten whole, so a domain the new config drops goes off unless an
	 * operator asked for it at runtime. */
	trace_uci_apply(cfg->debug.trace);
	net_iface_iface_set(cfg->local_interface);
	fw_reconcile_cfg_take(cfg);
	/* One ops->reload, before the lines: the protocol takes the new config
	 * and tears down the legs whose RTP range moved. */
	proto_reload(old, cfg);
	/* lines_reload recompiles the dial plans and applies the new view to
	 * every machine at rest. */
	lines_reload(cfg);
	cfg_free(old);

	ULOG_INFO("config reloaded: enabled=%d channels=%u\n", cfg->enabled,
		  cfg->channels);
	/* After the apply, so the snapshot it asks for renders the
	 * configuration the lines and the protocol now run on. */
	calltrace_config_applied();

	return 0;
}

static void event_job_dispatch(const struct hal_event *ev)
{
	struct hal_job *job = job_take(ev->u.job.token);

	if (!job) {
		ULOG_WARN("completion for unknown job token %u\n", ev->u.job.token);
		return;
	}

	job->cb(job, (enum hal_status)ev->u.job.status, ev);
}

static void event_read(struct uloop_fd *ufd, unsigned int events)
{
	struct hal_event ev;
	ssize_t len;

	while (true) {
		len = recv(ufd->fd, &ev, sizeof(ev), MSG_DONTWAIT);
		if (len < 0 && errno == EINTR)
			continue;
		if (len < 0)
			return;
		if (len != sizeof(ev)) {
			ULOG_ERR("short hal event: %zd bytes\n", len);
			continue;
		}

		uv.events_read++;

		/* Completions come from a worker, not the pump: they carry no
		 * seq and never take part in gap accounting (hal.h section 2). */
		if (ev.type == HAL_EVENT_JOB_DONE) {
			event_job_dispatch(&ev);
			continue;
		}

		event_seq_check(&ev);
		event_dispatch(&ev);
	}
}

static void dsp_start_submit(void)
{
	enum hal_status st;

	dsp_job.cb = dsp_start_done;
	st = hal_backend_ops.dsp_start(uv.hal, &dsp_job);
	if (st == HAL_OK) {
		uv.hal_state = UVOICE_HAL_STARTING;
		job_add(&dsp_job);
		return;
	}

	dsp_start_fail(st, "rejected");
}

static void dsp_start_retry(struct uloop_timeout *t)
{
	ULOG_WARN("dsp bring-up retry\n");
	dsp_start_submit();
}

/*
 * Only the DSP half of the bring-up waits here. The country tone table, the
 * hook windows and the boot hook query need started channels; the machines
 * and the leg layer behind them do not, and lines_init() already wired them
 * in main(). This runs on the bring-up completion and when no backend
 * attached at all, where the lines already forced OOS through the no-HAL
 * rule. A terminal bring-up failure never reaches it: that daemon exits for
 * the procd respawn, see dsp_start_fail().
 */
static void lines_start_once(void)
{
	if (lines_started)
		return;

	lines_started = true;
	lines_start(uv.cfg);
}

static void dsp_start_fail(enum hal_status status, const char *what)
{
	/*
	 * The stop that is already running owns the rest, and this sits above
	 * the retry on purpose. A completion that lands in one of the
	 * teardown's nested loops finds the daemon on its way out: a retry
	 * armed from here would submit a bring-up behind the dsp stop the
	 * teardown runs, and an uloop_end() would truncate the wait the
	 * completion landed in. procd reads the stop it asked for, not this
	 * outcome.
	 */
	if (stop_under_way) {
		ULOG_WARN("dsp bring-up %s inside the stop: %s, exit code unchanged\n",
			  what, hal_status_name(status));
		return;
	}

	if (status == HAL_TRANSIENT && !dsp_start_retried) {
		dsp_start_retried = true;
		dsp_retry_timer.cb = dsp_start_retry;
		uloop_timeout_set(&dsp_retry_timer, HAL_RETRY_MS);
		return;
	}

	uv.hal_state = UVOICE_HAL_FAILED;
	ULOG_ERR("dsp bring-up %s: %s\n", what, hal_status_name(status));

	/*
	 * A backend that attached and then failed its bring-up leaves. A
	 * daemon that settled here instead would keep the line registered but
	 * out of service and answer every INVITE 503 until somebody restarted
	 * it by hand; procd respawns it (files/uvoice.init: respawn 3600 5 0,
	 * a 5 s delay and no retry limit). A backend that never attached is
	 * the fused or absent DSP and still stays, idle and reporting over
	 * ubus, see hal_attach(). The DSP half of the lines is deliberately
	 * not started on this path: nothing of the teardown needs it, and
	 * lines_done() guards on the half lines_init() wired.
	 */
	ULOG_ERR("dsp bring-up is terminal, exiting for the procd respawn\n");
	exit_code = EXIT_FAILURE;
	uloop_end();
}

static void dsp_start_done(struct hal_job *job, enum hal_status status,
			   const struct hal_event *ev)
{
	if (status != HAL_OK) {
		dsp_start_fail(status, "failed");
		return;
	}

	uv.hal_state = UVOICE_HAL_READY;
	ULOG_INFO("dsp ready\n");
	/* A media trace domain switched on before the DSP was up had no
	 * backend to tell; this is the first moment there is one. */
	media_trace_sync();
	lines_start_once();
}

static void dsp_stop_done(struct hal_job *job, enum hal_status status,
			  const struct hal_event *ev)
{
	if (status != HAL_OK)
		ULOG_WARN("dsp stop reported %s\n", hal_status_name(status));

	uloop_timeout_cancel(&shutdown_timer);
	uloop_end();
}

static void shutdown_expire(struct uloop_timeout *t)
{
	ULOG_WARN("dsp stop did not complete in %u ms\n", SHUTDOWN_TIMEOUT_MS);
	uloop_end();
}

static int event_transport_open(void)
{
	int fds[2];

	/* CLOEXEC: the firewall helper and the event publisher spawn children
	 * that must not inherit the HAL event channel. */
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) < 0) {
		ULOG_ERR("event socketpair failed: %s\n", strerror(errno));
		return -1;
	}

	uv.event_ufd.fd = fds[0];
	uv.event_ufd.cb = event_read;
	uv.event_wfd = fds[1];

	if (uloop_fd_add(&uv.event_ufd, ULOOP_READ) < 0) {
		ULOG_ERR("event fd registration failed: %s\n", strerror(errno));
		close(fds[0]);
		close(fds[1]);
		uv.event_ufd.fd = -1;
		uv.event_wfd = -1;
		return -1;
	}

	return 0;
}

/*
 * A backend that does not attach is not fatal: the nvmem kill switch and a
 * missing DSP look exactly like this, and "ubus call voice status" is how the
 * field reports it. A broken event transport is fatal and is handled before
 * this runs.
 */
static int hal_attach(void)
{
	uv.hal = hal_backend_ops.open(uv.event_wfd);
	if (!uv.hal) {
		uv.hal_state = UVOICE_HAL_FAILED;
		ULOG_ERR("hal backend %s did not attach\n", hal_backend_ops.name);
		return -1;
	}

	uv.hal_state = UVOICE_HAL_ATTACHED;

	return 0;
}

/*
 * The event reader stays registered here: dsp_stop completes through the same
 * socketpair as every other job, so the loop must keep running until the
 * completion arrives or the timeout fires.
 */
static void dsp_stop_wait(void)
{
	static struct hal_job stop_job;

	stop_job.cb = dsp_stop_done;
	if (hal_backend_ops.dsp_stop(uv.hal, &stop_job) != HAL_OK)
		return;

	job_add(&stop_job);
	shutdown_timer.cb = shutdown_expire;
	uloop_timeout_set(&shutdown_timer, SHUTDOWN_TIMEOUT_MS);

	uloop_cancelled = false;
	uloop_run();
}

static void hal_close(void)
{
	const struct hal_cap *cap;
	unsigned int line;

	if (uv.hal) {
		dsp_stop_wait();

		cap = hal_backend_ops.cap_get(uv.hal);
		for (line = 0; line < cap->lines; line++) {
			hal_backend_ops.line_ring_stop(uv.hal, line);
			hal_backend_ops.line_state_set(uv.hal, line, HAL_LINE_DOWN);
		}

		hal_backend_ops.close(uv.hal);
		uv.hal = NULL;
	}

	if (uv.event_ufd.registered)
		uloop_fd_delete(&uv.event_ufd);

	if (uv.event_ufd.fd >= 0)
		close(uv.event_ufd.fd);
	if (uv.event_wfd >= 0)
		close(uv.event_wfd);
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-S] [-s ubus_socket] [-d domains] [-h]\n"
		"  -S          log to syslog only\n"
		"  -s path     ubus socket path\n"
		"  -d list     comma separated trace domains, or 'all'\n"
		"              (sip, fsm, hook, hal, dialplan, media, jb,\n"
		"              mgcp)\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *ubus_socket = NULL;
	bool syslog_only = false;
	bool hal_attached;
	int opt;
	int i;

	/* The sink choice must precede ulog_open, and the -d parse wants a
	 * working log for unknown-domain warnings, so -S alone is read ahead
	 * of the getopt pass. */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-S"))
			syslog_only = true;
	}
	trace_init("uvoiced", syslog_only);
	/* The first line also runs ulog's lazy syslog setup on this thread,
	 * before the backend's slic thread exists and logs through it. */
	ULOG_INFO("uvoiced %s starting\n", UVOICE_VERSION);
	INIT_LIST_HEAD(&uv.jobs);
	uv.event_ufd.fd = -1;
	uv.event_wfd = -1;

	while ((opt = getopt(argc, argv, "s:d:Sh")) != -1) {
		switch (opt) {
		case 's':
			ubus_socket = optarg;
			break;
		case 'd':
			trace_domains_parse(optarg);
			break;
		case 'S':
			break;
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	/* A closed peer must surface as EPIPE on the socket, not as a signal. */
	signal(SIGPIPE, SIG_IGN);

	uloop_init();
	/* After uloop_init: the udebug connect adds its socket to the loop,
	 * which needs the poll descriptor the init creates. */
	trace_ring_init();
	/* Before the config and the HAL: the hook has to be in place for the
	 * first trace_uci_apply, and its sync arms a uloop timer. */
	media_trace_init();

	uv.cfg = cfg_load();
	if (!uv.cfg) {
		ULOG_ERR("out of memory loading config\n");
		return 1;
	}
	uv.config_generation = 1;

	/* After the -d parse above, which set the runtime mask: the uci list
	 * owns its own mask alone, so a domain named on the command line
	 * survives a config that does not name it (trace.h). */
	trace_uci_apply(uv.cfg->debug.trace);

	if (event_transport_open() != 0) {
		ULOG_ERR("hal event transport unusable, uvoiced exits\n");
		cfg_free(uv.cfg);
		uloop_done();
		trace_ring_done();
		trace_done();
		return 1;
	}

	hal_attached = hal_attach() == 0;
	/* The uci list and the -d option were applied before a backend
	 * existed, so the mask they ask for is pushed now, ahead of the start
	 * job whose init-time debug lines it gates. */
	if (hal_attached)
		media_trace_sync();

	/* After the attach, because the arm preflight reads the backend's cap
	 * for the line range, and because the recovery this runs spawns a
	 * finisher off the engine CPU the config names. */
	calltrace_init();

	/* After the attach, which fixes the backend handle the media ops of the
	 * host table carry, and before lines_init() hands the table to the
	 * protocol. A config that names a protocol this image does not carry
	 * selects the null table: the daemon stays up and every line stays out
	 * of service. */
	proto_host_build();
	proto_select_from_cfg(uv.cfg);

	/*
	 * The call layer is wired before the DSP is even asked to start.
	 * Registration never waits for the DSP, so an INVITE can arrive while
	 * the DSP still comes up; with no leg layer the caller would hear
	 * that the subscriber is busy. The lines come up here instead, and the
	 * half that needs started channels follows in lines_start_once(). It
	 * is also in front of the submission, because a bring-up the backend
	 * refuses on the spot calls that half back inside dsp_start_submit().
	 */
	lines_init(uv.cfg);

	if (hal_attached)
		dsp_start_submit();

	/*
	 * ubus comes up before the protocol: net_iface_init() asks netifd for
	 * the voice interface over this connection, and that answer names the
	 * netdev the first bind has to scope to. The rebind path recovers a
	 * wrong address but not a wrong device, so this order is required.
	 * uvoice_ubus_init() neither fails nor blocks when ubusd is absent; it
	 * arms a retry and the daemon runs on without ubus. The not-started
	 * guard of the protocol refuses a command that arrives before
	 * proto_start().
	 */
	uvoice_ubus_init(ubus_socket);

	/* Signaling runs independently of the HAL: a fused or absent DSP still
	 * registers so the line is reachable and the fault shows over ubus. */
	event_init();
	net_iface_init(uv.cfg);
	fw_reconcile_cfg_take(uv.cfg);
	proto_start(uv.cfg);

	/*
	 * The DSP half runs on the dsp_start completion, see lines_start_once.
	 * A backend that did not attach produces no completion, so it runs here
	 * instead; every line already forced OOS through the no-HAL rule.
	 */
	if (!uv.hal)
		lines_start_once();

	ULOG_INFO("uvoiced %s started, hal %s\n", UVOICE_VERSION,
		  hal_backend_ops.name);
	/*
	 * uloop_run() clears uloop_cancelled on entry, so an exit the bring-up
	 * decided before the loop ran, which is a backend that refuses the
	 * submit on the spot, cannot travel through the uloop_end() of
	 * dsp_start_fail(). Skip the loop instead; the teardown below is the
	 * same one the completion path reaches.
	 */
	if (!exit_code)
		uloop_run();
	stop_under_way = true;
	/* A transient completion in the last moments before the stop leaves a
	 * 50 ms arm behind, and the bring-up it would submit has no place in a
	 * teardown that is about to stop the DSP. */
	uloop_timeout_cancel(&dsp_retry_timer);
	ULOG_INFO("uvoiced stopping\n");

	/*
	 * Detach ubus before the graceful stop, so a netifd address event, a
	 * reload or a register call cannot arrive during the bounded grace window
	 * and cancel an unregister or create a binding the drain will not wait
	 * for. The transport stays up for the Expires 0 answers.
	 */
	/* Before the ubus teardown, so a trace that was running writes its end
	 * snapshot and closes its export while every renderer it reads still
	 * answers. */
	calltrace_shutdown();
	uvoice_ubus_done();
	/* The tracker goes quiet with ubus: no answer and no recovery attempt
	 * reaches the protocol inside its drain. */
	net_iface_quiesce();
	/* Before hal_close: a sweep that fired after the backend was gone
	 * would call an op on a freed handle. */
	media_trace_done();
	proto_stop_wait();
	/* After the protocol drain, which sent the BYE of every call, and
	 * before hal_close: the protocol ends the legs that still wait for
	 * an answer, so their media stops on an open backend, and the forced
	 * OOS entry stops tones and rings through the backend. */
	lines_done();
	hal_close();
	proto_done();
	net_iface_done();
	event_done();
	cfg_free(uv.cfg);
	uloop_done();
	trace_ring_done();
	trace_done();

	return exit_code;
}
