// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The #define main embed exposes the CLI internals that this suite tests.
 * Each full CLI call runs in a child, because process state survives the
 * embed.
 *
 * A case that needs a quiet gap uses a preloaded record and an immediate
 * bound, not a real wait, so the run stays short. Two cases submit seq 0
 * records on purpose. They check the diagnostic and the summary that those
 * records produce.
 */
int udsp_cli_embedded_main(int argc, char **argv);

#define main udsp_cli_embedded_main
#include "../tools/udsp-cli.c"
#undef main

#include <fcntl.h>
#include <sys/wait.h>

#include "region_edit.h"
#include "util.h"

#define TC_OUTPUT_MAX		131072u
#define TC_CHILD_TIMEOUT_MS	15000u
#define TC_REGION_FILE		UDSP_REGION_DIR "/USA.conf"
#define TC_GBR_REGION_FILE	UDSP_REGION_DIR "/GBR.conf"
#define TC_AUS_REGION_FILE	UDSP_REGION_DIR "/AUS.conf"
#define TC_NZL_REGION_FILE	UDSP_REGION_DIR "/NZL.conf"

struct tc_cli_result {
	int exit_code;
	bool timed_out;
	char output[TC_OUTPUT_MAX];
};

static int tc_error_mute(void);
static void tc_error_restore(int saved);

static uint64_t tc_mono_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);

	return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void tc_cli_run(int argc, char **argv, unsigned timeout_ms,
		       struct tc_cli_result *result)
{
	char path[] = "/tmp/udsp-test-cli-output-XXXXXX";
	struct timespec nap = { .tv_sec = 0, .tv_nsec = 1000000L };
	uint64_t deadline;
	pid_t pid;
	int status = -1;
	int fd;
	ssize_t got;

	memset(result, 0, sizeof(*result));
	result->exit_code = -1;
	fd = mkstemp(path);
	if (fd < 0)
		return;
	unlink(path);
	fflush(NULL);
	pid = fork();
	if (!pid) {
		int result_code;

		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		close(fd);
		optind = 0;
		tool_stopped = 0;
		opterr = 1;
		result_code = udsp_cli_embedded_main(argc, argv);
		fflush(NULL);
		_exit(result_code);
	}
	if (pid < 0) {
		close(fd);
		return;
	}

	deadline = tc_mono_ms() + timeout_ms;
	for (;;) {
		pid_t waited = waitpid(pid, &status, WNOHANG);

		if (waited == pid)
			break;
		if (waited < 0)
			break;
		if (tc_mono_ms() >= deadline) {
			result->timed_out = true;
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			break;
		}
		nanosleep(&nap, NULL);
	}
	if (!result->timed_out && status != -1 && WIFEXITED(status))
		result->exit_code = WEXITSTATUS(status);
	got = pread(fd, result->output, sizeof(result->output) - 1u, 0);
	if (got > 0)
		result->output[got] = '\0';
	close(fd);
}

static int tc_capture_start(int target_fd, FILE *stream, int *saved)
{
	char path[] = "/tmp/udsp-test-cli-capture-XXXXXX";
	int fd;

	*saved = -1;
	fd = mkstemp(path);
	if (fd < 0)
		return -1;
	unlink(path);
	fflush(stream);
	*saved = dup(target_fd);
	if (*saved < 0 || dup2(fd, target_fd) < 0) {
		if (*saved >= 0)
			close(*saved);
		close(fd);
		*saved = -1;
		return -1;
	}

	return fd;
}

static bool tc_capture_stop(int fd, int target_fd, FILE *stream, int saved,
			    char *output, size_t size)
{
	ssize_t got;
	bool restored;

	fflush(stream);
	restored = dup2(saved, target_fd) >= 0;
	close(saved);
	got = pread(fd, output, size - 1u, 0);
	close(fd);
	if (got < 0)
		return false;
	output[got] = '\0';

	return restored;
}

static bool tc_temp_write(char *path, size_t path_size, const char *text)
{
	char name[] = "/tmp/udsp-test-cli-region-XXXXXX";
	ssize_t size = (ssize_t)strlen(text);
	int fd = mkstemp(name);
	bool ok;

	if (fd < 0)
		return false;
	ok = write(fd, text, (size_t)size) == size;
	close(fd);
	if (!ok) {
		unlink(name);
		return false;
	}
	if (snprintf(path, path_size, "%s", name) < 0 ||
	    strlen(name) >= path_size) {
		unlink(name);
		return false;
	}

	return true;
}

static bool tc_region_change_write(char *path, size_t path_size,
				   const char *find, const char *replace)
{
	const struct region_edit edit = { NULL, find, replace };
	char text[REGION_EDIT_TEXT_MAX];

	if (!region_text_edit(TC_REGION_FILE, text, sizeof(text), &edit, 1))
		return false;

	return tc_temp_write(path, path_size, text);
}

static bool tc_output_has(const struct tc_cli_result *result, const char *text)
{
	return strstr(result->output, text) != NULL;
}

static bool tc_output_uint(const struct tc_cli_result *result, const char *name,
			   uint64_t *value)
{
	const char *text = strstr(result->output, name);
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

static enum tool_parse tc_args_parse(int argc, char **argv, struct cli_args *args)
{
	enum tool_parse result;
	int saved;

	args_defaults(args);
	args->verb = verb_find(argv[0]);
	if (!args->verb)
		return TOOL_PARSE_ERROR;
	optind = 0;
	opterr = 0;
	saved = tc_error_mute();
	result = args_parse(argc, argv, args);
	tc_error_restore(saved);

	return result;
}

static bool tc_verb_args_valid(const struct cli_args *args)
{
	bool result;
	int saved = tc_error_mute();

	result = verb_args_valid(args);
	tc_error_restore(saved);

	return result;
}

static int tc_output_mute(void)
{
	int saved;
	int sink;

	fflush(stdout);
	saved = dup(STDOUT_FILENO);
	sink = open("/dev/null", O_WRONLY);
	if (saved < 0 || sink < 0) {
		if (saved >= 0)
			close(saved);
		if (sink >= 0)
			close(sink);
		return -1;
	}
	if (dup2(sink, STDOUT_FILENO) < 0) {
		close(saved);
		close(sink);
		return -1;
	}
	close(sink);

	return saved;
}

static void tc_output_restore(int saved)
{
	if (saved < 0)
		return;
	fflush(stdout);
	dup2(saved, STDOUT_FILENO);
	close(saved);
}

static int tc_error_mute(void)
{
	int saved;
	int sink;

	fflush(stderr);
	saved = dup(STDERR_FILENO);
	sink = open("/dev/null", O_WRONLY);
	if (saved < 0 || sink < 0) {
		if (saved >= 0)
			close(saved);
		if (sink >= 0)
			close(sink);
		return -1;
	}
	if (dup2(sink, STDERR_FILENO) < 0) {
		close(saved);
		close(sink);
		return -1;
	}
	close(sink);

	return saved;
}

static void tc_error_restore(int saved)
{
	if (saved < 0)
		return;
	fflush(stderr);
	dup2(saved, STDERR_FILENO);
	close(saved);
}

static uint16_t tc_seq_next(uint16_t *seq)
{
	(*seq)++;
	if (!*seq)
		*seq = 1;

	return *seq;
}

static void tc_parse_cases(void)
{
	struct cli_args args;

	CASE("parse rejects a separate leading-dash option value");
	{
		char *argv[] = { "call", "--playback", "-device" };

		CHECK_UINT(tc_args_parse(3, argv, &args), TOOL_PARSE_ERROR);
	}

	CASE("parse accepts a leading-dash value in name-equals form");
	{
		char *argv[] = { "call", "--playback=-device" };

		CHECK_UINT(tc_args_parse(2, argv, &args), TOOL_PARSE_OK);
		CHECK(!strcmp(args.base.cfg.tdm_playback, "-device"));
	}

	CASE("parse accepts a dash in a path component");
	{
		char *argv[] = { "call", "--playback",
				 "/tmp/root/-sandbox/device" };

		CHECK_UINT(tc_args_parse(3, argv, &args), TOOL_PARSE_OK);
		CHECK(!strcmp(args.base.cfg.tdm_playback,
			      "/tmp/root/-sandbox/device"));
	}

	CASE("parse accepts cid-at-ring lower bound 0");
	{
		char *argv[] = { "ring", "--cid-at-ring", "0" };

		CHECK_UINT(tc_args_parse(3, argv, &args), TOOL_PARSE_OK);
		CHECK_UINT(args.base.cid_at_ring, 0);
	}

	CASE("parse accepts cid-at-ring upper bound 7");
	{
		char *argv[] = { "ring", "--cid-at-ring", "7" };

		CHECK_UINT(tc_args_parse(3, argv, &args), TOOL_PARSE_OK);
		CHECK_UINT(args.base.cid_at_ring, 7);
	}

	CASE("parse rejects cid-at-ring 8");
	{
		char *argv[] = { "ring", "--cid-at-ring", "8" };

		CHECK_UINT(tc_args_parse(3, argv, &args), TOOL_PARSE_ERROR);
	}

	CASE("parse accepts cid-wait-ms upper bound 5000");
	{
		char *argv[] = { "ring", "--cid-wait-ms", "5000" };

		CHECK_UINT(tc_args_parse(3, argv, &args), TOOL_PARSE_OK);
		CHECK_UINT(args.base.cid_wait_ms, 5000);
	}

	CASE("parse rejects cid-wait-ms 5001");
	{
		char *argv[] = { "ring", "--cid-wait-ms", "5001" };

		CHECK_UINT(tc_args_parse(3, argv, &args), TOOL_PARSE_ERROR);
	}

	CASE("validation rejects ring timing options on events and call");
	{
		char *events_argv[] = { "events", "--cid-at-ring", "1" };
		char *call_argv[] = { "call", "--cid-at-ring", "1" };

		CHECK_UINT(tc_args_parse(3, events_argv, &args), TOOL_PARSE_OK);
		CHECK(!tc_verb_args_valid(&args));
		CHECK_UINT(tc_args_parse(3, call_argv, &args), TOOL_PARSE_OK);
		CHECK(!tc_verb_args_valid(&args));
	}

	CASE("parse rejects type2 with a DTMF caller-id format");
	{
		char *argv[] = { "cid", "--type2", "--cid-format", "dtmf" };

		CHECK_UINT(tc_args_parse(4, argv, &args), TOOL_PARSE_ERROR);
	}

	CASE("parse rejects ports below 1");
	{
		char *argv[] = { "events", "--ports", "0" };

		CHECK_UINT(tc_args_parse(3, argv, &args), TOOL_PARSE_ERROR);
	}

	CASE("parse accepts the cw variant upper bound");
	{
		char *argv[] = { "cw", "--variant", "4" };

		CHECK_UINT(tc_args_parse(3, argv, &args), TOOL_PARSE_OK);
		CHECK_UINT(args.variant, 4);
	}

	CASE("parse rejects a cw variant above the bound");
	{
		char *argv[] = { "cw", "--variant", "5" };

		CHECK_UINT(tc_args_parse(3, argv, &args), TOOL_PARSE_ERROR);
	}

	CASE("validation rejects a variant on a non-cw verb");
	{
		char *argv[] = { "tone", "--tone", "call_waiting", "--region",
				 "USA", "--variant", "1" };

		CHECK_UINT(tc_args_parse(7, argv, &args), TOOL_PARSE_OK);
		CHECK(!tc_verb_args_valid(&args));
	}

	CASE("validation requires a region for cw");
	{
		char *argv[] = { "cw", "--variant", "2" };

		CHECK_UINT(tc_args_parse(3, argv, &args), TOOL_PARSE_OK);
		CHECK(!tc_verb_args_valid(&args));
	}
}

static void tc_ring_timing_cases(void)
{
	struct cli_args args;
	struct tc_cli_result result;

	CASE("ring timing without caller identity exits 1");
	{
		char *argv[] = { "udsp-cli", "ring", "--tdm-backend", "file",
				 "--seconds", "1", "--cid-at-ring", "7" };

		tc_cli_run(8, argv, TC_CHILD_TIMEOUT_MS, &result);
		CHECK(!result.timed_out);
		CHECK_UINT(result.exit_code, CLI_EXIT_ERROR);
		CHECK(tc_output_has(&result,
			"ring timing option requires --cid-name or --cid-number"));
	}

	CASE("ring timing with caller identity is accepted");
	{
		char *argv[] = { "ring", "--cid-at-ring", "7", "--cid-name",
				 "Test", "--region", TC_REGION_FILE };

		CHECK_UINT(tc_args_parse(7, argv, &args), TOOL_PARSE_OK);
		CHECK(tc_verb_args_valid(&args));
	}
}

static void tc_ring_fill_cases(void)
{
	struct udsp_region region;
	struct udsp_ring ring;
	struct cli_args args;
	char output[512];
	int saved;
	int fd;

	CASE("ring uses the GBR r1 double cadence");
	{
		char *argv[] = { "ring", "--region", TC_GBR_REGION_FILE };

		CHECK_UINT(tc_args_parse(3, argv, &args), TOOL_PARSE_OK);
		CHECK(region_preparse(&args, &region));
		fd = tc_capture_start(STDOUT_FILENO, stdout, &saved);
		CHECK(fd >= 0);
		if (fd >= 0) {
			ring_fill(&args, &region, &ring);
			CHECK(tc_capture_stop(fd, STDOUT_FILENO, stdout, saved,
					      output, sizeof(output)));
			CHECK_UINT(ring.cadence_count, 2);
			CHECK_UINT(ring.cadence[0].on_ms, 400);
			CHECK_UINT(ring.cadence[0].off_ms, 200);
			CHECK_UINT(ring.cadence[1].on_ms, 400);
			CHECK_UINT(ring.cadence[1].off_ms, 2000);
			CHECK(strstr(output, "ring cadence on_ms=400 off_ms=200 "
				     "on_ms=400 off_ms=2000 source=region r1") != NULL);
		}
	}

	CASE("ring uses the USA r1 cadence");
	{
		char *argv[] = { "ring", "--region", TC_REGION_FILE };

		CHECK_UINT(tc_args_parse(3, argv, &args), TOOL_PARSE_OK);
		CHECK(region_preparse(&args, &region));
		fd = tc_capture_start(STDOUT_FILENO, stdout, &saved);
		CHECK(fd >= 0);
		if (fd >= 0) {
			ring_fill(&args, &region, &ring);
			CHECK(tc_capture_stop(fd, STDOUT_FILENO, stdout, saved,
					      output, sizeof(output)));
			CHECK_UINT(ring.cadence_count, 1);
			CHECK_UINT(ring.cadence[0].on_ms, 2000);
			CHECK_UINT(ring.cadence[0].off_ms, 4000);
			CHECK(strstr(output, "ring cadence on_ms=2000 off_ms=4000 "
				     "source=region r1") != NULL);
		}
	}

	CASE("ring without a region uses the built-in cadence");
	{
		char *argv[] = { "ring" };

		CHECK_UINT(tc_args_parse(1, argv, &args), TOOL_PARSE_OK);
		memset(&region, 0, sizeof(region));
		fd = tc_capture_start(STDOUT_FILENO, stdout, &saved);
		CHECK(fd >= 0);
		if (fd >= 0) {
			ring_fill(&args, &region, &ring);
			CHECK(tc_capture_stop(fd, STDOUT_FILENO, stdout, saved,
					      output, sizeof(output)));
			CHECK_UINT(ring.cadence_count, 1);
			CHECK_UINT(ring.cadence[0].on_ms, 2000);
			CHECK_UINT(ring.cadence[0].off_ms, 4000);
			CHECK(strstr(output, "ring cadence on_ms=2000 off_ms=4000 "
				     "source=built-in") != NULL);
			CHECK(strstr(output, "ring warning=") == NULL);
		}
	}

	CASE("ring reports a failed region parse with its path");
	{
		char path[128];
		char *argv[] = { "ring", "--region", path };
		char errors[4096];
		char message[512];
		bool parsed;
		bool made;

		/* cutting r1 alone leaves r2..r8, so the gap rule is the
		 * failure this file produces */
		made = tc_region_change_write(path, sizeof(path),
			"[ring r1]\ncadence = 2000 4000\n\n", "");
		CHECK(made);
		if (made) {
			CHECK_UINT(tc_args_parse(3, argv, &args), TOOL_PARSE_OK);
			fd = tc_capture_start(STDERR_FILENO, stderr, &saved);
			CHECK(fd >= 0);
			if (fd >= 0) {
				parsed = region_preparse(&args, &region);
				CHECK(tc_capture_stop(fd, STDERR_FILENO, stderr,
						      saved, errors, sizeof(errors)));
				CHECK(!parsed);
				snprintf(message, sizeof(message),
					 "udsp-cli: region parse failed: %s: "
					 "ring templates must run from r1 "
					 "with no gap", path);
				CHECK(strstr(errors, message) != NULL);
				CHECK(strstr(errors, "usage: udsp-cli") == NULL);
			}
			unlink(path);
		}
	}

	CASE("ring copies the region r1 first cadence");
	{
		char path[128];
		char *argv[] = { "ring", "--region", path };
		bool made;

		made = tc_region_change_write(path, sizeof(path),
			"[ring r1]\ncadence = 2000 4000",
			"[ring r1]\ncadence = 700 800 900 1000\n"
			"first = 1100 1200 1300 1400");
		CHECK(made);
		if (made) {
			CHECK_UINT(tc_args_parse(3, argv, &args), TOOL_PARSE_OK);
			CHECK(region_preparse(&args, &region));
			fd = tc_capture_start(STDOUT_FILENO, stdout, &saved);
			CHECK(fd >= 0);
			if (fd >= 0) {
				ring_fill(&args, &region, &ring);
				CHECK(tc_capture_stop(fd, STDOUT_FILENO, stdout,
						      saved, output, sizeof(output)));
					CHECK_UINT(ring.cadence_count, 2);
				CHECK_UINT(ring.cadence[0].on_ms, 700);
				CHECK_UINT(ring.cadence[0].off_ms, 800);
				CHECK_UINT(ring.cadence[1].on_ms, 900);
				CHECK_UINT(ring.cadence[1].off_ms, 1000);
				CHECK_UINT(ring.first_count, 2);
				CHECK_UINT(ring.first[0].on_ms, 1100);
				CHECK_UINT(ring.first[0].off_ms, 1200);
				CHECK_UINT(ring.first[1].on_ms, 1300);
				CHECK_UINT(ring.first[1].off_ms, 1400);
			}
			unlink(path);
		}
	}
}

static unsigned tc_socket_feed(int fd, unsigned limit)
{
	struct udsp_event event = { .type = UDSP_EV_LINE };
	uint16_t seq = 0;
	unsigned count = 0;

	while (count < limit) {
		event.seq = tc_seq_next(&seq);
		if (send(fd, &event, sizeof(event), MSG_DONTWAIT | MSG_NOSIGNAL) !=
		    (ssize_t)sizeof(event))
			break;
		count++;
	}

	return count;
}

static void tc_wait_cases(void)
{
	struct cli_engine engine;
	struct udsp_event event;
	int pair[2];
	int saved;

	CASE("signal precedes reached deadlines and a queued flood");
	memset(&engine, 0, sizeof(engine));
	CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair));
	if (!tu_bad) {
		struct udsp fake = { 0 };
		unsigned fed;

		engine.base.s = &fake;
		engine.base.event_fd = pair[0];
		fed = tc_socket_feed(pair[1], 20000);
		CHECK(fed > 0);
		if (fed > 0) {
			atomic_store(&fake.clock.tick, 2u);
			tool_stopped = 1;
			saved = tc_output_mute();
			CHECK_UINT(tool_event_wait(&engine.base, 20,
						   tool_mono_ms(), true,
						   &event), TOOL_WAIT_STOPPED);
			tc_output_restore(saved);
			tool_stopped = 0;
			CHECK(recv(pair[0], &event, sizeof(event), MSG_DONTWAIT) ==
			      (ssize_t)sizeof(event));
			CHECK_UINT(event.seq, 1);
		}
		close(pair[0]);
		close(pair[1]);
	}

	CASE("empty wait reports a reached wall deadline");
	memset(&engine, 0, sizeof(engine));
	CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair));
	if (!tu_bad) {
		engine.base.event_fd = pair[0];
		CHECK_UINT(tool_event_wait(&engine.base, 0, tool_mono_ms(),
					   false, &event),
			   TOOL_WAIT_WALL_EXPIRED);
		close(pair[0]);
		close(pair[1]);
	}

	CASE("event expiry precedes a reached wall deadline");
	memset(&engine, 0, sizeof(engine));
	CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair));
	if (!tu_bad) {
		struct udsp fake = { 0 };

		engine.base.s = &fake;
		engine.base.event_fd = pair[0];
		atomic_store(&fake.clock.tick, 2u);
		CHECK_UINT(tool_event_wait(&engine.base, 20, tool_mono_ms(),
					   false, &event),
			   TOOL_WAIT_EXPIRED);
		close(pair[0]);
		close(pair[1]);
	}

	CASE("a compressed quiet gap ends before its wall deadline");
	memset(&engine, 0, sizeof(engine));
	CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair));
	if (!tu_bad) {
		struct udsp_event sent = {
			.seq = 1,
			.type = UDSP_EV_LINE,
		};
		ssize_t sent_size;

		/* The quiet gap is compressed into a preloaded record and a
		 * one-millisecond bound, without a scheduler-dependent sleep. */
		sent_size = send(pair[1], &sent, sizeof(sent), MSG_NOSIGNAL);
		CHECK(sent_size == (ssize_t)sizeof(sent));
		if (sent_size == (ssize_t)sizeof(sent)) {
			engine.base.event_fd = pair[0];
			saved = tc_output_mute();
			CHECK_UINT(tool_event_wait(&engine.base, 0,
						   tool_mono_ms() + 1, false,
						   &event), TOOL_WAIT_GOT);
			tc_output_restore(saved);
			CHECK_UINT(event.seq, 1);
		}
		close(pair[0]);
		close(pair[1]);
	}

	CASE("an already-read event precedes event-clock expiry");
	memset(&engine, 0, sizeof(engine));
	CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, pair));
	if (!tu_bad) {
		struct udsp fake = { 0 };
		struct udsp_event queued = {
			.seq = 1,
			.type = UDSP_EV_LINE,
		};

		engine.base.s = &fake;
		engine.base.event_fd = pair[0];
		ssize_t sent = send(pair[1], &queued, sizeof(queued), MSG_NOSIGNAL);

		CHECK(sent == (ssize_t)sizeof(queued));
		if (sent == (ssize_t)sizeof(queued)) {
			saved = tc_output_mute();
			CHECK_UINT(tool_event_wait(&engine.base, 20, 0, false,
						   &event), TOOL_WAIT_GOT);
			tc_output_restore(saved);
			CHECK_UINT(event.seq, 1);

			queued.seq = 2;
			sent = send(pair[1], &queued, sizeof(queued), MSG_NOSIGNAL);
			CHECK(sent == (ssize_t)sizeof(queued));
			if (sent == (ssize_t)sizeof(queued)) {
				atomic_store(&fake.clock.tick, 2u);
				CHECK_UINT(tool_event_wait(&engine.base, 20, 0,
							   false, &event),
					   TOOL_WAIT_EXPIRED);
				CHECK(recv(pair[0], &event, sizeof(event),
					   MSG_DONTWAIT) == (ssize_t)sizeof(event));
				CHECK_UINT(event.seq, 2);
			}
		}
		close(pair[0]);
		close(pair[1]);
	}
}

static void tc_seq_walk(struct cli_engine *engine, uint64_t total,
			uint64_t drop_every)
{
	struct udsp_event event = { .type = UDSP_EV_LINE };
	uint16_t emitter = 0;

	for (uint64_t i = 1; i <= total; i++) {
		event.seq = tc_seq_next(&emitter);
		if (drop_every && !(i % drop_every))
			continue;
		event_seq_check(engine, &event);
	}
}

static void tc_seq_cases(void)
{
	struct cli_engine engine;
	struct udsp_event event = { .type = UDSP_EV_LINE };
	char errors[4096];
	int saved;

	CASE("lossless sequence replay crosses seven wraps without loss");
	memset(&engine, 0, sizeof(engine));
	saved = tc_error_mute();
	tc_seq_walk(&engine, 500000, 0);
	tc_error_restore(saved);
	CHECK_UINT(engine.events_lost, 0);

	CASE("sequence replay counts exactly 511 dropped records");
	memset(&engine, 0, sizeof(engine));
	saved = tc_error_mute();
	tc_seq_walk(&engine, 500000, 977);
	tc_error_restore(saved);
	CHECK_UINT(engine.events_lost, 511);

	CASE("sequence gap straddle 65534 to 2 counts 2");
	memset(&engine, 0, sizeof(engine));
	engine.seq_valid = true;
	engine.seq = 65534;
	event.seq = 2;
	saved = tc_error_mute();
	event_seq_check(&engine, &event);
	tc_error_restore(saved);
	CHECK_UINT(engine.events_lost, 2);

	CASE("sequence gap straddle 65530 to 3 counts 7");
	memset(&engine, 0, sizeof(engine));
	engine.seq_valid = true;
	engine.seq = 65530;
	event.seq = 3;
	saved = tc_error_mute();
	event_seq_check(&engine, &event);
	tc_error_restore(saved);
	CHECK_UINT(engine.events_lost, 7);

	CASE("five sequence-zero records preserve a fresh baseline");
	memset(&engine, 0, sizeof(engine));
	event.seq = 0;
	{
		const char *note = "synthetic/illegal LINE record has seq 0";
		int error_saved;
		int error_fd = tc_capture_start(STDERR_FILENO, stderr,
						&error_saved);

		CHECK(error_fd >= 0);
		if (error_fd >= 0) {
			const char *first;

			for (unsigned i = 0; i < 5; i++)
				event_seq_check(&engine, &event);
			CHECK(tc_capture_stop(error_fd, STDERR_FILENO, stderr,
					      error_saved, errors, sizeof(errors)));
			first = strstr(errors, note);
			CHECK(first != NULL);
			if (first)
				CHECK(strstr(first + strlen(note), note) == NULL);
		}
	}
	CHECK(!engine.seq_valid);
	CHECK_UINT(engine.seq, 0);
	CHECK_UINT(engine.events_lost, 0);
	CHECK_UINT(engine.seq_zero, 5);
}

static void tc_full_cli_cases(void)
{
	struct tc_cli_result result;

	CASE("full cli rejects a line at the reported capability cap");
	{
		char *argv[] = { "udsp-cli", "events", "--tdm-backend", "file",
				 "--seconds", "1", "--line", "2" };

		tc_cli_run(8, argv, TC_CHILD_TIMEOUT_MS, &result);
		CHECK(!result.timed_out);
		CHECK_UINT(result.exit_code, CLI_EXIT_ERROR);
		CHECK(tc_output_has(&result, "--line 2 is outside cap.lines 2"));
	}

	CASE("region prints the parsed profile in the grammar's form");
	{
		char *argv[] = { "udsp-cli", "region", "--region", TC_REGION_FILE };

		tc_cli_run(4, argv, TC_CHILD_TIMEOUT_MS, &result);
		CHECK(!result.timed_out);
		CHECK_UINT(result.exit_code, CLI_EXIT_OK);
		CHECK(tc_output_has(&result, "[region]\niso3 = USA\n"
				    "describe = United States\n"));
		CHECK(tc_output_has(&result, "[tone call_waiting]\nfreq = 440\n"
				    "level = -13\ncadence = 300 10000\nrepeat = 2\n"));
		CHECK(tc_output_has(&result, "[ring r1]\ncadence = 2000 4000\n"));
		CHECK(tc_output_has(&result, "[cw c1]\nfreq = 440\n"
				    "level = -13\ncadence = 300 10000\nrepeat = 2\n"));
		CHECK(tc_output_has(&result, "[cw c4]\nfreq = 440\n"
				    "level = -13\ncadence = 100 100 300 100 100 10000\n"
				    "repeat = 2\n"));
		CHECK(tc_output_has(&result, "format = bellcore-fsk\n"
				    "mwi_format = bellcore-fsk\nwait_ms = 500\n"));
		CHECK(tc_output_has(&result, "pulse_interdigit_ms = 300\n"
				    "flash_service = drop\n"));
		CHECK(tc_output_has(&result, "[hook]\npulse_enable = on\n"
				    "pulse_invert = off\ndebounce_ms = 10\n"));
		CHECK(!tc_output_has(&result, "[defaults]"));
	}

	CASE("the printed profile carries no header and parses again");
	{
		char *argv[] = { "udsp-cli", "region", "--region", TC_REGION_FILE };
		struct line_region_error err = {};
		struct udsp_region again;

		tc_cli_run(4, argv, TC_CHILD_TIMEOUT_MS, &result);
		CHECK(!result.timed_out);
		CHECK_UINT(result.exit_code, CLI_EXIT_OK);
		CHECK(!tc_output_has(&result, "verb = "));
		CHECK_UINT((unsigned)-line_region_parse(result.output,
							strlen(result.output),
							&again, &err), 0);
		CHECK(!strcmp(again.iso3, "USA"));
		CHECK_UINT(again.flash_service, UDSP_FLASH_SERVICE_DROP);
		CHECK_UINT(again.tone[UDSP_TONE_CALL_WAITING].repeat_count, 2);
		CHECK_UINT(again.cw_count, 4);
		CHECK(!again.hook.pulse_invert);
	}

	CASE("the printed NZL profile keeps pulse_invert on and parses again");
	{
		char *argv[] = { "udsp-cli", "region", "--region",
				 TC_NZL_REGION_FILE };
		struct line_region_error err = {};
		struct udsp_region again;

		tc_cli_run(4, argv, TC_CHILD_TIMEOUT_MS, &result);
		CHECK(!result.timed_out);
		CHECK_UINT(result.exit_code, CLI_EXIT_OK);
		CHECK(tc_output_has(&result, "[hook]\npulse_enable = on\n"
				    "pulse_invert = on\n"));
		CHECK_UINT((unsigned)-line_region_parse(result.output,
							strlen(result.output),
							&again, &err), 0);
		CHECK(!strcmp(again.iso3, "NZL"));
		CHECK(again.hook.pulse_enable);
		CHECK(again.hook.pulse_invert);
	}

	CASE("a printed frequency group and off segment parse again");
	{
		char *gbr[] = { "udsp-cli", "region", "--region",
				TC_GBR_REGION_FILE };
		char *aus[] = { "udsp-cli", "region", "--region",
				TC_AUS_REGION_FILE };
		struct line_region_error err = {};
		struct udsp_region again;
		const struct udsp_region_tone *t;

		tc_cli_run(4, gbr, TC_CHILD_TIMEOUT_MS, &result);
		CHECK(!result.timed_out);
		CHECK_UINT(result.exit_code, CLI_EXIT_OK);
		CHECK(tc_output_has(&result, "[tone stutter]\n"
				    "segment = 350+440 -13 750\n"
				    "segment = 440 -13 750\nsilence = 0\n"
				    "repeat = forever\n"));
		CHECK_UINT((unsigned)-line_region_parse(result.output,
							strlen(result.output),
							&again, &err), 0);
		t = &again.tone[UDSP_TONE_STUTTER];
		CHECK_UINT(t->segment[0].freq_count, 2);
		CHECK_UINT(t->segment[0].freq_hz[1], 440);

		tc_cli_run(4, aus, TC_CHILD_TIMEOUT_MS, &result);
		CHECK(!result.timed_out);
		CHECK_UINT(result.exit_code, CLI_EXIT_OK);
		CHECK(tc_output_has(&result, "[tone reorder]\n"
				    "segment = 425 -13 375\nsegment = off 375\n"
				    "segment = 425 -23 375\nsilence = 375\n"
				    "repeat = forever\n"));
		CHECK_UINT((unsigned)-line_region_parse(result.output,
							strlen(result.output),
							&again, &err), 0);
		t = &again.tone[UDSP_TONE_REORDER];
		CHECK_UINT(t->segment_count, 3);
		CHECK_UINT(t->segment[1].freq_count, 0);
		CHECK_UINT((uint8_t)t->segment[2].level_dbm0, (uint8_t)-23);
	}

	CASE("region without --region fails usage");
	{
		char *argv[] = { "udsp-cli", "region" };

		tc_cli_run(2, argv, TC_CHILD_TIMEOUT_MS, &result);
		CHECK(!result.timed_out);
		CHECK_UINT(result.exit_code, CLI_EXIT_ERROR);
		CHECK(tc_output_has(&result, "region requires --region"));
	}

	CASE("file selftest exits 0");
	{
		char *argv[] = { "udsp-cli", "selftest" };

		tc_cli_run(2, argv, TC_CHILD_TIMEOUT_MS, &result);
		CHECK(!result.timed_out);
		CHECK_UINT(result.exit_code, CLI_EXIT_OK);
		CHECK(tc_output_has(&result, "# selftest ok"));
		CHECK(tc_output_has(&result, "ok - g729 tables"));
		for (unsigned chan = 0; chan < 2; chan++) {
			char step[64];

			snprintf(step, sizeof(step),
				 "ok - chan %u reinit", chan);
			CHECK(tc_output_has(&result, step));
			snprintf(step, sizeof(step),
				 "ok - chan %u converter restart", chan);
			CHECK(tc_output_has(&result, step));
			snprintf(step, sizeof(step),
				 "ok - chan %u monitor read", chan);
			CHECK(tc_output_has(&result, step));
			snprintf(step, sizeof(step),
				 "ok - chan %u converter read", chan);
			CHECK(tc_output_has(&result, step));
		}
	}

	CASE("file loopback exits 0 with ticks wait reason");
	{
		char *argv[] = { "udsp-cli", "loopback", "--tdm-backend", "file",
				 "--seconds", "1" };
		uint64_t ticks = UINT64_MAX;

		tc_cli_run(6, argv, TC_CHILD_TIMEOUT_MS, &result);
		CHECK(!result.timed_out);
		CHECK_UINT(result.exit_code, CLI_EXIT_OK);
		CHECK(tc_output_has(&result, "wait_reason = ticks"));
		CHECK(tc_output_uint(&result, "ticks = ", &ticks));
		CHECK_UINT(ticks, 100);
	}

	CASE("malformed region reports a parse error");
	{
		char region[128];
		char *argv[] = { "udsp-cli", "events", "--tdm-backend", "file",
				 "--seconds", "1", "--region", region };
		bool made = tc_temp_write(region, sizeof(region),
			"[region]\niso3 = USA\nunknown = value\n");

		CHECK(made);
		if (made) {
			tc_cli_run(8, argv, TC_CHILD_TIMEOUT_MS, &result);
			CHECK(!result.timed_out);
			CHECK_UINT(result.exit_code, CLI_EXIT_ERROR);
			CHECK(tc_output_has(&result, "region parse failed:"));
			CHECK(tc_output_has(&result,
					    "unknown key in [region]: unknown"));
			CHECK(!tc_output_has(&result, "region load failed:"));
			unlink(region);
		}
	}

	CASE("missing region reports an open error distinct from parse");
	{
		char region[128];
		char *argv[] = { "udsp-cli", "events", "--tdm-backend", "file",
				 "--seconds", "1", "--region", region };

		snprintf(region, sizeof(region), "/tmp/udsp-test-cli-missing-%ld.conf",
			 (long)getpid());
		unlink(region);
		tc_cli_run(8, argv, TC_CHILD_TIMEOUT_MS, &result);
		CHECK(!result.timed_out);
		CHECK_UINT(result.exit_code, CLI_EXIT_ERROR);
		CHECK(tc_output_has(&result, "region load failed:"));
		CHECK(tc_output_has(&result, "No such file or directory"));
		CHECK(!tc_output_has(&result, "region parse failed:"));
	}

	CASE("invalid capture with valid region does not blame the region");
	{
		char *argv[] = { "udsp-cli", "events", "--tdm-backend", "alsa",
				 "--playback", "invalid-pcm-device", "--capture",
				 "invalid-pcm-device", "--seconds", "1", "--region",
				 TC_REGION_FILE };

		tc_cli_run(12, argv, TC_CHILD_TIMEOUT_MS, &result);
		CHECK(!result.timed_out);
		CHECK_UINT(result.exit_code, CLI_EXIT_ERROR);
		CHECK(tc_output_has(&result,
			"udsp_open failed (region file parses; suspect the tdm devices or the slic path)"));
		CHECK(!tc_output_has(&result, "region parse failed:"));
		CHECK(!tc_output_has(&result, "region load failed:"));
	}

	CASE("alsa-raw backend name reaches udsp_open");
	{
		char *argv[] = {
			"udsp-cli", "events", "--tdm-backend", "alsa-raw",
			"--playback", "invalid-pcm-device", "--capture",
			"invalid-pcm-device", "--seconds", "1", "--region",
			TC_REGION_FILE
		};

		tc_cli_run(12, argv, TC_CHILD_TIMEOUT_MS, &result);
		CHECK(!result.timed_out);
		CHECK_UINT(result.exit_code, CLI_EXIT_ERROR);
		CHECK(tc_output_has(&result,
			"udsp_open failed (region file parses; "
			"suspect the tdm devices or the slic path)"));
		CHECK(!tc_output_has(&result, "region parse failed:"));
		CHECK(!tc_output_has(&result, "region load failed:"));
	}

	CASE("file events reports sequence summaries and excludes JOB_DONE");
	{
		char *argv[] = { "udsp-cli", "events", "--tdm-backend", "file",
				 "--seconds", "1" };

		tc_cli_run(6, argv, TC_CHILD_TIMEOUT_MS, &result);
		CHECK(!result.timed_out);
		CHECK_UINT(result.exit_code, CLI_EXIT_OK);
		CHECK(tc_output_has(&result, "events_lost = "));
		CHECK(tc_output_has(&result, " JOB_DONE line="));
		CHECK(tc_output_has(&result, "seq_zero = 0"));
		CHECK(!tc_output_has(&result,
			"synthetic/illegal JOB_DONE record has seq 0"));
	}
}

static void tc_fault_cases_run(void)
{
	struct cli_engine engine = { .base.started = true };
	struct udsp_event event = { .type = UDSP_EV_SLIC_FAULT, .seq = 1 };
	char output[1024];
	int saved;
	int fd;
	int quiet;

	CASE("a fault clear does not fail a started CLI run");
	quiet = tc_output_mute();
	saved = tc_error_mute();
	event_record(&engine.base, &event);
	tc_error_restore(saved);
	tc_output_restore(quiet);
	CHECK(!engine.base.faulted);
	CHECK_UINT(engine.seq, 1);

	CASE("a nonzero fault fails a started CLI run "
	     "and a clear preserves failure");
	quiet = tc_output_mute();
	saved = tc_error_mute();
	event.seq++;
	event.u.fault.code = UDSP_FAULT_SLIC_THERMAL;
	event_record(&engine.base, &event);
	tc_error_restore(saved);
	tc_output_restore(quiet);
	CHECK(engine.base.faulted);
	quiet = tc_output_mute();
	saved = tc_error_mute();
	event.seq++;
	event.u.fault.code = 0;
	event_record(&engine.base, &event);
	tc_error_restore(saved);
	tc_output_restore(quiet);
	CHECK(engine.base.faulted);
	CHECK_UINT(engine.events_lost, 0);

	CASE("fault decode prints cleared for code zero");
	fd = tc_capture_start(STDERR_FILENO, stderr, &saved);
	CHECK(fd >= 0);
	if (fd >= 0) {
		fault_decode_print(0);
		CHECK(tc_capture_stop(fd, STDERR_FILENO, stderr, saved,
				      output, sizeof(output)));
		CHECK(!strcmp(output,
			"udsp-cli: SLIC_FAULT code=0x00000000 bits=cleared\n"));
	}

	CASE("fault decode names ABI 7 bits "
	     "and preserves existing and unknown bits");
	fd = tc_capture_start(STDERR_FILENO, stderr, &saved);
	CHECK(fd >= 0);
	if (fd >= 0) {
		fault_decode_print(0x7ffu);
		CHECK(tc_capture_stop(fd, STDERR_FILENO, stderr, saved,
				      output, sizeof(output)));
		CHECK(!strcmp(output,
			"udsp-cli: SLIC_FAULT code=0x000007ff bits="
			"ENGINE_STALL,SLIC_THERMAL,SLIC_DC,SLIC_PQ,SLIC_VBAT,"
			"SLIC_LONGSTAT,SLIC_REINIT,SLIC_DCDC,SLIC_CLOCK,"
			"DEVICE_RESET,UNKNOWN_0x00000100\n"));
	}
}

static void tc_nullable_case_run(void)
{
	const struct udsp_slic_ops *ops = udsp_slic_fake_descriptor.ops;
	struct udsp_slic *s;
	unsigned channels = 0;
	bool ok;
	int saved;

	CASE("the selftest accepts absent ABI 7 ops");
	udsp_slic_fake_reset();
	udsp_slic_fake_optional_set(UDSP_SLIC_FAKE_OPT_ALL &
		~(uint32_t)(UDSP_SLIC_FAKE_OPT_REINIT |
			    UDSP_SLIC_FAKE_OPT_CONVERTER |
			    UDSP_SLIC_FAKE_OPT_MONITOR |
			    UDSP_SLIC_FAKE_OPT_CONVERTER_READ));
	CHECK(descriptor_valid());
	s = ops->attach(udsp_slic_fake_host_get(), &channels);
	CHECK(s != NULL);
	if (s) {
		saved = tc_output_mute();
		ok = chan_sequence(ops, s, 0);
		tc_output_restore(saved);
		CHECK(ok);
		ops->detach(s);
	}
	udsp_slic_fake_reset();
}

static void tc_looptest_cases(void)
{
	struct tc_cli_result result;
	struct cli_args args;

	CASE("the cross loop arms both ports only while both are off-hook");
	{
		struct looptest lt = { .count = 2, .mode = UDSP_LOOP_PEER };

		lt.port[1].line = 1;
		CHECK(!looptest_audio_want(&lt, 0));
		lt.port[0].offhook = true;
		/* One handset up: neither port carries audio, so a lifted
		 * handset never hears the ring voltage of the other port. */
		CHECK(!looptest_audio_want(&lt, 0));
		CHECK(!looptest_audio_want(&lt, 1));
		lt.port[1].offhook = true;
		CHECK(looptest_audio_want(&lt, 0));
		CHECK(looptest_audio_want(&lt, 1));

		lt.mode = UDSP_LOOP_SELF;
		lt.count = 1;
		lt.port[1].offhook = false;
		CHECK(looptest_audio_want(&lt, 0));
		lt.port[0].offhook = false;
		CHECK(!looptest_audio_want(&lt, 0));
	}

	CASE("looptest takes a ring cap of 1 to 60 seconds and no other");
	{
		char *zero[] = { "looptest", "--ring-s", "0" };
		char *over[] = { "looptest", "--ring-s", "61" };
		char *low[] = { "looptest", "--ring-s", "1" };
		char *high[] = { "looptest", "--ring-s", "60" };
		char *late[] = { "looptest", "--delay-ms", "1001" };
		char *delay[] = { "looptest", "--delay-ms", "1000" };

		CHECK_UINT(tc_args_parse(3, zero, &args), TOOL_PARSE_ERROR);
		CHECK_UINT(tc_args_parse(3, over, &args), TOOL_PARSE_ERROR);
		CHECK_UINT(tc_args_parse(3, low, &args), TOOL_PARSE_OK);
		CHECK_UINT(args.ring_s, 1);
		CHECK_UINT(tc_args_parse(3, high, &args), TOOL_PARSE_OK);
		CHECK_UINT(args.ring_s, 60);
		CHECK_UINT(tc_args_parse(3, late, &args), TOOL_PARSE_ERROR);
		CHECK_UINT(tc_args_parse(3, delay, &args), TOOL_PARSE_OK);
		CHECK_UINT(args.delay_ms, 1000);
	}

	CASE("the loop test options apply to looptest alone");
	{
		char *ring_argv[] = { "ring", "--ring-s", "10" };
		char *line_argv[] = { "looptest", "--line", "1" };
		char *loop_argv[] = { "looptest", "--ring-s", "10",
				      "--delay-ms", "0", "--peer", "1" };

		CHECK_UINT(tc_args_parse(3, ring_argv, &args), TOOL_PARSE_OK);
		CHECK(!tc_verb_args_valid(&args));
		CHECK_UINT(tc_args_parse(3, line_argv, &args), TOOL_PARSE_OK);
		CHECK(!tc_verb_args_valid(&args));
		CHECK_UINT(tc_args_parse(7, loop_argv, &args), TOOL_PARSE_OK);
		CHECK(tc_verb_args_valid(&args));
		CHECK_UINT(args.peer, 1);
	}

	CASE("the usage text carries looptest and its options");
	{
		char *argv[] = { "udsp-cli", "--help" };

		tc_cli_run(2, argv, TC_CHILD_TIMEOUT_MS, &result);
		CHECK(!result.timed_out);
		CHECK_UINT(result.exit_code, CLI_EXIT_OK);
		CHECK(tc_output_has(&result, "  looptest   ring, then loop a port "
				    "back to itself or a peer\n"));
		CHECK(tc_output_has(&result, "  --ring-s <1-60>             looptest "
				    "ring cap in seconds\n"));
		CHECK(tc_output_has(&result, "  --delay-ms <0-1000>         looptest "
				    "loop delay (default 500)\n"));
		CHECK(tc_output_has(&result, "  --peer <1-7>                looptest "
				    "peer line (default 1)\n"));
		CHECK(tc_output_has(&result, "looptest rings each port until its "
				    "handset is lifted"));
	}

	/* The refusal the retry exists for is the library's own: a line a ring
	 * session owns answers TRANSIENT to an arm. The suite cannot inject a
	 * status into udsp_line_loop, so the case makes a real one. */
	CASE("a transient arm is retried instead of ending the run");
	{
		char *argv[] = { "looptest", "--tdm-backend", "file",
				 "--seconds", "1" };
		struct looptest lt = { .count = 1, .mode = UDSP_LOOP_SELF };
		struct udsp_ring ring;
		struct cli_engine e;
		bool opened;
		int quiet;
		int saved;

		CHECK_UINT(tc_args_parse(5, argv, &args), TOOL_PARSE_OK);
		tool_file_defaults(&args.base);
		quiet = tc_output_mute();
		saved = tc_error_mute();
		opened = engine_open_real(&args, &e);
		if (opened) {
			ring_fill(&args, &e.region, &ring);
			/* No cap, so the session owns the line until it is
			 * stopped below. */
			ring.duration_ms = 0;
			opened = udsp_ring_start(e.base.s, 0, &ring) == UDSP_OK;
		}
		if (opened) {
			lt.port[0].offhook = true;
			/* The arm is refused, the port keeps its audio off and
			 * the verb stays in its wait. */
			CHECK(looptest_reconcile(&args, &e, &lt));
			CHECK(lt.retry);
			CHECK(!lt.port[0].audio);

			CHECK(udsp_ring_stop(e.base.s, 0) == UDSP_OK);
			/* The retry the wait runs on its own bound. */
			CHECK(looptest_reconcile(&args, &e, &lt));
			CHECK(!lt.retry);
			CHECK(lt.port[0].audio);
			CHECK(udsp_line_loop(e.base.s, 0, UDSP_LOOP_OFF, 0, 0) ==
			      UDSP_OK);
		}
		engine_close_real(&e);
		tc_error_restore(saved);
		tc_output_restore(quiet);
		CHECK(opened);
	}

	CASE("file looptest rings both ports and exits 0");
	{
		char *argv[] = { "udsp-cli", "looptest", "--tdm-backend", "file",
				 "--seconds", "1", "--ring-s", "1" };

		tc_cli_run(8, argv, TC_CHILD_TIMEOUT_MS, &result);
		CHECK(!result.timed_out);
		CHECK_UINT(result.exit_code, CLI_EXIT_OK);
		CHECK(tc_output_has(&result,
				    "looptest mode=cross ring_s=1 delay_ms=500\n"));
		CHECK(tc_output_has(&result, "looptest port=0 offhook=no\n"));
		CHECK(tc_output_has(&result, "looptest port=1 offhook=no\n"));
		CHECK(tc_output_has(&result, "looptest port=0 ring=start\n"));
		CHECK(tc_output_has(&result, "looptest port=1 ring=start\n"));
		/* No handset moves on the file back end, so the run ends on the
		 * budget with both ports still on-hook. */
		CHECK(tc_output_has(&result, "wait expired awaiting hook edge"));
		CHECK(!tc_output_has(&result, "looptest port=0 audio=on"));
	}

	CASE("the peer of a loop test names a line other than the first");
	{
		char *zero[] = { "looptest", "--peer", "0" };
		char *one[] = { "looptest", "--peer", "1" };
		char *last[] = { "looptest", "--peer", "7" };
		char *over[] = { "looptest", "--peer", "8" };

		/* The first port of the verb is line 0, so 0 names no peer. */
		CHECK_UINT(tc_args_parse(3, zero, &args), TOOL_PARSE_ERROR);
		CHECK_UINT(tc_args_parse(3, over, &args), TOOL_PARSE_ERROR);
		CHECK_UINT(tc_args_parse(3, one, &args), TOOL_PARSE_OK);
		CHECK_UINT(args.peer, 1);
		CHECK_UINT(tc_args_parse(3, last, &args), TOOL_PARSE_OK);
		CHECK_UINT(args.peer, UDSP_LINE_MAX - 1);
	}
}

int main(void)
{
	tc_parse_cases();
	tc_ring_timing_cases();
	tc_ring_fill_cases();
	tc_wait_cases();
	tc_seq_cases();
	tc_fault_cases_run();
	tc_nullable_case_run();
	tc_full_cli_cases();
	tc_looptest_cases();

	return tu_summary("cli");
}
