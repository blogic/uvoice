// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the HAL event dispatch (src/event_dispatch.c).
 *
 * Isolation. event_dispatch.c and trace.c are compiled into this binary. The
 * suite defines every other daemon symbol that the dispatch reaches: uv, the
 * backend table and the two answers that event_dispatch.h declares. It also
 * defines recorders for line_hal_event() and lines_hook_resync() of the line
 * shell and for calltrace_event().
 *
 * main.c is not linked, and the dispatch has a translation unit of its own
 * for that purpose. main.c owns the uloop, the config, the ubus connection and
 * the event socketpair. A suite can reach none of them.
 *
 * The backend name is a mutable buffer. The backend gate is
 * hal_backend_udsp() in event_dispatch.c. It gives HAL_EVENT_SLIC_FAULT and
 * HAL_EVENT_LINE records to their arms only under the udsp backend. The gate
 * reads hal_backend_ops.name, so one binary must reach both variants of the
 * gate.
 * The table itself stays const, exactly as hal.h declares it. Only the
 * characters that the name points at change.
 *
 * What the fall-through looks like. A record that the dispatch does not
 * consume reaches the default trace of event_dispatch(). That trace calls
 * nothing that the suite can otherwise see. A silent drop and a trace
 * fall-through would therefore both read as "line_hal_event was not called".
 * The trace capture below makes the fall-through positive evidence.
 *
 * The capture is the warn-capture pattern of test_cfg.c, with the trace
 * domains turned on for the same window. It selects ULOG_STDIO for the length
 * of one case and points stderr at a temporary file. The first count puts
 * both back.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libubox/ulog.h>

#include "calltrace.h"
#include "event_dispatch.h"
#include "hal/hal.h"
#include "line/line.h"
#include "trace.h"
#include "uvoice.h"
#include "test_util.h"

/* ------------------------------------------------------------ the daemon's
 * side of the two hooks, plus everything else event_dispatch.c reaches */

struct uvoice uv;

static char backend_name[16] = "udsp";

const struct hal_ops hal_backend_ops = {
	.name = backend_name,
};

static unsigned int line_events;
static struct hal_event line_last;

void line_hal_event(const struct hal_event *ev)
{
	line_events++;
	line_last = *ev;
}

/*
 * The other line shell entry the dispatch reaches. The real one arms a timer
 * and queries the backend. This recorder counts the asks and keeps the loss
 * count each one was made with. That pins the order: the counter has to move
 * before the shell is asked to read the hardware.
 */
static unsigned int resync_asks;
static uint64_t resync_lost;

void lines_hook_resync(void)
{
	resync_asks++;
	resync_lost = uv.events_lost;
}

/*
 * The call trace notification. The controller is not linked here, so this is
 * the recorder in its place: it counts the calls, keeps the line and the
 * reason word of the last one, and lets a case say that a record kind reached
 * the trace, or that it did not.
 */
static unsigned int calltrace_events;
static unsigned int calltrace_line;
static char calltrace_reason[32];

void calltrace_event(unsigned int line, const char *reason)
{
	calltrace_events++;
	calltrace_line = line;
	snprintf(calltrace_reason, sizeof(calltrace_reason), "%s",
		 reason ? reason : "");
}

static unsigned int exit_requests;
static bool stop_pending;

void dispatch_exit_request(void)
{
	exit_requests++;
}

bool dispatch_stop_pending(void)
{
	return stop_pending;
}

static void backend_name_set(const char *name)
{
	snprintf(backend_name, sizeof(backend_name), "%s", name);
}

static void env_reset(void)
{
	memset(&uv, 0, sizeof(uv));
	memset(&line_last, 0, sizeof(line_last));
	line_events = 0;
	exit_requests = 0;
	stop_pending = false;
	calltrace_events = 0;
	calltrace_line = 0;
	calltrace_reason[0] = '\0';
	resync_asks = 0;
	resync_lost = 0;
	backend_name_set("udsp");
}

/* --------------------------------------------------------- trace capture */

static FILE *trace_file;
static int trace_stderr_fd = -1;

/*
 * The log function is this file's own, so a case can read the priority a
 * message carried and not only its text. libubox renders the text and drops
 * the priority, so a NOTE mistaken for an ERR would read the same. This one
 * writes the priority in front of the message and sends both to the same
 * redirected stderr the trace capture already reads. It writes only while a
 * capture is open, which is where the libubox default sent it under ctest
 * anyway. This is the recorder pattern of test_line.c.
 */
void ulog(int priority, const char *fmt, ...)
{
	va_list ap;

	if (trace_stderr_fd < 0)
		return;

	fprintf(stderr, "prio=%d ", priority);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static void trace_stderr_restore(void)
{
	if (trace_stderr_fd < 0)
		return;

	fflush(stderr);
	ulog_open(ULOG_SYSLOG, LOG_DAEMON, "test_dispatch");
	dup2(trace_stderr_fd, STDERR_FILENO);
	close(trace_stderr_fd);
	trace_stderr_fd = -1;
}

static void trace_capture_end(void)
{
	trace_stderr_restore();
	trace_domain_set(TRACE_HAL, false);
	trace_domain_set(TRACE_HOOK, false);

	if (!trace_file)
		return;

	fclose(trace_file);
	trace_file = NULL;
}

static void trace_capture_begin(void)
{
	trace_capture_end();

	trace_file = tmpfile();
	if (!trace_file)
		return;

	fflush(stderr);
	trace_stderr_fd = dup(STDERR_FILENO);
	dup2(fileno(trace_file), STDERR_FILENO);
	ulog_open(ULOG_STDIO, LOG_DAEMON, NULL);
	ulog_threshold(LOG_DEBUG);
	trace_domain_set(TRACE_HAL, true);
	trace_domain_set(TRACE_HOOK, true);
}

static unsigned int trace_capture_count(const char *needle)
{
	char line[512];
	unsigned int hits = 0;

	if (!trace_file)
		return 0;

	trace_stderr_restore();

	rewind(trace_file);
	while (fgets(line, sizeof(line), trace_file)) {
		if (strstr(line, needle))
			hits++;
	}

	return hits;
}

/* the same count against the level the daemon chose, for a message whose
 * priority is part of the rule */
static unsigned int trace_capture_count_at(int priority, const char *needle)
{
	char want[192];

	snprintf(want, sizeof(want), "prio=%d %s", priority, needle);

	return trace_capture_count(want);
}

/*
 * The default trace arm names the kind it did not consume by its word, so a
 * reader of the call trace timeline needs no enum; the two words this suite
 * asks for are spelled here, apart from the unit's own table.
 */
static unsigned int trace_default_count(uint16_t type)
{
	char needle[32];
	const char *word;

	switch (type) {
	case HAL_EVENT_SLIC_FAULT:
		word = "slic_fault";
		break;
	case HAL_EVENT_LINE:
		word = "line";
		break;
	default:
		word = "type=";
		break;
	}
	snprintf(needle, sizeof(needle), "event %s", word);

	return trace_capture_count(needle);
}

/* -------------------------------------------------------------- fixtures */

static struct hal_event ev_make(uint16_t type, uint16_t line, uint16_t stream)
{
	struct hal_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.line = line;
	ev.stream = stream;

	return ev;
}

static struct hal_event fault_make(uint32_t code, uint16_t line)
{
	struct hal_event ev = ev_make(HAL_EVENT_SLIC_FAULT, line, 0);

	ev.u.fault.code = code;

	return ev;
}

static struct hal_event line_state_make(uint8_t state, uint16_t line)
{
	struct hal_event ev = ev_make(HAL_EVENT_LINE, line, 0);

	ev.u.line.state = state;

	return ev;
}

static struct hal_event lec_make(uint8_t kind, uint8_t reason, uint16_t tone,
				 uint8_t direction, uint16_t line)
{
	struct hal_event ev = ev_make(HAL_EVENT_LEC_POLICY, line, 0);

	ev.u.lec.kind = kind;
	ev.u.lec.reason = reason;
	ev.u.lec.tone = tone;
	ev.u.lec.direction = direction;

	return ev;
}

/*
 * The tick a disabler record travels with. It is far from this host's clock on
 * purpose: the view keeps the instant the daemon received the record at, so a
 * writer that copied the tick is visible in the bracket below.
 */
#define LEC_STAMP	0x0055aa11u

/*
 * The reference clock, read directly and truncated the way uvoice_mono_ms()
 * truncates. A bracket built with the call under test would hold whatever
 * clock that call read, so a helper switched to CLOCK_REALTIME would pass.
 */
static uint32_t mono_ms_ref(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

#define JB_STAMP	0x11223344u
#define JB_TOTAL	900001ull
#define JB_DROP		4242ull
#define JB_SIZE_MS	60u
#define JB_JITTER	7u

static struct hal_event jb_make(uint16_t line, uint16_t stream)
{
	struct hal_event ev = ev_make(HAL_EVENT_JB_UPDATE, line, stream);

	ev.stamp_ms = JB_STAMP;
	ev.u.jb.total = JB_TOTAL;
	ev.u.jb.drop = JB_DROP;
	ev.u.jb.size_ms = JB_SIZE_MS;
	ev.u.jb.jitter_avg = JB_JITTER;

	return ev;
}

/*
 * A record outside the widths that reached the store would land inside the
 * array whenever the flat index still fits it: stream HAL_STREAM_MAX of line 0
 * is the first slot of line 1. Comparing the whole array against a cleared one
 * catches that write wherever it lands inside the cache.
 */
static bool jb_all_clear(void)
{
	struct uvoice_jb clear[HAL_LINE_MAX][HAL_STREAM_MAX];

	memset(clear, 0, sizeof(clear));

	return !memcmp(uv.jb, clear, sizeof(clear));
}

static unsigned int jb_valid_count(void)
{
	unsigned int count = 0;
	unsigned int line;
	unsigned int stream;

	for (line = 0; line < HAL_LINE_MAX; line++) {
		for (stream = 0; stream < HAL_STREAM_MAX; stream++) {
			if (uv.jb[line][stream].valid)
				count++;
		}
	}

	return count;
}

static unsigned int route_probe(uint16_t type)
{
	struct hal_event ev = ev_make(type, 0, 0);

	line_events = 0;
	event_dispatch(&ev);

	return line_events;
}

/* ------------------------------------------------------- R7: the seq walk */

/*
 * The emitter numbers hardware records 1..65535 and wraps to 1 (hal.h section
 * 2), so one pass of the replay below is SEQ_WRAP records long and record
 * SEQ_WRAP is followed by seq 1 again. 200000 records therefore carry three
 * wraps, which is what the lossless oracle needs: a walk that never crosses
 * the wrap proves nothing about the arithmetic that broke there (a cli round
 * measured 7 false losses over a 500k lossless replay).
 */
#define SEQ_WRAP		65535u
#define SEQ_REPLAY_RECORDS	200000u
#define SEQ_DROP_STRIDE		300u
#define SEQ_DROP_PHASE		150u
#define SEQ_DROP_BUDGET		511u

static void seq_feed(uint16_t seq)
{
	struct hal_event ev = ev_make(HAL_EVENT_NONE, 0, 0);

	ev.seq = seq;
	event_seq_check(&ev);
}

static unsigned int seq_replay(unsigned int records, unsigned int stride,
			       unsigned int phase, unsigned int budget)
{
	unsigned int dropped = 0;
	unsigned int n;

	for (n = 0; n < records; n++) {
		if (stride && dropped < budget && n % stride == phase) {
			dropped++;
			continue;
		}

		seq_feed((uint16_t)(n % SEQ_WRAP + 1));
	}

	return dropped;
}

static void seq_walk_tests(void)
{
	unsigned int dropped;

	CASE("R7 seq walk: a lossless 200k replay across three wraps loses nothing");
	env_reset();
	CHECK(SEQ_REPLAY_RECORDS / SEQ_WRAP >= 3);
	seq_replay(SEQ_REPLAY_RECORDS, 0, 0, 0);
	CHECK(uv.event_seq_valid);
	CHECK_UINT(uv.events_lost, 0);
	CHECK_UINT(uv.event_seq, (SEQ_REPLAY_RECORDS - 1) % SEQ_WRAP + 1);

	CASE("R7 seq walk: exactly 511 dropped records count 511");
	env_reset();
	dropped = seq_replay(SEQ_REPLAY_RECORDS, SEQ_DROP_STRIDE, SEQ_DROP_PHASE,
			     SEQ_DROP_BUDGET);
	CHECK_UINT(dropped, SEQ_DROP_BUDGET);
	CHECK_UINT(uv.events_lost, SEQ_DROP_BUDGET);

	CASE("R7 seq walk: the straddle 65534 -> 2 counts two");
	env_reset();
	seq_feed(65534);
	CHECK_UINT(uv.events_lost, 0);
	seq_feed(2);
	CHECK_UINT(uv.events_lost, 2);
	CHECK_UINT(uv.event_seq, 2);

	CASE("R7 seq walk: the straddle 65530 -> 3 counts seven");
	env_reset();
	seq_feed(65530);
	CHECK_UINT(uv.events_lost, 0);
	seq_feed(3);
	CHECK_UINT(uv.events_lost, 7);
	CHECK_UINT(uv.event_seq, 3);

	CASE("R7 seq walk: the first nonzero seq seeds without loss");
	env_reset();
	seq_feed(4242);
	CHECK(uv.event_seq_valid);
	CHECK_UINT(uv.event_seq, 4242);
	CHECK_UINT(uv.events_lost, 0);
	seq_feed(4243);
	CHECK_UINT(uv.events_lost, 0);

	CASE("R7 seq walk: a seq 0 record neither seeds nor advances the walk");
	env_reset();
	seq_feed(0);
	seq_feed(0);
	CHECK(!uv.event_seq_valid);
	CHECK_UINT(uv.event_seq, 0);
	CHECK_UINT(uv.events_lost, 0);
	seq_feed(5);
	CHECK(uv.event_seq_valid);
	CHECK_UINT(uv.event_seq, 5);
	CHECK_UINT(uv.events_lost, 0);
	seq_feed(0);
	CHECK_UINT(uv.event_seq, 5);
	CHECK_UINT(uv.events_lost, 0);

	CASE("R7 seq walk: a seq 0 record is excluded from the loss count");
	seq_feed(6);
	CHECK_UINT(uv.event_seq, 6);
	CHECK_UINT(uv.events_lost, 0);
	seq_feed(0);
	seq_feed(7);
	CHECK_UINT(uv.event_seq, 7);
	CHECK_UINT(uv.events_lost, 0);
}

/*
 * The hand-off behind the gap. A lost hook report leaves the machine and the
 * handset in disagreement, so the dispatch asks the line shell to read the
 * hardware again. The request carries no argument. A case can therefore check
 * two facts: that the dispatch made the request, and the loss count at that
 * moment.
 */
static void t_hook_resync(void)
{
	CASE("hook resync: a gap asks the line shell to re-read the hooks");
	env_reset();
	seq_feed(10);
	CHECK_UINT(resync_asks, 0);
	seq_feed(12);
	CHECK_UINT(uv.events_lost, 1);
	CHECK_UINT(resync_asks, 1);

	CASE("hook resync: a lossless walk asks for nothing");
	env_reset();
	seq_replay(SEQ_REPLAY_RECORDS, 0, 0, 0);
	CHECK_UINT(uv.events_lost, 0);
	CHECK_UINT(resync_asks, 0);

	CASE("hook resync: a seq 0 record asks for nothing");
	env_reset();
	seq_feed(0);
	seq_feed(20);
	seq_feed(0);
	CHECK_UINT(uv.events_lost, 0);
	CHECK_UINT(resync_asks, 0);

	CASE("hook resync: the ask follows the loss count, so the counter is "
	     "already moved");
	env_reset();
	seq_feed(30);
	seq_feed(35);
	CHECK_UINT(uv.events_lost, 4);
	CHECK_UINT(resync_asks, 1);
	CHECK_UINT(resync_lost, 4);
}

/* ---------------------------------------------------------- the fault gate */

static void fault_gate_tests(void)
{
	struct hal_event ev;

	CASE("fault gate: the udsp backend takes the stall bit to the exit request");
	env_reset();
	ev = fault_make(1u << 0, 0);
	event_dispatch(&ev);
	CHECK_UINT(exit_requests, 1);
	CHECK_UINT(line_events, 0);

	CASE("fault gate: the stall bit beside another bit still exits");
	env_reset();
	ev = fault_make((1u << 0) | (1u << 5), 1);
	event_dispatch(&ev);
	CHECK_UINT(exit_requests, 1);
	CHECK_UINT(line_events, 0);

	CASE("fault gate: a backend other than udsp leaves the same record to "
	     "the trace");
	env_reset();
	backend_name_set("other");
	trace_capture_begin();
	ev = fault_make(1u << 0, 0);
	event_dispatch(&ev);
	CHECK_UINT(exit_requests, 0);
	CHECK_UINT(line_events, 0);
	CHECK_UINT(trace_default_count(HAL_EVENT_SLIC_FAULT), 1);
	trace_capture_end();

	CASE("fault gate: a non-stall bit under a backend other than udsp "
	     "reaches neither line nor exit");
	env_reset();
	backend_name_set("other");
	trace_capture_begin();
	ev = fault_make(1u << 3, 1);
	event_dispatch(&ev);
	CHECK_UINT(exit_requests, 0);
	CHECK_UINT(line_events, 0);
	CHECK_UINT(trace_default_count(HAL_EVENT_SLIC_FAULT), 1);
	trace_capture_end();

	CASE("fault gate: the stub backend falls through as well");
	env_reset();
	backend_name_set("stub");
	ev = fault_make(1u << 0, 0);
	event_dispatch(&ev);
	CHECK_UINT(exit_requests, 0);
	CHECK_UINT(line_events, 0);

	CASE("fault gate: a non-stall bit under udsp reaches the line shell");
	env_reset();
	trace_capture_begin();
	ev = fault_make(1u << 1, 1);
	event_dispatch(&ev);
	CHECK_UINT(line_events, 1);
	CHECK_UINT(exit_requests, 0);
	CHECK_UINT(line_last.type, HAL_EVENT_SLIC_FAULT);
	CHECK_UINT(line_last.line, 1);
	CHECK_UINT(line_last.u.fault.code, 1u << 1);
	CHECK_UINT(trace_default_count(HAL_EVENT_SLIC_FAULT), 0);
	trace_capture_end();

	/* The backend owns bit 16 for a dead stream. It sat at bit 8 until the
	 * library took bits 9 and 10, and bit 8 is unassigned on both sides
	 * (hal/udsp/udsp_int.h, udsp event.h). */
	CASE("the udsp backend takes the stream-dead bit to the exit request");
	env_reset();
	ev = fault_make(1u << 16, 1);
	event_dispatch(&ev);
	CHECK_UINT(exit_requests, 1);
	CHECK_UINT(line_events, 0);

	CASE("the stream-dead bit during a pending stop requests no exit");
	env_reset();
	stop_pending = true;
	ev = fault_make(1u << 16, 1);
	event_dispatch(&ev);
	CHECK_UINT(exit_requests, 0);
	CHECK_UINT(line_events, 0);

	CASE("the stream-dead bit under a backend other than udsp reaches "
	     "neither line nor exit");
	env_reset();
	backend_name_set("other");
	ev = fault_make(1u << 16, 1);
	event_dispatch(&ev);
	CHECK_UINT(exit_requests, 0);
	CHECK_UINT(line_events, 0);

	/* bit 8 belongs to no producer now, so a record carrying it alone is
	 * an ordinary per line fault and must not exit */
	CASE("the retired bit 8 is a line fault and no exit");
	env_reset();
	ev = fault_make(1u << 8, 1);
	event_dispatch(&ev);
	CHECK_UINT(exit_requests, 0);
	CHECK_UINT(line_events, 1);

	/*
	 * The device reset is terminal for the daemon. Only a fresh udsp_open
	 * pulses the reset the device needs, and procd performs the respawn
	 * that opens the library again.
	 */
	CASE("a device reset record takes the exit request");
	env_reset();
	ev = fault_make(1u << 10, 1);
	event_dispatch(&ev);
	CHECK_UINT(exit_requests, 1);
	CHECK_UINT(line_events, 0);

	CASE("a device reset during a pending stop requests no exit");
	env_reset();
	stop_pending = true;
	ev = fault_make(1u << 10, 1);
	event_dispatch(&ev);
	CHECK_UINT(exit_requests, 0);
	CHECK_UINT(line_events, 0);

	CASE("a device reset under a backend other than udsp reaches neither "
	     "line nor exit");
	env_reset();
	backend_name_set("other");
	trace_capture_begin();
	ev = fault_make(1u << 10, 1);
	event_dispatch(&ev);
	CHECK_UINT(exit_requests, 0);
	CHECK_UINT(line_events, 0);
	CHECK_UINT(trace_default_count(HAL_EVENT_SLIC_FAULT), 1);
	trace_capture_end();

	/* The device reset is device scoped and a clock fault names every line
	 * of the device, so one record can carry both. The terminal answer
	 * wins: no line can act on a device that needs a reset. */
	CASE("a device reset beside a clock fault exits and reaches no line");
	env_reset();
	ev = fault_make((1u << 10) | (1u << 9), 1);
	event_dispatch(&ev);
	CHECK_UINT(exit_requests, 1);
	CHECK_UINT(line_events, 0);

	/* the clock fault is an ordinary per line fault: the line goes out of
	 * service and the clear brings it back (udsp event.h) */
	CASE("a clock fault record reaches the line and asks for no exit");
	env_reset();
	trace_capture_begin();
	ev = fault_make(1u << 9, 1);
	event_dispatch(&ev);
	CHECK_UINT(line_events, 1);
	CHECK_UINT(exit_requests, 0);
	CHECK_UINT(line_last.u.fault.code, 1u << 9);
	CHECK_UINT(trace_default_count(HAL_EVENT_SLIC_FAULT), 0);
	trace_capture_end();

	/*
	 * The clear (hal.h u.fault.code). It carries no bit at all, so it must
	 * not read as a fault of any kind. It reaches the line shell, which
	 * drops the fault reason, and nothing else moves.
	 */
	CASE("a clear reaches the line shell with its code 0");
	env_reset();
	trace_capture_begin();
	ev = fault_make(0, 1);
	event_dispatch(&ev);
	CHECK_UINT(line_events, 1);
	CHECK_UINT(exit_requests, 0);
	CHECK_UINT(line_last.type, HAL_EVENT_SLIC_FAULT);
	CHECK_UINT(line_last.line, 1);
	CHECK_UINT(line_last.u.fault.code, 0);
	CHECK_UINT(trace_default_count(HAL_EVENT_SLIC_FAULT), 0);

	/* The clear has its own report, and the fault report must not stand in
	 * for it. An operator who reads "forcing it out of service" against a
	 * line that just came back reads the log backwards. */
	CASE("and it is reported as a clear, not as a fault");
	CHECK_UINT(trace_capture_count("line 1 slic fault cleared"), 1);
	CHECK_UINT(trace_capture_count("forcing it out of service"), 0);
	trace_capture_end();

	/* The clear moves a line toward service, and the service entry behind
	 * it reads the hook. A stop that is already running would then see a
	 * seize and a dial tone on a daemon that is leaving. */
	CASE("a clear inside the stop does not reach the line");
	env_reset();
	stop_pending = true;
	ev = fault_make(0, 0);
	event_dispatch(&ev);
	CHECK_UINT(line_events, 0);
	CHECK_UINT(exit_requests, 0);

	CASE("a clear under a backend other than udsp reaches neither line nor "
	     "exit");
	env_reset();
	backend_name_set("other");
	trace_capture_begin();
	ev = fault_make(0, 1);
	event_dispatch(&ev);
	CHECK_UINT(line_events, 0);
	CHECK_UINT(exit_requests, 0);
	CHECK_UINT(trace_default_count(HAL_EVENT_SLIC_FAULT), 1);
	trace_capture_end();
}

/* --------------------------------------------- the stop that owns the rest */

static void stop_suppression_tests(void)
{
	struct hal_event ev;

	CASE("stop suppression: a stall inside the stop asks for no exit");
	env_reset();
	stop_pending = true;
	ev = fault_make(1u << 0, 0);
	event_dispatch(&ev);
	CHECK_UINT(exit_requests, 0);
	CHECK_UINT(line_events, 0);

	CASE("stop suppression: the same record outside the stop does exit");
	env_reset();
	ev = fault_make(1u << 0, 0);
	event_dispatch(&ev);
	CHECK_UINT(exit_requests, 1);

	CASE("stop suppression: a slic fault inside the stop still reaches the line");
	env_reset();
	stop_pending = true;
	ev = fault_make(1u << 2, 0);
	event_dispatch(&ev);
	CHECK_UINT(line_events, 1);
	CHECK_UINT(exit_requests, 0);
}

/* -------------------------------------------------------- the line record */

/*
 * The backend declares a port dead and reports it back later (hal.h
 * u.line.state), and the two directions differ, as at the fault gate. The
 * dispatch hands over a record that takes a line out of service whatever the
 * daemon does, because a teardown wants that line out anyway. Inside a stop,
 * it drops a record that puts the line back, because the service entry reads
 * the hook and can seize a handset.
 *
 * The dispatch logs the return at NOTE. It does not log a DOWN record, which
 * repeats; the line shell logs it once per entry against its own memory. The
 * case reads the priority as well as the text, so a NOTE cannot pass as an
 * ERR.
 */
static void line_state_tests(void)
{
	struct hal_event ev;
	uint8_t state;

	CASE("a dead line record reaches the line shell unchanged");
	env_reset();
	ev = line_state_make(HAL_LINE_DOWN, 1);
	event_dispatch(&ev);
	CHECK_UINT(line_events, 1);
	CHECK_UINT(exit_requests, 0);
	CHECK_UINT(line_last.type, HAL_EVENT_LINE);
	CHECK_UINT(line_last.line, 1);
	CHECK_UINT(line_last.u.line.state, HAL_LINE_DOWN);

	/* The dispatch adds no log of its own for a DOWN record. The line shell
	 * owns the once-per-entry ERR, because the record repeats and the shell
	 * holds the memory the repeat reads. */
	CASE("and the dispatch itself logs nothing for it");
	env_reset();
	trace_capture_begin();
	ev = line_state_make(HAL_LINE_DOWN, 1);
	event_dispatch(&ev);
	CHECK_UINT(trace_default_count(HAL_EVENT_LINE), 0);
	CHECK_UINT(trace_capture_count("backend reports the line down"), 0);
	CHECK_UINT(trace_capture_count("backend line returned"), 0);
	trace_capture_end();

	CASE("a dead line record inside the stop still reaches the line");
	env_reset();
	stop_pending = true;
	ev = line_state_make(HAL_LINE_DOWN, 0);
	event_dispatch(&ev);
	CHECK_UINT(line_events, 1);
	CHECK_UINT(exit_requests, 0);

	CASE("the line return reaches the line shell");
	env_reset();
	trace_capture_begin();
	ev = line_state_make(HAL_LINE_ACTIVE, 1);
	event_dispatch(&ev);
	CHECK_UINT(line_events, 1);
	CHECK_UINT(exit_requests, 0);
	CHECK_UINT(line_last.u.line.state, HAL_LINE_ACTIVE);
	CHECK_UINT(trace_default_count(HAL_EVENT_LINE), 0);

	/* The return is a NOTE, not an ERR. An operator who reads it as an
	 * error reads the line's service backwards. */
	CASE("and it is reported as the line returning at notice level");
	CHECK_UINT(trace_capture_count_at(LOG_NOTICE,
					  "line 1 backend line returned"), 1);
	CHECK_UINT(trace_capture_count_at(LOG_ERR, "backend line returned"), 0);
	trace_capture_end();

	/* the clear rule of the fault arm: the service entry behind this
	 * record would seize a line on a daemon that is leaving */
	CASE("the line return inside the stop does not reach the line");
	env_reset();
	stop_pending = true;
	trace_capture_begin();
	ev = line_state_make(HAL_LINE_ACTIVE, 1);
	event_dispatch(&ev);
	CHECK_UINT(line_events, 0);
	CHECK_UINT(exit_requests, 0);

	CASE("and the drop is reported once at notice level");
	CHECK_UINT(trace_capture_count_at(LOG_NOTICE,
					  "line 1 backend line returned inside "
					  "the stop, ignored"), 1);
	trace_capture_end();

	/* The test is against DOWN alone, so a state that is neither DOWN nor
	 * ACTIVE is a return and the stop gates it like any other return. */
	CASE("a non-active non-down state inside the stop is dropped");
	env_reset();
	stop_pending = true;
	ev = line_state_make(HAL_LINE_OHT, 1);
	event_dispatch(&ev);
	CHECK_UINT(line_events, 0);
	CHECK_UINT(exit_requests, 0);

	CASE("and the same state outside the stop reaches the line");
	env_reset();
	ev = line_state_make(HAL_LINE_BUSY, 1);
	event_dispatch(&ev);
	CHECK_UINT(line_events, 1);
	CHECK_UINT(exit_requests, 0);
	CHECK_UINT(line_last.u.line.state, HAL_LINE_BUSY);

	/* HAL_LINE_DOWN is the one state that says the port is dead. Every
	 * other state of the enum says the port carries a feed. */
	CASE("every state above down reads as the line returning");
	for (state = HAL_LINE_ACTIVE; state <= HAL_LINE_HOWLER; state++) {
		env_reset();
		ev = line_state_make(state, 0);
		event_dispatch(&ev);
		CHECK_UINT(line_events, 1);
		CHECK_UINT(line_last.u.line.state, state);
	}

	/* Only the udsp backend posts a line record for a dead port and for
	 * its return. Under any other backend the record says nothing about a
	 * dead port. */
	CASE("a dead line record under a backend other than udsp reaches "
	     "neither line nor exit");
	env_reset();
	backend_name_set("other");
	trace_capture_begin();
	ev = line_state_make(HAL_LINE_DOWN, 1);
	event_dispatch(&ev);
	CHECK_UINT(line_events, 0);
	CHECK_UINT(exit_requests, 0);
	CHECK_UINT(trace_default_count(HAL_EVENT_LINE), 1);
	CHECK_UINT(trace_capture_count("backend reports the line down"), 0);
	trace_capture_end();

	CASE("a line return under a backend other than udsp takes the same "
	     "default trace");
	env_reset();
	backend_name_set("other");
	trace_capture_begin();
	ev = line_state_make(HAL_LINE_ACTIVE, 1);
	event_dispatch(&ev);
	CHECK_UINT(line_events, 0);
	CHECK_UINT(trace_default_count(HAL_EVENT_LINE), 1);
	CHECK_UINT(trace_capture_count("backend line returned"), 0);
	trace_capture_end();

	CASE("the stub backend falls through on a line record as well");
	env_reset();
	backend_name_set("stub");
	ev = line_state_make(HAL_LINE_DOWN, 0);
	event_dispatch(&ev);
	CHECK_UINT(line_events, 0);
	CHECK_UINT(exit_requests, 0);
}

/* ---------------------------------------------------- the JB cache branch */

static void jb_arm_tests(void)
{
	struct hal_event ev;

	CASE("JB arm: a report caches its payload under its own line and stream");
	env_reset();
	ev = jb_make(1, 3);
	event_dispatch(&ev);
	CHECK(uv.jb[1][3].valid);
	CHECK_UINT(uv.jb[1][3].stamp_ms, JB_STAMP);
	CHECK_UINT(uv.jb[1][3].total, JB_TOTAL);
	CHECK_UINT(uv.jb[1][3].drop, JB_DROP);
	CHECK_UINT(uv.jb[1][3].size_ms, JB_SIZE_MS);
	CHECK_UINT(uv.jb[1][3].jitter_avg, JB_JITTER);
	CHECK_UINT(jb_valid_count(), 1);

	CASE("JB arm: the arm consumes the record before the default trace");
	env_reset();
	trace_capture_begin();
	ev = jb_make(0, 0);
	event_dispatch(&ev);
	CHECK(uv.jb[0][0].valid);
	CHECK_UINT(line_events, 0);
	CHECK_UINT(exit_requests, 0);
	CHECK_UINT(trace_default_count(HAL_EVENT_JB_UPDATE), 0);
	CHECK_UINT(trace_capture_count("jb update outside the hal widths"), 0);
	trace_capture_end();

	CASE("JB arm: a line outside the hal widths changes nothing");
	env_reset();
	trace_capture_begin();
	ev = jb_make(HAL_LINE_MAX, 0);
	event_dispatch(&ev);
	CHECK(jb_all_clear());
	CHECK_UINT(jb_valid_count(), 0);
	CHECK_UINT(line_events, 0);
	CHECK_UINT(trace_capture_count("jb update outside the hal widths"), 1);
	trace_capture_end();

	CASE("JB arm: a stream outside the hal widths changes nothing");
	env_reset();
	trace_capture_begin();
	ev = jb_make(0, HAL_STREAM_MAX);
	event_dispatch(&ev);
	CHECK(jb_all_clear());
	CHECK_UINT(jb_valid_count(), 0);
	CHECK_UINT(line_events, 0);
	CHECK_UINT(trace_capture_count("jb update outside the hal widths"), 1);
	trace_capture_end();

	CASE("JB arm: the last report of a stream replaces the one before it");
	env_reset();
	ev = jb_make(0, 1);
	event_dispatch(&ev);
	ev.stamp_ms = JB_STAMP + 1000;
	ev.u.jb.drop = JB_DROP + 1;
	event_dispatch(&ev);
	CHECK_UINT(uv.jb[0][1].stamp_ms, JB_STAMP + 1000);
	CHECK_UINT(uv.jb[0][1].drop, JB_DROP + 1);
	CHECK_UINT(jb_valid_count(), 1);
}

/* ------------------------------------------------ the tone disabler's report */

/*
 * The disabler runs inside the DSP and moves the canceller of a line whose
 * call state does not explain the move. This trace is therefore the only
 * record that an operator gets. Every case reads the rendered line: a record
 * that printed a number instead of a name answers no question on a unit.
 *
 * Every latch case reads the feed. The media switch of line/line.c drops that
 * datum on purpose, and the disabler keeps it. A row that lost the feed would
 * make the two layers seem to disagree by accident.
 */
static void lec_policy_tests(void)
{
	uint32_t before, after;
	struct hal_event ev;

	CASE("a modem latch names the tone, the feed and the policy it applied");
	env_reset();
	trace_capture_begin();
	ev = lec_make(HAL_PASSTHROUGH_MODEM, HAL_LEC_REASON_LATCH,
		      HAL_TONE_DET_ANSBAR, HAL_TONE_DIR_REMOTE, 0);
	ev.stamp_ms = LEC_STAMP;
	before = mono_ms_ref();
	event_dispatch(&ev);
	after = mono_ms_ref();
	CHECK_UINT(trace_capture_count("line 0 tone disabler: ANSBAR from the "
				       "wire latched, canceller off whole"), 1);
	trace_capture_end();

	/*
	 * The same record also fills the view the lec row of "voice status"
	 * reports (uvoice.h, struct uvoice_lec_policy). The trace is what a live
	 * log carries and the view is what a poll answers afterwards, so a
	 * record that reached one and not the other would let the two disagree.
	 */
	CASE("and it fills the view the lec row reports");
	CHECK(uv.lec_policy[0].valid);
	CHECK_UINT(uv.lec_policy[0].kind, HAL_PASSTHROUGH_MODEM);
	CHECK_UINT(uv.lec_policy[0].reason, HAL_LEC_REASON_LATCH);
	CHECK_UINT(uv.lec_policy[0].tone, HAL_TONE_DET_ANSBAR);
	CHECK_UINT(uv.lec_policy[0].direction, HAL_TONE_DIR_REMOTE);
	/* The instant is the daemon's receipt of the record and not the tick it
	 * travelled with: hal.h makes hal_event.stamp_ms a backend tick, and
	 * the row's other two instants are this host's clock. */
	CHECK(uv.lec_policy[0].stamp_ms >= before);
	CHECK(uv.lec_policy[0].stamp_ms <= after);
	CHECK(uv.lec_policy[0].stamp_ms != LEC_STAMP);
	/* and it fills the line it names and no other */
	CHECK(!uv.lec_policy[1].valid);

	CASE("a release fills the view too, with no tone to name");
	ev = lec_make(HAL_PASSTHROUGH_OFF, HAL_LEC_REASON_RELEASE_QUIET, 0, 0, 0);
	ev.stamp_ms = LEC_STAMP + 250;
	before = mono_ms_ref();
	event_dispatch(&ev);
	after = mono_ms_ref();
	CHECK(uv.lec_policy[0].valid);
	CHECK_UINT(uv.lec_policy[0].kind, HAL_PASSTHROUGH_OFF);
	CHECK_UINT(uv.lec_policy[0].reason, HAL_LEC_REASON_RELEASE_QUIET);
	CHECK(uv.lec_policy[0].stamp_ms >= before);
	CHECK(uv.lec_policy[0].stamp_ms <= after);
	CHECK(uv.lec_policy[0].stamp_ms != LEC_STAMP + 250);

	/* The view is the LAST move and not the first: a line that latched, was
	 * released and latched again reads the latch it is holding now. */
	CASE("a later record replaces the view the earlier one left");
	ev = lec_make(HAL_PASSTHROUGH_FAX, HAL_LEC_REASON_LATCH,
		      HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	ev.stamp_ms = LEC_STAMP + 500;
	before = mono_ms_ref();
	event_dispatch(&ev);
	after = mono_ms_ref();
	CHECK_UINT(uv.lec_policy[0].kind, HAL_PASSTHROUGH_FAX);
	CHECK_UINT(uv.lec_policy[0].reason, HAL_LEC_REASON_LATCH);
	CHECK_UINT(uv.lec_policy[0].tone, HAL_TONE_DET_CED);
	CHECK_UINT(uv.lec_policy[0].direction, HAL_TONE_DIR_LOCAL);
	CHECK(uv.lec_policy[0].stamp_ms >= before);
	CHECK(uv.lec_policy[0].stamp_ms <= after);
	CHECK(uv.lec_policy[0].stamp_ms != LEC_STAMP + 500);

	CASE("the arm consumes the record before the line shell and the trace");
	env_reset();
	trace_capture_begin();
	ev = lec_make(HAL_PASSTHROUGH_FAX, HAL_LEC_REASON_LATCH,
		      HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 1);
	event_dispatch(&ev);
	CHECK_UINT(line_events, 0);
	CHECK_UINT(exit_requests, 0);
	CHECK_UINT(trace_default_count(HAL_EVENT_LEC_POLICY), 0);
	CHECK_UINT(trace_capture_count("line 1 tone disabler: CED from the wire "
				       "latched, canceller on, processor off"), 1);
	trace_capture_end();

	CASE("a tone the phone raised names the phone");
	env_reset();
	trace_capture_begin();
	ev = lec_make(HAL_PASSTHROUGH_FAX, HAL_LEC_REASON_LATCH,
		      HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	event_dispatch(&ev);
	CHECK_UINT(trace_capture_count("line 0 tone disabler: CED from the phone "
				       "latched, canceller on, processor off"), 1);
	trace_capture_end();

	CASE("the V.21 demotion names the fall back to the fax policy");
	env_reset();
	trace_capture_begin();
	ev = lec_make(HAL_PASSTHROUGH_FAX, HAL_LEC_REASON_DEMOTE,
		      HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_REMOTE, 0);
	event_dispatch(&ev);
	CHECK_UINT(trace_capture_count("line 0 tone disabler: V21_PREAMBLE from "
				       "the wire demoted the latch, canceller "
				       "on, processor off"), 1);
	trace_capture_end();

	/* A release answers no tone, so the row must print neither the OTHER
	 * nor the LOCAL that its two zeroed fields read as. */
	CASE("the quiet release names the rule, no tone and no feed");
	env_reset();
	trace_capture_begin();
	ev = lec_make(HAL_PASSTHROUGH_OFF, HAL_LEC_REASON_RELEASE_QUIET, 0, 0, 0);
	event_dispatch(&ev);
	CHECK_UINT(trace_capture_count("line 0 tone disabler: released, holding "
				       "band quiet, canceller as the channel asks"), 1);
	CHECK_UINT(trace_capture_count("OTHER"), 0);
	CHECK_UINT(trace_capture_count("from the phone"), 0);
	trace_capture_end();

	CASE("the topology release names the streams that moved");
	env_reset();
	trace_capture_begin();
	ev = lec_make(HAL_PASSTHROUGH_OFF, HAL_LEC_REASON_RELEASE_TOPOLOGY, 0, 0,
		      2);
	event_dispatch(&ev);
	CHECK_UINT(trace_capture_count("line 2 tone disabler: released, the "
				       "line's streams moved, canceller as the "
				       "channel asks"), 1);
	trace_capture_end();

	/*
	 * Line 2 is at HAL_LINE_MAX, so that record has no slot to fill. It is
	 * dropped rather than clamped, the jb store rule: a clamp would file one
	 * line's policy under another one's name, and the row above shows the
	 * move still reaches a log.
	 */
	CASE("a record outside the hal widths fills no view and still traces");
	CHECK(!uv.lec_policy[0].valid);
	CHECK(!uv.lec_policy[1].valid);

	/* the one release this daemon asks for itself: the call layer decided
	 * the answer tone was a false one and took the leg back to voice */
	CASE("the caller release names the call layer that asked for it");
	env_reset();
	trace_capture_begin();
	ev = lec_make(HAL_PASSTHROUGH_OFF, HAL_LEC_REASON_RELEASE_CALLER, 0, 0,
		      1);
	event_dispatch(&ev);
	CHECK_UINT(trace_capture_count(
			   "line 1 tone disabler: released, the call layer "
			   "read the tone as a false one, canceller as the "
			   "channel asks"), 1);
	CHECK_UINT(trace_capture_count("OTHER"), 0);
	trace_capture_end();
	CHECK_UINT(uv.lec_policy[1].reason, HAL_LEC_REASON_RELEASE_CALLER);

	/* A backend that grew a reason, a kind or a feed this build does not
	 * name must still produce a readable row rather than an index into
	 * nothing. */
	CASE("a reason and a kind outside the contract still render");
	env_reset();
	trace_capture_begin();
	ev = lec_make(42, 77, HAL_TONE_DET_CED, 3, 0);
	event_dispatch(&ev);
	CHECK_UINT(trace_capture_count("line 0 tone disabler: moved for an "
				       "unknown reason, canceller policy unknown"), 1);
	trace_capture_end();

	CASE("and a feed outside the contract renders on a latch");
	env_reset();
	trace_capture_begin();
	ev = lec_make(HAL_PASSTHROUGH_MODEM, HAL_LEC_REASON_LATCH,
		      HAL_TONE_DET_ANSBAR, 9, 0);
	event_dispatch(&ev);
	CHECK_UINT(trace_capture_count("line 0 tone disabler: ANSBAR from an "
				       "unnamed feed latched, canceller off "
				       "whole"), 1);
	trace_capture_end();
}

/* --------------------------------------------------------- the tone names */

/*
 * The tone rows that the disabler answers use the same domain, so the domain
 * has two producers. A code and a name cannot be read side by side in one
 * log, so both producers print names.
 */
static void tone_name_tests(void)
{
	struct hal_event ev;

	CASE("a tone record names its code");
	env_reset();
	trace_capture_begin();
	ev = ev_make(HAL_EVENT_TONE, 0, 0);
	ev.u.tone.code = HAL_TONE_DET_ANSBAR;
	ev.u.tone.direction = HAL_TONE_DIR_REMOTE;
	ev.u.tone.duration_ms = 450;
	event_dispatch(&ev);
	CHECK_UINT(line_events, 1);
	CHECK_UINT(trace_capture_count("line 0 tone code=ANSBAR dir=wire dur=450"), 1);
	trace_capture_end();

	CASE("a code no contract names renders as unknown");
	env_reset();
	trace_capture_begin();
	ev = ev_make(HAL_EVENT_TONE, 1, 0);
	ev.u.tone.code = 4242;
	event_dispatch(&ev);
	CHECK_UINT(line_events, 1);
	CHECK_UINT(trace_capture_count("line 1 tone code=UNKNOWN"), 1);
	trace_capture_end();

	CASE("every named tone renders its own name");
	CHECK(!strcmp(hal_tone_detect_name(HAL_TONE_DET_OTHER), "OTHER"));
	CHECK(!strcmp(hal_tone_detect_name(HAL_TONE_DET_CNG), "CNG"));
	CHECK(!strcmp(hal_tone_detect_name(HAL_TONE_DET_CED), "CED"));
	CHECK(!strcmp(hal_tone_detect_name(HAL_TONE_DET_ANS), "ANS"));
	CHECK(!strcmp(hal_tone_detect_name(HAL_TONE_DET_ANSAM), "ANSAM"));
	CHECK(!strcmp(hal_tone_detect_name(HAL_TONE_DET_ANSBAR), "ANSBAR"));
	CHECK(!strcmp(hal_tone_detect_name(HAL_TONE_DET_V21_PREAMBLE),
		      "V21_PREAMBLE"));
	CHECK(!strcmp(hal_tone_detect_name(HAL_TONE_DET_FAX_CM), "FAX_CM"));
	CHECK(!strcmp(hal_tone_detect_name(HAL_TONE_DET_FAX_TIMEOUT),
		      "FAX_TIMEOUT"));
	CHECK(!strcmp(hal_tone_detect_name(HAL_TONE_DET_MODEM), "MODEM"));
}

/* --------------------------------------------------------------- routing */

static void routing_tests(void)
{
	struct hal_event ev;

	CASE("dispatch routing: the line events reach the line shell");
	env_reset();
	CHECK_UINT(route_probe(HAL_EVENT_HOOK), 1);
	CHECK_UINT(route_probe(HAL_EVENT_DIGIT), 1);
	CHECK_UINT(route_probe(HAL_EVENT_TONE), 1);
	CHECK_UINT(route_probe(HAL_EVENT_RING_START), 1);
	CHECK_UINT(route_probe(HAL_EVENT_RING_END), 1);
	CHECK_UINT(route_probe(HAL_EVENT_T38), 1);
	/* a zeroed payload carries HAL_LINE_DOWN, the dead line record */
	CHECK_UINT(route_probe(HAL_EVENT_LINE), 1);
	CHECK_UINT(exit_requests, 0);

	/*
	 * HAL_EVENT_LINE is not among them: the arm above hands it to the line
	 * shell under the udsp backend, which route_probe() leaves selected.
	 */
	CASE("dispatch routing: the traced types reach the line shell not at all");
	env_reset();
	CHECK_UINT(route_probe(HAL_EVENT_CID_SENT), 0);
	CHECK_UINT(route_probe(HAL_EVENT_NONE), 0);
	CHECK_UINT(route_probe(HAL_EVENT_JOB_DONE), 0);
	/* the disabler moves no call state, so its record is traced and goes
	 * no further */
	CHECK_UINT(route_probe(HAL_EVENT_LEC_POLICY), 0);
	CHECK_UINT(exit_requests, 0);

	CASE("dispatch routing: an unknown type reaches the default trace alone");
	env_reset();
	trace_capture_begin();
	ev = ev_make(4242, 1, 2);
	event_dispatch(&ev);
	CHECK_UINT(line_events, 0);
	CHECK_UINT(exit_requests, 0);
	CHECK(jb_all_clear());
	CHECK_UINT(trace_default_count(4242), 1);
	trace_capture_end();

	/* the line shell holds the ringback gate of the early stream and keys
	 * it by stream, so the arm has to carry both fields across */
	CASE("the first received packet reaches the line shell");
	env_reset();
	ev = ev_make(HAL_EVENT_RTP_FIRST_RX, 0, 1);
	event_dispatch(&ev);
	CHECK_UINT(line_events, 1);
	CHECK_UINT(line_last.type, HAL_EVENT_RTP_FIRST_RX);
	CHECK_UINT(line_last.stream, 1);

	CASE("dispatch routing: the record travels to the line shell unchanged");
	env_reset();
	ev = ev_make(HAL_EVENT_DIGIT, 1, 2);
	ev.seq = 77;
	ev.stamp_ms = 12345;
	ev.u.digit.digit = '7';
	ev.u.digit.remote = 1;
	ev.u.digit.duration_ms = 90;
	event_dispatch(&ev);
	CHECK_UINT(line_events, 1);
	CHECK(!memcmp(&line_last, &ev, sizeof(ev)));
}

/* ------------------------------------------------- the trace ownership rule */

/*
 * Two sources own one domain (trace.h), and the order of the
 * two decides what an operator sees. Every case below is one of those orders,
 * written as the sequence a person would perform it in.
 *
 * The hook is the other half: media_trace.c acts on it, so a hook that fired
 * on a no-op would call the HAL on every unrelated domain toggle, and one that
 * missed a real change would leave the backend armed after the domain went
 * off. It is counted per domain here.
 */
static unsigned int hook_calls;
static unsigned int hook_on;
static unsigned int hook_off;
static enum trace_domain hook_last_dom;
static bool hook_last_on;

static void trace_hook_count(enum trace_domain dom, bool on)
{
	hook_calls++;
	hook_last_dom = dom;
	hook_last_on = on;
	if (on)
		hook_on++;
	else
		hook_off++;
}

static void hook_reset(void)
{
	hook_calls = 0;
	hook_on = 0;
	hook_off = 0;
}

/* Every domain off through the API that clears both masks, so a case starts
 * from a known state whatever the one before it left. */
static void domains_clear(void)
{
	int dom;

	for (dom = 0; dom < TRACE_DOMAIN_MAX; dom++)
		trace_domain_set((enum trace_domain)dom, false);
}

static void trace_hook_tests(void)
{
	trace_hook_set(trace_hook_count);
	domains_clear();
	hook_reset();

	CASE("the hook fires once for a domain that really went on");
	trace_domain_set(TRACE_JB, true);
	CHECK_UINT(hook_calls, 1);
	CHECK_UINT(hook_on, 1);
	CHECK_UINT(hook_last_dom, TRACE_JB);
	CHECK(hook_last_on);
	CHECK(trace_domain_get(TRACE_JB));

	CASE("a set that changes nothing fires nothing");
	hook_reset();
	trace_domain_set(TRACE_JB, true);
	CHECK_UINT(hook_calls, 0);

	CASE("the hook fires once for a domain that really went off");
	hook_reset();
	trace_domain_set(TRACE_JB, false);
	CHECK_UINT(hook_calls, 1);
	CHECK_UINT(hook_off, 1);
	CHECK(!trace_domain_get(TRACE_JB));

	CASE("a clear that changes nothing fires nothing");
	hook_reset();
	trace_domain_set(TRACE_JB, false);
	CHECK_UINT(hook_calls, 0);

	CASE("a uci list fires one call per domain it moves");
	hook_reset();
	trace_uci_apply("media,jb");
	CHECK_UINT(hook_calls, 2);
	CHECK_UINT(hook_on, 2);
	CHECK(trace_domain_get(TRACE_MEDIA));
	CHECK(trace_domain_get(TRACE_JB));

	CASE("the same uci list again fires nothing");
	hook_reset();
	trace_uci_apply("media,jb");
	CHECK_UINT(hook_calls, 0);

	CASE("a uci list that drops a domain takes it off and says so once");
	hook_reset();
	trace_uci_apply("media");
	CHECK_UINT(hook_calls, 1);
	CHECK_UINT(hook_off, 1);
	CHECK(trace_domain_get(TRACE_MEDIA));
	CHECK(!trace_domain_get(TRACE_JB));

	trace_hook_set(NULL);
	domains_clear();
}

static void trace_ownership_tests(void)
{
	domains_clear();

	CASE("uci on, then ubus on, then a uci list without it: off");
	trace_uci_apply("jb");
	CHECK(trace_domain_get(TRACE_JB));
	trace_domain_set(TRACE_JB, true);
	CHECK(trace_domain_get(TRACE_JB));
	trace_uci_apply("");
	/* the ubus call set the runtime bit, and only an explicit off clears
	 * that; the uci list owns its own mask alone */
	CHECK(trace_domain_get(TRACE_JB));
	trace_domain_set(TRACE_JB, false);
	CHECK(!trace_domain_get(TRACE_JB));

	CASE("ubus on, then a uci apply of an unrelated list: stays on");
	domains_clear();
	trace_domain_set(TRACE_MEDIA, true);
	trace_uci_apply("sip,fsm");
	CHECK(trace_domain_get(TRACE_MEDIA));
	CHECK(trace_domain_get(TRACE_SIP));
	CHECK(trace_domain_get(TRACE_FSM));

	CASE("uci on, then ubus off: off, and the same uci apply turns it on again");
	domains_clear();
	trace_uci_apply("jb");
	CHECK(trace_domain_get(TRACE_JB));
	trace_domain_set(TRACE_JB, false);
	CHECK(!trace_domain_get(TRACE_JB));
	trace_uci_apply("jb");
	CHECK(trace_domain_get(TRACE_JB));

	CASE("a -d domain is runtime, so a uci list without it keeps it on");
	domains_clear();
	CHECK_UINT(trace_domains_parse("media"), 1);
	CHECK(trace_domain_get(TRACE_MEDIA));
	trace_uci_apply("sip");
	CHECK(trace_domain_get(TRACE_MEDIA));
	CHECK(trace_domain_get(TRACE_SIP));

	CASE("a uci list of all names every domain");
	domains_clear();
	trace_uci_apply("all");
	{
		int dom;

		for (dom = 0; dom < TRACE_DOMAIN_MAX; dom++)
			CHECK(trace_domain_get((enum trace_domain)dom));
	}
	trace_uci_apply(NULL);
	CHECK(!trace_domain_get(TRACE_JB));
	CHECK(!trace_domain_get(TRACE_SIP));

	CASE("an unknown name costs its own domain and not the list beside it");
	domains_clear();
	trace_uci_apply("media,nosuchdomain,jb");
	CHECK(trace_domain_get(TRACE_MEDIA));
	CHECK(trace_domain_get(TRACE_JB));

	CASE("the two media domains carry the names the northbound uses");
	CHECK(!strcmp(trace_domain_name(TRACE_MEDIA), "media"));
	CHECK(!strcmp(trace_domain_name(TRACE_JB), "jb"));
	CHECK_UINT(trace_domain_find("media"), TRACE_MEDIA);
	CHECK_UINT(trace_domain_find("jb"), TRACE_JB);

	domains_clear();
}

/* --------------------------------------------------- the call trace notice */

/*
 * Which record kinds reach an armed call trace. The dispatch asks for a
 * snapshot on the six kinds that name a moment of the media or the line an
 * investigator reads the counters against, and on no other: a completion
 * names no line at all, and a hook or a digit is already reported by the
 * state change the machine makes from it.
 */
static void calltrace_note_one(uint16_t type, unsigned int line,
			       const char *word)
{
	struct hal_event ev;

	env_reset();
	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.line = line;
	trace_capture_begin();
	event_dispatch(&ev);
	trace_capture_end();
	CHECK_UINT(calltrace_events, 1);
	CHECK_UINT(calltrace_line, line);
	CHECK(!strcmp(calltrace_reason, word));
}

static void calltrace_note_none(uint16_t type)
{
	struct hal_event ev;

	env_reset();
	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.line = 1;
	trace_capture_begin();
	event_dispatch(&ev);
	trace_capture_end();
	CHECK_UINT(calltrace_events, 0);
}

static void calltrace_note_tests(void)
{
	CASE("the first received packet asks for a snapshot on its line");
	calltrace_note_one(HAL_EVENT_RTP_FIRST_RX, 1, "rtp_first_rx");

	CASE("and so does the first transmitted one");
	calltrace_note_one(HAL_EVENT_RTP_FIRST_TX, 0, "rtp_first_tx");

	CASE("a detected tone asks for one");
	calltrace_note_one(HAL_EVENT_TONE, 0, "tone");

	CASE("an image session record asks for one");
	calltrace_note_one(HAL_EVENT_T38, 1, "t38");

	CASE("a canceller policy move asks for one");
	calltrace_note_one(HAL_EVENT_LEC_POLICY, 0, "lec_policy");

	CASE("a slic fault asks for one");
	calltrace_note_one(HAL_EVENT_SLIC_FAULT, 1, "slic_fault");

	CASE("a job completion names no line, so it asks for nothing");
	calltrace_note_none(HAL_EVENT_JOB_DONE);

	CASE("a hook record asks for nothing: its state change reports it");
	calltrace_note_none(HAL_EVENT_HOOK);

	CASE("and a digit record asks for nothing either");
	calltrace_note_none(HAL_EVENT_DIGIT);
}

int main(void)
{
	/*
	 * The gap warnings of the 511-loss replay would otherwise land on the
	 * runner's stderr. ULOG_SYSLOG is the ctest default the trace capture
	 * also restores.
	 */
	ulog_open(ULOG_SYSLOG, LOG_DAEMON, "test_dispatch");

	seq_walk_tests();
	t_hook_resync();
	fault_gate_tests();
	line_state_tests();
	stop_suppression_tests();
	jb_arm_tests();
	lec_policy_tests();
	tone_name_tests();
	routing_tests();
	calltrace_note_tests();
	trace_hook_tests();
	trace_ownership_tests();

	trace_capture_end();

	return tu_summary("test_dispatch");
}
