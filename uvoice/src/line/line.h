// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The line shell of the core: the HAL half of every line (src/line/line.c).
 * The signaling protocol runs the machine of the line (proto/proto.h struct
 * proto_ops) and reaches these services through the host table that
 * proto/proto.c builds from them.
 *
 * Duties. HAL event intake and normalization (a pulse hook event becomes a
 * digit, ten pulses are '0') before the event reaches the protocol, the HAL
 * tone, ring and caller id ops behind the host table, ring construction
 * (default 2 s on 4 s cadence, the Alert-Info Bellcore-dr patterns, type 1
 * CID), the hold of a ring a metallic line test refuses and its re-issue when
 * that test lets the part go, the call waiting alert (CALL_WAITING tone plus
 * type 2 CID), the message waiting store and its visual record, the forward
 * disconnect signal, the service state of every line, and the
 * detector-before-dial-tone ordering: channel_config_set with the
 * HAL_DETECT_DTMF and HAL_DETECT_PULSE bits runs on seize before the dial
 * tone op, deferred by LINE_DIAL_TONE_DEFER_MS, 0 for the stub and 500 on
 * hardware, so the first digit is never lost. A dial tone behind a star code
 * confirm tone waits for the region length of that tone instead
 * (line/line.c dial_tone_set()).
 *
 * A line whose channel is disabled or absent is out of service: the protocol
 * holds its machine out, and a seize plays reorder.
 */
#ifndef UVOICE_LINE_H
#define UVOICE_LINE_H

#include <stdbool.h>
#include <stdint.h>

#include "cfg/cfg.h"
#include "hal/hal.h"
#include "proto/proto.h"

/*
 * The bring-up is two halves, because the call layer must not wait for
 * the DSP. lines_init() wires everything that needs no hardware, and main.c
 * runs it at start-up: the timer environment, the record ring, the derived
 * views and machines, the leg layer and a provisional service state.
 * lines_start() carries the half the DSP has to accept, and main.c runs it
 * once the bring-up completed, or when no backend attached at all; a bring-up
 * that failed terminally exits the daemon for the procd respawn instead. It
 * carries the country tone table, the hook windows, the service state settled
 * against the port count of the started DSP, the silence an out of service
 * port owes, the message waiting records the window could not carry, and the
 * boot hook query.
 *
 * A call offered between the two is answered by the admission rule of the
 * protocol (proto/proto.h hal_ready); the SIP legs answer 503 while the DSP is
 * not ready. That answer is the reason for the split: the caller is told the
 * service is unavailable, not that the line is busy.
 *
 * Nothing is spent in the window. Two things the early half brings forward
 * cannot be finished without the DSP: the service state reads a port count
 * the started DSP settles, and a message waiting record cannot reach a
 * channel that is not started. Both are carried, not dropped, and lines_start()
 * settles them.
 */
void lines_init(const struct cfg *cfg);
void lines_start(const struct cfg *cfg);
void lines_reload(const struct cfg *cfg);
void lines_done(void);

/* the event_dispatch.c hand-off for hook, digit and ring events */
void line_hal_event(const struct hal_event *ev);

/*
 * Read the hook of every line again, because the event pump lost records
 * (event_dispatch.c event_seq_check). A lost HAL_HOOK_RELEASE leaves the
 * machine off hook. Nothing then heals it until the next service entry of
 * that line, or until the release verb runs. A line in use rarely gets a
 * service entry. The line answers every caller busy meanwhile.
 *
 * The pass runs one uloop pass later, so the records queued behind the gap
 * reach the machines first. A gap that arrives with no pass in flight starts
 * one at most once per LINE_HOOK_RESYNC_MIN_MS. A gap that lands inside the
 * confirm wait restarts the pass at once. The reading in front of that wait
 * is older than the loss. Under a storm of gaps the pass repeats every
 * confirm interval and heals nothing until the loss stops.
 *
 * A line out of service and a line inside a battery removal window are not
 * read at all. A line the loop test holds is out of service and so not read.
 * A metallic line test postpones the whole pass.
 *
 * An off-hook answer on a machine that is on hook seizes at once. A ringing
 * line is the exception: the answer of the query inside a ring cadence is not
 * verified on hardware, so the pass does not trust it. An on-hook answer on a
 * machine that is off hook releases only after a second reading that agrees.
 * A flash, a pulse and a key the subscriber presses all break the loop open
 * in normal use. A wrong release ends a live call.
 */
void lines_hook_resync(void);

/*
 * Whether this line can take an inbound call (proto/proto.h line_in_service).
 * It is false for every line the shell holds out of service, whatever took it
 * out, and for a port the board does not carry. The protocol refuses a call to
 * such a line before it rings anything, because the machine of an out of
 * service line ignores the incoming call and the caller would otherwise hear
 * ringback on a silent port.
 */
bool line_call_admit(unsigned int line);

/*
 * The three diagnostic injections, which the ubus verbs
 * "voice seize", "voice release" and "voice dial" carry northbound. They take
 * the 0 based line index every other voice method takes. A CLI that counts
 * lines from 1 converts the index itself, and no index arithmetic of any kind
 * happens here.
 *
 * Each one builds the record the DSP would have sent and hands it to
 * line_hal_event(), so an injected event and a hardware one are the same event
 * by the time anything decides what to do with it. The FSM matrix therefore
 * stays the single behavior authority: no state, no cell and no FSM event
 * exists for an injected event, and no state is refused, because the matrix
 * answers SEIZE, RELEASE and DIGIT in all nineteen of them.
 *
 * "Dial" is not uniform across the states, because the seize in front of it is
 * not. On an IDLE line it gives the dial tone and the digits are collected
 * against the dial plan. On a RINGING line that seize answers the inbound
 * call, so the digits reach that caller as DTMF instead. On a HOLDING line it
 * resumes the held leg and the digits follow into whatever the resume lands
 * in. On an OOS line the whole thing drops to reorder. That is the matrix
 * answering each state rather than a policy this file adds, and it is what
 * driving the verbs through the intake buys.
 *
 * A seize whose detector does not arm forces the line out of service, exactly
 * as a hardware seize does (hook_event): a line that cannot collect digits
 * cannot carry a call. The verb puts that trigger in an operator's hands.
 *
 * The hook desync is the point, and it is deliberate. A synthetic SEIZE on a
 * line whose handset is physically on hook leaves the machine off hook while
 * the loop stays open, and the two disagree until the subscriber next moves
 * the handset. That real edge is reported like any other, the matrix consumes
 * it in whatever state the injection left behind, and the line is back in
 * step; the RELEASE verb reaches the same rest state without waiting for a
 * subscriber. A verb that refused to desync could not exercise a line with no
 * telephone on it, which is the case a field engineer has. No hook state
 * tracking is added to prevent it, because tracking it would mean refusing
 * the one thing the verb exists to do.
 */
enum line_inject {
	LINE_INJECT_OK = 0,
	LINE_INJECT_NO_LINE,		/* this board carries no such port */
	LINE_INJECT_NO_LINES,		/* lines_init has not run: no machine yet */
	LINE_INJECT_BAD_NUMBER,		/* empty, over long, or not dialable */
};

enum line_inject line_inject_seize(unsigned int line);
enum line_inject line_inject_release(unsigned int line);

/*
 * The seize and the digits of one dial, in that order. The seize is made only
 * when the line is on hook; see the state by state consequences above. digits
 * must not be NULL: it carries how many digits were injected and is written 0
 * for every refusal, none of which injects anything at all.
 */
enum line_inject line_inject_dial(unsigned int line, const char *number,
				  unsigned int *digits);

/*
 * Whether a tone belongs to the first dial tone class, which is the class that
 * waits for the digit detector before it reaches the line. Both members
 * qualify: the message waiting stutter is a dial tone the subscriber dials over
 * exactly like the plain one in OFFHOOK_DIAL, so a stutter that reached the
 * line inside the seize event would lose the first digit of every line that
 * holds a message.
 *
 * It is exported because the window beside it is a compile time constant that
 * the stub build sets to 0 (line/line.c, LINE_DIAL_TONE_DEFER_MS), which makes
 * the deferral itself unobservable in that build. The class is the half a suite
 * can pin whatever the constant says.
 */
bool line_tone_is_dial(enum hal_tone tone);

/*
 * The two canceller layers of one line the lec row of "voice status" reports.
 *
 * config is the uci view of the line's live channel copy. That copy is the one
 * the push derives from, and it is not the loaded config: a reload stages a
 * channel view and applies it at the next IDLE. uv.cfg can therefore name
 * options the DSP has not been sent. The five members are the uci options
 * without their lec_ prefix, unresolved. control 0 makes the daemon ignore
 * enable and nlp, so a reader that holds both layers can see why applied
 * differs.
 *
 * applied is the record of the last push detector_arm() made, and not a second
 * derivation of it. ec and nlp are the pair cfg_channel_lec_effective()
 * resolved for that push. stamp_ms is the CLOCK_MONOTONIC millisecond it
 * went out and status is the hal_status of the two-try result. pending says
 * a fresh channel_cfg_build() would now send a different canceller view,
 * which is a config change no seize has carried to the DSP yet. valid is
 * false until the first push of the daemon's life.
 *
 * The whole view is written on every call. A line this board does not carry
 * reads the daemon policy in config: control false, enable true, nlp unset and
 * both tone options false. Its applied is invalid.
 */
struct line_lec_view {
	struct {
		bool control, enable, nlp_set, nlp;
		bool tone_latch, tone_force_disable;
	} config;
	struct {
		bool valid;
		bool ec, nlp, tone_latch, tone_force_disable;
		uint32_t stamp_ms;
		enum hal_status status;
		bool pending;
	} applied;
};

void line_lec_view_get(unsigned int line, struct line_lec_view *out);

/*
 * The live per-call media query, which "voice stats_live" carries northbound.
 * It is the deferred-reply half of the call record snapshot: the same slow HAL
 * op, on the same one job slot per (line, stream), against a stream that is
 * still running rather than one about to stop.
 *
 * The caller owns the reply. This entry submits and answers a refusal
 * synchronously; on LINE_STATS_OK it answers nothing here, and cb runs later on
 * the uloop thread with the numbers, or with NULL when the query failed. cb is
 * called at most once per accepted query and must not be NULL. The stats
 * pointer is valid for the callback only.
 *
 * A live query displaces nothing. The record snapshot of a call that is ending
 * may take the slot from a live query, which then gets its NULL answer; a live
 * query that finds the slot taken refuses busy and does not queue. The
 * asymmetry is deliberate: a record that loses its snapshot loses a fact
 * nothing can produce again, while a live query can simply be asked again.
 *
 * LINE_STATS_NO_STREAM covers both an empty slot and a leg whose media is not
 * running. The leg layer exports no per-slot occupancy predicate, and a second
 * copy of its in_use flag kept here would drift from it, so the two are one
 * refusal: neither can answer a media query.
 */
enum line_stats {
	LINE_STATS_OK = 0,
	LINE_STATS_NO_LINES,		/* lines_init has not run: no shell yet */
	LINE_STATS_NO_LINE,		/* this board carries no such port or slot */
	LINE_STATS_NO_STREAM,		/* nothing is running there to measure */
	LINE_STATS_BUSY,		/* one query already holds this pair */
	LINE_STATS_REFUSED,		/* the dsp is not up, or it refused */
};

typedef void (*line_stats_cb)(void *priv, const struct hal_media_stats *stats);

enum line_stats line_stats_live_query(unsigned int line, unsigned int stream,
				      line_stats_cb cb, void *priv);

/*
 * Abandon a query this consumer owns, because it stopped waiting for it. The
 * callback is not made: the caller is the one walking away. Harmless when the
 * pair holds no live query, so a consumer that has already been answered may
 * call it anyway.
 */
void line_stats_live_drop(unsigned int line, unsigned int stream);

/* The ProSLIC plug-in stops its poll loops at 90 s for the run and 5 s for the
 * abort. It reports the stop as a failed completion. A few register transfers
 * follow those loops, so this budget covers the call with a margin. libudsp
 * uses the same value for close. */
#define LINE_TEST_JOB_MS	100000

#define LINE_TEST_RECORD_MAX	512

enum line_test_query_status {
	LINE_TEST_QUERY_OK = 0,
	LINE_TEST_QUERY_BAD_ARGUMENT,
	LINE_TEST_QUERY_NO_LINE,
	LINE_TEST_QUERY_NOT_UP,
	LINE_TEST_QUERY_PENDING,
	LINE_TEST_QUERY_CALL_ACTIVE,
	LINE_TEST_QUERY_BUSY,
	LINE_TEST_QUERY_UNSUPPORTED,
	LINE_TEST_QUERY_REFUSED,
};

enum line_test_result_status {
	LINE_TEST_RESULT_COMPLETE = 0,
	LINE_TEST_RESULT_FAILED,
	LINE_TEST_RESULT_UNSUPPORTED,
	LINE_TEST_RESULT_FATAL,
	LINE_TEST_RESULT_TIMEOUT,
	LINE_TEST_RESULT_UNREADABLE,
};

/*
 * The callback runs once on the uloop thread for each accepted query.
 * record is valid only during the callback. len is the backend's full length.
 */
typedef void (*line_test_cb)(void *priv, enum line_test_result_status status,
			     const void *record, size_t len);

/*
 * Submit one metallic line test. A test blocks hook samples on every line,
 * so the caller refuses it while any line carries a call.
 */
enum line_test_query_status line_test_query(unsigned int line,
					    enum hal_line_test test,
					    line_test_cb cb, void *priv);

/* Abandon the accepted test on this line without calling its consumer. */
void line_test_drop(unsigned int line);

/*
 * The manufacturing loop test. It rings every port the board carries while
 * the handset is down, and it plays a lifted handset its own voice a fixed
 * delay later, or the capture of the other port on a two-port board. The
 * audio path is the backend's own (hal.h line_loop_set), so no packet and no
 * stream is involved.
 *
 * The mode holds every port out of service for as long as it is armed, and the
 * line shell consumes the hook edges of a held port itself, so no call is
 * offered, no digit is collected and no reorder is played at a subscriber who
 * lifted a handset for the test.
 *
 * ring_s is the ring window in seconds, 1 to LINE_LOOP_TEST_RING_S_MAX, and
 * the backend ends each bell on it. delay_ms is the playout delay of the self
 * loop, 0 to LINE_LOOP_TEST_DELAY_MAX_MS, where 0 asks for the shortest delay
 * the backend carries. The cross connection reads no delay.
 */
#define LINE_LOOP_TEST_RING_S_MAX	60u
#define LINE_LOOP_TEST_RING_S_DEFAULT	60u
#define LINE_LOOP_TEST_DELAY_MAX_MS	1000u
#define LINE_LOOP_TEST_DELAY_DEFAULT_MS	500u

/*
 * OK also answers a call that asks for the state the board already holds.
 * BAD_ARGUMENT is a window or a delay outside the bounds above. NO_LINE is a
 * daemon with no lines and no board port. NOT_READY is a DSP that is not
 * ready. UNSUPPORTED is a backend with no loop of its own. CALL_ACTIVE and
 * TEST_RUNNING are a call and a metallic line test, each of which owns the
 * ports the mode would take. REFUSED is a disarm the
 * backend answered transiently: the loop may still stand on a port, so the
 * mode holds every port and the operator asks again, and the repeat is let
 * through because the board is still armed. The arm makes no refusal of its
 * own: it reports a backend refusal in the log and holds the mode, because
 * the next hook edge issues the op again.
 */
enum line_loop_test_status {
	LINE_LOOP_TEST_OK = 0,
	LINE_LOOP_TEST_BAD_ARGUMENT,
	LINE_LOOP_TEST_NO_LINE,
	LINE_LOOP_TEST_NOT_READY,
	LINE_LOOP_TEST_UNSUPPORTED,
	LINE_LOOP_TEST_CALL_ACTIVE,
	LINE_LOOP_TEST_TEST_RUNNING,
	LINE_LOOP_TEST_REFUSED,
};

/* What the mode plays. SELF is the self loop, which is also what a one-port
 * board runs; CROSS is the connection between the two ports. */
enum line_loop_test_mode {
	LINE_LOOP_TEST_MODE_OFF = 0,
	LINE_LOOP_TEST_MODE_SELF,
	LINE_LOOP_TEST_MODE_CROSS,
};

/* The standing mode, written on every call, refusals included: lines is the
 * number of ports the mode holds and the other three are what it runs with.
 * A disarmed board reads enabled false, mode OFF and three zeros. */
struct line_loop_test_view {
	bool enabled;
	uint8_t mode;			/* enum line_loop_test_mode */
	unsigned int lines;
	unsigned int ring_s;
	unsigned int delay_ms;
};

/*
 * Arm or disarm the mode. want_self asks for the self loop on a board that
 * could cross two ports; a one-port board runs the self loop whatever it says.
 * out may be NULL, and it carries the standing view whatever the answer is.
 *
 * A call that asks for the state the board already holds changes nothing and
 * answers OK. A disarm that answers REFUSED changed nothing either: the mode
 * still stands and the same call repeats it.
 */
enum line_loop_test_status line_loop_test_set(bool enable, bool want_self,
					      unsigned int ring_s,
					      unsigned int delay_ms,
					      struct line_loop_test_view *out);

/* Disarm the mode on the daemon stop, while the backend is still attached. */
void lines_loop_test_stop(void);

struct blob_buf;

/* the call[] array of voice status: the core keys of each line, then the
 * protocol's own (ops->status_dump_call), for voice status and the e2e
 * asserts */
void lines_status_dump(struct blob_buf *b);

/* the region the daemon holds, the read-back or the compiled default, for
 * voice status; correct before lines_init, so a disabled daemon answers too */
void lines_region_status_dump(struct blob_buf *b);

/*
 * The state of one line's machine by the name the protocol gives it
 * (proto/proto.h ops->state_name), for the call trace snapshot. NULL before the
 * lines are up, for a port the board does not carry and for a protocol that
 * names no state.
 */
const char *line_state_name(unsigned int line);

/*
 * Where one line rests, from the protocol's line flags and the core's own oos
 * bit (proto/proto.h enum proto_line_flag): out of service, idle, or anything
 * else. The call trace reads it on every move of the line. An index the board
 * does not carry, and every line before the lines are up, reads out of service.
 */
enum line_rest {
	LINE_REST_ACTIVE = 0,
	LINE_REST_IDLE,
	LINE_REST_OOS,
};

enum line_rest line_rest_of(unsigned int line);

/* ------------------------------------------------- the services of the core
 *
 * The core functions behind struct proto_host (proto/proto.h), which
 * proto/proto.c fills from them. Each one keeps the contract of its host entry;
 * line is the 0 based line index and stream the HAL stream index. A line the
 * shell does not hold is refused or ignored.
 */
enum hal_status line_tone_play(unsigned int line, enum hal_tone tone,
			       uint32_t duration_ms);
void line_tone_stop(unsigned int line);
void line_ringer_start(unsigned int line, enum proto_ring_kind kind,
		     unsigned int ring_template, uint32_t duration_ms,
		     const struct proto_cid *cid);
void line_ringer_stop(unsigned int line);
void line_cw_alert(unsigned int line, bool on, unsigned int variant,
		   const struct proto_cid *cid);
enum hal_status line_cid_send(unsigned int line, enum hal_cid_kind kind,
			      const struct proto_cid *cid);
unsigned int line_ring_template_match(unsigned int line, const char *number);
void line_mwi_set(unsigned int line, bool on, uint32_t new_msgs,
		  uint32_t old_msgs);
bool line_mwi_get(unsigned int line, uint32_t *new_msgs, uint32_t *old_msgs);
bool line_detector_arm(unsigned int line, uint32_t detect_mask);
bool line_fwd_disc_signal(unsigned int line);
void line_fwd_disc_abort(unsigned int line);
enum hal_status line_battery_set(unsigned int line, enum proto_battery batt);
bool line_digit_play(unsigned int line, unsigned int stream, char digit,
		     uint16_t duration_ms, bool dedup);
void line_digit_play_cancel(unsigned int line, unsigned int stream);
enum hal_status line_stream_stats_get(unsigned int line, unsigned int stream,
				      struct hal_media_stats *out);
bool line_exists(unsigned int line);
bool line_hal_ready(void);

/*
 * A hold of the protocol's own, the sixth reason a line leaves service
 * (proto/proto.h line_oos_force). The service sync that applies it runs one
 * uloop pass later, because the sync calls back into the protocol.
 */
void line_service_hold(unsigned int line, const char *reason);
void line_service_release(unsigned int line);

void line_transition(unsigned int line, unsigned int state_id,
		     uint32_t flags);
void line_settle(unsigned int line);
void line_cfg_landed(unsigned int line);
enum hal_status line_conf_set(unsigned int line, bool on);
enum hal_status line_audio_path_set(enum hal_audio_path path);
enum hal_status line_volume_set(unsigned int level);
enum hal_status line_mute_set(bool mute);
enum hal_status line_led_set(enum hal_led led, enum hal_led_mode mode);

/**
 * line_audio_tune_set() - apply the audio tuning of the phone
 * @tune: the gains and the ring sound
 *
 * Keeps @tune as the live tuning and gives it to the backend. A reload puts
 * back the tuning of the config.
 *
 * Return: the status of the backend, or HAL_UNSUPPORTED if it has no tuning.
 */
enum hal_status line_audio_tune_set(const struct hal_audio_tune *tune);

/**
 * line_audio_tune_get() - read the live audio tuning
 * @tune: filled with the tuning that line_audio_tune_set() kept last
 */
void line_audio_tune_get(struct hal_audio_tune *tune);
enum hal_status line_key_tone_play(unsigned int line, char digit,
				   uint32_t duration_ms);
void line_fault(unsigned int line, bool loud, const char *text);

/* The stream ops over the backend, with no priv (proto_host_build sets it). */
const struct call_media_ops *line_media_ops(void);

/* The probed capability, or a zero record with no backend; never NULL. */
const struct hal_cap *lines_hal_cap(void);

/* The region the backend read back, or the compiled default. */
const struct hal_region *lines_region(void);

#endif
