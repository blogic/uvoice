// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The one host table (proto/proto.h struct proto_host) for every suite that
 * drives protocol code without the core. Every entry counts its calls and
 * keeps the arguments a case most often reads. trace_active and trace_write
 * reach the real trace.c, which the suite compiles in, and
 * calltrace_tap_endpoint_set converts the address the way proto/proto.c does
 * (trace_ring.h trace_endpoint_set). Every other entry is a recorder with a
 * fixed answer the suite can change, unless the suite compiles this file
 * with one of the two groups below.
 *
 *   PHF_NET_PORTS  the four net_port entries reach the real pool of
 *                  net/ports.c, which the suite compiles in.
 *   PHF_NET_CORE   the tracker, the conntrack bounds, the firewall reconcile
 *                  and the two event entries reach the core functions of
 *                  proto/proto.c's own adapters (net/iface.h, net/conntrack.h,
 *                  net/fw_reconcile.h, event.h, calltrace_event), which the
 *                  suite compiles in or records itself.
 *
 * The suite calls proto_host_fake_reset() and hands proto_host_fake() to the
 * ops table under test as its host. The entries that act on a line or a
 * stream also append a record of the whole call to hop[], in call order.
 */
#ifndef UVOICE_TEST_PROTO_HOST_FAKE_H
#define UVOICE_TEST_PROTO_HOST_FAKE_H

#include <stdbool.h>
#include <stdint.h>

#include "proto/proto.h"

enum phf_entry {
	PHF_TONE_PLAY = 0,
	PHF_TONE_STOP,
	PHF_RING_START,
	PHF_RING_STOP,
	PHF_CW_ALERT,
	PHF_CID_SEND,
	PHF_RING_TEMPLATE_MATCH,
	PHF_MWI_SET,
	PHF_MWI_GET,
	PHF_DETECTOR_ARM,
	PHF_FWD_DISC_SIGNAL,
	PHF_FWD_DISC_ABORT,
	PHF_BATTERY_SET,
	PHF_DIGIT_PLAY,
	PHF_DIGIT_PLAY_CANCEL,
	PHF_MEDIA_CONFIG_SET,
	PHF_MEDIA_START,
	PHF_MEDIA_STOP,
	PHF_MEDIA_DTMF_SEND,
	PHF_MEDIA_STATS_SNAPSHOT,
	PHF_MEDIA_FAX_APPLY,
	PHF_MEDIA_LEC_LATCH_CLEAR,
	PHF_MEDIA_RAND_BYTES,
	PHF_STREAM_STATS_GET,
	PHF_STATS_LIVE_QUERY,
	PHF_STATS_LIVE_DROP,
	PHF_LINE_EXISTS,
	PHF_LINE_IN_SERVICE,
	PHF_HAL_READY,
	PHF_LINE_OOS_FORCE,
	PHF_LINE_OOS_CLEAR,
	PHF_LINE_TRANSITION,
	PHF_LINE_SETTLE,
	PHF_LINE_CFG_LANDED,
	PHF_HAL_FEATURES,
	PHF_HAL_CODECS,
	PHF_HAL_CAP,
	PHF_REGION,
	PHF_CONF_SET,
	PHF_LINE_FAULT,
	PHF_LOG,
	PHF_CFG,
	PHF_CONFIG_GENERATION,
	PHF_UBUS_CTX,
	PHF_UBUS_REQUEST_REPLY,
	PHF_REG_STATE_POST,
	PHF_TLS_CERT_START,
	PHF_TLS_CERT_STOP,
	PHF_NET_IFACE_SUBSCRIBE,
	PHF_NET_IFACE_STATE_GET,
	PHF_NET_IFACE_BIND_DEVICE,
	PHF_NET_IFACE_SOURCE_HOST_PROBE,
	PHF_NET_IFACE_IPV6_SERVED_SET,
	PHF_NET_IFACE_RECOVER_SETTLE,
	PHF_NET_PORT_RANGE,
	PHF_NET_PORT_RANGE_EQ,
	PHF_NET_PORT_ALLOC,
	PHF_NET_PORT_FREE,
	PHF_NET_CT_UDP_BOUND_S,
	PHF_NET_CT_PUNCH_INTERVAL_MS,
	PHF_FW_ASK,
	PHF_FW_CLEAR,
	PHF_FW_PROXY_ADD,
	PHF_CDR_CALL_STARTED,
	PHF_CDR_CALL_SEQ,
	PHF_CDR_CALL_CORRELATE,
	PHF_CDR_CALL_ANSWERED,
	PHF_CDR_CALL_CONFERENCED,
	PHF_CDR_CALL_ENDED,
	PHF_CALLTRACE_CALL_STARTED,
	PHF_CALLTRACE_EVENT,
	PHF_CALLTRACE_STATE,
	PHF_CALLTRACE_TAP_ENDPOINT_SET,
	PHF_SHUTDOWN_DONE,
	PHF_RANDOM_BYTES,
	PHF_COUNT,
};

/*
 * One call of an entry that acts on a line or a stream, in call order, with
 * the whole of its arguments: the tones, the ring, the alert, the caller id,
 * the message waiting, the detector, the battery, the digits, the stream ops
 * and the mixer. val holds the scalar argument of the entry: the tone, the
 * ring template, the alert variant, the caller id kind, the detect mask or
 * the battery. A suite that reads a sequence of calls reads hop[].
 */
#define PHF_HOP_MAX	96

struct phf_hop {
	uint8_t entry;			/* enum phf_entry */
	unsigned int line;
	unsigned int stream;
	unsigned int val;
	uint32_t duration_ms;
	bool on;
	char digit;
	bool cid_set;
	struct proto_cid cid;
	struct hal_media media;
	struct hal_fax_profile fax;
};

/*
 * What the recorder holds. n[] counts the calls of each entry; the fields
 * below keep the last arguments. The answers are what the entries return,
 * and a case moves them before it calls the code under test.
 */
struct phf_rec {
	unsigned int n[PHF_COUNT];

	unsigned int line;
	unsigned int stream;
	enum hal_tone tone;
	enum proto_ring_kind ring_kind;
	unsigned int state_id;
	uint32_t flags;
	char digit;
	bool on;
	char before[32];
	char after[32];
	char reason[64];
	char verb[32];
	char log_text[256];
	int log_level;
	struct fw_sig_ask fw_ask;
	struct net_iface_sub sub;
	bool sub_set;

	/* the answers */
	enum hal_status status;
	bool line_exists;
	bool line_in_service;
	bool hal_ready;
	bool accept;
	uint32_t hal_features;
	uint32_t hal_codecs;
	const struct cfg *cfg;
	uint32_t config_generation;
	uint32_t cdr_seq;
	unsigned int ct_udp_bound_s;
	uint16_t punch_interval_ms;
	/* end the uloop in shutdown_done, the way proto/proto.c does */
	bool shutdown_ends_loop;

	/* the call sequence; a call past PHF_HOP_MAX is counted in n[] alone */
	struct phf_hop hop[PHF_HOP_MAX];
	unsigned int nhop;
	/* the answer of ring_template_match */
	unsigned int ring_template;
	/* stream_stats_get copies stats and answers stats_status */
	struct hal_media_stats stats;
	enum hal_status stats_status;
	/* the answer of region, a zeroed region when NULL */
	const struct hal_region *region;
	/* each of the next N calls of the stream op answers HAL_FATAL, then
	 * status again */
	unsigned int config_set_fail;
	unsigned int start_fail;
	unsigned int stop_fail;
	/*
	 * The digits toward the phone, as line/line.c holds them: one active
	 * digit that plays at once on an idle line, and a queue of at most
	 * digit_q_max behind it (LINE_DIGIT_Q_MAX). digit_play_cancel empties
	 * the queue and leaves the active digit; the fake never ends one.
	 */
	bool digit_active;
	unsigned int digit_q;
	unsigned int digit_q_max;
	/* the inbound flag of the last cdr_call_started and the report of the
	 * last cdr_call_ended; its strings are the caller's and may be gone */
	bool cdr_inbound;
	struct proto_call_ended cdr_ended;
	/* the answer of net_iface_state_get without PHF_NET_CORE, an empty
	 * record when NULL */
	const struct net_iface_state *iface;
	/* the answer of net_iface_source_host_probe without PHF_NET_CORE, no
	 * address when NULL */
	const char *probe_addr;
};

extern struct phf_rec phf;

/* Zero the recorder and set the answers of a line in service on a ready DSP
 * that accepts every request. */
void proto_host_fake_reset(void);

/* The host table, filled once; ctx and media.priv point at the recorder. */
const struct proto_host *proto_host_fake(void);

#endif
