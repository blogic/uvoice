// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The recorder host table of proto_host_fake.h.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <libubox/uloop.h>

#include "proto_host_fake.h"
#include "str.h"
#include "trace.h"
#include "trace_ring.h"

#ifdef PHF_NET_PORTS
#include "net/ports.h"
#endif

#ifdef PHF_NET_CORE
#include "calltrace.h"
#include "event.h"
#include "net/conntrack.h"
#include "net/fw_reconcile.h"
#include "net/iface.h"
#endif

struct phf_rec phf;

static void phf_line(unsigned int line)
{
	phf.line = line;
}

/* The next record of the call sequence; a sink past the end of hop[]. */
static struct phf_hop *phf_hop_add(enum phf_entry entry, unsigned int line)
{
	static struct phf_hop sink;
	struct phf_hop *h = phf.nhop < PHF_HOP_MAX ? &phf.hop[phf.nhop++] :
						     &sink;

	memset(h, 0, sizeof(*h));
	h->entry = (uint8_t)entry;
	h->line = line;

	return h;
}

static void phf_hop_cid(struct phf_hop *h, const struct proto_cid *cid)
{
	if (!cid)
		return;

	h->cid_set = true;
	h->cid = *cid;
}

/* ------------------------------------------------------------- the line */

static enum hal_status phf_tone_play(void *ctx, unsigned int line,
				     enum hal_tone tone, uint32_t duration_ms)
{
	struct phf_hop *h = phf_hop_add(PHF_TONE_PLAY, line);

	phf.n[PHF_TONE_PLAY]++;
	phf_line(line);
	phf.tone = tone;
	h->val = tone;
	h->duration_ms = duration_ms;

	return phf.status;
}

static void phf_tone_stop(void *ctx, unsigned int line)
{
	phf_hop_add(PHF_TONE_STOP, line);
	phf.n[PHF_TONE_STOP]++;
	phf_line(line);
}

static void phf_ring_start(void *ctx, unsigned int line,
			   enum proto_ring_kind kind,
			   unsigned int ring_template, uint32_t duration_ms,
			   const struct proto_cid *cid)
{
	struct phf_hop *h = phf_hop_add(PHF_RING_START, line);

	phf.n[PHF_RING_START]++;
	phf_line(line);
	phf.ring_kind = kind;
	h->val = ring_template;
	h->duration_ms = duration_ms;
	phf_hop_cid(h, cid);
}

static void phf_ring_stop(void *ctx, unsigned int line)
{
	phf_hop_add(PHF_RING_STOP, line);
	phf.n[PHF_RING_STOP]++;
	phf_line(line);
}

static void phf_cw_alert(void *ctx, unsigned int line, bool on,
			 unsigned int variant, const struct proto_cid *cid)
{
	struct phf_hop *h = phf_hop_add(PHF_CW_ALERT, line);

	phf.n[PHF_CW_ALERT]++;
	phf_line(line);
	phf.on = on;
	h->on = on;
	h->val = variant;
	phf_hop_cid(h, cid);
}

static enum hal_status phf_cid_send(void *ctx, unsigned int line,
				    enum hal_cid_kind kind,
				    const struct proto_cid *cid)
{
	struct phf_hop *h = phf_hop_add(PHF_CID_SEND, line);

	phf.n[PHF_CID_SEND]++;
	phf_line(line);
	h->val = kind;
	phf_hop_cid(h, cid);

	return phf.status;
}

static unsigned int phf_ring_template_match(void *ctx, unsigned int line,
					    const char *number)
{
	phf.n[PHF_RING_TEMPLATE_MATCH]++;
	phf_line(line);

	return phf.ring_template;
}

static void phf_mwi_set(void *ctx, unsigned int line, bool on,
			uint32_t new_msgs, uint32_t old_msgs)
{
	struct phf_hop *h = phf_hop_add(PHF_MWI_SET, line);

	phf.n[PHF_MWI_SET]++;
	phf_line(line);
	phf.on = on;
	h->on = on;
}

static bool phf_mwi_get(void *ctx, unsigned int line, uint32_t *new_msgs,
			uint32_t *old_msgs)
{
	phf.n[PHF_MWI_GET]++;
	phf_line(line);
	if (new_msgs)
		*new_msgs = 0;
	if (old_msgs)
		*old_msgs = 0;

	return false;
}

static bool phf_detector_arm(void *ctx, unsigned int line,
			     uint32_t detect_mask)
{
	phf_hop_add(PHF_DETECTOR_ARM, line)->val = detect_mask;
	phf.n[PHF_DETECTOR_ARM]++;
	phf_line(line);

	return phf.accept;
}

static bool phf_fwd_disc_signal(void *ctx, unsigned int line)
{
	phf.n[PHF_FWD_DISC_SIGNAL]++;
	phf_line(line);

	return phf.accept;
}

static void phf_fwd_disc_abort(void *ctx, unsigned int line)
{
	phf.n[PHF_FWD_DISC_ABORT]++;
	phf_line(line);
}

static enum hal_status phf_battery_set(void *ctx, unsigned int line,
				       enum proto_battery batt)
{
	phf_hop_add(PHF_BATTERY_SET, line)->val = batt;
	phf.n[PHF_BATTERY_SET]++;
	phf_line(line);

	return phf.status;
}

static bool phf_digit_play(void *ctx, unsigned int line, unsigned int stream,
			   char digit, uint16_t duration_ms, bool dedup)
{
	struct phf_hop *h = phf_hop_add(PHF_DIGIT_PLAY, line);

	phf.n[PHF_DIGIT_PLAY]++;
	phf_line(line);
	phf.stream = stream;
	phf.digit = digit;
	h->stream = stream;
	h->digit = digit;
	h->duration_ms = duration_ms;

	if (!phf.accept)
		return false;
	if (!phf.digit_active) {
		phf.digit_active = true;
		return true;
	}
	if (phf.digit_q >= phf.digit_q_max)
		return false;
	phf.digit_q++;

	return true;
}

static void phf_digit_play_cancel(void *ctx, unsigned int line,
				  unsigned int stream)
{
	phf_hop_add(PHF_DIGIT_PLAY_CANCEL, line)->stream = stream;
	phf.n[PHF_DIGIT_PLAY_CANCEL]++;
	phf_line(line);
	phf.stream = stream;
	phf.digit_q = 0;
}

/* ------------------------------------------------------------- the media */

static enum hal_status phf_fail_take(unsigned int *count)
{
	if (!*count)
		return phf.status;

	(*count)--;

	return HAL_FATAL;
}

static enum hal_status phf_media_config_set(void *priv, unsigned int line,
					    unsigned int stream,
					    const struct hal_media *media)
{
	struct phf_hop *h = phf_hop_add(PHF_MEDIA_CONFIG_SET, line);

	phf.n[PHF_MEDIA_CONFIG_SET]++;
	phf_line(line);
	phf.stream = stream;
	h->stream = stream;
	if (media)
		h->media = *media;

	return phf_fail_take(&phf.config_set_fail);
}

static enum hal_status phf_media_start(void *priv, unsigned int line,
				       unsigned int stream)
{
	phf_hop_add(PHF_MEDIA_START, line)->stream = stream;
	phf.n[PHF_MEDIA_START]++;
	phf_line(line);
	phf.stream = stream;

	return phf_fail_take(&phf.start_fail);
}

static enum hal_status phf_media_stop(void *priv, unsigned int line,
				      unsigned int stream)
{
	phf_hop_add(PHF_MEDIA_STOP, line)->stream = stream;
	phf.n[PHF_MEDIA_STOP]++;
	phf_line(line);
	phf.stream = stream;

	return phf_fail_take(&phf.stop_fail);
}

static enum hal_status phf_media_dtmf_send(void *priv, unsigned int line,
					   unsigned int stream, char digit,
					   uint32_t duration_ms)
{
	phf.n[PHF_MEDIA_DTMF_SEND]++;
	phf_line(line);
	phf.stream = stream;
	phf.digit = digit;

	return phf.status;
}

static void phf_media_stats_snapshot(void *priv, unsigned int line,
				     unsigned int stream)
{
	phf_hop_add(PHF_MEDIA_STATS_SNAPSHOT, line)->stream = stream;
	phf.n[PHF_MEDIA_STATS_SNAPSHOT]++;
	phf_line(line);
	phf.stream = stream;
}

static enum hal_status phf_media_fax_apply(void *priv, unsigned int line,
					   unsigned int stream,
					   const struct hal_fax_profile *fax)
{
	struct phf_hop *h = phf_hop_add(PHF_MEDIA_FAX_APPLY, line);

	phf.n[PHF_MEDIA_FAX_APPLY]++;
	phf_line(line);
	phf.stream = stream;
	h->stream = stream;
	if (fax)
		h->fax = *fax;

	return phf.status;
}

static enum hal_status phf_media_lec_latch_clear(void *priv, unsigned int line)
{
	phf.n[PHF_MEDIA_LEC_LATCH_CLEAR]++;
	phf_line(line);

	return phf.status;
}

/* A fixed pattern, so a suite that keys a stream reads the same key twice. */
static bool phf_bytes_fill(void *buf, size_t len)
{
	uint8_t *p = buf;
	size_t i;

	if (!phf.accept)
		return false;

	for (i = 0; i < len; i++)
		p[i] = (uint8_t)(i + 1);

	return true;
}

static bool phf_media_rand_bytes(void *priv, void *buf, size_t len)
{
	phf.n[PHF_MEDIA_RAND_BYTES]++;

	return phf_bytes_fill(buf, len);
}

static enum hal_status phf_stream_stats_get(void *ctx, unsigned int line,
					    unsigned int stream,
					    struct hal_media_stats *out)
{
	phf_hop_add(PHF_STREAM_STATS_GET, line)->stream = stream;
	phf.n[PHF_STREAM_STATS_GET]++;
	phf_line(line);
	phf.stream = stream;
	if (out)
		*out = phf.stats;

	return phf.stats_status;
}

static enum hal_status phf_stats_live_query(void *ctx, unsigned int line,
					    unsigned int stream,
					    proto_stats_cb cb, void *priv)
{
	phf.n[PHF_STATS_LIVE_QUERY]++;
	phf_line(line);
	phf.stream = stream;

	return phf.status;
}

static void phf_stats_live_drop(void *ctx, unsigned int line,
				unsigned int stream)
{
	phf.n[PHF_STATS_LIVE_DROP]++;
	phf_line(line);
	phf.stream = stream;
}

/* ---------------------------------------------------------- the service */

static bool phf_line_exists(void *ctx, unsigned int line)
{
	phf.n[PHF_LINE_EXISTS]++;

	return phf.line_exists;
}

static bool phf_line_in_service(void *ctx, unsigned int line)
{
	phf.n[PHF_LINE_IN_SERVICE]++;

	return phf.line_in_service;
}

static bool phf_hal_ready(void *ctx)
{
	phf.n[PHF_HAL_READY]++;

	return phf.hal_ready;
}

static void phf_line_oos_force(void *ctx, unsigned int line,
			       const char *reason)
{
	phf.n[PHF_LINE_OOS_FORCE]++;
	phf_line(line);
	str_copy(phf.reason, sizeof(phf.reason), reason ? reason : "");
}

static void phf_line_oos_clear(void *ctx, unsigned int line)
{
	phf.n[PHF_LINE_OOS_CLEAR]++;
	phf_line(line);
}

static void phf_line_transition(void *ctx, unsigned int line,
				unsigned int state_id, uint32_t flags)
{
	phf.n[PHF_LINE_TRANSITION]++;
	phf_line(line);
	phf.state_id = state_id;
	phf.flags = flags;
}

static void phf_line_settle(void *ctx, unsigned int line)
{
	phf.n[PHF_LINE_SETTLE]++;
	phf_line(line);
}

static void phf_line_cfg_landed(void *ctx, unsigned int line)
{
	phf.n[PHF_LINE_CFG_LANDED]++;
	phf_line(line);
}

static uint32_t phf_hal_features(void *ctx)
{
	phf.n[PHF_HAL_FEATURES]++;

	return phf.hal_features;
}

static uint32_t phf_hal_codecs(void *ctx)
{
	phf.n[PHF_HAL_CODECS]++;

	return phf.hal_codecs;
}

static const struct hal_cap *phf_hal_cap(void *ctx)
{
	static struct hal_cap cap;

	phf.n[PHF_HAL_CAP]++;
	cap.features = phf.hal_features;
	cap.codecs = phf.hal_codecs;

	return &cap;
}

static const struct hal_region *phf_region(void *ctx)
{
	static const struct hal_region region;

	phf.n[PHF_REGION]++;

	return phf.region ? phf.region : &region;
}

static enum hal_status phf_conf_set(void *ctx, unsigned int line, bool on)
{
	phf_hop_add(PHF_CONF_SET, line)->on = on;
	phf.n[PHF_CONF_SET]++;
	phf_line(line);
	phf.on = on;

	return phf.status;
}

static void phf_line_fault(void *ctx, unsigned int line, bool loud,
			   const char *text)
{
	phf.n[PHF_LINE_FAULT]++;
	phf_line(line);
	str_copy(phf.reason, sizeof(phf.reason), text ? text : "");
}

/* ------------------------------------------------------ trace and log */

static bool phf_trace_active(void *ctx, enum trace_domain dom)
{
	return trace_domain_active(dom);
}

static void phf_trace_write(void *ctx, enum trace_domain dom, const char *fmt,
			    ...) __attribute__((format(printf, 3, 4)));

static void phf_trace_write(void *ctx, enum trace_domain dom, const char *fmt,
			    ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	trace_write(dom, "%s", buf);
}

static void phf_log(void *ctx, int level, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void phf_log(void *ctx, int level, const char *fmt, ...)
{
	va_list ap;

	phf.n[PHF_LOG]++;
	phf.log_level = level;
	va_start(ap, fmt);
	vsnprintf(phf.log_text, sizeof(phf.log_text), fmt, ap);
	va_end(ap);
}

/* -------------------------------------------------- config, ubus, events */

static const struct cfg *phf_cfg(void *ctx)
{
	phf.n[PHF_CFG]++;

	return phf.cfg;
}

static uint32_t phf_config_generation(void *ctx)
{
	phf.n[PHF_CONFIG_GENERATION]++;

	return phf.config_generation;
}

static struct ubus_context *phf_ubus_ctx(void *ctx)
{
	phf.n[PHF_UBUS_CTX]++;

	return NULL;
}

static void phf_ubus_request_reply(void *ctx, struct ubus_context *ubus,
				   struct ubus_request_data *req,
				   unsigned int line, const char *verb)
{
	phf.n[PHF_UBUS_REQUEST_REPLY]++;
	phf_line(line);
	str_copy(phf.verb, sizeof(phf.verb), verb ? verb : "");
}

static void phf_reg_state_post(void *ctx, unsigned int line, bool up,
			       const char *proxy, bool moved)
{
	phf.n[PHF_REG_STATE_POST]++;
	phf_line(line);
	phf.on = up;
#ifdef PHF_NET_CORE
	event_reg_post(line, up, proxy);
	if (moved)
		calltrace_event(line, "registration");
#endif
}

static void phf_tls_cert_start(void *ctx, const char *url,
			       const char *username, const char *password,
			       bool verify, void (*done)(void))
{
	phf.n[PHF_TLS_CERT_START]++;
#ifdef PHF_NET_CORE
	event_tls_cert_start(url, username, password, verify, done);
#endif
}

static void phf_tls_cert_stop(void *ctx, enum proto_tls_stop mode)
{
	phf.n[PHF_TLS_CERT_STOP]++;
#ifdef PHF_NET_CORE
	event_tls_cert_stop(mode == PROTO_TLS_STOP_SHUTDOWN ?
			    EVENT_TLS_STOP_SHUTDOWN : EVENT_TLS_STOP_CONFIG);
#endif
}

/* ------------------------------------------------------------ the network */

static void phf_net_iface_subscribe(void *ctx, const struct net_iface_sub *sub)
{
	phf.n[PHF_NET_IFACE_SUBSCRIBE]++;
	phf.sub_set = sub != NULL;
	if (sub)
		phf.sub = *sub;
#ifdef PHF_NET_CORE
	net_iface_subscribe(sub);
#endif
}

#ifdef PHF_NET_CORE
static const struct net_iface_state *phf_iface_state(void)
{
	return net_iface_state_get();
}
#else
static const struct net_iface_state *phf_iface_state(void)
{
	static const struct net_iface_state none;

	return phf.iface ? phf.iface : &none;
}
#endif

static const struct net_iface_state *phf_net_iface_state_get(void *ctx)
{
	phf.n[PHF_NET_IFACE_STATE_GET]++;

	return phf_iface_state();
}

static const char *phf_net_iface_bind_device(void *ctx, const char *iface)
{
	phf.n[PHF_NET_IFACE_BIND_DEVICE]++;
#ifdef PHF_NET_CORE
	return net_iface_bind_device(iface);
#else
	return NULL;
#endif
}

static bool phf_net_iface_source_host_probe(void *ctx, const char *dev,
					    int family,
					    const struct sockaddr *sa,
					    socklen_t salen, char *out,
					    size_t olen)
{
	phf.n[PHF_NET_IFACE_SOURCE_HOST_PROBE]++;
#ifdef PHF_NET_CORE
	return net_iface_source_host_probe(dev, family, sa, salen, out, olen);
#else
	if (!phf.probe_addr)
		return false;

	return snprintf(out, olen, "%s", phf.probe_addr) < (int)olen;
#endif
}

static void phf_net_iface_ipv6_served_set(void *ctx, bool served)
{
	phf.n[PHF_NET_IFACE_IPV6_SERVED_SET]++;
	phf.on = served;
#ifdef PHF_NET_CORE
	net_iface_ipv6_served_set(served);
#endif
}

static void phf_net_iface_recover_settle(void *ctx)
{
	phf.n[PHF_NET_IFACE_RECOVER_SETTLE]++;
#ifdef PHF_NET_CORE
	net_iface_recover_settle();
#endif
}

#ifdef PHF_NET_PORTS
static void phf_net_port_range(void *ctx, const struct cfg_channel *ch,
			       uint16_t *min, uint16_t *max)
{
	phf.n[PHF_NET_PORT_RANGE]++;
	net_port_range(ch, min, max);
}

static bool phf_net_port_range_eq(void *ctx, const struct cfg_channel *a,
				  const struct cfg_channel *b)
{
	phf.n[PHF_NET_PORT_RANGE_EQ]++;

	return net_port_range_eq(a, b);
}

static bool phf_net_port_alloc(void *ctx, unsigned int min, unsigned int max,
			       uint16_t *out)
{
	phf.n[PHF_NET_PORT_ALLOC]++;

	return net_port_alloc(min, max, out);
}

static void phf_net_port_free(void *ctx, uint16_t port)
{
	phf.n[PHF_NET_PORT_FREE]++;
	net_port_free(port);
}
#else
static void phf_net_port_range(void *ctx, const struct cfg_channel *ch,
			       uint16_t *min, uint16_t *max)
{
	phf.n[PHF_NET_PORT_RANGE]++;
	if (min)
		*min = 10000;
	if (max)
		*max = 10030;
}

static bool phf_net_port_range_eq(void *ctx, const struct cfg_channel *a,
				  const struct cfg_channel *b)
{
	phf.n[PHF_NET_PORT_RANGE_EQ]++;

	return true;
}

static bool phf_net_port_alloc(void *ctx, unsigned int min, unsigned int max,
			       uint16_t *out)
{
	phf.n[PHF_NET_PORT_ALLOC]++;
	if (!phf.accept)
		return false;
	if (out)
		*out = (uint16_t)min;

	return true;
}

static void phf_net_port_free(void *ctx, uint16_t port)
{
	phf.n[PHF_NET_PORT_FREE]++;
}
#endif

#ifdef PHF_NET_CORE
static unsigned int phf_net_ct_udp_bound_s(void *ctx)
{
	phf.n[PHF_NET_CT_UDP_BOUND_S]++;

	return net_ct_udp_bound_s();
}

static uint16_t phf_net_ct_punch_interval_ms(void *ctx)
{
	phf.n[PHF_NET_CT_PUNCH_INTERVAL_MS]++;

	return net_ct_punch_interval_ms();
}
#else
static unsigned int phf_net_ct_udp_bound_s(void *ctx)
{
	phf.n[PHF_NET_CT_UDP_BOUND_S]++;

	return phf.ct_udp_bound_s;
}

static uint16_t phf_net_ct_punch_interval_ms(void *ctx)
{
	phf.n[PHF_NET_CT_PUNCH_INTERVAL_MS]++;

	return phf.punch_interval_ms;
}
#endif

static void phf_fw_ask(void *ctx, const struct fw_sig_ask *ask)
{
	phf.n[PHF_FW_ASK]++;
	if (ask)
		phf.fw_ask = *ask;
#ifdef PHF_NET_CORE
	fw_reconcile_ask(ask);
#endif
}

static void phf_fw_clear(void *ctx)
{
	phf.n[PHF_FW_CLEAR]++;
#ifdef PHF_NET_CORE
	fw_reconcile_clear();
#endif
}

static unsigned int phf_fw_proxy_add(void *ctx,
				     char (*list)[PROTO_FW_SRC_MAX],
				     unsigned int count, const char *addr)
{
	phf.n[PHF_FW_PROXY_ADD]++;
#ifdef PHF_NET_CORE
	return fw_reconcile_proxy_add(list, count, addr);
#else
	if (!addr || !*addr || count >= PROTO_FW_PROXY_MAX)
		return count;

	str_copy(list[count], PROTO_FW_SRC_MAX, addr);

	return count + 1;
#endif
}

/* ------------------------------------------------------ records and trace */

static uint32_t phf_cdr_call_started(void *ctx, unsigned int line,
				     unsigned int stream, bool inbound)
{
	phf_hop_add(PHF_CDR_CALL_STARTED, line)->stream = stream;
	phf.n[PHF_CDR_CALL_STARTED]++;
	phf_line(line);
	phf.stream = stream;
	phf.cdr_inbound = inbound;

	return phf.cdr_seq;
}

static uint32_t phf_cdr_call_seq(void *ctx, unsigned int line,
				 unsigned int stream)
{
	phf.n[PHF_CDR_CALL_SEQ]++;

	return phf.cdr_seq;
}

static void phf_cdr_call_correlate(void *ctx, unsigned int line,
				   unsigned int stream,
				   unsigned int from_stream)
{
	struct phf_hop *h = phf_hop_add(PHF_CDR_CALL_CORRELATE, line);

	h->stream = stream;
	h->val = from_stream;
	phf.n[PHF_CDR_CALL_CORRELATE]++;
	phf_line(line);
	phf.stream = stream;
}

static void phf_cdr_call_answered(void *ctx, unsigned int line,
				  unsigned int stream)
{
	phf_hop_add(PHF_CDR_CALL_ANSWERED, line)->stream = stream;
	phf.n[PHF_CDR_CALL_ANSWERED]++;
	phf_line(line);
	phf.stream = stream;
}

static void phf_cdr_call_conferenced(void *ctx, unsigned int line,
				     unsigned int stream)
{
	phf.n[PHF_CDR_CALL_CONFERENCED]++;
	phf_line(line);
	phf.stream = stream;
}

static void phf_cdr_call_ended(void *ctx, unsigned int line,
			       unsigned int stream,
			       const struct proto_call_ended *info)
{
	struct phf_hop *h = phf_hop_add(PHF_CDR_CALL_ENDED, line);

	h->stream = stream;
	h->on = info && info->transferred;
	phf.n[PHF_CDR_CALL_ENDED]++;
	phf_line(line);
	phf.stream = stream;
	if (info)
		phf.cdr_ended = *info;
}

static void phf_calltrace_call_started(void *ctx, unsigned int line,
				       uint32_t cdr_seq)
{
	phf.n[PHF_CALLTRACE_CALL_STARTED]++;
	phf_line(line);
}

static void phf_calltrace_event(void *ctx, unsigned int line,
				const char *reason)
{
	phf.n[PHF_CALLTRACE_EVENT]++;
	phf_line(line);
	str_copy(phf.reason, sizeof(phf.reason), reason ? reason : "");
}

static void phf_calltrace_state(void *ctx, unsigned int line,
				const char *before, const char *after)
{
	phf.n[PHF_CALLTRACE_STATE]++;
	phf_line(line);
	str_copy(phf.before, sizeof(phf.before), before ? before : "");
	str_copy(phf.after, sizeof(phf.after), after ? after : "");
}

static void phf_calltrace_tap_endpoint_set(void *ctx,
					   struct trace_endpoint *out,
					   const struct sockaddr *sa)
{
	phf.n[PHF_CALLTRACE_TAP_ENDPOINT_SET]++;
	trace_endpoint_set(out, sa);
}

/* -------------------------------------------------- lifecycle and entropy */

static void phf_shutdown_done(void *ctx)
{
	phf.n[PHF_SHUTDOWN_DONE]++;
	if (phf.shutdown_ends_loop)
		uloop_end();
}

static bool phf_random_bytes(void *ctx, void *buf, size_t len)
{
	phf.n[PHF_RANDOM_BYTES]++;

	return phf_bytes_fill(buf, len);
}

/* ------------------------------------------------------------ the table */

static const struct proto_host phf_host = {
	.ctx = &phf,
	.tone_play = phf_tone_play,
	.tone_stop = phf_tone_stop,
	.ring_start = phf_ring_start,
	.ring_stop = phf_ring_stop,
	.cw_alert = phf_cw_alert,
	.cid_send = phf_cid_send,
	.ring_template_match = phf_ring_template_match,
	.mwi_set = phf_mwi_set,
	.mwi_get = phf_mwi_get,
	.detector_arm = phf_detector_arm,
	.fwd_disc_signal = phf_fwd_disc_signal,
	.fwd_disc_abort = phf_fwd_disc_abort,
	.battery_set = phf_battery_set,
	.digit_play = phf_digit_play,
	.digit_play_cancel = phf_digit_play_cancel,
	.media = {
		.config_set = phf_media_config_set,
		.start = phf_media_start,
		.stop = phf_media_stop,
		.dtmf_send = phf_media_dtmf_send,
		.stats_snapshot = phf_media_stats_snapshot,
		.fax_apply = phf_media_fax_apply,
		.lec_latch_clear = phf_media_lec_latch_clear,
		.rand_bytes = phf_media_rand_bytes,
		.priv = &phf,
	},
	.stream_stats_get = phf_stream_stats_get,
	.stats_live_query = phf_stats_live_query,
	.stats_live_drop = phf_stats_live_drop,
	.line_exists = phf_line_exists,
	.line_in_service = phf_line_in_service,
	.hal_ready = phf_hal_ready,
	.line_oos_force = phf_line_oos_force,
	.line_oos_clear = phf_line_oos_clear,
	.line_transition = phf_line_transition,
	.line_settle = phf_line_settle,
	.line_cfg_landed = phf_line_cfg_landed,
	.hal_features = phf_hal_features,
	.hal_codecs = phf_hal_codecs,
	.hal_cap = phf_hal_cap,
	.region = phf_region,
	.conf_set = phf_conf_set,
	.line_fault = phf_line_fault,
	.trace_active = phf_trace_active,
	.trace_write = phf_trace_write,
	.log = phf_log,
	.cfg = phf_cfg,
	.config_generation = phf_config_generation,
	.ubus_ctx = phf_ubus_ctx,
	.ubus_request_reply = phf_ubus_request_reply,
	.reg_state_post = phf_reg_state_post,
	.tls_cert_start = phf_tls_cert_start,
	.tls_cert_stop = phf_tls_cert_stop,
	.net_iface_subscribe = phf_net_iface_subscribe,
	.net_iface_state_get = phf_net_iface_state_get,
	.net_iface_bind_device = phf_net_iface_bind_device,
	.net_iface_source_host_probe = phf_net_iface_source_host_probe,
	.net_iface_ipv6_served_set = phf_net_iface_ipv6_served_set,
	.net_iface_recover_settle = phf_net_iface_recover_settle,
	.net_port_range = phf_net_port_range,
	.net_port_range_eq = phf_net_port_range_eq,
	.net_port_alloc = phf_net_port_alloc,
	.net_port_free = phf_net_port_free,
	.net_ct_udp_bound_s = phf_net_ct_udp_bound_s,
	.net_ct_punch_interval_ms = phf_net_ct_punch_interval_ms,
	.fw_ask = phf_fw_ask,
	.fw_clear = phf_fw_clear,
	.fw_proxy_add = phf_fw_proxy_add,
	.cdr_call_started = phf_cdr_call_started,
	.cdr_call_seq = phf_cdr_call_seq,
	.cdr_call_correlate = phf_cdr_call_correlate,
	.cdr_call_answered = phf_cdr_call_answered,
	.cdr_call_conferenced = phf_cdr_call_conferenced,
	.cdr_call_ended = phf_cdr_call_ended,
	.calltrace_call_started = phf_calltrace_call_started,
	.calltrace_event = phf_calltrace_event,
	.calltrace_state = phf_calltrace_state,
	.calltrace_tap_endpoint_set = phf_calltrace_tap_endpoint_set,
	.shutdown_done = phf_shutdown_done,
	.random_bytes = phf_random_bytes,
};

void proto_host_fake_reset(void)
{
	memset(&phf, 0, sizeof(phf));
	phf.status = HAL_OK;
	phf.stats_status = HAL_OK;
	phf.line_exists = true;
	phf.line_in_service = true;
	phf.hal_ready = true;
	phf.accept = true;
	phf.digit_q_max = 16;
	phf.config_generation = 1;
	phf.cdr_seq = 1;
	phf.ct_udp_bound_s = 30;
	phf.punch_interval_ms = 15000;
}

const struct proto_host *proto_host_fake(void)
{
	return &phf_host;
}
