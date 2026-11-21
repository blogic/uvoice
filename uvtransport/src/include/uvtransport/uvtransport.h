/* SPDX-License-Identifier: LGPL-2.1-only */
/*
 * uvtransport - ICE and DTLS-SRTP media transport for one stream
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 */
#ifndef UVTRANSPORT_H
#define UVTRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* RFC 8445 section 5.3: ufrag 4 to 256 and pwd 22 to 256 characters. */
#define UVT_UFRAG_MAX		257U
#define UVT_PWD_MAX		257U
#define UVT_HOST_MAX		64U
#define UVT_CRED_MAX		64U
#define UVT_TURN_MAX		2U
/* One a=candidate line with its terminator. */
#define UVT_CANDIDATE_MAX	256U
/* SHA-256, the one fingerprint hash this library offers and verifies. */
#define UVT_FINGERPRINT_LEN	32U
/* The largest datagram a stream carries, before protection. */
#define UVT_PACKET_MAX		1500U

enum uvt_role {
	UVT_ROLE_CONTROLLED = 0,
	UVT_ROLE_CONTROLLING,
};

enum uvt_setup {
	UVT_SETUP_ACTPASS = 0,
	UVT_SETUP_ACTIVE,
	UVT_SETUP_PASSIVE,
};

enum uvt_ice_state {
	UVT_ICE_DISCONNECTED = 0,
	UVT_ICE_GATHERING,
	UVT_ICE_CONNECTING,
	UVT_ICE_CONNECTED,
	UVT_ICE_COMPLETED,
	UVT_ICE_FAILED,
};

enum uvt_event {
	UVT_EV_CANDIDATE = 0,
	UVT_EV_GATHERING_DONE,
	UVT_EV_ICE_STATE,
	UVT_EV_DTLS_UP,
	UVT_EV_DTLS_FAIL,
	UVT_EV_DTLS_CLOSED,
};

struct uvt_turn {
	char host[UVT_HOST_MAX];
	char user[UVT_CRED_MAX];
	char pass[UVT_CRED_MAX];
	uint16_t port;
};

/*
 * Thread contract. The callbacks run on a library thread. on_rtp and on_rtcp
 * hand over the plaintext of one datagram, valid for the call only. on_event
 * reports one enum uvt_event with its argument: the pending candidate count
 * for UVT_EV_CANDIDATE, the enum uvt_ice_state for UVT_EV_ICE_STATE and 0
 * otherwise. UVT_EV_DTLS_CLOSED says the far end ended the DTLS session in
 * order with a close_notify: no media flows after it, and it is no failure.
 * A callback may call every function of this API except uvt_destroy.
 */
struct uvt_cfg {
	enum uvt_role role;
	enum uvt_setup setup;
	char stun_host[UVT_HOST_MAX];
	uint16_t stun_port;
	struct uvt_turn turn[UVT_TURN_MAX];
	unsigned int turns;
	char bind_address[UVT_HOST_MAX];
	uint16_t port_min;
	uint16_t port_max;
	void (*on_rtp)(void *user, const uint8_t *buf, size_t len);
	void (*on_rtcp)(void *user, const uint8_t *buf, size_t len);
	void (*on_event)(void *user, enum uvt_event ev, int arg);
	void *user;
};

struct uvt_stream;

/**
 * uvt_init() - initialise the process-global state
 *
 * Generates the DTLS certificate of this process, builds the DTLS context and
 * initialises libuvsrtp. Idempotent.
 *
 * Return: 0 on success, or a negative errno.
 */
int uvt_init(void);

/**
 * uvt_fingerprint_get() - the SHA-256 fingerprint of the process certificate
 * @out: receives UVT_FINGERPRINT_LEN bytes
 *
 * Return: 0, or -EAGAIN before uvt_init() succeeded.
 */
int uvt_fingerprint_get(uint8_t out[UVT_FINGERPRINT_LEN]);

/**
 * uvt_create() - make the transport of one stream
 * @cfg: the configuration, copied
 *
 * A controlling stream gathers its candidates at once. A controlled stream
 * gathers once uvt_remote_set() named the far end, so the ICE role follows
 * the order of the two calls.
 *
 * Return: the stream, or NULL on failure.
 */
struct uvt_stream *uvt_create(const struct uvt_cfg *cfg);

/**
 * uvt_remote_set() - the far end's ICE credentials and DTLS role
 * @s: the stream
 * @ufrag: the remote ice-ufrag
 * @pwd: the remote ice-pwd
 * @fingerprint: the remote SHA-256 certificate fingerprint
 * @setup: the remote DTLS setup; actpass leaves this side active
 *
 * Return: 0, -EINVAL for a bad credential, or -EALREADY when called twice.
 */
int uvt_remote_set(struct uvt_stream *s, const char *ufrag, const char *pwd,
		   const uint8_t fingerprint[UVT_FINGERPRINT_LEN],
		   enum uvt_setup setup);

/**
 * uvt_candidate_add() - one remote candidate
 * @s: the stream
 * @sdp: an a=candidate line, with or without the a= prefix
 *
 * Return: 0, or -EINVAL for a line libjuice refused.
 */
int uvt_candidate_add(struct uvt_stream *s, const char *sdp);

/**
 * uvt_remote_done() - the far end sent its last candidate
 * @s: the stream
 */
void uvt_remote_done(struct uvt_stream *s);

/**
 * uvt_local_get() - the local ICE credentials
 * @s: the stream
 * @ufrag: receives the ice-ufrag, UVT_UFRAG_MAX bytes
 * @pwd: receives the ice-pwd, UVT_PWD_MAX bytes
 *
 * A controlled stream calls uvt_remote_set() first: libjuice takes the side
 * that describes itself before it knows the far end for the controlling one.
 *
 * Return: 0, or a negative errno.
 */
int uvt_local_get(struct uvt_stream *s, char *ufrag, char *pwd);

/**
 * uvt_candidate_next() - pop one gathered local candidate
 * @s: the stream
 * @out: receives the a=candidate line
 * @len: capacity of @out, UVT_CANDIDATE_MAX suffices
 *
 * Return: 1 with a line in @out, 0 when none waits.
 */
int uvt_candidate_next(struct uvt_stream *s, char *out, size_t len);

/**
 * uvt_send_rtp() - protect and send one RTP packet
 * @s: the stream
 * @buf: the plain packet
 * @len: its length
 *
 * Return: 0, -EAGAIN before DTLS is up, -EPIPE once the DTLS session is
 * closed, or another negative errno.
 */
int uvt_send_rtp(struct uvt_stream *s, const uint8_t *buf, size_t len);

/**
 * uvt_send_rtcp() - protect and send one RTCP packet
 * @s: the stream
 * @buf: the plain packet
 * @len: its length
 *
 * Return: 0, -EAGAIN before DTLS is up, -EPIPE once the DTLS session is
 * closed, or another negative errno.
 */
int uvt_send_rtcp(struct uvt_stream *s, const uint8_t *buf, size_t len);

/**
 * uvt_tick() - run the DTLS retransmission timer
 * @s: the stream
 *
 * Call it at least every 100 ms while the handshake runs.
 */
void uvt_tick(struct uvt_stream *s);

/**
 * uvt_ice_state() - the ICE state of the stream
 * @s: the stream
 *
 * Return: the enum uvt_ice_state.
 */
enum uvt_ice_state uvt_ice_state(struct uvt_stream *s);

/**
 * uvt_up() - whether media can flow
 * @s: the stream
 *
 * Return: true once the DTLS handshake verified the far end and keyed SRTP,
 * and until either end closed the DTLS session.
 */
bool uvt_up(struct uvt_stream *s);

/**
 * uvt_close() - end the DTLS session in order
 * @s: the stream
 *
 * Sends a close_notify to the far end. No media flows after it.
 *
 * Return: 0, or -ENOTCONN when the session is not up.
 */
int uvt_close(struct uvt_stream *s);

/**
 * uvt_destroy() - release the stream and zeroise its keys
 * @s: the stream, or NULL
 *
 * Never from a callback.
 */
void uvt_destroy(struct uvt_stream *s);

#endif
