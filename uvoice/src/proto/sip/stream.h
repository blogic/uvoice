// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * TCP and TLS transport for SIP (src/proto/sip/stream.c).
 *
 * What it is. The stream twin of sip/transport.h. transport.c serves UDP;
 * this module serves the two reliable transports on the same interface the
 * rest of the stack speaks, the single indirection
 * struct sip_txn_env.send(env, buf, len, dst). A transaction stores a
 * sockaddr and never a connection handle: struct sip_stream_env resolves the
 * destination to a connection inside send(), which is what makes the whole
 * stack transport agnostic.
 *
 * Dependencies. uloop, the socket API, sip/frame.h for the framing and
 * sip/tls.h for the record layer. This is a daemon file, like transport.c and
 * txn_uloop.c. It includes sip/transport.h for three shared constants
 * (SIP_TOS_UNSET, SIP_SOCK_IFACE_MAX, SIP_SOCK_HOST_MAX) and for nothing else;
 * no function of the UDP transport is called from here, so the include adds no
 * link dependency.
 *
 * Both families. A connection takes the family of the peer address its key
 * carries, and a listener takes the family its configuration names. A key whose
 * peer is neither AF_INET nor AF_INET6 is refused and counted. One listener per
 * line, in one family. An AF_INET6 listener is opened with IPV6_V6ONLY, so it
 * accepts no IPv4 mapped peer and an accepted connection's key names the family
 * the peer really used. A line that moves to the other family releases its
 * listener and takes a new one, exactly as it rebinds its datagram socket
 * (account.c). Two listeners on one port, one per family, would coexist under
 * IPV6_V6ONLY and this layer does not open them: a line signals in one family
 * at a time, its media follows the signalling (RFC 6157 section 5), and an
 * inbound call on the other family could only be answered with a media address
 * the caller cannot reach.
 *
 * The connection key. A connection is named by
 *
 *   (local identity: local port + interface, peer address + port, kind)
 *
 * and the local identity is in the key on purpose. Two accounts that configure
 * different sip_local_port values must not share one connection: the Via
 * sent-by, the Contact and the rport answer all name the local port, so a
 * shared connection would put one account's port in the other account's
 * messages and break the registrar binding of both. The interface joins for
 * the same reason a UDP socket carries it: a connection scoped to the wrong
 * netdev sends from the wrong address.
 *
 * SNI splits a TLS connection. The key above names the peer by address and
 * port, and two DNS names can resolve to one address and port. Over TLS each
 * name needs its own server_name, and a connection handshaken for name A
 * carries name A's certificate, so a send for name B must not reuse it: it
 * would reach the wrong virtual host. The requested SNI therefore joins the
 * outbound reuse match for a TLS key, held not in the key struct but in the
 * connection's stored sni and compared there. A TCP key ignores SNI (there is
 * no server_name on TCP), and an accepted or SIP_DST_F_NO_CONNECT connection
 * carries an empty sni, so an outbound send never reuses an inbound
 * connection by name.
 *
 * Ephemeral source ports. An outbound connect does not bind the local port
 * first. Binding it would put every connection to one peer in the same
 * four tuple, and a connection that closed leaves that four tuple in
 * TIME_WAIT for two minutes, during which the next connect to the same peer
 * fails with EADDRINUSE. The kernel picks the source port; the local port of
 * the key is the account's configured port, which is what the Via carries and
 * what the listener binds, not the source port of the connection.
 *
 * Listeners. sip_stream_listen() binds sip_local_port for one kind in one
 * family, sets SO_BINDTODEVICE before the bind, as transport.c does, and is
 * refcounted, so two accounts on one port share one listener and the last close
 * releases it.
 *
 * SO_REUSEADDR is set, SO_REUSEPORT is not. This is the opposite of the UDP
 * rule in sip/transport.h, and the difference is TIME_WAIT: a listener that
 * closes leaves accepted connections in TIME_WAIT on its port, and without
 * SO_REUSEADDR the bind after a reload fails until they expire. SO_REUSEPORT
 * is never set, because it would let a second uvoiced take a share of the
 * inbound connections instead of failing to start. A UDP socket takes no
 * SO_REUSEADDR, because a datagram socket has no TIME_WAIT to work around.
 *
 * Connection lifetime. An outbound connection is never idle closed. Behind
 * NAT the connection is the inbound path: closing it makes the device
 * unreachable for an incoming call until the next registration. An inbound
 * connection is idle closed after SIP_STREAM_IDLE_MS, because a peer that
 * opened it and went away must not hold a pool slot forever. When the pool is
 * full, the least recently used connection is evicted, and eviction never
 * takes a connection with queued transmit bytes: those bytes belong to a live
 * transaction.
 *
 * Reconnect backoff. Each connection slot carries its own backoff, doubling
 * from SIP_STREAM_BACKOFF_MS to SIP_STREAM_BACKOFF_MAX_MS. A retransmission
 * series must not become a connect series: the 2xx retransmission of ua.c
 * keeps running over a reliable transport (RFC 3261 13.3.1.4), so a peer that
 * refuses connections would otherwise get one SYN per retransmission. A send
 * that arrives while the backoff is running fails immediately.
 *
 * Rx reentrancy. One read can carry several messages, and dispatching one of
 * them can close the connection: the upcall can answer, the answer can fail to
 * send, an oversize frame can close, a TLS error can close. The receive loop
 * therefore holds the connection handle and not a pointer, revalidates it with
 * sip_stream_conn_get() before every frame_next() call, and stops the moment
 * it no longer resolves. test_sip_stream covers the pipelined close case, with
 * ASan as the detector.
 *
 * Dst provenance. The destination of an accepted connection is the peer's
 * ephemeral source port, which nothing is listening on. Connecting to it would
 * reach no SIP element. Such a destination is therefore marked
 * SIP_DST_F_NO_CONNECT (sip/txn.h) when the receive path reports it, the flag
 * travels with every copy of the destination into the server transaction, and
 * a send to it fails immediately when its connection is gone instead of
 * opening one. The response is then lost; the ua_accept retransmission is
 * what recovers a 2xx whose connection died.
 *
 * Queued send semantics. A send to a destination with no live connection opens
 * a non blocking connect, queues the payload and returns the byte count, that
 * is, success. A later connect or handshake failure kills the queued bytes.
 * For a client request the client transaction's life timer then converts that
 * into a timeout: timer B for an INVITE client transaction and timer F for a
 * non INVITE one (RFC 3261 17.1.1.2, 17.1.2.2). Timer H is not in this path:
 * it is the INVITE server timer for a missing ACK, so it only reaches a queued
 * send in the server response over a dead connection case, which is the lost
 * response of "Dst provenance" above and not a client request. A negative
 * return still terminates the transaction immediately, which is the struct
 * sip_txn_env send() contract. The queue is the per connection
 * SIP_STREAM_TX_MAX staging buffer, of which SIP_FRAME_PONG_LEN bytes are
 * the pong's reserve, so a full queue can never silence the RFC 5626
 * keepalive answer: a payload that does not fit the remainder is refused
 * with a negative return, and nothing here ever allocates.
 *
 * Ping is answered inside the connection. sip_transport_cb carries a ping
 * upcall because the UDP keepalive answer needs the socket and the source
 * address; struct sip_stream_cb does not. A FRAME_PING is answered with one
 * CRLF on the connection it arrived on, inside this module, so a keepalive
 * answer never leaves a TLS connection in the clear: it is inside the record
 * layer. account.c's tp_ping_cb stays UDP only and is not reached from here.
 *
 * Responses travel back on the arriving connection. The receive path reports
 * src as the connection's peer tuple, and on a reliable transport
 * sip_txn_server_start() uses that src verbatim as the response destination
 * rather than the Via sent-by (RFC 3261 18.2.2). The response then lands on
 * the arriving connection by construction, because the key that resolves it is
 * the key that connection carries.
 *
 * Per-send TOS. Two accounts that share a local port share a connection and
 * may carry different sip_tos values, so the byte is a property of the send
 * exactly as in transport.c: setsockopt(IP_TOS) runs when it differs from the
 * byte the connection currently holds.
 *
 * Lifetime. Connections and listeners follow the sip/txn.h handle rule
 * ((index << 16) | generation, sip_handle_make()). A caller stores the handle
 * and never a struct sip_conn pointer.
 *
 * Footprint. Eight connections at a 64 KB receive slab and a 16 KB transmit
 * staging buffer each is 640 KB of BSS. That is the accepted cost of a pool
 * that never allocates; SIP_STREAM_CONN_MAX is the knob if it has to shrink.
 * sip_stream_init zeroes around the slabs and never through them, so a slab
 * costs no resident memory until a connection takes its slot.
 */
#ifndef UVOICE_SIP_STREAM_H
#define UVOICE_SIP_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libubox/uloop.h>

#include "proto/sip/frame.h"
#include "proto/shared/msg.h"
#include "proto/sip/tls.h"
#include "proto/sip/transport.h"
#include "proto/proto_tap.h"
#include "proto/sip/txn.h"

/*
 * Two lines, each with a proxy, a registrar and an outbound proxy, reach at
 * most six peers, and the deployed case reaches one or two. Eight slots covers
 * it with room for an inbound connection or two; a ninth connection evicts the
 * least recently used one.
 */
#define SIP_STREAM_CONN_MAX	8

/* two kinds on two local ports is the worst case the schema can build */
#define SIP_STREAM_LISTEN_MAX	4

/* the per connection receive slab, and the transmit staging buffer */
#define SIP_STREAM_RX_MAX	SIP_FRAME_BUF_MAX
#define SIP_STREAM_TX_MAX	16384

/* longest DNS name kept for SNI, matching account.h ACCT_HOST_MAX */
#define SIP_STREAM_NAME_MAX	128

/* an inbound connection that carried nothing for this long is closed */
#define SIP_STREAM_IDLE_MS	600000

/*
 * The bound on a connect plus a handshake. It is 64*T1, the same bound
 * RFC 3261 17.1.1.2 gives timer B, so a queued request is reported by the
 * transaction timer that owns it and not by a connection timeout that fires
 * first and races it.
 */
#define SIP_STREAM_CONNECT_MS	32000

/* the per connection reconnect backoff: T1, doubling, capped at 64*T1 */
#define SIP_STREAM_BACKOFF_MS		500
#define SIP_STREAM_BACKOFF_MAX_MS	32000

/* SO_KEEPALIVE on a stream connection: idle, interval, probe count */
#define SIP_STREAM_KA_IDLE_S	120
#define SIP_STREAM_KA_INTVL_S	30
#define SIP_STREAM_KA_CNT	3

#define SIP_STREAM_BACKLOG	4

enum sip_stream_kind {
	SIP_STREAM_TCP = 0,
	SIP_STREAM_TLS,
	SIP_STREAM_KIND_MAX,
};

const char *sip_stream_kind_name(enum sip_stream_kind kind);

/*
 * SIP_CONN_CONNECTING  a non blocking connect is in flight; transmit bytes
 *                      queue.
 * SIP_CONN_HANDSHAKE   TLS only: the connect finished, the record layer is
 *                      negotiating.
 * SIP_CONN_UP          messages flow.
 * SIP_CONN_CLOSING     the staged bytes are draining before the socket goes.
 */
enum sip_conn_state {
	SIP_CONN_FREE = 0,
	SIP_CONN_CONNECTING,
	SIP_CONN_HANDSHAKE,
	SIP_CONN_UP,
	SIP_CONN_CLOSING,
};

const char *sip_conn_state_name(enum sip_conn_state state);

/*
 * The connection identity. peer carries the family, the address and the port;
 * only those take part in the comparison, so the SIP_DST_F_NO_CONNECT flag a
 * destination may carry never splits one connection into two. sip_dst_eq()
 * reads the family first, so a peer of one family never equals a peer of the
 * other family on the same port.
 */
struct sip_stream_key {
	struct sip_txn_dst peer;
	char iface[SIP_SOCK_IFACE_MAX];
	uint16_t local_port;
	uint8_t kind;			/* enum sip_stream_kind */
};

bool sip_stream_key_eq(const struct sip_stream_key *a, const struct sip_stream_key *b);

/*
 * addr     bind address in presentation form. NULL or "" binds the wildcard.
 * iface    SO_BINDTODEVICE when it is not NULL and not empty.
 * port     sip_local_port. 0 is refused: a listener needs a known port.
 * kind     TCP or TLS. A TLS listener needs a loaded server context and stays
 *          down without one (sip/tls.h "Client only is a normal state").
 * family   AF_INET or AF_INET6; 0 takes AF_INET, as struct sip_sock_cfg does.
 *          An AF_INET6 listener is opened with IPV6_V6ONLY. It joins the
 *          listener identity, so two families never share one slot.
 * backlog  0 takes SIP_STREAM_BACKLOG.
 */
struct sip_stream_listen_cfg {
	const char *addr;
	const char *iface;
	uint16_t port;
	enum sip_stream_kind kind;
	int family;
	int backlog;
};

struct sip_stream;

struct sip_listener {
	struct uloop_fd ufd;
	struct sip_stream *st;

	char addr[SIP_SOCK_HOST_MAX];
	char iface[SIP_SOCK_IFACE_MAX];
	int family;			/* AF_INET or AF_INET6 */
	uint16_t port;
	uint8_t kind;			/* enum sip_stream_kind */

	unsigned int refs;
	unsigned int index;
	uint16_t gen;
	bool in_use;
	bool up;
};

/*
 * One connection. rx is the framer's slab and tx the transmit staging buffer,
 * both owned here so nothing on this path allocates.
 *
 * used_ms serves two rules at once: the inbound idle close and the LRU
 * eviction order. deadline_ms bounds a connect and a handshake. backoff_until
 * and backoff_ms carry the per connection reconnect backoff across the slot's
 * reuse for the same key.
 */
struct sip_conn {
	struct uloop_fd ufd;
	struct sip_stream *st;

	struct sip_stream_key key;
	struct trace_endpoint local;
	struct sip_framer fr;
	struct sip_tls_conn tls;

	char sni[SIP_STREAM_NAME_MAX];	/* "" when the target was an address */

	uint64_t used_ms;
	uint64_t deadline_ms;
	uint64_t backoff_until;
	uint32_t backoff_ms;

	uint64_t frames;		/* messages this connection produced */
	uint32_t tos_cur;		/* SIP_TOS_UNSET, or the byte */

	size_t tx_len;			/* staged bytes */
	size_t tx_off;			/* staged bytes already written */

	unsigned int index;
	uint16_t gen;
	uint8_t state;			/* enum sip_conn_state */
	bool in_use;
	bool inbound;			/* accepted, not connected */

	char rx[SIP_STREAM_RX_MAX];
	char tx[SIP_STREAM_TX_MAX];
};

/*
 * The counters of struct sip_sock_stat that a stream also produces, plus the
 * ones only a connection has. Two UDP counters have no stream meaning and are
 * left out rather than kept at zero: truncated (a stream never truncates, an
 * oversize message is a frame error that closes the connection) and rebinds
 * (a stream connection is closed and reopened, never rebound in place).
 */
struct sip_stream_stat {
	uint64_t rx;			/* messages passed to the callback */
	uint64_t rx_bytes;
	uint64_t tx;			/* messages written or staged */
	uint64_t tx_bytes;
	uint64_t tx_err;		/* refused: no room, no route, dead conn */
	uint64_t rx_err;		/* read failed */
	uint64_t parse_err;		/* msg_parse refused a complete frame */
	uint64_t ping;			/* RFC 5626 5.4 pings answered */
	uint64_t pong;			/* pongs counted, never answered */
	uint64_t tos_err;		/* setsockopt refused the TOS byte */

	uint64_t connects;		/* outbound connects started */
	uint64_t accepts;		/* inbound connections taken */
	uint64_t handshakes_failed;	/* TLS handshakes that did not finish */
	uint64_t frames;		/* complete messages the framer produced */
	uint64_t frame_errs;		/* FRAME_ERR_*, each one closed a conn */
	uint64_t evictions;		/* LRU closes to make room */
	uint64_t reconnects;		/* connects to a key that had one before */
	uint64_t queued;		/* sends staged on a connection not yet up */
	uint64_t denied;		/* pool full and nothing was evictable */
};

/*
 * msg  a parsed message. Both the message and every string in it point into
 *      the connection's receive slab and are valid for the call only
 *      (sip/txn.h "Buffer contract"). conn is the handle the message arrived
 *      on; the callback may close it, and the receive loop revalidates the
 *      handle afterwards ("Rx reentrancy" above).
 *
 * conn a connection changed state. It is what the account layer needs for the
 *      "Reconnect re-registers" rule: a transition to SIP_CONN_UP on a key
 *      that already had a connection is a reconnect, and the registrar's
 *      binding still names the old connection's ephemeral five tuple, so the
 *      account forces a REGISTER refresh. A connection that the peer opened
 *      (inbound) forces none: the peer already reaches the line through it
 *      (account.h acct_reconnect_needed). An event is the only mechanism
 *      that catches a die and reopen between two polls. state is the new
 *      state; SIP_CONN_FREE reports a close. It may be NULL for a caller that
 *      does not track connection state.
 *
 * There is no ping upcall; see "Ping is answered inside the connection" above.
 */
struct sip_stream_cb {
	void (*msg)(struct sip_stream *st, uint32_t conn, struct sip_msg *msg,
		    const struct sip_txn_dst *src, void *priv);
	void (*conn)(struct sip_stream *st, uint32_t conn, enum sip_conn_state state,
		     void *priv);
};

struct sip_stream {
	struct sip_stream_cb cb;
	void *priv;

	/*
	 * The TLS contexts, owned by the caller. NULL refuses every
	 * SIP_STREAM_TLS key, which is the state before the certificate
	 * bundle lands.
	 */
	struct sip_tls *tls;

	struct uloop_timeout tick;	/* the idle and backoff sweep */

	struct sip_listener listen[SIP_STREAM_LISTEN_MAX];
	struct sip_conn conn[SIP_STREAM_CONN_MAX];
	unsigned int active;

	struct sip_stream_stat stat;
};

void sip_stream_init(struct sip_stream *st, const struct sip_stream_cb *cb, void *priv,
		     struct sip_tls *tls);
void sip_stream_done(struct sip_stream *st);

/*
 * Open a listener, or take another reference on the one that already serves
 * this address, family, port and kind. SIP_HANDLE_NONE means the bind failed,
 * the pool is full, or a TLS listener was asked for with no server context;
 * errno carries the reason of a failed bind.
 */
uint32_t sip_stream_listen(struct sip_stream *st, const struct sip_stream_listen_cfg *cfg);

/* Drop one reference. The listener closes when the last one goes. */
void sip_stream_listen_close(struct sip_stream *st, uint32_t handle);

struct sip_listener *sip_stream_listener_get(struct sip_stream *st, uint32_t handle);
unsigned int sip_stream_listen_count(const struct sip_stream *st);

/*
 * Send one message. Returns the number of bytes accepted, or a negative value,
 * which is exactly the struct sip_txn_env send() contract. The bytes are
 * written to a live connection, or staged on one that is still connecting or
 * handshaking ("Queued send semantics" above). A negative return means the
 * destination is unreachable now: no room in the staging buffer, a backoff in
 * progress, a SIP_DST_F_NO_CONNECT destination with no live connection, a TLS
 * key with no context, or a full pool with nothing evictable.
 *
 * tos is the full TOS byte and is applied to the connection before the write
 * when it differs from the one it holds.
 */
int sip_stream_send(struct sip_stream *st, const struct sip_stream_key *key, uint8_t tos,
		    const void *buf, size_t len);

/*
 * The same send with the SNI name the target came from, which the connection
 * uses if this send is what opens it. NULL, and sip_stream_send() itself, send
 * no server_name extension. The split follows sip_dns_resolve() and
 * sip_dns_resolve_svc(): the plain entry point keeps its signature and the
 * longer one carries what only the caller knows.
 */
int sip_stream_send_sni(struct sip_stream *st, const struct sip_stream_key *key, uint8_t tos,
			const void *buf, size_t len, const char *sni);

struct sip_conn *sip_stream_conn_get(struct sip_stream *st, uint32_t handle);
uint32_t sip_stream_conn_handle(const struct sip_conn *c);

/*
 * The connection that serves this key, or NULL. For a TLS key the match also
 * requires the connection's stored sni to equal sni ("SNI splits a TLS
 * connection" above): NULL and "" mean no server_name and match a connection
 * with an empty sni. For a TCP key sni is ignored. This is the reuse decision
 * the send path takes, and an account status query passes the SNI its own
 * line would send with.
 */
struct sip_conn *sip_stream_conn_find(struct sip_stream *st, const struct sip_stream_key *key,
				      const char *sni);
unsigned int sip_stream_conn_count(const struct sip_stream *st);

/* staged bytes still waiting for the socket, for the ubus status contract */
size_t sip_stream_conn_queued(const struct sip_conn *c);

/*
 * Close one connection, every connection, or every connection scoped to one
 * interface. The interface form is what an address change uses: account.c
 * rebinds its sockets and the affected connections go with them, and the next
 * send opens a new one. Both bulk forms return how many were closed.
 */
void sip_stream_conn_close(struct sip_stream *st, uint32_t handle);
unsigned int sip_stream_close_iface(struct sip_stream *st, const char *iface);
unsigned int sip_stream_close_all(struct sip_stream *st);

const struct sip_stream_stat *sip_stream_stat(const struct sip_stream *st);

/* ------------------------------------------------- transaction environment */

/*
 * The struct sip_txn_env of one account over a reliable transport: the stream
 * pool it sends through, the local identity every key it builds carries, the
 * kind, and the TOS byte it marks with. env is the first member, so the
 * transaction layer receives &se->env and the send path recovers the binding
 * from env->base.priv, exactly like struct sip_udp_env.
 *
 * sip_stream_env_init() sets SIP_TXN_ENV_RELIABLE in env.flags. That is what
 * turns off the retransmission timers in sip/txn.c (sip/txn.h "Reliable
 * transport timers"), so an account that builds this environment gets the
 * reliable timer set without a second call.
 */
struct sip_stream_env {
	struct sip_txn_env env;
	struct sip_stream *st;

	char iface[SIP_SOCK_IFACE_MAX];
	char sni[SIP_STREAM_NAME_MAX];	/* "" when the target is an address */
	uint16_t local_port;
	uint8_t kind;			/* enum sip_stream_kind */
	uint8_t tos;
};

void sip_stream_env_init(struct sip_stream_env *se, struct sip_stream *st,
			 enum sip_stream_kind kind, uint16_t local_port, const char *iface,
			 uint8_t tos);
void sip_stream_env_tos_set(struct sip_stream_env *se, uint8_t tos);

/*
 * The local identity after a rebind, and the name for SNI. Both are copied:
 * an account that reloads must not leave the environment pointing at strings
 * of the configuration it just replaced.
 */
void sip_stream_env_local_set(struct sip_stream_env *se, uint16_t local_port,
			      const char *iface);
void sip_stream_env_sni_set(struct sip_stream_env *se, const char *name);

/*
 * The key this environment builds for one destination, exported because the
 * account layer needs it to ask for a connection's state without repeating the
 * identity rule. False for a destination this environment cannot serve, which
 * means an address that is neither AF_INET nor AF_INET6.
 */
bool sip_stream_env_key(const struct sip_stream_env *se, const struct sip_txn_dst *dst,
			struct sip_stream_key *out);

#endif
