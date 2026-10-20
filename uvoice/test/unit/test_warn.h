// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The warning capture of the unit suites. Some fault rules include "warns
 * once", and the code under test keeps no counter to read, so a suite reads
 * the warning where it lands. The capture points stderr at a temporary file
 * and counts the lines that hold a needle.
 *
 * ULOG picks its channel from isatty(1) unless a caller chooses one, so under
 * ctest it goes to syslog and never reaches this process. The capture selects
 * ULOG_STDIO for its length. The first count puts back stderr and the
 * automatic channel selection. ulog_open(-1, -1, NULL) restores that
 * selection, which is stdio on a tty and syslog otherwise. A fixed
 * ULOG_SYSLOG would hide the later warnings from a terminal.
 *
 * A suite that defines WARN_CAPTURE_ULOG_OWN before it includes this header
 * gets its own ulog() instead. It writes the priority in front of the message
 * to stderr, and only inside a capture. libubox renders the message text and
 * drops the priority, so only that override lets warn_capture_count_at() read
 * the level back.
 *
 * The captured text outlives the count, so a case asks the same capture about
 * as many needles as it has. A count that closed the capture would answer 0
 * to every question after the first, and an expected 0 could not fail. The
 * capture ends at the next warn_capture_begin() or at warn_capture_end().
 * dup2 shares the file offset with stderr, so the rewind carries the first
 * count too.
 */
#ifndef UVOICE_TEST_WARN_H
#define UVOICE_TEST_WARN_H

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <libubox/ulog.h>

static FILE *warn_file;
static int warn_stderr_fd = -1;

#ifdef WARN_CAPTURE_ULOG_OWN
#include <stdarg.h>

void ulog(int priority, const char *fmt, ...)
{
	va_list ap;

	if (warn_stderr_fd < 0)
		return;

	fprintf(stderr, "prio=%d ", priority);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
#endif

static inline void warn_stderr_restore(void)
{
	if (warn_stderr_fd < 0)
		return;

	fflush(stderr);
#ifndef WARN_CAPTURE_ULOG_OWN
	ulog_open(-1, -1, NULL);
#endif
	dup2(warn_stderr_fd, STDERR_FILENO);
	close(warn_stderr_fd);
	warn_stderr_fd = -1;
}

static inline void warn_capture_end(void)
{
	warn_stderr_restore();

	if (!warn_file)
		return;

	fclose(warn_file);
	warn_file = NULL;
}

static inline void warn_capture_begin(void)
{
	warn_capture_end();

	warn_file = tmpfile();
	if (!warn_file)
		return;

	fflush(stderr);
	warn_stderr_fd = dup(STDERR_FILENO);
	dup2(fileno(warn_file), STDERR_FILENO);
#ifndef WARN_CAPTURE_ULOG_OWN
	ulog_open(ULOG_STDIO, LOG_DAEMON, NULL);
#endif
}

static inline unsigned int warn_capture_count(const char *needle)
{
	char line[512];
	unsigned int hits = 0;

	if (!warn_file)
		return 0;

	warn_stderr_restore();

	rewind(warn_file);
	while (fgets(line, sizeof(line), warn_file)) {
		if (strstr(line, needle))
			hits++;
	}

	return hits;
}

static inline void warn_capture_print(void)
{
	char line[512];

	warn_stderr_restore();
	if (!warn_file)
		return;

	rewind(warn_file);
	while (fgets(line, sizeof(line), warn_file))
		printf("    %s", line);
}

#ifdef WARN_CAPTURE_ULOG_OWN
static inline unsigned int warn_capture_count_at(int priority,
						 const char *needle)
{
	char want[192];

	snprintf(want, sizeof(want), "prio=%d %s", priority, needle);

	return warn_capture_count(want);
}
#endif

#endif
