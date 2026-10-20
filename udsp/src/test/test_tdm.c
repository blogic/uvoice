// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Unit tests for the file TDM backend (tdm/file.c). The alsa-lib backend
 * (tdm/alsa.c) needs hardware, so it runs on a target board and not in this
 * suite.
 *
 * Three cases build the device bytes by hand with le16_put, not through the
 * conversion helpers of the backend. The write case compares the file with
 * those bytes, and the two read cases read them back. A round trip through
 * both helpers therefore cannot pass when both helpers are wrong in the same
 * way. The bytes follow the S16_LE order of the raw files, so the cases assume
 * a little-endian host. The CI host and both targets are little-endian.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "udsp/tdm.h"
#include "util.h"

#define T_FRAMES 80u
#define T_CHANS  2u
#define T_NSAMP  (T_FRAMES * T_CHANS)
#define T_NBYTE  (T_NSAMP * sizeof(int16_t))

static const struct udsp_tdm_geom t_want = {
	.frames_per_period = T_FRAMES,
	.channels = T_CHANS,
	.prefill_periods = 3,
	.layout = UDSP_TDM_INTERLEAVED,
};

/* 10 ms at 16 kHz: the wideband geometry. */
#define T_FRAMES_WB 160u

static const struct udsp_tdm_geom t_want_wb = {
	.frames_per_period = T_FRAMES_WB,
	.channels = T_CHANS,
	.prefill_periods = 3,
	.layout = UDSP_TDM_INTERLEAVED,
	.rate = 16000,
};

static const struct udsp_tdm_geom t_want_nonint = {
	.frames_per_period = T_FRAMES,
	.channels = T_CHANS,
	.prefill_periods = 3,
	.layout = UDSP_TDM_NONINTERLEAVED,
};

static void tmp_path(char *out)
{
	strcpy(out, "/tmp/udsp_tdm_XXXXXX");
	int fd = mkstemp(out);
	if (fd >= 0)
		close(fd);
}

static void write_period_file(const char *path, const int16_t *planar,
			      const struct udsp_tdm_geom *want)
{
	struct udsp_tdm_geom got = { 0 };
	struct udsp_tdm *w = udsp_tdm_file.open(path, NULL, want, &got, NULL);
	if (!w)
		return;
	udsp_tdm_file.write_period(w, planar);
	udsp_tdm_file.close(w);
}

static void fixture_write(const char *path, const unsigned char *bytes,
			  size_t len)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fwrite(bytes, 1, len, f);
	fclose(f);
}

static size_t file_read_all(const char *path, unsigned char *out, size_t max)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	size_t n = fread(out, 1, max, f);
	fclose(f);
	return n;
}

static void le16_put(unsigned char *p, int16_t v)
{
	p[0] = (unsigned char)((uint16_t)v & 0xff);
	p[1] = (unsigned char)(((uint16_t)v >> 8) & 0xff);
}

int main(void)
{
	CASE("file open fills a matching got-geometry");
	{
		char path[64];
		tmp_path(path);
		struct udsp_tdm_geom got = { 0 };
		struct udsp_tdm *t = udsp_tdm_file.open(NULL, path, &t_want, &got, NULL);
		CHECK(t != NULL);
		if (t) {
			CHECK_UINT(got.frames_per_period, t_want.frames_per_period);
			CHECK_UINT(got.channels, t_want.channels);
			CHECK_UINT(got.prefill_periods, t_want.prefill_periods);
			CHECK_UINT(got.layout, t_want.layout);
			udsp_tdm_file.close(t);
		}
		unlink(path);
	}

	CASE("file grants the requested rate at both rates");
	{
		char path[64];
		struct udsp_tdm_geom got = { 0 };
		struct udsp_tdm *t;
		struct udsp_tdm_geom want_nb = t_want;

		/* An untouched want asks for the rate zero. The caller and
		 * the back end both read that zero as 8000 (tdm.h); the grant
		 * reports the resolved value so the engine compares two real
		 * numbers. */
		tmp_path(path);
		t = udsp_tdm_file.open(NULL, path, &t_want, &got, NULL);
		CHECK(t != NULL);
		if (t) {
			CHECK_UINT(got.rate, 8000);
			CHECK_UINT(got.frames_per_period, T_FRAMES);
			udsp_tdm_file.close(t);
		}

		want_nb.rate = 8000;
		memset(&got, 0, sizeof(got));
		t = udsp_tdm_file.open(NULL, path, &want_nb, &got, NULL);
		CHECK(t != NULL);
		if (t) {
			CHECK_UINT(got.rate, 8000);
			CHECK_UINT(got.frames_per_period, T_FRAMES);
			udsp_tdm_file.close(t);
		}

		memset(&got, 0, sizeof(got));
		t = udsp_tdm_file.open(NULL, path, &t_want_wb, &got, NULL);
		CHECK(t != NULL);
		if (t) {
			CHECK_UINT(got.rate, 16000);
			CHECK_UINT(got.frames_per_period, T_FRAMES_WB);
			CHECK_UINT(got.channels, T_CHANS);
			udsp_tdm_file.close(t);
		}
		unlink(path);
	}

	CASE("file moves a whole wideband period per tick");
	{
		char path[64];
		const unsigned nsamp = T_FRAMES_WB * T_CHANS;
		int16_t tx[T_FRAMES_WB * T_CHANS], rx[T_FRAMES_WB * T_CHANS];
		struct udsp_tdm_geom got = { 0 };
		struct udsp_tdm *t;

		tmp_path(path);
		for (unsigned f = 0; f < T_FRAMES_WB; f++) {
			tx[f] = (int16_t)(0x0700 + (int)f);
			tx[T_FRAMES_WB + f] = (int16_t)(-0x0700 + (int)f);
		}
		write_period_file(path, tx, &t_want_wb);
		{
			struct stat sb;

			CHECK(stat(path, &sb) == 0);
			CHECK_UINT((uint64_t)sb.st_size,
				   (uint64_t)(nsamp * sizeof(int16_t)));
		}

		memset(rx, 0, sizeof(rx));
		t = udsp_tdm_file.open(NULL, path, &t_want_wb, &got, NULL);
		CHECK(t != NULL);
		if (t) {
			CHECK(udsp_tdm_file.read_period(t, rx) == 0);
			CHECK(memcmp(tx, rx, sizeof(tx)) == 0);
			CHECK(udsp_tdm_file.avail(t) == 0);
			udsp_tdm_file.close(t);
		}
		unlink(path);
	}

	CASE("file round-trip preserves planar samples");
	{
		char path[64];
		tmp_path(path);
		int16_t tx[T_NSAMP], rx[T_NSAMP];
		for (unsigned f = 0; f < T_FRAMES; f++) {
			tx[f] = (int16_t)(1000 + (int)f);
			tx[T_FRAMES + f] = (int16_t)(-3000 + (int)f);
		}
		write_period_file(path, tx, &t_want);

		memset(rx, 0, sizeof(rx));
		struct udsp_tdm_geom got = { 0 };
		struct udsp_tdm *r = udsp_tdm_file.open(NULL, path, &t_want, &got, NULL);
		CHECK(r != NULL);
		if (r) {
			CHECK(udsp_tdm_file.read_period(r, rx) == 0);
			CHECK(memcmp(tx, rx, sizeof(tx)) == 0);
			udsp_tdm_file.close(r);
		}
		unlink(path);
	}

	CASE("file period count matches prefill plus ticks");
	{
		char path[64];
		tmp_path(path);
		int16_t silent[T_NSAMP];
		memset(silent, 0, sizeof(silent));
		const unsigned prefill = t_want.prefill_periods;
		const unsigned ticks = 5;
		struct udsp_tdm_geom got = { 0 };
		struct udsp_tdm *w = udsp_tdm_file.open(path, NULL, &t_want, &got, NULL);
		CHECK(w != NULL);
		if (w) {
			for (unsigned i = 0; i < prefill; i++)
				CHECK(udsp_tdm_file.write_period(w, silent) == 0);
			CHECK(udsp_tdm_file.start(w) == 0);
			for (unsigned i = 0; i < ticks; i++)
				CHECK(udsp_tdm_file.write_period(w, silent) == 0);
			CHECK(udsp_tdm_file.stop(w) == 0);
			struct stat st;
			CHECK(stat(path, &st) == 0);
			CHECK_UINT((uint64_t)st.st_size,
				   (uint64_t)((prefill + ticks) * T_NBYTE));
			udsp_tdm_file.close(w);
		}
		unlink(path);
	}

	CASE("file recover after short read stays usable");
	{
		char path[64];
		tmp_path(path);
		int16_t one[T_NSAMP], rx[T_NSAMP];
		for (unsigned i = 0; i < T_NSAMP; i++)
			one[i] = (int16_t)(i + 1);
		write_period_file(path, one, &t_want);

		struct udsp_tdm_geom got = { 0 };
		struct udsp_tdm *r = udsp_tdm_file.open(NULL, path, &t_want, &got, NULL);
		CHECK(r != NULL);
		if (r) {
			CHECK(udsp_tdm_file.read_period(r, rx) == 0);
			CHECK(memcmp(one, rx, sizeof(one)) == 0);
			CHECK(udsp_tdm_file.read_period(r, rx) == -EPIPE);
			CHECK(udsp_tdm_file.recover(r) == 0);
			memset(rx, 0, sizeof(rx));
			CHECK(udsp_tdm_file.read_period(r, rx) == 0);
			CHECK(memcmp(one, rx, sizeof(one)) == 0);
			udsp_tdm_file.close(r);
		}
		unlink(path);
	}

	CASE("file keeps channel 0 and channel 1 distinct");
	{
		char path[64];
		tmp_path(path);
		int16_t tx[T_NSAMP], rx[T_NSAMP];
		const int16_t a = 0x0101, b = 0x0202;
		for (unsigned f = 0; f < T_FRAMES; f++) {
			tx[f] = a;
			tx[T_FRAMES + f] = b;
		}
		write_period_file(path, tx, &t_want);

		memset(rx, 0, sizeof(rx));
		struct udsp_tdm_geom got = { 0 };
		struct udsp_tdm *r = udsp_tdm_file.open(NULL, path, &t_want, &got, NULL);
		CHECK(r != NULL);
		if (r) {
			CHECK(udsp_tdm_file.read_period(r, rx) == 0);
			bool ch0_ok = true, ch1_ok = true;
			for (unsigned f = 0; f < T_FRAMES; f++) {
				if (rx[f] != a)
					ch0_ok = false;
				if (rx[T_FRAMES + f] != b)
					ch1_ok = false;
			}
			CHECK(ch0_ok);
			CHECK(ch1_ok);
			udsp_tdm_file.close(r);
		}
		unlink(path);
	}

	CASE("file write emits interleaved device bytes");
	{
		char path[64];
		tmp_path(path);
		int16_t tx[T_NSAMP];
		unsigned char want_bytes[T_NBYTE], got_bytes[T_NBYTE];
		for (unsigned f = 0; f < T_FRAMES; f++) {
			tx[f] = (int16_t)(0x1000 + (int)f);
			tx[T_FRAMES + f] = (int16_t)(0x2000 + (int)f);
		}
		for (unsigned f = 0; f < T_FRAMES; f++) {
			le16_put(&want_bytes[f * 4], tx[f]);
			le16_put(&want_bytes[f * 4 + 2], tx[T_FRAMES + f]);
		}
		write_period_file(path, tx, &t_want);

		memset(got_bytes, 0, sizeof(got_bytes));
		CHECK_UINT(file_read_all(path, got_bytes, sizeof(got_bytes)),
			   (uint64_t)T_NBYTE);
		CHECK(memcmp(want_bytes, got_bytes, sizeof(want_bytes)) == 0);
		unlink(path);
	}

	CASE("file read converts interleaved device bytes to planar");
	{
		char path[64];
		tmp_path(path);
		unsigned char raw[T_NBYTE];
		int16_t want_planar[T_NSAMP], rx[T_NSAMP];
		for (unsigned f = 0; f < T_FRAMES; f++) {
			int16_t c0 = (int16_t)(0x4000 + (int)f);
			int16_t c1 = (int16_t)(-0x4000 + (int)f);
			le16_put(&raw[f * 4], c0);
			le16_put(&raw[f * 4 + 2], c1);
			want_planar[f] = c0;
			want_planar[T_FRAMES + f] = c1;
		}
		fixture_write(path, raw, sizeof(raw));

		memset(rx, 0, sizeof(rx));
		struct udsp_tdm_geom got = { 0 };
		struct udsp_tdm *r = udsp_tdm_file.open(NULL, path, &t_want, &got, NULL);
		CHECK(r != NULL);
		if (r) {
			CHECK(udsp_tdm_file.read_period(r, rx) == 0);
			CHECK(memcmp(want_planar, rx, sizeof(want_planar)) == 0);
			udsp_tdm_file.close(r);
		}
		unlink(path);
	}

	CASE("file noninterleaved layout keeps device planar order");
	{
		char path[64];
		tmp_path(path);
		unsigned char raw[T_NBYTE];
		int16_t want_planar[T_NSAMP], rx[T_NSAMP];
		for (unsigned f = 0; f < T_FRAMES; f++) {
			want_planar[f] = (int16_t)(0x0500 + (int)f);
			want_planar[T_FRAMES + f] = (int16_t)(0x0900 + (int)f);
		}
		for (unsigned i = 0; i < T_NSAMP; i++)
			le16_put(&raw[i * 2], want_planar[i]);
		fixture_write(path, raw, sizeof(raw));

		memset(rx, 0, sizeof(rx));
		struct udsp_tdm_geom got = { 0 };
		struct udsp_tdm *r = udsp_tdm_file.open(NULL, path, &t_want_nonint,
							&got, NULL);
		CHECK(r != NULL);
		if (r) {
			CHECK_UINT(got.layout, UDSP_TDM_NONINTERLEAVED);
			CHECK(udsp_tdm_file.read_period(r, rx) == 0);
			CHECK(memcmp(want_planar, rx, sizeof(want_planar)) == 0);
			udsp_tdm_file.close(r);
		}
		unlink(path);
	}

	CASE("file delay reports queued playback frames");
	{
		char path[64];
		tmp_path(path);
		int16_t silent[T_NSAMP], rx[T_NSAMP];
		memset(silent, 0, sizeof(silent));
		const unsigned prefill = t_want.prefill_periods;
		const long want_delay = (long)(prefill * T_FRAMES);
		struct udsp_tdm_geom got = { 0 };
		struct udsp_tdm *t = udsp_tdm_file.open(path, NULL, &t_want, &got, NULL);
		CHECK(t != NULL);
		if (t) {
			CHECK(udsp_tdm_file.delay(t) == 0);
			for (unsigned i = 0; i < prefill; i++)
				CHECK(udsp_tdm_file.write_period(t, silent) == 0);
			CHECK(udsp_tdm_file.delay(t) == want_delay);
			CHECK(udsp_tdm_file.start(t) == 0);
			for (unsigned i = 0; i < 5; i++) {
				CHECK(udsp_tdm_file.read_period(t, rx) == 0);
				CHECK(udsp_tdm_file.write_period(t, silent) == 0);
			}
			CHECK(udsp_tdm_file.delay(t) == want_delay);
			udsp_tdm_file.close(t);
		}
		unlink(path);
	}

	CASE("file delay drains on a tick without a write");
	{
		char path[64];
		tmp_path(path);
		int16_t silent[T_NSAMP], rx[T_NSAMP];
		memset(silent, 0, sizeof(silent));
		struct udsp_tdm_geom got = { 0 };
		struct udsp_tdm *t = udsp_tdm_file.open(path, NULL, &t_want, &got, NULL);
		CHECK(t != NULL);
		if (t) {
			CHECK(udsp_tdm_file.write_period(t, silent) == 0);
			CHECK(udsp_tdm_file.write_period(t, silent) == 0);
			CHECK(udsp_tdm_file.start(t) == 0);
			CHECK(udsp_tdm_file.read_period(t, rx) == 0);
			CHECK(udsp_tdm_file.delay(t) == (long)T_FRAMES);
			CHECK(udsp_tdm_file.stop(t) == 0);
			CHECK(udsp_tdm_file.delay(t) == 0);
			CHECK(udsp_tdm_file.write_period(t, silent) == 0);
			CHECK(udsp_tdm_file.recover(t) == 0);
			CHECK(udsp_tdm_file.delay(t) == 0);
			udsp_tdm_file.close(t);
		}
		unlink(path);
	}

	CASE("file open records why it refused");
	{
		struct udsp_tdm_geom got = { 0 };
		struct udsp_tdm_fail fail = { 0 };
		struct udsp_tdm_geom bad = t_want;

		/* A missing capture file is not the device's geometry answer. */
		CHECK(udsp_tdm_file.open(NULL, "/nonexistent/udsp-test.raw",
					 &t_want, &got, &fail) == NULL);
		CHECK(!fail.geometry);
		CHECK(fail.err == -ENOENT);
		CHECK(fail.stream == UDSP_TDM_DIR_CAPTURE);
		CHECK(fail.what && !strcmp(fail.what, "open"));

		memset(&fail, 0, sizeof(fail));
		bad.channels = 3;
		CHECK(udsp_tdm_file.open(NULL, NULL, &bad, &got, &fail) == NULL);
		CHECK(!fail.geometry);
		CHECK(fail.err == -EINVAL);
		CHECK(fail.stream == UDSP_TDM_DIR_NONE);
		CHECK(fail.what && !strcmp(fail.what, "geometry request"));

		/* A NULL record is allowed. */
		CHECK(udsp_tdm_file.open(NULL, NULL, &bad, &got, NULL) == NULL);
	}

	return tu_summary("tdm");
}
