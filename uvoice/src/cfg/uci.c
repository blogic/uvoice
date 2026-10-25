// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libubox/blobmsg.h>
#include <libubox/utils.h>
#include <uci.h>

#include "cfg.h"
#include "schema.h"
#include "../dialplan.h"
#include "../str.h"
#include "../trace.h"

#define CFG_PACKAGE		"voip"
#define CFG_SECTION_BASE	"base"
#define CFG_SECTION_HAL		"hal"
#define CFG_SECTION_CHANNEL	"channel"
#define CFG_SECTION_CODEC	"codec"
#define CFG_SECTION_DEBUG	"debug"
#define CFG_SECTION_MGCP	"mgcp"

static bool bool_parse(const char *val, bool *out)
{
	if (!val || !*val)
		return false;

	if (!strcmp(val, "1") || !strcasecmp(val, "y") || !strcasecmp(val, "yes") ||
	    !strcasecmp(val, "true") || !strcasecmp(val, "on") ||
	    !strcasecmp(val, "enabled")) {
		*out = true;
		return true;
	}

	if (!strcmp(val, "0") || !strcasecmp(val, "n") || !strcasecmp(val, "no") ||
	    !strcasecmp(val, "false") || !strcasecmp(val, "off") ||
	    !strcasecmp(val, "disabled")) {
		*out = false;
		return true;
	}

	return false;
}

/*
 * strtoul() accepts "-1" and wraps it into a huge value, so a sign is rejected
 * before the conversion runs. Callers decide what a malformed value means.
 */
static bool uint_parse(const char *val, unsigned int *out)
{
	unsigned long num;
	char *end;

	if (!val || !*val || *val == '-' || *val == '+')
		return false;

	errno = 0;
	num = strtoul(val, &end, 10);
	if (*end || (num == ULONG_MAX && errno == ERANGE) || num > UINT_MAX)
		return false;

	*out = (unsigned int)num;

	return true;
}

static bool int_parse(const char *val, int *out)
{
	long num;
	char *end;

	if (!val || !*val)
		return false;

	errno = 0;
	num = strtol(val, &end, 10);
	if (*end || (num == LONG_MIN && errno == ERANGE) ||
	    (num == LONG_MAX && errno == ERANGE) || num < INT_MIN || num > INT_MAX)
		return false;

	*out = (int)num;

	return true;
}

/* ------------------------------------------------------------ schema readers */

static const struct cfg_option_desc *desc_find(const struct cfg_option_desc *tbl,
					       const char *name)
{
	for (; tbl && tbl->name; tbl++) {
		if (!strcmp(tbl->name, name))
			return tbl;
	}

	return NULL;
}

static const char *desc_default(const struct cfg_option_desc *tbl, const char *name)
{
	const struct cfg_option_desc *d = desc_find(tbl, name);

	return d ? d->def : NULL;
}

static bool enum_value_valid(const struct cfg_option_desc *d, const char *val)
{
	const char *const *candidate;

	if (!d || d->type != CFG_ENUM || !d->enum_vals || !val)
		return false;

	for (candidate = d->enum_vals; *candidate; candidate++) {
		if (!strcmp(*candidate, val))
			return true;
	}

	return false;
}

/* A NULL section holds no option, so a reader returns its default for it. */
static const char *opt_get(struct uci_context *ctx, struct uci_section *s,
			   const char *name)
{
	return s ? uci_lookup_option_string(ctx, s, name) : NULL;
}

static struct uci_option *opt_find(struct uci_context *ctx,
				   struct uci_section *s, const char *name)
{
	return s ? uci_lookup_option(ctx, s, name) : NULL;
}

/*
 * The section rides the warning, and so does the outcome. An option name can
 * live in more than one descriptor table (transport does), so a line that
 * names the option alone cannot say which section carried the fault. A row
 * whose descriptor names no default has none
 * to fall back to either, and a "using default" line for a value that ends up
 * unset would tell an operator about an outcome that did not happen.
 */
static const char *opt_enum(struct uci_context *ctx, struct uci_section *s,
			    const struct cfg_option_desc *tbl, const char *name)
{
	const struct cfg_option_desc *d = desc_find(tbl, name);
	const char *val = opt_get(ctx, s, name);
	const char *def = d && enum_value_valid(d, d->def) ? d->def : NULL;

	if (enum_value_valid(d, val))
		return val;

	if (val && *val && def)
		ULOG_WARN("voip %s %s value '%s' is invalid, using default '%s'\n",
			  s->e.name, name, val, def);
	else if (val && *val)
		ULOG_WARN("voip %s %s value '%s' is invalid, ignored\n",
			  s->e.name, name, val);

	return def;
}

/* String option, falling back to the schema default when the config omits it. */
static void opt_str(struct uci_context *ctx, struct uci_section *s,
		    const struct cfg_option_desc *tbl, const char *name, char *dst,
		    size_t len)
{
	const char *val = opt_get(ctx, s, name);
	const struct cfg_option_desc *d;

	if (val && *val) {
		str_copy(dst, len, val);
		return;
	}

	d = desc_find(tbl, name);
	str_copy(dst, len, d && d->def ? d->def : "");
}

static void opt_str_none(struct uci_context *ctx, struct uci_section *s,
			 const struct cfg_option_desc *tbl, const char *name,
			 char *dst, size_t len)
{
	const char *val = opt_get(ctx, s, name);

	if (val && !strcmp(val, "none")) {
		str_copy(dst, len, "");
		return;
	}

	opt_str(ctx, s, tbl, name, dst, len);
}

/*
 * A string with a rule of its own. Text that the rule refuses, or that the
 * field cannot hold whole, warns once in the opt_enum() form and takes the
 * schema default: a cut host, pattern or list is a different value, not a
 * shorter one. A NULL rule checks the length alone. With none set, the
 * literal "none" loads empty, the opt_str_none() idiom, because libuci drops
 * an empty value.
 */
static void opt_str_checked(struct uci_context *ctx, struct uci_section *s,
			    const struct cfg_option_desc *tbl, const char *name,
			    char *dst, size_t len, bool (*valid)(const char *),
			    bool none)
{
	const char *val = opt_get(ctx, s, name);
	const char *def = desc_default(tbl, name);

	if (none && val && !strcmp(val, "none")) {
		str_copy(dst, len, "");
		return;
	}

	if (val && strlen(val) < len && (!valid || valid(val))) {
		str_copy(dst, len, val);
		return;
	}

	if (val)
		ULOG_WARN("voip %s %s value '%s' is invalid, using default "
			  "'%s'\n", s->e.name, name, val, def ? def : "");

	str_copy(dst, len, def);
}

/*
 * A star code. The schema default is the local service's code and applies
 * only while the base star_codes_local says the defaults are local; with it
 * off an absent code loads empty, so the dialled digits fall through the
 * feature table to the dial plan. A present code is local in either mode,
 * and the literal "none" clears one the way it clears slic_plugin.
 */
static void opt_code(struct uci_context *ctx, struct uci_section *s,
		     const struct cfg_option_desc *tbl, const char *name,
		     char *dst, size_t len, bool defaults)
{
	const char *val = opt_get(ctx, s, name);
	const struct cfg_option_desc *d;

	if (val && *val) {
		str_copy(dst, len, strcmp(val, "none") ? val : "");
		return;
	}

	d = defaults ? desc_find(tbl, name) : NULL;
	str_copy(dst, len, d && d->def ? d->def : "");
}

/*
 * Clamp a value to the descriptor bounds before any narrowing cast. Without
 * this a sip_local_port of 65536 survives uint_parse and only wraps to 0 when
 * the account narrows it to uint16, binding an ephemeral port while the Contact
 * still says 5060. The clamp keeps the low bound only when it is positive, so a
 * gain option with an INT_MIN floor is left alone. The high bound is always a
 * real ceiling, including INT_MAX for values consumed through signed integers.
 */
static unsigned int uint_clamp(const struct cfg_option_desc *d, const char *name,
			       unsigned int num)
{
	if (!d || (d->type != CFG_INT && d->type != CFG_ENUM))
		return num;

	if (d->min > 0 && num < (unsigned int)d->min) {
		ULOG_WARN("voip %s %u below %d, clamped\n", name, num, d->min);
		num = (unsigned int)d->min;
	}

	if (d->max > 0 && num > (unsigned int)d->max) {
		ULOG_WARN("voip %s %u above %d, clamped\n", name, num, d->max);
		num = (unsigned int)d->max;
	}

	return num;
}

static unsigned int desc_uint_default(const struct cfg_option_desc *tbl,
				      const char *name)
{
	const struct cfg_option_desc *d = desc_find(tbl, name);
	unsigned int num;

	if (!d || !uint_parse(d->def, &num))
		return 0;

	return uint_clamp(d, name, num);
}

static unsigned int opt_enum_uint(struct uci_context *ctx, struct uci_section *s,
				  const struct cfg_option_desc *tbl, const char *name)
{
	const struct cfg_option_desc *d = desc_find(tbl, name);
	const char *val = opt_enum(ctx, s, tbl, name);
	unsigned int num;

	if (!uint_parse(val, &num))
		return 0;

	return uint_clamp(d, name, num);
}

static unsigned int opt_enum_uint_bool_compat(struct uci_context *ctx,
					      struct uci_section *s,
					      const struct cfg_option_desc *tbl,
					      const char *name)
{
	const struct cfg_option_desc *d = desc_find(tbl, name);
	const char *val = opt_get(ctx, s, name);
	bool parsed;

	if (!enum_value_valid(d, val) && bool_parse(val, &parsed))
		return parsed ? 1 : 0;

	return opt_enum_uint(ctx, s, tbl, name);
}

/*
 * One integer read, warning where the option NAME is known: the alias reader
 * below calls this once per name, so a fault is reported against the option
 * that carried the text and not against the canonical name it resolves to.
 */
static bool opt_uint_take(struct uci_context *ctx, struct uci_section *s,
			  const char *name, unsigned int *out)
{
	const char *val = opt_get(ctx, s, name);

	if (val && uint_parse(val, out))
		return true;

	if (val && *val)
		ULOG_WARN("voip %s value '%s' is not a number, using default\n", name,
			  val);

	return false;
}

/*
 * Integer option, falling back to the schema default. A value that does not
 * parse is a provisioning fault and takes the default rather than a guess. The
 * result is clamped to the descriptor bounds.
 */
static unsigned int opt_uint(struct uci_context *ctx, struct uci_section *s,
			     const struct cfg_option_desc *tbl, const char *name)
{
	unsigned int num;

	if (opt_uint_take(ctx, s, name, &num))
		return uint_clamp(desc_find(tbl, name), name, num);

	return desc_uint_default(tbl, name);
}

static int int_clamp(const struct cfg_option_desc *d, const char *name, int num)
{
	/* Signed HAL values use real negative bounds. This deliberately differs
	 * from uint_clamp(), which applies a lower bound only when it is positive. */
	if (!d || d->type != CFG_INT)
		return num;

	if (num < d->min) {
		ULOG_WARN("voip %s %d below %d, clamped\n", name, num, d->min);
		num = d->min;
	}

	if (num > d->max) {
		ULOG_WARN("voip %s %d above %d, clamped\n", name, num, d->max);
		num = d->max;
	}

	return num;
}

static int desc_int_default(const struct cfg_option_desc *tbl, const char *name)
{
	const struct cfg_option_desc *d = desc_find(tbl, name);
	int num;

	if (!d || !int_parse(d->def, &num))
		return 0;

	return int_clamp(d, name, num);
}

static int opt_int(struct uci_context *ctx, struct uci_section *s,
		   const struct cfg_option_desc *tbl, const char *name)
{
	const struct cfg_option_desc *d = desc_find(tbl, name);
	const char *val = opt_get(ctx, s, name);
	int num;

	if (int_parse(val, &num))
		return int_clamp(d, name, num);

	if (val && *val)
		ULOG_WARN("voip %s value '%s' is not a number, using default\n",
			  name, val);

	return desc_int_default(tbl, name);
}

/*
 * The port form of opt_bool_optional: the value follows opt_uint, and the
 * flag says whether the config carried a port of its own.
 *
 * A value that does not parse behaves as absent. A port of 0 behaves as
 * absent too. The dependent ports already read 0 as their own inherit
 * sentinel (opt_uint0()), so a port of 0 here is no port either.
 */
static unsigned int opt_port_optional(struct uci_context *ctx,
				      struct uci_section *s,
				      const struct cfg_option_desc *tbl,
				      const char *name, bool *is_set)
{
	unsigned int num;

	*is_set = opt_uint_take(ctx, s, name, &num) && num != 0;

	return *is_set ? uint_clamp(desc_find(tbl, name), name, num) :
			 desc_uint_default(tbl, name);
}

/*
 * Integer option that stays 0 when the config omits it, for the port fields
 * that inherit another option through the fallback chain instead of a fixed
 * default. A value above the 16 bit port range is clamped, never wrapped.
 */
static unsigned int opt_uint0(struct uci_context *ctx, struct uci_section *s,
			      const char *name)
{
	const char *val = opt_get(ctx, s, name);
	unsigned int num;

	if (!val || !uint_parse(val, &num))
		return 0;

	if (num > 65535) {
		ULOG_WARN("voip %s %u above 65535, clamped\n", name, num);
		num = 65535;
	}

	return num;
}

static bool desc_bool_default(const struct cfg_option_desc *tbl, const char *name)
{
	bool parsed;

	return bool_parse(desc_default(tbl, name), &parsed) ? parsed : false;
}

static bool opt_bool(struct uci_context *ctx, struct uci_section *s,
		     const struct cfg_option_desc *tbl, const char *name)
{
	const char *val = opt_get(ctx, s, name);
	bool parsed;

	if (!val || !*val)
		return desc_bool_default(tbl, name);

	if (bool_parse(val, &parsed))
		return parsed;

	/*
	 * A value this parser refuses behaves as an absent option: the
	 * schema default applies and no presence flag is raised.
	 */
	ULOG_WARN("voip %s value '%s' is not a boolean, treated as unset\n", name, val);

	return desc_bool_default(tbl, name);
}

static bool opt_bool_optional(struct uci_context *ctx, struct uci_section *s,
			      const struct cfg_option_desc *tbl,
			      const char *name, bool *is_set)
{
	const char *val = opt_get(ctx, s, name);
	bool parsed;

	*is_set = bool_parse(val, &parsed);
	return *is_set ? parsed : opt_bool(ctx, s, tbl, name);
}

/*
 * A value with an alias: the canonical option wins, the alias fills in, and a
 * fixed default closes the chain. sip_registration_expiry accepts
 * sip_registration_exp_time, and sip_register_retry accepts
 * sip_rereg_delta_exp_time.
 */
static unsigned int opt_uint_alias(struct uci_context *ctx, struct uci_section *s,
				   const struct cfg_option_desc *tbl,
				   const char *name, const char *alias)
{
	unsigned int num;

	if (opt_uint_take(ctx, s, name, &num) ||
	    opt_uint_take(ctx, s, alias, &num))
		return uint_clamp(desc_find(tbl, name), name, num);

	return desc_uint_default(tbl, name);
}

/*
 * The two call waiting codes: the alias wins, a present value loads as
 * written (an empty one clears), "none" clears, and the schema default
 * closes the chain only while the star code defaults are local (opt_code()).
 */
static void opt_str_late_alias(struct uci_context *ctx, struct uci_section *s,
			       const struct cfg_option_desc *tbl, const char *name,
			       const char *alias, char *dst, size_t len,
			       bool defaults)
{
	struct uci_option *option = opt_find(ctx, s, alias);
	const char *val;

	if (!option)
		option = opt_find(ctx, s, name);
	if (!option) {
		str_copy(dst, len, defaults ? desc_default(tbl, name) : "");
		return;
	}

	val = option->type == UCI_TYPE_STRING ? option->v.string : NULL;
	if (val && !strcmp(val, "none"))
		val = "";
	str_copy(dst, len, val);
}

static unsigned int codec_priority_get(struct uci_context *ctx,
				       struct uci_section *s, const char *name)
{
	const struct cfg_option_desc *d = desc_find(cfg_codec_options, name);
	const char *val = opt_get(ctx, s, name);
	unsigned int priority;

	if (!val || !uint_parse(val, &priority))
		return UINT_MAX;

	return uint_clamp(d, name, priority);
}

static const char *enum_word(const char *const *table, unsigned int bound,
			     unsigned int val)
{
	return val < bound ? table[val] : "unknown";
}

/* Every word table is NULL-terminated, so the bound is always the table's
 * own length minus one. A bound taken any other way could fall out of step
 * with its table and read the terminator as a word. */
#define ENUM_WORD(table, val) enum_word((table), ARRAY_SIZE(table) - 1, (val))

/*
 * The inverse of enum_word(). opt_enum() hands over a word of the table or
 * the schema default. It hands over NULL only for a row with no default.
 * NULL and a word the table does not hold read as enum value 0.
 */
static unsigned int enum_index(const char *const *table, const char *word)
{
	unsigned int i;

	for (i = 0; word && table[i]; i++) {
		if (!strcmp(table[i], word))
			return i;
	}

	return 0;
}

static unsigned int enum_count(const char *const *table)
{
	unsigned int n = 0;

	while (table[n])
		n++;

	return n;
}

/* ------------------------------------------------------------- field tables */

/*
 * An option and the struct member that it loads. The kind names the reader
 * and the dump form. A row with no name is a dump-only value that no option
 * loads. A row with no key is a credential, which the dump leaves out
 * (cfg.h). An enum row takes its words from the enum_vals of its schema row.
 * A row with an alias reads it through opt_uint_alias() or
 * opt_str_late_alias(). A STR_CHECKED row reads through opt_str_checked()
 * with the grammar of its valid hook, and a UINT row with a floor hook
 * through field_uint_floor().
 */
enum cfg_field_kind {
	FIELD_END,
	FIELD_BOOL,
	FIELD_BOOL_SET,
	FIELD_UINT,
	FIELD_UINT_SET,
	FIELD_UINT0,
	FIELD_PORT_SET,
	FIELD_PRIORITY,
	FIELD_ENUM_UINT,
	FIELD_ENUM_IDX,
	FIELD_INT,
	FIELD_STR,
	FIELD_STR_NONE,
	FIELD_STR_CHECKED,
	FIELD_ENUM_STR,
	FIELD_CODE,
};

struct cfg_field {
	const char *name;
	const char *key;
	enum cfg_field_kind kind;
	size_t offset;
	size_t size;
	const char *alias;
	const char *set_key;
	size_t set;		/* the bool presence flag of a *_SET row */
	bool dump_late;		/* the dump reports the next row first */
	bool (*valid)(const char *val);	/* the rule of a STR_CHECKED row */
	bool none;		/* a STR_CHECKED row reads "none" as empty */
	/* the low bound that a UINT row takes from a row loaded before it */
	unsigned int (*floor)(const void *obj);
	const char *floor_name;
};

/* The pointer type of the member that each kind loads. */
#define FIELD_P_BOOL(m)		bool *
#define FIELD_P_BOOL_SET(m)	bool *
#define FIELD_P_UINT(m)		unsigned int *
#define FIELD_P_UINT_SET(m)	unsigned int *
#define FIELD_P_UINT0(m)	unsigned int *
#define FIELD_P_PORT_SET(m)	unsigned int *
#define FIELD_P_PRIORITY(m)	unsigned int *
#define FIELD_P_ENUM_UINT(m)	unsigned int *
#define FIELD_P_ENUM_IDX(m)	unsigned int *
#define FIELD_P_INT(m)		int *
#define FIELD_P_STR(m)		char (*)[sizeof(((FIELD_TYPE *)0)->m)]
#define FIELD_P_STR_NONE(m)	FIELD_P_STR(m)
#define FIELD_P_STR_CHECKED(m)	FIELD_P_STR(m)
#define FIELD_P_ENUM_STR(m)	FIELD_P_STR(m)
#define FIELD_P_CODE(m)		FIELD_P_STR(m)

/*
 * A table defines FIELD_TYPE as the struct that its rows load. FIELD_FITS is
 * 1 when member m has the type of kind t. A string kind needs a char array.
 * An enum member fits unsigned int, because the compiler gives an enum with
 * no negative value that type. A wrong kind fails the build with "size of
 * unnamed array is negative".
 */
#define FIELD_FITS(t, m) \
	sizeof(char[_Generic(&((FIELD_TYPE *)0)->m, FIELD_P_##t(m): 1, \
			     default: -1)])
#define FIELD_AT(t, m) \
	.kind = FIELD_##t, .offset = offsetof(FIELD_TYPE, m), \
	.size = sizeof(((FIELD_TYPE *)0)->m) * FIELD_FITS(t, m)
#define FIELD_ROW(t, n, k, m)	{ .name = n, .key = k, FIELD_AT(t, m) }
#define FIELD(t, n, m)		FIELD_ROW(t, n, n, m)
#define FIELD_SECRET(t, n, m)	FIELD_ROW(t, n, NULL, m)
#define FIELD_DUMP(t, k, m)	FIELD_ROW(t, NULL, k, m)
#define FIELD_LATE(t, n, m) \
	{ .name = n, .key = n, .dump_late = true, FIELD_AT(t, m) }
#define FIELD_ALIAS(t, n, a, m) \
	{ .name = n, .key = n, .alias = a, FIELD_AT(t, m) }
#define FIELD_FLAGGED(t, n, k, m, flag) \
	{ .name = n, .key = n, .set_key = k, FIELD_AT(t, m), \
	  .set = offsetof(FIELD_TYPE, flag) * FIELD_FITS(BOOL, flag) }
#define FIELD_SET(t, n, m)	FIELD_FLAGGED(t, n, n "_set", m, m##_set)
#define FIELD_CHECKED(n, m, fn) \
	{ .name = n, .key = n, .valid = fn, FIELD_AT(STR_CHECKED, m) }
#define FIELD_CHECKED_NONE(n, m, fn) \
	{ .name = n, .key = n, .valid = fn, .none = true, \
	  FIELD_AT(STR_CHECKED, m) }
#define FIELD_FLOOR(n, m, fn, fname) \
	{ .name = n, .key = n, .floor = fn, .floor_name = fname, \
	  FIELD_AT(UINT, m) }
#define FIELD_LAST		{ .kind = FIELD_END }

/* The presence flag of a BOOL_SET, UINT_SET or PORT_SET row. */
static bool *field_flag(const struct cfg_field *f, const void *obj)
{
	return (bool *)((const char *)obj + f->set);
}

/* A value that does not parse raises no flag and logs nothing. */
static void field_uint_set_parse(const struct cfg_field *f,
				 struct uci_context *ctx, struct uci_section *s,
				 const struct cfg_option_desc *tbl,
				 unsigned int *num, bool *set)
{
	*set = uint_parse(opt_get(ctx, s, f->name), num);
	if (*set)
		*num = uint_clamp(desc_find(tbl, f->name), f->name, *num);
}

/*
 * A row with a floor hook resolves the floor and the schema bounds in one
 * clamp, so one bad value logs one warning. The warning names the bound that
 * binds: the floor when it lies above the schema minimum, else uint_clamp()
 * reports the schema bound. A value that does not parse warned already and
 * meets the floor silently. Every floor lies below the schema maximum.
 */
static unsigned int field_uint_floor(const struct cfg_field *f,
				     struct uci_context *ctx,
				     struct uci_section *s,
				     const struct cfg_option_desc *tbl,
				     const void *obj)
{
	const struct cfg_option_desc *d = desc_find(tbl, f->name);
	const char *val = opt_get(ctx, s, f->name);
	unsigned int floor = f->floor(obj);
	bool warned = false;
	unsigned int num;

	if (!opt_uint_take(ctx, s, f->name, &num)) {
		num = desc_uint_default(tbl, f->name);
		warned = val && *val;
	}

	if (num < floor && (!d || d->min < 0 || floor > (unsigned int)d->min)) {
		if (!warned)
			ULOG_WARN("voip %s %u below %s (%u), clamped\n",
				  f->name, num, f->floor_name, floor);
		num = floor;
	}

	return uint_clamp(d, f->name, num);
}

static unsigned int field_uint_parse(const struct cfg_field *f,
				     struct uci_context *ctx,
				     struct uci_section *s,
				     const struct cfg_option_desc *tbl,
				     const void *obj)
{
	if (f->floor)
		return field_uint_floor(f, ctx, s, tbl, obj);

	if (f->alias)
		return opt_uint_alias(ctx, s, tbl, f->name, f->alias);

	return opt_uint(ctx, s, tbl, f->name);
}

static void field_code_parse(const struct cfg_field *f, struct uci_context *ctx,
			     struct uci_section *s,
			     const struct cfg_option_desc *tbl, bool codes,
			     char *dst)
{
	if (f->alias)
		opt_str_late_alias(ctx, s, tbl, f->name, f->alias, dst,
				   f->size, codes);
	else
		opt_code(ctx, s, tbl, f->name, dst, f->size, codes);
}

/* With s NULL, each row loads what an absent option loads. */
static void fields_parse(const struct cfg_field *f, struct uci_context *ctx,
			 struct uci_section *s,
			 const struct cfg_option_desc *tbl, bool codes,
			 void *obj)
{
	for (; f->kind != FIELD_END; f++) {
		char *p = (char *)obj + f->offset;

		if (!f->name)
			continue;

		switch (f->kind) {
		case FIELD_BOOL:
			*(bool *)p = opt_bool(ctx, s, tbl, f->name);
			break;
		case FIELD_BOOL_SET:
			*(bool *)p = opt_bool_optional(ctx, s, tbl, f->name,
						       field_flag(f, obj));
			break;
		case FIELD_UINT:
			*(unsigned int *)p = field_uint_parse(f, ctx, s, tbl,
							      obj);
			break;
		case FIELD_UINT_SET:
			field_uint_set_parse(f, ctx, s, tbl,
					     (unsigned int *)p,
					     field_flag(f, obj));
			break;
		case FIELD_UINT0:
			*(unsigned int *)p = opt_uint0(ctx, s, f->name);
			break;
		case FIELD_PORT_SET:
			*(unsigned int *)p = opt_port_optional(
				ctx, s, tbl, f->name, field_flag(f, obj));
			break;
		case FIELD_PRIORITY:
			*(unsigned int *)p = codec_priority_get(ctx, s,
								f->name);
			break;
		case FIELD_ENUM_UINT:
			*(unsigned int *)p = opt_enum_uint(ctx, s, tbl,
							   f->name);
			break;
		case FIELD_ENUM_IDX:
			*(unsigned int *)p = enum_index(
				desc_find(tbl, f->name)->enum_vals,
				opt_enum(ctx, s, tbl, f->name));
			break;
		case FIELD_INT:
			*(int *)p = opt_int(ctx, s, tbl, f->name);
			break;
		case FIELD_STR:
			opt_str(ctx, s, tbl, f->name, p, f->size);
			break;
		case FIELD_STR_NONE:
			opt_str_none(ctx, s, tbl, f->name, p, f->size);
			break;
		case FIELD_STR_CHECKED:
			opt_str_checked(ctx, s, tbl, f->name, p, f->size,
					f->valid, f->none);
			break;
		case FIELD_ENUM_STR:
			str_copy(p, f->size, opt_enum(ctx, s, tbl, f->name));
			break;
		case FIELD_CODE:
			field_code_parse(f, ctx, s, tbl, codes, p);
			break;
		case FIELD_END:
			break;
		}
	}
}

static void field_dump(const struct cfg_field *f,
		       const struct cfg_option_desc *tbl, const void *obj,
		       struct blob_buf *b)
{
	const char *p = (const char *)obj + f->offset;
	const char *const *vals;

	if (!f->key)
		return;

	switch (f->kind) {
	case FIELD_BOOL:
		blobmsg_add_u8(b, f->key, *(const bool *)p);
		break;
	case FIELD_BOOL_SET:
		blobmsg_add_u8(b, f->set_key, *field_flag(f, obj));
		if (*field_flag(f, obj))
			blobmsg_add_u8(b, f->key, *(const bool *)p);
		break;
	case FIELD_PORT_SET:
		blobmsg_add_u32(b, f->key, *(const unsigned int *)p);
		blobmsg_add_u8(b, f->set_key, *field_flag(f, obj));
		break;
	case FIELD_UINT_SET:
		if (*field_flag(f, obj))
			blobmsg_add_u32(b, f->key, *(const unsigned int *)p);
		break;
	case FIELD_UINT:
	case FIELD_UINT0:
	case FIELD_PRIORITY:
	case FIELD_ENUM_UINT:
		blobmsg_add_u32(b, f->key, *(const unsigned int *)p);
		break;
	case FIELD_ENUM_IDX:
		vals = desc_find(tbl, f->name)->enum_vals;
		blobmsg_add_string(b, f->key,
				   enum_word(vals, enum_count(vals),
					     *(const unsigned int *)p));
		break;
	case FIELD_INT:
		blobmsg_add_u32(b, f->key, (uint32_t)*(const int *)p);
		break;
	case FIELD_STR:
	case FIELD_STR_NONE:
	case FIELD_STR_CHECKED:
	case FIELD_ENUM_STR:
	case FIELD_CODE:
		blobmsg_add_string(b, f->key, p);
		break;
	case FIELD_END:
		break;
	}
}

static void fields_dump(const struct cfg_field *f,
			const struct cfg_option_desc *tbl, const void *obj,
			struct blob_buf *b)
{
	for (; f->kind != FIELD_END; f++) {
		if (f->dump_late && f[1].kind != FIELD_END) {
			field_dump(f + 1, tbl, obj, b);
			field_dump(f++, tbl, obj, b);
			continue;
		}
		field_dump(f, tbl, obj, b);
	}
}

/*
 * Every symbol of the digit map grammar is printable ASCII, so a decoded byte
 * outside that range belongs to no map. It is refused here, at the decoder:
 * the loader prints the decoded text, and a line feed in it would forge log
 * lines.
 */
static bool digit_map_printable(const char *map, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)map[i];

		if (c < 0x20 || c > 0x7e)
			return false;
	}

	return true;
}

static bool digit_map_decode(const char *encoded, char *dst, size_t len)
{
	char decoded[CFG_DIGIT_MAP_MAX];
	int decoded_len;

	if (!encoded || !*encoded || len < 2)
		return false;
	if (B64_DECODE_LEN(strlen(encoded)) > sizeof(decoded))
		return false;

	decoded_len = b64_decode(encoded, decoded, sizeof(decoded));
	if (decoded_len <= 0 || (size_t)decoded_len >= sizeof(decoded) ||
	    (size_t)decoded_len >= len ||
	    !digit_map_printable(decoded, (size_t)decoded_len))
		return false;

	decoded[decoded_len] = '\0';
	str_copy(dst, len, decoded);

	return true;
}

static void digit_map_parse(struct cfg_channel *ch, struct uci_context *ctx,
			    struct uci_section *s)
{
	const char *name = "digit_map_base64";
	const char *val = uci_lookup_option_string(ctx, s, name);
	const char *def = desc_default(cfg_channel_options, name);

	if (!val) {
		digit_map_decode(def, ch->digit_map, sizeof(ch->digit_map));
		return;
	}

	if (digit_map_decode(val, ch->digit_map, sizeof(ch->digit_map)))
		return;

	ULOG_WARN("voip %s is invalid, using decoded default\n", name);
	if (!digit_map_decode(def, ch->digit_map, sizeof(ch->digit_map)))
		ch->digit_map[0] = '\0';
}

static int channel_index_get(struct uci_context *ctx, struct uci_section *s)
{
	const char *val = uci_lookup_option_string(ctx, s, "id");
	unsigned int num;
	const char *p;

	if (val)
		return uint_parse(val, &num) ? (int)num : -1;

	/* Deployed configs name the sections channel1..channelN while the id
	 * option is 0 based; fall back to the name when id is missing. */
	for (p = s->e.name; *p; p++) {
		if (*p >= '1' && *p <= '9')
			return uint_parse(p, &num) ? (int)num - 1 : -1;
	}

	return -1;
}

#define FIELD_TYPE struct cfg
static const struct cfg_field base_fields[] = {
	FIELD_DUMP(BOOL, "loaded", loaded),
	FIELD(BOOL, "enabled", enabled),
	FIELD_DUMP(UINT, "channels", channels),
	FIELD(STR, "local_interface", local_interface),
	/* The transport vocabulary is closed: acct_transport_map() accepts the
	 * three tokens of the schema row and nothing else, so an unknown token
	 * takes the schema default here with one warn instead of travelling to
	 * the account as text. The derive keeps its own unknown-token arm as
	 * defence in depth for the views it builds without this loader. */
	FIELD(ENUM_STR, "transport", transport),
	/* The protocol vocabulary is closed for the same reason: an unknown
	 * word takes the default with one warn, so cfg_protocol_of() sees one
	 * of the two words. */
	FIELD(ENUM_STR, "protocol", protocol),
	FIELD(ENUM_STR, "ui", ui),
	FIELD(BOOL, "srtp_enabled", srtp_enabled),
	/* Keep this read free-form, and the schema row says so: line/line.c
	 * accepts more codes than any list here could hold. */
	FIELD(STR, "region", region),
	FIELD(ENUM_UINT, "restrict_source", restrict_source),
	FIELD(UINT, "options_interval", options_interval),
	FIELD_LATE(BOOL, "keepalive_enabled", keepalive_enabled),
	FIELD(UINT, "keepalive_interval", keepalive_interval),
	FIELD(BOOL, "dns_cache_update_mode", dns_cache_update_mode),
	FIELD(BOOL, "dns_move_unregister", dns_move_unregister),
	FIELD(ENUM_IDX, "dns_srv_mode", dns_srv_mode),
	FIELD(UINT, "dns_linger", dns_linger),
	FIELD(BOOL, "mwi_subscribe", mwi_subscribe),
	FIELD(BOOL, "star_codes_local", star_codes_local),
	FIELD(BOOL, "rtcp_enabled", rtcp_enabled),
	FIELD(BOOL, "ipv6_enabled", ipv6_enabled),
	FIELD(UINT, "rtcp_interval", rtcp_interval),
	FIELD(STR, "alert_info_prefix", alert_info_prefix),
	/* tls_cert_username and tls_cert_password are the download credentials
	 * of cfg.h, and that header keeps both out of the dump; the archive
	 * location and the two switches are diagnostics and stay. */
	FIELD(STR, "tls_cert_path", tls_cert_path),
	FIELD_SECRET(STR, "tls_cert_username", tls_cert_username),
	FIELD_SECRET(STR, "tls_cert_password", tls_cert_password),
	FIELD(BOOL, "tls_server_verification_enabled",
	      tls_server_verification_enabled),
	FIELD(BOOL, "tls_keylog_enabled", tls_keylog_enabled),
	FIELD_LAST
};
#undef FIELD_TYPE

static void base_parse(struct cfg *cfg, struct uci_context *ctx, struct uci_section *s)
{
	const struct cfg_option_desc *tbl = cfg_base_options;
	const char *val = uci_lookup_option_string(ctx, s, "channels");
	unsigned int channels = 0;

	fields_parse(base_fields, ctx, s, tbl, false, cfg);

	if (val && !uint_parse(val, &channels)) {
		/* A channel count we cannot read is a provisioning fault. Voice
		 * stays off rather than running on a guessed line count. */
		ULOG_ERR("voip.base.channels '%s' is not a number, voice disabled\n",
			 val);
		cfg->enabled = false;
		cfg->channels = 0;
		return;
	}

	if (channels > CFG_CHANNEL_MAX) {
		ULOG_WARN("voip.base.channels %u exceeds %u, clamped\n", channels,
			  CFG_CHANNEL_MAX);
		channels = CFG_CHANNEL_MAX;
	}

	cfg->channels = channels;
}

/* The dump reports both the configured port count and the resolved one. */
#define FIELD_TYPE struct cfg_hal
static const struct cfg_field hal_fields[] = {
	FIELD(ENUM_STR, "tdm_backend", tdm_backend),
	FIELD(STR, "tdm_playback", tdm_playback),
	FIELD(STR, "tdm_capture", tdm_capture),
	FIELD(STR_NONE, "slic_plugin", slic_plugin),
	FIELD(STR, "fax_plugin", fax_plugin),
	FIELD(STR, "spidev", spidev),
	FIELD(STR, "gpio_reset", gpio_reset),
	FIELD(STR_NONE, "gpio_interrupt", gpio_interrupt),
	FIELD_ROW(UINT, "populated_ports", "populated_ports_cfg",
		  populated_ports_cfg),
	FIELD_DUMP(UINT, "populated_ports", populated_ports),
	FIELD(UINT, "channels", channels),
	FIELD(UINT, "prefill_periods", prefill_periods),
	FIELD(INT, "engine_cpu", engine_cpu),
	FIELD(INT, "engine_prio", engine_prio),
	FIELD(UINT, "pcm_offset_bits", pcm_offset_bits),
	FIELD(ENUM_UINT, "sample_rate", sample_rate),
	FIELD(INT, "handset_mic_gain", handset_mic_gain),
	FIELD(INT, "headset_mic_gain", headset_mic_gain),
	FIELD(INT, "speaker_mic_gain", speaker_mic_gain),
	FIELD(INT, "handset_out_gain", handset_out_gain),
	FIELD(INT, "headset_out_gain", headset_out_gain),
	FIELD(ENUM_IDX, "ring_sound", ring_sound),
	FIELD(UINT, "ring_volume", ring_volume),
	FIELD_LAST
};
#undef FIELD_TYPE

/*
 * voip.debug. One option, the same first-section-wins rule
 * the hal section carries, and nothing is validated here: trace.c owns the
 * domain vocabulary and warns about a name it does not know, so a typo costs
 * that one domain and not the list beside it.
 */
#define FIELD_TYPE struct cfg_debug
static const struct cfg_field debug_fields[] = {
	FIELD(STR, "trace", trace),
	FIELD_LAST
};
#undef FIELD_TYPE

/*
 * endpoint_pattern names an endpoint through two tokens, {line} and {host}.
 * Each can appear once at most, and a brace outside them has no meaning.
 */
static bool mgcp_pattern_valid(const char *val)
{
	unsigned int line = 0;
	unsigned int host = 0;
	const char *p;

	for (p = val; *p; p++) {
		if (*p == '}')
			return false;
		if (*p != '{')
			continue;

		if (!strncmp(p, "{line}", 6))
			line++;
		else if (!strncmp(p, "{host}", 6))
			host++;
		else
			return false;
		p += 5;
	}

	return *val && line <= 1 && host <= 1;
}

static bool mgcp_event_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '/';
}

/* A comma list of event names of 1 to 8 characters; empty is no event. */
static bool mgcp_events_valid(const char *val)
{
	size_t item = 0;
	const char *p;

	if (!*val)
		return true;

	for (p = val; *p; p++) {
		if (*p == ',') {
			if (!item)
				return false;
			item = 0;
		} else if (!mgcp_event_char(*p) || ++item > 8) {
			return false;
		}
	}

	return item > 0;
}

/* Max2 stays above Max1, as RFC 3435 4.3 asks. */
static unsigned int mgcp_max2_floor(const void *obj)
{
	return ((const struct cfg_mgcp *)obj)->max1 + 1;
}

static unsigned int mgcp_rto_max_floor(const void *obj)
{
	return ((const struct cfg_mgcp *)obj)->rto_init_ms;
}

static unsigned int mgcp_t_hist_floor(const void *obj)
{
	return ((const struct cfg_mgcp *)obj)->t_max_s;
}

/* A FLOOR row follows the row its floor reads. */
#define FIELD_TYPE struct cfg_mgcp
static const struct cfg_field mgcp_fields[] = {
	FIELD_CHECKED("call_agent", call_agent, NULL),
	FIELD(UINT, "call_agent_port", call_agent_port),
	FIELD(UINT, "local_port", local_port),
	FIELD_CHECKED("domain", domain, NULL),
	FIELD(BOOL, "bracketed_ip", bracketed_ip),
	FIELD_CHECKED("endpoint_pattern", endpoint_pattern, mgcp_pattern_valid),
	FIELD(UINT, "line_base", line_base),
	FIELD(ENUM_STR, "dialect", dialect),
	FIELD(UINT, "max1", max1),
	FIELD_FLOOR("max2", max2, mgcp_max2_floor, "max1 + 1"),
	FIELD(UINT, "rto_init_ms", rto_init_ms),
	FIELD_FLOOR("rto_max_ms", rto_max_ms, mgcp_rto_max_floor,
		    "rto_init_ms"),
	FIELD(UINT, "t_max_s", t_max_s),
	FIELD_FLOOR("t_hist_s", t_hist_s, mgcp_t_hist_floor, "t_max_s"),
	FIELD_CHECKED_NONE("persistent_events", persistent_events,
			   mgcp_events_valid),
	FIELD(ENUM_IDX, "quarantine_default", quarantine_default),
	FIELD(BOOL, "send_ptime", send_ptime),
	FIELD(ENUM_IDX, "reply_method", reply_method),
	FIELD(UINT, "signaling_dscp", signaling_dscp),
	FIELD(UINT, "restart_delay_max_s", restart_delay_max_s),
	FIELD_LAST
};
#undef FIELD_TYPE

/* With s NULL, it loads the defaults of an absent section. */
static void mgcp_parse(struct cfg_mgcp *mgcp, struct uci_context *ctx,
		       struct uci_section *s)
{
	fields_parse(mgcp_fields, ctx, s, cfg_mgcp_options, false, mgcp);
}

static void hal_populated_ports_resolve(struct cfg_hal *hal)
{
	const char *val;
	unsigned int ports;

	if (hal->populated_ports_cfg) {
		hal->populated_ports = hal->populated_ports_cfg;
		return;
	}

	val = getenv("PDB_NUM_POTS");
	if (uint_parse(val, &ports) && ports >= 1 && ports <= 8) {
		hal->populated_ports = ports;
		return;
	}

	ULOG_WARN("voip hal populated_ports: PDB_NUM_POTS is unset or invalid, "
		  "using fallback 1\n");
	hal->populated_ports = 1;
}

/* The dump nests these under "timers", keyed by the timer name alone. */
#define FIELD_TYPE struct cfg_sip_timers
static const struct cfg_field channel_timer_fields[] = {
	FIELD_ROW(UINT, "sip_timer_t1", "t1", t1),
	FIELD_ROW(UINT, "sip_timer_t2", "t2", t2),
	FIELD_ROW(UINT, "sip_timer_t4", "t4", t4),
	FIELD_ROW(UINT, "sip_timer_a", "a", a),
	FIELD_ROW(UINT, "sip_timer_b", "b", b),
	FIELD_ROW(UINT, "sip_timer_d", "d", d),
	FIELD_ROW(UINT, "sip_timer_e", "e", e),
	FIELD_ROW(UINT, "sip_timer_f", "f", f),
	FIELD_ROW(UINT, "sip_timer_g", "g", g),
	FIELD_ROW(UINT, "sip_timer_h", "h", h),
	FIELD_ROW(UINT, "sip_timer_i", "i", i),
	FIELD_ROW(UINT, "sip_timer_j", "j", j),
	FIELD_ROW(UINT, "sip_timer_k", "k", k),
	FIELD_LAST
};
#undef FIELD_TYPE

/*
 * The options of a channel section. Each table is a run of options that the
 * loader reads and the dump reports in the same order. Hand code between the
 * runs reads sip_dtmf_mode, digit_map_base64, local_conf_enabled and
 * default_packetization_rate. The dump reports the canceller run before
 * local_conf_enabled and the media run, and the loader reads it after them.
 */
#define FIELD_TYPE struct cfg_channel
static const struct cfg_field channel_sip_fields[] = {
	FIELD_DUMP(UINT, "id", id),
	FIELD(BOOL, "enabled", enabled),
	FIELD(STR, "sip_username", sip_username),
	FIELD(STR, "sip_auth_username", sip_auth_username),
	FIELD(STR, "sip_callerid_name", sip_callerid_name),
	FIELD_SECRET(STR, "sip_secret", sip_secret),
	FIELD(STR, "sip_proxy", sip_proxy),
	FIELD(STR, "sip_outbound_proxy", sip_outbound_proxy),
	FIELD(STR, "sip_domain", sip_domain),
	FIELD(STR, "sip_registrar", sip_registrar),
	FIELD(STR, "sip_realm", sip_realm),
	/* Channel transport and local_interface stay empty when the section
	 * names neither, so the account falls back to the base value. Neither
	 * row carries a schema default. An unknown transport token reads as
	 * empty too, because an inherited transport is what a rejected token
	 * must leave behind. */
	FIELD(ENUM_STR, "transport", transport),
	FIELD(STR, "local_interface", local_interface),
	FIELD_SET(PORT_SET, "sip_port", sip_port),
	FIELD(UINT0, "sip_outbound_proxy_port", sip_outbound_proxy_port),
	FIELD(UINT0, "sip_registrar_port", sip_registrar_port),
	FIELD_ALIAS(UINT, "sip_registration_expiry",
		    "sip_registration_exp_time", sip_registration_expiry),
	FIELD_ALIAS(UINT, "sip_register_retry",
		    "sip_rereg_delta_exp_time", sip_register_retry),
	FIELD(UINT, "sip_local_port", sip_local_port),
	FIELD(UINT, "sip_tos", sip_tos),
	FIELD(UINT, "options_interval", options_interval),
	FIELD_LAST
};

static const struct cfg_field channel_session_fields[] = {
	FIELD(ENUM_IDX, "sip_flash_relay", sip_flash_relay),
	FIELD(UINT, "sip_session_max_expires", session_max_expires),
	FIELD(UINT, "sip_session_min_interval", session_min_interval),
	FIELD(ENUM_IDX, "sip_session_refresher", session_refresher),
	FIELD(UINT, "sip_timer_c", timer_c_ms),
	FIELD_LAST
};

static const struct cfg_field channel_dialplan_fields[] = {
	FIELD_DUMP(STR, "digit_map", digit_map),
	FIELD(ENUM_UINT, "digit_esc_enabled", digit_esc),
	FIELD(UINT, "start_digit_timer", start_digit_s),
	FIELD(UINT, "interdigit_timeout", interdigit_s),
	FIELD_ALIAS(UINT, "interdigit_timeout_long", "interdigit_long",
		    interdigit_long_s),
	FIELD(UINT, "roh_timer_sec", roh_s),
	FIELD(UINT, "busy_tone_timer_sec", busy_tone_s),
	FIELD(UINT, "flash_service_timer_sec", flash_svc_s),
	FIELD_LAST
};

#define FIELD_RING(i) \
	FIELD(STR, "distinctive_ring_" #i, distinctive_ring[i]), \
	FIELD_FLAGGED(UINT_SET, "distinctive_ring_def_" #i, NULL, \
		      distinctive_ring_def[i], distinctive_ring_def_set[i])

_Static_assert(CFG_DISTINCTIVE_RING_MAX == 10,
	       "channel_call_fields must list every distinctive ring");

static const struct cfg_field channel_call_fields[] = {
	FIELD(BOOL, "call_waiting_enabled", call_waiting_enabled),
	FIELD_SET(BOOL_SET, "call_waiting_local", call_waiting_local),
	FIELD_ALIAS(CODE, "call_waiting_enable_exten",
		    "cancel_cw_off_exten", cw_enable_code),
	FIELD_ALIAS(CODE, "call_waiting_disable_exten",
		    "cancel_cw_on_exten", cw_disable_code),
	FIELD(CODE, "call_waiting_query_exten", cw_query_code),
	FIELD(BOOL, "cid_remote_enable", cid_remote_enable),
	FIELD(BOOL, "cid_outgoing_enable", cid_outgoing_enable),
	FIELD_SET(BOOL_SET, "cid_delivery_local", cid_delivery_local),
	FIELD(CODE, "cid_enable_exten", cid_enable_exten),
	FIELD(CODE, "cid_disable_exten", cid_disable_exten),
	FIELD(CODE, "cid_single_call_enable_exten",
	      cid_single_call_enable_exten),
	FIELD(CODE, "cid_single_call_disable_exten",
	      cid_single_call_disable_exten),
	FIELD(BOOL, "call_fwd_local", call_fwd_local),
	FIELD(ENUM_UINT, "call_fwd_local_mode", call_fwd_local_mode),
	FIELD(STR, "call_fwd_uncond_uri", call_fwd_uncond_uri),
	FIELD(STR, "call_fwd_busy_uri", call_fwd_busy_uri),
	FIELD(STR, "call_fwd_no_answer_uri", call_fwd_no_answer_uri),
	FIELD(UINT, "call_fwd_no_answer_timeout", call_fwd_no_answer_timeout),
	FIELD(CODE, "call_fwd_uncond_exten", call_fwd_uncond_exten),
	FIELD(CODE, "call_fwd_uncond_cancel_exten",
	      call_fwd_uncond_cancel_exten),
	FIELD(CODE, "call_fwd_uncond_query_exten", call_fwd_uncond_query_exten),
	FIELD(CODE, "call_fwd_busy_exten", call_fwd_busy_exten),
	FIELD(CODE, "call_fwd_busy_cancel_exten", call_fwd_busy_cancel_exten),
	FIELD(CODE, "call_fwd_busy_query_exten", call_fwd_busy_query_exten),
	FIELD(CODE, "call_fwd_no_answer_exten", call_fwd_no_answer_exten),
	FIELD(CODE, "call_fwd_no_answer_cancel_exten",
	      call_fwd_no_answer_cancel_exten),
	FIELD(CODE, "call_fwd_no_answer_query_exten",
	      call_fwd_no_answer_query_exten),
	FIELD(CODE, "call_fwd_unreachable_exten", call_fwd_unreachable_exten),
	FIELD(CODE, "call_fwd_unreachable_cancel_exten",
	      call_fwd_unreachable_cancel_exten),
	FIELD(CODE, "call_fwd_all_exten", call_fwd_all_exten),
	FIELD(CODE, "call_fwd_all_cancel_exten", call_fwd_all_cancel_exten),
	FIELD(CODE, "blind_transfer_exten", blind_transfer_exten),
	FIELD(CODE, "attended_transfer_exten", attended_transfer_exten),
	FIELD(CODE, "consult_transfer_exten", consult_transfer_exten),
	FIELD(BOOL, "do_not_disturb_enabled", do_not_disturb_enabled),
	FIELD(CODE, "do_not_disturb_on_exten", do_not_disturb_on_exten),
	FIELD(CODE, "do_not_disturb_off_exten", do_not_disturb_off_exten),
	FIELD(BOOL, "distinctive_ring_enable", distinctive_ring_enable),
	FIELD_RING(0), FIELD_RING(1), FIELD_RING(2), FIELD_RING(3),
	FIELD_RING(4), FIELD_RING(5), FIELD_RING(6), FIELD_RING(7),
	FIELD_RING(8), FIELD_RING(9),
	FIELD(BOOL, "warmline_enable", warmline_enable),
	FIELD(STR, "warmline_number", warmline_number),
	FIELD(UINT, "warmline_timeout", warmline_timeout_s),
	/* Forward disconnect. line/line.c derives the feature from the pair
	 * and owns both the reverse form and the remove form. */
	FIELD(UINT, "forward_disconnect_delay", forward_disconnect_delay_ms),
	FIELD(ENUM_IDX, "forward_disconnect_battery",
	      forward_disconnect_battery),
	FIELD(BOOL, "amwi", amwi),
	FIELD(BOOL, "vmwi", vmwi),
	FIELD_LAST
};
#undef FIELD_RING

/*
 * The echo canceller and the half decibel loss plan (AU/NZ). Both are channel
 * config values the line pushes to the DSP. The loader carries the vocabulary
 * only; line/line.c owns which combination reaches the canceller and which
 * gain a loss lands on.
 */
static const struct cfg_field channel_ec_fields[] = {
	FIELD_SET(BOOL_SET, "lec_control", lec_control),
	FIELD(BOOL, "lec_enable", lec_enable),
	FIELD_SET(BOOL_SET, "lec_nlp", lec_nlp),
	FIELD(BOOL, "lec_tone_latch", lec_tone_latch),
	FIELD(BOOL, "lec_tone_force_disable", lec_tone_force_disable),
	FIELD(BOOL, "half_db_rx_loss", half_db_rx_loss),
	FIELD(BOOL, "half_db_tx_loss", half_db_tx_loss),
	/* the SLIC analog pair rides beside the loss plan: whole dB, and
	 * line/line.c doubles it into the half-dB HAL pair */
	FIELD(INT, "ps_tx_gain", ps_tx_gain),
	FIELD(INT, "ps_rx_gain", ps_rx_gain),
	FIELD_LAST
};

/* default_packetization_rate clamps with a warning before the next option
 * reads, so it stays hand code ahead of this table. */
static const struct cfg_field channel_media_fields[] = {
	FIELD(BOOL, "cn_enable", cn_enable),
	FIELD(ENUM_UINT, "g711a_ptype", g711a_ptype),
	FIELD(ENUM_UINT, "g729a_ptype", g729a_ptype),
	FIELD(ENUM_UINT, "g726_ptype", g726_ptype),
	FIELD(ENUM_UINT, "g722_ptype", g722_ptype),
	FIELD(ENUM_UINT, "opus_ptype", opus_ptype),
	FIELD(BOOL, "fax_t38_enable", fax_t38_enable),
	FIELD(BOOL, "fax_upspeed_follow", fax_upspeed_follow),
	FIELD(BOOL, "prack", prack),
	FIELD(BOOL, "early_media_enabled", early_media_enabled),
	FIELD(UINT, "jitter_target", jitter_target),
	FIELD(UINT, "jitter_buffer_max", jitter_max),
	FIELD(UINT, "rtp_tos", rtp_tos),
	FIELD(UINT, "rtcp_tos", rtcp_tos),
	FIELD(UINT, "rtp_local_port_min", rtp_port_min),
	FIELD(UINT, "rtp_local_port_max", rtp_port_max),
	FIELD(STR, "rtcp_cname", rtcp_cname),
	FIELD_LAST
};
#undef FIELD_TYPE

static void channel_session_parse(struct cfg_channel *ch, struct uci_context *ctx,
				  struct uci_section *s)
{
	const struct cfg_option_desc *tbl = cfg_channel_options;
	const char *val;

	val = opt_enum(ctx, s, tbl, "sip_dtmf_mode");
	if (val && !strcmp(val, "rfc2833"))
		ch->dtmf_mode = CFG_DTMF_RFC2833;
	else if (val && (!strcmp(val, "sipinfo") || !strcmp(val, "sip_info") ||
			 !strcmp(val, "info")))
		ch->dtmf_mode = CFG_DTMF_INFO;
	else
		ch->dtmf_mode = CFG_DTMF_INBAND;

	fields_parse(channel_session_fields, ctx, s, tbl, false, ch);
}

/*
 * A map that decodes but does not compile reaches line/line.c, which falls back
 * to the default map for each line of each reload. The report belongs here
 * instead: a config load reads the section once and knows its name. This
 * follows the transport token rule in base_parse(), warn in the loader and
 * keep the fallback in the derive.
 */
static void digit_map_check(const struct cfg_channel *ch, const char *section)
{
	struct dialplan dp;

	if (dialplan_compile(ch->digit_map, &dp))
		return;

	ULOG_WARN("voip.%s digit_map_base64 decodes to '%s', which is not a "
		  "valid digit map, using '%s'\n", section, ch->digit_map,
		  DIALPLAN_DEFAULT_MAP);
}

static void channel_dialplan_parse(struct cfg_channel *ch, struct uci_context *ctx,
				   struct uci_section *s)
{
	digit_map_parse(ch, ctx, s);
	digit_map_check(ch, s->e.name);
	fields_parse(channel_dialplan_fields, ctx, s, cfg_channel_options,
		     false, ch);
	if (ch->interdigit_s > 100)
		ch->interdigit_s /= 1000;
}

static void channel_call_parse(struct cfg_channel *ch, struct uci_context *ctx,
			       struct uci_section *s, bool codes)
{
	const struct cfg_option_desc *tbl = cfg_channel_options;

	fields_parse(channel_call_fields, ctx, s, tbl, codes, ch);
	ch->local_conf_enabled = opt_enum_uint_bool_compat(ctx, s, tbl,
							   "local_conf_enabled");
}

static void channel_media_parse(struct cfg_channel *ch, struct uci_context *ctx,
				struct uci_section *s)
{
	const struct cfg_option_desc *tbl = cfg_channel_options;

	ch->ptime_ms = opt_enum_uint(ctx, s, tbl, "default_packetization_rate");
	if (ch->ptime_ms == 40) {
		ULOG_WARN("voip default_packetization_rate 40 unsupported by DSP, clamped to 30\n");
		ch->ptime_ms = 30;
	}

	fields_parse(channel_media_fields, ctx, s, tbl, false, ch);
	if (!!ch->rtp_port_min != !!ch->rtp_port_max ||
	    (ch->rtp_port_min && ch->rtp_port_min > ch->rtp_port_max)) {
		ULOG_WARN("voip RTP port range %u..%u is incoherent, using defaults\n",
			  ch->rtp_port_min, ch->rtp_port_max);
		ch->rtp_port_min = 0;
		ch->rtp_port_max = 0;
	}
}

static void channel_parse(struct cfg *cfg, struct uci_context *ctx, struct uci_section *s)
{
	const struct cfg_option_desc *tbl = cfg_channel_options;
	struct cfg_channel *ch;
	int index = channel_index_get(ctx, s);

	if (index < 0 || index >= CFG_CHANNEL_MAX) {
		ULOG_WARN("voip.%s: channel index %d out of range, ignored\n",
			  s->e.name, index);
		return;
	}

	ch = &cfg->channel[index];
	ch->present = true;
	ch->id = (unsigned int)index;
	fields_parse(channel_sip_fields, ctx, s, tbl, false, ch);
	channel_session_parse(ch, ctx, s);
	channel_dialplan_parse(ch, ctx, s);
	channel_call_parse(ch, ctx, s, cfg->star_codes_local);
	channel_media_parse(ch, ctx, s);
	fields_parse(channel_ec_fields, ctx, s, tbl, false, ch);
	fields_parse(channel_timer_fields, ctx, s, tbl, false, &ch->timers);
}

static bool codec_precedes(const struct cfg_codec *candidate,
			   const struct cfg_codec *current)
{
	if (candidate->priority != current->priority)
		return candidate->priority < current->priority;

	return candidate->default_priority < current->default_priority;
}

#define FIELD_TYPE struct cfg_codec
static const struct cfg_field codec_fields[] = {
	FIELD(BOOL, "enabled", enabled),
	FIELD(BOOL, "silence_suppression", silence_suppression),
	FIELD(ENUM_UINT, "tr104_id", tr104_id),
	FIELD(PRIORITY, "priority", priority),
	FIELD(PRIORITY, "default_priority", default_priority),
	FIELD(UINT, "channel", channel),
	FIELD(UINT, "bitrate", bitrate),
	FIELD(BOOL, "fec", fec),
	FIELD(UINT, "complexity", complexity),
	FIELD_LAST
};
#undef FIELD_TYPE

static void codec_parse(struct cfg *cfg, struct uci_context *ctx,
			struct uci_section *s)
{
	struct cfg_codec candidate = { 0 };
	unsigned int pos;
	unsigned int i;

	fields_parse(codec_fields, ctx, s, cfg_codec_options, false,
		     &candidate);

	for (pos = 0; pos < cfg->codecs; pos++) {
		if (codec_precedes(&candidate, &cfg->codec[pos]))
			break;
	}

	if (cfg->codecs == CFG_CODEC_MAX) {
		ULOG_WARN("voip codec section limit %u exceeded, dropping lowest priority\n",
			  CFG_CODEC_MAX);
		if (pos == CFG_CODEC_MAX)
			return;
	} else {
		cfg->codecs++;
	}

	for (i = cfg->codecs - 1; i > pos; i--) {
		cfg->codec[i] = cfg->codec[i - 1];
	}

	cfg->codec[pos] = candidate;
}

static void package_parse(struct cfg *cfg, struct uci_context *ctx, struct uci_package *pkg)
{
	struct uci_element *e;
	struct uci_section *hal = NULL;
	struct uci_section *debug = NULL;
	struct uci_section *mgcp = NULL;
	struct uci_section *s;

	uci_foreach_element(&pkg->sections, e) {
		s = uci_to_section(e);
		if (!strcmp(s->type, CFG_SECTION_BASE)) {
			base_parse(cfg, ctx, s);
		} else if (!strcmp(s->type, CFG_SECTION_HAL)) {
			if (hal) {
				ULOG_WARN("voip.%s: additional hal section ignored\n",
					  s->e.name);
				continue;
			}

			fields_parse(hal_fields, ctx, s, cfg_hal_options,
				     false, &cfg->hal);
			hal = s;
		} else if (!strcmp(s->type, CFG_SECTION_DEBUG)) {
			if (debug) {
				ULOG_WARN("voip.%s: additional debug section ignored\n",
					  s->e.name);
				continue;
			}

			fields_parse(debug_fields, ctx, s, cfg_debug_options,
				     false, &cfg->debug);
			debug = s;
		} else if (!strcmp(s->type, CFG_SECTION_MGCP)) {
			if (mgcp) {
				ULOG_WARN("voip.%s: additional mgcp section "
					  "ignored\n", s->e.name);
				continue;
			}

			mgcp_parse(&cfg->mgcp, ctx, s);
			mgcp = s;
		}
	}

	uci_foreach_element(&pkg->sections, e) {
		s = uci_to_section(e);
		if (!strcmp(s->type, CFG_SECTION_CHANNEL))
			channel_parse(cfg, ctx, s);
		else if (!strcmp(s->type, CFG_SECTION_CODEC))
			codec_parse(cfg, ctx, s);
	}
}

static void cfg_defaults(struct cfg *cfg)
{
	fields_parse(base_fields, NULL, NULL, cfg_base_options, false, cfg);
	fields_parse(hal_fields, NULL, NULL, cfg_hal_options, false, &cfg->hal);
	cfg->hal.populated_ports = 1;
	fields_parse(debug_fields, NULL, NULL, cfg_debug_options, false,
		     &cfg->debug);
	mgcp_parse(&cfg->mgcp, NULL, NULL);
}

struct cfg *cfg_load(void)
{
	struct uci_context *ctx;
	struct uci_package *pkg = NULL;
	struct cfg *cfg;
	const char *confdir;

	cfg = calloc(1, sizeof(*cfg));
	if (!cfg)
		return NULL;

	cfg_defaults(cfg);

	ctx = uci_alloc_context();
	if (!ctx) {
		ULOG_ERR("uci context allocation failed\n");
		goto resolve;
	}

	/* The x86 CI build has no /etc/config, so the variable names the
	 * directory a host test writes. */
	confdir = getenv("UVOICE_UCI_CONFDIR");
	if (confdir)
		uci_set_confdir(ctx, confdir);

	if (uci_load(ctx, CFG_PACKAGE, &pkg) != UCI_OK || !pkg) {
		ULOG_WARN("config %s not loaded, voice stays idle\n", CFG_PACKAGE);
		uci_free_context(ctx);
		goto resolve;
	}

	package_parse(cfg, ctx, pkg);
	cfg->loaded = true;

	uci_unload(ctx, pkg);
	uci_free_context(ctx);

	TRACE(TRACE_FSM, "config loaded: enabled=%d channels=%u iface=%s",
	      cfg->enabled, cfg->channels, cfg->local_interface);

resolve:
	hal_populated_ports_resolve(&cfg->hal);

	return cfg;
}

void cfg_free(struct cfg *cfg)
{
	free(cfg);
}

enum cfg_protocol cfg_protocol_of(const struct cfg *cfg)
{
	if (!strcmp(cfg->protocol, "mgcp"))
		return CFG_PROTO_MGCP;

	return CFG_PROTO_SIP;
}

bool cfg_ui_desk(const struct cfg *cfg)
{
	return !strcmp(cfg->ui, "desk");
}

const char *const *cfg_ring_sound_names(void)
{
	return cfg_ring_sound_vals;
}

int cfg_hal_range(const char *name, int *min, int *max)
{
	const struct cfg_option_desc *o;

	for (o = cfg_hal_options; o->name; o++) {
		if (strcmp(o->name, name))
			continue;

		*min = o->min;
		*max = o->max;
		return 0;
	}

	return -ENOENT;
}

/* cfg_dtmf_mode_vals (schema.h) carries five uci aliases for three values,
 * so the dump keeps its own table indexed by the enum. */
static const char *const cfg_dtmf_mode_words[] = {
	"inband", "rfc2833", "info", NULL
};

/*
 * Each assert pins one table's length to its enum's last member.
 * The runtime dump cases in test_cfg pin the word order, not this assert.
 */
_Static_assert(ARRAY_SIZE(cfg_dtmf_mode_words) - 1 == CFG_DTMF_INFO + 1,
	       "cfg_dtmf_mode_words must list every enum cfg_dtmf_mode word");
_Static_assert(ARRAY_SIZE(cfg_flash_relay_vals) - 1 ==
	       CFG_FLASH_RELAY_INFO + 1,
	       "cfg_flash_relay_vals must list every enum cfg_flash_relay "
	       "word");
_Static_assert(ARRAY_SIZE(cfg_session_refresher_vals) - 1 ==
	       CFG_REFRESH_UNSPEC + 1,
	       "cfg_session_refresher_vals must list every enum "
	       "cfg_refresher word");
_Static_assert(ARRAY_SIZE(cfg_fwd_disc_battery_vals) - 1 ==
	       CFG_FWD_DISC_BATT_REVERSE + 1,
	       "cfg_fwd_disc_battery_vals must list every enum "
	       "cfg_fwd_disc_battery word");
_Static_assert(ARRAY_SIZE(cfg_dns_srv_mode_vals) - 1 == CFG_DNS_SRV_NEVER + 1,
	       "cfg_dns_srv_mode_vals must list every enum cfg_dns_srv_mode "
	       "word");
_Static_assert(ARRAY_SIZE(cfg_mgcp_dialect_vals) - 1 == CFG_MGCP_NCS + 1,
	       "cfg_mgcp_dialect_vals must list every enum cfg_mgcp_dialect "
	       "word");
_Static_assert(ARRAY_SIZE(cfg_mgcp_quarantine_vals) - 1 ==
	       CFG_MGCP_QUARANTINE_LOOP + 1,
	       "cfg_mgcp_quarantine_vals must list every enum "
	       "cfg_mgcp_quarantine word");
_Static_assert(ARRAY_SIZE(cfg_mgcp_reply_vals) - 1 ==
	       CFG_MGCP_REPLY_MGC_PORT + 1,
	       "cfg_mgcp_reply_vals must list every enum cfg_mgcp_reply word");

static void channel_timers_dump(const struct cfg_sip_timers *timers,
				struct blob_buf *b)
{
	fields_dump(channel_timer_fields, cfg_channel_options, timers, b);
}

static void channel_dump(const struct cfg_channel *ch, struct blob_buf *b)
{
	const struct cfg_option_desc *tbl = cfg_channel_options;
	void *timers;

	fields_dump(channel_sip_fields, tbl, ch, b);
	blobmsg_add_string(b, "sip_dtmf_mode",
			   ENUM_WORD(cfg_dtmf_mode_words, ch->dtmf_mode));
	fields_dump(channel_session_fields, tbl, ch, b);
	fields_dump(channel_dialplan_fields, tbl, ch, b);
	fields_dump(channel_call_fields, tbl, ch, b);
	fields_dump(channel_ec_fields, tbl, ch, b);
	blobmsg_add_u32(b, "local_conf_enabled", ch->local_conf_enabled);
	blobmsg_add_u32(b, "default_packetization_rate", ch->ptime_ms);
	fields_dump(channel_media_fields, tbl, ch, b);
	timers = blobmsg_open_table(b, "timers");
	channel_timers_dump(&ch->timers, b);
	blobmsg_close_table(b, timers);
}

static void hal_dump(const struct cfg_hal *hal, struct blob_buf *b)
{
	fields_dump(hal_fields, cfg_hal_options, hal, b);
}

static void codec_dump(const struct cfg_codec *codec, struct blob_buf *b)
{
	fields_dump(codec_fields, cfg_codec_options, codec, b);
}

void cfg_dump(const struct cfg *cfg, struct blob_buf *b)
{
	const struct cfg_channel *ch;
	const struct cfg_codec *codec;
	void *chans, *codecs, *debug, *entry, *hal, *mgcp;
	unsigned int i;

	fields_dump(base_fields, cfg_base_options, cfg, b);
	hal = blobmsg_open_table(b, "hal");
	hal_dump(&cfg->hal, b);
	blobmsg_close_table(b, hal);
	debug = blobmsg_open_table(b, "debug");
	fields_dump(debug_fields, cfg_debug_options, &cfg->debug, b);
	blobmsg_close_table(b, debug);
	mgcp = blobmsg_open_table(b, "mgcp");
	fields_dump(mgcp_fields, cfg_mgcp_options, &cfg->mgcp, b);
	blobmsg_close_table(b, mgcp);

	chans = blobmsg_open_array(b, "channel");
	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		ch = &cfg->channel[i];
		if (!ch->present)
			continue;

		entry = blobmsg_open_table(b, NULL);
		channel_dump(ch, b);
		blobmsg_close_table(b, entry);
	}
	blobmsg_close_array(b, chans);

	codecs = blobmsg_open_array(b, "codec");
	for (i = 0; i < cfg->codecs; i++) {
		codec = &cfg->codec[i];
		entry = blobmsg_open_table(b, NULL);
		codec_dump(codec, b);
		blobmsg_close_table(b, entry);
	}
	blobmsg_close_array(b, codecs);
}
