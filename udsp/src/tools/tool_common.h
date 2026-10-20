// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The option, configuration and engine layer that udsp-cli and udsp-linecap
 * share. Each option that both tools accept is declared here once, with its
 * range and its refusal text. A tool adds its own options, and it applies
 * --region and --duration itself, because their meaning differs between the
 * tools. The engine part opens, starts, stops and closes a libudsp handle,
 * and waits for its events and jobs, with one set of rules for both tools.
 *
 * A tool defines TOOL_PROG, its program name, before it includes this file,
 * and every message here starts with that name. The functions are static
 * inline: test_cli includes udsp-cli.c whole, and test_linecap builds its own
 * copy of udsp-linecap.c, so a shared object file would link twice.
 */
#ifndef UDSP_TOOL_COMMON_H
#define UDSP_TOOL_COMMON_H

#ifndef TOOL_PROG
#error "define TOOL_PROG to the program name before tool_common.h"
#endif

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "udsp/event.h"
#include "udsp/udsp.h"

#include "../core/udsp_priv.h"
#include "tool_names.h"

#define TOOL_PREFILL_MAX	16
#define TOOL_CPU_MAX		255
#define TOOL_PRIO_MIN		0	/* 0 = no-RT test mode */
#define TOOL_PRIO_MAX		99
#define TOOL_CID_AT_RING_MAX	7
#define TOOL_CID_AT_RING_DEFAULT	1
#define TOOL_CID_WAIT_MAX_MS	5000
#define TOOL_CID_WAIT_DEFAULT_MS	500
/* The file back end runs its virtual clock as fast as the CPU allows, so a
 * file-backed run is always bounded and its playback file stays a known size;
 * only the paced ALSA back ends run until a signal. */
#define TOOL_FILE_SECONDS	600
/* The tick net of a file-backed run. The virtual clock also runs while the
 * slic thread starts and stops the lines in real time, so those steps cost
 * virtual seconds that depend on the host. The net is twice the longest run,
 * so a run of TOOL_FILE_SECONDS still ends on its own deadline. */
#define TOOL_FILE_NET_SECONDS	(2 * TOOL_FILE_SECONDS)
/* The tick is one 10 ms period at every line rate (tdm.h Geometry). */
#define TOOL_TICKS_PER_SEC	100
#define TOOL_JOB_WAIT_MS	30000
#define TOOL_POLL_MS		10
/* GR-506-CORE power ringing, the cadence of a ring without a region. */
#define TOOL_RING_ON_MS		2000
#define TOOL_RING_OFF_MS	4000

/* getopt_long values of the shared options. A tool numbers its own options
 * from TOOL_OPT_LOCAL. */
enum tool_opt {
	TOOL_OPT_TDM_BACKEND = 256,
	TOOL_OPT_PLAYBACK,
	TOOL_OPT_CAPTURE,
	TOOL_OPT_SLIC_PLUGIN,
	TOOL_OPT_SPIDEV,
	TOOL_OPT_GPIO_RESET,
	TOOL_OPT_GPIO_INTERRUPT,
	TOOL_OPT_PORTS,
	TOOL_OPT_CHANNELS,
	TOOL_OPT_PREFILL,
	TOOL_OPT_RATE,
	TOOL_OPT_CPU,
	TOOL_OPT_PRIO,
	TOOL_OPT_REGION,
	TOOL_OPT_LINE,
	TOOL_OPT_CID_NAME,
	TOOL_OPT_CID_NUMBER,
	TOOL_OPT_CID_FORMAT,
	TOOL_OPT_CID_AT_RING,
	TOOL_OPT_CID_WAIT_MS,
	TOOL_OPT_MWI,
	TOOL_OPT_DURATION,
	TOOL_OPT_LOCAL,
};

/* The bit of an option in struct tool_args given. */
#define TOOL_BIT(opt)	(1ull << ((opt) - TOOL_OPT_TDM_BACKEND))

/* The getopt_long entries of the shared options. A tool's table starts with
 * these, adds its own entries and ends with the terminator. */
#define TOOL_OPTIONS \
	{ "tdm-backend", required_argument, NULL, TOOL_OPT_TDM_BACKEND }, \
	{ "playback", required_argument, NULL, TOOL_OPT_PLAYBACK }, \
	{ "capture", required_argument, NULL, TOOL_OPT_CAPTURE }, \
	{ "slic-plugin", required_argument, NULL, TOOL_OPT_SLIC_PLUGIN }, \
	{ "spidev", required_argument, NULL, TOOL_OPT_SPIDEV }, \
	{ "gpio-reset", required_argument, NULL, TOOL_OPT_GPIO_RESET }, \
	{ "gpio-interrupt", required_argument, NULL, \
	  TOOL_OPT_GPIO_INTERRUPT }, \
	{ "ports", required_argument, NULL, TOOL_OPT_PORTS }, \
	{ "channels", required_argument, NULL, TOOL_OPT_CHANNELS }, \
	{ "prefill", required_argument, NULL, TOOL_OPT_PREFILL }, \
	{ "rate", required_argument, NULL, TOOL_OPT_RATE }, \
	{ "cpu", required_argument, NULL, TOOL_OPT_CPU }, \
	{ "prio", required_argument, NULL, TOOL_OPT_PRIO }, \
	{ "region", required_argument, NULL, TOOL_OPT_REGION }, \
	{ "line", required_argument, NULL, TOOL_OPT_LINE }, \
	{ "cid-name", required_argument, NULL, TOOL_OPT_CID_NAME }, \
	{ "cid-number", required_argument, NULL, TOOL_OPT_CID_NUMBER }, \
	{ "cid-format", required_argument, NULL, TOOL_OPT_CID_FORMAT }, \
	{ "cid-at-ring", required_argument, NULL, TOOL_OPT_CID_AT_RING }, \
	{ "cid-wait-ms", required_argument, NULL, TOOL_OPT_CID_WAIT_MS }, \
	{ "mwi", required_argument, NULL, TOOL_OPT_MWI }, \
	{ "duration", required_argument, NULL, TOOL_OPT_DURATION }, \
	{ "help", no_argument, NULL, 'h' },

/* The arguments both tools carry. A tool's own argument struct holds this as
 * its first member, so the apply hook can convert the pointer back. */
struct tool_args {
	struct udsp_config cfg;
	uint64_t given;			/* TOOL_BIT of each option given */
	uint8_t line;
	uint8_t cid_format;
	uint8_t cid_at_ring;
	uint16_t cid_wait_ms;
	int mwi;			/* -1 not given, 0 off, 1 on */
	char cid_name[UDSP_CID_STR_MAX];
	char cid_number[UDSP_CID_STR_MAX];
};

static inline bool tool_given(const struct tool_args *t, int opt)
{
	return (t->given & TOOL_BIT(opt)) != 0;
}

static inline bool tool_cid_given(const struct tool_args *t)
{
	return tool_given(t, TOOL_OPT_CID_NAME) ||
	       tool_given(t, TOOL_OPT_CID_NUMBER);
}

/* Copies src whole or leaves dst as it is. */
static inline bool tool_str_set(char *dst, size_t size, const char *src)
{
	size_t len = strlen(src);

	if (len >= size)
		return false;
	memcpy(dst, src, len + 1u);

	return true;
}

static inline void tool_log(void *ctx, enum udsp_log_level level,
			    const char *msg)
{
	static const char *const names[] = {
		[UDSP_LOG_ERR] = "error",
		[UDSP_LOG_WARN] = "warning",
		[UDSP_LOG_INFO] = "info",
		[UDSP_LOG_DEBUG] = "debug",
	};

	fprintf(stderr, TOOL_PROG ": %s: %s\n",
		(unsigned)level <= UDSP_LOG_DEBUG ? names[level] : "log", msg);
}

/* Board defaults of udsp.h, with the TDM back end and the device names that
 * uvoiced uses. The ALSA names are the ones of MT7987. The tool sets the
 * populated ports, the pre-fill and the region. */
static inline void tool_args_defaults(struct tool_args *t)
{
	struct udsp_config *cfg = &t->cfg;

	memset(t, 0, sizeof(*t));
	cfg->log = tool_log;
	tool_str_set(cfg->tdm_backend, sizeof(cfg->tdm_backend), "alsa-raw");
	tool_str_set(cfg->tdm_playback, sizeof(cfg->tdm_playback),
		     "plughw:0,0");
	tool_str_set(cfg->tdm_capture, sizeof(cfg->tdm_capture), "plughw:0,1");
	tool_str_set(cfg->spidev, sizeof(cfg->spidev), TOOL_SPIDEV_DEFAULT);
	tool_str_set(cfg->gpio_reset, sizeof(cfg->gpio_reset), "slic_reset");
	tool_str_set(cfg->gpio_interrupt, sizeof(cfg->gpio_interrupt),
		     "slic_interrupt");
	cfg->channels = 2;
	cfg->engine_cpu = -1;
	cfg->engine_prio = 50;
	t->cid_format = UDSP_CIDF_BELLCORE_FSK;
	t->cid_at_ring = TOOL_CID_AT_RING_DEFAULT;
	t->cid_wait_ms = TOOL_CID_WAIT_DEFAULT_MS;
	t->mwi = -1;
}

static inline bool tool_opt_text(const struct option *o, const char *val,
				 char *dst, size_t size)
{
	if (tool_str_set(dst, size, val))
		return true;
	fprintf(stderr, TOOL_PROG ": --%s: value too long\n", o->name);

	return false;
}

static inline bool tool_opt_number(const struct option *o, const char *val,
				   long min, long max, long *n)
{
	if (tool_num_parse(val, min, max, n))
		return true;
	fprintf(stderr, TOOL_PROG ": --%s: expected %ld..%ld, got \"%s\"\n",
		o->name, min, max, val);

	return false;
}

/* Applies a shared option other than --region and --duration. */
static inline bool tool_opt_apply(struct tool_args *t, const struct option *o,
				  const char *val)
{
	struct udsp_config *cfg = &t->cfg;
	long n = 0;

	switch (o->val) {
	case TOOL_OPT_TDM_BACKEND:
		if (strcmp(val, "alsa") && strcmp(val, "alsa-raw") &&
		    strcmp(val, "file")) {
			fprintf(stderr, TOOL_PROG ": --tdm-backend: "
				"alsa, alsa-raw or file\n");
			return false;
		}
		return tool_opt_text(o, val, cfg->tdm_backend,
				     sizeof(cfg->tdm_backend));
	case TOOL_OPT_PLAYBACK:
		return tool_opt_text(o, val, cfg->tdm_playback,
				     sizeof(cfg->tdm_playback));
	case TOOL_OPT_CAPTURE:
		return tool_opt_text(o, val, cfg->tdm_capture,
				     sizeof(cfg->tdm_capture));
	case TOOL_OPT_SLIC_PLUGIN:
		return tool_opt_text(o, val, cfg->slic_plugin,
				     sizeof(cfg->slic_plugin));
	case TOOL_OPT_SPIDEV:
		return tool_opt_text(o, val, cfg->spidev, sizeof(cfg->spidev));
	case TOOL_OPT_GPIO_RESET:
		return tool_opt_text(o, val, cfg->gpio_reset,
				     sizeof(cfg->gpio_reset));
	case TOOL_OPT_GPIO_INTERRUPT:
		return tool_opt_text(o, val, cfg->gpio_interrupt,
				     sizeof(cfg->gpio_interrupt));
	case TOOL_OPT_PORTS:
		if (!tool_opt_number(o, val, 1, UDSP_LINE_MAX, &n))
			return false;
		cfg->populated_ports = (uint8_t)n;
		return true;
	case TOOL_OPT_CHANNELS:
		if (!tool_opt_number(o, val, 1, 2, &n))
			return false;
		cfg->channels = (uint8_t)n;
		return true;
	case TOOL_OPT_PREFILL:
		if (!tool_opt_number(o, val, 1, TOOL_PREFILL_MAX, &n))
			return false;
		cfg->prefill_periods = (uint8_t)n;
		return true;
	case TOOL_OPT_RATE:
		/* The two line rates udsp_open admits (udsp.h sample_rate). A
		 * 16 kHz ask that the board refuses degrades inside udsp_open,
		 * so the run continues at 8 kHz and the capability record says
		 * which line it got. */
		if (!tool_opt_number(o, val, 8000, 16000, &n))
			return false;
		if (n != 8000 && n != 16000) {
			fprintf(stderr, TOOL_PROG ": --rate: 8000 or 16000\n");
			return false;
		}
		cfg->sample_rate = (uint16_t)n;
		return true;
	case TOOL_OPT_CPU:
		if (!tool_opt_number(o, val, -1, TOOL_CPU_MAX, &n))
			return false;
		cfg->engine_cpu = (int)n;
		return true;
	case TOOL_OPT_PRIO:
		if (!tool_opt_number(o, val, TOOL_PRIO_MIN, TOOL_PRIO_MAX, &n))
			return false;
		cfg->engine_prio = (int)n;
		return true;
	case TOOL_OPT_LINE:
		if (!tool_opt_number(o, val, 0, UDSP_LINE_MAX - 1, &n))
			return false;
		t->line = (uint8_t)n;
		return true;
	case TOOL_OPT_CID_NAME:
		return tool_opt_text(o, val, t->cid_name, sizeof(t->cid_name));
	case TOOL_OPT_CID_NUMBER:
		return tool_opt_text(o, val, t->cid_number,
				     sizeof(t->cid_number));
	case TOOL_OPT_CID_FORMAT:
		if (tool_cid_format_parse(val, &t->cid_format))
			return true;
		fprintf(stderr, TOOL_PROG ": --cid-format: unknown format "
			"\"%s\"\n", val);
		return false;
	case TOOL_OPT_CID_AT_RING:
		if (!tool_opt_number(o, val, 0, TOOL_CID_AT_RING_MAX, &n))
			return false;
		t->cid_at_ring = (uint8_t)n;
		return true;
	case TOOL_OPT_CID_WAIT_MS:
		if (!tool_opt_number(o, val, 0, TOOL_CID_WAIT_MAX_MS, &n))
			return false;
		t->cid_wait_ms = (uint16_t)n;
		return true;
	case TOOL_OPT_MWI:
		if (!strcmp(val, "on") || !strcmp(val, "off")) {
			t->mwi = !strcmp(val, "on");
			return true;
		}
		fprintf(stderr, TOOL_PROG ": --mwi: on or off\n");
		return false;
	}

	return false;
}

/* getopt_long takes the next argument as the value even when that argument is
 * another flag, so "--playback --dump" would set the path to "--dump" and lose
 * the flag without a word. A separate value that starts with a dash and a
 * non-digit is therefore an error; a negative number such as --cpu -1 is not.
 * A genuine dash value is passed as --name=value, which getopt hands back as
 * a pointer into the option element instead of the next element, so the two
 * forms stay distinguishable here. */
static inline bool tool_value_sane(const char *val, char **argv)
{
	if (!val || val[0] != '-' || isdigit((unsigned char)val[1]))
		return true;

	return optind >= 1 && val != argv[optind - 1];
}

enum tool_parse { TOOL_PARSE_OK, TOOL_PARSE_ERROR, TOOL_PARSE_HELP };

/* A tool's option hook: its own options, then tool_opt_apply(). */
typedef bool (*tool_apply_fn)(struct tool_args *t, const struct option *o,
			      const char *val);

/* Parses the options from optind on. Each error prints one reason line; the
 * tool prints its usage text after it. glibc sets the long index only for a
 * long option it matched, so -h and ? return before the index is read. */
static inline enum tool_parse tool_args_parse(int argc, char **argv,
					      const struct option *opts,
					      struct tool_args *t,
					      tool_apply_fn apply)
{
	int longindex = -1;
	int opt;

	while ((opt = getopt_long(argc, argv, "h", opts, &longindex)) != -1) {
		const struct option *o;

		if (opt == 'h')
			return TOOL_PARSE_HELP;
		if (opt == '?') {
			fprintf(stderr, TOOL_PROG ": bad option\n");
			return TOOL_PARSE_ERROR;
		}
		o = &opts[longindex];
		if (!tool_value_sane(optarg, argv)) {
			fprintf(stderr, TOOL_PROG ": value looks like a flag, "
				"use --name=value: %s\n", o->name);
			return TOOL_PARSE_ERROR;
		}
		if (!apply(t, o, optarg))
			return TOOL_PARSE_ERROR;
		t->given |= TOOL_BIT(opt);
	}
	if (optind < argc) {
		fprintf(stderr, TOOL_PROG ": operands are not accepted: %s\n",
			argv[optind]);
		return TOOL_PARSE_ERROR;
	}

	return TOOL_PARSE_OK;
}

static inline bool tool_backend_is_file(const struct udsp_config *cfg)
{
	return !strcmp(cfg->tdm_backend, "file");
}

/* The board defaults name ALSA devices. The file back end takes paths instead,
 * and an unnamed path is its ramp generator (capture) or no output file
 * (playback), which is what a bare file-backed run wants. */
static inline void tool_file_defaults(struct tool_args *t)
{
	if (!tool_backend_is_file(&t->cfg))
		return;
	if (!tool_given(t, TOOL_OPT_PLAYBACK))
		t->cfg.tdm_playback[0] = '\0';
	if (!tool_given(t, TOOL_OPT_CAPTURE))
		t->cfg.tdm_capture[0] = '\0';
}

/* The fixed net can create a playback file for TOOL_FILE_NET_SECONDS virtual
 * seconds when an event deadline does not stop the run. */
static inline bool tool_file_ticks_set(const struct udsp_config *cfg)
{
	char ticks[32];

	if (!tool_backend_is_file(cfg))
		return true;
	snprintf(ticks, sizeof(ticks), "%d",
		 TOOL_FILE_NET_SECONDS * TOOL_TICKS_PER_SEC);
	if (!setenv("UDSP_MAX_TICKS", ticks, 1))
		return true;
	fprintf(stderr, TOOL_PROG ": UDSP_MAX_TICKS: %s\n", strerror(errno));

	return false;
}

static inline void tool_cid_fill(const struct tool_args *t,
				 struct udsp_cid *cid)
{
	memset(cid, 0, sizeof(*cid));
	cid->format = t->cid_format;
	if (t->mwi >= 0) {
		cid->mwi = t->mwi != 0;
		return;
	}
	tool_str_set(cid->name, sizeof(cid->name), t->cid_name);
	tool_str_set(cid->number, sizeof(cid->number), t->cid_number);
	/* Keep "O" out of the MWI branch: an MWI off record is identified by an
	 * all-empty identity, blockinfo included. */
	tool_str_set(cid->blockinfo, sizeof(cid->blockinfo), "O");
}

static inline bool tool_status_ok(const char *op, enum udsp_status st)
{
	if (st == UDSP_OK)
		return true;
	fprintf(stderr, TOOL_PROG ": %s: %s\n", op, udsp_status_name(st));

	return false;
}

/* --- the engine ---------------------------------------------------------- */

static volatile sig_atomic_t tool_stopped;

static inline void tool_signal(int sig)
{
	tool_stopped = 1;
}

static inline void tool_signals_install(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = tool_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

struct tool_engine;

/* A tool's event hook; it calls tool_event_record() for the shared part. */
typedef void (*tool_record_fn)(struct tool_engine *e,
			       const struct udsp_event *ev);

/* A libudsp handle and its event socket. A tool's own engine struct holds
 * this as its first member, so the event hook can convert the pointer
 * back. */
struct tool_engine {
	udsp_t *s;
	int event_fd;
	int library_fd;
	bool file_backend;
	bool started;
	bool faulted;
	tool_record_fn record;		/* NULL: tool_event_record() */
};

enum tool_wait {
	TOOL_WAIT_GOT,
	TOOL_WAIT_EXPIRED,		/* the event-clock deadline */
	TOOL_WAIT_WALL_EXPIRED,		/* the host-clock bound */
	TOOL_WAIT_STOPPED,
	TOOL_WAIT_ERROR,
};

enum tool_recv {
	TOOL_RECV_GOT,
	TOOL_RECV_SHORT,
	TOOL_RECV_EMPTY,
	TOOL_RECV_ERROR,
};

static inline void tool_engine_init(struct tool_engine *e,
				    tool_record_fn record)
{
	memset(e, 0, sizeof(*e));
	e->event_fd = -1;
	e->library_fd = -1;
	e->record = record;
}

static inline void tool_event_print(const struct udsp_event *ev)
{
	printf("ev %u %u %s line=%u", ev->seq, ev->stamp_ms,
	       tool_event_name(ev->type), ev->line);
	if (!strcmp(tool_event_name(ev->type), "UNKNOWN"))
		printf(" type=%u", ev->type);
	if (ev->stream)
		printf(" stream=%u", ev->stream);

	switch (ev->type) {
	case UDSP_EV_HOOK:
		printf(" state=%s pulses=%u", tool_hook_name(ev->u.hook.state),
		       ev->u.hook.pulses);
		break;
	case UDSP_EV_DIGIT:
		printf(" digit=%c remote=%u duration_ms=%u", ev->u.digit.digit,
		       ev->u.digit.remote, ev->u.digit.duration_ms);
		break;
	case UDSP_EV_TONE:
		printf(" code=%s direction=%s duration_ms=%u edge=%s",
		       tool_tone_detect_name(ev->u.tone.code),
		       tool_tone_direction_name(ev->u.tone.direction),
		       ev->u.tone.duration_ms,
		       ev->u.tone.end ? "end" : "start");
		break;
	case UDSP_EV_LINE:
		printf(" state=%s ring_count=%u",
		       tool_line_state_name(ev->u.line.state),
		       ev->u.line.ring_count);
		break;
	case UDSP_EV_RING_START:
		printf(" ring_count=%u", ev->u.line.ring_count);
		break;
	case UDSP_EV_RING_END:
		printf(" state=%s ring_count=%u",
		       tool_ring_reason_name(ev->u.line.state),
		       ev->u.line.ring_count);
		break;
	case UDSP_EV_CID_SENT:
		printf(" kind=%s status=%d",
		       ev->u.cid_sent.kind == UDSP_CID_TYPE2 ?
		       "TYPE2" : "TYPE1", ev->u.cid_sent.status);
		break;
	case UDSP_EV_CID_RECV:
		printf(" number=\"%.32s\" name=\"%.32s\" datetime=\"%.32s\"",
		       ev->u.cid.number, ev->u.cid.name, ev->u.cid.datetime);
		break;
	case UDSP_EV_JB_UPDATE:
		printf(" total=%llu drop=%llu size_ms=%u jitter_avg=%u",
		       (unsigned long long)ev->u.jb.total,
		       (unsigned long long)ev->u.jb.drop, ev->u.jb.size_ms,
		       ev->u.jb.jitter_avg);
		break;
	case UDSP_EV_T38:
		printf(" state=%u payload_type=%u", ev->u.t38.state,
		       ev->u.t38.payload_type);
		break;
	case UDSP_EV_SLIC_FAULT:
		printf(" code=0x%08x", ev->u.fault.code);
		break;
	case UDSP_EV_JOB_DONE:
		printf(" token=%u status=%d", ev->u.job.token,
		       ev->u.job.status);
		break;
	case UDSP_EV_LEC_POLICY:
		printf(" kind=%s reason=%s tone=%s direction=%s",
		       tool_passthrough_name(ev->u.lec.kind),
		       tool_lec_reason_name(ev->u.lec.reason),
		       tool_tone_detect_name(ev->u.lec.tone),
		       tool_tone_direction_name(ev->u.lec.direction));
		break;
	default:
		break;
	}

	putchar('\n');
	fflush(stdout);
}

/* Prints the event. A nonzero SLIC fault code after the start marks the run
 * faulted; a clear (code 0) does not. */
static inline void tool_event_record(struct tool_engine *e,
				     const struct udsp_event *ev)
{
	tool_event_print(ev);
	if (ev->type == UDSP_EV_SLIC_FAULT && e->started && ev->u.fault.code)
		e->faulted = true;
}

static inline enum tool_recv tool_event_recv(struct tool_engine *e,
					     struct udsp_event *ev)
{
	ssize_t n = recv(e->event_fd, ev, sizeof(*ev), MSG_DONTWAIT);

	if (n == (ssize_t)sizeof(*ev)) {
		if (e->record)
			e->record(e, ev);
		else
			tool_event_record(e, ev);
		return TOOL_RECV_GOT;
	}
	if (n >= 0) {
		fprintf(stderr, TOOL_PROG ": short event record: %zd\n", n);
		return TOOL_RECV_SHORT;
	}
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
		return TOOL_RECV_EMPTY;
	fprintf(stderr, TOOL_PROG ": event receive failed: %s\n",
		strerror(errno));

	return TOOL_RECV_ERROR;
}

/* deadline is on the event clock and wall on the host clock; 0 means none.
 * A signal comes first, so a queued flood of events cannot hide it. */
static inline enum tool_wait tool_event_wait(struct tool_engine *e,
					     uint32_t deadline, uint64_t wall,
					     bool stop_on_signal,
					     struct udsp_event *ev)
{
	struct pollfd pfd = { .fd = e->event_fd, .events = POLLIN };

	for (;;) {
		if (stop_on_signal && tool_stopped)
			return TOOL_WAIT_STOPPED;
		if (deadline && (int32_t)(udsp_now_ms(e->s) - deadline) >= 0)
			return TOOL_WAIT_EXPIRED;
		if (wall && tool_mono_ms() >= wall)
			return TOOL_WAIT_WALL_EXPIRED;
		switch (tool_event_recv(e, ev)) {
		case TOOL_RECV_GOT:
			return TOOL_WAIT_GOT;
		case TOOL_RECV_SHORT:
			continue;
		case TOOL_RECV_ERROR:
			return TOOL_WAIT_ERROR;
		case TOOL_RECV_EMPTY:
			break;
		}
		if (poll(&pfd, 1, TOOL_POLL_MS) < 0 && errno != EINTR) {
			fprintf(stderr, TOOL_PROG ": event poll failed: %s\n",
				strerror(errno));
			return TOOL_WAIT_ERROR;
		}
	}
}

static inline void tool_events_drain(struct tool_engine *e)
{
	struct udsp_event ev;
	enum tool_recv got;

	do
		got = tool_event_recv(e, &ev);
	while (got == TOOL_RECV_GOT || got == TOOL_RECV_SHORT);
}

/* Waits for the JOB_DONE of token. The file back end runs its virtual clock
 * faster than real time, so its event-clock bound is the whole tick net;
 * the wall bound covers an engine that stopped ticking. */
static inline bool tool_job_wait(struct tool_engine *e, const char *op,
				 uint32_t token, bool stop_on_signal)
{
	uint32_t wait_ms = e->file_backend ?
		TOOL_FILE_NET_SECONDS * 1000u : TOOL_JOB_WAIT_MS;
	uint32_t deadline = udsp_now_ms(e->s) + wait_ms;
	uint64_t wall = tool_mono_ms() + TOOL_JOB_WAIT_MS;
	struct udsp_event ev;

	for (;;) {
		enum tool_wait got = tool_event_wait(e, deadline, wall,
						     stop_on_signal, &ev);

		if (got == TOOL_WAIT_EXPIRED) {
			fprintf(stderr, TOOL_PROG ": %s: wait expired awaiting "
				"JOB_DONE token=%u at event deadline %u ms\n",
				op, token, deadline);
			return false;
		}
		if (got == TOOL_WAIT_WALL_EXPIRED) {
			fprintf(stderr, TOOL_PROG ": %s: wall bound expired "
				"awaiting JOB_DONE token=%u after %u ms\n", op,
				token, TOOL_JOB_WAIT_MS);
			return false;
		}
		if (got == TOOL_WAIT_STOPPED) {
			fprintf(stderr, TOOL_PROG ": %s: abandoned by signal\n",
				op);
			return false;
		}
		if (got == TOOL_WAIT_ERROR)
			return false;
		if (ev.type != UDSP_EV_JOB_DONE || ev.u.job.token != token)
			continue;
		if (!ev.u.job.status)
			return true;
		fprintf(stderr, TOOL_PROG ": %s completion failed: %d\n", op,
			ev.u.job.status);
		return false;
	}
}

/* open_hint follows "udsp_open failed" in the message, so a tool can name
 * what it knows about the cause. */
static inline bool tool_engine_open(struct tool_engine *e,
				    const struct udsp_config *cfg,
				    const char *open_hint)
{
	int fd[2];

	e->file_backend = tool_backend_is_file(cfg);
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC,
		       0, fd)) {
		fprintf(stderr, TOOL_PROG ": event socketpair failed: %s\n",
			strerror(errno));
		return false;
	}
	e->event_fd = fd[0];
	e->library_fd = fd[1];
	e->s = udsp_open(cfg, e->library_fd);
	if (e->s)
		return true;
	fprintf(stderr, TOOL_PROG ": udsp_open failed%s\n", open_hint);

	return false;
}

static inline bool tool_engine_start(struct tool_engine *e, unsigned line)
{
	const struct udsp_cap *cap = udsp_cap(e->s);
	struct udsp_job job = { 0 };

	if (!cap) {
		fprintf(stderr, TOOL_PROG ": capability query failed\n");
		return false;
	}
	printf("cap backend=%s slic=%s lines=%u features=0x%08x\n",
	       cap->backend, cap->slic, cap->lines, cap->features);
	if (line >= cap->lines) {
		fprintf(stderr, TOOL_PROG ": --line %u is outside "
			"cap.lines %u\n", line, cap->lines);
		return false;
	}
	if (!tool_status_ok("udsp_start", udsp_start(e->s, &job)))
		return false;
	e->started = true;

	return tool_job_wait(e, "udsp_start", job.token, true) && !e->faulted;
}

/* A signal does not cut the stop wait, so the engine stops before the close
 * whichever way the run ended. */
static inline bool tool_engine_stop(struct tool_engine *e)
{
	struct udsp_job job = { 0 };
	bool ok = true;

	if (!e->s)
		return true;
	if (e->started)
		ok = tool_status_ok("udsp_stop", udsp_stop(e->s, &job)) &&
		     tool_job_wait(e, "udsp_stop", job.token, false);
	tool_events_drain(e);

	return ok;
}

/* started stays set through the last drain, so a late fault still counts. */
static inline void tool_engine_close(struct tool_engine *e)
{
	if (e->s) {
		udsp_close(e->s);
		e->s = NULL;
		tool_events_drain(e);
	}
	if (e->event_fd >= 0)
		close(e->event_fd);
	if (e->library_fd >= 0)
		close(e->library_fd);
	e->event_fd = -1;
	e->library_fd = -1;
}

#endif
