// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The region file rewriter of test_core.c and test_cli.c. A case reads a
 * shipped region file and replaces text in it to make a variant region. A
 * replacement can be shorter or longer than the text it replaces. The buffer
 * holds REGION_EDIT_GROW bytes past the largest region file, which is the
 * room for a longer replacement.
 */
#ifndef UDSP_TEST_REGION_EDIT_H
#define UDSP_TEST_REGION_EDIT_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../line/line.h"

#define REGION_EDIT_GROW	1024u
#define REGION_EDIT_TEXT_MAX	(UDSP_REGION_FILE_MAX + REGION_EDIT_GROW)

struct region_edit {
	/* The search for find starts at the first match of scope. NULL starts
	 * it at the top of the file. */
	const char *scope;
	const char *find;
	const char *replace;
};

static inline bool region_edit_one(char *text, size_t size,
				   const struct region_edit *e)
{
	size_t find_len = strlen(e->find);
	size_t replace_len = strlen(e->replace);
	char *from = e->scope ? strstr(text, e->scope) : text;
	size_t tail_len;
	char *at;

	if (!from)
		return false;
	at = strstr(from, e->find);
	if (!at)
		return false;
	tail_len = strlen(at + find_len);
	if ((size_t)(at - text) + replace_len + tail_len + 1 > size)
		return false;
	memmove(at + replace_len, at + find_len, tail_len + 1);
	memcpy(at, e->replace, replace_len);

	return true;
}

/* Reads the region file at path into text and applies the n edits in order.
 * size must be at least REGION_EDIT_TEXT_MAX. */
static inline bool region_text_edit(const char *path, char *text, size_t size,
				    const struct region_edit *edit, unsigned n)
{
	FILE *f;
	size_t got;

	if (size < REGION_EDIT_TEXT_MAX)
		return false;
	f = fopen(path, "rb");
	if (!f)
		return false;
	got = fread(text, 1, UDSP_REGION_FILE_MAX, f);
	if (ferror(f)) {
		fclose(f);
		return false;
	}
	fclose(f);
	text[got] = '\0';
	for (unsigned i = 0; i < n; i++) {
		if (!region_edit_one(text, size, &edit[i]))
			return false;
	}

	return true;
}

#endif
