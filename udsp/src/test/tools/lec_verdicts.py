#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-only
#
# udsp - software voice DSP for FXS lines
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
"""Bin the double-talk verdict trace of one line out of a logread capture.

usage: lec_verdicts.py <capture> <line>
       lec_verdicts.py --check [fixture ...]

libudsp prints the trace under UDSP_DEBUG_SLIC_LOG, which uvoice sets with
the hal trace domain. Each record is one line of fixed words and word-value
pairs:

    lec verdict line N gen G at MS frame F excess DB hold H arm A trusted T
        tail Q since S nlp V probing P mic DB ref DB win DB est DB noise DB
        erle DB run R stale U
    lec second line N gen G at MS frame F fired X dt D probes P lost L
        gain DB

The stale word is the stale run of the detector. The script also reads a
verdict line without the stale word.

The script reads the words of every such line, whatever the logread prefix
before them, and keeps the records of the line given. It prints three parts:

1. Per second: the deltas between consecutive SECOND lines of one segment.
   A segment ends at every change of gen and at every decrease of frame
   against the line before, of either kind. It also ends where a SECOND line
   repeats the frame of the SECOND line before it. A re-time, a new
   canceller and a daemon restart each start a segment. A verdict line can
   be the only sign of a restart. The first SECOND line of a segment has no
   predecessor and gives no row. The span column is the frame distance of
   the two lines. A span over 100 is a missing SECOND line, and its deltas
   cover the whole span. The script never fills a gap with zeros. The hold
   column is 1 minus the verdict delta over the dt delta, and "-" for a dt
   delta of 0. A lost above 0 marks the interval.
2. The verdict lines by excess, in five bins, each split three ways: fresh
   (hold 0) against re-arm, by the arm that set the bar, and by the place in
   the talkspurt. A bin holds its lower edge, so 3.0 dB falls in "3 to 6"
   and 12.0 dB in "over 12". The place is the tail when tail reads 1, the
   onset when since is under 8, and the middle otherwise. A line whose nlp
   word is not dt counts apart, in "other". The two marginal bins also count
   the soft lines: a measured floor, and the microphone within 12 dB of it.
   That is one of the four conditions of the short hangover. The excess
   shows the marginal condition and the hold word a running hangover; the
   trace does not show the repeat limit, because it samples the re-arms.
3. The totals: the marginal share (under 3 dB), the strong share (over 12 dB)
   and the hold share. The last line counts the verdict lines that carry
   the stale word and gives the largest stale run among them.

The tick pushes every fresh verdict and one re-arm in eight, so the verdict
lines sample the re-arms, and the per-second rows carry the full counts. No
row asserts anything.

--check runs the per-second rule on the fixture texts beside the script, or on
the files given. A fixture names its line with "# check line N" and states
each row it expects with "# expect gen frame span verdict dt hold probes
lost". The check exits 1 on a difference.
"""
import itertools
import sys
from pathlib import Path

SECOND_FRAMES = 100
ONSET_FRAMES = 8
MARGINAL_DB = 3.0
STRONG_DB = 12.0
SOFT_DB = 12.0
FLOOR_NONE_DB = -127.0
BINS = [(None, 1.5, "under 1.5"), (1.5, 3.0, "1.5 to 3"),
        (3.0, 6.0, "3 to 6"), (6.0, 12.0, "6 to 12"), (12.0, None, "over 12")]
FIXTURES = ["lec_verdicts_retime.txt", "lec_verdicts_missing.txt",
            "lec_verdicts_dt_zero.txt", "lec_verdicts_restart.txt"]

INT_WORDS = {"line", "gen", "at", "frame", "hold", "trusted", "tail", "since",
             "probing", "run", "stale", "fired", "dt", "probes", "lost"}
TEXT_WORDS = {"arm", "nlp"}
VERDICT_WORDS = {"line", "gen", "at", "frame", "excess", "hold", "arm",
                 "trusted", "tail", "since", "nlp", "probing", "mic", "ref",
                 "win", "est", "noise", "erle", "run"}
SECOND_WORDS = {"line", "gen", "at", "frame", "fired", "dt", "probes", "lost",
                "gain"}


def value_get(name, text):
    if name in TEXT_WORDS:
        return text
    if name in INT_WORDS:
        return int(text)
    return float(text)


def record_parse(text):
    """The kind and the values of one trace line. None for a line that
    carries no record, and False for a record that does not read."""
    if text.lstrip().startswith("#"):
        return None
    for kind in ("verdict", "second"):
        at = text.find(f"lec {kind} ")
        if at >= 0:
            break
    else:
        return None
    words = text[at + len(f"lec {kind} "):].split()
    need = VERDICT_WORDS if kind == "verdict" else SECOND_WORDS
    try:
        rec = {k: value_get(k, v) for k, v in zip(words[0::2], words[1::2])}
    except ValueError:
        return False
    if not need <= rec.keys():
        return False
    rec["kind"] = kind
    return rec


def records_read(lines, line_no):
    """The verdict records and the SECOND records of one line, in capture
    order, and the count of the trace lines that did not read. Each record
    carries its segment number. The number moves when gen changes or frame
    falls against the record before it, of either kind. Reason: a verdict
    record between two SECOND records can be the only sign of a restart."""
    verdicts, seconds, bad = [], [], 0
    prev = None
    segment = 0
    for text in lines:
        rec = record_parse(text)
        if rec is None:
            continue
        if rec is False:
            bad += 1
            continue
        if rec["line"] != line_no:
            continue
        if prev is not None and (rec["gen"] != prev["gen"] or
                                 rec["frame"] < prev["frame"]):
            segment += 1
        rec["segment"] = segment
        prev = rec
        (verdicts if rec["kind"] == "verdict" else seconds).append(rec)
    return verdicts, seconds, bad


def hold_text(fired, dt):
    return "-" if dt == 0 else f"{1.0 - fired / dt:.2f}"


def seconds_rows(seconds):
    """One row per SECOND record that has a predecessor in its segment. The
    frame of a SECOND record never repeats inside one segment. An equal frame
    therefore starts a segment too: a restart between two SECOND records at
    frame 100 leaves no other sign."""
    rows = []
    prev = None
    for cur in seconds:
        if (prev is None or cur["segment"] != prev["segment"] or
                cur["gen"] != prev["gen"] or cur["frame"] <= prev["frame"]):
            prev = cur
            continue
        fired = cur["fired"] - prev["fired"]
        dt = cur["dt"] - prev["dt"]
        rows.append(dict(gen=cur["gen"], frame=cur["frame"],
                         span=cur["frame"] - prev["frame"], verdict=fired,
                         dt=dt, hold=hold_text(fired, dt),
                         probes=cur["probes"] - prev["probes"],
                         lost=cur["lost"]))
        prev = cur
    return rows


def row_text(row):
    return (f"{row['gen']} {row['frame']} {row['span']} {row['verdict']} "
            f"{row['dt']} {row['hold']} {row['probes']} {row['lost']}")


def bin_get(excess):
    for i, (lo, hi, _) in enumerate(BINS):
        if (lo is None or excess >= lo) and (hi is None or excess < hi):
            return i
    return len(BINS) - 1


def place_get(rec):
    if rec["tail"]:
        return "tail"
    if rec["since"] < ONSET_FRAMES:
        return "onset"
    return "mid"


def soft_get(rec):
    """The microphone stands within SOFT_DB of a measured floor, the soft
    condition of the short hangover. The trace prints a floor of 0 as
    -127.0."""
    return (rec["noise"] > FLOOR_NONE_DB and
            rec["mic"] - rec["noise"] < SOFT_DB)


def histogram_get(verdicts):
    cols = ["lines", "fresh", "rearm", "classic", "est", "tail", "onset",
            "mid", "other", "soft"]
    table = [dict.fromkeys(cols, 0) for _ in BINS]
    for rec in verdicts:
        cell = table[bin_get(rec["excess"])]
        cell["lines"] += 1
        if rec["nlp"] != "dt":
            cell["other"] += 1
            continue
        cell["fresh" if rec["hold"] == 0 else "rearm"] += 1
        cell["classic" if rec["arm"] == "classic" else "est"] += 1
        cell[place_get(rec)] += 1
        if rec["excess"] < MARGINAL_DB and soft_get(rec):
            cell["soft"] += 1
    return cols, table


def share_text(part, whole):
    return "-" if whole == 0 else f"{100.0 * part / whole:.0f}%"


def rows_print(line_no, rows):
    print(f"line {line_no}, per second: the deltas between consecutive "
          f"second lines of one segment")
    print("  gen  frame  span verdict     dt  hold probes  lost note")
    for row in rows:
        note = []
        if row["span"] != SECOND_FRAMES:
            note.append("gap")
        if row["lost"]:
            note.append("lost")
        print(f"{row['gen']:5d} {row['frame']:6d} {row['span']:5d} "
              f"{row['verdict']:7d} {row['dt']:6d} {row['hold']:>5s} "
              f"{row['probes']:6d} {row['lost']:5d} "
              f"{' '.join(note)}".rstrip())


def histogram_print(verdicts):
    cols, table = histogram_get(verdicts)
    print("\nverdict lines by excess in dB: the splits count the lines whose "
          "nlp word is dt, other counts the rest, soft counts the marginal "
          "lines with the microphone within 12 dB of a measured floor")
    print("excess       " + " ".join(f"{c:>7s}" for c in cols))
    for (_, hi, name), cell in zip(BINS, table):
        marginal = hi is not None and hi <= MARGINAL_DB
        cells = [f"{'-':>7s}" if c == "soft" and not marginal
                 else f"{cell[c]:7d}" for c in cols]
        print(f"{name:12s} " + " ".join(cells))


def totals_print(verdicts, seconds, rows, bad):
    n = len(verdicts)
    marginal = sum(1 for r in verdicts if r["excess"] < MARGINAL_DB)
    strong = sum(1 for r in verdicts if r["excess"] >= STRONG_DB)
    fired = sum(r["verdict"] for r in rows)
    dt = sum(r["dt"] for r in rows)
    lost = sum(r["lost"] for r in seconds)
    print(f"\ntotals: {n} verdict lines, re-arms sampled one in eight; "
          f"marginal (under {MARGINAL_DB:.0f} dB) {share_text(marginal, n)}; "
          f"strong (over {STRONG_DB:.0f} dB) {share_text(strong, n)}")
    print(f"hold share {hold_text(fired, dt)} over {len(rows)} intervals "
          f"({fired} verdict frames of {dt} dt frames); lost records {lost} "
          f"over {len(seconds)} second lines; trace lines that did not read "
          f"{bad}")
    stale = [r["stale"] for r in verdicts if "stale" in r]
    print(f"stale word on {len(stale)} of {n} verdict lines; largest stale "
          f"run {max(stale) if stale else '-'}")


def main(path, line_no):
    with open(path, errors="replace") as f:
        verdicts, seconds, bad = records_read(f, line_no)
    rows = seconds_rows(seconds)
    rows_print(line_no, rows)
    histogram_print(verdicts)
    totals_print(verdicts, seconds, rows, bad)


def fixture_check(path):
    """True when the rows of the fixture are the rows it expects."""
    line_no = None
    expect = []
    text = Path(path).read_text().splitlines()
    for line in text:
        if line.startswith("# check line "):
            line_no = int(line.split()[3])
        elif line.startswith("# expect "):
            expect.append(line[len("# expect "):].strip())
    if line_no is None:
        print(f"{Path(path).name}: no '# check line' directive")
        return False
    _, seconds, bad = records_read(text, line_no)
    got = [row_text(r) for r in seconds_rows(seconds)]
    name = Path(path).name
    if got == expect and bad == 0:
        print(f"{name}: {len(got)} rows as expected")
        return True
    print(f"{name}: rows differ ({bad} lines did not read)")
    for want, have in itertools.zip_longest(expect, got, fillvalue="-"):
        print(f"  expect {want:30s} got {have}")
    return False


def check(paths):
    here = Path(__file__).resolve().parent
    paths = paths or [here / name for name in FIXTURES]
    results = [fixture_check(p) for p in paths]
    return 0 if all(results) else 1


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "--check":
        sys.exit(check(sys.argv[2:]))
    if len(sys.argv) != 3:
        sys.exit(__doc__.split("\n\n")[1])
    main(sys.argv[1], int(sys.argv[2]))
