// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The pcapng writer. See pcapng.h for the contract.
 *
 * Every block carries its total length twice, in the header and in the
 * trailer, so the length is computed before the first byte goes out. Each
 * block is padded to a 4-byte boundary, and so is every option.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pcapng.h"

#define PCAPNG_BTYPE_SHB	0x0a0d0d0au
#define PCAPNG_BTYPE_IDB	1u
#define PCAPNG_BTYPE_EPB	6u

#define PCAPNG_BYTE_ORDER	0x1a2b3c4du
/* Zero is "no limit" (pcapng section 4.2). A number here would have to be
 * above the widest entry the daemon writes, and a reader is entitled to treat
 * a packet at the limit as cut. */
#define PCAPNG_SNAPLEN		0u

/* The option codes this writer emits (pcapng section 4). */
#define PCAPNG_OPT_END		0u
#define PCAPNG_OPT_IF_NAME	2u
#define PCAPNG_OPT_IF_TSRESOL	9u
#define PCAPNG_OPT_SHB_USERAPPL	4u

/* Microseconds, as a negative power of ten. */
#define PCAPNG_TSRESOL_US	6

static uint32_t align4(uint32_t len)
{
	return (len + 3u) & ~3u;
}

/*
 * The one write. A failed write closes the file at once and keeps the errno:
 * the blocks below then run to their end without touching the file, and the
 * caller reads the failure from the return value of the call it made.
 */
static void wr(struct pcapng_writer *w, const void *data, size_t len)
{
	if (!w->f || w->err)
		return;

	if (fwrite(data, 1, len, w->f) == len) {
		w->bytes += len;
		return;
	}

	w->err = errno ? -errno : -EIO;
	fclose(w->f);
	w->f = NULL;
}

static void wr_u16(struct pcapng_writer *w, uint16_t val)
{
	wr(w, &val, sizeof(val));
}

static void wr_u32(struct pcapng_writer *w, uint32_t val)
{
	wr(w, &val, sizeof(val));
}

static void wr_u64(struct pcapng_writer *w, uint64_t val)
{
	wr(w, &val, sizeof(val));
}

static void wr_pad(struct pcapng_writer *w, uint32_t len)
{
	static const uint8_t zero[4];

	if (len)
		wr(w, zero, len);
}

static uint32_t opt_str_size(const char *val)
{
	/* The option value carries its terminator, the shape a reader of the
	 * name expects. */
	return 4u + align4((uint32_t)strlen(val) + 1u);
}

static void opt_str_write(struct pcapng_writer *w, uint16_t code,
			  const char *val)
{
	uint32_t len = (uint32_t)strlen(val) + 1u;

	wr_u16(w, code);
	wr_u16(w, (uint16_t)len);
	wr(w, val, len);
	wr_pad(w, align4(len) - len);
}

static void opt_u8_write(struct pcapng_writer *w, uint16_t code, uint8_t val)
{
	wr_u16(w, code);
	wr_u16(w, 1);
	wr(w, &val, 1);
	wr_pad(w, 3);
}

static void opt_end_write(struct pcapng_writer *w)
{
	wr_u16(w, PCAPNG_OPT_END);
	wr_u16(w, 0);
}

int pcapng_open(struct pcapng_writer *w, const char *path, const char *app)
{
	uint32_t len;
	int fd;

	memset(w, 0, sizeof(*w));
	/* open() rather than fopen(), because the mode has to be 0600 whatever
	 * the umask of the daemon is: the file holds a capture. */
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return -errno;

	w->f = fdopen(fd, "w");
	if (!w->f) {
		int err = errno ? -errno : -EIO;

		close(fd);
		return err;
	}

	/* type, length, magic, version, section length, the option and its
	 * end, and the length again. */
	len = 8u + 16u + opt_str_size(app) + 4u + 4u;
	wr_u32(w, PCAPNG_BTYPE_SHB);
	wr_u32(w, len);
	wr_u32(w, PCAPNG_BYTE_ORDER);
	wr_u16(w, 1);
	wr_u16(w, 0);
	/* The section length is unknown while the file grows. */
	wr_u64(w, ~0ull);
	opt_str_write(w, PCAPNG_OPT_SHB_USERAPPL, app);
	opt_end_write(w);
	wr_u32(w, len);

	return w->err;
}

int pcapng_iface_write(struct pcapng_writer *w, uint16_t link_type,
		       const char *name, uint32_t *id)
{
	uint32_t len;

	*id = w->iface_next++;

	/* type, length, link type and snap length, the two options and their
	 * end, and the length again. */
	len = 8u + 8u + opt_str_size(name) + 8u + 4u + 4u;
	wr_u32(w, PCAPNG_BTYPE_IDB);
	wr_u32(w, len);
	wr_u16(w, link_type);
	wr_u16(w, 0);
	wr_u32(w, PCAPNG_SNAPLEN);
	opt_str_write(w, PCAPNG_OPT_IF_NAME, name);
	opt_u8_write(w, PCAPNG_OPT_IF_TSRESOL, PCAPNG_TSRESOL_US);
	opt_end_write(w);
	wr_u32(w, len);

	return w->err;
}

int pcapng_packet_write(struct pcapng_writer *w, uint32_t iface,
			uint64_t stamp_us, const void *data, size_t len)
{
	uint32_t pkt = (uint32_t)len;
	uint32_t block;

	/* type, length, the five fields, the padded payload, the length
	 * again. The block carries no option, which needs no end marker. */
	block = 8u + 20u + align4(pkt) + 4u;
	wr_u32(w, PCAPNG_BTYPE_EPB);
	wr_u32(w, block);
	wr_u32(w, iface);
	wr_u32(w, (uint32_t)(stamp_us >> 32));
	wr_u32(w, (uint32_t)stamp_us);
	wr_u32(w, pkt);
	wr_u32(w, pkt);
	wr(w, data, pkt);
	wr_pad(w, align4(pkt) - pkt);
	wr_u32(w, block);

	return w->err;
}

int pcapng_close(struct pcapng_writer *w, uint64_t *bytes)
{
	int err = w->err;

	if (bytes)
		*bytes = w->bytes;

	if (!w->f)
		return err;

	if (fflush(w->f) != 0 && !err)
		err = errno ? -errno : -EIO;
	if (fclose(w->f) != 0 && !err)
		err = errno ? -errno : -EIO;

	w->f = NULL;
	w->err = err;

	return err;
}
