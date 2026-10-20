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
// calltrace-summary.uc: the plain text report of one call trace archive.
//
// The finisher runs it on the unit and copies it into the archive under
// tools/, so a host builds the same report again from the same files. Every
// number comes from a file of the archive. A missing or unreadable input
// prints one line and the report goes on.
//
// The walk of rings.pcapng keeps only what the report prints: a log or SIP
// line, one record per media flow, one record per run of sweep lines, and a
// count of everything else.
//
// Usage: ucode calltrace-summary.uc -- <dir> [- | --full]
//   -       write the report to stdout instead of <dir>/SUMMARY.txt
//   --full  print every media and jitter buffer sweep line, walk every media
//           entry, and lift the line bound of the timeline
//
// ucode reads an option of its own before the script path, so an argument
// that starts with two dashes needs the separator in front of it.
//
// UVOICE_SUMMARY_RTP_MAX moves the media walk bound, which a test uses to
// reach the bound with a handful of entries.
//
// Exit: 0 on a report, 1 when the directory holds no readable manifest.json,
// 2 when SUMMARY.txt cannot be written.

'use strict';

import * as fs from 'fs';
import * as struct from 'struct';

const REPORT_WIDTH = 120;
const KEY_WIDTH = 22;
// The timeline of a long trace runs to more lines than a reader uses, and
// the walk keeps no line the report cannot print, so both stop here.
const TIMELINE_MAX = 5000;
const SIP_LINE_MAX = 256;
// A media ring this deep costs more time than the unit's finisher has. The
// walk counts what it reads, because a manifest of an interrupted trace can
// name another number; past the bound it steps over the media entries and
// the report says so. UVOICE_SUMMARY_RTP_MAX moves the bound for a test.
const RTP_WALK_MAX = 100000;
const TCPDUMP = "/usr/bin/tcpdump";

// The blocks of the export (src/pcapng.c): a section header, one interface
// per ring, and one enhanced packet block per entry.
const PCAPNG_SHB = 0x0a0d0d0a;
const PCAPNG_IDB = 1;
const PCAPNG_EPB = 6;
const PCAPNG_BOM = 0x1a2b3c4d;
const PCAPNG_SHB_MIN = 28;
const PCAPNG_IDB_MIN = 20;
const PCAPNG_EPB_MIN = 32;
const PCAPNG_OPT_IF_NAME = 2;
const PCAPNG_OPT_IF_TSRESOL = 9;

// The kinds of the DSCP byte of a synthesized header (src/trace_ring.h).
const PKT_KINDS = [ "SIP/UDP", "SIP/TCP", "SIP/TLS", "RTP", "RTCP", "UDPTL",
                    "MGCP" ];

// The upper edges of the engine tick histogram (udsp.h udsp_engine_diag).
const ENGINE_HIST_EDGES = [ "<=9ms", "<=11ms", "<=12ms", "<=15ms", "<=20ms",
                            "<=30ms", "<=50ms", ">50ms" ];

// The counters of one diag stream row, in the order the daemon writes them.
const DIAG_STREAM_KEYS = [ "net_drops", "net_stale", "net_trunc", "tx_fail",
                           "punch_sent", "srtp_protect_ok",
                           "srtp_protect_fail", "srtp_unprotect_ok",
                           "srtp_unprotect_auth_fail",
                           "srtp_unprotect_replay" ];

// The HAL counters the report compares between the first and the last
// snapshot. events_dropped comes from the events table, the other five from
// the hal table.
const HAL_COUNTER_KEYS = [ "holds", "capture_drops", "trim_drops",
                           "playback_expired", "events_dropped",
                           "completions_lost" ];

const LOG_DOMAIN_RE = regexp("^\\[([a-z]+)\\]");
const SWEEP_DOMAINS = [ "media", "jb" ];
const RELEASE_RE = regexp("^DISTRIB_([A-Z]+)='?([^']*)'?$");
const PORTRANGE_RE = regexp("portrange ([0-9]+)-([0-9]+)");
const PORT_RE = regexp("port ([0-9]+)");

// tcpdump prints one header line per datagram. IPv6 reads IP6 in the same
// place, and the text after "SIP" is absent for a payload it cannot read.
const TCPDUMP_SIP_RE = regexp(
	"^([0-9-]+ [0-9:.]+) IP6? ([^ ]+)\\.([0-9]+) > ([^ ]+)\\.([0-9]+): SIP(.*)$");
const TCPDUMP_UDP_RE = regexp(
	"^([0-9-]+ [0-9:.]+) IP6? ([^ ]+)\\.([0-9]+) > ([^ ]+)\\.([0-9]+): " +
	"UDP, length ([0-9]+)$");

let report = [];
let arc = {};

function report_line_add(text) {
	push(report, text ?? "");
}

function pad_right(text, width) {
	let s = sprintf("%s", text ?? "");
	while (length(s) < width)
		s += " ";
	return s;
}

// A long value runs on under its own key, so the report keeps its width
// without losing a filter or a note.
function report_wrap_add(indent, cont, text) {
	let rest = sprintf("%s", text ?? "");
	let lead = indent;

	while (length(lead) + length(rest) > REPORT_WIDTH) {
		let room = REPORT_WIDTH - length(lead);
		let cut = -1;

		for (let i = room; i > 0; i--) {
			if (substr(rest, i, 1) == " ") {
				cut = i;
				break;
			}
		}
		if (cut <= 0)
			cut = room;
		report_line_add(lead + rtrim(substr(rest, 0, cut)));
		rest = ltrim(substr(rest, cut));
		lead = cont;
		if (rest == "")
			return;
	}

	report_line_add(lead + rest);
}

function pair_print(key, value) {
	report_wrap_add("  " + pad_right(key, KEY_WIDTH) + " ",
	                "  " + pad_right("", KEY_WIDTH) + " ",
	                value ?? "-");
}

// An input the archive does not carry reads as one line of its own, so a
// reader sees what the report could not use.
function note_print(text) {
	report_wrap_add("  ", "    ", text);
}

function plural(count, word, many) {
	return sprintf("%d %s", count, count == 1 ? word : (many ?? word + "s"));
}

function dash(value) {
	if (value == null)
		return "-";
	return sprintf("%s", value);
}

function yn(value) {
	return value ? "yes" : "no";
}

// The archive size rule of uvoice-cli.uc. The two files share no code: this
// one runs from the archive with the fs and struct modules alone.
function bytes_text(bytes) {
	if (bytes == null)
		return "-";
	if (bytes < 1024)
		return sprintf("%d B", bytes);
	if (bytes < 1024 * 1024)
		return sprintf("%.2f KB", bytes / 1024.0);
	if (bytes < 1024 * 1024 * 1024)
		return sprintf("%.2f MB", bytes / (1024.0 * 1024.0));
	return sprintf("%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
}

function duration_text(seconds) {
	if (seconds == null)
		return "-";
	if (seconds < 60)
		return sprintf("%ds", seconds);
	if (seconds < 3600)
		return sprintf("%dm %ds", seconds / 60, seconds % 60);
	if (seconds < 86400)
		return sprintf("%dh %dm", seconds / 3600, (seconds % 3600) / 60);
	return sprintf("%dd %dh", seconds / 86400, (seconds % 86400) / 3600);
}

function duration_ms_text(ms) {
	if (ms == null)
		return "-";
	if (ms < 0)
		return "-";
	if (ms < 60000)
		return sprintf("%d.%03ds", ms / 1000, ms % 1000);
	return duration_text(ms / 1000);
}

// localtime() carries no offset, so the report derives the one date +%z
// prints: the same broken-down time read back as UTC.
function zone_text(ts) {
	let broken = localtime(ts);
	if (!broken)
		return "";
	let off = timegm(broken) - ts;
	let mag = off < 0 ? -off : off;

	return sprintf("%s%02d%02d", off < 0 ? "-" : "+", mag / 3600,
	               (mag % 3600) / 60);
}

function wall_text(ts) {
	if (!ts)
		return "-";
	let d = localtime(ts);
	if (!d)
		return "-";
	return sprintf("%04d-%02d-%02d %02d:%02d:%02d %s", d.year, d.mon, d.mday,
	               d.hour, d.min, d.sec, zone_text(ts));
}

function date_text(ts) {
	let d = localtime(ts);
	if (!d)
		return "-";
	return sprintf("%04d-%02d-%02d", d.year, d.mon, d.mday);
}

function clock_ms_text(ms) {
	if (ms == null)
		return "-";
	let d = localtime(ms / 1000);
	if (!d)
		return "-";
	return sprintf("%02d:%02d:%02d.%03d", d.hour, d.min, d.sec, ms % 1000);
}

function clock_us_text(us) {
	return clock_ms_text(us / 1000);
}

// An IPv6 address takes brackets in front of a port, the rule uvoice-cli.uc
// prints an endpoint by.
function host_port_text(host, port) {
	if (index(host, ":") >= 0)
		return sprintf("[%s]:%d", host, port);
	return sprintf("%s:%d", host, port);
}

function shell_quote(text) {
	return "'" + replace(text, "'", "'\\''") + "'";
}

function file_text_get(path) {
	return fs.readfile(path);
}

function file_json_get(path) {
	let text = file_text_get(path);
	if (text == null)
		return null;
	try {
		return json(text);
	}
	catch (e) {
		return null;
	}
}

function line_first_get(text) {
	if (text == null)
		return null;
	let cut = index(text, "\n");
	if (cut < 0)
		return rtrim(text, "\r");
	return rtrim(substr(text, 0, cut), "\r");
}

// The first line of a byte range of the export, copied alone. The scan stops
// at the cap, so a long payload costs the reader nothing past its first line.
function line_range_get(data, off, len, cap) {
	let stop = off + min(len, cap ?? SIP_LINE_MAX);
	let end = off;

	while (end < stop) {
		let c = ord(data, end);

		if (c == 10 || c == 13)
			break;
		end++;
	}

	return substr(data, off, end - off);
}

// The command's output goes to the caller one line at a time, so a capture
// of any size costs one line of memory. The return is the exit status of the
// command, or null when it could not start.
function command_lines_each(cmd, fn) {
	let pipe = fs.popen(cmd, "r");

	if (!pipe)
		return null;

	let line;

	while ((line = pipe.read("line")) != null)
		fn(rtrim(line, "\r\n"));

	return pipe.close();
}

// --- the archive --------------------------------------------------------

// Without jq the finisher leaves its results in manifest-finisher.json and
// the journal keeps the daemon's last stage, the rule uvoice-cli.uc reads a
// manifest by.
function manifest_get(dir) {
	let manifest = file_json_get(dir + "/manifest.json");

	if (manifest == null)
		return null;
	if (manifest.collectors != null)
		return manifest;

	let extra = file_json_get(dir + "/manifest-finisher.json");
	if (extra == null)
		return manifest;

	return { ...manifest, ...extra,
		notes: [ ...(manifest.notes ?? []), ...(extra.notes ?? []) ],
		contains: uniq([ ...(manifest.contains ?? []),
		                 ...(extra.contains ?? []) ]) };
}

function snapshots_get(dir) {
	let res = { present: false, rows: [], bad: 0 };
	let text = file_text_get(dir + "/snapshots.jsonl");

	if (text == null)
		return res;

	res.present = true;
	for (let line in split(text, "\n")) {
		if (trim(line) == "")
			continue;
		try {
			push(res.rows, json(line));
		}
		catch (e) {
			res.bad++;
		}
	}

	return res;
}

// --- the entries of the rings -------------------------------------------

function addr_text(data, off, count) {
	let raw = [];

	for (let i = 0; i < count; i++)
		push(raw, ord(data, off + i));

	return arrtoip(raw) ?? "?";
}

// One entry of the sip or the rtp ring, read where it lies: the synthesized
// IP and UDP headers of src/trace_ring.c, then the payload. The reader takes
// the offset and the length, so no entry is copied to be decoded.
function pkt_decode(data, off, len) {
	if (len < 1)
		return null;

	let version = ord(data, off) >> 4;
	let tos;
	let hlen;
	let src;
	let dst;

	if (version == 4) {
		hlen = (ord(data, off) & 0x0f) * 4;
		if (hlen < 20 || len < hlen + 8)
			return null;
		tos = ord(data, off + 1);
		src = addr_text(data, off + 12, 4);
		dst = addr_text(data, off + 16, 4);
	}
	else if (version == 6) {
		hlen = 40;
		if (len < hlen + 8)
			return null;
		tos = ((ord(data, off) & 0x0f) << 4) | (ord(data, off + 1) >> 4);
		src = addr_text(data, off + 8, 16);
		dst = addr_text(data, off + 24, 16);
	}
	else {
		return null;
	}

	return {
		rx: ((tos >> 6) & 0x3) == 1,
		kind: (tos >> 2) & 0x0f,
		src: src,
		sport: (ord(data, off + hlen) << 8) | ord(data, off + hlen + 1),
		dst: dst,
		dport: (ord(data, off + hlen + 2) << 8) | ord(data, off + hlen + 3),
		payload_off: off + hlen + 8,
		payload_len: len - hlen - 8
	};
}

function kind_text(kind) {
	return PKT_KINDS[kind] ?? sprintf("kind %d", kind);
}

function peer_text(pkt) {
	if (pkt.rx)
		return host_port_text(pkt.src, pkt.sport);
	return host_port_text(pkt.dst, pkt.dport);
}

function flow_key_text(pkt) {
	return sprintf("%s %s %s > %s", pkt.rx ? "rx" : "tx", kind_text(pkt.kind),
	               host_port_text(pkt.src, pkt.sport),
	               host_port_text(pkt.dst, pkt.dport));
}

// The bytes that name a flow: the DSCP byte, the two addresses and the two
// ports, as they lie. A media ring repeats a handful of flows over hundreds
// of thousands of entries, so the raw bytes find the flow record without a
// decode. A header with IPv4 options takes the slow path.
function flow_sig(data, off, len) {
	let first = ord(data, off);

	if (first == 0x45 && len >= 28)
		return substr(data, off + 1, 1) + substr(data, off + 12, 12);
	if ((first >> 4) == 6 && len >= 48)
		return substr(data, off, 2) + substr(data, off + 8, 36);
	return null;
}

// The domain of a log entry, the word in brackets the sink writes in front
// of every line (src/trace_ring.c trace_ring_log). The match reads the head
// of the line alone, because a sweep-heavy ring runs to hundreds of
// thousands of lines and each one pays this.
function log_domain_at(data, off, len) {
	let m = match(substr(data, off, min(len, 16)), LOG_DOMAIN_RE);

	return m ? m[1] : "";
}

// --- the walk of rings.pcapng -------------------------------------------

function u16_get(data, off, le) {
	return struct.unpack(le ? "<H" : "!H", data, off)[0];
}

function u32_get(data, off, le) {
	return struct.unpack(le ? "<I" : "!I", data, off)[0];
}

// A block the walk cannot use is skipped and named once, with the offset of
// the first one and the number of them.
function skip_note_add(res, text, off) {
	let note = res.skips[text];

	if (note == null) {
		note = { count: 0, off: off };
		res.skips[text] = note;
		push(res.skip_order, text);
	}
	note.count++;
}

// if_tsresol is a negative power of ten, or of two when bit 7 is set. The
// daemon writes 6, and a binary resolution is not scaled here: the stamps
// stay raw and the report says so.
function stamp_us_get(raw, tsresol) {
	if (tsresol == 6 || (tsresol & 0x80))
		return raw;

	let steps = 6 - tsresol;
	let value = raw;

	while (steps > 0) {
		value *= 10;
		steps--;
	}
	while (steps < 0) {
		value /= 10;
		steps++;
	}

	return value;
}

function idb_read(res, data, off, blen, le) {
	let name = sprintf("iface%d", length(res.names));
	let tsresol = 6;
	let opt = off + 16;
	let last = off + blen - 4;

	while (opt + 4 <= last) {
		let code = u16_get(data, opt, le);
		let olen = u16_get(data, opt + 2, le);

		if (code == 0)
			break;
		if (opt + 4 + olen > last)
			break;
		if (code == PCAPNG_OPT_IF_NAME && olen > 0)
			name = rtrim(substr(data, opt + 4, olen), "\x00");
		if (code == PCAPNG_OPT_IF_TSRESOL && olen > 0)
			tsresol = ord(data, opt + 4);
		opt += 4 + ((olen + 3) & -4);
	}

	if (tsresol & 0x80)
		skip_note_add(res,
		              sprintf("unsupported time resolution on %s, " +
		                      "stamps unscaled", name), off);

	push(res.names, name);
	push(res.scales, tsresol);
}

// A run of sweep lines folds while the walk reads it, so a ring of millions
// of them costs one record per run. A non-sweep log line or a SIP entry ends
// the run; a snapshot marker merges later and can print after a run whose
// span covers it, because the walk cannot see the snapshots.
function sweep_run_add(res, us, domain) {
	let run = res.run;

	if (run == null) {
		if (res.log_kept >= TIMELINE_MAX)
			return;
		run = { us: us, seq: res.seq++, sweep: true, count: 0, last: us,
		        domains: {}, order: [] };
		res.run = run;
		push(res.events, run);
	}

	run.count++;
	run.last = us;
	if (run.domains[domain] == null) {
		run.domains[domain] = true;
		push(run.order, domain);
	}
}

// A log entry is one line and the timeline prints it whole. Past the bound
// the walk counts the line and keeps nothing, because the report cannot
// print it.
function log_entry_take(res, us, data, off, len) {
	let domain = log_domain_at(data, off, len);

	res.domains[domain] = (res.domains[domain] ?? 0) + 1;
	if (!arc.full && index(SWEEP_DOMAINS, domain) >= 0) {
		sweep_run_add(res, us, domain);
		return;
	}

	res.run = null;
	res.log_lines++;
	if (!arc.full && res.log_kept >= TIMELINE_MAX)
		return;

	res.log_kept++;
	push(res.events, { us: us, seq: res.seq++, domain: domain,
	                   text: rtrim(substr(data, off, len), "\r\n") });
}

// The SIP section prints every entry, so the walk keeps the fields both
// ladders need and drops the message.
function sip_entry_take(res, us, data, off, len) {
	let pkt = pkt_decode(data, off, len);

	res.run = null;
	if (!pkt) {
		res.sip_bad++;
		push(res.events, { us: us, seq: res.seq++, sip: true, bad: true });
		return;
	}

	if (pkt.rx)
		res.sip_rx++;
	else {
		res.sip_tx++;
		// A transmit entry names the unit's own address, which the wire
		// ladder needs and no shared port can tell.
		if (res.unit == null)
			res.unit = pkt.src;
	}

	push(res.events, { us: us, seq: res.seq++, sip: true, rx: pkt.rx,
	                   peer: peer_text(pkt), kind: kind_text(pkt.kind),
	                   head: line_range_get(data, pkt.payload_off,
	                                        pkt.payload_len) });
}

// A media entry lands in its flow and goes. The report prints the flow, so
// the flow is what the walk keeps.
function rtp_entry_take(res, us, data, off, len) {
	let sig = flow_sig(data, off, len);
	let rec = sig != null ? res.sigs[sig] : null;

	if (rec == null) {
		let pkt = pkt_decode(data, off, len);

		if (!pkt) {
			res.rtp_bad++;
			return;
		}

		let key = flow_key_text(pkt);

		rec = res.flows[key];
		if (rec == null) {
			rec = { count: 0, first: us, last: us };
			res.flows[key] = rec;
			push(res.flow_order, key);
		}
		if (sig != null)
			res.sigs[sig] = rec;
	}

	rec.count++;
	rec.last = us;
}

function entry_take(res, ring, us, data, off, len) {
	res.counts[ring] = (res.counts[ring] ?? 0) + 1;
	if (ring == "uvoiced_log") {
		log_entry_take(res, us, data, off, len);
		return;
	}
	if (ring == "uvoiced_sip") {
		sip_entry_take(res, us, data, off, len);
		return;
	}
	if (ring != "uvoiced_rtp")
		return;

	// The walk steps over the media entries past the bound and reads the
	// blocks after them, so a log or SIP entry behind a deep media ring
	// still reaches the timeline.
	if (res.rtp_max && res.rtp_seen >= res.rtp_max) {
		res.rtp_capped = true;
		return;
	}

	res.rtp_seen++;
	rtp_entry_take(res, us, data, off, len);
}

function rtp_max_get() {
	let text = getenv("UVOICE_SUMMARY_RTP_MAX");
	let value = text != null ? int(text) : 0;

	return value > 0 ? value : RTP_WALK_MAX;
}

// The walk stops at a block whose length it cannot trust and the report
// names the offset, so a cut file still yields every entry before the cut. A
// block it can step over instead is skipped and counted.
function rings_read(path) {
	let res = { present: false, error: null, skips: {}, skip_order: [],
	            events: [], counts: {}, domains: {}, flows: {},
	            flow_order: [], sigs: {}, names: [], scales: [], seq: 0,
	            log_lines: 0, log_kept: 0, sip_tx: 0, sip_rx: 0, sip_bad: 0,
	            rtp_bad: 0, rtp_seen: 0, rtp_capped: false, run: null,
	            unit: null };
	let data = file_text_get(path);

	if (data == null)
		return res;

	res.present = true;
	res.rtp_max = arc.full ? 0 : rtp_max_get();

	let size = length(data);
	let le = true;
	let off = 0;

	try {
		while (off + 12 <= size) {
			// One read of the type and the length: a media ring runs to
			// hundreds of thousands of blocks and each call costs.
			let head = struct.unpack(le ? "<II" : "!II", data, off);
			let btype = head[0];

			// The section header block reads the same in either byte
			// order, so its magic sets the order of everything after it.
			if (btype == PCAPNG_SHB) {
				le = u32_get(data, off + 8, true) == PCAPNG_BOM;
				head = struct.unpack(le ? "<II" : "!II", data, off);
			}

			let blen = head[1];

			if (blen < 12 || (blen % 4) != 0 || off + blen > size) {
				res.error = sprintf("block at offset %d is cut or malformed",
				                    off);
				break;
			}
			if (u32_get(data, off + blen - 4, le) != blen) {
				res.error = sprintf("block at offset %d carries two lengths",
				                    off);
				break;
			}
			if (btype == PCAPNG_SHB) {
				if (blen < PCAPNG_SHB_MIN)
					skip_note_add(res, "a section header block is too short",
					              off);
				else {
					res.names = [];
					res.scales = [];
				}
			}
			else if (btype == PCAPNG_IDB) {
				if (blen < PCAPNG_IDB_MIN)
					skip_note_add(res, "an interface block is too short", off);
				else
					idb_read(res, data, off, blen, le);
			}
			else if (btype == PCAPNG_EPB) {
				if (blen < PCAPNG_EPB_MIN) {
					skip_note_add(res, "a packet block is too short", off);
					off += blen;
					continue;
				}

				let epb = struct.unpack(le ? "<IIII" : "!IIII", data,
				                        off + 8);
				let iface = epb[0];
				let caplen = epb[3];

				if (28 + caplen > blen - 4) {
					res.error = sprintf("packet at offset %d runs past its block",
					                    off);
					break;
				}
				if (res.names[iface] == null) {
					skip_note_add(res, "a packet names an interface with no description",
					              off);
					off += blen;
					continue;
				}
				entry_take(res, res.names[iface],
				           stamp_us_get(epb[1] * 4294967296 + epb[2],
				                        res.scales[iface] ?? 6),
				           data, off + 28, caplen);
			}

			off += blen;
		}
	}
	catch (e) {
		res.error = sprintf("block at offset %d could not be read", off);
	}

	if (res.error == null && off != size)
		res.error = sprintf("the file ends inside a block at offset %d", off);

	return res;
}

// --- what the archive says about itself ---------------------------------

function channel_get() {
	let line = arc.manifest.line;

	if (line == null)
		return {};

	for (let ch in (arc.config?.channel ?? []))
		if (ch.id == line)
			return ch;

	return {};
}

function wire_filter_text() {
	return arc.manifest.counts?.wire?.filter ?? "";
}

// The signaling protocol of the trace. The daemon names the signaling ring
// after the protocol that runs (src/calltrace.c ct_ring_name()), so the ring
// rows of the manifest record it; config.json records what was asked.
function signaling_get() {
	let rings = arc.manifest.counts?.rings;

	if (rings?.mgcp != null)
		return "mgcp";
	if (rings?.sip != null)
		return "sip";
	return arc.config?.protocol == "mgcp" ? "mgcp" : "sip";
}

// The configuration names the ports. Without it the capture filter of the
// manifest is the only other record of them, and neither one guesses.
function signaling_port_get() {
	let ch = channel_get();
	let port = arc.proto == "mgcp" ? arc.config?.mgcp?.local_port
	                               : ch.sip_local_port ?? ch.sip_port;

	if (port != null)
		return port;

	let m = match(wire_filter_text(), PORT_RE);

	return m ? int(m[1]) : null;
}

function rtp_range_get() {
	let ch = channel_get();
	let lo = ch.rtp_local_port_min ?? 0;
	let hi = ch.rtp_local_port_max ?? 0;

	if (lo > 0 && hi >= lo && hi <= 65535)
		return [ lo, hi ];

	let m = match(wire_filter_text(), PORTRANGE_RE);

	return m ? [ int(m[1]), int(m[2]) ] : null;
}

// The address of the interface the capture ran on, for a trace whose SIP
// ring holds no transmit entry.
function iface_addr_get() {
	let iface = arc.manifest.counts?.wire?.iface;

	if (iface == null)
		return null;

	let rows = file_json_get(arc.dir + "/system/ip-addr.json");

	if (type(rows) != "array")
		return null;

	for (let row in rows) {
		if (row.ifname != iface)
			continue;
		for (let addr in (row.addr_info ?? []))
			if (addr.family == "inet" && addr.local != null)
				return addr.local;
		for (let addr in (row.addr_info ?? []))
			if (addr.local != null)
				return addr.local;
	}

	return null;
}

function unit_addr_get() {
	return arc.rings.unit ?? iface_addr_get();
}

// --- the sections -------------------------------------------------------

function firmware_text() {
	let text = file_text_get(arc.dir + "/system/platform");

	if (text == null)
		return null;

	let id;
	let release;
	let revision;

	for (let line in split(text, "\n")) {
		let m = match(line, RELEASE_RE);
		if (!m)
			continue;
		if (m[1] == "ID")
			id = m[2];
		if (m[1] == "RELEASE")
			release = m[2];
		if (m[1] == "REVISION")
			revision = m[2];
	}

	if (id == null && release == null)
		return null;

	return trim(sprintf("%s %s%s", id ?? "", release ?? "",
	                    revision != null ? " (" + revision + ")" : ""));
}

function line_text() {
	let line = arc.manifest.line;

	if (line == null)
		return "-";
	return sprintf("line %d (index %d)", line + 1, line);
}

function trace_section() {
	let m = arc.manifest;
	let board = m.board ?? {};
	let opt = m.options ?? {};

	report_line_add("TRACE");
	pair_print("id", dash(m.id));
	pair_print("line", line_text());
	pair_print("stage", dash(m.stage));
	pair_print("reason", dash(m.reason));
	pair_print("board", sprintf("%s, serial %s", dash(board.model),
	                            dash(board.serial)));
	pair_print("firmware", dash(firmware_text()));
	pair_print("uvoice", dash(m.versions?.uvoice));
	pair_print("schema", dash(m.versions?.schema));
	pair_print("md5", dash(m.md5));
	pair_print("boot id", dash(m.boot_id));
	pair_print("options", sprintf(
		"timeout %s s, grace %s s, arm timeout %s s, wire %s, " +
		"wire cap %s MB, rings cap %s MB",
		dash(opt.timeout_s), dash(opt.grace_s), dash(opt.arm_timeout_s),
		opt.wire == null ? "-" : (opt.wire ? "on" : "off"),
		dash(opt.wire_max_mb), dash(opt.rings_max_mb)));
}

function span_text(from, to) {
	if (from == null || to == null)
		return "-";
	return duration_ms_text((to - from) * 1000);
}

function clock_pair_text(snap) {
	if (!snap || !snap.clock)
		return "-";
	return sprintf("realtime %s, monotonic %s ms",
	               clock_us_text(snap.clock.realtime_us),
	               snap.clock.monotonic_ns / 1000000);
}

function times_section() {
	let t = arc.manifest.times ?? {};
	let rows = arc.snaps.rows;
	let first = length(rows) ? rows[0] : null;
	let last = length(rows) ? rows[length(rows) - 1] : null;
	let stepped = false;

	for (let snap in rows)
		if (snap.clock?.step_detected)
			stepped = true;

	report_line_add("TIMES");
	pair_print("armed", wall_text(t.armed_at));
	pair_print("capture ready", wall_text(t.capture_ready_at));
	pair_print("started", wall_text(t.started_at));
	pair_print("ended", wall_text(t.ended_at));
	pair_print("finished", wall_text(t.finished_at));
	pair_print("armed for", span_text(t.armed_at, t.started_at));
	pair_print("window", span_text(t.started_at, t.ended_at));
	pair_print("first snapshot", clock_pair_text(first));
	pair_print("last snapshot", clock_pair_text(last));
	pair_print("clock step", length(rows) ? yn(stepped) : "-");
}

function timeline_events_get() {
	let events = [ ...arc.rings.events ];
	let seq = arc.rings.seq;

	for (let snap in arc.snaps.rows) {
		let reasons = join(", ", snap.reasons ?? []);

		push(events, { us: (snap.t_wall_ms ?? 0) * 1000, seq: seq++,
		               domain: "snapshot",
		               text: rtrim(sprintf("snapshot %s %s %s",
		                                   dash(snap.kind),
		                                   dash(snap.fsm_state), reasons)) });
	}

	return sort(events, (a, b) => (a.us - b.us) || (a.seq - b.seq));
}

// A SIP entry keeps its fields instead of its line, so the two ladders and
// the peer counts read the same record.
function event_text(ev) {
	// The domains of a run print in one order whatever order they arrived
	// in, so two traces of the same shape read the same.
	if (ev.sweep)
		return sprintf("%s sweep: %s from %s to %s", join("/", sort(ev.order)),
		               plural(ev.count, "line"), clock_us_text(ev.us),
		               clock_us_text(ev.last));
	if (!ev.sip)
		return ev.text;
	if (ev.bad)
		return "SIP entry the report cannot decode";
	// the signaling ring of an MGCP daemon carries MGCP messages
	return sprintf("%s %s %s %s", ev.kind == "MGCP" ? "MGCP" : "SIP",
	               ev.rx ? "<-" : "->", ev.peer,
	               ev.head == "" || ev.head == null
	               ? "(keepalive or fragment)" : ev.head);
}

function timeline_counts_text() {
	let parts = [];
	let order = [];

	for (let domain in arc.rings.domains)
		push(order, domain);

	for (let domain in sort(order))
		push(parts, sprintf("%s %d", domain == "" ? "other" : domain,
		                    arc.rings.domains[domain]));

	return sprintf("log ring %s; %s ring %d; snapshots %d",
	               length(parts) ? join(", ", parts) : "none", arc.proto,
	               arc.rings.counts.uvoiced_sip ?? 0,
	               length(arc.snaps.rows));
}

// Every line the timeline can hold, whatever the walk kept of them.
function timeline_total() {
	return arc.rings.log_lines + (arc.rings.counts.uvoiced_sip ?? 0) +
	       length(arc.snaps.rows);
}

function rings_notes_print() {
	if (!arc.rings.present) {
		note_print("rings.pcapng: absent, the timeline holds no ring entry");
		return;
	}
	if (arc.rings.error != null)
		note_print("rings.pcapng: " + arc.rings.error);
	for (let text in arc.rings.skip_order) {
		let note = arc.rings.skips[text];

		note_print(sprintf("rings.pcapng: %s, first at offset %d%s", text,
		                   note.off,
		                   note.count > 1 ? sprintf(", %d blocks", note.count)
		                                  : ""));
	}
}

function timeline_section() {
	let events = timeline_events_get();

	report_line_add("TIMELINE");
	rings_notes_print();
	if (!arc.snaps.present)
		note_print("snapshots.jsonl: absent, the timeline holds no snapshot");
	if (arc.snaps.bad)
		note_print(sprintf("snapshots.jsonl: %s are not JSON",
		                   plural(arc.snaps.bad, "line")));
	if (length(events) == 0) {
		pair_print("events", "none");
		return;
	}

	pair_print("date", sprintf("%s, local time %s",
	                           date_text(events[0].us / 1000000),
	                           zone_text(events[0].us / 1000000)));
	pair_print("lines", timeline_counts_text());
	if (!arc.full)
		pair_print("folded", "the media and jb sweep lines, one line per run " +
		                     "(--full prints them)");

	let shown = 0;

	for (let ev in events) {
		if (!ev.sweep) {
			if (!arc.full && shown >= TIMELINE_MAX) {
				note_print(sprintf("timeline cut at %d lines of %d; --full prints them all",
				                   shown, timeline_total()));
				return;
			}
			shown++;
		}
		report_wrap_add("  " + clock_us_text(ev.us) + "  ",
		                "  " + pad_right("", 12) + "  ", event_text(ev));
	}
}

function media_row_print(media) {
	let rows = [
		[ "packets sent", dash(media.packets_sent) ],
		[ "jitter avg (ms)", dash(media.jitter_avg) ],
		[ "packets recv", dash(media.packets_recv) ],
		[ "jitter max (ms)", dash(media.jitter_max) ],
		[ "packets lost", dash(media.packets_lost) ],
		[ "rtt avg (ms)", dash(media.rtt_avg) ],
		[ "loss rate", dash(media.loss_rate) ],
		[ "rtt max (ms)", dash(media.rtt_max) ],
		[ "MOS LQ", dash(media.mos_lq) ],
		[ "MOS CQ", dash(media.mos_cq) ],
		[ "R-factor", dash(media.rfactor) ],
		[ "bytes sent", bytes_text(media.bytes_sent) ],
		[ "bytes recv", bytes_text(media.bytes_recv) ]
	];

	for (let i = 0; i < length(rows); i += 2) {
		let right = i + 1 < length(rows) ? rows[i + 1] : [ "", "" ];

		report_line_add(rtrim(sprintf("    %-18s %-12s %-18s %s", rows[i][0],
		                              rows[i][1], right[0], right[1])));
	}
}

function jb_row_print(jb) {
	report_line_add(sprintf("    %-18s %s", "jb mode", dash(jb.mode)));
	report_line_add(rtrim(sprintf("    %-18s %-12s %-18s %s",
	                              "jb depth (ms)", dash(jb.depth_ms),
	                              "jb target (ms)", dash(jb.target_ms))));
	report_line_add(rtrim(sprintf("    %-18s %-12s %-18s %s",
	                              "jb underruns", dash(jb.underruns),
	                              "jb late drops", dash(jb.late_drop))));
	report_line_add(sprintf("    %-18s %s", "jb resyncs", dash(jb.resyncs)));
}

function calls_section() {
	let rows = arc.snaps.rows;
	let end = length(rows) ? rows[length(rows) - 1] : null;
	let records = end?.cdr ?? [];

	report_line_add("CALLS");
	if (length(records) == 0) {
		pair_print("records", "none (the end snapshot carries no call record)");
		return;
	}

	// A record the freeze caught before its close keeps duration_ms 0, so
	// the line says the record is open instead of leaving a reader to
	// doubt the zero.
	for (let rec in records) {
		pair_print(sprintf("record seq %s", dash(rec.seq)),
		           sprintf("%s, start %s, answered %s, duration %s, codec %s%s",
		                   rec.inbound ? "inbound" : "outbound",
		                   wall_text(rec.start), yn(rec.answered),
		                   duration_ms_text(rec.duration_ms), dash(rec.codec),
		                   rec.in_progress ? ", still open at the freeze" : ""));
		if (rec.media_state != null)
			pair_print("media state", rec.media_state);
		if (rec.media_state != null && rec.media)
			media_row_print(rec.media);
		if (rec.jb)
			jb_row_print(rec.jb);
	}
}

function wire_present() {
	return fs.stat(arc.dir + "/wire.pcap") != null;
}

function tcpdump_present() {
	return fs.access(TCPDUMP, "x") == true;
}

// The three reasons a wire summary cannot run, each as its own line. The
// answer is the reason, or null when tcpdump can read the capture.
function wire_block_text() {
	if (!wire_present())
		return "wire.pcap: absent (wire off)";
	if (!tcpdump_present())
		return "tcpdump: absent, no wire summary";
	return null;
}

function tcpdump_each(filter, fn) {
	let cmd = sprintf("%s -r %s -n -tttt %s 2>/dev/null", TCPDUMP,
	                  shell_quote(arc.dir + "/wire.pcap"),
	                  shell_quote(filter));

	try {
		return command_lines_each(cmd, fn);
	}
	catch (e) {
		return null;
	}
}

function peer_count_text(counts) {
	let parts = [];

	for (let peer, n in counts)
		push(parts, sprintf("%s %d", peer, n));

	return length(parts) ? join(", ", parts) : "none";
}

function sip_ring_print() {
	let rows = arc.rings.counts.uvoiced_sip ?? 0;
	let counts = {};
	let kinds = {};

	if (!arc.rings.present) {
		note_print(sprintf("rings.pcapng: absent, no %s ring ladder",
		                   arc.proto));
		return;
	}

	pair_print(arc.proto + " ring",
	           sprintf("%s of rings.pcapng",
	                   plural(rows, "entry", "entries")));

	for (let ev in arc.rings.events) {
		if (!ev.sip || ev.bad)
			continue;

		counts[ev.peer] = (counts[ev.peer] ?? 0) + 1;
		kinds[ev.kind] = (kinds[ev.kind] ?? 0) + 1;
		report_wrap_add("  " + clock_us_text(ev.us) + "  ",
		                "  " + pad_right("", 12) + "  ",
		                sprintf("%s %-21s %s", ev.rx ? "<-" : "->", ev.peer,
		                        ev.head == "" || ev.head == null
		                        ? "(keepalive or fragment)" : ev.head));
	}

	if (rows) {
		pair_print("ring direction", sprintf("%d sent, %d received",
		                                     arc.rings.sip_tx,
		                                     arc.rings.sip_rx));
		if (arc.rings.sip_bad)
			pair_print("ring undecodable",
			           plural(arc.rings.sip_bad, "entry", "entries"));
		pair_print("ring transport", peer_count_text(kinds));
		pair_print("ring peers", peer_count_text(counts));
	}
	if (kinds["SIP/TCP"] != null || kinds["SIP/TLS"] != null)
		note_print("this account runs SIP over TCP or TLS: wire.pcap holds " +
		           "the ciphertext and the sip ring of rings.pcapng holds " +
		           "the messages");
}

// One header line of tcpdump in the groups of TCPDUMP_SIP_RE, the head last.
// tcpdump decodes SIP and prints its first line; an MGCP datagram reads as
// plain UDP, so its head is its length.
function wire_line_parse(line) {
	let mgcp = arc.proto == "mgcp";
	let m = match(line, mgcp ? TCPDUMP_UDP_RE : TCPDUMP_SIP_RE);

	if (!m)
		return null;

	m[6] = mgcp ? sprintf("MGCP, %s", plural(int(m[6]), "byte"))
	            : ltrim(m[6], ": ");

	return m;
}

function sip_wire_print() {
	let blocked = wire_block_text();

	if (blocked != null) {
		note_print(blocked);
		return;
	}

	let port = signaling_port_get();

	if (port == null) {
		note_print(sprintf("the %s port is unknown, no wire ladder",
		                   uc(arc.proto)));
		return;
	}

	let unit = unit_addr_get();
	let counts = {};
	let shown = 0;

	if (unit == null)
		note_print(sprintf("the unit address is unknown: the %s ring " +
		                   "holds no transmit entry and " +
		                   "system/ip-addr.json names no address for " +
		                   "the capture interface, so the ladder " +
		                   "shows no direction", arc.proto));
	else
		pair_print("unit address", host_port_text(unit, port));

	// Each line of the capture prints as it arrives, so a ladder of any
	// length costs one line of memory.
	let status = tcpdump_each(sprintf("udp port %d", port), (line) => {
		let m = wire_line_parse(line);

		if (!m)
			return;

		let out = unit != null && m[2] == unit;
		let peer = sprintf("%s:%s", m[2], m[3]);
		let head = m[6];

		if (unit == null)
			peer = sprintf("%s:%s > %s:%s", m[2], m[3], m[4], m[5]);
		else if (out)
			peer = sprintf("%s:%s", m[4], m[5]);
		counts[peer] = (counts[peer] ?? 0) + 1;
		shown++;
		report_wrap_add("  " + substr(m[1], 11, 12) + "  ",
		                "  " + pad_right("", 12) + "  ",
		                sprintf("%s %-21s %s",
		                        unit == null ? "??" : (out ? "->" : "<-"),
		                        peer,
		                        head == "" ? "(keepalive or fragment)"
		                                   : head));
	});

	if (status == null || status != 0) {
		pair_print("on the wire", "-");
		note_print(sprintf("tcpdump: %s, the wire ladder is incomplete",
		                   status == null ? "could not run"
		                                  : sprintf("exit %d", status)));
		return;
	}

	pair_print("on the wire", sprintf("%s of wire.pcap through tcpdump",
	                                  plural(shown, "datagram")));
	pair_print("wire peers", peer_count_text(counts));
}

function sip_section() {
	report_line_add(uc(arc.proto));
	sip_ring_print();
	sip_wire_print();
}

function rtcp_flow_check(port, rtcp_port) {
	if (rtcp_port != null && port == rtcp_port)
		return true;
	return (port % 2) == 1;
}

// The answer is the count of media datagrams on the wire, or null when the
// capture or tcpdump cannot give one.
function media_wire_print(range) {
	let blocked = wire_block_text();

	if (blocked != null) {
		note_print(blocked);
		return null;
	}
	if (range == null) {
		note_print("the RTP port range is unknown, no wire flows");
		return null;
	}

	let ch = channel_get();
	let flows = {};
	let order = [];
	let total = 0;
	let rtp = 0;
	let status = tcpdump_each(sprintf("udp portrange %d-%d", range[0],
	                                  range[1]), (line) => {
		let m = match(line, TCPDUMP_UDP_RE);

		if (!m)
			return;

		let key = sprintf("%s:%s > %s:%s", m[2], m[3], m[4], m[5]);

		if (flows[key] == null) {
			flows[key] = { count: 0, bytes: 0,
			               rtcp: rtcp_flow_check(int(m[3]), ch.rtcp_port) ||
			                     rtcp_flow_check(int(m[5]), ch.rtcp_port) };
			push(order, key);
		}
		flows[key].count++;
		flows[key].bytes += int(m[6]);
		total++;
		if (!flows[key].rtcp)
			rtp++;
	});

	if (status == null || status != 0) {
		note_print(sprintf("tcpdump: %s, no wire flows",
		                   status == null ? "could not run"
		                                  : sprintf("exit %d", status)));
		return null;
	}

	pair_print("wire flows", sprintf("%s in ports %d-%d",
	                                 plural(total, "datagram"), range[0],
	                                 range[1]));
	for (let key in order)
		report_wrap_add("  ", "    ",
		                sprintf("%-44s %s %6d datagrams, %s", key,
		                        flows[key].rtcp ? "RTCP" : "RTP ",
		                        flows[key].count,
		                        bytes_text(flows[key].bytes)));

	return rtp;
}

function media_ring_print() {
	let flows = arc.rings.flows;
	let seen = arc.rings.counts.uvoiced_rtp ?? 0;

	if (!arc.rings.present) {
		note_print("rings.pcapng: absent, no media ring view");
		return;
	}

	pair_print("rtp ring", sprintf("%s of rings.pcapng",
	                               plural(seen, "entry", "entries")));
	if (arc.rings.rtp_capped)
		note_print(sprintf("rtp flows walked for the first %d of %d " +
		                   "records; build the report on a host with --full",
		                   arc.rings.rtp_max, seen));
	for (let key in arc.rings.flow_order)
		report_wrap_add("  ", "    ",
		                sprintf("%s  %d records, %s to %s", key,
		                        flows[key].count,
		                        clock_us_text(flows[key].first),
		                        clock_us_text(flows[key].last)));
	if (arc.rings.rtp_bad)
		note_print(sprintf("%s of the rtp ring carry no readable header",
		                   plural(arc.rings.rtp_bad, "entry", "entries")));
}

function media_section() {
	let counts = arc.manifest.counts?.rings?.rtp ?? {};

	report_line_add("MEDIA ON THE WIRE");
	let wire = media_wire_print(rtp_range_get());

	media_ring_print();
	pair_print("rtp ring counts", sprintf(
		"written %s, exported %s, overwritten %s, truncated %s",
		dash(counts.written), dash(counts.exported), dash(counts.overwritten),
		dash(counts.truncated)));
	if (wire != null)
		pair_print("wire against ring",
		           sprintf("wire RTP %d vs ring %s records", wire,
		                   dash(counts.written)));
}

function ring_count_text(counts) {
	if (counts == null)
		return "-";
	return sprintf("written %s, exported %s, overwritten %s, truncated %s",
	               dash(counts.written), dash(counts.exported),
	               dash(counts.overwritten), dash(counts.truncated));
}

function rings_section() {
	let rings = arc.manifest.counts?.rings ?? {};
	let wire = arc.manifest.counts?.wire ?? {};
	let stats = file_text_get(arc.dir + "/wire.stats");

	report_line_add("RINGS");
	pair_print("log", ring_count_text(rings.log));
	pair_print(arc.proto, ring_count_text(rings[arc.proto]));
	pair_print("status", ring_count_text(rings.status));
	pair_print("rtp", ring_count_text(rings.rtp));
	pair_print("lec", ring_count_text(rings.lec));
	pair_print("relay dropped", dash(rings.relay_dropped));
	pair_print("file bytes", bytes_text(rings.file_bytes));
	pair_print("rings truncated",
	           rings.truncated == null ? "-" : yn(rings.truncated));
	pair_print("wire bytes", bytes_text(wire.bytes));
	pair_print("wire restarts", dash(wire.restarts));
	pair_print("wire truncated",
	           wire.truncated == null ? "-" : yn(wire.truncated));
	pair_print("wire exit", dash(wire.exit));
	pair_print("wire iface", dash(wire.iface));
	pair_print("wire filter", dash(wire.filter));
	pair_print("wire cpumask", dash(wire.cpumask));

	if (stats == null) {
		note_print("wire.stats: absent (wire off)");
		return;
	}
	for (let line in split(rtrim(stats, "\n"), "\n"))
		if (index(line, "captured") >= 0 || index(line, "dropped") >= 0 ||
		    index(line, "received by filter") >= 0)
			report_wrap_add("  ", "    ", line);
}

function diag_snapshot_get() {
	let found;

	for (let snap in arc.snaps.rows)
		if (length(snap.diag?.streams ?? []))
			found = snap;

	return found;
}

function diag_stream_print(row) {
	let parts = [];

	for (let key in DIAG_STREAM_KEYS)
		push(parts, sprintf("%s %s", key, dash(row[key])));

	pair_print(sprintf("stream %s/%s", dash(row.line), dash(row.stream)),
	           join(", ", parts));
}

function diag_engine_print(engine) {
	let buckets = [];
	let hist = engine.interval ?? [];

	for (let i = 0; i < length(ENGINE_HIST_EDGES); i++)
		push(buckets, sprintf("%s %s", ENGINE_HIST_EDGES[i], dash(hist[i])));

	pair_print("engine", sprintf("running %s, period %s us, ticks %s",
	                             engine.running == null
	                             ? "-" : yn(engine.running),
	                             dash(engine.period_us), dash(engine.ticks)));
	pair_print("engine drops", sprintf(
		"holds %s, capture %s, trim %s, playback expired %s, xruns %s",
		dash(engine.holds), dash(engine.capture_drops),
		dash(engine.trim_drops), dash(engine.playback_expired),
		dash(engine.xruns)));
	pair_print("engine intervals", join(" | ", buckets));
}

function hal_counter_get(snap, key) {
	if (!snap)
		return null;
	if (key == "events_dropped")
		return snap.events?.dropped;
	return snap.hal?.[key];
}

function hal_delta_print() {
	let rows = arc.snaps.rows;

	if (length(rows) == 0)
		return;

	let first = rows[0];
	let last = rows[length(rows) - 1];

	report_line_add(sprintf("  %-22s %10s %10s %10s", "hal counter", "first",
	                        "last", "delta"));
	for (let key in HAL_COUNTER_KEYS) {
		let a = hal_counter_get(first, key);
		let b = hal_counter_get(last, key);

		report_line_add(sprintf("  %-22s %10s %10s %10s", key, dash(a),
		                        dash(b),
		                        a == null || b == null ? "-"
		                                               : sprintf("%d", b - a)));
	}
}

function diag_section() {
	let snap = diag_snapshot_get();
	let last = length(arc.snaps.rows)
	           ? arc.snaps.rows[length(arc.snaps.rows) - 1] : null;

	report_line_add("DIAG");
	if (snap == null)
		pair_print("streams", "no snapshot carries a diag stream row");
	else {
		pair_print("streams", sprintf("%s of the %s snapshot at %s",
		                              plural(length(snap.diag.streams), "row"),
		                              dash(snap.kind),
		                              clock_ms_text(snap.t_wall_ms)));
		for (let row in snap.diag.streams)
			diag_stream_print(row);
	}

	let engine = snap?.diag?.engine ?? last?.diag?.engine;
	if (engine == null)
		pair_print("engine", "no snapshot carries a diag engine row");
	else
		diag_engine_print(engine);

	hal_delta_print();
}

function collector_group_text(collectors, want) {
	let names = [];

	for (let name, result in collectors) {
		if (want == "ok" && result != "ok")
			continue;
		if (want == "omitted" && result != "omitted")
			continue;
		if (want == "failed" && (result == "ok" || result == "omitted"))
			continue;
		push(names, want == "failed" ? sprintf("%s (%s)", name, result) : name);
	}

	return length(names) ? join(", ", names) : "none";
}

function proc_lines_print() {
	let text = file_text_get(arc.dir + "/system/daemon-status");

	if (text == null)
		return;

	for (let line in split(text, "\n"))
		if (index(line, "VmRSS:") == 0 || index(line, "Threads:") == 0)
			report_line_add("  " + line);
}

function system_section() {
	let m = arc.manifest;
	let collectors = m.collectors;

	report_line_add("SYSTEM");
	if (collectors == null) {
		pair_print("collectors", "the manifest carries no collector results");
	} else {
		pair_print("collectors ok", collector_group_text(collectors, "ok"));
		pair_print("collectors failed",
		           collector_group_text(collectors, "failed"));
		pair_print("collectors omitted",
		           collector_group_text(collectors, "omitted"));
	}
	pair_print("contains", length(m.contains ?? []) ? join(", ", m.contains)
	                                                : "none");
	for (let note in (m.notes ?? []))
		pair_print("note", note);
	if (arc.config == null)
		note_print("config.json: absent");

	proc_lines_print();
	pair_print("loadavg",
	           trim(dash(line_first_get(file_text_get(arc.dir +
	                                                  "/system/loadavg")))));
	pair_print("uptime",
	           trim(dash(line_first_get(file_text_get(arc.dir +
	                                                  "/system/uptime")))));
}

function how_to_read_section() {
	report_line_add("HOW TO READ");
	report_wrap_add("  ", "  ",
	                "Open rings.pcapng in Wireshark and map DLT 147 to " +
	                "syslog under Decode As, which reads the log and the " +
	                "status rings.");
	report_wrap_add("  ", "  ",
	                "The sip and rtp rings carry synthesized IP and UDP " +
	                "headers. The DSCP byte holds the direction in bits 7..6 " +
	                "(0 transmit, 1 receive) and the kind in bits 5..2.");
	report_wrap_add("  ", "  ",
	                "wire.pcap is the raw capture. It holds the packets as " +
	                "they were, ciphertext included.");
	report_wrap_add("  ", "  ",
	                "snapshots.jsonl holds one JSON object per line, the " +
	                "rows voice status renders.");
	report_wrap_add("  ", "  ",
	                "Build this file again with: ucode " +
	                "tools/calltrace-summary.uc <dir>");
}

// --- the report ---------------------------------------------------------

function report_build() {
	trace_section();
	report_line_add("");
	times_section();
	report_line_add("");
	timeline_section();
	report_line_add("");
	calls_section();
	report_line_add("");
	sip_section();
	report_line_add("");
	media_section();
	report_line_add("");
	rings_section();
	report_line_add("");
	diag_section();
	report_line_add("");
	system_section();
	report_line_add("");
	how_to_read_section();
}

let dir;
let to_stdout = false;
// A POSIX getopt (the musl targets) stops at the script path and leaves the
// separator in ARGV, the rule uvoice-cli.uc follows.
let argv = ARGV[0] == "--" ? slice(ARGV, 1) : ARGV;

for (let i = 0; i < length(argv); i++) {
	if (argv[i] == "-")
		to_stdout = true;
	else if (argv[i] == "--full")
		arc.full = true;
	else if (dir == null)
		dir = argv[i];
}

if (dir == null) {
	warn("usage: ucode calltrace-summary.uc -- <dir> [- | --full]\n");
	exit(1);
}

dir = rtrim(dir, "/");
if (dir == "")
	dir = "/";

arc.dir = dir;
arc.manifest = manifest_get(dir);
if (arc.manifest == null) {
	warn(sprintf("%s holds no readable manifest.json\n", dir));
	exit(1);
}

arc.snaps = snapshots_get(dir);
arc.config = file_json_get(dir + "/config.json");
arc.proto = signaling_get();
arc.rings = rings_read(dir + "/rings.pcapng");

report_build();

let text = join("\n", report) + "\n";

if (to_stdout) {
	print(text);
	exit(0);
}

if (fs.writefile(dir + "/SUMMARY.txt", text) == null) {
	warn(sprintf("cannot write %s/SUMMARY.txt: %s\n", dir, fs.error()));
	exit(2);
}

exit(0);
