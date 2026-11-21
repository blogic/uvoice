// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <stddef.h>

#include "proto/xmpp/xml.h"
#include "test_util.h"

#define NS_SASL		"urn:ietf:params:xml:ns:xmpp-sasl"
#define NS_TLS		"urn:ietf:params:xml:ns:xmpp-tls"
#define NS_DISCO_INFO	"http://jabber.org/protocol/disco#info"
#define NS_JMI		"urn:xmpp:jingle-message:0"
#define NS_RTP		"urn:xmpp:jingle:apps:rtp:1"
#define NS_HINTS	"urn:xmpp:hints"

#define T_HEADER \
	"<?xml version='1.0'?>" \
	"<stream:stream xmlns='jabber:client'" \
	" xmlns:stream='http://etherx.jabber.org/streams'" \
	" id='c2s-1' from='example.com' version='1.0' xml:lang='en'>"

#define T_WRITTEN \
	"<message to=\"a@b\" type=\"chat\">" \
	"<propose xmlns=\"urn:xmpp:jingle-message:0\" id=\"1\">" \
	"<description xmlns=\"urn:xmpp:jingle:apps:rtp:1\" media=\"audio\"/>" \
	"</propose><store xmlns=\"urn:xmpp:hints\"/></message>"

#define T_SLICE		1024U
#define T_CDATA_EMPTY	"<![CDATA[]]>"
#define T_BIG_MAX	(XML_STANZA_MAX * 2)
#define T_NS_URI_LONG	120U
#define T_ATTRS_SLICED	1200U
#define T_WRITER_MAX	512U

struct tctx {
	struct xml_doc header;
	struct xml_doc stanza;
	unsigned int opens;
	unsigned int stanzas;
	unsigned int closes;
	unsigned int errors;
	const char *reason;
};

static char t_big[T_BIG_MAX];

/**
 * doc_copy() - copy a tree and point its strings into the copy
 * @dst: the copy
 * @src: the tree of the parser
 */
static void doc_copy(struct xml_doc *dst, const struct xml_doc *src)
{
	ptrdiff_t shift;
	unsigned int i;

	*dst = *src;
	shift = dst->text - src->text;
	for (i = 0; i < dst->nodes; i++) {
		dst->node[i].name += shift;
		dst->node[i].ns += shift;
		dst->node[i].text += shift;
	}
	for (i = 0; i < dst->attrs; i++) {
		dst->attr[i].name += shift;
		dst->attr[i].value += shift;
		if (dst->attr[i].ns)
			dst->attr[i].ns += shift;
	}
}

/**
 * on_open() - record the stream header
 * @user: the test context
 * @header: the header tree
 */
static void on_open(void *user, const struct xml_doc *header)
{
	struct tctx *ctx = user;

	ctx->opens++;
	doc_copy(&ctx->header, header);
}

/**
 * on_stanza() - record a stanza
 * @user: the test context
 * @stanza: the stanza tree
 */
static void on_stanza(void *user, const struct xml_doc *stanza)
{
	struct tctx *ctx = user;

	ctx->stanzas++;
	doc_copy(&ctx->stanza, stanza);
}

/**
 * on_close() - record the stream close
 * @user: the test context
 */
static void on_close(void *user)
{
	struct tctx *ctx = user;

	ctx->closes++;
}

/**
 * on_error() - record a stream error
 * @user: the test context
 * @reason: the static reason
 */
static void on_error(void *user, const char *reason)
{
	struct tctx *ctx = user;

	ctx->errors++;
	ctx->reason = reason;
}

static const struct xml_stream_cb t_cb = {
	.stream_open = on_open,
	.stanza = on_stanza,
	.stream_close = on_close,
	.error = on_error,
};

/**
 * stream_open() - make a stream over a fresh context and feed the header
 * @xs: the stream, zeroed here
 * @ctx: the context, zeroed here
 *
 * Return: true when the header arrived without an error.
 */
static bool stream_open(struct xml_stream *xs, struct tctx *ctx)
{
	memset(xs, 0, sizeof(*xs));
	memset(ctx, 0, sizeof(*ctx));
	if (xml_stream_init(xs, &t_cb, ctx))
		return false;
	if (xml_stream_feed(xs, T_HEADER, strlen(T_HEADER)))
		return false;

	return ctx->opens == 1 && !ctx->errors;
}

/**
 * feed_str() - feed a string in one piece
 * @xs: the stream
 * @text: the string
 *
 * Return: the feed result.
 */
static int feed_str(struct xml_stream *xs, const char *text)
{
	return xml_stream_feed(xs, text, strlen(text));
}

/**
 * feed_sliced() - feed a string in slices the size of a socket read
 * @xs: the stream
 * @text: the string
 * @len: its length
 *
 * Return: the first failing feed result, or 0.
 */
static int feed_sliced(struct xml_stream *xs, const char *text, size_t len)
{
	size_t slice;
	int ret;

	while (len) {
		slice = len > T_SLICE ? T_SLICE : len;
		ret = xml_stream_feed(xs, text, slice);
		if (ret)
			return ret;
		text += slice;
		len -= slice;
	}

	return 0;
}

/**
 * attr_ns_get() - the value of a prefixed attribute of a node
 * @doc: the tree
 * @node: the node
 * @name: the local name
 * @ns: the namespace URI
 *
 * Return: the value, or NULL.
 */
static const char *attr_ns_get(const struct xml_doc *doc, unsigned int node,
			       const char *name, const char *ns)
{
	const struct xml_attr *attr;
	unsigned int i;

	for (i = 0; i < doc->node[node].attr_count; i++) {
		attr = &doc->attr[doc->node[node].attr_first + i];
		if (!attr->ns || strcmp(attr->ns, ns))
			continue;
		if (!strcmp(attr->name, name))
			return attr->value;
	}

	return NULL;
}

/**
 * str_is() - compare a possibly NULL string with a literal
 * @got: the string
 * @want: the literal
 *
 * Return: true when both are equal.
 */
static bool str_is(const char *got, const char *want)
{
	return got && !strcmp(got, want);
}

/**
 * big_stanza_make() - build a stanza of empty CDATA sections of a wire size
 * @wire: the wire length wanted, rounded down to whole sections
 *
 * Return: the length of t_big.
 */
static size_t big_stanza_make(size_t wire)
{
	const char *head = "<message><body>";
	const char *tail = "</body></message>";
	size_t len = strlen(head);
	size_t section = strlen(T_CDATA_EMPTY);

	memcpy(t_big, head, len);
	while (len + section + strlen(tail) < wire) {
		memcpy(t_big + len, T_CDATA_EMPTY, section);
		len += section;
	}
	memcpy(t_big + len, tail, strlen(tail));
	len += strlen(tail);
	t_big[len] = 0;

	return len;
}

/**
 * nested_make() - build a stanza nested to a depth
 * @depth: the number of nested elements, the stanza element included
 *
 * Return: the length of t_big.
 */
static size_t nested_make(unsigned int depth)
{
	size_t len = 0;
	unsigned int i;

	for (i = 0; i < depth; i++)
		len += (size_t)sprintf(t_big + len, "<a>");
	for (i = 0; i < depth; i++)
		len += (size_t)sprintf(t_big + len, "</a>");

	return len;
}

/**
 * header_tests() - the stream header and the keepalive whitespace
 */
static void header_tests(void)
{
	struct xml_stream xs;
	struct tctx ctx;

	CASE("the stream header arrives as one node in the streams namespace");
	CHECK(stream_open(&xs, &ctx));
	CHECK_UINT(ctx.header.nodes, 1);
	CHECK(xml_node_is(&ctx.header, 0, "stream", XMPP_NS_STREAM));
	CHECK(ctx.header.node[0].parent == XML_NONE);
	CHECK(ctx.header.node[0].first_child == XML_NONE);
	CHECK(str_is(xml_attr_get(&ctx.header, 0, "id"), "c2s-1"));
	CHECK(str_is(xml_attr_get(&ctx.header, 0, "from"), "example.com"));
	CHECK(str_is(xml_attr_get(&ctx.header, 0, "version"), "1.0"));

	CASE("the xmlns declarations are not attributes, xml:lang is");
	CHECK_UINT(ctx.header.node[0].attr_count, 4);
	CHECK(xml_attr_get(&ctx.header, 0, "xmlns") == NULL);
	CHECK(xml_attr_get(&ctx.header, 0, "lang") == NULL);
	CHECK(str_is(attr_ns_get(&ctx.header, 0, "lang", XMPP_NS_XML), "en"));

	CASE("whitespace between stanzas gives no callback");
	CHECK(feed_str(&xs, " \n\t ") == 0);
	CHECK_UINT(ctx.stanzas, 0);
	CHECK_UINT(ctx.errors, 0);

	CASE("the root end tag gives stream_close");
	CHECK(feed_str(&xs, "</stream:stream>") == 0);
	CHECK_UINT(ctx.closes, 1);
	CHECK_UINT(ctx.errors, 0);

	CASE("an element after the root end tag fails the stream");
	CHECK(feed_str(&xs, "<message/>") == -EPROTO);
	CHECK_UINT(ctx.errors, 1);
	CHECK(ctx.reason != NULL);
	xml_stream_done(&xs);
}

/**
 * stanza_tests() - the stanza trees and their namespaces
 */
static void stanza_tests(void)
{
	struct xml_stream xs;
	struct tctx ctx;
	struct xml_doc *doc = &ctx.stanza;
	unsigned int node;
	unsigned int child;

	SUITE_CHECK(stream_open(&xs, &ctx));

	CASE("stream:features carries children in their own namespaces");
	CHECK(feed_str(&xs, "<stream:features>"
			    "<mechanisms xmlns='" NS_SASL "'>"
			    "<mechanism>SCRAM-SHA-1</mechanism>"
			    "<mechanism>PLAIN</mechanism>"
			    "</mechanisms>"
			    "<starttls xmlns='" NS_TLS "'><required/>"
			    "</starttls></stream:features>") == 0);
	CHECK_UINT(ctx.stanzas, 1);
	CHECK_UINT(doc->nodes, 6);
	CHECK(xml_node_is(doc, 0, "features", XMPP_NS_STREAM));
	node = xml_child(doc, XML_NONE, "mechanisms", NS_SASL);
	CHECK(node != XML_NONE);
	CHECK(xml_child(doc, XML_NONE, "mechanisms", XMPP_NS_CLIENT) ==
	      XML_NONE);
	child = xml_child(doc, node, "mechanism", NS_SASL);
	CHECK(child != XML_NONE);
	CHECK(str_is(xml_text_get(doc, child), "SCRAM-SHA-1"));
	child = xml_next(doc, child, "mechanism", NULL);
	CHECK(child != XML_NONE);
	CHECK(str_is(xml_text_get(doc, child), "PLAIN"));
	CHECK(xml_next(doc, child, NULL, NULL) == XML_NONE);
	node = xml_next(doc, node, NULL, NULL);
	CHECK(xml_node_is(doc, node, "starttls", NS_TLS));
	child = xml_child(doc, node, NULL, NULL);
	CHECK(xml_node_is(doc, child, "required", NS_TLS));
	CHECK(str_is(xml_text_get(doc, child), ""));
	CHECK_UINT(doc->node[child].parent, node);

	CASE("an iq with a disco#info query");
	CHECK(feed_str(&xs, "<iq type='get' id='disco1' to='example.com'>"
			    "<query xmlns='" NS_DISCO_INFO "'/></iq>") == 0);
	CHECK_UINT(ctx.stanzas, 2);
	CHECK(xml_node_is(doc, 0, "iq", XMPP_NS_CLIENT));
	CHECK(str_is(xml_attr_get(doc, 0, "type"), "get"));
	CHECK(str_is(xml_attr_get(doc, 0, "id"), "disco1"));
	CHECK(str_is(xml_attr_get(doc, 0, "to"), "example.com"));
	CHECK(xml_attr_get(doc, 0, "from") == NULL);
	node = xml_child(doc, XML_NONE, "query", NS_DISCO_INFO);
	CHECK(node != XML_NONE);
	CHECK(str_is(xml_text_get(doc, node), ""));
	CHECK_UINT(doc->node[node].attr_count, 0);
	CHECK(xml_attr_get(doc, node, "xmlns") == NULL);

	CASE("a message with a propose and a store sibling");
	CHECK(feed_str(&xs, "<message to='a@b' from='c@d/r' type='chat'>"
			    "<propose xmlns='" NS_JMI "' id='x'>"
			    "<description xmlns='" NS_RTP "' media='audio'/>"
			    "</propose><store xmlns='" NS_HINTS "'/>"
			    "</message>") == 0);
	CHECK_UINT(ctx.stanzas, 3);
	CHECK(xml_node_is(doc, 0, "message", XMPP_NS_CLIENT));
	node = xml_child(doc, XML_NONE, "propose", NS_JMI);
	CHECK(node != XML_NONE);
	CHECK(str_is(xml_attr_get(doc, node, "id"), "x"));
	child = xml_child(doc, node, "description", NS_RTP);
	CHECK(child != XML_NONE);
	CHECK(str_is(xml_attr_get(doc, child, "media"), "audio"));
	CHECK(xml_next(doc, child, NULL, NULL) == XML_NONE);
	node = xml_next(doc, node, "store", NS_HINTS);
	CHECK(node != XML_NONE);
	CHECK(xml_child(doc, XML_NONE, "store", NS_HINTS) == node);
	CHECK(xml_child(doc, XML_NONE, "store", NS_JMI) == XML_NONE);
	CHECK(xml_child(doc, node, NULL, NULL) == XML_NONE);

	CASE("a stanza split in the middle of a tag gives one callback");
	CHECK(feed_str(&xs, "<message to='a@b'><bo") == 0);
	CHECK_UINT(ctx.stanzas, 3);
	CHECK(feed_str(&xs, "dy>hi</body></message>") == 0);
	CHECK_UINT(ctx.stanzas, 4);
	node = xml_child(doc, XML_NONE, "body", XMPP_NS_CLIENT);
	CHECK(str_is(xml_text_get(doc, node), "hi"));

	CASE("character data with entities decodes");
	CHECK(feed_str(&xs, "<message><body>a &amp; b &lt; c &#x41; &quot;q"
			    "&quot; &apos;s&apos; &gt;</body></message>") == 0);
	CHECK_UINT(ctx.stanzas, 5);
	node = xml_child(doc, XML_NONE, "body", NULL);
	CHECK(str_is(xml_text_get(doc, node), "a & b < c A \"q\" 's' >"));

	CASE("character data around a child element concatenates");
	CHECK(feed_str(&xs, "<message><body>ab<x/>cd</body></message>") == 0);
	CHECK_UINT(ctx.stanzas, 6);
	node = xml_child(doc, XML_NONE, "body", NULL);
	CHECK(str_is(xml_text_get(doc, node), "abcd"));
	child = xml_child(doc, node, "x", XMPP_NS_CLIENT);
	CHECK(child != XML_NONE);
	CHECK(str_is(xml_text_get(doc, child), ""));

	CASE("a prefix declared on a child scopes to that child");
	CHECK(feed_str(&xs, "<message><a xmlns:p='urn:p'><p:b/></a>"
			    "<c xmlns='urn:c'><d/></c><e/></message>") == 0);
	CHECK_UINT(ctx.stanzas, 7);
	node = xml_child(doc, XML_NONE, "a", XMPP_NS_CLIENT);
	CHECK(xml_node_is(doc, xml_child(doc, node, NULL, NULL), "b", "urn:p"));
	node = xml_child(doc, XML_NONE, "c", "urn:c");
	CHECK(xml_node_is(doc, xml_child(doc, node, NULL, NULL), "d", "urn:c"));
	CHECK(xml_child(doc, XML_NONE, "e", XMPP_NS_CLIENT) != XML_NONE);

	CASE("the lookup helpers refuse a node outside the tree");
	CHECK(xml_child(doc, XML_NODES_MAX, NULL, NULL) == XML_NONE);
	CHECK(xml_next(doc, XML_NONE, NULL, NULL) == XML_NONE);
	CHECK(xml_attr_get(doc, XML_NONE, "to") == NULL);
	CHECK(str_is(xml_text_get(doc, XML_NONE), ""));
	CHECK(!xml_node_is(doc, XML_NONE, NULL, NULL));

	CASE("a prefixed attribute is not found by xml_attr_get");
	CHECK(feed_str(&xs, "<message><body xml:lang='de'>x</body>"
			    "</message>") == 0);
	CHECK_UINT(ctx.stanzas, 8);
	node = xml_child(doc, XML_NONE, "body", NULL);
	CHECK(xml_attr_get(doc, node, "lang") == NULL);
	CHECK(str_is(attr_ns_get(doc, node, "lang", XMPP_NS_XML), "de"));

	CASE("the header is not handed up again after the stanzas");
	CHECK_UINT(ctx.opens, 1);
	CHECK_UINT(ctx.errors, 0);
	xml_stream_done(&xs);
}

/**
 * fail_check() - feed one piece of input to a fresh stream and expect failure
 * @input: the bytes after the header
 * @reason: the expected reason, NULL for any
 *
 * Return: true when the stream failed once with that reason and refuses
 * the next feed.
 */
static bool fail_check(const char *input, const char *reason)
{
	struct xml_stream xs;
	struct tctx ctx;
	bool ok;

	if (!stream_open(&xs, &ctx))
		return false;

	ok = feed_str(&xs, input) == -EPROTO;
	ok = ok && ctx.errors == 1 && ctx.reason;
	ok = ok && (!reason || !strcmp(ctx.reason, reason));
	ok = ok && feed_str(&xs, "<message/>") == -EPROTO;
	ok = ok && ctx.errors == 1 && ctx.stanzas == 0;
	xml_stream_done(&xs);

	return ok;
}

/**
 * forbidden_tests() - the constructs RFC 6120 11.1 forbids
 */
static void forbidden_tests(void)
{
	struct xml_stream xs;
	struct tctx ctx;

	CASE("a comment fails the stream");
	CHECK(fail_check("<!-- hello --><message/>", "comment"));
	CHECK(fail_check("<message><!-- hello --></message>", "comment"));

	CASE("a processing instruction fails the stream");
	CHECK(fail_check("<?target data?><message/>",
			 "processing instruction"));

	CASE("an undefined entity fails the stream");
	CHECK(fail_check("<message><body>&bogus;</body></message>", NULL));

	CASE("a DOCTYPE fails the stream before the root element");
	memset(&xs, 0, sizeof(xs));
	memset(&ctx, 0, sizeof(ctx));
	CHECK(xml_stream_init(&xs, &t_cb, &ctx) == 0);
	CHECK(feed_str(&xs, "<?xml version='1.0'?>"
			    "<!DOCTYPE stream [<!ENTITY e 'x'>]>") == -EPROTO);
	CHECK_UINT(ctx.errors, 1);
	CHECK(str_is(ctx.reason, "doctype"));
	CHECK(feed_str(&xs, T_HEADER) == -EPROTO);
	CHECK_UINT(ctx.opens, 0);
	xml_stream_done(&xs);

	CASE("a parse error fails the stream");
	CHECK(fail_check("<message></iq>", NULL));
	CHECK(fail_check("<message to='a' to='b'/>", NULL));

	CASE("an unbound prefix fails the stream");
	CHECK(fail_check("<foo:message/>", "unbound namespace prefix"));
	CHECK(fail_check("<message foo:bar='x'/>", "unbound namespace prefix"));
}

/**
 * bound_tests() - the arena and wire bounds of a stanza
 */
static void bound_tests(void)
{
	struct xml_stream xs;
	struct tctx ctx;
	size_t len;
	unsigned int i;

	CASE("a stanza XML_DEPTH_MAX deep passes");
	SUITE_CHECK(stream_open(&xs, &ctx));
	len = nested_make(XML_DEPTH_MAX);
	CHECK(xml_stream_feed(&xs, t_big, len) == 0);
	CHECK_UINT(ctx.stanzas, 1);
	CHECK_UINT(ctx.stanza.nodes, XML_DEPTH_MAX);
	CHECK_UINT(ctx.errors, 0);

	CASE("a stanza deeper than XML_DEPTH_MAX fails");
	len = nested_make(XML_DEPTH_MAX + 1);
	CHECK(xml_stream_feed(&xs, t_big, len) == -EPROTO);
	CHECK_UINT(ctx.errors, 1);
	CHECK(str_is(ctx.reason, "stanza too deep"));
	CHECK_UINT(ctx.stanzas, 1);
	xml_stream_done(&xs);

	CASE("a stanza under XML_STANZA_MAX passes, twice in a row");
	SUITE_CHECK(stream_open(&xs, &ctx));
	len = big_stanza_make(XML_STANZA_MAX - T_SLICE);
	CHECK(feed_sliced(&xs, t_big, len) == 0);
	CHECK(feed_sliced(&xs, t_big, len) == 0);
	CHECK_UINT(ctx.stanzas, 2);
	CHECK_UINT(ctx.errors, 0);

	CASE("a stanza longer than XML_STANZA_MAX fails");
	len = big_stanza_make(XML_STANZA_MAX + 4 * T_SLICE);
	CHECK(feed_sliced(&xs, t_big, len) == -EPROTO);
	CHECK_UINT(ctx.errors, 1);
	CHECK(str_is(ctx.reason, "stanza too long"));
	CHECK_UINT(ctx.stanzas, 2);
	xml_stream_done(&xs);

	CASE("a stanza with more than XML_NODES_MAX elements fails");
	SUITE_CHECK(stream_open(&xs, &ctx));
	len = (size_t)sprintf(t_big, "<message>");
	for (i = 0; i < XML_NODES_MAX; i++)
		len += (size_t)sprintf(t_big + len, "<x/>");
	len += (size_t)sprintf(t_big + len, "</message>");
	CHECK(xml_stream_feed(&xs, t_big, len) == -EPROTO);
	CHECK(str_is(ctx.reason, "too many elements"));
	xml_stream_done(&xs);

	CASE("a start tag over many slices arrives without more input");
	SUITE_CHECK(stream_open(&xs, &ctx));
	len = (size_t)sprintf(t_big, "<message");
	for (i = 0; i < T_ATTRS_SLICED; i++)
		len += (size_t)sprintf(t_big + len, " a%u='v'", i);
	len += (size_t)sprintf(t_big + len, "/>");
	CHECK(len > 8 * T_SLICE);
	CHECK(feed_sliced(&xs, t_big, len) == 0);
	CHECK_UINT(ctx.stanzas, 1);
	CHECK_UINT(ctx.stanza.attrs, T_ATTRS_SLICED);
	CHECK_UINT(ctx.errors, 0);
	xml_stream_done(&xs);

	CASE("a stanza with more than XML_ATTRS_MAX attributes fails");
	SUITE_CHECK(stream_open(&xs, &ctx));
	len = (size_t)sprintf(t_big, "<message");
	for (i = 0; i <= XML_ATTRS_MAX; i++)
		len += (size_t)sprintf(t_big + len, " a%u='v'", i);
	len += (size_t)sprintf(t_big + len, "/>");
	CHECK(xml_stream_feed(&xs, t_big, len) == -EPROTO);
	CHECK(str_is(ctx.reason, "too many attributes"));
	xml_stream_done(&xs);

	CASE("a stanza with more than XML_TEXT_MAX bytes of text fails");
	SUITE_CHECK(stream_open(&xs, &ctx));
	len = (size_t)sprintf(t_big, "<message xmlns:p='urn:");
	memset(t_big + len, 'x', T_NS_URI_LONG);
	len += T_NS_URI_LONG;
	len += (size_t)sprintf(t_big + len, "'");
	for (i = 0; i < XML_ATTRS_MAX; i++)
		len += (size_t)sprintf(t_big + len, " p:a%u='v'", i);
	len += (size_t)sprintf(t_big + len, "/>");
	CHECK(len < XML_STANZA_MAX);
	CHECK(feed_sliced(&xs, t_big, len) == -EPROTO);
	CHECK(str_is(ctx.reason, "stanza text too long"));
	xml_stream_done(&xs);

	CASE("more than XML_NS_MAX declarations in scope fail");
	SUITE_CHECK(stream_open(&xs, &ctx));
	len = (size_t)sprintf(t_big, "<message");
	for (i = 0; i < XML_NS_MAX; i++)
		len += (size_t)sprintf(t_big + len, " xmlns:p%u='urn:%u'",
				       i, i);
	len += (size_t)sprintf(t_big + len, "/>");
	CHECK(xml_stream_feed(&xs, t_big, len) == -EPROTO);
	CHECK(str_is(ctx.reason, "too many namespace declarations"));
	xml_stream_done(&xs);
}

/**
 * reset_tests() - the stream restart after STARTTLS or SASL
 */
static void reset_tests(void)
{
	struct xml_stream xs;
	struct tctx ctx;

	CASE("xml_stream_reset accepts a new header afterwards");
	SUITE_CHECK(stream_open(&xs, &ctx));
	CHECK(feed_str(&xs, "<message/>") == 0);
	CHECK_UINT(ctx.stanzas, 1);
	CHECK(xml_stream_reset(&xs) == 0);
	CHECK(feed_str(&xs, "<stream:stream xmlns='jabber:client'"
			    " xmlns:stream='http://etherx.jabber.org/streams'"
			    " id='c2s-2' version='1.0'>") == 0);
	CHECK_UINT(ctx.opens, 2);
	CHECK(str_is(xml_attr_get(&ctx.header, 0, "id"), "c2s-2"));
	CHECK(feed_str(&xs, "<message/>") == 0);
	CHECK_UINT(ctx.stanzas, 2);
	CHECK_UINT(ctx.errors, 0);

	CASE("a reset forgets the namespaces of the old document");
	CHECK(xml_stream_reset(&xs) == 0);
	CHECK(feed_str(&xs, "<stream:features/>") == -EPROTO);
	CHECK_UINT(ctx.errors, 1);
	CHECK(str_is(ctx.reason, "unbound namespace prefix"));

	CASE("a reset recovers a failed stream");
	CHECK(xml_stream_reset(&xs) == 0);
	CHECK(feed_str(&xs, T_HEADER) == 0);
	CHECK_UINT(ctx.opens, 3);
	xml_stream_done(&xs);
}

/**
 * message_write() - the writer calls behind T_WRITTEN
 * @writer: the writer
 */
static void message_write(struct xml_w *writer)
{
	xml_w_start(writer, "message");
	xml_w_attr(writer, "to", "a@b");
	xml_w_attr(writer, "type", "chat");
	xml_w_start(writer, "propose");
	xml_w_attr(writer, "xmlns", NS_JMI);
	xml_w_attr_uint(writer, "id", 1);
	xml_w_start(writer, "description");
	xml_w_attr(writer, "xmlns", NS_RTP);
	xml_w_attr(writer, "media", "audio");
	xml_w_end(writer);
	xml_w_end(writer);
	xml_w_start(writer, "store");
	xml_w_attr(writer, "xmlns", NS_HINTS);
	xml_w_end(writer);
	xml_w_end(writer);
}

/**
 * writer_tests() - the stanza writer and the escaper
 */
static void writer_tests(void)
{
	struct xml_w writer;
	char mem[T_WRITER_MAX];
	char small[32];
	size_t len;

	CASE("the writer produces the expected message");
	xml_w_init(&writer, mem, sizeof(mem));
	message_write(&writer);
	len = xml_w_finish(&writer);
	CHECK_UINT(len, strlen(T_WRITTEN));
	CHECK(str_is(mem, T_WRITTEN));

	CASE("the writer escapes attribute values and text");
	xml_w_init(&writer, mem, sizeof(mem));
	xml_w_start(&writer, "body");
	xml_w_attr(&writer, "a", "<&\"'>");
	xml_w_text(&writer, "<&\"'>");
	xml_w_end(&writer);
	CHECK(xml_w_finish(&writer) > 0);
	CHECK(str_is(mem, "<body a=\"&lt;&amp;&quot;&apos;&gt;\">"
			  "&lt;&amp;&quot;&apos;&gt;</body>"));

	CASE("raw text closes the pending tag and goes in as it is");
	xml_w_init(&writer, mem, sizeof(mem));
	xml_w_start(&writer, "x");
	xml_w_raw(&writer, "<y/>");
	xml_w_end(&writer);
	CHECK_UINT(xml_w_finish(&writer), 11);
	CHECK(str_is(mem, "<x><y/></x>"));

	CASE("an empty text still closes the tag as a pair");
	xml_w_init(&writer, mem, sizeof(mem));
	xml_w_start(&writer, "x");
	xml_w_text(&writer, "");
	xml_w_end(&writer);
	CHECK_UINT(xml_w_finish(&writer), 7);
	CHECK(str_is(mem, "<x></x>"));

	CASE("a writer over a 32 byte buffer fails and finish returns 0");
	memset(small, 'z', sizeof(small));
	xml_w_init(&writer, small, sizeof(small));
	message_write(&writer);
	CHECK(writer.failed);
	CHECK_UINT(xml_w_finish(&writer), 0);
	CHECK(strlen(small) < sizeof(small));
	CHECK(!strncmp(small, "<message to=\"a@b\"", 17));

	CASE("an element left open makes finish return 0");
	xml_w_init(&writer, mem, sizeof(mem));
	xml_w_start(&writer, "x");
	CHECK_UINT(xml_w_finish(&writer), 0);
	CHECK(!writer.failed);

	CASE("an end without a start and an attribute without a tag fail");
	xml_w_init(&writer, mem, sizeof(mem));
	xml_w_end(&writer);
	CHECK(writer.failed);
	xml_w_init(&writer, mem, sizeof(mem));
	xml_w_start(&writer, "x");
	xml_w_text(&writer, "t");
	xml_w_attr(&writer, "a", "b");
	CHECK(writer.failed);

	CASE("a writer over an empty buffer fails without a write");
	xml_w_init(&writer, mem, 0);
	xml_w_start(&writer, "x");
	xml_w_end(&writer);
	CHECK_UINT(xml_w_finish(&writer), 0);

	CASE("xml_escape escapes the five characters, refuses a short buffer");
	CHECK_UINT(xml_escape("<&\"'>", mem, sizeof(mem)), 25);
	CHECK(str_is(mem, "&lt;&amp;&quot;&apos;&gt;"));
	CHECK_UINT(xml_escape("plain", mem, sizeof(mem)), 5);
	CHECK(str_is(mem, "plain"));
	CHECK_UINT(xml_escape("&", small, 5), 0);
	CHECK_UINT(xml_escape("&", small, 6), 5);
	CHECK(str_is(small, "&amp;"));
	CHECK_UINT(xml_escape("", small, 1), 0);
	CHECK(str_is(small, ""));
	CHECK_UINT(xml_escape("", small, 0), 0);
}

/**
 * round_trip_tests() - what the writer produced parses back as the same tree
 */
static void round_trip_tests(void)
{
	struct xml_stream xs;
	struct tctx ctx;
	struct xml_doc *doc = &ctx.stanza;
	struct xml_w writer;
	char mem[T_WRITER_MAX];
	unsigned int node;
	unsigned int child;

	SUITE_CHECK(stream_open(&xs, &ctx));

	CASE("the written message parses back into the same tree");
	xml_w_init(&writer, mem, sizeof(mem));
	message_write(&writer);
	CHECK(xml_w_finish(&writer) > 0);
	CHECK(feed_str(&xs, mem) == 0);
	CHECK_UINT(ctx.stanzas, 1);
	CHECK_UINT(doc->nodes, 4);
	CHECK_UINT(doc->attrs, 4);
	CHECK(xml_node_is(doc, 0, "message", XMPP_NS_CLIENT));
	CHECK(str_is(xml_attr_get(doc, 0, "to"), "a@b"));
	CHECK(str_is(xml_attr_get(doc, 0, "type"), "chat"));
	CHECK_UINT(doc->node[0].attr_count, 2);
	node = xml_child(doc, XML_NONE, "propose", NS_JMI);
	CHECK(node != XML_NONE);
	CHECK(str_is(xml_attr_get(doc, node, "id"), "1"));
	CHECK_UINT(doc->node[node].attr_count, 1);
	CHECK(str_is(xml_text_get(doc, node), ""));
	child = xml_child(doc, node, "description", NS_RTP);
	CHECK(child != XML_NONE);
	CHECK(str_is(xml_attr_get(doc, child, "media"), "audio"));
	CHECK(xml_child(doc, child, NULL, NULL) == XML_NONE);
	CHECK(xml_next(doc, child, NULL, NULL) == XML_NONE);
	node = xml_next(doc, node, NULL, NULL);
	CHECK(xml_node_is(doc, node, "store", NS_HINTS));
	CHECK_UINT(doc->node[node].attr_count, 0);
	CHECK(xml_next(doc, node, NULL, NULL) == XML_NONE);
	CHECK_UINT(ctx.errors, 0);
	xml_stream_done(&xs);
}

/**
 * main() - run every group of the suite
 *
 * Return: 0 when every case passed.
 */
int main(void)
{
	header_tests();
	stanza_tests();
	forbidden_tests();
	bound_tests();
	reset_tests();
	writer_tests();
	round_trip_tests();

	return tu_summary("test_xmpp_xml");
}
