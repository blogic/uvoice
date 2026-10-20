// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Hook state machine. It debounces the raw hook sample the SLIC thread caches,
 * then classifies seize, release, flash and pulse digits from the measured open
 * and make intervals.
 *
 * Windows. Every window is inclusive at both ends and every interval is
 * measured from the raw edge, so the debounce never shortens a measurement
 * (line.h section 4). A pulse train is closed as a digit by whichever comes
 * first: the inter-digit make timeout, an open that outgrows a valid break, or
 * a break that follows a make longer than pulse_make_max_ms. The flash
 * window's minimum is the one test that reads an open at its latest: it adds
 * the gap of the sample that found the closing edge, capped at
 * LINE_HOOK_GAP_MAX_MS, and LINE_HOOK_FLASH_TOL_MS for the line circuit's
 * on-hook detection lag, so an open of flash_min_ms at the terminal still
 * classifies. The flash maximum, the release and the break window keep the
 * earliest-instant measure. Every close off a seized line
 * reports its two bounds, the gap of each edge and the class it took (line.h
 * struct line_hook_report), so the engine's hook trace shows what the window
 * saw.
 *
 * Window precedence. region.c enforces flash_min_ms <= flash_max_ms <
 * release_min_ms and pulse_break_max_ms < release_min_ms. It does not enforce a
 * gap between the break window and the flash window, because most European
 * profiles overlap them (line.h line_hook_pulse_enable). An open is tested
 * against the break window first, and only while pulse classification is
 * enabled, so the two features are mutually exclusive by configuration and
 * never ambiguous at run time.
 *
 * Gaps. The caller samples the level; the machine measures between the stamps
 * of those samples, so a sample the caller took late moves the edge it found
 * by that lateness, and a pair of transitions inside one sample interval
 * leaves no edge at all. A pulse train is the one thing that cannot absorb
 * either: a break or a make measured wrong closes the digit early and reports
 * a wrong digit, which dials a wrong number, and a lost pair reports one
 * pulse short. So a train a gap touched is dropped, with one UDSP_HOOK_ERROR
 * at the moment the gap is known, and stays dropped until it ends at the
 * interdigit timeout or a release: a lost digit beats a wrong one. The
 * caller's evidence is the record of line.h struct line_hook_sample: the
 * part's own change record with an unchanged level (a lost pair; an odd count
 * of transitions inside one interval is not detectable by a level plus one
 * bit), a stamp gap past LINE_HOOK_GAP_MAX_MS, or a lost record. The stage and
 * the count the rules read are the ones before this tick's edge applies: the
 * first break of a train is itself an on-hook edge, and a caller who dials
 * seconds after the seize is sampled at the sampler's idle cadence when it
 * arrives. A stall the sampler reports (LINE_HOOK_SAMPLE_STALL: a sample
 * later than its own cadence, which only the sampler can tell from the
 * cadence itself, whose stamp gap looks the same) drops the train too when
 * no train is running, and poisons the first train after a lift it found
 * late. A sampler held for seconds under a sibling's line test can come back
 * inside a break of a zero that the caller started meanwhile, and a break
 * found that late is not a first break.
 *
 * Durations stop at the last sample, and an edge is read at the earliest
 * instant it can have happened. An open or a make that is still running is
 * measured to the instant the level was last confirmed, never to now, so a
 * sampler that stalls freezes the machine instead of feeding it a level it
 * never confirmed. An edge the sampler found late is stamped late by its
 * gap, and the edge happened anywhere inside that gap, so a duration that
 * ends at the edge is measured to the stamp minus the gap, its lower bound
 * (raw_edge_ms). A release therefore needs an open the samples themselves
 * bound at release_min_ms, which makes it never false, and a flash closed
 * after a stall reads short by the stall, the less destructive misread. When
 * the sampler returns, the caller's per-tick sample carries the stall as its
 * gap, and gap_check re-bases the running open and make at that sample: the
 * stall is not confirmed time, so it neither ends the poison nor releases
 * the line. A hang-up inside a stall is therefore reported release_min_ms
 * after the sampler returns, not at once. The flash window's minimum is the
 * exception. The closing edge is found by the per-tick sampler, so its gap
 * is real uncertainty and is credited, capped at LINE_HOOK_GAP_MAX_MS: a
 * longer gap is a stall, and a flash closed after a stall reads short by
 * design. The interrupt finds the opening edge at once; its gap is the
 * sampler's idle cadence, not lateness, and it is reported but not credited.
 * What shortens every open is the line circuit. On AN7581 the loop closure
 * bit clears 33 to 34 ms after the loop opens, at every flash duration from
 * 290 to 500 ms and independent of the sampler, so a 300 ms flash at the
 * terminal reaches this machine as about 266 ms.
 * LINE_HOOK_FLASH_TOL_MS credits that lag with margin. The floor is therefore
 * flash_min_ms minus the tolerance on the measured open, 250 ms for the USA
 * window. No other class claims that band: the USA break window ends at
 * 100 ms.
 */
#include "line.h"

/* The stamp error a train can absorb: three missed ticks. A nominal 60 ms
 * break at 10 pulses per second tolerates 25 ms of error inside the 33 to
 * 100 ms window. A policy value; the sampler's gap counters are what tunes
 * it. */
#define LINE_HOOK_GAP_MAX_MS	30u

/* The on-hook detection lag of the line circuit, with margin: the loop
 * closure bit clears about 34 ms after the loop opens (the file comment,
 * "Gaps"). A policy value like the cap above. */
#define LINE_HOOK_FLASH_TOL_MS	50u

static const struct udsp_region_hook hook_timing_default = {
	.pulse_enable = false,		/* no region, no pulse dialling */
	.pulse_invert = false,		/* no region, the direct coding */
	.debounce_ms = 10,		/* one 10 ms tick */
	.flash_min_ms = 300,		/* the window uvoice requires */
	.flash_max_ms = 1300,
	.release_min_ms = 1301,
	.pulse_break_min_ms = 40,
	.pulse_break_max_ms = 100,
	.pulse_make_max_ms = 60,
	.pulse_interdigit_ms = 300,
};

#define LINE_PULSE_DIGIT_MAX	20u	/* pulse dialling carries 1..20 */

static void report_add(struct line_hook_report *rep, enum udsp_hook state,
		       uint8_t pulses)
{
	if (rep->count >= LINE_HOOK_REPORT_MAX)
		return;
	rep->ev[rep->count].state = (uint8_t)state;
	rep->ev[rep->count].pulses = pulses;
	rep->count++;
}

static void digit_flush(struct line_hook *h, struct line_hook_report *rep)
{
	if (!h->pulses)
		return;
	if (!h->poisoned)
		report_add(rep, UDSP_HOOK_PULSE, h->pulses);
	h->pulses = 0;
}

/* The rules of the file comment's "Gaps": a lost pair or a stall anywhere off
 * hook, a stamp gap or a lost record inside a train. Not a stamp gap on the
 * first break of a train, which legitimately follows the sampler's idle
 * cadence. A hit re-bases the running durations at the sample that found
 * it, whether or not it poisons anew: the stall is time the level was not
 * confirmed, and crediting it to a make would end the poison before the
 * train's remainder arrives, crediting it to an open would release a
 * handset still dialling. The raw edge moves too, else a debounce still
 * pending across the stall would restore the old base when it confirms, and
 * it carries no gap from here on: a re-based start is a confirmed instant,
 * not an edge inside a gap, and the open reports none to the trace. The
 * re-base holds on every line; the poison is a pulse rule. */
static void gap_check(struct line_hook *h, const struct line_hook_sample *sp,
		      uint32_t known_ms, struct line_hook_report *rep)
{
	bool seized = h->stage == LINE_HOOK_SEIZED || h->stage == LINE_HOOK_OPEN;
	bool train = h->pulses || h->stage == LINE_HOOK_OPEN;
	bool hit;

	if (sp->flags & (LINE_HOOK_SAMPLE_STALL | LINE_HOOK_SAMPLE_GAP))
		hit = seized;
	else
		hit = train && ((sp->flags & LINE_HOOK_SAMPLE_LOST) ||
				sp->gap_ms > LINE_HOOK_GAP_MAX_MS);
	if (!hit)
		return;
	h->raw_since_ms = known_ms;
	h->raw_gap_ms = 0;
	h->make_since_ms = known_ms;
	if (h->stage == LINE_HOOK_OPEN) {
		h->open_ms = known_ms;
		h->open_gap_ms = 0;
	}
	if (h->poisoned || !h->pulse_enable)
		return;
	h->poisoned = true;
	h->pulses = 0;
	report_add(rep, UDSP_HOOK_ERROR, 0);
}

static void pulse_count(struct line_hook *h, uint32_t make, struct line_hook_report *rep)
{
	if (h->pulses && make > h->timing.pulse_make_max_ms)
		digit_flush(h, rep);
	if (h->pulses >= LINE_PULSE_DIGIT_MAX) {
		h->pulses = 0;
		report_add(rep, UDSP_HOOK_ERROR, 0);
		return;
	}
	h->pulses++;
}

/* Engine milliseconds wrap after 49.7 days; a signed difference reads an
 * instant up to 24.8 days on either side correctly. */
static uint32_t since(uint32_t from, uint32_t until)
{
	return (int32_t)(until - from) > 0 ? until - from : 0;
}

/* The earliest instant the current raw level can have started: its stamp
 * minus the gap of the sample that found it (the file comment, "Gaps"). A
 * stamp minus a duration, modular like every stamp: since() takes two
 * stamps, and a gap read as one answers 0 for every stamp past the half
 * clock. */
static uint32_t raw_edge_ms(const struct line_hook *h)
{
	return h->raw_since_ms - h->raw_gap_ms;
}

/* The sampler error the closing edge carries into the flash window's minimum
 * (the file comment, "Windows"). */
static uint32_t gap_credit(uint16_t gap_ms)
{
	return gap_ms > LINE_HOOK_GAP_MAX_MS ? LINE_HOOK_GAP_MAX_MS : gap_ms;
}

static void open_measured(struct line_hook_report *rep, const struct line_hook *h,
			  uint32_t open_lo, uint32_t open_hi, uint8_t cls)
{
	rep->measured = true;
	rep->open_class = cls;
	rep->open_gap_ms = h->open_gap_ms;
	rep->close_gap_ms = h->raw_gap_ms;
	rep->open_lo_ms = open_lo;
	rep->open_hi_ms = open_hi;
}

static void close_apply(struct line_hook *h, struct line_hook_report *rep)
{
	uint32_t open_lo = since(h->open_ms, raw_edge_ms(h));
	uint32_t open_hi = open_lo + gap_credit(h->raw_gap_ms) +
			   LINE_HOOK_FLASH_TOL_MS;
	uint32_t make = since(h->make_since_ms, h->open_ms);
	uint8_t stage = h->stage;
	uint8_t cls = LINE_HOOK_OPEN_NONE;

	h->stage = LINE_HOOK_SEIZED;
	h->make_since_ms = h->raw_since_ms;
	if (stage == LINE_HOOK_IDLE) {
		/* A lift the sampler found late (the STALL mark came with the
		 * sample that found this level) may have a train behind it
		 * inside the stall, so its first train is dropped; decided here
		 * and not in gap_check, so a lift the debounce rejects poisons
		 * nothing. */
		h->pulses = 0;
		h->poisoned = h->pulse_enable && h->raw_stall;
		report_add(rep, UDSP_HOOK_SEIZE, 0);
		if (h->poisoned)
			report_add(rep, UDSP_HOOK_ERROR, 0);
		return;
	}
	if (h->pulse_enable && open_lo >= h->timing.pulse_break_min_ms &&
	    open_lo <= h->timing.pulse_break_max_ms) {
		pulse_count(h, make, rep);
		cls = LINE_HOOK_OPEN_PULSE;
	} else if (open_hi >= h->timing.flash_min_ms &&
		   open_lo <= h->timing.flash_max_ms) {
		report_add(rep, UDSP_HOOK_FLASH, 0);
		cls = LINE_HOOK_OPEN_FLASH;
	}
	/* Anything else is shorter than a break or falls in the gap between the
	 * break and flash windows: no classification exists for it, and any
	 * pending digit left with the open that outgrew a break. */
	open_measured(rep, h, open_lo, open_hi, cls);
}

static void open_apply(struct line_hook *h)
{
	h->stage = LINE_HOOK_OPEN;
	h->open_ms = h->raw_since_ms;
	h->open_gap_ms = h->raw_gap_ms;
}

static void boot_apply(struct line_hook *h, struct line_hook_report *rep)
{
	h->stable = h->raw;
	h->started = true;
	h->stage = h->stable ? LINE_HOOK_SEIZED : LINE_HOOK_IDLE;
	h->open_ms = h->raw_since_ms;
	h->open_gap_ms = 0;
	h->make_since_ms = h->raw_since_ms;
	rep->boot = true;
	report_add(rep, h->stable ? UDSP_HOOK_SEIZE : UDSP_HOOK_RELEASE, 0);
}

/* known_ms is the last instant the raw level was confirmed and gap_ms the
 * sampler's gap on this sample (line.h struct line_hook_sample); the debounce
 * counts confirmed time alone. */
static void debounce_run(struct line_hook *h, uint32_t now_ms, uint32_t known_ms,
			 uint16_t gap_ms, uint8_t flags, bool raw,
			 struct line_hook_report *rep)
{
	if (!h->primed || raw != h->raw) {
		h->primed = true;
		h->raw = raw;
		h->raw_since_ms = now_ms;
		h->raw_gap_ms = gap_ms;
		h->raw_stall = (flags & LINE_HOOK_SAMPLE_STALL) != 0;
	}
	if (h->started && raw == h->stable)
		return;
	if ((int32_t)(known_ms - h->raw_since_ms) < (int32_t)h->timing.debounce_ms)
		return;
	if (!h->started) {
		boot_apply(h, rep);
		return;
	}
	h->stable = raw;
	if (!raw) {
		rep->onhook_edge = true;
		open_apply(h);
		return;
	}
	rep->offhook_edge = true;
	close_apply(h, rep);
}

/*
 * While the raw line has already flipped back but the debounce has not
 * confirmed it yet, the interval is frozen at the raw edge. Letting it run
 * would classify an open of exactly flash_max_ms as a release one debounce
 * window later.
 */
static void open_run(struct line_hook *h, uint32_t known_ms,
		     struct line_hook_report *rep)
{
	uint32_t open = since(h->open_ms, h->raw ? raw_edge_ms(h) : known_ms);

	if (open > h->timing.pulse_break_max_ms)
		digit_flush(h, rep);
	if (open < h->timing.release_min_ms)
		return;
	h->stage = LINE_HOOK_IDLE;
	h->pulses = 0;
	h->poisoned = false;
	report_add(rep, UDSP_HOOK_RELEASE, 0);
}

/* The interdigit timeout ends a poisoned train as it ends a counted one, so
 * the next train classifies again. */
static void make_run(struct line_hook *h, uint32_t known_ms,
		     struct line_hook_report *rep)
{
	uint32_t make = since(h->make_since_ms,
			      h->raw ? known_ms : raw_edge_ms(h));

	if (make < h->timing.pulse_interdigit_ms)
		return;
	digit_flush(h, rep);
	h->poisoned = false;
}

void line_hook_init(struct line_hook *h, const struct udsp_region_hook *timing)
{
	memset(h, 0, sizeof(*h));
	h->timing = timing ? *timing : hook_timing_default;
	h->stage = LINE_HOOK_INIT;
	h->pulse_enable = h->timing.pulse_enable;
}

void line_hook_pulse_enable(struct line_hook *h, bool on)
{
	h->pulse_enable = on;
	if (!on) {
		h->pulses = 0;
		h->poisoned = false;
	}
}

void line_hook_timing_set(struct line_hook *h, uint32_t flash_min_ms,
			  uint32_t flash_max_ms, uint32_t release_min_ms)
{
	h->timing.flash_min_ms = (uint16_t)(flash_min_ms > UINT16_MAX ?
					    UINT16_MAX : flash_min_ms);
	h->timing.flash_max_ms = (uint16_t)(flash_max_ms > UINT16_MAX ?
					    UINT16_MAX : flash_max_ms);
	h->timing.release_min_ms = (uint16_t)(release_min_ms > UINT16_MAX ?
					      UINT16_MAX : release_min_ms);
}

struct line_hook_report line_hook_tick(struct line_hook *h, uint32_t now_ms,
				       bool raw_offhook,
				       const struct line_hook_sample *sample)
{
	struct line_hook_report rep;
	uint32_t known_ms = now_ms;
	uint16_t gap_ms = 0;
	uint8_t flags = 0;

	memset(&rep, 0, sizeof(rep));
	if (sample) {
		if ((int32_t)(sample->known_ms - now_ms) < 0)
			known_ms = sample->known_ms;
		gap_ms = sample->gap_ms;
		flags = sample->flags;
		gap_check(h, sample, known_ms, &rep);
	}
	debounce_run(h, now_ms, known_ms, gap_ms, flags, raw_offhook, &rep);
	if (h->stage == LINE_HOOK_OPEN)
		open_run(h, known_ms, &rep);
	else if (h->stage == LINE_HOOK_SEIZED && (h->pulses || h->poisoned))
		make_run(h, known_ms, &rep);
	rep.offhook = h->stable;
	rep.pulse_invert = h->timing.pulse_invert;
	return rep;
}

void line_hook_emit(const struct line_hook_report *rep, unsigned line,
		    uint32_t now_ms, struct line_out *out)
{
	unsigned i;

	for (i = 0; i < rep->count; i++) {
		struct udsp_event *ev;

		ev = line_out_event(out, now_ms, line, UDSP_EV_HOOK);
		ev->u.hook.state = rep->ev[i].state;
		ev->u.hook.pulses = rep->ev[i].pulses;
		if (rep->ev[i].state == UDSP_HOOK_PULSE)
			ev->u.hook.pulse_invert = rep->pulse_invert;
	}
}
