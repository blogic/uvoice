// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The entropy source of the core: the SDES keys of a call leg
 * (struct call_media_ops rand_bytes) and struct proto_host random_bytes.
 */
#include <errno.h>
#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>

#include "uvoice.h"

/* urandom never blocks. It answers while the getrandom pool is not ready,
 * which only happens very early in boot. */
static bool urandom_read(uint8_t *p, size_t len)
{
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	bool ok = true;

	if (fd < 0)
		return false;

	while (len) {
		ssize_t n = read(fd, p, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			ok = false;
			break;
		}

		p += n;
		len -= (size_t)n;
	}

	close(fd);

	return ok;
}

/*
 * The read never blocks the uloop thread: GRND_NONBLOCK, and urandom for what
 * the pool did not answer. A read of up to 256 bytes from an initialized pool
 * returns whole, so the loop runs again only after a signal cut it short.
 */
bool uvoice_random_bytes(void *buf, size_t len)
{
	uint8_t *p = buf;

	while (len) {
		ssize_t n = getrandom(p, len, GRND_NONBLOCK);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return urandom_read(p, len);

		p += n;
		len -= (size_t)n;
	}

	return true;
}
