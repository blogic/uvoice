// SPDX-License-Identifier: BSD-2-Clause
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * This file stays BSD-2-Clause so a SLIC or fax plug-in and its tools
 * compile against it without LGPL obligations. The rest of libudsp is
 * LGPL-2.1-only.
 *
 * struct udsp_slic_host over a real Linux system: spidev for the one bus
 * primitive, sysfs gpio for slic_reset and slic_interrupt, CLOCK_MONOTONIC for
 * the clock hooks and a pthread mutex for the critical section (slic.h). No
 * chip knowledge lives here; the plug-in owns every wire frame.
 *
 * GPIO names. The plug-in asks for the canonical names "slic_reset" and
 * "slic_interrupt"; this module maps them onto the sysfs names the board
 * exports (gpio_reset and gpio_interrupt in struct udsp_config). Any other name
 * is used as the sysfs name directly, so a bare export name also resolves.
 * The interrupt pin's value node stays open after its first read, because the
 * slic thread reads it on every pass; every other node is opened per call.
 * slic_host_irq_arm turns the same node into an edge source: it writes "both"
 * to the export's edge node and hands the value descriptor out for a ppoll
 * with POLLPRI, and slic_host_close writes "none" back.
 *
 * CS framing. A part can need the chip select to break inside one buffer. The
 * ProSLIC part of the MT7987 boards answers only with one CS assertion per byte
 * (udsp.c udsp_host_open), and a part can want the address phase and the data
 * phase under separate CS assertions. The framing here is mechanical: cs_split
 * is a byte offset and cs_per_byte is a flag. The caller owns the
 * chip-meaningful offset, so no register or frame concept enters this module
 * (slic.h).
 */
#ifndef UDSP_SLIC_HOST_LINUX_H
#define UDSP_SLIC_HOST_LINUX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/spi/spidev.h>

#include "udsp/slic.h"

#define SLIC_HOST_XFER_MAX	4096
#define SLIC_HOST_SEGS_MAX	16	/* segments per SPI_IOC_MESSAGE */

struct slic_host_cfg {
	const char *spidev;		/* e.g. "/dev/spidev0.0" */
	const char *gpio_reset;		/* sysfs name for "slic_reset" */
	const char *gpio_interrupt;	/* sysfs name for "slic_interrupt" */
	const char *gpio_base;		/* NULL = "/sys/class/gpio" */
	uint32_t speed_hz;		/* 0 = 1000000 */
	uint8_t spi_mode;		/* SPI_MODE_0 by default */
	uint8_t bits_per_word;		/* 0 = 8 */
	uint8_t cs_split;		/* 0 = one CS over the whole buffer;
					   N > 0 = deassert CS once after the
					   first N bytes */
	bool cs_per_byte;		/* deassert CS after every byte;
					   overrides cs_split */
	/* The PCM highway's data offset in bit clocks after FSYNC. A board
	 * fact, carried through to udsp_slic_host so the plug-in can add it to
	 * every timeslot it programs. This module never interprets it; it only
	 * hands it on. */
	uint8_t pcm_offset_bits;
	/* Nonzero asks the plug-in for the wideband PCM mode. A config fact,
	 * carried through to udsp_slic_host untouched, exactly as
	 * pcm_offset_bits is. */
	uint8_t pcm_wideband;
	/* The text sink the plug-in reaches as udsp_slic_host.log, with the
	 * context it is called with. NULL leaves the hook NULL and the plug-in
	 * on its own fallback. This module neither formats nor filters a line;
	 * it hands each one on. */
	void (*log)(void *ctx, enum udsp_slic_log_level level, const char *msg);
	void *log_ctx;
};

/* What the descriptor is configured with right now. */
struct slic_host_bus {
	uint32_t speed_hz;
	uint8_t spi_mode;
	uint8_t cs_split;
	bool cs_per_byte;
};

struct slic_host;

/* Opens the spidev named in cfg and applies mode, word size and speed.
 * Returns 0 or a negative errno. */
int slic_host_open(const struct slic_host_cfg *cfg, struct slic_host **out);

/* Wraps a descriptor the caller already owns and does not configure the bus.
 * The caller keeps ownership of fd. */
int slic_host_open_fd(const struct slic_host_cfg *cfg, int fd,
		      struct slic_host **out);

/* Re-applies SPI mode, clock and CS framing on a live descriptor, so a bench
 * sweep does not have to reopen the node. speed_hz 0 keeps the current clock.
 * Word size is not touched; slic_host_open already applied it.
 *
 * Works on both open paths. On the slic_host_open_fd path that means it
 * reconfigures a descriptor the caller owns, and the caller sees the new mode
 * and clock on its own spidev; that is intended, so a sweep can drive a
 * borrowed node.
 *
 * Each field is stored only after its own ioctl succeeded, so a partial
 * failure leaves the struct mirroring what the hardware actually accepted.
 * Returns 0 or a negative errno. */
int slic_host_bus_set(struct slic_host *h, uint8_t spi_mode, uint32_t speed_hz,
		      uint8_t cs_split, bool cs_per_byte);

/* Reads back the stored bus state, so a caller can see what survived a failed
 * slic_host_bus_set. Returns 0 or a negative errno. */
int slic_host_bus_get(const struct slic_host *h, struct slic_host_bus *out);

/* Rewrites the wideband host fact after the host is open. The line rate settles
 * after the attach, so a 16 kHz ask the plug-in refuses clears the fact again
 * before init reads it. Returns 0 or -EINVAL. */
int slic_host_pcm_wideband_set(struct slic_host *h, bool on);

/* Arms an edge interrupt on the interrupt pin and returns its value
 * descriptor, which reports POLLPRI on every edge until the next read at
 * offset 0. The descriptor belongs to the host and is the one host_gpio_get
 * reads through. Returns a negative errno when the board exports no interrupt
 * pin (-ENODEV) or the edge node refuses the write, and the caller then keeps
 * its per-tick level read. */
int slic_host_irq_arm(struct slic_host *h);

/* The armed descriptor as it stands now: a read error reopens the node, so
 * the number can change between two reads, and the waiter refreshes it before
 * every wait. -1 before slic_host_irq_arm and when the node will not open. */
int slic_host_irq_fd(const struct slic_host *h);

void slic_host_close(struct slic_host *h);
const struct udsp_slic_host *slic_host_hooks(struct slic_host *h);

/* The live spidev descriptor. The async-signal-safe safe-state handler needs
 * it and the framing before a fatal signal arrives, because it may call
 * nothing at all once one has. -1 when the host carries no descriptor. */
int slic_host_fd(const struct slic_host *h);

/* Lays out the spidev segments for one buffer under the given framing. Pure:
 * it touches no descriptor, so the framing is testable without a spidev.
 * tr must hold max segments; rx may be NULL. Returns the segment count, or 0
 * when the arguments are unusable or max is too small for the framing. */
size_t slic_host_segs_build(struct spi_ioc_transfer *tr, size_t max,
			    const uint8_t *tx, uint8_t *rx, size_t len,
			    uint8_t cs_split, bool cs_per_byte,
			    uint32_t speed_hz, uint8_t bits);

/* Byte count of the next SPI message while remaining bytes are still to send.
 * Per-byte framing costs one segment per byte, so it goes out in chunks of
 * SLIC_HOST_SEGS_MAX; every other framing is a single message. */
size_t slic_host_chunk_plan(size_t remaining, bool cs_per_byte);

/* Issues one built message. Returns 0 or a negative errno. The production
 * implementation is the SPI_IOC_MESSAGE ioctl; the tests record instead, so
 * they check the chunk loop without a spidev. */
typedef int (*slic_host_msg_fn)(void *ctx, struct spi_ioc_transfer *tr,
				size_t n);

/* Walks one buffer under the given framing and hands each chunk to msg. Stops
 * at the first failing chunk and returns its errno. Returns 0 or a negative
 * errno. */
int slic_host_xfer_run(slic_host_msg_fn msg, void *ctx, const uint8_t *tx,
		       uint8_t *rx, size_t len, uint8_t cs_split,
		       bool cs_per_byte, uint32_t speed_hz, uint8_t bits);

#endif
