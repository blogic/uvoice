// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * UDP transport for SIP (src/proto/sip/transport.c).
 *
 * Dependencies. uloop and the socket API. This is a daemon file, like
 * txn_uloop.c: it is the piece that turns the abstract struct sip_txn_env into
 * real datagrams. sip/txn.c, ua.c, reg.c and dialog.c stay socket free and keep
 * running on a virtual clock in their own suites.
 *
 * One socket per (family, address, port, interface). The bind address and the
 * family join the (local_port, bind_iface) pair in the key because two
 * accounts that ask for one port must also agree on which address it is bound
 * to. sip_sock_open() returns the same socket, with one more reference, for a
 * repeated key, so two accounts on port 5060 share one file descriptor and one
 * receive path.
 *
 * SO_REUSEADDR is not set. A second uvoiced, or a stale one, must fail to bind
 * instead of silently sharing the port with the running daemon.
 *
 * Per-send TOS. Accounts that share a port may carry different sip_tos values,
 * so the byte is a property of the send and not of the socket: sip_sock_send()
 * applies it with setsockopt(IP_TOS) or setsockopt(IPV6_TCLASS) when it differs
 * from the value the socket currently holds, then calls sendto(). The value is
 * the full 8 bit TOS field; a caller that configures DSCP passes dscp << 2.
 *
 * Receive. One 64 KB buffer for the whole transport, filled by recvmsg() with
 * MSG_TRUNC so a datagram larger than the limit is counted and dropped rather
 * than parsed as a short message. The source address of every datagram is
 * captured and handed to the callback, which is what RFC 3581 rport and
 * RFC 3261 18.2.2 need.
 *
 * Buffer lifetime. The struct sip_msg the callback receives points into the
 * shared receive buffer and dies when the callback returns. Anything that has
 * to survive is copied by the layer that keeps it (sip/txn.h "Buffer
 * contract").
 *
 * Egress Via. Every request uvoice sends carries ;rport (RFC 3581), because
 * the device sits behind NAT in the deployed case. The Via itself is built by
 * the caller, so this layer offers the two things the caller cannot work out
 * by itself: the bound port after an ephemeral bind
 * (sip_sock_port(), from getsockname) and the source address the kernel picks
 * for a destination (sip_sock_source_host()). sip_sock_via_write() puts both
 * together and always sets ;rport.
 *
 * Rebind. An interface IP change invalidates a socket bound to that address.
 * sip_sock_rebind() closes and reopens it with the same configuration and the
 * same handle, so the accounts keep their references. Rebinding does not
 * re-register anything: the caller drives that, because only the caller knows
 * whether dns_cache_update_mode and the account state call for it.
 *
 * Lifetime. Sockets follow the sip/txn.h handle rule ((index << 16) |
 * generation). A caller stores the handle, never a struct sip_sock pointer.
 */
#ifndef UVOICE_SIP_TRANSPORT_H
#define UVOICE_SIP_TRANSPORT_H

#include <net/if.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libubox/uloop.h>

#include "proto/shared/hdr.h"
#include "proto/shared/msg.h"
#include "proto/sip/txn.h"

/*
 * Two lines with a proxy, a registrar and an outbound proxy each still share
 * one or two local ports, so two lines in two families on distinct local
 * ports fill four sockets. A line that moves family opens the new socket
 * before it releases the old one, and both lines can move at once, so eight
 * slots leave that headroom. A ninth open is refused and counted.
 */
#define SIP_SOCK_MAX		8

/* the 64 KB receive buffer; a larger datagram is dropped and traced */
#define SIP_RX_BUF_MAX		65536

#define SIP_SOCK_IFACE_MAX	IF_NAMESIZE
#define SIP_SOCK_HOST_MAX	64

/* the TOS byte the socket holds before the first send sets one */
#define SIP_TOS_UNSET		0x10000u

struct sip_transport;

/*
 * family  AF_INET or AF_INET6; 0 takes AF_INET. An AF_INET6 socket is opened
 *         with IPV6_V6ONLY, so the two families never share a port by accident
 *         and a v6 socket never reports a v4 mapped source address.
 * addr    bind address in presentation form. NULL or "" binds the wildcard.
 * port    0 binds an ephemeral port; sip_sock_port() reports the real one.
 * iface   SO_BINDTODEVICE when it is not NULL and not empty.
 * rx_max  largest accepted datagram; 0 takes SIP_RX_BUF_MAX. A larger datagram
 *         is dropped and counted in truncated.
 */
struct sip_sock_cfg {
	int family;
	const char *addr;
	uint16_t port;
	const char *iface;
	size_t rx_max;
};

struct sip_sock_stat {
	uint64_t rx;			/* datagrams passed to the callback */
	uint64_t rx_bytes;
	uint64_t tx;
	uint64_t tx_bytes;
	uint64_t tx_err;		/* sendto refused, or the socket is down */
	uint64_t truncated;		/* datagram larger than rx_max, dropped */
	uint64_t rx_err;		/* recvmsg failed */
	uint64_t parse_err;		/* msg_parse refused the datagram */
	uint64_t ping;			/* CRLF only: a NAT keepalive */
	uint64_t tos_err;		/* setsockopt refused the TOS byte */
	uint64_t rebinds;
};

struct sip_sock {
	struct uloop_fd ufd;
	struct sip_transport *tp;

	struct sip_sock_cfg cfg;
	char addr[SIP_SOCK_HOST_MAX];		/* the configured bind address */
	char iface[SIP_SOCK_IFACE_MAX];
	char bound[SIP_SOCK_HOST_MAX];		/* getsockname after the bind */
	uint16_t port;				/* getsockname after the bind */

	uint32_t tos_cur;			/* SIP_TOS_UNSET, or the byte */
	unsigned int refs;
	unsigned int index;
	uint16_t gen;
	bool in_use;
	bool up;				/* false after a failed rebind */

	struct sip_sock_stat stat;
};

/*
 * msg   a parsed datagram. Both the message and every string in it point into
 *       the transport receive buffer and are valid for the call only.
 * ping  a datagram that carried nothing but CRLF: the NAT keepalive of
 *       keepalive.c. It is reported separately so the keepalive layer can
 *       answer it without the parser calling it an error. len is the datagram
 *       length, so the receiver can tell the RFC 5626 4.4.1 ping (double CRLF,
 *       answered with a pong) from a pong (single CRLF, never answered).
 * Both may be NULL.
 */
struct sip_transport_cb {
	void (*msg)(struct sip_transport *tp, uint32_t sock, struct sip_msg *msg,
		    const struct sip_txn_dst *src, void *priv);
	void (*ping)(struct sip_transport *tp, uint32_t sock, size_t len,
		     const struct sip_txn_dst *src, void *priv);
};

struct sip_transport {
	struct sip_transport_cb cb;
	void *priv;

	struct sip_sock sock[SIP_SOCK_MAX];
	unsigned int active;

	struct sip_sock_stat total;
	uint64_t denied;			/* opens refused, pool full */

	char rx[SIP_RX_BUF_MAX];
};

void sip_transport_init(struct sip_transport *tp, const struct sip_transport_cb *cb, void *priv);
void sip_transport_done(struct sip_transport *tp);

/*
 * Open a socket, or take another reference on the one that already serves this
 * key. SIP_HANDLE_NONE means the bind failed or the pool is full; errno carries
 * the reason of a failed bind.
 */
uint32_t sip_sock_open(struct sip_transport *tp, const struct sip_sock_cfg *cfg);

/* Drop one reference. The socket closes when the last one goes. */
void sip_sock_close(struct sip_transport *tp, uint32_t handle);

struct sip_sock *sip_sock_get(struct sip_transport *tp, uint32_t handle);
uint32_t sip_sock_handle(const struct sip_sock *sk);
unsigned int sip_sock_count(const struct sip_transport *tp);

uint16_t sip_sock_port(const struct sip_sock *sk);
const char *sip_sock_iface(const struct sip_sock *sk);
bool sip_sock_up(const struct sip_sock *sk);

/* the address the socket is bound to; "0.0.0.0" or "::" for a wildcard bind */
bool sip_sock_local_host(const struct sip_sock *sk, char *out, size_t olen);

/*
 * The source address the kernel would use toward dst, which is the Via sent-by
 * host of a socket bound to the wildcard. A socket bound to one address
 * answers with that address without asking the kernel. The lookup opens a
 * scratch socket, connects it, reads getsockname() and closes it again; no
 * datagram leaves the machine.
 */
bool sip_sock_source_host(const struct sip_sock *sk, const struct sip_txn_dst *dst,
			  char *out, size_t olen);

/*
 * Via for a request that leaves through this socket, with ;rport always set.
 * dst may be NULL, in which case the sent-by host is the bound address.
 */
bool sip_sock_via_write(struct sip_transport *tp, uint32_t handle, struct sip_buf *b,
			const struct sip_txn_dst *dst, const char *branch);

/*
 * Close and reopen the socket under the same handle, for an interface that
 * changed its address. The port is preserved when the configuration named one;
 * a socket that was bound to an ephemeral port gets a new port, which the
 * caller must read back before it builds another Via. False leaves the socket
 * down: sends fail and the caller can try again.
 */
bool sip_sock_rebind(struct sip_transport *tp, uint32_t handle);

/*
 * Replace the device a socket scopes to (SO_BINDTODEVICE) so the next rebind
 * applies it: a netifd move of the logical interface onto another netdev
 * otherwise leaves the socket scoped to the old one forever, because the
 * rebind reuses the device captured at open. NULL or "" makes the next bind
 * unscoped. The running socket is untouched until it rebinds.
 */
bool sip_sock_iface_set(struct sip_transport *tp, uint32_t handle, const char *iface);

/* Rebind every open socket. Returns how many failed. */
unsigned int sip_transport_rebind_all(struct sip_transport *tp);

/*
 * Rebind the sockets of the pool that are down and leave the working ones
 * alone. Returns how many are still down. A rebind closes the file descriptor,
 * and the flow of a working socket loses its conntrack tuple with it, so a
 * recovery pass must touch the failed sockets only.
 */
unsigned int sip_transport_rebind_down(struct sip_transport *tp);

/*
 * How many sockets of the pool are down, which is how many a rebind failed on.
 * A socket that is down holds a reference and a slot, and no file descriptor.
 */
unsigned int sip_transport_down_count(const struct sip_transport *tp);

/*
 * Send one datagram. Returns the number of bytes sent, or a negative value,
 * which is exactly the struct sip_txn_env send() contract. tos is the full TOS
 * byte and is applied before the send when it differs from the one the socket
 * holds.
 */
int sip_sock_send(struct sip_transport *tp, uint32_t handle, uint8_t tos, const void *buf,
		  size_t len, const struct sip_txn_dst *dst);

const struct sip_sock_stat *sip_transport_stat(const struct sip_transport *tp);

/* ------------------------------------------------- transaction environment */

/*
 * The struct sip_txn_env of one account: the socket it sends through, the TOS
 * byte it marks with, and the uloop timer implementation from txn_uloop.c.
 * env is the first member, so the transaction layer receives &ue->env and the
 * send path recovers the binding from env->base.priv.
 *
 * The trace hook is here too: every line the transaction layer and the UA core
 * produce reaches TRACE(TRACE_SIP, ...) through this environment, so a sink
 * that attaches inside trace.c needs no call site change.
 */
struct sip_udp_env {
	struct sip_txn_env env;
	struct sip_transport *tp;
	uint32_t sock;
	uint8_t tos;
};

void sip_udp_env_init(struct sip_udp_env *ue, struct sip_transport *tp, uint32_t sock,
		      uint8_t tos);
void sip_udp_env_tos_set(struct sip_udp_env *ue, uint8_t tos);
void sip_udp_env_sock_set(struct sip_udp_env *ue, uint32_t sock);

#endif
