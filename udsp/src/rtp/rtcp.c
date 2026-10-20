// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <string.h>

#include "rtp.h"

#define RTCP_SR		200U
#define RTCP_RR		201U
#define RTCP_SDES	202U
#define RTCP_BYE	203U
#define RTCP_SDES_CNAME	1U
#define RTCP_REPORT_SIZE 24U

static int string_length(const char *string, size_t limit, size_t *length)
{
	size_t i;

	for (i = 0; i <= limit; i++) {
		if (!string[i]) {
			*length = i;
			return 0;
		}
	}
	return -1;
}

static void header_write(uint8_t *packet, uint8_t count, uint8_t type,
			 size_t packet_len)
{
	packet[0] = 0x80U | count;
	packet[1] = type;
	rtp_u16_write(packet + 2, (uint16_t)(packet_len / 4U - 1U));
}

static void report_write(uint8_t *packet, const struct rtcp_report *report)
{
	rtp_u32_write(packet, report->ssrc);
	packet[4] = report->fraction_lost;
	rtp_u24_write(packet + 5, report->cumulative_lost);
	rtp_u32_write(packet + 8, report->highest_seq);
	rtp_u32_write(packet + 12, report->jitter);
	rtp_u32_write(packet + 16, report->lsr);
	rtp_u32_write(packet + 20, report->dlsr);
}

static void report_read(const uint8_t *packet, struct rtcp_report *report)
{
	report->ssrc = rtp_u32_read(packet);
	report->fraction_lost = packet[4];
	report->cumulative_lost = rtp_i24_read(packet + 5);
	report->highest_seq = rtp_u32_read(packet + 8);
	report->jitter = rtp_u32_read(packet + 12);
	report->lsr = rtp_u32_read(packet + 16);
	report->dlsr = rtp_u32_read(packet + 20);
}

int rtcp_sr_build(const struct rtcp_sender_info *sender,
		  const struct rtcp_report *report, uint8_t *packet,
		  size_t capacity, size_t *packet_len)
{
	size_t length = 28U + (report ? RTCP_REPORT_SIZE : 0U);

	if (!sender || !packet || !packet_len || capacity < length)
		return -1;
	header_write(packet, report ? 1U : 0U, RTCP_SR, length);
	rtp_u32_write(packet + 4, sender->ssrc);
	rtp_u32_write(packet + 8, sender->ntp_msw);
	rtp_u32_write(packet + 12, sender->ntp_lsw);
	rtp_u32_write(packet + 16, sender->rtp_timestamp);
	rtp_u32_write(packet + 20, sender->packet_count);
	rtp_u32_write(packet + 24, sender->octet_count);
	if (report)
		report_write(packet + 28, report);
	*packet_len = length;
	return 0;
}

int rtcp_rr_build(uint32_t sender_ssrc, const struct rtcp_report *report,
		  uint8_t *packet, size_t capacity, size_t *packet_len)
{
	size_t length = 8U + (report ? RTCP_REPORT_SIZE : 0U);

	if (!packet || !packet_len || capacity < length)
		return -1;
	header_write(packet, report ? 1U : 0U, RTCP_RR, length);
	rtp_u32_write(packet + 4, sender_ssrc);
	if (report)
		report_write(packet + 8, report);
	*packet_len = length;
	return 0;
}

void rtcp_sender_info_get(const struct rtp_sender *rtp, uint32_t ntp_msw,
			  uint32_t ntp_lsw,
			  struct rtcp_sender_info *sender)
{
	memset(sender, 0, sizeof(*sender));
	sender->ssrc = rtp->ssrc;
	sender->ntp_msw = ntp_msw;
	sender->ntp_lsw = ntp_lsw;
	sender->rtp_timestamp = rtp->timestamp;
	if (!rtp->stats)
		return;
	sender->packet_count = (uint32_t)rtp->stats->packets_sent;
	sender->octet_count = (uint32_t)rtp->stats->octets_sent;
}

int rtcp_sdes_build(uint32_t ssrc, const char *cname, uint8_t *packet,
		    size_t capacity, size_t *packet_len)
{
	size_t cname_len;
	size_t chunk_len;
	size_t length;

	if (!cname || !packet || !packet_len)
		return -1;
	if (string_length(cname, RTCP_CNAME_MAX, &cname_len) < 0 || !cname_len)
		return -1;
	chunk_len = 4U + 2U + cname_len + 1U;
	length = 4U + ((chunk_len + 3U) & ~3U);
	if (capacity < length)
		return -1;
	memset(packet, 0, length);
	header_write(packet, 1, RTCP_SDES, length);
	rtp_u32_write(packet + 4, ssrc);
	packet[8] = RTCP_SDES_CNAME;
	packet[9] = (uint8_t)cname_len;
	memcpy(packet + 10, cname, cname_len);
	*packet_len = length;
	return 0;
}

int rtcp_bye_build(uint32_t ssrc, const char *reason, uint8_t *packet,
		   size_t capacity, size_t *packet_len)
{
	size_t reason_len = 0;
	size_t unpadded = 8U;
	size_t length;

	if (!packet || !packet_len ||
	    (reason && string_length(reason, 255U, &reason_len) < 0))
		return -1;
	if (reason_len)
		unpadded += 1U + reason_len;
	length = (unpadded + 3U) & ~3U;
	if (capacity < length)
		return -1;
	memset(packet, 0, length);
	header_write(packet, 1, RTCP_BYE, length);
	rtp_u32_write(packet + 4, ssrc);
	if (reason_len) {
		packet[8] = (uint8_t)reason_len;
		memcpy(packet + 9, reason, reason_len);
	}
	*packet_len = length;
	return 0;
}

static int report_packet_parse(const uint8_t *packet, size_t usable_len,
			       uint8_t count, uint32_t report_ssrc,
			       struct rtcp_parse_result *out)
{
	uint8_t type = packet[1];
	size_t base = type == RTCP_SR ? 28U : 8U;
	size_t required = base + (size_t)count * RTCP_REPORT_SIZE;
	uint8_t i;

	if (usable_len < required || (usable_len - required) % 4U)
		return -1;
	if (type == RTCP_SR) {
		out->has_sr = true;
		out->sender.ssrc = rtp_u32_read(packet + 4);
		out->sender.ntp_msw = rtp_u32_read(packet + 8);
		out->sender.ntp_lsw = rtp_u32_read(packet + 12);
		out->sender.rtp_timestamp = rtp_u32_read(packet + 16);
		out->sender.packet_count = rtp_u32_read(packet + 20);
		out->sender.octet_count = rtp_u32_read(packet + 24);
	} else {
		out->has_rr = true;
		out->receiver_ssrc = rtp_u32_read(packet + 4);
	}
	for (i = 0; i < count; i++) {
		struct rtcp_report report;

		report_read(packet + base + (size_t)i * RTCP_REPORT_SIZE, &report);
		if (!out->has_report && report.ssrc == report_ssrc) {
			out->report = report;
			out->has_report = true;
		}
	}
	return 0;
}

static int sdes_packet_parse(const uint8_t *packet, size_t usable_len,
			     uint8_t count, uint32_t selected_ssrc,
			     bool select_ssrc,
			     struct rtcp_parse_result *out)
{
	size_t offset = 4U;
	uint8_t chunk;

	for (chunk = 0; chunk < count; chunk++) {
		uint32_t ssrc;
		bool ended = false;

		if (usable_len - offset < 4U)
			return -1;
		ssrc = rtp_u32_read(packet + offset);
		offset += 4U;
		while (offset < usable_len) {
			uint8_t type = packet[offset++];
			uint8_t item_len;

			if (!type) {
				ended = true;
				break;
			}
			if (offset >= usable_len)
				return -1;
			item_len = packet[offset++];
			if (usable_len - offset < item_len)
				return -1;
			if (type == RTCP_SDES_CNAME && !out->has_cname &&
			    (!select_ssrc || ssrc == selected_ssrc)) {
				memcpy(out->cname, packet + offset, item_len);
				out->cname[item_len] = '\0';
				out->cname_ssrc = ssrc;
				out->has_cname = true;
			}
			offset += item_len;
		}
		if (!ended)
			return -1;
		while (offset & 3U) {
			if (offset >= usable_len || packet[offset])
				return -1;
			offset++;
		}
	}
	if (offset != usable_len)
		return -1;
	return 0;
}

static int bye_packet_parse(const uint8_t *packet, size_t usable_len,
			    uint8_t count, struct rtcp_parse_result *out)
{
	size_t offset = 4U + (size_t)count * 4U;

	if (!count || usable_len < offset)
		return -1;
	out->bye_ssrc = rtp_u32_read(packet + 4);
	out->has_bye = true;
	if (offset == usable_len)
		return 0;
	if (usable_len - offset < 1U + packet[offset])
		return -1;
	offset += 1U + packet[offset];
	while (offset < usable_len) {
		if (packet[offset])
			return -1;
		offset++;
	}
	return 0;
}

static int packet_parse(const uint8_t *packet, size_t packet_len,
			uint32_t report_ssrc, uint32_t selected_cname_ssrc,
			bool select_cname_ssrc,
			struct rtcp_parse_result *out)
{
	size_t wire_len;
	size_t usable_len;
	uint8_t count;
	uint8_t padding;

	if (!packet || !out || packet_len < 4U)
		return -1;
	memset(out, 0, sizeof(*out));
	if ((packet[0] >> 6) != 2U)
		return -1;
	wire_len = ((size_t)rtp_u16_read(packet + 2) + 1U) * 4U;
	if (wire_len != packet_len)
		return -1;
	usable_len = wire_len;
	if (packet[0] & 0x20U) {
		padding = packet[wire_len - 1U];
		if (!padding || padding % 4U || padding > wire_len - 4U)
			return -1;
		usable_len -= padding;
	}
	count = packet[0] & 0x1fU;
	switch (packet[1]) {
	case RTCP_SR:
	case RTCP_RR:
		return report_packet_parse(packet, usable_len, count, report_ssrc,
					   out);
	case RTCP_SDES:
		return sdes_packet_parse(packet, usable_len, count,
					 selected_cname_ssrc,
					 select_cname_ssrc, out);
	case RTCP_BYE:
		return bye_packet_parse(packet, usable_len, count, out);
	default:
		return 0;
	}
}

int rtcp_packet_parse(const uint8_t *packet, size_t packet_len,
		      uint32_t report_ssrc,
		      struct rtcp_parse_result *out)
{
	return packet_parse(packet, packet_len, report_ssrc, 0, false, out);
}

static void result_merge(struct rtcp_parse_result *result,
			 const struct rtcp_parse_result *part)
{
	if (part->has_sr) {
		result->has_sr = true;
		result->sender = part->sender;
	}
	if (part->has_rr) {
		result->has_rr = true;
		result->receiver_ssrc = part->receiver_ssrc;
	}
	if (part->has_report) {
		result->has_report = true;
		result->report = part->report;
	}
	if (part->has_cname) {
		result->has_cname = true;
		result->cname_ssrc = part->cname_ssrc;
		memcpy(result->cname, part->cname, sizeof(result->cname));
	}
	if (part->has_bye) {
		result->has_bye = true;
		result->bye_ssrc = part->bye_ssrc;
	}
}

int rtcp_compound_parse(const uint8_t *packet, size_t packet_len,
			uint32_t report_ssrc,
			struct rtcp_parse_result *out)
{
	struct rtcp_parse_result part;
	size_t offset = 0;
	uint32_t primary_ssrc = 0;
	bool parsed_any = false;
	bool saw_sdes = false;
	bool first = true;

	if (!packet || !out)
		return -1;
	memset(out, 0, sizeof(*out));
	while (offset < packet_len) {
		size_t length;
		bool last;

		if (packet_len - offset < 4U)
			return parsed_any ? 0 : -1;
		length = ((size_t)rtp_u16_read(packet + offset + 2) + 1U) * 4U;
		if (length < 4U || length > packet_len - offset)
			return parsed_any ? 0 : -1;
		last = length == packet_len - offset;
		if ((packet[offset] & 0x20U) && !last)
			return parsed_any ? 0 : -1;
		if (first) {
			if ((packet[offset] & 0x20U) ||
			    (packet[offset + 1] != RTCP_SR &&
			     packet[offset + 1] != RTCP_RR) || length < 8U)
				return -1;
			primary_ssrc = rtp_u32_read(packet + offset + 4);
			first = false;
		}
		if (packet[offset + 1U] == RTCP_SDES)
			saw_sdes = true;
		if (packet_parse(packet + offset, length, report_ssrc,
				 primary_ssrc, true, &part) < 0)
			return parsed_any ? 0 : -1;
		result_merge(out, &part);
		parsed_any = true;
		offset += length;
	}
	if (first || (!saw_sdes && !out->has_bye) ||
	    (saw_sdes &&
	     (!out->has_cname || out->cname_ssrc != primary_ssrc)))
		return -1;
	return 0;
}

void rtcp_state_init(struct rtcp_state *state, uint32_t local_ssrc,
		     struct rtp_stats *stats)
{
	memset(state, 0, sizeof(*state));
	state->local_ssrc = local_ssrc;
	state->stats = stats;
}

void rtcp_state_receive(struct rtcp_state *state,
			const struct rtcp_parse_result *packet,
			uint32_t arrival_mono_16_16,
			uint32_t now_ntp_middle)
{
	if (packet->has_sr) {
		state->remote_ssrc = packet->sender.ssrc;
		state->last_sr_lsr = (packet->sender.ntp_msw << 16) |
				     (packet->sender.ntp_lsw >> 16);
		state->last_sr_arrival_mono = arrival_mono_16_16;
		state->last_sr_valid = true;
	}
	if (state->stats && packet->has_report &&
	    packet->report.ssrc == state->local_ssrc)
		rtp_stats_rtcp_report(state->stats, &packet->report,
				      now_ntp_middle);
}

void rtcp_state_report_timing(const struct rtcp_state *state,
			      uint32_t now_mono_16_16, uint32_t *lsr,
			      uint32_t *dlsr)
{
	if (!state->last_sr_valid) {
		*lsr = 0;
		*dlsr = 0;
		return;
	}
	*lsr = state->last_sr_lsr;
	*dlsr = now_mono_16_16 - state->last_sr_arrival_mono;
}
