// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Analog line control: hook FSM, ring cadence engine, CID sequencer and the
 * region file parser. This header is private. The stable contracts are the
 * include/udsp headers and dsp/cid.h; this header is the interface of the four
 * line modules, and the engine is its only caller.
 *
 *
 * 1. Pure state machines. No thread, no clock, no allocation after init, no
 * lock and no log lives here. Every machine takes a tick with a caller-supplied
 * monotonic millisecond stamp and returns fixed-size records in a struct
 * line_out journal. The caller executes the records: SLIC actions,
 * tone-generator programs, FSK arming through dsp/cid.h, DTMF playout, and
 * udsp_event records it forwards to the event emitter. The journal is one
 * ordered list, so the record order is also the execution order.
 *
 *
 * 2. Tick order. Mandatory. Per line and per 10 ms tick the caller runs:
 *
 *	rep = line_hook_tick(&hook, now_ms, raw_offhook);
 *	line_ring_tick(&ring, now_ms, &rep, &gate, &out);
 *	line_cidseq_tick(&cid, now_ms, &rep, &gate, &out);
 *	line_hook_emit(&rep, line, now_ms, &out);
 *
 * line_hook_tick classifies but emits nothing, so the ring machine sees the
 * off-hook edge first. A ring trip therefore puts UDSP_EV_RING_END in the
 * journal before the UDSP_EV_HOOK seize, which is the order the core expects on
 * an answer. line_hook_emit closes the tick by appending the hook events.
 *
 *
 * 3. Ring to sequencer handshake. struct line_cid_gate is owned by the caller
 * and shared by the two machines, so neither reaches into the other. The ring
 * machine raises request at the CID point and holds the break. The sequencer
 * runs and raises complete. The ring machine sees complete on the next tick and
 * resumes. That resume costs one tick, 10 ms, against a break measured in
 * seconds, and it keeps the two modules independent.
 *
 *
 * 4. Timing boundaries. Every window is inclusive at both ends. An open of
 * exactly flash_max_ms is a flash; one millisecond more is release-pending. A
 * break of exactly pulse_break_min_ms or pulse_break_max_ms is a pulse break. A
 * type 2 acknowledgement stamped at exactly ack_timeout_ms is still inside the
 * window. Durations are measured from the raw edge, not from the debounced
 * edge, so the debounce window does not shorten a measured interval. The break
 * window and the flash window may overlap; see line_hook_pulse_enable.
 */
#ifndef UDSP_LINE_H
#define UDSP_LINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "udsp/event.h"
#include "udsp/udsp.h"
#include "udsp/slic.h"

#include "../dsp/cid.h"
#include "../dsp/dsp.h"

#define UDSP_REGION_TONE_MAX	((unsigned)UDSP_TONE_NUMBER_UNOBTAINABLE + 1u)
#define UDSP_REGION_RING_MAX	8u
#define UDSP_CW_VARIANT_MAX	4u	/* distinctive call-waiting rows c1..c4 */
#define UDSP_REGION_SEG_MAX	UDSP_TONE_CADENCE_MAX
#define UDSP_REGION_NAME_MAX	64u
#define UDSP_REGION_FILE_MAX	16384u	/* 16 KiB */
#define UDSP_REGION_LINE_MAX	200u

/*
 * CID signalling constants the region grammar has no key for; the grammar is
 * fixed. Every constant carries its source. A value the source states as a
 * range is set at the middle of that range.
 */
#define LINE_CAS_LOW_HZ		2130	/* GR-30-CORE / EN 300 659-1 CAS, DT-AS */
#define LINE_CAS_HIGH_HZ	2750
#define LINE_CAS_MS		80	/* GR-30-CORE CAS 80..85 ms */
#define LINE_CAS_LEVEL_DBM0	-17
/* Defaults for the [cid] keys the region file may omit. */
#define LINE_DTAS_MS		100	/* EN 300 659-1 DT-AS 88..110 ms */
#define LINE_RPAS_MS		250	/* EN 300 659-1 RP-AS pulse 200..300 ms */
#define LINE_LR_PAUSE_MS	250	/* SIN 227 line reversal to DT-AS gap */
/*
 * What a pre-message pause adds when a relay-off starts it: a ring burst that
 * ends the break, or the RP-AS pulse. The sequencer times the pause from the
 * relay-off record, but the part ramps out of ringing after that write, and
 * the slic thread runs the write up to a late tick after the engine queued it.
 * One 20 Hz ring cycle covers both. Reason: [cid] wait_ms is the standard's
 * minimum from the end of ringing on the line (GR-30-CORE type 1: 500 ms), and
 * a burst that ran one cycle long put the FSK 482 ms after it on MT7988.
 */
#define LINE_RING_TAIL_MS	50
#define LINE_CID_FSK_LEVEL_DBM0	-14	/* GR-30-CORE -13.5 dBm0, int8 dBm0 step */
#define LINE_CID2_SEIZURE_BITS	0	/* type 2 sends no channel seizure */
#define LINE_CID2_MARK_BITS	80
#define LINE_CID_DTMF_ON_MS	70	/* EN 300 659-1 DTMF CID digit timing */
#define LINE_CID_DTMF_OFF_MS	70
#define LINE_CID_DTMF_LEVEL_DBM0	-10
#define LINE_CID_DTMF_MAX	40u
/*
 * Liveness cap on a CID window, and nothing else. A break held open by a
 * sequencer that never completes would stop the ring for good, so the ring
 * machine gives up here. It is not a caller-supplied timing. udsp_ring
 * cid_wait_ms is the delay from the break to the burst, a phase inside this
 * window. As the window bound, it would close the break on the tick the
 * sequencer leaves LINE_CID_WAIT.
 *
 * line_cidseq_budget_ms() keeps the cap honest: a sequence whose worst case
 * does not fit here is refused at arm rather than truncated mid-burst.
 */
#define LINE_CID_WINDOW_MAX_MS	5000u

/*
 * The largest call-setup message udsp_cid_msg_build can emit: two header
 * bytes, the date parameter, a full number and a full name each behind a
 * marker and a length, and the checksum. The budget must never understate a
 * burst, so it counts this bound rather than the record in hand.
 */
#define LINE_CID_MSG_MAX_BYTES	(2u + 2u + 8u + 2u + (UDSP_CID_STR_MAX - 1u) \
				 + 2u + (UDSP_CID_STR_MAX - 1u) + 1u)
/* Tick quantisation, plus the one tick the ring-to-sequencer handshake costs
 * (section 3), plus room for the phase boundaries to fall off a tick edge. */
#define LINE_CID_BUDGET_MARGIN_MS	200u

/*
 * Distinctive call-waiting variant bounds. The subscriber alerting signal a
 * [cw] row draws must sit inside the GR-30-CORE X window, and its trailing
 * break must clear the whole type 2 exchange so the next presentation never
 * starts during the CAS, the acknowledgement and the FSK.
 */
#define LINE_CW_SAS_MIN_MS	250u	/* GR-30-CORE X 250..1000 ms */
#define LINE_CW_SAS_MAX_MS	1000u
/*
 * A full type 2 MDMF FSK burst at 1200 baud plus the Q transition headroom:
 * LINE_CID_MSG_MAX_BYTES bytes at 10 bits each is near 700 ms, and 900 ms
 * covers that and the mark preamble. A [cw] row's trailing break must clear
 * this plus the region's CAS, wait and acknowledgement windows, so the next
 * presentation never starts during the exchange (the API tone resumes while
 * the acknowledgement window is open).
 */
#define LINE_CW_FSK_BURST_MS	900u

/* --- region model; the grammar is fixed --- */

/* Every frequency of a segment plays at level_dbm0. freq_count 0 is a silent
 * segment, the grammar's "off". */
struct udsp_region_segment {
	uint16_t freq_hz[UDSP_TONE_FREQ_MAX];
	uint8_t freq_count;
	int8_t level_dbm0;
	uint16_t duration_ms;
};

/*
 * A tone is either the simple form (freq/level/cadence) or the segment form
 * (ordered segments plus a trailing silence). repeat_count 0 is "forever";
 * desc.repeat mirrors it for direct use with the tone generator, which has no
 * finite-count field, so a count above 1 is the caller's to honour.
 * then_steady is the "then = on" continuation: once the finite repeat count
 * runs out, the tone continues steadily on the last frequency set, or on the
 * last segment, which the parser requires to play. That is how a stutter dial
 * tone becomes a plain dial tone. then_steady needs a finite count, and the
 * caller honours it with repeat_count.
 */
struct udsp_region_tone {
	bool present;
	bool segmented;
	bool then_steady;
	struct udsp_tone_desc desc;
	uint8_t segment_count;
	struct udsp_region_segment segment[UDSP_REGION_SEG_MAX];
	uint16_t silence_ms;
	uint16_t repeat_count;
};

struct udsp_region_ring {
	char name[4];
	struct udsp_cadence cadence[UDSP_CADENCE_MAX];
	struct udsp_cadence first[UDSP_CADENCE_MAX];
	uint8_t cadence_count;
	uint8_t first_count;
};

/* dtas_ms, rpas_ms and lr_pause_ms are optional keys; a zero means the file
 * omitted them and the LINE_*_MS default applies. mwi_format is optional too,
 * but the parser folds an omitted key to format, so a parsed region always
 * carries a valid value. */
struct udsp_region_cid {
	uint8_t format;			/* enum udsp_cid_format */
	uint8_t mwi_format;		/* enum udsp_cid_format */
	uint16_t wait_ms;
	uint16_t ack_timeout_ms;
	uint16_t seizure_bits;
	uint16_t mark_bits;
	uint16_t dtas_ms;
	uint16_t rpas_ms;
	uint16_t lr_pause_ms;
};

struct udsp_region_hook {
	bool pulse_enable;		/* the region default, off when omitted */
	bool pulse_invert;		/* tags each train, off when omitted */
	uint16_t debounce_ms;
	uint16_t flash_min_ms;
	uint16_t flash_max_ms;
	uint16_t release_min_ms;
	uint16_t pulse_break_min_ms;
	uint16_t pulse_break_max_ms;
	uint16_t pulse_make_max_ms;
	uint16_t pulse_interdigit_ms;
};

struct udsp_region {
	char iso3[UDSP_ISO3_MAX];
	char describe[UDSP_REGION_NAME_MAX];
	uint8_t flash_service;		/* enum udsp_flash_service, read-back only */
	struct udsp_region_tone tone[UDSP_REGION_TONE_MAX];
	struct udsp_region_ring ring[UDSP_REGION_RING_MAX];
	uint8_t ring_count;
	struct udsp_region_tone cw[UDSP_CW_VARIANT_MAX];	/* c1..c4, cN is cw[N-1] */
	uint8_t cw_count;
	struct udsp_region_cid cid;
	struct udsp_region_hook hook;
};

struct line_region_error {
	unsigned line;
	char text[96];
};

/*
 * Parse-all-then-swap: *out is written only when the whole text is valid, so a
 * failed load never leaves a half-applied region behind. Returns 0, -EINVAL
 * with err filled, or -EFBIG when the text exceeds UDSP_REGION_FILE_MAX.
 */
int line_region_parse(const char *text, size_t len, struct udsp_region *out,
		      struct line_region_error *err);
/* The same over a file; also returns the negative errno of the failed open. */
int line_region_load(const char *path, struct udsp_region *out,
		     struct line_region_error *err);
const char *line_region_tone_name(enum udsp_tone tone);
const char *line_region_flash_service_name(enum udsp_flash_service service);
/*
 * The first-presentation length of a simple call-waiting cadence: the sum of
 * the make and break pairs less the trailing break. This is the point after
 * which the type 2 CAS may inject, so the caller defers the CAS by this plus a
 * gap. 0 for a segmented, continuous or absent tone.
 */
uint32_t line_region_cw_lead_ms(const struct udsp_region_tone *t);
/*
 * The whole length of a finite tone: one cycle times repeat_count. A cycle is
 * the make and break pairs, or the segments plus the trailing silence. The
 * engine stops the tone at that bound (chan.c playout_tone_start limit_ms,
 * playout_segments_advance cycles_left), so a caller's offer bound and the
 * engine's stop agree. 0 for an absent tone, a tone that repeats forever and a
 * then = on tone: those bound nothing.
 */
uint32_t line_region_tone_total_ms(const struct udsp_region_tone *t);

/* --- output journal --- */

enum line_cmd_type {
	LINE_CMD_NONE = 0,
	LINE_CMD_LINEFEED,
	LINE_CMD_POLARITY,
	LINE_CMD_RING_RELAY,
	LINE_CMD_TONE,
	LINE_CMD_TONE_STOP,
	LINE_CMD_FSK,
	LINE_CMD_DTMF,
	LINE_CMD_MUTE,
};

struct line_cmd {
	uint8_t type;			/* enum line_cmd_type */
	union {
		/* state is an enum udsp_linefeed. restore marks the write that
		 * ends a caller identity sequence: the engine then writes its
		 * current base feed, which a line state request may have moved
		 * while the sequence owned the line, and state carries the feed
		 * the sequence armed with. */
		struct { uint8_t state; bool restore; } linefeed;
		struct { uint8_t state; } polarity;	/* enum udsp_polarity */
		struct { bool on; } relay;
		struct { struct udsp_tone_desc desc; uint16_t duration_ms; } tone;
		struct {
			uint8_t std;			/* enum udsp_fsk_std */
			int8_t level_dbm0;
			uint16_t seizure_bits;
			uint16_t mark_bits;
			uint16_t len;
			uint8_t bytes[UDSP_CID_MSG_MAX];
		} fsk;
		struct {
			char digits[LINE_CID_DTMF_MAX];
			uint8_t count;
			int8_t level_dbm0;
			uint16_t on_ms;
			uint16_t off_ms;
		} dtmf;
		/* Mute silences the far-end media in both directions, uplink
		 * toward the network and downlink into the phone, for its
		 * duration. Local playout (CAS, FSK, tones) is not muted, so a
		 * type 2 exchange is clean both ways. */
		struct { bool on; } mute;
	} u;
};

enum line_rec_kind { LINE_REC_CMD = 0, LINE_REC_EVENT };

struct line_rec {
	uint8_t kind;			/* enum line_rec_kind */
	uint32_t stamp_ms;
	union {
		struct line_cmd cmd;
		struct udsp_event event;
	} u;
};

#define LINE_OUT_MAX	12u

/*
 * sink absorbs an overflow so no producer needs a NULL check on every push; the
 * drop is counted instead. One tick of the worst case (a ring trip that aborts
 * a running type 2 sequence) fits in LINE_OUT_MAX with room to spare.
 */
struct line_out {
	struct line_rec rec[LINE_OUT_MAX];
	struct line_rec sink;
	unsigned count;
	unsigned dropped;
};

static inline void line_out_reset(struct line_out *out)
{
	out->count = 0;
	out->dropped = 0;
}

static inline struct line_rec *line_out_push(struct line_out *out, uint8_t kind,
					     uint32_t now_ms)
{
	struct line_rec *rec = &out->sink;

	if (out->count < LINE_OUT_MAX)
		rec = &out->rec[out->count++];
	else
		out->dropped++;
	memset(rec, 0, sizeof(*rec));
	rec->kind = kind;
	rec->stamp_ms = now_ms;
	return rec;
}

static inline struct line_cmd *line_out_cmd(struct line_out *out, uint32_t now_ms,
					    enum line_cmd_type type)
{
	struct line_rec *rec = line_out_push(out, LINE_REC_CMD, now_ms);

	rec->u.cmd.type = (uint8_t)type;
	return &rec->u.cmd;
}

static inline struct udsp_event *line_out_event(struct line_out *out, uint32_t now_ms,
						unsigned line, enum udsp_event_type type)
{
	struct line_rec *rec = line_out_push(out, LINE_REC_EVENT, now_ms);

	rec->u.event.type = (uint16_t)type;
	rec->u.event.line = (uint16_t)line;
	rec->u.event.stamp_ms = now_ms;
	return &rec->u.event;
}

/* --- hook FSM (line/hook.c) --- */

enum line_hook_stage {
	LINE_HOOK_INIT = 0,	/* before the first debounce completes */
	LINE_HOOK_IDLE,		/* debounced on-hook, no seizure */
	LINE_HOOK_SEIZED,	/* debounced off-hook */
	LINE_HOOK_OPEN,		/* debounced on-hook while seized, being measured */
};

#define LINE_HOOK_REPORT_MAX	2u

/* The class close_apply gives an open it measured (hook.c, "Windows"). */
enum line_hook_open_class {
	LINE_HOOK_OPEN_NONE = 0,	/* no window claims it */
	LINE_HOOK_OPEN_FLASH,
	LINE_HOOK_OPEN_PULSE,		/* a break counted into a train */
};

/*
 * One tick can carry two classifications: an interrupted pulse train flushes
 * its digit at the moment the open outgrows a valid break, and the same open
 * can then resolve to a flash or a release on a coarse tick.
 */
struct line_hook_report {
	uint8_t count;
	struct {
		uint8_t state;		/* enum udsp_hook */
		uint8_t pulses;
	} ev[LINE_HOOK_REPORT_MAX];
	bool offhook;
	bool offhook_edge;
	bool onhook_edge;
	bool boot;
	/* The decadic coding of the timing the machine counts under, which
	 * line_hook_emit puts on a UDSP_HOOK_PULSE event alone. */
	bool pulse_invert;
	/* The open the tick closed off a seized line, when it closed one: its
	 * two bounds, the sampler gap of each edge that the minimum credits,
	 * and the class it took (hook.c close_apply). The engine's hook trace
	 * reads it; the machine keeps no copy. */
	bool measured;
	uint8_t open_class;		/* enum line_hook_open_class */
	uint16_t open_gap_ms;
	uint16_t close_gap_ms;
	uint32_t open_lo_ms;
	uint32_t open_hi_ms;
};

struct line_hook {
	struct udsp_region_hook timing;
	uint8_t stage;			/* enum line_hook_stage */
	bool pulse_enable;
	bool raw;
	bool stable;
	bool primed;			/* the first raw sample was taken */
	bool started;			/* the boot state was reported */
	/* A gap fell inside a pulse train: every digit of the train is
	 * dropped until the train ends at the interdigit timeout or a
	 * release (hook.c, "Gaps"). */
	bool poisoned;
	uint8_t pulses;
	/* The sampler's gap on the sample that found the current raw level:
	 * the edge happened up to that long before its stamp (hook.c
	 * raw_edge_ms). */
	uint16_t raw_gap_ms;
	/* The same gap on the sample that found the on-hook level of the open
	 * being measured: reported with the open for the engine's hook trace,
	 * not credited by the flash window's minimum (hook.c close_apply). */
	uint16_t open_gap_ms;
	bool raw_stall;			/* that sample carried the STALL mark */
	uint32_t raw_since_ms;
	uint32_t open_ms;		/* raw edge that started the current open */
	uint32_t make_since_ms;		/* raw edge that started the current make */
};

/* A NULL timing takes the built-in defaults, which leave pulse classification
 * off and the decadic coding direct; a region file sets its own values through
 * [hook] pulse_enable and pulse_invert. */
void line_hook_init(struct line_hook *h, const struct udsp_region_hook *timing);
/*
 * Pulse-break classification on or off. Init takes the pulse_enable of its
 * timing. The break window and the register-recall window overlap in most of
 * Europe (EN 300 001 accepts a recall of 50..130 ms while a loop-disconnect
 * break runs 42..94 ms; BT SIN 350 recalls on 53..103 ms against a 67 ms
 * break), so the two cannot both be live on one line. The region decides which
 * one is live. The engine turns this on only where the active region's [hook]
 * pulse_enable is on, and UDSP_DET_PULSE can only turn it off (chan.c
 * chan_hook_pulse_apply). While it is on, the break window wins an overlap;
 * while it is off, the flash window classifies an open in the overlap.
 */
void line_hook_pulse_enable(struct line_hook *h, bool on);
/* The udsp_line_hook_timing op: the three core-supplied windows override the
 * region values, the pulse and debounce windows stay regional. */
void line_hook_timing_set(struct line_hook *h, uint32_t flash_min_ms,
			  uint32_t flash_max_ms, uint32_t release_min_ms);
/* GAP: the sampler saw the level unchanged while the part recorded a
 * transition, an even count inside one sample interval. LOST: a sample record
 * before this one was dropped, so the raw level the machine held may be
 * stale. STALL: the sampler took this sample later than its cadence, so the
 * level went unconfirmed for longer than a cadence leaves it; only the
 * sampler can tell a stall from its own idle cadence, whose stamp gap looks
 * the same. All three are the caller's evidence that a train measured across
 * this record is wrong (hook.c, "Gaps"). */
#define LINE_HOOK_SAMPLE_GAP	(1u << 0)
#define LINE_HOOK_SAMPLE_LOST	(1u << 1)
#define LINE_HOOK_SAMPLE_STALL	(1u << 2)
/* IRQ: the sampler took this sample because the part's interrupt edge woke
 * it, so the edge the sample found lies within the interrupt latency before
 * the stamp, not anywhere inside gap_ms. Carried for the engine's hook
 * trace; the machine reads such a sample as a tick sample. */
#define LINE_HOOK_SAMPLE_IRQ	(1u << 3)

/*
 * What the caller knows about its sampler on this tick. known_ms is the last
 * instant the raw level was confirmed by a sample; the machine measures no
 * open or make past it, so a sampler that stalls freezes the machine instead
 * of feeding it a level it never confirmed. gap_ms is the time between the
 * sample that produced this tick's level and the sampler's previous sample,
 * 0 on a tick that carries no sample. flags are the LINE_HOOK_SAMPLE bits. A
 * NULL sample is an ideal sampler: the level is known to now_ms and carries
 * no gap, the shape the simulations drive.
 */
struct line_hook_sample {
	uint32_t known_ms;
	uint16_t gap_ms;
	uint8_t flags;
};

struct line_hook_report line_hook_tick(struct line_hook *h, uint32_t now_ms,
				       bool raw_offhook,
				       const struct line_hook_sample *sample);
void line_hook_emit(const struct line_hook_report *rep, unsigned line,
		    uint32_t now_ms, struct line_out *out);

/* --- ring cadence engine (line/ring.c) --- */

enum line_ring_stage {
	LINE_RING_OFF = 0,
	LINE_RING_SILENCE,	/* a break, or the zero-length break before burst 1 */
	LINE_RING_BURST,
};

/* Carried in UDSP_EV_RING_END u.line.state; the core may ignore it. */
enum line_ring_reason {
	LINE_RING_STOPPED = 0,
	LINE_RING_ANSWERED,
	LINE_RING_EXPIRED,
	LINE_RING_RESTARTED,	/* line_ring_start over a live session */
	LINE_RING_DEAD,		/* the line died under the session */
};

struct line_cid_gate {
	bool request;		/* ring machine: a CID window is open */
	bool complete;		/* sequencer: the window is done */
	/* ring machine: the window opened in the break after a burst, so the
	 * relay went off on the tick it opened (LINE_RING_TAIL_MS) */
	bool after_burst;
};

struct line_ring {
	struct udsp_ring cfg;
	unsigned line;
	uint8_t stage;		/* enum line_ring_stage */
	uint8_t index;
	bool first_cycle;
	bool relay_on;
	bool cid_done;
	bool cid_open;
	uint16_t bursts;
	uint16_t phase_len_ms;
	uint32_t phase_ms;
	uint32_t start_ms;
	uint32_t cid_open_ms;
};

void line_ring_init(struct line_ring *r, unsigned line);
/*
 * -EINVAL on an empty or oversized cadence table, and then the running session
 * is untouched. Over a live session, a valid start ends that session cleanly
 * first (relay off, UDSP_EV_RING_END with LINE_RING_RESTARTED) and only then
 * begins the new one. Every relay-on record therefore keeps its matching off,
 * and no session vanishes.
 */
int line_ring_start(struct line_ring *r, const struct udsp_ring *cfg, uint32_t now_ms,
		    struct line_cid_gate *gate, struct line_out *out);
void line_ring_stop(struct line_ring *r, uint32_t now_ms,
		    enum line_ring_reason reason, struct line_cid_gate *gate,
		    struct line_out *out);
void line_ring_tick(struct line_ring *r, uint32_t now_ms,
		    const struct line_hook_report *hook, struct line_cid_gate *gate,
		    struct line_out *out);
bool line_ring_active(const struct line_ring *r);

/* --- CID sequencer (line/cidseq.c) --- */

enum line_cid_step {
	LINE_CID_IDLE = 0,
	LINE_CID_PENDING,	/* armed, waiting for the ring machine's window */
	LINE_CID_RP_PULSE,	/* etsi-rpas ring pulse */
	LINE_CID_LR_PAUSE,	/* lr-dtas gap between reversal and DT-AS */
	LINE_CID_ALERT,		/* DT-AS, or the type 2 CAS */
	LINE_CID_WAIT,		/* pre-message pause, region wait_ms */
	LINE_CID_ACK,		/* type 2 acknowledgement window */
	LINE_CID_SEND,		/* FSK burst or DTMF string in flight */
};

/*
 * Message selection. LINE_CID_MWI_NONE renders the caller identity through
 * udsp_cid_msg_build. LINE_CID_MWI_ON and LINE_CID_MWI_OFF render a visual
 * message-waiting indicator through udsp_cid_vmwi_build, which needs the
 * explicit state because struct udsp_cid carries no off. A VMWI burst always
 * uses the type 1 transport of the region's format.
 */
enum line_cid_mwi {
	LINE_CID_MWI_NONE = 0,
	LINE_CID_MWI_OFF,
	LINE_CID_MWI_ON,
};

struct line_cidseq {
	unsigned line;
	uint8_t step;			/* enum line_cid_step */
	uint8_t kind;			/* enum udsp_cid_kind */
	uint8_t format;			/* enum udsp_cid_format */
	uint8_t mwi;			/* enum line_cid_mwi */
	uint8_t restore_lf;		/* enum udsp_linefeed */
	bool wait_ring;
	bool reversed;
	bool muted;
	bool oht_set;
	bool relay_on;
	bool audio_on;
	bool burst_done;
	bool ack_seen;
	uint32_t ack_ms;
	uint32_t phase_ms;
	uint16_t phase_len_ms;
	struct udsp_region_cid cfg;
	struct udsp_cid cid;
};

void line_cidseq_init(struct line_cidseq *c, unsigned line);
/*
 * wait_ring true holds the sequence until the ring machine opens its window
 * (every type 1 CID delivered with ringing); false starts it on the next tick,
 * which is the standalone path a type 2 CID and a VMWI burst take. mwi selects
 * the message the burst carries.
 *
 * wait_override_ms replaces the region [cid] wait_ms, the pause before the
 * message, for this sequence alone; 0 keeps the region value. It is the
 * udsp_ring cid_wait_ms the caller asked for, and it is uint32_t because that
 * field is, while the region timings are uint16_t. The budget test below runs
 * before the narrowing assignment can matter: a value that would wrap produces
 * a budget above the cap and is refused, so no truncation is reachable.
 *
 * -EINVAL on an unknown format, on UDSP_CID_TYPE2 with a DTMF format (call
 * waiting has no DTMF transport: the far end is on the line and the sequence is
 * CAS, acknowledgement, FSK), on UDSP_CID_TYPE2 with a VMWI message (the
 * indicator is a type 1 on-hook burst), and on a wait_ring sequence whose
 * budget exceeds LINE_CID_WINDOW_MAX_MS. -EBUSY while a sequence is running.
 */
int line_cidseq_arm(struct line_cidseq *c, enum udsp_cid_kind kind,
		    const struct udsp_cid *cid, const struct udsp_region *region,
		    enum udsp_linefeed restore_lf, bool wait_ring,
		    enum line_cid_mwi mwi, uint32_t wait_override_ms);
/*
 * The worst-case wall time of the phase chain those arguments select, which is
 * what a ring break must hold. Callers that own the break test it against
 * LINE_CID_WINDOW_MAX_MS before they start ringing, so a configuration that
 * cannot deliver a record is refused instead of ringing into a dead window.
 */
unsigned line_cidseq_budget_ms(enum udsp_cid_kind kind, const struct udsp_cid *cid,
			       const struct udsp_region *region,
			       uint32_t wait_override_ms);
void line_cidseq_tick(struct line_cidseq *c, uint32_t now_ms,
		      const struct line_hook_report *hook, struct line_cid_gate *gate,
		      struct line_out *out);
/* The armed FSK burst or DTMF string left the line. The engine raises this once
 * its playback pipeline drained the last rendered frame (chan.c), not when the
 * modulator finished, because the restore below takes the on-hook line back
 * into a feed the SLIC mutes. */
void line_cidseq_burst_done(struct line_cidseq *c);
/* A DTMF digit reached the detector. now_ms is the digit's own stamp, not the
 * stamp of the tick that consumes it: a digit inside the window stays valid
 * even when the deciding tick comes after the window closed. */
void line_cidseq_dtmf_ack(struct line_cidseq *c, uint32_t now_ms, char digit);
/* status must be negative and must fit int8_t (udsp_event u.cid_sent.status). */
void line_cidseq_abort(struct line_cidseq *c, int status, uint32_t now_ms,
		       struct line_cid_gate *gate, struct line_out *out);
bool line_cidseq_busy(const struct line_cidseq *c);
/* The type 2 acknowledgement window stands open. The handset answers the CAS
 * with a DTMF digit of its own, so the line keeps its detector armed there
 * even while a digit plays toward the line (core/chan.c chan_dtmf_hold). */
bool line_cidseq_ack_open(const struct line_cidseq *c);

#endif
