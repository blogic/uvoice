// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the Jingle codec (src/proto/xmpp/jingle.c) and Jingle
 * Message Initiation (src/proto/xmpp/jmi.c). Every stanza under test runs
 * through the real parser of xml.c, so the two layers are tested the way
 * the stream hands them a tree.
 */
#include <string.h>

#include "proto/xmpp/jingle.h"
#include "proto/xmpp/jmi.h"
#include "test_util.h"

#define STREAM_OPEN							\
	"<stream:stream xmlns='jabber:client' "				\
	"xmlns:stream='http://etherx.jabber.org/streams'>"

#define FP_TEXT								\
	"02:1A:CC:54:27:AB:EB:9C:53:3F:3E:4A:65:2E:58:AD:"		\
	"62:61:2F:B4:F7:D1:E8:C0:8C:5E:FF:F4:FB:F6:76:F2"

#define INITIATE							\
	"<iq from='romeo@montague.example/orchard' "			\
	"to='user@sip.example.test/uvoice' type='set' id='i1'>"	\
	"<jingle xmlns='urn:xmpp:jingle:1' action='session-initiate' "	\
	"initiator='romeo@montague.example/orchard' sid='a73sjjvkla37jfea'>"\
	"<content creator='initiator' name='voice' senders='both'>"	\
	"<description xmlns='urn:xmpp:jingle:apps:rtp:1' media='audio'>"\
	"<payload-type id='9' name='G722' clockrate='8000'/>"		\
	"<payload-type id='0' name='PCMU' clockrate='8000'/>"		\
	"<payload-type id='101' name='telephone-event' clockrate='8000'/>"\
	"<rtcp-mux/>"							\
	"</description>"						\
	"<transport xmlns='urn:xmpp:jingle:transports:ice-udp:1' "	\
	"pwd='asd88fgpdd777uzjYhagZg' ufrag='8hhy'>"			\
	"<fingerprint xmlns='urn:xmpp:jingle:apps:dtls:0' hash='sha-256' "\
	"setup='actpass'>" FP_TEXT "</fingerprint>"			\
	"<candidate component='1' foundation='1' generation='0' "	\
	"id='el0747fg11' ip='10.0.1.1' network='1' port='8998' "	\
	"priority='2130706431' protocol='udp' type='host'/>"		\
	"<candidate component='1' foundation='2' generation='0' "	\
	"id='y3s2b30v3r' ip='192.0.2.3' network='1' port='45664' "	\
	"priority='1694498815' protocol='udp' rel-addr='10.0.1.1' "	\
	"rel-port='8998' type='srflx'/>"				\
	"</transport></content></jingle></iq>"

#define IQ_SET_OPEN							\
	"<iq type='set' id='x'><jingle xmlns='urn:xmpp:jingle:1' "
#define CAND_HOST							\
	"a=candidate:1 1 UDP 2122317823 192.168.1.2 50000 typ host"
#define CAND_HOST_JINGLE						\
	"a=candidate:1 1 udp 2122317823 192.168.1.2 50000 typ host "	\
	"generation 0"

#define TERMINATE							\
	"<iq from='romeo@montague.example/orchard' type='set' id='t1'>"	\
	"<jingle xmlns='urn:xmpp:jingle:1' action='session-terminate' "	\
	"sid='a73sjjvkla37jfea'><reason><busy/><text>on the phone</text>"\
	"</reason></jingle></iq>"

#define INFO								\
	"<iq from='romeo@montague.example/orchard' type='set' id='n1'>"	\
	"<jingle xmlns='urn:xmpp:jingle:1' action='session-info' "	\
	"sid='a73sjjvkla37jfea'>"					\
	"<ringing xmlns='urn:xmpp:jingle:apps:rtp:info:1'/></jingle></iq>"

#define PROPOSE								\
	"<message from='romeo@montague.example/orchard' "		\
	"to='user@sip.example.test' type='chat' id='m1'>"		\
	"<propose xmlns='urn:xmpp:jingle-message:0' id='a73sjjvkla37jfea'>"\
	"<description xmlns='urn:xmpp:jingle:apps:rtp:1' media='audio'/>"\
	"</propose><store xmlns='urn:xmpp:hints'/></message>"

#define CARBON_ACCEPT							\
	"<message from='user@sip.example.test' "			\
	"to='user@sip.example.test/uvoice' type='chat'>"		\
	"<sent xmlns='urn:xmpp:carbons:2'>"				\
	"<forwarded xmlns='urn:xmpp:forward:0'>"			\
	"<message xmlns='jabber:client' from='user@sip.example.test/pc' "\
	"to='user@sip.example.test' type='chat'>"			\
	"<accept xmlns='urn:xmpp:jingle-message:0' id='a73sjjvkla37jfea'/>"\
	"</message></forwarded></sent></message>"

#define CARBON_PROCEED							\
	"<message from='user@sip.example.test' "			\
	"to='user@sip.example.test/uvoice' type='chat'>"		\
	"<received xmlns='urn:xmpp:carbons:2'>"				\
	"<forwarded xmlns='urn:xmpp:forward:0'>"			\
	"<message xmlns='jabber:client' from='romeo@montague.example/orchard' "\
	"to='user@sip.example.test/pc' type='chat'>"			\
	"<proceed xmlns='urn:xmpp:jingle-message:0' id='a73sjjvkla37jfea'/>"\
	"</message></forwarded></received></message>"

static struct jingle_msg jm;
static struct jmi_msg mm;
static bool jm_ok;
static bool mm_ok;
static unsigned int stanzas;

/**
 * on_stanza() - parse the stanza both ways
 * @user: unused
 * @doc: the tree
 */
static void on_stanza(void *user, const struct xml_doc *doc)
{
	stanzas++;
	jm_ok = jingle_parse(doc, &jm);
	mm_ok = jmi_parse(doc, &mm);
}

/**
 * on_error() - a parse error, which no case expects
 * @user: unused
 * @reason: the text
 */
static void on_error(void *user, const char *reason)
{
	printf("    xml error: %s\n", reason);
}

static const struct xml_stream_cb cb = {
	.stanza = on_stanza,
	.error = on_error,
};

/**
 * parse() - run one stanza through the parser
 * @xml: the stanza
 *
 * Return: true when exactly one stanza came out.
 */
static bool parse(const char *xml)
{
	struct xml_stream xs;

	memset(&xs, 0, sizeof(xs));
	if (xml_stream_init(&xs, &cb, NULL))
		return false;

	stanzas = 0;
	jm_ok = false;
	mm_ok = false;
	xml_stream_feed(&xs, STREAM_OPEN, strlen(STREAM_OPEN));
	xml_stream_feed(&xs, xml, strlen(xml));
	xml_stream_done(&xs);

	return stanzas == 1;
}

/**
 * test_parse_initiate() - a session-initiate as Conversations sends one
 */
static void test_parse_initiate(void)
{
	const struct jingle_content *content = &jm.content;

	CASE("a session-initiate yields the content, the transport and the "
	     "fingerprint");
	CHECK(parse(INITIATE));
	CHECK(jm_ok);
	CHECK(!mm_ok);
	CHECK(jm.action == JINGLE_SESSION_INITIATE);
	CHECK(!strcmp(jm.sid, "a73sjjvkla37jfea"));
	CHECK(!strcmp(jm.initiator, "romeo@montague.example/orchard"));
	CHECK(!jm.responder[0]);
	CHECK(jm.contents == 1);
	CHECK(content->description);
	CHECK(content->audio);
	CHECK(content->rtcp_mux);
	CHECK(!strcmp(content->name, "voice"));
	CHECK(!strcmp(content->creator, "initiator"));
	CHECK(!strcmp(content->senders, "both"));
	CHECK(content->payloads == 3);
	CHECK(content->payload[0].id == 9);
	CHECK(!strcmp(content->payload[0].name, "G722"));
	CHECK(content->payload[1].id == 0);
	CHECK(!strcmp(content->payload[1].name, "PCMU"));
	CHECK(content->payload[1].clockrate == 8000);
	CHECK(content->payload[2].id == 101);
	CHECK(!strcmp(content->payload[2].name, "telephone-event"));
	CHECK(content->transport);
	CHECK(!strcmp(content->ice.ufrag, "8hhy"));
	CHECK(!strcmp(content->ice.pwd, "asd88fgpdd777uzjYhagZg"));
	CHECK(content->ice.fp_present);
	CHECK(content->ice.fp[0] == 0x02);
	CHECK(content->ice.fp[1] == 0x1a);
	CHECK(content->ice.fp[31] == 0xf2);
	CHECK(content->ice.setup == JINGLE_SETUP_ACTPASS);
	CHECK(content->ice.candidates == 2);
	CHECK(!strcmp(content->ice.candidate[0].sdp,
		      "a=candidate:1 1 udp 2130706431 10.0.1.1 8998 typ host "
		      "generation 0"));
	CHECK(!strcmp(content->ice.candidate[1].sdp,
		      "a=candidate:2 1 udp 1694498815 192.0.2.3 45664 typ "
		      "srflx raddr 10.0.1.1 rport 8998 generation 0"));
	CHECK(jm.reason == JINGLE_REASON_NONE);
	CHECK(jm.info == JINGLE_INFO_NONE);

	CASE("a session-terminate yields its reason and text");
	CHECK(parse(TERMINATE));
	CHECK(jm_ok);
	CHECK(jm.action == JINGLE_SESSION_TERMINATE);
	CHECK(jm.reason == JINGLE_REASON_BUSY);
	CHECK(!strcmp(jm.reason_text, "on the phone"));
	CHECK(jm.contents == 0);

	CASE("a session-info yields its payload");
	CHECK(parse(INFO));
	CHECK(jm_ok);
	CHECK(jm.action == JINGLE_SESSION_INFO);
	CHECK(jm.info == JINGLE_INFO_RINGING);

	CASE("an iq that is no jingle, or no set, is refused");
	CHECK(parse("<iq type='result' id='x'>"
		    "<jingle xmlns='urn:xmpp:jingle:1' action='session-info' "
		    "sid='s'/></iq>"));
	CHECK(!jm_ok);
	CHECK(parse("<iq type='set' id='x'><query xmlns='x:y'/></iq>"));
	CHECK(!jm_ok);
	CHECK(parse(IQ_SET_OPEN "action='session-dance' sid='s'/></iq>"));
	CHECK(!jm_ok);
	CHECK(parse(IQ_SET_OPEN "action='session-info'/></iq>"));
	CHECK(!jm_ok);
}

/**
 * test_candidates() - the two directions of the candidate conversion
 */
static void test_candidates(void)
{
	char mem[1024];
	struct xml_w writer;

	CASE("an a=candidate line becomes a candidate element");
	xml_w_init(&writer, mem, sizeof(mem));
	xml_w_start(&writer, "transport");
	CHECK(jingle_candidate_write(&writer, CAND_HOST));
	CHECK(jingle_candidate_write(&writer,
				     "candidate:2 1 UDP 1686052607 203.0.113.5 "
				     "51000 typ srflx raddr 192.168.1.2 rport "
				     "50000 generation 1"));
	CHECK(!jingle_candidate_write(&writer, "a=rtpmap:0 PCMU/8000"));
	CHECK(!jingle_candidate_write(&writer, "a=candidate:1 1 UDP"));
	xml_w_end(&writer);
	CHECK(xml_w_finish(&writer) > 0);
	CHECK(strstr(mem, "<candidate component=\"1\" foundation=\"1\" "
		     "generation=\"0\" id=\"11\" ip=\"192.168.1.2\" "
		     "network=\"0\" port=\"50000\" priority=\"2122317823\" "
		     "protocol=\"udp\" type=\"host\"/>") != NULL);
	CHECK(strstr(mem, "<candidate component=\"1\" foundation=\"2\" "
		     "generation=\"1\" id=\"21\" ip=\"203.0.113.5\" "
		     "network=\"0\" port=\"51000\" priority=\"1686052607\" "
		     "protocol=\"udp\" type=\"srflx\" rel-addr=\"192.168.1.2\" "
		     "rel-port=\"50000\"/>") != NULL);

	CASE("a candidate element without its port is dropped");
	CHECK(parse(IQ_SET_OPEN "action='transport-info' sid='s'>"
		    "<content creator='initiator' name='voice'><transport "
		    "xmlns='urn:xmpp:jingle:transports:ice-udp:1' ufrag='a' "
		    "pwd='b'><candidate component='1' foundation='1' "
		    "ip='1.2.3.4' priority='1' protocol='udp' type='host'/>"
		    "<candidate component='1' foundation='3' ip='1.2.3.4' "
		    "port='7' priority='1' protocol='udp' type='host'/>"
		    "</transport></content></jingle></iq>"));
	CHECK(jm_ok);
	CHECK(jm.action == JINGLE_TRANSPORT_INFO);
	CHECK(jm.content.transport);
	CHECK(!jm.content.description);
	CHECK(jm.content.ice.candidates == 1);
	CHECK(!strcmp(jm.content.ice.candidate[0].sdp,
		      "a=candidate:3 1 udp 1 1.2.3.4 7 typ host"));
}

/**
 * test_fingerprint() - the hex text both ways
 */
static void test_fingerprint(void)
{
	uint8_t fp[JINGLE_FP_LEN];
	char text[3 * JINGLE_FP_LEN];

	CASE("the fingerprint text parses and prints back");
	CHECK(jingle_fingerprint_parse(FP_TEXT, fp));
	CHECK(fp[0] == 0x02 && fp[2] == 0xcc && fp[31] == 0xf2);
	jingle_fingerprint_text(fp, text, sizeof(text));
	CHECK(!strcmp(text, FP_TEXT));
	CHECK(jingle_fingerprint_parse(" " FP_TEXT " ", fp));

	CASE("a short, a long and a corrupt fingerprint are refused");
	CHECK(!jingle_fingerprint_parse("02:1A:CC", fp));
	CHECK(!jingle_fingerprint_parse(FP_TEXT ":00", fp));
	CHECK(!jingle_fingerprint_parse("0Z:1A:CC:54:27:AB:EB:9C:53:3F:3E:4A:"
				       "65:2E:58:AD:62:61:2F:B4:F7:D1:E8:C0:"
				       "8C:5E:FF:F4:FB:F6:76:F2", fp));
}

/**
 * test_write_round_trip() - what the writer builds parses back
 */
static void test_write_round_trip(void)
{
	struct jingle_local local = {
		.content_name = "audio",
		.creator = "initiator",
		.payloads = 2,
		.payload = {
			{ .id = 0, .name = "PCMU", .clockrate = 8000 },
			{ .id = 101, .name = "telephone-event",
			  .clockrate = 8000 },
		},
		.rtcp_mux = true,
		.ufrag = "8hhy",
		.pwd = "asd88fgpdd777uzjYhagZg",
		.fp_present = true,
		.setup = JINGLE_SETUP_ACTIVE,
	};
	char mem[JINGLE_STANZA_MAX];
	struct xml_w writer;

	CHECK(jingle_fingerprint_parse(FP_TEXT, local.fp));

	CASE("a session-accept with a description, transport and candidate");
	xml_w_init(&writer, mem, sizeof(mem));
	jingle_open(&writer, "romeo@montague.example/orchard", "a1",
		    JINGLE_SESSION_ACCEPT, "a73sjjvkla37jfea", NULL,
		    "user@sip.example.test/uvoice");
	jingle_content_open(&writer, &local);
	jingle_description_write(&writer, &local);
	jingle_transport_open(&writer, &local, true);
	CHECK(jingle_candidate_write(&writer, CAND_HOST));
	jingle_transport_close(&writer);
	jingle_content_close(&writer);
	jingle_close(&writer);
	CHECK(xml_w_finish(&writer) > 0);
	CHECK(strstr(mem, "<iq type=\"set\" "
		     "to=\"romeo@montague.example/orchard\" id=\"a1\">"
		     "<jingle xmlns=\"urn:xmpp:jingle:1\" "
		     "action=\"session-accept\" sid=\"a73sjjvkla37jfea\" "
		     "responder=\"user@sip.example.test/uvoice\">") != NULL);
	CHECK(strstr(mem, "<payload-type id=\"0\" name=\"PCMU\" "
		     "clockrate=\"8000\"/>") != NULL);
	CHECK(strstr(mem, "<rtcp-mux/>") != NULL);
	CHECK(strstr(mem, "<fingerprint xmlns=\"urn:xmpp:jingle:apps:dtls:0\" "
		     "hash=\"sha-256\" setup=\"active\">" FP_TEXT
		     "</fingerprint>") != NULL);

	CHECK(parse(mem));
	CHECK(jm_ok);
	CHECK(jm.action == JINGLE_SESSION_ACCEPT);
	CHECK(!strcmp(jm.responder, "user@sip.example.test/uvoice"));
	CHECK(jm.content.payloads == 2);
	CHECK(jm.content.rtcp_mux);
	CHECK(jm.content.ice.fp_present);
	CHECK(jm.content.ice.setup == JINGLE_SETUP_ACTIVE);
	CHECK(!memcmp(jm.content.ice.fp, local.fp, JINGLE_FP_LEN));
	CHECK(jm.content.ice.candidates == 1);
	CHECK(!strcmp(jm.content.ice.candidate[0].sdp, CAND_HOST_JINGLE));

	CASE("a session-terminate with a reason and a session-info");
	xml_w_init(&writer, mem, sizeof(mem));
	jingle_open(&writer, "romeo@montague.example/orchard", "t2",
		    JINGLE_SESSION_TERMINATE, "a73sjjvkla37jfea", NULL, NULL);
	jingle_reason_write(&writer, JINGLE_REASON_SUCCESS, "bye");
	jingle_close(&writer);
	CHECK(xml_w_finish(&writer) > 0);
	CHECK(parse(mem));
	CHECK(jm_ok);
	CHECK(jm.reason == JINGLE_REASON_SUCCESS);
	CHECK(!strcmp(jm.reason_text, "bye"));

	xml_w_init(&writer, mem, sizeof(mem));
	jingle_open(&writer, "romeo@montague.example/orchard", "n2",
		    JINGLE_SESSION_INFO, "a73sjjvkla37jfea", NULL, NULL);
	jingle_info_write(&writer, JINGLE_INFO_HOLD);
	jingle_close(&writer);
	CHECK(xml_w_finish(&writer) > 0);
	CHECK(parse(mem));
	CHECK(jm_ok);
	CHECK(jm.info == JINGLE_INFO_HOLD);

	CASE("the words of the tables");
	CHECK(!strcmp(jingle_action_name(JINGLE_TRANSPORT_INFO),
		      "transport-info"));
	CHECK(jingle_action_of("content-add") == JINGLE_CONTENT_ADD);
	CHECK(jingle_action_of(NULL) == JINGLE_ACTION_NONE);
	CHECK(!strcmp(jingle_reason_name(JINGLE_REASON_CONNECTIVITY_ERROR),
		      "connectivity-error"));
	CHECK(!strcmp(jingle_info_name(JINGLE_INFO_UNHOLD), "unhold"));
	CHECK(jingle_setup_of("passive") == JINGLE_SETUP_PASSIVE);
	CHECK(!strcmp(jingle_setup_name(JINGLE_SETUP_NONE), ""));
}

/**
 * test_jmi() - the messages of XEP-0353, plain and as carbons
 */
static void test_jmi(void)
{
	char mem[JMI_STANZA_MAX];
	struct xml_w writer;

	CASE("a propose names the caller, the id and the audio description");
	CHECK(parse(PROPOSE));
	CHECK(mm_ok);
	CHECK(!jm_ok);
	CHECK(mm.kind == JMI_PROPOSE);
	CHECK(!strcmp(mm.id, "a73sjjvkla37jfea"));
	CHECK(!strcmp(mm.from, "romeo@montague.example/orchard"));
	CHECK(!strcmp(mm.to, "user@sip.example.test"));
	CHECK(mm.audio);
	CHECK(!mm.carbon);

	CASE("an accept another device sent arrives as a sent carbon");
	CHECK(parse(CARBON_ACCEPT));
	CHECK(mm_ok);
	CHECK(mm.kind == JMI_ACCEPT);
	CHECK(mm.carbon);
	CHECK(mm.carbon_sent);
	CHECK(!strcmp(mm.from, "user@sip.example.test/pc"));
	CHECK(!strcmp(mm.id, "a73sjjvkla37jfea"));

	CASE("a proceed another device received arrives as a received carbon");
	CHECK(parse(CARBON_PROCEED));
	CHECK(mm_ok);
	CHECK(mm.kind == JMI_PROCEED);
	CHECK(mm.carbon);
	CHECK(!mm.carbon_sent);
	CHECK(!strcmp(mm.from, "romeo@montague.example/orchard"));
	CHECK(!strcmp(mm.to, "user@sip.example.test/pc"));

	CASE("a message outside the family or without an id is refused");
	CHECK(parse("<message from='a@b' type='chat'><body>hi</body>"
		    "</message>"));
	CHECK(!mm_ok);
	CHECK(parse("<message from='a@b' type='chat'>"
		    "<propose xmlns='urn:xmpp:jingle-message:0'/></message>"));
	CHECK(!mm_ok);
	CHECK(parse("<message from='a@b' type='chat'>"
		    "<sent xmlns='urn:xmpp:carbons:2'><body>hi</body></sent>"
		    "</message>"));
	CHECK(!mm_ok);

	CASE("the writer builds a propose with its description and the hint");
	xml_w_init(&writer, mem, sizeof(mem));
	jmi_write(&writer, JMI_PROPOSE, "juliet@capulet.example", "m7",
		  "a73sjjvkla37jfea");
	CHECK(xml_w_finish(&writer) > 0);
	CHECK(!strcmp(mem, "<message to=\"juliet@capulet.example\" "
			   "type=\"chat\" id=\"m7\">"
			   "<propose xmlns=\"urn:xmpp:jingle-message:0\" "
			   "id=\"a73sjjvkla37jfea\"><description "
			   "xmlns=\"urn:xmpp:jingle:apps:rtp:1\" "
			   "media=\"audio\"/></propose>"
			   "<store xmlns=\"urn:xmpp:hints\"/></message>"));
	CHECK(parse(mem));
	CHECK(mm_ok);
	CHECK(mm.kind == JMI_PROPOSE);
	CHECK(mm.audio);

	CASE("the writer builds a proceed without a description");
	xml_w_init(&writer, mem, sizeof(mem));
	jmi_write(&writer, JMI_PROCEED, "juliet@capulet.example/pc", "m8",
		  "a73sjjvkla37jfea");
	CHECK(xml_w_finish(&writer) > 0);
	CHECK(strstr(mem, "<proceed xmlns=\"urn:xmpp:jingle-message:0\" "
		     "id=\"a73sjjvkla37jfea\"/>") != NULL);
	CHECK(!strcmp(jmi_kind_name(JMI_FINISH), "finish"));
	CHECK(!strcmp(jmi_kind_name(JMI_NONE), ""));
}

int main(void)
{
	test_parse_initiate();
	test_candidates();
	test_fingerprint();
	test_write_round_trip();
	test_jmi();

	return tu_summary("test_xmpp_jingle");
}
