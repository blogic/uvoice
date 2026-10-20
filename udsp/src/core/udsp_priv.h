// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The private udsp object. This header is private; include/udsp/udsp.h is
 * the public contract.
 *
 * Four threads live behind it. The engine tick owns the line machines, the
 * generators and every event a line produces. It blocks in the TDM read, runs
 * one period and writes one period. The slic thread owns the slic_ops instance.
 * It polls the hook, scans faults and runs SLIC records and slow jobs. The net
 * thread runs epoll over the RTP and RTCP descriptors. It batches with recvmmsg,
 * stamps CLOCK_MONOTONIC arrival time and unprotects SRTP. The emitter owns
 * every event fd write, assigns sequence numbers and runs the watchdog.
 * The caller supplies the control thread and one SLOW worker. One control mutex
 * serialises those two for the whole post-and-wait of a SYNC op.
 *
 * One live handle per process: the crash handler, the signal
 * dispositions and the safe-state block are process-wide, so a second
 * udsp_open returns NULL until the first handle is closed.
 */
#ifndef UDSP_CORE_UDSP_PRIV_H
#define UDSP_CORE_UDSP_PRIV_H

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "udsp/udsp.h"
#include "udsp/slic.h"

#include "../line/line.h"
#include "../slic/host_linux.h"
#include "../slic/plugin_load.h"

#include "chan.h"
#include "clock.h"
#include "cmd.h"
#include "engine.h"
#include "event_priv.h"
#include "fax_plugin.h"
#include "jb_trace.h"
#include "lec_capture.h"
#include "net.h"
#include "pkt_trace.h"
#include "slic_thread.h"
#include "snap.h"
#include "stream.h"
#include "thread.h"

/* The line echo canceller state the engine publishes for udsp_line_lec_stats,
 * one double buffer per line beside the per-stream jb_snap of stream.h
 * (snap.h). It lives on the handle and not in struct udsp_chan because every
 * published record of this library lives here. */
UDSP_SNAP_DECLARE(lec_snap, struct udsp_lec_stats);

/*
 * The last completed metallic line test of one line, the record
 * udsp_line_test_result copies out (udsp.h: the reserved interface, whose
 * bytes are opaque and which libudsp never decodes).
 *
 * The record travels the double buffer of snap.h, because a lock-free reader
 * over plain fields is a data race whatever guards it. gen is the validity
 * beside it, zero while no completed record exists: the slic thread clears it
 * before a test starts and publishes a new value after the record is stored,
 * and the reader loads it before and after its copy, so a record replaced
 * under the reader is refused instead of returned.
 */
UDSP_SNAP_DECLARE(line_test_snap, struct udsp_slic_mlt_result);

struct udsp_line_test_slot {
	_Atomic uint_least32_t gen;
	struct line_test_snap snap;
};

/*
 * The newest line-monitor reading of one line, the record udsp_line_monitor
 * copies out and decodes. The slic thread is its single writer, through the
 * same double buffer the canceller record travels. stamp_ms is
 * CLOCK_MONOTONIC milliseconds modulo 2^32 (udsp.h). stale says the last
 * read answered no data and the reading is the one before it.
 */
struct udsp_monitor_rec {
	struct udsp_slic_monitor m;
	uint32_t stamp_ms;
	uint8_t stale;
};

UDSP_SNAP_DECLARE(monitor_snap, struct udsp_monitor_rec);

/* The udsp_region_get record. The publisher is the control thread at open and
 * the slic thread once a region job's swap is applied, never the tick. Region
 * jobs serialize on the slic thread, so the snap has one writer at a time. */
UDSP_SNAP_DECLARE(region_view_snap, struct udsp_region_view);

UDSP_SNAP_DECLARE(engine_diag_snap, struct engine_stats);

/* Bounded-stop deadlines. The slic budget is the largest because a stop can
 * arrive during LBCal or a host_delay_ms loop. */
#define UDSP_STOP_ENGINE_MS	500u
#define UDSP_STOP_EMITTER_MS	100u
#define UDSP_STOP_SLIC_MS	5000u
/* A metallic line test drives the line for as long as the plug-in takes, and
 * it carries no cancel. The plug-in stops its poll loops at 90 seconds for the
 * run and 5 seconds for the abort. A few register transfers follow them, so
 * this figure covers the call with a margin. udsp_close therefore waits this
 * long for a test to return before it applies the budget above. The ordinary
 * budget would abandon the slic thread inside the test, leak the handle and
 * lose the completion. */
#define UDSP_STOP_TEST_MS	100000u
/* A recovery op holds the slic thread for up to about 4.3 s (slic.h
 * converter_restart). A DONE posted before the quit flag is seen waits
 * SLIC_ACK_WAIT_MS on an engine that is already gone. udsp_close waits this
 * long for a recovery to return before the ordinary budget applies. */
#define UDSP_STOP_RECOVER_MS	10000u
#define UDSP_SLIC_STACK_BYTES	(256 * 1024)
/* The probe's --settle default after slic_reset is released. */
#define UDSP_RESET_SETTLE_MS	100u
#define UDSP_RESET_HOLD_MS	20u
#define UDSP_WATCHDOG_MS	1000u

/* Why the line did not run at the rate the config asked for. udsp_open
 * records exactly one of these per degrade. */
enum udsp_rate_degrade {
	UDSP_RATE_SETTLED = 0,		/* the ask stands, no degrade */
	UDSP_RATE_DEGRADE_TDM,		/* the TDM device refused 16 kHz */
	UDSP_RATE_DEGRADE_NO_CAPS,	/* the plug-in reports no capability */
	UDSP_RATE_DEGRADE_NO_WIDEBAND,	/* the part cannot run wideband */
};

/* The engine catch-up figures a watchdog run already reported, with the engine
 * identity they belong to: a replacement engine counts from zero again
 * (engine.h serial), so the mark follows the engine and not the handle. */
struct udsp_hold_mark {
	uint64_t serial;
	uint64_t holds;
	uint64_t capture_drops;
	uint64_t trim_drops;
	uint64_t playback_expired;
	uint64_t rephases;
	uint64_t rephase_aborts;
	uint64_t rephase_withheld;
};

struct udsp {
	/* cfg.sample_rate carries the settled rate from the settle on, because
	 * the engine is recreated at every udsp_start and must reopen the TDM
	 * at the rate the line agreed on. */
	struct udsp_config cfg;
	struct udsp_cap cap;
	/* The settled line rate in Hz and its per-tick frame count, rate/100.
	 * One rate per handle, fixed before udsp_start; the tick reads them
	 * instead of a channel's copy. */
	unsigned rate;
	unsigned frame;
	/* The standing playback-ring latency in ms: the prefill the engine
	 * granted times the tick, cached at engine create for the E-model
	 * delay input (stream.c stream_stats_publish). */
	unsigned prefill_ms;
	/* enum udsp_rate_degrade and how many times the settle degraded, which
	 * is at most one per open. */
	uint8_t rate_reason;
	unsigned rate_degrades;
	/* Why the last engine open failed (engine.h). Written by
	 * udsp_engine_create on whichever thread calls it, read by the rate
	 * settle at open: only a geometry refusal may move the rate. The tdm
	 * record inside it feeds the failure line. */
	struct engine_open_verdict engine_fail;
	int event_fd;			/* the caller's, never written here */
	/*
	 * The emitter writes this descriptor, a dup libudsp owns. The hal core
	 * closes its own event fd as soon as ops->close returns, so an emitter
	 * thread that outlived a bounded-stop expiry would otherwise write into
	 * whatever the number was recycled for. The dup is closed only on the
	 * path where that thread is proven gone; the leak path keeps it open
	 * for the life of the process.
	 */
	int event_dup;
	uint64_t max_ticks;
	/* The dead-line init retry cadence in ms, read once at open. The
	 * production value is SLIC_DEAD_RETRY_MS; a test sets a short cadence
	 * through UDSP_SLIC_DEAD_RETRY_MS. The slic thread reads it alone. */
	uint32_t dead_retry_ms;
	/* The lost-end bound of a telephone-event the receive side renders, in
	 * ms, read once at open so the engine tick never runs a getenv. The
	 * production value is UDSP_TEV_PLAY_TIMEOUT_MS; a test sets a short
	 * bound through UDSP_TEV_PLAY_TIMEOUT_MS (rtp/rtp.h). */
	uint32_t tev_play_timeout_ms;

	pthread_mutex_t engine_lock;
	struct engine *engine;
	/* The engine can be freed on the slic thread, so diagnostics read a
	 * handle-owned copy. engine_lock serializes its watchdog publisher
	 * with engine teardown. */
	struct engine_diag_snap engine_diag;
	atomic_bool engine_diag_valid;
	struct udsp_clock clock;

	struct slic_host *host;
	struct slic_plugin *plugin;
	const struct udsp_slic_ops *ops;
	struct udsp_slic *slic;
	/* The T.38 gateway plug-in and the vtable it carries, both NULL when
	 * the configuration names none, the file will not load or the test
	 * override supplies the vtable (core/fax_plugin.h). */
	struct fax_plugin *fax_plugin;
	const struct udsp_fax_ops *fax_ops;
	struct udsp_thread slic_thread;
	/* The SCHED_FIFO priority the slic thread asked for, two below the
	 * engine's, 0 for none; the raw policy it holds now (SCHED_*, -1 when
	 * the read failed), which the thread stores after every change and
	 * the open log reads; and whether a refused change was logged. */
	int slic_prio;
	_Atomic int slic_policy;
	bool slic_policy_logged;	/* slic thread only */
	int slic_cmd_fd;
	int slic_tick_fd;
	atomic_bool slic_armed;
	atomic_bool slic_quit;
	struct cmd_ack slic_ack;
	struct slic_ring ring;
	struct slic_up_ring up;
	struct slic_jobq jobs;
	atomic_uint job_inflight;
	/* The line tests among them, queued or running. udsp_close reads it to
	 * choose the budget it waits with; the ordinary job count cannot say
	 * which kind is in flight. */
	atomic_uint line_test_jobs;
	/* The recoveries in flight on the slic thread, for the same reason. */
	atomic_uint recover_jobs;
	/* The passes since the last periodic monitor sample in the per-tick
	 * modes, and the IDLE deadline of the next one. Slic thread only. */
	unsigned slic_monitor_pass;
	uint64_t slic_next_monitor_us;
	unsigned periodic_pass;		/* slic thread only */
	uint64_t last_periodic_tick;	/* slic thread only */
	/* The edge source of the interrupt pin (slic_thread.h enum slic_mode).
	 * -1 is POLL for the life of the handle: no pin, or an edge the
	 * kernel refused. The events are POLLPRI on a sysfs value node; the
	 * test override names its own. */
	int slic_irq_fd;
	short slic_irq_events;
	bool slic_irq_host;		/* the descriptor is the host's, refreshed per wait */
	/* enum slic_mode. Written by the slic thread alone; atomic so a test
	 * can read it from its own thread. */
	_Atomic uint_least8_t slic_mode;
	bool slic_irq_edge;		/* slic thread only: an edge woke the last wait */
	/* The burst window is on the engine clock, the hook machine's; the
	 * two IDLE deadlines are on CLOCK_MONOTONIC, the clock the thread
	 * sleeps against. Slic thread only. */
	uint32_t slic_burst_until_ms;
	uint64_t slic_next_sample_us;
	uint64_t slic_next_fault_us;
	uint64_t slic_last_fault_us;
	/* Set by the thread with its mode, read by the engine's tick wake:
	 * false in IDLE, where a quiet tick must not wake it. */
	atomic_bool slic_wants_tick;
	/* Engine thread only: a record went to the slic ring this tick, so
	 * the tick wake fires whatever the mode. */
	bool slic_ring_pushed;
	int rec_err;			/* slic thread only: the command's first error */
	uint32_t rec_token;		/* slic thread only: the error group */
	/* The generation the next stored line test record takes; it never
	 * reaches zero, which is the value that means "no record". */
	uint32_t line_test_gen;		/* slic thread only */
	unsigned attach_channels;
	char part_name[16];		/* captured during attach */

	pthread_mutex_t ctl_lock;
	struct cmd_ring ctl_ring;
	struct cmd_ack ctl_ack;
	/*
	 * Test-only fault injection. The timeout windows of the quiesce
	 * protocol are unreachable from a test otherwise: the ack has to be lost
	 * while the work still runs, which is exactly what the stale-token
	 * discard leaves behind in the field. A test arms the stream op it wants
	 * lost, as enum stream_op plus one, and the tick swallows that one ack;
	 * zero, the value of every shipped run, disarms it. One relaxed load per
	 * command, so the RT rules still hold.
	 */
	_Atomic uint_least32_t ack_drop_op;
	/*
	 * Test-only, like ack_drop_op but for the line canceller release. A test
	 * arms it true to make the tick run the UDSP_CMD_LEC_RELEASE retire and
	 * then swallow that one ack, so the control-side sync times out with the
	 * instance already retired. False, the value of every shipped run,
	 * disarms it.
	 */
	_Atomic bool lec_ack_drop;
	/*
	 * Test-only, the same shape for a recovery's DONE. A test arms it true
	 * to make the tick drop the next DONE record whole, unprocessed and
	 * unacked, the loss a stale token leaves behind. The slic thread's
	 * re-post then carries the result. False, the value of every shipped
	 * run, disarms it.
	 */
	_Atomic bool recover_ack_drop;
	/*
	 * Test-only fault injection. The errno the stream prepare returns from
	 * the step between the codec instances and the SRTP session, which is
	 * the only place a test can reach a failure that follows the line
	 * canceller: the steps that fail there for real live in src/rtp and
	 * src/dsp. Zero, the value of every shipped run, disarms it.
	 */
	_Atomic uint_least32_t prep_fail;
	uint32_t ctl_token;
	uint32_t job_token;
	uint32_t slic_token;

	struct udsp_emitter emitter;
	struct udsp_chan chan[UDSP_LINE_MAX];
	struct udsp_line_test_slot line_test[UDSP_LINE_MAX];

	/* cap.lines * UDSP_STREAM_MAX slots, allocated once at open so the
	 * engine and the net thread never allocate. */
	struct udsp_stream *stream[UDSP_LINE_MAX * UDSP_STREAM_MAX];
	/*
	 * cap.lines as the engine and net threads see it. The tick runs from
	 * engine start, so it is already ticking while udsp_open is still
	 * probing; this is published once, after the stream pool exists, and a
	 * tick that sees a line therefore sees the pool too.
	 */
	_Atomic uint_least8_t lines;
	/*
	 * The delayed loop (udsp.h udsp_line_loop). loop_ring is one ring of
	 * UDSP_LOOP_SLOTS frames per line, allocated at open beside the stream
	 * pool, because the tick writes a slot every period and never
	 * allocates. loop_tick is the tick index the rings are written and read
	 * at, engine tick only, so the delay between two lines is the same in
	 * both directions.
	 *
	 * loop_armed is the record of the lines that carry a loop, which is
	 * what makes udsp_line_test and udsp_stream_config refuse one. The
	 * engine tick is its one writer: it sets the line on the arm
	 * (core/stream.c stream_cmd_run) and clears it wherever it disarms a
	 * loop, including the stop that chan_lines_safe runs. The control
	 * thread only reads it, and it reads the arm at once, because the post
	 * of the command waits for the tick's ack.
	 */
	int16_t *loop_ring[UDSP_LINE_MAX];
	uint64_t loop_tick;
	_Atomic uint_least8_t loop_armed[UDSP_LINE_MAX];
	/*
	 * The runtime diagnostic mask of udsp.h enum udsp_debug_flag and the
	 * ring its jitter buffer bit feeds. The engine loads the mask once per
	 * tick, relaxed, exactly as it loads ack_drop_op once per command, so
	 * the RT rules still hold and a clear mask costs nothing beyond that
	 * load.
	 */
	_Atomic uint_least32_t debug;
	struct jb_trace_ring trace;
	/*
	 * The two rings of the plaintext packet trace (core/pkt_trace.h), NULL
	 * until udsp_pkt_trace_enable allocates them and for the life of a
	 * handle that never arms it. The control thread publishes each pointer
	 * with a release store; the engine tick and the net thread load it
	 * acquire and push only while UDSP_DEBUG_PKT_TRACE stands in the mask
	 * above. udsp_release frees the pair, after both producers stopped.
	 */
	_Atomic(struct pkt_trace_ring *) pkt_tx;
	_Atomic(struct pkt_trace_ring *) pkt_rx;
	/*
	 * The ring of the canceller capture (core/lec_capture.h), on the terms
	 * of the packet trace rings above. udsp_lec_capture_enable allocates it
	 * and publishes the pointer with a release store. The engine tick is
	 * the one producer: it loads the pointer acquire and pushes only while
	 * UDSP_DEBUG_LEC_CAPTURE stands. udsp_release frees it.
	 */
	_Atomic(struct lec_capture_ring *) lec_cap;
	/* The periods that the engine skipped since the open, engine tick only.
	 * The skip hook adds them. The capture writes the difference since the
	 * previous record of a line into the skipped field of the record
	 * (core/chan.c lec_capture_publish). */
	uint64_t lec_skip_total;
	struct udsp_net net;
	bool srtp_ready;		/* udsp_srtp_init succeeded */

	/* Engine tick only: the CLOCK_MONOTONIC stamp of this tick. */
	uint64_t tick_ns;
	/* The applied reference of each line canceller and whether it stands
	 * (chan.h struct chan_lec_line). The tick writes them. The control
	 * thread writes them too, through chan_lec_tracker_drop, but only where
	 * core/stream.c stream_lec_reclaim reads engine_live false. They are
	 * published, because a test reads what the line settled on. A plain
	 * field would be a data race even against a stopped engine. The joiner
	 * is the slic thread, and the reader never met it. */
	_Atomic uint_least32_t lec_delay[UDSP_LINE_MAX];
	atomic_bool lec_ready[UDSP_LINE_MAX];
	/* The loop the echo of a played digit crosses (chan.h loop_last). The
	 * tick publishes it for chan_digit_hold_arm. */
	_Atomic uint_least32_t lec_loop;
	/* The per-line canceller state the tick publishes and
	 * udsp_line_lec_stats copies. lec_valid says a publish succeeded for a
	 * canceller the line runs right now, the "not for this configuration
	 * yet" flag stream.h keeps beside jb_snap: snap.h skips a publish
	 * while a reader holds the slot, and the tick may not spin to replace
	 * it, so the reader is refused instead of being handed a reading that
	 * belongs to a canceller the line no longer has. */
	struct lec_snap lec_snap[UDSP_LINE_MAX];
	_Atomic bool lec_valid[UDSP_LINE_MAX];
	/* The line-monitor record per line, the slic thread's, and whether a
	 * reading arrived since the channel was reset. */
	struct monitor_snap monitor_snap[UDSP_LINE_MAX];
	_Atomic bool monitor_valid[UDSP_LINE_MAX];
	/* The canceller instance the last publish described, engine tick only
	 * (the reset at open runs before the tick walks a line). A line that
	 * carries another instance clears the flag first, so a publish the
	 * reader made the tick skip cannot leave the previous canceller's
	 * record standing for the new one. */
	struct udsp_ec *lec_pub[UDSP_LINE_MAX];
	/* The engine the tick may read its own statistics from; published
	 * before the gate opens and cleared after the thread has finished, so
	 * the tick never needs s->engine_lock: no mutex on the tick. */
	_Atomic(struct engine *) engine_pub;

	struct udsp_region region[2];
	atomic_uint region_idx;
	atomic_bool region_valid;
	struct region_view_snap region_view;

	atomic_bool fatal;
	atomic_bool started;
	atomic_bool engine_live;
	/* True while a deferred engine is open but its tick is still parked (DMA
	 * off, gate held). A tick op posted in this window cannot be acked, so it
	 * is refused, skipped or applied inline rather than left to time out. */
	atomic_bool tick_parked;
	atomic_bool dead;
	_Atomic uint_least64_t op_errors;
	_Atomic uint_least64_t stop_timeouts;
	/* Published by the watchdog and by the engine teardown so
	 * udsp_counters stays a FASTPATH that takes no lock (udsp.h). */
	_Atomic uint_least64_t tdm_stalls;
	/* The catch-up record of a late engine (engine.h), published at the
	 * same two instants and with the same one-second resolution. */
	_Atomic uint_least64_t holds;
	_Atomic uint_least64_t capture_drops;
	_Atomic uint_least64_t trim_drops;
	_Atomic uint_least64_t playback_expired;
	/* The hook sampling record (udsp.h udsp_counters). The slic thread
	 * is the one writer; the getter reads. */
	_Atomic uint_least64_t hook_gaps;
	_Atomic uint_least64_t hook_moved_gaps;
	_Atomic uint_least32_t hook_gap_max_ms;
	/* What the last hold warning reported, so one line covers one
	 * watchdog run and a replacement engine starts the record again.
	 * Emitter thread only. */
	struct udsp_hold_mark hold_warned;
	bool stall_seen;		/* emitter thread only */
	bool watchdog_primed;		/* emitter thread only */
	uint64_t watchdog_ticks;	/* emitter thread only */

	void *sig_stack;		/* the control thread's signal stack */
	stack_t sig_stack_prev;		/* restored at close */
	bool sig_stack_live;
	bool armed;
};

static inline unsigned udsp_lines(const struct udsp *s)
{
	return atomic_load_explicit(&s->lines, memory_order_acquire);
}

static inline void udsp_op_error(struct udsp *s)
{
	atomic_fetch_add_explicit(&s->op_errors, 1u, memory_order_relaxed);
}

#define UDSP_LOG_MAX	512

/* The text sink of udsp.h udsp_config.log. Control thread and slic thread
 * only: the engine tick and the net thread never log. Silent without a sink;
 * a DEBUG line needs UDSP_DEBUG_SLIC_LOG in the debug mask. */
void udsp_log(struct udsp *s, enum udsp_log_level level, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
/* The ERR line for an engine that would not open, from s->engine_fail. */
void udsp_engine_fail_log(struct udsp *s);
/* The fourth open record, built from the cap's rt fields: INFO when every
 * request was granted or none was made, WARN when the scheduler, the lock or
 * the pin refused one. engine_cpu is the requested pin, named when the pin
 * was refused. Exposed for the test that drives it with synthetic caps. */
void udsp_rt_line_format(const struct udsp_cap *cap, int engine_cpu,
			 uint8_t slic_policy,
			 enum udsp_log_level *level, char *buf, size_t len);

static inline const struct udsp_region *udsp_region_active(const struct udsp *s)
{
	if (!atomic_load_explicit(&s->region_valid, memory_order_acquire))
		return NULL;

	return &s->region[atomic_load_explicit(&s->region_idx,
					       memory_order_acquire) & 1u];
}

static inline uint32_t udsp_now_ms(const struct udsp *s)
{
	return udsp_clock_now_ms(&s->clock);
}

/* core/chan.c */
void udsp_engine_hook(void *ctx, int16_t *rx, int16_t *tx, unsigned channels,
		      unsigned frames, uint64_t tick);
/* The engine's skip hook: capture periods the ring overwrote or the catch-up
 * rule dropped, before the tick hook of the same tick. */
void udsp_engine_skip(void *ctx, unsigned periods, uint64_t tick);
/* The engine's render hook: a period of the refill of a restart, a tick with
 * no capture period behind it. */
void udsp_engine_render(void *ctx, int16_t *rx, int16_t *tx,
			unsigned channels, unsigned frames, uint64_t tick);
/* The engine's control hook: the commands and the slic reports, on a read
 * the engine dropped. */
void udsp_engine_control(void *ctx, uint64_t tick);
/* rate is the line rate the channel runs at; the frame count follows as
 * rate/100. udsp_open passes the settled rate, so the channels are reset
 * after the settle and never before it. The all-line form resets the first
 * lines channels, the count the open fixes for cap.lines; a channel past it
 * stays the zero the allocation left, and no path indexes one. */
void udsp_chan_reset(struct udsp *s, unsigned line, unsigned rate);
void udsp_chan_all_reset(struct udsp *s, unsigned lines, unsigned rate);
/* FASTPATH: the reader half of the lec_snap publish, the stream_jb_stats
 * shape. */
enum udsp_status chan_lec_stats(struct udsp *s, unsigned line,
				struct udsp_lec_stats *out);

/* core/region_ctl.c */
int udsp_region_path_build(const char *iso3, char *out, size_t cap);
int udsp_region_open_parse(struct udsp *s, const char *iso3);
int udsp_region_job_run(struct udsp *s, const char *path);
void udsp_region_swap_apply(struct udsp *s);

/* core/safe_state.c */
int udsp_safe_state_arm(struct udsp_slic *slic, const struct udsp_slic_ops *ops,
			struct slic_host *host);
void udsp_safe_state_disarm(void);

/* core/udsp.c. The engine lives from open to stop and is recreated by the next
 * start, so the clock order (TDM clocks before the SLIC leaves reset) holds on
 * every start. Both run on the slic thread inside a SLOW job, or on the
 * control thread at open/close. */
/* engine_open_ops is the injection point for a test backend; this is how a
 * test reaches it through the public open path. NULL restores the name lookup
 * of udsp_config.tdm_backend. */
void udsp_tdm_override(const struct udsp_tdm_ops *ops);
/* The interrupt edge source a test injects in place of the host's sysfs
 * value node: the descriptor the slic thread waits on with events, and reads
 * eight bytes from at offset 0 to acknowledge an edge, so an eventfd serves.
 * fd -1 restores the host path. Read at udsp_open. */
void udsp_slic_irq_override(int fd, short events);
/* The same injection point for the T.38 gateway: set before udsp_open, NULL
 * restores the dlopen of udsp_config.fax_plugin. */
void udsp_fax_override(const struct udsp_fax_ops *ops);
int udsp_engine_create(struct udsp *s);
void udsp_engine_gate(struct udsp *s);
int udsp_engine_start_streaming(struct udsp *s);
int udsp_engine_destroy(struct udsp *s, enum udsp_ev_producer p);
/* Called from the emitter thread. */
void udsp_watchdog_run(void *ctx);
/* The watchdog cadence in milliseconds: UDSP_WATCHDOG_MS, or the value of
 * UDSP_WATCHDOG_MS in the environment when a test shortens it. */
unsigned udsp_watchdog_ms_get(void);
/* The lifecycle guard of every op; slic_class adds the started and job-free
 * conditions of the SYNC-SLIC class (udsp.h section 1). */
enum udsp_status udsp_guard(struct udsp *s, unsigned line, bool slic_class);
/* The guard of a release-only op: the handle conditions of udsp_guard without
 * the dead-line refusal, so a consumer can free what it holds on a line the
 * library declared dead (udsp_stream_stop). */
enum udsp_status udsp_release_guard(struct udsp *s, unsigned line);
/* True while the line carries a call, a ring session or a caller identity
 * burst. A metallic line test must not run into any of them: the plug-in opens
 * the linefeed for the test and rings the line for the ringer measurement.
 * udsp_line_test tests this on the control thread and the slic thread tests it
 * again before it calls the op. */
bool udsp_line_busy(const struct udsp *s, unsigned line);
/* True while a delayed loop stands on the line (udsp.h udsp_line_loop), read
 * beside udsp_line_busy and never inside it (core/udsp.c). The slic thread
 * reads it before a metallic line test, which drives the same wire the loop
 * plays into. */
bool udsp_line_loop_armed(const struct udsp *s, unsigned line);
/* Post one command and wait for its ack with ctl_lock already held, so a
 * multi-step op (the stream reconfiguration of udsp.h section 3) keeps the
 * single ack slot to itself for its whole sequence. */
enum udsp_status udsp_sync_locked(struct udsp *s, struct udsp_cmd *cmd,
				  unsigned timeout_ms);
/* p == UDSP_EV_PROD_MAX latches without an event (no ring of the caller's). */
void udsp_fatal_latch(struct udsp *s, enum udsp_ev_producer p);

#endif
