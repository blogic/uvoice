// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The call record ring and the per line call counters.
 *
 * What it is. One global ring of CDR_RING records, oldest evicted first, plus
 * six counters and five media totals per line. It stores what the daemon hands
 * it and renders it into blobmsg. It calls no HAL op, arms no timer and reads
 * no config: line/line.c owns the snapshot job, call.c owns the leg, and this
 * module owns the history.
 *
 * The handle rule. It is the hal.h owner-handle rule applied to ring slots.
 * A media snapshot outlives the leg it belongs to and may outlive the record
 * itself, so cdr_media_pending() returns an opaque handle carrying the slot
 * index and a generation. The generation moves on every slot reuse, so a
 * completion that lands after its record was evicted is dropped instead of
 * overwriting somebody else's call.
 *
 * One corner of the eviction is accepted, and it is written down here instead
 * of being guarded against. An open record can be evicted, which needs the
 * other line to run a full ring of calls during one long call. The end of that
 * long call then finds no record of its own, so it adds nothing to in_failed or
 * out_failed, while the attempt it made was already counted at its start.
 * cdr_call_ended() cannot tell the two empty cases apart: an evicted record and
 * a call that never had one look the same to it, and it must keep dropping the
 * second, because a dial the leg layer refuses reports its failure before
 * line/line.c counts anything at all. Reaching this takes 64 finished calls
 * inside one call on a board of 2 lines and 2 slots, and it costs one uncounted
 * failure in a case where the record itself is already gone.
 *
 * What survives what. A reload clears none of it: the ring, the counters and
 * the totals are runtime state, and a config file is not a reason to forget a
 * call that happened. Only cdr_stats_reset() clears, and it keeps the ring,
 * because history is not a counter.
 *
 * The totals move only on a real snapshot (cdr_media_set). A call whose media
 * numbers never arrived adds nothing to them, so the totals and the sum of the
 * ring always say the same thing.
 *
 * Time. start_wall comes from CLOCK_REALTIME and is the only wall clock value
 * in a record; the answer offset and the duration are differences of
 * CLOCK_MONOTONIC, so a clock step during a call cannot make either negative.
 * The duration covers the whole leg, from the attempt to the end, and where
 * that end lies depends on what cleared the leg. A leg a BYE cleared, ours or
 * the peer's, ends at that BYE: it keeps its slot for the BYE transaction
 * drain, and the drain belongs to the protocol rather than to the call. A leg
 * a transfer silenced ends at the silence, the 202 or the verdict, for the
 * same reason: the dialog it keeps for the network's BYE is protocol, not
 * call. Every other leg ends at its release. The talk time stays duration_ms
 * minus answer_offset_ms.
 */
#ifndef UVOICE_CDR_H
#define UVOICE_CDR_H

#include <stdbool.h>
#include <stdint.h>

#define CDR_RING		64
#define CDR_HANDLE_NONE		0
#define MOS_TEXT_MAX		48

struct blob_buf;
struct proto_call_ended;
struct hal_jb_stats;
struct hal_media_stats;

void cdr_init(void);

/*
 * The lifecycle edges. cdr_call_started() takes the ring slot at once, so an
 * in-progress call is visible in the dump, counts the attempt (in_received or
 * out_attempted) and returns the record handle. The (line, slot) pair keys the
 * open record for the three edges below it; slot is the HAL stream of the
 * leg, below HAL_STREAM_MAX. A daemon stop closes the open records: the
 * protocol ends every leg still in use when lines_done() takes the lines out
 * of service, so the ended edge arrives for a call that was still up and no
 * record of that stop stays open.
 */
uint32_t cdr_call_started(unsigned int line, unsigned int slot, bool inbound);

/*
 * The sequence identifies a record in the dump; the handle dies with its
 * ring slot. Trace notifications use this sequence, or 0 with no open record.
 */
uint32_t cdr_call_seq(unsigned int line, unsigned int slot);

/*
 * The stable half of the transfer correlation (proto_media.h
 * proto_call_ended.transferred).
 * A transfer this line receives closes two records, one per leg, and the leg
 * that took the call over is the one that can name the other: this writes the
 * sequence of the record still open on from_slot into the record open on
 * slot.
 *
 * A sequence and not a handle, because a handle carries a ring generation and
 * dies with the slot it names, while the sequence is what a reader of the dump
 * already has in front of it. It is written once, at the open, while the leg
 * being taken over still holds its record; nothing later can recover it.
 *
 * Either record missing leaves the correlation absent, which is what a reader
 * sees for every call that was not transferred.
 */
void cdr_call_correlate(unsigned int line, unsigned int slot, unsigned int from_slot);
void cdr_call_answered(unsigned int line, unsigned int slot);

/*
 * The leg was mixed into the local three-way conference of its line. Both
 * legs of one join take the mark, so a reader pairs them the way it pairs the
 * two records of a transfer: by the flag plus the two records sitting on the
 * same line.
 *
 * It is written on the open record, at the moment the mixer accepts, and a
 * slot with no open record is a no-op. A line that joins the same pair twice
 * writes the same flag twice, which the record does not distinguish and does
 * not have to: the flag says the call was in a conference, not how often.
 */
void cdr_call_conferenced(unsigned int line, unsigned int slot);
void cdr_call_ended(unsigned int line, unsigned int slot,
		    const struct proto_call_ended *info);

/*
 * The media snapshot, which is asynchronous. The caller marks the record
 * pending before it submits the HAL job and reports the outcome against the
 * handle it got, never against the leg, which by then may be gone.
 */
uint32_t cdr_media_pending(unsigned int line, unsigned int slot);

/*
 * The jitter buffer counters of the leg, which are not part of the media
 * snapshot and do not travel its handle. They are read synchronously from a
 * fast HAL op while the stream still runs, so they need
 * no pending state and no generation: the record is looked up the way
 * cdr_media_pending looks it up, by the open (line, slot), and a slot with no
 * open record is a no-op.
 *
 * A record without them omits the table entirely, which is the identity rule
 * of this module: no counters and counters that are all zero are different
 * answers, and a reader must be able to tell them apart.
 */
void cdr_jb_set(unsigned int line, unsigned int slot,
		const struct hal_jb_stats *jb);
void cdr_media_set(uint32_t handle, const struct hal_media_stats *st);
void cdr_media_fail(uint32_t handle);

void cdr_dump(struct blob_buf *b);
/* want_line -1 emits every channel slot, a 0-based index that line alone */
void cdr_stats_dump(struct blob_buf *b, int want_line);
void cdr_stats_reset(struct blob_buf *b);

/*
 * The "media" table of one record, opened and closed by this call. It is
 * exported so the live per-call query of "voice stats_live" renders the same
 * table from the same numbers: a second renderer would give an operator two
 * vocabularies for one set of DSP counters, and the backend-raw units and the
 * two-decimal MOS form would drift apart when one changes. The stats belong
 * to the caller, so this entry reads no record and moves no total; it renders
 * what it is handed.
 */
void cdr_media_dump(struct blob_buf *b, const struct hal_media_stats *m);

/*
 * The jitter buffer keys of one record, and the single renderer of that
 * vocabulary: a status jb row writes the same keys for a live stream. The mode
 * word leads them, because it says what the counters mean; the nineteen
 * counters follow it in one fixed order.
 *
 * It opens no table of its own, unlike cdr_media_dump, because the two callers
 * need different containers: a call record nests the keys under "jb" while a
 * status jb row carries them beside its own line and stream. One renderer, two
 * containers, and no second spelling of the counters.
 */
void cdr_jb_dump(struct blob_buf *b, const struct hal_jb_stats *jb);

#endif
