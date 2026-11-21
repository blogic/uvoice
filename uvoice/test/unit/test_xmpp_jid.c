// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the JID helpers (src/proto/xmpp/jid.c) and the service
 * discovery module (src/proto/xmpp/disco.c). The known answer of the caps
 * hash is the published example of XEP-0115 section 5.2.
 */
#include <errno.h>
#include <string.h>

#include "proto/xmpp/disco.h"
#include "proto/xmpp/jid.h"
#include "test_util.h"

/**
 * test_jid_parse() - the three parts and the refusals
 */
static void test_jid_parse(void)
{
	struct jid jid;
	char text[JID_MAX];

	CASE("a full jid splits into its parts, the domain lower-cased");
	CHECK(jid_parse("Romeo@Montague.Example/orchard", &jid));
	CHECK(!strcmp(jid.local, "Romeo"));
	CHECK(!strcmp(jid.domain, "montague.example"));
	CHECK(!strcmp(jid.resource, "orchard"));
	jid_full(&jid, text, sizeof(text));
	CHECK(!strcmp(text, "Romeo@montague.example/orchard"));
	jid_bare(&jid, text, sizeof(text));
	CHECK(!strcmp(text, "Romeo@montague.example"));

	CASE("a bare jid and a bare domain");
	CHECK(jid_parse("juliet@capulet.example", &jid));
	CHECK(!jid.resource[0]);
	CHECK(jid_parse("capulet.example", &jid));
	CHECK(!jid.local[0]);
	jid_full(&jid, text, sizeof(text));
	CHECK(!strcmp(text, "capulet.example"));

	CASE("a resource may carry an at sign and a slash");
	CHECK(jid_parse("a@b.example/x@y/z", &jid));
	CHECK(!strcmp(jid.resource, "x@y/z"));

	CASE("the malformed shapes are refused");
	CHECK(!jid_parse("", &jid));
	CHECK(!jid_parse("@b.example", &jid));
	CHECK(!jid_parse("a@", &jid));
	CHECK(!jid_parse("a@b.example/", &jid));
	CHECK(!jid_parse("a b@c.example", &jid));
	CHECK(!jid_parse("a<b@c.example", &jid));
	CHECK(!jid_parse("a:b@c.example", &jid));
	CHECK(!jid_parse(NULL, &jid));

	CASE("two jids of one account compare equal whatever the resource");
	CHECK(jid_text_bare_eq("juliet@Capulet.example/phone",
			       "juliet@capulet.EXAMPLE/pc"));
	CHECK(!jid_text_bare_eq("juliet@capulet.example",
				"Juliet@capulet.example"));
	CHECK(!jid_text_bare_eq("juliet@capulet.example", "bad@"));
}

/**
 * test_disco_ver() - the XEP-0115 section 5.2 example and the own set
 */
static void test_disco_ver(void)
{
	static const struct disco_identity exodus = {
		"client", "pc", "", "Exodus 0.9.1"
	};
	static const char *const features[] = {
		"http://jabber.org/protocol/muc",
		"http://jabber.org/protocol/disco#items",
		"http://jabber.org/protocol/disco#info",
		"http://jabber.org/protocol/caps",
	};
	char ver[DISCO_VER_MAX];
	char mem[2048];
	struct xml_w writer;
	unsigned int count;

	CASE("the simple generation example of XEP-0115 section 5.2");
	CHECK(disco_ver_compute(&exodus, 1, features, 4, ver, sizeof(ver)) ==
	      0);
	CHECK(!strcmp(ver, "QgayPKawpkPSDYmwT/WM94uAlu0="));

	CASE("the own set hashes and names the call features");
	CHECK(disco_ver(ver, sizeof(ver)) == 0);
	CHECK(strlen(ver) == 28);
	disco_features(&count);
	CHECK(count >= 9);

	CASE("a short output buffer is refused");
	CHECK(disco_ver(ver, 8) == -E2BIG);

	CASE("the info answer and the caps element serialise");
	xml_w_init(&writer, mem, sizeof(mem));
	xml_w_start(&writer, "iq");
	disco_info_write(&writer, DISCO_NODE);
	xml_w_end(&writer);
	CHECK(xml_w_finish(&writer) > 0);
	CHECK(strstr(mem, "<query xmlns=\"" DISCO_NS_INFO "\" node=\""
		     DISCO_NODE "\">") != NULL);
	CHECK(strstr(mem, "<identity category=\"client\" type=\"phone\" "
		     "name=\"uvoice\"/>") != NULL);
	CHECK(strstr(mem, "<feature var=\"urn:xmpp:jingle-message:0\"/>") !=
	      NULL);
	CHECK(strstr(mem, "<feature var=\"urn:xmpp:jingle:apps:dtls:0\"/>") !=
	      NULL);

	xml_w_init(&writer, mem, sizeof(mem));
	xml_w_start(&writer, "presence");
	disco_caps_write(&writer);
	xml_w_end(&writer);
	CHECK(xml_w_finish(&writer) > 0);
	CHECK(strstr(mem, "<c xmlns=\"" DISCO_NS_CAPS "\" hash=\"sha-1\" "
		     "node=\"" DISCO_NODE "\" ver=\"") != NULL);
}

int main(void)
{
	test_jid_parse();
	test_disco_ver();

	return tu_summary("test_xmpp_jid");
}
