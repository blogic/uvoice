// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the call trace controller (src/calltrace.c).
 *
 * Isolation. calltrace.c, trace.c, trace_ring.c and pcapng.c are compiled
 * into this binary. The suite defines every renderer and every getter that
 * the controller reaches outside them, in place of the daemon. The rings and
 * the pcapng export are the real ones, so a case reads a real file back.
 *
 * No child runs. calltrace_test_mode() keeps the argument list of every spawn
 * and starts nothing. Each case completes a child by hand. The suite
 * therefore drives the production state machine and not a copy of it.
 *
 * Time. The suite drives uloop to a deadline, as the timing rule of the
 * package requires, and never sleeps on a wall clock. The first cases check
 * the shipped values of the five cadences: the snapshot, the export, the wire
 * poll, the kill deadline and the finisher budget. The environment then
 * shrinks all five for the rest of the suite.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libubox/blobmsg_json.h>
#include <libubox/uloop.h>

#include "calltrace.h"
#include "cdr.h"
#include "line/line.h"
#include "media_trace.h"
#include "net/iface.h"
#include "net/ports.h"
#include "proto/proto.h"
#include "proto/proto_tap.h"
#include "proto/sip/fsm.h"
#include "spawn_run.h"
#include "status.h"
#include "trace.h"
#include "trace_ring.h"
#include "uvoice.h"
#include "test_util.h"
#include "test_warn.h"

#define CT_WIRE_SH	"/usr/share/uvoice/calltrace-wire.sh"
#define CT_FINISH_SH	"/usr/share/uvoice/calltrace-finish.sh"

/* ------------------------------------------------------- the daemon's side */

struct uvoice uv;

static struct cfg fake_cfg;
static struct hal_cap fake_cap = {
	.backend = "stub",
	.lines = 2,
	.streams_per_chan = 2,
};
static int fake_hal;

static const struct hal_cap *fake_cap_get(struct hal *h)
{
	return &fake_cap;
}

const struct hal_ops hal_backend_ops = {
	.name = "stub",
	.cap_get = fake_cap_get,
};

/*
 * The machine state of each line, driven by the cases. The suite plays the
 * protocol with the SIP machine's vocabulary: the names the call trace
 * receives and the rest class line/line.c derives from the line flags.
 */
static enum fsm_state line_state[CFG_CHANNEL_MAX];

const char *line_state_name(unsigned int line)
{
	if (line >= CFG_CHANNEL_MAX)
		return NULL;

	return fsm_state_name(line_state[line]);
}

enum line_rest line_rest_of(unsigned int line)
{
	if (line >= CFG_CHANNEL_MAX || line_state[line] == FSM_OOS)
		return LINE_REST_OOS;

	return line_state[line] == FSM_IDLE ? LINE_REST_IDLE :
	       LINE_REST_ACTIVE;
}

static const char * const state_names[] = {
	"IDLE", "OFFHOOK_DIAL", "COLLECTING", "SPRE_COLLECT", "CALLING",
	"PROCEEDING", "CONNECTED", "RINGING", "CW_OFFERED", "HOLD_DIAL",
	"TWO_CALLS", "CONF", "PENDING", "HOLDING", "CLEARING", "ROH", "OOS",
	"XFER_COLLECT", "SILENT",
};

const char *fsm_state_name(enum fsm_state state)
{
	if ((size_t)state >= sizeof(state_names) / sizeof(state_names[0]))
		return "?";

	return state_names[state];
}

/* The renderers, each writing the key the controller expects under it. */
void status_hal_dump(struct blob_buf *b)
{
	void *tbl = blobmsg_open_table(b, "hal");

	blobmsg_add_string(b, "state", "ready");
	blobmsg_add_u32(b, "lines", fake_cap.lines);
	blobmsg_close_table(b, tbl);

	tbl = blobmsg_open_table(b, "events");
	blobmsg_add_u64(b, "read", 0);
	blobmsg_close_table(b, tbl);

	tbl = blobmsg_open_array(b, "jb_events");
	blobmsg_close_array(b, tbl);
}

void status_line_rows_dump(struct blob_buf *b, unsigned int line)
{
	void *arr;

	arr = blobmsg_open_array(b, "streams");
	blobmsg_close_array(b, arr);
	arr = blobmsg_open_array(b, "jb");
	blobmsg_close_array(b, arr);
	arr = blobmsg_open_array(b, "lec");
	blobmsg_close_array(b, arr);
	arr = blobmsg_open_array(b, "monitor");
	blobmsg_close_array(b, arr);
}

void status_diag_dump(struct blob_buf *b, int want_line)
{
	void *tbl = blobmsg_open_table(b, "diag");
	void *arr = blobmsg_open_array(b, "streams");

	blobmsg_close_array(b, arr);
	blobmsg_close_table(b, tbl);
}

void status_streams_dump(struct blob_buf *b)
{
}

void status_jb_dump(struct blob_buf *b)
{
}

void status_lec_dump(struct blob_buf *b)
{
}

void status_monitor_dump(struct blob_buf *b)
{
}

static void fake_status_dump_lines(struct blob_buf *b, int want_line)
{
	void *arr = blobmsg_open_array(b, "lines");
	void *entry = blobmsg_open_table(b, NULL);

	blobmsg_add_u32(b, "line", want_line < 0 ? 0 : (uint32_t)want_line);
	blobmsg_close_table(b, entry);
	blobmsg_close_array(b, arr);
}

void net_iface_status_dump(struct blob_buf *b)
{
	void *tbl = blobmsg_open_table(b, "network");

	blobmsg_add_string(b, "address", "192.0.2.10");
	blobmsg_close_table(b, tbl);
}

/*
 * The backend mask push the controller makes at every state change. The real
 * one (media_trace.c media_trace_sync) builds the mask from
 * calltrace_capturing(), and the packet trace of the backend follows that bit,
 * so what a case has to see is the controller's own state at the moment of the
 * push and whether the rings the backend's last drain writes into were still
 * open.
 */
#define SYNC_MAX	8u
/* The tick of the record the fake final drain writes, so a case can find it
 * in the exported file. */
#define SYNC_DRAIN_TICK	0x5eedf00dull

static unsigned int sync_calls;
static bool sync_capturing;
static bool sync_rings_open;
static bool sync_cap_at[SYNC_MAX];
static bool sync_open_at[SYNC_MAX];
static bool sync_lec_capturing;
static bool sync_lec_at[SYNC_MAX];
static bool sync_lec_was;
/* The answer of the fake final drain, 1 while none ran. */
static int sync_drain_rc = 1;
/* The payload of the packet that the fake final packet drain writes, so a
 * case can find it in the exported file. */
static const char sync_pkt_mark[] = "the fake final packet drain";
static bool sync_cap_was;
/* The answer of the fake final packet drain, 1 while none ran. */
static int sync_pkt_rc = 1;

static void lec_hdr_fill(struct trace_lec_hdr *hdr, uint8_t kind,
			 uint64_t tick)
{
	memset(hdr, 0, sizeof(*hdr));
	hdr->stamp_ns = tick * 10000000ull;
	hdr->tick = tick;
	hdr->samples = TRACE_LEC_SAMPLES_WB;
	hdr->kind = kind;
}

static int rtp_data_write(const void *data, size_t len)
{
	struct trace_endpoint ep;

	memset(&ep, 0, sizeof(ep));
	ep.ipver = 4;
	ep.addr[0] = 192;
	ep.addr[3] = 1;
	ep.port = 10000;

	return trace_ring_packet(TRACE_RING_RTP, TRACE_PKT_RX, TRACE_PKT_RTP,
				 &ep, &ep, 0, data, len);
}

/*
 * The canceller capture follows calltrace_lec_capturing() the same way. The
 * backend drains the last records of its ring inside the push that lowers
 * the bit (udsp.c lec_capture_stop()), so the fake writes one record at that
 * moment, as such a drain would. The packet trace does the same when
 * calltrace_capturing() falls (udsp.c pkt_trace_arm()). A case then finds
 * each record in the exported file, or finds that it came too late.
 */
void media_trace_sync(void)
{
	struct trace_lec_hdr hdr;

	sync_capturing = calltrace_capturing();
	sync_lec_capturing = calltrace_lec_capturing();
	sync_rings_open = trace_ring_is_open();
	if (sync_calls < SYNC_MAX) {
		sync_cap_at[sync_calls] = sync_capturing;
		sync_lec_at[sync_calls] = sync_lec_capturing;
		sync_open_at[sync_calls] = sync_rings_open;
	}
	sync_calls++;

	if (sync_lec_was && !sync_lec_capturing) {
		lec_hdr_fill(&hdr, TRACE_LEC_END, SYNC_DRAIN_TICK);
		sync_drain_rc = trace_ring_lec(&hdr, 0, NULL, NULL, NULL);
	}
	sync_lec_was = sync_lec_capturing;

	if (sync_cap_was && !sync_capturing)
		sync_pkt_rc = rtp_data_write(sync_pkt_mark,
					     sizeof(sync_pkt_mark));
	sync_cap_was = sync_capturing;
}

static void sync_reset(void)
{
	sync_calls = 0;
	sync_capturing = false;
	sync_lec_capturing = false;
	sync_rings_open = false;
	memset(sync_cap_at, 0, sizeof(sync_cap_at));
	memset(sync_lec_at, 0, sizeof(sync_lec_at));
	memset(sync_open_at, 0, sizeof(sync_open_at));
	sync_drain_rc = 1;
	sync_pkt_rc = 1;
}

/* the netdev the tracker answers for the line's interface; a case moves it */
static const char *bind_device_answer = "voice0";

const char *net_iface_bind_device(const char *iface)
{
	return bind_device_answer;
}

int net_iface_address(uint8_t ipver, uint8_t addr[16])
{
	return -ENOENT;
}

/* the transport counters of the protocol, under its own key */
static void fake_wire_stats(struct blob_buf *b)
{
	void *tbl = blobmsg_open_table(b, "sip");

	blobmsg_add_u64(b, "rx", 3);
	blobmsg_add_u64(b, "tx", 4);
	blobmsg_close_table(b, tbl);
}

static void fake_status_dump_call(struct blob_buf *b, unsigned int line)
{
	void *arr = blobmsg_open_array(b, "call");

	blobmsg_close_array(b, arr);
}

/* the two signaling ports of the line, the shape of the SIP table: the local
 * one, and the remote one when it differs */
static unsigned int fake_wire_ports(unsigned int line, uint16_t *ports,
				    unsigned int max)
{
	const struct cfg_channel *ch = &fake_cfg.channel[line];
	unsigned int n = 0;

	if (n < max)
		ports[n++] = (uint16_t)ch->sip_local_port;
	if (ch->sip_port != ch->sip_local_port && n < max)
		ports[n++] = (uint16_t)ch->sip_port;

	return n;
}

void net_port_range(const struct cfg_channel *ch, uint16_t *min,
		    uint16_t *max)
{
	*min = 10000;
	*max = 10100;
}

/*
 * Three records: the leg of this window, the leg that took its call over, and
 * an unrelated one. The end snapshot must keep the first two and drop the
 * third.
 */
static void cdr_entry(struct blob_buf *b, uint32_t seq, uint32_t from)
{
	void *tbl = blobmsg_open_table(b, NULL);

	blobmsg_add_u32(b, "seq", seq);
	blobmsg_add_u32(b, "line", 0);
	if (from)
		blobmsg_add_u32(b, "transferred_from", from);
	blobmsg_close_table(b, tbl);
}

/* The oldest and the newest of a long run, for the case that fills the
 * window's list; every other case leaves them out and reads three records. */
static bool cdr_wide;

void cdr_dump(struct blob_buf *b)
{
	void *arr = blobmsg_open_array(b, "cdr");

	if (cdr_wide)
		cdr_entry(b, 1, 0);
	cdr_entry(b, 7, 0);
	cdr_entry(b, 8, 7);
	cdr_entry(b, 9, 0);
	if (cdr_wide)
		cdr_entry(b, 65, 0);
	blobmsg_close_array(b, arr);
}

void cdr_stats_dump(struct blob_buf *b, int want_line)
{
	void *arr = blobmsg_open_array(b, "stats");

	blobmsg_close_array(b, arr);
}

void cfg_dump(const struct cfg *cfg, struct blob_buf *b)
{
	blobmsg_add_u8(b, "loaded", 1);
	blobmsg_add_u32(b, "channels", cfg ? cfg->channels : 0);
}

void lines_region_status_dump(struct blob_buf *b)
{
	void *tbl = blobmsg_open_table(b, "region");

	blobmsg_add_string(b, "applied", "USA");
	blobmsg_close_table(b, tbl);
}

static proto_tap_fn tap_installed;

static void fake_tap_set(proto_tap_fn fn)
{
	tap_installed = fn;
}

/* The protocol of this suite: the entries the controller reads. */
static const struct proto_ops fake_proto = {
	.name = "sip",
	.status_dump_call = fake_status_dump_call,
	.status_dump_lines = fake_status_dump_lines,
	.wire_ports = fake_wire_ports,
	.tap_set = fake_tap_set,
	.wire_stats = fake_wire_stats,
};

/* The same entries under the name of the MGCP table, and the state of
 * proto/proto.c that tells a running protocol from the null table. */
static const struct proto_ops fake_mgcp_proto = {
	.name = "mgcp",
	.status_dump_call = fake_status_dump_call,
	.status_dump_lines = fake_status_dump_lines,
	.wire_ports = fake_wire_ports,
	.tap_set = fake_tap_set,
	.wire_stats = fake_wire_stats,
};

static bool fake_mgcp;
static bool fake_running = true;

const struct proto_ops *proto_ops_get(void)
{
	return fake_mgcp ? &fake_mgcp_proto : &fake_proto;
}

bool proto_running(void)
{
	return fake_running;
}

pid_t spawn_run(char *const argv[], enum spawn_group group)
{
	errno = ENOSYS;

	return -1;
}

/* --------------------------------------------------------------- the pump */

static struct uloop_timeout pump_timer;
static unsigned int pump_ticks;
static unsigned int pump_limit;

static void pump_cb(struct uloop_timeout *t)
{
	if (++pump_ticks >= pump_limit) {
		uloop_end();
		return;
	}

	uloop_timeout_set(t, 5);
}

static void pump(unsigned int ms)
{
	pump_ticks = 0;
	pump_limit = ms / 5 + 1;
	pump_timer.cb = pump_cb;
	uloop_timeout_set(&pump_timer, 5);
	uloop_run();
}

/* -------------------------------------------------------------- the fixture */

static char tmp_root[96];
static char case_root[160];
static char work_dir[224];
static unsigned int case_seq;

static void state_set(unsigned int line, enum fsm_state after)
{
	enum fsm_state before = line_state[line];

	line_state[line] = after;
	calltrace_line_state(line, fsm_state_name(before),
			     fsm_state_name(after));
}

static void env_reset(void)
{
	unsigned int i;

	calltrace_test_mode(true);
	for (i = 0; i < CFG_CHANNEL_MAX; i++)
		line_state[i] = FSM_IDLE;

	memset(&fake_cfg, 0, sizeof(fake_cfg));
	fake_cfg.loaded = true;
	fake_cfg.enabled = true;
	fake_cfg.channels = 2;
	fake_cfg.hal.engine_cpu = -1;
	fake_cfg.channel[0].enabled = true;
	fake_cfg.channel[0].sip_local_port = 5060;
	fake_cfg.channel[0].sip_port = 5060;
	fake_cfg.channel[1].enabled = true;
	fake_cfg.channel[1].sip_local_port = 5062;
	fake_cfg.channel[1].sip_port = 5060;

	uv.cfg = &fake_cfg;
	uv.hal = (struct hal *)&fake_hal;
	uv.config_generation = 1;
	tap_installed = NULL;
	work_dir[0] = '\0';
	/* A reset abandons a running trace without a push, so the fake drains
	 * must not see the next push as the end of that trace. */
	sync_lec_was = false;
	sync_cap_was = false;

	snprintf(case_root, sizeof(case_root), "%s/c%u", tmp_root, ++case_seq);
	calltrace_test_root(case_root);
}

static struct calltrace_options opt_make(unsigned int timeout_s,
					 unsigned int grace_s)
{
	struct calltrace_options o = {
		.timeout_s = timeout_s,
		.grace_s = grace_s,
		.arm_timeout_s = 600,
		.wire_max_mb = 8,
		.rings_max_mb = 8,
		.lec_max_mb = 8,
		.wire = true,
	};

	return o;
}

/* ------------------------------------------------------- reading the reply */

static struct blob_buf sb;
static struct blob_buf jb;

static struct blob_attr *attr_find(struct blob_attr *tbl, const char *name)
{
	struct blob_attr *cur;
	size_t rem;

	blobmsg_for_each_attr(cur, tbl, rem) {
		const char *n = blobmsg_name(cur);

		if (n && !strcmp(n, name))
			return cur;
	}

	return NULL;
}

static void status_take(void)
{
	blob_buf_init(&sb, 0);
	calltrace_status_dump(&sb);
}

static bool status_has(const char *key)
{
	return attr_find(sb.head, key) != NULL;
}

/* Whether the rings table of a fresh status reply holds the row ring. */
static bool ring_row_has(const char *ring)
{
	struct blob_attr *rings;

	status_take();
	rings = attr_find(sb.head, "rings");

	return rings && attr_find(rings, ring) != NULL;
}

static bool status_is(const char *key, const char *want)
{
	struct blob_attr *a = attr_find(sb.head, key);

	return a && !strcmp(blobmsg_get_string(a), want);
}

static uint32_t status_u32(const char *key)
{
	struct blob_attr *a = attr_find(sb.head, key);

	return a ? blobmsg_get_u32(a) : 0;
}

static const char *status_str(const char *key)
{
	struct blob_attr *a = attr_find(sb.head, key);

	return a ? blobmsg_get_string(a) : "";
}

static bool state_is(const char *want)
{
	status_take();

	return status_is("state", want);
}

/* ---------------------------------------------------------- the cadences */

static void cadence_tests(void)
{
	CASE("the shipped snapshot cadence is ten seconds");
	calltrace_test_mode(true);
	CHECK_UINT(calltrace_test_cadence_ms(CALLTRACE_CADENCE_SNAPSHOT),
		   10000);

	CASE("the shipped export cadence is five seconds");
	CHECK_UINT(calltrace_test_cadence_ms(CALLTRACE_CADENCE_EXPORT), 5000);

	CASE("the shipped wire poll is one second");
	CHECK_UINT(calltrace_test_cadence_ms(CALLTRACE_CADENCE_WIRE_POLL),
		   1000);

	CASE("the shipped kill deadline is three seconds");
	CHECK_UINT(calltrace_test_cadence_ms(CALLTRACE_CADENCE_KILL), 3000);

	CASE("the shipped base of the finisher budget is sixty seconds");
	CHECK_UINT(calltrace_test_cadence_ms(CALLTRACE_CADENCE_FINISH), 60000);

	CASE("the shipped export cadence with the lec ring open is one second");
	CHECK_UINT(calltrace_test_cadence_ms(CALLTRACE_CADENCE_EXPORT_LEC),
		   1000);

	setenv("UVOICE_CALLTRACE_SNAPSHOT_MS", "60", 1);
	setenv("UVOICE_CALLTRACE_EXPORT_MS", "40", 1);
	setenv("UVOICE_CALLTRACE_WIRE_POLL_MS", "30", 1);
	setenv("UVOICE_CALLTRACE_KILL_MS", "50", 1);
	setenv("UVOICE_CALLTRACE_FINISH_MS", "150", 1);
	setenv("UVOICE_CALLTRACE_EXPORT_LEC_MS", "20", 1);
	calltrace_test_mode(true);

	CASE("the environment shrinks all five for the suite");
	CHECK_UINT(calltrace_test_cadence_ms(CALLTRACE_CADENCE_SNAPSHOT), 60);
	CHECK_UINT(calltrace_test_cadence_ms(CALLTRACE_CADENCE_EXPORT), 40);
	CHECK_UINT(calltrace_test_cadence_ms(CALLTRACE_CADENCE_WIRE_POLL), 30);
	CHECK_UINT(calltrace_test_cadence_ms(CALLTRACE_CADENCE_KILL), 50);
	CHECK_UINT(calltrace_test_cadence_ms(CALLTRACE_CADENCE_FINISH), 150);

	CASE("and it shrinks the export cadence with the lec ring open too");
	CHECK_UINT(calltrace_test_cadence_ms(CALLTRACE_CADENCE_EXPORT_LEC), 20);
}

/*
 * Everything below needs the rings. A libubox without the udebug producer
 * compiles the ring half of trace_ring.c to stubs that answer -ENOSYS, so the
 * controller refuses an arm and there is no capture to drive: that build runs
 * the refusal and the cadences and skips the rest with its reason.
 */
#ifdef UVOICE_HAVE_UDEBUG

static uint8_t wire_u8(const char *key)
{
	struct blob_attr *wire, *val;

	status_take();
	wire = attr_find(sb.head, "wire");
	val = wire ? attr_find(wire, key) : NULL;

	return val ? blobmsg_get_u8(val) : 0xff;
}

static uint32_t wire_u32(const char *key)
{
	struct blob_attr *wire, *val;

	status_take();
	wire = attr_find(sb.head, "wire");
	val = wire ? attr_find(wire, key) : NULL;

	return val ? blobmsg_get_u32(val) : 0xffffffffu;
}

/* bytes travels as a u64, and the u32 getter would read the high half of it
 * and answer 0 for every size below 4 GiB. */
static uint64_t wire_u64(const char *key)
{
	struct blob_attr *wire, *val;

	status_take();
	wire = attr_find(sb.head, "wire");
	val = wire ? attr_find(wire, key) : NULL;

	return val ? blobmsg_get_u64(val) : ~0ull;
}

/* The work dir of the armed trace, kept so a case can read its files after
 * the reply stopped naming it. */
static void work_dir_take(void)
{
	status_take();
	snprintf(work_dir, sizeof(work_dir), "%s", status_str("workdir"));
}

/* A failed export names its file and errno in failure, and last_error holds
 * the same text until a refused arm writes over it. */
static bool failure_names(const char *file)
{
	status_take();

	return status_has("failure") &&
	       !strcmp(status_str("failure"), status_str("last_error")) &&
	       strstr(status_str("failure"), file) != NULL;
}

/* ------------------------------------------------------- reading the files */

static char *file_slurp(const char *path)
{
	char *text;
	ssize_t got;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;

	text = malloc(512 * 1024 + 1);
	if (!text) {
		close(fd);
		return NULL;
	}

	got = read(fd, text, 512 * 1024);
	close(fd);
	if (got < 0) {
		free(text);
		return NULL;
	}

	text[got] = '\0';

	return text;
}

static void work_path(char *out, size_t len, const char *name)
{
	snprintf(out, len, "%s/%s", work_dir, name);
}

static bool json_take(const char *path)
{
	char *text = file_slurp(path);
	bool ok;

	if (!text)
		return false;

	blob_buf_init(&jb, 0);
	ok = blobmsg_add_json_from_string(&jb, text);
	free(text);

	return ok;
}

static bool json_has(const char *key)
{
	return attr_find(jb.head, key) != NULL;
}

static bool json_is(const char *key, const char *want)
{
	struct blob_attr *a = attr_find(jb.head, key);

	return a && !strcmp(blobmsg_get_string(a), want);
}

static unsigned int json_array_count(const char *key)
{
	struct blob_attr *a = attr_find(jb.head, key);
	struct blob_attr *cur;
	unsigned int n = 0;
	size_t rem;

	blobmsg_for_each_attr(cur, a, rem)
		n++;

	return n;
}

/* The first member of a string array, or the empty string. */
static const char *json_array_first(const char *key)
{
	struct blob_attr *a = attr_find(jb.head, key);
	struct blob_attr *cur;
	size_t rem;

	blobmsg_for_each_attr(cur, a, rem)
		return blobmsg_get_string(cur);

	return "";
}

static bool manifest_take(void)
{
	char path[256];

	work_path(path, sizeof(path), "manifest.json");

	return json_take(path);
}

static bool manifest_is(const char *key, const char *want)
{
	return manifest_take() && json_is(key, want);
}

/*
 * One line of snapshots.jsonl as a parsed object. kind selects the lines that
 * count, NULL takes every line, and a negative index reads the last of them.
 * The periodic cadence runs while a case waits for an event, so a case that
 * wants an event snapshot has to name the kind and not a position.
 */
static bool snapshot_line_is(const char *line, const char *kind)
{
	char needle[32];

	if (!kind)
		return true;

	snprintf(needle, sizeof(needle), "\"kind\":\"%s\"", kind);

	return strstr(line, needle) != NULL;
}

static bool snapshot_take_kind(const char *kind, int index)
{
	char path[256];
	char *text, *line, *save;
	char *want = NULL;
	int seen = 0;
	bool ok = false;

	work_path(path, sizeof(path), "snapshots.jsonl");
	text = file_slurp(path);
	if (!text)
		return false;

	for (line = strtok_r(text, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		if (!snapshot_line_is(line, kind))
			continue;
		if (index < 0 || seen == index)
			want = line;
		seen++;
		if (index >= 0 && seen > index)
			break;
	}

	if (want) {
		blob_buf_init(&jb, 0);
		ok = blobmsg_add_json_from_string(&jb, want);
	}
	free(text);

	return ok;
}

static bool snapshot_take(int index)
{
	return snapshot_take_kind(NULL, index);
}

static unsigned int snapshot_count_kind(const char *kind)
{
	char path[256];
	char *text, *line, *save;
	unsigned int n = 0;

	work_path(path, sizeof(path), "snapshots.jsonl");
	text = file_slurp(path);
	if (!text)
		return 0;

	for (line = strtok_r(text, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		if (snapshot_line_is(line, kind))
			n++;
	}
	free(text);

	return n;
}

static bool file_exists(const char *path)
{
	return !access(path, F_OK);
}

static void file_fill(const char *path, size_t bytes)
{
	char block[4096];
	size_t wrote = 0;
	int fd;

	memset(block, 0xa5, sizeof(block));
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return;

	while (wrote < bytes) {
		size_t want = bytes - wrote;

		if (want > sizeof(block))
			want = sizeof(block);
		if (write(fd, block, want) != (ssize_t)want)
			break;
		wrote += want;
	}
	close(fd);
}

/* A file with a size and no data. The finisher budget reads st_size, so a
 * case gets the size of a large capture without the write. */
static bool file_sparse(const char *path, off_t bytes)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	bool ok;

	if (fd < 0)
		return false;

	ok = !ftruncate(fd, bytes);
	close(fd);

	return ok;
}

static bool file_write_text(const char *path, const char *text)
{
	size_t len = strlen(text);
	ssize_t wrote;
	int fd;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return false;

	wrote = write(fd, text, len);
	close(fd);

	return wrote == (ssize_t)len;
}

/*
 * One capture envelope per state, for the schema check of
 * schema/uvoice-api-schema.json. The directory comes from the
 * environment, so a plain run writes nothing and a schema run gets the reply
 * the production renderer built.
 */
static void capture_write(const char *state)
{
	const char *dir = getenv("UVOICE_CALLTRACE_CAPTURE_DIR");
	struct blob_buf env = {};
	char path[256];
	char *json;
	void *tbl;

	if (!dir || !*dir)
		return;

	blob_buf_init(&env, 0);
	blobmsg_add_string(&env, "object", "voice.calltrace");
	blobmsg_add_string(&env, "method", "status");
	tbl = blobmsg_open_table(&env, "reply");
	calltrace_status_dump(&env);
	blobmsg_close_table(&env, tbl);

	json = blobmsg_format_json(env.head, true);
	if (json) {
		snprintf(path, sizeof(path), "%s/status-%s.json", dir, state);
		file_write_text(path, json);
		free(json);
	}
	blob_buf_free(&env);
}

/* ------------------------------------------------------------- the helpers */

/* A child that never started answers NULL for every argument, so a case that
 * is about to fail reports it instead of walking off the end. */
static bool argv_is(int child, unsigned int i, const char *want)
{
	const char *got = calltrace_test_argv(child, i);

	return got && !strcmp(got, want);
}

static bool argv_has(int child, unsigned int i, const char *needle)
{
	const char *got = calltrace_test_argv(child, i);

	return got && strstr(got, needle);
}

static int arm(unsigned int line, const struct calltrace_options *o)
{
	int err = calltrace_arm(line, o);

	if (!err)
		work_dir_take();

	return err;
}

/*
 * The freeze asks the wire capture to go and starts the finisher at its reap,
 * so a case that wants the finisher completes the capture first. A trace that
 * ran no capture needs none of this.
 */
static void wire_reap(int code)
{
	calltrace_test_child_done(CALLTRACE_CHILD_WIRE, code);
}

/* The leg of a normal call, from the seize to the rest. */
static void call_run(unsigned int line)
{
	state_set(line, FSM_OFFHOOK_DIAL);
	state_set(line, FSM_COLLECTING);
	state_set(line, FSM_CALLING);
	state_set(line, FSM_PROCEEDING);
	state_set(line, FSM_CONNECTED);
	state_set(line, FSM_IDLE);
}

/* -------------------------------------------------------------- the window */

static void normal_call_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);
	char path[256];
	char id[80];

	CASE("an arm on an enabled line the board carries is taken");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	CHECK(state_is("armed"));
	CHECK(calltrace_capturing());

	CASE("the arm installs the SIP tap and sets the ring mask");
	CHECK(tap_installed != NULL);
	CHECK(trace_domain_active(TRACE_SIP));
	CHECK(trace_domain_active(TRACE_JB));
	CHECK(trace_ring_is_open());

	CASE("the arm writes the config and opens the export");
	work_path(path, sizeof(path), "config.json");
	CHECK(file_exists(path));
	work_path(path, sizeof(path), "rings.pcapng");
	CHECK(file_exists(path));

	CASE("the wire child names the script, the device and the line's "
	     "ports");
	CHECK(argv_is(CALLTRACE_CHILD_WIRE, 0, CT_WIRE_SH));
	CHECK(argv_is(CALLTRACE_CHILD_WIRE, 1, work_dir));
	CHECK(argv_is(CALLTRACE_CHILD_WIRE, 3, "voice0"));
	CHECK(argv_has(CALLTRACE_CHILD_WIRE, 4, "portrange 10000-10100"));
	CHECK(calltrace_test_argv(CALLTRACE_CHILD_WIRE, 5) == NULL);

	CASE("a line at rest waits for a call and not for a rest");
	status_take();
	CHECK(status_is("waiting_for", "call"));

	CASE("the manifest stage after the arm is armed");
	CHECK(manifest_is("stage", "armed"));

	CASE("the first move out of rest starts the window");
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK(state_is("running"));
	status_take();
	CHECK_UINT(status_u32("calls"), 1);
	CHECK(!status_has("waiting_for"));
	snprintf(id, sizeof(id), "%s", status_str("id"));

	CASE("and the manifest stage moves with it");
	CHECK(manifest_is("stage", "running"));

	CASE("the rest of the call leaves the window running");
	state_set(0, FSM_COLLECTING);
	state_set(0, FSM_CALLING);
	state_set(0, FSM_PROCEEDING);
	state_set(0, FSM_CONNECTED);
	CHECK(state_is("running"));

	CASE("the line's rest arms the grace and the window still runs");
	state_set(0, FSM_IDLE);
	CHECK(state_is("running"));

	CASE("the grace elapses and the freeze reaches the finisher");
	pump(1300);
	CHECK(state_is("finishing"));
	wire_reap(0);
	CHECK(argv_is(CALLTRACE_CHILD_FINISH, 0, CT_FINISH_SH));
	wire_reap(0);
	CHECK(argv_is(CALLTRACE_CHILD_FINISH, 1, work_dir));
	CHECK(manifest_is("stage", "finishing"));
	CHECK(manifest_is("reason", "call_ended"));

	CASE("the rings and the tap are gone before the finisher runs");
	CHECK(!trace_ring_is_open());
	CHECK(tap_installed == NULL);
	CHECK(!trace_domain_active(TRACE_SIP));

	CASE("a finisher that exits 0 publishes the archive");
	wire_reap(0);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	CHECK(state_is("done"));
	status_take();
	CHECK(status_is("reason", "call_ended"));
	CHECK(status_has("archive"));
	CHECK(strstr(status_str("archive"), id) != NULL);
	CHECK(manifest_is("stage", "done"));

	CASE("and the controller is no longer capturing");
	CHECK(!calltrace_capturing());

	CASE("a finisher that fails leaves the trace failed with its code");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	call_run(0);
	pump(1300);
	CHECK(state_is("finishing"));
	wire_reap(0);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 3);
	CHECK(state_is("failed"));
	status_take();
	CHECK(status_is("reason", "finisher_failed"));
	CHECK(status_is("last_error", "exit 3"));
	CHECK(manifest_is("stage", "failed"));

	CASE("and its work dir is kept");
	CHECK(file_exists(work_dir));

	CASE("and failure names the same exit as last_error");
	status_take();
	CHECK(status_is("failure", "exit 3"));

	/* last_error answers the caller the arm refused, and failure keeps
	 * what ended the trace that still stands in the reply. */
	CASE("a refused arm over a failed trace moves last_error alone");
	CHECK_UINT(calltrace_arm(7, &o), -EINVAL);
	status_take();
	CHECK(status_is("state", "failed"));
	CHECK(status_is("last_error", "bad_line"));
	CHECK(status_is("failure", "exit 3"));

	CASE("and the next arm that is taken carries no failure");
	CHECK_UINT(arm(1, &o), 0);
	status_take();
	CHECK(status_is("state", "armed"));
	CHECK(!status_has("failure"));
	CHECK(!status_has("last_error"));
	CHECK_UINT(calltrace_cancel(), 0);
	wire_reap(0);
}

static void invite_fail_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);

	CASE("a call that fails at the INVITE still opens a window");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_CALLING);
	CHECK(state_is("running"));

	CASE("and its clearing chain ends the window on the grace");
	state_set(0, FSM_CLEARING);
	state_set(0, FSM_IDLE);
	pump(1300);
	CHECK(state_is("finishing"));
	status_take();
	CHECK_UINT(status_u32("calls"), 1);
	CHECK(manifest_is("reason", "call_ended"));
	wire_reap(0);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	status_take();
	CHECK(status_is("reason", "call_ended"));
}

static void second_call_tests(void)
{
	struct calltrace_options o = opt_make(30, 2);

	CASE("a second seize inside the grace continues the window");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	state_set(0, FSM_IDLE);
	pump(300);
	CHECK(state_is("running"));
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK(state_is("running"));

	CASE("and the window counts two calls");
	status_take();
	CHECK_UINT(status_u32("calls"), 2);

	CASE("the second rest ends the window after its own grace");
	state_set(0, FSM_IDLE);
	pump(2300);
	CHECK(state_is("finishing"));
}

static void timeout_tests(void)
{
	struct calltrace_options o = opt_make(1, 5);

	CASE("the window timeout ends a call that is still up");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	state_set(0, FSM_CONNECTED);
	pump(1300);
	CHECK(state_is("finishing"));
	CHECK(manifest_is("reason", "timeout"));
	wire_reap(0);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	status_take();
	CHECK(status_is("reason", "timeout"));
}

static void oos_tests(void)
{
	struct calltrace_options o = opt_make(30, 5);

	CASE("a line forced out of service ends the window at once");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	state_set(0, FSM_OOS);
	CHECK(state_is("finishing"));
	CHECK(manifest_is("reason", "line_restart"));
	wire_reap(0);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	status_take();
	CHECK(status_is("reason", "line_restart"));
}

static void offhook_arm_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);

	CASE("a line that is off hook at the arm waits for its rest");
	env_reset();
	line_state[0] = FSM_CONNECTED;
	CHECK_UINT(arm(0, &o), 0);
	status_take();
	CHECK(status_is("waiting_for", "line_idle"));

	CASE("its rest moves the wait to the next call");
	state_set(0, FSM_IDLE);
	CHECK(state_is("armed"));
	status_take();
	CHECK(status_is("waiting_for", "call"));

	CASE("and the next seize starts the window");
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK(state_is("running"));
}

/* -------------------------------------------------------------- the cancel */

/*
 * The capture device of the wire child (ct_wire_iface()): the netdev the
 * tracker resolves for the line's interface, while its name fits the argument
 * buffer, and every device otherwise.
 */
static void wire_iface_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);

	CASE("wire iface: a resolved name that fits names the capture device");
	env_reset();
	bind_device_answer = "lan4";
	CHECK_UINT(arm(0, &o), 0);
	CHECK(argv_is(CALLTRACE_CHILD_WIRE, 3, "lan4"));
	CHECK_UINT(calltrace_cancel(), 0);

	CASE("wire iface: a name the buffer cannot hold captures on every "
	     "device");
	env_reset();
	bind_device_answer = "uvoice-a-netdev-name-of-thirty";
	CHECK_UINT(arm(0, &o), 0);
	CHECK(argv_is(CALLTRACE_CHILD_WIRE, 3, "any"));
	CHECK_UINT(calltrace_cancel(), 0);

	CASE("wire iface: no resolved device captures on every device");
	env_reset();
	bind_device_answer = NULL;
	CHECK_UINT(arm(0, &o), 0);
	CHECK(argv_is(CALLTRACE_CHILD_WIRE, 3, "any"));
	CHECK_UINT(calltrace_cancel(), 0);

	bind_device_answer = "voice0";
}

static void cancel_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);

	CASE("a cancel with no trace answers not found");
	env_reset();
	CHECK_UINT(calltrace_cancel(), -ENOENT);

	CASE("a cancel while armed returns to idle and names the reason");
	CHECK_UINT(arm(0, &o), 0);
	CHECK_UINT(calltrace_cancel(), 0);
	CHECK(state_is("idle"));
	status_take();
	CHECK(status_is("reason", "cancelled"));
	CHECK(!status_has("id"));
	CHECK(!status_has("options"));

	CASE("and it removes the work dir, the rings and the tap");
	CHECK(!file_exists(work_dir));
	CHECK(!trace_ring_is_open());
	CHECK(tap_installed == NULL);

	CASE("a cancel while running freezes the window");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK_UINT(calltrace_cancel(), 0);
	CHECK(state_is("finishing"));
	CHECK(manifest_is("reason", "cancelled"));

	CASE("a cancel while finishing answers not found");
	CHECK_UINT(calltrace_cancel(), -ENOENT);

	CASE("a cancel after the archive answers not found");
	wire_reap(0);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	CHECK(state_is("done"));
	status_take();
	CHECK(status_is("reason", "cancelled"));
	CHECK_UINT(calltrace_cancel(), -ENOENT);

	CASE("a cancel after a failure answers not found");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK_UINT(calltrace_cancel(), 0);
	wire_reap(0);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 1);
	CHECK(state_is("failed"));
	CHECK_UINT(calltrace_cancel(), -ENOENT);
}

/*
 * The backend mask follows the state, at every edge. The media datagrams of a
 * trace reach the archive through a backend bit, and the controller's ring
 * mask moves before its state does: the arm sets the mask while the state is
 * still idle and the freeze clears it while the trace still runs. So the
 * controller pushes the mask itself at every state change, and these cases
 * pin the four edges that decide whether the capture runs at all.
 */
static void state_sync_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);
	struct rlimit saved, small;
	unsigned int i;

	CASE("the arm pushes the mask with the trace capturing and the rings "
	     "open");
	env_reset();
	sync_reset();
	CHECK_UINT(arm(0, &o), 0);
	CHECK_UINT(sync_calls, 2);
	CHECK(sync_capturing);
	CHECK(sync_rings_open);

	CASE("the window start pushes again and still asks for the capture");
	sync_reset();
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK(state_is("running"));
	CHECK_UINT(sync_calls, 1);
	CHECK(sync_capturing);

	/* The push is what takes the backend's packet trace down, and its last
	 * drain has to land in the ring the export then reads, so the freeze
	 * pushes before it closes the export and frees the rings. */
	CASE("the freeze pushes the mask down while the rings still take a "
	     "drain");
	state_set(0, FSM_IDLE);
	sync_reset();
	pump(1300);
	CHECK(state_is("finishing"));
	CHECK_UINT(sync_calls, 1);
	CHECK(!sync_capturing);
	CHECK(sync_rings_open);

	CASE("the finisher's answer pushes once more with nothing to capture");
	sync_reset();
	wire_reap(0);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	CHECK(state_is("done"));
	CHECK_UINT(sync_calls, 1);
	CHECK(!sync_capturing);

	CASE("a cancel while armed pushes the mask down");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	sync_reset();
	CHECK_UINT(calltrace_cancel(), 0);
	CHECK(state_is("idle"));
	CHECK_UINT(sync_calls, 1);
	CHECK(!sync_capturing);

	/* The daemon stop runs the freeze without the finisher, so it needs the
	 * freeze order too: the push has to reach the backend while the rings
	 * can still take the drain it triggers. */
	CASE("a daemon stop pushes the mask down while the rings are open");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK(state_is("running"));
	sync_reset();
	calltrace_shutdown();
	CHECK(state_is("idle"));
	CHECK_UINT(sync_calls, 2);
	/* The first of the two is the one that matters: it runs before the
	 * export finishes and before the rings go. */
	CHECK(!sync_cap_at[0]);
	CHECK(sync_open_at[0]);
	CHECK(!sync_capturing);
	CHECK(!trace_ring_is_open());

	CASE("a daemon stop while armed pushes the mask down as well");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	sync_reset();
	calltrace_shutdown();
	CHECK(state_is("idle"));
	CHECK_UINT(sync_calls, 2);
	CHECK(!sync_cap_at[0]);
	CHECK(sync_open_at[0]);
	CHECK(!sync_capturing);

	CASE("a failure while armed pushes the mask down");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	for (i = 0; i < 4000; i++)
		TRACE(TRACE_SIP, "a filler line %u for the export to carry", i);

	sync_reset();
	getrlimit(RLIMIT_FSIZE, &saved);
	small = saved;
	small.rlim_cur = 1;
	signal(SIGXFSZ, SIG_IGN);
	setrlimit(RLIMIT_FSIZE, &small);
	pump(300);
	setrlimit(RLIMIT_FSIZE, &saved);
	signal(SIGXFSZ, SIG_DFL);
	CHECK(state_is("failed"));
	CHECK_UINT(sync_calls, 1);
	CHECK(!sync_capturing);
}

static void arm_expire_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);

	CASE("an arm nobody used expires back to idle");
	env_reset();
	o.arm_timeout_s = 1;
	CHECK_UINT(arm(0, &o), 0);
	pump(1300);
	CHECK(state_is("idle"));
	status_take();
	CHECK(status_is("reason", "arm_expired"));
	CHECK(!trace_ring_is_open());
	CHECK(!file_exists(work_dir));
}

/* ------------------------------------------------------------ the refusals */

static void refusal_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);

	CASE("a line the board does not carry is a bad line");
	env_reset();
	CHECK_UINT(calltrace_arm(7, &o), -EINVAL);
	status_take();
	CHECK(status_is("last_error", "bad_line"));

	CASE("a line whose channel is disabled is a bad line too");
	fake_cfg.channel[1].enabled = false;
	CHECK_UINT(calltrace_arm(1, &o), -EINVAL);
	status_take();
	CHECK(status_is("last_error", "bad_line"));

	CASE("an arm with no backend is a bad line");
	uv.hal = NULL;
	CHECK_UINT(calltrace_arm(0, &o), -EINVAL);
	uv.hal = (struct hal *)&fake_hal;

	CASE("a timeout past the bound is a bad range");
	env_reset();
	o = opt_make(1801, 1);
	CHECK_UINT(calltrace_arm(0, &o), -EINVAL);
	status_take();
	CHECK(status_is("last_error", "bad_range"));

	CASE("a grace past the bound is a bad range");
	o = opt_make(30, 31);
	CHECK_UINT(calltrace_arm(0, &o), -EINVAL);
	status_take();
	CHECK(status_is("last_error", "bad_range"));

	CASE("a wire cap of no megabytes is a bad range");
	o = opt_make(30, 1);
	o.wire_max_mb = 0;
	CHECK_UINT(calltrace_arm(0, &o), -EINVAL);
	status_take();
	CHECK(status_is("last_error", "bad_range"));

	CASE("a ring cap of no megabytes is a bad range");
	o = opt_make(30, 1);
	o.rings_max_mb = 0;
	CHECK_UINT(calltrace_arm(0, &o), -EINVAL);
	status_take();
	CHECK(status_is("last_error", "bad_range"));

	CASE("a filesystem that cannot hold the trace refuses it");
	o = opt_make(30, 1);
	calltrace_test_space(1024);
	CHECK_UINT(calltrace_arm(0, &o), -ENOSPC);
	status_take();
	CHECK(status_is("last_error", "no_space"));
	CHECK(!trace_ring_is_open());
	calltrace_test_space(0);

	CASE("a second arm while one is armed is refused as busy");
	CHECK_UINT(arm(0, &o), 0);
	CHECK_UINT(calltrace_arm(1, &o), -EBUSY);
	status_take();
	CHECK(status_is("last_error", "busy"));

	CASE("and so is one while a window runs");
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK_UINT(calltrace_arm(1, &o), -EBUSY);

	CASE("and one while the finisher runs");
	CHECK_UINT(calltrace_cancel(), 0);
	CHECK(state_is("finishing"));
	CHECK_UINT(calltrace_arm(1, &o), -EBUSY);

	CASE("an arm after the archive is taken again");
	wire_reap(0);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	CHECK_UINT(arm(1, &o), 0);
	CHECK(state_is("armed"));

	CASE("and the arm clears the error the refusal left");
	status_take();
	CHECK(!status_has("last_error"));

	/* The word of a refused arm stands until the next arm, whatever the
	 * trace that was running does in the meantime. */
	CASE("a refusal word survives the cancel of the trace that caused it");
	CHECK_UINT(calltrace_arm(0, &o), -EBUSY);
	status_take();
	CHECK(status_is("last_error", "busy"));
	CHECK_UINT(calltrace_cancel(), 0);
	status_take();
	CHECK(status_is("state", "idle"));
	CHECK(status_is("reason", "cancelled"));
	CHECK(status_is("last_error", "busy"));

	CASE("and the next arm is what clears it");
	wire_reap(0);
	CHECK_UINT(arm(1, &o), 0);
	status_take();
	CHECK(!status_has("last_error"));
	CHECK_UINT(calltrace_cancel(), 0);
	wire_reap(0);
}

/* -------------------------------------------- the dir of a trace that ended */

/*
 * A trace on line 0 whose finisher exited 3, so it keeps its work dir. The
 * arm after it names line 1: an id holds the second and the line, so an arm
 * on line 0 in the same second names the same dir.
 */
static bool failed_trace_make(void)
{
	struct calltrace_options o = opt_make(30, 1);

	o.wire = false;
	if (arm(0, &o))
		return false;

	state_set(0, FSM_OFFHOOK_DIAL);
	calltrace_cancel();
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 3);

	return state_is("failed") && file_exists(work_dir);
}

/*
 * /tmp is RAM, and while the daemon runs nothing else removes the work dir of
 * a trace that ended. The arm after it removes the dir once the preflight
 * passed, so the space check counts its bytes as free. The caps of opt_make()
 * and the headroom ask for 32 MiB.
 */
static void reclaim_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);
	struct rlimit saved, small;
	char outside[200];
	char path[256];
	char dir[224];

	o.wire = false;

	CASE("an arm after a failed trace counts its work dir as free space");
	env_reset();
	CHECK(failed_trace_make());
	snprintf(dir, sizeof(dir), "%s", work_dir);
	work_path(path, sizeof(path), "capture");
	file_fill(path, 3 * 1024 * 1024);
	calltrace_test_space(30ull * 1024 * 1024);
	CHECK_UINT(arm(1, &o), 0);
	CHECK(state_is("armed"));

	CASE("and that arm removes the dir of the failed trace");
	CHECK(!file_exists(dir));
	calltrace_test_space(0);
	CHECK_UINT(calltrace_cancel(), 0);

	CASE("an arm short by more than the failed dir holds is refused");
	env_reset();
	CHECK(failed_trace_make());
	snprintf(dir, sizeof(dir), "%s", work_dir);
	work_path(path, sizeof(path), "capture");
	file_fill(path, 3 * 1024 * 1024);
	calltrace_test_space(28ull * 1024 * 1024);
	CHECK_UINT(calltrace_arm(1, &o), -ENOSPC);
	status_take();
	CHECK(status_is("last_error", "no_space"));
	CHECK(status_is("state", "failed"));

	CASE("and the dir of the failed trace is still there");
	CHECK(file_exists(dir));
	CHECK(status_is("workdir", dir));
	calltrace_test_space(0);

	CASE("the arm removes a failed dir with the finisher's subdirectories");
	env_reset();
	CHECK(failed_trace_make());
	snprintf(dir, sizeof(dir), "%s", work_dir);
	work_path(path, sizeof(path), ".finish");
	CHECK(!mkdir(path, 0700));
	work_path(path, sizeof(path), ".finish/x");
	CHECK(!mkdir(path, 0700));
	work_path(path, sizeof(path), ".finish/x/out");
	file_fill(path, 64);
	work_path(path, sizeof(path), "system");
	CHECK(!mkdir(path, 0700));
	work_path(path, sizeof(path), "system/y");
	file_fill(path, 64);
	CHECK_UINT(arm(1, &o), 0);
	CHECK(!file_exists(dir));
	CHECK_UINT(calltrace_cancel(), 0);

	/* The walk follows no link, so a link to a directory outside the work
	 * dir costs that directory nothing. */
	CASE("a link in a failed dir goes and the directory it names stays");
	env_reset();
	CHECK(failed_trace_make());
	snprintf(dir, sizeof(dir), "%s", work_dir);
	snprintf(outside, sizeof(outside), "%s/outside", case_root);
	CHECK(!mkdir(outside, 0700));
	snprintf(path, sizeof(path), "%s/kept", outside);
	file_fill(path, 64);
	work_path(path, sizeof(path), "link");
	CHECK(!symlink(outside, path));
	CHECK_UINT(arm(1, &o), 0);
	CHECK(!file_exists(dir));
	snprintf(path, sizeof(path), "%s/kept", outside);
	CHECK(file_exists(path));
	CHECK_UINT(calltrace_cancel(), 0);

	/* Test mode runs no finisher, so the dir of a done trace stays and the
	 * arm finds the leftover a real finisher would have removed. */
	CASE("the arm removes a dir that outlived the finisher of a done "
	     "trace");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK_UINT(calltrace_cancel(), 0);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	CHECK(state_is("done"));
	snprintf(dir, sizeof(dir), "%s", work_dir);
	CHECK(file_exists(dir));
	CHECK_UINT(arm(1, &o), 0);
	CHECK(!file_exists(dir));
	CHECK_UINT(calltrace_cancel(), 0);

	/* The work dir refusal comes after the preflight, so the record of
	 * the failed trace is gone by then, its failure text with it. */
	CASE("an arm refused after the preflight leaves no failure behind");
	env_reset();
	CHECK(failed_trace_make());
	snprintf(dir, sizeof(dir), "%s", work_dir);
	getrlimit(RLIMIT_FSIZE, &saved);
	small = saved;
	small.rlim_cur = 1;
	signal(SIGXFSZ, SIG_IGN);
	setrlimit(RLIMIT_FSIZE, &small);
	CHECK(calltrace_arm(1, &o) != 0);
	setrlimit(RLIMIT_FSIZE, &saved);
	signal(SIGXFSZ, SIG_DFL);
	status_take();
	CHECK(status_is("state", "idle"));
	CHECK(status_is("last_error", "workdir"));
	CHECK(!status_has("failure"));
	CHECK(!file_exists(dir));
}

/* --------------------------------------------------------- the wire capture */

static void wire_cap_tests(void)
{
	struct calltrace_options o = opt_make(30, 5);
	char path[256];

	CASE("a capture that fills its cap while armed is asked to stop");
	env_reset();
	o.wire_max_mb = 1;
	CHECK_UINT(arm(0, &o), 0);
	CHECK(calltrace_test_argv(CALLTRACE_CHILD_WIRE, 0) != NULL);
	work_path(path, sizeof(path), "wire.pcap");
	file_fill(path, 2 * 1024 * 1024);
	pump(200);
	CHECK(calltrace_test_signals(CALLTRACE_CHILD_WIRE)[0] == 'T');
	CHECK_UINT(wire_u32("restarts"), 0);

	/* The capture that is going still owns the file, so the new one starts
	 * at its reap and never beside it. */
	CASE("and the new capture starts at its reap");
	wire_reap(0);
	CHECK_UINT(wire_u32("restarts"), 1);
	CHECK(!file_exists(path));
	CHECK(argv_is(CALLTRACE_CHILD_WIRE, 0, CT_WIRE_SH));
	CHECK_UINT(wire_u8("running"), 1);

	CASE("and the restart is not a truncation");
	CHECK_UINT(wire_u8("truncated"), 0);
	CHECK_UINT(wire_u64("bytes"), 0);

	CASE("the manifest notes the gap the restart left");
	CHECK(manifest_take());
	CHECK(json_array_count("notes") >= 2);

	CASE("a capture that fills its cap while running is cut there");
	env_reset();
	o.wire_max_mb = 1;
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	work_path(path, sizeof(path), "wire.pcap");
	file_fill(path, 2 * 1024 * 1024);
	pump(200);
	CHECK_UINT(wire_u8("truncated"), 1);
	CHECK_UINT(wire_u32("restarts"), 0);

	CASE("the capture reports ready once the file header lands");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	CHECK_UINT(wire_u8("ready"), 0);
	work_path(path, sizeof(path), "wire.pcap");
	file_fill(path, 64);
	pump(400);
	CHECK_UINT(wire_u8("ready"), 1);
	status_take();
	CHECK(status_has("capture_ready_at"));
	CHECK_UINT(calltrace_cancel(), 0);

	CASE("an arm with no wire capture starts no child");
	env_reset();
	o = opt_make(30, 5);
	o.wire = false;
	CHECK_UINT(arm(0, &o), 0);
	CHECK(calltrace_test_argv(CALLTRACE_CHILD_WIRE, 0) == NULL);
	CHECK_UINT(wire_u8("enabled"), 0);
	CHECK_UINT(calltrace_cancel(), 0);
}

/* ------------------------------------------------------- the export failure */

static void export_fail_tests(void)
{
	struct calltrace_options o = opt_make(30, 5);
	struct rlimit saved, small;
	unsigned int i;

	CASE("a ring export that cannot write fails the trace");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);

	for (i = 0; i < 4000; i++)
		TRACE(TRACE_SIP, "a filler line %u for the export to carry", i);

	getrlimit(RLIMIT_FSIZE, &saved);
	small = saved;
	small.rlim_cur = 1;
	signal(SIGXFSZ, SIG_IGN);
	setrlimit(RLIMIT_FSIZE, &small);
	pump(300);
	setrlimit(RLIMIT_FSIZE, &saved);
	signal(SIGXFSZ, SIG_DFL);

	CHECK(state_is("failed"));
	status_take();
	CHECK(status_is("reason", "export_failed"));
	CHECK(status_has("last_error"));
	CHECK(strlen(status_str("last_error")) > 0);

	CASE("and failure carries the file and errno text of last_error");
	CHECK(failure_names("rings.pcapng: "));

	CASE("and it closes the rings behind it");
	CHECK(!trace_ring_is_open());
	CHECK(tap_installed == NULL);
}

/* ---------------------------------------------------------- the snapshots */

static void snapshot_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);

	CASE("the start snapshot carries the version, the region and the "
	     "config");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	calltrace_call_started(0, 7);
	CHECK(snapshot_take(0));
	CHECK(json_is("kind", "start"));
	CHECK(json_has("version"));
	CHECK(json_has("region"));
	CHECK(json_has("config"));
	CHECK(json_has("network"));
	CHECK(!json_has("reasons"));
	CHECK(!json_has("cdr"));

	CASE("every snapshot carries the clock pair and the line's state");
	CHECK(json_has("t_wall_ms"));
	CHECK(json_has("t_mono_ms"));
	CHECK(json_has("clock"));
	CHECK(json_is("fsm_state", "OFFHOOK_DIAL"));
	CHECK(json_has("config_generation"));
	CHECK(json_has("hal"));
	CHECK(json_has("lines"));
	CHECK(json_has("sip"));
	CHECK(json_has("rss_kb"));
	CHECK(json_has("stats"));

	CASE("a periodic snapshot carries neither the config nor the records");
	pump(200);
	CHECK(snapshot_take_kind("periodic", 0));
	CHECK(json_is("kind", "periodic"));
	CHECK(!json_has("config"));
	CHECK(!json_has("region"));
	CHECK(!json_has("version"));
	CHECK(!json_has("reasons"));
	CHECK(!json_has("cdr"));
	CHECK(!json_has("network"));

	CASE("the end snapshot carries the records of this window alone");
	state_set(0, FSM_IDLE);
	pump(1300);
	CHECK(state_is("finishing"));
	CHECK(snapshot_take(-1));
	CHECK(json_is("kind", "end"));
	CHECK(json_has("cdr"));
	/* the leg itself and the leg that took its call over, and not the
	 * unrelated record beside them */
	CHECK_UINT(json_array_count("cdr"), 2);
	CHECK(!json_has("reasons"));
}

static void event_snapshot_tests(void)
{
	struct calltrace_options o = opt_make(30, 5);
	unsigned int before;

	CASE("three events inside one second make one snapshot");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	pump(1200);
	before = snapshot_count_kind("event");
	calltrace_event(0, "rtp_first_rx");
	calltrace_event(0, "rtp_first_tx");
	calltrace_event(0, "tone");
	pump(1200);
	CHECK_UINT(snapshot_count_kind("event"), before + 1);

	CASE("and it keeps every reason it coalesced");
	CHECK(snapshot_take_kind("event", -1));
	CHECK(json_is("kind", "event"));
	CHECK_UINT(json_array_count("reasons"), 3);

	CASE("a state change of the traced line is an event of its own");
	before = snapshot_count_kind("event");
	state_set(0, FSM_CONNECTED);
	pump(1200);
	CHECK_UINT(snapshot_count_kind("event"), before + 1);
	CHECK(snapshot_take_kind("event", -1));
	CHECK_UINT(json_array_count("reasons"), 1);

	/* Every reason word is lower case, so a reader of the list has one
	 * vocabulary; the machine's own names are upper case tokens. */
	CASE("and its reason names the pair the machine moved through");
	CHECK(!strcmp(json_array_first("reasons"),
		      "fsm_offhook_dial_connected"));

	CASE("a config apply asks for one that renders the config again");
	before = snapshot_count_kind("event");
	calltrace_config_applied();
	pump(1200);
	CHECK_UINT(snapshot_count_kind("event"), before + 1);
	CHECK(snapshot_take_kind("event", -1));
	CHECK(json_has("config"));

	/* A registration move is the one reason that renders the network
	 * table, because that table is the question it answers. */
	CASE("a registration move renders the network table beside the line");
	before = snapshot_count_kind("event");
	calltrace_event(0, "registration");
	pump(1200);
	CHECK_UINT(snapshot_count_kind("event"), before + 1);
	CHECK(snapshot_take_kind("event", -1));
	CHECK(!strcmp(json_array_first("reasons"), "registration"));
	CHECK(json_has("network"));

	CASE("and an event of another kind does not");
	before = snapshot_count_kind("event");
	calltrace_event(0, "tone");
	pump(1200);
	CHECK_UINT(snapshot_count_kind("event"), before + 1);
	CHECK(snapshot_take_kind("event", -1));
	CHECK(!json_has("network"));

	CASE("an event on another line asks for nothing");
	before = snapshot_count_kind("event");
	calltrace_event(1, "tone");
	pump(1200);
	CHECK_UINT(snapshot_count_kind("event"), before);
	CHECK_UINT(calltrace_cancel(), 0);
}

/* ----------------------------------------------------------- the shutdown */

static void shutdown_tests(void)
{
	struct calltrace_options o = opt_make(30, 5);

	CASE("a daemon stop while armed marks the dir interrupted");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	calltrace_shutdown();
	CHECK(manifest_is("stage", "interrupted"));
	CHECK(manifest_is("reason", "daemon_stop"));
	CHECK(!trace_ring_is_open());
	CHECK(tap_installed == NULL);
	CHECK(!calltrace_capturing());

	CASE("a daemon stop while running writes its end snapshot first");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	calltrace_shutdown();
	CHECK(manifest_is("stage", "interrupted"));
	CHECK(manifest_is("reason", "daemon_stop"));
	CHECK(snapshot_take(-1));
	CHECK(json_is("kind", "end"));
	CHECK(!trace_ring_is_open());

	CASE("a daemon stop while finishing records the same stage");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK_UINT(calltrace_cancel(), 0);
	CHECK(state_is("finishing"));
	calltrace_shutdown();
	CHECK(manifest_is("stage", "interrupted"));
	CHECK(manifest_is("reason", "daemon_stop"));

	CASE("a daemon stop with no trace changes nothing");
	env_reset();
	calltrace_shutdown();
	CHECK(state_is("idle"));
}

/* ----------------------------------------------------------- the recovery */

static void boot_id_read(char *out, size_t len)
{
	char *text, *nl;

	snprintf(out, len, "unknown");
	text = file_slurp("/proc/sys/kernel/random/boot_id");
	if (!text)
		return;

	nl = strchr(text, '\n');
	if (nl)
		*nl = '\0';
	snprintf(out, len, "%s", text);
	free(text);
}

static bool recover_dir_make(const char *name, const char *json, char *out,
			     size_t len)
{
	char path[256];

	mkdir(tmp_root, 0700);
	mkdir(case_root, 0700);
	snprintf(out, len, "%s/%s", case_root, name);
	if (mkdir(out, 0700) < 0)
		return false;

	snprintf(path, sizeof(path), "%s/manifest.json", out);

	return file_write_text(path, json);
}

static void recovery_tests(void)
{
	char dir[224];
	char path[256];
	char boot[64];
	char json[512];

	CASE("a work dir of a dead daemon is marked interrupted");
	env_reset();
	CHECK(recover_dir_make("20260101-000000-line0-dead",
			       "{\"id\":\"20260101-000000-line0-dead\","
			       "\"line\":0,\"stage\":\"running\","
			       "\"boot_id\":\"not-this-boot\","
			       "\"daemon_pid\":4194300,"
			       "\"counts\":{\"calls\":1}}",
			       dir, sizeof(dir)));
	calltrace_init();
	snprintf(path, sizeof(path), "%s/manifest.json", dir);
	CHECK(json_take(path));
	CHECK(json_is("stage", "interrupted"));
	CHECK(json_is("reason", "recovered"));

	CASE("and the keys it already carried survive the rewrite");
	CHECK(json_has("counts"));
	CHECK(json_is("id", "20260101-000000-line0-dead"));

	CASE("and the finisher runs on that dir");
	CHECK(argv_is(CALLTRACE_CHILD_FINISH, 0, CT_FINISH_SH));
	CHECK(argv_is(CALLTRACE_CHILD_FINISH, 1, dir));

	/* The stage is still running, so the recovery has to move it and keep
	 * the reason the daemon stop already wrote: only that separates a
	 * crash from an orderly stop in the archive. */
	CASE("a dir a daemon stop left keeps that reason");
	env_reset();
	CHECK(recover_dir_make("20260101-000000-line0-stop",
			       "{\"stage\":\"running\","
			       "\"reason\":\"daemon_stop\","
			       "\"boot_id\":\"not-this-boot\","
			       "\"daemon_pid\":4194300}",
			       dir, sizeof(dir)));
	calltrace_init();
	snprintf(path, sizeof(path), "%s/manifest.json", dir);
	CHECK(json_take(path));
	CHECK(json_is("stage", "interrupted"));
	CHECK(json_is("reason", "daemon_stop"));

	CASE("a dir a live daemon of this boot owns is left alone");
	env_reset();
	boot_id_read(boot, sizeof(boot));
	snprintf(json, sizeof(json),
		 "{\"stage\":\"running\",\"boot_id\":\"%s\","
		 "\"daemon_pid\":%u}", boot, (unsigned int)getpid());
	CHECK(recover_dir_make("20260101-000000-line0-live", json, dir,
			       sizeof(dir)));
	calltrace_init();
	CHECK(calltrace_test_argv(CALLTRACE_CHILD_FINISH, 1) == NULL);
	snprintf(path, sizeof(path), "%s/manifest.json", dir);
	CHECK(json_take(path));
	CHECK(json_is("stage", "running"));

	CASE("a finished dir asks for nothing");
	env_reset();
	CHECK(recover_dir_make("20260101-000000-line0-done",
			       "{\"stage\":\"done\"}", dir, sizeof(dir)));
	calltrace_init();
	CHECK(calltrace_test_argv(CALLTRACE_CHILD_FINISH, 1) == NULL);
}

/* ---------------------------------------------------------- the retention */

static void retention_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);
	char older[256];
	char old[256];
	char keep[256];

	CASE("publishing an archive removes every older one");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	snprintf(old, sizeof(old), "%s/20250101-000000-line0-0001.tar.gz",
		 case_root);
	snprintf(older, sizeof(older), "%s/20240101-000000-line0-0002.tar.gz",
		 case_root);
	file_fill(old, 16);
	file_fill(older, 16);
	CHECK(file_exists(old));
	CHECK(file_exists(older));

	status_take();
	snprintf(keep, sizeof(keep), "%s/%s.tar.gz", case_root,
		 status_str("id"));
	file_fill(keep, 16);

	call_run(0);
	pump(1300);
	wire_reap(0);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	CHECK(state_is("done"));
	CHECK(!file_exists(old));
	CHECK(!file_exists(older));

	CASE("and keeps the one it just published");
	CHECK(file_exists(keep));
}

/* -------------------------------------------------- the presence of a reply */

static void presence_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);

	CASE("an idle reply with no trace behind it names the state alone");
	env_reset();
	status_take();
	CHECK(status_is("state", "idle"));
	CHECK(!status_has("id"));
	CHECK(!status_has("line"));
	CHECK(!status_has("options"));
	CHECK(!status_has("wire"));
	CHECK(!status_has("rings"));
	CHECK(!status_has("reason"));
	CHECK(!status_has("last_error"));
	CHECK(!status_has("failure"));
	CHECK(!status_has("finish_budget_ms"));
	capture_write("idle");

	CASE("an armed reply names the id, the options and what it waits for");
	CHECK_UINT(arm(0, &o), 0);
	status_take();
	CHECK(status_is("state", "armed"));
	CHECK(status_has("id"));
	CHECK(status_has("line"));
	CHECK(status_has("options"));
	CHECK(status_has("waiting_for"));
	CHECK(status_has("armed_at"));
	CHECK(status_has("workdir"));
	CHECK(status_has("wire"));
	CHECK(status_has("rings"));
	CHECK(!status_has("elapsed_ms"));
	CHECK(!status_has("calls"));
	CHECK(!status_has("started_at"));
	CHECK(!status_has("archive"));
	CHECK(!status_has("failure"));
	CHECK(!status_has("finish_budget_ms"));
	capture_write("armed");

	CASE("a running reply adds the window's own keys");
	state_set(0, FSM_OFFHOOK_DIAL);
	status_take();
	CHECK(status_is("state", "running"));
	CHECK(!status_has("waiting_for"));
	CHECK(status_has("started_at"));
	CHECK(status_has("elapsed_ms"));
	CHECK(status_has("calls"));
	CHECK(status_has("snapshots"));
	CHECK(!status_has("ended_at"));
	CHECK(!status_has("reason"));
	CHECK(!status_has("failure"));
	CHECK(!status_has("finish_budget_ms"));
	capture_write("running");

	CASE("the signaling ring carries the name of the protocol that runs");
	fake_mgcp = true;
	CHECK(ring_row_has("mgcp"));
	CHECK(!ring_row_has("sip"));
	fake_mgcp = false;

	CASE("under the null table the signaling ring keeps the name sip");
	fake_running = false;
	CHECK(ring_row_has("sip"));
	CHECK(!ring_row_has("none"));
	fake_running = true;

	CASE("a finishing reply names the end and keeps the elapsed time");
	CHECK_UINT(calltrace_cancel(), 0);
	status_take();
	CHECK(status_is("state", "finishing"));
	CHECK(status_has("ended_at"));
	CHECK(status_has("elapsed_ms"));
	CHECK(!status_has("archive"));
	CHECK(!status_has("reason"));
	CHECK(!status_has("failure"));
	/* the capture still runs, so no finisher has started yet */
	CHECK(!status_has("finish_budget_ms"));
	capture_write("finishing");

	CASE("a finishing reply names the budget of the finisher it started");
	wire_reap(0);
	status_take();
	CHECK(status_is("state", "finishing"));
	CHECK(status_has("finish_budget_ms"));
	CHECK_UINT(status_u32("finish_budget_ms"),
		   calltrace_test_finish_budget_ms());
	capture_write("finishing-started");

	CASE("a done reply names the reason and the archive");
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	status_take();
	CHECK(status_is("state", "done"));
	CHECK(status_has("reason"));
	CHECK(status_has("archive"));
	CHECK(!status_has("elapsed_ms"));
	CHECK(!status_has("last_error"));
	CHECK(!status_has("failure"));
	CHECK(!status_has("finish_budget_ms"));
	capture_write("done");

	CASE("a failed reply names the reason, the error and the failure");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK_UINT(calltrace_cancel(), 0);
	wire_reap(0);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 9);
	status_take();
	CHECK(status_is("state", "failed"));
	CHECK(status_has("reason"));
	CHECK(status_has("last_error"));
	CHECK(status_is("failure", "exit 9"));
	CHECK(status_has("workdir"));
	CHECK(!status_has("finish_budget_ms"));
	capture_write("failed");

	CASE("a refused arm leaves its word in an idle reply");
	env_reset();
	CHECK_UINT(calltrace_arm(9, &o), -EINVAL);
	status_take();
	CHECK(status_is("state", "idle"));
	CHECK(status_is("last_error", "bad_line"));
	CHECK(!status_has("id"));
	CHECK(!status_has("failure"));
	CHECK(!status_has("finish_budget_ms"));
	capture_write("idle-refused");

	/* A trace that ended leaves its instants and its counts behind, and an
	 * arm that inherited them would name the window of the call before
	 * this one. */
	CASE("an arm after a trace inherits none of its instants or counts");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK_UINT(calltrace_cancel(), 0);
	wire_reap(0);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	CHECK(state_is("done"));
	status_take();
	CHECK(status_has("started_at"));
	CHECK(status_has("archive"));

	CHECK_UINT(arm(1, &o), 0);
	status_take();
	CHECK(status_is("state", "armed"));
	CHECK(!status_has("started_at"));
	CHECK(!status_has("ended_at"));
	CHECK(!status_has("calls"));
	CHECK(!status_has("snapshots"));
	CHECK(!status_has("archive"));
	CHECK(!status_has("reason"));
	CHECK_UINT(calltrace_cancel(), 0);
}

/* ------------------------------------------------------ the manifest stages */

static void manifest_stage_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);

	CASE("the manifest carries the id, the boot and the privacy words");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	CHECK(manifest_take());
	CHECK(json_has("id"));
	CHECK(json_has("boot_id"));
	CHECK(json_has("daemon_pid"));
	CHECK(json_has("options"));
	CHECK(json_has("counts"));
	CHECK(json_has("times"));
	CHECK(json_has("versions"));
	CHECK(json_has("notes"));
	CHECK(json_has("children"));
	CHECK_UINT(json_array_count("contains"), 3);

	CASE("the key log word joins the list when the option writes one");
	CHECK_UINT(calltrace_cancel(), 0);
	env_reset();
	fake_cfg.tls_keylog_enabled = true;
	CHECK_UINT(arm(0, &o), 0);
	CHECK(manifest_take());
	CHECK_UINT(json_array_count("contains"), 4);

	CASE("the stages follow the window in order");
	CHECK(manifest_is("stage", "armed"));
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK(manifest_is("stage", "running"));
	state_set(0, FSM_IDLE);
	pump(1300);
	CHECK(manifest_is("stage", "finishing"));
	wire_reap(0);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	CHECK(manifest_is("stage", "done"));
}

/* ------------------------------------------------------ the owned children */

/*
 * A child is owned from the spawn to the reap. That is what lets the archive
 * carry the capture's own exit status, and what keeps a stop meant for one
 * process from reaching the one that replaced it.
 */
static unsigned int signal_count(int child, char sig)
{
	const char *s = calltrace_test_signals(child);
	unsigned int n = 0;

	for (; *s; s++) {
		if (*s == sig)
			n++;
	}

	return n;
}

static void child_owner_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);
	char path[256];

	CASE("the freeze asks the capture to stop and waits for its reap");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	call_run(0);
	pump(1300);
	CHECK(state_is("finishing"));
	/* The stop went; whether the kill deadline has passed by the time this
	 * reads depends on the grace, so the case pins the first signal. */
	CHECK(calltrace_test_signals(CALLTRACE_CHILD_WIRE)[0] == 'T');
	CHECK(calltrace_test_argv(CALLTRACE_CHILD_FINISH, 0) == NULL);

	CASE("and the finisher starts only once the capture is reaped");
	wire_reap(0);
	CHECK(argv_is(CALLTRACE_CHILD_FINISH, 0, CT_FINISH_SH));
	CHECK(state_is("finishing"));

	CASE("the capture's exit status stands before the finisher runs");
	CHECK_UINT(wire_u32("exit"), 0);
	CHECK_UINT(wire_u8("running"), 0);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	CHECK(state_is("done"));

	CASE("a capture that ignores the stop is killed after the deadline");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	CHECK_UINT(calltrace_cancel(), 0);
	CHECK(!strcmp(calltrace_test_signals(CALLTRACE_CHILD_WIRE), "T"));
	pump(200);
	CHECK(!strcmp(calltrace_test_signals(CALLTRACE_CHILD_WIRE), "TK"));

	/*
	 * The size poll runs every tick while a capture sits over its cap, so
	 * it asks for the stop again and again. One deadline is set and one
	 * stop goes: a second would push the kill out and leave the daemon
	 * waiting on a process that ignores it.
	 */
	CASE("a capture asked to stop many times is asked once");
	env_reset();
	o.wire_max_mb = 1;
	CHECK_UINT(arm(0, &o), 0);
	work_path(path, sizeof(path), "wire.pcap");
	file_fill(path, 2 * 1024 * 1024);
	pump(400);
	CHECK_UINT(signal_count(CALLTRACE_CHILD_WIRE, 'T'), 1);
	CHECK_UINT(signal_count(CALLTRACE_CHILD_WIRE, 'K'), 1);
	o.wire_max_mb = 8;
	CHECK_UINT(calltrace_cancel(), 0);

	CASE("an arm is refused while the capture of the last trace lives");
	CHECK(state_is("idle"));
	CHECK_UINT(calltrace_arm(0, &o), -EBUSY);
	status_take();
	CHECK(status_is("last_error", "busy"));

	CASE("and it is taken once that capture is reaped");
	wire_reap(-1);
	CHECK_UINT(arm(0, &o), 0);
	CHECK(state_is("armed"));
	CHECK_UINT(calltrace_cancel(), 0);
	wire_reap(0);

	CASE("a trace with no capture reaches the finisher at once");
	env_reset();
	o.wire = false;
	CHECK_UINT(arm(0, &o), 0);
	call_run(0);
	pump(1300);
	CHECK(state_is("finishing"));
	CHECK(argv_is(CALLTRACE_CHILD_FINISH, 0, CT_FINISH_SH));
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	CHECK(state_is("done"));
	o.wire = true;

	/* The budget is a deadline on the finisher, and the escalation of the
	 * child takes it from there; the reap is what decides the trace. */
	CASE("a finisher that outruns its budget is stopped and then killed");
	env_reset();
	/* The few files of this dir round up to one MiB, so a small term keeps
	 * the deadline inside the pumped window. */
	calltrace_test_finish_mib_ms(50);
	o.wire = false;
	CHECK_UINT(arm(0, &o), 0);
	call_run(0);
	pump(1300);
	CHECK(state_is("finishing"));
	pump(400);
	CHECK(!strcmp(calltrace_test_signals(CALLTRACE_CHILD_FINISH), "TK"));
	/* The deadline asks it to go; only the reap decides the trace. */
	CHECK(state_is("finishing"));

	CASE("and the trace fails when that finisher is reaped");
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, -1);
	CHECK(state_is("failed"));
	status_take();
	CHECK(status_is("reason", "finisher_failed"));
	o.wire = true;
}

/* ------------------------------------------------------ the finisher budget */

/*
 * The budget of a finisher run is the base plus one term for each MiB of the
 * dir that it packs, MiB rounded up. The control files of a case's dir stay
 * under one MiB, so a dir with N MiB of capture rounds up to N + 1. A sparse
 * file has the size of a capture and costs no write.
 */
static void finish_budget_tests(void)
{
	unsigned int base = calltrace_test_cadence_ms(CALLTRACE_CADENCE_FINISH);
	struct calltrace_options o = opt_make(30, 1);
	char path[256];
	char dir[224];

	o.wire = false;

	CASE("the shipped term adds two seconds to the budget for each MiB");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	work_path(path, sizeof(path), "capture");
	CHECK(file_sparse(path, 2 * 1024 * 1024));
	CHECK_UINT(calltrace_cancel(), 0);
	CHECK(state_is("finishing"));
	CHECK_UINT(calltrace_test_finish_budget_ms(), base + 3 * 2000);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);

	CASE("a finisher of a large dir is stopped at its deadline and not "
	     "before");
	env_reset();
	calltrace_test_finish_mib_ms(100);
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	work_path(path, sizeof(path), "capture");
	CHECK(file_sparse(path, 4 * 1024 * 1024));
	CHECK_UINT(calltrace_cancel(), 0);
	CHECK_UINT(calltrace_test_finish_budget_ms(), base + 5 * 100);
	/* past the deadline that the base and the control files alone give */
	pump(400);
	CHECK(!strcmp(calltrace_test_signals(CALLTRACE_CHILD_FINISH), ""));
	pump(350);
	CHECK(calltrace_test_signals(CALLTRACE_CHILD_FINISH)[0] == 'T');
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, -1);

	CASE("a file in a subdirectory of the work dir counts in the sum");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	work_path(path, sizeof(path), "system");
	CHECK(!mkdir(path, 0700));
	work_path(path, sizeof(path), "system/logread");
	CHECK(file_sparse(path, 2 * 1024 * 1024));
	work_path(path, sizeof(path), ".finish");
	CHECK(!mkdir(path, 0700));
	work_path(path, sizeof(path), ".finish/logread");
	CHECK(!mkdir(path, 0700));
	work_path(path, sizeof(path), ".finish/logread/out");
	CHECK(file_sparse(path, 2 * 1024 * 1024));
	CHECK_UINT(calltrace_cancel(), 0);
	CHECK_UINT(calltrace_test_finish_budget_ms(), base + 5 * 2000);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);

	CASE("a recovery run takes the budget of its dir the same way");
	env_reset();
	calltrace_test_finish_mib_ms(100);
	CHECK(recover_dir_make("20260101-000000-line0-big",
			       "{\"stage\":\"running\","
			       "\"boot_id\":\"not-this-boot\","
			       "\"daemon_pid\":4194300}", dir, sizeof(dir)));
	snprintf(path, sizeof(path), "%s/wire.pcap", dir);
	CHECK(file_sparse(path, 4 * 1024 * 1024));
	calltrace_init();
	CHECK(argv_is(CALLTRACE_CHILD_FINISH, 1, dir));
	CHECK_UINT(calltrace_test_finish_budget_ms(), base + 5 * 100);
	pump(400);
	CHECK(!strcmp(calltrace_test_signals(CALLTRACE_CHILD_FINISH), ""));
	pump(350);
	CHECK(calltrace_test_signals(CALLTRACE_CHILD_FINISH)[0] == 'T');
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, -1);
}

/* ------------------------------------------------------ the arm-time record */

static void arm_record_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);
	char id[80];

	CASE("a refusal leaves the record of the trace before it untouched");
	env_reset();
	o.wire = false;
	CHECK_UINT(arm(0, &o), 0);
	call_run(0);
	pump(1300);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	CHECK(state_is("done"));
	status_take();
	snprintf(id, sizeof(id), "%s", status_str("id"));
	CHECK_UINT(status_u32("calls"), 1);

	CHECK_UINT(calltrace_arm(9, &o), -EINVAL);
	status_take();
	CHECK(status_is("state", "done"));
	CHECK(status_is("id", id));
	CHECK(status_has("options"));
	CHECK_UINT(status_u32("calls"), 1);
	CHECK(status_is("last_error", "bad_line"));

	CASE("and so does a range refusal");
	o.timeout_s = 1801;
	CHECK_UINT(calltrace_arm(0, &o), -EINVAL);
	status_take();
	CHECK(status_is("id", id));
	CHECK(status_is("last_error", "bad_range"));
	o.timeout_s = 30;

	CASE("an arm timeout past a day is a bad range");
	o.arm_timeout_s = CALLTRACE_ARM_TIMEOUT_MAX_S + 1;
	CHECK_UINT(calltrace_arm(0, &o), -EINVAL);
	status_take();
	CHECK(status_is("last_error", "bad_range"));

	CASE("and a day itself is taken");
	o.arm_timeout_s = CALLTRACE_ARM_TIMEOUT_MAX_S;
	CHECK_UINT(arm(0, &o), 0);
	CHECK(state_is("armed"));
	CHECK_UINT(calltrace_cancel(), 0);
	o.arm_timeout_s = 600;
	o.wire = true;
}

/* -------------------------------------------------- the capture description */

static void wire_description_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);
	const char *filter;

	CASE("the filter names both SIP ports, the media range and the rest");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	filter = calltrace_test_argv(CALLTRACE_CHILD_WIRE, 4);
	CHECK(filter != NULL);
	CHECK(strstr(filter, "port 5060") != NULL);
	CHECK(strstr(filter, "portrange 10000-10100") != NULL);
	CHECK(strstr(filter, "port 53") != NULL);
	CHECK(strstr(filter, "icmp") != NULL);
	CHECK(strstr(filter, "icmp6") != NULL);

	CASE("the manifest records what the capture was pointed at");
	CHECK(manifest_take());
	{
		struct blob_attr *counts = attr_find(jb.head, "counts");
		struct blob_attr *wire = counts ? attr_find(counts, "wire") :
			NULL;
		struct blob_attr *val;

		CHECK(wire != NULL);
		val = wire ? attr_find(wire, "iface") : NULL;
		CHECK(val && !strcmp(blobmsg_get_string(val), "voice0"));
		val = wire ? attr_find(wire, "filter") : NULL;
		CHECK(val && strstr(blobmsg_get_string(val), "portrange"));
		val = wire ? attr_find(wire, "cpumask") : NULL;
		CHECK(val && !strcmp(blobmsg_get_string(val), "all"));
	}
	CHECK_UINT(calltrace_cancel(), 0);
	wire_reap(0);

	CASE("a pinned engine keeps the capture off its CPU");
	env_reset();
	calltrace_test_cpus(4);
	fake_cfg.hal.engine_cpu = 1;
	CHECK_UINT(arm(0, &o), 0);
	CHECK(argv_is(CALLTRACE_CHILD_WIRE, 2, "0,2,3"));
	CHECK_UINT(calltrace_cancel(), 0);
	wire_reap(0);

	CASE("a board with one CPU runs it unpinned");
	env_reset();
	calltrace_test_cpus(1);
	fake_cfg.hal.engine_cpu = 1;
	CHECK_UINT(arm(0, &o), 0);
	CHECK(argv_is(CALLTRACE_CHILD_WIRE, 2, "all"));
	CHECK_UINT(calltrace_cancel(), 0);
	wire_reap(0);
	calltrace_test_cpus(0);
}

/* ------------------------------------------------------ the write failures */

static void write_fail_tests(void)
{
	struct calltrace_options o = opt_make(30, 5);
	struct rlimit saved, small;
	char path[256];
	int err;

	/* The configuration the trace runs on is the first thing the dir has
	 * to take, and a dir that cannot hold it is no dir for an archive. */
	CASE("a work dir that cannot take the config refuses the arm");
	env_reset();
	getrlimit(RLIMIT_FSIZE, &saved);
	small = saved;
	small.rlim_cur = 1;
	signal(SIGXFSZ, SIG_IGN);
	setrlimit(RLIMIT_FSIZE, &small);
	err = calltrace_arm(0, &o);
	setrlimit(RLIMIT_FSIZE, &saved);
	signal(SIGXFSZ, SIG_DFL);

	CHECK(err != 0);
	status_take();
	CHECK(status_is("state", "idle"));
	CHECK(status_is("last_error", "workdir"));
	CHECK(!status_has("id"));
	CHECK(!trace_ring_is_open());

	CASE("a snapshot the work dir cannot take fails the trace");
	env_reset();
	o.wire = false;
	CHECK_UINT(arm(0, &o), 0);
	work_path(path, sizeof(path), "snapshots.jsonl");
	/* a directory of that name takes no append */
	unlink(path);
	CHECK(!mkdir(path, 0700));
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK(state_is("failed"));
	status_take();
	CHECK(status_is("reason", "export_failed"));
	CHECK(strstr(status_str("last_error"), "snapshots.jsonl") != NULL);
	CHECK(failure_names("snapshots.jsonl: "));

	CASE("and the count follows the writes that landed");
	CHECK(!status_has("calls") || status_u32("snapshots") == 0);
	rmdir(path);

	/*
	 * A trace that failed inside the window start arms nothing. A window
	 * timer left behind there would belong to no trace, and the next
	 * trace would be frozen by it at a moment nobody asked for.
	 */
	CASE("a trace that failed at its window start leaves no timer behind");
	env_reset();
	o.wire = false;
	o.timeout_s = 1;
	CHECK_UINT(arm(0, &o), 0);
	work_path(path, sizeof(path), "snapshots.jsonl");
	unlink(path);
	CHECK(!mkdir(path, 0700));
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK(state_is("failed"));
	rmdir(path);

	/* The second line, so the id of this trace differs from the one that
	 * failed in the same second. */
	CASE("so the trace after it keeps its own window");
	o.timeout_s = 30;
	CHECK_UINT(arm(1, &o), 0);
	state_set(1, FSM_OFFHOOK_DIAL);
	CHECK(state_is("running"));
	/* past the first trace's one-second window */
	pump(1400);
	CHECK(state_is("running"));
	CHECK_UINT(calltrace_cancel(), 0);
	o.wire = true;
}

/* -------------------------------------------------------- the export cap */

static void export_cap_tests(void)
{
	struct calltrace_options o = opt_make(30, 5);
	uint64_t at_cap, later;
	unsigned int i;

	/*
	 * The log, the SIP and the status rings are the cheap evidence and
	 * keep exporting past the cap; the media ring is the one that stops.
	 * That ring carries no producer yet, so this case reads the split
	 * through the file still growing and the status ring still landing;
	 * the assertion that the media ring alone is stopped waits for the
	 * unit that fills it.
	 */
	CASE("the export cap stops the media ring and not the cheap ones");
	env_reset();
	o.wire = false;
	o.rings_max_mb = 1;
	CHECK_UINT(arm(0, &o), 0);
	/* The export walks a bounded batch per tick, so the lines come in
	 * batches the export can keep up with and reach the file. */
	for (i = 0; i < 12; i++) {
		unsigned int n;

		for (n = 0; n < 3000; n++)
			TRACE(TRACE_SIP, "a filler line %u for the export", n);
		pump(120);
	}

	status_take();
	{
		struct blob_attr *rings = attr_find(sb.head, "rings");
		struct blob_attr *val = rings ? attr_find(rings, "truncated") :
			NULL;

		CHECK(val && blobmsg_get_u8(val));
		val = rings ? attr_find(rings, "file_bytes") : NULL;
		at_cap = val ? blobmsg_get_u64(val) : 0;
	}
	CHECK(at_cap > 0);

	CASE("and the file goes on growing while they still have entries");
	for (i = 0; i < 4; i++) {
		unsigned int n;

		for (n = 0; n < 3000; n++)
			TRACE(TRACE_SIP, "a line %u written after the cap", n);
		pump(120);
	}
	status_take();
	{
		struct blob_attr *rings = attr_find(sb.head, "rings");
		struct blob_attr *val = rings ? attr_find(rings, "file_bytes") :
			NULL;

		later = val ? blobmsg_get_u64(val) : 0;
	}
	CHECK(later > at_cap);
	CHECK_UINT(calltrace_cancel(), 0);
}

/* ----------------------------------------------------- the window's records */

static void window_record_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);

	/* An inbound call opens its record before the machine moves, so the
	 * arm has to take the sequence or the window loses the call it was
	 * armed for. */
	CASE("a record opened before the first move still joins the window");
	env_reset();
	o.wire = false;
	CHECK_UINT(arm(0, &o), 0);
	calltrace_call_started(0, 7);
	state_set(0, FSM_RINGING);
	CHECK(state_is("running"));

	CASE("and the end snapshot names that record and the leg behind it");
	state_set(0, FSM_IDLE);
	pump(1300);
	CHECK(state_is("finishing"));
	CHECK(snapshot_take_kind("end", -1));
	CHECK_UINT(json_array_count("cdr"), 2);
	{
		struct blob_attr *arr = attr_find(jb.head, "cdr");
		struct blob_attr *cur;
		uint32_t seen = 0;
		size_t rem;

		blobmsg_for_each_attr(cur, arr, rem) {
			struct blob_attr *seq = attr_find(cur, "seq");

			if (seq)
				seen |= 1u << blobmsg_get_u32(seq);
		}
		/* the leg itself and the leg that took its call over, and not
		 * the unrelated record beside them */
		CHECK_UINT(seen, (1u << 7) | (1u << 8));
	}
	o.wire = true;
}

/* --------------------------------------------------------- the reason list */

static void reason_list_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);
	char word[32];
	unsigned int i;

	CASE("a reason queued just before the freeze reaches the end snapshot");
	env_reset();
	o.wire = false;
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	pump(1200);
	state_set(0, FSM_IDLE);
	calltrace_event(0, "rtp_first_rx");
	pump(1300);
	CHECK(state_is("finishing"));
	CHECK(snapshot_take_kind("end", -1));
	CHECK(json_has("reasons"));
	{
		struct blob_attr *arr = attr_find(jb.head, "reasons");
		struct blob_attr *cur;
		bool found = false;
		size_t rem;

		blobmsg_for_each_attr(cur, arr, rem) {
			if (!strcmp(blobmsg_get_string(cur), "rtp_first_rx"))
				found = true;
		}
		CHECK(found);
	}

	CASE("a second that overruns the list keeps 32 and counts the rest");
	env_reset();
	o.wire = false;
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	pump(1200);
	for (i = 0; i < 33; i++) {
		snprintf(word, sizeof(word), "reason_%u", i);
		calltrace_event(0, word);
	}
	pump(1200);
	CHECK(snapshot_take_kind("event", -1));
	CHECK_UINT(json_array_count("reasons"), 32);
	{
		struct blob_attr *val = attr_find(jb.head, "reasons_dropped");

		CHECK(val && blobmsg_get_u32(val) == 1);
	}
	CHECK_UINT(calltrace_cancel(), 0);
	o.wire = true;
}

/* ------------------------------------------------------------ the clock */

static void clock_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);
	unsigned int stepped;

	CASE("a snapshot after a clock correction says the offset moved");
	env_reset();
	o.wire = false;
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK(snapshot_take_kind("start", 0));
	{
		struct blob_attr *clk = attr_find(jb.head, "clock");
		struct blob_attr *val = clk ? attr_find(clk, "step_detected") :
			NULL;

		CHECK(val && !blobmsg_get_u8(val));
	}

	/* The first snapshot after the correction is the one that sees it, so
	 * the case reads that one and not whichever ran last. */
	stepped = snapshot_count_kind("periodic");
	calltrace_test_clock_shift(5000);
	pump(200);
	CHECK(snapshot_take_kind("periodic", (int)stepped));
	{
		struct blob_attr *clk = attr_find(jb.head, "clock");
		struct blob_attr *val = clk ? attr_find(clk, "step_detected") :
			NULL;

		CHECK(val && blobmsg_get_u8(val));
	}

	CASE("and the one after it reads steady again");
	CHECK(snapshot_take_kind("periodic", (int)stepped + 1));
	{
		struct blob_attr *clk = attr_find(jb.head, "clock");
		struct blob_attr *val = clk ? attr_find(clk, "step_detected") :
			NULL;

		CHECK(val && !blobmsg_get_u8(val));
	}
	calltrace_test_clock_shift(-5000);

	CASE("a config apply moves the generation the snapshots carry");
	uv.config_generation = 2;
	calltrace_config_applied();
	pump(1200);
	CHECK(snapshot_take_kind("event", -1));
	{
		struct blob_attr *val = attr_find(jb.head,
						  "config_generation");

		CHECK(val && blobmsg_get_u32(val) == 2);
	}
	CHECK_UINT(calltrace_cancel(), 0);
	o.wire = true;
}

/* ------------------------------------------------- the line leaving service */

static void armed_oos_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);

	CASE("a line that leaves service while armed ends the arm");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OOS);
	CHECK(state_is("idle"));
	status_take();
	CHECK(status_is("reason", "line_restart"));
	CHECK(!trace_ring_is_open());
	CHECK(!file_exists(work_dir));
	CHECK(!strcmp(calltrace_test_signals(CALLTRACE_CHILD_WIRE), "T"));
	wire_reap(0);
}

/* --------------------------------------------- the export that cannot close */

/*
 * The close is the last write of the file and the one that finishes it, so a
 * close that fails has to end the trace exactly as a failed step does. The
 * bound sits above the snapshot file and below the export, so the end
 * snapshot lands and the close alone cannot.
 */
static void export_close_fail_tests(void)
{
	struct calltrace_options o = opt_make(30, 5);
	struct rlimit saved, small;
	unsigned int i;

	CASE("an export that cannot close fails the trace");
	env_reset();
	o.wire = false;
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	for (i = 0; i < 6; i++) {
		unsigned int n;

		for (n = 0; n < 3000; n++)
			TRACE(TRACE_SIP, "a line %u the export carries", n);
		pump(120);
	}

	getrlimit(RLIMIT_FSIZE, &saved);
	small = saved;
	small.rlim_cur = 64 * 1024;
	signal(SIGXFSZ, SIG_IGN);
	setrlimit(RLIMIT_FSIZE, &small);
	CHECK_UINT(calltrace_cancel(), 0);
	setrlimit(RLIMIT_FSIZE, &saved);
	signal(SIGXFSZ, SIG_DFL);

	CHECK(state_is("failed"));
	status_take();
	CHECK(status_is("reason", "export_failed"));
	CHECK(strstr(status_str("last_error"), "rings.pcapng") != NULL);
	CHECK(failure_names("rings.pcapng: "));

	CASE("and the rings are closed behind it");
	CHECK(!trace_ring_is_open());
	o.wire = true;
}

/* -------------------------------------------------- the recovered work dirs */

static void recovery_outcome_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);
	char dir[224];
	char path[256];
	char old[256];

	CASE("a recovered dir its finisher packed reads done");
	env_reset();
	CHECK(recover_dir_make("20260101-000000-line0-ok",
			       "{\"stage\":\"running\","
			       "\"boot_id\":\"not-this-boot\","
			       "\"daemon_pid\":4194300}", dir, sizeof(dir)));
	snprintf(old, sizeof(old), "%s/20240101-000000-line0-old.tar.gz",
		 case_root);
	file_fill(old, 16);
	calltrace_init();
	CHECK(argv_is(CALLTRACE_CHILD_FINISH, 1, dir));

	CASE("an arm is refused while that finisher runs");
	CHECK_UINT(calltrace_arm(0, &o), -EBUSY);
	status_take();
	CHECK(status_is("last_error", "busy"));

	CASE("and its exit moves the stage and sweeps the older archives");
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	snprintf(path, sizeof(path), "%s/manifest.json", dir);
	CHECK(json_take(path));
	CHECK(json_is("stage", "done"));
	CHECK(!file_exists(old));

	CASE("a recovered dir its finisher could not pack reads failed");
	env_reset();
	CHECK(recover_dir_make("20260101-000000-line0-bad",
			       "{\"stage\":\"running\","
			       "\"boot_id\":\"not-this-boot\","
			       "\"daemon_pid\":4194300}", dir, sizeof(dir)));
	calltrace_init();
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 2);
	snprintf(path, sizeof(path), "%s/manifest.json", dir);
	CHECK(json_take(path));
	CHECK(json_is("stage", "failed"));
	CHECK(json_is("reason", "finisher_failed"));

	CASE("and an arm is taken again once it is reaped");
	CHECK_UINT(arm(0, &o), 0);
	CHECK(state_is("armed"));
	CHECK_UINT(calltrace_cancel(), 0);
	wire_reap(0);
}

/* ---------------------------------------- the stop with a child still alive */

static void shutdown_child_tests(void)
{
	struct calltrace_options o = opt_make(30, 5);

	CASE("a stop while a failed trace still owns its capture keeps both");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK_UINT(calltrace_cancel(), 0);
	wire_reap(0);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 4);
	CHECK(state_is("failed"));
	calltrace_shutdown();
	status_take();
	CHECK(status_is("state", "failed"));
	CHECK(status_is("reason", "finisher_failed"));
	CHECK(status_has("last_error"));

	CASE("a stop after an archive keeps the archive and its reason");
	env_reset();
	o.grace_s = 1;
	CHECK_UINT(arm(0, &o), 0);
	call_run(0);
	pump(1300);
	wire_reap(0);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	CHECK(state_is("done"));
	calltrace_shutdown();
	status_take();
	CHECK(status_is("state", "done"));
	CHECK(status_has("archive"));
	CHECK(status_is("reason", "call_ended"));

	/*
	 * The capture of a cancelled arm is still this daemon's, and an arm
	 * is refused while it lives. The stop is where that ownership ends:
	 * procd gives five seconds and a child that ignores the signal must
	 * not spend them, so the stop drops the reap and asks it to go.
	 */
	CASE("an arm is refused while the capture of a cancelled arm lives");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	CHECK_UINT(calltrace_cancel(), 0);
	CHECK(state_is("idle"));
	CHECK(calltrace_test_signals(CALLTRACE_CHILD_WIRE)[0] == 'T');
	CHECK_UINT(calltrace_arm(1, &o), -EBUSY);

	CASE("and the daemon stop is what stops owning it");
	calltrace_shutdown();
	CHECK_UINT(arm(1, &o), 0);
	CHECK(state_is("armed"));
	CHECK_UINT(calltrace_cancel(), 0);
	wire_reap(0);
}

/* ------------------------------------ the failure that owns its children */

static void fail_child_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);
	char path[256];

	/*
	 * The freeze records the stage before it asks the capture to go, and
	 * the journal is the evidence a recovery reads: a dir that cannot
	 * take it ends the trace there, with the capture still owned.
	 */
	CASE("a freeze whose journal fails ends the trace and stops the "
	     "capture");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	state_set(0, FSM_IDLE);
	work_path(path, sizeof(path), "manifest.json");
	unlink(path);
	CHECK(!mkdir(path, 0700));
	pump(1300);
	rmdir(path);

	CHECK(state_is("failed"));
	status_take();
	CHECK(status_is("reason", "export_failed"));
	CHECK(strstr(status_str("last_error"), "manifest.json") != NULL);
	CHECK(failure_names("manifest.json: "));
	CHECK(calltrace_test_signals(CALLTRACE_CHILD_WIRE)[0] == 'T');

	CASE("and the reap that follows starts no finisher and publishes "
	     "nothing");
	CHECK(calltrace_test_argv(CALLTRACE_CHILD_FINISH, 0) == NULL);
	wire_reap(0);
	CHECK(calltrace_test_argv(CALLTRACE_CHILD_FINISH, 0) == NULL);
	CHECK(state_is("failed"));
	status_take();
	CHECK(!status_has("archive"));
	CHECK_UINT(wire_u32("exit"), 0);

	CASE("a capture that will not start leaves the trace running");
	env_reset();
	calltrace_test_spawn_fail(true);
	CHECK_UINT(arm(0, &o), 0);
	calltrace_test_spawn_fail(false);
	CHECK(state_is("armed"));
	CHECK_UINT(wire_u8("enabled"), 0);
	CHECK_UINT(wire_u8("running"), 0);
	status_take();
	CHECK(status_is("last_error", "wire"));

	CASE("and the manifest says the trace holds no packets of its own");
	CHECK(manifest_take());
	{
		struct blob_attr *arr = attr_find(jb.head, "notes");
		struct blob_attr *cur;
		bool said = false;
		size_t rem;

		blobmsg_for_each_attr(cur, arr, rem) {
			if (strstr(blobmsg_get_string(cur), "would not start"))
				said = true;
		}
		CHECK(said);
	}

	CASE("a finisher that will not start fails the trace at once");
	call_run(0);
	calltrace_test_spawn_fail(true);
	pump(1300);
	calltrace_test_spawn_fail(false);
	CHECK(state_is("failed"));
	status_take();
	CHECK(status_is("reason", "finisher_failed"));
}

/* ------------------------------------------- the arm that cannot journal */

static void initial_manifest_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);
	struct rlimit saved, small;
	int err;

	/* The first manifest is written before the arm commits, so a dir that
	 * refuses it is a refusal, not a trace that failed a moment later. */
	CASE("a dir that cannot take the first manifest refuses the arm");
	env_reset();
	o.wire = false;
	CHECK_UINT(arm(0, &o), 0);
	CHECK(state_is("armed"));
	CHECK_UINT(calltrace_cancel(), 0);

	env_reset();
	getrlimit(RLIMIT_FSIZE, &saved);
	small = saved;
	/* above cfg_dump()'s output, below a manifest */
	small.rlim_cur = 200;
	signal(SIGXFSZ, SIG_IGN);
	setrlimit(RLIMIT_FSIZE, &small);
	err = calltrace_arm(0, &o);
	setrlimit(RLIMIT_FSIZE, &saved);
	signal(SIGXFSZ, SIG_DFL);

	CHECK(err != 0);
	status_take();
	CHECK(status_is("state", "idle"));
	CHECK(status_is("last_error", "workdir"));
	CHECK(!status_has("id"));
	CHECK(!status_has("workdir"));
	CHECK(!trace_ring_is_open());
	CHECK(!calltrace_capturing());
	o.wire = true;
}

/* -------------------------------------------- the sequences of one window */

static void sequence_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);
	unsigned int i;

	/*
	 * A line that was off hook at the arm is still on the call the arm
	 * did not follow, and the records that call opens are not this
	 * window's. The rest that opens the wait is where they go.
	 */
	CASE("the records of the call the arm waited out are discarded");
	env_reset();
	o.wire = false;
	line_state[0] = FSM_CONNECTED;
	CHECK_UINT(arm(0, &o), 0);
	calltrace_call_started(0, 7);
	state_set(0, FSM_IDLE);
	CHECK(state_is("armed"));

	CASE("and the window keeps the records of the call it did follow");
	calltrace_call_started(0, 9);
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK(state_is("running"));
	state_set(0, FSM_IDLE);
	pump(1300);
	CHECK(state_is("finishing"));
	CHECK(snapshot_take_kind("end", -1));
	{
		struct blob_attr *arr = attr_find(jb.head, "cdr");
		struct blob_attr *cur;
		uint32_t seen = 0;
		size_t rem;

		blobmsg_for_each_attr(cur, arr, rem) {
			struct blob_attr *seq = attr_find(cur, "seq");

			if (seq)
				seen |= 1u << blobmsg_get_u32(seq);
		}
		CHECK_UINT(seen, 1u << 9);
	}

	/* The record ring keeps the newest of what it holds, and so does the
	 * window's list: a trace that outran it loses the oldest. */
	CASE("a window that outruns its list keeps the newest records");
	env_reset();
	o.wire = false;
	cdr_wide = true;
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	for (i = 1; i <= 65; i++)
		calltrace_call_started(0, i);
	state_set(0, FSM_IDLE);
	pump(1300);
	CHECK(snapshot_take_kind("end", -1));
	{
		struct blob_attr *arr = attr_find(jb.head, "cdr");
		struct blob_attr *cur;
		bool has_one = false;
		bool has_last = false;
		size_t rem;

		blobmsg_for_each_attr(cur, arr, rem) {
			struct blob_attr *seq = attr_find(cur, "seq");
			uint32_t v = seq ? blobmsg_get_u32(seq) : 0;

			if (v == 1)
				has_one = true;
			if (v == 65)
				has_last = true;
		}
		/* 1 left the list when 65 arrived, and 65 is in it */
		CHECK(!has_one);
		CHECK(has_last);
	}
	cdr_wide = false;
	o.wire = true;
}

/* ------------------------------------ the capture that is capped mid window */

static void cap_across_window_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);
	char path[256];

	/*
	 * The cap is reached while the trace only waits, and the call starts
	 * before the capture has gone. The restart still belongs to the
	 * window that is now running, and it is the reap that starts it.
	 */
	CASE("a capture capped while armed restarts after the window opened");
	env_reset();
	o.wire_max_mb = 1;
	CHECK_UINT(arm(0, &o), 0);
	work_path(path, sizeof(path), "wire.pcap");
	file_fill(path, 2 * 1024 * 1024);
	pump(200);
	CHECK(calltrace_test_signals(CALLTRACE_CHILD_WIRE)[0] == 'T');
	CHECK_UINT(wire_u32("restarts"), 0);

	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK(state_is("running"));
	wire_reap(0);
	CHECK_UINT(wire_u32("restarts"), 1);
	CHECK(argv_is(CALLTRACE_CHILD_WIRE, 0, CT_WIRE_SH));
	CHECK_UINT(wire_u8("running"), 1);

	CASE("and the manifest notes the gap it left");
	CHECK(manifest_take());
	{
		struct blob_attr *arr = attr_find(jb.head, "notes");
		struct blob_attr *cur;
		bool said = false;
		size_t rem;

		blobmsg_for_each_attr(cur, arr, rem) {
			if (strstr(blobmsg_get_string(cur), "holds a gap"))
				said = true;
		}
		CHECK(said);
	}
	CHECK_UINT(calltrace_cancel(), 0);
	wire_reap(0);
	o.wire_max_mb = 8;
}

/* ------------------------------------ the journal the finisher is handed */

static void finisher_manifest_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);
	char path[256];

	/*
	 * The finisher packs the dir as it stands when it starts, so the
	 * manifest on disk at that moment has to carry the capture's exit and
	 * name no capture pid: the archive is what a reader gets.
	 */
	CASE("the manifest the finisher packs carries the capture's exit");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	call_run(0);
	pump(1300);
	CHECK(state_is("finishing"));
	wire_reap(3);
	CHECK(argv_is(CALLTRACE_CHILD_FINISH, 0, CT_FINISH_SH));

	CHECK(manifest_take());
	{
		struct blob_attr *counts = attr_find(jb.head, "counts");
		struct blob_attr *wire = counts ? attr_find(counts, "wire") :
			NULL;
		struct blob_attr *val = wire ? attr_find(wire, "exit") : NULL;

		CHECK(val != NULL);
		CHECK_UINT(val ? blobmsg_get_u32(val) : 0, 3);
	}

	/* The journal is written before the spawn, so it names neither the
	 * capture that has gone nor the finisher that has not started. */
	CASE("and names neither the capture nor the finisher");
	{
		struct blob_attr *kids = attr_find(jb.head, "children");

		CHECK(kids != NULL);
		CHECK(!kids || !attr_find(kids, "wire_pid"));
		CHECK(!kids || !attr_find(kids, "finisher_pid"));
	}
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);

	/*
	 * The archive is named only once the journal that records it landed.
	 * A reply carrying an archive and a failure together would say two
	 * things about one trace, and the schema refuses it.
	 */
	CASE("a done whose journal fails publishes no archive");
	env_reset();
	o.wire = false;
	CHECK_UINT(arm(0, &o), 0);
	call_run(0);
	pump(1300);
	CHECK(state_is("finishing"));
	work_path(path, sizeof(path), "manifest.json");
	unlink(path);
	CHECK(!mkdir(path, 0700));
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	rmdir(path);

	CHECK(state_is("failed"));
	status_take();
	CHECK(!status_has("archive"));
	CHECK(status_is("reason", "export_failed"));
	CHECK(failure_names("manifest.json: "));
	o.wire = true;
}

/* ------------------------------- the arm that cannot close what it opened */

static void teardown_export_fail_tests(void)
{
	struct calltrace_options o = opt_make(30, 5);
	struct rlimit saved, small;
	unsigned int i;

	/* An arm that collected nothing still wrote a file, and a cancel that
	 * cannot finish it is a fault the dir is kept for. */
	CASE("a cancel whose export cannot close fails the trace");
	env_reset();
	o.wire = false;
	CHECK_UINT(arm(0, &o), 0);
	for (i = 0; i < 6; i++) {
		unsigned int n;

		for (n = 0; n < 3000; n++)
			TRACE(TRACE_SIP, "a line %u the export carries", n);
		pump(120);
	}

	getrlimit(RLIMIT_FSIZE, &saved);
	small = saved;
	small.rlim_cur = 64 * 1024;
	signal(SIGXFSZ, SIG_IGN);
	setrlimit(RLIMIT_FSIZE, &small);
	CHECK_UINT(calltrace_cancel(), 0);
	setrlimit(RLIMIT_FSIZE, &saved);
	signal(SIGXFSZ, SIG_DFL);

	CHECK(state_is("failed"));
	status_take();
	CHECK(status_is("reason", "export_failed"));
	CHECK(strstr(status_str("last_error"), "rings.pcapng") != NULL);
	CHECK(failure_names("rings.pcapng: "));

	CASE("and the work dir is kept, not removed");
	CHECK(status_has("workdir"));
	CHECK(file_exists(work_dir));
	CHECK(!trace_ring_is_open());
	o.wire = true;
}

/* ------------------------------------------------------ the canceller ring */

static int16_t lec_plane[TRACE_LEC_SAMPLES_WB];

/* One count of one ring row of the status reply, or all ones when the reply
 * carries no such row. */
static uint64_t ring_count(const char *ring, const char *key)
{
	struct blob_attr *rings, *row, *val;

	status_take();
	rings = attr_find(sb.head, "rings");
	row = rings ? attr_find(rings, ring) : NULL;
	val = row ? attr_find(row, key) : NULL;

	return val ? blobmsg_get_u64(val) : ~0ull;
}

static bool rings_truncated(void)
{
	struct blob_attr *rings, *val;

	status_take();
	rings = attr_find(sb.head, "rings");
	val = rings ? attr_find(rings, "truncated") : NULL;

	return val && blobmsg_get_u8(val);
}

static uint64_t rings_file_bytes(void)
{
	struct blob_attr *rings, *val;

	status_take();
	rings = attr_find(sb.head, "rings");
	val = rings ? attr_find(rings, "file_bytes") : NULL;

	return val ? blobmsg_get_u64(val) : 0;
}

/* One key of the options table of a reply or a manifest. */
static struct blob_attr *option_find(struct blob_attr *head, const char *key)
{
	struct blob_attr *opt = attr_find(head, "options");

	return opt ? attr_find(opt, key) : NULL;
}

static bool json_array_has(const char *key, const char *word)
{
	struct blob_attr *a = attr_find(jb.head, key);
	struct blob_attr *cur;
	size_t rem;

	blobmsg_for_each_attr(cur, a, rem) {
		if (!strcmp(blobmsg_get_string(cur), word))
			return true;
	}

	return false;
}

static void lec_run_write(uint64_t tick)
{
	struct trace_lec_hdr hdr;

	lec_hdr_fill(&hdr, TRACE_LEC_RUN, tick);
	trace_ring_lec(&hdr, 0, lec_plane, lec_plane, lec_plane);
}

static void rtp_write(size_t len)
{
	static const uint8_t payload[CALLTRACE_RTP_ENTRY_MAX];

	if (len > sizeof(payload))
		len = sizeof(payload);

	rtp_data_write(payload, len);
}

/*
 * The canceller entries of an exported rings.pcapng, found by the name of
 * their interface. The writer is native endian (pcapng.h), so the reader is
 * too. drain_seen says that the record of the fake final drain is there.
 */
struct lec_scan {
	unsigned int entries;
	uint16_t link_type;
	bool drain_seen;
};

static uint32_t scan_u32(const uint8_t *p)
{
	uint32_t val;

	memcpy(&val, p, sizeof(val));

	return val;
}

static void scan_idb(const uint8_t *body, size_t len, uint32_t index,
		     uint32_t *lec_iface, struct lec_scan *out)
{
	size_t at = 8;

	while (at + 4 <= len) {
		uint16_t code, olen;

		memcpy(&code, &body[at], sizeof(code));
		memcpy(&olen, &body[at + 2], sizeof(olen));
		if (!code || at + 4 + olen > len)
			return;
		if (code == 2 && olen >= 12 &&
		    !memcmp(&body[at + 4], "uvoiced_lec", 12)) {
			*lec_iface = index;
			memcpy(&out->link_type, body, sizeof(out->link_type));
			return;
		}
		at += 4 + ((olen + 3u) & ~3u);
	}
}

static void scan_epb(const uint8_t *body, size_t len, uint32_t lec_iface,
		     struct lec_scan *out)
{
	uint64_t tick;
	uint32_t cap;

	if (len < 20 || scan_u32(body) != lec_iface)
		return;

	out->entries++;
	cap = scan_u32(&body[12]);
	if (cap < 16 || 20 + (size_t)cap > len)
		return;

	memcpy(&tick, &body[20 + 8], sizeof(tick));
	if (tick == SYNC_DRAIN_TICK)
		out->drain_seen = true;
}

/* The whole of a binary file, which the caller frees, or NULL. */
static uint8_t *file_load(const char *path, size_t *len)
{
	uint8_t *buf;
	long size;
	FILE *f;

	f = fopen(path, "r");
	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0) {
		fclose(f);
		return NULL;
	}
	rewind(f);
	*len = (size_t)size;
	buf = malloc(*len ? *len : 1);
	if (buf && fread(buf, 1, *len, f) != *len) {
		free(buf);
		buf = NULL;
	}
	fclose(f);

	return buf;
}

/* The packet of the fake final packet drain keeps its payload whole in the
 * file, so a byte search finds it. */
static bool pkt_drain_seen(const char *path)
{
	size_t len;
	uint8_t *buf = file_load(path, &len);
	bool seen;

	if (!buf)
		return false;

	seen = memmem(buf, len, sync_pkt_mark, sizeof(sync_pkt_mark)) != NULL;
	free(buf);

	return seen;
}

static bool lec_file_scan(const char *path, struct lec_scan *out)
{
	uint32_t lec_iface = UINT32_MAX, ifaces = 0;
	size_t len, pos = 0;
	uint8_t *buf;

	memset(out, 0, sizeof(*out));
	buf = file_load(path, &len);
	if (!buf)
		return false;

	while (pos + 12 <= len) {
		uint32_t type = scan_u32(&buf[pos]);
		uint32_t blen = scan_u32(&buf[pos + 4]);

		if (blen < 12 || (blen & 3u) || pos + blen > len)
			break;
		if (type == 1)
			scan_idb(&buf[pos + 8], blen - 12, ifaces++, &lec_iface,
				 out);
		else if (type == 6)
			scan_epb(&buf[pos + 8], blen - 12, lec_iface, out);
		pos += blen;
	}
	free(buf);

	return pos == len;
}

static void lec_option_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);
	struct trace_lec_hdr hdr;
	struct blob_attr *val;
	struct lec_scan scan;
	char path[256];

	o.wire = false;
	lec_hdr_fill(&hdr, TRACE_LEC_END, 1);

	CASE("an arm with lec opens the canceller ring and dumps its counts");
	env_reset();
	o.lec = true;
	CHECK_UINT(arm(0, &o), 0);
	CHECK_UINT(trace_ring_lec(&hdr, 0, NULL, NULL, NULL), 0);
	CHECK_UINT(ring_count("lec", "written"), 1);

	CASE("the reply carries the lec option and its cap");
	status_take();
	val = option_find(sb.head, "lec");
	CHECK(val && blobmsg_get_u8(val));
	val = option_find(sb.head, "lec_max_mb");
	CHECK(val && blobmsg_get_u32(val) == 8);

	CASE("and the manifest carries them and names lec_audio");
	CHECK(manifest_take());
	val = option_find(jb.head, "lec");
	CHECK(val && blobmsg_get_bool(val));
	val = option_find(jb.head, "lec_max_mb");
	CHECK(val && blobmsg_get_u32(val) == 8);
	CHECK(json_array_has("contains", "lec_audio"));
	CHECK_UINT(json_array_count("contains"), 4);

	/* Every line in a call is captured, so an arm that waits for its
	 * seize must not fill the cap with a call before the traced one. */
	CASE("an armed trace captures no canceller");
	CHECK(calltrace_capturing());
	CHECK(!calltrace_lec_capturing());
	CHECK(!sync_lec_capturing);

	CASE("the window start captures it, and its push carries the bit");
	sync_reset();
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK(state_is("running"));
	CHECK(calltrace_lec_capturing());
	CHECK_UINT(sync_calls, 1);
	CHECK(sync_lec_at[0]);

	/* The backend drains inside the push that lowers the bit, so the push
	 * has to come while the rings still take that drain and before the
	 * export closes. */
	CASE("the freeze lowers it while the rings take the last drain");
	sync_reset();
	CHECK_UINT(calltrace_cancel(), 0);
	CHECK(state_is("finishing"));
	CHECK(!calltrace_lec_capturing());
	CHECK(!sync_lec_at[0]);
	CHECK(sync_open_at[0]);
	CHECK_UINT(sync_drain_rc, 0);

	CASE("and the exported file holds that last drain");
	work_path(path, sizeof(path), "rings.pcapng");
	CHECK(lec_file_scan(path, &scan));
	CHECK_UINT(scan.link_type, 148);
	CHECK_UINT(scan.entries, 2);
	CHECK(scan.drain_seen);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	CHECK(state_is("done"));

	CASE("an arm without lec leaves the ring closed with zero counts");
	env_reset();
	o.lec = false;
	CHECK_UINT(arm(0, &o), 0);
	CHECK_UINT(trace_ring_lec(&hdr, 0, NULL, NULL, NULL), -ENOENT);
	CHECK_UINT(ring_count("lec", "written"), 0);
	CHECK_UINT(ring_count("lec", "exported"), 0);
	status_take();
	val = option_find(sb.head, "lec");
	CHECK(val && !blobmsg_get_u8(val));

	CASE("and names no lec_audio");
	CHECK(manifest_take());
	CHECK(!json_array_has("contains", "lec_audio"));
	CHECK_UINT(json_array_count("contains"), 3);

	CASE("and its window captures no canceller");
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK(state_is("running"));
	CHECK(!calltrace_lec_capturing());
	CHECK_UINT(calltrace_cancel(), 0);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	CHECK(state_is("done"));

	CASE("a lec cap of zero is refused as a bad range");
	env_reset();
	o.lec_max_mb = 0;
	CHECK_UINT(calltrace_arm(0, &o), -EINVAL);
	status_take();
	CHECK(status_is("last_error", "bad_range"));
	o.lec_max_mb = 8;

	/* wire, rings and lec at 8 MiB each and the 16 MiB of headroom */
	CASE("the space check counts the lec cap with the option");
	env_reset();
	o.lec = true;
	calltrace_test_space(39ull * 1024 * 1024);
	CHECK_UINT(calltrace_arm(0, &o), -ENOSPC);
	status_take();
	CHECK(status_is("last_error", "no_space"));

	CASE("and leaves it out without the option");
	o.lec = false;
	CHECK_UINT(arm(0, &o), 0);
	CHECK_UINT(calltrace_cancel(), 0);
	calltrace_test_space(0);
}

/*
 * The export cadence. The environment stretches the base cadence far past
 * the window of the case, so a step inside it comes from the cadence of the
 * canceller ring alone. The shipped values of both stand in cadence_tests.
 */
static void lec_cadence_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);

	o.wire = false;
	setenv("UVOICE_CALLTRACE_EXPORT_MS", "2000", 1);

	CASE("the export takes the lec cadence while the canceller ring is "
	     "open");
	env_reset();
	o.lec = true;
	CHECK_UINT(arm(0, &o), 0);
	CHECK_UINT(trace_ring_status("{\"seq\":1}"), 0);
	pump(300);
	CHECK(ring_count("status", "exported") >= 1u);
	CHECK_UINT(calltrace_cancel(), 0);

	CASE("and the base cadence without it");
	env_reset();
	o.lec = false;
	CHECK_UINT(arm(0, &o), 0);
	CHECK_UINT(trace_ring_status("{\"seq\":1}"), 0);
	pump(300);
	CHECK_UINT(ring_count("status", "exported"), 0);
	CHECK_UINT(calltrace_cancel(), 0);

	setenv("UVOICE_CALLTRACE_EXPORT_MS", "40", 1);
	calltrace_test_mode(true);
}

/*
 * The cap of the canceller ring. The ring takes entries straight through
 * trace_ring_lec(), 1056 bytes of file each, so 1200 of them pass a cap of
 * 1 MiB. The media ring has a cap of 1 MiB too, and the canceller bytes must
 * not count against it.
 */
static void lec_cap_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);
	uint64_t exported, written;
	unsigned int i, n;
	uint64_t tick = 1;

	o.wire = false;

	CASE("the lec cap stops the canceller ring and notes it");
	env_reset();
	o.lec = true;
	o.lec_max_mb = 1;
	o.rings_max_mb = 1;
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK(calltrace_lec_capturing());
	sync_reset();
	for (i = 0; i < 4; i++) {
		for (n = 0; n < 300; n++)
			lec_run_write(tick++);
		pump(120);
	}
	pump(120);
	CHECK(manifest_take());
	CHECK(json_array_has("notes", "the canceller ring stopped at its cap"));

	/* Nothing exports the frames after the cap, so the backend must stop
	 * copying them: the stop pushes the mask down while the window still
	 * runs, and the final drain of that push finds the ring open. */
	CASE("the cap takes the capture bit down while the window runs");
	CHECK(state_is("running"));
	CHECK(!calltrace_lec_capturing());
	CHECK(sync_calls >= 1u);
	CHECK(!sync_lec_capturing);
	CHECK(sync_rings_open);
	CHECK_UINT(sync_drain_rc, 0);

	CASE("and the media ring is not stopped by the canceller bytes");
	CHECK(!rings_truncated());

	CASE("and the media ring goes on exporting");
	exported = ring_count("rtp", "exported");
	for (n = 0; n < 10; n++)
		rtp_write(172);
	pump(120);
	CHECK_UINT(ring_count("rtp", "exported"), exported + 10u);

	CASE("while the canceller ring exports nothing more");
	exported = ring_count("lec", "exported");
	written = ring_count("lec", "written");
	for (n = 0; n < 10; n++)
		lec_run_write(tick++);
	pump(120);
	CHECK_UINT(ring_count("lec", "exported"), exported);
	CHECK_UINT(ring_count("lec", "written"), written + 10u);

	/* the ten entries above and the record of the final drain at the cap */
	CASE("and the freeze counts what the stopped ring kept");
	CHECK_UINT(calltrace_cancel(), 0);
	CHECK(ring_count("lec", "truncated") >= 11u);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	CHECK(state_is("done"));
}

/*
 * The two caps apart, from the other side: media bytes past lec_max_mb leave
 * the canceller ring running. An RTP entry of 2048 bytes of payload takes
 * 2108 bytes of file, so 600 of them pass 1 MiB, while the media cap stands
 * at 8 MiB.
 */
static void lec_cap_media_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);
	uint64_t exported;
	unsigned int i, n;

	o.wire = false;

	CASE("media bytes past the lec cap leave the canceller ring running");
	env_reset();
	o.lec = true;
	o.lec_max_mb = 1;
	o.rings_max_mb = 8;
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	for (i = 0; i < 4; i++) {
		for (n = 0; n < 150; n++)
			rtp_write(CALLTRACE_RTP_ENTRY_MAX);
		pump(120);
	}
	CHECK(rings_file_bytes() > 1024u * 1024u);
	exported = ring_count("lec", "exported");
	for (n = 0; n < 10; n++)
		lec_run_write(n + 1);
	pump(120);
	CHECK_UINT(ring_count("lec", "exported"), exported + 10u);
	CHECK(calltrace_lec_capturing());
	CHECK(manifest_take());
	CHECK(!json_array_has("notes",
			      "the canceller ring stopped at its cap"));
	CHECK(!rings_truncated());
	CHECK_UINT(calltrace_cancel(), 0);
	calltrace_test_child_done(CALLTRACE_CHILD_FINISH, 0);
	CHECK(state_is("done"));
}

/*
 * A journal that fails at the lec cap ends the trace, and the failure closes
 * the export. The step must stop there: an export walk on the closed file
 * would log an export failure that did not happen.
 */
static void lec_cap_fail_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);
	unsigned int i, n;
	uint64_t tick = 1;
	char path[256];

	o.wire = false;

	CASE("a journal that fails at the lec cap fails the trace alone");
	env_reset();
	o.lec = true;
	o.lec_max_mb = 1;
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	work_path(path, sizeof(path), "manifest.json");
	unlink(path);
	CHECK(!mkdir(path, 0700));
	warn_capture_begin();
	for (i = 0; i < 4; i++) {
		for (n = 0; n < 300; n++)
			lec_run_write(tick++);
		pump(120);
	}
	CHECK(warn_capture_count("manifest write failed") >= 1u);
	CHECK_UINT(warn_capture_count("the ring export failed"), 0);
	warn_capture_end();
	rmdir(path);
	CHECK(state_is("failed"));
	status_take();
	CHECK(status_is("reason", "export_failed"));
	CHECK(strstr(status_str("last_error"), "manifest.json") != NULL);
	CHECK(failure_names("manifest.json: "));
}

/*
 * The same failure at the media cap. The cap writes its note to the journal
 * before the step walks the rings, so a journal that fails there closes the
 * export under that walk. 600 RTP entries of 2108 bytes of file pass 1 MiB.
 */
static void media_cap_fail_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);
	unsigned int i, n;
	char path[256];

	o.wire = false;

	CASE("a journal that fails at the media cap fails the trace alone");
	env_reset();
	o.rings_max_mb = 1;
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	work_path(path, sizeof(path), "manifest.json");
	unlink(path);
	CHECK(!mkdir(path, 0700));
	warn_capture_begin();
	for (i = 0; i < 4; i++) {
		for (n = 0; n < 150; n++)
			rtp_write(CALLTRACE_RTP_ENTRY_MAX);
		pump(120);
	}
	CHECK(warn_capture_count("manifest write failed") >= 1u);
	CHECK_UINT(warn_capture_count("the ring export failed"), 0);
	warn_capture_end();
	rmdir(path);
	CHECK(state_is("failed"));
	CHECK(rings_truncated());
	status_take();
	CHECK(status_is("reason", "export_failed"));
	CHECK(strstr(status_str("last_error"), "manifest.json") != NULL);
	CHECK(failure_names("manifest.json: "));
}

/*
 * A failure takes the capture bits down as the freeze does, and the backend
 * drains inside that push. The drains must reach rings that are still open,
 * so the file of the failed trace holds them. An armed trace has only the
 * packet trace up, so its failure drains that one alone.
 */
static void fail_drain_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);
	struct lec_scan scan;
	unsigned int i, n;
	uint64_t tick = 1;
	char path[256];

	o.wire = false;
	o.lec = true;

	CASE("a failure in the window lowers both bits while the rings are "
	     "open");
	env_reset();
	CHECK_UINT(arm(0, &o), 0);
	state_set(0, FSM_OFFHOOK_DIAL);
	CHECK(calltrace_lec_capturing());
	work_path(path, sizeof(path), "snapshots.jsonl");
	unlink(path);
	CHECK(!mkdir(path, 0700));
	sync_reset();
	for (n = 0; n < 10; n++) {
		lec_run_write(tick++);
		rtp_write(172);
	}
	pump(120);
	rmdir(path);
	CHECK(state_is("failed"));
	status_take();
	CHECK(strstr(status_str("last_error"), "snapshots.jsonl") != NULL);
	CHECK(!sync_capturing);
	CHECK(!sync_lec_capturing);
	CHECK(sync_rings_open);
	CHECK_UINT(sync_drain_rc, 0);
	CHECK_UINT(sync_pkt_rc, 0);

	/* the ten entries above and the record of the final drain */
	CASE("and the file of the failed trace holds both last drains");
	work_path(path, sizeof(path), "rings.pcapng");
	CHECK(lec_file_scan(path, &scan));
	CHECK_UINT(scan.entries, 11);
	CHECK(scan.drain_seen);
	CHECK(pkt_drain_seen(path));

	/* The lec cap is the one journal write of an armed trace after the
	 * arm, so a journal that fails there fails the trace from armed. */
	CASE("a failure from armed lowers the packet trace while the rings are "
	     "open");
	env_reset();
	o.lec_max_mb = 1;
	CHECK_UINT(arm(0, &o), 0);
	work_path(path, sizeof(path), "manifest.json");
	unlink(path);
	CHECK(!mkdir(path, 0700));
	sync_reset();
	for (i = 0; i < 4; i++) {
		for (n = 0; n < 300; n++)
			lec_run_write(tick++);
		pump(120);
	}
	rmdir(path);
	CHECK(state_is("failed"));
	status_take();
	CHECK(strstr(status_str("last_error"), "manifest.json") != NULL);
	CHECK(!sync_capturing);
	CHECK(sync_rings_open);
	CHECK_UINT(sync_pkt_rc, 0);
	CHECK_UINT(sync_drain_rc, 1);

	CASE("and the file of the failed trace holds the last packet drain");
	work_path(path, sizeof(path), "rings.pcapng");
	CHECK(pkt_drain_seen(path));
}

#else /* UVOICE_HAVE_UDEBUG */

/* ------------------------------------------------- the build with no rings */

static void no_udebug_tests(void)
{
	struct calltrace_options o = opt_make(30, 1);

	CASE("an arm without udebug support is refused and names the reason");
	env_reset();
	CHECK_UINT(calltrace_arm(0, &o), -ENOSYS);
	status_take();
	CHECK(status_is("state", "idle"));
	CHECK(status_is("last_error", "no_udebug"));
	CHECK(!status_has("id"));
	CHECK(!status_has("workdir"));

	CASE("and the controller is not capturing");
	CHECK(!calltrace_capturing());
	CHECK(!trace_ring_is_open());

	CASE("a cancel still answers that there is nothing to cancel");
	CHECK_UINT(calltrace_cancel(), -ENOENT);

	/* Every notification is a state load and a return while no trace is
	 * armed, which is what a build with no rings leaves the daemon. */
	CASE("a line that moves with no trace armed changes nothing");
	state_set(0, FSM_OFFHOOK_DIAL);
	state_set(0, FSM_IDLE);
	calltrace_call_started(0, 7);
	calltrace_event(0, "tone");
	calltrace_config_applied();
	pump(100);
	CHECK(state_is("idle"));
	CHECK(!calltrace_capturing());
	status_take();
	CHECK_UINT(status_u32("calls"), 0);
	CHECK(!strcmp(status_str("state"), "idle"));

	CASE("the window, the snapshots and the archive");
	tu_skip("this libubox carries no udebug producer");

	CASE("the capture, the export and the freeze");
	tu_skip("this libubox carries no udebug producer");

	CASE("the recovery and the retention sweep");
	tu_skip("this libubox carries no udebug producer");
}

#endif /* UVOICE_HAVE_UDEBUG */

int main(void)
{
	char template[] = "/tmp/uvoice-calltrace-XXXXXX";

	if (!mkdtemp(template)) {
		printf("1..0 # SKIP cannot create a temp root: %s\n",
		       strerror(errno));
		return 0;
	}
	snprintf(tmp_root, sizeof(tmp_root), "%s", template);

	uloop_init();
	trace_ring_init();

	cadence_tests();
#ifndef UVOICE_HAVE_UDEBUG
	no_udebug_tests();

	calltrace_shutdown();
	trace_ring_done();
	blob_buf_free(&sb);
	blob_buf_free(&jb);
	uloop_done();

	return tu_summary("test_calltrace");
#else
	normal_call_tests();
	child_owner_tests();
	finish_budget_tests();
	arm_record_tests();
	wire_description_tests();
	write_fail_tests();
	export_cap_tests();
	window_record_tests();
	reason_list_tests();
	clock_tests();
	armed_oos_tests();
	invite_fail_tests();
	second_call_tests();
	timeout_tests();
	oos_tests();
	offhook_arm_tests();
	cancel_tests();
	wire_iface_tests();
	state_sync_tests();
	arm_expire_tests();
	refusal_tests();
	reclaim_tests();
	wire_cap_tests();
	export_fail_tests();
	snapshot_tests();
	event_snapshot_tests();
	export_close_fail_tests();
	teardown_export_fail_tests();
	fail_child_tests();
	initial_manifest_tests();
	sequence_tests();
	cap_across_window_tests();
	finisher_manifest_tests();
	shutdown_tests();
	shutdown_child_tests();
	recovery_tests();
	recovery_outcome_tests();
	retention_tests();
	presence_tests();
	manifest_stage_tests();
	lec_option_tests();
	lec_cadence_tests();
	lec_cap_tests();
	lec_cap_media_tests();
	lec_cap_fail_tests();
	media_cap_fail_tests();
	fail_drain_tests();

	calltrace_shutdown();
	trace_ring_done();
	blob_buf_free(&sb);
	blob_buf_free(&jb);
	uloop_done();

	return tu_summary("test_calltrace");
#endif
}
