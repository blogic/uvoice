#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.

PLATFORMDB=/usr/srg/scripts/platformdb

# The voice-scripts seeder calls this helper with the device model as $1 when
# the voice stack is uvoice, after it created /etc/config/voip and the channel
# sections. The seeder gates on the stack, so the model argument is the whole
# answer. A standalone run (bench, a live opkg install) reads the platform
# database itself and applies the same stack gate, so a board another stack
# owns keeps its file untouched. The helper writes an option only when it is
# unset.
device_model="$1"
if [ -z "$device_model" ]; then
	# Use a subshell because platformdb can exit. The precedent is the
	# uvoice.init PDB_NUM_POTS subshell idiom. A read that exits prints
	# nothing, which the model check below turns into a retry.
	pdb_answer="$(
		if [ -r "$PLATFORMDB" ]; then
			. "$PLATFORMDB" >/dev/null 2>&1
			printf '%s %s\n' "${PDB_VOICE_STACK:-none}" "${PDB_DEVICE_MODEL:-}"
		fi
	)"
	pdb_stack="${pdb_answer%% *}"
	case "$pdb_stack" in
	''|uvoice)
		;;
	*)
		exit 0
		;;
	esac
	device_model="${pdb_answer#* }"
fi

# No answer is not "not served": a failed platform database read must leave
# the caller retrying, never resolve as a board without a hal seed.
# pdb.default.sh spells its placeholder model 'unknow'; cover the corrected
# spelling too.
case "$device_model" in
''|unknow|unknown)
	exit 1
	;;
esac

# The schema defaults are the MT798x values: the SLIC plug-in variant
# udsp-slic-proslic-mt798x.so serves the MT7987 and MT7988 boards, and the
# spidev, gpio and ALSA device names match every board.
# The variant also carries the PCM highway offset default, so no
# pcm_offset_bits seed is needed. The 16 kHz line runs on the AN7581, MT7987
# and MT7988 boards, so the seed asks for it everywhere but on MT7622, whose
# 32-bit highway slots carry no wideband pair: a 16000 ask there costs a
# degrade and a reopen at every start, so the seed states the board rate
# instead. The fax plug-in is seeded wherever the file is part of the image.
engine_cpu=''
slic_plugin=''
sample_rate=16000
fax_plugin=/usr/lib/udsp/udsp-fax-spandsp.so
case "$device_model" in
834-v6|854-v6)
	engine_cpu=1
	slic_plugin=/usr/lib/udsp/udsp-slic-proslic-mt7622.so
	sample_rate=8000
	;;
# The ETH-WAN alias boards (engnums 1600/1601) source the base pdb and
# differ only in the WAN wiring, so the arm matches the model prefix.
SDG-P72v-Plus*|SDG-P73v-Ultra*)
	slic_plugin=/usr/lib/udsp/udsp-slic-proslic-an7581.so
	;;
esac

# The voice-scripts seeder owns this file, so this board delta must not
# create it. A nonzero exit makes the calling seeder retry next boot; a
# standalone run before the seeder has nothing to write to.
[ -f /etc/config/voip ] || exit 1

if ! uci -q get voip.hal >/dev/null 2>&1; then
	uci set voip.hal=hal >/dev/null 2>&1
fi

# A value the user or provisioning already wrote wins over the seed.
seed() {
	if uci -q get "voip.hal.$1" >/dev/null 2>&1; then
		return 0
	fi
	uci set "voip.hal.$1=$2" >/dev/null 2>&1
}

if [ -n "$engine_cpu" ]; then
	seed engine_cpu "$engine_cpu"
fi

if [ -n "$slic_plugin" ]; then
	seed slic_plugin "$slic_plugin"
fi

# Some images carry no fax package. Seed its path only when the plug-in is
# part of this image.
if [ -f "$fax_plugin" ]; then
	seed fax_plugin "$fax_plugin"
fi

seed sample_rate "$sample_rate"

uci commit voip >/dev/null 2>&1
exit 0
