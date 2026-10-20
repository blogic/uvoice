// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * x86 HAL backend. It implements the whole ops table so the core, the ubus
 * surface and the event transport can run in CI without an AN7581. Media ops
 * record what they were asked to do and succeed; hook, digit and flash events
 * are injected through the ubus object voice.stub. The slow ops complete
 * through the same socketpair a hardware backend uses, so the job path is
 * exercised for real rather than short circuited.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <libubox/uloop.h>
#include <libubus.h>

#include "../../str.h"
#include "../../uvoice.h"
#include "../hal.h"

#define STUB_JOB_DELAY_MS	20
/* The late levers of stub_dsp_start: long enough for a SIGTERM to reach
 * the daemon and start its teardown before the completion lands, and a dsp stop
 * that holds its own completion back for longer than that, so the teardown has
 * a nested wait running when the bring-up completion arrives. Both stay under
 * the 3 s shutdown backstop of main.c. */
#define STUB_JOB_LATE_DELAY_MS	2000
#define STUB_JOB_STOP_HOLD_MS	2500
#define STUB_LINES		2
#define STUB_STREAMS_PER_CHAN	2
/* The longest loop delay this backend takes, the libudsp bound
 * (UDSP_LOOP_DELAY_MAX_MS), so a description CI accepts is one the shipped
 * backend accepts too. */
#define STUB_LOOP_DELAY_MAX_MS	1000
/* Completion retries are bounded here because the stub writes from the uloop
 * thread: an unbounded retry would spin against the reader that runs on that
 * same thread. A real backend worker retries without a bound. The core job
 * timeout is the outer backstop either way. */
#define STUB_COMPLETION_TRIES	5
#define STUB_COMPLETION_WAIT_MS	40

struct stub_stream {
	bool configured;
	bool running;
	bool stats_set;			/* the media_stats knob wrote this stream */
	bool fax_set;			/* a fax profile reached this stream */
	/* A fax passthrough profile governs this stream. It ends with the
	 * stream and with the next description, because hal.h leaves the kind
	 * of a later programming on the description alone. */
	bool fax_governs;
	struct hal_media media;
	struct hal_fax_profile fax;
	struct hal_media_stats stats;
};

struct stub_line {
	bool offhook;
	bool ringing;
	/* The line canceller instance, which this backend has no canceller to
	 * hold. libudsp makes the instance at stream config (udsp core/stream.c
	 * stream_lec_ensure). It releases the instance once no stream of the
	 * line is described any more (stream_lec_release through
	 * stream_line_used). The flag follows that lifetime. */
	bool lec_made;
	/* How often the call layer dropped the tone-disabler latch of this
	 * line (hal.h line_lec_latch_clear). This backend runs no disabler, so
	 * the count is what a test or the e2e can read back. */
	unsigned int latch_clears;
	uint8_t state;
	uint8_t polarity;
	uint8_t mwi;
	uint8_t tone;
	uint32_t tone_duration_ms;
	uint8_t cw_variant;
	uint32_t cw_variant_duration_ms;
	uint8_t conf;
	uint8_t loop_mode;
	uint8_t loop_peer;
	uint16_t loop_delay_ms;
	struct hal_cid cid;
	struct hal_channel_cfg chan;
	struct hal_hook_timing hook;
	struct stub_stream stream[HAL_STREAM_MAX];
	struct uloop_timeout ring_timer;
	unsigned int index;
	char test_result[64];
};

struct stub_job {
	struct uloop_timeout timeout;
	struct hal *h;
	uint32_t token;
	int32_t status;
};

struct hal {
	int event_fd;
	uint16_t seq;
	uint32_t token_next;
	struct hal_cap cap;
	struct hal_counters counters;
	struct stub_line line[HAL_LINE_MAX];
	char region[8];
};

static struct hal *stub_dev;
static struct blob_buf b;

static bool event_write(struct hal *h, const struct hal_event *ev)
{
	ssize_t len = send(h->event_fd, ev, sizeof(*ev), MSG_DONTWAIT | MSG_NOSIGNAL);

	if (len != (ssize_t)sizeof(*ev))
		return false;

	h->counters.events_sent++;

	return true;
}

static void event_push(struct hal *h, struct hal_event *ev)
{
	ev->seq = ++h->seq;
	ev->stamp_ms = uvoice_mono_ms();

	if (event_write(h, ev))
		return;

	/* hal.h section 2 (a): the pump never blocks and never queues. */
	h->counters.events_dropped++;
}

/*
 * hal.h section 2 (b): a lost completion strands a call, so this record is
 * retried instead of dropped. seq stays 0 to keep it out of gap accounting.
 */
static void event_push_completion(struct hal *h, struct hal_event *ev)
{
	struct pollfd pfd = { .fd = h->event_fd, .events = POLLOUT };
	int try;

	ev->seq = 0;
	ev->stamp_ms = uvoice_mono_ms();

	for (try = 0; try < STUB_COMPLETION_TRIES; try++) {
		if (event_write(h, ev))
			return;

		poll(&pfd, 1, STUB_COMPLETION_WAIT_MS);
	}

	h->counters.events_dropped++;
	ULOG_ERR("stub: completion for token %u dropped\n", ev->u.job.token);
}

static struct stub_line *line_get(struct hal *h, unsigned int line)
{
	if (line >= h->cap.lines) {
		h->counters.op_errors++;
		return NULL;
	}

	return &h->line[line];
}

static struct stub_stream *stream_get(struct hal *h, unsigned int line,
				      unsigned int stream)
{
	struct stub_line *l = line_get(h, line);

	if (!l || stream >= h->cap.streams_per_chan) {
		h->counters.op_errors++;
		return NULL;
	}

	return &l->stream[stream];
}

static void job_complete(struct uloop_timeout *t)
{
	struct stub_job *job = container_of(t, struct stub_job, timeout);
	struct hal_event ev = { .type = HAL_EVENT_JOB_DONE };

	ev.u.job.token = job->token;
	ev.u.job.status = job->status;
	event_push_completion(job->h, &ev);
	free(job);
}

static enum hal_status job_schedule_after(struct hal *h, struct hal_job *out,
					  enum hal_status status, unsigned int delay_ms)
{
	struct stub_job *job = calloc(1, sizeof(*job));

	if (!job)
		return HAL_FATAL;

	job->h = h;
	job->token = ++h->token_next;
	job->status = (int32_t)status;
	job->timeout.cb = job_complete;
	out->token = job->token;
	uloop_timeout_set(&job->timeout, delay_ms);

	return HAL_OK;
}

static enum hal_status job_schedule(struct hal *h, struct hal_job *out,
				    enum hal_status status)
{
	return job_schedule_after(h, out, status, STUB_JOB_DELAY_MS);
}

static struct hal *stub_open(int event_fd)
{
	struct hal *h = calloc(1, sizeof(*h));
	unsigned int i;

	if (!h)
		return NULL;

	h->event_fd = event_fd;
	for (i = 0; i < HAL_LINE_MAX; i++)
		h->line[i].index = i;

	snprintf(h->cap.backend, sizeof(h->cap.backend), "stub");
	str_copy(h->cap.version, sizeof(h->cap.version), UVOICE_VERSION);
	snprintf(h->cap.slic, sizeof(h->cap.slic), "none");
	h->cap.lines = STUB_LINES;
	h->cap.streams_per_chan = STUB_STREAMS_PER_CHAN;
	/* HAL_FEAT_CONF is advertised so CI exercises conf_set.
	 * HAL_FEAT_T38 is advertised for the same reason, so CI reaches the
	 * T.38 stream path. HAL_FEAT_SRTP joins them on the same
	 * precedent, so CI reaches the SRTP offer path.
	 * HAL_FEAT_G729B takes that precedent too, so CI reaches the Annex B
	 * advertisement and the armed transmit DTX; a backend that carries an
	 * Annex A coder alone leaves it clear and its legs then write
	 * annexb=no. HAL_FEAT_IPV6_MEDIA and HAL_FEAT_RTCP_ADDR take the same
	 * precedent, so CI reaches an IPv6 stream and a separate RTCP
	 * address. HAL_FEAT_LOOP takes it as well, so CI reaches the loop test
	 * arm; the udsp backend maps that bit from the library. */
	h->cap.features = HAL_FEAT_G729 | HAL_FEAT_G729B | HAL_FEAT_G726 |
			  HAL_FEAT_LINE_TEST | HAL_FEAT_POLARITY |
			  HAL_FEAT_MWI_LAMP | HAL_FEAT_CONF | HAL_FEAT_T38 |
			  HAL_FEAT_SRTP | HAL_FEAT_IPV6_MEDIA |
			  HAL_FEAT_RTCP_ADDR | HAL_FEAT_LOOP;
	/*
	 * The G.726-32 and T.38 codec bits travel with their feature bits.
	 * This backend accepts both configs. It must describe a consistent DSP.
	 *
	 * G.726-32 and Opus are in the answer because the udsp media plane
	 * carries both codecs. The codec mask decides what a leg may offer
	 * (call.h calls_hal_codecs_set). A CI daemon needs these bits to reach
	 * the flows.
	 */
	h->cap.codecs = (1u << HAL_CODEC_PCMU) | (1u << HAL_CODEC_PCMA) |
			(1u << HAL_CODEC_G729) | (1u << HAL_CODEC_T38) |
			(1u << HAL_CODEC_G722) | (1u << HAL_CODEC_OPUS) |
			(1u << HAL_CODEC_G726_32);
	/* the default of the SDP layer (sdp.c SDP_T38_MAX_DATAGRAM), so the
	 * stub leaves the announced T38FaxMaxDatagram as it is */
	h->cap.t38_max_datagram = 72;
	snprintf(h->region, sizeof(h->region), "USA");

	stub_dev = h;
	ULOG_INFO("stub hal attached: %u lines, %u streams per channel\n",
		  h->cap.lines, h->cap.streams_per_chan);

	return h;
}

static void stub_close(struct hal *h)
{
	unsigned int i;

	if (!h)
		return;

	for (i = 0; i < HAL_LINE_MAX; i++)
		uloop_timeout_cancel(&h->line[i].ring_timer);

	stub_dev = NULL;
	free(h);
}

static const struct hal_cap *stub_cap_get(struct hal *h)
{
	return &h->cap;
}

static void stub_counters_get(struct hal *h, struct hal_counters *out)
{
	*out = h->counters;
}

static enum hal_status stub_line_state_set(struct hal *h, unsigned int line,
					   enum hal_line_state state)
{
	struct stub_line *l = line_get(h, line);

	if (!l)
		return HAL_FATAL;

	l->state = state;
	TRACE(TRACE_HAL, "stub: line %u state=%u", line, state);

	return HAL_OK;
}

static enum hal_status stub_line_polarity_set(struct hal *h, unsigned int line,
					      enum hal_polarity pol)
{
	struct stub_line *l = line_get(h, line);

	if (!l)
		return HAL_FATAL;

	l->polarity = pol;
	TRACE(TRACE_HAL, "stub: line %u polarity=%u", line, pol);

	return HAL_OK;
}

static enum hal_status stub_line_hook_query(struct hal *h, unsigned int line,
					    bool *offhook)
{
	struct stub_line *l = line_get(h, line);

	if (!l)
		return HAL_FATAL;

	*offhook = l->offhook;

	return HAL_OK;
}

static enum hal_status stub_line_hook_timing_set(struct hal *h, unsigned int line,
						 const struct hal_hook_timing *ts)
{
	struct stub_line *l = line_get(h, line);

	if (!l)
		return HAL_FATAL;

	l->hook = *ts;
	TRACE(TRACE_HAL, "stub: line %u hook timing flash=%u..%u release=%u",
	      line, ts->flash_min_ms, ts->flash_max_ms, ts->release_min_ms);

	return HAL_OK;
}

static void ring_end_emit(struct hal *h, struct stub_line *l)
{
	struct hal_event ev = { .type = HAL_EVENT_RING_END };

	l->ringing = false;
	uloop_timeout_cancel(&l->ring_timer);
	ev.line = l->index;
	event_push(h, &ev);
}

static void ring_expire(struct uloop_timeout *t)
{
	struct stub_line *l = container_of(t, struct stub_line, ring_timer);

	TRACE(TRACE_HAL, "stub: line %u ring duration elapsed", l->index);
	ring_end_emit(stub_dev, l);
}

/*
 * The test-only fault injection of the ring op, in the transient_once shape
 * stub_dsp_start uses. UVOICE_STUB_RING_START=transient_once answers the first
 * ring start HAL_TRANSIENT and every later one HAL_OK. That is what libudsp
 * answers while a metallic line test holds the part. fatal_once and
 * unsupported_once answer their own status the same way. Every other value,
 * the absent variable included, answers ok. The variable is read at each call,
 * so the shape needs no state beyond the call count.
 */
static enum hal_status ring_start_status_take(void)
{
	static unsigned int ring_starts;
	const char *mode = getenv("UVOICE_STUB_RING_START");

	if (!mode)
		return HAL_OK;

	ring_starts++;
	if (ring_starts > 1)
		return HAL_OK;

	if (!strcmp(mode, "transient_once"))
		return HAL_TRANSIENT;
	if (!strcmp(mode, "fatal_once"))
		return HAL_FATAL;
	if (!strcmp(mode, "unsupported_once"))
		return HAL_UNSUPPORTED;

	return HAL_OK;
}

/*
 * The test-only cap on the ring timer. UVOICE_STUB_RING_CAP_MS holds the timer
 * to that many milliseconds when it is shorter than the duration the core sent,
 * so a suite can watch a 60 s ring end inside a pumped window. It is a clamp
 * and not the one-shot fault injector UVOICE_STUB_RING_START is: only the
 * getenv placement is shared. The absent variable keeps the duration the core
 * sent, and the trace keeps it either way, because the trace reports what the
 * core asked for.
 */
static uint32_t ring_cap_take(uint32_t duration_ms)
{
	const char *cap = getenv("UVOICE_STUB_RING_CAP_MS");
	unsigned long ms;

	if (!cap)
		return duration_ms;

	ms = strtoul(cap, NULL, 10);
	if (!ms || ms >= duration_ms)
		return duration_ms;

	return (uint32_t)ms;
}

static enum hal_status stub_line_ring_start(struct hal *h, unsigned int line,
					    const struct hal_ring *ring)
{
	struct hal_event ev = { .type = HAL_EVENT_RING_START };
	struct stub_line *l = line_get(h, line);
	enum hal_status st;

	if (!l)
		return HAL_FATAL;

	st = ring_start_status_take();
	if (st != HAL_OK) {
		TRACE(TRACE_HAL, "stub: line %u ring start refused: %s", line,
		      hal_status_name(st));
		return st;
	}

	l->ringing = true;
	l->state = HAL_LINE_ACTIVE;
	if (ring->cid_present)
		l->cid = ring->cid;

	uloop_timeout_cancel(&l->ring_timer);
	if (ring->duration_ms) {
		l->ring_timer.cb = ring_expire;
		uloop_timeout_set(&l->ring_timer,
				  (int)ring_cap_take(ring->duration_ms));
	}

	TRACE(TRACE_HAL, "stub: line %u ring start cadences=%u cid=%d dur=%u", line,
	      ring->cadence_count, ring->cid_present, ring->duration_ms);

	ev.line = line;
	event_push(h, &ev);

	return HAL_OK;
}

static enum hal_status stub_line_ring_stop(struct hal *h, unsigned int line)
{
	struct stub_line *l = line_get(h, line);

	if (!l)
		return HAL_FATAL;

	if (!l->ringing)
		return HAL_OK;

	TRACE(TRACE_HAL, "stub: line %u ring stop", line);
	ring_end_emit(h, l);

	return HAL_OK;
}

static enum hal_status stub_line_cid_send(struct hal *h, unsigned int line,
					  enum hal_cid_kind kind,
					  const struct hal_cid *cid)
{
	struct hal_event ev = { .type = HAL_EVENT_CID_SENT };
	struct stub_line *l = line_get(h, line);

	if (!l)
		return HAL_FATAL;

	l->cid = *cid;
	TRACE(TRACE_HAL, "stub: line %u cid kind=%u number='%s' name='%s' mwi=%u",
	      line, kind, cid->number, cid->name, cid->mwi);

	ev.line = line;
	ev.u.cid_sent.kind = kind;
	ev.u.cid_sent.status = 0;
	event_push(h, &ev);

	return HAL_OK;
}

static enum hal_status stub_line_mwi_set(struct hal *h, unsigned int line,
					 enum hal_mwi mwi)
{
	struct stub_line *l = line_get(h, line);

	if (!l)
		return HAL_FATAL;

	l->mwi = mwi;
	TRACE(TRACE_HAL, "stub: line %u mwi lamp=%u", line, mwi);

	return HAL_OK;
}

static enum hal_status stub_line_test_result_get(struct hal *h, unsigned int line,
						 void *buf, size_t buf_len,
						 size_t *out_len)
{
	struct stub_line *l = line_get(h, line);
	size_t len;

	if (!l)
		return HAL_FATAL;

	len = strlen(l->test_result) + 1;
	if (len > buf_len)
		return HAL_FATAL;

	memcpy(buf, l->test_result, len);
	*out_len = len;

	return HAL_OK;
}

static enum hal_status stub_tone_play(struct hal *h, unsigned int line,
				      enum hal_tone tone, uint32_t duration_ms)
{
	struct stub_line *l = line_get(h, line);

	if (!l)
		return HAL_FATAL;

	/* A backend whose region data carries no special information tone and
	 * no number-unobtainable tone refuses both. The stub refuses them in
	 * every region, so CI exercises the substitution line/line.c makes. */
	if (tone == HAL_TONE_SIT || tone == HAL_TONE_NUMBER_UNOBTAINABLE) {
		TRACE(TRACE_HAL, "stub: line %u has no profile for tone %u",
		      line, tone);
		return HAL_UNSUPPORTED;
	}

	l->tone = tone;
	l->tone_duration_ms = duration_ms;
	TRACE(TRACE_HAL, "stub: line %u tone=%u dur=%u", line, tone, duration_ms);

	return HAL_OK;
}

static enum hal_status stub_tone_stop(struct hal *h, unsigned int line)
{
	struct stub_line *l = line_get(h, line);

	if (!l)
		return HAL_FATAL;

	l->tone = HAL_TONE_NONE;
	l->tone_duration_ms = 0;
	TRACE(TRACE_HAL, "stub: line %u tone stop", line);

	return HAL_OK;
}

static enum hal_status stub_cw_tone_play(struct hal *h, unsigned int line,
					 unsigned int variant, uint32_t duration_ms)
{
	struct stub_line *l = line_get(h, line);

	if (!l)
		return HAL_FATAL;

	l->cw_variant = (uint8_t)variant;
	l->cw_variant_duration_ms = duration_ms;
	l->tone = HAL_TONE_CALL_WAITING;
	l->tone_duration_ms = duration_ms;
	TRACE(TRACE_HAL, "stub: line %u cw variant=%u dur=%u", line, variant,
	      duration_ms);

	return HAL_OK;
}

static enum hal_status stub_digit_play(struct hal *h, unsigned int line, char digit,
				       uint32_t duration_ms)
{
	struct stub_line *l = line_get(h, line);

	if (!l)
		return HAL_FATAL;

	TRACE(TRACE_HAL, "stub: line %u play digit '%c' dur=%u", line, digit,
	      duration_ms);

	return HAL_OK;
}

static enum hal_status stub_channel_config_set(struct hal *h, unsigned int line,
					       const struct hal_channel_cfg *cfg)
{
	struct stub_line *l = line_get(h, line);

	if (!l)
		return HAL_FATAL;

	l->chan = *cfg;
	TRACE(TRACE_HAL,
	      "stub: line %u detect=0x%x gain tx=%d rx=%d slic tx=%d rx=%d ec=%d "
	      "tone_latch=%d tone_force=%d",
	      line, cfg->detect_mask, cfg->gain_tx, cfg->gain_rx,
	      cfg->slic_gain_tx, cfg->slic_gain_rx, cfg->ec_enable,
	      cfg->ec_tone_latch, cfg->ec_tone_force_disable);

	return HAL_OK;
}

/*
 * What a configured stream leaves on its line. A configuration makes the line
 * canceller, and only for a channel that asks for one. That is what libudsp
 * does (udsp core/stream.c stream_lec_ensure). The ubus stream verb marks its
 * stream through this too, so both paths leave one state and one copy of the
 * rule.
 */
static void stub_stream_configured_mark(struct hal *h, unsigned int line,
					struct stub_stream *s)
{
	s->configured = true;
	if (h->line[line].chan.ec_enable)
		h->line[line].lec_made = true;
}

static enum hal_status stub_stream_config_set(struct hal *h, unsigned int line,
					      unsigned int stream,
					      const struct hal_media *media)
{
	struct stub_stream *s = stream_get(h, line, stream);

	/* hal.h line_loop_set: a line that carries a loop refuses a stream
	 * until the loop goes, and the loop refuses a line that carries a
	 * stream. The two exclude each other in both directions. */
	if (s && h->line[line].loop_mode != HAL_LOOP_OFF) {
		TRACE(TRACE_HAL, "stub: line %u strm %u refused: a loop test "
		      "holds the line", line, stream);
		return HAL_TRANSIENT;
	}

	if (!s)
		return HAL_FATAL;

	s->media = *media;
	/* The description carries the kind of every later programming (hal.h
	 * fax_profile_apply), so it replaces what the profile asked for. */
	s->fax_governs = false;
	stub_stream_configured_mark(h, line, s);
	TRACE(TRACE_HAL,
	      "stub: line %u strm %u codec=%u pt=%u ptime=%u dir=%u dtmf=%u remote port=%u",
	      line, stream, media->codec, media->codec_pt, media->ptime_ms,
	      media->direction, media->dtmf_mode, media->remote.port);

	return HAL_OK;
}

static enum hal_status stub_stream_start(struct hal *h, unsigned int line,
					 unsigned int stream)
{
	struct stub_stream *s = stream_get(h, line, stream);

	if (!s)
		return HAL_FATAL;

	if (!s->configured)
		return HAL_FATAL;

	s->running = true;
	TRACE(TRACE_HAL, "stub: line %u strm %u start", line, stream);

	return HAL_OK;
}

/* A line carries a media plane while any stream of it runs, which is the
 * condition the canceller and the jitter buffer both live under here. */
static bool stub_line_running(const struct stub_line *l)
{
	unsigned int i;

	for (i = 0; i < HAL_STREAM_MAX; i++) {
		if (l->stream[i].running)
			return true;
	}

	return false;
}

/*
 * A line is described while any stream of it holds a description. That is the
 * count libudsp releases the canceller against: stream_line_used takes every
 * stream that is not idle (udsp core/stream.c). A stream configured and never
 * started therefore keeps the instance alive.
 */
/* What the loop test waits for on a port (hal.h line_loop_set): the bell and
 * a running stream. A caller identity burst is the third owner the contract
 * names, and this backend keeps no session for one. */
static bool stub_line_busy(const struct stub_line *l)
{
	return l->ringing || stub_line_running(l);
}

static bool stub_line_described(const struct stub_line *l)
{
	unsigned int i;

	for (i = 0; i < HAL_STREAM_MAX; i++) {
		if (l->stream[i].configured)
			return true;
	}

	return false;
}

static enum hal_status stub_stream_stop(struct hal *h, unsigned int line,
					unsigned int stream)
{
	struct stub_stream *s = stream_get(h, line, stream);

	if (!s)
		return HAL_FATAL;

	s->running = false;
	/* The zeroize rule of hal.h: the key material a backend retains dies
	 * with the stream, so the suite goes back to NONE and the dump of a
	 * stopped stream carries no digest. */
	memset(&s->media.srtp, 0, sizeof(s->media.srtp));
	/* hal.h stream_stop: a stopped stream must be described again before it
	 * can start again. The rest of the description stays, so the dump of a
	 * stopped stream still says what the call ran. */
	s->configured = false;
	/* The profile dies with the leg that took it, as it does in libudsp
	 * (udsp core/stream.c stream_teardown clears the fax record). */
	s->fax_governs = false;
	/* The last description of the line takes the canceller with it, the
	 * release rule of libudsp. A channel that asks for no canceller does
	 * not, so a switch that lands mid-call leaves the instance alone. */
	if (!stub_line_described(&h->line[line]))
		h->line[line].lec_made = false;
	TRACE(TRACE_HAL, "stub: line %u strm %u stop", line, stream);

	return HAL_OK;
}

static enum hal_status stub_stream_dtmf_send(struct hal *h, unsigned int line,
					     unsigned int stream, char digit,
					     uint32_t duration_ms)
{
	struct stub_stream *s = stream_get(h, line, stream);

	if (!s)
		return HAL_FATAL;

	TRACE(TRACE_HAL, "stub: line %u strm %u relay digit '%c' dur=%u", line,
	      stream, digit, duration_ms);

	return HAL_OK;
}

/*
 * What a stream reports when nothing overrode it. Every number is non-zero on
 * purpose: a call record that carries a zero where the HAL reported one is a
 * different fault from a record that was never filled, and a canned zero would
 * hide the second.
 */
static void stats_canned(struct hal_media_stats *out)
{
	memset(out, 0, sizeof(*out));
	out->packets_sent = 1500;
	out->packets_recv = 1498;
	out->packets_lost = 2;
	out->packets_discarded = 1;
	out->loss_rate = 1;
	out->loss_rate_far = 2;
	out->jitter_avg = 3;
	out->jitter_max = 11;
	out->jitter_far = 5;
	out->rtt_avg = 24;
	out->rtt_max = 48;
	out->bytes_sent = 240000;
	out->bytes_recv = 239680;
	out->mos_lq = 4.3f;
	out->mos_cq = 4.1f;
	out->rfactor = 88;
}

static enum hal_status stub_stream_stats_result_get(struct hal *h, unsigned int line,
						    unsigned int stream,
						    struct hal_media_stats *out)
{
	struct stub_stream *s = stream_get(h, line, stream);

	if (!s)
		return HAL_FATAL;

	if (s->stats_set)
		*out = s->stats;
	else
		stats_canned(out);

	return HAL_OK;
}

/*
 * The stub carries no jitter buffer, so the counters below are canned, and
 * only a running stream answers them. That gate is not a test knob: it is what
 * a real backend does, because a stream that is not carrying audio has no
 * counters for the call it is between (udsp_stream_jb_stats refuses a stream
 * that has not ticked since its last configuration). It is also what gives CI
 * the stopped row of "voice jb": bring a stream up, poll it, stop it, and
 * "voice jb" must still carry its final numbers.
 *
 * A stream that never ran answers HAL_UNSUPPORTED and is skipped, which is the
 * answer a backend with no jitter buffer at all gives for every stream it has.
 *
 * The values are distinct per field on purpose, the way the CDR case picks
 * them: a pair that swapped anywhere between here and the ubus row would
 * otherwise read as correct.
 */
static enum hal_status stub_stream_jb_stats_get(struct hal *h, unsigned int line,
						unsigned int stream,
						struct hal_jb_stats *out)
{
	struct stub_stream *s = stream_get(h, line, stream);

	if (!s || !out)
		return HAL_UNSUPPORTED;

	if (!s->running)
		return HAL_UNSUPPORTED;

	memset(out, 0, sizeof(*out));
	out->frames_out = 4800;
	out->plc = 12;
	out->cn = 6;
	out->silence = 3;
	out->late_drop = 5;
	out->dup_drop = 2;
	out->overflow_drop = 1;
	/* 8 would collide with jitter_ms and hide a swapped pair in the one
	 * surface without a unit test, the media trace line. */
	out->hold_drop = 11;
	out->resync_discard_ms = 13;
	out->resyncs = 1;
	out->starve_resyncs = 0;
	out->cn_reanchors = 0;
	out->underruns = 4;
	out->skew_drops = 7;
	out->skew_inserts = 9;
	out->target_ms = 50;
	out->depth_ms = 46;
	out->jitter_ms = 8;
	out->pool_free = 57;
	/* The applied description, so a mode a caller programmed reads back
	 * here rather than a constant of this backend. */
	out->mode = s->media.jb_mode;
	/*
	 * The answer depends on the pair (the fake_stats_result precedent of
	 * the line suite). Every stream of this backend carries the same canned
	 * shape, so a row that had picked up another stream's numbers would
	 * read as correct; the offset makes that fill visible instead.
	 */
	out->frames_out += line * 100u + stream * 10u;

	return HAL_OK;
}

/*
 * The kind the streams of a line carry, on the rule the engine applies. The
 * modem kind wins over the fax kind, because it takes the canceller off the
 * frame whole. That is the safe answer for a carrier of either kind (udsp
 * core/stream.c stream_line_passthrough). A T.38 stream reads the modem kind,
 * the canceller policy of an image session (hal.h hal_media passthrough). A
 * fax passthrough profile reads it too, because that profile arms the modem
 * kind (hal.h fax_profile_apply).
 *
 * A stream on local hold governs nothing. It puts nothing of its own on the
 * wire, so its carrier is off the line. The other call of the line needs its
 * canceller back (udsp core/stream.c stream_governs).
 */
static enum hal_passthrough stub_line_kind_get(const struct stub_line *l)
{
	enum hal_passthrough kind = HAL_PASSTHROUGH_OFF;
	unsigned int i;

	for (i = 0; i < HAL_STREAM_MAX; i++) {
		const struct stub_stream *s = &l->stream[i];

		if (!s->running || s->media.tx_silence)
			continue;
		if (s->fax_governs ||
		    s->media.passthrough == HAL_PASSTHROUGH_MODEM ||
		    s->media.codec == HAL_CODEC_T38)
			return HAL_PASSTHROUGH_MODEM;
		if (s->media.passthrough == HAL_PASSTHROUGH_FAX)
			kind = HAL_PASSTHROUGH_FAX;
	}

	return kind;
}

/*
 * The stub carries no echo canceller, so the state below is canned, and the
 * ladder above it is the contract's, not a test knob. hal.h leaves a live
 * backend one refusal: a line that holds no canceller instance. Two conditions
 * carry it here:
 *
 *   the line made no instance      stub_line.lec_made is clear
 *   the line runs no media plane   no stream of the line runs
 *
 * The running gate is the one stub_stream_jb_stats_get uses, so the two
 * diagnostic surfaces of this backend come up and go away together.
 *
 * A line with an instance answers a kind and three flags. The canned counters
 * below stand in every row. The kind comes from the streams of the line. That
 * is the pair the engine publishes: the tick's kind beside a measured run
 * (udsp core/chan.c chan_lec_publish).
 *
 * The modem kind bypasses the filter, so its row reads ec_active 0 and no
 * processor. Three states reach it:
 *
 *   a running stream a fax passthrough profile governs
 *   a running stream that carries the modem kind
 *   a running stream that carries T.38
 *
 * The fax kind keeps the filter and takes the processor off, so that row reads
 * ec_active 1, no processor and the fax kind. Every other line reads the voice
 * row: the filter ran, the processor is on, and it acted on the frame.
 *
 * Two channel switches clear flags without moving the kind. A channel that
 * asks for no canceller clears all three: the filter ran nothing. A channel
 * that asks for no processor clears the two processor flags alone (hal.h
 * hal_lec_stats, the engine's chan_nlp_want). What the line carries is what
 * its streams say it carries either way.
 *
 * What the "voice status" lec array shows for a refused line is a northbound
 * choice (Last call from the cache, else an Idle zero row); this op only
 * decides whether it answers.
 *
 * The values are distinct per field on purpose, the way the jitter buffer case
 * picks them: a pair that swapped anywhere between here and the ubus row would
 * otherwise read as correct. tail_ms and taps agree with each other at 8 kHz,
 * because a row whose tap count did not follow its tail would be a fault of
 * this record and not of the surface. The six processor verdicts sum to 149
 * frames under frames on line 0, so a row keeps a processor-off remainder,
 * and the five that exclude the quiet frames sum to 5832, the denominator of
 * the duty the CLI derives. The peak tap is one period of guard plus 4
 * samples at 8 kHz. loop_open stays 0, because this backend models no hook
 * and the CLI suite reads rows on lines it never seized.
 */
static enum hal_status stub_line_lec_stats_get(struct hal *h, unsigned int line,
					       struct hal_lec_stats *out)
{
	struct stub_line *l = line_get(h, line);
	enum hal_passthrough kind;

	if (!l || !out)
		return HAL_UNSUPPORTED;

	if (!l->lec_made || !stub_line_running(l))
		return HAL_UNSUPPORTED;

	kind = stub_line_kind_get(l);

	memset(out, 0, sizeof(*out));
	out->tail_ms = 48;
	out->taps = 384;
	out->frames = 6000;
	out->hold_frames = 37;
	out->skip_frames = 11;
	out->retimes = 5;
	out->nlp_active_frames = 4944;
	out->nlp_wait_frames = 91;
	out->nlp_erle_frames = 310;
	out->nlp_unlike_frames = 470;
	out->nlp_dt_frames = 17;
	out->nlp_quiet_frames = 19;
	out->coef_peak_tap = 84;
	out->diverge_resets = 2;
	out->probes = 3;
	out->erle_mdb = 18500;
	out->coef_energy_micro = 62500;
	out->echo_gain_micro = 41000;
	out->double_talk = 1;
	/* A distinct value per field cannot separate flags that only carry 0
	 * and 1, so the kind and the two channel switches pick all three. A row
	 * with the processor off and acting would describe a state no canceller
	 * reaches. A filter that did not run carries no processor at all. */
	out->ec_active = kind != HAL_PASSTHROUGH_MODEM && l->chan.ec_enable;
	out->nlp_enabled = out->ec_active && kind == HAL_PASSTHROUGH_OFF &&
			   l->chan.ec_nlp_enable;
	out->nlp_active = out->nlp_enabled;
	out->passthrough = kind;
	/* 30 ms of loop at 8 kHz, and a measurement that stands: the shape a
	 * healthy line reports. */
	out->ref_delay_samples = 240;
	out->ref_ready = 1;
	/*
	 * The answer depends on the line (the stub_stream_jb_stats_get
	 * precedent). Every line of this backend carries the same canned
	 * shape, so a row that had picked up another line's numbers would read
	 * as correct; the offset makes that fill visible instead.
	 */
	out->frames += line * 100u;

	return HAL_OK;
}

/*
 * The caller's own release of the tone-disabler latch (hal.h
 * line_lec_latch_clear). This backend runs no disabler, so it counts the
 * request and answers HAL_OK for every line it carries. The count is the whole
 * check: the dump carries it, and a flow that expects a return to voice reads
 * it back.
 */
static enum hal_status stub_line_lec_latch_clear(struct hal *h,
						 unsigned int line)
{
	struct stub_line *l = line_get(h, line);

	if (!l)
		return HAL_UNSUPPORTED;

	l->latch_clears++;

	return HAL_OK;
}

/*
 * The media counters are answered, from the same record the slow query would
 * fill, so the CLI suite and the host smoke test keep a media surface with no
 * job in flight. The canned fallback is the one stub_stream_stats_result_get
 * uses; a suite that set numbers reads its own back through both ops.
 *
 * Only a running stream answers, the stub_stream_jb_stats_get gate: the fast
 * op reports what plays now, and an idle stream has nothing playing. The slow
 * path keeps answering after a stop, because stream_stats_result_get holds
 * the snapshot the last slow query took, which outlives the stream.
 */
static enum hal_status stub_stream_stats_get(struct hal *h, unsigned int line,
					     unsigned int stream,
					     struct hal_media_stats *out)
{
	struct stub_stream *s = stream_get(h, line, stream);

	if (!s || !s->running)
		return HAL_UNSUPPORTED;

	return stub_stream_stats_result_get(h, line, stream, out);
}

static enum hal_status stub_debug_set(struct hal *h, uint32_t flags)
{
	return HAL_UNSUPPORTED;
}

static enum hal_status stub_fax_profile_apply(struct hal *h, unsigned int line,
					      unsigned int stream,
					      const struct hal_fax_profile *fax)
{
	struct stub_stream *s = stream_get(h, line, stream);

	if (!s)
		return HAL_FATAL;

	/* The passthrough profile arms the modem kind on the stream it reached,
	 * and every other mode takes that arm away (hal.h fax_profile_apply).
	 * It belongs to the stream, because the description of the next
	 * programming is what replaces it. */
	s->fax_governs = fax->mode == HAL_FAX_PASSTHROUGH;
	/* The whole profile, not only the mode. A suite or the e2e has no
	 * other way to read what the core asked the DSP for, and the fields
	 * that decide whether a page survives are the four disable flags and
	 * the jitter buffer mode, not the mode byte. */
	s->fax = *fax;
	s->fax_set = true;
	TRACE(TRACE_HAL,
	      "stub: line %u strm %u fax mode=%u codec=%u pt=%u ec_off=%d vad_off=%d "
	      "cng_off=%d silence_off=%d jb_fixed=%d",
	      line, stream, fax->mode, fax->codec, fax->codec_pt, fax->ec_disable,
	      fax->vad_disable, fax->cng_disable, fax->silence_comp_disable,
	      fax->jb_fixed);

	return HAL_OK;
}

static enum hal_status stub_conf_set(struct hal *h, unsigned int line,
				     enum hal_conf_mode mode)
{
	struct stub_line *l = line_get(h, line);

	if (!l)
		return HAL_FATAL;

	if (!(h->cap.features & HAL_FEAT_CONF))
		return HAL_UNSUPPORTED;

	l->conf = mode;
	TRACE(TRACE_HAL, "stub: line %u conf mode=%u", line, mode);

	return HAL_OK;
}

/*
 * The manufacturing loop. This backend renders no audio, so the op records
 * what the core asked for and the dump is where a suite reads it back. The
 * argument refusals are the ones hal.h states, in that order. The busy
 * refusals belong to a backend that runs a media plane of its own, and this
 * one has none.
 */
static enum hal_status stub_line_loop_set(struct hal *h, unsigned int line,
					  const struct hal_loop *loop)
{
	struct stub_line *l = line_get(h, line);

	if (!l || !loop || loop->mode > HAL_LOOP_PEER)
		return HAL_UNSUPPORTED;

	if (!(h->cap.features & HAL_FEAT_LOOP))
		return HAL_UNSUPPORTED;

	if (loop->mode != HAL_LOOP_OFF &&
	    loop->delay_ms > STUB_LOOP_DELAY_MAX_MS)
		return HAL_UNSUPPORTED;

	if (loop->mode == HAL_LOOP_PEER &&
	    (loop->peer >= h->cap.lines || loop->peer == line))
		return HAL_UNSUPPORTED;

	/* hal.h: an arm waits for a port that rings or carries a stream, and
	 * under PEER for the other port too. OFF takes no busy refusal at
	 * all. */
	if (loop->mode != HAL_LOOP_OFF && stub_line_busy(l)) {
		TRACE(TRACE_HAL, "stub: line %u loop refused: the line is busy",
		      line);
		return HAL_TRANSIENT;
	}

	if (loop->mode == HAL_LOOP_PEER &&
	    stub_line_busy(&h->line[loop->peer])) {
		TRACE(TRACE_HAL, "stub: line %u loop refused: peer %u is busy",
		      line, loop->peer);
		return HAL_TRANSIENT;
	}

	l->loop_mode = loop->mode;
	l->loop_peer = loop->mode == HAL_LOOP_PEER ? loop->peer : 0;
	l->loop_delay_ms = loop->mode == HAL_LOOP_OFF ? 0 : loop->delay_ms;
	TRACE(TRACE_HAL, "stub: line %u loop mode=%u peer=%u delay=%u", line,
	      l->loop_mode, l->loop_peer, l->loop_delay_ms);

	return HAL_OK;
}

/* The two levers that hold their completion back also hold the dsp stop open,
 * so the teardown of a SIGTERM has a nested wait running when the bring-up
 * completion arrives. */
static bool dsp_start_late(const char *mode)
{
	if (!mode)
		return false;

	return !strcmp(mode, "fatal_late") || !strcmp(mode, "transient_late");
}

/*
 * The bring-up outcome is a lever. UVOICE_STUB_DSP_START=fatal completes the
 * job HAL_FATAL, which is the exit the daemon owes procd; fatal_sync refuses
 * the submit with the same status instead of scheduling a job, which is the
 * bring-up a backend turns down on the spot; fatal_late and transient_late
 * complete HAL_FATAL and HAL_TRANSIENT two seconds later, inside the stop;
 * transient_once completes the first call HAL_TRANSIENT and every later one
 * HAL_OK, which is the single retry in front of the exit; every other value,
 * the absent variable included, completes ok. The variable is read at each
 * call, so the transient shape needs no state beyond the call count.
 */
static enum hal_status stub_dsp_start(struct hal *h, struct hal_job *job)
{
	static unsigned int dsp_start_calls;
	const char *mode = getenv("UVOICE_STUB_DSP_START");
	unsigned int delay_ms = STUB_JOB_DELAY_MS;
	enum hal_status st = HAL_OK;

	if (mode && !strcmp(mode, "fatal_sync")) {
		TRACE(TRACE_HAL, "stub: dsp start refused: fatal");
		return HAL_FATAL;
	}

	if (mode && !strcmp(mode, "fatal")) {
		st = HAL_FATAL;
	} else if (mode && !strcmp(mode, "fatal_late")) {
		st = HAL_FATAL;
	} else if (mode && !strcmp(mode, "transient_late")) {
		st = HAL_TRANSIENT;
	} else if (mode && !strcmp(mode, "transient_once") && dsp_start_calls == 0) {
		st = HAL_TRANSIENT;
	}

	if (dsp_start_late(mode))
		delay_ms = STUB_JOB_LATE_DELAY_MS;

	dsp_start_calls++;
	TRACE(TRACE_HAL, "stub: dsp start %s in %u ms", hal_status_name(st), delay_ms);

	return job_schedule_after(h, job, st, delay_ms);
}

static enum hal_status stub_dsp_stop(struct hal *h, struct hal_job *job)
{
	const char *mode = getenv("UVOICE_STUB_DSP_START");
	unsigned int delay_ms = STUB_JOB_DELAY_MS;

	if (dsp_start_late(mode))
		delay_ms = STUB_JOB_STOP_HOLD_MS;

	TRACE(TRACE_HAL, "stub: dsp stop");

	return job_schedule_after(h, job, HAL_OK, delay_ms);
}

static enum hal_status stub_line_test_start(struct hal *h, unsigned int line,
					    enum hal_line_test test,
					    struct hal_job *job)
{
	struct stub_line *l = line_get(h, line);

	if (!l)
		return HAL_FATAL;

	/* the other direction of the same exclusion: the metallic test would
	 * take the part away from a loop that is playing */
	if (l->loop_mode != HAL_LOOP_OFF) {
		TRACE(TRACE_HAL, "stub: line %u line test refused: a loop test "
		      "holds the line", line);
		return HAL_TRANSIENT;
	}

	snprintf(l->test_result, sizeof(l->test_result), "test=%u result=pass", test);
	TRACE(TRACE_HAL, "stub: line %u line test %u", line, test);

	return job_schedule(h, job, HAL_OK);
}

static enum hal_status stub_stream_stats_start(struct hal *h, unsigned int line,
					       unsigned int stream,
					       struct hal_job *job)
{
	struct stub_stream *s = stream_get(h, line, stream);

	if (!s)
		return HAL_FATAL;

	TRACE(TRACE_HAL, "stub: line %u strm %u stats query", line, stream);

	return job_schedule(h, job, HAL_OK);
}

static enum hal_status stub_region_apply(struct hal *h, const char *iso3,
					 struct hal_job *job)
{
	str_copy(h->region, sizeof(h->region), iso3);
	TRACE(TRACE_HAL, "stub: region %s", h->region);

	return job_schedule(h, job, HAL_OK);
}

/*
 * The stub answers the shipped USA shape by default, so the CLI and e2e
 * suites run the read-back path the udsp backend runs and the core's
 * compiled default is never the live path there. UVOICE_STUB_REGION_GET=none
 * refuses, the shape of a backend with no read-back, for a case that wants
 * the compiled default; long_sas answers the USA shape with a 70 s
 * call-waiting alert, the bound the extended CW offer needs a case for.
 * UVOICE_STUB_FLASH_SERVICE=0|1|2 names the flash service the shape carries,
 * drop when unset. UVOICE_STUB_PULSE_INVERT=1 answers the inverted decadic
 * coding of New Zealand, and any other value or none the direct coding. A
 * pulse that the hook verb injects carries the same coding, so the read-back
 * and the event agree as they do on libudsp.
 * UVOICE_STUB_CONFIRM_TOTAL_MS=<ms> answers that confirm tone length, and
 * UVOICE_STUB_CONFIRM_ENDLESS=1 a confirm tone that repeats until stopped.
 * Both read 0 when unset, unlike the 600 ms of USA, so the suites keep the
 * immediate dial tone after a star code. test_line.c links its own
 * hal_backend_ops, so the unit coverage of the read-back lives in its fake,
 * not here.
 */
static uint8_t stub_flash_service(void)
{
	const char *mode = getenv("UVOICE_STUB_FLASH_SERVICE");

	if (mode && mode[0] >= '0' && mode[0] <= '2' && !mode[1])
		return (uint8_t)(mode[0] - '0');

	return HAL_FLASH_SERVICE_DROP;
}

static uint8_t stub_env_flag(const char *name)
{
	const char *mode = getenv(name);

	return mode && !strcmp(mode, "1");
}

static uint8_t stub_pulse_invert(void)
{
	return stub_env_flag("UVOICE_STUB_PULSE_INVERT");
}

static uint32_t stub_confirm_total_ms(void)
{
	const char *ms = getenv("UVOICE_STUB_CONFIRM_TOTAL_MS");

	return ms ? (uint32_t)strtoul(ms, NULL, 10) : 0;
}

static enum hal_status stub_region_get(struct hal *h, struct hal_region *out)
{
	const char *mode = getenv("UVOICE_STUB_REGION_GET");
	bool long_sas = mode && !strcmp(mode, "long_sas");
	static const struct hal_region_ring rings[HAL_REGION_RING_MAX] = {
		{ .cadence = { { 2000, 4000 } }, .cadence_count = 1 },
		{ .cadence = { { 2000, 4000 } }, .cadence_count = 1 },
		{ .cadence = { { 800, 400 }, { 800, 4000 } },
		  .cadence_count = 2 },
		{ .cadence = { { 400, 200 }, { 400, 200 }, { 800, 4000 } },
		  .cadence_count = 3 },
		{ .cadence = { { 300, 200 }, { 1000, 200 }, { 300, 4000 } },
		  .cadence_count = 3 },
		{ .cadence = { { 2000, 4000 } }, .cadence_count = 1 },
		{ .cadence = { { 2000, 4000 } }, .cadence_count = 1 },
		{ .cadence = { { 2000, 4000 } }, .cadence_count = 1 },
	};

	if (!out || (mode && !strcmp(mode, "none")))
		return HAL_UNSUPPORTED;

	memset(out, 0, sizeof(*out));
	str_copy(out->iso3, sizeof(out->iso3), h->region);
	out->cid_format = HAL_CID_BELLCORE_FSK;
	out->mwi_format = HAL_CID_BELLCORE_FSK;
	out->hook_flash_min_ms = 300;
	out->hook_flash_max_ms = 1300;
	out->hook_release_min_ms = 1301;
	out->cw_sas_total_ms = long_sas ? 70000 : 20600;
	memcpy(out->ring, rings, sizeof(rings));
	out->flash_service = stub_flash_service();
	out->pulse_invert = stub_pulse_invert();
	out->confirm_total_ms = stub_confirm_total_ms();
	out->confirm_endless = stub_env_flag("UVOICE_STUB_CONFIRM_ENDLESS");
	snprintf(out->describe, sizeof(out->describe), "Stub USA");
	/* Four variants with distinct lead-ins: index 0 is the plain tone at
	 * 300 ms (so variant 0 keeps the 350 ms deferral), and c3 at 500 ms
	 * gives the deferral test a value apart from the fallback. */
	out->cw_count = 4;
	out->cw_lead_in_ms[0] = 300;
	out->cw_lead_in_ms[1] = 300;
	out->cw_lead_in_ms[2] = 300;
	out->cw_lead_in_ms[3] = 500;
	out->cw_lead_in_ms[4] = 700;

	return HAL_OK;
}

const struct hal_ops hal_backend_ops = {
	.name = "stub",
	.open = stub_open,
	.close = stub_close,
	.cap_get = stub_cap_get,
	.counters_get = stub_counters_get,
	.line_state_set = stub_line_state_set,
	.line_polarity_set = stub_line_polarity_set,
	.line_hook_query = stub_line_hook_query,
	.line_hook_timing_set = stub_line_hook_timing_set,
	.line_ring_start = stub_line_ring_start,
	.line_ring_stop = stub_line_ring_stop,
	.line_cid_send = stub_line_cid_send,
	.line_mwi_set = stub_line_mwi_set,
	.line_test_result_get = stub_line_test_result_get,
	.tone_play = stub_tone_play,
	.tone_stop = stub_tone_stop,
	.cw_tone_play = stub_cw_tone_play,
	.digit_play = stub_digit_play,
	.channel_config_set = stub_channel_config_set,
	.stream_config_set = stub_stream_config_set,
	.stream_start = stub_stream_start,
	.stream_stop = stub_stream_stop,
	.stream_dtmf_send = stub_stream_dtmf_send,
	.stream_stats_result_get = stub_stream_stats_result_get,
	.fax_profile_apply = stub_fax_profile_apply,
	.conf_set = stub_conf_set,
	.line_loop_set = stub_line_loop_set,
	.stream_jb_stats_get = stub_stream_jb_stats_get,
	.stream_stats_get = stub_stream_stats_get,
	.line_lec_stats_get = stub_line_lec_stats_get,
	.line_lec_latch_clear = stub_line_lec_latch_clear,
	.region_get = stub_region_get,
	.debug_set = stub_debug_set,
	.dsp_start = stub_dsp_start,
	.dsp_stop = stub_dsp_stop,
	.line_test_start = stub_line_test_start,
	.stream_stats_start = stub_stream_stats_start,
	.region_apply = stub_region_apply,
};

enum {
	STUB_HOOK_LINE,
	STUB_HOOK_STATE,
	STUB_HOOK_PULSES,
	__STUB_HOOK_MAX,
};

static const struct blobmsg_policy stub_hook_policy[__STUB_HOOK_MAX] = {
	[STUB_HOOK_LINE] = { "line", BLOBMSG_TYPE_INT32 },
	[STUB_HOOK_STATE] = { "state", BLOBMSG_TYPE_STRING },
	[STUB_HOOK_PULSES] = { "pulses", BLOBMSG_TYPE_INT32 },
};

enum {
	STUB_DIGIT_LINE,
	STUB_DIGIT_DIGIT,
	STUB_DIGIT_DURATION,
	STUB_DIGIT_REMOTE,
	__STUB_DIGIT_MAX,
};

static const struct blobmsg_policy stub_digit_policy[__STUB_DIGIT_MAX] = {
	[STUB_DIGIT_LINE] = { "line", BLOBMSG_TYPE_INT32 },
	[STUB_DIGIT_DIGIT] = { "digit", BLOBMSG_TYPE_STRING },
	[STUB_DIGIT_DURATION] = { "duration", BLOBMSG_TYPE_INT32 },
	[STUB_DIGIT_REMOTE] = { "remote", BLOBMSG_TYPE_BOOL },
};

/*
 * The fax and modem tone injection. A hardware detector reports these with a
 * direction, so the knob carries both: the direction is what the trigger gate
 * reads, and a stub that could only inject the local one could not prove the
 * gate drops a tone from the wire.
 *
 * end selects the edge (hal.h u.tone.end). It defaults to the start row, so a
 * caller that names neither end nor duration injects a start row with a
 * duration of 0.
 */
enum {
	STUB_TONE_LINE,
	STUB_TONE_CODE,
	STUB_TONE_DIRECTION,
	STUB_TONE_END,
	STUB_TONE_DURATION,
	__STUB_TONE_MAX,
};

static const struct blobmsg_policy stub_tone_policy[__STUB_TONE_MAX] = {
	[STUB_TONE_LINE] = { "line", BLOBMSG_TYPE_INT32 },
	[STUB_TONE_CODE] = { "code", BLOBMSG_TYPE_STRING },
	[STUB_TONE_DIRECTION] = { "direction", BLOBMSG_TYPE_STRING },
	[STUB_TONE_END] = { "end", BLOBMSG_TYPE_BOOL },
	[STUB_TONE_DURATION] = { "duration", BLOBMSG_TYPE_INT32 },
};

enum {
	STUB_FLASH_LINE,
	__STUB_FLASH_MAX,
};

static const struct blobmsg_policy stub_flash_policy[__STUB_FLASH_MAX] = {
	[STUB_FLASH_LINE] = { "line", BLOBMSG_TYPE_INT32 },
};

enum {
	STUB_RING_LINE,
	STUB_RING_DURATION,
	__STUB_RING_MAX,
};

static const struct blobmsg_policy stub_ring_policy[__STUB_RING_MAX] = {
	[STUB_RING_LINE] = { "line", BLOBMSG_TYPE_INT32 },
	[STUB_RING_DURATION] = { "duration", BLOBMSG_TYPE_INT32 },
};

/*
 * The call record check. Without it a suite that reads a media record can
 * only compare it against the canned numbers above, which proves nothing
 * about the path between the HAL and the record: both sides would agree by
 * sharing one constant. This knob puts chosen numbers on one (line, stream),
 * so the record has to carry what the backend reported.
 *
 * An absent argument keeps the canned value, so a caller names only the fields
 * it cares about. The MOS strings use the same two-decimal form as the
 * northbound record, so replay preserves that form through the HAL float.
 */
enum {
	STUB_STATS_LINE,
	STUB_STATS_STREAM,
	STUB_STATS_PACKETS_SENT,
	STUB_STATS_PACKETS_RECV,
	STUB_STATS_PACKETS_LOST,
	STUB_STATS_PACKETS_DISCARDED,
	STUB_STATS_LOSS_RATE,
	STUB_STATS_LOSS_RATE_FAR,
	STUB_STATS_JITTER_AVG,
	STUB_STATS_JITTER_MAX,
	STUB_STATS_JITTER_FAR,
	STUB_STATS_RTT_AVG,
	STUB_STATS_RTT_MAX,
	STUB_STATS_BYTES_SENT,
	STUB_STATS_BYTES_RECV,
	STUB_STATS_MOS_LQ,
	STUB_STATS_MOS_CQ,
	STUB_STATS_RFACTOR,
	__STUB_STATS_MAX,
};

/* UNSPEC and not INT64: the ubus client turns a small JSON number into an
 * INT32 attribute, and a policy that named one width would drop the other */
static const struct blobmsg_policy stub_stats_policy[__STUB_STATS_MAX] = {
	[STUB_STATS_LINE] = { "line", BLOBMSG_TYPE_INT32 },
	[STUB_STATS_STREAM] = { "stream", BLOBMSG_TYPE_INT32 },
	[STUB_STATS_PACKETS_SENT] = { "packets_sent", BLOBMSG_TYPE_UNSPEC },
	[STUB_STATS_PACKETS_RECV] = { "packets_recv", BLOBMSG_TYPE_UNSPEC },
	[STUB_STATS_PACKETS_LOST] = { "packets_lost", BLOBMSG_TYPE_UNSPEC },
	[STUB_STATS_PACKETS_DISCARDED] = { "packets_discarded", BLOBMSG_TYPE_UNSPEC },
	[STUB_STATS_LOSS_RATE] = { "loss_rate", BLOBMSG_TYPE_UNSPEC },
	[STUB_STATS_LOSS_RATE_FAR] = { "loss_rate_far", BLOBMSG_TYPE_UNSPEC },
	[STUB_STATS_JITTER_AVG] = { "jitter_avg", BLOBMSG_TYPE_UNSPEC },
	[STUB_STATS_JITTER_MAX] = { "jitter_max", BLOBMSG_TYPE_UNSPEC },
	[STUB_STATS_JITTER_FAR] = { "jitter_far", BLOBMSG_TYPE_UNSPEC },
	[STUB_STATS_RTT_AVG] = { "rtt_avg", BLOBMSG_TYPE_UNSPEC },
	[STUB_STATS_RTT_MAX] = { "rtt_max", BLOBMSG_TYPE_UNSPEC },
	[STUB_STATS_BYTES_SENT] = { "bytes_sent", BLOBMSG_TYPE_UNSPEC },
	[STUB_STATS_BYTES_RECV] = { "bytes_recv", BLOBMSG_TYPE_UNSPEC },
	[STUB_STATS_MOS_LQ] = { "mos_lq", BLOBMSG_TYPE_STRING },
	[STUB_STATS_MOS_CQ] = { "mos_cq", BLOBMSG_TYPE_STRING },
	[STUB_STATS_RFACTOR] = { "rfactor", BLOBMSG_TYPE_UNSPEC },
};

static const struct {
	unsigned int arg;
	size_t off;
} stub_stats_u64[] = {
	{ STUB_STATS_PACKETS_SENT, offsetof(struct hal_media_stats, packets_sent) },
	{ STUB_STATS_PACKETS_RECV, offsetof(struct hal_media_stats, packets_recv) },
	{ STUB_STATS_PACKETS_LOST, offsetof(struct hal_media_stats, packets_lost) },
	{ STUB_STATS_PACKETS_DISCARDED,
	  offsetof(struct hal_media_stats, packets_discarded) },
	{ STUB_STATS_LOSS_RATE, offsetof(struct hal_media_stats, loss_rate) },
	{ STUB_STATS_LOSS_RATE_FAR, offsetof(struct hal_media_stats, loss_rate_far) },
	{ STUB_STATS_JITTER_AVG, offsetof(struct hal_media_stats, jitter_avg) },
	{ STUB_STATS_JITTER_MAX, offsetof(struct hal_media_stats, jitter_max) },
	{ STUB_STATS_JITTER_FAR, offsetof(struct hal_media_stats, jitter_far) },
	{ STUB_STATS_RTT_AVG, offsetof(struct hal_media_stats, rtt_avg) },
	{ STUB_STATS_RTT_MAX, offsetof(struct hal_media_stats, rtt_max) },
	{ STUB_STATS_BYTES_SENT, offsetof(struct hal_media_stats, bytes_sent) },
	{ STUB_STATS_BYTES_RECV, offsetof(struct hal_media_stats, bytes_recv) },
};

static bool stub_mos_parse(struct blob_attr *attr, float *mos)
{
	const char *text = blobmsg_get_string(attr);
	char *end;
	float value = strtof(text, &end);

	if (end == text || *end)
		return false;

	*mos = value;
	return true;
}

static int stub_ubus_media_stats(struct ubus_context *ctx, struct ubus_object *obj,
				 struct ubus_request_data *req, const char *method,
				 struct blob_attr *msg)
{
	struct blob_attr *tb[__STUB_STATS_MAX] = {};
	unsigned int line, stream, i;
	float mos_lq, mos_cq;
	struct stub_stream *s;

	if (msg)
		blobmsg_parse(stub_stats_policy, __STUB_STATS_MAX, tb, blob_data(msg),
			      blob_len(msg));

	if (!stub_dev)
		return UBUS_STATUS_NOT_FOUND;

	line = tb[STUB_STATS_LINE] ? blobmsg_get_u32(tb[STUB_STATS_LINE]) : 0;
	stream = tb[STUB_STATS_STREAM] ? blobmsg_get_u32(tb[STUB_STATS_STREAM]) : 0;
	s = stream_get(stub_dev, line, stream);
	if (!s)
		return UBUS_STATUS_INVALID_ARGUMENT;
	if (tb[STUB_STATS_MOS_LQ] && !stub_mos_parse(tb[STUB_STATS_MOS_LQ], &mos_lq))
		return UBUS_STATUS_INVALID_ARGUMENT;
	if (tb[STUB_STATS_MOS_CQ] && !stub_mos_parse(tb[STUB_STATS_MOS_CQ], &mos_cq))
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (!s->stats_set) {
		stats_canned(&s->stats);
		s->stats_set = true;
	}

	for (i = 0; i < ARRAY_SIZE(stub_stats_u64); i++) {
		struct blob_attr *a = tb[stub_stats_u64[i].arg];

		if (a)
			*(uint64_t *)((char *)&s->stats + stub_stats_u64[i].off) =
				blobmsg_cast_u64(a);
	}

	if (tb[STUB_STATS_MOS_LQ])
		s->stats.mos_lq = mos_lq;
	if (tb[STUB_STATS_MOS_CQ])
		s->stats.mos_cq = mos_cq;
	if (tb[STUB_STATS_RFACTOR])
		s->stats.rfactor = (uint32_t)blobmsg_cast_u64(tb[STUB_STATS_RFACTOR]);

	TRACE(TRACE_HAL, "stub: line %u strm %u media stats overridden", line, stream);

	return 0;
}

static int hook_state_parse(const char *name, uint8_t *state)
{
	static const struct {
		const char *name;
		uint8_t state;
	} map[] = {
		{ "seize", HAL_HOOK_SEIZE },
		{ "offhook", HAL_HOOK_SEIZE },
		{ "release", HAL_HOOK_RELEASE },
		{ "onhook", HAL_HOOK_RELEASE },
		{ "flash", HAL_HOOK_FLASH },
		{ "pulse", HAL_HOOK_PULSE },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(map); i++) {
		if (strcmp(name, map[i].name))
			continue;

		*state = map[i].state;
		return 0;
	}

	return -1;
}

/* The stub event verbs that name a line share this lookup: hook, digit, tone
 * and flash. The ring verbs do not; they answer a missing device with
 * NOT_FOUND, not INVALID_ARGUMENT. */
static bool stub_line_arg(struct blob_attr **tb, int line_idx,
			  unsigned int *line, struct stub_line **l)
{
	struct stub_line *found;
	unsigned int n = tb[line_idx] ? blobmsg_get_u32(tb[line_idx]) : 0;

	found = line_get(stub_dev, n);
	if (line)
		*line = n;
	if (l)
		*l = found;

	return found != NULL;
}

static int stub_ubus_hook(struct ubus_context *ctx, struct ubus_object *obj,
			  struct ubus_request_data *req, const char *method,
			  struct blob_attr *msg)
{
	struct blob_attr *tb[__STUB_HOOK_MAX] = {};
	struct hal_event ev = { .type = HAL_EVENT_HOOK };
	struct stub_line *l;
	unsigned int line;

	if (msg)
		blobmsg_parse(stub_hook_policy, __STUB_HOOK_MAX, tb, blob_data(msg),
			      blob_len(msg));

	if (!stub_dev || !tb[STUB_HOOK_STATE])
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (!stub_line_arg(tb, STUB_HOOK_LINE, &line, &l))
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (hook_state_parse(blobmsg_get_string(tb[STUB_HOOK_STATE]), &ev.u.hook.state))
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (tb[STUB_HOOK_PULSES])
		ev.u.hook.pulses = blobmsg_get_u32(tb[STUB_HOOK_PULSES]);
	if (ev.u.hook.state == HAL_HOOK_PULSE)
		ev.u.hook.pulse_invert = stub_pulse_invert();

	/* A pulse digit and a flash both end with the handset off hook, so only
	 * seize and release move the stored hook state. */
	if (ev.u.hook.state == HAL_HOOK_SEIZE)
		l->offhook = true;
	else if (ev.u.hook.state == HAL_HOOK_RELEASE)
		l->offhook = false;

	ev.line = line;
	event_push(stub_dev, &ev);

	return 0;
}

static int stub_ubus_digit(struct ubus_context *ctx, struct ubus_object *obj,
			   struct ubus_request_data *req, const char *method,
			   struct blob_attr *msg)
{
	struct blob_attr *tb[__STUB_DIGIT_MAX] = {};
	struct hal_event ev = { .type = HAL_EVENT_DIGIT };
	const char *digit;
	unsigned int line;

	if (msg)
		blobmsg_parse(stub_digit_policy, __STUB_DIGIT_MAX, tb, blob_data(msg),
			      blob_len(msg));

	if (!stub_dev || !tb[STUB_DIGIT_DIGIT])
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (!stub_line_arg(tb, STUB_DIGIT_LINE, &line, NULL))
		return UBUS_STATUS_INVALID_ARGUMENT;

	digit = blobmsg_get_string(tb[STUB_DIGIT_DIGIT]);
	if (!digit[0])
		return UBUS_STATUS_INVALID_ARGUMENT;

	ev.line = line;
	ev.u.digit.digit = digit[0];
	ev.u.digit.duration_ms = tb[STUB_DIGIT_DURATION] ?
				 blobmsg_get_u32(tb[STUB_DIGIT_DURATION]) : 100;
	ev.u.digit.remote = tb[STUB_DIGIT_REMOTE] ?
			    blobmsg_get_bool(tb[STUB_DIGIT_REMOTE]) : 0;
	event_push(stub_dev, &ev);

	return 0;
}

static int tone_code_parse(const char *name, uint16_t *code)
{
	static const struct {
		const char *name;
		uint16_t code;
	} map[] = {
		{ "ced", HAL_TONE_DET_CED },
		{ "ans", HAL_TONE_DET_ANS },
		{ "ansam", HAL_TONE_DET_ANSAM },
		{ "ansbar", HAL_TONE_DET_ANSBAR },
		{ "cng", HAL_TONE_DET_CNG },
		{ "v21", HAL_TONE_DET_V21_PREAMBLE },
		{ "cm", HAL_TONE_DET_FAX_CM },
		{ "timeout", HAL_TONE_DET_FAX_TIMEOUT },
		{ "modem", HAL_TONE_DET_MODEM },
		{ "other", HAL_TONE_DET_OTHER },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(map); i++) {
		if (strcmp(name, map[i].name))
			continue;

		*code = map[i].code;
		return 0;
	}

	return -1;
}

static int tone_direction_parse(const char *name, uint8_t *dir)
{
	if (!strcmp(name, "local")) {
		*dir = HAL_TONE_DIR_LOCAL;
		return 0;
	}

	if (!strcmp(name, "remote")) {
		*dir = HAL_TONE_DIR_REMOTE;
		return 0;
	}

	return -1;
}

static int stub_ubus_tone(struct ubus_context *ctx, struct ubus_object *obj,
			  struct ubus_request_data *req, const char *method,
			  struct blob_attr *msg)
{
	struct blob_attr *tb[__STUB_TONE_MAX] = {};
	struct hal_event ev = { .type = HAL_EVENT_TONE };
	unsigned int line;

	if (msg)
		blobmsg_parse(stub_tone_policy, __STUB_TONE_MAX, tb, blob_data(msg),
			      blob_len(msg));

	if (!stub_dev || !tb[STUB_TONE_CODE])
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (!stub_line_arg(tb, STUB_TONE_LINE, &line, NULL))
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (tone_code_parse(blobmsg_get_string(tb[STUB_TONE_CODE]), &ev.u.tone.code))
		return UBUS_STATUS_INVALID_ARGUMENT;

	/* the detector reports most of these toward the phone, which is where a
	 * CED comes from, so that is the default a caller may leave out */
	ev.u.tone.direction = HAL_TONE_DIR_LOCAL;
	if (tb[STUB_TONE_DIRECTION] &&
	    tone_direction_parse(blobmsg_get_string(tb[STUB_TONE_DIRECTION]),
				 &ev.u.tone.direction))
		return UBUS_STATUS_INVALID_ARGUMENT;

	ev.line = line;
	ev.u.tone.end = tb[STUB_TONE_END] &&
			blobmsg_get_bool(tb[STUB_TONE_END]) ? 1 : 0;
	ev.u.tone.duration_ms = tb[STUB_TONE_DURATION] ?
				(uint16_t)blobmsg_get_u32(tb[STUB_TONE_DURATION]) : 0;
	event_push(stub_dev, &ev);

	return 0;
}

static int stub_ubus_flash(struct ubus_context *ctx, struct ubus_object *obj,
			   struct ubus_request_data *req, const char *method,
			   struct blob_attr *msg)
{
	struct blob_attr *tb[__STUB_FLASH_MAX] = {};
	struct hal_event ev = { .type = HAL_EVENT_HOOK };
	unsigned int line;

	if (msg)
		blobmsg_parse(stub_flash_policy, __STUB_FLASH_MAX, tb, blob_data(msg),
			      blob_len(msg));

	if (!stub_dev)
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (!stub_line_arg(tb, STUB_FLASH_LINE, &line, NULL))
		return UBUS_STATUS_INVALID_ARGUMENT;

	ev.line = line;
	ev.u.hook.state = HAL_HOOK_FLASH;
	event_push(stub_dev, &ev);

	return 0;
}

/*
 * Ring is driven, not injected: it exercises the real op and lets CI observe
 * the RING_END that hal_ring.duration_ms produces.
 */
static int stub_ubus_ring(struct ubus_context *ctx, struct ubus_object *obj,
			  struct ubus_request_data *req, const char *method,
			  struct blob_attr *msg)
{
	struct blob_attr *tb[__STUB_RING_MAX] = {};
	struct hal_ring ring = {
		.cadence_count = 1,
		.cadence = { { .on_ms = 2000, .off_ms = 4000 } },
	};
	unsigned int line;

	if (msg)
		blobmsg_parse(stub_ring_policy, __STUB_RING_MAX, tb, blob_data(msg),
			      blob_len(msg));

	if (!stub_dev)
		return UBUS_STATUS_NOT_FOUND;

	line = tb[STUB_RING_LINE] ? blobmsg_get_u32(tb[STUB_RING_LINE]) : 0;
	if (tb[STUB_RING_DURATION])
		ring.duration_ms = blobmsg_get_u32(tb[STUB_RING_DURATION]);

	if (hal_backend_ops.line_ring_start(stub_dev, line, &ring) != HAL_OK)
		return UBUS_STATUS_INVALID_ARGUMENT;

	return 0;
}

static int stub_ubus_ring_stop(struct ubus_context *ctx, struct ubus_object *obj,
			       struct ubus_request_data *req, const char *method,
			       struct blob_attr *msg)
{
	struct blob_attr *tb[__STUB_RING_MAX] = {};
	unsigned int line;

	if (msg)
		blobmsg_parse(stub_ring_policy, __STUB_RING_MAX, tb, blob_data(msg),
			      blob_len(msg));

	if (!stub_dev)
		return UBUS_STATUS_NOT_FOUND;

	line = tb[STUB_RING_LINE] ? blobmsg_get_u32(tb[STUB_RING_LINE]) : 0;
	if (hal_backend_ops.line_ring_stop(stub_dev, line) != HAL_OK)
		return UBUS_STATUS_INVALID_ARGUMENT;

	return 0;
}

/*
 * The codec a stream was programmed with, as a name. The names are the ones
 * "ubus call voice status" already publishes for hal_cap.codecs, so one word
 * means one codec across both surfaces. The numeric field stays beside it in
 * the dump: a name is what makes a T.38 stream legible, because nothing else
 * in the record says the stream carries UDPTL instead of audio.
 */
static const char *codec_name(uint8_t codec)
{
	static const char * const names[] = {
		[HAL_CODEC_NONE] = "none",
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

	if (codec >= ARRAY_SIZE(names) || !names[codec])
		return "unknown";

	return names[codec];
}

/*
 * The suite a stream was keyed with, as a name, beside the numeric field the
 * same way the codec pair above works. The names are the SDES suite names of
 * RFC 4568 in lower case, so one word means one crypto attribute across the
 * dump, the daemon trace and the wire.
 */
static const char *srtp_suite_name(uint8_t suite)
{
	static const char * const names[] = {
		[HAL_SRTP_NONE] = "none",
		[HAL_SRTP_AES_CM_128_SHA1_80] = "aes_cm_128_hmac_sha1_80",
		[HAL_SRTP_AES_CM_128_SHA1_32] = "aes_cm_128_hmac_sha1_32",
	};

	if (suite >= ARRAY_SIZE(names) || !names[suite])
		return "unknown";

	return names[suite];
}

/*
 * The standard reflected CRC-32, the IEEE 802.3 polynomial 0xEDB88320 with an
 * all-ones start and a final inversion, computed bit by bit because 30 bytes
 * twice per dump does not pay for a table. It is the digest zlib crc32() and
 * python binascii.crc32() produce, so the side that offered a key computes the
 * same number from that key alone and needs nothing from this file.
 */
static uint32_t crc32_get(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint32_t crc = 0xFFFFFFFF;
	size_t i;
	unsigned int bit;

	for (i = 0; i < len; i++) {
		crc ^= p[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0);
	}

	return ~crc;
}

/*
 * The SRTP block of the applied hal_media. The keys themselves never enter
 * this dump, in whole or in part: a key that reaches a ubus reply reaches
 * every log that captures one, and the rule is to trace suite names only. The
 * two digests exist so an end to end run can assert that the key it offered
 * is the key the backend was programmed with, which a suite name alone cannot
 * show. They are absent on a plain RTP stream, where the key bytes are all
 * zero and mean nothing, and absent again once the stream stops, because the
 * stop zeroizes the block (hal.h).
 *
 * Each digest travels as a 64 bit field although it is 32 bits wide. A blobmsg
 * INT32 renders signed in JSON, so about half of the digests would reach a
 * reader as a negative number that no zlib crc32() output can match; a u32
 * widened into a u64 is always the positive number both sides compute
 * (cdr_media_dump carries its counters the same way).
 */
static void srtp_dump(const struct hal_media *m)
{
	blobmsg_add_u32(&b, "srtp_suite", m->srtp.suite);
	blobmsg_add_string(&b, "srtp_suite_name", srtp_suite_name(m->srtp.suite));

	if (m->srtp.suite == HAL_SRTP_NONE)
		return;

	blobmsg_add_u64(&b, "srtp_key_tx_crc",
			crc32_get(m->srtp.key_tx, sizeof(m->srtp.key_tx)));
	blobmsg_add_u64(&b, "srtp_key_rx_crc",
			crc32_get(m->srtp.key_rx, sizeof(m->srtp.key_rx)));
}

/* An address of the record in presentation form, and the empty string for an
 * ipver the contract does not name. */
static void addr_text(char *out, size_t len, uint8_t ipver, const uint8_t *addr)
{
	out[0] = 0;

	if (ipver == 4)
		inet_ntop(AF_INET, addr, out, len);
	else if (ipver == 6)
		inet_ntop(AF_INET6, addr, out, len);
}

/*
 * The endpoints of the applied hal_media, in presentation form. An end to end
 * run reads the family a stream really took here, because no other field of
 * the dump says it.
 *
 * port is false for the remote endpoint, because the remote_port key above
 * already carries it and a table names a key once.
 */
static void addr_dump(const char *prefix, bool port, const struct hal_addr *a)
{
	uint16_t rtcp = a->rtcp_port;
	char text[INET6_ADDRSTRLEN];
	char key[32];

	addr_text(text, sizeof(text), a->ipver, a->addr);

	snprintf(key, sizeof(key), "%s_ipver", prefix);
	blobmsg_add_u32(&b, key, a->ipver);
	snprintf(key, sizeof(key), "%s_addr", prefix);
	blobmsg_add_string(&b, key, text);
	if (port) {
		snprintf(key, sizeof(key), "%s_port", prefix);
		blobmsg_add_u32(&b, key, a->port);
	}

	/* The port the udsp backend derives, so the dump names the socket
	 * it would open and not the raw field (hal.h struct hal_addr). A
	 * stream with no port of its own derives nothing and reads 0. */
	if (!rtcp && a->port)
		rtcp = (uint16_t)(a->port + 1);

	snprintf(key, sizeof(key), "%s_rtcp_port", prefix);
	blobmsg_add_u32(&b, key, rtcp);
}

/*
 * The far end's own RTCP address, and only when it named one. Presence is the
 * message, as in srtp_dump: no key at all means RTCP goes to the media
 * address (hal.h struct hal_media rtcp_remote).
 */
static void rtcp_remote_dump(const struct hal_rtcp_addr *a)
{
	char text[INET6_ADDRSTRLEN];

	if (!a->ipver)
		return;

	addr_text(text, sizeof(text), a->ipver, a->addr);
	blobmsg_add_u32(&b, "rtcp_remote_ipver", a->ipver);
	blobmsg_add_string(&b, "rtcp_remote_addr", text);
}

static void stream_dump(const struct stub_line *l, unsigned int streams)
{
	const struct stub_stream *s;
	void *array, *table;
	unsigned int i;

	array = blobmsg_open_array(&b, "stream");
	for (i = 0; i < streams; i++) {
		s = &l->stream[i];
		table = blobmsg_open_table(&b, NULL);
		blobmsg_add_u32(&b, "index", i);
		blobmsg_add_u8(&b, "configured", s->configured);
		blobmsg_add_u8(&b, "running", s->running);
		blobmsg_add_u32(&b, "codec", s->media.codec);
		blobmsg_add_string(&b, "codec_name",
				   codec_name(s->media.codec));
		blobmsg_add_u32(&b, "payload_type", s->media.codec_pt);
		blobmsg_add_u32(&b, "ptime", s->media.ptime_ms);
		blobmsg_add_u32(&b, "direction", s->media.direction);
		/* The hold silence beside the direction it accompanies (hal.h):
		 * the two together are what says whether the far end hears this
		 * room, and neither one says it alone. */
		blobmsg_add_u8(&b, "tx_silence", s->media.tx_silence);
		/* The passthrough kind of the applied hal_media, raw: the stub
		 * runs no canceller, so what it can show is the number the core
		 * programmed. */
		blobmsg_add_u32(&b, "passthrough", s->media.passthrough);
		/*
		 * The three suppression fields of the applied hal_media, which
		 * are one answer and are read as one. The stub runs no encoder,
		 * so what it can show is what the negotiation reached, and no
		 * SDP line states it: annexb is our own transmitter and the
		 * wire annexb is the peer's claim about its decoder.
		 *
		 * The pair is emitted beside it because annexb REPLACES it
		 * (hal.h). An end to end run that read annexb alone could not
		 * tell an armed stream from one that also runs the generic
		 * detector, which is the one thing the substitution promises.
		 */
		blobmsg_add_u8(&b, "vad", s->media.vad);
		blobmsg_add_u8(&b, "cng", s->media.cng);
		blobmsg_add_u8(&b, "annexb", s->media.annexb);
		blobmsg_add_u32(&b, "remote_port", s->media.remote.port);
		addr_dump("local", true, &s->media.local);
		addr_dump("remote", false, &s->media.remote);
		rtcp_remote_dump(&s->media.rtcp_remote);
		/* Presence is the message, as in srtp_dump: an absent blob key
		 * means the stream took no device and binds unscoped */
		if (s->media.bind_dev[0])
			blobmsg_add_string(&b, "bind_dev", s->media.bind_dev);
		/* the T.38 block of the applied hal_media, which is meaningful
		 * only when the codec above is t38 */
		blobmsg_add_u32(&b, "t38_max_bit_rate",
				s->media.t38.max_bit_rate);
		blobmsg_add_u32(&b, "t38_max_datagram",
				s->media.t38.max_datagram);
		blobmsg_add_u32(&b, "t38_ec", s->media.t38.ec);
		/*
		 * The opus block of the applied hal_media, meaningful only
		 * when the codec above is opus. It carries the encoder the
		 * negotiation settled on, the configured ask narrowed by the
		 * peer's fmtp, so the e2e reads the whole constraint chain off
		 * a running stream instead of stopping at the payload type.
		 */
		blobmsg_add_u32(&b, "opus_bitrate", s->media.opus.bitrate);
		blobmsg_add_u32(&b, "opus_complexity", s->media.opus.complexity);
		blobmsg_add_u32(&b, "opus_max_bandwidth",
				s->media.opus.max_bandwidth);
		blobmsg_add_u8(&b, "opus_fec", s->media.opus.fec);
		srtp_dump(&s->media);
		/* whether this stream answers the canned media numbers or the
		 * ones a media_stats call chose */
		blobmsg_add_u8(&b, "stats_override", s->stats_set);
		/* the fax profile this stream was switched with */
		blobmsg_add_u8(&b, "fax_applied", s->fax_set);
		blobmsg_add_u32(&b, "fax_mode", s->fax.mode);
		blobmsg_add_u32(&b, "fax_codec", s->fax.codec);
		blobmsg_add_u32(&b, "fax_payload_type", s->fax.codec_pt);
		blobmsg_add_u8(&b, "fax_ec_disable", s->fax.ec_disable);
		blobmsg_add_u8(&b, "fax_vad_disable", s->fax.vad_disable);
		blobmsg_add_u8(&b, "fax_cng_disable", s->fax.cng_disable);
		blobmsg_add_u8(&b, "fax_silence_comp_disable",
			       s->fax.silence_comp_disable);
		blobmsg_add_u8(&b, "fax_jb_fixed", s->fax.jb_fixed);
		blobmsg_close_table(&b, table);
	}
	blobmsg_close_array(&b, array);
}

static int stub_ubus_dump(struct ubus_context *ctx, struct ubus_object *obj,
			  struct ubus_request_data *req, const char *method,
			  struct blob_attr *msg)
{
	const struct stub_line *l;
	void *array, *table;
	unsigned int i;

	if (!stub_dev)
		return UBUS_STATUS_NOT_FOUND;

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "region", stub_dev->region);

	array = blobmsg_open_array(&b, "line");
	for (i = 0; i < stub_dev->cap.lines; i++) {
		l = &stub_dev->line[i];
		table = blobmsg_open_table(&b, NULL);
		blobmsg_add_u32(&b, "index", i);
		blobmsg_add_u8(&b, "offhook", l->offhook);
		blobmsg_add_u8(&b, "ringing", l->ringing);
		blobmsg_add_u32(&b, "state", l->state);
		blobmsg_add_u32(&b, "polarity", l->polarity);
		blobmsg_add_u32(&b, "mwi", l->mwi);
		blobmsg_add_u32(&b, "tone", l->tone);
		blobmsg_add_u32(&b, "tone_duration_ms", l->tone_duration_ms);
		blobmsg_add_u32(&b, "cw_variant", l->cw_variant);
		blobmsg_add_u32(&b, "conf", l->conf);
		/* the loop test record, carried whatever the mode is, as the
		 * conference key above is */
		blobmsg_add_u32(&b, "loop_mode", l->loop_mode);
		blobmsg_add_u32(&b, "loop_peer", l->loop_peer);
		blobmsg_add_u32(&b, "loop_delay_ms", l->loop_delay_ms);
		/* the whole hal_cid record, so a test or the e2e can tell a
		 * withheld identity from an absent one without a phone */
		blobmsg_add_string(&b, "cid_number", l->cid.number);
		blobmsg_add_string(&b, "cid_name", l->cid.name);
		blobmsg_add_u32(&b, "cid_format", l->cid.format);
		blobmsg_add_u8(&b, "cid_number_private", l->cid.number_private);
		blobmsg_add_u8(&b, "cid_name_private", l->cid.name_private);
		blobmsg_add_string(&b, "cid_blockinfo", l->cid.blockinfo);
		/* the last hook timing the core pushed, so a suite can read the
		 * three /etc/config/voip thresholds back without a phone */
		blobmsg_add_u32(&b, "hook_flash_min", l->hook.flash_min_ms);
		blobmsg_add_u32(&b, "hook_flash_max", l->hook.flash_max_ms);
		blobmsg_add_u32(&b, "hook_release_min", l->hook.release_min_ms);
		/* how often the call layer dropped the tone-disabler latch of
		 * this line, which is what a return to voice asks for */
		blobmsg_add_u32(&b, "latch_clears", l->latch_clears);
		stream_dump(l, stub_dev->cap.streams_per_chan);
		blobmsg_close_table(&b, table);
	}
	blobmsg_close_array(&b, array);

	ubus_send_reply(ctx, req, b.head);

	return 0;
}

/*
 * The media stream of a call, without a call. The jitter buffer getter above
 * answers only a running stream, and nothing in the CLI environment can bring
 * one up the ordinary way: there is no registrar, so no SIP leg, so no media.
 * This injects the state a leg would have left, the way the hook verb injects
 * the state a handset would have left, and it moves the same flag the real
 * stream ops move, so the getter carries no test-only branch of its own.
 */
enum {
	STUB_STREAM_LINE,
	STUB_STREAM_STREAM,
	STUB_STREAM_UP,
	__STUB_STREAM_MAX,
};

static const struct blobmsg_policy stub_stream_policy[__STUB_STREAM_MAX] = {
	[STUB_STREAM_LINE] = { "line", BLOBMSG_TYPE_INT32 },
	[STUB_STREAM_STREAM] = { "stream", BLOBMSG_TYPE_INT32 },
	[STUB_STREAM_UP] = { "up", BLOBMSG_TYPE_BOOL },
};

static int stub_ubus_stream(struct ubus_context *ctx, struct ubus_object *obj,
			    struct ubus_request_data *req, const char *method,
			    struct blob_attr *msg)
{
	struct blob_attr *tb[__STUB_STREAM_MAX] = {};
	unsigned int line, stream;
	struct stub_stream *s;
	bool up;

	if (msg)
		blobmsg_parse(stub_stream_policy, __STUB_STREAM_MAX, tb,
			      blob_data(msg), blob_len(msg));

	if (!stub_dev)
		return UBUS_STATUS_NOT_FOUND;

	line = tb[STUB_STREAM_LINE] ? blobmsg_get_u32(tb[STUB_STREAM_LINE]) : 0;
	stream = tb[STUB_STREAM_STREAM] ?
		 blobmsg_get_u32(tb[STUB_STREAM_STREAM]) : 0;
	s = stream_get(stub_dev, line, stream);
	if (!s)
		return UBUS_STATUS_INVALID_ARGUMENT;

	up = tb[STUB_STREAM_UP] ? blobmsg_get_bool(tb[STUB_STREAM_UP]) : true;
	/*
	 * The real ops do the work, and the down path especially. A stop is
	 * more than clearing a flag. It clears the description flag, because a
	 * stopped stream must be described again (hal.h stream_stop). It keeps
	 * the media snapshot, so the dump still says what the call ran. It
	 * zeroizes the key material (hal.h, the zeroize rule). An injector that
	 * cleared the wrong flag or kept a digest would make "voice.stub dump"
	 * after a forced down disagree with the same dump after a real hangup,
	 * and CI would then be reading a state no call can produce. Only the
	 * configure step is short-circuited, because this object has no media
	 * description to hand stream_config_set. The mark it leaves is the real
	 * one: stub_stream_configured_mark is what stream_config_set calls. A
	 * stream this verb brings up therefore leaves the line state a
	 * configured stream leaves, the canceller instance included.
	 */
	if (up) {
		stub_stream_configured_mark(stub_dev, line, s);
		stub_stream_start(stub_dev, line, stream);
	} else {
		stub_stream_stop(stub_dev, line, stream);
	}
	TRACE(TRACE_HAL, "stub: line %u strm %u %s", line, stream,
	      up ? "up" : "down");

	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "line", line);
	blobmsg_add_u32(&b, "stream", stream);
	blobmsg_add_u8(&b, "running", s->running);
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

/*
 * The first received packet of a stream (hal.h HAL_EVENT_RTP_FIRST_RX), which
 * this backend carries no RTP socket to produce. It is the injector shape of
 * hook, digit and tone, and not a field of the verb above: that verb runs the
 * real ops and defaults up to true, so it would restart a running stream.
 *
 * A stopped stream is refused. libudsp emits the record from the receive path
 * of a running stream only (udsp core/stream.c stream_mark()), so an injection
 * on a stopped one would let CI read a state no call can reach, the rule
 * stub_ubus_stream() states for itself.
 */
enum {
	STUB_RTP_RX_LINE,
	STUB_RTP_RX_STREAM,
	__STUB_RTP_RX_MAX,
};

static const struct blobmsg_policy stub_rtp_rx_policy[__STUB_RTP_RX_MAX] = {
	[STUB_RTP_RX_LINE] = { "line", BLOBMSG_TYPE_INT32 },
	[STUB_RTP_RX_STREAM] = { "stream", BLOBMSG_TYPE_INT32 },
};

static int stub_ubus_rtp_rx(struct ubus_context *ctx, struct ubus_object *obj,
			    struct ubus_request_data *req, const char *method,
			    struct blob_attr *msg)
{
	struct blob_attr *tb[__STUB_RTP_RX_MAX] = {};
	struct hal_event ev = { .type = HAL_EVENT_RTP_FIRST_RX };
	unsigned int line, stream;
	struct stub_stream *s;

	if (msg)
		blobmsg_parse(stub_rtp_rx_policy, __STUB_RTP_RX_MAX, tb,
			      blob_data(msg), blob_len(msg));

	if (!stub_dev)
		return UBUS_STATUS_NOT_FOUND;

	line = tb[STUB_RTP_RX_LINE] ? blobmsg_get_u32(tb[STUB_RTP_RX_LINE]) : 0;
	stream = tb[STUB_RTP_RX_STREAM] ?
		 blobmsg_get_u32(tb[STUB_RTP_RX_STREAM]) : 0;
	s = stream_get(stub_dev, line, stream);
	if (!s || !s->running)
		return UBUS_STATUS_INVALID_ARGUMENT;

	ev.line = (uint16_t)line;
	ev.stream = (uint16_t)stream;
	event_push(stub_dev, &ev);
	TRACE(TRACE_HAL, "stub: line %u strm %u first packet received", line,
	      stream);

	return 0;
}

static const struct ubus_method stub_methods[] = {
	UBUS_METHOD("hook", stub_ubus_hook, stub_hook_policy),
	UBUS_METHOD("digit", stub_ubus_digit, stub_digit_policy),
	UBUS_METHOD("tone", stub_ubus_tone, stub_tone_policy),
	UBUS_METHOD("flash", stub_ubus_flash, stub_flash_policy),
	UBUS_METHOD("ring", stub_ubus_ring, stub_ring_policy),
	UBUS_METHOD("ring_stop", stub_ubus_ring_stop, stub_ring_policy),
	UBUS_METHOD("media_stats", stub_ubus_media_stats, stub_stats_policy),
	UBUS_METHOD("stream", stub_ubus_stream, stub_stream_policy),
	UBUS_METHOD("rtp_rx", stub_ubus_rtp_rx, stub_rtp_rx_policy),
	UBUS_METHOD_NOARG("dump", stub_ubus_dump),
};

static struct ubus_object_type stub_object_type =
	UBUS_OBJECT_TYPE("voice.stub", stub_methods);

static struct ubus_object stub_object = {
	.name = "voice.stub",
	.type = &stub_object_type,
	.methods = stub_methods,
	.n_methods = ARRAY_SIZE(stub_methods),
};

void stub_ubus_attach(struct ubus_context *ctx)
{
	int ret = ubus_add_object(ctx, &stub_object);

	if (ret)
		ULOG_ERR("failed to add object voice.stub: %s\n", ubus_strerror(ret));
}
