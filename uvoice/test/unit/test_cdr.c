// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the call record ring (src/cdr.c).
 *
 * Isolation. cdr.c stores what it is given and renders it, so this suite
 * needs no HAL, and uloop only runs its time windows. The suite calls the
 * edges by hand. It reads the result back through the three dumps,
 * cdr_dump(), cdr_stats_dump() and cdr_jb_dump(), so the northbound keys stay
 * under test.
 *
 * The record is struct proto_call_ended (proto_media.h). The module names the
 * clearing class through proto_fail_name() and the codec through
 * hal_codec_name(), both inline in that header, so the suite links no leg
 * layer. HAL_STREAM_MAX (hal.h) bounds the stream of the record keys.
 *
 * The suite checks time as relations. It checks only that the wall clock
 * start is a plausible epoch value. It compares the two millisecond fields,
 * duration_ms and answer_offset_ms, with each other and with a loop window of
 * known length (window_run()). A check of absolute values would make the suite
 * a clock test.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <libubox/blobmsg.h>
#include <libubox/utils.h>

#include "cdr.h"
#include "cfg/cfg.h"
#include "hal/hal.h"
#include "proto/proto_media.h"
#include "test_util.h"
#include "test_uloop.h"

/* 2026-01-01, a date the sandbox clock is always past */
#define EPOCH_FLOOR		1767225600ull
#define VIEW_MAX		(CDR_RING + 4)

static struct blob_buf b;

/* ------------------------------------------------------------- the views */

struct media_view {
	uint64_t packets_sent;
	uint64_t packets_recv;
	uint64_t packets_lost;
	uint64_t packets_discarded;
	uint64_t loss_rate;
	uint64_t loss_rate_far;
	uint64_t jitter_avg;
	uint64_t jitter_max;
	uint64_t jitter_far;
	uint64_t rtt_avg;
	uint64_t rtt_max;
	uint64_t bytes_sent;
	uint64_t bytes_recv;
	char mos_lq[MOS_TEXT_MAX];
	char mos_cq[MOS_TEXT_MAX];
	uint32_t rfactor;
};

struct jb_view {
	char mode[16];
	uint64_t frames_out;
	uint64_t plc;
	uint64_t cn;
	uint64_t silence;
	uint64_t late_drop;
	uint64_t dup_drop;
	uint64_t overflow_drop;
	uint64_t hold_drop;
	uint64_t resync_discard_ms;
	uint64_t resyncs;
	uint64_t starve_resyncs;
	uint64_t cn_reanchors;
	uint64_t underruns;
	uint64_t skew_drops;
	uint64_t skew_inserts;
	uint32_t target_ms;
	uint32_t depth_ms;
	uint32_t jitter_ms;
	uint32_t pool_free;
};

struct cdr_view {
	uint32_t seq;
	uint32_t line;
	uint32_t slot;
	uint32_t duration_ms;
	uint32_t answer_offset_ms;
	uint32_t ptime;
	uint32_t rtp_port;
	uint64_t start;
	char fail[32];
	char number[80];
	char remote[80];
	char name[80];
	char codec[16];
	char media_state[16];
	struct media_view media;
	struct jb_view jb;
	bool has_jb;
	bool inbound;
	bool answered;
	bool in_progress;
	bool transferred;
	bool has_transferred;
	uint32_t transferred_from;
	bool has_transferred_from;
	bool conferenced;
	bool has_conferenced;
	bool has_fail;
	bool has_number;
	bool has_remote;
	bool has_name;
	bool has_codec;
	bool has_ptime;
	bool has_rtp_port;
	bool has_answer_offset;
	bool has_media;
};

enum {
	CV_SEQ,
	CV_LINE,
	CV_SLOT,
	CV_INBOUND,
	CV_ANSWERED,
	CV_FAIL,
	CV_NUMBER,
	CV_REMOTE,
	CV_NAME,
	CV_START,
	CV_DURATION,
	CV_ANSWER_OFFSET,
	CV_CODEC,
	CV_PTIME,
	CV_RTP_PORT,
	CV_MEDIA_STATE,
	CV_MEDIA,
	CV_JB,
	CV_IN_PROGRESS,
	CV_TRANSFERRED,
	CV_TRANSFERRED_FROM,
	CV_CONFERENCED,
	__CV_MAX,
};

static const struct blobmsg_policy cv_policy[__CV_MAX] = {
	[CV_SEQ] = { "seq", BLOBMSG_TYPE_INT32 },
	[CV_LINE] = { "line", BLOBMSG_TYPE_INT32 },
	[CV_SLOT] = { "slot", BLOBMSG_TYPE_INT32 },
	[CV_INBOUND] = { "inbound", BLOBMSG_TYPE_BOOL },
	[CV_ANSWERED] = { "answered", BLOBMSG_TYPE_BOOL },
	[CV_FAIL] = { "fail", BLOBMSG_TYPE_STRING },
	[CV_NUMBER] = { "number", BLOBMSG_TYPE_STRING },
	[CV_REMOTE] = { "remote", BLOBMSG_TYPE_STRING },
	[CV_NAME] = { "name", BLOBMSG_TYPE_STRING },
	[CV_START] = { "start", BLOBMSG_TYPE_INT64 },
	[CV_DURATION] = { "duration_ms", BLOBMSG_TYPE_INT32 },
	[CV_ANSWER_OFFSET] = { "answer_offset_ms", BLOBMSG_TYPE_INT32 },
	[CV_CODEC] = { "codec", BLOBMSG_TYPE_STRING },
	[CV_PTIME] = { "ptime", BLOBMSG_TYPE_INT32 },
	[CV_RTP_PORT] = { "rtp_port", BLOBMSG_TYPE_INT32 },
	[CV_MEDIA_STATE] = { "media_state", BLOBMSG_TYPE_STRING },
	[CV_MEDIA] = { "media", BLOBMSG_TYPE_TABLE },
	[CV_JB] = { "jb", BLOBMSG_TYPE_TABLE },
	[CV_IN_PROGRESS] = { "in_progress", BLOBMSG_TYPE_BOOL },
	[CV_TRANSFERRED] = { "transferred", BLOBMSG_TYPE_BOOL },
	[CV_TRANSFERRED_FROM] = { "transferred_from", BLOBMSG_TYPE_INT32 },
	[CV_CONFERENCED] = { "conferenced", BLOBMSG_TYPE_BOOL },
};

enum {
	MV_PACKETS_SENT,
	MV_PACKETS_RECV,
	MV_PACKETS_LOST,
	MV_PACKETS_DISCARDED,
	MV_LOSS_RATE,
	MV_LOSS_RATE_FAR,
	MV_JITTER_AVG,
	MV_JITTER_MAX,
	MV_JITTER_FAR,
	MV_RTT_AVG,
	MV_RTT_MAX,
	MV_BYTES_SENT,
	MV_BYTES_RECV,
	MV_MOS_LQ,
	MV_MOS_CQ,
	MV_RFACTOR,
	__MV_MAX,
};

static const struct blobmsg_policy mv_policy[__MV_MAX] = {
	[MV_PACKETS_SENT] = { "packets_sent", BLOBMSG_TYPE_INT64 },
	[MV_PACKETS_RECV] = { "packets_recv", BLOBMSG_TYPE_INT64 },
	[MV_PACKETS_LOST] = { "packets_lost", BLOBMSG_TYPE_INT64 },
	[MV_PACKETS_DISCARDED] = { "packets_discarded", BLOBMSG_TYPE_INT64 },
	[MV_LOSS_RATE] = { "loss_rate", BLOBMSG_TYPE_INT64 },
	[MV_LOSS_RATE_FAR] = { "loss_rate_far", BLOBMSG_TYPE_INT64 },
	[MV_JITTER_AVG] = { "jitter_avg", BLOBMSG_TYPE_INT64 },
	[MV_JITTER_MAX] = { "jitter_max", BLOBMSG_TYPE_INT64 },
	[MV_JITTER_FAR] = { "jitter_far", BLOBMSG_TYPE_INT64 },
	[MV_RTT_AVG] = { "rtt_avg", BLOBMSG_TYPE_INT64 },
	[MV_RTT_MAX] = { "rtt_max", BLOBMSG_TYPE_INT64 },
	[MV_BYTES_SENT] = { "bytes_sent", BLOBMSG_TYPE_INT64 },
	[MV_BYTES_RECV] = { "bytes_recv", BLOBMSG_TYPE_INT64 },
	[MV_MOS_LQ] = { "mos_lq", BLOBMSG_TYPE_STRING },
	[MV_MOS_CQ] = { "mos_cq", BLOBMSG_TYPE_STRING },
	[MV_RFACTOR] = { "rfactor", BLOBMSG_TYPE_INT32 },
};

struct stats_view {
	uint32_t in_received;
	uint32_t in_answered;
	uint32_t in_failed;
	uint32_t out_attempted;
	uint32_t out_answered;
	uint32_t out_failed;
	uint64_t packets_sent;
	uint64_t packets_recv;
	uint64_t packets_lost;
	uint64_t bytes_sent;
	uint64_t bytes_recv;
	bool found;
};

enum {
	SV_LINE,
	SV_IN_RECEIVED,
	SV_IN_ANSWERED,
	SV_IN_FAILED,
	SV_OUT_ATTEMPTED,
	SV_OUT_ANSWERED,
	SV_OUT_FAILED,
	SV_TOTALS,
	__SV_MAX,
};

static const struct blobmsg_policy sv_policy[__SV_MAX] = {
	[SV_LINE] = { "line", BLOBMSG_TYPE_INT32 },
	[SV_IN_RECEIVED] = { "in_received", BLOBMSG_TYPE_INT32 },
	[SV_IN_ANSWERED] = { "in_answered", BLOBMSG_TYPE_INT32 },
	[SV_IN_FAILED] = { "in_failed", BLOBMSG_TYPE_INT32 },
	[SV_OUT_ATTEMPTED] = { "out_attempted", BLOBMSG_TYPE_INT32 },
	[SV_OUT_ANSWERED] = { "out_answered", BLOBMSG_TYPE_INT32 },
	[SV_OUT_FAILED] = { "out_failed", BLOBMSG_TYPE_INT32 },
	[SV_TOTALS] = { "totals", BLOBMSG_TYPE_TABLE },
};

enum {
	TV_PACKETS_SENT,
	TV_PACKETS_RECV,
	TV_PACKETS_LOST,
	TV_BYTES_SENT,
	TV_BYTES_RECV,
	__TV_MAX,
};

static const struct blobmsg_policy tv_policy[__TV_MAX] = {
	[TV_PACKETS_SENT] = { "packets_sent", BLOBMSG_TYPE_INT64 },
	[TV_PACKETS_RECV] = { "packets_recv", BLOBMSG_TYPE_INT64 },
	[TV_PACKETS_LOST] = { "packets_lost", BLOBMSG_TYPE_INT64 },
	[TV_BYTES_SENT] = { "bytes_sent", BLOBMSG_TYPE_INT64 },
	[TV_BYTES_RECV] = { "bytes_recv", BLOBMSG_TYPE_INT64 },
};

static void str_take(char *dst, size_t dlen, struct blob_attr *a)
{
	snprintf(dst, dlen, "%s", a ? blobmsg_get_string(a) : "");
}

static void media_take(struct blob_attr *table, struct media_view *m)
{
	struct blob_attr *tb[__MV_MAX] = {};

	memset(m, 0, sizeof(*m));
	blobmsg_parse(mv_policy, __MV_MAX, tb, blobmsg_data(table),
		      blobmsg_data_len(table));

	if (tb[MV_PACKETS_SENT])
		m->packets_sent = blobmsg_get_u64(tb[MV_PACKETS_SENT]);
	if (tb[MV_PACKETS_RECV])
		m->packets_recv = blobmsg_get_u64(tb[MV_PACKETS_RECV]);
	if (tb[MV_PACKETS_LOST])
		m->packets_lost = blobmsg_get_u64(tb[MV_PACKETS_LOST]);
	if (tb[MV_PACKETS_DISCARDED])
		m->packets_discarded = blobmsg_get_u64(tb[MV_PACKETS_DISCARDED]);
	if (tb[MV_LOSS_RATE])
		m->loss_rate = blobmsg_get_u64(tb[MV_LOSS_RATE]);
	if (tb[MV_LOSS_RATE_FAR])
		m->loss_rate_far = blobmsg_get_u64(tb[MV_LOSS_RATE_FAR]);
	if (tb[MV_JITTER_AVG])
		m->jitter_avg = blobmsg_get_u64(tb[MV_JITTER_AVG]);
	if (tb[MV_JITTER_MAX])
		m->jitter_max = blobmsg_get_u64(tb[MV_JITTER_MAX]);
	if (tb[MV_JITTER_FAR])
		m->jitter_far = blobmsg_get_u64(tb[MV_JITTER_FAR]);
	if (tb[MV_RTT_AVG])
		m->rtt_avg = blobmsg_get_u64(tb[MV_RTT_AVG]);
	if (tb[MV_RTT_MAX])
		m->rtt_max = blobmsg_get_u64(tb[MV_RTT_MAX]);
	if (tb[MV_BYTES_SENT])
		m->bytes_sent = blobmsg_get_u64(tb[MV_BYTES_SENT]);
	if (tb[MV_BYTES_RECV])
		m->bytes_recv = blobmsg_get_u64(tb[MV_BYTES_RECV]);
	if (tb[MV_MOS_LQ])
		str_take(m->mos_lq, sizeof(m->mos_lq), tb[MV_MOS_LQ]);
	if (tb[MV_MOS_CQ])
		str_take(m->mos_cq, sizeof(m->mos_cq), tb[MV_MOS_CQ]);
	if (tb[MV_RFACTOR])
		m->rfactor = blobmsg_get_u32(tb[MV_RFACTOR]);
}

enum {
	JV_MODE,
	JV_FRAMES_OUT, JV_PLC, JV_CN, JV_SILENCE, JV_LATE_DROP, JV_DUP_DROP,
	JV_OVERFLOW_DROP, JV_HOLD_DROP, JV_RESYNC_DISCARD_MS, JV_RESYNCS,
	JV_STARVE_RESYNCS,
	JV_CN_REANCHORS,
	JV_UNDERRUNS, JV_SKEW_DROPS, JV_SKEW_INSERTS, JV_TARGET_MS, JV_DEPTH_MS,
	JV_JITTER_MS, JV_POOL_FREE,
	__JV_MAX,
};

static const struct blobmsg_policy jv_policy[__JV_MAX] = {
	[JV_MODE] = { "mode", BLOBMSG_TYPE_STRING },
	[JV_FRAMES_OUT] = { "frames_out", BLOBMSG_TYPE_INT64 },
	[JV_PLC] = { "plc", BLOBMSG_TYPE_INT64 },
	[JV_CN] = { "cn", BLOBMSG_TYPE_INT64 },
	[JV_SILENCE] = { "silence", BLOBMSG_TYPE_INT64 },
	[JV_LATE_DROP] = { "late_drop", BLOBMSG_TYPE_INT64 },
	[JV_DUP_DROP] = { "dup_drop", BLOBMSG_TYPE_INT64 },
	[JV_OVERFLOW_DROP] = { "overflow_drop", BLOBMSG_TYPE_INT64 },
	[JV_HOLD_DROP] = { "hold_drop", BLOBMSG_TYPE_INT64 },
	[JV_RESYNC_DISCARD_MS] = { "resync_discard_ms", BLOBMSG_TYPE_INT64 },
	[JV_RESYNCS] = { "resyncs", BLOBMSG_TYPE_INT64 },
	[JV_STARVE_RESYNCS] = { "starve_resyncs", BLOBMSG_TYPE_INT64 },
	[JV_CN_REANCHORS] = { "cn_reanchors", BLOBMSG_TYPE_INT64 },
	[JV_UNDERRUNS] = { "underruns", BLOBMSG_TYPE_INT64 },
	[JV_SKEW_DROPS] = { "skew_drops", BLOBMSG_TYPE_INT64 },
	[JV_SKEW_INSERTS] = { "skew_inserts", BLOBMSG_TYPE_INT64 },
	[JV_TARGET_MS] = { "target_ms", BLOBMSG_TYPE_INT32 },
	[JV_DEPTH_MS] = { "depth_ms", BLOBMSG_TYPE_INT32 },
	[JV_JITTER_MS] = { "jitter_ms", BLOBMSG_TYPE_INT32 },
	[JV_POOL_FREE] = { "pool_free", BLOBMSG_TYPE_INT32 },
};

static void jb_take(struct blob_attr *table, struct jb_view *j)
{
	struct blob_attr *tb[__JV_MAX] = {};

	memset(j, 0, sizeof(*j));
	blobmsg_parse(jv_policy, __JV_MAX, tb, blobmsg_data(table),
		      blobmsg_data_len(table));

	if (tb[JV_MODE])
		str_take(j->mode, sizeof(j->mode), tb[JV_MODE]);
	if (tb[JV_FRAMES_OUT])
		j->frames_out = blobmsg_get_u64(tb[JV_FRAMES_OUT]);
	if (tb[JV_PLC])
		j->plc = blobmsg_get_u64(tb[JV_PLC]);
	if (tb[JV_CN])
		j->cn = blobmsg_get_u64(tb[JV_CN]);
	if (tb[JV_SILENCE])
		j->silence = blobmsg_get_u64(tb[JV_SILENCE]);
	if (tb[JV_LATE_DROP])
		j->late_drop = blobmsg_get_u64(tb[JV_LATE_DROP]);
	if (tb[JV_DUP_DROP])
		j->dup_drop = blobmsg_get_u64(tb[JV_DUP_DROP]);
	if (tb[JV_OVERFLOW_DROP])
		j->overflow_drop = blobmsg_get_u64(tb[JV_OVERFLOW_DROP]);
	if (tb[JV_HOLD_DROP])
		j->hold_drop = blobmsg_get_u64(tb[JV_HOLD_DROP]);
	if (tb[JV_RESYNC_DISCARD_MS])
		j->resync_discard_ms = blobmsg_get_u64(tb[JV_RESYNC_DISCARD_MS]);
	if (tb[JV_RESYNCS])
		j->resyncs = blobmsg_get_u64(tb[JV_RESYNCS]);
	if (tb[JV_STARVE_RESYNCS])
		j->starve_resyncs = blobmsg_get_u64(tb[JV_STARVE_RESYNCS]);
	if (tb[JV_CN_REANCHORS])
		j->cn_reanchors = blobmsg_get_u64(tb[JV_CN_REANCHORS]);
	if (tb[JV_UNDERRUNS])
		j->underruns = blobmsg_get_u64(tb[JV_UNDERRUNS]);
	if (tb[JV_SKEW_DROPS])
		j->skew_drops = blobmsg_get_u64(tb[JV_SKEW_DROPS]);
	if (tb[JV_SKEW_INSERTS])
		j->skew_inserts = blobmsg_get_u64(tb[JV_SKEW_INSERTS]);
	if (tb[JV_TARGET_MS])
		j->target_ms = blobmsg_get_u32(tb[JV_TARGET_MS]);
	if (tb[JV_DEPTH_MS])
		j->depth_ms = blobmsg_get_u32(tb[JV_DEPTH_MS]);
	if (tb[JV_JITTER_MS])
		j->jitter_ms = blobmsg_get_u32(tb[JV_JITTER_MS]);
	if (tb[JV_POOL_FREE])
		j->pool_free = blobmsg_get_u32(tb[JV_POOL_FREE]);
}

static void entry_take(struct blob_attr *entry, struct cdr_view *v)
{
	struct blob_attr *tb[__CV_MAX] = {};

	memset(v, 0, sizeof(*v));
	blobmsg_parse(cv_policy, __CV_MAX, tb, blobmsg_data(entry),
		      blobmsg_data_len(entry));

	if (tb[CV_SEQ])
		v->seq = blobmsg_get_u32(tb[CV_SEQ]);
	if (tb[CV_LINE])
		v->line = blobmsg_get_u32(tb[CV_LINE]);
	if (tb[CV_SLOT])
		v->slot = blobmsg_get_u32(tb[CV_SLOT]);
	if (tb[CV_INBOUND])
		v->inbound = blobmsg_get_bool(tb[CV_INBOUND]);
	if (tb[CV_ANSWERED])
		v->answered = blobmsg_get_bool(tb[CV_ANSWERED]);
	if (tb[CV_START])
		v->start = blobmsg_get_u64(tb[CV_START]);
	if (tb[CV_DURATION])
		v->duration_ms = blobmsg_get_u32(tb[CV_DURATION]);
	if (tb[CV_ANSWER_OFFSET])
		v->answer_offset_ms = blobmsg_get_u32(tb[CV_ANSWER_OFFSET]);
	if (tb[CV_PTIME])
		v->ptime = blobmsg_get_u32(tb[CV_PTIME]);
	if (tb[CV_RTP_PORT])
		v->rtp_port = blobmsg_get_u32(tb[CV_RTP_PORT]);
	if (tb[CV_IN_PROGRESS])
		v->in_progress = blobmsg_get_bool(tb[CV_IN_PROGRESS]);
	if (tb[CV_TRANSFERRED])
		v->transferred = blobmsg_get_bool(tb[CV_TRANSFERRED]);
	if (tb[CV_TRANSFERRED_FROM])
		v->transferred_from = blobmsg_get_u32(tb[CV_TRANSFERRED_FROM]);
	if (tb[CV_CONFERENCED])
		v->conferenced = blobmsg_get_bool(tb[CV_CONFERENCED]);

	str_take(v->fail, sizeof(v->fail), tb[CV_FAIL]);
	str_take(v->number, sizeof(v->number), tb[CV_NUMBER]);
	str_take(v->remote, sizeof(v->remote), tb[CV_REMOTE]);
	str_take(v->name, sizeof(v->name), tb[CV_NAME]);
	str_take(v->codec, sizeof(v->codec), tb[CV_CODEC]);
	str_take(v->media_state, sizeof(v->media_state), tb[CV_MEDIA_STATE]);

	/* every optional key is recorded as present or absent, because the
	 * omission itself is the contract for the identity strings */
	v->has_fail = tb[CV_FAIL] != NULL;
	v->has_number = tb[CV_NUMBER] != NULL;
	v->has_remote = tb[CV_REMOTE] != NULL;
	v->has_name = tb[CV_NAME] != NULL;
	v->has_codec = tb[CV_CODEC] != NULL;
	v->has_ptime = tb[CV_PTIME] != NULL;
	v->has_rtp_port = tb[CV_RTP_PORT] != NULL;
	v->has_answer_offset = tb[CV_ANSWER_OFFSET] != NULL;
	v->has_media = tb[CV_MEDIA] != NULL;
	v->has_transferred = tb[CV_TRANSFERRED] != NULL;
	v->has_transferred_from = tb[CV_TRANSFERRED_FROM] != NULL;
	v->has_conferenced = tb[CV_CONFERENCED] != NULL;

	v->has_jb = tb[CV_JB] != NULL;

	if (tb[CV_MEDIA])
		media_take(tb[CV_MEDIA], &v->media);
	if (tb[CV_JB])
		jb_take(tb[CV_JB], &v->jb);
}

static struct cdr_view views[VIEW_MAX];

/* the whole ring, oldest first, and how many records it holds */
static unsigned int ring_take(void)
{
	struct blob_attr *cur, *entry;
	unsigned int rem, rem2, n = 0;

	memset(views, 0, sizeof(views));
	blob_buf_init(&b, 0);
	cdr_dump(&b);

	blob_for_each_attr(cur, b.head, rem) {
		if (strcmp(blobmsg_name(cur), "cdr"))
			continue;

		blobmsg_for_each_attr(entry, cur, rem2) {
			if (n < VIEW_MAX)
				entry_take(entry, &views[n]);
			n++;
		}
	}

	return n;
}

/* the newest record, which is what most cases just wrote */
static struct cdr_view *ring_last(void)
{
	static struct cdr_view none;
	unsigned int n = ring_take();

	if (!n || n > VIEW_MAX)
		return &none;

	return &views[n - 1];
}

static bool stats_take(unsigned int line, struct stats_view *v)
{
	struct blob_attr *cur, *entry;
	unsigned int rem, rem2;

	memset(v, 0, sizeof(*v));
	blob_buf_init(&b, 0);
	cdr_stats_dump(&b, -1);

	blob_for_each_attr(cur, b.head, rem) {
		if (strcmp(blobmsg_name(cur), "stats"))
			continue;

		blobmsg_for_each_attr(entry, cur, rem2) {
			struct blob_attr *tb[__SV_MAX] = {};
			struct blob_attr *tt[__TV_MAX] = {};

			blobmsg_parse(sv_policy, __SV_MAX, tb, blobmsg_data(entry),
				      blobmsg_data_len(entry));
			if (!tb[SV_LINE] || blobmsg_get_u32(tb[SV_LINE]) != line)
				continue;

			v->found = true;
			if (tb[SV_IN_RECEIVED])
				v->in_received = blobmsg_get_u32(tb[SV_IN_RECEIVED]);
			if (tb[SV_IN_ANSWERED])
				v->in_answered = blobmsg_get_u32(tb[SV_IN_ANSWERED]);
			if (tb[SV_IN_FAILED])
				v->in_failed = blobmsg_get_u32(tb[SV_IN_FAILED]);
			if (tb[SV_OUT_ATTEMPTED])
				v->out_attempted = blobmsg_get_u32(tb[SV_OUT_ATTEMPTED]);
			if (tb[SV_OUT_ANSWERED])
				v->out_answered = blobmsg_get_u32(tb[SV_OUT_ANSWERED]);
			if (tb[SV_OUT_FAILED])
				v->out_failed = blobmsg_get_u32(tb[SV_OUT_FAILED]);
			if (!tb[SV_TOTALS])
				continue;

			blobmsg_parse(tv_policy, __TV_MAX, tt,
				      blobmsg_data(tb[SV_TOTALS]),
				      blobmsg_data_len(tb[SV_TOTALS]));
			if (tt[TV_PACKETS_SENT])
				v->packets_sent = blobmsg_get_u64(tt[TV_PACKETS_SENT]);
			if (tt[TV_PACKETS_RECV])
				v->packets_recv = blobmsg_get_u64(tt[TV_PACKETS_RECV]);
			if (tt[TV_PACKETS_LOST])
				v->packets_lost = blobmsg_get_u64(tt[TV_PACKETS_LOST]);
			if (tt[TV_BYTES_SENT])
				v->bytes_sent = blobmsg_get_u64(tt[TV_BYTES_SENT]);
			if (tt[TV_BYTES_RECV])
				v->bytes_recv = blobmsg_get_u64(tt[TV_BYTES_RECV]);
		}
	}

	return v->found;
}

/* ------------------------------------------------------------- fixtures */

/* uloop compares a timeout in whole milliseconds, so it can fire up to 1 ms
 * early, and the record rounds each stamp down. The window therefore runs
 * 2 ms past its nominal length, so the measured time is never below it. */
#define WINDOW_SLACK_MS	2

static void window_run(unsigned int ms)
{
	uloop_pump(ms + WINDOW_SLACK_MS);
}

static struct proto_call_ended ended_of(bool inbound, bool answered,
					bool failed, enum proto_fail_class cls)
{
	struct proto_call_ended info;

	memset(&info, 0, sizeof(info));
	info.inbound = inbound;
	info.answered = answered;
	info.failed = failed;
	info.fail_class = cls;
	info.number = "";
	info.remote_number = "";
	info.remote_name = "";

	return info;
}

/* a full outbound call: dialled number, committed codec, media that ran */
static struct proto_call_ended ended_outbound(void)
{
	struct proto_call_ended info = ended_of(false, true, false,
						PROTO_FAIL_BUSY);

	info.media_ran = true;
	info.number = "15551234";
	info.codec = HAL_CODEC_PCMU;
	info.ptime_ms = 20;
	info.rtp_port = 10002;

	return info;
}

static struct hal_media_stats media_of(uint64_t base)
{
	struct hal_media_stats st;

	memset(&st, 0, sizeof(st));
	st.packets_sent = base;
	st.packets_recv = base + 1;
	st.packets_lost = base + 2;
	st.packets_discarded = base + 3;
	st.loss_rate = base + 4;
	st.loss_rate_far = base + 5;
	st.jitter_avg = base + 6;
	st.jitter_max = base + 7;
	st.jitter_far = base + 8;
	st.rtt_avg = base + 9;
	st.rtt_max = base + 10;
	st.bytes_sent = base + 11;
	st.bytes_recv = base + 12;
	st.mos_lq = 4.3f;
	st.mos_cq = 4.1f;
	st.rfactor = 88;

	return st;
}

/* ------------------------------------------------------------- lifecycle */

static void t_lifecycle(void)
{
	struct hal_media_stats st = media_of(1000);
	struct proto_call_ended info;
	struct cdr_view *v;
	uint32_t handle;

	cdr_init();

	CASE("an empty ring dumps an empty array");
	CHECK_UINT(ring_take(), 0);

	CASE("a started call takes its record at once");
	handle = cdr_call_started(0, 0, false);
	CHECK(handle != CDR_HANDLE_NONE);
	CHECK_UINT(ring_take(), 1);

	CASE("the record of a running call says it is in progress");
	v = ring_last();
	CHECK(v->in_progress);
	CHECK_UINT(v->seq, 1);
	CHECK_UINT(v->line, 0);
	CHECK_UINT(v->slot, 0);
	CHECK(!v->inbound);
	CHECK(!v->answered);

	CASE("a running call reports no media yet");
	CHECK(!strcmp(v->media_state, "absent"));
	CHECK(!v->has_media);

	CASE("the start carries a plausible wall clock");
	CHECK(v->start > EPOCH_FLOOR);

	CASE("the answer is recorded with an offset");
	window_run(5);
	cdr_call_answered(0, 0);
	v = ring_last();
	CHECK(v->answered);
	CHECK(v->has_answer_offset);
	CHECK(v->answer_offset_ms >= 5);

	CASE("the media snapshot marks the record pending");
	CHECK_UINT(cdr_media_pending(0, 0), handle);
	v = ring_last();
	CHECK(!strcmp(v->media_state, "pending"));
	CHECK(!v->has_media);

	CASE("the end closes the record and fills the identity");
	window_run(5);
	info = ended_outbound();
	cdr_call_ended(0, 0, &info);
	v = ring_last();
	CHECK(!v->in_progress);
	CHECK(!strcmp(v->number, "15551234"));
	CHECK(!v->has_remote);
	CHECK(!v->has_name);
	CHECK(!v->has_fail);

	CASE("the committed codec and its ptime travel together");
	CHECK(!strcmp(v->codec, "PCMU"));
	CHECK(v->has_ptime);
	CHECK_UINT(v->ptime, 20);
	CHECK_UINT(v->rtp_port, 10002);

	CASE("the duration covers the whole leg and holds the answer offset");
	CHECK(v->duration_ms >= 10);
	CHECK(v->answer_offset_ms <= v->duration_ms);

	CASE("a completion after the leg is gone still reaches its record");
	cdr_media_set(handle, &st);
	v = ring_last();
	CHECK(!strcmp(v->media_state, "present"));
	CHECK(v->has_media);

	CASE("the media table carries every member the HAL reported");
	CHECK_UINT(v->media.packets_sent, 1000);
	CHECK_UINT(v->media.packets_recv, 1001);
	CHECK_UINT(v->media.packets_lost, 1002);
	CHECK_UINT(v->media.packets_discarded, 1003);
	CHECK_UINT(v->media.loss_rate, 1004);
	CHECK_UINT(v->media.loss_rate_far, 1005);
	CHECK_UINT(v->media.jitter_avg, 1006);
	CHECK_UINT(v->media.jitter_max, 1007);
	CHECK_UINT(v->media.jitter_far, 1008);
	CHECK_UINT(v->media.rtt_avg, 1009);
	CHECK_UINT(v->media.rtt_max, 1010);
	CHECK_UINT(v->media.bytes_sent, 1011);
	CHECK_UINT(v->media.bytes_recv, 1012);

	CASE("the two MOS values travel as strings with exactly two decimals");
	CHECK(!strcmp(v->media.mos_lq, "4.30"));
	CHECK(!strcmp(v->media.mos_cq, "4.10"));
	CHECK_UINT(v->media.rfactor, 88);

	CASE("an absent MOS measurement remains present as an exact zero string");
	cdr_init();
	handle = cdr_call_started(0, 0, false);
	st = media_of(10);
	st.mos_lq = 0.0f;
	st.mos_cq = 0.0f;
	cdr_media_set(handle, &st);
	v = ring_last();
	CHECK(v->has_media);
	CHECK(!strcmp(v->media.mos_lq, "0.00"));
	CHECK(!strcmp(v->media.mos_cq, "0.00"));

	CASE("NaN and positive infinite MOS values render as exact zero strings");
	st.mos_lq = NAN;
	st.mos_cq = INFINITY;
	cdr_media_set(handle, &st);
	v = ring_last();
	CHECK(!strcmp(v->media.mos_lq, "0.00"));
	CHECK(!strcmp(v->media.mos_cq, "0.00"));

	CASE("negative infinite and finite MOS values keep their ruled strings");
	st.mos_lq = -INFINITY;
	st.mos_cq = -1.0f;
	cdr_media_set(handle, &st);
	v = ring_last();
	CHECK(!strcmp(v->media.mos_lq, "0.00"));
	CHECK(!strcmp(v->media.mos_cq, "-1.00"));

	/* 4.005f lies above the decimal tie on this libc, so %.2f produces 4.01. */
	CASE("a MOS boundary follows the libc two-decimal rounding result");
	cdr_init();
	handle = cdr_call_started(0, 0, false);
	st = media_of(10);
	st.mos_lq = 4.005f;
	cdr_media_set(handle, &st);
	v = ring_last();
	CHECK(!strcmp(v->media.mos_lq, "4.01"));

	CASE("an inbound call keeps the caller identity and omits the dialled one");
	cdr_init();
	cdr_call_started(1, 1, true);
	info = ended_of(true, false, true, PROTO_FAIL_BUSY);
	info.remote_number = "15559999";
	info.remote_name = "Alice";
	cdr_call_ended(1, 1, &info);
	v = ring_last();
	CHECK(v->inbound);
	CHECK(!v->has_number);
	CHECK(!strcmp(v->remote, "15559999"));
	CHECK(!strcmp(v->name, "Alice"));
	CHECK_UINT(v->line, 1);
	CHECK_UINT(v->slot, 1);

	CASE("an unanswered failure names its clearing class and no answer offset");
	CHECK(!v->answered);
	CHECK(v->has_fail);
	CHECK(!strcmp(v->fail, "busy"));
	CHECK(!v->has_answer_offset);

	CASE("a caller with no name leaves the key out instead of sending an empty one");
	cdr_init();
	cdr_call_started(0, 0, true);
	info = ended_of(true, true, false, PROTO_FAIL_BUSY);
	info.remote_number = "15558888";
	info.remote_name = "";
	cdr_call_ended(0, 0, &info);
	v = ring_last();
	CHECK(v->has_remote);
	CHECK(!v->has_name);

	CASE("a call with no committed codec reports neither codec nor ptime");
	CHECK(!v->has_codec);
	CHECK(!v->has_ptime);
	CHECK(!v->has_rtp_port);

	/*
	 * A T.38 call negotiated no audio codec, so its committed codec id is
	 * NONE and the branch above would leave the record naming no media at
	 * all. The leg reports the image session as a flag beside the codec,
	 * and the record renders the one word the status dump uses, with the 0
	 * ptime an image exchange pins.
	 */
	CASE("a record that carried T.38 names the image codec and its ptime");
	cdr_init();
	cdr_call_started(0, 0, false);
	info = ended_of(false, true, false, PROTO_FAIL_BUSY);
	info.t38 = true;
	info.codec = HAL_CODEC_NONE;	/* an image exchange */
	info.ptime_ms = 0;
	cdr_call_ended(0, 0, &info);
	v = ring_last();
	CHECK(v->has_codec);
	CHECK(!strcmp(v->codec, "t38"));
	CHECK(v->has_ptime);
	CHECK_UINT(v->ptime, 0);

	CASE("while an audio record of the same shape keeps its own name");
	cdr_init();
	cdr_call_started(0, 0, false);
	info = ended_outbound();
	cdr_call_ended(0, 0, &info);
	v = ring_last();
	CHECK(!strcmp(v->codec, "PCMU"));
	CHECK_UINT(v->ptime, 20);

	CASE("a leg the far end never gave media to stays absent");
	CHECK(!strcmp(v->media_state, "absent"));

	CASE("a snapshot that failed is recorded as failed, not as absent");
	cdr_init();
	handle = cdr_call_started(0, 0, false);
	CHECK_UINT(cdr_media_pending(0, 0), handle);
	cdr_media_fail(handle);
	info = ended_of(false, true, false, PROTO_FAIL_BUSY);
	cdr_call_ended(0, 0, &info);
	v = ring_last();
	CHECK(!strcmp(v->media_state, "failed"));
	CHECK(!v->has_media);

	CASE("the end reports the direction the leg carried");
	cdr_init();
	cdr_call_started(0, 0, false);
	info = ended_of(true, false, false, PROTO_FAIL_BUSY);
	cdr_call_ended(0, 0, &info);
	v = ring_last();
	CHECK(v->inbound);

	CASE("a leg that reached active is answered even with no answered edge");
	cdr_init();
	cdr_call_started(0, 1, false);
	info = ended_of(false, true, false, PROTO_FAIL_BUSY);
	cdr_call_ended(0, 1, &info);
	v = ring_last();
	CHECK(v->answered);
	CHECK(v->has_answer_offset);
}

/* --------------------------------------------------------------- counters */

static void t_counters(void)
{
	struct proto_call_ended info;
	struct stats_view s;

	cdr_init();

	CASE("a fresh daemon reports every counter at zero");
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.in_received, 0);
	CHECK_UINT(s.out_attempted, 0);
	CHECK_UINT(s.packets_sent, 0);

	CASE("an inbound leg counts one received call");
	cdr_call_started(0, 0, true);
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.in_received, 1);
	CHECK_UINT(s.out_attempted, 0);

	CASE("answering it counts one answered inbound call");
	cdr_call_answered(0, 0);
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.in_answered, 1);
	CHECK_UINT(s.out_answered, 0);

	CASE("a second answered edge on one record counts once");
	cdr_call_answered(0, 0);
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.in_answered, 1);

	CASE("a normal end counts no failure");
	info = ended_of(true, true, false, PROTO_FAIL_BUSY);
	cdr_call_ended(0, 0, &info);
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.in_failed, 0);

	CASE("an outbound leg counts one attempt");
	cdr_call_started(0, 0, false);
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.out_attempted, 1);
	CHECK_UINT(s.in_received, 1);

	CASE("the far end answering counts one answered outbound call");
	cdr_call_answered(0, 0);
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.out_answered, 1);
	CHECK_UINT(s.in_answered, 1);

	CASE("an outbound failure counts against the outbound side");
	info = ended_of(false, true, true, PROTO_FAIL_CONGESTION);
	cdr_call_ended(0, 0, &info);
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.out_failed, 1);
	CHECK_UINT(s.in_failed, 0);

	CASE("an inbound failure counts against the inbound side");
	cdr_call_started(0, 0, true);
	info = ended_of(true, false, true, PROTO_FAIL_BUSY);
	cdr_call_ended(0, 0, &info);
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.in_failed, 1);
	CHECK_UINT(s.out_failed, 1);

	CASE("the counters are per line");
	CHECK(stats_take(1, &s));
	CHECK_UINT(s.in_received, 0);
	CHECK_UINT(s.out_attempted, 0);
	CHECK_UINT(s.in_failed, 0);

	CASE("a line the board does not carry counts nothing and returns nothing");
	CHECK_UINT(cdr_call_started(CFG_CHANNEL_MAX, 0, true), CDR_HANDLE_NONE);
	CHECK_UINT(cdr_call_started(0, HAL_STREAM_MAX, true), CDR_HANDLE_NONE);
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.in_received, 2);

	/*
	 * The counter must be keyed by the line, not by the slot, and no case
	 * above tells the two apart: every edge so far drove (line 0, slot 0),
	 * where a counter keyed by the slot answers exactly the numbers a
	 * counter keyed by the line answers. The two cases below move one
	 * index at a time.
	 *
	 * The gap matters because the two slots a SIP line uses are also the
	 * two line indexes of this product: a slot-keyed counter stays inside
	 * the counter array, so the defect is memory safe by coincidence and no
	 * sanitizer would ever report it. Only an oracle that separates the two
	 * indexes can.
	 */
	CASE("a call on the second line counts against the second line");
	CHECK(cdr_call_started(1, 0, false) != CDR_HANDLE_NONE);
	CHECK(stats_take(1, &s));
	CHECK_UINT(s.out_attempted, 1);
	CHECK_UINT(s.in_received, 0);
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.out_attempted, 1);
	CHECK_UINT(s.in_received, 2);

	CASE("the second slot of the first line still counts against the first line");
	CHECK(cdr_call_started(0, 1, true) != CDR_HANDLE_NONE);
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.in_received, 3);
	CHECK_UINT(s.out_attempted, 1);
	CHECK(stats_take(1, &s));
	CHECK_UINT(s.in_received, 0);
	CHECK_UINT(s.out_attempted, 1);
}

/* ------------------------------------------------------------- the totals */

static void t_totals(void)
{
	struct hal_media_stats st = media_of(100);
	struct proto_call_ended info;
	struct stats_view s;
	uint32_t handle;

	cdr_init();

	CASE("a call that ends with no snapshot moves no total");
	cdr_call_started(0, 0, false);
	info = ended_outbound();
	cdr_call_ended(0, 0, &info);
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.packets_sent, 0);
	CHECK_UINT(s.packets_recv, 0);
	CHECK_UINT(s.bytes_sent, 0);

	CASE("a failed snapshot moves no total either");
	handle = cdr_call_started(0, 0, false);
	CHECK_UINT(cdr_media_pending(0, 0), handle);
	cdr_media_fail(handle);
	cdr_call_ended(0, 0, &info);
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.packets_sent, 0);

	CASE("a real snapshot moves the five totals");
	handle = cdr_call_started(0, 0, false);
	CHECK_UINT(cdr_media_pending(0, 0), handle);
	cdr_media_set(handle, &st);
	cdr_call_ended(0, 0, &info);
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.packets_sent, 100);
	CHECK_UINT(s.packets_recv, 101);
	CHECK_UINT(s.packets_lost, 102);
	CHECK_UINT(s.bytes_sent, 111);
	CHECK_UINT(s.bytes_recv, 112);

	CASE("a second snapshot adds to them");
	handle = cdr_call_started(0, 1, false);
	CHECK_UINT(cdr_media_pending(0, 1), handle);
	cdr_media_set(handle, &st);
	cdr_call_ended(0, 1, &info);
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.packets_sent, 200);
	CHECK_UINT(s.bytes_recv, 224);

	/* the numbers arrive from a HAL job the leg no longer waits for, so a
	 * total that moved with the leg instead of with the snapshot would lose
	 * every call whose answer was slower than its own teardown */
	CASE("a snapshot that lands after the call ended still moves them");
	handle = cdr_call_started(0, 0, false);
	CHECK_UINT(cdr_media_pending(0, 0), handle);
	cdr_call_ended(0, 0, &info);
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.packets_sent, 200);
	cdr_media_set(handle, &st);
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.packets_sent, 300);
	CHECK_UINT(s.bytes_recv, 336);

	CASE("the totals of another line are untouched");
	CHECK(stats_take(1, &s));
	CHECK_UINT(s.packets_sent, 0);
	CHECK_UINT(s.bytes_recv, 0);

	CASE("a snapshot with no numbers at all is dropped");
	handle = cdr_call_started(1, 0, false);
	CHECK_UINT(cdr_media_pending(1, 0), handle);
	cdr_media_set(handle, NULL);
	CHECK(stats_take(1, &s));
	CHECK_UINT(s.packets_sent, 0);
	CHECK(!strcmp(ring_last()->media_state, "pending"));
}

/* ---------------------------------------------------------------- the ring */

static void t_ring(void)
{
	struct hal_media_stats st = media_of(500);
	struct proto_call_ended info = ended_of(false, true, false,
						PROTO_FAIL_BUSY);
	uint32_t first, handle;
	struct stats_view s;
	unsigned int i, n;

	cdr_init();

	CASE("the ring fills to its depth");
	for (i = 0; i < CDR_RING; i++) {
		cdr_call_started(0, 0, false);
		cdr_call_ended(0, 0, &info);
	}
	CHECK_UINT(ring_take(), CDR_RING);

	CASE("it holds the records oldest first");
	n = ring_take();
	CHECK_UINT(n, CDR_RING);
	CHECK_UINT(views[0].seq, 1);
	CHECK_UINT(views[CDR_RING - 1].seq, CDR_RING);

	CASE("the next call evicts the oldest record");
	cdr_call_started(0, 0, false);
	cdr_call_ended(0, 0, &info);
	n = ring_take();
	CHECK_UINT(n, CDR_RING);
	CHECK_UINT(views[0].seq, 2);
	CHECK_UINT(views[CDR_RING - 1].seq, CDR_RING + 1);

	CASE("the record sequence keeps growing across the wrap");
	for (i = 0; i < CDR_RING; i++) {
		cdr_call_started(0, 0, false);
		cdr_call_ended(0, 0, &info);
	}
	n = ring_take();
	CHECK_UINT(n, CDR_RING);
	CHECK_UINT(views[CDR_RING - 1].seq, 2 * CDR_RING + 1);

	CASE("a snapshot handle survives its own call");
	cdr_init();
	first = cdr_call_started(0, 0, false);
	CHECK_UINT(cdr_media_pending(0, 0), first);
	cdr_call_ended(0, 0, &info);
	cdr_media_set(first, &st);
	CHECK(!strcmp(ring_last()->media_state, "present"));

	CASE("a handle whose record was evicted no longer matches");
	cdr_init();
	first = cdr_call_started(0, 0, false);
	CHECK_UINT(cdr_media_pending(0, 0), first);
	cdr_call_ended(0, 0, &info);
	for (i = 0; i < CDR_RING; i++) {
		handle = cdr_call_started(0, 1, false);
		CHECK(handle != first);
		cdr_call_ended(0, 1, &info);
	}
	cdr_media_set(first, &st);

	CASE("the record that took the slot is unharmed");
	n = ring_take();
	CHECK_UINT(n, CDR_RING);
	CHECK_UINT(views[0].seq, 2);
	CHECK(!strcmp(views[0].media_state, "absent"));
	for (i = 0; i < n; i++)
		CHECK(!views[i].has_media);

	CASE("the dropped result moved no total either");
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.packets_sent, 0);

	CASE("a failure for an evicted handle is dropped the same way");
	cdr_media_fail(first);
	n = ring_take();
	for (i = 0; i < n; i++)
		CHECK(strcmp(views[i].media_state, "failed") != 0);

	CASE("the zero handle names no record");
	cdr_media_set(CDR_HANDLE_NONE, &st);
	cdr_media_fail(CDR_HANDLE_NONE);
	n = ring_take();
	for (i = 0; i < n; i++)
		CHECK(!views[i].has_media);
}

/* ------------------------------------------------------------ the edges */

static void t_edges(void)
{
	struct proto_call_ended info = ended_of(false, false, false,
						PROTO_FAIL_BUSY);
	struct stats_view s;
	struct cdr_view *v;
	uint32_t handle;
	unsigned int n;

	cdr_init();

	CASE("an end with no open record changes nothing");
	cdr_call_ended(0, 0, &info);
	CHECK_UINT(ring_take(), 0);
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.out_failed, 0);

	CASE("an end with no report at all changes nothing");
	cdr_call_started(0, 0, false);
	cdr_call_ended(0, 0, NULL);
	v = ring_last();
	CHECK(v->in_progress);

	CASE("a second start on one slot finalises the record that never ended");
	handle = cdr_call_started(0, 0, false);
	CHECK(handle != CDR_HANDLE_NONE);
	n = ring_take();
	CHECK_UINT(n, 2);
	CHECK(!views[0].in_progress);
	CHECK(views[1].in_progress);

	CASE("the finalised record keeps the numbers it had");
	CHECK_UINT(views[0].seq, 1);
	CHECK(!views[0].answered);
	CHECK(!views[0].has_fail);

	CASE("an answer for a slot with no open record is ignored");
	cdr_init();
	cdr_call_answered(0, 0);
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.in_answered, 0);
	CHECK_UINT(s.out_answered, 0);

	CASE("a pending mark for a slot with no open record names no record");
	CHECK_UINT(cdr_media_pending(0, 0), CDR_HANDLE_NONE);
	CHECK_UINT(cdr_media_pending(CFG_CHANNEL_MAX, 0), CDR_HANDLE_NONE);
	CHECK_UINT(ring_take(), 0);

	CASE("the two open records of one line are independent");
	cdr_init();
	cdr_call_started(0, 0, true);
	cdr_call_started(0, 1, false);
	cdr_call_answered(0, 1);
	n = ring_take();
	CHECK_UINT(n, 2);
	CHECK(views[0].inbound);
	CHECK(!views[0].answered);
	CHECK(!views[1].inbound);
	CHECK(views[1].answered);
}

/* --------------------------------------------- the jitter buffer counters */

/*
 * The keys are pinned one by one, and no two carry the same value. The record
 * is nineteen counters wide behind the mode word and the renderer writes them
 * by hand, so a pair that swapped would pass every "is it there" check and
 * report the wrong quantity for the life of the surface.
 */
static void jb_fill(struct hal_jb_stats *jb)
{
	memset(jb, 0, sizeof(*jb));
	jb->mode = HAL_JB_FIXED;
	jb->frames_out = 1001;
	jb->plc = 1002;
	jb->cn = 1003;
	jb->silence = 1004;
	jb->late_drop = 1005;
	jb->dup_drop = 1006;
	jb->overflow_drop = 1007;
	jb->hold_drop = 1014;
	jb->resync_discard_ms = 1015;
	jb->resyncs = 1008;
	jb->starve_resyncs = 1009;
	jb->cn_reanchors = 1010;
	jb->underruns = 1011;
	jb->skew_drops = 1012;
	jb->skew_inserts = 1013;
	jb->target_ms = 61;
	jb->depth_ms = 62;
	jb->jitter_ms = 63;
	jb->pool_free = 64;
}

/*
 * The key set of the renderer, in order. The blobmsg parse every other case
 * uses tolerates a key it was not told about, so a key added or dropped by
 * hand would pass them all; this walks the table instead. The order is the
 * schema's JbCounters order, mode first.
 */
static const char *const jb_keys[] = {
	"mode", "frames_out", "plc", "cn", "silence", "late_drop", "dup_drop",
	"overflow_drop", "hold_drop", "resync_discard_ms", "resyncs",
	"starve_resyncs", "cn_reanchors", "underruns", "skew_drops",
	"skew_inserts", "target_ms", "depth_ms", "jitter_ms", "pool_free",
};

static void jb_keys_check(const struct hal_jb_stats *jb)
{
	struct blob_attr *cur;
	unsigned int at = 0;
	size_t rem;

	blob_buf_init(&b, 0);
	cdr_jb_dump(&b, jb);
	blobmsg_for_each_attr(cur, b.head, rem) {
		CHECK(at < ARRAY_SIZE(jb_keys));
		if (at >= ARRAY_SIZE(jb_keys))
			break;
		CHECK(!strcmp(blobmsg_name(cur), jb_keys[at]));
		at++;
	}
	CHECK_UINT(at, ARRAY_SIZE(jb_keys));
}

static void t_jb(void)
{
	struct proto_call_ended info = ended_of(true, true, false,
						PROTO_FAIL_BUSY);
	struct hal_jb_stats jb;
	struct cdr_view *v;

	jb_fill(&jb);
	cdr_init();

	CASE("the renderer writes twenty keys, the mode word and no other");
	jb_keys_check(&jb);

	CASE("a record nobody read a buffer for carries no jb table");
	cdr_call_started(0, 0, true);
	cdr_call_ended(0, 0, &info);
	v = ring_last();
	CHECK(!v->has_jb);

	CASE("cdr_jb_set fills the open record and every key survives the dump");
	cdr_init();
	cdr_call_started(0, 0, true);
	cdr_jb_set(0, 0, &jb);
	cdr_call_ended(0, 0, &info);
	v = ring_last();
	CHECK(v->has_jb);
	CHECK(!strcmp(v->jb.mode, "fixed"));
	CHECK_UINT(v->jb.frames_out, 1001);
	CHECK_UINT(v->jb.plc, 1002);
	CHECK_UINT(v->jb.cn, 1003);
	CHECK_UINT(v->jb.silence, 1004);
	CHECK_UINT(v->jb.late_drop, 1005);
	CHECK_UINT(v->jb.dup_drop, 1006);
	CHECK_UINT(v->jb.overflow_drop, 1007);
	CHECK_UINT(v->jb.hold_drop, 1014);
	CHECK_UINT(v->jb.resync_discard_ms, 1015);
	CHECK_UINT(v->jb.resyncs, 1008);
	CHECK_UINT(v->jb.starve_resyncs, 1009);
	CHECK_UINT(v->jb.cn_reanchors, 1010);
	CHECK_UINT(v->jb.underruns, 1011);
	CHECK_UINT(v->jb.skew_drops, 1012);
	CHECK_UINT(v->jb.skew_inserts, 1013);
	CHECK_UINT(v->jb.target_ms, 61);
	CHECK_UINT(v->jb.depth_ms, 62);
	CHECK_UINT(v->jb.jitter_ms, 63);
	CHECK_UINT(v->jb.pool_free, 64);

	/* The mode is the buffer's own word and not a constant of the record,
	 * so a second mode has to reach the table as itself. */
	CASE("a buffer that adapts its depth reports the adaptive mode");
	jb.mode = HAL_JB_ADAPTIVE;
	cdr_init();
	cdr_call_started(0, 0, true);
	cdr_jb_set(0, 0, &jb);
	cdr_call_ended(0, 0, &info);
	v = ring_last();
	CHECK(v->has_jb);
	CHECK(!strcmp(v->jb.mode, "adaptive"));
	jb.mode = HAL_JB_FIXED;

	CASE("a read that arrives with no open record changes nothing");
	cdr_init();
	cdr_jb_set(0, 0, &jb);
	CHECK_UINT(ring_take(), 0);
	cdr_jb_set(CFG_CHANNEL_MAX, 0, &jb);
	CHECK_UINT(ring_take(), 0);

	CASE("the counters follow the record they were set on");
	cdr_init();
	cdr_call_started(0, 0, true);
	cdr_call_started(0, 1, false);
	cdr_jb_set(0, 1, &jb);
	cdr_call_ended(0, 0, &info);
	cdr_call_ended(0, 1, &info);
	CHECK_UINT(ring_take(), 2);
	CHECK(!views[0].has_jb);
	CHECK(views[1].has_jb);
	CHECK_UINT(views[1].jb.frames_out, 1001);

	CASE("a null record is refused and leaves the entry as it was");
	cdr_init();
	cdr_call_started(0, 0, true);
	cdr_jb_set(0, 0, NULL);
	cdr_call_ended(0, 0, &info);
	v = ring_last();
	CHECK(!v->has_jb);
}

/* ------------------------------------------------------------- the reset */

enum {
	RV_RESET,
	RV_RING_KEPT,
	__RV_MAX,
};

static const struct blobmsg_policy rv_policy[__RV_MAX] = {
	[RV_RESET] = { "reset", BLOBMSG_TYPE_ARRAY },
	[RV_RING_KEPT] = { "ring_kept", BLOBMSG_TYPE_BOOL },
};

static void t_reset(void)
{
	struct hal_media_stats st = media_of(300);
	struct proto_call_ended info = ended_outbound();
	struct blob_attr *tb[__RV_MAX] = {};
	struct blob_attr *cur;
	struct stats_view s;
	unsigned int rem, n = 0;
	char what[2][32] = { { 0 }, { 0 } };
	uint32_t handle;

	cdr_init();

	CASE("the reset answers what it cleared and what it kept");
	handle = cdr_call_started(0, 0, false);
	CHECK_UINT(cdr_media_pending(0, 0), handle);
	cdr_media_set(handle, &st);
	cdr_call_ended(0, 0, &info);

	blob_buf_init(&b, 0);
	cdr_stats_reset(&b);
	blobmsg_parse(rv_policy, __RV_MAX, tb, blob_data(b.head), blob_len(b.head));
	CHECK(tb[RV_RESET] != NULL);
	CHECK(tb[RV_RING_KEPT] != NULL);
	CHECK(tb[RV_RING_KEPT] && blobmsg_get_bool(tb[RV_RING_KEPT]));

	CASE("it names the counters and the totals, in that order");
	if (tb[RV_RESET]) {
		blobmsg_for_each_attr(cur, tb[RV_RESET], rem) {
			if (n < 2)
				snprintf(what[n], sizeof(what[n]), "%s",
					 blobmsg_get_string(cur));
			n++;
		}
	}
	CHECK_UINT(n, 2);
	CHECK(!strcmp(what[0], "counters"));
	CHECK(!strcmp(what[1], "totals"));

	CASE("the counters and the totals are zero again");
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.out_attempted, 0);
	CHECK_UINT(s.out_answered, 0);
	CHECK_UINT(s.packets_sent, 0);
	CHECK_UINT(s.bytes_recv, 0);

	CASE("the ring kept every record, media included");
	CHECK_UINT(ring_take(), 1);
	CHECK_UINT(views[0].seq, 1);
	CHECK(!strcmp(views[0].media_state, "present"));
	CHECK_UINT(views[0].media.packets_sent, 300);

	CASE("the record sequence keeps counting past a reset");
	cdr_call_started(0, 0, false);
	cdr_call_ended(0, 0, &info);
	CHECK_UINT(ring_take(), 2);
	CHECK_UINT(views[1].seq, 2);

	CASE("the counters count again from zero");
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.out_attempted, 1);

	CASE("a reset of a daemon that never had a call is harmless");
	cdr_init();
	blob_buf_init(&b, 0);
	cdr_stats_reset(&b);
	CHECK_UINT(ring_take(), 0);
	CHECK(stats_take(0, &s));
	CHECK_UINT(s.in_received, 0);
}

/* ------------------------------------------------------------- the clear */

/*
 * The duration of a BYE-cleared leg ends at the BYE. The leg outlives its own
 * call by the BYE transaction drain, so the report names the age of the clear
 * and this module subtracts it. The age comes from the leg clock, and the
 * elapsed time comes from this module. The two cases below therefore assert
 * relations, not a difference of absolute values.
 */
static void t_cleared(void)
{
	struct proto_call_ended info;
	struct cdr_view *v;
	uint32_t plain, cut;

	cdr_init();

	CASE("a leg no BYE cleared is measured to its release");
	cdr_call_started(0, 0, false);
	window_run(40);
	info = ended_outbound();
	cdr_call_ended(0, 0, &info);
	plain = ring_last()->duration_ms;
	CHECK(plain >= 40);

	CASE("a cleared leg stops its duration at the clear");
	cdr_call_started(0, 0, false);
	cdr_call_answered(0, 0);
	window_run(40);
	info = ended_outbound();
	info.cleared = true;
	info.cleared_ago_ms = 30;
	cdr_call_ended(0, 0, &info);
	v = ring_last();
	cut = v->duration_ms;
	CHECK(cut + 30 >= 40);
	CHECK(cut < 25);
	CHECK(cut < plain);

	CASE("the answer offset keeps the stamp the answer gave it");
	CHECK(v->has_answer_offset);
	CHECK(v->answer_offset_ms <= cut);

	CASE("an age the report carries without a clear changes nothing");
	cdr_call_started(0, 0, false);
	window_run(20);
	info = ended_outbound();
	info.cleared_ago_ms = 60000;
	cdr_call_ended(0, 0, &info);
	CHECK(ring_last()->duration_ms >= 20);

	/* the two clocks are read microseconds apart, so an age one tick past
	 * the elapsed time is a skew, not a call that ended before it began */
	CASE("an age larger than the leg itself clamps to zero");
	cdr_call_started(0, 0, false);
	window_run(20);
	info = ended_outbound();
	info.cleared = true;
	info.cleared_ago_ms = 60000;
	cdr_call_ended(0, 0, &info);
	CHECK_UINT(ring_last()->duration_ms, 0);

	CASE("the record of a clear counts and renders like any other");
	CHECK(!ring_last()->in_progress);
	CHECK(!strcmp(ring_last()->number, "15551234"));
}

/* ------------------------------------------------------------- the stop */

/*
 * The daemon stop closes the open records. calls_done() ends every leg still
 * in use. This module therefore sees an ordinary ended edge for a call that
 * was still up, with the clear that the teardown BYE stamped. test_call covers
 * the leg half of that chain. The cases below drive the edges by hand, as
 * everywhere in this suite.
 */
static void t_shutdown(void)
{
	struct proto_call_ended info;
	unsigned int i, n;
	uint32_t dur;

	cdr_init();

	CASE("the stop closes a record that was open at the report");
	cdr_call_started(0, 0, false);
	cdr_call_answered(0, 0);
	cdr_call_started(0, 1, true);
	window_run(40);

	info = ended_outbound();
	info.cleared = true;
	info.cleared_ago_ms = 30;
	cdr_call_ended(0, 0, &info);

	info = ended_of(true, false, false, PROTO_FAIL_BUSY);
	cdr_call_ended(0, 1, &info);

	n = ring_take();
	CHECK_UINT(n, 2);

	CASE("the cleared leg keeps the duration its BYE ended");
	dur = views[0].duration_ms;
	CHECK(dur + 30 >= 40);
	CHECK(dur < 25);

	CASE("no record is left in progress after the stop");
	for (i = 0; i < n; i++)
		CHECK(!views[i].in_progress);

	CASE("a call that starts after the stop finalises nothing behind it");
	cdr_call_started(0, 0, false);
	CHECK_UINT(ring_take(), 3);
	CHECK_UINT(views[0].duration_ms, dur);
	CHECK(!views[0].in_progress);
	CHECK(views[2].in_progress);
}

/*
 * The transfer correlation (call.h call_ended.transferred). The record is
 * fixed: a transferee closes two records, one per leg. The reader pairs the
 * child with the parent by the flag on the parent and by the adjacency of the
 * two records on one line. No transfer id is carried. The
 * ring stores nothing more about the transfer.
 */
static void t_transferred(void)
{
	struct proto_call_ended info;

	cdr_init();

	CASE("the two legs of a transfer open two records on two slots");
	cdr_call_started(0, 0, false);
	cdr_call_answered(0, 0);
	cdr_call_started(0, 1, false);
	CHECK_UINT(ring_take(), 2);
	CHECK_UINT(views[0].slot, 0);
	CHECK_UINT(views[1].slot, 1);
	CHECK(views[0].in_progress);
	CHECK(views[1].in_progress);

	CASE("the replaced leg closes with the transfer flag set");
	info = ended_outbound();
	info.transferred = true;
	cdr_call_ended(0, 0, &info);
	CHECK_UINT(ring_take(), 2);
	CHECK(views[0].transferred);
	CHECK(!views[0].in_progress);

	CASE("the child's record is untouched by it");
	CHECK(views[1].in_progress);
	CHECK(!views[1].has_transferred);

	CASE("the two are adjacent on the same line, which IS the correlation");
	CHECK_UINT(views[0].line, views[1].line);
	CHECK_UINT(views[1].seq, views[0].seq + 1);

	CASE("the child closes its own record, and it carries no flag");
	info = ended_outbound();
	cdr_call_ended(0, 1, &info);
	CHECK_UINT(ring_take(), 2);
	CHECK(!views[1].in_progress);
	CHECK(!views[1].transferred);
	CHECK(!views[1].has_transferred);

	/*
	 * The stable half of the pairing (cdr.h cdr_call_correlate). The leg
	 * that took the call over names the record of the leg it took it from,
	 * so a reader pairs the two without depending on them being neighbours
	 * in the ring.
	 */
	CASE("the child's record names the record it took the call over from");
	cdr_init();
	cdr_call_started(0, 0, false);
	cdr_call_started(0, 1, false);
	cdr_call_correlate(0, 1, 0);
	CHECK_UINT(ring_take(), 2);
	CHECK(views[1].has_transferred_from);
	CHECK_UINT(views[1].transferred_from, views[0].seq);

	CASE("and the record it names carries the other half of the pairing");
	info = ended_outbound();
	info.transferred = true;
	cdr_call_ended(0, 0, &info);
	CHECK_UINT(ring_take(), 2);
	CHECK(views[0].transferred);
	CHECK(!views[0].has_transferred_from);

	CASE("the pairing survives a record opening between the two");
	cdr_init();
	cdr_call_started(0, 0, false);
	cdr_call_started(1, 0, true);
	cdr_call_started(0, 1, false);
	cdr_call_correlate(0, 1, 0);
	CHECK_UINT(ring_take(), 3);
	CHECK_UINT(views[2].transferred_from, views[0].seq);
	CHECK(views[1].seq > views[0].seq);
	CHECK(views[2].seq > views[1].seq);

	CASE("a correlation with no record on either side writes nothing");
	cdr_init();
	cdr_call_started(0, 0, false);
	cdr_call_correlate(0, 0, 1);
	cdr_call_correlate(0, 1, 0);
	cdr_call_correlate(0, 0, 0);
	cdr_call_correlate(CFG_CHANNEL_MAX, 0, 1);
	cdr_call_correlate(0, HAL_STREAM_MAX, 0);
	cdr_call_correlate(0, 0, HAL_STREAM_MAX);
	CHECK_UINT(ring_take(), 1);
	CHECK(!views[0].has_transferred_from);

	/*
	 * A leg that announced its answer to nobody carries the moment as an
	 * age in its ending report, or its record would date the answer at the
	 * end and show a call with no talk time at all (call.h
	 * answered_ago_ms).
	 */
	CASE("an ended leg's answer age becomes the answer offset");
	cdr_init();
	cdr_call_started(0, 0, false);
	window_run(40);
	info = ended_outbound();
	info.answered = true;
	info.answered_ago_ms = 30;
	cdr_call_ended(0, 0, &info);
	CHECK_UINT(ring_take(), 1);
	CHECK(views[0].answered);
	CHECK(views[0].has_answer_offset);
	CHECK(views[0].answer_offset_ms < views[0].duration_ms);
	CHECK(views[0].answer_offset_ms + 30 >= 40);

	CASE("an answer the record already timed is not re-dated by the age");
	cdr_init();
	cdr_call_started(0, 0, false);
	cdr_call_answered(0, 0);
	window_run(40);
	info = ended_outbound();
	info.answered = true;
	info.answered_ago_ms = 30;
	cdr_call_ended(0, 0, &info);
	CHECK_UINT(ring_take(), 1);
	CHECK(views[0].answer_offset_ms < 20);

	CASE("an age wider than the elapsed time cannot underflow the offset");
	cdr_init();
	cdr_call_started(0, 0, false);
	info = ended_outbound();
	info.answered = true;
	info.answered_ago_ms = 900000;
	cdr_call_ended(0, 0, &info);
	CHECK_UINT(ring_take(), 1);
	CHECK(views[0].answer_offset_ms <= views[0].duration_ms);

	CASE("an ordinary call writes the key at all only when it is set");
	cdr_init();
	cdr_call_started(0, 0, false);
	info = ended_outbound();
	cdr_call_ended(0, 0, &info);
	CHECK_UINT(ring_take(), 1);
	CHECK(!views[0].has_transferred);

	CASE("the flag is a fact of the record and not of the counters");
	cdr_init();
	cdr_call_started(0, 0, false);
	info = ended_outbound();
	info.transferred = true;
	info.failed = true;
	info.fail_class = PROTO_FAIL_QUIET;
	cdr_call_ended(0, 0, &info);
	CHECK_UINT(ring_take(), 1);
	CHECK(views[0].transferred);
	CHECK(views[0].has_fail);
}

/*
 * The conference mark (cdr.h cdr_call_conferenced). It has the same shape as
 * the transfer flag above: one bool on the record. The mark is written on the
 * open record, and the dump shows it only where it is set.
 */
static void t_conferenced(void)
{
	CASE("a leg the mixer joined carries the conference mark");
	cdr_init();
	cdr_call_started(0, 0, false);
	cdr_call_started(0, 1, false);
	cdr_call_conferenced(0, 0);
	cdr_call_conferenced(0, 1);
	CHECK_UINT(ring_take(), 2);
	CHECK(views[0].has_conferenced);
	CHECK(views[0].conferenced);
	CHECK(views[1].has_conferenced);
	CHECK(views[1].conferenced);

	CASE("a leg the mixer never joined writes no key at all");
	cdr_init();
	cdr_call_started(0, 0, false);
	cdr_call_conferenced(0, 1);
	cdr_call_conferenced(1, 0);
	cdr_call_conferenced(CFG_CHANNEL_MAX, 0);
	cdr_call_conferenced(0, HAL_STREAM_MAX);
	CHECK_UINT(ring_take(), 1);
	CHECK(!views[0].has_conferenced);
	CHECK(!views[0].conferenced);
}

static void t_call_seq(void)
{
	struct proto_call_ended info = ended_outbound();
	uint32_t handle;

	CASE("call sequence: an unopened slot has no sequence");
	cdr_init();
	CHECK_UINT(cdr_call_seq(0, 0), 0);
	CHECK_UINT(cdr_call_seq(CFG_CHANNEL_MAX, 0), 0);
	CHECK_UINT(cdr_call_seq(0, HAL_STREAM_MAX), 0);

	CASE("call sequence: an open record returns its dump sequence");
	cdr_call_started(1, 0, true);
	handle = cdr_call_started(0, 1, false);
	CHECK_UINT(ring_take(), 2);
	CHECK_UINT(cdr_call_seq(0, 1), views[1].seq);
	CHECK_UINT(cdr_call_seq(0, 1), 2);
	CHECK(cdr_call_seq(0, 1) != handle);
	CHECK_UINT(cdr_call_seq(1, 0), views[0].seq);

	CASE("call sequence: an ended record has no open sequence");
	cdr_call_ended(0, 1, &info);
	CHECK_UINT(cdr_call_seq(0, 1), 0);
	CHECK_UINT(ring_take(), 2);
	CHECK_UINT(views[1].seq, 2);
}

int main(void)
{
	uloop_init();
	t_lifecycle();
	t_call_seq();
	t_counters();
	t_totals();
	t_ring();
	t_edges();
	t_jb();
	t_reset();
	t_cleared();
	t_shutdown();
	t_transferred();
	t_conferenced();

	blob_buf_free(&b);
	uloop_done();

	return tu_summary("test_cdr");
}
