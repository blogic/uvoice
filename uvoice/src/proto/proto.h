// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The signaling protocol contract. The core (the line shell, the HAL, the
 * network tracker, the firewall reconcile, the records and the northbound)
 * offers its services through struct proto_host. A signaling protocol (SIP,
 * MGCP) exports struct proto_ops. One protocol runs per daemon and it owns
 * every line. The build selects which protocols the image carries, and
 * voip.base.protocol selects the one that runs (proto_select_from_cfg).
 *
 * Append rule. This header is append only, like hal.h. A new member goes at
 * the end of its record, and a new value at the end of its enum. No record
 * carries a version field or a size check, because the core and every
 * protocol come from one build.
 *
 * Include rule. A protocol unit includes these headers and no other header
 * of the core:
 *   - proto/proto.h, proto_media.h, proto_tap.h, proto_trace.h and the headers
 *     of proto/shared/;
 *   - cfg/cfg.h and hal/hal.h;
 *   - dialplan.h, str.h and posix.h.
 *
 * proto/proto.h brings trace_domain.h, which holds one enum. str.h and posix.h
 * are header only. posix.h holds the monotonic clock helpers, uvoice_mono_ms
 * and uvoice_stamp_nonzero among them. Every other core service a protocol
 * needs is an entry of struct proto_host.
 *
 * Link rule. dialplan.c is the one core object that every protocol links,
 * because a protocol compiles and matches its digit maps through dialplan.h.
 * A protocol suite links a fake host table and no other core object. A call
 * into the core that bypasses the table then fails the link of the suite.
 *
 * Re-entrancy. host->ring_start can call ops->ring_held and ops->ring_started
 * before it returns. ops->ring_failed always arrives one uloop pass later.
 * host->line_oos_force and host->line_oos_clear take effect one uloop pass
 * later, because the service sync calls back into ops->line_service. The
 * core delivers ops->line_service and ops->line_event in the order it decides
 * them, and the protocol must not reorder them: the seize path forces the
 * line out of service before it delivers the SEIZE.
 *
 * Mandatory entries. A protocol fills name, init, start, reload, done,
 * line_event, line_service, line_reset, line_flags, status_dump_call,
 * status_dump_lines, shutdown_begin and shutdown_pending.
 * proto_ops_complete() checks them, and proto_select() refuses a table that
 * fails the check. Every other ops entry can be NULL, and the core tests it
 * before each call.
 *
 * Threads. Every entry of both tables runs on the uloop thread.
 */
#ifndef UVOICE_PROTO_H
#define UVOICE_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "cfg/cfg.h"
#include "hal/hal.h"
#include "proto/proto_media.h"
#include "trace_domain.h"

struct blob_buf;
struct proto_tap_endpoints;
struct trace_endpoint;
struct ubus_context;
struct ubus_method;
struct ubus_request_data;

/* ------------------------------------------------------------- the types */

/*
 * The identity of a caller, for the ring, the call waiting alert and a bare
 * caller id push. present false sends no record at all. number_private and
 * name_private say what the subscriber may see: the backend renders a
 * withheld field as the GR-30 reason letter instead of the value. datetime is
 * MMDDHHMM, and empty lets the DSP fill it (hal.h struct hal_cid).
 *
 * Gate rule. The core applies the channel gates of its own channel copy to
 * every protocol. cid_remote_enable gates the ring record and the alert
 * record, and vmwi gates the visual message waiting record. The audible
 * choice (amwi) belongs to the protocol, which reads the store through
 * host->mwi_get.
 */
struct proto_cid {
	char number[HAL_CID_STR_MAX];
	char name[HAL_CID_STR_MAX];
	char datetime[HAL_CID_STR_MAX];
	bool number_private;
	bool name_private;
	bool present;
};

/*
 * Which cadence a ring plays. CALL rings for a caller who waits for an
 * answer, with its template and its caller id. RECALL rings back a held call
 * on an on-hook line, with the plain cadence and no caller id.
 */
enum proto_ring_kind {
	PROTO_RING_CALL = 0,
	PROTO_RING_RECALL,
};

/*
 * The loop feed a protocol asks for (host->battery_set). ACTIVE is the normal
 * feed. DOWN removes the battery. REVERSE reverses the polarity and needs
 * HAL_FEAT_POLARITY.
 */
enum proto_battery {
	PROTO_BATT_ACTIVE = 0,
	PROTO_BATT_DOWN,
	PROTO_BATT_REVERSE,
};

/*
 * The flags of one line, from the state of the protocol's own machine
 * (ops->line_flags).
 *
 *   PROTO_LF_OFFHOOK  the machine holds the handset up.
 *   PROTO_LF_CALL     a call, or a held call, that a line test would end.
 *   PROTO_LF_SESSION  a subscriber interaction in progress that a test would
 *                     disrupt: dialing, digit collection, a clearing chain.
 *
 * The SIP machine (proto/sip/fsm.h enum fsm_state) maps its states as follows.
 * "+held" adds PROTO_LF_CALL while the machine holds a held leg
 * (struct fsm other_held).
 *
 *   FSM_IDLE, FSM_OOS                        0
 *   FSM_OFFHOOK_DIAL, FSM_COLLECTING,        OFFHOOK | SESSION, +held
 *   FSM_SPRE_COLLECT, FSM_CLEARING
 *   FSM_ROH, FSM_SILENT                      OFFHOOK, +held
 *   FSM_CALLING, FSM_PROCEEDING,             OFFHOOK | CALL
 *   FSM_CONNECTED, FSM_CW_OFFERED,
 *   FSM_HOLD_DIAL, FSM_TWO_CALLS, FSM_CONF,
 *   FSM_PENDING, FSM_XFER_COLLECT
 *   FSM_RINGING, FSM_HOLDING                 CALL
 *
 * The core reads the flags and its own oos bit, and derives these predicates
 * from them:
 *   - a line test may run on a line when !(flags & (CALL | SESSION)), and
 *     on the board when no line has PROTO_LF_CALL (line/line.c
 *     line_test_state_safe and line_test_call_active);
 *   - a visual message waiting record may go out when flags == 0 && !oos &&
 *     !ringing (line/line.c mwi_at_rest);
 *   - the handset is up when flags & PROTO_LF_OFFHOOK (line/line.c
 *     hook_offhook_is);
 *   - flags == 0 is IDLE when oos is clear and OOS when it is set, which the
 *     call trace reads.
 *
 * The core sets oos before the off call of ops->line_service, and it clears
 * oos before the on call. A drain inside either call then reads the service
 * state that the call establishes.
 */
enum proto_line_flag {
	PROTO_LF_OFFHOOK = 1,
	PROTO_LF_CALL = 2,
	PROTO_LF_SESSION = 4,
};

/*
 * What host->tone_play, host->ring_start or host->cw_alert put on the line,
 * when the result arrives after the host call returned (ops->signal_result).
 */
enum proto_signal {
	PROTO_SIG_TONE = 0,
	PROTO_SIG_RING,
	PROTO_SIG_ALERT,
};

/*
 * Why the core takes a line out of service (ops->line_service).
 *
 *   PROTO_SERVICE_CFG    the configuration, the loop test, no running
 *                        protocol, the protocol's own hold and the daemon
 *                        stop. The protocol tears every leg down, then
 *                        forces its machine out.
 *   PROTO_SERVICE_FAULT  a SLIC fault, a line the backend declares dead, or
 *                        a detector arm the DSP refused. The protocol ends
 *                        the legs through its machine alone.
 *
 * A move into service carries PROTO_SERVICE_CFG.
 */
enum proto_service_reason {
	PROTO_SERVICE_CFG = 0,
	PROTO_SERVICE_FAULT,
};

/*
 * One HAL event of a line, after the core intake (ops->line_event). The core
 * does the hook and digit intake, the activity count, the drops of the battery
 * removal window, the loop test consumption, and the detector arm and the
 * out of service force at the seize. Then it forwards the event.
 *
 *   HOOK        a seize, a release or a flash; hook.state is enum hal_hook.
 *               The core turns a pulse train into one DIGIT event.
 *   DIGIT       one digit from the phone. measured says a detector measured
 *               duration_ms. An injected digit and a pulse digit carry
 *               measured false and duration_ms 0. pulse says the digit came
 *               from a pulse train. A digit the HAL reports from the wire
 *               stays in the core.
 *   TONE        a detected tone that is not DTMF (hal.h HAL_EVENT_TONE), on
 *               the start row and on the end row.
 *   FIRST_RTP   the first packet arrived on stream (HAL_EVENT_RTP_FIRST_RX).
 *   RING_END    the backend ended a ring (HAL_EVENT_RING_END).
 *   LINE_STATE  the backend reports a line state (HAL_EVENT_LINE);
 *               line.state is enum hal_line_state.
 *
 * stream is the HAL stream the record names, and 0 where the record names
 * none. The field widths are those of struct hal_event.
 */
enum proto_line_ev {
	PROTO_EV_HOOK = 0,
	PROTO_EV_DIGIT,
	PROTO_EV_TONE,
	PROTO_EV_FIRST_RTP,
	PROTO_EV_RING_END,
	PROTO_EV_LINE_STATE,
};

struct proto_line_event {
	enum proto_line_ev type;
	unsigned int stream;
	union {
		struct {
			uint8_t state;		/* enum hal_hook */
		} hook;
		struct {
			char digit;
			uint16_t duration_ms;
			bool measured;
			bool pulse;
		} digit;
		struct {
			uint16_t code;		/* enum hal_tone_detect */
			uint8_t direction;	/* enum hal_tone_dir */
			uint8_t end;		/* 0 on a start row, 1 on an
						 * end row */
			uint16_t duration_ms;
		} tone;
		struct {
			uint8_t state;		/* enum hal_line_state */
		} line;
	} u;
};

/*
 * The voice interface as the netifd tracker of the core holds it
 * (net/iface.h). The texts are empty while netifd named nothing.
 *
 * addr6 is the global IPv6 address of the voice netdev. The interface that
 * reports it is not always the voice interface: the IPv6 side of a WAN is
 * often an interface of its own on the same netdev, and addr6_iface names
 * the one that holds the record.
 *
 * down says an ifdown of the voice interface arrived and no answer since
 * carried an address, so the next answer rebinds even on an unchanged
 * address.
 *
 * ipv6_served is the gate a protocol sets (host->net_iface_ipv6_served_set):
 * the operator switch ipv6_enabled and a backend that carries
 * HAL_FEAT_IPV6_MEDIA. A line signals in IPv6 only while it is set and addr6
 * is known.
 */
#define NET_IFACE_NAME_MAX	16
#define NET_IFACE_ADDR_MAX	64

struct net_iface_state {
	char iface[NET_IFACE_NAME_MAX];		/* the logical name */
	char l3_device[NET_IFACE_NAME_MAX];	/* its netdev */
	char addr4[NET_IFACE_ADDR_MAX];
	char addr6[NET_IFACE_ADDR_MAX];
	char addr6_iface[NET_IFACE_NAME_MAX];
	bool addr4_known;
	bool addr6_known;
	bool down;
	bool ipv6_served;
};

/*
 * What one tracker event moved, handed beside the new state
 * (struct net_iface_sub changed). It carries the old addresses and the old
 * served gate, and the state carries the new ones, because a protocol
 * compares its family preference before and after the event.
 *
 *   addr4   the IPv4 half asks for a rebind: the address moved, it came back
 *           after an ifdown, or the netdev moved (net_iface_rebind_needed).
 *   addr6   the global IPv6 address of the netdev moved: it arrived, it
 *           changed or it went. A netdev move drops the address of the old
 *           netdev, and that drop counts as a move.
 *   dev     the netdev of the voice interface moved; l3_device of the state
 *           names the new one.
 *   force   the IPv4 address came back unchanged after an ifdown, so a line
 *           that holds it still registers through a fresh socket.
 *   served  the IPv6 served gate moved (host->net_iface_ipv6_served_set);
 *           ipv6_served_old holds the gate before the move.
 */
struct net_iface_change {
	bool addr4;
	bool addr6;
	bool dev;
	bool force;
	bool served;
	char addr4_old[NET_IFACE_ADDR_MAX];
	char addr6_old[NET_IFACE_ADDR_MAX];
	bool ipv6_served_old;
};

/*
 * The one subscriber of the tracker, which is the running protocol.
 *
 * changed runs once per tracker event that moved anything, after the record
 * write. recover runs from the recovery attempt of the tracker: the protocol
 * reopens what it holds down and starts the attempts that this freed.
 * down_count answers how many sockets and listeners of the protocol are still
 * down; the tracker arms its next attempt while it is not 0.
 */
struct net_iface_sub {
	void *priv;
	void (*changed)(void *priv, const struct net_iface_state *st,
			const struct net_iface_change *ch);
	void (*recover)(void *priv);
	unsigned int (*down_count)(void *priv);
};

/*
 * The firewall ask of a protocol (host->fw_ask). The core computes the RTP
 * ranges from the config it took (fw_reconcile_cfg_take); the protocol names
 * what only it knows.
 *
 * active_mask has bit i set for each line i the protocol runs. port[i] is the
 * signaling port of line i. proto_udp is true for a datagram transport.
 * proxy holds nproxy proxy source addresses in the sorted order
 * host->fw_proxy_add keeps; restrict_source 1 admits signaling from these
 * alone.
 *
 * The two bounds equal FW_PROXY_MAX and FW_SRC_MAX of fw.h, which
 * net/fw_reconcile.h asserts.
 */
#define PROTO_FW_PROXY_MAX	16
#define PROTO_FW_SRC_MAX	48

struct fw_sig_ask {
	uint32_t active_mask;
	uint16_t port[CFG_CHANNEL_MAX];
	bool proto_udp;
	char proxy[PROTO_FW_PROXY_MAX][PROTO_FW_SRC_MAX];
	unsigned int nproxy;
};

/* Why host->tls_cert_stop runs: a config that no longer asks for the
 * bundle clears it, and a shutdown only reaps the fetch. */
enum proto_tls_stop {
	PROTO_TLS_STOP_CONFIG = 0,
	PROTO_TLS_STOP_SHUTDOWN,
};

/* The answer of host->stats_live_query: the numbers, or NULL when the query
 * failed. stats is valid for the call only. */
typedef void (*proto_stats_cb)(void *priv, const struct hal_media_stats *stats);

/* ------------------------------------------------------- the host table */

/*
 * The services the core offers a protocol. Every entry takes ctx first. line
 * is the 0 based line index and stream the HAL stream index.
 *
 * proto_host_build() fills the table once, after the HAL attached and before
 * ops->init, and proto_host_get() answers it. The priv of media is fixed at
 * that build.
 */
struct proto_host {
	void *ctx;

	/*
	 * Tones. The protocol computes the configured tone lengths itself,
	 * from busy_tone_s and roh_s.
	 */

	/* Play a tone toward the phone; duration_ms 0 plays it until stopped.
	 * The core keeps the dial tone deferral, and a deferred tone reports
	 * through ops->signal_result. */
	enum hal_status (*tone_play)(void *ctx, unsigned int line,
				     enum hal_tone tone, uint32_t duration_ms);
	/* Stop the tone of the line. */
	void (*tone_stop)(void *ctx, unsigned int line);

	/*
	 * Ring and identity.
	 */

	/*
	 * Ring the line with the cadence of ring_template and the type 1
	 * caller id. duration_ms 0 rings until host->ring_stop. A ring that
	 * ends on its duration reaches the protocol as PROTO_EV_RING_END. The
	 * results arrive as ops->ring_held, ring_started or ring_failed. The
	 * gate rule of struct proto_cid applies.
	 */
	void (*ring_start)(void *ctx, unsigned int line,
			   enum proto_ring_kind kind,
			   unsigned int ring_template, uint32_t duration_ms,
			   const struct proto_cid *cid);
	/* Stop the ring and drop its hold and its retry. */
	void (*ring_stop)(void *ctx, unsigned int line);
	/* Start or stop the call waiting alert: variant 0 is the plain tone,
	 * 1 to 4 a region variant, and the type 2 record follows the alert.
	 * The gate rule of struct proto_cid applies. */
	void (*cw_alert)(void *ctx, unsigned int line, bool on,
			 unsigned int variant, const struct proto_cid *cid);
	/* Push a caller id record alone, for a protocol that sequences its
	 * own alert. The core fills the record and calls the HAL
	 * line_cid_send. */
	enum hal_status (*cid_send)(void *ctx, unsigned int line,
				    enum hal_cid_kind kind,
				    const struct proto_cid *cid);
	/* The ring template the distinctive ring options assign to number, 0
	 * for none. */
	unsigned int (*ring_template_match)(void *ctx, unsigned int line,
					    const char *number);

	/*
	 * Message waiting.
	 */

	/*
	 * Store what the switch said, and present the visual record under the
	 * gate rule of struct proto_cid. The core tells no machine: the
	 * protocol applies the audible choice itself.
	 */
	void (*mwi_set)(void *ctx, unsigned int line, bool on,
			uint32_t new_msgs, uint32_t old_msgs);
	/* The stored indication and its two counts, the input of the audible
	 * choice. */
	bool (*mwi_get)(void *ctx, unsigned int line, uint32_t *new_msgs,
			uint32_t *old_msgs);

	/*
	 * Detector and battery.
	 */

	/* Push the channel view with detect_mask (enum hal_detect bits). False
	 * when the DSP refused it. */
	bool (*detector_arm)(void *ctx, unsigned int line,
			     uint32_t detect_mask);
	/*
	 * Start the configured forward disconnect signal for its configured
	 * length. True when it started. False when the bell, a caller id
	 * record or an open battery removal holds the line, or when the
	 * backend refused the signal.
	 */
	bool (*fwd_disc_signal)(void *ctx, unsigned int line);
	/* End a forward disconnect signal and put the battery back. */
	void (*fwd_disc_abort)(void *ctx, unsigned int line);
	/*
	 * Set the loop feed through the forward disconnect primitives of the
	 * core. DOWN removes the feed, drops the hook reports of the line while
	 * the feed is down, and runs the settle window when the feed returns.
	 * REVERSE reverses the polarity under HAL_FEAT_POLARITY. The core
	 * restores the polarity at ACTIVE, at ops->line_reset and at the
	 * daemon stop. The wanted feed survives a service sync.
	 */
	enum hal_status (*battery_set)(void *ctx, unsigned int line,
				       enum proto_battery batt);

	/*
	 * Digits toward the phone.
	 */

	/*
	 * Queue one digit for the phone, under the queue bound and the pacing;
	 * it asks ops->digit_play_allowed. dedup true is a digit the far end
	 * reported, which a peer can send on two transports at once: a repeat
	 * of the last reported digit inside the de-duplication window is
	 * dropped. dedup false is a digit the protocol was commanded to play
	 * (an MGCP Brief signal): every request queues. True when queued.
	 */
	bool (*digit_play)(void *ctx, unsigned int line, unsigned int stream,
			   char digit, uint16_t duration_ms, bool dedup);
	/* Drop the queued digits of stream that have not started. The digit
	 * that plays runs to its end, as RFC 3435 section 2.1.7 asks of a
	 * Brief signal. */
	void (*digit_play_cancel)(void *ctx, unsigned int line,
				  unsigned int stream);

	/*
	 * Media.
	 */

	/* The stream ops over the HAL backend (proto_media.h). Its priv is
	 * fixed when the core builds the table. */
	struct call_media_ops media;
	/* The media counters of one stream from cached engine state, through
	 * the HAL stream_stats_get; HAL_UNSUPPORTED where the backend has
	 * none. */
	enum hal_status (*stream_stats_get)(void *ctx, unsigned int line,
					    unsigned int stream,
					    struct hal_media_stats *out);
	/* Start the slow media query of a running stream; cb answers it once.
	 * HAL_OK runs it; UNSUPPORTED: no stream, TRANSIENT: busy, FATAL:
	 * refused. */
	enum hal_status (*stats_live_query)(void *ctx, unsigned int line,
					    unsigned int stream,
					    proto_stats_cb cb, void *priv);
	/* Abandon the live query of stream; cb is not called. */
	void (*stats_live_drop)(void *ctx, unsigned int line,
				unsigned int stream);

	/*
	 * Service state.
	 */

	/* Whether the board carries the line. */
	bool (*line_exists)(void *ctx, unsigned int line);
	/* Whether the core holds the line in service. */
	bool (*line_in_service)(void *ctx, unsigned int line);
	/* Whether the DSP is ready. */
	bool (*hal_ready)(void *ctx);
	/*
	 * Hold the line out of service for reason, a hold of the protocol's
	 * own. It is a sixth reason beside config, fault, down, loop test and
	 * no running protocol. The hold applies one uloop pass later.
	 */
	void (*line_oos_force)(void *ctx, unsigned int line,
			       const char *reason);
	/* End the hold of the protocol, one uloop pass later. The line returns
	 * to service only when none of the other five reasons holds it. */
	void (*line_oos_clear)(void *ctx, unsigned int line);
	/*
	 * The protocol calls it after every event that it ran through its
	 * machine. state_id is the protocol's own number for the new state,
	 * and flags its enum proto_line_flag bits. The core stamps a pending
	 * tone replay with state_id, and drops the replay when a later call
	 * names another state.
	 *
	 * When flags is 0, the out-of-service rest included, the core also
	 * drops what it holds for the call that ended:
	 *   - the type 2 record and the flag of its suppression trace;
	 *   - the queued digits toward the phone;
	 *   - the canceller view of the line.
	 * The protocol drops its own records of that call (a one-call caller id
	 * override, a network call waiting record) on the same edge.
	 */
	void (*line_transition)(void *ctx, unsigned int line,
				unsigned int state_id, uint32_t flags);
	/*
	 * The protocol calls it once at the end of a drain, after its own
	 * config apply (which calls host->line_cfg_landed) and after its own
	 * forward disconnect settle. The core then presents a visual message
	 * waiting record that it held back.
	 */
	void (*line_settle)(void *ctx, unsigned int line);
	/*
	 * The protocol applied its staged config at a rest state. The core then
	 * swaps in its staged channel copy, caller id formats and ring
	 * patterns, and pushes the hook windows. The core stages its copy
	 * before it calls ops->line_cfg_changed or ops->region_changed. When
	 * the entry it calls is NULL, the core applies its staged copy at once.
	 */
	void (*line_cfg_landed)(void *ctx, unsigned int line);
	/* The probed enum hal_feature bits (struct hal_cap features). */
	uint32_t (*hal_features)(void *ctx);
	/* The probed codec bits, 1 << enum hal_codec (struct hal_cap
	 * codecs). */
	uint32_t (*hal_codecs)(void *ctx);
	/* The probed capability record, never NULL: the answer of the HAL
	 * cap_get. */
	const struct hal_cap *(*hal_cap)(void *ctx);
	/* The region the backend holds, or the compiled default. */
	const struct hal_region *(*region)(void *ctx);
	/* Turn the local three-way mixer of the line on or off. */
	enum hal_status (*conf_set)(void *ctx, unsigned int line, bool on);
	/* Report a fault of the line's configuration or hardware; loud logs
	 * it, otherwise it is traced. */
	void (*line_fault)(void *ctx, unsigned int line, bool loud,
			   const char *text);

	/*
	 * Trace and log. PROTO_TRACE of proto_trace.h wraps the first two.
	 */

	/* Whether dom is active in the trace masks of the core. */
	bool (*trace_active)(void *ctx, enum trace_domain dom);
	/* Write one trace line in dom. */
	void (*trace_write)(void *ctx, enum trace_domain dom, const char *fmt,
			    ...) __attribute__((format(printf, 3, 4)));
	/* Write one line to syslog at a syslog level (LOG_ERR to
	 * LOG_DEBUG). */
	void (*log)(void *ctx, int level, const char *fmt, ...)
		__attribute__((format(printf, 3, 4)));

	/*
	 * Config.
	 */

	/* The running config; immutable, and valid until the next reload. */
	const struct cfg *(*cfg)(void *ctx);
	/* The count of applied reloads. */
	uint32_t (*config_generation)(void *ctx);

	/*
	 * ubus.
	 */

	/* The daemon's ubus connection, or NULL without one. */
	struct ubus_context *(*ubus_ctx)(void *ctx);
	/* Answer a protocol verb with the line and the verb under the key
	 * "requested". */
	void (*ubus_request_reply)(void *ctx, struct ubus_context *ubus,
				   struct ubus_request_data *req,
				   unsigned int line, const char *verb);

	/*
	 * Events.
	 */

	/* A registration state edge of one line: the cloud event and, on a
	 * move, the call trace event. */
	void (*reg_state_post)(void *ctx, unsigned int line, bool up,
			       const char *proxy, bool moved);
	/* Fetch the TLS bundle; done runs when a new bundle arrived. */
	void (*tls_cert_start)(void *ctx, const char *url, const char *username,
			       const char *password, bool verify,
			       void (*done)(void));
	/* Stop the fetch of the TLS bundle. */
	void (*tls_cert_stop)(void *ctx, enum proto_tls_stop mode);

	/*
	 * Network. net/iface.h, net/ports.h, net/conntrack.h and
	 * net/fw_reconcile.h hold the contracts.
	 */

	/* Install the one tracker subscriber. net_iface_subscribe(). */
	void (*net_iface_subscribe)(void *ctx, const struct net_iface_sub *sub);
	/* The tracker record. net_iface_state_get(). */
	const struct net_iface_state *(*net_iface_state_get)(void *ctx);
	/* The netdev a socket of iface binds to, or NULL for an unscoped bind.
	 * net_iface_bind_device(). */
	const char *(*net_iface_bind_device)(void *ctx, const char *iface);
	/* The source address the kernel picks toward sa, scoped to dev, as
	 * text of family. net_iface_source_host_probe(). */
	bool (*net_iface_source_host_probe)(void *ctx, const char *dev,
					    int family,
					    const struct sockaddr *sa,
					    socklen_t salen, char *out,
					    size_t olen);
	/* Set the IPv6 served gate. net_iface_ipv6_served_set(). */
	void (*net_iface_ipv6_served_set)(void *ctx, bool served);
	/* Ask the tracker to decide its recovery now.
	 * net_iface_recover_settle(). */
	void (*net_iface_recover_settle)(void *ctx);
	/* The effective RTP range of a channel. net_port_range(). */
	void (*net_port_range)(void *ctx, const struct cfg_channel *ch,
			       uint16_t *min, uint16_t *max);
	/* Whether two channels name one range. net_port_range_eq(). */
	bool (*net_port_range_eq)(void *ctx, const struct cfg_channel *a,
				  const struct cfg_channel *b);
	/* Take an RTP port. net_port_alloc(). */
	bool (*net_port_alloc)(void *ctx, unsigned int min, unsigned int max,
			       uint16_t *out);
	/* Give an RTP port back. net_port_free(). */
	void (*net_port_free)(void *ctx, uint16_t port);
	/* The conntrack udp bound in seconds. net_ct_udp_bound_s(). */
	unsigned int (*net_ct_udp_bound_s)(void *ctx);
	/* The media punch cadence. net_ct_punch_interval_ms(). */
	uint16_t (*net_ct_punch_interval_ms)(void *ctx);
	/* Reconcile the firewall with ask. fw_reconcile_ask(). */
	void (*fw_ask)(void *ctx, const struct fw_sig_ask *ask);
	/* Remove every VoIP rule. fw_reconcile_clear(). */
	void (*fw_clear)(void *ctx);
	/* Add one proxy source to a sorted list; answers the new count.
	 * fw_reconcile_proxy_add(). */
	unsigned int (*fw_proxy_add)(void *ctx, char (*list)[PROTO_FW_SRC_MAX],
				     unsigned int count, const char *addr);

	/*
	 * Records and trace. A record is keyed by the stream of its leg.
	 */

	/* Open the record of a leg; answers its handle. */
	uint32_t (*cdr_call_started)(void *ctx, unsigned int line,
				     unsigned int stream, bool inbound);
	/* The sequence of the open record, 0 for none. */
	uint32_t (*cdr_call_seq)(void *ctx, unsigned int line,
				 unsigned int stream);
	/* Write the sequence of the record open on from_stream into the record
	 * open on stream. */
	void (*cdr_call_correlate)(void *ctx, unsigned int line,
				   unsigned int stream,
				   unsigned int from_stream);
	/* Mark the record answered. */
	void (*cdr_call_answered)(void *ctx, unsigned int line,
				  unsigned int stream);
	/* Mark the record conferenced. */
	void (*cdr_call_conferenced)(void *ctx, unsigned int line,
				     unsigned int stream);
	/* Close the record with what the leg was. */
	void (*cdr_call_ended)(void *ctx, unsigned int line,
			       unsigned int stream,
			       const struct proto_call_ended *info);
	/* A call started on the traced line. */
	void (*calltrace_call_started)(void *ctx, unsigned int line,
				       uint32_t cdr_seq);
	/* Ask the call trace for a snapshot with reason. */
	void (*calltrace_event)(void *ctx, unsigned int line,
				const char *reason);
	/*
	 * The machine of the line moved. The protocol calls it on every move
	 * of the line's own state, with before and after as the names its
	 * status dump writes. At this call the core reads ops->line_flags and
	 * its oos bit to tell idle from out of service.
	 */
	void (*calltrace_state)(void *ctx, unsigned int line,
				const char *before, const char *after);
	/* Fill one tap endpoint from a socket address. */
	void (*calltrace_tap_endpoint_set)(void *ctx,
					   struct trace_endpoint *out,
					   const struct sockaddr *sa);

	/*
	 * Lifecycle.
	 */

	/* The graceful stop of the protocol ended. proto_stop_wait() ends its
	 * wait on this call or on its bound. A protocol never calls
	 * uloop_end(). */
	void (*shutdown_done)(void *ctx);

	/*
	 * Entropy.
	 */

	/* Fill len bytes of buf from the entropy source of the OS; false when
	 * no source answered. media.rand_bytes reads the same source. */
	bool (*random_bytes)(void *ctx, void *buf, size_t len);
};

/* -------------------------------------------------------- the ops table */

/*
 * What a protocol exports. line is the 0 based line index and stream the HAL
 * stream index.
 *
 * Index bounds. A stream index is below hal_cap streams_per_chan and below
 * HAL_STREAM_MAX, which bounds every core array keyed by stream. A UI slot is
 * the protocol's own number and opaque to the core. The core validates a slot
 * that a ubus verb names through ops->stream_of_slot alone.
 *
 * Mandatory: name, init, start, reload, done, line_event, line_service,
 * line_reset, line_flags, status_dump_call, status_dump_lines, shutdown_begin
 * and shutdown_pending. proto_ops_complete() checks them. Every other entry
 * can be NULL.
 */
struct proto_ops {
	const char *name;

	/*
	 * Lifecycle.
	 */

	/* Take the host table and the config, after the core wired its lines
	 * and before ubus. */
	void (*init)(const struct proto_host *host, const struct cfg *cfg);
	/* Start signaling, after ubus. */
	void (*start)(const struct cfg *cfg);
	/*
	 * The DSP capability, with host->hal_cap()->t38_max_datagram beside
	 * it. The core calls it twice, each time before the service sync.
	 * lines_init() passes the provisional capability of the attach, and
	 * lines_start() the final one. The protocol re-derives on each call.
	 * The core alone reports the conference capability fault.
	 */
	void (*dsp_ready)(uint32_t features, uint32_t codecs);
	/* Apply a new config. The protocol tears down the legs of a line whose
	 * RTP range moved and asks the firewall again. */
	void (*reload)(const struct cfg *old_cfg, const struct cfg *new_cfg);
	/* Begin the graceful stop. */
	void (*shutdown_begin)(void);
	/* Whether the graceful stop still waits. proto_stop_wait() asks it
	 * once after shutdown_begin, and waits for host->shutdown_done only
	 * when it answers true. */
	bool (*shutdown_pending)(void);
	/* Free everything, once, after the HAL closed. */
	void (*done)(void);

	/*
	 * Lines.
	 */

	/* One HAL event of the line after the core intake. */
	void (*line_event)(unsigned int line,
			   const struct proto_line_event *ev);
	/*
	 * Off: end every leg and force the machine out, the way reason asks
	 * (enum proto_service_reason), with no HAL call in between. On: clear
	 * it; the core passes PROTO_SERVICE_CFG and the protocol reads no
	 * reason. The core sets its oos bit before the off call and clears it
	 * before the on call. A drain inside either call then reads the service
	 * state that the call establishes. The off form can arrive between
	 * ops->init and ops->start, from the provisional service state of
	 * lines_init().
	 */
	void (*line_service)(unsigned int line, bool in_service,
			     enum proto_service_reason reason);
	/* Cancel every protocol timer linked from the line, before the core
	 * initializes it again. */
	void (*line_reset)(unsigned int line);
	/*
	 * The core staged a new channel view for the line, on a reload. The
	 * protocol stages its own derived view, applies it at its next rest
	 * state and calls host->line_cfg_landed then. The core stages before
	 * this call. When the entry is NULL, the core applies its staged copy
	 * at once.
	 */
	void (*line_cfg_changed)(unsigned int line);
	/*
	 * The region read-back moved. The protocol re-derives and stages what
	 * it takes from the region, and calls host->line_cfg_landed when it
	 * applies that at a rest state. The core calls it from the region job
	 * completion, never from host->line_settle. The core stages before
	 * this call. When the entry is NULL, the core applies its staged copy
	 * at once.
	 */
	void (*region_changed)(unsigned int line,
			       const struct hal_region *region);

	/*
	 * Ring and signal results.
	 */

	/* A line test holds the ring. */
	void (*ring_held)(unsigned int line);
	/* A held ring sounds now. */
	void (*ring_started)(unsigned int line);
	/* The backend will not ring the line for this call. It never arrives
	 * for a PROTO_RING_RECALL ring. */
	void (*ring_failed)(unsigned int line);
	/* A tone, ring or alert the core applied or refused after the host
	 * call returned. MGCP maps it to the oc and of events. */
	void (*signal_result)(unsigned int line, enum proto_signal sig,
			      bool ok);

	/*
	 * Line queries.
	 */

	/* The enum proto_line_flag bits of the line, from the state of its
	 * machine. */
	uint32_t (*line_flags)(unsigned int line);
	/* The state of the machine by name, for the call trace snapshot key
	 * fsm_state. */
	const char *(*state_name)(unsigned int line);
	/* Whether a far-end digit on stream may play now. */
	bool (*digit_play_allowed)(unsigned int line, unsigned int stream);
	/* The stream a UI slot runs on, or HAL_STREAM_MAX when no leg holds
	 * the slot. */
	unsigned int (*stream_of_slot)(unsigned int line, unsigned int slot);
	/* The strongest passthrough kind (enum hal_passthrough) of the running
	 * legs of the line; *media_running says a leg runs media. */
	uint8_t (*line_fax_kind)(unsigned int line, bool *media_running);

	/*
	 * Northbound.
	 */

	/* The protocol's own ubus methods and their count. */
	const struct ubus_method *(*ubus_methods)(unsigned int *n);
	/* The protocol keys inside the open call[] entry of the line. */
	void (*status_dump_call)(struct blob_buf *b, unsigned int line);
	/* The lines[] array: every line for a negative want_line, else that
	 * line alone. */
	void (*status_dump_lines)(struct blob_buf *b, int want_line);
	/* The protocol keys of the media table. When the entry is NULL, the
	 * core writes opus_dtmf_fallbacks 0, because the API schema requires
	 * the key. */
	void (*status_dump_media)(struct blob_buf *b);
	/* The protocol's own top-level tables of "voice status". */
	void (*status_dump_top)(struct blob_buf *b);
	/* The signaling ports of the line for the capture filter of the call
	 * trace, at most max; answers the count. */
	unsigned int (*wire_ports)(unsigned int line, uint16_t *ports,
				   unsigned int max);
	/* Install the call trace tap (proto_tap.h proto_tap_fn), or NULL to
	 * remove it. */
	void (*tap_set)(void (*fn)(bool tx,
				   const struct proto_tap_endpoints *ep,
				   const void *buf, size_t len));
	/* The protocol's transport counters as a table under its own key. */
	void (*wire_stats)(struct blob_buf *b);
};

/* ---------------------------------------------- the registry, proto/proto.c */

/*
 * The ops tables the build carries. proto/proto.c lists them under the build
 * flags and holds a null table of its own, which stands while no protocol runs.
 */
#ifdef UVOICE_PROTO_SIP
extern const struct proto_ops sip_proto_ops;
#endif
#ifdef UVOICE_PROTO_MGCP
extern const struct proto_ops mgcp_proto_ops;
#endif

/* Whether every mandatory entry of ops is set. */
bool proto_ops_complete(const struct proto_ops *ops);

/* The table of that name, or NULL when the image does not carry it. */
const struct proto_ops *proto_select(const char *name);

/*
 * Select the protocol cfg->protocol names. The answer is the table the core
 * runs: the selected one, or the null table when the image does not carry
 * that protocol. proto/proto.c records running or unavailable either way.
 */
const struct proto_ops *proto_select_from_cfg(const struct cfg *cfg);

/* Run ops->start on the selected protocol. */
void proto_start(const struct cfg *cfg);

/* Run ops->reload once, before lines_reload(). */
void proto_reload(const struct cfg *old_cfg, const struct cfg *new_cfg);

/*
 * Run ops->shutdown_begin. When ops->shutdown_pending then answers true, pump
 * uloop until host->shutdown_done arrives or the bound passes.
 */
void proto_stop_wait(void);

/* Run ops->done once, after hal_close(), and clear the firewall. */
void proto_done(void);

/* The signaling table of "voice status": the protocol, its state, the error
 * when it is unavailable and the protocols the image carries. */
void proto_status_dump(struct blob_buf *b);

/* The selected protocol's ubus methods, or NULL and 0 for none. */
const struct ubus_method *proto_ubus_methods(unsigned int *n);

/* Whether a protocol runs. */
bool proto_running(void);

/* The table in force: the selected protocol, or the null table "none" while
 * no protocol runs. Never NULL. */
const struct proto_ops *proto_ops_get(void);

/* Fill the host table once, after the HAL attached and before ops->init.
 * The priv of host->media is fixed then. */
void proto_host_build(void);

/* The host table, for the line shell and the suites. */
const struct proto_host *proto_host_get(void);

#endif
