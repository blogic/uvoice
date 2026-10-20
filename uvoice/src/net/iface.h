// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The netifd tracker of the voice interface (src/net/iface.c). It asks
 * netifd for the voice interface and for every interface on its netdev,
 * follows the interface events, and keeps one record: the netdev, the IPv4
 * address, the global IPv6 address of the netdev and the down flag. It owns
 * the one recovery timer of the daemon, which asks netifd again for an answer
 * that never arrived and asks the subscriber to reopen what it holds down.
 *
 * The record, the change report and the subscriber are proto/proto.h types,
 * because a protocol reads them through struct proto_host. Everything here runs
 * on the uloop thread.
 */
#ifndef UVOICE_NET_IFACE_H
#define UVOICE_NET_IFACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "cfg/cfg.h"
#include "proto/proto.h"

struct blob_attr;
struct blob_buf;
struct ubus_context;

/*
 * The recovery delay doubles from NET_IFACE_RECOVER_MS and stops at
 * NET_IFACE_RECOVER_MAX_MS, so a permanent fault costs one attempt per
 * minute.
 */
#define NET_IFACE_RECOVER_MS		5000
#define NET_IFACE_RECOVER_MAX_MS	60000

/*
 * Take the voice interface of cfg and, with ubus attached, ask netifd at once.
 * It runs before the protocol starts, so the first bind finds the netdev.
 */
void net_iface_init(const struct cfg *cfg);

/*
 * Subscribe to the netifd interface events on ctx and ask for the current
 * state. Called from the ubus connect callback.
 */
void net_iface_ubus_attach(struct ubus_context *ctx);

/*
 * Install the one subscriber, or remove it with NULL. The tracker copies the
 * record. Every move of the record reaches the protocol through it.
 */
void net_iface_subscribe(const struct net_iface_sub *sub);

/* The record of the moment, never NULL. */
const struct net_iface_state *net_iface_state_get(void);

/*
 * The netdev SO_BINDTODEVICE gets for iface, or NULL for an unscoped bind:
 * the tracked netdev when iface is the voice interface and the kernel knows
 * the netdev, else iface when the kernel knows it.
 */
const char *net_iface_bind_device(const char *iface);

/*
 * The source address the kernel picks toward sa, as text of family, from a
 * transient connected datagram socket scoped to dev; dev NULL or empty is no
 * scope. False for a family that is neither AF_INET nor AF_INET6, an sa of
 * another family, an unknown device or no route.
 */
bool net_iface_source_host_probe(const char *dev, int family,
				 const struct sockaddr *sa, socklen_t salen,
				 char *out, size_t olen);

/*
 * Set the IPv6 served gate: the operator switch and HAL_FEAT_IPV6_MEDIA
 * together. A move reaches the subscriber as a change with served set.
 */
void net_iface_ipv6_served_set(bool served);

/*
 * Repair what can be repaired through the subscriber, then arm the next
 * attempt or disarm. The protocol calls it after a reload or a certificate
 * bundle moved its sockets.
 */
void net_iface_recover_settle(void);

/*
 * Stop every tracker callback and the recovery timer, for the shutdown
 * drain. A netifd answer or event that arrives after it changes nothing.
 */
void net_iface_quiesce(void);

/*
 * A reload names another voice interface: forget the address, the netdev and
 * the down flag, disarm the recovery and ask netifd again.
 */
void net_iface_iface_set(const char *iface);

/*
 * The network table of "voice status": address, address6 and l3_device, each
 * present while known, and no table while none is.
 */
void net_iface_status_dump(struct blob_buf *b);

/*
 * The voice interface address of one family in network byte order: four bytes
 * for ipver 4, sixteen for ipver 6. 0 with the bytes written, -EINVAL for
 * another version, -ENOENT while netifd named no address of that family.
 */
int net_iface_address(uint8_t ipver, uint8_t addr[16]);

/*
 * Whether an answer rebinds: a changed address, an address back after an
 * ifdown, or a netdev move.
 */
bool net_iface_rebind_needed(bool addr_changed, bool iface_was_down,
			     bool dev_changed);

/*
 * Whether a failed netifd lookup asks for another attempt. status is the enum
 * ubus_msg_status of ubus_lookup_id(); UBUS_STATUS_NOT_FOUND asks for none.
 */
bool net_iface_retry_needed(int status);

/*
 * Whether the kernel carries a netdev of this name, answered from sysfs so
 * that an absent name never asks the kernel to load a module for it. The name
 * rules are the kernel's dev_valid_name(); an address alias (a colon) reads
 * as absent. Exported for the suite, beside the rule below.
 */
bool net_iface_netdev_present(const char *name);

/*
 * The pure rule behind net_iface_bind_device(): the netifd l3_device when iface
 * is base_iface and the kernel knows that device, else iface when the kernel
 * knows it, else NULL. Exported so the suite drives the rule without a sysfs.
 */
const char *net_iface_bind_device_pick(const char *iface,
				       const char *base_iface,
				       const char *l3_device, bool iface_known,
				       bool l3_known);

/* Disarm the recovery and forget the record. */
void net_iface_done(void);

/*
 * Test control, never used by the daemon. The three injects hand a scripted
 * netifd status answer, dump answer or interface event to the tracker the way
 * ubus would; a NULL message is ignored.
 */
void net_iface_test_status_inject(struct blob_attr *msg);
void net_iface_test_dump_inject(struct blob_attr *msg);
void net_iface_test_event_inject(struct blob_attr *msg);

/*
 * Test control of the recovery timer, never used by the daemon. The first
 * reads the delay of the pending attempt, 0 when none is armed. The second
 * runs a pending attempt the way the uloop dispatch would, false when none is
 * armed.
 */
unsigned int net_iface_test_recover_ms(void);
bool net_iface_test_recover_fire(void);

#endif
