#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-only
#
# udsp - software voice DSP for FXS lines
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
"""Timeline of a udsp-linecap capture: per frame RMS and band amplitude.

Reads versioned or headerless PCMU captures and mono 16 bit WAV files. Analysis
and WAV output use the sample rate recorded by the input.

The default bands are the Bell 202 mark and space (1200 and 2200 Hz) and the
GR-506 dial tone pair (350 and 440 Hz). A channel seizure (alternating mark
and space at 1200 baud) puts its energy between the two carriers, not on
them, so a frame with a loud RMS and quiet 1200/2200 columns is the seizure,
not noise."""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import udsp_audio as ua  # noqa: E402

FRAME_MS_DEFAULT = 20
BURST_THRESHOLD_DBFS = -50.0
BURST_GAP_FRAMES = 4
QUIET_EVERY_FRAMES = 50


def frames_of(samples, frame_ms, sample_rate):
    size = sample_rate * frame_ms // 1000
    for start in range(0, len(samples) - size + 1, size):
        yield start * 1000 // sample_rate, samples[start:start + size]


def bursts_find(samples, frame_ms, threshold_dbfs,
                sample_rate=ua.SAMPLE_RATE):
    """Runs of frames above the threshold, joined across gaps shorter than
    BURST_GAP_FRAMES; each is (start_ms, end_ms, peak_rms_dbfs)."""
    found = []
    current = None
    quiet = 0
    for start_ms, frame in frames_of(samples, frame_ms, sample_rate):
        level = ua.dbfs(ua.rms(frame))
        loud = level > threshold_dbfs
        if loud:
            if current is None:
                current = [start_ms, start_ms + frame_ms, level]
            else:
                current[1] = start_ms + frame_ms
                current[2] = max(current[2], level)
            quiet = 0
            continue
        if current is not None:
            quiet += 1
            if quiet >= BURST_GAP_FRAMES:
                found.append(tuple(current))
                current = None
                quiet = 0
    if current is not None:
        found.append(tuple(current))
    return found


def timeline_print(samples, frame_ms, bands, threshold_dbfs, show_all,
                   sample_rate):
    print("%7s %9s" % ("t_ms", "rms_dBFS") +
          "".join(" %7s" % ("%dHz" % b) for b in bands))
    for index, (start_ms, frame) in enumerate(
            frames_of(samples, frame_ms, sample_rate)):
        level = ua.dbfs(ua.rms(frame))
        if not show_all and level <= threshold_dbfs and \
           index % QUIET_EVERY_FRAMES:
            continue
        columns = "".join(
            " %7.1f" % ua.dbfs(ua.goertzel(frame, b, sample_rate))
            for b in bands)
        print("%7d %9.1f%s" % (start_ms, level, columns))


def self_test():
    """A synthetic capture: silence, a 1 kHz tone, silence, a dial tone pair,
    through the mu-law round trip. The bursts and the band columns must fall
    where the synthesis put them."""
    amp = 32768.0 * 10 ** (-20.0 / 20.0)
    samples = [0] * 4000
    burst1, _ = ua.tone(1000.0, 2400, amp)
    samples += burst1 + [0] * 4000
    low, _ = ua.tone(350.0, 4000, amp / 2)
    high, _ = ua.tone(440.0, 4000, amp / 2)
    samples += [a + b for a, b in zip(low, high)] + [0] * 2000
    decoded = ua.ulaw_decode(ua.ulaw_encode(samples))
    found = bursts_find(decoded, FRAME_MS_DEFAULT, BURST_THRESHOLD_DBFS)
    ok = len(found) == 2
    ok = ok and abs(found[0][0] - 500) <= FRAME_MS_DEFAULT
    ok = ok and abs(found[0][1] - 800) <= FRAME_MS_DEFAULT
    ok = ok and abs(found[1][0] - 1300) <= FRAME_MS_DEFAULT
    ok = ok and abs(found[1][1] - 1800) <= FRAME_MS_DEFAULT
    ok = ok and abs(found[0][2] - (-20.0 - 3.0)) < 1.0
    frame = decoded[5600:5760]
    ok = ok and abs(ua.dbfs(ua.goertzel(frame, 1000.0)) - (-20.0)) < 1.0
    ok = ok and ua.dbfs(ua.goertzel(frame, 1200.0)) < -40.0
    frame = decoded[12000:12160]
    ok = ok and abs(ua.dbfs(ua.goertzel(frame, 350.0)) - (-26.0)) < 1.5
    ok = ok and abs(ua.dbfs(ua.goertzel(frame, 440.0)) - (-26.0)) < 1.5
    print("linecap-analyse self-test: %s (%d bursts)" %
          ("ok" if ok else "FAILED", len(found)))
    return 0 if ok else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("capture", nargs="?",
                        help="line capture or a mono 16 bit WAV")
    parser.add_argument("--ulaw", action="store_true",
                        help="read capture input even when its name ends in .wav")
    parser.add_argument("--wav", metavar="PATH",
                        help="also write the capture as a 16 bit WAV")
    parser.add_argument("--frame-ms", type=int, default=FRAME_MS_DEFAULT,
                        help="analysis frame (default %d)" % FRAME_MS_DEFAULT)
    parser.add_argument("--bands", default="1200,2200,350,440",
                        help="frequencies to report, comma separated")
    parser.add_argument("--threshold", type=float, default=BURST_THRESHOLD_DBFS,
                        help="burst threshold in dBFS (default %.0f)" %
                        BURST_THRESHOLD_DBFS)
    parser.add_argument("--all", action="store_true",
                        help="print every frame, not only the loud ones")
    parser.add_argument("--self-test", action="store_true",
                        help="check the analysis on a synthetic capture")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.capture:
        parser.error("a capture file is required")
    bands = [float(b) for b in args.bands.split(",") if b]
    samples, sample_rate = ua.audio_load(args.capture, args.ulaw)
    print("samples %d rate %d Hz (%.2f s)" %
          (len(samples), sample_rate, len(samples) / sample_rate))
    if args.wav:
        ua.wav_write(args.wav, samples, sample_rate)
        print("wav %s" % args.wav)
    for start_ms, end_ms, peak in bursts_find(samples, args.frame_ms,
                                              args.threshold, sample_rate):
        print("burst %d..%d ms (%d ms) peak %.1f dBFS" %
              (start_ms, end_ms, end_ms - start_ms, peak))
    timeline_print(samples, args.frame_ms, bands, args.threshold, args.all,
                   sample_rate)
    return 0


if __name__ == "__main__":
    sys.exit(main())
