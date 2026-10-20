// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Line control suite: the hook FSM, the ring cadence engine and the CID
 * sequencer.
 *
 * The clock is simulated. Every case ticks the three machines in the order
 * that line.h section 2 fixes, and drains the journal. Nothing sleeps, and
 * every assertion is on a stamp that the test chose.
 *
 * dsp/cid.c implements dsp/cid.h. This file carries test-local stubs of its six
 * entry points. The suite therefore checks what the sequencer hands the engine,
 * without a dependency on the framing. The sequencer arms the burst, and the
 * engine plays it. udsp_cid_msg_build and udsp_cid_vmwi_build return
 * recognizable and distinct byte ramps. The test feeds the armed record back
 * into the modulator stub, to prove that the record carries exactly those
 * bytes. The suite must therefore never link dsp/cid.c or dsp/fsk_mod.c.
 */
#include <stdlib.h>
#include <string.h>

#include "../line/line.h"
#include "util.h"

/* --- test-local dsp/cid.h stubs --- */

static size_t stub_msg_len;
static unsigned stub_msg_calls;
static bool stub_msg_type2;
static int stub_msg_fmt;
static uint8_t stub_msg_bytes[UDSP_CID_MSG_MAX];

static size_t stub_dtmf_len;
static unsigned stub_dtmf_calls;
static char stub_dtmf_digits[LINE_CID_DTMF_MAX];

static unsigned stub_fsk_init_calls;
static int stub_fsk_std;
static int stub_fsk_level;
static unsigned stub_fsk_seizure;
static unsigned stub_fsk_mark;
static size_t stub_fsk_data_len;
static uint8_t stub_fsk_data[UDSP_CID_MSG_MAX];

size_t udsp_cid_msg_build(const struct udsp_cid *cid, enum udsp_cid_format fmt,
			  bool type2, uint8_t *out, size_t cap)
{
	size_t i;

	stub_msg_calls++;
	stub_msg_type2 = type2;
	stub_msg_fmt = (int)fmt;
	if (!stub_msg_len || stub_msg_len > cap)
		return 0;
	for (i = 0; i < stub_msg_len; i++)
		out[i] = (uint8_t)(0x80u + i);
	memcpy(stub_msg_bytes, out, stub_msg_len);
	return stub_msg_len;
}

static size_t stub_vmwi_len;
static unsigned stub_vmwi_calls;
static bool stub_vmwi_on;
static int stub_vmwi_fmt;
static uint8_t stub_vmwi_bytes[UDSP_CID_MSG_MAX];

size_t udsp_cid_vmwi_build(bool on, const char *datetime, enum udsp_cid_format fmt,
			   uint8_t *out, size_t cap)
{
	size_t i;

	stub_vmwi_calls++;
	stub_vmwi_on = on;
	stub_vmwi_fmt = (int)fmt;
	if (!stub_vmwi_len || stub_vmwi_len > cap)
		return 0;
	for (i = 0; i < stub_vmwi_len; i++)
		out[i] = (uint8_t)((on ? 0x40u : 0x20u) + i);
	memcpy(stub_vmwi_bytes, out, stub_vmwi_len);
	return stub_vmwi_len;
}

size_t udsp_cid_dtmf_string(const struct udsp_cid *cid, enum udsp_cid_format fmt,
			    char *out, size_t cap)
{
	size_t i;

	stub_dtmf_calls++;
	if (!stub_dtmf_len || stub_dtmf_len > cap)
		return 0;
	for (i = 0; i < stub_dtmf_len; i++)
		out[i] = (char)('0' + (i % 10));
	memcpy(stub_dtmf_digits, out, stub_dtmf_len);
	return stub_dtmf_len;
}

void udsp_fsk_mod_init(struct udsp_fsk_mod *m, enum udsp_fsk_std std,
		       int level_dbm0, unsigned seizure_bits, unsigned mark_bits,
		       unsigned rate_hz)
{
	memset(m, 0, sizeof(*m));
	stub_fsk_init_calls++;
	stub_fsk_std = (int)std;
	stub_fsk_level = level_dbm0;
	stub_fsk_seizure = seizure_bits;
	stub_fsk_mark = mark_bits;
	stub_fsk_data_len = 0;
}

size_t udsp_fsk_mod_data(struct udsp_fsk_mod *m, const uint8_t *bytes, size_t len)
{
	if (len > sizeof(stub_fsk_data))
		len = sizeof(stub_fsk_data);
	memcpy(stub_fsk_data, bytes, len);
	stub_fsk_data_len = len;
	return len;
}

bool udsp_fsk_mod_run(struct udsp_fsk_mod *m, int16_t *frame, unsigned n)
{
	return false;
}

/* --- journal --- */

#define JOURNAL_MAX	512u

static struct line_rec journal[JOURNAL_MAX];
static unsigned journal_count;
static unsigned journal_dropped;

static void journal_reset(void)
{
	journal_count = 0;
	journal_dropped = 0;
	memset(journal, 0, sizeof(journal));
}

static void journal_drain(struct line_out *out)
{
	unsigned i;

	for (i = 0; i < out->count && journal_count < JOURNAL_MAX; i++)
		journal[journal_count++] = out->rec[i];
	journal_dropped += out->dropped;
	line_out_reset(out);
}

static int cmd_find(unsigned from, enum line_cmd_type type)
{
	unsigned i;

	for (i = from; i < journal_count; i++) {
		if (journal[i].kind == LINE_REC_CMD && journal[i].u.cmd.type == type)
			return (int)i;
	}
	return -1;
}

static int relay_nth(unsigned n, bool on)
{
	unsigned i, seen = 0;

	for (i = 0; i < journal_count; i++) {
		if (journal[i].kind != LINE_REC_CMD ||
		    journal[i].u.cmd.type != LINE_CMD_RING_RELAY)
			continue;
		if (journal[i].u.cmd.u.relay.on != on)
			continue;
		if (seen++ == n)
			return (int)i;
	}
	return -1;
}

static int ev_find(unsigned from, enum udsp_event_type type)
{
	unsigned i;

	for (i = from; i < journal_count; i++) {
		if (journal[i].kind == LINE_REC_EVENT && journal[i].u.event.type == type)
			return (int)i;
	}
	return -1;
}

static unsigned cmd_total(enum line_cmd_type type)
{
	unsigned i, n = 0;

	for (i = 0; i < journal_count; i++) {
		if (journal[i].kind == LINE_REC_CMD && journal[i].u.cmd.type == type)
			n++;
	}
	return n;
}

static unsigned mute_total(bool on)
{
	unsigned i, n = 0;

	for (i = 0; i < journal_count; i++) {
		if (journal[i].kind != LINE_REC_CMD ||
		    journal[i].u.cmd.type != LINE_CMD_MUTE)
			continue;
		if (journal[i].u.cmd.u.mute.on == on)
			n++;
	}
	return n;
}

static unsigned ev_total(enum udsp_event_type type)
{
	unsigned i, n = 0;

	for (i = 0; i < journal_count; i++) {
		if (journal[i].kind == LINE_REC_EVENT && journal[i].u.event.type == type)
			n++;
	}
	return n;
}

/*
 * Standing invariant: ring-relay records strictly alternate and start with an
 * on, so no session can leave the relay energized or emit two ons in a row.
 */
static bool relay_balanced(void)
{
	bool want_on = true;
	unsigned i;

	for (i = 0; i < journal_count; i++) {
		if (journal[i].kind != LINE_REC_CMD ||
		    journal[i].u.cmd.type != LINE_CMD_RING_RELAY)
			continue;
		if (journal[i].u.cmd.u.relay.on != want_on)
			return false;
		want_on = !want_on;
	}
	return true;
}

static unsigned hook_total(enum udsp_hook state)
{
	unsigned i, n = 0;

	for (i = 0; i < journal_count; i++) {
		if (journal[i].kind != LINE_REC_EVENT ||
		    journal[i].u.event.type != UDSP_EV_HOOK)
			continue;
		if (journal[i].u.event.u.hook.state == state)
			n++;
	}
	return n;
}

static int hook_find(enum udsp_hook state)
{
	unsigned i;

	for (i = 0; i < journal_count; i++) {
		if (journal[i].kind == LINE_REC_EVENT &&
		    journal[i].u.event.type == UDSP_EV_HOOK &&
		    journal[i].u.event.u.hook.state == state)
			return (int)i;
	}
	return -1;
}

static uint32_t rec_stamp(int index)
{
	return index >= 0 ? journal[index].stamp_ms : UINT32_MAX;
}

/* --- simulation --- */

struct line_sim {
	struct line_hook hook;
	struct line_ring ring;
	struct line_cidseq cid;
	struct line_cid_gate gate;
	struct line_out out;
	uint32_t now;
	uint32_t step;
	bool raw;
	/* The sampler's record for the hook machine (line.h struct
	 * line_hook_sample), when a case drives one: known says the level is
	 * confirmed only to known_ms, else to now; gap and flags ride the
	 * next tick alone and are cleared by it. */
	bool known;
	uint32_t known_ms;
	uint16_t gap_ms;
	uint8_t flags;
	/* The last report that measured an open (line.h line_hook_report
	 * measured), for the cases that pin the bounds and the credits. */
	struct line_hook_report measured;
};

static void sim_init(struct line_sim *s, uint32_t step, bool raw)
{
	memset(s, 0, sizeof(*s));
	line_hook_init(&s->hook, NULL);
	line_ring_init(&s->ring, 0);
	line_cidseq_init(&s->cid, 0);
	s->step = step;
	s->raw = raw;
	journal_reset();
}

static void sim_tick(struct line_sim *s)
{
	struct line_hook_sample sample = {
		.known_ms = s->known ? s->known_ms : s->now,
		.gap_ms = s->gap_ms,
		.flags = s->flags,
	};
	struct line_hook_report rep = line_hook_tick(&s->hook, s->now, s->raw,
						     &sample);

	s->gap_ms = 0;
	s->flags = 0;
	if (rep.measured)
		s->measured = rep;
	line_ring_tick(&s->ring, s->now, &rep, &s->gate, &s->out);
	line_cidseq_tick(&s->cid, s->now, &rep, &s->gate, &s->out);
	line_hook_emit(&rep, 0, s->now, &s->out);
	journal_drain(&s->out);
	s->now += s->step;
}

static void sim_to(struct line_sim *s, bool raw, uint32_t until_ms)
{
	s->raw = raw;
	while (s->now <= until_ms)
		sim_tick(s);
}

static void sim_for(struct line_sim *s, bool raw, uint32_t ms)
{
	sim_to(s, raw, s->now + ms);
}

static void region_test_fill(struct udsp_region *r)
{
	struct udsp_region_tone *cw = &r->tone[UDSP_TONE_CALL_WAITING];

	memset(r, 0, sizeof(*r));
	r->cid.format = UDSP_CIDF_BELLCORE_FSK;
	r->cid.wait_ms = 500;
	r->cid.ack_timeout_ms = 400;
	r->cid.seizure_bits = 300;
	r->cid.mark_bits = 180;
	r->hook.debounce_ms = 10;
	r->hook.flash_min_ms = 300;
	r->hook.flash_max_ms = 1300;
	r->hook.release_min_ms = 1301;
	r->hook.pulse_break_min_ms = 40;
	r->hook.pulse_break_max_ms = 100;
	r->hook.pulse_make_max_ms = 60;
	r->hook.pulse_interdigit_ms = 300;
	cw->present = true;
	cw->desc.tone = UDSP_TONE_CALL_WAITING;
	cw->desc.frequency_count = 1;
	cw->desc.frequency_hz[0] = 440;
	cw->desc.level_dbm0[0] = -13;
	cw->desc.cadence_count = 1;
	cw->desc.cadence[0].make_ms = 300;
	cw->desc.cadence[0].break_ms = 9700;
}

/* --- hook cases --- */

static void hook_boot_test(void)
{
	struct line_sim s;

	sim_init(&s, 10, false);
	sim_to(&s, false, 500);
	CASE("hook boot state reported once on hook");
	CHECK_UINT(ev_total(UDSP_EV_HOOK), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 1);
	CHECK_UINT(rec_stamp(ev_find(0, UDSP_EV_HOOK)), 10);

	sim_init(&s, 10, true);
	sim_to(&s, true, 500);
	CASE("hook boot state reported once off hook");
	CHECK_UINT(ev_total(UDSP_EV_HOOK), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_SEIZE), 1);
}

static void hook_debounce_test(void)
{
	struct line_sim s;

	sim_init(&s, 10, false);
	sim_to(&s, false, 100);
	journal_reset();
	/* sim_for is relative: sim_to with an absolute stamp already behind the
	 * clock would run no tick at all and present no glitch. */
	sim_for(&s, true, 0);		/* exactly one tick of raw off-hook */
	CHECK_UINT(s.now, 120);
	sim_for(&s, false, 500);
	CASE("hook debounce rides out a one tick glitch");
	CHECK_UINT(ev_total(UDSP_EV_HOOK), 0);
	CHECK(!s.hook.stable);
	CHECK_UINT(s.hook.stage, LINE_HOOK_IDLE);
}

static void hook_seize_test(void)
{
	struct line_sim s;

	sim_init(&s, 10, false);
	sim_to(&s, false, 100);
	journal_reset();
	sim_to(&s, true, 500);
	CASE("hook seize after the idle state");
	CHECK_UINT(ev_total(UDSP_EV_HOOK), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_SEIZE), 1);
	CHECK_UINT(rec_stamp(ev_find(0, UDSP_EV_HOOK)), 120);
}

/* Seize the line and leave the journal empty at t = 100. */
static void hook_seized_start(struct line_sim *s, uint32_t step)
{
	sim_init(s, step, true);
	sim_to(s, true, 99);
	journal_reset();
}

static void hook_flash_test(void)
{
	struct line_sim s;

	hook_seized_start(&s, 1);
	sim_to(&s, false, 399);		/* open 100..400 = 300 ms */
	sim_to(&s, true, 500);
	CASE("hook flash at flash_min is a flash");
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 0);
	/* The ideal sampler carries no gap, so the low bound is the open and
	 * the high bound adds the tolerance alone. */
	CHECK(s.measured.measured);
	CHECK_UINT(s.measured.open_lo_ms, 300);
	CHECK_UINT(s.measured.open_hi_ms, 350);
	CHECK_UINT(s.measured.open_gap_ms, 0);
	CHECK_UINT(s.measured.close_gap_ms, 0);
	CHECK_UINT(s.measured.open_class, LINE_HOOK_OPEN_FLASH);

	/* The floor: flash_min_ms minus LINE_HOOK_FLASH_TOL_MS on the measured
	 * open (hook.c, "Windows"). */
	hook_seized_start(&s, 1);
	sim_to(&s, false, 349);		/* open 250 ms */
	sim_to(&s, true, 500);
	CASE("hook open at flash_min minus the tolerance is a flash");
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 1);
	CHECK_UINT(s.measured.open_lo_ms, 250);
	CHECK_UINT(s.measured.open_hi_ms, 300);
	CHECK_UINT(s.measured.open_class, LINE_HOOK_OPEN_FLASH);

	hook_seized_start(&s, 1);
	sim_to(&s, false, 348);		/* open 249 ms */
	sim_to(&s, true, 500);
	CASE("hook one millisecond below the tolerance floor is not a flash");
	CHECK_UINT(ev_total(UDSP_EV_HOOK), 0);
	CHECK_UINT(s.measured.open_lo_ms, 249);
	CHECK_UINT(s.measured.open_hi_ms, 299);
	CHECK_UINT(s.measured.open_class, LINE_HOOK_OPEN_NONE);

	hook_seized_start(&s, 1);
	sim_to(&s, false, 1399);	/* open 1300 ms */
	sim_to(&s, true, 1500);
	CASE("hook flash at flash_max is a flash");
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 0);
}

static void hook_release_test(void)
{
	struct line_sim s;

	hook_seized_start(&s, 1);
	sim_to(&s, false, 1400);	/* the longest open seen is 1300 ms */
	CASE("hook one millisecond below release_min is not a release");
	CHECK_UINT(ev_total(UDSP_EV_HOOK), 0);

	hook_seized_start(&s, 1);
	sim_to(&s, false, 1401);	/* the open reaches 1301 ms */
	CASE("hook release at release_min");
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 1);
	CHECK_UINT(rec_stamp(ev_find(0, UDSP_EV_HOOK)), 1401);
	CHECK_UINT(s.hook.stage, LINE_HOOK_IDLE);
}

static void pulse_train_run(struct line_sim *s, unsigned pulses)
{
	unsigned i;

	for (i = 0; i < pulses; i++) {
		sim_for(s, false, 50);	/* 60 ms break at 10 pps */
		sim_for(s, true, 30);	/* 40 ms make */
	}
}

/* The built-in windows leave pulse classification off, so a pulse case turns
 * it on the way the engine does for a region whose [hook] pulse_enable is on
 * (core/chan.c chan_hook_pulse_apply). */
static void hook_seized_pulse_start(struct line_sim *s)
{
	sim_init(s, 10, true);
	line_hook_pulse_enable(&s->hook, true);
	sim_to(s, true, 99);
	journal_reset();
}

/* One tick that carries the sampler's record: the level, the gap since the
 * previous sample and the flags (line.h LINE_HOOK_SAMPLE_*). */
static void sim_sample(struct line_sim *s, bool raw, uint16_t gap_ms,
		       uint8_t flags)
{
	s->raw = raw;
	s->gap_ms = gap_ms;
	s->flags = flags;
	sim_tick(s);
}

/*
 * The minimum of the flash window against a real sampler (hook.c, "Windows").
 * The machine stores the gap of the sampler only on the tick where the raw
 * level changes. It ignores a gap at or below LINE_HOOK_GAP_MAX_MS. The test
 * therefore models the tick sampler with one sim_sample call per edge and
 * plain ticks in between.
 */
static void hook_sampler_flash_test(void)
{
	struct line_sim s;

	hook_seized_start(&s, 10);
	sim_sample(&s, false, 10, 0);	/* the open starts at 100 */
	sim_to(&s, false, 399);
	sim_sample(&s, true, 10, 0);	/* the close is found at 400 */
	sim_to(&s, true, 500);
	CASE("hook flash of 300 ms under the tick sampler is a flash");
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 0);
	CHECK_UINT(s.measured.open_lo_ms, 290);
	CHECK_UINT(s.measured.open_hi_ms, 350);
	CHECK_UINT(s.measured.open_gap_ms, 10);
	CHECK_UINT(s.measured.close_gap_ms, 10);
	CHECK_UINT(s.measured.open_class, LINE_HOOK_OPEN_FLASH);

	/* The floor under the tick sampler: the closing gap and the tolerance
	 * are the whole credit; the opening gap is reported, not credited. */
	hook_seized_start(&s, 10);
	sim_sample(&s, false, 10, 0);
	sim_to(&s, false, 349);
	sim_sample(&s, true, 10, 0);	/* 240 ms measured, 300 credited */
	sim_to(&s, true, 500);
	CASE("hook open of 250 ms under the tick sampler is a flash");
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 1);
	CHECK_UINT(s.measured.open_lo_ms, 240);
	CHECK_UINT(s.measured.open_hi_ms, 300);
	CHECK_UINT(s.measured.open_gap_ms, 10);
	CHECK_UINT(s.measured.close_gap_ms, 10);
	CHECK_UINT(s.measured.open_class, LINE_HOOK_OPEN_FLASH);

	hook_seized_start(&s, 10);
	sim_sample(&s, false, 10, 0);
	sim_to(&s, false, 339);
	sim_sample(&s, true, 10, 0);	/* 230 ms measured, 290 credited */
	sim_to(&s, true, 500);
	CASE("hook open of 240 ms under the tick sampler is not a flash");
	CHECK_UINT(ev_total(UDSP_EV_HOOK), 0);
	CHECK_UINT(s.measured.open_lo_ms, 230);
	CHECK_UINT(s.measured.open_hi_ms, 290);
	CHECK_UINT(s.measured.open_class, LINE_HOOK_OPEN_NONE);

	/* The per-tick sampler stamps a sample at its tick base plus a sub-tick
	 * offset, so two samples run 1 to 19 ms apart (core/clock.h
	 * udsp_clock_stamp_ms). The opening edge is found 19 ms after the
	 * previous sample and the closing edge 9 ms after its own. */
	hook_seized_start(&s, 1);
	sim_to(&s, true, 118);
	sim_sample(&s, false, 19, 0);
	sim_to(&s, false, 398);
	sim_sample(&s, false, 10, 0);
	sim_to(&s, false, 407);
	sim_sample(&s, true, 9, 0);	/* 280 ms measured, 339 credited */
	sim_to(&s, true, 500);
	CASE("hook flash under a jittering tick sampler");
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 1);

	hook_seized_start(&s, 10);
	sim_sample(&s, false, 250, 0);	/* the IDLE cadence before the edge */
	sim_to(&s, false, 399);
	sim_sample(&s, true, 10, 0);
	sim_to(&s, true, 500);
	CASE("hook flash whose opening edge follows the idle cadence");
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 1);
	/* The idle cadence is reported as it was sampled and not credited:
	 * 290 measured, 10 plus the tolerance credited. */
	CHECK_UINT(s.measured.open_gap_ms, 250);
	CHECK_UINT(s.measured.open_lo_ms, 290);
	CHECK_UINT(s.measured.open_hi_ms, 350);

	hook_seized_start(&s, 10);
	sim_to(&s, true, 119);
	sim_sample(&s, false, 250, 0);	/* the edge at 100, the sample at 120 */
	sim_to(&s, false, 399);
	sim_sample(&s, true, 10, 0);	/* 270 ms measured, 330 credited */
	sim_to(&s, true, 500);
	CASE("hook flash whose opening edge the interrupt found 20 ms late");
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 1);

	hook_seized_start(&s, 10);
	sim_sample(&s, false, 250, 0);
	sim_to(&s, false, 159);
	sim_sample(&s, true, 10, 0);	/* 50 ms measured, 110 credited */
	sim_to(&s, true, 300);
	CASE("hook tap after the idle cadence is not a flash");
	CHECK_UINT(ev_total(UDSP_EV_HOOK), 0);

	/* The opening edge found 30 ms late reads 260 ms; the tolerance, not
	 * the opening gap, carries it over the window. */
	hook_seized_start(&s, 10);
	sim_to(&s, true, 129);
	sim_sample(&s, false, 250, 0);
	sim_to(&s, false, 399);
	sim_sample(&s, true, 10, 0);	/* 260 ms measured, 320 credited */
	sim_to(&s, true, 500);
	CASE("hook flash whose opening edge the interrupt found 30 ms late");
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 1);

	/* A stall while the opening debounce is still pending: the re-base
	 * moves the raw edge, so the open that the debounce then confirms
	 * starts at a confirmed instant, reports no opening gap, and the floor
	 * is measured from it. */
	hook_seized_start(&s, 10);
	sim_sample(&s, false, 10, 0);
	s.known = true;
	s.known_ms = 100;
	sim_for(&s, false, 1090);
	s.known = false;
	sim_sample(&s, false, 1100, LINE_HOOK_SAMPLE_STALL);	/* at 1210 */
	sim_to(&s, false, 1459);
	sim_sample(&s, true, 10, 0);	/* 240 ms measured, 300 credited */
	sim_to(&s, true, 1600);
	CASE("hook stall during the opening debounce: the floor holds from the re-base");
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 1);
	CHECK_UINT(s.measured.open_gap_ms, 0);
	CHECK_UINT(s.measured.open_lo_ms, 240);
	CHECK_UINT(s.measured.open_hi_ms, 300);

	hook_seized_start(&s, 10);
	sim_sample(&s, false, 10, 0);
	s.known = true;
	s.known_ms = 100;
	sim_for(&s, false, 1090);
	s.known = false;
	sim_sample(&s, false, 1100, LINE_HOOK_SAMPLE_STALL);	/* at 1210 */
	sim_to(&s, false, 1449);
	sim_sample(&s, true, 10, 0);	/* 230 ms measured, 290 credited */
	sim_to(&s, true, 1600);
	CASE("hook stall during the opening debounce: one tick under the floor is nothing");
	CHECK_UINT(ev_total(UDSP_EV_HOOK), 0);

	/* The re-base of a stall inside an open: the new start is a confirmed
	 * instant, so the close's own gap and the tolerance are the whole
	 * credit. */
	hook_seized_start(&s, 10);
	sim_sample(&s, false, 10, 0);
	sim_to(&s, false, 199);
	s.known = true;
	s.known_ms = s.now - 10;
	sim_for(&s, false, 1000);
	s.known = false;
	sim_sample(&s, false, 1200, LINE_HOOK_SAMPLE_STALL);
	sim_to(&s, false, 1459);
	sim_sample(&s, true, 10, 0);	/* 240 ms measured, 300 credited */
	sim_to(&s, true, 1600);
	CASE("hook re-based open: the floor holds from the re-base");
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 1);
	CHECK_UINT(s.measured.open_gap_ms, 0);
	CHECK_UINT(s.measured.open_lo_ms, 240);
	CHECK_UINT(s.measured.open_hi_ms, 300);

	hook_seized_start(&s, 10);
	sim_sample(&s, false, 10, 0);
	sim_to(&s, false, 199);
	s.known = true;
	s.known_ms = s.now - 10;
	sim_for(&s, false, 1000);
	s.known = false;
	sim_sample(&s, false, 1200, LINE_HOOK_SAMPLE_STALL);
	sim_to(&s, false, 1449);
	sim_sample(&s, true, 10, 0);	/* 230 ms measured, 290 credited */
	sim_to(&s, true, 1600);
	CASE("hook re-based open one tick under the floor is nothing");
	CHECK_UINT(ev_total(UDSP_EV_HOOK), 0);

	hook_seized_start(&s, 10);
	sim_sample(&s, false, 10, 0);
	sim_to(&s, false, 1399);
	sim_sample(&s, true, 10, 0);	/* 1290 ms measured */
	sim_to(&s, true, 1500);
	CASE("hook flash at flash_max under the tick sampler");
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 0);

	hook_seized_start(&s, 10);
	sim_sample(&s, false, 10, 0);
	sim_to(&s, false, 1409);
	sim_sample(&s, true, 10, 0);	/* the open freezes at the raw edge */
	sim_to(&s, true, 1520);
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 0);

	hook_seized_start(&s, 10);
	sim_sample(&s, false, 10, 0);
	sim_to(&s, false, 1419);	/* the release at the 1410 sample */
	sim_sample(&s, true, 10, 0);
	sim_to(&s, true, 1520);
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 0);
}

/*
 * The gap verdict and the bounded durations of hook.c ("Gaps"). Every case
 * drives the machine with the record of the sampler directly. The test
 * therefore checks the rules with no engine and no slic thread between the
 * rules and the check.
 */
static void hook_gap_test(void)
{
	struct line_sim s;
	uint32_t at;
	int index;

	hook_seized_pulse_start(&s);
	pulse_train_run(&s, 2);
	at = s.now;
	sim_sample(&s, true, 0, LINE_HOOK_SAMPLE_GAP);
	pulse_train_run(&s, 2);
	sim_for(&s, true, 400);
	CASE("hook lost pair inside a train drops it with one error at once");
	CHECK_UINT(hook_total(UDSP_HOOK_ERROR), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_PULSE), 0);
	CHECK_UINT(rec_stamp(0), at);
	journal_reset();
	pulse_train_run(&s, 3);
	sim_for(&s, true, 400);
	CASE("hook train after a dropped one classifies again");
	index = ev_find(0, UDSP_EV_HOOK);
	CHECK_UINT(hook_total(UDSP_HOOK_PULSE), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_ERROR), 0);
	CHECK(index >= 0);
	CHECK_UINT(journal[index >= 0 ? index : 0].u.event.u.hook.pulses, 3);

	hook_seized_pulse_start(&s);
	sim_sample(&s, true, 0, LINE_HOOK_SAMPLE_GAP);
	sim_for(&s, true, 400);
	pulse_train_run(&s, 2);
	sim_for(&s, true, 400);
	CASE("hook lost pair on a seized line poisons until the interdigit gap");
	CHECK_UINT(hook_total(UDSP_HOOK_ERROR), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_PULSE), 1);

	sim_init(&s, 10, false);
	line_hook_pulse_enable(&s.hook, true);
	sim_to(&s, false, 99);
	journal_reset();
	sim_sample(&s, false, 0, LINE_HOOK_SAMPLE_GAP);
	sim_for(&s, false, 100);
	CASE("hook lost pair on an on-hook line is ignored");
	CHECK_UINT(ev_total(UDSP_EV_HOOK), 0);

	hook_seized_pulse_start(&s);
	sim_sample(&s, false, 250, 0);
	sim_for(&s, false, 40);
	sim_for(&s, true, 30);
	pulse_train_run(&s, 2);
	sim_for(&s, true, 400);
	CASE("hook stamp gap on the first break of a train does not poison");
	index = ev_find(0, UDSP_EV_HOOK);
	CHECK_UINT(hook_total(UDSP_HOOK_ERROR), 0);
	CHECK_UINT(hook_total(UDSP_HOOK_PULSE), 1);
	CHECK(index >= 0);
	CHECK_UINT(journal[index >= 0 ? index : 0].u.event.u.hook.pulses, 3);

	hook_seized_pulse_start(&s);
	pulse_train_run(&s, 2);
	sim_sample(&s, false, 60, 0);
	sim_for(&s, false, 40);
	sim_for(&s, true, 400);
	CASE("hook stamp gap inside a train drops it with one error");
	CHECK_UINT(hook_total(UDSP_HOOK_ERROR), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_PULSE), 0);

	hook_seized_pulse_start(&s);
	pulse_train_run(&s, 1);
	sim_for(&s, false, 30);
	sim_sample(&s, false, 10, LINE_HOOK_SAMPLE_LOST);
	sim_for(&s, false, 20);
	sim_for(&s, true, 400);
	CASE("hook lost record inside a break drops the train");
	CHECK_UINT(hook_total(UDSP_HOOK_ERROR), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_PULSE), 0);

	hook_seized_pulse_start(&s);
	sim_for(&s, false, 20);
	s.known = true;
	s.known_ms = s.now - 10;
	sim_for(&s, false, 1500);
	CASE("hook durations stop at the last sample: no release while stalled");
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 0);
	CHECK_UINT(hook_total(UDSP_HOOK_PULSE), 0);
	s.known = false;
	sim_for(&s, false, 10);
	CASE("hook release once a sample confirms the open");
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 1);

	hook_seized_pulse_start(&s);
	sim_for(&s, false, 20);
	s.known = true;
	s.known_ms = s.now - 10;
	sim_for(&s, false, 1400);
	s.known = false;
	sim_sample(&s, true, 1400, 0);
	sim_for(&s, true, 400);
	CASE("hook closing edge found late is read at its earliest instant");
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 0);
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 0);
	CHECK_UINT(hook_total(UDSP_HOOK_PULSE), 0);
	CHECK_UINT(hook_total(UDSP_HOOK_ERROR), 1);

	hook_seized_pulse_start(&s);
	sim_sample(&s, true, 0, LINE_HOOK_SAMPLE_GAP);
	sim_for(&s, false, 500);
	sim_for(&s, true, 100);
	CASE("hook flash is unchanged by a poisoned train");
	CHECK_UINT(hook_total(UDSP_HOOK_ERROR), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 1);

	/* The bench shapes of the stall rules: a sampler held for seconds
	 * under a sibling's line test while the caller keeps dialing. The
	 * sample that ends the stall carries the sampler's STALL mark; its
	 * stamp gap alone would look like the idle cadence under the
	 * harness's fast engine clock. */
	hook_seized_pulse_start(&s);
	s.known = true;
	s.known_ms = s.now - 10;
	sim_for(&s, true, 8747);
	s.known = false;
	sim_sample(&s, false, 8747, LINE_HOOK_SAMPLE_STALL);
	sim_for(&s, false, 40);
	sim_for(&s, true, 40);
	pulse_train_run(&s, 7);
	sim_for(&s, true, 400);
	CASE("hook stall on a quiet seized line drops the break it returns inside");
	CHECK_UINT(hook_total(UDSP_HOOK_ERROR), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_PULSE), 0);
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 0);
	pulse_train_run(&s, 3);
	sim_for(&s, true, 400);
	CASE("hook train after a stall-dropped one classifies again");
	CHECK_UINT(hook_total(UDSP_HOOK_PULSE), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_ERROR), 1);

	hook_seized_pulse_start(&s);
	sim_sample(&s, false, 250, 0);
	sim_for(&s, false, 40);
	sim_for(&s, true, 30);
	pulse_train_run(&s, 2);
	sim_for(&s, true, 400);
	CASE("hook idle cadence gap on a quiet seized line is not a stall");
	CHECK_UINT(hook_total(UDSP_HOOK_ERROR), 0);
	CHECK_UINT(hook_total(UDSP_HOOK_PULSE), 1);

	hook_seized_pulse_start(&s);
	pulse_train_run(&s, 7);
	s.known = true;
	s.known_ms = s.now - 10;
	sim_for(&s, true, 8787);
	s.known = false;
	sim_sample(&s, true, 8787, LINE_HOOK_SAMPLE_STALL);
	sim_sample(&s, true, 8787, LINE_HOOK_SAMPLE_GAP | LINE_HOOK_SAMPLE_STALL);
	pulse_train_run(&s, 6);
	sim_for(&s, true, 400);
	CASE("hook stall inside a train drops both halves before the lost pair record");
	CHECK_UINT(hook_total(UDSP_HOOK_ERROR), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_PULSE), 0);

	hook_seized_pulse_start(&s);
	sim_for(&s, false, 500);
	s.known = true;
	s.known_ms = s.now - 10;
	sim_for(&s, false, 8747);
	s.known = false;
	sim_sample(&s, false, 8747, LINE_HOOK_SAMPLE_STALL);
	sim_for(&s, false, 1290);
	CASE("hook stall on an open line: no release before release_min of confirmed open");
	CHECK_UINT(hook_total(UDSP_HOOK_ERROR), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 0);
	sim_for(&s, false, 20);
	CASE("hook stall on an open line: the release counts from the sampler's return");
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 1);

	sim_init(&s, 10, false);
	line_hook_pulse_enable(&s.hook, true);
	sim_to(&s, false, 99);
	journal_reset();
	s.known = true;
	s.known_ms = s.now - 10;
	sim_for(&s, false, 8747);
	s.known = false;
	sim_sample(&s, true, 8747, LINE_HOOK_SAMPLE_STALL);
	sim_for(&s, true, 40);
	pulse_train_run(&s, 7);
	sim_for(&s, true, 400);
	CASE("hook lift found late seizes and drops the train it returns inside");
	CHECK_UINT(hook_total(UDSP_HOOK_SEIZE), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_ERROR), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_PULSE), 0);
	pulse_train_run(&s, 3);
	sim_for(&s, true, 400);
	CASE("hook train after a late lift classifies again");
	CHECK_UINT(hook_total(UDSP_HOOK_PULSE), 1);

	/* A late lift that the debounce rejects is a transient, not a seize,
	 * and poisons nothing. */
	sim_init(&s, 10, false);
	line_hook_pulse_enable(&s.hook, true);
	sim_to(&s, false, 99);
	journal_reset();
	s.known = true;
	s.known_ms = s.now - 10;
	sim_for(&s, false, 8747);
	s.known = false;
	sim_sample(&s, true, 8747, LINE_HOOK_SAMPLE_STALL);
	sim_for(&s, false, 100);
	CASE("hook late lift rejected by the debounce reports nothing");
	CHECK_UINT(ev_total(UDSP_EV_HOOK), 0);
	sim_for(&s, true, 40);
	pulse_train_run(&s, 3);
	sim_for(&s, true, 400);
	CASE("hook seize after a rejected late lift carries no poison");
	CHECK_UINT(hook_total(UDSP_HOOK_SEIZE), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_ERROR), 0);
	CHECK_UINT(hook_total(UDSP_HOOK_PULSE), 1);

	/* An on-hook edge sampled once, then the stall before the debounce
	 * confirms it: the open must count from the sampler's return, not
	 * from the edge the pending debounce confirms afterwards. */
	hook_seized_pulse_start(&s);
	s.known = true;
	s.known_ms = s.now;
	sim_for(&s, false, 8747);
	s.known = false;
	sim_sample(&s, false, 8747, LINE_HOOK_SAMPLE_STALL);
	sim_for(&s, false, 1290);
	CASE("hook stall across a pending on-hook debounce: no release at the return");
	CHECK_UINT(hook_total(UDSP_HOOK_ERROR), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 0);
	sim_for(&s, false, 30);
	CASE("hook stall across a pending on-hook debounce: release from the return");
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 1);

	/* The opposite pending edge: a break in progress, the closing edge
	 * sampled once, then the stall; the make must count from the return,
	 * so the poison outlives the train's remainder. */
	hook_seized_pulse_start(&s);
	pulse_train_run(&s, 2);
	sim_for(&s, false, 60);
	s.known = true;
	s.known_ms = s.now;
	sim_for(&s, true, 8787);
	s.known = false;
	sim_sample(&s, true, 8787, LINE_HOOK_SAMPLE_STALL);
	pulse_train_run(&s, 6);
	sim_for(&s, true, 400);
	CASE("hook stall across a pending off-hook debounce drops the remainder");
	CHECK_UINT(hook_total(UDSP_HOOK_ERROR), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_PULSE), 0);

	/* A pulse-disabled line keeps the re-base without the verdict. */
	hook_seized_pulse_start(&s);
	line_hook_pulse_enable(&s.hook, false);
	sim_for(&s, false, 200);
	s.known = true;
	s.known_ms = s.now - 10;
	sim_for(&s, false, 8747);
	s.known = false;
	sim_sample(&s, false, 8747, LINE_HOOK_SAMPLE_STALL);
	sim_for(&s, false, 1290);
	CASE("hook stall on a pulse-disabled open line: no release at the return");
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 0);
	CHECK_UINT(hook_total(UDSP_HOOK_ERROR), 0);
	sim_for(&s, false, 30);
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 1);

	/* The engine clock wraps at 49.7 days; an open that starts before the
	 * wrap must still release after release_min_ms. */
	sim_init(&s, 10, true);
	line_hook_pulse_enable(&s.hook, true);
	s.now = UINT32_MAX - 2000;
	sim_for(&s, true, 100);
	journal_reset();
	sim_for(&s, false, 1290);
	CASE("hook open across the clock wrap: no release before release_min");
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 0);
	sim_for(&s, false, 30);
	CASE("hook open across the clock wrap releases after release_min");
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 1);

	/* raw_edge_ms() subtracts a duration from a stamp. A stamp past the
	 * half clock (24.8 days) must still give the edge, or the frozen open
	 * of a closing debounce reads as a release. */
	sim_init(&s, 10, true);
	line_hook_pulse_enable(&s.hook, true);
	s.now = 0x80000000u + 100;
	sim_for(&s, true, 100);
	journal_reset();
	at = s.now;
	sim_sample(&s, false, 10, 0);
	sim_to(&s, false, at + 299);
	sim_sample(&s, true, 10, 0);	/* the close found at at + 300 */
	sim_for(&s, true, 100);
	CASE("hook flash past the half clock is a flash, not a release");
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 0);

	/* The closing sample's stamp is smaller than its gap: the raw edge
	 * lies before the wrap. */
	sim_init(&s, 10, true);
	line_hook_pulse_enable(&s.hook, true);
	s.now = UINT32_MAX - 404;
	sim_for(&s, true, 100);
	journal_reset();
	sim_sample(&s, false, 10, 0);
	/* sim_to() compares stamps unsigned, so the ticks across the wrap are
	 * counted: 29 of them put the close at stamp 5 */
	for (at = 0; at < 29; at++)
		sim_tick(&s);
	sim_sample(&s, true, 10, 0);
	CHECK_UINT(s.now, 15);
	sim_for(&s, true, 100);
	CASE("hook flash across the clock wrap is a flash");
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 0);
}

static void hook_pulse_test(void)
{
	static const unsigned digits[] = { 1, 5, 20 };
	struct line_sim s;
	unsigned d;

	for (d = 0; d < sizeof(digits) / sizeof(digits[0]); d++) {
		char name[64];
		int index;

		hook_seized_pulse_start(&s);
		pulse_train_run(&s, digits[d]);
		sim_for(&s, true, 400);
		snprintf(name, sizeof(name), "hook pulse train reports digit %u",
			 digits[d]);
		CASE(name);
		index = ev_find(0, UDSP_EV_HOOK);
		CHECK_UINT(ev_total(UDSP_EV_HOOK), 1);
		CHECK(index >= 0);
		CHECK_UINT(journal[index >= 0 ? index : 0].u.event.u.hook.state,
			   UDSP_HOOK_PULSE);
		CHECK_UINT(journal[index >= 0 ? index : 0].u.event.u.hook.pulses,
			   digits[d]);
	}

	hook_seized_pulse_start(&s);
	pulse_train_run(&s, 21);
	sim_for(&s, true, 400);
	CASE("hook pulse train above twenty reports an error");
	CHECK_UINT(hook_total(UDSP_HOOK_ERROR), 1);

	hook_seized_pulse_start(&s);
	pulse_train_run(&s, 3);
	sim_for(&s, false, 2000);
	CASE("hook hang up during a train reports the digit then the release");
	CHECK_UINT(ev_total(UDSP_EV_HOOK), 2);
	CHECK_UINT(journal[0].u.event.u.hook.state, UDSP_HOOK_PULSE);
	CHECK_UINT(journal[0].u.event.u.hook.pulses, 3);
	CHECK_UINT(journal[1].u.event.u.hook.state, UDSP_HOOK_RELEASE);
	CHECK(rec_stamp(0) < rec_stamp(1));
}

/*
 * A pulse event carries the decadic coding of the timing that counted the
 * train (event.h u.hook.pulse_invert). The built-in timing is direct. The
 * inverting timing has the NZL windows. Every other hook state carries 0.
 */
static void hook_pulse_invert_test(void)
{
	static const struct udsp_region_hook nzl = {
		.pulse_enable = true, .pulse_invert = true, .debounce_ms = 10,
		.flash_min_ms = 460, .flash_max_ms = 800, .release_min_ms = 960,
		.pulse_break_min_ms = 40, .pulse_break_max_ms = 100,
		.pulse_make_max_ms = 60, .pulse_interdigit_ms = 300,
	};
	struct line_sim s;
	unsigned i, others = 0;
	int index;

	hook_seized_pulse_start(&s);
	pulse_train_run(&s, 9);
	sim_for(&s, true, 400);
	CASE("hook pulse of the built-in timing carries the direct coding");
	index = ev_find(0, UDSP_EV_HOOK);
	CHECK(index >= 0);
	CHECK_UINT(journal[index >= 0 ? index : 0].u.event.u.hook.pulses, 9);
	CHECK_UINT(journal[index >= 0 ? index : 0].u.event.u.hook.pulse_invert,
		   0);

	sim_init(&s, 10, false);
	line_hook_init(&s.hook, &nzl);
	sim_to(&s, false, 99);
	sim_for(&s, true, 200);
	pulse_train_run(&s, 9);
	sim_for(&s, true, 400);
	sim_for(&s, false, 1200);
	CASE("hook pulse of the NZL timing carries the inverted coding");
	index = hook_find(UDSP_HOOK_PULSE);
	CHECK_UINT(hook_total(UDSP_HOOK_PULSE), 1);
	CHECK(index >= 0);
	CHECK_UINT(journal[index >= 0 ? index : 0].u.event.u.hook.pulses, 9);
	CHECK_UINT(journal[index >= 0 ? index : 0].u.event.u.hook.pulse_invert,
		   1);

	CASE("hook states other than a pulse carry the direct coding");
	for (i = 0; i < journal_count; i++) {
		const struct udsp_event *ev = &journal[i].u.event;

		if (journal[i].kind != LINE_REC_EVENT ||
		    ev->type != UDSP_EV_HOOK ||
		    ev->u.hook.state == UDSP_HOOK_PULSE)
			continue;
		others++;
		CHECK_UINT(ev->u.hook.pulse_invert, 0);
	}
	CHECK_UINT(others, 3);
}

/*
 * The EU recall window against the on-hook lag of the line circuit (hook.c
 * LINE_HOOK_FLASH_TOL_MS). A 50 ms recall at the terminal reaches the machine
 * as about 16 ms, which is above the debounce. The tolerance carries any
 * confirmed open over the 35 ms minimum. An open that the debounce does not
 * confirm is nothing.
 */
static void hook_recall_lag_test(void)
{
	static const struct udsp_region_hook ets = {
		.debounce_ms = 10, .flash_min_ms = 35, .flash_max_ms = 145,
		.release_min_ms = 490, .pulse_break_min_ms = 42,
		.pulse_break_max_ms = 94, .pulse_make_max_ms = 85,
		.pulse_interdigit_ms = 240,
	};
	struct line_sim s;

	sim_init(&s, 1, true);
	line_hook_init(&s.hook, &ets);
	sim_to(&s, true, 99);
	journal_reset();
	sim_for(&s, false, 15);		/* a 16 ms open */
	sim_for(&s, true, 400);
	CASE("a 16 ms open is a flash in the ETS window");
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_RELEASE), 0);

	sim_init(&s, 1, true);
	line_hook_init(&s.hook, &ets);
	sim_to(&s, true, 99);
	journal_reset();
	sim_for(&s, false, 8);		/* a 9 ms open, under the debounce */
	sim_for(&s, true, 400);
	CASE("a 9 ms open is nothing in the ETS window");
	CHECK_UINT(ev_total(UDSP_EV_HOOK), 0);
}

static void hook_timing_override_test(void)
{
	struct line_sim s;

	hook_seized_start(&s, 1);
	line_hook_timing_set(&s.hook, 200, 700, 701);
	sim_to(&s, false, 799);		/* open 700 ms */
	sim_to(&s, true, 900);
	CASE("hook timing override moves the flash window");
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 1);
}

/*
 * Most European profiles overlap the break window with the recall window.
 * EN 300 001 accepts a recall of 50 to 130 ms against a break of 42 to 94 ms.
 * The classification must therefore follow the configuration, not the timing
 * alone.
 */
static void hook_overlap_test(void)
{
	static const struct udsp_region_hook overlap = {
		.debounce_ms = 10, .flash_min_ms = 50, .flash_max_ms = 130,
		.release_min_ms = 501, .pulse_break_min_ms = 42,
		.pulse_break_max_ms = 94, .pulse_make_max_ms = 85,
		.pulse_interdigit_ms = 240,
	};
	struct line_sim s;

	sim_init(&s, 10, true);
	line_hook_init(&s.hook, &overlap);
	line_hook_pulse_enable(&s.hook, true);
	sim_to(&s, true, 99);
	journal_reset();
	sim_for(&s, false, 50);		/* a 60 ms open, inside both windows */
	sim_for(&s, true, 400);
	CASE("an overlapping open is a pulse while pulse detection is on");
	CHECK_UINT(hook_total(UDSP_HOOK_PULSE), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 0);

	sim_init(&s, 10, true);
	line_hook_init(&s.hook, &overlap);
	line_hook_pulse_enable(&s.hook, false);
	sim_to(&s, true, 99);
	journal_reset();
	sim_for(&s, false, 50);
	sim_for(&s, true, 400);
	CASE("the same open is a flash while pulse detection is off");
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_PULSE), 0);

	/* Nobody called the setter here. The built-in default and the region
	 * default both leave pulse classification off, so only the engine can
	 * turn it on. */
	sim_init(&s, 10, true);
	line_hook_init(&s.hook, &overlap);
	sim_to(&s, true, 99);
	journal_reset();
	sim_for(&s, false, 50);
	sim_for(&s, true, 400);
	CASE("pulse classification is off until the caller asks for it");
	CHECK(!s.hook.pulse_enable);
	CHECK_UINT(hook_total(UDSP_HOOK_FLASH), 1);
	CHECK_UINT(hook_total(UDSP_HOOK_PULSE), 0);
}

/* --- ring cases --- */

static void ring_cfg_fill(struct udsp_ring *ring, uint16_t on_ms, uint16_t off_ms)
{
	memset(ring, 0, sizeof(*ring));
	ring->cadence[0].on_ms = on_ms;
	ring->cadence[0].off_ms = off_ms;
	ring->cadence_count = 1;
}

static void ring_start_at(struct line_sim *s, const struct udsp_ring *ring)
{
	CHECK_UINT(line_ring_start(&s->ring, ring, s->now, &s->gate, &s->out), 0);
	journal_drain(&s->out);
}

static void ring_cadence_test(void)
{
	struct udsp_ring ring;
	struct line_sim s;

	sim_init(&s, 10, false);
	sim_to(&s, false, 90);
	journal_reset();
	ring_cfg_fill(&ring, 2000, 4000);
	ring_start_at(&s, &ring);
	sim_to(&s, false, 14200);
	CASE("ring cadence 2s on 4s off over three cycles");
	CHECK_UINT(ev_total(UDSP_EV_RING_START), 1);
	CHECK_UINT(cmd_total(LINE_CMD_RING_RELAY), 6);
	CHECK_UINT(rec_stamp(relay_nth(0, true)), 100);
	CHECK_UINT(rec_stamp(relay_nth(0, false)), 2100);
	CHECK_UINT(rec_stamp(relay_nth(1, true)), 6100);
	CHECK(relay_balanced());
	CHECK_UINT(rec_stamp(relay_nth(1, false)), 8100);
	CHECK_UINT(rec_stamp(relay_nth(2, true)), 12100);
	CHECK_UINT(rec_stamp(relay_nth(2, false)), 14100);
	CHECK(relay_balanced());
}

static void ring_first_cadence_test(void)
{
	struct udsp_ring ring;
	struct line_sim s;

	sim_init(&s, 10, false);
	sim_to(&s, false, 90);
	journal_reset();
	ring_cfg_fill(&ring, 2000, 4000);
	ring.first[0].on_ms = 1200;
	ring.first[0].off_ms = 4000;
	ring.first_count = 1;
	ring_start_at(&s, &ring);
	sim_to(&s, false, 8000);
	CASE("ring first cadence runs once then the repeating cadence");
	CHECK_UINT(rec_stamp(relay_nth(0, true)), 100);
	CHECK_UINT(rec_stamp(relay_nth(0, false)), 1300);
	CHECK_UINT(rec_stamp(relay_nth(1, true)), 5300);
	CHECK_UINT(rec_stamp(relay_nth(1, false)), 7300);
	CHECK(relay_balanced());
}

static void ring_duration_test(void)
{
	struct udsp_ring ring;
	struct line_sim s;
	int index;

	sim_init(&s, 10, false);
	sim_to(&s, false, 90);
	journal_reset();
	ring_cfg_fill(&ring, 2000, 4000);
	ring.duration_ms = 5000;
	ring_start_at(&s, &ring);
	sim_to(&s, false, 9000);
	CASE("ring duration cap ends the session");
	index = ev_find(0, UDSP_EV_RING_END);
	CHECK(index >= 0);
	CHECK_UINT(rec_stamp(index), 5100);
	CHECK_UINT(journal[index >= 0 ? index : 0].u.event.u.line.state,
		   LINE_RING_EXPIRED);
	CHECK_UINT(journal[index >= 0 ? index : 0].u.event.u.line.ring_count, 1);
	CHECK(!line_ring_active(&s.ring));
	CHECK(relay_balanced());
}

static void ring_trip_test(void)
{
	struct udsp_ring ring;
	struct line_sim s;
	int relay_off, end, seize;

	sim_init(&s, 10, false);
	sim_to(&s, false, 90);
	journal_reset();
	ring_cfg_fill(&ring, 2000, 4000);
	ring_start_at(&s, &ring);
	sim_to(&s, false, 990);
	sim_to(&s, true, 2000);
	CASE("ring trip stops the relay and orders RING_END before the seize");
	relay_off = relay_nth(0, false);
	end = ev_find(0, UDSP_EV_RING_END);
	seize = ev_find(0, UDSP_EV_HOOK);
	CHECK(relay_off >= 0 && end >= 0 && seize >= 0);
	CHECK(relay_off < end);
	CHECK(end < seize);
	CHECK_UINT(rec_stamp(relay_off), 1010);
	CHECK_UINT(rec_stamp(end), 1010);
	CHECK_UINT(rec_stamp(seize), 1010);
	CHECK_UINT(journal[end >= 0 ? end : 0].u.event.u.line.state,
		   LINE_RING_ANSWERED);
	CHECK_UINT(journal[seize >= 0 ? seize : 0].u.event.u.hook.state,
		   UDSP_HOOK_SEIZE);
	CHECK_UINT(ev_total(UDSP_EV_RING_END), 1);
	/* The busiest tick of the suite: nothing was lost to the journal bound. */
	CHECK_UINT(journal_dropped, 0);
}

static void ring_stop_test(void)
{
	struct udsp_ring ring;
	struct line_sim s;
	int index;

	sim_init(&s, 10, false);
	sim_to(&s, false, 90);
	journal_reset();
	ring_cfg_fill(&ring, 2000, 4000);
	ring_start_at(&s, &ring);
	sim_to(&s, false, 6500);
	line_ring_stop(&s.ring, s.now, LINE_RING_STOPPED, &s.gate, &s.out);
	journal_drain(&s.out);
	CASE("ring stop reports the burst count");
	index = ev_find(0, UDSP_EV_RING_END);
	CHECK(index >= 0);
	CHECK_UINT(journal[index >= 0 ? index : 0].u.event.u.line.state,
		   LINE_RING_STOPPED);
	CHECK_UINT(journal[index >= 0 ? index : 0].u.event.u.line.ring_count, 2);
	CHECK(relay_balanced());
}

static void ring_restart_test(void)
{
	struct udsp_ring ring;
	struct line_sim s;
	int end, on2;

	sim_init(&s, 10, false);
	sim_to(&s, false, 90);
	journal_reset();
	ring_cfg_fill(&ring, 2000, 4000);
	ring_start_at(&s, &ring);
	sim_to(&s, false, 990);
	/* A restart mid-burst: the relay is physically on, so the old session
	 * must close before the new one opens. */
	ring_cfg_fill(&ring, 1000, 3000);
	ring_start_at(&s, &ring);
	sim_to(&s, false, 3000);
	CASE("a restart mid-burst closes the old session before the new one");
	end = ev_find(0, UDSP_EV_RING_END);
	CHECK(end >= 0);
	CHECK_UINT(rec_stamp(end), 1000);
	CHECK_UINT(journal[end >= 0 ? end : 0].u.event.u.line.state,
		   LINE_RING_RESTARTED);
	CHECK_UINT(journal[end >= 0 ? end : 0].u.event.u.line.ring_count, 1);
	CHECK_UINT(rec_stamp(relay_nth(0, false)), 1000);
	on2 = relay_nth(1, true);
	CHECK(on2 >= 0 && on2 > end);
	CHECK_UINT(rec_stamp(on2), 1000);
	CHECK_UINT(ev_total(UDSP_EV_RING_START), 2);
	CHECK_UINT(ev_total(UDSP_EV_RING_END), 1);
	CHECK(relay_balanced());

	CASE("a refused restart leaves the running session alone");
	memset(&ring, 0, sizeof(ring));
	CHECK_UINT((unsigned)-line_ring_start(&s.ring, &ring, s.now, &s.gate,
					      &s.out), 22);
	journal_drain(&s.out);
	CHECK(line_ring_active(&s.ring));
	CHECK_UINT(ev_total(UDSP_EV_RING_END), 1);
	CHECK(relay_balanced());
}

static void ring_cid_window_test(void)
{
	struct udsp_region region;
	struct udsp_ring ring;
	struct line_sim s;
	struct udsp_cid cid;

	region_test_fill(&region);
	memset(&cid, 0, sizeof(cid));
	cid.format = UDSP_CIDF_BELLCORE_FSK;
	stub_msg_len = 24;

	sim_init(&s, 10, false);
	sim_to(&s, false, 90);
	journal_reset();
	ring_cfg_fill(&ring, 2000, 4000);
	ring.cid_present = true;
	ring.cid_at_ring = 1;
	ring.cid_wait_ms = 3000;
	CHECK_UINT(line_cidseq_arm(&s.cid, UDSP_CID_TYPE1, &cid, &region,
				   UDSP_LF_FWD_ACTIVE, true, LINE_CID_MWI_NONE, 0), 0);
	ring_start_at(&s, &ring);
	sim_to(&s, false, 2900);
	line_cidseq_burst_done(&s.cid);
	sim_to(&s, false, 8000);
	CASE("cid window opens in break one and the cadence resumes");
	CHECK_UINT(rec_stamp(cmd_find(0, LINE_CMD_LINEFEED)), 2100);
	CHECK_UINT(rec_stamp(cmd_find(0, LINE_CMD_FSK)), 2100 + 500 + LINE_RING_TAIL_MS);
	CHECK_UINT(rec_stamp(ev_find(0, UDSP_EV_CID_SENT)), 2910);
	CHECK_UINT(rec_stamp(relay_nth(1, true)), 6100);
	CHECK_UINT(ev_total(UDSP_EV_RING_START), 1);

	/*
	 * A window before the first burst follows no relay-off, so its pause is
	 * the region value alone. The pause runs from the on-hook transmission
	 * feed the window opens with.
	 */
	sim_init(&s, 10, false);
	sim_to(&s, false, 90);
	journal_reset();
	ring_cfg_fill(&ring, 2000, 4000);
	ring.cid_present = true;
	ring.cid_at_ring = 0;
	CHECK_UINT(line_cidseq_arm(&s.cid, UDSP_CID_TYPE1, &cid, &region,
				   UDSP_LF_FWD_ACTIVE, true, LINE_CID_MWI_NONE, 0), 0);
	ring_start_at(&s, &ring);
	sim_to(&s, false, 900);
	line_cidseq_burst_done(&s.cid);
	sim_to(&s, false, 3000);
	CASE("a window before the first burst takes the plain pause");
	CHECK(cmd_find(0, LINE_CMD_FSK) >= 0);
	CHECK_UINT(rec_stamp(cmd_find(0, LINE_CMD_FSK)) -
		   rec_stamp(cmd_find(0, LINE_CMD_LINEFEED)), 500);
	CHECK(rec_stamp(relay_nth(0, true)) > rec_stamp(cmd_find(0, LINE_CMD_FSK)));

	/*
	 * The cap is the liveness bound and nothing else: the burst here never
	 * reports done, so the window runs the full LINE_CID_WINDOW_MAX_MS and
	 * the cadence resumes after it. cid_wait_ms takes no part; the delay it
	 * carries is a phase inside the window, which the case above covers.
	 */
	sim_init(&s, 10, false);
	sim_to(&s, false, 90);
	journal_reset();
	ring_cfg_fill(&ring, 2000, 1000);
	ring.cid_present = true;
	ring.cid_at_ring = 1;
	ring.cid_wait_ms = 500;
	CHECK_UINT(line_cidseq_arm(&s.cid, UDSP_CID_TYPE1, &cid, &region,
				   UDSP_LF_FWD_ACTIVE, true, LINE_CID_MWI_NONE,
				   ring.cid_wait_ms), 0);
	ring_start_at(&s, &ring);
	sim_to(&s, false, 9000);
	CASE("the liveness cap ends a window the sequencer never completes");
	/* Burst one ends at 2100, so the cap expires at 7100. */
	CHECK_UINT(rec_stamp(relay_nth(1, true)), 7100);
	CHECK(rec_stamp(ev_find(0, UDSP_EV_CID_SENT)) <= 7110);
	CHECK((int8_t)journal[ev_find(0, UDSP_EV_CID_SENT)].u.event.u.cid_sent.status
	      < 0);

	/*
	 * A delay the break cannot hold is refused at arm, not truncated at
	 * play time: LINE_CID_WINDOW_MAX_MS is the whole window, and the
	 * sequence must fit inside it with its message.
	 */
	CASE("a delay the window cannot hold is refused at arm");
	sim_init(&s, 10, false);
	CHECK_UINT((unsigned)-line_cidseq_arm(&s.cid, UDSP_CID_TYPE1, &cid,
					      &region, UDSP_LF_FWD_ACTIVE, true,
					      LINE_CID_MWI_NONE,
					      LINE_CID_WINDOW_MAX_MS), 22);
	/* The same delay is accepted with no break to hold it. */
	CHECK_UINT(line_cidseq_arm(&s.cid, UDSP_CID_TYPE1, &cid, &region,
				   UDSP_LF_FWD_ACTIVE, false, LINE_CID_MWI_NONE,
				   LINE_CID_WINDOW_MAX_MS), 0);
}

/*
 * The distinctive ring case that the single burst window above cannot show: a
 * cycle of more than one burst. cid_at_ring is an index of a ring break, so
 * uvoice sends the burst count of the first cycle. The window must open in the
 * break after the last burst of the cycle, never in a gap inside the cycle.
 */
static void ring_cid_multi_burst_test(void)
{
	struct udsp_region region;
	struct udsp_ring ring;
	struct line_sim s;
	struct udsp_cid cid;

	region_test_fill(&region);
	memset(&cid, 0, sizeof(cid));
	cid.format = UDSP_CIDF_BELLCORE_FSK;
	stub_msg_len = 24;

	/* Two burst cycle 800/400, 800/4000; cid_at_ring 2. */
	sim_init(&s, 10, false);
	sim_to(&s, false, 90);
	journal_reset();
	memset(&ring, 0, sizeof(ring));
	ring.cadence[0].on_ms = 800;
	ring.cadence[0].off_ms = 400;
	ring.cadence[1].on_ms = 800;
	ring.cadence[1].off_ms = 4000;
	ring.cadence_count = 2;
	ring.cid_present = true;
	ring.cid_at_ring = 2;
	CHECK_UINT(line_cidseq_arm(&s.cid, UDSP_CID_TYPE1, &cid, &region,
				   UDSP_LF_FWD_ACTIVE, true, LINE_CID_MWI_NONE, 0), 0);
	ring_start_at(&s, &ring);
	sim_to(&s, false, 2900);
	line_cidseq_burst_done(&s.cid);
	sim_to(&s, false, 8000);
	CASE("the window waits for the whole two burst cycle, not the first gap");
	CHECK_UINT(rec_stamp(relay_nth(1, true)), 1300);
	CHECK_UINT(rec_stamp(relay_nth(1, false)), 2100);
	CHECK_UINT(rec_stamp(cmd_find(0, LINE_CMD_LINEFEED)), 2100);
	CHECK_UINT(rec_stamp(cmd_find(0, LINE_CMD_FSK)), 2100 + 500 + LINE_RING_TAIL_MS);
	CHECK_UINT(rec_stamp(relay_nth(2, true)), 6100);
	CHECK_UINT(ev_total(UDSP_EV_CID_SENT), 1);
	CHECK_UINT(ev_total(UDSP_EV_RING_START), 1);

	/* Three burst cycle 400/200, 400/200, 800/4000; cid_at_ring 3. */
	sim_init(&s, 10, false);
	sim_to(&s, false, 90);
	journal_reset();
	memset(&ring, 0, sizeof(ring));
	ring.cadence[0].on_ms = 400;
	ring.cadence[0].off_ms = 200;
	ring.cadence[1].on_ms = 400;
	ring.cadence[1].off_ms = 200;
	ring.cadence[2].on_ms = 800;
	ring.cadence[2].off_ms = 4000;
	ring.cadence_count = 3;
	ring.cid_present = true;
	ring.cid_at_ring = 3;
	CHECK_UINT(line_cidseq_arm(&s.cid, UDSP_CID_TYPE1, &cid, &region,
				   UDSP_LF_FWD_ACTIVE, true, LINE_CID_MWI_NONE, 0), 0);
	ring_start_at(&s, &ring);
	sim_to(&s, false, 2900);
	line_cidseq_burst_done(&s.cid);
	sim_to(&s, false, 8000);
	CASE("a three burst cycle moves the window to the break after burst three");
	CHECK_UINT(rec_stamp(relay_nth(2, true)), 1300);
	CHECK_UINT(rec_stamp(relay_nth(2, false)), 2100);
	CHECK_UINT(rec_stamp(cmd_find(0, LINE_CMD_LINEFEED)), 2100);
	CHECK_UINT(rec_stamp(cmd_find(0, LINE_CMD_FSK)), 2100 + 500 + LINE_RING_TAIL_MS);
	CHECK_UINT(ev_total(UDSP_EV_CID_SENT), 1);

	/*
	 * The long silence is not last here, an unusual order no region ships.
	 * The break index is the first cycle burst count, so the window opens
	 * after the last burst of the array, in the 400 ms trailing gap, which the
	 * window then holds open. This pins the end of cycle meaning.
	 */
	sim_init(&s, 10, false);
	sim_to(&s, false, 90);
	journal_reset();
	memset(&ring, 0, sizeof(ring));
	ring.cadence[0].on_ms = 800;
	ring.cadence[0].off_ms = 4000;
	ring.cadence[1].on_ms = 800;
	ring.cadence[1].off_ms = 400;
	ring.cadence_count = 2;
	ring.cid_present = true;
	ring.cid_at_ring = 2;
	CHECK_UINT(line_cidseq_arm(&s.cid, UDSP_CID_TYPE1, &cid, &region,
				   UDSP_LF_FWD_ACTIVE, true, LINE_CID_MWI_NONE, 0), 0);
	ring_start_at(&s, &ring);
	sim_to(&s, false, 6500);
	line_cidseq_burst_done(&s.cid);
	sim_to(&s, false, 9000);
	CASE("the break index is the array end even when the long silence is not last");
	CHECK_UINT(rec_stamp(relay_nth(1, true)), 4900);
	CHECK_UINT(rec_stamp(cmd_find(0, LINE_CMD_LINEFEED)), 5700);
	CHECK_UINT(rec_stamp(cmd_find(0, LINE_CMD_FSK)), 5700 + 500 + LINE_RING_TAIL_MS);
	CHECK_UINT(ev_total(UDSP_EV_CID_SENT), 1);
}

/* --- CID sequencer cases --- */

static void cid_arm_standalone(struct line_sim *s, struct udsp_region *region,
			       enum udsp_cid_format fmt, enum udsp_cid_kind kind,
			       enum line_cid_mwi mwi)
{
	struct udsp_cid cid;

	memset(&cid, 0, sizeof(cid));
	cid.format = (uint8_t)fmt;
	sim_init(s, 10, kind == UDSP_CID_TYPE2);
	sim_to(s, kind == UDSP_CID_TYPE2, 90);
	journal_reset();
	CHECK_UINT(line_cidseq_arm(&s->cid, kind, &cid, region,
				   UDSP_LF_FWD_ACTIVE, false, mwi, 0), 0);
}

static void cid_bellcore_test(void)
{
	struct udsp_region region;
	struct udsp_fsk_mod mod;
	struct line_sim s;
	const struct line_cmd *fsk;
	int index;

	region_test_fill(&region);
	stub_msg_len = 32;
	stub_msg_calls = 0;
	cid_arm_standalone(&s, &region, UDSP_CIDF_BELLCORE_FSK, UDSP_CID_TYPE1,
			   LINE_CID_MWI_NONE);
	sim_to(&s, false, 700);
	CASE("bellcore type 1 journal is OHT then wait then the armed burst");
	index = cmd_find(0, LINE_CMD_LINEFEED);
	CHECK(index >= 0);
	CHECK_UINT(rec_stamp(index), 100);
	CHECK_UINT(journal[index >= 0 ? index : 0].u.cmd.u.linefeed.state,
		   UDSP_LF_FWD_OHT);
	index = cmd_find(0, LINE_CMD_FSK);
	CHECK(index >= 0);
	CHECK_UINT(rec_stamp(index), 600);
	CHECK_UINT(stub_msg_calls, 1);
	CHECK_UINT(stub_msg_fmt, UDSP_CIDF_BELLCORE_FSK);
	CHECK(!stub_msg_type2);
	fsk = &journal[index >= 0 ? index : 0].u.cmd;
	CHECK_UINT(fsk->u.fsk.std, UDSP_FSK_BELL202);
	CHECK_UINT(fsk->u.fsk.seizure_bits, 300);
	CHECK_UINT(fsk->u.fsk.mark_bits, 180);
	CHECK_UINT(fsk->u.fsk.len, stub_msg_len);
	CHECK_UINT(fsk->u.fsk.level_dbm0 & 0xff, LINE_CID_FSK_LEVEL_DBM0 & 0xff);

	CASE("the armed record carries the exact message bytes to the modulator");
	udsp_fsk_mod_init(&mod, (enum udsp_fsk_std)fsk->u.fsk.std,
			  fsk->u.fsk.level_dbm0, fsk->u.fsk.seizure_bits,
			  fsk->u.fsk.mark_bits, UDSP_SAMPLE_RATE);
	udsp_fsk_mod_data(&mod, fsk->u.fsk.bytes, fsk->u.fsk.len);
	CHECK_UINT(stub_fsk_init_calls, 1);
	CHECK_UINT(stub_fsk_data_len, stub_msg_len);
	CHECK(memcmp(stub_fsk_data, stub_msg_bytes, stub_msg_len) == 0);
	CHECK_UINT(stub_fsk_seizure, 300);
	CHECK_UINT(stub_fsk_mark, 180);
	CHECK_UINT(stub_fsk_std, UDSP_FSK_BELL202);
	CHECK_UINT((uint8_t)stub_fsk_level, (uint8_t)LINE_CID_FSK_LEVEL_DBM0);

	line_cidseq_burst_done(&s.cid);
	sim_to(&s, false, 800);
	CASE("bellcore completion restores the linefeed and reports status zero");
	index = ev_find(0, UDSP_EV_CID_SENT);
	CHECK(index >= 0);
	CHECK_UINT(journal[index >= 0 ? index : 0].u.event.u.cid_sent.status, 0);
	CHECK_UINT(cmd_total(LINE_CMD_LINEFEED), 2);
	CHECK_UINT(cmd_total(LINE_CMD_TONE_STOP), 0);
	CHECK(cmd_find(1, LINE_CMD_LINEFEED) < index);
	CHECK(!line_cidseq_busy(&s.cid));
}

static void cid_lr_dtas_test(void)
{
	struct udsp_region region;
	struct line_sim s;
	int polarity, tone, fsk, linefeed;

	region_test_fill(&region);
	stub_msg_len = 20;
	cid_arm_standalone(&s, &region, UDSP_CIDF_ETSI_LR_DTAS, UDSP_CID_TYPE1,
			   LINE_CID_MWI_NONE);
	sim_to(&s, false, 1200);
	CASE("lr-dtas orders reversal, DT-AS and the burst with the spec pauses");
	polarity = cmd_find(0, LINE_CMD_POLARITY);
	tone = cmd_find(0, LINE_CMD_TONE);
	linefeed = cmd_find(0, LINE_CMD_LINEFEED);
	fsk = cmd_find(0, LINE_CMD_FSK);
	CHECK(polarity >= 0 && tone >= 0 && linefeed >= 0 && fsk >= 0);
	CHECK(polarity < linefeed);
	CHECK(linefeed < tone);
	CHECK(tone < fsk);
	/* UDSP_LF_FWD_OHT would cancel the reversal before the alert. */
	CHECK_UINT(journal[linefeed >= 0 ? linefeed : 0].u.cmd.u.linefeed.state,
		   UDSP_LF_REV_OHT);
	CHECK_UINT(rec_stamp(polarity), 100);
	CHECK_UINT(journal[polarity >= 0 ? polarity : 0].u.cmd.u.polarity.state,
		   UDSP_POL_REVERSE);
	CHECK_UINT(rec_stamp(tone), 100 + LINE_LR_PAUSE_MS);
	CHECK_UINT(journal[tone >= 0 ? tone : 0].u.cmd.u.tone.duration_ms,
		   LINE_DTAS_MS);
	CHECK_UINT(journal[tone >= 0 ? tone : 0].u.cmd.u.tone.desc.frequency_hz[0],
		   LINE_CAS_LOW_HZ);
	CHECK_UINT(journal[tone >= 0 ? tone : 0].u.cmd.u.tone.desc.frequency_hz[1],
		   LINE_CAS_HIGH_HZ);
	CHECK_UINT(rec_stamp(fsk), 100 + LINE_LR_PAUSE_MS + LINE_DTAS_MS + 500);
	CHECK_UINT(journal[fsk >= 0 ? fsk : 0].u.cmd.u.fsk.std, UDSP_FSK_V23);

	line_cidseq_burst_done(&s.cid);
	sim_to(&s, false, 1400);
	CASE("lr-dtas completion returns the polarity to forward");
	polarity = cmd_find((unsigned)cmd_find(0, LINE_CMD_POLARITY) + 1,
			    LINE_CMD_POLARITY);
	CHECK(polarity >= 0);
	CHECK_UINT(journal[polarity >= 0 ? polarity : 0].u.cmd.u.polarity.state,
		   UDSP_POL_FORWARD);
}

static void cid_rpas_test(void)
{
	struct udsp_region region;
	struct line_sim s;

	region_test_fill(&region);
	stub_msg_len = 20;
	cid_arm_standalone(&s, &region, UDSP_CIDF_ETSI_RPAS, UDSP_CID_TYPE1,
			   LINE_CID_MWI_NONE);
	sim_to(&s, false, 1200);
	CASE("rpas pulses the ring relay before the message");
	CHECK_UINT(rec_stamp(relay_nth(0, true)), 100);
	CHECK_UINT(rec_stamp(relay_nth(0, false)), 100 + LINE_RPAS_MS);
	CHECK_UINT(rec_stamp(cmd_find(0, LINE_CMD_LINEFEED)), 100 + LINE_RPAS_MS);
	CHECK_UINT(rec_stamp(cmd_find(0, LINE_CMD_FSK)), 100 + LINE_RPAS_MS + 500 + LINE_RING_TAIL_MS);
	CHECK_UINT(cmd_total(LINE_CMD_TONE), 0);
	CHECK(relay_balanced());
}

static void cid_dtas_test(void)
{
	struct udsp_region region;
	struct line_sim s;

	region_test_fill(&region);
	stub_msg_len = 20;
	cid_arm_standalone(&s, &region, UDSP_CIDF_ETSI_DTAS, UDSP_CID_TYPE1,
			   LINE_CID_MWI_NONE);
	sim_to(&s, false, 1200);
	CASE("dtas alerts with the dual tone before the message");
	CHECK_UINT(rec_stamp(cmd_find(0, LINE_CMD_LINEFEED)), 100);
	CHECK_UINT(rec_stamp(cmd_find(0, LINE_CMD_TONE)), 100);
	CHECK_UINT(rec_stamp(cmd_find(0, LINE_CMD_FSK)), 100 + LINE_DTAS_MS + 500);
	CHECK_UINT(cmd_total(LINE_CMD_RING_RELAY), 0);
}

static void cid_dtmf_test(void)
{
	struct udsp_region region;
	struct line_sim s;
	int index;

	region_test_fill(&region);
	stub_dtmf_len = 12;
	stub_dtmf_calls = 0;
	cid_arm_standalone(&s, &region, UDSP_CIDF_DTMF, UDSP_CID_TYPE1,
			   LINE_CID_MWI_NONE);
	sim_to(&s, false, 900);
	CASE("dtmf format plays the digit string the builder returned");
	index = cmd_find(0, LINE_CMD_DTMF);
	CHECK(index >= 0);
	CHECK_UINT(stub_dtmf_calls, 1);
	CHECK_UINT(rec_stamp(index), 600);
	CHECK_UINT(journal[index >= 0 ? index : 0].u.cmd.u.dtmf.count, stub_dtmf_len);
	CHECK(memcmp(journal[index >= 0 ? index : 0].u.cmd.u.dtmf.digits,
		     stub_dtmf_digits, stub_dtmf_len) == 0);
	CHECK_UINT(cmd_total(LINE_CMD_FSK), 0);
}

static void cid_ntt_test(void)
{
	struct udsp_region region;
	struct line_sim s;
	int index;

	region_test_fill(&region);
	stub_msg_len = 0;
	cid_arm_standalone(&s, &region, UDSP_CIDF_NTT, UDSP_CID_TYPE1,
			   LINE_CID_MWI_NONE);
	sim_to(&s, false, 900);
	CASE("ntt reverses the polarity and reports ENOTSUP without a frame");
	CHECK_UINT(rec_stamp(cmd_find(0, LINE_CMD_POLARITY)), 100);
	CHECK_UINT(journal[cmd_find(0, LINE_CMD_LINEFEED)].u.cmd.u.linefeed.state,
		   UDSP_LF_REV_OHT);
	index = ev_find(0, UDSP_EV_CID_SENT);
	CHECK(index >= 0);
	CHECK_UINT((uint8_t)journal[index >= 0 ? index : 0].u.event.u.cid_sent.status,
		   (uint8_t)(int8_t)-95);
	CHECK_UINT(cmd_total(LINE_CMD_POLARITY), 2);
	CHECK(!line_cidseq_busy(&s.cid));
}

static void cid_type2_test(void)
{
	struct udsp_region region;
	struct line_sim s;
	int cas, ack_send, index;

	region_test_fill(&region);
	stub_msg_len = 28;
	stub_msg_calls = 0;
	cid_arm_standalone(&s, &region, UDSP_CIDF_BELLCORE_FSK, UDSP_CID_TYPE2,
			   LINE_CID_MWI_NONE);
	sim_to(&s, true, 100);
	/* The subscriber alert belongs to the uvoice core, so exactly one tone
	 * record leaves here and it is the CAS. */
	CASE("type 2 mutes then plays the CAS alone, no SAS");
	CHECK_UINT(mute_total(true), 1);
	CHECK_UINT(cmd_total(LINE_CMD_TONE), 1);
	cas = cmd_find(0, LINE_CMD_TONE);
	CHECK(cas >= 0);
	CHECK_UINT(rec_stamp(cas), 100);
	CHECK_UINT(journal[cas >= 0 ? cas : 0].u.cmd.u.tone.duration_ms, LINE_CAS_MS);
	CHECK_UINT(journal[cas >= 0 ? cas : 0].u.cmd.u.tone.desc.frequency_hz[0],
		   LINE_CAS_LOW_HZ);
	CHECK_UINT(journal[cas >= 0 ? cas : 0].u.cmd.u.tone.desc.frequency_hz[1],
		   LINE_CAS_HIGH_HZ);
	CHECK_UINT(cmd_total(LINE_CMD_LINEFEED), 0);

	sim_to(&s, true, 300);
	line_cidseq_dtmf_ack(&s.cid, s.now, 'D');
	sim_to(&s, true, 400);
	CASE("type 2 acknowledgement inside the window arms the type 2 burst");
	ack_send = cmd_find(0, LINE_CMD_FSK);
	CHECK(ack_send >= 0);
	CHECK(stub_msg_type2);
	CHECK_UINT(journal[ack_send >= 0 ? ack_send : 0].u.cmd.u.fsk.seizure_bits,
		   LINE_CID2_SEIZURE_BITS);
	CHECK_UINT(journal[ack_send >= 0 ? ack_send : 0].u.cmd.u.fsk.mark_bits,
		   LINE_CID2_MARK_BITS);

	line_cidseq_burst_done(&s.cid);
	sim_to(&s, true, 600);
	CASE("type 2 completion balances the mute bracket");
	index = ev_find(0, UDSP_EV_CID_SENT);
	CHECK(index >= 0);
	CHECK_UINT(journal[index >= 0 ? index : 0].u.event.u.cid_sent.status, 0);
	CHECK_UINT(mute_total(true), 1);
	CHECK_UINT(mute_total(false), 1);
}

static void cid_type2_timeout_test(void)
{
	struct udsp_region region;
	struct line_sim s;

	region_test_fill(&region);
	stub_msg_len = 28;
	cid_arm_standalone(&s, &region, UDSP_CIDF_BELLCORE_FSK, UDSP_CID_TYPE2,
			   LINE_CID_MWI_NONE);
	sim_to(&s, true, 2000);
	CASE("type 2 without an acknowledgement times out with a balanced mute");
	CHECK_UINT(ev_total(UDSP_EV_CID2_TIMEOUT), 1);
	CHECK_UINT(ev_total(UDSP_EV_CID_SENT), 0);
	CHECK_UINT(cmd_total(LINE_CMD_FSK), 0);
	CHECK_UINT(mute_total(true), 1);
	CHECK_UINT(mute_total(false), 1);
	/* CAS from 100 for 80 ms, then a 400 ms window that expires at 590. */
	CHECK_UINT(rec_stamp(ev_find(0, UDSP_EV_CID2_TIMEOUT)), 590);
	CHECK(!line_cidseq_busy(&s.cid));
}

static void cid_type2_late_ack_test(void)
{
	struct udsp_region region;
	struct line_sim s;

	region_test_fill(&region);
	stub_msg_len = 28;
	cid_arm_standalone(&s, &region, UDSP_CIDF_BELLCORE_FSK, UDSP_CID_TYPE2,
			   LINE_CID_MWI_NONE);
	sim_to(&s, true, 570);
	line_cidseq_dtmf_ack(&s.cid, 585, 'D');	/* one ms past the window */
	sim_to(&s, true, 2000);
	CASE("type 2 rejects an acknowledgement stamped after the window");
	CHECK_UINT(ev_total(UDSP_EV_CID2_TIMEOUT), 1);
	CHECK_UINT(cmd_total(LINE_CMD_FSK), 0);
}

static void cid_type2_reject_test(void)
{
	struct udsp_region region;
	struct line_cidseq c;
	struct udsp_cid cid;

	region_test_fill(&region);
	memset(&cid, 0, sizeof(cid));
	line_cidseq_init(&c, 0);
	CASE("type 2 refuses the DTMF formats and a visual indicator");
	cid.format = UDSP_CIDF_DTMF;
	CHECK_UINT((unsigned)-line_cidseq_arm(&c, UDSP_CID_TYPE2, &cid, &region,
					      UDSP_LF_FWD_ACTIVE, false,
					      LINE_CID_MWI_NONE, 0), 22);
	cid.format = UDSP_CIDF_DTMF_DTAS;
	CHECK_UINT((unsigned)-line_cidseq_arm(&c, UDSP_CID_TYPE2, &cid, &region,
					      UDSP_LF_FWD_ACTIVE, false,
					      LINE_CID_MWI_NONE, 0), 22);
	cid.format = UDSP_CIDF_BELLCORE_FSK;
	CHECK_UINT((unsigned)-line_cidseq_arm(&c, UDSP_CID_TYPE2, &cid, &region,
					      UDSP_LF_FWD_ACTIVE, false,
					      LINE_CID_MWI_ON, 0), 22);
	CHECK(!line_cidseq_busy(&c));
	CHECK_UINT(line_cidseq_arm(&c, UDSP_CID_TYPE2, &cid, &region,
				   UDSP_LF_FWD_ACTIVE, false, LINE_CID_MWI_NONE, 0), 0);
}

static void vmwi_case(struct udsp_region *region, enum udsp_cid_format fmt,
		      enum line_cid_mwi mwi, const char *name, unsigned std)
{
	struct line_sim s;
	const struct line_cmd *fsk;
	int index;

	stub_vmwi_len = 18;
	stub_vmwi_calls = 0;
	stub_msg_calls = 0;
	cid_arm_standalone(&s, region, fmt, UDSP_CID_TYPE1, mwi);
	sim_to(&s, false, 700);
	CASE(name);
	CHECK_UINT(stub_vmwi_calls, 1);
	CHECK_UINT(stub_msg_calls, 0);
	CHECK(stub_vmwi_on == (mwi == LINE_CID_MWI_ON));
	CHECK_UINT(stub_vmwi_fmt, fmt);
	CHECK_UINT(rec_stamp(cmd_find(0, LINE_CMD_LINEFEED)), 100);
	CHECK_UINT(journal[cmd_find(0, LINE_CMD_LINEFEED)].u.cmd.u.linefeed.state,
		   UDSP_LF_FWD_OHT);
	index = cmd_find(0, LINE_CMD_FSK);
	CHECK(index >= 0);
	CHECK_UINT(rec_stamp(index), 600);
	fsk = &journal[index >= 0 ? index : 0].u.cmd;
	CHECK_UINT(fsk->u.fsk.std, std);
	CHECK_UINT(fsk->u.fsk.len, stub_vmwi_len);
	CHECK(memcmp(fsk->u.fsk.bytes, stub_vmwi_bytes, stub_vmwi_len) == 0);

	line_cidseq_burst_done(&s.cid);
	sim_to(&s, false, 800);
	CHECK_UINT(journal[ev_find(0, UDSP_EV_CID_SENT)].u.event.u.cid_sent.status, 0);
	CHECK_UINT(cmd_total(LINE_CMD_LINEFEED), 2);
}

static void cid_vmwi_test(void)
{
	struct udsp_region region;

	region_test_fill(&region);
	vmwi_case(&region, UDSP_CIDF_BELLCORE_FSK, LINE_CID_MWI_ON,
		  "vmwi on rides the bellcore transport", UDSP_FSK_BELL202);
	vmwi_case(&region, UDSP_CIDF_BELLCORE_FSK, LINE_CID_MWI_OFF,
		  "vmwi off rides the bellcore transport", UDSP_FSK_BELL202);
	vmwi_case(&region, UDSP_CIDF_ETSI_FSK, LINE_CID_MWI_ON,
		  "vmwi on rides the etsi transport", UDSP_FSK_V23);
	vmwi_case(&region, UDSP_CIDF_ETSI_FSK, LINE_CID_MWI_OFF,
		  "vmwi off rides the etsi transport", UDSP_FSK_V23);
}

static void cid_abort_test(void)
{
	struct udsp_region region;
	struct line_sim s;
	int index, linefeed;

	region_test_fill(&region);
	stub_msg_len = 24;
	cid_arm_standalone(&s, &region, UDSP_CIDF_BELLCORE_FSK, UDSP_CID_TYPE1,
			   LINE_CID_MWI_NONE);
	sim_to(&s, false, 300);
	sim_to(&s, true, 600);
	CASE("a hook change aborts the sequence and restores the linefeed");
	index = ev_find(0, UDSP_EV_CID_SENT);
	CHECK(index >= 0);
	CHECK((int8_t)journal[index >= 0 ? index : 0].u.event.u.cid_sent.status < 0);
	CHECK_UINT(cmd_total(LINE_CMD_FSK), 0);
	CHECK_UINT(cmd_total(LINE_CMD_LINEFEED), 2);
	linefeed = cmd_find(1, LINE_CMD_LINEFEED);
	CHECK(linefeed >= 0 && linefeed < index);
	CHECK_UINT(journal[linefeed >= 0 ? linefeed : 0].u.cmd.u.linefeed.state,
		   UDSP_LF_FWD_ACTIVE);
	CHECK(!line_cidseq_busy(&s.cid));
}

static void cid_arm_guard_test(void)
{
	struct udsp_region region;
	struct udsp_cid cid;
	struct line_cidseq c;

	region_test_fill(&region);
	memset(&cid, 0, sizeof(cid));
	line_cidseq_init(&c, 0);
	cid.format = 200;
	CASE("cidseq refuses an unknown format and a second arm");
	CHECK_UINT((unsigned)-line_cidseq_arm(&c, UDSP_CID_TYPE1, &cid, &region,
					      UDSP_LF_FWD_ACTIVE, false,
					      LINE_CID_MWI_NONE, 0), 22);
	cid.format = UDSP_CIDF_BELLCORE_FSK;
	CHECK_UINT(line_cidseq_arm(&c, UDSP_CID_TYPE1, &cid, &region,
				   UDSP_LF_FWD_ACTIVE, false, LINE_CID_MWI_NONE, 0), 0);
	CHECK_UINT((unsigned)-line_cidseq_arm(&c, UDSP_CID_TYPE1, &cid, &region,
					      UDSP_LF_FWD_ACTIVE, false,
					      LINE_CID_MWI_NONE, 0), 16);
}

static void ring_guard_test(void)
{
	struct udsp_ring ring;
	struct line_ring r;
	struct line_cid_gate gate;
	struct line_out out;

	memset(&gate, 0, sizeof(gate));
	memset(&out, 0, sizeof(out));
	line_ring_init(&r, 0);
	CASE("ring refuses an empty and a zero-length cadence");
	memset(&ring, 0, sizeof(ring));
	CHECK_UINT((unsigned)-line_ring_start(&r, &ring, 0, &gate, &out), 22);
	ring.cadence_count = 1;
	CHECK_UINT((unsigned)-line_ring_start(&r, &ring, 0, &gate, &out), 22);
	ring.cadence[0].on_ms = 2000;
	ring.cadence[0].off_ms = 4000;
	CHECK_UINT(line_ring_start(&r, &ring, 0, &gate, &out), 0);
	CHECK(line_ring_active(&r));
}

static void journal_bound_test(void)
{
	struct line_out out;
	unsigned i;

	memset(&out, 0, sizeof(out));
	for (i = 0; i < LINE_OUT_MAX + 4; i++)
		line_out_cmd(&out, i, LINE_CMD_TONE_STOP);
	CASE("the journal drops records past its bound and counts them");
	CHECK_UINT(out.count, LINE_OUT_MAX);
	CHECK_UINT(out.dropped, 4);
}

int main(void)
{
	hook_boot_test();
	hook_debounce_test();
	hook_seize_test();
	hook_flash_test();
	hook_sampler_flash_test();
	hook_release_test();
	hook_pulse_test();
	hook_pulse_invert_test();
	hook_gap_test();
	hook_timing_override_test();
	hook_overlap_test();
	hook_recall_lag_test();
	ring_cadence_test();
	ring_first_cadence_test();
	ring_duration_test();
	ring_trip_test();
	ring_stop_test();
	ring_restart_test();
	ring_cid_window_test();
	ring_cid_multi_burst_test();
	cid_bellcore_test();
	cid_lr_dtas_test();
	cid_rpas_test();
	cid_dtas_test();
	cid_dtmf_test();
	cid_ntt_test();
	cid_type2_test();
	cid_type2_timeout_test();
	cid_type2_late_ack_test();
	cid_type2_reject_test();
	cid_vmwi_test();
	cid_abort_test();
	cid_arm_guard_test();
	ring_guard_test();
	journal_bound_test();
	return tu_summary("line");
}
