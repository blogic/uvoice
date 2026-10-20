// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Ring cadence engine. It turns a struct udsp_ring into ring-relay commands and
 * the ring events, and it owns the CID window that the sequencer fills.
 *
 * A ring session is a chain of phases. Phase 0 is a zero-length break before
 * the first burst, which makes cid_at_ring 0 ("before the first burst") the
 * same code path as any other break. first[] clocks the first cycle when it is
 * present, cadence[] clocks every cycle after it.
 *
 * The CID window opens in the break whose index equals cid_at_ring, holds the
 * cadence, and closes when the sequencer raises gate->complete or when the
 * liveness cap runs out. Holding the break is the only correct behaviour: the
 * next burst must not cut the message.
 *
 * The cap is not a caller timing. cid_wait_ms is the delay from the break to
 * the burst, a phase the sequencer runs inside this window; reading it as the
 * window bound closed the break on the very tick the sequencer left
 * LINE_CID_WAIT, so no burst was ever armed. The sequencer refuses at arm any
 * sequence whose budget cannot fit LINE_CID_WINDOW_MAX_MS, which is what lets
 * one flat cap be correct for every format.
 *
 * Ring trip. An off-hook edge ends the session in the tick it arrives: the
 * relay command and UDSP_EV_RING_END enter the journal here, and the caller
 * appends UDSP_EV_HOOK afterwards (line.h section 2), so the core always sees
 * UDSP_EV_RING_END before the seize.
 */
#include <errno.h>

#include "line.h"

static const struct udsp_cadence *table_get(const struct line_ring *r, uint8_t *count)
{
	if (r->first_cycle && r->cfg.first_count) {
		*count = r->cfg.first_count;
		return r->cfg.first;
	}
	*count = r->cfg.cadence_count;
	return r->cfg.cadence;
}

static void relay_set(struct line_ring *r, uint32_t now_ms, bool on,
		      struct line_out *out)
{
	if (r->relay_on == on)
		return;
	r->relay_on = on;
	line_out_cmd(out, now_ms, LINE_CMD_RING_RELAY)->u.relay.on = on;
}

static void ring_end(struct line_ring *r, uint32_t now_ms,
		     enum line_ring_reason reason, struct line_cid_gate *gate,
		     struct line_out *out)
{
	struct udsp_event *ev;

	relay_set(r, now_ms, false, out);
	if (gate) {
		gate->request = false;
		gate->complete = false;
	}
	r->stage = LINE_RING_OFF;
	r->cid_open = false;
	ev = line_out_event(out, now_ms, r->line, UDSP_EV_RING_END);
	ev->u.line.state = (uint8_t)reason;
	ev->u.line.ring_count = r->bursts;
}

static void index_advance(struct line_ring *r, uint8_t count)
{
	r->index++;
	if (r->index < count)
		return;
	r->index = 0;
	r->first_cycle = false;
}

static void burst_start(struct line_ring *r, uint32_t now_ms, struct line_out *out)
{
	const struct udsp_cadence *tbl;
	uint8_t count;

	tbl = table_get(r, &count);
	r->stage = LINE_RING_BURST;
	r->phase_ms = now_ms;
	r->phase_len_ms = tbl[r->index].on_ms;
	r->bursts++;
	relay_set(r, now_ms, true, out);
	if (r->bursts == 1)
		line_out_event(out, now_ms, r->line,
			       UDSP_EV_RING_START)->u.line.ring_count = r->bursts;
}

static void burst_run(struct line_ring *r, uint32_t now_ms, struct line_out *out)
{
	const struct udsp_cadence *tbl;
	uint8_t count;

	if (now_ms - r->phase_ms < r->phase_len_ms)
		return;
	relay_set(r, now_ms, false, out);
	tbl = table_get(r, &count);
	r->stage = LINE_RING_SILENCE;
	r->phase_ms = now_ms;
	r->phase_len_ms = tbl[r->index].off_ms;
	index_advance(r, count);
}

static void cid_window_run(struct line_ring *r, uint32_t now_ms,
			   struct line_cid_gate *gate)
{
	if (!gate)
		return;
	if (!r->cid_open) {
		if (r->cid_done || !r->cfg.cid_present ||
		    r->bursts != r->cfg.cid_at_ring)
			return;
		r->cid_open = true;
		r->cid_open_ms = now_ms;
		gate->request = true;
		gate->complete = false;
		gate->after_burst = r->bursts > 0;
		return;
	}
	if (!gate->complete &&
	    now_ms - r->cid_open_ms < LINE_CID_WINDOW_MAX_MS)
		return;
	r->cid_open = false;
	r->cid_done = true;
	gate->request = false;
	gate->complete = false;
}

static void silence_run(struct line_ring *r, uint32_t now_ms,
			struct line_cid_gate *gate, struct line_out *out)
{
	cid_window_run(r, now_ms, gate);
	if (r->cid_open)
		return;
	if (now_ms - r->phase_ms < r->phase_len_ms)
		return;
	burst_start(r, now_ms, out);
}

static int cadence_check(const struct udsp_cadence *tbl, uint8_t count)
{
	uint8_t i;

	if (count > UDSP_CADENCE_MAX)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		if (!tbl[i].on_ms)
			return -EINVAL;
	}
	return 0;
}

void line_ring_init(struct line_ring *r, unsigned line)
{
	memset(r, 0, sizeof(*r));
	r->line = line;
}

bool line_ring_active(const struct line_ring *r)
{
	return r->stage != LINE_RING_OFF;
}

int line_ring_start(struct line_ring *r, const struct udsp_ring *cfg, uint32_t now_ms,
		    struct line_cid_gate *gate, struct line_out *out)
{
	unsigned line = r->line;

	if (!cfg->cadence_count)
		return -EINVAL;
	if (cadence_check(cfg->cadence, cfg->cadence_count) ||
	    cadence_check(cfg->first, cfg->first_count))
		return -EINVAL;
	/* A restart over a live session must not lose the relay state with a
	 * memset: the relay is physically on, so the next burst would emit a
	 * second on with no off between them, and the session would end with no
	 * UDSP_EV_RING_END at all. */
	if (r->stage != LINE_RING_OFF)
		ring_end(r, now_ms, LINE_RING_RESTARTED, gate, out);
	memset(r, 0, sizeof(*r));
	r->line = line;
	r->cfg = *cfg;
	r->stage = LINE_RING_SILENCE;
	r->first_cycle = true;
	r->phase_ms = now_ms;
	r->start_ms = now_ms;
	if (gate) {
		gate->request = false;
		gate->complete = false;
	}
	line_ring_tick(r, now_ms, NULL, gate, out);
	return 0;
}

void line_ring_stop(struct line_ring *r, uint32_t now_ms,
		    enum line_ring_reason reason, struct line_cid_gate *gate,
		    struct line_out *out)
{
	if (r->stage == LINE_RING_OFF)
		return;
	ring_end(r, now_ms, reason, gate, out);
}

void line_ring_tick(struct line_ring *r, uint32_t now_ms,
		    const struct line_hook_report *hook, struct line_cid_gate *gate,
		    struct line_out *out)
{
	if (r->stage == LINE_RING_OFF)
		return;
	if (hook && hook->offhook_edge) {
		ring_end(r, now_ms, LINE_RING_ANSWERED, gate, out);
		return;
	}
	if (r->cfg.duration_ms && now_ms - r->start_ms >= r->cfg.duration_ms) {
		ring_end(r, now_ms, LINE_RING_EXPIRED, gate, out);
		return;
	}
	if (r->stage == LINE_RING_BURST) {
		burst_run(r, now_ms, out);
		/* The break must be evaluated in the same tick that opened it, or
		 * the CID window would start one tick into the silence. */
		if (r->stage != LINE_RING_SILENCE)
			return;
	}
	silence_run(r, now_ms, gate, out);
}
