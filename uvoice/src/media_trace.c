// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The media trace domains. See media_trace.h for the contracts.
 */
#include <stdio.h>
#include <string.h>

#include <libubox/uloop.h>

#include "calltrace.h"
#include "hal/hal.h"
#include "media_trace.h"
#include "status.h"
#include "trace.h"
#include "uvoice.h"

/* One sweep per second. The counters are cumulative, so a slower cadence loses
 * nothing but resolution and a faster one costs a HAL call per stream per
 * tick for a number that barely moved. */
#define MEDIA_TRACE_PERIOD_MS	1000
/* The two halves of one line. Either may be absent, so each is built into its
 * own buffer and the line is what the two of them make. */
/* The worst-case line with every counter at its widest is 435 bytes.
 * Truncation is safe but loses the RTP tail of the line. */
#define MEDIA_TRACE_JB_MAX	512
#define MEDIA_TRACE_TAIL_MAX	96

static struct uloop_timeout media_timer;

/*
 * The two ops fail apart, so each half is built on its own, and the line is
 * written when either one spoke. A backend that answers neither produces
 * nothing for its streams; the stub answers the media half for every stream
 * and the jitter buffer half for one; a live udsp stream answers both, and a
 * stream of that same backend that has not ticked yet answers only the media
 * half. A stream is worth a line as soon as one source has numbers for it.
 *
 * A half that is absent is omitted, not written as zeros. Zeros read as a
 * healthy buffer with nothing to report, which is the opposite of "this
 * backend cannot tell you", and it is the same rule the northbound row skip
 * and hal.h both carry.
 */
static bool media_rtp_tail(unsigned int line, unsigned int stream, char *buf,
			   size_t len)
{
	struct hal_media_stats st;

	buf[0] = '\0';
	if (!hal_backend_ops.stream_stats_get)
		return false;

	memset(&st, 0, sizeof(st));
	if (hal_backend_ops.stream_stats_get(uv.hal, line, stream, &st) != HAL_OK)
		return false;

	snprintf(buf, len, " rtp recv=%llu lost=%llu jit=%llu/%llu",
		 (unsigned long long)st.packets_recv,
		 (unsigned long long)st.packets_lost,
		 (unsigned long long)st.jitter_avg,
		 (unsigned long long)st.jitter_max);

	return true;
}

static bool media_jb_text(unsigned int line, unsigned int stream, char *buf,
			  size_t len)
{
	struct hal_jb_stats jb;

	buf[0] = '\0';
	if (!hal_backend_ops.stream_jb_stats_get)
		return false;

	memset(&jb, 0, sizeof(jb));
	if (hal_backend_ops.stream_jb_stats_get(uv.hal, line, stream, &jb) !=
	    HAL_OK)
		return false;

	snprintf(buf, len,
		 " jb out=%llu plc=%llu cn=%llu sil=%llu late=%llu dup=%llu "
		 "hold=%llu ovf=%llu resync=%llu starve=%llu disc=%llu "
		 "cnre=%llu "
		 "under=%llu "
		 "sdrop=%llu sins=%llu tgt=%u depth=%u jit=%u free=%u mode=%s",
		 (unsigned long long)jb.frames_out, (unsigned long long)jb.plc,
		 (unsigned long long)jb.cn, (unsigned long long)jb.silence,
		 (unsigned long long)jb.late_drop, (unsigned long long)jb.dup_drop,
		 (unsigned long long)jb.hold_drop,
		 (unsigned long long)jb.overflow_drop,
		 (unsigned long long)jb.resyncs,
		 (unsigned long long)jb.starve_resyncs,
		 (unsigned long long)jb.resync_discard_ms,
		 (unsigned long long)jb.cn_reanchors,
		 (unsigned long long)jb.underruns,
		 (unsigned long long)jb.skew_drops,
		 (unsigned long long)jb.skew_inserts,
		 jb.target_ms, jb.depth_ms, jb.jitter_ms, jb.pool_free,
		 hal_jb_mode_name(jb.mode));

	return true;
}

static void media_stream_trace(unsigned int line, unsigned int stream)
{
	char jbtext[MEDIA_TRACE_JB_MAX];
	char tail[MEDIA_TRACE_TAIL_MAX];
	bool have_jb = media_jb_text(line, stream, jbtext, sizeof(jbtext));
	bool have_rtp = media_rtp_tail(line, stream, tail, sizeof(tail));

	if (!have_jb && !have_rtp)
		return;

	TRACE(TRACE_MEDIA, "l%u s%u%s%s", line, stream, jbtext, tail);
}

/*
 * The canceller rows of one line, once per sweep. The status reply carries the
 * same reading at the cadence a caller polls it; an echo investigation needs
 * the second by second shape of the convergence, which only a sampled line
 * gives. The first line carries the fields that move: the two switch words,
 * the measured return loss, the reference delay state, the retimes behind it,
 * the policy kind the engine ran the frame under, the open loop and the peak
 * tap.
 *
 * The processor verdicts take a second line, because the first line must stay
 * readable in a live log. The counts are cumulative, so the difference between
 * two lines covers the second between them, and frames lets a reader form the
 * duty of that second. Both lines start with the line and "lec", so one grep
 * finds both.
 */
static void media_lec_trace(unsigned int line)
{
	struct hal_lec_stats lec;

	if (!hal_backend_ops.line_lec_stats_get)
		return;

	memset(&lec, 0, sizeof(lec));
	if (hal_backend_ops.line_lec_stats_get(uv.hal, line, &lec) != HAL_OK)
		return;

	TRACE(TRACE_MEDIA,
	      "l%u lec ec=%s nlp=%s erle_mdb=%d ref_ready=%u retimes=%u "
	      "kind=%s loop_open=%u peak_tap=%u",
	      line, status_lec_ec_word(&lec), status_lec_nlp_word(&lec),
	      (int)lec.erle_mdb, lec.ref_ready, lec.retimes,
	      status_lec_kind_word(lec.passthrough), lec.loop_open,
	      lec.coef_peak_tap);
	TRACE(TRACE_MEDIA,
	      "l%u lec nlp frames=%u active=%u wait=%u erle=%u unlike=%u dt=%u "
	      "quiet=%u",
	      line, lec.frames, lec.nlp_active_frames, lec.nlp_wait_frames,
	      lec.nlp_erle_frames, lec.nlp_unlike_frames, lec.nlp_dt_frames,
	      lec.nlp_quiet_frames);
}

static void media_trace_sweep(struct uloop_timeout *t)
{
	const struct hal_cap *cap;
	unsigned int line, stream;

	uloop_timeout_set(&media_timer, MEDIA_TRACE_PERIOD_MS);
	/* Neither op is required: the sweep asks for both per stream and each
	 * half stands on its own, so a backend that carries one of them still
	 * produces lines. */
	if (!uv.hal || uv.hal_state != UVOICE_HAL_READY)
		return;

	cap = hal_backend_ops.cap_get(uv.hal);
	for (line = 0; line < cap->lines; line++) {
		for (stream = 0; stream < cap->streams_per_chan; stream++)
			media_stream_trace(line, stream);

		media_lec_trace(line);
	}
}

/* The states that hold a backend to push to. Listed, because FAILED sorts
 * after READY in enum uvoice_hal_state. */
static bool hal_state_up(void)
{
	return uv.hal_state == UVOICE_HAL_ATTACHED ||
	       uv.hal_state == UVOICE_HAL_STARTING ||
	       uv.hal_state == UVOICE_HAL_READY;
}

/*
 * The backend mask is pushed on every sync, not only on a change, because the
 * backend this daemon talks to may not be the one the last sync reached: a
 * DSP that came up after the domain was switched on was never told.
 * hal_ops.debug_set is idempotent by contract, so a repeat costs one call.
 */
void media_trace_sync(void)
{
	uint32_t mask = 0u;

	if (trace_domain_active(TRACE_JB))
		mask |= HAL_DEBUG_JB_TRACE;
	if (trace_domain_active(TRACE_HAL))
		mask |= HAL_DEBUG_SLIC_LOG;
	/* The media datagrams and the canceller frames belong to the call trace
	 * and to nothing else: no trace domain carries them, because they go
	 * to a ring and never to the log. */
	if (calltrace_capturing())
		mask |= HAL_DEBUG_PKT_TRACE;
	if (calltrace_lec_capturing())
		mask |= HAL_DEBUG_LEC_CAPTURE;

	if (uv.hal && hal_state_up() && hal_backend_ops.debug_set)
		hal_backend_ops.debug_set(uv.hal, mask);

	if (!trace_domain_active(TRACE_MEDIA)) {
		uloop_timeout_cancel(&media_timer);
		return;
	}

	/* An already armed sweep keeps its place in the second it is in, so a
	 * second toggle cannot push the next line out by another period. */
	if (!media_timer.pending)
		uloop_timeout_set(&media_timer, MEDIA_TRACE_PERIOD_MS);
}

/* The three domains the backend hears about and no other: a hook that acted
 * on every domain would call the HAL each time an operator turned the SIP
 * trace on. */
static void media_trace_hook(enum trace_domain dom, bool on)
{
	if (dom != TRACE_MEDIA && dom != TRACE_JB && dom != TRACE_HAL)
		return;

	media_trace_sync();
}

void media_trace_init(void)
{
	media_timer.cb = media_trace_sweep;
	trace_hook_set(media_trace_hook);
}

void media_trace_done(void)
{
	uloop_timeout_cancel(&media_timer);
}
