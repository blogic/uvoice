// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The voip registration events.
 *
 * What it publishes. Two topics, and only those two. The cloud consumers key
 * on the strings, so they are fixed:
 *
 *   voip/nodeMAC/line-<N>/reg/successful   prio info  {"Proxy": "<registrar>"}
 *   voip/nodeMAC/line-<N>/reg/failed       prio crit  {}
 *
 * N is one based, and the event schema documents N = 1 or 2. This layer takes
 * the daemon's 0 based line index and adds the one.
 *
 * The nodeMAC token is literal. event-post-now.sh substitutes it with MFG_MAC
 * ("topic=${topic/nodeMAC/$MFG_MAC}"), so the daemon never learns the node MAC
 * and never passes -m.
 *
 * How it publishes. It runs /usr/srg/scripts/event-post.sh with an argv array
 * through the shared spawn (spawn_run.h), and reaps the child through
 * uloop_process, the way fw.c drives firewall_exception. No shell: an argv
 * array needs no shell quoting and cannot be re-parsed.
 *
 * Soft gate. The script belongs to the eventmgr package, which uvoice does not
 * depend on, and it is absent on the build host. event_init() checks the path
 * at start, and a post checks it when no check ran before. An absent or
 * non-executable script turns the whole layer off with one trace. There is no
 * uci option: the gate is the switch.
 *
 * State, not events. A failed registration retries every backoff tick, so the
 * failure edge would otherwise post an event per attempt. This layer keeps a
 * per line tri-state (unknown, up, down) and fires only when the state
 * changes. One spawn at a time per line: a change that arrives while a post
 * is in flight is remembered as the latest state, not appended to a queue, and
 * fires when the reap lands. A state that is back to what was already reported
 * by then fires nothing.
 */
#ifndef UVOICE_EVENT_H
#define UVOICE_EVENT_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Lines this layer tracks. CFG_CHANNEL_MAX is 2 and the fixed topic
 * vocabulary names line-1 and line-2; the array is sized above it so a channel
 * count growth needs no change here, and a line at or above the bound is
 * refused rather than folded onto another line's dedup state.
 */
#define EVENT_LINE_MAX		8

/* A host name or an IP literal, the same size class as SIP_SOCK_HOST_MAX. */
#define EVENT_PROXY_MAX		64

/* "voip/nodeMAC/line-<N>/reg/successful" */
#define EVENT_TOPIC_MAX		64

/* {"Proxy":"<EVENT_PROXY_MAX-1 bytes>"} */
#define EVENT_DATA_MAX		96

/* script, -p prio, -t topic, the json, and the NULL the spawn needs */
#define EVENT_ARGV_MAX		8

/*
 * Evaluate the soft gate. Optional: the first post evaluates it anyway, so a
 * caller that never calls this still behaves. Calling it at start moves the
 * "script absent" trace to a place where it is read, and calling it again
 * re-evaluates, which is what lets a test point UVOICE_EVENT_POST somewhere
 * else. UVOICE_EVENT_POST overrides the script path (the UVOICE_UCI_CONFDIR
 * rule of cfg/uci.c: a host test aid, unset on the device).
 */
void event_init(void);

/*
 * The registration state of one line changed. line is the daemon's 0 based
 * line index; the topic carries line + 1. up true posts reg/successful at
 * priority info with proxy as the EventData Proxy value, up false posts
 * reg/failed at priority crit with an empty EventData and ignores proxy.
 *
 * proxy is the configured next hop of the account, the name the config names,
 * not the address the attempt resolved to. The cloud consumers read the field
 * as the configured string: a resolved address changes the field's shape and
 * churns on every failover, while the configured name is stable. A NULL,
 * empty, over-long or non-host value drops the Proxy field and the event
 * still fires with {}: the state change is the news, the address is
 * decoration.
 *
 * Deduplicated per line: a call that repeats the state already reported posts
 * nothing. The call returns at once; the script runs asynchronously.
 */
void event_reg_post(unsigned int line, bool up, const char *proxy);

enum event_tls_stop_mode {
	EVENT_TLS_STOP_CONFIG = 0,
	EVENT_TLS_STOP_SHUTDOWN,
};

/*
 * Fetch the fixed TLS bundle through /usr/sbin/uvoice_tls_cert.sh. The helper
 * receives url, username, password and the verification flag as four separate
 * arguments. Exit 0 installed a bundle, 1 is retryable, 2 is unchanged, 3
 * cleared an invalid bundle, and 4 refused the configuration. Exit 0 and 3
 * call done. Missing bundle files arm a 30 second retry, except after exit 4.
 * A later call replaces the arguments after the current child exits.
 */
void event_tls_cert_start(const char *url, const char *username, const char *password,
			  bool verify, void (*done)(void));

/* A configuration stop clears the bundle. A shutdown stop only reaps the child. */
void event_tls_cert_stop(enum event_tls_stop_mode mode);

/*
 * Final teardown at process exit. Reaps a post still in flight on every line
 * and forgets a queued state. The waitpid is unbounded, the fw_shutdown() rule:
 * event-post.sh returns as soon as it has backgrounded its worker, and its own
 * 2 s gate lock bounds the wait in practice, but nothing in this daemon caps
 * it. uloop must still be valid.
 */
void event_done(void);

/* Whether the publish script is present and executable. Evaluates the gate. */
bool event_available(void);

/*
 * Test mode, never used by the daemon, on the fw.c shape. event_test_mode()
 * resets the layer and makes it believe the script exists while suppressing
 * the spawn, so a post records its vector and the fixture completes the child
 * by hand with event_test_reap(). event_test_mode(false) resets it back to the
 * real spawn, and the recorder and the counters keep working there, so a
 * fixture that points UVOICE_EVENT_POST at a script of its own and pumps
 * uloop drives the whole spawn and reap path. The recorder holds the first
 * eight posts since the last reset; the counters keep counting past it. Calling
 * event_init() afterwards re-evaluates the gate without leaving test mode,
 * which is how a fixture drives the refusal class.
 *
 * The four counters are the observable classes: posts handed to the spawner,
 * posts refused by the gate or a bad line, posts suppressed as duplicates at
 * entry, and states that collapsed at reap time because the post in flight had
 * already reported them.
 *
 * event_test_reap()'s code is deliberately unused: the reap ignores the child's
 * exit status by design (event.c event_reap), and the parameter is there so the
 * test entry has the shape of the real uloop_process completion.
 *
 * The recorder copies the argument count the emitter states, never up to the
 * terminator, so a vector that lost its NULL cannot walk off the end.
 * event_test_arg() returns NULL past the end of a record, and
 * event_test_terminated() reports separately whether argv[argc] really was the
 * NULL the spawn needs.
 */
void event_test_mode(bool on);
void event_test_reap(unsigned int line, int code);
unsigned int event_test_posts(void);
unsigned int event_test_refused(void);
unsigned int event_test_suppressed(void);
unsigned int event_test_collapsed(void);
unsigned int event_test_recs(void);
unsigned int event_test_rec_line(unsigned int rec);
unsigned int event_test_argc(unsigned int rec);
const char *event_test_arg(unsigned int rec, unsigned int idx);
bool event_test_terminated(unsigned int rec);
bool event_test_busy(unsigned int line);

/* TLS helper controls on the same no-spawn test mode. */
void event_test_tls_reap(int code);
void event_test_tls_retry(void);
unsigned int event_test_tls_starts(void);
unsigned int event_test_tls_refused(void);
const char *event_test_tls_arg(unsigned int idx);
bool event_test_tls_terminated(void);
bool event_test_tls_busy(void);
bool event_test_tls_armed(void);
bool event_test_tls_retry_pending(void);

#endif
