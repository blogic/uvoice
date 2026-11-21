// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <expat.h>

#include "proto/xmpp/xml.h"

#define XML_XMLNS		"xmlns"
#define XML_XMLNS_LEN		(sizeof(XML_XMLNS) - 1)
#define XML_PREFIX_XML		"xml"
#define XML_PREFIX_XML_LEN	(sizeof(XML_PREFIX_XML) - 1)
#define XML_UINT_TEXT_MAX	16U

/**
 * doc_reset() - empty the arena of a tree, keeping its shared empty string
 * @doc: the tree
 */
static void doc_reset(struct xml_doc *doc)
{
	doc->nodes = 0;
	doc->attrs = 0;
	doc->text[0] = 0;
	doc->text_used = 1;
}

/**
 * text_append() - store a string in the arena of a tree
 * @doc: the tree
 * @src: the bytes
 * @len: their count
 *
 * Return: the stored, terminated copy, or NULL when the arena is full.
 */
static const char *text_append(struct xml_doc *doc, const char *src, size_t len)
{
	char *dst;

	if (doc->text_used + len + 1 > XML_TEXT_MAX)
		return NULL;

	dst = doc->text + doc->text_used;
	memcpy(dst, src, len);
	dst[len] = 0;
	doc->text_used += len + 1;

	return dst;
}

/**
 * node_text_add() - append character data to the text of a node
 * @doc: the tree
 * @node: the node
 * @src: the bytes
 * @len: their count
 *
 * Return: false when the arena is full.
 */
static bool node_text_add(struct xml_doc *doc, struct xml_node *node,
			  const char *src, size_t len)
{
	size_t old = strlen(node->text);
	const char *end = doc->text + doc->text_used;
	char *dst;

	if (node->text != doc->text && node->text + old + 1 == end) {
		if (doc->text_used + len > XML_TEXT_MAX)
			return false;

		dst = doc->text + doc->text_used - 1;
		memcpy(dst, src, len);
		dst[len] = 0;
		doc->text_used += len;
		return true;
	}

	if (doc->text_used + old + len + 1 > XML_TEXT_MAX)
		return false;

	dst = doc->text + doc->text_used;
	memcpy(dst, node->text, old);
	memcpy(dst + old, src, len);
	dst[old + len] = 0;
	node->text = dst;
	doc->text_used += old + len + 1;

	return true;
}

/**
 * stream_fail() - break the stream once, stop expat and report why
 * @xs: the stream
 * @reason: static text for the error callback
 */
static void stream_fail(struct xml_stream *xs, const char *reason)
{
	if (xs->failed)
		return;

	xs->failed = true;
	XML_StopParser(xs->parser, XML_FALSE);
	if (xs->cb.error)
		xs->cb.error(xs->user, reason);
}

/**
 * ns_push() - declare a namespace for the element that opens
 * @xs: the stream
 * @prefix: the prefix, "" for the default namespace
 * @uri: the namespace URI
 *
 * Return: false when the declaration does not fit, with the stream failed.
 */
static bool ns_push(struct xml_stream *xs, const char *prefix, const char *uri)
{
	struct xml_ns_decl *decl;

	if (xs->ns_count >= XML_NS_MAX) {
		stream_fail(xs, "too many namespace declarations");
		return false;
	}

	if (strlen(prefix) >= XML_NAME_MAX || strlen(uri) >= XML_NS_URI_MAX) {
		stream_fail(xs, "namespace declaration too long");
		return false;
	}

	decl = &xs->ns[xs->ns_count++];
	strcpy(decl->prefix, prefix);
	strcpy(decl->uri, uri);
	decl->depth = xs->depth;

	return true;
}

/**
 * ns_pop() - forget the declarations of the element that closes
 * @xs: the stream
 * @depth: the depth of that element
 */
static void ns_pop(struct xml_stream *xs, unsigned int depth)
{
	while (xs->ns_count && xs->ns[xs->ns_count - 1].depth == depth)
		xs->ns_count--;
}

/**
 * ns_resolve() - the namespace URI in scope for a prefix
 * @xs: the stream
 * @prefix: the prefix bytes
 * @prefix_len: their count, 0 for the default namespace
 *
 * Return: the URI, "" for an unprefixed name without a default namespace,
 * or NULL for an unbound prefix.
 */
static const char *ns_resolve(const struct xml_stream *xs, const char *prefix,
			      size_t prefix_len)
{
	unsigned int i = xs->ns_count;

	while (i--) {
		if (strlen(xs->ns[i].prefix) != prefix_len)
			continue;
		if (!memcmp(xs->ns[i].prefix, prefix, prefix_len))
			return xs->ns[i].uri;
	}

	return prefix_len ? NULL : "";
}

/**
 * qname_split() - split a qualified name into its prefix and local name
 * @qname: the name as expat reports it
 * @prefix_len: receives the length of the prefix, 0 when there is none
 *
 * Return: the local name inside @qname.
 */
static const char *qname_split(const char *qname, size_t *prefix_len)
{
	const char *colon = strchr(qname, ':');

	if (!colon) {
		*prefix_len = 0;
		return qname;
	}

	*prefix_len = (size_t)(colon - qname);

	return colon + 1;
}

/**
 * attr_is_xmlns() - whether an attribute declares a namespace
 * @qname: the attribute name
 * @prefix: receives the declared prefix, "" for the default namespace
 *
 * Return: true for xmlns and xmlns:prefix.
 */
static bool attr_is_xmlns(const char *qname, const char **prefix)
{
	if (!strcmp(qname, XML_XMLNS)) {
		*prefix = "";
		return true;
	}

	if (strncmp(qname, XML_XMLNS ":", XML_XMLNS_LEN + 1))
		return false;

	*prefix = qname + XML_XMLNS_LEN + 1;

	return true;
}

/**
 * ns_declare() - push every namespace declaration of an element
 * @xs: the stream
 * @atts: the attribute pairs as expat reports them
 *
 * Return: false when a declaration does not fit, with the stream failed.
 */
static bool ns_declare(struct xml_stream *xs, const char **atts)
{
	const char *prefix;
	unsigned int i;

	for (i = 0; atts[i]; i += 2) {
		if (!attr_is_xmlns(atts[i], &prefix))
			continue;
		if (!ns_push(xs, prefix, atts[i + 1]))
			return false;
	}

	return true;
}

/**
 * attr_resolve() - the local name and the namespace of an attribute
 * @xs: the stream
 * @qname: the attribute name
 * @local: receives the local name
 * @ns: receives the namespace URI, NULL for an unprefixed attribute
 *
 * Return: false for an unbound prefix, with the stream failed.
 */
static bool attr_resolve(struct xml_stream *xs, const char *qname,
			 const char **local, const char **ns)
{
	size_t prefix_len;

	*local = qname_split(qname, &prefix_len);
	if (!prefix_len) {
		*ns = NULL;
		return true;
	}

	if (prefix_len == XML_PREFIX_XML_LEN &&
	    !memcmp(qname, XML_PREFIX_XML, prefix_len)) {
		*ns = XMPP_NS_XML;
		return true;
	}

	*ns = ns_resolve(xs, qname, prefix_len);
	if (*ns)
		return true;

	stream_fail(xs, "unbound namespace prefix");

	return false;
}

/**
 * node_link() - append a node to the children of its parent
 * @doc: the tree
 * @parent: the parent node
 * @child: the new node
 */
static void node_link(struct xml_doc *doc, unsigned int parent,
		      unsigned int child)
{
	uint16_t *slot = &doc->node[parent].first_child;

	while (*slot != XML_NONE)
		slot = &doc->node[*slot].next_sibling;

	*slot = (uint16_t)child;
}

/**
 * node_add() - add an element to the tree
 * @xs: the stream
 * @parent: the parent node, XML_NONE for the root of the tree
 * @name: the local name
 * @ns: the namespace URI
 *
 * Return: the node index, or XML_NONE when the tree is full, with the
 * stream failed.
 */
static unsigned int node_add(struct xml_stream *xs, unsigned int parent,
			     const char *name, const char *ns)
{
	struct xml_doc *doc = &xs->doc;
	struct xml_node *node;
	unsigned int idx = doc->nodes;

	if (idx >= XML_NODES_MAX) {
		stream_fail(xs, "too many elements");
		return XML_NONE;
	}

	node = &doc->node[idx];
	node->name = text_append(doc, name, strlen(name));
	node->ns = text_append(doc, ns, strlen(ns));
	if (!node->name || !node->ns) {
		stream_fail(xs, "stanza text too long");
		return XML_NONE;
	}

	node->text = doc->text;
	node->parent = (uint16_t)parent;
	node->first_child = XML_NONE;
	node->next_sibling = XML_NONE;
	node->attr_first = XML_NONE;
	node->attr_count = 0;
	doc->nodes++;
	if (parent != XML_NONE)
		node_link(doc, parent, idx);

	return idx;
}

/**
 * attr_add() - add an attribute to the node added last
 * @xs: the stream
 * @idx: the node
 * @name: the local name
 * @ns: the namespace URI, NULL for an unprefixed attribute
 * @value: the value
 *
 * Return: false when the tree is full, with the stream failed.
 */
static bool attr_add(struct xml_stream *xs, unsigned int idx, const char *name,
		     const char *ns, const char *value)
{
	struct xml_doc *doc = &xs->doc;
	struct xml_node *node = &doc->node[idx];
	struct xml_attr *attr;

	if (doc->attrs >= XML_ATTRS_MAX) {
		stream_fail(xs, "too many attributes");
		return false;
	}

	attr = &doc->attr[doc->attrs];
	attr->name = text_append(doc, name, strlen(name));
	attr->value = text_append(doc, value, strlen(value));
	attr->ns = ns ? text_append(doc, ns, strlen(ns)) : NULL;
	if (!attr->name || !attr->value || (ns && !attr->ns)) {
		stream_fail(xs, "stanza text too long");
		return false;
	}

	if (node->attr_first == XML_NONE)
		node->attr_first = (uint16_t)doc->attrs;
	node->attr_count++;
	doc->attrs++;

	return true;
}

/**
 * attrs_add() - add every attribute of an element but its declarations
 * @xs: the stream
 * @idx: the node
 * @atts: the attribute pairs as expat reports them
 *
 * Return: false when an attribute does not fit or carries an unbound
 * prefix, with the stream failed.
 */
static bool attrs_add(struct xml_stream *xs, unsigned int idx,
		      const char **atts)
{
	const char *prefix;
	const char *local;
	const char *ns;
	unsigned int i;

	for (i = 0; atts[i]; i += 2) {
		if (attr_is_xmlns(atts[i], &prefix))
			continue;
		if (!attr_resolve(xs, atts[i], &local, &ns))
			return false;
		if (!attr_add(xs, idx, local, ns, atts[i + 1]))
			return false;
	}

	return true;
}

/**
 * header_emit() - hand the stream header up and empty the arena
 * @xs: the stream
 */
static void header_emit(struct xml_stream *xs)
{
	xs->header_seen = true;
	if (xs->cb.stream_open)
		xs->cb.stream_open(xs->user, &xs->doc);
	doc_reset(&xs->doc);
}

/**
 * stanza_emit() - hand a complete stanza up and empty the arena
 * @xs: the stream
 */
static void stanza_emit(struct xml_stream *xs)
{
	if (xs->cb.stanza)
		xs->cb.stanza(xs->user, &xs->doc);
	doc_reset(&xs->doc);
}

/**
 * on_element_start() - expat reports a start tag
 * @user: the stream
 * @qname: the element name with its prefix
 * @atts: the attribute pairs, terminated by NULL
 */
static void XMLCALL on_element_start(void *user, const XML_Char *qname,
				     const XML_Char **atts)
{
	struct xml_stream *xs = user;
	unsigned int parent = XML_NONE;
	unsigned int idx;
	const char *local;
	const char *ns;
	size_t prefix_len;

	if (xs->failed)
		return;

	if (xs->depth > XML_DEPTH_MAX) {
		stream_fail(xs, "stanza too deep");
		return;
	}

	if (xs->depth == 1)
		xs->stanza_bytes = 0;
	if (xs->depth >= 2)
		parent = xs->open[xs->depth - 2];

	if (!ns_declare(xs, atts))
		return;

	local = qname_split(qname, &prefix_len);
	ns = ns_resolve(xs, qname, prefix_len);
	if (!ns) {
		stream_fail(xs, "unbound namespace prefix");
		return;
	}

	idx = node_add(xs, parent, local, ns);
	if (idx == XML_NONE || !attrs_add(xs, idx, atts))
		return;

	if (xs->depth == 0) {
		xs->depth = 1;
		header_emit(xs);
		return;
	}

	xs->open[xs->depth - 1] = (uint16_t)idx;
	xs->depth++;
}

/**
 * on_element_end() - expat reports an end tag
 * @user: the stream
 * @qname: the element name with its prefix
 */
static void XMLCALL on_element_end(void *user, const XML_Char *qname)
{
	struct xml_stream *xs = user;

	if (xs->failed)
		return;

	xs->depth--;
	ns_pop(xs, xs->depth);
	if (xs->depth > 1)
		return;

	if (xs->depth == 1) {
		stanza_emit(xs);
		return;
	}

	xs->root_closed = true;
	if (xs->cb.stream_close)
		xs->cb.stream_close(xs->user);
}

/**
 * on_text() - expat reports character data, dropped outside a stanza
 * @user: the stream
 * @data: the bytes, not terminated
 * @len: their count
 */
static void XMLCALL on_text(void *user, const XML_Char *data, int len)
{
	struct xml_stream *xs = user;
	unsigned int idx;

	if (xs->failed || xs->depth < 2)
		return;

	idx = xs->open[xs->depth - 2];
	if (!node_text_add(&xs->doc, &xs->doc.node[idx], data, (size_t)len))
		stream_fail(xs, "stanza text too long");
}

/**
 * on_comment() - expat reports a comment, forbidden by RFC 6120 11.1
 * @user: the stream
 * @data: the comment text
 */
static void XMLCALL on_comment(void *user, const XML_Char *data)
{
	stream_fail(user, "comment");
}

/**
 * on_pi() - expat reports a processing instruction, forbidden by RFC 6120
 * @user: the stream
 * @target: the instruction target
 * @data: the instruction text
 */
static void XMLCALL on_pi(void *user, const XML_Char *target,
			  const XML_Char *data)
{
	stream_fail(user, "processing instruction");
}

/**
 * on_doctype() - expat reports a DOCTYPE, forbidden by RFC 6120 11.1
 * @user: the stream
 * @name: the document type name
 * @sysid: the system identifier
 * @pubid: the public identifier
 * @has_internal_subset: whether an internal subset follows
 */
static void XMLCALL on_doctype(void *user, const XML_Char *name,
			       const XML_Char *sysid, const XML_Char *pubid,
			       int has_internal_subset)
{
	stream_fail(user, "doctype");
}

/**
 * on_entity_decl() - expat reports an entity declaration, forbidden too
 * @user: the stream
 * @name: the entity name
 * @is_parameter_entity: whether it is a parameter entity
 * @value: the replacement text of an internal entity
 * @value_len: its length
 * @base: the base URI
 * @sysid: the system identifier of an external entity
 * @pubid: its public identifier
 * @notation: the notation name of an unparsed entity
 */
static void XMLCALL on_entity_decl(void *user, const XML_Char *name,
				   int is_parameter_entity,
				   const XML_Char *value, int value_len,
				   const XML_Char *base, const XML_Char *sysid,
				   const XML_Char *pubid,
				   const XML_Char *notation)
{
	stream_fail(user, "entity declaration");
}

/**
 * on_skipped_entity() - expat reports a reference to an undefined entity
 * @user: the stream
 * @name: the entity name
 * @is_parameter_entity: whether it is a parameter entity
 */
static void XMLCALL on_skipped_entity(void *user, const XML_Char *name,
				      int is_parameter_entity)
{
	stream_fail(user, "undefined entity");
}

/**
 * handlers_set() - install the expat handlers of a stream
 * @xs: the stream
 */
static void handlers_set(struct xml_stream *xs)
{
	XML_Parser parser = xs->parser;

	XML_SetUserData(parser, xs);
	XML_SetReparseDeferralEnabled(parser, XML_FALSE);
	XML_SetElementHandler(parser, on_element_start, on_element_end);
	XML_SetCharacterDataHandler(parser, on_text);
	XML_SetCommentHandler(parser, on_comment);
	XML_SetProcessingInstructionHandler(parser, on_pi);
	XML_SetStartDoctypeDeclHandler(parser, on_doctype);
	XML_SetEntityDeclHandler(parser, on_entity_decl);
	XML_SetSkippedEntityHandler(parser, on_skipped_entity);
}

/**
 * state_clear() - forget the document of a stream
 * @xs: the stream
 */
static void state_clear(struct xml_stream *xs)
{
	xs->ns_count = 0;
	xs->depth = 0;
	xs->failed = false;
	xs->header_seen = false;
	xs->root_closed = false;
	xs->stanza_bytes = 0;
	doc_reset(&xs->doc);
}

/**
 * xml_stream_init() - make the parser of one stream
 * @xs: the stream, zeroed by the caller
 * @cb: the callbacks, copied
 * @user: handed to every callback
 *
 * Return: 0, or -ENOMEM when expat refused a parser.
 */
int xml_stream_init(struct xml_stream *xs, const struct xml_stream_cb *cb,
		    void *user)
{
	xs->parser = XML_ParserCreate(NULL);
	if (!xs->parser)
		return -ENOMEM;

	if (cb)
		xs->cb = *cb;
	xs->user = user;
	state_clear(xs);
	handlers_set(xs);

	return 0;
}

/**
 * slice_feed() - count a slice against the open stanza, then parse it
 * @xs: the stream
 * @buf: the bytes
 * @len: their count, which fits an expat length
 *
 * Return: false once the stream failed.
 */
static bool slice_feed(struct xml_stream *xs, const char *buf, int len)
{
	if (xs->failed)
		return false;

	if (xs->depth >= 2) {
		xs->stanza_bytes += (size_t)len;
		if (xs->stanza_bytes > XML_STANZA_MAX) {
			stream_fail(xs, "stanza too long");
			return false;
		}
	}

	if (XML_Parse(xs->parser, buf, len, XML_FALSE) == XML_STATUS_OK)
		return true;

	stream_fail(xs, XML_ErrorString(XML_GetErrorCode(xs->parser)));

	return false;
}

/**
 * xml_stream_feed() - parse the next bytes of the stream
 * @xs: the stream
 * @buf: the bytes
 * @len: their count
 *
 * The callbacks run from inside this call. After an error the stream is
 * failed and every later feed is refused.
 *
 * Return: 0, or -EPROTO once the stream failed.
 */
int xml_stream_feed(struct xml_stream *xs, const char *buf, size_t len)
{
	int slice;

	while (len) {
		slice = len > (size_t)INT_MAX ? INT_MAX : (int)len;
		if (!slice_feed(xs, buf, slice))
			return -EPROTO;
		buf += slice;
		len -= (size_t)slice;
	}

	return xs->failed ? -EPROTO : 0;
}

/**
 * xml_stream_reset() - start a new XML document on the same stream
 * @xs: the stream
 *
 * RFC 6120 restarts the stream after STARTTLS and after SASL: the parser
 * forgets the old document, its namespaces and its header.
 *
 * Return: 0, or -ENOMEM when the parser could not be remade.
 */
int xml_stream_reset(struct xml_stream *xs)
{
	if (!XML_ParserReset(xs->parser, NULL)) {
		XML_ParserFree(xs->parser);
		xs->parser = XML_ParserCreate(NULL);
		if (!xs->parser)
			return -ENOMEM;
	}

	state_clear(xs);
	handlers_set(xs);

	return 0;
}

/**
 * xml_stream_done() - release the parser
 * @xs: the stream
 */
void xml_stream_done(struct xml_stream *xs)
{
	XML_ParserFree(xs->parser);
	xs->parser = NULL;
}

/**
 * node_match() - whether a node carries a name and a namespace
 * @node: the node
 * @name: the local name, NULL for any
 * @ns: the namespace URI, NULL for any
 *
 * Return: true when both match.
 */
static bool node_match(const struct xml_node *node, const char *name,
		       const char *ns)
{
	if (name && strcmp(node->name, name))
		return false;

	return !ns || !strcmp(node->ns, ns);
}

/**
 * sibling_find() - find a name and a namespace along a sibling chain
 * @doc: the tree
 * @node: the first node of the chain, XML_NONE for an empty chain
 * @name: the local name, NULL for any
 * @ns: the namespace URI, NULL for any
 *
 * Return: the node index, or XML_NONE.
 */
static unsigned int sibling_find(const struct xml_doc *doc, unsigned int node,
				 const char *name, const char *ns)
{
	while (node != XML_NONE && node < doc->nodes) {
		if (node_match(&doc->node[node], name, ns))
			return node;
		node = doc->node[node].next_sibling;
	}

	return XML_NONE;
}

/**
 * xml_child() - the first child of a node with a name and a namespace
 * @doc: the tree
 * @parent: the node, XML_NONE for the stanza element itself
 * @name: the local name, NULL for any
 * @ns: the namespace URI, NULL for any
 *
 * Return: the node index, or XML_NONE.
 */
unsigned int xml_child(const struct xml_doc *doc, unsigned int parent,
		       const char *name, const char *ns)
{
	if (parent == XML_NONE)
		parent = 0;
	if (parent >= doc->nodes)
		return XML_NONE;

	return sibling_find(doc, doc->node[parent].first_child, name, ns);
}

/**
 * xml_next() - the next sibling of a node with a name and a namespace
 * @doc: the tree
 * @node: the node
 * @name: the local name, NULL for any
 * @ns: the namespace URI, NULL for any
 *
 * Return: the node index, or XML_NONE.
 */
unsigned int xml_next(const struct xml_doc *doc, unsigned int node,
		      const char *name, const char *ns)
{
	if (node >= doc->nodes)
		return XML_NONE;

	return sibling_find(doc, doc->node[node].next_sibling, name, ns);
}

/**
 * xml_attr_get() - the value of an unprefixed attribute of a node
 * @doc: the tree
 * @node: the node
 * @name: the attribute name
 *
 * Return: the value, or NULL when the node carries no such attribute.
 */
const char *xml_attr_get(const struct xml_doc *doc, unsigned int node,
			 const char *name)
{
	const struct xml_attr *attr;
	unsigned int first;
	unsigned int i;

	if (node >= doc->nodes)
		return NULL;

	first = doc->node[node].attr_first;
	for (i = 0; i < doc->node[node].attr_count; i++) {
		attr = &doc->attr[first + i];
		if (!attr->ns && !strcmp(attr->name, name))
			return attr->value;
	}

	return NULL;
}

/**
 * xml_text_get() - the character data of a node
 * @doc: the tree
 * @node: the node
 *
 * Return: the text, "" when the node holds none or does not exist.
 */
const char *xml_text_get(const struct xml_doc *doc, unsigned int node)
{
	if (node >= doc->nodes)
		return "";

	return doc->node[node].text;
}

/**
 * xml_node_is() - whether a node carries a name and a namespace
 * @doc: the tree
 * @node: the node
 * @name: the local name, NULL for any
 * @ns: the namespace URI, NULL for any
 *
 * Return: true when both match.
 */
bool xml_node_is(const struct xml_doc *doc, unsigned int node, const char *name,
		 const char *ns)
{
	if (node >= doc->nodes)
		return false;

	return node_match(&doc->node[node], name, ns);
}

/**
 * escape_of() - the entity that stands for a byte in XML text
 * @byte: the byte
 *
 * Return: the entity text, or NULL when the byte stands as it is.
 */
static const char *escape_of(char byte)
{
	switch (byte) {
	case '&':
		return "&amp;";
	case '<':
		return "&lt;";
	case '>':
		return "&gt;";
	case '"':
		return "&quot;";
	case '\'':
		return "&apos;";
	default:
		return NULL;
	}
}

/**
 * xml_escape() - escape text for an attribute value or character data
 * @src: the text
 * @dst: receives the escaped text
 * @len: capacity of @dst
 *
 * Return: the escaped length, or 0 when it did not fit.
 */
size_t xml_escape(const char *src, char *dst, size_t len)
{
	size_t used = 0;
	const char *esc;
	size_t need;

	if (!len)
		return 0;

	for (; *src; src++) {
		esc = escape_of(*src);
		need = esc ? strlen(esc) : 1;
		if (used + need >= len) {
			dst[0] = 0;
			return 0;
		}

		if (esc)
			memcpy(dst + used, esc, need);
		else
			dst[used] = *src;
		used += need;
	}

	dst[used] = 0;

	return used;
}

/**
 * w_put() - append bytes to the writer, keeping one byte for the terminator
 * @writer: the writer
 * @src: the bytes
 * @len: their count
 */
static void w_put(struct xml_w *writer, const char *src, size_t len)
{
	if (writer->failed)
		return;

	if (writer->used + len + 1 > writer->len) {
		writer->failed = true;
		return;
	}

	memcpy(writer->mem + writer->used, src, len);
	writer->used += len;
}

/**
 * w_put_escaped() - append text to the writer with its entities
 * @writer: the writer
 * @text: the text
 */
static void w_put_escaped(struct xml_w *writer, const char *text)
{
	size_t len;

	if (writer->failed)
		return;

	len = xml_escape(text, writer->mem + writer->used,
			 writer->len - writer->used);
	if (!len && *text) {
		writer->failed = true;
		return;
	}

	writer->used += len;
}

/**
 * w_tag_close() - finish the start tag that still waits for its '>'
 * @writer: the writer
 */
static void w_tag_close(struct xml_w *writer)
{
	if (!writer->pending)
		return;

	w_put(writer, ">", 1);
	writer->pending = false;
}

/**
 * xml_w_init() - start a writer over a buffer
 * @writer: the writer
 * @mem: the buffer
 * @len: its capacity
 */
void xml_w_init(struct xml_w *writer, char *mem, size_t len)
{
	writer->mem = mem;
	writer->len = len;
	writer->used = 0;
	writer->open = 0;
	writer->pending = false;
	writer->failed = !len;
	if (len)
		mem[0] = 0;
}

/**
 * xml_w_start() - open an element
 * @writer: the writer
 * @name: the element name, a literal that outlives the writer
 */
void xml_w_start(struct xml_w *writer, const char *name)
{
	w_tag_close(writer);
	if (writer->open >= XML_DEPTH_MAX) {
		writer->failed = true;
		return;
	}

	w_put(writer, "<", 1);
	w_put(writer, name, strlen(name));
	writer->stack[writer->open++] = name;
	writer->pending = true;
}

/**
 * xml_w_attr() - one attribute of the element just started
 * @writer: the writer
 * @name: the attribute name
 * @value: its value, escaped here
 */
void xml_w_attr(struct xml_w *writer, const char *name, const char *value)
{
	if (!writer->pending) {
		writer->failed = true;
		return;
	}

	w_put(writer, " ", 1);
	w_put(writer, name, strlen(name));
	w_put(writer, "=\"", 2);
	w_put_escaped(writer, value);
	w_put(writer, "\"", 1);
}

/**
 * xml_w_attr_uint() - one numeric attribute of the element just started
 * @writer: the writer
 * @name: the attribute name
 * @value: its value
 */
void xml_w_attr_uint(struct xml_w *writer, const char *name, unsigned int value)
{
	char text[XML_UINT_TEXT_MAX];

	snprintf(text, sizeof(text), "%u", value);
	xml_w_attr(writer, name, text);
}

/**
 * xml_w_text() - character data inside the open element
 * @writer: the writer
 * @text: the text, escaped here
 */
void xml_w_text(struct xml_w *writer, const char *text)
{
	w_tag_close(writer);
	w_put_escaped(writer, text);
}

/**
 * xml_w_end() - close the innermost open element
 * @writer: the writer
 *
 * An element without content closes as an empty tag.
 */
void xml_w_end(struct xml_w *writer)
{
	const char *name;

	if (!writer->open) {
		writer->failed = true;
		return;
	}

	name = writer->stack[--writer->open];
	if (writer->pending) {
		w_put(writer, "/>", 2);
		writer->pending = false;
		return;
	}

	w_put(writer, "</", 2);
	w_put(writer, name, strlen(name));
	w_put(writer, ">", 1);
}

/**
 * xml_w_raw() - append text as it is
 * @writer: the writer
 * @text: the text, which the caller already escaped or serialised
 */
void xml_w_raw(struct xml_w *writer, const char *text)
{
	w_tag_close(writer);
	w_put(writer, text, strlen(text));
}

/**
 * xml_w_finish() - end the write
 * @writer: the writer
 *
 * Return: the length of the text in mem, or 0 when a write did not fit or an
 * element stayed open. mem is terminated either way.
 */
size_t xml_w_finish(struct xml_w *writer)
{
	if (writer->len)
		writer->mem[writer->used] = 0;

	if (writer->failed || writer->open)
		return 0;

	return writer->used;
}
