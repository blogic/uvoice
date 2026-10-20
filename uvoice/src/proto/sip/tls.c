// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * TLS for the SIP stream transport. See sip/tls.h for the contracts.
 *
 * The OpenSSL include lives here and only here: sip/tls.h names the two
 * library objects as opaque struct pointers, so no other translation unit
 * inherits the library headers (sip/tls.h "No OpenSSL in this header").
 *
 * Two contexts, one struct. The client context is the only one a load has to
 * produce. A configuration that names no certificate pair is a working
 * configuration (sip/tls.h "Client only is a normal state"), so every server
 * side failure records a reason and returns success. The one exception is the
 * trust store of a verifying client: a client that must verify and has no
 * anchor to verify against cannot open a connection at all, so that failure
 * takes the client context down with it.
 *
 * What the modes are for. SSL_MODE_ENABLE_PARTIAL_WRITE lets a write report
 * the bytes it moved instead of holding the whole payload, and
 * SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER lets the retry come from a different
 * address. The stream engine drains a staging buffer from an offset, so it
 * needs both. SSL_OP_NO_RENEGOTIATION refuses a TLS 1.2 renegotiation,
 * because a second handshake in the middle of a connection is a state the
 * sip/stream.c machine does not model. A TLS 1.3 key update is not affected:
 * it makes a write want a read, which the SIP_TLS_WANT_READ contract already
 * covers.
 *
 * The key log has no file descriptor. struct sip_tls holds a path, not an fd,
 * so the callback opens the file, appends one line and closes it again. That
 * is a handshake rate event, a few lines per connection, and the alternative
 * would be a descriptor held open for the life of the daemon over a file that
 * holds session secrets. The callback reaches its struct sip_tls through the
 * context app data, which is the only pointer OpenSSL hands back to it.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "str.h"
#include "proto/sip/tls.h"

/*
 * The version numbers are fixed to the library's own values, so the
 * configuration value passes straight into SSL_CTX_set_min_proto_version()
 * with no translation table to drift.
 */
_Static_assert(SIP_TLS_V1_2 == TLS1_2_VERSION, "SIP_TLS_V1_2 must be the OpenSSL value");
_Static_assert(SIP_TLS_V1_3 == TLS1_3_VERSION, "SIP_TLS_V1_3 must be the OpenSSL value");

/* the key log file mode: it holds session secrets and nothing else reads it */
#define KEYLOG_MODE	0600

/*
 * The label, then the first queued library error in whatever room is left.
 * The library text is the long half and the one that may be cut, which is why
 * it is appended in place instead of built in a second buffer.
 */
static void err_ssl_set(char *dst, size_t len, const char *what)
{
	unsigned long queued = ERR_get_error();
	int used;

	/* one failure can queue several entries, and the rest belong to nobody
	 * once this text exists: the next reader must not report them as its own */
	ERR_clear_error();

	if (!queued) {
		snprintf(dst, len, "%s failed", what);
		return;
	}

	used = snprintf(dst, len, "%s: ", what);
	if (used < 0 || (size_t)used >= len)
		return;

	ERR_error_string_n(queued, dst + used, len - (size_t)used);
}

static size_t fd_write_all(int fd, const char *buf, size_t len)
{
	size_t done = 0;
	ssize_t wrote;

	while (done < len) {
		wrote = write(fd, buf + done, len - done);
		if (wrote < 0 && errno == EINTR)
			continue;

		if (wrote <= 0)
			break;

		done += (size_t)wrote;
	}

	return done;
}

/*
 * One line and its newline, and the count of what reached the file. One
 * writev() keeps the line whole against any other writer of the same appended
 * file; a short one is finished by hand, because a torn line that the count
 * does not know about would make keylog_bytes drift below the file size and
 * the rotation bound with it.
 */
static size_t keylog_line_put(int fd, const char *line, size_t len)
{
	struct iovec iov[2];
	ssize_t wrote;
	size_t done;

	iov[0].iov_base = (void *)line;
	iov[0].iov_len = len;
	iov[1].iov_base = (void *)"\n";
	iov[1].iov_len = 1;

	wrote = writev(fd, iov, 2);
	done = wrote < 0 ? 0 : (size_t)wrote;

	if (done < len)
		done += fd_write_all(fd, line + done, len - done);

	if (done == len)
		done += fd_write_all(fd, "\n", 1);

	return done;
}

/*
 * One NSS key log line. A failed open is not fatal: a handshake must not
 * depend on a debug facility, so the line is dropped and the connection
 * proceeds.
 */
static void keylog_write(struct sip_tls *t, const char *line)
{
	int flags = O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC;
	size_t len = strlen(line);
	bool rotate;
	int fd;

	if (!t->keylog_path[0])
		return;

	rotate = t->keylog_bytes + len + 1 > SIP_TLS_KEYLOG_MAX;
	if (rotate)
		flags = O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC;

	/* O_NOFOLLOW: a symbolic link planted at the configured path would send
	 * the session secrets to a file this daemon never named */
	fd = open(t->keylog_path, flags, KEYLOG_MODE);
	if (fd < 0)
		return;

	/* the open already emptied it, so the count follows the file here */
	if (rotate)
		t->keylog_bytes = 0;

	/*
	 * The mode of an existing file, and a wide umask on a new one, must not
	 * leave session secrets readable by anyone else. A mode this daemon
	 * cannot repair, which is what an existing file of another owner gives
	 * it, drops the line: a debug facility never outranks the secret.
	 */
	if (fchmod(fd, KEYLOG_MODE)) {
		close(fd);
		return;
	}

	t->keylog_bytes += keylog_line_put(fd, line, len);

	close(fd);
}

static void keylog_cb(const SSL *ssl, const char *line)
{
	SSL_CTX *ctx = SSL_get_SSL_CTX(ssl);
	struct sip_tls *t = ctx ? SSL_CTX_get_app_data(ctx) : NULL;

	if (!t || !line)
		return;

	keylog_write(t, line);
}

/*
 * The bound of sip/tls.h belongs to the FILE, not to one run of the daemon, so
 * the count starts at what the named file already holds. A daemon that
 * restarts and reopens a log near the bound rotates it on the next line
 * instead of appending a whole cycle past it. A file that is not there yet,
 * and a path that cannot be read, both start at zero.
 */
static void keylog_take(struct sip_tls *t, const char *path)
{
	struct stat st;

	str_copy(t->keylog_path, sizeof(t->keylog_path), path);

	t->keylog_bytes = 0;

	if (t->keylog_path[0] && !stat(t->keylog_path, &st))
		t->keylog_bytes = (size_t)st.st_size;
}

/*
 * TLS 1.2 is the pinned floor, so a configuration below it is raised. A value
 * the library refuses falls back to the floor instead of leaving the context
 * at the library default, which can be lower. The fallback answer is not
 * checked: the two _Static_asserts above tie SIP_TLS_V1_2 to TLS1_2_VERSION,
 * so a library that refuses it is not a library this build links.
 */
static void version_floor_set(SSL_CTX *ctx, int min_version)
{
	int want = min_version < SIP_TLS_V1_2 ? SIP_TLS_V1_2 : min_version;

	if (SSL_CTX_set_min_proto_version(ctx, want))
		return;

	SSL_CTX_set_min_proto_version(ctx, SIP_TLS_V1_2);
}

static void ctx_common(struct sip_tls *t, SSL_CTX *ctx, const struct sip_tls_cfg *cfg)
{
	version_floor_set(ctx, cfg->min_version);
	SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE |
			 SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
	SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION);

	/* the app data is how keylog_cb() finds its struct sip_tls, so a
	 * context that cannot carry it gets no key log either */
	if (SSL_CTX_set_app_data(ctx, t) && t->keylog_path[0])
		SSL_CTX_set_keylog_callback(ctx, keylog_cb);
}

static bool trust_load(struct sip_tls *t, SSL_CTX *ctx, const char *ca_path)
{
	int ok;

	ERR_clear_error();

	if (ca_path && ca_path[0])
		ok = SSL_CTX_load_verify_locations(ctx, ca_path, NULL);
	else
		ok = SSL_CTX_set_default_verify_paths(ctx);

	if (ok)
		return true;

	err_ssl_set(t->err, sizeof(t->err), "trust store");

	return false;
}

static bool client_build(struct sip_tls *t, const struct sip_tls_cfg *cfg)
{
	SSL_CTX *ctx;

	ERR_clear_error();

	ctx = SSL_CTX_new(TLS_client_method());
	if (!ctx) {
		err_ssl_set(t->err, sizeof(t->err), "client context");
		return false;
	}

	ctx_common(t, ctx, cfg);

	/*
	 * SSL_VERIFY_PEER checks the chain and nothing else. The peer name is
	 * never checked here, in either mode: sip/tls.h "No hostname
	 * verification" records the deviation from RFC 5922 section 7.3 and
	 * the reason for it.
	 */
	SSL_CTX_set_verify(ctx, cfg->verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);

	if (cfg->verify && !trust_load(t, ctx, cfg->ca_path)) {
		SSL_CTX_free(ctx);
		return false;
	}

	t->client = ctx;

	return true;
}

static bool identity_load(struct sip_tls *t, SSL_CTX *ctx, const struct sip_tls_cfg *cfg)
{
	ERR_clear_error();

	if (!SSL_CTX_use_certificate_chain_file(ctx, cfg->cert_path)) {
		err_ssl_set(t->err, sizeof(t->err), "certificate");
		return false;
	}

	if (!SSL_CTX_use_PrivateKey_file(ctx, cfg->key_path, SSL_FILETYPE_PEM)) {
		err_ssl_set(t->err, sizeof(t->err), "private key");
		return false;
	}

	if (!SSL_CTX_check_private_key(ctx)) {
		err_ssl_set(t->err, sizeof(t->err), "key mismatch");
		return false;
	}

	return true;
}

/*
 * The server context is optional by contract, so every failure below records a
 * reason and leaves t->server NULL. The load still succeeds: the daemon
 * registers over TLS and takes no inbound connection until the bundle lands.
 */
static void server_build(struct sip_tls *t, const struct sip_tls_cfg *cfg)
{
	bool cert = cfg->cert_path && cfg->cert_path[0];
	bool key = cfg->key_path && cfg->key_path[0];
	SSL_CTX *ctx;

	if (!cert && !key)
		return;

	if (!cert || !key) {
		snprintf(t->err, sizeof(t->err), "certificate and key must both be set");
		return;
	}

	ERR_clear_error();

	ctx = SSL_CTX_new(TLS_server_method());
	if (!ctx) {
		err_ssl_set(t->err, sizeof(t->err), "server context");
		return;
	}

	ctx_common(t, ctx, cfg);

	/* uvoice never asks an inbound peer for a certificate */
	SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

	if (!identity_load(t, ctx, cfg)) {
		SSL_CTX_free(ctx);
		return;
	}

	t->server = ctx;
}

static void ctx_drop(struct sip_tls *t)
{
	SSL_CTX_free(t->client);
	SSL_CTX_free(t->server);
	t->client = NULL;
	t->server = NULL;
}

static bool conn_fail(struct sip_tls_conn *c, const char *text)
{
	str_copy(c->err, sizeof(c->err), text);

	return false;
}

static bool conn_drop(struct sip_tls_conn *c, const char *what)
{
	err_ssl_set(c->err, sizeof(c->err), what);
	SSL_free(c->ssl);
	c->ssl = NULL;

	return false;
}

static enum sip_tls_io conn_none(struct sip_tls_conn *c)
{
	if (c)
		conn_fail(c, "no connection");

	return SIP_TLS_ERR;
}

/*
 * The result of SSL_get_error() as the caller's next step. Everything that is
 * not a want is terminal, and the text comes from the first queued error, from
 * sys_err when the library only reports a system call, and from neither when
 * the peer disappeared without a word.
 *
 * sys_err is the caller's errno, taken at the failing call. It is a parameter
 * and not a read of errno here because the queue calls below, and anything
 * else that ran between, may have replaced it: a key log open that failed
 * during the same handshake would otherwise name its own ENOENT here.
 */
static enum sip_tls_io io_map(struct sip_tls_conn *c, int err, int sys_err)
{
	unsigned long queued;

	if (err == SSL_ERROR_WANT_READ)
		return SIP_TLS_WANT_READ;

	if (err == SSL_ERROR_WANT_WRITE)
		return SIP_TLS_WANT_WRITE;

	queued = ERR_get_error();
	ERR_clear_error();

	if (queued) {
		ERR_error_string_n(queued, c->err, sizeof(c->err));
		return SIP_TLS_ERR;
	}

	if (err == SSL_ERROR_SYSCALL && sys_err)
		str_copy(c->err, sizeof(c->err), strerror(sys_err));
	else
		snprintf(c->err, sizeof(c->err), "unexpected eof");

	return SIP_TLS_ERR;
}

const char *sip_tls_io_name(enum sip_tls_io io)
{
	switch (io) {
	case SIP_TLS_OK:
		return "ok";
	case SIP_TLS_WANT_READ:
		return "want_read";
	case SIP_TLS_WANT_WRITE:
		return "want_write";
	case SIP_TLS_ERR:
		return "error";
	}

	return "unknown";
}

void sip_tls_init(struct sip_tls *t)
{
	if (!t)
		return;

	memset(t, 0, sizeof(*t));
}

void sip_tls_done(struct sip_tls *t)
{
	if (!t)
		return;

	ctx_drop(t);
	t->loaded = false;
}

bool sip_tls_ctx_load(struct sip_tls *t, const struct sip_tls_cfg *cfg)
{
	if (!t || !cfg)
		return false;

	/* the bundle reload: the old contexts go, and the caller closes the
	 * connections that hold their session secrets */
	ctx_drop(t);
	t->loaded = false;
	t->err[0] = '\0';

	keylog_take(t, cfg->keylog_path);

	if (!client_build(t, cfg))
		return false;

	server_build(t, cfg);
	t->loaded = true;

	return true;
}

bool sip_tls_client_ready(const struct sip_tls *t)
{
	return t && t->client;
}

bool sip_tls_server_ready(const struct sip_tls *t)
{
	return t && t->server;
}

const char *sip_tls_error(const struct sip_tls *t)
{
	return t ? t->err : "";
}

bool sip_tls_conn_start(struct sip_tls *t, struct sip_tls_conn *c, int fd, bool server,
			const char *sni)
{
	SSL_CTX *ctx;

	if (!t || !c)
		return false;

	memset(c, 0, sizeof(*c));
	c->t = t;
	c->server = server;

	ctx = server ? t->server : t->client;
	if (!ctx)
		return conn_fail(c, server ? "no server context" : "no client context");

	if (fd < 0)
		return conn_fail(c, "bad socket");

	ERR_clear_error();

	c->ssl = SSL_new(ctx);
	if (!c->ssl) {
		err_ssl_set(c->err, sizeof(c->err), "ssl object");
		return false;
	}

	/* the socket belongs to the stream engine: SSL_set_fd() wraps it in a
	 * BIO that does not close it, and sip_tls_conn_close() leaves it open */
	if (!SSL_set_fd(c->ssl, fd))
		return conn_drop(c, "socket");

	if (server) {
		SSL_set_accept_state(c->ssl);
		return true;
	}

	SSL_set_connect_state(c->ssl);

	/*
	 * RFC 6066 section 3 forbids a literal address in server_name, and the
	 * caller passes NULL for one. A name that the extension refuses would
	 * reach the peer's default virtual host and take the wrong certificate,
	 * so the connection is refused instead of opened without the name.
	 */
	if (sni && sni[0] && !SSL_set_tlsext_host_name(c->ssl, sni))
		return conn_drop(c, "server_name");

	return true;
}

enum sip_tls_io sip_tls_conn_io(struct sip_tls_conn *c)
{
	int ret, err, sys_err;

	if (!c || !c->ssl)
		return conn_none(c);

	ERR_clear_error();

	ret = SSL_do_handshake(c->ssl);
	if (ret == 1) {
		c->up = true;
		return SIP_TLS_OK;
	}

	err = SSL_get_error(c->ssl, ret);
	sys_err = errno;

	return io_map(c, err, sys_err);
}

enum sip_tls_io sip_tls_conn_read(struct sip_tls_conn *c, void *buf, size_t len, size_t *done)
{
	size_t got = 0;
	int ret, err, sys_err;

	if (done)
		*done = 0;

	if (!c || !c->ssl)
		return conn_none(c);

	ERR_clear_error();

	ret = SSL_read_ex(c->ssl, buf, len, &got);
	if (ret == 1) {
		if (done)
			*done = got;
		return SIP_TLS_OK;
	}

	err = SSL_get_error(c->ssl, ret);
	sys_err = errno;

	/* the peer sent close_notify: an orderly end of the byte stream, which
	 * the caller reads as SIP_TLS_OK with no bytes */
	if (err == SSL_ERROR_ZERO_RETURN)
		return SIP_TLS_OK;

	return io_map(c, err, sys_err);
}

enum sip_tls_io sip_tls_conn_write(struct sip_tls_conn *c, const void *buf, size_t len,
				   size_t *done)
{
	size_t wrote = 0;
	int ret, err, sys_err;

	if (done)
		*done = 0;

	if (!c || !c->ssl)
		return conn_none(c);

	ERR_clear_error();

	ret = SSL_write_ex(c->ssl, buf, len, &wrote);
	if (ret == 1) {
		if (done)
			*done = wrote;
		return SIP_TLS_OK;
	}

	err = SSL_get_error(c->ssl, ret);
	sys_err = errno;

	return io_map(c, err, sys_err);
}

void sip_tls_conn_close(struct sip_tls_conn *c)
{
	if (!c || !c->ssl)
		return;

	/*
	 * One SSL_shutdown() and no second one. A close_notify that the socket
	 * cannot take right now answers SIP_TLS_WANT_WRITE, and waiting for it
	 * would block a path that must not block; the peer sees the connection
	 * go instead. c->err carries the last terminal failure, so an empty
	 * text is what says the record layer can still speak.
	 */
	if (c->up && !c->err[0])
		SSL_shutdown(c->ssl);

	SSL_free(c->ssl);
	c->ssl = NULL;
	c->up = false;
}

bool sip_tls_conn_up(const struct sip_tls_conn *c)
{
	return c && c->up;
}

const char *sip_tls_conn_error(const struct sip_tls_conn *c)
{
	return c ? c->err : "";
}
