#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-only
#
# udsp - software voice DSP for FXS lines
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
"""Bell 202 or V.23 caller identity demodulator for a line capture.

The carriers stay in the 8 kHz caller identity band. The demodulator uses the
input sample rate for timing and correlation, including a 16 kHz input.

The decision is a one bit correlation against the mark and space carriers
per sample; the frames are sampled at bit centres from the first space after
the longest mark run. That is enough for a clean capture or for the strong
reflection udsp-linecap records on an open loop. The exit status is 0 when
at least one burst parsed with a valid checksum, so a script can gate on it.

Message types: 0x04 SDMF call setup, 0x80 MDMF call setup, 0x06 SDMF visual
message waiting, 0x82 MDMF visual message waiting. MDMF parameters: 0x01
date and time, 0x02 number, 0x04 number absent, 0x07 name, 0x08 name absent,
0x0b visual indicator."""

import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import udsp_audio as ua  # noqa: E402

BAUD = 1200.0
BELL202 = (1200.0, 2200.0)
V23 = (1300.0, 2100.0)
BURST_THRESHOLD = 300.0
BITS_PER_FRAME = 10

MESSAGE_NAMES = {
    0x04: "SDMF call setup",
    0x80: "MDMF call setup",
    0x06: "SDMF visual message waiting",
    0x82: "MDMF visual message waiting",
}
PARAMETER_NAMES = {
    0x01: "date and time",
    0x02: "number",
    0x04: "number absent",
    0x07: "name",
    0x08: "name absent",
    0x0B: "visual indicator",
}


def correlate(samples, index, freq_hz, sample_rate, window):
    """Energy of freq_hz over one bit window starting at index."""
    re = im = 0.0
    for k in range(window):
        j = index + k
        if j >= len(samples):
            break
        angle = 2.0 * math.pi * freq_hz * j / sample_rate
        re += samples[j] * math.cos(angle)
        im += samples[j] * math.sin(angle)
    return re * re + im * im


def decisions(samples, carriers, sample_rate):
    """Per sample: True for mark, False for space."""
    mark_hz, space_hz = carriers
    window = int(round(sample_rate / BAUD))
    return [correlate(samples, i, mark_hz, sample_rate, window) >
            correlate(samples, i, space_hz, sample_rate, window)
            for i in range(len(samples) - window)]


def envelope(samples, sample_rate):
    window = int(round(sample_rate / BAUD))
    return [ua.rms(samples[i:i + window])
            for i in range(len(samples) - window)]


def bursts_find(env, sample_rate):
    """(start, end) sample spans where the envelope stays above the threshold,
    joined across gaps shorter than 10 ms."""
    spans = []
    gap_samples = sample_rate // 100
    i = 0
    while i < len(env):
        if env[i] <= BURST_THRESHOLD:
            i += 1
            continue
        j = i
        while j < len(env):
            if env[j] > BURST_THRESHOLD:
                j += 1
                continue
            ahead = env[j:j + gap_samples]
            if ahead and max(ahead) > BURST_THRESHOLD:
                j += 1
                continue
            break
        spans.append((i, j))
        i = j
    return spans


def preamble_find(bits):
    """The longest mark run: (length, index of its last sample)."""
    best_len = best_end = 0
    run = 0
    for index, mark in enumerate(bits):
        run = run + 1 if mark else 0
        if run > best_len:
            best_len, best_end = run, index
    return best_len, best_end


def frames_decode(bits, start, sample_rate):
    """8N1 frames from the first space at or after start, LSB first, until a
    frame does not fit or the framing is lost."""
    out = []
    pos = start
    samples_per_bit = sample_rate / BAUD
    frame_span = int(samples_per_bit * BITS_PER_FRAME)
    while pos + frame_span < len(bits):
        while pos < len(bits) and bits[pos]:
            pos += 1
        if pos + frame_span >= len(bits):
            break
        centre = pos + samples_per_bit / 2.0
        cells = [bits[int(centre + samples_per_bit * b)]
                 for b in range(BITS_PER_FRAME)]
        if cells[0] or not cells[BITS_PER_FRAME - 1]:
            pos += 1
            continue
        out.append(sum(cells[1 + b] << b for b in range(8)))
        pos = int(centre + samples_per_bit * (BITS_PER_FRAME - 0.5))
    return out


def message_parse(data):
    """(ok, lines) for a decoded byte string: the type, the length, every
    parameter, and the checksum verdict. ok is True only for a complete
    message whose checksum matches."""
    lines = []
    if len(data) < 2:
        return False, ["no message header"]
    kind, length = data[0], data[1]
    lines.append("type 0x%02x (%s) length %d" %
                 (kind, MESSAGE_NAMES.get(kind, "unknown"), length))
    body = data[2:2 + length]
    complete = len(data) >= 2 + length + 1
    if kind in (0x80, 0x82):
        k = 0
        while k + 1 < len(body):
            tag, size = body[k], body[k + 1]
            value = bytes(body[k + 2:k + 2 + size])
            lines.append("  param 0x%02x (%s) length %d: %r" %
                         (tag, PARAMETER_NAMES.get(tag, "unknown"), size,
                          value))
            k += 2 + size
    else:
        lines.append("  body: %r" % bytes(body))
    if not complete:
        lines.append("  checksum: MISSING, %d of %d bytes present" %
                     (len(data), 2 + length + 1))
        return False, lines
    want = (-sum(data[:2 + length])) & 0xFF
    got = data[2 + length]
    if want == got:
        lines.append("  checksum: OK (0x%02x)" % got)
        return True, lines
    lines.append("  checksum: BAD (want 0x%02x, got 0x%02x)" % (want, got))
    return False, lines


def burst_decode(samples, span, carriers, sample_rate):
    """(ok, report lines) for one burst."""
    start, end = span
    lines = ["burst %d..%d ms (%d ms)" %
             (start * 1000 // sample_rate, end * 1000 // sample_rate,
              (end - start) * 1000 // sample_rate)]
    bits = decisions(samples[start:end], carriers, sample_rate)
    pre_len, pre_end = preamble_find(bits)
    lines.append("  mark preamble %d ms, ends at %d ms" %
                 (pre_len * 1000 // sample_rate,
                  (start + pre_end) * 1000 // sample_rate))
    data = frames_decode(bits, pre_end + 1, sample_rate)
    lines.append("  bytes(%d): %s" % (len(data), " ".join("%02x" % b for b in data)))
    if not data:
        return False, lines
    ok, parsed = message_parse(data)
    return ok, lines + parsed


def capture_decode(samples, carriers, sample_rate=ua.SAMPLE_RATE):
    env = envelope(samples, sample_rate)
    results = [burst_decode(samples, span, carriers, sample_rate)
               for span in bursts_find(env, sample_rate)]
    return results


def modulate(message, carriers, seizure_bits, mark_bits, level_dbfs,
             sample_rate=ua.SAMPLE_RATE):
    """A continuous phase FSK burst the way dsp/fsk_mod.c builds one: the
    seizure alternates 0 and 1, the mark preamble follows, then 8N1 frames
    LSB first, then two stop bits. For the self-test."""
    mark_hz, space_hz = carriers
    bits = [i & 1 for i in range(seizure_bits)] + [1] * mark_bits
    for value in message:
        bits += [0] + [(value >> b) & 1 for b in range(8)] + [1]
    bits += [1, 1]
    amplitude = ua.FULL_SCALE * 10 ** (level_dbfs / 20.0)
    out = []
    phase = 0.0
    accumulator = 0
    index = 0
    while index < len(bits):
        freq = mark_hz if bits[index] else space_hz
        out.append(int(round(amplitude * math.sin(phase))))
        phase += 2.0 * math.pi * freq / sample_rate
        if phase >= 2.0 * math.pi:
            phase -= 2.0 * math.pi
        accumulator += int(BAUD)
        if accumulator >= sample_rate:
            accumulator -= sample_rate
            index += 1
    return out


def message_build(kind, params):
    body = []
    for tag, value in params:
        body += [tag, len(value)] + list(value)
    data = [kind, len(body)] + body
    return data + [(-sum(data)) & 0xFF]


def self_test():
    """Three synthetic captures through the mu-law round trip: a Bell 202
    MDMF call setup that must decode byte for byte, a V.23 record, and the
    same message with its last 14 bytes cut, which must report a missing
    checksum and a nonzero exit."""
    message = message_build(0x80, [(0x01, b"08292206"), (0x02, b"5551234"),
                                   (0x07, b"TEST CALLER")])
    failures = []

    def run(name, samples, carriers, want_ok, want_bytes):
        decoded = ua.ulaw_decode(ua.ulaw_encode(samples))
        results = capture_decode(decoded, carriers)
        got_ok = any(ok for ok, _ in results)
        got_bytes = None
        for ok, lines in results:
            for line in lines:
                if line.strip().startswith("bytes("):
                    got_bytes = [int(v, 16) for v in line.split(":")[1].split()]
        if got_ok != want_ok or got_bytes != want_bytes:
            failures.append(name)
            for _, lines in results:
                print("\n".join(lines))

    silence = [0] * 2000
    burst = modulate(message, BELL202, 300, 180, -17.0)
    run("bell202", silence + burst + silence, BELL202, True, message)
    burst = modulate(message, V23, 0, 80, -17.0)
    run("v23", silence + burst + silence, V23, True, message)
    cut = message[:-14]
    burst = modulate(cut, BELL202, 300, 180, -17.0)
    run("truncated", silence + burst + silence, BELL202, False, cut)
    print("fsk-demod self-test: %s" %
          ("ok" if not failures else "FAILED " + ", ".join(failures)))
    return 0 if not failures else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("capture", nargs="?",
                        help="line capture or a mono 16 bit WAV")
    parser.add_argument("--ulaw", action="store_true",
                        help="read capture input even when its name ends in .wav")
    parser.add_argument("--v23", action="store_true",
                        help="V.23 carriers (1300/2100 Hz) instead of Bell 202")
    parser.add_argument("--self-test", action="store_true",
                        help="decode synthetic bursts and check the bytes")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.capture:
        parser.error("a capture file is required")
    samples, sample_rate = ua.audio_load(args.capture, args.ulaw)
    results = capture_decode(samples, V23 if args.v23 else BELL202,
                             sample_rate)
    for _, lines in results:
        print("\n".join(lines))
    if not results:
        print("no burst found")
    return 0 if any(ok for ok, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
