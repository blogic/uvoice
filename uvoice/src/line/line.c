// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The line shell of the core: the HAL half of every line (line/line.h
 * "Duties"). The signaling protocol runs the machine of the line and reaches
 * this file through the host table of proto/proto.c; this file reaches the
 * protocol through proto_ops_get() alone.
 */
#include <stdio.h>
#include <string.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubox/utils.h>
#include <libubus.h>

#include "uvoice.h"
#include "cdr.h"
#include "dialplan.h"
#include "line/line.h"
#include "net/ports.h"
#include "str.h"

/*
 * The dial tone waits this long behind the start of digit collection, so the
 * detector is live before the subscriber hears the tone and the first digit
 * is not lost in OFFHOOK_DIAL. The stub has no detector to wait for, so the
 * stub suites see the tone in the same event.
 */
#ifdef UVOICE_HAL_STUB
#define LINE_DIAL_TONE_DEFER_MS	0
#else
#define LINE_DIAL_TONE_DEFER_MS	500
#endif

/* GR-30 type 1 CID sits in the silence after the first ring cycle, where the
 * FSK families put it; cid_after_ring() decides which do, and ring_first_bursts()
 * sets the break index to the first cycle burst count so the record lands after
 * the whole cycle, not after burst one of a multi burst distinctive cadence. */
#define LINE_CID_WAIT_MS	500

/*
 * The subscriber alerting signal of a waiting call, in the GR-30 order: the
 * host plays the SAS itself for 300 ms, then waits 50 ms, the upper bound of
 * the GR-30 transition time X1, and only then pushes the type 2 record. A
 * record pushed inside the alert event puts the CAS and the FSK on top of the
 * beep, and the subscriber hears a modem chirp with no alert in front of it.
 */
#define LINE_CW_SAS_MS		300
#define LINE_CW_GAP_MS		50
#define LINE_CW_CID_MS		(LINE_CW_SAS_MS + LINE_CW_GAP_MS)

/*
 * Forward disconnect. The protocol decides when a line signals it
 * (proto/proto.h fwd_disc_signal), and this file carries the signal: a
 * reversal or a battery removal. The length of the signal is the channel
 * option; the clamp exists because uloop_timeout_set takes an int and no
 * reversal runs for a day.
 */
#define LINE_FWD_DISC_MAX_MS	86400000u

/*
 * How long the hook intake stays quiet after the battery comes back, for the
 * removal form alone. It is measured from the restore and not from the
 * removal, because both windows it has to outlast start there: the DSP
 * classifier reports a seize about 300 ms after the line is energised, which
 * is its own debounce, and it reports a release only after hook_release_min,
 * which is 1301 ms in the shipped US table.
 */
#define LINE_FWD_DISC_SETTLE_MS	2000

/*
 * How long a refused restore waits before it asks the SLIC again. A line whose
 * battery this shell took off and could not put back can never carry a call
 * again, so the attempt repeats until it lands or the line goes away; one
 * second is short enough that a subscriber lifting the handset finds the loop
 * alive, and long enough that a backend in trouble is not hammered.
 */
#define LINE_FWD_DISC_RETRY_MS	1000

/*
 * How long a ring the backend still refuses waits before it asks again. This
 * cadence belongs to a line test alone, so it is far slower than HAL_RETRY_MS.
 * The caller hears ringback while it runs. A measurement that outlasts the
 * core's job budget must not cost that caller the bell, so the attempt repeats
 * until it lands. The ring stop of the protocol ends it (proto/proto.h
 * ring_stop), so the call itself bounds the loop.
 */
#define LINE_RING_HOLD_RETRY_MS	500

/*
 * The hook resync behind an event gap (line/line.h lines_hook_resync).
 *
 * The first value rate limits the entry alone. A gap that arrives with no
 * pass in flight starts one at most once per LINE_HOOK_RESYNC_MIN_MS. A burst
 * of gaps must not queue one query per gap on a pump that already loses
 * records.
 *
 * It does not bound the pass. A gap that lands inside the confirm wait
 * restarts the pass at once. The reading in front of that wait is older than
 * the loss. Under a storm of gaps the pass repeats every confirm interval and
 * heals nothing until the loss stops. The same value postpones a pass a
 * metallic line test holds.
 *
 * The second value is how long the release arm waits for its second reading.
 * The third is the pad the floor of that wait adds to the flash window of the
 * region. A flash holds the loop open for hook_flash_max_ms. Two readings
 * inside one flash both answer on hook for a subscriber who never went on
 * hook. The shipped regions carry 1300 ms or less, so 1500 ms covers them.
 * The floor keeps the pair apart under a region that raises the window.
 */
#define LINE_HOOK_RESYNC_MIN_MS		5000
#define LINE_HOOK_RESYNC_CONFIRM_MS	1500
#define LINE_HOOK_RESYNC_FLASH_PAD_MS	200

/*
 * The digit the far end sent out of band, played toward the phone. The body
 * or the wire event announces a length and this pair bounds it: the floor is
 * the digit length libudsp plays by itself (UDSP_DIGIT_DEFAULT_MS) and it
 * clears the 50 ms GR-506 accepts, and the ceiling keeps the tone under the
 * eight seconds an RFC 4733 duration can carry.
 *
 * The gap is the silence between two digits, the value the caller identity
 * generator of libudsp uses (LINE_CID_DTMF_OFF_MS), so a far end that dials
 * faster than the line plays still leaves the handset two separate tones.
 *
 * The tail is how long a digit from the phone stays suppressed behind a
 * playout. The regenerated tone reaches the uplink detector through the echo
 * path, and call_dtmf() would send the same digit back to the far end that
 * sent it.
 *
 * The queue depth holds the digits of one call. A far end that overruns it
 * sends digits faster than the line can ever play, so the newest digit goes.
 *
 * None of the five is compiled to 0 in the stub build, which is what lets the
 * timed cases prove the values (the LINE_DIAL_TONE_DEFER_MS precedent above
 * is not followed here).
 */
#define LINE_DIGIT_PLAY_MIN_MS		100
#define LINE_DIGIT_PLAY_MAX_MS		500
#define LINE_DIGIT_PLAY_GAP_MS		70
#define LINE_DIGIT_PLAY_TAIL_MS		100
#define LINE_DIGIT_Q_MAX		16

/*
 * How long a digit from the network shadows the same digit arriving on the
 * other transport. A peer that sends one key as an INFO and as an RFC 4733
 * event would otherwise play it twice. A 50/50 ms dialler ends two identical
 * keys 100 ms apart, so a real repeat of one key still passes.
 */
#define LINE_DIGIT_DEDUP_MS		80

/* the region every backend can load: the udsp backend ships USA.conf */
#define LINE_REGION_DEFAULT	"USA"

/* The backend region buffer, the width of hal.h HAL_REGION_ISO3_MAX, of
 * libudsp UDSP_ISO3_MAX and of the stub. A name of seven characters plus the
 * terminator fits. */
#define LINE_REGION_MAX		8

/*
 * One digit from the far end waiting for the line. The stream of the leg it
 * arrived on travels with it, and the protocol answers at its turn whether
 * that stream still faces the phone (ops->digit_play_allowed): a swap can move
 * the leg to hold between the arrival and the playout, and a transfer can
 * replace the leg without a state change, and the replacement must not play
 * the digits of the leg it took over from.
 */
struct line_digit_q {
	char digit;
	uint8_t stream;
	uint16_t ms;
};

/*
 * The ring the line owes now, kept so that a held or a retried ring is built
 * again from the request (ring_issue). kind is enum proto_ring_kind; a
 * PROTO_RING_RECALL ring carries the plain cadence and no caller id.
 */
struct line_ring_req {
	struct proto_cid cid;
	uint32_t duration_ms;
	uint8_t kind;
	uint8_t ring_template;
};

/*
 * What the switch said about the subscriber's mailbox (message-waiting,
 * sip/sub.h).
 *
 * It survives IDLE and a reload, and that is the deliberate difference from
 * the call records of the protocol and from every other runtime state around
 * it. Those stores describe a call: IDLE is where a call ends and a reload
 * re-reads a config file, so both drop them. A waiting message is owned by the
 * switch, belongs to no call and is not written in any config file, so neither
 * edge may touch it: only a later NOTIFY moves it.
 *
 * on, new_msgs and old_msgs are independent: "yes" with 0/0 is a real answer
 * and means the counts carry no detail.
 *
 * deferred holds a presentation the line could not make when the state moved:
 * the lines were not wired, vmwi was off, the DSP was not ready, the line was
 * out of service, the handset was off hook or the bell was ringing.
 */
struct line_mwi {
	bool on;
	uint32_t new_msgs;
	uint32_t old_msgs;
	bool deferred;
};

struct line {
	/* the channel copy the hardware view is derived from */
	struct cfg_channel chan;
	struct cfg_channel chan_new;	/* staged by a reload, applied at rest */
	/* the region cadence table, staged like the CID format beside it so a
	 * reload cannot move a cadence under a ring already playing */
	struct hal_region_ring ring_pattern[HAL_REGION_RING_MAX];
	struct hal_region_ring ring_pattern_new[HAL_REGION_RING_MAX];
	struct line_mwi mwi;
	/*
	 * The last channel view detector_arm() pushed, with the instant and the
	 * answer it got. The lec row of "voice status" reports what the DSP
	 * holds, and only a record can answer that. The view is derived from
	 * the live channel copy, so a re-derivation would describe what the
	 * next seize will send. It would never describe the last push.
	 */
	struct hal_channel_cfg chan_applied;
	uint32_t chan_applied_ms;
	enum hal_status chan_applied_st;
	bool chan_applied_valid;
	/* the last hook window the backend accepted, the chan_applied idiom:
	 * the push guards on it, so a failed push stays owed and a region
	 * read-back that moved only the base still pushes */
	struct hal_hook_timing hook_applied;
	bool hook_applied_valid;
	/* the record the deferred type 2 push carries, taken at the alert */
	struct hal_cid cw_cid;
	struct uloop_timeout cw_cid_timer;
	struct uloop_timeout dial_tone;
	/* the length of the reversal itself */
	struct uloop_timeout fwd_disc_restore;
	/* the removal form's quiet window behind the restore, which is a
	 * different thing from the rest state edge of the protocol: this one
	 * is LINE_FWD_DISC_SETTLE_MS of suppression */
	struct uloop_timeout fwd_disc_settle;
	/* the re-attempt of a restore the backend refused */
	struct uloop_timeout fwd_disc_retry;
	/* the repeat of a ring the backend refused: once behind HAL_RETRY_MS
	 * with no line test running, and every LINE_RING_HOLD_RETRY_MS for as
	 * long as a re-issue keeps answering transient */
	struct uloop_timeout ring_retry;
	/* the timer that paces the digits the far end sent out of band */
	struct uloop_timeout digit_play;
	struct line_digit_q digit_q[LINE_DIGIT_Q_MAX];
	unsigned int index;
	uint8_t tone_pending;		/* enum hal_tone, 0 = nothing to replay */
	uint32_t tone_pending_ms;	/* its length, 0 = until stopped */
	uint8_t tone_pending_state;	/* the state that asked for it */
	uint8_t cid_format;		/* enum hal_cid_format, the resolved region one */
	uint8_t cid_format_new;		/* staged by a reload, applied at IDLE */
	uint8_t mwi_format;		/* the region-load resolved region mwi_format */
	uint8_t mwi_format_new;		/* staged beside cid_format_new */
	uint8_t dial_tone_id;		/* the tone the defer below owes the line */
	struct line_ring_req ring_req;	/* the request of a held or retried ring */
	uint8_t digit_q_head;
	uint8_t digit_q_count;
	bool digit_playing;		/* a digit is on the line right now */
	/*
	 * The suppression window behind a playout, and the flag that says one
	 * stands. digit_play_until_ms is an instant of uvoice_mono_ms(), read
	 * against now through uvoice_deadline_ahead(), so the window survives
	 * the wrap of that clock; the timer clears the flag once the tail
	 * passed, so a deadline never grows old enough to read as ahead again.
	 */
	uint32_t digit_play_until_ms;
	bool digit_play_window;
	/* the last digit the network sent and when, the de-duplication record
	 * of a peer that sends one key on both transports */
	uint32_t digit_net_last_ms;
	char digit_net_last;
	bool digit_net_last_valid;
	bool tone_pending_stamped;
	/* the confirm tone a star code started, the instant it started and its
	 * region length at that instant: a dial tone behind it waits for its
	 * end (dial_tone_set()), and a later region load cannot move the end */
	uint32_t confirm_start_ms;
	uint32_t confirm_total_ms;
	bool confirm_on;
	bool confirm_endless;
	/* the core half of a staged view: chan_new, the two staged formats and
	 * ring_pattern_new wait for line_cfg_landed() */
	bool chan_staged;
	/* a view the final capability landed (lines_start()): its settle runs
	 * behind the presentation of this line */
	bool start_settle;
	/*
	 * The protocol's own hold of the line (line_service_hold), the sixth
	 * reason beside config, fault, down, loop test and no running
	 * protocol, and the one uloop pass that applies a move of it.
	 */
	bool proto_hold;
	struct uloop_timeout service_sync;
	/*
	 * The hook resync behind an event gap (line/line.h lines_hook_resync).
	 * hook_activity counts the hook reports this line received and the keys
	 * the subscriber pressed on it. The release arm compares it across its
	 * two readings. A flash, a pulse and a dialled key all break the loop
	 * open in normal use. A reading taken over one of them describes
	 * nothing. hook_resync_wait says this line owes the second reading, and
	 * hook_resync_activity is the count the first one saw.
	 */
	uint32_t hook_activity;
	uint32_t hook_resync_activity;
	bool hook_resync_wait;
	bool oos;
	/* The two hardware reasons a line is out of service, each on its own
	 * bit because they clear from different records and must not cross. The
	 * config cannot describe a broken part, so a reload cannot revoke
	 * either. fault is the SLIC fault, ended by a fault code of 0 (hal.h
	 * u.fault.code). down is the dead line the backend declared, ended by a
	 * line record above HAL_LINE_DOWN (hal.h u.line.state). */
	bool fault;
	bool down;
	bool ringing;
	bool ring_held;			/* a line test refused this ring */
	/*
	 * The backend refused this ring for good and the caller is owed a 480.
	 * It is applied one uloop pass later. The bell op runs in front of the
	 * RINGING entry, so the machine is not alerting inside that frame.
	 * ring_fail_apply() clears it before it reads anything else, so a
	 * refusal that applies to nothing cannot take the next retry of the
	 * shared ring_retry timer.
	 */
	bool ring_failed;
	bool cw_cid_warned;		/* the type 2 suppression trace, once per call */
	bool fwd_disc_reversed;		/* this line asked the SLIC for REVERSE */
	/*
	 * Two facts, and they come apart. fwd_disc_down is the suppression: the
	 * whole window in which the hook intake drops every report. fwd_disc_dead
	 * is the loop: this shell commanded the battery off and owes an ACTIVE
	 * that has not landed yet. They move together in the ordinary window and
	 * separate when the backend refuses a restore, where the loop is still
	 * owed but nothing can be reported on it to suppress.
	 */
	bool fwd_disc_down;
	bool fwd_disc_dead;
	/*
	 * The battery this port wants, from the config fact of
	 * line_in_service() or the loop test, and the last state the backend
	 * accepted. known is false until a write lands, so the first derive
	 * after a start always writes. The forward disconnect window moves the
	 * battery inside a wanted ACTIVE and is the one other writer; every
	 * write goes through fwd_disc_line_state(), which keeps the mirror.
	 */
	bool battery_wanted;
	bool battery_known;
	bool battery_on;
	/* a config write the backend refused, owed through fwd_disc_retry; the
	 * rest-state abort leaves that timer alone while this stands */
	bool battery_retry;
	bool fwd_disc_avail;		/* the backend carries HAL_FEAT_POLARITY */
	/*
	 * The manufacturing loop test on this port. loop_test is the mode
	 * itself, which holds the line out of service; the three below are what
	 * the controller knows about the port while it holds it: the hook state
	 * it reconciles the audio from, whether the bell is running, and
	 * whether the loop is armed in the backend.
	 */
	bool loop_test;
	bool loop_test_offhook;
	bool loop_test_ringing;
	bool loop_test_audio;
};

static struct line lines[CFG_CHANNEL_MAX];
static bool lines_up;
static struct hal_audio_tune line_tune;
/* lines_start() hands the protocol the final capability */
static bool lines_cap_final_push;

/*
 * The manufacturing loop test. It is one mode for the whole board: armed
 * says every port the board carries is held by it, want_self says the
 * operator asked for the self loop rather than the cross connection, and the
 * two values below are the ring window and the playout delay the arm
 * carried. The per-line half sits in struct line.
 *
 * The state is file scope because the mode belongs to the board and not to a
 * line: a metallic test is refused for the whole board while it stands, and
 * the ubus reply reports one mode.
 */
static bool loop_test_armed;
static bool loop_test_want_self;
static unsigned int loop_test_delay_ms;
static unsigned int loop_test_ring_ms;

/*
 * Whether the backend renders a digit it receives from the wire toward the
 * phone by itself (hal.h HAL_FEAT_DTMF_PLAYOUT). It is one value for the
 * daemon, like the capability it comes from, and it decides one thing: a wire
 * digit such a backend reports is a report alone, and this shell plays
 * nothing for it.
 */
static bool hal_plays_wire_digit;

/*
 * The country tone table. Every local tone the FSM asks for is a row of the
 * DSP tone profile, and that profile is unconfigured until region_apply loads
 * the region. Without the load the handset is silent: no dial tone, no
 * ringback, no busy, while digit detection and the call itself work. The stub
 * backend records tones instead of playing them, so the stub suites cannot
 * show a missing load.
 */
static struct hal_job region_job;
static char region_want[LINE_REGION_MAX];	/* the directory the config asks for */
static char region_inflight[LINE_REGION_MAX];	/* the one the running job carries */
static char region_applied[LINE_REGION_MAX];	/* the one the DSP acknowledged */
static bool region_busy;

/*
 * The region the backend read back after a region_apply, the base every line
 * derives its transports, windows, templates and flash service from. Until
 * the first read-back lands, and for the life of a backend with no
 * region_get, the base is this compiled default: the Bellcore transport, the
 * GR-506 flash window and the drop flash service the US seed carried, empty
 * templates (the built-in GR-506 table answers for them), no alert bound
 * (the fsm keeps its constant offer) and no confirm length (the dial tone
 * behind a confirm tone keeps LINE_DIAL_TONE_DEFER_MS). An empty iso3 means
 * this default and nothing else: udsp_region_get refuses on an empty iso3, so
 * no read-back carries one. Two objects from one initializer, because a
 * static cannot initialize another and the status dump reads region_hal
 * before lines_init on a daemon whose voice is disabled.
 */
#define REGION_HAL_DEFAULT { \
	.cid_format = HAL_CID_BELLCORE_FSK, \
	.mwi_format = HAL_CID_BELLCORE_FSK, \
	.hook_flash_min_ms = 300, \
	.hook_flash_max_ms = 1300, \
	.hook_release_min_ms = 1301, \
	.flash_service = HAL_FLASH_SERVICE_DROP, \
}
static const struct hal_region region_hal_default = REGION_HAL_DEFAULT;
static struct hal_region region_hal = REGION_HAL_DEFAULT;
static bool region_hal_read;		/* a backend answer, not the default */

static void region_settle(struct line *l);
/* the settle of the removal window re-reads the hook through it, in both
 * directions */
static void hook_state_sync(struct line *l);
/* the end of a hardware reason derives the service state of one line again;
 * the derive sits with the config section far below the arm */
static void line_service_sync(struct line *l, const struct cfg *cfg);
/* the forward disconnect primitives report a refusal, and they sit in front
 * of the config section that defines the report */
static void line_fault_report(const struct line *l, bool loud,
			      const char *text);

static struct line *line_get(unsigned int index)
{
	if (index >= CFG_CHANNEL_MAX)
		return NULL;

	return &lines[index];
}

static void cw_cid_expire(struct uloop_timeout *t)
{
	struct line *l = container_of(t, struct line, cw_cid_timer);

	if (!uv.hal)
		return;

	TRACE(TRACE_FSM, "line %u call waiting record pushed behind the alert tone",
	      l->index);
	hal_backend_ops.line_cid_send(uv.hal, l->index, HAL_CID_TYPE2, &l->cw_cid);
}

/*
 * A push that outlives its alert announces a caller the subscriber is no
 * longer being alerted about, so every path that ends an alert ends the push
 * with it: the alert off, every rest of the line and a landing reload, which
 * is the discipline the protocol keeps for its own network call waiting
 * record.
 */
static void cw_cid_drop(struct line *l)
{
	uloop_timeout_cancel(&l->cw_cid_timer);
}

/*
 * The sequence needs a tone to sit behind. Before the first country table
 * lands there is no SAS: tone_pending_take() holds the request for the replay
 * of region_done() (see Replay below), so a record pushed on this timer would
 * reach the phone first and reverse the order the alert sequence sets. The
 * record is dropped with a trace instead of being held for the replay: a load
 * that never succeeds would hold it forever, which is the same lost record
 * with a second piece of alert state to cancel, and the window where the
 * first load lands during an outstanding offer needs a call answered inside
 * the bring-up. The alert tone still replays; only the record of that one
 * offer is lost.
 */
/*
 * The type 2 CAS must land after the first call-waiting presentation, never in
 * the middle of it, so the record is deferred by the selected variant's first
 * burst plus the gap. region_hal carries a lead-in per variant (index 0 is the
 * plain tone). An unknown variant, or a backend with no region read-back, takes
 * the fixed LINE_CW_CID_MS.
 */
static uint32_t cw_cid_lead_ms(unsigned int variant)
{
	/* A region swap after the variant was resolved can shrink cw_count. A
	 * stale variant above it then plays the plain tone (chan_cw_tone_start),
	 * so its lead is the plain lead, not the 350 ms fallback. */
	if (variant > region_hal.cw_count)
		variant = 0;
	if (variant < HAL_REGION_CW_MAX && region_hal.cw_lead_in_ms[variant])
		return (uint32_t)region_hal.cw_lead_in_ms[variant] + LINE_CW_GAP_MS;
	return LINE_CW_CID_MS;
}

static void cw_cid_defer(struct line *l, const struct hal_cid *cid, uint32_t lead_ms)
{
	if (!region_applied[0]) {
		TRACE(TRACE_FSM, "line %u call waiting record dropped: no tone profile "
		      "has loaded, so no alert tone can precede it", l->index);
		return;
	}

	l->cw_cid = *cid;
	l->cw_cid_timer.cb = cw_cid_expire;
	uloop_timeout_set(&l->cw_cid_timer, (int)lead_ms);
}

/* ------------------------------------------------------ forward disconnect */

/*
 * The feature is the pair. A reversal length alone says how long but not
 * what, a battery form alone says what but not how long, so neither enables
 * anything by itself and the shipped config, which carries neither, is off
 * (cfg_fwd_disc_on).
 *
 * The gate reads the live channel copy, so a reload cannot start or stop a
 * reversal that is already running: the staged copy lands at the next rest
 * state like the rest of the view.
 *
 * The config arms it, the backend has to be able to carry it. A board whose
 * SLIC cannot reverse the loop answers the op with HAL_UNSUPPORTED forever, so
 * the availability is resolved once at the derive, beside the other faults of
 * this file, rather than once per reversal.
 */
static bool fwd_disc_on(const struct line *l)
{
	return l->fwd_disc_avail && cfg_fwd_disc_on(&l->chan);
}

static uint32_t fwd_disc_ms(const struct line *l)
{
	uint32_t ms = l->chan.forward_disconnect_delay_ms;

	return ms > LINE_FWD_DISC_MAX_MS ? LINE_FWD_DISC_MAX_MS : ms;
}

static void fwd_disc_polarity(struct line *l, enum hal_polarity pol)
{
	enum hal_status st;

	if (!uv.hal)
		return;

	st = hal_backend_ops.line_polarity_set(uv.hal, l->index, pol);
	if (st == HAL_OK)
		return;

	ULOG_WARN("line %u polarity not applied: %s\n", l->index,
		  hal_status_name(st));
}

/*
 * The restore is unconditional and it is doubled. The second half really
 * executes and is required, not defensive. The reversal really does reach the
 * SLIC, so a line whose restore timer never ran stays physically reversed:
 * the subscriber who goes on hook inside the window, the line forced out of
 * service, and the daemon that stops would each leave the loop backwards into
 * the next call. The timer below is one half and fwd_disc_abort() is the
 * other, which every rest state, the out of service force, a timer cancel and
 * lines_done() reach. The same backstops carry the removal form, where the
 * state left behind is a line with no battery at all rather than a backwards
 * one.
 *
 * The flag is what makes the backstop free: a line that never asked for a
 * reversal issues no backend call at all.
 */
static void fwd_disc_forward(struct line *l)
{
	uloop_timeout_cancel(&l->fwd_disc_restore);

	if (!l->fwd_disc_reversed)
		return;

	l->fwd_disc_reversed = false;
	fwd_disc_polarity(l, HAL_POL_FORWARD);
	TRACE(TRACE_FSM, "line %u forward disconnect restored", l->index);
}

/*
 * The removal window of CLEARING. The remove form breaks the loop instead of
 * turning it around: HAL_LINE_DOWN de-energizes the SLIC for the configured
 * length and HAL_LINE_ACTIVE, which the backend programs in the recorded
 * polarity, brings it back.
 *
 * Why the hook intake has to go quiet with it. This daemon is handed
 * classified hook decisions and not edges, and the DSP cannot tell our own
 * loop break from a subscriber: a removal of 300 to 1300 ms comes back as a
 * FLASH, and a longer one as a RELEASE and then a SEIZE. Both are ruinous in
 * CLEARING, which is where every removal is made. A flash resumes a held leg
 * or hands out dial tone, and a hotline line then places a call; a release
 * rings a held leg back or takes the line to IDLE. So every class is dropped
 * for the whole window.
 *
 * The window is a flag with an owner and a deadline. fwd_disc_abort() closes
 * it on every path, so no cancel can leave the suppression alive for the
 * life of the port.
 */
static enum hal_status fwd_disc_line_state(struct line *l, enum hal_line_state state)
{
	enum hal_status st;

	if (!uv.hal)
		return HAL_FATAL;

	st = hal_backend_ops.line_state_set(uv.hal, l->index, state);

	/* hal.h section 3: the retry runs in this frame, like hook_timing_push()
	 * and hook_state_sync() */
	if (st == HAL_TRANSIENT)
		st = hal_backend_ops.line_state_set(uv.hal, l->index, state);

	if (st == HAL_OK) {
		l->battery_known = true;
		l->battery_on = state == HAL_LINE_ACTIVE;
		l->battery_retry = false;
	}

	/* the callers report it: a refused removal is a window that never opened,
	 * and a refused restore is a line with no battery, which are not the same
	 * severity and do not read the same in a log */
	return st;
}

static void fwd_disc_settle_expire(struct uloop_timeout *t)
{
	struct line *l = container_of(t, struct line, fwd_disc_settle);

	/* the flag goes first: the sync injects through the real intake */
	l->fwd_disc_down = false;
	TRACE(TRACE_FSM, "line %u battery removal settled, reading the hook state",
	      l->index);
	hook_state_sync(l);
}

static void fwd_disc_retry_expire(struct uloop_timeout *t);

static void fwd_disc_retry_arm(struct line *l, enum hal_line_state state,
			       enum hal_status st)
{
	ULOG_ERR("line %u battery %s refused: %s, asking again in %u ms\n",
		 l->index, state == HAL_LINE_ACTIVE ? "restore" : "removal",
		 hal_status_name(st), LINE_FWD_DISC_RETRY_MS);

	l->fwd_disc_retry.cb = fwd_disc_retry_expire;
	uloop_timeout_set(&l->fwd_disc_retry, LINE_FWD_DISC_RETRY_MS);
}

/*
 * Out of the dead state, whichever attempt gets there. The settle starts from
 * the ACTIVE that succeeded and not from the moment the removal was supposed
 * to end, because what it measures is the classifier settling on an energised
 * line; a restore that took three attempts moved that moment with it.
 *
 * A refused ACTIVE lifts the suppression and keeps the loop owed. No hook
 * report can arrive on a loop with no battery, so the intake has nothing left
 * to protect, and holding it shut would only hide the subscriber from the
 * moment the retry lands. The retry itself does not give up: a line left down
 * can never carry a call again, and nothing else in this daemon would ever
 * command it.
 *
 * A configuration that disabled the line meanwhile owns the battery: the loop
 * is no longer owed, and battery_sync() takes it down.
 */
static void fwd_disc_revive(struct line *l)
{
	enum hal_status st;

	if (!l->battery_wanted) {
		l->fwd_disc_dead = false;
		l->fwd_disc_down = false;
		uloop_timeout_cancel(&l->fwd_disc_retry);
		return;
	}

	st = fwd_disc_line_state(l, HAL_LINE_ACTIVE);
	if (st != HAL_OK) {
		l->fwd_disc_down = false;
		fwd_disc_retry_arm(l, HAL_LINE_ACTIVE, st);
		return;
	}

	l->fwd_disc_dead = false;
	uloop_timeout_cancel(&l->fwd_disc_retry);
	l->fwd_disc_down = true;
	l->fwd_disc_settle.cb = fwd_disc_settle_expire;
	uloop_timeout_set(&l->fwd_disc_settle, LINE_FWD_DISC_SETTLE_MS);
	TRACE(TRACE_FSM, "line %u battery restored, hook events suppressed for %u ms",
	      l->index, LINE_FWD_DISC_SETTLE_MS);
}

/*
 * The battery follows the configuration and the loop test. A port the config
 * takes out of service carries no battery unless the loop test holds it, so a
 * handset on it hears nothing and no hook report reaches this daemon from it.
 * A port in service carries it, whatever the two hardware reasons say,
 * because the backend clears a fault only under a confirmed powered feed
 * (udsp chan_fault_clear_due) and a dead line's return is the one event that
 * re-derives this.
 *
 * The raise suppresses nothing and reads no hook state of its own. A lifted
 * handset closes the loop once the feed is up, and the classifier reports
 * that as the ordinary seize a moment later; the query hook_state_sync()
 * makes in the same frame reads the loop before it closed and injects
 * nothing, which is the right answer for that frame.
 *
 * A refused write is asked again every LINE_FWD_DISC_RETRY_MS, through the
 * same timer the removal window uses, until it lands. The backend refuses
 * every line state request while a SLOW job holds its control thread
 * (udsp_guard job_inflight), and the region load lines_start() submits is
 * one; so is a dead line's own return, which posts its event before the
 * retry that brought it back has released the thread.
 *
 * A dead line writes nothing: the backend answers fatal until its return
 * event, and that event runs this derive. A removal window that still owns
 * the loop keeps it: its own timers restore the battery.
 */
static void battery_sync(struct line *l)
{
	enum hal_line_state want;
	enum hal_status st;

	if (!uv.hal || uv.hal_state != UVOICE_HAL_READY || !line_exists(l->index))
		return;
	if (l->down)
		return;
	if (l->battery_wanted && l->fwd_disc_dead)
		return;
	if (l->battery_known && l->battery_on == l->battery_wanted)
		return;

	want = l->battery_wanted ? HAL_LINE_ACTIVE : HAL_LINE_DOWN;
	st = fwd_disc_line_state(l, want);
	if (st != HAL_OK) {
		l->battery_retry = true;
		fwd_disc_retry_arm(l, want, st);
		return;
	}

	uloop_timeout_cancel(&l->fwd_disc_retry);
	if (want == HAL_LINE_DOWN) {
		uloop_timeout_cancel(&l->fwd_disc_settle);
		l->fwd_disc_down = false;
		TRACE(TRACE_FSM, "line %u battery removed: the line is out of service",
		      l->index);
		return;
	}

	TRACE(TRACE_FSM, "line %u battery raised: the line is in service",
	      l->index);
}

static void fwd_disc_retry_expire(struct uloop_timeout *t)
{
	struct line *l = container_of(t, struct line, fwd_disc_retry);

	if (l->fwd_disc_dead)
		fwd_disc_revive(l);
	else
		battery_sync(l);
}

/*
 * The window ends here and nowhere else: the timers go, a battery that is
 * still off is put back, and the suppression is cleared. It never derives:
 * the settle is the only reader of the hook state, so an abort cannot inject
 * an event into a line the shell is in the middle of taking away.
 *
 * The settle must be cancelled here. lines_init() zeroes the line storage
 * that the timer list is threaded through, so an armed settle would fire into
 * a freed line and the suite dies with a SIGSEGV.
 *
 * A line on its way out stops retrying. line_timers_cancel() runs in front of
 * exactly that memset, and the backend programs LINE_DOWN at the close anyway,
 * so those callers make one attempt, say so and stop.
 */
static void fwd_disc_abort(struct line *l, bool leaving)
{
	enum hal_status st;

	uloop_timeout_cancel(&l->fwd_disc_settle);
	/* the reversal form's half, which also cancels the shared restore */
	fwd_disc_forward(l);

	if (l->fwd_disc_down) {
		l->fwd_disc_down = false;
		TRACE(TRACE_FSM, "line %u battery removal ended early, hook events "
		      "pass again", l->index);
	}

	if (!l->fwd_disc_dead) {
		/* the timer may be carrying a refused config write instead,
		 * which only the line's departure ends */
		if (leaving || !l->battery_retry)
			uloop_timeout_cancel(&l->fwd_disc_retry);
		return;
	}

	/* the config took the line out of service inside the window: the loop
	 * is not owed back, battery_sync() takes it down for good */
	if (!l->battery_wanted) {
		l->fwd_disc_dead = false;
		uloop_timeout_cancel(&l->fwd_disc_retry);
		return;
	}

	st = fwd_disc_line_state(l, HAL_LINE_ACTIVE);
	if (st == HAL_OK) {
		l->fwd_disc_dead = false;
		uloop_timeout_cancel(&l->fwd_disc_retry);
		return;
	}

	if (leaving) {
		uloop_timeout_cancel(&l->fwd_disc_retry);
		ULOG_ERR("line %u battery is left down: %s, and the line is going "
			 "away\n", l->index, hal_status_name(st));
		return;
	}

	fwd_disc_retry_arm(l, HAL_LINE_ACTIVE, st);
}

/*
 * The restore is not the end of the window. The battery comes back here and
 * the suppression stays on for the settle, because the classifier answers for
 * the energised line only after its own debounce and a release it is still
 * measuring lands later still. Nothing is derived from the hook until then,
 * and then it is read once, through the query hook_state_sync() already owns.
 *
 * The dead flag and not the suppression decides which form this is. They are
 * the same bit in the ordinary window and differ behind a refused restore: a
 * read of the suppression here would send a removal down the reversal path.
 */
static void fwd_disc_restore_expire(struct uloop_timeout *t)
{
	struct line *l = container_of(t, struct line, fwd_disc_restore);

	if (!l->fwd_disc_dead) {
		fwd_disc_forward(l);
		return;
	}

	fwd_disc_revive(l);
}

/*
 * The invariant a reversal needs. Both caller id records ride the line
 * voltage, so a reversal inside a ring cadence, or in front of the type 2
 * push a call waiting alert has queued, corrupts what the display decodes.
 *
 * No path of the current matrix reaches either of those two branches, and
 * that is the point of writing them down: a reversal is only ever made from
 * CLEARING, no cell enters CLEARING with the bell running (RINGING has no
 * CLEARING target and the HOLDING seize stops the bell first), and every path
 * into CLEARING ends the alert, which drops the queued record with it. This
 * states the property the reversal depends on instead of leaving it to be
 * rediscovered when a later change adds a state that breaks it.
 *
 * The third branch backs up the episode rule. A window that is still open
 * owns the loop: a second removal inside it would take a battery that is
 * already off, and its restore and settle would then race the ones already
 * armed. fwd_disc_hold_expire() marks the episode, so no path of the current
 * matrix arms a second signal inside an open window. This branch states that
 * invariant, like the two above it.
 *
 * A suppressed signal is dropped with one trace and never deferred: it means
 * "the far end released just now", and a late one says something that is no
 * longer true.
 */
static bool fwd_disc_blocked(const struct line *l)
{
	return l->ringing || l->cw_cid_timer.pending ||
	       l->fwd_disc_down || l->fwd_disc_dead;
}

/* it cannot fail the way a removal can: a SLIC that refuses the polarity op is
 * reported where the op is made and the restore still runs */
static bool fwd_disc_reverse(struct line *l)
{
	l->fwd_disc_reversed = true;
	fwd_disc_polarity(l, HAL_POL_REVERSE);
	TRACE(TRACE_FSM, "line %u forward disconnect reversed for %u ms", l->index,
	      fwd_disc_ms(l));

	return true;
}

/*
 * A refused removal opens no window. The suppression above exists to cover the
 * hook reports our own loop break produces, so a break the backend never made
 * has nothing to cover and nothing to undo: the line stays ACTIVE and every
 * hook report on it is the subscriber's.
 */
static bool fwd_disc_remove(struct line *l)
{
	enum hal_status st;

	/*
	 * A window that re-opens owns its own deadline. A settle still armed
	 * from an earlier window would expire over a battery this one has just
	 * taken off, lift the suppression there and leave the loop owed with
	 * nobody waiting to command it back. fwd_disc_blocked() above already
	 * refuses the second window; this is the other lock on the same door,
	 * because the cost of being wrong is a line that never rings again.
	 */
	uloop_timeout_cancel(&l->fwd_disc_settle);

	st = fwd_disc_line_state(l, HAL_LINE_DOWN);
	if (st != HAL_OK) {
		ULOG_WARN("line %u battery removal not applied: %s\n", l->index,
			  hal_status_name(st));
		return false;
	}

	l->fwd_disc_dead = true;
	l->fwd_disc_down = true;
	TRACE(TRACE_FSM, "line %u forward disconnect battery removed for %u ms",
	      l->index, fwd_disc_ms(l));

	return true;
}

/* false when a guard or the backend dropped it, which the once-per-episode
 * rule reads. The battery form chooses the arm; the length is the same option
 * and the same restore timer in both */
static bool fwd_disc_apply(struct line *l)
{
	bool armed;

	if (fwd_disc_blocked(l)) {
		TRACE(TRACE_FSM, "line %u forward disconnect dropped: the bell, a "
		      "caller id record or a removal already owns the line", l->index);
		return false;
	}

	if (l->chan.forward_disconnect_battery == CFG_FWD_DISC_BATT_REMOVE)
		armed = fwd_disc_remove(l);
	else
		armed = fwd_disc_reverse(l);

	if (!armed)
		return false;

	l->fwd_disc_restore.cb = fwd_disc_restore_expire;
	uloop_timeout_set(&l->fwd_disc_restore, (int)fwd_disc_ms(l));

	return true;
}

/* ------------------------------------------------------------------ region */

/*
 * cfg->region has to name a region the backend loads. The table maps the ISO
 * 3166 alpha-2 forms onto the three-letter region names, so a config written
 * as "US" or "DE" resolves.
 *
 * Two rows resolve to names that are not the ISO alpha-3 of the key. EU is no
 * country code, and it takes ETS, the generic ETSI profile. RS, the alpha-2 of
 * Serbia, takes YUG, a 425 Hz Serbian set (interrupted dial 200/300/700/800,
 * call waiting 300/9000), which the udsp backend loads from SRB.conf. Without
 * these rows both regions would warn and load the USA tones.
 *
 * AT is a plain ISO row. AUT is its alpha-3, and udsp ships AUT.conf. Every
 * plug-in variant gives AUT the AU impedance and the 25 Hz ring, so the name
 * has to reach the udsp backend unchanged.
 */
static const struct {
	char alpha2[3];
	char iso3[4];
} region_alpha2[] = {
	{ "AU", "AUS" }, { "BE", "BEL" }, { "BR", "BRA" }, { "CH", "CHE" },
	{ "CL", "CHL" }, { "CN", "CHN" }, { "DE", "DEU" }, { "DK", "DNK" },
	{ "ES", "ESP" }, { "EU", "ETS" }, { "AT", "AUT" }, { "FI", "FIN" },
	{ "FR", "FRA" }, { "GB", "GBR" },
	{ "UK", "GBR" }, { "HK", "HKO" }, { "HU", "HUN" }, { "IN", "IND" },
	{ "IR", "IRN" }, { "IT", "ITA" }, { "JP", "JPN" }, { "LK", "LKA" },
	{ "MX", "MEX" }, { "NL", "NLD" }, { "NZ", "NZL" }, { "PL", "POL" },
	{ "RU", "RUS" }, { "SE", "SWE" }, { "TH", "THA" }, { "US", "USA" },
	{ "VN", "VNM" }, { "RS", "YUG" },
};

static char char_upper(char c)
{
	if (c >= 'a' && c <= 'z')
		return (char)(c - 'a' + 'A');

	return c;
}

static void region_iso3_get(const char *region, char out[LINE_REGION_MAX])
{
	size_t len = region ? strlen(region) : 0;
	char up[LINE_REGION_MAX] = { 0 };
	size_t i;

	memset(out, 0, LINE_REGION_MAX);

	if (len >= 2 && len < LINE_REGION_MAX) {
		for (i = 0; i < len; i++)
			up[i] = char_upper(region[i]);
	}

	/* Every name of three characters or more goes to the backend verbatim.
	 * A config can name a region the alpha-2 table cannot express, for
	 * example ETS or SRB, and a name can be longer than three characters,
	 * so the width of the backend buffer is the only limit. The udsp
	 * backend loads ETS or USA for a name with no region file, and warns
	 * once. The read-back keeps naming the region the backend holds. */
	if (len >= 3 && len < LINE_REGION_MAX) {
		memcpy(out, up, LINE_REGION_MAX);
		return;
	}

	for (i = 0; len == 2 && i < ARRAY_SIZE(region_alpha2); i++) {
		if (strcmp(region_alpha2[i].alpha2, up))
			continue;

		memcpy(out, region_alpha2[i].iso3, sizeof(region_alpha2[i].iso3));
		return;
	}

	ULOG_WARN("region '%s' is not a known region name, using %s\n",
		  region ? region : "", LINE_REGION_DEFAULT);
	memcpy(out, LINE_REGION_DEFAULT, sizeof(LINE_REGION_DEFAULT));
}

static void region_submit(void);
static void tone_replay_all(void);

static bool region_hal_take(void)
{
	struct hal_region reg;

	if (!uv.hal || !hal_backend_ops.region_get)
		return false;
	if (hal_backend_ops.region_get(uv.hal, &reg) != HAL_OK)
		return false;

	region_hal = reg;
	region_hal_read = true;

	return true;
}

static void region_hal_drop(void)
{
	region_hal = region_hal_default;
	region_hal_read = false;
}

/*
 * Re-ask rather than assume: the library keeps its previous table across a
 * failed load, so after any region job the getter names what the DSP really
 * plays, and only a refusal drops back to the compiled default. Nothing to
 * settle when no answer stood before and none stands now (a backend with no
 * region_get): a settle drains every line, and a drain that lands a view
 * resets the runtime star code state, so a backend that never answers must
 * not pay that on every region job.
 */
static void region_hal_settle_all(void)
{
	bool had = region_hal_read;
	unsigned int i;

	if (!region_hal_take())
		region_hal_drop();

	if (!region_hal_read && !had)
		return;

	for (i = 0; i < CFG_CHANNEL_MAX; i++)
		region_settle(&lines[i]);
}

/*
 * A tone table that did not load leaves the subscriber in silence, which is
 * bad, and a daemon that exits leaves the subscriber with no line at all,
 * which is worse. So a failure is reported and the call path keeps running;
 * the next reload retries.
 */
static void region_done(struct hal_job *job, enum hal_status status,
			const struct hal_event *ev)
{
	bool first = region_applied[0] == 0;

	region_busy = false;

	if (status != HAL_OK) {
		ULOG_WARN("region %s not applied: %s, tones stay unconfigured\n",
			  region_inflight, hal_status_name(status));

		/*
		 * What the DSP holds is now unknown. A push that failed partway
		 * leaves an installed prefix of the new table, so the name of
		 * the previous region no longer describes it. Blanking the
		 * applied name keeps region_submit from skipping a later config
		 * that names that previous region (want == applied), which would
		 * strand the hybrid table for the life of the daemon.
		 *
		 * Two consequences, both deliberate: the next successful load
		 * replays a pending tone, and the call waiting record is dropped
		 * until then, because no slot is known to carry the alert tone
		 * it has to sit behind (cw_cid_defer).
		 */
		region_applied[0] = 0;

		region_hal_settle_all();

		/*
		 * A reload moved the target while this job was in flight. The
		 * failure of the code that job carried says nothing about the
		 * new one, so it must not bury it until some later reload. An
		 * unmoved target stops here rather than retrying in a loop.
		 */
		if (strcmp(region_want, region_inflight))
			region_submit();

		return;
	}

	/* what the DSP holds is what this job carried, which a reload may have
	 * already replaced as the wanted one */
	str_copy(region_applied, sizeof(region_applied), region_inflight);
	ULOG_INFO("region %s applied\n", region_applied);

	region_hal_settle_all();

	/* the tone profiles exist only now, so whatever was asked for while
	 * they did not is played here */
	if (first)
		tone_replay_all();

	region_submit();
}

static void region_submit(void)
{
	enum hal_status st;

	if (region_busy || !uv.hal || uv.hal_state != UVOICE_HAL_READY)
		return;
	if (!strcmp(region_want, region_applied))
		return;

	str_copy(region_inflight, sizeof(region_inflight), region_want);
	region_job.cb = region_done;
	st = hal_backend_ops.region_apply(uv.hal, region_inflight, &region_job);
	if (st != HAL_OK) {
		ULOG_WARN("region %s refused: %s\n", region_inflight, hal_status_name(st));
		return;
	}

	region_busy = true;
	job_add(&region_job);
	TRACE(TRACE_HAL, "region %s submitted, token %u", region_inflight, region_job.token);
}

static void region_take(const struct cfg *cfg)
{
	region_iso3_get(cfg->region, region_want);
	region_submit();
}

/* a completion after the lines are gone would report on a dead shell */
static void region_drop(void)
{
	if (region_busy)
		job_take(region_job.token);

	region_busy = false;
}

/* ------------------------------------------------------------------- media */

static enum hal_status media_config_set(void *priv, unsigned int line,
					unsigned int stream,
					const struct hal_media *media)
{
	struct hal *h = priv;

	if (!h)
		return HAL_FATAL;

	return hal_backend_ops.stream_config_set(h, line, stream, media);
}

/*
 * Which (line, stream) pairs carry a running stream. A live query measures a
 * running stream, so it has to know which pairs can answer one at all. The
 * two ops below are the only places a stream starts and stops, so the flag
 * is taken where the backend answered rather than derived from the leg: the
 * leg belongs to call.c and this module holds no copy of it.
 *
 * It is therefore also the answer to "is there a call there": an empty slot
 * and a leg whose media never started are one refusal here, because the leg
 * layer exports no per-slot occupancy predicate and a second copy of its
 * in_use flag would drift from it.
 */
static bool media_running[CFG_CHANNEL_MAX][HAL_STREAM_MAX];

static void media_running_set(unsigned int line, unsigned int stream, bool on)
{
	if (line >= CFG_CHANNEL_MAX || stream >= HAL_STREAM_MAX)
		return;

	media_running[line][stream] = on;
}

static enum hal_status media_start(void *priv, unsigned int line, unsigned int stream)
{
	struct hal *h = priv;
	enum hal_status st;

	if (!h)
		return HAL_FATAL;

	st = hal_backend_ops.stream_start(h, line, stream);
	if (st == HAL_OK)
		media_running_set(line, stream, true);

	return st;
}

static enum hal_status media_stop(void *priv, unsigned int line, unsigned int stream)
{
	struct hal *h = priv;

	if (!h)
		return HAL_FATAL;

	/* the leg stops using the stream whatever the backend answers, so a
	 * refused stop must not leave a pair this module calls running */
	media_running_set(line, stream, false);

	return hal_backend_ops.stream_stop(h, line, stream);
}

static enum hal_status media_dtmf_send(void *priv, unsigned int line,
				       unsigned int stream, char digit,
				       uint32_t duration_ms)
{
	struct hal *h = priv;

	if (!h)
		return HAL_FATAL;

	return hal_backend_ops.stream_dtmf_send(h, line, stream, digit, duration_ms);
}

/*
 * The media snapshot. call.c asks for it at the only moment the numbers can
 * be read, with the stream still running, and cannot wait for it: the query
 * is a slow op and the leg is released inside the same frame. The call record
 * handle owns the answer, so a completion that arrives after the leg is gone
 * still lands in the right record, and one that arrives after the record
 * itself was evicted is dropped by the generation check.
 *
 * One job slot per (line, stream), which is the region_job pattern per
 * stream. A second teardown of the same stream while the first job is still
 * out takes the slot: the stale token leaves the pending list and its record
 * is closed as failed, so two jobs are never pending on one stream and no
 * answer can be credited to the wrong call.
 *
 * Two consumers share that one slot. A call record waiting for its numbers
 * carries a record handle and no callback; a live "voice stats_live" query
 * carries a callback and no handle. Which one holds the slot decides who is
 * answered, and it decides who may take it: the takeover above belongs to the
 * CDR path alone, because a record that loses its snapshot loses a fact
 * nothing can produce again, while a live query can simply be asked again by
 * whoever asked it. A live query therefore refuses busy instead
 * (line_stats_live_query) and displaces nothing.
 */
struct stats_slot {
	struct hal_job job;
	uint32_t handle;		/* the record the answer belongs to */
	line_stats_cb live_cb;		/* the live consumer, NULL for a record */
	void *live_priv;
	uint8_t line;
	uint8_t stream;
	bool busy;
};

static struct stats_slot stats_slots[CFG_CHANNEL_MAX][HAL_STREAM_MAX];

static void stats_answer(line_stats_cb cb, void *priv, uint32_t handle,
			 const struct hal_media_stats *st)
{
	if (cb) {
		cb(priv, st);
		return;
	}

	if (!st) {
		cdr_media_fail(handle);
		return;
	}

	cdr_media_set(handle, st);
}

/* the slot is cleared before the consumer runs, so a consumer that asks again
 * from inside its own answer finds a free slot rather than its own stale one */
static void stats_slot_release(struct stats_slot *s, const struct hal_media_stats *st)
{
	line_stats_cb cb = s->live_cb;
	void *priv = s->live_priv;
	uint32_t handle = s->handle;

	s->busy = false;
	s->handle = CDR_HANDLE_NONE;
	s->live_cb = NULL;
	s->live_priv = NULL;

	stats_answer(cb, priv, handle, st);
}

static void media_stats_done(struct hal_job *job, enum hal_status status,
			     const struct hal_event *ev)
{
	struct stats_slot *s = container_of(job, struct stats_slot, job);
	struct hal_media_stats stats;

	/* ev is NULL when the core backstop fired instead of a completion
	 * (hal.h section 2), so nothing here reads it */
	if (status != HAL_OK || !uv.hal) {
		TRACE(TRACE_HAL, "line %u strm %u media stats failed: %s", s->line,
		      s->stream, hal_status_name(status));
		stats_slot_release(s, NULL);
		return;
	}

	if (hal_backend_ops.stream_stats_result_get(uv.hal, s->line, s->stream,
						    &stats) != HAL_OK) {
		TRACE(TRACE_HAL, "line %u strm %u media stats unreadable", s->line,
		      s->stream);
		stats_slot_release(s, NULL);
		return;
	}

	stats_slot_release(s, &stats);
}

static void stats_slot_take(struct stats_slot *s)
{
	if (!s->busy)
		return;

	if (job_take(s->job.token))
		TRACE(TRACE_HAL, "line %u strm %u media stats token %u dropped",
		      s->line, s->stream, s->job.token);

	stats_slot_release(s, NULL);
}

static void jb_snapshot(unsigned int line, unsigned int stream)
{
	struct hal_jb_stats jb;

	if (!hal_backend_ops.stream_jb_stats_get)
		return;

	memset(&jb, 0, sizeof(jb));
	/* A refusal here leaves the poll cache exactly as it was. The last
	 * successful reading of this stream is still the best answer anyone
	 * has, and losing it because the teardown arrived a tick too late
	 * would be worse than reporting numbers a moment old. */
	if (hal_backend_ops.stream_jb_stats_get(uv.hal, line, stream, &jb) !=
	    HAL_OK)
		return;

	/* The final numbers of the call reach the surface "voice status"
	 * renders even when nobody polled while the call ran, which is the
	 * whole reason this capture and not a timer feeds the cache. */
	uvoice_jb_cache_set(line, stream, &jb);
	cdr_jb_set(line, stream, &jb);
}

/*
 * The line canceller counterpart of jb_snapshot(): the same teardown read, so
 * "voice status" keeps the canceller reading of a line after its call ended.
 * The canceller belongs to the line, so this takes no stream.
 *
 * The getter refuses only a line that holds no canceller instance. That
 * refusal leaves the cache as it was, which is the jb rule. A bypassed
 * canceller answers ec_active 0 and is cached like any other reading. No CDR
 * write: the record carries no canceller field.
 */
static void lec_snapshot(unsigned int line)
{
	struct hal_lec_stats lec;

	if (!hal_backend_ops.line_lec_stats_get)
		return;

	memset(&lec, 0, sizeof(lec));
	if (hal_backend_ops.line_lec_stats_get(uv.hal, line, &lec) != HAL_OK)
		return;

	uvoice_lec_cache_set(line, &lec);
}

static void media_stats_snapshot(void *priv, unsigned int line, unsigned int stream)
{
	struct stats_slot *s;
	enum hal_status st;
	uint32_t handle;

	if (line >= CFG_CHANNEL_MAX || stream >= HAL_STREAM_MAX)
		return;

	s = &stats_slots[line][stream];
	/* a live query holding the slot is displaced here and answered with its
	 * failure, which is what keeps a call that ends mid-query from leaving a
	 * deferred ubus reply behind */
	stats_slot_take(s);

	/*
	 * The region_submit gate: no backend, or a DSP that is not up, cannot
	 * answer a media query. The record then carries no media rather than a
	 * failure, because nothing was ever asked.
	 */
	if (!priv || !uv.hal || uv.hal_state != UVOICE_HAL_READY) {
		TRACE(TRACE_HAL, "line %u strm %u media stats not taken, the dsp is "
		      "not ready", line, stream);
		return;
	}

	/*
	 * The jitter buffer counters are taken here and not with the media
	 * snapshot. They come from a fast op, so they need no job and no
	 * handle, and they are read before the slow query can refuse: a
	 * backend that answers no media numbers still has a buffer to report,
	 * and the record would otherwise carry neither. A backend without one
	 * answers HAL_UNSUPPORTED and the record simply omits the table.
	 */
	jb_snapshot(line, stream);
	/* The canceller reading of the line, kept for "voice status" the same
	 * way. Per line, so a two leg line captures it on each teardown and the
	 * later reading wins; harmless. */
	lec_snapshot(line);

	handle = cdr_media_pending(line, stream);
	/* the slot describes the job before the op can accept it, so the
	 * completion path never depends on when the answer arrives */
	s->handle = handle;
	s->line = (uint8_t)line;
	s->stream = (uint8_t)stream;
	s->job.cb = media_stats_done;

	st = hal_backend_ops.stream_stats_start(uv.hal, line, stream, &s->job);
	if (st != HAL_OK) {
		TRACE(TRACE_HAL, "line %u strm %u media stats refused: %s", line, stream,
		      hal_status_name(st));
		s->handle = CDR_HANDLE_NONE;
		cdr_media_fail(handle);
		return;
	}

	s->busy = true;
	job_add(&s->job);
	TRACE(TRACE_HAL, "line %u strm %u media stats submitted, token %u", line, stream,
	      s->job.token);
}

/*
 * The live query, the northbound half of the same job slot. "voice
 * stats_live" defers its ubus reply, this submits the query, and the
 * completion hands the numbers straight to the consumer: no record is opened,
 * no total moves, and a live query a call never made leaves nothing behind.
 *
 * It names a stream. The ubus verb names a UI slot and translates it through
 * the protocol (ops->stream_of_slot), which answers HAL_STREAM_MAX for a slot
 * no leg holds; that index and a stream that does not run are one refusal.
 *
 * Every refusal is synchronous, so a caller that gets no reply knows the query
 * is running. The order of the two middle ones is deliberate: a pair with no
 * running stream is answered NO_STREAM even while a record snapshot still
 * holds the slot, because "there is nothing to measure" is the more useful
 * truth than "try again"; busy is left for the pair that could have answered,
 * which is a stream still running behind a snapshot the teardown submitted or
 * behind another live query.
 */
enum line_stats line_stats_live_query(unsigned int line, unsigned int stream,
				      line_stats_cb cb, void *priv)
{
	struct stats_slot *s;
	enum hal_status st;

	if (!lines_up) {
		TRACE(TRACE_HAL, "line %u strm %u live stats refused: the lines are "
		      "not up", line, stream);
		return LINE_STATS_NO_LINES;
	}

	/* the inject_line_take refusal set: a port the board does not carry
	 * and a config array narrower than the port count the backend
	 * reports */
	if (!line_exists(line) || !line_get(line)) {
		TRACE(TRACE_HAL, "line %u strm %u live stats refused: this board "
		      "carries no such port", line, stream);
		return LINE_STATS_NO_LINE;
	}

	if (stream >= HAL_STREAM_MAX || !media_running[line][stream]) {
		TRACE(TRACE_HAL, "line %u strm %u live stats refused: no running "
		      "stream", line, stream);
		return LINE_STATS_NO_STREAM;
	}

	s = &stats_slots[line][stream];
	if (s->busy) {
		TRACE(TRACE_HAL, "line %u strm %u live stats refused: a query is "
		      "already out", line, stream);
		return LINE_STATS_BUSY;
	}

	if (!uv.hal || uv.hal_state != UVOICE_HAL_READY) {
		TRACE(TRACE_HAL, "line %u strm %u live stats refused: the dsp is "
		      "not ready", line, stream);
		return LINE_STATS_REFUSED;
	}

	s->handle = CDR_HANDLE_NONE;
	s->live_cb = cb;
	s->live_priv = priv;
	s->line = (uint8_t)line;
	s->stream = (uint8_t)stream;
	s->job.cb = media_stats_done;

	st = hal_backend_ops.stream_stats_start(uv.hal, line, stream, &s->job);
	if (st != HAL_OK) {
		TRACE(TRACE_HAL, "line %u strm %u live stats refused: %s", line,
		      stream, hal_status_name(st));
		s->live_cb = NULL;
		s->live_priv = NULL;
		return LINE_STATS_REFUSED;
	}

	s->busy = true;
	job_add(&s->job);
	TRACE(TRACE_HAL, "line %u strm %u live stats submitted, token %u", line,
	      stream, s->job.token);

	return LINE_STATS_OK;
}

/*
 * The consumer walked away, so the query it owns is abandoned. The job leaves
 * the pending list, which is the hal.h section 2 stale-handle rule: a
 * completion that lands afterwards is unknown to the core, and main.c logs and
 * discards it. Only a live query is dropped; a record snapshot is still owed
 * to a call record and is left alone.
 */
void line_stats_live_drop(unsigned int line, unsigned int stream)
{
	struct stats_slot *s;

	if (line >= CFG_CHANNEL_MAX || stream >= HAL_STREAM_MAX)
		return;

	s = &stats_slots[line][stream];
	if (!s->busy || !s->live_cb)
		return;

	if (job_take(s->job.token))
		TRACE(TRACE_HAL, "line %u strm %u live stats token %u abandoned",
		      line, stream, s->job.token);

	s->busy = false;
	s->live_cb = NULL;
	s->live_priv = NULL;
}

struct line_test_slot {
	struct hal_job job;
	line_test_cb cb;
	void *priv;
	uint8_t line;
	uint8_t test;
	bool busy;
};

static struct line_test_slot line_test_slots[CFG_CHANNEL_MAX];

static bool line_test_running(unsigned int *tested)
{
	unsigned int line;

	for (line = 0; line < CFG_CHANNEL_MAX; line++) {
		if (!line_test_slots[line].busy)
			continue;

		*tested = line;
		return true;
	}

	return false;
}

/* The protocol's flags of one line (proto/proto.h enum proto_line_flag). */
static uint32_t line_flags_of(const struct line *l)
{
	return proto_ops_get()->line_flags(l->index);
}

/* The protocol's name for the state of one line, for a log line. */
static const char *line_state_word(const struct line *l)
{
	const struct proto_ops *ops = proto_ops_get();
	const char *name = ops->state_name ? ops->state_name(l->index) : NULL;

	return name ? name : "?";
}

/* A line where a test disrupts no subscriber interaction (proto/proto.h, the
 * line test predicate): no call a test would end and no session it would
 * cut. */
static bool line_test_state_safe(const struct line *l)
{
	return !(line_flags_of(l) & (PROTO_LF_CALL | PROTO_LF_SESSION));
}

/*
 * A call on any line of the board, a held one included: the clearing chain
 * keeps a held call after an unused recall dial tone of HOLD_DIAL, and a
 * test or the loop mode would end that call.
 */
static bool line_test_call_active(void)
{
	unsigned int line;

	for (line = 0; line < CFG_CHANNEL_MAX; line++) {
		if (line_flags_of(&lines[line]) & PROTO_LF_CALL)
			return true;
	}

	return false;
}

/*
 * libudsp refuses every SYNC-SLIC op while a test runs. A ring refused for
 * that reason is held and issued again when the test completes (ring_issue
 * below), so a call that arrives during a test still reaches the subscriber.
 *
 * The test also opens and rings its own line. Only a line at rest, or one
 * whose handset is up with no session and no call (the SIP ROH and SILENT),
 * permits that action without disrupting a handset session.
 */
static bool line_test_policy_refused(unsigned int line)
{
	/* The loop test holds every port and its own audio path, and a
	 * metallic test would take the part away from it. The line reads out
	 * of service while the mode stands, which the predicate above admits,
	 * so the mode is refused by name. */
	if (loop_test_armed)
		return true;

	if (!line_test_state_safe(&lines[line]))
		return true;

	return line_test_call_active();
}

static void ring_hold_replay(void);

static void line_test_finish(struct line_test_slot *slot,
			     enum line_test_result_status status,
			     const void *record, size_t len)
{
	line_test_cb cb = slot->cb;
	void *priv = slot->priv;

	slot->busy = false;
	slot->cb = NULL;
	slot->priv = NULL;
	/* the part is free from the moment the slot is, so a ring this test
	 * held reaches the line before the consumer reads the result */
	ring_hold_replay();
	cb(priv, status, record, len);
}

static void line_test_done(struct hal_job *job, enum hal_status status,
			   const struct hal_event *ev)
{
	struct line_test_slot *slot = container_of(job, struct line_test_slot,
						   job);
	uint8_t record[LINE_TEST_RECORD_MAX];
	enum hal_status read_status;
	size_t len = 0;

	if (!slot->busy)
		return;
	if (!ev) {
		line_test_finish(slot, LINE_TEST_RESULT_TIMEOUT, NULL, 0);
		return;
	}
	if (status == HAL_FATAL || !uv.hal) {
		line_test_finish(slot, LINE_TEST_RESULT_FATAL, NULL, 0);
		return;
	}
	if (status == HAL_UNSUPPORTED) {
		line_test_finish(slot, LINE_TEST_RESULT_UNSUPPORTED, NULL, 0);
		return;
	}

	read_status = hal_backend_ops.line_test_result_get(
		uv.hal, slot->line, record, sizeof(record), &len);
	if (read_status == HAL_UNSUPPORTED && status == HAL_TRANSIENT) {
		line_test_finish(slot, LINE_TEST_RESULT_FAILED, NULL, 0);
		return;
	}
	if (read_status != HAL_OK) {
		TRACE(TRACE_HAL, "line %u test %u result unreadable: %s",
		      slot->line, slot->test, hal_status_name(read_status));
		line_test_finish(slot, LINE_TEST_RESULT_UNREADABLE, NULL, 0);
		return;
	}

	line_test_finish(slot,
		status == HAL_OK ? LINE_TEST_RESULT_COMPLETE :
		LINE_TEST_RESULT_FAILED, record, len);
}

enum line_test_query_status line_test_query(unsigned int line,
					    enum hal_line_test test,
					    line_test_cb cb, void *priv)
{
	struct line_test_slot *slot;
	struct line *shell;
	enum hal_status status;

	if (line >= CFG_CHANNEL_MAX || test > HAL_LINE_TEST_LOOP || !cb) {
		TRACE(TRACE_HAL, "line %u test %u refused: bad argument", line,
		      test);
		return LINE_TEST_QUERY_BAD_ARGUMENT;
	}
	if (!uv.cfg || line >= uv.cfg->channels ||
	    !uv.cfg->channel[line].present) {
		TRACE(TRACE_HAL,
		      "line %u test %u refused: no configured line",
		      line, test);
		return LINE_TEST_QUERY_NO_LINE;
	}
	if (!lines_up) {
		TRACE(TRACE_HAL, "line %u test %u refused: lines are not up",
		      line, test);
		return LINE_TEST_QUERY_NOT_UP;
	}
	if (!line_exists(line)) {
		TRACE(TRACE_HAL,
		      "line %u test %u refused: no board port", line, test);
		return LINE_TEST_QUERY_BAD_ARGUMENT;
	}

	shell = line_get(line);
	if (!shell || !uv.hal || uv.hal_state != UVOICE_HAL_READY) {
		TRACE(TRACE_HAL, "line %u test %u refused: the line is not up",
		      line, test);
		return LINE_TEST_QUERY_NOT_UP;
	}

	slot = &line_test_slots[line];
	if (slot->busy) {
		TRACE(TRACE_HAL, "line %u test %u refused: a query is pending",
		      line, test);
		return LINE_TEST_QUERY_PENDING;
	}

	if (line_test_policy_refused(line)) {
		TRACE(TRACE_HAL, "line %u test %u refused: a call is active",
		      line, test);
		return LINE_TEST_QUERY_CALL_ACTIVE;
	}

	slot->cb = cb;
	slot->priv = priv;
	slot->line = (uint8_t)line;
	slot->test = (uint8_t)test;
	slot->job.cb = line_test_done;

	status = hal_backend_ops.line_test_start(uv.hal, line, test,
						 &slot->job);
	if (status != HAL_OK) {
		TRACE(TRACE_HAL, "line %u test %u refused by backend: %s",
		      line, test, hal_status_name(status));
		slot->cb = NULL;
		slot->priv = NULL;
		if (status == HAL_UNSUPPORTED)
			return LINE_TEST_QUERY_UNSUPPORTED;
		if (status == HAL_TRANSIENT)
			return LINE_TEST_QUERY_BUSY;
		return LINE_TEST_QUERY_REFUSED;
	}

	slot->busy = true;
	job_add_budget(&slot->job, LINE_TEST_JOB_MS);
	TRACE(TRACE_HAL, "line %u test %u submitted, token %u", line, test,
	      slot->job.token);

	return LINE_TEST_QUERY_OK;
}

void line_test_drop(unsigned int line)
{
	struct line_test_slot *slot;

	if (line >= CFG_CHANNEL_MAX)
		return;

	slot = &line_test_slots[line];
	if (!slot->busy)
		return;

	if (job_take(slot->job.token))
		TRACE(TRACE_HAL, "line %u test token %u abandoned", line,
		      slot->job.token);
	slot->busy = false;
	slot->cb = NULL;
	slot->priv = NULL;
	/* an abandoned test frees the part exactly as a completed one does */
	ring_hold_replay();
}

static void line_tests_drop(void)
{
	unsigned int line;

	for (line = 0; line < CFG_CHANNEL_MAX; line++)
		line_test_drop(line);
}

/* a completion after the lines are gone would report on a dead shell */
static void stats_drop(void)
{
	unsigned int line, stream;

	for (line = 0; line < CFG_CHANNEL_MAX; line++) {
		for (stream = 0; stream < HAL_STREAM_MAX; stream++)
			stats_slot_take(&stats_slots[line][stream]);
	}

	/* the legs went with the shell, so no stream this module started is
	 * running any more; a flag left standing would let a live query ask the
	 * backend about a stream nobody owns */
	memset(media_running, 0, sizeof(media_running));
}

/*
 * The fax switch. The leg builds the profile, because it owns the committed
 * codec; the shell owns the backend, so the op crosses here like every other
 * media op. It is fast (hal.h fax_profile_apply), so the answer is the
 * answer: nothing is deferred and the leg reads the status.
 */
static enum hal_status media_fax_apply(void *priv, unsigned int line, unsigned int stream,
				       const struct hal_fax_profile *fax)
{
	struct hal *h = priv;
	enum hal_status st;

	if (!h)
		return HAL_FATAL;

	st = hal_backend_ops.fax_profile_apply(h, line, stream, fax);

	/*
	 * hal.h section 3: HAL_TRANSIENT means retry once. The retry runs in
	 * this frame, which is the idiom detector_arm() and hook_timing_push()
	 * already use: the op is one short call and the shell must not sleep
	 * on the uloop thread.
	 *
	 * It carries more weight here than it does there. The switch happens
	 * once per leg (tone_event), and the answer tone that drove it does not
	 * come again, so a transient refusal that is dropped instead of retried
	 * costs that call its fax outright.
	 */
	if (st == HAL_TRANSIENT)
		st = hal_backend_ops.fax_profile_apply(h, line, stream, fax);

	return st;
}

/*
 * The tone disabler release of one line. The leg decides that an answer tone
 * was a false one and the shell owns the backend, so the op crosses here like
 * every other media op. It takes the line and no stream, because the disabler
 * is the line's.
 *
 * The op is nullable (hal.h line_lec_latch_clear): a backend with no disabler
 * of its own carries none, and the leg treats the answer as a trace.
 */
static enum hal_status media_lec_latch_clear(void *priv, unsigned int line)
{
	struct hal *h = priv;

	if (!h)
		return HAL_FATAL;

	if (!hal_backend_ops.line_lec_latch_clear)
		return HAL_UNSUPPORTED;

	return hal_backend_ops.line_lec_latch_clear(h, line);
}

/*
 * The SDES entropy of a call leg. The leg takes it through the vtable because
 * the source belongs to the core (proto_media.h rand_bytes), the one
 * struct proto_host random_bytes reads.
 */
static bool media_rand_bytes(void *priv, void *buf, size_t len)
{
	return uvoice_random_bytes(buf, len);
}

static const struct call_media_ops media_ops = {
	.config_set = media_config_set,
	.start = media_start,
	.stop = media_stop,
	.dtmf_send = media_dtmf_send,
	.stats_snapshot = media_stats_snapshot,
	.fax_apply = media_fax_apply,
	.lec_latch_clear = media_lec_latch_clear,
	.rand_bytes = media_rand_bytes,
};

const struct call_media_ops *line_media_ops(void)
{
	return &media_ops;
}

/* -------------------------------------------------------------- tone, ring */

/*
 * Substitution. A backend whose region defines no special information tone
 * or no number-unobtainable tone answers HAL_UNSUPPORTED for it. A refusal
 * would leave the unobtainable clearing silent for its reorder window. The
 * matrix keeps the requested tone as the cell action, and the shell
 * substitutes reorder, the closest tone the hardware does carry, once, at the
 * submission point. Every submission runs it, so the pending-tone replay of a
 * late region load substitutes again; the stored request stays the original.
 * HAL_TONE_SIT keeps the rule, although no cell plays it.
 */
static bool tone_is_unobtainable(enum hal_tone tone)
{
	return tone == HAL_TONE_NUMBER_UNOBTAINABLE || tone == HAL_TONE_SIT;
}

/* One tone toward the phone for duration_ms, 0 until stopped. */
static enum hal_status tone_play_ms(struct line *l, enum hal_tone tone,
				    uint32_t duration_ms)
{
	enum hal_status st;

	if (!uv.hal)
		return HAL_FATAL;

	if (tone == HAL_TONE_NONE)
		return hal_backend_ops.tone_stop(uv.hal, l->index);

	st = hal_backend_ops.tone_play(uv.hal, l->index, tone, duration_ms);
	if (st != HAL_UNSUPPORTED || !tone_is_unobtainable(tone))
		return st;

	TRACE(TRACE_HAL, "line %u has no profile for tone %u, playing reorder",
	      l->index, tone);

	return hal_backend_ops.tone_play(uv.hal, l->index, HAL_TONE_REORDER,
					 duration_ms);
}

/* A tone of the core's own, until stopped: the dial tone class, the confirm
 * tone and the stop. The protocol gives a length to the tones it asks for
 * (proto/proto.h tone_play). */
static enum hal_status tone_apply(struct line *l, enum hal_tone tone)
{
	return tone_play_ms(l, tone, 0);
}

/*
 * The result of a tone the core applied after the host call returned: the
 * deferred dial tone and the replay behind a region load (proto/proto.h
 * ops->signal_result).
 */
static void signal_report(const struct line *l, enum proto_signal sig,
			  enum hal_status st)
{
	const struct proto_ops *ops = proto_ops_get();

	if (ops->signal_result)
		ops->signal_result(l->index, sig, st == HAL_OK);
}

/*
 * A dial tone that waits for the replay keeps the confirm record, so the
 * replay plays the confirm tone in front of it again (tone_replay_all()).
 */
static enum hal_status dial_tone_apply(struct line *l, enum hal_tone tone)
{
	if (!l->tone_pending)
		l->confirm_on = false;

	return tone_apply(l, tone);
}

static void dial_tone_expire(struct uloop_timeout *t)
{
	struct line *l = container_of(t, struct line, dial_tone);

	signal_report(l, PROTO_SIG_TONE,
		      dial_tone_apply(l, (enum hal_tone)l->dial_tone_id));
}

/*
 * Replay. The DSP tone profiles do not exist until region_apply completes,
 * and that job can take seconds while the dial tone defer is half a second,
 * so a subscriber who lifts the handset during bring-up would hold a silent
 * line until the next transition asked for a tone again. The shell therefore
 * remembers the last tone requested while no region is applied, one slot per
 * line. region_done re-issues it on the next successful load. A failed
 * load blanks region_applied, so the line holds its tones again until a load
 * succeeds. The slot is cleared by a tone stop and by a state change that asks
 * for no tone of its own, so nothing stale is ever replayed. Line service is
 * not gated on the region: calls work, only the local tones are late.
 */
static void tone_pending_take(struct line *l, enum hal_tone tone,
			      uint32_t duration_ms)
{
	if (region_applied[0] || tone == HAL_TONE_NONE) {
		l->tone_pending = HAL_TONE_NONE;
		return;
	}

	l->tone_pending = (uint8_t)tone;
	l->tone_pending_ms = duration_ms;
	l->tone_pending_stamped = false;
}

/*
 * The protocol emits the entry tone before it moves to the new state, so the
 * state that owns a request is only known once the transition returns
 * (line_transition, state_id). The first transition after a request stamps
 * it; a later one in a different state drops it, which is the "cleared by
 * state change" half of the rule.
 */
static void tone_pending_settle(struct line *l, unsigned int state_id)
{
	if (!l->tone_pending)
		return;

	if (!l->tone_pending_stamped) {
		l->tone_pending_state = (uint8_t)state_id;
		l->tone_pending_stamped = true;
		return;
	}

	if (l->tone_pending_state != (uint8_t)state_id)
		l->tone_pending = HAL_TONE_NONE;
}

bool line_tone_is_dial(enum hal_tone tone)
{
	return tone == HAL_TONE_DIAL || tone == HAL_TONE_STUTTER;
}

static void confirm_start(struct line *l)
{
	l->confirm_on = true;
	l->confirm_start_ms = uvoice_mono_ms();
	l->confirm_total_ms = region_hal.confirm_total_ms;
	l->confirm_endless = region_hal.confirm_endless;
	tone_apply(l, HAL_TONE_CONFIRM);
}

/*
 * The confirm tone of a star code plays its region length, and the dial tone
 * starts at its end. The length that confirm_start() copied decides, because
 * a region load during the confirm tone does not change the tone that plays:
 *   - a confirm tone that repeats until stopped gets no dial tone, and the
 *     start-digit timer ends it;
 *   - a bound defers the dial tone to that length after the confirm start;
 *   - with no bound known, LINE_DIAL_TONE_DEFER_MS applies, as it does to
 *     every other dial tone.
 * False means that no dial tone plays. The library swaps its tone table before
 * region_done() takes the read-back, so a confirm tone that starts between the
 * two gets the length of the previous region.
 */
static bool dial_tone_wait_get(const struct line *l, uint32_t *wait_ms)
{
	uint32_t bound = l->confirm_total_ms;
	uint32_t elapsed;

	*wait_ms = LINE_DIAL_TONE_DEFER_MS;
	if (!l->confirm_on)
		return true;
	if (l->confirm_endless)
		return false;
	if (!bound)
		return true;

	if (bound > LINE_TIMER_MAX_S * 1000u)
		bound = LINE_TIMER_MAX_S * 1000u;
	elapsed = uvoice_mono_ms() - l->confirm_start_ms;
	*wait_ms = elapsed < bound ? bound - elapsed : 0;

	return true;
}

static enum hal_status dial_tone_set(struct line *l, enum hal_tone tone)
{
	uint32_t wait_ms;

	if (!dial_tone_wait_get(l, &wait_ms)) {
		TRACE(TRACE_FSM, "line %u plays no dial tone behind an endless "
		      "confirm tone", l->index);
		return HAL_OK;
	}

	if (!wait_ms)
		return dial_tone_apply(l, tone);

	l->dial_tone_id = (uint8_t)tone;
	l->dial_tone.cb = dial_tone_expire;
	uloop_timeout_set(&l->dial_tone, (int)wait_ms);

	return HAL_OK;
}

/*
 * One tone request, for duration_ms (0 until stopped). A dial tone and the
 * confirm tone take their own lengths: the dial tone class waits for the
 * detector, and the confirm tone plays its region length.
 */
static enum hal_status tone_set_ms(struct line *l, enum hal_tone tone,
				   uint32_t duration_ms)
{
	uloop_timeout_cancel(&l->dial_tone);
	tone_pending_take(l, tone, duration_ms);

	/*
	 * The class is the exported predicate and the constant stays private:
	 * the stub build compiles the constant to 0 on purpose, so a suite that
	 * asserts the deferral proves nothing about which tones belong to the
	 * class. The suite checks line_tone_is_dial() instead, which is the
	 * half that decides.
	 */
	if (line_tone_is_dial(tone))
		return dial_tone_set(l, tone);

	if (tone == HAL_TONE_CONFIRM) {
		confirm_start(l);
		return HAL_OK;
	}

	l->confirm_on = false;

	return tone_play_ms(l, tone, duration_ms);
}

static void tone_set(struct line *l, enum hal_tone tone)
{
	tone_set_ms(l, tone, 0);
}

/* the dial and confirm tones play until stopped, every other tone the
 * length of its request */
static uint32_t tone_replay_ms(const struct line *l, enum hal_tone tone)
{
	if (line_tone_is_dial(tone) || tone == HAL_TONE_CONFIRM)
		return 0;

	return l->tone_pending_ms;
}

static void tone_replay_all(void)
{
	unsigned int i;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct line *l = &lines[i];
		enum hal_tone tone;

		if (!l->tone_pending)
			continue;

		ULOG_INFO("line %u tone %u re-issued after the region load\n", i,
			  l->tone_pending);
		tone = (enum hal_tone)l->tone_pending;
		l->tone_pending = HAL_TONE_NONE;
		uloop_timeout_cancel(&l->dial_tone);
		/* the length of the request, which can be "until stopped"; the
		 * dial and confirm tones keep their own lengths */
		if (!l->confirm_on || !line_tone_is_dial(tone)) {
			signal_report(l, PROTO_SIG_TONE,
				      tone_play_ms(l, tone, tone_replay_ms(l, tone)));
			continue;
		}

		/* no profile carried the confirm tone either, so it plays now
		 * and the dial tone waits for its end again */
		confirm_start(l);
		dial_tone_set(l, tone);
	}
}

/*
 * The distinctive ring cadences of GR-506, which is what the Alert-Info
 * "Bellcore-dr<n>" token selects (shared/hdr.h). The join is 1 based, so row
 * 0 is template 1 and row 4 is template 5. It is the fallback table: a region
 * read-back that defines template n wins over it for that template, so every
 * shipped region file (all define r1 to r8) leaves it unread, and it answers
 * for the compiled default alone, which a backend with no region read-back
 * keeps. Row 0 is also the plain ring every unresolved id ends at. Row 4 is
 * the ring splash: the cadence repeats it, because the ring op has no
 * single-burst form.
 */
static const struct {
	uint8_t count;
	struct hal_cadence cad[HAL_CADENCE_MAX];
} ring_patterns[] = {
	{ 1, { { 2000, 4000 } } },
	{ 2, { { 800, 400 }, { 800, 4000 } } },
	{ 3, { { 400, 200 }, { 400, 200 }, { 800, 4000 } } },
	{ 3, { { 300, 200 }, { 1000, 200 }, { 300, 4000 } } },
	{ 1, { { 500, 4000 } } },
};

static const struct hal_region_ring *ring_pattern_get(const struct line *l,
						      unsigned int t)
{
	if (t >= HAL_REGION_RING_MAX || !l->ring_pattern[t].cadence_count)
		return NULL;

	return &l->ring_pattern[t];
}

static void ring_pattern_base(struct hal_region_ring out[HAL_REGION_RING_MAX])
{
	memcpy(out, region_hal.ring, sizeof(region_hal.ring));
}

static void ring_builtin_take(unsigned int idx, struct hal_ring *out)
{
	memcpy(out->cadence, ring_patterns[idx].cad, sizeof(out->cadence));
	out->cadence_count = ring_patterns[idx].count;
}

/*
 * The cadence a ring template id resolves to. A configured region pattern
 * wins for every id 0 to 7; an id from 1 to 5 the region left unset falls to
 * the built-in GR-506 row, which is where the dr5 splash lives; anything else
 * takes the default shape, which is pattern0 when the region carries one and
 * the plain ring when it does not. So all eight region rows are honored, and
 * ids 8 and 9 take the default.
 */
static void ring_cadence_take(const struct line *l, unsigned int t,
			      struct hal_ring *out)
{
	const struct hal_region_ring *pattern = ring_pattern_get(l, t);
	unsigned int i;

	if (!pattern && t >= 1 && t <= ARRAY_SIZE(ring_patterns)) {
		ring_builtin_take(t - 1, out);
		return;
	}

	if (!pattern)
		pattern = ring_pattern_get(l, 0);

	if (!pattern) {
		ring_builtin_take(0, out);
		return;
	}

	for (i = 0; i < pattern->cadence_count; i++)
		out->cadence[i] = pattern->cadence[i];

	out->cadence_count = pattern->cadence_count;
}

/*
 * Where the type 1 record sits relative to the bell. The two FSK families
 * send it in the silence after the first ring burst, at least 500 ms after
 * the ring ends as GR-30 requires (LINE_CID_WAIT_MS). Every alerting family
 * sends its whole sequence before the first ring instead, so there is no ring
 * break to sit behind and no wait to configure. NTT keeps the after-ring
 * form: its off-hook acknowledge handshake belongs to the DSP sequence,
 * nothing in this tree describes it further, and no shipped region selects
 * it.
 */
static bool cid_after_ring(uint8_t format)
{
	switch (format) {
	case HAL_CID_BELLCORE_FSK:
	case HAL_CID_ETSI_FSK:
	case HAL_CID_NTT:
		return true;
	default:
		return false;
	}
}

/*
 * The type 2 form of the same format. A type 2 record is FSK alone, so an
 * alerting format maps onto the family it modulates with and the DTMF class
 * has no type 2 at all. false means the alert carries no record.
 */
static bool cid_format_type2(uint8_t format, uint8_t *out)
{
	switch (format) {
	case HAL_CID_BELLCORE_FSK:
	case HAL_CID_ETSI_FSK:
	case HAL_CID_NTT:
		*out = format;
		return true;
	case HAL_CID_ETSI_RPAS:
	case HAL_CID_ETSI_DTAS:
	case HAL_CID_ETSI_LR_DTAS:
		*out = HAL_CID_ETSI_FSK;
		return true;
	default:
		return false;
	}
}

static void cid_fill(struct hal_cid *out, uint8_t format, const char *number,
		     const char *name, bool number_private, bool name_private)
{
	memset(out, 0, sizeof(*out));
	out->format = format;
	out->number_private = number_private;
	out->name_private = name_private;
	str_copy(out->number, sizeof(out->number), number);
	str_copy(out->name, sizeof(out->name), name);

	/*
	 * GR-30 tells a caller who withheld an identity from one who never had
	 * one, and the two reason letters are how a display says which. The
	 * private flags carry the first, "P"; this is the second, "O".
	 *
	 * The reason is per field, not per record. The number and the name
	 * each carry their own absence reason, so a caller who sent a number
	 * but no name gets the number and "O" in the name, and the reverse
	 * gets "O" in the number. hal_cid carries one blockinfo string, so the
	 * reason is raised whenever either field is absent and not withheld,
	 * and the backend renders it into whichever field is empty. A withheld
	 * field keeps "P": privacy wins over absence, because that caller has
	 * an identity and chose not to send it.
	 */
	if ((out->number[0] || number_private) && (out->name[0] || name_private))
		return;

	str_copy(out->blockinfo, sizeof(out->blockinfo), "O");
}

/* The record of a caller the protocol names (proto/proto.h struct
 * proto_cid). */
static void cid_of_proto(struct hal_cid *out, uint8_t format,
			 const struct proto_cid *cid)
{
	cid_fill(out, format, cid->number, cid->name, cid->number_private,
		 cid->name_private);
	str_copy(out->datetime, sizeof(out->datetime), cid->datetime);
}

/* A ring-break index, one break per cadence entry. The first cycle rides
 * first[] when it carries one, else cadence[], so its burst count is the break
 * after which the whole cycle has played. */
static uint8_t ring_first_bursts(const struct hal_ring *r)
{
	return r->first_count ? r->first_count : r->cadence_count;
}

/*
 * The bell of the request the line holds. A call ring takes the template and
 * the caller of the request. A HOLDING recall is not a new incoming call, so
 * neither a caller id nor a distinctive template rides the bell.
 */
static void ring_build(const struct line *l, struct hal_ring *out)
{
	const struct line_ring_req *rq = &l->ring_req;

	memset(out, 0, sizeof(*out));
	out->duration_ms = rq->duration_ms;

	if (rq->kind == PROTO_RING_RECALL) {
		ring_cadence_take(l, 0, out);
		return;
	}

	ring_cadence_take(l, rq->ring_template, out);
	if (cid_after_ring(l->cid_format)) {
		out->cid_at_ring = ring_first_bursts(out);
		out->cid_wait_ms = LINE_CID_WAIT_MS;
	}
	cid_of_proto(&out->cid, l->cid_format, &rq->cid);
	/* one uci bool decides whether this line announces callers at all.
	 * With it on every inbound call carries a record, so a caller who
	 * withheld the number, and one who sent none, both reach the display as
	 * a reason letter instead of as silence. */
	out->cid_present = l->chan.cid_remote_enable && rq->cid.present;
}

/* --------------------------------------------------------- message waiting */

/*
 * Where the FSK record can go: on hook, with no bell running. An off-hook line
 * has no on-hook signalling path at all, and a burst inside a ring cadence
 * collides with the bell, which needs a listen test on hardware and is not
 * something the shell can resolve.
 */
static bool mwi_at_rest(const struct line *l)
{
	return !line_flags_of(l) && !l->oos && !l->ringing;
}

static void mwi_fsk_send(struct line *l)
{
	struct hal_cid cid;

	/*
	 * The record is held, not spent, while the hardware cannot carry it.
	 * The call layer comes up before the DSP, so a NOTIFY can land on a
	 * line whose config is loaded and whose channels are not started, and
	 * this push would cross a backend that cannot take it. The switch does
	 * not send that message again, so a flag spent on a refused push would
	 * lose the indication for the life of the daemon. The deferral stands
	 * instead, and lines_start() replays it on the started DSP.
	 */
	if (!uv.hal || uv.hal_state != UVOICE_HAL_READY) {
		l->mwi.deferred = true;
		TRACE(TRACE_FSM, "line %u visual message waiting held, the dsp is not ready",
		      l->index);
		return;
	}

	l->mwi.deferred = false;

	/*
	 * A visual message waiting record announces no caller, so it is built
	 * here instead of through cid_fill(): that helper raises the GR-30
	 * absence reason for its two empty identity fields, and a display would
	 * then render "O" beside an indication nobody called about.
	 */
	memset(&cid, 0, sizeof(cid));
	cid.format = l->mwi_format;
	cid.mwi = l->mwi.on ? HAL_MWI_ON : HAL_MWI_OFF;

	TRACE(TRACE_FSM, "line %u visual message waiting %s, format %u", l->index,
	      l->mwi.on ? "on" : "off", cid.format);
	hal_backend_ops.line_cid_send(uv.hal, l->index, HAL_CID_TYPE1, &cid);
}

/*
 * A record the line cannot present now is held, never dropped: the switch
 * does not send the summary again, and a repeated one is not a transition. A
 * stored summary is presented once the indicator is enabled, at the next rest.
 * Before lines_init() the channel view is not loaded, so the vmwi gate cannot
 * be read yet.
 */
static void mwi_present(struct line *l)
{
	if (!lines_up) {
		l->mwi.deferred = true;
		return;
	}

	if (!l->chan.vmwi) {
		l->mwi.deferred = true;
		TRACE(TRACE_FSM, "line %u visual message waiting held, vmwi 0",
		      l->index);
		return;
	}

	if (!mwi_at_rest(l)) {
		l->mwi.deferred = true;
		TRACE(TRACE_FSM, "line %u visual message waiting held until the line rests",
		      l->index);
		return;
	}

	mwi_fsk_send(l);
}

/* The replay of a held presentation. It carries the same vmwi gate as the
 * first attempt, so an option switched off in between presents nothing and
 * the record stays held until the option is on again. */
static void mwi_replay(struct line *l)
{
	if (!l->mwi.deferred || !l->chan.vmwi || !mwi_at_rest(l))
		return;

	mwi_fsk_send(l);
}

/*
 * A transition is what the subscriber is presented with: the indication
 * itself moving, or a count moving while it stays on. A NOTIFY that repeats
 * what the line already holds is stored and presented to nobody, which is
 * what keeps a switch that re-sends its summary every refresh from ringing an
 * FSK burst at the phone each time.
 */
static bool mwi_moved(const struct line *l, bool on, uint32_t new_msgs, uint32_t old_msgs)
{
	if (l->mwi.on != on)
		return true;

	return on && (l->mwi.new_msgs != new_msgs || l->mwi.old_msgs != old_msgs);
}

/*
 * The store and the visual presentation, the core half of a NOTIFY. The
 * machine of the protocol learns nothing here: the protocol applies the
 * audible choice itself (proto/proto.h mwi_set).
 */
void line_mwi_set(unsigned int line, bool on, uint32_t new_msgs,
		  uint32_t old_msgs)
{
	struct line *l = line_get(line);
	bool moved;

	if (!l)
		return;

	moved = mwi_moved(l, on, new_msgs, old_msgs);
	l->mwi.on = on;
	l->mwi.new_msgs = new_msgs;
	l->mwi.old_msgs = old_msgs;

	TRACE(TRACE_FSM, "line %u message waiting %d new %u old %u%s", line, on, new_msgs,
	      old_msgs, moved ? "" : ", unchanged");

	if (moved)
		mwi_present(l);
}

bool line_mwi_get(unsigned int line, uint32_t *new_msgs, uint32_t *old_msgs)
{
	const struct line *l = line_get(line);

	if (!l)
		return false;

	*new_msgs = l->mwi.new_msgs;
	*old_msgs = l->mwi.old_msgs;

	return l->mwi.on;
}

/* ------------------------------------------------- the digit from the wire */

/*
 * The far end's own key press, played toward the phone. Two transports carry
 * it and both end here: the SIP INFO call.c parses, and the wire event a
 * backend reports as HAL_EVENT_DIGIT with remote set.
 */

static void digit_play_run(struct line *l);

/* the echo of the digit this line is playing is still in the uplink for this
 * long after the tone ends */
static bool digit_play_window_open(const struct line *l)
{
	return l->digit_play_window &&
	       uvoice_deadline_ahead(l->digit_play_until_ms, uvoice_mono_ms());
}

static void digit_q_drop_head(struct line *l)
{
	l->digit_q_head = (uint8_t)((l->digit_q_head + 1) % LINE_DIGIT_Q_MAX);
	l->digit_q_count--;
}

/*
 * The queue and its timer go together. line_timers_cancel() runs this in
 * front of the re-init memset that would corrupt the uloop list an armed
 * timer is linked into, and the IDLE edge runs it because the digits describe
 * a call this line no longer has.
 */
static void digit_queue_flush(struct line *l)
{
	uloop_timeout_cancel(&l->digit_play);
	l->digit_q_head = 0;
	l->digit_q_count = 0;
	l->digit_playing = false;
	l->digit_play_window = false;
	l->digit_net_last_valid = false;
}

static void digit_net_stamp(struct line *l, char digit)
{
	l->digit_net_last = digit;
	l->digit_net_last_ms = uvoice_mono_ms();
	l->digit_net_last_valid = true;
}

/*
 * The de-duplication of one key a peer sent on both transports. The record is
 * kept by every network digit that reaches this shell, the backend's own
 * render included, and a repeat inside the window is dropped without moving
 * the record, so a far end that really presses one key twice is heard. The
 * age of the record is an unsigned difference, so a record left from long ago
 * reads as old whatever the clock did since.
 */
static bool digit_net_repeat(struct line *l, char digit)
{
	uint32_t age_ms = uvoice_mono_ms() - l->digit_net_last_ms;

	if (l->digit_net_last_valid && l->digit_net_last == digit &&
	    age_ms < LINE_DIGIT_DEDUP_MS)
		return true;

	digit_net_stamp(l, digit);

	return false;
}

static uint16_t digit_play_clamp(uint16_t duration_ms)
{
	if (duration_ms < LINE_DIGIT_PLAY_MIN_MS)
		return LINE_DIGIT_PLAY_MIN_MS;

	return duration_ms > LINE_DIGIT_PLAY_MAX_MS ? LINE_DIGIT_PLAY_MAX_MS :
	       duration_ms;
}

/* Whether the protocol lets a far-end digit on stream play now. */
static bool digit_stream_allowed(const struct line *l, unsigned int stream)
{
	const struct proto_ops *ops = proto_ops_get();

	return ops->digit_play_allowed &&
	       ops->digit_play_allowed(l->index, stream);
}

/*
 * One digit toward the phone (proto/proto.h digit_play). The de-duplication
 * window holds for a digit the far end reported alone (dedup): a commanded
 * digit always queues.
 */
bool line_digit_play(unsigned int line, unsigned int stream, char digit,
		     uint16_t duration_ms, bool dedup)
{
	struct line *l = line_get(line);
	struct line_digit_q *e;

	if (!l || stream >= HAL_STREAM_MAX)
		return false;

	if (!digit_stream_allowed(l, stream)) {
		TRACE(TRACE_HOOK, "line %u network digit '%c' for stream %u "
		      "dropped, it faces no conversation", l->index, digit, stream);
		return false;
	}

	if (dedup && digit_net_repeat(l, digit)) {
		TRACE(TRACE_HOOK, "line %u network digit '%c' repeated inside %u ms, dropped",
		      l->index, digit, LINE_DIGIT_DEDUP_MS);
		return false;
	}

	if (l->digit_q_count >= LINE_DIGIT_Q_MAX) {
		TRACE(TRACE_HOOK, "line %u network digit '%c' dropped, the queue "
		      "is full", l->index, digit);
		return false;
	}

	e = &l->digit_q[(l->digit_q_head + l->digit_q_count) % LINE_DIGIT_Q_MAX];
	e->digit = digit;
	e->stream = (uint8_t)stream;
	e->ms = digit_play_clamp(duration_ms);
	l->digit_q_count++;

	TRACE(TRACE_HOOK, "line %u network digit '%c' queued for stream %u, %u ms",
	      l->index, digit, stream, e->ms);

	digit_play_run(l);

	return true;
}

/*
 * Drop the queued digits of stream. The digit that plays is no longer in the
 * queue (digit_play_run takes it out as it starts), so it runs to its end.
 */
void line_digit_play_cancel(unsigned int line, unsigned int stream)
{
	struct line *l = line_get(line);
	struct line_digit_q keep[LINE_DIGIT_Q_MAX];
	unsigned int i, n = 0;

	if (!l)
		return;

	for (i = 0; i < l->digit_q_count; i++) {
		const struct line_digit_q *e =
			&l->digit_q[(l->digit_q_head + i) % LINE_DIGIT_Q_MAX];

		if (e->stream != stream)
			keep[n++] = *e;
	}

	memcpy(l->digit_q, keep, n * sizeof(keep[0]));
	l->digit_q_head = 0;
	l->digit_q_count = (uint8_t)n;
}

/*
 * The timer runs twice behind the last digit of a burst: once at the gap, to
 * pace the queue, and once more at the end of the tail, to close the window.
 * The second run is what keeps the deadline from growing stale.
 */
static void digit_play_expire(struct uloop_timeout *t)
{
	struct line *l = container_of(t, struct line, digit_play);
	uint32_t now = uvoice_mono_ms();

	l->digit_playing = false;
	if (l->digit_play_window &&
	    !uvoice_deadline_ahead(l->digit_play_until_ms, now))
		l->digit_play_window = false;

	digit_play_run(l);
	if (l->digit_playing || !l->digit_play_window)
		return;

	uloop_timeout_set(&l->digit_play, (int)(l->digit_play_until_ms - now));
}

/*
 * The step that paces the queue, and the one place a queued digit reaches
 * the backend. It runs on the arrival of a digit and again on every expiry of
 * the timer it arms, and it asks the gate again for every entry: the machine
 * reaches a
 * clearing from its own timers and a swap moves a leg to hold, and neither
 * closes a frame this shell would see.
 */
static void digit_play_run(struct line *l)
{
	const struct line_digit_q *e;
	enum hal_status st;

	if (l->digit_playing)
		return;

	while (l->digit_q_count) {
		e = &l->digit_q[l->digit_q_head];
		if (digit_stream_allowed(l, e->stream))
			break;

		TRACE(TRACE_HOOK, "line %u queued digit '%c' for stream %u dropped "
		      "at its turn", l->index, e->digit, e->stream);
		digit_q_drop_head(l);
	}

	if (!l->digit_q_count)
		return;

	if (!uv.hal || !hal_backend_ops.digit_play) {
		TRACE(TRACE_HAL, "line %u plays no network digit, the backend has "
		      "no digit_play", l->index);
		digit_queue_flush(l);
		return;
	}

	e = &l->digit_q[l->digit_q_head];
	st = hal_backend_ops.digit_play(uv.hal, l->index, e->digit, e->ms);
	if (st != HAL_OK)
		TRACE(TRACE_HAL, "line %u network digit '%c' refused, status %d",
		      l->index, e->digit, st);

	/* the window swallows the echo of a tone that really sounded, so a
	 * refused playout opens none */
	if (st == HAL_OK) {
		l->digit_play_window = true;
		l->digit_play_until_ms = uvoice_mono_ms() + e->ms +
					 LINE_DIGIT_PLAY_TAIL_MS;
	}

	l->digit_playing = true;
	l->digit_play.cb = digit_play_expire;
	uloop_timeout_set(&l->digit_play, (int)(e->ms + LINE_DIGIT_PLAY_GAP_MS));
	digit_q_drop_head(l);
}

/*
 * A digit the backend heard on the wire. The stream id names the leg, because
 * the record is a leg fact; the protocol refuses a stream no leg facing the
 * phone holds, which is a delayed event from a leg a transfer replaced, so it
 * is dropped rather than played on whichever leg the line holds now.
 */
static void digit_net_wire(struct line *l, const struct hal_event *ev)
{
	if (hal_plays_wire_digit) {
		digit_net_stamp(l, ev->u.digit.digit);
		TRACE(TRACE_HOOK, "line %u remote digit '%c', the backend played it",
		      l->index, ev->u.digit.digit);
		return;
	}

	line_digit_play(l->index, ev->stream, ev->u.digit.digit,
			ev->u.digit.duration_ms, true);
}

/* No HAL means no line, and a channel section above the HAL line count names
 * a port this board does not carry. */
bool line_exists(unsigned int index)
{
	if (!uv.hal)
		return false;

	return index < hal_backend_ops.cap_get(uv.hal)->lines;
}

/*
 * The three hook thresholds /etc/config/voip drives. The backend makes the
 * flash vs on-hook decision from them, so they are pushed once per line at
 * bring-up and again only when a reload moves them; the seize threshold and
 * pulse detection have no config source and stay the hardware defaults.
 *
 * A 1-FXS board carrying a 2 channel config has a section for a port that does
 * not exist, and the backend refuses the op for it, so the push asks only for
 * the lines the HAL reports.
 */
/*
 * The effective window of every line: the region's own, the same for each
 * line of the board. The push below and the forward disconnect band check
 * both read it here, so neither can drift from the numbers the backend
 * classifies with.
 */
static void hook_timing_get(struct hal_hook_timing *ts)
{
	/* The effective release floor, not the configured one. A release
	 * window that sits below the flash window can never fire, because the
	 * release would have to fire inside a flash. So the release window is
	 * raised to the flash window once, in front of both readers, and both
	 * read the same window. The whole structure is assigned, so a
	 * member added to it later cannot reach the backend as a stack
	 * leftover. */
	uint32_t flash_max = region_hal.hook_flash_max_ms;
	uint32_t release_min = region_hal.hook_release_min_ms;

	*ts = (struct hal_hook_timing){
		.flash_min_ms = region_hal.hook_flash_min_ms,
		.flash_max_ms = flash_max,
		.release_min_ms = release_min < flash_max ? flash_max :
				  release_min,
	};
}

static void hook_timing_push(struct line *l)
{
	struct hal_hook_timing ts;
	enum hal_status st;

	if (!line_exists(l->index))
		return;

	/* A backend that programs the windows from its own region data
	 * (HAL_FEAT_REGION_HOOK) already holds the ones this would carry, and a
	 * push would only latch them against the next region swap. */
	if (uv.hal &&
	    (hal_backend_ops.cap_get(uv.hal)->features & HAL_FEAT_REGION_HOOK))
		return;

	hook_timing_get(&ts);

	/* the guard is the last window the backend ACCEPTED, not the staged
	 * view: a failed push stays owed and retries at the next apply, and a
	 * region read-back that moved only the base is a real move even when
	 * the channel copy did not change */
	if (l->hook_applied_valid && !memcmp(&ts, &l->hook_applied, sizeof(ts)))
		return;

	TRACE(TRACE_HOOK, "line %u hook timing flash %u..%u release %u", l->index,
	      ts.flash_min_ms, ts.flash_max_ms, ts.release_min_ms);

	st = hal_backend_ops.line_hook_timing_set(uv.hal, l->index, &ts);

	/*
	 * hal.h section 3: HAL_TRANSIENT means retry once. The retry runs here,
	 * in this frame, for the same reason detector_arm() retries in its own:
	 * the op is one short call, and the shell must not sleep on the uloop
	 * thread.
	 */
	if (st == HAL_TRANSIENT)
		st = hal_backend_ops.line_hook_timing_set(uv.hal, l->index, &ts);

	if (st == HAL_OK) {
		l->hook_applied = ts;
		l->hook_applied_valid = true;
		return;
	}

	/* the hardware defaults still classify hook events, so the line keeps
	 * running with the thresholds it already holds */
	ULOG_WARN("line %u hook timing not applied: %s\n", l->index,
		  hal_status_name(st));
}

/*
 * The core half of a staged view arrives here, at a rest state of the
 * protocol (proto/proto.h line_cfg_landed): the channel copy, the two caller id
 * formats and the ring patterns, together, so a ring in progress keeps what
 * it started with. The type 2 record goes with the old view, and its
 * suppression trace is armed again.
 */
static void line_chan_land(struct line *l)
{
	if (l->chan_staged) {
		l->chan = l->chan_new;
		l->cid_format = l->cid_format_new;
		l->mwi_format = l->mwi_format_new;
		memcpy(l->ring_pattern, l->ring_pattern_new,
		       sizeof(l->ring_pattern));
		l->chan_staged = false;
	}

	/* the push rides the applied copy and guards on the last accepted
	 * window, so a reload cannot retune the hook window under a flash the
	 * subscriber is already holding, and an unmoved window costs no op */
	hook_timing_push(l);

	cw_cid_drop(l);
	l->cw_cid_warned = false;
}

/*
 * A view the final capability landed (lines_start()) lands its core half
 * behind the presentation of the line, with nothing staged too: the hook
 * window push the backend refused at the first derive is retried there.
 */
void line_cfg_landed(unsigned int line)
{
	struct line *l = line_get(line);

	if (!l)
		return;

	if (lines_cap_final_push) {
		l->start_settle = true;
		return;
	}

	if (!l->chan_staged)
		return;

	line_chan_land(l);
}

/*
 * One move of the protocol's machine (proto/proto.h line_transition). A pending
 * tone replay takes the state that asked for it. At rest, the core drops what
 * it held for the call that ended: the queued type 2 record and the flag of
 * its suppression trace, so the next call says it once more instead of
 * staying quiet for the life of the daemon; the queued digits toward the
 * phone, which belong to a call this line no longer has; and the tone
 * disabler's view. That view is the one entry here that mirrors a backend
 * rather than this shell: the DSP drops its latch once the last stream of the
 * line stops and sends no record for that drop (udsp/src/core/stream.c
 * stream_lec_release), so a view held past this edge would report a hold the
 * canceller no longer has.
 */
void line_transition(unsigned int line, unsigned int state_id, uint32_t flags)
{
	struct line *l = line_get(line);

	if (!l)
		return;

	tone_pending_settle(l, state_id);

	/* every rest, the out-of-service one included: a teardown passes the
	 * rest on its way out, and the three drops are idempotent */
	if (flags)
		return;

	cw_cid_drop(l);
	digit_queue_flush(l);
	l->cw_cid_warned = false;
	uvoice_lec_policy_clear(l->index);
}

/*
 * The end of a drain of the protocol (proto/proto.h line_settle), behind its
 * config apply and its own forward disconnect settle. The held presentation is
 * made here. A reload staged while the line was busy lands in that apply, and a
 * replay that ran before it would read the old channel view: a reload that
 * turned vmwi off would still put one more record on the line it was just
 * switched off for. Every path that brings a line to rest ends here, so this
 * one call covers the on-hook edge, the rest entry and the return from out
 * of service. It is idempotent, because mwi_fsk_send() clears the flag it
 * tests. A core copy that is still staged waits for its landing, which ends
 * in this settle again.
 */
void line_settle(unsigned int line)
{
	struct line *l = line_get(line);

	if (!l || l->chan_staged)
		return;

	mwi_replay(l);
}

/*
 * A hardware reason or a refused detector arm takes the line out with the
 * fault reason: the protocol ends the legs through its machine alone, with no
 * leg teardown in front of it (proto/proto.h enum proto_service_reason).
 */
static void line_oos_take(struct line *l)
{
	l->oos = true;
	proto_ops_get()->line_service(l->index, false, PROTO_SERVICE_FAULT);
}

static void ring_retry_expire(struct uloop_timeout *t);

static void ring_retry_arm(struct line *l, unsigned int ms)
{
	l->ring_retry.cb = ring_retry_expire;
	uloop_timeout_set(&l->ring_retry, (int)ms);
}

/*
 * The bell is lost for this call, so the caller loses the ringback with it.
 * The verdict reaches the machine on the next pass and not here.
 * bell_start() runs in front of ringing_enter(), so the machine is not in
 * FSM_RINGING inside this frame. A refusal applied here would find nothing to
 * refuse.
 */
static void ring_give_up(struct line *l)
{
	l->ring_failed = true;
	ring_retry_arm(l, 0);
}

/*
 * Which attempt at the bell this is, because the three answer a refusal
 * differently. RING_TRY_FIRST is the attempt the machine asked for.
 * RING_TRY_RETRY is the single repeat of a transient refusal. RING_TRY_HELD is
 * the re-issue of a ring a line test held.
 */
enum ring_try {
	RING_TRY_FIRST = 0,
	RING_TRY_RETRY,
	RING_TRY_HELD,
};

/*
 * One attempt at the bell, and what the backend's answer means.
 *
 * libudsp answers HAL_TRANSIENT to a ring on any line while a metallic line
 * test runs. The thread that executes a ring is inside the measurement. This
 * shell holds the ring instead. The line still reads as ringing and the caller
 * hears ringback. The end of the test issues the ring again, and a GR-909 set
 * delays the bell by about 10 s.
 *
 * A re-issue the backend still refuses keeps the hold and asks again every
 * LINE_RING_HOLD_RETRY_MS. The core gives the test job a budget, and a plug-in
 * that caps each measurement separately can outlast it, so the end of the job
 * does not prove the part is free. The line rings or the call ends; nothing
 * else stops the attempts.
 *
 * A transient answer with no test in flight is the ordinary busy answer of
 * hal.h section 3. The shell waits HAL_RETRY_MS and asks once more, and a
 * second refusal there gives the bell up. A fatal or unsupported answer gives
 * it up from the first attempt. Either way the subscriber loses the call, so
 * the give-up is logged and not traced. The log names the outcome of the
 * profile it belongs to: a caller gets 480, and a recall stays silent, because
 * it has no caller to refuse.
 */
static void ring_issue(struct line *l, enum ring_try try)
{
	bool recall = l->ring_req.kind == PROTO_RING_RECALL;
	bool held = l->ring_held;
	const struct proto_ops *ops = proto_ops_get();
	struct hal_ring ring;
	enum hal_status st;
	unsigned int tested;

	ring_build(l, &ring);

	l->ringing = true;
	l->ring_held = false;
	TRACE(TRACE_FSM, "line %u ring %s template %u cadences %u cid %d dur %u",
	      l->index, recall ? "recall" : "call", l->ring_req.ring_template,
	      ring.cadence_count, ring.cid_present, ring.duration_ms);

	if (!uv.hal)
		return;

	st = hal_backend_ops.line_ring_start(uv.hal, l->index, &ring);
	if (st == HAL_OK) {
		/* the no-answer window measures a phone the subscriber can
		 * hear, so a bell the hold delayed restarts it (fsm.h) */
		if (held && ops->ring_started)
			ops->ring_started(l->index);
		return;
	}

	if (st != HAL_TRANSIENT || try == RING_TRY_RETRY) {
		ULOG_ERR("line %u ring not started: %s, %s\n", l->index,
			 hal_status_name(st),
			 recall ? "the recall stays silent" :
				  "the caller gets 480");
		ring_give_up(l);
		return;
	}

	if (try == RING_TRY_HELD) {
		l->ring_held = true;
		if (ops->ring_held)
			ops->ring_held(l->index);
		ring_retry_arm(l, LINE_RING_HOLD_RETRY_MS);
		TRACE(TRACE_FSM, "line %u ring still held, asking again in "
		      "%u ms", l->index, LINE_RING_HOLD_RETRY_MS);
		return;
	}

	if (line_test_running(&tested)) {
		l->ring_held = true;
		/* the no-answer window must not run over a silent telephone,
		 * and a hold longer than that window would forward the call
		 * before the bell ever sounded (fsm.h) */
		if (ops->ring_held)
			ops->ring_held(l->index);
		TRACE(TRACE_FSM, "line %u ring held by the line test on line "
		      "%u", l->index, tested);
		return;
	}

	ring_retry_arm(l, HAL_RETRY_MS);
	TRACE(TRACE_FSM, "line %u ring refused: %s, asking again in %u ms",
	      l->index, hal_status_name(st), HAL_RETRY_MS);
}

/*
 * The refusal of the pass before, handed to the protocol. The flag goes first,
 * because ring_retry is one timer with three jobs. A flag that survived an
 * early return below would take the next transient retry, and the bell would
 * never ring again on that line.
 *
 * A recall ring refuses nobody. No caller waits for it, and a reject would end
 * the held call of the subscriber.
 */
static void ring_fail_apply(struct line *l)
{
	const struct proto_ops *ops = proto_ops_get();

	l->ring_failed = false;

	if (!l->ringing)
		return;
	if (l->ring_req.kind == PROTO_RING_RECALL)
		return;

	if (ops->ring_failed)
		ops->ring_failed(l->index);
}

/* the give-up owns the same timer, and the hold owns the slower cadence, so
 * the two flags choose which job this expiry is */
static void ring_retry_expire(struct uloop_timeout *t)
{
	struct line *l = container_of(t, struct line, ring_retry);
	enum ring_try try;

	if (l->ring_failed) {
		ring_fail_apply(l);
		return;
	}

	try = l->ring_held ? RING_TRY_HELD : RING_TRY_RETRY;
	ring_issue(l, try);
}

/*
 * The bells a line test held. The last test to finish rings them.
 *
 * This builds the profile again from the request the ring belongs to. The
 * cadence starts from its beginning and the profile carries the full
 * duration, so the subscriber hears a whole ring and not the tail of one.
 *
 * A line whose call ended in the meantime rings nothing: line_ringer_stop()
 * cleared the hold, so a caller who gave up does not ring the phone late.
 */
static void ring_hold_replay(void)
{
	unsigned int i, tested;

	/* a second test still owns the part, so the hold outlives this one */
	if (line_test_running(&tested))
		return;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct line *l = &lines[i];

		if (!l->ring_held || !l->ringing)
			continue;

		TRACE(TRACE_FSM, "line %u ring re-issued after the line test",
		      i);
		ring_issue(l, RING_TRY_HELD);
	}
}

/*
 * The bell of one request (proto/proto.h ring_start). A new request drops the
 * record of the old one, the rule line_ringer_stop() below follows: this bell
 * is the one the line owes now.
 *
 * A reversal can outlive the clearing that made it, and the ring starts inside
 * the event that asks for it, before the protocol's own settle runs: the
 * on-hook that parks a held leg out of a clearing is the common case. So the
 * reversal is put back here first. Otherwise the restore would land inside
 * the ring cadence (fwd_disc_blocked()).
 */
void line_ringer_start(unsigned int line, enum proto_ring_kind kind,
		     unsigned int ring_template, uint32_t duration_ms,
		     const struct proto_cid *cid)
{
	struct line *l = line_get(line);

	if (!l)
		return;

	fwd_disc_abort(l, false);
	uloop_timeout_cancel(&l->ring_retry);
	l->ring_failed = false;

	memset(&l->ring_req, 0, sizeof(l->ring_req));
	l->ring_req.kind = (uint8_t)kind;
	l->ring_req.ring_template = (uint8_t)ring_template;
	l->ring_req.duration_ms = duration_ms;
	if (cid)
		l->ring_req.cid = *cid;

	ring_issue(l, RING_TRY_FIRST);
}

void line_ringer_stop(unsigned int line)
{
	struct line *l = line_get(line);

	if (!l)
		return;

	l->ringing = false;
	l->ring_held = false;
	l->ring_failed = false;
	uloop_timeout_cancel(&l->ring_retry);

	if (uv.hal)
		hal_backend_ops.line_ring_stop(uv.hal, l->index);
}

/*
 * Whether the subscriber alerting signal carries a type 2 record, and in which
 * format. Two gates gone through in order: cid_remote_enable, which is the
 * one uci bool that drives the ring CID and both call waiting paths, and the
 * type 2 format map, which has no DTMF form. A suppressed format still gets
 * the alert tone; only the record goes, and the trace says so once per call
 * rather than once per push.
 */
static bool cw_cid_wanted(struct line *l, uint8_t *format)
{
	if (!l->chan.cid_remote_enable)
		return false;

	if (cid_format_type2(l->cid_format, format))
		return true;

	if (!l->cw_cid_warned) {
		l->cw_cid_warned = true;
		TRACE(TRACE_FSM,
		      "line %u cid format %u has no type 2 form, alerting without a record",
		      l->index, l->cid_format);
	}

	return false;
}

/*
 * Play the call-waiting SAS for the resolved variant. variant 0, or a backend
 * without the variant op, takes the plain tone_set path, which keeps the
 * dial-tone cancel and the region-load replay. A distinctive variant goes
 * straight to the backend, cancelling the dial-tone timer and clearing the
 * replay slot and the confirm record the same way tone_set would.
 */
static void cw_tone_set(struct line *l, unsigned int variant)
{
	if (!variant || !uv.hal || !hal_backend_ops.cw_tone_play) {
		tone_set(l, HAL_TONE_CALL_WAITING);
		return;
	}

	uloop_timeout_cancel(&l->dial_tone);
	tone_pending_take(l, HAL_TONE_CALL_WAITING, 0);
	l->confirm_on = false;
	hal_backend_ops.cw_tone_play(uv.hal, l->index, variant, 0);
}

/*
 * The subscriber alerting signal of a waiting call (proto/proto.h cw_alert):
 * the CALL_WAITING tone toward the phone, which is the host-played SAS of the
 * variant, and the type 2 record of the caller behind it (line/line.h
 * "Duties"). The tone stops with the alert; its own cadenced profile carries
 * the GR-506 re-presentation, and the record is pushed once.
 */
void line_cw_alert(unsigned int line, bool on, unsigned int variant,
		   const struct proto_cid *cid)
{
	struct line *l = line_get(line);
	struct hal_cid rec;
	uint8_t format;

	if (!l)
		return;

	cw_cid_drop(l);

	if (!on) {
		tone_set(l, HAL_TONE_NONE);
		return;
	}

	cw_tone_set(l, variant);
	if (!cid || !cid->present || !cw_cid_wanted(l, &format))
		return;

	cid_of_proto(&rec, format, cid);
	cw_cid_defer(l, &rec, cw_cid_lead_ms(variant));
}

/*
 * A caller id record alone (proto/proto.h cid_send), in the format of the line:
 * the region format for a type 1 record and its type 2 form for a type 2 one.
 * The gate rule of struct proto_cid holds: cid_remote_enable gates the record.
 */
enum hal_status line_cid_send(unsigned int line, enum hal_cid_kind kind,
			      const struct proto_cid *cid)
{
	struct line *l = line_get(line);
	struct hal_cid rec;
	uint8_t format = 0;

	if (!l || !uv.hal)
		return HAL_FATAL;
	if (!cid || !cid->present || !l->chan.cid_remote_enable)
		return HAL_OK;

	if (kind == HAL_CID_TYPE2) {
		if (!cid_format_type2(l->cid_format, &format))
			return HAL_UNSUPPORTED;
	} else {
		format = l->cid_format;
	}

	cid_of_proto(&rec, format, cid);

	return hal_backend_ops.line_cid_send(uv.hal, l->index, kind, &rec);
}

/*
 * The mixer of the local three-way conference (fsm.h conf_set, CONF). It
 * names no slot: the conference is the whole line, so the backend mixes the
 * two legs this line owns and the phone.
 *
 * hal.h section 3: HAL_TRANSIENT means retry once. The retry runs in this
 * frame, which is the idiom detector_arm() and hook_timing_push() already
 * use: the op is one short call and the shell must not sleep on the uloop
 * thread. The gesture behind it does not come again, so a transient refusal
 * dropped instead of retried costs the subscriber the conference outright.
 */
enum hal_status line_conf_set(unsigned int line, bool on)
{
	enum hal_conf_mode mode = on ? HAL_CONF_LOCAL_MIX : HAL_CONF_OFF;
	enum hal_status st;

	if (!uv.hal || !line_get(line))
		return HAL_FATAL;

	st = hal_backend_ops.conf_set(uv.hal, line, mode);
	if (st == HAL_TRANSIENT)
		st = hal_backend_ops.conf_set(uv.hal, line, mode);

	return st;
}

enum hal_status line_audio_path_set(enum hal_audio_path path)
{
	if (!uv.hal || !hal_backend_ops.audio_path_set)
		return HAL_UNSUPPORTED;

	return hal_backend_ops.audio_path_set(uv.hal, path);
}

enum hal_status line_volume_set(unsigned int level)
{
	if (!uv.hal || !hal_backend_ops.volume_set)
		return HAL_UNSUPPORTED;

	return hal_backend_ops.volume_set(uv.hal, level);
}

enum hal_status line_mute_set(bool mute)
{
	if (!uv.hal || !hal_backend_ops.mute_set)
		return HAL_UNSUPPORTED;

	return hal_backend_ops.mute_set(uv.hal, mute);
}

enum hal_status line_led_set(enum hal_led led, enum hal_led_mode mode)
{
	if (!uv.hal || !hal_backend_ops.led_set)
		return HAL_UNSUPPORTED;

	return hal_backend_ops.led_set(uv.hal, led, mode);
}

enum hal_status line_audio_tune_set(const struct hal_audio_tune *tune)
{
	if (tune->ring_sound >= __HAL_RING_SOUND_MAX)
		return HAL_UNSUPPORTED;

	line_tune = *tune;
	if (!uv.hal || !hal_backend_ops.audio_tune_set)
		return HAL_UNSUPPORTED;

	return hal_backend_ops.audio_tune_set(uv.hal, &line_tune);
}

void line_audio_tune_get(struct hal_audio_tune *tune)
{
	*tune = line_tune;
}

static void audio_tune_take(const struct cfg *cfg)
{
	const struct hal_audio_tune tune = {
		.handset_mic_gain = cfg->hal.handset_mic_gain,
		.headset_mic_gain = cfg->hal.headset_mic_gain,
		.speaker_mic_gain = cfg->hal.speaker_mic_gain,
		.handset_out_gain = cfg->hal.handset_out_gain,
		.headset_out_gain = cfg->hal.headset_out_gain,
		.ring_sound = (enum hal_ring_sound)cfg->hal.ring_sound,
	};

	line_audio_tune_set(&tune);
}

enum hal_status line_key_tone_play(unsigned int line, char digit,
				   uint32_t duration_ms)
{
	if (!uv.hal || !hal_backend_ops.digit_play)
		return HAL_UNSUPPORTED;

	return hal_backend_ops.digit_play(uv.hal, line, digit, duration_ms);
}

/*
 * The ring template the caller's number selects. The comparison is
 * full-string exact, the first configured slot that matches wins, and a slot
 * participates only when it carries both a number and a definition. The
 * number is the one the protocol parsed even when the caller withheld it,
 * because presentation gates the display and not the match.
 *
 * It is also the whole parsed number, not the copy the display record
 * carries: that copy is cut to the HAL field, and a long number cut to the
 * length of a slot would then compare equal to it and ring a template its
 * caller never selected.
 */
unsigned int line_ring_template_match(unsigned int line, const char *number)
{
	const struct line *l = line_get(line);
	const struct cfg_channel *ch;
	unsigned int i;

	if (!l)
		return 0;

	ch = &l->chan;
	if (!ch->distinctive_ring_enable || !number)
		return 0;

	for (i = 0; i < CFG_DISTINCTIVE_RING_MAX; i++) {
		if (!ch->distinctive_ring[i][0] || !ch->distinctive_ring_def_set[i])
			continue;
		if (strcmp(ch->distinctive_ring[i], number))
			continue;

		return ch->distinctive_ring_def[i];
	}

	return 0;
}

/* -------------------------------------------------------------- HAL events */

/*
 * The whole channel view, not only the mask. hal.h documents
 * channel_config_set as authoritative for the gains, the detector mask and the
 * echo canceller together, so a zeroed member is not "leave it alone", it is
 * "switch it off": the backend writes every field it is given. This is the one
 * place the view is derived, and it derives it from the live channel copy, so
 * a reload lands with the rest of the staged view and reaches the DSP at the
 * next seize instead of under a call.
 */
static void channel_cfg_build(const struct line *l, struct hal_channel_cfg *out)
{
	const struct cfg_channel *ch = &l->chan;
	struct hal_channel_cfg cfg = {
		/*
		 * The modem detector is always on. It stays armed for the life
		 * of the channel, so nothing has to arm it at the moment a fax
		 * starts, and the policy that decides which reported tone
		 * switches the media lives in tone_event() below rather than in
		 * this mask. An arm at answer would need a per-call push, and
		 * this view has no moment for one.
		 */
		.detect_mask = HAL_DETECT_DTMF | HAL_DETECT_PULSE | HAL_DETECT_MODEM,
		/* hal.h: 0 asks for the backend default tail. No config
		 * option carries a tail length. */
		.ec_tail_ms = 0,
	};

	cfg_channel_lec_effective(ch, &cfg.ec_enable, &cfg.ec_nlp_enable);

	/* The answer-tone disabler stands outside that gate: it runs under the
	 * daemon policy as well, and these two only pick its release rule and
	 * its trigger mapping (hal.h, hal_channel_cfg). */
	cfg.ec_tone_latch = ch->lec_tone_latch;
	cfg.ec_tone_force_disable = ch->lec_tone_force_disable;

	/*
	 * The half decibel loss plan is reversed: half_db_rx_loss sets gain_tx
	 * and half_db_tx_loss sets gain_rx, because the two options name the
	 * direction opposite to the DSP pair. gain_tx is the gain toward the
	 * interface, so what the subscriber hears is a step off gain_tx, and
	 * gain_rx is the gain from the interface, which is what the subscriber
	 * sends (hal.h). One step is 0.5 dB.
	 */
	if (ch->half_db_rx_loss)
		cfg.gain_tx = -1;
	if (ch->half_db_tx_loss)
		cfg.gain_rx = -1;

	/*
	 * The analog pair is not reversed. ps_tx_gain is the SLIC transmit
	 * path, the subscriber-to-network direction, so tx stays tx here; the
	 * reversal above belongs to the DSP pair only. The uci pair is whole
	 * dB and the HAL pair is 0.5 dB steps, hence times two.
	 */
	cfg.slic_gain_tx = (int8_t)(ch->ps_tx_gain * 2);
	cfg.slic_gain_rx = (int8_t)(ch->ps_rx_gain * 2);

	*out = cfg;
}

/* The canceller half of two channel views, member by member. memcmp cannot
 * serve here. struct hal_channel_cfg carries padding, and padding a compiler
 * left uninitialized would make two equal views differ. */
static bool lec_cfg_same(const struct hal_channel_cfg *a,
			 const struct hal_channel_cfg *b)
{
	return a->ec_enable == b->ec_enable &&
	       a->ec_nlp_enable == b->ec_nlp_enable &&
	       a->ec_tail_ms == b->ec_tail_ms &&
	       a->ec_tone_latch == b->ec_tone_latch &&
	       a->ec_tone_force_disable == b->ec_tone_force_disable;
}

/*
 * The two canceller layers of one line, for the lec row of "voice status"
 * (line/line.h, struct line_lec_view).
 *
 * config is the live channel copy and not uv.cfg. A reload stages chan_new and
 * applies it at the next IDLE, so the loaded config can name options the DSP
 * has not been sent. The options travel unresolved, because the row prints
 * them beside the resolved pair the push carried. That pair alone cannot say
 * that lec_control 0 is what made the two differ.
 *
 * applied is the record detector_arm() left, so the row reports the push that
 * happened. pending compares a fresh view against that record. It is the state
 * a reload reaches between the config apply and the next seize.
 *
 * The copy is gated on a line that is up and that this board carries. lines[]
 * holds a slot for every compiled channel, so a zeroed slot would otherwise be
 * read as a configuration, before lines_init(), after lines_done() and for a
 * channel section above the backend line count.
 */
void line_lec_view_get(unsigned int line, struct line_lec_view *out)
{
	const struct line *l = lines_up && line_exists(line) ?
			       line_get(line) : NULL;
	struct hal_channel_cfg now;

	if (!out)
		return;

	memset(out, 0, sizeof(*out));
	/* a line this board does not carry runs the daemon policy, which is
	 * what the backend bring-up programs for it */
	out->config.enable = true;
	if (!l)
		return;

	out->config.control = l->chan.lec_control;
	out->config.enable = l->chan.lec_enable;
	out->config.nlp_set = l->chan.lec_nlp_set;
	out->config.nlp = l->chan.lec_nlp;
	out->config.tone_latch = l->chan.lec_tone_latch;
	out->config.tone_force_disable = l->chan.lec_tone_force_disable;

	if (!l->chan_applied_valid)
		return;

	channel_cfg_build(l, &now);
	out->applied.valid = true;
	out->applied.ec = l->chan_applied.ec_enable;
	out->applied.nlp = l->chan_applied.ec_nlp_enable;
	out->applied.tone_latch = l->chan_applied.ec_tone_latch;
	out->applied.tone_force_disable = l->chan_applied.ec_tone_force_disable;
	out->applied.stamp_ms = l->chan_applied_ms;
	out->applied.status = l->chan_applied_st;
	out->applied.pending = !lec_cfg_same(&now, &l->chan_applied);
}

/*
 * The detect mask must be live before the dial tone the OFFHOOK_DIAL entry
 * requests, so the first digit is never lost. The push carries the
 * whole configured view with it (channel_cfg_build above).
 *
 * The first seize is when the config reaches the DSP. The backend bring-up
 * programs its own policy defaults for the recovery window before any config
 * is loaded, and this is the first call that replaces them, so a line whose
 * config turns the canceller off runs the bring-up default until its first
 * off-hook. No call runs in that window.
 */
static bool detector_push(struct line *l, uint32_t detect_mask)
{
	struct hal_channel_cfg cfg;
	enum hal_status st;

	if (!uv.hal)
		return false;

	channel_cfg_build(l, &cfg);
	cfg.detect_mask = detect_mask;
	st = hal_backend_ops.channel_config_set(uv.hal, l->index, &cfg);

	/*
	 * hal.h section 3: HAL_TRANSIENT means retry once. The retry runs in
	 * this frame rather than 50 ms later, because the seize event must not
	 * be reordered behind a later hook report and the shell must not sleep
	 * on the uloop thread. The op is one short call, so a second attempt
	 * here is the only form that keeps the event order.
	 */
	if (st == HAL_TRANSIENT)
		st = hal_backend_ops.channel_config_set(uv.hal, l->index, &cfg);

	/* The record the lec row reads, kept whatever the answer was. A push
	 * the backend refused still describes what the DSP was last asked for.
	 * The status beside it is what says the ask did not land. */
	l->chan_applied = cfg;
	l->chan_applied_ms = uvoice_mono_ms();
	l->chan_applied_st = st;
	l->chan_applied_valid = true;

	if (st == HAL_OK)
		return true;

	ULOG_ERR("line %u digit detector did not arm: %s\n", l->index,
		 hal_status_name(st));

	return false;
}

/* The seize view: the configured view with the mask the shell keeps on. */
static bool detector_arm(struct line *l)
{
	struct hal_channel_cfg cfg;

	channel_cfg_build(l, &cfg);

	return detector_push(l, cfg.detect_mask);
}

bool line_detector_arm(unsigned int line, uint32_t detect_mask)
{
	struct line *l = line_get(line);

	return l && detector_push(l, detect_mask);
}

/* One HAL event of the line after the intake, handed to the protocol. */
static void line_event_forward(struct line *l, const struct proto_line_event *ev)
{
	proto_ops_get()->line_event(l->index, ev);
}

static void hook_pulse(struct line *l, unsigned int pulses, bool invert)
{
	struct proto_line_event ev = { .type = PROTO_EV_DIGIT };

	if (pulses < 1 || pulses > 10) {
		TRACE(TRACE_HOOK, "line %u pulse count %u out of range", l->index, pulses);
		return;
	}

	/* Under an inverting region (New Zealand) a dialed N arrives as 10 - N
	 * pulses. The event carries the coding of the region that counted the
	 * train. region_hal can already hold the next region, because a region
	 * swap can complete before a queued train arrives. */
	if (invert && pulses < 10)
		pulses = 10 - pulses;

	ev.u.digit.digit = pulses == 10 ? '0' : (char)('0' + pulses);
	ev.u.digit.pulse = true;
	line_event_forward(l, &ev);
}

/* --------------------------------------------------------- the loop test */

/*
 * The manufacturing loop test. It rings every port the board carries while
 * the handset is down, and on an off-hook edge it plays the subscriber his
 * own voice a fixed delay later, or the capture of the other port on a
 * two-port board. The audio path is the backend's own (hal.h line_loop_set):
 * no packet and no stream is involved.
 *
 * It adds no FSM state. The mode takes every line out of service through
 * line_serve_want() below, and this controller consumes the hook edges of a
 * held port itself, so the OOS machine never plays its seize reorder at a
 * subscriber who lifted the handset for the test. detector_arm() does not run
 * either, so the channel record and the lec row keep what the line held
 * before the arm.
 *
 * The ring window belongs to the backend: hal_ring.duration_ms carries it and
 * the backend ends the bell on it, so this daemon paces no ring timer.
 */

/* The ports the mode holds: what the board carries, bounded by the config
 * array this shell keeps one machine in. */
static unsigned int loop_test_line_count(void)
{
	unsigned int count;

	if (!uv.hal)
		return 0;

	count = hal_backend_ops.cap_get(uv.hal)->lines;

	return count < CFG_CHANNEL_MAX ? count : CFG_CHANNEL_MAX;
}

/*
 * SELF when the operator asked for it, and on a board with one port, which
 * has no other port to cross to. The peer of a two-port board is the other
 * port.
 */
static enum hal_loop_mode loop_test_mode_of(const struct line *l,
					    unsigned int *peer)
{
	*peer = 0;

	if (loop_test_want_self || loop_test_line_count() < 2)
		return HAL_LOOP_SELF;

	*peer = l->index ^ 1u;

	return HAL_LOOP_PEER;
}

static void loop_test_ring_retry_expire(struct uloop_timeout *t);
static void loop_test_audio_retry_expire(struct uloop_timeout *t);
static bool loop_test_audio_want(const struct line *l);

/*
 * One attempt at the bell of the mode. It carries the default cadence
 * template and no caller identity, because nobody is calling, and the ring
 * window the arm asked for.
 *
 * A transient refusal is asked once more after HAL_RETRY_MS, the rule of
 * hal.h section 3. A second refusal leaves the port quiet, and the next hook
 * edge enters this path again.
 *
 * The timer is the ring retry of the protocol's ring, which is free while the
 * mode holds the line: ring_issue() is reached from line_ringer_start(),
 * ring_retry_expire() and ring_hold_replay(), all of which need a ring the
 * out of service machine never asks for, and the force that took the line
 * out reached line_ringer_stop(), which cancelled it. Every arm below writes
 * its own callback first, and line_timers_cancel() cancels it at the end of
 * the line's life.
 */
static void loop_test_ring_issue(struct line *l, bool retry)
{
	struct hal_ring ring;
	enum hal_status st;

	if (!uv.hal)
		return;

	memset(&ring, 0, sizeof(ring));
	ring_cadence_take(l, 0, &ring);
	ring.duration_ms = loop_test_ring_ms;

	st = hal_backend_ops.line_ring_start(uv.hal, l->index, &ring);
	if (st == HAL_OK) {
		l->loop_test_ringing = true;
		TRACE(TRACE_FSM, "line %u loop test ring for %u ms", l->index,
		      ring.duration_ms);
		return;
	}

	if (st == HAL_TRANSIENT && !retry) {
		l->ring_retry.cb = loop_test_ring_retry_expire;
		uloop_timeout_set(&l->ring_retry, HAL_RETRY_MS);
		TRACE(TRACE_FSM, "line %u loop test ring refused: %s, asking "
		      "again in %u ms", l->index, hal_status_name(st),
		      HAL_RETRY_MS);
		return;
	}

	ULOG_ERR("line %u loop test ring not started: %s, the port stays "
		 "quiet\n", l->index, hal_status_name(st));
}

static void loop_test_ring_start(struct line *l)
{
	uloop_timeout_cancel(&l->ring_retry);
	loop_test_ring_issue(l, false);
}

static void loop_test_ring_retry_expire(struct uloop_timeout *t)
{
	struct line *l = container_of(t, struct line, ring_retry);

	loop_test_ring_issue(l, true);
}

static void loop_test_ring_stop(struct line *l)
{
	uloop_timeout_cancel(&l->ring_retry);
	l->loop_test_ringing = false;

	if (uv.hal)
		hal_backend_ops.line_ring_stop(uv.hal, l->index);
}

/*
 * One arm or one clear of the audio path, and what the backend answered. The
 * delay belongs to the self loop alone: a cross connection is a conversation
 * between two ports, and a delay in it would be an echo the other subscriber
 * hears.
 *
 * An arm the backend refuses transiently is asked once more, on the timer the
 * ring uses and for the same reason.
 *
 * A clear is never retried here, and its answer is what the caller reads
 * instead. A transient clear is the one answer that can leave the loop
 * standing in the backend, so the port keeps its record: the mode holds it
 * and the operator repeats the disarm (line_loop_test_set). A fatal or
 * unsupported clear describes a port that plays nothing at all, a dead line
 * or a backend with no loop, so the record goes.
 */
static enum hal_status loop_test_audio_issue(struct line *l, bool on,
					     bool retry)
{
	unsigned int peer = 0;
	struct hal_loop loop;
	enum hal_status st;

	if (!uv.hal || !hal_backend_ops.line_loop_set)
		return HAL_OK;

	memset(&loop, 0, sizeof(loop));
	if (on) {
		loop.mode = (uint8_t)loop_test_mode_of(l, &peer);
		loop.peer = (uint8_t)peer;
		if (loop.mode == HAL_LOOP_SELF)
			loop.delay_ms = (uint16_t)loop_test_delay_ms;
	}

	st = hal_backend_ops.line_loop_set(uv.hal, l->index, &loop);
	TRACE(TRACE_FSM, "line %u loop test audio mode %u peer %u delay %u: %s",
	      l->index, loop.mode, loop.peer, loop.delay_ms,
	      hal_status_name(st));

	if (st == HAL_OK) {
		l->loop_test_audio = on;
		return st;
	}

	if (!on) {
		ULOG_ERR("line %u loop test audio not cleared: %s\n", l->index,
			 hal_status_name(st));
		if (st != HAL_TRANSIENT)
			l->loop_test_audio = false;
		return st;
	}

	if (st == HAL_TRANSIENT && !retry) {
		l->ring_retry.cb = loop_test_audio_retry_expire;
		uloop_timeout_set(&l->ring_retry, HAL_RETRY_MS);
		return st;
	}

	ULOG_ERR("line %u loop test audio not armed: %s\n", l->index,
		 hal_status_name(st));

	return st;
}

static enum hal_status loop_test_audio_set(struct line *l, bool on)
{
	uloop_timeout_cancel(&l->ring_retry);

	return loop_test_audio_issue(l, on, false);
}

/* The hook can move under a pending retry, and the rule is read again here
 * rather than trusted from the refusal: a cross pair whose other handset went
 * down wants no audio on this port any more. */
static void loop_test_audio_retry_expire(struct uloop_timeout *t)
{
	struct line *l = container_of(t, struct line, ring_retry);

	if (!loop_test_audio_want(l)) {
		TRACE(TRACE_FSM, "line %u loop test audio retry dropped: the "
		      "rule no longer wants it", l->index);
		return;
	}

	loop_test_audio_issue(l, true, true);
}

/*
 * The audio rule. Under SELF a port carries the loop while its own handset is
 * lifted. Under PEER both ports carry the cross connection only while both
 * handsets are lifted: the subscriber who lifted first hears silence rather
 * than the ring voltage of the other port, and the arm then meets two lines
 * that are off hook and not ringing, which is what the backend's own busy
 * guard asks of it.
 */
static bool loop_test_audio_want(const struct line *l)
{
	unsigned int peer;

	if (!l->loop_test || !l->loop_test_offhook)
		return false;

	if (loop_test_mode_of(l, &peer) == HAL_LOOP_SELF)
		return true;

	return lines[peer].loop_test && lines[peer].loop_test_offhook;
}

/* Every port the mode holds, after every hook edge, so a staggered pickup on
 * a two-port board is answered correctly. Only a change is issued. */
static void loop_test_reconcile(void)
{
	unsigned int i;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct line *l = &lines[i];
		bool want;

		if (!l->loop_test)
			continue;

		want = loop_test_audio_want(l);
		/* An arm this port is still waiting to retry describes a state
		 * the rule no longer wants, and nothing else would drop it: the
		 * skip below sees no change, because a refused arm left the
		 * record clear. A pending ring retry is not this one and stays.
		 */
		if (!want && l->ring_retry.cb == loop_test_audio_retry_expire)
			uloop_timeout_cancel(&l->ring_retry);

		if (want == l->loop_test_audio)
			continue;

		loop_test_audio_set(l, want);
	}
}

/*
 * The entry of one port. The tone stop comes first: a line the config already
 * held out of service can be playing the reorder of st_oos(), and
 * line_service_sync() does not force a machine that is already FSM_OOS, so
 * nothing else would silence it.
 *
 * The hook is then read directly, in the hook_state_sync() idiom, because
 * this controller consumes the hook edges of a held port and has no other way
 * to learn where the handset is. A query the backend refuses is taken as on
 * hook: the port then rings, which is the state the mode starts in and the
 * one an off-hook edge corrects.
 */
static void loop_test_enter(struct line *l)
{
	enum hal_status st;
	bool offhook = false;

	if (!uv.hal)
		return;

	hal_backend_ops.tone_stop(uv.hal, l->index);

	st = hal_backend_ops.line_hook_query(uv.hal, l->index, &offhook);
	if (st == HAL_TRANSIENT)
		st = hal_backend_ops.line_hook_query(uv.hal, l->index,
						     &offhook);

	if (st != HAL_OK) {
		TRACE(TRACE_HOOK, "line %u hook state not read on the loop "
		      "test entry: %s", l->index, hal_status_name(st));
		offhook = false;
	}

	l->loop_test_offhook = offhook;
	if (!offhook)
		loop_test_ring_start(l);

	loop_test_reconcile();
}

/*
 * One port out of the mode, and whether it left. A transient clear keeps the
 * port held, because the loop may still stand in the backend and a line that
 * carries one refuses every stream configuration; the operator repeats the
 * disarm and the port leaves on the attempt that lands.
 */
static bool loop_test_leave(struct line *l)
{
	loop_test_ring_stop(l);
	if (loop_test_audio_set(l, false) == HAL_TRANSIENT)
		return false;

	l->loop_test = false;
	l->loop_test_offhook = false;

	return true;
}

/*
 * The hook intake of a held port. Every edge is consumed here and nothing
 * reaches the protocol. An off-hook edge stops the bell and reconciles
 * the audio; an on-hook edge takes the audio down and rings again, so a
 * subscriber who hangs up gets the next ring window.
 */
static void loop_test_hook(struct line *l, const struct hal_event *ev)
{
	switch (ev->u.hook.state) {
	case HAL_HOOK_SEIZE:
		l->loop_test_offhook = true;
		loop_test_ring_stop(l);
		loop_test_reconcile();
		return;
	case HAL_HOOK_RELEASE:
		l->loop_test_offhook = false;
		loop_test_reconcile();
		loop_test_ring_start(l);
		return;
	default:
		TRACE(TRACE_HOOK, "line %u hook state %u ignored by the loop "
		      "test", l->index, ev->u.hook.state);
		return;
	}
}

/* The standing view, which every answer carries: a refusal reports the mode
 * that is running and an accepted call the one it left behind. */
static void loop_test_view_fill(struct line_loop_test_view *out)
{
	unsigned int peer;

	if (!out)
		return;

	memset(out, 0, sizeof(*out));
	if (!loop_test_armed)
		return;

	out->enabled = true;
	out->mode = loop_test_mode_of(&lines[0], &peer) == HAL_LOOP_SELF ?
		    LINE_LOOP_TEST_MODE_SELF : LINE_LOOP_TEST_MODE_CROSS;
	out->lines = loop_test_line_count();
	out->ring_s = loop_test_ring_ms / 1000u;
	out->delay_ms = loop_test_delay_ms;
}

static bool loop_test_backend_has(void)
{
	if (!uv.hal || !hal_backend_ops.line_loop_set)
		return false;

	return (hal_backend_ops.cap_get(uv.hal)->features & HAL_FEAT_LOOP) != 0;
}

/*
 * The arm. Every port is marked before the first service sync, because
 * line_serve_want() reads the mark: that sync is what forces the machine out
 * of service, tears the legs down and powers a port the config leaves
 * unpowered. The entry pass then runs over a board that is already held, so
 * the audio rule reads both ports.
 */
static void loop_test_enable(unsigned int count, bool want_self,
			     unsigned int ring_s, unsigned int delay_ms)
{
	unsigned int i;

	loop_test_want_self = want_self;
	loop_test_ring_ms = ring_s * 1000u;
	loop_test_delay_ms = delay_ms;
	loop_test_armed = true;

	for (i = 0; i < count; i++)
		lines[i].loop_test = true;

	for (i = 0; i < count; i++)
		line_service_sync(&lines[i], uv.cfg);

	for (i = 0; i < count; i++)
		loop_test_enter(&lines[i]);
}

/*
 * The disarm, which undoes the arm in its own order: the ports go quiet, the
 * service state comes back from the config, and the closing query injects the
 * seize of a handset the subscriber left lifted.
 *
 * A port the clear could not take out holds the whole mode. Nothing returns
 * to service behind a loop that may still stand, because a line that carries
 * one refuses every stream configuration, and the caller reports the refusal
 * so the operator asks again.
 */
static bool loop_test_disable(unsigned int count)
{
	unsigned int i;
	bool left = true;

	for (i = 0; i < count; i++) {
		if (!loop_test_leave(&lines[i]))
			left = false;
	}

	if (!left)
		return false;

	loop_test_armed = false;

	for (i = 0; i < count; i++)
		line_service_sync(&lines[i], uv.cfg);

	for (i = 0; i < count; i++)
		hook_state_sync(&lines[i]);

	return true;
}

enum line_loop_test_status line_loop_test_set(bool enable, bool want_self,
					      unsigned int ring_s,
					      unsigned int delay_ms,
					      struct line_loop_test_view *out)
{
	unsigned int count = loop_test_line_count();
	unsigned int tested;

	/* every answer carries the standing view, so a refusal reports what
	 * the board is doing now */
	loop_test_view_fill(out);

	if (!lines_up || !uv.hal || !uv.cfg || !count)
		return LINE_LOOP_TEST_NO_LINE;

	if (!line_hal_ready())
		return LINE_LOOP_TEST_NOT_READY;

	if (enable && !loop_test_backend_has())
		return LINE_LOOP_TEST_UNSUPPORTED;

	if (line_test_call_active())
		return LINE_LOOP_TEST_CALL_ACTIVE;

	if (line_test_running(&tested))
		return LINE_LOOP_TEST_TEST_RUNNING;

	if (ring_s < 1 || ring_s > LINE_LOOP_TEST_RING_S_MAX ||
	    delay_ms > LINE_LOOP_TEST_DELAY_MAX_MS)
		return LINE_LOOP_TEST_BAD_ARGUMENT;

	/* a call that asks for the state the board is already in changes
	 * nothing: the window and the delay belong to the arm that started
	 * it */
	if (enable == loop_test_armed)
		return LINE_LOOP_TEST_OK;

	if (enable)
		loop_test_enable(count, want_self, ring_s, delay_ms);
	else if (!loop_test_disable(count))
		return LINE_LOOP_TEST_REFUSED;

	loop_test_view_fill(out);

	return LINE_LOOP_TEST_OK;
}

/*
 * The daemon stop. It runs while the HAL is still attached, so the bell stops
 * and the loop clears on a live backend; the service state is not derived
 * again, because every line is about to be forced out anyway.
 */
void lines_loop_test_stop(void)
{
	unsigned int i;

	if (!loop_test_armed)
		return;

	/* the answer of the clear is read nowhere here: every line is about to
	 * be forced out and the backend goes with the daemon */
	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		if (lines[i].loop_test)
			loop_test_leave(&lines[i]);
	}

	loop_test_armed = false;
}

static void hook_event(struct line *l, const struct hal_event *ev)
{
	struct proto_line_event fwd = { .type = PROTO_EV_HOOK };

	/* In front of the two drops below, so the count follows what this line
	 * received. A report either drop swallows says the shell itself moved
	 * the loop. The two readings of the hook resync then bracket different
	 * conditions and cannot be compared. The resync abandons the release,
	 * which is the conservative direction (lines_hook_resync). */
	l->hook_activity++;

	/*
	 * Every class, for the whole removal window (fwd_disc_line_state()).
	 * The report is the DSP's verdict on a loop break this shell made, and
	 * it says nothing about the handset. It is dropped and never deferred:
	 * an edge replayed behind the window would describe a hook position the
	 * settle has since read directly.
	 */
	if (l->fwd_disc_down) {
		TRACE(TRACE_HOOK, "line %u hook state %u suppressed during the battery "
		      "removal", l->index, ev->u.hook.state);
		return;
	}

	/* The loop test owns this port. It sits behind the drop above because
	 * the arm closes that window itself: line_service_sync() forces the
	 * machine out of service, and the force aborts the removal. */
	if (l->loop_test) {
		loop_test_hook(l, ev);
		return;
	}

	switch (ev->u.hook.state) {
	case HAL_HOOK_SEIZE:
		/* A line that cannot collect digits cannot carry a call, so it
		 * goes out of service and the seize behind it plays the OOS
		 * reorder instead of a dial tone no digit would ever follow.
		 * The force reaches the protocol before the seize does
		 * (proto/proto.h, the re-entrancy rule). */
		if (!detector_arm(l) && !l->oos)
			line_oos_take(l);
		break;
	case HAL_HOOK_RELEASE:
	case HAL_HOOK_FLASH:
		break;
	case HAL_HOOK_PULSE:
		hook_pulse(l, ev->u.hook.pulses, ev->u.hook.pulse_invert);
		return;
	default:
		TRACE(TRACE_HOOK, "line %u hook state %u ignored", l->index,
		      ev->u.hook.state);
		return;
	}

	fwd.u.hook.state = ev->u.hook.state;
	line_event_forward(l, &fwd);
}

/*
 * The digit intake. The two directions share the record and nothing else. A
 * digit from the phone is one the subscriber dialled and it goes to the
 * machine; a digit from the wire is a key the far end pressed on its own
 * handset, and this line plays it toward the phone instead.
 */
static void digit_event(struct line *l, const struct hal_event *ev)
{
	struct proto_line_event fwd = { .type = PROTO_EV_DIGIT };

	if (ev->u.digit.remote) {
		digit_net_wire(l, ev);
		return;
	}

	/*
	 * The regenerated tone reaches the uplink detector through the echo
	 * path, and the call layer sends an INFO for a digit with no measured
	 * length, so without this the line would send the far end's own key
	 * back to it.
	 */
	if (digit_play_window_open(l)) {
		TRACE(TRACE_HOOK, "line %u digit '%c' dropped, a network digit is playing",
		      l->index, ev->u.digit.digit);
		return;
	}

	/*
	 * Behind both drops above. A key the subscriber pressed says the
	 * handset is up, and the release arm of the hook resync must not read
	 * over it (lines_hook_resync). A key from the wire says nothing about
	 * this loop. A report the drop above swallowed fell inside the playout
	 * window, so the shell cannot say which feed produced it.
	 */
	l->hook_activity++;

	/* an injected digit carries no length, and nothing measured it */
	fwd.u.digit.digit = ev->u.digit.digit;
	fwd.u.digit.duration_ms = ev->u.digit.duration_ms;
	fwd.u.digit.measured = ev->u.digit.duration_ms != 0;
	line_event_forward(l, &fwd);
}

/*
 * The T.38 stream report (hal.h HAL_EVENT_T38). The DSP says the fax session
 * ended or that the far end moved its UDPTL payload type, and it says so per
 * stream (hal.h, stream ids).
 *
 * The state and the stream are the whole record here. hal_event carries a
 * payload_type beside them and nothing fills it: UDPTL carries no payload
 * type, so the udsp backend posts the disconnect state alone and a trace of
 * that field would print 0 on every board.
 *
 * It drives nothing. The SIP session decides when a T.38 call ends, and the
 * payload type of a UDPTL stream is not something this side programs, so the
 * report is a diagnostic: what a field engineer reads to tell a fax that
 * finished from a call that dropped.
 */
static void t38_event(struct line *l, const struct hal_event *ev)
{
	/* hal.h u.t38.state: 0 = disconnect, 1 = payload type change */
	static const char * const states[] = {
		[0] = "disconnect",
		[1] = "payload-type-change",
	};
	const char *name = ev->u.t38.state < ARRAY_SIZE(states) ?
			   states[ev->u.t38.state] : "unknown";

	TRACE(TRACE_HAL, "line %u stream %u t38 %s", l->index, ev->stream, name);
}

/*
 * The two hardware reasons a line leaves service, and the shared shape of the
 * two moves.
 *
 * A port whose SLIC reported a fault cannot carry a call, and neither can a
 * port the backend declares dead. Such a port leaves service the same way a
 * failed detector arm makes it leave (hook_event above). The shell records the
 * reason, because the config cannot describe a faulted part. Without the
 * record the next reload derives service from the config alone and puts a
 * broken line back.
 *
 * The two reasons are independent, so each rides its own bit: l->fault for the
 * SLIC fault, l->down for the dead line. Each clear source ends only its own
 * bit. This is what keeps the two apart across the two event rings. A fault
 * rides the engine ring and a line record rides the SLIC ring, and the rings
 * drain in their own order, so a line-return that overtook a fresh fault would
 * end the fault too if one bit held both.
 *
 * hw_oos_take() takes the line out for either reason, and it runs the force
 * once: a second record on a line that is already out must not re-run the OOS
 * entry, which would re-attempt a battery restore the forward disconnect still
 * owes. hw_oos_derive() answers the service question again after a bit ends. A
 * line the config disables, a line the board does not carry, and a line whose
 * other bit still stands all stay out. A line that holds no bit and no config
 * reason returns, which is what keeps a failed detector arm out until a reload.
 *
 * Only a line that really returns reads its hook. lines_reload() reads it on
 * the service entry for the same reason. A handset lifted while the line was
 * out leaves the machine in IDLE with the hook desynced.
 */
static void hw_oos_take(struct line *l)
{
	if (l->oos)
		return;

	line_oos_take(l);
}

static void hw_oos_derive(struct line *l)
{
	bool was_oos = l->oos;

	/* the early half of the bring-up wires the machines before a config
	 * exists, and the derive reads one */
	if (uv.cfg)
		line_service_sync(l, uv.cfg);

	if (was_oos && !l->oos)
		hook_state_sync(l);
}

/*
 * A code of 0 is the clear of a SLIC fault (hal.h u.fault.code). The library
 * posts it once per fault. It posts it only for a line whose feed is powered
 * and confirmed.
 *
 * The engine stall never reaches here, and neither does a fault from a backend
 * whose fault word the daemon cannot decode. event_dispatch.c owns both
 * decisions. It exits on the stall bit and hands over the SLIC bits alone, and
 * it hands over nothing unless the udsp backend produced the record. So this
 * arm needs no backend knowledge of its own, which is what keeps the line
 * shell free of it.
 */
static void slic_fault_event(struct line *l, const struct hal_event *ev)
{
	if (ev->u.fault.code) {
		l->fault = true;
		hw_oos_take(l);
		return;
	}

	if (!l->fault)
		return;

	l->fault = false;
	hw_oos_derive(l);
}

/*
 * HAL_LINE_DOWN says the backend declared this port dead: its SLIC init
 * failed, or its control bus answers nothing. Any other state says the port
 * carries a feed again.
 *
 * The ERR is logged here and not in event_dispatch.c, because the record can
 * repeat: the backend re-posts it after a lost completion and after a
 * start-stop-start on a still failing init. The log then rides the l->down
 * transition, so one entry logs once and a repeat logs nothing.
 *
 * event_dispatch.c scopes this arm to the one backend that posts the record
 * for a dead port alone, exactly as it scopes the fault arm.
 */
static void line_state_take(struct line *l, const struct hal_event *ev)
{
	if (ev->u.line.state == HAL_LINE_DOWN) {
		if (!l->down)
			ULOG_ERR("line %u backend reports the line down, "
				 "forcing it out of service\n", l->index);

		l->down = true;
		hw_oos_take(l);
		return;
	}

	if (!l->down)
		return;

	l->down = false;
	hw_oos_derive(l);
}

/* The core takes the state, and the protocol hears the record behind it. */
static void line_state_event(struct line *l, const struct hal_event *ev)
{
	struct proto_line_event fwd = { .type = PROTO_EV_LINE_STATE };

	line_state_take(l, ev);

	fwd.u.line.state = ev->u.line.state;
	line_event_forward(l, &fwd);
}

static void input_forward(struct line *l, const struct hal_event *ev)
{
	struct proto_line_event fwd = { .type = PROTO_EV_INPUT };

	TRACE(TRACE_HOOK, "line %u input key %u pressed %u", l->index,
	      ev->u.input.key, ev->u.input.pressed);

	fwd.u.input.key = ev->u.input.key;
	fwd.u.input.pressed = ev->u.input.pressed;
	line_event_forward(l, &fwd);
}

/* A tone the detector reported, the fax and modem tones above all, which
 * the protocol's media policy reads (PROTO_EV_TONE). */
static void tone_forward(struct line *l, const struct hal_event *ev)
{
	struct proto_line_event fwd = { .type = PROTO_EV_TONE };

	fwd.stream = ev->stream;
	fwd.u.tone.code = ev->u.tone.code;
	fwd.u.tone.direction = ev->u.tone.direction;
	fwd.u.tone.end = ev->u.tone.end;
	fwd.u.tone.duration_ms = ev->u.tone.duration_ms;
	line_event_forward(l, &fwd);
}

/*
 * The first packet of a stream. The trace runs for every record and keeps a
 * fixed shape, because the on-target procedure reads that line against the
 * rtp_first_tx one.
 */
static void rtp_first_rx_forward(struct line *l, const struct hal_event *ev)
{
	struct proto_line_event fwd = { .type = PROTO_EV_FIRST_RTP };

	TRACE(TRACE_HAL, "line %u event rtp_first_rx stream=%u", l->index,
	      ev->stream);

	fwd.stream = ev->stream;
	line_event_forward(l, &fwd);
}

/*
 * The backend ended a ring. The loop test runs no protocol ring at all, so
 * the end of its capped bell is the only report that says the port went
 * quiet, and its status key follows it.
 *
 * Known window: a disable and an enable inside one tick clear the flag of the
 * new ring with the end record of the stopped one. The key is informational
 * and the next hook edge corrects it.
 */
static void ring_end_event(struct line *l, const struct hal_event *ev)
{
	struct proto_line_event fwd = { .type = PROTO_EV_RING_END };

	if (l->loop_test)
		l->loop_test_ringing = false;

	fwd.stream = ev->stream;
	line_event_forward(l, &fwd);
}

void line_hal_event(const struct hal_event *ev)
{
	struct line *l = line_get(ev->line);

	/*
	 * The DSP reports the current hook state right after the channel
	 * start, and that record races the dsp_start completion lines_start()
	 * runs on: both cross one socketpair in no fixed order. The machines
	 * are wired before the daemon enters its event loop, so the report is
	 * consumed whichever order it arrives in and the seize of a handset
	 * that was already lifted lands at t~0. The closing query of
	 * lines_start() (hook_state_sync) still derives the state, for a boot
	 * that produced no report at all; a report that already seized the
	 * line leaves it where the matrix ignores the second SEIZE.
	 *
	 * The guard below therefore holds for the suites and for the window
	 * after lines_done(), and nothing in the daemon reaches it at start-up.
	 */
	if (!lines_up) {
		TRACE(TRACE_HOOK, "line %u event type %u before the lines are up",
		      ev->line, ev->type);
		return;
	}

	if (!l) {
		TRACE(TRACE_HOOK, "event type %u for unknown line %u", ev->type, ev->line);
		return;
	}

	switch (ev->type) {
	case HAL_EVENT_HOOK:
		hook_event(l, ev);
		return;
	case HAL_EVENT_DIGIT:
		digit_event(l, ev);
		return;
	case HAL_EVENT_TONE:
		tone_forward(l, ev);
		return;
	case HAL_EVENT_T38:
		t38_event(l, ev);
		return;
	case HAL_EVENT_RTP_FIRST_RX:
		rtp_first_rx_forward(l, ev);
		return;
	case HAL_EVENT_SLIC_FAULT:
		slic_fault_event(l, ev);
		return;
	case HAL_EVENT_LINE:
		line_state_event(l, ev);
		return;
	case HAL_EVENT_RING_START:
		/* Queued indications can describe a previous ring. The
		 * protocol's ring requests own the current one, a deferred ring
		 * included. */
		return;
	case HAL_EVENT_RING_END:
		ring_end_event(l, ev);
		return;
	case HAL_EVENT_INPUT:
		input_forward(l, ev);
		return;
	default:
		TRACE(TRACE_HOOK, "line %u event type %u ignored", l->index, ev->type);
		return;
	}
}

/* ---------------------------------------------------------- injected events */

/*
 * The diagnostic injections (line/line.h carries the contract and the hook
 * desync record). Both helpers below build the record the DSP would have sent
 * and hand it to line_hal_event() above, which is what makes the matrix the
 * single authority: the seize arms the digit detector exactly as a lifted
 * handset does, and an out of service line answers with the OOS reorder
 * rather than with a refusal invented here.
 *
 * Three fields the injected record leaves at zero, and nothing on the path it
 * takes reads any of them. seq belongs to the gap accounting of
 * event_seq_check() in event_dispatch.c. The event reader of main.c runs it
 * above this intake, so it never sees an injection. A verb is therefore not
 * counted as a hardware report that never happened, and "voice status"
 * events.lost stays honest. stream and stamp_ms are backend stamped on a real
 * record. event_dispatch.c reads both, and an injection never passes through
 * it. The stream arms of this file read stream: digit_net_wire() for a remote
 * digit, t38_event(), tone_forward() and rtp_first_rx_forward(). A hook record
 * and a local digit reach none of them. An injection that reaches one has to
 * start filling the field here.
 */
static void inject_hook_event(struct line *l, uint8_t state)
{
	struct hal_event ev = { .type = HAL_EVENT_HOOK, .line = (uint16_t)l->index };

	ev.u.hook.state = state;
	line_hal_event(&ev);
}

/*
 * Duration 0, which is the injected class. digit_event() forwards the
 * reported length, and the protocol reads it as a detector measurement
 * (proto/proto.h PROTO_EV_DIGIT measured): nothing measured an injected digit.
 * Zero is the value the whole path already reads as "nobody measured this", the
 * value a pulse digit carries too, so the event carries nothing here that a
 * later reader could mistake for a measured length, and digit_event() needs
 * no injected-digit branch of its own.
 */
static void inject_digit_event(struct line *l, char digit)
{
	struct hal_event ev = { .type = HAL_EVENT_DIGIT, .line = (uint16_t)l->index };

	ev.u.digit.digit = digit;
	line_hal_event(&ev);
}

/*
 * The whole refusal gate: a port this board carries, and a machine to give the
 * event to. The service state is not part of it. An out of service line
 * answers a seize with reorder and drops a digit, and both are real
 * answers the matrix already carries, so refusing an OOS line here would hide
 * the one state a field engineer asks about most.
 */
static enum line_inject inject_line_take(unsigned int index, struct line **out)
{
	/* before lines_init() the machines are not wired, which is the same
	 * reason line_hal_event() drops a hardware event that beats it */
	if (!lines_up) {
		TRACE(TRACE_HOOK, "line %u injection refused: the lines are not up",
		      index);
		return LINE_INJECT_NO_LINES;
	}

	if (!line_exists(index)) {
		TRACE(TRACE_HOOK, "line %u injection refused: this board carries no "
		      "such port", index);
		return LINE_INJECT_NO_LINE;
	}

	*out = line_get(index);
	if (*out)
		return LINE_INJECT_OK;

	TRACE(TRACE_HOOK, "line %u injection refused: the config array is narrower "
	      "than the port count the backend reports", index);

	return LINE_INJECT_NO_LINE;
}

/*
 * The dialable alphabet, and only it. The dial plan matcher and call.c both
 * also admit the DTMF letters A to D, which no subscriber keypad carries and
 * no shipped map uses; this verb takes the subscriber's alphabet instead, so a
 * mistyped character is refused rather than buffered as a digit no map can
 * ever match.
 */
static bool dial_digit_ok(char c)
{
	if (c >= '0' && c <= '9')
		return true;

	return c == '*' || c == '#';
}

/*
 * The length bound is the digit buffer of the dial plan, which the machine of
 * the protocol collects into and which a configured warmline number is held
 * to for the same reason: a collector drops what does not fit, so an
 * over-long number would dial a different party than the caller named. It is
 * refused whole, and the refusal runs before anything is injected.
 */
static bool dial_number_ok(const char *number)
{
	size_t i;

	if (!number || !number[0] || strlen(number) > DIALPLAN_DIGITS_MAX)
		return false;

	for (i = 0; number[i]; i++) {
		if (!dial_digit_ok(number[i]))
			return false;
	}

	return true;
}

/*
 * Where a seize still means something: the handset is on the cradle, which
 * the protocol's flags say (proto/proto.h PROTO_LF_OFFHOOK). Under SIP four
 * states act on SEIZE: IDLE gives the dial tone, or the fast busy chain on a
 * line that cannot carry a dial, RINGING answers the caller, HOLDING resumes
 * the held leg and OOS plays reorder. In the other thirteen the cell ignores
 * the seize as a duplicate hook report, so the dial verb skips the seize
 * there.
 *
 * The skip is not cosmetic. The SEIZE intake arms the detector before the
 * machine sees the event (hook_event), so an unconditional seize would push
 * the whole channel view again under a call that is already running.
 */
static bool line_on_hook(const struct line *l)
{
	return !(line_flags_of(l) & PROTO_LF_OFFHOOK);
}

enum line_inject line_inject_seize(unsigned int line)
{
	struct line *l = NULL;
	enum line_inject st = inject_line_take(line, &l);

	if (st != LINE_INJECT_OK)
		return st;

	/* loud rather than traced: this is where a line and its handset stop
	 * agreeing, and a log read afterwards has to tell the two apart */
	ULOG_INFO("line %u seize injected\n", line);
	inject_hook_event(l, HAL_HOOK_SEIZE);

	return LINE_INJECT_OK;
}

enum line_inject line_inject_release(unsigned int line)
{
	struct line *l = NULL;
	enum line_inject st = inject_line_take(line, &l);

	if (st != LINE_INJECT_OK)
		return st;

	ULOG_INFO("line %u release injected\n", line);
	inject_hook_event(l, HAL_HOOK_RELEASE);

	return LINE_INJECT_OK;
}

enum line_inject line_inject_dial(unsigned int line, const char *number,
				  unsigned int *digits)
{
	struct line *l = NULL;
	enum line_inject st = inject_line_take(line, &l);
	unsigned int i;

	*digits = 0;

	if (st != LINE_INJECT_OK)
		return st;

	if (!dial_number_ok(number)) {
		TRACE(TRACE_HOOK, "line %u dial injection refused: '%s' is not a "
		      "dialable number", line, number ? number : "");
		return LINE_INJECT_BAD_NUMBER;
	}

	ULOG_INFO("line %u dial '%s' injected\n", line, number);

	if (line_on_hook(l))
		inject_hook_event(l, HAL_HOOK_SEIZE);

	/*
	 * Every digit, whatever the earlier ones did to the state. A number the
	 * dial plan completes early leaves the rest to the CALLING cell, which
	 * ignores them; dropping them here would put a second policy beside the
	 * matrix.
	 */
	for (i = 0; number[i]; i++)
		inject_digit_event(l, number[i]);

	*digits = i;

	return LINE_INJECT_OK;
}

/* ------------------------------------------------------------------ config */

/*
 * A configuration fault costs the subscriber either a feature the daemon
 * implements or only a code the daemon recognizes and does not support. The
 * first is a warning, the second a trace. A fault between two codes of
 * unsupported rows, for example the two forward-on-unreachable codes, must
 * not warn on every reload.
 */
static void line_fault_report(const struct line *l, bool loud, const char *text)
{
	if (loud) {
		ULOG_WARN("line %u %s\n", l->index, text);
		return;
	}

	TRACE(TRACE_FSM, "line %u %s", l->index, text);
}

/*
 * The removal's own hazard, reported and not clamped. A loop break is
 * classified like any other: under the default window a 750 ms removal comes
 * back as a FLASH and a 2000 ms one as a RELEASE. The removal window
 * suppresses both, which is why this fault is informational and the length is
 * used exactly as configured: the operator is told which band it lands in,
 * once per reload like every other derive-time fault of this file.
 *
 * It reads the same effective window hook_timing_push() programs, so a
 * region whose flash window moves takes the band and the fault with it.
 */
static void fwd_disc_band_check(struct line *l, const struct cfg_channel *ch)
{
	unsigned int ms = ch->forward_disconnect_delay_ms;
	struct hal_hook_timing ts;
	char text[160];

	hook_timing_get(&ts);

	if (ms >= ts.flash_min_ms && ms <= ts.flash_max_ms)
		snprintf(text, sizeof(text),
			 "forward_disconnect_delay %u ms is inside this line's flash "
			 "band %u..%u ms, and the hook report it makes is suppressed",
			 ms, ts.flash_min_ms, ts.flash_max_ms);
	else if (ms >= ts.release_min_ms)
		snprintf(text, sizeof(text),
			 "forward_disconnect_delay %u ms is at or above this line's "
			 "release window %u ms, and the hook report it makes is "
			 "suppressed", ms, ts.release_min_ms);
	else
		return;

	line_fault_report(l, true, text);
}

static void fwd_disc_check(struct line *l, const struct cfg_channel *ch)
{
	if (!cfg_fwd_disc_on(ch))
		return;

	/*
	 * The capability is resolved here and stored, so the reversal path asks
	 * no questions of the backend and a board that cannot reverse says so
	 * once per reload instead of once per call. No HAL at all is not a
	 * fault to report: the line is out of service for larger reasons and
	 * every other op is refused the same way.
	 */
	l->fwd_disc_avail = uv.hal &&
			    (hal_backend_ops.cap_get(uv.hal)->features &
			     HAL_FEAT_POLARITY);

	/* the fault names what this line asked for: the removal form needs the
	 * loop broken, and telling its operator the board cannot reverse a
	 * battery would send them looking for the wrong thing */
	if (uv.hal && !l->fwd_disc_avail)
		line_fault_report(l, true,
				  ch->forward_disconnect_battery == CFG_FWD_DISC_BATT_REMOVE ?
				  "forward disconnect is configured but this "
				  "backend cannot break the line loop" :
				  "forward disconnect is configured but this "
				  "backend cannot reverse the line battery");

	/* the band belongs to the loop break: a reversal keeps the loop current
	 * up, so the classifier sees no edge and there is nothing to report */
	if (ch->forward_disconnect_battery != CFG_FWD_DISC_BATT_REMOVE)
		return;

	fwd_disc_band_check(l, ch);
}

/* The core copies staged from the live ones, when nothing is staged yet. */
static void chan_stage_seed(struct line *l)
{
	if (l->chan_staged)
		return;

	l->chan_new = l->chan;
	l->cid_format_new = l->cid_format;
	l->mwi_format_new = l->mwi_format;
	memcpy(l->ring_pattern_new, l->ring_pattern, sizeof(l->ring_pattern_new));
	l->chan_staged = true;
}

/*
 * The region read-back moved under this line, so the region-derived base of
 * its view moved with it. The core stages its copies when nothing is staged
 * and patches the region-derived fields; the protocol re-derives what it
 * takes from the region (ops->region_changed) and applies both at its rest
 * state, so a read-back never retunes a line under a live call. A protocol
 * with no such entry takes the core copies at once. The hook window is not
 * patched here: hook_timing_get resolves the region base at read time and
 * the guarded push of the landing carries it. The forward disconnect is
 * checked again, because the effective hook window can move the delay band;
 * a band already reported at derive time reports again, which is the
 * accepted cost.
 */
static void region_settle(struct line *l)
{
	const struct proto_ops *ops = proto_ops_get();

	if (!uv.cfg || !lines_up)
		return;

	chan_stage_seed(l);
	l->cid_format_new = region_hal.cid_format;
	l->mwi_format_new = region_hal.mwi_format;
	ring_pattern_base(l->ring_pattern_new);
	fwd_disc_check(l, &l->chan_new);

	if (!ops->region_changed) {
		line_cfg_landed(l->index);
		return;
	}

	ops->region_changed(l->index, &region_hal);
}

static bool line_in_service(const struct cfg *cfg, unsigned int index)
{
	const struct cfg_channel *ch = &cfg->channel[index];

	if (!line_exists(index))
		return false;

	/* voip.base.channels is the count in service; a section above it stays
	 * configured but unused, which is how the schema shrinks a board */
	if (index >= cfg->channels)
		return false;

	return cfg->enabled && ch->present && ch->enabled;
}

/*
 * The whole service question, and the one predicate every caller asks. The
 * config admits the line, and neither hardware reason stands against it. The
 * facts are separate. A reload re-derives the config fact and cannot answer
 * the two hardware facts. Each hardware bit therefore survives a reload, and
 * only its own backend record ends it: a fault code of 0 for l->fault (hal.h
 * u.fault.code), and a line state above HAL_LINE_DOWN for l->down (hal.h
 * u.line.state).
 */
static bool line_serve_want(const struct line *l, const struct cfg *cfg)
{
	/* The loop test is the fourth fact, and the only one an operator arms.
	 * A port the mode holds carries no call: this one term is what forces
	 * the machine, tears the legs down, holds the line out across a reload
	 * and puts the oos key in the status reply. The fifth is no running
	 * protocol: the null table of proto/proto.c then stands, and every line
	 * stays out across the DSP completion, a reload and every hook event.
	 * The sixth is a hold of the protocol's own (line_service_hold). */
	return line_in_service(cfg, l->index) && !l->fault && !l->down &&
	       !l->loop_test && proto_running() && !l->proto_hold;
}

/*
 * The service state moved, in the one shape that carries it: the oos bit
 * moves first, and then the protocol hears it (ops->line_service), so a drain
 * inside that call reads the service state the call establishes (proto/proto.h
 * enum proto_line_flag). On the way out the protocol tears the legs down and
 * forces its machine out; on the way in it clears the machine, and the drain
 * inside presents what the line came back owing. lines_reload() runs it for a
 * config that moved.
 *
 * lines_start() runs it for a capability that moved. The early half has to
 * derive the service state before the DSP has reported anything, and
 * line_exists() reads the backend line count. The early derivation is
 * provisional, and lines_start() settles it against the line count of the
 * started DSP.
 */
static void line_service_sync(struct line *l, const struct cfg *cfg)
{
	bool serve = line_serve_want(l, cfg);

	/* The battery follows the config fact and the loop test
	 * (battery_sync). It is derived before the force below, whose forward
	 * disconnect abort reads it, and the write lands after the teardown on
	 * the way out and before the clear on the way in, so the drain of that
	 * clear presents a deferred message over a fed loop.
	 *
	 * The loop test is the one reason to feed a port the config does not
	 * enable: a port with no battery can neither ring nor report a hook
	 * edge, so the test would hold a dead line. The disarm clears the mark
	 * and the port goes back to the config rule. */
	l->battery_wanted = line_in_service(cfg, l->index) || l->loop_test;

	if (!serve && !l->oos) {
		l->oos = true;
		proto_ops_get()->line_service(l->index, false,
					      PROTO_SERVICE_CFG);
		TRACE(TRACE_FSM, "line %u out of service", l->index);
		battery_sync(l);
		return;
	}

	if (!serve || !l->oos) {
		battery_sync(l);
		return;
	}

	battery_sync(l);
	l->oos = false;
	proto_ops_get()->line_service(l->index, true, PROTO_SERVICE_CFG);
	TRACE(TRACE_FSM, "line %u in service", l->index);
}

/* The hold of the protocol moved: the service sync runs one pass later. */
static void service_sync_expire(struct uloop_timeout *t)
{
	struct line *l = container_of(t, struct line, service_sync);
	bool was_oos = l->oos;

	if (!lines_up || !uv.cfg)
		return;

	line_service_sync(l, uv.cfg);
	if (was_oos && !l->oos)
		hook_state_sync(l);
}

static void service_sync_arm(struct line *l)
{
	l->service_sync.cb = service_sync_expire;
	uloop_timeout_set(&l->service_sync, 0);
}

void line_service_hold(unsigned int line, const char *reason)
{
	struct line *l = line_get(line);

	if (!l || l->proto_hold)
		return;

	l->proto_hold = true;
	TRACE(TRACE_FSM, "line %u held out of service by the protocol: %s",
	      line, reason ? reason : "");
	service_sync_arm(l);
}

void line_service_release(unsigned int line)
{
	struct line *l = line_get(line);

	if (!l || !l->proto_hold)
		return;

	l->proto_hold = false;
	service_sync_arm(l);
}

/*
 * Every timer under this line, the shell's own uloop timeouts and the ones the
 * protocol owns (ops->line_reset), which it cancels first. Both kinds are
 * linked into a list threaded through the storage that holds them, so a
 * lifecycle edge that frees or re-initialises a line while one is still armed
 * corrupts that list for whatever runs next. The call waiting record deferral
 * is a shell timer of this kind; the machine half follows the same rule,
 * because the SIP machine's timers live inside struct sip_line, which the
 * re-init of the protocol clears the same way.
 */
static void line_timers_cancel(struct line *l)
{
	proto_ops_get()->line_reset(l->index);

	cw_cid_drop(l);
	digit_queue_flush(l);
	uloop_timeout_cancel(&l->dial_tone);
	uloop_timeout_cancel(&l->ring_retry);
	uloop_timeout_cancel(&l->service_sync);
	/* the abort carries the other two forward disconnect timers, and it is
	 * also where a battery this line removed is put back: a re-init or a
	 * daemon that stops must not leave a loop dead. It is also where the
	 * retry of a refused restore stops, in front of the memset that would
	 * corrupt the list an armed timer is linked into */
	fwd_disc_abort(l, true);
}

/*
 * The boot seize is derived, not awaited. The start-up hook report (the first
 * debounced hook sample of the udsp backend) and the dsp_start completion
 * lines_start() runs on cross one socketpair unordered, so a handset already
 * off hook before the daemon started either seized through line_hal_event()
 * or was dropped by its lines_up gate, and nothing followed the drop to
 * correct it. Reading the state here makes the two orders the same boot: the
 * seize takes the ordinary intake, so the digit detector, the dial tone and
 * the OFFHOOK_DIAL hotline entry all behave as they do for a lifted handset. A
 * report that already seized the line leaves it in OFFHOOK_DIAL, or in
 * CLEARING for a line that cannot carry a dial, and the matrix ignores the
 * second SEIZE in both. Since the machines are wired at start-up, that is the
 * ordinary outcome rather than the lucky one.
 *
 * The second caller is the service entry: lines_reload() runs this for a
 * line it takes into service, which is the same derivation for a line that
 * was out of service when the boot query ran.
 *
 * The accepted failure shape is the common one: a line with no live
 * registration at this instant, because its registrar has not resolved or
 * not answered yet, gets the fast busy and then the howler at the seize itself
 * (hook_event, calls_line_dial_ready), with no dial tone and, on a hotline
 * line, no dial.
 * That is what the subscriber gets for holding the handset up through the
 * bring-up.
 *
 * The read answers in both directions. An on-hook answer for a line the
 * machine believes is off hook injects a RELEASE, and the battery removal
 * window is what makes that reachable at all: the subscriber's own on-hook is
 * dropped inside it, so without this arm a handset put down during a removal
 * leaves the machine in CLEARING, announcing a clear into a dead handset until
 * some later hook edge corrects it. An unconditional on-hook at the end of
 * the removal would hand a handset that is still lifted a spurious seize;
 * this asks the question instead, so each direction answers for itself.
 *
 * The release arm cannot misfire, and the reason is structural rather than a
 * fact about which call sites exist. A machine in an off-hook state got
 * there by consuming a SEIZE report; a SEIZE report is only emitted once the
 * classifier's seize debounce has expired; and past that expiry this very
 * query answers off hook. The two facts the arm needs cannot disagree unless
 * the handset really did go down.
 *
 * The seize arm is the other way around. It exists for the boot and the
 * service entry, where the line is at IDLE and the seize is the whole point;
 * in the states a removal window can be in it is a matrix no-op, because
 * CLEARING and ROH both ignore SEIZE as a duplicate hook report, and
 * only the detector arm in front of it would move.
 *
 * The state test is line_on_hook() above, spelled as its complement: the four
 * states where a SEIZE still means something are exactly the four where the
 * handset is on the cradle, and one table has to serve both readers.
 */
static bool hook_offhook_is(const struct line *l)
{
	return !line_on_hook(l);
}

/*
 * One reading of the hook, with the retry hal.h section 3 asks for. Two
 * readers share it: the service entry above and the resync below. The caller
 * owns the answer and the refusal, so neither carries a retry rule of its own.
 *
 * HAL_TRANSIENT means ask once more. The retry runs in this frame, like
 * hook_timing_push() and detector_arm(). The op is one short call and the
 * shell must not sleep on the uloop thread. The retry carries more weight
 * here than it does there. The service entry reads at the instant the DSP
 * finished its bring-up, which is where a busy answer is most likely. A
 * refusal dropped instead of retried hands that line straight back to the
 * race the query closed.
 */
static enum hal_status hook_query_take(const struct line *l, bool *offhook)
{
	enum hal_status st;

	*offhook = false;
	st = hal_backend_ops.line_hook_query(uv.hal, l->index, offhook);

	if (st == HAL_TRANSIENT)
		st = hal_backend_ops.line_hook_query(uv.hal, l->index, offhook);

	return st;
}

static void hook_state_sync(struct line *l)
{
	enum hal_status st;
	bool offhook;

	if (!uv.hal)
		return;

	st = hook_query_take(l, &offhook);

	if (st != HAL_OK) {
		/* the line keeps the outcome of the race above: a hook state
		 * invented here would be worse than the one nobody read */
		TRACE(TRACE_HOOK, "line %u hook state not read on the service entry: %s",
		      l->index, hal_status_name(st));
		return;
	}

	/* loud rather than traced, like the injected seize of the diagnostic
	 * verb: a log read afterwards has to show where this call came from */
	if (offhook) {
		ULOG_INFO("line %u is off hook on the service entry, seizing\n",
			  l->index);
		inject_hook_event(l, HAL_HOOK_SEIZE);
		return;
	}

	if (!hook_offhook_is(l))
		return;

	ULOG_INFO("line %u is on hook in %s, releasing\n", l->index,
		  line_state_word(l));
	inject_hook_event(l, HAL_HOOK_RELEASE);
}

/* ------------------------------------------------ the resync behind a gap */

/*
 * The hook state read again because the event pump lost records (line/line.h
 * lines_hook_resync). It is one timer for the whole board and it runs in two
 * phases. The read phase queries every line the skip list admits. The confirm
 * phase takes the second reading the release arm needs. hook_resync_phase
 * says which job an expiry is; the ring timer picks its own job from two
 * flags instead. hook_resync_arm() writes the callback on every arm, the rule
 * ring_retry_arm() follows.
 *
 * hook_resync_ms is when the last pass was asked for and hook_resync_asked
 * says one was: together they are the rate limit on lines_hook_resync().
 * hook_resync_lost is uv.events_lost at the first reading, so the confirm can
 * tell that the pump lost more records while it waited.
 */
enum hook_resync_phase {
	HOOK_RESYNC_READ = 0,
	HOOK_RESYNC_CONFIRM,
};

static struct uloop_timeout hook_resync_timer;
static enum hook_resync_phase hook_resync_phase;
static uint32_t hook_resync_ms;
static bool hook_resync_asked;
static uint64_t hook_resync_lost;

static void hook_resync_expire(struct uloop_timeout *t);

static void hook_resync_arm(enum hook_resync_phase phase, unsigned int ms)
{
	hook_resync_phase = phase;
	hook_resync_timer.cb = hook_resync_expire;
	uloop_timeout_set(&hook_resync_timer, (int)ms);
}

/* The daemon stop and the re-entered bring-up. The timer is cancelled in
 * front of the storage it is linked from. The rate limit of a daemon that is
 * gone must not hold against the next one. */
static void hook_resync_drop(void)
{
	uloop_timeout_cancel(&hook_resync_timer);
	hook_resync_phase = HOOK_RESYNC_READ;
	hook_resync_asked = false;
	hook_resync_ms = 0;
	hook_resync_lost = 0;
}

/*
 * The lines this pass leaves alone. An out of service line is the skip
 * lines_start() makes at the boot. Its own SEIZE cell plays the OOS reorder,
 * and the port may not exist at all. A battery removal window drops
 * every hook report it produces (hook_event). A reading taken inside that
 * window describes a loop this shell opened and not a handset.
 *
 * The loop test needs no term of its own. line_serve_want() reads the mode,
 * so every port the test holds is already out of service.
 */
static bool hook_resync_skips(const struct line *l)
{
	return l->oos || l->fwd_disc_down || l->fwd_disc_dead;
}

/*
 * How long the release arm waits for its second reading. The floor is the
 * flash window of the region plus a pad. Two readings inside one flash both
 * answer on hook for a subscriber who never put the handset down.
 *
 * The floor comes from hook_timing_get() and not from the region record
 * beside it. That accessor carries the effective window the hook push and the
 * forward disconnect band both read.
 */
static unsigned int hook_resync_confirm_ms(void)
{
	struct hal_hook_timing ts;
	unsigned int floor_ms;

	hook_timing_get(&ts);
	floor_ms = ts.flash_max_ms + LINE_HOOK_RESYNC_FLASH_PAD_MS;

	if (floor_ms > LINE_HOOK_RESYNC_CONFIRM_MS)
		return floor_ms;

	return LINE_HOOK_RESYNC_CONFIRM_MS;
}

/*
 * The first reading of one line, and what it is allowed to do with it.
 *
 * An answer that agrees with the machine injects nothing. The seize arm of
 * hook_state_sync() injects on every off-hook answer, which arms the digit
 * detector again. That is right for a line entering service. It is wrong
 * under a call this pass runs across.
 *
 * A ringing line reads off hook and is left alone. The answer of the query
 * inside a ring cadence is not verified on hardware, so the answer cannot be
 * trusted there. l->ringing covers the recall ring of a held call too, because
 * holding_enter() starts the bell.
 *
 * An on-hook answer under a machine that is off hook injects nothing yet. It
 * owes a second reading. A flash and a pulse both break the loop open in
 * normal use, and a RELEASE over one of them ends a live call.
 */
static void hook_resync_line_read(struct line *l)
{
	enum hal_status st;
	bool offhook;

	st = hook_query_take(l, &offhook);
	if (st != HAL_OK) {
		TRACE(TRACE_HOOK, "line %u hook state not read behind the "
		      "event gap: %s", l->index, hal_status_name(st));
		return;
	}

	if (offhook == hook_offhook_is(l))
		return;

	if (!offhook) {
		l->hook_resync_wait = true;
		l->hook_resync_activity = l->hook_activity;
		return;
	}

	if (l->ringing) {
		TRACE(TRACE_HOOK, "line %u reads off hook while it rings, left "
		      "alone", l->index);
		return;
	}

	/* loud rather than traced, like the service entry above: a log read
	 * afterwards has to show where this seize came from */
	ULOG_INFO("line %u is off hook behind an event gap, seizing\n",
		  l->index);
	inject_hook_event(l, HAL_HOOK_SEIZE);
}

static void hook_resync_read(void)
{
	unsigned int i;

	hook_resync_lost = uv.events_lost;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct line *l = &lines[i];

		l->hook_resync_wait = false;

		if (hook_resync_skips(l))
			continue;

		hook_resync_line_read(l);
	}
}

static bool hook_resync_waits(void)
{
	unsigned int i;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		if (lines[i].hook_resync_wait)
			return true;
	}

	return false;
}

/*
 * The second reading, and the release it decides. Three conditions gate it:
 *
 * - both readings say on hook;
 * - the machine is still off hook;
 * - the line stayed quiet between the two readings.
 *
 * A hook report or a dialled key in that window says the loop moved. The
 * reading in front of it then describes a moment that is gone.
 */
static void hook_resync_line_confirm(struct line *l)
{
	enum hal_status st;
	bool offhook;

	if (l->hook_activity != l->hook_resync_activity) {
		TRACE(TRACE_HOOK, "line %u moved between the two hook "
		      "readings, no release", l->index);
		return;
	}

	st = hook_query_take(l, &offhook);
	if (st != HAL_OK) {
		TRACE(TRACE_HOOK, "line %u second hook state not read: %s",
		      l->index, hal_status_name(st));
		return;
	}

	if (offhook || !hook_offhook_is(l))
		return;

	ULOG_INFO("line %u is on hook in %s behind an event gap, releasing\n",
		  l->index, line_state_word(l));
	inject_hook_event(l, HAL_HOOK_RELEASE);
}

static void hook_resync_confirm(void)
{
	unsigned int i;

	/*
	 * The pump lost more records while this wait ran. The reading in front
	 * of the wait is therefore older than the state the line is in now.
	 * The whole pass runs again instead, and the second reading it takes
	 * is the one that decides. The rate limit does not hold here: it
	 * guards lines_hook_resync() alone.
	 */
	if (uv.events_lost != hook_resync_lost) {
		hook_resync_arm(HOOK_RESYNC_READ, 0);
		return;
	}

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct line *l = &lines[i];

		if (!l->hook_resync_wait)
			continue;

		l->hook_resync_wait = false;

		if (hook_resync_skips(l))
			continue;

		hook_resync_line_confirm(l);
	}
}

static void hook_resync_expire(struct uloop_timeout *t)
{
	unsigned int tested;

	if (!uv.hal)
		return;

	/*
	 * A metallic line test blocks the hook sample of every port for the
	 * length of its measurement. Both phases therefore wait for it rather
	 * than read a part the test owns. The per line state is kept, so a
	 * wait this postpone carries forward still ends in one second reading.
	 */
	if (line_test_running(&tested)) {
		TRACE(TRACE_HOOK, "hook resync held by the line test on "
		      "line %u", tested);
		hook_resync_arm(hook_resync_phase, LINE_HOOK_RESYNC_MIN_MS);
		return;
	}

	if (hook_resync_phase == HOOK_RESYNC_CONFIRM) {
		hook_resync_confirm();
		return;
	}

	hook_resync_read();

	if (hook_resync_waits())
		hook_resync_arm(HOOK_RESYNC_CONFIRM, hook_resync_confirm_ms());
}

void lines_hook_resync(void)
{
	uint32_t now = uvoice_mono_ms();
	uint32_t next_ms = hook_resync_ms + LINE_HOOK_RESYNC_MIN_MS;

	/* before lines_init() there is no machine to hand a reading to, which
	 * is the guard line_hal_event() takes for a hardware record */
	if (!lines_up)
		return;

	if (hook_resync_asked && uvoice_deadline_ahead(next_ms, now))
		return;

	hook_resync_asked = true;
	hook_resync_ms = now;
	/* Zero, and not the pass this frame is in. event_read() drains the
	 * socket in one loop, so the records queued behind the gap reach the
	 * machines before this reading does. */
	hook_resync_arm(HOOK_RESYNC_READ, 0);
}

/*
 * The predicate the call layer admits against, through host->hal_ready
 * (proto/sip/call.h calls_hal_ready_bind). The shell owns the daemon HAL
 * state, which it already reads for the region load and the media
 * statistics, so the leg layer is handed the answer instead of the state.
 * UVOICE_HAL_READY is the only value that carries a call:
 * UVOICE_HAL_STARTING is the bring-up window between the two halves, and
 * UVOICE_HAL_FAILED is a DSP that never comes back on its own.
 */
bool line_hal_ready(void)
{
	return uv.hal_state == UVOICE_HAL_READY;
}

/*
 * The second predicate the call layer admits against (proto/sip/call.h
 * calls_line_admit_bind), and the shell owns it for the reason it owns the
 * one above. A line the shell holds out of service carries no call: its
 * machine is FSM_OOS, and the FSM_EV_CALL_INCOMING an admitted INVITE would
 * deliver is an event that state ignores, so the caller hears ringback while
 * the port stays silent. The reasons are the ones line_serve_want() names: a
 * port the config disables, a SLIC fault, a dead line, the loop test, no
 * running protocol and a hold of the protocol's own, and a detector that
 * did not arm.
 *
 * A line the board does not carry, and every line before lines_init(), admit
 * nothing.
 */
bool line_call_admit(unsigned int line)
{
	const struct line *l = line_get(line);

	if (!lines_up || !l || !line_exists(line))
		return false;

	return !l->oos;
}

/* ---------------------------------------------- the services of the core */

const struct hal_cap *lines_hal_cap(void)
{
	static const struct hal_cap none;

	return uv.hal ? hal_backend_ops.cap_get(uv.hal) : &none;
}

const struct hal_region *lines_region(void)
{
	return &region_hal;
}

enum hal_status line_tone_play(unsigned int line, enum hal_tone tone,
			       uint32_t duration_ms)
{
	struct line *l = line_get(line);

	if (!l)
		return HAL_FATAL;

	return tone_set_ms(l, tone, duration_ms);
}

void line_tone_stop(unsigned int line)
{
	struct line *l = line_get(line);

	if (l)
		tone_set_ms(l, HAL_TONE_NONE, 0);
}

bool line_fwd_disc_signal(unsigned int line)
{
	struct line *l = line_get(line);

	return l && fwd_disc_on(l) && fwd_disc_apply(l);
}

void line_fwd_disc_abort(unsigned int line)
{
	struct line *l = line_get(line);

	if (l)
		fwd_disc_abort(l, false);
}

/*
 * The loop feed through the forward disconnect primitives, so the window
 * rules of those primitives hold for a protocol that commands the feed: the
 * removal drops the hook reports, and the return runs the settle.
 */
enum hal_status line_battery_set(unsigned int line, enum proto_battery batt)
{
	struct line *l = line_get(line);

	if (!l || !uv.hal)
		return HAL_FATAL;

	switch (batt) {
	case PROTO_BATT_ACTIVE:
		/* the polarity goes back first, so the feed returns forward */
		fwd_disc_forward(l);
		if (l->fwd_disc_dead)
			fwd_disc_revive(l);
		return HAL_OK;
	case PROTO_BATT_DOWN:
		/* fwd_disc_down alone is also the settle after a revive, with
		 * the feed up; only a removed feed makes this a no-op */
		if (l->fwd_disc_dead)
			return HAL_OK;
		return fwd_disc_remove(l) ? HAL_OK : HAL_FATAL;
	case PROTO_BATT_REVERSE:
		if (!(lines_hal_cap()->features & HAL_FEAT_POLARITY))
			return HAL_UNSUPPORTED;
		fwd_disc_reverse(l);
		return HAL_OK;
	}

	return HAL_FATAL;
}

enum hal_status line_stream_stats_get(unsigned int line, unsigned int stream,
				      struct hal_media_stats *out)
{
	if (!line_get(line) || !uv.hal || stream >= HAL_STREAM_MAX ||
	    !hal_backend_ops.stream_stats_get)
		return HAL_UNSUPPORTED;

	return hal_backend_ops.stream_stats_get(uv.hal, line, stream, out);
}

void line_fault(unsigned int line, bool loud, const char *text)
{
	const struct line *l = line_get(line);

	if (l)
		line_fault_report(l, loud, text);
}

const char *line_state_name(unsigned int line)
{
	const struct proto_ops *ops = proto_ops_get();

	if (!lines_up || !line_get(line) || !line_exists(line) ||
	    !ops->state_name)
		return NULL;

	return ops->state_name(line);
}

enum line_rest line_rest_of(unsigned int line)
{
	const struct line *l = line_get(line);

	if (!lines_up || !l || !line_exists(line))
		return LINE_REST_OOS;
	if (line_flags_of(l))
		return LINE_REST_ACTIVE;

	return l->oos ? LINE_REST_OOS : LINE_REST_IDLE;
}

/*
 * The stops the forced OOS entry already made, re-issued on a started DSP.
 * The early half marks a line out of service, and the machine stops its tone
 * and its bell there, but those two ops cross a backend whose DSP has not
 * started and the backend refuses them. The port has to be silent once
 * the channels exist, so they are made again here. Both go through the ops
 * the force itself used, so one line of behavior stays in one place.
 */
static void line_oos_silence(struct line *l)
{
	tone_apply(l, HAL_TONE_NONE);
	line_ringer_stop(l->index);
}

/*
 * The capability of the moment handed to the protocol (ops->dsp_ready), with
 * the one reader this shell keeps: the bit decides what this file does with a
 * wire digit, not the protocol.
 */
static void dsp_ready_push(void)
{
	const struct proto_ops *ops = proto_ops_get();
	const struct hal_cap *cap = lines_hal_cap();

	if (ops->dsp_ready)
		ops->dsp_ready(cap->features, cap->codecs);

	hal_plays_wire_digit = (cap->features & HAL_FEAT_DTMF_PLAYOUT) != 0;
}

/*
 * The bring-up is two halves. This one wires the call layer and needs nothing
 * from the DSP: the timer environment, the record ring, one derived view and
 * one machine per line, the leg layer with its config, the service state and
 * the message waiting presentation. main.c runs it at start-up, before the
 * accounts register, so a call offered to a line inside the bring-up window
 * reaches the admission rule of call.c, which answers 503 while the DSP is
 * not ready. Everything the DSP has to accept first is in lines_start()
 * below.
 */
void lines_init(const struct cfg *cfg)
{
	const struct proto_ops *ops = proto_ops_get();
	unsigned int i;

	line_tests_drop();
	stats_drop();

	/*
	 * The call history starts empty here and nowhere else, and the guard is
	 * the lifecycle itself rather than the single call site in main.c: a
	 * second early half over lines that are already up must not drop a
	 * record or a counter, whoever calls it. Only "voice stats_reset" clears
	 * the counters, and it keeps the ring. The suites re-enter the bring-up
	 * per fixture behind lines_done(), which is what puts the ring back to
	 * empty for them.
	 */
	if (!lines_up)
		cdr_init();

	/* in front of the memset below, which drops the per line half of the
	 * same state */
	hook_resync_drop();

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct line *l = &lines[i];
		struct line_mwi mwi = l->mwi;

		/* the memset below would leave a pending timer linked into a
		 * list from storage that no longer describes it. main.c runs
		 * this half once, so a running daemon reaches it once; the
		 * suites re-enter it per fixture, which is where an armed
		 * clearing timer would actually be left behind */
		line_timers_cancel(l);
		memset(l, 0, sizeof(*l));
		l->index = i;
		/*
		 * The warm init keeps the message. Production runs this once,
		 * at start-up, but a NOTIFY can arrive before it, so a clear
		 * here would drop a message the switch already delivered and
		 * would not send again. The derive of the protocol reads the
		 * restored store, which is what gives the machine the right
		 * dial tone across a re-init.
		 */
		l->mwi = mwi;
		l->cid_format = region_hal.cid_format;
		l->mwi_format = region_hal.mwi_format;
		ring_pattern_base(l->ring_pattern);
		l->chan = cfg->channel[i];
		fwd_disc_check(l, &l->chan);
	}

	/* the RTP port pool of the legs starts empty with the lines */
	net_port_reset();

	/* The protocol takes the host table and the config once the lines are
	 * wired (proto/proto.h ops->init). */
	ops->init(proto_host_get(), cfg);
	/*
	 * The capability is resolved here and handed down, the fwd_disc_check()
	 * pattern: the shell owns the backend handle, and the protocol reads
	 * the answer. This push is the baseline: it comes before the DSP
	 * starts, and lines_start() pushes the capability again on the DSP
	 * start completion. No backend at all carries no optional feature.
	 */
	dsp_ready_push();
	lines_up = true;

	/*
	 * The service state here is provisional. line_in_service() reads the
	 * backend line count before the DSP starts, so lines_start() settles
	 * this again against the count of the started DSP.
	 */
	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct line *l = &lines[i];

		l->oos = !line_serve_want(l, cfg);
		if (!l->oos)
			continue;

		ops->line_service(i, false, PROTO_SERVICE_CFG);
		TRACE(TRACE_FSM, "line %u out of service", i);
	}
}

/*
 * The half the DSP has to accept. main.c runs it from lines_start_once(), on
 * the bring-up completion and when no backend attached at all, so every op
 * below meets a DSP that is either started or never will be; a terminal
 * bring-up failure does not reach it, that daemon exits for the procd
 * respawn. The machines already exist: this loads the country tone table,
 * pushes the hook windows, settles the service state against the capability
 * of the started DSP, silences the ports that are out of service, presents
 * the message waiting records the window could not carry, and reads the boot
 * hook state.
 */
void lines_start(const struct cfg *cfg)
{
	unsigned int i;

	/* the same guard lines_reload() and lines_done() take: there is nothing
	 * to push to before the early half wired the machines */
	if (!lines_up)
		return;

	/*
	 * The capability of the started DSP. This half runs on the dsp_start
	 * completion, so dsp_ready_push() hands the protocol the features it
	 * reads from here on, instead of the baseline lines_init() pushed. It
	 * is the same reason line_service_sync() settles the line count below,
	 * and it is safe against a call in flight: no leg can have built its
	 * view before the daemon reports UVOICE_HAL_READY (proto/sip/call.h
	 * calls_hal_ready_bind).
	 *
	 * The codec mask follows it here for the reason it follows it in the
	 * early half: the offer a leg builds names only the codecs in it. The
	 * account layer takes the mask here for the same reason: the family a
	 * line may register in depends on it.
	 */
	lines_cap_final_push = true;
	dsp_ready_push();
	lines_cap_final_push = false;

	/* this is the first moment the DSP accepts the country tone table; the
	 * read-back of a previous table is dropped with it, so the pushes
	 * below carry the compiled default until the new load answers */
	region_drop();
	region_hal_drop();
	region_applied[0] = 0;

	for (i = 0; i < CFG_CHANNEL_MAX; i++)
		hook_timing_push(&lines[i]);

	/* The battery goes up here, in front of the region submission: that
	 * job holds the backend's control thread for its whole load and every
	 * line state request meanwhile is refused (udsp_guard), which would
	 * cost each line a retry period of no battery on every start. */
	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct line *l = &lines[i];

		l->battery_wanted = line_in_service(cfg, l->index);
		battery_sync(l);
	}

	region_take(cfg);
	audio_tune_take(cfg);

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct line *l = &lines[i];
		bool was_oos = l->oos;

		line_service_sync(l, cfg);

		/* the stops the early half made crossed a DSP that had not
		 * started; a line the reconcile just took out of service made
		 * its own on this one */
		if (l->oos && was_oos)
			line_oos_silence(l);

		/*
		 * A NOTIFY can arrive before this line exists at all, and
		 * also while it exists and its DSP is not started: the switch
		 * registers the daemon as soon as the network is up, and the
		 * DSP reports ready later (the e2e flow does exactly that).
		 * Surviving the bring-up is not enough, because the switch does
		 * not send that message again; this is the first moment the
		 * line can carry one. Each of those records is held, and the
		 * start replays a held record only. A line that the sync above
		 * took into service already replayed it in its drain, and an
		 * out of service line presents when it enters service. A held
		 * clear is not replayed here: it waits for the next rest, and
		 * line_settle() presents it there.
		 */
		if (l->mwi.on)
			mwi_replay(l);

		/*
		 * Behind the presentation: a view that the final capability
		 * landed (the protocol's conference settle) lands its core half
		 * here and ends in the settle of this line, which replays a
		 * record held before the DSP was ready.
		 */
		if (l->start_settle) {
			l->start_settle = false;
			line_chan_land(l);
			line_settle(l->index);
		}
	}

	/*
	 * Last, and after the service state of every line is settled. An OOS
	 * line is skipped: the port may not exist at all, and the reorder its
	 * own SEIZE cell plays answers a hook edge the subscriber makes,
	 * not a state the daemon read at start-up.
	 */
	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct line *l = &lines[i];

		if (l->oos)
			continue;

		hook_state_sync(l);
	}
}

void lines_reload(const struct cfg *cfg)
{
	const struct proto_ops *ops = proto_ops_get();
	unsigned int i;

	if (!lines_up)
		return;

	/* region_apply is idempotent, so this submits only on a real change */
	region_take(cfg);
	audio_tune_take(cfg);

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct line *l = &lines[i];
		bool was_oos = l->oos;

		l->chan_new = cfg->channel[i];
		fwd_disc_check(l, &l->chan_new);
		/* the CID format and the region cadence table land with the
		 * channel copy they belong to, so a ring in progress keeps the
		 * format and the cadence it started with */
		l->cid_format_new = region_hal.cid_format;
		l->mwi_format_new = region_hal.mwi_format;
		ring_pattern_base(l->ring_pattern_new);
		l->chan_staged = true;

		/* the core copy is staged in front of the service sync, so a
		 * held presentation waits for it to land (line_settle) */
		line_service_sync(l, cfg);

		if (ops->line_cfg_changed)
			ops->line_cfg_changed(i);
		else
			line_cfg_landed(i);

		/*
		 * Entering service re-derives the hook state too. A line that
		 * came up out of service with the handset already lifted, and
		 * that this reload takes into service, otherwise lands in IDLE
		 * with the hook desynced: the class the boot query closes,
		 * reached through a config move instead of through the
		 * channel start race. The same edge is reached without a config
		 * move at all: a detector arm the DSP refused forces the line
		 * out of service, and the next reload that finds it servable
		 * brings it back, which is where this query heals that class.
		 * Only the edge into service carries it; a line leaving service
		 * answers its own SEIZE with the OOS reorder, and one whose
		 * service did not move has nothing to re-derive. It runs behind
		 * the drain, so the seize meets a line the reload has finished
		 * moving.
		 */
		if (was_oos && !l->oos)
			hook_state_sync(l);
	}
}

void lines_done(void)
{
	unsigned int i;

	if (!lines_up)
		return;

	/* before the force below, so the ring stop and the loop clear of the
	 * mode cross a backend that is still attached */
	lines_loop_test_stop();

	/* the board timer, beside the per line ones the loop below cancels */
	hook_resync_drop();

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct line *l = &lines[i];

		/* the protocol ends every leg of the line and forces it out,
		 * here and not later: the legs stop their media before
		 * hal_close(). The forced state is a rest state, so the settle
		 * inside it puts the polarity back and closes any battery
		 * removal; the cancel below is the second half */
		l->oos = true;
		proto_ops_get()->line_service(i, false, PROTO_SERVICE_CFG);
		line_timers_cancel(l);
	}

	region_drop();
	region_hal_drop();
	line_tests_drop();
	/* after the legs are torn down, because their teardown is what submits
	 * the last media snapshots */
	stats_drop();
	lines_up = false;
}

/*
 * The loop test record of one port, present only while the mode holds it. A
 * line that reads oos with no other reason is otherwise indistinguishable
 * from a port the config disabled, and the three flags are what an operator
 * at the other end of the phone needs: where the handset is, whether the bell
 * runs and whether the audio path is armed.
 */
static void loop_test_dump(struct blob_buf *b, const struct line *l)
{
	unsigned int peer;
	void *table;

	if (!l->loop_test)
		return;

	table = blobmsg_open_table(b, "loop_test");
	blobmsg_add_string(b, "mode",
			   loop_test_mode_of(l, &peer) == HAL_LOOP_SELF ?
			   "self" : "cross");
	blobmsg_add_u8(b, "offhook", l->loop_test_offhook);
	blobmsg_add_u8(b, "ringing", l->loop_test_ringing);
	blobmsg_add_u8(b, "audio", l->loop_test_audio);
	blobmsg_close_table(b, table);
}

static void region_ring_dump(struct blob_buf *b, const struct hal_region *r)
{
	void *arr, *entry, *cad, *pair;
	unsigned int i, j;

	arr = blobmsg_open_array(b, "ring");
	for (i = 0; i < HAL_REGION_RING_MAX; i++) {
		const struct hal_region_ring *t = &r->ring[i];

		if (!t->cadence_count)
			continue;

		entry = blobmsg_open_table(b, NULL);
		blobmsg_add_u32(b, "template", i);
		cad = blobmsg_open_array(b, "cadence");
		for (j = 0; j < t->cadence_count; j++) {
			pair = blobmsg_open_table(b, NULL);
			blobmsg_add_u32(b, "on_ms", t->cadence[j].on_ms);
			blobmsg_add_u32(b, "off_ms", t->cadence[j].off_ms);
			blobmsg_close_table(b, pair);
		}
		blobmsg_close_array(b, cad);
		blobmsg_close_table(b, entry);
	}
	blobmsg_close_array(b, arr);
}

void lines_region_status_dump(struct blob_buf *b)
{
	const struct hal_region *r = &region_hal;
	void *table;

	table = blobmsg_open_table(b, "region");
	blobmsg_add_string(b, "requested", region_want);
	blobmsg_add_string(b, "applied", region_applied);
	blobmsg_add_string(b, "source", region_hal_read ? "backend" : "default");
	blobmsg_add_string(b, "iso3", r->iso3);
	blobmsg_add_string(b, "describe", r->describe);
	blobmsg_add_string(b, "cid_format", hal_cid_format_name(r->cid_format));
	blobmsg_add_string(b, "mwi_format", hal_cid_format_name(r->mwi_format));
	blobmsg_add_u32(b, "hook_flash_min_ms", r->hook_flash_min_ms);
	blobmsg_add_u32(b, "hook_flash_max_ms", r->hook_flash_max_ms);
	blobmsg_add_u32(b, "hook_release_min_ms", r->hook_release_min_ms);
	blobmsg_add_u32(b, "cw_sas_total_ms", r->cw_sas_total_ms);
	blobmsg_add_string(b, "flash_service",
			   hal_flash_service_name(r->flash_service));
	blobmsg_add_u8(b, "pulse_invert", r->pulse_invert);
	blobmsg_add_u32(b, "confirm_total_ms", r->confirm_total_ms);
	blobmsg_add_u8(b, "confirm_endless", r->confirm_endless);
	region_ring_dump(b, r);
	blobmsg_close_table(b, table);
}

/*
 * The core keys of one call[] entry, then the protocol's own
 * (ops->status_dump_call).
 */
void lines_status_dump(struct blob_buf *b)
{
	const struct proto_ops *ops = proto_ops_get();
	void *arr, *entry;
	unsigned int i;

	arr = blobmsg_open_array(b, "call");
	/* before lines_init() there are no machines, and reporting a zeroed
	 * struct as an idle line would be a lie */
	for (i = 0; lines_up && i < CFG_CHANNEL_MAX; i++) {
		struct line *l = &lines[i];

		entry = blobmsg_open_table(b, NULL);
		blobmsg_add_u32(b, "line", i);
		blobmsg_add_u8(b, "oos", l->oos);
		/* present while either hardware reason holds the line out, the
		 * SLIC fault or the dead line. One operator key covers both;
		 * the split into two words is a later item. A reload derives
		 * the config reasons again, and neither of these is a config
		 * value, so a reload does not move them */
		if (l->fault || l->down)
			blobmsg_add_u8(b, "fault", 1);
		blobmsg_add_u8(b, "ringing", l->ringing);
		/* present only while a line test holds the bell back, which is
		 * the one state where a line reads as ringing and the phone is
		 * quiet */
		if (l->ring_held)
			blobmsg_add_u8(b, "ring_held", 1);
		/* what the switch said, not what either presentation gate did
		 * with it: the store is the state, amwi and vmwi are policy */
		blobmsg_add_u8(b, "mwi", l->mwi.on);
		blobmsg_add_u32(b, "mwi_new", l->mwi.new_msgs);
		blobmsg_add_u32(b, "mwi_old", l->mwi.old_msgs);
		/* the forward disconnect state, each key present only while it
		 * describes something: the reversal the line is holding. It is
		 * not a uci value, so this dump is the only place it can be
		 * read, which is also what a field engineer needs when a PBX
		 * behind the port does not release */
		if (l->fwd_disc_reversed)
			blobmsg_add_u8(b, "fwd_disc_reversed", 1);
		/* the removal window, which is the one that also silences the
		 * hook intake: a line that answers no hook edge at all is what
		 * a field engineer would otherwise have to guess at */
		if (l->fwd_disc_down)
			blobmsg_add_u8(b, "fwd_disc_down", 1);
		loop_test_dump(b, l);
		ops->status_dump_call(b, i);
		blobmsg_close_table(b, entry);
	}
	blobmsg_close_array(b, arr);
}

