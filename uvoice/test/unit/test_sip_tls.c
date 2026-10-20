// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the TLS wrapper (src/proto/sip/tls.c).
 *
 * Both ends in one process. Every handshake except one runs over a non blocking
 * socketpair(AF_UNIX, SOCK_STREAM), with the client on one end and the server
 * on the other. A bounded loop calls sip_tls_conn_io() on each side in turn.
 * One struct sip_tls carries both contexts, as in the daemon. A case that
 * changes the configuration therefore changes it for both ends at once, and the
 * configurations of the two ends never differ.
 *
 * The IPv6 case. One case takes a connected pair over the IPv6 loopback
 * instead. The stream transport opens connections in that family, and the
 * record layer runs on them. The case skips with the reason that
 * tu_v6_loopback() reports when the host has no ::1.
 *
 * Fixtures. The committed PEM files test/unit/tls/{ca,server,other-ca}.* are
 * the fixtures, so no test run calls openssl. The script test/unit/tls/gen.sh
 * made them. The suite reads the environment variable UVOICE_TLS_FIXTURES first
 * and the compile define UVOICE_TLS_FIXTURE_DIR second. A run that cannot find
 * them fails the suite through a suite check, which prints the failed
 * expression. A silent skip would report a pass for a suite that verifies
 * nothing about certificates.
 *
 * The OpenSSL queries. The header sip/tls.h keeps the library out, but the
 * suite includes the library on purpose. The minimum protocol version, the
 * negotiated version and the server_name that the server received are
 * properties of the OpenSSL objects. Only a query to the library can assert
 * that tls.c set them. sip/tls.h declares struct ssl_st and struct ssl_ctx_st,
 * which OpenSSL defines, so its pointers pass into the library with no cast.
 */
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/ssl.h>

#include "proto/sip/tap.h"
#include "proto/sip/tls.h"
#include "proto_host_fake.h"
#include "test_util.h"

/* the handshake needs a handful of turns; anything past this is a stall */
#define PUMP_MAX	64

/* more than the socket buffer, so one write cannot place all of it */
#define BULK_LEN	(256 * 1024)

/* the client send buffer of the bulk case, small enough to fill */
#define BULK_SNDBUF	32768

static char fix_ca[PATH_MAX];
static char fix_ca_key[PATH_MAX];
static char fix_other_ca[PATH_MAX];
static char fix_cert[PATH_MAX];
static char fix_key[PATH_MAX];

/* shorter than keylog so the file name always fits behind it */
static char keydir[PATH_MAX - 16];
static char keylog[PATH_MAX];

static char bulk[BULK_LEN];
static char sink[16384];

/* ------------------------------------------------------------- fixtures */

static bool fixture_path(char *dst, size_t len, const char *dir, const char *name)
{
	snprintf(dst, len, "%s/%s", dir, name);

	return access(dst, R_OK) == 0;
}

static bool fixtures_take(void)
{
	const char *dir = getenv("UVOICE_TLS_FIXTURES");
	bool ok;

#ifdef UVOICE_TLS_FIXTURE_DIR
	if (!dir)
		dir = UVOICE_TLS_FIXTURE_DIR;
#endif

	SUITE_CHECK(dir != NULL);

	if (!dir) {
		printf("    set UVOICE_TLS_FIXTURES to test/unit/tls\n");
		return false;
	}

	ok = fixture_path(fix_ca, sizeof(fix_ca), dir, "ca.crt");
	ok &= fixture_path(fix_ca_key, sizeof(fix_ca_key), dir, "ca.key");
	ok &= fixture_path(fix_other_ca, sizeof(fix_other_ca), dir, "other-ca.crt");
	ok &= fixture_path(fix_cert, sizeof(fix_cert), dir, "server.crt");
	ok &= fixture_path(fix_key, sizeof(fix_key), dir, "server.key");
	SUITE_CHECK(ok);

	return ok;
}

/* ------------------------------------------------------------- the pair */

struct pair {
	struct sip_tls_conn cli;
	struct sip_tls_conn srv;
	int fd[2];
	enum sip_tls_io cli_io;		/* the client's last answer, for a refusal */
};

static void cfg_server(struct sip_tls_cfg *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->cert_path = fix_cert;
	cfg->key_path = fix_key;
}

static void sock_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);

	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static bool pair_fail(struct pair *p)
{
	close(p->fd[0]);
	close(p->fd[1]);
	p->fd[0] = -1;
	p->fd[1] = -1;

	return false;
}

static bool pair_open(struct sip_tls *t, struct pair *p, const char *sni)
{
	memset(p, 0, sizeof(*p));
	p->fd[0] = -1;
	p->fd[1] = -1;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, p->fd))
		return false;

	sock_nonblock(p->fd[0]);
	sock_nonblock(p->fd[1]);

	if (!sip_tls_conn_start(t, &p->cli, p->fd[0], false, sni))
		return pair_fail(p);

	if (!sip_tls_conn_start(t, &p->srv, p->fd[1], true, NULL))
		return pair_fail(p);

	return true;
}

/* a connected pair over ::1, so the record layer runs on a real TCP socket */
static bool sock_pair6(int fd[2])
{
	struct sockaddr_in6 sa;
	socklen_t slen = sizeof(sa);
	int lfd, cfd, afd;
	int on = 1;

	memset(&sa, 0, sizeof(sa));
	sa.sin6_family = AF_INET6;
	sa.sin6_addr = in6addr_loopback;

	lfd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (lfd < 0)
		return false;

	if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) || listen(lfd, 1) ||
	    getsockname(lfd, (struct sockaddr *)&sa, &slen)) {
		close(lfd);
		return false;
	}

	cfd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (cfd < 0) {
		close(lfd);
		return false;
	}

	/* the listener is already up on the loopback, so the connect and the
	 * accept behind it both complete at once */
	afd = connect(cfd, (struct sockaddr *)&sa, sizeof(sa)) ?
	      -1 : accept(lfd, NULL, NULL);
	close(lfd);

	if (afd < 0) {
		close(cfd);
		return false;
	}

	/*
	 * TCP_NODELAY, because the pump below has no clock: Nagle would hold a
	 * record as short as these until the peer's delayed acknowledgement,
	 * and the bounded read loop would come back empty.
	 */
	setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
	setsockopt(afd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
	sock_nonblock(cfd);
	sock_nonblock(afd);
	fd[0] = cfd;
	fd[1] = afd;

	return true;
}

static bool pair_open6(struct sip_tls *t, struct pair *p, const char *sni)
{
	memset(p, 0, sizeof(*p));
	p->fd[0] = -1;
	p->fd[1] = -1;

	if (!sock_pair6(p->fd))
		return false;

	if (!sip_tls_conn_start(t, &p->cli, p->fd[0], false, sni))
		return pair_fail(p);

	if (!sip_tls_conn_start(t, &p->srv, p->fd[1], true, NULL))
		return pair_fail(p);

	return true;
}

static void pair_close(struct pair *p)
{
	sip_tls_conn_close(&p->cli);
	sip_tls_conn_close(&p->srv);

	if (p->fd[0] >= 0)
		close(p->fd[0]);

	if (p->fd[1] >= 0)
		close(p->fd[1]);

	p->fd[0] = -1;
	p->fd[1] = -1;
}

static enum sip_tls_io conn_step(struct sip_tls_conn *c)
{
	if (sip_tls_conn_up(c))
		return SIP_TLS_OK;

	return sip_tls_conn_io(c);
}

/*
 * Drive both handshakes. SIP_TLS_OK means both ends are up; SIP_TLS_ERR means
 * one of them refused, and cli_io says whether the client was that one. A run
 * that never settles answers SIP_TLS_WANT_READ, which no case expects.
 */
static enum sip_tls_io pair_handshake(struct pair *p)
{
	enum sip_tls_io srv;
	int i;

	for (i = 0; i < PUMP_MAX; i++) {
		p->cli_io = conn_step(&p->cli);
		srv = conn_step(&p->srv);

		if (p->cli_io == SIP_TLS_ERR || srv == SIP_TLS_ERR)
			return SIP_TLS_ERR;

		if (sip_tls_conn_up(&p->cli) && sip_tls_conn_up(&p->srv))
			return SIP_TLS_OK;
	}

	return SIP_TLS_WANT_READ;
}

static bool read_exact(struct sip_tls_conn *c, char *buf, size_t len)
{
	size_t got = 0, n = 0;
	int i;

	for (i = 0; i < PUMP_MAX && got < len; i++) {
		if (sip_tls_conn_read(c, buf + got, len - got, &n) == SIP_TLS_ERR)
			return false;

		got += n;
	}

	return got == len;
}

static bool round_trip(struct sip_tls_conn *from, struct sip_tls_conn *to, const char *text)
{
	char buf[128];
	size_t len = strlen(text);
	size_t n = 0;

	if (sip_tls_conn_write(from, text, len, &n) != SIP_TLS_OK || n != len)
		return false;

	if (!read_exact(to, buf, len))
		return false;

	return !memcmp(buf, text, len);
}

/* ---------------------------------------------------------- bulk helpers */

static uint32_t fnv(uint32_t hash, const void *buf, size_t len)
{
	const unsigned char *p = buf;
	size_t i;

	for (i = 0; i < len; i++) {
		hash ^= p[i];
		hash *= 16777619u;
	}

	return hash;
}

static size_t push(struct sip_tls_conn *c, const char *buf, size_t len, bool *shorted)
{
	size_t n = 0;
	enum sip_tls_io io = sip_tls_conn_write(c, buf, len, &n);

	if (io == SIP_TLS_WANT_WRITE || (io == SIP_TLS_OK && n < len))
		*shorted = true;

	return n;
}

static size_t drain(struct sip_tls_conn *c, uint32_t *hash)
{
	size_t total = 0, n = 0;
	int i;

	for (i = 0; i < PUMP_MAX; i++) {
		if (sip_tls_conn_read(c, sink, sizeof(sink), &n) != SIP_TLS_OK || !n)
			break;

		*hash = fnv(*hash, sink, n);
		total += n;
	}

	return total;
}

/* -------------------------------------------------------- key log helpers */

static long file_size(const char *path)
{
	struct stat st;

	if (stat(path, &st))
		return -1;

	return (long)st.st_size;
}

static int file_mode(const char *path)
{
	struct stat st;

	if (stat(path, &st))
		return -1;

	return (int)(st.st_mode & 07777);
}

static size_t keylog_lines(const char *path, bool *ok)
{
	char line[4096];
	size_t count = 0;
	FILE *f = fopen(path, "r");

	if (!f) {
		*ok = false;
		return 0;
	}

	while (fgets(line, sizeof(line), f)) {
		count++;
		if (!strncmp(line, "CLIENT_", 7) || !strncmp(line, "SERVER_", 7) ||
		    !strncmp(line, "EXPORTER_", 9))
			continue;

		*ok = false;
	}

	fclose(f);

	return count;
}

/* a log the previous run left behind, of a size the caller picks */
static bool file_fill(const char *path, size_t len)
{
	char pad[256];
	FILE *f = fopen(path, "w");
	size_t left = len, n;

	if (!f)
		return false;

	memset(pad, 'x', sizeof(pad));

	while (left) {
		n = left < sizeof(pad) ? left : sizeof(pad);
		if (fwrite(pad, 1, n, f) != n)
			break;

		left -= n;
	}

	fclose(f);

	return file_size(path) == (long)len;
}

static bool keylog_dir_make(void)
{
	const char *tmp = getenv("TMPDIR");

	snprintf(keydir, sizeof(keydir), "%s/uvoice-tls-XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
	if (!mkdtemp(keydir))
		return false;

	snprintf(keylog, sizeof(keylog), "%s/ssl.keylog", keydir);

	return true;
}

static void keylog_dir_drop(void)
{
	if (!keydir[0])
		return;

	unlink(keylog);
	rmdir(keydir);
}

static bool handshake_once(struct sip_tls *t)
{
	struct pair p;
	bool ok;

	if (!pair_open(t, &p, NULL))
		return false;

	ok = pair_handshake(&p) == SIP_TLS_OK;
	pair_close(&p);

	return ok;
}

/* ---------------------------------------------------------------- cases */

static void t_constants(void)
{
	CASE("tls: the frozen version, keylog and result constants");
	CHECK_UINT(SIP_TLS_V1_2, 0x0303);
	CHECK_UINT(SIP_TLS_V1_3, 0x0304);
	CHECK_UINT(SIP_TLS_KEYLOG_MAX, 5000);
	CHECK_UINT(SIP_TLS_OK, 0);
	CHECK(!strcmp(SIP_TLS_CERT_DEFAULT, SIP_TLS_DIR_DEFAULT "/cert.pem"));
	CHECK(!strcmp(SIP_TLS_KEY_DEFAULT, SIP_TLS_DIR_DEFAULT "/key.pem"));
	CHECK(!strcmp(SIP_TLS_CA_DEFAULT, SIP_TLS_DIR_DEFAULT "/ca.crt"));
}

static void t_names(void)
{
	CASE("tls: every result has a name and anything else is unknown");
	CHECK(!strcmp(sip_tls_io_name(SIP_TLS_OK), "ok"));
	CHECK(!strcmp(sip_tls_io_name(SIP_TLS_WANT_READ), "want_read"));
	CHECK(!strcmp(sip_tls_io_name(SIP_TLS_WANT_WRITE), "want_write"));
	CHECK(!strcmp(sip_tls_io_name(SIP_TLS_ERR), "error"));
	CHECK(!strcmp(sip_tls_io_name((enum sip_tls_io)42), "unknown"));
}

static void t_life(void)
{
	struct sip_tls t;

	CASE("tls: init clears the struct and done is idempotent");
	memset(&t, 0xa5, sizeof(t));
	sip_tls_init(&t);
	CHECK(!sip_tls_client_ready(&t));
	CHECK(!sip_tls_server_ready(&t));
	CHECK(!t.loaded);
	CHECK(!t.keylog_bytes);
	CHECK(!strcmp(sip_tls_error(&t), ""));

	/* done on a struct that never loaded, then again after that */
	sip_tls_done(&t);
	sip_tls_done(&t);
	CHECK(!sip_tls_client_ready(&t));
	CHECK(!sip_tls_server_ready(&t));
}

static void t_load_client_only(void)
{
	struct sip_tls_cfg cfg;
	struct sip_tls t;

	CASE("tls: a load with no certificate pair is client only and not an error");
	memset(&cfg, 0, sizeof(cfg));
	sip_tls_init(&t);
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK(sip_tls_client_ready(&t));
	CHECK(!sip_tls_server_ready(&t));
	CHECK(!strcmp(sip_tls_error(&t), ""));
	CHECK(t.loaded);
	sip_tls_done(&t);
}

static void t_load_pair(void)
{
	struct sip_tls_cfg cfg;
	struct sip_tls t;

	CASE("tls: a load with the certificate pair builds the server context");
	cfg_server(&cfg);
	sip_tls_init(&t);
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK(sip_tls_client_ready(&t));
	CHECK(sip_tls_server_ready(&t));
	CHECK(!strcmp(sip_tls_error(&t), ""));

	CASE("tls: a second load replaces both contexts");
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK(sip_tls_client_ready(&t));
	CHECK(sip_tls_server_ready(&t));
	CHECK(!strcmp(sip_tls_error(&t), ""));
	sip_tls_done(&t);
}

static void t_load_half_pair(void)
{
	struct sip_tls_cfg cfg;
	struct sip_tls t;

	CASE("tls: a certificate without a key stays client only with a reason");
	memset(&cfg, 0, sizeof(cfg));
	cfg.cert_path = fix_cert;
	sip_tls_init(&t);
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK(sip_tls_client_ready(&t));
	CHECK(!sip_tls_server_ready(&t));
	CHECK(strcmp(sip_tls_error(&t), "") != 0);

	CASE("tls: a key without a certificate stays client only with a reason");
	memset(&cfg, 0, sizeof(cfg));
	cfg.key_path = fix_key;
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK(sip_tls_client_ready(&t));
	CHECK(!sip_tls_server_ready(&t));
	CHECK(strcmp(sip_tls_error(&t), "") != 0);
	sip_tls_done(&t);
}

static void t_load_bad_paths(void)
{
	struct sip_tls_cfg cfg;
	struct sip_tls t;

	CASE("tls: a certificate path that does not exist stays client only");
	memset(&cfg, 0, sizeof(cfg));
	cfg.cert_path = "/nonexistent/uvoice-test/cert.pem";
	cfg.key_path = fix_key;
	sip_tls_init(&t);
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK(sip_tls_client_ready(&t));
	CHECK(!sip_tls_server_ready(&t));
	CHECK(strcmp(sip_tls_error(&t), "") != 0);

	CASE("tls: a key that does not match the certificate stays client only");
	memset(&cfg, 0, sizeof(cfg));
	cfg.cert_path = fix_cert;
	cfg.key_path = fix_ca_key;		/* the CA's key, not the server's */
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK(!sip_tls_server_ready(&t));
	CHECK(strcmp(sip_tls_error(&t), "") != 0);

	/*
	 * A verifying client with no trust store cannot verify anything, so
	 * this is the one load failure that is fatal: it takes the client
	 * context with it and the whole load answers false.
	 */
	CASE("tls: a verifying client with an unreadable trust store fails the load");
	memset(&cfg, 0, sizeof(cfg));
	cfg.verify = true;
	cfg.ca_path = "/nonexistent/uvoice-test/ca.crt";
	CHECK(!sip_tls_ctx_load(&t, &cfg));
	CHECK(!sip_tls_client_ready(&t));
	CHECK(!sip_tls_server_ready(&t));
	CHECK(!t.loaded);
	CHECK(strcmp(sip_tls_error(&t), "") != 0);
	sip_tls_done(&t);
}

static void t_version_floor(void)
{
	struct sip_tls_cfg cfg;
	struct sip_tls t;

	CASE("tls: an unset minimum version takes the TLS 1.2 floor on both contexts");
	cfg_server(&cfg);
	sip_tls_init(&t);
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK_UINT(SSL_CTX_get_min_proto_version(t.client), TLS1_2_VERSION);
	CHECK_UINT(SSL_CTX_get_min_proto_version(t.server), TLS1_2_VERSION);

	CASE("tls: a minimum version below TLS 1.2 is raised to it");
	cfg.min_version = 0x0301;		/* TLS 1.0 */
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK_UINT(SSL_CTX_get_min_proto_version(t.client), TLS1_2_VERSION);
	CHECK_UINT(SSL_CTX_get_min_proto_version(t.server), TLS1_2_VERSION);

	CASE("tls: a minimum version of TLS 1.3 is honored");
	cfg.min_version = SIP_TLS_V1_3;
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK_UINT(SSL_CTX_get_min_proto_version(t.client), TLS1_3_VERSION);
	CHECK_UINT(SSL_CTX_get_min_proto_version(t.server), TLS1_3_VERSION);
	sip_tls_done(&t);
}

/*
 * The three context flags that the stream engine depends on. Nothing in a
 * handshake over a socket pair can tell whether they were set. The library
 * getters are therefore the only check. Without this case, a revert of the two
 * ctx_common() calls stays green.
 */
static void t_ctx_flags(void)
{
	struct sip_tls_cfg cfg;
	struct sip_tls t;

	CASE("tls: both contexts take the partial write modes and refuse renegotiation");
	cfg_server(&cfg);
	sip_tls_init(&t);
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK(sip_tls_server_ready(&t));

	CHECK((SSL_CTX_get_mode(t.client) & SSL_MODE_ENABLE_PARTIAL_WRITE) != 0);
	CHECK((SSL_CTX_get_mode(t.client) & SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER) != 0);
	CHECK((SSL_CTX_get_mode(t.server) & SSL_MODE_ENABLE_PARTIAL_WRITE) != 0);
	CHECK((SSL_CTX_get_mode(t.server) & SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER) != 0);

	CHECK((SSL_CTX_get_options(t.client) & SSL_OP_NO_RENEGOTIATION) != 0);
	CHECK((SSL_CTX_get_options(t.server) & SSL_OP_NO_RENEGOTIATION) != 0);
	sip_tls_done(&t);
}

static void t_handshake(void)
{
	struct sip_tls_cfg cfg;
	struct sip_tls t;
	struct pair p;
	size_t n = 42;

	CASE("tls: a handshake over a socket pair brings both ends up");
	cfg_server(&cfg);
	sip_tls_init(&t);
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK(pair_open(&t, &p, NULL));

	/* the ClientHello went out and nothing came back yet */
	CHECK_UINT(sip_tls_conn_io(&p.cli), SIP_TLS_WANT_READ);
	CHECK(!sip_tls_conn_up(&p.cli));

	CHECK_UINT(pair_handshake(&p), SIP_TLS_OK);
	CHECK(sip_tls_conn_up(&p.cli));
	CHECK(sip_tls_conn_up(&p.srv));
	CHECK(!strcmp(sip_tls_conn_error(&p.cli), ""));
	CHECK(!strcmp(sip_tls_conn_error(&p.srv), ""));
	CHECK_UINT(SSL_version(p.cli.ssl), TLS1_3_VERSION);
	CHECK_UINT(SSL_version(p.srv.ssl), TLS1_3_VERSION);

	CASE("tls: application bytes cross in both directions");
	CHECK(round_trip(&p.cli, &p.srv, "REGISTER sip:example.test SIP/2.0\r\n\r\n"));
	CHECK(round_trip(&p.srv, &p.cli, "SIP/2.0 200 OK\r\n\r\n"));

	CASE("tls: a read with nothing to read wants a read");
	CHECK_UINT(sip_tls_conn_read(&p.srv, sink, sizeof(sink), &n), SIP_TLS_WANT_READ);
	CHECK_UINT(n, 0);

	pair_close(&p);
	sip_tls_done(&t);
}

static void t_handshake6(void)
{
	struct sip_tls_cfg cfg;
	struct sip_tls t;
	struct pair p;
	char why[128];

	CASE("tls6: a handshake over a ::1 connection comes up and carries a "
	     "message");
	if (!tu_v6_loopback(why, sizeof(why))) {
		tu_skip(why);

		return;
	}

	cfg_server(&cfg);
	sip_tls_init(&t);
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK(pair_open6(&t, &p, NULL));
	CHECK_UINT(pair_handshake(&p), SIP_TLS_OK);
	CHECK(sip_tls_conn_up(&p.cli));
	CHECK(sip_tls_conn_up(&p.srv));
	CHECK(p.cli.ssl && SSL_version(p.cli.ssl) == TLS1_3_VERSION);
	CHECK(round_trip(&p.cli, &p.srv,
			 "REGISTER sip:example.test SIP/2.0\r\n\r\n"));
	CHECK(round_trip(&p.srv, &p.cli, "SIP/2.0 200 OK\r\n\r\n"));

	pair_close(&p);
	sip_tls_done(&t);
}

static void t_verify(void)
{
	struct sip_tls_cfg cfg;
	struct sip_tls t;
	struct pair p;

	CASE("tls: a verifying client accepts the CA that signed the server");
	cfg_server(&cfg);
	cfg.verify = true;
	cfg.ca_path = fix_ca;
	sip_tls_init(&t);
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK(pair_open(&t, &p, NULL));
	CHECK_UINT(pair_handshake(&p), SIP_TLS_OK);
	pair_close(&p);

	CASE("tls: a verifying client refuses a chain the other CA did not sign");
	cfg.ca_path = fix_other_ca;
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK(pair_open(&t, &p, NULL));
	CHECK_UINT(pair_handshake(&p), SIP_TLS_ERR);
	CHECK_UINT(p.cli_io, SIP_TLS_ERR);
	CHECK(!sip_tls_conn_up(&p.cli));
	CHECK(strcmp(sip_tls_conn_error(&p.cli), "") != 0);
	pair_close(&p);

	CASE("tls: verify off accepts the same chain the verifying client refused");
	cfg.verify = false;
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK(pair_open(&t, &p, NULL));
	CHECK_UINT(pair_handshake(&p), SIP_TLS_OK);
	pair_close(&p);

	/*
	 * See sip/tls.h "No hostname verification": chain verification never
	 * checks the peer name, so a certificate for sip.example.test serves a
	 * connection that asked for wrong.name.test. This case exists to fail
	 * the day someone adds SSL_set1_host().
	 */
	CASE("tls: a verified chain with the wrong name still comes up");
	cfg.verify = true;
	cfg.ca_path = fix_ca;
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK(pair_open(&t, &p, "wrong.name.test"));
	CHECK_UINT(pair_handshake(&p), SIP_TLS_OK);
	CHECK(sip_tls_conn_up(&p.cli));
	pair_close(&p);

	sip_tls_done(&t);
}

static void t_sni(void)
{
	struct sip_tls_cfg cfg;
	struct sip_tls t;
	struct pair p;
	const char *seen;

	CASE("tls: the server sees the name the client sent");
	cfg_server(&cfg);
	sip_tls_init(&t);
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK(pair_open(&t, &p, "sip.example.test"));
	CHECK_UINT(pair_handshake(&p), SIP_TLS_OK);
	seen = SSL_get_servername(p.srv.ssl, TLSEXT_NAMETYPE_host_name);
	CHECK(seen != NULL);
	CHECK(seen && !strcmp(seen, "sip.example.test"));
	pair_close(&p);

	CASE("tls: no name means no server_name extension");
	CHECK(pair_open(&t, &p, NULL));
	CHECK_UINT(pair_handshake(&p), SIP_TLS_OK);
	CHECK(SSL_get_servername(p.srv.ssl, TLSEXT_NAMETYPE_host_name) == NULL);
	pair_close(&p);

	CASE("tls: an empty name means no server_name extension");
	CHECK(pair_open(&t, &p, ""));
	CHECK_UINT(pair_handshake(&p), SIP_TLS_OK);
	CHECK(SSL_get_servername(p.srv.ssl, TLSEXT_NAMETYPE_host_name) == NULL);
	pair_close(&p);

	sip_tls_done(&t);
}

static void t_bulk(void)
{
	struct sip_tls_cfg cfg;
	struct sip_tls t;
	struct pair p;
	uint32_t want, got_hash = 2166136261u;
	size_t sent = 0, got = 0;
	int sndbuf = BULK_SNDBUF;
	bool shorted = false;
	size_t i;
	int step;

	CASE("tls: a write larger than the socket buffer is short and loses no byte");
	for (i = 0; i < sizeof(bulk); i++)
		bulk[i] = (char)(i * 31u + (i >> 8));

	want = fnv(2166136261u, bulk, sizeof(bulk));

	cfg_server(&cfg);
	sip_tls_init(&t);
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK(pair_open(&t, &p, NULL));
	setsockopt(p.fd[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
	CHECK_UINT(pair_handshake(&p), SIP_TLS_OK);

	for (step = 0; step < 20000; step++) {
		if (sent < sizeof(bulk))
			sent += push(&p.cli, bulk + sent, sizeof(bulk) - sent, &shorted);

		got += drain(&p.srv, &got_hash);

		if (sent == sizeof(bulk) && got == sizeof(bulk))
			break;
	}

	CHECK(shorted);
	CHECK_UINT(sent, sizeof(bulk));
	CHECK_UINT(got, sizeof(bulk));
	CHECK_UINT(got_hash, want);
	CHECK(!strcmp(sip_tls_conn_error(&p.cli), ""));

	pair_close(&p);
	sip_tls_done(&t);
}

static void t_shutdown(void)
{
	struct sip_tls_cfg cfg;
	struct sip_tls t;
	struct pair p;
	size_t n = 42;

	CASE("tls: a close_notify reads as an orderly end of the stream");
	cfg_server(&cfg);
	sip_tls_init(&t);
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK(pair_open(&t, &p, NULL));
	CHECK_UINT(pair_handshake(&p), SIP_TLS_OK);

	sip_tls_conn_close(&p.cli);
	CHECK(!sip_tls_conn_up(&p.cli));
	CHECK_UINT(sip_tls_conn_read(&p.srv, sink, sizeof(sink), &n), SIP_TLS_OK);
	CHECK_UINT(n, 0);

	/* the socket belongs to the caller and the close left it open */
	CHECK(fcntl(p.fd[0], F_GETFD) != -1);

	CASE("tls: a second close is a no-op");
	sip_tls_conn_close(&p.cli);
	CHECK(!sip_tls_conn_up(&p.cli));
	CHECK(fcntl(p.fd[0], F_GETFD) != -1);
	pair_close(&p);

	CASE("tls: a socket that goes without close_notify is an error");
	CHECK(pair_open(&t, &p, NULL));
	CHECK_UINT(pair_handshake(&p), SIP_TLS_OK);
	close(p.fd[0]);
	p.fd[0] = -1;
	n = 42;
	CHECK_UINT(sip_tls_conn_read(&p.srv, sink, sizeof(sink), &n), SIP_TLS_ERR);
	CHECK_UINT(n, 0);
	CHECK(strcmp(sip_tls_conn_error(&p.srv), "") != 0);

	/* the client SSL still points at a socket that is gone: the close must
	 * not reach for it, which pair_close() and ASan together prove */
	pair_close(&p);
	sip_tls_done(&t);
}

static void t_conn_refusals(void)
{
	struct sip_tls_cfg cfg;
	struct sip_tls_conn c;
	struct sip_tls t;
	size_t n = 42;
	int fds[2];

	CASE("tls: a server connection needs a server context");
	memset(&cfg, 0, sizeof(cfg));
	sip_tls_init(&t);
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	CHECK(!sip_tls_conn_start(&t, &c, fds[0], true, NULL));
	CHECK(strcmp(sip_tls_conn_error(&c), "") != 0);
	CHECK(!sip_tls_conn_up(&c));

	CASE("tls: a connection needs a socket");
	CHECK(!sip_tls_conn_start(&t, &c, -1, false, NULL));
	CHECK(strcmp(sip_tls_conn_error(&c), "") != 0);
	close(fds[0]);
	close(fds[1]);
	sip_tls_done(&t);

	CASE("tls: a connection that never started refuses every operation");
	memset(&c, 0, sizeof(c));
	CHECK_UINT(sip_tls_conn_io(&c), SIP_TLS_ERR);
	CHECK(!strcmp(sip_tls_conn_error(&c), "no connection"));
	CHECK_UINT(sip_tls_conn_read(&c, sink, sizeof(sink), &n), SIP_TLS_ERR);
	CHECK_UINT(n, 0);
	CHECK(!strcmp(sip_tls_conn_error(&c), "no connection"));
	CHECK_UINT(sip_tls_conn_write(&c, "x", 1, &n), SIP_TLS_ERR);
	CHECK_UINT(n, 0);
	CHECK(!strcmp(sip_tls_conn_error(&c), "no connection"));

	/* closing one is still a no-op, not a crash */
	sip_tls_conn_close(&c);
	CHECK(!sip_tls_conn_up(&c));
}

static void t_keylog(void)
{
	struct sip_tls_cfg cfg;
	struct sip_tls t;
	bool lines_ok = true;
	bool rotated = false;
	bool over = false;
	long size, prev;
	size_t count;
	int i;

	CASE("tls: the key log is written 0600 and counted");
	CHECK(keylog_dir_make());
	cfg_server(&cfg);
	cfg.keylog_path = keylog;
	sip_tls_init(&t);
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK(handshake_once(&t));

	size = file_size(keylog);
	CHECK(size > 0);
	CHECK_UINT(file_mode(keylog), 0600);
	CHECK_UINT(t.keylog_bytes, (uint64_t)size);

	/*
	 * Both contexts log: a TLS 1.3 handshake gives five labels per end, so
	 * anything under ten lines means one of the two callbacks is missing.
	 */
	count = keylog_lines(keylog, &lines_ok);
	CHECK(count >= 10);
	CHECK(lines_ok);

	CASE("tls: the key log truncates and starts again at the bound");
	prev = size;
	for (i = 0; i < 32 && !rotated; i++) {
		CHECK(handshake_once(&t));
		size = file_size(keylog);
		over = over || size > SIP_TLS_KEYLOG_MAX;
		rotated = rotated || size < prev;
		prev = size;
	}

	CHECK(rotated);
	CHECK(!over);
	CHECK(size > 0);
	CHECK_UINT(t.keylog_bytes, (uint64_t)size);
	lines_ok = true;
	CHECK(keylog_lines(keylog, &lines_ok) > 0);
	CHECK(lines_ok);
	sip_tls_done(&t);

	/*
	 * The bound belongs to the file. A daemon that restarts finds the log
	 * its previous run left, and the first line past the bound has to
	 * rotate it: the padding below is gone afterwards, which is what the
	 * line prefixes prove.
	 */
	CASE("tls: an existing key log is counted from its size across a restart");
	CHECK(file_fill(keylog, SIP_TLS_KEYLOG_MAX - 100));
	cfg_server(&cfg);
	cfg.keylog_path = keylog;
	sip_tls_init(&t);
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK_UINT(t.keylog_bytes, SIP_TLS_KEYLOG_MAX - 100);
	CHECK(handshake_once(&t));

	size = file_size(keylog);
	CHECK(size > 0);
	CHECK(size < SIP_TLS_KEYLOG_MAX - 100);
	CHECK_UINT(t.keylog_bytes, (uint64_t)size);
	lines_ok = true;
	CHECK(keylog_lines(keylog, &lines_ok) > 0);
	CHECK(lines_ok);
	sip_tls_done(&t);

	/*
	 * A log left at 0644 holds session secrets the moment the next line
	 * lands in it, so the mode is repaired before the write. The suite runs
	 * as one uid, so the repair succeeds here; the arm where it cannot
	 * succeed drops the line instead, which no portable case can reach.
	 */
	CASE("tls: a key log left readable is repaired to 0600 before a line lands");
	unlink(keylog);
	CHECK(file_fill(keylog, 10));
	CHECK(chmod(keylog, 0644) == 0);
	CHECK_UINT(file_mode(keylog), 0644);
	cfg_server(&cfg);
	cfg.keylog_path = keylog;
	sip_tls_init(&t);
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK_UINT(t.keylog_bytes, 10);
	CHECK(handshake_once(&t));

	size = file_size(keylog);
	CHECK_UINT(file_mode(keylog), 0600);
	CHECK(size > 10);			/* appended, not rotated */
	CHECK_UINT(t.keylog_bytes, (uint64_t)size);
	sip_tls_done(&t);

	CASE("tls: no key log path leaves no file behind");
	unlink(keylog);
	cfg_server(&cfg);
	sip_tls_init(&t);
	CHECK(sip_tls_ctx_load(&t, &cfg));
	CHECK(!t.keylog_path[0]);
	CHECK(handshake_once(&t));
	CHECK_UINT(t.keylog_bytes, 0);
	CHECK(file_size(keylog) < 0);
	sip_tls_done(&t);

	keylog_dir_drop();
}

int main(void)
{
	/* a write to a closed peer must surface as a case failure, not as a
	 * signal that takes the whole suite with it, as in src/main.c */
	signal(SIGPIPE, SIG_IGN);
	sip_wire_host = proto_host_fake();

	t_constants();
	t_names();
	t_life();

	if (!fixtures_take())
		return tu_summary("test_sip_tls");

	t_load_client_only();
	t_load_pair();
	t_load_half_pair();
	t_load_bad_paths();
	t_version_floor();
	t_ctx_flags();
	t_handshake();
	t_handshake6();
	t_verify();
	t_sni();
	t_bulk();
	t_shutdown();
	t_conn_refusals();
	t_keylog();

	return tu_summary("test_sip_tls");
}
