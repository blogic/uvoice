// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the Jingle call legs (src/proto/xmpp/call.c).
 *
 * No daemon. The environment of the legs is a fake: the stanzas the legs
 * send land in a log, the iq answers are scripted, the clock is virtual
 * with a timer table, the media ops are those of proto_host_fake.c and the
 * callbacks toward the shell are recorded. The stanzas of the peer are
 * texts that run through the real parser of xml.c, so every path the stream
 * would take is the path the suite takes.
 */
#include <string.h>

#include "proto/xmpp/call.h"
#include "proto/xmpp/jmi.h"
#include "proto_host_fake.h"
#include "test_util.h"

#define STREAM_OPEN							\
	"<stream:stream xmlns='jabber:client' "				\
	"xmlns:stream='http://etherx.jabber.org/streams'>"
#define SELF		"user@sip.example.test/uvoice"
#define SELF_BARE	"user@sip.example.test"
#define PEER_BARE	"romeo@montague.example"
#define PEER		"romeo@montague.example/orchard"
#define LOCAL_ADDR	"192.168.1.20"
#define FP_TEXT								\
	"02:1A:CC:54:27:AB:EB:9C:53:3F:3E:4A:65:2E:58:AD:"		\
	"62:61:2F:B4:F7:D1:E8:C0:8C:5E:FF:F4:FB:F6:76:F2"
#define CAND_PEER							\
	"<candidate component='1' foundation='1' generation='0' id='c1' "\
	"ip='10.0.1.1' network='1' port='8998' priority='2130706431' "	\
	"protocol='udp' type='host'/>"
#define TRANSPORT_PEER(setup)						\
	"<transport xmlns='urn:xmpp:jingle:transports:ice-udp:1' "	\
	"pwd='asd88fgpdd777uzjYhagZg' ufrag='8hhy'>"			\
	"<fingerprint xmlns='urn:xmpp:jingle:apps:dtls:0' hash='sha-256' "\
	"setup='" setup "'>" FP_TEXT "</fingerprint>" CAND_PEER		\
	"</transport>"
#define DESC_PEER							\
	"<description xmlns='urn:xmpp:jingle:apps:rtp:1' media='audio'>"\
	"<payload-type id='9' name='G722' clockrate='8000'/>"		\
	"<payload-type id='0' name='PCMU' clockrate='8000'/>"		\
	"<payload-type id='101' name='telephone-event' clockrate='8000'/>"\
	"</description>"

#define LOG_MAX		32
#define TEXT_MAX	JINGLE_STANZA_MAX
#define TIMERS_MAX	8
#define ID_MAX		64

struct sent {
	char text[TEXT_MAX];
	char id[ID_MAX];
	bool iq;
	xcall_reply_cb cb;
	void *cb_priv;
};

struct fake {
	struct sent sent[LOG_MAX];
	unsigned int sends;
	unsigned int results;
	unsigned int errors;
	char error_condition[64];
	unsigned int ids;
	bool ready;
	bool self_known;
	unsigned int relay_missing;
	unsigned int turns;
	struct proto_timer *timer[TIMERS_MAX];
	unsigned int timers;
	uint64_t now;
};

struct cblog {
	unsigned int incoming;
	unsigned int proceeding;
	unsigned int answered;
	unsigned int failed;
	unsigned int remote_bye;
	unsigned int remote_cancel;
	unsigned int hold_changed;
	unsigned int reinvite_done;
	unsigned int ended;
	unsigned int elsewhere;
	enum call_fail_class fail_class;
	bool held;
	char cid_number[JID_MAX];
	char cid_name[XCALL_NAME_MAX];
	struct proto_call_ended ended_info;
	char ended_number[JID_MAX];
	char ended_remote[JID_MAX];
};

static struct fake fake;
static struct cblog cbl;
static struct proto_timer_env timer_env;
static struct cfg cfg;
static struct xml_stream parser;
static const struct xml_doc *parsed;
static unsigned int parsed_count;
static unsigned int line_now;
static bool parsed_is_iq;

/* ------------------------------------------------------------- the clock */

/**
 * fake_now() - the virtual clock
 * @env: unused
 *
 * Return: the time.
 */
static uint64_t fake_now(struct proto_timer_env *env)
{
	return fake.now;
}

/**
 * timer_drop() - forget a timer of the table
 * @timer: the timer
 */
static void timer_drop(struct proto_timer *timer)
{
	unsigned int i;

	for (i = 0; i < fake.timers; i++) {
		if (fake.timer[i] != timer)
			continue;
		fake.timer[i] = fake.timer[--fake.timers];
		return;
	}
}

/**
 * fake_timer_set() - arm a timer on the table
 * @env: unused
 * @timer: the timer
 * @ms: the delay
 */
static void fake_timer_set(struct proto_timer_env *env,
			   struct proto_timer *timer, unsigned int ms)
{
	timer_drop(timer);
	timer->due_ms = fake.now + ms;
	if (fake.timers < TIMERS_MAX)
		fake.timer[fake.timers++] = timer;
}

/**
 * fake_timer_cancel() - disarm a timer
 * @env: unused
 * @timer: the timer
 */
static void fake_timer_cancel(struct proto_timer_env *env,
			      struct proto_timer *timer)
{
	timer_drop(timer);
}

/**
 * advance() - move the clock, firing what falls due
 * @ms: how far
 */
static void advance(uint64_t ms)
{
	uint64_t target = fake.now + ms;
	struct proto_timer *next;
	unsigned int i;

	for (;;) {
		next = NULL;
		for (i = 0; i < fake.timers; i++) {
			if (fake.timer[i]->due_ms <= target &&
			    (!next || fake.timer[i]->due_ms < next->due_ms))
				next = fake.timer[i];
		}
		if (!next)
			break;
		fake.now = next->due_ms;
		timer_drop(next);
		proto_timer_expire(next);
	}

	fake.now = target;
}

/* ------------------------------------------------------- the environment */

/**
 * sent_add() - record one stanza the legs sent
 * @text: the stanza
 * @len: its length
 *
 * Return: the record, or NULL when the log is full.
 */
static struct sent *sent_add(const char *text, size_t len)
{
	struct sent *rec;

	if (fake.sends >= LOG_MAX) {
		fake.sends++;
		return NULL;
	}

	rec = &fake.sent[fake.sends++];
	memset(rec, 0, sizeof(*rec));
	snprintf(rec->text, sizeof(rec->text), "%.*s", (int)len, text);

	return rec;
}

/**
 * env_ready() - the readiness the fake stream reports
 * @priv: unused
 * @line: unused
 *
 * Return: fake.ready.
 */
static bool env_ready(void *priv, unsigned int line)
{
	return fake.ready;
}

/**
 * env_send() - record a stanza
 * @priv: unused
 * @line: unused
 * @text: the stanza
 * @len: its length
 *
 * Return: 0 while the fake stream is ready.
 */
static int env_send(void *priv, unsigned int line, const char *text,
		    size_t len)
{
	sent_add(text, len);

	return fake.ready ? 0 : -ENOTCONN;
}

/**
 * env_iq_send() - record an iq with the callback that awaits its answer
 * @priv: unused
 * @line: unused
 * @text: the iq
 * @len: its length
 * @id: its id
 * @cb: the callback
 * @cb_priv: its pointer
 *
 * Return: 0 while the fake stream is ready.
 */
static int env_iq_send(void *priv, unsigned int line, const char *text,
		       size_t len, const char *id, xcall_reply_cb cb,
		       void *cb_priv)
{
	struct sent *rec = sent_add(text, len);

	if (rec) {
		rec->iq = true;
		snprintf(rec->id, sizeof(rec->id), "%s", id);
		rec->cb = cb;
		rec->cb_priv = cb_priv;
	}

	return fake.ready ? 0 : -ENOTCONN;
}

/**
 * env_reply_result() - count a result the legs sent
 * @priv: unused
 * @line: unused
 * @iq: unused
 *
 * Return: 0.
 */
static int env_reply_result(void *priv, unsigned int line,
			    const struct xml_doc *iq)
{
	fake.results++;

	return 0;
}

/**
 * env_reply_error() - count an error the legs sent and keep its condition
 * @priv: unused
 * @line: unused
 * @iq: unused
 * @type: unused
 * @condition: the condition element
 *
 * Return: 0.
 */
static int env_reply_error(void *priv, unsigned int line,
			   const struct xml_doc *iq, const char *type,
			   const char *condition)
{
	fake.errors++;
	snprintf(fake.error_condition, sizeof(fake.error_condition), "%s",
		 condition);

	return 0;
}

/**
 * env_id_next() - a counted stanza id
 * @priv: unused
 * @line: unused
 * @out: receives the id
 * @len: capacity of @out
 */
static void env_id_next(void *priv, unsigned int line, char *out, size_t len)
{
	snprintf(out, len, "id%u", ++fake.ids);
}

/**
 * env_self_jid() - the bound JID of the fake account
 * @priv: unused
 * @line: unused
 * @out: receives it
 *
 * Return: fake.self_known.
 */
static bool env_self_jid(void *priv, unsigned int line, struct jid *out)
{
	return fake.self_known && jid_parse(SELF, out);
}

/**
 * env_local_addr() - the fixed address of the fake interface
 * @priv: unused
 * @line: unused
 * @out: receives it
 * @len: capacity of @out
 *
 * Return: true.
 */
static bool env_local_addr(void *priv, unsigned int line, char *out,
			   size_t len)
{
	snprintf(out, len, "%s", LOCAL_ADDR);

	return true;
}

/**
 * env_bind_dev() - the fixed netdev of the fake interface
 * @priv: unused
 * @line: unused
 *
 * Return: "eth0".
 */
static const char *env_bind_dev(void *priv, unsigned int line)
{
	return "eth0";
}

/**
 * env_contact_name() - the one roster entry of the fake account
 * @priv: unused
 * @line: unused
 * @bare: the bare JID
 * @out: receives the name
 * @len: capacity of @out
 *
 * Return: true for the peer of the suite.
 */
static bool env_contact_name(void *priv, unsigned int line, const char *bare,
			     char *out, size_t len)
{
	if (strcmp(bare, PEER_BARE))
		return false;

	snprintf(out, len, "Romeo");

	return true;
}

/**
 * env_speed_dial() - the one speed dial entry of the fake account
 * @priv: unused
 * @line: unused
 * @digits: the digits
 * @out: receives the JID
 * @len: capacity of @out
 *
 * Return: true for "42".
 */
static bool env_speed_dial(void *priv, unsigned int line, const char *digits,
			   char *out, size_t len)
{
	if (strcmp(digits, "42"))
		return false;

	snprintf(out, len, "%s", PEER_BARE);

	return true;
}

/**
 * env_ice_servers() - a STUN server and, when scripted, one TURN relay
 * @priv: unused
 * @line: unused
 * @out: the transport block
 */
static void env_ice_servers(void *priv, unsigned int line,
			    struct hal_transport *out)
{
	snprintf(out->stun_host, sizeof(out->stun_host), "203.0.113.9");
	out->stun_port = 3478;
	out->turns = (uint8_t)fake.turns;
	if (fake.turns) {
		snprintf(out->turn[0].host, sizeof(out->turn[0].host),
			 "203.0.113.9");
		out->turn[0].port = 3478;
	}
}

/**
 * env_relay_missing() - count the warning of a call without a relay
 * @priv: unused
 * @line: unused
 */
static void env_relay_missing(void *priv, unsigned int line)
{
	fake.relay_missing++;
}

/* ------------------------------------------------------- the callbacks */

/**
 * cb_incoming() - record an offered call
 * @line: unused
 * @slot: unused
 * @cid_number: the caller
 * @cid_name: the caller's name
 * @number_private: unused
 * @name_private: unused
 * @ring_id: unused
 */
static void cb_incoming(unsigned int line, unsigned int slot,
			const char *cid_number, const char *cid_name,
			bool number_private, bool name_private,
			unsigned int ring_id)
{
	cbl.incoming++;
	snprintf(cbl.cid_number, sizeof(cbl.cid_number), "%s", cid_number);
	snprintf(cbl.cid_name, sizeof(cbl.cid_name), "%s", cid_name);
}

/**
 * cb_proceeding() - count a proceeding report
 * @line: unused
 * @slot: unused
 * @early_media: unused
 */
static void cb_proceeding(unsigned int line, unsigned int slot,
			  bool early_media)
{
	cbl.proceeding++;
}

/**
 * cb_answered() - count an answer
 * @line: unused
 * @slot: unused
 */
static void cb_answered(unsigned int line, unsigned int slot)
{
	cbl.answered++;
}

/**
 * cb_failed() - count a failure and keep its class
 * @line: unused
 * @slot: unused
 * @cls: the class
 */
static void cb_failed(unsigned int line, unsigned int slot,
		      enum call_fail_class cls)
{
	cbl.failed++;
	cbl.fail_class = cls;
}

/**
 * cb_remote_bye() - count a far end clear of an answered call
 * @line: unused
 * @slot: unused
 */
static void cb_remote_bye(unsigned int line, unsigned int slot)
{
	cbl.remote_bye++;
}

/**
 * cb_remote_cancel() - count a withdrawn offer
 * @line: unused
 * @slot: unused
 */
static void cb_remote_cancel(unsigned int line, unsigned int slot)
{
	cbl.remote_cancel++;
}

/**
 * cb_hold_changed() - record a hold of the peer
 * @line: unused
 * @slot: unused
 * @held: the new state
 */
static void cb_hold_changed(unsigned int line, unsigned int slot, bool held)
{
	cbl.hold_changed++;
	cbl.held = held;
}

/**
 * cb_reinvite_done() - count a finished hold or resume
 * @line: unused
 * @slot: unused
 * @ok: unused
 */
static void cb_reinvite_done(unsigned int line, unsigned int slot, bool ok)
{
	cbl.reinvite_done++;
}

/**
 * cb_ended() - keep the record of the leg that ended
 * @line: unused
 * @slot: unused
 * @info: the record, copied with its two strings
 */
static void cb_ended(unsigned int line, unsigned int slot,
		     const struct call_ended *info)
{
	cbl.ended++;
	cbl.ended_info = *info;
	snprintf(cbl.ended_number, sizeof(cbl.ended_number), "%s",
		 info->number ? info->number : "");
	snprintf(cbl.ended_remote, sizeof(cbl.ended_remote), "%s",
		 info->remote_number ? info->remote_number : "");
}

/**
 * cb_elsewhere() - count an offer another device took
 * @line: unused
 * @slot: unused
 */
static void cb_elsewhere(unsigned int line, unsigned int slot)
{
	cbl.elsewhere++;
}

static const struct call_cb call_cb = {
	.incoming = cb_incoming,
	.proceeding = cb_proceeding,
	.answered = cb_answered,
	.failed = cb_failed,
	.remote_bye = cb_remote_bye,
	.remote_cancel = cb_remote_cancel,
	.hold_changed = cb_hold_changed,
	.reinvite_done = cb_reinvite_done,
	.ended = cb_ended,
	.answered_elsewhere = cb_elsewhere,
};

/* ------------------------------------------------------------ the parser */

/**
 * on_stanza() - hand a parsed peer stanza to the legs
 * @user: unused
 * @doc: the tree
 */
static void on_stanza(void *user, const struct xml_doc *doc)
{
	parsed_count++;
	parsed = doc;
	if (xml_node_is(doc, 0, "iq", XMPP_NS_CLIENT))
		parsed_is_iq = xcall_jingle_rx(line_now, doc);
	else
		parsed_is_iq = xcall_jmi_rx(line_now, doc);
	parsed = NULL;
}

static const struct xml_stream_cb parser_cb = {
	.stanza = on_stanza,
};

/**
 * rx() - the peer sends one stanza to line 0
 * @xml: the stanza
 *
 * Return: true when the legs took it as theirs.
 */
static bool rx(const char *xml)
{
	parsed_count = 0;
	parsed_is_iq = false;
	line_now = 0;
	xml_stream_feed(&parser, xml, strlen(xml));

	return parsed_count == 1 && parsed_is_iq;
}

/* ------------------------------------------------------------ fixtures */

/**
 * reset() - a fresh environment and a fresh leg layer
 */
static void reset(void)
{
	struct xcall_env env = {
		.timer = &timer_env,
		.media = &proto_host_fake()->media,
		.cb = &call_cb,
		.host = proto_host_fake(),
		.ready = env_ready,
		.send = env_send,
		.iq_send = env_iq_send,
		.reply_result = env_reply_result,
		.reply_error = env_reply_error,
		.id_next = env_id_next,
		.self_jid = env_self_jid,
		.local_addr = env_local_addr,
		.bind_dev = env_bind_dev,
		.contact_name = env_contact_name,
		.speed_dial = env_speed_dial,
		.ice_servers = env_ice_servers,
		.relay_missing = env_relay_missing,
	};

	memset(&fake, 0, sizeof(fake));
	memset(&cbl, 0, sizeof(cbl));
	fake.ready = true;
	fake.self_known = true;
	fake.now = 1000;

	proto_host_fake_reset();
	snprintf(phf.ice_local.ufrag, sizeof(phf.ice_local.ufrag), "myufrag");
	snprintf(phf.ice_local.pwd, sizeof(phf.ice_local.pwd),
		 "mypasswordmypasswordxx");
	phf.ice_local.fp_hash = HAL_FP_SHA256;
	memset(phf.ice_local.fp, 0xab, sizeof(phf.ice_local.fp));

	memset(&cfg, 0, sizeof(cfg));
	cfg.channels = CFG_CHANNEL_MAX;
	cfg.channel[0].present = true;
	cfg.channel[0].enabled = true;
	cfg.channel[0].dtmf_mode = CFG_DTMF_RFC2833;
	cfg.channel[0].ptime_ms = 20;
	cfg.channel[0].jitter_target = 60;
	cfg.channel[0].jitter_max = 200;
	cfg.channel[0].rtp_tos = 184;
	cfg.channel[0].rtcp_tos = 184;
	cfg.rtcp_enabled = true;
	cfg.rtcp_interval = 5;

	xcalls_init(&env);
	xcalls_reload(&cfg);
	xcalls_hal_features_set(HAL_FEAT_ICE_DTLS);
	xcalls_hal_codecs_set(~0u);

	xml_stream_reset(&parser);
	xml_stream_feed(&parser, STREAM_OPEN, strlen(STREAM_OPEN));
}

/**
 * sent_find() - the last stanza that holds a text
 * @needle: the text
 *
 * Return: the record, or NULL.
 */
static struct sent *sent_find(const char *needle)
{
	unsigned int i, n = fake.sends < LOG_MAX ? fake.sends : LOG_MAX;

	for (i = n; i > 0; i--) {
		if (strstr(fake.sent[i - 1].text, needle))
			return &fake.sent[i - 1];
	}

	return NULL;
}

/**
 * sid_take() - the session id of the propose the legs sent
 * @out: receives it
 * @len: capacity of @out
 *
 * Return: true when a propose was sent.
 */
static bool sid_take(char *out, size_t len)
{
	static const char prefix[] =
		"<propose xmlns=\"urn:xmpp:jingle-message:0\" id=\"";
	struct sent *rec = sent_find("<propose ");
	const char *at, *end;

	if (!rec)
		return false;

	at = strstr(rec->text, prefix);
	if (!at)
		return false;
	at += sizeof(prefix) - 1;
	end = strchr(at, '"');
	if (!end)
		return false;

	snprintf(out, len, "%.*s", (int)(end - at), at);

	return true;
}

/* ---------------------------------------------------------------- cases */

/**
 * test_outbound() - a call this phone places, up to its end by the peer
 */
static void test_outbound(void)
{
	char sid[JINGLE_SID_MAX];
	char xml[2048];
	struct sent *rec;
	unsigned int slot;

	reset();
	phf.cand_script[0] = "a=candidate:1 1 UDP 2122317823 192.168.1.20 "
			     "50000 typ host";
	phf.cand_n = 1;

	CASE("a dial proposes to the bare jid of the callee");
	CHECK(xcall_slot_free(0, &slot) && slot == 0);
	CHECK(xcall_line_dial_ready(0));
	CHECK(xcall_dial(0, 0, "42"));
	CHECK(fake.sends == 1);
	rec = sent_find("<propose ");
	CHECK(rec != NULL);
	CHECK(rec && strstr(rec->text, "to=\"" PEER_BARE "\"") != NULL);
	CHECK(rec && strstr(rec->text, "media=\"audio\"") != NULL);
	CHECK(sid_take(sid, sizeof(sid)));
	CHECK(xcall_slot_stream(0, 0) == 0);
	CHECK(!xcall_slot_free(0, &slot) || slot == 1);

	CASE("a ringing from a device of the callee reports proceeding");
	snprintf(xml, sizeof(xml), "<message from='" PEER "' to='" SELF
		 "' type='chat'><ringing xmlns='" JMI_NS "' id='%s'/>"
		 "</message>", sid);
	CHECK(rx(xml));
	CHECK(cbl.proceeding == 1);

	CASE("a proceed opens the session toward that device");
	snprintf(xml, sizeof(xml), "<message from='" PEER "' to='" SELF
		 "' type='chat'><proceed xmlns='" JMI_NS "' id='%s'/>"
		 "</message>", sid);
	CHECK(rx(xml));
	CHECK(phf.n[PHF_MEDIA_CONFIG_SET] == 1);
	CHECK(phf.n[PHF_MEDIA_START] == 1);
	CHECK(phf.hop[0].media.transport.kind == HAL_TRANSPORT_ICE_DTLS);
	CHECK(phf.hop[0].media.transport.ice_role == HAL_ICE_CONTROLLING);
	CHECK(phf.hop[0].media.transport.dtls_setup == HAL_DTLS_ACTPASS);
	CHECK(!phf.hop[0].media.transport.remote_ufrag[0]);
	CHECK(!strcmp(phf.hop[0].media.transport.stun_host, "203.0.113.9"));
	CHECK(phf.hop[0].media.local.ipver == 4);
	CHECK(phf.hop[0].media.codec == HAL_CODEC_PCMU);
	rec = sent_find("action=\"session-initiate\"");
	CHECK(rec != NULL);
	CHECK(rec && rec->iq);
	CHECK(rec && strstr(rec->text, "to=\"" PEER "\"") != NULL);
	CHECK(rec && strstr(rec->text, "initiator=\"" SELF "\"") != NULL);
	CHECK(rec && strstr(rec->text, "<payload-type id=\"0\" name=\"PCMU\" "
					"clockrate=\"8000\"/>") != NULL);
	CHECK(rec && strstr(rec->text, "name=\"telephone-event\"") != NULL);
	CHECK(rec && strstr(rec->text, "ufrag=\"myufrag\"") != NULL);
	CHECK(rec && strstr(rec->text, "setup=\"actpass\">AB:AB:") != NULL);
	CHECK(rec && strstr(rec->text, "<candidate") == NULL);
	rec = sent_find("action=\"transport-info\"");
	CHECK(rec != NULL);
	CHECK(rec && strstr(rec->text, "ip=\"192.168.1.20\"") != NULL);
	CHECK(phf.n[PHF_MEDIA_CANDIDATE_NEXT] >= 2);

	CASE("a second ringing on the session reports proceeding again");
	snprintf(xml, sizeof(xml), "<iq from='" PEER "' to='" SELF
		 "' type='set' id='r1'><jingle xmlns='" JINGLE_NS
		 "' action='session-info' sid='%s'><ringing xmlns='"
		 JINGLE_NS_RTP_INFO "'/></jingle></iq>", sid);
	CHECK(rx(xml));
	CHECK(fake.results == 1);
	CHECK(cbl.proceeding == 2);

	CASE("the session-accept programs the far end and answers the call");
	snprintf(xml, sizeof(xml), "<iq from='" PEER "' to='" SELF
		 "' type='set' id='a1'><jingle xmlns='" JINGLE_NS
		 "' action='session-accept' sid='%s' responder='" PEER "'>"
		 "<content creator='initiator' name='audio'>" DESC_PEER
		 TRANSPORT_PEER("active") "</content></jingle></iq>", sid);
	CHECK(rx(xml));
	CHECK(fake.results == 2);
	CHECK(cbl.answered == 1);
	CHECK(phf.n[PHF_MEDIA_CONFIG_SET] == 2);
	CHECK(phf.n[PHF_MEDIA_START] == 1);
	CHECK(phf.hop[2].entry == PHF_MEDIA_CONFIG_SET);
	CHECK(!strcmp(phf.hop[2].media.transport.remote_ufrag, "8hhy"));
	CHECK(phf.hop[2].media.transport.dtls_setup == HAL_DTLS_PASSIVE);
	CHECK(phf.hop[2].media.transport.remote_fp[0] == 0x02);
	CHECK(phf.hop[2].media.codec == HAL_CODEC_PCMU);
	CHECK(phf.hop[2].media.dtmf_pt == 101);
	CHECK(phf.hop[3].entry == PHF_MEDIA_CANDIDATE_ADD);
	CHECK(phf.n[PHF_MEDIA_CANDIDATE_ADD] == 1);
	CHECK(strstr(phf.candidate, "10.0.1.1 8998 typ host") != NULL);

	CASE("a trickled candidate of the peer reaches the backend");
	snprintf(xml, sizeof(xml), "<iq from='" PEER "' type='set' id='t1'>"
		 "<jingle xmlns='" JINGLE_NS "' action='transport-info' "
		 "sid='%s'><content creator='initiator' name='audio'>"
		 "<transport xmlns='" JINGLE_NS_ICE "' ufrag='8hhy' "
		 "pwd='asd88fgpdd777uzjYhagZg'><candidate component='1' "
		 "foundation='2' generation='0' id='c2' ip='192.0.2.3' "
		 "network='1' port='45664' priority='1694498815' "
		 "protocol='udp' type='srflx'/></transport></content></jingle>"
		 "</iq>", sid);
	CHECK(rx(xml));
	CHECK(phf.n[PHF_MEDIA_CANDIDATE_ADD] == 2);
	CHECK(strstr(phf.candidate, "192.0.2.3 45664 typ srflx") != NULL);

	CASE("the media comes up, a digit goes out as a telephone event");
	xcall_transport_event(0, 0, HAL_TRANSPORT_EV_ICE_STATE,
			      HAL_ICE_CONNECTED);
	xcall_transport_event(0, 0, HAL_TRANSPORT_EV_DTLS_UP, 0);
	CHECK(fake.timers == 0);
	CHECK(xcall_dtmf(0, 0, '5', 100, false));
	CHECK(phf.n[PHF_MEDIA_DTMF_SEND] == 1);
	CHECK(phf.digit == '5');

	CASE("a hold reprograms the stream silent and tells the peer");
	CHECK(xcall_hold(0, 0));
	CHECK(phf.n[PHF_MEDIA_CONFIG_SET] == 3);
	CHECK(phf.hop[phf.nhop - 1].media.tx_silence);
	CHECK(sent_find("<hold xmlns=\"" JINGLE_NS_RTP_INFO "\"/>") != NULL);
	CHECK(cbl.reinvite_done == 1);
	CHECK(xcall_resume(0, 0));
	CHECK(!phf.hop[phf.nhop - 1].media.tx_silence);
	CHECK(cbl.reinvite_done == 2);

	CASE("a hold of the peer reports hold_changed");
	snprintf(xml, sizeof(xml), "<iq from='" PEER "' type='set' id='h1'>"
		 "<jingle xmlns='" JINGLE_NS "' action='session-info' sid='%s'>"
		 "<hold xmlns='" JINGLE_NS_RTP_INFO "'/></jingle></iq>", sid);
	CHECK(rx(xml));
	CHECK(cbl.hold_changed == 1 && cbl.held);

	CASE("the peer ends the session: remote_bye and a record");
	snprintf(xml, sizeof(xml), "<iq from='" PEER "' type='set' id='e1'>"
		 "<jingle xmlns='" JINGLE_NS "' action='session-terminate' "
		 "sid='%s'><reason><success/></reason></jingle></iq>", sid);
	CHECK(rx(xml));
	CHECK(cbl.remote_bye == 1);
	CHECK(cbl.ended == 1);
	CHECK(!cbl.ended_info.inbound);
	CHECK(cbl.ended_info.answered);
	CHECK(!cbl.ended_info.failed);
	CHECK(cbl.ended_info.media_ran);
	CHECK(cbl.ended_info.cleared);
	CHECK(cbl.ended_info.codec == HAL_CODEC_PCMU);
	CHECK(!strcmp(cbl.ended_number, PEER_BARE));
	CHECK(!strcmp(cbl.ended_remote, PEER_BARE));
	CHECK(phf.n[PHF_MEDIA_STOP] == 1);
	CHECK(phf.n[PHF_MEDIA_STATS_SNAPSHOT] == 1);
	CHECK(!xcalls_line_busy(0));
	CHECK(xcall_slot_stream(0, 0) == CALL_SLOTS);
}

/**
 * dtls_close_call() - bring one outbound call to flowing media
 * @sid: receives the session id
 * @len: capacity of @sid
 */
static void dtls_close_call(char *sid, size_t len)
{
	char xml[2048];

	CHECK(xcall_dial(0, 0, PEER_BARE));
	CHECK(sid_take(sid, len));
	snprintf(xml, sizeof(xml), "<message from='" PEER "' type='chat'>"
		 "<proceed xmlns='" JMI_NS "' id='%s'/></message>", sid);
	CHECK(rx(xml));
	snprintf(xml, sizeof(xml), "<iq from='" PEER "' type='set' id='a1'>"
		 "<jingle xmlns='" JINGLE_NS "' action='session-accept' "
		 "sid='%s'><content creator='initiator' name='audio'>" DESC_PEER
		 TRANSPORT_PEER("active") "</content></jingle></iq>", sid);
	CHECK(rx(xml));
	CHECK(cbl.answered == 1);
	xcall_transport_event(0, 0, HAL_TRANSPORT_EV_DTLS_UP, 0);
	CHECK(fake.timers == 0);
}

/**
 * test_dtls_close() - a close of the media by the peer is no failure
 */
static void test_dtls_close(void)
{
	char sid[JINGLE_SID_MAX];
	char xml[1024];

	reset();

	CASE("a close of the media waits for the session-terminate of the peer");
	dtls_close_call(sid, sizeof(sid));
	fake.sends = 0;
	xcall_transport_event(0, 0, HAL_TRANSPORT_EV_DTLS_CLOSED, 0);
	CHECK(fake.timers == 1);
	CHECK(fake.sends == 0);
	CHECK(cbl.remote_bye == 0);
	CHECK(xcalls_line_busy(0));
	snprintf(xml, sizeof(xml), "<iq from='" PEER "' type='set' id='e1'>"
		 "<jingle xmlns='" JINGLE_NS "' action='session-terminate' "
		 "sid='%s'><reason><success/></reason></jingle></iq>", sid);
	CHECK(rx(xml));
	CHECK(cbl.remote_bye == 1);
	CHECK(cbl.failed == 0);
	CHECK(cbl.ended == 1 && !cbl.ended_info.failed);
	CHECK(sent_find("<security-error/>") == NULL);
	CHECK(fake.timers == 0);
	CHECK(!xcalls_line_busy(0));

	reset();

	CASE("a close of the media without a session-terminate ends the call");
	dtls_close_call(sid, sizeof(sid));
	xcall_transport_event(0, 0, HAL_TRANSPORT_EV_DTLS_CLOSED, 0);
	xcall_transport_event(0, 0, HAL_TRANSPORT_EV_DTLS_CLOSED, 0);
	CHECK(fake.timers == 1);
	advance(XCALL_CLOSE_MS);
	CHECK(sent_find("<success/>") != NULL);
	CHECK(sent_find("<security-error/>") == NULL);
	CHECK(cbl.remote_bye == 1);
	CHECK(cbl.failed == 0);
	CHECK(cbl.ended == 1 && !cbl.ended_info.failed);
	CHECK(cbl.ended_info.cleared);
	CHECK(!xcalls_line_busy(0));
}

/**
 * test_outbound_refusals() - the ways an outbound attempt ends early
 */
static void test_outbound_refusals(void)
{
	char sid[JINGLE_SID_MAX];
	char xml[1024];
	struct sent *rec;

	reset();

	CASE("a target nothing maps reports unobtainable and places nothing");
	CHECK(!xcall_dial(0, 0, "99"));
	CHECK(cbl.failed == 1 && cbl.fail_class == CALL_FAIL_UNOBTAINABLE);
	CHECK(fake.sends == 0);
	CHECK(!xcall_dial(0, 0, "nolocal.example"));
	CHECK(cbl.failed == 2);

	CASE("a stream that is not ready reports congestion");
	fake.ready = false;
	CHECK(!xcall_dial(0, 0, PEER_BARE));
	CHECK(cbl.failed == 3 && cbl.fail_class == CALL_FAIL_CONGESTION);
	fake.ready = true;

	CASE("a reject by a device of the callee reports busy");
	CHECK(xcall_dial(0, 0, PEER_BARE));
	CHECK(sid_take(sid, sizeof(sid)));
	snprintf(xml, sizeof(xml), "<message from='" PEER "' type='chat'>"
		 "<reject xmlns='" JMI_NS "' id='%s'/></message>", sid);
	CHECK(rx(xml));
	CHECK(cbl.failed == 4 && cbl.fail_class == CALL_FAIL_BUSY);
	CHECK(cbl.ended == 1 && cbl.ended_info.failed);

	CASE("a propose nobody answers is retracted as unobtainable");
	CHECK(xcall_dial(0, 0, PEER_BARE));
	CHECK(fake.timers == 1);
	advance(XCALL_RING_MS);
	CHECK(sent_find("<retract ") != NULL);
	CHECK(cbl.failed == 5 && cbl.fail_class == CALL_FAIL_UNOBTAINABLE);

	CASE("a hangup while proposing retracts");
	CHECK(xcall_dial(0, 0, PEER_BARE));
	fake.sends = 0;
	CHECK(xcall_hangup(0, 0));
	rec = sent_find("<retract ");
	CHECK(rec != NULL);
	CHECK(rec && strstr(rec->text, "to=\"" PEER_BARE "\"") != NULL);
	CHECK(cbl.ended == 3 && cbl.ended_info.cleared);
	CHECK(!cbl.ended_info.failed);

	CASE("a peer that accepts without a fingerprint gets security-error");
	CHECK(xcall_dial(0, 0, PEER_BARE));
	CHECK(sid_take(sid, sizeof(sid)));
	snprintf(xml, sizeof(xml), "<message from='" PEER "' type='chat'>"
		 "<proceed xmlns='" JMI_NS "' id='%s'/></message>", sid);
	CHECK(rx(xml));
	snprintf(xml, sizeof(xml), "<iq from='" PEER "' type='set' id='a1'>"
		 "<jingle xmlns='" JINGLE_NS "' action='session-accept' "
		 "sid='%s'><content creator='initiator' name='audio'>" DESC_PEER
		 "<transport xmlns='" JINGLE_NS_ICE "' ufrag='a' pwd='b'/>"
		 "</content></jingle></iq>", sid);
	CHECK(rx(xml));
	CHECK(sent_find("<security-error/>") != NULL);
	CHECK(cbl.failed == 6 && cbl.fail_class == CALL_FAIL_CONGESTION);

	CASE("a peer with no common codec ends with failed-application");
	CHECK(xcall_dial(0, 0, PEER_BARE));
	CHECK(sid_take(sid, sizeof(sid)));
	snprintf(xml, sizeof(xml), "<message from='" PEER "' type='chat'>"
		 "<proceed xmlns='" JMI_NS "' id='%s'/></message>", sid);
	CHECK(rx(xml));
	snprintf(xml, sizeof(xml), "<iq from='" PEER "' type='set' id='a2'>"
		 "<jingle xmlns='" JINGLE_NS "' action='session-accept' "
		 "sid='%s'><content creator='initiator' name='audio'>"
		 "<description xmlns='" JINGLE_NS_RTP "' media='audio'>"
		 "<payload-type id='96' "
		 "name='speex' clockrate='16000'/></description>"
		 TRANSPORT_PEER("active") "</content></jingle></iq>", sid);
	CHECK(rx(xml));
	CHECK(sent_find("<failed-application/>") != NULL);
	CHECK(cbl.failed == 7);

	CASE("a session nobody accepts times out");
	CHECK(xcall_dial(0, 0, PEER_BARE));
	CHECK(sid_take(sid, sizeof(sid)));
	snprintf(xml, sizeof(xml), "<message from='" PEER "' type='chat'>"
		 "<proceed xmlns='" JMI_NS "' id='%s'/></message>", sid);
	CHECK(rx(xml));
	advance(XCALL_SETUP_MS);
	CHECK(sent_find("<timeout/>") != NULL);
	CHECK(cbl.failed == 8 && cbl.fail_class == CALL_FAIL_CONGESTION);

	CASE("a failed transport without a relay tells the user");
	CHECK(xcall_dial(0, 0, PEER_BARE));
	CHECK(sid_take(sid, sizeof(sid)));
	snprintf(xml, sizeof(xml), "<message from='" PEER "' type='chat'>"
		 "<proceed xmlns='" JMI_NS "' id='%s'/></message>", sid);
	CHECK(rx(xml));
	snprintf(xml, sizeof(xml), "<iq from='" PEER "' type='set' id='a3'>"
		 "<jingle xmlns='" JINGLE_NS "' action='session-accept' "
		 "sid='%s'><content creator='initiator' name='audio'>" DESC_PEER
		 TRANSPORT_PEER("active") "</content></jingle></iq>", sid);
	CHECK(rx(xml));
	CHECK(cbl.answered == 1);
	xcall_transport_event(0, 0, HAL_TRANSPORT_EV_ICE_STATE, HAL_ICE_FAILED);
	CHECK(fake.relay_missing == 1);
	CHECK(sent_find("<connectivity-error/>") != NULL);
	CHECK(cbl.failed == 9);

	CASE("a stanza for an unknown session gets item-not-found");
	CHECK(rx("<iq from='" PEER "' type='set' id='u1'><jingle xmlns='"
		 JINGLE_NS "' action='session-terminate' sid='nosuch'>"
		 "<reason><success/></reason></jingle></iq>"));
	CHECK(fake.errors >= 1);
	CHECK(!strcmp(fake.error_condition, "item-not-found"));

	CASE("a stanza that is no jingle and no jmi is not taken");
	CHECK(!rx("<iq from='" PEER "' type='set' id='q1'><query xmlns='x:y'/>"
		  "</iq>"));
	CHECK(!rx("<message from='" PEER "' type='chat'><body>hi</body>"
		  "</message>"));
}

/**
 * test_inbound() - a call another account places to this phone
 */
static void test_inbound(void)
{
	char xml[2048];
	struct sent *rec;

	reset();
	fake.turns = 1;
	phf.cand_script[0] = "a=candidate:1 1 UDP 2122317823 192.168.1.20 "
			     "50000 typ host";
	phf.cand_n = 1;

	CASE("a propose rings the shell with the roster name");
	CHECK(rx("<message from='" PEER "' to='" SELF_BARE "' type='chat'>"
		 "<propose xmlns='" JMI_NS "' id='s1'><description xmlns='"
		 JINGLE_NS_RTP "' media='audio'/></propose></message>"));
	CHECK(cbl.incoming == 1);
	CHECK(!strcmp(cbl.cid_number, PEER_BARE));
	CHECK(!strcmp(cbl.cid_name, "Romeo"));
	rec = sent_find("<ringing ");
	CHECK(rec != NULL);
	CHECK(rec && strstr(rec->text, "to=\"" PEER "\"") != NULL);
	CHECK(xcall_ring(0, 0));
	CHECK(xcall_slot_stream(0, 0) == 0);

	CASE("the same propose again changes nothing");
	CHECK(rx("<message from='" PEER "' to='" SELF_BARE "' type='chat'>"
		 "<propose xmlns='" JMI_NS "' id='s1'><description xmlns='"
		 JINGLE_NS_RTP "' media='audio'/></propose></message>"));
	CHECK(cbl.incoming == 1);

	CASE("the answer proceeds to the caller and accepts to the account");
	fake.sends = 0;
	CHECK(xcall_answer(0, 0));
	rec = sent_find("<proceed ");
	CHECK(rec != NULL);
	CHECK(rec && strstr(rec->text, "to=\"" PEER "\"") != NULL);
	rec = sent_find("<accept ");
	CHECK(rec != NULL);
	CHECK(rec && strstr(rec->text, "to=\"" SELF_BARE "\"") != NULL);
	CHECK(fake.timers == 1);

	CASE("the session-initiate is answered with a session-accept");
	fake.sends = 0;
	snprintf(xml, sizeof(xml), "<iq from='" PEER "' to='" SELF
		 "' type='set' id='i1'><jingle xmlns='" JINGLE_NS
		 "' action='session-initiate' sid='s1' initiator='" PEER "'>"
		 "<content creator='initiator' name='voice' senders='both'>"
		 DESC_PEER TRANSPORT_PEER("actpass") "</content></jingle>"
		 "</iq>");
	CHECK(rx(xml));
	CHECK(fake.results == 1);
	CHECK(phf.n[PHF_MEDIA_CONFIG_SET] == 1);
	CHECK(phf.n[PHF_MEDIA_START] == 1);
	CHECK(phf.hop[0].entry == PHF_MEDIA_CONFIG_SET);
	CHECK(phf.hop[0].media.transport.ice_role == HAL_ICE_CONTROLLED);
	CHECK(phf.hop[0].media.transport.dtls_setup == HAL_DTLS_ACTIVE);
	CHECK(!strcmp(phf.hop[0].media.transport.remote_ufrag, "8hhy"));
	CHECK(phf.hop[0].media.transport.turns == 1);
	CHECK(phf.hop[2].entry == PHF_MEDIA_CANDIDATE_ADD);
	CHECK(phf.n[PHF_MEDIA_CANDIDATE_ADD] == 1);
	rec = sent_find("action=\"session-accept\"");
	CHECK(rec != NULL);
	CHECK(rec && strstr(rec->text, "responder=\"" SELF "\"") != NULL);
	CHECK(rec && strstr(rec->text, "name=\"voice\"") != NULL);
	CHECK(rec && strstr(rec->text, "<payload-type id=\"0\" name=\"PCMU\" "
					"clockrate=\"8000\"/>") != NULL);
	CHECK(rec && strstr(rec->text, "name=\"G722\"") == NULL);
	CHECK(rec && strstr(rec->text, "setup=\"active\"") != NULL);
	CHECK(sent_find("action=\"transport-info\"") != NULL);
	CHECK(cbl.answered == 0);

	CASE("a transport that never comes up with a relay stays quiet");
	advance(XCALL_ICE_MS);
	CHECK(fake.relay_missing == 0);
	CHECK(sent_find("<connectivity-error/>") != NULL);
	CHECK(cbl.failed == 1);
	CHECK(cbl.ended == 1 && cbl.ended_info.inbound);
	CHECK(cbl.ended_info.answered);
}

/**
 * test_inbound_ends() - the ways an offered call ends without an answer
 */
static void test_inbound_ends(void)
{
	struct sent *rec;

	reset();

	CASE("a retract of the caller is a remote cancel");
	CHECK(rx("<message from='" PEER "' to='" SELF_BARE "' type='chat'>"
		 "<propose xmlns='" JMI_NS "' id='s2'><description xmlns='"
		 JINGLE_NS_RTP "' media='audio'/></propose></message>"));
	CHECK(rx("<message from='" PEER "' to='" SELF_BARE "' type='chat'>"
		 "<retract xmlns='" JMI_NS "' id='s2'/></message>"));
	CHECK(cbl.remote_cancel == 1);
	CHECK(cbl.ended == 1 && cbl.ended_info.cleared);

	CASE("an accept another device sent ends the ring, answered elsewhere");
	CHECK(rx("<message from='" PEER "' to='" SELF_BARE "' type='chat'>"
		 "<propose xmlns='" JMI_NS "' id='s3'><description xmlns='"
		 JINGLE_NS_RTP "' media='audio'/></propose></message>"));
	CHECK(cbl.incoming == 2);
	CHECK(rx("<message from='" SELF_BARE "' to='" SELF "' type='chat'>"
		 "<sent xmlns='urn:xmpp:carbons:2'><forwarded xmlns='"
		 XMPP_NS_FORWARD "'><message xmlns='jabber:client' from='"
		 SELF_BARE "/pc' to='" SELF_BARE "' type='chat'><accept xmlns='"
		 JMI_NS "' id='s3'/></message></forwarded></sent></message>"));
	CHECK(cbl.elsewhere == 1);
	CHECK(cbl.remote_cancel == 1);
	CHECK(cbl.ended == 2);

	CASE("a proceed of another device, seen as a received carbon, too");
	CHECK(rx("<message from='" PEER "' to='" SELF_BARE "' type='chat'>"
		 "<propose xmlns='" JMI_NS "' id='s4'><description xmlns='"
		 JINGLE_NS_RTP "' media='audio'/></propose></message>"));
	CHECK(rx("<message from='" SELF_BARE "' to='" SELF "' type='chat'>"
		 "<proceed xmlns='" JMI_NS "' id='s4'/></message>"));
	CHECK(cbl.elsewhere == 2);

	CASE("a reject by the user goes to the caller");
	CHECK(rx("<message from='" PEER "' to='" SELF_BARE "' type='chat'>"
		 "<propose xmlns='" JMI_NS "' id='s5'><description xmlns='"
		 JINGLE_NS_RTP "' media='audio'/></propose></message>"));
	fake.sends = 0;
	CHECK(xcall_reject(0, 0, 486));
	rec = sent_find("<reject ");
	CHECK(rec != NULL);
	CHECK(rec && strstr(rec->text, "to=\"" PEER "\"") != NULL);
	CHECK(cbl.ended == 4);

	CASE("an offer with both slots taken is rejected at once");
	CHECK(rx("<message from='" PEER "' to='" SELF_BARE "' type='chat'>"
		 "<propose xmlns='" JMI_NS "' id='s6'><description xmlns='"
		 JINGLE_NS_RTP "' media='audio'/></propose></message>"));
	CHECK(rx("<message from='" PEER "' to='" SELF_BARE "' type='chat'>"
		 "<propose xmlns='" JMI_NS "' id='s7'><description xmlns='"
		 JINGLE_NS_RTP "' media='audio'/></propose></message>"));
	CHECK(cbl.incoming == 6);
	fake.sends = 0;
	CHECK(rx("<message from='" PEER "' to='" SELF_BARE "' type='chat'>"
		 "<propose xmlns='" JMI_NS "' id='s8'><description xmlns='"
		 JINGLE_NS_RTP "' media='audio'/></propose></message>"));
	CHECK(cbl.incoming == 6);
	rec = sent_find("<reject ");
	CHECK(rec != NULL);
	CHECK(rec && strstr(rec->text, "id=\"s8\"") != NULL);

	CASE("an offer nobody takes ends on its guard");
	advance(XCALL_OFFER_MS);
	CHECK(cbl.remote_cancel == 3);
	CHECK(!xcalls_line_busy(0));

	CASE("a direct session-initiate rings without a propose");
	CHECK(rx("<iq from='" PEER "' to='" SELF "' type='set' id='d1'>"
		 "<jingle xmlns='" JINGLE_NS "' action='session-initiate' "
		 "sid='d1' initiator='" PEER "'><content creator='initiator' "
		 "name='audio'>" DESC_PEER TRANSPORT_PEER("actpass")
		 "</content></jingle></iq>"));
	CHECK(cbl.incoming == 7);
	CHECK(sent_find("<ringing xmlns=\"" JINGLE_NS_RTP_INFO "\"/>") != NULL);
	fake.sends = 0;
	CHECK(xcall_answer(0, 0));
	CHECK(sent_find("action=\"session-accept\"") != NULL);
	CHECK(phf.n[PHF_MEDIA_START] == 1);

	CASE("a stream that falls ends the call quietly");
	xcalls_stream_down(0);
	CHECK(cbl.failed == 1 && cbl.fail_class == CALL_FAIL_QUIET);
	CHECK(!xcalls_line_busy(0));
	CHECK(fake.timers == 0);
}

int main(void)
{
	memset(&timer_env, 0, sizeof(timer_env));
	timer_env.now_ms = fake_now;
	timer_env.timer_set = fake_timer_set;
	timer_env.timer_cancel = fake_timer_cancel;

	memset(&parser, 0, sizeof(parser));
	SUITE_CHECK(xml_stream_init(&parser, &parser_cb, NULL) == 0);

	test_outbound();
	test_outbound_refusals();
	test_dtls_close();
	test_inbound();
	test_inbound_ends();

	xcalls_done();
	xml_stream_done(&parser);

	return tu_summary("test_xmpp_call");
}
