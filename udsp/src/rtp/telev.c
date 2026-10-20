// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "rtp.h"

static int digit_event(char digit, uint8_t *event)
{
	if (digit >= '0' && digit <= '9') {
		*event = (uint8_t)(digit - '0');
		return 0;
	}
	if (digit == '*') {
		*event = 10;
		return 0;
	}
	if (digit == '#') {
		*event = 11;
		return 0;
	}
	if (digit >= 'A' && digit <= 'D') {
		*event = (uint8_t)(digit - 'A' + 12);
		return 0;
	}
	return -1;
}

static int event_digit(uint8_t event, char *digit)
{
	if (event <= 9U) {
		*digit = (char)('0' + event);
		return 0;
	}
	if (event == 10U) {
		*digit = '*';
		return 0;
	}
	if (event == 11U) {
		*digit = '#';
		return 0;
	}
	if (event <= 15U) {
		*digit = (char)('A' + event - 12U);
		return 0;
	}
	return -1;
}

void telev_sender_init(struct telev_sender *sender, struct rtp_sender *rtp)
{
	memset(sender, 0, sizeof(*sender));
	sender->rtp = rtp;
}

int telev_sender_start(struct telev_sender *sender, char digit, uint8_t volume)
{
	uint8_t event;

	if (!sender->rtp || sender->active || volume > 63U ||
	    sender->rtp->event_pt == RTP_PT_DISABLED ||
	    digit_event(digit, &event) < 0)
		return -EINVAL;
	sender->timestamp = sender->rtp->timestamp;
	sender->duration = 0;
	sender->event = event;
	sender->volume = volume;
	sender->end_packets = 0;
	sender->active = true;
	sender->marker_pending = true;
	sender->ending = false;
	return 0;
}

int telev_sender_update(struct telev_sender *sender, uint16_t duration)
{
	if (!sender->active || sender->ending || duration < sender->duration)
		return -1;
	sender->duration = duration;
	sender->rtp->timestamp = sender->timestamp + duration;
	return 0;
}

int telev_sender_end(struct telev_sender *sender, uint16_t duration)
{
	if (!sender->active || sender->ending || duration < sender->duration)
		return -1;
	sender->duration = duration;
	sender->rtp->timestamp = sender->timestamp + duration;
	sender->ending = true;
	/* RFC 4733 2.5.1.4 sends the final event packet three times. */
	sender->end_packets = 3;
	return 0;
}

/*
 * RFC 4733 2.5.1.3. The duration field holds 16 bits, so an event that outruns
 * it continues in a new segment. That segment starts where this one ended, on
 * the same event code. The caller puts the closing packet on the wire first,
 * which carries the duration this advances by. The E bit stays clear on it,
 * because the event has not ended. The marker stays down too: a segment is not
 * a new event onset, and a receiver that read one would report the chain as
 * two digits.
 */
int telev_sender_segment(struct telev_sender *sender)
{
	if (!sender->rtp || !sender->active || sender->ending ||
	    !sender->duration)
		return -EINVAL;
	sender->timestamp += sender->duration;
	sender->duration = 0;
	sender->marker_pending = false;
	sender->rtp->timestamp = sender->timestamp;
	return 0;
}

int telev_sender_packet_build(struct telev_sender *sender, uint8_t *packet,
			      size_t capacity, size_t *packet_len)
{
	bool marker;

	if (!sender->active || !packet_len || capacity < RTP_HEADER_SIZE + 4U)
		return -1;
	marker = sender->marker_pending;
	if (rtp_header_build(sender->rtp, sender->rtp->event_pt, marker,
			     sender->timestamp, packet, capacity) < 0)
		return -1;
	packet[12] = sender->event;
	packet[13] = sender->volume | (sender->ending ? 0x80U : 0U);
	rtp_u16_write(packet + 14, sender->duration);
	*packet_len = RTP_HEADER_SIZE + 4U;
	sender->marker_pending = false;
	if (sender->rtp->stats) {
		sender->rtp->stats->packets_sent++;
		sender->rtp->stats->bytes_sent += *packet_len;
		sender->rtp->stats->octets_sent += 4U;
	}
	if (!sender->ending)
		return 0;
	if (--sender->end_packets)
		return 0;
	sender->active = false;
	sender->ending = false;
	return 0;
}

void telev_receiver_init_cfg(struct telev_receiver *receiver,
			     uint32_t clock_rate)
{
	memset(receiver, 0, sizeof(*receiver));
	receiver->clock_rate = clock_rate ? clock_rate : RTP_CLOCK_RATE;
}

void telev_receiver_init(struct telev_receiver *receiver)
{
	telev_receiver_init_cfg(receiver, RTP_CLOCK_RATE);
}

static bool report_was_sent(const struct telev_receiver *receiver,
			    uint8_t event, uint32_t end_timestamp)
{
	unsigned i;

	for (i = 0; i < TELEV_RECENT_COUNT; i++) {
		if (receiver->recent[i].valid &&
		    receiver->recent[i].event == event &&
		    receiver->recent[i].end_timestamp == end_timestamp)
			return true;
	}
	return false;
}

static void report_remember(struct telev_receiver *receiver, uint8_t event,
			    uint32_t end_timestamp)
{
	struct telev_reported *reported =
		&receiver->recent[receiver->recent_next];

	reported->event = event;
	reported->end_timestamp = end_timestamp;
	reported->valid = true;
	receiver->recent_next = (receiver->recent_next + 1U) % TELEV_RECENT_COUNT;
}

/* The user-visible record. duration covers the whole event in timestamp units,
 * segments included, so it outgrows the 16-bit wire field by design (rtp.h
 * struct telev_receiver). */
static int event_fill(const struct telev_receiver *receiver, uint8_t event_code,
		      uint32_t duration, struct telev_event *event)
{
	uint32_t duration_ms = duration / (receiver->clock_rate / 1000U);
	char digit;

	if (event_digit(event_code, &digit) < 0)
		return -1;
	event->digit = digit;
	/* The record states milliseconds, so the clamp is the field's own
	 * bound and not the wire duration's. */
	event->duration_ms = duration_ms > UINT16_MAX ? UINT16_MAX :
						        (uint16_t)duration_ms;
	return 1;
}

/* The dedup runs first, then the record is filled: a retransmitted end packet
 * reports nothing, and a caller that keeps the last record it was handed must
 * not find that one overwritten by the copy. */
static int event_report(struct telev_receiver *receiver, uint8_t event_code,
			uint32_t duration, uint32_t end_timestamp,
			struct telev_event *event)
{
	char digit;

	if (event_digit(event_code, &digit) < 0)
		return -1;
	if (report_was_sent(receiver, event_code, end_timestamp))
		return 0;
	report_remember(receiver, event_code, end_timestamp);

	return event_fill(receiver, event_code, duration, event);
}

/* The chain this packet continues, taken whole: a segment starts where the
 * held one ended (RFC 4733 2.5.1.3), so that timestamp is the whole test. */
static uint32_t segment_take(struct telev_receiver *receiver,
			     uint8_t event_code, uint32_t start_timestamp,
			     uint16_t duration)
{
	uint32_t total;

	if (!receiver->seg_valid || receiver->seg_event != event_code ||
	    receiver->seg_end_ts != start_timestamp)
		return duration;
	total = receiver->seg_total + duration;
	receiver->seg_valid = false;
	receiver->seg_total = 0;
	return total;
}

/*
 * RFC 4733 2.5.1.3: the duration field at its maximum with the E bit clear
 * closes an intermediate segment, and the next segment of the same event
 * starts at that end. The chain is held, never reported: one event is one
 * digit however many segments carry it.
 *
 * The end is remembered in the report ring although nothing is reported, so a
 * retransmitted closing packet that crosses the final segment cannot open a
 * second chain and report the digit twice. The chain reports through
 * segment_flush instead, which does not deduplicate against that entry.
 */
static int segment_close(struct telev_receiver *receiver,
			 const struct rtp_packet *packet, uint8_t event_code,
			 uint32_t end_timestamp)
{
	if (report_was_sent(receiver, event_code, end_timestamp))
		return 0;
	receiver->seg_total = segment_take(receiver, event_code,
					   packet->timestamp,
					   TELEV_DURATION_MAX);
	receiver->seg_end_ts = end_timestamp;
	receiver->seg_event = event_code;
	receiver->seg_arrival_ns = packet->arrival_ns;
	receiver->seg_valid = true;
	receiver->active_valid = false;
	report_remember(receiver, event_code, end_timestamp);
	return 0;
}

/* One bound serves the chain: how long the receiver waits for a continuation
 * before it reports what it holds, and how long a reported chain's identity
 * can still claim one afterwards. */
static uint64_t segment_timeout_ns(const struct telev_receiver *receiver)
{
	return (uint64_t)TELEV_DURATION_MAX * 2U * 1000000000ULL /
	       receiver->clock_rate;
}

static void flushed_remember(struct telev_receiver *receiver,
			     uint8_t event_code, uint32_t next_ts,
			     uint64_t arrival_ns)
{
	struct telev_flushed *flushed =
		&receiver->flushed[receiver->flushed_next];

	flushed->event = event_code;
	flushed->next_ts = next_ts;
	flushed->arrival_ns = arrival_ns;
	flushed->valid = true;
	receiver->flushed_next = (receiver->flushed_next + 1U) %
				 TELEV_RECENT_COUNT;
}

/* A key the continuation window has run out on is dropped where it is found:
 * a packet that late is a new event by every test the wire offers, and the
 * key set may not decide anything forever. */
static struct telev_flushed *flushed_find(struct telev_receiver *receiver,
					  uint8_t event_code, uint32_t timestamp,
					  uint64_t arrival_ns)
{
	struct telev_flushed *flushed;
	unsigned i;

	for (i = 0; i < TELEV_RECENT_COUNT; i++) {
		flushed = &receiver->flushed[i];
		if (!flushed->valid || flushed->event != event_code ||
		    flushed->next_ts != timestamp)
			continue;
		if (arrival_ns > flushed->arrival_ns &&
		    arrival_ns - flushed->arrival_ns >=
		    segment_timeout_ns(receiver)) {
			flushed->valid = false;
			return NULL;
		}

		return flushed;
	}

	return NULL;
}

/*
 * The chain reports here and nowhere else, so this does not deduplicate
 * against the entry its own closing packet left in the ring.
 *
 * It must leave the chain's identity behind. The far end's continuation can
 * still arrive after this, and its own end packet would report a second digit
 * under a timestamp no ring entry could have predicted.
 */
static int segment_flush(struct telev_receiver *receiver, uint64_t arrival_ns,
			 struct telev_event *event)
{
	int result;

	result = event_fill(receiver, receiver->seg_event, receiver->seg_total,
			    event);
	flushed_remember(receiver, receiver->seg_event, receiver->seg_end_ts,
			 arrival_ns);
	receiver->seg_valid = false;
	receiver->seg_total = 0;
	return result;
}

/* The continuation of a chain whose digit the caller already has. A late
 * continuation may close a segment of its own, and the suppression follows the
 * far end to the segment after it. */
static bool segment_reported(struct telev_receiver *receiver,
			     const struct rtp_packet *packet, uint8_t event_code,
			     uint16_t duration, bool ending)
{
	struct telev_flushed *flushed;

	flushed = flushed_find(receiver, event_code, packet->timestamp,
			       packet->arrival_ns);
	if (!flushed)
		return false;
	flushed->arrival_ns = packet->arrival_ns;
	if (!ending && duration == TELEV_DURATION_MAX)
		flushed->next_ts += TELEV_DURATION_MAX;

	return true;
}

/* The continuation never came: the chain reports what it holds rather than
 * costing the caller the digit. The bound mirrors the active-event timeout,
 * measured on the segment the field could just carry. */
static int segment_timeout_report(struct telev_receiver *receiver,
				  const struct rtp_packet *packet,
				  struct telev_event *event)
{
	if (!receiver->seg_valid ||
	    packet->arrival_ns < receiver->seg_arrival_ns)
		return 0;
	if (packet->arrival_ns - receiver->seg_arrival_ns <
	    segment_timeout_ns(receiver))
		return 0;

	return segment_flush(receiver, packet->arrival_ns, event);
}

static int active_timeout_report(struct telev_receiver *receiver,
				 const struct rtp_packet *packet,
				 struct telev_event *event)
{
	uint64_t timeout_ns;
	uint32_t end_timestamp;
	uint32_t total;
	int result;

	if (!receiver->active_valid || !receiver->active_duration ||
	    packet->arrival_ns < receiver->active_arrival_ns)
		return 0;
	timeout_ns = (uint64_t)receiver->active_duration * 2U * 1000000000ULL /
		     receiver->clock_rate;
	if (packet->arrival_ns - receiver->active_arrival_ns < timeout_ns)
		return 0;
	end_timestamp = receiver->active_timestamp + receiver->active_duration;
	/* A segment whose own end packets were all lost still belongs to its
	 * chain, so the timeout reports the whole event once. */
	total = segment_take(receiver, receiver->active_event,
			     receiver->active_timestamp,
			     receiver->active_duration);
	result = event_report(receiver, receiver->active_event, total,
			      end_timestamp, event);
	receiver->active_valid = false;
	return result;
}

int telev_packet_parse(struct telev_receiver *receiver,
		       const struct rtp_packet *packet, struct telev_event *event)
{
	uint16_t duration;
	uint32_t end_timestamp;
	uint32_t total;
	uint8_t event_code;
	int timeout_result;

	if (!receiver || !packet || !event)
		return -1;
	if (packet->kind != RTP_PAYLOAD_TELEPHONE_EVENT) {
		timeout_result = active_timeout_report(receiver, packet, event);
		if (timeout_result)
			return timeout_result;
		return segment_timeout_report(receiver, packet, event);
	}
	if (packet->payload_len < 4U)
		return -1;
	event_code = packet->payload[0];
	if (event_digit(event_code, &event->digit) < 0)
		return -1;
	duration = rtp_u16_read(packet->payload + 2);
	end_timestamp = packet->timestamp + duration;
	if (segment_reported(receiver, packet, event_code, duration,
			     (packet->payload[1] & 0x80U) != 0))
		return 0;
	if (packet->payload[1] & 0x80U) {
		if (receiver->active_valid &&
		    receiver->active_event == event_code &&
		    receiver->active_timestamp == packet->timestamp)
			receiver->active_valid = false;
		total = segment_take(receiver, event_code, packet->timestamp,
				     duration);
		return event_report(receiver, event_code, total, end_timestamp,
				    event);
	}
	if (duration == TELEV_DURATION_MAX)
		return segment_close(receiver, packet, event_code,
				     end_timestamp);
	if (receiver->active_valid &&
	    (receiver->active_event != event_code ||
	     receiver->active_timestamp != packet->timestamp)) {
		timeout_result = active_timeout_report(receiver, packet, event);
		receiver->active_event = event_code;
		receiver->active_timestamp = packet->timestamp;
		receiver->active_arrival_ns = packet->arrival_ns;
		receiver->active_duration = duration;
		receiver->active_valid = true;
		return timeout_result;
	}
	/* Every field of the active event comes from the packet that makes it
	 * active, the duration included: a duration left over from the event
	 * before it is what the timeout would then measure, and a continuation
	 * segment whose own end packets are lost would time out against a
	 * stretch of the event that is already reported. */
	if (!receiver->active_valid) {
		receiver->active_event = event_code;
		receiver->active_timestamp = packet->timestamp;
		receiver->active_arrival_ns = packet->arrival_ns;
		receiver->active_duration = duration;
		receiver->active_valid = true;
		return 0;
	}
	if (duration > receiver->active_duration)
		receiver->active_duration = duration;
	return 0;
}

/* --- the event the receive half renders toward the line ------------------ */

static uint32_t play_timeout_env(void)
{
	const char *v = getenv("UDSP_TEV_PLAY_TIMEOUT_MS");
	char *end = NULL;
	unsigned long n;

	if (!v || !*v)
		return UDSP_TEV_PLAY_TIMEOUT_MS;
	n = strtoul(v, &end, 10);
	if (end == v || (end && *end) || !n || n > UINT32_MAX)
		return UDSP_TEV_PLAY_TIMEOUT_MS;

	return (uint32_t)n;
}

uint32_t telev_play_timeout_ms(void)
{
	static _Atomic uint_least32_t cached;
	uint_least32_t ms = atomic_load_explicit(&cached, memory_order_relaxed);

	if (ms)
		return (uint32_t)ms;
	ms = play_timeout_env();
	atomic_store_explicit(&cached, ms, memory_order_relaxed);

	return (uint32_t)ms;
}

void telev_play_init(struct telev_play *play)
{
	memset(play, 0, sizeof(*play));
	play->timeout_ms = telev_play_timeout_ms();
}

int telev_play_packet(struct telev_play *play, const struct rtp_packet *packet)
{
	uint16_t duration;
	uint8_t event_code;
	bool ending;
	char digit;

	if (packet->kind != RTP_PAYLOAD_TELEPHONE_EVENT ||
	    packet->payload_len < 4U)
		return 0;
	event_code = packet->payload[0];
	duration = rtp_u16_read(packet->payload + 2);
	ending = (packet->payload[1] & 0x80U) != 0;
	if (play->active && play->event == event_code &&
	    play->event_ts == packet->timestamp) {
		play->last_ns = packet->arrival_ns;
		if (play->seg_units + duration > play->target)
			play->target = play->seg_units + duration;
		if (ending)
			play->ended = true;
		return 0;
	}
	/* RFC 4733 2.5.1.3: a segment that closed at the duration ceiling
	 * continues under the timestamp it ended at. The chain is one event,
	 * so the render, the ceiling and the generator all carry on. */
	if (play->active && play->event == event_code &&
	    play->target >= play->seg_units + TELEV_DURATION_MAX &&
	    packet->timestamp == play->event_ts + TELEV_DURATION_MAX) {
		play->seg_units += TELEV_DURATION_MAX;
		play->event_ts = packet->timestamp;
		play->target = play->seg_units + duration;
		play->last_ns = packet->arrival_ns;
		play->ended = ending;
		return 0;
	}
	/* A packet older than the identity held is a packet of an event the
	 * line played out, reordered or the second and third end of RFC 4733
	 * 2.5.1.4, and it must not replay that digit over the newer one. A
	 * first-seen end whose timestamp is newer is the other case, where the
	 * start packets were lost or the whole event drained inside one tick,
	 * and it renders what it announces. */
	if (play->active && (int32_t)(packet->timestamp - play->event_ts) < 0)
		return 0;
	if (event_digit(event_code, &digit) < 0)
		return 0;
	play->event = event_code;
	play->volume = packet->payload[1] & 0x3fU;
	play->digit = digit;
	play->event_ts = packet->timestamp;
	play->seg_units = 0;
	play->target = duration;
	play->rendered = 0;
	play->start_ns = packet->arrival_ns;
	play->last_ns = packet->arrival_ns;
	play->active = true;
	play->ended = ending;
	play->done = false;

	return 1;
}

static bool play_expired(const struct telev_play *play, uint64_t now_ns)
{
	uint64_t timeout_ns = (uint64_t)play->timeout_ms * 1000000ULL;

	if (now_ns > play->start_ns &&
	    now_ns - play->start_ns >= (uint64_t)UDSP_TELEV_MAX_MS * 1000000ULL)
		return true;
	/* The end came, so no update from the far end is due and the event
	 * renders out what that end announced; the bound is for an end that
	 * never comes. */
	if (play->ended)
		return false;

	return now_ns > play->last_ns && now_ns - play->last_ns >= timeout_ns;
}

bool telev_play_tick(struct telev_play *play, uint64_t now_ns, uint32_t *left)
{
	*left = 0;
	if (!play->active || play->done)
		return false;
	if (play->ended && play->rendered >= play->target) {
		play->done = true;
		return false;
	}
	if (play_expired(play, now_ns)) {
		play->done = true;
		return false;
	}
	*left = play->ended ? play->target - play->rendered : UINT32_MAX;

	return true;
}

void telev_play_advance(struct telev_play *play, uint32_t units)
{
	play->rendered += units;
}
