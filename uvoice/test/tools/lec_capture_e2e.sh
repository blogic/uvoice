#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
#
# lec_capture_e2e.sh: the framing of the canceller ring across the two
# packages.
#
#   lec_capture_e2e.sh <rings.pcapng> <lec-capture.py> <lec_replay>
#   lec_capture_e2e.sh --skip <reason>
#
# The second form is what the build passes when the export case cannot write
# a capture, for example without udebug support: the entry then skips with
# that reason instead of reading the absent file as a failure.
#
# test_hal_udsp writes rings.pcapng: one lineage of a live udsp canceller,
# drained by the udsp backend onto the canceller ring and exported the way a
# call trace exports it (test_lec_capture_export). The extractor of the udsp
# package reads the file, and lec_replay runs its replay file through the same
# canceller code. The entry passes when the extractor finds the ring, refuses
# no entry and exits 0, and the replay of line 0 reads "result: PASS". The
# lineage carries a dial tone inside its body, so the evaluated body must also
# hold a verdict other than quiet: a replay that compared only quiet frames
# would say little about the verdicts.
#
# Why a script and not a unit case. The reader and the replay belong to the
# udsp package, one in python and one a host program, and the point is that
# they read what uvoice wrote without a shared header between them.
#
# Exit 0 on a pass, 1 on a failure, 77 on a skip (the ctest SKIP_RETURN_CODE).
# A skip prints one "# SKIP" line with its reason. The extractor output goes
# to a temporary directory that the script removes. A pass removes
# rings.pcapng too; a failure keeps it in the build directory to inspect.

set -u

pcap=${1:-}
extractor=${2:-}
replay=${3:-}

skip()
{
	printf '# SKIP %s\n' "$1"
	exit 77
}

fail()
{
	printf 'not ok - %s\n' "$1"
	exit 1
}

[ "$pcap" = --skip ] && skip "${extractor:-the build asked for a skip}"
[ -n "$extractor" ] && [ -r "$extractor" ] ||
	skip "no udsp tree: the extractor lec-capture.py is absent"
[ -n "$replay" ] && [ -x "$replay" ] ||
	skip "no udsp tree: no lec_replay binary"
command -v python3 >/dev/null 2>&1 || skip "no python3"
[ -n "$pcap" ] && [ -r "$pcap" ] ||
	fail "no rings.pcapng at '$pcap'; test_hal_udsp writes it"

tmp=$(mktemp -d) || fail "mktemp failed"
trap 'rm -rf "$tmp"' EXIT

python3 "$extractor" "$pcap" "$tmp/e2e" > "$tmp/extract.txt" 2>&1
rc=$?
sed 's/^/# /' "$tmp/extract.txt"
[ "$rc" = 0 ] || fail "the extractor exits $rc"
grep -q 'uvoiced_lec (148)' "$tmp/extract.txt" ||
	fail "the extractor names no uvoiced_lec interface at DLT 148"
grep -q '^canceller ring: entries [1-9][0-9]*: .*refused 0$' \
	"$tmp/extract.txt" ||
	fail "the extractor refused an entry or found none"
grep -q 'from NEW' "$tmp/extract.txt" ||
	fail "the lineage does not start at NEW"
[ -r "$tmp/e2e-l0.lecr" ] || fail "the extractor wrote no replay file"
printf 'ok - the extractor reads every entry of the canceller ring\n'

"$replay" "$tmp/e2e-l0.lecr" > "$tmp/replay.txt" 2>&1
rc=$?
sed 's/^/# /' "$tmp/replay.txt"
[ "$rc" = 0 ] || fail "lec_replay exits $rc"
grep -q '^result: PASS$' "$tmp/replay.txt" ||
	fail "lec_replay reads no result: PASS"
printf 'ok - lec_replay replays the lineage with result: PASS\n'

# The count table after the totals line: the verdict, the live count, the
# replay count.
awk '/^total over the evaluated bodies/ { body = 1; next }
     body && $1 ~ /^(active|wait|erle|unlike|dt)$/ && $2 > 0 { seen++ }
     END { exit seen ? 0 : 1 }' "$tmp/replay.txt" ||
	fail "the replay body holds no verdict other than quiet"
printf 'ok - the replay compares verdicts other than quiet\n'

rm -f "$pcap"
exit 0
