// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The SIP protocol behind struct proto_ops (proto/proto.h): the lifecycle of
 * the account layer (account.c) and the leg layer (call.c), the two account
 * verbs of the voice object, the status dumps and the wire hooks of the call
 * trace. The machines of the lines are those of proto/sip/line.c. The core
 * reaches this table through proto_ops_get() alone.
 */
#include <string.h>

#include <libubox/blobmsg.h>
#include <libubox/utils.h>
#include <libubus.h>

#include "proto/proto.h"
#include "proto/shared/sdp.h"
#include "proto/sip/account.h"
#include "proto/sip/call.h"
#include "proto/sip/line.h"
#include "proto/sip/tap.h"
#include "proto/sip/transport.h"

/* the signaling port a channel with no config listens and sends on */
#define SIP_WIRE_PORT_DEFAULT	5060

/* The DSP readiness and the service state the leg layer admits against
 * (call.h), answered by the core. */
static bool sip_hal_ready(void)
{
	return sip_host->hal_ready(sip_host->ctx);
}

static bool sip_line_admit(unsigned int line)
{
	return sip_host->line_in_service(sip_host->ctx, line);
}

static void sip_init(const struct proto_host *host, const struct cfg *cfg)
{
	sip_lines_init(host, cfg);
	sip_wire_host = host;
	accounts_host_bind(host);

	/* calls_init() takes no config; the leg layer reads it through
	 * calls_reload(), and a call_dial before that would refuse silently */
	calls_init(&host->media, sip_line_call_cb());
	/* after calls_init(), which clears the bindings (call.h) */
	calls_host_bind(host);
	calls_reload(cfg);
	calls_hal_ready_bind(sip_hal_ready);
	calls_line_admit_bind(sip_line_admit);
}

static void sip_start(const struct cfg *cfg)
{
	accounts_init();
	accounts_start(cfg);
}

/*
 * The codec mask rides the feature push because it has the same lifetime and
 * the same probe behind it. The account layer takes the same mask: a line
 * registers over IPv6 only on a backend that carries IPv6 media.
 */
static void sip_dsp_ready(uint32_t features, uint32_t codecs)
{
	const struct hal_cap *cap = sip_host->hal_cap(sip_host->ctx);

	calls_hal_features_set(features);
	accounts_hal_features_set(features);
	calls_hal_codecs_set(codecs);
	sdp_t38_max_datagram_set(cap->t38_max_datagram);
	sip_lines_dsp_ready();
}

static void sip_reload(const struct cfg *old_cfg, const struct cfg *new_cfg)
{
	accounts_reload(old_cfg, new_cfg);
	calls_reload(new_cfg);
}

static void sip_shutdown_begin(void)
{
	sip_lines_stop_begin();
	accounts_shutdown_begin();
}

/* The legs ended at lines_done(), so the leg layer has nothing left to end. */
static void sip_done(void)
{
	accounts_done();
	calls_done();
}

/* The SIP machine reads no late signal result: its tones and its bell do not
 * report back. */
static void sip_signal_result(unsigned int line, enum proto_signal sig,
			      bool ok)
{
}

static uint8_t sip_line_fax_kind(unsigned int line, bool *media_running)
{
	return calls_line_fax_kind(line, media_running);
}

enum {
	SIP_LINE_ARG,
	__SIP_LINE_MAX,
};

static const struct blobmsg_policy sip_line_policy[__SIP_LINE_MAX] = {
	[SIP_LINE_ARG] = { "line", BLOBMSG_TYPE_INT32 },
};

static bool sip_line_arg_get(struct blob_attr *msg, uint32_t *line)
{
	struct blob_attr *tb[__SIP_LINE_MAX] = {};

	if (msg)
		blobmsg_parse(sip_line_policy, __SIP_LINE_MAX, tb,
			      blob_data(msg), blob_len(msg));
	if (!tb[SIP_LINE_ARG])
		return false;

	*line = blobmsg_get_u32(tb[SIP_LINE_ARG]);

	return true;
}

/*
 * The two account command verbs. They answer with the line and what was asked
 * for, under "requested": the daemon accepted a command whose outcome voice
 * status reports.
 */
static int sip_ubus_register(struct ubus_context *ctx, struct ubus_object *obj,
			     struct ubus_request_data *req, const char *method,
			     struct blob_attr *msg)
{
	uint32_t line;

	if (!sip_line_arg_get(msg, &line))
		return UBUS_STATUS_INVALID_ARGUMENT;
	if (!account_register_cmd(line))
		return UBUS_STATUS_NOT_FOUND;

	sip_host->ubus_request_reply(sip_host->ctx, ctx, req, line,
				     "register");

	return 0;
}

static int sip_ubus_unregister(struct ubus_context *ctx,
			       struct ubus_object *obj,
			       struct ubus_request_data *req,
			       const char *method, struct blob_attr *msg)
{
	uint32_t line;

	if (!sip_line_arg_get(msg, &line))
		return UBUS_STATUS_INVALID_ARGUMENT;
	if (!account_unregister_cmd(line))
		return UBUS_STATUS_NOT_FOUND;

	sip_host->ubus_request_reply(sip_host->ctx, ctx, req, line,
				     "unregister");

	return 0;
}

static const struct ubus_method sip_ubus_rows[] = {
	UBUS_METHOD("register", sip_ubus_register, sip_line_policy),
	UBUS_METHOD("unregister", sip_ubus_unregister, sip_line_policy),
};

static const struct ubus_method *sip_ubus_methods(unsigned int *n)
{
	*n = ARRAY_SIZE(sip_ubus_rows);

	return sip_ubus_rows;
}

static void sip_status_dump_call(struct blob_buf *b, unsigned int line)
{
	sip_line_status_dump(b, line);
	calls_status_dump(b, line);
}

static void sip_status_dump_media(struct blob_buf *b)
{
	blobmsg_add_u32(b, "opus_dtmf_fallbacks", calls_opus_dtmf_fallbacks());
}

/*
 * The capture filter ports of one line: the local SIP port catches every
 * exchange whatever port the far end sends from, and the configured remote
 * port catches an outbound stream connection from an ephemeral local port.
 */
static unsigned int sip_wire_ports(unsigned int line, uint16_t *ports,
				   unsigned int max)
{
	const struct cfg *cfg = sip_host->cfg(sip_host->ctx);
	uint16_t local = SIP_WIRE_PORT_DEFAULT;
	uint16_t remote = SIP_WIRE_PORT_DEFAULT;
	unsigned int n = 0;

	if (cfg && line < CFG_CHANNEL_MAX) {
		local = cfg->channel[line].sip_local_port;
		remote = cfg->channel[line].sip_port;
	}

	if (n < max)
		ports[n++] = local;
	if (remote != local && n < max)
		ports[n++] = remote;

	return n;
}

/* The datagram pool's own counters. The pool serves every line, so these are
 * the daemon's totals and not the traced line's. */
static void sip_wire_stats(struct blob_buf *b)
{
	const struct sip_sock_stat *st = sip_transport_stat(acct_transport());
	void *tbl;

	if (!st)
		return;

	tbl = blobmsg_open_table(b, "sip");
	blobmsg_add_u64(b, "rx", st->rx);
	blobmsg_add_u64(b, "rx_bytes", st->rx_bytes);
	blobmsg_add_u64(b, "tx", st->tx);
	blobmsg_add_u64(b, "tx_bytes", st->tx_bytes);
	blobmsg_add_u64(b, "tx_err", st->tx_err);
	blobmsg_add_u64(b, "rx_err", st->rx_err);
	blobmsg_add_u64(b, "parse_err", st->parse_err);
	blobmsg_add_u64(b, "truncated", st->truncated);
	blobmsg_add_u64(b, "ping", st->ping);
	blobmsg_close_table(b, tbl);
}

const struct proto_ops sip_proto_ops = {
	.name = "sip",
	.init = sip_init,
	.start = sip_start,
	.dsp_ready = sip_dsp_ready,
	.reload = sip_reload,
	.shutdown_begin = sip_shutdown_begin,
	.shutdown_pending = accounts_shutdown_pending,
	.done = sip_done,
	.line_event = sip_line_event,
	.line_service = sip_line_service,
	.line_reset = sip_line_reset,
	.line_cfg_changed = sip_line_cfg_changed,
	.region_changed = sip_line_region_changed,
	.ring_held = sip_line_ring_held,
	.ring_started = sip_line_ring_started,
	.ring_failed = sip_line_ring_failed,
	.signal_result = sip_signal_result,
	.line_flags = sip_line_flags,
	.state_name = sip_line_state_name,
	.digit_play_allowed = sip_line_digit_play_allowed,
	.stream_of_slot = sip_line_stream_of_slot,
	.line_fax_kind = sip_line_fax_kind,
	.ubus_methods = sip_ubus_methods,
	.status_dump_call = sip_status_dump_call,
	.status_dump_lines = accounts_status_dump,
	.status_dump_media = sip_status_dump_media,
	.wire_ports = sip_wire_ports,
	.tap_set = sip_tap_set,
	.wire_stats = sip_wire_stats,
};
