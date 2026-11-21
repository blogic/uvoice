// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Jingle call legs. See xmpp/call.h for the contracts.
 *
 * One guard timer per leg, whose meaning the state gives: the ring of a
 * propose, the wait for the session behind a proceed, the ring of an offer,
 * and the wait for media once both sides described their transport.
 *
 * The description a leg programs is built the way sip/call.c builds one:
 * media_fill() from a result the negotiation produced, then the transport
 * block on top. The remote address of that result is the unspecified
 * address of the local family, because an ICE stream has no remote tuple
 * until the checks ran and the backend owns the one it finds.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <libubox/blobmsg.h>

#include "str.h"
#include "proto/proto_trace.h"
#include "proto/shared/media_fill.h"
#include "proto/shared/sdp.h"
#include "proto/xmpp/call.h"
#include "proto/xmpp/jmi.h"

#define XCALL_DTMF_NAME		"telephone-event"
#define XCALL_DTMF_CLOCK	8000U
#define XCALL_OPUS_CLOCK	48000U
#define XCALL_G711_CLOCK	8000U
#define XCALL_DISCARD_PORT	9
#define XCALL_FLUSH_MAX		JINGLE_CANDIDATES_MAX
#define XCALL_GUARD_ID		1
#define XCALL_ID_MAX		64U

#define XCALL_TRACE(leg, fmt, ...)					\
	do {								\
		if (xv.env.host)					\
			PROTO_TRACE(xv.env.host, TRACE_XMPP,		\
				    "xcall line %u slot %u: " fmt,	\
				    (leg)->line, (leg)->slot,		\
				    ##__VA_ARGS__);			\
	} while (0)

struct xcall_remote {
	char ufrag[JINGLE_UFRAG_MAX];
	char pwd[JINGLE_PWD_MAX];
	uint8_t fp[JINGLE_FP_LEN];
	enum jingle_setup setup;
	bool known;
};

struct xcall {
	bool in_use;
	bool inbound;
	bool jmi;			/* the call started with a propose */
	unsigned int line;
	unsigned int slot;
	enum xcall_state state;
	char sid[JINGLE_SID_MAX];
	char peer[JID_MAX];		/* the device, "" until it is known */
	char peer_bare[JID_MAX];	/* the account */
	char target[XCALL_TARGET_MAX];	/* what was dialled */
	char name[XCALL_NAME_MAX];
	struct cfg_channel ch;
	char local_addr[SDP_ADDR_MAX];
	char bind_dev[HAL_IFNAME_MAX];
	struct sdp_local loc;
	struct jingle_local local;
	struct xcall_remote remote;
	struct jingle_candidate pending[JINGLE_CANDIDATES_MAX];
	unsigned int pendings;
	enum sdp_codec_id codec;
	uint8_t codec_pt;
	int dtmf_pt;
	struct hal_media media;
	bool media_configured;
	bool media_started;
	bool media_ran;
	bool held_local;
	bool held_remote;
	bool answered;
	bool failed;
	enum proto_fail_class fail_class;
	bool cleared;
	bool dtls_up;
	bool dtls_closed;
	bool relay;
	uint64_t answered_ms;
	uint64_t cleared_ms;
	struct proto_timer guard;
};

static struct {
	struct xcall_env env;
	const struct cfg *cfg;
	uint32_t hal_features;
	uint32_t hal_codecs;
	bool inited;
} xv;

static struct xcall legs[CFG_CHANNEL_MAX][CALL_SLOTS];

static const char *const state_names[XCALL_STATE_MAX] = {
	[XCALL_FREE] = "free",
	[XCALL_PROPOSED] = "proposed",
	[XCALL_OFFERED] = "offered",
	[XCALL_PROCEEDED] = "proceeded",
	[XCALL_INITIATED] = "initiated",
	[XCALL_ACTIVE] = "active",
};

static void guard_fire(struct proto_timer *timer);

const char *xcall_state_name(enum xcall_state state)
{
	if (state >= XCALL_STATE_MAX)
		return "unknown";

	return state_names[state];
}

/* ------------------------------------------------------------------ legs */

/**
 * leg_get() - the leg of a slot
 * @line: the line
 * @slot: the slot
 *
 * Return: the leg, or NULL for an index out of range or a free slot.
 */
static struct xcall *leg_get(unsigned int line, unsigned int slot)
{
	if (line >= CFG_CHANNEL_MAX || slot >= CALL_SLOTS ||
	    !legs[line][slot].in_use)
		return NULL;

	return &legs[line][slot];
}

/**
 * leg_find_sid() - the leg of a session id
 * @line: the line
 * @sid: the session id
 *
 * Return: the leg, or NULL.
 */
static struct xcall *leg_find_sid(unsigned int line, const char *sid)
{
	unsigned int slot;

	if (line >= CFG_CHANNEL_MAX)
		return NULL;

	for (slot = 0; slot < CALL_SLOTS; slot++) {
		if (legs[line][slot].in_use &&
		    !strcmp(legs[line][slot].sid, sid))
			return &legs[line][slot];
	}

	return NULL;
}

/**
 * now_ms() - the clock of the environment
 *
 * Return: milliseconds.
 */
static uint64_t now_ms(void)
{
	if (!xv.env.timer || !xv.env.timer->now_ms)
		return 0;

	return xv.env.timer->now_ms(xv.env.timer);
}

/**
 * guard_arm() - start the guard of a leg
 * @leg: the leg
 * @ms: the bound
 */
static void guard_arm(struct xcall *leg, unsigned int ms)
{
	if (xv.env.timer)
		proto_timer_arm(xv.env.timer, &leg->guard, ms);
}

/**
 * guard_cancel() - stop the guard of a leg
 * @leg: the leg
 */
static void guard_cancel(struct xcall *leg)
{
	if (xv.env.timer)
		proto_timer_cancel(xv.env.timer, &leg->guard);
}

/**
 * leg_alloc() - take a slot for a new leg
 * @line: the line
 * @slot: the slot
 * @inbound: the direction
 *
 * Return: the leg, or NULL when the slot is taken or the line unknown.
 */
static struct xcall *leg_alloc(unsigned int line, unsigned int slot,
			       bool inbound)
{
	struct xcall *leg;

	if (line >= CFG_CHANNEL_MAX || slot >= CALL_SLOTS ||
	    legs[line][slot].in_use || !xv.cfg)
		return NULL;

	leg = &legs[line][slot];
	memset(leg, 0, sizeof(*leg));
	leg->in_use = true;
	leg->inbound = inbound;
	leg->line = line;
	leg->slot = slot;
	leg->ch = xv.cfg->channel[line];
	leg->dtmf_pt = -1;
	proto_timer_init(&leg->guard, leg, XCALL_GUARD_ID, guard_fire);

	if (xv.env.local_addr)
		xv.env.local_addr(xv.env.priv, line, leg->local_addr,
				  sizeof(leg->local_addr));
	if (xv.env.bind_dev)
		str_copy(leg->bind_dev, sizeof(leg->bind_dev),
			 xv.env.bind_dev(xv.env.priv, line));

	return leg;
}

/**
 * media_stop() - stop the stream of a leg, once
 * @leg: the leg
 */
static void media_stop(struct xcall *leg)
{
	const struct call_media_ops *ops = xv.env.media;

	if (!leg->media_started)
		return;

	leg->media_started = false;
	if (ops && ops->stats_snapshot)
		ops->stats_snapshot(ops->priv, leg->line, leg->slot);
	if (ops && ops->stop)
		ops->stop(ops->priv, leg->line, leg->slot);
}

/**
 * leg_release() - end a leg and report what it was
 * @leg: the leg
 */
static void leg_release(struct xcall *leg)
{
	struct proto_call_ended info;
	uint64_t now = now_ms();
	unsigned int line = leg->line, slot = leg->slot;

	guard_cancel(leg);
	media_stop(leg);

	memset(&info, 0, sizeof(info));
	info.inbound = leg->inbound;
	info.answered = leg->answered;
	info.failed = leg->failed;
	info.fail_class = leg->fail_class;
	info.media_ran = leg->media_ran;
	info.cleared = leg->cleared;
	if (leg->cleared && now >= leg->cleared_ms)
		info.cleared_ago_ms = (uint32_t)(now - leg->cleared_ms);
	if (leg->answered && now >= leg->answered_ms)
		info.answered_ago_ms = (uint32_t)(now - leg->answered_ms);
	info.number = leg->target;
	info.remote_number = leg->peer_bare;
	info.remote_name = leg->name;
	if (leg->answered) {
		info.codec = (uint8_t)media_codec_of(leg->codec);
		info.ptime_ms = leg->loc.ptime_ms;
	}

	XCALL_TRACE(leg, "released in %s", xcall_state_name(leg->state));
	leg->in_use = false;
	leg->state = XCALL_FREE;

	if (xv.env.cb && xv.env.cb->ended)
		xv.env.cb->ended(line, slot, &info);

	memset(leg, 0, sizeof(*leg));
}

/**
 * leg_clear() - mark the end the user or the peer made
 * @leg: the leg
 */
static void leg_clear(struct xcall *leg)
{
	leg->cleared = true;
	leg->cleared_ms = now_ms();
}

/**
 * leg_fail() - end a leg through a failure class
 * @leg: the leg
 * @cls: the class
 */
static void leg_fail(struct xcall *leg, enum proto_fail_class cls)
{
	unsigned int line = leg->line, slot = leg->slot;

	leg->failed = true;
	leg->fail_class = cls;
	XCALL_TRACE(leg, "failed with %s", proto_fail_name(cls));
	leg_release(leg);

	if (xv.env.cb && xv.env.cb->failed)
		xv.env.cb->failed(line, slot, cls);
}

static void leg_bye(struct xcall *leg)
{
	unsigned int line = leg->line, slot = leg->slot;

	leg_release(leg);

	if (xv.env.cb && xv.env.cb->remote_bye)
		xv.env.cb->remote_bye(line, slot);
}

/* ------------------------------------------------------------------- wire */

/**
 * text_send() - one stanza on the stream of the leg
 * @leg: the leg
 * @writer: the writer, finished here
 *
 * Return: true when the stream took it.
 */
static bool text_send(struct xcall *leg, struct xml_w *writer)
{
	size_t len = xml_w_finish(writer);

	if (!len || !xv.env.send)
		return false;

	return xv.env.send(xv.env.priv, leg->line, writer->mem, len) == 0;
}

/**
 * id_next() - a fresh stanza id
 * @leg: the leg
 * @out: receives the id
 * @len: capacity of @out
 */
static void id_next(struct xcall *leg, char *out, size_t len)
{
	if (xv.env.id_next) {
		xv.env.id_next(xv.env.priv, leg->line, out, len);
		return;
	}

	snprintf(out, len, "x%u%u", leg->line, leg->slot);
}

/**
 * jmi_send() - one message of XEP-0353
 * @leg: the leg
 * @kind: the element
 * @to: the recipient
 *
 * Return: true when the stream took it.
 */
static bool jmi_send(struct xcall *leg, enum jmi_kind kind, const char *to)
{
	char mem[JMI_STANZA_MAX];
	char msg_id[XCALL_ID_MAX];
	struct xml_w writer;

	id_next(leg, msg_id, sizeof(msg_id));
	xml_w_init(&writer, mem, sizeof(mem));
	jmi_write(&writer, kind, to, msg_id, leg->sid);
	XCALL_TRACE(leg, "jmi %s to %s", jmi_kind_name(kind), to);

	return text_send(leg, &writer);
}

/**
 * iq_reply() - the answer to a session-initiate or a session-accept
 * @priv: the leg
 * @ok: whether the peer answered with a result
 * @reply: the answer, NULL for none
 *
 * A refused initiate ends the attempt. A refused accept ends the call.
 */
static void iq_reply(void *priv, bool ok, const struct xml_doc *reply)
{
	struct xcall *leg = priv;

	if (!leg->in_use || ok)
		return;

	XCALL_TRACE(leg, "the peer refused the session iq in %s",
		    xcall_state_name(leg->state));
	if (leg->state == XCALL_INITIATED || leg->state == XCALL_ACTIVE)
		leg_fail(leg, PROTO_FAIL_CONGESTION);
}

/**
 * iq_ignore() - the answer to an iq whose outcome changes nothing
 * @priv: unused
 * @ok: unused
 * @reply: unused
 */
static void iq_ignore(void *priv, bool ok, const struct xml_doc *reply)
{
}

/**
 * jingle_send() - one Jingle iq to the peer device
 * @leg: the leg
 * @writer: the writer, finished here
 * @id: the iq id
 * @cb: the answer callback
 *
 * Return: true when the stream took it.
 */
static bool jingle_send(struct xcall *leg, struct xml_w *writer,
			const char *id, xcall_reply_cb cb)
{
	size_t len = xml_w_finish(writer);

	if (!len || !xv.env.iq_send)
		return false;

	return xv.env.iq_send(xv.env.priv, leg->line, writer->mem, len, id,
			      cb, leg) == 0;
}

/**
 * terminate_send() - a session-terminate to the peer device
 * @leg: the leg
 * @reason: the reason
 */
static void terminate_send(struct xcall *leg, enum jingle_reason reason)
{
	char mem[JINGLE_STANZA_MAX];
	char id[XCALL_ID_MAX];
	struct xml_w writer;

	if (!leg->peer[0])
		return;

	id_next(leg, id, sizeof(id));
	xml_w_init(&writer, mem, sizeof(mem));
	jingle_open(&writer, leg->peer, id, JINGLE_SESSION_TERMINATE, leg->sid,
		    NULL, NULL);
	jingle_reason_write(&writer, reason, NULL);
	jingle_close(&writer);
	XCALL_TRACE(leg, "terminate %s", jingle_reason_name(reason));
	jingle_send(leg, &writer, id, iq_ignore);
}

/**
 * info_send() - a session-info to the peer device
 * @leg: the leg
 * @info: the payload
 *
 * Return: true when the stream took it.
 */
static bool info_send(struct xcall *leg, enum jingle_info info)
{
	char mem[JINGLE_STANZA_MAX];
	char id[XCALL_ID_MAX];
	struct xml_w writer;

	id_next(leg, id, sizeof(id));
	xml_w_init(&writer, mem, sizeof(mem));
	jingle_open(&writer, leg->peer, id, JINGLE_SESSION_INFO, leg->sid,
		    NULL, NULL);
	jingle_info_write(&writer, info);
	jingle_close(&writer);

	return jingle_send(leg, &writer, id, iq_ignore);
}

/* ------------------------------------------------------------------ media */

/**
 * payload_fill() - the wire form of one codec
 * @codec: the SDP codec
 * @pt: its payload type
 * @out: receives the payload type element
 */
static void payload_fill(enum sdp_codec_id codec, uint8_t pt,
			 struct jingle_payload *out)
{
	memset(out, 0, sizeof(*out));
	out->id = pt;
	str_copy(out->name, sizeof(out->name), sdp_codec_name(codec));
	out->clockrate = codec == SDP_CODEC_OPUS ? XCALL_OPUS_CLOCK :
						   XCALL_G711_CLOCK;
	if (codec == SDP_CODEC_OPUS)
		out->channels = 2;
}

/**
 * dtmf_fill() - the telephone-event payload type
 * @pt: its payload type
 * @clock: the clock of the audio it rides
 * @out: receives the element
 */
static void dtmf_fill(uint8_t pt, uint32_t clock, struct jingle_payload *out)
{
	memset(out, 0, sizeof(*out));
	out->id = pt;
	str_copy(out->name, sizeof(out->name), XCALL_DTMF_NAME);
	out->clockrate = clock;
}

/**
 * local_offer_build() - the codec list this leg offers
 * @leg: the leg
 *
 * The list is the one a SIP leg offers, built from the codec sections of
 * the channel and the backend mask, with the telephone-event behind it.
 *
 * Return: true when at least one codec is offerable.
 */
static bool local_offer_build(struct xcall *leg)
{
	struct jingle_local *local = &leg->local;
	unsigned int i;

	media_sdp_local_build(xv.cfg, &leg->ch, xv.hal_codecs, leg->local_addr,
			      0, &leg->loc);

	memset(local, 0, sizeof(*local));
	str_copy(local->content_name, sizeof(local->content_name),
		 JINGLE_CONTENT_DEFAULT);
	str_copy(local->creator, sizeof(local->creator), "initiator");
	local->rtcp_mux = true;

	for (i = 0; i < leg->loc.codecs &&
		    local->payloads < JINGLE_PAYLOADS_MAX; i++) {
		if (!sdp_codec_offerable(&leg->loc.codec[i]))
			continue;
		payload_fill((enum sdp_codec_id)leg->loc.codec[i].id,
			     leg->loc.codec[i].pt,
			     &local->payload[local->payloads++]);
	}

	if (!local->payloads)
		return false;

	if (leg->loc.dtmf_pt && local->payloads < JINGLE_PAYLOADS_MAX)
		dtmf_fill(leg->loc.dtmf_pt, XCALL_DTMF_CLOCK,
			  &local->payload[local->payloads++]);

	return true;
}

/**
 * codec_of_payload() - the SDP codec a peer payload type names
 * @payload: the element
 *
 * Return: the codec, SDP_CODEC_NONE for one this stack lacks.
 */
static enum sdp_codec_id codec_of_payload(const struct jingle_payload *payload)
{
	static const struct {
		const char *name;
		enum sdp_codec_id id;
		uint8_t static_pt;
	} table[] = {
		{ "PCMU", SDP_CODEC_PCMU, 0 },
		{ "PCMA", SDP_CODEC_PCMA, 8 },
		{ "G722", SDP_CODEC_G722, 9 },
		{ "G729", SDP_CODEC_G729, 18 },
		{ "opus", SDP_CODEC_OPUS, 255 },
		{ "G726-32", SDP_CODEC_G726_32, 255 },
	};
	unsigned int i;

	for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		if (payload->name[0] &&
		    !strcasecmp(payload->name, table[i].name))
			return table[i].id;
		if (!payload->name[0] && payload->id == table[i].static_pt)
			return table[i].id;
	}

	return SDP_CODEC_NONE;
}

/**
 * payload_select() - the codec of the session, from the peer's list
 * @leg: the leg
 * @content: the peer's content
 *
 * The first payload type of the peer that this leg offers wins, which is
 * the answer rule of RFC 3264 section 6.1. The telephone-event on the clock
 * of that codec, when the peer named one, carries the digits.
 *
 * Return: true when a codec was found.
 */
static bool payload_select(struct xcall *leg,
			   const struct jingle_content *content)
{
	const struct jingle_payload *payload;
	enum sdp_codec_id id;
	uint32_t clock;
	unsigned int i, j;

	leg->codec = SDP_CODEC_NONE;
	leg->dtmf_pt = -1;

	for (i = 0; i < content->payloads && leg->codec == SDP_CODEC_NONE;
	     i++) {
		id = codec_of_payload(&content->payload[i]);
		if (id == SDP_CODEC_NONE)
			continue;
		for (j = 0; j < leg->loc.codecs; j++) {
			if (leg->loc.codec[j].id != id ||
			    !sdp_codec_offerable(&leg->loc.codec[j]))
				continue;
			leg->codec = id;
			leg->codec_pt = content->payload[i].id;
			break;
		}
	}

	if (leg->codec == SDP_CODEC_NONE)
		return false;

	clock = leg->codec == SDP_CODEC_OPUS ? XCALL_OPUS_CLOCK :
					       XCALL_DTMF_CLOCK;
	for (i = 0; i < content->payloads && leg->loc.dtmf_pt; i++) {
		payload = &content->payload[i];
		if (!strcasecmp(payload->name, XCALL_DTMF_NAME) &&
		    (!payload->clockrate || payload->clockrate == clock))
			leg->dtmf_pt = payload->id;
	}

	return true;
}

/**
 * local_answer_build() - narrow the offer to the selected codec
 * @leg: the leg
 * @content: the peer's content, whose names the answer mirrors
 */
static void local_answer_build(struct xcall *leg,
			       const struct jingle_content *content)
{
	struct jingle_local *local = &leg->local;

	local->payloads = 0;
	payload_fill(leg->codec, leg->codec_pt,
		     &local->payload[local->payloads++]);
	if (leg->dtmf_pt >= 0)
		dtmf_fill((uint8_t)leg->dtmf_pt,
			  leg->codec == SDP_CODEC_OPUS ? XCALL_OPUS_CLOCK :
							 XCALL_DTMF_CLOCK,
			  &local->payload[local->payloads++]);
	if (content->name[0])
		str_copy(local->content_name, sizeof(local->content_name),
			 content->name);
	if (content->creator[0])
		str_copy(local->creator, sizeof(local->creator),
			 content->creator);
}

/**
 * setup_of_remote() - this side's DTLS role against the peer's setup
 * @leg: the leg
 *
 * Return: the enum hal_dtls_setup.
 */
static uint8_t setup_of_remote(const struct xcall *leg)
{
	if (!leg->remote.known)
		return HAL_DTLS_ACTPASS;
	if (leg->remote.setup == JINGLE_SETUP_ACTIVE)
		return HAL_DTLS_PASSIVE;

	return HAL_DTLS_ACTIVE;
}

/**
 * setup_to_jingle() - the setup word this side writes
 * @setup: the enum hal_dtls_setup
 *
 * Return: the enum jingle_setup.
 */
static enum jingle_setup setup_to_jingle(uint8_t setup)
{
	switch (setup) {
	case HAL_DTLS_ACTIVE:
		return JINGLE_SETUP_ACTIVE;
	case HAL_DTLS_PASSIVE:
		return JINGLE_SETUP_PASSIVE;
	default:
		return JINGLE_SETUP_ACTPASS;
	}
}

/**
 * transport_fill() - the ICE and DTLS block of the description
 * @leg: the leg
 * @out: the block
 */
static void transport_fill(struct xcall *leg, struct hal_transport *out)
{
	uint16_t min = 0, max = 0;

	memset(out, 0, sizeof(*out));
	out->kind = HAL_TRANSPORT_ICE_DTLS;
	out->ice_role = leg->inbound ? HAL_ICE_CONTROLLED : HAL_ICE_CONTROLLING;
	out->dtls_setup = setup_of_remote(leg);
	out->rtcp_mux = true;
	if (leg->remote.known) {
		str_copy(out->remote_ufrag, sizeof(out->remote_ufrag),
			 leg->remote.ufrag);
		str_copy(out->remote_pwd, sizeof(out->remote_pwd),
			 leg->remote.pwd);
		memcpy(out->remote_fp, leg->remote.fp, sizeof(out->remote_fp));
		out->remote_fp_hash = HAL_FP_SHA256;
	}
	if (xv.env.ice_servers)
		xv.env.ice_servers(xv.env.priv, leg->line, out);
	leg->relay = out->turns > 0;
	if (xv.env.host && xv.env.host->net_port_range) {
		xv.env.host->net_port_range(xv.env.host->ctx, &leg->ch, &min,
					    &max);
		out->port_min = min;
		out->port_max = max;
	}
}

/**
 * result_fill() - the negotiated result the fill reads
 * @leg: the leg
 * @out: receives it
 */
static void result_fill(const struct xcall *leg, struct sdp_result *out)
{
	bool ipv6 = strchr(leg->local_addr, ':') != NULL;
	unsigned int i;

	memset(out, 0, sizeof(*out));
	for (i = 0; i < leg->loc.codecs; i++) {
		if (leg->loc.codec[i].id == leg->codec)
			out->codec = leg->loc.codec[i];
	}
	out->codec.pt = leg->codec_pt;
	out->dtmf_pt = leg->dtmf_pt;
	out->cn_pt = -1;
	out->dir = SDP_SENDRECV;
	out->ptime_ms = leg->loc.ptime_ms;
	str_copy(out->remote_addr, sizeof(out->remote_addr),
		 ipv6 ? "::" : "0.0.0.0");
	out->remote_ipver = ipv6 ? 6 : 4;
	out->remote_port = XCALL_DISCARD_PORT;
	out->valid = true;
}

/**
 * media_program() - configure the stream of a leg, start it when asked
 * @leg: the leg
 * @start: whether the stream starts
 *
 * Return: true when the backend took the description.
 */
static bool media_program(struct xcall *leg, bool start)
{
	const struct call_media_ops *ops = xv.env.media;
	struct sdp_result res;
	struct hal_media media;
	enum hal_status st;

	if (!ops || !ops->config_set || !xv.cfg)
		return false;

	result_fill(leg, &res);
	media_fill(&leg->ch, xv.cfg->rtcp_enabled, xv.cfg->rtcp_interval,
		   (xv.hal_features & HAL_FEAT_RTCP_ADDR) != 0, &res,
		   leg->local_addr, 0, leg->bind_dev, HAL_PASSTHROUGH_OFF, 0,
		   &media);
	if (!media.local.ipver) {
		XCALL_TRACE(leg, "the media description was refused");
		return false;
	}

	transport_fill(leg, &media.transport);
	media.tx_silence = leg->held_local;

	if (!leg->media_configured ||
	    memcmp(&media, &leg->media, sizeof(media))) {
		st = ops->config_set(ops->priv, leg->line, leg->slot, &media);
		if (st != HAL_OK) {
			XCALL_TRACE(leg, "stream config refused (%s)",
				    hal_status_name(st));
			return false;
		}
		leg->media = media;
		leg->media_configured = true;
	}

	if (start && !leg->media_started && ops->start) {
		st = ops->start(ops->priv, leg->line, leg->slot);
		if (st != HAL_OK) {
			XCALL_TRACE(leg, "stream start refused (%s)",
				    hal_status_name(st));
			return false;
		}
		leg->media_started = true;
		leg->media_ran = true;
	}

	return true;
}

/**
 * ice_local_take() - read the local credentials and the fingerprint
 * @leg: the leg
 *
 * Return: true when the backend answered them.
 */
static bool ice_local_take(struct xcall *leg)
{
	const struct call_media_ops *ops = xv.env.media;
	struct hal_ice_local local;

	if (!ops || !ops->ice_local_get ||
	    ops->ice_local_get(ops->priv, leg->line, leg->slot, &local) !=
	    HAL_OK)
		return false;

	str_copy(leg->local.ufrag, sizeof(leg->local.ufrag), local.ufrag);
	str_copy(leg->local.pwd, sizeof(leg->local.pwd), local.pwd);
	memcpy(leg->local.fp, local.fp, sizeof(leg->local.fp));
	leg->local.fp_present = local.fp_hash == HAL_FP_SHA256;
	leg->local.setup = setup_to_jingle(setup_of_remote(leg));

	return leg->local.ufrag[0] && leg->local.fp_present;
}

/**
 * remote_take() - the credentials of the peer, from a content
 * @leg: the leg
 * @content: the content
 *
 * Return: true when the content named ICE credentials and a fingerprint.
 */
static bool remote_take(struct xcall *leg, const struct jingle_content *content)
{
	const struct jingle_transport *ice = &content->ice;

	if (!content->transport || !ice->ufrag[0] || !ice->pwd[0] ||
	    !ice->fp_present)
		return false;

	str_copy(leg->remote.ufrag, sizeof(leg->remote.ufrag), ice->ufrag);
	str_copy(leg->remote.pwd, sizeof(leg->remote.pwd), ice->pwd);
	memcpy(leg->remote.fp, ice->fp, sizeof(leg->remote.fp));
	leg->remote.setup = ice->setup;
	leg->remote.known = true;

	return true;
}

/**
 * remote_candidates_push() - hand the waiting candidates of the peer down
 * @leg: the leg
 *
 * The backend takes a candidate only once its transport exists, which the
 * first description makes, so the candidates wait until then.
 */
static void remote_candidates_push(struct xcall *leg)
{
	const struct call_media_ops *ops = xv.env.media;
	unsigned int i;

	if (!leg->media_configured || !ops || !ops->candidate_add)
		return;

	for (i = 0; i < leg->pendings; i++) {
		if (ops->candidate_add(ops->priv, leg->line, leg->slot,
				       leg->pending[i].sdp) != HAL_OK)
			XCALL_TRACE(leg, "the backend refused a candidate");
	}
	leg->pendings = 0;
}

/**
 * remote_candidates_add() - take the candidates of a content
 * @leg: the leg
 * @content: the content
 */
static void remote_candidates_add(struct xcall *leg,
				  const struct jingle_content *content)
{
	unsigned int i;

	for (i = 0; i < content->ice.candidates; i++) {
		if (leg->pendings >= JINGLE_CANDIDATES_MAX) {
			XCALL_TRACE(leg, "a candidate of the peer was dropped");
			break;
		}
		leg->pending[leg->pendings++] = content->ice.candidate[i];
	}

	remote_candidates_push(leg);
}

/**
 * session_on_wire() - whether the peer device holds our description
 * @leg: the leg
 *
 * Return: true once the initiate or the accept left.
 */
static bool session_on_wire(const struct xcall *leg)
{
	return leg->peer[0] && (leg->state == XCALL_INITIATED ||
				leg->state == XCALL_ACTIVE);
}

/**
 * candidates_flush() - send the local candidates the backend gathered
 * @leg: the leg
 *
 * The candidates stay in the backend until the session is on the wire, so
 * none is lost to a peer that cannot take it yet.
 */
static void candidates_flush(struct xcall *leg)
{
	const struct call_media_ops *ops = xv.env.media;
	char mem[JINGLE_STANZA_MAX];
	char line[JINGLE_CANDIDATE_MAX];
	char id[XCALL_ID_MAX];
	struct xml_w writer;
	unsigned int count = 0;

	if (!session_on_wire(leg) || !ops || !ops->candidate_next)
		return;

	id_next(leg, id, sizeof(id));
	xml_w_init(&writer, mem, sizeof(mem));
	jingle_open(&writer, leg->peer, id, JINGLE_TRANSPORT_INFO, leg->sid,
		    NULL, NULL);
	jingle_content_open(&writer, &leg->local);
	jingle_transport_open(&writer, &leg->local, false);

	while (count < XCALL_FLUSH_MAX &&
	       ops->candidate_next(ops->priv, leg->line, leg->slot, line,
				   sizeof(line)) == HAL_OK) {
		if (jingle_candidate_write(&writer, line))
			count++;
	}

	jingle_transport_close(&writer);
	jingle_content_close(&writer);
	jingle_close(&writer);

	if (!count)
		return;

	XCALL_TRACE(leg, "transport-info with %u candidates", count);
	jingle_send(leg, &writer, id, iq_ignore);
}

/**
 * session_send() - the initiate or the accept with the full description
 * @leg: the leg
 * @action: JINGLE_SESSION_INITIATE or JINGLE_SESSION_ACCEPT
 *
 * Return: true when the stream took it.
 */
static bool session_send(struct xcall *leg, enum jingle_action action)
{
	char mem[JINGLE_STANZA_MAX];
	char id[XCALL_ID_MAX];
	char self[JID_MAX] = "";
	struct jid bound;
	struct xml_w writer;

	if (xv.env.self_jid && xv.env.self_jid(xv.env.priv, leg->line, &bound))
		jid_full(&bound, self, sizeof(self));

	id_next(leg, id, sizeof(id));
	xml_w_init(&writer, mem, sizeof(mem));
	jingle_open(&writer, leg->peer, id, action, leg->sid,
		    action == JINGLE_SESSION_INITIATE ? self : leg->peer,
		    action == JINGLE_SESSION_ACCEPT ? self : NULL);
	jingle_content_open(&writer, &leg->local);
	jingle_description_write(&writer, &leg->local);
	jingle_transport_open(&writer, &leg->local, true);
	jingle_transport_close(&writer);
	jingle_content_close(&writer);
	jingle_close(&writer);

	XCALL_TRACE(leg, "%s to %s", jingle_action_name(action), leg->peer);

	return jingle_send(leg, &writer, id, iq_reply);
}

/**
 * session_fail() - end a session that cannot carry media
 * @leg: the leg
 * @reason: what the peer is told
 * @cls: what the shell is told
 */
static void session_fail(struct xcall *leg, enum jingle_reason reason,
			 enum proto_fail_class cls)
{
	terminate_send(leg, reason);
	leg_fail(leg, cls);
}

/* -------------------------------------------------------- the two ends */

/**
 * initiate_start() - the callee device answered proceed: open the session
 * @leg: the leg
 */
static void initiate_start(struct xcall *leg)
{
	leg->state = XCALL_INITIATED;
	guard_arm(leg, XCALL_SETUP_MS);

	if (!local_offer_build(leg)) {
		XCALL_TRACE(leg, "no codec to offer");
		leg_fail(leg, PROTO_FAIL_CONGESTION);
		return;
	}

	leg->codec = (enum sdp_codec_id)leg->loc.codec[0].id;
	leg->codec_pt = leg->loc.codec[0].pt;
	leg->dtmf_pt = leg->loc.dtmf_pt ? leg->loc.dtmf_pt : -1;

	if (!media_program(leg, true) || !ice_local_take(leg)) {
		leg_fail(leg, PROTO_FAIL_CONGESTION);
		return;
	}

	if (!session_send(leg, JINGLE_SESSION_INITIATE)) {
		leg_fail(leg, PROTO_FAIL_CONGESTION);
		return;
	}

	candidates_flush(leg);
}

/**
 * accept_take() - the session-accept of the peer
 * @leg: the leg
 * @msg: the parsed jingle
 * @iq: the stanza, answered here
 */
static void accept_take(struct xcall *leg, const struct jingle_msg *msg,
			const struct xml_doc *iq)
{
	unsigned int line = leg->line, slot = leg->slot;

	if (leg->state != XCALL_INITIATED) {
		xv.env.reply_error(xv.env.priv, line, iq, "cancel",
				   "unexpected-request");
		return;
	}

	xv.env.reply_result(xv.env.priv, line, iq);

	if (!msg->content.description || !payload_select(leg, &msg->content)) {
		session_fail(leg, JINGLE_REASON_FAILED_APPLICATION,
			     PROTO_FAIL_CONGESTION);
		return;
	}
	if (!remote_take(leg, &msg->content)) {
		session_fail(leg, JINGLE_REASON_SECURITY_ERROR,
			     PROTO_FAIL_CONGESTION);
		return;
	}

	if (!media_program(leg, true)) {
		session_fail(leg, JINGLE_REASON_MEDIA_ERROR,
			     PROTO_FAIL_CONGESTION);
		return;
	}
	remote_candidates_add(leg, &msg->content);

	leg->state = XCALL_ACTIVE;
	leg->answered = true;
	leg->answered_ms = now_ms();
	guard_arm(leg, XCALL_ICE_MS);
	XCALL_TRACE(leg, "accepted with %s pt %u dtmf %d",
		    sdp_codec_name(leg->codec), leg->codec_pt, leg->dtmf_pt);

	if (xv.env.cb && xv.env.cb->answered)
		xv.env.cb->answered(line, slot);

	candidates_flush(leg);
}

/**
 * accept_send() - answer the session of the caller
 * @leg: the leg
 */
static void accept_send(struct xcall *leg)
{
	if (!media_program(leg, true) || !ice_local_take(leg)) {
		session_fail(leg, JINGLE_REASON_MEDIA_ERROR,
			     PROTO_FAIL_CONGESTION);
		return;
	}
	remote_candidates_push(leg);

	if (!session_send(leg, JINGLE_SESSION_ACCEPT)) {
		leg_fail(leg, PROTO_FAIL_CONGESTION);
		return;
	}

	leg->state = XCALL_ACTIVE;
	leg->answered = true;
	leg->answered_ms = now_ms();
	guard_arm(leg, XCALL_ICE_MS);
	candidates_flush(leg);
}

/**
 * offer_announce() - ring the shell for an offered call
 * @leg: the leg
 */
static void offer_announce(struct xcall *leg)
{
	unsigned int line = leg->line, slot = leg->slot;

	if (xv.env.contact_name)
		xv.env.contact_name(xv.env.priv, line, leg->peer_bare,
				    leg->name, sizeof(leg->name));

	leg->state = XCALL_OFFERED;
	guard_arm(leg, XCALL_OFFER_MS);

	if (xv.env.cb && xv.env.cb->incoming)
		xv.env.cb->incoming(line, slot, leg->peer_bare, leg->name,
				    false, false, 0);
}

/**
 * initiate_take() - the session-initiate of a caller
 * @line: the line
 * @msg: the parsed jingle
 * @iq: the stanza, answered here
 * @from: the sender
 *
 * A session behind a proceed is accepted at once. A session nobody proposed
 * is a direct Jingle call: it rings here like a propose does.
 */
static void initiate_take(unsigned int line, const struct jingle_msg *msg,
			  const struct xml_doc *iq, const char *from)
{
	struct xcall *leg = leg_find_sid(line, msg->sid);
	unsigned int slot;
	bool direct = false;

	if (leg && leg->state != XCALL_PROCEEDED) {
		xv.env.reply_error(xv.env.priv, line, iq, "cancel",
				   "unexpected-request");
		return;
	}

	if (!leg) {
		if (!xcall_slot_free(line, &slot) ||
		    !(leg = leg_alloc(line, slot, true))) {
			xv.env.reply_error(xv.env.priv, line, iq, "wait",
					   "resource-constraint");
			return;
		}
		direct = true;
		str_copy(leg->sid, sizeof(leg->sid), msg->sid);
		str_copy(leg->peer, sizeof(leg->peer), from);
		jid_text_bare(from, leg->peer_bare, sizeof(leg->peer_bare));
	}

	xv.env.reply_result(xv.env.priv, line, iq);

	if (!local_offer_build(leg) || !msg->content.description ||
	    !payload_select(leg, &msg->content)) {
		session_fail(leg, JINGLE_REASON_FAILED_APPLICATION,
			     direct ? PROTO_FAIL_QUIET : PROTO_FAIL_CONGESTION);
		return;
	}
	if (!remote_take(leg, &msg->content)) {
		session_fail(leg, JINGLE_REASON_SECURITY_ERROR,
			     direct ? PROTO_FAIL_QUIET : PROTO_FAIL_CONGESTION);
		return;
	}
	local_answer_build(leg, &msg->content);
	remote_candidates_add(leg, &msg->content);

	if (!direct) {
		accept_send(leg);
		return;
	}

	info_send(leg, JINGLE_INFO_RINGING);
	offer_announce(leg);
}

/**
 * terminate_take() - the session-terminate of the peer
 * @leg: the leg
 * @msg: the parsed jingle
 */
static void terminate_take(struct xcall *leg, const struct jingle_msg *msg)
{
	unsigned int line = leg->line, slot = leg->slot;
	enum proto_fail_class cls;

	XCALL_TRACE(leg, "the peer ended the session: %s",
		    jingle_reason_name(msg->reason));
	leg_clear(leg);

	if (leg->answered) {
		leg_bye(leg);
		return;
	}

	if (leg->inbound) {
		leg_release(leg);
		if (xv.env.cb && xv.env.cb->remote_cancel)
			xv.env.cb->remote_cancel(line, slot);
		return;
	}

	switch (msg->reason) {
	case JINGLE_REASON_BUSY:
	case JINGLE_REASON_DECLINE:
		cls = PROTO_FAIL_BUSY;
		break;
	case JINGLE_REASON_GONE:
	case JINGLE_REASON_CANCEL:
	case JINGLE_REASON_TIMEOUT:
	case JINGLE_REASON_EXPIRED:
		cls = PROTO_FAIL_QUIET;
		break;
	default:
		cls = PROTO_FAIL_CONGESTION;
		break;
	}
	leg_fail(leg, cls);
}

/**
 * info_take() - the session-info of the peer
 * @leg: the leg
 * @msg: the parsed jingle
 */
static void info_take(struct xcall *leg, const struct jingle_msg *msg)
{
	unsigned int line = leg->line, slot = leg->slot;

	switch (msg->info) {
	case JINGLE_INFO_RINGING:
		if (leg->state == XCALL_INITIATED && xv.env.cb &&
		    xv.env.cb->proceeding)
			xv.env.cb->proceeding(line, slot, false);
		return;
	case JINGLE_INFO_HOLD:
	case JINGLE_INFO_UNHOLD:
		leg->held_remote = msg->info == JINGLE_INFO_HOLD;
		if (leg->state == XCALL_ACTIVE && xv.env.cb &&
		    xv.env.cb->hold_changed)
			xv.env.cb->hold_changed(line, slot, leg->held_remote);
		return;
	default:
		return;
	}
}

bool xcall_jingle_rx(unsigned int line, const struct xml_doc *iq)
{
	struct jingle_msg msg;
	struct xcall *leg;
	const char *from = xml_attr_get(iq, 0, "from");

	if (!jingle_parse(iq, &msg) || !xv.env.reply_result ||
	    !xv.env.reply_error)
		return false;

	if (!from)
		from = "";

	if (msg.action == JINGLE_SESSION_INITIATE) {
		initiate_take(line, &msg, iq, from);
		return true;
	}

	leg = leg_find_sid(line, msg.sid);
	if (!leg || (leg->peer[0] && strcmp(leg->peer, from))) {
		xv.env.reply_error(xv.env.priv, line, iq, "cancel",
				   "item-not-found");
		return true;
	}

	switch (msg.action) {
	case JINGLE_SESSION_ACCEPT:
		accept_take(leg, &msg, iq);
		return true;
	case JINGLE_SESSION_TERMINATE:
		xv.env.reply_result(xv.env.priv, line, iq);
		terminate_take(leg, &msg);
		return true;
	case JINGLE_SESSION_INFO:
		xv.env.reply_result(xv.env.priv, line, iq);
		info_take(leg, &msg);
		return true;
	case JINGLE_TRANSPORT_INFO:
		xv.env.reply_result(xv.env.priv, line, iq);
		remote_candidates_add(leg, &msg.content);
		return true;
	default:
		xv.env.reply_error(xv.env.priv, line, iq, "cancel",
				   "feature-not-implemented");
		return true;
	}
}

/* --------------------------------------------------------------------- JMI */

/**
 * self_bare() - the bare JID of this account
 * @line: the line
 * @out: receives it, "" when the stream bound nothing
 * @len: capacity of @out
 */
static void self_bare(unsigned int line, char *out, size_t len)
{
	struct jid bound;

	out[0] = 0;
	if (xv.env.self_jid && xv.env.self_jid(xv.env.priv, line, &bound))
		jid_bare(&bound, out, len);
}

/**
 * propose_take() - a caller rings this account
 * @line: the line
 * @msg: the parsed message
 */
static void propose_take(unsigned int line, const struct jmi_msg *msg)
{
	struct xcall *leg = leg_find_sid(line, msg->id);
	unsigned int slot;

	if (leg || !msg->audio || msg->carbon)
		return;

	if (!xcall_slot_free(line, &slot) ||
	    !(leg = leg_alloc(line, slot, true))) {
		struct xcall refusal;

		memset(&refusal, 0, sizeof(refusal));
		refusal.line = line;
		str_copy(refusal.sid, sizeof(refusal.sid), msg->id);
		jmi_send(&refusal, JMI_REJECT, msg->from);
		return;
	}

	leg->jmi = true;
	str_copy(leg->sid, sizeof(leg->sid), msg->id);
	str_copy(leg->peer, sizeof(leg->peer), msg->from);
	jid_text_bare(msg->from, leg->peer_bare, sizeof(leg->peer_bare));
	jmi_send(leg, JMI_RINGING, leg->peer);
	offer_announce(leg);
}

/**
 * elsewhere_take() - another device of this account took or refused the call
 * @leg: the leg
 */
static void elsewhere_take(struct xcall *leg)
{
	unsigned int line = leg->line, slot = leg->slot;

	XCALL_TRACE(leg, "answered elsewhere");
	leg_clear(leg);
	leg_release(leg);

	if (!xv.env.cb)
		return;
	if (xv.env.cb->answered_elsewhere)
		xv.env.cb->answered_elsewhere(line, slot);
	else if (xv.env.cb->remote_cancel)
		xv.env.cb->remote_cancel(line, slot);
}

/**
 * retract_take() - the caller withdrew the offer
 * @leg: the leg
 */
static void retract_take(struct xcall *leg)
{
	unsigned int line = leg->line, slot = leg->slot;

	leg_clear(leg);
	leg_release(leg);
	if (xv.env.cb && xv.env.cb->remote_cancel)
		xv.env.cb->remote_cancel(line, slot);
}

/**
 * callee_take() - a device of the callee answered the propose
 * @leg: the leg
 * @msg: the parsed message
 */
static void callee_take(struct xcall *leg, const struct jmi_msg *msg)
{
	unsigned int line = leg->line, slot = leg->slot;

	switch (msg->kind) {
	case JMI_RINGING:
		if (xv.env.cb && xv.env.cb->proceeding)
			xv.env.cb->proceeding(line, slot, false);
		return;
	case JMI_PROCEED:
		str_copy(leg->peer, sizeof(leg->peer), msg->from);
		initiate_start(leg);
		return;
	case JMI_REJECT:
		leg_fail(leg, PROTO_FAIL_BUSY);
		return;
	default:
		return;
	}
}

bool xcall_jmi_rx(unsigned int line, const struct xml_doc *message)
{
	struct jmi_msg msg;
	struct xcall *leg;
	char self[JID_MAX];
	bool own;

	if (!jmi_parse(message, &msg))
		return false;

	if (msg.kind == JMI_PROPOSE) {
		propose_take(line, &msg);
		return true;
	}

	leg = leg_find_sid(line, msg.id);
	if (!leg)
		return true;

	self_bare(line, self, sizeof(self));
	own = msg.carbon_sent ||
	      (self[0] && jid_text_bare_eq(msg.from, self));

	if (leg->inbound) {
		if (leg->state != XCALL_OFFERED)
			return true;
		if (own && (msg.kind == JMI_PROCEED || msg.kind == JMI_ACCEPT ||
			    msg.kind == JMI_REJECT))
			elsewhere_take(leg);
		else if (!own && msg.kind == JMI_RETRACT)
			retract_take(leg);
		return true;
	}

	if (leg->state == XCALL_PROPOSED && !own &&
	    jid_text_bare_eq(msg.from, leg->peer_bare))
		callee_take(leg, &msg);

	return true;
}

/* ------------------------------------------------------------ transport */

void xcall_transport_event(unsigned int line, unsigned int stream,
			   unsigned int what, unsigned int state)
{
	struct xcall *leg = leg_get(line, stream);

	if (!leg)
		return;

	switch (what) {
	case HAL_TRANSPORT_EV_CANDIDATE:
		candidates_flush(leg);
		return;
	case HAL_TRANSPORT_EV_ICE_STATE:
		if (state != HAL_ICE_FAILED)
			return;
		XCALL_TRACE(leg, "ice failed%s",
			    leg->relay ? "" : " without a relay");
		if (!leg->relay && xv.env.relay_missing)
			xv.env.relay_missing(xv.env.priv, line);
		session_fail(leg, JINGLE_REASON_CONNECTIVITY_ERROR,
			     PROTO_FAIL_CONGESTION);
		return;
	case HAL_TRANSPORT_EV_DTLS_UP:
		leg->dtls_up = true;
		guard_cancel(leg);
		XCALL_TRACE(leg, "media flows");
		return;
	case HAL_TRANSPORT_EV_DTLS_FAIL:
		session_fail(leg, JINGLE_REASON_SECURITY_ERROR,
			     PROTO_FAIL_CONGESTION);
		return;
	case HAL_TRANSPORT_EV_DTLS_CLOSED:
		if (leg->state != XCALL_ACTIVE || leg->dtls_closed)
			return;
		leg->dtls_closed = true;
		XCALL_TRACE(leg, "the peer closed the media");
		guard_arm(leg, XCALL_CLOSE_MS);
		return;
	default:
		return;
	}
}

/**
 * guard_fire() - the guard of a leg expired
 * @timer: the timer
 */
static void guard_fire(struct proto_timer *timer)
{
	struct xcall *leg = timer->owner;
	unsigned int line = leg->line, slot = leg->slot;

	if (!leg->in_use)
		return;

	XCALL_TRACE(leg, "guard expired in %s", xcall_state_name(leg->state));

	switch (leg->state) {
	case XCALL_PROPOSED:
		jmi_send(leg, JMI_RETRACT, leg->target);
		leg_fail(leg, PROTO_FAIL_UNOBTAINABLE);
		return;
	case XCALL_OFFERED:
		leg_clear(leg);
		leg_release(leg);
		if (xv.env.cb && xv.env.cb->remote_cancel)
			xv.env.cb->remote_cancel(line, slot);
		return;
	case XCALL_PROCEEDED:
		leg_fail(leg, PROTO_FAIL_CONGESTION);
		return;
	case XCALL_INITIATED:
		session_fail(leg, JINGLE_REASON_TIMEOUT, PROTO_FAIL_CONGESTION);
		return;
	case XCALL_ACTIVE:
		if (leg->dtls_closed) {
			leg_clear(leg);
			terminate_send(leg, JINGLE_REASON_SUCCESS);
			leg_bye(leg);
			return;
		}
		if (leg->dtls_up)
			return;
		if (!leg->relay && xv.env.relay_missing)
			xv.env.relay_missing(xv.env.priv, line);
		session_fail(leg, JINGLE_REASON_CONNECTIVITY_ERROR,
			     PROTO_FAIL_CONGESTION);
		return;
	default:
		return;
	}
}

/* --------------------------------------------------------------- commands */

bool xcall_slot_free(unsigned int line, unsigned int *slot)
{
	unsigned int i;

	if (line >= CFG_CHANNEL_MAX || !slot)
		return false;

	for (i = 0; i < CALL_SLOTS; i++) {
		if (!legs[line][i].in_use) {
			*slot = i;
			return true;
		}
	}

	return false;
}

unsigned int xcall_slot_stream(unsigned int line, unsigned int ui_slot)
{
	return leg_get(line, ui_slot) ? ui_slot : CALL_SLOTS;
}

bool xcall_line_dial_ready(unsigned int line)
{
	return line < CFG_CHANNEL_MAX && xv.env.ready &&
	       xv.env.ready(xv.env.priv, line);
}

/**
 * target_resolve() - the JID a dialled string names
 * @line: the line
 * @target: the string
 * @out: receives the JID
 * @len: capacity of @out
 *
 * Return: true when the string is a JID or the speed dial maps it.
 */
static bool target_resolve(unsigned int line, const char *target, char *out,
			   size_t len)
{
	struct jid jid;

	if (!target || !target[0])
		return false;

	if (strchr(target, '@')) {
		if (!jid_parse(target, &jid) || !jid.local[0])
			return false;
		str_copy(out, len, target);
		return true;
	}

	return xv.env.speed_dial &&
	       xv.env.speed_dial(xv.env.priv, line, target, out, len) &&
	       jid_parse(out, &jid) && jid.local[0];
}

bool xcall_dial(unsigned int line, unsigned int slot, const char *target)
{
	char jid[XCALL_TARGET_MAX];
	struct xcall *leg;

	if (line >= CFG_CHANNEL_MAX || slot >= CALL_SLOTS || !target ||
	    !target[0])
		return false;

	if (!target_resolve(line, target, jid, sizeof(jid))) {
		if (xv.env.cb && xv.env.cb->failed)
			xv.env.cb->failed(line, slot, PROTO_FAIL_UNOBTAINABLE);
		return false;
	}

	if (!xcall_line_dial_ready(line) ||
	    !(leg = leg_alloc(line, slot, false))) {
		if (xv.env.cb && xv.env.cb->failed)
			xv.env.cb->failed(line, slot, PROTO_FAIL_CONGESTION);
		return false;
	}

	leg->jmi = true;
	str_copy(leg->target, sizeof(leg->target), jid);
	jid_text_bare(jid, leg->peer_bare, sizeof(leg->peer_bare));
	if (xv.env.contact_name)
		xv.env.contact_name(xv.env.priv, line, leg->peer_bare,
				    leg->name, sizeof(leg->name));
	id_next(leg, leg->sid, sizeof(leg->sid));
	leg->state = XCALL_PROPOSED;
	guard_arm(leg, XCALL_RING_MS);

	if (!jmi_send(leg, JMI_PROPOSE, jid)) {
		leg_fail(leg, PROTO_FAIL_CONGESTION);
		return false;
	}

	return true;
}

bool xcall_ring(unsigned int line, unsigned int slot)
{
	struct xcall *leg = leg_get(line, slot);

	return leg && leg->state == XCALL_OFFERED;
}

bool xcall_answer(unsigned int line, unsigned int slot)
{
	struct xcall *leg = leg_get(line, slot);
	char self[JID_MAX];

	if (!leg || leg->state != XCALL_OFFERED)
		return false;

	if (!leg->jmi) {
		accept_send(leg);
		return leg->in_use;
	}

	leg->state = XCALL_PROCEEDED;
	guard_arm(leg, XCALL_SETUP_MS);
	if (!jmi_send(leg, JMI_PROCEED, leg->peer)) {
		leg_fail(leg, PROTO_FAIL_CONGESTION);
		return false;
	}

	self_bare(line, self, sizeof(self));
	if (self[0])
		jmi_send(leg, JMI_ACCEPT, self);

	return true;
}

bool xcall_reject(unsigned int line, unsigned int slot, uint32_t code)
{
	struct xcall *leg = leg_get(line, slot);

	if (!leg || leg->state != XCALL_OFFERED)
		return false;

	leg_clear(leg);
	if (leg->jmi)
		jmi_send(leg, JMI_REJECT, leg->peer);
	else
		terminate_send(leg, JINGLE_REASON_DECLINE);
	leg_release(leg);

	return true;
}

bool xcall_hangup(unsigned int line, unsigned int slot)
{
	struct xcall *leg = leg_get(line, slot);

	if (!leg)
		return false;

	leg_clear(leg);
	switch (leg->state) {
	case XCALL_PROPOSED:
		jmi_send(leg, JMI_RETRACT, leg->target);
		break;
	case XCALL_OFFERED:
		if (leg->jmi)
			jmi_send(leg, JMI_REJECT, leg->peer);
		else
			terminate_send(leg, JINGLE_REASON_DECLINE);
		break;
	case XCALL_PROCEEDED:
		terminate_send(leg, JINGLE_REASON_CANCEL);
		break;
	case XCALL_INITIATED:
		terminate_send(leg, JINGLE_REASON_CANCEL);
		break;
	case XCALL_ACTIVE:
		terminate_send(leg, JINGLE_REASON_SUCCESS);
		break;
	default:
		break;
	}
	leg_release(leg);

	return true;
}

/**
 * hold_set() - move the local hold of an active leg
 * @line: the line
 * @slot: the slot
 * @hold: the new state
 *
 * Return: true when the stream and the peer took it.
 */
static bool hold_set(unsigned int line, unsigned int slot, bool hold)
{
	struct xcall *leg = leg_get(line, slot);

	if (!leg || leg->state != XCALL_ACTIVE)
		return false;

	leg->held_local = hold;
	if (!media_program(leg, true)) {
		leg->held_local = !hold;
		return false;
	}
	if (!info_send(leg, hold ? JINGLE_INFO_HOLD : JINGLE_INFO_UNHOLD))
		return false;

	if (xv.env.cb && xv.env.cb->reinvite_done)
		xv.env.cb->reinvite_done(line, slot, true);

	return true;
}

bool xcall_hold(unsigned int line, unsigned int slot)
{
	return hold_set(line, slot, true);
}

bool xcall_resume(unsigned int line, unsigned int slot)
{
	return hold_set(line, slot, false);
}

bool xcall_dtmf(unsigned int line, unsigned int slot, char digit,
		uint16_t duration_ms, bool measured)
{
	struct xcall *leg = leg_get(line, slot);
	const struct call_media_ops *ops = xv.env.media;

	if (!leg || leg->state != XCALL_ACTIVE || leg->dtmf_pt < 0 ||
	    !leg->media_started || !ops || !ops->dtmf_send)
		return false;

	if (measured && (xv.hal_features & HAL_FEAT_DTMF_RELAY))
		return true;

	return ops->dtmf_send(ops->priv, line, slot, digit, duration_ms) ==
	       HAL_OK;
}

/* -------------------------------------------------------------- lifecycle */

void xcalls_init(const struct xcall_env *env)
{
	memset(&xv, 0, sizeof(xv));
	memset(legs, 0, sizeof(legs));
	if (env)
		xv.env = *env;
	xv.inited = true;
}

void xcalls_reload(const struct cfg *cfg)
{
	xv.cfg = cfg;
}

void xcalls_hal_features_set(uint32_t features)
{
	xv.hal_features = features;
}

void xcalls_hal_codecs_set(uint32_t codecs)
{
	xv.hal_codecs = codecs;
}

/**
 * line_end_all() - end every leg of a line
 * @line: the line
 * @terminate: whether the peer is told
 * @cls: the failure class the shell hears
 */
static void line_end_all(unsigned int line, bool terminate,
			 enum proto_fail_class cls)
{
	struct xcall *leg;
	unsigned int slot;

	if (line >= CFG_CHANNEL_MAX)
		return;

	for (slot = 0; slot < CALL_SLOTS; slot++) {
		leg = &legs[line][slot];
		if (!leg->in_use)
			continue;
		if (terminate && leg->state == XCALL_PROPOSED)
			jmi_send(leg, JMI_RETRACT, leg->target);
		else if (terminate && session_on_wire(leg))
			terminate_send(leg, leg->answered ?
					    JINGLE_REASON_SUCCESS :
					    JINGLE_REASON_CANCEL);
		leg_fail(leg, cls);
	}
}

void xcalls_line_teardown(unsigned int line)
{
	line_end_all(line, true, PROTO_FAIL_QUIET);
}

void xcalls_stream_down(unsigned int line)
{
	line_end_all(line, false, PROTO_FAIL_QUIET);
}

bool xcalls_line_busy(unsigned int line)
{
	unsigned int slot;

	if (line >= CFG_CHANNEL_MAX)
		return false;

	for (slot = 0; slot < CALL_SLOTS; slot++) {
		if (legs[line][slot].in_use)
			return true;
	}

	return false;
}

void xcalls_done(void)
{
	unsigned int line;

	for (line = 0; line < CFG_CHANNEL_MAX; line++)
		line_end_all(line, false, PROTO_FAIL_QUIET);

	xv.inited = false;
}

void xcalls_status_dump(struct blob_buf *b, unsigned int line)
{
	struct xcall *leg;
	unsigned int slot;
	void *arr, *tbl;

	arr = blobmsg_open_array(b, "jingle");
	if (line < CFG_CHANNEL_MAX) {
		for (slot = 0; slot < CALL_SLOTS; slot++) {
			leg = &legs[line][slot];
			if (!leg->in_use)
				continue;
			tbl = blobmsg_open_table(b, NULL);
			blobmsg_add_u32(b, "slot", slot);
			blobmsg_add_string(b, "state",
					   xcall_state_name(leg->state));
			blobmsg_add_string(b, "sid", leg->sid);
			blobmsg_add_string(b, "peer", leg->peer);
			blobmsg_add_u8(b, "inbound", leg->inbound);
			blobmsg_add_u8(b, "answered", leg->answered);
			blobmsg_add_u8(b, "media", leg->dtls_up);
			blobmsg_close_table(b, tbl);
		}
	}
	blobmsg_close_array(b, arr);
}
