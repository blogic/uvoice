#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
"""Scripted MGCP call agent of the end-to-end runner.

usage: mgcp_ca.py --flow NAME --flow-dir DIR --port PORT --socket SOCK
                  [--media-port PORT] [--dialect rfc3435|ncs]
                  [--rto-ms MS] [--max1 N] [--t-hist-s S]

run_e2e.sh starts the agent before the daemon, inside its private network
namespace, so the restart RSIP of the daemon has an answer. The agent binds
UDP on 127.0.0.1 at PORT. It answers the commands of the gateway, sends its
own commands, and drives the stub and reads "voice status" through the ubus
socket SOCK. The flow NAME runs once. When both endpoints read in_service,
the agent writes that status to DIR/ready.json, which is the readiness the
runner waits for. After the flow, the agent writes one verdict line to
DIR/verdict.txt and to its standard output, and answers the gateway until
SIGTERM. The verdict line starts with PASS or FAIL. A SIGTERM before the
verdict ends every wait at once, and the flow fails with that reason.

Every datagram goes to DIR/ca.log with its time.monotonic() stamp, its
direction and its peer. DIR/ca.json holds the same datagrams as a list of
{stamp, dir, text}; the agent writes it at the verdict and again at exit.

The agent answers each command of the gateway once. A repeated command with
the same source and transaction id gets the bytes of the first answer again
(RFC 3435 3.5.2). A command that the flow leaves unanswered is not on record,
so the flow decides again on its repetition. A command of the agent goes out
again on a doubling timer from --rto-ms, at most --max1 times. After a
provisional response it goes out every LONGTRAN_S, and its final response can
come until --t-hist-s behind that provisional response (3.5.6). Every NTFY
must carry the request id that its endpoint accepted last when the NTFY
came, or 0 before the first request (2.3.4).

The agent imports the standard library alone.
"""

import argparse
import json
import os
import re
import select
import signal
import socket
import subprocess
import sys
import time

VERSION_RFC = "MGCP 1.0"
VERSION_NCS = "MGCP 1.0 NCS 1.0"
LONGTRAN_S = 5.0
POLL_S = 0.1
# The HAL words the stub dump carries as numbers (hal.h).
TONE_NONE = 0
TONE_DIAL = 1
TONE_CALL_WAITING = 8
DIR_INACTIVE = 0
DIR_RECVONLY = 2
DIR_SENDRECV = 3
FAX_PASSTHROUGH = 1
# The trace of the stub for a type 2 record (hal.h HAL_CID_TYPE2 is 1).
CID_TYPE2_TRACE = "cid kind=1"
# The ring cadences of the stub trace: L/rg takes the one cadence of the
# region pattern, L/r2 the two of GR-506 pattern 2 (line/line.c
# ring_patterns[]).
RING_CADENCES_RG = 1
RING_CADENCES_R2 = 2
# RFC 3435 4.4.7 step 4 doubles the delay of the disconnected procedure. The
# measured gap holds one RTO of the agent's quiet attempt on top of the delay,
# so the bounds are wider than the scheduling error alone.
DISC_RATIO_MIN = 1.8
DISC_RATIO_MAX = 2.2
# The first delay is uniform between 1 s and Tdinit (mgcp/txn.h
# MGCP_TXN_TD_INIT_DEFAULT), and a wire gap may stand this far from the delay
# the daemon traced. The slack stays under the 0.3 s one attempt lasts, so a
# delay counted from the start of an attempt does not pass.
DISC_FIRST_MIN_MS = 1000
DISC_FIRST_MAX_MS = 15000
DISC_GAP_SLACK_S = 0.1
# The third attempt comes 7 times the first delay after the entry, 105 s at
# most.
DISC_THIRD_WAIT_S = 150
# A disconnected attempt can come 1 s after the entry at the earliest, so
# the piggyback RQNT must leave well inside that second.
PIGGYBACK_SEND_MAX_S = 0.5


class FlowFail(Exception):
    """An assertion of a flow that did not hold."""


class Msg:
    """One MGCP message of a datagram: a command or a response."""

    def __init__(self, text):
        self.text = text
        lines = text.replace("\r\n", "\n").split("\n")
        head = lines[0].split()
        if len(head) < 2:
            raise ValueError(f"short first line: {lines[0]!r}")
        self.is_response = bool(re.fullmatch(r"\d{3}", head[0]))
        self.txid = int(head[1])
        if self.is_response:
            self.code = int(head[0])
            self.verb = None
            self.endpoint = None
            self.version = None
            self.comment = " ".join(head[2:])
        else:
            self.code = None
            self.verb = head[0].upper()
            self.endpoint = head[2] if len(head) > 2 else ""
            self.version = " ".join(head[3:])
        self.params = []
        self.sdp = ""
        for i, line in enumerate(lines[1:], 1):
            if not line.strip():
                self.sdp = "\n".join(lines[i + 1:]).strip("\n")
                break
            name, _, value = line.partition(":")
            self.params.append((name.strip(), value.strip()))

    def param(self, name):
        for key, value in self.params:
            if key.upper() == name.upper():
                return value
        return None

    def first_line(self):
        return self.text.split("\n", 1)[0].rstrip("\r")


def msgs_split(text):
    """The messages of one datagram. A line with one "." separates two
    piggybacked messages (RFC 3435 3.5.5)."""
    msgs, part = [], []
    for line in text.splitlines(keepends=True):
        if line.rstrip("\r\n") == ".":
            msgs.append("".join(part))
            part = []
            continue
        part.append(line)
    if "".join(part).strip():
        msgs.append("".join(part))
    return [m for m in msgs if m.strip()]


def sdp_audio_get(port):
    return ("v=0\r\no=- 4242 1 IN IP4 127.0.0.1\r\ns=-\r\n"
            "c=IN IP4 127.0.0.1\r\nt=0 0\r\n"
            f"m=audio {port} RTP/AVP 0 101\r\n"
            "a=rtpmap:0 PCMU/8000\r\na=rtpmap:101 telephone-event/8000\r\n"
            "a=fmtp:101 0-15\r\na=ptime:20\r\n")


def sdp_t38_get(port):
    return ("v=0\r\no=- 4242 2 IN IP4 127.0.0.1\r\ns=-\r\n"
            "c=IN IP4 127.0.0.1\r\nt=0 0\r\n"
            f"m=image {port} udptl t38\r\n"
            "a=T38FaxVersion:0\r\na=T38MaxBitRate:14400\r\n"
            "a=T38FaxRateManagement:transferredTCF\r\n"
            "a=T38FaxMaxDatagram:160\r\na=T38FaxUdpEC:t38UDPRedundancy\r\n")


def sdp_port_get(sdp, media="audio"):
    m = re.search(rf"^m={media} (\d+) ", sdp, re.M)
    return int(m.group(1)) if m else None


class CallAgent:
    """The wire, the transaction record and the ubus client of one flow."""

    def __init__(self, args):
        self.args = args
        self.flow_dir = args.flow_dir
        self.version = VERSION_NCS if args.dialect == "ncs" else VERSION_RFC
        self.ncs = args.dialect == "ncs"
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", args.port))
        self.sock.setblocking(False)
        self.log = open(os.path.join(self.flow_dir, "ca.log"), "w")
        self.transcript = []
        self.rx = []
        self.answers = {}
        self.txid = 1000 + os.getpid() % 1000 * 1000
        self.rid = 0
        # the request id in force per endpoint, and the requests still open
        self.rids = {}
        self.rids_open = {}
        self.gw = None
        self.policy = None
        self.stopping = False

    def file_write(self, name, text):
        """One file of the flow directory, whole or not at all: the runner
        reads it as soon as it exists."""
        path = os.path.join(self.flow_dir, name)
        with open(path + ".tmp", "w") as f:
            f.write(text)
        os.replace(path + ".tmp", path)

    def stop_check(self):
        if self.stopping:
            raise FlowFail("the agent stopped on SIGTERM before the verdict")

    # ---------------------------------------------------------- the wire

    def datagram_log(self, direction, peer, text):
        stamp = time.monotonic()
        self.log.write(f"{stamp:.6f} {direction} {peer[0]}:{peer[1]}\n")
        for line in text.replace("\r\n", "\n").rstrip("\n").split("\n"):
            self.log.write(f"    {line}\n")
        self.log.flush()
        self.transcript.append({"stamp": stamp, "dir": direction,
                                "text": text})
        return stamp

    def transcript_write(self):
        self.file_write("ca.json", json.dumps(self.transcript, indent=1))

    def datagram_send(self, data, peer=None):
        peer = peer or self.gw
        text = data.decode("latin-1")
        self.datagram_log("tx", peer, text)
        self.sock.sendto(data, peer)

    def command_answer(self, msg, peer):
        key = (peer, msg.txid)
        if key in self.answers:
            self.datagram_send(self.answers[key], peer)
            return
        code = (self.policy or answer_code_get)(msg)
        if code is None:
            return
        comment = "OK" if code < 300 else "Error"
        data = f"{code} {msg.txid} {comment}\r\n".encode()
        self.answers[key] = data
        self.datagram_send(data, peer)

    def response_take(self, msg, peer):
        # RFC 3435 3.5.6: a final response that carries K: wants a 000.
        if msg.code >= 200 and msg.param("K") is not None:
            self.datagram_send(f"000 {msg.txid}\r\n".encode(), peer)
        # a request takes effect with its success response, before any NTFY
        # that the gateway sends behind it
        if msg.txid in self.rids_open and 200 <= msg.code < 300:
            endpoint, rid = self.rids_open.pop(msg.txid)
            self.rids[endpoint] = rid

    def datagram_take(self, data, peer):
        text = data.decode("latin-1")
        stamp = self.datagram_log("rx", peer, text)
        if self.gw is None:
            self.gw = peer
        dgram = len(self.transcript) - 1
        for part in msgs_split(text):
            try:
                msg = Msg(part)
            except ValueError as e:
                self.log.write(f"    unparsed message: {e}\n")
                continue
            # the request id in force on the endpoint when the message came
            self.rx.append({"stamp": stamp, "peer": peer, "dgram": dgram,
                            "msg": msg,
                            "rid": self.rids.get(msg.endpoint, "0")})
            if msg.is_response:
                self.response_take(msg, peer)
            else:
                self.command_answer(msg, peer)

    def wire_pump(self, until):
        """Read every datagram until the monotonic time until."""
        while True:
            left = until - time.monotonic()
            if left <= 0 or self.stopping:
                return
            ready, _, _ = select.select([self.sock], [], [], left)
            if not ready:
                return
            self.datagrams_read()

    def datagrams_read(self):
        while True:
            try:
                data, peer = self.sock.recvfrom(65535)
            except BlockingIOError:
                return
            self.datagram_take(data, peer)

    def rx_mark(self):
        return len(self.rx)

    def wait_for(self, predicate, timeout, start=0):
        """The first received message at or after index start that satisfies
        predicate, or None at the timeout."""
        deadline = time.monotonic() + timeout
        seen = start
        while True:
            for entry in self.rx[seen:]:
                if predicate(entry["msg"]):
                    return entry
            seen = len(self.rx)
            self.stop_check()
            if time.monotonic() >= deadline:
                return None
            self.wire_pump(min(deadline, time.monotonic() + POLL_S))

    def command_wait(self, verb, timeout, start=0, endpoint=None,
                     observed=None, method=None):
        """A command of the gateway. Every NTFY from start up to the one
        found must carry the request id in force on its endpoint when it
        came."""
        def match(m):
            if m.is_response or m.verb != verb:
                return False
            if endpoint is not None and m.endpoint != endpoint:
                return False
            if observed is not None and m.param("O") != observed:
                return False
            return method is None or m.param("RM") == method
        entry = self.wait_for(match, timeout, start)
        if entry is None:
            what = verb + (f" O: {observed}" if observed else "")
            what += f" RM: {method}" if method else ""
            raise FlowFail(f"no {what} from the gateway in {timeout:g} s")
        self.ntfy_check(start, self.rx.index(entry) + 1)
        return entry

    def ntfy_check(self, start, end):
        """Each NTFY received at an index from start to end carries the
        request id in force on its endpoint when it came (RFC 3435 2.3.4)."""
        for e in self.rx[start:end]:
            m = e["msg"]
            if m.is_response or m.verb != "NTFY" or m.param("X") == e["rid"]:
                continue
            raise FlowFail(f"NTFY {m.txid} O: {m.param('O')} carries X: "
                           f"{m.param('X')}, not the request {e['rid']} in "
                           "force")

    # -------------------------------------------------- the agent's commands

    def txid_next(self):
        self.txid += 1
        return self.txid

    def rid_next(self):
        self.rid += 1
        return f"{self.rid:x}"

    def command_build(self, verb, endpoint, params, sdp=None, txid=None):
        txid = txid or self.txid_next()
        text = f"{verb} {txid} {endpoint} {self.version}\r\n"
        for name, value in params:
            text += f"{name}: {value}\r\n"
            if name == "X":
                self.rids_open[txid] = (endpoint, value)
        if sdp:
            text += "\r\n" + sdp
        return txid, text

    def responses_find(self, txids, start):
        """The first final response to each of txids at or after index
        start, and the stamp of the first provisional response to each
        transaction that has no final one yet."""
        finals, provisional = {}, {}
        for entry in self.rx[start:]:
            m = entry["msg"]
            if not m.is_response or m.txid not in txids or m.txid in finals:
                continue
            if m.code >= 200:
                finals[m.txid] = entry
                provisional.pop(m.txid, None)
            else:
                provisional.setdefault(m.txid, entry["stamp"])
        return finals, provisional

    def datagram_exchange(self, data, txids, timeout):
        """Send one datagram of commands and wait for the final response to
        each. The answer maps each transaction id to the first final
        response behind the first send. Each transaction keeps its own state
        (RFC 3435 3.5.6). While one with no provisional response is missing,
        the datagram goes out again on the doubling timer, at most max1
        times. Once every missing one has a provisional response, it goes
        out every LONGTRAN_S. A provisional response moves the deadline to
        T-HIST behind it, the time in which its final response can come."""
        start = self.rx_mark()
        deadline = time.monotonic() + timeout
        rto = self.args.rto_ms / 1000.0
        retries = 0
        longtran = False
        self.datagram_send(data)
        resend_at = time.monotonic() + rto
        while True:
            finals, provisional = self.responses_find(txids, start)
            missing = set(txids) - set(finals)
            if not missing:
                return finals
            self.stop_check()
            now = time.monotonic()
            deadline = max([deadline] + [s + self.args.t_hist_s
                                         for s in provisional.values()])
            if now >= deadline:
                raise FlowFail(f"no final response to transaction "
                               f"{' and '.join(map(str, sorted(missing)))} "
                               f"in time")
            if missing <= set(provisional) and not longtran:
                longtran = True
                resend_at = max(provisional.values()) + LONGTRAN_S
            if now >= resend_at and (longtran or retries < self.args.max1):
                self.datagram_send(data)
                retries += 1
                rto *= 2
                resend_at = now + (LONGTRAN_S if longtran else rto)
            elif now >= resend_at:
                resend_at = deadline
            self.wire_pump(min(deadline, resend_at, now + POLL_S))

    def command_run(self, verb, endpoint, params, sdp=None, timeout=5.0):
        txid, text = self.command_build(verb, endpoint, params, sdp)
        return self.datagram_exchange(text.encode(), [txid], timeout)[txid][
            "msg"]

    def code_expect(self, resp, code, what):
        if resp.code != code:
            raise FlowFail(f"{what} answered {resp.code}, not {code}")
        return resp

    # ------------------------------------------------- ubus, stub, status

    def ubus_call(self, obj, method, args=None):
        cmd = ["ubus", "-s", self.args.socket, "call", obj, method]
        if args is not None:
            cmd.append(json.dumps(args))
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        if out.returncode != 0:
            return None
        return json.loads(out.stdout) if out.stdout.strip() else {}

    def snapshot_save(self, name, data):
        self.file_write(f"{name}.json", json.dumps(data, indent=1))

    def ubus_poll(self, obj, method, predicate, timeout, label):
        """Poll one ubus answer while the wire keeps running. A timeout saves
        the last answer as <label>-timeout.json."""
        deadline = time.monotonic() + timeout
        last = None
        while True:
            self.stop_check()
            last = self.ubus_call(obj, method)
            if last is not None and predicate_check(predicate, last):
                self.snapshot_save(label, last)
                return last
            if time.monotonic() >= deadline:
                break
            self.wire_pump(time.monotonic() + POLL_S)
        self.snapshot_save(f"{label}-timeout", last)
        raise FlowFail(f"{label}: the wait of {timeout:g} s ended")

    def status_wait(self, predicate, timeout, label):
        return self.ubus_poll("voice", "status", predicate, timeout,
                              f"status-{label}")

    def stub_wait(self, predicate, timeout, label):
        return self.ubus_poll("voice.stub", "dump", predicate, timeout,
                              f"stub-{label}")

    def stub_call(self, method, args):
        if self.ubus_call("voice.stub", method, args) is None:
            raise FlowFail(f"voice.stub {method} {json.dumps(args)} refused")
        self.wire_pump(time.monotonic() + 0.02)

    def hook_set(self, state, line=0):
        self.stub_call("hook", {"line": line, "state": state})

    def digits_send(self, digits, line=0):
        for d in digits:
            self.stub_call("digit", {"line": line, "digit": d})
            self.wire_pump(time.monotonic() + 0.05)

    def flash_send(self, line=0):
        self.stub_call("flash", {"line": line})

    def tone_send(self, code, direction="local", line=0):
        self.stub_call("tone", {"line": line, "code": code,
                                "direction": direction})

    def daemon_log_read(self):
        try:
            with open(os.path.join(self.flow_dir, "daemon.log"),
                      errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def daemon_log_has(self, text):
        return text in self.daemon_log_read()

    def ring_cadences_get(self, line=0):
        """The cadence count of the last ring start of one line in the stub
        trace."""
        counts = re.findall(rf"stub: line {line} ring start cadences=(\d+)",
                            self.daemon_log_read())
        return int(counts[-1]) if counts else None

    # ----------------------------------------------------- common steps

    def ready_wait(self, timeout=20.0):
        """Both endpoints in service, after the restart RSIP got its 200.
        The first pass writes ready.json, the readiness of the runner, so
        the runner never polls through a flow that leaves service again."""
        status = self.status_wait(
            lambda s: s["hal"]["state"] == "ready" and len(s["lines"]) == 2
            and all(ln["service"] == "in_service" for ln in s["lines"]),
            timeout, "in-service")
        if not os.path.exists(os.path.join(self.flow_dir, "ready.json")):
            self.file_write("ready.json", json.dumps(status, indent=1))
        return [ln["endpoint"] for ln in status["lines"]]

    def digit_pkg_get(self):
        return "L" if self.ncs else "D"

    def fax_pkg_get(self):
        return "L" if self.ncs else "G"

    def offhook_take(self, ep, line=0):
        """RQNT for L/hd, then the seize and its NTFY."""
        resp = self.command_run("RQNT", ep, [("X", self.rid_next()),
                                             ("R", "L/hd(N)")])
        self.code_expect(resp, 200, "RQNT R: L/hd")
        mark = self.rx_mark()
        self.hook_set("seize", line)
        self.command_wait("NTFY", 5, mark, ep, "L/hd")

    def call_connect(self, ep, call_id, port, line=0, events="L/hu(N)"):
        """An off-hook phone and one connection in sendrecv toward port."""
        self.offhook_take(ep, line)
        resp = self.command_run("CRCX", ep, [
            ("C", call_id), ("L", "p:20, a:PCMU"), ("M", "sendrecv"),
            ("X", self.rid_next()), ("R", events)], sdp_audio_get(port))
        self.code_expect(resp, 200, "CRCX sendrecv")
        conn = resp.param("I")
        if not conn:
            raise FlowFail("the 200 to CRCX carries no I:")
        self.stub_wait(lambda d: stream_get(d, line, 0)["running"] and
                       stream_get(d, line, 0)["remote_port"] == port,
                       5, "media")
        return conn


def predicate_check(predicate, data):
    try:
        return bool(predicate(data))
    except (KeyError, IndexError, TypeError):
        return False


def stream_get(dump, line, stream):
    return dump["line"][line]["stream"][stream]


def answer_code_get(msg):
    if msg.verb == "DLCX":
        return 250
    return 200


# --------------------------------------------------------------- the flows

def mgcp_restart(ca):
    entry = ca.command_wait("RSIP", 15, 0, method="restart")
    rsip = entry["msg"]
    first = ca.rx[0]["msg"]
    if first is not rsip:
        raise FlowFail(f"the first message of the gateway is "
                       f"{first.first_line()!r}, not the restart RSIP")
    if not rsip.endpoint.startswith("aaln/*@"):
        raise FlowFail(f"the restart RSIP names {rsip.endpoint}, not the "
                       "wildcard of every endpoint")
    ep = ca.ready_wait()[0]
    # a persistent event before the first request (RFC 3435 4.4.1)
    mark = ca.rx_mark()
    ca.hook_set("seize")
    ntfy = ca.command_wait("NTFY", 5, mark, ep, "L/hd")["msg"]
    ca.hook_set("release")
    entity = f"127.0.0.1:{ca.args.port}"
    status = ca.status_wait(
        lambda s: s["mgcp"]["notified_entity"] == entity
        and all(ln["notified_entity"] == entity and ln["state"] == "idle"
                for ln in s["lines"])
        and s["mgcp"]["socket"] == "bound"
        and not s["mgcp"]["disconnected"] and s["mgcp"]["rsip"] >= 1,
        5, "restart")
    return (f"RSIP RM: restart for {rsip.endpoint} answered 200, a seize "
            f"before any request gave NTFY O: L/hd with X: "
            f"{ntfy.param('X')}, both endpoints in_service and idle, notified "
            f"entity {status['mgcp']['notified_entity']}")


def mgcp_outbound(ca):
    ep = ca.ready_wait()[0]
    port = ca.args.media_port
    dig = ca.digit_pkg_get()
    ca.offhook_take(ep)
    resp = ca.command_run("RQNT", ep, [
        ("X", ca.rid_next()), ("D", "(xxx#)"),
        ("R", f"L/hu(N), {dig}/[0-9#*T](D)"), ("S", "L/dl")])
    ca.code_expect(resp, 200, "RQNT with the digit map and S: L/dl")
    ca.stub_wait(lambda d: d["line"][0]["tone"] == TONE_DIAL, 3, "dial-tone")
    mark = ca.rx_mark()
    ca.digits_send("123#")
    observed = f"{dig}/1,{dig}/2,{dig}/3,{dig}/#"
    ca.command_wait("NTFY", 8, mark, ep, observed)
    ca.stub_wait(lambda d: d["line"][0]["tone"] == TONE_NONE, 3,
                 "dial-tone-stopped")
    call_id = "A1B2C3D4"
    resp = ca.command_run("CRCX", ep, [
        ("C", call_id), ("L", "p:20, a:PCMU"), ("M", "sendrecv"),
        ("X", ca.rid_next()), ("R", "L/hu(N)")], sdp_audio_get(port))
    ca.code_expect(resp, 200, "CRCX sendrecv")
    conn = resp.param("I")
    local = sdp_port_get(resp.sdp)
    if not conn or not local:
        raise FlowFail("the 200 to CRCX lacks I: or the local description")
    ca.stub_wait(lambda d: stream_get(d, 0, 0)["running"]
                 and stream_get(d, 0, 0)["remote_port"] == port
                 and stream_get(d, 0, 0)["local_port"] == local, 5, "media")
    ca.status_wait(lambda s: s["lines"][0]["state"] == "connected"
                   and s["lines"][0]["connections"][0]["mode"] == "sendrecv",
                   3, "connected")
    resp = ca.command_run("MDCX", ep, [
        ("C", call_id), ("I", conn), ("M", "sendrecv")],
        sdp_audio_get(port + 2))
    ca.code_expect(resp, 200, "MDCX with a new remote port")
    ca.stub_wait(lambda d: stream_get(d, 0, 0)["running"]
                 and stream_get(d, 0, 0)["remote_port"] == port + 2, 5,
                 "media-moved")
    resp = ca.command_run("DLCX", ep, [("C", call_id), ("I", conn)])
    ca.code_expect(resp, 250, "DLCX")
    stats = resp.param("P") or ""
    keys = [k.split("=")[0].strip() for k in stats.split(",") if "=" in k]
    if keys != ["PS", "OS", "PR", "OR", "PL", "JI", "LA"]:
        raise FlowFail(f"the 250 to DLCX carries P: {stats!r}")
    ca.stub_wait(lambda d: not stream_get(d, 0, 0)["running"], 3, "stopped")
    mark = ca.rx_mark()
    ca.hook_set("release")
    ca.command_wait("NTFY", 5, mark, ep, "L/hu")
    return (f"NTFY L/hd, dial tone, NTFY {observed}, CRCX {conn} with local "
            f"port {local}, media on {port} then {port + 2} after MDCX, DLCX "
            f"250 with P: {stats}, NTFY L/hu")


def mgcp_inbound_cid_ring(ca):
    ep = ca.ready_wait()[0]
    port = ca.args.media_port
    call_id = "B1B2B3B4"
    resp = ca.command_run("CRCX", ep, [
        ("C", call_id), ("L", "p:20, a:PCMU"), ("M", "recvonly")])
    ca.code_expect(resp, 200, "CRCX recvonly without a remote description")
    conn = resp.param("I")
    if not conn or sdp_port_get(resp.sdp) is None:
        raise FlowFail("the 200 to CRCX lacks I: or the local description")
    ca.status_wait(lambda s: s["lines"][0]["connections"][0]["mode"] ==
                   "recvonly" and "remote" not in
                   s["lines"][0]["connections"][0], 3, "recvonly")
    dump = ca.ubus_call("voice.stub", "dump")
    if dump is None or stream_get(dump, 0, 0)["running"]:
        raise FlowFail("a stream runs with no remote description")
    resp = ca.command_run("RQNT", ep, [
        ("X", ca.rid_next()), ("R", "L/hd(N)"),
        ("S", 'L/rg, L/ci(10/14/17/26,5551234,"Alice")')])
    ca.code_expect(resp, 200, "RQNT S: L/rg, L/ci")
    ca.stub_wait(lambda d: d["line"][0]["ringing"]
                 and d["line"][0]["cid_number"] == "5551234"
                 and d["line"][0]["cid_name"] == "Alice", 5, "ringing")
    cadences = ca.ring_cadences_get()
    if cadences != RING_CADENCES_RG:
        raise FlowFail(f"L/rg rang with {cadences} cadences, not "
                       f"{RING_CADENCES_RG}")
    mark = ca.rx_mark()
    ca.hook_set("seize")
    ca.command_wait("NTFY", 5, mark, ep, "L/hd")
    ca.stub_wait(lambda d: not d["line"][0]["ringing"], 3, "ring-stopped")
    resp = ca.command_run("MDCX", ep, [
        ("C", call_id), ("I", conn), ("M", "sendrecv")], sdp_audio_get(port))
    ca.code_expect(resp, 200, "MDCX sendrecv with the remote description")
    ca.stub_wait(lambda d: stream_get(d, 0, 0)["running"]
                 and stream_get(d, 0, 0)["remote_port"] == port
                 and stream_get(d, 0, 0)["direction"] == DIR_SENDRECV, 5,
                 "media")
    ca.status_wait(lambda s: s["lines"][0]["state"] == "connected", 3,
                   "connected")
    resp = ca.command_run("DLCX", ep, [("C", call_id), ("I", conn)])
    ca.code_expect(resp, 250, "DLCX")
    ca.stub_wait(lambda d: not stream_get(d, 0, 0)["running"], 3, "stopped")
    ca.hook_set("release")
    return (f"CRCX {conn} recvonly started no stream, L/rg rang "
            f"{cadences} cadence with caller 5551234 Alice, NTFY L/hd stopped "
            f"the ring, MDCX sendrecv ran media on {port}, DLCX 250")


def mgcp_call_waiting(ca):
    ep = ca.ready_wait()[0]
    port = ca.args.media_port
    events = "L/hu(N), L/hf(N)"
    conn_a = ca.call_connect(ep, "C1C1C1C1", port, events=events)
    resp = ca.command_run("CRCX", ep, [
        ("C", "C2C2C2C2"), ("L", "p:20, a:PCMU"), ("M", "inactive"),
        ("X", ca.rid_next()), ("R", events),
        ("S", 'L/wt, L/ci(10/14/17/30,5552222,"Bob")')],
        sdp_audio_get(port + 4))
    ca.code_expect(resp, 200, "second CRCX with S: L/wt")
    conn_b = resp.param("I")
    ca.stub_wait(lambda d: d["line"][0]["tone"] == TONE_CALL_WAITING
                 and d["line"][0]["cid_number"] == "5552222"
                 and d["line"][0]["cid_name"] == "Bob", 5, "call-waiting")
    if not ca.daemon_log_has(f"{CID_TYPE2_TRACE} number='5552222'"):
        raise FlowFail("daemon.log holds no type 2 record for 5552222")
    mark = ca.rx_mark()
    ca.flash_send()
    ca.command_wait("NTFY", 5, mark, ep, "L/hf")
    resp = ca.command_run("MDCX", ep, [
        ("C", "C1C1C1C1"), ("I", conn_a), ("M", "inactive")])
    ca.code_expect(resp, 200, "MDCX inactive on the first call")
    resp = ca.command_run("MDCX", ep, [
        ("C", "C2C2C2C2"), ("I", conn_b), ("M", "sendrecv")])
    ca.code_expect(resp, 200, "MDCX sendrecv on the waiting call")
    ca.stub_wait(lambda d: stream_get(d, 0, 0)["direction"] == DIR_INACTIVE
                 and stream_get(d, 0, 1)["running"]
                 and stream_get(d, 0, 1)["direction"] == DIR_SENDRECV
                 and stream_get(d, 0, 1)["remote_port"] == port + 4, 5,
                 "swapped")
    ca.status_wait(lambda s: [c["mode"] for c in
                              s["lines"][0]["connections"]] ==
                   ["inactive", "sendrecv"], 3, "modes")
    for call_id, conn in (("C1C1C1C1", conn_a), ("C2C2C2C2", conn_b)):
        resp = ca.command_run("DLCX", ep, [("C", call_id), ("I", conn)])
        ca.code_expect(resp, 250, f"DLCX {conn}")
    ca.stub_wait(lambda d: not stream_get(d, 0, 0)["running"]
                 and not stream_get(d, 0, 1)["running"], 3, "stopped")
    ca.hook_set("release")
    return (f"second CRCX {conn_b} played call waiting with the type 2 "
            f"record of 5552222, NTFY L/hf, MDCX swapped {conn_a} to inactive "
            f"and {conn_b} to sendrecv on stream 1, DLCX 250 for both")


def mgcp_hold(ca):
    ep = ca.ready_wait()[0]
    port = ca.args.media_port
    call_id = "D1D2D3D4"
    conn = ca.call_connect(ep, call_id, port)
    ca.stub_wait(lambda d: stream_get(d, 0, 0)["direction"] == DIR_SENDRECV,
                 3, "sendrecv")
    resp = ca.command_run("MDCX", ep, [
        ("C", call_id), ("I", conn), ("M", "recvonly")])
    ca.code_expect(resp, 200, "MDCX M: recvonly")
    if resp.sdp:
        raise FlowFail("the 200 to a change of mode alone carries a "
                       "description")
    ca.stub_wait(lambda d: stream_get(d, 0, 0)["running"]
                 and stream_get(d, 0, 0)["direction"] == DIR_RECVONLY, 3,
                 "held")
    ca.status_wait(lambda s: s["lines"][0]["connections"][0]["mode"] ==
                   "recvonly", 3, "held")
    resp = ca.command_run("MDCX", ep, [
        ("C", call_id), ("I", conn), ("M", "sendrecv")])
    ca.code_expect(resp, 200, "MDCX M: sendrecv")
    ca.stub_wait(lambda d: stream_get(d, 0, 0)["running"]
                 and stream_get(d, 0, 0)["direction"] == DIR_SENDRECV
                 and stream_get(d, 0, 0)["remote_port"] == port, 3, "resumed")
    resp = ca.command_run("DLCX", ep, [("C", call_id), ("I", conn)])
    ca.code_expect(resp, 250, "DLCX")
    mark = ca.rx_mark()
    ca.hook_set("release")
    ca.command_wait("NTFY", 5, mark, ep, "L/hu")
    return (f"MDCX recvonly left stream 0 receiving alone with no "
            f"description in the 200, MDCX sendrecv sent media again on "
            f"{port}, DLCX 250, NTFY L/hu")


def mgcp_fax_tone(ca):
    ep = ca.ready_wait()[0]
    port = ca.args.media_port
    fax = ca.fax_pkg_get()
    call_id = "E1E2E3E4"
    conn = ca.call_connect(ep, call_id, port,
                           events=f"L/hu(N), {fax}/mt(N), {fax}/ft(N)")
    ca.stub_wait(lambda d: stream_get(d, 0, 0)["codec_name"] == "pcmu", 3,
                 "pcmu")
    # one reading: a wait would pass a profile that a later poll still shows
    dump = ca.ubus_call("voice.stub", "dump")
    ca.snapshot_save("stub-before-ced", dump)
    if dump is None or stream_get(dump, 0, 0)["fax_applied"]:
        raise FlowFail("stream 0 carries a fax profile before the CED")
    mark = ca.rx_mark()
    ca.tone_send("ced")
    ca.command_wait("NTFY", 5, mark, ep, f"{fax}/mt")
    ca.stub_wait(lambda d: stream_get(d, 0, 0)["fax_applied"]
                 and stream_get(d, 0, 0)["fax_mode"] == FAX_PASSTHROUGH
                 and stream_get(d, 0, 0)["codec_name"] == "pcmu", 5,
                 "passthrough")
    resp = ca.command_run("MDCX", ep, [
        ("C", call_id), ("I", conn), ("M", "sendrecv")], sdp_t38_get(port + 6))
    ca.code_expect(resp, 200, "MDCX with a T.38 offer")
    if sdp_port_get(resp.sdp, "image") is None:
        raise FlowFail("the 200 to the T.38 MDCX carries no m=image answer")
    ca.stub_wait(lambda d: stream_get(d, 0, 0)["running"]
                 and stream_get(d, 0, 0)["codec_name"] == "t38"
                 and stream_get(d, 0, 0)["remote_port"] == port + 6, 5, "t38")
    resp = ca.command_run("DLCX", ep, [("C", call_id), ("I", conn)])
    ca.code_expect(resp, 250, "DLCX")
    ca.hook_set("release")
    return (f"CED gave NTFY {fax}/mt and the G.711 passthrough profile on "
            f"stream 0, MDCX with an image offer answered m=image and moved "
            f"stream 0 to t38 on {port + 6}, DLCX 250")


def disc_attempts_read(ca, ep):
    """The RSIP attempts of the disconnected procedure: per transaction id,
    the first and the last stamp and the RD: value."""
    attempts = {}
    for entry in ca.rx:
        m = entry["msg"]
        if m.is_response or m.verb != "RSIP" or m.endpoint != ep:
            continue
        if m.param("RM") != "disconnected":
            continue
        a = attempts.setdefault(m.txid, {"first": entry["stamp"],
                                         "rd": m.param("RD")})
        a["last"] = entry["stamp"]
    return sorted(attempts.items(), key=lambda kv: kv[1]["first"])


def copy_last_get(ca, verb, txid):
    """The stamp of the last copy of one command of the gateway."""
    return [e["stamp"] for e in ca.rx if not e["msg"].is_response
            and e["msg"].verb == verb and e["msg"].txid == txid][-1]


def disc_delays_read(ca):
    """The delays of the procedure that the daemon traced: the first one,
    then the one after each failed attempt (mgcp/txn.c)."""
    log = ca.daemon_log_read()
    first = re.findall(r"disconnected: entered, first attempt in (\d+) ms",
                       log)
    nexts = re.findall(r"disconnected: attempt failed, next in (\d+) ms", log)
    return [int(v) for v in first[:1] + nexts]


def disc_timing_check(attempts, entry_s, delays, rto_s):
    """The three attempts against the delays the daemon traced. The first
    delay lies between 1 s and Tdinit and each next one doubles it (RFC 3435
    4.4.7). An attempt ends one RTO behind its last copy, and the next one
    leaves its delay later; entry_s, the end of the NTFY the agent left
    unanswered, stands for the end before the first one. RD: is the whole
    seconds since the entry of the daemon (2.3.12), which the first attempt
    less the first delay gives, within one second each way."""
    if len(delays) < 3:
        raise FlowFail(f"daemon.log traces the delays {delays}, not three")
    d = delays[:3]
    if not DISC_FIRST_MIN_MS <= d[0] <= DISC_FIRST_MAX_MS:
        raise FlowFail(f"the first delay {d[0]} ms is outside "
                       f"{DISC_FIRST_MIN_MS} to {DISC_FIRST_MAX_MS} ms")
    if d[1] != 2 * d[0] or d[2] != 2 * d[1]:
        raise FlowFail(f"the delays {d[0]}, {d[1]} and {d[2]} ms do not "
                       "double")
    starts = [entry_s] + [a["last"] + rto_s for _, a in attempts[:2]]
    entered = attempts[0][1]["first"] - d[0] / 1000.0
    for k, (txid, a) in enumerate(attempts):
        wait = a["first"] - starts[k]
        if abs(wait - d[k] / 1000.0) > DISC_GAP_SLACK_S:
            raise FlowFail(f"RSIP {txid} left {wait:.3f} s after the end "
                           f"before it, not {d[k]} ms")
        since = int(a["first"] - entered)
        if not (a["rd"] or "").isdigit() or abs(int(a["rd"]) - since) > 1:
            raise FlowFail(f"RSIP {txid} carries RD: {a['rd']}, {since} "
                           "whole seconds after the entry")


def mgcp_disconnected(ca):
    ep = ca.ready_wait()[0]
    resp = ca.command_run("RQNT", ep, [("X", ca.rid_next()), ("R", "L/hd(N)")])
    ca.code_expect(resp, 200, "RQNT R: L/hd")
    answered = set()

    def policy(msg):
        if msg.verb != "RSIP" or msg.param("RM") != "disconnected":
            return None
        if msg.txid in answered:
            return 200
        seen = {a for a, _ in disc_attempts_read(ca, ep)}
        if len(seen) >= 3:
            answered.add(msg.txid)
            return 200
        return None

    ca.policy = policy
    mark = ca.rx_mark()
    ca.hook_set("seize")
    ntfy = ca.command_wait("NTFY", 5, mark, ep, "L/hd")["msg"]
    ca.status_wait(lambda s: s["mgcp"]["disconnected"]
                   and s["lines"][0]["service"] == "disconnected", 10,
                   "disconnected")
    ca.command_wait("RSIP", 30, mark, ep, method="disconnected")
    ca.status_wait(lambda s: s["mgcp"]["disc_attempts"] >= 1, 5,
                   "disc-attempts")
    entry = ca.wait_for(lambda m: not m.is_response and
                        m.verb == "RSIP" and m.txid in answered,
                        DISC_THIRD_WAIT_S, mark)
    if entry is None:
        raise FlowFail(f"no third RSIP RM: disconnected in "
                       f"{DISC_THIRD_WAIT_S} s")
    ca.status_wait(lambda s: not s["mgcp"]["disconnected"]
                   and s["mgcp"]["disc_attempts"] == 0
                   and s["lines"][0]["service"] == "in_service", 5,
                   "connected-again")
    attempts = disc_attempts_read(ca, ep)[:3]
    gaps = [attempts[k + 1][1]["first"] - attempts[k][1]["last"]
            for k in range(2)]
    ratio = gaps[1] / gaps[0]
    delays = disc_delays_read(ca)
    spacing = (f"attempts {', '.join(str(t) for t, _ in attempts)} with RD: "
               f"{', '.join(a['rd'] or '-' for _, a in attempts)}, delays "
               f"{', '.join(str(d) for d in delays)} ms, gaps "
               f"{gaps[0]:.3f} s and {gaps[1]:.3f} s, ratio {ratio:.3f}")
    ca.file_write("disc-spacing.txt", spacing + "\n")
    if not DISC_RATIO_MIN <= ratio <= DISC_RATIO_MAX:
        raise FlowFail(f"{spacing}, outside {DISC_RATIO_MIN} to "
                       f"{DISC_RATIO_MAX}")
    rto_s = ca.args.rto_ms / 1000.0
    disc_timing_check(attempts, copy_last_get(ca, "NTFY", ntfy.txid) + rto_s,
                      delays, rto_s)
    return (f"NTFY L/hd unanswered put {ep} in the disconnected procedure, "
            f"{spacing}, the 200 to the third returned it to in_service")


def mgcp_piggyback(ca):
    first = {}

    def policy(msg):
        if msg.verb == "RSIP" and msg.param("RM") == "restart" \
                and first.setdefault("txid", msg.txid) == msg.txid:
            return None
        return answer_code_get(msg)

    ca.policy = policy
    restart = ca.command_wait("RSIP", 15, 0, method="restart")["msg"]
    status = ca.status_wait(
        lambda s: len(s["lines"]) == 2 and
        all(ln["service"] == "disconnected" for ln in s["lines"]), 10,
        "restart-lost")
    ep = status["lines"][0]["endpoint"]
    # The restart RSIP ends one RTO behind its last copy, and the first
    # attempt of the procedure can follow 1 s later.
    late = time.monotonic() - copy_last_get(ca, "RSIP", restart.txid) \
        - ca.args.rto_ms / 1000.0
    if late > PIGGYBACK_SEND_MAX_S:
        raise FlowFail(f"the RQNT leaves {late:.3f} s after the restart RSIP "
                       f"ended, past {PIGGYBACK_SEND_MAX_S} s: a slow host, "
                       "and a disconnected attempt can come first")
    txid, text = ca.command_build("RQNT", ep, [("X", ca.rid_next()),
                                              ("R", "L/hd(N)")])
    resp = ca.datagram_exchange(text.encode(), [txid], 5)[txid]
    ca.code_expect(resp["msg"], 405, "RQNT while restarting")
    front = [e["msg"] for e in ca.rx if e["dgram"] == resp["dgram"]]
    if len(front) != 2 or front[0].verb != "RSIP" \
            or front[1] is not resp["msg"]:
        raise FlowFail("the 405 does not come behind an RSIP in one datagram")
    if front[0].param("RM") != "restart":
        raise FlowFail(f"the RSIP in front carries RM: {front[0].param('RM')}")
    ca.ready_wait()
    t1, t2, rid = piggyback_pair_check(ca, ep)
    return (f"RQNT {txid} left {late:.3f} s after the end of the restart "
            f"RSIP, and its 405 came behind RSIP {front[0].txid} RM: restart "
            f"in one datagram while {ep} was restarting and disconnected; "
            f"RQNT {t1} with AUEP {t2} in one datagram got 200 and 200 in "
            f"order, AUEP X: {rid}")


def piggyback_pair_check(ca, ep):
    """RQNT and AUEP in one datagram. The first final response to each
    behind the send must come in the order of the commands, and the AUEP
    must show the request of the RQNT in front of it."""
    rid = ca.rid_next()
    t1, rqnt = ca.command_build("RQNT", ep, [("X", rid), ("R", "L/hd(N)")])
    t2, auep = ca.command_build("AUEP", ep, [("F", "X,R")])
    finals = ca.datagram_exchange((rqnt + ".\r\n" + auep).encode(),
                                  [t1, t2], 5)
    r1, r2 = finals[t1], finals[t2]
    ca.code_expect(r1["msg"], 200, "piggybacked RQNT")
    ca.code_expect(r2["msg"], 200, "piggybacked AUEP")
    if ca.rx.index(r1) > ca.rx.index(r2):
        raise FlowFail("the AUEP answer came before the RQNT answer")
    if r2["msg"].param("X") != rid:
        raise FlowFail(f"the AUEP shows X: {r2['msg'].param('X')}, not the "
                       f"request {rid} of the RQNT in front of it")
    return t1, t2, rid


def mgcp_response_cache(ca):
    ep = ca.ready_wait()[0]
    txid, text = ca.command_build("CRCX", ep, [("C", "F1F2F3F4"),
                                              ("L", "p:20, a:PCMU"),
                                              ("M", "recvonly")])
    data = text.encode()
    first = ca.datagram_exchange(data, [txid], 5)[txid]
    ca.code_expect(first["msg"], 200, "CRCX")
    second = ca.datagram_exchange(data, [txid], 5)[txid]
    if second is first or second["msg"].text != first["msg"].text:
        raise FlowFail("the repeated CRCX got no copy of its first response")
    conn = first["msg"].param("I")
    resp = ca.command_run("AUEP", ep, [("F", "I")])
    ca.code_expect(resp, 200, "AUEP F: I")
    if resp.param("I") != conn:
        raise FlowFail(f"AUEP lists I: {resp.param('I')!r}, not {conn} alone")
    ca.status_wait(lambda s: len(s["lines"][0]["connections"]) == 1, 3,
                   "one-connection")
    resp = ca.command_run("DLCX", ep, [("C", "F1F2F3F4"), ("I", conn)])
    ca.code_expect(resp, 250, "DLCX")
    return (f"CRCX {txid} sent twice got the same response with I: {conn}, "
            f"AUEP lists that connection alone; the deferred final is not "
            f"tested")


def mgcp_ncs(ca):
    hold = {"on": True}

    def policy(msg):
        if msg.verb == "RSIP" and msg.param("RM") == "restart" and hold["on"]:
            return None
        return answer_code_get(msg)

    ca.policy = policy
    entry = ca.command_wait("RSIP", 15, 0, method="restart")
    rsip = entry["msg"]
    if rsip.version != VERSION_NCS:
        raise FlowFail(f"the restart RSIP carries {rsip.version!r}")
    status = ca.status_wait(lambda s: len(s["lines"]) == 2, 5, "endpoints")
    ep = status["lines"][0]["endpoint"]
    fronts = []
    for verb, params, code in (("AUEP", [("F", "R")], 200),
                               ("RQNT", [("X", ca.rid_next()),
                                         ("R", "L/hd(N)")], 520)):
        txid, text = ca.command_build(verb, ep, params)
        resp = ca.datagram_exchange(text.encode(), [txid], 5)[txid]
        ca.code_expect(resp["msg"], code, f"{verb} while restarting")
        pair = [e["msg"] for e in ca.rx if e["dgram"] == resp["dgram"]]
        if len(pair) != 2 or pair[0].verb != "RSIP" \
                or pair[0].txid != rsip.txid:
            raise FlowFail(f"the {code} to {verb} has no restart RSIP in "
                           "front of it")
        fronts.append(f"{verb} {code}")
    hold["on"] = False
    ca.command_answer(rsip, entry["peer"])
    ca.ready_wait()
    ca.offhook_take(ep)
    resp = ca.command_run("RQNT", ep, [
        ("X", ca.rid_next()), ("D", "(xx)"), ("R", "L/hu(N), L/[0-9#*T](D)")])
    ca.code_expect(resp, 200, "RQNT with L/ digits")
    mark = ca.rx_mark()
    ca.digits_send("12")
    ca.command_wait("NTFY", 8, mark, ep, "L/1,L/2")
    resp = ca.command_run("RQNT", ep, [("X", ca.rid_next()), ("R", "L/hu(N)")])
    ca.code_expect(resp, 200, "RQNT R: L/hu")
    resp = ca.command_run("EPCF", ep, [("B", "e:mu")])
    ca.code_expect(resp, 510, "EPCF under ncs")
    mark = ca.rx_mark()
    ca.hook_set("release")
    ca.command_wait("NTFY", 5, mark, ep, "L/hu")
    resp = ca.command_run("RQNT", ep, [
        ("X", ca.rid_next()), ("R", "L/hd(N)"), ("S", "L/r2")])
    ca.code_expect(resp, 200, "RQNT S: L/r2")
    ca.stub_wait(lambda d: d["line"][0]["ringing"], 3, "ringing")
    cadences = ca.ring_cadences_get()
    if cadences != RING_CADENCES_R2:
        raise FlowFail(f"L/r2 rang with {cadences} cadences, not "
                       f"{RING_CADENCES_R2}")
    resp = ca.command_run("AUEP", ep, [("F", "S")])
    ca.code_expect(resp, 200, "AUEP F: S")
    if resp.param("S") != "L/r2":
        raise FlowFail(f"AUEP shows S: {resp.param('S')!r}, not L/r2")
    resp = ca.command_run("RQNT", ep, [("X", ca.rid_next()), ("S", "L/wt")])
    ca.code_expect(resp, 522, "RQNT S: L/wt under ncs")
    return (f"version {VERSION_NCS}, the restart RSIP in front of "
            f"{' and '.join(fronts)}, NTFY L/hd and L/1,L/2, EPCF 510, "
            f"L/r2 rang {cadences} cadences and AUEP shows it, L/wt 522")


FLOWS = {
    "mgcp-restart": mgcp_restart,
    "mgcp-outbound": mgcp_outbound,
    "mgcp-inbound-cid-ring": mgcp_inbound_cid_ring,
    "mgcp-call-waiting": mgcp_call_waiting,
    "mgcp-hold": mgcp_hold,
    "mgcp-fax-tone": mgcp_fax_tone,
    "mgcp-disconnected": mgcp_disconnected,
    "mgcp-piggyback": mgcp_piggyback,
    "mgcp-response-cache": mgcp_response_cache,
    "mgcp-ncs": mgcp_ncs,
}


def verdict_get(ca, flow):
    try:
        detail = flow(ca)
        # the NTFY commands that no wait of the flow looked at
        ca.ntfy_check(0, len(ca.rx))
        return "PASS " + detail
    except FlowFail as e:
        return f"FAIL {e}"
    except Exception as e:  # noqa: BLE001
        return f"FAIL agent error: {type(e).__name__}: {e}"


def args_parse():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--flow", required=True, choices=sorted(FLOWS))
    ap.add_argument("--flow-dir", required=True)
    ap.add_argument("--port", type=int, default=2727)
    ap.add_argument("--socket", required=True)
    ap.add_argument("--media-port", type=int, default=30000)
    ap.add_argument("--dialect", choices=["rfc3435", "ncs"], default="rfc3435")
    ap.add_argument("--rto-ms", type=int, default=200)
    ap.add_argument("--max1", type=int, default=5)
    ap.add_argument("--t-hist-s", type=int, default=10)
    return ap.parse_args()


def main():
    args = args_parse()
    ca = CallAgent(args)

    def stop(_sig, _frame):
        ca.stopping = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    verdict = verdict_get(ca, FLOWS[args.flow])
    ca.transcript_write()
    ca.file_write("verdict.txt", verdict + "\n")
    print(verdict, flush=True)
    ca.policy = None
    while not ca.stopping:
        ca.wire_pump(time.monotonic() + POLL_S)
    ca.transcript_write()
    return 0


if __name__ == "__main__":
    sys.exit(main())
