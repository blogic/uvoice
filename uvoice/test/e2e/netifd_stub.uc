#!/usr/bin/env ucode
// SPDX-License-Identifier: GPL-2.0-only
//
// uvoice - native voice stack for OpenWrt
// Copyright (C) 2026 Adtran Inc.
// Author: Chad Monroe <chad.monroe@adtran.com>
//
// Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
//
//
// netifd_stub.uc: the netifd objects of the wan-flap-outage flow.
//
// The harness runs a bare ubusd, so no interface status ever reaches the
// daemon and every line binds unscoped on the address the route probe gives
// (src/net/iface.c net_iface_source_host_probe). This script publishes the
// two objects netifd publishes, network.interface.<name> with a status method
// and network.interface with a dump method, and answers both from a state
// file the flow rewrites between the steps of its sequence.
//
// The answer carries the fields the tracker reads, in the shapes netifd
// sends them (src/net/iface.c netif_pol and netif_a_pol): "up" as a boolean,
// "l3_device" and "interface" as strings, "ipv4-address" and "ipv6-address"
// as arrays of tables with an "address" and a "mask". The dump answer is one
// "interface" array of those same tables (netifd_dump_cb).
//
// The state file also carries the refusal. With "answer" set to "error" a
// call ends on a ubus status and not on a reply, which is the failed invoke
// of the recovery timer (src/net/iface.c netifd_query). The unit suite has no
// ubus connection, so that path has no test of its own.
//
// Usage: ucode netifd_stub.uc <ubus socket> <state file>

'use strict';

import * as fs from 'fs';
import * as ubus from 'ubus';
import * as uloop from 'uloop';

const sock_path = ARGV[0];
const state_path = ARGV[1];

function trace(text) {
	// stderr, because the flow reads this log while the stub still runs
	warn(sprintf("netifd stub: %s\n", text));
}

// The state file, read at every call: the flow moves the address under a
// running daemon, and a copy taken at start-up would answer the old one.
function state_read() {
	let text = fs.readfile(state_path);

	if (text == null)
		return null;

	try {
		return json(text);
	} catch (e) {
		trace(sprintf("%s does not parse: %s", state_path, e));

		return null;
	}
}

// One interface status. An empty address leaves the address list empty,
// which is what netifd answers for an interface that is up without an
// address of that family.
function status_make(state) {
	let addrs = [];

	if (state.address)
		push(addrs, { address: state.address, mask: state.mask ?? 32 });

	return {
		up: state.up ?? true,
		pending: false,
		available: true,
		interface: state.interface,
		l3_device: state.l3_device ?? "",
		"ipv4-address": addrs,
		"ipv6-address": []
	};
}

// A handler that returns an integer ends the request on that ubus status
// (ucode lib/ubus.c uc_ubus_handle_reply_common), which is the refusal the
// daemon reads as a failed invoke.
function state_take(method) {
	let state = state_read();

	if (!state) {
		trace(sprintf("%s: no state", method));

		return null;
	}
	if (state.answer == "error") {
		trace(sprintf("%s: refused", method));

		return null;
	}

	return state;
}

function status_call(req) {
	let state = state_take("status");

	if (!state)
		return ubus.STATUS_UNKNOWN_ERROR;

	let body = status_make(state);
	trace(sprintf("status: %J", body));

	return body;
}

function dump_call(req) {
	let state = state_take("dump");

	if (!state)
		return ubus.STATUS_UNKNOWN_ERROR;

	trace("dump: one interface");

	return { interface: [ status_make(state) ] };
}

if (!sock_path || !state_path)
	die("usage: netifd_stub.uc <ubus socket> <state file>");

const boot = state_read();

if (!boot || !boot.interface)
	die(sprintf("netifd stub: %s names no interface", state_path));

uloop.init();

const conn = ubus.connect(sock_path);

if (!conn)
	die(sprintf("netifd stub: no connection on %s: %s", sock_path, ubus.error()));

if (!conn.publish(sprintf("network.interface.%s", boot.interface),
		  { status: { call: status_call } }))
	die(sprintf("netifd stub: network.interface.%s refused: %s",
		    boot.interface, conn.error()));

if (!conn.publish("network.interface", { dump: { call: dump_call } }))
	die(sprintf("netifd stub: network.interface refused: %s", conn.error()));

trace(sprintf("network.interface and network.interface.%s published on %s",
	      boot.interface, sock_path));

uloop.run();
