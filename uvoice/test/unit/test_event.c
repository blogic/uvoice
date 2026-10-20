// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the registration event publisher (src/event.c), which posts
 * the voip registration topics. The suite drives the publisher in two ways.
 *
 * Most of the suite starts no child. In the no-spawn test mode, event.c
 * records the argv that each post builds and does not call the spawn. The
 * fixture completes the child by hand. Without a script, a shell or a uloop,
 * the suite thus reads the wire bytes of both fixed topics and the dedup per
 * line. It reads the soft gate and the rule of one post in flight the same
 * way.
 *
 * test_real_spawn() starts children. It runs the path that the device runs:
 * the shared spawn of spawn_run.h and the uloop_process reap. The children
 * run generated scripts that record what they receive. Reason: a test mode
 * that the suite never checks against the real path proves only the test
 * mode.
 *
 * Three classes at the end of test_real_spawn() check what the spawn is for.
 * An exec that cannot succeed fails the call itself, and no child is in
 * flight. The TLS helper leads its own process group. A listener released
 * right after a spawn binds again at once.
 *
 * The fixed strings come from the voip section of the event schema of the
 * eventmgr package. A topic reads "voip/nodeMAC/line-%d/%s", with line+1
 * over the 0 based line index, because the subscribers of the topics read
 * that form.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libubox/uloop.h>

#include "event.h"
#include "spawn_run.h"
#include "test_util.h"

#define TOPIC_UP_1	"voip/nodeMAC/line-1/reg/successful"
#define TOPIC_DOWN_1	"voip/nodeMAC/line-1/reg/failed"
#define TOPIC_UP_2	"voip/nodeMAC/line-2/reg/successful"
#define TOPIC_DOWN_2	"voip/nodeMAC/line-2/reg/failed"

#define SCRIPT_DEFAULT	"/usr/srg/scripts/event-post.sh"
#define TLS_HELPER_DEFAULT	"/usr/sbin/uvoice_tls_cert.sh"
#define TLS_KEYLOG	"/tmp/uvoice/ssl.keylog"

static const char *arg(unsigned int rec, unsigned int idx)
{
	const char *s = event_test_arg(rec, idx);

	return s ? s : "(none)";
}

/* The whole published vector but argv[0]: priority, topic and EventData. */
static bool rec_is(unsigned int rec, const char *prio, const char *topic, const char *data)
{
	return !strcmp(arg(rec, 1), "-p") && !strcmp(arg(rec, 2), prio) &&
	       !strcmp(arg(rec, 3), "-t") && !strcmp(arg(rec, 4), topic) &&
	       !strcmp(arg(rec, 5), data);
}

static void post_up(unsigned int line, const char *proxy)
{
	event_reg_post(line, true, proxy);
}

static void post_down(unsigned int line)
{
	event_reg_post(line, false, NULL);
}

/* one post, driven to completion, so the next post on that line can fire */
static void cycle_up(unsigned int line, const char *proxy)
{
	post_up(line, proxy);
	event_test_reap(line, 0);
}

static void cycle_down(unsigned int line)
{
	post_down(line);
	event_test_reap(line, 0);
}

/* ------------------------------------------------------------- the dedup */

static void test_dedup(void)
{
	event_test_mode(true);

	CASE("dedup: unknown to up fires");
	cycle_up(0, "198.51.100.10");
	CHECK_UINT(event_test_posts(), 1);
	CHECK_UINT(event_test_recs(), 1);
	CHECK(rec_is(0, "info", TOPIC_UP_1, "{\"Proxy\":\"198.51.100.10\"}"));

	CASE("dedup: up to up is suppressed");
	cycle_up(0, "198.51.100.10");
	CHECK_UINT(event_test_posts(), 1);
	CHECK_UINT(event_test_suppressed(), 1);

	CASE("dedup: a second registrar on an already up line still posts nothing");
	cycle_up(0, "198.51.100.11");
	CHECK_UINT(event_test_posts(), 1);
	CHECK_UINT(event_test_suppressed(), 2);

	CASE("dedup: up to down fires");
	cycle_down(0);
	CHECK_UINT(event_test_posts(), 2);
	CHECK(rec_is(1, "crit", TOPIC_DOWN_1, "{}"));

	CASE("dedup: down to down is suppressed");
	cycle_down(0);
	CHECK_UINT(event_test_posts(), 2);
	CHECK_UINT(event_test_suppressed(), 3);

	/* without the dedup, each backoff tick would post a failed attempt */
	CASE("dedup: a failure retry storm posts one crit event");
	cycle_down(0);
	cycle_down(0);
	cycle_down(0);
	CHECK_UINT(event_test_posts(), 2);
	CHECK_UINT(event_test_suppressed(), 6);

	CASE("dedup: down to up fires");
	cycle_up(0, "198.51.100.10");
	CHECK_UINT(event_test_posts(), 3);
	CHECK(rec_is(2, "info", TOPIC_UP_1, "{\"Proxy\":\"198.51.100.10\"}"));

	CASE("dedup: unknown to down fires, a line that never came up reports failed");
	event_test_mode(true);
	cycle_down(1);
	CHECK_UINT(event_test_posts(), 1);
	CHECK(rec_is(0, "crit", TOPIC_DOWN_2, "{}"));
}

static void test_dedup_per_line(void)
{
	event_test_mode(true);

	CASE("dedup: each line keeps its own state");
	cycle_up(0, "198.51.100.10");
	cycle_up(1, "198.51.100.11");
	CHECK_UINT(event_test_posts(), 2);
	CHECK_UINT(event_test_suppressed(), 0);
	CHECK(rec_is(0, "info", TOPIC_UP_1, "{\"Proxy\":\"198.51.100.10\"}"));
	CHECK(rec_is(1, "info", TOPIC_UP_2, "{\"Proxy\":\"198.51.100.11\"}"));
	CHECK_UINT(event_test_rec_line(0), 0);
	CHECK_UINT(event_test_rec_line(1), 1);

	CASE("dedup: one line going down leaves the other line's state alone");
	cycle_down(0);
	cycle_up(1, "198.51.100.11");
	CHECK_UINT(event_test_posts(), 3);
	CHECK_UINT(event_test_suppressed(), 1);
	CHECK(rec_is(2, "crit", TOPIC_DOWN_1, "{}"));

	CASE("dedup: the second line still fires its own down edge");
	cycle_down(1);
	CHECK_UINT(event_test_posts(), 4);
	CHECK(rec_is(3, "crit", TOPIC_DOWN_2, "{}"));
}

/* ------------------------------------------------- the frozen wire strings */

static void test_topics(void)
{
	event_test_mode(true);

	CASE("topic: line 0 publishes line-1, a 1 based topic");
	cycle_up(0, "sip.example.com");
	CHECK(!strcmp(arg(0, 4), TOPIC_UP_1));

	CASE("topic: line 1 publishes line-2");
	cycle_up(1, "sip.example.com");
	CHECK(!strcmp(arg(1, 4), TOPIC_UP_2));

	CASE("topic: the highest tracked line publishes line-8");
	cycle_up(EVENT_LINE_MAX - 1, "sip.example.com");
	CHECK(!strcmp(arg(2, 4), "voip/nodeMAC/line-8/reg/successful"));

	CASE("topic: the failed topic carries the same base");
	cycle_down(1);
	CHECK(!strcmp(arg(3, 4), TOPIC_DOWN_2));

	CASE("priority: successful is info, failed is crit");
	CHECK(!strcmp(arg(0, 2), "info"));
	CHECK(!strcmp(arg(3, 2), "crit"));

	CASE("json: the failed EventData is the empty object, never a Proxy");
	CHECK(!strcmp(arg(3, 5), "{}"));

	CASE("json: a proxy on the failed edge is ignored");
	event_test_mode(true);
	event_reg_post(0, false, "198.51.100.10");
	CHECK(rec_is(0, "crit", TOPIC_DOWN_1, "{}"));
}

/* ----------------------------------------------------------- the argv shape */

static void test_argv(void)
{
	event_test_mode(true);

	CASE("argv: the vector is script, -p prio, -t topic, json");
	cycle_up(0, "198.51.100.10");
	CHECK_UINT(event_test_argc(0), 6);
	CHECK(!strcmp(arg(0, 0), SCRIPT_DEFAULT));
	CHECK(!strcmp(arg(0, 1), "-p"));
	CHECK(!strcmp(arg(0, 2), "info"));
	CHECK(!strcmp(arg(0, 3), "-t"));
	CHECK(!strcmp(arg(0, 4), TOPIC_UP_1));
	CHECK(!strcmp(arg(0, 5), "{\"Proxy\":\"198.51.100.10\"}"));

	/* the recorder copies the emitter's count and records the terminator
	 * beside it as data, so a lost NULL fails here instead of crashing */
	CASE("argv: the vector carries the NULL execv needs after the json");
	CHECK(event_test_arg(0, 5) != NULL);
	CHECK(event_test_arg(0, 6) == NULL);
	CHECK(event_test_terminated(0));

	CASE("argv: no argument carries a shell quote");
	CHECK(!strchr(arg(0, 5), '\''));
	CHECK(!strstr(arg(0, 5), "$'"));

	CASE("argv: -m is never passed, the nodeMAC token is substituted downstream");
	CHECK(strcmp(arg(0, 1), "-m") && strcmp(arg(0, 3), "-m"));
	CHECK(strstr(arg(0, 4), "voip/nodeMAC/") == arg(0, 4));
}

/* ------------------------------------------------------------- the proxy */

static void proxy_case(const char *name, const char *proxy, const char *want)
{
	event_test_mode(true);
	CASE(name);
	cycle_up(0, proxy);
	CHECK_UINT(event_test_recs(), 1);
	if (strcmp(arg(0, 5), want))
		printf("    fail proxy data: got %s, want %s\n", arg(0, 5), want);
	CHECK(!strcmp(arg(0, 5), want));
}

static void test_proxy(void)
{
	char big[EVENT_PROXY_MAX + 8];
	char edge[EVENT_PROXY_MAX + 1];
	char fit[EVENT_PROXY_MAX];
	char want[EVENT_DATA_MAX];

	proxy_case("proxy: an IPv4 literal is published",
		   "198.51.100.10", "{\"Proxy\":\"198.51.100.10\"}");
	proxy_case("proxy: a host name with dots and a dash is published",
		   "sip-1.example.com", "{\"Proxy\":\"sip-1.example.com\"}");
	proxy_case("proxy: a bracketed IPv6 address with a scope is published",
		   "[fe80::1%eth0]", "{\"Proxy\":\"[fe80::1%eth0]\"}");
	proxy_case("proxy: a host:port form is published",
		   "sip.example.com:5062", "{\"Proxy\":\"sip.example.com:5062\"}");

	proxy_case("proxy: NULL drops the field and the event still fires", NULL, "{}");
	proxy_case("proxy: an empty string drops the field", "", "{}");

	/* the whole field goes, never an escape: a value no consumer can match
	 * is worth less than an absent one */
	proxy_case("proxy: a double quote drops the field", "a\"b", "{}");
	proxy_case("proxy: a backslash drops the field", "a\\b", "{}");
	proxy_case("proxy: an injected json field drops the field",
		   "x\",\"Injected\":\"y", "{}");
	proxy_case("proxy: a space drops the field", "a b", "{}");
	proxy_case("proxy: a newline drops the field", "a\nb", "{}");
	proxy_case("proxy: a shell metacharacter drops the field", "a;$(id)", "{}");
	proxy_case("proxy: a non-ascii byte drops the field", "a\xc3\xa9""b", "{}");

	/* EVENT_PROXY_MAX - 1 bytes: the last length the buffer holds whole */
	memset(fit, 'a', sizeof(fit));
	fit[sizeof(fit) - 1] = 0;
	CHECK_UINT(strlen(fit), EVENT_PROXY_MAX - 1);
	snprintf(want, sizeof(want), "{\"Proxy\":\"%s\"}", fit);
	proxy_case("proxy: the longest value that fits is published whole", fit, want);

	/* the boundary itself. EVENT_PROXY_MAX bytes needs EVENT_PROXY_MAX + 1
	 * of buffer, so it is one byte too long and the field goes. A rule
	 * written with <= instead of < would publish it. */
	memset(edge, 'a', sizeof(edge));
	edge[sizeof(edge) - 1] = 0;
	CHECK_UINT(strlen(edge), EVENT_PROXY_MAX);
	proxy_case("proxy: a value of exactly EVENT_PROXY_MAX bytes is refused", edge, "{}");

	memset(big, 'a', sizeof(big));
	big[sizeof(big) - 1] = 0;
	proxy_case("proxy: an over-long value is refused, never truncated", big, "{}");

	CASE("proxy: dropping the field does not suppress the event");
	CHECK_UINT(event_test_posts(), 1);
	CHECK_UINT(event_test_suppressed(), 0);
}

/* ---------------------------------------------------------- the soft gate */

/* a real regular file, optionally executable, for the gate to look at */
static bool script_make(char *path, size_t plen, bool executable)
{
	int fd;

	snprintf(path, plen, "/tmp/uvoice-event-XXXXXX");
	fd = mkstemp(path);
	if (fd < 0)
		return false;

	close(fd);

	return chmod(path, executable ? 0755 : 0644) == 0;
}

static void test_gate(void)
{
	char path[64];

	CASE("gate: an absent script refuses every post and records none");
	event_test_mode(true);
	setenv("UVOICE_EVENT_POST", "/nonexistent/uvoice/event-post.sh", 1);
	event_init();
	CHECK(!event_available());
	post_up(0, "198.51.100.10");
	post_down(0);
	CHECK_UINT(event_test_posts(), 0);
	CHECK_UINT(event_test_recs(), 0);
	CHECK_UINT(event_test_refused(), 2);

	/* a refusal is no duplicate: the gate never touched the dedup */
	CASE("gate: a refusal is counted apart from a suppressed duplicate");
	CHECK_UINT(event_test_suppressed(), 0);
	post_down(0);
	CHECK_UINT(event_test_refused(), 3);
	CHECK_UINT(event_test_suppressed(), 0);

	/*
	 * The dedup basis is what the layer published, not what it was asked to
	 * post. A refusal published nothing, so it must leave the basis alone:
	 * a gate that comes back must
	 * still report the state it was refused, or the line stays silent until
	 * it happens to change twice.
	 */
	CASE("gate: a refused post does not advance the dedup, the re-armed gate fires it");
	event_test_mode(true);
	setenv("UVOICE_EVENT_POST", "/nonexistent/uvoice/event-post.sh", 1);
	event_init();
	post_up(0, "198.51.100.10");
	CHECK_UINT(event_test_refused(), 1);
	CHECK_UINT(event_test_posts(), 0);
	CHECK(script_make(path, sizeof(path), true));
	setenv("UVOICE_EVENT_POST", path, 1);
	event_init();
	CHECK(event_available());
	post_up(0, "198.51.100.10");
	CHECK_UINT(event_test_posts(), 1);
	CHECK_UINT(event_test_suppressed(), 0);
	CHECK(rec_is(0, "info", TOPIC_UP_1, "{\"Proxy\":\"198.51.100.10\"}"));
	unlink(path);
	unsetenv("UVOICE_EVENT_POST");

	CASE("gate: a directory is not a script, X_OK alone would arm the gate");
	event_test_mode(true);
	setenv("UVOICE_EVENT_POST", "/tmp", 1);
	event_init();
	CHECK(!event_available());
	post_up(0, "198.51.100.10");
	CHECK_UINT(event_test_posts(), 0);
	CHECK_UINT(event_test_refused(), 1);

	CASE("gate: a regular file with no execute bit is refused");
	event_test_mode(true);
	CHECK(script_make(path, sizeof(path), false));
	setenv("UVOICE_EVENT_POST", path, 1);
	event_init();
	CHECK(!event_available());
	post_up(0, "198.51.100.10");
	CHECK_UINT(event_test_posts(), 0);
	unlink(path);

	CASE("gate: an executable regular file arms the gate and names argv[0]");
	event_test_mode(true);
	CHECK(script_make(path, sizeof(path), true));
	setenv("UVOICE_EVENT_POST", path, 1);
	event_init();
	CHECK(event_available());
	cycle_up(0, "198.51.100.10");
	CHECK_UINT(event_test_posts(), 1);
	CHECK_UINT(event_test_refused(), 0);
	CHECK(!strcmp(arg(0, 0), path));
	unlink(path);
	unsetenv("UVOICE_EVENT_POST");

	CASE("gate: the default path is the eventmgr script");
	event_test_mode(true);
	cycle_up(0, "198.51.100.10");
	CHECK(!strcmp(arg(0, 0), SCRIPT_DEFAULT));

	CASE("gate: a line at or above the bound is refused, not folded");
	event_test_mode(true);
	post_up(EVENT_LINE_MAX, "198.51.100.10");
	post_up(EVENT_LINE_MAX + 100, "198.51.100.10");
	CHECK_UINT(event_test_posts(), 0);
	CHECK_UINT(event_test_refused(), 2);
	cycle_up(EVENT_LINE_MAX - 1, "198.51.100.10");
	CHECK_UINT(event_test_posts(), 1);
}

/* --------------------------------------------------------- one in flight */

static void test_inflight(void)
{
	char big[EVENT_PROXY_MAX + 8];

	event_test_mode(true);

	CASE("inflight: a change during a post waits for the reap");
	post_up(0, "198.51.100.10");
	CHECK_UINT(event_test_posts(), 1);
	post_down(0);
	CHECK_UINT(event_test_posts(), 1);
	CHECK_UINT(event_test_recs(), 1);
	CHECK_UINT(event_test_suppressed(), 0);

	CASE("inflight: the reap fires the queued state");
	event_test_reap(0, 0);
	CHECK_UINT(event_test_posts(), 2);
	CHECK(rec_is(1, "crit", TOPIC_DOWN_1, "{}"));

	CASE("inflight: the queue holds one state, the LATEST, never a list");
	event_test_reap(0, 0);
	CHECK_UINT(event_test_posts(), 2);

	/* up (posts), down, up, down while it runs: one down event, not three */
	CASE("inflight: a flap during a post collapses to its final state");
	event_test_mode(true);
	post_up(0, "198.51.100.10");
	post_down(0);
	post_up(0, "198.51.100.11");
	post_down(0);
	CHECK_UINT(event_test_posts(), 1);
	event_test_reap(0, 0);
	CHECK_UINT(event_test_posts(), 2);
	CHECK(rec_is(1, "crit", TOPIC_DOWN_1, "{}"));
	event_test_reap(0, 0);
	CHECK_UINT(event_test_posts(), 2);

	CASE("inflight: a flap that ends where it started fires nothing at the reap");
	event_test_mode(true);
	post_up(0, "198.51.100.10");
	post_down(0);
	post_up(0, "198.51.100.11");
	CHECK_UINT(event_test_posts(), 1);
	event_test_reap(0, 0);
	CHECK_UINT(event_test_posts(), 1);
	CHECK_UINT(event_test_collapsed(), 1);
	CHECK_UINT(event_test_recs(), 1);

	CASE("inflight: the collapsed line still reports its next real change");
	cycle_down(0);
	CHECK_UINT(event_test_posts(), 2);
	CHECK(rec_is(1, "crit", TOPIC_DOWN_1, "{}"));

	CASE("inflight: a repeat of the queued state is a suppressed duplicate");
	event_test_mode(true);
	post_up(0, "198.51.100.10");
	post_down(0);
	post_down(0);
	post_down(0);
	CHECK_UINT(event_test_suppressed(), 2);
	event_test_reap(0, 0);
	CHECK_UINT(event_test_posts(), 2);

	CASE("inflight: one line's post never blocks another line");
	event_test_mode(true);
	post_up(0, "198.51.100.10");
	post_up(1, "198.51.100.11");
	CHECK_UINT(event_test_posts(), 2);
	CHECK_UINT(event_test_rec_line(0), 0);
	CHECK_UINT(event_test_rec_line(1), 1);

	CASE("inflight: the queued up post carries the registrar that queued it");
	event_test_mode(true);
	post_down(0);
	post_up(0, "198.51.100.10");
	post_up(0, "203.0.113.7");
	event_test_reap(0, 0);
	CHECK_UINT(event_test_posts(), 2);
	CHECK_UINT(event_test_suppressed(), 1);
	CHECK(rec_is(1, "info", TOPIC_UP_1, "{\"Proxy\":\"198.51.100.10\"}"));

	/* the queue copies into a bounded buffer, so an over-long value must be
	 * refused first, or it would come out truncated and clean */
	CASE("inflight: an over-long registrar queued during a post drops the field");
	event_test_mode(true);
	memset(big, 'a', sizeof(big));
	big[sizeof(big) - 1] = 0;
	post_down(0);
	post_up(0, big);
	event_test_reap(0, 0);
	CHECK_UINT(event_test_posts(), 2);
	CHECK(rec_is(1, "info", TOPIC_UP_1, "{}"));

	CASE("inflight: a quoted registrar queued during a post drops the field");
	event_test_mode(true);
	post_down(0);
	post_up(0, "a\"b");
	event_test_reap(0, 0);
	CHECK(rec_is(1, "info", TOPIC_UP_1, "{}"));
}

/* ------------------------------------------------------------- shutdown */

static void test_done(void)
{
	CASE("done: a post in flight is dropped and the line posts again after");
	event_test_mode(true);
	post_up(0, "198.51.100.10");
	post_down(0);
	event_done();
	CHECK_UINT(event_test_posts(), 1);

	/* the state reported before the shutdown survives: only up is news */
	cycle_down(0);
	CHECK_UINT(event_test_posts(), 2);
	CHECK(rec_is(1, "crit", TOPIC_DOWN_1, "{}"));

	CASE("done: a second call on an idle layer changes nothing");
	event_done();
	event_done();
	CHECK_UINT(event_test_posts(), 2);

	CASE("done: a reap after the shutdown fires nothing");
	event_test_reap(0, 0);
	CHECK_UINT(event_test_posts(), 2);
}

/* --------------------------------------------------------- the TLS helper */

static unsigned int tls_done_calls;
static char tls_fixture_dir[64];
static char tls_dir[96];
static char tls_status[96];
static bool tls_fixture_ready;

static bool tls_fixture_make(void);

static void tls_path_get(char *path, size_t path_size, const char *name)
{
	snprintf(path, path_size, "%s/%s", tls_dir, name);
}

static void tls_done(void)
{
	tls_done_calls++;
}

static void tls_bundle_drop(void)
{
	static const char *const names[] = {
		"cert.pem", "key.pem", "ca.crt", "voip_cert.tgz", "header.txt",
		"prev_header.txt", "last_error_reason.txt", "extra.file",
	};
	char path[128];
	unsigned int i;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		tls_path_get(path, sizeof(path), names[i]);
		unlink(path);
	}
	rmdir(tls_dir);
}

static bool tls_bundle_make(void)
{
	char path[128];
	FILE *f;

	if (mkdir(tls_dir, 0700) && access(tls_dir, F_OK))
		return false;

	tls_path_get(path, sizeof(path), "cert.pem");
	f = fopen(path, "w");
	if (!f)
		return false;
	fputs("cert", f);
	fclose(f);

	tls_path_get(path, sizeof(path), "key.pem");
	f = fopen(path, "w");
	if (!f)
		return false;
	fputs("key", f);
	fclose(f);

	return true;
}

static const char *tls_arg(unsigned int idx)
{
	const char *s = event_test_tls_arg(idx);

	return s ? s : "(none)";
}

static void test_tls_helper(void)
{
	CASE("tls fixture: the suite owns unique bundle and status paths");
	tls_fixture_ready = tls_fixture_make();
	CHECK(tls_fixture_ready);
	if (!tls_fixture_ready)
		return;

	tls_bundle_drop();
	event_test_mode(true);
	tls_done_calls = 0;

	CASE("tls helper: start records the frozen helper argument vector");
	event_tls_cert_start("https://cert.example/voice.tgz", "alice", "secret", true,
			     tls_done);
	CHECK_UINT(event_test_tls_starts(), 1);
	CHECK(event_test_tls_busy());
	CHECK(event_test_tls_armed());
	CHECK(!strcmp(tls_arg(0), TLS_HELPER_DEFAULT));
	CHECK(!strcmp(tls_arg(1), "https://cert.example/voice.tgz"));
	CHECK(!strcmp(tls_arg(2), "alice"));
	CHECK(!strcmp(tls_arg(3), "secret"));
	CHECK(!strcmp(tls_arg(4), "1"));
	CHECK(event_test_tls_arg(5) == NULL);
	CHECK(event_test_tls_terminated());

	CASE("tls helper: the same request does not start a second child");
	event_tls_cert_start("https://cert.example/voice.tgz", "alice", "secret", true,
			     tls_done);
	CHECK_UINT(event_test_tls_starts(), 1);

	CASE("tls helper: a moved request waits behind the child in flight");
	event_tls_cert_start("ftp://cert.example/new.tgz", "bob", "changed", false,
			     tls_done);
	CHECK_UINT(event_test_tls_starts(), 1);
	event_test_tls_reap(0);
	CHECK_UINT(event_test_tls_starts(), 2);
	CHECK(event_test_tls_busy());
	CHECK(!strcmp(tls_arg(1), "ftp://cert.example/new.tgz"));
	CHECK(!strcmp(tls_arg(2), "bob"));
	CHECK(!strcmp(tls_arg(3), "changed"));
	CHECK(!strcmp(tls_arg(4), "0"));
	CHECK_UINT(tls_done_calls, 0);

	CASE("tls helper: an installed bundle calls the completion once");
	CHECK(tls_bundle_make());
	event_test_tls_reap(0);
	CHECK_UINT(tls_done_calls, 1);
	CHECK(!event_test_tls_busy());
	CHECK(!event_test_tls_retry_pending());

	CASE("tls helper: an unchanged bundle does not call the completion");
	event_test_mode(true);
	tls_done_calls = 0;
	event_tls_cert_start("https://cert.example/voice.tgz", "", "", false, tls_done);
	event_test_tls_reap(2);
	CHECK_UINT(tls_done_calls, 0);
	CHECK(!event_test_tls_retry_pending());

	CASE("tls helper: a configuration refusal calls no callback and arms no retry");
	tls_bundle_drop();
	event_test_mode(true);
	tls_done_calls = 0;
	event_tls_cert_start("", "", "", false, tls_done);
	event_test_tls_reap(4);
	CHECK_UINT(tls_done_calls, 0);
	CHECK(!event_test_tls_retry_pending());

	CASE("tls helper: an absent bundle arms a 30 second retry after failure");
	tls_bundle_drop();
	event_test_mode(true);
	event_tls_cert_start("https://cert.example/voice.tgz", "", "", false, tls_done);
	event_test_tls_reap(1);
	CHECK(event_test_tls_retry_pending());
	CHECK_UINT(event_test_tls_starts(), 1);
	event_test_tls_retry();
	CHECK_UINT(event_test_tls_starts(), 2);
	CHECK(event_test_tls_busy());

	CASE("tls helper: clearing an invalid bundle calls done and keeps the retry armed");
	event_test_tls_reap(3);
	CHECK_UINT(tls_done_calls, 1);
	CHECK(event_test_tls_retry_pending());

	CASE("tls helper: a configuration stop serializes clear behind the fetch");
	event_test_tls_retry();
	CHECK(event_test_tls_busy());
	event_tls_cert_stop(EVENT_TLS_STOP_CONFIG);
	CHECK(!event_test_tls_armed());
	CHECK(!event_test_tls_retry_pending());
	event_test_tls_reap(0);
	CHECK_UINT(tls_done_calls, 1);
	CHECK(event_test_tls_busy());
	CHECK(!strcmp(tls_arg(1), "clear"));
	CHECK(event_test_tls_arg(2) == NULL);
	CHECK(event_test_tls_terminated());
	event_tls_cert_stop(EVENT_TLS_STOP_CONFIG);
	CHECK(event_test_tls_busy());
	CHECK(!strcmp(tls_arg(1), "clear"));
	event_test_tls_reap(0);
	CHECK(!event_test_tls_busy());

	CASE("tls helper: a shutdown stop does not spawn clear");
	event_tls_cert_start("https://cert.example/voice.tgz", "", "", false, tls_done);
	CHECK(event_test_tls_busy());
	{
		unsigned int starts = event_test_tls_starts();

		event_tls_cert_stop(EVENT_TLS_STOP_SHUTDOWN);
		event_test_tls_reap(0);
		CHECK_UINT(event_test_tls_starts(), starts);
		CHECK(!event_test_tls_busy());
	}

	tls_bundle_drop();
}

static char tls_fake_curl[96];
static char tls_fake_event[96];
static char tls_event_out[96];
static char tls_curl_out[96];
static char tls_curl_pid[96];
static char tls_curl_term[96];
static char tls_valid_dir[96];
static char tls_no_key_dir[96];
static char tls_no_ca_dir[96];
static char tls_helper[256];
static char tls_path_save[1024];

static bool text_file_put(const char *path, const char *text, mode_t mode)
{
	FILE *f = fopen(path, "w");

	if (!f)
		return false;

	fputs(text, f);
	if (fclose(f))
		return false;

	return chmod(path, mode) == 0;
}

static bool tls_helper_find(void)
{
	static const char suffix[] = "/files/uvoice_tls_cert.sh";
	char source[sizeof(tls_helper)];
	char *tail;
	size_t root_len;

	snprintf(source, sizeof(source), "%s", __FILE__);
	tail = strstr(source, "/test/unit/test_event.c");
	if (!tail)
		return false;

	root_len = (size_t)(tail - source);
	if (root_len + sizeof(suffix) > sizeof(tls_helper))
		return false;

	memcpy(tls_helper, source, root_len);
	memcpy(tls_helper + root_len, suffix, sizeof(suffix));
	return access(tls_helper, X_OK) == 0;
}

static bool tls_fixture_make(void)
{
	const char *path = getenv("PATH");
	char next_path[sizeof(tls_path_save) + sizeof(tls_fixture_dir) + 2];
	char file[128];

	snprintf(tls_fixture_dir, sizeof(tls_fixture_dir), "/tmp/uvoice-tls-XXXXXX");
	if (!mkdtemp(tls_fixture_dir))
		return false;

	snprintf(tls_dir, sizeof(tls_dir), "%s/bundle", tls_fixture_dir);
	snprintf(tls_status, sizeof(tls_status), "%s/status", tls_fixture_dir);
	snprintf(tls_fake_curl, sizeof(tls_fake_curl), "%s/curl", tls_fixture_dir);
	snprintf(tls_fake_event, sizeof(tls_fake_event), "%s/event-post", tls_fixture_dir);
	snprintf(tls_event_out, sizeof(tls_event_out), "%s/event.out", tls_fixture_dir);
	snprintf(tls_curl_out, sizeof(tls_curl_out), "%s/curl.out", tls_fixture_dir);
	snprintf(tls_curl_pid, sizeof(tls_curl_pid), "%s/curl.pid", tls_fixture_dir);
	snprintf(tls_curl_term, sizeof(tls_curl_term), "%s/curl.term", tls_fixture_dir);
	snprintf(tls_valid_dir, sizeof(tls_valid_dir), "%s/valid", tls_fixture_dir);
	snprintf(tls_no_key_dir, sizeof(tls_no_key_dir), "%s/no-key", tls_fixture_dir);
	snprintf(tls_no_ca_dir, sizeof(tls_no_ca_dir), "%s/no-ca", tls_fixture_dir);

	if (mkdir(tls_valid_dir, 0700) || mkdir(tls_no_key_dir, 0700) ||
	    mkdir(tls_no_ca_dir, 0700))
		return false;

	snprintf(file, sizeof(file), "%s/cert.pem", tls_valid_dir);
	if (!text_file_put(file, "cert\n", 0600))
		return false;
	snprintf(file, sizeof(file), "%s/key.pem", tls_valid_dir);
	if (!text_file_put(file, "key\n", 0600))
		return false;
	snprintf(file, sizeof(file), "%s/ca.crt", tls_valid_dir);
	if (!text_file_put(file, "ca\n", 0600))
		return false;

	snprintf(file, sizeof(file), "%s/cert.pem", tls_no_key_dir);
	if (!text_file_put(file, "cert\n", 0600))
		return false;
	snprintf(file, sizeof(file), "%s/ca.crt", tls_no_key_dir);
	if (!text_file_put(file, "ca\n", 0600))
		return false;

	snprintf(file, sizeof(file), "%s/cert.pem", tls_no_ca_dir);
	if (!text_file_put(file, "cert\n", 0600))
		return false;
	snprintf(file, sizeof(file), "%s/key.pem", tls_no_ca_dir);
	if (!text_file_put(file, "key\n", 0600))
		return false;

	if (!text_file_put(tls_fake_curl,
		"#!/bin/sh\n"
		"printf '%s\\n' \"$*\" >> \"$TLS_CURL_OUT\"\n"
		"header=\n"
		"out=\n"
		"while [ \"$#\" -gt 0 ]; do\n"
		" case \"$1\" in\n"
		"  -D|-o|-u) key=$1; value=$2; shift 2; [ \"$key\" = -D ] && header=$value; [ \"$key\" = -o ] && out=$value ;;\n"
		"  *) shift ;;\n"
		" esac\n"
		"done\n"
		"if [ -n \"$header\" ]; then\n"
		" case \"$TLS_FAKE_MODE\" in\n"
		/* the trap is installed first, then the pid file: the test
		 * signals as soon as that file exists, and a signal that
		 * arrives with no trap installed kills the shell on the
		 * default disposition */
		"  hold) trap 'printf term > \"$TLS_CURL_TERM\"; exit 0' TERM; printf '%s' \"$$\" > \"$TLS_CURL_PID\"; while :; do sleep 1; done ;;\n"
		"  http-404) printf 'HTTP/1.1 404 Not Found\\r\\n\\r\\n' > \"$header\" ;;\n"
		"  http-wrong) printf 'HTTP/1.1 200 OK\\r\\nContent-Type: text/plain\\r\\nContent-Length: 10\\r\\nETag: one\\r\\n\\r\\n' > \"$header\" ;;\n"
		"  http-large) printf 'HTTP/1.1 200 OK\\r\\nContent-Type: application/x-gzip\\r\\nContent-Length: 5001\\r\\nETag: one\\r\\n\\r\\n' > \"$header\" ;;\n"
		"  ftp-missing) : > \"$header\" ;;\n"
		"  ftp-text) printf '213 words\\r\\n' > \"$header\" ;;\n"
		"  ftp-large) printf '213 5001\\r\\n' > \"$header\" ;;\n"
		"  ftp-ok|ftp-keep) printf '213 100\\r\\n' > \"$header\" ;;\n"
		"  http-etag-two) printf 'HTTP/1.1 200 OK\\r\\nContent-Type: application/x-gzip\\r\\nContent-Length: 100\\r\\nETag: two\\r\\n\\r\\n' > \"$header\" ;;\n"
		"  *) printf 'HTTP/1.1 200 OK\\r\\nContent-Type: application/x-gzip\\r\\nContent-Length: 100\\r\\nETag: one\\r\\n\\r\\n' > \"$header\" ;;\n"
		" esac\n"
		" exit 0\n"
		"fi\n"
		"[ \"$TLS_FAKE_MODE\" = ftp-keep ] && exit 0\n"
		"[ \"$TLS_FAKE_MODE\" = bad-archive ] && { printf bad > \"$out\"; exit 0; }\n"
		"exec tar czf \"$out\" -C \"$TLS_FAKE_INPUT\" .\n", 0755))
		return false;

	if (!text_file_put(tls_fake_event,
		"#!/bin/sh\nprintf '%s\\n' \"$@\" >> \"$TLS_EVENT_OUT\"\n", 0755))
		return false;
	if (!text_file_put(tls_event_out, "", 0600))
		return false;
	if (!text_file_put(tls_curl_out, "", 0600))
		return false;

	snprintf(tls_path_save, sizeof(tls_path_save), "%s", path ? path : "");
	snprintf(next_path, sizeof(next_path), "%s:%s", tls_fixture_dir, tls_path_save);
	setenv("PATH", next_path, 1);
	setenv("UVOICE_EVENT_POST", tls_fake_event, 1);
	setenv("TLS_EVENT_OUT", tls_event_out, 1);
	setenv("TLS_CURL_OUT", tls_curl_out, 1);
	setenv("TLS_CURL_PID", tls_curl_pid, 1);
	setenv("TLS_CURL_TERM", tls_curl_term, 1);
	setenv("UVOICE_TLS_DIR", tls_dir, 1);
	setenv("UVOICE_TLS_STATUS", tls_status, 1);

	return tls_helper_find();
}

static void tls_fixture_drop(void)
{
	char file[128];

	setenv("PATH", tls_path_save, 1);
	unsetenv("UVOICE_EVENT_POST");
	unsetenv("TLS_EVENT_OUT");
	unsetenv("TLS_CURL_OUT");
	unsetenv("TLS_CURL_PID");
	unsetenv("TLS_CURL_TERM");
	unsetenv("TLS_FAKE_MODE");
	unsetenv("TLS_FAKE_INPUT");
	unsetenv("UVOICE_TLS_DIR");
	unsetenv("UVOICE_TLS_STATUS");

	snprintf(file, sizeof(file), "%s/cert.pem", tls_valid_dir);
	unlink(file);
	snprintf(file, sizeof(file), "%s/key.pem", tls_valid_dir);
	unlink(file);
	snprintf(file, sizeof(file), "%s/ca.crt", tls_valid_dir);
	unlink(file);
	snprintf(file, sizeof(file), "%s/cert.pem", tls_no_key_dir);
	unlink(file);
	snprintf(file, sizeof(file), "%s/ca.crt", tls_no_key_dir);
	unlink(file);
	snprintf(file, sizeof(file), "%s/cert.pem", tls_no_ca_dir);
	unlink(file);
	snprintf(file, sizeof(file), "%s/key.pem", tls_no_ca_dir);
	unlink(file);
	rmdir(tls_valid_dir);
	rmdir(tls_no_key_dir);
	rmdir(tls_no_ca_dir);
	unlink(tls_fake_curl);
	unlink(tls_fake_event);
	unlink(tls_event_out);
	unlink(tls_curl_out);
	unlink(tls_curl_pid);
	unlink(tls_curl_term);
	tls_bundle_drop();
	unlink(tls_status);
	rmdir(tls_fixture_dir);
}

static int tls_helper_run(const char *url, const char *verify)
{
	int status;
	pid_t pid = fork();

	if (pid < 0)
		return -1;
	if (pid == 0) {
		execl(tls_helper, tls_helper, url, "user", "pass", verify, NULL);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
		return -1;

	return WEXITSTATUS(status);
}

static int tls_clear_run(void)
{
	int status;
	pid_t pid = fork();

	if (pid < 0)
		return -1;
	if (pid == 0) {
		execl(tls_helper, tls_helper, "clear", NULL);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
		return -1;

	return WEXITSTATUS(status);
}

static unsigned int file_count(const char *path, const char *needle)
{
	char line[512];
	unsigned int count = 0;
	FILE *f = fopen(path, "r");

	if (!f)
		return 0;

	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, needle))
			count++;
	}
	fclose(f);
	return count;
}

static bool tls_file_exists(const char *name)
{
	char path[128];

	tls_path_get(path, sizeof(path), name);
	return access(path, F_OK) == 0;
}

static mode_t tls_file_mode(const char *name)
{
	char path[128];
	struct stat st;

	tls_path_get(path, sizeof(path), name);
	return stat(path, &st) ? 0 : st.st_mode & 0777;
}

static bool tls_status_is(const char *want)
{
	char got[160];
	size_t n;
	FILE *f = fopen(tls_status, "rb");

	if (!f)
		return false;

	n = fread(got, 1, sizeof(got), f);
	fclose(f);

	return n == strlen(want) && !memcmp(got, want, n);
}

static void tls_script_case(const char *name, const char *mode, const char *input,
			    const char *url, const char *verify, int code, const char *status)
{
	tls_bundle_drop();
	setenv("TLS_FAKE_MODE", mode, 1);
	setenv("TLS_FAKE_INPUT", input, 1);
	CASE(name);
	CHECK_UINT(tls_helper_run(url, verify), code);
	CHECK(tls_status_is(status));
}

static bool file_wait(const char *path)
{
	unsigned int i;

	for (i = 0; i < 200; i++) {
		if (!access(path, F_OK))
			return true;
		usleep(10000);
	}
	return false;
}

static pid_t pid_file_read(const char *path)
{
	long value;
	FILE *f = fopen(path, "r");

	if (!f)
		return -1;
	if (fscanf(f, "%ld", &value) != 1)
		value = -1;
	fclose(f);
	return (pid_t)value;
}

static void test_tls_script(void)
{
	char path[128];

	if (!tls_fixture_ready)
		return;

	tls_script_case("tls script: an empty URL has the byte-exact status", "http-ok",
			tls_valid_dir, "", "0", 4, "Unspecified certificate server URL");

	CASE("tls script: every error invokes the optional event-post surface");
	CHECK_UINT(file_count(tls_event_out, "voip/nodeMAC/TLS-cert/error"), 1);
	CHECK_UINT(file_count(tls_event_out,
			      "{\"Reason\":\"Unspecified certificate server URL\"}"), 1);

	tls_script_case("tls script: a multi-word URL has the byte-exact status", "http-ok",
			tls_valid_dir, "http://cert.example/a b", "0", 4,
			"Unexpected space in certificate server URL");
	tls_script_case("tls script: an unsupported scheme has the byte-exact status",
			"http-ok", tls_valid_dir, "file:///tmp/voice.tgz", "0", 4,
			"Unsupported file transfer protocol");
	tls_script_case("tls script: an HTTP failure includes the returned status", "http-404",
			tls_valid_dir, "http://cert.example/voice.tgz", "0", 1,
			"Bad certificate server URL (404)");
	tls_script_case("tls script: the HTTP media type is byte-exact in the status",
			"http-wrong", tls_valid_dir, "https://cert.example/voice.tgz", "0", 1,
			"Wrong file type text/plain");
	tls_script_case("tls script: the HTTP size enforces the 5000 byte bound", "http-large",
			tls_valid_dir, "https://cert.example/large.tgz", "0", 1,
			"File size (5001) exceeded 5000");
	tls_script_case("tls script: a missing FTP 213 response has its fixed status", "ftp-missing",
			tls_valid_dir, "ftp://cert.example/voice.tgz", "0", 1,
			"File not found (FTP 213)");
	tls_script_case("tls script: a nonnumeric FTP size has its fixed status", "ftp-text",
			tls_valid_dir, "ftp://cert.example/voice.tgz", "0", 1,
			"File size not a number (FTP)");
	tls_script_case("tls script: the 5000 byte bound includes the returned size", "ftp-large",
			tls_valid_dir, "ftp://cert.example/voice.tgz", "0", 1,
			"File size (5001) exceeded 5000");
	tls_script_case("tls script: a valid local archive installs the fixed pair", "http-ok",
			tls_valid_dir, "https://cert.example/voice.tgz", "0", 0,
			"Certificate updated and applied");
	CHECK(tls_file_exists("cert.pem"));
	CHECK(tls_file_exists("key.pem"));
	CHECK_UINT(tls_file_mode("key.pem"), 0600);
	CHECK(file_count(tls_curl_out, "-g") >= 2);
	CHECK(file_count(tls_curl_out, "--connect-timeout 10") >= 2);
	CHECK(file_count(tls_curl_out, "-m 25") >= 2);
	CHECK(file_count(tls_curl_out, "--max-filesize 5000") >= 1);

	CASE("tls script: verification accepts a bundle that contains ca.crt");
	CHECK_UINT(tls_helper_run("https://cert.example/voice.tgz", "1"), 2);
	CHECK(tls_status_is("Certificate applied (etag unchanged)"));

	setenv("TLS_FAKE_MODE", "http-etag-two", 1);
	CASE("tls script: a changed HTTP ETag installs the second archive");
	CHECK_UINT(tls_helper_run("https://cert.example/voice.tgz", "1"), 0);
	CHECK(tls_status_is("Certificate updated and applied"));

	CASE("tls script: the same HTTP ETag skips the second archive apply");
	CHECK_UINT(tls_helper_run("https://cert.example/voice.tgz", "1"), 2);
	CHECK(tls_status_is("Certificate applied (etag unchanged)"));

	tls_path_get(path, sizeof(path), "key.pem");
	unlink(path);
	CASE("tls script: an equal ETag extracts again when key.pem is absent");
	CHECK_UINT(tls_helper_run("https://cert.example/voice.tgz", "1"), 0);
	CHECK(tls_file_exists("key.pem"));

	tls_bundle_drop();
	setenv("TLS_FAKE_MODE", "ftp-ok", 1);
	setenv("TLS_FAKE_INPUT", tls_valid_dir, 1);
	CASE("tls script: a valid FTP archive installs before the md5 check");
	CHECK_UINT(tls_helper_run("ftp://cert.example/voice.tgz", "0"), 0);
	CHECK(tls_status_is("Certificate updated and applied"));
	setenv("TLS_FAKE_MODE", "ftp-keep", 1);
	CASE("tls script: an identical FTP archive skips the second apply");
	CHECK_UINT(tls_helper_run("ftp://cert.example/voice.tgz", "0"), 2);
	CHECK(tls_status_is("Certificate applied (md5sum unchanged)"));
	tls_path_get(path, sizeof(path), "key.pem");
	unlink(path);
	CASE("tls script: an equal FTP archive extracts again when key.pem is absent");
	CHECK_UINT(tls_helper_run("ftp://cert.example/voice.tgz", "0"), 0);
	CHECK(tls_file_exists("key.pem"));

	tls_script_case("tls script: an archive without the fixed pair clears the directory",
			"http-ok", tls_no_key_dir, "http://cert.example/missing.tgz", "0", 3,
			"Files cert.pem and key.pem not found");
	CHECK(!tls_file_exists("cert.pem"));

	text_file_put(tls_event_out, "", 0600);
	tls_script_case("tls script: verify overwrites a missing-pair terminal status",
			"http-ok", tls_no_key_dir, "http://cert.example/missing.tgz", "1", 3,
			"File ca.crt not found");
	CHECK_UINT(file_count(tls_event_out,
			      "{\"Reason\":\"Files cert.pem and key.pem not found\"}"), 1);
	CHECK_UINT(file_count(tls_event_out,
			      "{\"Reason\":\"File ca.crt not found\"}"), 1);

	tls_script_case("tls script: verification requires ca.crt and clears the pair", "http-ok",
			tls_no_ca_dir, "http://cert.example/no-ca.tgz", "1", 3,
			"File ca.crt not found");
	CHECK(!tls_file_exists("cert.pem"));

	tls_bundle_drop();
	text_file_put(tls_event_out, "", 0600);
	CASE("tls script: a repeated reason posts one error event");
	CHECK_UINT(tls_helper_run("", "0"), 4);
	CHECK_UINT(tls_helper_run("", "0"), 4);
	CHECK_UINT(file_count(tls_event_out,
			      "{\"Reason\":\"Unspecified certificate server URL\"}"), 1);
	CHECK_UINT(tls_helper_run("file:///voice.tgz", "0"), 4);
	CHECK_UINT(file_count(tls_event_out, "{\"Reason\":\""), 2);

	tls_path_get(path, sizeof(path), "extra.file");
	CHECK(text_file_put(path, "extra", 0600));
	snprintf(path, sizeof(path), "%s/.hidden", tls_dir);
	CHECK(text_file_put(path, "hidden", 0600));
	CHECK(!mkdir("/tmp/uvoice", 0700) || errno == EEXIST);
	CHECK(text_file_put(TLS_KEYLOG, "keylog", 0600));
	CHECK(text_file_put(TLS_KEYLOG ".old", "old", 0600));
	CASE("tls script: clear empties the bundle, key logs, and reason record");
	CHECK_UINT(tls_clear_run(), 0);
	CHECK(tls_status_is("TLS disabled"));
	CHECK(!tls_file_exists("extra.file"));
	snprintf(path, sizeof(path), "%s/.hidden", tls_dir);
	CHECK(access(path, F_OK));
	CHECK(access(TLS_KEYLOG, F_OK));
	CHECK(access(TLS_KEYLOG ".old", F_OK));
	CHECK_UINT(tls_helper_run("", "0"), 4);
	CHECK_UINT(file_count(tls_event_out,
			      "{\"Reason\":\"Unspecified certificate server URL\"}"), 2);

	tls_bundle_drop();
	unlink(tls_status);
	unlink(tls_curl_pid);
	unlink(tls_curl_term);
	setenv("TLS_FAKE_MODE", "hold", 1);
	setenv("TLS_FAKE_INPUT", tls_valid_dir, 1);
	CASE("tls script: every run writes the byte-exact N/A initial status");
	{
		pid_t pid = fork();
		int status;

		if (pid == 0) {
			setpgid(0, 0);
			execl(tls_helper, tls_helper, "http://cert.example/slow.tgz", "", "", "0",
			      NULL);
			_exit(127);
		}
		CHECK(pid > 0);
		if (pid < 0)
			return;
		setpgid(pid, pid);
		CHECK(file_wait(tls_curl_pid));
		CHECK(tls_status_is("N/A"));
		/* The group signal also ends the foreground sleep of the held
		 * curl, so its trap runs now, not at the end of that sleep. */
		kill(-pid, SIGTERM);
		CHECK(waitpid(pid, &status, 0) == pid);
		CHECK(file_wait(tls_curl_term));
	}
}

/* --------------------------------------------------------- the real spawn */

/*
 * Everything above drives the test mode. This class drives the real path, the
 * one the device runs: the spawn and the uloop_process reap. UVOICE_EVENT_POST
 * names a generated script that appends its arguments to a file, so the
 * published vector is read back from what the child actually received, not
 * from the recorder. The line stays busy until uloop reaps the child, so the
 * second post is the proof that the reap ran.
 */
static char spawn_script[64];
static char spawn_out[64];
static char spawn_tls_script[64];
/* passes event_script_ok(), and its exec can never succeed */
static char spawn_dead_script[64];
/* records its own pid and stays alive until the stop file lands */
static char spawn_hold_script[64];
static char spawn_pid_file[96];
static char spawn_stop_file[96];

static bool spawn_fixture_make(void)
{
	FILE *f;
	int fd;

	snprintf(spawn_out, sizeof(spawn_out), "/tmp/uvoice-event-out-XXXXXX");
	fd = mkstemp(spawn_out);
	if (fd < 0)
		return false;
	close(fd);

	snprintf(spawn_script, sizeof(spawn_script), "/tmp/uvoice-event-run-XXXXXX");
	fd = mkstemp(spawn_script);
	if (fd < 0)
		return false;

	f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		return false;
	}

	fprintf(f, "#!/bin/sh\nprintf '%%s\\n' \"$@\" >> %s\n", spawn_out);
	fclose(f);

	if (chmod(spawn_script, 0755))
		return false;

	snprintf(spawn_tls_script, sizeof(spawn_tls_script),
		 "/tmp/uvoice-tls-exit-XXXXXX");
	fd = mkstemp(spawn_tls_script);
	if (fd < 0)
		return false;
	close(fd);

	if (!text_file_put(spawn_tls_script, "#!/bin/sh\nexit 0\n", 0755))
		return false;

	snprintf(spawn_dead_script, sizeof(spawn_dead_script),
		 "/tmp/uvoice-spawn-dead-XXXXXX");
	fd = mkstemp(spawn_dead_script);
	if (fd < 0)
		return false;
	close(fd);

	if (!text_file_put(spawn_dead_script, "#!/nonexistent/uvoice/sh\nexit 0\n", 0755))
		return false;

	snprintf(spawn_hold_script, sizeof(spawn_hold_script),
		 "/tmp/uvoice-spawn-hold-XXXXXX");
	fd = mkstemp(spawn_hold_script);
	if (fd < 0)
		return false;
	close(fd);

	snprintf(spawn_pid_file, sizeof(spawn_pid_file), "%s.pid", spawn_hold_script);
	snprintf(spawn_stop_file, sizeof(spawn_stop_file), "%s.stop", spawn_hold_script);

	/*
	 * The pid lands through a rename, so a reader that finds the file finds
	 * the whole number. The child then stays alive until the test drops the
	 * stop file, and gives up by itself after ten seconds, so a case that
	 * fails before it can stop the child leaves nothing behind.
	 */
	return text_file_put(spawn_hold_script,
		"#!/bin/sh\n"
		"printf '%s' \"$$\" > \"$UVOICE_SPAWN_PID.tmp\"\n"
		"mv \"$UVOICE_SPAWN_PID.tmp\" \"$UVOICE_SPAWN_PID\"\n"
		"n=0\n"
		"while [ ! -e \"$UVOICE_SPAWN_STOP\" ] && [ \"$n\" -lt 200 ]; do\n"
		" sleep 0.05\n"
		" n=$((n + 1))\n"
		"done\n"
		"exit 0\n", 0755);
}

static void spawn_fixture_drop(void)
{
	unlink(spawn_script);
	unlink(spawn_tls_script);
	unlink(spawn_dead_script);
	unlink(spawn_hold_script);
	unlink(spawn_pid_file);
	unlink(spawn_stop_file);
	unlink(spawn_out);
}

/* The reader stops here, so it must stay ahead of what the classes below
 * publish: a bound the file reaches turns the next case's line count into a
 * silent equality instead of a failure. */
#define SPAWN_LINES_MAX	60

static unsigned int spawn_lines(char out[SPAWN_LINES_MAX][EVENT_DATA_MAX])
{
	char line[EVENT_DATA_MAX];
	unsigned int n = 0;
	FILE *f = fopen(spawn_out, "r");

	if (!f)
		return 0;

	while (n < SPAWN_LINES_MAX && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		snprintf(out[n++], EVENT_DATA_MAX, "%s", line);
	}

	fclose(f);

	return n;
}

static struct uloop_timeout pump_timer;
static unsigned int pump_ticks;

static void pump_cb(struct uloop_timeout *t)
{
	if (!event_test_busy(0) || ++pump_ticks > 400) {
		uloop_end();
		return;
	}

	uloop_timeout_set(t, 5);
}

/* run uloop until line 0 is reaped, or 2 s, whichever comes first */
static void pump(void)
{
	pump_ticks = 0;
	pump_timer.cb = pump_cb;
	uloop_timeout_set(&pump_timer, 5);
	uloop_run();
}

static void tls_pump_cb(struct uloop_timeout *t)
{
	if (!event_test_tls_busy() || ++pump_ticks > 400) {
		uloop_end();
		return;
	}

	uloop_timeout_set(t, 5);
}

static void tls_pump(void)
{
	pump_ticks = 0;
	pump_timer.cb = tls_pump_cb;
	uloop_timeout_set(&pump_timer, 5);
	uloop_run();
}

/*
 * A loopback listener of the shape account.c opens through sip_stream: stream,
 * SO_REUSEADDR, CLOEXEC. A zero port takes an ephemeral one and reports it
 * back, so the same port can be asked for again after the close.
 */
static int listen_port_open(uint16_t *port)
{
	struct sockaddr_in sa;
	socklen_t len = sizeof(sa);
	int on = 1;
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = htons(*port);

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0 ||
	    bind(fd, (const struct sockaddr *)&sa, sizeof(sa)) < 0 ||
	    listen(fd, 8) < 0)
		goto fail;

	if (!*port) {
		if (getsockname(fd, (struct sockaddr *)&sa, &len) < 0)
			goto fail;
		*port = ntohs(sa.sin_port);
	}

	return fd;

fail:
	close(fd);
	return -1;
}

/*
 * An exec that cannot succeed. The script is a regular file with the execute
 * bit, so the soft gate accepts it. Its interpreter line names a file that
 * does not exist. A plain fork and execv would answer with a child that
 * exits 127, and the caller would learn that only at the reap. The post would
 * count, the line would go busy, and the state would read reported although
 * nothing ran. The spawn answers at once, with ENOENT, and the failure
 * contract of the layer does the rest.
 */
static void spawn_dead_interpreter_cases(void)
{
	char got[SPAWN_LINES_MAX][EVENT_DATA_MAX];
	unsigned int suppressed;
	unsigned int refused;
	unsigned int starts;
	unsigned int posts;
	unsigned int lines;

	setenv("UVOICE_EVENT_POST", spawn_script, 1);
	event_init();
	post_down(0);
	pump();

	CASE("spawn: a dead interpreter fails the post itself, so no child is in flight");
	setenv("UVOICE_EVENT_POST", spawn_dead_script, 1);
	event_init();
	CHECK(event_available());
	posts = event_test_posts();
	suppressed = event_test_suppressed();
	post_up(0, "198.51.100.10");
	CHECK_UINT(event_test_posts(), posts);
	CHECK_UINT(event_test_suppressed(), suppressed);
	CHECK(!event_test_busy(0));

	/* The script is the oracle, not the counter: a layer that believed the
	 * dead child had published the state suppresses this post as a
	 * duplicate and the script receives nothing. */
	CASE("spawn: the state a failed exec never published is posted by the next edge");
	lines = spawn_lines(got);
	setenv("UVOICE_EVENT_POST", spawn_script, 1);
	event_init();
	post_up(0, "198.51.100.10");
	CHECK_UINT(event_test_posts(), posts + 1);
	CHECK(event_test_busy(0));
	pump();
	CHECK(!event_test_busy(0));
	CHECK_UINT(spawn_lines(got), lines + 5);

	CASE("spawn: a dead interpreter fails the TLS helper start and arms its retry");
	tls_bundle_drop();
	starts = event_test_tls_starts();
	refused = event_test_tls_refused();
	tls_done_calls = 0;
	setenv("UVOICE_TLS_CERT", spawn_dead_script, 1);
	event_tls_cert_start("https://cert.example/dead.tgz", "", "", false, tls_done);
	CHECK(!event_test_tls_busy());
	/* the gate accepted the script: the failure is the exec, not the
	 * path */
	CHECK_UINT(event_test_tls_refused(), refused);
	CHECK_UINT(event_test_tls_starts(), starts + 1);
	CHECK(event_test_tls_retry_pending());
	CHECK_UINT(tls_done_calls, 0);
	event_tls_cert_stop(EVENT_TLS_STOP_SHUTDOWN);
	unsetenv("UVOICE_TLS_CERT");
}

/*
 * The process group. The cases read it from the child itself. The TLS helper
 * leads a group of its own, so that a stop kills the curl that it started
 * (event.h). The event publisher stays in the group of the daemon. The spawn
 * takes the group before the exec, so the answer is already true when the
 * call returns.
 */
static void spawn_group_cases(void)
{
	pid_t child;

	setenv("UVOICE_SPAWN_PID", spawn_pid_file, 1);
	setenv("UVOICE_SPAWN_STOP", spawn_stop_file, 1);

	CASE("spawn: the TLS helper child leads a process group of its own");
	unlink(spawn_pid_file);
	unlink(spawn_stop_file);
	setenv("UVOICE_TLS_CERT", spawn_hold_script, 1);
	event_tls_cert_start("https://cert.example/group.tgz", "", "", false, tls_done);
	CHECK(event_test_tls_busy());
	CHECK(file_wait(spawn_pid_file));
	child = pid_file_read(spawn_pid_file);
	CHECK(child > 0);
	CHECK_UINT(getpgid(child), (uint64_t)child);
	CHECK(getpgid(child) != getpgid(0));
	CHECK(text_file_put(spawn_stop_file, "", 0600));
	tls_pump();
	CHECK(!event_test_tls_busy());
	event_tls_cert_stop(EVENT_TLS_STOP_SHUTDOWN);
	unsetenv("UVOICE_TLS_CERT");

	CASE("spawn: the event helper child stays in the daemon's process group");
	unlink(spawn_pid_file);
	unlink(spawn_stop_file);
	setenv("UVOICE_EVENT_POST", spawn_script, 1);
	event_init();
	post_up(0, "198.51.100.10");
	pump();
	setenv("UVOICE_EVENT_POST", spawn_hold_script, 1);
	event_init();
	post_down(0);
	CHECK(event_test_busy(0));
	CHECK(file_wait(spawn_pid_file));
	child = pid_file_read(spawn_pid_file);
	CHECK(child > 0);
	CHECK_UINT(getpgid(child), (uint64_t)getpgid(0));
	CHECK(text_file_put(spawn_stop_file, "", 0600));
	pump();
	CHECK(!event_test_busy(0));

	unlink(spawn_pid_file);
	unlink(spawn_stop_file);
	unsetenv("UVOICE_SPAWN_PID");
	unsetenv("UVOICE_SPAWN_STOP");
}

/*
 * The property that the shared spawn exists for, over the production path.
 * account.c releases a SIP listener and binds a new socket on the same port
 * in one frame, microseconds after the start of a helper. A child that holds
 * a copy of the listening descriptor keeps the socket in TCP_LISTEN. Linux
 * then refuses the bind, whatever SO_REUSEADDR says on either side, and the
 * line comes up with no inbound path at all. The case checks that the spawn
 * closes the descriptors of the daemon in the child before the call returns
 * (spawn_run.h). Without that order, the case fails on the schedule of the
 * child.
 */
static void spawn_listener_cases(void)
{
	unsigned int rebinds = 0;
	unsigned int spawns = 0;
	unsigned int posts;
	unsigned int i;

	CASE("spawn: a listener released right after a spawn binds again, fifty times over");
	setenv("UVOICE_EVENT_POST", spawn_script, 1);
	event_init();
	post_down(0);
	pump();
	posts = event_test_posts();

	for (i = 0; i < 50; i++) {
		uint16_t port = 0;
		int held = listen_port_open(&port);
		int again;

		if (held < 0)
			break;

		/* the edge alternates, so every iteration really spawns */
		event_reg_post(0, (i % 2) == 0, "198.51.100.10");
		spawns++;

		close(held);
		again = listen_port_open(&port);
		if (again >= 0) {
			rebinds++;
			close(again);
		}

		pump();
	}

	CHECK_UINT(spawns, 50);
	CHECK_UINT(event_test_posts(), posts + 50);
	CHECK_UINT(rebinds, 50);
}

/*
 * The stdout of the helper is /dev/null, and its stderr stays the stderr of
 * the daemon (spawn_run.h). Reason: procd logs the stdio of the daemon under
 * the name of the daemon. The firewall helper prints a line for each delete
 * of a rule that does not exist. The case swaps its own stdio for two
 * pipes around the spawn, and reads what the child wrote to each.
 */
static void test_spawn_stdio(void)
{
	char *argv[] = { "/bin/sh", "-c", "echo out; echo err 1>&2", NULL };
	char buf[64];
	int out[2], err[2], saved_out, saved_err, status = 0;
	ssize_t n;
	pid_t pid;

	CASE("spawn: the child's stdout is silent and its stderr is the daemon's");
	CHECK(pipe(out) == 0);
	CHECK(pipe(err) == 0);
	fflush(stdout);
	fflush(stderr);
	saved_out = dup(STDOUT_FILENO);
	saved_err = dup(STDERR_FILENO);
	dup2(out[1], STDOUT_FILENO);
	dup2(err[1], STDERR_FILENO);
	pid = spawn_run(argv, SPAWN_GROUP_DAEMON);
	dup2(saved_out, STDOUT_FILENO);
	dup2(saved_err, STDERR_FILENO);
	close(saved_out);
	close(saved_err);
	close(out[1]);
	close(err[1]);
	CHECK(pid > 0);
	if (pid > 0)
		CHECK(waitpid(pid, &status, 0) == pid);
	n = read(out[0], buf, sizeof(buf));
	CHECK_UINT(n, 0);
	n = read(err[0], buf, sizeof(buf) - 1);
	CHECK_UINT(n, 4);
	if (n > 0) {
		buf[n] = '\0';
		CHECK(!strcmp(buf, "err\n"));
	}
	close(out[0]);
	close(err[0]);
}

static void test_real_spawn(void)
{
	char got[SPAWN_LINES_MAX][EVENT_DATA_MAX];
	struct rlimit lim, save;
	unsigned int n;

	event_test_mode(false);
	CHECK(spawn_fixture_make());
	setenv("UVOICE_EVENT_POST", spawn_script, 1);
	event_init();
	uloop_init();

	CASE("spawn: the real spawn hands the whole vector to the script");
	CHECK(event_available());
	post_up(0, "198.51.100.10");
	CHECK_UINT(event_test_posts(), 1);
	CHECK(event_test_busy(0));
	pump();
	n = spawn_lines(got);
	CHECK_UINT(n, 5);
	CHECK(!strcmp(got[0], "-p"));
	CHECK(!strcmp(got[1], "info"));
	CHECK(!strcmp(got[2], "-t"));
	CHECK(!strcmp(got[3], TOPIC_UP_1));
	CHECK(!strcmp(got[4], "{\"Proxy\":\"198.51.100.10\"}"));

	CASE("spawn: the uloop reap un-busies the line and the next change fires");
	CHECK(!event_test_busy(0));
	post_down(0);
	CHECK_UINT(event_test_posts(), 2);
	pump();
	n = spawn_lines(got);
	CHECK_UINT(n, 10);
	CHECK(!strcmp(got[6], "crit"));
	CHECK(!strcmp(got[8], TOPIC_DOWN_1));
	CHECK(!strcmp(got[9], "{}"));

	CASE("spawn: a change during a real post waits for the real reap");
	post_up(0, "198.51.100.10");
	CHECK(event_test_busy(0));
	post_down(0);
	CHECK_UINT(event_test_posts(), 3);
	pump();
	CHECK_UINT(event_test_posts(), 4);
	CHECK_UINT(spawn_lines(got), 20);

	/*
	 * A spawn that fails must leave the state unreported, or the same state
	 * on the next edge would be suppressed as a duplicate of a post that
	 * never ran and the line would go silent. RLIMIT_NPROC at 1 makes the
	 * spawn fail with EAGAIN for an unprivileged process; the CHECK below
	 * asserts it really failed, so an environment where it does not fails
	 * the case rather than skipping it silently.
	 */
	CASE("spawn: a failed spawn reports nothing, so the next edge posts the same state");
	pump();
	CHECK(!event_test_busy(0));
	CHECK(!getrlimit(RLIMIT_NPROC, &save));
	lim = save;
	lim.rlim_cur = 1;
	CHECK(!setrlimit(RLIMIT_NPROC, &lim));
	post_up(0, "198.51.100.10");
	CHECK(!setrlimit(RLIMIT_NPROC, &save));
	CHECK_UINT(event_test_posts(), 4);
	CHECK(!event_test_busy(0));
	post_up(0, "198.51.100.10");
	CHECK_UINT(event_test_posts(), 5);
	pump();
	CHECK_UINT(spawn_lines(got), 25);

	CASE("spawn: the real TLS helper path reaps through uloop and calls done");
	CHECK(tls_bundle_make());
	tls_done_calls = 0;
	setenv("UVOICE_TLS_CERT", spawn_script, 1);
	event_tls_cert_start("https://cert.example/voice.tgz", "alice", "secret", true,
			     tls_done);
	CHECK(event_test_tls_busy());
	tls_pump();
	CHECK(!event_test_tls_busy());
	CHECK_UINT(tls_done_calls, 1);
	n = spawn_lines(got);
	CHECK_UINT(n, 29);
	CHECK(!strcmp(got[25], "https://cert.example/voice.tgz"));
	CHECK(!strcmp(got[26], "alice"));
	CHECK(!strcmp(got[27], "secret"));
	CHECK(!strcmp(got[28], "1"));
	event_tls_cert_stop(EVENT_TLS_STOP_SHUTDOWN);
	tls_bundle_drop();

	CASE("spawn: real exit 3 calls done and arms the absent-bundle retry");
	CHECK(text_file_put(spawn_tls_script, "#!/bin/sh\nexit 3\n", 0755));
	setenv("UVOICE_TLS_CERT", spawn_tls_script, 1);
	tls_done_calls = 0;
	event_tls_cert_start("https://cert.example/exit-3.tgz", "", "", false, tls_done);
	CHECK(event_test_tls_busy());
	tls_pump();
	CHECK_UINT(tls_done_calls, 1);
	CHECK(event_test_tls_retry_pending());
	event_tls_cert_stop(EVENT_TLS_STOP_SHUTDOWN);

	CASE("spawn: real exit 2 calls no callback and arms no bundle retry");
	CHECK(tls_bundle_make());
	CHECK(text_file_put(spawn_tls_script, "#!/bin/sh\nexit 2\n", 0755));
	tls_done_calls = 0;
	event_tls_cert_start("https://cert.example/exit-2.tgz", "", "", false, tls_done);
	CHECK(event_test_tls_busy());
	tls_pump();
	CHECK_UINT(tls_done_calls, 0);
	CHECK(!event_test_tls_retry_pending());
	event_tls_cert_stop(EVENT_TLS_STOP_SHUTDOWN);
	tls_bundle_drop();

	CASE("spawn: a missing TLS helper increments the real refusal counter");
	setenv("UVOICE_TLS_CERT", "/nonexistent/uvoice/tls-helper", 1);
	event_tls_cert_start("https://cert.example/missing-helper.tgz", "", "", false,
			     tls_done);
	CHECK_UINT(event_test_tls_refused(), 1);
	CHECK(!event_test_tls_busy());
	CHECK(event_test_tls_retry_pending());
	event_tls_cert_stop(EVENT_TLS_STOP_SHUTDOWN);

	CASE("spawn: a configuration stop terminates curl and then runs clear");
	unlink(tls_curl_pid);
	unlink(tls_curl_term);
	setenv("TLS_FAKE_MODE", "hold", 1);
	setenv("TLS_FAKE_INPUT", tls_valid_dir, 1);
	setenv("UVOICE_TLS_CERT", tls_helper, 1);
	event_tls_cert_start("http://cert.example/hold.tgz", "", "", false, tls_done);
	CHECK(event_test_tls_busy());
	CHECK(file_wait(tls_curl_pid));
	event_tls_cert_stop(EVENT_TLS_STOP_CONFIG);
	tls_pump();
	{
		/*
		 * The wait is bounded, not immediate. tls_pump() returns on
		 * the daemon's timeline, when the killed fetch has been reaped
		 * and the clear child has run; curl's own trap is on a
		 * timeline nothing orders against it, and it lands about 2 ms
		 * after the group signal because the trap waits for the
		 * foreground sleep to die first. A single read of the file is
		 * a race the daemon wins under load. The wait keeps the oracle:
		 * a curl the group kill never reached still fails, at the
		 * timeout.
		 */
		bool terminated = file_wait(tls_curl_term);
		pid_t curl_pid = pid_file_read(tls_curl_pid);

		if (!terminated && curl_pid > 0)
			kill(curl_pid, SIGTERM);
		CHECK(terminated);
	}
	CHECK(!event_test_tls_busy());
	CHECK(tls_status_is("TLS disabled"));
	unsetenv("UVOICE_TLS_CERT");

	CASE("spawn: event_done reaps a real child still in flight");
	post_down(0);
	CHECK(event_test_busy(0));
	event_done();
	CHECK(!event_test_busy(0));

	spawn_dead_interpreter_cases();
	spawn_group_cases();
	spawn_listener_cases();

	uloop_done();
	unsetenv("UVOICE_EVENT_POST");
	spawn_fixture_drop();
}

int main(void)
{
	test_dedup();
	test_dedup_per_line();
	test_topics();
	test_argv();
	test_proxy();
	test_gate();
	test_inflight();
	test_done();
	test_tls_helper();
	test_tls_script();
	test_spawn_stdio();
	test_real_spawn();
	if (tls_fixture_ready)
		tls_fixture_drop();

	return tu_summary("test_event");
}
