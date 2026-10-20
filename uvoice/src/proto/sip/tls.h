// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * TLS for the SIP stream transport (src/proto/sip/tls.c).
 *
 * What it is. A thin wrapper around one OpenSSL 3 client context and one
 * OpenSSL 3 server context, plus the per connection operations the stream
 * engine drives. It owns no socket, no timer and no connection state machine:
 * sip/stream.h owns those, and this layer answers "what does the record layer
 * want next".
 *
 * No OpenSSL in this header. Every OpenSSL type appears here as an opaque
 * struct pointer, so a translation unit that includes sip/stream.h does not
 * inherit <openssl/ssl.h>. That keeps the parser tier and the transaction tier
 * free of the library, and it keeps the include path of the unit suites
 * unchanged. tls.c owns the include. The two pointer types name the structs
 * OpenSSL itself typedefs: SSL is struct ssl_st and SSL_CTX is struct
 * ssl_ctx_st, so the pointers pass straight into the library with no cast.
 *
 * Socket BIOs. The connection uses SSL_set_fd() on the stream engine's non
 * blocking socket, not a memory BIO pair. An operation that cannot finish
 * reports SIP_TLS_WANT_READ or SIP_TLS_WANT_WRITE, and the caller turns that
 * into the uloop mask it waits on. That is the whole contract between the two
 * layers: this header never touches uloop.
 *
 * A write can want read. TLS is not a byte pipe: a renegotiation or a session
 * ticket makes a write ask for a read and a read ask for a write. The caller
 * must set the uloop mask the result names, and never the mask its own
 * direction suggests.
 *
 * Client only is a normal state. sip_tls_ctx_load() builds the server context
 * only when the configuration names both a certificate and a private key. A
 * configuration with neither is not an error: uvoice registers over TLS and
 * takes no inbound TLS connection until the certificate bundle arrives.
 * sip_tls_server_ready() reports which mode the load produced.
 *
 * No hostname verification. The peer certificate is never checked against the
 * name uvoice dialled, not with X509_check_host() and not through
 * SSL_set1_host(). This deviates from RFC 5922 section 7.3 on purpose: a
 * carrier whose certificate carries a name the SRV chain does not produce
 * would stop registering. cfg tls_server_verification_enabled
 * turns on chain verification alone (SSL_VERIFY_PEER plus the configured
 * ca.crt); the name stays unchecked in both modes.
 *
 * Server name indication. The client sends server_name only when the target
 * came from a DNS name, because RFC 6066 3 forbids a literal address in the
 * extension. The stream engine knows the provenance of the target and passes
 * the name, or NULL for an address.
 *
 * Keylog. cfg tls_keylog_enabled writes the NSS key log line of every
 * handshake to the configured path so a support capture decrypts. The file is
 * rotated once it passes SIP_TLS_KEYLOG_MAX bytes.
 * It holds session secrets: it is a debug facility, off by default, and never
 * a default path in the shipped configuration.
 */
#ifndef UVOICE_SIP_TLS_H
#define UVOICE_SIP_TLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto/shared/tls_paths.h"

/* the two OpenSSL objects, opaque here; tls.c has the definitions */
struct ssl_st;
struct ssl_ctx_st;

/*
 * The bundle layout. uvoice_tls_cert.sh unpacks the fetched archive into
 * this directory (tls_paths.h), so the three paths are what a configuration
 * that names none takes.
 */
#define SIP_TLS_DIR_DEFAULT	UVOICE_TLS_DIR_DEFAULT
#define SIP_TLS_CERT_DEFAULT	"/tmp/voip_tls/cert.pem"
#define SIP_TLS_KEY_DEFAULT	"/tmp/voip_tls/key.pem"
#define SIP_TLS_CA_DEFAULT	"/tmp/voip_tls/ca.crt"

/*
 * The key log path cfg-tls maps bool tls_keylog_enabled to, in uvoice's own
 * directory, because this is uvoice's capture.
 */
#define SIP_TLS_KEYLOG_DEFAULT	"/tmp/uvoice/ssl.keylog"

/* longest path this layer stores, matching cfg.h tls_cert_path */
#define SIP_TLS_PATH_MAX	128

/* the rotation bound: a key log longer than this is truncated and restarted */
#define SIP_TLS_KEYLOG_MAX	5000

/* longest error text kept for the ubus status contract */
#define SIP_TLS_ERR_MAX		96

/*
 * Protocol versions as the wire numbers (RFC 8446 legacy_version): 0x0303 is
 * TLS 1.2 and 0x0304 is TLS 1.3. They are the values OpenSSL's
 * TLS1_2_VERSION and TLS1_3_VERSION carry, so tls.c passes them straight to
 * SSL_CTX_set_min_proto_version() and asserts the equality at compile time.
 */
#define SIP_TLS_V1_2		0x0303
#define SIP_TLS_V1_3		0x0304

/*
 * cert_path, key_path  the server identity. Both must be present for a server
 *                      context; neither present is client only mode.
 * ca_path              the trust store for verify. NULL takes the system store.
 * keylog_path          NULL turns the key log off. Nothing else does.
 * verify               cfg tls_server_verification_enabled: SSL_VERIFY_PEER on
 *                      the client context. The peer name is never checked
 *                      ("No hostname verification" above).
 * min_version          0 takes SIP_TLS_V1_2, which is the pinned floor. A
 *                      configuration cannot lower it below TLS 1.2.
 */
struct sip_tls_cfg {
	const char *cert_path;
	const char *key_path;
	const char *ca_path;
	const char *keylog_path;
	bool verify;
	int min_version;
};

/*
 * client  the context every outbound connection uses. Always built.
 * server  the context an accepted connection uses, NULL in client only mode.
 * err     the last failure text, for the ubus status contract.
 */
struct sip_tls {
	struct ssl_ctx_st *client;
	struct ssl_ctx_st *server;

	char keylog_path[SIP_TLS_PATH_MAX];
	size_t keylog_bytes;

	char err[SIP_TLS_ERR_MAX];
	bool loaded;
};

/*
 * The result of every per connection operation.
 *
 * SIP_TLS_OK         the operation finished. For read and write, *done says
 *                    how many bytes moved, and a short count is normal.
 * SIP_TLS_WANT_READ  wait for ULOOP_READ and call the same operation again.
 * SIP_TLS_WANT_WRITE wait for ULOOP_WRITE and call the same operation again.
 * SIP_TLS_ERR        terminal. The connection closes; sip_tls_conn_error()
 *                    carries the reason.
 */
enum sip_tls_io {
	SIP_TLS_OK = 0,
	SIP_TLS_WANT_READ,
	SIP_TLS_WANT_WRITE,
	SIP_TLS_ERR,
};

const char *sip_tls_io_name(enum sip_tls_io io);

/*
 * One connection's TLS state. It lives inside struct sip_conn, so it holds no
 * pointer the stream engine has to free by hand: sip_tls_conn_close() releases
 * the SSL object and the socket stays with the engine that opened it.
 */
struct sip_tls_conn {
	struct ssl_st *ssl;
	struct sip_tls *t;
	char err[SIP_TLS_ERR_MAX];
	bool server;
	bool up;			/* the handshake finished */
};

void sip_tls_init(struct sip_tls *t);
void sip_tls_done(struct sip_tls *t);

/*
 * Build both contexts from cfg. False means the client context could not be
 * built, which is the only fatal case: a missing or unreadable certificate
 * pair leaves the server context NULL and still returns true ("Client only is
 * a normal state" above). Calling it again replaces the contexts, which is
 * what a certificate bundle reload does; connections built on the old
 * contexts are closed by the caller, because their session secrets belong to
 * the bundle that is going away.
 */
bool sip_tls_ctx_load(struct sip_tls *t, const struct sip_tls_cfg *cfg);

bool sip_tls_client_ready(const struct sip_tls *t);
bool sip_tls_server_ready(const struct sip_tls *t);

/* the last context level failure, "" when there was none */
const char *sip_tls_error(const struct sip_tls *t);

/*
 * Attach a connection to an open, non blocking socket. server picks the
 * context and the handshake direction. sni is the name the target came from,
 * or NULL for an address target and for every accepted connection. False means
 * the context is missing or the SSL object could not be built; the caller
 * closes the socket.
 */
bool sip_tls_conn_start(struct sip_tls *t, struct sip_tls_conn *c, int fd, bool server,
			const char *sni);

/*
 * Drive the handshake. SIP_TLS_OK means it finished and the connection carries
 * application data from now on. The stream engine calls it on every readable
 * or writable event until it stops answering SIP_TLS_WANT_*.
 */
enum sip_tls_io sip_tls_conn_io(struct sip_tls_conn *c);

/*
 * Move application data. *done receives the byte count and is 0 for anything
 * but SIP_TLS_OK. A read that returns SIP_TLS_OK with *done == 0 is the clean
 * shutdown of the peer, which closes the connection without an error.
 */
enum sip_tls_io sip_tls_conn_read(struct sip_tls_conn *c, void *buf, size_t len, size_t *done);
enum sip_tls_io sip_tls_conn_write(struct sip_tls_conn *c, const void *buf, size_t len,
				   size_t *done);

/*
 * Release the SSL object. It sends a close_notify when the handshake finished
 * and the socket still accepts it, and it never blocks. The socket belongs to
 * the caller and stays open.
 */
void sip_tls_conn_close(struct sip_tls_conn *c);

bool sip_tls_conn_up(const struct sip_tls_conn *c);

/* the last connection level failure, "" when there was none */
const char *sip_tls_conn_error(const struct sip_tls_conn *c);

#endif
