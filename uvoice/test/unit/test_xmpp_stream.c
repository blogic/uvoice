// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the client to server stream (src/proto/xmpp/stream.c).
 *
 * Real sockets, real uloop, a scripted server. The server listens on the
 * IPv4 loopback, accepts the one connection the stream opens and walks a
 * script: each step waits for a substring in what the client sent, answers
 * with a canned text and may switch the connection to TLS or close it. The
 * SASL steps carry the RFC 5802 section 5 exchange, so the client nonce
 * comes from the entropy hook of the stream configuration.
 *
 * The TLS fixtures are the committed PEM files of test/unit/tls/, found the
 * way test_sip_stream finds them: the UVOICE_TLS_FIXTURES variable first,
 * the cmake define second, the tls directory beside this file third.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include <libubox/utils.h>

#include "posix.h"
#include "proto/xmpp/disco.h"
#include "proto/xmpp/stream.h"
#include "proto_host_fake.h"
#include "test_util.h"

#define SLICE_MS	10
#define WAIT_MS		4000
#define STEPS_MAX	24
#define LOG_MAX		32
#define FAKE_RX_MAX	16384
#define FAKE_ALL_MAX	65536

#define DOMAIN		"sip.example.test"
#define USER_JID	"user@" DOMAIN
#define BOUND_JID	USER_JID "/uvoice"

#define SERVER_HEADER							\
	"<?xml version='1.0'?><stream:stream xmlns='jabber:client' "	\
	"xmlns:stream='http://etherx.jabber.org/streams' id='s1' "	\
	"from='" DOMAIN "' version='1.0'>"
#define FEATURES_STARTTLS						\
	"<stream:features><starttls xmlns='" XMPP_NS_TLS "'>"		\
	"<required/></starttls></stream:features>"
#define FEATURES_NO_TLS							\
	"<stream:features><mechanisms xmlns='" XMPP_NS_SASL "'>"	\
	"<mechanism>PLAIN</mechanism></mechanisms></stream:features>"
#define FEATURES_SCRAM							\
	"<stream:features><mechanisms xmlns='" XMPP_NS_SASL "'>"	\
	"<mechanism>PLAIN</mechanism><mechanism>SCRAM-SHA-1</mechanism>"\
	"</mechanisms></stream:features>"
#define FEATURES_PLAIN							\
	"<stream:features><mechanisms xmlns='" XMPP_NS_SASL "'>"	\
	"<mechanism>PLAIN</mechanism></mechanisms></stream:features>"
#define FEATURES_BIND							\
	"<stream:features><bind xmlns='" XMPP_NS_BIND "'/>"		\
	"<session xmlns='" XMPP_NS_SESSION "'><optional/></session>"	\
	"<sm xmlns='" XMPP_NS_SM "'/></stream:features>"
#define FEATURES_BIND_ONLY						\
	"<stream:features><bind xmlns='" XMPP_NS_BIND "'/>"		\
	"</stream:features>"
#define PROCEED		"<proceed xmlns='" XMPP_NS_TLS "'/>"
#define BIND_RESULT							\
	"<iq type='result' id='%s'><bind xmlns='" XMPP_NS_BIND "'>"	\
	"<jid>" BOUND_JID "</jid></bind></iq>"
#define ENABLED		"<enabled xmlns='" XMPP_NS_SM "' id='sm1'/>"
#define IQ_RESULT	"<iq type='result' id='%s'/>"
#define SASL_FAILURE							\
	"<failure xmlns='" XMPP_NS_SASL "'><not-authorized/></failure>"
#define SASL_SUCCESS_EMPTY	"<success xmlns='" XMPP_NS_SASL "'/>"

/* The RFC 5802 section 5 exchange. */
#define RFC_NONCE_B64	"fyko+d2lbbFgONRv9qkxdawL"
#define RFC_CLIENT_FIRST "n,,n=user,r=" RFC_NONCE_B64
#define RFC_SERVER_FIRST						\
	"r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,s=QSXCR+Q6sek8bf92,"\
	"i=4096"
#define RFC_CLIENT_FINAL						\
	"c=biws,r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,"		\
	"p=v0X8v3Bz2T0CJGbJQyF0X+HI4Ts="
#define RFC_SERVER_FINAL "v=rmF9pqV8S7suAoZWja4dJRkFsKQ="

enum step_action {
	STEP_NONE = 0,
	STEP_TLS,
	STEP_CLOSE,
};

struct step {
	const char *wait;
	const char *reply;		/* NULL sends nothing */
	bool with_id;			/* reply carries %s for the iq id */
	enum step_action action;
};

#define STEP(w, r)		{ .wait = (w), .reply = (r) }
#define STEP_ID(w, r)		{ .wait = (w), .reply = (r), .with_id = true }
#define STEP_ACT(w, r, a)	{ .wait = (w), .reply = (r), .action = (a) }

struct fake {
	struct uloop_fd listen;
	struct uloop_fd conn;
	struct sip_tls_conn tls;
	bool tls_up;
	bool tls_handshake;
	bool direct_tls;
	const struct step *steps;
	unsigned int nsteps;
	unsigned int step;
	unsigned int accepts;
	uint16_t port;
	char rx[FAKE_RX_MAX];
	size_t rx_len;
	char all[FAKE_ALL_MAX];
	size_t all_len;
	char tls_err[SIP_TLS_ERR_MAX];
};

struct clog {
	unsigned int states;
	uint8_t state[LOG_MAX];
	char reason[LOG_MAX][XMPP_REASON_MAX];
	unsigned int stanzas;
	char name[LOG_MAX][32];
	char from[LOG_MAX][JID_MAX];
	unsigned int iq_replies;
	bool iq_reply_null;
	char iq_reply_type[16];
};

static struct fake fake;
static struct clog clog;
static struct xmpp_stream xs;
static struct sip_dns dns;
static struct sip_tls cli_tls;
static struct sip_tls srv_tls;

static char fix_cert[PATH_MAX];
static char fix_key[PATH_MAX];
static char fix_ca[PATH_MAX];
static char fix_other_ca[PATH_MAX];

static char auth_scram[512];
static char challenge[512];
static char response_final[512];
static char success_final[512];
static char auth_plain[512];

static uint8_t rfc_nonce[SASL_NONCE_BYTES];

/* ------------------------------------------------------------- the loop */

/**
 * guard_expire() - end one slice of the loop
 * @timeout: the guard
 */
static void guard_expire(struct uloop_timeout *timeout)
{
	uloop_end();
}

/**
 * loop_pump() - run uloop for a while
 * @ms: the slice
 */
static void loop_pump(unsigned int ms)
{
	static struct uloop_timeout guard;

	guard.cb = guard_expire;
	uloop_timeout_set(&guard, (int)ms);
	uloop_cancelled = false;
	uloop_run();
	uloop_timeout_cancel(&guard);
}

/**
 * wait_state() - pump until the stream reaches a state
 * @want: the state
 * @ms: the window
 *
 * Return: true when it got there.
 */
static bool wait_state(enum xmpp_stream_state want, unsigned int ms)
{
	unsigned int spent;

	for (spent = 0; spent < ms && xs.state != want; spent += SLICE_MS)
		loop_pump(SLICE_MS);

	return xs.state == want;
}

/**
 * wait_step() - pump until the server script passed a step
 * @step: the index the script must reach
 * @ms: the window
 *
 * Return: true when it got there.
 */
static bool wait_step(unsigned int step, unsigned int ms)
{
	unsigned int spent;

	for (spent = 0; spent < ms && fake.step < step; spent += SLICE_MS)
		loop_pump(SLICE_MS);

	return fake.step >= step;
}

/**
 * wait_stanzas() - pump until the owner saw a number of stanzas
 * @count: the number
 * @ms: the window
 *
 * Return: true when it got there.
 */
static bool wait_stanzas(unsigned int count, unsigned int ms)
{
	unsigned int spent;

	for (spent = 0; spent < ms && clog.stanzas < count; spent += SLICE_MS)
		loop_pump(SLICE_MS);

	return clog.stanzas >= count;
}

/* ------------------------------------------------------------ fixtures */

/**
 * fixture_dir_take() - where the PEM files live
 * @out: receives the directory
 * @olen: its capacity
 *
 * Return: true when a directory was named.
 */
static bool fixture_dir_take(char *out, size_t olen)
{
	const char *env = getenv("UVOICE_TLS_FIXTURES");
	const char *sep;

	if (env && env[0]) {
		snprintf(out, olen, "%s", env);
		return true;
	}

#ifdef UVOICE_TLS_FIXTURE_DIR
	snprintf(out, olen, "%s", UVOICE_TLS_FIXTURE_DIR);
	return true;
#endif

	sep = strrchr(__FILE__, '/');
	if (!sep)
		return false;

	snprintf(out, olen, "%.*s/tls", (int)(sep - __FILE__), __FILE__);

	return true;
}

/**
 * fixtures_take() - name the four PEM files
 *
 * Return: true when the server pair is readable.
 */
static bool fixtures_take(void)
{
	char dir[PATH_MAX - 32];

	if (!fixture_dir_take(dir, sizeof(dir)))
		return false;

	snprintf(fix_cert, sizeof(fix_cert), "%s/server.crt", dir);
	snprintf(fix_key, sizeof(fix_key), "%s/server.key", dir);
	snprintf(fix_ca, sizeof(fix_ca), "%s/ca.crt", dir);
	snprintf(fix_other_ca, sizeof(fix_other_ca), "%s/other-ca.crt", dir);

	return !access(fix_cert, R_OK) && !access(fix_key, R_OK) &&
	       !access(fix_ca, R_OK);
}

/**
 * sasl_texts_build() - the base64 forms of the RFC 5802 exchange
 */
static void sasl_texts_build(void)
{
	char b64[256];
	static const char plain[] = "\0user\0pencil";

	b64_decode(RFC_NONCE_B64, rfc_nonce, sizeof(rfc_nonce));

	b64_encode(RFC_CLIENT_FIRST, strlen(RFC_CLIENT_FIRST), b64,
		   sizeof(b64));
	snprintf(auth_scram, sizeof(auth_scram),
		 "<auth xmlns=\"" XMPP_NS_SASL "\" mechanism=\"SCRAM-SHA-1\">%s"
		 "</auth>", b64);

	b64_encode(RFC_SERVER_FIRST, strlen(RFC_SERVER_FIRST), b64,
		   sizeof(b64));
	snprintf(challenge, sizeof(challenge),
		 "<challenge xmlns='" XMPP_NS_SASL "'>%s</challenge>", b64);

	b64_encode(RFC_CLIENT_FINAL, strlen(RFC_CLIENT_FINAL), b64,
		   sizeof(b64));
	snprintf(response_final, sizeof(response_final),
		 "<response xmlns=\"" XMPP_NS_SASL "\">%s</response>", b64);

	b64_encode(RFC_SERVER_FINAL, strlen(RFC_SERVER_FINAL), b64,
		   sizeof(b64));
	snprintf(success_final, sizeof(success_final),
		 "<success xmlns='" XMPP_NS_SASL "'>%s</success>", b64);

	b64_encode(plain, sizeof(plain) - 1, b64, sizeof(b64));
	snprintf(auth_plain, sizeof(auth_plain),
		 "<auth xmlns=\"" XMPP_NS_SASL "\" mechanism=\"PLAIN\">%s"
		 "</auth>", b64);
}

/* --------------------------------------------------------- fake server */

static void fake_io(struct uloop_fd *ufd, unsigned int events);

/**
 * fake_conn_close() - drop the accepted connection
 */
static void fake_conn_close(void)
{
	if (fake.conn.fd < 0)
		return;

	sip_tls_conn_close(&fake.tls);
	uloop_fd_delete(&fake.conn);
	close(fake.conn.fd);
	fake.conn.fd = -1;
	fake.tls_up = false;
	fake.tls_handshake = false;
}

/**
 * fake_send() - write text to the client, in the clear or through TLS
 * @text: the text
 */
static void fake_send(const char *text)
{
	size_t len = strlen(text), off = 0, done;
	enum sip_tls_io io;
	ssize_t moved;

	while (off < len) {
		if (fake.tls_up) {
			io = sip_tls_conn_write(&fake.tls, text + off,
						len - off, &done);
			if (io == SIP_TLS_ERR)
				return;
			off += done;
			if (!done)
				loop_pump(SLICE_MS);
			continue;
		}

		moved = send(fake.conn.fd, text + off, len - off, MSG_NOSIGNAL);
		if (moved < 0 && errno == EAGAIN) {
			loop_pump(SLICE_MS);
			continue;
		}
		if (moved < 0)
			return;
		off += (size_t)moved;
	}
}

/**
 * fake_handshake() - one step of the server side handshake
 */
static void fake_handshake(void)
{
	enum sip_tls_io io = sip_tls_conn_io(&fake.tls);

	if (io == SIP_TLS_ERR) {
		snprintf(fake.tls_err, sizeof(fake.tls_err), "%s",
			 sip_tls_conn_error(&fake.tls));
		fake_conn_close();
		return;
	}

	if (io == SIP_TLS_OK) {
		fake.tls_up = true;
		fake.tls_handshake = false;
		uloop_fd_add(&fake.conn, ULOOP_READ);
		return;
	}

	uloop_fd_add(&fake.conn, io == SIP_TLS_WANT_WRITE ? ULOOP_WRITE :
							   ULOOP_READ);
}

/**
 * fake_tls_start() - switch the accepted connection to TLS
 */
static void fake_tls_start(void)
{
	if (!sip_tls_conn_start(&srv_tls, &fake.tls, fake.conn.fd, true,
				NULL)) {
		snprintf(fake.tls_err, sizeof(fake.tls_err), "%s",
			 sip_tls_conn_error(&fake.tls));
		fake_conn_close();
		return;
	}

	fake.tls_handshake = true;
	fake_handshake();
}

/**
 * fake_id_take() - the id of the last iq the client sent
 * @out: receives the id
 * @len: its capacity
 */
static void fake_id_take(char *out, size_t len)
{
	const char *at = NULL, *next = fake.rx, *end;

	out[0] = 0;
	while ((next = strstr(next, " id=\"")) != NULL) {
		at = next + 5;
		next = at;
	}
	if (!at)
		return;

	end = strchr(at, '"');
	if (!end)
		return;

	snprintf(out, len, "%.*s", (int)(end - at), at);
}

/**
 * fake_steps_run() - answer every step the received text satisfies
 */
static void fake_steps_run(void)
{
	char id[XMPP_ID_MAX];
	char reply[1024];
	const struct step *step;

	while (fake.step < fake.nsteps && fake.conn.fd >= 0) {
		step = &fake.steps[fake.step];
		if (!strstr(fake.rx, step->wait))
			return;

		fake_id_take(id, sizeof(id));
		fake.rx_len = 0;
		fake.rx[0] = 0;
		fake.step++;

		if (step->reply) {
			if (step->with_id)
				snprintf(reply, sizeof(reply), step->reply, id);
			else
				snprintf(reply, sizeof(reply), "%s",
					 step->reply);
			fake_send(reply);
		}

		if (step->action == STEP_TLS)
			fake_tls_start();
		else if (step->action == STEP_CLOSE)
			fake_conn_close();
	}
}

/**
 * fake_rx_take() - append received bytes to the two buffers
 * @chunk: the bytes
 * @got: their count
 */
static void fake_rx_take(const char *chunk, size_t got)
{
	if (fake.rx_len + got < sizeof(fake.rx)) {
		memcpy(fake.rx + fake.rx_len, chunk, got);
		fake.rx_len += got;
		fake.rx[fake.rx_len] = 0;
	}

	if (fake.all_len + got < sizeof(fake.all)) {
		memcpy(fake.all + fake.all_len, chunk, got);
		fake.all_len += got;
		fake.all[fake.all_len] = 0;
	}
}

/**
 * fake_io() - the accepted connection has an event
 * @ufd: the connection
 * @events: unused
 */
static void fake_io(struct uloop_fd *ufd, unsigned int events)
{
	char chunk[4096];
	size_t got;
	ssize_t moved;
	enum sip_tls_io io;

	if (fake.tls_handshake) {
		fake_handshake();
		return;
	}

	if (fake.tls_up) {
		io = sip_tls_conn_read(&fake.tls, chunk, sizeof(chunk), &got);
		if (io == SIP_TLS_ERR || (io == SIP_TLS_OK && !got)) {
			fake_conn_close();
			return;
		}
		if (io != SIP_TLS_OK)
			return;
		fake_rx_take(chunk, got);
		fake_steps_run();
		return;
	}

	moved = recv(fake.conn.fd, chunk, sizeof(chunk), MSG_DONTWAIT);
	if (moved == 0 || (moved < 0 && errno != EAGAIN)) {
		fake_conn_close();
		return;
	}
	if (moved < 0)
		return;

	fake_rx_take(chunk, (size_t)moved);
	fake_steps_run();
}

/**
 * fake_accept() - take the connection the stream opened
 * @ufd: the listener
 * @events: unused
 */
static void fake_accept(struct uloop_fd *ufd, unsigned int events)
{
	int fd = accept4(fake.listen.fd, NULL, NULL,
			 SOCK_NONBLOCK | SOCK_CLOEXEC);

	if (fd < 0)
		return;

	fake_conn_close();
	fake.conn.fd = fd;
	fake.conn.cb = fake_io;
	fake.step = 0;
	fake.rx_len = 0;
	fake.rx[0] = 0;
	fake.accepts++;
	uloop_fd_add(&fake.conn, ULOOP_READ);

	if (fake.direct_tls)
		fake_tls_start();
}

/**
 * fake_listen() - open the loopback listener
 *
 * Return: true when it listens.
 */
static bool fake_listen(void)
{
	struct sockaddr_in sa = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	socklen_t len = sizeof(sa);
	int fd;

	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return false;

	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
	    listen(fd, 4) < 0 ||
	    getsockname(fd, (struct sockaddr *)&sa, &len)) {
		close(fd);
		return false;
	}

	fake.port = ntohs(sa.sin_port);
	fake.conn.fd = -1;
	fake.listen.fd = fd;
	fake.listen.cb = fake_accept;
	uloop_fd_add(&fake.listen, ULOOP_READ);

	return true;
}

/**
 * fake_script_set() - arm the next scenario
 * @steps: the script
 * @nsteps: its length
 * @direct_tls: whether the connection starts with TLS
 */
static void fake_script_set(const struct step *steps, unsigned int nsteps,
			    bool direct_tls)
{
	fake_conn_close();
	fake.steps = steps;
	fake.nsteps = nsteps;
	fake.step = 0;
	fake.direct_tls = direct_tls;
	fake.rx_len = 0;
	fake.rx[0] = 0;
	fake.all_len = 0;
	fake.all[0] = 0;
	fake.tls_err[0] = 0;
}

/* ------------------------------------------------------------- the client */

/**
 * on_state() - record a transition
 * @stream: the stream
 * @state: the new state
 * @reason: the text
 */
static void on_state(struct xmpp_stream *stream, enum xmpp_stream_state state,
		     const char *reason)
{
	unsigned int i = clog.states;

	if (i < LOG_MAX) {
		clog.state[i] = (uint8_t)state;
		snprintf(clog.reason[i], sizeof(clog.reason[i]), "%s", reason);
	}
	clog.states++;
}

/**
 * on_stanza() - record a stanza, answer the test iq
 * @stream: the stream
 * @doc: the stanza
 *
 * Return: true for an iq that carried the test element.
 */
static bool on_stanza(struct xmpp_stream *stream, const struct xml_doc *doc)
{
	unsigned int i = clog.stanzas;
	const char *from = xml_attr_get(doc, 0, "from");

	if (i < LOG_MAX) {
		snprintf(clog.name[i], sizeof(clog.name[i]), "%s",
			 doc->node[0].name);
		snprintf(clog.from[i], sizeof(clog.from[i]), "%s",
			 from ? from : "");
	}
	clog.stanzas++;

	if (xml_child(doc, XML_NONE, "test", "test:ns") == XML_NONE)
		return false;

	xmpp_stream_iq_result(stream, doc);

	return true;
}

/**
 * on_iq_reply() - record the answer to the iq the test sent
 * @stream: the stream
 * @reply: the answer, NULL for none
 * @priv: unused
 */
static void on_iq_reply(struct xmpp_stream *stream, const struct xml_doc *reply,
			void *priv)
{
	const char *type = reply ? xml_attr_get(reply, 0, "type") : NULL;

	clog.iq_replies++;
	clog.iq_reply_null = !reply;
	snprintf(clog.iq_reply_type, sizeof(clog.iq_reply_type), "%s",
		 type ? type : "");
}

/**
 * fixed_random() - the RFC nonce for a nonce request, zeros otherwise
 * @buf: receives the bytes
 * @len: how many
 */
static void fixed_random(void *buf, size_t len)
{
	if (len == SASL_NONCE_BYTES) {
		memcpy(buf, rfc_nonce, len);
		return;
	}

	memset(buf, 0x5a, len);
}

/**
 * dns_done() - hand the resolver answer to the stream
 * @resolver: the resolver
 * @query: the query handle
 * @owner: the owner handle
 * @res: the answer
 * @priv: unused
 */
static void dns_done(struct sip_dns *resolver, uint32_t query, uint32_t owner,
		     const struct sip_dns_result *res, void *priv)
{
	xmpp_stream_dns_done(&xs, query, res);
}

static const struct xmpp_stream_cb client_cb = {
	.state = on_state,
	.stanza = on_stanza,
};

/**
 * client_cfg() - the account of every scenario
 * @cfg: receives it
 * @direct_tls: XEP-0368 instead of STARTTLS
 */
static void client_cfg(struct xmpp_stream_cfg *cfg, bool direct_tls)
{
	memset(cfg, 0, sizeof(*cfg));
	snprintf(cfg->jid, sizeof(cfg->jid), "%s", USER_JID);
	snprintf(cfg->password, sizeof(cfg->password), "pencil");
	snprintf(cfg->host, sizeof(cfg->host), "127.0.0.1");
	cfg->port = fake.port;
	cfg->direct_tls = direct_tls;
	cfg->tls_verify = true;
	cfg->allow_plain = true;
	cfg->ping_s = 1;
	cfg->random = fixed_random;
}

/**
 * client_start() - a fresh stream against the armed script
 * @direct_tls: XEP-0368 instead of STARTTLS
 * @tls: the client contexts
 *
 * Return: the xmpp_stream_start() verdict.
 */
static int client_start(bool direct_tls, struct sip_tls *tls)
{
	struct xmpp_stream_env env = {
		.host = proto_host_fake(),
		.dns = &dns,
		.dns_owner = 1,
		.tls = tls,
	};
	struct xmpp_stream_cfg cfg;

	memset(&clog, 0, sizeof(clog));
	client_cfg(&cfg, direct_tls);
	xmpp_stream_init(&xs, &client_cb, NULL, &env);

	return xmpp_stream_start(&xs, &cfg);
}

/**
 * client_end() - stop the stream and release the parser
 */
static void client_end(void)
{
	xmpp_stream_done(&xs);
	loop_pump(SLICE_MS);
}

/**
 * state_seen() - whether a transition to a state was recorded
 * @state: the state
 *
 * Return: the index of the record plus one, 0 when none.
 */
static unsigned int state_seen(enum xmpp_stream_state state)
{
	unsigned int i, n = clog.states < LOG_MAX ? clog.states : LOG_MAX;

	for (i = 0; i < n; i++) {
		if (clog.state[i] == state)
			return i + 1;
	}

	return 0;
}

/**
 * last_reason() - the reason of the last recorded transition
 *
 * Return: the text.
 */
static const char *last_reason(void)
{
	unsigned int n = clog.states < LOG_MAX ? clog.states : LOG_MAX;

	return n ? clog.reason[n - 1] : "";
}

/* ---------------------------------------------------------------- cases */

/**
 * test_full_session() - STARTTLS, SCRAM, bind, sm, carbons, then traffic
 */
static void test_full_session(void)
{
	struct step steps[STEPS_MAX] = {
		STEP("<stream:stream", SERVER_HEADER FEATURES_STARTTLS),
		STEP_ACT("<starttls", PROCEED, STEP_TLS),
		STEP("<stream:stream", SERVER_HEADER FEATURES_SCRAM),
		STEP(auth_scram, challenge),
		STEP(response_final, success_final),
		STEP("<stream:stream", SERVER_HEADER FEATURES_BIND),
		STEP_ID("<bind", BIND_RESULT),
		STEP("<enable xmlns='" XMPP_NS_SM "'/>", ENABLED),
		STEP_ID(XMPP_NS_CARBONS, IQ_RESULT),
		STEP_ID("<query xmlns=\"test:ns\"",
			"<iq type='result' id='%s'><query xmlns='test:ns'/>"
			"</iq>"),
		STEP("<a xmlns=\"" XMPP_NS_SM "\" h=\"3\"/>", NULL),
		STEP("<iq type=\"result\" to=\"" DOMAIN "\" id=\"p1\"/>", NULL),
		STEP("<feature var=\"urn:xmpp:jingle:1\"/>", NULL),
		STEP("<service-unavailable xmlns=\"" XMPP_NS_STANZAS "\"/>",
		     NULL),
		STEP("<iq type=\"result\" to=\"" DOMAIN "\" id=\"h1\"/>", NULL),
		STEP("<r xmlns='" XMPP_NS_SM "'/>",
		     "<a xmlns='" XMPP_NS_SM "' h='7'/>"),
	};
	char iq[256];
	char id[XMPP_ID_MAX];
	int len;

	fake_script_set(steps, 16, false);

	CASE("the stream negotiates its way to ready over starttls");
	CHECK(client_start(false, &cli_tls) == 0);
	CHECK(wait_state(XMPP_ST_READY, WAIT_MS));
	CHECK(fake.step == 9);
	CHECK(xs.tls_up);
	CHECK(xs.authenticated);
	CHECK(xs.sm.enabled);
	CHECK(xs.carbons);
	CHECK(!strcmp(xs.bound.resource, "uvoice"));
	CHECK(state_seen(XMPP_ST_RESOLVE));
	CHECK(state_seen(XMPP_ST_CONNECT));
	CHECK(state_seen(XMPP_ST_STARTTLS));
	CHECK(state_seen(XMPP_ST_TLS));
	CHECK(state_seen(XMPP_ST_SASL));
	CHECK(state_seen(XMPP_ST_BIND));
	CHECK(!state_seen(XMPP_ST_SESSION));
	CHECK(state_seen(XMPP_ST_SM));
	CHECK(state_seen(XMPP_ST_CARBONS));
	CHECK(strstr(fake.all, "<stream:stream from='" USER_JID "' to='"
		     DOMAIN "'") != NULL);
	CHECK(strstr(fake.all, "<resource>uvoice</resource>") != NULL);
	CHECK(xmpp_stream_ready(&xs));

	CASE("an iq of the owner gets its answer through the callback");
	xmpp_stream_id_next(&xs, id, sizeof(id));
	len = snprintf(iq, sizeof(iq),
		       "<iq type=\"get\" id=\"%s\"><query xmlns=\"test:ns\"/>"
		       "</iq>", id);
	CHECK(xmpp_stream_iq_send(&xs, iq, (size_t)len, id, on_iq_reply,
				  NULL) == 0);
	CHECK(wait_step(10, WAIT_MS));
	loop_pump(SLICE_MS);
	CHECK(clog.iq_replies == 1);
	CHECK(!clog.iq_reply_null);
	CHECK(!strcmp(clog.iq_reply_type, "result"));

	CASE("a message reaches the owner, the stream answers the sm request");
	fake_send("<message from='juliet@capulet.example/pc' to='" BOUND_JID
		  "' type='chat'><body>hi</body></message>");
	CHECK(wait_stanzas(1, WAIT_MS));
	CHECK(!strcmp(clog.name[0], "message"));
	CHECK(!strcmp(clog.from[0], "juliet@capulet.example/pc"));
	fake_send("<r xmlns='" XMPP_NS_SM "'/>");
	CHECK(wait_step(11, WAIT_MS));

	CASE("an iq ping is answered inside the stream");
	fake_send("<iq type='get' from='" DOMAIN "' to='" BOUND_JID
		  "' id='p1'><ping xmlns='" XMPP_NS_PING "'/></iq>");
	CHECK(wait_step(12, WAIT_MS));
	CHECK(clog.stanzas == 1);
	CHECK(xs.stat.pings == 1);

	CASE("a disco#info query is answered with the feature set");
	fake_send("<iq type='get' from='" DOMAIN "' to='" BOUND_JID
		  "' id='d1'><query xmlns='" DISCO_NS_INFO "'/></iq>");
	CHECK(wait_step(13, WAIT_MS));
	CHECK(clog.stanzas == 1);

	CASE("an iq the owner does not handle gets service-unavailable");
	fake_send("<iq type='set' from='" DOMAIN "' to='" BOUND_JID
		  "' id='u1'><foo xmlns='x:y'/></iq>");
	CHECK(wait_step(14, WAIT_MS));
	CHECK(clog.stanzas == 2);
	CHECK(!strcmp(clog.name[1], "iq"));

	CASE("an iq the owner handles is answered by the owner");
	fake_send("<iq type='set' from='" DOMAIN "' to='" BOUND_JID
		  "' id='h1'><test xmlns='test:ns'/></iq>");
	CHECK(wait_step(15, WAIT_MS));
	CHECK(clog.stanzas == 3);

	CASE("silence makes the stream ask for an ack, the answer is kept");
	CHECK(wait_step(16, WAIT_MS));
	loop_pump(SLICE_MS);
	CHECK(xs.sm.acked == 7);
	CHECK(xs.stat.pings == 2);
	CHECK(!xs.ping_waiting);

	CASE("a send outside ready is refused, a peer close starts a backoff");
	fake_send("<message from='x@y' type='chat'><body>bye</body></message>");
	CHECK(wait_stanzas(4, WAIT_MS));
	fake_conn_close();
	CHECK(wait_state(XMPP_ST_BACKOFF, WAIT_MS));
	CHECK(!strcmp(xs.reason, "peer closed the connection"));
	CHECK(xmpp_stream_send(&xs, "<x/>", 4) == -ENOTCONN);
	CHECK(xs.fails == 1);
	CHECK(xs.ufd.fd < 0);

	CASE("the backoff ends in a new connection");
	CHECK(wait_state(XMPP_ST_FEATURES, XMPP_BACKOFF_MS + WAIT_MS));
	CHECK(fake.accepts == 2);
	CHECK(wait_step(1, WAIT_MS));
	loop_pump(SLICE_MS * 2);

	CASE("a stop closes the document and reconnects no more");
	xmpp_stream_stop(&xs);
	CHECK(xs.state == XMPP_ST_IDLE);
	CHECK(!strcmp(last_reason(), "stopped"));
	loop_pump(SLICE_MS * 5);
	CHECK(xs.state == XMPP_ST_IDLE);
	CHECK(strstr(fake.all, "</stream:stream>") != NULL);
	client_end();
}

/**
 * test_direct_tls_plain() - XEP-0368 with PLAIN and a mandatory session
 */
static void test_direct_tls_plain(void)
{
	struct step steps[STEPS_MAX] = {
		STEP("<stream:stream", SERVER_HEADER FEATURES_PLAIN),
		STEP(auth_plain, SASL_SUCCESS_EMPTY),
		STEP("<stream:stream", SERVER_HEADER
		     "<stream:features><bind xmlns='" XMPP_NS_BIND "'/>"
		     "<session xmlns='" XMPP_NS_SESSION "'/>"
		     "</stream:features>"),
		STEP_ID("<bind", BIND_RESULT),
		STEP_ID("<session xmlns=\"" XMPP_NS_SESSION "\"/>", IQ_RESULT),
		STEP_ID(XMPP_NS_CARBONS,
			"<iq type='error' id='%s'><error type='cancel'>"
			"<feature-not-implemented xmlns='" XMPP_NS_STANZAS "'/>"
			"</error></iq>"),
		STEP_ID("<iq type=\"get\" to=\"" DOMAIN "\" id=\"", IQ_RESULT),
	};

	fake_script_set(steps, 7, true);

	CASE("direct tls, plain, a session request and no carbons still end ready");
	CHECK(client_start(true, &cli_tls) == 0);
	CHECK(wait_state(XMPP_ST_READY, WAIT_MS));
	CHECK(fake.step == 6);
	CHECK(!state_seen(XMPP_ST_STARTTLS));
	CHECK(state_seen(XMPP_ST_SESSION));
	CHECK(!state_seen(XMPP_ST_SM));
	CHECK(!xs.sm.enabled);
	CHECK(!xs.carbons);

	CASE("without stream management the liveness check is an iq ping");
	CHECK(wait_step(7, WAIT_MS));
	loop_pump(SLICE_MS * 3);
	CHECK(xs.stat.pings == 1);
	CHECK(!xs.ping_waiting);
	CHECK(xs.state == XMPP_ST_READY);
	client_end();
}

/**
 * test_refusals() - the server shapes that end an attempt
 */
static void test_refusals(void)
{
	struct step no_tls[] = {
		STEP("<stream:stream", SERVER_HEADER FEATURES_NO_TLS),
	};
	struct step bad_password[] = {
		STEP("<stream:stream", SERVER_HEADER FEATURES_STARTTLS),
		STEP_ACT("<starttls", PROCEED, STEP_TLS),
		STEP("<stream:stream", SERVER_HEADER FEATURES_SCRAM),
		STEP(auth_scram, SASL_FAILURE),
	};
	struct step stream_error[] = {
		STEP("<stream:stream", SERVER_HEADER
		     "<stream:error><conflict xmlns='" XMPP_NS_STREAMS "'/>"
		     "</stream:error></stream:stream>"),
	};
	struct step plain_only[] = {
		STEP("<stream:stream", SERVER_HEADER FEATURES_STARTTLS),
		STEP_ACT("<starttls", PROCEED, STEP_TLS),
		STEP("<stream:stream", SERVER_HEADER FEATURES_PLAIN),
	};
	struct step garbage[] = {
		STEP("<stream:stream", SERVER_HEADER "<!-- no -->"),
	};
	struct sip_tls_cfg wrong = { .ca_path = fix_other_ca, .verify = true };
	struct sip_tls other_tls;
	struct xmpp_stream_cfg cfg;

	CASE("a server without starttls is refused");
	fake_script_set(no_tls, 1, false);
	CHECK(client_start(false, &cli_tls) == 0);
	CHECK(wait_state(XMPP_ST_BACKOFF, WAIT_MS));
	CHECK(!strcmp(xs.reason, "server offers no starttls"));
	client_end();

	CASE("a refused password jumps to the long backoff");
	fake_script_set(bad_password, 4, false);
	CHECK(client_start(false, &cli_tls) == 0);
	CHECK(wait_state(XMPP_ST_BACKOFF, WAIT_MS));
	CHECK(!strcmp(xs.reason, "sasl: not-authorized"));
	CHECK(xs.fails == XMPP_AUTH_FAILS + 1);
	CHECK(xs.backoff_until - mono_ms() > 8 * XMPP_BACKOFF_MS);
	CHECK(phf.n[PHF_LOG] >= 2);
	client_end();

	CASE("a stream error names its condition");
	fake_script_set(stream_error, 1, false);
	CHECK(client_start(false, &cli_tls) == 0);
	CHECK(wait_state(XMPP_ST_BACKOFF, WAIT_MS));
	CHECK(!strcmp(xs.reason, "stream error: conflict"));
	client_end();

	CASE("plain is refused when the configuration forbids it");
	fake_script_set(plain_only, 3, false);
	CHECK(client_start(false, &cli_tls) == 0);
	xs.cfg.allow_plain = false;
	CHECK(wait_state(XMPP_ST_BACKOFF, WAIT_MS));
	CHECK(!strcmp(xs.reason, "sasl: no usable mechanism"));
	client_end();

	CASE("a comment in the stream fails it");
	fake_script_set(garbage, 1, false);
	CHECK(client_start(false, &cli_tls) == 0);
	CHECK(wait_state(XMPP_ST_BACKOFF, WAIT_MS));
	CHECK(!strncmp(xs.reason, "xml: ", 5));
	client_end();

	CASE("a certificate of another authority fails the handshake");
	sip_tls_init(&other_tls);
	CHECK(sip_tls_ctx_load(&other_tls, &wrong));
	fake_script_set(bad_password, 2, false);
	CHECK(client_start(false, &other_tls) == 0);
	CHECK(wait_state(XMPP_ST_BACKOFF, WAIT_MS));
	CHECK(!strncmp(xs.reason, "tls: ", 5));
	client_end();
	sip_tls_done(&other_tls);

	CASE("the start refuses a bad jid, an empty password and a second start");
	client_cfg(&cfg, false);
	snprintf(cfg.jid, sizeof(cfg.jid), "nolocal.example");
	CHECK(xmpp_stream_start(&xs, &cfg) == -EINVAL);
	client_cfg(&cfg, false);
	cfg.password[0] = 0;
	CHECK(xmpp_stream_start(&xs, &cfg) == -EINVAL);
	fake_script_set(no_tls, 1, false);
	client_cfg(&cfg, false);
	CHECK(xmpp_stream_start(&xs, &cfg) == 0);
	CHECK(xmpp_stream_start(&xs, &cfg) == -EBUSY);
	client_end();
}

/**
 * test_names() - the state words
 */
static void test_names(void)
{
	CASE("every state has a word");
	CHECK(!strcmp(xmpp_stream_state_name(XMPP_ST_IDLE), "idle"));
	CHECK(!strcmp(xmpp_stream_state_name(XMPP_ST_READY), "ready"));
	CHECK(!strcmp(xmpp_stream_state_name(XMPP_ST_BACKOFF), "backoff"));
	CHECK(!strcmp(xmpp_stream_state_name(XMPP_ST_MAX), "unknown"));
}

int main(void)
{
	struct sip_tls_cfg client = { .ca_path = fix_ca, .verify = true };
	struct sip_tls_cfg server = { .cert_path = fix_cert, .key_path = fix_key };
	struct sip_dns_cb dcb = { .done = dns_done };
	struct sip_dns_cfg dcfg;
	bool fixtures_ok = fixtures_take();

	SUITE_CHECK(fixtures_ok);
	if (!fixtures_ok) {
		printf("    set UVOICE_TLS_FIXTURES to test/unit/tls\n");
		return tu_summary("test_xmpp_stream");
	}

	uloop_init();
	proto_host_fake_reset();
	sasl_texts_build();

	sip_tls_init(&cli_tls);
	sip_tls_init(&srv_tls);
	SUITE_CHECK(sip_tls_ctx_load(&cli_tls, &client));
	SUITE_CHECK(sip_tls_ctx_load(&srv_tls, &server));
	SUITE_CHECK(sip_tls_server_ready(&srv_tls));

	sip_dns_cfg_default(&dcfg);
	dcfg.srv_fallback = true;
	sip_dns_init(&dns, &dcfg, &dcb, NULL);

	SUITE_CHECK(fake_listen());

	test_names();
	test_full_session();
	test_direct_tls_plain();
	test_refusals();

	fake_conn_close();
	sip_dns_done(&dns);
	sip_tls_done(&cli_tls);
	sip_tls_done(&srv_tls);
	uloop_done();

	return tu_summary("test_xmpp_stream");
}
