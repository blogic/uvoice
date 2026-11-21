// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The Jingle codec of the XMPP unit (src/proto/xmpp/jingle.c): the <jingle/>
 * element of XEP-0166 with one audio content of XEP-0167, the ICE-UDP
 * transport of XEP-0176 and the DTLS fingerprint of XEP-0320, parsed into
 * values and written from them. No state lives here: the session machine
 * is the call layer's.
 *
 * One content. The phone carries one audio stream per call, so the parser
 * reads the first <content/> that holds an RTP description or an ICE-UDP
 * transport and counts the rest. The writer writes one.
 *
 * Candidates travel as SDP lines. The HAL takes and hands out a=candidate
 * lines (hal.h stream_candidate_add), so a Jingle <candidate/> is turned
 * into that line on the way in and built from it on the way out
 * (RFC 5245 section 15.1 against XEP-0176 section 5.1).
 *
 * The fingerprint is the SHA-256 digest of the certificate, 32 bytes, which
 * is the one hash the transport library computes and verifies
 * (uvtransport.h). Another hash is parsed as absent.
 */
#ifndef UVOICE_XMPP_JINGLE_H
#define UVOICE_XMPP_JINGLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto/xmpp/jid.h"
#include "proto/xmpp/xml.h"

#define JINGLE_NS		"urn:xmpp:jingle:1"
#define JINGLE_NS_RTP		"urn:xmpp:jingle:apps:rtp:1"
#define JINGLE_NS_RTP_INFO	"urn:xmpp:jingle:apps:rtp:info:1"
#define JINGLE_NS_ICE		"urn:xmpp:jingle:transports:ice-udp:1"
#define JINGLE_NS_DTLS		"urn:xmpp:jingle:apps:dtls:0"
#define JINGLE_NS_ERRORS	"urn:xmpp:jingle:errors:1"

#define JINGLE_SID_MAX		64U
#define JINGLE_NAME_MAX		32U
#define JINGLE_WORD_MAX		32U
#define JINGLE_TEXT_MAX		128U
#define JINGLE_PAYLOADS_MAX	16U
#define JINGLE_CANDIDATES_MAX	16U
/* RFC 8445 section 5.3 bounds with the terminator, as hal.h has them. */
#define JINGLE_UFRAG_MAX	257U
#define JINGLE_PWD_MAX		257U
/* One a=candidate line with its terminator, as hal.h has it. */
#define JINGLE_CANDIDATE_MAX	256U
#define JINGLE_FP_LEN		32U
#define JINGLE_CONTENT_DEFAULT	"audio"
/* A session-initiate with sixteen candidates fits with room to spare. */
#define JINGLE_STANZA_MAX	8192U

enum jingle_action {
	JINGLE_ACTION_NONE = 0,
	JINGLE_SESSION_INITIATE,
	JINGLE_SESSION_ACCEPT,
	JINGLE_SESSION_TERMINATE,
	JINGLE_SESSION_INFO,
	JINGLE_TRANSPORT_INFO,
	JINGLE_TRANSPORT_ACCEPT,
	JINGLE_TRANSPORT_REJECT,
	JINGLE_TRANSPORT_REPLACE,
	JINGLE_CONTENT_ACCEPT,
	JINGLE_CONTENT_ADD,
	JINGLE_CONTENT_MODIFY,
	JINGLE_CONTENT_REJECT,
	JINGLE_CONTENT_REMOVE,
	JINGLE_DESCRIPTION_INFO,
	JINGLE_SECURITY_INFO,
	JINGLE_ACTION_MAX,
};

/* XEP-0166 section 7.4, the reason of a session-terminate. */
enum jingle_reason {
	JINGLE_REASON_NONE = 0,
	JINGLE_REASON_SUCCESS,
	JINGLE_REASON_BUSY,
	JINGLE_REASON_DECLINE,
	JINGLE_REASON_CANCEL,
	JINGLE_REASON_CONNECTIVITY_ERROR,
	JINGLE_REASON_FAILED_APPLICATION,
	JINGLE_REASON_FAILED_TRANSPORT,
	JINGLE_REASON_GENERAL_ERROR,
	JINGLE_REASON_GONE,
	JINGLE_REASON_INCOMPATIBLE_PARAMETERS,
	JINGLE_REASON_MEDIA_ERROR,
	JINGLE_REASON_SECURITY_ERROR,
	JINGLE_REASON_TIMEOUT,
	JINGLE_REASON_UNSUPPORTED_APPLICATIONS,
	JINGLE_REASON_UNSUPPORTED_TRANSPORTS,
	JINGLE_REASON_EXPIRED,
	JINGLE_REASON_ALTERNATIVE_SESSION,
	JINGLE_REASON_MAX,
};

/* XEP-0167 section 8, the payload of a session-info. */
enum jingle_info {
	JINGLE_INFO_NONE = 0,
	JINGLE_INFO_ACTIVE,
	JINGLE_INFO_HOLD,
	JINGLE_INFO_UNHOLD,
	JINGLE_INFO_MUTE,
	JINGLE_INFO_UNMUTE,
	JINGLE_INFO_RINGING,
	JINGLE_INFO_MAX,
};

/* RFC 4145 section 4, as XEP-0320 carries it. */
enum jingle_setup {
	JINGLE_SETUP_NONE = 0,
	JINGLE_SETUP_ACTPASS,
	JINGLE_SETUP_ACTIVE,
	JINGLE_SETUP_PASSIVE,
	JINGLE_SETUP_MAX,
};

struct jingle_payload {
	uint8_t id;
	uint8_t channels;		/* 0 when the element named none */
	uint32_t clockrate;		/* 0 when the element named none */
	char name[JINGLE_WORD_MAX];	/* "" when the element named none */
};

struct jingle_candidate {
	char sdp[JINGLE_CANDIDATE_MAX];
};

/*
 * The ICE-UDP transport with its DTLS fingerprint. fp_present says the
 * fingerprint was a SHA-256 one this layer could read, with its setup.
 * candidates_dropped says the element held more than fit.
 */
struct jingle_transport {
	char ufrag[JINGLE_UFRAG_MAX];
	char pwd[JINGLE_PWD_MAX];
	uint8_t fp[JINGLE_FP_LEN];
	bool fp_present;
	enum jingle_setup setup;
	struct jingle_candidate candidate[JINGLE_CANDIDATES_MAX];
	unsigned int candidates;
	bool candidates_dropped;
};

/*
 * One content. description says an RTP description was present, audio
 * that it named media audio. payloads_dropped says the description held
 * more payload types than fit. transport says an ICE-UDP transport was
 * present, with its values in ice.
 */
struct jingle_content {
	char name[JINGLE_NAME_MAX];
	char creator[JINGLE_WORD_MAX];
	char senders[JINGLE_WORD_MAX];	/* "" when the element named none */
	bool description;
	bool audio;
	struct jingle_payload payload[JINGLE_PAYLOADS_MAX];
	unsigned int payloads;
	bool payloads_dropped;
	bool rtcp_mux;
	bool transport;
	struct jingle_transport ice;
};

/*
 * One parsed <jingle/>. contents counts every <content/> the element held,
 * content holds the first with a description or a transport. reason and
 * info are the children of a session-terminate and a session-info.
 */
struct jingle_msg {
	enum jingle_action action;
	char sid[JINGLE_SID_MAX];
	char initiator[JID_MAX];
	char responder[JID_MAX];
	unsigned int contents;
	struct jingle_content content;
	enum jingle_reason reason;
	char reason_text[JINGLE_TEXT_MAX];
	enum jingle_info info;
};

/*
 * What this side writes into a content: the payload types in preference
 * order, the ICE credentials and the fingerprint.
 */
struct jingle_local {
	char content_name[JINGLE_NAME_MAX];
	char creator[JINGLE_WORD_MAX];
	struct jingle_payload payload[JINGLE_PAYLOADS_MAX];
	unsigned int payloads;
	bool rtcp_mux;
	char ufrag[JINGLE_UFRAG_MAX];
	char pwd[JINGLE_PWD_MAX];
	uint8_t fp[JINGLE_FP_LEN];
	bool fp_present;
	enum jingle_setup setup;
};

/**
 * jingle_action_name() - the wire word of an action
 * @action: the action
 *
 * Return: the word, "" for none.
 */
const char *jingle_action_name(enum jingle_action action);

/**
 * jingle_action_of() - the action of a wire word
 * @name: the word
 *
 * Return: the action, JINGLE_ACTION_NONE for an unknown word.
 */
enum jingle_action jingle_action_of(const char *name);

/**
 * jingle_reason_name() - the element name of a reason
 * @reason: the reason
 *
 * Return: the name, "" for none.
 */
const char *jingle_reason_name(enum jingle_reason reason);

/**
 * jingle_info_name() - the element name of a session-info payload
 * @info: the payload
 *
 * Return: the name, "" for none.
 */
const char *jingle_info_name(enum jingle_info info);

/**
 * jingle_setup_name() - the setup word of XEP-0320
 * @setup: the setup
 *
 * Return: the word, "" for none.
 */
const char *jingle_setup_name(enum jingle_setup setup);

/**
 * jingle_setup_of() - the setup of a wire word
 * @name: the word
 *
 * Return: the setup, JINGLE_SETUP_NONE for an unknown word.
 */
enum jingle_setup jingle_setup_of(const char *name);

/**
 * jingle_parse() - read the <jingle/> of an iq
 * @iq: the iq stanza
 * @out: receives the values, cleared first
 *
 * Return: true when the iq is of type set and carries a jingle element with
 * an action this layer knows and a sid.
 */
bool jingle_parse(const struct xml_doc *iq, struct jingle_msg *out);

/**
 * jingle_candidate_sdp() - the a=candidate line of a <candidate/>
 * @doc: the tree
 * @node: the candidate element
 * @out: receives the line
 * @len: capacity of @out
 *
 * Return: true when the element carried what the line needs.
 */
bool jingle_candidate_sdp(const struct xml_doc *doc, unsigned int node,
			  char *out, size_t len);

/**
 * jingle_fingerprint_parse() - the bytes of a fingerprint text
 * @text: the colon separated hex pairs
 * @out: receives JINGLE_FP_LEN bytes
 *
 * Return: true when the text held exactly 32 pairs.
 */
bool jingle_fingerprint_parse(const char *text, uint8_t out[JINGLE_FP_LEN]);

/**
 * jingle_fingerprint_text() - the colon separated hex of a fingerprint
 * @fp: the bytes
 * @out: receives the text
 * @len: capacity of @out, 96 suffices
 */
void jingle_fingerprint_text(const uint8_t fp[JINGLE_FP_LEN], char *out,
			     size_t len);

/**
 * jingle_open() - start an iq with its <jingle/>
 * @writer: the writer
 * @to: the peer
 * @id: the iq id
 * @action: the action
 * @sid: the session id
 * @initiator: the initiator attribute, NULL for none
 * @responder: the responder attribute, NULL for none
 */
void jingle_open(struct xml_w *writer, const char *to, const char *id,
		 enum jingle_action action, const char *sid,
		 const char *initiator, const char *responder);

/**
 * jingle_close() - end the <jingle/> and its iq
 * @writer: the writer
 */
void jingle_close(struct xml_w *writer);

/**
 * jingle_content_open() - start the one content
 * @writer: the writer
 * @local: the names
 */
void jingle_content_open(struct xml_w *writer,
			 const struct jingle_local *local);

/**
 * jingle_content_close() - end the content
 * @writer: the writer
 */
void jingle_content_close(struct xml_w *writer);

/**
 * jingle_description_write() - the RTP description of the content
 * @writer: the writer
 * @local: the payload types
 */
void jingle_description_write(struct xml_w *writer,
			      const struct jingle_local *local);

/**
 * jingle_transport_open() - start the ICE-UDP transport of the content
 * @writer: the writer
 * @local: the credentials and the fingerprint
 * @fingerprint: whether the fingerprint element is written
 */
void jingle_transport_open(struct xml_w *writer,
			   const struct jingle_local *local, bool fingerprint);

/**
 * jingle_transport_close() - end the transport
 * @writer: the writer
 */
void jingle_transport_close(struct xml_w *writer);

/**
 * jingle_candidate_write() - one <candidate/> from an a=candidate line
 * @writer: the writer, inside an open transport
 * @sdp: the line, with or without the a= prefix
 *
 * Return: true when the line parsed, false when nothing was written.
 */
bool jingle_candidate_write(struct xml_w *writer, const char *sdp);

/**
 * jingle_reason_write() - the <reason/> of a session-terminate
 * @writer: the writer, inside the jingle element
 * @reason: the reason
 * @text: a human text, NULL for none
 */
void jingle_reason_write(struct xml_w *writer, enum jingle_reason reason,
			 const char *text);

/**
 * jingle_info_write() - the payload of a session-info
 * @writer: the writer, inside the jingle element
 * @info: the payload
 */
void jingle_info_write(struct xml_w *writer, enum jingle_info info);

#endif
