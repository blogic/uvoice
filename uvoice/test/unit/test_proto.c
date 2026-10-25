// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the protocol registry (src/proto/proto.c).
 *
 * Isolation. The suite compiles proto/proto.c with build flags of the
 * protocols. It defines a scripted SIP table whose entries count their calls.
 * Host adapters of proto/proto.c reach real net/, fw.c and cdr.c objects where
 * they need no daemon. Inert stubs below stand for line shell, call trace and
 * ubus. Tables under test run on a recorder host (proto_host_fake.c).
 *
 * Two builds. cmake builds the suite twice. test_proto carries
 * UVOICE_PROTO_SIP alone, so a config that names mgcp runs the null table.
 * test_proto_both carries UVOICE_PROTO_MGCP too and links the MGCP unit. Its
 * registry lists both tables in their fixed order: the MGCP table of the
 * image (src/proto/mgcp/proto.c) and a scripted SIP table. The SIP table
 * of the image does not link in. Its call agent is a UDP socket of the suite
 * on the loopback.
 *
 * Time. The graceful stop runs the real uloop to its end, which a uloop timer
 * of the scripted table brings, or to the bound of proto/proto.c.
 *
 * A started MGCP table runs its timers on a virtual clock
 * (test_mgcp_clock.h). The real uloop runs only until a socket event that a
 * case waits for.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubus.h>

#include "calltrace.h"
#include "event.h"
#include "line/line.h"
#include "net/ports.h"
#include "proto/proto.h"
#ifdef UVOICE_PROTO_MGCP
#include "proto/mgcp/proto.h"
#include "proto/shared/dst.h"
#include "proto/shared/resolve.h"
#endif
#include "uvoice.h"
#include "proto_host_fake.h"
#include "test_util.h"

struct uvoice uv;

/* --------------------------------------------- the line shell, inert here */

enum hal_status line_tone_play(unsigned int line, enum hal_tone tone,
			       uint32_t duration_ms)
{
	return HAL_OK;
}

void line_tone_stop(unsigned int line)
{
}

void line_ringer_start(unsigned int line, enum proto_ring_kind kind,
		     unsigned int ring_template, uint32_t duration_ms,
		     const struct proto_cid *cid)
{
}

void line_ringer_stop(unsigned int line)
{
}

void line_cw_alert(unsigned int line, bool on, unsigned int variant,
		   const struct proto_cid *cid)
{
}

enum hal_status line_cid_send(unsigned int line, enum hal_cid_kind kind,
			      const struct proto_cid *cid)
{
	return HAL_OK;
}

unsigned int line_ring_template_match(unsigned int line, const char *number)
{
	return 0;
}

void line_mwi_set(unsigned int line, bool on, uint32_t new_msgs,
		  uint32_t old_msgs)
{
}

bool line_mwi_get(unsigned int line, uint32_t *new_msgs, uint32_t *old_msgs)
{
	return false;
}

bool line_detector_arm(unsigned int line, uint32_t detect_mask)
{
	return true;
}

bool line_fwd_disc_signal(unsigned int line)
{
	return false;
}

void line_fwd_disc_abort(unsigned int line)
{
}

enum hal_status line_battery_set(unsigned int line, enum proto_battery batt)
{
	return HAL_OK;
}

bool line_digit_play(unsigned int line, unsigned int stream, char digit,
		     uint16_t duration_ms, bool dedup)
{
	return true;
}

void line_digit_play_cancel(unsigned int line, unsigned int stream)
{
}

enum hal_status line_stream_stats_get(unsigned int line, unsigned int stream,
				      struct hal_media_stats *out)
{
	return HAL_UNSUPPORTED;
}

enum line_stats line_stats_live_query(unsigned int line, unsigned int stream,
				      line_stats_cb cb, void *priv)
{
	return LINE_STATS_NO_STREAM;
}

void line_stats_live_drop(unsigned int line, unsigned int stream)
{
}

bool line_exists(unsigned int line)
{
	return true;
}

bool line_call_admit(unsigned int line)
{
	return true;
}

bool line_hal_ready(void)
{
	return true;
}

void line_service_hold(unsigned int line, const char *reason)
{
}

void line_service_release(unsigned int line)
{
}

void line_transition(unsigned int line, unsigned int state_id, uint32_t flags)
{
}

void line_settle(unsigned int line)
{
}

void line_cfg_landed(unsigned int line)
{
}

enum hal_status line_conf_set(unsigned int line, bool on)
{
	return HAL_OK;
}

void line_fault(unsigned int line, bool loud, const char *text)
{
}

const struct call_media_ops *line_media_ops(void)
{
	static const struct call_media_ops none;

	return &none;
}

const struct hal_cap *lines_hal_cap(void)
{
	static const struct hal_cap none;

	return &none;
}

const struct hal_region *lines_region(void)
{
	static const struct hal_region none;

	return &none;
}

/* ------------------------------------------- events, trace and ubus, inert */

void event_reg_post(unsigned int line, bool up, const char *proxy)
{
}

void event_tls_cert_start(const char *url, const char *username,
			  const char *password, bool verify, void (*done)(void))
{
}

void event_tls_cert_stop(enum event_tls_stop_mode mode)
{
}

void calltrace_event(unsigned int line, const char *reason)
{
}

void calltrace_call_started(unsigned int line, uint32_t cdr_seq)
{
}

void calltrace_line_state(unsigned int line, const char *before,
			  const char *after)
{
}

struct ubus_context *uvoice_ubus_ctx(void)
{
	return NULL;
}

void uvoice_ubus_request_reply(struct ubus_context *ctx,
			       struct ubus_request_data *req, unsigned int line,
			       const char *verb)
{
}

void uvoice_ubus_notify(const char *type, struct blob_attr *msg)
{
}

enum hal_status line_audio_path_set(enum hal_audio_path path)
{
	return HAL_OK;
}

enum hal_status line_volume_set(unsigned int level)
{
	return HAL_OK;
}

enum hal_status line_mute_set(bool mute)
{
	return HAL_OK;
}

enum hal_status line_led_set(enum hal_led led, enum hal_led_mode mode)
{
	return HAL_OK;
}

enum hal_status line_key_tone_play(unsigned int line, char digit,
				   uint32_t duration_ms)
{
	return HAL_OK;
}

/* ------------------------------------------------- the scripted SIP table */

static struct {
	unsigned int init;
	unsigned int start;
	unsigned int reload;
	unsigned int done;
	unsigned int shutdown_begin;
	/* the drain: pending answers it, and report_ms is when the scripted
	 * stop reports its end through the host, 0 for never */
	bool pending;
	unsigned int report_ms;
	const struct proto_host *host;
	struct uloop_timeout report;
} sip;

static void sip_report_cb(struct uloop_timeout *t)
{
	sip.pending = false;
	sip.host->shutdown_done(sip.host->ctx);
}

static void sip_init(const struct proto_host *host, const struct cfg *cfg)
{
	sip.init++;
	sip.host = host;
}

static void sip_start(const struct cfg *cfg)
{
	sip.start++;
}

static void sip_reload(const struct cfg *old_cfg, const struct cfg *new_cfg)
{
	sip.reload++;
}

static void sip_shutdown_begin(void)
{
	sip.shutdown_begin++;
	if (!sip.pending || !sip.report_ms)
		return;

	sip.report.cb = sip_report_cb;
	uloop_timeout_set(&sip.report, (int)sip.report_ms);
}

static bool sip_shutdown_pending(void)
{
	return sip.pending;
}

static void sip_done(void)
{
	sip.done++;
}

static void sip_line_event(unsigned int line, const struct proto_line_event *ev)
{
}

static void sip_line_service(unsigned int line, bool in_service,
			     enum proto_service_reason reason)
{
}

static void sip_line_reset(unsigned int line)
{
}

static uint32_t sip_line_flags(unsigned int line)
{
	return 0;
}

static void sip_status_dump_call(struct blob_buf *b, unsigned int line)
{
}

static void sip_status_dump_lines(struct blob_buf *b, int want_line)
{
	void *arr = blobmsg_open_array(b, "lines");

	blobmsg_close_array(b, arr);
}

static int sip_ubus_noop(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method,
			 struct blob_attr *msg)
{
	return 0;
}

static const struct ubus_method sip_rows[] = {
	UBUS_METHOD_NOARG("register", sip_ubus_noop),
	UBUS_METHOD_NOARG("unregister", sip_ubus_noop),
};

static const struct ubus_method *sip_ubus_methods(unsigned int *n)
{
	*n = sizeof(sip_rows) / sizeof(sip_rows[0]);

	return sip_rows;
}

const struct proto_ops sip_proto_ops = {
	.name = "sip",
	.init = sip_init,
	.start = sip_start,
	.reload = sip_reload,
	.shutdown_begin = sip_shutdown_begin,
	.shutdown_pending = sip_shutdown_pending,
	.done = sip_done,
	.line_event = sip_line_event,
	.line_service = sip_line_service,
	.line_reset = sip_line_reset,
	.line_flags = sip_line_flags,
	.status_dump_call = sip_status_dump_call,
	.status_dump_lines = sip_status_dump_lines,
	.ubus_methods = sip_ubus_methods,
};

static void sip_reset(void)
{
	uloop_timeout_cancel(&sip.report);
	memset(&sip, 0, sizeof(sip));
}

/* ------------------------------------------------------ the MGCP table */

#ifdef UVOICE_PROTO_MGCP
/* the image carries both tables, so a word the registry knows nowhere selects
 * the null table */
#define NULL_WORD	"h323"
#define AVAILABLE	"sip mgcp"
#define SUITE_NAME	"test_proto_both"
#else
#define NULL_WORD	"mgcp"
#define AVAILABLE	"sip"
#define SUITE_NAME	"test_proto"
#endif

/* ------------------------------------------------------------- the helpers */

static struct cfg cfg;

static void cfg_protocol(const char *word)
{
	memset(&cfg, 0, sizeof(cfg));
	snprintf(cfg.protocol, sizeof(cfg.protocol), "%s", word);
}

/* One string key of the signaling table, or NULL. */
static struct blob_attr *signaling_key(struct blob_buf *b, const char *key)
{
	struct blob_attr *cur, *inner;
	size_t rem, irem;

	blob_for_each_attr(cur, b->head, rem) {
		if (strcmp(blobmsg_name(cur), "signaling"))
			continue;

		blobmsg_for_each_attr(inner, cur, irem) {
			if (!strcmp(blobmsg_name(inner), key))
				return inner;
		}
	}

	return NULL;
}

static bool signaling_str_is(struct blob_buf *b, const char *key,
			     const char *want)
{
	struct blob_attr *a = signaling_key(b, key);

	return a && blobmsg_type(a) == BLOBMSG_TYPE_STRING &&
	       !strcmp(blobmsg_get_string(a), want);
}

/* The available[] array holds exactly these words, in this order, separated
 * by one space. */
static bool available_is(struct blob_buf *b, const char *want)
{
	struct blob_attr *arr = signaling_key(b, "available");
	struct blob_attr *cur;
	char got[64] = "";
	size_t rem, len;

	if (!arr || blobmsg_type(arr) != BLOBMSG_TYPE_ARRAY)
		return false;

	blobmsg_for_each_attr(cur, arr, rem) {
		len = strlen(got);
		snprintf(got + len, sizeof(got) - len, "%s%s", len ? " " : "",
			 blobmsg_get_string(cur));
	}

	return !strcmp(got, want);
}

static void status_take(struct blob_buf *b)
{
	blob_buf_init(b, 0);
	proto_status_dump(b);
}

/* ------------------------------------------------------------- the registry */

static void test_registry(void)
{
	CASE("registry: a known name answers its table");
	CHECK(proto_select("sip") == &sip_proto_ops);

	CASE("registry: an unknown name answers none");
	CHECK(proto_select("h323") == NULL);
	CHECK(proto_select("") == NULL);
	CHECK(proto_select(NULL) == NULL);

#ifdef UVOICE_PROTO_MGCP
	CASE("registry: an image with both flags answers the mgcp table");
	CHECK(proto_select("mgcp") == &mgcp_proto_ops);
	CHECK(proto_select("sip") == &sip_proto_ops);
#else
	CASE("registry: an image without the mgcp table answers none for it");
	CHECK(proto_select("mgcp") == NULL);
#endif
}

/*
 * The mandatory entries of proto/proto.h: every one of them, taken away alone,
 * makes the table incomplete, and an optional one does not.
 */
static void test_complete(void)
{
	static const size_t mandatory[] = {
		offsetof(struct proto_ops, init),
		offsetof(struct proto_ops, start),
		offsetof(struct proto_ops, reload),
		offsetof(struct proto_ops, done),
		offsetof(struct proto_ops, line_event),
		offsetof(struct proto_ops, line_service),
		offsetof(struct proto_ops, line_reset),
		offsetof(struct proto_ops, line_flags),
		offsetof(struct proto_ops, status_dump_call),
		offsetof(struct proto_ops, status_dump_lines),
		offsetof(struct proto_ops, shutdown_begin),
		offsetof(struct proto_ops, shutdown_pending),
	};
	struct proto_ops t;
	unsigned int i, refused = 0;

	CASE("complete: a table with every mandatory entry passes");
	CHECK(proto_ops_complete(&sip_proto_ops));

	CASE("complete: a table that lacks one mandatory entry fails");
	for (i = 0; i < sizeof(mandatory) / sizeof(mandatory[0]); i++) {
		t = sip_proto_ops;
		memset((char *)&t + mandatory[i], 0, sizeof(void (*)(void)));
		if (!proto_ops_complete(&t))
			refused++;
	}
	CHECK_UINT(refused, sizeof(mandatory) / sizeof(mandatory[0]));

	CASE("complete: and so does a table with no name");
	t = sip_proto_ops;
	t.name = NULL;
	CHECK(!proto_ops_complete(&t));
	CHECK(!proto_ops_complete(NULL));

	CASE("complete: an optional entry may be NULL");
	t = sip_proto_ops;
	t.dsp_ready = NULL;
	t.line_cfg_changed = NULL;
	t.region_changed = NULL;
	t.ubus_methods = NULL;
	t.status_dump_media = NULL;
	CHECK(proto_ops_complete(&t));
}

/* --------------------------------------------- the state and its status */

static void test_state(void)
{
	struct blob_buf b;

	memset(&b, 0, sizeof(b));

	CASE("state: a config naming sip runs the SIP table");
	cfg_protocol("sip");
	CHECK(proto_select_from_cfg(&cfg) == &sip_proto_ops);
	CHECK(proto_running());
	CHECK(proto_ops_get() == &sip_proto_ops);

	CASE("state: an empty word selects sip");
	cfg_protocol("");
	CHECK(proto_select_from_cfg(&cfg) == &sip_proto_ops);
	CHECK(proto_running());

	CASE("status: the running table names its protocol and no error");
	status_take(&b);
	CHECK(signaling_str_is(&b, "protocol", "sip"));
	CHECK(signaling_str_is(&b, "state", "running"));
	CHECK(signaling_key(&b, "error") == NULL);

	CASE("status: available lists the tables of the image in their order");
	CHECK(available_is(&b, AVAILABLE));

#ifdef UVOICE_PROTO_MGCP
	CASE("state: a config naming mgcp runs the MGCP table");
	cfg_protocol("mgcp");
	CHECK(proto_select_from_cfg(&cfg) == &mgcp_proto_ops);
	CHECK(proto_running());
	CHECK(proto_ops_get() == &mgcp_proto_ops);

	CASE("status: the MGCP table names its protocol and no error");
	status_take(&b);
	CHECK(signaling_str_is(&b, "protocol", "mgcp"));
	CHECK(signaling_str_is(&b, "state", "running"));
	CHECK(signaling_key(&b, "error") == NULL);
	CHECK(available_is(&b, AVAILABLE));

	CASE("state: a word the image carries nowhere is unavailable");
#else
	CASE("state: a config naming mgcp is unavailable in this image");
#endif
	cfg_protocol(NULL_WORD);
	CHECK(proto_select_from_cfg(&cfg) != &sip_proto_ops);
	CHECK(!proto_running());
	CHECK(!strcmp(proto_ops_get()->name, "none"));

	CASE("status: the unavailable table names what was asked and why");
	status_take(&b);
	CHECK(signaling_str_is(&b, "protocol", NULL_WORD));
	CHECK(signaling_str_is(&b, "state", "unavailable"));
	CHECK(signaling_str_is(&b, "error",
			       "the image does not carry this protocol"));
	CHECK(available_is(&b, AVAILABLE));

	CASE("state: a later config naming sip runs it again");
	cfg_protocol("sip");
	proto_select_from_cfg(&cfg);
	CHECK(proto_running());

	CASE("status: and the error of the unavailable state goes with it");
	status_take(&b);
	CHECK(signaling_str_is(&b, "state", "running"));
	CHECK(signaling_key(&b, "error") == NULL);
	blob_buf_free(&b);
}

/* ------------------------------------------------------- the null table */

static void test_null_table(void)
{
	const struct proto_ops *ops;
	struct proto_line_event ev;
	unsigned int n = 1;
	struct blob_buf b;

	cfg_protocol(NULL_WORD);
	ops = proto_select_from_cfg(&cfg);
	proto_host_fake_reset();

	CASE("null table: every mandatory entry is present");
	CHECK(proto_ops_complete(ops));

	CASE("null table: a seize plays reorder until stopped");
	ops->init(proto_host_fake(), &cfg);
	ops->start(&cfg);
	memset(&ev, 0, sizeof(ev));
	ev.type = PROTO_EV_HOOK;
	ev.u.hook.state = HAL_HOOK_SEIZE;
	ops->line_event(0, &ev);
	CHECK_UINT(phf.n[PHF_TONE_PLAY], 1);
	CHECK_UINT(phf.tone, HAL_TONE_REORDER);

	CASE("null table: the release stops it");
	ev.u.hook.state = HAL_HOOK_RELEASE;
	ops->line_event(0, &ev);
	CHECK_UINT(phf.n[PHF_TONE_STOP], 1);

	CASE("null table: a digit and a flash do nothing");
	ev.u.hook.state = HAL_HOOK_FLASH;
	ops->line_event(0, &ev);
	ev.type = PROTO_EV_DIGIT;
	ev.u.digit.digit = '5';
	ops->line_event(0, &ev);
	CHECK_UINT(phf.n[PHF_TONE_PLAY], 1);
	CHECK_UINT(phf.n[PHF_TONE_STOP], 1);

	CASE("null table: the inert entries answer at rest");
	ops->line_service(0, false, PROTO_SERVICE_CFG);
	ops->line_service(0, false, PROTO_SERVICE_FAULT);
	ops->line_service(0, true, PROTO_SERVICE_CFG);
	ops->line_reset(0);
	ops->reload(&cfg, &cfg);
	CHECK_UINT(ops->line_flags(0), 0);
	ops->shutdown_begin();
	CHECK(!ops->shutdown_pending());

	CASE("null table: the status writes an empty lines array and no call "
	     "key");
	memset(&b, 0, sizeof(b));
	blob_buf_init(&b, 0);
	ops->status_dump_call(&b, 0);
	CHECK_UINT(blob_len(b.head), 0);
	ops->status_dump_lines(&b, -1);
	CHECK(blob_len(b.head) > 0);
	blob_buf_free(&b);

	CASE("null table: no ubus rows of its own");
	CHECK(proto_ubus_methods(&n) == NULL);
	CHECK_UINT(n, 0);

	CASE("null table: done leaves the host");
	ops->done();
	ev.type = PROTO_EV_HOOK;
	ev.u.hook.state = HAL_HOOK_SEIZE;
	ops->line_event(0, &ev);
	CHECK_UINT(phf.n[PHF_TONE_PLAY], 1);
}

/*
 * The core builds its voice object from core rows and rows of a table in
 * force. register and unregister are SIP rows, so neither a null table nor
 * another protocol adds them.
 */
static void test_ubus_rows(void)
{
	const struct ubus_method *rows;
	unsigned int n;

	CASE("ubus rows: the SIP table adds register and unregister");
	cfg_protocol("sip");
	proto_select_from_cfg(&cfg);
	n = 0;
	rows = proto_ubus_methods(&n);
	CHECK_UINT(n, 2);
	CHECK(rows && !strcmp(rows[0].name, "register") &&
	      !strcmp(rows[1].name, "unregister"));

	CASE("ubus rows: the null table adds none");
	cfg_protocol(NULL_WORD);
	proto_select_from_cfg(&cfg);
	n = 1;
	CHECK(proto_ubus_methods(&n) == NULL);
	CHECK_UINT(n, 0);

#ifdef UVOICE_PROTO_MGCP
	CASE("ubus rows: the MGCP table adds no register row");
	cfg_protocol("mgcp");
	proto_select_from_cfg(&cfg);
	n = 1;
	CHECK(proto_ubus_methods(&n) == NULL);
	CHECK_UINT(n, 0);
#endif

	cfg_protocol("sip");
	proto_select_from_cfg(&cfg);
}

/* ----------------------------------------------- the lifecycle entries */

static void test_lifecycle(void)
{
	const struct ubus_method *rows;
	uint16_t first = 0, next = 0;
	unsigned int n = 0;

	sip_reset();
	cfg_protocol("sip");
	proto_select_from_cfg(&cfg);

	CASE("lifecycle: start and reload reach the table once each");
	proto_start(&cfg);
	proto_reload(&cfg, &cfg);
	CHECK_UINT(sip.start, 1);
	CHECK_UINT(sip.reload, 1);

	CASE("lifecycle: the table's ubus rows are the ones the core composes");
	rows = proto_ubus_methods(&n);
	CHECK_UINT(n, 2);
	CHECK(rows && !strcmp(rows[0].name, "register"));

	CASE("lifecycle: done reaches the table once, however often it runs");
	proto_done();
	proto_done();
	CHECK_UINT(sip.done, 1);

	CASE("lifecycle: a new start arms done again");
	proto_start(&cfg);
	proto_done();
	CHECK_UINT(sip.done, 2);

	CASE("lifecycle: done gives the RTP port pool back");
	proto_start(&cfg);
	CHECK(net_port_alloc(10000, 10030, &first));
	CHECK(net_port_alloc(10000, 10030, &next));
	CHECK(next != first);
	proto_done();
	CHECK(net_port_alloc(10000, 10030, &next));
	CHECK_UINT(next, first);
	net_port_reset();
}

/*
 * The graceful stop. proto_stop_wait() runs the drain the table reports and
 * returns on host->shutdown_done, or on its bound.
 */
static void test_stop_wait(void)
{
	uint32_t t0, waited;

	sip_reset();
	cfg_protocol("sip");
	proto_select_from_cfg(&cfg);
	proto_host_build();
	sip_init(proto_host_get(), &cfg);

	CASE("stop wait: a table with nothing pending returns at once");
	t0 = uvoice_mono_ms();
	proto_stop_wait();
	waited = uvoice_mono_ms() - t0;
	CHECK_UINT(sip.shutdown_begin, 1);
	CHECK(waited < 100);

	CASE("stop wait: the wait ends on host->shutdown_done");
	sip.pending = true;
	sip.report_ms = 50;
	t0 = uvoice_mono_ms();
	proto_stop_wait();
	waited = uvoice_mono_ms() - t0;
	CHECK_UINT(sip.shutdown_begin, 2);
	CHECK(!sip.pending);
	CHECK(waited >= 40 && waited < 1000);

	CASE("stop wait: a drain that never reports ends on the bound");
	sip.pending = true;
	sip.report_ms = 0;
	t0 = uvoice_mono_ms();
	proto_stop_wait();
	waited = uvoice_mono_ms() - t0;
	CHECK(sip.pending);
	CHECK(waited >= 2900 && waited < 4000);
	sip.pending = false;
}

/* ------------------------------------------------------- the host table */

static void test_host_build(void)
{
	const struct proto_host *h;
	static int marker;

	CASE("host: the build fills every entry the contract names");
	uv.hal = (struct hal *)&marker;
	proto_host_build();
	h = proto_host_get();
	CHECK(h->tone_play && h->tone_stop && h->ring_start && h->ring_stop);
	CHECK(h->cw_alert && h->cid_send && h->ring_template_match);
	CHECK(h->mwi_set && h->mwi_get && h->detector_arm);
	CHECK(h->fwd_disc_signal && h->fwd_disc_abort && h->battery_set);
	CHECK(h->digit_play && h->digit_play_cancel);
	CHECK(h->stream_stats_get && h->stats_live_query && h->stats_live_drop);
	CHECK(h->line_exists && h->line_in_service && h->hal_ready);
	CHECK(h->line_oos_force && h->line_oos_clear);
	CHECK(h->line_transition && h->line_settle && h->line_cfg_landed);
	CHECK(h->hal_features && h->hal_codecs && h->hal_cap && h->region);
	CHECK(h->conf_set && h->line_fault);
	CHECK(h->trace_active && h->trace_write && h->log);
	CHECK(h->cfg && h->config_generation && h->ubus_ctx &&
	      h->ubus_request_reply);
	CHECK(h->reg_state_post && h->tls_cert_start && h->tls_cert_stop);
	CHECK(h->net_iface_subscribe && h->net_iface_state_get &&
	      h->net_iface_bind_device && h->net_iface_source_host_probe &&
	      h->net_iface_ipv6_served_set && h->net_iface_recover_settle);
	CHECK(h->net_port_range && h->net_port_range_eq && h->net_port_alloc &&
	      h->net_port_free);
	CHECK(h->net_ct_udp_bound_s && h->net_ct_punch_interval_ms);
	CHECK(h->fw_ask && h->fw_clear && h->fw_proxy_add);
	CHECK(h->cdr_call_started && h->cdr_call_seq && h->cdr_call_correlate &&
	      h->cdr_call_answered && h->cdr_call_conferenced &&
	      h->cdr_call_ended);
	CHECK(h->calltrace_call_started && h->calltrace_event &&
	      h->calltrace_state && h->calltrace_tap_endpoint_set);
	CHECK(h->shutdown_done && h->random_bytes);

	CASE("host: the media priv is the backend of the build");
	CHECK(h->media.priv == (void *)&marker);

	/* more than one getrandom() call answers whole: the read returns at
	 * most 256 bytes from an initialized pool in one call */
	CASE("host: the entropy entry fills a buffer longer than one read");
	{
		static uint8_t buf[600];
		unsigned int i, zero = 0;

		memset(buf, 0, sizeof(buf));
		CHECK(h->random_bytes(h->ctx, buf, sizeof(buf)));
		for (i = 0; i < sizeof(buf); i++)
			zero += buf[i] == 0;
		CHECK(zero < sizeof(buf) / 8);
	}

	CASE("host: the config entry answers the running config");
	uv.cfg = &cfg;
	CHECK(h->cfg(h->ctx) == &cfg);
	uv.cfg = NULL;
	uv.hal = NULL;
}

/* ------------------------------------------------------ the MGCP table */

#ifdef UVOICE_PROTO_MGCP
static void cfg_mgcp_fill(const char *call_agent, unsigned int ca_port,
			  unsigned int local_port, const char *domain)
{
	struct cfg_mgcp *m = &cfg.mgcp;

	cfg_protocol("mgcp");
	cfg.channels = 2;
	cfg.channel[0].enabled = true;
	cfg.channel[1].enabled = true;
	snprintf(m->call_agent, sizeof(m->call_agent), "%s", call_agent);
	m->call_agent_port = ca_port;
	m->local_port = local_port;
	snprintf(m->domain, sizeof(m->domain), "%s", domain);
	snprintf(m->endpoint_pattern, sizeof(m->endpoint_pattern),
		 "aaln/{line}@{host}");
	m->line_base = 1;
	m->max1 = 5;
	m->max2 = 7;
	m->rto_init_ms = 200;
	m->rto_max_ms = 4000;
	m->t_max_s = 20;
	m->t_hist_s = 30;
	snprintf(m->persistent_events, sizeof(m->persistent_events),
		 "hd,hu,hf");
	m->send_ptime = true;
	m->signaling_dscp = 24;
}

static struct blob_attr *key_in(struct blob_attr *tbl, const char *key)
{
	struct blob_attr *cur;
	size_t rem;

	if (!tbl)
		return NULL;
	blobmsg_for_each_attr(cur, tbl, rem) {
		if (!strcmp(blobmsg_name(cur), key))
			return cur;
	}

	return NULL;
}

static struct blob_attr *top_key(struct blob_buf *b, const char *key)
{
	struct blob_attr *cur;
	size_t rem;

	blob_for_each_attr(cur, b->head, rem) {
		if (!strcmp(blobmsg_name(cur), key))
			return cur;
	}

	return NULL;
}

static bool str_key_is(struct blob_attr *tbl, const char *key,
		       const char *want)
{
	struct blob_attr *a = key_in(tbl, key);

	return a && blobmsg_type(a) == BLOBMSG_TYPE_STRING &&
	       !strcmp(blobmsg_get_string(a), want);
}

static bool bool_key_is(struct blob_attr *tbl, const char *key, bool want)
{
	struct blob_attr *a = key_in(tbl, key);

	return a && blobmsg_type(a) == BLOBMSG_TYPE_BOOL &&
	       blobmsg_get_bool(a) == want;
}

/* The first entry of an array, or NULL. */
static struct blob_attr *first_of(struct blob_attr *arr)
{
	struct blob_attr *cur;
	size_t rem;

	if (!arr)
		return NULL;
	blobmsg_for_each_attr(cur, arr, rem)
		return cur;

	return NULL;
}

static unsigned int count_of(struct blob_attr *arr)
{
	struct blob_attr *cur;
	unsigned int n = 0;
	size_t rem;

	if (!arr)
		return 0;
	blobmsg_for_each_attr(cur, arr, rem)
		n++;

	return n;
}

/* The call agent of the suite: a UDP socket on the loopback. */
static struct {
	struct uloop_fd ufd;
	char buf[2048];
	ssize_t len;
	struct sockaddr_in from;
	socklen_t from_len;
	uint16_t port;
} ca;

static void ca_read(struct uloop_fd *u, unsigned int events)
{
	ca.from_len = sizeof(ca.from);
	ca.len = recvfrom(u->fd, ca.buf, sizeof(ca.buf) - 1, 0,
			  (struct sockaddr *)&ca.from, &ca.from_len);
	if (ca.len <= 0)
		return;
	ca.buf[ca.len] = '\0';
	uloop_end();
}

static void ca_wait_end(struct uloop_timeout *t)
{
	uloop_end();
}

/* Run the loop until the call agent reads a datagram or ms pass. */
static bool ca_wait(int ms)
{
	struct uloop_timeout t = { .cb = ca_wait_end };

	ca.len = 0;
	uloop_timeout_set(&t, ms);
	uloop_cancelled = false;
	uloop_run();
	uloop_timeout_cancel(&t);

	return ca.len > 0;
}

/* Run the loop for ms at most; a socket of the suite can end it early. */
static void loop_for(int ms)
{
	struct uloop_timeout t = { .cb = ca_wait_end };

	uloop_timeout_set(&t, ms);
	uloop_cancelled = false;
	uloop_run();
	uloop_timeout_cancel(&t);
}

/* Run the loop in short slices until done() answers true. ms bounds a case
 * that fails; a case that passes ends at its event. */
static bool io_until(bool (*done)(void), int ms)
{
	int t;

	for (t = 0; t < ms && !done(); t += 5)
		loop_for(5);

	return done();
}

/*
 * The virtual clock of the MGCP unit (test_mgcp_clock.h). The suite binds it
 * after ops->init, so every timer of the unit fires inside fake_advance() and
 * no case waits for one. The sockets stay real.
 */
#define FAKE_WIRE_MAX		64

struct fake_send {
	char buf[FAKE_WIRE_MAX + 1];
	size_t len;
	char host[64];
	uint16_t port;
	uint64_t at_ms;
};

static struct {
	uint64_t now;
	struct proto_timer *timer[64];
	unsigned int timers;
	unsigned int fired;
	struct fake_send send[1];
	unsigned int sends;
	bool send_fail;
} fk;

#include "test_mgcp_clock.h"

/* uloop can run a timer up to 1 ms early; with clock_early set, every timer
 * of the unit runs 1 ms early. */
static bool clock_early;

static void early_timer_set(struct proto_timer_env *env, struct proto_timer *t,
			    unsigned int ms)
{
	fake_timer_set(env, t, ms ? ms - 1 : 0);
}

static void mgcp_clock_bind(void)
{
	struct proto_timer_env *env = mgcp_proto_test_timer_env();

	memset(&fk, 0, sizeof(fk));
	fk.now = 1000;
	fake_clock_bind(env);
	if (clock_early)
		env->timer_set = early_timer_set;
}

static bool in_service0(void)
{
	return mgcp_proto_ops.digit_play_allowed(0, 0);
}

static bool dns_idle(void)
{
	return sip_dns_query_count(mgcp_proto_test_dns()) == 0;
}

static bool ca_open(void)
{
	struct sockaddr_in sa;
	socklen_t slen = sizeof(sa);
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
	    getsockname(fd, (struct sockaddr *)&sa, &slen) < 0)
		return false;

	ca.port = ntohs(sa.sin_port);
	ca.ufd.fd = fd;
	ca.ufd.cb = ca_read;

	return uloop_fd_add(&ca.ufd, ULOOP_READ) == 0;
}

static void ca_close(void)
{
	uloop_fd_delete(&ca.ufd);
	close(ca.ufd.fd);
}

/* No domain option and no local address: the endpoints wait for a name. */
static void test_mgcp_waiting(const struct proto_ops *ops, struct blob_buf *b)
{
	CASE("mgcp table: the firewall ask names each line port and the call "
	     "agent");
	CHECK(phf.n[PHF_FW_ASK] >= 1);
	CHECK_UINT(phf.fw_ask.active_mask, 3);
	CHECK_UINT(phf.fw_ask.port[0], 2427);
	CHECK_UINT(phf.fw_ask.port[1], 2427);
	CHECK(phf.fw_ask.proto_udp);
	CHECK_UINT(phf.fw_ask.nproxy, 1);
	CHECK(!strcmp(phf.fw_ask.proxy[0], "192.0.2.1"));

	CASE("mgcp table: without a name the lines array is empty");
	blob_buf_init(b, 0);
	ops->status_dump_lines(b, -1);
	CHECK(top_key(b, "lines") != NULL);
	CHECK_UINT(count_of(top_key(b, "lines")), 0);
	blob_buf_init(b, 0);
	ops->status_dump_call(b, 0);
	CHECK(blob_len(b->head) > 0);
	CHECK(!strcmp(ops->state_name(0), "down"));

	CASE("mgcp table: the top table shows a socket that waits");
	blob_buf_init(b, 0);
	ops->status_dump_top(b);
	CHECK(str_key_is(top_key(b, "mgcp"), "socket", "waiting"));
	CHECK(str_key_is(top_key(b, "mgcp"), "dialect", "rfc3435"));
	CHECK(str_key_is(top_key(b, "mgcp"), "notified_entity",
			 "192.0.2.1:2727"));
}

static void test_mgcp_wire(const struct proto_ops *ops, struct blob_buf *b)
{
	struct net_iface_state st;
	char ans[64];
	unsigned long txid;
	int n;

	CASE("mgcp table: the socket sends the RSIP to the call agent");
	CHECK(ca_open());
	memset(&st, 0, sizeof(st));
	snprintf(st.addr4, sizeof(st.addr4), "127.0.0.1");
	st.addr4_known = true;
	cfg_mgcp_fill("127.0.0.1", ca.port, 0, "");
	proto_host_fake_reset();
	phf.cfg = &cfg;
	phf.iface = &st;
	ops->init(proto_host_fake(), &cfg);
	mgcp_clock_bind();
	ops->start(&cfg);
	fake_advance(0);
	CHECK(ca_wait(2000));
	CHECK(!strncmp(ca.buf, "RSIP ", 5));
	CHECK(strstr(ca.buf, " aaln/*@127.0.0.1 MGCP 1.0\r\n") != NULL);
	CHECK(strstr(ca.buf, "RM: restart\r\n") != NULL);

	CASE("mgcp table: lines[] holds one endpoint row per line");
	blob_buf_init(b, 0);
	ops->status_dump_lines(b, -1);
	CHECK_UINT(count_of(top_key(b, "lines")), 2);
	CHECK(str_key_is(first_of(top_key(b, "lines")), "endpoint",
			 "aaln/1@127.0.0.1"));
	CHECK(str_key_is(first_of(top_key(b, "lines")), "service",
			 "restarting"));
	CHECK(str_key_is(first_of(top_key(b, "lines")), "request_id", "0"));

	CASE("mgcp table: the 200 of the call agent puts the endpoints in "
	     "service");
	txid = strtoul(ca.buf + 5, NULL, 10);
	n = snprintf(ans, sizeof(ans), "200 %lu OK\r\n", txid);
	CHECK(sendto(ca.ufd.fd, ans, (size_t)n, 0,
		     (struct sockaddr *)&ca.from, ca.from_len) == n);
	CHECK(io_until(in_service0, 2000));
	blob_buf_init(b, 0);
	ops->status_dump_lines(b, 0);
	CHECK(str_key_is(first_of(top_key(b, "lines")), "service",
			 "in_service"));
	blob_buf_init(b, 0);
	ops->status_dump_top(b);
	CHECK(str_key_is(top_key(b, "mgcp"), "socket", "bound"));

	CASE("mgcp table: the stop waits for no transaction once answered");
	ops->shutdown_begin();
	CHECK(!ops->shutdown_pending());
	ops->done();
	ca_close();
}

/* A UDP socket on the wildcard address and a free port, which takes that port
 * from the unit. */
static int port_block(uint16_t *port)
{
	struct sockaddr_in sa;
	socklen_t slen = sizeof(sa);
	int fd = socket(AF_INET, SOCK_DGRAM, 0);

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
	    getsockname(fd, (struct sockaddr *)&sa, &slen) < 0)
		return -1;

	*port = ntohs(sa.sin_port);

	return fd;
}

static void test_mgcp_bind(const struct proto_ops *ops, struct blob_buf *b)
{
	struct net_iface_state st;
	uint16_t port = 0;
	int fd;

	CASE("mgcp table: a bind failure on a healthy interface settles the "
	     "recovery");
	CHECK(ca_open());
	fd = port_block(&port);
	CHECK(fd >= 0);
	memset(&st, 0, sizeof(st));
	snprintf(st.addr4, sizeof(st.addr4), "127.0.0.1");
	st.addr4_known = true;
	cfg_mgcp_fill("127.0.0.1", ca.port, port, "");
	proto_host_fake_reset();
	phf.cfg = &cfg;
	phf.iface = &st;
	ops->init(proto_host_fake(), &cfg);
	mgcp_clock_bind();
	ops->start(&cfg);
	CHECK(phf.n[PHF_NET_IFACE_RECOVER_SETTLE] >= 1);
	CHECK(phf.sub_set && phf.sub.down_count);
	CHECK_UINT(phf.sub.down_count(phf.sub.priv), 1);
	blob_buf_init(b, 0);
	ops->status_dump_top(b);
	CHECK(str_key_is(top_key(b, "mgcp"), "socket", "failed"));

	CASE("mgcp table: a bind failure puts the endpoints in the "
	     "disconnected procedure");
	CHECK(bool_key_is(top_key(b, "mgcp"), "disconnected", true));
	blob_buf_init(b, 0);
	ops->status_dump_lines(b, 0);
	CHECK(str_key_is(first_of(top_key(b, "lines")), "service",
			 "disconnected"));

	CASE("mgcp table: the recovery reopens a freed port with no interface "
	     "event");
	close(fd);
	phf.sub.recover(phf.sub.priv);
	CHECK_UINT(phf.sub.down_count(phf.sub.priv), 0);
	blob_buf_init(b, 0);
	ops->status_dump_top(b);
	CHECK(str_key_is(top_key(b, "mgcp"), "socket", "bound"));
	CHECK(ca_wait(2000));
	CHECK(!strncmp(ca.buf, "RSIP ", 5));

	ops->shutdown_begin();
	ops->done();
	ca_close();
}

/*
 * The DNS server of the suite on the loopback: it counts the questions and
 * keeps the virtual time of each. It answers SERVFAIL while fail is set,
 * else A 127.0.0.1 to an A question, and A 127.0.0.2 beside it while two is
 * set. While v6 is set it answers AAAA ::1 and no A record, and while mute
 * is set it answers nothing.
 */
static struct {
	struct uloop_fd ufd;
	uint16_t port;
	unsigned int asked;
	uint64_t at[8];
	bool fail;
	bool two;
	bool v6;
	bool mute;
} fdns;

static void fdns_read(struct uloop_fd *u, unsigned int events)
{
	static const uint8_t rr_a[] = { 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60,
					0, 4, 127, 0, 0, 1 };
	static const uint8_t rr_aaaa[] = { 0xc0, 0x0c, 0, 28, 0, 1, 0, 0, 0, 60,
					   0, 16, 0, 0, 0, 0, 0, 0, 0, 0,
					   0, 0, 0, 0, 0, 0, 0, 1 };
	struct sockaddr_in from;
	socklen_t flen = sizeof(from);
	uint8_t q[512], r[512 + 2 * sizeof(rr_aaaa)];
	size_t off = 12, len;
	ssize_t n;

	n = recvfrom(u->fd, q, sizeof(q), 0, (struct sockaddr *)&from, &flen);
	if (n < 12)
		return;
	while (off < (size_t)n && q[off])
		off += q[off] + 1u;
	/* the root label, QTYPE and QCLASS */
	off += 5;
	if (off > (size_t)n)
		return;

	if (fdns.asked < 8)
		fdns.at[fdns.asked] = fk.now;
	fdns.asked++;
	if (fdns.mute)
		return;
	memcpy(r, q, off);
	r[2] = 0x81;
	r[3] = fdns.fail ? 0x82 : 0x80;
	memset(r + 6, 0, 6);
	len = off;
	if (fdns.v6 && q[off - 4] == 0 && q[off - 3] == 28) {
		memcpy(r + len, rr_aaaa, sizeof(rr_aaaa));
		len += sizeof(rr_aaaa);
		r[7] = 1;
	} else if (!fdns.fail && !fdns.v6 && q[off - 4] == 0 &&
		   q[off - 3] == 1) {
		memcpy(r + len, rr_a, sizeof(rr_a));
		len += sizeof(rr_a);
		r[7] = 1;
	}
	if (r[7] && fdns.two) {
		memcpy(r + len, rr_a, sizeof(rr_a));
		r[len + sizeof(rr_a) - 1] = 2;
		len += sizeof(rr_a);
		r[7] = 2;
	}
	sendto(u->fd, r, len, 0, (struct sockaddr *)&from, flen);
}

static bool fdns_open(void)
{
	struct sockaddr_in sa;
	socklen_t slen = sizeof(sa);
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);

	memset(&fdns, 0, sizeof(fdns));
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
	    getsockname(fd, (struct sockaddr *)&sa, &slen) < 0)
		return false;

	fdns.port = ntohs(sa.sin_port);
	fdns.ufd.fd = fd;
	fdns.ufd.cb = fdns_read;

	return uloop_fd_add(&fdns.ufd, ULOOP_READ) == 0;
}

static void fdns_close(void)
{
	uloop_fd_delete(&fdns.ufd);
	close(fdns.ufd.fd);
}

/* The unit up to its start on the call agent name ca.test, whose resolver
 * asks the DNS server of the suite. */
static void mgcp_named_init(const struct proto_ops *ops, const char *domain)
{
	struct sip_txn_dst srv;

	cfg_mgcp_fill("ca.test", ca.port, 0, domain);
	proto_host_fake_reset();
	phf.cfg = &cfg;
	phf.probe_addr = "127.0.0.1";
	ops->init(proto_host_fake(), &cfg);
	mgcp_clock_bind();
	if (sip_dst_set(&srv, "127.0.0.1", fdns.port))
		sip_dns_servers_set(mgcp_proto_test_dns(), &srv, 1);
}

static void mgcp_named_start(const struct proto_ops *ops, const char *domain)
{
	mgcp_named_init(ops, domain);
	ops->start(&cfg);
	CHECK(io_until(dns_idle, 2000));
}

static bool endpoints_up(void)
{
	return strcmp(mgcp_proto_ops.state_name(0), "down") != 0;
}

/* The questions that fill the query pool of the resolver all arrived. */
static bool fdns_fill_seen(void)
{
	return fdns.asked >= SIP_DNS_QUERY_MAX;
}

/* The virtual clock moves by ms in steps of 100 ms. Each step lets a query
 * that a timer sent reach the DNS server of the suite and end. */
static void clock_walk(unsigned int ms)
{
	unsigned int t, step;

	for (t = 0; t < ms; t += step) {
		step = ms - t < 100 ? ms - t : 100;
		fake_advance(step);
		CHECK(io_until(dns_idle, 2000));
	}
}

/* The spacing of the first four queries: 5 s, then 10 s, then 20 s. */
static bool fdns_ladder_is(void)
{
	return fdns.asked >= 4 && fdns.at[1] - fdns.at[0] == 5000 &&
	       fdns.at[2] - fdns.at[1] == 10000 &&
	       fdns.at[3] - fdns.at[2] == 20000;
}

/* The retry of a failed call agent name at its edges: the timer, the
 * kinds of failure, the stop and an interface event. */
static void test_mgcp_dns_edges(const struct proto_ops *ops)
{
	uint32_t fill[SIP_DNS_QUERY_MAX];
	struct net_iface_change ch;
	struct net_iface_state st;
	unsigned int i;

	CASE("mgcp table: the retry asks at its time, not a millisecond "
	     "before");
	fdns.fail = true;
	fdns.asked = 0;
	mgcp_named_start(ops, "");
	clock_walk(4999);
	CHECK_UINT(fdns.asked, 1);
	clock_walk(1);
	CHECK_UINT(fdns.asked, 2);
	CHECK_UINT(fdns.at[1] - fdns.at[0], 5000);
	clock_walk(1);
	CHECK_UINT(fdns.asked, 2);
	ops->done();

	CASE("mgcp table: a retry timer that runs 1 ms early still asks, and "
	     "the next retry follows");
	clock_early = true;
	fdns.asked = 0;
	mgcp_named_start(ops, "");
	clock_walk(4999);
	CHECK_UINT(fdns.asked, 2);
	CHECK_UINT(fdns.at[1] - fdns.at[0], 4999);
	clock_walk(9999);
	CHECK_UINT(fdns.asked, 3);
	ops->done();
	clock_early = false;

	CASE("mgcp table: an answer with no IPv4 address arms the retry");
	fdns.fail = false;
	fdns.v6 = true;
	fdns.asked = 0;
	mgcp_named_init(ops, "");
	sip_dns_ipv6_set(mgcp_proto_test_dns(), true);
	ops->start(&cfg);
	CHECK(io_until(dns_idle, 2000));
	CHECK(fdns.asked >= 1);
	CHECK_UINT(mgcp_proto_test_ca_retry_ms(), 5000);
	CHECK(!endpoints_up());
	ops->done();
	fdns.v6 = false;

	CASE("mgcp table: a query the resolver refuses arms the retry");
	fdns.fail = true;
	fdns.mute = true;
	fdns.asked = 0;
	mgcp_named_init(ops, "");
	for (i = 0; i < SIP_DNS_QUERY_MAX; i++)
		fill[i] = sip_dns_resolve(mgcp_proto_test_dns(), "fill.test",
					  SIP_DNS_ADDR, 0, 0);
	ops->start(&cfg);
	CHECK_UINT(mgcp_proto_test_ca_retry_ms(), 5000);
	CHECK(io_until(fdns_fill_seen, 2000));
	for (i = 0; i < SIP_DNS_QUERY_MAX; i++)
		sip_dns_cancel(mgcp_proto_test_dns(), fill[i]);
	fdns.mute = false;
	fdns.asked = 0;
	clock_walk(5000);
	CHECK_UINT(fdns.asked, 1);
	ops->done();

	CASE("mgcp table: the retry asks nothing once the stop began");
	fdns.asked = 0;
	mgcp_named_start(ops, "");
	ops->shutdown_begin();
	clock_walk(5000);
	CHECK_UINT(fdns.asked, 1);
	ops->done();

	CASE("mgcp table: an interface event after a failed first answer "
	     "brings the endpoints up");
	fdns.asked = 0;
	mgcp_named_start(ops, "");
	CHECK(!endpoints_up());
	memset(&st, 0, sizeof(st));
	memset(&ch, 0, sizeof(ch));
	snprintf(st.addr4, sizeof(st.addr4), "127.0.0.1");
	st.addr4_known = true;
	ch.addr4 = true;
	CHECK(phf.sub_set && phf.sub.changed);
	phf.sub.changed(phf.sub.priv, &st, &ch);
	CHECK(io_until(dns_idle, 2000));
	CHECK(endpoints_up());
	CHECK_UINT(fdns.asked, 2);
	CHECK_UINT(mgcp_proto_test_ca_retry_ms(), 5000);
	ops->done();
}

static void test_mgcp_dns(const struct proto_ops *ops, struct blob_buf *b)
{
	unsigned int n0, t;

	CASE("mgcp table: a failed call agent query arms a retry at 5 s");
	CHECK(ca_open());
	CHECK(fdns_open());
	fdns.fail = true;
	mgcp_named_start(ops, "");
	CHECK_UINT(fdns.asked, 1);
	CHECK_UINT(mgcp_proto_test_ca_retry_ms(), 5000);
	blob_buf_init(b, 0);
	ops->status_dump_lines(b, -1);
	CHECK_UINT(count_of(top_key(b, "lines")), 0);

	CASE("mgcp table: the retries of a failing call agent name double");
	clock_walk(4900);
	CHECK_UINT(fdns.asked, 1);
	clock_walk(100);
	CHECK_UINT(fdns.asked, 2);
	CHECK_UINT(mgcp_proto_test_ca_retry_ms(), 10000);
	clock_walk(10000);
	CHECK_UINT(fdns.asked, 3);

	CASE("mgcp table: the retry that gets an answer brings the endpoints "
	     "up with no interface event");
	fdns.fail = false;
	clock_walk(20000);
	CHECK(fdns_ladder_is());
	CHECK_UINT(mgcp_proto_test_ca_retry_ms(), 0);
	fake_advance(0);
	CHECK(ca_wait(2000));
	CHECK(!strncmp(ca.buf, "RSIP ", 5));
	CHECK(strstr(ca.buf, " aaln/*@127.0.0.1 MGCP 1.0\r\n") != NULL);

	CASE("mgcp table: a name that answered starts its next ladder at 5 s");
	/* the RSIP gets no answer, and its Max1 asks the name again */
	fdns.fail = true;
	n0 = fdns.asked;
	for (t = 0; fdns.asked == n0 && t < 60000; t += 100)
		clock_walk(100);
	CHECK(fdns.asked > n0);
	CHECK_UINT(mgcp_proto_test_ca_retry_ms(), 5000);
	ops->done();

	CASE("mgcp table: every path of a failing call agent name waits for "
	     "its retry");
	fdns.fail = true;
	fdns.asked = 0;
	mgcp_named_start(ops, "gw.test");
	clock_walk(35000);
	CHECK_UINT(fdns.asked, 4);
	CHECK(fdns_ladder_is());
	ops->done();

	test_mgcp_dns_edges(ops);

	CHECK_UINT(fake_timer_dropped, 0);
	fdns_close();
	ca_close();
}

/* ------------------------------------------- call agents that keep a log */

#define PEER_LOG_MAX	48

/* A call agent socket of the suite. It keeps every datagram it reads with
 * its TOS byte, and it ends the loop run at want datagrams. */
struct peer {
	struct uloop_fd ufd;
	struct sockaddr_in at;
	struct sockaddr_in from;
	unsigned int n;
	unsigned int want;
	char log[PEER_LOG_MAX][512];
	int tos[PEER_LOG_MAX];
};

static struct peer pa, pz;

static int cmsg_tos(struct msghdr *mh)
{
	struct cmsghdr *cm;

	for (cm = CMSG_FIRSTHDR(mh); cm; cm = CMSG_NXTHDR(mh, cm)) {
		if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_TOS)
			return *(unsigned char *)CMSG_DATA(cm);
	}

	return -1;
}

static void peer_read(struct uloop_fd *u, unsigned int events)
{
	struct peer *p = container_of(u, struct peer, ufd);
	char cbuf[CMSG_SPACE(sizeof(int))], buf[2048];
	struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) - 1 };
	struct msghdr mh;
	ssize_t n;

	for (;;) {
		memset(&mh, 0, sizeof(mh));
		mh.msg_name = &p->from;
		mh.msg_namelen = sizeof(p->from);
		mh.msg_iov = &iov;
		mh.msg_iovlen = 1;
		mh.msg_control = cbuf;
		mh.msg_controllen = sizeof(cbuf);
		n = recvmsg(u->fd, &mh, 0);
		if (n <= 0)
			return;
		if ((size_t)n >= sizeof(p->log[0]))
			n = sizeof(p->log[0]) - 1;
		if (p->n < PEER_LOG_MAX) {
			memcpy(p->log[p->n], buf, (size_t)n);
			p->log[p->n][n] = '\0';
			p->tos[p->n] = cmsg_tos(&mh);
		}
		p->n++;
		if (p->want && p->n >= p->want)
			uloop_end();
	}
}

static bool peer_open(struct peer *p, const char *addr, uint16_t port)
{
	socklen_t slen = sizeof(p->at);
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	int on = 1;

	memset(p, 0, sizeof(*p));
	p->at.sin_family = AF_INET;
	p->at.sin_port = htons(port);
	if (fd < 0 || inet_pton(AF_INET, addr, &p->at.sin_addr) != 1 ||
	    setsockopt(fd, IPPROTO_IP, IP_RECVTOS, &on, sizeof(on)) < 0 ||
	    bind(fd, (struct sockaddr *)&p->at, sizeof(p->at)) < 0 ||
	    getsockname(fd, (struct sockaddr *)&p->at, &slen) < 0)
		return false;

	p->ufd.fd = fd;
	p->ufd.cb = peer_read;

	return uloop_fd_add(&p->ufd, ULOOP_READ) == 0;
}

static void peer_close(struct peer *p)
{
	uloop_fd_delete(&p->ufd);
	close(p->ufd.fd);
}

static uint16_t peer_port(const struct peer *p)
{
	return ntohs(p->at.sin_port);
}

/* Run the loop until p holds n datagrams; ms bounds a case that fails. */
static void peer_wait(struct peer *p, unsigned int n, int ms)
{
	if (p->n >= n)
		return;
	p->want = n;
	loop_for(ms);
	p->want = 0;
}

/* Read what the unit sent to p: a send of the unit is complete on the
 * loopback when its call returns, so no loop has to run. */
static void peer_drain(struct peer *p)
{
	peer_read(&p->ufd, 0);
}

static bool stop_done(void)
{
	return phf.n[PHF_SHUTDOWN_DONE] > 0;
}

static uint32_t txid_in(const char *text)
{
	const char *sp = strchr(text, ' ');

	return sp ? (uint32_t)strtoul(sp + 1, NULL, 10) : 0;
}

/* The first datagram of p from index from on that starts with prefix and
 * holds needle (NULL: any), or -1. */
static int peer_find(const struct peer *p, unsigned int from,
		     const char *prefix, const char *needle)
{
	unsigned int i;

	for (i = from; i < p->n && i < PEER_LOG_MAX; i++) {
		if (strncmp(p->log[i], prefix, strlen(prefix)))
			continue;
		if (!needle || strstr(p->log[i], needle))
			return (int)i;
	}

	return -1;
}

static unsigned int peer_count(const struct peer *p, const char *prefix,
			       uint32_t txid)
{
	unsigned int i, n = 0;

	for (i = 0; i < p->n && i < PEER_LOG_MAX; i++) {
		if (!strncmp(p->log[i], prefix, strlen(prefix)) &&
		    txid_in(p->log[i]) == txid)
			n++;
	}

	return n;
}

static void peer_send(struct peer *p, const struct sockaddr_in *to,
		      const char *text)
{
	sendto(p->ufd.fd, text, strlen(text), 0, (const struct sockaddr *)to,
	       sizeof(*to));
}

/* The 200 to datagram i of p, toward the unit that sent it. */
static void peer_ok(struct peer *p, int i)
{
	char ans[64];

	if (i < 0)
		return;
	snprintf(ans, sizeof(ans), "200 %u OK\r\n", txid_in(p->log[i]));
	peer_send(p, &p->from, ans);
}

/* The unit on the literal call agent pa, endpoints aaln/N@gw.test. */
static void mgcp_literal_start(const struct proto_ops *ops,
			       enum cfg_mgcp_reply reply)
{
	cfg_mgcp_fill("127.0.0.1", peer_port(&pa), 0, "gw.test");
	cfg.mgcp.reply_method = reply;
	proto_host_fake_reset();
	phf.cfg = &cfg;
	phf.hal_codecs = (1u << HAL_CODEC_PCMU) | (1u << HAL_CODEC_PCMA);
	ops->init(proto_host_fake(), &cfg);
	mgcp_clock_bind();
	ops->start(&cfg);
	fake_advance(0);
	peer_wait(&pa, 1, 2000);
}

/* A command of pa that gets its 200. */
static bool pa_cmd_ok(const char *text)
{
	unsigned int n0 = pa.n;
	char want[16];

	peer_send(&pa, &pa.from, text);
	peer_wait(&pa, n0 + 1, 1000);
	snprintf(want, sizeof(want), "200 %u ", txid_in(text));

	return peer_find(&pa, n0, want, NULL) >= 0;
}

static void test_mgcp_ops(const struct proto_ops *ops)
{
	unsigned int n0, landed;
	uint32_t first;
	int i, j;

	CASE("mgcp table: the first transaction id of the daemon is a random "
	     "one");
	CHECK(peer_open(&pa, "127.0.0.1", 0));
	CHECK(peer_open(&pz, "127.0.0.1", 0));
	mgcp_literal_start(ops, CFG_MGCP_REPLY_MGC_PORT);
	CHECK(pa.n >= 1 && !strncmp(pa.log[0], "RSIP ", 5));
	first = txid_in(pa.log[0]);
	CHECK(first != 0 && first != 1);

	CASE("mgcp table: the signaling DSCP is the TOS byte of a datagram");
	CHECK_UINT(pa.tos[0], 24 << 2);

	CASE("mgcp table: a digit toward the phone passes on a line in "
	     "service");
	peer_ok(&pa, 0);
	CHECK(io_until(in_service0, 2000));

	CASE("mgcp table: under mgc_port a response goes to the call agent "
	     "port");
	n0 = pa.n;
	peer_send(&pz, &pa.from, "AUEP 901 aaln/1@gw.test MGCP 1.0\r\n");
	peer_wait(&pa, n0 + 1, 1000);
	CHECK(peer_find(&pa, n0, "200 901 ", NULL) >= 0);
	CHECK_UINT(pz.n, 0);

	CASE("mgcp table: a fault sends DLCX with E: 900 and RSIP forced");
	CHECK(pa_cmd_ok("CRCX 902 aaln/1@gw.test MGCP 1.0\r\nC: A1\r\n"
			"M: recvonly\r\n"));
	CHECK(pa_cmd_ok("CRCX 903 aaln/2@gw.test MGCP 1.0\r\nC: A2\r\n"
			"M: recvonly\r\n"));
	n0 = pa.n;
	ops->line_service(1, false, PROTO_SERVICE_FAULT);
	peer_wait(&pa, n0 + 2, 1000);
	i = peer_find(&pa, n0, "DLCX ", "E: 900");
	j = peer_find(&pa, n0, "RSIP ", "RM: forced");
	CHECK(i >= 0 && j >= 0);
	peer_ok(&pa, i);
	peer_ok(&pa, j);

	CASE("mgcp table: a digit toward the phone does not pass out of "
	     "service");
	CHECK(!ops->digit_play_allowed(1, 0));
	CHECK(ops->digit_play_allowed(0, 0));

	CASE("mgcp table: the staged config waits for a busy endpoint and "
	     "lands on an idle one");
	landed = phf.n[PHF_LINE_CFG_LANDED];
	ops->line_cfg_changed(0);
	CHECK_UINT(phf.n[PHF_LINE_CFG_LANDED], landed);
	ops->line_cfg_changed(1);
	CHECK_UINT(phf.n[PHF_LINE_CFG_LANDED], landed + 1);

	CASE("mgcp table: the stop sends RSIP graceful for a line with a "
	     "connection");
	n0 = pa.n;
	ops->shutdown_begin();
	peer_wait(&pa, n0 + 1, 1000);
	i = peer_find(&pa, n0, "RSIP ", "RM: graceful");
	CHECK(i >= 0);
	CHECK(ops->shutdown_pending());

	CASE("mgcp table: the end of the last transaction reports the stop "
	     "done");
	CHECK_UINT(phf.n[PHF_SHUTDOWN_DONE], 0);
	peer_ok(&pa, i);
	CHECK(io_until(stop_done, 2000));
	CHECK_UINT(phf.n[PHF_SHUTDOWN_DONE], 1);
	CHECK(!ops->shutdown_pending());

	ops->done();
	peer_close(&pz);
	peer_close(&pa);
}

static void test_mgcp_reload(const struct proto_ops *ops)
{
	static struct cfg old;
	uint32_t last = 0;
	unsigned int k, n0;
	int i;

	CASE("mgcp table: a reload with a changed option restarts the unit");
	CHECK(peer_open(&pa, "127.0.0.1", 0));
	mgcp_literal_start(ops, CFG_MGCP_REPLY_SOURCE);
	peer_ok(&pa, 0);
	CHECK(io_until(in_service0, 2000));
	old = cfg;
	cfg.mgcp.max1 = 4;
	n0 = pa.n;
	ops->reload(&old, &cfg);
	fake_advance(0);
	peer_drain(&pa);
	i = peer_find(&pa, n0, "RSIP ", "RM: restart");
	CHECK(i >= 0);

	CASE("mgcp table: the first id after a reload is above the ids before "
	     "it");
	for (k = 0; i >= 0 && k < (unsigned int)i; k++) {
		if (strncmp(pa.log[k], "RSIP ", 5))
			continue;
		if (txid_in(pa.log[k]) > last)
			last = txid_in(pa.log[k]);
	}
	CHECK(last != 0);
	CHECK(i >= 0 && txid_in(pa.log[i]) > last);

	ops->done();
	peer_close(&pa);
}

/* The virtual clock moves by ms, and the two call agents read what the
 * timers sent. */
static void clock_peers(unsigned int ms, struct peer *a, struct peer *b)
{
	fake_advance(ms);
	peer_drain(a);
	peer_drain(b);
}

/*
 * A call agent name with two addresses, on the virtual clock. The first
 * command of an endpoint meets Max1 and moves to the second address. A later
 * command of the same endpoint keeps the first address until its own Max1.
 * Timers: RTO 400 ms, fixed by rto_init_ms and rto_max_ms, and no sample,
 * because the restart gets its answer after a retransmission. The NTFY
 * starts at t, the RSIP at t + 200 ms. Max1 meets the NTFY at t + 1200 ms
 * and the RSIP at t + 1400 ms.
 */
static void test_mgcp_max1(const struct proto_ops *ops)
{
	struct proto_line_event ev = { .type = PROTO_EV_HOOK };
	static struct peer py;
	struct peer *p1, *p2;
	struct sip_txn_dst srv;
	unsigned int t, n1, n2;
	uint32_t forced;
	int i;

	CASE("mgcp table: Max1 moves the one command that reached it to the "
	     "next address");
	CHECK(fdns_open());
	fdns.two = true;
	CHECK(peer_open(&pa, "127.0.0.1", 0));
	CHECK(peer_open(&py, "127.0.0.2", peer_port(&pa)));
	cfg_mgcp_fill("ca.test", peer_port(&pa), 0, "gw.test");
	cfg.mgcp.max1 = 2;
	cfg.mgcp.rto_init_ms = 400;
	cfg.mgcp.rto_max_ms = 400;
	proto_host_fake_reset();
	phf.cfg = &cfg;
	ops->init(proto_host_fake(), &cfg);
	mgcp_clock_bind();
	if (sip_dst_set(&srv, "127.0.0.1", fdns.port))
		sip_dns_servers_set(mgcp_proto_test_dns(), &srv, 1);
	ops->start(&cfg);
	CHECK(io_until(dns_idle, 2000));
	clock_peers(0, &pa, &py);
	clock_peers(400, &pa, &py);
	p1 = pa.n ? &pa : &py;
	p2 = p1 == &pa ? &py : &pa;
	CHECK(p1->n == 2 && p2->n == 0);
	peer_ok(p1, 1);
	CHECK(io_until(in_service0, 2000));

	ev.u.hook.state = HAL_HOOK_SEIZE;
	ops->line_event(0, &ev);
	clock_peers(200, &pa, &py);
	ops->line_service(0, false, PROTO_SERVICE_CFG);
	for (t = 200; t < 1300; t += 100)
		clock_peers(100, &pa, &py);
	CHECK(peer_find(p2, 0, "NTFY ", NULL) >= 0);
	i = peer_find(p1, 0, "RSIP ", "RM: forced");
	CHECK(i >= 0);
	forced = i >= 0 ? txid_in(p1->log[i]) : 0;

	CASE("mgcp table: the other command of the endpoint keeps its address "
	     "and its count");
	CHECK_UINT(peer_count(p1, "RSIP ", forced), 3);
	CHECK_UINT(peer_count(p2, "RSIP ", forced), 0);
	clock_peers(100, &pa, &py);
	CHECK_UINT(peer_count(p2, "RSIP ", forced), 1);

	CASE("mgcp table: Max1 on the last address asks no name answered less "
	     "than 5 s ago");
	/* the NTFY meets Max1 on the second address at t + 2400 ms */
	for (t = 1400; t < 2500; t += 100)
		clock_peers(100, &pa, &py);
	CHECK(io_until(dns_idle, 2000));
	CHECK_UINT(fdns.asked, 1);

	CASE("mgcp table: a new command of the endpoint goes to the address "
	     "its name moved to");
	/* the name answered 400 ms before t; t + 3600 ms is 5 s after it */
	for (; t < 3600; t += 100)
		clock_peers(100, &pa, &py);
	n1 = p1->n;
	n2 = p2->n;
	ops->line_service(1, false, PROTO_SERVICE_CFG);
	clock_peers(0, &pa, &py);
	CHECK(peer_find(p2, n2, "RSIP ", " aaln/2@gw.test ") >= 0);
	CHECK(peer_find(p1, n1, "RSIP ", " aaln/2@gw.test ") < 0);

	CASE("mgcp table: Max1 or Max2 on the last address asks the name "
	     "again once it is due");
	for (; t < 4900; t += 100)
		clock_peers(100, &pa, &py);
	CHECK(io_until(dns_idle, 2000));
	CHECK_UINT(fdns.asked, 2);

	ops->done();
	peer_close(&py);
	peer_close(&pa);
	fdns_close();
}

static void test_mgcp_table(void)
{
	const struct proto_ops *ops = &mgcp_proto_ops;
	struct blob_buf b;

	memset(&b, 0, sizeof(b));

	CASE("mgcp table: every mandatory entry is present");
	CHECK(proto_ops_complete(ops));

	CASE("lifecycle: the start reaches the MGCP table and not the SIP one");
	cfg_mgcp_fill("192.0.2.1", 2727, 2427, "");
	proto_select_from_cfg(&cfg);
	proto_host_fake_reset();
	phf.cfg = &cfg;
	ops->init(proto_host_fake(), &cfg);
	sip_reset();
	proto_start(&cfg);
	CHECK_UINT(phf.n[PHF_NET_IFACE_SUBSCRIBE], 1);
	CHECK_UINT(sip.start, 0);

	test_mgcp_waiting(ops, &b);
	ops->done();

	test_mgcp_wire(ops, &b);
	test_mgcp_bind(ops, &b);
	test_mgcp_dns(ops, &b);
	test_mgcp_ops(ops);
	test_mgcp_reload(ops);
	test_mgcp_max1(ops);
	blob_buf_free(&b);

	cfg_protocol("sip");
	proto_select_from_cfg(&cfg);
}
#endif

int main(void)
{
	uloop_init();

	test_registry();
	test_complete();
	test_state();
	test_null_table();
	test_ubus_rows();
#ifdef UVOICE_PROTO_MGCP
	test_mgcp_table();
#endif
	test_lifecycle();
	test_host_build();
	test_stop_wait();

	uloop_done();

	return tu_summary(SUITE_NAME);
}
