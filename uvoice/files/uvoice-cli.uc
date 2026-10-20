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
// uvoice-cli.uc: the table formatter behind /usr/srg/scripts/uvoice.sh.
//
// The contract of every reply this script reads is the API schema,
// schema/uvoice-api-schema.json. Lines and slots are 0-based on input,
// matching the ubus surface. The klish-facing voice-cli is 1-based and sets
// UVOICE_LINE_BASE=1, so the views it renders print line numbers its way.

'use strict';

import * as ubus from 'ubus';
import * as fs from 'fs';

let ubus_socket = getenv("UVOICE_UBUS_SOCKET");
if (ubus_socket == "")
	ubus_socket = null;

const UBUS_TIMEOUT = 5;
// The client outlives the daemon's 5 s stats_live backstop and its line test
// budget, so the daemon that knows why reports a query that never completes
// (the same margins voice-cli keeps).
const UBUS_LIVE_TIMEOUT = 10;
const UBUS_LINE_TEST_TIMEOUT = 110;

const LINE_TESTS = ["gr909", "roh", "capacitance", "loop"];
const TRACE_DOMAINS = ["sip", "fsm", "hook", "hal", "dialplan", "media", "jb",
                       "mgcp"];
const LINETEST_META_KEYS = ["version", "test_id", "code", "valid"];
const DIAL_RE = /^[0-9*#]+$/;
// The two numeric loop test options. A sign is not part of it: the surface
// takes a window and a delay, and the daemon bounds both.
const LOOPTEST_UINT_RE = /^[0-9]+$/;

// The color layer. uvoice.sh raises UVOICE_COLOR=1 when stdout is a tty,
// so a pipe, a capture and the test suite read plain bytes.
const COLOR_ON = getenv("UVOICE_COLOR") == "1" && getenv("NO_COLOR") == null;

// The display base of every line number. The base moves the print alone:
// every --line argument and every ubus parameter stays the 0-based index.
const LINE_BASE = getenv("UVOICE_LINE_BASE") == "1" ? 1 : 0;

// The command names the footers cite, so a klish user is pointed at a verb
// the klish tree has and not at a shell helper.
const HINT_STYLE = getenv("UVOICE_HINT_STYLE") == "klish" ? "klish" : "sh";
const HINTS = {
	sh: { cdr: "uvoice.cdr", line: "uvoice.line N",
	      channel: "uvoice.config.channel N" },
	klish: { cdr: "show voip call-records", line: "show voip line N",
	         channel: "show voip line N config" }
};

function sgr(text, code) {
	if (!COLOR_ON || text == null || text === "")
		return text;
	return "\x1b[" + code + "m" + text + "\x1b[0m";
}

function c_bold(text) { return sgr(text, "1"); }
function c_dim(text) { return sgr(text, "2"); }
function c_red(text) { return sgr(text, "31"); }
function c_green(text) { return sgr(text, "32"); }
function c_yellow(text) { return sgr(text, "33"); }

// printf widths count the ANSI bytes, so a colored cell pads plain text
// first and wraps after.
function pad_left(text, width) {
	let s = sprintf("%s", text ?? "");
	while (length(s) < width)
		s = " " + s;
	return s;
}

function pad_right(text, width) {
	let s = sprintf("%s", text ?? "");
	while (length(s) < width)
		s += " ";
	return s;
}

function cell_left(text, width, hue) {
	let s = pad_right(text, width);
	return hue ? hue(s) : s;
}

function cell_right(text, width, hue) {
	let s = pad_left(text, width);
	return hue ? hue(s) : s;
}

// A grid counter cell: zero and absent read dim, non-zero reads plain or,
// where the counter names an error, the warn hue.
function grid_num(v, width, warn_hue) {
	if (v == null)
		return cell_right("-", width, c_dim);
	if (v == 0)
		return cell_right(v, width, c_dim);
	return cell_right(v, width, warn_hue);
}

function underline(text, ch) {
	let bar = "";
	for (let i = 0; i < length(text); i++)
		bar += ch;
	printf("%s\n%s\n", c_bold(text), bar);
}

// A grid header names columns, so it dims where a section title bolds.
function print_header(header) {
	let bar = "";
	for (let i = 0; i < length(header); i++)
		bar += "-";
	printf("%s\n%s\n", c_dim(header), bar);
}

function yn(v) {
	return v ? "yes" : "no";
}

function onoff(v) {
	return v ? "on" : "off";
}

function u32fix(v) {
	// A blobmsg u32 at or past 2^31 arrives sign-extended (schema $defs/U32)
	if (v != null && type(v) == "int" && v < 0)
		v += 0x100000000;
	return v;
}

function line_text(n) {
	if (n == null)
		return "-";
	return sprintf("%d", u32fix(n) + LINE_BASE);
}

function format_bytes(bytes) {
	if (bytes == null)
		return "0 B";
	if (bytes < 1024)
		return sprintf("%d B", bytes);
	if (bytes < 1024 * 1024)
		return sprintf("%.2f KB", bytes / 1024.0);
	if (bytes < 1024 * 1024 * 1024)
		return sprintf("%.2f MB", bytes / (1024.0 * 1024.0));
	return sprintf("%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
}

function format_duration(seconds) {
	if (seconds == null || seconds == 0)
		return "0s";
	if (seconds < 60)
		return sprintf("%ds", seconds);
	if (seconds < 3600)
		return sprintf("%dm %ds", seconds / 60, seconds % 60);
	if (seconds < 86400)
		return sprintf("%dh %dm", seconds / 3600, (seconds % 3600) / 60);
	return sprintf("%dd %dh", seconds / 86400, (seconds % 86400) / 3600);
}

function format_duration_ms(ms) {
	if (ms == null)
		return "-";
	if (ms < 60000)
		return sprintf("%d.%03ds", ms / 1000, ms % 1000);
	return format_duration(ms / 1000);
}

function format_timestamp(ts) {
	if (!ts)
		return "-";
	let d = localtime(ts);
	if (!d)
		return "-";
	return sprintf("%04d-%02d-%02d %02d:%02d:%02d",
	               d.year, d.mon, d.mday, d.hour, d.min, d.sec);
}

function format_erle(mdb) {
	if (mdb == null)
		return "-";
	return sprintf("%.1f dB", mdb / 1000.0);
}

function pct_text(part, total) {
	if (!total)
		return "-";
	return sprintf("%d%%", (part * 200 + total) / (2 * total));
}

function samples_ms_text(samples, rate_khz) {
	if (samples % rate_khz == 0)
		return sprintf("%d", samples / rate_khz);
	return sprintf("%.1f", samples * 1.0 / rate_khz);
}

// The matrix printer behind the counter blocks. A group carries one header
// row of names and one or more value rows under it; all groups of one call
// share their column widths, so a header name never misaligns against the
// row above it. A cell is { text, hue }.
const MATRIX_LABEL_W = 22;
const MATRIX_CELL_MIN = 8;

function matrix_print(ind, groups) {
	let widths = [];

	for (let g in groups) {
		for (let i = 0; i < length(g.header); i++)
			widths[i] = max(widths[i] ?? MATRIX_CELL_MIN, length(g.header[i]));
		for (let row in g.rows)
			for (let i = 0; i < length(row.cells); i++)
				widths[i] = max(widths[i] ?? MATRIX_CELL_MIN,
				                length(row.cells[i].text));
	}

	for (let g in groups) {
		let head = pad_right("", MATRIX_LABEL_W);
		for (let i = 0; i < length(g.header); i++)
			head += " " + pad_left(g.header[i], widths[i]);
		printf("%s%s\n", ind, c_dim(head));

		for (let row in g.rows) {
			let line = ind + c_dim(pad_right(row.label, MATRIX_LABEL_W));
			for (let i = 0; i < length(row.cells); i++)
				line += " " + cell_right(row.cells[i].text, widths[i],
				                         row.cells[i].hue);
			print(rtrim(line, " ") + "\n");
		}
	}
}

function cell(text, hue) {
	return { text: sprintf("%s", text ?? "-"), hue: text == null ? c_dim : hue };
}

// A zero counter reads dim; a non-zero one reads plain, or in the warn hue
// where the counter names an error.
function count_cell(v, warn_hue) {
	if (v == null || v == 0)
		return cell(v, c_dim);
	return cell(v, warn_hue);
}

function count_pct_cell(v, total, warn_hue) {
	if (v == null || v == 0)
		return cell(v, c_dim);
	return cell(sprintf("%d (%s)", v, pct_text(v, total)), warn_hue);
}

// The reading words of the jb, lec and monitor rows.
function sample_hue(text, sample) {
	if (sample == "never")
		return c_dim(text);
	if (sample == "stale")
		return c_yellow(text);
	if (sample == "live" || sample == "sampled")
		return c_green(text);
	return text;
}

function yn_bad(v) {
	return v ? c_red("yes") : "no";
}

function reg_word_hue(word) {
	if (word == null || word == "-")
		return null;
	if (word == "registered")
		return c_green;
	if (word == "failed" || word == "rejected")
		return c_red;
	return c_yellow;
}

// The service word of an MGCP endpoint (proto/mgcp/proto.c).
function mgcp_service_hue(word) {
	if (word == "in_service")
		return c_green;
	if (word == "out_of_service" || word == "disconnected")
		return c_red;
	return c_yellow;
}

function sip_code_hue(code) {
	if (code == null)
		return null;
	if (code >= 200 && code < 300)
		return c_green;
	if (code >= 400)
		return c_red;
	return c_yellow;
}

// The two resting FSM states read green, a fault red, and any transient
// state yellow, so a watch view separates them at a glance.
function fsm_state_hue(state, oos) {
	if (oos)
		return c_red;
	if (state == "IDLE" || state == "CONNECTED")
		return c_green;
	return c_yellow;
}

// The line monitor units. The daemon reports mV, uA and mW (schema
// $defs/StatusMonitorRow), so a voltage and a current both scale by one
// thousand. A loop current lives in the milliampere range, so a current
// reads in mA always, and one decimal is enough for both electrical
// readings. A value carries its unit beside the number, in the table and in
// the line view alike; only a matrix cell under a unit label prints bare.
function milli_text(v) {
	if (v == null)
		return "-";
	return sprintf("%.1f", v / 1000.0);
}

function format_mv(v) {
	if (v == null)
		return "-";
	return milli_text(v) + " V";
}

function format_ma(v) {
	if (v == null)
		return "-";
	return milli_text(v) + " mA";
}

function format_mw(v) {
	if (v == null)
		return "-";
	return sprintf("%d mW", v);
}

// The fault set of a line, by bit (udsp/src/include/udsp/event.h).
const FAULT_BIT_NAMES = {
	[1 << 0]: "stall",
	[1 << 1]: "thermal",
	[1 << 2]: "dc",
	[1 << 3]: "pq",
	[1 << 4]: "vbat",
	[1 << 5]: "longstat",
	[1 << 6]: "reinit",
	[1 << 7]: "dcdc",
	[1 << 9]: "clock",
	[1 << 10]: "device_reset",
	[1 << 16]: "stream_dead"
};

// A bit the table does not name prints as raw hex beside the names. A word
// from a newer library then reads whole, instead of losing the part this
// table cannot spell.
function fault_bits_text(mask) {
	let bits = u32fix(mask);
	if (bits == null)
		return "-";
	if (bits == 0)
		return "none";

	let names = [];
	let rest = bits;

	for (let bit, name in FAULT_BIT_NAMES) {
		if (!(bits & int(bit)))
			continue;
		push(names, name);
		rest -= int(bit);
	}

	if (rest)
		push(names, sprintf("0x%x", rest));
	return join(", ", names);
}

// The daemon truncates its CLOCK_MONOTONIC ms to 32 bits (uvoice.h
// uvoice_mono_ms). The CLI subtracts in the same width, so the age wraps with
// the stamp every 49.7 days.
const AGE_MASK = 0xffffffff;

function age_text(stamp) {
	let now = clock(true);
	let now_ms = now[0] * 1000 + now[1] / 1000000;
	let age = (now_ms - stamp) & AGE_MASK;

	if (age < 60000)
		return sprintf("%.1f s ago", age * 1.0 / 1000);
	if (age < 3600000)
		return sprintf("%d min %d s ago", age / 60000, (age % 60000) / 1000);
	return sprintf("%d h %d min ago", age / 3600000, (age % 3600000) / 60000);
}

// Only a last_call row names an instant. A live row answers from the frame
// the daemon read, and a never row holds no reading.
function row_sample_text(row) {
	if (row.sample == "last_call")
		return sprintf("last_call, %s", age_text(u32fix(row.stamp_ms)));
	return row.sample ?? "-";
}

function row_age_text(row) {
	if (row.sample != "last_call")
		return "-";
	return age_text(u32fix(row.stamp_ms));
}

// A monitor row names its instant on every reading it has, so both reading
// words take an age. The two helpers above carry the jb and lec rule instead.
function monitor_age_text(row) {
	if (row.sample != "sampled" && row.sample != "stale")
		return "-";
	return age_text(u32fix(row.stamp_ms));
}

function pair_print(ind, key, value) {
	printf("%s%s %s\n", ind, c_dim(pad_right(key, 22)), value);
}

function host_port_join(host, port) {
	if (index(host, ":") >= 0)
		host = "[" + host + "]";
	return sprintf("%s:%d", host, u32fix(port));
}

// The uci vocabulary of the number-valued enums. A word-valued enum arrives
// as its word and needs no entry here.
const CONFIG_LEGEND = {
	digit_esc_enabled: {
		words: { [0]: "raw", [1]: "star and pound", [2]: "pound" }
	},
	local_conf_enabled: {
		words: { [0]: "off", [1]: "conference",
		         [2]: "conference with transfer" }
	},
	restrict_source: {
		words: { [0]: "open", [1]: "proxy addresses",
		         [2]: "keepalive pinhole" }
	},
	call_fwd_local_mode: {
		bits: { [1]: "unconditional", [2]: "busy", [4]: "no answer" }
	}
};

// The options dump_config reports as a value beside a <name>_set flag.
const CONFIG_SET_PAIRS = ["sip_port", "call_waiting_local",
	"cid_delivery_local", "lec_control", "lec_nlp"];

// A value carrying a bit the table does not name gets no legend at all: the
// mask would read as the bits it does name and hide the rest.
function legend_bits_text(bits, value) {
	let known = 0;
	let names = [];

	for (let bit, name in bits) {
		known |= int(bit);
		if (value & int(bit))
			push(names, name);
	}

	if (value & ~known)
		return null;
	return length(names) ? join(", ", names) : "none";
}

function legend_text(entry, value) {
	if (!entry)
		return null;
	if (entry.bits)
		return legend_bits_text(entry.bits, value);
	return entry.words[value];
}

function config_pair_base(key) {
	let len = length(key);
	if (len <= 4 || substr(key, len - 4) != "_set")
		return null;

	let base = substr(key, 0, len - 4);
	return index(CONFIG_SET_PAIRS, base) >= 0 ? base : null;
}

function kv_text(value) {
	let t = type(value);
	if (t == "bool")
		return yn(value);
	if (t == "string" && value == "")
		return "(empty)";
	return sprintf("%s", value);
}

function kv_line(indent, key, text) {
	printf("%s%s %s\n", indent, c_dim(pad_right(key, 36)), text);
}

function kv_print(tbl, indent, skip, legend) {
	for (let key, value in tbl) {
		if (skip && index(skip, key) >= 0)
			continue;
		if (index(CONFIG_SET_PAIRS, key) >= 0 &&
		    exists(tbl, key + "_set"))
			continue;

		let t = type(value);
		if (t == "object" || t == "array")
			continue;

		let base = config_pair_base(key);
		if (base != null) {
			kv_line(indent, base, value ? kv_text(tbl[base]) : "(unset)");
			continue;
		}

		let meaning = legend_text(legend ? legend[key] : null, value);
		if (meaning == null)
			kv_line(indent, key, kv_text(value));
		else
			kv_line(indent, key, sprintf("%s (%s)", kv_text(value), meaning));
	}
}

// The ubus binding clears its error state on the first error() read, so the
// message is read once and the status code is derived from the leading
// ubus_strerror() word ("Not found: Failed to invoke ...").
const UBUS_ERROR_WORDS = {
	"Invalid command": 1,
	"Invalid argument": 2,
	"Method not found": 3,
	"Not found": 4,
	"No response": 5,
	"Permission denied": 6,
	"Request timed out": 7,
	"Operation not supported": 8,
	"Unknown error": 9,
	"Connection failed": 10
};

function ubus_error_code(err) {
	for (let word, code in UBUS_ERROR_WORDS)
		if (index(err, word) == 0)
			return code;
	return null;
}

function voice_call(object, method, params, timeout) {
	let conn = ubus.connect(ubus_socket, timeout ?? UBUS_TIMEOUT);
	if (!conn)
		return { err: "cannot connect to ubus" };

	let res = { data: conn.call({ object: object, method: method, data: params ?? {} }) };
	if (res.data == null) {
		res.err = conn.error();
		if (res.err == null)
			res.data = {};
		else
			res.code = ubus_error_code(res.err);
	}
	conn.disconnect();
	return res;
}

function voice_present() {
	let conn = ubus.connect(ubus_socket, 2);
	if (!conn)
		return false;

	let methods = conn.list("voice");
	conn.disconnect();
	return methods != null;
}

function call_fail(what, res) {
	printf("Error: %s failed: %s\n", what, res.err ?? "unknown error");
	return 1;
}

// The refusal codes of the two deferred verbs, from the src/ubus.c handlers;
// the schema carries only the success bodies.
function deferred_fail(what, res, line) {
	let why;
	if (res.code == 2)
		why = "invalid argument (unknown line, slot or test)";
	else if (res.code == 4 && !voice_present())
		why = "uvoiced does not answer ubus";
	else if (res.code == 4)
		why = sprintf("line %d is not configured, the lines are not up, or the slot has no running stream", line);
	else if (res.code == 5)
		why = sprintf("line %d is down, busy with a call or a test, or the backend has no support", line);
	else if (res.code == 7)
		why = sprintf("line %d: the request timed out in the daemon", line);
	else if (res.code == 9)
		why = "the backend refused, or the result is unreadable";
	else
		why = res.err ?? "unknown error";

	printf("Error: %s failed: %s\n", what, why);
	return 1;
}

function status_get() {
	let res = voice_call("voice", "status", {}, UBUS_TIMEOUT);
	if (res.err != null) {
		call_fail("voice status", res);
		return null;
	}
	return res.data;
}

function line_arg_get(args, what) {
	if (args.line == null || args.line === true) {
		printf("Error: --line N required for %s\n", what);
		return null;
	}
	return int(args.line);
}

// loss and loss_far are the RFC 3550 A.3 loss fractions in 1/256 units
// (udsp stats.c). Every view prints them raw.
function media_block_print(m, ind) {
	if (!m)
		return;

	matrix_print(ind, [
		{
			header: [ "sent", "recv", "lost", "disc", "loss", "loss_far" ],
			rows: [
				{ label: "packets", cells: [
					count_cell(m.packets_sent, null),
					count_cell(m.packets_recv, null),
					count_cell(m.packets_lost, c_yellow),
					count_cell(m.packets_discarded, c_yellow),
					count_cell(m.loss_rate, c_yellow),
					count_cell(m.loss_rate_far, c_yellow)
				] },
				{ label: "bytes", cells: [
					cell(format_bytes(m.bytes_sent), null),
					cell(format_bytes(m.bytes_recv), null)
				] }
			]
		},
		{
			header: [ "avg", "max", "far" ],
			rows: [
				{ label: "jitter (ms)", cells: [
					count_cell(m.jitter_avg, null),
					count_cell(m.jitter_max, null),
					count_cell(m.jitter_far, null)
				] },
				{ label: "rtt (ms)", cells: [
					count_cell(m.rtt_avg, null),
					count_cell(m.rtt_max, null)
				] }
			]
		},
		{
			header: [ "mos_lq", "mos_cq", "rfactor" ],
			rows: [ { label: "score", cells: [
				cell(m.mos_lq, null),
				cell(m.mos_cq, null),
				cell(u32fix(m.rfactor), null)
			] } ]
		}
	]);
}

// The five output counters partition the output, so a share is of their sum
// and not of frames_out.
function jb_output_total(jb) {
	return jb.frames_out + jb.plc + jb.cn + jb.silence + jb.skew_inserts;
}

// A status jb row and a call record's jb table share these counters
// (schema JbCounters), so one block serves both views. The four groups run
// through one matrix_print call and share their column widths.
function jb_block_print(jb, ind) {
	if (!jb)
		return;

	let total = jb_output_total(jb);

	matrix_print(ind, [
		{
			header: [ "target", "depth", "jitter", "pool" ],
			rows: [ { label: "buffer (ms)", cells: [
				count_cell(u32fix(jb.target_ms), null),
				count_cell(u32fix(jb.depth_ms), null),
				count_cell(u32fix(jb.jitter_ms), null),
				count_cell(u32fix(jb.pool_free), null)
			] } ]
		},
		{
			header: [ "total", "frames", "plc", "cn", "silence", "skew_ins" ],
			rows: [ { label: "output", cells: [
				count_cell(total, null),
				count_cell(jb.frames_out, null),
				count_pct_cell(jb.plc, total, c_yellow),
				count_cell(jb.cn, null),
				count_pct_cell(jb.silence, total, null),
				count_cell(jb.skew_inserts, null)
			] } ]
		},
		{
			header: [ "late", "dup", "ovfl", "hold", "skew" ],
			rows: [ { label: "drops", cells: [
				count_cell(jb.late_drop, c_yellow),
				count_cell(jb.dup_drop, c_yellow),
				count_cell(jb.overflow_drop, c_yellow),
				count_cell(jb.hold_drop, c_yellow),
				count_cell(jb.skew_drops, c_yellow)
			] } ]
		},
		{
			header: [ "resync", "starve", "cn_re", "disc_ms", "underrun" ],
			rows: [ { label: "recovery", cells: [
				count_cell(jb.resyncs, c_yellow),
				count_cell(jb.starve_resyncs, c_yellow),
				count_cell(jb.cn_reanchors, c_yellow),
				count_cell(u32fix(jb.resync_discard_ms), c_yellow),
				count_cell(jb.underruns, c_yellow)
			] } ]
		}
	]);
}

const LINES_HEADER = sprintf("%-4s %-5s %-11s %-12s %7s %7s %5s %-22s %-7s %s",
	"Line", "Trans", "State", "Status", "Expires", "Refresh", "Code",
	"Registrar", "MWI", "AOR");

function account_row_print(a) {
	let registrar = "-";
	if (a.registrar != null)
		registrar = host_port_join(a.registrar, a.registrar_port);

	print(join(" ", [
		cell_left(line_text(a.line), 4, null),
		cell_left(a.transport ?? "-", 5, null),
		cell_left(a.state ?? "-", 11, reg_word_hue(a.state)),
		cell_left(a.status ?? "-", 12, reg_word_hue(a.status)),
		cell_right(a.expires != null ? u32fix(a.expires) : "-", 7, null),
		cell_right(a.refresh_in != null ? u32fix(a.refresh_in) : "-", 7, null),
		cell_right(a.last_code ?? "-", 5, sip_code_hue(a.last_code)),
		cell_left(registrar, 22, null),
		cell_left(a.mwi_subscription ?? "-", 7, null),
		a.aor ?? "-"
	]) + "\n");

	if (a.listener == null && !a.connection && !a.tls_error)
		return;

	let extras = [];
	if (a.listener != null)
		push(extras, sprintf("listener %s", yn(a.listener)));
	if (a.connection)
		push(extras, sprintf("conn %s%s, %d queued",
		     a.connection.state,
		     a.connection.peer ? " to " + a.connection.peer : "",
		     a.connection.queued));
	if (a.tls_error)
		push(extras, c_red("tls_error " + a.tls_error));
	printf("     %s\n", join("  ", extras));
}

const ENDPOINTS_HEADER = sprintf("%-4s %-14s %-14s %-8s %-24s %s",
	"Line", "Service", "State", "ReqId", "Notified entity", "Endpoint");

// One connection of an endpoint, under its row.
function mgcp_conn_print(c, ind) {
	printf("%sconn %s call %s %s stream %d%s\n", ind, c.id, c.call_id,
	       c.mode, c.stream, c.remote ? " remote " + c.remote : "");
}

function endpoint_row_print(e) {
	print(join(" ", [
		cell_left(line_text(e.line), 4, null),
		cell_left(e.service ?? "-", 14, mgcp_service_hue(e.service)),
		cell_left(e.state ?? "-", 14, null),
		cell_left(e.request_id ?? "-", 8, null),
		cell_left(e.notified_entity || "-", 24, null),
		e.endpoint ?? "-"
	]) + "\n");

	for (let c in (e.connections ?? []))
		mgcp_conn_print(c, "     ");
}

function mgcp_lines_print(lines) {
	if (!lines || length(lines) == 0) {
		print("No endpoints: the MGCP unit waits for a domain or an address.\n");
		return 0;
	}

	print_header(ENDPOINTS_HEADER);
	for (let e in lines)
		endpoint_row_print(e);

	printf("\nTotal: %d endpoint(s)\n", length(lines));
	return 0;
}

// The mgcp table of voice status (proto/mgcp/proto.c
// mgcp_status_dump_top()).
function mgcp_status_print(m) {
	let t = m.transactions ?? {};

	print("\n");
	underline("MGCP", "-");
	pair_print("  ", "dialect", m.dialect ?? "-");
	pair_print("  ", "call_agent", m.call_agent || "(none)");
	pair_print("  ", "notified_entity", m.notified_entity || "(none)");
	pair_print("  ", "local_port", u32fix(m.local_port));
	pair_print("  ", "socket",
	           m.socket == "bound" ? c_green(m.socket) :
	           c_yellow(m.socket ?? "-"));
	pair_print("  ", "disconnected",
	           m.disconnected ? c_red(sprintf("yes, %d attempt(s)",
	                                          u32fix(m.disc_attempts))) :
	           "no");
	matrix_print("  ", [
		{
			header: [ "open", "created", "retx", "timeout",
			          "transport" ],
			rows: [ { label: "txn", cells: [
				count_cell(t.open, null),
				count_cell(t.created, null),
				count_cell(t.retransmitted, c_yellow),
				count_cell(t.timeout, c_yellow),
				count_cell(t.transport, c_yellow)
			] } ]
		},
		{
			header: [ "commands", "errors", "ntfy", "rsip",
			          "quar_drop" ],
			rows: [ { label: "gateway", cells: [
				count_cell(m.commands, null),
				count_cell(m.errors, c_yellow),
				count_cell(m.ntfy, null),
				count_cell(m.rsip, null),
				count_cell(m.quar_dropped, c_yellow)
			] } ]
		}
	]);
}

const CALLS_HEADER = sprintf("%-4s %-13s %-4s %-4s %-4s %-4s %-8s %-12s %s",
	"Line", "State", "OOS", "Ring", "CW", "DND", "MWI", "Digits", "Flags");

function leg_print(leg, ind) {
	let id = leg.number ?? leg.remote ?? "-";
	if (leg.name)
		id += sprintf(" (%s)", leg.name);
	if (leg.cid_private)
		id += " [private]";

	let media = "-";
	if (leg.codec) {
		media = sprintf("%s/%sms %s", leg.codec, leg.ptime, leg.direction);
		if (leg.srtp)
			media += "  srtp " + leg.srtp;
	}
	if (leg.fax)
		media += "  fax " + leg.fax;
	if (leg.fax_kind)
		media += "/" + leg.fax_kind;

	let extras = [];
	if (leg.shadow)
		push(extras, "shadow");
	if (leg.ui_busy)
		push(extras, "ui-busy");
	if (leg.rtp_port != null)
		push(extras, sprintf("rtp %d", u32fix(leg.rtp_port)));
	if (leg.session_expires != null)
		push(extras, sprintf("timer %ss%s", u32fix(leg.session_expires),
		     leg.we_refresh ? " (we refresh)" : ""));

	printf("%s    slot %d (ui %d)  %-16s %-4s %-28s %s%s\n",
	       ind, leg.slot, leg.ui_slot, leg.state ?? "-",
	       leg.inbound ? "in" : "out", id, media,
	       length(extras) ? "  [" + join(", ", extras) + "]" : "");
}

function call_entry_print(c, ind) {
	let flags = [];
	if (c.fault)
		push(flags, c_red("slic-fault"));
	if (c.ring_held)
		push(flags, "ring-held");
	if (c.clearing)
		push(flags, "clearing " + c.clearing);
	if (c.conf)
		push(flags, "conf");
	if (c.fwd_disc_reversed)
		push(flags, "fwd-disc-reversed");
	if (c.fwd_disc_armed)
		push(flags, "fwd-disc-armed");
	if (c.fwd_disc_down)
		push(flags, "fwd-disc-down");
	if (c.cf_uncond != null)
		push(flags, "cf-uncond " + (c.cf_uncond != "" ? c.cf_uncond : "(no target)"));
	if (c.cf_busy != null)
		push(flags, "cf-busy " + (c.cf_busy != "" ? c.cf_busy : "(no target)"));
	if (c.cf_no_answer != null)
		push(flags, "cf-no-answer " + (c.cf_no_answer != "" ? c.cf_no_answer : "(no target)"));
	if (c.cid_once != null)
		push(flags, sprintf("cid-once %s", c.cid_once ? "deliver" : "withhold"));
	if (c.cid_restrict != null)
		push(flags, sprintf("cid-restrict %s", c.cid_restrict ? "withheld" : "delivered"));
	if (c.fax_switched)
		push(flags, sprintf("fax-switched %s/%s",
		     yn(c.fax_switched[0]), yn(c.fax_switched[1])));
	if (c.t38_switched)
		push(flags, sprintf("t38-switched %s/%s",
		     yn(c.t38_switched[0]), yn(c.t38_switched[1])));
	if (c.deferred)
		push(flags, sprintf("deferred %d", u32fix(c.deferred)));
	if (c.deferred_lost)
		push(flags, sprintf("deferred-lost %d", u32fix(c.deferred_lost)));
	if (c.refused)
		push(flags, sprintf("refused %d (last %d, %d s)",
		     u32fix(c.refused.busy) + u32fix(c.refused.unavailable) +
		     u32fix(c.refused.other), u32fix(c.refused.last_code),
		     u32fix(c.refused.last_age_ms) / 1000));

	let mwi = yn(c.mwi);
	if (c.mwi_new || c.mwi_old)
		mwi += sprintf(" %d/%d", u32fix(c.mwi_new), u32fix(c.mwi_old));

	print(ind + join(" ", [
		cell_left(line_text(c.line), 4, null),
		cell_left(c.state ?? "-", 13, fsm_state_hue(c.state, c.oos)),
		cell_left(yn(c.oos), 4, c.oos ? c_red : null),
		cell_left(yn(c.ringing), 4, c.ringing ? c_yellow : null),
		cell_left(yn(c.call_waiting), 4, null),
		cell_left(yn(c.dnd), 4, c.dnd ? c_yellow : null),
		cell_left(mwi, 8, null),
		cell_left(c.digits ?? "-", 12, null),
		join(", ", flags)
	]) + "\n");

	for (let leg in (c.calls ?? []))
		leg_print(leg, ind);
}

const CDR_TABLE_HEADER = sprintf("%-5s %-4s %-4s %-4s %-22s %-19s %10s %-9s %-8s %-5s %s",
	"Seq", "Line", "Dir", "Ans", "Number", "Start", "Duration",
	"Codec", "Media", "MOS", "Flags");

function cdr_row_print(rec, ind) {
	let id = rec.number ?? rec.remote ?? "-";
	let codec = rec.codec ? sprintf("%s/%s", rec.codec, rec.ptime) : "-";
	let flags = [];
	if (rec.fail)
		push(flags, c_red("fail " + rec.fail));
	if (rec.transferred)
		push(flags, rec.transferred_from != null
		     ? sprintf("xfer from %d", rec.transferred_from) : "xfer");
	if (rec.conferenced)
		push(flags, "conf");
	if (rec.in_progress)
		push(flags, c_green("live"));

	printf("%s%-5s %-4s %-4s %-4s %-22s %-19s %10s %-9s %-8s %-5s %s\n",
	       ind, rec.seq, line_text(rec.line), rec.inbound ? "in" : "out",
	       yn(rec.answered),
	       substr(id, 0, 22), format_timestamp(rec.start),
	       format_duration_ms(u32fix(rec.duration_ms)), codec,
	       rec.media_state ?? "-", rec.media ? rec.media.mos_lq : "-",
	       join(", ", flags));
}

const STATS_FMT = "%-4s %8s %8s %8s %8s %8s %8s %10s %10s %8s %10s %10s";
const STATS_HEADER = sprintf(STATS_FMT,
	"Line", "InRecv", "InAns", "InFail", "OutAtt", "OutAns", "OutFail",
	"PktSent", "PktRecv", "PktLost", "BytesSent", "BytesRecv");

function stats_row_print(r, ind) {
	let t = r.totals ?? {};
	print(ind + join(" ", [
		cell_left(line_text(r.line), 4, null),
		grid_num(u32fix(r.in_received), 8, null),
		grid_num(u32fix(r.in_answered), 8, null),
		grid_num(u32fix(r.in_failed), 8, c_yellow),
		grid_num(u32fix(r.out_attempted), 8, null),
		grid_num(u32fix(r.out_answered), 8, null),
		grid_num(u32fix(r.out_failed), 8, c_yellow),
		grid_num(t.packets_sent, 10, null),
		grid_num(t.packets_recv, 10, null),
		grid_num(t.packets_lost, 8, c_yellow),
		cell_right(format_bytes(t.bytes_sent), 10, null),
		cell_right(format_bytes(t.bytes_recv), 10, null)
	]) + "\n");
}

const KEEPALIVE_FMT = "%8s %8s %8s %8s %8s %8s %8s %8s %8s";
const KEEPALIVE_HEADER = sprintf(KEEPALIVE_FMT,
	"CRLFSent", "CRLFSkip", "CRLFFail", "OptSent", "OptAlive", "OptFail",
	"OptSkip", "Interval", "CTBound");

// interval is the effective CRLF round and ct_bound the conntrack udp bound
// the round runs under, both in seconds (schema KeepaliveTable).
function keepalive_row_print(ka, ind) {
	print(ind + join(" ", [
		grid_num(ka.crlf_sent, 8, null),
		grid_num(ka.crlf_skipped, 8, c_yellow),
		grid_num(ka.crlf_failed, 8, c_yellow),
		grid_num(ka.options_sent, 8, null),
		grid_num(ka.options_alive, 8, null),
		grid_num(ka.options_failed, 8, c_yellow),
		grid_num(ka.options_skipped, 8, c_yellow),
		cell_right(sprintf("%d s", u32fix(ka.interval)), 8, null),
		cell_right(sprintf("%d s", u32fix(ka.ct_bound)), 8, null)
	]) + "\n");
}

// A SIP line counts its call legs, and an MGCP endpoint row its connections.
function legs_count(data) {
	let legs = 0;

	if (data.mgcp) {
		for (let row in (data.lines ?? []))
			legs += length(row.connections ?? []);
		return legs;
	}

	for (let c in (data.call ?? []))
		for (let leg in (c.calls ?? []))
			if (!leg.shadow)
				legs++;

	return legs;
}

function cmd_status(args) {
	let data = status_get();
	if (!data)
		return 1;

	underline("uvoice Status", "=");
	print("\n");
	let cfg = data.config ?? {};
	pair_print("  ", "version", data.version ?? "-");
	pair_print("  ", "loaded", yn(cfg.loaded));
	pair_print("  ", "enabled", yn(cfg.enabled));
	pair_print("  ", "channels", cfg.channels ?? 0);
	pair_print("  ", "address", data.network?.address ?? "-");
	pair_print("  ", "address6", data.network?.address6 ?? "-");
	pair_print("  ", "l3_device", data.network?.l3_device ?? "-");

	let sig = data.signaling ?? {};
	print("\n");
	underline("Signaling", "-");
	pair_print("  ", "protocol", sig.protocol ?? "-");
	pair_print("  ", "state",
	           sig.state == "running" ? c_green(sig.state) :
	           c_red(sig.state ?? "-"));
	if (sig.error != null)
		pair_print("  ", "error", sig.error);
	pair_print("  ", "available", length(sig.available) ?
	           join(", ", sig.available) : "(none)");

	if (data.mgcp)
		mgcp_status_print(data.mgcp);

	let hal = data.hal ?? {};
	print("\n");
	underline("HAL", "-");
	pair_print("  ", "state", hal.state ?? "-");
	pair_print("  ", "backend", hal.backend ?? "-");
	if (hal.version != null) {
		pair_print("  ", "version", hal.version);
		pair_print("  ", "slic", hal.slic);
		pair_print("  ", "lines", u32fix(hal.lines));
		pair_print("  ", "streams_per_channel", u32fix(hal.streams_per_channel));
		pair_print("  ", "features",
		           length(hal.features) ? join(", ", hal.features) : "(none)");
		pair_print("  ", "codecs",
		           length(hal.codecs) ? join(", ", hal.codecs) : "(none)");
		matrix_print("  ", [
			{
				header: [ "op_err", "tdm_stall", "cmpl_lost", "holds",
				          "cap_drops" ],
				rows: [ { label: "health", cells: [
					count_cell(hal.op_errors, c_yellow),
					count_cell(hal.tdm_stalls, c_yellow),
					count_cell(hal.completions_lost, c_yellow),
					count_cell(hal.holds, null),
					count_cell(hal.capture_drops, c_yellow)
				] } ]
			},
			{
				header: [ "trim_drops", "pb_expd", "hook_gaps",
				          "moved_gap", "gapmax_ms" ],
				rows: [ { label: "", cells: [
					count_cell(hal.trim_drops, c_yellow),
					count_cell(hal.playback_expired, c_yellow),
					count_cell(hal.hook_gaps, null),
					count_cell(hal.hook_moved_gaps, null),
					count_cell(hal.hook_gap_max_ms, null)
				] } ]
			}
		]);
		if (hal.rt)
			pair_print("  ", "rt",
			           sprintf("%s prio %d cpu %d sched_errno %d mlock %s net %s",
			                   hal.rt.policy, hal.rt.prio, hal.rt.cpu,
			                   hal.rt.sched_errno, yn(hal.rt.mlock),
			                   hal.rt.net_policy));
	}

	let region = data.region;
	if (region) {
		print("\n");
		underline("Region", "-");
		pair_print("  ", "requested", region.requested || "(none)");
		pair_print("  ", "applied", region.applied || "(none)");
		pair_print("  ", "source", region.source ?? "-");
		pair_print("  ", "iso3", region.iso3 || "(none)");
		pair_print("  ", "describe", region.describe || "(none)");
		pair_print("  ", "cid_format", region.cid_format ?? "-");
		pair_print("  ", "mwi_format", region.mwi_format ?? "-");
		pair_print("  ", "hook_flash_min_ms", u32fix(region.hook_flash_min_ms));
		pair_print("  ", "hook_flash_max_ms", u32fix(region.hook_flash_max_ms));
		pair_print("  ", "hook_release_min_ms", u32fix(region.hook_release_min_ms));
		pair_print("  ", "cw_sas_total_ms", u32fix(region.cw_sas_total_ms));
		pair_print("  ", "flash_service", region.flash_service ?? "-");
		pair_print("  ", "pulse_invert",
		           region.pulse_invert == null ? "-" : yn(region.pulse_invert));
		pair_print("  ", "confirm_total_ms",
		           u32fix(region.confirm_total_ms));
		pair_print("  ", "confirm_endless",
		           region.confirm_endless == null ? "-" :
		           yn(region.confirm_endless));
		for (let t in region.ring) {
			let pairs = map(t.cadence,
			                (c) => sprintf("%d/%d", u32fix(c.on_ms),
			                               u32fix(c.off_ms)));
			pair_print("  ", sprintf("ring r%d", u32fix(t.template) + 1),
			           join(" ", pairs));
		}
	}

	if (data.events) {
		print("\n");
		underline("HAL Events", "-");
		pair_print("  ", "read", data.events.read);
		pair_print("  ", "sent", data.events.sent);
		pair_print("  ", "dropped", data.events.dropped);
		pair_print("  ", "lost", data.events.lost);
	}

	print("\n");
	underline("Media", "-");
	pair_print("  ", "opus_dtmf_fallbacks",
	           u32fix(data.media?.opus_dtmf_fallbacks) ?? 0);

	let registered = 0;
	for (let a in (data.lines ?? []))
		if (a.status == "registered" || a.service == "in_service")
			registered++;

	print("\n");
	underline("Summary", "-");
	if (data.mgcp)
		pair_print("  ", "endpoints",
		           sprintf("%d (%d in service)", length(data.lines ?? []),
		                   registered));
	else
		pair_print("  ", "accounts",
		           sprintf("%d (%d registered)", length(data.lines ?? []),
		                   registered));
	pair_print("  ", "call_legs", legs_count(data));
	pair_print("  ", "streams", length(data.streams ?? []));
	pair_print("  ", "jb_rows", length(data.jb ?? []));
	pair_print("  ", "lec_rows", length(data.lec ?? []));
	pair_print("  ", "monitor_rows", length(data.monitor ?? []));
	return 0;
}

function cmd_lines(args) {
	let data = status_get();
	if (!data)
		return 1;

	let sig = data.signaling ?? {};
	if (sig.state == "unavailable") {
		printf("Signaling protocol %s is unavailable: %s.\n",
		       sig.protocol ?? "-", sig.error ?? "no reason given");
		return 0;
	}

	let lines = data.lines;
	if (sig.protocol == "mgcp")
		return mgcp_lines_print(lines);
	if (!lines || length(lines) == 0) {
		print("No active accounts.\n");
		return 0;
	}

	print_header(LINES_HEADER);
	for (let a in lines)
		account_row_print(a);

	printf("\nTotal: %d account(s)\n", length(lines));
	return 0;
}

// A never row measured nothing, so it prints its word and no counter.
function jb_row_block_print(r) {
	if (r.sample == "never") {
		pair_print("  ", sprintf("stream %d", r.stream),
		           sample_hue(row_sample_text(r), r.sample));
		return;
	}
	pair_print("  ", sprintf("stream %d", r.stream),
	           sprintf("%s, %s", r.mode ?? "-",
	                   sample_hue(row_sample_text(r), r.sample)));
	jb_block_print(r, "    ");
}

function lec_sample_text(row) {
	if (row.sample == "never")
		return "never, this line has run no canceller";
	return row_sample_text(row);
}

// The last move of the answer-tone disabler. A release names no tone and no
// feed, so the line carries only the keys that arrived.
function lec_tone_text(policy) {
	let parts = [];
	if (policy.tone != null)
		push(parts, policy.tone);
	if (policy.tone_dir != null)
		push(parts, policy.tone_dir);
	if (policy.tone_action != null)
		push(parts, policy.tone_action);

	let text = join(" ", parts);
	if (policy.tone_stamp_ms != null)
		text += sprintf(", %s", age_text(u32fix(policy.tone_stamp_ms)));
	return text;
}

function lec_push_note(ap) {
	if (ap == null)
		return "no push yet";

	let note = sprintf("%s, %s, pending %s", ap.status ?? "-",
	                   age_text(u32fix(ap.stamp_ms)), yn(ap.pending));
	if (ap.pending)
		note += ": a seize would push a different view";
	return note;
}

// With lec_control off the daemon ignores lec_enable and lec_nlp, and an
// absent lec_nlp keeps the policy value under lec_control on. The cell
// carries the mark, the row's note the rule.
function lec_uci_cells(cfg) {
	let ignored = !cfg.control;
	let mark_ec = ignored ? "*" : "";
	let mark_nlp = (ignored || cfg.nlp == null) ? "*" : "";

	return [ yn(cfg.enable) + mark_ec,
	         (cfg.nlp != null ? yn(cfg.nlp) : "unset") + mark_nlp,
	         yn(cfg.tone_latch), yn(cfg.tone_force_disable) ];
}

function lec_uci_note(cfg) {
	if (!cfg.control)
		return "* ignored, lec_control is off";
	if (cfg.nlp == null)
		return "* unset, the policy value stands";
	return "";
}

const LEC_MATRIX_FMT = "  %-22s %-10s %-7s %-11s %-12s %s";

function lec_matrix_row(label, c1, c2, c3, c4, note) {
	print(rtrim(sprintf(LEC_MATRIX_FMT, label, c1, c2, c3, c4, note)) + "\n");
}

// The three layers as rows and the switches as columns, so what uci asked,
// what the daemon pushed and what the engine ran read as one column scan.
function lec_matrix_print(r) {
	let cfg = r.config ?? {};
	let cells = lec_uci_cells(cfg);
	let ap = r.applied;
	let st = r.state;

	lec_matrix_row("layer", "canceller", "nlp", "tone_latch",
	               "tone_force_disable", "");
	lec_matrix_row("uci", cells[0], cells[1], cells[2], cells[3],
	               lec_uci_note(cfg));
	if (ap == null)
		pair_print("  ", "pushed", lec_push_note(null));
	else
		lec_matrix_row("pushed", onoff(ap.ec), onoff(ap.nlp),
		               yn(ap.tone_latch), yn(ap.tone_force_disable),
		               lec_push_note(ap));
	// An open loop holds the filter with the counters of the call behind
	// it, so the note names why the filter reads off. The reason stays out
	// of the ec cell, because a word wider than the column shifts every
	// later cell of the row.
	if (st != null)
		lec_matrix_row("engine", st.ec ?? "-", st.nlp ?? "-", "-", "-",
		               sprintf("%sdouble_talk %s",
		                       st.loop_open ? "loop open; " : "",
		                       yn(st.double_talk)));
}

// Taps are the tail times the line rate in kHz, so the rate and the
// reference in ms follow from the row.
function lec_rate_khz(st) {
	let tail = u32fix(st.tail_ms);
	let taps = u32fix(st.taps);
	if (!tail || taps == null || taps % tail)
		return null;
	return taps / tail;
}

// The peak tap counts from the applied reference. A filter with no energy
// reads tap 0 too, so the energy tells "no peak" from a peak on the first tap.
function lec_peak_text(st, rate) {
	let peak = u32fix(st.coef_peak_tap);
	if (peak == null)
		return "";
	if (peak == 0 && !st.coef_energy_micro)
		return "; no peak tap";

	let text = sprintf("; peak tap %d samples", peak);
	if (rate != null)
		text += sprintf(" (%s ms)", samples_ms_text(peak, rate));
	return text;
}

function lec_filter_text(st) {
	let rate = lec_rate_khz(st);
	let delay = u32fix(st.ref_delay_samples);
	let text = sprintf("%d ms tail, %d taps", u32fix(st.tail_ms),
	                   u32fix(st.taps));

	if (rate != null)
		text += sprintf(" (%d kHz)", rate);
	text += sprintf("; reference %d samples", delay);
	if (rate != null)
		text += sprintf(" (%s ms)", samples_ms_text(delay, rate));
	text += st.ref_ready ? ", ready" : ", not ready";
	return text + lec_peak_text(st, rate);
}

// One frame is 10 ms at any line rate (udsp chan.c, rate / 100 samples).
function lec_state_matrix_print(st) {
	matrix_print("    ", [
		{
			header: [ "frames", "time", "held", "skip", "erle", "gain",
			          "coef_e" ],
			rows: [ { label: "run", cells: [
				count_cell(st.frames, null),
				cell(sprintf("%.1f s", st.frames / 100.0), null),
				count_pct_cell(st.hold_frames, st.frames, null),
				count_pct_cell(st.skip_frames, st.frames, c_yellow),
				cell(format_erle(st.erle_mdb), null),
				cell(sprintf("%.2f", st.echo_gain_micro / 1000000.0), null),
				cell(sprintf("%.3f", st.coef_energy_micro / 1000000.0), null)
			] } ]
		},
		{
			header: [ "div_resets", "retimes", "probes" ],
			rows: [ { label: "events", cells: [
				count_cell(st.diverge_resets, c_yellow),
				count_cell(st.retimes, null),
				count_cell(st.probes, null)
			] } ]
		},
		{
			header: [ "acted", "wait", "erle", "unlike", "dt", "quiet" ],
			rows: [ { label: "processor", cells: [
				count_pct_cell(st.nlp_active_frames, st.frames, null),
				count_pct_cell(st.nlp_wait_frames, st.frames, null),
				count_pct_cell(st.nlp_erle_frames, st.frames, null),
				count_pct_cell(st.nlp_unlike_frames, st.frames, null),
				count_pct_cell(st.nlp_dt_frames, st.frames, null),
				count_pct_cell(st.nlp_quiet_frames, st.frames, null)
			] } ]
		}
	]);
}

function call_t38_check(c) {
	for (let leg in (c?.calls ?? []))
		if (leg.fax == "t38")
			return true;
	return false;
}

function lec_row_block_print(r, t38) {
	let policy = r.policy ?? {};
	let tone = lec_tone_text(policy);

	pair_print("  ", "sample", sample_hue(lec_sample_text(r), r.sample));
	pair_print("  ", "policy", sprintf("%s, from %s",
	           policy.kind ?? "-", policy.source ?? "-"));
	if (tone != "")
		pair_print("  ", "tone", tone);

	print("\n");
	lec_matrix_print(r);
	if (r.state == null)
		return;

	print("\n");
	pair_print("  ", "filter", lec_filter_text(r.state));
	lec_state_matrix_print(r.state);
	if (t38 && policy.kind == "modem")
		print("  (a T.38 session runs the modem canceller policy)\n");
}

// The three fault fields stand in every row, a never row included. They are
// the backend engine's state of the line at the call, and they are defined
// before the first reading.
function monitor_faults_text(r) {
	let bits = fault_bits_text(r.fault_bits);
	return sprintf("%s; open %s, held %s",
	               bits == "none" ? c_dim(bits) : c_red(bits),
	               yn_bad(r.fault_open), yn_bad(r.fault_held));
}

// Each raw word prints beside the bits it decodes to (schema
// StatusMonitorRow): pd_dcdc carries dcdc_down, dcdc_status carries ovlo,
// lcrrtp carries voc_err, lcr and rtp, and linefeed the two states settled
// compares.
function milli_cell(v) {
	if (v == null)
		return cell(null, null);
	return cell(milli_text(v), null);
}

function monitor_values_print(r) {
	// vdc_mv stays off the table: the Si3228x has no VDC sense and the
	// plug-in writes a plain 0 there (udsp slic.h).
	matrix_print("  ", [ {
		header: [ "tip-ring", "tip", "ring", "long" ],
		rows: [
			{ label: "voltage (V)", cells: [
				milli_cell(r.vtr_mv), milli_cell(r.vtip_mv),
				milli_cell(r.vring_mv), milli_cell(r.vlong_mv)
			] },
			{ label: "current (mA)", cells: [
				milli_cell(r.itr_ua), milli_cell(r.itip_ua),
				milli_cell(r.iring_ua), milli_cell(r.ilong_ua)
			] }
		]
	} ]);
	pair_print("  ", "battery",
	           sprintf("%s, target %s, alarm %s; hvic %s", format_mv(r.vbat_mv),
	                   format_mv(r.vbat_tgt_mv), format_mv(r.vbat_irq_th_mv),
	                   format_mw(r.p_hvic_mw)));
	pair_print("  ", "converter",
	           sprintf("pd_dcdc 0x%06x, dcdc_status 0x%06x; down %s, ovlo %s",
	                   u32fix(r.pd_dcdc), u32fix(r.dcdc_status),
	                   yn_bad(r.dcdc_down), yn_bad(r.ovlo)));
	pair_print("  ", "loop",
	           sprintf("linefeed 0x%02x, lcrrtp 0x%02x; settled %s, voc_err %s, lcr %s, rtp %s",
	                   u32fix(r.linefeed), u32fix(r.lcrrtp), yn(r.settled),
	                   yn_bad(r.voc_err), yn(r.lcr), yn(r.rtp)));
}

function monitor_sample_text(r) {
	if (r.sample != "sampled" && r.sample != "stale")
		return r.sample ?? "-";
	return sprintf("%s, %s", r.sample, monitor_age_text(r));
}

// A sampled row reads best values first, so its sample line closes the
// block. A stale or never row leads with its explanation instead.
function monitor_row_block_print(r) {
	let sample = sample_hue(monitor_sample_text(r), r.sample);

	if (r.sample != "sampled") {
		pair_print("  ", "sample", sample);
		if (r.sample == "stale")
			print("  (stale: the last read answered no data)\n");
	}
	if (r.sample == "sampled" || r.sample == "stale")
		monitor_values_print(r);
	pair_print("  ", "faults", monitor_faults_text(r));
	if (r.sample == "sampled")
		pair_print("  ", "sample", sample);
}

function cmd_line(args) {
	let line = line_arg_get(args, "line");
	if (line == null)
		return 1;

	let data = status_get();
	if (!data)
		return 1;

	let acct;
	for (let a in (data.lines ?? []))
		if (a.line == line)
			acct = a;

	let centry;
	for (let c in (data.call ?? []))
		if (c.line == line)
			centry = c;

	if (!acct && !centry) {
		printf("Line %s is not present in voice status.\n", line_text(line));
		return 1;
	}

	underline(sprintf("Line %s", line_text(line)), "=");

	if (acct && data.signaling?.protocol == "mgcp") {
		print("\n");
		underline("Endpoint", "-");
		pair_print("  ", "endpoint", acct.endpoint ?? "-");
		pair_print("  ", "service", cell_left(acct.service, 0,
		           mgcp_service_hue(acct.service)));
		pair_print("  ", "state", acct.state ?? "-");
		pair_print("  ", "notified_entity", acct.notified_entity || "(none)");
		pair_print("  ", "request_id", acct.request_id ?? "-");
		for (let c in (acct.connections ?? []))
			mgcp_conn_print(c, "  ");
	} else if (acct) {
		print("\n");
		underline("Account", "-");
		pair_print("  ", "aor", acct.aor ?? "-");
		pair_print("  ", "resolve_host", acct.resolve_host ?? "-");
		pair_print("  ", "dns_lookup", acct.dns_lookup ?? "-");
		pair_print("  ", "transport", acct.transport);
		pair_print("  ", "local_port", u32fix(acct.local_port));
		pair_print("  ", "family", acct.family ?? "-");
		pair_print("  ", "state", cell_left(acct.state, 0, reg_word_hue(acct.state)));
		pair_print("  ", "status", cell_left(acct.status, 0, reg_word_hue(acct.status)));
		if (acct.expires != null) {
			pair_print("  ", "expires", sprintf("%ss", u32fix(acct.expires)));
			pair_print("  ", "last_code",
			           cell_left(acct.last_code, 0, sip_code_hue(acct.last_code)));
			pair_print("  ", "targets", acct.targets);
			pair_print("  ", "dns_answer", acct.dns_answer ?? "-");
			pair_print("  ", "contact",
			           acct.contact != "" ? acct.contact : "(empty)");
		}
		if (acct.refresh_in != null)
			pair_print("  ", "refresh_in",
			           sprintf("%ss", u32fix(acct.refresh_in)));
		if (acct.registrar != null)
			pair_print("  ", "registrar",
			           host_port_join(acct.registrar, acct.registrar_port));
		if (acct.resolved)
			pair_print("  ", "resolved",
			           join(" ", map(acct.resolved,
			                         t => host_port_join(t.host, t.port))));
		if (acct.linger)
			pair_print("  ", "linger",
			           join(" ", map(acct.linger,
			                         e => sprintf("%s(%ss)",
			                                      host_port_join(e.host, e.port),
			                                      u32fix(e.age)))));
		if (acct.mwi_subscription != null)
			pair_print("  ", "mwi_subscription", acct.mwi_subscription);
		if (acct.listener != null)
			pair_print("  ", "listener", yn(acct.listener));
		if (acct.connection) {
			print("  connection\n");
			pair_print("    ", "state", acct.connection.state);
			if (acct.connection.peer)
				pair_print("    ", "peer", acct.connection.peer);
			pair_print("    ", "queued", acct.connection.queued);
		}
		if (acct.tls_error)
			pair_print("  ", "tls_error", c_red(acct.tls_error));

		if (acct.keepalive) {
			print("\n");
			underline("Keepalive", "-");
			printf("  %s\n", c_dim(KEEPALIVE_HEADER));
			keepalive_row_print(acct.keepalive, "  ");
		}
	}

	// A table inside a section indents under the title and carries no rule
	// of its own, so the section keeps one underline.
	if (centry) {
		print("\n");
		underline("Call state", "-");
		printf("  %s\n", c_dim(CALLS_HEADER));
		call_entry_print(centry, "  ");
	}

	let stats_res = voice_call("voice", "stats", { line: line }, UBUS_TIMEOUT);
	let srow;
	for (let r in (stats_res.data?.stats ?? []))
		if (r.line == line)
			srow = r;

	print("\n");
	underline("Stats", "-");
	if (!srow) {
		printf("  %s\n", c_dim(sprintf("(no stats row: %s)",
		       stats_res.err ?? "line not compiled")));
	} else {
		printf("  %s\n", c_dim(STATS_HEADER));
		stats_row_print(srow, "  ");
	}

	let streams = filter(data.streams ?? [], s => s.line == line);
	print("\n");
	underline("Live media", "-");
	if (length(streams) == 0)
		print("  " + c_dim("(no running streams)") + "\n");
	for (let row in streams) {
		printf("  stream %d%s\n", row.stream,
		       row.ui_slot != null ? sprintf(" (ui_slot %d)", row.ui_slot) : "");
		media_block_print(row.media, "    ");
	}

	let jbrows = filter(data.jb ?? [], r => r.line == line);
	print("\n");
	underline("Jitter buffer", "-");
	if (length(jbrows) == 0)
		print("  " + c_dim("(no rows: the voice stack reports no jitter buffer for this line)") + "\n");
	for (let r in jbrows)
		jb_row_block_print(r);

	let lecrows = filter(data.lec ?? [], r => r.line == line);
	let t38 = call_t38_check(centry);
	print("\n");
	underline("Echo canceller", "-");
	if (length(lecrows) == 0)
		print("  " + c_dim("(no rows: the voice stack reports no canceller for this line)") + "\n");
	for (let r in lecrows)
		lec_row_block_print(r, t38);

	let monrows = filter(data.monitor ?? [], r => r.line == line);
	print("\n");
	underline("Line monitor", "-");
	if (length(monrows) == 0)
		print("  " + c_dim("(no rows: the voice stack reports no line monitor for this line)") + "\n");
	for (let r in monrows)
		monitor_row_block_print(r);

	let cdr_res = voice_call("voice", "cdr", {}, UBUS_TIMEOUT);
	let records = filter(cdr_res.data?.cdr ?? [], r => r.line == line);
	if (length(records) > 5)
		records = slice(records, length(records) - 5);

	print("\n");
	underline("Recent calls", "-");
	if (length(records) == 0) {
		print("  " + c_dim("(none)") + "\n");
	} else {
		printf("  %s\n", c_dim(CDR_TABLE_HEADER));
		for (let rec in records)
			cdr_row_print(rec, "  ");
		printf("  (last %d of this line; %s shows the full ring)\n",
		       length(records), HINTS[HINT_STYLE].cdr);
	}

	return 0;
}

function cmd_calls(args) {
	let data = status_get();
	if (!data)
		return 1;

	let calls = data.call;
	if (!calls || length(calls) == 0) {
		print("No line state yet (the lines are not up).\n");
		return 0;
	}

	print_header(CALLS_HEADER);
	for (let c in calls)
		call_entry_print(c, "");
	return 0;
}

function cmd_streams(args) {
	let data = status_get();
	if (!data)
		return 1;

	let streams = data.streams;
	if (!streams || length(streams) == 0) {
		print("No running streams.\n");
		return 0;
	}

	print_header(sprintf("%-4s %-4s %-4s %10s %10s %8s %8s %6s %6s %8s %8s %8s %7s %7s %6s %6s %4s",
	       "Line", "Strm", "Slot", "Sent", "Recv", "Lost", "Disc",
	       "Loss", "LossF", "JitAvg", "JitMax", "JitFar", "RttAvg", "RttMax",
	       "MosLQ", "MosCQ", "R"));

	for (let row in streams) {
		let m = row.media;
		print(join(" ", [
			cell_left(line_text(row.line), 4, null),
			cell_left(row.stream, 4, null),
			cell_left(row.ui_slot ?? "-", 4, null),
			grid_num(m.packets_sent, 10, null),
			grid_num(m.packets_recv, 10, null),
			grid_num(m.packets_lost, 8, c_yellow),
			grid_num(m.packets_discarded, 8, c_yellow),
			grid_num(m.loss_rate, 6, c_yellow),
			grid_num(m.loss_rate_far, 6, c_yellow),
			grid_num(m.jitter_avg, 8, null),
			grid_num(m.jitter_max, 8, null),
			grid_num(m.jitter_far, 8, null),
			grid_num(m.rtt_avg, 7, null),
			grid_num(m.rtt_max, 7, null),
			cell_right(m.mos_lq ?? "-", 6, null),
			cell_right(m.mos_cq ?? "-", 6, null),
			cell_right(u32fix(m.rfactor) ?? "-", 4, null)
		]) + "\n");
	}

	printf("\nJitter and round-trip figures are ms (this array is udsp-only).\n");
	printf("Total: %d stream(s)\n", length(streams));
	return 0;
}

// On udsp the event cache repeats the jb array. The cache stands in only
// where nothing else measured anything: a backend that answers no jb row
// leaves every row at never.
function jb_events_print(rows, events) {
	if (length(events) == 0)
		return;
	for (let r in rows)
		if (r.sample != "never")
			return;

	print("\n");
	underline("Jitter buffer events (backend cache)", "-");
	print_header(sprintf("%-4s %-4s %10s %10s %6s %6s %s",
	       "Line", "Strm", "Total", "Drop", "Size", "Jit", "Age"));

	for (let e in events)
		printf("%-4s %-4s %10s %10s %6s %6s %s\n",
		       line_text(e.line), e.stream, e.total, e.drop, u32fix(e.size_ms),
		       u32fix(e.jitter_avg), age_text(u32fix(e.stamp_ms)));

	printf("\nSize and Jit are ms. The backend pushes these rows on its own clock.\n");
}

function cmd_jb(args) {
	let data = status_get();
	if (!data)
		return 1;

	let rows = data.jb;
	if (!rows || length(rows) == 0) {
		print("No jitter buffer rows (the voice stack reports no capability).\n");
		return 0;
	}

	print_header(sprintf("%-4s %-4s %-9s %-9s %4s %4s %4s %5s %9s %6s %5s %5s %5s %5s %5s %5s %5s %5s %6s %5s %s",
	       "Line", "Strm", "Sample", "Mode", "Tgt", "Dep", "Jit", "Pool",
	       "Frames", "PLC", "CN", "Sil", "Undr", "Late", "Dup", "Ovfl",
	       "Hold", "Skew", "Rsync", "Strv", "Age"));

	for (let r in rows) {
		// a never row reads zeros on the wire and none is a measurement
		let never = r.sample == "never";
		let num = (v) => never ? null : v;
		print(join(" ", [
			cell_left(line_text(r.line), 4, null),
			cell_left(r.stream, 4, null),
			sample_hue(pad_right(r.sample ?? "-", 9), r.sample),
			cell_left(r.mode ?? "-", 9, never ? c_dim : null),
			grid_num(num(u32fix(r.target_ms)), 4, null),
			grid_num(num(u32fix(r.depth_ms)), 4, null),
			grid_num(num(u32fix(r.jitter_ms)), 4, null),
			grid_num(num(u32fix(r.pool_free)), 5, null),
			grid_num(num(r.frames_out), 9, null),
			grid_num(num(r.plc), 6, c_yellow),
			grid_num(num(r.cn), 5, null),
			grid_num(num(r.silence), 5, null),
			grid_num(num(r.underruns), 5, c_yellow),
			grid_num(num(r.late_drop), 5, c_yellow),
			grid_num(num(r.dup_drop), 5, c_yellow),
			grid_num(num(r.overflow_drop), 5, c_yellow),
			grid_num(num(r.hold_drop), 5, c_yellow),
			grid_num(num(r.skew_drops), 5, c_yellow),
			grid_num(num(r.resyncs), 6, c_yellow),
			grid_num(num(r.starve_resyncs), 5, c_yellow),
			row_age_text(r)
		]) + "\n");
	}

	printf("\nTgt/Dep/Jit in ms. Under a fax or modem passthrough the mode reads fixed and Tgt is\n");
	printf("the depth the line adapted to, not the configured jitter_target.\n");
	printf("Undr underruns; Late, Dup, Ovfl and Hold the four drop counters; Skew skew\n");
	printf("drops; Rsync resyncs; Strv starvation resyncs; Pool free payload slots.\n");
	printf("Total: %d row(s)\n", length(rows));

	jb_events_print(rows, data.jb_events ?? []);
	return 0;
}

// The share of the judged frames the processor acted on. The five verdicts
// of a frame with an active reference window make the denominator, so the
// quiet frames and the frames the processor was off for stay out of it.
function lec_duty_text(st) {
	let sum = 0;
	for (let v in [ st.nlp_active_frames, st.nlp_wait_frames,
	                st.nlp_erle_frames, st.nlp_unlike_frames,
	                st.nlp_dt_frames ]) {
		if (v == null)
			return "-";
		sum += v;
	}
	return pct_text(st.nlp_active_frames, sum);
}

function cmd_lec(args) {
	let data = status_get();
	if (!data)
		return 1;

	let rows = data.lec;
	if (!rows || length(rows) == 0) {
		print("No echo canceller rows (the voice stack reports no capability).\n");
		return 0;
	}

	const fmt = "%-4s %-9s %-7s %-11s %-3s %-6s %9s %10s %5s %6s %6s %-6s %-4s %5s %s";

	print_header(sprintf(fmt,
	       "Line", "Sample", "Kind", "Src", "EC", "NLP", "ERLE", "Frames",
	       "Held", "Tail", "Rate", "RefRdy", "Pend", "Duty", "Age"));

	for (let r in rows) {
		let st = r.state ?? {};
		let policy = r.policy ?? {};
		let rate = lec_rate_khz(st);
		let pending = r.applied != null ? r.applied.pending : null;
		print(join(" ", [
			cell_left(line_text(r.line), 4, null),
			sample_hue(pad_right(r.sample ?? "-", 9), r.sample),
			cell_left(policy.kind ?? "-", 7, null),
			cell_left(policy.source ?? "-", 11, null),
			cell_left(st.ec ?? "-", 3, null),
			cell_left(st.nlp ?? "-", 6, null),
			cell_right(format_erle(st.erle_mdb), 9, null),
			cell_right(st.frames ?? "-", 10, null),
			cell_right(pct_text(st.hold_frames, st.frames), 5, null),
			cell_right(r.state != null
			           ? sprintf("%d ms", u32fix(st.tail_ms)) : "-", 6, null),
			cell_right(rate != null ? sprintf("%d kHz", rate) : "-", 6, null),
			cell_left(st.ref_ready != null ? yn(st.ref_ready) : "-", 6, null),
			cell_left(pending != null ? yn(pending) : "-", 4,
			          pending ? c_yellow : null),
			cell_right(lec_duty_text(st), 5, null),
			row_age_text(r)
		]) + "\n");
	}

	printf("\nSample: live is read this frame, last_call is the reading kept from the last\n");
	printf("call, never means no canceller ran. %s prints the uci, pushed and\n",
	       HINTS[HINT_STYLE].line);
	printf("engine rows and the tone line.\n");
	printf("Duty: the processor acted on this share of the frames it judged, the frames\n");
	printf("with an active reference window.\n");
	printf("Total: %d row(s)\n", length(rows));
	return 0;
}

function cmd_monitor(args) {
	let data = status_get();
	if (!data)
		return 1;

	let rows = data.monitor;
	if (!rows || length(rows) == 0) {
		print("No line monitor rows (the voice stack reports no capability).\n");
		return 0;
	}

	// The column names are the words of the Line monitor block, so a
	// reader moves between the table and uvoice.line N without a mapping.
	// The unit rides the value here, because one flat row carries both a
	// voltage and a current of the same pair.
	const fmt = "%-4s %-7s %9s %9s %8s %10s %9s %8s " +
	            "%-4s %-4s %-8s %-20s %-4s %s";

	print_header(sprintf(fmt,
	       "Line", "Sample", "Tip-ring", "Battery", "Target",
	       "Tip-ring", "Long", "HVIC",
	       "DCDC", "OVLO", "Linefeed", "Fault", "Open", "Age"));

	for (let r in rows) {
		let bits = fault_bits_text(r.fault_bits);
		print(join(" ", [
			cell_left(line_text(r.line), 4, null),
			sample_hue(pad_right(r.sample ?? "-", 7), r.sample),
			cell_right(format_mv(r.vtr_mv), 9, null),
			cell_right(format_mv(r.vbat_mv), 9, null),
			cell_right(format_mv(r.vbat_tgt_mv), 8, null),
			cell_right(format_ma(r.itr_ua), 10, null),
			cell_right(format_ma(r.ilong_ua), 9, null),
			cell_right(format_mw(r.p_hvic_mw), 8, null),
			cell_left(r.dcdc_down != null ? yn(r.dcdc_down) : "-", 4,
			          r.dcdc_down ? c_red : null),
			cell_left(r.ovlo != null ? yn(r.ovlo) : "-", 4,
			          r.ovlo ? c_red : null),
			cell_left(r.linefeed != null
			          ? sprintf("0x%02x", u32fix(r.linefeed)) : "-", 8, null),
			cell_left(bits, 20,
			          bits == "none" ? c_dim : (bits == "-" ? null : c_red)),
			cell_left(yn(r.fault_open), 4, r.fault_open ? c_red : null),
			monitor_age_text(r)
		]) + "\n");
	}

	return 0;
}

function cdr_record_detail(rec) {
	underline(sprintf("CDR %d", rec.seq), "=");
	pair_print("  ", "line", line_text(rec.line));
	pair_print("  ", "slot", rec.slot);
	pair_print("  ", "direction", rec.inbound ? "inbound" : "outbound");
	if (rec.number)
		pair_print("  ", "number", rec.number);
	if (rec.remote)
		pair_print("  ", "remote", rec.remote);
	if (rec.name)
		pair_print("  ", "name", rec.name);
	pair_print("  ", "start", format_timestamp(rec.start));
	pair_print("  ", "duration", format_duration_ms(u32fix(rec.duration_ms)));
	pair_print("  ", "answered", yn(rec.answered));
	if (rec.answered)
		pair_print("  ", "answer_offset",
		           format_duration_ms(u32fix(rec.answer_offset_ms)));
	if (rec.fail)
		pair_print("  ", "fail", rec.fail);
	if (rec.codec) {
		pair_print("  ", "codec", rec.codec);
		pair_print("  ", "ptime", sprintf("%s ms", rec.ptime));
	}
	if (rec.rtp_port != null)
		pair_print("  ", "rtp_port", u32fix(rec.rtp_port));
	if (rec.transferred)
		pair_print("  ", "transferred", "yes");
	if (rec.transferred_from != null)
		pair_print("  ", "transferred_from", sprintf("seq %d", rec.transferred_from));
	if (rec.conferenced)
		pair_print("  ", "conferenced", "yes");
	if (rec.in_progress)
		pair_print("  ", "in_progress", "yes");
	pair_print("  ", "media_state", rec.media_state ?? "-");

	if (rec.media) {
		print("\n");
		underline("Media", "-");
		media_block_print(rec.media, "  ");
	}
	if (rec.jb) {
		print("\n");
		underline("Jitter buffer", "-");
		pair_print("  ", "mode", rec.jb.mode ?? "-");
		jb_block_print(rec.jb, "  ");
	}
}

function cmd_cdr(args) {
	let res = voice_call("voice", "cdr", {}, UBUS_TIMEOUT);
	if (res.err != null)
		return call_fail("voice cdr", res);

	let records = res.data.cdr ?? [];

	if (args.seq != null && args.seq !== true) {
		let seq = int(args.seq);
		for (let rec in records) {
			if (rec.seq != seq)
				continue;
			cdr_record_detail(rec);
			return 0;
		}
		printf("No call record with seq %d (the ring keeps the last 64).\n", seq);
		return 1;
	}

	if (length(records) == 0) {
		print("No call records.\n");
		return 0;
	}

	let shown = records;
	if (args.limit != null && args.limit !== true) {
		let limit = int(args.limit);
		if (limit > 0 && length(records) > limit)
			shown = slice(records, length(records) - limit);
	}

	print_header(CDR_TABLE_HEADER);
	for (let rec in shown)
		cdr_row_print(rec, "");

	printf("\nShowing %d of %d record(s), oldest first; --seq N for one record in full\n",
	       length(shown), length(records));
	return 0;
}

function cmd_stats(args) {
	let params = {};
	if (args.line != null && args.line !== true)
		params.line = int(args.line);

	let res = voice_call("voice", "stats", params, UBUS_TIMEOUT);
	if (res.err != null)
		return call_fail("voice stats", res);

	let rows = res.data.stats ?? [];
	if (length(rows) == 0) {
		print("No stats rows.\n");
		return 0;
	}

	print_header(STATS_HEADER);
	for (let r in rows)
		stats_row_print(r, "");

	printf("\nTotals move at call teardown; stats_reset clears counters and totals.\n");
	return 0;
}

function cmd_stats_live(args) {
	let line = line_arg_get(args, "stats_live");
	if (line == null)
		return 1;

	let slot = 0;
	if (args.slot != null && args.slot !== true)
		slot = int(args.slot);

	let res = voice_call("voice", "stats_live",
	                     { line: line, slot: slot }, UBUS_LIVE_TIMEOUT);
	if (res.err != null)
		return deferred_fail("voice stats_live", res, line);

	underline(sprintf("Line %s slot %d live media",
	                  line_text(res.data.line ?? line),
	                  res.data.slot ?? slot), "-");
	media_block_print(res.data.media, "  ");
	return 0;
}

function cmd_stats_reset(args) {
	let res = voice_call("voice", "stats_reset", {}, UBUS_TIMEOUT);
	if (res.err != null)
		return call_fail("voice stats_reset", res);

	printf("Reset: %s%s\n", join(", ", res.data.reset ?? []),
	       res.data.ring_kept ? " (cdr ring kept)" : "");
	return 0;
}

function config_channel_print(ch) {
	underline(sprintf("Channel %s", line_text(ch.id)), "=");
	kv_print(ch, "  ", ["id"], CONFIG_LEGEND);
	if (ch.timers) {
		print("\n");
		underline("SIP timers (ms)", "-");
		kv_print(ch.timers, "  ", null, null);
	}
}

// The TR-104 codec ids of a uci codec section (option tr104_id).
const TR104_CODEC_NAMES = {
	[1]: "G.711u", [2]: "G.711a", [3]: "G.729a", [4]: "G.726-32",
	[5]: "G.722", [6]: "Opus"
};

// The channel option of a codec section is 1-based and 0 names every
// channel (call.c matches cc->channel - 1 against ch->id), while every
// other view prints the id through the display base.
function codec_channel_text(c) {
	return c.channel ? line_text(c.channel - 1) : "all";
}

function config_codecs_print(data) {
	let rows = sort([...(data.codec ?? [])],
	                (a, b) => a.channel - b.channel || a.priority - b.priority);
	if (length(rows) == 0) {
		print("No codec rows.\n");
		return;
	}

	const fmt = "%-7s %-9s %-6s %-8s %5s %8s %9s %-4s %11s %-4s";

	print_header(sprintf(fmt,
	       "Channel", "Codec", "TR-104", "Enabled", "Prio", "PrioDef",
	       "Bitrate", "FEC", "Complexity", "VAD"));

	for (let c in rows)
		printf(fmt + "\n", codec_channel_text(c),
		       TR104_CODEC_NAMES[c.tr104_id] ?? c.tr104_id, c.tr104_id,
		       yn(c.enabled), c.priority, c.default_priority, c.bitrate,
		       yn(c.fec), c.complexity, yn(c.silence_suppression));

	printf("\nChannel is the id %s takes; all is a row every channel shares.\n",
	       HINTS[HINT_STYLE].channel);
	printf("Rows sort by channel, then priority.\n");
}

function cmd_config(args) {
	let res = voice_call("voice", "dump_config", {}, UBUS_TIMEOUT);
	if (res.err != null)
		return call_fail("voice dump_config", res);
	let data = res.data;

	if (args.channel != null && args.channel !== true) {
		let id = int(args.channel);
		for (let ch in (data.channel ?? [])) {
			if (ch.id != id)
				continue;
			config_channel_print(ch);
			return 0;
		}
		printf("No channel with id %s (the config carries %d).\n",
		       line_text(id), length(data.channel ?? []));
		return 1;
	}

	if (args.codecs) {
		config_codecs_print(data);
		return 0;
	}

	underline("uvoice Configuration", "=");
	kv_print(data, "  ", null, CONFIG_LEGEND);
	if (data.debug)
		kv_line("  ", "trace (uci)",
		        data.debug.trace != "" ? data.debug.trace : "(none)");

	print("\n");
	underline("HAL", "-");
	kv_print(data.hal ?? {}, "  ", null, null);
	return 0;
}

function linetest_value_format(key, value) {
	if (value == "open")
		return "open";
	if (type(value) == "bool")
		return yn(value);
	if (key == "ren_milli")
		return sprintf("%.3f REN", value / 1000.0);
	if (wildcard(key, "*_mvrms"))
		return sprintf("%d mVrms", value);
	if (wildcard(key, "*_mv"))
		return sprintf("%d mV", value);
	if (wildcard(key, "*_ohm"))
		return sprintf("%d ohm", u32fix(value));
	if (wildcard(key, "*_nf"))
		return sprintf("%d nF", u32fix(value));
	return sprintf("%s", value);
}

function cmd_linetest(args) {
	let line = line_arg_get(args, "linetest");
	if (line == null)
		return 1;

	let test = "gr909";
	if (args.test != null && args.test !== true)
		test = args.test;
	if (index(LINE_TESTS, test) < 0) {
		printf("Error: test \"%s\" is not known; expected %s\n",
		       test, join(", ", LINE_TESTS));
		return 1;
	}

	printf("Running %s on line %d (a test can take a while)...\n", test, line);
	let res = voice_call("voice", "line_test",
	                     { line: line, test: test }, UBUS_LINE_TEST_TIMEOUT);
	if (res.err != null)
		return deferred_fail("voice line_test", res, line);

	let data = res.data;
	printf("\nLine %s %s: %s\n", line_text(data.line ?? line),
	       data.test ?? test, data.status ?? "?");
	if (data.status == "malformed") {
		printf("  The record is malformed (raw length %s)\n", data.length);
		return 1;
	}
	if (data.status == "failed")
		printf("  MLT status code: %s\n", data.code);

	let rec = data.record ?? {};
	if (rec.valid != null)
		printf("  record v%s, valid mask 0x%x\n", rec.version ?? "?",
		       u32fix(rec.valid));
	for (let key, value in rec) {
		if (index(LINETEST_META_KEYS, key) >= 0)
			continue;
		pair_print("  ", key, linetest_value_format(key, value));
	}

	return data.status == "complete" ? 0 : 1;
}

// The manufacturing loop test (ubus voice loop_test). The two numbers keep
// their ubus names, and the daemon bounds them: this verb sends what the
// operator typed and prints the mode the daemon answered with.
function looptest_reply_print(data) {
	underline("Loop test", "-");
	pair_print("  ", "enabled", yn(data.enabled));
	pair_print("  ", "mode", data.mode ?? "off");
	pair_print("  ", "lines", data.lines ?? 0);
	pair_print("  ", "ring_s", data.ring_s ?? 0);
	pair_print("  ", "delay_ms", data.delay_ms ?? 0);
}

// parse_args() records an option whose next token starts with "-" as the bare
// value true, so a missing value and a negative number reach this verb the
// same way. Both are named here, because a silent fall back to the default
// would arm the test with a window the operator did not ask for.
function looptest_number_take(args, key, params, name) {
	let text = args[key];

	if (text == null)
		return true;

	if (text === true) {
		printf("Error: --%s needs a value\n", key);
		return false;
	}

	if (!match(text, LOOPTEST_UINT_RE)) {
		printf("Error: --%s takes a whole number, not \"%s\"\n", key,
		       text);
		return false;
	}

	params[name] = int(text);

	return true;
}

function cmd_looptest(args) {
	let params = {};

	if (args.start && args.stop) {
		print("Error: looptest takes --start or --stop, not both\n");
		return 1;
	}
	if (!args.start && !args.stop) {
		print("Usage: looptest --start|--stop [--mode self|cross] " +
		      "[--ring-s N] [--delay-ms N]\n");
		return 1;
	}

	params.enable = args.start ? true : false;
	if (args.mode != null) {
		if (args.mode === true) {
			print("Error: --mode needs a value\n");
			return 1;
		}
		if (args.mode != "self" && args.mode != "cross") {
			printf("Error: mode \"%s\" is not known; expected self or cross\n",
			       args.mode);
			return 1;
		}
		params.mode = args.mode;
	}
	if (!looptest_number_take(args, "ring-s", params, "ring_s"))
		return 1;
	if (!looptest_number_take(args, "delay-ms", params, "delay_ms"))
		return 1;

	let res = voice_call("voice", "loop_test", params, UBUS_TIMEOUT);
	if (res.err != null) {
		let why = res.err;
		if (res.code == 2)
			why = "a value is out of range, or the mode word is " +
			      "not self or cross";
		else if (res.code == 5)
			why = "the dsp is not ready, a call or a line test " +
			      "is running, or the backend carries no loop";
		printf("Error: voice loop_test failed: %s\n", why);
		return 1;
	}

	looptest_reply_print(res.data);
	if (res.data.enabled)
		print("\nEvery line is out of service while the test runs.\n");
	return 0;
}

function trace_table_print(data) {
	underline("Trace domains", "-");
	for (let d in TRACE_DOMAINS)
		pair_print("  ", d, onoff(data[d]));
}

function cmd_trace(args) {
	let res = voice_call("voice.trace", "get", {}, UBUS_TIMEOUT);
	if (res.err != null)
		return call_fail("voice.trace get", res);
	trace_table_print(res.data);
	return 0;
}

function bool_parse(text) {
	if (text === true || text == "on" || text == "true" || text == "1")
		return true;
	if (text == "off" || text == "false" || text == "0")
		return false;
	return null;
}

function cmd_trace_set(args) {
	let params = {};
	for (let d in TRACE_DOMAINS) {
		if (!(d in args))
			continue;
		let v = bool_parse(args[d]);
		if (v == null) {
			printf("Error: --%s takes on or off\n", d);
			return 1;
		}
		params[d] = v;
	}

	if (length(params) == 0) {
		printf("Usage: trace_set --<domain> on|off  (domains: %s)\n",
		       join(" ", TRACE_DOMAINS));
		return 1;
	}

	let res = voice_call("voice.trace", "set", params, UBUS_TIMEOUT);
	if (res.err != null)
		return call_fail("voice.trace set", res);
	trace_table_print(res.data);
	return 0;
}

// register and unregister are rows of the SIP table, so the daemon carries
// them only while it runs sip.
function account_verb_run(method, args) {
	let line = line_arg_get(args, method);
	if (line == null)
		return 1;

	let data = status_get();
	if (!data)
		return 1;
	let protocol = data.signaling?.protocol;
	if (protocol != "sip" || data.signaling?.state != "running") {
		printf("Error: %s applies to a running sip protocol only, " +
		       "and the daemon reports %s %s\n", method,
		       protocol ?? "no protocol",
		       data.signaling?.state ?? "unknown");
		return 1;
	}

	let res = voice_call("voice", method, { line: line }, UBUS_TIMEOUT);
	if (res.err != null)
		return call_fail("voice " + method, res);

	printf("Line %s: %s requested (voice status reports the outcome)\n",
	       line_text(res.data.line ?? line), res.data.requested ?? method);
	return 0;
}

function cmd_register(args) {
	return account_verb_run("register", args);
}

function cmd_unregister(args) {
	return account_verb_run("unregister", args);
}

function inject_verb_run(method, args) {
	let line = line_arg_get(args, method);
	if (line == null)
		return 1;

	let params = { line: line };
	if (method == "dial") {
		if (args.number == null || args.number === true) {
			print("Error: --number required for dial\n");
			return 1;
		}
		if (match(args.number, DIAL_RE) == null || length(args.number) > 32) {
			print("Error: the number must be 1 to 32 characters from 0-9, * and #\n");
			return 1;
		}
		params.number = args.number;
	}

	let res = voice_call("voice", method, params, UBUS_TIMEOUT);
	if (res.err != null)
		return call_fail("voice " + method, res);

	if (method == "dial")
		printf("Line %s: injected dial, %d digit event(s)\n",
		       line_text(res.data.line ?? line), res.data.digits ?? 0);
	else
		printf("Line %s: injected %s\n",
		       line_text(res.data.line ?? line), res.data.injected ?? method);
	return 0;
}

function cmd_seize(args) {
	return inject_verb_run("seize", args);
}

function cmd_release(args) {
	return inject_verb_run("release", args);
}

function cmd_dial(args) {
	return inject_verb_run("dial", args);
}

function cmd_reload(args) {
	let res = voice_call("voice", "reload", {}, UBUS_TIMEOUT);
	if (res.err != null)
		return call_fail("voice reload", res);

	printf("Reloaded: enabled %s, %s channel(s)\n",
	       yn(res.data.enabled), res.data.channels ?? 0);
	return 0;
}

function stub_line_print(l) {
	underline(sprintf("Line %s", line_text(l.index)), "-");
	kv_print(l, "  ", ["index"], null);
	for (let s in (l.stream ?? [])) {
		printf("  stream %d\n", s.index);
		kv_print(s, "    ", ["index"], null);
	}
	print("\n");
}

function cmd_stub_dump(args) {
	let res = voice_call("voice.stub", "dump", {}, UBUS_TIMEOUT);
	if (res.err != null) {
		printf("Error: voice.stub dump failed: %s (voice.stub exists on stub HAL builds only)\n",
		       res.err);
		return 1;
	}

	pair_print("", "region", res.data.region ?? "-");
	print("\n");
	for (let l in (res.data.line ?? []))
		stub_line_print(l);
	return 0;
}

const CALLTRACE_ERRORS = {
	busy: "another trace or its child process is still active",
	bad_line: "the backend or line is absent, or the channel is disabled",
	bad_range: "a timeout exceeds its limit or a size cap is zero",
	no_udebug: "this build has no udebug producer support",
	rings: "the daemon could not map the trace rings",
	no_tcpdump: "the wire capture needs /usr/bin/tcpdump",
	no_taskset: "the wire capture needs /usr/bin/taskset",
	no_space: "/tmp needs the size caps plus 16 MiB of free space, and " +
		"the trace directory of a done or failed trace counts as free",
	workdir: "the daemon could not create the trace directory"
};
const CALLTRACE_UINT_RE = /^[0-9]+$/;

function calltrace_uint_get(value, name) {
	if (type(value) == "string" &&
	    match(value, CALLTRACE_UINT_RE) && length(value) <= 10 &&
	    int(value) <= 0xffffffff)
		return int(value);
	warn(sprintf("Error: --%s needs an unsigned 32-bit integer\n", name));
	return null;
}

function shell_arg_quote(text) {
	return "'" + replace(text, "'", "'\\''") + "'";
}

function calltrace_manifest_read(data, name) {
	let text;
	if (data.workdir)
		text = fs.readfile(data.workdir + "/" + name);
	else if (data.archive && data.id) {
		let pipe = fs.popen("tar -xzOf " + shell_arg_quote(data.archive) +
		                   " " + shell_arg_quote(data.id + "/" + name) +
		                   " 2>/dev/null", "r");
		if (pipe) {
			text = pipe.read("all");
			pipe.close();
		}
	}
	if (text == null)
		return null;
	try {
		return json(text);
	}
	catch (e) {
		return null;
	}
}

function calltrace_manifest_get(data) {
	let manifest = calltrace_manifest_read(data, "manifest.json");
	if (manifest?.collectors != null)
		return manifest;
	let extra = calltrace_manifest_read(data, "manifest-finisher.json");
	if (extra == null)
		return manifest;
	return { ...(manifest ?? {}), ...extra,
		notes: [...(manifest?.notes ?? []), ...(extra.notes ?? [])],
		contains: uniq([...(manifest?.contains ?? []), ...(extra.contains ?? [])]) };
}

function calltrace_output(data, args) {
	if (args.json) {
		printf("%J\n", data);
		return;
	}
	printf("Call trace: %s\n", data.state);
	pair_print("  ", "id", data.id ?? "-");
	if (data.line != null) {
		let line = u32fix(data.line);
		pair_print("  ", "line", sprintf("%d (index %d)", line + 1, line));
	}
	if (data.waiting_for)
		pair_print("  ", "waiting_for", data.waiting_for == "line_idle"
		           ? "line_idle: the line must rest before its next call"
		           : "call: the next call on this line");
	if (data.wire)
		pair_print("  ", "wire.ready", yn(data.wire.ready));
	if (data.rings)
		pair_print("  ", "rings.registered", yn(data.rings.registered));
	if (data.options) {
		// A daemon older than the capture sends neither key.
		let lec = data.options.lec;
		let cap = data.options.lec_max_mb;

		pair_print("  ", "options.lec", lec == null ? "-" : yn(lec));
		pair_print("  ", "options.lec_max_mb",
		           cap == null ? "-" : sprintf("%d", u32fix(cap)));
	}
	let degraded = [];
	if (data.wire?.enabled === false)
		push(degraded, "wire.enabled false (no wire capture)");
	if (data.wire?.truncated)
		push(degraded, "wire.truncated");
	if (data.wire?.exit != null && data.wire.exit != 0)
		push(degraded, sprintf("wire.exit %d", data.wire.exit));
	if (data.wire?.enabled && data.wire.running === false &&
	    index(["armed", "running"], data.state) >= 0)
		push(degraded, "wire stopped before the trace ended");
	if (data.rings?.truncated)
		push(degraded, "rings.truncated");
	if (data.rings?.relay_dropped > 0)
		push(degraded, sprintf("relay_dropped %d", data.rings.relay_dropped));
	pair_print("  ", "degraded", length(degraded) ? join(", ", degraded) : "none");
	if (data.elapsed_ms != null) {
		pair_print("  ", "elapsed", format_duration_ms(data.elapsed_ms));
		pair_print("  ", "window remaining", format_duration_ms(max(0,
		           u32fix(data.options.timeout_s) * 1000 - data.elapsed_ms)));
	}
	if (data.last_error)
		pair_print("  ", "last_error", data.last_error +
		           (CALLTRACE_ERRORS[data.last_error]
		            ? ": " + CALLTRACE_ERRORS[data.last_error] : ""));
	// A refusal writes over last_error, and failure keeps the cause.
	if (data.failure)
		pair_print("  ", "failure", data.failure);
	if (data.archive) {
		let info = fs.stat(data.archive);
		pair_print("  ", "archive", data.archive);
		pair_print("  ", "size", info ? format_bytes(info.size) : "unavailable");
		pair_print("  ", "summary",
		           sprintf("tar -xzOf %s %s/SUMMARY.txt", data.archive,
		                   data.id ?? "<id>"));
	}
	if (data.reason)
		pair_print("  ", "reason", data.reason);
	if (data.reason || data.state == "done" || data.state == "failed") {
		let manifest = calltrace_manifest_get(data);
		let omissions = [];
		for (let name, result in (manifest?.collectors ?? {}))
			if (result != "ok")
				push(omissions, name + ": " + result);
		pair_print("  ", "omissions", manifest?.collectors == null
		           ? "collector results unavailable"
		           : (length(omissions) ? join(", ", omissions) : "none"));
		for (let note in (manifest?.notes ?? []))
			pair_print("  ", "note", note);
	}
	if (data.archive) {
		print("  Privacy: handle this archive like a packet capture.\n");
		print("  It can contain credentials, keys, numbers, and audio.\n");
	}
}

function calltrace_command(method, params, args) {
	let res = voice_call("voice.calltrace", method, params, UBUS_TIMEOUT);
	if (res.err != null) {
		// A refused arm sends a status code alone; status owns last_error.
		let status = voice_call("voice.calltrace", "status", {}, UBUS_TIMEOUT);
		if (status.data != null)
			calltrace_output(status.data, args);
		if (!args.json && params.line != null)
			printf("  requested line %d (index %d)\n",
			       params.line + 1, params.line);
		warn(sprintf("Error: voice.calltrace %s failed: %s\n", method, res.err));
		return 1;
	}
	calltrace_output(res.data, args);
	return res.data.state == "failed" ? 1 : 0;
}

function cmd_calltrace_arm(args) {
	let line = line_arg_get(args, "calltrace_arm");
	if (line == null)
		return 1;
	line = calltrace_uint_get(args.line, "line");
	if (line == null)
		return 1;
	let params = { line: line };
	const options = {
		timeout: "timeout", grace: "grace", "arm-timeout": "arm_timeout",
		"wire-max-mb": "wire_max_mb", "rings-max-mb": "rings_max_mb",
		"lec-max-mb": "lec_max_mb"
	};
	for (let flag, key in options) {
		if (!(flag in args))
			continue;
		let value = calltrace_uint_get(args[flag], flag);
		if (value == null)
			return 1;
		params[key] = value;
	}
	if (args["no-wire"])
		params.wire = false;
	if (args.lec)
		params.lec = true;
	return calltrace_command("arm", params, args);
}

function cmd_calltrace_status(args) {
	return calltrace_command("status", {}, args);
}

function cmd_calltrace_cancel(args) {
	return calltrace_command("cancel", {}, args);
}

function calltrace_clock_ms() {
	let now = clock(true);
	return now[0] * 1000 + now[1] / 1000000;
}

function calltrace_progress_print(data, args) {
	if (args?.json) {
		calltrace_output(data, args);
		return;
	}
	let remaining = max(0, u32fix(data.options?.timeout_s ?? 0) * 1000 -
	                    (data.elapsed_ms ?? 0));
	printf("Call trace: %s, elapsed %s, remaining %s\n", data.state,
	       format_duration_ms(data.elapsed_ms), format_duration_ms(remaining));
}

function cmd_calltrace_wait(args) {
	let id = args.id;
	if (id === true) {
		warn("Error: --id needs a trace id\n");
		return 1;
	}
	let seconds;
	if (args.deadline != null) {
		seconds = calltrace_uint_get(args.deadline, "deadline");
		if (seconds == null)
			return 1;
	}
	let start = calltrace_clock_ms();
	let deadline;
	let extended;
	while (true) {
		let res = voice_call("voice.calltrace", "status", {}, UBUS_TIMEOUT);
		if (res.err != null) {
			warn(sprintf("Error: voice.calltrace status failed: %s\n", res.err));
			return 1;
		}
		let data = res.data;
		if (deadline == null)
			deadline = start + 1000 * (seconds ??
				((data.options?.arm_timeout_s ?? 3600) +
				 (data.options?.timeout_s ?? 300) + 120));
		// The daemon names the budget of the finisher it started, so
		// the default deadline covers that run, plus 30 s for the kill
		// and the reap. Once, so a daemon stuck in finishing cannot
		// hold the wait.
		if (seconds == null && !extended &&
		    data.finish_budget_ms != null) {
			deadline = max(deadline, calltrace_clock_ms() +
			               u32fix(data.finish_budget_ms) + 30000);
			extended = true;
		}
		if (id == null)
			id = data.id;
		if (data.id != null && data.id != id) {
			calltrace_progress_print(data, args);
			warn(sprintf("Error: trace id changed from %s to %s\n", id, data.id));
			return 1;
		}
		if (data.state == "done" || data.state == "failed" ||
		    data.state == "idle") {
			calltrace_output(data, args);
			if (data.state == "idle" && !data.reason)
				warn("Error: no call trace is active\n");
			return data.state == "done" ? 0 : 1;
		}
		if (calltrace_clock_ms() >= deadline) {
			calltrace_progress_print(data, args);
			warn(sprintf("Error: call trace %s wait deadline reached\n", id));
			return 2;
		}
		if (!args.json)
			calltrace_progress_print(data);
		sleep(min(1000, max(1, deadline - calltrace_clock_ms())));
	}
}

function usage() {
	print("Usage: uvoice-cli.uc <command> [options]\n\n");
	print("Read commands:\n");
	print("  status                 Daemon summary: version, config, HAL, events\n");
	print("  lines                  Account and registration table, or the MGCP endpoints\n");
	print("  line --line N          Complete line status: account, call state, stats,\n");
	print("                         live media, jitter buffer, echo canceller, recent calls\n");
	print("  calls                  Per-line FSM state and call legs\n");
	print("  streams                Live media counters per running stream\n");
	print("  jb                     Jitter buffer rows, one per line and stream\n");
	print("  lec                    Echo canceller rows, one per line\n");
	print("  monitor                Line monitor rows, one per line\n");
	print("  cdr [--limit N] [--seq N]\n");
	print("                         Call records, oldest first; --seq for one record\n");
	print("  stats [--line N]       Per-line call counters and lifetime totals\n");
	print("  config [--channel N | --codecs]\n");
	print("                         Loaded configuration views\n");
	print("  trace                  Trace domain table\n");
	print("\nAction commands:\n");
	print("  register --line N      Ask the account to REGISTER now\n");
	print("  unregister --line N    Stop the registration\n");
	print("  seize --line N         Inject a SEIZE hook event\n");
	print("  release --line N       Inject a RELEASE hook event\n");
	print("  dial --line N --number X\n");
	print("                         Seize when on hook, then inject the digits\n");
	print("  stats_live --line N [--slot S]\n");
	print("                         Live media counters of a running call leg\n");
	print("  linetest --line N [--test gr909|roh|capacitance|loop]\n");
	print("                         Run a metallic line test (default gr909)\n");
	print("  looptest --start|--stop [--mode self|cross] [--ring-s N]\n");
	print("      [--delay-ms N]\n");
	print("                         Arm or disarm the manufacturing loop test;\n");
	print("                         every line is out of service while it runs\n");
	print("  stats_reset            Clear counters and totals, keep the cdr ring\n");
	print("  reload                 Re-read UCI and rebuild the accounts\n");
	print("  trace_set --<domain> on|off\n");
	print("                         Domains: sip fsm hook hal dialplan media jb\n");
	print("\nCall trace commands (line is a 0-based index):\n");
	print("  calltrace_arm --line N [--timeout S] [--grace S]\n");
	print("      [--arm-timeout S] [--no-wire] [--wire-max-mb M]\n");
	print("      [--rings-max-mb M] [--lec] [--lec-max-mb M]\n");
	print("                         --lec captures the echo canceller\n");
	print("                         of every line in a call, the\n");
	print("                         near-end room included\n");
	print("  calltrace_status       Trace state, readiness, and archive\n");
	print("  calltrace_cancel       Cancel the active trace\n");
	print("  calltrace_wait [--id ID] [--deadline S]\n");
	print("                         Wait for an archive; deadline exits 2\n");
	print("  Each call trace command accepts --json for the last raw reply.\n");
	print("\nStub commands (voice.stub, stub HAL builds only):\n");
	print("  stub_dump              Per-line and per-stream state the stub recorded\n");
	print("\n  help                 This text\n");
	print("\nA jb, lec or monitor row keeps its place for the life of the\n");
	print("daemon. Sample says what its numbers are:\n");
	print("  live       the reading of this frame\n");
	print("  last_call  the reading kept from the last call\n");
	print("  never      the pair or the line never answered\n");
	print("A monitor row uses three words of its own, because the stack\n");
	print("samples the line on its own cadence and every reading names its\n");
	print("instant:\n");
	print("  sampled    a reading stands\n");
	print("  stale      the last read answered no data, so the values are\n");
	print("             the previous reading\n");
	print("  never      no reading landed yet\n");
	print("The canceller block of line prints three rows: uci is what the\n");
	print("operator asked for, pushed is what the daemon sent to the DSP at the\n");
	print("last seize of the line, engine is what the canceller measured.\n");
	print("The uvoice.sh helpers add refreshing views of five of these\n");
	print("commands: uvoice.calls.watch, uvoice.jb.watch, uvoice.lec.watch,\n");
	print("uvoice.monitor.watch and uvoice.line.watch N.\n");
	print("\nLines and slots are 0-based on input, matching the ubus surface.\n");
	print("Environment: UVOICE_UBUS_SOCKET overrides the ubus socket path.\n");
	print("UVOICE_LINE_BASE=1 prints every line number 1-based, the way the\n");
	print("klish CLI counts (voice-cli sets it). UVOICE_HINT_STYLE=klish names\n");
	print("the klish verbs in the footers instead of the uvoice.sh helpers.\n");
	print("UVOICE_COLOR=1 turns the color layer on (uvoice.sh sets it on a\n");
	print("tty); NO_COLOR or UVOICE_COLOR=0 keeps the output plain.\n");
}

let commands = {
	status: cmd_status,
	lines: cmd_lines,
	line: cmd_line,
	calls: cmd_calls,
	streams: cmd_streams,
	jb: cmd_jb,
	lec: cmd_lec,
	monitor: cmd_monitor,
	cdr: cmd_cdr,
	stats: cmd_stats,
	stats_live: cmd_stats_live,
	stats_reset: cmd_stats_reset,
	config: cmd_config,
	linetest: cmd_linetest,
	looptest: cmd_looptest,
	trace: cmd_trace,
	trace_set: cmd_trace_set,
	calltrace_arm: cmd_calltrace_arm,
	calltrace_status: cmd_calltrace_status,
	calltrace_cancel: cmd_calltrace_cancel,
	calltrace_wait: cmd_calltrace_wait,
	register: cmd_register,
	unregister: cmd_unregister,
	seize: cmd_seize,
	release: cmd_release,
	dial: cmd_dial,
	reload: cmd_reload,
	stub_dump: cmd_stub_dump,
	help: function(args) { usage(); return 0; }
};

function parse_args(argv) {
	let result = {};
	let i = 0;

	while (i < length(argv)) {
		let arg = argv[i];

		if (substr(arg, 0, 2) == "--") {
			let key = substr(arg, 2);
			if (i + 1 < length(argv) && substr(argv[i + 1], 0, 1) != "-") {
				result[key] = argv[i + 1];
				i += 2;
			} else {
				result[key] = true;
				i++;
			}
		} else {
			i++;
		}
	}

	return result;
}

// uvoice.sh invokes this script behind a --. A permuting getopt (glibc
// hosts) consumes it; a POSIX getopt (musl targets) stops at the script
// path and the -- lands in ARGV, so drop it here.
let argv = ARGV;
if (argv[0] == "--")
	argv = slice(argv, 1);

if (length(argv) < 1) {
	usage();
	exit(1);
}

let cmd = argv[0];
let args = parse_args(slice(argv, 1));

if (!(cmd in commands)) {
	printf("Unknown command: %s\n\n", cmd);
	usage();
	exit(1);
}

exit(commands[cmd](args));
