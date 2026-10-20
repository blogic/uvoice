// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Shared HAL status and per-line diagnostic rows. Live reads refresh the
 * same caches for status replies and call trace snapshots.
 */
#include <limits.h>
#include <string.h>

#include <libubox/blobmsg.h>
#include <libubox/utils.h>

#include "uvoice.h"
#include "cdr.h"
#include "line/line.h"
#include "proto/proto.h"
#include "status.h"

static const struct {
	uint32_t bit;
	const char *name;
} feature_names[] = {
	{ HAL_FEAT_G729, "g729" },
	{ HAL_FEAT_G726, "g726" },
	{ HAL_FEAT_T38, "t38" },
	{ HAL_FEAT_VBD, "vbd" },
	{ HAL_FEAT_LINE_TEST, "line_test" },
	{ HAL_FEAT_POLARITY, "polarity" },
	{ HAL_FEAT_MWI_LAMP, "mwi_lamp" },
	{ HAL_FEAT_METERING, "metering" },
	{ HAL_FEAT_KERNEL_RTP, "kernel_rtp" },
	{ HAL_FEAT_CONF, "conf" },
	{ HAL_FEAT_SRTP, "srtp" },
	{ HAL_FEAT_WIDEBAND, "wideband" },
	{ HAL_FEAT_G729B, "g729b" },
	{ HAL_FEAT_IPV6_MEDIA, "ipv6_media" },
	{ HAL_FEAT_RTCP_ADDR, "rtcp_addr" },
	{ HAL_FEAT_REGION_HOOK, "region_hook" },
	{ HAL_FEAT_DTMF_RELAY, "dtmf_relay" },
	{ HAL_FEAT_DTMF_PLAYOUT, "dtmf_playout" },
	{ HAL_FEAT_LOOP, "loop" },
};

static const char * const codec_names[] = {
	[HAL_CODEC_PCMU] = "pcmu",
	[HAL_CODEC_PCMA] = "pcma",
	[HAL_CODEC_G729] = "g729",
	[HAL_CODEC_G726_32] = "g726_32",
	[HAL_CODEC_T38] = "t38",
	[HAL_CODEC_VBD_PCMU] = "vbd_pcmu",
	[HAL_CODEC_VBD_PCMA] = "vbd_pcma",
	[HAL_CODEC_G722] = "g722",
	[HAL_CODEC_OPUS] = "opus",
};

static void hal_features_dump(struct blob_buf *b, uint32_t features)
{
	void *array;
	size_t i;

	array = blobmsg_open_array(b, "features");
	for (i = 0; i < ARRAY_SIZE(feature_names); i++) {
		if (features & feature_names[i].bit)
			blobmsg_add_string(b, NULL, feature_names[i].name);
	}
	blobmsg_close_array(b, array);
}

static void hal_codecs_dump(struct blob_buf *b, uint32_t codecs)
{
	void *array;
	size_t i;

	array = blobmsg_open_array(b, "codecs");
	for (i = 0; i < ARRAY_SIZE(codec_names); i++) {
		if (codec_names[i] && (codecs & (1u << i)))
			blobmsg_add_string(b, NULL, codec_names[i]);
	}
	blobmsg_close_array(b, array);
}

/*
 * One jitter buffer slot, and the slots that never reported are left out. A
 * stream that made no report and a stream that reported no drops are different
 * answers, and a row of zeros would read as the second one (struct uvoice_jb).
 */
static void hal_jb_event_dump(struct blob_buf *b, unsigned int line,
			      unsigned int stream)
{
	const struct uvoice_jb *jb = &uv.jb[line][stream];
	void *table;

	if (!jb->valid)
		return;

	table = blobmsg_open_table(b, NULL);
	blobmsg_add_u32(b, "line", line);
	blobmsg_add_u32(b, "stream", stream);
	blobmsg_add_u64(b, "total", jb->total);
	blobmsg_add_u64(b, "drop", jb->drop);
	blobmsg_add_u32(b, "size_ms", jb->size_ms);
	blobmsg_add_u32(b, "jitter_avg", jb->jitter_avg);
	blobmsg_add_u32(b, "stamp_ms", jb->stamp_ms);
	blobmsg_close_table(b, table);
}

static void hal_jb_events_dump(struct blob_buf *b)
{
	unsigned int line, stream;
	void *array;

	array = blobmsg_open_array(b, "jb_events");
	for (line = 0; line < HAL_LINE_MAX; line++)
		for (stream = 0; stream < HAL_STREAM_MAX; stream++)
			hal_jb_event_dump(b, line, stream);
	blobmsg_close_array(b, array);
}

static const char *hal_rt_policy_name(uint8_t policy)
{
	switch (policy) {
	case HAL_RT_POLICY_FIFO:
		return "fifo";
	case HAL_RT_POLICY_RR:
		return "rr";
	case HAL_RT_POLICY_OTHER:
		return "other";
	default:
		return "unknown";
	}
}

/* Only a backend that reported one: the stub record stays at
 * HAL_RT_POLICY_NONE, and a row of zeros would read as a granted request at
 * priority 0. */
static void hal_rt_dump(struct blob_buf *b, const struct hal_cap *cap)
{
	void *table;

	if (cap->rt_policy == HAL_RT_POLICY_NONE)
		return;

	table = blobmsg_open_table(b, "rt");
	blobmsg_add_string(b, "policy", hal_rt_policy_name(cap->rt_policy));
	blobmsg_add_u32(b, "prio", (uint32_t)cap->rt_prio);
	blobmsg_add_u32(b, "cpu", (uint32_t)(int32_t)cap->rt_cpu);
	blobmsg_add_u32(b, "sched_errno", (uint32_t)cap->rt_sched_errno);
	blobmsg_add_u8(b, "mlock", cap->rt_mlock);
	blobmsg_add_string(b, "net_policy",
			   hal_rt_policy_name(cap->rt_net_policy));
	blobmsg_close_table(b, table);
}

void status_hal_dump(struct blob_buf *b)
{
	const struct hal_cap *cap;
	struct hal_counters counters = {};
	void *table;

	table = blobmsg_open_table(b, "hal");
	blobmsg_add_string(b, "state", uvoice_hal_state_name(uv.hal_state));
	blobmsg_add_string(b, "backend", hal_backend_ops.name);

	if (!uv.hal) {
		blobmsg_close_table(b, table);
		return;
	}

	cap = hal_backend_ops.cap_get(uv.hal);
	hal_backend_ops.counters_get(uv.hal, &counters);

	blobmsg_add_string(b, "version", cap->version);
	blobmsg_add_string(b, "slic", cap->slic);
	blobmsg_add_u32(b, "lines", cap->lines);
	blobmsg_add_u32(b, "streams_per_channel", cap->streams_per_chan);
	hal_features_dump(b, cap->features);
	hal_codecs_dump(b, cap->codecs);
	blobmsg_add_u64(b, "op_errors", counters.op_errors);
	/* Nine counters every backend reports. A backend that cannot lose a
	 * completion, or that has no TDM engine or no SLIC thread, reports 0
	 * here rather than leave the reader to guess which it was. */
	blobmsg_add_u64(b, "tdm_stalls", counters.tdm_stalls);
	blobmsg_add_u64(b, "completions_lost", counters.completions_lost);
	blobmsg_add_u64(b, "holds", counters.holds);
	blobmsg_add_u64(b, "capture_drops", counters.capture_drops);
	blobmsg_add_u64(b, "trim_drops", counters.trim_drops);
	blobmsg_add_u64(b, "playback_expired", counters.playback_expired);
	blobmsg_add_u64(b, "hook_gaps", counters.hook_gaps);
	blobmsg_add_u64(b, "hook_moved_gaps", counters.hook_moved_gaps);
	blobmsg_add_u32(b, "hook_gap_max_ms", counters.hook_gap_max_ms);
	hal_rt_dump(b, cap);
	blobmsg_close_table(b, table);

	table = blobmsg_open_table(b, "events");
	blobmsg_add_u64(b, "read", uv.events_read);
	blobmsg_add_u64(b, "sent", counters.events_sent);
	blobmsg_add_u64(b, "dropped", counters.events_dropped);
	blobmsg_add_u64(b, "lost", uv.events_lost);
	blobmsg_close_table(b, table);

	hal_jb_events_dump(b);
}

/*
 * Three of the four diagnostic arrays of "voice status": streams, jb and lec.
 * The fourth one, monitor, carries the analog line state and keeps its own
 * sample words below.
 *
 * Each one asks its backend through a fast op and answers from the same frame.
 * streams gates its bounds on its op, so it is empty until a stream runs. jb
 * and lec take their bounds from the cap instead, so once the backend
 * published its cap they are a constant grid: one row per (line, stream) for
 * jb and one per line for lec, on every poll for the life of the daemon. A
 * consumer never has to handle a missing pair. The row-dump helpers
 * NULL-check the op, so a cap without a getter still emits rows and never
 * dereferences it. An absent HAL, or a cap not yet published, gives the empty
 * arrays, never an error.
 *
 * The jb rows differ from the "jb_events" cache above. That one is the last
 * HAL_EVENT_JB_UPDATE of every stream, four numbers, filled by whichever
 * backend sends the record; these rows carry the full counter set of a stream
 * that runs right now.
 *
 * One word tells the three states apart on the wire, sample:
 *   live       the counters were read in the frame that answers.
 *   last_call  the last reading this daemon took, kept after the call ended.
 *              stamp_ms names the instant it was taken, so "how did that call
 *              end" has an answer after the hangup.
 *   never      the pair or the line never answered in this daemon's lifetime.
 *              The counters read 0 and none of them is a measurement. A lec
 *              row carries no state table at all there.
 * A live read feeds the cache on its way out, so a poll during a call leaves
 * the newest numbers for the poll after it.
 */

enum status_sample {
	SAMPLE_LIVE,
	SAMPLE_LAST_CALL,
	SAMPLE_NEVER,
};

static const char *status_sample_name(enum status_sample sample)
{
	switch (sample) {
	case SAMPLE_LIVE:
		return "live";
	case SAMPLE_LAST_CALL:
		return "last_call";
	case SAMPLE_NEVER:
		return "never";
	}

	return "never";
}

/* One renderer for both kinds of row, so a live row and a cached one cannot
 * grow different keys. stamp_ms is presence gated like every other optional
 * fact of this daemon's northbound. A live row was read in the frame that
 * answers and a never row was never read, so neither names an instant. */
static void jb_row_write(struct blob_buf *b, unsigned int line,
			 unsigned int stream,
			 const struct hal_jb_stats *jb,
			 enum status_sample sample, uint32_t stamp_ms)
{
	void *table = blobmsg_open_table(b, NULL);

	blobmsg_add_u32(b, "line", line);
	blobmsg_add_u32(b, "stream", stream);
	blobmsg_add_string(b, "sample", status_sample_name(sample));
	if (sample == SAMPLE_LAST_CALL)
		blobmsg_add_u32(b, "stamp_ms", stamp_ms);
	/* the cdr.h renderer, so a live row, a cached row and a call record
	 * spell the same counters the same way */
	cdr_jb_dump(b, jb);
	blobmsg_close_table(b, table);
}

static void jb_row_dump(struct blob_buf *b, unsigned int line,
			unsigned int stream)
{
	const struct uvoice_jb_last *last;
	struct hal_jb_stats jb;

	memset(&jb, 0, sizeof(jb));
	/* A live answer wins, and it feeds the cache on its way out: a caller
	 * that polls during a call leaves the newest numbers behind for the
	 * one that polls after it. */
	if (hal_backend_ops.stream_jb_stats_get &&
	    hal_backend_ops.stream_jb_stats_get(uv.hal, line, stream, &jb) ==
	    HAL_OK) {
		uvoice_jb_cache_set(line, stream, &jb);
		jb_row_write(b, line, stream, &jb, SAMPLE_LIVE, 0);
		return;
	}

	/* The pair the cache can index and has a reading for is a stopped
	 * stream: its last numbers, not live. Everything else is the never row,
	 * all zeros, whether the pair is out of range or never answered. The
	 * array is then a constant grid and a consumer never has to handle a
	 * missing pair. */
	if (line < HAL_LINE_MAX && stream < HAL_STREAM_MAX) {
		last = &uv.jb_last[line][stream];
		if (last->valid) {
			jb_row_write(b, line, stream, &last->jb,
				     SAMPLE_LAST_CALL, last->stamp_ms);
			return;
		}
	}

	memset(&jb, 0, sizeof(jb));
	/* a never row measured nothing, so the enum's zero word would be a
	 * claim; this value renders as "unknown" instead */
	jb.mode = UINT8_MAX;
	jb_row_write(b, line, stream, &jb, SAMPLE_NEVER, 0);
}

/*
 * The two row walks: one row per line, or one row per (line, stream) pair, for
 * the lines from first to last. The bounds come from the cap and not from an
 * op, so the daemon emits the same grid on every poll. A backend that answers
 * no counters still emits a row of zeros rather than an absent pair. A NULL
 * cap gives an empty array.
 *
 * The jb, lec and monitor row helpers NULL-check their own op. The streams
 * and diag row helpers do not, so their callers pass no cap or do not walk
 * when the backend has no getter.
 */
static const struct hal_cap *status_cap_get(void)
{
	return uv.hal ? hal_backend_ops.cap_get(uv.hal) : NULL;
}

static void status_line_rows_walk(struct blob_buf *b, const char *key,
				  const struct hal_cap *cap,
				  unsigned int first, unsigned int last,
				  void (*row)(struct blob_buf *, unsigned int))
{
	unsigned int lines = cap ? cap->lines : 0;
	unsigned int line;
	void *array;

	array = blobmsg_open_array(b, key);
	for (line = first; line < lines && line <= last; line++)
		row(b, line);
	blobmsg_close_array(b, array);
}

static void status_stream_rows_walk(struct blob_buf *b, const char *key,
				    const struct hal_cap *cap,
				    unsigned int first, unsigned int last,
				    void (*row)(struct blob_buf *, unsigned int,
						unsigned int))
{
	unsigned int lines = cap ? cap->lines : 0;
	unsigned int streams = cap ? cap->streams_per_chan : 0;
	unsigned int line, stream;
	void *array;

	array = blobmsg_open_array(b, key);
	for (line = first; line < lines && line <= last; line++)
		for (stream = 0; stream < streams; stream++)
			row(b, line, stream);
	blobmsg_close_array(b, array);
}

static void status_jb_rows_dump(struct blob_buf *b,
				unsigned int first, unsigned int last)
{
	status_stream_rows_walk(b, "jb", status_cap_get(), first, last,
				jb_row_dump);
}

/* The canceller policy of a line, in the lower vocabulary a leg's fax_kind
 * uses. "voice" is the kind nobody named, which is a different answer from a
 * canceller that is off; the config and state tables carry that one. */
const char *status_lec_kind_word(uint8_t kind)
{
	switch (kind) {
	case HAL_PASSTHROUGH_OFF:
		return "voice";
	case HAL_PASSTHROUGH_MODEM:
		return "modem";
	case HAL_PASSTHROUGH_FAX:
		return "fax";
	}

	return "unknown";
}

/*
 * The daemon's own resolution: which policy the line runs under and what named
 * it. The precedence is the backend's own (udsp/src/core/chan.c
 * chan_passthrough_get): a committed description wins, and the disabler's latch
 * governs only the case call control never described. A latch that released
 * names nothing, so it falls through to the channel configuration exactly as
 * the engine does.
 */
static const char *lec_policy_daemon(unsigned int line, uint8_t *kind)
{
	const struct proto_ops *ops = proto_ops_get();
	const struct uvoice_lec_policy *p;
	bool media_running = false;

	*kind = ops->line_fax_kind ? ops->line_fax_kind(line, &media_running) :
		HAL_PASSTHROUGH_OFF;
	if (*kind != HAL_PASSTHROUGH_OFF)
		return "description";

	if (line >= HAL_LINE_MAX)
		return "config";

	/*
	 * The latch view is trusted only while the line carries media. The
	 * library's latch cannot outlive the line's streams
	 * (udsp/src/core/stream.c stream_lec_release, the clear that sends no
	 * record), so on a line whose media stopped the view is history
	 * whatever it holds, and the configuration is the truthful answer.
	 * The row's tone block still reports that history as the last move.
	 */
	p = &uv.lec_policy[line];
	if (!media_running || !p->valid || p->kind == HAL_PASSTHROUGH_OFF)
		return "config";

	*kind = p->kind;

	return "tone";
}

/*
 * The policy the row reports, and what named it. A live reading carries the
 * kind the engine applied to the frame it describes. That kind wins whenever
 * it differs from the resolution above, in either direction: an engine that
 * ran a carrier the daemon did not name, and an engine that ran none where the
 * daemon named one. The daemon then reports the policy that ran and says it
 * cannot name the cause. A cached or absent reading describes a frame that is
 * over, so only the daemon's own resolution stands there.
 */
static const char *lec_policy_resolve(unsigned int line,
				      const struct hal_lec_stats *lec,
				      enum status_sample sample, uint8_t *kind)
{
	const char *source = lec_policy_daemon(line, kind);

	if (sample != SAMPLE_LIVE || lec->passthrough == *kind)
		return source;

	*kind = lec->passthrough;

	return "engine";
}

/*
 * The last disabler transition, emitted only once one arrived. A release
 * answers no tone, so its row names neither the tone nor the feed rather than
 * printing the OTHER and the LOCAL that the record's two zeroed fields read as
 * (hal.h, enum hal_lec_reason). That is the rule the trace of event_dispatch.c
 * follows, and the wire keeps it rather than inventing a tone that never
 * sounded.
 */
static void lec_tone_write(struct blob_buf *b,
			   const struct uvoice_lec_policy *p)
{
	if (!p->valid)
		return;

	if (p->reason == HAL_LEC_REASON_LATCH ||
	    p->reason == HAL_LEC_REASON_DEMOTE) {
		blobmsg_add_string(b, "tone", hal_tone_detect_name(p->tone));
		blobmsg_add_string(b, "tone_dir",
				   hal_tone_dir_name(p->direction));
	}

	blobmsg_add_string(b, "tone_action", hal_lec_reason_name(p->reason));
	blobmsg_add_u32(b, "tone_stamp_ms", p->stamp_ms);
}

/* What the operator asked the canceller for, the uci options of the line's
 * live channel copy. nlp stands only for an option that carries a value,
 * because an absent lec_nlp reaches the canceller as nothing at all. */
static void lec_config_write(struct blob_buf *b,
			     const struct line_lec_view *view)
{
	void *table = blobmsg_open_table(b, "config");

	blobmsg_add_u8(b, "control", view->config.control);
	blobmsg_add_u8(b, "enable", view->config.enable);
	if (view->config.nlp_set)
		blobmsg_add_u8(b, "nlp", view->config.nlp);
	blobmsg_add_u8(b, "tone_latch", view->config.tone_latch);
	blobmsg_add_u8(b, "tone_force_disable",
		       view->config.tone_force_disable);
	blobmsg_close_table(b, table);
}

/* What the daemon pushed to the DSP, absent until the first push of this
 * daemon's life. pending says a seize would send a different view now, which
 * is the state a reload reaches before the next off-hook. */
static void lec_applied_write(struct blob_buf *b,
			      const struct line_lec_view *view)
{
	void *table;

	if (!view->applied.valid)
		return;

	table = blobmsg_open_table(b, "applied");
	blobmsg_add_u8(b, "ec", view->applied.ec);
	blobmsg_add_u8(b, "nlp", view->applied.nlp);
	blobmsg_add_u8(b, "tone_latch", view->applied.tone_latch);
	blobmsg_add_u8(b, "tone_force_disable",
		       view->applied.tone_force_disable);
	blobmsg_add_u32(b, "stamp_ms", view->applied.stamp_ms);
	blobmsg_add_string(b, "status", hal_status_name(view->applied.status));
	blobmsg_add_u8(b, "pending", view->applied.pending);
	blobmsg_close_table(b, table);
}

/* What the canceller runs under and what named it, with the last move of the
 * answer-tone disabler behind it. */
static void lec_policy_write(struct blob_buf *b, unsigned int line,
			     const struct hal_lec_stats *lec,
			     enum status_sample sample)
{
	static const struct uvoice_lec_policy no_policy;
	uint8_t kind = HAL_PASSTHROUGH_OFF;
	const char *source = lec_policy_resolve(line, lec, sample, &kind);
	void *table = blobmsg_open_table(b, "policy");

	blobmsg_add_string(b, "kind", status_lec_kind_word(kind));
	blobmsg_add_string(b, "source", source);
	lec_tone_write(b, line < HAL_LINE_MAX ?
			     &uv.lec_policy[line] : &no_policy);
	blobmsg_close_table(b, table);
}

/* Whether the filter ran on the last engine frame. */
const char *status_lec_ec_word(const struct hal_lec_stats *lec)
{
	return lec->ec_active ? "on" : "off";
}

/* One word for the two processor flags, so a reader has one thing to look at.
 * The switch the policy asked for and the per-frame gate answer the same
 * question at two depths. */
const char *status_lec_nlp_word(const struct hal_lec_stats *lec)
{
	if (!lec->nlp_enabled)
		return "off";

	return lec->nlp_active ? "acting" : "on";
}

/*
 * What the engine measured. The counters go out as 64 bit fields, the way the
 * call record spells every counter. The three scaled figures carry their unit
 * in their key. erle_mdb is signed and travels in the INT32 slot, which the
 * json renderer prints signed. The two micro-unit figures are unsigned and
 * travel in the wider slot, so a saturated reading cannot come out negative.
 * The peak tap is an index, like taps, so it takes the u32 slot, and loop_open
 * is a flag, like ref_ready.
 */
static void lec_state_write(struct blob_buf *b, const struct hal_lec_stats *lec)
{
	void *table = blobmsg_open_table(b, "state");

	blobmsg_add_string(b, "ec", status_lec_ec_word(lec));
	blobmsg_add_string(b, "nlp", status_lec_nlp_word(lec));
	blobmsg_add_u8(b, "double_talk", lec->double_talk);
	blobmsg_add_u32(b, "tail_ms", lec->tail_ms);
	blobmsg_add_u32(b, "taps", lec->taps);
	blobmsg_add_u64(b, "frames", lec->frames);
	blobmsg_add_u64(b, "hold_frames", lec->hold_frames);
	blobmsg_add_u64(b, "skip_frames", lec->skip_frames);
	blobmsg_add_u64(b, "diverge_resets", lec->diverge_resets);
	blobmsg_add_u64(b, "retimes", lec->retimes);
	blobmsg_add_u64(b, "probes", lec->probes);
	blobmsg_add_u64(b, "nlp_active_frames", lec->nlp_active_frames);
	blobmsg_add_u64(b, "nlp_wait_frames", lec->nlp_wait_frames);
	blobmsg_add_u64(b, "nlp_erle_frames", lec->nlp_erle_frames);
	blobmsg_add_u64(b, "nlp_unlike_frames", lec->nlp_unlike_frames);
	blobmsg_add_u64(b, "nlp_dt_frames", lec->nlp_dt_frames);
	blobmsg_add_u64(b, "nlp_quiet_frames", lec->nlp_quiet_frames);
	blobmsg_add_u32(b, "erle_mdb", (uint32_t)lec->erle_mdb);
	blobmsg_add_u64(b, "coef_energy_micro", lec->coef_energy_micro);
	blobmsg_add_u32(b, "coef_peak_tap", lec->coef_peak_tap);
	blobmsg_add_u64(b, "echo_gain_micro", lec->echo_gain_micro);
	blobmsg_add_u32(b, "ref_delay_samples", lec->ref_delay_samples);
	blobmsg_add_u8(b, "ref_ready", lec->ref_ready);
	blobmsg_add_u8(b, "loop_open", lec->loop_open);
	blobmsg_close_table(b, table);
}

/*
 * The line echo canceller rows, the surface an echo investigation reads. The
 * jb rows answer for a stream; these answer for a line, because the canceller
 * belongs to the analog line and not to any call leg on it.
 *
 * The row carries the same sample word a jb row carries. The cache below is
 * the lec counterpart of the jb poll cache: a live read wins and feeds the
 * cache, and a line that no longer answers keeps its last reading with the
 * stamp it was taken at. The engine refuses a reading only for a line that
 * holds no canceller instance, so a bypassed canceller stays live and answers
 * ec off.
 *
 * Four layers, each its own table. config is what the operator asked for and
 * policy is what the line runs under. Both stand in every row, a never row
 * included, because a line that never ran a canceller still has a
 * configuration and still runs a policy. applied is the record of the last
 * push and is absent until one happened. state is the engine's measurement,
 * and it is absent from a never row, where nothing was ever measured.
 */
static void lec_row_write(struct blob_buf *b, unsigned int line,
			  const struct hal_lec_stats *lec,
			  enum status_sample sample, uint32_t stamp_ms)
{
	struct line_lec_view view;
	void *table = blobmsg_open_table(b, NULL);

	line_lec_view_get(line, &view);

	blobmsg_add_u32(b, "line", line);
	blobmsg_add_string(b, "sample", status_sample_name(sample));
	if (sample == SAMPLE_LAST_CALL)
		blobmsg_add_u32(b, "stamp_ms", stamp_ms);

	lec_config_write(b, &view);
	lec_applied_write(b, &view);
	lec_policy_write(b, line, lec, sample);
	if (sample != SAMPLE_NEVER)
		lec_state_write(b, lec);

	blobmsg_close_table(b, table);
}

static void lec_row_dump(struct blob_buf *b, unsigned int line)
{
	const struct uvoice_lec_last *last;
	struct hal_lec_stats lec;

	memset(&lec, 0, sizeof(lec));
	/* A live answer wins and feeds the cache, the jb_row_dump shape. */
	if (hal_backend_ops.line_lec_stats_get &&
	    hal_backend_ops.line_lec_stats_get(uv.hal, line, &lec) == HAL_OK) {
		uvoice_lec_cache_set(line, &lec);
		lec_row_write(b, line, &lec, SAMPLE_LIVE, 0);
		return;
	}

	if (line < HAL_LINE_MAX) {
		last = &uv.lec_last[line];
		if (last->valid) {
			lec_row_write(b, line, &last->lec, SAMPLE_LAST_CALL,
				      last->stamp_ms);
			return;
		}
	}

	memset(&lec, 0, sizeof(lec));
	lec_row_write(b, line, &lec, SAMPLE_NEVER, 0);
}

/* A backend that leaves the canceller getter NULL emits never rows. */
static void status_lec_rows_dump(struct blob_buf *b,
				 unsigned int first, unsigned int last)
{
	status_line_rows_walk(b, "lec", status_cap_get(), first, last,
			      lec_row_dump);
}

/*
 * The line monitor rows: the analog state of the line. Each row carries the
 * voltages, the currents, the battery, the HVIC power and the DC-DC converter
 * words the backend read from the SLIC. There is one row per line for the
 * life of the daemon, the lec grid rule, so a consumer never handles a
 * missing line.
 *
 * Three words tell the states apart, and they are this array's own:
 *   sampled  a reading stands, and stamp_ms names the instant it was taken.
 *   stale    the last read answered no data, so the values are the previous
 *            reading and stamp_ms names the instant of that reading.
 *   never    no reading landed in this daemon's lifetime, or the backend
 *            offers no monitor. The row carries no measurement at all.
 *
 * The row names its instant on every reading it has. The jb and lec rows
 * deliberately do not. The sample table above jb_row_write() carries their
 * rule: a live row was read in the frame that answers. A monitor reading
 * is up to ten seconds old by design, because the backend samples the part on
 * its own cadence. A reader that cannot age such a reading cannot use it.
 *
 * The three fault fields stand in every row, a never row included. They are
 * the backend engine's own state of the line, and they are defined before the
 * first sample.
 */

enum monitor_sample {
	MONITOR_SAMPLED,
	MONITOR_STALE,
	MONITOR_NEVER,
};

static const char *monitor_sample_name(enum monitor_sample sample)
{
	switch (sample) {
	case MONITOR_SAMPLED:
		return "sampled";
	case MONITOR_STALE:
		return "stale";
	case MONITOR_NEVER:
		return "never";
	}

	return "never";
}

/* The measurements, the raw words and their decode. The signed readings travel
 * in the INT32 slot, which the json renderer prints signed. The raw words and
 * the masks are unsigned and travel in the same slot, the width blobmsg
 * carries. */
static void monitor_values_write(struct blob_buf *b,
				 const struct hal_monitor *mon)
{
	blobmsg_add_u32(b, "vtr_mv", (uint32_t)mon->vtr_mv);
	blobmsg_add_u32(b, "vtip_mv", (uint32_t)mon->vtip_mv);
	blobmsg_add_u32(b, "vring_mv", (uint32_t)mon->vring_mv);
	blobmsg_add_u32(b, "vlong_mv", (uint32_t)mon->vlong_mv);
	blobmsg_add_u32(b, "vbat_mv", (uint32_t)mon->vbat_mv);
	blobmsg_add_u32(b, "vdc_mv", (uint32_t)mon->vdc_mv);
	blobmsg_add_u32(b, "itr_ua", (uint32_t)mon->itr_ua);
	blobmsg_add_u32(b, "itip_ua", (uint32_t)mon->itip_ua);
	blobmsg_add_u32(b, "iring_ua", (uint32_t)mon->iring_ua);
	blobmsg_add_u32(b, "ilong_ua", (uint32_t)mon->ilong_ua);
	blobmsg_add_u32(b, "p_hvic_mw", (uint32_t)mon->p_hvic_mw);
	blobmsg_add_u32(b, "vbat_tgt_mv", (uint32_t)mon->vbat_tgt_mv);
	blobmsg_add_u32(b, "vbat_irq_th_mv", (uint32_t)mon->vbat_irq_th_mv);
	blobmsg_add_u32(b, "pd_dcdc", mon->pd_dcdc);
	blobmsg_add_u32(b, "dcdc_status", mon->dcdc_status);
	blobmsg_add_u32(b, "lcrrtp", mon->lcrrtp);
	blobmsg_add_u32(b, "linefeed", mon->linefeed);
	blobmsg_add_u8(b, "dcdc_down", mon->dcdc_down);
	blobmsg_add_u8(b, "ovlo", mon->ovlo);
	blobmsg_add_u8(b, "settled", mon->settled);
	blobmsg_add_u8(b, "voc_err", mon->voc_err);
	blobmsg_add_u8(b, "lcr", mon->lcr);
	blobmsg_add_u8(b, "rtp", mon->rtp);
}

static void monitor_row_write(struct blob_buf *b, unsigned int line,
			      const struct hal_monitor *mon,
			      enum monitor_sample sample)
{
	void *table = blobmsg_open_table(b, NULL);

	blobmsg_add_u32(b, "line", line);
	blobmsg_add_string(b, "sample", monitor_sample_name(sample));
	if (sample != MONITOR_NEVER) {
		blobmsg_add_u32(b, "stamp_ms", mon->stamp_ms);
		monitor_values_write(b, mon);
	}

	blobmsg_add_u32(b, "fault_bits", mon->fault_bits);
	blobmsg_add_u8(b, "fault_open", mon->fault_open);
	blobmsg_add_u8(b, "fault_held", mon->fault_held);
	blobmsg_close_table(b, table);
}

/* The word a row reads. A refused answer and a record with no reading both
 * read never; the record's own stale flag picks between the two reading
 * words. */
static enum monitor_sample monitor_sample_get(const struct hal_monitor *mon,
					      enum hal_status st)
{
	if (st != HAL_OK || !mon->valid)
		return MONITOR_NEVER;

	return mon->stale ? MONITOR_STALE : MONITOR_SAMPLED;
}

static void monitor_row_dump(struct blob_buf *b, unsigned int line)
{
	struct hal_monitor mon;
	enum hal_status st = HAL_UNSUPPORTED;

	memset(&mon, 0, sizeof(mon));
	if (hal_backend_ops.line_monitor_get)
		st = hal_backend_ops.line_monitor_get(uv.hal, line, &mon);
	/* A backend with no monitor op, a line the backend refuses and a dead
	 * line all read never. The record is dropped with the answer, because
	 * a backend that wrote part of it owes nothing on a refusal. */
	if (st != HAL_OK)
		memset(&mon, 0, sizeof(mon));

	monitor_row_write(b, line, &mon, monitor_sample_get(&mon, st));
}

static void status_monitor_rows_dump(struct blob_buf *b,
				     unsigned int first, unsigned int last)
{
	status_line_rows_walk(b, "monitor", status_cap_get(), first, last,
			      monitor_row_dump);
}

/*
 * The live media rows: the counters "voice stats_live" fetches through a slow
 * job, answered here from cached engine state where the backend can (hal.h
 * stream_stats_get). A backend whose numbers need the slow op answers
 * HAL_UNSUPPORTED for every pair and the array stays empty; the deferred verb
 * remains the path that works on every backend.
 *
 * ui_slot names the UI slot whose leg owns the stream (proto/proto.h
 * ops->stream_of_slot) and is presence gated: a stream no leg owns has no UI
 * name to carry, and at most one leg holds a stream, so the first match is
 * the only one.
 */
static void stream_row_dump(struct blob_buf *b, unsigned int line,
			    unsigned int stream)
{
	const struct proto_ops *ops = proto_ops_get();
	struct hal_media_stats st;
	unsigned int ui;
	void *table;

	memset(&st, 0, sizeof(st));
	if (hal_backend_ops.stream_stats_get(uv.hal, line, stream, &st) !=
	    HAL_OK)
		return;

	table = blobmsg_open_table(b, NULL);
	blobmsg_add_u32(b, "line", line);
	blobmsg_add_u32(b, "stream", stream);
	for (ui = 0; ops->stream_of_slot && ui < HAL_STREAM_MAX; ui++) {
		if (ops->stream_of_slot(line, ui) != stream)
			continue;
		blobmsg_add_u32(b, "ui_slot", ui);
		break;
	}
	cdr_media_dump(b, &st);
	blobmsg_close_table(b, table);
}

static void status_streams_rows_dump(struct blob_buf *b,
				     unsigned int first, unsigned int last)
{
	status_stream_rows_walk(b, "streams",
				hal_backend_ops.stream_stats_get ?
				status_cap_get() : NULL,
				first, last, stream_row_dump);
}

/*
 * The backend's own diagnostic counters, and the one surface that carries
 * them: the transport and encryption figures of every stream that answers,
 * and one snapshot of the engine behind them.
 *
 * The whole table is absent on a backend that counts neither. The stub
 * leaves both ops NULL, and an empty table there would read as a
 * backend with nothing to report, which is the opposite of "this backend
 * cannot tell you". It is the rule hal.h states for every diagnostic getter.
 *
 * A stream row stands only for a stream the getter answered for, the rule the
 * streams array above follows: an idle stream carries no counters of a call
 * and a row of zeros would read as a stream that ran silent. The engine table
 * stands once the backend published a snapshot, which its watchdog does once
 * per period.
 */
static void diag_stream_row_dump(struct blob_buf *b, unsigned int line,
				 unsigned int stream)
{
	struct hal_stream_diag diag;
	void *table;

	memset(&diag, 0, sizeof(diag));
	if (hal_backend_ops.stream_diag_get(uv.hal, line, stream, &diag) !=
	    HAL_OK)
		return;

	table = blobmsg_open_table(b, NULL);
	blobmsg_add_u32(b, "line", line);
	blobmsg_add_u32(b, "stream", stream);
	blobmsg_add_u64(b, "net_drops", diag.net_drops);
	blobmsg_add_u64(b, "net_stale", diag.net_stale);
	blobmsg_add_u64(b, "net_trunc", diag.net_trunc);
	blobmsg_add_u64(b, "tx_fail", diag.tx_fail);
	blobmsg_add_u64(b, "punch_sent", diag.punch_sent);
	blobmsg_add_u64(b, "srtp_protect_ok", diag.srtp_protect_ok);
	blobmsg_add_u64(b, "srtp_protect_fail", diag.srtp_protect_fail);
	blobmsg_add_u64(b, "srtp_unprotect_ok", diag.srtp_unprotect_ok);
	blobmsg_add_u64(b, "srtp_unprotect_auth_fail",
			diag.srtp_unprotect_auth_fail);
	blobmsg_add_u64(b, "srtp_unprotect_replay", diag.srtp_unprotect_replay);
	blobmsg_close_table(b, table);
}

/* The tick interval histogram, one slot per bucket. The upper edges are 9, 11,
 * 12, 15, 20, 30 and 50 milliseconds and the last bucket holds everything
 * above 50 ms, so the array is read by position. */
static void diag_interval_dump(struct blob_buf *b,
			       const struct hal_engine_diag *eng)
{
	void *array = blobmsg_open_array(b, "interval");
	size_t i;

	for (i = 0; i < ARRAY_SIZE(eng->interval); i++)
		blobmsg_add_u64(b, NULL, eng->interval[i]);
	blobmsg_close_array(b, array);
}

/* The four signed figures travel in the INT32 slot, which the json renderer
 * prints signed; every other number here is unsigned. */
static void diag_engine_dump(struct blob_buf *b)
{
	struct hal_engine_diag eng;
	void *table;

	memset(&eng, 0, sizeof(eng));
	if (!uv.hal || !hal_backend_ops.engine_diag_get ||
	    hal_backend_ops.engine_diag_get(uv.hal, &eng) != HAL_OK)
		return;

	table = blobmsg_open_table(b, "engine");
	blobmsg_add_u64(b, "serial", eng.serial);
	blobmsg_add_u64(b, "ticks", eng.ticks);
	diag_interval_dump(b, &eng);
	blobmsg_add_u64(b, "interval_min_us", eng.interval_min_us);
	blobmsg_add_u64(b, "interval_max_us", eng.interval_max_us);
	blobmsg_add_u64(b, "capture_stalls", eng.capture_stalls);
	blobmsg_add_u64(b, "delay_stalls", eng.delay_stalls);
	blobmsg_add_u64(b, "xruns", eng.xruns);
	blobmsg_add_u64(b, "holds", eng.holds);
	blobmsg_add_u64(b, "capture_drops", eng.capture_drops);
	blobmsg_add_u64(b, "trim_drops", eng.trim_drops);
	blobmsg_add_u64(b, "playback_expired", eng.playback_expired);
	blobmsg_add_u64(b, "recover_failures", eng.recover_failures);
	blobmsg_add_u64(b, "read_errors", eng.read_errors);
	blobmsg_add_u64(b, "write_errors", eng.write_errors);
	blobmsg_add_u64(b, "cal_restarts", eng.cal_restarts);
	blobmsg_add_u32(b, "delay_ref", (uint32_t)eng.delay_ref);
	blobmsg_add_u32(b, "loop_ref", (uint32_t)eng.loop_ref);
	blobmsg_add_u32(b, "delay_drift_min", (uint32_t)eng.delay_drift_min);
	blobmsg_add_u32(b, "delay_drift_max", (uint32_t)eng.delay_drift_max);
	blobmsg_add_u32(b, "granted_periods", eng.granted_periods);
	blobmsg_add_u32(b, "period_us", eng.period_us);
	blobmsg_add_u8(b, "running", eng.running);
	blobmsg_add_u8(b, "stopped_fatal", eng.stopped_fatal);
	blobmsg_add_u8(b, "delay_calibrated", eng.delay_calibrated);
	blobmsg_close_table(b, table);
}

void status_diag_dump(struct blob_buf *b, int want_line)
{
	bool all_lines = want_line < 0;
	void *table;

	if (!hal_backend_ops.stream_diag_get)
		return;

	table = blobmsg_open_table(b, "diag");
	status_stream_rows_walk(b, "streams", status_cap_get(),
				all_lines ? 0 : (unsigned int)want_line,
				all_lines ? UINT_MAX : (unsigned int)want_line,
				diag_stream_row_dump);
	diag_engine_dump(b);
	blobmsg_close_table(b, table);
}

void status_streams_dump(struct blob_buf *b)
{
	status_streams_rows_dump(b, 0, UINT_MAX);
}

void status_jb_dump(struct blob_buf *b)
{
	status_jb_rows_dump(b, 0, UINT_MAX);
}

void status_lec_dump(struct blob_buf *b)
{
	status_lec_rows_dump(b, 0, UINT_MAX);
}

void status_monitor_dump(struct blob_buf *b)
{
	status_monitor_rows_dump(b, 0, UINT_MAX);
}

void status_line_rows_dump(struct blob_buf *b, unsigned int line)
{
	status_streams_rows_dump(b, line, line);
	status_jb_rows_dump(b, line, line);
	status_lec_rows_dump(b, line, line);
	status_monitor_rows_dump(b, line, line);
}
