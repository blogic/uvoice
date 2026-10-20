// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>

#include <linux/spi/spidev.h>

#include "udsp_priv.h"
#include "safe_state.h"

#define SAFE_MSG_MAX	(UDSP_SLIC_SAFE_MAX / SLIC_HOST_SEGS_MAX)
#define SAFE_BITS	8

static const int safe_signals[] = {
	SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS,
};

#define SAFE_SIGNAL_COUNT	(sizeof(safe_signals) / sizeof(safe_signals[0]))

struct safe_msg {
	struct spi_ioc_transfer tr[SLIC_HOST_SEGS_MAX];
	size_t segs;
};

/* Process-wide because signal dispositions are: one live handle per process is
 * exactly what makes that safe. */
static struct {
	struct udsp_slic_safe safe;
	struct safe_msg msg[SAFE_MSG_MAX];
	unsigned count;
	int fd;
	bool armed;
	slic_host_msg_fn msg_fn;
	void *msg_ctx;
	struct sigaction prev[SAFE_SIGNAL_COUNT];
} safe_block;

static int safe_msg_ioctl(void *ctx, struct spi_ioc_transfer *tr, size_t n)
{
	int fd = *(const int *)ctx;

	if (fd < 0)
		return -ENODEV;
	if (ioctl(fd, SPI_IOC_MESSAGE(n), tr) < 0)
		return -errno;

	return 0;
}

void udsp_safe_state_msg_set(slic_host_msg_fn fn, void *ctx)
{
	safe_block.msg_fn = fn ? fn : safe_msg_ioctl;
	safe_block.msg_ctx = fn ? ctx : &safe_block.fd;
}

static void safe_handler(int sig)
{
	for (unsigned i = 0; i < safe_block.count; i++)
		safe_block.msg_fn(safe_block.msg_ctx, safe_block.msg[i].tr,
				  safe_block.msg[i].segs);

	/* SA_RESETHAND already put the default disposition back, so one raise
	 * terminates the process with the original signal. */
	raise(sig);
}

static void safe_messages_build(uint8_t cs_split, bool cs_per_byte,
				uint32_t speed_hz)
{
	size_t done = 0;

	safe_block.count = 0;
	while (done < safe_block.safe.len && safe_block.count < SAFE_MSG_MAX) {
		struct safe_msg *m = &safe_block.msg[safe_block.count];
		size_t chunk = slic_host_chunk_plan(safe_block.safe.len - done,
						    cs_per_byte);
		size_t n = slic_host_segs_build(m->tr, SLIC_HOST_SEGS_MAX,
						safe_block.safe.bytes + done,
						NULL, chunk, cs_split,
						cs_per_byte, speed_hz,
						SAFE_BITS);

		if (!n)
			return;
		m->segs = n;
		safe_block.count++;
		done += chunk;
	}
}

int udsp_safe_state_arm(struct udsp_slic *slic, const struct udsp_slic_ops *ops,
			struct slic_host *host)
{
	struct slic_host_bus bus = {
		.speed_hz = 0,
		.spi_mode = 0,
		.cs_split = 0,
		/* The measured production framing; it is also what the fake's
		 * frames are laid out against. */
		.cs_per_byte = true,
	};
	struct sigaction sa;

	if (!slic || !ops || !ops->safe_xfer)
		return -EINVAL;

	memset(&safe_block.safe, 0, sizeof(safe_block.safe));
	ops->safe_xfer(slic, &safe_block.safe);
	if (host) {
		slic_host_bus_get(host, &bus);
		safe_block.fd = slic_host_fd(host);
	} else {
		safe_block.fd = -1;
	}
	if (!safe_block.msg_fn)
		udsp_safe_state_msg_set(NULL, NULL);
	safe_messages_build(bus.cs_split, bus.cs_per_byte, bus.speed_hz);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = safe_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_ONSTACK | SA_NODEFER | SA_RESETHAND;
	for (unsigned i = 0; i < SAFE_SIGNAL_COUNT; i++)
		sigaction(safe_signals[i], &sa, &safe_block.prev[i]);
	safe_block.armed = true;

	return 0;
}

void udsp_safe_state_disarm(void)
{
	if (!safe_block.armed)
		return;

	for (unsigned i = 0; i < SAFE_SIGNAL_COUNT; i++)
		sigaction(safe_signals[i], &safe_block.prev[i], NULL);
	safe_block.armed = false;
	safe_block.count = 0;
	safe_block.fd = -1;
}
