#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
"""Tabulate the processor duty of one echo canceller, second by second.

usage: lec_duty.py <capture>
       lec_duty.py --check [fixture ...]

Under the media trace domain, uvoiced prints two lines per second for each
line with a canceller (media_lec_trace() in src/media_trace.c):

    l<N> lec ec=W nlp=W erle_mdb=M ref_ready=R retimes=T kind=K
        loop_open=L peak_tap=P
    l<N> lec nlp frames=F active=A wait=W erle=E unlike=U dt=D quiet=Q

The second line carries cumulative counts. Each frame that the filter ran with
the processor on adds one to one of the six verdict counts. A re-time zeroes
them with frames.

The script reads every such line of a logread capture, whatever the prefix
before it. It keys each line by its time of day to the second, so a later line
of one kind in the same second replaces the earlier one. The time of day also
sorts the seconds, so a capture must not cross midnight. The script does not
separate two canceller lines: a capture must hold one, as lec-trace.sh writes
it. Each second with a counts line gives the deltas against the second before.
A second whose frames fall against the second before gives no row: a new
canceller or a re-time zeroed the counts. A second with no new frame prints
no row.

The columns of the table:

    time       the second, from the time of day of the capture
    frames     the frames of the second
    judged     the sum of the five counts other than quiet: active, wait,
               erle, unlike and dt
    duty       the acted share: active over judged
    wait       wait over judged: eligible, inside the engage run
    erle       erle over judged: refused by the ERLE gate
    unlike     unlike over judged: refused as not echo-like
    dt         dt over judged: refused by double talk
    quiet      the quiet frames, a count: the reference window was inactive
    erle_dB    erle_mdb of the first line of the same second, in dB
    nlp        the nlp word of that line
    peak       its peak_tap
    loop_open  its loop_open

A share reads "-" in a second with no judged frame. A second with no first
line reads 0.0 and "?". The summary covers the seconds with 20 judged frames
or more. It gives the median and quartiles of the duty, the count of seconds
with duty 0, and each judged count over the judged frames of those seconds.
It then gives the median and quartiles of the ERLE. No line asserts anything.

--check runs the script on the fixture texts beside it, or on the files given.
A fixture names the script with "# check lec_duty" and states each line of
the output it expects with "# expect <line>". Blank lines do not count. The
check exits 1 on a difference.
"""
import itertools
import re
import statistics
import sys
from pathlib import Path

LINE_RE = re.compile(r"(\d\d:\d\d:\d\d)\.\d+ .* l(\d) lec (nlp )?(.*)$")
KEYS = ["frames", "active", "wait", "erle", "unlike", "dt", "quiet"]
JUDGED_KEYS = ["active", "wait", "erle", "unlike", "dt"]
JUDGED_MIN = 20
HEADER = ("time     frames judged  duty  wait  erle unlike   dt  quiet | "
          "erle_dB nlp    peak loop_open")
FIXTURES = ["lec_duty_silent.txt", "lec_duty_twoway.txt",
            "lec_duty_reset.txt"]


def lines_parse(lines):
    """The first lines and the counts lines of a capture, each a dict of its
    key=value words, keyed by the second of its time of day."""
    first, second = {}, {}
    for text in lines:
        m = LINE_RE.search(text)
        if not m:
            continue
        t, _, nlp, rest = m.groups()
        kv = dict(p.split("=") for p in rest.split() if "=" in p)
        (second if nlp else first)[t] = kv
    return first, second


def counts_get(kv):
    return {k: int(kv[k]) for k in KEYS}


def judged_get(counts):
    return sum(counts[k] for k in JUDGED_KEYS)


def quartiles_get(values):
    """The median, the first quartile and the third quartile."""
    q = statistics.quantiles(values, n=4)
    return statistics.median(values), q[0], q[2]


def rows_get(first, second):
    """One row per second whose frames did not fall against the second
    before: the time, the deltas, the judged count and the first line."""
    rows = []
    prev = None
    for t in sorted(second):
        cur = counts_get(second[t])
        if prev is not None and cur["frames"] >= prev["frames"]:
            d = {k: cur[k] - prev[k] for k in KEYS}
            rows.append((t, d, judged_get(d), first.get(t, {})))
        prev = cur
    return rows


def share_text(part, judged):
    return f"{100.0 * part / judged:4.0f}%" if judged else "   -"


def row_text(t, d, judged, f):
    shares = " ".join(share_text(d[k], judged) for k in JUDGED_KEYS)
    return (f"{t} {d['frames']:6d} {judged:6d} {shares} {d['quiet']:6d} | "
            f"{int(f.get('erle_mdb', 0)) / 1000:6.1f} "
            f"{f.get('nlp', '?'):6s} {f.get('peak_tap', '?'):>4s} "
            f"{f.get('loop_open', '?')}")


def summary_lines(rows):
    out = []
    kept = [(d, j) for _, d, j, _ in rows if j >= JUDGED_MIN]
    if kept:
        duties = [100.0 * d["active"] / j for d, j in kept]
        med, p25, p75 = quartiles_get(duties)
        zero = sum(1 for x in duties if x == 0)
        out.append("")
        out.append(f"seconds with >= {JUDGED_MIN} judged frames: {len(kept)}; "
                   f"duty median {med:.0f}% (p25 {p25:.0f}, p75 {p75:.0f}); "
                   f"seconds with duty 0: {zero}")
        tot = {k: sum(d[k] for d, _ in kept) for k in JUDGED_KEYS}
        judged = sum(j for _, j in kept)
        out.append("share of judged frames over those seconds: " +
                   ", ".join(f"{k} {100.0 * tot[k] / judged:.0f}%"
                             for k in JUDGED_KEYS))
    erles = [int(f["erle_mdb"]) / 1000 for _, _, j, f in rows
             if j >= JUDGED_MIN and "erle_mdb" in f]
    if erles:
        med, p25, p75 = quartiles_get(erles)
        out.append(f"ERLE over those seconds: median {med:.1f} dB "
                   f"(p25 {p25:.1f}, p75 {p75:.1f})")
    return out


def report_lines(lines):
    first, second = lines_parse(lines)
    rows = rows_get(first, second)
    out = [HEADER]
    out += [row_text(*row) for row in rows if row[1]["frames"] != 0]
    return out + summary_lines(rows)


def main(path):
    with open(path) as f:
        for text in report_lines(f):
            print(text)


def fixture_check(path, script, report):
    """True when the output of report on the trace lines of the fixture is
    the output the fixture expects. report takes the trace lines and the
    words of the "# check <script>" line after the script name."""
    name = Path(path).name
    text = Path(path).read_text().splitlines()
    args = None
    for line in text:
        words = line.split()
        if words[:3] == ["#", "check", script]:
            args = words[3:]
    if args is None:
        print(f"{name}: no '# check {script}' directive")
        return False
    expect = [line[len("# expect "):].rstrip() for line in text
              if line.startswith("# expect ")]
    trace = [line for line in text if not line.startswith("#")]
    try:
        got = [line.rstrip() for line in report(trace, args) if line.strip()]
    except (KeyError, ValueError, statistics.StatisticsError) as e:
        print(f"{name}: the fixture did not read ({e!r})")
        return False
    if got == expect:
        print(f"{name}: {len(got)} lines as expected")
        return True
    print(f"{name}: output differs")
    pairs = itertools.zip_longest(expect, got, fillvalue="-")
    for n, (want, have) in enumerate(pairs, 1):
        if want != have:
            print(f"  line {n}: expect {want}")
            print(f"  line {n}: got    {have}")
    return False


def check(paths, script, fixtures, report):
    here = Path(__file__).resolve().parent
    paths = paths or [here / name for name in fixtures]
    results = [fixture_check(p, script, report) for p in paths]
    return 0 if all(results) else 1


def fixture_report(trace, args):
    return report_lines(trace)


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "--check":
        sys.exit(check(sys.argv[2:], "lec_duty", FIXTURES, fixture_report))
    if len(sys.argv) != 2:
        sys.exit(__doc__.split("\n\n")[1])
    main(sys.argv[1])
