// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the stub HAL backend (src/hal/stub/stub.c). The tests cover
 * two getters and the capability word that the open path publishes.
 * line_lec_stats_get reads the line canceller state, and stream_jb_stats_get
 * reads the counters of the receive jitter buffer.
 *
 * Isolation. The real backend is compiled into this binary. A case drives
 * hal_backend_ops end to end, with nothing between the ops table and the
 * records that the backend keeps. The getter cases need no device and no
 * event pump. A case opens the backend on a socketpair, programs the channel
 * and the streams, and reads the two getters back.
 *
 * The cases check the answer of each state, not the numbers. The stub runs no
 * canceller and no jitter buffer, so both records are canned. A caller
 * depends on which state answers, which state refuses, and which of the three
 * canceller rows a line reads. The cases still check the canned values, with
 * one helper for each record: lec_canned_check() and jb_canned_check().
 * Reason: without them, a row that carried the fill of another line or
 * stream would read as correct.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <libubox/uloop.h>

#include "hal/hal.h"
#include "trace.h"

#include "test_util.h"
#include "test_uloop.h"

struct probe {
	struct hal *h;
	int rx_fd;
	int tx_fd;
};

/*
 * The core owns the event transport (hal.h section 2), so the suite makes the
 * socketpair and hands the write end to the backend. Nothing reads the core
 * end here: these ops answer from cached state and emit no record.
 */
static bool probe_open(struct probe *p)
{
	int fd[2];

	memset(p, 0, sizeof(*p));
	p->rx_fd = p->tx_fd = -1;

	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fd))
		return false;

	p->rx_fd = fd[0];
	p->tx_fd = fd[1];
	p->h = hal_backend_ops.open(p->tx_fd);
	if (p->h)
		return true;

	/* An open that failed leaves no handle to close the pair with, so it
	 * goes here or the descriptors outlive the case. */
	close(p->rx_fd);
	close(p->tx_fd);
	p->rx_fd = p->tx_fd = -1;

	return false;
}

static void probe_close(struct probe *p)
{
	if (p->h)
		hal_backend_ops.close(p->h);
	p->h = NULL;

	if (p->rx_fd >= 0)
		close(p->rx_fd);
	if (p->tx_fd >= 0)
		close(p->tx_fd);
	p->rx_fd = p->tx_fd = -1;
}

/*
 * The backend's own trace line, taken through the sink the call trace uses
 * (trace.h trace_sink_set). The ring and the loop ops report what the core
 * asked for and this suite reads no ubus dump, so the sink is where a case
 * reads the arguments back.
 */
static char trace_last[256];

static void trace_capture(enum trace_domain dom, const char *msg)
{
	snprintf(trace_last, sizeof(trace_last), "%s", msg);
}

/* The stub completes a slow op 20 ms after it accepts it (stub.c
 * STUB_JOB_DELAY_MS), and this suite links no core job list, so a case that
 * starts one drains it on the loop itself. */
#define STUB_JOB_WAIT_MS	100

/* Every record waiting on the core end of the event socket, and whether one of
 * them carries this type. The pair is non-blocking, so the drain ends on the
 * first refusal. */
static bool ev_drain_has(struct probe *p, uint16_t type)
{
	struct hal_event ev;
	bool found = false;

	while (recv(p->rx_fd, &ev, sizeof(ev), MSG_DONTWAIT) ==
	       (ssize_t)sizeof(ev)) {
		if (ev.type == type)
			found = true;
	}

	return found;
}

/*
 * The canceller half of a channel view. The two switches are separate,
 * because the filter and the processor are separate answers. The tail is the
 * one the canned row reports, so a case reads one tail across the
 * configuration and the record.
 */
static void chan_build(struct hal_channel_cfg *cfg, bool ec_enable,
		       bool nlp_enable)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->detect_mask = HAL_DETECT_DTMF;
	cfg->ec_enable = ec_enable;
	cfg->ec_nlp_enable = nlp_enable;
	cfg->ec_tail_ms = 48;
}

static void media_build(struct hal_media *m, uint8_t codec, uint8_t passthrough,
			uint8_t jb_mode)
{
	memset(m, 0, sizeof(*m));
	m->local.ipver = 4;
	m->local.addr[0] = 127;
	m->local.addr[3] = 1;
	m->local.port = 40000;
	m->remote = m->local;
	m->remote.port = 40002;
	m->codec = codec;
	m->direction = HAL_MEDIA_SENDRECV;
	m->ptime_ms = 20;
	m->jb_mode = jb_mode;
	m->passthrough = passthrough;
	m->rtcp_enable = true;
}

static void fax_build(struct hal_fax_profile *fax, uint8_t mode)
{
	memset(fax, 0, sizeof(*fax));
	fax->mode = mode;
	fax->codec = HAL_CODEC_PCMU;
	fax->ec_disable = true;
	fax->vad_disable = true;
	fax->cng_disable = true;
	fax->silence_comp_disable = true;
	fax->jb_fixed = true;
}

/* The kind and the three flags. They are named at the call site, because a
 * helper that derived them would repeat the rule under test. */
static void lec_flags_check(const struct hal_lec_stats *lec, unsigned int ec,
			    unsigned int nlp_enabled, unsigned int nlp_active,
			    unsigned int kind)
{
	CHECK_UINT(lec->ec_active, ec);
	CHECK_UINT(lec->nlp_enabled, nlp_enabled);
	CHECK_UINT(lec->nlp_active, nlp_active);
	CHECK_UINT(lec->passthrough, kind);
}

/* The canned canceller record, which every answering row carries, and the per
 * line offset on frames. The processor-off remainder follows that offset, so
 * the verdicts must sum under frames on every line. */
static void lec_canned_check(const struct hal_lec_stats *lec, unsigned int line)
{
	CHECK_UINT(lec->tail_ms, 48);
	CHECK_UINT(lec->taps, 384);
	CHECK_UINT(lec->frames, 6000u + line * 100u);
	CHECK_UINT(lec->hold_frames, 37);
	CHECK_UINT(lec->skip_frames, 11);
	CHECK_UINT(lec->retimes, 5);
	CHECK_UINT(lec->nlp_active_frames, 4944);
	CHECK_UINT(lec->nlp_wait_frames, 91);
	CHECK_UINT(lec->nlp_erle_frames, 310);
	CHECK_UINT(lec->nlp_unlike_frames, 470);
	CHECK_UINT(lec->nlp_dt_frames, 17);
	CHECK_UINT(lec->nlp_quiet_frames, 19);
	CHECK_UINT(lec->coef_peak_tap, 84);
	CHECK_UINT(lec->loop_open, 0);
	CHECK_UINT(lec->frames - lec->nlp_active_frames - lec->nlp_wait_frames -
		   lec->nlp_erle_frames - lec->nlp_unlike_frames -
		   lec->nlp_dt_frames - lec->nlp_quiet_frames,
		   149u + line * 100u);
	CHECK_UINT(lec->diverge_resets, 2);
	CHECK_UINT(lec->probes, 3);
	CHECK_UINT(lec->erle_mdb, 18500);
	CHECK_UINT(lec->coef_energy_micro, 62500);
	CHECK_UINT(lec->echo_gain_micro, 41000);
	CHECK_UINT(lec->double_talk, 1);
	CHECK_UINT(lec->ref_ready, 1);
	CHECK_UINT(lec->ref_delay_samples, 240);
}

/* The canned buffer record and the per pair offset on frames_out. */
static void jb_canned_check(const struct hal_jb_stats *jb, unsigned int line,
			    unsigned int stream)
{
	CHECK_UINT(jb->frames_out, 4800u + line * 100u + stream * 10u);
	CHECK_UINT(jb->plc, 12);
	CHECK_UINT(jb->hold_drop, 11);
	CHECK_UINT(jb->resync_discard_ms, 13);
	CHECK_UINT(jb->target_ms, 50);
	CHECK_UINT(jb->pool_free, 57);
}

/*
 * The instance that the two getters read, line_lec_stats_get and
 * stream_jb_stats_get. The backend answers a line that made a canceller and
 * runs a media plane, and refuses every other line. These cases drive the
 * states of one stream: no stream, a configured and started stream, and the
 * two channel switches under it. They also drive the stop, and a
 * configuration that never made an instance. Line 1 and stream 1 carry the
 * cases, and the canned records offset frames by line and frames_out by pair.
 * A row that picked up line 0 or stream 0 would read as correct on line 0
 * alone.
 */
static void test_lec_instance(void)
{
	struct hal_channel_cfg ccfg;
	struct hal_lec_stats lec;
	struct hal_jb_stats jb;
	struct hal_media m;
	struct probe p;

	if (!probe_open(&p)) {
		CASE("the canceller instance case needs an open backend");
		CHECK(false);
		return;
	}

	memset(&lec, 0xa5, sizeof(lec));
	memset(&jb, 0xa5, sizeof(jb));
	chan_build(&ccfg, true, true);
	media_build(&m, HAL_CODEC_PCMU, HAL_PASSTHROUGH_OFF, HAL_JB_ADAPTIVE);

	CASE("a line with no stream answers neither surface");
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 1, &lec) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.stream_jb_stats_get(p.h, 1, 1, &jb) ==
	      HAL_UNSUPPORTED);

	CASE("a started stream reads the voice row and the adaptive buffer");
	CHECK(hal_backend_ops.channel_config_set(p.h, 1, &ccfg) == HAL_OK);
	CHECK(hal_backend_ops.stream_config_set(p.h, 1, 1, &m) == HAL_OK);
	CHECK(hal_backend_ops.stream_start(p.h, 1, 1) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 1, &lec) == HAL_OK);
	lec_flags_check(&lec, 1, 1, 1, HAL_PASSTHROUGH_OFF);
	lec_canned_check(&lec, 1);
	CHECK(hal_backend_ops.stream_jb_stats_get(p.h, 1, 1, &jb) == HAL_OK);
	CHECK_UINT(jb.mode, HAL_JB_ADAPTIVE);
	jb_canned_check(&jb, 1, 1);
	/* The instance belongs to one line. Line 0 of this board made none, so
	 * an op that indexed the wrong line would answer for both. */
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) ==
	      HAL_UNSUPPORTED);

	CASE("a channel that switches the canceller off keeps the instance");
	/* The filter ran nothing, so the three flags are clear. The line still
	 * carries what its streams carry: the switch is no kind of its own. */
	chan_build(&ccfg, false, true);
	CHECK(hal_backend_ops.channel_config_set(p.h, 1, &ccfg) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 1, &lec) == HAL_OK);
	lec_flags_check(&lec, 0, 0, 0, HAL_PASSTHROUGH_OFF);
	lec_canned_check(&lec, 1);

	CASE("a switch off under the modem kind keeps the kind");
	media_build(&m, HAL_CODEC_PCMU, HAL_PASSTHROUGH_MODEM, HAL_JB_ADAPTIVE);
	CHECK(hal_backend_ops.stream_config_set(p.h, 1, 1, &m) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 1, &lec) == HAL_OK);
	lec_flags_check(&lec, 0, 0, 0, HAL_PASSTHROUGH_MODEM);
	media_build(&m, HAL_CODEC_PCMU, HAL_PASSTHROUGH_OFF, HAL_JB_ADAPTIVE);
	CHECK(hal_backend_ops.stream_config_set(p.h, 1, 1, &m) == HAL_OK);

	CASE("a channel that asks for the canceller again reads the voice row");
	chan_build(&ccfg, true, true);
	CHECK(hal_backend_ops.channel_config_set(p.h, 1, &ccfg) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 1, &lec) == HAL_OK);
	lec_flags_check(&lec, 1, 1, 1, HAL_PASSTHROUGH_OFF);

	CASE("a channel that asks for no processor keeps the filter");
	/* The filter runs and the processor does not, which is the switch
	 * hal_lec_stats names beside the fax policy. */
	chan_build(&ccfg, true, false);
	CHECK(hal_backend_ops.channel_config_set(p.h, 1, &ccfg) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 1, &lec) == HAL_OK);
	lec_flags_check(&lec, 1, 0, 0, HAL_PASSTHROUGH_OFF);
	chan_build(&ccfg, true, true);
	CHECK(hal_backend_ops.channel_config_set(p.h, 1, &ccfg) == HAL_OK);

	CASE("a stopped stream takes both surfaces away");
	CHECK(hal_backend_ops.stream_stop(p.h, 1, 1) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 1, &lec) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.stream_jb_stats_get(p.h, 1, 1, &jb) ==
	      HAL_UNSUPPORTED);

	CASE("a configuration with the canceller off makes no instance");
	chan_build(&ccfg, false, true);
	media_build(&m, HAL_CODEC_PCMU, HAL_PASSTHROUGH_OFF, HAL_JB_OFF);
	CHECK(hal_backend_ops.channel_config_set(p.h, 1, &ccfg) == HAL_OK);
	CHECK(hal_backend_ops.stream_config_set(p.h, 1, 1, &m) == HAL_OK);
	CHECK(hal_backend_ops.stream_start(p.h, 1, 1) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 1, &lec) ==
	      HAL_UNSUPPORTED);
	/* The buffer lives under the running stream alone, so it answers where
	 * the canceller does not, and it carries the mode this description
	 * asked for. */
	CHECK(hal_backend_ops.stream_jb_stats_get(p.h, 1, 1, &jb) == HAL_OK);
	CHECK_UINT(jb.mode, HAL_JB_OFF);

	CHECK(hal_backend_ops.stream_stop(p.h, 1, 1) == HAL_OK);
	probe_close(&p);
}

/*
 * The instance outlives one stream and dies with the last description. This
 * is the lifetime in libudsp. The release counts every stream that is not
 * idle, so a stream that was configured and never started holds the line.
 */
static void test_lec_lifetime(void)
{
	struct hal_channel_cfg ccfg;
	struct hal_lec_stats lec;
	struct hal_media m;
	struct probe p;

	if (!probe_open(&p)) {
		CASE("the canceller lifetime case needs an open backend");
		CHECK(false);
		return;
	}

	memset(&lec, 0xa5, sizeof(lec));
	chan_build(&ccfg, true, true);
	media_build(&m, HAL_CODEC_PCMU, HAL_PASSTHROUGH_OFF, HAL_JB_ADAPTIVE);
	CHECK(hal_backend_ops.channel_config_set(p.h, 0, &ccfg) == HAL_OK);

	CASE("a stop leaves the instance while another stream is described");
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_OK);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 1, &m) == HAL_OK);
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	/* No stream runs, so the media plane is gone and the row waits. */
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.stream_start(p.h, 0, 1) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	lec_flags_check(&lec, 1, 1, 1, HAL_PASSTHROUGH_OFF);

	CASE("a stop of a stream that was never described keeps the instance");
	/* The last description goes first, so the instance goes with it. */
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 1) == HAL_OK);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	/* Stream 1 holds no description now, and a stop of it answers ok and
	 * takes nothing away. */
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 1) == HAL_OK);
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	lec_flags_check(&lec, 1, 1, 1, HAL_PASSTHROUGH_OFF);

	CASE("a start after a stop needs a new description");
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_FATAL);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	lec_flags_check(&lec, 1, 1, 1, HAL_PASSTHROUGH_OFF);

	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	probe_close(&p);
}

/*
 * The three rows of a line that holds an instance. The fax profile and the
 * description are the two sources of a kind, and each one ends the other.
 */
static void test_lec_kinds(void)
{
	struct hal_channel_cfg ccfg;
	struct hal_fax_profile fax;
	struct hal_lec_stats lec;
	struct hal_jb_stats jb;
	struct hal_media m;
	struct probe p;

	if (!probe_open(&p)) {
		CASE("the canceller kind case needs an open backend");
		CHECK(false);
		return;
	}

	memset(&lec, 0xa5, sizeof(lec));
	memset(&jb, 0xa5, sizeof(jb));
	chan_build(&ccfg, true, true);
	media_build(&m, HAL_CODEC_PCMU, HAL_PASSTHROUGH_OFF, HAL_JB_ADAPTIVE);
	CHECK(hal_backend_ops.channel_config_set(p.h, 0, &ccfg) == HAL_OK);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_OK);

	CASE("a fax passthrough profile reads the bypassed modem row");
	fax_build(&fax, HAL_FAX_PASSTHROUGH);
	CHECK(hal_backend_ops.fax_profile_apply(p.h, 0, 0, &fax) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	lec_flags_check(&lec, 0, 0, 0, HAL_PASSTHROUGH_MODEM);
	lec_canned_check(&lec, 0);

	CASE("a description after the profile takes the kind back");
	/* hal.h leaves the kind of every later programming on the description,
	 * so a plain one ends the profile. */
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	lec_flags_check(&lec, 1, 1, 1, HAL_PASSTHROUGH_OFF);

	CASE("a profile that goes off reads the voice row again");
	fax_build(&fax, HAL_FAX_PASSTHROUGH);
	CHECK(hal_backend_ops.fax_profile_apply(p.h, 0, 0, &fax) == HAL_OK);
	fax_build(&fax, HAL_FAX_OFF);
	CHECK(hal_backend_ops.fax_profile_apply(p.h, 0, 0, &fax) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	lec_flags_check(&lec, 1, 1, 1, HAL_PASSTHROUGH_OFF);

	CASE("a call after a fax call reads the voice row");
	/* The profile dies with the leg that took it, so the next call on the
	 * line starts from the voice policy. */
	fax_build(&fax, HAL_FAX_PASSTHROUGH);
	CHECK(hal_backend_ops.fax_profile_apply(p.h, 0, 0, &fax) == HAL_OK);
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	lec_flags_check(&lec, 1, 1, 1, HAL_PASSTHROUGH_OFF);

	CASE("a description with the modem kind bypasses the filter");
	media_build(&m, HAL_CODEC_PCMU, HAL_PASSTHROUGH_MODEM, HAL_JB_FIXED);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	lec_flags_check(&lec, 0, 0, 0, HAL_PASSTHROUGH_MODEM);
	/* The mode of the record is the mode the description asked for, so a
	 * buffer pinned for the carrier reads back here. */
	CHECK(hal_backend_ops.stream_jb_stats_get(p.h, 0, 0, &jb) == HAL_OK);
	CHECK_UINT(jb.mode, HAL_JB_FIXED);
	jb_canned_check(&jb, 0, 0);

	CASE("a description with the fax kind keeps the filter alone");
	media_build(&m, HAL_CODEC_PCMU, HAL_PASSTHROUGH_FAX, HAL_JB_ADAPTIVE);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	lec_flags_check(&lec, 1, 0, 0, HAL_PASSTHROUGH_FAX);
	CHECK(hal_backend_ops.stream_jb_stats_get(p.h, 0, 0, &jb) == HAL_OK);
	CHECK_UINT(jb.mode, HAL_JB_ADAPTIVE);

	CASE("a description that carries no kind reads the voice row again");
	media_build(&m, HAL_CODEC_PCMU, HAL_PASSTHROUGH_OFF, HAL_JB_ADAPTIVE);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	lec_flags_check(&lec, 1, 1, 1, HAL_PASSTHROUGH_OFF);

	CASE("a T.38 description reads the modem kind");
	media_build(&m, HAL_CODEC_T38, HAL_PASSTHROUGH_OFF, HAL_JB_ADAPTIVE);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	lec_flags_check(&lec, 0, 0, 0, HAL_PASSTHROUGH_MODEM);

	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	probe_close(&p);
}

/*
 * One canceller serves the whole line, so two streams of one line reach one
 * row. The modem kind wins over the fax kind, and a stream on local hold
 * carries neither.
 */
static void test_lec_two_streams(void)
{
	struct hal_channel_cfg ccfg;
	struct hal_lec_stats lec;
	struct hal_media held;
	struct hal_media m;
	struct probe p;

	if (!probe_open(&p)) {
		CASE("the two stream case needs an open backend");
		CHECK(false);
		return;
	}

	memset(&lec, 0xa5, sizeof(lec));
	chan_build(&ccfg, true, true);
	CHECK(hal_backend_ops.channel_config_set(p.h, 0, &ccfg) == HAL_OK);

	CASE("a held stream governs no kind");
	media_build(&held, HAL_CODEC_PCMU, HAL_PASSTHROUGH_MODEM,
		    HAL_JB_ADAPTIVE);
	held.tx_silence = true;
	media_build(&m, HAL_CODEC_PCMU, HAL_PASSTHROUGH_OFF, HAL_JB_ADAPTIVE);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &held) == HAL_OK);
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_OK);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 1, &m) == HAL_OK);
	CHECK(hal_backend_ops.stream_start(p.h, 0, 1) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	lec_flags_check(&lec, 1, 1, 1, HAL_PASSTHROUGH_OFF);
	/* The resume puts the carrier back on the line, so the kind returns. */
	held.tx_silence = false;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &held) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	lec_flags_check(&lec, 0, 0, 0, HAL_PASSTHROUGH_MODEM);

	CASE("the modem kind wins over the fax kind whichever stream holds it");
	media_build(&m, HAL_CODEC_PCMU, HAL_PASSTHROUGH_FAX, HAL_JB_ADAPTIVE);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	media_build(&m, HAL_CODEC_PCMU, HAL_PASSTHROUGH_MODEM, HAL_JB_ADAPTIVE);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 1, &m) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	lec_flags_check(&lec, 0, 0, 0, HAL_PASSTHROUGH_MODEM);
	/* The other order, so a rule that took the last stream it read would
	 * answer the fax kind here. */
	media_build(&m, HAL_CODEC_PCMU, HAL_PASSTHROUGH_MODEM, HAL_JB_ADAPTIVE);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	media_build(&m, HAL_CODEC_PCMU, HAL_PASSTHROUGH_FAX, HAL_JB_ADAPTIVE);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 1, &m) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	lec_flags_check(&lec, 0, 0, 0, HAL_PASSTHROUGH_MODEM);

	CASE("a line with one stream left reads that stream's kind");
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	lec_flags_check(&lec, 1, 0, 0, HAL_PASSTHROUGH_FAX);
	lec_canned_check(&lec, 0);

	CASE("the last stream of the line takes the row away");
	CHECK(hal_backend_ops.stream_stop(p.h, 0, 1) == HAL_OK);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) ==
	      HAL_UNSUPPORTED);

	probe_close(&p);
}

/*
 * The arguments that neither getter accepts. The line runs a canceller and a
 * stream first, so a refusal here is the argument check and not the state.
 */
static void test_stats_refusals(void)
{
	struct hal_channel_cfg ccfg;
	struct hal_lec_stats lec;
	struct hal_jb_stats jb;
	const struct hal_cap *cap;
	struct hal_media m;
	struct probe p;

	if (!probe_open(&p)) {
		CASE("the refusal case needs an open backend");
		CHECK(false);
		return;
	}

	memset(&lec, 0xa5, sizeof(lec));
	memset(&jb, 0xa5, sizeof(jb));
	chan_build(&ccfg, true, true);
	media_build(&m, HAL_CODEC_PCMU, HAL_PASSTHROUGH_OFF, HAL_JB_ADAPTIVE);
	cap = hal_backend_ops.cap_get(p.h);
	CHECK(hal_backend_ops.channel_config_set(p.h, 0, &ccfg) == HAL_OK);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_OK);

	CASE("a line outside the cap and a null record answer unsupported");
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, cap->lines, &lec) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, HAL_LINE_MAX, &lec) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, NULL) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.stream_jb_stats_get(p.h, cap->lines, 0, &jb) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.stream_jb_stats_get(p.h, 0,
						  cap->streams_per_chan,
						  &jb) == HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.stream_jb_stats_get(p.h, 0, HAL_STREAM_MAX,
						  &jb) == HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.stream_jb_stats_get(p.h, 0, 0, NULL) ==
	      HAL_UNSUPPORTED);
	/* The same line answers both getters with the arguments it accepts, so
	 * the refusals above are the arguments and nothing else. */
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	CHECK(hal_backend_ops.stream_jb_stats_get(p.h, 0, 0, &jb) == HAL_OK);

	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	probe_close(&p);
}

/*
 * The two media capabilities of the backend, and the description that they
 * let through. The stub validates nothing and records what the core asked for
 * (hal.h enum hal_srtp_suite, the stub exemption). A case therefore checks the
 * bits of the capability word, and that an IPv6 description reaches the
 * stream without a refusal.
 *
 * The addresses themselves leave through the voice.stub dump, and this suite
 * makes no ubus connection to read one. The host smoke test reads them.
 */
static void test_media_families(void)
{
	struct hal_channel_cfg ccfg;
	const struct hal_cap *cap;
	struct hal_lec_stats lec;
	struct hal_jb_stats jb;
	struct hal_media m;
	struct probe p;

	if (!probe_open(&p)) {
		CASE("the media family case needs an open backend");
		CHECK(false);
		return;
	}

	memset(&lec, 0xa5, sizeof(lec));
	memset(&jb, 0xa5, sizeof(jb));
	cap = hal_backend_ops.cap_get(p.h);

	CASE("the backend carries IPv6 media and a separate RTCP address");
	CHECK((cap->features & HAL_FEAT_IPV6_MEDIA) != 0);
	CHECK((cap->features & HAL_FEAT_RTCP_ADDR) != 0);

	CASE("an IPv6 description and one with no ports are both taken");
	chan_build(&ccfg, true, true);
	media_build(&m, HAL_CODEC_PCMU, HAL_PASSTHROUGH_OFF, HAL_JB_ADAPTIVE);
	/* ::1 for both ends, and ::2 as the address the peer named for its own
	 * RTCP (hal.h struct hal_media rtcp_remote). */
	memset(&m.local.addr, 0, sizeof(m.local.addr));
	m.local.ipver = 6;
	m.local.addr[15] = 1;
	m.remote = m.local;
	m.remote.port = 40002;
	m.rtcp_remote.ipver = 6;
	m.rtcp_remote.addr[15] = 2;
	CHECK(hal_backend_ops.channel_config_set(p.h, 0, &ccfg) == HAL_OK);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.stream_start(p.h, 0, 0) == HAL_OK);
	/* The stream really runs, so the description was taken and not
	 * silently dropped on the way in. */
	CHECK(hal_backend_ops.line_lec_stats_get(p.h, 0, &lec) == HAL_OK);
	CHECK(hal_backend_ops.stream_jb_stats_get(p.h, 0, 0, &jb) == HAL_OK);
	CHECK_UINT(jb.mode, HAL_JB_ADAPTIVE);
	/*
	 * A description with no port at either end. A live backend answers
	 * HAL_FATAL for it (hal.h stream_config_set rule 4); this one validates
	 * nothing and records it, which is the exemption the same header grants
	 * the stub. The buffer mode is the one field of the description this
	 * suite can read back, so it carries the proof that the record landed.
	 */
	m.local.port = 0;
	m.local.rtcp_port = 0;
	m.remote.port = 0;
	m.remote.rtcp_port = 0;
	m.jb_mode = HAL_JB_OFF;
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.stream_jb_stats_get(p.h, 0, 0, &jb) == HAL_OK);
	CHECK_UINT(jb.mode, HAL_JB_OFF);

	CHECK(hal_backend_ops.stream_stop(p.h, 0, 0) == HAL_OK);
	probe_close(&p);
}

static void test_region_get_gate(void)
{
	struct hal_region reg;
	struct probe p;

	CHECK(probe_open(&p));
	if (!p.h)
		return;

	CASE("the region read-back answers the USA shape with no env at all");
	unsetenv("UVOICE_STUB_REGION_GET");
	memset(&reg, 0, sizeof(reg));
	CHECK(hal_backend_ops.region_get(p.h, &reg) == HAL_OK);
	CHECK(!strcmp(reg.iso3, "USA"));
	CHECK_UINT(reg.cid_format, HAL_CID_BELLCORE_FSK);
	CHECK_UINT(reg.mwi_format, HAL_CID_BELLCORE_FSK);
	CHECK_UINT(reg.hook_flash_min_ms, 300);
	CHECK_UINT(reg.hook_flash_max_ms, 1300);
	CHECK_UINT(reg.hook_release_min_ms, 1301);
	CHECK_UINT(reg.cw_sas_total_ms, 20600);
	CHECK_UINT(reg.ring[0].cadence_count, 1);
	CHECK_UINT(reg.ring[0].cadence[0].on_ms, 2000);
	CHECK_UINT(reg.ring[0].cadence[0].off_ms, 4000);
	CHECK_UINT(reg.flash_service, HAL_FLASH_SERVICE_DROP);
	CHECK(!strcmp(reg.describe, "Stub USA"));
	CHECK_UINT(reg.cw_count, 4);
	CHECK_UINT(reg.cw_lead_in_ms[0], 300);
	CHECK_UINT(reg.cw_lead_in_ms[3], 500);
	CHECK_UINT(reg.cw_lead_in_ms[4], 700);
	CHECK_UINT(reg.pulse_invert, 0);
	CHECK_UINT(reg.confirm_total_ms, 0);
	CHECK_UINT(reg.confirm_endless, 0);

	CASE("the backend accepts a call waiting variant and stops it");
	CHECK(hal_backend_ops.cw_tone_play != NULL);
	CHECK(hal_backend_ops.cw_tone_play(p.h, 0, 3, 0) == HAL_OK);
	CHECK(hal_backend_ops.cw_tone_play(p.h, 0, 0, 0) == HAL_OK);
	CHECK(hal_backend_ops.tone_stop(p.h, 0) == HAL_OK);

	CASE("none refuses, the shape of a backend with no read-back");
	setenv("UVOICE_STUB_REGION_GET", "none", 1);
	CHECK(hal_backend_ops.region_get(p.h, &reg) == HAL_UNSUPPORTED);

	CASE("the long_sas shape stretches the alert bound");
	setenv("UVOICE_STUB_REGION_GET", "long_sas", 1);
	CHECK(hal_backend_ops.region_get(p.h, &reg) == HAL_OK);
	CHECK_UINT(reg.cw_sas_total_ms, 70000);

	CASE("an unknown mode answers the USA shape");
	setenv("UVOICE_STUB_REGION_GET", "bogus", 1);
	CHECK(hal_backend_ops.region_get(p.h, &reg) == HAL_OK);
	CHECK_UINT(reg.cw_sas_total_ms, 20600);

	CASE("the flash service env names the shape's service");
	setenv("UVOICE_STUB_FLASH_SERVICE", "1", 1);
	CHECK(hal_backend_ops.region_get(p.h, &reg) == HAL_OK);
	CHECK_UINT(reg.flash_service, HAL_FLASH_SERVICE_SPLIT);
	setenv("UVOICE_STUB_FLASH_SERVICE", "7", 1);
	CHECK(hal_backend_ops.region_get(p.h, &reg) == HAL_OK);
	CHECK_UINT(reg.flash_service, HAL_FLASH_SERVICE_DROP);
	unsetenv("UVOICE_STUB_FLASH_SERVICE");

	CASE("the pulse invert env sets the decadic inversion of the shape");
	setenv("UVOICE_STUB_PULSE_INVERT", "1", 1);
	CHECK(hal_backend_ops.region_get(p.h, &reg) == HAL_OK);
	CHECK_UINT(reg.pulse_invert, 1);
	setenv("UVOICE_STUB_PULSE_INVERT", "2", 1);
	CHECK(hal_backend_ops.region_get(p.h, &reg) == HAL_OK);
	CHECK_UINT(reg.pulse_invert, 0);
	unsetenv("UVOICE_STUB_PULSE_INVERT");

	CASE("the confirm env sets the confirm tone length of the shape");
	setenv("UVOICE_STUB_CONFIRM_TOTAL_MS", "3000", 1);
	CHECK(hal_backend_ops.region_get(p.h, &reg) == HAL_OK);
	CHECK_UINT(reg.confirm_total_ms, 3000);
	CHECK_UINT(reg.confirm_endless, 0);
	unsetenv("UVOICE_STUB_CONFIRM_TOTAL_MS");

	CASE("the endless env sets a confirm tone that repeats until stopped");
	setenv("UVOICE_STUB_CONFIRM_ENDLESS", "1", 1);
	CHECK(hal_backend_ops.region_get(p.h, &reg) == HAL_OK);
	CHECK_UINT(reg.confirm_total_ms, 0);
	CHECK_UINT(reg.confirm_endless, 1);
	setenv("UVOICE_STUB_CONFIRM_ENDLESS", "2", 1);
	CHECK(hal_backend_ops.region_get(p.h, &reg) == HAL_OK);
	CHECK_UINT(reg.confirm_endless, 0);
	unsetenv("UVOICE_STUB_CONFIRM_ENDLESS");

	unsetenv("UVOICE_STUB_REGION_GET");
	probe_close(&p);
}

/*
 * The debug mask and the two diagnostic getters. This backend runs no engine
 * of its own, so it carries no bit of the mask and no counter behind a bit.
 * It stores nothing and answers unsupported for every bit, the packet trace
 * included. hal.h gives that answer to a backend that carries none of the
 * bits. Both diagnostic getters stay NULL, so a caller renders no diag table
 * at all, not a table of zeros.
 */
static void test_debug_and_diag(void)
{
	struct probe p;

	if (!probe_open(&p)) {
		CASE("the debug case needs an open backend");
		CHECK(false);
		return;
	}

	CASE("every debug bit is refused, the packet trace included");
	CHECK(hal_backend_ops.debug_set(p.h, 0) == HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_PKT_TRACE) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.debug_set(p.h, HAL_DEBUG_JB_TRACE |
					      HAL_DEBUG_SLIC_LOG |
					      HAL_DEBUG_PKT_TRACE) ==
	      HAL_UNSUPPORTED);

	CASE("the two diagnostic getters are absent on this backend");
	CHECK(hal_backend_ops.stream_diag_get == NULL);
	CHECK(hal_backend_ops.engine_diag_get == NULL);

	probe_close(&p);
}

/*
 * The delayed loop that the manufacturing test sets. This backend renders no
 * audio, so a case can read only the record that the op keeps. The trace line
 * carries the mode, the peer and the delay that the core asked for, and the
 * refusals are the ones that hal.h states.
 */
static void test_loop_set(void)
{
	const struct hal_cap *cap;
	struct hal_loop loop;
	struct probe p;

	if (!probe_open(&p)) {
		CASE("the loop case needs an open backend");
		CHECK(false);
		return;
	}

	cap = hal_backend_ops.cap_get(p.h);

	CASE("the backend carries the loop feature and the op");
	CHECK((cap->features & HAL_FEAT_LOOP) != 0);
	CHECK(hal_backend_ops.line_loop_set != NULL);

	CASE("a self loop is recorded on the line it names");
	memset(&loop, 0, sizeof(loop));
	loop.mode = HAL_LOOP_SELF;
	loop.delay_ms = 500;
	trace_last[0] = '\0';
	CHECK(hal_backend_ops.line_loop_set(p.h, 1, &loop) == HAL_OK);
	CHECK(!strcmp(trace_last, "stub: line 1 loop mode=1 peer=0 delay=500"));

	CASE("a peer loop records the peer it plays");
	loop.mode = HAL_LOOP_PEER;
	loop.peer = 1;
	loop.delay_ms = 0;
	CHECK(hal_backend_ops.line_loop_set(p.h, 0, &loop) == HAL_OK);
	CHECK(!strcmp(trace_last, "stub: line 0 loop mode=2 peer=1 delay=0"));

	CASE("off reads neither the peer nor the delay");
	loop.mode = HAL_LOOP_OFF;
	loop.peer = 7;
	loop.delay_ms = 4000;
	CHECK(hal_backend_ops.line_loop_set(p.h, 0, &loop) == HAL_OK);
	CHECK(!strcmp(trace_last, "stub: line 0 loop mode=0 peer=0 delay=0"));

	CASE("a peer outside the cap and a peer naming the line are refused");
	loop.mode = HAL_LOOP_PEER;
	loop.delay_ms = 0;
	loop.peer = (uint8_t)cap->lines;
	CHECK(hal_backend_ops.line_loop_set(p.h, 0, &loop) == HAL_UNSUPPORTED);
	loop.peer = 0;
	CHECK(hal_backend_ops.line_loop_set(p.h, 0, &loop) == HAL_UNSUPPORTED);

	CASE("a delay the backend cannot carry is refused");
	loop.mode = HAL_LOOP_SELF;
	loop.delay_ms = 1001;
	CHECK(hal_backend_ops.line_loop_set(p.h, 0, &loop) == HAL_UNSUPPORTED);
	loop.delay_ms = 1000;
	CHECK(hal_backend_ops.line_loop_set(p.h, 0, &loop) == HAL_OK);

	CASE("a mode above peer, a null record and a bad line are refused");
	loop.mode = HAL_LOOP_PEER + 1;
	CHECK(hal_backend_ops.line_loop_set(p.h, 0, &loop) == HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.line_loop_set(p.h, 0, NULL) == HAL_UNSUPPORTED);
	loop.mode = HAL_LOOP_SELF;
	loop.delay_ms = 0;
	CHECK(hal_backend_ops.line_loop_set(p.h, cap->lines, &loop) ==
	      HAL_UNSUPPORTED);
	CHECK(hal_backend_ops.line_loop_set(p.h, HAL_LINE_MAX, &loop) ==
	      HAL_UNSUPPORTED);

	probe_close(&p);
}

/*
 * The mutual exclusion that hal.h states, in both directions. A port that
 * rings or carries a stream refuses a loop request. A port that carries a
 * loop refuses a stream and a metallic test. OFF passes every one of them,
 * because a clear must reach a busy port.
 */
static void test_loop_exclusion(void)
{
	struct hal_channel_cfg ccfg;
	struct hal_job job = { 0 };
	struct hal_loop loop;
	struct hal_ring ring;
	struct hal_media m;
	struct probe p;

	if (!probe_open(&p)) {
		CASE("the loop exclusion case needs an open backend");
		CHECK(false);
		return;
	}

	memset(&loop, 0, sizeof(loop));
	memset(&ring, 0, sizeof(ring));
	ring.cadence_count = 1;
	ring.cadence[0].on_ms = 2000;
	chan_build(&ccfg, true, true);
	media_build(&m, HAL_CODEC_PCMU, HAL_PASSTHROUGH_OFF, HAL_JB_ADAPTIVE);

	CASE("a ringing port refuses an arm and takes the clear");
	CHECK(hal_backend_ops.line_ring_start(p.h, 0, &ring) == HAL_OK);
	loop.mode = HAL_LOOP_SELF;
	CHECK(hal_backend_ops.line_loop_set(p.h, 0, &loop) == HAL_TRANSIENT);
	loop.mode = HAL_LOOP_OFF;
	CHECK(hal_backend_ops.line_loop_set(p.h, 0, &loop) == HAL_OK);
	CHECK(hal_backend_ops.line_ring_stop(p.h, 0) == HAL_OK);

	CASE("a running stream refuses an arm on its own port and on a peer");
	CHECK(hal_backend_ops.channel_config_set(p.h, 1, &ccfg) == HAL_OK);
	CHECK(hal_backend_ops.stream_config_set(p.h, 1, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.stream_start(p.h, 1, 0) == HAL_OK);
	loop.mode = HAL_LOOP_SELF;
	CHECK(hal_backend_ops.line_loop_set(p.h, 1, &loop) == HAL_TRANSIENT);
	loop.mode = HAL_LOOP_PEER;
	loop.peer = 1;
	CHECK(hal_backend_ops.line_loop_set(p.h, 0, &loop) == HAL_TRANSIENT);
	CHECK(hal_backend_ops.stream_stop(p.h, 1, 0) == HAL_OK);

	CASE("and the same pair is taken once the stream stops");
	CHECK(hal_backend_ops.line_loop_set(p.h, 0, &loop) == HAL_OK);

	CASE("an armed port refuses a stream and a metallic test");
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) ==
	      HAL_TRANSIENT);
	CHECK(hal_backend_ops.line_test_start(p.h, 0, HAL_LINE_TEST_GR909,
					      &job) == HAL_TRANSIENT);
	/* the peer of that arm carries no loop of its own, so it takes both */
	CHECK(hal_backend_ops.stream_config_set(p.h, 1, 0, &m) == HAL_OK);

	CASE("and both come back once the loop is cleared");
	loop.mode = HAL_LOOP_OFF;
	CHECK(hal_backend_ops.line_loop_set(p.h, 0, &loop) == HAL_OK);
	CHECK(hal_backend_ops.stream_config_set(p.h, 0, 0, &m) == HAL_OK);
	CHECK(hal_backend_ops.line_test_start(p.h, 0, HAL_LINE_TEST_GR909,
					      &job) == HAL_OK);
	/* the accepted test schedules a completion on the loop, and the handle
	 * it writes to is this probe's: it has to land before the close */
	uloop_pump(STUB_JOB_WAIT_MS);

	probe_close(&p);
}

/*
 * The ring cap hook. UVOICE_STUB_RING_CAP_MS shortens only the timer, so a
 * suite can watch a 60 s ring end inside a pumped window. The record that the
 * op traces keeps the duration that the core sent.
 */
static void test_ring_cap(void)
{
	struct hal_ring ring;
	struct probe p;

	if (!probe_open(&p)) {
		CASE("the ring cap case needs an open backend");
		CHECK(false);
		return;
	}

	memset(&ring, 0, sizeof(ring));
	ring.cadence_count = 1;
	ring.cadence[0].on_ms = 2000;
	ring.cadence[0].off_ms = 4000;
	ring.duration_ms = 60000;

	CASE("a capped ring ends in the window and traces the duration sent");
	setenv("UVOICE_STUB_RING_CAP_MS", "40", 1);
	trace_last[0] = '\0';
	CHECK(hal_backend_ops.line_ring_start(p.h, 0, &ring) == HAL_OK);
	CHECK(strstr(trace_last, "dur=60000") != NULL);
	uloop_pump(150);
	CHECK(ev_drain_has(&p, HAL_EVENT_RING_END));

	CASE("a cap above the duration the core sent leaves the timer alone");
	setenv("UVOICE_STUB_RING_CAP_MS", "5000", 1);
	ring.duration_ms = 40;
	CHECK(hal_backend_ops.line_ring_start(p.h, 0, &ring) == HAL_OK);
	uloop_pump(150);
	CHECK(ev_drain_has(&p, HAL_EVENT_RING_END));

	CASE("with no variable the ring holds the duration the core sent");
	unsetenv("UVOICE_STUB_RING_CAP_MS");
	ring.duration_ms = 60000;
	CHECK(hal_backend_ops.line_ring_start(p.h, 0, &ring) == HAL_OK);
	uloop_pump(150);
	CHECK(!ev_drain_has(&p, HAL_EVENT_RING_END));

	CASE("and the stop ends that ring");
	CHECK(hal_backend_ops.line_ring_stop(p.h, 0) == HAL_OK);
	CHECK(ev_drain_has(&p, HAL_EVENT_RING_END));

	probe_close(&p);
}

int main(void)
{
	uloop_init();
	trace_sink_set(trace_capture);
	trace_ring_set(1u << TRACE_HAL);

	test_lec_instance();
	test_lec_lifetime();
	test_lec_kinds();
	test_lec_two_streams();
	test_stats_refusals();
	test_media_families();
	test_region_get_gate();
	test_debug_and_diag();
	test_loop_set();
	test_loop_exclusion();
	test_ring_cap();

	uloop_done();

	return tu_summary("test_hal_stub");
}
