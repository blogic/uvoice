#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
#
# The finisher copies collector output and packs the trace directory.

umask 077

# The generator of SUMMARY.txt. The archive keeps a copy of it under tools/,
# so a host builds the same report again from the same files.
summary_script=/usr/share/uvoice/calltrace-summary.uc

collector_omit() {
	printf 'OMITTED: %s\n' "$*"
	: > "$scratch/omitted"
	return 125
}

command_require() {
	command -v "$1" >/dev/null 2>&1 || collector_omit "no $1 command"
}

file_collect() {
	[ -r "$1" ] || { collector_omit "$1 is absent or unreadable"; return; }
	cat "$1"
}

threads_collect() {
	[ -d "/proc/$daemon_pid/task" ] || {
		collector_omit "daemon $daemon_pid is absent"
		return
	}
	for task in /proc/"$daemon_pid"/task/*; do
		printf 'task %s\n' "${task##*/}"
		cat "$task/comm" "$task/sched" || return
		grep '^Cpus_allowed_list:' "$task/status" || return
		printf '\n'
	done
}

records_select() {
	case "$1" in
	head | tail) "$1" -n "$2" ;;
	rtp)
		awk -v lo="$rtp_min" -v hi="$rtp_max" -v cap="$2" '
			function port_matches(field, parts) {
				if (field !~ /^(sport|dport)=[0-9]+$/) return 0
				split(field, parts, "=")
				return parts[2] >= lo && parts[2] <= hi
			}
			{
				for (i = 1; i <= NF; i++) {
					if (!port_matches($i)) continue
					print
					if (++count >= cap) exit
					next
				}
			}'
		;;
	esac
}

# A pipe normally reports the reader's status, not a failed collector's.
# SIGPIPE is expected when a bounded reader has enough records.
limited_collect() {
	selector=$1
	limit=$2
	shift 2
	("$@"; printf '%s\n' "$?" > "$scratch/command-status") |
		records_select "$selector" "$limit"
	read_code=$?
	code=$(cat "$scratch/command-status") || return 1
	[ "$read_code" -eq 0 ] || return "$read_code"
	[ "$code" -eq 141 ] && return 0
	return "$code"
}

conntrack_collect() {
	command_require conntrack || return
	command_require jq || return
	trace_line=$(jq -er '.line' "$dir/manifest.json") || return
	# call.c port_range_norm() uses 10000-10030 for an unset or invalid pair.
	range=$(jq -er --argjson line "$trace_line" '
		[(.channel // [])[] | select(.id == $line)][0] // {} |
		(.rtp_local_port_min // 0) as $lo |
		(.rtp_local_port_max // 0) as $hi |
		if $lo > 0 and $hi >= $lo and $hi <= 65535 then [$lo, $hi]
		else [10000, 10030] end | @tsv' "$dir/config.json") || return
	read -r rtp_min rtp_max <<EOF
$range
EOF
	limited_collect rtp 5000 conntrack -L -p udp
}

nft_collect() {
	command_require nft || return
	nft list ruleset > "$scratch/ruleset" || return
	# fw4 renders the daemon's Allow_VoIP_* uci rules inside its zone chains
	# with the rule name in a comment, so a chain name alone never finds
	# them: a matching rule line is kept with its table and chain headers.
	awk '
		/^table[ \t]/ { table = $0; shown_t = 0 }
		/^[ \t]*chain[ \t]+[^ \t]+[ \t]*\{/ {
			chain = $0
			shown_c = 0
			if (tolower(chain) ~ /voip|uvoice/) {
				active = 1
				if (!shown_t) { print table; shown_t = 1 }
				shown_c = 1
			}
		}
		active {
			print
			line = $0
			depth += gsub(/\{/, "", line) - gsub(/\}/, "", line)
			if (depth == 0) active = 0
			next
		}
		tolower($0) ~ /voip|uvoice/ {
			if (!shown_t) { print table; shown_t = 1 }
			if (!shown_c) { print chain; shown_c = 1 }
			print
		}
	' "$scratch/ruleset" > "$scratch/chains" || return
	if [ -s "$scratch/chains" ]; then
		cat "$scratch/chains"
	else
		printf '%s\n' 'no VoIP nftables rule matched; kept the ruleset head' \
			> "$scratch/note" || return
		head -n 2000 "$scratch/ruleset"
	fi
}

log_collect() {
	command_require logread || return
	log_help=$(logread --help 2>&1)
	case "$log_help" in
	*--since*--until*)
		if command -v jq >/dev/null 2>&1; then
			start=$(jq -er '.times.armed_at - 30' "$dir/manifest.json")
			end=$(jq -er '.times.ended_at // now | floor | . + 30' \
				"$dir/manifest.json")
			# SmartOS help uses a date and local time for --since.
			start=$(date -d "@$start" '+%Y-%m-%d %H:%M:%S') &&
				end=$(date -d "@$end" '+%Y-%m-%d %H:%M:%S') &&
				logread -S "$start" -U "$end" --iso8601 -t && return 0
		fi
		;;
	esac
	printf '%s\n' 'Time options unavailable; last 20000 records follow.' >&2
	logread -l 20000
}

platform_model_collect() (
	[ -r /usr/srg/scripts/platformdb ] || {
		collector_omit 'no platformdb model'
		return
	}
	# A platform database can exit, so it must not own the worker shell.
	# shellcheck source=/dev/null
	. /usr/srg/scripts/platformdb >/dev/null 2>&1 || return
	[ -n "${PDB_DEVICE_MODEL:-}" ] || {
		collector_omit 'platformdb has no model'
		return
	}
	printf '%s' "$PDB_DEVICE_MODEL" > "$dir/.board-model" || return
	printf 'model=%s\n' "$PDB_DEVICE_MODEL"
)

platform_serial_collect() (
	[ -r /etc/mfginfo.sh ] || {
		collector_omit 'no manufacturing serial'
		return
	}
	# shellcheck source=/dev/null
	. /etc/mfginfo.sh || return
	if [ -n "${MFG_MODEL:-}" ]; then
		printf 'MFG_MODEL=%s\n' "$MFG_MODEL"
	fi
	if [ -n "${MFG_HWREV:-}" ]; then
		printf 'MFG_HWREV=%s\n' "$MFG_HWREV"
	fi
	[ -n "${MFG_SERIAL:-}" ] || {
		collector_omit 'manufacturing data has no serial'
		return
	}
	printf '%s' "$MFG_SERIAL" > "$dir/.board-serial" || return
	printf 'serial=%s\n' "$MFG_SERIAL"
)

# An absent identity field must not hide a failed read of another field.
platform_result_take() {
	code=$1
	if [ "$code" -eq 125 ] && [ -f "$scratch/omitted" ]; then
		missing=1
		rm "$scratch/omitted" || return
		return 0
	fi
	return "$code"
}

platform_collect() {
	missing=0
	platform_model_collect
	platform_result_take "$?" || return
	platform_serial_collect
	platform_result_take "$?" || return
	file_collect /etc/openwrt_release
	platform_result_take "$?" || return
	printf 'boot_id='
	file_collect /proc/sys/kernel/random/boot_id
	platform_result_take "$?" || return
	[ "$missing" -eq 0 ] || collector_omit 'platform identity is incomplete'
}

packages_collect() {
	command_require opkg || return
	opkg list-installed > "$scratch/packages" || return
	grep -E 'uvoice|udsp' "$scratch/packages"
	code=$?
	[ "$code" -eq 1 ] && return 0
	return "$code"
}

collector_run() {
	case "$name" in
	uptime) command_require uptime && uptime ;;
	loadavg) file_collect /proc/loadavg ;;
	meminfo) file_collect /proc/meminfo ;;
	daemon-status) file_collect "/proc/$daemon_pid/status" ;;
	threads) threads_collect ;;
	ps) command_require ps && ps w ;;
	ip-addr.json) command_require ip && ip -j addr ;;
	ip-route.json) command_require ip && ip -j route ;;
	ip6-route.json) command_require ip && ip -6 -j route ;;
	conntrack-rtp) conntrack_collect ;;
	nft-voip) nft_collect ;;
	logread) log_collect ;;
	dmesg)
		command_require dmesg || return
		limited_collect tail 500 dmesg
		;;
	ssl.keylog) file_collect /tmp/uvoice/ssl.keylog ;;
	platform) platform_collect ;;
	packages) packages_collect ;;
	md5)
		command_require md5sum || return
		[ -r /usr/sbin/uvoiced ] || {
			collector_omit '/usr/sbin/uvoiced is absent'
			return
		}
		md5sum /usr/sbin/uvoiced
		;;
	summary)
		[ -r "$summary_script" ] || {
			collector_omit 'the summary generator is absent'
			return
		}
		command_require ucode || return
		# A worker must not write into the archive directory, so the
		# report goes to stdout and the parent publishes what it
		# captured. The diagnostics go to a file of their own.
		ucode "$summary_script" "$dir" - 2> "$scratch/err"
		;;
	*) return 2 ;;
	esac
}

# Busybox timeout stops this shell, not all its tools. The parent copies its
# output to a new inode so an orphan tool cannot change an archived file.
if [ "${1:-}" = --collect ]; then
	[ "$#" -eq 4 ] || exit 2
	name=$2
	dir=$3
	daemon_pid=$4
	scratch=$dir/.finish/$name
	collector_run
	exit $?
fi

[ "$#" -eq 4 ] || exit 2
dir=${1%/}
archive=$2
cpumask=$3
daemon_pid=$4
[ -n "$dir" ] && [ -n "$archive" ] && [ -n "$cpumask" ] || exit 2
case "$daemon_pid" in ''|*[!0-9]*) exit 2 ;; esac

if [ "$cpumask" != all ] && [ "${UVOICE_FINISH_PINNED:-}" != 1 ] &&
	[ -x /usr/bin/taskset ]; then
	UVOICE_FINISH_PINNED=1
	export UVOICE_FINISH_PINNED
	exec /usr/bin/taskset -c "$cpumask" "$0" "$@"
fi

[ -f "$dir/manifest.json" ] || exit 1
rm -rf "$dir/.finish" || exit 1
rm -f "$dir/.board-model" "$dir/.board-serial" || exit 1
mkdir -p "$dir/system" "$dir/.finish" || exit 1
: > "$dir/.finish/notes" || exit 1
collectors='uptime loadavg meminfo daemon-status threads ps ip-addr.json
ip-route.json ip6-route.json conntrack-rtp nft-voip logread dmesg ssl.keylog
platform packages md5'

# $2 is the time limit in seconds. A context collector answers in 15; the
# summary generator reads the whole export and gets more.
collector_start() {
	name=$1
	budget=${2:-15}
	mkdir -p "$dir/.finish/$name" || return 1
	if command -v timeout >/dev/null 2>&1; then
		timeout "$budget" nice -n 19 "$0" --collect "$name" "$dir" \
			"$daemon_pid" > "$dir/.finish/$name/out" 2>&1
	else
		nice -n 19 "$0" --collect "$name" "$dir" "$daemon_pid" \
			> "$dir/.finish/$name/out" 2>&1
	fi
	code=$?
	result="failed $code"
	if [ "$code" -eq 0 ]; then
		result=ok
	elif [ "$code" -eq 124 ] || [ "$code" -eq 143 ]; then
		result=timeout
	elif [ "$code" -eq 125 ] && [ -f "$dir/.finish/$name/omitted" ]; then
		result=omitted
	fi
	# The summary answers with a report, not with a system context file,
	# so the parent publishes its output instead of copying it here.
	if [ "$name" != summary ]; then
		rm -f "$dir/system/$name" || return 1
		cp "$dir/.finish/$name/out" "$dir/system/$name" || return 1
	fi
	printf '%s\n' "$result" > "$dir/.finish/$name/result" || return 1
}

# Collectors run together, so several missing replies fit the 60 second base
# of the finisher budget.
for name in $collectors; do
	collector_start "$name" &
done
wait

collector_count_add() {
	case "$1" in
	ok) ok_count=$((ok_count + 1)) ;;
	omitted) omitted_count=$((omitted_count + 1)) ;;
	*) failed_count=$((failed_count + 1)) ;;
	esac
}

ok_count=0
failed_count=0
omitted_count=0
: > "$dir/.finish/collectors.tsv" || exit 1
for name in $collectors; do
	result=$(cat "$dir/.finish/$name/result") || exit 1
	printf '%s\t%s\n' "$name" "$result" \
		>> "$dir/.finish/collectors.tsv" || exit 1
	collector_count_add "$result"
	if [ -f "$dir/.finish/$name/note" ]; then
		cat "$dir/.finish/$name/note" >> "$dir/.finish/notes" || exit 1
	fi
done

# This encoder also works when jq is absent, without editing the journal text.
json_string() {
	awk 'BEGIN {
		printf "\""
		for (i = 1; i < 32; i++) escape[sprintf("%c", i)] = sprintf("\\u%04x", i)
	}
	{
		if (NR > 1) printf "\\n"
		for (i = 1; i <= length($0); i++) {
			c = substr($0, i, 1)
			if (c == "\\" || c == "\"") printf "\\%s", c
			else if (c in escape) printf "%s", escape[c]
			else printf "%s", c
		}
	}
	END { printf "\"" }'
}

manifest_extra_write() {
	printf '{"stage":"%s","collectors":{' "$1"
	separator=''
	while IFS="$(printf '\t')" read -r name result; do
		printf '%s"%s":"%s"' "$separator" "$name" "$result"
		separator=,
	done < "$dir/.finish/collectors.tsv"
	printf '},"board":{'
	separator=''
	for key in model serial; do
		[ -f "$dir/.board-$key" ] || continue
		printf '%s"%s":' "$separator" "$key"
		json_string < "$dir/.board-$key"
		separator=,
	done
	printf '}'
	if [ "$(cat "$dir/.finish/md5/result")" = ok ]; then
		printf ',"md5":'
		json_string < "$dir/system/md5"
	fi
	if [ "$(cat "$dir/.finish/ssl.keylog/result")" = ok ]; then
		printf ',"contains":["tls_keylog"]'
	fi
	printf ',"notes":['
	separator=''
	while IFS= read -r note; do
		printf '%s' "$separator"
		printf '%s' "$note" | json_string
		separator=,
	done < "$dir/.finish/notes"
	printf ']}\n'
}

note_add() {
	printf '%s\n' "$*" >> "$dir/.finish/notes"
}

wire_process_matches() {
	[ -r "/proc/$wire_pid/cmdline" ] || return 1
	tr '\000' ' ' 2>/dev/null < "/proc/$wire_pid/cmdline" |
		grep -Fq "$dir/wire.pcap"
}

wire_pid_stop() {
	case "$wire_pid" in ''|*[!0-9]*) return 1 ;; esac
	[ "$wire_pid" -gt 1 ] || return 1
	wire_process_matches || return 0
	if ! kill -TERM "$wire_pid" 2>/dev/null; then
		wire_process_matches && return 1
		return 0
	fi
	wire_wait=0
	while wire_process_matches && [ "$wire_wait" -lt 30 ]; do
		sleep 0.1
		wire_wait=$((wire_wait + 1))
	done
	if wire_process_matches; then
		kill -KILL "$wire_pid" 2>/dev/null || return 1
	fi
	note_add "stopped orphan wire capture PID $wire_pid"
}

wire_stop() {
	if command -v jq >/dev/null 2>&1; then
		wire_pid=$(jq -er '.children.wire_pid // empty' \
			"$dir/manifest.json") || return 0
		wire_pid_stop
		return
	fi
	for cmdline in /proc/[0-9]*/cmdline; do
		wire_pid=${cmdline%/cmdline}
		wire_pid=${wire_pid##*/}
		wire_process_matches || continue
		wire_pid_stop || return
	done
}

wire_stop || exit 1
if [ -r /usr/share/uvoice/calltrace-README.txt ]; then
	cp /usr/share/uvoice/calltrace-README.txt "$dir/README.txt" || exit 1
else
	note_add 'calltrace README template is absent' || exit 1
fi
if ! command -v jq >/dev/null 2>&1; then
	note_add 'jq is absent; finisher results are in manifest-finisher.json' ||
		exit 1
fi

# Keep the finishing journal so a failure needs no new JSON file.
# SIGKILL or power loss cannot run this trap; a done work directory can remain.
manifest_done=0
finish_exit() {
	code=$1
	trap - 0 HUP INT TERM
	[ "$code" -eq 0 ] && exit 0
	rm -f "$archive.part"
	if [ "$manifest_done" -eq 1 ] && [ -d "$dir" ]; then
		mv "$dir/.finish/manifest-finishing.json" "$manifest_target" ||
			printf 'cannot restore the finishing journal\n' >&2
	fi
	exit "$code"
}
trap 'finish_exit "$?"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

manifest_publish() {
	manifest_extra_write "done" > "$dir/.finish/manifest.json" || return 1
	if command -v jq >/dev/null 2>&1; then
		jq -s '.[0] as $old | .[1] as $new |
			$old + $new | .notes = (($old.notes // []) + $new.notes) |
			if $new | has("contains") then
				.contains = (($old.contains // []) + $new.contains | unique)
			else . end' \
			"$dir/.finish/manifest-daemon.json" \
			"$dir/.finish/manifest.json" \
			> "$dir/.finish/manifest-done.json" || return 1
		jq '.stage = "finishing"' "$dir/.finish/manifest-done.json" \
			> "$dir/.finish/manifest-finishing.json" || return 1
		manifest_target=$dir/manifest.json
	else
		mv "$dir/.finish/manifest.json" \
			"$dir/.finish/manifest-done.json" || return 1
		manifest_extra_write finishing \
			> "$dir/.finish/manifest-finishing.json" || return 1
		manifest_target=$dir/manifest-finisher.json
	fi
	manifest_done=1
	mv "$dir/.finish/manifest-done.json" "$manifest_target" || return 1
}

# The journal goes out twice: once so the report reads the collector results,
# the notes and the contains words, and once more with the report's own
# result. Both merges read the daemon's own copy, so a note lands once.
cp "$dir/manifest.json" "$dir/.finish/manifest-daemon.json" || exit 1
manifest_publish || exit 1

summary_budget_get() {
	capture_bytes=0
	for capture in rings.pcapng wire.pcap; do
		[ -f "$dir/$capture" ] || continue
		capture_size=$(wc -c < "$dir/$capture") || return 1
		capture_bytes=$((capture_bytes + capture_size))
	done
	printf '%s\n' $((30 + (capture_bytes + 1048575) / 1048576))
}

# A report the generator could not write costs the archive nothing: the
# collectors table carries the result and the files stand as they are. The
# generator reads the ring file and the wire capture whole, at about 3 MiB a
# second on the slowest board for the ring walk and for the two tcpdump passes
# over the wire capture together. One second per MiB of the two files on top
# of 30 leaves a margin of about three.
summary_budget=$(summary_budget_get) || exit 1
collector_start summary "$summary_budget" || exit 1
summary_result=$(cat "$dir/.finish/summary/result") || exit 1
# A timeout or a failure publishes no report.
if [ "$summary_result" = ok ]; then
	cp "$dir/.finish/summary/out" "$dir/SUMMARY.txt" || exit 1
fi
# The generator says nothing when the report is whole, so the archive
# carries system/summary only when there is something to read in it.
if [ -s "$dir/.finish/summary/err" ]; then
	rm -f "$dir/system/summary" || exit 1
	cp "$dir/.finish/summary/err" "$dir/system/summary" || exit 1
else
	rm -f "$dir/system/summary" || exit 1
fi
# The archive carries the generator whatever its own run did, so a host can
# build the report again.
if [ -r "$summary_script" ]; then
	mkdir -p "$dir/tools" || exit 1
	cp "$summary_script" "$dir/tools/calltrace-summary.uc" || exit 1
fi
printf '%s\t%s\n' summary "$summary_result" \
	>> "$dir/.finish/collectors.tsv" || exit 1
collector_count_add "$summary_result"
printf 'collectors: ok %s, failed %s, omitted %s\n' \
	"$ok_count" "$failed_count" "$omitted_count" >&2
manifest_publish || exit 1
rm -f "$dir/.board-model" "$dir/.board-serial" || exit 1
parent=$(dirname "$dir")
stem=$(basename "$dir")
rm -f "$archive.part" || exit 1
tar --warning=no-file-changed --exclude="$stem/.finish" \
	-C "$parent" -zcf "$archive.part" -- "$stem" || {
	rm -f "$archive.part"
	exit 1
}
mv "$archive.part" "$archive" || exit 1
rm -rf "$dir" || exit 1
finish_exit 0
