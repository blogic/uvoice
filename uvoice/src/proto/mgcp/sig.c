// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The MGCP package tables. See mgcp/sig.h for the contracts.
 */
#include <ctype.h>
#include <string.h>

#include "proto/mgcp/sig.h"
#include "str.h"

#define ARRAY_LEN(a)	(sizeof(a) / sizeof((a)[0]))

const char *mgcp_pkg_name(enum mgcp_pkg pkg)
{
	switch (pkg) {
	case MGCP_PKG_L:
		return "L";
	case MGCP_PKG_G:
		return "G";
	case MGCP_PKG_D:
		return "D";
	case MGCP_PKG_NONE:
		break;
	}

	return "";
}

enum mgcp_pkg mgcp_pkg_of(enum cfg_mgcp_dialect dialect, struct sip_str name)
{
	if (sip_str_eq_ci(name, "L"))
		return MGCP_PKG_L;
	if (dialect == CFG_MGCP_NCS)
		return MGCP_PKG_NONE;
	if (sip_str_eq_ci(name, "G"))
		return MGCP_PKG_G;
	if (sip_str_eq_ci(name, "D"))
		return MGCP_PKG_D;

	return MGCP_PKG_NONE;
}

enum mgcp_pkg mgcp_pkg_digits(enum cfg_mgcp_dialect dialect)
{
	return dialect == CFG_MGCP_NCS ? MGCP_PKG_L : MGCP_PKG_D;
}

enum mgcp_pkg mgcp_pkg_tones(enum cfg_mgcp_dialect dialect)
{
	return dialect == CFG_MGCP_NCS ? MGCP_PKG_L : MGCP_PKG_G;
}

/* ------------------------------------------------------------- signals */

#define SIG(n, p, t, pr, a, f, to) \
	{ .name = (n), .pkg = (p), .type = (t), .prim = (pr), .arg = (a), \
	  .flags = (f), .to_ms = (to) }

#define OFFH	MGCP_SIG_F_OFFHOOK
#define ONH	MGCP_SIG_F_ONHOOK
#define CONN	MGCP_SIG_F_CONN
#define DEF	MGCP_SIG_RING_DEFAULT

/*
 * RFC 3660 2.4 (page 24), 2.1 (page 7) and 2.2 (page 11). The 402 rows
 * follow the signal texts of pages 25 to 31 and page 8; the 401 rows are the
 * ring signals (pages 29 and 30). An event-only name of a package is not a
 * signal and gets 522.
 */
static const struct mgcp_sig_def sig_rfc3435[] = {
	SIG("adsi", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_NONE, 0, 0, 0),
	SIG("aw", MGCP_PKG_L, MGCP_SIG_OO, MGCP_PRIM_NONE, 0, 0, 0),
	SIG("bz", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_TONE, HAL_TONE_BUSY,
	    OFFH, 30000),
	SIG("ci", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_CID, 0, 0, 0),
	SIG("dl", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_TONE, HAL_TONE_DIAL,
	    OFFH, 16000),
	SIG("e", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_NONE, 0, 0, 2000),
	SIG("ht", MGCP_PKG_L, MGCP_SIG_OO, MGCP_PRIM_NONE, 0, 0, 0),
	SIG("lsa", MGCP_PKG_L, MGCP_SIG_OO, MGCP_PRIM_LSA, 0, 0, 0),
	SIG("mwi", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_TONE, HAL_TONE_STUTTER,
	    OFFH, 16000),
	SIG("nbz", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_TONE, HAL_TONE_REORDER,
	    0, 0),
	SIG("osi", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_OSI, 0, 0, 900),
	SIG("ot", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_TONE, HAL_TONE_HOWLER,
	    OFFH, 0),
	SIG("p", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_NONE, 0, 0, 0),
	SIG("rg", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_RING, DEF, ONH, 180000),
	SIG("r0", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_RING, DEF, ONH, 180000),
	SIG("r1", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_RING, 1, ONH, 180000),
	SIG("r2", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_RING, 2, ONH, 180000),
	SIG("r3", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_RING, 3, ONH, 180000),
	SIG("r4", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_RING, 4, ONH, 180000),
	SIG("r5", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_RING, 5, ONH, 180000),
	SIG("r6", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_RING, DEF, ONH, 180000),
	SIG("r7", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_RING, DEF, ONH, 180000),
	SIG("ro", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_TONE, HAL_TONE_REORDER,
	    0, 30000),
	SIG("rs", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_SPLASH, 0, ONH, 0),
	SIG("s", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_NONE, 0, 0, 0),
	SIG("sit", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_NONE, 0, 0, 2000),
	SIG("sl", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_TONE, HAL_TONE_STUTTER,
	    OFFH, 16000),
	SIG("v", MGCP_PKG_L, MGCP_SIG_OO, MGCP_PRIM_NONE, 0, 0, 0),
	SIG("vmwi", MGCP_PKG_L, MGCP_SIG_OO, MGCP_PRIM_VMWI, 0, 0, 0),
	SIG("wt", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_CW, 0, 0, 12000),
	SIG("wt1", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_CW, 1, 0, 12000),
	SIG("wt2", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_CW, 2, 0, 12000),
	SIG("wt3", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_CW, 3, 0, 12000),
	SIG("wt4", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_CW, 4, 0, 12000),
	SIG("y", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_NONE, 0, 0, 0),
	SIG("z", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_NONE, 0, 0, 0),

	SIG("cf", MGCP_PKG_G, MGCP_SIG_BR, MGCP_PRIM_TONE, HAL_TONE_CONFIRM,
	    OFFH, 0),
	SIG("cg", MGCP_PKG_G, MGCP_SIG_TO, MGCP_PRIM_TONE, HAL_TONE_REORDER,
	    0, 0),
	SIG("it", MGCP_PKG_G, MGCP_SIG_TO, MGCP_PRIM_NONE, 0, 0, 0),
	SIG("pat", MGCP_PKG_G, MGCP_SIG_OO, MGCP_PRIM_NONE, 0, 0, 0),
	SIG("pt", MGCP_PKG_G, MGCP_SIG_TO, MGCP_PRIM_NONE, 0, 0, 0),
	SIG("rbk", MGCP_PKG_G, MGCP_SIG_TO, MGCP_PRIM_NONE, 0, 0, 180000),
	SIG("rt", MGCP_PKG_G, MGCP_SIG_TO, MGCP_PRIM_TONE, HAL_TONE_RINGBACK,
	    OFFH | CONN, 180000),

	SIG("0", MGCP_PKG_D, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '0', OFFH, 0),
	SIG("1", MGCP_PKG_D, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '1', OFFH, 0),
	SIG("2", MGCP_PKG_D, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '2', OFFH, 0),
	SIG("3", MGCP_PKG_D, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '3', OFFH, 0),
	SIG("4", MGCP_PKG_D, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '4', OFFH, 0),
	SIG("5", MGCP_PKG_D, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '5', OFFH, 0),
	SIG("6", MGCP_PKG_D, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '6', OFFH, 0),
	SIG("7", MGCP_PKG_D, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '7', OFFH, 0),
	SIG("8", MGCP_PKG_D, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '8', OFFH, 0),
	SIG("9", MGCP_PKG_D, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '9', OFFH, 0),
	SIG("*", MGCP_PKG_D, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '*', OFFH, 0),
	SIG("#", MGCP_PKG_D, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '#', OFFH, 0),
	SIG("A", MGCP_PKG_D, MGCP_SIG_BR, MGCP_PRIM_DIGIT, 'A', OFFH, 0),
	SIG("B", MGCP_PKG_D, MGCP_SIG_BR, MGCP_PRIM_DIGIT, 'B', OFFH, 0),
	SIG("C", MGCP_PKG_D, MGCP_SIG_BR, MGCP_PRIM_DIGIT, 'C', OFFH, 0),
	SIG("D", MGCP_PKG_D, MGCP_SIG_BR, MGCP_PRIM_DIGIT, 'D', OFFH, 0),
	SIG("DD", MGCP_PKG_D, MGCP_SIG_TO, MGCP_PRIM_NONE, 0, 0, 3000),
	SIG("DO", MGCP_PKG_D, MGCP_SIG_OO, MGCP_PRIM_NONE, 0, 0, 0),
	SIG("T", MGCP_PKG_D, MGCP_SIG_TO, MGCP_PRIM_NONE, 0, 0, 16000),
};

/*
 * NCS Table 19 (page 109). The wt time-out is (MaxReps + 1) + MaxReps *
 * Delay seconds with the defaults MaxReps 1 and Delay 10. The 402 rows
 * follow the signal texts of pages 110 to 113, and ro and wt1 to wt4 carry
 * it there.
 */
static const struct mgcp_sig_def sig_ncs[] = {
	SIG("0", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '0', OFFH, 0),
	SIG("1", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '1', OFFH, 0),
	SIG("2", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '2', OFFH, 0),
	SIG("3", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '3', OFFH, 0),
	SIG("4", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '4', OFFH, 0),
	SIG("5", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '5', OFFH, 0),
	SIG("6", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '6', OFFH, 0),
	SIG("7", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '7', OFFH, 0),
	SIG("8", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '8', OFFH, 0),
	SIG("9", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '9', OFFH, 0),
	SIG("*", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '*', OFFH, 0),
	SIG("#", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_DIGIT, '#', OFFH, 0),
	SIG("A", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_DIGIT, 'A', OFFH, 0),
	SIG("B", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_DIGIT, 'B', OFFH, 0),
	SIG("C", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_DIGIT, 'C', OFFH, 0),
	SIG("D", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_DIGIT, 'D', OFFH, 0),
	SIG("bz", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_TONE, HAL_TONE_BUSY,
	    OFFH, 30000),
	SIG("cf", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_TONE, HAL_TONE_CONFIRM,
	    OFFH, 0),
	SIG("ci", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_CID, 0, 0, 0),
	SIG("dl", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_TONE, HAL_TONE_DIAL,
	    OFFH, 16000),
	SIG("mwi", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_TONE, HAL_TONE_STUTTER,
	    OFFH, 16000),
	SIG("osi", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_OSI, 0, 0, 900),
	SIG("ot", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_TONE, HAL_TONE_HOWLER,
	    OFFH, 0),
	SIG("rg", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_RING, DEF, ONH, 180000),
	SIG("r0", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_RING, DEF, ONH, 180000),
	SIG("r1", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_RING, 1, ONH, 180000),
	SIG("r2", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_RING, 2, ONH, 180000),
	SIG("r3", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_RING, 3, ONH, 180000),
	SIG("r4", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_RING, 4, ONH, 180000),
	SIG("r5", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_RING, 5, ONH, 180000),
	SIG("r6", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_RING, DEF, ONH, 180000),
	SIG("r7", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_RING, DEF, ONH, 180000),
	SIG("ro", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_TONE, HAL_TONE_REORDER,
	    OFFH, 30000),
	SIG("rs", MGCP_PKG_L, MGCP_SIG_BR, MGCP_PRIM_SPLASH, 0, ONH, 0),
	SIG("rt", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_TONE, HAL_TONE_RINGBACK,
	    OFFH | CONN, 180000),
	SIG("sl", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_TONE, HAL_TONE_STUTTER,
	    OFFH, 16000),
	SIG("vmwi", MGCP_PKG_L, MGCP_SIG_OO, MGCP_PRIM_VMWI, 0, 0, 0),
	SIG("wt1", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_CW, 1, OFFH, 12000),
	SIG("wt2", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_CW, 2, OFFH, 12000),
	SIG("wt3", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_CW, 3, OFFH, 12000),
	SIG("wt4", MGCP_PKG_L, MGCP_SIG_TO, MGCP_PRIM_CW, 4, OFFH, 12000),
};

#undef SIG
#undef OFFH
#undef ONH
#undef CONN
#undef DEF

/* A digit, X or T name, or a range, which takes the digit package when the
 * name carries no package. */
static bool name_digitish(struct sip_str n)
{
	char c;

	if (!n.len)
		return false;
	if (n.p[0] == '[')
		return true;
	if (n.len != 1)
		return false;

	c = (char)toupper((unsigned char)n.p[0]);

	return mgcp_sig_digit_bit(c) >= 0 || c == 'X' || c == 'T';
}

/* The package an event or signal name names, the dialect's digit package
 * for an unqualified digit name. */
static enum mgcp_pkg ev_pkg(enum cfg_mgcp_dialect dialect,
			    const struct mgcp_event *ev)
{
	if (ev->pkg_default && name_digitish(ev->name))
		return mgcp_pkg_digits(dialect);

	return mgcp_pkg_of(dialect, ev->pkg);
}

const struct mgcp_sig_def *mgcp_sig_lookup(enum cfg_mgcp_dialect dialect,
					   const struct mgcp_event *ev,
					   uint32_t *code)
{
	const struct mgcp_sig_def *tab;
	enum mgcp_pkg pkg = ev_pkg(dialect, ev);
	size_t n, i;

	if (pkg == MGCP_PKG_NONE) {
		*code = 518;
		return NULL;
	}

	if (dialect == CFG_MGCP_NCS) {
		tab = sig_ncs;
		n = ARRAY_LEN(sig_ncs);
	} else {
		tab = sig_rfc3435;
		n = ARRAY_LEN(sig_rfc3435);
	}

	for (i = 0; i < n; i++) {
		if (tab[i].pkg == pkg && sip_str_eq_ci(ev->name, tab[i].name))
			return &tab[i];
	}

	*code = 522;

	return NULL;
}

static bool num_take(struct sip_str v, uint32_t *out)
{
	return sip_str_uint(sip_str_trim(v), out);
}

/* One parameter of a TO signal: to, rep for a ring, del for sl. */
static bool to_param_take(const struct mgcp_sig_def *def, struct sip_str name,
			  struct sip_str value, struct mgcp_sig_params *out)
{
	/* uloop takes an int, and the daemon bounds a line timer at a day */
	if (sip_str_eq_ci(name, "to")) {
		out->to_set = true;
		return num_take(value, &out->to_ms) &&
		       out->to_ms <= LINE_TIMER_MAX_S * 1000u;
	}

	if (def->prim == MGCP_PRIM_RING && sip_str_eq_ci(name, "rep")) {
		out->rep_set = true;
		return num_take(value, &out->rep) && out->rep >= 1 &&
		       out->rep <= 255;
	}

	/* RFC 3660 page 31: the delay of sl between its confirmation tone and
	 * its dial tone. The tone profile of the region sets that gap. */
	if (!strcmp(def->name, "sl") && sip_str_eq_ci(name, "del")) {
		uint32_t del;

		return num_take(value, &del) && del <= 10000;
	}

	return false;
}

bool mgcp_sig_params_parse(const struct mgcp_sig_def *def,
			   const struct mgcp_event *ev,
			   struct mgcp_sig_params *out)
{
	struct sip_str rest = ev->params, name, value;

	memset(out, 0, sizeof(*out));
	if (!ev->has_params)
		return true;
	if (def->prim == MGCP_PRIM_CID)
		return true;

	while (mgcp_event_param_next(&rest, &name, &value)) {
		if (def->type == MGCP_SIG_OO && !name.len &&
		    (sip_str_eq(value, "+") || sip_str_eq(value, "-"))) {
			out->off = sip_str_eq(value, "-");
			continue;
		}
		if (def->type != MGCP_SIG_TO || !name.len)
			return false;
		if (!to_param_take(def, name, value, out))
			return false;
	}

	return true;
}

uint32_t mgcp_sig_timeout(const struct mgcp_sig_def *def,
			  const struct mgcp_sig_params *p, uint32_t cycle_ms)
{
	uint64_t ms;

	if (def->type != MGCP_SIG_TO)
		return 0;

	if (p->rep_set && def->prim == MGCP_PRIM_RING) {
		ms = (uint64_t)p->rep *
		     (cycle_ms ? cycle_ms : MGCP_SIG_RING_CYCLE_MS);
		return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
	}

	if (p->to_set)
		return p->to_ms;

	return def->to_ms;
}

unsigned int mgcp_sig_ring_template(const struct proto_host *host,
				    unsigned int line,
				    const struct mgcp_sig_def *def,
				    const char *number)
{
	if (def->arg != MGCP_SIG_RING_DEFAULT)
		return def->arg;

	return host->ring_template_match(host->ctx, line,
					 number ? number : "");
}

/*
 * One field of ci(): the text up to the next comma outside a quoted string.
 * Empty fields count, because RFC 3660 page 25 keeps every comma.
 */
static bool cid_field_next(struct sip_str *rest, struct sip_str *field,
			   bool *last)
{
	bool quoted = false;
	size_t i;

	for (i = 0; i < rest->len; i++) {
		char c = rest->p[i];

		if (c == '"') {
			quoted = !quoted;
			continue;
		}
		if (c == ',' && !quoted)
			break;
	}
	if (quoted)
		return false;

	*field = sip_str_trim(sip_str_init(rest->p, i));
	*last = i >= rest->len;
	*rest = i < rest->len ? sip_str_init(rest->p + i + 1,
					     rest->len - i - 1) :
				sip_str_init(rest->p + rest->len, 0);

	return true;
}

/*
 * A field longer than the record keeps its head, as a display does. A
 * quoted field loses its quotes, and "" inside it is one quote.
 */
static bool cid_text_take(struct sip_str field, char *out, size_t olen)
{
	bool quoted = field.len && field.p[0] == '"';
	size_t i, used = 0;

	if (quoted) {
		if (field.len < 2 || field.p[field.len - 1] != '"')
			return false;
		field = mgcp_quoted_strip(field);
	}

	for (i = 0; i < field.len && used + 1 < olen; i++) {
		if (quoted && field.p[i] == '"' && i + 1 < field.len &&
		    field.p[i + 1] == '"')
			i++;
		out[used++] = field.p[i];
	}
	out[used] = '\0';

	return true;
}

static void digits_keep(char *s)
{
	char *w = s;

	for (; *s; s++) {
		if (isdigit((unsigned char)*s))
			*w++ = *s;
	}
	*w = '\0';
}

/* "P" withholds the field, "O" says it is unavailable (RFC 3660 page 26). */
static void cid_mark_take(char *text, bool *priv)
{
	if (!strcmp(text, "P")) {
		*priv = true;
		text[0] = '\0';
		return;
	}
	if (!strcmp(text, "O"))
		text[0] = '\0';
}

static void space_drop(char *s)
{
	char *w = s;

	for (; *s; s++) {
		if (!isspace((unsigned char)*s))
			*w++ = *s;
	}
	*w = '\0';
}

bool mgcp_sig_cid_parse(struct sip_str params, struct proto_cid *out)
{
	struct sip_str rest = params, field[3];
	bool last = false;
	unsigned int n = 0;

	memset(out, 0, sizeof(*out));

	while (!last) {
		if (n >= ARRAY_LEN(field))
			return false;
		if (!cid_field_next(&rest, &field[n], &last))
			return false;
		n++;
	}
	if (n != ARRAY_LEN(field))
		return false;

	if (!cid_text_take(field[0], out->datetime, sizeof(out->datetime)) ||
	    !cid_text_take(field[1], out->number, sizeof(out->number)) ||
	    !cid_text_take(field[2], out->name, sizeof(out->name)))
		return false;

	digits_keep(out->datetime);
	cid_mark_take(out->number, &out->number_private);
	space_drop(out->number);
	cid_mark_take(out->name, &out->name_private);
	out->present = true;

	return true;
}

/* ------------------------------------------------------------- events */

int mgcp_sig_digit_bit(char digit)
{
	char c = (char)toupper((unsigned char)digit);

	if (c >= '0' && c <= '9')
		return c - '0';
	if (c == '*')
		return 10;
	if (c == '#')
		return 11;
	if (c >= 'A' && c <= 'D')
		return 12 + (c - 'A');

	return -1;
}

#define DIGITS_0_9	0x03ffu
#define DIGITS_ALL	0xffffu

/*
 * A range of RFC 3435 3.2.2.4: digits, letters A to D, "*", "#", X, T and
 * subranges between two digits or two letters, as "0-9" and "A-D".
 */
static bool range_take(struct sip_str name, struct mgcp_ev_pat *out)
{
	struct sip_str in;
	size_t i;

	if (name.len < 3 || name.p[name.len - 1] != ']')
		return false;

	in = sip_str_init(name.p + 1, name.len - 2);
	for (i = 0; i < in.len; i++) {
		char c = (char)toupper((unsigned char)in.p[i]);
		int bit = mgcp_sig_digit_bit(c);

		if (i + 2 < in.len && in.p[i + 1] == '-') {
			char hi = (char)toupper((unsigned char)in.p[i + 2]);
			int lo_bit = bit, hi_bit = mgcp_sig_digit_bit(hi), b;
			bool digits = isdigit((unsigned char)c) &&
				      isdigit((unsigned char)hi);
			bool letters = c >= 'A' && c <= 'D' &&
				       hi >= 'A' && hi <= 'D';

			if ((!digits && !letters) || hi_bit < lo_bit)
				return false;
			for (b = lo_bit; b <= hi_bit; b++)
				out->digits |= (uint16_t)(1u << b);
			i += 2;
			continue;
		}

		if (bit >= 0)
			out->digits |= (uint16_t)(1u << bit);
		else if (c == 'X')
			out->digits |= DIGITS_0_9;
		else if (c == 'T')
			out->timer = true;
		else
			return false;
	}

	return out->digits || out->timer;
}

static uint32_t digit_pat_take(struct sip_str name, struct mgcp_ev_pat *out)
{
	int bit;

	out->kind = MGCP_EVK_DIGIT;
	if (name.p[0] == '[')
		return range_take(name, out) ? 0 : 522;
	if (name.len != 1)
		return 522;

	bit = mgcp_sig_digit_bit(name.p[0]);
	if (bit >= 0) {
		out->digits = (uint16_t)(1u << bit);
		return 0;
	}
	if (toupper((unsigned char)name.p[0]) == 'X') {
		out->digits = DIGITS_0_9;
		return 0;
	}
	if (toupper((unsigned char)name.p[0]) == 'T') {
		out->timer = true;
		return 0;
	}

	return 522;
}

struct ev_name {
	const char *name;
	uint8_t kind;			/* MGCP_EVK_NONE: not detected */
};

/* RFC 3660 pages 24 (R column), 7 and 11 */
static const struct ev_name ev_l_rfc3435[] = {
	{ "hd", MGCP_EVK_HD }, { "hu", MGCP_EVK_HU }, { "hf", MGCP_EVK_HF },
	{ "oc", MGCP_EVK_OC }, { "of", MGCP_EVK_OF }, { "aw", MGCP_EVK_NONE },
	{ "e", MGCP_EVK_NONE }, { "nbz", MGCP_EVK_NONE },
	{ "p", MGCP_EVK_NONE }, { "s", MGCP_EVK_NONE },
};

static const struct ev_name ev_g[] = {
	{ "ft", MGCP_EVK_FT }, { "mt", MGCP_EVK_MT }, { "oc", MGCP_EVK_OC },
	{ "of", MGCP_EVK_OF }, { "ld", MGCP_EVK_NONE },
	{ "pat", MGCP_EVK_NONE },
};

static const struct ev_name ev_d[] = {
	{ "oc", MGCP_EVK_OC }, { "of", MGCP_EVK_OF }, { "L", MGCP_EVK_NONE },
	{ "DD", MGCP_EVK_NONE },
};

/* NCS Table 19, the Event column */
static const struct ev_name ev_l_ncs[] = {
	{ "hd", MGCP_EVK_HD }, { "hu", MGCP_EVK_HU }, { "hf", MGCP_EVK_HF },
	{ "oc", MGCP_EVK_OC }, { "of", MGCP_EVK_OF }, { "ft", MGCP_EVK_FT },
	{ "mt", MGCP_EVK_MT }, { "L", MGCP_EVK_NONE },
	{ "ld", MGCP_EVK_NONE }, { "ma", MGCP_EVK_NONE },
	{ "TDD", MGCP_EVK_NONE },
};

static uint32_t named_pat_take(const struct ev_name *tab, size_t n,
			       struct sip_str name, struct mgcp_ev_pat *out)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (!sip_str_eq_ci(name, tab[i].name))
			continue;
		if (tab[i].kind == MGCP_EVK_NONE)
			return 512;
		out->kind = tab[i].kind;
		return 0;
	}

	return 522;
}

static uint32_t ev_name_take(enum cfg_mgcp_dialect dialect,
			     enum mgcp_pkg pkg, const struct mgcp_event *ev,
			     struct mgcp_ev_pat *out)
{
	bool digit_pkg = pkg == mgcp_pkg_digits(dialect);

	if (sip_str_eq_ci(ev->name, "all")) {
		out->all = true;
		return 0;
	}

	if (digit_pkg && name_digitish(ev->name))
		return digit_pat_take(ev->name, out);

	switch (pkg) {
	case MGCP_PKG_L:
		if (dialect == CFG_MGCP_NCS)
			return named_pat_take(ev_l_ncs, ARRAY_LEN(ev_l_ncs),
					      ev->name, out);
		return named_pat_take(ev_l_rfc3435, ARRAY_LEN(ev_l_rfc3435),
				      ev->name, out);
	case MGCP_PKG_G:
		return named_pat_take(ev_g, ARRAY_LEN(ev_g), ev->name, out);
	case MGCP_PKG_D:
		return named_pat_take(ev_d, ARRAY_LEN(ev_d), ev->name, out);
	case MGCP_PKG_NONE:
		break;
	}

	return 518;
}

uint32_t mgcp_ev_pat_compile(enum cfg_mgcp_dialect dialect,
			     const struct mgcp_event *ev,
			     struct mgcp_ev_pat *out)
{
	enum mgcp_pkg pkg = ev_pkg(dialect, ev);
	uint32_t code;

	memset(out, 0, sizeof(*out));
	out->pkg = (uint8_t)pkg;

	if (pkg == MGCP_PKG_NONE)
		return 518;

	code = ev_name_take(dialect, pkg, ev, out);
	if (code)
		return code;

	/* no event of these packages is detected on a connection, and none
	 * takes a parameter (RFC 3661 pages 11 and 18) */
	if (ev->conn.len)
		return 512;

	return ev->has_params ? 538 : 0;
}

/* The package an observed event belongs to under the dialect. */
static enum mgcp_pkg obs_pkg(enum cfg_mgcp_dialect dialect,
			     const struct mgcp_obs *o)
{
	switch (o->kind) {
	case MGCP_EVK_DIGIT:
	case MGCP_EVK_T:
		return mgcp_pkg_digits(dialect);
	case MGCP_EVK_FT:
	case MGCP_EVK_MT:
		return mgcp_pkg_tones(dialect);
	case MGCP_EVK_OC:
	case MGCP_EVK_OF:
		return mgcp_obs_sig_pkg(dialect, o);
	default:
		return MGCP_PKG_L;
	}
}

bool mgcp_ev_pat_match(enum cfg_mgcp_dialect dialect,
		       const struct mgcp_ev_pat *p, const struct mgcp_obs *o)
{
	int bit;

	if (p->pkg != obs_pkg(dialect, o))
		return false;
	if (p->all)
		return true;

	switch (o->kind) {
	case MGCP_EVK_DIGIT:
		bit = mgcp_sig_digit_bit(o->digit);
		return p->kind == MGCP_EVK_DIGIT && bit >= 0 &&
		       (p->digits & (1u << bit));
	case MGCP_EVK_T:
		return p->kind == MGCP_EVK_DIGIT && p->timer;
	default:
		return p->kind == o->kind;
	}
}

uint32_t mgcp_ev_pat_detect_mask(enum cfg_mgcp_dialect dialect,
				 const struct mgcp_ev_pat *p)
{
	uint32_t mask = 0;

	if (p->all) {
		if (p->pkg == mgcp_pkg_digits(dialect))
			mask |= HAL_DETECT_DTMF | HAL_DETECT_PULSE;
		if (p->pkg == mgcp_pkg_tones(dialect))
			mask |= HAL_DETECT_MODEM;
		return mask;
	}

	switch (p->kind) {
	case MGCP_EVK_DIGIT:
		return p->digits ? HAL_DETECT_DTMF | HAL_DETECT_PULSE : 0;
	case MGCP_EVK_FT:
	case MGCP_EVK_MT:
		return HAL_DETECT_MODEM;
	default:
		return 0;
	}
}

static uint8_t tone_kind(uint16_t code)
{
	switch (code) {
	case HAL_TONE_DET_CNG:
	case HAL_TONE_DET_V21_PREAMBLE:
	case HAL_TONE_DET_FAX_CM:
		return MGCP_EVK_FT;
	case HAL_TONE_DET_CED:
	case HAL_TONE_DET_ANS:
	case HAL_TONE_DET_ANSAM:
	case HAL_TONE_DET_ANSBAR:
	case HAL_TONE_DET_MODEM:
		return MGCP_EVK_MT;
	default:
		return MGCP_EVK_NONE;
	}
}

bool mgcp_obs_of_line_event(const struct proto_line_event *ev,
			    struct mgcp_obs *out)
{
	memset(out, 0, sizeof(*out));

	switch (ev->type) {
	case PROTO_EV_HOOK:
		if (ev->u.hook.state == HAL_HOOK_SEIZE)
			out->kind = MGCP_EVK_HD;
		else if (ev->u.hook.state == HAL_HOOK_RELEASE)
			out->kind = MGCP_EVK_HU;
		else if (ev->u.hook.state == HAL_HOOK_FLASH)
			out->kind = MGCP_EVK_HF;
		break;
	case PROTO_EV_DIGIT:
		if (mgcp_sig_digit_bit(ev->u.digit.digit) < 0)
			return false;
		out->kind = MGCP_EVK_DIGIT;
		out->digit = (char)toupper((unsigned char)ev->u.digit.digit);
		break;
	case PROTO_EV_TONE:
		if (!ev->u.tone.end)
			out->kind = tone_kind(ev->u.tone.code);
		break;
	default:
		break;
	}

	return out->kind != MGCP_EVK_NONE;
}

static const char *obs_name(const struct mgcp_obs *o)
{
	switch (o->kind) {
	case MGCP_EVK_HD:
		return "hd";
	case MGCP_EVK_HU:
		return "hu";
	case MGCP_EVK_HF:
		return "hf";
	case MGCP_EVK_T:
		return "T";
	case MGCP_EVK_FT:
		return "ft";
	case MGCP_EVK_MT:
		return "mt";
	case MGCP_EVK_OC:
		return "oc";
	case MGCP_EVK_OF:
		return "of";
	default:
		return "";
	}
}

bool mgcp_obs_write(enum cfg_mgcp_dialect dialect, const struct mgcp_obs *o,
		    struct sip_buf *b)
{
	const char *pkg = mgcp_pkg_name(obs_pkg(dialect, o));

	if (o->kind == MGCP_EVK_DIGIT)
		return sip_buf_addf(b, "%s/%c", pkg, o->digit);
	if (o->kind == MGCP_EVK_OC || o->kind == MGCP_EVK_OF)
		return sip_buf_addf(b, "%s/%s(%s)", pkg, obs_name(o), o->sig);

	return sip_buf_addf(b, "%s/%s", pkg, obs_name(o));
}

enum mgcp_pkg mgcp_obs_sig_pkg(enum cfg_mgcp_dialect dialect,
			       const struct mgcp_obs *o)
{
	const char *slash = strchr(o->sig, '/');

	if (!slash)
		return MGCP_PKG_L;

	return mgcp_pkg_of(dialect, sip_str_init(o->sig,
						 (size_t)(slash - o->sig)));
}
