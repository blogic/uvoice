// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * udsp-linecap, the line capture bench tool. It runs one line action on the
 * real engine (a ring with caller identity, a standalone type 1 record, a
 * visual message waiting record, a dial tone, or nothing) while the SLIC
 * transmit path streams to a loopback RTP socket the tool reads itself, and
 * writes every payload as PCMU after a versioned capture header. The host tools
 * linecap-analyse.py and fsk-demod.py turn that file into a timeline and into
 * the bytes the line carried.
 *
 * Why the transmit path shows the receive signal. The SLIC synthesises a
 * 600 ohm source and balances its hybrid for a matched load. An on-hook
 * terminal leaves the loop open, so the receive signal the part drives onto
 * tip and ring reflects into the transmit path at about 0 dB: the file shows
 * what the line carried, level and timing included. The same path is exact
 * digital silence while the on-hook line sits in the active feed.
 *
 * One owner. The engine needs the ALSA front end and the spidev to itself, so
 * uvoiced must be stopped first, exactly as for udsp-cli. The tool waits
 * --settle-ms after bring-up before it arms anything: the boot hook report
 * arrives before the DC feed has settled, and a hook edge inside a sequence
 * aborts it.
 *
 * Exit codes. 0 when the action reached its terminal event and the capture
 * closed; 1 on an argument, operation, fault, wait or signal failure; 2 on a
 * usage error.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "udsp/event.h"
#include "udsp/udsp.h"

#include "../core/udsp_priv.h"

#define TOOL_PROG	"udsp-linecap"
#include "tool_common.h"

#define LC_EXIT_OK		0
#define LC_EXIT_ERROR		1
#define LC_EXIT_USAGE		2

#define LC_ACTION_NET_MS	20000u
#define LC_SETTLE_DEFAULT_MS	1500u
#define LC_SETTLE_MAX_MS	10000u
/* The playback pipeline plus the engine's own completion drain, so the
 * capture holds the whole tail of an action before the stream stops. */
#define LC_DRAIN_MS		700u
#define LC_DURATION_MAX_MS	60000u
#define LC_RING_DURATION_MS	7000u
#define LC_TONE_DURATION_MS	2000u
#define LC_IDLE_DURATION_MS	3000u
#define LC_OUT_DEFAULT		"/tmp/linecap.ul"
#define LC_SLIC_PLUGIN_DEFAULT	"/usr/lib/udsp/udsp-slic-proslic-mt798x.so"
#define LC_PATH_MAX		256
#define LC_RTP_HEADER		12u
#define LC_RTP_VERSION		2u
#define LC_RX_BUF		2048u
/* A capture on the unpaced file back end arrives faster than real time. */
#define LC_RX_SO_RCVBUF		(1 << 20)
#define LC_STREAM		0u
#define LC_PTIME_MS		20u
#define LC_DTMF_PT		101u
#define LC_CAPTURE_HEADER_BYTES	16u
#define LC_CAPTURE_VERSION	1u
#define LC_CAPTURE_PCMU		0u

/* The header says 8000 Hz under --rate 16000 too: a refused 16 kHz ask
 * degrades inside udsp_open, and the PCMU payload stays an 8 kHz stream
 * across the rate conversion. */
static const uint8_t lc_capture_header[LC_CAPTURE_HEADER_BYTES] = {
	'U', 'D', 'S', 'P', 'L', 'C', 'A', 'P',
	LC_CAPTURE_VERSION, LC_CAPTURE_PCMU,
	LC_CAPTURE_HEADER_BYTES, 0,
	0x40, 0x1f, 0, 0,
};

struct lc_args {
	struct tool_args base;
	const char *verb;
	char out[LC_PATH_MAX];
	unsigned duration_ms;
	unsigned settle_ms;
	bool ec;
};

_Static_assert(offsetof(struct lc_args, base) == 0,
	       "opt_apply converts the base pointer back");

struct lc_capture {
	int fd;
	FILE *file;
	pthread_t thread;
	bool live;
	atomic_bool stop;
	uint64_t packets;
	uint64_t bytes;
	uint64_t lost;
	uint16_t seq;
	bool seq_valid;
};

struct lc_run {
	struct tool_engine base;
	int rtp_fd;
	int rtcp_fd;
	int sink_fd;
	struct lc_capture cap;
};

/* --- arguments ----------------------------------------------------------- */

static const char *const lc_verbs[] = { "ring", "cid", "vmwi", "tone", "idle" };

static void usage(FILE *out)
{
	fprintf(out,
		"usage: udsp-linecap <verb> [options]\n"
		"\n"
		"verbs:\n"
		"  ring     ring the line, with caller identity when --cid-* is given\n"
		"  cid      send a standalone type 1 caller identity record\n"
		"  vmwi     send a visual message waiting record (--mwi on|off)\n"
		"  tone     play the region dial tone for --duration\n"
		"  idle     capture the idle line for --duration\n"
		"\n"
		"options:\n"
		"  --out <path>                versioned PCMU capture (default %s)\n"
		"  --tdm-backend <alsa|alsa-raw|file>\n"
		"                              tdm plug-in (default alsa-raw)\n"
		"  --playback <dev|path>       playback device or file\n"
		"  --capture <dev|path>        capture device or file\n"
		"  --slic-plugin <path>        slic .so; the default is %s\n"
		"                              on hardware, the fake on file\n"
		"  --spidev <path>             spidev node of the slic\n"
		"                              (default " TOOL_SPIDEV_DEFAULT ")\n"
		"  --gpio-reset <name>         slic reset gpio name\n"
		"  --gpio-interrupt <name>     slic interrupt gpio name\n"
		"  --ports <1-%d>               populated fxs ports\n"
		"  --channels <1-2>            tdm channels to open\n"
		"  --prefill <1-%u>           playback pre-fill periods\n"
		"  --rate <8000|16000>         line sample rate (default 8000)\n"
		"  --cpu <-1-%d>             engine cpu, -1 does not pin\n"
		"  --prio <0-%d>              engine SCHED_FIFO priority\n"
		"  --region <ISO3>             region to apply at open\n"
		"  --line <0-%u>               line number (default 0)\n"
		"  --cid-name <text>           caller name, at most 32 characters\n"
		"  --cid-number <text>         caller number, at most 32 characters\n"
		"  --cid-format <name>         bellcore-fsk (default), etsi-fsk,\n"
		"                              etsi-rpas, etsi-dtas, dtmf, ntt,\n"
		"                              etsi-lr-dtas, or dtmf-dtas\n"
		"  --cid-at-ring <0-%u>         ring break for the record (default %u)\n"
		"  --cid-wait-ms <0-%u>      delay from the break to the burst\n"
		"                              (default %u)\n"
		"  --mwi <on|off>              the vmwi record to send\n"
		"  --duration <ms>             ring, tone or idle length\n"
		"                              (defaults %u, %u, %u)\n"
		"  --settle-ms <0-%u>       wait after bring-up before the action\n"
		"                              (default %u)\n"
		"  --ec                        keep the echo canceller on; the\n"
		"                              engine skips it on an open loop\n"
		"  -h, --help                  this text\n"
		"\n"
		"The engine must own the audio front end and the spidev: stop\n"
		"uvoiced first. The capture is what the SLIC transmit path carried;\n"
		"linecap-analyse.py and fsk-demod.py read it on the host.\n",
		LC_OUT_DEFAULT, LC_SLIC_PLUGIN_DEFAULT, UDSP_LINE_MAX,
		TOOL_PREFILL_MAX, TOOL_CPU_MAX, TOOL_PRIO_MAX,
		UDSP_LINE_MAX - 1u, TOOL_CID_AT_RING_MAX,
		TOOL_CID_AT_RING_DEFAULT, TOOL_CID_WAIT_MAX_MS,
		TOOL_CID_WAIT_DEFAULT_MS, LC_RING_DURATION_MS,
		LC_TONE_DURATION_MS, LC_IDLE_DURATION_MS, LC_SETTLE_MAX_MS,
		LC_SETTLE_DEFAULT_MS);
}

static int usage_fail(const char *what, const char *arg)
{
	if (arg)
		fprintf(stderr, "udsp-linecap: %s: %s\n", what, arg);
	else
		fprintf(stderr, "udsp-linecap: %s\n", what);
	usage(stderr);
	return LC_EXIT_USAGE;
}

static void args_defaults(struct lc_args *a)
{
	struct udsp_config *cfg = &a->base.cfg;

	memset(a, 0, sizeof(*a));
	tool_args_defaults(&a->base);
	tool_str_set(cfg->region, sizeof(cfg->region), "USA");
	cfg->populated_ports = 1;
	cfg->prefill_periods = 5;
	tool_str_set(a->out, sizeof(a->out), LC_OUT_DEFAULT);
	a->settle_ms = LC_SETTLE_DEFAULT_MS;
}

enum {
	LC_OPT_OUT = TOOL_OPT_LOCAL,
	LC_OPT_SETTLE_MS,
	LC_OPT_EC,
	LC_OPT_END,
};

_Static_assert(LC_OPT_END - TOOL_OPT_TDM_BACKEND <= 64,
	       "every option needs a bit of struct tool_args given");

static const struct option lc_options[] = {
	TOOL_OPTIONS
	{ "out", required_argument, NULL, LC_OPT_OUT },
	{ "settle-ms", required_argument, NULL, LC_OPT_SETTLE_MS },
	{ "ec", no_argument, NULL, LC_OPT_EC },
	{ NULL, 0, NULL, 0 },
};

/* The tool's own options, and the shared ones whose meaning is the tool's:
 * --region names the region applied at open, and --duration is the length
 * of the action. */
static bool opt_apply(struct tool_args *t, const struct option *o,
		      const char *val)
{
	struct lc_args *a = (struct lc_args *)t;
	long n = 0;

	switch (o->val) {
	case TOOL_OPT_REGION:
		return tool_opt_text(o, val, t->cfg.region,
				     sizeof(t->cfg.region));
	case TOOL_OPT_DURATION:
		if (!tool_opt_number(o, val, 1, LC_DURATION_MAX_MS, &n))
			return false;
		a->duration_ms = (unsigned)n;
		return true;
	case LC_OPT_OUT:
		return tool_opt_text(o, val, a->out, sizeof(a->out));
	case LC_OPT_SETTLE_MS:
		if (!tool_opt_number(o, val, 0, LC_SETTLE_MAX_MS, &n))
			return false;
		a->settle_ms = (unsigned)n;
		return true;
	case LC_OPT_EC:
		a->ec = true;
		return true;
	}

	return tool_opt_apply(t, o, val);
}

static bool verb_valid(const char *verb)
{
	for (size_t i = 0; i < sizeof(lc_verbs) / sizeof(lc_verbs[0]); i++) {
		if (!strcmp(lc_verbs[i], verb))
			return true;
	}

	return false;
}

static bool verb_is(const struct lc_args *a, const char *verb)
{
	return !strcmp(a->verb, verb);
}

/*
 * The file back end is the host leg: unpaced, bounded by the tick net through
 * the knob the engine reads, a synthetic ramp for capture and no playback file
 * unless asked, and the in-tree fake SLIC. The ALSA leg is the board and
 * defaults to the ProSLIC plug-in.
 */
static bool file_backend_prepare(struct lc_args *a)
{
	struct udsp_config *cfg = &a->base.cfg;

	if (!tool_backend_is_file(cfg)) {
		if (!tool_given(&a->base, TOOL_OPT_SLIC_PLUGIN))
			tool_str_set(cfg->slic_plugin, sizeof(cfg->slic_plugin),
				     LC_SLIC_PLUGIN_DEFAULT);
		return true;
	}
	tool_file_defaults(&a->base);

	return tool_file_ticks_set(cfg);
}

static int args_parse(int argc, char **argv, struct lc_args *a)
{
	enum tool_parse rc;
	bool identity;

	args_defaults(a);
	if (argc < 2)
		return usage_fail("a verb is required", NULL);
	if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		usage(stdout);
		return -1;
	}
	if (!verb_valid(argv[1]))
		return usage_fail("unknown verb", argv[1]);
	a->verb = argv[1];

	optind = 2;
	opterr = 1;
	rc = tool_args_parse(argc, argv, lc_options, &a->base, opt_apply);
	if (rc == TOOL_PARSE_HELP) {
		usage(stdout);
		return -1;
	}
	if (rc == TOOL_PARSE_ERROR) {
		usage(stderr);
		return LC_EXIT_USAGE;
	}

	identity = tool_cid_given(&a->base);
	if (verb_is(a, "cid") && !identity)
		return usage_fail("cid requires --cid-name or --cid-number", NULL);
	if (verb_is(a, "vmwi") && a->base.mwi < 0)
		return usage_fail("vmwi requires --mwi on or off", NULL);
	if (!verb_is(a, "vmwi") && a->base.mwi >= 0)
		return usage_fail("--mwi applies to vmwi alone", NULL);
	if ((verb_is(a, "tone") || verb_is(a, "idle") || verb_is(a, "vmwi")) &&
	    identity)
		return usage_fail("caller identity does not apply to verb", a->verb);
	if (!tool_given(&a->base, TOOL_OPT_DURATION)) {
		a->duration_ms = verb_is(a, "ring") ? LC_RING_DURATION_MS :
				 verb_is(a, "tone") ? LC_TONE_DURATION_MS :
				 LC_IDLE_DURATION_MS;
	}
	if (!file_backend_prepare(a))
		return LC_EXIT_USAGE;

	return LC_EXIT_OK;
}

/* --- the capture side ---------------------------------------------------- */

static int udp_local_bind(uint16_t *port)
{
	struct sockaddr_in addr = { .sin_family = AF_INET };
	socklen_t len = sizeof(addr);
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -errno;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) ||
	    getsockname(fd, (struct sockaddr *)&addr, &len)) {
		int err = errno;

		close(fd);
		return -err;
	}
	*port = ntohs(addr.sin_port);
	return fd;
}

static int udp_local_connect(uint16_t peer_port)
{
	struct sockaddr_in addr = { .sin_family = AF_INET };
	uint16_t own = 0;
	int fd = udp_local_bind(&own);

	if (fd < 0)
		return fd;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(peer_port);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
		int err = errno;

		close(fd);
		return -err;
	}
	return fd;
}

static void capture_packet(struct lc_capture *c, const uint8_t *buf, size_t len)
{
	size_t header;
	uint16_t seq;

	if (len < LC_RTP_HEADER || (buf[0] >> 6) != LC_RTP_VERSION)
		return;
	header = LC_RTP_HEADER + 4u * (buf[0] & 0x0fu);
	if (len <= header)
		return;
	seq = (uint16_t)((buf[2] << 8) | buf[3]);
	if (c->seq_valid)
		c->lost += (uint16_t)(seq - c->seq - 1u);
	c->seq = seq;
	c->seq_valid = true;
	c->packets++;
	c->bytes += len - header;
	fwrite(buf + header, 1, len - header, c->file);
}

static void capture_drain(struct lc_capture *c)
{
	uint8_t buf[LC_RX_BUF];
	ssize_t n;

	while ((n = recv(c->fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0)
		capture_packet(c, buf, (size_t)n);
}

/* An unpaced engine sends thousands of packets a second, so every wake-up
 * empties the socket, and the stop is followed by one last sweep. */
static void *capture_thread(void *arg)
{
	struct lc_capture *c = arg;

	while (!atomic_load_explicit(&c->stop, memory_order_acquire)) {
		struct pollfd p = { .fd = c->fd, .events = POLLIN };

		if (poll(&p, 1, TOOL_POLL_MS) > 0)
			capture_drain(c);
	}
	capture_drain(c);
	return NULL;
}

static bool capture_open(struct lc_run *r, const struct lc_args *a)
{
	struct lc_capture *c = &r->cap;
	int rcvbuf = LC_RX_SO_RCVBUF;
	uint16_t rx_port = 0, sink_port = 0;

	c->fd = udp_local_bind(&rx_port);
	r->sink_fd = udp_local_bind(&sink_port);
	if (c->fd < 0 || r->sink_fd < 0) {
		fprintf(stderr, "udsp-linecap: loopback socket: %s\n",
			strerror(c->fd < 0 ? -c->fd : -r->sink_fd));
		return false;
	}
	setsockopt(c->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	r->rtp_fd = udp_local_connect(rx_port);
	/* RTCP stays off, so its socket only has to be a bound, connected
	 * descriptor the library can dup; the sink never reads it. */
	r->rtcp_fd = udp_local_connect(sink_port);
	if (r->rtp_fd < 0 || r->rtcp_fd < 0) {
		fprintf(stderr, "udsp-linecap: loopback connect: %s\n",
			strerror(r->rtp_fd < 0 ? -r->rtp_fd : -r->rtcp_fd));
		return false;
	}
	c->file = fopen(a->out, "wb");
	if (!c->file) {
		fprintf(stderr, "udsp-linecap: %s: %s\n", a->out, strerror(errno));
		return false;
	}
	if (fwrite(lc_capture_header, 1, sizeof(lc_capture_header), c->file) !=
	    sizeof(lc_capture_header)) {
		fprintf(stderr, "udsp-linecap: %s: %s\n", a->out, strerror(errno));
		return false;
	}
	atomic_init(&c->stop, false);
	if (pthread_create(&c->thread, NULL, capture_thread, c)) {
		fprintf(stderr, "udsp-linecap: capture thread: %s\n", strerror(errno));
		return false;
	}
	c->live = true;
	return true;
}

static void capture_close(struct lc_run *r)
{
	struct lc_capture *c = &r->cap;

	if (c->live) {
		atomic_store_explicit(&c->stop, true, memory_order_release);
		pthread_join(c->thread, NULL);
		c->live = false;
	}
	if (c->file) {
		fclose(c->file);
		c->file = NULL;
	}
	if (c->fd >= 0)
		close(c->fd);
	if (r->sink_fd >= 0)
		close(r->sink_fd);
	if (r->rtp_fd >= 0)
		close(r->rtp_fd);
	if (r->rtcp_fd >= 0)
		close(r->rtcp_fd);
	c->fd = r->sink_fd = r->rtp_fd = r->rtcp_fd = -1;
}

/* --- events -------------------------------------------------------------- */

/* Waits for the event type, printing the rest; a fault or a signal cuts the
 * wait. deadline_ms is on the engine clock and wall_ms on the host clock. */
static enum tool_wait event_wait_for(struct lc_run *r, uint16_t type,
				     uint32_t deadline_ms, uint64_t wall_ms,
				     struct udsp_event *out)
{
	for (;;) {
		struct udsp_event ev;
		enum tool_wait got = tool_event_wait(&r->base, deadline_ms,
						     wall_ms, true, &ev);

		if (got != TOOL_WAIT_GOT)
			return got;
		if (r->base.faulted)
			return TOOL_WAIT_ERROR;
		if (ev.type != type)
			continue;
		if (out)
			*out = ev;
		return TOOL_WAIT_GOT;
	}
}

/* Passes engine time while printing events; a stop or a fault cuts it. Either
 * bound ends the sleep as planned. */
static enum tool_wait engine_sleep(struct lc_run *r, uint32_t ms)
{
	uint32_t deadline = udsp_now_ms(r->base.s) + ms;
	uint64_t wall = tool_mono_ms() + TOOL_JOB_WAIT_MS;

	for (;;) {
		struct udsp_event ev;
		enum tool_wait got = tool_event_wait(&r->base, deadline, wall,
						     true, &ev);

		if (got == TOOL_WAIT_EXPIRED || got == TOOL_WAIT_WALL_EXPIRED)
			return TOOL_WAIT_GOT;
		if (got != TOOL_WAIT_GOT || r->base.faulted)
			return got == TOOL_WAIT_GOT ? TOOL_WAIT_ERROR : got;
	}
}

/* --- the engine ---------------------------------------------------------- */

/* The canceller would subtract the very reflection the capture is for. */
static bool channel_prepare(struct lc_run *r, const struct lc_args *a)
{
	struct udsp_chan_cfg chan;

	memset(&chan, 0, sizeof(chan));
	chan.ec_enable = a->ec;
	return tool_status_ok("udsp_chan_config",
			      udsp_chan_config(r->base.s, a->base.line, &chan));
}

static bool stream_open(struct lc_run *r, const struct lc_args *a)
{
	struct udsp_media m;

	memset(&m, 0, sizeof(m));
	m.local.ipver = 4;
	m.remote.ipver = 4;
	m.codec = UDSP_SEL_PCMU;
	m.codec_pt = 0;
	m.dtmf_pt = LC_DTMF_PT;
	m.dtmf_mode = UDSP_DTMF_INBAND;
	m.direction = UDSP_MEDIA_SENDONLY;
	m.ptime_ms = LC_PTIME_MS;
	m.jb_mode = UDSP_JB_FIXED;
	m.jb_init_ms = LC_PTIME_MS;
	m.jb_max_ms = 200;
	if (!tool_status_ok("udsp_stream_config",
			    udsp_stream_config(r->base.s, a->base.line,
					       LC_STREAM, &m, r->rtp_fd,
					       r->rtcp_fd)))
		return false;
	return tool_status_ok("udsp_stream_start",
			      udsp_stream_start(r->base.s, a->base.line,
						LC_STREAM));
}

static enum tool_wait action_ring(struct lc_run *r, const struct lc_args *a)
{
	struct udsp_ring ring;

	memset(&ring, 0, sizeof(ring));
	ring.cadence[0].on_ms = TOOL_RING_ON_MS;
	ring.cadence[0].off_ms = TOOL_RING_OFF_MS;
	ring.cadence_count = 1;
	ring.duration_ms = a->duration_ms;
	ring.cid_present = tool_cid_given(&a->base);
	ring.cid_at_ring = a->base.cid_at_ring;
	ring.cid_wait_ms = a->base.cid_wait_ms;
	tool_cid_fill(&a->base, &ring.cid);
	printf("ring on_ms=%d off_ms=%d duration_ms=%u cid=%d cid_at_ring=%u "
	       "cid_wait_ms=%u\n", TOOL_RING_ON_MS, TOOL_RING_OFF_MS,
	       a->duration_ms, ring.cid_present, a->base.cid_at_ring,
	       a->base.cid_wait_ms);
	if (!tool_status_ok("udsp_ring_start",
			    udsp_ring_start(r->base.s, a->base.line, &ring)))
		return TOOL_WAIT_ERROR;
	return event_wait_for(r, UDSP_EV_RING_END,
			      udsp_now_ms(r->base.s) + a->duration_ms +
			      LC_ACTION_NET_MS,
			      tool_mono_ms() + a->duration_ms + LC_ACTION_NET_MS,
			      NULL);
}

static enum tool_wait action_cid(struct lc_run *r, const struct lc_args *a)
{
	struct udsp_cid cid;
	struct udsp_event ev;
	enum tool_wait got;

	tool_cid_fill(&a->base, &cid);
	if (!tool_status_ok("udsp_cid_send",
			    udsp_cid_send(r->base.s, a->base.line,
					  UDSP_CID_TYPE1, &cid)))
		return TOOL_WAIT_ERROR;
	got = event_wait_for(r, UDSP_EV_CID_SENT,
			     udsp_now_ms(r->base.s) + LC_ACTION_NET_MS,
			     tool_mono_ms() + LC_ACTION_NET_MS, &ev);
	if (got == TOOL_WAIT_GOT && ev.u.cid_sent.status) {
		fprintf(stderr, "udsp-linecap: the record was not delivered: "
			"status %d\n", ev.u.cid_sent.status);
		return TOOL_WAIT_ERROR;
	}
	return got;
}

static enum tool_wait action_tone(struct lc_run *r, const struct lc_args *a)
{
	if (!tool_status_ok("udsp_tone_play",
			    udsp_tone_play(r->base.s, a->base.line,
					   UDSP_TONE_DIAL, a->duration_ms)))
		return TOOL_WAIT_ERROR;
	return event_wait_for(r, UDSP_EV_AUDIO_END,
			      udsp_now_ms(r->base.s) + a->duration_ms +
			      LC_ACTION_NET_MS,
			      tool_mono_ms() + a->duration_ms + LC_ACTION_NET_MS,
			      NULL);
}

static enum tool_wait action_run(struct lc_run *r, const struct lc_args *a)
{
	if (verb_is(a, "ring"))
		return action_ring(r, a);
	if (verb_is(a, "cid") || verb_is(a, "vmwi"))
		return action_cid(r, a);
	if (verb_is(a, "tone"))
		return action_tone(r, a);
	return engine_sleep(r, a->duration_ms);
}

static const char *wait_name(enum tool_wait w)
{
	switch (w) {
	case TOOL_WAIT_GOT:
		return "ok";
	case TOOL_WAIT_EXPIRED:
	case TOOL_WAIT_WALL_EXPIRED:
		return "expired";
	case TOOL_WAIT_STOPPED:
		return "signal";
	case TOOL_WAIT_ERROR:
		break;
	}
	return "error";
}

/* The outcome is already chosen; a failed stop prints its reason and does
 * not change it. */
static void run_close(struct lc_run *r, const struct lc_args *a)
{
	if (r->base.s && r->base.started)
		udsp_stream_stop(r->base.s, a->base.line, LC_STREAM);
	tool_engine_stop(&r->base);
	tool_engine_close(&r->base);
}

static void summary_print(const struct lc_run *r, const struct lc_args *a,
			  enum tool_wait outcome)
{
	const struct lc_capture *c = &r->cap;

	printf("result = %s\n", wait_name(outcome));
	printf("capture_path = %s\n", a->out);
	printf("capture_packets = %llu\n", (unsigned long long)c->packets);
	printf("capture_lost = %llu\n", (unsigned long long)c->lost);
	printf("capture_samples = %llu\n", (unsigned long long)c->bytes);
	printf("capture_ms = %llu\n",
	       (unsigned long long)(c->bytes / (UDSP_SAMPLE_RATE / 1000u)));
}

int main(int argc, char **argv)
{
	struct lc_args a;
	struct lc_run r;
	enum tool_wait outcome = TOOL_WAIT_ERROR;
	int rc;

	rc = args_parse(argc, argv, &a);
	if (rc < 0)
		return LC_EXIT_OK;
	if (rc)
		return rc;

	memset(&r, 0, sizeof(r));
	tool_engine_init(&r.base, NULL);
	r.rtp_fd = r.rtcp_fd = r.sink_fd = -1;
	r.cap.fd = -1;
	tool_signals_install();

	if (!tool_engine_open(&r.base, &a.base.cfg, "") ||
	    !tool_engine_start(&r.base, a.base.line) ||
	    !channel_prepare(&r, &a) || !capture_open(&r, &a) ||
	    !stream_open(&r, &a))
		goto out;

	printf("settle_ms = %u\n", a.settle_ms);
	if (engine_sleep(&r, a.settle_ms) != TOOL_WAIT_GOT)
		goto out;
	printf("action = %s\n", a.verb);
	outcome = action_run(&r, &a);
	if (outcome == TOOL_WAIT_GOT &&
	    engine_sleep(&r, LC_DRAIN_MS) != TOOL_WAIT_GOT)
		outcome = TOOL_WAIT_ERROR;
	if (r.base.faulted)
		outcome = TOOL_WAIT_ERROR;

out:
	run_close(&r, &a);
	capture_close(&r);
	summary_print(&r, &a, outcome);
	return outcome == TOOL_WAIT_GOT ? LC_EXIT_OK : LC_EXIT_ERROR;
}
