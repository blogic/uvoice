// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Jingle call legs (src/proto/xmpp/call.c): the XMPP side of one call of a
 * line, beside the SIP legs of sip/call.c. The line shell drives both
 * through the same commands and hears both through the same struct call_cb,
 * so the desk machine never learns which protocol carries a call.
 *
 * One leg is one Jingle session (XEP-0166) with one audio content
 * (XEP-0167) over ICE-UDP (XEP-0176) and DTLS-SRTP (XEP-0320). Every call
 * starts with Jingle Message Initiation (XEP-0353): an outbound call
 * proposes to the bare JID of the callee, so every device of that account
 * rings. The device that answers with proceed becomes the peer of the
 * session. An inbound call arrives as a propose. This phone answers ringing
 * and, when the user picks up, proceed to the caller and accept to its own
 * account, so the other devices stop. A proceed or an accept another device
 * of this account sent ends the ring here as answered elsewhere.
 *
 * Media. The leg programs its HAL stream with a HAL_TRANSPORT_ICE_DTLS
 * description (hal.h). The backend gathers the candidates and runs the
 * handshake. The leg reads the local credentials and the fingerprint from
 * the backend, writes them into the session-initiate or the session-accept,
 * trickles each gathered candidate in a transport-info and hands every
 * candidate of the peer down. The initiator is the controlling agent with
 * setup actpass, the responder is controlled with setup active, as RFC 5763
 * section 5 has it.
 *
 * The environment. Everything the legs need of the outside arrives through
 * struct xcall_env: the stream of the line, the media ops, the clock, the
 * callbacks of the shell and the services of the account. A suite fills it
 * with fakes and drives the legs without a daemon.
 *
 * Slots. One leg per UI slot, CALL_SLOTS per line, the physical and the UI
 * slot being the same number: no transfer moves a Jingle leg.
 */
#ifndef UVOICE_XMPP_CALL_H
#define UVOICE_XMPP_CALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cfg/cfg.h"
#include "hal/hal.h"
#include "proto/proto.h"
#include "proto/shared/timer.h"
#include "proto/sip/call.h"
#include "proto/xmpp/jid.h"
#include "proto/xmpp/jingle.h"
#include "proto/xmpp/xml.h"

/* How long a propose rings every device before the caller gives up. */
#define XCALL_RING_MS		60000U
/* How long the session is awaited once a device answered proceed. */
#define XCALL_SETUP_MS		20000U
/* How long an offered call may ring here before the caller is told. */
#define XCALL_OFFER_MS		120000U
/* How long the transport may take to carry media once both sides spoke. */
#define XCALL_ICE_MS		30000U
/* How long a session-terminate is awaited once the peer closed the media. */
#define XCALL_CLOSE_MS		5000U
#define XCALL_NAME_MAX		64U
#define XCALL_TARGET_MAX	JID_MAX

enum xcall_state {
	XCALL_FREE = 0,
	XCALL_PROPOSED,		/* outbound, the propose rings the callee */
	XCALL_OFFERED,		/* inbound, this phone rings */
	XCALL_PROCEEDED,	/* inbound, proceed sent, the session is owed */
	XCALL_INITIATED,	/* outbound, the initiate awaits its accept */
	XCALL_ACTIVE,
	XCALL_STATE_MAX,
};

/**
 * typedef xcall_reply_cb - the answer to an iq the legs sent
 * @priv: the pointer the leg gave
 * @ok: true for a result, false for an error or no answer at all
 * @reply: the iq, NULL when none arrived
 */
typedef void (*xcall_reply_cb)(void *priv, bool ok,
			       const struct xml_doc *reply);

/*
 * The services of the account layer, per line.
 *
 * ready        whether the stream of the line carries stanzas.
 * send         one stanza on the stream, 0 or a negative errno.
 * iq_send      an iq that awaits its answer through cb.
 * reply_result an empty result for an iq the leg took.
 * reply_error  a stanza error for an iq the leg refused.
 * id_next      a fresh stanza id.
 * self_jid     the full JID the stream bound, false before the bind.
 * local_addr   the address of the voice interface the media binds to.
 * bind_dev     the netdev the media scopes to, "" for none.
 * contact_name the roster name of a bare JID, false for none.
 * speed_dial   the JID a dialled digit string stands for, false for none.
 * ice_servers  the STUN and TURN servers of the line, into the transport.
 * relay_missing the transport failed and no relay was available, so the
 *              user is told.
 */
struct xcall_env {
	struct proto_timer_env *timer;
	const struct call_media_ops *media;
	const struct call_cb *cb;
	const struct proto_host *host;
	void *priv;
	bool (*ready)(void *priv, unsigned int line);
	int (*send)(void *priv, unsigned int line, const char *text,
		    size_t len);
	int (*iq_send)(void *priv, unsigned int line, const char *text,
		       size_t len, const char *id, xcall_reply_cb cb,
		       void *cb_priv);
	int (*reply_result)(void *priv, unsigned int line,
			    const struct xml_doc *iq);
	int (*reply_error)(void *priv, unsigned int line,
			   const struct xml_doc *iq, const char *type,
			   const char *condition);
	void (*id_next)(void *priv, unsigned int line, char *out, size_t len);
	bool (*self_jid)(void *priv, unsigned int line, struct jid *out);
	bool (*local_addr)(void *priv, unsigned int line, char *out,
			   size_t len);
	const char *(*bind_dev)(void *priv, unsigned int line);
	bool (*contact_name)(void *priv, unsigned int line, const char *bare,
			     char *out, size_t len);
	bool (*speed_dial)(void *priv, unsigned int line, const char *digits,
			   char *out, size_t len);
	void (*ice_servers)(void *priv, unsigned int line,
			    struct hal_transport *out);
	void (*relay_missing)(void *priv, unsigned int line);
};

/**
 * xcalls_init() - wire the leg layer
 * @env: the environment, copied
 */
void xcalls_init(const struct xcall_env *env);

/**
 * xcalls_reload() - take the configuration the next legs read
 * @cfg: the running configuration
 */
void xcalls_reload(const struct cfg *cfg);

/**
 * xcalls_hal_features_set() - the feature bits of the backend
 * @features: the enum hal_feature bits
 */
void xcalls_hal_features_set(uint32_t features);

/**
 * xcalls_hal_codecs_set() - the codec bits of the backend
 * @codecs: 1 << enum hal_codec per codec
 */
void xcalls_hal_codecs_set(uint32_t codecs);

/**
 * xcalls_done() - end every leg without a word on the wire
 */
void xcalls_done(void);

/**
 * xcalls_line_teardown() - end every leg of a line, with a terminate
 * @line: the line
 */
void xcalls_line_teardown(unsigned int line);

/**
 * xcalls_line_busy() - whether a leg of the line still exists
 * @line: the line
 *
 * Return: true while one does.
 */
bool xcalls_line_busy(unsigned int line);

/**
 * xcall_slot_free() - the admission check of a new call
 * @line: the line
 * @slot: receives a free slot
 *
 * Return: true when a slot is free.
 */
bool xcall_slot_free(unsigned int line, unsigned int *slot);

/**
 * xcall_slot_stream() - the stream a UI slot runs on
 * @line: the line
 * @ui_slot: the UI slot
 *
 * Return: the slot itself while a leg holds it, CALL_SLOTS otherwise.
 */
unsigned int xcall_slot_stream(unsigned int line, unsigned int ui_slot);

/**
 * xcall_line_dial_ready() - whether the line can place a call now
 * @line: the line
 *
 * Return: true when the stream is ready.
 */
bool xcall_line_dial_ready(unsigned int line);

/**
 * xcall_dial() - place a call
 * @line: the line
 * @slot: the free slot
 * @target: a JID, or digits the speed dial maps to one
 *
 * A target nothing maps reports the unobtainable class and returns false,
 * as call_dial() does for a dial it cannot place.
 *
 * Return: true when the propose left.
 */
bool xcall_dial(unsigned int line, unsigned int slot, const char *target);

/**
 * xcall_ring() - the shell rings for an offered call
 * @line: the line
 * @slot: the slot
 *
 * The ringing message left when the offer arrived, so this changes nothing.
 *
 * Return: true for an offered leg.
 */
bool xcall_ring(unsigned int line, unsigned int slot);

/**
 * xcall_answer() - take an offered call
 * @line: the line
 * @slot: the slot
 *
 * Return: true when the proceed left.
 */
bool xcall_answer(unsigned int line, unsigned int slot);

/**
 * xcall_reject() - decline an offered call
 * @line: the line
 * @slot: the slot
 * @code: unused, the SIP status the shell would have sent
 *
 * Return: true when the reject left.
 */
bool xcall_reject(unsigned int line, unsigned int slot, uint32_t code);

/**
 * xcall_hangup() - end a call in any state
 * @line: the line
 * @slot: the slot
 *
 * Return: true when a leg was there.
 */
bool xcall_hangup(unsigned int line, unsigned int slot);

/**
 * xcall_hold() - hold an active call
 * @line: the line
 * @slot: the slot
 *
 * Reports reinvite_done true once the session-info left.
 *
 * Return: true when it left.
 */
bool xcall_hold(unsigned int line, unsigned int slot);

/**
 * xcall_resume() - resume a held call
 * @line: the line
 * @slot: the slot
 *
 * Return: true when it left.
 */
bool xcall_resume(unsigned int line, unsigned int slot);

/**
 * xcall_dtmf() - send a digit as a telephone event
 * @line: the line
 * @slot: the slot
 * @digit: the digit
 * @duration_ms: its length
 * @measured: whether a detector of the line timed it
 *
 * Return: true when the backend took it.
 */
bool xcall_dtmf(unsigned int line, unsigned int slot, char digit,
		uint16_t duration_ms, bool measured);

/**
 * xcall_jingle_rx() - an iq of type set that may carry a jingle element
 * @line: the line
 * @iq: the stanza
 *
 * Return: true when the iq was a Jingle one, answered here either way.
 */
bool xcall_jingle_rx(unsigned int line, const struct xml_doc *iq);

/**
 * xcall_jmi_rx() - a message that may carry a Jingle Message Initiation
 * @line: the line
 * @message: the stanza
 *
 * Return: true when the message was of that family.
 */
bool xcall_jmi_rx(unsigned int line, const struct xml_doc *message);

/**
 * xcall_transport_event() - a HAL_EVENT_TRANSPORT record of a stream
 * @line: the line
 * @stream: the stream
 * @what: the enum hal_transport_ev
 * @state: the enum hal_ice_state of an ICE_STATE record
 */
void xcall_transport_event(unsigned int line, unsigned int stream,
			   unsigned int what, unsigned int state);

/**
 * xcalls_stream_down() - the stream of a line went
 * @line: the line
 *
 * Every leg of the line ends with the quiet failure class.
 */
void xcalls_stream_down(unsigned int line);

/**
 * xcall_state_name() - the word of a state
 * @state: the state
 *
 * Return: the word.
 */
const char *xcall_state_name(enum xcall_state state);

struct blob_buf;

/**
 * xcalls_status_dump() - the legs of a line for the status verb
 * @b: the blob buffer
 * @line: the line
 */
void xcalls_status_dump(struct blob_buf *b, unsigned int line);

#endif
