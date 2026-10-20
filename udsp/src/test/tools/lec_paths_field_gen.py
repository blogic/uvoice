#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-only
#
# udsp - software voice DSP for FXS lines
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
"""Write the captured echo paths of the LEC bench as a C header.

src/test/tools/path_fit.py fits a tap table from a call trace archive into
src/test/bench/field/path-<platform>-<backend>-line<N>k.txt. This script
reads every table there and writes src/test/bench/lec_paths_field.h: one
static const float array of FIELD_PATH_TAPS per table and the field_paths[]
descriptor table, in the order of the capture times.

- The file name gives the platform, the back end and the line rate.
- The first header line of a table gives the onset tap, the onset time, the
  uplink level and the residual rms.
- The second header line gives the key=value facts of path_fit.py.
- Each tap keeps the digits of the table and takes an f suffix, so a second
  run cannot change a value.

Paths resolve from the place of this script, so it runs from any directory:

    lec_paths_field_gen.py            rewrite the header
    lec_paths_field_gen.py --check    exit 1 when the header would change

The header is written to a temporary file first and then renamed, so a
failed write leaves the old header in place.
"""

import argparse
import contextlib
import os
import re
import stat
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
SRC = TOOLS.parent.parent
FIELD_DIR = SRC / "test" / "bench" / "field"
HEADER = SRC / "test" / "bench" / "lec_paths_field.h"
SELF = f"src/test/tools/{Path(__file__).name}"
TAPS = 512
NAME_MAX = 13
TAPS_PER_LINE = 4

FILE_RE = re.compile(r"path-(?P<platform>[a-z0-9]+)-(?P<backend>[a-z0-9]+)"
                     r"-line(?P<khz>\d+)k\.txt")
HEAD_RE = re.compile(r"# echo path at the 8 kHz of the wire, (?P<taps>\d+) "
                     r"taps, tap (?P<onset_tap>\d+) is the onset lobe at "
                     r"(?P<onset_ms>[\d.]+) ms; uplink (?P<uplink>[\d.]+) dB "
                     r"under the far end, fit erle (?P<erle>[\d.]+) dB, "
                     r"residual rms (?P<residual>[\d.]+); source "
                     r"(?P<source>\S+)")
NUMBER_RE = re.compile(r"\d+(\.\d+)?")
TAP_RE = re.compile(r"[-+]?\d\.\d+e[-+]\d+")
SOURCE_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")
FACTS = ["far_rms", "erle_48", "erle_48_onset", "erle_48_1ms", "erle_48_5ms",
         "erle_48_10ms_before", "erle_64", "erle_64_onset", "source"]

BANNER = f"""\
// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The captured echo paths of the LEC bench.
 * {SELF} writes this file from the tap tables
 * in src/test/bench/field/. Do not edit it: change a table or the generator,
 * and run the generator again. Its --check mode exits 1 when this file and
 * the tables disagree.
 *
 * The taps past about 40 ms after the onset carry estimation noise of the
 * fit. They are kept as fitted. The rms of the tail falls toward the end of
 * a table, so no flat noise floor is visible to cut at. The bench path then
 * equals the path of the fit, and the ideal ERLE of the fit applies to it.
 */
#ifndef UDSP_TEST_LEC_PATHS_FIELD_H
#define UDSP_TEST_LEC_PATHS_FIELD_H

#include "lec_paths.h"
"""


def fail(path, why):
    raise SystemExit(f"{path}: {why}")


def number_get(path, key, text):
    if not NUMBER_RE.fullmatch(text):
        fail(path, f"{key}={text} is not a decimal number")
    return text


def table_read(path):
    m = FILE_RE.fullmatch(path.name)
    if not m:
        fail(path, "the name is not path-<platform>-<backend>-line<N>k.txt")
    lines = path.read_text(encoding="utf-8").splitlines()
    if len(lines) < 2:
        fail(path, "no header lines")
    head = HEAD_RE.fullmatch(lines[0])
    if not head:
        fail(path, "the first header line is not the one path_fit.py writes")
    if not lines[1].startswith("# "):
        fail(path, "the second header line holds no facts")
    facts = dict(item.partition("=")[::2] for item in lines[1][2:].split())
    missing = [k for k in FACTS if not facts.get(k)]
    if missing:
        fail(path, f"facts missing: {', '.join(missing)}")
    if facts["source"] != head["source"]:
        fail(path, "the two header lines name different sources")
    if not SOURCE_RE.fullmatch(facts["source"]):
        fail(path, f"the source {facts['source']!r} is not an archive name")
    if facts["erle_64"] != head["erle"]:
        fail(path, "erle_64 differs from the fit erle of the first line")
    taps = lines[2:]
    if int(head["taps"]) != TAPS or len(taps) != TAPS:
        fail(path, f"{len(taps)} taps, not {TAPS}")
    for n, tap in enumerate(taps):
        if not TAP_RE.fullmatch(tap):
            fail(path, f"tap {n} ({tap!r}) is not a number in e notation")
    line_rate = int(m["khz"]) * 1000
    name = f"{m['platform']} {m['backend']}"
    if line_rate == 8000:
        name += " 8k"
    if len(name) > NAME_MAX:
        fail(path, f"the row name {name!r} has more than {NAME_MAX} "
             "characters")
    for key in FACTS[:-1]:
        number_get(path, key, facts[key])
    for key in ("onset_ms", "uplink", "residual"):
        number_get(path, key, head[key])
    return dict(file=path.name, ident="field_" + path.stem[len("path-"):]
                .replace("-", "_"), name=name, platform=m["platform"],
                backend=m["backend"], line_rate=line_rate,
                onset_tap=head["onset_tap"], onset_ms=head["onset_ms"],
                uplink=head["uplink"], residual=head["residual"],
                facts=facts, taps=taps)


def array_render(t):
    out = [f"/* {t['file']} */",
           f"static const float {t['ident']}[FIELD_PATH_TAPS] = {{"]
    for i in range(0, TAPS, TAPS_PER_LINE):
        row = ", ".join(f"{tap}f" for tap in t["taps"][i:i + TAPS_PER_LINE])
        out.append(f"\t{row},")
    out.append("};")
    return "\n".join(out) + "\n"


def entry_render(t):
    f = t["facts"]
    fields = [("name", f'"{t["name"]}"'),
              ("platform", f'"{t["platform"]}"'),
              ("backend", f'"{t["backend"]}"'),
              ("source", f'"{f["source"]}"'),
              ("line_rate", f"{t['line_rate']}u"),
              ("onset_tap", f"{t['onset_tap']}u"),
              ("onset_ms", t["onset_ms"]),
              ("uplink_db", t["uplink"]),
              ("far_rms", f["far_rms"]),
              ("residual_rms", t["residual"]),
              ("erle_48_db", f["erle_48"]),
              ("erle_48_onset_db", f["erle_48_onset"]),
              ("erle_64_db", f["erle_64"]),
              ("erle_64_onset_db", f["erle_64_onset"]),
              ("h", t["ident"])]
    body = "".join(f"\t\t.{k} = {v},\n" for k, v in fields)
    return "\t{\n" + body + "\t},\n"


def header_render(tables):
    out = [BANNER]
    out += [array_render(t) for t in tables]
    out.append("static const struct field_path field_paths[] = {\n" +
               "".join(entry_render(t) for t in tables) + "};\n")
    out.append("#define FIELD_PATHS_COUNT\t"
               "(sizeof(field_paths) / sizeof(field_paths[0]))\n")
    out.append("#endif\n")
    return "\n".join(out)


def header_write(text):
    """The temporary file sits beside the header, so the rename stays on one
    file system and replaces the header in one step."""
    mode = (stat.S_IMODE(HEADER.stat().st_mode) if HEADER.exists()
            else 0o644)
    fd, temp = tempfile.mkstemp(dir=HEADER.parent, prefix=f".{HEADER.name}.")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(text)
        os.chmod(temp, mode)
        os.replace(temp, HEADER)
    except OSError as e:
        with contextlib.suppress(OSError):
            os.unlink(temp)
        raise SystemExit(f"{HEADER}: not written: {e.strerror}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true",
                    help="exit 1 when the header would change")
    args = ap.parse_args()
    paths = sorted(FIELD_DIR.glob("path-*.txt"))
    if not paths:
        raise SystemExit(f"{FIELD_DIR}: no tap tables")
    tables = sorted((table_read(p) for p in paths),
                    key=lambda t: t["facts"]["source"])
    text = header_render(tables)
    if args.check:
        old = (HEADER.read_text(encoding="utf-8") if HEADER.exists()
               else None)
        return 0 if text == old else 1
    header_write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
