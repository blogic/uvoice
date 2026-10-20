// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <errno.h>
#include <string.h>

#include "rtp.h"

uint8_t media_event_pt_get(const struct udsp_media *media)
{
	if (media->dtmf_mode != UDSP_DTMF_RFC2833 || !media->dtmf_pt ||
	    media->dtmf_pt > 127U || media->dtmf_pt == media->codec_pt)
		return RTP_PT_DISABLED;
	return media->dtmf_pt;
}

static bool media_direction_sends(uint8_t direction)
{
	return direction == UDSP_MEDIA_SENDONLY ||
	       direction == UDSP_MEDIA_SENDRECV;
}

int rtp_sender_init(struct rtp_sender *sender,
		    const struct udsp_media *media,
		    const struct rtp_entropy *entropy,
		    struct rtp_stats *stats)
{
	if (!sender || !media || !entropy || media->codec_pt > 127U)
		return -EINVAL;
	memset(sender, 0, sizeof(*sender));
	sender->stats = stats;
	sender->ssrc = entropy->ssrc;
	sender->sequence = (uint16_t)entropy->sequence;
	sender->timestamp = entropy->timestamp;
	sender->payload_type = media->codec_pt;
	sender->event_pt = media_event_pt_get(media);
	sender->codec = media->codec;
	sender->ptime_ms = media->ptime_ms;
	sender->direction = media->direction;
	sender->marker_pending = true;
	return 0;
}

int rtp_sender_reconfigure(struct rtp_sender *sender,
			   const struct udsp_media *media)
{
	bool description_changed;
	bool sending_started;
	uint8_t event_pt;

	if (media->codec_pt > 127U)
		return -1;
	event_pt = media_event_pt_get(media);
	description_changed = sender->payload_type != media->codec_pt ||
			      sender->event_pt != event_pt ||
			      sender->codec != media->codec ||
			      sender->ptime_ms != media->ptime_ms;
	sending_started = !media_direction_sends(sender->direction) &&
			  media_direction_sends(media->direction);
	sender->payload_type = media->codec_pt;
	sender->event_pt = event_pt;
	sender->codec = media->codec;
	sender->ptime_ms = media->ptime_ms;
	sender->direction = media->direction;
	if (description_changed || sending_started)
		sender->marker_pending = true;
	return 0;
}

void rtp_sender_restart(struct rtp_sender *sender)
{
	sender->marker_pending = true;
}

int rtp_header_build(struct rtp_sender *sender, uint8_t payload_type,
		     bool marker, uint32_t timestamp, uint8_t *packet,
		     size_t capacity)
{
	if (capacity < RTP_HEADER_SIZE || payload_type > 127U)
		return -1;
	packet[0] = 0x80U;
	packet[1] = payload_type | (marker ? 0x80U : 0U);
	rtp_u16_write(packet + 2, sender->sequence++);
	rtp_u32_write(packet + 4, timestamp);
	rtp_u32_write(packet + 8, sender->ssrc);
	return 0;
}

int rtp_packet_build(struct rtp_sender *sender, const uint8_t *payload,
		     size_t payload_len, uint32_t samples, uint8_t *packet,
		     size_t capacity, size_t *packet_len)
{
	bool marker;

	if (!packet_len || payload_len > capacity ||
	    capacity - payload_len < RTP_HEADER_SIZE)
		return -1;
	marker = sender->marker_pending;
	if (rtp_header_build(sender, sender->payload_type, marker,
			     sender->timestamp, packet, capacity) < 0)
		return -1;
	if (payload_len)
		memmove(packet + RTP_HEADER_SIZE, payload, payload_len);
	*packet_len = RTP_HEADER_SIZE + payload_len;
	sender->timestamp += samples;
	sender->marker_pending = false;
	if (sender->stats) {
		sender->stats->packets_sent++;
		sender->stats->bytes_sent += *packet_len;
		sender->stats->octets_sent += payload_len;
	}
	return 0;
}

static void receiver_sequence_init(struct rtp_receiver *receiver, uint16_t seq)
{
	receiver->base_seq = seq;
	receiver->max_seq = seq;
	receiver->bad_seq = RTP_SEQ_MOD + 1U;
	receiver->cycles = 0;
	receiver->received = 0;
	receiver->received_prior = 0;
	receiver->expected_prior = 0;
	receiver->transit_valid = false;
	receiver->jitter_q4 = 0;
}

static bool receiver_sequence_update(struct rtp_receiver *receiver, uint16_t seq)
{
	uint16_t delta = (uint16_t)(seq - receiver->max_seq);

	/* RFC 3550 A.1 requires two sequential packets before source acceptance. */
	if (receiver->probation) {
		if (seq == (uint16_t)(receiver->max_seq + 1U)) {
			receiver->max_seq = seq;
			receiver->probation--;
			if (!receiver->probation) {
				receiver_sequence_init(receiver, seq);
				receiver->received++;
				return true;
			}
		} else {
			receiver->probation = RTP_MIN_SEQUENTIAL - 1U;
			receiver->max_seq = seq;
		}
		return false;
	}
	if (delta < RTP_MAX_DROPOUT) {
		if (seq < receiver->max_seq)
			receiver->cycles += RTP_SEQ_MOD;
		receiver->max_seq = seq;
	} else if (delta <= RTP_SEQ_MOD - RTP_MAX_MISORDER) {
		if (seq == receiver->bad_seq) {
			receiver_sequence_init(receiver, seq);
		} else {
			receiver->bad_seq = (seq + 1U) & (RTP_SEQ_MOD - 1U);
			return false;
		}
	}
	receiver->received++;
	return true;
}

/* RFC 3550 6.4.1 measures the transit in the stream's own timestamp units, so
 * the arrival instant is carried at the same clock the packet is stamped in. */
static uint32_t arrival_timestamp(uint64_t arrival_ns, uint32_t clock_rate)
{
	uint64_t seconds = arrival_ns / 1000000000ULL;
	uint64_t remainder = arrival_ns % 1000000000ULL;

	return (uint32_t)(seconds * clock_rate +
			  remainder * clock_rate / 1000000000ULL);
}

static void receiver_jitter_update(struct rtp_receiver *receiver,
				   uint32_t timestamp, uint64_t arrival_ns,
				   bool marker)
{
	int32_t transit;
	int32_t delta;
	uint32_t delta_magnitude;
	uint32_t jitter;
	bool marked_step;

	transit = (int32_t)(arrival_timestamp(arrival_ns, receiver->clock_rate) -
			    timestamp);
	if (!receiver->transit_valid) {
		receiver->transit = transit;
		receiver->transit_valid = true;
		return;
	}
	/* This implementation-defined cast preserves the required 32-bit wrap. */
	delta = (int32_t)((uint32_t)transit - (uint32_t)receiver->transit);
	receiver->transit = transit;
	delta_magnitude = delta < 0 ? 0U - (uint32_t)delta : (uint32_t)delta;
	marked_step = marker && delta < 0 && delta_magnitude >
		      RTP_MARKED_STEP_TS(receiver->clock_rate);
	if (marked_step ||
	    delta_magnitude >= RTP_TRANSIT_STEP_TS(receiver->clock_rate)) {
		if (receiver->stats)
			receiver->stats->ts_steps++;
		return;
	}
	/* RFC 3550 A.8 keeps four fractional bits to avoid floating point. */
	receiver->jitter_q4 += delta_magnitude -
			       ((receiver->jitter_q4 + 8U) >> 4);
	jitter = receiver->jitter_q4 >> 4;
	if (!receiver->stats)
		return;
	receiver->stats->jitter_sum += jitter;
	receiver->stats->jitter_samples++;
	if (jitter > receiver->stats->jitter_max)
		receiver->stats->jitter_max = jitter;
}

int rtp_receiver_init_cfg(struct rtp_receiver *receiver,
			  const struct udsp_media *media, uint32_t clock_rate,
			  struct rtp_stats *stats)
{
	if (!receiver || !media || media->codec_pt > 127U)
		return -EINVAL;
	memset(receiver, 0, sizeof(*receiver));
	receiver->stats = stats;
	receiver->clock_rate = clock_rate ? clock_rate : RTP_CLOCK_RATE;
	receiver->audio_pt = media->codec_pt;
	receiver->event_pt = media_event_pt_get(media);
	/* Codec-native DTX forces the generic cng flag off (stream.c
	 * media_prepare), yet the peer may have negotiated PT 13 in our offer
	 * and send it during the same call: the classification stays open for
	 * either silence machinery. media_prepare clears dtx on a codec
	 * without a SID frame, so the flag is trustworthy here. */
	receiver->cng_enabled = media->cng || media->dtx;
	return 0;
}

int rtp_receiver_init(struct rtp_receiver *receiver,
		      const struct udsp_media *media, struct rtp_stats *stats)
{
	return rtp_receiver_init_cfg(receiver, media, RTP_CLOCK_RATE, stats);
}

int rtp_receiver_reconfigure(struct rtp_receiver *receiver,
			     const struct udsp_media *media)
{
	if (media->codec_pt > 127U)
		return -1;
	receiver->audio_pt = media->codec_pt;
	receiver->event_pt = media_event_pt_get(media);
	/* The same gate rule as rtp_receiver_init_cfg. */
	receiver->cng_enabled = media->cng || media->dtx;
	return 0;
}

static int receiver_discard(struct rtp_receiver *receiver)
{
	if (receiver->stats)
		receiver->stats->packets_discarded++;
	return -1;
}

static bool receiver_kind_get(const struct rtp_receiver *receiver,
			      uint8_t payload_type,
			      enum rtp_payload_kind *kind)
{
	if (payload_type == receiver->audio_pt)
		*kind = RTP_PAYLOAD_AUDIO;
	else if (payload_type == receiver->event_pt)
		*kind = RTP_PAYLOAD_TELEPHONE_EVENT;
	else if (receiver->cng_enabled && payload_type == 13U)
		*kind = RTP_PAYLOAD_COMFORT_NOISE;
	else
		return false;
	return true;
}

/*
 * RFC 3550 A.1 probation holds a new source's first packet, the only one that
 * RTP_MIN_SEQUENTIAL 2 puts on probation. The receiver releases it when its
 * successor proves the source. The RFC allows the hold in place of the
 * discard ("discarded (or delayed in a queue)"). Reason: a discard cuts the
 * first packet, 20 ms at ptime 20, from a far end that opens a stream on a
 * tone. The held packet gives no jitter observation and no RFC 3550 A.3
 * reception, as in the RFC. packets_recv counts it once the caller takes it.
 */
_Static_assert(RTP_MIN_SEQUENTIAL == 2U,
	       "the hold keeps one packet, the only one on probation");

static void receiver_held_drop(struct rtp_receiver *receiver)
{
	if (receiver->held.state == RTP_HELD_NONE)
		return;
	receiver->held.state = RTP_HELD_NONE;
	receiver_discard(receiver);
}

/* A larger payload than the jitter buffer takes is discarded, not held. */
static int receiver_hold(struct rtp_receiver *receiver, const uint8_t *packet,
			 size_t header_len, size_t payload_len,
			 size_t packet_end, uint64_t arrival_ns)
{
	struct rtp_held *held = &receiver->held;

	receiver_held_drop(receiver);
	if (payload_len > RTP_HELD_PAYLOAD_MAX)
		return receiver_discard(receiver);
	memcpy(held->payload, packet + header_len, payload_len);
	held->len = (uint16_t)payload_len;
	held->bytes = (uint32_t)packet_end;
	held->arrival_ns = arrival_ns;
	held->timestamp = rtp_u32_read(packet + 4);
	held->sequence = rtp_u16_read(packet + 2);
	held->payload_type = packet[1] & 0x7fU;
	held->csrc_count = packet[0] & 0x0fU;
	held->marker = (packet[1] & 0x80U) != 0;
	held->state = RTP_HELD_PROBATION;
	return -1;
}

/* The packet at sequence proved the source. Probation restarts on every gap,
 * so the held packet is its predecessor; the test only guards the release. */
static void receiver_held_release(struct rtp_receiver *receiver,
				  uint16_t sequence)
{
	struct rtp_held *held = &receiver->held;

	if (held->state != RTP_HELD_PROBATION)
		return;
	if (held->sequence != (uint16_t)(sequence - 1U)) {
		receiver_held_drop(receiver);
		return;
	}
	held->state = RTP_HELD_RELEASED;
}

bool rtp_receiver_held_take(struct rtp_receiver *receiver,
			    struct rtp_packet *out)
{
	struct rtp_held *held = &receiver->held;
	enum rtp_payload_kind kind;

	if (held->state != RTP_HELD_RELEASED)
		return false;
	if (!receiver_kind_get(receiver, held->payload_type, &kind)) {
		receiver_held_drop(receiver);
		return false;
	}
	held->state = RTP_HELD_NONE;
	memset(out, 0, sizeof(*out));
	out->payload = held->payload;
	out->payload_len = held->len;
	out->ssrc = receiver->ssrc;
	out->timestamp = held->timestamp;
	out->sequence = held->sequence;
	out->payload_type = held->payload_type;
	out->csrc_count = held->csrc_count;
	out->kind = kind;
	out->marker = held->marker;
	out->arrival_ns = held->arrival_ns;
	if (receiver->stats) {
		receiver->stats->packets_recv++;
		receiver->stats->bytes_recv += held->bytes;
	}
	return true;
}

/*
 * RFC 3550 11 lets one lower-layer PDU carry several RTP packets when the
 * lower layer supplies a length, and UDP does. A field capture on AN7581
 * shows a far end that delivers two 20 ms G.722 packets in one 344 byte
 * datagram, the second header at offset 172 carrying the successor sequence
 * number, the next timestamp and the same SSRC. Whether the sender coalesced
 * them or a local UDP receive offload merged them is not established, so the
 * guard holds for both.
 *
 * RTP carries no length of its own, so the only thing that marks the split is
 * the next packet's own header. Version 2, the successor sequence number and
 * the same SSRC is fifty bits of exact match, which over the few hundred
 * offsets of one datagram puts a false split near 2^-41; a payload byte that
 * misses the two version bits ends the test at its first compare.
 *
 * Reading the trailing bytes as payload is not the safe default. The buffer
 * slices the oversized "frame" into halves no codec accepts, so the first
 * packet is concealed away as well as the second. The receiver also reports
 * each unseen sequence number upstream as network loss that did not occur.
 * In a 233 s field call, 13 such datagrams cost 520 ms of dead audio.
 *
 * Returns the offset the packet ends at, which is packet_len when the
 * datagram holds one packet.
 */
static size_t rtp_tail_find(const uint8_t *packet, size_t packet_len,
			    size_t header_len, uint16_t sequence, uint32_t ssrc)
{
	uint16_t want = (uint16_t)(sequence + 1U);
	size_t off;

	/* From the first payload byte, because a packet with no payload at all
	 * is legal and two of them in one PDU still have to come apart. */
	for (off = header_len; off + RTP_HEADER_SIZE <= packet_len; off++) {
		if ((packet[off] >> 6) != 2U)
			continue;
		if (rtp_u16_read(packet + off + 2U) != want)
			continue;
		if (rtp_u32_read(packet + off + 8U) != ssrc)
			continue;
		return off;
	}
	return packet_len;
}

/*
 * The caller stopped walking a PDU with packets still inside it, which
 * RTP_PDU_PACKETS_MAX makes possible. Those packets arrived here and this box
 * dropped them, so they are discards, not losses. Left uncounted, they surface
 * as RFC 3550 A.3 cumulative_lost in the next receiver report and blame the
 * network for a refusal that happened here.
 *
 * The walk reads framing only. It never touches the sequence window, the
 * jitter estimate or the reception count, so a refused tail cannot move a
 * number that belongs to a delivered packet: the packets are absent from
 * `received` because they were never delivered, and present in
 * packets_discarded because they were thrown away here. That is the same
 * split every other refusal in this file already reports.
 */
void rtp_receiver_pdu_residual_discard(struct rtp_receiver *receiver,
				       const uint8_t *data, size_t len)
{
	unsigned counted = 0;

	if (!receiver || !receiver->stats || !data)
		return;
	while (len >= RTP_HEADER_SIZE && counted < RTP_PDU_RESIDUAL_MAX) {
		size_t header_len = RTP_HEADER_SIZE +
				    (size_t)(data[0] & 0x0fU) * 4U;
		size_t end;

		if ((data[0] >> 6) != 2U || len < header_len)
			break;
		/* rtp_tail_find answers len when nothing follows, and never
		 * less than header_len, so the residual shrinks every turn. */
		end = rtp_tail_find(data, len, header_len,
				    rtp_u16_read(data + 2),
				    rtp_u32_read(data + 8));
		counted++;
		data += end;
		len -= end;
	}
	receiver->stats->packets_discarded += counted;
}

int rtp_packet_parse(struct rtp_receiver *receiver, const uint8_t *packet,
		     size_t packet_len, uint64_t arrival_ns,
		     struct rtp_packet *out)
{
	enum rtp_payload_kind kind;
	uint32_t ssrc;
	uint32_t timestamp;
	size_t header_len;
	size_t payload_len;
	size_t packet_end;
	size_t extension_len;
	uint16_t sequence;
	uint16_t extension_words;
	uint8_t payload_type;
	uint8_t csrc_count;
	uint8_t padding_count;
	bool probation;

	/* A released packet that the caller did not take is lost here. */
	if (receiver->held.state == RTP_HELD_RELEASED)
		receiver_held_drop(receiver);
	if (!packet || !out || packet_len < RTP_HEADER_SIZE)
		return receiver_discard(receiver);
	if ((packet[0] >> 6) != 2U)
		return receiver_discard(receiver);
	/* The split scan below needs the identity, and every field it reads
	 * sits inside the fixed header the length test above already cleared. */
	payload_type = packet[1] & 0x7fU;
	sequence = rtp_u16_read(packet + 2);
	timestamp = rtp_u32_read(packet + 4);
	ssrc = rtp_u32_read(packet + 8);
	csrc_count = packet[0] & 0x0fU;
	header_len = RTP_HEADER_SIZE + (size_t)csrc_count * 4U;
	if (packet_len < header_len)
		return receiver_discard(receiver);
	if (packet[0] & 0x10U) {
		if (packet_len - header_len < 4U)
			return receiver_discard(receiver);
		extension_words = rtp_u16_read(packet + header_len + 2U);
		extension_len = 4U + (size_t)extension_words * 4U;
		if (packet_len - header_len < extension_len)
			return receiver_discard(receiver);
		header_len += extension_len;
	}
	packet_end = rtp_tail_find(packet, packet_len, header_len, sequence, ssrc);
	payload_len = packet_end - header_len;
	/* The padding count byte belongs to the packet that declared the P bit,
	 * so it sits at the end of this packet, not at the end of the PDU. */
	if (packet[0] & 0x20U) {
		if (!payload_len)
			return receiver_discard(receiver);
		padding_count = packet[packet_end - 1U];
		if (!padding_count || padding_count > payload_len)
			return receiver_discard(receiver);
		payload_len -= padding_count;
	}
	if (!receiver->source_valid || receiver->ssrc != ssrc) {
		receiver_held_drop(receiver);
		receiver->ssrc = ssrc;
		receiver->source_valid = true;
		receiver->probation = RTP_MIN_SEQUENTIAL;
		receiver->max_seq = sequence;
		receiver->transit_valid = false;
		receiver->jitter_q4 = 0;
	}
	probation = receiver->probation != 0;
	if (!receiver_sequence_update(receiver, sequence)) {
		if (receiver->probation)
			return receiver_hold(receiver, packet, header_len,
					     payload_len, packet_end,
					     arrival_ns);
		return receiver_discard(receiver);
	}
	if (probation)
		receiver_held_release(receiver, sequence);
	if (!receiver_kind_get(receiver, payload_type, &kind))
		return receiver_discard(receiver);
	if (kind == RTP_PAYLOAD_AUDIO)
		receiver_jitter_update(receiver, timestamp, arrival_ns,
				       (packet[1] & 0x80U) != 0);
	memset(out, 0, sizeof(*out));
	out->payload = packet + header_len;
	out->payload_len = payload_len;
	out->ssrc = ssrc;
	out->timestamp = timestamp;
	out->sequence = sequence;
	out->payload_type = payload_type;
	out->csrc_count = csrc_count;
	out->kind = kind;
	out->marker = (packet[1] & 0x80U) != 0;
	out->arrival_ns = arrival_ns;
	out->tail = packet_end < packet_len ? packet + packet_end : NULL;
	out->tail_len = packet_len - packet_end;
	if (receiver->stats) {
		receiver->stats->packets_recv++;
		/* One PDU of two packets is two receptions of its own bytes,
		 * not two of the whole datagram. */
		receiver->stats->bytes_recv += packet_end;
	}
	return 0;
}

uint32_t rtp_receiver_expected_get(const struct rtp_receiver *receiver)
{
	if (!receiver || !receiver->source_valid || receiver->probation)
		return 0;
	return receiver->cycles + receiver->max_seq - receiver->base_seq + 1U;
}

void rtp_receiver_report(struct rtp_receiver *receiver, uint32_t lsr,
			 uint32_t dlsr, struct rtcp_report *report)
{
	uint32_t extended_max;
	uint32_t expected;
	uint32_t expected_interval;
	uint32_t received_interval;
	uint64_t fraction;
	int64_t cumulative_lost;
	int64_t lost_interval;

	memset(report, 0, sizeof(*report));
	report->ssrc = receiver->ssrc;
	if (!receiver->source_valid || receiver->probation)
		return;
	extended_max = receiver->cycles + receiver->max_seq;
	expected = rtp_receiver_expected_get(receiver);
	cumulative_lost = (int64_t)expected - receiver->received;
	if (cumulative_lost > 0x7fffff)
		cumulative_lost = 0x7fffff;
	if (cumulative_lost < -0x800000)
		cumulative_lost = -0x800000;
	expected_interval = expected - receiver->expected_prior;
	receiver->expected_prior = expected;
	received_interval = receiver->received - receiver->received_prior;
	receiver->received_prior = receiver->received;
	lost_interval = (int64_t)expected_interval - received_interval;
	if (expected_interval && lost_interval > 0) {
		fraction = ((uint64_t)lost_interval << 8) / expected_interval;
		report->fraction_lost = fraction > 255U ? 255U : (uint8_t)fraction;
	}
	report->cumulative_lost = (int32_t)cumulative_lost;
	report->highest_seq = extended_max;
	report->jitter = receiver->jitter_q4 >> 4;
	report->lsr = lsr;
	report->dlsr = dlsr;
	if (receiver->stats)
		receiver->stats->loss_fraction = report->fraction_lost;
}
