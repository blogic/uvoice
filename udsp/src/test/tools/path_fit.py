#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-only
#
# udsp - software voice DSP for FXS lines
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
"""Fit the echo path of a call trace or of a canceller capture.

usage: path_fit.py <rings.pcapng> <onset_ms> <out.txt>
       path_fit.py --capture <prefix>-l<N> [--skip-s S] [--loop-ref N]
                   [--ref-loss N]
       path_fit.py --self-test

The call trace mode fits the path of a canceller-off call trace and writes
it as a tap table. It reads the PCMU RTP of both directions out of the
rings.pcapng of a uvoice call trace archive through tshark. The synthetic IP
header carries the direction in DSCP bits 7..6 (0 transmit, 1 receive) and
the kind in bits 5..2 (3 RTP), per uvoice/files/calltrace-README.txt. Each
direction goes on a sample axis anchored on the RTP timestamp, and the
capture stamps align the two axes.

The opening seconds of a call carry call-progress tones, whose correlation is
periodic and pulls the fit off the path, so the fit drops the first five
seconds. onset_ms is the first lobe of the per-second cross-correlation. The
table starts 2 ms before it and runs 64 ms at the 8 kHz of the wire. The fit
is a least-squares filter on the far-active frames, the 10 ms frames whose
downlink mean square is at or over 2500.

The table has two header lines. The first states the geometry, the uplink
level, the fit ERLE of the table and the residual rms. The second holds
key=value facts:

- far_rms: the far-end rms on the active frames;
- erle_48, erle_64: the ideal ERLE of a 48 ms and a 64 ms filter whose first
  tap sits 2 ms before the onset;
- erle_48_onset, erle_48_1ms, erle_48_5ms, erle_48_10ms_before: the 48 ms
  filter with its first tap at the onset, 1 ms and 5 ms after the onset, and
  10 ms before the onset;
- erle_64_onset: the 64 ms filter with its first tap at the onset;
- source: the name of the archive directory.

The capture mode reads the files that src/tools/lec-capture.py writes for
line N: <prefix>-l<N>.csv, -ref.wav and -mic.wav. The ref plane is the
reference as the canceller applied it, so lag 0 is tap 0 of the filter. The
rate comes from the WAV header, and the frame is the rate over 100. The tail
is the default tail of the rate in src/dsp/lec.c: 48 ms at 8 kHz and 64 ms
at 16 kHz. The CSV does not carry the tail, so the fit of a line with another
tail still uses the default, and the first output line names it.

--ref-loss N gives the reference loss of the capture in half-dB steps
(default 0): the loss that the engine applied ahead of the reference of the
canceller. lec-capture.py writes the loss of --ref-loss into the comment line
of the CSV, so the operator reads it there; this tool skips the comment
lines. The tool scales the ref plane up by 10^(N/40) before the frame
selection and the fit, so the reference reads the line level, and the
figures read as those of a capture at 0. The first output line names the
loss.

A segment runs from a NEW or a RESET record to the next one, because the
applied delay can change inside a capture. The tool fits each segment on its
own. It first removes the mean of each plane over the records of each
segment. Reason: captures of real lines carry a constant offset of about
-1.5 LSB on the reference plane and +5 to +7 LSB on the microphone plane. The
fit spreads a microphone offset as a flat tail. The path gain is small, so a
reference offset can model a microphone offset only through large taps.

A bad input stops the tool with a message that names the file:

- a WAV file that the wave reader cannot read;
- a CSV without a record, or a row with a missing or malformed field;
- two planes of different lengths or rates, or a rate without a default
  tail;
- records that do not fill the frames of the WAV files.

A fit frame is a RUN frame of the segment with all of these properties:

- the power of its reference frame is over -40 dBFS;
- it has no DT flag and no skipped count;
- its tick is --skip-s seconds or more after the first record (default 10);
- the reference history of each fit sample lies in records of the segment
  with no skipped count, so no zero-filled tick enters the fit.

The fit is a least-squares filter with a ridge of 1e-3 of the reference
power. Unlike the call trace mode, it uses every sample of the fit frames.
Reason: a fit on fewer samples can move a tap across the -20 dB bar of the
onset.

A first fit from one period before lag 0 to the tail finds the onset. The
second fit runs from one period before that onset to the tail after it. The
peak is the tap of the largest magnitude. The onset is the first tap whose
magnitude reaches -20 dB of the peak. Each of the two fits needs one second
of fit frames. A segment with less, without a RUN record, or with a silent
reference history under its fit frames gets no fit and no solve. A fit with
no tap energy prints no path: the microphone holds no echo of the reference.

The mode prints these figures for each segment:

- the onset and the peak, as a lag in samples and in ms;
- for both, the echo delay from the downlink write: the applied reference
  plus the lag;
- that echo delay less loop_ref, when --loop-ref gives loop_ref. The one
  value serves every segment of the capture;
- the ideal ERLE of a filter from one period before the onset to 10, 20, 40
  and 64 ms after it. Each filter is fitted on the even 10 ms frames and
  scored on the odd ones, so a longer filter cannot read better by fit
  noise. A window prints no score and the reason in three cases: under half
  a second of even or of odd frames, a silent reference history under its
  fit frames, or score frames with no microphone energy;
- the energy of the second fit before the onset and in each 10 ms bin from
  the onset, in dB of its total. A part with no tap energy prints no energy.

The self-test writes captures at 8 kHz and at 16 kHz through the writer of
lec-capture.py. The reference is noise in talk spurts with pauses of a frame
or more, and an offset per segment. An inverted two-lobe path, noise and an
offset per segment make the microphone. The path has a tap under the -20 dB
bar, then the onset tap over it, and its peak 3 taps later. A RESET in the
middle of the capture moves the applied reference and the delay. The
self-test checks these results:

- the onset, the peak and the delays of each segment, exactly;
- the plateau of the cross-validated ERLE at or before 20 ms, and two
  disjoint halves of the frames for each window;
- the removal of the offsets of each plane in each segment, which differ
  between the segments;
- the selection of the fit frames: talk, quiet, pause, DT, the gap, the
  skip and the segment of the history;
- a capture without score frames, which prints no score and exits 0;
- a silent microphone, which reads no path, and a silent reference
  history, which reads no fit;
- a capture at a reference loss of 18 with --ref-loss 18, which reads the
  onset, the peak, the delays and the fit frames of the capture at 0, and
  its ERLE within 0.2 dB.

Without numpy, the self-test prints a skip line and exits 0.

A fit needs python3 and numpy, and the call trace mode also needs tshark.
The archives and the captures hold plaintext call audio and stay out of the
tree.
"""
import argparse
import csv
import importlib.util
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

try:
    import numpy as np
except ImportError:
    np = None

TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
import udsp_audio as ua  # noqa: E402

# The capture mode reads the axis of the WAV files by the rule of the writer,
# and the self-test writes its capture through that writer.
_spec = importlib.util.spec_from_file_location("lec_capture",
                                               TOOLS / "lec-capture.py")
lc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(lc)

RATE = 8000
ULAW_BIAS = 0x84
SKIP_S = 5
TAPS = 512
PRE = 16
FRAME = RATE // 100
ACTIVE_POWER = 2500

CAPTURE_SKIP_S = 10.0
# The reach of the engine's receive loss in half-dB (udsp/src/dsp/lec.h
# UDSP_EC_NLMS_REF_LOSS_MAX).
REF_LOSS_MAX = 60
ACTIVE_DBFS = -40.0
ONSET_DB = -20.0
RIDGE = 1e-3
NORMAL_BLOCK = 2000
TAIL_MS = {8000: 48, 16000: 64}
ERLE_WINDOWS_MS = (10, 20, 40, 64)
BIN_MS = 10
FIT_FRAMES_MIN = lc.TICKS_PER_SECOND
CV_FRAMES_MIN = FIT_FRAMES_MIN // 2


def ulaw_decode(data):
    u = np.frombuffer(data, dtype=np.uint8).astype(np.int32)
    u = ~u & 0xFF
    sign = u & 0x80
    exponent = (u >> 4) & 0x07
    mantissa = u & 0x0F
    sample = ((mantissa << 3) + ULAW_BIAS) << exponent
    sample = sample - ULAW_BIAS
    return np.where(sign, -sample, sample).astype(np.float64)


def rtp_read(path):
    fields = ["frame.time_epoch", "ip.dsfield", "rtp.p_type", "rtp.seq",
              "rtp.timestamp", "rtp.payload"]
    cmd = ["tshark", "-r", path, "-o", "rtp.heuristic_rtp:TRUE",
           "-Y", "rtp", "-T", "fields", "-E", "separator=\t"]
    for f in fields:
        cmd += ["-e", f]
    out = subprocess.run(cmd, capture_output=True, text=True, check=True)
    tx, rx = [], []
    for line in out.stdout.splitlines():
        cols = line.split("\t")
        if len(cols) < 6 or not cols[5]:
            continue
        t = float(cols[0])
        dsfield = (int(cols[1], 16) if cols[1].startswith("0x")
                   else int(cols[1]))
        pt = int(cols[2])
        seq = int(cols[3])
        ts = int(cols[4])
        payload = bytes.fromhex(cols[5].replace(":", ""))
        direction = (dsfield >> 6) & 0x3
        kind = (dsfield >> 2) & 0xF
        if kind != 3 or pt != 0:
            continue
        (rx if direction == 1 else tx).append((t, seq, ts, payload))
    return tx, rx


def ts_unwrap(pkts):
    """The 32-bit RTP timestamps of pkts, in capture order, on one axis. Each
    step between two packets is taken modulo 2^32 and read as signed, so a
    wrap inside the call continues the axis and a late packet steps back."""
    out = [pkts[0][2]]
    for prev, cur in zip(pkts, pkts[1:]):
        step = (cur[2] - prev[2]) % (1 << 32)
        if step >= 1 << 31:
            step -= 1 << 32
        out.append(out[-1] + step)
    return out


def stream_build(pkts):
    """Lay the payloads on a sample axis anchored on the earliest RTP
    timestamp, and return the samples plus the wall time of the first
    sample. The first packet in capture order can be a late one, so it does
    not set the anchor."""
    pkts.sort(key=lambda p: p[0])
    stamps = ts_unwrap(pkts)
    ts0 = min(stamps)
    n = max(ts - ts0 + len(p[3]) for p, ts in zip(pkts, stamps))
    out = np.zeros(n, dtype=np.float64)
    for p, ts in zip(pkts, stamps):
        i = ts - ts0
        out[i:i + len(p[3])] = ulaw_decode(p[3])
    # The median of stamp minus offset, so a jittered arrival does not set
    # the wall time of the first sample.
    offs = [p[0] - (ts - ts0) / RATE for p, ts in zip(pkts, stamps)]
    return out, float(np.median(offs))


def streams_align(path):
    tx, rx = rtp_read(path)
    if not tx or not rx:
        sys.exit(f"{path}: need RTP in both directions")
    ul, ul_t0 = stream_build(tx)
    dl, dl_t0 = stream_build(rx)
    t0 = max(ul_t0, dl_t0)
    ul = ul[int(round((t0 - ul_t0) * RATE)) + SKIP_S * RATE:]
    dl = dl[int(round((t0 - dl_t0) * RATE)) + SKIP_S * RATE:]
    n = min(len(ul), len(dl))
    return ul[:n], dl[:n]


def taps_fit(ul, dl, active, first, taps):
    """The least-squares filter of taps taps whose tap 0 sits at lag first,
    on every third sample of the active frames and at most 30000 samples.
    Returns the taps, the ERLE in dB, the mean square of the residual echo
    and that of the uplink."""
    n = len(ul)
    nf = len(active)
    idx = np.arange(first + taps, n)[::3]
    idx = idx[active[np.minimum(idx // FRAME, nf - 1)]]
    if len(idx) > 30000:
        idx = idx[np.linspace(0, len(idx) - 1, 30000).astype(int)]
    A = np.array([dl[i - first - taps + 1:i - first + 1][::-1] for i in idx])
    b = ul[idx]
    h, *_ = np.linalg.lstsq(A, b, rcond=None)
    res = b - A @ h
    erle = 10 * np.log10(np.mean(b ** 2) / np.mean(res ** 2))
    return h, erle, np.mean(res ** 2), np.mean(b ** 2)


def normal_sum(ul, dl, active, first, taps, frame):
    """The normal equations of a filter of taps taps whose tap 0 sits at lag
    first, over every sample of the active frames: the matrix, the vector
    and the energy of the uplink. The samples run from the first one whose
    oldest reference exists to the last one whose newest reference exists."""
    n = len(ul)
    idx = np.arange(max(first + taps - 1, 0), n + min(first, 0))
    idx = idx[active[np.minimum(idx // frame, len(active) - 1)]]
    view = np.lib.stride_tricks.sliding_window_view(dl, taps)
    R = np.zeros((taps, taps))
    p = np.zeros(taps)
    energy = 0.0
    for k in range(0, len(idx), NORMAL_BLOCK):
        rows = idx[k:k + NORMAL_BLOCK]
        A = view[rows - first - taps + 1]
        b = ul[rows]
        R += A.T @ A
        p += A.T @ b
        energy += b @ b
    # A row of the view holds the oldest sample first, and tap 0 is the
    # newest sample, so the order of the taps turns over.
    return R[::-1, ::-1], p[::-1], energy


def normal_solve(R, p, ridge):
    """The filter of the normal equations R, p, with ridge times the mean
    diagonal of R added to the diagonal."""
    R = R.copy()
    R[np.diag_indices(len(p))] += ridge * np.trace(R) / len(p)
    return np.linalg.solve(R, p)


def normal_erle(R, p, energy, h):
    """The ERLE in dB of the filter h over the samples of R, p and energy."""
    return 10 * np.log10(energy / (energy - 2 * h @ p + h @ R @ h))


def trace_fit(path, onset_ms, out):
    ul, dl = streams_align(path)
    n = len(ul)
    nf = n // FRAME
    dlp = (dl[:nf * FRAME].reshape(nf, FRAME) ** 2).mean(axis=1)
    ulp = (ul[:nf * FRAME].reshape(nf, FRAME) ** 2).mean(axis=1)
    active = dlp >= ACTIVE_POWER
    onset = int(round(onset_ms * RATE / 1000))
    start = onset - PRE
    ms = RATE // 1000

    h, erle, resp, _ = taps_fit(ul, dl, active, start, TAPS)
    hp = h ** 2
    cum = np.cumsum(hp) / hp.sum()
    erl = -10 * np.log10(ulp[active].mean() / dlp[active].mean())
    far_rms = np.sqrt(dlp[active].mean())
    print(f"active frames {active.sum()}; far rms {far_rms:.1f}; uplink "
          f"{erl:.1f} dB under the far end; fit erle {erle:.1f} dB, "
          f"residual rms {np.sqrt(resp):.1f}, coef energy {hp.sum():.5f}")
    peak = int(np.argmax(hp))
    print(f"peak tap {peak} ({(peak - PRE) / ms:+.2f} ms from the onset); "
          f"energy 50% by {(np.searchsorted(cum, 0.5) - PRE) / ms:.1f} ms, "
          f"90% by {(np.searchsorted(cum, 0.9) - PRE) / ms:.1f} ms, "
          f"99% by {(np.searchsorted(cum, 0.99) - PRE) / ms:.1f} ms after "
          f"the onset")

    shifts = [(None, 32, start, "2 ms before the onset"),
              ("erle_48", 48, start, "2 ms before the onset"),
              ("erle_48_onset", 48, onset, "the onset"),
              ("erle_48_1ms", 48, onset + ms, "1 ms after the onset"),
              ("erle_48_5ms", 48, onset + 5 * ms, "5 ms after the onset"),
              ("erle_48_10ms_before", 48, onset - 10 * ms,
               "10 ms before the onset"),
              ("erle_64", 64, start, "2 ms before the onset"),
              ("erle_64_onset", 64, onset, "the onset")]
    facts = {"far_rms": f"{far_rms:.1f}"}
    for key, tail_ms, first, where in shifts:
        _, e, _, _ = taps_fit(ul, dl, active, first, tail_ms * ms)
        if key:
            facts[key] = f"{e:.1f}"
        print(f"  {tail_ms} ms of taps from {where}: {e:.1f} dB")

    source = Path(path).resolve().parent.name
    facts["source"] = source
    head = (f"echo path at the 8 kHz of the wire, {TAPS} taps, tap {PRE} is "
            f"the onset lobe at {onset_ms} ms; uplink {erl:.1f} dB under the "
            f"far end, fit erle {erle:.1f} dB, residual rms "
            f"{np.sqrt(resp):.1f}; source {source}")
    line = " ".join(f"{k}={v}" for k, v in facts.items())
    np.savetxt(out, h, fmt="%.8e", header=head + "\n" + line)


CSV_FIELDS = (("tick", int), ("kind", lc.KIND_NAMES.index),
              ("new", ("0", "1").index), ("reset", ("0", "1").index),
              ("dt", ("0", "1").index), ("skipped", int),
              ("ref_delay_samples", int))


def wav_load(path):
    """The samples and the rate of a WAV file. A file that the wave reader
    cannot read raises ValueError with the name of the file."""
    try:
        return ua.wav_read(path)
    except (EOFError, struct.error, wave.Error) as e:
        raise ValueError(f"{path}: not a readable WAV file: "
                         f"{str(e) or 'the file ends before its header'}")


def rows_read(path):
    """The fields of CSV_FIELDS of each row of a capture CSV, parsed. The
    comment lines before the column names stay out. A missing or malformed
    field raises ValueError with the file and line."""
    rows = []
    with open(path, newline="") as f:
        lines = f.readlines()
    skip = 0
    while skip < len(lines) and lines[skip].startswith("#"):
        skip += 1
    reader = csv.DictReader(lines[skip:])
    for r in reader:
        row = {}
        for name, parse in CSV_FIELDS:
            try:
                row[name] = parse(r.get(name))
            except (TypeError, ValueError):
                raise ValueError(f"{path} line {skip + reader.line_num}: "
                                 f"{name} reads {r.get(name)!r}")
        rows.append(row)
    return rows


class Capture:
    """The planes of a capture, each segment without its mean, and one entry
    per frame of the WAV axis. A zero-filled tick has tick -1 and no flag.
    The ref plane stands at the line level, the reference loss taken back
    out. A file that does not match the others raises ValueError."""

    def __init__(self, prefix, ref_loss=0):
        ref, self.rate = wav_load(prefix + "-ref.wav")
        self.ref_loss = ref_loss
        mic, mic_rate = wav_load(prefix + "-mic.wav")
        rows = rows_read(prefix + ".csv")
        if not rows:
            raise ValueError(f"{prefix}.csv holds no record")
        if len(mic) != len(ref) or mic_rate != self.rate:
            raise ValueError(f"{prefix}-mic.wav holds {len(mic)} samples at "
                             f"{mic_rate} Hz, {prefix}-ref.wav {len(ref)} at "
                             f"{self.rate} Hz")
        if self.rate not in TAIL_MS:
            raise ValueError(f"{prefix}-ref.wav: {self.rate} Hz has no "
                             f"default tail; the tool knows "
                             f"{sorted(TAIL_MS)} Hz")
        self.frame = self.rate // 100
        self.tail_ms = TAIL_MS[self.rate]
        nf = len(ref) // self.frame
        self.tick0 = rows[0]["tick"]
        self.tick = np.full(nf, -1, dtype=np.int64)
        self.run = np.zeros(nf, dtype=bool)
        self.clean = np.zeros(nf, dtype=bool)
        self.dt = np.zeros(nf, dtype=bool)
        self.seg = np.full(nf, -1, dtype=np.int64)
        self.segments = []
        self.frames_place(prefix, rows, nf)

        n = nf * self.frame
        self.ref = np.asarray(ref[:n], dtype=np.float64)
        if ref_loss:
            self.ref *= 10 ** (ref_loss / 40)
        self.mic = np.asarray(mic[:n], dtype=np.float64)
        for seg in self.segments:
            own = (self.seg == seg["index"]) & (self.tick >= 0)
            own = np.repeat(own, self.frame)
            self.ref[own] -= self.ref[own].mean()
            self.mic[own] -= self.mic[own].mean()
        power = (self.ref.reshape(nf, self.frame) ** 2).mean(axis=1)
        self.loud = power > ua.FULL_SCALE ** 2 * 10 ** (ACTIVE_DBFS / 10)

    def frames_place(self, prefix, rows, nf):
        """Put each RUN and FEED row on its frame of the WAV axis, by the
        rule of planes_gaps, and number the segments. The rows must fill the
        nf frames of the WAV files exactly."""
        recs = [lc.Record(tick=r["tick"], kind=r["kind"]) for r in rows]
        body = [r for r in rows if r["kind"] != lc.KIND_END]
        gaps, _ = lc.planes_gaps(recs)
        placed = sum(gap + 1 for gap, _ in gaps)
        if placed != nf:
            raise ValueError(f"{prefix}.csv places {placed} frames, "
                             f"{prefix}-ref.wav holds {nf}")
        pos = 0
        for (gap, _), r in zip(gaps, body):
            pos += gap
            if r["new"] or r["reset"]:
                self.segments.append({
                    "index": len(self.segments),
                    "word": "NEW" if r["new"] else "RESET",
                    "tick": r["tick"], "ref": None, "frames": 0})
            self.tick[pos] = r["tick"]
            self.run[pos] = r["kind"] == lc.KIND_RUN
            self.clean[pos] = r["skipped"] == 0
            self.dt[pos] = r["dt"]
            if self.segments:
                seg = self.segments[-1]
                self.seg[pos] = seg["index"]
                seg["frames"] += 1
                if seg["ref"] is None and self.run[pos]:
                    seg["ref"] = r["ref_delay_samples"]
            pos += 1

    def frames_select(self, s, skip_s, first, taps):
        """The fit frames of segment s for a filter of taps taps from lag
        first: the rules of the docstring. Every frame that the reference
        history of a fit frame covers is a record of segment s with no
        skipped count."""
        nf = len(self.tick)
        base = (self.run & self.clean & ~self.dt & self.loud &
                (self.seg == s) &
                (self.tick - self.tick0 >= skip_s * lc.TICKS_PER_SECOND))
        own = self.clean & (self.seg == s)
        bad = np.concatenate([[0], np.cumsum(~own)])
        j = np.arange(nf)
        lo = j + (1 - first - taps) // self.frame
        hi = j + (self.frame - 1 - first) // self.frame
        inside = (lo >= 0) & (hi < nf)
        lo = np.clip(lo, 0, nf)
        hi = np.clip(hi + 1, 0, nf)
        return base & inside & (bad[hi] - bad[lo] == 0)


def path_read(h, first):
    """The lags of the onset and of the peak of the filter h."""
    mag = np.abs(h)
    peak = int(np.argmax(mag))
    onset = int(np.nonzero(mag >= mag[peak] * 10 ** (ONSET_DB / 20))[0][0])
    return first + onset, first + peak


def window_score(cap, train, score, start, n, counts):
    """The cross-validated ERLE of one window and None, or None and the
    reason that the window has no score."""
    if min(counts[:2]) < CV_FRAMES_MIN:
        return None, (f"{counts[0]} fit and {counts[1]} score frames, "
                      f"under {CV_FRAMES_MIN}")
    R, p, _ = normal_sum(cap.mic, cap.ref, train, start, n, cap.frame)
    if np.trace(R) == 0:
        return None, "the reference history of the fit frames is silent"
    sums = normal_sum(cap.mic, cap.ref, score, start, n, cap.frame)
    if sums[2] == 0:
        return None, "the score frames hold no microphone energy"
    return normal_erle(*sums, normal_solve(R, p, RIDGE)), None


def segment_fit(cap, seg, skip_s, loop_ref):
    """The fit of one segment: the figures of the docstring, in a dict. A
    segment without a RUN record has no fit. A segment that the tool cannot
    fit has no onset key, and its "no_fit" value gives the reason. A window
    that the tool cannot score has the ERLE None and a note."""
    s = seg["index"]
    period = cap.frame
    taps = period + cap.tail_ms * cap.rate // 1000
    fit = dict(seg, fit_frames=0)
    if seg["ref"] is None:
        return fit

    first = -period
    for fit_pass in ("find", "fit"):
        sel = cap.frames_select(s, skip_s, first, taps)
        fit["fit_frames"] = int(sel.sum())
        if fit["fit_frames"] < FIT_FRAMES_MIN:
            fit["no_fit"] = (f"no fit: {fit['fit_frames']} fit frames, "
                             f"under {FIT_FRAMES_MIN}")
            return fit
        R, p, _ = normal_sum(cap.mic, cap.ref, sel, first, taps, period)
        if np.trace(R) == 0:
            fit["no_fit"] = ("no fit: the reference history of the fit "
                             "frames is silent")
            return fit
        h = normal_solve(R, p, RIDGE)
        if not h.any():
            fit["no_fit"] = ("no path: the microphone holds no echo of the "
                             "reference")
            return fit
        onset, peak = path_read(h, first)
        if fit_pass == "find":
            first = onset - period
    fit.update(first=first, taps=taps, onset=onset, peak=peak)
    for name in ("onset", "peak"):
        fit[name + "_delay"] = seg["ref"] + fit[name]
        if loop_ref is not None:
            fit[name + "_less"] = fit[name + "_delay"] - loop_ref

    fit["erle"] = {}
    fit["cv"] = {}
    fit["note"] = {}
    for ms in ERLE_WINDOWS_MS:
        start = onset - period
        n = period + ms * cap.rate // 1000
        sel = cap.frames_select(s, skip_s, start, n)
        train = sel & (cap.tick % 2 == 0)
        score = sel & (cap.tick % 2 == 1)
        fit["cv"][ms] = (int(train.sum()), int(score.sum()),
                         int((train & score).sum()))
        fit["erle"][ms], fit["note"][ms] = window_score(
            cap, train, score, start, n, fit["cv"][ms])

    lags = first + np.arange(taps)
    energy = h ** 2
    total = energy.sum()
    fit["before"] = share_db(energy[lags < onset].sum(), total)
    fit["bins"] = []
    for lo_ms in range(0, cap.tail_ms, BIN_MS):
        hi_ms = min(lo_ms + BIN_MS, cap.tail_ms)
        lo = onset + lo_ms * cap.rate // 1000
        hi = onset + hi_ms * cap.rate // 1000
        part = energy[(lags >= lo) & (lags < hi)].sum()
        fit["bins"].append((lo_ms, hi_ms, share_db(part, total)))
    return fit


def share_db(part, total):
    """part over total in dB, or None for a part with no energy."""
    return 10 * np.log10(part / total) if part else None


def db_text(value):
    return "no energy" if value is None else f"{value:.1f}"


def lag_text(lag, ms):
    return f"{lag} ({lag * ms:.2f} ms)"


def erle_text(fit, ms):
    if fit["erle"][ms] is None:
        return f"{ms} ms no score ({fit['note'][ms]})"
    return f"{ms} ms {fit['erle'][ms]:.1f} dB"


def segment_print(cap, fit):
    ms = 1000 / cap.rate
    head = (f"segment {fit['index'] + 1}: {fit['word']} at tick "
            f"{fit['tick']}, {fit['frames']} frames")
    if fit["ref"] is None:
        print(f"{head}, no RUN record")
        return
    print(f"{head}, applied reference {fit['ref']} samples")
    if "onset" not in fit:
        print(f"  {fit['no_fit']}")
        return
    print(f"  fit frames {fit['fit_frames']}, taps from lag {fit['first']} "
          f"to lag {fit['first'] + fit['taps'] - 1}")
    for name in ("onset", "peak"):
        text = (f"  {name}: lag {lag_text(fit[name], ms)}, echo delay "
                f"{lag_text(fit[name + '_delay'], ms)}")
        if name + "_less" in fit:
            text += f", less loop_ref {lag_text(fit[name + '_less'], ms)}"
        print(text)
    print("  ideal ERLE from one period before the onset, fit on the even "
          "frames, scored on the odd: " +
          ", ".join(erle_text(fit, w) for w in fit["erle"]))
    print(f"  energy of the fit, dB of the total, ridge {RIDGE:g}: before "
          f"the onset {db_text(fit['before'])}; from the onset " +
          ", ".join(f"{lo}-{hi} ms {db_text(e)}" for lo, hi, e in fit["bins"]))


def capture_fit(prefix, skip_s, loop_ref, ref_loss=0):
    cap = Capture(prefix, ref_loss)
    return cap, [segment_fit(cap, seg, skip_s, loop_ref)
                 for seg in cap.segments]


def capture_print(prefix, cap, fits):
    print(f"capture {Path(prefix).name}: {cap.rate} Hz, frame {cap.frame}, "
          f"fit tail {cap.tail_ms} ms (the default of the rate), reference "
          f"loss {cap.ref_loss} half-dB, segments {len(fits)}")
    if not fits:
        print("  no NEW or RESET record: no segment to fit")
    for fit in fits:
        segment_print(cap, fit)


# Tap 0 stands at -26 dB of the peak, under the -20 dB bar, and tap 1 at
# -14 dB, over it. The onset is therefore tap 1, and the peak is tap 4.
T_PATH = ((0, 0.05), (1, -0.2), (2, -0.5), (3, -0.8), (4, -1.0), (5, -0.6),
          (6, -0.2), (8, 0.3), (9, 0.45), (10, 0.3))
T_GAIN = 0.5
T_ONSET = 1
T_PEAK = 4
T_TAIL_AMP = 0.05
T_TAIL_TAU_MS = 2
T_LATE_MS = 12
T_LATE_AMP = 0.08
T_FRAMES = 1000
T_SKIP_S = 1.0
T_DT = range(400, 430)
T_GAP = range(600, 605)
T_TALK = 3000
T_QUIET = 184
T_LEVELS = ((380, 650, T_TALK), (690, 710, T_TALK), (720, 730, T_QUIET),
            (740, 750, 0), (990, 1100, T_TALK))
T_REF_OFFSETS = (300, -200)
T_MIC_OFFSETS = (600, -600)
T_PLATEAU_DB = 0.5
T_ERLE_MIN_DB = 35


def t_path(rate):
    """The inverted two-lobe path of T_PATH, then a tail that decays with
    T_TAIL_TAU_MS and alternates its sign, and a weak reflection T_LATE_MS
    after tap 0. A 10 ms filter misses the reflection, and a 20 ms filter
    holds it."""
    h = np.zeros(15 * rate // 1000)
    for k, v in T_PATH:
        h[k] = v
    k = np.arange(T_PATH[-1][0] + 1, len(h))
    h[k] = (T_TAIL_AMP * (-1.0) ** k *
            np.exp(-(k - k[0]) / (T_TAIL_TAU_MS * rate / 1000)))
    h[T_LATE_MS * rate // 1000] += T_LATE_AMP
    return T_GAIN * h


def t_downlink(rate, count, rng, shifts):
    """Noise from 100 Hz to 0.45 of the rate, falling above 1 kHz, in talk
    spurts of 0.3 to 1.5 s with pauses of one frame to 0.6 s. The frames of
    T_LEVELS take their rms on the reference plane, which reads the
    downlink later by one of the shifts, in frames. T_QUIET is -45 dBFS."""
    frame = rate // 100
    noise = rng.standard_normal(count * frame)
    spec = np.fft.rfft(noise)
    f = np.fft.rfftfreq(len(noise), 1 / rate)
    spec *= ((f >= 100) & (f <= 0.45 * rate)) / np.sqrt(1 + (f / 1000) ** 2)
    talk = np.fft.irfft(spec, len(noise))
    talk /= np.sqrt(np.mean(talk ** 2))
    env = np.zeros(count)
    i = 0
    while i < count:
        n = int(rng.integers(30, 150))
        env[i:i + n] = rng.uniform(1000, 4000)
        i += n + int(rng.integers(1, 60))
    for lo, hi, level in T_LEVELS:
        env[lo + min(shifts):hi + max(shifts) + 1] = level
    return talk * np.repeat(env, frame)


def t_capture_write(prefix, rate, rng, dt_odd=False, ref_loss=0):
    """A capture of two segments through the writer of lec-capture.py. Both
    lags are more than one period, so the reference history of a fit frame
    ends before the frame. With dt_odd, every odd tick carries the DT flag.
    A ref_loss in half-dB steps lowers the ref plane alone, as the engine
    lowers the reference, and the CSV carries it. Returns the loop, the
    applied reference and the onset delay of each segment."""
    frame = rate // 100
    count = 2 * T_FRAMES
    loop = (3 * frame, 4 * frame)
    applied = (loop[0] - frame, loop[1] - frame)
    delay = (loop[0] + frame + 3, loop[0] + 3 * frame + 8)
    margin = delay[1] + 20 * rate // 1000
    shifts = [(margin - a) // frame for a in applied]
    dl = t_downlink(rate, count + margin // frame + 1, rng, shifts)
    echo = np.convolve(dl, t_path(rate))[:len(dl)]
    ref = np.zeros(count * frame)
    mic = np.zeros(count * frame)
    for s in (0, 1):
        t = np.arange(s * T_FRAMES * frame, (s + 1) * T_FRAMES * frame)
        ref[t] = dl[t + margin - applied[s]] + T_REF_OFFSETS[s]
        mic[t] = (echo[t + margin - delay[s] + T_ONSET] + T_MIC_OFFSETS[s] +
                  rng.normal(0, 20, len(t)))
    near = np.concatenate([np.arange(i * frame, (i + 1) * frame)
                           for i in T_DT])
    mic[near] += rng.normal(0, 2000, len(near))
    ref *= 10 ** (-ref_loss / 40)
    ref = np.clip(np.round(ref), -32768, 32767).astype(np.int16)
    mic = np.clip(np.round(mic), -32768, 32767).astype(np.int16)

    recs = []
    for i in range(count):
        if i in T_GAP:
            continue
        s = i // T_FRAMES
        flags = 0
        if i == 0:
            flags |= lc.FLAG_NEW
        if i == T_FRAMES:
            flags |= lc.FLAG_RESET
        if i in T_DT or (dt_odd and i % 2):
            flags |= 1 << lc.FLAG_NAMES.index("dt")
        skipped = len(T_GAP) if i == T_GAP[-1] + 1 else 0
        if skipped:
            flags |= lc.FLAG_SKIPPED
        planes = slice(i * frame, (i + 1) * frame)
        recs.append(lc.Record(
            stamp_ns=i * 10000000, tick=5000 + i, frames=i % T_FRAMES + 1,
            ref_delay_samples=applied[s], gen=s + 1, flags=flags,
            samples=frame, tail_ms=TAIL_MS[rate], noise_ddbov=1270,
            kind=lc.KIND_RUN, nlp_verdict=lc.VERDICT_PASS, skipped=skipped,
            ref=ref[planes].tolist(), mic=mic[planes].tolist(),
            out=mic[planes].tolist()))
    recs.append(lc.Record(stamp_ns=count * 10000000, tick=5000 + count,
                          frames=T_FRAMES, ref_delay_samples=applied[1],
                          gen=2, samples=frame, tail_ms=TAIL_MS[rate],
                          noise_ddbov=1270, kind=lc.KIND_END))
    lr = lc.LineResult(0)
    lr.records = recs
    lr.samples = frame
    lr.ref_loss = ref_loss or None
    lc.outputs_write(prefix, lr)
    return loop, applied, delay


def t_select_check(check, cap, rate, fit):
    """The selection rules on the frames of T_LEVELS."""
    def span(fit):
        """The frames before a fit frame that its history covers."""
        return -((1 - fit["first"] - fit["taps"]) // cap.frame)

    sel = cap.frames_select(0, T_SKIP_S, fit[0]["first"], fit[0]["taps"])
    after = T_GAP.stop + 1
    check(sel[690:710].all(), f"{rate}: the talk frames are fit frames")
    check(not sel[722:728].any(),
          f"{rate}: the frames at -45 dBFS are not fit frames")
    check(not sel[742:748].any(),
          f"{rate}: the frames of a pause are not fit frames")
    check(not sel[T_DT.start:T_DT.stop].any(),
          f"{rate}: the DT frames are not fit frames")
    check(not sel[T_GAP.start:after].any(),
          f"{rate}: the gap and the skipped record are not fit frames")
    check(not sel[after:after + span(fit[0])].any() and
          sel[after + span(fit[0]):650].all(),
          f"{rate}: the frames whose history reaches the gap are not fit "
          f"frames, the later ones are")
    check(not sel[:int(T_SKIP_S * lc.TICKS_PER_SECOND)].any(),
          f"{rate}: the frames before --skip-s are not fit frames")
    check(not sel[T_FRAMES:1100].any(),
          f"{rate}: a frame of segment 2 is not a fit frame of segment 1, "
          f"also when its history lies in segment 1")
    sel = cap.frames_select(1, T_SKIP_S, fit[1]["first"], fit[1]["taps"])
    check(not sel[T_FRAMES:T_FRAMES + span(fit[1])].any() and
          sel[T_FRAMES + span(fit[1]):1100].all(),
          f"{rate}: the history of a fit frame stays in its segment")
    taps = fit[0]["taps"]
    sel = cap.frames_select(0, T_SKIP_S, -cap.frame, taps)
    check(sel[T_GAP.start - 2] and not sel[T_GAP.start - 1] and
          sel[T_FRAMES - 2] and not sel[T_FRAMES - 1],
          f"{rate}: a history after the frame ends before the gap and "
          f"before the RESET")


def t_rate_check(check, tmp, rate, seed):
    prefix = str(Path(tmp) / f"t{rate}")
    loop, applied, delay = t_capture_write(prefix, rate,
                                           np.random.default_rng(seed))
    cap, fits = capture_fit(prefix + "-l0", T_SKIP_S, loop[0])
    capture_print(prefix + "-l0", cap, fits)
    check(cap.rate == rate and cap.frame == rate // 100,
          f"{rate}: the rate and the frame come from the WAV header")
    check(len(fits) == 2 and all("onset" in f for f in fits),
          f"{rate}: two segments, each fitted")
    if len(fits) != 2 or not all("onset" in f for f in fits):
        return
    for s, fit in enumerate(fits):
        name = f"{rate}: segment {s + 1}"
        peak = delay[s] - T_ONSET + T_PEAK
        check(fit["ref"] == applied[s],
              f"{name} reads its applied reference")
        check(fit["onset"] == delay[s] - applied[s] and
              fit["onset_delay"] == delay[s],
              f"{name} onset delay {fit['onset_delay']}, expected "
              f"{delay[s]}")
        check(fit["peak"] == peak - applied[s] and
              fit["peak_delay"] == peak,
              f"{name} peak delay {fit['peak_delay']}, expected {peak}")
        # One --loop-ref value serves the whole capture, so each segment
        # reads its delay less loop[0], whatever its own loop.
        check(fit["onset_less"] == delay[s] - loop[0] and
              fit["peak_less"] == peak - loop[0],
              f"{name} delays less loop_ref {fit['onset_less']} and "
              f"{fit['peak_less']}")
        check(fit["first"] == fit["onset"] - cap.frame,
              f"{name} second fit starts one period before the onset")
        erle = fit["erle"]
        check(None not in erle.values() and
              max(erle.values()) - erle[20] <= T_PLATEAU_DB,
              f"{name} ERLE plateau by 20 ms: {erle}")
        check(None not in erle.values() and erle[20] - erle[10] > 3,
              f"{name} 10 ms filter misses the reflection: {erle}")
        check(None not in erle.values() and erle[20] >= T_ERLE_MIN_DB,
              f"{name} ERLE at 20 ms over {T_ERLE_MIN_DB} dB, so the "
              f"offsets of the segment are gone: {erle}")
        for ms, (train, score, both) in fit["cv"].items():
            check(train > 0 and score > 0 and both == 0 and
                  abs(train - score) <= 0.2 * (train + score),
                  f"{name} {ms} ms window fits and scores on two halves: "
                  f"{train} fit, {score} score, {both} in both")
    check(fits[1]["onset"] - fits[0]["onset"] ==
          delay[1] - applied[1] - delay[0] + applied[0],
          f"{rate}: the segments keep their own lags")
    for s in (0, 1):
        own = np.repeat((cap.seg == s) & (cap.tick >= 0), cap.frame)
        means = (cap.ref[own].mean(), cap.mic[own].mean())
        check(max(abs(m) for m in means) < 1e-6,
              f"{rate}: segment {s + 1} planes have the means {means}")
    t_select_check(check, cap, rate, fits)
    t_loss_check(check, tmp, rate, seed, fits)
    t_zero_check(check, cap, rate)


T_REF_LOSS = 18
T_LOSS_ERLE_DB = 0.2
T_LOSS_KEYS = ("first", "onset", "peak", "onset_delay", "peak_delay",
               "onset_less", "peak_less", "fit_frames")


def t_loss_check(check, tmp, rate, seed, base):
    """The capture of t_rate_check with the ref plane 9 dB lower, fitted
    with --ref-loss 18: the figures of the capture at 0."""
    prefix = str(Path(tmp) / f"t{rate}-loss")
    loop, _, _ = t_capture_write(prefix, rate, np.random.default_rng(seed),
                                 ref_loss=T_REF_LOSS)
    with open(prefix + "-l0.csv") as f:
        head = f.readline().strip()
    check(head == lc.CSV_REF_LOSS % T_REF_LOSS,
          f"{rate}: the CSV of the loss capture names the loss: {head}")
    cap, fits = capture_fit(prefix + "-l0", T_SKIP_S, loop[0], T_REF_LOSS)
    capture_print(prefix + "-l0", cap, fits)
    check(len(fits) == len(base) and
          all(fit.get(k) == want.get(k) for fit, want in zip(fits, base)
              for k in T_LOSS_KEYS),
          f"{rate}: the fit at a loss of {T_REF_LOSS} reads the lags and the "
          f"fit frames of the fit at 0")
    for s, (fit, want) in enumerate(zip(fits, base)):
        got, ref = fit.get("erle", {}), want.get("erle", {})
        check(got.keys() == ref.keys() and
              all(None not in (got[ms], ref[ms]) and
                  abs(got[ms] - ref[ms]) <= T_LOSS_ERLE_DB for ms in ref),
              f"{rate}: segment {s + 1} ERLE at a loss of {T_REF_LOSS} "
              f"{got}, at 0 {ref}")


def t_zero_check(check, cap, rate):
    """A silent microphone gives no path, and a silent reference history
    under loud fit frames gives no fit. Neither runs a solve."""
    seg = cap.segments[0]
    first, taps = -cap.frame, cap.frame + cap.tail_ms * cap.rate // 1000
    sel = cap.frames_select(0, T_SKIP_S, first, taps)
    even, odd = (sel & (cap.tick % 2 == k) for k in (0, 1))
    counts = (int(even.sum()), int(odd.sum()), 0)
    cap.mic[:] = 0
    fit = segment_fit(cap, seg, T_SKIP_S, None)
    check("onset" not in fit and fit.get("no_fit", "").startswith("no path"),
          f"{rate}: a silent microphone reads no path: {fit.get('no_fit')}")
    erle, note = window_score(cap, even, odd, first, taps, counts)
    check(erle is None and "no microphone energy" in note,
          f"{rate}: a window with a silent microphone has no score: {note}")
    cap.ref[:] = 0
    fit = segment_fit(cap, seg, T_SKIP_S, None)
    check("onset" not in fit and "silent" in fit.get("no_fit", ""),
          f"{rate}: a silent reference history reads no fit: "
          f"{fit.get('no_fit')}")
    erle, note = window_score(cap, even, odd, first, taps, counts)
    check(erle is None and "silent" in note,
          f"{rate}: a window with a silent reference history has no score: "
          f"{note}")


def t_bounds_check(check):
    """normal_sum takes each sample whose whole reference history exists:
    from first + taps - 1, and up to n + first for a negative first."""
    ones = np.ones(10)
    active = np.ones(10, dtype=bool)
    for first, taps, count in ((0, 3, 8), (2, 3, 6), (-2, 3, 8)):
        _, _, energy = normal_sum(ones, ones, active, first, taps, 1)
        check(energy == count, f"normal_sum over first {first} and {taps} "
              f"taps reads {energy:g} samples, expected {count}")


def t_short_check(check, tmp):
    """A capture whose odd ticks all carry DT has no score frame. Each ERLE
    window then prints no score, and the tool exits 0."""
    prefix = str(Path(tmp) / "todd")
    t_capture_write(prefix, 8000, np.random.default_rng(3), dt_odd=True)
    out = subprocess.run([sys.executable, __file__, "--capture",
                          prefix + "-l0", "--skip-s", str(T_SKIP_S)],
                         capture_output=True, text=True)
    lines = [ln for ln in out.stdout.splitlines() if "ideal ERLE" in ln]
    check(out.returncode == 0 and len(lines) == 2 and
          all(ln.count(f" score frames, under {CV_FRAMES_MIN})") ==
              len(ERLE_WINDOWS_MS) for ln in lines),
          f"a window without score frames prints no score: "
          f"exit {out.returncode}, {lines} {out.stderr.strip()}")


def self_test():
    failed = []
    count = [0]

    def check(ok, what):
        count[0] += 1
        if not ok:
            failed.append(what)

    with tempfile.TemporaryDirectory(prefix="path-fit-") as tmp:
        for seed, rate in enumerate((8000, 16000)):
            t_rate_check(check, tmp, rate, seed + 1)
        t_short_check(check, tmp)
    t_bounds_check(check)
    for what in failed:
        print(f"  fail: {what}")
    print(f"path_fit self-test: {'ok' if not failed else 'FAILED'} "
          f"({count[0]} checks, {len(failed)} failed)")
    return 0 if not failed else 1


def main():
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        usage=__doc__.split("\n\n")[1][len("usage: "):])
    parser.add_argument("trace", nargs="*",
                        help="<rings.pcapng> <onset_ms> <out.txt>")
    parser.add_argument("--capture", metavar="PREFIX",
                        help="the <prefix>-l<N> of a canceller capture")
    parser.add_argument("--skip-s", type=float,
                        help="the opening seconds that the capture mode "
                        f"leaves out (default {CAPTURE_SKIP_S:g})")
    parser.add_argument("--loop-ref", type=int, metavar="N",
                        help="loop_ref of the line, in samples")
    parser.add_argument("--ref-loss", type=int, metavar="N",
                        help="the reference loss of the capture in half-dB "
                        "steps (default 0)")
    parser.add_argument("--self-test", action="store_true",
                        help="check the capture mode on synthetic captures")
    args = parser.parse_args()
    if args.self_test:
        if np is None:
            print("path_fit self-test: skipped, numpy is not installed")
            return 0
        return self_test()
    if np is None:
        sys.exit("path_fit.py: a fit needs numpy")
    if args.capture:
        if args.trace:
            parser.error("--capture takes no positional argument")
        skip_s = CAPTURE_SKIP_S if args.skip_s is None else args.skip_s
        ref_loss = args.ref_loss or 0
        if not 0 <= ref_loss <= REF_LOSS_MAX:
            parser.error(f"the reference loss must be 0 to {REF_LOSS_MAX}")
        try:
            cap, fits = capture_fit(args.capture, skip_s, args.loop_ref,
                                    ref_loss)
        except (OSError, ValueError) as e:
            sys.exit(f"path_fit.py: {args.capture}: {e}")
        capture_print(args.capture, cap, fits)
        return 0
    if args.skip_s is not None or args.loop_ref is not None or \
            args.ref_loss is not None:
        parser.error("--skip-s, --loop-ref and --ref-loss apply to --capture "
                     "only")
    if len(args.trace) != 3:
        parser.error("the call trace mode takes three arguments")
    trace_fit(args.trace[0], float(args.trace[1]), args.trace[2])
    return 0


if __name__ == "__main__":
    sys.exit(main())
