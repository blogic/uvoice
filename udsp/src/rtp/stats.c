// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <string.h>

#include "rtp.h"

void rtp_stats_init_cfg(struct rtp_stats *stats, uint32_t clock_rate)
{
	memset(stats, 0, sizeof(*stats));
	stats->clock_rate = clock_rate ? clock_rate : RTP_CLOCK_RATE;
}

void rtp_stats_init(struct rtp_stats *stats)
{
	rtp_stats_init_cfg(stats, RTP_CLOCK_RATE);
}

void rtp_stats_delay_set(struct rtp_stats *stats, uint32_t delay_ms)
{
	stats->model_delay_ms = delay_ms;
}

void rtp_stats_rtcp_report(struct rtp_stats *stats,
			   const struct rtcp_report *report,
			   uint32_t now_ntp_middle)
{
	uint32_t rtt_units;
	uint32_t rtt_ms;

	stats->loss_fraction_far = report->fraction_lost;
	stats->jitter_far = report->jitter;
	if (!report->lsr)
		return;
	rtt_units = now_ntp_middle - report->lsr - report->dlsr;
	if (rtt_units & 0x80000000U)
		return;
	rtt_ms = (uint32_t)(((uint64_t)rtt_units * 1000U + 32768U) >> 16);
	stats->rtt_sum_ms += rtt_ms;
	stats->rtt_samples++;
	if (rtt_ms > stats->rtt_max_ms)
		stats->rtt_max_ms = rtt_ms;
}

static double score_clamp(double score)
{
	if (score < 0.0)
		return 0.0;
	if (score > 100.0)
		return 100.0;
	return score;
}

static float score_mos(double score)
{
	if (score <= 0.0)
		return 1.0f;
	if (score >= 100.0)
		return 4.5f;
	return (float)(1.0 + 0.035 * score +
		       0.000007 * score * (score - 60.0) * (100.0 - score));
}

double rtp_stats_rfactor(double loss_percent, uint32_t delay_ms)
{
	double delay = delay_ms;
	double id;
	double ie_eff;

	if (loss_percent < 0.0)
		loss_percent = 0.0;
	if (loss_percent > 100.0)
		loss_percent = 100.0;
	/*
	 * G.107 7 and Annex B subset: R0=93.2, Is=A=0, G.711 Ie=0,
	 * Bpl=25.1, BurstR=1. Id=0.024d+0.11(d-177.3)H(d-177.3), and
	 * Ie-eff=Ie+(95-Ie)Ppl/(Ppl/BurstR+Bpl). MOS uses Annex B.4.
	 */
	id = 0.024 * delay;
	if (delay > 177.3)
		id += 0.11 * (delay - 177.3);
	ie_eff = 95.0 * loss_percent / (loss_percent + 25.1);
	return score_clamp(93.2 - id - ie_eff);
}

void rtp_stats_quality(double loss_percent, uint32_t delay_ms,
		       float *mos_lq, float *mos_cq, uint32_t *rfactor)
{
	double listening_score;
	double conversational_score;

	listening_score = rtp_stats_rfactor(loss_percent, 0);
	conversational_score = rtp_stats_rfactor(loss_percent, delay_ms);
	*mos_lq = score_mos(listening_score);
	*mos_cq = score_mos(conversational_score);
	*rfactor = (uint32_t)(conversational_score + 0.5);
}

void rtp_stats_snapshot(const struct rtp_stats *stats,
			const struct rtp_receiver *receiver,
			struct udsp_media_stats *out)
{
	uint32_t ts_per_ms = stats->clock_rate ? stats->clock_rate / 1000U :
						 RTP_CLOCK_RATE / 1000U;
	uint32_t expected = 0;
	uint32_t lost = 0;
	double loss_percent = 0.0;

	memset(out, 0, sizeof(*out));
	if (receiver) {
		expected = rtp_receiver_expected_get(receiver);
		if (expected > receiver->received)
			lost = expected - receiver->received;
		if (expected)
			loss_percent = (double)lost * 100.0 / expected;
	}
	out->packets_sent = stats->packets_sent;
	out->packets_recv = stats->packets_recv;
	out->packets_lost = lost;
	out->packets_discarded = stats->packets_discarded;
	/* loss_rate fields use the RFC 3550 A.3 fraction, in units of 1/256. */
	out->loss_rate = stats->loss_fraction;
	out->loss_rate_far = stats->loss_fraction_far;
	/* Every jitter here is in the stream clock's units: ours from the
	 * RFC 3550 6.4.1 estimator, the far one from its report block, which
	 * RFC 7160 pins to the same clock as the SSRC it describes. */
	if (stats->jitter_samples)
		out->jitter_avg = stats->jitter_sum / stats->jitter_samples /
				  ts_per_ms;
	out->jitter_max = stats->jitter_max / ts_per_ms;
	out->jitter_far = stats->jitter_far / ts_per_ms;
	if (stats->rtt_samples)
		out->rtt_avg = stats->rtt_sum_ms / stats->rtt_samples;
	out->rtt_max = stats->rtt_max_ms;
	out->bytes_sent = stats->bytes_sent;
	out->bytes_recv = stats->bytes_recv;
	rtp_stats_quality(loss_percent, stats->model_delay_ms, &out->mos_lq,
			  &out->mos_cq, &out->rfactor);
}
