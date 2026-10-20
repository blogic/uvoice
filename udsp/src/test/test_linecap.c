// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * udsp-linecap runs as a child on the file backend with the fake SLIC. The
 * binary under test is a private build of tools/udsp-linecap.c against the
 * source region data (CMakeLists). Its path arrives as argv[1]. The suite
 * therefore runs with UDSP_TOOLS off, and never depends on an installed tool.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "udsp/udsp.h"

#include "util.h"

#define TL_OUTPUT_MAX	65536u
#define TL_ARGS_MAX	32u
#define TL_HEADER_V1_BYTES	16u
#define TL_HEADERLESS_BYTES	0u

static const uint8_t tl_header_v1[TL_HEADER_V1_BYTES] = {
	'U', 'D', 'S', 'P', 'L', 'C', 'A', 'P',
	1, 0, 16, 0, 0x40, 0x1f, 0, 0,
};

_Static_assert(TL_HEADERLESS_BYTES == 0u,
	       "a headerless capture carries no header bytes");

static const char *tl_binary;

struct tl_result {
	int exit_code;
	char output[TL_OUTPUT_MAX];
};

/* Runs the tool with the arguments after argv[0] and keeps both streams. */
static bool tl_run(const char *const *args, struct tl_result *result)
{
	char path[] = "/tmp/udsp-test-linecap-XXXXXX";
	const char *argv[TL_ARGS_MAX + 2];
	unsigned n = 0;
	int fd, status = 0;
	ssize_t got;
	pid_t pid;

	memset(result, 0, sizeof(*result));
	result->exit_code = -1;
	argv[n++] = tl_binary;
	while (args[n - 1] && n < TL_ARGS_MAX + 1)
		argv[n] = args[n - 1], n++;
	argv[n] = NULL;

	fd = mkstemp(path);
	if (fd < 0)
		return false;
	unlink(path);
	fflush(NULL);
	pid = fork();
	if (!pid) {
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		close(fd);
		execv(tl_binary, (char *const *)argv);
		_exit(127);
	}
	if (pid < 0) {
		close(fd);
		return false;
	}
	if (waitpid(pid, &status, 0) < 0) {
		close(fd);
		return false;
	}
	got = pread(fd, result->output, sizeof(result->output) - 1u, 0);
	close(fd);
	if (got < 0)
		return false;
	result->output[got] = '\0';
	if (WIFEXITED(status))
		result->exit_code = WEXITSTATUS(status);
	return true;
}

static bool tl_has(const struct tl_result *r, const char *text)
{
	return strstr(r->output, text) != NULL;
}

static bool tl_uint(const struct tl_result *r, const char *name, uint64_t *value)
{
	const char *text = strstr(r->output, name);
	char *end = NULL;
	unsigned long long parsed;

	if (!text)
		return false;
	errno = 0;
	parsed = strtoull(text + strlen(name), &end, 10);
	if (errno || end == text + strlen(name))
		return false;
	*value = parsed;
	return true;
}

static uint64_t tl_file_size(const char *path)
{
	struct stat st;

	if (stat(path, &st))
		return 0;
	return (uint64_t)st.st_size;
}

static bool tl_file_prefix(const char *path, uint8_t *out, size_t count)
{
	FILE *file = fopen(path, "rb");
	bool ok;

	if (!file)
		return false;
	ok = fread(out, 1, count, file) == count;
	fclose(file);
	return ok;
}

static bool tl_file_has_bytes(const char *path, const uint8_t *bytes,
			      size_t count)
{
	FILE *file = fopen(path, "rb");
	size_t matched = 0;
	int value;

	if (!file)
		return false;
	while ((value = fgetc(file)) != EOF) {
		if ((uint8_t)value == bytes[matched])
			matched++;
		else
			matched = (uint8_t)value == bytes[0] ? 1u : 0u;
		if (matched != count)
			continue;
		fclose(file);
		return true;
	}
	fclose(file);
	return false;
}

static void usage_cases(void)
{
	struct tl_result r;

	CASE("the binary pins the headerless and version 1 capture shapes");
	{
		CHECK_UINT(TL_HEADERLESS_BYTES, 0);
		CHECK(tl_file_has_bytes(tl_binary, tl_header_v1,
					sizeof(tl_header_v1)));
	}

	CASE("no verb exits 2 with the usage");
	{
		const char *const args[] = { NULL };

		CHECK(tl_run(args, &r));
		CHECK_UINT(r.exit_code, 2);
		CHECK(tl_has(&r, "a verb is required"));
		CHECK(tl_has(&r, "usage: udsp-linecap"));
	}

	CASE("an unknown verb exits 2");
	{
		const char *const args[] = { "dance", NULL };

		CHECK(tl_run(args, &r));
		CHECK_UINT(r.exit_code, 2);
		CHECK(tl_has(&r, "unknown verb: dance"));
	}

	CASE("cid without an identity exits 2");
	{
		const char *const args[] = { "cid", "--tdm-backend", "file", NULL };

		CHECK(tl_run(args, &r));
		CHECK_UINT(r.exit_code, 2);
		CHECK(tl_has(&r, "cid requires --cid-name or --cid-number"));
	}

	CASE("vmwi without --mwi exits 2, and --mwi elsewhere exits 2");
	{
		const char *const args[] = { "vmwi", "--tdm-backend", "file", NULL };
		const char *const args2[] = { "idle", "--mwi", "on", NULL };

		CHECK(tl_run(args, &r));
		CHECK_UINT(r.exit_code, 2);
		CHECK(tl_has(&r, "vmwi requires --mwi on or off"));
		CHECK(tl_run(args2, &r));
		CHECK_UINT(r.exit_code, 2);
		CHECK(tl_has(&r, "--mwi applies to vmwi alone"));
	}

	CASE("an out of range option names its range");
	{
		const char *const args[] = { "idle", "--cid-wait-ms", "5001", NULL };

		CHECK(tl_run(args, &r));
		CHECK_UINT(r.exit_code, 2);
		CHECK(tl_has(&r, "--cid-wait-ms: expected 0..5000"));
	}

	CASE("an unknown caller identity format is refused");
	{
		const char *const args[] = { "cid", "--cid-number", "1",
					     "--cid-format", "morse", NULL };

		CHECK(tl_run(args, &r));
		CHECK_UINT(r.exit_code, 2);
		CHECK(tl_has(&r, "unknown format \"morse\""));
	}

	CASE("--help exits 0");
	{
		const char *const args[] = { "--help", NULL };

		CHECK(tl_run(args, &r));
		CHECK_UINT(r.exit_code, 0);
		CHECK(tl_has(&r, "verbs:"));
	}
}

static void capture_cases(void)
{
	char out[] = "/tmp/udsp-test-linecap-cap-XXXXXX";
	struct tl_result r;
	uint64_t samples = 0, packets = 0, lost = 0;
	uint8_t header[TL_HEADER_V1_BYTES];
	int fd = mkstemp(out);

	if (fd < 0) {
		CASE("capture file");
		CHECK(false);
		return;
	}
	close(fd);

	/*
	 * The file back end captures its synthetic ramp at one 20 ms packet
	 * per two ticks, so the sample count is the engine time the stream
	 * ran: at least settle plus the idle length plus the completion
	 * drain. The engine is unpaced there and keeps ticking through every
	 * control step, so the count overshoots by whatever the host took;
	 * only the floor and the packet accounting are exact.
	 */
	CASE("the writer emits the exact version 1 header before its payload");
	{
		const char *const args[] = { "idle", "--tdm-backend", "file",
					     "--duration", "1000", "--settle-ms",
					     "500", "--out", out, NULL };

		CHECK(tl_run(args, &r));
		CHECK_UINT(r.exit_code, 0);
		CHECK(tl_has(&r, "result = ok"));
		CHECK(tl_uint(&r, "capture_samples = ", &samples));
		CHECK(tl_uint(&r, "capture_packets = ", &packets));
		CHECK(tl_uint(&r, "capture_lost = ", &lost));
		/* 500 + 1000 + 700 ms of engine time at 8 kHz, less two
		 * packets for the stream start and stop edges. */
		CHECK(samples >= 2200u * 8u - 320u);
		CHECK_UINT(lost, 0);
		CHECK_UINT(samples, packets * 160u);
		CHECK_UINT(tl_file_size(out), samples + TL_HEADER_V1_BYTES);
		CHECK(tl_file_prefix(out, header, sizeof(header)));
		CHECK(!memcmp(header, tl_header_v1, sizeof(header)));
	}

	CASE("ring with caller identity completes the record and the cadence");
	{
		const char *const args[] = { "ring", "--tdm-backend", "file",
					     "--duration", "7000", "--region", "USA",
					     "--cid-name", "TEST CALLER",
					     "--cid-number", "5551234",
					     "--settle-ms", "200", "--out", out,
					     NULL };

		CHECK(tl_run(args, &r));
		CHECK_UINT(r.exit_code, 0);
		CHECK(tl_has(&r, "result = ok"));
		CHECK(tl_has(&r, "RING_START"));
		CHECK(tl_has(&r, "CID_SENT line=0 kind=TYPE1 status=0"));
		CHECK(tl_has(&r, "RING_END line=0 state=EXPIRED ring_count=2"));
		CHECK(tl_uint(&r, "capture_samples = ", &samples));
		CHECK(samples > 7000u * 8u);
	}

	CASE("vmwi on completes the record without a ring");
	{
		const char *const args[] = { "vmwi", "--mwi", "on",
					     "--tdm-backend", "file", "--region", "USA",
					     "--settle-ms", "200", "--out", out,
					     NULL };

		CHECK(tl_run(args, &r));
		CHECK_UINT(r.exit_code, 0);
		CHECK(tl_has(&r, "CID_SENT line=0 kind=TYPE1 status=0"));
	}

	CASE("tone completes on AUDIO_END");
	{
		const char *const args[] = { "tone", "--tdm-backend", "file",
					     "--duration", "500",
					     "--region", "USA", "--settle-ms",
					     "200", "--out", out, NULL };

		CHECK(tl_run(args, &r));
		CHECK_UINT(r.exit_code, 0);
		CHECK(tl_has(&r, "AUDIO_END"));
		CHECK(tl_has(&r, "result = ok"));
	}

	CASE("a line past the capability cap exits 1 before any capture");
	{
		const char *const args[] = { "idle", "--tdm-backend", "file",
					     "--line", "7", "--out", out, NULL };

		CHECK(tl_run(args, &r));
		CHECK_UINT(r.exit_code, 1);
		CHECK(tl_has(&r, "--line 7 is outside cap.lines"));
	}
	unlink(out);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: test_linecap <udsp-linecap binary>\n");
		return 2;
	}
	tl_binary = argv[1];
	usage_cases();
	capture_cases();
	return tu_summary("linecap");
}
