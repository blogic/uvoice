// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * A bounded copy into a fixed character buffer. str_copy() writes at most
 * len minus one bytes of src into dst and always writes the terminator. A
 * NULL src is the empty string. The source must be NUL terminated: a fixed
 * width field that may carry no terminator reads past its end here, so such
 * a field keeps its own bounded-read copy.
 *
 * The header carries only the two libc includes it needs, so a libc-only unit
 * (the star code table) can share the helper without pulling the daemon
 * headers.
 */
#ifndef UVOICE_STR_H
#define UVOICE_STR_H

#include <stddef.h>
#include <string.h>

static inline void str_copy(char *dst, size_t len, const char *src)
{
	size_t n;

	if (!len)
		return;

	n = src ? strlen(src) : 0;
	if (n >= len)
		n = len - 1;
	if (n)
		memcpy(dst, src, n);
	dst[n] = '\0';
}

#endif
