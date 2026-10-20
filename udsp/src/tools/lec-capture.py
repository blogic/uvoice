#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-only
#
# udsp - software voice DSP for FXS lines
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
"""Extract the canceller capture of each line from a uvoice call trace.

usage: lec-capture.py <rings.pcapng | archive dir | archive.tar.gz>
                      <out-prefix> [--line N] [--ref-loss N]
       lec-capture.py --self-test [--lec-replay PATH]

The input is the rings.pcapng of a call trace archive, the directory of an
unpacked archive, or the archive itself (uvoice/files/calltrace-README.txt).
The tool reads a tar archive in memory, so no file of the archive goes to the
disk. The pcapng reader follows the byte-order magic of each section header,
so it reads both byte orders. It ignores each block type other than the
section header, the interface description and the enhanced packet. The tool
reads each line of the canceller ring, or line N only with --line N.

--ref-loss N gives the reference loss of the capture in half-dB steps, 0 to
60: the loss that the engine applied ahead of the reference of the
canceller (udsp/src/dsp/lec.h, udsp_ec_nlms_ref_loss_set). The ring does not
carry it, so the operator gives it from the configuration of the call. The
tool writes it into the header of each replay file and into the comment line
of each CSV, and prints it. Without the option, the replay file carries no
loss and lec_replay replays it at 0.

The canceller ring. The interface has the name uvoiced_lec (the if_name
option) and the link type 148 (DLT_USER1). One enhanced packet block carries
one entry. Its time stamp is the wall clock of the tick in microseconds. Its
captured length and its original length are the length of the entry. An entry
is a header of 64 bytes, then the planes. Each field and each sample is in the
byte order of the section. The header has the field order, the offsets and
the widths of the record header of udsp/src/test/bench/lec_replay.h:

    offset  width  field
    0       8      stamp_ns, CLOCK_MONOTONIC of the tick
    8       8      tick, the engine tick index
    16      4      lost_before, records of the line lost before this one
    20      4      frames of the instance after the record
    24      4      hold_frames of the instance after the record
    28      4      erle_mdb, signed
    32      4      echo_gain_micro
    36      4      coef_energy_micro
    40      4      ref_delay_samples
    44      2      gen
    46      2      flags
    48      2      samples, 80 or 160
    50      2      tail_ms
    52      2      noise_ddbov, tenths of dB; 1270 is unmeasured
    54      2      probes
    56      2      diverge_resets
    58      1      line
    59      1      kind: 0 RUN, 1 FEED, 2 END
    60      1      nlp_verdict: 0 off, 1 quiet, 2 dt, 3 unlike, 4 erle, 5 pass
    61      1      hold_left
    62      2      skipped, periods the engine skipped before the record

The header has no reserved byte. skipped counts the periods that the engine
skipped since the previous record of the line. The engine ran no tick in
them, so the canceller saw nothing in them.

The flags are NEW 1<<0, RESET 1<<1, FREEZE 1<<2, NLP 1<<3, FIRED 1<<4, DT
1<<5, ACTIVE 1<<6 and PROBING 1<<7. Bits 8 to 11 name the reason of a FEED:
open 1<<8, modem 1<<9, off 1<<10 and inactive 1<<11. SKIPPED 1<<12 stands
exactly when skipped is not 0. The planes follow the header, with samples
int16 in each plane:

    kind      planes          entry bytes
    0 RUN     ref, mic, out   64 + 6 x samples: 544 or 1024
    1 FEED    ref, mic        64 + 4 x samples: 384 or 704
    2 END     none            64

The rules below are the rules of the replay too. The header comment of
udsp/src/test/bench/lec_replay.h lists them for the C reader.

The header carries no version. The reader refuses an entry in these cases:

- a captured length other than the original length of the packet;
- a kind above 2, or samples other than 80 or 160 (END too);
- a length that does not match the kind and samples;
- a FEED without exactly one reason bit, or a RUN or an END with one;
- SKIPPED without a skipped count, or a count without SKIPPED;
- a skipped count on NEW or on an END.

The C reader also refuses a record whose line differs from the line of the
file. The tool writes one file per line, so its files meet that rule.

The checks of each line. A lineage holds the records of one canceller
instance. NEW starts a lineage, RESET restarts frames inside it, and END
closes it. Each check reports the tick and the count:

- A record with lost_before above 0 is a loss, with SKIPPED or without it.
  The loss breaks the lineage until the next NEW.
- A loss on a NEW record belongs to the lineage before it. The lost records
  can hold the END of that lineage, so the tool reports the loss alone.
- Inside a lineage, each RUN adds 1 to frames and each FEED adds 0. A RUN
  with NEW or RESET reads frames 1, and a FEED with NEW reads 0.
- An END without a loss reads the frames of the record before it, because
  the instance ran no frame after that record. Another count is a gap: a
  RUN before the END is lost from the archive. A FEED lost just before an
  END stays invisible, because an END carries no skipped count and follows
  no tick rule.
- Each engine tick of a line in a call gives one record. The tick of a
  record follows the previous tick by one. On a record with SKIPPED, it
  follows by skipped + lost_before + 1, and the tool counts the skipped
  periods. Any other jump is a gap: records are lost from the archive. A gap
  breaks the lineage too.
- A record with no NEW before it starts a joined lineage. lec_replay
  replays such a lineage from the next NEW only.
- A lineage with no END before the next NEW is a failure, unless that NEW
  carries a loss. The tool also reports a last lineage with no END as open
  at the end. That is not a failure, because a capture can stop before the
  release of the instance.

The completeness check. The log ring (uvoiced_log, link type 147) carries the
canceller lines of the backend, one per second and line (media_lec_trace() in
uvoice/src/media_trace.c):

    [media] l<N> lec nlp frames=F active=A wait=W erle=E unlike=U
        dt=D quiet=Q

For each such line inside a lineage of line N, the tool finds the records of
that lineage with frames F. It takes the record whose stamp is nearest to the
stamp of the line, and reports the offset between the two stamps. It then
compares the six counts with its tally of the records up to that record. The
tally counts from the start of the lineage or from the last RESET, because a
re-time zeroes the six counts with frames. The tally rule:

- a pass with ACTIVE counts as active, and a pass without ACTIVE as wait;
- quiet, dt, unlike and erle count under their names;
- an off verdict counts nowhere.

A media line outside all lineages is not checked. A tally after a loss, a gap
or a joined start is incomplete, and its media line is not checked. A media
line with no record at its frames, or with other counts, is a completeness
failure.

The outputs of each line keep the samples of its first record. A RUN or a
FEED record at other samples is a failure, and it stays out of the outputs.

- <prefix>-l<N>.lecr, the replay file of lec_replay.h. The file flag bit 0
  is set when the first record is NEW. With --ref-loss, the file header
  holds the loss at offset 18, and the file flag bit 1 is set. A FEED record
  has zeros in out, and an END record has zeros in the three planes.
- <prefix>-l<N>.csv, one row per record. With --ref-loss, a comment line
  "# ref_loss_half_db=N" comes before the column names. A row starts with
  the tick, the ms from the first record, the kind, gen and frames. The
  flags word and the eight flags follow. Then come the reason of a FEED
  (open, modem, off or inactive), skipped, the verdict of a RUN, hold_left
  and hold_delta. Then
  come the ERLE and the echo gain in dB, the coefficient energy, the floor
  in dBov (empty while unmeasured), probes, diverge_resets,
  ref_delay_samples and lost_before. The RMS level of each plane in dBFS
  ends the row.
  hold_delta is the adapt verdict: the difference of hold_frames to the
  previous record. It reads 1 for a frame whose update did not stand, and 0
  on NEW and on RESET.
- <prefix>-l<N>-ref.wav, -mic.wav and -out.wav, mono 16 bit at the line
  rate (samples x 100 Hz). A FEED frame writes mic into out, because the
  uplink carried the raw frame. An END record writes no frame.

The time axis of the WAV files is the axis of lec_replay --residual. The
frame of a RUN or a FEED record sits at its tick less the tick of the first
record of the line. Zeros fill the ticks between two such records: skipped
periods, gaps and the time from an END to the next NEW. One such run of zeros
is cut to 60 s, and a note counts the cuts. The RIFF header limits the data
of a WAV file to 4 GiB less 36 bytes. A line whose files would pass that
limit is an I/O error, and the tool writes no file of the line.

The summary gives, for each line, the lineages, the skipped periods of the
engine, the losses and the gaps. For each lineage, it gives the FEED records
by reason and the six counts at the end. It also gives the shares of
the judged frames after the first 15 s of each lineage, by the rule of
uvoice/test/tools/lec_calls.py. The judged frames are the five counts other
than quiet. The seconds with 20 judged frames or more make the body, and a
body under 5 s is too short.

The exit status is 0 when each check passed and 1 when a check failed. It is
2 for a usage or an I/O error.

The self-test builds pcapng sections in memory in both byte orders. It checks
the parse, each check, the CSV, the WAV files and the replay file. Small
scenarios check the jump rule, a lost END, a lost final RUN, the flag
refusals, the time axis, the WAV limit and a truncated archive. With
--lec-replay PATH it also runs that lec_replay binary on the replay files it
wrote. It reads back the reference loss of a file with the loss and the
warning of a file without it. It then writes a synthetic call and counts the
census of lec_replay again from the frame file. Without the option it skips
these steps and prints a note."""

import argparse
import contextlib
import csv
import gzip
import io
import math
import os
import re
import statistics
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import wave
import zlib
from array import array

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import udsp_audio as ua  # noqa: E402

SHB_BYTES = b"\x0a\x0d\x0d\x0a"
ORDER_MAGIC = {b"\x4d\x3c\x2b\x1a": "<", b"\x1a\x2b\x3c\x4d": ">"}
ORDER_NAMES = {"<": "little-endian", ">": "big-endian"}
NATIVE_ORDER = "<" if sys.byteorder == "little" else ">"
BLOCK_SHB = 0x0A0D0D0A
BLOCK_IDB = 1
BLOCK_EPB = 6
OPT_END = 0
OPT_IF_NAME = 2
OPT_IF_TSRESOL = 9
TSRESOL_US = 6

LEC_IFACE = "uvoiced_lec"
LEC_LINKTYPE = 148
LOG_IFACE = "uvoiced_log"
LOG_LINKTYPE = 147

KIND_RUN, KIND_FEED, KIND_END = 0, 1, 2
KIND_NAMES = ("run", "feed", "end")
KIND_PLANES = (3, 2, 0)
FLAG_NAMES = ("new", "reset", "freeze", "nlp", "fired", "dt", "active",
              "probing")
FLAG_NEW = 1 << 0
FLAG_RESET = 1 << 1
FLAG_ACTIVE = 1 << 6
FLAG_SKIPPED = 1 << 12
REASON_NAMES = ("open", "modem", "off", "inactive", "synthetic")
REASON_BITS = (1 << 8, 1 << 9, 1 << 10, 1 << 11, 1 << 13)
REASON_MASK = sum(REASON_BITS)
VERDICT_PASS = 5
VERDICT_NAMES = ("off", "quiet", "dt", "unlike", "erle", "pass")
SAMPLES_VALID = (80, 160)

HEAD_BYTES = 64
HEAD_FORMAT = "QQIIIiIIIHHHHHHHBBBBH"
HEAD_FIELDS = ("stamp_ns", "tick", "lost_before", "frames", "hold_frames",
               "erle_mdb", "echo_gain_micro", "coef_energy_micro",
               "ref_delay_samples", "gen", "flags", "samples", "tail_ms",
               "noise_ddbov", "probes", "diverge_resets", "line", "kind",
               "nlp_verdict", "hold_left", "skipped")

LECR_MAGIC = b"UDSPLECR"
LECR_VERSION = 2
LECR_FILE_FORMAT = "<8sHHIHHIII"
LECR_FIRST_NEW = 1 << 0
LECR_REF_LOSS = 1 << 1
# The reach of the engine's receive loss (udsp/src/dsp/lec.h
# UDSP_EC_NLMS_REF_LOSS_MAX); the replay reader refuses a larger value.
REF_LOSS_MAX = 60
CSV_REF_LOSS = "# ref_loss_half_db=%d"
NOISE_UNMEASURED = 1270
TICKS_PER_SECOND = 100
WAV_GAP_TICKS_MAX = 60 * TICKS_PER_SECOND
# The RIFF size field holds 36 + the data bytes in 32 bits.
WAV_DATA_MAX = 0xFFFFFFFF - 36

COUNT_KEYS = ("active", "wait", "erle", "unlike", "dt", "quiet")
JUDGED_KEYS = ("active", "wait", "erle", "unlike", "dt")
VERDICT_COUNT = {1: "quiet", 2: "dt", 3: "unlike", 4: "erle"}
SKIP_S = 15
JUDGED_MIN = 20
BODY_MIN = 5

MEDIA_RE = re.compile(r"\[media\] l(\d+) lec nlp (.*)$")
LIST_MAX = 10

CSV_HEAD = (["tick", "rel_ms", "kind", "gen", "frames", "flags"] +
            list(FLAG_NAMES) + ["reason", "skipped"] +
            ["verdict", "hold_left", "hold_delta", "erle_db",
             "echo_gain_db", "coef_energy", "floor_dbov", "probes",
             "diverge_resets", "ref_delay_samples", "lost_before",
             "ref_dbfs", "mic_dbfs", "out_dbfs"])

assert struct.calcsize("<" + HEAD_FORMAT) == HEAD_BYTES
assert struct.calcsize(LECR_FILE_FORMAT) == 32
assert array("h").itemsize == 2


class CaptureError(Exception):
    """A usage or an I/O error: the exit status is 2."""


class Record:
    """One entry of the canceller ring. An END record has no plane."""
    __slots__ = HEAD_FIELDS + ("stamp_us", "ref", "mic", "out", "tally",
                               "valid")

    def __init__(self, **fields):
        for name in HEAD_FIELDS + ("stamp_us",):
            setattr(self, name, fields.get(name, 0))
        for name in ("ref", "mic", "out", "tally"):
            setattr(self, name, fields.get(name))
        self.valid = False


class Iface:
    __slots__ = ("name", "link", "tsresol")

    def __init__(self, name, link, tsresol):
        self.name = name
        self.link = link
        self.tsresol = tsresol


class MediaLine:
    __slots__ = ("stamp_us", "line", "frames", "counts")

    def __init__(self, stamp_us, line, frames, counts):
        self.stamp_us = stamp_us
        self.line = line
        self.frames = frames
        self.counts = counts


class Trace:
    """What the pcapng holds for the tool."""

    def __init__(self):
        self.orders = []
        self.ifaces = []
        self.problems = []
        self.lec_seen = False
        self.log_seen = False
        self.records = []
        self.refused = []
        self.log_lines = 0
        self.media = []
        self.unparsed = []


class Lineage:
    """The records of one instance, and the state of the checks."""

    def __init__(self, index, joined):
        self.index = index
        self.joined = joined
        self.records = []
        self.ended = False
        self.end_lost = False
        self.followed = None
        self.broken = None
        self.prev_tick = None
        self.prev_frames = None if joined else 0
        self.tally = [0] * len(COUNT_KEYS)
        self.valid = not joined
        self.runs = 0
        self.feeds = 0
        self.reasons = [0] * len(REASON_NAMES)
        self.resets = 0
        self.skip_marks = 0
        self.skip_periods = 0
        self.by_frames = None


class Issue:
    __slots__ = ("word", "tick", "text", "failure")

    def __init__(self, word, tick, text, failure=True):
        self.word = word
        self.tick = tick
        self.text = text
        self.failure = failure


class MediaResult:
    def __init__(self):
        self.checked = 0
        self.equal = 0
        self.outside = 0
        self.incomplete = 0
        self.missing = []
        self.unequal = []
        self.offsets = []


class LineResult:
    def __init__(self, line):
        self.line = line
        self.records = []
        self.samples = 0
        self.mixed = 0
        self.lineages = []
        self.issues = []
        self.media = MediaResult()
        self.frames_written = 0
        self.gaps_capped = 0
        self.ref_loss = None


class Result:
    def __init__(self):
        self.trace = None
        self.lines = []
        self.failures = 0


def stamp_us_get(units, tsresol):
    """A pcapng time stamp in microseconds (pcapng, option if_tsresol)."""
    exp = tsresol & 0x7F
    if tsresol & 0x80:
        return (units * 1000000) >> exp
    if exp <= TSRESOL_US:
        return units * 10 ** (TSRESOL_US - exp)
    return units // 10 ** (exp - TSRESOL_US)


def blocks_walk(data, problems):
    """Each whole block as (byte order, block type, body). A damaged block
    ends the walk and adds a line to problems."""
    off = 0
    order = None
    while off < len(data):
        head = bytes(data[off:off + 12])
        if order is None and head[:4] != SHB_BYTES:
            raise CaptureError("not a pcapng file: no section header at "
                               "byte %d" % off)
        if len(head) < 12:
            problems.append("byte %d: a truncated block" % off)
            return
        if head[:4] == SHB_BYTES:
            order = ORDER_MAGIC.get(head[8:12])
            if order is None:
                problems.append("byte %d: a section header with an unknown "
                                "byte-order magic" % off)
                return
        btype, blen = struct.unpack_from(order + "II", data, off)
        if blen < 12 or blen % 4 or off + blen > len(data):
            problems.append("byte %d: a block of %d bytes, %d bytes left" %
                            (off, blen, len(data) - off))
            return
        if struct.unpack_from(order + "I", data, off + blen - 4)[0] != blen:
            problems.append("byte %d: the block trailer differs from the "
                            "block length" % off)
            return
        yield order, btype, memoryview(data)[off + 8:off + blen - 4]
        off += blen


def iface_parse(order, body):
    link = struct.unpack_from(order + "H", body, 0)[0]
    name = ""
    tsresol = TSRESOL_US
    off = 8
    while off + 4 <= len(body):
        code, size = struct.unpack_from(order + "HH", body, off)
        value = bytes(body[off + 4:off + 4 + size])
        off += 4 + ((size + 3) & ~3)
        if code == OPT_END:
            break
        if code == OPT_IF_NAME:
            name = value.split(b"\0")[0].decode("utf-8", "replace")
        elif code == OPT_IF_TSRESOL and value:
            tsresol = value[0]
    return Iface(name, link, tsresol)


def reason_get(flags):
    """The index in REASON_NAMES of the one reason bit of flags, None when
    no reason bit is set, or -1 for more than one."""
    bits = flags & REASON_MASK
    if not bits:
        return None
    if bits & (bits - 1):
        return -1
    return REASON_BITS.index(bits)


def flags_refusal(rec):
    """The flag rule of udsp.h that the record breaks, or None."""
    reason = reason_get(rec.flags)
    kind = KIND_NAMES[rec.kind].upper()
    feed = rec.kind == KIND_FEED
    one = reason is not None and reason >= 0
    if (feed and not one) or (not feed and reason is not None):
        return "a %s with reason bits 0x%04x" % (kind,
                                                 rec.flags & REASON_MASK)
    if bool(rec.flags & FLAG_SKIPPED) != bool(rec.skipped):
        return "SKIPPED %s with skipped %d" % (
            "set" if rec.flags & FLAG_SKIPPED else "clear", rec.skipped)
    if rec.skipped and (rec.kind == KIND_END or rec.flags & FLAG_NEW):
        return "skipped %d on %s" % (
            rec.skipped, "an END" if rec.kind == KIND_END else "NEW")
    return None


def entry_decode(payload, order, stamp_us):
    """A Record, or the reason the framing refuses the entry."""
    if len(payload) < HEAD_BYTES:
        return "an entry of %d bytes, shorter than the header" % len(payload)
    rec = Record(**dict(zip(HEAD_FIELDS,
                            struct.unpack_from(order + HEAD_FORMAT, payload))))
    rec.stamp_us = stamp_us
    if rec.kind > KIND_END:
        return "kind %d" % rec.kind
    if rec.samples not in SAMPLES_VALID:
        return "samples %d" % rec.samples
    refusal = flags_refusal(rec)
    if refusal:
        return refusal
    n = rec.samples
    want = HEAD_BYTES + 2 * n * KIND_PLANES[rec.kind]
    if len(payload) != want:
        return "%d bytes for a %s entry of %d samples, not %d" % (
            len(payload), KIND_NAMES[rec.kind].upper(), n, want)
    planes = array("h", bytes(payload[HEAD_BYTES:]))
    if order != NATIVE_ORDER:
        planes.byteswap()
    if rec.kind != KIND_END:
        rec.ref = planes[:n]
        rec.mic = planes[n:2 * n]
    if rec.kind == KIND_RUN:
        rec.out = planes[2 * n:]
    return rec


def media_parse(text):
    """A MediaLine, None for another line, or "" for a canceller count line
    that does not carry the seven fields."""
    m = MEDIA_RE.match(text)
    if not m:
        return None
    words = dict(w.split("=", 1) for w in m.group(2).split() if "=" in w)
    try:
        counts = tuple(int(words[k]) for k in COUNT_KEYS)
        return MediaLine(0, int(m.group(1)), int(words["frames"]), counts)
    except (KeyError, ValueError):
        return ""


def trace_parse(data):
    trace = Trace()
    section = []
    for order, btype, body in blocks_walk(data, trace.problems):
        if btype == BLOCK_SHB:
            trace.orders.append(order)
            section = []
            continue
        if btype == BLOCK_IDB:
            if len(body) < 8:
                trace.problems.append("an interface block of %d bytes" %
                                      len(body))
                break
            section.append(len(trace.ifaces))
            trace.ifaces.append(iface_parse(order, body))
            continue
        if btype != BLOCK_EPB:
            continue
        if len(body) < 20:
            trace.problems.append("a packet block of %d bytes" % len(body))
            break
        iid, high, low, cap, orig = struct.unpack_from(order + "IIIII",
                                                       body, 0)
        if iid >= len(section) or 20 + cap > len(body):
            trace.problems.append("a packet block that names interface %d "
                                  "or holds %d bytes" % (iid, cap))
            continue
        packet_add(trace, trace.ifaces[section[iid]], order,
                   (high << 32) | low, body[20:20 + cap], orig)
    trace.lec_seen = any(i.name == LEC_IFACE and i.link == LEC_LINKTYPE
                         for i in trace.ifaces)
    trace.log_seen = any(i.name == LOG_IFACE and i.link == LOG_LINKTYPE
                         for i in trace.ifaces)
    return trace


def packet_add(trace, iface, order, units, payload, orig):
    """orig is the original length of the packet. An entry of the canceller
    ring is never cut, so a captured length other than orig is refused."""
    stamp_us = stamp_us_get(units, iface.tsresol)
    if iface.name == LEC_IFACE and iface.link == LEC_LINKTYPE:
        rec = entry_decode(payload, order, stamp_us)
        if len(payload) != orig:
            rec = "a captured length of %d bytes, not the original %d" % (
                len(payload), orig)
        if isinstance(rec, str):
            trace.refused.append((len(trace.records) + len(trace.refused),
                                  rec))
        else:
            trace.records.append(rec)
        return
    if iface.name != LOG_IFACE or iface.link != LOG_LINKTYPE:
        return
    trace.log_lines += 1
    text = bytes(payload).decode("utf-8", "replace").rstrip("\0\r\n")
    media = media_parse(text)
    if media == "":
        trace.unparsed.append(text)
    elif media is not None:
        media.stamp_us = stamp_us
        trace.media.append(media)


def count_index_get(verdict, active):
    if verdict == VERDICT_PASS:
        return COUNT_KEYS.index("active" if active else "wait")
    name = VERDICT_COUNT.get(verdict)
    return COUNT_KEYS.index(name) if name else None


def lineage_break(lin, rec):
    if lin.broken is None:
        lin.broken = rec.tick
    lin.valid = False


def jump_check(lin, rec, issues):
    """The tick rule against the previous record of an intact lineage. A
    loss clears prev_tick (line_check), so lost_before reads 0 here."""
    skipped = rec.skipped if rec.flags & FLAG_SKIPPED else 0
    want = skipped + rec.lost_before + 1
    jump = rec.tick - lin.prev_tick
    if jump == want:
        if skipped:
            lin.skip_marks += 1
            lin.skip_periods += skipped
        return
    if jump > 0:
        issues.append(Issue("gap", rec.tick, "a jump of %d ticks before "
                            "tick %d, where skipped %d and lost_before %d "
                            "account for %d" % (jump, rec.tick, skipped,
                                                rec.lost_before, want)))
    else:
        issues.append(Issue("tick", rec.tick, "tick %d follows tick %d" %
                            (rec.tick, lin.prev_tick)))
    lineage_break(lin, rec)
    lin.prev_frames = None


def record_check(lin, rec, issues):
    """The tick, frames and tally rules for a RUN or a FEED record."""
    run = rec.kind == KIND_RUN
    if lin.prev_tick is not None:
        jump_check(lin, rec, issues)
    lin.prev_tick = rec.tick
    if run and rec.flags & FLAG_RESET:
        lin.prev_frames = 0
        lin.tally = [0] * len(COUNT_KEYS)
        lin.valid = True
        lin.resets += 1
    if lin.prev_frames is not None:
        want = lin.prev_frames + (1 if run else 0)
        if rec.frames != want:
            issues.append(Issue("frames", rec.tick, "frames %d at tick %d, "
                                "not %d" % (rec.frames, rec.tick, want)))
            lineage_break(lin, rec)
    lin.prev_frames = rec.frames
    if run:
        lin.runs += 1
        index = count_index_get(rec.nlp_verdict, rec.flags & FLAG_ACTIVE)
        if index is not None:
            lin.tally[index] += 1
    else:
        lin.feeds += 1
        lin.reasons[reason_get(rec.flags)] += 1
    rec.tally = tuple(lin.tally)
    rec.valid = lin.valid


def end_check(lin, rec, issues):
    """The frames rule for an END. A loss on the END, a joined start and a
    tick gap leave prev_frames None, and the rule then does not apply."""
    if lin.prev_frames is not None and rec.frames != lin.prev_frames:
        issues.append(Issue("gap", rec.tick, "an END with frames %d at tick "
                            "%d after frames %d: a RUN before it is lost" %
                            (rec.frames, rec.tick, lin.prev_frames)))
        lineage_break(lin, rec)
    lin.ended = True
    rec.tally = tuple(lin.tally)
    rec.valid = lin.valid


def line_check(recs):
    """The lineages of one line in record order, and the issues of the
    checks. A loss on a NEW record belongs to the lineage before it, and
    the lost records can hold the END of that lineage."""
    lineages = []
    issues = []
    cur = None
    for rec in recs:
        end = rec.kind == KIND_END
        fresh = not end and rec.flags & FLAG_NEW
        loss = rec.lost_before > 0
        if loss:
            issues.append(Issue("loss", rec.tick, "%d records lost before "
                                "tick %d" % (rec.lost_before, rec.tick)))
        if fresh:
            if cur is not None and loss:
                lineage_break(cur, rec)
                cur.end_lost = not cur.ended
            elif cur is not None and not cur.ended:
                cur.followed = rec.tick
            cur = Lineage(len(lineages) + 1, False)
            lineages.append(cur)
        elif cur is None or cur.ended:
            cur = Lineage(len(lineages) + 1, True)
            lineages.append(cur)
            if loss:
                lineage_break(cur, rec)
        elif loss:
            lineage_break(cur, rec)
            cur.prev_tick = None
            cur.prev_frames = None
        cur.records.append(rec)
        if end:
            end_check(cur, rec, issues)
            continue
        record_check(cur, rec, issues)
    for lin in lineages:
        last = lin.records[-1].tick
        if lin.joined:
            issues.append(Issue("joined", lin.records[0].tick, "lineage %d "
                                "starts without NEW at tick %d: %d records "
                                "before the next NEW" %
                                (lin.index, lin.records[0].tick,
                                 len(lin.records))))
        if lin.ended or lin.end_lost:
            continue
        if lin.followed is not None:
            issues.append(Issue("open", last, "lineage %d has no END before "
                                "the NEW at tick %d" %
                                (lin.index, lin.followed)))
        else:
            issues.append(Issue("open", last, "lineage %d is open at the end "
                                "(last record at tick %d)" %
                                (lin.index, last), failure=False))
    return lineages, issues


def lineage_span_find(lineages, stamp_us):
    for lin in lineages:
        if lin.records[0].stamp_us <= stamp_us <= lin.records[-1].stamp_us:
            return lin
    return None


def media_check(lineages, media):
    out = MediaResult()
    for m in media:
        lin = lineage_span_find(lineages, m.stamp_us)
        if lin is None:
            out.outside += 1
            continue
        if lin.by_frames is None:
            lin.by_frames = {}
            for rec in lin.records:
                if rec.kind != KIND_END:
                    lin.by_frames.setdefault(rec.frames, []).append(rec)
        found = lin.by_frames.get(m.frames)
        if not found:
            out.missing.append(m)
            continue
        rec = min(found, key=lambda r: abs(m.stamp_us - r.stamp_us))
        if not rec.valid:
            out.incomplete += 1
            continue
        out.checked += 1
        out.offsets.append(m.stamp_us - rec.stamp_us)
        if m.counts == rec.tally:
            out.equal += 1
        else:
            out.unequal.append((m, rec))
    return out


def shares_text(lin):
    """The judged shares of a lineage after the first SKIP_S seconds, by the
    rule of lec_calls.py."""
    first = lin.records[0].tick
    seconds = {}
    for rec in lin.records:
        if rec.kind != KIND_RUN:
            continue
        second = (rec.tick - first) // TICKS_PER_SECOND
        if second < SKIP_S:
            continue
        cell = seconds.setdefault(second, [0] * (len(COUNT_KEYS) + 1))
        cell[-1] += 1
        index = count_index_get(rec.nlp_verdict, rec.flags & FLAG_ACTIVE)
        if index is not None:
            cell[index] += 1
    judged_of = [COUNT_KEYS.index(k) for k in JUDGED_KEYS]
    body = [c for c in seconds.values()
            if sum(c[i] for i in judged_of) >= JUDGED_MIN]
    if len(body) < BODY_MIN:
        return "too short after the first %d s (%d judged seconds)" % (
            SKIP_S, len(body))
    total = [sum(c[i] for c in body) for i in range(len(COUNT_KEYS) + 1)]
    judged = sum(total[i] for i in judged_of)
    shares = ", ".join("%s %.0f%%" % (k, 100.0 * total[COUNT_KEYS.index(k)] /
                                      judged) for k in JUDGED_KEYS)
    quiet = 100.0 * total[COUNT_KEYS.index("quiet")] / total[-1]
    return "%d judged seconds after the first %d s: %s; quiet frames %.0f%% " \
           "of all" % (len(body), SKIP_S, shares, quiet)


def counts_text(counts):
    return " ".join("%s %d" % (k, v) for k, v in zip(COUNT_KEYS, counts))


def clock_text(stamp_us):
    return "%s.%03d" % (time.strftime("%H:%M:%S", time.gmtime(
        stamp_us // 1000000)), stamp_us % 1000000 // 1000)


def lecr_encode(line, samples, recs, ref_loss=None):
    """The replay file of lec_replay.h, little-endian on each host. ref_loss
    is the reference loss in half-dB steps, or None for a file without it."""
    first_new = bool(recs and recs[0].flags & FLAG_NEW)
    flags = LECR_FIRST_NEW if first_new else 0
    if ref_loss is not None:
        flags |= LECR_REF_LOSS
    out = bytearray(struct.pack(LECR_FILE_FORMAT, LECR_MAGIC, LECR_VERSION,
                                line, samples * TICKS_PER_SECOND, samples,
                                ref_loss or 0, len(recs), flags, 0))
    zeros = (0,) * samples
    head = struct.Struct("<" + HEAD_FORMAT)
    planes = struct.Struct("<%dh" % (3 * samples))
    for rec in recs:
        out += head.pack(*(getattr(rec, f) for f in HEAD_FIELDS))
        end = rec.kind == KIND_END
        out += planes.pack(*(zeros if end else rec.ref),
                           *(zeros if end else rec.mic),
                           *(rec.out if rec.kind == KIND_RUN else zeros))
    return bytes(out)


def level_text(plane):
    return "" if plane is None else "%.1f" % ua.dbfs(ua.rms(plane))


def csv_row_get(rec, first, prev):
    """One CSV row. prev is the RUN or FEED record before rec, or None."""
    end = rec.kind == KIND_END
    fresh_or_reset = rec.flags & (FLAG_NEW | FLAG_RESET)
    if end or (prev is None and not fresh_or_reset):
        delta = ""
    elif fresh_or_reset:
        delta = 0
    else:
        delta = rec.hold_frames - prev.hold_frames
    gain = ""
    if rec.echo_gain_micro:
        gain = "%.2f" % (10.0 * math.log10(rec.echo_gain_micro / 1e6))
    floor = ""
    if rec.noise_ddbov != NOISE_UNMEASURED:
        floor = "%.1f" % (rec.noise_ddbov / 10.0)
    verdict = ""
    if rec.kind == KIND_RUN:
        verdict = (VERDICT_NAMES[rec.nlp_verdict]
                   if rec.nlp_verdict < len(VERDICT_NAMES)
                   else str(rec.nlp_verdict))
    reason = ""
    if rec.kind == KIND_FEED:
        reason = REASON_NAMES[reason_get(rec.flags)]
    bits = [1 if rec.flags & (1 << i) else 0 for i in range(len(FLAG_NAMES))]
    return ([rec.tick, "%.3f" % ((rec.stamp_ns - first.stamp_ns) / 1e6),
             KIND_NAMES[rec.kind], rec.gen, rec.frames,
             "0x%04x" % rec.flags] + bits + [reason, rec.skipped] +
            [verdict, rec.hold_left, delta, "%.3f" % (rec.erle_mdb / 1000.0),
             gain, "%.6f" % (rec.coef_energy_micro / 1e6), floor, rec.probes,
             rec.diverge_resets, rec.ref_delay_samples, rec.lost_before,
             level_text(rec.ref), level_text(rec.mic), level_text(rec.out)])


def csv_write(path, recs, ref_loss=None):
    prev = None
    with open(path, "w", newline="") as f:
        if ref_loss is not None:
            f.write(CSV_REF_LOSS % ref_loss + "\n")
        out = csv.writer(f)
        out.writerow(CSV_HEAD)
        for rec in recs:
            out.writerow(csv_row_get(rec, recs[0], prev))
            if rec.kind != KIND_END:
                prev = rec


def wav_data_max():
    """WAV_DATA_MAX, or a smaller count of bytes in UDSP_TEST_WAV_DATA_MAX,
    the override of lec_replay. A test can thus reach the limit with a small
    trace. Any other value leaves the limit of the format."""
    v = os.environ.get("UDSP_TEST_WAV_DATA_MAX", "")
    if v.isdigit() and 0 < int(v) <= WAV_DATA_MAX:
        return int(v)
    return WAV_DATA_MAX


def planes_gaps(recs):
    """The zero frames before each RUN or FEED record on the time axis of the
    docstring. The second value counts the runs cut to WAV_GAP_TICKS_MAX."""
    gaps = []
    capped = 0
    tick_next = recs[0].tick if recs else 0
    for rec in recs:
        if rec.kind == KIND_END:
            continue
        gap = max(rec.tick - tick_next, 0)
        tick_next = rec.tick + 1
        if gap > WAV_GAP_TICKS_MAX:
            gap = WAV_GAP_TICKS_MAX
            capped += 1
        gaps.append((gap, rec))
    return gaps, capped


def planes_collect(recs, samples):
    """The three planes on one time axis. Returns (ref, mic, out, capped):
    capped counts the gaps cut to WAV_GAP_TICKS_MAX. A plane that would pass
    the data limit of a WAV file raises CaptureError before any copy."""
    gaps, capped = planes_gaps(recs)
    frames = sum(gap + 1 for gap, _ in gaps)
    limit = wav_data_max()
    if 2 * samples * frames > limit:
        raise CaptureError("line %d needs %d frames, more than the %d data "
                           "bytes of a WAV file" % (recs[0].line, frames,
                                                   limit))
    ref, mic, out = array("h"), array("h"), array("h")
    for gap, rec in gaps:
        if gap:
            zeros = array("h", bytes(2 * samples * gap))
            ref += zeros
            mic += zeros
            out += zeros
        ref.extend(rec.ref)
        mic.extend(rec.mic)
        out.extend(rec.out if rec.kind == KIND_RUN else rec.mic)
    return ref, mic, out, capped


def outputs_write(prefix, lr):
    base = "%s-l%d" % (prefix, lr.line)
    recs = lr.records
    ref, mic, out, lr.gaps_capped = planes_collect(recs, lr.samples)
    try:
        with open(base + ".lecr", "wb") as f:
            f.write(lecr_encode(lr.line, lr.samples, recs, lr.ref_loss))
        csv_write(base + ".csv", recs, lr.ref_loss)
        rate = lr.samples * TICKS_PER_SECOND
        for suffix, plane in (("-ref", ref), ("-mic", mic), ("-out", out)):
            ua.wav_write(base + suffix + ".wav", plane, rate)
    except OSError as e:
        raise CaptureError("cannot write %s: %s" % (base, e.strerror))
    lr.frames_written = len(ref) // lr.samples
    return base


def line_print(lr):
    recs = lr.records
    print("line %d: %d Hz, records %d, ticks %d to %d, lineages %d, "
          "skipped periods %d in %d jumps" %
          (lr.line, lr.samples * TICKS_PER_SECOND, len(recs), recs[0].tick,
           recs[-1].tick, len(lr.lineages),
           sum(lin.skip_periods for lin in lr.lineages),
           sum(lin.skip_marks for lin in lr.lineages)))
    if lr.mixed:
        print("  mixed rates: records with other samples than %d: %d, "
              "not written" % (lr.samples, lr.mixed))
    for lin in lr.lineages:
        first, last = lin.records[0], lin.records[-1]
        print("  lineage %d: ticks %d to %d, from %s, gen %d, records %d "
              "(RUN %d, FEED %d, RESET %d), %s%s" %
              (lin.index, first.tick, last.tick,
               "a join" if lin.joined else "NEW", first.gen,
               len(lin.records), lin.runs, lin.feeds, lin.resets,
               "END" if lin.ended else "no END",
               "" if lin.broken is None else
               ", broken at tick %d" % lin.broken))
        print("    FEED by reason: %s" %
              ", ".join("%s %d" % kv for kv in zip(REASON_NAMES,
                                                     lin.reasons)))
        print("    counts at the end: %s%s" %
              (counts_text(lin.tally),
               "" if lin.valid else " (incomplete)"))
        print("    shares: %s" % shares_text(lin))
    words = {}
    for issue in lr.issues:
        words.setdefault(issue.word, []).append(issue)
    for word in ("loss", "gap", "tick", "frames", "joined", "open"):
        issues = words.get(word, [])
        for issue in issues[:LIST_MAX]:
            print("  %s: %s" % (word if issue.failure else "note",
                                issue.text))
        if len(issues) > LIST_MAX:
            print("  %s: %d more" % (word, len(issues) - LIST_MAX))
    media_print(lr.media)


def media_print(mr):
    print("  media lines checked %d, equal %d; not checked: %d outside the "
          "capture, %d with an incomplete tally" %
          (mr.checked, mr.equal, mr.outside, mr.incomplete))
    for m in mr.missing[:LIST_MAX]:
        print("  media line at %s: no record with frames %d" %
              (clock_text(m.stamp_us), m.frames))
    for m, rec in mr.unequal[:LIST_MAX]:
        print("  media line at %s, frames %d: %s; records to tick %d: %s" %
              (clock_text(m.stamp_us), m.frames, counts_text(m.counts),
               rec.tick, counts_text(rec.tally)))
    more = len(mr.missing) + len(mr.unequal) - 2 * LIST_MAX
    if more > 0:
        print("  media lines: %d more failures" % more)
    if mr.offsets:
        ms = [o / 1000.0 for o in mr.offsets]
        print("  offset of a media line to its record: median %.1f ms "
              "(min %.1f, max %.1f)" %
              (statistics.median(ms), min(ms), max(ms)))


def line_failures(lr):
    return (sum(1 for i in lr.issues if i.failure) + (1 if lr.mixed else 0) +
            len(lr.media.missing) + len(lr.media.unequal))


def trace_print(trace, where, size):
    orders = sorted(set(ORDER_NAMES[o] for o in trace.orders))
    print("source %s: %d bytes, pcapng sections %d (%s)" %
          (where, size, len(trace.orders), ", ".join(orders)))
    print("interfaces: %s" % (", ".join("%s (%d)" % (i.name, i.link)
                                        for i in trace.ifaces) or "none"))
    for problem in trace.problems:
        print("pcapng: %s" % problem)
    if trace.log_seen:
        print("log ring: lines %d, canceller count lines %d, unparsed %d" %
              (trace.log_lines, len(trace.media), len(trace.unparsed)))
    else:
        print("no log ring (interface %s, link type %d)" %
              (LOG_IFACE, LOG_LINKTYPE))
    for text in trace.unparsed[:LIST_MAX]:
        print("  unparsed: %s" % text)
    if not trace.lec_seen:
        print("no canceller ring (interface %s, link type %d)" %
              (LEC_IFACE, LEC_LINKTYPE))
        return
    kinds = [sum(1 for r in trace.records if r.kind == k) for k in range(3)]
    print("canceller ring: entries %d: RUN %d, FEED %d, END %d; refused %d" %
          (len(trace.records) + len(trace.refused), kinds[0], kinds[1],
           kinds[2], len(trace.refused)))
    for index, reason in trace.refused[:LIST_MAX]:
        print("  refused entry %d: %s" % (index, reason))


def ref_loss_text(ref_loss):
    if ref_loss is None:
        return ("reference loss: none given, so lec_replay replays the "
                "files at 0")
    return "reference loss %d half-dB (%.1f dB)" % (ref_loss, ref_loss / 2.0)


def capture_process(data, where, prefix, line_want, ref_loss=None):
    """Parses, checks, writes and prints. The caller maps the result to the
    exit status. ref_loss is the --ref-loss value, or None."""
    res = Result()
    res.trace = trace = trace_parse(data)
    trace_print(trace, where, len(data))
    print(ref_loss_text(ref_loss))
    res.failures = (len(trace.problems) + len(trace.refused) +
                    len(trace.unparsed))
    if not trace.lec_seen:
        res.failures += 1
        return res
    lines = sorted(set(r.line for r in trace.records))
    if line_want is not None:
        lines = [line_want]
    if not lines:
        print("no canceller record")
        res.failures += 1
    for line in lines:
        lr = LineResult(line)
        lr.ref_loss = ref_loss
        recs = [r for r in trace.records if r.line == line]
        if not recs:
            print("line %d: no canceller record" % line)
            res.failures += 1
            continue
        lr.samples = recs[0].samples
        lr.records = [r for r in recs
                      if r.kind == KIND_END or r.samples == lr.samples]
        lr.mixed = len(recs) - len(lr.records)
        lr.lineages, lr.issues = line_check(lr.records)
        lr.media = media_check(lr.lineages,
                               [m for m in trace.media if m.line == line])
        base = outputs_write(prefix, lr)
        line_print(lr)
        print("  wrote %s.lecr (records %d), %s.csv, %s-ref.wav, "
              "%s-mic.wav, %s-out.wav (frames %d, %.2f s)" %
              (base, len(lr.records), base, base, base, base,
               lr.frames_written, lr.frames_written / TICKS_PER_SECOND))
        if lr.gaps_capped:
            print("  note: %d tick gaps cut to %d s in the WAV files" %
                  (lr.gaps_capped, WAV_GAP_TICKS_MAX // TICKS_PER_SECOND))
        res.failures += line_failures(lr)
        res.lines.append(lr)
    return res


def source_read(path):
    """The bytes of rings.pcapng, and the name of their source."""
    try:
        if os.path.isdir(path):
            path = os.path.join(path, "rings.pcapng")
            with open(path, "rb") as f:
                return f.read(), path
        with open(path, "rb") as f:
            data = f.read()
        if data[:4] == SHB_BYTES:
            return data, path
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as tar:
            found = [m for m in tar.getmembers()
                     if m.isfile() and os.path.basename(m.name) ==
                     "rings.pcapng"]
            if len(found) != 1:
                raise CaptureError("%s: %d rings.pcapng in the archive" %
                                   (path, len(found)))
            return tar.extractfile(found[0]).read(), "%s:%s" % (
                path, found[0].name)
    except OSError as e:
        raise CaptureError("%s: %s" % (path, e.strerror or e))
    except tarfile.TarError:
        raise CaptureError("%s: not a pcapng file, a directory or a tar "
                           "archive" % path)
    except (EOFError, zlib.error) as e:
        raise CaptureError("%s: a truncated or damaged archive: %s" %
                           (path, e))


def capture_run(source, prefix, line_want, ref_loss=None):
    data, where = source_read(source)
    res = capture_process(data, where, prefix, line_want, ref_loss)
    print("result: %s" % ("PASS" if not res.failures else
                          "FAIL, failed checks %d" % res.failures))
    return 0 if not res.failures else 1


# The fixture of the test_lec case "lec capture fixture reads back the
# documented values". The case holds the same values as literals.
FIXTURE_LINE = 1
FIXTURE_SAMPLES = 80
FIXTURE_KINDS = (KIND_RUN, KIND_RUN, KIND_FEED, KIND_RUN, KIND_RUN, KIND_END)
FIXTURE_FLAGS = (0x0009, 0x0048, 0x0100, 0x000E, 0x10B8, 0x0000)
FIXTURE_FRAMES = (1, 2, 2, 1, 2, 2)
FIXTURE_VERDICTS = (5, 5, 0, 1, 2, 0)
FIXTURE_REF_LOSS = 18


def fixture_records():
    """Six records of line 1 at 8000 Hz. The file header carries the
    reference loss FIXTURE_REF_LOSS. Record i has tick 1000 + i,
    stamp_ns 1000000000 + 10000000 x i, hold_frames i, erle_mdb -1500 x i,
    echo_gain_micro 250000, coef_energy_micro 1000 x i, ref_delay_samples
    80 + i, gen 7, tail_ms 48, probes i and diverge_resets 0. noise_ddbov is
    1270 on record 0 and 653 after it. On record 4, lost_before is 7,
    hold_left is 3 and skipped is 5; the three are 0 elsewhere. The kinds,
    the flags, frames and the verdicts are the FIXTURE_ tuples. Sample k of
    record i reads ref 100 x i + k and mic i, and out -(100 x i + k) on a
    RUN."""
    recs = []
    for i, kind in enumerate(FIXTURE_KINDS):
        ks = range(FIXTURE_SAMPLES)
        recs.append(Record(
            stamp_ns=1000000000 + 10000000 * i, tick=1000 + i,
            lost_before=7 if i == 4 else 0, frames=FIXTURE_FRAMES[i],
            hold_frames=i, erle_mdb=-1500 * i, echo_gain_micro=250000,
            coef_energy_micro=1000 * i, ref_delay_samples=80 + i, gen=7,
            flags=FIXTURE_FLAGS[i], samples=FIXTURE_SAMPLES, tail_ms=48,
            noise_ddbov=NOISE_UNMEASURED if i == 0 else 653, probes=i,
            line=FIXTURE_LINE, kind=kind, nlp_verdict=FIXTURE_VERDICTS[i],
            hold_left=3 if i == 4 else 0, skipped=5 if i == 4 else 0,
            ref=tuple(100 * i + k for k in ks), mic=(i,) * FIXTURE_SAMPLES,
            out=tuple(-(100 * i + k) for k in ks)))
    return recs


def fixture_write(path):
    try:
        with open(path, "wb") as f:
            f.write(lecr_encode(FIXTURE_LINE, FIXTURE_SAMPLES,
                                fixture_records(), FIXTURE_REF_LOSS))
    except OSError as e:
        raise CaptureError("cannot write %s: %s" % (path, e.strerror))
    print("wrote %s" % path)
    return 0


# --- the self-test --------------------------------------------------------
#
# The builders below restate the pcapng blocks and the entry framing at
# fixed offsets, with no use of the reader, so a fault in the reader cannot
# hide in a matching fault of the stimulus.

T_OFFSETS = ((0, "Q", "stamp_ns"), (8, "Q", "tick"), (16, "I", "lost_before"),
             (20, "I", "frames"), (24, "I", "hold_frames"),
             (28, "i", "erle_mdb"), (32, "I", "echo_gain_micro"),
             (36, "I", "coef_energy_micro"), (40, "I", "ref_delay_samples"),
             (44, "H", "gen"), (46, "H", "flags"), (48, "H", "samples"),
             (50, "H", "tail_ms"), (52, "H", "noise_ddbov"),
             (54, "H", "probes"), (56, "H", "diverge_resets"),
             (58, "B", "line"), (59, "B", "kind"), (60, "B", "nlp_verdict"),
             (61, "B", "hold_left"), (62, "H", "skipped"))
T_HEAD = 64
T_PLANES = {0: 3, 1: 2, 2: 0}
T_COUNT_KEYS = ("active", "wait", "erle", "unlike", "dt", "quiet")
T_LOG = ("uvoiced_log", 147)
T_LEC = ("uvoiced_lec", 148)
T_WALL_US = 1790000000000000
T_MONO_NS = 5000000000000
T_MEDIA_US = 4000

R, F, E = 0, 1, 2
NEW, RST, FRZ, NLP, FIR, DT, ACT, PRB = (1 << i for i in range(8))
SO, SM, SF, SI = (1 << i for i in range(8, 12))
SKP = 1 << 12
V_OFF, V_QUIET, V_DT, V_UNLIKE, V_ERLE, V_PASS = range(6)

# tick, kind, frames, flags, verdict, lost_before, and skipped when it is
# not 0
T_LINE0_CLEAN = [
    (1000, R, 1, NEW | NLP, V_QUIET, 0),
    (1001, R, 2, NLP, V_QUIET, 0),
    (1002, R, 3, NLP, V_PASS, 0),
    (1003, R, 4, NLP | ACT, V_PASS, 0),
    (1004, R, 5, NLP | ACT, V_PASS, 0),
    (1005, F, 5, SO, V_OFF, 0),
    (1006, F, 5, SO, V_OFF, 0),
    (1007, R, 6, NLP | DT | FIR, V_DT, 0),
    (1008, R, 7, NLP, V_ERLE, 0),
    (1009, R, 8, NLP, V_UNLIKE, 0),
    (1010, R, 9, 0, V_OFF, 0),
    (1011, R, 1, RST | NLP, V_QUIET, 0),
    (1014, R, 2, NLP | ACT | SKP, V_PASS, 0, 2),
    (1015, E, 2, 0, V_OFF, 0),
]
T_LINE0_MORE = [
    (1100, R, 1, NEW | NLP, V_QUIET, 0),
    (1101, R, 2, NLP, V_QUIET, 0),
    (1104, R, 5, NLP, V_QUIET, 2),
    (1105, R, 6, NLP, V_QUIET, 0),
    (1106, E, 6, 0, V_OFF, 0),
    (1200, F, 0, NEW | SI, V_OFF, 0),
    (1201, R, 1, NLP, V_QUIET, 0),
    (1202, R, 2, NLP, V_QUIET, 0),
    (1205, R, 5, NLP, V_QUIET, 0),
    (1206, R, 6, NLP, V_QUIET, 0),
]
T_LINE1 = [
    (500, R, 40, NLP | ACT, V_PASS, 0),
    (501, R, 41, NLP | ACT, V_PASS, 0),
    (502, E, 41, 0, V_OFF, 0),
    (600, R, 1, NEW | NLP | FRZ, V_QUIET, 0),
    (601, F, 1, SM, V_OFF, 0),
    (602, R, 2, NLP | PRB, V_ERLE, 0),
    (603, E, 2, 0, V_OFF, 0),
    (700, R, 1, NEW | NLP, V_QUIET, 0),
    (701, R, 3, NLP, V_QUIET, 0),
    (702, F, 3, SF, V_OFF, 0),
    (800, R, 1, NEW | NLP, V_QUIET, 0),
    (801, E, 1, 0, V_OFF, 0),
]
# line, the tick of the record the line follows, frames, the six counts
T_MEDIA_CLEAN = [
    (0, 990, 0, (0, 0, 0, 0, 0, 0)),
    (0, 1009, 8, (2, 1, 1, 1, 1, 2)),
    (0, 1014, 2, (1, 0, 0, 0, 0, 1)),
]
T_MEDIA_MORE = [
    (0, 1006, 5, (3, 1, 0, 0, 0, 2)),
    (0, 1105, 6, (0, 0, 0, 0, 0, 4)),
    (0, 1203, 99, (0, 0, 0, 0, 0, 2)),
    (1, 501, 41, (41, 0, 0, 0, 0, 0)),
    (1, 602, 2, (0, 0, 1, 0, 0, 1)),
]
T_LOG_OTHER = ["[fsm] l0 off-hook", "[media] l0 s0 jb out=1 plc=0",
               "[media] l0 lec ec=on nlp=on erle_mdb=1000 ref_ready=1"]
T_LOG_BAD = "[media] l0 lec nlp frames=3 active=1"


def t_record(line, samples, row):
    tick, kind, frames, flags, verdict, lost = row[:6]
    ks = range(samples)
    return Record(
        stamp_ns=T_MONO_NS + tick * 10000000, tick=tick, lost_before=lost,
        frames=frames, hold_frames=tick % 7, erle_mdb=-2500 + tick,
        echo_gain_micro=250000, coef_energy_micro=tick, ref_delay_samples=80,
        gen=3, flags=flags, samples=samples, tail_ms=48,
        noise_ddbov=1270 if tick % 2 else 600, probes=1,
        diverge_resets=0, line=line, kind=kind, nlp_verdict=verdict,
        hold_left=0, skipped=row[6] if len(row) > 6 else 0,
        stamp_us=T_WALL_US + tick * 10000,
        ref=tuple((tick * 37 + k * 11) % 4000 - 2000 for k in ks),
        mic=tuple((tick * 53 + k * 7) % 3000 - 1500 for k in ks),
        out=tuple((tick * 29 + k * 5) % 200 - 100 for k in ks))


def t_entry(rec, order, planes=None):
    head = bytearray(T_HEAD)
    for off, code, name in T_OFFSETS:
        struct.pack_into(order + code, head, off, getattr(rec, name))
    if planes is None:
        planes = T_PLANES[rec.kind]
    samples = []
    for plane in (rec.ref, rec.mic, rec.out)[:planes]:
        samples += list(plane)
    return bytes(head) + struct.pack("%s%dh" % (order, len(samples)),
                                     *samples)


def t_block(order, btype, body):
    body = body + b"\0" * (-len(body) % 4)
    size = len(body) + 12
    return (struct.pack(order + "II", btype, size) + body +
            struct.pack(order + "I", size))


def t_option(order, code, value):
    return (struct.pack(order + "HH", code, len(value)) + value +
            b"\0" * (-len(value) % 4))


def t_pcapng(order, ifaces, packets):
    """ifaces: (name, link); packets: (iface index, stamp_us, payload), and
    an original length as a fourth item when it is not the payload length.
    The block types are 0x0A0D0D0A, 1 and 6, with a name resolution block
    (4) for the reader to skip. The options are if_name 2, if_tsresol 9 and
    the end 0 (pcapng section 4)."""
    out = t_block(order, 0x0A0D0D0A,
                  struct.pack(order + "IHHq", 0x1A2B3C4D, 1, 0, -1) +
                  t_option(order, 0, b""))
    out += t_block(order, 4, b"\x00\x00\x00\x00")
    for name, link in ifaces:
        out += t_block(order, 1, struct.pack(order + "HHI", link, 0, 0) +
                       t_option(order, 2, name.encode() + b"\0") +
                       t_option(order, 9, bytes([6])) +
                       t_option(order, 0, b""))
    for packet in packets:
        iface, stamp, payload = packet[:3]
        orig = packet[3] if len(packet) > 3 else len(payload)
        out += t_block(order, 6,
                       struct.pack(order + "IIIII", iface, stamp >> 32,
                                   stamp & 0xFFFFFFFF, len(payload),
                                   orig) + payload)
    return out


def t_scenario(order, more):
    """The pcapng of the clean scenario, or of the whole scenario with each
    fault. It returns (image, records by line)."""
    rows = [(0, 80, r) for r in T_LINE0_CLEAN]
    media = list(T_MEDIA_CLEAN)
    if more:
        rows += [(0, 80, r) for r in T_LINE0_MORE]
        rows += [(1, 160, r) for r in T_LINE1]
        media += T_MEDIA_MORE
    recs = [t_record(line, samples, row) for line, samples, row in rows]
    recs.sort(key=lambda r: (r.tick, r.line))
    packets = [(1, r.stamp_us, t_entry(r, order)) for r in recs]
    packets.append((2, T_WALL_US, b"\x45" + b"\0" * 27))
    for text in T_LOG_OTHER:
        packets.append((0, T_WALL_US + 1000, text.encode()))
    for line, tick, frames, counts in media:
        text = ("[media] l%d lec nlp frames=%d " % (line, frames) +
                " ".join("%s=%d" % kv for kv in zip(T_COUNT_KEYS, counts)))
        packets.append((0, T_WALL_US + tick * 10000 + T_MEDIA_US,
                        text.encode() + b"\0"))
    if more:
        packets.append((0, T_WALL_US + 1100 * 10000, T_LOG_BAD.encode()))
        bad = t_record(0, 80, (1150, R, 1, NLP, V_QUIET, 0))
        packets.append((1, bad.stamp_us, t_entry(bad, order, planes=2)))
        bad.samples = 100
        packets.append((1, bad.stamp_us, t_entry(bad, order)))
        bad.samples, bad.kind = 80, 3
        packets.append((1, bad.stamp_us, t_entry(bad, order, planes=3)))
        bad.kind = R
        entry = bytearray(t_entry(bad, order))
        entry[62] = 1
        packets.append((1, bad.stamp_us, bytes(entry)))
    packets.sort(key=lambda p: p[1])
    image = t_pcapng(order, [T_LOG, T_LEC, ("uvoiced_rtp", 101)], packets)
    by_line = {}
    for rec in recs:
        by_line.setdefault(rec.line, []).append(rec)
    return image, by_line


def t_quiet(fn, *args):
    """Runs fn with its standard output kept; returns (value, text)."""
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        value = fn(*args)
    return value, buf.getvalue()


def t_lecr_check(check, data, line, samples, recs, ref_loss=None):
    """The replay file against the documented offsets of lec_replay.h."""
    size = 32 + len(recs) * (T_HEAD + 6 * samples)
    check(len(data) == size, "lecr size %d, not %d" % (len(data), size))
    if len(data) != size:
        return
    check(data[:8] == b"UDSPLECR", "lecr magic")
    check(struct.unpack_from("<HHIH", data, 8) ==
          (2, line, samples * 100, samples), "lecr version, line, rate")
    flags = (1 if recs[0].flags & NEW else 0) | (0 if ref_loss is None else 2)
    check(struct.unpack_from("<HII", data, 18) ==
          (ref_loss or 0, len(recs), flags),
          "lecr reference loss, count and flags")
    zeros = (0,) * samples
    for i, rec in enumerate(recs):
        off = 32 + i * (T_HEAD + 6 * samples)
        for field_off, code, name in T_OFFSETS:
            got = struct.unpack_from("<" + code, data, off + field_off)[0]
            check(got == getattr(rec, name), "lecr record %d %s" % (i, name))
        planes =struct.unpack_from("<%dh" % (3 * samples), data,
                                    off + T_HEAD)
        end = rec.kind == E
        want = ((zeros if end else rec.ref) + (zeros if end else rec.mic) +
                (rec.out if rec.kind == R else zeros))
        check(planes == want, "lecr record %d planes" % i)


def t_wav_read(path):
    with wave.open(path, "rb") as w:
        return (w.getnchannels(), w.getsampwidth(), w.getframerate(),
                array("h", w.readframes(w.getnframes())))


def t_issues(lr):
    return sorted((i.word, i.tick, i.failure) for i in lr.issues)


def t_result_key(res):
    """What the two byte orders must agree on."""
    return [(lr.line, len(lr.records), len(lr.lineages), t_issues(lr),
             lr.media.checked, lr.media.equal, lr.media.outside,
             lr.media.incomplete, len(lr.media.missing),
             len(lr.media.unequal)) for lr in res.lines] + \
        [len(res.trace.refused), len(res.trace.unparsed), res.failures]


def t_clean_check(check, tmp):
    for order in ("<", ">"):
        image, by_line = t_scenario(order, False)
        prefix = os.path.join(tmp, "clean" + ORDER_NAMES[order])
        res, text = t_quiet(capture_process, image, "clean", prefix, None)
        check(res.failures == 0, "clean %s: %d failures" %
              (ORDER_NAMES[order], res.failures))
        check(res.trace.orders == [order], "clean section order")
        check([lr.line for lr in res.lines] == [0], "clean lines")
        if not res.lines:
            return
        lr = res.lines[0]
        check(len(lr.lineages) == 1 and lr.lineages[0].ended and
              lr.lineages[0].resets == 1, "clean lineage")
        check(lr.lineages[0].skip_periods == 2 and
              lr.lineages[0].skip_marks == 1 and
              "skipped periods 2 in 1 jumps" in text,
              "a jump with SKIPPED counts the skipped periods")
        check(lr.lineages[0].tally == [1, 0, 0, 0, 0, 1],
              "clean counts at the end")
        check("media lines checked 2, equal 2;" in text,
              "clean media line summary")
        check(lr.media.outside == 1, "clean media outside")
        check(lr.media.offsets == [T_MEDIA_US] * 2, "clean media offsets")
        with open(prefix + "-l0.lecr", "rb") as f:
            t_lecr_check(check, f.read(), 0, 80, by_line[0])
        with open(prefix + "-l0.csv", newline="") as f:
            rows = list(csv.reader(f))
        check(rows[0] == CSV_HEAD and len(rows) == 1 + len(by_line[0]),
              "clean csv rows")
        if len(rows) != 1 + len(by_line[0]):
            return
        col = {name: i for i, name in enumerate(CSV_HEAD)}
        check([r[col["hold_delta"]] for r in rows[1:4]] ==
              ["0", str(1001 % 7 - 1000 % 7), str(1002 % 7 - 1001 % 7)],
              "clean csv hold_delta")
        check(rows[12][col["hold_delta"]] == "0", "csv hold_delta on RESET")
        check(rows[6][col["kind"]] == "feed" and rows[6][col["out_dbfs"]] ==
              "" and rows[6][col["verdict"]] == "", "csv FEED row")
        check(rows[14][col["ref_dbfs"]] == "", "csv END row")
        check(rows[1][col["floor_dbov"]] == "60.0" and
              rows[2][col["floor_dbov"]] == "", "csv floor")
        check(rows[4][col["active"]] == "1" and rows[3][col["active"]] ==
              "0" and rows[4][col["verdict"]] == "pass", "csv flags")
        check(rows[13][col["skipped"]] == "2" and
              rows[13][col["flags"]] == "0x1048" and
              rows[12][col["skipped"]] == "0", "csv skipped count")
        check(rows[6][col["reason"]] == "open" and
              rows[13][col["reason"]] == "" and
              rows[14][col["reason"]] == "", "csv FEED reason")
        check("FEED by reason: open 2, modem 0, off 0, inactive 0" in text,
              "summary FEED by reason")
        check(rows[2][col["rel_ms"]] == "10.000", "csv rel_ms")
        waves = [t_wav_read("%s-l0-%s.wav" % (prefix, p))
                 for p in ("ref", "mic", "out")]
        check(all(w[:3] == (1, 2, 8000) and len(w[3]) == 15 * 80
                  for w in waves), "clean wav format and length")
        feed = 5 * 80
        check(waves[2][3][feed:feed + 80] == waves[1][3][feed:feed + 80] and
              list(waves[1][3][feed:feed + 80]) == list(by_line[0][5].mic),
              "a FEED frame writes mic into out")


T_REF_LOSS = 18


def t_loss_check(check, tmp, lec_replay):
    """--ref-loss through the writer: the field and the flag of the replay
    file, the comment line of the CSV and the summary line. With lec_replay,
    the program reads the loss back, and it warns on a file without it."""
    image, by_line = t_scenario("<", False)
    prefix = os.path.join(tmp, "loss")
    res, text = t_quiet(capture_process, image, "loss", prefix, None,
                        T_REF_LOSS)
    check(res.failures == 0 and "reference loss 18 half-dB (9.0 dB)" in text,
          "--ref-loss: %d failures, the summary line" % res.failures)
    with open(prefix + "-l0.lecr", "rb") as f:
        t_lecr_check(check, f.read(), 0, 80, by_line[0], T_REF_LOSS)
    with open(prefix + "-l0.csv", newline="") as f:
        rows = f.read().splitlines()
    check(rows[:1] == [CSV_REF_LOSS % T_REF_LOSS] and
          rows[1:2] == [",".join(CSV_HEAD)] and
          len(rows) == 2 + len(by_line[0]),
          "--ref-loss: the CSV comment line %s" % rows[:1])
    if not lec_replay:
        return
    plain = os.path.join(tmp, "plain")
    t_quiet(capture_process, image, "plain", plain, None)
    for path, want, warned in ((prefix, "reference loss 18 half-dB\n", False),
                               (plain, "reference loss 0 half-dB (the file "
                                "carries none)\n", True)):
        run = subprocess.run([lec_replay, path + "-l0.lecr"],
                             capture_output=True, text=True)
        warning = "carries no reference loss, replayed at 0" in run.stderr
        check(run.returncode in (0, 1, 3) and want in run.stdout and
              warning == warned,
              "lec_replay reads the reference loss of %s: status %d, "
              "stderr %r" % (os.path.basename(path), run.returncode,
                             run.stderr.strip()))


def t_more_check(check, tmp):
    results = {}
    for order in ("<", ">"):
        image, by_line = t_scenario(order, True)
        prefix = os.path.join(tmp, "more" + ORDER_NAMES[order])
        res, text = t_quiet(capture_process, image, "more", prefix, None)
        results[order] = (res, prefix)
        check(len(res.trace.refused) == 4, "%d refused entries, not 4" %
              len(res.trace.refused))
        check(len(res.trace.unparsed) == 1, "unparsed media line")
        lines = {lr.line: lr for lr in res.lines}
        check(sorted(lines) == [0, 1], "lines of the whole scenario")
        if sorted(lines) != [0, 1]:
            return
        l0, l1 = lines[0], lines[1]
        check(t_issues(l0) == [("gap", 1205, True), ("loss", 1104, True),
                               ("open", 1206, False)],
              "line 0 issues %s" % t_issues(l0))
        check(t_issues(l1) == [("frames", 701, True), ("joined", 500, True),
                               ("open", 702, True)],
              "line 1 issues %s" % t_issues(l1))
        check([len(lin.records) for lin in l0.lineages] == [14, 5, 5],
              "line 0 lineages")
        check([lin.broken for lin in l0.lineages] == [None, 1104, 1205],
              "line 0 breaks")
        check([len(lin.records) for lin in l1.lineages] == [3, 4, 3, 2],
              "line 1 lineages")
        m0 = l0.media
        check((m0.checked, m0.equal, m0.outside, m0.incomplete,
               len(m0.missing), len(m0.unequal)) == (3, 2, 1, 1, 1, 1),
              "line 0 media %d %d %d %d %d %d" %
              (m0.checked, m0.equal, m0.outside, m0.incomplete,
               len(m0.missing), len(m0.unequal)))
        check(m0.unequal and m0.unequal[0][1].tick == 1006,
              "the unequal media line meets tick 1006")
        m1 = l1.media
        check((m1.checked, m1.equal, m1.incomplete) == (1, 1, 1),
              "line 1 media")
        check("media lines checked 3, equal 2;" in text, "media summary")
        check(res.failures == 4 + 1 + 2 + 3 + 1 + 1,
              "failures %d" % res.failures)
        waves = [t_wav_read("%s-l0-%s.wav" % (prefix, p))
                 for p in ("ref", "mic", "out")]
        check(all(len(w[3]) == 207 * 80 for w in waves), "line 0 wav length")
        gap = (1102 - 1000) * 80
        check(all(not any(w[3][gap:gap + 160]) for w in waves),
              "a loss writes zeros")
        wave1 = t_wav_read("%s-l1-mic.wav" % prefix)
        check(wave1[2] == 16000 and len(wave1[3]) == 301 * 160,
              "line 1 wav rate and length")
        with open(prefix + "-l1.lecr", "rb") as f:
            t_lecr_check(check, f.read(), 1, 160, by_line[1])
    names = ("lecr", "csv", "ref.wav", "mic.wav", "out.wav")
    a, b = results["<"], results[">"]
    check(t_result_key(a[0]) == t_result_key(b[0]),
          "the byte orders give other results")
    for line in (0, 1):
        for name in names:
            sep = "." if name in ("lecr", "csv") else "-"
            with open("%s-l%d%s%s" % (a[1], line, sep, name), "rb") as f:
                one = f.read()
            with open("%s-l%d%s%s" % (b[1], line, sep, name), "rb") as f:
                two = f.read()
            check(one == two, "the byte orders write other %s" % name)
    return results["<"][1]


# The scenarios of the jump and loss rules on line 0 at 8000 Hz. Each has a
# name, rows, the issues of the checks and the skipped periods they count.
T_RULES = (
    ("a skip", [
        (100, F, 0, NEW | SO, V_OFF, 0), (101, R, 1, NLP, V_QUIET, 0),
        (105, R, 2, NLP | SKP, V_QUIET, 0, 3), (106, E, 2, 0, V_OFF, 0)],
     [], 3),
    ("a skip beside a FEED lost from the archive", [
        (100, F, 0, NEW | SO, V_OFF, 0), (101, R, 1, NLP, V_QUIET, 0),
        (106, R, 2, NLP | SKP, V_QUIET, 0, 3), (107, E, 2, 0, V_OFF, 0)],
     [("gap", 106, True)], 0),
    ("a skip one period short of its jump", [
        (100, F, 0, NEW | SO, V_OFF, 0), (101, R, 1, NLP, V_QUIET, 0),
        (105, R, 2, NLP | SKP, V_QUIET, 0, 2), (106, E, 2, 0, V_OFF, 0)],
     [("gap", 105, True)], 0),
    ("a skip and a loss in one jump", [
        (100, F, 0, NEW | SO, V_OFF, 0), (101, R, 1, NLP, V_QUIET, 0),
        (106, R, 3, NLP | SKP, V_QUIET, 1, 3), (107, E, 3, 0, V_OFF, 0)],
     [("loss", 106, True)], 0),
    ("a lost END", [
        (100, F, 0, NEW | SO, V_OFF, 0), (101, R, 1, NLP, V_QUIET, 0),
        (102, R, 2, NLP, V_QUIET, 0), (200, F, 0, NEW | SI, V_OFF, 1),
        (201, R, 1, NLP, V_QUIET, 0), (202, E, 1, 0, V_OFF, 0)],
     [("loss", 200, True)], 0),
    ("a lost final RUN", [
        (100, R, 1, NEW | NLP, V_QUIET, 0), (102, E, 2, 0, V_OFF, 0)],
     [("gap", 102, True)], 0),
    ("a final RUN lost in the ring", [
        (100, R, 1, NEW | NLP, V_QUIET, 0), (102, E, 2, 0, V_OFF, 1)],
     [("loss", 102, True)], 0),
)
# Each row breaks one flag rule of udsp.h, so the reader refuses it.
T_REFUSED = (
    (300, F, 0, 0, V_OFF, 0),
    (300, F, 0, SO | SM, V_OFF, 0),
    (300, R, 1, NLP | SI, V_QUIET, 0),
    (300, E, 1, SF, V_OFF, 0),
    (300, R, 1, NLP | SO | SM, V_QUIET, 0),
    (300, E, 1, SO | SM, V_OFF, 0),
    (300, R, 1, NLP | SKP, V_QUIET, 0),
    (300, R, 1, NLP, V_QUIET, 0, 2),
    (300, R, 1, NEW | NLP | SKP, V_QUIET, 0, 2),
    (300, E, 1, SKP, V_OFF, 0, 2),
)
# The time axis of the WAV files: rows, and the frames of each file. Two
# instances, and a joined record after a first END.
T_AXIS = (
    ("two instances", [
        (100, R, 1, NEW | NLP, V_QUIET, 0), (101, E, 1, 0, V_OFF, 0),
        (200, R, 1, NEW | NLP, V_QUIET, 0), (201, E, 1, 0, V_OFF, 0)], 101),
    ("a joined record after a first END", [
        (100, E, 5, 0, V_OFF, 0), (103, R, 6, NLP, V_QUIET, 0),
        (104, E, 6, 0, V_OFF, 0), (120, R, 1, NEW | NLP, V_QUIET, 0),
        (121, F, 1, SO, V_OFF, 0), (122, E, 1, 0, V_OFF, 0)], 22),
)


def t_rows_image(rows):
    recs = [t_record(0, 80, row) for row in rows]
    return t_pcapng("<", [T_LEC], [(0, r.stamp_us, t_entry(r, "<"))
                                   for r in recs])


def t_rule_check(check, tmp):
    prefix = os.path.join(tmp, "rule")
    for name, rows, issues, periods in T_RULES:
        res, _ = t_quiet(capture_process, t_rows_image(rows), name, prefix,
                         None)
        lr = res.lines[0] if res.lines else None
        check(lr is not None and t_issues(lr) == issues,
              "%s: issues %s" % (name, t_issues(lr) if lr else None))
        check(lr is not None and
              sum(lin.skip_periods for lin in lr.lineages) == periods,
              "%s: skipped periods" % name)
    res, _ = t_quiet(capture_process, t_rows_image(T_RULES[4][1]), "end",
                     prefix, None)
    lins = res.lines[0].lineages if res.lines else []
    check(len(lins) == 2 and lins[0].broken == 200 and lins[1].ended,
          "a lost END: the loss breaks the lineage before the NEW")
    res, _ = t_quiet(capture_process, t_rows_image(T_REFUSED), "refused",
                     prefix, None)
    reasons = [reason for _, reason in res.trace.refused]
    check(len(reasons) == len(T_REFUSED) and not res.trace.records,
          "flag rules: %d refused of %d" % (len(reasons), len(T_REFUSED)))
    check(all("reason bits" in r or "skipped" in r for r in reasons),
          "flag rules: refusal reasons %s" % reasons)
    rec = t_record(0, 80, T_RULES[0][1][1])
    entry = t_entry(rec, "<")
    image = t_pcapng("<", [T_LEC], [(0, rec.stamp_us, entry),
                                    (0, rec.stamp_us, entry, len(entry) + 4)])
    res, _ = t_quiet(capture_process, image, "cut", prefix, None)
    reasons = [reason for _, reason in res.trace.refused]
    check(len(res.trace.records) == 1 and len(reasons) == 1 and
          "captured length" in reasons[0],
          "an entry cut short of its original length: refused %s" % reasons)


@contextlib.contextmanager
def t_env(name, value):
    """The variable set for the body, and restored after it."""
    old = os.environ.get(name)
    os.environ[name] = value
    try:
        yield
    finally:
        if old is None:
            del os.environ[name]
        else:
            os.environ[name] = old


def t_residual_frames(lec_replay, lecr, wav, env=None):
    """Runs lec_replay --residual on a file of 80 samples; returns (status,
    stderr, tick frames of the WAV file or None when no file stands)."""
    run = subprocess.run([lec_replay, lecr, "--residual", wav],
                         capture_output=True, text=True,
                         env=dict(os.environ, **(env or {})))
    if not os.path.exists(wav):
        return run.returncode, run.stderr, None
    with wave.open(wav, "rb") as w:
        return run.returncode, run.stderr, w.getnframes() / 80


def t_axis_check(check, tmp, lec_replay):
    """The time axis of the WAV files in both tools. Then the data limit of a
    WAV file, with output just inside it and one byte over it."""
    prefix = os.path.join(tmp, "axis")
    for name, rows, frames in T_AXIS:
        res, _ = t_quiet(capture_process, t_rows_image(rows), name, prefix,
                         None)
        waves = [t_wav_read("%s-l0-%s.wav" % (prefix, p))
                 for p in ("ref", "mic", "out")]
        check(all(len(w[3]) == frames * 80 for w in waves),
              "%s: WAV frames %s, not %d" %
              (name, [len(w[3]) // 80 for w in waves], frames))
        if not lec_replay:
            continue
        _, _, got = t_residual_frames(lec_replay, prefix + "-l0.lecr",
                                      prefix + "-res.wav")
        check(got == frames, "%s: residual frames %s, not %d" %
              (name, got, frames))
    image = t_rows_image(T_AXIS[0][1])
    need = T_AXIS[0][2] * 80 * 2
    for limit, fits in ((need, True), (need - 1, False)):
        prefix = os.path.join(tmp, "limit%d" % limit)
        with t_env("UDSP_TEST_WAV_DATA_MAX", str(limit)):
            try:
                t_quiet(capture_process, image, "limit", prefix, None)
                wrote = True
            except CaptureError:
                wrote = False
        check(wrote == fits and
              os.path.exists(prefix + "-l0-mic.wav") == fits and
              os.path.exists(prefix + "-l0.lecr") == fits,
              "the WAV limit at %d bytes: written %s" % (limit, wrote))
    if not lec_replay:
        return
    lecr = os.path.join(tmp, "limit%d-l0.lecr" % need)
    for limit, fits in ((need, True), (need - 1, False)):
        wav = os.path.join(tmp, "limit%d-res.wav" % limit)
        status, err, got = t_residual_frames(
            lec_replay, lecr, wav, {"UDSP_TEST_WAV_DATA_MAX": str(limit)})
        check((got == T_AXIS[0][2]) if fits else
              (status == 2 and got is None and "data bytes" in err),
              "the residual limit at %d bytes: status %d, frames %s" %
              (limit, status, got))


def t_source_check(check, tmp):
    image, _ = t_scenario(">", False)
    top = os.path.join(tmp, "archive")
    os.mkdir(top)
    with open(os.path.join(top, "rings.pcapng"), "wb") as f:
        f.write(image)
    tar_bytes = io.BytesIO()
    with tarfile.open(fileobj=tar_bytes, mode="w") as tar:
        info = tarfile.TarInfo("20260930-000000-line0-test/rings.pcapng")
        info.size = len(image)
        tar.addfile(info, io.BytesIO(image))
    path = os.path.join(tmp, "archive.tar.gz")
    with open(path, "wb") as f:
        f.write(gzip.compress(tar_bytes.getvalue()))
    outputs = []
    for source, name in ((top, "dir"), (path, "tgz"),
                         (os.path.join(top, "rings.pcapng"), "file")):
        prefix = os.path.join(tmp, name)
        status, _ = t_quiet(capture_run, source, prefix, 0)
        check(status == 0, "source %s: status %d" % (name, status))
        if os.path.exists(prefix + "-l0.lecr"):
            with open(prefix + "-l0.lecr", "rb") as f:
                outputs.append(f.read())
    check(len(outputs) == 3 and outputs[0] == outputs[1] == outputs[2],
          "the three sources differ")
    status, text = t_quiet(capture_run, path, os.path.join(tmp, "x"), 5)
    check(status == 1 and "line 5: no canceller record" in text,
          "a line with no record fails")
    image = t_pcapng("<", [T_LOG], [(0, T_WALL_US, T_LOG_OTHER[0].encode())])
    res, text = t_quiet(capture_process, image, "nolec", tmp, None)
    check(res.failures == 1 and "no canceller ring" in text,
          "a trace with no canceller ring fails")
    res, text = t_quiet(capture_process, image[:-8], "cut", tmp, None)
    check(res.trace.problems and res.failures == 2, "a truncated block")
    cut = os.path.join(tmp, "cut.tar.gz")
    with open(path, "rb") as f, open(cut, "wb") as g:
        g.write(f.read()[:-32])
    try:
        t_quiet(capture_run, cut, os.path.join(tmp, "cut"), 0)
        text = "no error"
    except CaptureError as e:
        text = str(e)
    check("truncated" in text, "a truncated archive: %s" % text)


T_FRAMES_HEAD = (
    "tick,frames,verdict,nlp_active,fired,dt,hold_left,probing,trial_arm,"
    "trial_end,probes,diverge_resets,dt_run,stale_run,trusted,classic_arm,"
    "erle_db,echo_gain,coef_energy,noise_dbov,bar,ref_power,window_power,"
    "mic_power,estimate_power,out_power,talker,ref_bar,ref_bar_max,"
    "background,ref_floor,trust_run")

# The census call: one line at 8 kHz with a 48 ms tail, whose reference window
# holds 5 frames. The echo is the reference 40 samples late at -12 dB. A run
# stretch is ("run", RUN frames, far dBFS, near dBFS, floor dBFS, processor
# on, near period), and an eighth item gives another echo gain. None is a
# silent plane. The near end sounds on the first frame of each period. "feed"
# adds FEED records with a far end, "reset" marks the next RUN record RESET,
# and "new" ends the instance and opens the next one. The first RUN record has
# index 1, so a stretch of 100 frames from index 100k is second k.
#
# The near end first sounds at index T_CENSUS_NEAR. The talker of the indexes
# in T_CENSUS_TALKER stands 16 dB over the far end, so the talker guard of
# dsp/lec.c reads it and starts no trial. The echo that amplifies by 8 dB
# from index 1900 holds the detector, and its latch trial passes the
# watchdog bound.
T_CENSUS_SAMPLES = 80
T_CENSUS_WINDOW = 5
T_CENSUS_DELAY = 40
T_CENSUS_GAIN = 0.25
T_CENSUS_OUT = 30000
T_CENSUS_FS = 32768.0
T_CENSUS_RANGE = (1103, 3050)
T_CENSUS_RESET = 1100
T_CENSUS_NEW = 3150
T_CENSUS_NEAR = 300
T_CENSUS_TALKER = (1300, 1900)
T_CENSUS_CALL = (
    ("run", 49, None, None, -62, True, 1),
    ("feed", 5, None),
    ("run", 50, None, None, -62, True, 1),
    ("run", 50, -20, None, -62, True, 1),
    ("feed", 3, -20),
    ("run", 150, -20, None, -62, True, 1),
    ("run", 100, -20, -20, -62, True, 1),
    ("run", 100, -20, -20, -62, False, 1),
    ("run", 100, -20, -32, -62, True, 20),
    ("run", 94, -20, None, -75, True, 1),
    ("run", 6, None, None, -75, True, 1),
    ("run", 49, None, None, -75, True, 1),
    ("run", 2, -20, None, -75, True, 1),
    ("run", 49, None, None, -75, True, 1),
    ("run", 100, -46, None, -75, True, 1),
    ("run", 100, -20, -14, -75, True, 1),
    ("run", 100, -20, None, -75, True, 1),
    ("reset",),
    ("run", 10, None, None, -75, True, 1),
    ("run", 50, -20, -12.5, -75, True, 1),
    ("run", 140, -20, None, -75, True, 1),
    ("run", 600, -26, -10, -75, True, 1),
    ("run", 600, -20, None, -62, True, 1, 2.5),
    ("run", 100, None, None, -62, True, 1),
    ("run", 400, -20, None, -75, True, 1),
    ("run", 100, -20, -20, -62, True, 1),
    ("run", 50, -20, None, -62, True, 1),
    ("new",),
    ("run", 20, None, None, -62, True, 1),
    ("run", 80, -20, None, -62, True, 1),
)


def t_census_noise(state, dbfs):
    """One frame of uniform noise at dbfs from the xorshift32 state (a list of
    one value), or silence for None."""
    if dbfs is None:
        return [0.0] * T_CENSUS_SAMPLES
    peak = T_CENSUS_FS * 10.0 ** (dbfs / 20.0) * math.sqrt(3.0)
    out = []
    x = state[0]
    for _ in range(T_CENSUS_SAMPLES):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        out.append(peak * (x / 2147483648.0 - 1.0))
    state[0] = x
    return out


class TCensusLine:
    """The census call while it is written. It keeps the tick, the frames of
    the instance and the late reference of the echo. It also keeps the
    reference powers of the window."""

    def __init__(self):
        self.recs = []
        self.tick = 10000
        self.frames = 0
        self.flags = FLAG_NEW
        self.late = [0] * T_CENSUS_DELAY
        self.window = []
        self.state = [0x2545F491]
        self.refs = []
        self.windows = []

    def planes(self, far, near, floor, gain=T_CENSUS_GAIN):
        ref = [int(round(v)) for v in t_census_noise(self.state, far)]
        full = self.late + ref
        self.late = full[T_CENSUS_SAMPLES:]
        sound = zip(t_census_noise(self.state, near),
                    t_census_noise(self.state, floor))
        mic = [max(-32768, min(32767, int(round(gain * full[k] + a + b))))
               for k, (a, b) in enumerate(sound)]
        self.window = (self.window + [sum(v * v for v in ref) /
                                      T_CENSUS_SAMPLES])[-T_CENSUS_WINDOW:]
        return tuple(ref), tuple(mic)

    def add(self, kind, flags, ref=None, mic=None):
        self.recs.append(Record(
            stamp_ns=self.tick * 10000000, tick=self.tick, frames=self.frames,
            flags=flags, samples=T_CENSUS_SAMPLES, tail_ms=48, kind=kind,
            ref=ref, mic=mic, out=(T_CENSUS_OUT,) * T_CENSUS_SAMPLES))
        self.tick += 1

    def run(self, far, near, floor, nlp, near_now, gain):
        ref, mic = self.planes(far, near if near_now else None, floor, gain)
        self.frames = 1 if self.flags & (FLAG_NEW | FLAG_RESET) else \
            self.frames + 1
        self.add(KIND_RUN, self.flags | (1 << 3 if nlp else 0), ref, mic)
        self.flags = 0
        self.refs.append(sum(v * v for v in ref) / T_CENSUS_SAMPLES)
        self.windows.append(max(self.window))

    def item(self, item):
        if item[0] == "feed":
            for _ in range(item[1]):
                ref, mic = self.planes(item[2], None, None)
                self.add(KIND_FEED, REASON_BITS[0], ref, mic)
        elif item[0] == "reset":
            self.flags = FLAG_RESET
            self.window = []
        elif item[0] == "new":
            self.add(KIND_END, 0)
            self.tick += 1000
            self.flags = FLAG_NEW
            self.late = [0] * T_CENSUS_DELAY
            self.window = []
        else:
            _, count, far, near, floor, nlp, period = item[:7]
            gain = item[7] if len(item) > 7 else T_CENSUS_GAIN
            for k in range(count):
                self.run(far, near, floor, nlp, k % period == 0, gain)


def t_census_call():
    line = TCensusLine()
    for item in T_CENSUS_CALL:
        line.item(item)
    line.add(KIND_END, 0)
    return line


def t_census_marks(refs, windows, news):
    """The far frames and the guard windows of the RUN frames, by the terms
    of lec_replay.c, from the reference and the window powers. The history
    restarts at the indexes in news and runs on through a RESET."""
    bar = T_CENSUS_FS * T_CENSUS_FS * math.pow(10.0, -40.0 / 10.0)
    out = []
    last = []
    window_last = 0.0
    for i, (ref, win) in enumerate(zip(refs, windows), 1):
        if i in news:
            last, window_last = [], 0.0
        guard = max(win, window_last)
        out.append((max([ref] + last[-6:]) >= bar, guard))
        last.append(ref)
        window_last = win
    return out


def t_census_db(num, den):
    """10 log10(num / den) as lec_replay.c prints it, and its value."""
    if not num > 0.0 and not den > 0.0:
        value = float("nan")
    elif den == 0.0:
        value = float("inf")
    elif num == 0.0:
        value = float("-inf")
    else:
        value = 10.0 * math.log10(num / den)
    return "%.2f" % value, value


class TCensusSel:
    def __init__(self):
        self.n = dict.fromkeys(("frames",) + VERDICT_NAMES +
                               ("nlp_active", "dt_flag", "fired", "leak",
                                "exposure"), 0)
        self.out_sum = 0.0
        self.out_max = 0.0
        self.mic_sum = 0.0

    def add(self, row, leak, exposure):
        for key, on in (("frames", True), (row["verdict"], True),
                        ("nlp_active", row["nlp_active"] == "1"),
                        ("dt_flag", row["dt"] == "1"),
                        ("fired", row["fired"] == "1"), ("leak", leak),
                        ("exposure", exposure)):
            self.n[key] += 1 if on else 0
        self.out_sum += float(row["out_power"])
        self.mic_sum += float(row["mic_power"])
        self.out_max = max(self.out_max, float(row["out_power"]))

    def text(self, keys):
        fs2 = T_CENSUS_FS * T_CENSUS_FS
        extra = {
            "out_sum": "%.9g" % self.out_sum,
            "out_max": "%.9g" % self.out_max,
            "out_max_dbfs": t_census_db(self.out_max, fs2)[0],
            "mic_sum": "%.9g" % self.mic_sum,
            "out_mic_db": t_census_db(self.out_sum, self.mic_sum)[0],
        }
        return {k: str(self.n[k]) if k in self.n else extra[k] for k in keys}


T_CENSUS_SEL_KEYS = ("frames",) + VERDICT_NAMES + ("nlp_active", "dt_flag",
                                                   "fired", "leak",
                                                   "exposure")


def t_census_count(rows, news, first, end):
    """The census lines of the range from the rows of --frames, as
    {(kind, name): {key: text}}, and the seconds as (second, far, dt, loss)."""
    fs2 = T_CENSUS_FS * T_CENSUS_FS
    leak_bar = fs2 * math.pow(10.0, -65.0 / 10.0)
    far_bar = fs2 * math.pow(10.0, -40.0 / 10.0)
    expose_bar = fs2 * math.pow(10.0, -60.0 / 10.0)
    marks = t_census_marks([float(r["ref_power"]) for r in rows],
                           [float(r["window_power"]) for r in rows], news)
    sel = {name: TCensusSel() for name in ("all", "far", "low", "near",
                                           "far_not_near")}
    seconds = {}
    for i, (row, (far, guard)) in enumerate(zip(rows, marks), 1):
        if i < first or (end and i >= end):
            continue
        mic = float(row["mic_power"])
        factor = min(2.0 * float(row["echo_gain"]), 0.25)
        near = mic >= 16.0 * guard * factor
        low = guard < far_bar
        leak = far and float(row["out_power"]) > leak_bar
        exposure = (low and mic >= expose_bar and
                    float(row["estimate_power"]) >= mic / 10.0)
        for name, on in (("all", True), ("far", far), ("low", low),
                         ("near", near), ("far_not_near", far and not near)):
            if on:
                sel[name].add(row, leak, exposure)
        sec = seconds.setdefault(i // 100, [0, 0, 0.0, 0.0])
        sec[1] += row["verdict"] == "dt"
        if far:
            sec[0] += 1
            sec[2] += mic
            sec[3] += float(row["out_power"])
    runs = sel["all"].n["frames"]
    lines = {("range", "%d:%s" % (first, end or "")): {"frames": str(runs)}}
    for name in ("all", "low"):
        lines[("sel", name)] = sel[name].text(T_CENSUS_SEL_KEYS)
    lines[("sel", "far")] = sel["far"].text(
        T_CENSUS_SEL_KEYS + ("out_sum", "out_max", "out_max_dbfs"))
    lines[("sel", "far")]["leak_per_min"] = "%.2f" % (
        sel["far"].n["leak"] * 6000.0 / runs if runs else 0.0)
    lines[("sel", "near")] = sel["near"].text(
        T_CENSUS_SEL_KEYS + ("out_sum", "mic_sum", "out_mic_db"))
    lines[("sel", "far_not_near")] = sel["far_not_near"].text(
        ("frames", "nlp_active", "leak", "out_sum", "out_max",
         "out_max_dbfs"))
    kept = []
    for second, (far, dt, mic_sum, out_sum) in seconds.items():
        if far < 10:
            continue
        loss_text, loss = t_census_db(mic_sum, out_sum)
        lines[("second", str(second))] = {"far": str(far), "dt": str(dt),
                                          "loss_db": loss_text}
        kept.append((second, far, dt, loss))
    lockout = [str(s) for s, far, dt, loss in kept
               if 3 <= s <= 29 and dt >= 30 and loss < 8.0]
    lines[("lockout", str(len(lockout)))] = {
        "seconds": ",".join(lockout) if lockout else "-"}
    return lines, kept


def t_census_parse(text):
    """The census lines of a report, in the form of t_census_count."""
    lines = {}
    for line in text.splitlines():
        words = line.split()
        if not words or words[0] != "census" or len(words) % 2 == 0:
            continue
        pairs = list(zip(words[1::2], words[2::2]))
        lines[pairs[0]] = dict(pairs[1:])
    return lines


def t_census_diff(want, got):
    """The first keys where two census line sets differ, for a message."""
    out = []
    for line in sorted(set(want) | set(got)):
        a, b = want.get(line), got.get(line)
        if a is None or b is None:
            out.append("%s %s: %s against %s" % (line + (a, b)))
            continue
        out += ["%s %s %s: %s against %s" % (line + (k, a.get(k), b.get(k)))
                for k in sorted(set(a) | set(b)) if a.get(k) != b.get(k)]
    return out[:4]


def t_census_rows(lec_replay, lecr, frames, extra):
    """Runs lec_replay with --frames; returns (status, stdout, rows)."""
    run = subprocess.run([lec_replay, lecr, "--frames", frames] + extra,
                         capture_output=True, text=True)
    rows = []
    if os.path.exists(frames):
        with open(frames, newline="") as f:
            rows = list(csv.DictReader(f))
    return run.returncode, run.stdout, rows


def t_census_terms(line, first, end):
    """The far frames and the low-reference frames of the range that the
    planes of the call fix, and its RUN frames."""
    bar = T_CENSUS_FS * T_CENSUS_FS * math.pow(10.0, -40.0 / 10.0)
    marks = t_census_marks(line.refs, line.windows, {1, T_CENSUS_NEW})
    inside = [m for i, m in enumerate(marks, 1)
              if i >= first and (not end or i < end)]
    return (sum(1 for far, guard in inside if far),
            sum(1 for far, guard in inside if guard < bar), len(inside))


def t_census_steps(rows, field, hit):
    """True when hit(row) holds exactly on the rows where field steps up.
    The counters restart at a NEW."""
    last = 0
    for i, row in enumerate(rows, 1):
        if i == T_CENSUS_NEW:
            last = 0
        step = int(row[field]) > last
        last = int(row[field])
        if step != hit(row):
            return False
    return True


def t_census_values(check, rows):
    """Row values that the planes of the call fix."""
    plane = float(T_CENSUS_OUT) * T_CENSUS_OUT
    check(len(rows) >= T_CENSUS_NEW and
          all(r["out_power"] == r["mic_power"] and float(r["ref_power"]) == 0
              for r in rows[:99]),
          "census call: out_power equals mic_power before any echo")
    check(rows and all(float(r["out_power"]) < plane / 2 for r in rows),
          "census call: out_power is the output of the replay, not the out "
          "plane of the file")
    check(any(r["trial_arm"] for r in rows) and
          t_census_steps(rows, "probes", lambda r: r["trial_arm"] != ""),
          "census call: trial_arm reads exactly where probes steps")
    check(any(r["trial_end"] for r in rows) and
          t_census_steps(rows, "diverge_resets",
                         lambda r: r["trial_end"] == "diverged"),
          "census call: trial_end reads diverged exactly where "
          "diverge_resets steps")
    first, end = T_CENSUS_TALKER
    loud = [(r["talker"] == "1", float(r["mic_power"]) > 16.0 *
             max(float(r["window_power"]), float(p["window_power"])))
            for p, r in zip(rows[first - 2:end - 2], rows[first - 1:end - 1])]
    check(len(rows) >= end and
          all(r["talker"] == "0" for r in rows[:T_CENSUS_NEAR - 1]) and
          all(a == b for a, b in loud) and
          sum(a for a, b in loud) * 10 >= (end - first) * 9,
          "census call: talker reads 0 before the near end, and 1 exactly "
          "where the microphone stands 16 times over the guard window")
    check(len(rows) >= T_CENSUS_NEW and
          rows[T_CENSUS_RESET - 2]["frames"] == str(T_CENSUS_RESET - 1) and
          rows[T_CENSUS_RESET - 1]["frames"] == "1" and
          rows[T_CENSUS_NEW - 1]["frames"] == "1",
          "census call: frames restarts at the RESET and the NEW")


def t_census_cover(check, rows, kept):
    """The call holds each class that the census tells apart."""
    fs2 = T_CENSUS_FS * T_CENSUS_FS
    marks = t_census_marks([float(r["ref_power"]) for r in rows],
                           [float(r["window_power"]) for r in rows],
                           {1, T_CENSUS_NEW})
    outs = [float(r["out_power"]) for r, (far, g) in zip(rows, marks) if far]
    bars = [fs2 * math.pow(10.0, db / 10.0) for db in (-65.0, -60.0)]
    check(any(o <= bars[0] for o in outs) and
          any(bars[0] < o <= bars[1] for o in outs) and
          any(o > bars[1] for o in outs),
          "census call: far frames on both sides of -65 and -60 dBFS")
    wins = [float(r["window_power"]) for r in rows]
    check(any(w < fs2 * 1e-4 <= p for w, p in zip(wins[1:], wins)),
          "census call: a frame low by its own window and not by the "
          "frame before")
    by = {s: (far, dt, loss) for s, far, dt, loss in kept}
    check(by.get(3, (0, 0, 99.0))[1] >= 30 and by[3][2] < 8.0 and
          by.get(4, (0, 99, 0.0))[1] < 30 and by[4][2] < 8.0 and
          by.get(5, (0, 0, 0.0))[1] >= 30 and by[5][2] >= 8.0 and
          7 not in by and by.get(30, (0, 0, 99.0))[1] >= 30 and
          by[30][2] < 8.0,
          "census call: a lockout second, and seconds that miss one "
          "condition each, %s" % sorted(by.items())[:8])


def t_census_check(check, tmp, lec_replay):
    """The frame file and the census of lec_replay against a recount from the
    frame file, and against the planes of a synthetic call."""
    line = t_census_call()
    lecr = os.path.join(tmp, "census.lecr")
    with open(lecr, "wb") as f:
        f.write(lecr_encode(0, T_CENSUS_SAMPLES, line.recs))
    frames = os.path.join(tmp, "census-frames.csv")
    status, text, rows = t_census_rows(lec_replay, lecr, frames, [])
    news = {1, T_CENSUS_NEW}
    check(status in (0, 1) and len(rows) == len(line.refs),
          "census call: status %d, %d rows for %d RUN frames" %
          (status, len(rows), len(line.refs)))
    for first, end in ((1, 0), T_CENSUS_RANGE):
        if end:
            status, text, _ = t_census_rows(lec_replay, lecr, frames + "2",
                                            ["--range", "%d:%d" % (first,
                                                                   end)])
            check(status in (0, 1), "census call %d:%d: status %d" %
                  (first, end, status))
        want, kept = t_census_count(rows, news, first, end)
        got = t_census_parse(text)
        check(want == got, "census call %d:%s: the recount differs: %s" %
              (first, end or "", t_census_diff(want, got)))
        far, low, runs = t_census_terms(line, first, end)
        span = "%d:%s" % (first, end or "")
        check(got.get(("range", span)) == {"frames": str(runs)},
              "census call %s: the RUN frames of the range" % span)
        check(got.get(("sel", "far"), {}).get("frames") == str(far) and
              got.get(("sel", "low"), {}).get("frames") == str(low),
              "census call %s: far %d and low %d frames by the planes" %
              (span, far, low))
        if not end:
            t_census_cover(check, rows, kept)
    t_census_values(check, rows)
    for args in (["--range", "5:5"],
                 ["--frames", os.path.join(tmp, "none", "x.csv")]):
        run = subprocess.run([lec_replay, lecr] + args, capture_output=True,
                             text=True)
        check(run.returncode == 2, "lec_replay %s: status %d" %
              (" ".join(args), run.returncode))


def t_replay_check(check, lec_replay, prefix):
    if not lec_replay:
        print("lec-capture self-test: no --lec-replay, the lec_replay step "
              "is skipped")
        return
    rule = os.path.join(os.path.dirname(prefix), "final")
    t_quiet(capture_process, t_rows_image(T_RULES[5][1]), "final", rule, None)
    run = subprocess.run([lec_replay, rule + "-l0.lecr"],
                         capture_output=True, text=True)
    check(run.returncode == 1 and " 1 gaps," in run.stdout,
          "lec_replay on a lost final RUN: status %d" % run.returncode)
    run = subprocess.run([lec_replay, prefix + "-l0.lecr"],
                         capture_output=True, text=True)
    check(run.returncode == 1, "lec_replay status %d" % run.returncode)
    check("records 24, instances 3, resets 1, losses 1 (2 records)" in
          run.stdout, "lec_replay header")
    check(" 1 gaps," in run.stdout and "error:" not in run.stdout,
          "lec_replay gaps")
    run = subprocess.run([lec_replay, prefix + "-l1.lecr"],
                         capture_output=True, text=True)
    check(run.returncode == 3 and "records 12, instances 3, resets 0" in
          run.stdout and "the first record is not NEW" in run.stdout,
          "lec_replay on the joined line: status %d" % run.returncode)
    head = T_FRAMES_HEAD
    frames = prefix + "-frames.csv"
    run = subprocess.run([lec_replay, prefix + "-l0.lecr", "--frames",
                          frames], capture_output=True, text=True)
    lines = []
    if os.path.exists(frames):
        with open(frames) as f:
            lines = f.read().splitlines()
    replayed = re.search(r" (\d+) RUN frames replayed,", run.stdout)
    runs = int(replayed.group(1)) if replayed else -1
    check(run.returncode == 1 and lines[:1] == [head],
          "lec_replay --frames: status %d, header %s" %
          (run.returncode, lines[:1]))
    check(runs > 0 and len(lines) == runs + 1 and
          all(len(x.split(",")) == len(head.split(",")) for x in lines),
          "lec_replay --frames: %d rows for %d RUN frames" %
          (len(lines) - 1, runs))
    check(("census range 1: frames %d\n" % runs) in run.stdout and
          "\ncensus sel far " in run.stdout and
          "\ncensus sel far_not_near " in run.stdout and
          "\ncensus lockout " in run.stdout,
          "lec_replay census lines")
    run = subprocess.run([lec_replay, prefix + "-l0.lecr", "--range", "2:4"],
                         capture_output=True, text=True)
    check(run.returncode == 1 and
          ("census range 2:4 frames %d\n" % max(0, min(4, runs + 1) - 2))
          in run.stdout,
          "lec_replay --range: status %d" % run.returncode)
    print("lec-capture self-test: lec_replay read the replay files")


def self_test(lec_replay):
    """The parse, the checks and the outputs on synthetic sections in both
    byte orders, then the replay files through lec_replay when given."""
    failed = []
    count = [0]

    def check(ok, what):
        count[0] += 1
        if not ok:
            failed.append(what)

    with tempfile.TemporaryDirectory(prefix="lec-capture-") as tmp:
        t_clean_check(check, tmp)
        t_loss_check(check, tmp, lec_replay)
        prefix = t_more_check(check, tmp)
        t_rule_check(check, tmp)
        t_source_check(check, tmp)
        t_axis_check(check, tmp, lec_replay)
        if prefix:
            t_replay_check(check, lec_replay, prefix)
        if lec_replay:
            t_census_check(check, tmp, lec_replay)
    for what in failed:
        print("  fail: %s" % what)
    print("lec-capture self-test: %s (%d checks, %d failed)" %
          ("ok" if not failed else "FAILED", count[0], len(failed)))
    return 0 if not failed else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("source", nargs="?",
                        help="rings.pcapng, an archive directory or a "
                        "tar.gz archive")
    parser.add_argument("prefix", nargs="?",
                        help="the output prefix: <prefix>-l<N>.lecr and so on")
    parser.add_argument("--line", type=int, metavar="N",
                        help="extract line N only")
    parser.add_argument("--ref-loss", type=int, metavar="N",
                        help="the reference loss of the capture in half-dB "
                        "steps, written into each replay file")
    parser.add_argument("--self-test", action="store_true",
                        help="check the tool on synthetic sections")
    parser.add_argument("--lec-replay", metavar="PATH",
                        help="with --self-test, the lec_replay binary to run")
    parser.add_argument("--write-fixture", metavar="PATH",
                        help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        if args.self_test:
            return self_test(args.lec_replay)
        if args.write_fixture:
            return fixture_write(args.write_fixture)
        if not args.source or not args.prefix:
            parser.error("a source and an output prefix are required")
        if args.line is not None and not 0 <= args.line <= 255:
            parser.error("the line must be 0 to 255")
        if args.ref_loss is not None and \
                not 0 <= args.ref_loss <= REF_LOSS_MAX:
            parser.error("the reference loss must be 0 to %d" % REF_LOSS_MAX)
        return capture_run(args.source, args.prefix, args.line,
                           args.ref_loss)
    except CaptureError as e:
        print("lec-capture: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
