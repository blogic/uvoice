// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The MGCP endpoints of a residential gateway (RFC 3435 sections 2 and 4.4,
 * NCS 1.5 sections 7 and 8): one endpoint per line, and the gateway that
 * holds them.
 *
 * An endpoint holds its service state, its notified entity, the request in
 * force (X, R with actions, S, D, Q, T), the digit collection, the
 * observed events, the quarantine buffer and two connections with their SDP
 * state and their HAL stream. It answers EPCF, RQNT, CRCX, MDCX, DLCX, AUEP
 * and AUCX, and it sends NTFY, RSIP and DLCX.
 *
 * The caller. The protocol unit (mgcp/proto.c) or a suite owns the
 * transaction layer (mgcp/txn.h) with its environment, and the socket behind
 * its send. It calls the entry points below from the upcalls of that
 * environment and from the ops of struct proto_ops. The endpoint sends every
 * message through the transaction layer: mgcp_txn_send() toward the
 * notified entity, which the caller resolves through struct mgcp_ep_env dst,
 * and mgcp_txn_respond() toward the source of a command. The timers of this
 * file run on the timer environment of the transaction layer.
 *
 * The core. The endpoint reaches the core through the host table alone
 * (proto/proto.h struct proto_host): tones, ring, call waiting, caller id,
 * message waiting, battery, digits toward the phone, the detector mask, the
 * stream ops and the RTP ports.
 *
 * Lifetime. mgcp_ep_init() takes a caller-owned struct mgcp_gw and allocates
 * nothing. Every entry runs on the uloop thread.
 */
#ifndef UVOICE_MGCP_EP_H
#define UVOICE_MGCP_EP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "cfg/cfg.h"
#include "dialplan.h"
#include "hal/hal.h"
#include "proto/mgcp/msg.h"
#include "proto/mgcp/sig.h"
#include "proto/mgcp/txn.h"
#include "proto/proto.h"
#include "proto/shared/sdp.h"

#define MGCP_EP_MAX		CFG_CHANNEL_MAX
/* call waiting and a three-way call need two connections */
#define MGCP_CONN_MAX		2
/* RFC 3435 4.4.1: the quarantine buffer; the excess is discarded */
#define MGCP_QUAR_MAX		16
#define MGCP_OBS_MAX		32
#define MGCP_REQ_MAX		16
#define MGCP_SIG_RUN_MAX	8
/* 1*32 HEXDIG and its terminator (3.2.2.2, 3.2.2.5, 3.2.2.18) */
#define MGCP_ID_MAX		33
#define MGCP_ENTITY_MAX		128
#define MGCP_EP_NAME_MAX	160
#define MGCP_TEXT_MAX		512
#define MGCP_EMBED_MAX		256
#define MGCP_SDP_MAX		2048
#define MGCP_ARENA_MAX		2048
/* the D/ digits of one S:, the queue toward the phone */
#define MGCP_BR_MAX		32
/* a body holds a local and a remote description, each up to MGCP_SDP_MAX */
#define MGCP_BODY_MAX		(2 * MGCP_SDP_MAX + 4)

/* The transaction owner of the commands that name every endpoint. An
 * endpoint's own commands carry its index plus one. */
#define MGCP_EP_OWNER_GW	0x100u

/* The E: codes of RFC 3435 2.5 that a gateway DLCX and a forced RSIP carry. */
#define MGCP_EP_REASON_FAULT	900
#define MGCP_EP_REASON_OOS	901

enum mgcp_ep_svc {
	MGCP_EP_RESTARTING = 0,		/* until a success response to RSIP */
	MGCP_EP_IN_SERVICE,
	MGCP_EP_OUT_OF_SERVICE,
};

/*
 * What the caller supplies. dst answers the address of a notified entity,
 * the N: form of RFC 3435 3.2.1.3 with an optional port; false when the
 * entity has no known address yet.
 */
struct mgcp_ep_env {
	void *priv;
	bool (*dst)(void *priv, const char *entity,
		    struct sockaddr_storage *out, socklen_t *len);
};

/*
 * The view of the gateway the caller derives from voip.mgcp and from the
 * network tracker.
 *
 * pattern and line_base name the endpoints: {line} is line_base plus the
 * line index, {host} is host, the host part with the brackets of an address
 * when the config asks for them. entity is the notified entity at start.
 * persistent is the persistent_events list, empty for none. quarantine_loop
 * selects loop for a request without Q:. media_addr is the address of every
 * LocalConnectionDescriptor and bind_dev the device the streams bind to.
 * ptime_omit leaves a=ptime out of every descriptor (send_ptime 0).
 */
struct mgcp_ep_cfg {
	enum cfg_mgcp_dialect dialect;
	unsigned int lines;
	char pattern[CFG_STR_MAX];
	unsigned int line_base;
	char host[CFG_MGCP_DOMAIN_MAX + 2];
	char entity[MGCP_ENTITY_MAX];
	char persistent[CFG_STR_MAX];
	bool quarantine_loop;
	bool ptime_omit;
	uint32_t restart_delay_max_ms;
	char media_addr[SDP_ADDR_MAX];
	char bind_dev[HAL_IFNAME_MAX];
};

/* One requested event with its actions; embedded is the E(...) body. */
struct mgcp_req_ent {
	struct mgcp_ev_pat pat;
	uint32_t act;			/* MGCP_ACT_* */
	char embedded[MGCP_EMBED_MAX];
};

/* One signal that runs: a TO signal with its time-out, or an OO signal. */
struct mgcp_sig_run {
	const struct mgcp_sig_def *def;
	struct proto_timer timer;
	char name[MGCP_SIG_NAME_MAX];	/* "L/dl", as an oc or of names it */
	char params[64];		/* the parameters, for an audit */
	bool used;
};

/*
 * The LocalConnectionOptions a connection keeps (RFC 3435 2.3.6 page 53):
 * an L: names the options it changes and the others stay. sup holds the
 * supplied text, one "name:value" item per option name with the last value
 * supplied, in the order the names first arrived; an AUCX writes it as its
 * L: (2.3.11 page 66). The fields after it derive from sup: a is the a:
 * list, ptime the packet time p: resolves to (0 for none), vad the s: value
 * and ec the e: value (-1 for none), tos the t: byte (-1 for none).
 */
struct mgcp_lopt {
	char sup[MGCP_TEXT_MAX];
	char a[MGCP_TEXT_MAX];
	uint8_t ptime;
	int8_t vad;
	int8_t ec;
	int16_t tos;
};

/* One connection (RFC 3435 2.3.5). The stream is the index of the slot. */
struct mgcp_conn {
	bool used;
	char id[MGCP_ID_MAX];
	char call_id[MGCP_ID_MAX];
	uint8_t mode;			/* enum mgcp_mode */
	unsigned int stream;
	uint16_t port;
	struct sdp_local loc;
	struct sdp_state sdp;
	bool remote;			/* a remote description was taken */
	bool media_set;
	bool media_started;
	bool fax_applied;
	uint8_t fax_kind;		/* enum hal_passthrough */
	struct hal_media media;
	struct mgcp_lopt lopt;
	char lcd[MGCP_SDP_MAX];
	size_t lcd_len;
	char rcd[MGCP_SDP_MAX];
	size_t rcd_len;
};

/*
 * The call record of one call id (C:) on the endpoint, for the core's call
 * records (struct proto_host cdr_call_*). The CRCX that makes the first
 * connection of the call opens it, keyed by that connection's stream; the
 * delete of the last connection of the call closes it. inbound says the
 * line was on hook at that CRCX, which is how a call agent starts an
 * incoming call.
 */
struct mgcp_rec {
	bool used;
	bool inbound;
	bool answered;
	bool media_ran;
	unsigned int stream;
	uint64_t answered_ms;
	char call_id[MGCP_ID_MAX];
};

struct mgcp_gw;

struct mgcp_ep {
	struct mgcp_gw *gw;
	unsigned int index;
	char name[MGCP_EP_NAME_MAX];
	char entity[MGCP_ENTITY_MAX];	/* the notified entity */
	uint8_t svc;			/* enum mgcp_ep_svc */
	uint8_t rm;			/* enum mgcp_rm an audit reports */
	uint32_t reason;		/* E: of the last RSIP or DLCX sent */
	bool offhook;

	/* the request in force */
	char rid[MGCP_ID_MAX];		/* "0" before the first request */
	char req_ne[MGCP_ENTITY_MAX];	/* its N:, which the NTFY repeats */
	struct mgcp_req_ent req[MGCP_REQ_MAX];
	unsigned int nreq;
	char req_text[MGCP_TEXT_MAX];
	struct mgcp_ev_pat detect[MGCP_REQ_MAX];
	unsigned int ndetect;
	bool detect_set;		/* a T: arrived since the restart */
	char detect_text[MGCP_TEXT_MAX];
	bool q_loop;
	bool q_discard;
	struct dialplan dp;
	char dmap[CFG_DIGIT_MAP_MAX];
	struct mgcp_ev_pat persist[MGCP_REQ_MAX];
	unsigned int npersist;

	/* signals */
	struct mgcp_sig_run sig[MGCP_SIG_RUN_MAX];
	bool vmwi_on;
	bool lsa_on;
	char br_pending[MGCP_TEXT_MAX];
	/* the stream the digits went to; a cancel names the same one, also
	 * after its connection went */
	unsigned int br_stream;
	char bearer[8];

	/* digit collection and the observed events */
	char dial[DIALPLAN_DIGITS_MAX + 1];
	unsigned int ndial;
	bool dial_timer;		/* the digit map set names T */
	struct proto_timer digit_timer;
	struct mgcp_obs obs[MGCP_OBS_MAX];
	unsigned int nobs;
	struct mgcp_obs quar[MGCP_QUAR_MAX];
	unsigned int nquar;
	uint32_t quar_dropped;
	struct mgcp_obs later[4];	/* events behind a response */
	unsigned int nlater;

	/* notification state (RFC 3435 4.4.1) */
	bool notif;			/* a NTFY left, no response yet */
	bool notified;			/* the request in force caused one */
	uint32_t ntfy_txid;		/* 0 when that NTFY was lost */
	uint32_t rsip_txid;		/* this endpoint's own RSIP */
	bool disc;			/* disconnected (RFC 3435 4.4.7) */

	struct mgcp_conn conn[MGCP_CONN_MAX];
	bool conf_on;
	struct mgcp_rec rec[MGCP_CONN_MAX];
	uint8_t state_last;		/* the state the trace saw last */
};

/* The counters of the gateway, for the status table. */
struct mgcp_ep_stat {
	uint64_t commands;
	uint64_t errors;		/* responses of class 4xx and 5xx */
	uint64_t ntfy;
	uint64_t rsip;
	uint64_t quar_dropped;
};

struct mgcp_gw {
	const struct proto_host *host;
	struct mgcp_txn_mgr *mgr;
	struct mgcp_ep_env env;
	struct mgcp_ep_cfg cfg;
	char wild[MGCP_EP_NAME_MAX];	/* every endpoint, as RSIP names them */
	struct mgcp_ep ep[MGCP_EP_MAX];

	/* mgcp_ep_start() ran; before it a service change sends nothing */
	bool started;
	/* mgcp_ep_stop_begin() ran; no restart RSIP goes out after it */
	bool stopping;

	struct proto_timer restart_timer;
	bool restart_wait;		/* the delay runs, no RSIP left yet */
	bool restart_stop;		/* a command or the timer restarts it */
	uint32_t restart_mask;		/* the endpoints the wildcard names */
	uint32_t restart_txid;
	uint32_t restart_handle;

	/*
	 * The RSIP of the disconnected procedure the transaction layer runs:
	 * the endpoints it names, and a success that waits for its response
	 * before the endpoints drain.
	 */
	uint32_t disc_handle;
	uint32_t disc_txid;
	uint32_t disc_mask;
	bool disc_end;

	struct mgcp_ep_stat stat;

	/* scratch, so no handler holds these on its stack */
	struct mgcp_conn scratch;
	char wire[MGCP_TXN_WIRE_DEFAULT];
	char body[MGCP_BODY_MAX];
	char arena[MGCP_ARENA_MAX];
	size_t arena_used;
};

/*
 * Take the caller's view and name the endpoints. Every endpoint starts
 * restarting, with the persistent events as its request under the
 * identifier "0" and the digit map of its channel. 0, or -EINVAL.
 */
int mgcp_ep_init(struct mgcp_gw *gw, const struct proto_host *host,
		 struct mgcp_txn_mgr *mgr, const struct mgcp_ep_env *env,
		 const struct mgcp_ep_cfg *cfg);

/*
 * Cancel every timer and give every RTP port back. It sends nothing and
 * calls no stream op: it runs after the backend closed, and the streams
 * stopped when the lines left service (mgcp_ep_service).
 */
void mgcp_ep_done(struct mgcp_gw *gw);

/*
 * The daemon stops (RFC 3435 2.3.12 p. 67). Each endpoint in service that
 * holds a connection sends RSIP "graceful" with RD: delay_s, and no restart
 * RSIP goes out after this call. The RSIP "forced" of each endpoint follows
 * when the line leaves service (mgcp_ep_service).
 */
void mgcp_ep_stop_begin(struct mgcp_gw *gw, uint32_t delay_s);

/*
 * Arm the restart delay, uniform between 0 and restart_delay_max_ms from
 * host->random_bytes (RFC 3435 4.4.6 step 1). Its end, a command or an
 * off-hook sends RSIP "restart": one wildcard when every endpoint shares
 * one notified entity, else one RSIP per endpoint. An RSIP that does not
 * go out arms the delay again.
 */
void mgcp_ep_start(struct mgcp_gw *gw);

/*
 * Upcalls of struct mgcp_txn_env. src is where the response goes. A gateway
 * command that ends in MGCP_TXN_DONE_TIMEOUT puts the endpoints it names in
 * the disconnected procedure; mgcp_ep_disconnected() marks none.
 */
void mgcp_ep_command(struct mgcp_gw *gw, const struct mgcp_msg *msg,
		     const struct sockaddr *src, socklen_t src_len);
void mgcp_ep_response(struct mgcp_gw *gw, uint32_t owner, uint32_t txid,
		      const struct mgcp_msg *msg);
void mgcp_ep_txn_done(struct mgcp_gw *gw, uint32_t owner, uint32_t txid,
		      enum mgcp_txn_reason reason);
void mgcp_ep_disconnected(struct mgcp_gw *gw);
void mgcp_ep_disc_attempt(struct mgcp_gw *gw);
void mgcp_ep_disc_ended(struct mgcp_gw *gw);

/*
 * Every endpoint in service enters the disconnected procedure, as for a lost
 * notified entity. The protocol unit calls it when its socket cannot bind
 * to the address of the interface (RFC 3435 4.4.7).
 */
void mgcp_ep_disc_all(struct mgcp_gw *gw);

/* The ops of struct proto_ops the endpoints answer. */
void mgcp_ep_line_event(struct mgcp_gw *gw, unsigned int line,
			const struct proto_line_event *ev);
void mgcp_ep_signal_result(struct mgcp_gw *gw, unsigned int line,
			   enum proto_signal sig, bool ok);
void mgcp_ep_ring_failed(struct mgcp_gw *gw, unsigned int line);

/*
 * Take the line out of service or back in. Out: a gateway DLCX with E:
 * reason for each connection, then RSIP "forced" for the endpoint; every
 * non-audit command then gets 501. In: RSIP "restart" for the endpoint.
 * Before mgcp_ep_start() the state changes and nothing goes out: the
 * restart of the start covers the endpoints in service.
 */
void mgcp_ep_service(struct mgcp_gw *gw, unsigned int line, bool in_service,
		     uint32_t reason);

/*
 * Cancel the timers of the endpoint, the digit timer and the time-outs of
 * its signals, before the core initializes the line again
 * (proto/proto.h line_reset). The signals the timers ended leave the record.
 */
void mgcp_ep_line_reset(struct mgcp_gw *gw, unsigned int line);

/* enum proto_line_flag bits and the state word of the line. */
uint32_t mgcp_ep_line_flags(const struct mgcp_gw *gw, unsigned int line);

/*
 * Whether a digit may play toward the phone (proto/proto.h digit_play_allowed):
 * true while the endpoint is in service, on any stream, because a D/ signal
 * plays with or without a connection (RFC 3660 2.3).
 */
bool mgcp_ep_digit_play_allowed(const struct mgcp_gw *gw, unsigned int line);
const char *mgcp_ep_state_name(const struct mgcp_gw *gw, unsigned int line);

/* The name of a service state, as the status table writes it. */
const char *mgcp_ep_svc_name(enum mgcp_ep_svc svc);

/*
 * Expand an endpoint pattern: {line} is line_no, or "*" when line_no is
 * negative, and {host} is host. false when out runs short.
 */
bool mgcp_ep_name_expand(const char *pattern, int line_no, const char *host,
			 char *out, size_t olen);

#endif
