// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Region parser suite. The golden case parses the shipped USA.conf and
 * asserts the numeric model field by field. A silent change to the country
 * data or to the parser therefore fails here. The error cases edit that same
 * text in memory, so every negative case is one edit away from a file that is
 * known to parse.
 *
 * The suite finds the data directory by a probe of the paths where a cmake
 * build tree can put the binary. UDSP_REGION_DIR in the environment, or
 * argv[1], overrides the probe.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../line/line.h"
#include "util.h"

static char region_dir[512];
static char base_text[UDSP_REGION_FILE_MAX + 1];
static size_t base_len;
static char work[UDSP_REGION_FILE_MAX + 1024];

static const char *const region_dir_candidates[] = {
	"../data/regions", "../../data/regions", "../../../data/regions",
	"data/regions", "udsp/data/regions", "../udsp/data/regions",
};

static bool region_dir_find(const char *hint)
{
	char probe[600];
	unsigned i;

	if (hint) {
		snprintf(probe, sizeof(probe), "%s/USA.conf", hint);
		if (!access(probe, R_OK)) {
			snprintf(region_dir, sizeof(region_dir), "%s", hint);
			return true;
		}
	}
	for (i = 0; i < sizeof(region_dir_candidates) /
			sizeof(region_dir_candidates[0]); i++) {
		snprintf(probe, sizeof(probe), "%s/USA.conf",
			 region_dir_candidates[i]);
		if (access(probe, R_OK))
			continue;
		snprintf(region_dir, sizeof(region_dir), "%s",
			 region_dir_candidates[i]);
		return true;
	}
	return false;
}

static bool base_text_load(const char *iso3)
{
	char path[600];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s.conf", region_dir, iso3);
	f = fopen(path, "rb");
	if (!f)
		return false;
	base_len = fread(base_text, 1, sizeof(base_text) - 1, f);
	fclose(f);
	base_text[base_len] = '\0';
	return base_len > 0;
}

static void work_reset(void)
{
	memcpy(work, base_text, base_len);
	work[base_len] = '\0';
}

static bool work_replace(const char *find, const char *repl)
{
	size_t tail, flen = strlen(find), rlen = strlen(repl);
	char *at = strstr(work, find);

	if (!at)
		return false;
	tail = strlen(at + flen);
	if ((size_t)(at - work) + rlen + tail + 1 > sizeof(work))
		return false;
	memmove(at + rlen, at + flen, tail + 1);
	memcpy(at, repl, rlen);
	return true;
}

static bool work_cut(const char *from, const char *to)
{
	char *start = strstr(work, from);
	char *end;

	if (!start)
		return false;
	end = strstr(start + strlen(from), to);
	if (!end)
		return false;
	memmove(start, end, strlen(end) + 1);
	return true;
}

static bool work_append(const char *s)
{
	size_t len = strlen(work);

	if (len + strlen(s) + 1 > sizeof(work))
		return false;
	memcpy(work + len, s, strlen(s) + 1);
	return true;
}

static int work_parse(struct udsp_region *out, struct line_region_error *err)
{
	return line_region_parse(work, strlen(work), out, err);
}

static void golden_test(void)
{
	const struct udsp_region_tone *t;
	struct line_region_error err;
	struct udsp_region r;

	memset(&r, 0, sizeof(r));
	work_reset();
	CASE("USA.conf parses");
	CHECK_UINT(work_parse(&r, &err), 0);
	if (err.text[0])
		printf("    # parse error line %u: %s\n", err.line, err.text);

	CASE("USA region identity");
	CHECK(!strcmp(r.iso3, "USA"));
	CHECK(!strcmp(r.describe, "United States"));

	CASE("USA dial tone is 350 plus 440 Hz continuous");
	t = &r.tone[UDSP_TONE_DIAL];
	CHECK(t->present && !t->segmented);
	CHECK_UINT(t->desc.tone, UDSP_TONE_DIAL);
	CHECK_UINT(t->desc.frequency_count, 2);
	CHECK_UINT(t->desc.frequency_hz[0], 350);
	CHECK_UINT(t->desc.frequency_hz[1], 440);
	CHECK_UINT((uint8_t)t->desc.level_dbm0[0], (uint8_t)-13);
	CHECK_UINT((uint8_t)t->desc.level_dbm0[1], (uint8_t)-13);
	CHECK_UINT(t->desc.cadence_count, 0);
	CHECK_UINT(t->repeat_count, 0);
	CHECK(t->desc.repeat);

	CASE("USA stutter and second dial tone interrupt at 100 ms then steady");
	CHECK_UINT(r.tone[UDSP_TONE_STUTTER].desc.cadence_count, 1);
	CHECK_UINT(r.tone[UDSP_TONE_STUTTER].desc.cadence[0].make_ms, 100);
	CHECK_UINT(r.tone[UDSP_TONE_STUTTER].desc.cadence[0].break_ms, 100);
	CHECK_UINT(r.tone[UDSP_TONE_STUTTER].repeat_count, 10);
	CHECK(r.tone[UDSP_TONE_STUTTER].then_steady);
	CHECK_UINT(r.tone[UDSP_TONE_SECOND_DIAL].desc.cadence[0].make_ms, 100);
	CHECK_UINT(r.tone[UDSP_TONE_SECOND_DIAL].repeat_count, 3);
	CHECK(r.tone[UDSP_TONE_SECOND_DIAL].then_steady);
	CHECK(!r.tone[UDSP_TONE_DIAL].then_steady);

	CASE("USA ringback is 440 plus 480 Hz at 2 s on 4 s off");
	t = &r.tone[UDSP_TONE_RINGBACK];
	CHECK_UINT(t->desc.frequency_hz[0], 440);
	CHECK_UINT(t->desc.frequency_hz[1], 480);
	CHECK_UINT((uint8_t)t->desc.level_dbm0[0], (uint8_t)-19);
	CHECK_UINT(t->desc.cadence[0].make_ms, 2000);
	CHECK_UINT(t->desc.cadence[0].break_ms, 4000);

	CASE("USA busy and reorder share 480 plus 620 Hz");
	CHECK_UINT(r.tone[UDSP_TONE_BUSY].desc.frequency_hz[0], 480);
	CHECK_UINT(r.tone[UDSP_TONE_BUSY].desc.frequency_hz[1], 620);
	CHECK_UINT(r.tone[UDSP_TONE_BUSY].desc.cadence[0].make_ms, 500);
	CHECK_UINT(r.tone[UDSP_TONE_REORDER].desc.cadence[0].make_ms, 250);
	CHECK_UINT(r.tone[UDSP_TONE_REORDER].desc.cadence[0].break_ms, 250);
	CHECK_UINT((uint8_t)r.tone[UDSP_TONE_REORDER].desc.level_dbm0[1],
		   (uint8_t)-24);

	CASE("USA special information tone is three segments then silence");
	t = &r.tone[UDSP_TONE_SIT];
	CHECK(t->segmented);
	CHECK_UINT(t->desc.frequency_count, 0);
	CHECK_UINT(t->segment_count, 3);
	CHECK_UINT(t->segment[0].freq_count, 1);
	CHECK_UINT(t->segment[0].freq_hz[0], 914);
	CHECK_UINT(t->segment[0].duration_ms, 276);
	CHECK_UINT(t->segment[1].freq_hz[0], 1371);
	CHECK_UINT(t->segment[2].freq_hz[0], 1777);
	CHECK_UINT(t->segment[2].duration_ms, 380);
	CHECK_UINT((uint8_t)t->segment[2].level_dbm0, (uint8_t)-24);
	CHECK_UINT(t->silence_ms, 1000);

	CASE("USA call waiting is two 440 Hz presentations ten seconds apart");
	t = &r.tone[UDSP_TONE_CALL_WAITING];
	CHECK(t->present);
	CHECK_UINT(t->desc.frequency_count, 1);
	CHECK_UINT(t->desc.frequency_hz[0], 440);
	CHECK_UINT(t->desc.cadence[0].make_ms, 300);
	CHECK_UINT(t->desc.cadence[0].break_ms, 10000);
	CHECK_UINT(t->repeat_count, 2);
	CHECK(!t->then_steady);

	CASE("USA confirmation tone repeats three times");
	CHECK_UINT(r.tone[UDSP_TONE_CONFIRM].repeat_count, 3);
	CHECK(r.tone[UDSP_TONE_CONFIRM].desc.repeat);

	CASE("USA short stutter dial tone is three bursts then steady");
	t = &r.tone[UDSP_TONE_SHORT_STUTTER_DIAL];
	CHECK(t->present);
	CHECK_UINT(t->desc.frequency_count, 2);
	CHECK_UINT(t->desc.frequency_hz[0], 350);
	CHECK_UINT(t->desc.frequency_hz[1], 440);
	CHECK_UINT(t->desc.cadence[0].make_ms, 100);
	CHECK_UINT(t->desc.cadence[0].break_ms, 100);
	CHECK_UINT(t->repeat_count, 3);
	CHECK(t->then_steady);
	CHECK(t->desc.repeat);

	CASE("USA howler carries the four ROH frequencies");
	t = &r.tone[UDSP_TONE_HOWLER];
	CHECK(!t->segmented);
	CHECK_UINT(t->desc.frequency_count, 4);
	CHECK_UINT(t->desc.frequency_hz[0], 1400);
	CHECK_UINT(t->desc.frequency_hz[1], 2060);
	CHECK_UINT(t->desc.frequency_hz[2], 2450);
	CHECK_UINT(t->desc.frequency_hz[3], 2600);
	CHECK_UINT((uint8_t)t->desc.level_dbm0[3], (uint8_t)-12);
	CHECK_UINT(t->desc.cadence[0].make_ms, 100);

	CASE("USA ring templates");
	CHECK_UINT(r.ring_count, 8);
	CHECK(!strcmp(r.ring[0].name, "r1"));
	CHECK_UINT(r.ring[0].cadence_count, 1);
	CHECK_UINT(r.ring[0].cadence[0].on_ms, 2000);
	CHECK_UINT(r.ring[0].cadence[0].off_ms, 4000);
	CHECK_UINT(r.ring[0].first_count, 0);
	CHECK_UINT(r.ring[2].cadence_count, 2);
	CHECK_UINT(r.ring[2].cadence[0].on_ms, 800);
	CHECK_UINT(r.ring[2].cadence[0].off_ms, 400);
	CHECK_UINT(r.ring[2].cadence[1].on_ms, 800);
	CHECK_UINT(r.ring[2].cadence[1].off_ms, 4000);
	CHECK_UINT(r.ring[7].cadence_count, 1);
	CHECK_UINT(r.ring[7].cadence[0].on_ms, 2000);
	CHECK_UINT(r.ring[7].cadence[0].off_ms, 4000);

	CASE("USA cid profile");
	CHECK_UINT(r.cid.format, UDSP_CIDF_BELLCORE_FSK);
	/* USA omits mwi_format, so it folds to format. */
	CHECK_UINT(r.cid.mwi_format, UDSP_CIDF_BELLCORE_FSK);
	CHECK_UINT(r.cid.wait_ms, 500);
	CHECK_UINT(r.cid.ack_timeout_ms, 300);
	CHECK_UINT(r.cid.seizure_bits, 300);
	CHECK_UINT(r.cid.mark_bits, 180);
	/* USA omits the alert timings, so the parser fills the defaults. */
	CHECK_UINT(r.cid.dtas_ms, LINE_DTAS_MS);
	CHECK_UINT(r.cid.rpas_ms, LINE_RPAS_MS);
	CHECK_UINT(r.cid.lr_pause_ms, LINE_LR_PAUSE_MS);

	CASE("USA hook timing");
	CHECK(r.hook.pulse_enable);
	CHECK_UINT(r.hook.debounce_ms, 10);
	CHECK_UINT(r.hook.flash_min_ms, 300);
	CHECK_UINT(r.hook.flash_max_ms, 1300);
	CHECK_UINT(r.hook.release_min_ms, 1301);
	CHECK_UINT(r.hook.pulse_break_min_ms, 33);
	CHECK_UINT(r.hook.pulse_break_max_ms, 100);
	CHECK_UINT(r.hook.pulse_make_max_ms, 85);
	CHECK_UINT(r.hook.pulse_interdigit_ms, 300);
	CHECK_UINT(r.flash_service, UDSP_FLASH_SERVICE_DROP);
}

static void shipped_files_test(void)
{
	static const char *const iso3[] = {
		"USA", "GBR", "ETS", "AUS", "NZL", "AUT", "SRB",
	};
	struct line_region_error err;
	struct udsp_region r;
	unsigned i;
	char path[600];

	for (i = 0; i < sizeof(iso3) / sizeof(iso3[0]); i++) {
		char name[64];

		snprintf(name, sizeof(name), "%s.conf parses and names itself",
			 iso3[i]);
		snprintf(path, sizeof(path), "%s/%s.conf", region_dir, iso3[i]);
		memset(&r, 0, sizeof(r));
		CASE(name);
		CHECK_UINT(line_region_load(path, &r, &err), 0);
		if (err.text[0])
			printf("    # line %u: %s\n", err.line, err.text);
		CHECK(!strcmp(r.iso3, iso3[i]));
		CHECK_UINT(r.ring_count, 8);
		CHECK(r.tone[UDSP_TONE_CALL_WAITING].present);
		/* Every shipped call waiting self-terminates; the CW offer
		 * bound depends on a finite presentation count. */
		CHECK(r.tone[UDSP_TONE_CALL_WAITING].repeat_count > 0);
		CHECK(r.cw_count <= UDSP_CW_VARIANT_MAX);
		CHECK(r.cid.mwi_format < UDSP_CIDF_COUNT);
		CHECK(r.hook.flash_max_ms < r.hook.release_min_ms);
		CHECK(r.flash_service < UDSP_FLASH_SERVICE_COUNT);
	}
}

static void ring_expect(const struct udsp_region_ring *ring,
			const struct udsp_cadence *want, unsigned n)
{
	unsigned i;

	CHECK_UINT(ring->cadence_count, n);
	for (i = 0; i < n && i < ring->cadence_count; i++) {
		CHECK_UINT(ring->cadence[i].on_ms, want[i].on_ms);
		CHECK_UINT(ring->cadence[i].off_ms, want[i].off_ms);
	}
}

static void gbr_golden_test(void)
{
	static const struct udsp_cadence r2[] = { { 400, 200 }, { 400, 2000 } };
	static const struct udsp_cadence r3[] = { { 800, 400 }, { 800, 4000 } };
	struct line_region_error err;
	struct udsp_region r;
	char path[600];

	snprintf(path, sizeof(path), "%s/GBR.conf", region_dir);
	memset(&r, 0, sizeof(r));
	CASE("GBR states its own alert timings and leaves recall the live feature");
	CHECK_UINT(line_region_load(path, &r, &err), 0);
	CHECK_UINT(r.cid.format, UDSP_CIDF_ETSI_LR_DTAS);
	CHECK_UINT(r.cid.mwi_format, UDSP_CIDF_ETSI_LR_DTAS);
	CHECK_UINT(r.cid.lr_pause_ms, 250);
	CHECK_UINT(r.cid.dtas_ms, 100);
	CHECK(!r.hook.pulse_enable);
	CHECK_UINT(r.hook.flash_min_ms, 35);
	CHECK_UINT(r.hook.flash_max_ms, 145);
	CHECK_UINT(r.hook.release_min_ms, 590);

	/* Bellcore-dr<n> selects r<n+1>, so r2 is dr1, the normal ring. */
	CASE("GBR r2 is the normal double ring and r3 the dr2 shape");
	ring_expect(&r.ring[1], r2, 2);
	ring_expect(&r.ring[2], r3, 2);
}

static void aus_golden_test(void)
{
	static const struct udsp_cadence r2[] = { { 400, 200 }, { 400, 2000 } };
	static const struct udsp_cadence r3[] = {
		{ 400, 400 }, { 200, 200 }, { 400, 1400 },
	};
	struct line_region_error err;
	struct udsp_region r;
	char path[600];

	snprintf(path, sizeof(path), "%s/AUS.conf", region_dir);
	memset(&r, 0, sizeof(r));
	CASE("AUS r2 is the normal double ring and r3 the first seed pattern");
	CHECK_UINT(line_region_load(path, &r, &err), 0);
	ring_expect(&r.ring[1], r2, 2);
	ring_expect(&r.ring[2], r3, 3);
}

static void srb_golden_test(void)
{
	const struct udsp_region_tone *t;
	struct line_region_error err;
	struct udsp_region r;
	char path[600];

	snprintf(path, sizeof(path), "%s/SRB.conf", region_dir);
	memset(&r, 0, sizeof(r));
	CASE("SRB splits the caller identity and VMWI transports");
	CHECK_UINT(line_region_load(path, &r, &err), 0);
	CHECK_UINT(r.cid.format, UDSP_CIDF_DTMF_DTAS);
	CHECK_UINT(r.cid.mwi_format, UDSP_CIDF_ETSI_DTAS);

	CASE("SRB call waiting is seven single-burst presentations");
	t = &r.tone[UDSP_TONE_CALL_WAITING];
	CHECK_UINT(t->desc.frequency_hz[0], 425);
	CHECK_UINT(t->desc.cadence_count, 1);
	CHECK_UINT(t->desc.cadence[0].make_ms, 300);
	CHECK_UINT(t->desc.cadence[0].break_ms, 9700);
	CHECK_UINT(t->repeat_count, 7);

	CASE("SRB hook window carries the provisioned values");
	CHECK_UINT(r.hook.flash_min_ms, 35);
	CHECK_UINT(r.hook.flash_max_ms, 145);
	CHECK_UINT(r.hook.release_min_ms, 490);
}

/* One expected segment of a parsed tone. count 0 is an off segment. */
struct segment_want {
	uint16_t hz[UDSP_TONE_FREQ_MAX];
	uint8_t count;
	int8_t level;
	uint16_t ms;
};

/* A shipped endless segmented tone against its expected segments. */
static void segments_expect(const char *iso3, enum udsp_tone tone,
			    const struct segment_want *want, unsigned n,
			    unsigned silence_ms)
{
	struct line_region_error err;
	struct udsp_region r;
	const struct udsp_region_tone *t = &r.tone[tone];
	char path[600];
	unsigned i;

	snprintf(path, sizeof(path), "%s/%s.conf", region_dir, iso3);
	memset(&r, 0, sizeof(r));
	CHECK_UINT(line_region_load(path, &r, &err), 0);
	CHECK(t->segmented);
	CHECK_UINT(t->segment_count, n);
	CHECK_UINT(t->silence_ms, silence_ms);
	CHECK_UINT(t->repeat_count, 0);
	CHECK(!t->then_steady);
	for (i = 0; i < n && i < t->segment_count; i++) {
		const struct udsp_region_segment *seg = &t->segment[i];

		CHECK_UINT(seg->freq_count, want[i].count);
		CHECK(!memcmp(seg->freq_hz, want[i].hz, sizeof(seg->freq_hz)));
		CHECK_UINT((uint8_t)seg->level_dbm0, (uint8_t)want[i].level);
		CHECK_UINT(seg->duration_ms, want[i].ms);
	}
}

static void segment_golden_test(void)
{
	static const struct segment_want gbr[] = {
		{ { 350, 440 }, 2, -13, 750 },
		{ { 440 }, 1, -13, 750 },
	};
	static const struct segment_want aut[] = {
		{ { 420 }, 1, -10, 1000 },
		{ { 380, 420 }, 2, -13, 1000 },
	};
	static const struct segment_want aus[] = {
		{ { 425 }, 1, -13, 375 },
		{ { 0 }, 0, 0, 375 },
		{ { 425 }, 1, -23, 375 },
	};

	CASE("GBR stutter plays the dial pair, then 440 Hz alone");
	segments_expect("GBR", UDSP_TONE_STUTTER, gbr, 2, 0);

	CASE("AUT stutter alternates 420 Hz with the 380 plus 420 Hz pair");
	segments_expect("AUT", UDSP_TONE_STUTTER, aut, 2, 0);

	CASE("AUS congestion tone drops every second burst by 10 dB");
	segments_expect("AUS", UDSP_TONE_REORDER, aus, 3, 375);
}

/* A shipped endless one-frequency tone against its expected cadence. n 0 is
 * a continuous tone. */
static void simple_expect(const char *iso3, enum udsp_tone tone, uint16_t hz,
			  int8_t level, const struct udsp_cadence *want,
			  unsigned n)
{
	struct line_region_error err;
	struct udsp_region r;
	const struct udsp_region_tone *t = &r.tone[tone];
	char path[600];
	unsigned i;

	snprintf(path, sizeof(path), "%s/%s.conf", region_dir, iso3);
	memset(&r, 0, sizeof(r));
	CHECK_UINT(line_region_load(path, &r, &err), 0);
	CHECK(t->present && !t->segmented);
	CHECK_UINT(t->desc.tone, tone);
	CHECK_UINT(t->desc.frequency_count, 1);
	CHECK_UINT(t->desc.frequency_hz[0], hz);
	CHECK_UINT((uint8_t)t->desc.level_dbm0[0], (uint8_t)level);
	CHECK_UINT(t->desc.cadence_count, n);
	CHECK_UINT(t->repeat_count, 0);
	CHECK(t->desc.repeat);
	for (i = 0; i < n && i < t->desc.cadence_count; i++) {
		CHECK_UINT(t->desc.cadence[i].make_ms, want[i].on_ms);
		CHECK_UINT(t->desc.cadence[i].break_ms, want[i].off_ms);
	}
}

/*
 * The national number unobtainable tone of each shipped file. USA plays its
 * reorder tone. The other files take the tone from the E.180 annex of ITU
 * Operational Bulletin No. 781 (1 February 2003); ETS and SRB have no such
 * row, so they play their special information tone.
 */
static void unobtainable_golden_test(void)
{
	static const struct udsp_cadence aus[] = { { 2500, 500 } };
	static const struct udsp_cadence nzl[] = {
		{ 750, 100 }, { 750, 100 }, { 750, 100 }, { 750, 400 },
	};
	static const struct segment_want sit[] = {
		{ { 950 }, 1, -10, 330 },
		{ { 1400 }, 1, -10, 330 },
		{ { 1800 }, 1, -10, 330 },
	};
	static const struct segment_want srb[] = {
		{ { 950 }, 1, -10, 333 },
		{ { 1400 }, 1, -10, 333 },
		{ { 1800 }, 1, -10, 333 },
	};
	const enum udsp_tone tone = UDSP_TONE_NUMBER_UNOBTAINABLE;
	const struct udsp_region_tone *nu, *reorder;
	struct line_region_error err;
	struct udsp_region r;
	char path[600];
	unsigned i;

	CASE("USA number unobtainable is the reorder tone");
	snprintf(path, sizeof(path), "%s/USA.conf", region_dir);
	memset(&r, 0, sizeof(r));
	CHECK_UINT(line_region_load(path, &r, &err), 0);
	nu = &r.tone[tone];
	reorder = &r.tone[UDSP_TONE_REORDER];
	CHECK(nu->present && !nu->segmented);
	CHECK_UINT(nu->desc.tone, tone);
	CHECK_UINT(nu->desc.frequency_count, 2);
	CHECK_UINT(nu->desc.frequency_hz[0], 480);
	CHECK_UINT(nu->desc.frequency_hz[1], 620);
	CHECK_UINT((uint8_t)nu->desc.level_dbm0[0], (uint8_t)-24);
	CHECK_UINT((uint8_t)nu->desc.level_dbm0[1], (uint8_t)-24);
	CHECK_UINT(nu->desc.cadence_count, 1);
	CHECK_UINT(nu->desc.cadence[0].make_ms, 250);
	CHECK_UINT(nu->desc.cadence[0].break_ms, 250);
	CHECK_UINT(nu->repeat_count, 0);
	CHECK(nu->desc.repeat);
	CHECK_UINT(nu->desc.frequency_count, reorder->desc.frequency_count);
	CHECK_UINT(nu->desc.cadence_count, reorder->desc.cadence_count);
	for (i = 0; i < 2; i++) {
		CHECK_UINT(nu->desc.frequency_hz[i],
			   reorder->desc.frequency_hz[i]);
		CHECK_UINT((uint8_t)nu->desc.level_dbm0[i],
			   (uint8_t)reorder->desc.level_dbm0[i]);
	}
	CHECK_UINT(nu->desc.cadence[0].make_ms,
		   reorder->desc.cadence[0].make_ms);
	CHECK_UINT(nu->desc.cadence[0].break_ms,
		   reorder->desc.cadence[0].break_ms);

	CASE("GBR number unobtainable is a continuous 400 Hz");
	simple_expect("GBR", tone, 400, -10, NULL, 0);

	CASE("AUS number unobtainable is 425 Hz at 2.5 s on 0.5 s off");
	simple_expect("AUS", tone, 425, -13, aus, 1);

	CASE("NZL number unobtainable is 3 x 750/100 ms, then 750/400 ms");
	simple_expect("NZL", tone, 400, -9, nzl, 4);

	CASE("AUT number unobtainable is the 950, 1400 and 1800 Hz triple");
	segments_expect("AUT", tone, sit, 3, 1000);

	CASE("ETS number unobtainable plays its special information tone");
	segments_expect("ETS", tone, sit, 3, 1000);

	CASE("SRB number unobtainable plays its special information tone");
	segments_expect("SRB", tone, srb, 3, 1000);
}

/* The tones that follow the OB 781 rows in place of an older value. */
static void ob781_golden_test(void)
{
	static const struct udsp_cadence srb_dial[] = {
		{ 700, 800 }, { 200, 300 },
	};
	static const struct udsp_cadence srb_special[] = { { 500, 50 } };
	static const struct udsp_cadence nzl_nu[] = {
		{ 750, 100 }, { 750, 100 }, { 750, 100 }, { 750, 400 },
	};
	const struct udsp_region_tone *t;
	struct line_region_error err;
	struct udsp_region r;
	char path[600];

	CASE("SRB dial tones are the interrupted 425 Hz of the OB 781 row");
	simple_expect("SRB", UDSP_TONE_DIAL, 425, -10, srb_dial, 2);
	simple_expect("SRB", UDSP_TONE_SECOND_DIAL, 425, -10, srb_dial, 2);
	simple_expect("SRB", UDSP_TONE_SHORT_STUTTER_DIAL, 425, -10, srb_dial,
		      2);

	CASE("SRB stutter is the ETSI special dial tone");
	simple_expect("SRB", UDSP_TONE_STUTTER, 425, -10, srb_special, 1);

	CASE("NZL sit plays the number unobtainable tone");
	simple_expect("NZL", UDSP_TONE_SIT, 400, -9, nzl_nu, 4);

	snprintf(path, sizeof(path), "%s/NZL.conf", region_dir);
	memset(&r, 0, sizeof(r));
	CASE("NZL ships call waiting tones II and III as c1 and c2");
	CHECK_UINT(line_region_load(path, &r, &err), 0);
	CHECK_UINT(r.cw_count, 2);
	CHECK_UINT(line_region_cw_lead_ms(&r.cw[0]), 300);
	CHECK_UINT(line_region_cw_lead_ms(&r.cw[1]), 500);
	CHECK_UINT(r.cw[0].repeat_count, 3);
	CHECK_UINT(r.cw[1].repeat_count, 3);
	/* Tone I keeps a break after its last burst, so the CAS waits for one
	 * burst and not for the whole OB 781 cycle. */
	CHECK_UINT(line_region_cw_lead_ms(&r.tone[UDSP_TONE_CALL_WAITING]),
		   200);

	CASE("NZL short stutter dial is three bursts, then steady");
	t = &r.tone[UDSP_TONE_SHORT_STUTTER_DIAL];
	CHECK_UINT(t->desc.cadence_count, 1);
	CHECK_UINT(t->desc.cadence[0].make_ms, 100);
	CHECK_UINT(t->desc.cadence[0].break_ms, 100);
	CHECK_UINT(t->repeat_count, 3);
	CHECK(t->then_steady);
}

static void level_broadcast_test(void)
{
	struct line_region_error err;
	struct udsp_region r;

	memset(&r, 0, sizeof(r));
	work_reset();
	CASE("one level value applies to every frequency");
	CHECK(work_replace("level = -13 -13", "level = -11"));
	CHECK_UINT(work_parse(&r, &err), 0);
	CHECK_UINT(r.tone[UDSP_TONE_DIAL].desc.frequency_count, 2);
	CHECK_UINT((uint8_t)r.tone[UDSP_TONE_DIAL].desc.level_dbm0[0], (uint8_t)-11);
	CHECK_UINT((uint8_t)r.tone[UDSP_TONE_DIAL].desc.level_dbm0[1], (uint8_t)-11);
}

static void error_case(const char *name, const char *expect)
{
	struct line_region_error err;
	struct udsp_region r;

	memset(&r, 0, sizeof(r));
	memset(&err, 0, sizeof(err));
	CASE(name);
	CHECK_UINT((unsigned)-work_parse(&r, &err), 22);
	CHECK(strstr(err.text, expect) != NULL);
	if (!strstr(err.text, expect))
		printf("    # got line %u: %s\n", err.line, err.text);
}

static void unknown_key_test(void)
{
	work_reset();
	CHECK(work_append("bogus_key = 1\n"));
	error_case("an unknown key is an error", "unknown key in [hook]");
}

static void unknown_section_test(void)
{
	work_reset();
	CHECK(work_append("[bogus]\nx = 1\n"));
	error_case("an unknown section is an error", "unknown section");
}

static void removed_key_test(void)
{
	work_reset();
	CHECK(work_replace("wait_ms = 500", "at_ring = 1\nwait_ms = 500"));
	error_case("an at_ring key is an unknown [cid] key",
		   "unknown key in [cid]: at_ring");

	work_reset();
	CHECK(work_append("\n[defaults]\nec_tail_ms = 48\n"));
	error_case("a [defaults] section is an unknown section",
		   "unknown section: defaults");
}

static void unknown_tone_test(void)
{
	struct line_region_error err;
	struct udsp_region r;

	work_reset();
	CHECK(work_replace("[tone dial]", "[tone dail]"));
	memset(&r, 0, sizeof(r));
	memset(&err, 0, sizeof(err));
	CASE("an unknown tone name is an error naming the line");
	CHECK_UINT((unsigned)-work_parse(&r, &err), 22);
	CHECK(strstr(err.text, "unknown tone name") != NULL);
	CHECK(err.line > 0);
}

static void five_freq_test(void)
{
	work_reset();
	CHECK(work_replace("freq = 350 440", "freq = 300 350 400 440 480"));
	error_case("a fifth frequency is an error", "bad freq list");
}

static void mixed_form_test(void)
{
	work_reset();
	CHECK(work_replace("cadence = on\nrepeat = forever",
			   "cadence = on\nsegment = 950 -24 330\nrepeat = forever"));
	error_case("mixing the simple and segment forms is an error",
		   "mixes simple and segment form");
}

_Static_assert(UDSP_TONE_FREQ_MAX == 4u,
	       "the frequency group cases write four and five frequencies");

static void segment_form_test(void)
{
	struct line_region_error err;
	struct udsp_region r;
	const struct udsp_region_tone *t = &r.tone[UDSP_TONE_SIT];

	memset(&r, 0, sizeof(r));
	work_reset();
	CHECK(work_replace("segment = 914 -24 276",
			   "segment = 914+1371+1777+2000 -24 276"));
	CASE("a segment plays up to four frequencies at one level");
	CHECK_UINT(work_parse(&r, &err), 0);
	CHECK_UINT(t->segment[0].freq_count, UDSP_TONE_FREQ_MAX);
	CHECK_UINT(t->segment[0].freq_hz[0], 914);
	CHECK_UINT(t->segment[0].freq_hz[1], 1371);
	CHECK_UINT(t->segment[0].freq_hz[2], 1777);
	CHECK_UINT(t->segment[0].freq_hz[3], 2000);
	CHECK_UINT((uint8_t)t->segment[0].level_dbm0, (uint8_t)-24);
	CHECK_UINT(t->segment[0].duration_ms, 276);

	memset(&r, 0, sizeof(r));
	work_reset();
	CHECK(work_replace("segment = 1371 -24 276", "segment = off 276"));
	CASE("an off segment is silent and keeps its duration");
	CHECK_UINT(work_parse(&r, &err), 0);
	CHECK_UINT(t->segment_count, 3);
	CHECK_UINT(t->segment[1].freq_count, 0);
	CHECK_UINT(t->segment[1].duration_ms, 276);
	CHECK_UINT(t->segment[2].freq_hz[0], 1777);

	memset(&r, 0, sizeof(r));
	work_reset();
	CHECK(work_replace("segment = 1371 -24 276\nsegment = 1777 -24 380\n"
			   "silence = 1000\nrepeat = forever",
			   "segment = off 276\nsegment = 1777 -24 380\n"
			   "silence = 0\nrepeat = 2\nthen = on"));
	CASE("then = on after a silent middle segment and no silence parses");
	CHECK_UINT(work_parse(&r, &err), 0);
	CHECK_UINT(t->silence_ms, 0);
	CHECK_UINT(t->repeat_count, 2);
	CHECK(t->then_steady);
}

/* The first USA sit segment replaced by line, for an error case. */
static void sit_segment_set(const char *line)
{
	work_reset();
	CHECK(work_replace("segment = 914 -24 276", line));
}

static void segment_error_test(void)
{
	sit_segment_set("segment = 914+1371+1777+2000+2400 -24 276");
	error_case("a fifth frequency in a segment is an error",
		   "bad segment frequency list");

	sit_segment_set("segment = 914+3401 -24 276");
	error_case("a segment frequency above 3400 Hz is an error",
		   "bad segment frequency list");

	sit_segment_set("segment = 99 -24 276");
	error_case("a segment frequency below 100 Hz is an error",
		   "bad segment frequency list");

	sit_segment_set("segment = 914++1371 -24 276");
	error_case("an empty member between two plus signs is an error",
		   "bad segment frequency list");

	sit_segment_set("segment = 914+ -24 276");
	error_case("a plus sign that ends the frequency group is an error",
		   "bad segment frequency list");

	sit_segment_set("segment = +914 -24 276");
	error_case("a plus sign that starts the frequency group is an error",
		   "bad segment frequency list");

	sit_segment_set("segment = off -24 276");
	error_case("an off segment with a level is an error",
		   "off segment needs a duration only");

	sit_segment_set("segment = off");
	error_case("an off segment without a duration is an error",
		   "off segment needs a duration only");

	work_reset();
	CHECK(work_replace("segment = 1777 -24 380\nsilence = 1000\n"
			   "repeat = forever",
			   "segment = off 380\nsilence = 1000\nrepeat = 2\n"
			   "then = on"));
	error_case("then = on after a silent last segment is an error",
		   "then needs a last segment with a frequency");
}

static void missing_tone_test(void)
{
	work_reset();
	CHECK(work_cut("[tone call_waiting]", "[tone confirm]"));
	error_case("a missing call_waiting tone is an error naming it",
		   "missing tone section: call_waiting");

	work_reset();
	CHECK(work_cut("[tone number_unobtainable]", "[tone call_waiting]"));
	error_case("a missing number_unobtainable tone is an error naming it",
		   "missing tone section: number_unobtainable");
}

static void oversize_line_test(void)
{
	char long_line[UDSP_REGION_LINE_MAX + 8];

	memset(long_line, 'x', sizeof(long_line) - 2);
	long_line[sizeof(long_line) - 2] = '\n';
	long_line[sizeof(long_line) - 1] = '\0';
	work_reset();
	CHECK(work_append(long_line));
	error_case("a line above two hundred characters is an error",
		   "line above 200 characters");
}

static void oversize_file_test(void)
{
	struct line_region_error err;
	struct udsp_region r;
	static char big[UDSP_REGION_FILE_MAX + 2];

	memset(big, '\n', sizeof(big));
	memset(&r, 0, sizeof(r));
	memset(&err, 0, sizeof(err));
	CASE("a file above sixteen kibibytes is refused");
	CHECK_UINT((unsigned)-line_region_parse(big, sizeof(big), &r, &err), 27);
	CHECK(strstr(err.text, "file above 16 KiB") != NULL);
}

static void hook_order_test(void)
{
	work_reset();
	CHECK(work_replace("release_min_ms = 1301", "release_min_ms = 1200"));
	error_case("a release below the flash maximum is an error",
		   "release_min_ms not above flash_max_ms");

	work_reset();
	CHECK(work_replace("pulse_interdigit_ms = 300", "pulse_interdigit_ms = 80"));
	error_case("an inter-digit gap below the make maximum is an error",
		   "pulse_make_max_ms not below");
}

static void then_key_test(void)
{
	struct line_region_error err;
	struct udsp_region r;

	work_reset();
	CHECK(work_replace("repeat = 10\nthen = on", "repeat = forever\nthen = on"));
	error_case("then without a finite repeat count is an error",
		   "then needs a finite repeat count");

	work_reset();
	CHECK(work_replace("then = on", "then = maybe"));
	error_case("a non boolean then value is an error", "value must be on or off");

	work_reset();
	CHECK(work_replace("pulse_enable = on", "pulse_enable = 1"));
	error_case("a non boolean pulse_enable value is an error",
		   "value must be on or off");

	work_reset();
	CHECK(work_replace("pulse_enable = on",
			   "pulse_enable = on\npulse_invert = 1"));
	error_case("a non boolean pulse_invert value is an error",
		   "value must be on or off: pulse_invert");

	memset(&r, 0, sizeof(r));
	work_reset();
	CHECK(work_replace("[cid]\nformat", "[cid]\ndtas_ms = 120\nformat"));
	CASE("an explicit dtas_ms overrides the default");
	CHECK_UINT(work_parse(&r, &err), 0);
	CHECK_UINT(r.cid.dtas_ms, 120);

	work_reset();
	CHECK(work_replace("mark_bits = 180", "mark_bits = 180\nbogus_cid = 1"));
	error_case("an unknown key in [cid] is an error", "unknown key in [cid]");
}

static void mwi_format_test(void)
{
	struct line_region_error err;
	struct udsp_region r;

	memset(&r, 0, sizeof(r));
	work_reset();
	CHECK(work_replace("[cid]\nformat", "[cid]\nmwi_format = etsi-dtas\nformat"));
	CASE("an explicit mwi_format overrides the format fold");
	CHECK_UINT(work_parse(&r, &err), 0);
	CHECK_UINT(r.cid.format, UDSP_CIDF_BELLCORE_FSK);
	CHECK_UINT(r.cid.mwi_format, UDSP_CIDF_ETSI_DTAS);

	work_reset();
	CHECK(work_replace("[cid]\nformat", "[cid]\nmwi_format = bogus\nformat"));
	error_case("an unknown mwi_format value is an error", "unknown cid format");
}

static void flash_service_test(void)
{
	struct line_region_error err;
	struct udsp_region r;

	memset(&r, 0, sizeof(r));
	work_reset();
	CHECK(work_replace("flash_service = drop", "flash_service = off"));
	CASE("flash_service off parses");
	CHECK_UINT(work_parse(&r, &err), 0);
	CHECK_UINT(r.flash_service, UDSP_FLASH_SERVICE_OFF);
	CHECK(!strcmp(line_region_flash_service_name(r.flash_service), "off"));

	work_reset();
	CHECK(work_replace("flash_service = drop", "flash_service = split"));
	CASE("flash_service split parses");
	CHECK_UINT(work_parse(&r, &err), 0);
	CHECK_UINT(r.flash_service, UDSP_FLASH_SERVICE_SPLIT);

	work_reset();
	CHECK(work_replace("flash_service = drop", "flash_service = 2"));
	error_case("a flash_service value outside the three words is an error",
		   "unknown flash service");

	work_reset();
	CHECK(work_replace("flash_service = drop\n", ""));
	error_case("a missing flash_service names the key",
		   "missing [hook] key: flash_service");

	work_reset();
	CHECK(work_replace("debounce_ms = 10\n", ""));
	error_case("a missing hook window names its key",
		   "missing [hook] key: debounce_ms");
}

static void no_partial_apply_test(void)
{
	struct line_region_error err;
	struct udsp_region canary;
	unsigned char reference[sizeof(struct udsp_region)];

	memset(reference, 0xa5, sizeof(reference));

	memset(&canary, 0xa5, sizeof(canary));
	memset(&err, 0, sizeof(err));
	work_reset();
	CHECK(work_append("bogus_trailing_key = 1\n"));
	CASE("an error on the last line applies nothing to the caller's region");
	CHECK_UINT((unsigned)-work_parse(&canary, &err), 22);
	CHECK(err.line > 0);
	CHECK(memcmp(&canary, reference, sizeof(reference)) == 0);

	/* The second half of the same rule: an error the whole-file checks find
	 * after the last line must not leave the earlier lines applied either. */
	memset(&canary, 0xa5, sizeof(canary));
	memset(&err, 0, sizeof(err));
	work_reset();
	CHECK(work_cut("[tone call_waiting]", "[tone confirm]"));
	CASE("an error found after the last line applies nothing either");
	CHECK_UINT((unsigned)-work_parse(&canary, &err), 22);
	CHECK_UINT(err.line, 0);
	CHECK(memcmp(&canary, reference, sizeof(reference)) == 0);
}

static void duplicate_section_test(void)
{
	work_reset();
	CHECK(work_append("[cid]\nformat = etsi-fsk\n"));
	error_case("a duplicate section is an error", "duplicate section");

	work_reset();
	CHECK(work_append("[tone dial]\nfreq = 400\n"));
	error_case("a duplicate tone section is an error", "duplicate tone section");
}

static void ring_gap_test(void)
{
	work_reset();
	CHECK(work_cut("[ring r2]", "[ring r3]"));
	error_case("a gap in the ring templates is an error",
		   "ring templates must run from r1");

	work_reset();
	CHECK(work_cut("[ring r1]", "[cid]"));
	error_case("a file with no ring template is an error", "no ring template");
}

static void width_boundary_test(void)
{
	struct line_region_error err;
	struct udsp_region r;

	memset(&r, 0, sizeof(r));
	memset(&err, 0, sizeof(err));
	work_reset();
	CHECK(work_replace("wait_ms = 500", "wait_ms = 5000"));
	CASE("a cid wait stores whole at its maximum");
	CHECK(work_parse(&r, &err) == 0);
	CHECK_UINT(r.cid.wait_ms, 5000);

	work_reset();
	CHECK(work_replace("wait_ms = 500", "wait_ms = 5001"));
	error_case("a wait_ms above the range names the key", "bad wait_ms");
}

static void cw_variant_test(void)
{
	struct line_region_error err;
	struct udsp_region r;
	const struct udsp_region_tone *t;

	memset(&r, 0, sizeof(r));
	work_reset();
	CASE("USA defines four distinctive call waiting variants");
	CHECK_UINT(work_parse(&r, &err), 0);
	CHECK_UINT(r.cw_count, 4);

	t = &r.cw[0];
	CHECK(t->present && !t->segmented);
	CHECK_UINT(t->desc.tone, UDSP_TONE_CALL_WAITING);
	CHECK_UINT(t->desc.frequency_count, 1);
	CHECK_UINT(t->desc.frequency_hz[0], 440);
	CHECK_UINT((uint8_t)t->desc.level_dbm0[0], (uint8_t)-13);
	CHECK_UINT(t->desc.cadence_count, 1);
	CHECK_UINT(t->desc.cadence[0].make_ms, 300);
	CHECK_UINT(t->desc.cadence[0].break_ms, 10000);
	CHECK_UINT(t->repeat_count, 2);
	/* c1 repeats the plain pattern; c2 to c4 draw the short and long forms
	 * and their presentation lengths widen. */
	CHECK_UINT(line_region_cw_lead_ms(&r.cw[0]), 300);
	CHECK_UINT(r.cw[1].desc.cadence_count, 2);
	CHECK_UINT(line_region_cw_lead_ms(&r.cw[1]), 300);
	CHECK_UINT(r.cw[2].desc.cadence_count, 3);
	CHECK_UINT(line_region_cw_lead_ms(&r.cw[2]), 500);
	CHECK_UINT(r.cw[3].desc.cadence_count, 3);
	CHECK_UINT(r.cw[3].desc.cadence[1].make_ms, 300);
	CHECK_UINT(line_region_cw_lead_ms(&r.cw[3]), 700);

	memset(&r, 0, sizeof(r));
	work_reset();
	CHECK(work_cut("[cw c1]", "[cid]"));
	CASE("a region without cw variants loads with cw_count zero");
	CHECK_UINT(work_parse(&r, &err), 0);
	CHECK_UINT(r.cw_count, 0);
}

static void tone_total_test(void)
{
	static const struct {
		const char *iso3;
		uint32_t total_ms;
	} cw[] = {
		{ "USA", 20600 }, { "GBR", 8200 }, { "ETS", 19200 },
		{ "AUS", 50000 }, { "NZL", 12800 }, { "AUT", 19200 },
		{ "SRB", 70000 },
	};
	struct line_region_error err;
	struct udsp_region r;
	char path[600];
	unsigned i;

	CASE("every shipped call waiting tone keeps its bound");
	for (i = 0; i < sizeof(cw) / sizeof(cw[0]); i++) {
		const struct udsp_region_tone *t =
			&r.tone[UDSP_TONE_CALL_WAITING];

		snprintf(path, sizeof(path), "%s/%s.conf", region_dir,
			 cw[i].iso3);
		memset(&r, 0, sizeof(r));
		CHECK_UINT(line_region_load(path, &r, &err), 0);
		CHECK_UINT(line_region_tone_total_ms(t), cw[i].total_ms);
	}

	memset(&r, 0, sizeof(r));
	work_reset();
	CASE("the USA call waiting rows keep their bounds");
	CHECK_UINT(work_parse(&r, &err), 0);
	CHECK_UINT(line_region_tone_total_ms(&r.cw[0]), 20600);
	CHECK_UINT(line_region_tone_total_ms(&r.cw[1]), 20600);
	CHECK_UINT(line_region_tone_total_ms(&r.cw[2]), 21000);
	CHECK_UINT(line_region_tone_total_ms(&r.cw[3]), 21400);

	CASE("an absent, an endless and a then = on tone bound nothing");
	CHECK_UINT(line_region_tone_total_ms(&r.tone[UDSP_TONE_NONE]), 0);
	CHECK_UINT(line_region_tone_total_ms(&r.tone[UDSP_TONE_DIAL]), 0);
	CHECK_UINT(line_region_tone_total_ms(&r.tone[UDSP_TONE_SIT]), 0);
	CHECK_UINT(line_region_tone_total_ms(&r.tone[UDSP_TONE_STUTTER]), 0);

	memset(&r, 0, sizeof(r));
	work_reset();
	CHECK(work_replace("silence = 1000\nrepeat = forever",
			   "silence = 1000\nrepeat = 2"));
	CASE("a finite segmented tone is one cycle times its count");
	CHECK_UINT(work_parse(&r, &err), 0);
	/* (276 + 276 + 380 + 1000) x 2 */
	CHECK_UINT(line_region_tone_total_ms(&r.tone[UDSP_TONE_SIT]), 3864);

	memset(&r, 0, sizeof(r));
	work_reset();
	CHECK(work_replace("silence = 1000\nrepeat = forever",
			   "silence = 1000\nrepeat = 2\nthen = on"));
	CASE("a segmented then = on tone bounds nothing");
	CHECK_UINT(work_parse(&r, &err), 0);
	CHECK(r.tone[UDSP_TONE_SIT].then_steady);
	CHECK_UINT(line_region_tone_total_ms(&r.tone[UDSP_TONE_SIT]), 0);
}

/* The region view carries the confirm length, so a caller can start the next
 * tone when the confirm tone ends. AUS and AUT repeat the tone until the
 * caller stops it, which bounds nothing. */
static void confirm_total_test(void)
{
	static const struct {
		const char *iso3;
		uint32_t total_ms;
	} want[] = {
		{ "USA", 600 }, { "GBR", 20000 }, { "ETS", 600 },
		{ "AUS", 0 }, { "NZL", 3000 }, { "AUT", 0 },
		{ "SRB", 600 },
	};
	struct line_region_error err;
	struct udsp_region r;
	char path[600];
	unsigned i;

	CASE("every shipped confirm tone keeps its region length");
	for (i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		const struct udsp_region_tone *t = &r.tone[UDSP_TONE_CONFIRM];

		snprintf(path, sizeof(path), "%s/%s.conf", region_dir,
			 want[i].iso3);
		memset(&r, 0, sizeof(r));
		CHECK_UINT(line_region_load(path, &r, &err), 0);
		CHECK_UINT(line_region_tone_total_ms(t), want[i].total_ms);
	}
}

/* New Zealand codes a dialed N as 10 - N pulses. Every other shipped file
 * omits the key, so it reads the direct coding. */
static void pulse_invert_test(void)
{
	static const struct {
		const char *iso3;
		bool invert;
	} want[] = {
		{ "USA", false }, { "GBR", false }, { "ETS", false },
		{ "AUS", false }, { "NZL", true }, { "AUT", false },
		{ "SRB", false },
	};
	struct line_region_error err;
	struct udsp_region r;
	char path[600];
	unsigned i;

	CASE("only NZL inverts the decadic digit coding");
	for (i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s.conf", region_dir,
			 want[i].iso3);
		memset(&r, 0, sizeof(r));
		CHECK_UINT(line_region_load(path, &r, &err), 0);
		CHECK_UINT(r.hook.pulse_invert, want[i].invert);
	}

	memset(&r, 0, sizeof(r));
	work_reset();
	CHECK(work_replace("pulse_enable = on",
			   "pulse_enable = on\npulse_invert = on"));
	CASE("pulse_invert on parses beside pulse_enable");
	CHECK_UINT(work_parse(&r, &err), 0);
	CHECK(r.hook.pulse_invert);
	CHECK(r.hook.pulse_enable);
}

static void cw_error_test(void)
{
	work_reset();
	CHECK(work_cut("[cw c1]", "[cw c2]"));
	error_case("a gap in the cw variants is an error",
		   "cw variants must run from c1 with no gap");

	work_reset();
	CHECK(work_append("\n[cw c5]\nfreq = 440\nlevel = -13\n"
			  "cadence = 300 10000\nrepeat = 2\n"));
	error_case("a cw section beyond c4 is an error",
		   "call waiting variant must be c1..c4");

	work_reset();
	CHECK(work_append("\n[cw c2]\nfreq = 440\nlevel = -13\n"
			  "cadence = 300 10000\nrepeat = 2\n"));
	error_case("a duplicate cw section is an error", "duplicate cw section");

	work_reset();
	CHECK(work_replace("cadence = 100 100 100 10000\nrepeat = 2",
			   "cadence = 100 100 100 10000\nrepeat = 2\nthen = on"));
	error_case("a cw then form is an error", "cw variant must be a simple tone");

	work_reset();
	CHECK(work_replace("cadence = 100 100 100 10000\nrepeat = 2",
			   "segment = 440 -13 300\nsilence = 10000\nrepeat = 2"));
	error_case("a cw segment form is an error",
		   "cw variant must be a simple tone");

	work_reset();
	CHECK(work_replace("cadence = 100 100 100 10000", "cadence = on"));
	error_case("a continuous cw variant is an error",
		   "cw variant needs a cadence, not on");

	work_reset();
	CHECK(work_replace("cadence = 100 100 100 10000", "cadence = 100 10000"));
	error_case("a cw presentation below 250 ms is an error",
		   "cw presentation outside 250..1000 ms");

	work_reset();
	CHECK(work_replace("cadence = 100 100 100 10000\nrepeat = 2",
			   "cadence = 100 100 100 10000\nrepeat = forever"));
	error_case("a cw variant that repeats forever is an error",
		   "cw variant needs a finite repeat count");

	work_reset();
	CHECK(work_replace("cadence = 100 100 100 10000", "cadence = 300 500"));
	error_case("a cw trailing break short of the type 2 exchange is an error",
		   "cw trailing break is shorter than the type 2 exchange");
}

int main(int argc, char **argv)
{
	if (!region_dir_find(argc > 1 ? argv[1] : getenv("UDSP_REGION_DIR"))) {
		printf("Bail out! data/regions not found from %s\n",
		       argv[0] ? argv[0] : "?");
		return 1;
	}
	if (!base_text_load("USA")) {
		printf("Bail out! cannot read %s/USA.conf\n", region_dir);
		return 1;
	}
	golden_test();
	shipped_files_test();
	gbr_golden_test();
	aus_golden_test();
	srb_golden_test();
	segment_golden_test();
	unobtainable_golden_test();
	ob781_golden_test();
	level_broadcast_test();
	unknown_key_test();
	unknown_section_test();
	removed_key_test();
	unknown_tone_test();
	five_freq_test();
	mixed_form_test();
	segment_form_test();
	segment_error_test();
	missing_tone_test();
	oversize_line_test();
	oversize_file_test();
	hook_order_test();
	duplicate_section_test();
	ring_gap_test();
	cw_variant_test();
	tone_total_test();
	confirm_total_test();
	pulse_invert_test();
	cw_error_test();
	then_key_test();
	mwi_format_test();
	flash_service_test();
	no_partial_apply_test();
	width_boundary_test();
	return tu_summary("region");
}
