// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Host tests for the two BSD-licensed modules that the SLIC plug-in needs:
 * slic/host_linux.c (spidev, sysfs gpio, clock, semaphore) and
 * slic/plugin_load.c (dlopen and descriptor validation). No SLIC hardware and
 * no proprietary plug-in take part. The gpio hooks run against a temporary
 * fake sysfs tree. The loader runs against stub .so files that the suite
 * compiles into a temporary directory.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "udsp/slic.h"

#include "host_linux.h"
#include "plugin_load.h"
#include "util.h"

#define OPS_SLOTS	24
#define TPATH_MAX	256
#define SEG_SPEED	500000
#define SEG_BITS	8
#define MSG_REC_MAX	8

_Static_assert(sizeof(struct udsp_slic_ops) == OPS_SLOTS * sizeof(void *),
	       "the stub generator needs OPS_SLOTS pointers for the ops table");

static char tmpdir[TPATH_MAX];

static bool dir_make(const char *path)
{
	return mkdir(path, 0755) == 0 || errno == EEXIST;
}

static bool file_write(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");

	if (!f)
		return false;
	fputs(text, f);
	return fclose(f) == 0;
}

static bool file_read(const char *path, char *out, size_t cap)
{
	FILE *f = fopen(path, "r");
	size_t n;

	if (!f)
		return false;
	n = fread(out, 1, cap - 1, f);
	out[n] = '\0';
	fclose(f);
	return true;
}

static bool path_join(char *out, size_t cap, const char *a, const char *b)
{
	int n = snprintf(out, cap, "%s/%s", a, b);

	return n > 0 && (size_t)n < cap;
}

static int fd_count(void)
{
	DIR *d = opendir("/proc/self/fd");
	struct dirent *e;
	int n = 0;

	if (!d)
		return -1;
	while ((e = readdir(d)))
		n++;
	closedir(d);
	return n;
}

/* Builds one stub plug-in. The stub restates the descriptor layout instead of
 * including slic.h, so a silent change to either side shows up here. */
static bool stub_build_named(const char *name, unsigned abi, size_t struct_size,
			     bool with_ops, bool with_name, char *so_path,
			     size_t cap)
{
	char src[TPATH_MAX], cmd[TPATH_MAX * 3];
	char body[2048];
	FILE *f;

	if (snprintf(src, sizeof(src), "%s/%s.c", tmpdir, name) < 0)
		return false;
	if (snprintf(so_path, cap, "%s/%s.so", tmpdir, name) < 0)
		return false;

	/* Explicit slots expose a member inserted in the middle of the ABI.
	 * Order: name, nine mandatory ops, five nullable ops, safe_xfer,
	 * part_name, region_set, caps_get, reinit, converter_restart,
	 * monitor_read, converter_read. */
	snprintf(body, sizeof(body),
		 "static void stub_op(void) { }\n"
		 "static const char stub_name[] = \"stub\";\n"
		 "static const void *stub_ops[%u] = {\n"
		 "\t%s,\n"
		 "\t%s, %s, %s, %s, %s, %s, %s, %s, %s,\n"
		 "\t0, 0, 0, 0, 0,\n"
		 "\t%s,\n"
		 "\t0, 0, 0,\n"
		 "\t0, 0, 0, 0\n"
		 "};\n"
		 "struct stub_desc { unsigned abi; unsigned size; const void *ops; };\n"
		 "const struct stub_desc udsp_slic_descriptor = { %uu, %uu, stub_ops };\n",
		 OPS_SLOTS,
		 with_name ? "stub_name" : "0",
		 with_ops ? "stub_op" : "0", with_ops ? "stub_op" : "0",
		 with_ops ? "stub_op" : "0", with_ops ? "stub_op" : "0",
		 with_ops ? "stub_op" : "0", with_ops ? "stub_op" : "0",
		 with_ops ? "stub_op" : "0", with_ops ? "stub_op" : "0",
		 with_ops ? "stub_op" : "0", with_ops ? "stub_op" : "0",
		 abi, (unsigned)struct_size);

	f = fopen(src, "w");
	if (!f)
		return false;
	fputs(body, f);
	fclose(f);

	snprintf(cmd, sizeof(cmd), "cc -shared -fPIC -o '%s' '%s' 2>/dev/null",
		 so_path, src);
	return system(cmd) == 0;
}

static bool stub_build(const char *name, unsigned abi, size_t struct_size,
		       bool with_ops, char *so_path, size_t cap)
{
	return stub_build_named(name, abi, struct_size, with_ops, true, so_path,
				cap);
}

static bool stub_build_empty(char *so_path, size_t cap)
{
	char src[TPATH_MAX], cmd[TPATH_MAX * 3];

	if (!path_join(src, sizeof(src), tmpdir, "nosym.c"))
		return false;
	if (!path_join(so_path, cap, tmpdir, "nosym.so"))
		return false;

	if (!file_write(src, "int udsp_unrelated_symbol;\n"))
		return false;

	snprintf(cmd, sizeof(cmd), "cc -shared -fPIC -o '%s' '%s' 2>/dev/null",
		 so_path, src);
	return system(cmd) == 0;
}

static void test_gpio(void)
{
	struct slic_host_cfg cfg;
	const struct udsp_slic_host *hooks;
	struct slic_host *h = NULL;
	char dir[TPATH_MAX], path[TPATH_MAX], text[16];
	int value = -1, fd, fds;

	char pin[TPATH_MAX];

	CHECK(path_join(dir, sizeof(dir), tmpdir, "gpio"));
	CHECK(dir_make(dir));
	CHECK(path_join(pin, sizeof(pin), dir, "pio30"));
	CHECK(dir_make(pin));
	CHECK(path_join(path, sizeof(path), pin, "value"));
	CHECK(file_write(path, "0\n"));

	memset(&cfg, 0, sizeof(cfg));
	cfg.gpio_base = dir;
	cfg.gpio_reset = "pio30";
	cfg.gpio_interrupt = "pio12";

	fd = open("/dev/null", O_RDWR);
	CHECK(fd >= 0);

	CASE("slic_host_open_fd wraps a descriptor the caller owns");
	CHECK_UINT(slic_host_open_fd(&cfg, fd, &h), 0);
	CHECK(h != NULL);
	hooks = slic_host_hooks(h);
	CHECK(hooks != NULL);

	CASE("gpio_set writes the sysfs value node of the mapped export");
	CHECK_UINT(hooks->gpio_set(hooks->ctx, "slic_reset", 1), 0);
	CHECK(file_read(path, text, sizeof(text)));
	CHECK_UINT(text[0], '1');
	CHECK_UINT(hooks->gpio_set(hooks->ctx, "slic_reset", 0), 0);
	CHECK(file_read(path, text, sizeof(text)));
	CHECK_UINT(text[0], '0');

	CASE("gpio_get reads it back");
	CHECK(file_write(path, "1\n"));
	CHECK_UINT(hooks->gpio_get(hooks->ctx, "slic_reset", &value), 0);
	CHECK_UINT(value, 1);
	CHECK(file_write(path, "0\n"));
	CHECK_UINT(hooks->gpio_get(hooks->ctx, "slic_reset", &value), 0);
	CHECK_UINT(value, 0);
	CHECK_UINT(hooks->gpio_get(hooks->ctx, "slic_reset", NULL), -EINVAL);

	CASE("a bare sysfs export name resolves too");
	CHECK_UINT(hooks->gpio_set(hooks->ctx, "pio30", 1), 0);
	CHECK(file_read(path, text, sizeof(text)));
	CHECK_UINT(text[0], '1');

	CASE("an unconfigured canonical name is ENODEV, a missing node ENOENT");
	CHECK_UINT(hooks->gpio_set(hooks->ctx, "slic_interrupt", 1), -ENOENT);
	CHECK_UINT(hooks->gpio_set(hooks->ctx, NULL, 1), -ENODEV);
	CHECK_UINT(hooks->gpio_get(hooks->ctx, "nosuchpin", &value), -ENOENT);

	CASE("the interrupt pin keeps one descriptor open across reads");
	{
		char irq_pin[TPATH_MAX], irq_path[TPATH_MAX];

		CHECK(path_join(irq_pin, sizeof(irq_pin), dir, "pio12"));
		CHECK(dir_make(irq_pin));
		CHECK(path_join(irq_path, sizeof(irq_path), irq_pin, "value"));
		/* A directory opens but does not read, so this is the failed
		 * read that must drop the descriptor. */
		CHECK(dir_make(irq_path));
		fds = fd_count();
		CHECK_UINT(hooks->gpio_get(hooks->ctx, "slic_interrupt", &value),
			   -EISDIR);
		CHECK_UINT(fd_count(), fds);
		CHECK(rmdir(irq_path) == 0);
		CHECK(file_write(irq_path, "1\n"));
		CHECK_UINT(hooks->gpio_get(hooks->ctx, "slic_interrupt", &value),
			   0);
		CHECK_UINT(value, 1);
		CHECK_UINT(fd_count(), fds + 1);
		CHECK(file_write(irq_path, "0\n"));
		CHECK_UINT(hooks->gpio_get(hooks->ctx, "slic_interrupt", &value),
			   0);
		CHECK_UINT(value, 0);
		CHECK(file_write(irq_path, "1\n"));
		CHECK_UINT(hooks->gpio_get(hooks->ctx, "pio12", &value), 0);
		CHECK_UINT(value, 1);
		CHECK_UINT(fd_count(), fds + 1);
		CHECK_UINT(hooks->gpio_get(hooks->ctx, "slic_reset", &value), 0);
		CHECK_UINT(fd_count(), fds + 1);
	}

	CASE("the interrupt edge arms through the export's edge node");
	{
		char edge_path[TPATH_MAX];
		int irq;

		CHECK(path_join(edge_path, sizeof(edge_path), dir,
				"pio12/edge"));
		CHECK_UINT(slic_host_irq_arm(NULL), -EINVAL);
		/* No edge node: the board's gpio driver offers no interrupt,
		 * and the caller keeps its per-tick level read. */
		CHECK_UINT(slic_host_irq_arm(h), -ENOENT);
		CHECK(file_write(edge_path, "none\n"));
		irq = slic_host_irq_arm(h);
		CHECK(irq >= 0);
		CHECK(file_read(edge_path, text, sizeof(text)));
		CHECK(!strcmp(text, "both"));
		/* The value descriptor of the reads above is the one handed
		 * out: no second open. */
		CHECK_UINT(fd_count(), fds + 1);
		CHECK_UINT(slic_host_irq_arm(h), irq);
		CHECK_UINT(slic_host_irq_fd(h), irq);
		CHECK_UINT(hooks->gpio_get(hooks->ctx, "slic_interrupt", &value),
			   0);
		CHECK_UINT(value, 1);
	}

	CASE("a read error on the armed pin reopens the node and re-arms it");
	{
		char irq_dir[TPATH_MAX], irq_path[TPATH_MAX], edge_path[TPATH_MAX];

		CHECK(path_join(irq_dir, sizeof(irq_dir), dir, "pio12"));
		CHECK(path_join(irq_path, sizeof(irq_path), irq_dir, "value"));
		CHECK(path_join(edge_path, sizeof(edge_path), irq_dir, "edge"));
		/* An empty node reads no byte, which is the read error a dead
		 * node gives: the host re-arms the edge, reopens, reads once
		 * more, and holds nothing when that fails too. */
		CHECK(file_write(edge_path, "none\n"));
		CHECK(truncate(irq_path, 0) == 0);
		CHECK_UINT(hooks->gpio_get(hooks->ctx, "slic_interrupt", &value),
			   -EIO);
		CHECK(slic_host_irq_fd(h) < 0);
		CHECK_UINT(fd_count(), fds);
		CHECK(file_read(edge_path, text, sizeof(text)));
		CHECK(!strcmp(text, "both"));
		/* The node comes back: the next read opens it again and the
		 * armed descriptor is on offer again. */
		CHECK(file_write(irq_path, "0\n"));
		CHECK_UINT(hooks->gpio_get(hooks->ctx, "slic_interrupt", &value),
			   0);
		CHECK_UINT(value, 0);
		CHECK(slic_host_irq_fd(h) >= 0);
		CHECK_UINT(fd_count(), fds + 1);
	}

	CASE("the clock hooks move forward and the delay hook sleeps");
	{
		uint32_t t0 = hooks->mono_ms(hooks->ctx);
		uint32_t t1;

		hooks->delay_ms(hooks->ctx, 20);
		t1 = hooks->mono_ms(hooks->ctx);
		CHECK(t1 - t0 >= 15);
		CHECK(t1 - t0 < 2000);
	}

	CASE("the semaphore hook takes and releases the section");
	CHECK_UINT(hooks->sem(hooks->ctx, 1), 0);
	CHECK_UINT(hooks->sem(hooks->ctx, 0), 0);

	CASE("spi_xfer refuses arguments it cannot honour");
	{
		uint8_t tx[3] = { 0, 0, 0 };

		CHECK_UINT(hooks->spi_xfer(hooks->ctx, NULL, NULL, 3), -EINVAL);
		CHECK_UINT(hooks->spi_xfer(hooks->ctx, tx, NULL, 0), -EINVAL);
		CHECK_UINT(hooks->spi_xfer(hooks->ctx, tx, NULL,
					   SLIC_HOST_XFER_MAX + 1), -EINVAL);
	}

	CASE("spi_xfer passes the kernel's errno straight through");
	{
		uint8_t tx[3] = { 0, 0, 0 };

		CHECK_UINT(hooks->spi_xfer(hooks->ctx, tx, NULL, 3), -ENOTTY);
	}

	CASE("slic_host_bus_set refuses no host and reports the ioctl errno");
	CHECK_UINT(slic_host_bus_set(NULL, 0, 0, 0, false), -EINVAL);
	CHECK_UINT(slic_host_bus_set(h, 3, 2000000, 2, false), -ENOTTY);

	CASE("a failed slic_host_bus_set leaves the previous bus state in place");
	{
		struct slic_host_bus bus;

		CHECK_UINT(slic_host_bus_get(NULL, &bus), -EINVAL);
		CHECK_UINT(slic_host_bus_get(h, NULL), -EINVAL);
		CHECK_UINT(slic_host_bus_get(h, &bus), 0);
		CHECK_UINT(bus.spi_mode, 0);
		CHECK_UINT(bus.speed_hz, 1000000);
		CHECK_UINT(bus.cs_split, 0);
		CHECK_UINT(bus.cs_per_byte, false);
		CHECK_UINT(slic_host_bus_set(h, 3, 2000000, 2, true), -ENOTTY);
		CHECK_UINT(slic_host_bus_get(h, &bus), 0);
		CHECK_UINT(bus.spi_mode, 0);
		CHECK_UINT(bus.speed_hz, 1000000);
		CHECK_UINT(bus.cs_split, 0);
		CHECK_UINT(bus.cs_per_byte, false);
	}

	CASE("spi_xfer reports the ioctl errno under per-byte framing");
	{
		struct slic_host_cfg bcfg = cfg;
		struct slic_host *hb = NULL;
		const struct udsp_slic_host *bhooks;
		uint8_t tx[SLIC_HOST_SEGS_MAX + 4];

		memset(tx, 0, sizeof(tx));
		bcfg.cs_per_byte = true;
		CHECK_UINT(slic_host_open_fd(&bcfg, fd, &hb), 0);
		bhooks = slic_host_hooks(hb);
		CHECK_UINT(bhooks->spi_xfer(bhooks->ctx, tx, NULL, sizeof(tx)),
			   -ENOTTY);
		slic_host_close(hb);
	}

	slic_host_close(h);
	h = NULL;

	CASE("slic_host_close releases the interrupt descriptor and the edge");
	CHECK_UINT(fd_count(), fds);
	{
		char edge_path[TPATH_MAX];

		CHECK(path_join(edge_path, sizeof(edge_path), dir,
				"pio12/edge"));
		CHECK(file_read(edge_path, text, sizeof(text)));
		CHECK(!strcmp(text, "none"));
	}

	CASE("a host without an interrupt export has no edge to arm");
	{
		struct slic_host_cfg ncfg = cfg;
		struct slic_host *hn = NULL;

		ncfg.gpio_interrupt = NULL;
		CHECK_UINT(slic_host_open_fd(&ncfg, fd, &hn), 0);
		CHECK_UINT(slic_host_irq_arm(hn), -ENODEV);
		slic_host_close(hn);
	}

	CASE("a closed descriptor reports EBADF, not a crash");
	CHECK_UINT(slic_host_open_fd(&cfg, fd, &h), 0);
	hooks = slic_host_hooks(h);
	close(fd);
	{
		uint8_t tx[3] = { 0, 0, 0 };

		CHECK_UINT(hooks->spi_xfer(hooks->ctx, tx, NULL, 3), -EBADF);
	}
	slic_host_close(h);

	CASE("slic_host_open rejects an empty configuration");
	CHECK_UINT(slic_host_open(NULL, &h), -EINVAL);
	CHECK_UINT(slic_host_open(&cfg, NULL), -EINVAL);
	CHECK_UINT(slic_host_open(&cfg, &h), -EINVAL);
	CHECK_UINT(slic_host_open_fd(&cfg, -1, &h), -EINVAL);

	CASE("slic_host_open reports why a spidev node would not open");
	cfg.spidev = "/nonexistent/spidev9.9";
	CHECK_UINT(slic_host_open(&cfg, &h), -ENOENT);

	CASE("slic_host_open reports a node that is not a spidev");
	cfg.spidev = "/dev/null";
	CHECK_UINT(slic_host_open(&cfg, &h), -ENOTTY);
}

/* The bench sweep drives these framings from proslic-probe --cs. The framing
 * is pure arithmetic, so the test checks it without a spidev. */
static void test_segs(void)
{
	struct spi_ioc_transfer tr[SLIC_HOST_SEGS_MAX];
	uint8_t tx[SLIC_HOST_SEGS_MAX + 1], rx[SLIC_HOST_SEGS_MAX + 1];
	size_t i, n;

	memset(tx, 0, sizeof(tx));
	memset(rx, 0, sizeof(rx));

	CASE("segs_build frames one CS over the whole buffer by default");
	n = slic_host_segs_build(tr, SLIC_HOST_SEGS_MAX, tx, rx, 3, 0, false,
				 SEG_SPEED, SEG_BITS);
	CHECK_UINT(n, 1);
	CHECK_UINT(tr[0].len, 3);
	CHECK_UINT(tr[0].cs_change, 0);
	CHECK_UINT(tr[0].tx_buf, (uintptr_t)tx);
	CHECK_UINT(tr[0].rx_buf, (uintptr_t)rx);
	CHECK_UINT(tr[0].speed_hz, SEG_SPEED);
	CHECK_UINT(tr[0].bits_per_word, SEG_BITS);

	CASE("segs_build breaks CS after the first cs_split bytes");
	n = slic_host_segs_build(tr, SLIC_HOST_SEGS_MAX, tx, rx, 3, 2, false,
				 SEG_SPEED, SEG_BITS);
	CHECK_UINT(n, 2);
	CHECK_UINT(tr[0].len, 2);
	CHECK_UINT(tr[1].len, 1);
	CHECK_UINT(tr[0].cs_change, 1);
	CHECK_UINT(tr[1].cs_change, 0);
	CHECK_UINT(tr[0].tx_buf, (uintptr_t)tx);
	CHECK_UINT(tr[1].tx_buf, (uintptr_t)(tx + 2));
	CHECK_UINT(tr[0].rx_buf, (uintptr_t)rx);
	CHECK_UINT(tr[1].rx_buf, (uintptr_t)(rx + 2));
	CHECK_UINT(tr[1].speed_hz, SEG_SPEED);
	CHECK_UINT(tr[1].bits_per_word, SEG_BITS);

	CASE("segs_build deasserts CS after every byte");
	n = slic_host_segs_build(tr, SLIC_HOST_SEGS_MAX, tx, rx, 3, 0, true,
				 SEG_SPEED, SEG_BITS);
	CHECK_UINT(n, 3);
	for (i = 0; i < 3; i++) {
		CHECK_UINT(tr[i].len, 1);
		CHECK_UINT(tr[i].cs_change, i < 2 ? 1 : 0);
		CHECK_UINT(tr[i].tx_buf, (uintptr_t)(tx + i));
		CHECK_UINT(tr[i].rx_buf, (uintptr_t)(rx + i));
	}

	CASE("cs_per_byte overrides a cs_split that would disagree");
	n = slic_host_segs_build(tr, SLIC_HOST_SEGS_MAX, tx, rx, 3, 2, true,
				 SEG_SPEED, SEG_BITS);
	CHECK_UINT(n, 3);
	CHECK_UINT(tr[0].len, 1);

	CASE("cs_split at or past the buffer length stays one segment");
	n = slic_host_segs_build(tr, SLIC_HOST_SEGS_MAX, tx, rx, 3, 3, false,
				 SEG_SPEED, SEG_BITS);
	CHECK_UINT(n, 1);
	CHECK_UINT(tr[0].len, 3);
	CHECK_UINT(tr[0].cs_change, 0);
	n = slic_host_segs_build(tr, SLIC_HOST_SEGS_MAX, tx, rx, 3, 9, false,
				 SEG_SPEED, SEG_BITS);
	CHECK_UINT(n, 1);
	CHECK_UINT(tr[0].len, 3);

	CASE("cs_split 1 of 2 splits after the first byte");
	n = slic_host_segs_build(tr, SLIC_HOST_SEGS_MAX, tx, rx, 2, 1, false,
				 SEG_SPEED, SEG_BITS);
	CHECK_UINT(n, 2);
	CHECK_UINT(tr[0].len, 1);
	CHECK_UINT(tr[1].len, 1);
	CHECK_UINT(tr[0].cs_change, 1);
	CHECK_UINT(tr[1].cs_change, 0);
	CHECK_UINT(tr[1].tx_buf, (uintptr_t)(tx + 1));
	CHECK_UINT(tr[1].rx_buf, (uintptr_t)(rx + 1));

	CASE("a NULL rx leaves every rx_buf zero");
	n = slic_host_segs_build(tr, SLIC_HOST_SEGS_MAX, tx, NULL, 3, 0, true,
				 SEG_SPEED, SEG_BITS);
	CHECK_UINT(n, 3);
	for (i = 0; i < 3; i++) {
		CHECK_UINT(tr[i].rx_buf, 0);
		CHECK_UINT(tr[i].tx_buf, (uintptr_t)(tx + i));
	}

	CASE("segs_build refuses an array too small for the framing");
	CHECK_UINT(slic_host_segs_build(tr, 2, tx, rx, 3, 0, true, SEG_SPEED,
					SEG_BITS), 0);
	CHECK_UINT(slic_host_segs_build(tr, 1, tx, rx, 3, 2, false, SEG_SPEED,
					SEG_BITS), 0);
	CHECK_UINT(slic_host_segs_build(tr, 0, tx, rx, 3, 0, false, SEG_SPEED,
					SEG_BITS), 0);
	CHECK_UINT(slic_host_segs_build(tr, SLIC_HOST_SEGS_MAX, tx, rx, 0, 0,
					false, SEG_SPEED, SEG_BITS), 0);
	CHECK_UINT(slic_host_segs_build(tr, SLIC_HOST_SEGS_MAX, NULL, rx, 3, 0,
					false, SEG_SPEED, SEG_BITS), 0);
	CHECK_UINT(slic_host_segs_build(NULL, SLIC_HOST_SEGS_MAX, tx, rx, 3, 0,
					false, SEG_SPEED, SEG_BITS), 0);

	CASE("per-byte framing chunks at the 16 segment ceiling");
	CHECK_UINT(slic_host_chunk_plan(SLIC_HOST_SEGS_MAX + 1, false),
		   SLIC_HOST_SEGS_MAX + 1);
	CHECK_UINT(slic_host_chunk_plan(SLIC_HOST_SEGS_MAX + 1, true),
		   SLIC_HOST_SEGS_MAX);
	CHECK_UINT(slic_host_chunk_plan(1, true), 1);
	CHECK_UINT(slic_host_segs_build(tr, SLIC_HOST_SEGS_MAX, tx, rx,
					SLIC_HOST_SEGS_MAX + 1, 0, true,
					SEG_SPEED, SEG_BITS), 0);
	n = slic_host_segs_build(tr, SLIC_HOST_SEGS_MAX, tx, rx,
				 SLIC_HOST_SEGS_MAX, 0, true, SEG_SPEED,
				 SEG_BITS);
	CHECK_UINT(n, SLIC_HOST_SEGS_MAX);
	CHECK_UINT(tr[0].cs_change, 1);
	CHECK_UINT(tr[SLIC_HOST_SEGS_MAX - 2].cs_change, 1);
	CHECK_UINT(tr[SLIC_HOST_SEGS_MAX - 1].cs_change, 0);
	CHECK_UINT(tr[SLIC_HOST_SEGS_MAX - 1].tx_buf,
		   (uintptr_t)(tx + SLIC_HOST_SEGS_MAX - 1));
	n = slic_host_segs_build(tr, SLIC_HOST_SEGS_MAX,
				 tx + SLIC_HOST_SEGS_MAX,
				 rx + SLIC_HOST_SEGS_MAX, 1, 0, true, SEG_SPEED,
				 SEG_BITS);
	CHECK_UINT(n, 1);
	CHECK_UINT(tr[0].len, 1);
	CHECK_UINT(tr[0].cs_change, 0);
	CHECK_UINT(tr[0].tx_buf, (uintptr_t)(tx + SLIC_HOST_SEGS_MAX));
	CHECK_UINT(tr[0].rx_buf, (uintptr_t)(rx + SLIC_HOST_SEGS_MAX));
}

struct msg_rec {
	size_t calls;
	size_t fail_on;
	size_t n[MSG_REC_MAX];
	uint64_t tx0[MSG_REC_MAX];
	uint64_t rx0[MSG_REC_MAX];
	uint8_t first_cs[MSG_REC_MAX];
	uint8_t last_cs[MSG_REC_MAX];
};

static int msg_record(void *ctx, struct spi_ioc_transfer *tr, size_t n)
{
	struct msg_rec *r = ctx;

	if (r->calls < MSG_REC_MAX) {
		r->n[r->calls] = n;
		r->tx0[r->calls] = tr[0].tx_buf;
		r->rx0[r->calls] = tr[0].rx_buf;
		r->first_cs[r->calls] = tr[0].cs_change;
		r->last_cs[r->calls] = tr[n - 1].cs_change;
	}
	r->calls++;
	return r->fail_on == r->calls ? -EIO : 0;
}

/* The production callback is the SPI_IOC_MESSAGE ioctl, which needs a spidev.
 * The test records the transfers instead. It checks the chunk arithmetic that
 * the 18-byte RAM write of the plug-in meets under per-byte framing
 * (glue/spi.c MPI_BATCH_MAX). */
static void test_xfer_run(void)
{
	uint8_t tx[2 * SLIC_HOST_SEGS_MAX + 2], rx[2 * SLIC_HOST_SEGS_MAX + 2];
	struct msg_rec r;

	memset(tx, 0, sizeof(tx));
	memset(rx, 0, sizeof(rx));

	CASE("the chunk loop sends 18 per-byte bytes as 16 then 2");
	memset(&r, 0, sizeof(r));
	CHECK_UINT(slic_host_xfer_run(msg_record, &r, tx, rx, 18, 0, true,
				      SEG_SPEED, SEG_BITS), 0);
	CHECK_UINT(r.calls, 2);
	CHECK_UINT(r.n[0], SLIC_HOST_SEGS_MAX);
	CHECK_UINT(r.n[1], 2);
	CHECK_UINT(r.tx0[0], (uintptr_t)tx);
	CHECK_UINT(r.rx0[0], (uintptr_t)rx);
	CHECK_UINT(r.tx0[1], (uintptr_t)(tx + SLIC_HOST_SEGS_MAX));
	CHECK_UINT(r.rx0[1], (uintptr_t)(rx + SLIC_HOST_SEGS_MAX));
	CHECK_UINT(r.first_cs[0], 1);
	CHECK_UINT(r.last_cs[0], 0);
	CHECK_UINT(r.first_cs[1], 1);
	CHECK_UINT(r.last_cs[1], 0);

	CASE("a NULL rx keeps every chunk write-only");
	memset(&r, 0, sizeof(r));
	CHECK_UINT(slic_host_xfer_run(msg_record, &r, tx, NULL, 18, 0, true,
				      SEG_SPEED, SEG_BITS), 0);
	CHECK_UINT(r.calls, 2);
	CHECK_UINT(r.rx0[0], 0);
	CHECK_UINT(r.rx0[1], 0);
	CHECK_UINT(r.tx0[1], (uintptr_t)(tx + SLIC_HOST_SEGS_MAX));

	CASE("a failing chunk propagates its errno and stops the loop");
	memset(&r, 0, sizeof(r));
	r.fail_on = 2;
	CHECK_UINT(slic_host_xfer_run(msg_record, &r, tx, rx, 34, 0, true,
				      SEG_SPEED, SEG_BITS), -EIO);
	CHECK_UINT(r.calls, 2);
	CHECK_UINT(r.n[1], SLIC_HOST_SEGS_MAX);

	CASE("one CS and cs_split framings stay a single message");
	memset(&r, 0, sizeof(r));
	CHECK_UINT(slic_host_xfer_run(msg_record, &r, tx, rx, 18, 0, false,
				      SEG_SPEED, SEG_BITS), 0);
	CHECK_UINT(r.calls, 1);
	CHECK_UINT(r.n[0], 1);
	memset(&r, 0, sizeof(r));
	CHECK_UINT(slic_host_xfer_run(msg_record, &r, tx, rx, 18, 2, false,
				      SEG_SPEED, SEG_BITS), 0);
	CHECK_UINT(r.calls, 1);
	CHECK_UINT(r.n[0], 2);
	CHECK_UINT(r.first_cs[0], 1);
	CHECK_UINT(r.last_cs[0], 0);

	CASE("the chunk loop refuses arguments it cannot honour");
	memset(&r, 0, sizeof(r));
	CHECK_UINT(slic_host_xfer_run(NULL, &r, tx, rx, 3, 0, false, SEG_SPEED,
				      SEG_BITS), -EINVAL);
	CHECK_UINT(slic_host_xfer_run(msg_record, &r, NULL, rx, 3, 0, false,
				      SEG_SPEED, SEG_BITS), -EINVAL);
	CHECK_UINT(slic_host_xfer_run(msg_record, &r, tx, rx, 0, 0, false,
				      SEG_SPEED, SEG_BITS), -EINVAL);
	CHECK_UINT(slic_host_xfer_run(msg_record, &r, tx, rx,
				      SLIC_HOST_XFER_MAX + 1, 0, false,
				      SEG_SPEED, SEG_BITS), -EINVAL);
	CHECK_UINT(r.calls, 0);
}

/*
 * The PCM data offset is a board fact that this module carries but never
 * reads; the plug-in adds it to every timeslot it programs. If the value does
 * not survive the trip from slic_host_cfg to the hook block, the audio is dead
 * and overdriven.
 *
 * Both open paths reach host_alloc, which makes the copy. slic_host_open_fd
 * can be driven to success without hardware. slic_host_open is driven to the
 * ioctl failure that follows its host_alloc, because its success needs a real
 * spidev. The test runs the bus through the slic_host_msg_fn message hook,
 * which the module provides to run the bus without a spidev.
 */
static void test_pcm_offset(void)
{
	const struct udsp_slic_host *hooks;
	struct slic_host_cfg cfg;
	struct slic_host_bus bus;
	struct slic_host *h = NULL;
	struct msg_rec r;
	uint8_t tx[6], rx[6];
	int fd = open("/dev/null", O_RDWR);

	CHECK(fd >= 0);

	CASE("a zeroed configuration leaves the PCM offset at zero");
	memset(&cfg, 0, sizeof(cfg));
	CHECK_UINT(slic_host_open_fd(&cfg, fd, &h), 0);
	hooks = slic_host_hooks(h);
	CHECK(hooks != NULL);
	CHECK_UINT(hooks->pcm_offset_bits, 0);
	slic_host_close(h);

	CASE("the PCM offset in the configuration reaches the hooks");
	cfg.pcm_offset_bits = 1;
	CHECK_UINT(slic_host_open_fd(&cfg, fd, &h), 0);
	hooks = slic_host_hooks(h);
	CHECK(hooks != NULL);
	CHECK_UINT(hooks->pcm_offset_bits, 1);

	/* The message hook the module offers for a bus run without a spidev. A
	 * transfer that really ran must not disturb a field it has no business
	 * touching. */
	CASE("a successful transfer through the message hook leaves the offset alone");
	memset(&r, 0, sizeof(r));
	memset(tx, 0xa5, sizeof(tx));
	CHECK_UINT(slic_host_xfer_run(msg_record, &r, tx, rx, sizeof(tx), 0,
				      true, SEG_SPEED, SEG_BITS), 0);
	CHECK(r.calls > 0);
	CHECK_UINT(slic_host_hooks(h)->pcm_offset_bits, 1);

	/* slic_host_bus_set issues two real SPI ioctls, so on this descriptor
	 * it fails; what the case pins is that a refused reconfiguration
	 * changes nothing, neither the mirrored bus state nor the board fact. */
	CASE("a refused bus reconfiguration disturbs neither the bus state nor the offset");
	CHECK_UINT(slic_host_bus_get(h, &bus), 0);
	CHECK_UINT(slic_host_bus_set(h, 3, 100000, 0, true), -ENOTTY);
	CHECK_UINT(slic_host_hooks(h)->pcm_offset_bits, 1);
	{
		struct slic_host_bus after;

		CHECK_UINT(slic_host_bus_get(h, &after), 0);
		CHECK_UINT(after.spi_mode, bus.spi_mode);
		CHECK_UINT(after.speed_hz, bus.speed_hz);
		CHECK_UINT(after.cs_per_byte, bus.cs_per_byte);
	}
	slic_host_close(h);

	/* The value passes through whole: this module must not clamp, default
	 * or otherwise interpret a board fact it does not own. */
	CASE("the offset passes through untouched, whatever it is");
	cfg.pcm_offset_bits = 15;
	CHECK_UINT(slic_host_open_fd(&cfg, fd, &h), 0);
	CHECK_UINT(slic_host_hooks(h)->pcm_offset_bits, 15);
	slic_host_close(h);
	cfg.pcm_offset_bits = 255;
	CHECK_UINT(slic_host_open_fd(&cfg, fd, &h), 0);
	CHECK_UINT(slic_host_hooks(h)->pcm_offset_bits, 255);
	slic_host_close(h);

	/* The other open path runs the same host_alloc and then configures the
	 * bus. /dev/null gets through the open and fails the first ioctl, so
	 * the path is exercised up to and past the copy, and it must hand back
	 * nothing rather than a half built host. */
	CASE("the open-by-path route runs the same allocation and cleans up after it");
	cfg.spidev = "/dev/null";
	cfg.pcm_offset_bits = 1;
	h = NULL;
	CHECK_UINT(slic_host_open(&cfg, &h), -ENOTTY);
	CHECK(h == NULL);

	close(fd);
}

/*
 * The wideband host fact. pcm_wideband travels like pcm_offset_bits: from
 * slic_host_cfg through host_alloc into the hook block that the plug-in reads
 * at init. It differs in one respect. The line rate settles after the attach,
 * so udsp_open must be able to clear the fact again on a degrade. The plug-in
 * must then see a zero at init. This module never interprets the value.
 */
static void test_pcm_wideband(void)
{
	const struct udsp_slic_host *hooks;
	struct slic_host_cfg cfg;
	struct slic_host *h = NULL;
	int fd = open("/dev/null", O_RDWR);

	CHECK(fd >= 0);

	CASE("a zeroed configuration asks for no wideband");
	memset(&cfg, 0, sizeof(cfg));
	CHECK_UINT(slic_host_open_fd(&cfg, fd, &h), 0);
	hooks = slic_host_hooks(h);
	CHECK(hooks != NULL);
	CHECK_UINT(hooks->pcm_wideband, 0);
	slic_host_close(h);

	CASE("the wideband ask in the configuration reaches the hooks");
	cfg.pcm_wideband = 1;
	CHECK_UINT(slic_host_open_fd(&cfg, fd, &h), 0);
	hooks = slic_host_hooks(h);
	CHECK(hooks != NULL);
	CHECK_UINT(hooks->pcm_wideband, 1);

	/* The degrade path: the settle refused the rate after the host was
	 * open, so the fact the plug-in will read at init has to go back to
	 * zero. */
	CASE("the settle can clear the wideband fact after the open");
	CHECK_UINT(slic_host_pcm_wideband_set(h, false), 0);
	CHECK_UINT(slic_host_hooks(h)->pcm_wideband, 0);
	CHECK_UINT(slic_host_pcm_wideband_set(h, true), 0);
	CHECK_UINT(slic_host_hooks(h)->pcm_wideband, 1);
	CHECK_UINT(slic_host_pcm_wideband_set(NULL, false), -EINVAL);

	/* Neither fact owns the other: clearing one leaves the other alone. */
	CASE("clearing the wideband fact leaves the PCM offset alone");
	cfg.pcm_offset_bits = 1;
	slic_host_close(h);
	CHECK_UINT(slic_host_open_fd(&cfg, fd, &h), 0);
	CHECK_UINT(slic_host_hooks(h)->pcm_offset_bits, 1);
	CHECK_UINT(slic_host_pcm_wideband_set(h, false), 0);
	CHECK_UINT(slic_host_hooks(h)->pcm_offset_bits, 1);
	CHECK_UINT(slic_host_hooks(h)->pcm_wideband, 0);
	slic_host_close(h);

	/* The value passes through whole, as the offset does. */
	CASE("the wideband ask passes through untouched, whatever it is");
	cfg.pcm_wideband = 255;
	CHECK_UINT(slic_host_open_fd(&cfg, fd, &h), 0);
	CHECK_UINT(slic_host_hooks(h)->pcm_wideband, 255);
	slic_host_close(h);

	close(fd);
}

struct log_rec {
	unsigned calls;
	void *ctx;
	enum udsp_slic_log_level level;
	char msg[64];
};

static void log_record(void *ctx, enum udsp_slic_log_level level, const char *msg)
{
	struct log_rec *r = ctx;

	r->calls++;
	r->ctx = ctx;
	r->level = level;
	snprintf(r->msg, sizeof(r->msg), "%s", msg);
}

/*
 * The log hook travels like the two board facts: from slic_host_cfg through
 * host_alloc into the hook block. It differs in one respect. An absent sink
 * must leave the hook NULL, because the plug-in reads the pointer to choose
 * between the host and its own fallback.
 */
static void test_log_hook(void)
{
	const struct udsp_slic_host *hooks;
	struct slic_host_cfg cfg;
	struct slic_host *h = NULL;
	struct log_rec r;
	int fd = open("/dev/null", O_RDWR);

	CHECK(fd >= 0);

	CASE("a configuration without a sink leaves the log hook NULL");
	memset(&cfg, 0, sizeof(cfg));
	CHECK_UINT(slic_host_open_fd(&cfg, fd, &h), 0);
	hooks = slic_host_hooks(h);
	CHECK(hooks != NULL);
	CHECK(hooks->log == NULL);
	slic_host_close(h);

	CASE("a sink reaches the hook with its level, text and context");
	memset(&r, 0, sizeof(r));
	cfg.log = log_record;
	cfg.log_ctx = &r;
	CHECK_UINT(slic_host_open_fd(&cfg, fd, &h), 0);
	hooks = slic_host_hooks(h);
	CHECK(hooks != NULL);
	CHECK(hooks->log != NULL);
	hooks->log(hooks->ctx, UDSP_SLIC_LOG_WARN, "chan 0 declined wideband");
	CHECK_UINT(r.calls, 1);
	CHECK(r.ctx == &r);
	CHECK_UINT(r.level, UDSP_SLIC_LOG_WARN);
	CHECK(!strcmp(r.msg, "chan 0 declined wideband"));
	hooks->log(hooks->ctx, UDSP_SLIC_LOG_DEBUG, "");
	CHECK_UINT(r.calls, 2);
	CHECK_UINT(r.level, UDSP_SLIC_LOG_DEBUG);
	CHECK_UINT(strlen(r.msg), 0);
	slic_host_close(h);

	close(fd);
}

static void test_plugin_load(void)
{
	char so[TPATH_MAX], err[SLIC_PLUGIN_ERR_MAX];
	struct slic_plugin *p = NULL;

	CASE("the loader refuses a call with no path");
	CHECK_UINT(slic_plugin_load(NULL, &p, err, sizeof(err)), -EINVAL);
	CHECK_UINT(slic_plugin_load("", &p, err, sizeof(err)), -EINVAL);
	CHECK_UINT(slic_plugin_load("/x.so", NULL, err, sizeof(err)), -EINVAL);

	CASE("a plug-in file that is not there is ENOENT and says so");
	err[0] = '\0';
	CHECK_UINT(slic_plugin_load("/nonexistent/plugin.so", &p, err,
				    sizeof(err)), -ENOENT);
	CHECK(strlen(err) > 0);

	CASE("a shared object without the descriptor symbol is ENOEXEC");
	CHECK(stub_build_empty(so, sizeof(so)));
	err[0] = '\0';
	CHECK_UINT(slic_plugin_load(so, &p, err, sizeof(err)), -ENOEXEC);
	CHECK(strstr(err, "udsp_slic_descriptor") != NULL);

	/*
	 * The version gate is an inequality, not a floor: a plug-in of an older
	 * ABI is as wrong as one of a newer ABI, because the two sides disagree
	 * about the layout either way. The cases cover both sides, so a gate
	 * that only refuses newer plug-ins fails here.
	 */
	CASE("a plug-in from an older ABI is EPROTO and hands back nothing");
	CHECK(stub_build("oldabi", UDSP_SLIC_ABI_VERSION - 1,
			 sizeof(struct udsp_slic_ops), true, so, sizeof(so)));
	err[0] = '\0';
	p = NULL;
	CHECK_UINT(slic_plugin_load(so, &p, err, sizeof(err)), -EPROTO);
	CHECK(p == NULL);

	CASE("the older ABI complaint names what it got and what it wanted");
	{
		char want[64];

		snprintf(want, sizeof(want), "abi %u, expected %u",
			 (unsigned)UDSP_SLIC_ABI_VERSION - 1,
			 (unsigned)UDSP_SLIC_ABI_VERSION);
		CHECK(strstr(err, want) != NULL);
	}

	CASE("a plug-in from a newer ABI is EPROTO and hands back nothing");
	CHECK(stub_build("newabi", UDSP_SLIC_ABI_VERSION + 1,
			 sizeof(struct udsp_slic_ops), true, so, sizeof(so)));
	err[0] = '\0';
	p = NULL;
	CHECK_UINT(slic_plugin_load(so, &p, err, sizeof(err)), -EPROTO);
	CHECK(p == NULL);

	CASE("the newer ABI complaint names what it got and what it wanted");
	{
		char want[64];

		snprintf(want, sizeof(want), "abi %u, expected %u",
			 (unsigned)UDSP_SLIC_ABI_VERSION + 1,
			 (unsigned)UDSP_SLIC_ABI_VERSION);
		CHECK(strstr(err, want) != NULL);
	}

	CASE("a descriptor whose ops struct is a different size is EMSGSIZE");
	CHECK(stub_build("badsize", UDSP_SLIC_ABI_VERSION,
			 sizeof(struct udsp_slic_ops) / 2, true, so,
			 sizeof(so)));
	CHECK_UINT(slic_plugin_load(so, &p, err, sizeof(err)), -EMSGSIZE);

	CASE("a descriptor with no backend name is ENOSYS");
	CHECK(stub_build_named("noname", UDSP_SLIC_ABI_VERSION,
			       sizeof(struct udsp_slic_ops), true, false, so,
			       sizeof(so)));
	err[0] = '\0';
	CHECK_UINT(slic_plugin_load(so, &p, err, sizeof(err)), -ENOSYS);
	CHECK(strstr(err, "name") != NULL);

	CASE("a descriptor missing a mandatory op is ENOSYS");
	CHECK(stub_build("noops", UDSP_SLIC_ABI_VERSION,
			 sizeof(struct udsp_slic_ops), false, so, sizeof(so)));
	CHECK_UINT(slic_plugin_load(so, &p, err, sizeof(err)), -ENOSYS);

	CASE("a well formed plug-in loads and hands back its ops");
	CHECK(stub_build("good", UDSP_SLIC_ABI_VERSION,
			 sizeof(struct udsp_slic_ops), true, so, sizeof(so)));
	err[0] = '\0';
	CHECK_UINT(slic_plugin_load(so, &p, err, sizeof(err)), 0);
	CHECK(p != NULL);
	CHECK_UINT(strlen(err), 0);
	CHECK(slic_plugin_ops(p) != NULL);
	CHECK(slic_plugin_ops(p)->attach != NULL);
	CHECK(slic_plugin_ops(p)->safe_xfer != NULL);
	CHECK(slic_plugin_ops(p)->mwi_lamp == NULL);
	CHECK(!strcmp(slic_plugin_ops(p)->name, "stub"));

	CASE("a plug-in that reports no part name still loads");
	CHECK(slic_plugin_ops(p)->part_name == NULL);

	CASE("unload is safe on a loaded plug-in and on nothing");
	slic_plugin_unload(p);
	slic_plugin_unload(NULL);
	CHECK(slic_plugin_ops(NULL) == NULL);
}

int main(void)
{
	char tmpl[] = "/tmp/udsp-slic-host-XXXXXX";
	char cmd[TPATH_MAX + 32];

	if (!mkdtemp(tmpl)) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(tmpdir, sizeof(tmpdir), "%s", tmpl);

	test_gpio();
	test_pcm_offset();
	test_pcm_wideband();
	test_log_hook();
	test_segs();
	test_xfer_run();
	test_plugin_load();

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
	if (system(cmd) != 0)
		fprintf(stderr, "# could not remove %s\n", tmpdir);

	return tu_summary("test_slic_host");
}
