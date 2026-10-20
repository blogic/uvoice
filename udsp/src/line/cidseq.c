// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Caller identity sequencer. One sequence per line, driven by the tick and
 * expressed as ordered phases. This module arms the FSK burst and the tone
 * programs, and the engine plays them, so a multi-second message never blocks
 * a caller.
 *
 * Type 1 phase chains, one per enum udsp_cid_format:
 *   bellcore-fsk, etsi-fsk  OHT -> wait_ms -> FSK
 *   etsi-rpas               ring pulse -> OHT -> wait_ms -> FSK
 *   etsi-dtas               OHT + DT-AS -> wait_ms -> FSK
 *   etsi-lr-dtas            reversal -> pause -> OHT + DT-AS -> wait_ms -> FSK
 *   dtmf                    OHT -> wait_ms -> DTMF string
 *   dtmf-dtas               OHT + DT-AS -> wait_ms -> DTMF string
 *   ntt                     reversal + OHT -> wait_ms -> FSK
 *
 * Type 2 (UDSP_CID_TYPE2) runs mid-call: mute -> CAS -> acknowledgement window
 * -> FSK on the type 2 profile (no channel seizure) -> unmute. The far end must
 * hear none of it, so the mute records bracket the whole sequence. This module
 * emits no subscriber alert (SAS); the uvoice core plays the call-waiting tone
 * itself.
 *
 * A VMWI burst is the same type 1 transport carrying the indicator message from
 * udsp_cid_vmwi_build instead of the caller identity.
 *
 * Abort. A hook edge, a ring stop and an expired CID window all abort: the
 * audio stops, the polarity, linefeed and mute return to their pre-sequence
 * values and UDSP_EV_CID_SENT carries a negative status. The type 2
 * acknowledgement timeout is the one exception: it reports
 * UDSP_EV_CID2_TIMEOUT, which is the core's dedicated signal for it.
 */
#include <errno.h>

#include "line.h"

static bool format_valid(uint8_t format)
{
	return format <= (uint8_t)UDSP_CIDF_DTMF_DTAS;
}

static bool format_is_dtmf(uint8_t format)
{
	return format == (uint8_t)UDSP_CIDF_DTMF ||
	       format == (uint8_t)UDSP_CIDF_DTMF_DTAS;
}

static enum udsp_fsk_std fsk_std_get(uint8_t format)
{
	/* Bell 202 is the Bellcore carrier; every ETSI variant and the NTT
	 * frame use V.23 (EN 300 659-1, NTT technical reference). */
	if (format == (uint8_t)UDSP_CIDF_BELLCORE_FSK)
		return UDSP_FSK_BELL202;
	return UDSP_FSK_V23;
}

/* The tone generator treats UDSP_TONE_NONE as "already complete", so the alert
 * borrows the call-waiting id; the id is only a label for the generator. */
static void alert_desc_fill(struct udsp_tone_desc *desc)
{
	memset(desc, 0, sizeof(*desc));
	desc->tone = UDSP_TONE_CALL_WAITING;
	desc->frequency_count = 2;
	desc->frequency_hz[0] = LINE_CAS_LOW_HZ;
	desc->frequency_hz[1] = LINE_CAS_HIGH_HZ;
	desc->level_dbm0[0] = LINE_CAS_LEVEL_DBM0;
	desc->level_dbm0[1] = LINE_CAS_LEVEL_DBM0;
}

/* A region file may omit the alert timing keys; a zero takes the default. */
static uint16_t cfg_ms(uint16_t value, uint16_t fallback)
{
	return value ? value : fallback;
}

static void relay_set(struct line_cidseq *c, uint32_t now_ms, bool on,
		      struct line_out *out)
{
	if (c->relay_on == on)
		return;
	c->relay_on = on;
	line_out_cmd(out, now_ms, LINE_CMD_RING_RELAY)->u.relay.on = on;
}

/*
 * On-hook transmission carries the polarity in the linefeed state: slic.h
 * defines UDSP_LF_FWD_OHT as forward, so arming it while the sequence holds a
 * reversal would cancel that reversal before the alert and the burst, and the
 * SIN 227 and NTT terminals would never see the line reverse at all.
 */
static void oht_arm(struct line_cidseq *c, uint32_t now_ms, struct line_out *out)
{
	if (c->oht_set)
		return;
	c->oht_set = true;
	line_out_cmd(out, now_ms, LINE_CMD_LINEFEED)->u.linefeed.state =
		c->reversed ? UDSP_LF_REV_OHT : UDSP_LF_FWD_OHT;
}

static void polarity_reverse(struct line_cidseq *c, uint32_t now_ms,
			     struct line_out *out)
{
	c->reversed = true;
	line_out_cmd(out, now_ms, LINE_CMD_POLARITY)->u.polarity.state = UDSP_POL_REVERSE;
}

static void tone_arm(struct line_cidseq *c, uint32_t now_ms,
		     const struct udsp_tone_desc *desc, uint16_t duration_ms,
		     struct line_out *out)
{
	struct line_cmd *cmd = line_out_cmd(out, now_ms, LINE_CMD_TONE);

	cmd->u.tone.desc = *desc;
	cmd->u.tone.duration_ms = duration_ms;
	c->audio_on = true;
}

static void restore_apply(struct line_cidseq *c, uint32_t now_ms, struct line_out *out)
{
	if (c->audio_on) {
		c->audio_on = false;
		line_out_cmd(out, now_ms, LINE_CMD_TONE_STOP);
	}
	relay_set(c, now_ms, false, out);
	if (c->reversed) {
		c->reversed = false;
		line_out_cmd(out, now_ms,
			     LINE_CMD_POLARITY)->u.polarity.state = UDSP_POL_FORWARD;
	}
	if (c->oht_set) {
		struct line_cmd *cmd = line_out_cmd(out, now_ms, LINE_CMD_LINEFEED);

		c->oht_set = false;
		cmd->u.linefeed.state = c->restore_lf;
		cmd->u.linefeed.restore = true;
	}
	if (c->muted) {
		c->muted = false;
		line_out_cmd(out, now_ms, LINE_CMD_MUTE)->u.mute.on = false;
	}
}

static void seq_reset(struct line_cidseq *c, struct line_cid_gate *gate)
{
	c->step = LINE_CID_IDLE;
	c->burst_done = false;
	c->ack_seen = false;
	if (gate)
		gate->complete = true;
}

static void seq_finish(struct line_cidseq *c, int status, uint32_t now_ms,
		       struct line_cid_gate *gate, struct line_out *out)
{
	struct udsp_event *ev;

	restore_apply(c, now_ms, out);
	ev = line_out_event(out, now_ms, c->line, UDSP_EV_CID_SENT);
	ev->u.cid_sent.kind = c->kind;
	ev->u.cid_sent.status = (int8_t)(status < INT8_MIN ? -EIO : status);
	seq_reset(c, gate);
}

static void seq_timeout(struct line_cidseq *c, uint32_t now_ms,
			struct line_cid_gate *gate, struct line_out *out)
{
	restore_apply(c, now_ms, out);
	line_out_event(out, now_ms, c->line, UDSP_EV_CID2_TIMEOUT);
	seq_reset(c, gate);
}

static enum line_cid_step step_first(const struct line_cidseq *c)
{
	if (c->kind == (uint8_t)UDSP_CID_TYPE2)
		return LINE_CID_ALERT;
	switch (c->format) {
	case UDSP_CIDF_ETSI_RPAS:
		return LINE_CID_RP_PULSE;
	case UDSP_CIDF_ETSI_LR_DTAS:
		return LINE_CID_LR_PAUSE;
	case UDSP_CIDF_ETSI_DTAS:
	case UDSP_CIDF_DTMF_DTAS:
		return LINE_CID_ALERT;
	default:
		return LINE_CID_WAIT;
	}
}

static enum line_cid_step step_after(const struct line_cidseq *c)
{
	switch (c->step) {
	case LINE_CID_RP_PULSE:
		return LINE_CID_WAIT;
	case LINE_CID_LR_PAUSE:
		return LINE_CID_ALERT;
	case LINE_CID_ALERT:
		return c->kind == (uint8_t)UDSP_CID_TYPE2 ? LINE_CID_ACK :
							    LINE_CID_WAIT;
	default:
		return LINE_CID_SEND;
	}
}

static void alert_enter(struct line_cidseq *c, uint32_t now_ms, struct line_out *out)
{
	struct udsp_tone_desc desc;

	alert_desc_fill(&desc);
	if (c->kind != (uint8_t)UDSP_CID_TYPE2) {
		uint16_t dtas = cfg_ms(c->cfg.dtas_ms, LINE_DTAS_MS);

		oht_arm(c, now_ms, out);
		tone_arm(c, now_ms, &desc, dtas, out);
		c->phase_len_ms = dtas;
		return;
	}
	if (!c->muted) {
		c->muted = true;
		line_out_cmd(out, now_ms, LINE_CMD_MUTE)->u.mute.on = true;
	}
	tone_arm(c, now_ms, &desc, LINE_CAS_MS, out);
	c->phase_len_ms = LINE_CAS_MS;
}

static void fsk_enter(struct line_cidseq *c, uint32_t now_ms,
		      struct line_cid_gate *gate, struct line_out *out)
{
	uint8_t buf[UDSP_CID_MSG_MAX];
	bool type2 = c->kind == (uint8_t)UDSP_CID_TYPE2;
	struct line_cmd *cmd;
	size_t len;

	if (c->mwi != LINE_CID_MWI_NONE)
		len = udsp_cid_vmwi_build(c->mwi == LINE_CID_MWI_ON, c->cid.datetime,
					  (enum udsp_cid_format)c->format, buf,
					  sizeof(buf));
	else
		len = udsp_cid_msg_build(&c->cid, (enum udsp_cid_format)c->format,
					 type2, buf, sizeof(buf));
	if (!len || len > sizeof(buf)) {
		seq_finish(c, -ENOTSUP, now_ms, gate, out);
		return;
	}
	cmd = line_out_cmd(out, now_ms, LINE_CMD_FSK);
	cmd->u.fsk.std = (uint8_t)fsk_std_get(c->format);
	cmd->u.fsk.level_dbm0 = LINE_CID_FSK_LEVEL_DBM0;
	cmd->u.fsk.seizure_bits = type2 ? LINE_CID2_SEIZURE_BITS : c->cfg.seizure_bits;
	cmd->u.fsk.mark_bits = type2 ? LINE_CID2_MARK_BITS : c->cfg.mark_bits;
	cmd->u.fsk.len = (uint16_t)len;
	memcpy(cmd->u.fsk.bytes, buf, len);
	c->audio_on = true;
}

static void dtmf_enter(struct line_cidseq *c, uint32_t now_ms,
		       struct line_cid_gate *gate, struct line_out *out)
{
	char digits[LINE_CID_DTMF_MAX];
	struct line_cmd *cmd;
	size_t len;

	len = udsp_cid_dtmf_string(&c->cid, (enum udsp_cid_format)c->format, digits,
				   sizeof(digits));
	if (!len || len > sizeof(digits)) {
		seq_finish(c, -ENOTSUP, now_ms, gate, out);
		return;
	}
	cmd = line_out_cmd(out, now_ms, LINE_CMD_DTMF);
	memcpy(cmd->u.dtmf.digits, digits, len);
	cmd->u.dtmf.count = (uint8_t)len;
	cmd->u.dtmf.level_dbm0 = LINE_CID_DTMF_LEVEL_DBM0;
	cmd->u.dtmf.on_ms = LINE_CID_DTMF_ON_MS;
	cmd->u.dtmf.off_ms = LINE_CID_DTMF_OFF_MS;
	c->audio_on = true;
}

static void step_enter(struct line_cidseq *c, enum line_cid_step step, uint32_t now_ms,
		       struct line_cid_gate *gate, struct line_out *out)
{
	uint8_t prev = c->step;

	/* An alert that ran its programmed duration needs no stop record; only
	 * an abort inside the alert or the message does. */
	if (c->step == LINE_CID_ALERT)
		c->audio_on = false;
	c->step = (uint8_t)step;
	c->phase_ms = now_ms;
	c->phase_len_ms = 0;
	switch (step) {
	case LINE_CID_RP_PULSE:
		relay_set(c, now_ms, true, out);
		c->phase_len_ms = cfg_ms(c->cfg.rpas_ms, LINE_RPAS_MS);
		return;
	case LINE_CID_LR_PAUSE:
		polarity_reverse(c, now_ms, out);
		c->phase_len_ms = cfg_ms(c->cfg.lr_pause_ms, LINE_LR_PAUSE_MS);
		return;
	case LINE_CID_ALERT:
		alert_enter(c, now_ms, out);
		return;
	case LINE_CID_WAIT: {
		/* The pause starts at a relay-off when it opens the window
		 * that follows a burst, or when this sequence's own RP-AS
		 * pulse ends here, and the line rings on for up to a cycle
		 * after it (line.h LINE_RING_TAIL_MS). An alert in front of
		 * the pause already keeps it clear of the burst. */
		bool tail = c->relay_on ||
			    (prev == LINE_CID_PENDING && c->wait_ring && gate &&
			     gate->after_burst);

		relay_set(c, now_ms, false, out);
		if (c->kind != (uint8_t)UDSP_CID_TYPE2)
			oht_arm(c, now_ms, out);
		unsigned len = c->cfg.wait_ms + (tail ? LINE_RING_TAIL_MS : 0u);

		c->phase_len_ms = (uint16_t)(len > UINT16_MAX ? UINT16_MAX : len);
		return;
	}
	case LINE_CID_ACK:
		c->phase_len_ms = c->cfg.ack_timeout_ms;
		return;
	default:
		break;
	}
	/* A visual indicator is always an FSK message: udsp_cid_vmwi_build has
	 * no DTMF rendering and returns 0 there, which completes -ENOTSUP. */
	if (c->mwi == LINE_CID_MWI_NONE && format_is_dtmf(c->format)) {
		dtmf_enter(c, now_ms, gate, out);
		return;
	}
	fsk_enter(c, now_ms, gate, out);
}

static void seq_begin(struct line_cidseq *c, uint32_t now_ms,
		      struct line_cid_gate *gate, struct line_out *out)
{
	/* NTT signals the terminal with a reversal before the on-hook frame. */
	if (c->format == (uint8_t)UDSP_CIDF_NTT)
		polarity_reverse(c, now_ms, out);
	step_enter(c, step_first(c), now_ms, gate, out);
}

static void ack_run(struct line_cidseq *c, uint32_t now_ms, struct line_cid_gate *gate,
		    struct line_out *out)
{
	if (c->ack_seen && c->ack_ms - c->phase_ms <= c->phase_len_ms) {
		step_enter(c, LINE_CID_SEND, now_ms, gate, out);
		return;
	}
	if (now_ms - c->phase_ms <= c->phase_len_ms)
		return;
	seq_timeout(c, now_ms, gate, out);
}

/* Every FSK family runs at 1200 baud (fsk_mod.c), and a byte costs its eight
 * bits plus a start and a stop, with the two-bit postamble on the end. */
static unsigned fsk_span_ms(const struct udsp_region_cid *cfg, bool type2)
{
	unsigned seizure = type2 ? LINE_CID2_SEIZURE_BITS : cfg->seizure_bits;
	unsigned mark = type2 ? LINE_CID2_MARK_BITS : cfg->mark_bits;
	unsigned bits = seizure + mark + LINE_CID_MSG_MAX_BYTES * 10u + 2u;

	return (bits * 1000u + 1199u) / 1200u;
}

/*
 * The alert and pause phases step_first and step_after walk for this format,
 * plus the pre-message pause, plus the message. It overstates on purpose: the
 * caller tests it against a window the sequence must fit inside, so an
 * estimate that ran short would let a burst be cut mid-message.
 */
unsigned line_cidseq_budget_ms(enum udsp_cid_kind kind, const struct udsp_cid *cid,
			       const struct udsp_region *region,
			       uint32_t wait_override_ms)
{
	const struct udsp_region_cid *cfg;
	unsigned span = LINE_CID_BUDGET_MARGIN_MS;
	unsigned wait;

	if (!cid || !region)
		return 0;
	cfg = &region->cid;

	if (kind == UDSP_CID_TYPE2)
		return span + LINE_CAS_MS + cfg->ack_timeout_ms +
		       fsk_span_ms(cfg, true);

	/* The reversal pause of LR-DTAS precedes the alert the DTAS chain also
	 * plays, so that format pays both. */
	if (cid->format == (uint8_t)UDSP_CIDF_ETSI_LR_DTAS)
		span += cfg_ms(cfg->lr_pause_ms, LINE_LR_PAUSE_MS);

	switch (cid->format) {
	case UDSP_CIDF_ETSI_RPAS:
		span += cfg_ms(cfg->rpas_ms, LINE_RPAS_MS);
		break;
	case UDSP_CIDF_ETSI_LR_DTAS:
	case UDSP_CIDF_ETSI_DTAS:
	case UDSP_CIDF_DTMF_DTAS:
		span += cfg_ms(cfg->dtas_ms, LINE_DTAS_MS);
		break;
	default:
		break;
	}

	/* The ring tail can follow any type 1 pause (step_enter). */
	wait = wait_override_ms ? wait_override_ms : cfg->wait_ms;
	span += wait + LINE_RING_TAIL_MS;
	if (format_is_dtmf(cid->format))
		return span + LINE_CID_DTMF_MAX *
			      (LINE_CID_DTMF_ON_MS + LINE_CID_DTMF_OFF_MS);

	return span + fsk_span_ms(cfg, false);
}

void line_cidseq_init(struct line_cidseq *c, unsigned line)
{
	memset(c, 0, sizeof(*c));
	c->line = line;
}

bool line_cidseq_busy(const struct line_cidseq *c)
{
	return c->step != LINE_CID_IDLE;
}

bool line_cidseq_ack_open(const struct line_cidseq *c)
{
	return c->step == LINE_CID_ACK;
}

int line_cidseq_arm(struct line_cidseq *c, enum udsp_cid_kind kind,
		    const struct udsp_cid *cid, const struct udsp_region *region,
		    enum udsp_linefeed restore_lf, bool wait_ring,
		    enum line_cid_mwi mwi, uint32_t wait_override_ms)
{
	bool type2 = kind == UDSP_CID_TYPE2;
	unsigned line = c->line;

	if (!cid || !region || !format_valid(cid->format))
		return -EINVAL;
	/* Call waiting has no DTMF transport and no visual-indicator form: the
	 * far end is already on the line and the chain is CAS, acknowledgement,
	 * FSK. Accepting either would arm a message no terminal can receive. */
	if (type2 && (format_is_dtmf(cid->format) || mwi != LINE_CID_MWI_NONE))
		return -EINVAL;
	/* A sequence that outlives the break it runs in is refused here rather
	 * than cut mid-message by the window. Only a wait_ring sequence has a
	 * break to fit inside; a standalone burst owns the line already. */
	if (wait_ring && line_cidseq_budget_ms(kind, cid, region,
					       wait_override_ms) >
			 LINE_CID_WINDOW_MAX_MS)
		return -EINVAL;
	if (c->step != LINE_CID_IDLE)
		return -EBUSY;
	memset(c, 0, sizeof(*c));
	c->line = line;
	c->kind = (uint8_t)kind;
	c->format = cid->format;
	c->mwi = (uint8_t)mwi;
	c->cid = *cid;
	c->cfg = region->cid;
	if (wait_override_ms)
		c->cfg.wait_ms = (uint16_t)wait_override_ms;
	c->restore_lf = (uint8_t)restore_lf;
	c->wait_ring = wait_ring;
	c->step = LINE_CID_PENDING;
	return 0;
}

void line_cidseq_burst_done(struct line_cidseq *c)
{
	if (c->step != LINE_CID_SEND)
		return;
	c->burst_done = true;
	c->audio_on = false;
}

void line_cidseq_dtmf_ack(struct line_cidseq *c, uint32_t now_ms, char digit)
{
	if (c->step != LINE_CID_ACK || c->ack_seen)
		return;
	if (digit != 'A' && digit != 'a' && digit != 'D' && digit != 'd')
		return;
	c->ack_seen = true;
	c->ack_ms = now_ms;
}

void line_cidseq_abort(struct line_cidseq *c, int status, uint32_t now_ms,
		       struct line_cid_gate *gate, struct line_out *out)
{
	if (c->step == LINE_CID_IDLE)
		return;
	seq_finish(c, status < 0 ? status : -ECANCELED, now_ms, gate, out);
}

void line_cidseq_tick(struct line_cidseq *c, uint32_t now_ms,
		      const struct line_hook_report *hook, struct line_cid_gate *gate,
		      struct line_out *out)
{
	if (c->step == LINE_CID_IDLE)
		return;
	if (hook && (hook->offhook_edge || hook->onhook_edge)) {
		line_cidseq_abort(c, -ECANCELED, now_ms, gate, out);
		return;
	}
	if (c->step == LINE_CID_PENDING) {
		if (c->wait_ring && (!gate || !gate->request))
			return;
		seq_begin(c, now_ms, gate, out);
		return;
	}
	if (c->wait_ring && gate && !gate->request) {
		/* The ring machine closed the window: a ring stop, or the
		 * liveness cap that arm already proved this sequence fits. */
		line_cidseq_abort(c, -ETIMEDOUT, now_ms, gate, out);
		return;
	}
	if (c->step == LINE_CID_ACK) {
		ack_run(c, now_ms, gate, out);
		return;
	}
	if (c->step == LINE_CID_SEND) {
		if (c->burst_done)
			seq_finish(c, 0, now_ms, gate, out);
		return;
	}
	if (now_ms - c->phase_ms < c->phase_len_ms)
		return;
	step_enter(c, step_after(c), now_ms, gate, out);
}
