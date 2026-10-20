// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Region file parser. The grammar is fixed. It carries the optional keys [tone]
 * then, [cid] mwi_format, dtas_ms, rpas_ms and lr_pause_ms, and [hook]
 * pulse_enable and pulse_invert. The library never reads two keys: the
 * required [hook] flash_service word exists for the region read-back, and only
 * udsp-cli reads the optional [ring] first cadence. The hook machine reads
 * [hook] pulse_invert to tag each pulse train it reports. This file is the
 * grammar's only reader.
 *
 * Parse all, then swap. Every value goes into a scratch struct udsp_region and
 * the caller's region is overwritten only after the whole file validates, so a
 * typo on the last line never leaves a half-applied country behind. The parser
 * is strict by design: an unknown section, an unknown key, an unknown tone
 * name, a missing tone, a missing required key and an out-of-range count are
 * all errors. A silently defaulted flash window or a silently dropped tone is a
 * field defect nobody sees until a phone misbehaves.
 *
 * Bounds. File 16 KiB, line 200 characters, and every count is checked against
 * the dsp.h and udsp.h limits (4 frequencies, 8 tone cadence pairs, 5 ring
 * cadence pairs, 8 ring templates). A '#' starts a comment anywhere on a line.
 * An error found after the last line reports line 0.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "line.h"

#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))

enum parse_section {
	SEC_NONE = 0, SEC_REGION, SEC_TONE, SEC_RING, SEC_CID, SEC_HOOK,
	SEC_CW, SEC_MAX,
};

/*
 * The [tone] and [cw] sections share one key grammar. A target points the
 * shared setters and the shared check at the tone record, its completeness key
 * word and its level scratch, so [tone name] fills region.tone[i] and [cw cN]
 * fills region.cw[N-1] through the same code.
 */
struct tone_target {
	struct udsp_region_tone *t;
	uint8_t *keys;
	int8_t *level;			/* a row of UDSP_TONE_FREQ_MAX */
	uint8_t *level_count;
};

#define TONE_KEY_FREQ		(1u << 0)
#define TONE_KEY_LEVEL		(1u << 1)
#define TONE_KEY_CADENCE	(1u << 2)
#define TONE_KEY_SEGMENT	(1u << 3)
#define TONE_KEY_SILENCE	(1u << 4)
#define TONE_KEY_REPEAT		(1u << 5)
#define TONE_KEY_THEN		(1u << 6)
#define TONE_KEYS_SIMPLE	(TONE_KEY_FREQ | TONE_KEY_LEVEL | TONE_KEY_CADENCE)
#define TONE_KEYS_SEGMENT	(TONE_KEY_SEGMENT | TONE_KEY_SILENCE)

#define RING_KEY_CADENCE	(1u << 0)
#define RING_KEY_FIRST		(1u << 1)
#define RING_KEY_SEEN		(1u << 7)

#define CID_KEYS_ALL		0x1fu
#define HOOK_KEY_FLASH_SERVICE	(1u << 8)
#define HOOK_KEYS_ALL		0x1ffu

#define REGION_LEVEL_MIN	(-60)
#define REGION_LEVEL_MAX	0
#define REGION_FREQ_MIN		100
#define REGION_FREQ_MAX		3400
#define REGION_MS_MAX		60000

struct parse_ctx {
	struct udsp_region region;
	struct line_region_error *err;
	unsigned line;
	int section;
	int ring_index;
	struct tone_target tone_tgt;	/* the open [tone] or [cw] section */
	bool seen[SEC_MAX];
	bool cid_mwi_seen;
	uint32_t cid_keys;
	uint32_t hook_keys;
	uint32_t region_keys;
	uint8_t tone_keys[UDSP_REGION_TONE_MAX];
	uint8_t ring_keys[UDSP_REGION_RING_MAX];
	int8_t tone_level[UDSP_REGION_TONE_MAX][UDSP_TONE_FREQ_MAX];
	uint8_t tone_level_count[UDSP_REGION_TONE_MAX];
	uint8_t cw_keys[UDSP_CW_VARIANT_MAX];
	int8_t cw_level[UDSP_CW_VARIANT_MAX][UDSP_TONE_FREQ_MAX];
	uint8_t cw_level_count[UDSP_CW_VARIANT_MAX];
};

static struct tone_target tone_target_of(struct parse_ctx *p, unsigned i)
{
	return (struct tone_target){
		.t = &p->region.tone[i],
		.keys = &p->tone_keys[i],
		.level = p->tone_level[i],
		.level_count = &p->tone_level_count[i],
	};
}

static struct tone_target cw_target_of(struct parse_ctx *p, unsigned i)
{
	return (struct tone_target){
		.t = &p->region.cw[i],
		.keys = &p->cw_keys[i],
		.level = p->cw_level[i],
		.level_count = &p->cw_level_count[i],
	};
}

struct section_def {
	const char *name;
	int id;
};

struct key_def {
	const char *name;
	int bit;		/* completeness bit, or -1 for an optional key */
	long min;
	long max;
	size_t offset;		/* of a uint16_t member */
};

static const char *const tone_names[UDSP_REGION_TONE_MAX] = {
	NULL, "dial", "stutter", "second_dial", "ringback", "busy",
	"reorder", "sit", "call_waiting", "confirm", "howler", "short_stutter_dial",
	"number_unobtainable",
};

static const char *const cid_format_names[] = {
	"bellcore-fsk", "etsi-fsk", "etsi-rpas", "etsi-dtas",
	"dtmf", "ntt", "etsi-lr-dtas", "dtmf-dtas",
};
_Static_assert(ARRAY_SIZE(cid_format_names) == (size_t)UDSP_CIDF_COUNT,
	       "cid_format_names must match enum udsp_cid_format");

static const char *const flash_service_names[] = { "off", "split", "drop" };
_Static_assert(ARRAY_SIZE(flash_service_names) == (size_t)UDSP_FLASH_SERVICE_COUNT,
	       "flash_service_names must match enum udsp_flash_service");

static const struct section_def section_defs[] = {
	{ "region", SEC_REGION }, { "cid", SEC_CID }, { "hook", SEC_HOOK },
};

static const struct key_def hook_key_defs[] = {
	{ "debounce_ms", 0, 1, 200,
	  offsetof(struct udsp_region_hook, debounce_ms) },
	{ "flash_min_ms", 1, 1, 60000,
	  offsetof(struct udsp_region_hook, flash_min_ms) },
	{ "flash_max_ms", 2, 1, 60000,
	  offsetof(struct udsp_region_hook, flash_max_ms) },
	{ "release_min_ms", 3, 1, 60000,
	  offsetof(struct udsp_region_hook, release_min_ms) },
	{ "pulse_break_min_ms", 4, 1, 1000,
	  offsetof(struct udsp_region_hook, pulse_break_min_ms) },
	{ "pulse_break_max_ms", 5, 1, 1000,
	  offsetof(struct udsp_region_hook, pulse_break_max_ms) },
	{ "pulse_make_max_ms", 6, 1, 1000,
	  offsetof(struct udsp_region_hook, pulse_make_max_ms) },
	{ "pulse_interdigit_ms", 7, 1, 5000,
	  offsetof(struct udsp_region_hook, pulse_interdigit_ms) },
};

/* format is bit 0 but takes a name, and the optional mwi_format takes the
 * same names, so cid_key_apply keeps both out of the table. The three alert
 * timings carry bit -1: they are optional and set no completeness bit, so an
 * absent key leaves a zero the sequencer defaults. */
static const struct key_def cid_key_defs[] = {
	{ "wait_ms", 1, 0, 5000,
	  offsetof(struct udsp_region_cid, wait_ms) },
	{ "ack_timeout_ms", 2, 50, 2000,
	  offsetof(struct udsp_region_cid, ack_timeout_ms) },
	{ "seizure_bits", 3, 0, 2000,
	  offsetof(struct udsp_region_cid, seizure_bits) },
	{ "mark_bits", 4, 0, 2000,
	  offsetof(struct udsp_region_cid, mark_bits) },
	{ "dtas_ms", -1, 20, 1000,
	  offsetof(struct udsp_region_cid, dtas_ms) },
	{ "rpas_ms", -1, 20, 1000,
	  offsetof(struct udsp_region_cid, rpas_ms) },
	{ "lr_pause_ms", -1, 20, 2000,
	  offsetof(struct udsp_region_cid, lr_pause_ms) },
};

const char *line_region_tone_name(enum udsp_tone tone)
{
	if ((unsigned)tone >= UDSP_REGION_TONE_MAX || !tone_names[tone])
		return "none";
	return tone_names[tone];
}

const char *line_region_flash_service_name(enum udsp_flash_service service)
{
	if ((unsigned)service >= UDSP_FLASH_SERVICE_COUNT)
		return "none";
	return flash_service_names[service];
}

static uint32_t region_tone_cycle_ms(const struct udsp_region_tone *t)
{
	uint32_t sum = 0;
	uint8_t i;

	if (t->segmented) {
		for (i = 0; i < t->segment_count; i++)
			sum += t->segment[i].duration_ms;
		return sum + t->silence_ms;
	}
	for (i = 0; i < t->desc.cadence_count; i++)
		sum += (uint32_t)t->desc.cadence[i].make_ms +
		       t->desc.cadence[i].break_ms;
	return sum;
}

uint32_t line_region_cw_lead_ms(const struct udsp_region_tone *t)
{
	if (!t->present || t->segmented || !t->desc.cadence_count)
		return 0;
	return region_tone_cycle_ms(t) -
	       t->desc.cadence[t->desc.cadence_count - 1].break_ms;
}

uint32_t line_region_tone_total_ms(const struct udsp_region_tone *t)
{
	if (!t->present || t->then_steady || !t->repeat_count)
		return 0;
	return region_tone_cycle_ms(t) * t->repeat_count;
}

static int parse_fail(struct parse_ctx *p, const char *what, const char *detail)
{
	if (!p->err)
		return -EINVAL;
	p->err->line = p->line;
	if (detail)
		snprintf(p->err->text, sizeof(p->err->text), "%s: %s", what, detail);
	else
		snprintf(p->err->text, sizeof(p->err->text), "%s", what);
	return -EINVAL;
}

static char *trim_end(char *s)
{
	size_t n = strlen(s);

	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
		s[--n] = '\0';
	return s;
}

static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	return trim_end(s);
}

static const char *token_take(const char *s, char *tok, size_t cap)
{
	size_t k = 0;

	while (*s == ' ' || *s == '\t')
		s++;
	while (*s && *s != ' ' && *s != '\t') {
		if (k + 1 >= cap)
			return NULL;
		tok[k++] = *s++;
	}
	tok[k] = '\0';
	return s;
}

static int int_parse(const char *s, long min, long max, long *out)
{
	char *end;
	long v;

	if (!*s)
		return -EINVAL;
	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || end == s || *end || v < min || v > max)
		return -EINVAL;
	*out = v;
	return 0;
}

static int list_parse(const char *s, long min, long max, long *vals, unsigned cap,
		      unsigned *count)
{
	char tok[32];
	unsigned n = 0;

	while (*s) {
		s = token_take(s, tok, sizeof(tok));
		if (!s)
			return -EINVAL;
		if (!tok[0])
			break;
		if (n >= cap || int_parse(tok, min, max, &vals[n]))
			return -EINVAL;
		n++;
	}
	if (!n)
		return -EINVAL;
	*count = n;
	return 0;
}

static int cadence_parse(const char *s, struct udsp_cadence *out, unsigned cap,
			 uint8_t *count)
{
	long vals[2 * UDSP_TONE_CADENCE_MAX];
	unsigned n = 0;
	unsigned i;

	if (list_parse(s, 0, REGION_MS_MAX, vals, 2 * cap, &n) || n % 2)
		return -EINVAL;
	for (i = 0; i < n; i += 2) {
		if (!vals[i])
			return -EINVAL;
		out[i / 2].on_ms = (uint16_t)vals[i];
		out[i / 2].off_ms = (uint16_t)vals[i + 1];
	}
	*count = (uint8_t)(n / 2);
	return 0;
}

static int tone_cadence_parse(const char *s, struct udsp_tone_desc *desc)
{
	struct udsp_cadence pairs[UDSP_TONE_CADENCE_MAX];
	uint8_t count = 0;
	uint8_t i;

	if (!strcmp(s, "on")) {
		desc->cadence_count = 0;
		return 0;
	}
	if (cadence_parse(s, pairs, UDSP_TONE_CADENCE_MAX, &count))
		return -EINVAL;
	for (i = 0; i < count; i++) {
		desc->cadence[i].make_ms = pairs[i].on_ms;
		desc->cadence[i].break_ms = pairs[i].off_ms;
	}
	desc->cadence_count = count;
	return 0;
}

static int tone_section_open(struct parse_ctx *p, const char *arg)
{
	unsigned i;

	for (i = 1; i < UDSP_REGION_TONE_MAX; i++) {
		if (!strcmp(arg, tone_names[i]))
			break;
	}
	if (i >= UDSP_REGION_TONE_MAX)
		return parse_fail(p, "unknown tone name", arg);
	if (p->region.tone[i].present)
		return parse_fail(p, "duplicate tone section", arg);
	p->region.tone[i].present = true;
	p->region.tone[i].desc.tone = (enum udsp_tone)i;
	p->tone_tgt = tone_target_of(p, i);
	p->section = SEC_TONE;
	return 0;
}

/* [cw cN] reuses the tone key grammar; its desc.tone label keeps the tone
 * generator's range check happy, exactly as tone_section_open does. */
static int cw_section_open(struct parse_ctx *p, const char *arg)
{
	unsigned i;

	if (arg[0] != 'c' || arg[1] < '1' ||
	    arg[1] > '0' + (int)UDSP_CW_VARIANT_MAX || arg[2])
		return parse_fail(p, "call waiting variant must be c1..c4", arg);
	i = (unsigned)(arg[1] - '1');
	if (p->region.cw[i].present)
		return parse_fail(p, "duplicate cw section", arg);
	p->region.cw[i].present = true;
	p->region.cw[i].desc.tone = UDSP_TONE_CALL_WAITING;
	p->tone_tgt = cw_target_of(p, i);
	p->section = SEC_CW;
	return 0;
}

static int ring_section_open(struct parse_ctx *p, const char *arg)
{
	unsigned i;

	if (arg[0] != 'r' || arg[1] < '1' || arg[1] > '8' || arg[2])
		return parse_fail(p, "ring template must be r1..r8", arg);
	i = (unsigned)(arg[1] - '1');
	if (p->ring_keys[i] & RING_KEY_SEEN)
		return parse_fail(p, "duplicate ring section", arg);
	p->ring_keys[i] |= RING_KEY_SEEN;
	p->ring_index = (int)i;
	p->section = SEC_RING;
	snprintf(p->region.ring[i].name, sizeof(p->region.ring[i].name), "%s", arg);
	return 0;
}

static int section_open(struct parse_ctx *p, char *s)
{
	char *end = strchr(s, ']');
	char *arg;
	unsigned i;

	if (!end || end[1])
		return parse_fail(p, "malformed section header", s);
	*end = '\0';
	s = trim(s + 1);
	arg = strpbrk(s, " \t");
	if (arg) {
		*arg++ = '\0';
		arg = trim(arg);
	}
	if (!strcmp(s, "tone") || !strcmp(s, "ring") || !strcmp(s, "cw")) {
		if (!arg || !*arg)
			return parse_fail(p, "section needs a name", s);
		if (!strcmp(s, "tone"))
			return tone_section_open(p, arg);
		if (!strcmp(s, "ring"))
			return ring_section_open(p, arg);
		return cw_section_open(p, arg);
	}
	if (arg && *arg)
		return parse_fail(p, "section takes no name", s);
	for (i = 0; i < ARRAY_SIZE(section_defs); i++) {
		if (strcmp(s, section_defs[i].name))
			continue;
		if (p->seen[section_defs[i].id])
			return parse_fail(p, "duplicate section", s);
		p->seen[section_defs[i].id] = true;
		p->section = section_defs[i].id;
		return 0;
	}
	return parse_fail(p, "unknown section", s);
}

static bool iso3_valid(const char *value)
{
	unsigned i;

	if (strlen(value) != 3)
		return false;
	for (i = 0; i < 3; i++) {
		if (value[i] < 'A' || value[i] > 'Z')
			return false;
	}
	return true;
}

static int region_key_apply(struct parse_ctx *p, const char *key, const char *value)
{
	if (!strcmp(key, "iso3")) {
		if (!iso3_valid(value))
			return parse_fail(p, "iso3 must be three upper case letters",
					  value);
		snprintf(p->region.iso3, sizeof(p->region.iso3), "%s", value);
		p->region_keys |= 1u;
		return 0;
	}
	if (strcmp(key, "describe"))
		return parse_fail(p, "unknown key in [region]", key);
	if (strlen(value) >= sizeof(p->region.describe))
		return parse_fail(p, "describe too long", key);
	snprintf(p->region.describe, sizeof(p->region.describe), "%s", value);
	return 0;
}

static int freq_list_parse(const char *s, long *hz, unsigned *count)
{
	return list_parse(s, REGION_FREQ_MIN, REGION_FREQ_MAX, hz,
			  UDSP_TONE_FREQ_MAX, count);
}

/*
 * A segment names its frequencies in one token, f1[+f2...], which becomes the
 * blank-separated list of the freq key. token_take skips a run of blanks, so
 * an empty member is refused before the '+' signs turn into blanks.
 */
static int freq_group_parse(const char *group, long *hz, unsigned *count)
{
	char list[32];
	size_t n = strlen(group);
	size_t i;

	if (!n || n >= sizeof(list) || group[0] == '+' || group[n - 1] == '+' ||
	    strstr(group, "++"))
		return -EINVAL;
	for (i = 0; i <= n; i++)
		list[i] = group[i] == '+' ? ' ' : group[i];
	return freq_list_parse(list, hz, count);
}

static int tone_freq_apply(struct parse_ctx *p, struct udsp_region_tone *t,
			   const char *value)
{
	long vals[UDSP_TONE_FREQ_MAX];
	unsigned n = 0;
	unsigned i;

	if (freq_list_parse(value, vals, &n))
		return parse_fail(p, "bad freq list", value);
	for (i = 0; i < n; i++)
		t->desc.frequency_hz[i] = (uint16_t)vals[i];
	t->desc.frequency_count = (uint8_t)n;
	return 0;
}

static int tone_level_apply(struct parse_ctx *p, struct tone_target *tt,
			    const char *value)
{
	long vals[UDSP_TONE_FREQ_MAX];
	unsigned n = 0;
	unsigned i;

	if (list_parse(value, REGION_LEVEL_MIN, REGION_LEVEL_MAX, vals,
		       UDSP_TONE_FREQ_MAX, &n))
		return parse_fail(p, "bad level list", value);
	for (i = 0; i < n; i++)
		tt->level[i] = (int8_t)vals[i];
	*tt->level_count = (uint8_t)n;
	return 0;
}

static int tone_segment_apply(struct parse_ctx *p, struct udsp_region_tone *t,
			      const char *value)
{
	struct udsp_region_segment *seg;
	long hz[UDSP_TONE_FREQ_MAX];
	unsigned count = 0;
	unsigned n = 0;
	unsigned i;
	char group[32];
	const char *rest;
	long vals[2];
	long ms;
	bool off;

	if (t->segment_count >= UDSP_REGION_SEG_MAX)
		return parse_fail(p, "too many segments", value);
	rest = token_take(value, group, sizeof(group));
	off = rest && !strcmp(group, "off");
	if (!rest || list_parse(rest, REGION_LEVEL_MIN, 10000, vals, 2, &n) ||
	    n != (off ? 1u : 2u))
		return parse_fail(p, off ? "off segment needs a duration only" :
				  "segment needs freq level duration", value);
	if (!off && freq_group_parse(group, hz, &count))
		return parse_fail(p, "bad segment frequency list", value);
	if (!off && (vals[0] < REGION_LEVEL_MIN || vals[0] > REGION_LEVEL_MAX))
		return parse_fail(p, "segment level out of range", value);
	ms = vals[n - 1];
	if (ms < 1 || ms > 10000)
		return parse_fail(p, "segment duration out of range", value);
	seg = &t->segment[t->segment_count++];
	for (i = 0; i < count; i++)
		seg->freq_hz[i] = (uint16_t)hz[i];
	seg->freq_count = (uint8_t)count;
	seg->level_dbm0 = off ? 0 : (int8_t)vals[0];
	seg->duration_ms = (uint16_t)ms;
	return 0;
}

static int tone_repeat_apply(struct parse_ctx *p, struct udsp_region_tone *t,
			     const char *value)
{
	long v;

	if (!strcmp(value, "forever")) {
		t->repeat_count = 0;
		return 0;
	}
	if (int_parse(value, 1, 1000, &v))
		return parse_fail(p, "repeat must be forever or 1..1000", value);
	t->repeat_count = (uint16_t)v;
	return 0;
}

static int on_off_parse(struct parse_ctx *p, const char *key, const char *value,
			bool *out)
{
	if (!strcmp(value, "on")) {
		*out = true;
		return 0;
	}
	if (!strcmp(value, "off")) {
		*out = false;
		return 0;
	}
	return parse_fail(p, "value must be on or off", key);
}

static int tone_key_apply(struct parse_ctx *p, const char *key, const char *value)
{
	struct tone_target *tt = &p->tone_tgt;
	struct udsp_region_tone *t = tt->t;
	long v;

	if (!strcmp(key, "freq")) {
		*tt->keys |= TONE_KEY_FREQ;
		return tone_freq_apply(p, t, value);
	}
	if (!strcmp(key, "level")) {
		*tt->keys |= TONE_KEY_LEVEL;
		return tone_level_apply(p, tt, value);
	}
	if (!strcmp(key, "cadence")) {
		*tt->keys |= TONE_KEY_CADENCE;
		if (tone_cadence_parse(value, &t->desc))
			return parse_fail(p, "bad tone cadence", value);
		return 0;
	}
	if (!strcmp(key, "segment")) {
		*tt->keys |= TONE_KEY_SEGMENT;
		return tone_segment_apply(p, t, value);
	}
	if (!strcmp(key, "silence")) {
		*tt->keys |= TONE_KEY_SILENCE;
		if (int_parse(value, 0, 10000, &v))
			return parse_fail(p, "bad silence", value);
		t->silence_ms = (uint16_t)v;
		return 0;
	}
	if (!strcmp(key, "repeat")) {
		*tt->keys |= TONE_KEY_REPEAT;
		return tone_repeat_apply(p, t, value);
	}
	if (strcmp(key, "then"))
		return parse_fail(p, "unknown tone key", key);
	*tt->keys |= TONE_KEY_THEN;
	return on_off_parse(p, key, value, &t->then_steady);
}

static int ring_key_apply(struct parse_ctx *p, const char *key, const char *value)
{
	struct udsp_region_ring *r = &p->region.ring[p->ring_index];

	if (!strcmp(key, "cadence")) {
		p->ring_keys[p->ring_index] |= RING_KEY_CADENCE;
		if (cadence_parse(value, r->cadence, UDSP_CADENCE_MAX,
				  &r->cadence_count))
			return parse_fail(p, "bad ring cadence", value);
		return 0;
	}
	if (strcmp(key, "first"))
		return parse_fail(p, "unknown key in [ring]", key);
	p->ring_keys[p->ring_index] |= RING_KEY_FIRST;
	if (cadence_parse(value, r->first, UDSP_CADENCE_MAX, &r->first_count))
		return parse_fail(p, "bad ring first cadence", value);
	return 0;
}

static int name_take(const char *const *names, size_t count, const char *value,
		     uint8_t *out)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (strcmp(value, names[i]))
			continue;
		*out = (uint8_t)i;
		return 0;
	}
	return -EINVAL;
}

static int cid_format_name_take(const char *value, uint8_t *out)
{
	return name_take(cid_format_names, ARRAY_SIZE(cid_format_names), value, out);
}

static void key_store(void *base, const struct key_def *d, long v)
{
	*(uint16_t *)((char *)base + d->offset) = (uint16_t)v;
}

/* 0 stored the key, -EINVAL is a range error with the "bad <key>" message, and
 * 1 means no descriptor matched, so the caller names the section in the
 * unknown-key message. */
static int keys_apply(struct parse_ctx *p, const char *key, const char *value,
		      const struct key_def *defs, size_t count,
		      void *base, uint32_t *keys)
{
	size_t i;

	for (i = 0; i < count; i++) {
		const struct key_def *d = &defs[i];
		char what[32];
		long v;

		if (strcmp(key, d->name))
			continue;
		if (int_parse(value, d->min, d->max, &v)) {
			snprintf(what, sizeof(what), "bad %s", d->name);
			return parse_fail(p, what, value);
		}
		key_store(base, d, v);
		if (d->bit >= 0)
			*keys |= 1u << (unsigned)d->bit;
		return 0;
	}
	return 1;
}

static int cid_key_apply(struct parse_ctx *p, const char *key, const char *value)
{
	int rc;

	if (!strcmp(key, "format")) {
		p->cid_keys |= 1u << 0;
		if (cid_format_name_take(value, &p->region.cid.format))
			return parse_fail(p, "unknown cid format", value);
		return 0;
	}
	if (!strcmp(key, "mwi_format")) {
		p->cid_mwi_seen = true;
		if (cid_format_name_take(value, &p->region.cid.mwi_format))
			return parse_fail(p, "unknown cid format", value);
		return 0;
	}
	rc = keys_apply(p, key, value, cid_key_defs, ARRAY_SIZE(cid_key_defs),
			&p->region.cid, &p->cid_keys);
	if (rc <= 0)
		return rc;
	return parse_fail(p, "unknown key in [cid]", key);
}

static int hook_key_apply(struct parse_ctx *p, const char *key, const char *value)
{
	unsigned i;
	long v;

	/* Optional: which of the overlapping break and recall windows this
	 * country arms by default. */
	if (!strcmp(key, "pulse_enable"))
		return on_off_parse(p, key, value, &p->region.hook.pulse_enable);
	/* Optional: the country codes a dialed N as 10 - N pulses. The hook
	 * machine tags the count with it, and the caller maps the count. */
	if (!strcmp(key, "pulse_invert"))
		return on_off_parse(p, key, value,
				    &p->region.hook.pulse_invert);
	if (!strcmp(key, "flash_service")) {
		p->hook_keys |= HOOK_KEY_FLASH_SERVICE;
		if (name_take(flash_service_names, ARRAY_SIZE(flash_service_names),
			      value, &p->region.flash_service))
			return parse_fail(p, "unknown flash service", value);
		return 0;
	}
	for (i = 0; i < ARRAY_SIZE(hook_key_defs); i++) {
		const struct key_def *d = &hook_key_defs[i];

		if (strcmp(key, d->name))
			continue;
		if (int_parse(value, d->min, d->max, &v))
			return parse_fail(p, "hook value out of range", key);
		key_store(&p->region.hook, d, v);
		p->hook_keys |= 1u << (unsigned)d->bit;
		return 0;
	}
	return parse_fail(p, "unknown key in [hook]", key);
}

static int key_apply(struct parse_ctx *p, const char *key, const char *value)
{
	if (!*key || !*value)
		return parse_fail(p, "empty key or value", key);
	switch (p->section) {
	case SEC_REGION:
		return region_key_apply(p, key, value);
	case SEC_TONE:
	case SEC_CW:
		return tone_key_apply(p, key, value);
	case SEC_RING:
		return ring_key_apply(p, key, value);
	case SEC_CID:
		return cid_key_apply(p, key, value);
	case SEC_HOOK:
		return hook_key_apply(p, key, value);
	default:
		break;
	}
	return parse_fail(p, "key outside any section", key);
}

static int line_apply(struct parse_ctx *p, char *buf)
{
	char *hash = strchr(buf, '#');
	char *value;
	char *s;

	if (hash)
		*hash = '\0';
	s = trim(buf);
	if (!*s)
		return 0;
	if (*s == '[')
		return section_open(p, s);
	value = strchr(s, '=');
	if (!value)
		return parse_fail(p, "not a key = value line", s);
	*value++ = '\0';
	return key_apply(p, trim_end(s), trim(value));
}

static int tone_check(struct parse_ctx *p, struct tone_target *tt,
		      const char *name)
{
	struct udsp_region_tone *t = tt->t;
	uint8_t keys = *tt->keys;
	uint8_t levels = *tt->level_count;
	uint8_t k;

	if (!t->present)
		return parse_fail(p, "missing tone section", name);
	if ((keys & TONE_KEYS_SIMPLE) && (keys & TONE_KEYS_SEGMENT))
		return parse_fail(p, "tone mixes simple and segment form", name);
	t->desc.repeat = t->repeat_count != 1;
	if (t->then_steady && !t->repeat_count)
		return parse_fail(p, "then needs a finite repeat count", name);
	if (keys & TONE_KEY_SEGMENT) {
		if (t->then_steady &&
		    !t->segment[t->segment_count - 1].freq_count)
			return parse_fail(p, "then needs a last segment with a "
					  "frequency", name);
		t->segmented = true;
		return 0;
	}
	if ((keys & TONE_KEYS_SIMPLE) != TONE_KEYS_SIMPLE)
		return parse_fail(p, "tone needs freq, level and cadence", name);
	if (levels != 1 && levels != t->desc.frequency_count)
		return parse_fail(p, "level count matches neither 1 nor freq count",
				  name);
	for (k = 0; k < t->desc.frequency_count; k++)
		t->desc.level_dbm0[k] = tt->level[levels == 1 ? 0 : k];
	return 0;
}

/*
 * The distinctive call-waiting rows. The table is optional, so count 0 is a
 * valid region. A row must be a simple finite cadence that draws a subscriber
 * alerting signal: no segment, silence or then form, no continuous tone, a
 * presentation inside the GR-30-CORE X window, and a trailing break that clears
 * the type 2 exchange so the next presentation never starts during the CAS, the
 * acknowledgement and the FSK.
 */
static int cw_check(struct parse_ctx *p)
{
	unsigned i;

	for (i = 0; i < UDSP_CW_VARIANT_MAX; i++) {
		if (!p->region.cw[i].present)
			break;
	}
	p->region.cw_count = (uint8_t)i;
	for (; i < UDSP_CW_VARIANT_MAX; i++) {
		if (p->region.cw[i].present)
			return parse_fail(p, "cw variants must run from c1 with "
					  "no gap", NULL);
	}
	for (i = 0; i < p->region.cw_count; i++) {
		struct tone_target tt = cw_target_of(p, i);
		struct udsp_region_tone *t = tt.t;
		/* "c1".."c4"; built by hand so the format-truncation check needs no
		 * value-range proof that i + 1 is a single digit. */
		char name[] = { 'c', (char)('1' + i), '\0' };
		uint32_t lead, exchange_ms;
		int rc;

		if (*tt.keys & (TONE_KEY_SEGMENT | TONE_KEY_SILENCE | TONE_KEY_THEN))
			return parse_fail(p, "cw variant must be a simple tone",
					  name);
		rc = tone_check(p, &tt, name);
		if (rc)
			return rc;
		if (!t->desc.cadence_count)
			return parse_fail(p, "cw variant needs a cadence, not on",
					  name);
		if (!t->repeat_count)
			return parse_fail(p, "cw variant needs a finite repeat count",
					  name);
		lead = line_region_cw_lead_ms(t);
		if (lead < LINE_CW_SAS_MIN_MS || lead > LINE_CW_SAS_MAX_MS)
			return parse_fail(p, "cw presentation outside 250..1000 ms",
					  name);
		/* The next presentation must not start during the type 2 exchange:
		 * the CAS, the region wait and acknowledgement windows, and a full
		 * FSK burst. The API tone resumes while the acknowledgement window
		 * is open, so a fixed floor would miss a region with a long ack. */
		exchange_ms = LINE_CAS_MS + p->region.cid.wait_ms +
			      p->region.cid.ack_timeout_ms + LINE_CW_FSK_BURST_MS;
		if (t->desc.cadence[t->desc.cadence_count - 1].break_ms < exchange_ms)
			return parse_fail(p, "cw trailing break is shorter than the "
					  "type 2 exchange", name);
	}
	return 0;
}

static int ring_check(struct parse_ctx *p)
{
	unsigned i;

	for (i = 0; i < UDSP_REGION_RING_MAX; i++) {
		if (!(p->ring_keys[i] & RING_KEY_CADENCE))
			break;
	}
	p->region.ring_count = (uint8_t)i;
	for (; i < UDSP_REGION_RING_MAX; i++) {
		if (p->ring_keys[i])
			return parse_fail(p, "ring templates must run from r1 with "
					  "no gap", NULL);
	}
	if (!p->region.ring_count)
		return parse_fail(p, "no ring template", NULL);
	return 0;
}

static int hook_check(struct parse_ctx *p)
{
	const struct udsp_region_hook *h = &p->region.hook;

	if (h->pulse_break_min_ms > h->pulse_break_max_ms)
		return parse_fail(p, "pulse_break_min_ms above pulse_break_max_ms",
				  NULL);
	/* The break window may overlap the flash window (most of Europe does);
	 * hook.c resolves that by configuration. It may never reach a release. */
	if (h->pulse_break_max_ms >= h->release_min_ms)
		return parse_fail(p, "pulse_break_max_ms not below release_min_ms",
				  NULL);
	if (h->flash_min_ms > h->flash_max_ms)
		return parse_fail(p, "flash_min_ms above flash_max_ms", NULL);
	if (h->release_min_ms <= h->flash_max_ms)
		return parse_fail(p, "release_min_ms not above flash_max_ms", NULL);
	if (h->pulse_make_max_ms >= h->pulse_interdigit_ms)
		return parse_fail(p, "pulse_make_max_ms not below "
				  "pulse_interdigit_ms", NULL);
	return 0;
}

static const char *hook_missing_key(const struct parse_ctx *p)
{
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(hook_key_defs); i++) {
		if (!(p->hook_keys & (1u << (unsigned)hook_key_defs[i].bit)))
			return hook_key_defs[i].name;
	}
	return "flash_service";
}

static int region_check(struct parse_ctx *p)
{
	unsigned i;
	int rc;

	p->line = 0;
	if (!p->seen[SEC_REGION] || !(p->region_keys & 1u))
		return parse_fail(p, "missing [region] iso3", NULL);
	if (!p->seen[SEC_CID] || p->cid_keys != CID_KEYS_ALL)
		return parse_fail(p, "incomplete [cid] section", NULL);
	if (!p->seen[SEC_HOOK])
		return parse_fail(p, "missing [hook] section", NULL);
	if (p->hook_keys != HOOK_KEYS_ALL)
		return parse_fail(p, "missing [hook] key", hook_missing_key(p));
	/* A parsed region is complete: the optional alert timings take their
	 * defaults here rather than at every use. */
	if (!p->region.cid.dtas_ms)
		p->region.cid.dtas_ms = LINE_DTAS_MS;
	if (!p->region.cid.rpas_ms)
		p->region.cid.rpas_ms = LINE_RPAS_MS;
	if (!p->region.cid.lr_pause_ms)
		p->region.cid.lr_pause_ms = LINE_LR_PAUSE_MS;
	if (!p->cid_mwi_seen)
		p->region.cid.mwi_format = p->region.cid.format;
	for (i = 1; i < UDSP_REGION_TONE_MAX; i++) {
		struct tone_target tt = tone_target_of(p, i);

		rc = tone_check(p, &tt, tone_names[i]);
		if (rc)
			return rc;
	}
	rc = ring_check(p);
	if (rc)
		return rc;
	rc = cw_check(p);
	if (rc)
		return rc;
	return hook_check(p);
}

static size_t line_take(const char *text, size_t len, size_t *pos, char *buf,
			size_t cap)
{
	size_t n = 0;

	while (*pos < len && text[*pos] != '\n') {
		if (n + 1 < cap)
			buf[n] = text[*pos];
		n++;
		(*pos)++;
	}
	if (*pos < len)
		(*pos)++;
	buf[n + 1 < cap ? n : cap - 1] = '\0';
	return n;
}

int line_region_parse(const char *text, size_t len, struct udsp_region *out,
		      struct line_region_error *err)
{
	char buf[UDSP_REGION_LINE_MAX + 2];
	struct parse_ctx p;
	size_t pos = 0;
	int rc;

	if (!text || !out)
		return -EINVAL;
	memset(&p, 0, sizeof(p));
	p.err = err;
	if (err) {
		err->line = 0;
		err->text[0] = '\0';
	}
	if (len > UDSP_REGION_FILE_MAX) {
		parse_fail(&p, "file above 16 KiB", NULL);
		return -EFBIG;
	}
	if (memchr(text, '\0', len))
		return parse_fail(&p, "embedded NUL byte", NULL);
	while (pos < len) {
		p.line++;
		if (line_take(text, len, &pos, buf, sizeof(buf)) > UDSP_REGION_LINE_MAX)
			return parse_fail(&p, "line above 200 characters", NULL);
		rc = line_apply(&p, buf);
		if (rc)
			return rc;
	}
	rc = region_check(&p);
	if (rc)
		return rc;
	*out = p.region;
	return 0;
}

int line_region_load(const char *path, struct udsp_region *out,
		     struct line_region_error *err)
{
	char text[UDSP_REGION_FILE_MAX + 1];
	FILE *f;
	size_t n;

	if (err) {
		err->line = 0;
		err->text[0] = '\0';
	}
	if (!path || !out)
		return -EINVAL;
	f = fopen(path, "rb");
	if (!f)
		return -errno;
	n = fread(text, 1, sizeof(text), f);
	if (ferror(f)) {
		fclose(f);
		return -EIO;
	}
	fclose(f);
	return line_region_parse(text, n, out, err);
}
