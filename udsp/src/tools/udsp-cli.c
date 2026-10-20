// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * udsp-cli, the lab and bring-up tool. The loopback verb is the vehicle for the
 * real-time gate. The verbs are loopback, tone, ring, cid, dtmf, looptest,
 * events and selftest; the call verb is declared but not implemented.
 *
 * The control verbs drive libudsp through its stable public API. Loopback keeps
 * its own direct engine path, and selftest keeps its direct fake SLIC path.
 *
 * Signal rule. SIGINT succeeds for ring, events, looptest and an unbounded
 * tone. It abandons an active dtmf or cid sequence, which exits 1 with a named
 * reason. Quiet event streams are valid. Wall-clock bounds apply only while
 * the CLI waits for a named job or media completion.
 *
 * Exit codes. 0 means success for loopback, tone, ring, cid, dtmf, looptest,
 * events, and selftest. 1 means an argument, operation, wait, fault, or
 * selftest failure. 2 means the call verb is not implemented.
 */
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "udsp/udsp.h"
#include "udsp/slic.h"

#include "../core/engine.h"
#include "../core/udsp_priv.h"
#include "../dsp/g729.h"
#include "../line/line.h"
#include "../slic/fake.h"

#define TOOL_PROG	"udsp-cli"
#include "tool_common.h"

/* This in-tree diagnostic uses private headers for the event clock, the
 * enum line_ring_reason names and the G.729 table check. None of these
 * dependencies is part of libudsp's API. */

#define CLI_EXIT_OK	0
#define CLI_EXIT_ERROR	1
#define CLI_EXIT_TODO	2

#define CLI_SECONDS_MAX	604800
#define CLI_POLL_MS	20
#define CLI_REGION_MAX	192
/* The manufacturing loop test. The ring cap is the requirement's ceiling, so
 * no run of this verb rings a port for longer than a minute at a time. The
 * delay default is the `delay_ms` default of the uvoice loop test API. */
#define CLI_RING_S_MAX	60
#define CLI_RING_S_DEFAULT 60
#define CLI_DELAY_MS_DEFAULT 500
/* The published hook pair is unset until the slic thread takes its first
 * sample, so the initial read waits this long for it. */
#define CLI_HOOK_READ_MS 2000
/* How long an arm the library refused as transient waits before the verb tries
 * it again. The tick stores the line's ownership in the same journal run that
 * posts the off-hook event, so a retry five ticks
 * later finds the ownership stored. */
#define CLI_LOOP_RETRY_MS 50

enum {
	CLI_OPT_FAX_PLUGIN = TOOL_OPT_LOCAL,
	CLI_OPT_DUMP,
	CLI_OPT_SECONDS,
	CLI_OPT_TONE,
	CLI_OPT_DIGITS,
	CLI_OPT_TYPE2,
	CLI_OPT_VARIANT,
	CLI_OPT_RING_S,
	CLI_OPT_DELAY_MS,
	CLI_OPT_PEER,
	CLI_OPT_DEBUG,
	CLI_OPT_END,
};

_Static_assert(CLI_OPT_END - TOOL_OPT_TDM_BACKEND <= 64,
	       "every option needs a bit of struct tool_args given");

static const struct option cli_options[] = {
	TOOL_OPTIONS
	{ "fax-plugin", required_argument, NULL, CLI_OPT_FAX_PLUGIN },
	{ "dump", no_argument, NULL, CLI_OPT_DUMP },
	{ "seconds", required_argument, NULL, CLI_OPT_SECONDS },
	{ "tone", required_argument, NULL, CLI_OPT_TONE },
	{ "digits", required_argument, NULL, CLI_OPT_DIGITS },
	{ "type2", no_argument, NULL, CLI_OPT_TYPE2 },
	{ "variant", required_argument, NULL, CLI_OPT_VARIANT },
	{ "ring-s", required_argument, NULL, CLI_OPT_RING_S },
	{ "delay-ms", required_argument, NULL, CLI_OPT_DELAY_MS },
	{ "peer", required_argument, NULL, CLI_OPT_PEER },
	{ "debug", no_argument, NULL, CLI_OPT_DEBUG },
	{ NULL, 0, NULL, 0 },
};

struct cli_args;
struct cli_engine;

static bool tone_run(const struct cli_args *a, struct cli_engine *e);
static bool cw_run(const struct cli_args *a, struct cli_engine *e);
static bool dtmf_run(const struct cli_args *a, struct cli_engine *e);
static bool ring_run(const struct cli_args *a, struct cli_engine *e);
static bool cid_run(const struct cli_args *a, struct cli_engine *e);
static bool looptest_run(const struct cli_args *a, struct cli_engine *e);
static bool events_run(const struct cli_args *a, struct cli_engine *e);
static int selftest_local(const struct cli_args *a);
static int region_print_run(const struct cli_args *a);
static int loopback_run(const struct cli_args *a);
static int call_run(const struct cli_args *a);
static bool cid_args_valid(const struct cli_args *a);
static bool ring_args_valid(const struct cli_args *a);

/* The options every verb takes: the board and the plug-ins. */
#define CLI_OPTS_BOARD	(TOOL_BIT(TOOL_OPT_TDM_BACKEND) | \
			 TOOL_BIT(TOOL_OPT_PLAYBACK) | \
			 TOOL_BIT(TOOL_OPT_CAPTURE) | \
			 TOOL_BIT(TOOL_OPT_SLIC_PLUGIN) | \
			 TOOL_BIT(CLI_OPT_FAX_PLUGIN) | \
			 TOOL_BIT(TOOL_OPT_SPIDEV) | \
			 TOOL_BIT(TOOL_OPT_GPIO_RESET) | \
			 TOOL_BIT(TOOL_OPT_GPIO_INTERRUPT) | \
			 TOOL_BIT(TOOL_OPT_PORTS) | \
			 TOOL_BIT(TOOL_OPT_CHANNELS) | \
			 TOOL_BIT(TOOL_OPT_PREFILL) | \
			 TOOL_BIT(TOOL_OPT_RATE) | \
			 TOOL_BIT(TOOL_OPT_CPU) | \
			 TOOL_BIT(TOOL_OPT_PRIO) | \
			 TOOL_BIT(TOOL_OPT_REGION) | \
			 TOOL_BIT(CLI_OPT_DEBUG))
/* The run budget and the line of a verb that acts on one line. */
#define CLI_OPTS_LINE	(TOOL_BIT(CLI_OPT_SECONDS) | TOOL_BIT(TOOL_OPT_LINE))
#define CLI_OPTS_CID	(TOOL_BIT(TOOL_OPT_CID_NAME) | \
			 TOOL_BIT(TOOL_OPT_CID_NUMBER) | \
			 TOOL_BIT(TOOL_OPT_CID_FORMAT))

/* run is the handler of an engine verb under real_run. exec replaces real_run
 * for loopback, which opens the engine directly, and for the verbs that open
 * none. A verb without engine owns its stdout, so main prints no
 * configuration header in front of it. allowed and required are option bits
 * beyond CLI_OPTS_BOARD; valid holds the rules across options. */
struct cli_verb {
	const char *name;
	const char *help;
	bool engine;
	bool (*run)(const struct cli_args *a, struct cli_engine *e);
	int (*exec)(const struct cli_args *a);
	bool (*valid)(const struct cli_args *a);
	uint64_t allowed;
	uint64_t required;
};

static const struct cli_verb cli_verbs[] = {
	{
		.name = "loopback",
		.help = "capture to playback, the real-time gate",
		.engine = true,
		.exec = loopback_run,
		.allowed = TOOL_BIT(CLI_OPT_SECONDS) | TOOL_BIT(CLI_OPT_DUMP),
	},
	{
		.name = "tone",
		.help = "play a region tone on a line",
		.engine = true,
		.run = tone_run,
		.allowed = CLI_OPTS_LINE | TOOL_BIT(CLI_OPT_TONE) |
			   TOOL_BIT(TOOL_OPT_DURATION),
		.required = TOOL_BIT(CLI_OPT_TONE) | TOOL_BIT(TOOL_OPT_REGION),
	},
	{
		.name = "cw",
		.help = "play a call-waiting variant on a line",
		.engine = true,
		.run = cw_run,
		.allowed = CLI_OPTS_LINE | TOOL_BIT(TOOL_OPT_DURATION) |
			   TOOL_BIT(CLI_OPT_VARIANT),
		.required = TOOL_BIT(TOOL_OPT_REGION),
	},
	{
		.name = "ring",
		.help = "ring a line with the region cadence",
		.engine = true,
		.run = ring_run,
		.valid = ring_args_valid,
		.allowed = CLI_OPTS_LINE | CLI_OPTS_CID |
			   TOOL_BIT(TOOL_OPT_CID_AT_RING) |
			   TOOL_BIT(TOOL_OPT_CID_WAIT_MS),
	},
	{
		.name = "cid",
		.help = "send a type 1 or type 2 caller id",
		.engine = true,
		.run = cid_run,
		.valid = cid_args_valid,
		.allowed = CLI_OPTS_LINE | CLI_OPTS_CID |
			   TOOL_BIT(CLI_OPT_TYPE2) | TOOL_BIT(TOOL_OPT_MWI),
		.required = TOOL_BIT(TOOL_OPT_REGION),
	},
	{
		.name = "dtmf",
		.help = "play digits on a line",
		.engine = true,
		.run = dtmf_run,
		.allowed = CLI_OPTS_LINE | TOOL_BIT(CLI_OPT_DIGITS) |
			   TOOL_BIT(TOOL_OPT_DURATION),
		.required = TOOL_BIT(CLI_OPT_DIGITS),
	},
	{
		.name = "call",
		.help = "run a call to a fixed RTP peer",
		.engine = true,
		.exec = call_run,
		.allowed = CLI_OPTS_LINE | CLI_OPTS_CID |
			   TOOL_BIT(CLI_OPT_TONE) | TOOL_BIT(CLI_OPT_DIGITS) |
			   TOOL_BIT(TOOL_OPT_DURATION) |
			   TOOL_BIT(CLI_OPT_TYPE2) | TOOL_BIT(TOOL_OPT_MWI),
	},
	{
		.name = "looptest",
		.help = "ring, then loop a port back to itself or a peer",
		.engine = true,
		.run = looptest_run,
		/* The verb drives every port it finds, so no option names a
		 * line. */
		.allowed = TOOL_BIT(CLI_OPT_SECONDS) | TOOL_BIT(CLI_OPT_RING_S) |
			   TOOL_BIT(CLI_OPT_DELAY_MS) | TOOL_BIT(CLI_OPT_PEER),
	},
	{
		.name = "events",
		.help = "print line and media events",
		.engine = true,
		.run = events_run,
		.allowed = CLI_OPTS_LINE,
	},
	{
		.name = "region",
		.help = "print a parsed region profile",
		.exec = region_print_run,
		.required = TOOL_BIT(TOOL_OPT_REGION),
	},
	{
		.name = "selftest",
		.help = "drive the in-tree fake SLIC through its ops",
		.exec = selftest_local,
	},
};

struct cli_args {
	struct tool_args base;
	const struct cli_verb *verb;
	bool dump;
	int seconds;
	uint16_t duration_ms;
	enum udsp_tone tone;
	uint8_t variant;
	const char *digits;
	bool type2;
	uint8_t ring_s;
	uint16_t delay_ms;
	uint8_t peer;
	bool debug;
	char region[CLI_REGION_MAX];
};

_Static_assert(offsetof(struct cli_args, base) == 0,
	       "opt_apply converts the base pointer back");

static void usage(FILE *out)
{
	fprintf(out, "usage: udsp-cli <verb> [options]\n\nverbs:\n");
	for (size_t i = 0; i < sizeof(cli_verbs) / sizeof(cli_verbs[0]); i++)
		fprintf(out, "  %-10s %s\n", cli_verbs[i].name, cli_verbs[i].help);

	fprintf(out,
		"\noptions:\n"
		"  --tdm-backend <alsa|alsa-raw|file>\n"
		"                              tdm plug-in (default alsa-raw)\n"
		"  --playback <dev|path>       playback device or file\n"
		"  --capture <dev|path>        capture device or file\n"
		"  --slic-plugin <path>        slic .so, empty selects the fake\n"
		"  --fax-plugin <path>         t.38 gateway .so, or none\n"
		"  --spidev <path>             spidev node of the slic\n"
		"                              (default " TOOL_SPIDEV_DEFAULT ")\n"
		"  --gpio-reset <name>         slic reset gpio name\n"
		"  --gpio-interrupt <name>     slic interrupt gpio name\n"
		"  --channels <1-2>            tdm channels to open\n"
		"  --prefill <1-%d>           playback pre-fill periods\n"
		"  --rate <8000|16000>         line rate in Hz (default 8000)\n"
		"  --cpu <-1-%d>             engine cpu, -1 does not pin\n"
		"  --prio <%d-%d>              engine SCHED_FIFO priority\n"
		"  --region <ISO3-or-path>     region to load\n"
		"  --ports <1-%d>              populated fxs ports\n"
		"  --dump                      loopback media dump only\n"
		"  --seconds <0-%d>          event-clock run budget; 0 waits for\n"
		"                              SIGINT on hardware. File requires 1..600\n"
		"                              virtual seconds and uses a fixed %u-tick\n"
		"                              net, so playback can reach "
		"1200 seconds\n"
		"  --line <0-%d>               line number (default 0)\n"
		"  --tone <name>               dial, stutter, second_dial, ringback,\n"
		"                              busy, reorder, sit, call_waiting,\n"
		"                              confirm, howler, short_stutter_dial,\n"
		"                              or number_unobtainable\n"
		"  --variant <0-4>             call-waiting variant, 0 plays the plain\n"
		"                              tone (cw verb)\n"
		"  --ring-s <1-%d>             looptest ring cap in seconds\n"
		"                              (default %d)\n"
		"  --delay-ms <0-%u>         looptest loop delay (default %d)\n"
		"  --peer <1-%d>                looptest peer line (default 1)\n"
		"  --digits <0-9*#A-D>         digits to play\n"
		"  --duration <0-65535>        tone or digit duration in milliseconds\n"
		"  --cid-name <text>           caller name, at most 32 characters\n"
		"  --cid-number <text>         caller number, at most 32 characters\n"
		"  --cid-format <format>       bellcore-fsk, etsi-fsk, etsi-rpas,\n"
		"                              etsi-dtas, dtmf, ntt, etsi-lr-dtas,\n"
		"                              or dtmf-dtas\n"
		"  --cid-at-ring <0-7>         ring break for caller id (default 1)\n"
		"  --cid-wait-ms <0-%d>      caller id delay in the break (default 500)\n"
		"  --type2                     send type 2 caller identity\n"
		"  --mwi <on|off>              send a VMWI caller identity record\n"
		"  --debug                     print the slic plug-in's debug lines\n"
		"  -h, --help                  this text\n"
		"\nevents arms DTMF and pulse detection on every available line.\n"
		"Pulse digits need a --region with [hook] pulse_enable on.\n"
		"Quiet event streams wait for the full --seconds window.\n"
		"\nlooptest rings each port until its handset is lifted, plays the\n"
		"loop while it is off-hook, and rings again on-hook. --seconds is the\n"
		"run budget and --ring-s caps one ring.\n"
		"\na text value that starts with a dash needs the --name=value form.\n",
		TOOL_PREFILL_MAX, TOOL_CPU_MAX, TOOL_PRIO_MIN, TOOL_PRIO_MAX,
		UDSP_LINE_MAX, CLI_SECONDS_MAX,
		TOOL_FILE_NET_SECONDS * TOOL_TICKS_PER_SEC,
		UDSP_LINE_MAX - 1, CLI_RING_S_MAX, CLI_RING_S_DEFAULT,
		UDSP_LOOP_DELAY_MAX_MS, CLI_DELAY_MS_DEFAULT, UDSP_LINE_MAX - 1,
		TOOL_CID_WAIT_MAX_MS);
}

static void usage_fail(const char *what, const char *arg)
{
	fprintf(stderr, "udsp-cli: %s%s%s\n", what, arg ? ": " : "", arg ? arg : "");
	usage(stderr);
}

static bool digits_valid(const char *digits)
{
	if (!digits[0])
		return false;
	for (const char *p = digits; *p; p++) {
		if ((*p >= '0' && *p <= '9') || *p == '*' || *p == '#' ||
		    (*p >= 'A' && *p <= 'D'))
			continue;
		return false;
	}

	return true;
}

static bool region_set(struct cli_args *a, const struct option *o,
		       const char *region)
{
	struct udsp_config *cfg = &a->base.cfg;

	/* A given --region must name one, so the given bit and region[0]
	 * agree for every rule that reads either. */
	if (!region[0]) {
		fprintf(stderr, "udsp-cli: --region: empty value\n");
		return false;
	}
	if (!tool_opt_text(o, region, a->region, sizeof(a->region)))
		return false;
	if (strchr(region, '/')) {
		cfg->region[0] = '\0';
		return true;
	}

	return tool_opt_text(o, region, cfg->region, sizeof(cfg->region));
}

static void args_defaults(struct cli_args *a)
{
	memset(a, 0, sizeof(*a));
	tool_args_defaults(&a->base);
	a->base.cfg.populated_ports = 2;
	a->base.cfg.prefill_periods = 3;
	a->ring_s = CLI_RING_S_DEFAULT;
	a->delay_ms = CLI_DELAY_MS_DEFAULT;
	a->peer = 1;
}

static const struct cli_verb *verb_find(const char *name)
{
	for (size_t i = 0; i < sizeof(cli_verbs) / sizeof(cli_verbs[0]); i++) {
		if (!strcmp(cli_verbs[i].name, name))
			return &cli_verbs[i];
	}

	return NULL;
}

/* The tool's own options, and the shared ones whose meaning is the tool's:
 * --region takes a path as well as an ISO3 name, and --duration 0 asks for
 * an unbounded tone. */
static bool opt_apply(struct tool_args *t, const struct option *o,
		      const char *val)
{
	struct cli_args *a = (struct cli_args *)t;
	long n = 0;

	switch (o->val) {
	case TOOL_OPT_REGION:
		return region_set(a, o, val);
	case TOOL_OPT_DURATION:
		if (!tool_opt_number(o, val, 0, UINT16_MAX, &n))
			return false;
		a->duration_ms = (uint16_t)n;
		return true;
	case CLI_OPT_FAX_PLUGIN:
		return tool_opt_text(o, val, t->cfg.fax_plugin,
				     sizeof(t->cfg.fax_plugin));
	case CLI_OPT_SECONDS:
		if (!tool_opt_number(o, val, 0, CLI_SECONDS_MAX, &n))
			return false;
		a->seconds = (int)n;
		return true;
	case CLI_OPT_TONE:
		if (tool_tone_parse(val, &a->tone))
			return true;
		fprintf(stderr, "udsp-cli: --tone: unknown tone \"%s\"\n", val);
		return false;
	case CLI_OPT_DIGITS:
		if (digits_valid(val)) {
			a->digits = val;
			return true;
		}
		fprintf(stderr, "udsp-cli: --digits: expected 0-9, *, # or A-D, "
			"got \"%s\"\n", val);
		return false;
	case CLI_OPT_TYPE2:
		a->type2 = true;
		return true;
	case CLI_OPT_VARIANT:
		if (!tool_opt_number(o, val, 0, UDSP_REGION_CW_VIEW_MAX, &n))
			return false;
		a->variant = (uint8_t)n;
		return true;
	case CLI_OPT_RING_S:
		if (!tool_opt_number(o, val, 1, CLI_RING_S_MAX, &n))
			return false;
		a->ring_s = (uint8_t)n;
		return true;
	case CLI_OPT_DELAY_MS:
		if (!tool_opt_number(o, val, 0, UDSP_LOOP_DELAY_MAX_MS, &n))
			return false;
		a->delay_ms = (uint16_t)n;
		return true;
	case CLI_OPT_PEER:
		/* The first port of the verb is line 0, so a peer of 0 names
		 * that same port and never a second one. */
		if (!tool_opt_number(o, val, 1, UDSP_LINE_MAX - 1, &n))
			return false;
		a->peer = (uint8_t)n;
		return true;
	case CLI_OPT_DUMP:
		a->dump = true;
		return true;
	case CLI_OPT_DEBUG:
		a->debug = true;
		return true;
	}

	return tool_opt_apply(t, o, val);
}

/* argv[0] is the verb: getopt_long treats it as the program name and skips
 * it, so the caller passes argc - 1 and argv + 1. */
static enum tool_parse args_parse(int argc, char **argv, struct cli_args *a)
{
	enum tool_parse rc = tool_args_parse(argc, argv, cli_options, &a->base,
					     opt_apply);

	if (rc == TOOL_PARSE_ERROR) {
		usage(stderr);
		return rc;
	}
	if (rc == TOOL_PARSE_OK && a->type2 &&
	    tool_given(&a->base, TOOL_OPT_CID_FORMAT) &&
	    (a->base.cid_format == UDSP_CIDF_DTMF ||
	     a->base.cid_format == UDSP_CIDF_DTMF_DTAS)) {
		usage_fail("--type2 rejects a DTMF --cid-format", NULL);
		return TOOL_PARSE_ERROR;
	}

	return rc;
}

/* The getopt name of the lowest option in bits. The table lists the options
 * in the order of their ids. */
static const char *opt_first_name(uint64_t bits)
{
	for (size_t i = 0; cli_options[i].name; i++) {
		int id = cli_options[i].val;

		if (id >= TOOL_OPT_TDM_BACKEND && (bits & TOOL_BIT(id)))
			return cli_options[i].name;
	}

	return "option";
}

static bool verb_args_valid(const struct cli_args *a)
{
	const struct cli_verb *v = a->verb;
	uint64_t missing = v->required & ~a->base.given;
	uint64_t extra = a->base.given & ~(CLI_OPTS_BOARD | v->allowed);
	char what[96];

	if (missing) {
		snprintf(what, sizeof(what), "%s requires --%s", v->name,
			 opt_first_name(missing));
		usage_fail(what, NULL);
		return false;
	}
	if (extra) {
		snprintf(what, sizeof(what), "--%s does not apply to %s",
			 opt_first_name(extra), v->name);
		usage_fail(what, NULL);
		return false;
	}

	return !v->valid || v->valid(a);
}

static bool cid_args_valid(const struct cli_args *a)
{
	bool mwi = tool_given(&a->base, TOOL_OPT_MWI);
	bool identity = tool_cid_given(&a->base);

	if (!mwi && !identity) {
		usage_fail("cid requires identity fields or --mwi", NULL);
		return false;
	}
	if (mwi && (a->type2 || identity)) {
		usage_fail("--mwi cannot be combined with identity or --type2", NULL);
		return false;
	}

	return true;
}

static bool ring_args_valid(const struct cli_args *a)
{
	const struct tool_args *t = &a->base;
	bool identity = tool_cid_given(t);

	if (!identity && (tool_given(t, TOOL_OPT_CID_AT_RING) ||
			  tool_given(t, TOOL_OPT_CID_WAIT_MS))) {
		usage_fail("ring timing option requires --cid-name or --cid-number",
			   NULL);
		return false;
	}
	if (!identity && tool_given(t, TOOL_OPT_CID_FORMAT)) {
		usage_fail("ring caller id requires --cid-name or --cid-number", NULL);
		return false;
	}
	if (identity && !tool_given(t, TOOL_OPT_REGION)) {
		usage_fail("ring caller identity requires --region", NULL);
		return false;
	}

	return true;
}

static void cfg_print(const struct cli_args *a)
{
	const struct udsp_config *cfg = &a->base.cfg;

	printf("verb = %s\n", a->verb->name);
	printf("dump = %s\n", a->dump ? "yes" : "no");
	printf("seconds = %d\n", a->seconds);
	printf("tdm_backend = \"%s\"\n", cfg->tdm_backend);
	printf("tdm_playback = \"%s\"\n", cfg->tdm_playback);
	printf("tdm_capture = \"%s\"\n", cfg->tdm_capture);
	printf("slic_plugin = \"%s\"\n", cfg->slic_plugin);
	printf("fax_plugin = \"%s\"\n", cfg->fax_plugin);
	printf("spidev = \"%s\"\n", cfg->spidev);
	printf("gpio_reset = \"%s\"\n", cfg->gpio_reset);
	printf("gpio_interrupt = \"%s\"\n", cfg->gpio_interrupt);
	printf("populated_ports = %u\n", cfg->populated_ports);
	printf("channels = %u\n", cfg->channels);
	printf("prefill_periods = %u\n", cfg->prefill_periods);
	printf("sample_rate = %u\n", cfg->sample_rate);
	printf("engine_cpu = %d\n", cfg->engine_cpu);
	printf("engine_prio = %d\n", cfg->engine_prio);
	printf("region = \"%s\"\n", cfg->region);
}

static bool step_report(const char *step, bool ok)
{
	printf("%s - %s\n", ok ? "ok" : "fail", step);

	return ok;
}

static bool chan_step(unsigned chan, const char *step, bool ok)
{
	printf("%s - chan %u %s\n", ok ? "ok" : "fail", chan, step);

	return ok;
}

static bool chan_sequence(const struct udsp_slic_ops *ops, struct udsp_slic *s,
			  unsigned chan)
{
	struct udsp_slic_fake_chan_state st = { 0 };
	bool offhook = true;
	uint32_t bits = 1;
	bool ok = true;

	ok &= chan_step(chan, "init", ops->init(s, chan) == 0);
	ok &= chan_step(chan, "linefeed", ops->linefeed_set(s, chan,
							    UDSP_LF_FWD_ACTIVE) == 0);
	ok &= chan_step(chan, "ring start", ops->ring_start(s, chan) == 0);
	ok &= chan_step(chan, "ring stop", ops->ring_stop(s, chan) == 0);
	ok &= chan_step(chan, "hook read", ops->hook_read(s, chan, &offhook) == 0 &&
			!offhook);
	if (ops->hook_read_moved) {
		bool moved = true;

		ok &= chan_step(chan, "hook read moved",
				ops->hook_read_moved(s, chan, &offhook, &moved) == 0 &&
				!offhook && !moved);
	}
	ok &= chan_step(chan, "fault read", ops->fault_read(s, chan, &bits) == 0 &&
			bits == 0);
	ok &= chan_step(chan, "recorded", udsp_slic_fake_chan_state_get(s, chan, &st) &&
			st.inited && !st.ringing && st.linefeed == UDSP_LF_FWD_ACTIVE &&
			st.ring_start_count == 1 && st.ring_stop_count == 1);

	if (ops->reinit) {
		unsigned count = st.reinit_count;

		ok &= chan_step(chan, "reinit", ops->reinit(s, chan) == 0 &&
			udsp_slic_fake_chan_state_get(s, chan, &st) &&
			st.reinit_count == count + 1 &&
			st.linefeed == UDSP_LF_OPEN);
	}
	if (ops->converter_restart) {
		unsigned count = st.converter_restart_count;

		ok &= chan_step(chan, "converter restart",
			ops->converter_restart(s, chan) == 0 &&
			udsp_slic_fake_chan_state_get(s, chan, &st) &&
			st.converter_restart_count == count + 1 &&
			st.linefeed == UDSP_LF_OPEN);
	}
	if (ops->monitor_read) {
		struct udsp_slic_monitor monitor;
		unsigned count = st.monitor_count;

		ok &= chan_step(chan, "monitor read",
			ops->monitor_read(s, chan, &monitor) == 0 &&
			udsp_slic_fake_chan_state_get(s, chan, &st) &&
			st.monitor_count == count + 1);
	}
	if (ops->converter_read) {
		struct udsp_slic_converter converter;
		unsigned count = st.converter_read_count;

		ok &= chan_step(chan, "converter read",
			ops->converter_read(s, chan, &converter) == 0 &&
			udsp_slic_fake_chan_state_get(s, chan, &st) &&
			st.converter_read_count == count + 1);
	}

	return ok;
}

static bool descriptor_valid(void)
{
	const struct udsp_slic_ops *ops = udsp_slic_fake_descriptor.ops;

	if (udsp_slic_fake_descriptor.abi_version != UDSP_SLIC_ABI_VERSION)
		return false;
	if (udsp_slic_fake_descriptor.struct_size != sizeof(struct udsp_slic_ops))
		return false;

	return ops && ops->attach && ops->detach && ops->init && ops->linefeed_set &&
	       ops->ring_start && ops->ring_stop && ops->hook_read &&
	       ops->fault_read && ops->gain_set && ops->safe_xfer;
}

static int selftest_run(void)
{
	const struct udsp_slic_ops *ops = udsp_slic_fake_descriptor.ops;
	struct udsp_slic_safe safe = { .len = 0 };
	unsigned channels = 0;
	bool ok = true;

	udsp_slic_fake_reset();

	if (!step_report("descriptor", descriptor_valid()))
		return CLI_EXIT_ERROR;

	struct udsp_slic *s = ops->attach(udsp_slic_fake_host_get(), &channels);

	if (!step_report("attach", s != NULL))
		return CLI_EXIT_ERROR;

	for (unsigned c = 0; c < channels; c++)
		ok &= chan_sequence(ops, s, c);

	ops->safe_xfer(s, &safe);
	ok &= step_report("safe transfer", safe.len > 0 && safe.len <= UDSP_SLIC_SAFE_MAX);
	ok &= step_report("g729 tables", g729_tables_verify());

	ops->detach(s);
	printf("# selftest %s, %u channels\n", ok ? "ok" : "fail", channels);

	return ok ? CLI_EXIT_OK : CLI_EXIT_ERROR;
}

/* --- loopback: the real-time gate --- */

enum cli_wait_reason { CLI_WAIT_DONE, CLI_WAIT_SIGNAL, CLI_WAIT_EXPIRED };

/* A timed run ends on the tick cap, because the file back end has a virtual
 * clock and its ticks are not seconds; the wall clock only backstops an engine
 * that stopped ticking, and reaching it means the run did not deliver the ticks
 * that were asked for. */
static enum cli_wait_reason cli_wait(struct engine *e, int seconds)
{
	uint64_t deadline = tool_mono_ms() + ((uint64_t)seconds + 2) * 1000u;
	struct timespec poll = { .tv_sec = 0, .tv_nsec = CLI_POLL_MS * 1000000 };

	while (!tool_stopped) {
		if (engine_done(e))
			return CLI_WAIT_DONE;
		if (seconds && tool_mono_ms() >= deadline)
			return CLI_WAIT_EXPIRED;
		nanosleep(&poll, NULL);
	}

	return CLI_WAIT_SIGNAL;
}

static const char *cli_wait_name(enum cli_wait_reason why)
{
	switch (why) {
	case CLI_WAIT_DONE:
		return "ticks";
	case CLI_WAIT_SIGNAL:
		return "signal";
	case CLI_WAIT_EXPIRED:
		return "expired";
	}

	return "unknown";
}

static const char *cli_policy_name(int policy)
{
	switch (policy) {
	case SCHED_FIFO:
		return "fifo";
	case SCHED_RR:
		return "rr";
	case SCHED_OTHER:
		return "other";
	}

	return "unknown";
}

static void cli_geom_print(const struct engine_stats *st)
{
	printf("serial = %llu\n", (unsigned long long)st->serial);
	printf("ticks = %llu\n", (unsigned long long)st->ticks);
	printf("prefill_periods = %u\n", st->prefill_periods);
	printf("granted_periods = %u\n", st->granted_periods);
	/* A gate run must never measure a depth it did not ask for. */
	if (st->prefill_periods < st->prefill_requested)
		printf("prefill_clamped = yes, requested %u, using %u, granted ring %u\n",
		       st->prefill_requested, st->prefill_periods,
		       st->granted_periods);
	else
		printf("prefill_clamped = no\n");
	printf("frames_per_period = %u\n", st->frames_per_period);
	printf("channels = %u\n", st->channels);
	printf("paced = %s\n", st->paced ? "yes" : "no");
}

/* The buckets are exclusive ranges, so the labels name a range: a cumulative
 * "le" label over an exclusive bucket reads as a count it does not hold. */
static void cli_hist_print(const struct engine_stats *st)
{
	for (unsigned i = 0; i < ENGINE_HIST_BUCKETS - 1; i++) {
		unsigned low = i ? engine_hist_edge_us[i - 1] + 1 : 0;

		printf("interval_us_%u_%u = %llu\n", low, engine_hist_edge_us[i],
		       (unsigned long long)st->interval[i]);
	}

	printf("interval_us_%u_up = %llu\n",
	       engine_hist_edge_us[ENGINE_HIST_BUCKETS - 2] + 1,
	       (unsigned long long)st->interval[ENGINE_HIST_BUCKETS - 1]);
	printf("interval_samples = %llu\n", (unsigned long long)st->interval_samples);
	printf("interval_min_us = %u\n", st->interval_min_us);
	printf("interval_max_us = %u\n", st->interval_max_us);
}

static void cli_fault_print(const struct engine_stats *st)
{
	printf("holds = %llu\n", (unsigned long long)st->holds);
	printf("capture_drops = %llu\n", (unsigned long long)st->capture_drops);
	printf("trim_drops = %llu\n", (unsigned long long)st->trim_drops);
	printf("playback_expired = %llu\n",
	       (unsigned long long)st->playback_expired);
	printf("capture_stalls = %llu\n", (unsigned long long)st->capture_stalls);
	printf("delay_stalls = %llu\n", (unsigned long long)st->delay_stalls);
	printf("xruns = %llu\n", (unsigned long long)st->xruns);
	printf("stalls_total = %llu\n", (unsigned long long)engine_stats_stalls(st));
	printf("recover_failures = %llu\n", (unsigned long long)st->recover_failures);
	printf("again = %llu\n", (unsigned long long)st->again);
	printf("read_errors = %llu\n", (unsigned long long)st->read_errors);
	printf("write_errors = %llu\n", (unsigned long long)st->write_errors);
	printf("delay_errors = %llu\n", (unsigned long long)st->delay_errors);
	printf("delay_samples = %llu\n", (unsigned long long)st->delay_samples);
	printf("delay_calibrated = %s\n", st->delay_calibrated ? "yes" : "no");
	printf("delay_ref_frames = %d\n", st->delay_ref);
	printf("loop_ref_frames = %d\n", st->loop_ref);
	printf("delay_drift_samples = %llu\n",
	       (unsigned long long)st->delay_drift_samples);
	printf("delay_drift_min_frames = %d\n", st->delay_drift_min);
	printf("delay_drift_max_frames = %d\n", st->delay_drift_max);
	printf("delay_drift_last_frames = %d\n", st->delay_drift_last);
	printf("delay_dev_min_frames = %d\n", st->delay_dev_min);
	printf("delay_dev_max_frames = %d\n", st->delay_dev_max);
	printf("delay_dev_last_frames = %d\n", st->delay_dev_last);
	printf("last_errno = %d\n", st->last_errno);
	printf("stopped_fatal = %s\n", st->stopped_fatal ? "yes" : "no");
}

static void cli_rt_print(const struct rt_report *rt)
{
	printf("rt_applied = %s\n", rt->sched_applied ? "yes" : "no");
	printf("rt_errno = %d\n", rt->sched_errno);
	printf("rt_prio = %d\n", rt->prio);
	printf("rt_policy = %s\n", cli_policy_name(rt->policy));
	printf("rt_cpu = %d\n", rt->cpu);
	printf("mlock_applied = %s\n", rt->mlock_applied ? "yes" : "no");
	printf("mlock_errno = %d\n", rt->mlock_errno);
	printf("affinity_applied = %s\n", rt->affinity_applied ? "yes" : "no");
	printf("affinity_errno = %d\n", rt->affinity_errno);
}

/* The file back end runs a virtual clock, so every run on it needs a bound. */
static bool file_run_bounded(const struct cli_args *a)
{
	if (!tool_backend_is_file(&a->base.cfg))
		return true;
	if (a->seconds > 0 && a->seconds <= TOOL_FILE_SECONDS)
		return true;

	fprintf(stderr, "udsp-cli: %s: the file back end requires --seconds "
		"1..%d\n", a->verb->name, TOOL_FILE_SECONDS);

	return false;
}

static int loopback_run(const struct cli_args *a)
{
	struct engine_opts opts = {
		.loopback = true,
		.max_ticks = (uint64_t)a->seconds * TOOL_TICKS_PER_SEC,
	};
	struct engine_stats st;

	if (!file_run_bounded(a))
		return CLI_EXIT_ERROR;

	struct engine_open_verdict fail = { 0 };
	struct engine *e = engine_open(&a->base.cfg, &opts, &fail);

	if (!e) {
		char detail[128];

		/* The loopback verb opens the engine directly, so it says
		 * which half refused: a --rate the board will not take reads
		 * as a geometry refusal (engine.h enum engine_open_fail), and
		 * the backend's own record names the call and the errno. */
		engine_tdm_fail_text(&fail.tdm, detail, sizeof(detail));
		fprintf(stderr, "udsp-cli: loopback: engine open failed%s%s%s\n",
			fail.cause == ENGINE_OPEN_GEOM ?
			" (the backend refused the geometry:"
			" rate, period or ring)" : "",
			detail[0] ? ": " : "", detail);
		return CLI_EXIT_ERROR;
	}

	tool_signals_install();

	enum cli_wait_reason why = cli_wait(e, a->seconds);

	engine_stop(e, &st);
	engine_close(e);

	printf("# loopback\n");
	printf("wait_reason = %s\n", cli_wait_name(why));
	cli_geom_print(&st);
	cli_hist_print(&st);
	cli_fault_print(&st);
	cli_rt_print(&st.rt);

	/* An expired timed run did not deliver the ticks that were asked for,
	 * so the snapshot is partial and the gate must not read it as a pass. */
	if (st.stopped_fatal || why == CLI_WAIT_EXPIRED)
		return CLI_EXIT_ERROR;

	return CLI_EXIT_OK;
}

struct cli_engine {
	struct tool_engine base;
	struct udsp_region region;
	const char *verb;
	bool seq_valid;
	uint16_t seq;
	uint64_t events_lost;
	uint64_t seq_zero;
	bool hook_seen[UDSP_LINE_MAX];
	bool hook_on[UDSP_LINE_MAX];
	bool hook_answer_edge[UDSP_LINE_MAX];
};

_Static_assert(offsetof(struct cli_engine, base) == 0,
	       "event_record converts the base pointer back");

static void fault_decode_print(uint32_t code)
{
	static const struct {
		uint32_t bit;
		const char *name;
	} bits[] = {
		{ UDSP_FAULT_ENGINE_STALL, "ENGINE_STALL" },
		{ UDSP_FAULT_SLIC_THERMAL, "SLIC_THERMAL" },
		{ UDSP_FAULT_SLIC_DC, "SLIC_DC" },
		{ UDSP_FAULT_SLIC_PQ, "SLIC_PQ" },
		{ UDSP_FAULT_SLIC_VBAT, "SLIC_VBAT" },
		{ UDSP_FAULT_SLIC_LONGSTAT, "SLIC_LONGSTAT" },
		{ UDSP_FAULT_SLIC_REINIT, "SLIC_REINIT" },
		{ UDSP_FAULT_SLIC_DCDC, "SLIC_DCDC" },
		{ UDSP_FAULT_SLIC_CLOCK, "SLIC_CLOCK" },
		{ UDSP_FAULT_DEVICE_RESET, "DEVICE_RESET" },
	};
	uint32_t known = 0;
	const char *separator = "";

	fprintf(stderr, "udsp-cli: SLIC_FAULT code=0x%08x bits=", code);
	for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
		known |= bits[i].bit;
		if (!(code & bits[i].bit))
			continue;
		fprintf(stderr, "%s%s", separator, bits[i].name);
		separator = ",";
	}
	if (code & ~known) {
		fprintf(stderr, "%sUNKNOWN_0x%08x", separator, code & ~known);
		separator = ",";
	}
	if (!separator[0])
		fputs("cleared", stderr);
	fputc('\n', stderr);
}

static void event_seq_check(struct cli_engine *e, const struct udsp_event *ev)
{
	uint16_t expect;
	uint16_t lost;

	if (!ev->seq) {
		e->seq_zero++;
		if (e->seq_zero == 1)
			fprintf(stderr, "udsp-cli: synthetic/illegal %s record has "
				"seq 0: line=%u stamp_ms=%u\n",
				tool_event_name(ev->type), ev->line, ev->stamp_ms);
		return;
	}
	if (!e->seq_valid) {
		e->seq_valid = true;
		e->seq = ev->seq;
		return;
	}

	expect = e->seq + 1;
	if (!expect)
		expect = 1;
	e->seq = ev->seq;
	if (ev->seq == expect)
		return;

	lost = (uint16_t)(ev->seq - expect);
	if (ev->seq < expect)
		lost--;
	e->events_lost += lost;
	fprintf(stderr, "udsp-cli: event gap: expected seq %u, got %u, "
		"lost %llu total\n", expect, ev->seq,
		(unsigned long long)e->events_lost);
}

/* The engine's event hook: the shared print and fault rule, then the
 * sequence check, the hook edges and the fault decode of this tool. */
static void event_record(struct tool_engine *base, const struct udsp_event *ev)
{
	struct cli_engine *e = (struct cli_engine *)base;

	tool_event_record(base, ev);
	if (ev->type != UDSP_EV_JOB_DONE)
		event_seq_check(e, ev);
	if (ev->type == UDSP_EV_HOOK && ev->line < UDSP_LINE_MAX) {
		if (ev->u.hook.state == UDSP_HOOK_RELEASE) {
			e->hook_seen[ev->line] = true;
			e->hook_on[ev->line] = true;
		}
		if (ev->u.hook.state == UDSP_HOOK_SEIZE) {
			e->hook_answer_edge[ev->line] =
				e->hook_seen[ev->line] && e->hook_on[ev->line];
			e->hook_seen[ev->line] = true;
			e->hook_on[ev->line] = false;
		}
	}
	if (ev->type == UDSP_EV_SLIC_FAULT)
		fault_decode_print(ev->u.fault.code);
}

static uint32_t event_clock_sample(struct cli_engine *e)
{
	return udsp_now_ms(e->base.s);
}

static bool verb_status_ok(const struct cli_args *a, const char *op,
			   enum udsp_status status)
{
	if (tool_status_ok(op, status))
		return true;
	if (status == UDSP_UNSUPPORTED && !a->region[0])
		fprintf(stderr, "udsp-cli: %s: no --region is active\n", op);

	return false;
}

static void region_application_print(const struct cli_args *a)
{
	if (!a->region[0]) {
		printf("region requested=<none> applied=none\n");
		return;
	}

	printf("region requested=\"%s\" applied=%s\n", a->region,
	       strchr(a->region, '/') ? "via job" : "at open");
}

static bool region_preparse(const struct cli_args *a, struct udsp_region *region)
{
	struct line_region_error err;
	char path[SLIC_JOB_PATH_MAX];
	int rc;

	memset(region, 0, sizeof(*region));
	if (!a->region[0])
		return true;
	rc = udsp_region_path_build(a->region, path, sizeof(path));
	if (rc) {
		fprintf(stderr, "udsp-cli: region path failed for \"%s\": %s\n",
			a->region, strerror(-rc));
		return false;
	}
	rc = line_region_load(path, region, &err);
	if (!rc)
		return true;
	if (err.text[0] && err.line)
		fprintf(stderr, "udsp-cli: region parse failed: %s: line %u: %s\n",
			path, err.line, err.text);
	else if (err.text[0])
		fprintf(stderr, "udsp-cli: region parse failed: %s: %s\n", path,
			err.text);
	else
		fprintf(stderr, "udsp-cli: region load failed: %s: %s\n", path,
			strerror(-rc));

	return false;
}

static int selftest_local(const struct cli_args *a)
{
	cfg_print(a);

	return selftest_run();
}

static void region_segment_print(const struct udsp_region_segment *seg)
{
	unsigned i;

	if (!seg->freq_count) {
		printf("segment = off %u\n", seg->duration_ms);
		return;
	}
	printf("segment = ");
	for (i = 0; i < seg->freq_count; i++)
		printf("%s%u", i ? "+" : "", seg->freq_hz[i]);
	printf(" %d %u\n", seg->level_dbm0, seg->duration_ms);
}

static void region_tone_body_print(const struct udsp_region_tone *t)
{
	const struct udsp_tone_desc *d = &t->desc;
	unsigned i;

	if (t->segmented) {
		for (i = 0; i < t->segment_count; i++)
			region_segment_print(&t->segment[i]);
		printf("silence = %u\n", t->silence_ms);
	} else {
		printf("freq =");
		for (i = 0; i < d->frequency_count; i++)
			printf(" %u", d->frequency_hz[i]);
		printf("\nlevel =");
		for (i = 0; i < d->frequency_count; i++)
			printf(" %d", d->level_dbm0[i]);
		printf("\ncadence =");
		if (!d->cadence_count)
			printf(" on");
		for (i = 0; i < d->cadence_count; i++)
			printf(" %u %u", d->cadence[i].make_ms, d->cadence[i].break_ms);
		printf("\n");
	}
	if (t->repeat_count)
		printf("repeat = %u\n", t->repeat_count);
	else
		printf("repeat = forever\n");
	if (t->then_steady)
		printf("then = on\n");
}

static void region_tone_print(const struct udsp_region_tone *t)
{
	printf("\n[tone %s]\n", line_region_tone_name(t->desc.tone));
	region_tone_body_print(t);
}

static void region_cadence_print(const char *key, const struct udsp_cadence *c,
				 unsigned count)
{
	unsigned i;

	printf("%s =", key);
	for (i = 0; i < count; i++)
		printf(" %u %u", c[i].on_ms, c[i].off_ms);
	printf("\n");
}

/* The profile in the grammar's own form, so the print of a file and the file
 * read the same way and a printed profile parses again. */
static void region_profile_print(const struct udsp_region *r)
{
	const struct udsp_region_cid *cid = &r->cid;
	const struct udsp_region_hook *h = &r->hook;
	unsigned i;

	printf("[region]\niso3 = %s\n", r->iso3);
	if (r->describe[0])
		printf("describe = %s\n", r->describe);
	for (i = 1; i < UDSP_REGION_TONE_MAX; i++)
		region_tone_print(&r->tone[i]);
	for (i = 0; i < r->ring_count; i++) {
		printf("\n[ring %s]\n", r->ring[i].name);
		region_cadence_print("cadence", r->ring[i].cadence,
				     r->ring[i].cadence_count);
		if (r->ring[i].first_count)
			region_cadence_print("first", r->ring[i].first,
					     r->ring[i].first_count);
	}
	for (i = 0; i < r->cw_count; i++) {
		printf("\n[cw c%u]\n", i + 1);
		region_tone_body_print(&r->cw[i]);
	}
	printf("\n[cid]\nformat = %s\nmwi_format = %s\n",
	       tool_cid_format_name(cid->format),
	       tool_cid_format_name(cid->mwi_format));
	printf("wait_ms = %u\nack_timeout_ms = %u\n", cid->wait_ms,
	       cid->ack_timeout_ms);
	printf("seizure_bits = %u\nmark_bits = %u\n", cid->seizure_bits,
	       cid->mark_bits);
	printf("dtas_ms = %u\nrpas_ms = %u\nlr_pause_ms = %u\n", cid->dtas_ms,
	       cid->rpas_ms, cid->lr_pause_ms);
	printf("\n[hook]\npulse_enable = %s\npulse_invert = %s\n",
	       h->pulse_enable ? "on" : "off", h->pulse_invert ? "on" : "off");
	printf("debounce_ms = %u\n", h->debounce_ms);
	printf("flash_min_ms = %u\nflash_max_ms = %u\nrelease_min_ms = %u\n",
	       h->flash_min_ms, h->flash_max_ms, h->release_min_ms);
	printf("pulse_break_min_ms = %u\npulse_break_max_ms = %u\n",
	       h->pulse_break_min_ms, h->pulse_break_max_ms);
	printf("pulse_make_max_ms = %u\npulse_interdigit_ms = %u\n",
	       h->pulse_make_max_ms, h->pulse_interdigit_ms);
	printf("flash_service = %s\n",
	       line_region_flash_service_name(r->flash_service));
}

static int region_print_run(const struct cli_args *a)
{
	struct udsp_region region;

	if (!region_preparse(a, &region))
		return CLI_EXIT_ERROR;
	region_profile_print(&region);

	return CLI_EXIT_OK;
}

/* A refused open with a region that parses points at the devices. */
static const char *open_hint(const struct cli_args *a)
{
	if (!a->region[0])
		return "";

	return " (region file parses; suspect the tdm devices or the slic path)";
}

/* The whole engine struct is zeroed here, because the callers declare it on
 * the stack and the sequence and hook state start from that zero. */
static bool engine_open_real(const struct cli_args *a, struct cli_engine *e)
{
	struct udsp_job job = { 0 };

	memset(e, 0, sizeof(*e));
	tool_engine_init(&e->base, event_record);
	e->verb = a->verb->name;
	region_application_print(a);
	if (!file_run_bounded(a) || !tool_file_ticks_set(&a->base.cfg) ||
	    !region_preparse(a, &e->region))
		return false;
	if (!tool_engine_open(&e->base, &a->base.cfg, open_hint(a)))
		return false;
	/* The tool's log sink replaces the plug-in's own stderr fallback, and
	 * the library drops its debug lines until the mask asks for them. */
	if (a->debug)
		udsp_debug_set(e->base.s, UDSP_DEBUG_SLIC_LOG);
	if (!tool_engine_start(&e->base, a->base.line))
		return false;
	if (!strchr(a->region, '/'))
		return true;

	if (!tool_status_ok("udsp_region_load",
			    udsp_region_load(e->base.s, a->region, &job)))
		return false;

	return tool_job_wait(&e->base, "udsp_region_load", job.token, true) &&
	       !e->base.faulted;
}

static bool engine_close_real(struct cli_engine *e)
{
	struct udsp_counters counters = { 0 };
	bool counters_valid = e->base.s != NULL;
	bool ok = tool_engine_stop(&e->base);

	if (counters_valid)
		udsp_counters(e->base.s, &counters);
	tool_engine_close(&e->base);
	if (counters_valid) {
		printf("events_sent = %llu\n",
		       (unsigned long long)counters.events_sent);
		printf("events_dropped = %llu\n",
		       (unsigned long long)counters.events_dropped);
		printf("op_errors = %llu\n",
		       (unsigned long long)counters.op_errors);
		printf("tdm_stalls = %llu\n",
		       (unsigned long long)counters.tdm_stalls);
		printf("holds = %llu\n", (unsigned long long)counters.holds);
		printf("capture_drops = %llu\n",
		       (unsigned long long)counters.capture_drops);
		printf("trim_drops = %llu\n",
		       (unsigned long long)counters.trim_drops);
		printf("playback_expired = %llu\n",
		       (unsigned long long)counters.playback_expired);
		printf("hook_gaps = %llu\n",
		       (unsigned long long)counters.hook_gaps);
		printf("hook_moved_gaps = %llu\n",
		       (unsigned long long)counters.hook_moved_gaps);
		printf("hook_gap_max_ms = %u\n", counters.hook_gap_max_ms);
		printf("events_lost = %llu\n",
		       (unsigned long long)e->events_lost);
		printf("seq_zero = %llu\n",
		       (unsigned long long)e->seq_zero);
	}

	return ok && !e->base.faulted;
}

static uint32_t verb_deadline(const struct cli_args *a, struct cli_engine *e,
			      uint32_t grace_ms)
{
	if (!a->seconds)
		return 0;

	return event_clock_sample(e) + (uint32_t)a->seconds * 1000u + grace_ms;
}

static uint64_t verb_wall_deadline(const struct cli_args *a,
				   const struct cli_engine *e, uint32_t grace_ms)
{
	if (!a->seconds)
		return 0;
	if (e->base.file_backend)
		return tool_mono_ms() + TOOL_JOB_WAIT_MS;

	return tool_mono_ms() + (uint64_t)a->seconds * 1000u + grace_ms;
}

enum cli_wait_outcome {
	CLI_VERB_ENDED,
	CLI_VERB_EXPIRED,
	CLI_VERB_SIGNAL,
	CLI_VERB_ERROR,
	CLI_VERB_CONTINUE,	/* verb_event_next: an event arrived, inspect it */
};

static void wait_expired_print(const struct cli_engine *e, const char *awaited,
			       uint32_t deadline)
{
	fprintf(stderr, "udsp-cli: %s: wait expired awaiting %s at event "
		"deadline %u ms\n", e->verb, awaited, deadline);
}

static void wait_wall_expired_print(const struct cli_engine *e,
				    const char *awaited)
{
	fprintf(stderr, "udsp-cli: %s: wall bound expired awaiting %s\n",
		e->verb, awaited);
}

/* One turn of a verb's event loop: a shared terminal outcome with its message,
 * or CLI_VERB_CONTINUE when a normal event arrived in ev for the caller to
 * inspect. */
static enum cli_wait_outcome verb_event_next(struct cli_engine *e,
					     uint32_t deadline,
					     uint64_t wall_deadline,
					     const char *awaited,
					     struct udsp_event *ev)
{
	enum tool_wait got = tool_event_wait(&e->base, deadline, wall_deadline,
					     true, ev);

	if (got == TOOL_WAIT_EXPIRED) {
		wait_expired_print(e, awaited, deadline);
		return CLI_VERB_EXPIRED;
	}
	if (got == TOOL_WAIT_WALL_EXPIRED) {
		wait_wall_expired_print(e, awaited);
		return CLI_VERB_EXPIRED;
	}
	if (got == TOOL_WAIT_STOPPED)
		return CLI_VERB_SIGNAL;
	if (got == TOOL_WAIT_ERROR || e->base.faulted)
		return CLI_VERB_ERROR;

	return CLI_VERB_CONTINUE;
}

static enum cli_wait_outcome audio_wait(struct cli_engine *e, unsigned line,
					uint32_t deadline,
					uint64_t wall_deadline)
{
	struct udsp_event ev;
	enum cli_wait_outcome oc;

	while ((oc = verb_event_next(e, deadline, wall_deadline, "AUDIO_END",
				     &ev)) == CLI_VERB_CONTINUE) {
		if (ev.type == UDSP_EV_AUDIO_END && ev.line == line)
			return CLI_VERB_ENDED;
	}

	return oc;
}

/* tone and cw share the play, wait, stop shape; only the play op, its name and
 * the verb label in the abandon message differ. */
static bool tone_play_wait(const struct cli_args *a, struct cli_engine *e,
			   enum udsp_status play_st, const char *play_op,
			   const char *verb)
{
	uint32_t deadline = verb_deadline(a, e, 0);
	uint64_t wall_deadline = verb_wall_deadline(a, e, 0);
	enum cli_wait_outcome outcome;

	if (!verb_status_ok(a, play_op, play_st))
		return false;
	outcome = audio_wait(e, a->base.line, deadline, wall_deadline);
	if (outcome == CLI_VERB_ENDED)
		return true;
	if (!verb_status_ok(a, "udsp_tone_stop",
			    udsp_tone_stop(e->base.s, a->base.line)))
		return false;
	if (outcome == CLI_VERB_SIGNAL && a->duration_ms) {
		fprintf(stderr, "udsp-cli: %s: abandoned by signal\n", verb);
		return false;
	}

	return !a->duration_ms &&
	       (outcome == CLI_VERB_SIGNAL || outcome == CLI_VERB_EXPIRED);
}

static bool tone_run(const struct cli_args *a, struct cli_engine *e)
{
	return tone_play_wait(a, e,
			      udsp_tone_play(e->base.s, a->base.line, a->tone,
					     a->duration_ms),
			      "udsp_tone_play", "tone");
}

static bool cw_run(const struct cli_args *a, struct cli_engine *e)
{
	return tone_play_wait(a, e,
			      udsp_cw_tone_play(e->base.s, a->base.line,
						a->variant, a->duration_ms),
			      "udsp_cw_tone_play", "cw");
}

static bool dtmf_run(const struct cli_args *a, struct cli_engine *e)
{
	uint32_t deadline = verb_deadline(a, e, 0);
	uint64_t wall_deadline = verb_wall_deadline(a, e, 0);

	for (const char *p = a->digits; *p; p++) {
		enum cli_wait_outcome outcome;

		if (!verb_status_ok(a, "udsp_digit_play",
				    udsp_digit_play(e->base.s, a->base.line, *p,
						    a->duration_ms)))
			return false;
		outcome = audio_wait(e, a->base.line, deadline, wall_deadline);
		if (outcome == CLI_VERB_SIGNAL)
			fprintf(stderr, "udsp-cli: dtmf: abandoned by signal\n");
		if (outcome != CLI_VERB_ENDED)
			return false;
	}

	return true;
}

static void ring_fill(const struct cli_args *a,
		      const struct udsp_region *region, struct udsp_ring *ring)
{
	const struct udsp_region_ring *r1 = NULL;
	const char *source = "built-in";

	if (a->region[0]) {
		r1 = &region->ring[0];
		source = "region r1";
	}

	memset(ring, 0, sizeof(*ring));
	if (r1) {
		memcpy(ring->cadence, r1->cadence, sizeof(ring->cadence));
		memcpy(ring->first, r1->first, sizeof(ring->first));
		ring->cadence_count = r1->cadence_count;
		ring->first_count = r1->first_count;
	} else {
		ring->cadence[0].on_ms = TOOL_RING_ON_MS;
		ring->cadence[0].off_ms = TOOL_RING_OFF_MS;
		ring->cadence_count = 1;
	}
	ring->duration_ms = (uint32_t)a->seconds * 1000u;
	ring->cid_present = tool_cid_given(&a->base);
	ring->cid_at_ring = a->base.cid_at_ring;
	ring->cid_wait_ms = a->base.cid_wait_ms;
	tool_cid_fill(&a->base, &ring->cid);
	fputs("ring cadence", stdout);
	for (unsigned i = 0; i < ring->cadence_count; i++)
		printf(" on_ms=%u off_ms=%u", ring->cadence[i].on_ms,
		       ring->cadence[i].off_ms);
	printf(" source=%s\n", source);
	printf("ring cid_at_ring=%u cid_wait_ms=%u\n", ring->cid_at_ring,
	       ring->cid_wait_ms);
}

static enum cli_wait_outcome ring_end_wait(const struct cli_args *a,
					   struct cli_engine *e, bool *answered)
{
	uint32_t deadline = verb_deadline(a, e, 2000);
	uint64_t wall_deadline = verb_wall_deadline(a, e, 2000);
	struct udsp_event ev;
	enum cli_wait_outcome oc;
	bool offhook_warned = false;

	if (e->hook_seen[a->base.line] && !e->hook_on[a->base.line]) {
		fprintf(stderr, "udsp-cli: ring: initial off-hook does not count "
			"as ANSWERED\n");
		offhook_warned = true;
	}

	while ((oc = verb_event_next(e, deadline, wall_deadline, "RING_END",
				     &ev)) == CLI_VERB_CONTINUE) {
		if (ev.type == UDSP_EV_HOOK && ev.line == a->base.line &&
		    ev.u.hook.state == UDSP_HOOK_SEIZE) {
			if (e->hook_answer_edge[a->base.line]) {
				*answered = true;
				return CLI_VERB_ENDED;
			}
			if (!offhook_warned) {
				fprintf(stderr, "udsp-cli: ring: initial off-hook does "
					"not count as ANSWERED\n");
				offhook_warned = true;
			}
		}
		if (ev.type == UDSP_EV_RING_END && ev.line == a->base.line)
			return CLI_VERB_ENDED;
	}

	return oc;
}

static bool ring_run(const struct cli_args *a, struct cli_engine *e)
{
	struct udsp_ring ring;
	bool answered = false;
	enum cli_wait_outcome outcome;

	ring_fill(a, &e->region, &ring);
	e->hook_answer_edge[a->base.line] = false;
	if (!verb_status_ok(a, "udsp_ring_start",
			    udsp_ring_start(e->base.s, a->base.line, &ring)))
		return false;
	outcome = ring_end_wait(a, e, &answered);
	if (!verb_status_ok(a, "udsp_ring_stop",
			    udsp_ring_stop(e->base.s, a->base.line)))
		return false;
	if (answered)
		printf("ring answered line=%u\n", a->base.line);

	return outcome == CLI_VERB_ENDED || outcome == CLI_VERB_SIGNAL;
}

/* --- looptest: the manufacturing loop --- */

struct looptest_port {
	unsigned line;
	bool offhook;
	bool ringing;
	bool audio;
};

struct looptest {
	struct looptest_port port[2];
	unsigned count;
	enum udsp_loop_mode mode;
	/* An arm the library refused as transient gets another try. */
	bool retry;
};

/* What one turn of the verb's wait produced. */
enum looptest_turn {
	LOOPTEST_TURN_EVENT,
	LOOPTEST_TURN_RETRY,
	LOOPTEST_TURN_END,
};

/*
 * What the verb drives: one port on a single-line handle, which hears itself,
 * or two ports on a two-line handle, which hear each other. The peer defaults
 * to line 1 and --peer names another.
 */
static bool looptest_setup(const struct cli_args *a, struct cli_engine *e,
			   struct looptest *lt)
{
	const struct udsp_cap *cap = udsp_cap(e->base.s);

	memset(lt, 0, sizeof(*lt));
	if (!cap)
		return false;
	if (!(cap->features & UDSP_FEAT_LOOP)) {
		fprintf(stderr, "udsp-cli: looptest: the library reports no loop\n");
		return false;
	}
	lt->count = 1;
	lt->mode = UDSP_LOOP_SELF;
	if (cap->lines < 2) {
		if (!tool_given(&a->base, CLI_OPT_PEER))
			return true;
		fprintf(stderr, "udsp-cli: looptest: --peer needs a second line, "
			"cap.lines is %u\n", cap->lines);
		return false;
	}
	if (a->peer >= cap->lines) {
		fprintf(stderr, "udsp-cli: looptest: --peer %u is outside "
			"cap.lines %u\n", a->peer, cap->lines);
		return false;
	}
	lt->port[1].line = a->peer;
	lt->count = 2;
	lt->mode = UDSP_LOOP_PEER;

	return true;
}

static struct looptest_port *looptest_port_find(struct looptest *lt,
						unsigned line)
{
	for (unsigned i = 0; i < lt->count; i++) {
		if (lt->port[i].line == line)
			return &lt->port[i];
	}

	return NULL;
}

static void looptest_intro_print(const struct cli_args *a,
				 const struct looptest *lt)
{
	printf("looptest mode=%s ring_s=%u delay_ms=%u\n",
	       lt->mode == UDSP_LOOP_PEER ? "cross" : "self", a->ring_s,
	       a->delay_ms);
	for (unsigned i = 0; i < lt->count; i++)
		printf("looptest port=%u\n", lt->port[i].line);
	puts("one port: the handset hears its own voice after the delay");
	puts("two ports: the two handsets hear each other");
	puts("caution: a port rings until its handset is lifted or the ring "
	     "cap ends");
}

/* The published hook pair is unset until the slic thread takes its first
 * sample, so the read waits the short window that takes. A handset already
 * lifted must arm the audio at once, and a cached report from before the verb
 * ran would name the wrong state. */
static bool looptest_hook_read(const struct cli_args *a, struct cli_engine *e,
			       unsigned line, bool *offhook)
{
	uint64_t deadline = tool_mono_ms() + CLI_HOOK_READ_MS;

	for (;;) {
		struct timespec nap = { .tv_sec = 0,
					.tv_nsec = CLI_POLL_MS * 1000000 };
		enum udsp_status st = udsp_line_hook_query(e->base.s, line,
							   offhook);

		if (st == UDSP_OK)
			return true;
		if (st != UDSP_TRANSIENT || tool_mono_ms() >= deadline)
			return verb_status_ok(a, "udsp_line_hook_query", st);
		nanosleep(&nap, NULL);
	}
}

static bool looptest_hooks_read(const struct cli_args *a, struct cli_engine *e,
				struct looptest *lt)
{
	for (unsigned i = 0; i < lt->count; i++) {
		bool offhook = false;

		if (!looptest_hook_read(a, e, lt->port[i].line, &offhook))
			return false;
		lt->port[i].offhook = offhook;
		printf("looptest port=%u offhook=%s\n", lt->port[i].line,
		       offhook ? "yes" : "no");
	}

	return true;
}

/*
 * The audio rule, applied to every armed port after each hook edge. Under SELF
 * a port hears itself while it is off-hook. Under PEER the two ports hear each
 * other only while both are off-hook, so a lifted handset never carries the
 * ring voltage of the other port.
 */
static bool looptest_audio_want(const struct looptest *lt, unsigned i)
{
	if (lt->mode == UDSP_LOOP_SELF)
		return lt->port[i].offhook;

	return lt->port[0].offhook && lt->port[1].offhook;
}

static bool looptest_reconcile(const struct cli_args *a, struct cli_engine *e,
			       struct looptest *lt)
{
	lt->retry = false;
	for (unsigned i = 0; i < lt->count; i++) {
		bool want = looptest_audio_want(lt, i);
		unsigned peer = lt->port[i ^ 1u].line;
		enum udsp_status st;

		if (want == lt->port[i].audio)
			continue;
		st = udsp_line_loop(e->base.s, lt->port[i].line,
				    want ? lt->mode : UDSP_LOOP_OFF, peer,
				    a->delay_ms);
		/* The tick posts a line's events before it stores the line's
		 * ownership, so an arm that follows the off-hook event at once
		 * can find the ended ring still busy.
		 * The port keeps its audio off and the next turn of the wait
		 * arms it again. */
		if (st == UDSP_TRANSIENT && want) {
			lt->retry = true;
			continue;
		}
		if (!verb_status_ok(a, "udsp_line_loop", st))
			return false;
		lt->port[i].audio = want;
		printf("looptest port=%u audio=%s\n", lt->port[i].line,
		       want ? "on" : "off");
	}

	return true;
}

static bool looptest_ring_start(const struct cli_args *a, struct cli_engine *e,
				struct looptest_port *p,
				const struct udsp_ring *ring)
{
	if (p->ringing)
		return true;
	if (!verb_status_ok(a, "udsp_ring_start",
			    udsp_ring_start(e->base.s, p->line, ring)))
		return false;
	p->ringing = true;
	printf("looptest port=%u ring=start\n", p->line);

	return true;
}

static bool looptest_ring_stop(const struct cli_args *a, struct cli_engine *e,
			       struct looptest_port *p)
{
	if (!p->ringing)
		return true;
	if (!verb_status_ok(a, "udsp_ring_stop",
			    udsp_ring_stop(e->base.s, p->line)))
		return false;
	p->ringing = false;

	return true;
}

/* A ring that ran to its cap leaves the port quiet. The next ring waits for the
 * handset to go back on-hook, which is the RELEASE edge below. */
static bool looptest_event(const struct cli_args *a, struct cli_engine *e,
			   struct looptest *lt, const struct udsp_ring *ring,
			   const struct udsp_event *ev)
{
	struct looptest_port *p = looptest_port_find(lt, ev->line);

	if (!p)
		return true;
	if (ev->type == UDSP_EV_RING_END) {
		p->ringing = false;
		return true;
	}
	if (ev->type != UDSP_EV_HOOK)
		return true;
	if (ev->u.hook.state == UDSP_HOOK_SEIZE) {
		p->offhook = true;
		if (!looptest_ring_stop(a, e, p))
			return false;
		return looptest_reconcile(a, e, lt);
	}
	if (ev->u.hook.state != UDSP_HOOK_RELEASE)
		return true;
	p->offhook = false;
	if (!looptest_reconcile(a, e, lt))
		return false;

	return looptest_ring_start(a, e, p, ring);
}

static bool looptest_start(const struct cli_args *a, struct cli_engine *e,
			   struct looptest *lt, const struct udsp_ring *ring)
{
	for (unsigned i = 0; i < lt->count; i++) {
		if (lt->port[i].offhook)
			continue;
		if (!looptest_ring_start(a, e, &lt->port[i], ring))
			return false;
	}

	return looptest_reconcile(a, e, lt);
}

/* The verb leaves every port as it found it, whichever way the run ended. */
static bool looptest_stop(const struct cli_args *a, struct cli_engine *e,
			  struct looptest *lt)
{
	bool ok = true;

	for (unsigned i = 0; i < lt->count; i++) {
		struct looptest_port *p = &lt->port[i];

		ok &= verb_status_ok(a, "udsp_line_loop",
				     udsp_line_loop(e->base.s, p->line,
						    UDSP_LOOP_OFF, 0, 0));
		p->audio = false;
		ok &= verb_status_ok(a, "udsp_ring_stop",
				     udsp_ring_stop(e->base.s, p->line));
		p->ringing = false;
	}

	return ok;
}

/*
 * One turn of the verb's wait. A pending retry needs a wake of its own, because
 * nothing else happens on the line until the next hook edge, so the wait takes
 * a nearer wall bound while one stands and the caller reconciles again on it.
 * The verb's own bounds and their messages are unchanged: only a bound this
 * function set itself answers RETRY.
 */
static enum looptest_turn looptest_turn_next(struct cli_engine *e,
					     uint32_t deadline,
					     uint64_t wall_deadline, bool retry,
					     struct udsp_event *ev,
					     enum cli_wait_outcome *oc)
{
	uint64_t retry_at = tool_mono_ms() + CLI_LOOP_RETRY_MS;
	bool own = retry && (!wall_deadline || retry_at < wall_deadline);
	enum tool_wait got = tool_event_wait(&e->base, deadline,
					     own ? retry_at : wall_deadline,
					     true, ev);

	if (got == TOOL_WAIT_WALL_EXPIRED && own)
		return LOOPTEST_TURN_RETRY;
	if (got == TOOL_WAIT_GOT && !e->base.faulted)
		return LOOPTEST_TURN_EVENT;
	if (got == TOOL_WAIT_EXPIRED)
		wait_expired_print(e, "hook edge", deadline);
	if (got == TOOL_WAIT_WALL_EXPIRED)
		wait_wall_expired_print(e, "hook edge");
	if (got == TOOL_WAIT_STOPPED)
		*oc = CLI_VERB_SIGNAL;
	else if (got == TOOL_WAIT_EXPIRED || got == TOOL_WAIT_WALL_EXPIRED)
		*oc = CLI_VERB_EXPIRED;
	else
		*oc = CLI_VERB_ERROR;

	return LOOPTEST_TURN_END;
}

static bool looptest_run(const struct cli_args *a, struct cli_engine *e)
{
	uint32_t deadline = verb_deadline(a, e, 0);
	uint64_t wall_deadline = verb_wall_deadline(a, e, 0);
	enum cli_wait_outcome oc = CLI_VERB_ERROR;
	enum looptest_turn turn;
	struct udsp_event ev;
	struct udsp_ring ring;
	struct looptest lt;
	bool stopped;

	if (!looptest_setup(a, e, &lt))
		return false;
	looptest_intro_print(a, &lt);
	ring_fill(a, &e->region, &ring);
	/* The ring cap is the verb's own bound and not the run budget, so a
	 * ring ends while the mode stands. */
	ring.duration_ms = (uint32_t)a->ring_s * 1000u;
	if (!looptest_hooks_read(a, e, &lt) || !looptest_start(a, e, &lt, &ring)) {
		looptest_stop(a, e, &lt);
		return false;
	}

	while ((turn = looptest_turn_next(e, deadline, wall_deadline, lt.retry,
					  &ev, &oc)) != LOOPTEST_TURN_END) {
		bool ok = turn == LOOPTEST_TURN_RETRY ?
			  looptest_reconcile(a, e, &lt) :
			  looptest_event(a, e, &lt, &ring, &ev);

		if (ok)
			continue;
		oc = CLI_VERB_ERROR;
		break;
	}
	stopped = looptest_stop(a, e, &lt);

	return stopped && (oc == CLI_VERB_SIGNAL || oc == CLI_VERB_EXPIRED);
}

static bool cid_run(const struct cli_args *a, struct cli_engine *e)
{
	/* Type 2 changes only detection. Neutral channel defaults must preserve
	 * gain and echo cancellation while the acknowledgement detector runs. */
	struct udsp_chan_cfg chan = {
		.detect_mask = UDSP_DET_DTMF,
		.gain_tx = 0,
		.gain_rx = 0,
		.ec_enable = true,
		.ec_nlp_enable = false,
		.ec_tail_ms = 0,
	};
	struct udsp_cid cid;
	enum udsp_cid_kind kind = a->type2 ? UDSP_CID_TYPE2 : UDSP_CID_TYPE1;
	uint32_t deadline = verb_deadline(a, e, 0);
	uint64_t wall_deadline = verb_wall_deadline(a, e, 0);
	struct udsp_event ev;

	if (a->base.mwi >= 0)
		kind = UDSP_CID_TYPE1;
	if (kind == UDSP_CID_TYPE2 &&
	    !verb_status_ok(a, "udsp_chan_config",
			    udsp_chan_config(e->base.s, a->base.line, &chan)))
		return false;
	tool_cid_fill(&a->base, &cid);
	if (!verb_status_ok(a, "udsp_cid_send",
			    udsp_cid_send(e->base.s, a->base.line, kind, &cid)))
		return false;

	for (;;) {
		enum cli_wait_outcome oc = verb_event_next(e, deadline,
							   wall_deadline,
							   "CID_SENT or CID2_TIMEOUT",
							   &ev);

		if (oc == CLI_VERB_SIGNAL)
			fprintf(stderr, "udsp-cli: cid: abandoned by signal\n");
		if (oc != CLI_VERB_CONTINUE)
			return false;
		if (ev.line != a->base.line)
			continue;
		if (ev.type == UDSP_EV_CID2_TIMEOUT) {
			printf("cid status=timeout\n");
			return false;
		}
		if (ev.type != UDSP_EV_CID_SENT)
			continue;
		printf("cid status=%d\n", ev.u.cid_sent.status);
		return ev.u.cid_sent.status == 0;
	}
}

static bool events_run(const struct cli_args *a, struct cli_engine *e)
{
	struct udsp_chan_cfg chan = {
		.detect_mask = UDSP_DET_DTMF | UDSP_DET_PULSE,
		.ec_enable = true,
	};
	const struct udsp_cap *cap = udsp_cap(e->base.s);
	uint32_t deadline = verb_deadline(a, e, 0);
	struct udsp_event ev;

	if (!cap)
		return false;
	for (unsigned line = 0; line < cap->lines; line++) {
		if (!verb_status_ok(a, "udsp_chan_config",
				    udsp_chan_config(e->base.s, line, &chan)))
			return false;
	}

	for (;;) {
		enum tool_wait got = tool_event_wait(&e->base, deadline, 0,
						     true, &ev);

		if (got == TOOL_WAIT_EXPIRED) {
			wait_expired_print(e, "event stream", deadline);
			return true;
		}
		if (got == TOOL_WAIT_WALL_EXPIRED)
			return false;
		if (got == TOOL_WAIT_STOPPED)
			return true;
		if (got == TOOL_WAIT_ERROR || e->base.faulted)
			return false;
	}
}

static int real_run(const struct cli_args *a)
{
	struct cli_engine e;
	bool ran;
	bool stopped;

	tool_stopped = 0;
	tool_signals_install();
	if (!engine_open_real(a, &e)) {
		engine_close_real(&e);
		return CLI_EXIT_ERROR;
	}

	ran = a->verb->run(a, &e);
	stopped = engine_close_real(&e);

	return ran && stopped ? CLI_EXIT_OK : CLI_EXIT_ERROR;
}

static int call_run(const struct cli_args *a)
{
	fprintf(stderr, "udsp-cli: call: not implemented yet\n");

	return CLI_EXIT_TODO;
}

int main(int argc, char **argv)
{
	struct cli_args a;
	enum tool_parse rc;

	if (argc < 2) {
		usage_fail("a verb is required", NULL);
		return CLI_EXIT_ERROR;
	}

	if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		usage(stdout);
		return CLI_EXIT_OK;
	}

	args_defaults(&a);
	a.verb = verb_find(argv[1]);
	if (!a.verb) {
		usage_fail("unknown verb", argv[1]);
		return CLI_EXIT_ERROR;
	}

	rc = args_parse(argc - 1, argv + 1, &a);
	if (rc == TOOL_PARSE_ERROR)
		return CLI_EXIT_ERROR;
	if (rc == TOOL_PARSE_HELP) {
		usage(stdout);
		return CLI_EXIT_OK;
	}

	tool_file_defaults(&a.base);
	if (!verb_args_valid(&a))
		return CLI_EXIT_ERROR;

	if (a.verb->engine)
		cfg_print(&a);

	return a.verb->exec ? a.verb->exec(&a) : real_run(&a);
}
