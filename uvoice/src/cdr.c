// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The call record ring. See cdr.h for the contracts.
 */
#include <math.h>
#include <string.h>
#include <time.h>

#include <libubox/blobmsg.h>

#include "cdr.h"
#include "cfg/cfg.h"
#include "hal/hal.h"
#include "posix.h"
#include "proto/proto_media.h"
#include "str.h"
#include "trace.h"

/* the widths of the three identity strings a record keeps */
#define CDR_NUMBER_MAX		64
#define CDR_NAME_MAX		64

enum cdr_media_state {
	CDR_MEDIA_ABSENT = 0,
	CDR_MEDIA_PENDING,
	CDR_MEDIA_PRESENT,
	CDR_MEDIA_FAILED,
};

struct cdr_entry {
	uint32_t seq;
	uint16_t gen;			/* 0 while the slot was never used */
	uint8_t line;
	uint8_t slot;
	bool inbound;
	bool answered;
	bool failed;
	bool open;			/* the call is still running */
	uint8_t fail_class;		/* enum proto_fail_class, valid when failed */
	uint8_t media_state;		/* enum cdr_media_state */
	uint8_t codec;			/* enum hal_codec */
	bool t38;			/* the session ended carrying T.38 */
	/*
	 * The leg ended because a transfer completed (proto_media.h,
	 * proto_call_ended transferred). It is the correlation key of the
	 * fixed record: a transferee closes two records, and a reader pairs
	 * the child to the parent by this flag plus the adjacency of the two
	 * records on the same line. No transfer id is carried.
	 */
	bool transferred;
	/* the sequence of the record this leg took the call over from, 0 when
	 * it took none (cdr.h cdr_call_correlate) */
	uint32_t transferred_from;
	/* the mixer of the line joined this leg into a local three-way
	 * conference (cdr.h cdr_call_conferenced) */
	bool conferenced;
	uint8_t ptime_ms;
	uint16_t rtp_port;
	uint64_t start_wall;		/* epoch seconds */
	uint64_t start_mono_ms;
	uint32_t answer_offset_ms;
	uint32_t duration_ms;
	char number[CDR_NUMBER_MAX];
	char remote_number[CDR_NUMBER_MAX];
	char remote_name[CDR_NAME_MAX];
	struct hal_media_stats media;
	/* The jitter buffer counters of the leg, read while the stream still
	 * ran (cdr.h cdr_jb_set). The flag says a read happened, which is a
	 * different answer from a buffer that dropped nothing. */
	struct hal_jb_stats jb;
	bool jb_present;
};

struct cdr_line {
	uint32_t in_received;
	uint32_t in_answered;
	uint32_t in_failed;
	uint32_t out_attempted;
	uint32_t out_answered;
	uint32_t out_failed;
	uint64_t packets_sent;
	uint64_t packets_recv;
	uint64_t packets_lost;
	uint64_t bytes_sent;
	uint64_t bytes_recv;
};

static struct {
	struct cdr_entry ring[CDR_RING];
	struct cdr_line line[CFG_CHANNEL_MAX];
	uint32_t open[CFG_CHANNEL_MAX][HAL_STREAM_MAX];
	uint32_t seq_next;
	unsigned int head;		/* where the next record lands */
	unsigned int count;
} cv;

static const char * const media_state_names[] = {
	[CDR_MEDIA_ABSENT] = "absent",
	[CDR_MEDIA_PENDING] = "pending",
	[CDR_MEDIA_PRESENT] = "present",
	[CDR_MEDIA_FAILED] = "failed",
};

/* ------------------------------------------------------------------ clocks */

static uint64_t wall_secs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);

	return (uint64_t)ts.tv_sec;
}

static uint32_t since_ms(uint64_t base)
{
	uint64_t now = mono_ms();

	return now > base ? (uint32_t)(now - base) : 0;
}

/* ------------------------------------------------------------ ring slots */

static uint16_t gen_bump(uint16_t g)
{
	g++;

	return g ? g : 1;
}

static uint32_t handle_of(const struct cdr_entry *e)
{
	uint32_t idx = (uint32_t)(e - cv.ring);

	return (idx << 16) | e->gen;
}

static struct cdr_entry *entry_get(uint32_t handle)
{
	unsigned int idx = handle >> 16;
	uint16_t gen = (uint16_t)(handle & 0xffff);
	struct cdr_entry *e;

	if (!gen || idx >= CDR_RING)
		return NULL;

	e = &cv.ring[idx];

	return e->gen == gen ? e : NULL;
}

static struct cdr_entry *entry_open(unsigned int line, unsigned int slot)
{
	struct cdr_entry *e;

	if (line >= CFG_CHANNEL_MAX || slot >= HAL_STREAM_MAX)
		return NULL;

	e = entry_get(cv.open[line][slot]);

	return e && e->open ? e : NULL;
}

static void entry_close(struct cdr_entry *e)
{
	e->open = false;
	cv.open[e->line][e->slot] = CDR_HANDLE_NONE;
}

/*
 * The generation moves on every reuse, including the first one, so a handle
 * built from a record is never the zero handle and an evicted record answers
 * no completion that was still in flight for it.
 */
static struct cdr_entry *entry_alloc(void)
{
	struct cdr_entry *e = &cv.ring[cv.head];
	uint16_t gen = gen_bump(e->gen);

	memset(e, 0, sizeof(*e));
	e->gen = gen;

	cv.head = (cv.head + 1) % CDR_RING;
	if (cv.count < CDR_RING)
		cv.count++;

	return e;
}

static const struct cdr_entry *entry_at(unsigned int age)
{
	unsigned int idx = (cv.head + CDR_RING - cv.count + age) % CDR_RING;

	return &cv.ring[idx];
}

/* ------------------------------------------------------------ the edges */

void cdr_init(void)
{
	memset(&cv, 0, sizeof(cv));
}

/*
 * The answered edge is shared. line/line.c reports it from two places, the leg
 * layer's answered() for a call this line placed and the accepted answer for
 * one it took, and each line reaches only one of them; the guard makes a
 * second report on one record cost nothing.
 */
static void answered_take(struct cdr_entry *e, uint32_t offset_ms)
{
	struct cdr_line *st = &cv.line[e->line];

	if (e->answered)
		return;

	e->answered = true;
	e->answer_offset_ms = offset_ms;

	if (e->inbound)
		st->in_answered++;
	else
		st->out_answered++;
}

uint32_t cdr_call_started(unsigned int line, unsigned int slot, bool inbound)
{
	struct cdr_entry *e;
	uint32_t handle;

	if (line >= CFG_CHANNEL_MAX || slot >= HAL_STREAM_MAX)
		return CDR_HANDLE_NONE;

	/*
	 * The pairing is structural: call.c holds one leg per (line, slot) and
	 * reports its end from the in_use guard of leg_release(), so a start
	 * that finds the previous record still open means an end was lost. The
	 * record is finalised as it stands and the fault is said out loud,
	 * because a defensive re-open would hide the missing report and leave
	 * two records claiming one slot.
	 */
	e = entry_open(line, slot);
	if (e) {
		TRACE(TRACE_FSM, "cdr line %u slot %u seq %u never reported its end",
		      line, slot, e->seq);
		e->duration_ms = since_ms(e->start_mono_ms);
		entry_close(e);
	}

	e = entry_alloc();
	e->seq = ++cv.seq_next;
	e->line = (uint8_t)line;
	e->slot = (uint8_t)slot;
	e->inbound = inbound;
	e->open = true;
	e->media_state = CDR_MEDIA_ABSENT;
	e->start_wall = wall_secs();
	e->start_mono_ms = mono_ms();

	/*
	 * The counters count legs and not inbound offers, so a call refused
	 * for want of a slot is uncounted here and visible in the SIP trace
	 * instead.
	 */
	if (inbound)
		cv.line[line].in_received++;
	else
		cv.line[line].out_attempted++;

	handle = handle_of(e);
	cv.open[line][slot] = handle;

	return handle;
}

uint32_t cdr_call_seq(unsigned int line, unsigned int slot)
{
	const struct cdr_entry *e = entry_open(line, slot);

	return e ? e->seq : 0;
}

void cdr_call_answered(unsigned int line, unsigned int slot)
{
	struct cdr_entry *e = entry_open(line, slot);

	if (!e)
		return;

	answered_take(e, since_ms(e->start_mono_ms));
}

void cdr_call_correlate(unsigned int line, unsigned int slot, unsigned int from_slot)
{
	struct cdr_entry *e, *from;

	if (line >= CFG_CHANNEL_MAX || slot >= HAL_STREAM_MAX ||
	    from_slot >= HAL_STREAM_MAX)
		return;

	e = entry_open(line, slot);
	from = entry_open(line, from_slot);
	if (!e || !from || e == from)
		return;

	e->transferred_from = from->seq;
}

void cdr_call_conferenced(unsigned int line, unsigned int slot)
{
	struct cdr_entry *e = entry_open(line, slot);

	if (!e)
		return;

	e->conferenced = true;
}

/*
 * A BYE-cleared leg ends at its BYE (cdr.h "Time"), and the report carries the
 * age of that BYE on the leg's clock while the elapsed time above comes from
 * this module's own. The zero clamp is what makes the mix safe: the two clocks
 * differ by the microseconds between the two reads, and an age one microsecond
 * past the elapsed time would underflow the subtraction into weeks.
 */
static uint32_t duration_of(const struct cdr_entry *e,
			    const struct proto_call_ended *info)
{
	uint32_t elapsed = since_ms(e->start_mono_ms);

	if (!info->cleared)
		return elapsed;

	return elapsed > info->cleared_ago_ms ? elapsed - info->cleared_ago_ms : 0;
}

void cdr_call_ended(unsigned int line, unsigned int slot,
		    const struct proto_call_ended *info)
{
	struct cdr_entry *e = entry_open(line, slot);
	struct cdr_line *st;

	if (!info)
		return;

	/*
	 * A leg that never became a record ends here too: a dial the account
	 * layer refused reports its failure from inside call_dial(), before
	 * line/line.c ever counted the attempt.
	 */
	if (!e) {
		TRACE(TRACE_FSM, "cdr line %u slot %u ended with no open record",
		      line, slot);
		return;
	}

	e->inbound = info->inbound;
	/*
	 * The answer as an age, for the leg that announced it to nobody
	 * (proto_media.h answered_ago_ms). The elapsed time is this module's
	 * own clock and the age is the leg's, so the two never meet; the zero
	 * clamp is what makes the mix safe, exactly as it does for the clear
	 * below.
	 */
	if (info->answered) {
		uint32_t elapsed = since_ms(e->start_mono_ms);

		answered_take(e, elapsed > info->answered_ago_ms ?
				 elapsed - info->answered_ago_ms : elapsed);
	}

	e->failed = info->failed;
	e->fail_class = (uint8_t)info->fail_class;
	e->codec = info->codec;
	e->t38 = info->t38;
	e->transferred = info->transferred;
	e->ptime_ms = info->ptime_ms;
	e->rtp_port = info->rtp_port;
	str_copy(e->number, sizeof(e->number), info->number);
	str_copy(e->remote_number, sizeof(e->remote_number),
		 info->remote_number);
	str_copy(e->remote_name, sizeof(e->remote_name), info->remote_name);
	e->duration_ms = duration_of(e, info);

	/*
	 * Every media state is already decided: the snapshot marked the record
	 * pending before the stream stopped, its completion moves it to present
	 * or failed whenever it lands, and a record whose stream never ran, or
	 * whose submission the HAL refused, is still absent. Media that ran
	 * without a snapshot would be a hole in that chain, so it is traced.
	 */
	if (info->media_ran && e->media_state == CDR_MEDIA_ABSENT)
		TRACE(TRACE_FSM, "cdr line %u slot %u seq %u ran media with no snapshot",
		      line, slot, e->seq);

	st = &cv.line[e->line];
	if (info->failed && info->inbound)
		st->in_failed++;
	else if (info->failed)
		st->out_failed++;

	entry_close(e);
}

uint32_t cdr_media_pending(unsigned int line, unsigned int slot)
{
	struct cdr_entry *e = entry_open(line, slot);

	if (!e)
		return CDR_HANDLE_NONE;

	e->media_state = CDR_MEDIA_PENDING;

	return handle_of(e);
}

void cdr_jb_set(unsigned int line, unsigned int slot,
		const struct hal_jb_stats *jb)
{
	struct cdr_entry *e = entry_open(line, slot);

	if (!e || !jb)
		return;

	e->jb = *jb;
	e->jb_present = true;
}

void cdr_media_set(uint32_t handle, const struct hal_media_stats *st)
{
	struct cdr_entry *e = entry_get(handle);
	struct cdr_line *tot;

	if (!e || !st) {
		TRACE(TRACE_FSM, "cdr media result for handle %08x dropped", handle);
		return;
	}

	e->media = *st;
	e->media_state = CDR_MEDIA_PRESENT;

	/* The totals move here and nowhere else, so they never count a call
	 * whose numbers the DSP never gave us. */
	tot = &cv.line[e->line];
	tot->packets_sent += st->packets_sent;
	tot->packets_recv += st->packets_recv;
	tot->packets_lost += st->packets_lost;
	tot->bytes_sent += st->bytes_sent;
	tot->bytes_recv += st->bytes_recv;
}

void cdr_media_fail(uint32_t handle)
{
	struct cdr_entry *e = entry_get(handle);

	if (!e) {
		TRACE(TRACE_FSM, "cdr media failure for handle %08x dropped", handle);
		return;
	}

	e->media_state = CDR_MEDIA_FAILED;
}

/* -------------------------------------------------------------- the dumps */

void cdr_media_dump(struct blob_buf *b, const struct hal_media_stats *m)
{
	char mos_lq[MOS_TEXT_MAX];
	char mos_cq[MOS_TEXT_MAX];
	void *table = blobmsg_open_table(b, "media");

	snprintf(mos_lq, sizeof(mos_lq), "%.2f",
		 isfinite(m->mos_lq) ? m->mos_lq : 0.0f);
	snprintf(mos_cq, sizeof(mos_cq), "%.2f",
		 isfinite(m->mos_cq) ? m->mos_cq : 0.0f);

	blobmsg_add_u64(b, "packets_sent", m->packets_sent);
	blobmsg_add_u64(b, "packets_recv", m->packets_recv);
	blobmsg_add_u64(b, "packets_lost", m->packets_lost);
	blobmsg_add_u64(b, "packets_discarded", m->packets_discarded);
	blobmsg_add_u64(b, "loss_rate", m->loss_rate);
	blobmsg_add_u64(b, "loss_rate_far", m->loss_rate_far);
	/*
	 * Backend raw, and deliberately unconverted: hal.h gives the jitter
	 * and delay members no fixed unit (struct hal_media_stats). An
	 * invented scale here would put a wrong number in a record an
	 * operator reads as truth.
	 */
	blobmsg_add_u64(b, "jitter_avg", m->jitter_avg);
	blobmsg_add_u64(b, "jitter_max", m->jitter_max);
	blobmsg_add_u64(b, "jitter_far", m->jitter_far);
	blobmsg_add_u64(b, "rtt_avg", m->rtt_avg);
	blobmsg_add_u64(b, "rtt_max", m->rtt_max);
	blobmsg_add_u64(b, "bytes_sent", m->bytes_sent);
	blobmsg_add_u64(b, "bytes_recv", m->bytes_recv);
	blobmsg_add_string(b, "mos_lq", mos_lq);
	blobmsg_add_string(b, "mos_cq", mos_cq);
	blobmsg_add_u32(b, "rfactor", m->rfactor);
	blobmsg_close_table(b, table);
}

void cdr_jb_dump(struct blob_buf *b, const struct hal_jb_stats *jb)
{
	/* The mode leads the counters, because it says what they mean. A fixed
	 * buffer pins a target the configuration never named. */
	blobmsg_add_string(b, "mode", hal_jb_mode_name(jb->mode));
	blobmsg_add_u64(b, "frames_out", jb->frames_out);
	blobmsg_add_u64(b, "plc", jb->plc);
	blobmsg_add_u64(b, "cn", jb->cn);
	blobmsg_add_u64(b, "silence", jb->silence);
	blobmsg_add_u64(b, "late_drop", jb->late_drop);
	blobmsg_add_u64(b, "dup_drop", jb->dup_drop);
	blobmsg_add_u64(b, "overflow_drop", jb->overflow_drop);
	blobmsg_add_u64(b, "hold_drop", jb->hold_drop);
	blobmsg_add_u64(b, "resync_discard_ms", jb->resync_discard_ms);
	blobmsg_add_u64(b, "resyncs", jb->resyncs);
	blobmsg_add_u64(b, "starve_resyncs", jb->starve_resyncs);
	blobmsg_add_u64(b, "cn_reanchors", jb->cn_reanchors);
	blobmsg_add_u64(b, "underruns", jb->underruns);
	blobmsg_add_u64(b, "skew_drops", jb->skew_drops);
	blobmsg_add_u64(b, "skew_inserts", jb->skew_inserts);
	/* The four millisecond values are u32 like every other ms field of
	 * this daemon's northbound, whatever width the record carries. */
	blobmsg_add_u32(b, "target_ms", jb->target_ms);
	blobmsg_add_u32(b, "depth_ms", jb->depth_ms);
	blobmsg_add_u32(b, "jitter_ms", jb->jitter_ms);
	blobmsg_add_u32(b, "pool_free", jb->pool_free);
}

static void entry_dump(struct blob_buf *b, const struct cdr_entry *e)
{
	blobmsg_add_u32(b, "seq", e->seq);
	blobmsg_add_u32(b, "line", e->line);
	blobmsg_add_u32(b, "slot", e->slot);
	blobmsg_add_u8(b, "inbound", e->inbound);
	blobmsg_add_u8(b, "answered", e->answered);
	if (e->failed)
		blobmsg_add_string(b, "fail",
				   proto_fail_name((enum proto_fail_class)e->fail_class));

	/* the calls_status_dump convention: an empty identity is absent, not an
	 * empty string, so a reader never has to tell the two apart */
	if (e->number[0])
		blobmsg_add_string(b, "number", e->number);
	if (e->remote_number[0])
		blobmsg_add_string(b, "remote", e->remote_number);
	if (e->remote_name[0])
		blobmsg_add_string(b, "name", e->remote_name);

	blobmsg_add_u64(b, "start", e->start_wall);
	blobmsg_add_u32(b, "duration_ms", e->duration_ms);
	if (e->answered)
		blobmsg_add_u32(b, "answer_offset_ms", e->answer_offset_ms);

	/*
	 * A T.38 call negotiated no audio codec, so the codec below stays NONE
	 * and this record would name no media at all. It is written as the one
	 * word the status dump uses (calls_status_dump), with the 0 ptime an
	 * image exchange pins beside it.
	 */
	if (e->t38) {
		blobmsg_add_string(b, "codec", "t38");
		blobmsg_add_u32(b, "ptime", e->ptime_ms);
	} else if (e->codec != HAL_CODEC_NONE) {
		blobmsg_add_string(b, "codec",
				   hal_codec_name((enum hal_codec)e->codec));
		blobmsg_add_u32(b, "ptime", e->ptime_ms);
	}
	if (e->rtp_port)
		blobmsg_add_u32(b, "rtp_port", e->rtp_port);

	/* present only where a transfer ended the leg, the way every other
	 * optional fact of this record is written */
	if (e->transferred)
		blobmsg_add_u8(b, "transferred", 1);
	/* the other half of that pairing, on the record that took the call
	 * over: the sequence of the one it took it from */
	if (e->transferred_from)
		blobmsg_add_u32(b, "transferred_from", e->transferred_from);
	/* present only on a leg the mixer joined, as the transfer keys
	 * above are */
	if (e->conferenced)
		blobmsg_add_u8(b, "conferenced", 1);

	blobmsg_add_string(b, "media_state", media_state_names[e->media_state]);
	if (e->media_state == CDR_MEDIA_PRESENT)
		cdr_media_dump(b, &e->media);

	/* Present only where the leg's buffer answered, exactly as the media
	 * table is (cdr.h cdr_jb_set). */
	if (e->jb_present) {
		void *jb = blobmsg_open_table(b, "jb");

		cdr_jb_dump(b, &e->jb);
		blobmsg_close_table(b, jb);
	}

	if (e->open)
		blobmsg_add_u8(b, "in_progress", 1);
}

void cdr_dump(struct blob_buf *b)
{
	void *array, *table;
	unsigned int i;

	array = blobmsg_open_array(b, "cdr");
	for (i = 0; i < cv.count; i++) {
		table = blobmsg_open_table(b, NULL);
		entry_dump(b, entry_at(i));
		blobmsg_close_table(b, table);
	}
	blobmsg_close_array(b, array);
}

void cdr_stats_dump(struct blob_buf *b, int want_line)
{
	void *array, *entry, *table;
	unsigned int i;

	array = blobmsg_open_array(b, "stats");
	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		const struct cdr_line *st = &cv.line[i];

		if (want_line >= 0 && (unsigned int)want_line != i)
			continue;

		entry = blobmsg_open_table(b, NULL);
		blobmsg_add_u32(b, "line", i);
		blobmsg_add_u32(b, "in_received", st->in_received);
		blobmsg_add_u32(b, "in_answered", st->in_answered);
		blobmsg_add_u32(b, "in_failed", st->in_failed);
		blobmsg_add_u32(b, "out_attempted", st->out_attempted);
		blobmsg_add_u32(b, "out_answered", st->out_answered);
		blobmsg_add_u32(b, "out_failed", st->out_failed);

		table = blobmsg_open_table(b, "totals");
		blobmsg_add_u64(b, "packets_sent", st->packets_sent);
		blobmsg_add_u64(b, "packets_recv", st->packets_recv);
		blobmsg_add_u64(b, "packets_lost", st->packets_lost);
		blobmsg_add_u64(b, "bytes_sent", st->bytes_sent);
		blobmsg_add_u64(b, "bytes_recv", st->bytes_recv);
		blobmsg_close_table(b, table);
		blobmsg_close_table(b, entry);
	}
	blobmsg_close_array(b, array);
}

/*
 * The reply says what the reset did, because the part it does not do is the
 * surprising half: the ring keeps every record it holds, and the record
 * sequence keeps counting, so a reset cannot be used to hide a call.
 */
void cdr_stats_reset(struct blob_buf *b)
{
	void *array;

	memset(cv.line, 0, sizeof(cv.line));

	array = blobmsg_open_array(b, "reset");
	blobmsg_add_string(b, NULL, "counters");
	blobmsg_add_string(b, NULL, "totals");
	blobmsg_close_array(b, array);
	blobmsg_add_u8(b, "ring_kept", 1);
}
