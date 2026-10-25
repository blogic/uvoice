// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <string.h>

#include <libubus.h>

#include "uvoice.h"
#include "calltrace.h"
#include "cdr.h"
#include "line/line.h"
#include "net/iface.h"
#include "proto/proto.h"
#include "status.h"

/*
 * How long a deferred media reply may wait. Most HAL jobs use the 30 s default
 * (HAL_JOB_TIMEOUT_MS). A media query is SLOW only to leave room for a
 * backend that must query the device (hal.h section 1), and no backend needs
 * seconds for it, so a caller that waited the default out would wait on a
 * fault. Five seconds is far above the query and far below the 30 s ubus
 * client timeout. This daemon can then report a query that never completes.
 */
#define STATS_LIVE_TIMEOUT_MS	5000
/* The line test budget owns the normal timeout. This later timer closes the
 * ubus request if the HAL callback path fails to deliver its answer. */
#define LINE_TEST_TIMEOUT_MS	(LINE_TEST_JOB_MS + 1000)

enum {
	MLT_VERSION = 1,
	MLT_HEAD_SIZE = 8,
	MLT_GR909_SIZE = 52,
	MLT_ROH_SIZE = 12,
	MLT_CAPACITANCE_SIZE = 20,
	MLT_LOOP_SIZE = 20,
	MLT_VERSION_OFFSET = 0,
	MLT_TEST_OFFSET = 1,
	MLT_STATUS_OFFSET = 2,
	MLT_VALID_OFFSET = 4,
	MLT_VDC_TR_OFFSET = 8,
	MLT_VDC_TG_OFFSET = 12,
	MLT_VDC_RG_OFFSET = 16,
	MLT_VAC_TR_OFFSET = 20,
	MLT_VAC_TG_OFFSET = 24,
	MLT_VAC_RG_OFFSET = 28,
	MLT_R_TR_OFFSET = 32,
	MLT_R_TG_OFFSET = 36,
	MLT_R_RG_OFFSET = 40,
	MLT_GR909_ROH_OFFHOOK_OFFSET = 44,
	MLT_GR909_ROH_FAULT_OFFSET = 45,
	MLT_REN_OFFSET = 48,
	MLT_ROH_OFFHOOK_OFFSET = 8,
	MLT_ROH_FAULT_OFFSET = 9,
	MLT_LOOP_R_TR_OFFSET = 8,
	MLT_LOOP_R_TG_OFFSET = 12,
	MLT_LOOP_R_RG_OFFSET = 16,
	MLT_C_TR_OFFSET = 8,
	MLT_C_TG_OFFSET = 12,
	MLT_C_RG_OFFSET = 16,
};

enum {
	MLT_VALID_VDC_TR = 1u << 0,
	MLT_VALID_VDC_TG = 1u << 1,
	MLT_VALID_VDC_RG = 1u << 2,
	MLT_VALID_VAC_TR = 1u << 3,
	MLT_VALID_VAC_TG = 1u << 4,
	MLT_VALID_VAC_RG = 1u << 5,
	MLT_VALID_R_TR = 1u << 6,
	MLT_VALID_R_TG = 1u << 7,
	MLT_VALID_R_RG = 1u << 8,
	MLT_VALID_ROH_OFFHOOK = 1u << 9,
	MLT_VALID_ROH_FAULT = 1u << 10,
	MLT_VALID_REN = 1u << 11,
	MLT_VALID_C_TR = 1u << 12,
	MLT_VALID_C_TG = 1u << 13,
	MLT_VALID_C_RG = 1u << 14,
};

#define MLT_OHMS_OPEN	UINT32_MAX

/* An OHMS value is a U32. Its all-ones value reads as the word "open". */
enum mlt_kind {
	MLT_KIND_U32,
	MLT_KIND_U8,
	MLT_KIND_OHMS,
};

struct mlt_field {
	enum hal_line_test test;
	const char *key;
	uint8_t offset;
	uint32_t valid;
	enum mlt_kind kind;
};

#define MLT_FIELD(test, key, offset, valid, kind) \
	{ HAL_LINE_TEST_##test, key, MLT_##offset##_OFFSET, \
	  MLT_VALID_##valid, MLT_KIND_##kind }

static const struct mlt_field mlt_fields[] = {
	/* The three DC voltages are signed. They travel as their raw bits in
	 * the INT32 slot, which the json renderer prints signed. */
	MLT_FIELD(GR909, "vdc_tr_mv", VDC_TR, VDC_TR, U32),
	MLT_FIELD(GR909, "vdc_tg_mv", VDC_TG, VDC_TG, U32),
	MLT_FIELD(GR909, "vdc_rg_mv", VDC_RG, VDC_RG, U32),
	MLT_FIELD(GR909, "vac_tr_mvrms", VAC_TR, VAC_TR, U32),
	MLT_FIELD(GR909, "vac_tg_mvrms", VAC_TG, VAC_TG, U32),
	MLT_FIELD(GR909, "vac_rg_mvrms", VAC_RG, VAC_RG, U32),
	MLT_FIELD(GR909, "r_tr_ohm", R_TR, R_TR, OHMS),
	MLT_FIELD(GR909, "r_tg_ohm", R_TG, R_TG, OHMS),
	MLT_FIELD(GR909, "r_rg_ohm", R_RG, R_RG, OHMS),
	MLT_FIELD(GR909, "roh_offhook", GR909_ROH_OFFHOOK, ROH_OFFHOOK, U8),
	MLT_FIELD(GR909, "roh_res_fault", GR909_ROH_FAULT, ROH_FAULT, U8),
	MLT_FIELD(GR909, "ren_milli", REN, REN, U32),
	MLT_FIELD(ROH, "roh_offhook", ROH_OFFHOOK, ROH_OFFHOOK, U8),
	MLT_FIELD(ROH, "roh_res_fault", ROH_FAULT, ROH_FAULT, U8),
	MLT_FIELD(CAPACITANCE, "c_tr_nf", C_TR, C_TR, U32),
	MLT_FIELD(CAPACITANCE, "c_tg_nf", C_TG, C_TG, U32),
	MLT_FIELD(CAPACITANCE, "c_rg_nf", C_RG, C_RG, U32),
	MLT_FIELD(LOOP, "r_tr_ohm", LOOP_R_TR, R_TR, OHMS),
	MLT_FIELD(LOOP, "r_tg_ohm", LOOP_R_TG, R_TG, OHMS),
	MLT_FIELD(LOOP, "r_rg_ohm", LOOP_R_RG, R_RG, OHMS),
};

#undef MLT_FIELD

static const uint8_t mlt_sizes[] = {
	[HAL_LINE_TEST_GR909] = MLT_GR909_SIZE,
	[HAL_LINE_TEST_ROH] = MLT_ROH_SIZE,
	[HAL_LINE_TEST_CAPACITANCE] = MLT_CAPACITANCE_SIZE,
	[HAL_LINE_TEST_LOOP] = MLT_LOOP_SIZE,
};

static struct ubus_auto_conn conn;
static bool ubus_connected;
static struct blob_buf b;

static int voice_status(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	const struct proto_ops *ops = proto_ops_get();
	void *table;

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "version", UVOICE_VERSION);
	/* The counter the call trace snapshots carry: a reader that sees it
	 * move knows a reload took between two replies. */
	blobmsg_add_u32(&b, "config_generation", uv.config_generation);

	table = blobmsg_open_table(&b, "config");
	blobmsg_add_u8(&b, "loaded", uv.cfg->loaded);
	blobmsg_add_u8(&b, "enabled", uv.cfg->enabled);
	blobmsg_add_u32(&b, "channels", uv.cfg->channels);
	blobmsg_close_table(&b, table);

	status_hal_dump(&b);
	lines_region_status_dump(&b);

	/*
	 * The protocol's own media counters, outside the hal table because
	 * they survive a backend that never attached (status_hal_dump returns
	 * early on one) and because the protocol, not the HAL, decides them.
	 * The schema requires the key, so a protocol with no counters of its
	 * own reports 0.
	 */
	table = blobmsg_open_table(&b, "media");
	if (ops->status_dump_media)
		ops->status_dump_media(&b);
	else
		blobmsg_add_u32(&b, "opus_dtmf_fallbacks", 0);
	blobmsg_close_table(&b, table);

	status_streams_dump(&b);
	status_jb_dump(&b);
	status_lec_dump(&b);
	status_monitor_dump(&b);
	status_diag_dump(&b, -1);

	proto_status_dump(&b);
	ops->status_dump_lines(&b, -1);
	net_iface_status_dump(&b);
	lines_status_dump(&b);
	if (ops->status_dump_top)
		ops->status_dump_top(&b);
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

enum {
	LINE_ARG,
	__LINE_MAX,
};

static const struct blobmsg_policy line_policy[__LINE_MAX] = {
	[LINE_ARG] = { "line", BLOBMSG_TYPE_INT32 },
};

static bool line_arg_get(struct blob_attr *msg, uint32_t *line)
{
	struct blob_attr *tb[__LINE_MAX] = {};

	if (msg)
		blobmsg_parse(line_policy, __LINE_MAX, tb, blob_data(msg),
			      blob_len(msg));
	if (!tb[LINE_ARG])
		return false;

	*line = blobmsg_get_u32(tb[LINE_ARG]);

	return true;
}

/* The command verbs key the answer "requested", the inject verbs "injected". */
static void reply_line_open(uint32_t line, const char *key, const char *verb)
{
	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "line", line);
	blobmsg_add_string(&b, key, verb);
}

/*
 * The command verbs of a protocol (proto/proto.h ubus_request_reply) answer
 * with the line and what was asked for. "requested" and not "injected": nothing
 * is injected here, the daemon accepted a command whose outcome voice status
 * reports.
 */
void uvoice_ubus_request_reply(struct ubus_context *ctx,
			       struct ubus_request_data *req, unsigned int line,
			       const char *verb)
{
	reply_line_open(line, "requested", verb);
	ubus_send_reply(ctx, req, b.head);
}

/*
 * The three diagnostic verbs, the daemon surface the klish voice commands
 * drive. Each one injects the hook or digit events the DSP would have
 * reported and answers what it injected. line/line.h carries the semantics,
 * the refusal set and the hook desync a synthetic seize deliberately leaves
 * behind; the handlers here marshal and nothing else, which is the shape of
 * the command verbs of a protocol.
 *
 * They live on voice and not on voice.stub. The stub object is a test backend
 * that only the CI build carries; these verbs are the production surface and
 * exist on hardware.
 */
enum {
	DIAL_LINE,
	DIAL_NUMBER,
	__DIAL_MAX,
};

static const struct blobmsg_policy dial_policy[__DIAL_MAX] = {
	[DIAL_LINE] = { "line", BLOBMSG_TYPE_INT32 },
	[DIAL_NUMBER] = { "number", BLOBMSG_TYPE_STRING },
};

/*
 * A port this board does not carry and a number nobody can dial are both bad
 * arguments; lines that are not up yet are the register precedent instead,
 * because the daemon has no line object to name.
 */
static int inject_status(enum line_inject st)
{
	switch (st) {
	case LINE_INJECT_OK:
		return 0;
	case LINE_INJECT_NO_LINES:
		return UBUS_STATUS_NOT_FOUND;
	case LINE_INJECT_NO_LINE:
	case LINE_INJECT_BAD_NUMBER:
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	return UBUS_STATUS_UNKNOWN_ERROR;
}

static void inject_reply_open(uint32_t line, const char *injected)
{
	reply_line_open(line, "injected", injected);
}

static int voice_seize(struct ubus_context *ctx, struct ubus_object *obj,
		       struct ubus_request_data *req, const char *method,
		       struct blob_attr *msg)
{
	enum line_inject st;
	uint32_t line;

	if (!line_arg_get(msg, &line))
		return UBUS_STATUS_INVALID_ARGUMENT;
	st = line_inject_seize(line);
	if (st != LINE_INJECT_OK)
		return inject_status(st);

	inject_reply_open(line, "seize");
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static int voice_release(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method,
			 struct blob_attr *msg)
{
	enum line_inject st;
	uint32_t line;

	if (!line_arg_get(msg, &line))
		return UBUS_STATUS_INVALID_ARGUMENT;
	st = line_inject_release(line);
	if (st != LINE_INJECT_OK)
		return inject_status(st);

	inject_reply_open(line, "release");
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static int voice_dial(struct ubus_context *ctx, struct ubus_object *obj,
		      struct ubus_request_data *req, const char *method,
		      struct blob_attr *msg)
{
	struct blob_attr *tb[__DIAL_MAX] = {};
	unsigned int digits = 0;
	enum line_inject st;
	uint32_t line;

	if (msg)
		blobmsg_parse(dial_policy, __DIAL_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[DIAL_LINE] || !tb[DIAL_NUMBER])
		return UBUS_STATUS_INVALID_ARGUMENT;

	line = blobmsg_get_u32(tb[DIAL_LINE]);
	st = line_inject_dial(line, blobmsg_get_string(tb[DIAL_NUMBER]), &digits);
	if (st != LINE_INJECT_OK)
		return inject_status(st);

	inject_reply_open(line, "dial");
	/* how many digit events the verb produced. The seize is not one of
	 * them, and it is not reported: whether it was made is a property of
	 * the state the line was already in, which voice status answers */
	blobmsg_add_u32(&b, "digits", digits);
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static int voice_reload(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	int ret = uvoice_reload();

	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "enabled", uv.cfg->enabled);
	blobmsg_add_u32(&b, "channels", uv.cfg->channels);
	ubus_send_reply(ctx, req, b.head);

	if (ret == UVOICE_RELOAD_RESTART)
		return UBUS_STATUS_NOT_SUPPORTED;

	return ret ? UBUS_STATUS_UNKNOWN_ERROR : 0;
}

static int voice_dump_config(struct ubus_context *ctx, struct ubus_object *obj,
			     struct ubus_request_data *req, const char *method,
			     struct blob_attr *msg)
{
	blob_buf_init(&b, 0);
	cfg_dump(uv.cfg, &b);
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

/*
 * The three call record methods (call-records). All of them answer from daemon
 * state alone; the live per-call query that needs a slow HAL job and a
 * deferred reply is "stats_live" below.
 */
static int voice_cdr(struct ubus_context *ctx, struct ubus_object *obj,
		     struct ubus_request_data *req, const char *method,
		     struct blob_attr *msg)
{
	blob_buf_init(&b, 0);
	cdr_dump(&b);
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static int voice_stats(struct ubus_context *ctx, struct ubus_object *obj,
		       struct ubus_request_data *req, const char *method,
		       struct blob_attr *msg)
{
	struct blob_attr *tb[__LINE_MAX] = {};
	int want_line = -1;

	if (msg)
		blobmsg_parse(line_policy, __LINE_MAX, tb, blob_data(msg), blob_len(msg));
	if (tb[LINE_ARG]) {
		uint32_t line = blobmsg_get_u32(tb[LINE_ARG]);

		if (line >= CFG_CHANNEL_MAX)
			return UBUS_STATUS_INVALID_ARGUMENT;
		want_line = (int)line;
	}

	blob_buf_init(&b, 0);
	cdr_stats_dump(&b, want_line);
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static int voice_stats_reset(struct ubus_context *ctx, struct ubus_object *obj,
			     struct ubus_request_data *req, const char *method,
			     struct blob_attr *msg)
{
	blob_buf_init(&b, 0);
	cdr_stats_reset(&b);
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

enum line_test_decode_status {
	LINE_TEST_DECODE_COMPLETE = 0,
	LINE_TEST_DECODE_FAILED,
	LINE_TEST_DECODE_MALFORMED,
};

static uint32_t mlt_u32_get(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
	       (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void mlt_field_add(const struct mlt_field *f, const uint8_t *record)
{
	uint32_t value;

	if (f->kind == MLT_KIND_U8) {
		blobmsg_add_u8(&b, f->key, record[f->offset]);
		return;
	}

	value = mlt_u32_get(&record[f->offset]);
	if (f->kind == MLT_KIND_OHMS && value == MLT_OHMS_OPEN)
		blobmsg_add_string(&b, f->key, "open");
	else
		blobmsg_add_u32(&b, f->key, value);
}

/*
 * This decoder mirrors version 1 of mlt_record.h from the ProSLIC plug-in.
 * That header remains the source of truth for the byte layout and valid bits.
 */
static enum line_test_decode_status
line_test_record_dump(enum hal_line_test requested, const uint8_t *record,
		      size_t len, uint8_t *code)
{
	const struct mlt_field *f;
	uint32_t expected = 0;
	uint32_t valid = 0;
	size_t width;
	void *table;

	table = blobmsg_open_table(&b, "record");
	if (len > MLT_VERSION_OFFSET)
		blobmsg_add_u32(&b, "version", record[MLT_VERSION_OFFSET]);
	if (len > MLT_TEST_OFFSET)
		blobmsg_add_u32(&b, "test_id", record[MLT_TEST_OFFSET]);
	if (len > MLT_STATUS_OFFSET) {
		*code = record[MLT_STATUS_OFFSET];
		blobmsg_add_u32(&b, "code", *code);
	}

	if (len < MLT_HEAD_SIZE || len > LINE_TEST_RECORD_MAX)
		goto malformed;

	valid = mlt_u32_get(&record[MLT_VALID_OFFSET]);
	blobmsg_add_u32(&b, "valid", valid);
	if (record[MLT_VERSION_OFFSET] != MLT_VERSION ||
	    record[MLT_TEST_OFFSET] != requested)
		goto malformed;

	if ((size_t)requested < ARRAY_SIZE(mlt_sizes))
		expected = mlt_sizes[requested];
	if (!*code && len < expected)
		goto malformed;

	for (f = mlt_fields; f < mlt_fields + ARRAY_SIZE(mlt_fields); f++) {
		width = f->kind == MLT_KIND_U8 ? 1 : sizeof(uint32_t);
		if (f->test == requested && (valid & f->valid) &&
		    len >= f->offset + width)
			mlt_field_add(f, record);
	}

	blobmsg_close_table(&b, table);
	return *code ? LINE_TEST_DECODE_FAILED : LINE_TEST_DECODE_COMPLETE;

malformed:
	blobmsg_close_table(&b, table);
	return LINE_TEST_DECODE_MALFORMED;
}

static const char * const line_test_names[] = {
	[HAL_LINE_TEST_GR909] = "gr909",
	[HAL_LINE_TEST_ROH] = "roh",
	[HAL_LINE_TEST_CAPACITANCE] = "capacitance",
	[HAL_LINE_TEST_LOOP] = "loop",
};

/* A reply that waits for a HAL answer: the part that the line test and the
 * live stats requests share. */
struct deferred_reply {
	struct ubus_request_data req;
	struct uloop_timeout timeout;
	struct ubus_context *ctx;
	unsigned int line;
	bool pending;
};

static void deferred_reply_finish(struct deferred_reply *reply, int status)
{
	reply->pending = false;
	uloop_timeout_cancel(&reply->timeout);
	ubus_complete_deferred_request(reply->ctx, &reply->req, status);
}

struct line_test_request {
	struct deferred_reply reply;
	enum hal_line_test test;
};

static struct line_test_request line_test_reqs[CFG_CHANNEL_MAX];

static void line_test_request_done(void *priv,
			   enum line_test_result_status status,
			   const void *record, size_t len)
{
	struct line_test_request *request = priv;
	struct deferred_reply *reply = &request->reply;
	enum line_test_decode_status decoded;
	uint8_t code = 0;

	if (!reply->pending) {
		TRACE(TRACE_HAL,
		      "line %u test %s result arrived after its reply closed",
		      reply->line, line_test_names[request->test]);
		return;
	}
	if (status == LINE_TEST_RESULT_TIMEOUT) {
		deferred_reply_finish(reply, UBUS_STATUS_TIMEOUT);
		return;
	}
	if (status == LINE_TEST_RESULT_FATAL ||
	    status == LINE_TEST_RESULT_UNREADABLE) {
		deferred_reply_finish(reply, UBUS_STATUS_UNKNOWN_ERROR);
		return;
	}
	if (!record) {
		TRACE(TRACE_HAL,
		      "line %u test %s completed without a record: %s",
		      reply->line, line_test_names[request->test],
		      status == LINE_TEST_RESULT_UNSUPPORTED ? "unsupported" :
		      "the line became busy");
		deferred_reply_finish(reply, UBUS_STATUS_NO_DATA);
		return;
	}

	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "line", reply->line);
	blobmsg_add_string(&b, "test", line_test_names[request->test]);
	decoded = line_test_record_dump(request->test, record, len, &code);
	if (decoded == LINE_TEST_DECODE_MALFORMED) {
		blobmsg_add_string(&b, "status", "malformed");
		blobmsg_add_u32(&b, "length", len);
	} else if (status == LINE_TEST_RESULT_FAILED ||
		   decoded == LINE_TEST_DECODE_FAILED) {
		blobmsg_add_string(&b, "status", "failed");
		blobmsg_add_u32(&b, "code", code);
	} else {
		blobmsg_add_string(&b, "status", "complete");
	}
	ubus_send_reply(reply->ctx, &reply->req, b.head);
	deferred_reply_finish(reply, 0);
}

static void line_test_request_expire(struct uloop_timeout *timeout)
{
	struct line_test_request *request = container_of(
		timeout, struct line_test_request, reply.timeout);

	ULOG_WARN("line %u test %s produced no answer in %u ms\n",
		  request->reply.line, line_test_names[request->test],
		  LINE_TEST_TIMEOUT_MS);
	line_test_drop(request->reply.line);
	deferred_reply_finish(&request->reply, UBUS_STATUS_TIMEOUT);
}

enum {
	LINE_TEST_LINE,
	LINE_TEST_NAME,
	__LINE_TEST_MAX,
};

static const struct blobmsg_policy line_test_policy[__LINE_TEST_MAX] = {
	[LINE_TEST_LINE] = { "line", BLOBMSG_TYPE_INT32 },
	[LINE_TEST_NAME] = { "test", BLOBMSG_TYPE_STRING },
};

static bool line_test_name_get(const char *name, enum hal_line_test *test)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(line_test_names); i++) {
		if (strcmp(name, line_test_names[i]))
			continue;
		*test = (enum hal_line_test)i;
		return true;
	}

	return false;
}

static int voice_line_test(struct ubus_context *ctx, struct ubus_object *obj,
			   struct ubus_request_data *req, const char *method,
			   struct blob_attr *msg)
{
	struct blob_attr *tb[__LINE_TEST_MAX] = {};
	struct line_test_request *request;
	enum line_test_query_status status;
	enum hal_line_test test;
	uint32_t line;

	if (msg)
		blobmsg_parse(line_test_policy, __LINE_TEST_MAX, tb,
			      blob_data(msg), blob_len(msg));
	if (!tb[LINE_TEST_LINE] || !tb[LINE_TEST_NAME] ||
	    !line_test_name_get(blobmsg_get_string(tb[LINE_TEST_NAME]), &test))
		return UBUS_STATUS_INVALID_ARGUMENT;

	line = blobmsg_get_u32(tb[LINE_TEST_LINE]);
	if (line >= CFG_CHANNEL_MAX)
		return UBUS_STATUS_INVALID_ARGUMENT;

	request = &line_test_reqs[line];
	if (request->reply.pending) {
		TRACE(TRACE_HAL,
		      "line %u test refused: a reply is already deferred",
		      line);
		return UBUS_STATUS_NO_DATA;
	}

	request->reply.ctx = ctx;
	request->reply.line = line;
	request->test = test;
	status = line_test_query(line, test, line_test_request_done, request);
	switch (status) {
	case LINE_TEST_QUERY_OK:
		break;
	case LINE_TEST_QUERY_BAD_ARGUMENT:
		return UBUS_STATUS_INVALID_ARGUMENT;
	case LINE_TEST_QUERY_NO_LINE:
		return UBUS_STATUS_NOT_FOUND;
	case LINE_TEST_QUERY_NOT_UP:
		TRACE(TRACE_HAL,
		      "line %u test refused: the line is not up", line);
		return UBUS_STATUS_NO_DATA;
	case LINE_TEST_QUERY_PENDING:
		TRACE(TRACE_HAL,
		      "line %u test refused: a query is pending", line);
		return UBUS_STATUS_NO_DATA;
	case LINE_TEST_QUERY_CALL_ACTIVE:
		TRACE(TRACE_HAL,
		      "line %u test refused: a call is active", line);
		return UBUS_STATUS_NO_DATA;
	case LINE_TEST_QUERY_BUSY:
		TRACE(TRACE_HAL,
		      "line %u test refused: the line or library is busy",
		      line);
		return UBUS_STATUS_NO_DATA;
	case LINE_TEST_QUERY_UNSUPPORTED:
		TRACE(TRACE_HAL,
		      "line %u test refused: the backend has no test op",
		      line);
		return UBUS_STATUS_NO_DATA;
	case LINE_TEST_QUERY_REFUSED:
		TRACE(TRACE_HAL,
		      "line %u test refused: the backend failed", line);
		return UBUS_STATUS_UNKNOWN_ERROR;
	}

	ubus_defer_request(ctx, req, &request->reply.req);
	request->reply.pending = true;
	request->reply.timeout.cb = line_test_request_expire;
	uloop_timeout_set(&request->reply.timeout, LINE_TEST_TIMEOUT_MS);

	return 0;
}

enum {
	LOOP_TEST_ENABLE,
	LOOP_TEST_MODE,
	LOOP_TEST_RING_S,
	LOOP_TEST_DELAY_MS,
	__LOOP_TEST_MAX,
};

static const struct blobmsg_policy loop_test_policy[__LOOP_TEST_MAX] = {
	[LOOP_TEST_ENABLE] = { "enable", BLOBMSG_TYPE_BOOL },
	[LOOP_TEST_MODE] = { "mode", BLOBMSG_TYPE_STRING },
	[LOOP_TEST_RING_S] = { "ring_s", BLOBMSG_TYPE_INT32 },
	[LOOP_TEST_DELAY_MS] = { "delay_ms", BLOBMSG_TYPE_INT32 },
};

/* The two words the surface takes. "cross" is the connection between the two
 * ports and "self" the loop a port plays to itself; a one-port board runs the
 * self loop whichever word arrives. */
static bool loop_test_mode_get(const char *name, bool *want_self)
{
	if (!strcmp(name, "self")) {
		*want_self = true;
		return true;
	}

	if (!strcmp(name, "cross")) {
		*want_self = false;
		return true;
	}

	return false;
}

static const char *loop_test_mode_name(uint8_t mode)
{
	switch (mode) {
	case LINE_LOOP_TEST_MODE_SELF:
		return "self";
	case LINE_LOOP_TEST_MODE_CROSS:
		return "cross";
	default:
		return "off";
	}
}

/*
 * The manufacturing loop test (line/line.h line_loop_test_set). It answers
 * synchronously, like every other refusal on this object. A successful reply
 * is the standing mode with all five keys, so a caller reads what the board
 * is doing, also when its call changed nothing. A refusal answers a ubus
 * status and no reply body.
 *
 * A blobmsg policy carries no defaults, so the two optional numbers take
 * theirs here, and the bounds are checked before the shell sees them.
 */
static int voice_loop_test(struct ubus_context *ctx, struct ubus_object *obj,
			   struct ubus_request_data *req, const char *method,
			   struct blob_attr *msg)
{
	struct blob_attr *tb[__LOOP_TEST_MAX] = {};
	unsigned int ring_s = LINE_LOOP_TEST_RING_S_DEFAULT;
	unsigned int delay_ms = LINE_LOOP_TEST_DELAY_DEFAULT_MS;
	struct line_loop_test_view view = {};
	enum line_loop_test_status status;
	bool want_self = false;
	bool enable;

	if (msg)
		blobmsg_parse(loop_test_policy, __LOOP_TEST_MAX, tb,
			      blob_data(msg), blob_len(msg));
	if (!tb[LOOP_TEST_ENABLE])
		return UBUS_STATUS_INVALID_ARGUMENT;

	enable = blobmsg_get_bool(tb[LOOP_TEST_ENABLE]);
	if (tb[LOOP_TEST_MODE] &&
	    !loop_test_mode_get(blobmsg_get_string(tb[LOOP_TEST_MODE]),
				&want_self))
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (tb[LOOP_TEST_RING_S])
		ring_s = blobmsg_get_u32(tb[LOOP_TEST_RING_S]);
	if (tb[LOOP_TEST_DELAY_MS])
		delay_ms = blobmsg_get_u32(tb[LOOP_TEST_DELAY_MS]);

	/* a negative INT32 arrives here as a large unsigned value, so both
	 * ends of each range are covered by the test above the maximum */
	if (ring_s < 1 || ring_s > LINE_LOOP_TEST_RING_S_MAX ||
	    delay_ms > LINE_LOOP_TEST_DELAY_MAX_MS)
		return UBUS_STATUS_INVALID_ARGUMENT;

	status = line_loop_test_set(enable, want_self, ring_s, delay_ms, &view);
	switch (status) {
	case LINE_LOOP_TEST_OK:
		break;
	case LINE_LOOP_TEST_BAD_ARGUMENT:
		return UBUS_STATUS_INVALID_ARGUMENT;
	case LINE_LOOP_TEST_NO_LINE:
		return UBUS_STATUS_NOT_FOUND;
	case LINE_LOOP_TEST_NOT_READY:
		TRACE(TRACE_HAL,
		      "loop test refused: the dsp is not ready");
		return UBUS_STATUS_NO_DATA;
	case LINE_LOOP_TEST_UNSUPPORTED:
		TRACE(TRACE_HAL,
		      "loop test refused: the backend has no loop op");
		return UBUS_STATUS_NO_DATA;
	case LINE_LOOP_TEST_CALL_ACTIVE:
		TRACE(TRACE_HAL, "loop test refused: a call is active");
		return UBUS_STATUS_NO_DATA;
	case LINE_LOOP_TEST_TEST_RUNNING:
		TRACE(TRACE_HAL, "loop test refused: a line test is running");
		return UBUS_STATUS_NO_DATA;
	case LINE_LOOP_TEST_REFUSED:
		TRACE(TRACE_HAL, "loop test refused: the backend failed");
		return UBUS_STATUS_UNKNOWN_ERROR;
	}

	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "enabled", view.enabled);
	blobmsg_add_string(&b, "mode", loop_test_mode_name(view.mode));
	blobmsg_add_u32(&b, "lines", view.lines);
	blobmsg_add_u32(&b, "ring_s", view.ring_s);
	blobmsg_add_u32(&b, "delay_ms", view.delay_ms);
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static void line_test_shutdown(void)
{
	unsigned int line;

	for (line = 0; line < CFG_CHANNEL_MAX; line++) {
		struct line_test_request *request = &line_test_reqs[line];

		if (!request->reply.pending)
			continue;
		ULOG_WARN("line %u test %s reply closed by the daemon stop\n",
			  line, line_test_names[request->test]);
		line_test_drop(line);
		deferred_reply_finish(&request->reply, UBUS_STATUS_NO_DATA);
	}
}

/*
 * The live per-call media query. Like line_test, it does not answer from
 * daemon state. The numbers come from a SLOW HAL op (hal.h
 * stream_stats_start), so the request defers: line/line.c submits the query
 * against the leg's still-running stream and the completion below renders the
 * same media table a call record carries, through the same cdr renderer.
 *
 * Every refusal is synchronous, which is the diagnostic-verb idiom: a caller
 * that gets no immediate error knows a query is running for it and one answer
 * is coming, either the table or an honest failure.
 *
 * One request per (line, slot), because line/line.c holds one query slot per
 * (line, stream) and a leg drives the stream of its own slot. The protocol
 * names that stream (ops->stream_of_slot), and the record keeps it, because
 * the drop names the query by its stream. A UI slot is the protocol's own
 * number, so the table holds HAL_STREAM_MAX of them per line, one per stream
 * a slot can name, and a slot no leg holds reads as no stream. The records
 * are static: a deferred handle must outlive the frame that took it. Its
 * lifetime is tied to this table rather than to the leg, so a call that ends
 * mid-query leaves no dangling handle.
 */
struct stats_live {
	struct deferred_reply reply;
	unsigned int slot;
	unsigned int stream;
};

static struct stats_live stats_live_reqs[CFG_CHANNEL_MAX][HAL_STREAM_MAX];

/*
 * The consumer line/line.c calls. A query whose request is no longer pending
 * was already answered by the timeout below, so its numbers are dropped: the
 * stale-handle rule of the record ring, applied to a ubus request.
 */
static void stats_live_done(void *priv, const struct hal_media_stats *stats)
{
	struct stats_live *q = priv;
	struct deferred_reply *reply = &q->reply;

	if (!reply->pending) {
		TRACE(TRACE_HAL, "line %u slot %u live stats arrived after the reply "
		      "was closed", reply->line, q->slot);
		return;
	}

	if (!stats) {
		deferred_reply_finish(reply, UBUS_STATUS_NO_DATA);
		return;
	}

	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "line", reply->line);
	blobmsg_add_u32(&b, "slot", q->slot);
	cdr_media_dump(&b, stats);
	ubus_send_reply(reply->ctx, &reply->req, b.head);
	deferred_reply_finish(reply, 0);
}

static void stats_live_expire(struct uloop_timeout *t)
{
	struct stats_live *q = container_of(t, struct stats_live,
					    reply.timeout);

	ULOG_WARN("live media stats for line %u slot %u produced no answer in %u ms\n",
		  q->reply.line, q->slot, STATS_LIVE_TIMEOUT_MS);
	line_stats_live_drop(q->reply.line, q->stream);
	deferred_reply_finish(&q->reply, UBUS_STATUS_TIMEOUT);
}

/*
 * ubus carries no busy code, so the two refusals that mean "there are no
 * numbers for you right now" share UBUS_STATUS_NO_DATA and the daemon trace
 * names which one it was; the bad-index and not-up classes keep the answers
 * the register and inject verbs already give for the same faults. The ubus
 * client renders NO_DATA as "No response", which is the client's word for the
 * code: this daemon answered in the same frame, and the hal trace domain
 * carries the reason.
 *
 * Three outcomes therefore share NO_DATA and a caller cannot tell them apart
 * from the code alone: a busy pair, a query the DSP answered with a failure,
 * and a daemon stop that closed the reply. All three mean "no numbers", none
 * of them means the daemon stayed silent, and the log names which one it was.
 */
static int stats_live_status(enum line_stats st)
{
	switch (st) {
	case LINE_STATS_OK:
		return 0;
	case LINE_STATS_NO_LINES:
	case LINE_STATS_NO_STREAM:
		return UBUS_STATUS_NOT_FOUND;
	case LINE_STATS_NO_LINE:
		return UBUS_STATUS_INVALID_ARGUMENT;
	case LINE_STATS_BUSY:
		return UBUS_STATUS_NO_DATA;
	case LINE_STATS_REFUSED:
		return UBUS_STATUS_UNKNOWN_ERROR;
	}

	return UBUS_STATUS_UNKNOWN_ERROR;
}

enum {
	STATS_LIVE_LINE,
	STATS_LIVE_SLOT,
	__STATS_LIVE_MAX,
};

static const struct blobmsg_policy stats_live_policy[__STATS_LIVE_MAX] = {
	[STATS_LIVE_LINE] = { "line", BLOBMSG_TYPE_INT32 },
	[STATS_LIVE_SLOT] = { "slot", BLOBMSG_TYPE_INT32 },
};

static int voice_stats_live(struct ubus_context *ctx, struct ubus_object *obj,
			    struct ubus_request_data *req, const char *method,
			    struct blob_attr *msg)
{
	const struct proto_ops *ops = proto_ops_get();
	struct blob_attr *tb[__STATS_LIVE_MAX] = {};
	struct stats_live *q;
	enum line_stats st;
	uint32_t line, slot;

	if (msg)
		blobmsg_parse(stats_live_policy, __STATS_LIVE_MAX, tb, blob_data(msg),
			      blob_len(msg));
	if (!tb[STATS_LIVE_LINE] || !tb[STATS_LIVE_SLOT])
		return UBUS_STATUS_INVALID_ARGUMENT;

	line = blobmsg_get_u32(tb[STATS_LIVE_LINE]);
	slot = blobmsg_get_u32(tb[STATS_LIVE_SLOT]);
	if (line >= CFG_CHANNEL_MAX || slot >= HAL_STREAM_MAX)
		return UBUS_STATUS_INVALID_ARGUMENT;

	q = &stats_live_reqs[line][slot];
	/* the line/line.c busy rule again, at the request table, and it is this
	 * one a second caller normally meets: one deferred reply per pair,
	 * because a second one would overwrite the handle the first is still
	 * owed */
	if (q->reply.pending) {
		TRACE(TRACE_HAL, "line %u slot %u live stats refused: a reply is "
		      "already deferred", line, slot);
		return UBUS_STATUS_NO_DATA;
	}

	q->reply.ctx = ctx;
	q->reply.line = line;
	q->slot = slot;
	q->stream = ops->stream_of_slot ? ops->stream_of_slot(line, slot) :
		    HAL_STREAM_MAX;

	st = line_stats_live_query(line, q->stream, stats_live_done, q);
	if (st != LINE_STATS_OK)
		return stats_live_status(st);

	/* After the submission, so a refusal leaves no deferred request behind
	 * and answers from this frame like every other refusal */
	ubus_defer_request(ctx, req, &q->reply.req);
	q->reply.pending = true;
	q->reply.timeout.cb = stats_live_expire;
	uloop_timeout_set(&q->reply.timeout, STATS_LIVE_TIMEOUT_MS);

	return 0;
}

/*
 * A daemon stop with queries in flight. main.c tears ubus down first, before
 * the account drain, lines_done() and hal_close(), so this is the last moment a
 * deferred reply can still reach its caller: every one of them is closed here
 * with an honest error rather than left for a client timeout to explain. The
 * queries themselves are abandoned with the replies, so the completions
 * lines_done() forces afterwards find nothing pending and land nowhere.
 *
 * It is required for memory safety, not only for the nicer error. Without it
 * a query is still pending when lines_done() runs stats_drop(), and that
 * forced failure reaches stats_live_done() on a ubus context ubus_shutdown()
 * already tore down: the reply would be written to a closed socket descriptor,
 * which by then may name another file entirely, and the blob buffer freed on
 * the way out would be built again with nothing left to free it. The hook is
 * what makes that path unreachable.
 *
 * Whether the answer arrives is ubusd's race, not this daemon's. The status is
 * written to the ubus socket here and ubus_auto_shutdown() closes that socket
 * on the next line, so epoll can report the data and the peer hangup to ubusd
 * in one event; ubusd then skips the read (ubusd_main.c client_cb, which reads
 * only "if (!sock->eof)") and the caller waits out its own timeout instead.
 * Nothing in this daemon can close that window, and the caller is strictly
 * better off than with no answer sent at all.
 */
static void stats_live_shutdown(void)
{
	unsigned int line, slot;

	for (line = 0; line < CFG_CHANNEL_MAX; line++) {
		for (slot = 0; slot < HAL_STREAM_MAX; slot++) {
			struct stats_live *q = &stats_live_reqs[line][slot];
			struct deferred_reply *reply = &q->reply;

			if (!reply->pending)
				continue;

			ULOG_WARN("live media stats for line %u slot %u closed by the "
				  "daemon stop\n", line, slot);
			line_stats_live_drop(line, q->stream);
			deferred_reply_finish(reply, UBUS_STATUS_NO_DATA);
		}
	}
}

/*
 * The rows of the voice object the core owns. The protocol's own rows go
 * between the two parts (voice_methods_compose), so the method list keeps one
 * order whatever the protocol adds.
 */
static const struct ubus_method voice_methods_head[] = {
	UBUS_METHOD_NOARG("status", voice_status),
	UBUS_METHOD_NOARG("reload", voice_reload),
	UBUS_METHOD_NOARG("dump_config", voice_dump_config),
	UBUS_METHOD_NOARG("cdr", voice_cdr),
	UBUS_METHOD("stats", voice_stats, line_policy),
	UBUS_METHOD_NOARG("stats_reset", voice_stats_reset),
	UBUS_METHOD("line_test", voice_line_test, line_test_policy),
	UBUS_METHOD("loop_test", voice_loop_test, loop_test_policy),
	UBUS_METHOD("stats_live", voice_stats_live, stats_live_policy),
};

static const struct ubus_method voice_methods_tail[] = {
	UBUS_METHOD("seize", voice_seize, line_policy),
	UBUS_METHOD("release", voice_release, line_policy),
	UBUS_METHOD("dial", voice_dial, dial_policy),
};

/* the bound of the rows a protocol adds to the voice object */
#define VOICE_PROTO_METHODS_MAX	24

static struct ubus_method voice_methods[ARRAY_SIZE(voice_methods_head) +
					VOICE_PROTO_METHODS_MAX +
					ARRAY_SIZE(voice_methods_tail)];

static struct ubus_object_type voice_object_type = {
	.name = "voice",
	.methods = voice_methods,
};

static struct ubus_object voice_object = {
	.name = "voice",
	.type = &voice_object_type,
	.methods = voice_methods,
};

/*
 * The voice object of the selected protocol: the core rows and the rows of
 * proto_ubus_methods(). A protocol that adds more rows than the bound loses
 * the rest, and the log names the cut.
 */
static void voice_methods_compose(void)
{
	const struct ubus_method *rows;
	unsigned int n = 0, nproto = 0, i;

	for (i = 0; i < ARRAY_SIZE(voice_methods_head); i++)
		voice_methods[n++] = voice_methods_head[i];

	rows = proto_ubus_methods(&nproto);
	if (nproto > VOICE_PROTO_METHODS_MAX) {
		ULOG_ERR("the protocol adds %u ubus methods, the voice object "
			 "takes %u\n", nproto, VOICE_PROTO_METHODS_MAX);
		nproto = VOICE_PROTO_METHODS_MAX;
	}
	for (i = 0; rows && i < nproto; i++)
		voice_methods[n++] = rows[i];

	for (i = 0; i < ARRAY_SIZE(voice_methods_tail); i++) {
		unsigned int j;

		for (j = 0; j < n; j++)
			if (!strcmp(voice_methods[j].name,
				    voice_methods_tail[i].name))
				break;
		if (j < n)
			continue;
		voice_methods[n++] = voice_methods_tail[i];
	}

	voice_object_type.n_methods = (int)n;
	voice_object.n_methods = (int)n;
}

static void trace_state_dump(void)
{
	int dom;

	for (dom = 0; dom < TRACE_DOMAIN_MAX; dom++)
		blobmsg_add_u8(&b, trace_domain_name(dom), trace_domain_get(dom));
}

static int trace_get(struct ubus_context *ctx, struct ubus_object *obj,
		     struct ubus_request_data *req, const char *method,
		     struct blob_attr *msg)
{
	blob_buf_init(&b, 0);
	trace_state_dump();
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static const struct blobmsg_policy trace_policy[TRACE_DOMAIN_MAX] = {
	[TRACE_SIP] = { "sip", BLOBMSG_TYPE_BOOL },
	[TRACE_FSM] = { "fsm", BLOBMSG_TYPE_BOOL },
	[TRACE_HOOK] = { "hook", BLOBMSG_TYPE_BOOL },
	[TRACE_HAL] = { "hal", BLOBMSG_TYPE_BOOL },
	[TRACE_DIALPLAN] = { "dialplan", BLOBMSG_TYPE_BOOL },
	[TRACE_MEDIA] = { "media", BLOBMSG_TYPE_BOOL },
	[TRACE_JB] = { "jb", BLOBMSG_TYPE_BOOL },
	[TRACE_MGCP] = { "mgcp", BLOBMSG_TYPE_BOOL },
};

static int trace_set(struct ubus_context *ctx, struct ubus_object *obj,
		     struct ubus_request_data *req, const char *method,
		     struct blob_attr *msg)
{
	struct blob_attr *tb[TRACE_DOMAIN_MAX] = {};
	int dom;

	if (msg)
		blobmsg_parse(trace_policy, TRACE_DOMAIN_MAX, tb, blob_data(msg),
			      blob_len(msg));

	for (dom = 0; dom < TRACE_DOMAIN_MAX; dom++) {
		if (!tb[dom])
			continue;

		trace_domain_set(dom, blobmsg_get_bool(tb[dom]));
	}

	blob_buf_init(&b, 0);
	trace_state_dump();
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

static const struct ubus_method trace_methods[] = {
	UBUS_METHOD_NOARG("get", trace_get),
	UBUS_METHOD("set", trace_set, trace_policy),
};

static struct ubus_object_type trace_object_type =
	UBUS_OBJECT_TYPE("voice.trace", trace_methods);

static struct ubus_object trace_object = {
	.name = "voice.trace",
	.type = &trace_object_type,
	.methods = trace_methods,
	.n_methods = ARRAY_SIZE(trace_methods),
};

/*
 * voice.calltrace: arm one trace on one line, cancel it, read it. The three
 * methods answer the same status reply, so a caller reads the outcome of what
 * it asked for from the reply it already has.
 */
enum {
	CT_ARG_LINE,
	CT_ARG_TIMEOUT,
	CT_ARG_GRACE,
	CT_ARG_ARM_TIMEOUT,
	CT_ARG_WIRE,
	CT_ARG_WIRE_MAX_MB,
	CT_ARG_RINGS_MAX_MB,
	CT_ARG_LEC,
	CT_ARG_LEC_MAX_MB,
	__CT_ARG_MAX,
};

static const struct blobmsg_policy calltrace_policy[__CT_ARG_MAX] = {
	[CT_ARG_LINE] = { "line", BLOBMSG_TYPE_INT32 },
	[CT_ARG_TIMEOUT] = { "timeout", BLOBMSG_TYPE_INT32 },
	[CT_ARG_GRACE] = { "grace", BLOBMSG_TYPE_INT32 },
	[CT_ARG_ARM_TIMEOUT] = { "arm_timeout", BLOBMSG_TYPE_INT32 },
	[CT_ARG_WIRE] = { "wire", BLOBMSG_TYPE_BOOL },
	[CT_ARG_WIRE_MAX_MB] = { "wire_max_mb", BLOBMSG_TYPE_INT32 },
	[CT_ARG_RINGS_MAX_MB] = { "rings_max_mb", BLOBMSG_TYPE_INT32 },
	[CT_ARG_LEC] = { "lec", BLOBMSG_TYPE_BOOL },
	[CT_ARG_LEC_MAX_MB] = { "lec_max_mb", BLOBMSG_TYPE_INT32 },
};

static void calltrace_reply_send(struct ubus_context *ctx,
				 struct ubus_request_data *req)
{
	blob_buf_init(&b, 0);
	calltrace_status_dump(&b);
	ubus_send_reply(ctx, req, b.head);
}

/*
 * An absent argument takes the shipped value, so a caller that names the line
 * alone gets the default window of calltrace.h. The wire capture is on
 * unless the caller turns it off: a trace of a call without its packets is
 * the exception, not the rule. The canceller capture is off unless the caller
 * turns it on, because it records the near-end room of every line in a call.
 */
static void calltrace_options_take(struct blob_attr **tb,
				   struct calltrace_options *o)
{
	memset(o, 0, sizeof(*o));
	o->timeout_s = tb[CT_ARG_TIMEOUT] ?
		blobmsg_get_u32(tb[CT_ARG_TIMEOUT]) :
		CALLTRACE_TIMEOUT_DEFAULT_S;
	o->grace_s = tb[CT_ARG_GRACE] ? blobmsg_get_u32(tb[CT_ARG_GRACE]) :
		CALLTRACE_GRACE_DEFAULT_S;
	o->arm_timeout_s = tb[CT_ARG_ARM_TIMEOUT] ?
		blobmsg_get_u32(tb[CT_ARG_ARM_TIMEOUT]) :
		CALLTRACE_ARM_TIMEOUT_DEF_S;
	o->wire_max_mb = tb[CT_ARG_WIRE_MAX_MB] ?
		blobmsg_get_u32(tb[CT_ARG_WIRE_MAX_MB]) :
		CALLTRACE_WIRE_MAX_MB_DEF;
	o->rings_max_mb = tb[CT_ARG_RINGS_MAX_MB] ?
		blobmsg_get_u32(tb[CT_ARG_RINGS_MAX_MB]) :
		CALLTRACE_RINGS_MAX_MB_DEF;
	o->lec_max_mb = tb[CT_ARG_LEC_MAX_MB] ?
		blobmsg_get_u32(tb[CT_ARG_LEC_MAX_MB]) :
		CALLTRACE_LEC_MAX_MB_DEF;
	o->wire = tb[CT_ARG_WIRE] ? blobmsg_get_bool(tb[CT_ARG_WIRE]) : true;
	o->lec = tb[CT_ARG_LEC] ? blobmsg_get_bool(tb[CT_ARG_LEC]) : false;
}

/*
 * The refusal classes of an arm. A bad line and a bad range are bad
 * arguments; a second arm shares the answer the
 * busy stats_live pair gives, because ubus carries no busy code; every other
 * preflight refusal is an unknown error whose word the reply carries in
 * last_error.
 */
static int calltrace_arm_status(int err)
{
	switch (err) {
	case 0:
		return 0;
	case -EINVAL:
		return UBUS_STATUS_INVALID_ARGUMENT;
	case -EBUSY:
		return UBUS_STATUS_NO_DATA;
	default:
		return UBUS_STATUS_UNKNOWN_ERROR;
	}
}

static int voice_calltrace_arm(struct ubus_context *ctx,
			       struct ubus_object *obj,
			       struct ubus_request_data *req,
			       const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__CT_ARG_MAX] = {};
	struct calltrace_options opt;
	int err;

	if (msg)
		blobmsg_parse(calltrace_policy, __CT_ARG_MAX, tb,
			      blob_data(msg), blob_len(msg));
	if (!tb[CT_ARG_LINE])
		return UBUS_STATUS_INVALID_ARGUMENT;

	/*
	 * Every index goes to the controller, which owns the line range: a
	 * bound checked here would answer Invalid argument without recording
	 * the word the reply carries, so a caller would read no reason why.
	 */
	calltrace_options_take(tb, &opt);
	err = calltrace_arm(blobmsg_get_u32(tb[CT_ARG_LINE]), &opt);
	if (err)
		return calltrace_arm_status(err);

	calltrace_reply_send(ctx, req);

	return 0;
}

static int voice_calltrace_cancel(struct ubus_context *ctx,
				  struct ubus_object *obj,
				  struct ubus_request_data *req,
				  const char *method, struct blob_attr *msg)
{
	if (calltrace_cancel())
		return UBUS_STATUS_NOT_FOUND;

	calltrace_reply_send(ctx, req);

	return 0;
}

static int voice_calltrace_status(struct ubus_context *ctx,
				  struct ubus_object *obj,
				  struct ubus_request_data *req,
				  const char *method, struct blob_attr *msg)
{
	calltrace_reply_send(ctx, req);

	return 0;
}

static const struct ubus_method calltrace_methods[] = {
	UBUS_METHOD("arm", voice_calltrace_arm, calltrace_policy),
	UBUS_METHOD_NOARG("cancel", voice_calltrace_cancel),
	UBUS_METHOD_NOARG("status", voice_calltrace_status),
};

static struct ubus_object_type calltrace_object_type =
	UBUS_OBJECT_TYPE("voice.calltrace", calltrace_methods);

static struct ubus_object calltrace_object = {
	.name = "voice.calltrace",
	.type = &calltrace_object_type,
	.methods = calltrace_methods,
	.n_methods = ARRAY_SIZE(calltrace_methods),
};

static void ubus_connect_cb(struct ubus_context *ctx)
{
	int ret;

	ubus_connected = true;
	voice_methods_compose();
	ret = ubus_add_object(ctx, &voice_object);
	if (ret)
		ULOG_ERR("failed to add object voice: %s\n", ubus_strerror(ret));

	ret = ubus_add_object(ctx, &trace_object);
	if (ret)
		ULOG_ERR("failed to add object voice.trace: %s\n", ubus_strerror(ret));

	ret = ubus_add_object(ctx, &calltrace_object);
	if (ret)
		ULOG_ERR("failed to add object voice.calltrace: %s\n",
			 ubus_strerror(ret));

	net_iface_ubus_attach(ctx);

#ifdef UVOICE_HAL_STUB
	stub_ubus_attach(ctx);
#endif
}

struct ubus_context *uvoice_ubus_ctx(void)
{
	return ubus_connected ? &conn.ctx : NULL;
}

void uvoice_ubus_notify(const char *type, struct blob_attr *msg)
{
	if (!ubus_connected || !voice_object.id)
		return;

	ubus_notify(&conn.ctx, &voice_object, type, msg, -1);
}

int uvoice_ubus_init(const char *socket_path)
{
	conn.path = socket_path;
	conn.cb = ubus_connect_cb;
	ubus_auto_connect(&conn);

	return 0;
}

void uvoice_ubus_done(void)
{
	line_test_shutdown();
	stats_live_shutdown();
	ubus_connected = false;
	ubus_auto_shutdown(&conn);
	blob_buf_free(&b);
}
