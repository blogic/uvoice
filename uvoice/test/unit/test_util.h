// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Test harness for the uvoice unit tests. No framework: one line per case on
 * stdout, a failing check prints file, line and expression, and the process
 * exits non zero when anything failed.
 * A counted skip reports its reason and must never hide a failed check.
 */
#ifndef UVOICE_TEST_UTIL_H
#define UVOICE_TEST_UTIL_H

#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "proto/shared/msg.h"

static unsigned int tu_total;
static unsigned int tu_failed;
static unsigned int tu_skipped;
static bool tu_open;
static bool tu_bad;
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

/* Socket creation alone does not prove that the host can bind IPv6 loopback.
 * Report the failed step so a suite can explain why it skips a case.
 * This function does not preserve errno. */
static inline bool tu_v6_loopback(char *why, size_t wlen)
{
	struct sockaddr_in6 addr = {
		.sin6_family = AF_INET6,
		.sin6_port = 0,
		.sin6_addr = IN6ADDR_LOOPBACK_INIT,
	};
	int fd;

	if (wlen)
		why[0] = '\0';
	fd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	if (fd < 0) {
		snprintf(why, wlen, "socket(AF_INET6): %s", strerror(errno));
		return false;
	}
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		snprintf(why, wlen, "bind(::1): %s", strerror(errno));
		close(fd);
		return false;
	}

	close(fd);
	return true;
}

/* An empty scratch file under $TMPDIR, or /tmp when it is unset. The pid and
 * the mkstemp() suffix give each run its own name, so two build trees can run
 * one suite at once. The caller removes the file. On a failure errno says why:
 * ENAMETOOLONG when the name does not fit the buffer. */
static inline bool tu_tmp_file_make(char *path, size_t plen, const char *name)
{
	const char *tmp = getenv("TMPDIR");
	int n;
	int fd;

	if (!tmp || !*tmp)
		tmp = "/tmp";

	n = snprintf(path, plen, "%s/%s-%ld-XXXXXX", tmp, name, (long)getpid());
	if (n < 0)
		return false;
	if ((size_t)n >= plen) {
		errno = ENAMETOOLONG;
		return false;
	}

	fd = mkstemp(path);
	if (fd < 0)
		return false;

	close(fd);
	return true;
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

static inline void tu_check_str(struct sip_str s, const char *lit, const char *expr,
				const char *file, int line)
{
	if (sip_str_eq(s, lit))
		return;

	tu_failure();
	printf("    fail %s:%d: %s (got \"%.*s\", want \"%s\")\n", file, line, expr,
	       (int)s.len, s.p ? s.p : "", lit);
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
#define CHECK_STR(s, lit)	tu_check_str((s), (lit), #s, __FILE__, __LINE__)
#define CHECK_UINT(got, want)	tu_check_uint((uint64_t)(got), (uint64_t)(want), #got, \
					      __FILE__, __LINE__)

/* A suite check closes the open case and belongs to no case. It prints
 * nothing when it holds, and its failure fails the suite (tu_failure()). A
 * precondition of a group of cases uses it, so a setup step is not counted
 * as a case. The checks after it belong to no case until the next CASE(). */
#define SUITE_CHECK(expr)	do { tu_case_end(); CHECK(expr); } while (0)
#define SUITE_CHECK_UINT(got, want) \
	do { tu_case_end(); CHECK_UINT(got, want); } while (0)

#endif
