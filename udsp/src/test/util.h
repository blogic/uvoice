// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Test harness for libudsp unit tests. Same shape as uvoice test_util.h: one
 * TAP line per case, a failing check prints file, line and expression, the
 * process exits non-zero when anything failed. No framework, libc only.
 * A counted skip reports its reason and must never hide a failed check.
 */
#ifndef UDSP_TEST_UTIL_H
#define UDSP_TEST_UTIL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static unsigned tu_total, tu_failed, tu_skipped;
static bool tu_open, tu_bad;
static char tu_name[160];

static inline void tu_case_end(void)
{
	if (!tu_open)
		return;
	tu_total++;
	if (tu_bad) {
		tu_failed++;
		printf("not ok %u - %s\n", tu_total, tu_name);
	} else {
		printf("ok %u - %s\n", tu_total, tu_name);
	}
	tu_open = false;
}

/* A skip closes and counts an open case, but a failed check takes priority.
 * It closes the case, so a later failing check belongs to no case and fails
 * the suite. */
static inline void tu_skip(const char *reason)
{
	if (!tu_open)
		return;
	if (tu_bad) {
		tu_case_end();
		return;
	}
	tu_total++;
	tu_skipped++;
	printf("ok %u - %s # SKIP %s\n", tu_total, tu_name, reason);
	tu_open = false;
}

static inline void tu_case(const char *name)
{
	tu_case_end();
	snprintf(tu_name, sizeof(tu_name), "%s", name);
	tu_open = true;
	tu_bad = false;
}

static inline void tu_failure(void)
{
	if (tu_open)
		tu_bad = true;
	else
		tu_failed++;
}

static inline void tu_check(bool ok, const char *expr, const char *file, int line)
{
	if (ok)
		return;
	tu_failure();
	printf("    fail %s:%d: %s\n", file, line, expr);
}

static inline void tu_check_uint(uint64_t got, uint64_t want, const char *expr,
				 const char *file, int line)
{
	if (got == want)
		return;
	tu_failure();
	printf("    fail %s:%d: %s (got %llu, want %llu)\n", file, line, expr,
	       (unsigned long long)got, (unsigned long long)want);
}

static inline int tu_summary(const char *suite)
{
	tu_case_end();
	printf("# %s: %u cases, %u failed", suite, tu_total, tu_failed);
	if (tu_skipped)
		printf(", %u skipped", tu_skipped);
	printf("\n");
	return tu_failed ? 1 : 0;
}

#define CASE(name)		tu_case(name)
#define CHECK(expr)		tu_check((expr) ? true : false, #expr, __FILE__, __LINE__)
#define CHECK_UINT(g, w)	tu_check_uint((uint64_t)(g), (uint64_t)(w), #g, \
					      __FILE__, __LINE__)

#endif
