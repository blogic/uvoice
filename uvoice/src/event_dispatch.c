// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <string.h>

#include "uvoice.h"
#include "calltrace.h"
#include "event_dispatch.h"
#include "line/line.h"

/*
 * Which backend wrote the record, and why the question comes first. Two arms
 * ask it, the fault arm and the line arm, and each asks for its own reason.
 *
 * u.fault.code is an opaque producer word: hal.h defines no enum for it, so a
 * bit means what its backend says it means. libudsp fills a mask whose bits
 * are pinned one per name in hal/udsp/udsp_events.c. Another backend can fill
 * the word with bits of its own, and this arm would then restart the daemon
 * on a fault that never meant that.
 *
 * u.line.state is the opposite case. Every backend fills the same pinned enum,
 * so the scope answers what a record means and not how to read it. libudsp
 * posts one only for a line it declares dead and for a line that comes back
 * (udsp/src/core/slic_thread.c slic_line_dead). Another backend can post one
 * for every line state its device reports, so a DOWN record from it says
 * nothing about a dead port.
 *
 * Both arms are therefore scoped to the one producer, the exit and the line
 * hand-off alike. Under every other backend the record falls to the default
 * trace of event_dispatch().
 *
 * The test is on hal_backend_ops.name, the linked backend's own name, and not
 * on a preprocessor conditional: the name needs no handle, and conditional
 * compilation would stop the stub build from compiling this code at all and
 * let it rot unnoticed. It is not on cap_get()->backend either, which needs a
 * live handle a fault can outlive.
 */
static bool hal_backend_udsp(void)
{
	return !strcmp(hal_backend_ops.name, "udsp");
}

/*
 * The terminal arms, reached only through the gate above.
 *
 * Each bit this function decides on is spelled out as a literal. The meaning
 * is pinned where it lives: hal/udsp/udsp_events.c asserts every member of
 * enum udsp_fault_bit against its value. Bit 0 is the stall and bit 10 is the
 * device reset.
 *
 * A stalled engine means the real time tick stopped advancing, so every line
 * on the board is silent. The daemon leaves for the procd respawn
 * (files/uvoice.init: respawn 3600 5 0) rather than serve a registered line
 * that carries no audio, which is the same answer dsp_start_fail() gives a
 * terminal bring-up. The difference is the latency: this does not wait for the
 * next op to fail.
 *
 * The device reset takes the same exit. The SLIC device holds a fault its own
 * recovery cannot clear, and only a reset pulse on a running clock ends it. A
 * fresh udsp_open pulses that reset, so the daemon leaves and procd starts it
 * again.
 *
 * Every other bit is a SLIC fault on one line, and it is reported here and
 * handed to the line shell, which forces that line out of service. A code of 0
 * is the clear of such a fault (hal.h u.fault.code). It takes the same hand
 * off, and the line shell drops the fault reason.
 *
 * The clear is the one record a pending stop drops. A fault moves a line
 * toward out of service, which a teardown wants anyway, but a clear moves it
 * toward service. The service entry behind a clear reads the hook and can
 * seize a line on a daemon that is on its way out. A stop therefore keeps the
 * record and logs it.
 */
#define HAL_FAULT_ENGINE_STALL	(1u << 0)
#define HAL_FAULT_DEVICE_RESET	(1u << 10)
/* hal/udsp/udsp_events.c pins this backend-owned bit outside the library
 * fault range. hal.h carries no fault enum. */
#define HAL_FAULT_STREAM_DEAD	(1u << 16)

void event_fault_dispatch(const struct hal_event *ev)
{
	if (!ev->u.fault.code) {
		if (dispatch_stop_pending()) {
			ULOG_NOTE("line %u slic fault cleared inside the stop, "
				  "ignored\n", ev->line);
			return;
		}

		ULOG_NOTE("line %u slic fault cleared\n", ev->line);
		line_hal_event(ev);
		return;
	}

	if (ev->u.fault.code & HAL_FAULT_STREAM_DEAD) {
		if (dispatch_stop_pending()) {
			ULOG_WARN("line %u stream dead inside the stop, exit code "
				  "unchanged\n", ev->line);
			return;
		}

		ULOG_ERR("line %u stream dead, its local port is stranded until "
			 "the respawn, exiting for the procd respawn\n", ev->line);
		dispatch_exit_request();
		return;
	}

	if (ev->u.fault.code & HAL_FAULT_DEVICE_RESET) {
		if (dispatch_stop_pending()) {
			ULOG_WARN("slic device reset 0x%08x inside the "
				  "stop, exit code unchanged\n",
				  ev->u.fault.code);
			return;
		}

		ULOG_ERR("slic device reset 0x%08x, the device needs a reset "
			 "pulse and a full init, exiting for the procd "
			 "respawn\n", ev->u.fault.code);
		dispatch_exit_request();
		return;
	}

	if (!(ev->u.fault.code & HAL_FAULT_ENGINE_STALL)) {
		ULOG_ERR("line %u slic fault 0x%08x, forcing it out of service\n",
			 ev->line, ev->u.fault.code);
		line_hal_event(ev);
		return;
	}

	/*
	 * The stop that is already running owns the rest, for the reason
	 * dsp_start_fail() gives: the teardown runs two nested loops of its own
	 * and an uloop_end() from here would cut one of them short. procd reads
	 * the stop it asked for, not this outcome.
	 */
	if (dispatch_stop_pending()) {
		ULOG_WARN("hal engine stall 0x%08x inside the stop, exit code "
			  "unchanged\n", ev->u.fault.code);
		return;
	}

	ULOG_ERR("hal engine stall 0x%08x, no line can carry audio, exiting for "
		 "the procd respawn\n", ev->u.fault.code);
	dispatch_exit_request();
}

/*
 * The other way a port leaves service, and the way it comes back.
 *
 * The backend posts HAL_EVENT_LINE with HAL_LINE_DOWN when it declares a line
 * dead, which a failed SLIC init and a control bus that answers nothing both
 * produce. The line shell forces that line out of service and logs the entry
 * itself, because a DOWN record can repeat and the shell owns the memory that
 * makes it once per entry. The backend posts the same record with an active
 * state when a later init brings the line back, and the line shell puts the
 * line back in service.
 *
 * The stop reads the two directions apart, exactly as the fault arm reads its
 * own two. A DOWN record moves the line toward out of service, which a
 * teardown wants anyway, so a pending stop keeps it. The return moves the line
 * toward service, and the service entry behind it reads the hook. It can seize
 * a line and start a dial tone on a daemon that is on its way out, so a
 * pending stop drops it and logs it. Every state above HAL_LINE_DOWN is a
 * return, so the test is against DOWN alone.
 */
static void event_line_dispatch(const struct hal_event *ev)
{
	if (ev->u.line.state == HAL_LINE_DOWN) {
		line_hal_event(ev);
		return;
	}

	if (dispatch_stop_pending()) {
		ULOG_NOTE("line %u backend line returned inside the stop, "
			  "ignored\n", ev->line);
		return;
	}

	ULOG_NOTE("line %u backend line returned\n", ev->line);
	line_hal_event(ev);
}

/*
 * The jitter buffer report is telemetry, not a line event (struct uvoice_jb).
 * It says how the playout buffer of one stream is running, so it is cached for
 * "ubus call voice status" and goes no further: line_hal_event() would carry it
 * into the call state machine, which has no state this record can move, and the
 * detector mask is untouched because the mask is a request the line shell
 * composes from the channel policy and no policy option asks for these records
 * (line/line.c sends DTMF, PULSE and MODEM alone). A backend reports them on
 * its own schedule and the cache takes whatever arrives.
 *
 * A record naming a line or a stream outside the HAL widths is dropped rather
 * than clamped: the arrays are the widths, and a clamp would file one stream's
 * numbers under another one's name.
 */
static void event_jb_store(const struct hal_event *ev)
{
	struct uvoice_jb *jb;

	if (ev->line >= HAL_LINE_MAX || ev->stream >= HAL_STREAM_MAX) {
		TRACE(TRACE_HAL, "jb update outside the hal widths: line %u "
		      "stream %u", ev->line, ev->stream);
		return;
	}

	jb = &uv.jb[ev->line][ev->stream];
	jb->stamp_ms = ev->stamp_ms;
	jb->total = ev->u.jb.total;
	jb->drop = ev->u.jb.drop;
	jb->size_ms = ev->u.jb.size_ms;
	jb->jitter_avg = ev->u.jb.jitter_avg;
	jb->valid = true;
}

/*
 * The canceller policy the answer-tone disabler asked of the line, in words.
 * The kind is what the disabler did to the filter and the reason is why, and a
 * field engineer reading a fax that failed needs both: a line whose canceller
 * went off whole under a modem latch sounds nothing like one whose nonlinear
 * processor alone went off.
 */
static const char *lec_policy_text(uint8_t kind)
{
	switch (kind) {
	case HAL_PASSTHROUGH_MODEM:
		return "canceller off whole";
	case HAL_PASSTHROUGH_FAX:
		return "canceller on, processor off";
	case HAL_PASSTHROUGH_OFF:
		return "canceller as the channel asks";
	}

	return "canceller policy unknown";
}

/*
 * Tone direction separates the media kind from the profile and offer gates
 * in line/line.c tone_event(). A remote tone arms the kind but starts neither a
 * profile nor an offer. The DSP disabler also reads tones from either feed.
 */
static const char *lec_direction_text(uint8_t direction)
{
	switch (direction) {
	case HAL_TONE_DIR_LOCAL:
		return "from the phone";
	case HAL_TONE_DIR_REMOTE:
		return "from the wire";
	}

	return "from an unnamed feed";
}

static const char *lec_reason_text(uint8_t reason)
{
	switch (reason) {
	case HAL_LEC_REASON_LATCH:
		return "latched";
	case HAL_LEC_REASON_DEMOTE:
		return "demoted the latch";
	case HAL_LEC_REASON_RELEASE_QUIET:
		return "released, holding band quiet";
	case HAL_LEC_REASON_RELEASE_TOPOLOGY:
		return "released, the line's streams moved";
	case HAL_LEC_REASON_RELEASE_CALLER:
		return "released, the call layer read the tone as a false one";
	}

	return "moved for an unknown reason";
}

/*
 * The disabler's whole story lands in one domain with the tone rows it answers,
 * because the two are read together: a policy that moved with no tone in front
 * of it is a different fault from a tone that moved no policy.
 *
 * It drives nothing. The DSP owns its own canceller and no call state describes
 * the switch, which is the same reason a remote fax tone pushes no FSM event
 * (line/line.c tone_event). A release answers no tone, so the row for it names
 * neither the tone nor the feed rather than printing the OTHER and the LOCAL
 * that a zeroed field reads as.
 */
static void event_lec_policy_trace(const struct hal_event *ev)
{
	if (ev->u.lec.reason == HAL_LEC_REASON_LATCH ||
	    ev->u.lec.reason == HAL_LEC_REASON_DEMOTE) {
		TRACE(TRACE_HOOK, "line %u tone disabler: %s %s %s, %s", ev->line,
		      hal_tone_detect_name(ev->u.lec.tone),
		      lec_direction_text(ev->u.lec.direction),
		      lec_reason_text(ev->u.lec.reason),
		      lec_policy_text(ev->u.lec.kind));
		return;
	}

	TRACE(TRACE_HOOK, "line %u tone disabler: %s, %s", ev->line,
	      lec_reason_text(ev->u.lec.reason), lec_policy_text(ev->u.lec.kind));
}

/* The trace line names the kind, because a reader of the call trace timeline
 * has no enum at hand; a kind this table does not know keeps its number. */
static const char *event_word(uint16_t type, char *buf, size_t len)
{
	switch (type) {
	case HAL_EVENT_HOOK:		return "hook";
	case HAL_EVENT_DIGIT:		return "digit";
	case HAL_EVENT_TONE:		return "tone";
	case HAL_EVENT_LINE:		return "line";
	case HAL_EVENT_RING_START:	return "ring_start";
	case HAL_EVENT_RING_END:	return "ring_end";
	case HAL_EVENT_CID_SENT:	return "cid_sent";
	case HAL_EVENT_CID_RECV:	return "cid_recv";
	case HAL_EVENT_CID2_TIMEOUT:	return "cid2_timeout";
	case HAL_EVENT_RTP_FIRST_RX:	return "rtp_first_rx";
	case HAL_EVENT_RTP_FIRST_TX:	return "rtp_first_tx";
	case HAL_EVENT_JB_UPDATE:	return "jb_update";
	case HAL_EVENT_T38:		return "t38";
	case HAL_EVENT_SLIC_FAULT:	return "slic_fault";
	case HAL_EVENT_AUDIO_END:	return "audio_end";
	case HAL_EVENT_JOB_DONE:	return "job_done";
	case HAL_EVENT_LEC_POLICY:	return "lec_policy";
	case HAL_EVENT_INPUT:		return "input";
	}

	snprintf(buf, len, "type=%u", type);

	return buf;
}

/*
 * The record kinds an armed call trace asks for a snapshot on. Each one names
 * a moment an investigator reads the counters against: the first packet in
 * each direction, a detected tone, the image session, a move of the canceller
 * policy and a SLIC fault. Every other kind is either a line event the
 * machine already reports through its own state change, or a completion that
 * names no line at all.
 */
static const char *event_calltrace_word(uint16_t type)
{
	switch (type) {
	case HAL_EVENT_RTP_FIRST_RX:
		return "rtp_first_rx";
	case HAL_EVENT_RTP_FIRST_TX:
		return "rtp_first_tx";
	case HAL_EVENT_TONE:
		return "tone";
	case HAL_EVENT_T38:
		return "t38";
	case HAL_EVENT_LEC_POLICY:
		return "lec_policy";
	case HAL_EVENT_SLIC_FAULT:
		return "slic_fault";
	}

	return NULL;
}

static void event_calltrace_note(const struct hal_event *ev)
{
	const char *word = event_calltrace_word(ev->type);

	if (word)
		calltrace_event(ev->line, word);
}

void event_dispatch(const struct hal_event *ev)
{
	char word[16];

	/* Before the arms below, because two of them end the daemon and one
	 * forces the line out of service: the reason must reach the trace
	 * whatever the record does next. */
	event_calltrace_note(ev);

	switch (ev->type) {
	case HAL_EVENT_HOOK:
		TRACE(TRACE_HOOK, "line %u hook state=%u pulses=%u", ev->line,
		      ev->u.hook.state, ev->u.hook.pulses);
		line_hal_event(ev);
		return;
	case HAL_EVENT_DIGIT:
		/* the stream travels because a remote digit is played on the
		 * leg it names, and only this record carries that */
		TRACE(TRACE_HOOK, "line %u digit '%c' remote=%u dur=%u stream=%u",
		      ev->line, ev->u.digit.digit, ev->u.digit.remote,
		      ev->u.digit.duration_ms, ev->stream);
		line_hal_event(ev);
		return;
	case HAL_EVENT_TONE:
		TRACE(TRACE_HOOK, "line %u tone code=%s dir=%s dur=%u", ev->line,
		      hal_tone_detect_name(ev->u.tone.code),
		      ev->u.tone.direction == HAL_TONE_DIR_REMOTE ?
		      "wire" : "phone",
		      ev->u.tone.duration_ms);
		line_hal_event(ev);
		return;
	case HAL_EVENT_RING_START:
	case HAL_EVENT_RING_END:
		TRACE(TRACE_HOOK, "line %u %s", ev->line,
		      ev->type == HAL_EVENT_RING_START ? "ring_start" : "ring_end");
		line_hal_event(ev);
		return;
	case HAL_EVENT_CID_SENT:
		TRACE(TRACE_HOOK, "line %u cid sent kind=%u status=%d", ev->line,
		      ev->u.cid_sent.kind, ev->u.cid_sent.status);
		return;
	case HAL_EVENT_INPUT:
		line_hal_event(ev);
		return;
	case HAL_EVENT_T38:
		/* the line names the state and the stream; this arm exists so the
		 * record reaches it instead of the default trace, which knows
		 * nothing about the T.38 payload */
		line_hal_event(ev);
		return;
	case HAL_EVENT_RTP_FIRST_RX:
		/* the line holds the ringback gate of the early stream and
		 * traces every record itself; this arm exists so the record
		 * reaches it instead of the default trace */
		line_hal_event(ev);
		return;
	case HAL_EVENT_JB_UPDATE:
		event_jb_store(ev);
		return;
	case HAL_EVENT_LEC_POLICY:
		/* The store first, so the row "voice status" renders and the row
		 * a log carries describe the same move. The trace runs for every
		 * record, including one naming a line the store drops, because
		 * a policy that moved on a line this build has no slot for is
		 * still a fact a reader needs. */
		uvoice_lec_policy_set(ev->line, ev);
		event_lec_policy_trace(ev);
		return;
	case HAL_EVENT_SLIC_FAULT:
		/* A backend whose fault word this daemon cannot decode falls to
		 * the trace below, the default path. */
		if (hal_backend_udsp()) {
			event_fault_dispatch(ev);
			return;
		}
		break;
	case HAL_EVENT_LINE:
		/* A backend whose line record describes an ordinary state
		 * change falls to the same default path. */
		if (hal_backend_udsp()) {
			event_line_dispatch(ev);
			return;
		}
		break;
	default:
		break;
	}

	TRACE(TRACE_HAL, "line %u event %s stream=%u", ev->line,
	      event_word(ev->type, word, sizeof(word)), ev->stream);
}

void event_seq_check(const struct hal_event *ev)
{
	uint16_t expect;
	uint16_t lost;

	/* A hardware record carries a seq from 1 (hal.h section 2), so a 0
	 * here came from outside the pump: it takes no part in the walk and
	 * must not move it. */
	if (!ev->seq)
		return;

	if (!uv.event_seq_valid) {
		uv.event_seq_valid = true;
		uv.event_seq = ev->seq;
		return;
	}

	/* The emitter never assigns 0: 65535 wraps to 1 (hal.h section 2), so
	 * the successor of 65535 is 1 and a plain increment would report one
	 * lost record on every wrap. */
	expect = uv.event_seq + 1;
	if (!expect)
		expect = 1;

	uv.event_seq = ev->seq;

	if (ev->seq == expect)
		return;

	/* A distance that straddles the wrap counts the number 0 as well, and
	 * that number was never used. */
	lost = (uint16_t)(ev->seq - expect);
	if (ev->seq < expect)
		lost--;

	uv.events_lost += lost;
	ULOG_WARN("hal event gap: expected seq %u, got %u, lost %llu total\n",
		  expect, ev->seq, (unsigned long long)uv.events_lost);
	/* Behind the count and the log, so both describe the gap the line shell
	 * is answering. A lost hook report leaves the machine and the handset
	 * disagreeing until something reads the hardware again (line/line.h
	 * lines_hook_resync). */
	lines_hook_resync();
}
