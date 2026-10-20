// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the voice ubus object and its metallic line test decoder.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uvoice.h"
#include <libubox/blobmsg_json.h>
#include "proto/sip/account.h"
#include "cdr.h"
#include "line/line.h"
#include "test_util.h"

/*
 * hal.h declares hal_backend_ops const. The sources under test read this
 * writable table in its place, so that a case can set an op to NULL.
 */
static struct hal_ops test_backend_ops;
#define hal_backend_ops test_backend_ops

#include "../../src/ubus.c"
#include "../../src/status.c"
#undef hal_backend_ops

struct uvoice uv;

/* the backend owns this type; this suite's backend is the table below */
struct hal {
	int marker;
};

static struct hal fake_hal = { .marker = 1 };

/* features is writable so the capability vocabulary case can raise every bit;
 * every other case leaves it clear, which is the shape they always ran on */
static struct hal_cap fake_cap = {
	.backend = "test",
	.version = "0",
	.slic = "none",
	.lines = 2,
	.streams_per_chan = 2,
};

static const struct hal_cap *fake_cap_get(struct hal *h)
{
	return &fake_cap;
}

static void fake_counters_get(struct hal *h, struct hal_counters *out) { }

static enum hal_status fake_jb_stats_get(struct hal *h, unsigned int line,
					 unsigned int stream,
					 struct hal_jb_stats *out)
{
	return HAL_UNSUPPORTED;
}

static bool stream_stats_live;

static enum hal_status fake_stream_stats_get(struct hal *h, unsigned int line,
					     unsigned int stream,
					     struct hal_media_stats *out)
{
	memset(out, 0, sizeof(*out));

	return stream_stats_live ? HAL_OK : HAL_UNSUPPORTED;
}

/*
 * The monitor op of this table. mon_status is the answer it gives and
 * mon_record what it writes; both are driven from the cases. It fills the
 * record whatever it answers. A refusal that still renders a never row then
 * proves the caller dropped the record.
 */
static enum hal_status mon_status;
static struct hal_monitor mon_record;

static enum hal_status fake_monitor_get(struct hal *h, unsigned int line,
					struct hal_monitor *out)
{
	*out = mon_record;

	return mon_status;
}

/*
 * The stream diagnostic op. Each counter of a row has its own value above
 * 32 bits, so a counter in a 32-bit slot or under another key fails. With
 * diag_live clear, the op refuses every row. With diag_live set, it refuses
 * the row of line 1 and stream 0 and answers the other rows.
 */
static bool diag_live;

static const char *const diag_keys[] = {
	"net_drops", "net_stale", "net_trunc", "tx_fail", "punch_sent",
	"srtp_protect_ok", "srtp_protect_fail", "srtp_unprotect_ok",
	"srtp_unprotect_auth_fail", "srtp_unprotect_replay",
};

static uint64_t diag_value(unsigned int line, unsigned int stream,
			   unsigned int key)
{
	return (uint64_t)(key + 1) << 36 | (uint64_t)line << 4 | stream;
}

static enum hal_status fake_stream_diag_get(struct hal *h, unsigned int line,
					    unsigned int stream,
					    struct hal_stream_diag *out)
{
	if (!diag_live || (line == 1 && stream == 0))
		return HAL_UNSUPPORTED;

	out->net_drops = diag_value(line, stream, 0);
	out->net_stale = diag_value(line, stream, 1);
	out->net_trunc = diag_value(line, stream, 2);
	out->tx_fail = diag_value(line, stream, 3);
	out->punch_sent = diag_value(line, stream, 4);
	out->srtp_protect_ok = diag_value(line, stream, 5);
	out->srtp_protect_fail = diag_value(line, stream, 6);
	out->srtp_unprotect_ok = diag_value(line, stream, 7);
	out->srtp_unprotect_auth_fail = diag_value(line, stream, 8);
	out->srtp_unprotect_replay = diag_value(line, stream, 9);

	return HAL_OK;
}

/*
 * The minimal backend shape: both stream getters answer HAL_UNSUPPORTED and
 * the canceller op is left NULL. The status dumps must gate each sweep on its
 * own op pointer, not on uv.hal alone, or this table crashes them. The monitor
 * op is present, because its four mappings are what the cases drive. The NULL
 * op of a backend that measures no line is pinned by the CLI suite, against the
 * stub daemon. The stream diagnostic op is present for the diag cases, and
 * the engine diagnostic op is NULL.
 */
static struct hal_ops test_backend_ops = {
	.name = "test",
	.cap_get = fake_cap_get,
	.counters_get = fake_counters_get,
	.stream_jb_stats_get = fake_jb_stats_get,
	.stream_stats_get = fake_stream_stats_get,
	.line_monitor_get = fake_monitor_get,
	.stream_diag_get = fake_stream_diag_get,
};

static enum line_test_query_status query_status;
static line_test_cb query_cb;
static void *query_priv;
static unsigned int query_calls;
static unsigned int drop_calls;
static uint8_t reply_data[4096];
static struct blob_attr *reply;
static int complete_status;
static unsigned int complete_calls;
static struct ubus_context request_context;

enum line_test_query_status line_test_query(unsigned int line,
					    enum hal_line_test test,
					    line_test_cb cb, void *priv)
{
	query_calls++;
	query_cb = cb;
	query_priv = priv;
	return query_status;
}

void line_test_drop(unsigned int line)
{
	drop_calls++;
}

/*
 * The loop test in its place. The object under test marshals the four
 * arguments and renders the view, so the recorder keeps what the handler
 * passed down and answers what the case asked it to. The defaults live in the
 * handler, which makes this recorder the only thing that can pin them.
 */
static enum line_loop_test_status loop_status;
static struct line_loop_test_view loop_view;
static unsigned int loop_calls;
static bool loop_enable_seen;
static bool loop_self_seen;
static unsigned int loop_ring_s_seen;
static unsigned int loop_delay_ms_seen;

enum line_loop_test_status line_loop_test_set(bool enable, bool want_self,
					      unsigned int ring_s,
					      unsigned int delay_ms,
					      struct line_loop_test_view *out)
{
	loop_calls++;
	loop_enable_seen = enable;
	loop_self_seen = want_self;
	loop_ring_s_seen = ring_s;
	loop_delay_ms_seen = delay_ms;

	if (out)
		*out = loop_view;

	return loop_status;
}

void lines_loop_test_stop(void) { }

/* the stream each entry was handed, and the answer the query gives */
static enum line_stats live_answer = LINE_STATS_REFUSED;
static unsigned int live_query_stream = ~0u;
static unsigned int live_drop_stream = ~0u;

enum line_stats line_stats_live_query(unsigned int line, unsigned int stream,
				      line_stats_cb cb, void *priv)
{
	live_query_stream = stream;

	/* the line/line.c contract: a stream past the table names no stream */
	if (stream >= HAL_STREAM_MAX)
		return LINE_STATS_NO_STREAM;

	return live_answer;
}

void line_stats_live_drop(unsigned int line, unsigned int stream)
{
	live_drop_stream = stream;
}
enum line_inject line_inject_seize(unsigned int line) { return LINE_INJECT_OK; }
enum line_inject line_inject_release(unsigned int line)
{
	return LINE_INJECT_OK;
}
enum line_inject line_inject_dial(unsigned int line, const char *number,
				  unsigned int *digits)
{
	return LINE_INJECT_OK;
}

void lines_status_dump(struct blob_buf *buf) { }
void lines_region_status_dump(struct blob_buf *buf) { }
void accounts_status_dump(struct blob_buf *buf, int want_line) { }
void net_iface_ubus_attach(struct ubus_context *ctx) { }
static unsigned int net_dump_calls;
void net_iface_status_dump(struct blob_buf *buf) { net_dump_calls++; }
static bool account_cmd_ok;
bool account_register_cmd(unsigned int line) { return account_cmd_ok; }
bool account_unregister_cmd(unsigned int line) { return account_cmd_ok; }

/*
 * The two per-line facts the lec row asks its neighbours for. Both are driven
 * from the suite, so the row's own resolution is what the assertions read: the
 * leg walk and the canceller view belong to the protocol and line/line.c and
 * are pinned in their own suites.
 */
static uint8_t fake_line_kind;
static bool fake_line_running = true;
static struct line_lec_view fake_lec_view;

static uint8_t fake_line_fax_kind(unsigned int line, bool *media_running)
{
	if (media_running)
		*media_running = fake_line_running;

	return fake_line_kind;
}

void line_lec_view_get(unsigned int line, struct line_lec_view *out)
{
	*out = fake_lec_view;
}

/*
 * The protocol of this suite: the shape of the SIP table of sip/proto.c for
 * the entries ubus.c and status.c read, so the composed method table and the
 * status reply carry what the daemon carries. The two command rows answer
 * through uvoice_ubus_request_reply(), the host entry of ubus.c, the way the
 * SIP rows do.
 */
static int fake_command(struct ubus_context *ctx,
			struct ubus_request_data *req, struct blob_attr *msg,
			bool (*cmd)(unsigned int line), const char *verb)
{
	uint32_t line;

	if (!line_arg_get(msg, &line))
		return UBUS_STATUS_INVALID_ARGUMENT;
	if (!cmd(line))
		return UBUS_STATUS_NOT_FOUND;

	uvoice_ubus_request_reply(ctx, req, line, verb);

	return 0;
}

static int fake_register(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method,
			 struct blob_attr *msg)
{
	return fake_command(ctx, req, msg, account_register_cmd, "register");
}

static int fake_unregister(struct ubus_context *ctx, struct ubus_object *obj,
			   struct ubus_request_data *req, const char *method,
			   struct blob_attr *msg)
{
	return fake_command(ctx, req, msg, account_unregister_cmd,
			    "unregister");
}

static const struct ubus_method fake_proto_rows[] = {
	UBUS_METHOD("register", fake_register, line_policy),
	UBUS_METHOD("unregister", fake_unregister, line_policy),
};

static const struct ubus_method *fake_ubus_methods(unsigned int *n)
{
	*n = ARRAY_SIZE(fake_proto_rows);

	return fake_proto_rows;
}

/* The two slots run on each other's stream, the shape of a transfer swap, so
 * a verb that passes the slot through as a stream is caught. */
static unsigned int fake_stream_of_slot(unsigned int line, unsigned int slot)
{
	if (slot == 0)
		return 1;
	if (slot == 1)
		return 0;

	return HAL_STREAM_MAX;
}

static void fake_status_dump_media(struct blob_buf *buf)
{
	blobmsg_add_u32(buf, "opus_dtmf_fallbacks", 0);
}

static const struct proto_ops fake_proto = {
	.name = "sip",
	.stream_of_slot = fake_stream_of_slot,
	.line_fax_kind = fake_line_fax_kind,
	.ubus_methods = fake_ubus_methods,
	.status_dump_lines = accounts_status_dump,
	.status_dump_media = fake_status_dump_media,
};

/*
 * The shape of the null table of proto/proto.c for the same entries: an empty
 * lines array, and no ubus rows, media keys, slots or fax kind of its own.
 * The flag selects it, and the signaling table below follows the flag.
 */
static bool fake_null;

static void fake_null_status_dump_lines(struct blob_buf *buf, int want_line)
{
	void *arr = blobmsg_open_array(buf, "lines");

	blobmsg_close_array(buf, arr);
}

static const struct proto_ops fake_null_proto = {
	.name = "none",
	.status_dump_lines = fake_null_status_dump_lines,
};

/* A protocol with a top-level table of its own (ops->status_dump_top). */
static bool fake_top;

static void fake_status_dump_top(struct blob_buf *buf)
{
	void *tbl = blobmsg_open_table(buf, "fake_top");

	blobmsg_add_u32(buf, "endpoints", 2);
	blobmsg_close_table(buf, tbl);
}

static const struct proto_ops fake_top_proto = {
	.name = "sip",
	.status_dump_lines = accounts_status_dump,
	.status_dump_top = fake_status_dump_top,
};

/*
 * The shape of the MGCP table of proto/mgcp/proto.c: endpoint rows in
 * lines[], the state word in the call entry and the "mgcp" table, and no
 * ubus rows.
 */
static bool fake_mgcp;

static void fake_mgcp_status_dump_lines(struct blob_buf *buf, int want_line)
{
	void *arr = blobmsg_open_array(buf, "lines");
	void *tbl = blobmsg_open_table(buf, NULL);
	void *conns;

	blobmsg_add_u32(buf, "line", 0);
	blobmsg_add_string(buf, "endpoint", "aaln/1@gw.test");
	blobmsg_add_string(buf, "service", "in_service");
	blobmsg_add_string(buf, "state", "idle");
	blobmsg_add_string(buf, "notified_entity", "ca@192.0.2.1:2727");
	blobmsg_add_string(buf, "request_id", "0");
	conns = blobmsg_open_array(buf, "connections");
	blobmsg_close_array(buf, conns);
	blobmsg_close_table(buf, tbl);
	blobmsg_close_array(buf, arr);
}

static void fake_mgcp_status_dump_call(struct blob_buf *buf, unsigned int line)
{
	blobmsg_add_string(buf, "state", "idle");
}

static void fake_mgcp_status_dump_top(struct blob_buf *buf)
{
	void *tbl = blobmsg_open_table(buf, "mgcp");

	blobmsg_add_string(buf, "dialect", "rfc3435");
	blobmsg_add_string(buf, "socket", "bound");
	blobmsg_close_table(buf, tbl);
}

static const struct proto_ops fake_mgcp_proto = {
	.name = "mgcp",
	.status_dump_call = fake_mgcp_status_dump_call,
	.status_dump_lines = fake_mgcp_status_dump_lines,
	.status_dump_top = fake_mgcp_status_dump_top,
};

const struct proto_ops *proto_ops_get(void)
{
	if (fake_null)
		return &fake_null_proto;
	if (fake_mgcp)
		return &fake_mgcp_proto;

	return fake_top ? &fake_top_proto : &fake_proto;
}

const struct ubus_method *proto_ubus_methods(unsigned int *n)
{
	*n = 0;
	if (fake_null || fake_mgcp)
		return NULL;

	return fake_ubus_methods(n);
}

/* The three shapes of the signaling table of proto/proto.c
 * proto_status_dump(). */
void proto_status_dump(struct blob_buf *buf)
{
	void *tbl, *arr;

	tbl = blobmsg_open_table(buf, "signaling");
	blobmsg_add_string(buf, "protocol",
			   fake_null || fake_mgcp ? "mgcp" : "sip");
	blobmsg_add_string(buf, "state", fake_null ? "unavailable" : "running");
	if (fake_null)
		blobmsg_add_string(buf, "error",
				   "the image does not carry this protocol");
	arr = blobmsg_open_array(buf, "available");
	blobmsg_add_string(buf, NULL, "sip");
	blobmsg_close_array(buf, arr);
	blobmsg_close_table(buf, tbl);
}

void cdr_dump(struct blob_buf *buf) { }
void cdr_stats_dump(struct blob_buf *buf, int want_line) { }
void cdr_stats_reset(struct blob_buf *buf) { }
void cdr_media_dump(struct blob_buf *buf,
		    const struct hal_media_stats *media) { }
/* The counter vocabulary belongs to test_cdr, which pins its key set and its
 * order. This suite renders the mode word alone, because the jb row's own
 * rules reach it: jb_row_dump() picks the word a never row carries. */
void cdr_jb_dump(struct blob_buf *buf, const struct hal_jb_stats *jb)
{
	blobmsg_add_string(buf, "mode", hal_jb_mode_name(jb->mode));
}
void cfg_dump(const struct cfg *cfg, struct blob_buf *buf) { }
static int reload_answer;
int uvoice_reload(void) { return reload_answer; }
const char *uvoice_hal_state_name(enum uvoice_hal_state state)
{
	return "test";
}
const char *trace_domain_name(enum trace_domain domain) { return "test"; }
bool trace_domain_get(enum trace_domain domain) { return false; }
bool trace_domain_active(enum trace_domain domain) { return false; }
void trace_domain_set(enum trace_domain domain, bool on) { }
void trace_write(enum trace_domain domain, const char *fmt, ...) { }

/*
 * The call trace controller in its place. The object under test marshals the
 * three methods and nothing else, so the recorder keeps what the handler
 * passed down and answers what the case asked it to. The defaults live in
 * calltrace.h and this layer is their only filler, so calltrace_arm_opt is
 * what pins them.
 */
static unsigned int calltrace_arms;
static unsigned int calltrace_arm_line;
static struct calltrace_options calltrace_arm_opt;
static int calltrace_arm_result;
static int calltrace_cancel_result;
static const char *calltrace_state_word = "idle";

int calltrace_arm(unsigned int line, const struct calltrace_options *o)
{
	calltrace_arms++;
	calltrace_arm_line = line;
	calltrace_arm_opt = *o;

	return calltrace_arm_result;
}

int calltrace_cancel(void)
{
	return calltrace_cancel_result;
}

void calltrace_status_dump(struct blob_buf *buf)
{
	blobmsg_add_string(buf, "state", calltrace_state_word);
}
#ifdef UVOICE_HAL_STUB
void stub_ubus_attach(struct ubus_context *ctx) { }
#endif

int ubus_send_reply(struct ubus_context *ctx, struct ubus_request_data *req,
		    struct blob_attr *msg)
{
	size_t len = blob_pad_len(msg);

	CHECK(len <= sizeof(reply_data));
	if (len > sizeof(reply_data))
		return -1;
	memcpy(reply_data, msg, len);
	reply = (struct blob_attr *)reply_data;
	return 0;
}

void ubus_complete_deferred_request(struct ubus_context *ctx,
				    struct ubus_request_data *req, int status)
{
	complete_status = status;
	complete_calls++;
}

static struct blob_attr *field_get(struct blob_attr *parent, const char *name,
				   int type)
{
	struct blobmsg_policy policy = { .name = name, .type = type };
	struct blob_attr *field[1] = {};

	if (!parent)
		return NULL;
	blobmsg_parse(&policy, 1, field, blob_data(parent), blob_len(parent));
	return field[0];
}

static struct blob_attr *nested_field_get(struct blob_attr *parent,
					  const char *name, int type)
{
	struct blobmsg_policy policy = { .name = name, .type = type };
	struct blob_attr *field[1] = {};

	if (!parent)
		return NULL;
	blobmsg_parse_attr(&policy, 1, field, parent);
	return field[0];
}

static unsigned int array_len(struct blob_attr *array)
{
	struct blob_attr *cur;
	size_t rem;
	unsigned int n = 0;

	if (!array)
		return 0;
	blobmsg_for_each_attr(cur, array, rem)
		n++;
	return n;
}

static void u32_put(uint8_t *record, size_t offset, uint32_t value)
{
	record[offset] = (uint8_t)value;
	record[offset + 1] = (uint8_t)(value >> 8);
	record[offset + 2] = (uint8_t)(value >> 16);
	record[offset + 3] = (uint8_t)(value >> 24);
}

static void record_head_set(uint8_t *record, uint8_t test, uint8_t status,
			    uint32_t valid)
{
	record[MLT_VERSION_OFFSET] = MLT_VERSION;
	record[MLT_TEST_OFFSET] = test;
	record[MLT_STATUS_OFFSET] = status;
	u32_put(record, MLT_VALID_OFFSET, valid);
}

static struct blob_attr *record_decode(enum hal_line_test test,
				       const uint8_t *record, size_t len,
				       enum line_test_decode_status *status)
{
	uint8_t code = 0;

	blob_buf_init(&b, 0);
	*status = line_test_record_dump(test, record, len, &code);
	return field_get(b.head, "record", BLOBMSG_TYPE_TABLE);
}

static void state_reset(void)
{
	unsigned int line;

	for (line = 0; line < ARRAY_SIZE(line_test_reqs); line++)
		uloop_timeout_cancel(&line_test_reqs[line].reply.timeout);
	memset(line_test_reqs, 0, sizeof(line_test_reqs));
	query_status = LINE_TEST_QUERY_OK;
	query_cb = NULL;
	query_priv = NULL;
	query_calls = 0;
	drop_calls = 0;
	reply = NULL;
	complete_status = -1;
	complete_calls = 0;
	fake_line_kind = HAL_PASSTHROUGH_OFF;
	stream_stats_live = false;
	diag_live = false;
	/* the answer line/line.c gives for a line no board carries: the daemon
	 * policy in config and nothing pushed yet */
	memset(&fake_lec_view, 0, sizeof(fake_lec_view));
	fake_lec_view.config.enable = true;
	memset(uv.lec_policy, 0, sizeof(uv.lec_policy));
	/* the answer of a backend that measures no line */
	mon_status = HAL_UNSUPPORTED;
	memset(&mon_record, 0, sizeof(mon_record));
	loop_status = LINE_LOOP_TEST_OK;
	loop_calls = 0;
	loop_enable_seen = false;
	loop_self_seen = false;
	loop_ring_s_seen = 0;
	loop_delay_ms_seen = 0;
	memset(&loop_view, 0, sizeof(loop_view));
}

static int request_run(uint32_t line, const char *test,
		       struct ubus_request_data *request)
{
	struct blob_buf args = {};
	int status;

	blob_buf_init(&args, 0);
	blobmsg_add_u32(&args, "line", line);
	blobmsg_add_string(&args, "test", test);
	status = voice_line_test(&request_context, NULL, request, "line_test",
				 args.head);
	blob_buf_free(&args);
	return status;
}

static void test_method_and_refusals(void)
{
	struct ubus_request_data first = {};
	struct ubus_request_data second = {};
	unsigned int i;
	bool found = false;

	voice_methods_compose();
	for (i = 0; i < (unsigned int)voice_object.n_methods; i++) {
		if (!strcmp(voice_methods[i].name, "line_test"))
			found = true;
	}
	CASE("the voice object registers the line test method");
	CHECK(found);

	state_reset();
	CASE("a bad test name is an invalid argument");
	CHECK_UINT(request_run(0, "foreign", &first),
		   UBUS_STATUS_INVALID_ARGUMENT);
	CHECK_UINT(query_calls, 0);

	state_reset();
	CASE("a bad line is an invalid argument");
	CHECK_UINT(request_run(CFG_CHANNEL_MAX, "gr909", &first),
		   UBUS_STATUS_INVALID_ARGUMENT);
	CHECK_UINT(query_calls, 0);

	state_reset();
	query_status = LINE_TEST_QUERY_NO_LINE;
	CASE("a line absent from the configuration is not found");
	CHECK_UINT(request_run(0, "gr909", &first), UBUS_STATUS_NOT_FOUND);

	state_reset();
	query_status = LINE_TEST_QUERY_NOT_UP;
	CASE("a line that is not up has no line test data");
	CHECK_UINT(request_run(0, "gr909", &first), UBUS_STATUS_NO_DATA);

	state_reset();
	query_status = LINE_TEST_QUERY_CALL_ACTIVE;
	CASE("a call on any line blocks a line test");
	CHECK_UINT(request_run(0, "gr909", &first), UBUS_STATUS_NO_DATA);

	state_reset();
	query_status = LINE_TEST_QUERY_BUSY;
	CASE("a busy line or backend has no line test data");
	CHECK_UINT(request_run(0, "gr909", &first), UBUS_STATUS_NO_DATA);

	state_reset();
	query_status = LINE_TEST_QUERY_UNSUPPORTED;
	CASE("a backend without the line test op has no test data");
	CHECK_UINT(request_run(0, "gr909", &first), UBUS_STATUS_NO_DATA);

	state_reset();
	CASE("an accepted line test defers its request");
	CHECK_UINT(request_run(0, "gr909", &first), 0);
	CHECK(first.deferred);
	CHECK(query_cb != NULL);

	CASE("a second request is refused while the first is pending");
	CHECK_UINT(request_run(0, "roh", &second), UBUS_STATUS_NO_DATA);
	CHECK_UINT(query_calls, 1);
	line_test_request_done(query_priv, LINE_TEST_RESULT_FAILED, NULL, 0);

	state_reset();
	query_status = LINE_TEST_QUERY_PENDING;
	CASE("a pending core slot has no line test data");
	CHECK_UINT(request_run(0, "gr909", &first), UBUS_STATUS_NO_DATA);

	state_reset();
	query_status = LINE_TEST_QUERY_REFUSED;
	CASE("a fatal backend refusal reports an unknown error");
	CHECK_UINT(request_run(0, "gr909", &first),
		   UBUS_STATUS_UNKNOWN_ERROR);
}

static void test_deferred_replies(void)
{
	struct ubus_request_data request = {};
	struct blob_attr *field;
	struct blob_attr *record_field;
	uint8_t record[12] = {};

	state_reset();
	record[MLT_TEST_OFFSET] = HAL_LINE_TEST_GR909;
	CASE("the fake slic record completes as malformed version zero");
	CHECK_UINT(request_run(0, "gr909", &request), 0);
	query_cb(query_priv, LINE_TEST_RESULT_COMPLETE, record, sizeof(record));
	field = field_get(reply, "status", BLOBMSG_TYPE_STRING);
	CHECK(field && !strcmp(blobmsg_get_string(field), "malformed"));
	field = field_get(reply, "length", BLOBMSG_TYPE_INT32);
	CHECK(field && blobmsg_get_u32(field) == sizeof(record));
	record_field = field_get(reply, "record", BLOBMSG_TYPE_TABLE);
	field = nested_field_get(record_field, "version", BLOBMSG_TYPE_INT32);
	CHECK(field && blobmsg_get_u32(field) == 0);
	CHECK(nested_field_get(record_field, "vdc_tr_mv",
			       BLOBMSG_TYPE_INT32) == NULL);
	CHECK_UINT(complete_status, 0);

	state_reset();
	memset(record, 0, sizeof(record));
	record_head_set(record, HAL_LINE_TEST_ROH, 0, 0);
	CASE("a version one result reports complete with no invalid fields");
	CHECK_UINT(request_run(0, "roh", &request), 0);
	query_cb(query_priv, LINE_TEST_RESULT_COMPLETE, record, sizeof(record));
	field = field_get(reply, "status", BLOBMSG_TYPE_STRING);
	CHECK(field && !strcmp(blobmsg_get_string(field), "complete"));
	record_field = field_get(reply, "record", BLOBMSG_TYPE_TABLE);
	CHECK(nested_field_get(record_field, "roh_offhook",
			       BLOBMSG_TYPE_BOOL) == NULL);

	state_reset();
	memset(record, 0, sizeof(record));
	record_head_set(record, HAL_LINE_TEST_ROH, 9, 0);
	CASE("a readable transient result reports its failure code");
	CHECK_UINT(request_run(0, "roh", &request), 0);
	query_cb(query_priv, LINE_TEST_RESULT_FAILED, record, MLT_HEAD_SIZE);
	field = field_get(reply, "status", BLOBMSG_TYPE_STRING);
	CHECK(field && !strcmp(blobmsg_get_string(field), "failed"));
	field = field_get(reply, "code", BLOBMSG_TYPE_INT32);
	CHECK(field && blobmsg_get_u32(field) == 9);

	state_reset();
	CASE("a transient completion without a record has no data");
	CHECK_UINT(request_run(0, "roh", &request), 0);
	query_cb(query_priv, LINE_TEST_RESULT_FAILED, NULL, 0);
	CHECK_UINT(complete_status, UBUS_STATUS_NO_DATA);

	state_reset();
	CASE("a fatal completion reports an unknown error");
	CHECK_UINT(request_run(0, "roh", &request), 0);
	query_cb(query_priv, LINE_TEST_RESULT_FATAL, NULL, 0);
	CHECK_UINT(complete_status, UBUS_STATUS_UNKNOWN_ERROR);

	state_reset();
	CASE("an unreadable result reports an unknown error");
	CHECK_UINT(request_run(0, "roh", &request), 0);
	query_cb(query_priv, LINE_TEST_RESULT_UNREADABLE, NULL, 0);
	CHECK_UINT(complete_status, UBUS_STATUS_UNKNOWN_ERROR);

	state_reset();
	CASE("an unsupported completion has no data");
	CHECK_UINT(request_run(0, "roh", &request), 0);
	query_cb(query_priv, LINE_TEST_RESULT_UNSUPPORTED, NULL, 0);
	CHECK_UINT(complete_status, UBUS_STATUS_NO_DATA);

	state_reset();
	CASE("the line test job timeout returns a timeout");
	CHECK_UINT(request_run(0, "loop", &request), 0);
	query_cb(query_priv, LINE_TEST_RESULT_TIMEOUT, NULL, 0);
	CHECK_UINT(drop_calls, 0);
	CHECK_UINT(complete_calls, 1);
	CHECK_UINT(complete_status, UBUS_STATUS_TIMEOUT);

	state_reset();
	CASE("the later request timer remains a safety net");
	CHECK_UINT(request_run(0, "loop", &request), 0);
	line_test_request_expire(&line_test_reqs[0].reply.timeout);
	CHECK_UINT(drop_calls, 1);
	CHECK_UINT(complete_calls, 1);
	CHECK_UINT(complete_status, UBUS_STATUS_TIMEOUT);

	CASE("the request safety timer follows the line test budget");
	CHECK_UINT(LINE_TEST_TIMEOUT_MS, LINE_TEST_JOB_MS + 1000);

	CASE("the line test budget exceeds the plug-in cap and the default");
	CHECK(LINE_TEST_JOB_MS > 90000);
	CHECK(LINE_TEST_JOB_MS > HAL_JOB_TIMEOUT_MS);

	state_reset();
	CASE("a daemon stop closes a pending line test reply");
	CHECK_UINT(request_run(0, "loop", &request), 0);
	line_test_shutdown();
	CHECK_UINT(drop_calls, 1);
	CHECK_UINT(complete_calls, 1);
	CHECK_UINT(complete_status, UBUS_STATUS_NO_DATA);
}

static void test_decoder_records(void)
{
	enum line_test_decode_status status;
	struct blob_attr *field;
	struct blob_attr *decoded;
	char *json;
	uint8_t record[MLT_GR909_SIZE] = {};
	uint32_t valid;

	valid = MLT_VALID_VDC_TR | MLT_VALID_VAC_TG | MLT_VALID_R_TR |
		MLT_VALID_R_RG | MLT_VALID_ROH_OFFHOOK |
		MLT_VALID_ROH_FAULT | MLT_VALID_REN;
	record_head_set(record, HAL_LINE_TEST_GR909, 0, valid);
	u32_put(record, MLT_VDC_TR_OFFSET, (uint32_t)-25);
	u32_put(record, MLT_VAC_TG_OFFSET, 17);
	u32_put(record, MLT_R_TR_OFFSET, MLT_OHMS_OPEN);
	u32_put(record, MLT_R_RG_OFFSET, 1234);
	record[MLT_GR909_ROH_OFFHOOK_OFFSET] = 1;
	record[MLT_GR909_ROH_FAULT_OFFSET] = 0;
	u32_put(record, MLT_REN_OFFSET, 875);
	decoded = record_decode(HAL_LINE_TEST_GR909, record, sizeof(record),
				&status);
	CASE("a gr909 record decodes its valid fields and open resistance");
	CHECK_UINT(status, LINE_TEST_DECODE_COMPLETE);
	field = nested_field_get(decoded, "vdc_tr_mv", BLOBMSG_TYPE_INT32);
	CHECK(field && (int32_t)blobmsg_get_u32(field) == -25);
	json = blobmsg_format_json(decoded, true);
	CHECK(json && strstr(json, "\"vdc_tr_mv\":-25"));
	free(json);
	field = nested_field_get(decoded, "vac_tg_mvrms", BLOBMSG_TYPE_INT32);
	CHECK(field && blobmsg_get_u32(field) == 17);
	field = nested_field_get(decoded, "r_tr_ohm", BLOBMSG_TYPE_STRING);
	CHECK(field && !strcmp(blobmsg_get_string(field), "open"));
	field = nested_field_get(decoded, "ren_milli", BLOBMSG_TYPE_INT32);
	CHECK(field && blobmsg_get_u32(field) == 875);

	memset(record, 0, sizeof(record));
	record_head_set(record, HAL_LINE_TEST_ROH, 0,
			MLT_VALID_ROH_OFFHOOK | MLT_VALID_ROH_FAULT);
	record[MLT_ROH_OFFHOOK_OFFSET] = 1;
	record[MLT_ROH_FAULT_OFFSET] = 1;
	decoded = record_decode(HAL_LINE_TEST_ROH, record, MLT_ROH_SIZE,
				&status);
	CASE("a receiver off-hook record decodes both valid flags");
	CHECK_UINT(status, LINE_TEST_DECODE_COMPLETE);
	field = nested_field_get(decoded, "roh_offhook", BLOBMSG_TYPE_BOOL);
	CHECK(field && blobmsg_get_bool(field));
	field = nested_field_get(decoded, "roh_res_fault", BLOBMSG_TYPE_BOOL);
	CHECK(field && blobmsg_get_bool(field));

	memset(record, 0, sizeof(record));
	record_head_set(record, HAL_LINE_TEST_CAPACITANCE, 0,
			MLT_VALID_C_TR | MLT_VALID_C_TG | MLT_VALID_C_RG);
	u32_put(record, MLT_C_TR_OFFSET, 10);
	u32_put(record, MLT_C_TG_OFFSET, 20);
	u32_put(record, MLT_C_RG_OFFSET, 30);
	decoded = record_decode(HAL_LINE_TEST_CAPACITANCE, record,
				MLT_CAPACITANCE_SIZE, &status);
	CASE("a capacitance record decodes all three valid values");
	CHECK_UINT(status, LINE_TEST_DECODE_COMPLETE);
	field = nested_field_get(decoded, "c_rg_nf", BLOBMSG_TYPE_INT32);
	CHECK(field && blobmsg_get_u32(field) == 30);

	memset(record, 0, sizeof(record));
	record_head_set(record, HAL_LINE_TEST_LOOP, 0,
			MLT_VALID_R_TR | MLT_VALID_R_TG | MLT_VALID_R_RG);
	u32_put(record, MLT_LOOP_R_TR_OFFSET, 100);
	u32_put(record, MLT_LOOP_R_TG_OFFSET, 200);
	u32_put(record, MLT_LOOP_R_RG_OFFSET, 300);
	decoded = record_decode(HAL_LINE_TEST_LOOP, record, MLT_LOOP_SIZE,
				&status);
	CASE("a loop record decodes all three valid resistances");
	CHECK_UINT(status, LINE_TEST_DECODE_COMPLETE);
	field = nested_field_get(decoded, "r_tg_ohm", BLOBMSG_TYPE_INT32);
	CHECK(field && blobmsg_get_u32(field) == 200);

	memset(record, 0xa5, sizeof(record));
	record_head_set(record, HAL_LINE_TEST_GR909, 0, MLT_VALID_VDC_TR);
	u32_put(record, MLT_VDC_TR_OFFSET, 7);
	decoded = record_decode(HAL_LINE_TEST_GR909, record, sizeof(record),
				&status);
	CASE("an unset valid bit keeps its field out of the decoded record");
	CHECK(nested_field_get(decoded, "vdc_tr_mv", BLOBMSG_TYPE_INT32) !=
	      NULL);
	CHECK(nested_field_get(decoded, "vac_tr_mvrms", BLOBMSG_TYPE_INT32) ==
	      NULL);
	CHECK(nested_field_get(decoded, "r_tr_ohm", BLOBMSG_TYPE_INT32) ==
	      NULL);
}

static void test_decoder_failures(void)
{
	enum line_test_decode_status status;
	uint8_t record[MLT_GR909_SIZE] = {};

	record_head_set(record, HAL_LINE_TEST_GR909, 6, 0);
	record_decode(HAL_LINE_TEST_GR909, record, MLT_HEAD_SIZE, &status);
	CASE("a head-only failure is readable without measurement fields");
	CHECK_UINT(status, LINE_TEST_DECODE_FAILED);

	record_head_set(record, HAL_LINE_TEST_GR909, 0, 0);
	record[MLT_VERSION_OFFSET] = 2;
	record_decode(HAL_LINE_TEST_GR909, record, sizeof(record), &status);
	CASE("a record from another version is malformed");
	CHECK_UINT(status, LINE_TEST_DECODE_MALFORMED);

	record_head_set(record, HAL_LINE_TEST_GR909, 0, 0);
	record_decode(HAL_LINE_TEST_GR909, record, MLT_GR909_SIZE - 1, &status);
	CASE("a short successful record is malformed");
	CHECK_UINT(status, LINE_TEST_DECODE_MALFORMED);

	record_head_set(record, HAL_LINE_TEST_LOOP + 1, 1, 0);
	record_decode(HAL_LINE_TEST_GR909, record, MLT_HEAD_SIZE, &status);
	CASE("an unknown test identifier is malformed");
	CHECK_UINT(status, LINE_TEST_DECODE_MALFORMED);
}

/* The row of the composed voice object that carries name, or -1. */
static int method_index(const char *name)
{
	int i;

	for (i = 0; i < voice_object.n_methods; i++) {
		if (!strcmp(voice_methods[i].name, name))
			return i;
	}

	return -1;
}

/* A command verb, called through the row the voice object carries. */
static int method_call(const char *name, struct ubus_request_data *request,
		       struct blob_attr *msg)
{
	int i = method_index(name);

	if (i < 0)
		return -1;

	return voice_methods[i].handler(&request_context, NULL, request, name,
					msg);
}

/*
 * The voice object at connect: the core rows and the rows of the selected
 * protocol, in one order. The two account verbs come from the SIP table and
 * sit where the core list always had them, between stats_live and seize.
 */
static void test_method_table(void)
{
	int live, reg, unreg, seize;

	voice_methods_compose();
	live = method_index("stats_live");
	reg = method_index("register");
	unreg = method_index("unregister");
	seize = method_index("seize");

	CASE("method table: the protocol adds register and unregister");
	CHECK(reg >= 0);
	CHECK(unreg >= 0);

	CASE("method table: the protocol rows sit between stats_live "
	     "and seize");
	CHECK(live >= 0 && reg == live + 1 && unreg == reg + 1 &&
	      seize == unreg + 1);

	CASE("method table: the object and its type count every composed row");
	CHECK_UINT(voice_object.n_methods,
		   ARRAY_SIZE(voice_methods_head) +
		   ARRAY_SIZE(fake_proto_rows) +
		   ARRAY_SIZE(voice_methods_tail));
	CHECK_UINT(voice_object_type.n_methods, voice_object.n_methods);

	CASE("method table: the null table composes no register row");
	fake_null = true;
	voice_methods_compose();
	CHECK(method_index("register") < 0);
	CHECK(method_index("unregister") < 0);
	live = method_index("stats_live");
	seize = method_index("seize");
	CHECK(live >= 0 && seize == live + 1);
	CHECK_UINT(voice_object.n_methods,
		   ARRAY_SIZE(voice_methods_head) +
		   ARRAY_SIZE(voice_methods_tail));
	CHECK_UINT(voice_object_type.n_methods, voice_object.n_methods);

	fake_null = false;
	voice_methods_compose();
}

static int stats_live_run(uint32_t line, uint32_t slot,
			  struct ubus_request_data *request)
{
	struct blob_buf args = {};
	int status;

	blob_buf_init(&args, 0);
	blobmsg_add_u32(&args, "line", line);
	blobmsg_add_u32(&args, "slot", slot);
	status = voice_stats_live(&request_context, NULL, request, "stats_live",
				  args.head);
	blob_buf_free(&args);

	return status;
}

/*
 * stats_live names a UI slot, and the protocol names the stream that slot
 * runs on (ops->stream_of_slot). The fake protocol swaps the two slots, so
 * the query and its drop must carry the swapped stream.
 */
static void test_stats_live_stream(void)
{
	struct ubus_request_data request = {};

	state_reset();
	live_answer = LINE_STATS_OK;

	CASE("stats_live: the query runs on the stream the protocol names");
	live_query_stream = ~0u;
	CHECK_UINT(stats_live_run(0, 0, &request), 0);
	CHECK_UINT(live_query_stream, 1);

	CASE("stats_live: and its drop names that stream too");
	live_drop_stream = ~0u;
	stats_live_expire(&stats_live_reqs[0][0].reply.timeout);
	CHECK_UINT(live_drop_stream, 1);

	CASE("stats_live: the other slot runs on the other stream");
	live_query_stream = ~0u;
	CHECK_UINT(stats_live_run(0, 1, &request), 0);
	CHECK_UINT(live_query_stream, 0);
	live_drop_stream = ~0u;
	stats_live_shutdown();
	CHECK_UINT(live_drop_stream, 0);

	CASE("stats_live: a slot that maps to no stream answers not found");
	live_query_stream = ~0u;
	CHECK_UINT(stats_live_run(0, 2, &request), UBUS_STATUS_NOT_FOUND);
	CHECK_UINT(live_query_stream, HAL_STREAM_MAX);
	CHECK(!stats_live_reqs[0][2].reply.pending);

	live_answer = LINE_STATS_REFUSED;
}

/*
 * The reload verb maps the answer of uvoice_reload(). A config that moves the
 * protocol is refused with a status of its own, which the init script reads
 * as "restart the daemon"; a load that failed keeps its old answer.
 */
static void test_reload_verb(void)
{
	static struct cfg running;
	struct ubus_request_data request = {};

	state_reset();
	uv.cfg = &running;
	CASE("reload: an applied config answers 0");
	reload_answer = 0;
	CHECK_UINT(method_call("reload", &request, NULL), 0);

	CASE("reload: a protocol move answers not supported");
	reload_answer = UVOICE_RELOAD_RESTART;
	CHECK_UINT(method_call("reload", &request, NULL),
		   UBUS_STATUS_NOT_SUPPORTED);

	CASE("reload: a config that does not load answers an unknown error");
	reload_answer = -1;
	CHECK_UINT(method_call("reload", &request, NULL),
		   UBUS_STATUS_UNKNOWN_ERROR);
	reload_answer = 0;
	uv.cfg = NULL;
}

static void test_account_verbs(void)
{
	struct ubus_request_data request = {};
	struct blob_attr *field;
	struct blob_buf args = {};

	state_reset();
	voice_methods_compose();
	account_cmd_ok = true;
	blob_buf_init(&args, 0);
	blobmsg_add_u32(&args, "line", 1);

	CASE("a register answers the line and the request");
	CHECK_UINT(method_call("register", &request, args.head), 0);
	field = field_get(reply, "line", BLOBMSG_TYPE_INT32);
	CHECK(field && blobmsg_get_u32(field) == 1);
	field = field_get(reply, "requested", BLOBMSG_TYPE_STRING);
	CHECK(field && !strcmp(blobmsg_get_string(field), "register"));

	reply = NULL;
	CASE("an unregister answers the line and the request");
	CHECK_UINT(method_call("unregister", &request, args.head), 0);
	field = field_get(reply, "line", BLOBMSG_TYPE_INT32);
	CHECK(field && blobmsg_get_u32(field) == 1);
	field = field_get(reply, "requested", BLOBMSG_TYPE_STRING);
	CHECK(field && !strcmp(blobmsg_get_string(field), "unregister"));

	reply = NULL;
	CASE("a seize injects the event and answers the line and what it injected");
	CHECK_UINT(voice_seize(&request_context, NULL, &request, "seize",
			       args.head), 0);
	field = field_get(reply, "line", BLOBMSG_TYPE_INT32);
	CHECK(field && blobmsg_get_u32(field) == 1);
	field = field_get(reply, "injected", BLOBMSG_TYPE_STRING);
	CHECK(field && !strcmp(blobmsg_get_string(field), "seize"));

	account_cmd_ok = false;
	reply = NULL;
	CASE("a refused account command sends no body");
	CHECK_UINT(method_call("register", &request, args.head),
		   UBUS_STATUS_NOT_FOUND);
	CHECK(reply == NULL);

	blob_buf_free(&args);
}

static struct blob_attr *first_attr(struct blob_attr *array)
{
	struct blob_attr *cur;
	size_t rem;

	if (!array)
		return NULL;
	blobmsg_for_each_attr(cur, array, rem)
		return cur;
	return NULL;
}

/* is name one of the strings this array carries */
static bool array_has_string(struct blob_attr *array, const char *name)
{
	struct blob_attr *cur;
	size_t rem;

	if (!array)
		return false;

	blobmsg_for_each_attr(cur, array, rem) {
		if (blobmsg_type(cur) == BLOBMSG_TYPE_STRING &&
		    !strcmp(blobmsg_get_string(cur), name))
			return true;
	}

	return false;
}

/*
 * One loop test call, built the way a ubus client builds it. Every key but
 * enable is optional, so a case passes LOOP_ARG_NONE for a number it wants
 * left out and NULL for the word; every other value travels, a negative one
 * included, because the policy takes a signed integer.
 */
#define LOOP_ARG_NONE	INT32_MIN

static int loop_run(const int *enable, const char *mode, int32_t ring_s,
		    int32_t delay_ms)
{
	struct ubus_request_data request = {};
	struct blob_buf args = {};
	int status;

	blob_buf_init(&args, 0);
	if (enable)
		blobmsg_add_u8(&args, "enable", (uint8_t)*enable);
	if (mode)
		blobmsg_add_string(&args, "mode", mode);
	if (ring_s != LOOP_ARG_NONE)
		blobmsg_add_u32(&args, "ring_s", (uint32_t)ring_s);
	if (delay_ms != LOOP_ARG_NONE)
		blobmsg_add_u32(&args, "delay_ms", (uint32_t)delay_ms);
	status = voice_loop_test(&request_context, NULL, &request, "loop_test",
				 args.head);
	blob_buf_free(&args);

	return status;
}

/* the common call: enable alone, so the handler fills both defaults */
static int loop_run_plain(const int *enable)
{
	return loop_run(enable, NULL, LOOP_ARG_NONE, LOOP_ARG_NONE);
}

static const char *reply_string(const char *name)
{
	struct blob_attr *field = field_get(reply, name, BLOBMSG_TYPE_STRING);

	return field ? blobmsg_get_string(field) : "";
}

static uint32_t reply_u32(const char *name)
{
	struct blob_attr *field = field_get(reply, name, BLOBMSG_TYPE_INT32);

	return field ? blobmsg_get_u32(field) : 0;
}

static void test_loop_test_verb(void)
{
	const int on = 1;
	const int off = 0;
	struct blob_attr *field;
	unsigned int i;
	bool found = false;

	voice_methods_compose();
	for (i = 0; i < (unsigned int)voice_object.n_methods; i++) {
		if (!strcmp(voice_methods[i].name, "loop_test"))
			found = true;
	}
	CASE("the voice object registers the loop test method");
	CHECK(found);

	state_reset();
	CASE("a call with no enable key is an invalid argument");
	CHECK_UINT(loop_run_plain(NULL), UBUS_STATUS_INVALID_ARGUMENT);
	CHECK_UINT(loop_calls, 0);

	state_reset();
	CASE("a mode word the surface does not carry is an invalid argument");
	CHECK_UINT(loop_run(&on, "loopback", LOOP_ARG_NONE, LOOP_ARG_NONE),
		   UBUS_STATUS_INVALID_ARGUMENT);
	CHECK_UINT(loop_calls, 0);

	state_reset();
	CASE("each bound of the ring window and the delay is refused");
	CHECK_UINT(loop_run(&on, NULL, 0, LOOP_ARG_NONE),
		   UBUS_STATUS_INVALID_ARGUMENT);
	CHECK_UINT(loop_run(&on, NULL, 61, LOOP_ARG_NONE),
		   UBUS_STATUS_INVALID_ARGUMENT);
	CHECK_UINT(loop_run(&on, NULL, LOOP_ARG_NONE, 1001),
		   UBUS_STATUS_INVALID_ARGUMENT);
	/* a negative INT32 reads as a large unsigned value, so the same test
	 * covers it */
	CHECK_UINT(loop_run(&on, NULL, LOOP_ARG_NONE, -1),
		   UBUS_STATUS_INVALID_ARGUMENT);
	CHECK_UINT(loop_calls, 0);

	state_reset();
	CASE("the absent keys reach the shell as 60 seconds and 500 ms");
	CHECK_UINT(loop_run_plain(&on), 0);
	CHECK_UINT(loop_calls, 1);
	CHECK(loop_enable_seen);
	CHECK(!loop_self_seen);
	CHECK_UINT(loop_ring_s_seen, 60);
	CHECK_UINT(loop_delay_ms_seen, 500);

	state_reset();
	CASE("the four keys the caller sent reach the shell as they are");
	CHECK_UINT(loop_run(&on, "self", 5, 0), 0);
	CHECK(loop_self_seen);
	CHECK_UINT(loop_ring_s_seen, 5);
	CHECK_UINT(loop_delay_ms_seen, 0);
	CHECK_UINT(loop_run(&on, "cross", 60, 1000), 0);
	CHECK(!loop_self_seen);
	CHECK_UINT(loop_ring_s_seen, 60);
	CHECK_UINT(loop_delay_ms_seen, 1000);

	state_reset();
	loop_view.enabled = true;
	loop_view.mode = LINE_LOOP_TEST_MODE_CROSS;
	loop_view.lines = 2;
	loop_view.ring_s = 60;
	loop_view.delay_ms = 500;
	CASE("the reply carries the five keys of the standing mode");
	CHECK_UINT(loop_run_plain(&on), 0);
	field = field_get(reply, "enabled", BLOBMSG_TYPE_BOOL);
	CHECK(field && blobmsg_get_bool(field));
	CHECK(!strcmp(reply_string("mode"), "cross"));
	CHECK_UINT(reply_u32("lines"), 2);
	CHECK_UINT(reply_u32("ring_s"), 60);
	CHECK_UINT(reply_u32("delay_ms"), 500);

	state_reset();
	CASE("a disarmed board answers the off word and three zeros");
	CHECK_UINT(loop_run_plain(&off), 0);
	CHECK(!loop_enable_seen);
	field = field_get(reply, "enabled", BLOBMSG_TYPE_BOOL);
	CHECK(field && !blobmsg_get_bool(field));
	CHECK(!strcmp(reply_string("mode"), "off"));
	CHECK_UINT(reply_u32("lines"), 0);
	CHECK_UINT(reply_u32("ring_s"), 0);
	CHECK_UINT(reply_u32("delay_ms"), 0);

	state_reset();
	loop_view.enabled = true;
	loop_view.mode = LINE_LOOP_TEST_MODE_SELF;
	CASE("the self mode reaches the reply as its own word");
	CHECK_UINT(loop_run(&on, "self", LOOP_ARG_NONE, LOOP_ARG_NONE), 0);
	CHECK(!strcmp(reply_string("mode"), "self"));

	state_reset();
	loop_status = LINE_LOOP_TEST_BAD_ARGUMENT;
	CASE("a shell refusal of the arguments is an invalid argument");
	CHECK_UINT(loop_run_plain(&on), UBUS_STATUS_INVALID_ARGUMENT);

	state_reset();
	loop_status = LINE_LOOP_TEST_NO_LINE;
	CASE("a board with no port is not found");
	CHECK_UINT(loop_run_plain(&on), UBUS_STATUS_NOT_FOUND);

	state_reset();
	loop_status = LINE_LOOP_TEST_NOT_READY;
	CASE("a dsp that is not ready has no loop test data");
	CHECK_UINT(loop_run_plain(&on), UBUS_STATUS_NO_DATA);

	state_reset();
	loop_status = LINE_LOOP_TEST_UNSUPPORTED;
	CASE("a backend without the loop op has no loop test data");
	CHECK_UINT(loop_run_plain(&on), UBUS_STATUS_NO_DATA);

	state_reset();
	loop_status = LINE_LOOP_TEST_CALL_ACTIVE;
	CASE("a call on any line blocks the loop test");
	CHECK_UINT(loop_run_plain(&on), UBUS_STATUS_NO_DATA);

	state_reset();
	loop_status = LINE_LOOP_TEST_TEST_RUNNING;
	CASE("a metallic line test blocks the loop test");
	CHECK_UINT(loop_run_plain(&on), UBUS_STATUS_NO_DATA);

	state_reset();
	loop_status = LINE_LOOP_TEST_REFUSED;
	CASE("a shell refusal of its own is an unknown error");
	CHECK_UINT(loop_run_plain(&on), UBUS_STATUS_UNKNOWN_ERROR);

	state_reset();
}

/*
 * The capability vocabulary of voice status. A feature bit that an operator
 * cannot see is, for that operator, a capability that does not exist. The
 * Annex B bit decides whether a leg advertises annexb=yes and enables its
 * transmit DTX, and no other field of the reply reports it.
 *
 * The case raises every bit and reads the whole list back, so a name that
 * feature_names[] drops or spells differently fails here. The case does not
 * catch a bit that enum hal_feature gains with no name beside it. The array
 * is then one entry shorter than the enum, and this suite has no count of the
 * enum to compare with.
 */
static void test_hal_feature_names(void)
{
	struct ubus_request_data request = {};
	struct blob_attr *field;
	struct cfg cfg = {};

	state_reset();
	uv.cfg = &cfg;
	uv.hal = &fake_hal;
	fake_cap.features = ~0u;

	CASE("voice status names every capability bit the backend raised");
	CHECK_UINT(voice_status(&request_context, NULL, &request, "status",
				NULL), 0);
	field = nested_field_get(field_get(reply, "hal", BLOBMSG_TYPE_TABLE),
				 "features", BLOBMSG_TYPE_ARRAY);
	CHECK(field != NULL);
	CHECK_UINT(array_len(field), 19);
	CHECK(array_has_string(field, "g729"));
	CHECK(array_has_string(field, "g726"));
	CHECK(array_has_string(field, "t38"));
	CHECK(array_has_string(field, "vbd"));
	CHECK(array_has_string(field, "line_test"));
	CHECK(array_has_string(field, "polarity"));
	CHECK(array_has_string(field, "mwi_lamp"));
	CHECK(array_has_string(field, "metering"));
	CHECK(array_has_string(field, "kernel_rtp"));
	CHECK(array_has_string(field, "conf"));
	CHECK(array_has_string(field, "srtp"));
	CHECK(array_has_string(field, "wideband"));

	/* the one this unit added, and the reason the case exists */
	CASE("voice status names the Annex B capability");
	CHECK(array_has_string(field, "g729b"));

	/* the two bits are separate capabilities. A backend can carry IPv6
	 * media and still send RTCP to the media address alone. No other field
	 * of the reply reports either one. */
	CASE("voice status names the IPv6 media and the rtcp address "
	     "capabilities");
	CHECK(array_has_string(field, "ipv6_media"));
	CHECK(array_has_string(field, "rtcp_addr"));

	/* the bit that tells an operator the backend programs its own hook
	 * windows, so a missing push is not a fault */
	CASE("voice status names the region hook capability");
	CHECK(array_has_string(field, "region_hook"));

	/* the two digit bits decide who puts a digit on the wire and who
	 * plays one from it; an operator reading a missing or a doubled digit
	 * needs to know which side owns it */
	CASE("voice status names the two digit capabilities");
	CHECK(array_has_string(field, "dtmf_relay"));
	CHECK(array_has_string(field, "dtmf_playout"));

	/* the bit the loop test reads before it arms: a board whose backend
	 * carries no loop refuses the mode, and this word is where an operator
	 * sees that in front of the refusal */
	CASE("voice status names the loop capability");
	CHECK(array_has_string(field, "loop"));

	/* it is a bit of its own: a board with Annex A alone names g729 and
	 * not g729b, which is what makes the word worth reading */
	CASE("and it leaves with its own bit, not with the G.729 one");
	fake_cap.features = HAL_FEAT_G729;
	CHECK_UINT(voice_status(&request_context, NULL, &request, "status",
				NULL), 0);
	field = nested_field_get(field_get(reply, "hal", BLOBMSG_TYPE_TABLE),
				 "features", BLOBMSG_TYPE_ARRAY);
	CHECK(field != NULL);
	CHECK_UINT(array_len(field), 1);
	CHECK(array_has_string(field, "g729"));
	CHECK(!array_has_string(field, "g729b"));

	fake_cap.features = 0;
}

/*
 * The status reply carries the configuration generation at the top level, so
 * a client can tell that a reload took without a call trace.
 */
static void test_status_config_generation(void)
{
	struct ubus_request_data request = {};
	struct blob_attr *field;
	struct cfg cfg = {};

	state_reset();
	uv.cfg = &cfg;
	uv.hal = &fake_hal;
	uv.config_generation = 7;

	CASE("voice status carries the configuration generation");
	net_dump_calls = 0;
	CHECK_UINT(voice_status(&request_context, NULL, &request, "status",
				NULL), 0);
	field = field_get(reply, "config_generation", BLOBMSG_TYPE_INT32);
	CHECK(field != NULL);
	if (field)
		CHECK_UINT(blobmsg_get_u32(field), 7);
	/* the network table is the core tracker's, written into the reply */
	CHECK_UINT(net_dump_calls, 1);

	CASE("and the next reply shows the generation a reload moved to");
	uv.config_generation++;
	CHECK_UINT(voice_status(&request_context, NULL, &request, "status",
				NULL), 0);
	field = field_get(reply, "config_generation", BLOBMSG_TYPE_INT32);
	CHECK(field != NULL);
	if (field)
		CHECK_UINT(blobmsg_get_u32(field), 8);

	uv.config_generation = 0;
}

static bool nested_string_is(struct blob_attr *table, const char *name,
			     const char *want)
{
	struct blob_attr *field = nested_field_get(table, name,
						   BLOBMSG_TYPE_STRING);

	return field && !strcmp(blobmsg_get_string(field), want);
}

/*
 * Every reply carries a signaling table from the registry. Under a null
 * table, it also keeps each key that the API schema requires. That table
 * writes an empty lines array, and the core writes its media counter.
 */
static void test_status_signaling(void)
{
	struct ubus_request_data request = {};
	struct blob_attr *field;
	struct cfg cfg = {};

	state_reset();
	uv.cfg = &cfg;
	uv.hal = &fake_hal;

	CASE("voice status carries the running signaling table");
	CHECK_UINT(voice_status(&request_context, NULL, &request, "status",
				NULL), 0);
	field = field_get(reply, "signaling", BLOBMSG_TYPE_TABLE);
	CHECK(field != NULL);
	CHECK(nested_string_is(field, "protocol", "sip"));
	CHECK(nested_string_is(field, "state", "running"));
	CHECK(nested_field_get(field, "error", BLOBMSG_TYPE_STRING) == NULL);

	CASE("voice status under the null table names the reason");
	fake_null = true;
	CHECK_UINT(voice_status(&request_context, NULL, &request, "status",
				NULL), 0);
	field = field_get(reply, "signaling", BLOBMSG_TYPE_TABLE);
	CHECK(nested_string_is(field, "state", "unavailable"));
	CHECK(nested_string_is(field, "error",
			       "the image does not carry this protocol"));

	CASE("voice status under the null table keeps the required keys");
	field = field_get(reply, "lines", BLOBMSG_TYPE_ARRAY);
	CHECK(field != NULL);
	CHECK_UINT(array_len(field), 0);
	field = field_get(reply, "media", BLOBMSG_TYPE_TABLE);
	field = nested_field_get(field, "opus_dtmf_fallbacks",
				 BLOBMSG_TYPE_INT32);
	CHECK(field != NULL);
	if (field)
		CHECK_UINT(blobmsg_get_u32(field), 0);

	fake_null = false;

	CASE("voice status carries no protocol table of a table without one");
	CHECK_UINT(voice_status(&request_context, NULL, &request, "status",
				NULL), 0);
	CHECK(field_get(reply, "fake_top", BLOBMSG_TYPE_TABLE) == NULL);

	CASE("voice status carries the top-level table of the protocol");
	fake_top = true;
	CHECK_UINT(voice_status(&request_context, NULL, &request, "status",
				NULL), 0);
	field = field_get(reply, "fake_top", BLOBMSG_TYPE_TABLE);
	CHECK(field != NULL);
	field = nested_field_get(field, "endpoints", BLOBMSG_TYPE_INT32);
	CHECK(field != NULL);
	if (field)
		CHECK_UINT(blobmsg_get_u32(field), 2);
	fake_top = false;

	CASE("voice status under mgcp carries endpoint rows and the mgcp "
	     "table");
	fake_mgcp = true;
	CHECK_UINT(voice_status(&request_context, NULL, &request, "status",
				NULL), 0);
	field = field_get(reply, "signaling", BLOBMSG_TYPE_TABLE);
	CHECK(nested_string_is(field, "protocol", "mgcp"));
	CHECK(nested_string_is(field, "state", "running"));
	field = field_get(reply, "lines", BLOBMSG_TYPE_ARRAY);
	CHECK(field != NULL);
	CHECK_UINT(array_len(field), 1);
	field = field_get(reply, "mgcp", BLOBMSG_TYPE_TABLE);
	CHECK(nested_string_is(field, "socket", "bound"));
	CHECK(field_get(reply, "fake_top", BLOBMSG_TYPE_TABLE) == NULL);
	fake_mgcp = false;
}

static void hal_rt_field_check(struct blob_attr *rt, const char *name,
			       int type, const char *want_string,
			       int32_t want_int)
{
	struct blob_attr *field = nested_field_get(rt, name, type);

	CHECK(field != NULL);
	if (!field)
		return;
	if (type == BLOBMSG_TYPE_STRING)
		CHECK(!strcmp(blobmsg_get_string(field), want_string));
	else if (type == BLOBMSG_TYPE_BOOL)
		CHECK_UINT(blobmsg_get_bool(field), (unsigned)want_int);
	else
		CHECK((int32_t)blobmsg_get_u32(field) == want_int);
}

/*
 * The rt table under hal is the one optional member of the attached half. A
 * backend that reports no outcome (stub) leaves it out. A row of zeros
 * would read as a granted request at priority 0.
 */
static void test_hal_rt_table(void)
{
	struct ubus_request_data request = {};
	struct blob_attr *hal, *rt;
	struct cfg cfg = {};

	state_reset();
	uv.cfg = &cfg;
	uv.hal = &fake_hal;

	CASE("voice status leaves the rt table out of a backend that reports none");
	CHECK_UINT(voice_status(&request_context, NULL, &request, "status",
				NULL), 0);
	hal = field_get(reply, "hal", BLOBMSG_TYPE_TABLE);
	CHECK(hal != NULL);
	CHECK(nested_field_get(hal, "rt", BLOBMSG_TYPE_TABLE) == NULL);

	CASE("and names the refused request when the backend reported one");
	fake_cap.rt_policy = HAL_RT_POLICY_OTHER;
	fake_cap.rt_net_policy = HAL_RT_POLICY_OTHER;
	fake_cap.rt_prio = 50;
	fake_cap.rt_sched_errno = EPERM;
	fake_cap.rt_cpu = -1;
	fake_cap.rt_mlock = true;
	CHECK_UINT(voice_status(&request_context, NULL, &request, "status",
				NULL), 0);
	hal = field_get(reply, "hal", BLOBMSG_TYPE_TABLE);
	rt = nested_field_get(hal, "rt", BLOBMSG_TYPE_TABLE);
	CHECK(rt != NULL);
	hal_rt_field_check(rt, "policy", BLOBMSG_TYPE_STRING, "other", 0);
	hal_rt_field_check(rt, "prio", BLOBMSG_TYPE_INT32, NULL, 50);
	hal_rt_field_check(rt, "sched_errno", BLOBMSG_TYPE_INT32, NULL, EPERM);
	hal_rt_field_check(rt, "cpu", BLOBMSG_TYPE_INT32, NULL, -1);
	hal_rt_field_check(rt, "mlock", BLOBMSG_TYPE_BOOL, NULL, 1);
	hal_rt_field_check(rt, "net_policy", BLOBMSG_TYPE_STRING, "other", 0);

	CASE("a granted request reads fifo with its cpu");
	fake_cap.rt_policy = HAL_RT_POLICY_FIFO;
	fake_cap.rt_net_policy = HAL_RT_POLICY_FIFO;
	fake_cap.rt_sched_errno = 0;
	fake_cap.rt_cpu = 3;
	CHECK_UINT(voice_status(&request_context, NULL, &request, "status",
				NULL), 0);
	hal = field_get(reply, "hal", BLOBMSG_TYPE_TABLE);
	rt = nested_field_get(hal, "rt", BLOBMSG_TYPE_TABLE);
	CHECK(rt != NULL);
	hal_rt_field_check(rt, "policy", BLOBMSG_TYPE_STRING, "fifo", 0);
	hal_rt_field_check(rt, "net_policy", BLOBMSG_TYPE_STRING, "fifo", 0);
	hal_rt_field_check(rt, "cpu", BLOBMSG_TYPE_INT32, NULL, 3);
	hal_rt_field_check(rt, "sched_errno", BLOBMSG_TYPE_INT32, NULL, 0);

	CASE("a policy the backend could not read prints as unknown");
	fake_cap.rt_net_policy = HAL_RT_POLICY_UNKNOWN;
	CHECK_UINT(voice_status(&request_context, NULL, &request, "status",
				NULL), 0);
	hal = field_get(reply, "hal", BLOBMSG_TYPE_TABLE);
	rt = nested_field_get(hal, "rt", BLOBMSG_TYPE_TABLE);
	hal_rt_field_check(rt, "net_policy", BLOBMSG_TYPE_STRING, "unknown", 0);

	fake_cap.rt_policy = 0;
	fake_cap.rt_net_policy = 0;
	fake_cap.rt_prio = 0;
	fake_cap.rt_sched_errno = 0;
	fake_cap.rt_cpu = 0;
	fake_cap.rt_mlock = false;
}

static unsigned int table_len(struct blob_attr *table)
{
	struct blob_attr *cur;
	unsigned int count = 0;
	size_t rem;

	if (!table)
		return 0;
	blobmsg_for_each_attr(cur, table, rem)
		count++;
	return count;
}

static struct blob_attr *attr_at(struct blob_attr *array, unsigned int want)
{
	struct blob_attr *cur;
	unsigned int at = 0;
	size_t rem;

	if (!array)
		return NULL;
	blobmsg_for_each_attr(cur, array, rem) {
		if (at++ == want)
			return cur;
	}
	return NULL;
}

/*
 * The two records the monitor cases render. Every field carries its own value
 * and the six decoded bits alternate inside a record. The second record
 * inverts every boolean, so each one is asserted at 1 in one case and at 0 in
 * another. A dropped copy then cannot pass.
 */
static const struct hal_monitor wire_monitor = {
	.stamp_ms = 4242,
	.valid = 1,
	.vtr_mv = 1001,
	.vtip_mv = -1002,
	.vring_mv = 1003,
	.vlong_mv = -1004,
	.vbat_mv = -48005,
	.vdc_mv = 1006,
	.itr_ua = -1007,
	.itip_ua = 1008,
	.iring_ua = -1009,
	.ilong_ua = 1010,
	.p_hvic_mw = 1011,
	.vbat_tgt_mv = -48012,
	.vbat_irq_th_mv = 5013,
	.pd_dcdc = 0x100014u,
	.dcdc_status = 0x000015u,
	.lcrrtp = 0x02u,
	.linefeed = 0x11u,
	.dcdc_down = 1,
	.ovlo = 0,
	.settled = 1,
	.voc_err = 0,
	.lcr = 1,
	.rtp = 0,
	.fault_bits = 0x82u,
	.fault_open = 1,
	.fault_held = 0,
};

static const struct hal_monitor wire_monitor_flip = {
	.stamp_ms = 5353,
	.valid = 1,
	.stale = 1,
	.vtr_mv = -2001,
	.vtip_mv = 2002,
	.vring_mv = -2003,
	.vlong_mv = 2004,
	.vbat_mv = 2005,
	.vdc_mv = -2006,
	.itr_ua = 2007,
	.itip_ua = -2008,
	.iring_ua = 2009,
	.ilong_ua = -2010,
	.p_hvic_mw = 2011,
	.vbat_tgt_mv = 2012,
	.vbat_irq_th_mv = -2013,
	.pd_dcdc = 0x400024u,
	.dcdc_status = 0x400025u,
	.lcrrtp = 0x09u,
	.linefeed = 0x12u,
	.dcdc_down = 0,
	.ovlo = 1,
	.settled = 0,
	.voc_err = 1,
	.lcr = 0,
	.rtp = 1,
	.fault_bits = 0x10200u,
	.fault_open = 0,
	.fault_held = 1,
};

static void monitor_i32_check(struct blob_attr *row, const char *name,
			      int32_t want)
{
	struct blob_attr *sub = nested_field_get(row, name, BLOBMSG_TYPE_INT32);

	CHECK(sub && (int32_t)blobmsg_get_u32(sub) == want);
}

static void monitor_u32_check(struct blob_attr *row, const char *name,
			      uint32_t want)
{
	struct blob_attr *sub = nested_field_get(row, name, BLOBMSG_TYPE_INT32);

	CHECK(sub && blobmsg_get_u32(sub) == want);
}

static void monitor_bool_check(struct blob_attr *row, const char *name,
			       bool want)
{
	struct blob_attr *sub = nested_field_get(row, name, BLOBMSG_TYPE_BOOL);

	CHECK(sub && blobmsg_get_bool(sub) == want);
}

static void monitor_values_check(struct blob_attr *row,
				 const struct hal_monitor *mon)
{
	monitor_i32_check(row, "vtr_mv", mon->vtr_mv);
	monitor_i32_check(row, "vtip_mv", mon->vtip_mv);
	monitor_i32_check(row, "vring_mv", mon->vring_mv);
	monitor_i32_check(row, "vlong_mv", mon->vlong_mv);
	monitor_i32_check(row, "vbat_mv", mon->vbat_mv);
	monitor_i32_check(row, "vdc_mv", mon->vdc_mv);
	monitor_i32_check(row, "itr_ua", mon->itr_ua);
	monitor_i32_check(row, "itip_ua", mon->itip_ua);
	monitor_i32_check(row, "iring_ua", mon->iring_ua);
	monitor_i32_check(row, "ilong_ua", mon->ilong_ua);
	monitor_i32_check(row, "p_hvic_mw", mon->p_hvic_mw);
	monitor_i32_check(row, "vbat_tgt_mv", mon->vbat_tgt_mv);
	monitor_i32_check(row, "vbat_irq_th_mv", mon->vbat_irq_th_mv);
	monitor_u32_check(row, "pd_dcdc", mon->pd_dcdc);
	monitor_u32_check(row, "dcdc_status", mon->dcdc_status);
	monitor_u32_check(row, "lcrrtp", mon->lcrrtp);
	monitor_u32_check(row, "linefeed", mon->linefeed);
	monitor_bool_check(row, "dcdc_down", mon->dcdc_down);
	monitor_bool_check(row, "ovlo", mon->ovlo);
	monitor_bool_check(row, "settled", mon->settled);
	monitor_bool_check(row, "voc_err", mon->voc_err);
	monitor_bool_check(row, "lcr", mon->lcr);
	monitor_bool_check(row, "rtp", mon->rtp);
}

static void monitor_fault_check(struct blob_attr *row,
				const struct hal_monitor *mon)
{
	monitor_u32_check(row, "fault_bits", mon->fault_bits);
	monitor_bool_check(row, "fault_open", mon->fault_open);
	monitor_bool_check(row, "fault_held", mon->fault_held);
}

/* The first monitor row of a fresh "voice status" reply. */
static struct blob_attr *monitor_row_first(struct ubus_request_data *request)
{
	CHECK_UINT(voice_status(&request_context, NULL, request, "status",
				NULL), 0);

	return first_attr(field_get(reply, "monitor", BLOBMSG_TYPE_ARRAY));
}

static void test_status_diagnostics(void)
{
	struct ubus_request_data request = {};
	struct blob_attr *field, *row, *sub;
	struct cfg cfg = {};

	state_reset();
	uv.cfg = &cfg;
	uv.hal = &fake_hal;
	/*
	 * The fake table is the minimal shape: the jb and media getters answer
	 * HAL_UNSUPPORTED and the canceller getter is NULL. The bounds come
	 * from the cap (2 lines, 2 streams), so jb and lec are a constant grid
	 * of never rows and the NULL canceller op is never called.
	 */
	CASE("voice status emits a constant jb and lec grid on the minimal "
	     "shape");
	CHECK_UINT(voice_status(&request_context, NULL, &request, "status",
				NULL), 0);
	field = field_get(reply, "jb", BLOBMSG_TYPE_ARRAY);
	CHECK(field != NULL);
	CHECK_UINT(array_len(field), 4);
	field = field_get(reply, "lec", BLOBMSG_TYPE_ARRAY);
	CHECK(field != NULL);
	CHECK_UINT(array_len(field), 2);
	/* streams stays gated on its op, so an unsupported media getter leaves
	 * it empty; jb_events has no events. */
	field = field_get(reply, "streams", BLOBMSG_TYPE_ARRAY);
	CHECK(field != NULL);
	CHECK_UINT(array_len(field), 0);
	field = field_get(reply, "jb_events", BLOBMSG_TYPE_ARRAY);
	CHECK(field != NULL);
	CHECK_UINT(array_len(field), 0);

	/*
	 * A row that never answered says so in one word and names no instant,
	 * because it was never read. The suite's cdr_jb_dump renders the mode
	 * word alone, so a jb row carries no counter keys here; the lec
	 * renderer is real, so the four table rules are readable on it.
	 */
	CASE("a jb row that never answered says never and names no instant");
	row = first_attr(field_get(reply, "jb", BLOBMSG_TYPE_ARRAY));
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 4);
	sub = nested_field_get(row, "sample", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "never"));
	CHECK(nested_field_get(row, "stamp_ms", BLOBMSG_TYPE_INT32) == NULL);

	/* A never row measured no playout mode either. The enum's zero word is
	 * "adaptive", which would read as a mode the buffer runs. */
	CASE("and its playout mode reads unknown, not the enum's zero word");
	sub = nested_field_get(row, "mode", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "unknown"));

	CASE("a lec row that never answered carries no state and no instant");
	row = first_attr(field_get(reply, "lec", BLOBMSG_TYPE_ARRAY));
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 4);
	sub = nested_field_get(row, "sample", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "never"));
	CHECK(nested_field_get(row, "stamp_ms", BLOBMSG_TYPE_INT32) == NULL);
	CHECK(nested_field_get(row, "state", BLOBMSG_TYPE_TABLE) == NULL);

	/* The two layers that measure nothing stand in it anyway, and the push
	 * record does not, because no push happened. */
	CASE("and it still carries the config and policy tables alone");
	CHECK(nested_field_get(row, "config", BLOBMSG_TYPE_TABLE) != NULL);
	CHECK(nested_field_get(row, "policy", BLOBMSG_TYPE_TABLE) != NULL);
	CHECK(nested_field_get(row, "applied", BLOBMSG_TYPE_TABLE) == NULL);

	/*
	 * The monitor grid takes its bounds from the same cap. The op answers
	 * unsupported here, the answer of a backend that measures no line, so
	 * every row reads never.
	 */
	CASE("voice status emits a constant monitor grid on a refused answer");
	field = field_get(reply, "monitor", BLOBMSG_TYPE_ARRAY);
	CHECK(field != NULL);
	CHECK_UINT(array_len(field), 2);

	CASE("a monitor row with no reading names no instant and no measurement");
	row = first_attr(field);
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 5);
	sub = nested_field_get(row, "sample", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "never"));
	CHECK(nested_field_get(row, "stamp_ms", BLOBMSG_TYPE_INT32) == NULL);
	CHECK(nested_field_get(row, "vbat_mv", BLOBMSG_TYPE_INT32) == NULL);
	CHECK(nested_field_get(row, "pd_dcdc", BLOBMSG_TYPE_INT32) == NULL);
	CHECK(nested_field_get(row, "dcdc_down", BLOBMSG_TYPE_BOOL) == NULL);

	/* The three fault fields are the engine's own state of the line. They
	 * are defined before the first reading, so they stand in a never
	 * row. */
	CASE("and it still carries the three fault keys");
	sub = nested_field_get(row, "fault_bits", BLOBMSG_TYPE_INT32);
	CHECK(sub && blobmsg_get_u32(sub) == 0);
	sub = nested_field_get(row, "fault_open", BLOBMSG_TYPE_BOOL);
	CHECK(sub && !blobmsg_get_bool(sub));
	sub = nested_field_get(row, "fault_held", BLOBMSG_TYPE_BOOL);
	CHECK(sub && !blobmsg_get_bool(sub));

	uv.hal = NULL;
	CASE("voice status empties the grids with no hal at all");
	CHECK_UINT(voice_status(&request_context, NULL, &request, "status",
				NULL), 0);
	field = field_get(reply, "streams", BLOBMSG_TYPE_ARRAY);
	CHECK(field != NULL);
	CHECK_UINT(array_len(field), 0);
	field = field_get(reply, "jb", BLOBMSG_TYPE_ARRAY);
	CHECK(field != NULL);
	CHECK_UINT(array_len(field), 0);
	field = field_get(reply, "lec", BLOBMSG_TYPE_ARRAY);
	CHECK(field != NULL);
	CHECK_UINT(array_len(field), 0);
	field = field_get(reply, "monitor", BLOBMSG_TYPE_ARRAY);
	CHECK(field != NULL);
	CHECK_UINT(array_len(field), 0);
	CHECK(field_get(reply, "jb_events", BLOBMSG_TYPE_ARRAY) == NULL);
	uv.cfg = NULL;
}

/*
 * The four answers that the backend can give, through the whole method. The
 * op fills the record on every one of them. A case that reads a never row
 * therefore proves that the sweep dropped what a refusal wrote.
 */
static void test_monitor_grid_mappings(void)
{
	struct ubus_request_data request = {};
	struct blob_attr *row, *sub;
	struct cfg cfg = {};

	state_reset();
	uv.cfg = &cfg;
	uv.hal = &fake_hal;

	CASE("a reading that landed reads sampled, with its instant and values");
	mon_status = HAL_OK;
	mon_record = wire_monitor;
	row = monitor_row_first(&request);
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 29);
	sub = nested_field_get(row, "sample", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "sampled"));
	monitor_u32_check(row, "stamp_ms", 4242);
	monitor_values_check(row, &wire_monitor);
	monitor_fault_check(row, &wire_monitor);

	CASE("a record whose last read answered no data reads stale");
	mon_record = wire_monitor_flip;
	row = monitor_row_first(&request);
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 29);
	sub = nested_field_get(row, "sample", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "stale"));
	monitor_u32_check(row, "stamp_ms", 5353);
	monitor_values_check(row, &wire_monitor_flip);
	monitor_fault_check(row, &wire_monitor_flip);

	/* valid 0 is the backend's "no reading landed yet". The fault fields
	 * are defined there, so the row keeps them and nothing else. */
	CASE("a record with no reading reads never and keeps its fault set");
	mon_record = wire_monitor;
	mon_record.valid = 0;
	row = monitor_row_first(&request);
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 5);
	sub = nested_field_get(row, "sample", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "never"));
	CHECK(nested_field_get(row, "stamp_ms", BLOBMSG_TYPE_INT32) == NULL);
	monitor_fault_check(row, &wire_monitor);

	/* A dead line answers fatal. The record it wrote is dropped, so even
	 * the fault fields read zero rather than a state that never applied. */
	CASE("a dead line reads never from a record the sweep dropped");
	mon_status = HAL_FATAL;
	mon_record = wire_monitor;
	row = monitor_row_first(&request);
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 5);
	sub = nested_field_get(row, "sample", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "never"));
	monitor_u32_check(row, "fault_bits", 0);
	monitor_bool_check(row, "fault_open", false);
	monitor_bool_check(row, "fault_held", false);

	state_reset();
	uv.hal = NULL;
	uv.cfg = NULL;
}

/* The record every field of the row test reads back. Every counter carries its
 * own value, so a swapped pair of keys cannot pass, and the flag set is the
 * one shape only the three distinct keys can carry: the filter ran, the
 * processor is on and it did not act. loop_open reads 0 beside double_talk 1
 * and ref_ready 1, so a key that carried either other flag reads wrong. */
static const struct hal_lec_stats wire_lec = {
	.tail_ms = 32,
	.taps = 768,
	.frames = 1001,
	.hold_frames = 902,
	.skip_frames = 903,
	.retimes = 5,
	.nlp_active_frames = 904,
	.nlp_wait_frames = 905,
	.nlp_erle_frames = 906,
	.nlp_unlike_frames = 907,
	.nlp_dt_frames = 908,
	.nlp_quiet_frames = 909,
	.coef_peak_tap = 97,
	.loop_open = 0,
	.diverge_resets = 3,
	.probes = 4,
	.erle_mdb = -2500,
	.coef_energy_micro = 5001,
	.echo_gain_micro = 5002,
	.double_talk = 1,
	.nlp_active = 0,
	.ref_delay_samples = 240,
	.ref_ready = 1,
	.nlp_enabled = 1,
	.ec_active = 1,
	.passthrough = HAL_PASSTHROUGH_OFF,
};

static struct blob_attr *lec_table_get(struct blob_attr *row, const char *name)
{
	return nested_field_get(row, name, BLOBMSG_TYPE_TABLE);
}

/* Every key of the state table, with the value wire_lec carries for it, so a
 * swapped pair of keys cannot pass. The two words are given, because three
 * cases drive them off a record of their own. */
static void lec_state_check(struct blob_attr *row, const char *ec,
			    const char *nlp)
{
	struct blob_attr *t = lec_table_get(row, "state");
	struct blob_attr *sub;

	CHECK(t != NULL);
	CHECK_UINT(table_len(t), 24);
	sub = nested_field_get(t, "ec", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), ec));
	sub = nested_field_get(t, "nlp", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), nlp));
	sub = nested_field_get(t, "double_talk", BLOBMSG_TYPE_BOOL);
	CHECK(sub && blobmsg_get_bool(sub));
	sub = nested_field_get(t, "tail_ms", BLOBMSG_TYPE_INT32);
	CHECK(sub && blobmsg_get_u32(sub) == 32);
	sub = nested_field_get(t, "taps", BLOBMSG_TYPE_INT32);
	CHECK(sub && blobmsg_get_u32(sub) == 768);
	sub = nested_field_get(t, "frames", BLOBMSG_TYPE_INT64);
	CHECK(sub && blobmsg_get_u64(sub) == 1001);
	sub = nested_field_get(t, "hold_frames", BLOBMSG_TYPE_INT64);
	CHECK(sub && blobmsg_get_u64(sub) == 902);
	sub = nested_field_get(t, "skip_frames", BLOBMSG_TYPE_INT64);
	CHECK(sub && blobmsg_get_u64(sub) == 903);
	sub = nested_field_get(t, "diverge_resets", BLOBMSG_TYPE_INT64);
	CHECK(sub && blobmsg_get_u64(sub) == 3);
	sub = nested_field_get(t, "retimes", BLOBMSG_TYPE_INT64);
	CHECK(sub && blobmsg_get_u64(sub) == 5);
	sub = nested_field_get(t, "probes", BLOBMSG_TYPE_INT64);
	CHECK(sub && blobmsg_get_u64(sub) == 4);
	sub = nested_field_get(t, "nlp_active_frames", BLOBMSG_TYPE_INT64);
	CHECK(sub && blobmsg_get_u64(sub) == 904);
	sub = nested_field_get(t, "nlp_wait_frames", BLOBMSG_TYPE_INT64);
	CHECK(sub && blobmsg_get_u64(sub) == 905);
	sub = nested_field_get(t, "nlp_erle_frames", BLOBMSG_TYPE_INT64);
	CHECK(sub && blobmsg_get_u64(sub) == 906);
	sub = nested_field_get(t, "nlp_unlike_frames", BLOBMSG_TYPE_INT64);
	CHECK(sub && blobmsg_get_u64(sub) == 907);
	sub = nested_field_get(t, "nlp_dt_frames", BLOBMSG_TYPE_INT64);
	CHECK(sub && blobmsg_get_u64(sub) == 908);
	sub = nested_field_get(t, "nlp_quiet_frames", BLOBMSG_TYPE_INT64);
	CHECK(sub && blobmsg_get_u64(sub) == 909);
	sub = nested_field_get(t, "erle_mdb", BLOBMSG_TYPE_INT32);
	CHECK(sub && (int32_t)blobmsg_get_u32(sub) == -2500);
	sub = nested_field_get(t, "coef_energy_micro", BLOBMSG_TYPE_INT64);
	CHECK(sub && blobmsg_get_u64(sub) == 5001);
	sub = nested_field_get(t, "coef_peak_tap", BLOBMSG_TYPE_INT32);
	CHECK(sub && blobmsg_get_u32(sub) == 97);
	sub = nested_field_get(t, "echo_gain_micro", BLOBMSG_TYPE_INT64);
	CHECK(sub && blobmsg_get_u64(sub) == 5002);
	sub = nested_field_get(t, "ref_delay_samples", BLOBMSG_TYPE_INT32);
	CHECK(sub && blobmsg_get_u32(sub) == 240);
	sub = nested_field_get(t, "ref_ready", BLOBMSG_TYPE_BOOL);
	CHECK(sub && blobmsg_get_bool(sub));
	sub = nested_field_get(t, "loop_open", BLOBMSG_TYPE_BOOL);
	CHECK(sub && !blobmsg_get_bool(sub));
}

/* The uci layer, read back against the view the fake answers. nlp is the one
 * key of the table that is presence gated. */
static void lec_config_check(struct blob_attr *row)
{
	const struct line_lec_view *v = &fake_lec_view;
	struct blob_attr *t = lec_table_get(row, "config");
	struct blob_attr *sub;

	CHECK(t != NULL);
	CHECK_UINT(table_len(t), v->config.nlp_set ? 5 : 4);
	sub = nested_field_get(t, "control", BLOBMSG_TYPE_BOOL);
	CHECK(sub && blobmsg_get_bool(sub) == v->config.control);
	sub = nested_field_get(t, "enable", BLOBMSG_TYPE_BOOL);
	CHECK(sub && blobmsg_get_bool(sub) == v->config.enable);
	sub = nested_field_get(t, "nlp", BLOBMSG_TYPE_BOOL);
	if (v->config.nlp_set)
		CHECK(sub && blobmsg_get_bool(sub) == v->config.nlp);
	else
		CHECK(sub == NULL);
	sub = nested_field_get(t, "tone_latch", BLOBMSG_TYPE_BOOL);
	CHECK(sub && blobmsg_get_bool(sub) == v->config.tone_latch);
	sub = nested_field_get(t, "tone_force_disable", BLOBMSG_TYPE_BOOL);
	CHECK(sub && blobmsg_get_bool(sub) == v->config.tone_force_disable);
}

/* The push record, absent whenever the view says no push happened. */
static void lec_applied_check(struct blob_attr *row)
{
	const struct line_lec_view *v = &fake_lec_view;
	struct blob_attr *t = lec_table_get(row, "applied");
	struct blob_attr *sub;

	if (!v->applied.valid) {
		CHECK(t == NULL);
		return;
	}

	CHECK(t != NULL);
	CHECK_UINT(table_len(t), 7);
	sub = nested_field_get(t, "ec", BLOBMSG_TYPE_BOOL);
	CHECK(sub && blobmsg_get_bool(sub) == v->applied.ec);
	sub = nested_field_get(t, "nlp", BLOBMSG_TYPE_BOOL);
	CHECK(sub && blobmsg_get_bool(sub) == v->applied.nlp);
	sub = nested_field_get(t, "tone_latch", BLOBMSG_TYPE_BOOL);
	CHECK(sub && blobmsg_get_bool(sub) == v->applied.tone_latch);
	sub = nested_field_get(t, "tone_force_disable", BLOBMSG_TYPE_BOOL);
	CHECK(sub && blobmsg_get_bool(sub) == v->applied.tone_force_disable);
	sub = nested_field_get(t, "stamp_ms", BLOBMSG_TYPE_INT32);
	CHECK(sub && blobmsg_get_u32(sub) == v->applied.stamp_ms);
	sub = nested_field_get(t, "status", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub),
			     hal_status_name(v->applied.status)));
	sub = nested_field_get(t, "pending", BLOBMSG_TYPE_BOOL);
	CHECK(sub && blobmsg_get_bool(sub) == v->applied.pending);
}

/* The two words that stand in every row, a never one included, because a line
 * that never ran a canceller still runs a policy. */
static void lec_row_policy_check(struct blob_attr *row, const char *kind,
				 const char *source)
{
	struct blob_attr *t = lec_table_get(row, "policy");
	struct blob_attr *sub;

	CHECK(t != NULL);
	sub = nested_field_get(t, "kind", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), kind));
	sub = nested_field_get(t, "source", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), source));
}

/*
 * The reference clock is read directly and not through uvoice_mono_ms(), the
 * test_line rule: a bracket built with the call under test would hold whatever
 * clock that call read.
 */
static uint32_t mono_ms_ref(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/*
 * The view is filled through its one writer, so the row reads the instant the
 * daemon received the record at. The event stamp is a backend tick and is set
 * far from this host's clock on purpose: a writer that copied it would put a
 * tick on the wire beside two monotonic instants.
 */
static void lec_policy_view_set(unsigned int line, uint8_t kind, uint8_t reason,
				uint16_t tone, uint8_t direction, uint32_t stamp)
{
	struct hal_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.line = line;
	ev.stamp_ms = stamp;
	ev.u.lec.kind = kind;
	ev.u.lec.reason = reason;
	ev.u.lec.tone = tone;
	ev.u.lec.direction = direction;
	uvoice_lec_policy_set(line, &ev);
}

/* the move's instant, bounded against a direct clock read around the call */
static void lec_tone_stamp_check(struct blob_attr *row, uint32_t before,
				 uint32_t after)
{
	struct blob_attr *sub;

	sub = nested_field_get(lec_table_get(row, "policy"), "tone_stamp_ms",
			       BLOBMSG_TYPE_INT32);
	CHECK(sub != NULL);
	if (!sub)
		return;
	CHECK(blobmsg_get_u32(sub) >= before);
	CHECK(blobmsg_get_u32(sub) <= after);
}

/*
 * The keys of the jb row, and its stamp rule. The cdr_jb_dump of this suite
 * renders the mode word alone, so a row here is the work of the writer alone.
 * test_cdr checks the counter vocabulary.
 */
static void test_jb_row_wire(void)
{
	struct hal_jb_stats jb;
	struct blob_attr *row, *sub;

	memset(&jb, 0, sizeof(jb));
	jb.mode = HAL_JB_FIXED;

	CASE("a live jb row names the sample and no instant");
	blob_buf_init(&b, 0);
	jb_row_write(&b, 1, 0, &jb, SAMPLE_LIVE, 0);
	row = first_attr(b.head);
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 4);
	sub = nested_field_get(row, "line", BLOBMSG_TYPE_INT32);
	CHECK(sub && blobmsg_get_u32(sub) == 1);
	sub = nested_field_get(row, "stream", BLOBMSG_TYPE_INT32);
	CHECK(sub && blobmsg_get_u32(sub) == 0);
	sub = nested_field_get(row, "sample", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "live"));
	CHECK(nested_field_get(row, "stamp_ms", BLOBMSG_TYPE_INT32) == NULL);

	/* the writer renders the word the record carries and picks none of its
	 * own; jb_row_dump() is what chooses the word of a never row */
	CASE("and the mode word is the one the record carries");
	sub = nested_field_get(row, "mode", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "fixed"));

	CASE("a cached jb row names the instant it was read at");
	blob_buf_init(&b, 0);
	jb_row_write(&b, 1, 0, &jb, SAMPLE_LAST_CALL, 7007);
	row = first_attr(b.head);
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 5);
	sub = nested_field_get(row, "sample", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "last_call"));
	sub = nested_field_get(row, "stamp_ms", BLOBMSG_TYPE_INT32);
	CHECK(sub && blobmsg_get_u32(sub) == 7007);

	CASE("a jb row that never answered names no instant either");
	blob_buf_init(&b, 0);
	jb_row_write(&b, 1, 0, &jb, SAMPLE_NEVER, 7007);
	row = first_attr(b.head);
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 4);
	sub = nested_field_get(row, "sample", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "never"));
	CHECK(nested_field_get(row, "stamp_ms", BLOBMSG_TYPE_INT32) == NULL);
}

/*
 * The lec row on the wire, key by key. The case calls the renderer directly
 * (this suite compiles ubus.c in). The assertions check the names, the blob
 * types and the presence rules that the schema documents. The key counts fail
 * when a table gains a field that this test and the schema do not name. The
 * cache branch then runs through the real method, because the sample word
 * comes from lec_row_dump and not from the writer.
 */
static void test_lec_row_wire(void)
{
	struct ubus_request_data request = {};
	struct hal_lec_stats lec;
	struct blob_attr *field, *row, *sub;
	uint32_t before, after;
	struct cfg cfg = {};

	state_reset();
	memset(uv.lec_last, 0, sizeof(uv.lec_last));

	CASE("a live lec row carries five keys and names no instant");
	blob_buf_init(&b, 0);
	lec_row_write(&b, 7, &wire_lec, SAMPLE_LIVE, 0);
	row = first_attr(b.head);
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 5);
	sub = nested_field_get(row, "line", BLOBMSG_TYPE_INT32);
	CHECK(sub && blobmsg_get_u32(sub) == 7);
	sub = nested_field_get(row, "sample", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "live"));
	CHECK(nested_field_get(row, "stamp_ms", BLOBMSG_TYPE_INT32) == NULL);

	CASE("and its tables read config, policy and state, no push record");
	lec_config_check(row);
	lec_applied_check(row);
	lec_state_check(row, "on", "on");
	/* line 7 is past HAL_LINE_MAX, so no view can be indexed for it: the
	 * row is the shape of a line whose disabler never moved */
	lec_row_policy_check(row, "voice", "config");
	CHECK_UINT(table_len(lec_table_get(row, "policy")), 2);

	CASE("a push record adds the applied table and nothing else");
	fake_lec_view.applied.valid = true;
	fake_lec_view.applied.ec = true;
	fake_lec_view.applied.nlp = false;
	fake_lec_view.applied.tone_latch = true;
	fake_lec_view.applied.tone_force_disable = false;
	fake_lec_view.applied.stamp_ms = 8181;
	fake_lec_view.applied.status = HAL_TRANSIENT;
	fake_lec_view.applied.pending = true;
	blob_buf_init(&b, 0);
	lec_row_write(&b, 7, &wire_lec, SAMPLE_LIVE, 0);
	row = first_attr(b.head);
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 6);
	lec_applied_check(row);

	CASE("a cached lec row adds the stamp and the last_call word");
	blob_buf_init(&b, 0);
	lec_row_write(&b, 7, &wire_lec, SAMPLE_LAST_CALL, 4242);
	row = first_attr(b.head);
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 7);
	sub = nested_field_get(row, "sample", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "last_call"));
	sub = nested_field_get(row, "stamp_ms", BLOBMSG_TYPE_INT32);
	CHECK(sub && blobmsg_get_u32(sub) == 4242);
	lec_state_check(row, "on", "on");

	CASE("a row that never answered drops the state table and the stamp");
	blob_buf_init(&b, 0);
	lec_row_write(&b, 7, &wire_lec, SAMPLE_NEVER, 4242);
	row = first_attr(b.head);
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 5);
	sub = nested_field_get(row, "sample", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "never"));
	CHECK(nested_field_get(row, "stamp_ms", BLOBMSG_TYPE_INT32) == NULL);
	CHECK(lec_table_get(row, "state") == NULL);
	lec_config_check(row);
	lec_applied_check(row);
	lec_row_policy_check(row, "voice", "config");

	CASE("the configured options reach the config table unswapped");
	state_reset();
	fake_lec_view.config.control = true;
	fake_lec_view.config.enable = false;
	fake_lec_view.config.nlp_set = true;
	fake_lec_view.config.nlp = true;
	fake_lec_view.config.tone_latch = true;
	fake_lec_view.config.tone_force_disable = false;
	blob_buf_init(&b, 0);
	lec_row_write(&b, 0, &wire_lec, SAMPLE_LIVE, 0);
	row = first_attr(b.head);
	CHECK(row != NULL);
	lec_config_check(row);
	state_reset();

	/* One word for the two processor flags, and the filter word beside it
	 * reads its own field. */
	CASE("the processor word reads off, on and acting in turn");
	lec = wire_lec;
	lec.nlp_enabled = 0;
	blob_buf_init(&b, 0);
	lec_row_write(&b, 0, &lec, SAMPLE_LIVE, 0);
	lec_state_check(first_attr(b.head), "on", "off");
	lec.nlp_enabled = 1;
	blob_buf_init(&b, 0);
	lec_row_write(&b, 0, &lec, SAMPLE_LIVE, 0);
	lec_state_check(first_attr(b.head), "on", "on");
	lec.nlp_active = 1;
	blob_buf_init(&b, 0);
	lec_row_write(&b, 0, &lec, SAMPLE_LIVE, 0);
	lec_state_check(first_attr(b.head), "on", "acting");

	CASE("and the canceller word follows the measured frame alone");
	lec = wire_lec;
	lec.ec_active = 0;
	blob_buf_init(&b, 0);
	lec_row_write(&b, 0, &lec, SAMPLE_LIVE, 0);
	lec_state_check(first_attr(b.head), "off", "on");

	/*
	 * The engine's reading wins on a live row. The daemon resolves "voice"
	 * here, so a record that ran the modem policy names a carrier the
	 * daemon could not see, and the row reports that word and admits it
	 * cannot name the cause.
	 */
	CASE("a live engine kind the daemon did not name reads from engine");
	lec = wire_lec;
	lec.passthrough = HAL_PASSTHROUGH_MODEM;
	blob_buf_init(&b, 0);
	lec_row_write(&b, 0, &lec, SAMPLE_LIVE, 0);
	lec_row_policy_check(first_attr(b.head), "modem", "engine");

	CASE("and the same record on a cached row leaves the daemon's pair");
	blob_buf_init(&b, 0);
	lec_row_write(&b, 0, &lec, SAMPLE_LAST_CALL, 11);
	lec_row_policy_check(first_attr(b.head), "voice", "config");

	CASE("an engine kind the daemon named keeps the daemon's cause");
	fake_line_kind = HAL_PASSTHROUGH_MODEM;
	blob_buf_init(&b, 0);
	lec_row_write(&b, 0, &lec, SAMPLE_LIVE, 0);
	lec_row_policy_check(first_attr(b.head), "modem", "description");
	fake_line_kind = HAL_PASSTHROUGH_OFF;

	/*
	 * The precedence runs the other way too. A leg describes the modem kind
	 * and the engine ran none, so the canceller followed its configuration
	 * and the row must say that rather than repeat the description.
	 */
	CASE("an engine that ran no carrier beats a described one");
	fake_line_kind = HAL_PASSTHROUGH_MODEM;
	lec = wire_lec;
	lec.passthrough = HAL_PASSTHROUGH_OFF;
	blob_buf_init(&b, 0);
	lec_row_write(&b, 0, &lec, SAMPLE_LIVE, 0);
	lec_row_policy_check(first_attr(b.head), "voice", "engine");
	fake_line_kind = HAL_PASSTHROUGH_OFF;

	/* a backend that grew a kind this build does not name still has to
	 * produce a readable policy table */
	CASE("a live kind outside the contract reads unknown, from engine");
	lec = wire_lec;
	lec.passthrough = 42;
	blob_buf_init(&b, 0);
	lec_row_write(&b, 0, &lec, SAMPLE_LIVE, 0);
	lec_row_policy_check(first_attr(b.head), "unknown", "engine");

	/*
	 * The disabler cases read a cached row, where the daemon's own
	 * resolution is the whole answer: a live row would hand the kind to the
	 * engine record beside it, which is the rule the three cases above pin.
	 */
	CASE("a latched view names the tone, the feed and the action");
	before = mono_ms_ref();
	lec_policy_view_set(0, HAL_PASSTHROUGH_MODEM, HAL_LEC_REASON_LATCH,
			    HAL_TONE_DET_ANSBAR, HAL_TONE_DIR_REMOTE, 555);
	after = mono_ms_ref();
	blob_buf_init(&b, 0);
	lec_row_write(&b, 0, &wire_lec, SAMPLE_LAST_CALL, 12);
	row = first_attr(b.head);
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 6);
	lec_state_check(row, "on", "on");
	lec_row_policy_check(row, "modem", "tone");
	CHECK_UINT(table_len(lec_table_get(row, "policy")), 6);
	sub = nested_field_get(lec_table_get(row, "policy"), "tone",
			       BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "ANSBAR"));
	sub = nested_field_get(lec_table_get(row, "policy"), "tone_dir",
			       BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "REMOTE"));
	sub = nested_field_get(lec_table_get(row, "policy"), "tone_action",
			       BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "LATCH"));

	/* The instant is the daemon's, not the tick the record travelled with.
	 * A row that carried the tick would be an age no reader can take. */
	CASE("and the move's instant is the daemon's clock, not the tick");
	lec_tone_stamp_check(row, before, after);

	/*
	 * The precedence is the engine's (chan_passthrough_get): a committed
	 * description wins over the latch, even the weaker fax kind over a
	 * standing modem latch. A row that answered "modem" here would report a
	 * canceller policy the line is not running.
	 */
	CASE("a described leg wins over a standing latch");
	fake_line_kind = HAL_PASSTHROUGH_FAX;
	blob_buf_init(&b, 0);
	lec_row_write(&b, 0, &wire_lec, SAMPLE_LAST_CALL, 12);
	row = first_attr(b.head);
	lec_row_policy_check(row, "fax", "description");
	/* and the transition it displaced is still reported, because the
	 * disabler did make it */
	sub = nested_field_get(lec_table_get(row, "policy"), "tone_action",
			       BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "LATCH"));
	fake_line_kind = HAL_PASSTHROUGH_OFF;

	CASE("a released view names the action and neither tone nor feed");
	before = mono_ms_ref();
	lec_policy_view_set(0, HAL_PASSTHROUGH_OFF, HAL_LEC_REASON_RELEASE_QUIET,
			    0, 0, 777);
	after = mono_ms_ref();
	blob_buf_init(&b, 0);
	lec_row_write(&b, 0, &wire_lec, SAMPLE_LAST_CALL, 12);
	row = first_attr(b.head);
	CHECK(row != NULL);
	lec_row_policy_check(row, "voice", "config");
	sub = lec_table_get(row, "policy");
	CHECK_UINT(table_len(sub), 4);
	CHECK(nested_field_get(sub, "tone", BLOBMSG_TYPE_STRING) == NULL);
	CHECK(nested_field_get(sub, "tone_dir", BLOBMSG_TYPE_STRING) == NULL);
	sub = nested_field_get(lec_table_get(row, "policy"), "tone_action",
			       BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "RELEASE_QUIET"));
	lec_tone_stamp_check(row, before, after);

	/* the release a caller asked for, which is the one reason the daemon
	 * itself causes: the call layer read an answer tone as a false one */
	CASE("the caller release names its own action and no tone either");
	lec_policy_view_set(0, HAL_PASSTHROUGH_OFF,
			    HAL_LEC_REASON_RELEASE_CALLER, 0, 0, 888);
	blob_buf_init(&b, 0);
	lec_row_write(&b, 0, &wire_lec, SAMPLE_LAST_CALL, 12);
	row = first_attr(b.head);
	CHECK(row != NULL);
	sub = lec_table_get(row, "policy");
	CHECK_UINT(table_len(sub), 4);
	CHECK(nested_field_get(sub, "tone", BLOBMSG_TYPE_STRING) == NULL);
	CHECK(nested_field_get(sub, "tone_dir", BLOBMSG_TYPE_STRING) == NULL);
	sub = nested_field_get(lec_table_get(row, "policy"), "tone_action",
			       BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "RELEASE_CALLER"));

	/* A backend that grew a kind, a reason or a feed this build does not
	 * name must still produce a readable row. */
	CASE("a kind, a reason and a feed outside the contract still render");
	lec_policy_view_set(0, 42, 77, HAL_TONE_DET_CED, 9, 1);
	blob_buf_init(&b, 0);
	lec_row_write(&b, 0, &wire_lec, SAMPLE_LAST_CALL, 12);
	row = first_attr(b.head);
	lec_row_policy_check(row, "unknown", "tone");
	sub = nested_field_get(lec_table_get(row, "policy"), "tone_action",
			       BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "UNKNOWN"));

	memset(uv.lec_policy, 0, sizeof(uv.lec_policy));

	CASE("voice status serves the cached reading for a line that stopped answering");
	uv.cfg = &cfg;
	uv.hal = &fake_hal;
	uv.lec_last[1].valid = true;
	uv.lec_last[1].stamp_ms = 999;
	uv.lec_last[1].lec = wire_lec;
	CHECK_UINT(voice_status(&request_context, NULL, &request, "status",
				NULL), 0);
	field = field_get(reply, "lec", BLOBMSG_TYPE_ARRAY);
	CHECK(field != NULL);
	CHECK_UINT(array_len(field), 2);
	row = attr_at(field, 1);
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 6);
	sub = nested_field_get(row, "sample", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "last_call"));
	sub = nested_field_get(row, "stamp_ms", BLOBMSG_TYPE_INT32);
	CHECK(sub && blobmsg_get_u32(sub) == 999);
	lec_state_check(row, "on", "on");

	/* The line beside it never answered, so it carries no measurement at
	 * all; the two layers that measure nothing stand in both rows. */
	CASE("and the row beside it reads never, with no state and no stamp");
	row = attr_at(field, 0);
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 4);
	sub = nested_field_get(row, "sample", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "never"));
	CHECK(nested_field_get(row, "stamp_ms", BLOBMSG_TYPE_INT32) == NULL);
	CHECK(lec_table_get(row, "state") == NULL);
	lec_row_policy_check(row, "voice", "config");
	lec_row_policy_check(attr_at(field, 1), "voice", "config");

	CASE("and both rows the method rendered carry the config table");
	lec_config_check(attr_at(field, 0));
	lec_config_check(attr_at(field, 1));

	/*
	 * The push record does not follow the reading. A line that never
	 * answered a canceller has still been seized, so its row carries the
	 * applied table beside a sample of never and no state at all.
	 */
	CASE("a never row carries the push record of a line that was seized");
	fake_lec_view.applied.valid = true;
	fake_lec_view.applied.ec = true;
	fake_lec_view.applied.nlp = true;
	fake_lec_view.applied.stamp_ms = 3131;
	fake_lec_view.applied.status = HAL_OK;
	CHECK_UINT(voice_status(&request_context, NULL, &request, "status",
				NULL), 0);
	field = field_get(reply, "lec", BLOBMSG_TYPE_ARRAY);
	row = attr_at(field, 0);
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 5);
	sub = nested_field_get(row, "sample", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "never"));
	CHECK(lec_table_get(row, "state") == NULL);
	lec_applied_check(row);

	memset(uv.lec_last, 0, sizeof(uv.lec_last));
	memset(uv.lec_policy, 0, sizeof(uv.lec_policy));
	uv.hal = NULL;
	uv.cfg = NULL;
}

/*
 * The monitor row on the wire, key by key. The case calls the renderer
 * directly, in the shape of the lec row test. The assertions check the names,
 * the blob types and the presence rules that the schema documents. The key
 * counts fail when the row gains a field that this test and the schema do not
 * name.
 */
static void test_monitor_row_wire(void)
{
	struct blob_attr *row, *sub;

	state_reset();

	CASE("a sampled row carries its instant, every measurement and the faults");
	blob_buf_init(&b, 0);
	monitor_row_write(&b, 7, &wire_monitor, MONITOR_SAMPLED);
	row = first_attr(b.head);
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 29);
	sub = nested_field_get(row, "line", BLOBMSG_TYPE_INT32);
	CHECK(sub && blobmsg_get_u32(sub) == 7);
	sub = nested_field_get(row, "sample", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "sampled"));
	monitor_u32_check(row, "stamp_ms", 4242);
	monitor_values_check(row, &wire_monitor);
	monitor_fault_check(row, &wire_monitor);

	/* The other value of every boolean, from the record that inverts them
	 * all: a copy that dropped one of them reads the record's zero here. */
	CASE("the complementary record renders the other value of every flag");
	blob_buf_init(&b, 0);
	monitor_row_write(&b, 7, &wire_monitor_flip, MONITOR_SAMPLED);
	row = first_attr(b.head);
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 29);
	monitor_u32_check(row, "stamp_ms", 5353);
	monitor_values_check(row, &wire_monitor_flip);
	monitor_fault_check(row, &wire_monitor_flip);

	/* A stale row is the previous reading, so it keeps every key and the
	 * instant of that reading; one word tells the two apart. */
	CASE("a stale row keeps the same keys and the same instant");
	blob_buf_init(&b, 0);
	monitor_row_write(&b, 7, &wire_monitor, MONITOR_STALE);
	row = first_attr(b.head);
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 29);
	sub = nested_field_get(row, "sample", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "stale"));
	monitor_u32_check(row, "stamp_ms", 4242);
	monitor_values_check(row, &wire_monitor);
	monitor_fault_check(row, &wire_monitor);

	CASE("a never row drops the instant and every measurement");
	blob_buf_init(&b, 0);
	monitor_row_write(&b, 7, &wire_monitor, MONITOR_NEVER);
	row = first_attr(b.head);
	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 5);
	sub = nested_field_get(row, "sample", BLOBMSG_TYPE_STRING);
	CHECK(sub && !strcmp(blobmsg_get_string(sub), "never"));
	CHECK(nested_field_get(row, "stamp_ms", BLOBMSG_TYPE_INT32) == NULL);
	CHECK(nested_field_get(row, "vbat_mv", BLOBMSG_TYPE_INT32) == NULL);
	CHECK(nested_field_get(row, "linefeed", BLOBMSG_TYPE_INT32) == NULL);
	CHECK(nested_field_get(row, "lcr", BLOBMSG_TYPE_BOOL) == NULL);

	/* The fault fields are the backend engine's, not the sample's, so they
	 * stand even here. */
	CASE("and it still names the fault set of the line");
	monitor_fault_check(row, &wire_monitor);
}

static void line_rows_check(struct blob_buf *buf, const char *key,
			    unsigned int line, unsigned int count)
{
	struct blob_attr *array, *row, *field;
	size_t rem;
	unsigned int stream = 0;

	array = field_get(buf->head, key, BLOBMSG_TYPE_ARRAY);
	CHECK(array != NULL);
	CHECK_UINT(array_len(array), count);
	if (!array)
		return;

	blobmsg_for_each_attr(row, array, rem) {
		field = nested_field_get(row, "line", BLOBMSG_TYPE_INT32);
		CHECK(field && blobmsg_get_u32(field) == line);
		if (!strcmp(key, "streams") || !strcmp(key, "jb")) {
			field = nested_field_get(row, "stream",
						 BLOBMSG_TYPE_INT32);
			CHECK(field && blobmsg_get_u32(field) == stream++);
		}
	}
}

static void test_status_line_rows(void)
{
	static const char *const keys[] = { "streams", "jb", "lec", "monitor" };
	static const char *const names[] = {
		"line rows: streams contain only the selected line",
		"line rows: jb contains only the selected line",
		"line rows: lec contains only the selected line",
		"line rows: monitor contains only the selected line",
	};
	struct blob_buf out = {};
	unsigned int i, line;

	state_reset();
	uv.hal = &fake_hal;
	stream_stats_live = true;
	for (i = 0; i < ARRAY_SIZE(keys); i++) {
		CASE(names[i]);
		for (line = 0; line < fake_cap.lines; line++) {
			blob_buf_init(&out, 0);
			status_line_rows_dump(&out, line);
			line_rows_check(&out, keys[i], line, i < 2 ? 2 : 1);
		}
	}

	CASE("line rows: an index outside the cap emits empty arrays");
	blob_buf_init(&out, 0);
	status_line_rows_dump(&out, UINT_MAX);
	for (i = 0; i < ARRAY_SIZE(keys); i++)
		line_rows_check(&out, keys[i], UINT_MAX, 0);

	CASE("line rows: no HAL emits empty arrays");
	uv.hal = NULL;
	blob_buf_init(&out, 0);
	status_line_rows_dump(&out, 0);
	for (i = 0; i < ARRAY_SIZE(keys); i++)
		line_rows_check(&out, keys[i], 0, 0);

	stream_stats_live = false;
	blob_buf_free(&out);
}

static void diag_row_check(struct blob_attr *row, unsigned int line,
			   unsigned int stream)
{
	struct blob_attr *sub;
	unsigned int i;

	CHECK(row != NULL);
	CHECK_UINT(table_len(row), 2 + ARRAY_SIZE(diag_keys));
	sub = attr_at(row, 0);
	CHECK(sub && blobmsg_type(sub) == BLOBMSG_TYPE_INT32 &&
	      !strcmp(blobmsg_name(sub), "line") &&
	      blobmsg_get_u32(sub) == line);
	sub = attr_at(row, 1);
	CHECK(sub && blobmsg_type(sub) == BLOBMSG_TYPE_INT32 &&
	      !strcmp(blobmsg_name(sub), "stream") &&
	      blobmsg_get_u32(sub) == stream);
	for (i = 0; i < ARRAY_SIZE(diag_keys); i++) {
		sub = attr_at(row, 2 + i);
		CHECK(sub && blobmsg_type(sub) == BLOBMSG_TYPE_INT64 &&
		      !strcmp(blobmsg_name(sub), diag_keys[i]) &&
		      blobmsg_get_u64(sub) == diag_value(line, stream, i));
	}
}

static struct blob_attr *diag_streams_get(struct blob_buf *buf, int want_line)
{
	struct blob_attr *diag;

	blob_buf_init(buf, 0);
	status_diag_dump(buf, want_line);
	diag = field_get(buf->head, "diag", BLOBMSG_TYPE_TABLE);

	return nested_field_get(diag, "streams", BLOBMSG_TYPE_ARRAY);
}

/*
 * Each case reads the diag table through the stream diagnostic op of a fake
 * backend. That op refuses one row, so each case also shows that a refused
 * row stays out.
 */
static void test_status_diag_rows(void)
{
	struct ubus_request_data request = {};
	struct blob_attr *diag, *streams;
	struct blob_buf out = {};
	struct cfg cfg = {};

	state_reset();
	uv.hal = &fake_hal;

	CASE("diag: a backend with no stream diag op gives no diag table");
	test_backend_ops.stream_diag_get = NULL;
	blob_buf_init(&out, 0);
	status_diag_dump(&out, -1);
	CHECK(field_get(out.head, "diag", BLOBMSG_TYPE_TABLE) == NULL);
	CHECK_UINT(blob_len(out.head), 0);
	test_backend_ops.stream_diag_get = fake_stream_diag_get;

	CASE("diag: an op that refuses every row gives an empty streams array");
	streams = diag_streams_get(&out, -1);
	CHECK(streams != NULL);
	CHECK_UINT(array_len(streams), 0);

	CASE("diag: a backend with no engine op gives no engine table");
	diag = field_get(out.head, "diag", BLOBMSG_TYPE_TABLE);
	CHECK(diag != NULL);
	CHECK_UINT(table_len(diag), 1);
	CHECK(nested_field_get(diag, "engine", BLOBMSG_TYPE_TABLE) == NULL);

	diag_live = true;
	CASE("diag: all lines give the rows in line order, then stream order");
	streams = diag_streams_get(&out, -1);
	CHECK_UINT(array_len(streams), 3);
	diag_row_check(attr_at(streams, 0), 0, 0);
	diag_row_check(attr_at(streams, 1), 0, 1);
	diag_row_check(attr_at(streams, 2), 1, 1);

	CASE("diag: one line gives the rows of that line alone");
	streams = diag_streams_get(&out, 0);
	CHECK_UINT(array_len(streams), 2);
	diag_row_check(attr_at(streams, 0), 0, 0);
	diag_row_check(attr_at(streams, 1), 0, 1);
	streams = diag_streams_get(&out, 1);
	CHECK_UINT(array_len(streams), 1);
	diag_row_check(attr_at(streams, 0), 1, 1);

	CASE("diag: a line outside the cap gives no rows");
	streams = diag_streams_get(&out, 2);
	CHECK(streams != NULL);
	CHECK_UINT(array_len(streams), 0);

	CASE("diag: a cap with zero streams gives no rows");
	fake_cap.streams_per_chan = 0;
	streams = diag_streams_get(&out, -1);
	CHECK(streams != NULL);
	CHECK_UINT(array_len(streams), 0);
	fake_cap.streams_per_chan = 2;

	CASE("diag: no hal gives no rows, and the table still stands");
	uv.hal = NULL;
	streams = diag_streams_get(&out, -1);
	CHECK(streams != NULL);
	CHECK_UINT(array_len(streams), 0);

	CASE("diag: voice status carries the diag table of all lines");
	uv.hal = &fake_hal;
	uv.cfg = &cfg;
	CHECK_UINT(voice_status(&request_context, NULL, &request, "status",
				NULL), 0);
	diag = field_get(reply, "diag", BLOBMSG_TYPE_TABLE);
	streams = nested_field_get(diag, "streams", BLOBMSG_TYPE_ARRAY);
	CHECK_UINT(array_len(streams), 3);

	uv.cfg = NULL;
	uv.hal = NULL;
	diag_live = false;
	blob_buf_free(&out);
}

/* ------------------------------------------------------- voice.calltrace */

static void calltrace_reset(void)
{
	state_reset();
	calltrace_arms = 0;
	calltrace_arm_line = 0;
	memset(&calltrace_arm_opt, 0, sizeof(calltrace_arm_opt));
	calltrace_arm_result = 0;
	calltrace_cancel_result = 0;
	calltrace_state_word = "idle";
	reply = NULL;
}

static int calltrace_arm_call(struct blob_attr *args)
{
	struct ubus_request_data request = {};

	return voice_calltrace_arm(&request_context, NULL, &request, "arm",
				   args);
}

static void test_calltrace_verbs(void)
{
	struct blob_attr *field;
	struct blob_buf args = {};

	CASE("an arm with no line is a bad argument and reaches no controller");
	calltrace_reset();
	blob_buf_init(&args, 0);
	CHECK_UINT(calltrace_arm_call(args.head),
		   UBUS_STATUS_INVALID_ARGUMENT);
	CHECK_UINT(calltrace_arms, 0);
	CHECK(reply == NULL);

	/*
	 * The controller owns the line range, and it records the word the
	 * reply carries. A bound checked here would answer the same status
	 * with no reason behind it, so every index goes down.
	 */
	CASE("a line the board cannot carry still reaches the controller");
	calltrace_reset();
	calltrace_arm_result = -EINVAL;
	blob_buf_init(&args, 0);
	blobmsg_add_u32(&args, "line", CFG_CHANNEL_MAX);
	CHECK_UINT(calltrace_arm_call(args.head),
		   UBUS_STATUS_INVALID_ARGUMENT);
	CHECK_UINT(calltrace_arms, 1);
	CHECK_UINT(calltrace_arm_line, CFG_CHANNEL_MAX);

	CASE("an arm naming the line alone fills the shipped defaults");
	calltrace_reset();
	blob_buf_init(&args, 0);
	blobmsg_add_u32(&args, "line", 1);
	CHECK_UINT(calltrace_arm_call(args.head), 0);
	CHECK_UINT(calltrace_arms, 1);
	CHECK_UINT(calltrace_arm_line, 1);
	CHECK_UINT(calltrace_arm_opt.timeout_s,
		   CALLTRACE_TIMEOUT_DEFAULT_S);
	CHECK_UINT(calltrace_arm_opt.grace_s, CALLTRACE_GRACE_DEFAULT_S);
	CHECK_UINT(calltrace_arm_opt.arm_timeout_s,
		   CALLTRACE_ARM_TIMEOUT_DEF_S);
	CHECK_UINT(calltrace_arm_opt.wire_max_mb,
		   CALLTRACE_WIRE_MAX_MB_DEF);
	CHECK_UINT(calltrace_arm_opt.rings_max_mb,
		   CALLTRACE_RINGS_MAX_MB_DEF);
	CHECK_UINT(calltrace_arm_opt.wire, 1);

	/* The canceller capture records the near-end room of every line in a
	 * call, so an arm that does not name it captures none. */
	CASE("and the canceller capture stays off with its cap at the default");
	CHECK_UINT(calltrace_arm_opt.lec, 0);
	CHECK_UINT(calltrace_arm_opt.lec_max_mb, CALLTRACE_LEC_MAX_MB_DEF);
	CHECK_UINT(CALLTRACE_LEC_MAX_MB_DEF, 32);

	CASE("and the arm answers the status reply");
	field = field_get(reply, "state", BLOBMSG_TYPE_STRING);
	CHECK(field && !strcmp(blobmsg_get_string(field), "idle"));

	CASE("a named option overrides its default and the rest stay shipped");
	calltrace_reset();
	blob_buf_init(&args, 0);
	blobmsg_add_u32(&args, "line", 0);
	blobmsg_add_u8(&args, "wire", 0);
	blobmsg_add_u32(&args, "timeout", 45);
	CHECK_UINT(calltrace_arm_call(args.head), 0);
	CHECK_UINT(calltrace_arm_opt.wire, 0);
	CHECK_UINT(calltrace_arm_opt.timeout_s, 45);
	CHECK_UINT(calltrace_arm_opt.grace_s, CALLTRACE_GRACE_DEFAULT_S);
	CHECK_UINT(calltrace_arm_opt.wire_max_mb,
		   CALLTRACE_WIRE_MAX_MB_DEF);

	CASE("the lec option and its cap reach the controller as named");
	calltrace_reset();
	blob_buf_init(&args, 0);
	blobmsg_add_u32(&args, "line", 0);
	blobmsg_add_u8(&args, "lec", 1);
	blobmsg_add_u32(&args, "lec_max_mb", 4);
	CHECK_UINT(calltrace_arm_call(args.head), 0);
	CHECK_UINT(calltrace_arm_opt.lec, 1);
	CHECK_UINT(calltrace_arm_opt.lec_max_mb, 4);
	CHECK_UINT(calltrace_arm_opt.rings_max_mb,
		   CALLTRACE_RINGS_MAX_MB_DEF);
	CHECK_UINT(calltrace_arm_opt.wire, 1);

	CASE("and the cap alone takes its value with the option still off");
	calltrace_reset();
	blob_buf_init(&args, 0);
	blobmsg_add_u32(&args, "line", 0);
	blobmsg_add_u32(&args, "lec_max_mb", 0);
	CHECK_UINT(calltrace_arm_call(args.head), 0);
	CHECK_UINT(calltrace_arm_opt.lec, 0);
	CHECK_UINT(calltrace_arm_opt.lec_max_mb, 0);

	CASE("a bad range answers Invalid argument");
	calltrace_reset();
	calltrace_arm_result = -EINVAL;
	blob_buf_init(&args, 0);
	blobmsg_add_u32(&args, "line", 0);
	CHECK_UINT(calltrace_arm_call(args.head),
		   UBUS_STATUS_INVALID_ARGUMENT);
	CHECK(reply == NULL);

	CASE("a second arm shares the answer the busy pair gives");
	calltrace_reset();
	calltrace_arm_result = -EBUSY;
	CHECK_UINT(calltrace_arm_call(args.head), UBUS_STATUS_NO_DATA);

	CASE("every other preflight refusal is an unknown error");
	calltrace_reset();
	calltrace_arm_result = -ENOSPC;
	CHECK_UINT(calltrace_arm_call(args.head), UBUS_STATUS_UNKNOWN_ERROR);
	calltrace_reset();
	calltrace_arm_result = -ENOSYS;
	CHECK_UINT(calltrace_arm_call(args.head), UBUS_STATUS_UNKNOWN_ERROR);
	calltrace_reset();
	calltrace_arm_result = -ENOENT;
	CHECK_UINT(calltrace_arm_call(args.head), UBUS_STATUS_UNKNOWN_ERROR);

	CASE("a cancel with no trace answers Not found");
	calltrace_reset();
	calltrace_cancel_result = -ENOENT;
	{
		struct ubus_request_data request = {};

		CHECK_UINT(voice_calltrace_cancel(&request_context, NULL,
						  &request, "cancel", NULL),
			   UBUS_STATUS_NOT_FOUND);
	}
	CHECK(reply == NULL);

	CASE("a cancel that took answers the status reply");
	calltrace_reset();
	calltrace_state_word = "finishing";
	{
		struct ubus_request_data request = {};

		CHECK_UINT(voice_calltrace_cancel(&request_context, NULL,
						  &request, "cancel", NULL), 0);
	}
	field = field_get(reply, "state", BLOBMSG_TYPE_STRING);
	CHECK(field && !strcmp(blobmsg_get_string(field), "finishing"));

	CASE("status answers the same reply and takes no argument");
	calltrace_reset();
	calltrace_state_word = "running";
	{
		struct ubus_request_data request = {};

		CHECK_UINT(voice_calltrace_status(&request_context, NULL,
						  &request, "status", NULL), 0);
	}
	field = field_get(reply, "state", BLOBMSG_TYPE_STRING);
	CHECK(field && !strcmp(blobmsg_get_string(field), "running"));

	blob_buf_free(&args);
	calltrace_reset();
}

int main(void)
{
	uloop_init();
	test_method_and_refusals();
	test_deferred_replies();
	test_loop_test_verb();
	test_method_table();
	test_reload_verb();
	test_stats_live_stream();
	test_account_verbs();
	test_hal_feature_names();
	test_status_config_generation();
	test_status_signaling();
	test_hal_rt_table();
	test_status_diagnostics();
	test_status_line_rows();
	test_status_diag_rows();
	test_monitor_grid_mappings();
	test_jb_row_wire();
	test_lec_row_wire();
	test_monitor_row_wire();
	test_decoder_records();
	test_decoder_failures();
	test_calltrace_verbs();
	state_reset();
	blob_buf_free(&b);
	uloop_done();

	return tu_summary("test_ubus");
}
