// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The media plane of the css backend: one /dev/css-voice session per stream
 * (hal.h stream_config_set rules 1-4), the UDP sockets this backend owns and
 * protects with libuvsrtp, the DVF99 voice config mapping, DTMF and stats. The
 * datagram pump between the sockets and the device is the packet thread, added
 * in the next change; this file carries the control path that the thread needs.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <uvsrtp/uvsrtp.h>

#include "css_int.h"

#include "../../posix.h"
#include "../../trace.h"

static struct css_stream *css_stream_get(struct hal *h, unsigned int line,
					 unsigned int stream)
{
	if (line >= CSS_LINES || stream >= CSS_STREAMS_PER_CHAN)
		return NULL;

	return &h->stream[line][stream];
}

/* hal codec -> enum dvf99_voice_codec, or -1 for one this backend does not
 * carry. The cap.codecs mask the open published is the same set. */
static int css_codec_map(uint8_t codec)
{
	switch (codec) {
	case HAL_CODEC_PCMU:
		return DVF99_VOICE_CODEC_PCMU;
	case HAL_CODEC_PCMA:
		return DVF99_VOICE_CODEC_PCMA;
	case HAL_CODEC_G722:
		return DVF99_VOICE_CODEC_G722;
	case HAL_CODEC_G729:
		return DVF99_VOICE_CODEC_G729;
	case HAL_CODEC_G726_32:
		return DVF99_VOICE_CODEC_G726_32;
	default:
		return -1;
	}
}

static uint8_t css_dir_map(uint8_t dir)
{
	switch (dir) {
	case HAL_MEDIA_SENDONLY:
		return DVF99_VOICE_DIR_SENDONLY;
	case HAL_MEDIA_RECVONLY:
		return DVF99_VOICE_DIR_RECVONLY;
	case HAL_MEDIA_INACTIVE:
		return DVF99_VOICE_DIR_INACTIVE;
	default:
		return DVF99_VOICE_DIR_SENDRECV;
	}
}

/* An RFC 4733 event number for a dialled character, or -1 for one the CSS has
 * no event for. */
static int css_dtmf_event(char digit)
{
	if (digit >= '0' && digit <= '9')
		return digit - '0';
	if (digit == '*')
		return 10;
	if (digit == '#')
		return 11;
	if (digit >= 'A' && digit <= 'D')
		return 12 + (digit - 'A');
	if (digit >= 'a' && digit <= 'd')
		return 12 + (digit - 'a');

	return -1;
}

static uint32_t css_rand32(void)
{
	uint32_t v;

	if (getrandom(&v, sizeof(v), 0) != (ssize_t)sizeof(v))
		v = (uint32_t)rand();

	return v;
}

static int css_srtp_suite(uint8_t hal_suite, enum uvsrtp_suite *out)
{
	switch (hal_suite) {
	case HAL_SRTP_NONE:
		*out = UVSRTP_SUITE_NONE;
		return 0;
	case HAL_SRTP_AES_CM_128_SHA1_80:
		*out = UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80;
		return 0;
	case HAL_SRTP_AES_CM_128_SHA1_32:
		*out = UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_32;
		return 0;
	default:
		return -1;
	}
}

/*
 * The DVF99 voice config of one description. The identity (ssrc, timestamp and
 * cname) is set once when the stream is first configured and kept across a
 * re-INVITE, so a bare reconfiguration does not reset the RTP stream; only the
 * description-derived fields are rebuilt. A stream is detached from the audio
 * path (spec 8.2) when it does not send and receive, or when the core holds it
 * (tx_silence).
 */
static void css_cfg_build(unsigned int line, unsigned int stream,
			  const struct hal_media *m,
			  struct dvf99_voice_config *cfg, bool fresh)
{
	if (fresh) {
		memset(cfg, 0, sizeof(*cfg));
		cfg->ssrc = css_rand32();
		cfg->timestamp = css_rand32();
		snprintf(cfg->cname, sizeof(cfg->cname), "uvoice-%08x",
			 cfg->ssrc);
	}

	cfg->line = css_dev_line(line, stream);
	cfg->codec = (uint32_t)css_codec_map(m->codec);
	cfg->flags = 0;
	if (m->vad)
		cfg->flags |= DVF99_VOICE_F_VAD;
	if (m->rtcp_enable)
		cfg->flags |= DVF99_VOICE_F_RTCP;
	if (m->direction != HAL_MEDIA_SENDRECV || m->tx_silence)
		cfg->flags |= DVF99_VOICE_F_DETACHED;
	cfg->ptime_ms = m->ptime_ms;
	cfg->tx_pt = m->codec_pt;
	cfg->rx_pt = m->codec_pt;
	cfg->tx_pt_event = m->dtmf_pt ? m->dtmf_pt : DVF99_VOICE_PT_NONE;
	cfg->rx_pt_event = m->dtmf_pt ? m->dtmf_pt : DVF99_VOICE_PT_NONE;
	cfg->direction = css_dir_map(m->direction);
	cfg->jb_mode = m->jb_mode == HAL_JB_FIXED ? DVF99_VOICE_JB_FIXED :
						    DVF99_VOICE_JB_ADAPTIVE;
	cfg->rtcp_interval_s = (uint8_t)(m->rtcp_interval_ms / 1000u);
}

/*
 * What this backend can program at all (hal.h stream_config_set rule 4), tested
 * before anything is touched. The codec and the SRTP suite are the backend's
 * own capability; the address shape is the shared check.
 */
static enum hal_status css_media_validate(struct hal *h,
					  const struct hal_media *m)
{
	if (!srtp_suite_known(m->srtp.suite)) {
		TRACE(TRACE_HAL, "css: srtp suite %u is not one this backend "
		      "carries", m->srtp.suite);
		return HAL_UNSUPPORTED;
	}
	if (css_codec_map(m->codec) < 0) {
		TRACE(TRACE_HAL, "css: codec %u is not one this backend carries",
		      m->codec);
		return HAL_UNSUPPORTED;
	}

	return hal_media_validate_addrs(m, h->cap.features);
}

static void css_srtp_drop(struct css_stream *s)
{
	if (s->srtp_live)
		uvsrtp_destroy(s->srtp);
	s->srtp = NULL;
	s->srtp_live = false;
}

/*
 * Wake the packet thread and wait until it has rebuilt its poll set to the
 * current running flags, so the uloop thread can close a stopped stream's
 * descriptors without racing the thread's poll. A no-op before the thread runs.
 */
static void css_pkt_sync(struct hal *h)
{
	uint32_t g;
	char b = 1;

	if (!h->pkt_alive)
		return;

	pthread_mutex_lock(&h->pkt_lock);
	g = ++h->pkt_gen;
	pthread_mutex_unlock(&h->pkt_lock);

	while (write(h->pkt_wake[1], &b, 1) < 0 && errno == EINTR)
		;

	pthread_mutex_lock(&h->pkt_lock);
	while (h->pkt_acked != g)
		pthread_cond_wait(&h->pkt_cond, &h->pkt_lock);
	pthread_mutex_unlock(&h->pkt_lock);
}

/*
 * The stream stop of this backend: drop the stream from the packet thread (so
 * no descriptor is closed under its poll), tell the CSS to stop the session,
 * close the device and the sockets, drop the SRTP context.
 */
static void css_stream_halt(struct hal *h, struct css_stream *s)
{
	if (s->running) {
		pthread_mutex_lock(&h->pkt_lock);
		s->running = false;
		pthread_mutex_unlock(&h->pkt_lock);
		css_pkt_sync(h);
	}
	if (s->dev_fd >= 0) {
		h->dev->xioctl(h->dev->ctx, s->dev_fd, DVF99_VOICE_STOP, NULL);
		h->dev->xclose(h->dev->ctx, s->dev_fd);
		s->dev_fd = -1;
	}
	hal_media_sock_close(&s->sock);
	css_srtp_drop(s);
	s->configured = false;
}

/*
 * The sockets a programming pass needs. A stream that already holds a pair on
 * the same local tuple and device keeps it (a re-INVITE re-applies on a running
 * stream); a local address, port or device move halts the stream and makes a
 * fresh pair, because a live media socket cannot rebind under a flow the far
 * end is filtering.
 */
static enum hal_status css_sockets_take(struct hal *h, struct css_stream *s,
					const struct hal_media *m, bool *fresh)
{
	enum hal_status st;

	*fresh = false;
	if (s->sock.rtp_fd >= 0 && addr_same(&s->sock.local, &m->local) &&
	    !memcmp(s->sock.bind_dev, m->bind_dev, sizeof(s->sock.bind_dev)))
		return HAL_OK;

	if (s->sock.rtp_fd >= 0)
		css_stream_halt(h, s);

	st = hal_media_sock_open(&s->sock, m);
	if (st != HAL_OK)
		return st;

	*fresh = true;

	return HAL_OK;
}

enum hal_status css_stream_config_set(struct hal *h, unsigned int line,
				      unsigned int stream,
				      const struct hal_media *media)
{
	struct css_stream *s = css_stream_get(h, line, stream);
	struct dvf99_voice_config cfg;
	enum uvsrtp_suite suite = UVSRTP_SUITE_NONE;
	struct hal_addr rtcp;
	enum hal_status st;
	bool fresh;

	if (!s || !media)
		return HAL_UNSUPPORTED;
	st = css_media_validate(h, media);
	if (st != HAL_OK)
		return st;

	st = css_sockets_take(h, s, media, &fresh);
	if (st != HAL_OK)
		return st;

	if (s->dev_fd < 0) {
		s->dev_fd = h->dev->open(h->dev->ctx, css_dev_line(line, stream));
		if (s->dev_fd < 0) {
			TRACE(TRACE_HAL, "css: open %s: %s", css_voice_dev_path,
			      strerror(errno));
			if (fresh)
				hal_media_sock_close(&s->sock);
			return HAL_FATAL;
		}
	}

	hal_media_rtcp_peer(media, &rtcp);
	if (fresh || !addr_same(&s->sock.remote, &media->remote) ||
	    !addr_same(&s->sock.rtcp_remote, &rtcp)) {
		st = hal_media_sock_connect(&s->sock, media);
		if (st != HAL_OK) {
			if (fresh)
				css_stream_halt(h, s);
			return st;
		}
	} else {
		hal_media_sock_tos_refresh(&s->sock, media);
	}

	/* SRTP: create on the first keyed config, rekey on a live one, drop on a
	 * description that turns it off. The suite is already known. */
	css_srtp_suite(media->srtp.suite, &suite);
	if (suite == UVSRTP_SUITE_NONE) {
		css_srtp_drop(s);
	} else if (uvsrtp_init()) {
		TRACE(TRACE_HAL, "css: libsrtp init failed");
		if (fresh)
			css_stream_halt(h, s);
		return HAL_FATAL;
	} else {
		int rc = s->srtp_live ?
			 uvsrtp_rekey(s->srtp, suite, media->srtp.key_tx,
				      media->srtp.key_rx) :
			 uvsrtp_create(&s->srtp, suite, media->srtp.key_tx,
				       media->srtp.key_rx);

		if (rc) {
			TRACE(TRACE_HAL, "css: srtp context: %d", rc);
			if (fresh)
				css_stream_halt(h, s);
			return HAL_FATAL;
		}
		s->srtp_live = true;
	}

	cfg = s->cfg;
	css_cfg_build(line, stream, media, &cfg, !s->configured);

	/* A reconfiguration of a running stream reaches the CSS at once; a fresh
	 * one is pushed by stream_start. */
	if (s->running &&
	    h->dev->xioctl(h->dev->ctx, s->dev_fd, DVF99_VOICE_UPDATE, &cfg)) {
		TRACE(TRACE_HAL, "css: DVF99_VOICE_UPDATE: %s", strerror(errno));
		return HAL_FATAL;
	}

	TRACE(TRACE_HAL, "css: line %u stream %u config: css line %u pt %u "
	      "event pt %u running %d", line, stream, cfg.line, cfg.tx_pt,
	      cfg.tx_pt_event, s->running);
	s->cfg = cfg;
	s->configured = true;

	return HAL_OK;
}

enum hal_status css_stream_start(struct hal *h, unsigned int line,
				 unsigned int stream)
{
	struct css_stream *s = css_stream_get(h, line, stream);

	if (!s || !s->configured || s->dev_fd < 0)
		return HAL_UNSUPPORTED;
	if (s->running)
		return HAL_OK;

	s->jit_have = false;
	s->jit_transit = 0;
	s->jit_ssrc = 0;
	atomic_store_explicit(&s->jit, 0u, memory_order_relaxed);

	if (h->dev->xioctl(h->dev->ctx, s->dev_fd, DVF99_VOICE_START, &s->cfg)) {
		TRACE(TRACE_HAL, "css: DVF99_VOICE_START: %s", strerror(errno));
		return HAL_FATAL;
	}
	/* Publish the stream to the packet thread, which starts pumping its
	 * device and sockets on the next cycle. */
	pthread_mutex_lock(&h->pkt_lock);
	s->running = true;
	pthread_mutex_unlock(&h->pkt_lock);
	css_pkt_sync(h);

	return HAL_OK;
}

enum hal_status css_stream_stop(struct hal *h, unsigned int line,
				unsigned int stream)
{
	struct css_stream *s = css_stream_get(h, line, stream);

	if (!s)
		return HAL_UNSUPPORTED;

	css_stream_halt(h, s);

	return HAL_OK;
}

enum hal_status css_stream_dtmf_send(struct hal *h, unsigned int line,
				     unsigned int stream, char digit,
				     uint32_t duration_ms)
{
	struct css_stream *s = css_stream_get(h, line, stream);
	struct dvf99_voice_dtmf d;
	int event = css_dtmf_event(digit);

	if (!s || s->dev_fd < 0 || !s->running)
		return HAL_UNSUPPORTED;
	if (event < 0)
		return HAL_UNSUPPORTED;

	memset(&d, 0, sizeof(d));
	d.event = (uint8_t)event;
	d.volume = 10;			/* -10 dBm0, the RFC 4733 default */
	d.duration_ms = duration_ms > 0xffffu ? 0xffffu : (uint16_t)duration_ms;
	if (h->dev->xioctl(h->dev->ctx, s->dev_fd, DVF99_VOICE_DTMF, &d)) {
		TRACE(TRACE_HAL, "css: DVF99_VOICE_DTMF: %s", strerror(errno));
		return HAL_FATAL;
	}
	TRACE(TRACE_HAL, "css: line %u stream %u dtmf '%c' %u ms on css line %u",
	      line, stream, digit, d.duration_ms, s->cfg.line);

	return HAL_OK;
}

/* Every codec this backend carries uses an 8 kHz RTP timestamp clock, G.722 by
 * the RFC 3551 convention. */
#define CSS_RTP_CLOCK_HZ	8000u

/*
 * The CSS reports 0xffffffff for a value it has not measured. The schema never
 * reports an unmeasured zero, so such a field is left at 0 here and the
 * northbound keeps the last reading or shows "never". rtt and jitter are the
 * CSS raw figures; jitter_max and the far-end fields stay 0, the CSS reports
 * neither.
 */
static void css_stats_map(const struct dvf99_voice_stats *st,
			  struct hal_media_stats *out)
{
	memset(out, 0, sizeof(*out));
	out->packets_sent = st->pkts_sent;
	out->packets_recv = st->pkts_recvd;
	out->bytes_sent = st->bytes_sent;
	out->bytes_recv = st->bytes_recvd;
	if (st->pkts_lost != 0xffffffffu)
		out->packets_lost = st->pkts_lost;
	if (st->pkts_dropped != 0xffffffffu)
		out->packets_discarded = st->pkts_dropped;
	if (st->rtt != 0xffffffffu)
		out->rtt_avg = st->rtt;
	if (st->jitter != 0xffffffffu)
		out->jitter_avg = st->jitter;
}

struct css_emodel_codec {
	double ie;
	double bpl;
};

/*
 * The equipment impairment Ie and the packet-loss robustness Bpl of each codec,
 * from ITU-T G.113 Appendix I. G.711 is taken with packet-loss concealment.
 * G.722 and G.726 use the nearest documented narrow-band values, so a wideband
 * G.722 call reads as a clean narrow-band call and is not credited its full
 * wideband advantage.
 */
static struct css_emodel_codec css_emodel_params(uint32_t codec)
{
	switch (codec) {
	case DVF99_VOICE_CODEC_PCMU:
	case DVF99_VOICE_CODEC_PCMA:
		return (struct css_emodel_codec){ 0.0, 25.1 };
	case DVF99_VOICE_CODEC_G729:
		return (struct css_emodel_codec){ 11.0, 19.0 };
	case DVF99_VOICE_CODEC_G726_32:
		return (struct css_emodel_codec){ 7.0, 16.1 };
	case DVF99_VOICE_CODEC_G722:
	default:
		return (struct css_emodel_codec){ 0.0, 20.0 };
	}
}

/* ITU-T G.107 Annex B: the R factor to MOS conversion. */
static float css_r_to_mos(double r)
{
	if (r <= 0.0)
		return 1.0f;
	if (r >= 100.0)
		return 4.5f;
	return (float)(1.0 + 0.035 * r +
		       r * (r - 60.0) * (100.0 - r) * 7.0e-6);
}

/*
 * The CSS reports no opinion score, so the R factor and the two MOS values are
 * the ITU-T G.107 E-model estimate from the measured loss, the delay and the
 * codec. Random loss is assumed (burst ratio 1). The one-way delay is half the
 * round-trip time plus the packetisation; below 177 ms it adds almost nothing,
 * which is why the unknown jitter-buffer depth is left out. mos_lq drops the
 * delay term, mos_cq keeps it.
 */
static void css_emodel_fill(const struct css_stream *s,
			    struct hal_media_stats *out)
{
	struct css_emodel_codec p = css_emodel_params(s->cfg.codec);
	uint64_t seen = out->packets_recv + out->packets_lost;
	double ppl = seen ? 100.0 * (double)out->packets_lost / (double)seen : 0.0;
	double ie_eff = p.ie + (95.0 - p.ie) * ppl / (ppl + p.bpl);
	double d = (double)out->rtt_avg / 2.0 + (double)s->cfg.ptime_ms;
	double id = 0.024 * d + (d > 177.3 ? 0.11 * (d - 177.3) : 0.0);
	double r_lq = 93.2 - ie_eff;
	double r_cq = r_lq - id;
	double r_clamped = r_cq < 0.0 ? 0.0 : (r_cq > 100.0 ? 100.0 : r_cq);

	out->loss_rate = (uint64_t)(ppl + 0.5);
	out->rfactor = (uint32_t)(r_clamped + 0.5);
	out->mos_lq = css_r_to_mos(r_lq);
	out->mos_cq = css_r_to_mos(r_cq);
}

static enum hal_status css_stats_fetch(struct hal *h, struct css_stream *s,
				       struct hal_media_stats *out)
{
	struct dvf99_voice_stats st;

	if (s->dev_fd < 0)
		return HAL_UNSUPPORTED;

	memset(&st, 0, sizeof(st));
	if (h->dev->xioctl(h->dev->ctx, s->dev_fd, DVF99_VOICE_STATS, &st)) {
		TRACE(TRACE_HAL, "css: DVF99_VOICE_STATS: %s", strerror(errno));
		return HAL_FATAL;
	}
	css_stats_map(&st, out);
	css_emodel_fill(s, out);

	return HAL_OK;
}

enum hal_status css_stream_stats_get(struct hal *h, unsigned int line,
				     unsigned int stream,
				     struct hal_media_stats *out)
{
	struct css_stream *s = css_stream_get(h, line, stream);

	if (!s || !out)
		return HAL_UNSUPPORTED;

	return css_stats_fetch(h, s, out);
}

/*
 * The SLOW stats op. The CSS numbers are a device read, not a slow job, so the
 * backend reads them into the stream's record and posts the completion under a
 * token of its own; stream_stats_result_get then copies the record. The record
 * outlives a stop, so the core reads it back after the teardown.
 */
enum hal_status css_stream_stats_start(struct hal *h, unsigned int line,
				       unsigned int stream, struct hal_job *job)
{
	struct css_stream *s = css_stream_get(h, line, stream);
	enum hal_status st;

	if (!s || !job)
		return HAL_UNSUPPORTED;

	st = css_stats_fetch(h, s, &s->stats);
	if (st != HAL_OK)
		return st;

	job->token = hal_relay_stats_token_next(&h->relay);
	hal_relay_completion_post(&h->relay, job->token, HAL_OK);

	return HAL_OK;
}

enum hal_status css_stream_stats_result_get(struct hal *h, unsigned int line,
					    unsigned int stream,
					    struct hal_media_stats *out)
{
	struct css_stream *s = css_stream_get(h, line, stream);

	if (!s || !out)
		return HAL_UNSUPPORTED;

	*out = s->stats;

	return HAL_OK;
}

/*
 * The local conference, as the CSS mixes it: both streams of the line join the
 * one audio connection (spec 8.2). Attaching clears F_DETACHED on every running
 * stream of the line and pushes it; a later stream_config_set re-applies each
 * stream's own detach from its description when the conference ends.
 */
enum hal_status css_conf_set(struct hal *h, unsigned int line,
			     enum hal_conf_mode mode)
{
	bool on = mode == HAL_CONF_LOCAL_MIX;
	unsigned int stream;

	if (line >= CSS_LINES)
		return HAL_UNSUPPORTED;

	for (stream = 0; stream < CSS_STREAMS_PER_CHAN; stream++) {
		struct css_stream *s = &h->stream[line][stream];
		uint32_t flags = s->cfg.flags;

		if (!s->running)
			continue;
		if (on)
			flags &= ~(uint32_t)DVF99_VOICE_F_DETACHED;
		else
			flags |= DVF99_VOICE_F_DETACHED;
		if (flags == s->cfg.flags)
			continue;
		s->cfg.flags = flags;
		if (h->dev->xioctl(h->dev->ctx, s->dev_fd, DVF99_VOICE_UPDATE,
				   &s->cfg)) {
			TRACE(TRACE_HAL, "css: conf UPDATE: %s", strerror(errno));
			return HAL_FATAL;
		}
	}

	return HAL_OK;
}

/* --- the packet thread ---------------------------------------------------- */

static char css_event_char(uint8_t event)
{
	if (event <= 9)
		return (char)('0' + event);
	if (event == 10)
		return '*';
	if (event == 11)
		return '#';
	if (event <= 15)
		return (char)('A' + (event - 12));

	return '?';
}

/**
 * css_rtp_jitter_update() - apply one RFC 3550 A.8 interarrival jitter step
 * @s: stream whose receive jitter state is updated
 * @rtp_ts: RTP timestamp of the received packet
 * @arrival_units: arrival time of the packet in the RTP clock
 *
 * Return: nothing; s->jit holds 16 times the jitter in RTP clock units.
 */
void css_rtp_jitter_update(struct css_stream *s, uint32_t rtp_ts,
			   uint32_t arrival_units)
{
	int32_t transit = (int32_t)(arrival_units - rtp_ts);
	uint32_t j;
	int32_t d;

	if (!s->jit_have) {
		s->jit_transit = transit;
		s->jit_have = true;
		return;
	}

	d = transit - s->jit_transit;
	s->jit_transit = transit;
	if (d < 0)
		d = -d;

	j = atomic_load_explicit(&s->jit, memory_order_relaxed);
	j += (uint32_t)d - ((j + 8) >> 4);
	atomic_store_explicit(&s->jit, j, memory_order_relaxed);
}

/*
 * One record the CSS produced. A received DTMF or a first-receive event becomes
 * a HAL event; an outgoing RTP or RTCP datagram is protected and sent on the
 * matching socket. The DTMF is reported once, on its start row.
 */
static void css_pkt_from_device(struct hal *h, struct css_stream *s,
				unsigned int line, unsigned int stream)
{
	uint8_t buf[sizeof(struct dvf99_voice_pkt) + DVF99_VOICE_PKT_MAX +
		    UVSRTP_MAX_SRTCP_TRAILER];
	struct dvf99_voice_pkt *pkt = (void *)buf;
	long len = h->dev->xread(h->dev->ctx, s->dev_fd, buf, sizeof(buf));
	size_t plen;
	bool rtcp;
	int sock;

	if (len < (long)sizeof(*pkt))
		return;
	plen = (size_t)len - sizeof(*pkt);

	if (pkt->type == DVF99_VOICE_PKT_DTMF) {
		struct dvf99_voice_dtmf_rx *rx = (void *)pkt->data;
		struct hal_event ev = { .type = HAL_EVENT_DIGIT };

		if (plen < sizeof(*rx) || rx->end)
			return;
		ev.line = (uint16_t)line;
		ev.stream = (uint16_t)stream;
		ev.u.digit.digit = css_event_char(rx->event);
		ev.u.digit.remote = 1;
		css_event_emit(h, &ev);
		return;
	}
	if (pkt->type == DVF99_VOICE_PKT_EVENT) {
		struct dvf99_voice_event *de = (void *)pkt->data;
		struct hal_event ev = { .type = HAL_EVENT_RTP_FIRST_RX };

		if (plen < sizeof(*de) || de->type != DVF99_VOICE_EV_FIRST_RX)
			return;
		ev.line = (uint16_t)line;
		ev.stream = (uint16_t)stream;
		css_event_emit(h, &ev);
		return;
	}
	if (pkt->type != DVF99_VOICE_PKT_RTP &&
	    pkt->type != DVF99_VOICE_PKT_RTCP)
		return;

	rtcp = pkt->type == DVF99_VOICE_PKT_RTCP;
	sock = rtcp ? s->sock.rtcp_fd : s->sock.rtp_fd;
	if (sock < 0)
		return;
	if (s->srtp_live) {
		size_t out = 0;
		int rc = rtcp ?
			 uvsrtp_protect_rtcp(s->srtp, pkt->data, plen,
					     sizeof(buf) - sizeof(*pkt), &out) :
			 uvsrtp_protect(s->srtp, pkt->data, plen,
					sizeof(buf) - sizeof(*pkt), &out);

		if (rc)
			return;
		plen = out;
	}
	send(sock, pkt->data, plen, MSG_DONTWAIT | MSG_NOSIGNAL);
}

/* One datagram from the wire: unprotect and hand it to the CSS. A bad or
 * replayed SRTP datagram is dropped. */
static void css_pkt_from_socket(struct hal *h, struct css_stream *s, int sock,
				bool rtcp)
{
	uint8_t buf[sizeof(struct dvf99_voice_pkt) + DVF99_VOICE_PKT_MAX];
	struct dvf99_voice_pkt *pkt = (void *)buf;
	ssize_t len = recv(sock, pkt->data, DVF99_VOICE_PKT_MAX, MSG_DONTWAIT);
	struct timespec now;
	uint64_t arrival_us;
	uint32_t arrival_units;
	size_t plen;

	if (len <= 0)
		return;
	clock_gettime(CLOCK_MONOTONIC, &now);
	arrival_us = (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
	arrival_units = (uint32_t)(arrival_us * CSS_RTP_CLOCK_HZ / 1000000u);
	plen = (size_t)len;

	if (s->srtp_live) {
		size_t out = 0;
		int rc = rtcp ?
			 uvsrtp_unprotect_rtcp(s->srtp, pkt->data, plen,
					       DVF99_VOICE_PKT_MAX, &out) :
			 uvsrtp_unprotect(s->srtp, pkt->data, plen,
					  DVF99_VOICE_PKT_MAX, &out);

		if (rc)
			return;
		plen = out;
	}

	if (!rtcp && plen >= 12) {
		const uint8_t *r = pkt->data;
		uint32_t rtp_ts = (uint32_t)r[4] << 24 | (uint32_t)r[5] << 16 |
				  (uint32_t)r[6] << 8 | r[7];
		uint32_t ssrc = (uint32_t)r[8] << 24 | (uint32_t)r[9] << 16 |
				(uint32_t)r[10] << 8 | r[11];

		if (s->jit_have && ssrc != s->jit_ssrc)
			s->jit_have = false;
		s->jit_ssrc = ssrc;
		css_rtp_jitter_update(s, rtp_ts, arrival_units);
	}

	pkt->type = rtcp ? DVF99_VOICE_PKT_RTCP : DVF99_VOICE_PKT_RTP;
	pkt->ttl = 64;
	pkt->time_ms = (uint32_t)(arrival_us / 1000u);
	h->dev->xwrite(h->dev->ctx, s->dev_fd, buf, sizeof(*pkt) + plen);
}

#define CSS_PKT_FDS	(1 + CSS_LINES * CSS_STREAMS_PER_CHAN * 3)

static void *css_pkt_thread(void *arg)
{
	struct hal *h = arg;
	struct pollfd pfd[CSS_PKT_FDS];
	struct {
		struct css_stream *s;
		unsigned int line;
		unsigned int stream;
		int kind;		/* 0 device, 1 rtp socket, 2 rtcp socket */
	} map[CSS_PKT_FDS];

	for (;;) {
		unsigned int line, stream;
		int n = 1;
		int i;

		pthread_mutex_lock(&h->pkt_lock);
		if (h->pkt_quit) {
			pthread_mutex_unlock(&h->pkt_lock);
			break;
		}
		pfd[0].fd = h->pkt_wake[0];
		pfd[0].events = POLLIN;
		for (line = 0; line < CSS_LINES; line++)
			for (stream = 0; stream < CSS_STREAMS_PER_CHAN; stream++) {
				struct css_stream *s = &h->stream[line][stream];
				int kind;

				if (!s->running || s->dev_fd < 0)
					continue;
				for (kind = 0; kind < 3; kind++) {
					pfd[n].fd = kind == 0 ? s->dev_fd :
						    kind == 1 ? s->sock.rtp_fd :
								s->sock.rtcp_fd;
					pfd[n].events = POLLIN;
					map[n].s = s;
					map[n].line = line;
					map[n].stream = stream;
					map[n].kind = kind;
					n++;
				}
			}
		h->pkt_acked = h->pkt_gen;
		pthread_cond_broadcast(&h->pkt_cond);
		pthread_mutex_unlock(&h->pkt_lock);

		if (poll(pfd, (nfds_t)n, 100) <= 0)
			continue;

		if (pfd[0].revents & POLLIN) {
			char drain[64];

			while (read(h->pkt_wake[0], drain, sizeof(drain)) > 0)
				;
		}

		for (i = 1; i < n; i++) {
			if (!(pfd[i].revents & POLLIN))
				continue;
			if (map[i].kind == 0)
				css_pkt_from_device(h, map[i].s, map[i].line,
						    map[i].stream);
			else
				css_pkt_from_socket(h, map[i].s, pfd[i].fd,
						    map[i].kind == 2);
		}
	}

	return NULL;
}

int css_pkt_start(struct hal *h)
{
	if (pipe2(h->pkt_wake, O_NONBLOCK | O_CLOEXEC)) {
		h->pkt_wake[0] = h->pkt_wake[1] = -1;
		return -1;
	}
	pthread_mutex_init(&h->pkt_lock, NULL);
	pthread_cond_init(&h->pkt_cond, NULL);
	h->pkt_gen = 0;
	h->pkt_acked = 0;
	h->pkt_quit = false;

	if (pthread_create(&h->pkt_tid, NULL, css_pkt_thread, h)) {
		close(h->pkt_wake[0]);
		close(h->pkt_wake[1]);
		h->pkt_wake[0] = h->pkt_wake[1] = -1;
		pthread_cond_destroy(&h->pkt_cond);
		pthread_mutex_destroy(&h->pkt_lock);
		return -1;
	}
	h->pkt_alive = true;

	return 0;
}

void css_pkt_stop(struct hal *h)
{
	char b = 1;

	if (!h->pkt_alive)
		return;

	pthread_mutex_lock(&h->pkt_lock);
	h->pkt_quit = true;
	pthread_mutex_unlock(&h->pkt_lock);
	while (write(h->pkt_wake[1], &b, 1) < 0 && errno == EINTR)
		;
	pthread_join(h->pkt_tid, NULL);

	close(h->pkt_wake[0]);
	close(h->pkt_wake[1]);
	h->pkt_wake[0] = h->pkt_wake[1] = -1;
	pthread_cond_destroy(&h->pkt_cond);
	pthread_mutex_destroy(&h->pkt_lock);
	h->pkt_alive = false;
}
