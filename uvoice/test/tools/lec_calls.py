#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
"""Summarize each call of one echo canceller from its trace lines.

usage: lec_calls.py <capture> [skip]
       lec_calls.py --check [fixture ...]

The script reads the two per-second canceller lines of the media trace domain,
with the parser and the rules of lec_duty.py: the capture holds one canceller
line, keyed by the time of day to the second.

A call starts at the first counts line of the capture. A new call starts at
every counts line whose frames fall against the line before. The frames fall
when a new canceller starts and when a re-time zeroes the counts, so a re-time
also starts a new call. A second whose frames equal those of the second before
gives no delta and does not count. Each second with a delta counts as one
second of the call.

The script drops the first skip seconds of each call, 15 by default, because
they carry the ringback tone and the transition to the answered call. The
seconds that remain with 20 judged frames or more are the body of the call.
The judged frames are the five counts other than quiet: active, wait, erle,
unlike and dt. The duty of a second is its active count over its judged
frames.

For each call with a body of 5 seconds or more, the script prints three lines:

1. the first and the last second of the call, the seconds of the call, and
   the seconds of the body;
2. over the body: the median and quartiles of the ERLE (erle_mdb of the first
   line of each second, in dB), the median and quartiles of the duty, and the
   count of seconds with duty 0;
3. each judged count over the judged frames of the body, and the quiet frames
   over all frames of the body.

A call with a shorter body prints one line with its seconds and "too short
after the skip". No line asserts anything.

--check runs the script on the fixture texts beside it, or on the files given.
A fixture names the script and the skip with "# check lec_calls skip N" and
states each line of the output it expects with "# expect <line>". Blank lines
do not count. The check exits 1 on a difference.
"""
import sys

from lec_duty import (JUDGED_KEYS, JUDGED_MIN, KEYS, check, counts_get,
                      judged_get, lines_parse, quartiles_get)

SKIP_S = 15
BODY_MIN = 5
FIXTURES = ["lec_calls_silent.txt", "lec_calls_twoway.txt",
            "lec_calls_reset.txt"]


def calls_get(first, second):
    """The calls of a capture. Each call is a list of seconds: the time, the
    deltas and the first line of that second."""
    calls, call, prev = [], [], None
    for t in sorted(second):
        cur = counts_get(second[t])
        if prev is None or cur["frames"] < prev["frames"]:
            if call:
                calls.append(call)
            call = []
        elif cur["frames"] > prev["frames"]:
            d = {k: cur[k] - prev[k] for k in KEYS}
            call.append((t, d, first.get(t, {})))
        prev = cur
    if call:
        calls.append(call)
    return calls


def call_lines(n, call, skip):
    body = [s for s in call[skip:] if judged_get(s[1]) >= JUDGED_MIN]
    if len(body) < BODY_MIN:
        return [f"call {n}: {len(call)} s, too short after the skip"]
    tot = {k: sum(d[k] for _, d, _ in body) for k in KEYS}
    judged = judged_get(tot)
    duties = [100.0 * d["active"] / judged_get(d) for _, d, _ in body]
    erles = [int(f["erle_mdb"]) / 1000 for _, _, f in body if "erle_mdb" in f]
    dm, d25, d75 = quartiles_get(duties)
    em, e25, e75 = quartiles_get(erles)
    zero = sum(1 for x in duties if x == 0)
    shares = ", ".join(f"{k} {100.0 * tot[k] / judged:.0f}%"
                       for k in JUDGED_KEYS)
    quiet = 100.0 * tot["quiet"] / tot["frames"]
    return [
        f"call {n}: {call[0][0]} to {call[-1][0]}, {len(call)} s, "
        f"{len(body)} judged seconds after the first {skip} s",
        f"  ERLE median {em:.1f} dB (p25 {e25:.1f}, p75 {e75:.1f}); "
        f"duty per second median {dm:.0f}% (p25 {d25:.0f}, p75 {d75:.0f}); "
        f"seconds with duty 0: {zero}",
        f"  share of judged frames: {shares}; quiet frames {quiet:.0f}% of all",
    ]


def report_lines(lines, skip):
    first, second = lines_parse(lines)
    out = []
    for n, call in enumerate(calls_get(first, second), 1):
        out += call_lines(n, call, skip)
    return out


def main(path, skip):
    with open(path) as f:
        for text in report_lines(f, skip):
            print(text)


def fixture_report(trace, args):
    if len(args) != 2 or args[0] != "skip":
        raise ValueError("the check line names no skip")
    return report_lines(trace, int(args[1]))


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "--check":
        sys.exit(check(sys.argv[2:], "lec_calls", FIXTURES, fixture_report))
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__.split("\n\n")[1])
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) == 3 else SKIP_S)
