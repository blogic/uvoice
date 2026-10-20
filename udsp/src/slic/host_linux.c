// SPDX-License-Identifier: BSD-2-Clause
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * This file stays BSD-2-Clause so a SLIC or fax plug-in and its tools
 * compile against it without LGPL obligations. The rest of libudsp is
 * LGPL-2.1-only.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/spi/spidev.h>

#include "host_linux.h"

#define GPIO_BASE_DEFAULT	"/sys/class/gpio"
#define SPI_SPEED_DEFAULT	1000000
#define SPI_BITS_DEFAULT	8
#define GPIO_PATH_MAX		256
#define GPIO_NAME_MAX		64

struct slic_host {
	struct udsp_slic_host hooks;
	int fd;
	int irq_fd;
	bool irq_armed;
	bool own_fd;
	uint32_t speed_hz;
	uint8_t spi_mode;
	uint8_t bits_per_word;
	uint8_t cs_split;
	bool cs_per_byte;
	char gpio_base[GPIO_PATH_MAX];
	char gpio_reset[GPIO_NAME_MAX];
	char gpio_interrupt[GPIO_NAME_MAX];
	pthread_mutex_t lock;
	void (*log)(void *ctx, enum udsp_slic_log_level level, const char *msg);
	void *log_ctx;
};

static void str_copy(char *dst, size_t cap, const char *src, const char *fallback)
{
	const char *s = (src && *src) ? src : fallback;

	snprintf(dst, cap, "%s", s ? s : "");
}

static const char *gpio_name_resolve(const struct slic_host *h, const char *name)
{
	if (!name || !*name)
		return NULL;
	if (!strcmp(name, "slic_reset"))
		return *h->gpio_reset ? h->gpio_reset : NULL;
	if (!strcmp(name, "slic_interrupt"))
		return *h->gpio_interrupt ? h->gpio_interrupt : NULL;
	return name;
}

static int gpio_node_path_build(const struct slic_host *h, const char *resolved,
				const char *node, char *out, size_t cap)
{
	int n;

	if (!resolved)
		return -ENODEV;

	n = snprintf(out, cap, "%s/%s/%s", h->gpio_base, resolved, node);
	if (n < 0 || (size_t)n >= cap)
		return -ENAMETOOLONG;
	return 0;
}

static int gpio_path_build(const struct slic_host *h, const char *resolved,
			   char *out, size_t cap)
{
	return gpio_node_path_build(h, resolved, "value", out, cap);
}

static bool gpio_is_interrupt(const struct slic_host *h, const char *resolved)
{
	return *h->gpio_interrupt && !strcmp(resolved, h->gpio_interrupt);
}

/* A read at offset 0 makes sysfs run the attribute's show again, so one open
 * descriptor serves every later read with a fresh value. */
static int gpio_value_read(int fd, int *value)
{
	char byte = 0;
	ssize_t n = pread(fd, &byte, 1, 0);

	if (n != 1)
		return n < 0 ? -errno : -EIO;
	*value = byte != '0';
	return 0;
}

static int gpio_node_write(const char *path, const char *text)
{
	size_t len = strlen(text);
	ssize_t n;
	int fd;

	fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	n = write(fd, text, len);
	close(fd);
	if (n < 0)
		return -errno;
	return (size_t)n == len ? 0 : -EIO;
}

static int irq_fd_open(struct slic_host *h)
{
	char path[GPIO_PATH_MAX];
	int rc;

	if (h->irq_fd >= 0)
		return h->irq_fd;
	rc = gpio_path_build(h, *h->gpio_interrupt ? h->gpio_interrupt : NULL,
			     path, sizeof(path));
	if (rc)
		return rc;
	h->irq_fd = open(path, O_RDONLY | O_CLOEXEC);
	return h->irq_fd >= 0 ? h->irq_fd : -errno;
}

static int irq_edge_set(struct slic_host *h, const char *edge)
{
	char path[GPIO_PATH_MAX];
	int rc;

	rc = gpio_node_path_build(h, *h->gpio_interrupt ? h->gpio_interrupt :
						NULL, "edge", path, sizeof(path));
	if (rc)
		return rc;
	return gpio_node_write(path, edge);
}

static void irq_fd_drop(struct slic_host *h)
{
	if (h->irq_fd >= 0)
		close(h->irq_fd);
	h->irq_fd = -1;
}

/* A read that failed through the kept descriptor: a pin exported again
 * behind the daemon leaves a dead node behind it, so the node is opened
 * afresh, with its edge armed again when it was armed before, and read once
 * more. A retry that fails too leaves no descriptor, so the next call starts
 * from the open again. The caller of slic_host_irq_fd picks the new number,
 * or the -1, up before its next wait. Every call runs on the slic thread. */
static int irq_read_retry(struct slic_host *h, int *value, int err)
{
	int fd, rc;

	irq_fd_drop(h);
	if (h->irq_armed && irq_edge_set(h, "both"))
		return err;
	fd = irq_fd_open(h);
	if (fd < 0)
		return fd;
	rc = gpio_value_read(fd, value);
	if (rc)
		irq_fd_drop(h);
	return rc;
}


static size_t segs_count(size_t len, uint8_t cs_split, bool cs_per_byte)
{
	if (cs_per_byte)
		return len;
	if (cs_split == 0 || cs_split >= len)
		return 1;
	return 2;
}

/* spidev.h: cs_change on a non-final transfer deasserts CS between it and the
 * next one, so the final transfer keeps 0 and CS behaves normally at the end
 * of the message. */
size_t slic_host_segs_build(struct spi_ioc_transfer *tr, size_t max,
			    const uint8_t *tx, uint8_t *rx, size_t len,
			    uint8_t cs_split, bool cs_per_byte,
			    uint32_t speed_hz, uint8_t bits)
{
	size_t n, i, off = 0;

	if (!tr || !tx || len == 0)
		return 0;

	n = segs_count(len, cs_split, cs_per_byte);
	if (n > max)
		return 0;

	memset(tr, 0, n * sizeof(*tr));

	for (i = 0; i < n; i++) {
		size_t seg = len - off;

		if (i + 1 < n)
			seg = cs_per_byte ? 1 : cs_split;

		tr[i].tx_buf = (unsigned long)(uintptr_t)(tx + off);
		tr[i].rx_buf = rx ? (unsigned long)(uintptr_t)(rx + off) : 0;
		tr[i].len = (uint32_t)seg;
		tr[i].speed_hz = speed_hz;
		tr[i].bits_per_word = bits;
		tr[i].cs_change = i + 1 < n ? 1 : 0;
		off += seg;
	}

	return n;
}

size_t slic_host_chunk_plan(size_t remaining, bool cs_per_byte)
{
	if (!cs_per_byte || remaining <= SLIC_HOST_SEGS_MAX)
		return remaining;
	return SLIC_HOST_SEGS_MAX;
}

int slic_host_xfer_run(slic_host_msg_fn msg, void *ctx, const uint8_t *tx,
		       uint8_t *rx, size_t len, uint8_t cs_split,
		       bool cs_per_byte, uint32_t speed_hz, uint8_t bits)
{
	struct spi_ioc_transfer tr[SLIC_HOST_SEGS_MAX];
	size_t done = 0;

	if (!msg || !tx || len == 0 || len > SLIC_HOST_XFER_MAX)
		return -EINVAL;

	while (done < len) {
		size_t chunk = slic_host_chunk_plan(len - done, cs_per_byte);
		size_t n = slic_host_segs_build(tr, SLIC_HOST_SEGS_MAX,
						tx + done, rx ? rx + done : NULL,
						chunk, cs_split, cs_per_byte,
						speed_hz, bits);
		int rc;

		if (!n)
			return -EINVAL;
		rc = msg(ctx, tr, n);
		if (rc)
			return rc;
		done += chunk;
	}

	return 0;
}

static int host_msg_ioctl(void *ctx, struct spi_ioc_transfer *tr, size_t n)
{
	struct slic_host *h = ctx;

	if (ioctl(h->fd, SPI_IOC_MESSAGE(n), tr) < 0)
		return -errno;
	return 0;
}

static int host_spi_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
	struct slic_host *h = ctx;

	if (!h || !tx || len == 0 || len > SLIC_HOST_XFER_MAX)
		return -EINVAL;
	if (h->fd < 0)
		return -ENODEV;

	return slic_host_xfer_run(host_msg_ioctl, h, tx, rx, len, h->cs_split,
				  h->cs_per_byte, h->speed_hz, h->bits_per_word);
}

static int host_gpio_set(void *ctx, const char *name, int value)
{
	struct slic_host *h = ctx;
	char path[GPIO_PATH_MAX];
	char byte = value ? '1' : '0';
	int fd, rc;
	ssize_t n;

	rc = gpio_path_build(h, gpio_name_resolve(h, name), path, sizeof(path));
	if (rc)
		return rc;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	n = write(fd, &byte, 1);
	rc = n == 1 ? 0 : (n < 0 ? -errno : -EIO);
	close(fd);
	return rc;
}

/*
 * The slic thread reads the interrupt pin on every pass, so its value node
 * stays open from the first read to slic_host_close and each pass costs one
 * pread. Every other pin is opened per call. A read that fails through the
 * kept descriptor reopens the node and reads once more (irq_read_retry).
 */
static int host_gpio_get(void *ctx, const char *name, int *value)
{
	struct slic_host *h = ctx;
	const char *resolved = gpio_name_resolve(h, name);
	char path[GPIO_PATH_MAX];
	int fd, rc;

	if (!value)
		return -EINVAL;

	rc = gpio_path_build(h, resolved, path, sizeof(path));
	if (rc)
		return rc;

	if (!gpio_is_interrupt(h, resolved)) {
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			return -errno;
		rc = gpio_value_read(fd, value);
		close(fd);
		return rc;
	}

	fd = irq_fd_open(h);
	if (fd < 0)
		return fd;
	rc = gpio_value_read(fd, value);
	if (rc)
		return irq_read_retry(h, value, rc);
	return 0;
}

static uint32_t host_mono_ms(void *ctx)
{
	struct timespec ts;

	(void)ctx;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
}

static void host_delay_ms(void *ctx, unsigned ms)
{
	struct timespec req = {
		.tv_sec = ms / 1000,
		.tv_nsec = (long)(ms % 1000) * 1000000L,
	};

	(void)ctx;
	while (nanosleep(&req, &req) < 0 && errno == EINTR)
		continue;
}

/* si_voice_ctrl.h: the plug-in passes 1 to enter and 0 to leave. */
static int host_sem(void *ctx, int enter)
{
	struct slic_host *h = ctx;
	int rc = enter ? pthread_mutex_lock(&h->lock)
		       : pthread_mutex_unlock(&h->lock);

	return rc ? -rc : 0;
}

static void host_log(void *ctx, enum udsp_slic_log_level level, const char *msg)
{
	struct slic_host *h = ctx;

	h->log(h->log_ctx, level, msg);
}

static int host_alloc(const struct slic_host_cfg *cfg, int fd, bool own_fd,
		      struct slic_host **out)
{
	struct slic_host *h = calloc(1, sizeof(*h));

	if (!h)
		return -ENOMEM;

	h->fd = fd;
	h->irq_fd = -1;
	h->own_fd = own_fd;
	h->speed_hz = cfg->speed_hz ? cfg->speed_hz : SPI_SPEED_DEFAULT;
	h->spi_mode = cfg->spi_mode;
	h->bits_per_word = cfg->bits_per_word ? cfg->bits_per_word : SPI_BITS_DEFAULT;
	h->cs_split = cfg->cs_split;
	h->cs_per_byte = cfg->cs_per_byte;
	str_copy(h->gpio_base, sizeof(h->gpio_base), cfg->gpio_base,
		 GPIO_BASE_DEFAULT);
	str_copy(h->gpio_reset, sizeof(h->gpio_reset), cfg->gpio_reset, "");
	str_copy(h->gpio_interrupt, sizeof(h->gpio_interrupt),
		 cfg->gpio_interrupt, "");

	if (pthread_mutex_init(&h->lock, NULL)) {
		free(h);
		return -ENOMEM;
	}

	h->hooks.ctx = h;
	h->hooks.spi_xfer = host_spi_xfer;
	h->hooks.delay_ms = host_delay_ms;
	h->hooks.mono_ms = host_mono_ms;
	h->hooks.gpio_set = host_gpio_set;
	h->hooks.gpio_get = host_gpio_get;
	h->hooks.sem = host_sem;
	/* A board fact, not a hook: both open paths reach host_alloc, so this
	 * one copy is what serves slic_host_hooks() on either. The wideband ask
	 * goes in the same copy. */
	h->hooks.pcm_offset_bits = cfg->pcm_offset_bits;
	h->hooks.pcm_wideband = cfg->pcm_wideband;
	h->log = cfg->log;
	h->log_ctx = cfg->log_ctx;
	if (h->log)
		h->hooks.log = host_log;

	*out = h;
	return 0;
}

int slic_host_open_fd(const struct slic_host_cfg *cfg, int fd,
		      struct slic_host **out)
{
	if (!cfg || !out || fd < 0)
		return -EINVAL;
	return host_alloc(cfg, fd, false, out);
}

int slic_host_open(const struct slic_host_cfg *cfg, struct slic_host **out)
{
	struct slic_host *h;
	uint32_t speed;
	uint8_t mode, bits;
	int fd, rc;

	if (!cfg || !out || !cfg->spidev || !*cfg->spidev)
		return -EINVAL;

	fd = open(cfg->spidev, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	rc = host_alloc(cfg, fd, true, &h);
	if (rc) {
		close(fd);
		return rc;
	}

	mode = h->spi_mode;
	bits = h->bits_per_word;
	speed = h->speed_hz;

	if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
	    ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
	    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
		rc = -errno;
		slic_host_close(h);
		return rc;
	}

	*out = h;
	return 0;
}

int slic_host_bus_set(struct slic_host *h, uint8_t spi_mode, uint32_t speed_hz,
		      uint8_t cs_split, bool cs_per_byte)
{
	uint8_t mode = spi_mode;
	uint32_t speed;

	if (!h)
		return -EINVAL;
	if (h->fd < 0)
		return -ENODEV;

	if (ioctl(h->fd, SPI_IOC_WR_MODE, &mode) < 0)
		return -errno;
	h->spi_mode = mode;

	speed = speed_hz ? speed_hz : h->speed_hz;
	if (ioctl(h->fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0)
		return -errno;
	h->speed_hz = speed;

	h->cs_split = cs_split;
	h->cs_per_byte = cs_per_byte;
	return 0;
}

int slic_host_bus_get(const struct slic_host *h, struct slic_host_bus *out)
{
	if (!h || !out)
		return -EINVAL;

	out->speed_hz = h->speed_hz;
	out->spi_mode = h->spi_mode;
	out->cs_split = h->cs_split;
	out->cs_per_byte = h->cs_per_byte;
	return 0;
}

int slic_host_pcm_wideband_set(struct slic_host *h, bool on)
{
	if (!h)
		return -EINVAL;

	h->hooks.pcm_wideband = on ? 1u : 0u;

	return 0;
}

int slic_host_irq_arm(struct slic_host *h)
{
	int rc;

	if (!h)
		return -EINVAL;
	if (!*h->gpio_interrupt)
		return -ENODEV;
	rc = irq_edge_set(h, "both");
	if (rc)
		return rc;
	h->irq_armed = true;
	return irq_fd_open(h);
}

int slic_host_irq_fd(const struct slic_host *h)
{
	return h && h->irq_armed ? h->irq_fd : -1;
}

void slic_host_close(struct slic_host *h)
{
	if (!h)
		return;
	if (h->own_fd && h->fd >= 0)
		close(h->fd);
	if (h->irq_fd >= 0)
		close(h->irq_fd);
	/* The kernel holds the interrupt line while an edge is set; the next
	 * daemon starts from an idle pin. */
	if (h->irq_armed)
		irq_edge_set(h, "none");
	pthread_mutex_destroy(&h->lock);
	free(h);
}

const struct udsp_slic_host *slic_host_hooks(struct slic_host *h)
{
	return h ? &h->hooks : NULL;
}

int slic_host_fd(const struct slic_host *h)
{
	return h ? h->fd : -1;
}
