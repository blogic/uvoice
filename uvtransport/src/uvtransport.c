// SPDX-License-Identifier: LGPL-2.1-only
/*
 * uvtransport - ICE and DTLS-SRTP media transport for one stream
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <juice/juice.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/srtp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <uvsrtp/uvsrtp.h>

#include "uvtransport/uvtransport.h"

#define UVT_CANDIDATE_RING	16U
#define UVT_DTLS_MTU		1200L
#define UVT_SRTP_PROFILES	"SRTP_AES128_CM_SHA1_80:SRTP_AES128_CM_SHA1_32"
#define UVT_EXPORT_LABEL	"EXTRACTOR-dtls_srtp"
#define UVT_KEY_LEN		16U
#define UVT_SALT_LEN		14U
#define UVT_EXPORT_LEN		(2U * (UVT_KEY_LEN + UVT_SALT_LEN))
#define UVT_CERT_DAYS		365L
#define UVT_DESC_MAX		1024U
#define UVT_DRAIN_MAX		64U
#define UVT_BUF_MAX		(UVT_PACKET_MAX + UVSRTP_MAX_SRTCP_TRAILER)

enum uvt_recv_verdict {
	UVT_RECV_NONE = 0,
	UVT_RECV_RTP,
	UVT_RECV_RTCP,
	UVT_RECV_UP,
	UVT_RECV_FAIL,
	UVT_RECV_CLOSED,
};

struct uvt_global {
	pthread_mutex_t lock;
	SSL_CTX *ctx;
	X509 *cert;
	EVP_PKEY *key;
	BIO_METHOD *bio_meth;
	uint8_t fingerprint[UVT_FINGERPRINT_LEN];
	bool ready;
};

struct uvt_stream {
	pthread_mutex_t lock;
	struct uvt_cfg cfg;
	juice_agent_t *agent;
	juice_turn_server_t turn[UVT_TURN_MAX];
	SSL *ssl;
	BIO *rbio;
	struct uvsrtp *srtp;
	enum uvt_setup setup;
	enum uvt_ice_state ice;
	uint8_t remote_fp[UVT_FINGERPRINT_LEN];
	bool remote_fp_set;
	bool gathered;
	bool remote_set;
	bool dtls_up;
	bool dtls_failed;
	bool dtls_closed;
	char cand[UVT_CANDIDATE_RING][UVT_CANDIDATE_MAX];
	unsigned int cand_head;
	unsigned int cand_count;
};

static struct uvt_global global = { .lock = PTHREAD_MUTEX_INITIALIZER };

/**
 * event_emit() - report one transport event to the owner
 * @stream: the stream
 * @event: the event
 * @arg: its argument
 */
static void event_emit(struct uvt_stream *stream, enum uvt_event event, int arg)
{
	if (stream->cfg.on_event)
		stream->cfg.on_event(stream->cfg.user, event, arg);
}

/**
 * bio_write() - send one DTLS record through the ICE agent
 * @bio: the write BIO of the stream
 * @buf: the record
 * @len: its length
 *
 * Return: @len, or -1 when the agent refused the datagram.
 */
static int bio_write(BIO *bio, const char *buf, int len)
{
	struct uvt_stream *stream = BIO_get_data(bio);

	if (len <= 0)
		return 0;
	if (juice_send(stream->agent, buf, (size_t)len) < 0)
		return -1;

	return len;
}

/**
 * bio_ctrl() - answer the BIO controls the DTLS record layer asks
 * @bio: the write BIO of the stream
 * @cmd: the control
 * @num: its numeric argument
 * @ptr: its pointer argument
 *
 * Return: 1 for a flush, the MTU for the MTU query, 0 otherwise.
 */
static long bio_ctrl(BIO *bio, int cmd, long num, void *ptr)
{
	switch (cmd) {
	case BIO_CTRL_FLUSH:
		return 1;
	case BIO_CTRL_DGRAM_QUERY_MTU:
		return UVT_DTLS_MTU;
	default:
		return 0;
	}
}

/**
 * bio_create() - mark a new write BIO as initialised
 * @bio: the BIO
 *
 * Return: 1.
 */
static int bio_create(BIO *bio)
{
	BIO_set_init(bio, 1);

	return 1;
}

/**
 * bio_destroy() - release a write BIO, which owns nothing
 * @bio: the BIO
 *
 * Return: 1.
 */
static int bio_destroy(BIO *bio)
{
	return 1;
}

static bool cert_fingerprint_ok(const struct uvt_stream *stream, X509 *cert)
{
	uint8_t fingerprint[UVT_FINGERPRINT_LEN];
	unsigned int len = 0;

	if (!cert || !stream->remote_fp_set)
		return false;
	if (X509_digest(cert, EVP_sha256(), fingerprint, &len) != 1)
		return false;

	return len == UVT_FINGERPRINT_LEN &&
	       !memcmp(fingerprint, stream->remote_fp, UVT_FINGERPRINT_LEN);
}

/**
 * verify_peer() - check the peer certificate at handshake time
 * @ok: the verdict of the chain check
 * @ctx: the store context
 *
 * The signalled fingerprint replaces the chain check: the leaf certificate
 * must match it, so a mismatch aborts the handshake with a fatal alert and
 * fails both ends. dtls_finish() checks the fingerprint once more.
 *
 * Return: 1 to go on, 0 to abort the handshake.
 */
static int verify_peer(int ok, X509_STORE_CTX *ctx)
{
	SSL *ssl = X509_STORE_CTX_get_ex_data(ctx,
					      SSL_get_ex_data_X509_STORE_CTX_idx());
	struct uvt_stream *stream = ssl ? SSL_get_app_data(ssl) : NULL;

	if (X509_STORE_CTX_get_error_depth(ctx) > 0)
		return 1;

	return stream &&
	       cert_fingerprint_ok(stream, X509_STORE_CTX_get_current_cert(ctx));
}

/**
 * cert_make() - generate the self-signed certificate of this process
 *
 * Return: 0, or a negative errno.
 */
static int cert_make(void)
{
	EVP_PKEY *key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
	X509 *cert = X509_new();
	BIGNUM *serial = BN_new();
	X509_NAME *name;
	unsigned int len = 0;
	int ret = -EIO;

	if (!key || !cert || !serial)
		goto out;
	if (!BN_rand(serial, 64, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) ||
	    !BN_to_ASN1_INTEGER(serial, X509_get_serialNumber(cert)))
		goto out;
	if (!X509_set_version(cert, 2) ||
	    !X509_gmtime_adj(X509_getm_notBefore(cert), -86400L) ||
	    !X509_gmtime_adj(X509_getm_notAfter(cert), UVT_CERT_DAYS * 86400L) ||
	    !X509_set_pubkey(cert, key))
		goto out;
	name = X509_get_subject_name(cert);
	if (!X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
					(const unsigned char *)"uvtransport",
					-1, -1, 0) ||
	    !X509_set_issuer_name(cert, name))
		goto out;
	if (X509_sign(cert, key, EVP_sha256()) <= 0)
		goto out;
	if (!X509_digest(cert, EVP_sha256(), global.fingerprint, &len) ||
	    len != UVT_FINGERPRINT_LEN)
		goto out;

	global.cert = cert;
	global.key = key;
	cert = NULL;
	key = NULL;
	ret = 0;
out:
	BN_free(serial);
	X509_free(cert);
	EVP_PKEY_free(key);

	return ret;
}

/**
 * ctx_make() - build the DTLS context every stream handshakes with
 *
 * Return: 0, or a negative errno.
 */
static int ctx_make(void)
{
	SSL_CTX *ctx = SSL_CTX_new(DTLS_method());

	if (!ctx)
		return -ENOMEM;
	if (!SSL_CTX_set_min_proto_version(ctx, DTLS1_2_VERSION) ||
	    SSL_CTX_use_certificate(ctx, global.cert) != 1 ||
	    SSL_CTX_use_PrivateKey(ctx, global.key) != 1 ||
	    SSL_CTX_set_tlsext_use_srtp(ctx, UVT_SRTP_PROFILES) != 0) {
		SSL_CTX_free(ctx);
		return -EIO;
	}
	SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
			   verify_peer);
	SSL_CTX_set_options(ctx, SSL_OP_NO_QUERY_MTU);
	SSL_CTX_set_read_ahead(ctx, 1);
	global.ctx = ctx;

	return 0;
}

/**
 * bio_meth_make() - build the write BIO method that sends through libjuice
 *
 * Return: 0, or -ENOMEM.
 */
static int bio_meth_make(void)
{
	BIO_METHOD *method = BIO_meth_new(BIO_TYPE_SOURCE_SINK |
					  BIO_get_new_index(), "uvtransport");

	if (!method)
		return -ENOMEM;
	BIO_meth_set_write(method, bio_write);
	BIO_meth_set_ctrl(method, bio_ctrl);
	BIO_meth_set_create(method, bio_create);
	BIO_meth_set_destroy(method, bio_destroy);
	global.bio_meth = method;

	return 0;
}

int uvt_init(void)
{
	int ret = 0;

	pthread_mutex_lock(&global.lock);
	if (global.ready)
		goto out;
	ret = uvsrtp_init();
	if (ret)
		goto out;
	ret = cert_make();
	if (ret)
		goto out;
	ret = ctx_make();
	if (ret)
		goto out;
	ret = bio_meth_make();
	if (ret)
		goto out;
	global.ready = true;
out:
	pthread_mutex_unlock(&global.lock);

	return ret;
}

int uvt_fingerprint_get(uint8_t out[UVT_FINGERPRINT_LEN])
{
	int ret = -EAGAIN;

	pthread_mutex_lock(&global.lock);
	if (global.ready) {
		memcpy(out, global.fingerprint, UVT_FINGERPRINT_LEN);
		ret = 0;
	}
	pthread_mutex_unlock(&global.lock);

	return ret;
}

/**
 * peer_fingerprint_ok() - compare the peer certificate with the signalled one
 * @stream: the stream, after its handshake
 *
 * Return: true when the SHA-256 fingerprints match.
 */
static bool peer_fingerprint_ok(struct uvt_stream *stream)
{
	X509 *peer = SSL_get1_peer_certificate(stream->ssl);
	bool ok = cert_fingerprint_ok(stream, peer);

	X509_free(peer);

	return ok;
}

/**
 * srtp_suite_of() - map the negotiated use_srtp profile onto libuvsrtp
 * @stream: the stream, after its handshake
 * @suite: receives the suite
 *
 * Return: true when the profile is one this library carries.
 */
static bool srtp_suite_of(struct uvt_stream *stream, enum uvsrtp_suite *suite)
{
	const SRTP_PROTECTION_PROFILE *profile;

	profile = SSL_get_selected_srtp_profile(stream->ssl);
	if (!profile)
		return false;
	if (profile->id == SRTP_AES128_CM_SHA1_80) {
		*suite = UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_80;
		return true;
	}
	if (profile->id == SRTP_AES128_CM_SHA1_32) {
		*suite = UVSRTP_SUITE_AES_CM_128_HMAC_SHA1_32;
		return true;
	}

	return false;
}

/**
 * srtp_keys_take() - export the SRTP keys of the handshake into libuvsrtp
 * @stream: the stream, after its handshake
 * @suite: the negotiated suite
 *
 * RFC 5764 section 4.2 orders the material as client key, server key,
 * client salt, server salt. The DTLS client sends with the client half.
 *
 * Return: 0, or a negative errno.
 */
static int srtp_keys_take(struct uvt_stream *stream, enum uvsrtp_suite suite)
{
	uint8_t material[UVT_EXPORT_LEN];
	uint8_t client[UVSRTP_KEY_LEN];
	uint8_t server[UVSRTP_KEY_LEN];
	bool is_client = stream->setup == UVT_SETUP_ACTIVE;
	int ret;

	if (SSL_export_keying_material(stream->ssl, material, sizeof(material),
				       UVT_EXPORT_LABEL, strlen(UVT_EXPORT_LABEL),
				       NULL, 0, 0) != 1)
		return -EPROTO;

	memcpy(client, material, UVT_KEY_LEN);
	memcpy(client + UVT_KEY_LEN, material + 2 * UVT_KEY_LEN, UVT_SALT_LEN);
	memcpy(server, material + UVT_KEY_LEN, UVT_KEY_LEN);
	memcpy(server + UVT_KEY_LEN, material + 2 * UVT_KEY_LEN + UVT_SALT_LEN,
	       UVT_SALT_LEN);
	ret = uvsrtp_create(&stream->srtp, suite, is_client ? client : server,
			    is_client ? server : client);
	explicit_bzero(material, sizeof(material));
	explicit_bzero(client, sizeof(client));
	explicit_bzero(server, sizeof(server));

	return ret;
}

/**
 * dtls_finish() - verify the far end and key the stream after the handshake
 * @stream: the stream
 *
 * Context: the stream lock is held.
 * Return: 1 when media may flow, -1 when the stream failed.
 */
static int dtls_finish(struct uvt_stream *stream)
{
	enum uvsrtp_suite suite;

	if (peer_fingerprint_ok(stream) && srtp_suite_of(stream, &suite) &&
	    !srtp_keys_take(stream, suite)) {
		stream->dtls_up = true;
		return 1;
	}

	stream->dtls_failed = true;
	SSL_shutdown(stream->ssl);

	return -1;
}

/**
 * dtls_drive() - run the handshake as far as the records at hand allow
 * @stream: the stream
 *
 * Context: the stream lock is held.
 * Return: 1 when the handshake completed, 0 while it waits, -1 on failure.
 */
static int dtls_drive(struct uvt_stream *stream)
{
	int ret, err;

	if (stream->dtls_up)
		return 1;
	if (stream->dtls_failed)
		return -1;

	ret = SSL_do_handshake(stream->ssl);
	if (ret == 1)
		return dtls_finish(stream);

	err = SSL_get_error(stream->ssl, ret);
	if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
		return 0;

	stream->dtls_failed = true;

	return -1;
}

/**
 * dtls_start() - make the DTLS object of a connected stream and open the flight
 * @stream: the stream
 *
 * Context: the stream lock is held.
 * Return: the dtls_drive() verdict, -1 when the object could not be made.
 */
static int dtls_start(struct uvt_stream *stream)
{
	BIO *wbio;

	stream->ssl = SSL_new(global.ctx);
	stream->rbio = BIO_new(BIO_s_mem());
	wbio = BIO_new(global.bio_meth);
	if (!stream->ssl || !stream->rbio || !wbio) {
		SSL_free(stream->ssl);
		BIO_free(stream->rbio);
		BIO_free(wbio);
		stream->ssl = NULL;
		stream->rbio = NULL;
		stream->dtls_failed = true;
		return -1;
	}
	BIO_set_mem_eof_return(stream->rbio, -1);
	BIO_set_data(wbio, stream);
	SSL_set_app_data(stream->ssl, stream);
	SSL_set_bio(stream->ssl, stream->rbio, wbio);
	SSL_set_mtu(stream->ssl, UVT_DTLS_MTU);
	if (stream->setup == UVT_SETUP_ACTIVE)
		SSL_set_connect_state(stream->ssl);
	else
		SSL_set_accept_state(stream->ssl);

	return dtls_drive(stream);
}

/**
 * dtls_due() - whether the handshake can start now
 * @stream: the stream
 *
 * Return: true once ICE connected, the far end is known and no object exists.
 */
static bool dtls_due(const struct uvt_stream *stream)
{
	bool connected = stream->ice == UVT_ICE_CONNECTED ||
			 stream->ice == UVT_ICE_COMPLETED;

	return connected && !stream->ssl && stream->setup != UVT_SETUP_ACTPASS &&
	       stream->remote_set;
}

/**
 * dtls_drain() - read what the far end sent after the handshake
 * @stream: the stream
 *
 * Context: the stream lock is held.
 * Return: UVT_RECV_CLOSED when the far end closed the session, else NONE.
 */
static enum uvt_recv_verdict dtls_drain(struct uvt_stream *stream)
{
	char tmp[UVT_DRAIN_MAX];
	int ret;

	if (stream->dtls_closed)
		return UVT_RECV_NONE;

	for (;;) {
		ret = SSL_read(stream->ssl, tmp, sizeof(tmp));
		if (ret > 0)
			continue;
		if (SSL_get_error(stream->ssl, ret) == SSL_ERROR_ZERO_RETURN) {
			stream->dtls_closed = true;
			return UVT_RECV_CLOSED;
		}
		return UVT_RECV_NONE;
	}
}

/**
 * dtls_input() - feed one DTLS datagram to the record layer
 * @stream: the stream
 * @data: the datagram
 * @size: its length
 *
 * Context: the stream lock is held.
 * Return: the verdict the owner is told about.
 */
static enum uvt_recv_verdict dtls_input(struct uvt_stream *stream,
					const char *data, size_t size)
{
	int verdict;

	if (!stream->ssl)
		return UVT_RECV_NONE;
	BIO_write(stream->rbio, data, (int)size);
	if (stream->dtls_up)
		return dtls_drain(stream);

	verdict = dtls_drive(stream);
	if (verdict > 0)
		return UVT_RECV_UP;

	return verdict < 0 ? UVT_RECV_FAIL : UVT_RECV_NONE;
}

/**
 * rtcp_is() - tell RTCP from RTP on the one component (RFC 5761 section 4)
 * @pkt: the datagram
 * @len: its length
 *
 * Return: true for a packet type of 200 to 207.
 */
static bool rtcp_is(const uint8_t *pkt, size_t len)
{
	return len >= 2 && pkt[1] >= 200 && pkt[1] <= 207;
}

/**
 * media_input() - unprotect one SRTP or SRTCP datagram in place
 * @stream: the stream
 * @buf: the datagram in a buffer of UVT_BUF_MAX bytes
 * @size: its length
 * @out: receives the plaintext length
 *
 * Context: the stream lock is held.
 * Return: UVT_RECV_RTP or UVT_RECV_RTCP, NONE for a datagram that failed.
 */
static enum uvt_recv_verdict media_input(struct uvt_stream *stream,
					 uint8_t *buf, size_t size, size_t *out)
{
	bool rtcp = rtcp_is(buf, size);
	int ret;

	ret = rtcp ? uvsrtp_unprotect_rtcp(stream->srtp, buf, size, UVT_BUF_MAX,
					   out) :
		     uvsrtp_unprotect(stream->srtp, buf, size, UVT_BUF_MAX, out);
	if (ret)
		return UVT_RECV_NONE;

	return rtcp ? UVT_RECV_RTCP : UVT_RECV_RTP;
}

/**
 * on_recv() - one datagram from the connected pair (RFC 7983 demultiplexing)
 * @agent: the ICE agent
 * @data: the datagram
 * @size: its length
 * @user: the stream
 *
 * Context: the libjuice thread.
 */
static void on_recv(juice_agent_t *agent, const char *data, size_t size,
		    void *user)
{
	struct uvt_stream *stream = user;
	_Alignas(uint32_t) uint8_t buf[UVT_BUF_MAX];
	enum uvt_recv_verdict verdict = UVT_RECV_NONE;
	uint8_t first;
	size_t out = 0;

	if (!size || size > UVT_PACKET_MAX)
		return;
	first = (uint8_t)data[0];

	pthread_mutex_lock(&stream->lock);
	if (first >= 20 && first <= 63) {
		verdict = dtls_input(stream, data, size);
	} else if (first >= 128 && first <= 191 && stream->dtls_up &&
		   !stream->dtls_closed) {
		memcpy(buf, data, size);
		verdict = media_input(stream, buf, size, &out);
	}
	pthread_mutex_unlock(&stream->lock);

	switch (verdict) {
	case UVT_RECV_RTP:
		if (stream->cfg.on_rtp)
			stream->cfg.on_rtp(stream->cfg.user, buf, out);
		break;
	case UVT_RECV_RTCP:
		if (stream->cfg.on_rtcp)
			stream->cfg.on_rtcp(stream->cfg.user, buf, out);
		break;
	case UVT_RECV_UP:
		event_emit(stream, UVT_EV_DTLS_UP, 0);
		break;
	case UVT_RECV_FAIL:
		event_emit(stream, UVT_EV_DTLS_FAIL, 0);
		break;
	case UVT_RECV_CLOSED:
		event_emit(stream, UVT_EV_DTLS_CLOSED, 0);
		break;
	case UVT_RECV_NONE:
		break;
	}
}

/**
 * on_state() - the ICE state moved
 * @agent: the ICE agent
 * @state: the new state
 * @user: the stream
 *
 * Context: the libjuice thread.
 */
static void on_state(juice_agent_t *agent, juice_state_t state, void *user)
{
	struct uvt_stream *stream = user;
	int dtls = 0;

	pthread_mutex_lock(&stream->lock);
	stream->ice = (enum uvt_ice_state)state;
	if (dtls_due(stream))
		dtls = dtls_start(stream);
	pthread_mutex_unlock(&stream->lock);

	event_emit(stream, UVT_EV_ICE_STATE, (int)state);
	if (dtls < 0)
		event_emit(stream, UVT_EV_DTLS_FAIL, 0);
}

/**
 * on_candidate() - one local candidate gathered
 * @agent: the ICE agent
 * @sdp: the a=candidate line
 * @user: the stream
 *
 * Context: the libjuice thread.
 */
static void on_candidate(juice_agent_t *agent, const char *sdp, void *user)
{
	struct uvt_stream *stream = user;
	unsigned int pending;

	pthread_mutex_lock(&stream->lock);
	if (stream->cand_count < UVT_CANDIDATE_RING) {
		unsigned int slot = (stream->cand_head + stream->cand_count) %
				    UVT_CANDIDATE_RING;

		snprintf(stream->cand[slot], UVT_CANDIDATE_MAX, "%s", sdp);
		stream->cand_count++;
	}
	pending = stream->cand_count;
	pthread_mutex_unlock(&stream->lock);

	event_emit(stream, UVT_EV_CANDIDATE, (int)pending);
}

/**
 * on_gathering_done() - the agent gathered its last candidate
 * @agent: the ICE agent
 * @user: the stream
 *
 * Context: the libjuice thread.
 */
static void on_gathering_done(juice_agent_t *agent, void *user)
{
	event_emit(user, UVT_EV_GATHERING_DONE, 0);
}

/**
 * juice_cfg_fill() - the libjuice configuration of a stream
 * @stream: the stream, whose own copies the pointers name
 * @config: receives the configuration
 */
static void juice_cfg_fill(struct uvt_stream *stream, juice_config_t *config)
{
	unsigned int i;

	memset(config, 0, sizeof(*config));
	config->concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
	if (stream->cfg.stun_host[0]) {
		config->stun_server_host = stream->cfg.stun_host;
		config->stun_server_port = stream->cfg.stun_port;
	}
	for (i = 0; i < stream->cfg.turns && i < UVT_TURN_MAX; i++) {
		stream->turn[i].host = stream->cfg.turn[i].host;
		stream->turn[i].username = stream->cfg.turn[i].user;
		stream->turn[i].password = stream->cfg.turn[i].pass;
		stream->turn[i].port = stream->cfg.turn[i].port;
	}
	config->turn_servers = stream->turn;
	config->turn_servers_count = (int)i;
	if (stream->cfg.bind_address[0])
		config->bind_address = stream->cfg.bind_address;
	config->local_port_range_begin = stream->cfg.port_min;
	config->local_port_range_end = stream->cfg.port_max;
	config->cb_state_changed = on_state;
	config->cb_candidate = on_candidate;
	config->cb_gathering_done = on_gathering_done;
	config->cb_recv = on_recv;
	config->user_ptr = stream;
}

struct uvt_stream *uvt_create(const struct uvt_cfg *cfg)
{
	pthread_mutexattr_t attr;
	juice_config_t config;
	struct uvt_stream *stream;

	if (!cfg || uvt_init())
		return NULL;

	stream = calloc(1, sizeof(*stream));
	if (!stream)
		return NULL;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&stream->lock, &attr);
	pthread_mutexattr_destroy(&attr);

	stream->cfg = *cfg;
	stream->setup = cfg->setup;
	stream->ice = UVT_ICE_DISCONNECTED;
	juice_cfg_fill(stream, &config);

	stream->agent = juice_create(&config);
	if (!stream->agent) {
		pthread_mutex_destroy(&stream->lock);
		free(stream);
		return NULL;
	}

	if (cfg->role == UVT_ROLE_CONTROLLING &&
	    juice_gather_candidates(stream->agent) == 0)
		stream->gathered = true;

	return stream;
}

/**
 * credential_ok() - bound one ICE credential
 * @str: the credential
 * @min: the shortest length allowed
 * @max: the longest length allowed
 *
 * Return: true when the length is inside the bounds.
 */
static bool credential_ok(const char *str, size_t min, size_t max)
{
	size_t len = str ? strlen(str) : 0;

	return len >= min && len <= max;
}

int uvt_remote_set(struct uvt_stream *stream, const char *ufrag,
		   const char *pwd,
		   const uint8_t fingerprint[UVT_FINGERPRINT_LEN],
		   enum uvt_setup setup)
{
	char desc[UVT_DESC_MAX];
	bool gather;
	int dtls = 0;

	if (!stream || !fingerprint ||
	    !credential_ok(ufrag, 4, UVT_UFRAG_MAX - 1) ||
	    !credential_ok(pwd, 22, UVT_PWD_MAX - 1))
		return -EINVAL;

	pthread_mutex_lock(&stream->lock);
	if (stream->remote_set) {
		pthread_mutex_unlock(&stream->lock);
		return -EALREADY;
	}
	snprintf(desc, sizeof(desc), "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\n", ufrag,
		 pwd);
	if (juice_set_remote_description(stream->agent, desc) < 0) {
		pthread_mutex_unlock(&stream->lock);
		return -EINVAL;
	}
	stream->remote_set = true;
	memcpy(stream->remote_fp, fingerprint, UVT_FINGERPRINT_LEN);
	stream->remote_fp_set = true;
	if (stream->setup == UVT_SETUP_ACTPASS)
		stream->setup = setup == UVT_SETUP_ACTIVE ? UVT_SETUP_PASSIVE :
							    UVT_SETUP_ACTIVE;
	if (dtls_due(stream))
		dtls = dtls_start(stream);
	gather = !stream->gathered;
	if (gather)
		stream->gathered = true;
	pthread_mutex_unlock(&stream->lock);

	if (gather)
		juice_gather_candidates(stream->agent);
	if (dtls < 0)
		event_emit(stream, UVT_EV_DTLS_FAIL, 0);

	return 0;
}

int uvt_candidate_add(struct uvt_stream *stream, const char *sdp)
{
	if (!stream || !sdp)
		return -EINVAL;

	return juice_add_remote_candidate(stream->agent, sdp) < 0 ? -EINVAL : 0;
}

void uvt_remote_done(struct uvt_stream *stream)
{
	if (stream)
		juice_set_remote_gathering_done(stream->agent);
}

/**
 * desc_value() - the value of one attribute line of a libjuice description
 * @desc: the description text
 * @key: the attribute with its colon, such as "a=ice-pwd:"
 * @out: receives the value
 * @len: capacity of @out
 *
 * Return: true when the line exists and its value fits.
 */
static bool desc_value(const char *desc, const char *key, char *out,
		       size_t len)
{
	const char *value = strstr(desc, key);
	size_t span;

	if (!value)
		return false;
	value += strlen(key);
	span = strcspn(value, "\r\n");
	if (span >= len)
		return false;
	memcpy(out, value, span);
	out[span] = 0;

	return true;
}

int uvt_local_get(struct uvt_stream *stream, char *ufrag, char *pwd)
{
	char desc[UVT_DESC_MAX];

	if (!stream || !ufrag || !pwd)
		return -EINVAL;
	if (juice_get_local_description(stream->agent, desc, sizeof(desc)) < 0)
		return -EIO;
	if (!desc_value(desc, "a=ice-ufrag:", ufrag, UVT_UFRAG_MAX) ||
	    !desc_value(desc, "a=ice-pwd:", pwd, UVT_PWD_MAX))
		return -EPROTO;

	return 0;
}

int uvt_candidate_next(struct uvt_stream *stream, char *out, size_t len)
{
	if (!stream || !out || !len)
		return 0;

	pthread_mutex_lock(&stream->lock);
	if (!stream->cand_count) {
		pthread_mutex_unlock(&stream->lock);
		return 0;
	}
	snprintf(out, len, "%s", stream->cand[stream->cand_head]);
	stream->cand_head = (stream->cand_head + 1) % UVT_CANDIDATE_RING;
	stream->cand_count--;
	pthread_mutex_unlock(&stream->lock);

	return 1;
}

/**
 * media_send() - protect one packet and send it through the agent
 * @stream: the stream
 * @buf: the plain packet
 * @len: its length
 * @rtcp: whether the packet is RTCP
 *
 * Return: 0, -EAGAIN before DTLS is up, or another negative errno.
 */
static int media_send(struct uvt_stream *stream, const uint8_t *buf, size_t len,
		      bool rtcp)
{
	_Alignas(uint32_t) uint8_t pkt[UVT_BUF_MAX];
	size_t out = 0;
	int ret;

	if (!stream || !buf || !len || len > UVT_PACKET_MAX)
		return -EINVAL;

	pthread_mutex_lock(&stream->lock);
	if (!stream->dtls_up) {
		pthread_mutex_unlock(&stream->lock);
		return -EAGAIN;
	}
	if (stream->dtls_closed) {
		pthread_mutex_unlock(&stream->lock);
		return -EPIPE;
	}
	memcpy(pkt, buf, len);
	ret = rtcp ? uvsrtp_protect_rtcp(stream->srtp, pkt, len, sizeof(pkt),
					 &out) :
		     uvsrtp_protect(stream->srtp, pkt, len, sizeof(pkt), &out);
	if (!ret && juice_send(stream->agent, (const char *)pkt, out) < 0)
		ret = -EIO;
	pthread_mutex_unlock(&stream->lock);

	return ret;
}

int uvt_send_rtp(struct uvt_stream *stream, const uint8_t *buf, size_t len)
{
	return media_send(stream, buf, len, false);
}

int uvt_send_rtcp(struct uvt_stream *stream, const uint8_t *buf, size_t len)
{
	return media_send(stream, buf, len, true);
}

void uvt_tick(struct uvt_stream *stream)
{
	struct timeval left;
	bool fail = false;

	if (!stream)
		return;

	pthread_mutex_lock(&stream->lock);
	if (stream->ssl && !stream->dtls_up && !stream->dtls_failed &&
	    DTLSv1_get_timeout(stream->ssl, &left) && !left.tv_sec &&
	    !left.tv_usec && DTLSv1_handle_timeout(stream->ssl) < 0) {
		stream->dtls_failed = true;
		fail = true;
	}
	pthread_mutex_unlock(&stream->lock);

	if (fail)
		event_emit(stream, UVT_EV_DTLS_FAIL, 0);
}

enum uvt_ice_state uvt_ice_state(struct uvt_stream *stream)
{
	enum uvt_ice_state state;

	if (!stream)
		return UVT_ICE_FAILED;
	pthread_mutex_lock(&stream->lock);
	state = stream->ice;
	pthread_mutex_unlock(&stream->lock);

	return state;
}

bool uvt_up(struct uvt_stream *stream)
{
	bool up;

	if (!stream)
		return false;
	pthread_mutex_lock(&stream->lock);
	up = stream->dtls_up && !stream->dtls_failed && !stream->dtls_closed;
	pthread_mutex_unlock(&stream->lock);

	return up;
}

int uvt_close(struct uvt_stream *stream)
{
	int ret = 0;

	if (!stream)
		return -ENOTCONN;

	pthread_mutex_lock(&stream->lock);
	if (!stream->dtls_up || stream->dtls_closed) {
		ret = -ENOTCONN;
	} else {
		stream->dtls_closed = true;
		SSL_shutdown(stream->ssl);
	}
	pthread_mutex_unlock(&stream->lock);

	return ret;
}

void uvt_destroy(struct uvt_stream *stream)
{
	if (!stream)
		return;

	juice_destroy(stream->agent);
	stream->agent = NULL;
	SSL_free(stream->ssl);
	uvsrtp_destroy(stream->srtp);
	explicit_bzero(stream->remote_fp, sizeof(stream->remote_fp));
	pthread_mutex_destroy(&stream->lock);
	free(stream);
}
