// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The XML layer of the XMPP unit (src/proto/xmpp/xml.c): the stream parser
 * over expat and the stanza writer.
 *
 * An XMPP stream is one XML document per direction (RFC 6120 section 4.1).
 * The parser hands the stream header up as the depth 0 element, every
 * complete depth 1 element up as one stanza tree, and the end of the root
 * element up as the stream close. The tree lives in fixed arenas inside
 * struct xml_doc: no allocation, a bounded size, and a stanza that outgrows
 * a bound fails the stream (RFC 6120 section 13.12).
 *
 * Namespaces are resolved here. The feed's expat carries no namespace mode,
 * so the parser keeps the xmlns declarations of every open element and maps
 * each name onto its namespace URI itself. An element's ns is the URI in
 * scope for its prefix, or for the default namespace without a prefix. An
 * attribute without a prefix has a NULL ns. The xmlns declarations do not
 * appear as attributes of the tree.
 *
 * RFC 6120 section 11.1 forbids comments, processing instructions, DTDs and
 * entity references beyond the five predefined ones in an XMPP stream. The
 * parser fails the stream on each of them.
 *
 * The writer builds one stanza into a caller buffer: start, attr, text, end.
 * It escapes every value and refuses to overflow; a refused write marks the
 * buffer and every later call is a no-op, so the caller checks once at the
 * end. Element names are literals of the caller and are not copied.
 */
#ifndef UVOICE_XMPP_XML_H
#define UVOICE_XMPP_XML_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One stanza on the wire, the bound every peer must respect. */
#define XML_STANZA_MAX		65536U
/* Elements of one stanza tree, attributes of one tree, bytes of its names,
 * namespaces, values and character data, and the element nesting depth. */
#define XML_NODES_MAX		1024U
#define XML_ATTRS_MAX		2048U
#define XML_TEXT_MAX		(2U * XML_STANZA_MAX)
#define XML_DEPTH_MAX		16U
/* Namespace declarations in scope at once, across the open elements. */
#define XML_NS_MAX		32U
#define XML_NAME_MAX		128U
#define XML_NS_URI_MAX		128U
/* No node, no attribute. */
#define XML_NONE		0xffffU

#define XMPP_NS_CLIENT		"jabber:client"
#define XMPP_NS_STREAM		"http://etherx.jabber.org/streams"
#define XMPP_NS_XML		"http://www.w3.org/XML/1998/namespace"

struct xml_attr {
	const char *name;
	const char *ns;			/* NULL for an unprefixed attribute */
	const char *value;
};

struct xml_node {
	const char *name;		/* the local name */
	const char *ns;			/* the namespace URI, never NULL */
	const char *text;		/* the character data, "" when none */
	uint16_t parent;		/* XML_NONE on the root of the tree */
	uint16_t first_child;		/* XML_NONE when none */
	uint16_t next_sibling;		/* XML_NONE when none */
	uint16_t attr_first;		/* into attr[], XML_NONE when none */
	uint16_t attr_count;
};

/*
 * One stanza tree. node[0] is the stanza element itself. Every string of the
 * tree points into text[], so the tree is a value and a copy of it stays
 * valid as long as the copy is kept whole.
 */
struct xml_doc {
	struct xml_node node[XML_NODES_MAX];
	unsigned int nodes;
	struct xml_attr attr[XML_ATTRS_MAX];
	unsigned int attrs;
	char text[XML_TEXT_MAX];
	size_t text_used;
};

/*
 * The parser callbacks, all on the thread that feeds the parser. The tree a
 * callback receives belongs to the parser and is valid for the call only.
 *
 *   stream_open   the root element with its attributes (id, from, version)
 *   stanza        one complete depth 1 element
 *   stream_close  the end tag of the root element
 *   error         the stream is broken: a parse error, a forbidden construct
 *                 or a stanza outside the bounds; reason is static text
 */
struct xml_stream_cb {
	void (*stream_open)(void *user, const struct xml_doc *header);
	void (*stanza)(void *user, const struct xml_doc *stanza);
	void (*stream_close)(void *user);
	void (*error)(void *user, const char *reason);
};

struct xml_ns_decl {
	char prefix[XML_NAME_MAX];	/* "" for the default namespace */
	char uri[XML_NS_URI_MAX];
	unsigned int depth;		/* the element that declared it */
};

struct xml_stream {
	void *parser;			/* the expat parser */
	struct xml_stream_cb cb;
	void *user;
	struct xml_doc doc;
	struct xml_ns_decl ns[XML_NS_MAX];
	unsigned int ns_count;
	unsigned int depth;
	uint16_t open[XML_DEPTH_MAX];	/* the node of each open element */
	bool failed;
	bool header_seen;
	bool root_closed;
	size_t stanza_bytes;
};

/**
 * xml_stream_init() - make the parser of one stream
 * @xs: the stream, zeroed by the caller
 * @cb: the callbacks, copied
 * @user: handed to every callback
 *
 * Return: 0, or -ENOMEM when expat refused a parser.
 */
int xml_stream_init(struct xml_stream *xs, const struct xml_stream_cb *cb,
		    void *user);

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
int xml_stream_feed(struct xml_stream *xs, const char *buf, size_t len);

/**
 * xml_stream_reset() - start a new XML document on the same stream
 * @xs: the stream
 *
 * RFC 6120 restarts the stream after STARTTLS and after SASL: the parser
 * forgets the old document, its namespaces and its header.
 *
 * Return: 0, or -ENOMEM when the parser could not be remade.
 */
int xml_stream_reset(struct xml_stream *xs);

/**
 * xml_stream_done() - release the parser
 * @xs: the stream
 */
void xml_stream_done(struct xml_stream *xs);

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
		       const char *name, const char *ns);

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
		      const char *name, const char *ns);

/**
 * xml_attr_get() - the value of an unprefixed attribute of a node
 * @doc: the tree
 * @node: the node
 * @name: the attribute name
 *
 * Return: the value, or NULL when the node carries no such attribute.
 */
const char *xml_attr_get(const struct xml_doc *doc, unsigned int node,
			 const char *name);

/**
 * xml_text_get() - the character data of a node
 * @doc: the tree
 * @node: the node
 *
 * Return: the text, "" when the node holds none or does not exist.
 */
const char *xml_text_get(const struct xml_doc *doc, unsigned int node);

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
		 const char *ns);

/*
 * The writer. mem is the caller's buffer, len its capacity. open counts the
 * elements started and not ended, pending says the start tag of the last one
 * still waits for its '>'. failed is set by the write that did not fit.
 */
struct xml_w {
	char *mem;
	size_t len;
	size_t used;
	const char *stack[XML_DEPTH_MAX];
	unsigned int open;
	bool pending;
	bool failed;
};

/**
 * xml_w_init() - start a writer over a buffer
 * @w: the writer
 * @mem: the buffer
 * @len: its capacity
 */
void xml_w_init(struct xml_w *w, char *mem, size_t len);

/**
 * xml_w_start() - open an element
 * @w: the writer
 * @name: the element name, a literal that outlives the writer
 */
void xml_w_start(struct xml_w *w, const char *name);

/**
 * xml_w_attr() - one attribute of the element just started
 * @w: the writer
 * @name: the attribute name
 * @value: its value, escaped here
 */
void xml_w_attr(struct xml_w *w, const char *name, const char *value);

/**
 * xml_w_attr_uint() - one numeric attribute of the element just started
 * @w: the writer
 * @name: the attribute name
 * @value: its value
 */
void xml_w_attr_uint(struct xml_w *w, const char *name, unsigned int value);

/**
 * xml_w_text() - character data inside the open element
 * @w: the writer
 * @text: the text, escaped here
 */
void xml_w_text(struct xml_w *w, const char *text);

/**
 * xml_w_end() - close the innermost open element
 * @w: the writer
 *
 * An element without content closes as an empty tag.
 */
void xml_w_end(struct xml_w *w);

/**
 * xml_w_raw() - append text as it is
 * @w: the writer
 * @text: the text, which the caller already escaped or serialised
 */
void xml_w_raw(struct xml_w *w, const char *text);

/**
 * xml_w_finish() - end the write
 * @w: the writer
 *
 * Return: the length of the text in mem, or 0 when a write did not fit or an
 * element stayed open; mem is terminated either way.
 */
size_t xml_w_finish(struct xml_w *w);

/**
 * xml_escape() - escape text for an attribute value or character data
 * @src: the text
 * @dst: receives the escaped text
 * @len: capacity of @dst
 *
 * Return: the escaped length, or 0 when it did not fit.
 */
size_t xml_escape(const char *src, char *dst, size_t len);

#endif
