// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The protocol registry of proto/proto.h: the ops tables the build carries, the
 * null table that stands while no protocol runs, the state the signaling
 * table of "voice status" reports, the entry points main.c calls, and the one
 * host table every protocol reaches the core through.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>

#include "calltrace.h"
#include "cdr.h"
#include "event.h"
#include "fw.h"
#include "line/line.h"
#include "net/conntrack.h"
#include "net/fw_reconcile.h"
#include "net/iface.h"
#include "net/ports.h"
#include "proto/proto.h"
#include "str.h"
#include "trace_ring.h"
#include "uvoice.h"

/* The bound of the graceful stop: the Expires 0 window of SIP. */
#define PROTO_STOP_TIMEOUT_MS	3000

/* the protocol word of the default, which an empty config word names */
#define PROTO_NAME_DEFAULT	"sip"

/* -------------------------------------------------------- the null table */

/*
 * The table that stands while no protocol runs: every mandatory entry is
 * present and inert, so the core calls every entry without a test. The fifth
 * service reason of the line shell holds every line out of service under it
 * (line/line.c line_serve_want), and the one subscriber interaction it answers
 * is a seize: it plays reorder until the release.
 */
static const struct proto_host *null_host;

static void null_init(const struct proto_host *host, const struct cfg *cfg)
{
	null_host = host;
}

static void null_start(const struct cfg *cfg)
{
}

static void null_reload(const struct cfg *old_cfg, const struct cfg *new_cfg)
{
}

static void null_shutdown_begin(void)
{
}

static bool null_shutdown_pending(void)
{
	return false;
}

static void null_done(void)
{
	null_host = NULL;
}

static void null_line_event(unsigned int line,
			    const struct proto_line_event *ev)
{
	if (!null_host || ev->type != PROTO_EV_HOOK)
		return;

	if (ev->u.hook.state == HAL_HOOK_SEIZE)
		null_host->tone_play(null_host->ctx, line, HAL_TONE_REORDER, 0);
	else if (ev->u.hook.state == HAL_HOOK_RELEASE)
		null_host->tone_stop(null_host->ctx, line);
}

static void null_line_service(unsigned int line, bool in_service,
			      enum proto_service_reason reason)
{
}

static void null_line_reset(unsigned int line)
{
}

static uint32_t null_line_flags(unsigned int line)
{
	return 0;
}

static void null_status_dump_call(struct blob_buf *b, unsigned int line)
{
}

static void null_status_dump_lines(struct blob_buf *b, int want_line)
{
	void *arr = blobmsg_open_array(b, "lines");

	blobmsg_close_array(b, arr);
}

static const struct proto_ops proto_null_ops = {
	.name = "none",
	.init = null_init,
	.start = null_start,
	.reload = null_reload,
	.shutdown_begin = null_shutdown_begin,
	.shutdown_pending = null_shutdown_pending,
	.done = null_done,
	.line_event = null_line_event,
	.line_service = null_line_service,
	.line_reset = null_line_reset,
	.line_flags = null_line_flags,
	.status_dump_call = null_status_dump_call,
	.status_dump_lines = null_status_dump_lines,
};

/* ------------------------------------------------------------ the registry */

static const struct proto_ops *const proto_table[] = {
#ifdef UVOICE_PROTO_SIP
	&sip_proto_ops,
#endif
#ifdef UVOICE_PROTO_MGCP
	&mgcp_proto_ops,
#endif
	NULL,
};

static struct {
	const struct proto_ops *cur;
	char requested[CFG_PROTOCOL_MAX];
	const char *error;
	bool running;
	bool done;
	struct uloop_timeout stop_timer;
} pr = {
	.cur = &proto_null_ops,
};

bool proto_ops_complete(const struct proto_ops *ops)
{
	return ops && ops->name && ops->init && ops->start && ops->reload &&
	       ops->done && ops->line_event && ops->line_service &&
	       ops->line_reset && ops->line_flags && ops->status_dump_call &&
	       ops->status_dump_lines && ops->shutdown_begin &&
	       ops->shutdown_pending;
}

static const struct proto_ops *proto_find(const char *name)
{
	unsigned int i;

	for (i = 0; proto_table[i]; i++) {
		if (!strcmp(proto_table[i]->name, name))
			return proto_table[i];
	}

	return NULL;
}

const struct proto_ops *proto_select(const char *name)
{
	const struct proto_ops *ops = name ? proto_find(name) : NULL;

	return proto_ops_complete(ops) ? ops : NULL;
}

const struct proto_ops *proto_select_from_cfg(const struct cfg *cfg)
{
	const char *name = cfg && cfg->protocol[0] ? cfg->protocol :
			   PROTO_NAME_DEFAULT;
	const struct proto_ops *ops = proto_find(name);

	str_copy(pr.requested, sizeof(pr.requested), name);

	if (!ops) {
		pr.error = "the image does not carry this protocol";
		ULOG_ERR("signaling protocol %s is not built into this image, "
			 "every line stays out of service\n", name);
	} else if (!proto_ops_complete(ops)) {
		pr.error = "the protocol table lacks a mandatory entry";
		ULOG_ERR("signaling protocol %s lacks a mandatory entry, every "
			 "line stays out of service\n", name);
		ops = NULL;
	}

	if (!ops) {
		pr.cur = &proto_null_ops;
		pr.running = false;
		return pr.cur;
	}

	pr.cur = ops;
	pr.error = NULL;
	pr.running = true;
	ULOG_INFO("signaling protocol %s\n", name);

	return pr.cur;
}

const struct proto_ops *proto_ops_get(void)
{
	return pr.cur;
}

bool proto_running(void)
{
	return pr.running;
}

void proto_start(const struct cfg *cfg)
{
	pr.done = false;
	pr.cur->start(cfg);
}

void proto_reload(const struct cfg *old_cfg, const struct cfg *new_cfg)
{
	pr.cur->reload(old_cfg, new_cfg);
}

static void proto_stop_expire(struct uloop_timeout *t)
{
	ULOG_WARN("signaling stop did not complete in %u ms\n",
		  PROTO_STOP_TIMEOUT_MS);
	uloop_end();
}

/*
 * Graceful stop on SIGTERM. The protocol clears its calls and starts its own
 * drain (the Expires 0 of SIP), and the loop runs a bounded window until the
 * protocol reports its end through host->shutdown_done or the window closes.
 * It runs before hal_close, so the sockets are still open.
 */
void proto_stop_wait(void)
{
	pr.cur->shutdown_begin();

	if (!pr.cur->shutdown_pending())
		return;

	pr.stop_timer.cb = proto_stop_expire;
	uloop_timeout_set(&pr.stop_timer, PROTO_STOP_TIMEOUT_MS);

	uloop_cancelled = false;
	uloop_run();

	uloop_timeout_cancel(&pr.stop_timer);
}

/*
 * The protocol frees what it holds once, after the HAL closed, and the port
 * pool and the rules go with it. The loop runs no chain after the stop, so
 * the removal is the synchronous one of fw_shutdown() and not the chain of
 * fw_reconcile_clear().
 */
void proto_done(void)
{
	if (!pr.done) {
		pr.cur->done();
		pr.done = true;
	}

	net_port_reset();
	fw_shutdown();
}

/*
 * The signaling table: the protocol that runs, or the one the config named
 * and the image does not run, the state, the reason when unavailable, and the
 * protocols the image carries.
 */
void proto_status_dump(struct blob_buf *b)
{
	void *tbl, *arr;
	unsigned int i;

	tbl = blobmsg_open_table(b, "signaling");
	blobmsg_add_string(b, "protocol",
			   pr.running ? pr.cur->name : pr.requested);
	blobmsg_add_string(b, "state", pr.running ? "running" : "unavailable");
	if (!pr.running && pr.error)
		blobmsg_add_string(b, "error", pr.error);

	arr = blobmsg_open_array(b, "available");
	for (i = 0; proto_table[i]; i++)
		blobmsg_add_string(b, NULL, proto_table[i]->name);
	blobmsg_close_array(b, arr);

	blobmsg_close_table(b, tbl);
}

const struct ubus_method *proto_ubus_methods(unsigned int *n)
{
	*n = 0;

	if (!pr.cur->ubus_methods)
		return NULL;

	return pr.cur->ubus_methods(n);
}

/* ---------------------------------------------------------- the host table */

static enum hal_status host_tone_play(void *ctx, unsigned int line,
				      enum hal_tone tone, uint32_t duration_ms)
{
	return line_tone_play(line, tone, duration_ms);
}

static void host_tone_stop(void *ctx, unsigned int line)
{
	line_tone_stop(line);
}

static void host_ring_start(void *ctx, unsigned int line,
			    enum proto_ring_kind kind,
			    unsigned int ring_template, uint32_t duration_ms,
			    const struct proto_cid *cid)
{
	line_ringer_start(line, kind, ring_template, duration_ms, cid);
}

static void host_ring_stop(void *ctx, unsigned int line)
{
	line_ringer_stop(line);
}

static void host_cw_alert(void *ctx, unsigned int line, bool on,
			  unsigned int variant, const struct proto_cid *cid)
{
	line_cw_alert(line, on, variant, cid);
}

static enum hal_status host_cid_send(void *ctx, unsigned int line,
				     enum hal_cid_kind kind,
				     const struct proto_cid *cid)
{
	return line_cid_send(line, kind, cid);
}

static unsigned int host_ring_template_match(void *ctx, unsigned int line,
					     const char *number)
{
	return line_ring_template_match(line, number);
}

static void host_mwi_set(void *ctx, unsigned int line, bool on,
			 uint32_t new_msgs, uint32_t old_msgs)
{
	line_mwi_set(line, on, new_msgs, old_msgs);
}

static bool host_mwi_get(void *ctx, unsigned int line, uint32_t *new_msgs,
			 uint32_t *old_msgs)
{
	return line_mwi_get(line, new_msgs, old_msgs);
}

static bool host_detector_arm(void *ctx, unsigned int line,
			      uint32_t detect_mask)
{
	return line_detector_arm(line, detect_mask);
}

static bool host_fwd_disc_signal(void *ctx, unsigned int line)
{
	return line_fwd_disc_signal(line);
}

static void host_fwd_disc_abort(void *ctx, unsigned int line)
{
	line_fwd_disc_abort(line);
}

static enum hal_status host_battery_set(void *ctx, unsigned int line,
					enum proto_battery batt)
{
	return line_battery_set(line, batt);
}

static bool host_digit_play(void *ctx, unsigned int line, unsigned int stream,
			    char digit, uint16_t duration_ms, bool dedup)
{
	return line_digit_play(line, stream, digit, duration_ms, dedup);
}

static void host_digit_play_cancel(void *ctx, unsigned int line,
				   unsigned int stream)
{
	line_digit_play_cancel(line, stream);
}

static enum hal_status host_stream_stats_get(void *ctx, unsigned int line,
					     unsigned int stream,
					     struct hal_media_stats *out)
{
	return line_stream_stats_get(line, stream, out);
}

static enum hal_status host_stats_live_query(void *ctx, unsigned int line,
					     unsigned int stream,
					     proto_stats_cb cb, void *priv)
{
	switch (line_stats_live_query(line, stream, cb, priv)) {
	case LINE_STATS_OK:
		return HAL_OK;
	case LINE_STATS_BUSY:
		return HAL_TRANSIENT;
	case LINE_STATS_REFUSED:
		return HAL_FATAL;
	case LINE_STATS_NO_LINES:
	case LINE_STATS_NO_LINE:
	case LINE_STATS_NO_STREAM:
		break;
	}

	return HAL_UNSUPPORTED;
}

static void host_stats_live_drop(void *ctx, unsigned int line,
				 unsigned int stream)
{
	line_stats_live_drop(line, stream);
}

static bool host_line_exists(void *ctx, unsigned int line)
{
	return line_exists(line);
}

static bool host_line_in_service(void *ctx, unsigned int line)
{
	return line_call_admit(line);
}

static bool host_hal_ready(void *ctx)
{
	return line_hal_ready();
}

static void host_line_oos_force(void *ctx, unsigned int line,
				const char *reason)
{
	line_service_hold(line, reason);
}

static void host_line_oos_clear(void *ctx, unsigned int line)
{
	line_service_release(line);
}

static void host_line_transition(void *ctx, unsigned int line,
				 unsigned int state_id, uint32_t flags)
{
	line_transition(line, state_id, flags);
}

static void host_line_settle(void *ctx, unsigned int line)
{
	line_settle(line);
}

static void host_line_cfg_landed(void *ctx, unsigned int line)
{
	line_cfg_landed(line);
}

static uint32_t host_hal_features(void *ctx)
{
	return lines_hal_cap()->features;
}

static uint32_t host_hal_codecs(void *ctx)
{
	return lines_hal_cap()->codecs;
}

static const struct hal_cap *host_hal_cap(void *ctx)
{
	return lines_hal_cap();
}

static const struct hal_region *host_region(void *ctx)
{
	return lines_region();
}

static enum hal_status host_conf_set(void *ctx, unsigned int line, bool on)
{
	return line_conf_set(line, on);
}

static void host_line_fault(void *ctx, unsigned int line, bool loud,
			    const char *text)
{
	line_fault(line, loud, text);
}

static bool host_trace_active(void *ctx, enum trace_domain dom)
{
	return trace_domain_active(dom);
}

static void host_trace_write(void *ctx, enum trace_domain dom,
			     const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void host_trace_write(void *ctx, enum trace_domain dom,
			     const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	trace_write(dom, "%s", buf);
}

static void host_log(void *ctx, int level, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void host_log(void *ctx, int level, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	ulog(level, "%s", buf);
}

static const struct cfg *host_cfg(void *ctx)
{
	return uv.cfg;
}

static uint32_t host_config_generation(void *ctx)
{
	return uv.config_generation;
}

static struct ubus_context *host_ubus_ctx(void *ctx)
{
	return uvoice_ubus_ctx();
}

static void host_ubus_request_reply(void *ctx, struct ubus_context *ubus,
				    struct ubus_request_data *req,
				    unsigned int line, const char *verb)
{
	uvoice_ubus_request_reply(ubus, req, line, verb);
}

static void host_reg_state_post(void *ctx, unsigned int line, bool up,
				const char *proxy, bool moved)
{
	event_reg_post(line, up, proxy);
	if (moved)
		calltrace_event(line, "registration");
}

static void host_tls_cert_start(void *ctx, const char *url,
				const char *username, const char *password,
				bool verify, void (*done)(void))
{
	event_tls_cert_start(url, username, password, verify, done);
}

static void host_tls_cert_stop(void *ctx, enum proto_tls_stop mode)
{
	event_tls_cert_stop(mode == PROTO_TLS_STOP_SHUTDOWN ?
			    EVENT_TLS_STOP_SHUTDOWN : EVENT_TLS_STOP_CONFIG);
}

static void host_net_iface_subscribe(void *ctx,
				     const struct net_iface_sub *sub)
{
	net_iface_subscribe(sub);
}

static const struct net_iface_state *host_net_iface_state_get(void *ctx)
{
	return net_iface_state_get();
}

static const char *host_net_iface_bind_device(void *ctx, const char *iface)
{
	return net_iface_bind_device(iface);
}

static bool host_net_iface_source_host_probe(void *ctx, const char *dev,
					     int family,
					     const struct sockaddr *sa,
					     socklen_t salen, char *out,
					     size_t olen)
{
	return net_iface_source_host_probe(dev, family, sa, salen, out, olen);
}

static void host_net_iface_ipv6_served_set(void *ctx, bool served)
{
	net_iface_ipv6_served_set(served);
}

static void host_net_iface_recover_settle(void *ctx)
{
	net_iface_recover_settle();
}

static void host_net_port_range(void *ctx, const struct cfg_channel *ch,
				uint16_t *min, uint16_t *max)
{
	net_port_range(ch, min, max);
}

static bool host_net_port_range_eq(void *ctx, const struct cfg_channel *a,
				   const struct cfg_channel *b)
{
	return net_port_range_eq(a, b);
}

static bool host_net_port_alloc(void *ctx, unsigned int min,
				unsigned int max, uint16_t *out)
{
	return net_port_alloc(min, max, out);
}

static void host_net_port_free(void *ctx, uint16_t port)
{
	net_port_free(port);
}

static unsigned int host_net_ct_udp_bound_s(void *ctx)
{
	return net_ct_udp_bound_s();
}

static uint16_t host_net_ct_punch_interval_ms(void *ctx)
{
	return net_ct_punch_interval_ms();
}

static void host_fw_ask(void *ctx, const struct fw_sig_ask *ask)
{
	fw_reconcile_ask(ask);
}

static void host_fw_clear(void *ctx)
{
	fw_reconcile_clear();
}

static unsigned int host_fw_proxy_add(void *ctx,
				      char (*list)[PROTO_FW_SRC_MAX],
				      unsigned int count, const char *addr)
{
	return fw_reconcile_proxy_add(list, count, addr);
}

static uint32_t host_cdr_call_started(void *ctx, unsigned int line,
				      unsigned int stream, bool inbound)
{
	return cdr_call_started(line, stream, inbound);
}

static uint32_t host_cdr_call_seq(void *ctx, unsigned int line,
				  unsigned int stream)
{
	return cdr_call_seq(line, stream);
}

static void host_cdr_call_correlate(void *ctx, unsigned int line,
				    unsigned int stream,
				    unsigned int from_stream)
{
	cdr_call_correlate(line, stream, from_stream);
}

static void host_cdr_call_answered(void *ctx, unsigned int line,
				   unsigned int stream)
{
	cdr_call_answered(line, stream);
}

static void host_cdr_call_conferenced(void *ctx, unsigned int line,
				      unsigned int stream)
{
	cdr_call_conferenced(line, stream);
}

static void host_cdr_call_ended(void *ctx, unsigned int line,
				unsigned int stream,
				const struct proto_call_ended *info)
{
	cdr_call_ended(line, stream, info);
}

static void host_calltrace_call_started(void *ctx, unsigned int line,
					uint32_t cdr_seq)
{
	calltrace_call_started(line, cdr_seq);
}

static void host_calltrace_event(void *ctx, unsigned int line,
				 const char *reason)
{
	calltrace_event(line, reason);
}

static void host_calltrace_state(void *ctx, unsigned int line,
				 const char *before, const char *after)
{
	calltrace_line_state(line, before, after);
}

static void host_calltrace_tap_endpoint_set(void *ctx,
					    struct trace_endpoint *out,
					    const struct sockaddr *sa)
{
	trace_endpoint_set(out, sa);
}

/* The protocol's graceful stop ended: the wait of proto_stop_wait() ends. */
static void host_shutdown_done(void *ctx)
{
	uloop_end();
}

static bool host_random_bytes(void *ctx, void *buf, size_t len)
{
	return uvoice_random_bytes(buf, len);
}

static struct proto_host host;

void proto_host_build(void)
{
	memset(&host, 0, sizeof(host));

	host.tone_play = host_tone_play;
	host.tone_stop = host_tone_stop;
	host.ring_start = host_ring_start;
	host.ring_stop = host_ring_stop;
	host.cw_alert = host_cw_alert;
	host.cid_send = host_cid_send;
	host.ring_template_match = host_ring_template_match;
	host.mwi_set = host_mwi_set;
	host.mwi_get = host_mwi_get;
	host.detector_arm = host_detector_arm;
	host.fwd_disc_signal = host_fwd_disc_signal;
	host.fwd_disc_abort = host_fwd_disc_abort;
	host.battery_set = host_battery_set;
	host.digit_play = host_digit_play;
	host.digit_play_cancel = host_digit_play_cancel;
	host.media = *line_media_ops();
	host.media.priv = uv.hal;
	host.stream_stats_get = host_stream_stats_get;
	host.stats_live_query = host_stats_live_query;
	host.stats_live_drop = host_stats_live_drop;
	host.line_exists = host_line_exists;
	host.line_in_service = host_line_in_service;
	host.hal_ready = host_hal_ready;
	host.line_oos_force = host_line_oos_force;
	host.line_oos_clear = host_line_oos_clear;
	host.line_transition = host_line_transition;
	host.line_settle = host_line_settle;
	host.line_cfg_landed = host_line_cfg_landed;
	host.hal_features = host_hal_features;
	host.hal_codecs = host_hal_codecs;
	host.hal_cap = host_hal_cap;
	host.region = host_region;
	host.conf_set = host_conf_set;
	host.line_fault = host_line_fault;
	host.trace_active = host_trace_active;
	host.trace_write = host_trace_write;
	host.log = host_log;
	host.cfg = host_cfg;
	host.config_generation = host_config_generation;
	host.ubus_ctx = host_ubus_ctx;
	host.ubus_request_reply = host_ubus_request_reply;
	host.reg_state_post = host_reg_state_post;
	host.tls_cert_start = host_tls_cert_start;
	host.tls_cert_stop = host_tls_cert_stop;
	host.net_iface_subscribe = host_net_iface_subscribe;
	host.net_iface_state_get = host_net_iface_state_get;
	host.net_iface_bind_device = host_net_iface_bind_device;
	host.net_iface_source_host_probe = host_net_iface_source_host_probe;
	host.net_iface_ipv6_served_set = host_net_iface_ipv6_served_set;
	host.net_iface_recover_settle = host_net_iface_recover_settle;
	host.net_port_range = host_net_port_range;
	host.net_port_range_eq = host_net_port_range_eq;
	host.net_port_alloc = host_net_port_alloc;
	host.net_port_free = host_net_port_free;
	host.net_ct_udp_bound_s = host_net_ct_udp_bound_s;
	host.net_ct_punch_interval_ms = host_net_ct_punch_interval_ms;
	host.fw_ask = host_fw_ask;
	host.fw_clear = host_fw_clear;
	host.fw_proxy_add = host_fw_proxy_add;
	host.cdr_call_started = host_cdr_call_started;
	host.cdr_call_seq = host_cdr_call_seq;
	host.cdr_call_correlate = host_cdr_call_correlate;
	host.cdr_call_answered = host_cdr_call_answered;
	host.cdr_call_conferenced = host_cdr_call_conferenced;
	host.cdr_call_ended = host_cdr_call_ended;
	host.calltrace_call_started = host_calltrace_call_started;
	host.calltrace_event = host_calltrace_event;
	host.calltrace_state = host_calltrace_state;
	host.calltrace_tap_endpoint_set = host_calltrace_tap_endpoint_set;
	host.shutdown_done = host_shutdown_done;
	host.random_bytes = host_random_bytes;
}

const struct proto_host *proto_host_get(void)
{
	return &host;
}
