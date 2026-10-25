// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#ifndef UVOICE_H
#define UVOICE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <libubox/list.h>
#include <libubox/uloop.h>

#include "cfg/cfg.h"
#include "hal/hal.h"
#include "posix.h"
#include "trace.h"

#ifndef UVOICE_VERSION
#error "UVOICE_VERSION must be supplied by the build; see src/CMakeLists.txt"
#endif

enum uvoice_hal_state {
	UVOICE_HAL_DOWN = 0,
	UVOICE_HAL_ATTACHED,
	UVOICE_HAL_STARTING,
	UVOICE_HAL_READY,
	UVOICE_HAL_FAILED,
};

/*
 * The last jitter buffer report of one (line, stream).
 *
 * Without this cache the HAL_EVENT_JB_UPDATE payload reaches only the default
 * trace arm of event_dispatch, which prints the type and drops the numbers. A
 * live call can then lose most of its received audio inside the playout
 * buffer while every RTP counter reads healthy, and nothing on the unit can
 * say so. This cache holds the last report of every stream, published by
 * "ubus call voice status".
 *
 * valid says a report arrived, which is a different answer from a report of
 * no drops, so the dump emits a slot only once it is set. The numbers are the
 * event payload unchanged (hal_event.u.jb) beside the stamp the record
 * travelled with.
 *
 * The surface is backend neutral. libudsp fills u.jb from its own jitter
 * buffer (core stream.c stream_jb_event).
 */
struct uvoice_jb {
	bool valid;
	uint32_t stamp_ms;
	uint64_t total;
	uint64_t drop;
	uint32_t size_ms;
	uint32_t jitter_avg;
};

/*
 * The last successful jitter buffer reading of one (line, stream). It is a
 * second cache and not a field of the one above.
 *
 * struct uvoice_jb is the event cache: four numbers a backend pushed at a
 * moment of its own choice, published by "voice status". This one is the
 * poll cache: the whole counter set the fast getter answered, kept so that
 * "voice jb" can still show a stream after its call ended. A caller asking
 * "how did that call end" gets the final numbers instead of a row that
 * vanished with the leg.
 *
 * Two writers and no timer. Every successful live read the "voice status"
 * handler makes updates it, and so does the teardown capture in line/line.c
 * jb_snapshot(), which already reads the counters for the call record: the
 * final numbers of a call therefore arrive even when nobody polled while it
 * ran. Nothing else writes it and nothing polls on its behalf.
 *
 * valid says a reading happened for this pair in this daemon's lifetime, which
 * is what separates "there is no stream right now" from "this pair never
 * answered". The status handler emits a row for every pair regardless (the
 * constant jb grid): a valid cache reads sample last_call and carries the
 * stamp, an invalid one reads sample never and carries zeros.
 *
 * stamp_ms is CLOCK_MONOTONIC milliseconds, the clock cdr.c samples for its
 * own durations. The daemon samples it here as well, and does not take an
 * instant from a record: hal_event.stamp_ms is a backend tick, "monotonic
 * within one run" (hal.h section 2), and no rule ties that tick to this host's
 * clock. The stub backend happens to read CLOCK_MONOTONIC for it; the
 * udsp backend counts engine ticks instead (udsp/src/core/clock.h).
 */
struct uvoice_jb_last {
	bool valid;
	uint32_t stamp_ms;
	struct hal_jb_stats jb;
};

/*
 * The line echo canceller counterpart of the jitter buffer poll cache. The
 * canceller belongs to the analog line, so this is one entry per line and not
 * per stream. "voice status" answers the last reading of a line after its call
 * ended, the same question the jb cache answers for a stream: how did that
 * call end. Two writers and no timer, the jb rule: every successful live read
 * of the status handler, and the teardown capture in line/line.c
 * lec_snapshot(), which reads the canceller as the call ends.
 *
 * valid carries the same meaning it carries above: a valid entry is the
 * sample last_call of the lec row, an invalid one is sample never.
 */
struct uvoice_lec_last {
	bool valid;
	uint32_t stamp_ms;
	struct hal_lec_stats lec;
};

/*
 * The last move the backend's answer-tone disabler made on one line
 * (HAL_EVENT_LEC_POLICY). It is a view and not state this daemon drives: the
 * DSP owns its canceller, and nothing here decides anything from these fields.
 * "voice status" reports it beside the counters, because the counters show the
 * effect of a hold and never its cause.
 *
 * The view has a known staleness, and one rule bounds it. The DSP drops its
 * latch when the last stream of the line stops and emits no record for that
 * drop (udsp/src/core/stream.c stream_lec_release), so a view kept past the
 * end of the call would report a hold the canceller no longer has.
 * line/line.c clears it when the line reaches its rest state (line_ev_run,
 * the IDLE settle), which is that teardown seen from this side.
 *
 * Read reason first. A LATCH and a DEMOTE name the tone and the feed it
 * arrived on; the two releases name neither, and their tone and direction
 * fields are zero rather than meaningful (hal.h, enum hal_lec_reason).
 *
 * stamp_ms is the CLOCK_MONOTONIC millisecond the daemon received the record
 * at, and not the tick the record travelled with. hal_event.stamp_ms is a
 * backend tick (hal.h section 2), which the udsp engine counts from its own
 * start, so an age taken against this host's clock would be wrong by the
 * uptime at that start. The row's other two instants are this same clock, so
 * one reader can subtract all three. valid is what says a move arrived at all,
 * which is why 0 is a legal instant here.
 */
struct uvoice_lec_policy {
	bool valid;
	uint8_t kind;			/* enum hal_passthrough, after the move */
	uint8_t reason;			/* enum hal_lec_reason */
	uint16_t tone;			/* enum hal_tone_detect, 0 on a release */
	uint8_t direction;		/* enum hal_tone_dir, 0 on a release */
	uint32_t stamp_ms;
};

struct uvoice {
	struct cfg *cfg;
	struct hal *hal;
	enum uvoice_hal_state hal_state;

	struct uloop_fd event_ufd;	/* core read end of the HAL socketpair */
	int event_wfd;			/* backend write end, owned by the core */
	struct list_head jobs;

	uint64_t events_read;
	uint64_t events_lost;		/* seq gaps, see hal.h section 2 */
	uint16_t event_seq;
	bool event_seq_valid;

	/*
	 * How many configurations this daemon ran on, counted from 1 at the
	 * load and moved by every reload that took. Every call trace snapshot
	 * carries it, so a reader tells a counter that moved under one
	 * configuration from one that moved across a reload.
	 */
	uint32_t config_generation;

	struct uvoice_jb jb[HAL_LINE_MAX][HAL_STREAM_MAX];
	struct uvoice_jb_last jb_last[HAL_LINE_MAX][HAL_STREAM_MAX];
	struct uvoice_lec_last lec_last[HAL_LINE_MAX];
	struct uvoice_lec_policy lec_policy[HAL_LINE_MAX];
};

extern struct uvoice uv;

/*
 * Whether a deadline taken from uvoice_mono_ms() is still ahead of an instant
 * from the same clock. The stamp is 32 bits and wraps every 2^32 ms of uptime,
 * so the difference is read as signed and a window that straddles the wrap
 * stays open for the rest of its length.
 */
static inline bool uvoice_deadline_ahead(uint32_t deadline_ms, uint32_t now_ms)
{
	return (int32_t)(deadline_ms - now_ms) > 0;
}

/*
 * The one writer of the jitter buffer poll cache, shared by its two callers so
 * the stamp and the validity rule cannot drift apart. A pair outside the HAL
 * widths is dropped rather than clamped: the caller already walked a cap that
 * fits.
 */
static inline void uvoice_jb_cache_set(unsigned int line, unsigned int stream,
				       const struct hal_jb_stats *jb)
{
	struct uvoice_jb_last *last;

	if (line >= HAL_LINE_MAX || stream >= HAL_STREAM_MAX || !jb)
		return;

	last = &uv.jb_last[line][stream];
	last->jb = *jb;
	/* the raw monotonic instant. The sample word of the row says the
	 * reading exists, so 0 is a legal instant here. */
	last->stamp_ms = uvoice_mono_ms();
	last->valid = true;
}

/* The line canceller counterpart, one entry per line. */
static inline void uvoice_lec_cache_set(unsigned int line,
					const struct hal_lec_stats *lec)
{
	struct uvoice_lec_last *last;

	if (line >= HAL_LINE_MAX || !lec)
		return;

	last = &uv.lec_last[line];
	last->lec = *lec;
	last->stamp_ms = uvoice_mono_ms();
	last->valid = true;
}

/*
 * The disabler view's one writer. A record naming a line outside the HAL widths
 * is dropped rather than clamped, the jb cache rule: a clamp would file one
 * line's policy under another one's name. The drop is silent because the caller
 * traces the record either way, so the move still reaches a log; the jb store
 * has to speak up instead because it drops the whole record.
 */
static inline void uvoice_lec_policy_set(unsigned int line,
					 const struct hal_event *ev)
{
	struct uvoice_lec_policy *p;

	if (line >= HAL_LINE_MAX || !ev)
		return;

	p = &uv.lec_policy[line];
	p->kind = ev->u.lec.kind;
	p->reason = ev->u.lec.reason;
	p->tone = ev->u.lec.tone;
	p->direction = ev->u.lec.direction;
	/* the daemon's receipt instant, not ev->stamp_ms: that field is a
	 * backend tick and the row's other instants are this host's clock */
	p->stamp_ms = uvoice_mono_ms();
	p->valid = true;
}

/* The teardown the DSP makes without a record, seen from this side. */
static inline void uvoice_lec_policy_clear(unsigned int line)
{
	if (line >= HAL_LINE_MAX)
		return;

	memset(&uv.lec_policy[line], 0, sizeof(uv.lec_policy[line]));
}

/*
 * Load the config again and apply it. 0 when it applied, -1 when the load
 * failed, and UVOICE_RELOAD_RESTART when the new config moves the signaling
 * protocol: the daemon keeps the running config, and only a restart applies
 * the move. main.c.
 */
#define UVOICE_RELOAD_RESTART	(-2)

int uvoice_reload(void);
const char *uvoice_hal_state_name(enum uvoice_hal_state state);

/*
 * Fill len bytes of buf from getrandom(2). False when the kernel refused the
 * read; buf then holds no usable entropy. uvoice_random.c.
 */
bool uvoice_random_bytes(void *buf, size_t len);

void job_add(struct hal_job *job);
void job_add_budget(struct hal_job *job, unsigned int budget_ms);
struct hal_job *job_take(uint32_t token);

int uvoice_ubus_init(const char *socket_path);
void uvoice_ubus_done(void);

struct blob_attr;
struct ubus_context;
struct ubus_request_data;

/*
 * The daemon's ubus connection: NULL before the first connect and after
 * uvoice_ubus_done(). Across a reconnect of ubus_auto_conn it answers the
 * context, which may be down for that moment. ubus.c.
 */
struct ubus_context *uvoice_ubus_ctx(void);
void uvoice_ubus_notify(const char *type, struct blob_attr *msg);

/* Answer a command verb with the line and the verb under "requested",
 * the reply shape of every command verb of the voice object. ubus.c. */
void uvoice_ubus_request_reply(struct ubus_context *ctx,
			       struct ubus_request_data *req, unsigned int line,
			       const char *verb);

#ifdef UVOICE_HAL_STUB
void stub_ubus_attach(struct ubus_context *ctx);
#endif

#endif
