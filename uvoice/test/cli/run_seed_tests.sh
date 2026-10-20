#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
#
#
# run_seed_tests.sh: the uvoice board defaults helper (uvoice-defaults.sh)
# against a fake platformdb and a recording uci.
#
# The helper takes the device model as $1 from the voice-scripts seeder, or
# sources /usr/srg/scripts/platformdb for PDB_DEVICE_MODEL on its own; it
# tests /etc/config/voip and calls plain uci, and a build host has none of
# the three. The suite re-enters itself under "unshare -rm" (a user and a
# mount namespace, the run_cli_tests.sh recipe without its network
# namespace) and puts an unprivileged overlay over /usr and /etc, so the
# shipped script runs with its own strings and not a rewritten copy. uci is
# a PATH shim that keeps the option state in a file and records every call.
#
# Every leg runs under every interpreter the host offers among dash and
# busybox sh: the helper ships as #!/bin/sh and both run it, on a target and
# in the build tree.
#
# Shim fidelity: a set on an option whose section does not exist fails, and
# an empty value unsets rather than stores, matching real uci
# (measured against /tmp/uvoice-host/prefix/bin/uci). Known shim limits:
# it does not check that the config file exists (real uci answers "Entry not
# found" rc 1 for a set or a commit on a missing file), and a set argument
# with no '=' is stored as key=key (real uci answers "Invalid argument" rc 1).
# No leg relies on either.
#
#   SEED      the helper under test (default ../../files/uvoice-defaults.sh)
#
# Output is TAP: one "ok"/"not ok" line per check, a trailing plan line, and a
# non-zero exit when any check fails.

here=$(cd "$(dirname "$0")" && pwd)
uvoice_dir=$(cd "$here/../.." && pwd)
seed=${SEED:-$uvoice_dir/files/uvoice-defaults.sh}
export here uvoice_dir seed

if [ "${SEED_TESTS_INNER:-}" != 1 ]; then
	SEED_TESTS_INNER=1
	export SEED_TESTS_INNER
	exec unshare -rm "$0" "$@"
fi

bail() {
	printf 'Bail out! %s\n' "$*"
	exit 1
}

[ -r "$seed" ] || bail "no seed script at $seed"

tmp=$(mktemp -d) || bail "mktemp failed"

cleanup() {
	cd /
	umount -l /etc 2>/dev/null
	umount -l /usr 2>/dev/null
	rm -rf "$tmp"
}
trap 'cleanup' EXIT

# /usr/lib belongs to a uid the user namespace does not map, so the merged
# view refuses a new directory under it. The upper layer takes lib/udsp
# instead, and the overlay shows it as /usr/lib/udsp.
mkdir -p "$tmp/usr-up/lib/udsp" "$tmp/usr-work" "$tmp/etc-up" \
	"$tmp/etc-work" "$tmp/bin" "$tmp/rec"

# Neither /usr/srg nor /etc/config exists on a build host, so both trees take
# an overlay and the paths the seed names land in the upper layers.
mount -t overlay overlay \
	-o "lowerdir=/usr,upperdir=$tmp/usr-up,workdir=$tmp/usr-work" /usr ||
	bail 'cannot overlay /usr, the kernel refuses an unprivileged overlay mount'
mount -t overlay overlay \
	-o "lowerdir=/etc,upperdir=$tmp/etc-up,workdir=$tmp/etc-work" /etc ||
	bail 'cannot overlay /etc, the kernel refuses an unprivileged overlay mount'
mkdir -p /usr/srg/scripts /etc/config || bail 'cannot create the device paths'

platformdb=/usr/srg/scripts/platformdb
voip_cfg=/etc/config/voip

#
# The uci shim. State is one "key=value" line per option; every call lands in
# the record as its argument vector, "-q" included, so a leg can assert the
# exact calls the seed made.
#
UCI_STATE=$tmp/rec/state
UCI_REC=$tmp/rec/uci.argv
export UCI_STATE UCI_REC

cat > "$tmp/bin/uci" <<'SHIM'
#!/bin/sh
printf '%s\n' "$*" >> "$UCI_REC"
quiet=
[ "$1" = -q ] && quiet=1 && shift
case "$1" in
get)
	awk -v k="$2=" 'index($0, k) == 1 { print substr($0, length(k) + 1); f = 1 }
			END { exit !f }' "$UCI_STATE"
	;;
set)
	k=${2%%=*}
	v=${2#*=}
	case "$k" in
	*.*.*)
		sect=${k%.*}
		if ! awk -v k="$sect=" 'index($0, k) == 1 { f = 1 } END { exit !f }' \
				"$UCI_STATE"; then
			[ "$quiet" = 1 ] || printf '%s: Invalid argument\n' "$0" >&2
			exit 1
		fi
		;;
	esac
	awk -v k="$k=" 'index($0, k) != 1' "$UCI_STATE" > "$UCI_STATE.new"
	[ -n "$v" ] && printf '%s=%s\n' "$k" "$v" >> "$UCI_STATE.new"
	mv "$UCI_STATE.new" "$UCI_STATE"
	;;
commit)
	;;
*)
	exit 2
	;;
esac
SHIM
chmod +x "$tmp/bin/uci"

n=0
failed=0

ok() {
	n=$((n + 1))
	printf 'ok %d - %s\n' "$n" "$1"
}

not_ok() {
	n=$((n + 1))
	failed=$((failed + 1))
	printf 'not ok %d - %s\n' "$n" "$1"
}

check() {
	if [ "$2" = "$3" ]; then
		ok "$1"
	else
		not_ok "$1 (got '$2', want '$3')"
	fi
}

state_of() {
	awk -v k="$1=" 'index($0, k) == 1 { print substr($0, length(k) + 1) }' \
		"$UCI_STATE"
}

# exact argument vectors, so "set voip.hal=hal" does not match a set of an
# option under the section
rec_count() {
	grep -c -F -x -- "$1" "$UCI_REC"
}

rec_lines() {
	wc -l < "$UCI_REC" | tr -d ' '
}

state_lines() {
	wc -l < "$UCI_STATE" | tr -d ' '
}

# seed_fixture model present [preset...]: an empty model means no platformdb;
# present 1 creates /etc/config/voip, 0 removes it; every preset is one
# "key=value" line of prior uci state. The platformdb names the uvoice stack
# unless fixture_stack says otherwise; NONE leaves the stack variable out.
fixture_stack=uvoice

seed_fixture() {
	model=$1
	present=$2
	shift 2

	: > "$UCI_STATE"
	: > "$UCI_REC"
	for kv in "$@"; do
		printf '%s\n' "$kv" >> "$UCI_STATE"
	done

	rm -f "$platformdb" "$voip_cfg" "$fax_plugin"
	if [ -n "$model" ]; then
		[ "$fixture_stack" = NONE ] ||
			printf 'PDB_VOICE_STACK=%s\n' "$fixture_stack" > "$platformdb"
		printf 'PDB_DEVICE_MODEL=%s\n' "$model" >> "$platformdb"
	fi
	[ "$present" = 1 ] && : > "$voip_cfg"
}

run_seed() {
	shell=$1
	shift
	seed_fixture "$@"

	PATH="$tmp/bin:$PATH" $shell "$seed" >/dev/null 2>&1
	rc=$?
}

run_seed_with_fax() {
	shell=$1
	shift
	seed_fixture "$@"

	: > "$fax_plugin"
	PATH="$tmp/bin:$PATH" $shell "$seed" >/dev/null 2>&1
	rc=$?
}

# The voice-scripts seeder passes the model as the first argument, so a
# call needs no platformdb read of its own.
run_seed_arg() {
	shell=$1
	model_arg=$2
	shift 2
	seed_fixture "$@"

	PATH="$tmp/bin:$PATH" $shell "$seed" "$model_arg" >/dev/null 2>&1
	rc=$?
}

mt7622=/usr/lib/udsp/udsp-slic-proslic-mt7622.so
an7581=/usr/lib/udsp/udsp-slic-proslic-an7581.so
fax_plugin=/usr/lib/udsp/udsp-fax-spandsp.so

rec_sets() {
	grep -c '^set ' "$UCI_REC"
}

legs() {
	shell=$1

	$shell -n "$seed" && ok "$shell -n accepts the seed" ||
		not_ok "$shell -n accepts the seed"
	$shell -n "$0" && ok "$shell -n accepts this harness" ||
		not_ok "$shell -n accepts this harness"

	# MT7622 carries no wideband pair, so the seed states 8000 instead of
	# leaving the board to degrade a 16000 ask at every start.
	for model in 834-v6 854-v6; do
		run_seed "$shell" "$model" 1
		check "$shell $model: exit 0" "$rc" 0
		check "$shell $model: the hal section is created once" \
			"$(rec_count 'set voip.hal=hal')" 1
		check "$shell $model: engine_cpu 1" "$(state_of voip.hal.engine_cpu)" 1
		check "$shell $model: the mt7622 plug-in" \
			"$(state_of voip.hal.slic_plugin)" "$mt7622"
		check "$shell $model: sample_rate 8000" \
			"$(state_of voip.hal.sample_rate)" 8000
		check "$shell $model: one sample_rate set call" \
			"$(rec_count "set voip.hal.sample_rate=8000")" 1
		check "$shell $model: no fax plug-in without its file" \
			"$(state_of voip.hal.fax_plugin)" ""
		check "$shell $model: one commit" "$(rec_count 'commit voip')" 1
	done

	run_seed_with_fax "$shell" 834-v6 1
	check "$shell 834-v6 fax file present: the fax plug-in is seeded" \
		"$(state_of voip.hal.fax_plugin)" "$fax_plugin"

	# The -eth names are the ETH-WAN alias boards: their pdb sources the
	# base file, so the same seed must land.
	for model in SDG-P72v-Plus SDG-P73v-Ultra SDG-P72v-Plus-Eth SDG-P73v-Ultra-Eth; do
		run_seed "$shell" "$model" 1
		check "$shell $model: exit 0" "$rc" 0
		check "$shell $model: the an7581 plug-in" \
			"$(state_of voip.hal.slic_plugin)" "$an7581"
		check "$shell $model: engine_cpu stays unset" \
			"$(state_of voip.hal.engine_cpu)" ""
		check "$shell $model: no engine_cpu set call" \
			"$(rec_count "set voip.hal.engine_cpu=1")" 0
		check "$shell $model: sample_rate 16000" \
			"$(state_of voip.hal.sample_rate)" 16000
		check "$shell $model: one sample_rate set call" \
			"$(rec_count "set voip.hal.sample_rate=16000")" 1
		check "$shell $model: no fax plug-in without its file" \
			"$(state_of voip.hal.fax_plugin)" ""
		check "$shell $model: one commit" "$(rec_count 'commit voip')" 1
	done

	run_seed_with_fax "$shell" SDG-P73v-Ultra 1
	check "$shell fax file present: exit 0" "$rc" 0
	check "$shell fax file present: the fax plug-in is seeded" \
		"$(state_of voip.hal.fax_plugin)" "$fax_plugin"
	check "$shell fax file present: one fax_plugin set call" \
		"$(rec_count "set voip.hal.fax_plugin=$fax_plugin")" 1

	run_seed_with_fax "$shell" SDG-P72v-Plus 1 voip.hal=hal \
		voip.hal.fax_plugin=/usr/lib/udsp/custom-fax.so
	check "$shell fax preset: exit 0" "$rc" 0
	check "$shell fax preset: the user value is kept" \
		"$(state_of voip.hal.fax_plugin)" /usr/lib/udsp/custom-fax.so
	check "$shell fax preset: no fax_plugin set call" \
		"$(rec_count "set voip.hal.fax_plugin=$fax_plugin")" 0

	# the MT798x boards keep the built-in mt798x plug-in and take the 16 kHz
	# line; an unknown model takes the same arm, so a later board is seeded
	# before this script learns its name
	for model in SDG-8732v SDG-8712v SDG-8733v SDG-9999v; do
		run_seed "$shell" "$model" 1
		check "$shell $model: exit 0" "$rc" 0
		check "$shell $model: the hal section is created once" \
			"$(rec_count 'set voip.hal=hal')" 1
		check "$shell $model: slic_plugin stays unset" \
			"$(state_of voip.hal.slic_plugin)" ""
		check "$shell $model: engine_cpu stays unset" \
			"$(state_of voip.hal.engine_cpu)" ""
		check "$shell $model: sample_rate 16000" \
			"$(state_of voip.hal.sample_rate)" 16000
		check "$shell $model: no fax plug-in without its file" \
			"$(state_of voip.hal.fax_plugin)" ""
		check "$shell $model: two set calls, the section and the rate" \
			"$(rec_sets)" 2
		check "$shell $model: one commit" "$(rec_count 'commit voip')" 1
	done

	run_seed_with_fax "$shell" SDG-8733v 1
	check "$shell SDG-8733v fax file present: exit 0" "$rc" 0
	check "$shell SDG-8733v fax file present: the fax plug-in is seeded" \
		"$(state_of voip.hal.fax_plugin)" "$fax_plugin"
	check "$shell SDG-8733v fax file present: sample_rate 16000" \
		"$(state_of voip.hal.sample_rate)" 16000

	run_seed "$shell" SDG-8732v 1 voip.hal=hal \
		"voip.hal.slic_plugin=$mt7622" voip.hal.sample_rate=8000
	check "$shell current path preset: exit 0" "$rc" 0
	check "$shell current path preset: the path is not rewritten" \
		"$(state_of voip.hal.slic_plugin)" "$mt7622"
	check "$shell current path preset: no set call" "$(rec_sets)" 0
	check "$shell current path preset: one commit" \
		"$(rec_count 'commit voip')" 1

	run_seed "$shell" 834-v6 1 voip.hal=hal voip.hal.engine_cpu=2 \
		voip.hal.slic_plugin=/usr/lib/udsp/custom.so \
		voip.hal.sample_rate=16000
	check "$shell preset: exit 0" "$rc" 0
	check "$shell preset: engine_cpu keeps the user value" \
		"$(state_of voip.hal.engine_cpu)" 2
	check "$shell preset: slic_plugin keeps the user value" \
		"$(state_of voip.hal.slic_plugin)" /usr/lib/udsp/custom.so
	check "$shell preset: sample_rate keeps the user value" \
		"$(state_of voip.hal.sample_rate)" 16000
	check "$shell preset: no set call" "$(grep -c '^set ' "$UCI_REC")" 0

	run_seed "$shell" SDG-P72v-Plus 1 voip.hal=hal
	check "$shell existing section: no second section set" \
		"$(rec_count 'set voip.hal=hal')" 0
	check "$shell existing section: the plug-in is still seeded" \
		"$(state_of voip.hal.slic_plugin)" "$an7581"
	check "$shell existing section: absent sample_rate is seeded" \
		"$(state_of voip.hal.sample_rate)" 16000

	run_seed "$shell" SDG-P73v-Ultra 1 voip.hal=hal voip.hal.sample_rate=8000
	check "$shell sample_rate preset: exit 0" "$rc" 0
	check "$shell sample_rate preset: the user value is kept" \
		"$(state_of voip.hal.sample_rate)" 8000
	check "$shell sample_rate preset: no sample_rate set call" \
		"$(rec_count 'set voip.hal.sample_rate=16000')" 0

	run_seed "$shell" 834-v6 0
	check "$shell retention: exit 1 without /etc/config/voip" "$rc" 1
	check "$shell retention: no uci call" "$(rec_lines)" 0
	[ -e "$voip_cfg" ] && not_ok "$shell retention: the file is not created" ||
		ok "$shell retention: the file is not created"

	# every model is served, so the retention rule is the same on each
	run_seed "$shell" SDG-8732v 0
	check "$shell SDG-8732v without /etc/config/voip: exit 1" "$rc" 1
	check "$shell SDG-8732v without /etc/config/voip: no uci call" \
		"$(rec_lines)" 0

	# a standalone run gates on the stack the platform database names: a
	# board another stack owns keeps its file, and a database with no stack
	# variable is a voiceless board; the seeder path passes the model and
	# gated already, so the argument wins over the database
	fixture_stack=other
	run_seed "$shell" SDG-8732v 1
	check "$shell other stack standalone: exit 0" "$rc" 0
	check "$shell other stack standalone: no uci call" "$(rec_lines)" 0
	run_seed_arg "$shell" SDG-8732v SDG-8732v 1
	check "$shell other stack, model arg: exit 0" "$rc" 0
	check "$shell other stack, model arg: sample_rate 16000" \
		"$(state_of voip.hal.sample_rate)" 16000
	fixture_stack=NONE
	run_seed "$shell" SDG-8732v 1
	check "$shell no stack variable standalone: exit 0" "$rc" 0
	check "$shell no stack variable standalone: no uci call" "$(rec_lines)" 0
	fixture_stack=uvoice

	run_seed_arg "$shell" SDG-P72v-Plus '' 1
	check "$shell model arg, no platformdb: exit 0" "$rc" 0
	check "$shell model arg, no platformdb: the an7581 plug-in" \
		"$(state_of voip.hal.slic_plugin)" "$an7581"
	check "$shell model arg, no platformdb: sample_rate 16000" \
		"$(state_of voip.hal.sample_rate)" 16000

	run_seed_arg "$shell" SDG-P72v-Plus 834-v6 1
	check "$shell model arg beats platformdb: exit 0" "$rc" 0
	check "$shell model arg beats platformdb: the an7581 plug-in" \
		"$(state_of voip.hal.slic_plugin)" "$an7581"
	check "$shell model arg beats platformdb: engine_cpu stays unset" \
		"$(state_of voip.hal.engine_cpu)" ""

	run_seed "$shell" "" 1
	check "$shell no platformdb: exit 1, the caller retries" "$rc" 1
	check "$shell no platformdb: no uci call" "$(rec_lines)" 0

	run_seed "$shell" unknow 1
	check "$shell placeholder model: exit 1, the caller retries" "$rc" 1
	check "$shell placeholder model: no uci call" "$(rec_lines)" 0

	# the real platformdb exits 1 when /etc/engnum.sh names no pdb_file,
	# which is why the helper sources it in a subshell: the exit must end
	# the read and not the helper, which then has no model and answers
	# exit 1 for a retry
	run_seed "$shell" 834-v6 1
	printf 'exit 1\n' > "$platformdb"
	: > "$UCI_STATE"
	: > "$UCI_REC"
	PATH="$tmp/bin:$PATH" $shell "$seed" >/dev/null 2>&1
	check "$shell exiting platformdb: exit 1, the exit stays in the subshell" \
		"$?" 1
	check "$shell exiting platformdb: no uci call" "$(rec_lines)" 0

	# Shim fidelity, pinned against real uci
	# (/tmp/uvoice-host/prefix/bin/uci). Direct shim calls, not through the
	# seed: the seed always creates voip.hal before an option set, so it
	# never exercises the missing-section refusal.
	seed_fixture '' 1
	$shell "$tmp/bin/uci" set voip.hal.engine_cpu=1 2>"$tmp/rec/stderr"
	check "$shell fidelity: set on a missing section fails" "$?" 1
	check "$shell fidelity: missing-section message class" \
		"$(grep -c -F 'Invalid argument' "$tmp/rec/stderr")" 1
	check "$shell fidelity: missing-section leaves no state" \
		"$(state_lines)" 0

	$shell "$tmp/bin/uci" set voip.hal=hal >/dev/null 2>&1
	$shell "$tmp/bin/uci" set voip.hal.foo=bar >/dev/null 2>&1
	$shell "$tmp/bin/uci" set voip.hal.foo= >/dev/null 2>&1
	check "$shell fidelity: set with an empty value exits 0" "$?" 0
	$shell "$tmp/bin/uci" get voip.hal.foo >/dev/null 2>&1
	check "$shell fidelity: an empty value is unset" "$?" 1

	$shell "$tmp/bin/uci" get voip.hal.absent >/dev/null 2>&1
	check "$shell fidelity: get of an absent option exits 1" "$?" 1
}

ran=0
if command -v dash >/dev/null 2>&1; then
	legs dash
	ran=1
fi
if command -v busybox >/dev/null 2>&1; then
	legs 'busybox sh'
	ran=1
fi
[ "$ran" = 1 ] || legs sh

printf '1..%d\n' "$n"
[ "$failed" -eq 0 ]
