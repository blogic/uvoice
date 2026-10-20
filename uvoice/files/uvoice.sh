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
# uvoice.sh - Shell helpers for the uvoice ubus API (voice, voice.trace,
# voice.stub)
#
# Usage: source uvoice.sh
#        uvoice.help
#
# Lines and slots are 0-based, matching the ubus surface. The klish-facing
# voice-cli stays 1-based; these helpers are the raw debug surface.
#
# Optional positional parameters are read as ${N:-} so the file can be
# sourced into a shell running under set -u.

UVOICE_CLI="${UVOICE_CLI:-/usr/share/uvoice/uvoice-cli.uc}"

######################################################
#
# HELPER FUNCTIONS
#
######################################################

_uvoice_ubus() {
	if [ -n "${UVOICE_UBUS_SOCKET:-}" ]; then
		ubus -s "$UVOICE_UBUS_SOCKET" "$@"
	else
		ubus "$@"
	fi
}

_uvoice_check() {
	_uvoice_ubus -t 2 list voice >/dev/null 2>&1 && return 0
	echo "Error: uvoiced is not running (no voice object on ubus)"
	return 1
}

_uvoice_stub_check() {
	_uvoice_ubus -t 2 list voice.stub >/dev/null 2>&1 && return 0
	echo "Error: voice.stub is not on ubus (stub HAL builds only)"
	return 1
}

_uvoice_table() {
	if [ ! -f "$UVOICE_CLI" ]; then
		echo "CLI script not found: $UVOICE_CLI"
		return 1
	fi
	# The -- keeps a permuting getopt (glibc hosts) from eating the --flag
	# arguments as ucode options. Color rides an env word so a pipe, a
	# capture and the test suite read plain bytes; the formatter honours
	# NO_COLOR and an explicit UVOICE_COLOR on its own.
	local color="${UVOICE_COLOR:-}"
	if [ -z "$color" ] && [ -t 1 ]; then
		color=1
	fi
	UVOICE_COLOR="$color" \
	UVOICE_UBUS_SOCKET="${UVOICE_UBUS_SOCKET:-}" ucode "$UVOICE_CLI" -- "$@"
}

_uvoice_uint_check() {
	case ${1:-} in
	'' | *[!0-9]*) return 1 ;;
	esac
	return 0
}

######################################################
#
# PUBLIC FUNCTIONS
#
######################################################

uvoice.help() {
	cat <<'EOF'
uvoice shell helpers (lines and slots are 0-based):

  Table-Formatted Output (uvoice.*):
    uvoice.status              - Daemon summary: version, config, HAL, region,
                                 events
    uvoice.lines               - Account and registration table
    uvoice.line N              - Complete line status: account, call state,
                                 stats, live media, jitter buffer, echo
                                 canceller, recent calls
    uvoice.line.watch N        - Live line view (refresh every 2s)
    uvoice.calls               - Per-line FSM state and call legs
    uvoice.calls.watch         - Live call view (refresh every 2s)
    uvoice.streams             - Live media counters per running stream
    uvoice.jb                  - Jitter buffer rows, one per line and stream
    uvoice.jb.watch            - Live jitter buffer view (refresh every 2s)
    uvoice.lec                 - Echo canceller rows
    uvoice.lec.watch           - Live echo canceller view (refresh every 2s)
    uvoice.monitor             - Line monitor rows, one per line
    uvoice.monitor.watch       - Live line monitor view (refresh every 2s)
    uvoice.cdr [N]             - Call records, oldest first (last N)
    uvoice.cdr.seq SEQ         - One call record in full (media + jitter buffer)
    uvoice.stats [LINE]        - Per-line call counters and lifetime totals
    uvoice.stats.live LINE [SLOT] - Live media counters of a running call leg
    uvoice.stats.reset         - Clear counters and totals, keep the cdr ring
    uvoice.config              - Global configuration
    uvoice.config.channel N    - One channel configuration
    uvoice.config.codecs       - Codec table
    uvoice.linetest LINE [TEST] - Metallic line test (default gr909;
                                 gr909|roh|capacitance|loop)
    uvoice.looptest start|stop [MODE]
                               - Manufacturing loop test on every line
                                 (MODE: self|cross); every line is out of
                                 service until it stops
    uvoice.trace               - Trace domain table
    uvoice.trace.set DOM on|off [DOM on|off ...]
                               - Domains: sip fsm hook hal dialplan media jb
    uvoice.register LINE       - Ask the account to REGISTER now
    uvoice.unregister LINE     - Stop the registration
    uvoice.seize LINE          - Inject a SEIZE hook event
    uvoice.release LINE        - Inject a RELEASE hook event
    uvoice.dial LINE NUMBER    - Seize when on hook, then inject the digits
    uvoice.reload              - Re-read UCI and rebuild the accounts
    uvoice.calltrace.arm N [S] - Arm the next call on index N (0-based),
                                 with optional timeout S in seconds
    uvoice.calltrace.status    - Trace state, readiness, and archive
    uvoice.calltrace.cancel    - Cancel the active trace
    uvoice.calltrace.wait      - Wait for an archive; deadline exits 2

  Stub Injection (uvoice.stub.*, stub HAL builds only):
    uvoice.stub                - Per-line and per-stream stub state (formatted)
    uvoice.stub.hook LINE STATE [PULSES]
                               - STATE: seize|offhook|release|onhook|flash|pulse
    uvoice.stub.digit LINE DIGITS [MS] - One DTMF digit event per character
    uvoice.stub.tone LINE CODE [local|remote] [MS]
                               - Inject a tone detector event; CODE: ced, ans,
                                 ansam, ansbar, cng, v21, cm, timeout, modem, other
    uvoice.stub.flash LINE     - Shorthand for a flash hook event
    uvoice.stub.ring LINE [MS] - Start ringing (non-zero MS rings that long)
    uvoice.stub.ring.stop LINE - Stop ringing

  JSON Output (uvoice.ubus.*):
    uvoice.ubus.status         - voice status
    uvoice.ubus.config         - voice dump_config
    uvoice.ubus.cdr            - voice cdr
    uvoice.ubus.stats [LINE]   - voice stats
    uvoice.ubus.stats.live LINE [SLOT] - voice stats_live (10s timeout)
    uvoice.ubus.stats.reset    - voice stats_reset
    uvoice.ubus.linetest LINE [TEST] - voice line_test (110s timeout)
    uvoice.ubus.looptest start|stop [MODE] - voice loop_test
    uvoice.ubus.register LINE  - voice register
    uvoice.ubus.unregister LINE - voice unregister
    uvoice.ubus.seize LINE     - voice seize
    uvoice.ubus.release LINE   - voice release
    uvoice.ubus.dial LINE NUMBER - voice dial
    uvoice.ubus.reload         - voice reload
    uvoice.ubus.trace          - voice.trace get
    uvoice.ubus.trace.set DOM on|off [DOM on|off ...] - voice.trace set
    uvoice.ubus.stub.dump      - voice.stub dump

  Environment:
    UVOICE_UBUS_SOCKET         - ubus socket path override (host testing)
    UVOICE_CLI                 - formatter path override
                                 (default /usr/share/uvoice/uvoice-cli.uc)
EOF
}

######################################################
# TABLE-FORMATTED HELPERS
######################################################

uvoice.status() {
	_uvoice_table status
}

uvoice.lines() {
	_uvoice_table lines
}

uvoice.line() {
	if ! _uvoice_uint_check "${1:-}"; then
		echo "Usage: uvoice.line <LINE>"
		return 1
	fi
	_uvoice_table line --line "$1"
}

uvoice.line.watch() {
	if ! _uvoice_uint_check "${1:-}"; then
		echo "Usage: uvoice.line.watch <LINE>"
		return 1
	fi
	_uvoice_check || return 1
	while true; do
		clear
		_uvoice_table line --line "$1"
		sleep 2
	done
}

uvoice.calls() {
	_uvoice_table calls
}

uvoice.calls.watch() {
	_uvoice_check || return 1
	while true; do
		clear
		_uvoice_table calls
		sleep 2
	done
}

uvoice.streams() {
	_uvoice_table streams
}

uvoice.jb() {
	_uvoice_table jb
}

uvoice.jb.watch() {
	_uvoice_check || return 1
	while true; do
		clear
		_uvoice_table jb
		sleep 2
	done
}

uvoice.lec() {
	_uvoice_table lec
}

uvoice.lec.watch() {
	_uvoice_check || return 1
	while true; do
		clear
		_uvoice_table lec
		sleep 2
	done
}

uvoice.monitor() {
	_uvoice_table monitor
}

uvoice.monitor.watch() {
	_uvoice_check || return 1
	while true; do
		clear
		_uvoice_table monitor
		sleep 2
	done
}

uvoice.cdr() {
	if [ -z "${1:-}" ]; then
		_uvoice_table cdr
		return
	fi
	if ! _uvoice_uint_check "$1"; then
		echo "Usage: uvoice.cdr [N]"
		return 1
	fi
	_uvoice_table cdr --limit "$1"
}

uvoice.cdr.seq() {
	if ! _uvoice_uint_check "${1:-}"; then
		echo "Usage: uvoice.cdr.seq <SEQ>"
		return 1
	fi
	_uvoice_table cdr --seq "$1"
}

uvoice.stats() {
	if [ -z "${1:-}" ]; then
		_uvoice_table stats
		return
	fi
	if ! _uvoice_uint_check "$1"; then
		echo "Usage: uvoice.stats [LINE]"
		return 1
	fi
	_uvoice_table stats --line "$1"
}

uvoice.stats.live() {
	if ! _uvoice_uint_check "${1:-}"; then
		echo "Usage: uvoice.stats.live <LINE> [SLOT]   (slot default 0)"
		return 1
	fi
	if [ -n "${2:-}" ] && ! _uvoice_uint_check "$2"; then
		echo "SLOT must be a number"
		return 1
	fi
	_uvoice_table stats_live --line "$1" --slot "${2:-0}"
}

uvoice.stats.reset() {
	_uvoice_table stats_reset
}

uvoice.config() {
	_uvoice_table config
}

uvoice.config.channel() {
	if ! _uvoice_uint_check "${1:-}"; then
		echo "Usage: uvoice.config.channel <N>"
		return 1
	fi
	_uvoice_table config --channel "$1"
}

uvoice.config.codecs() {
	_uvoice_table config --codecs
}

uvoice.linetest() {
	if ! _uvoice_uint_check "${1:-}"; then
		echo "Usage: uvoice.linetest <LINE> [gr909|roh|capacitance|loop]"
		return 1
	fi
	_uvoice_table linetest --line "$1" --test "${2:-gr909}"
}

uvoice.looptest() {
	case ${1:-} in
	start | stop) ;;
	*)
		echo "Usage: uvoice.looptest <start|stop> [self|cross]"
		return 1
		;;
	esac
	if [ -n "${2:-}" ]; then
		_uvoice_table looptest "--$1" --mode "$2"
		return
	fi
	_uvoice_table looptest "--$1"
}

uvoice.trace() {
	_uvoice_table trace
}

uvoice.trace.set() {
	if [ $# -lt 2 ] || [ $(($# % 2)) -ne 0 ]; then
		echo "Usage: uvoice.trace.set <DOMAIN> <on|off> [<DOMAIN> <on|off> ...]"
		echo "  domains: sip fsm hook hal dialplan media jb"
		return 1
	fi
	local flags=""
	while [ $# -gt 0 ]; do
		flags="$flags --$1 $2"
		shift 2
	done
	# The list is meant to split into one option per word: a domain name
	# and an on/off word carry no space and no glob character.
	# shellcheck disable=SC2086
	_uvoice_table trace_set $flags
}

uvoice.register() {
	if ! _uvoice_uint_check "${1:-}"; then
		echo "Usage: uvoice.register <LINE>"
		return 1
	fi
	_uvoice_table register --line "$1"
}

uvoice.unregister() {
	if ! _uvoice_uint_check "${1:-}"; then
		echo "Usage: uvoice.unregister <LINE>"
		return 1
	fi
	_uvoice_table unregister --line "$1"
}

uvoice.seize() {
	if ! _uvoice_uint_check "${1:-}"; then
		echo "Usage: uvoice.seize <LINE>"
		return 1
	fi
	_uvoice_table seize --line "$1"
}

uvoice.release() {
	if ! _uvoice_uint_check "${1:-}"; then
		echo "Usage: uvoice.release <LINE>"
		return 1
	fi
	_uvoice_table release --line "$1"
}

uvoice.dial() {
	if ! _uvoice_uint_check "${1:-}" || [ -z "${2:-}" ]; then
		echo "Usage: uvoice.dial <LINE> <NUMBER>"
		return 1
	fi
	case $2 in
	*[!0-9*#]*)
		echo "The number must be 1 to 32 characters from 0-9, * and #"
		return 1
		;;
	esac
	_uvoice_table dial --line "$1" --number "$2"
}

uvoice.calltrace.arm() {
	if ! _uvoice_uint_check "${1:-}"; then
		echo "Usage: uvoice.calltrace.arm <0-based index> [timeout seconds]"
		return 1
	fi
	if [ -n "${2:-}" ]; then
		_uvoice_table calltrace_arm --line "$1" --timeout "$2"
	else
		_uvoice_table calltrace_arm --line "$1"
	fi
}

uvoice.calltrace.status() {
	_uvoice_table calltrace_status
}

uvoice.calltrace.cancel() {
	_uvoice_table calltrace_cancel
}

uvoice.calltrace.wait() {
	_uvoice_table calltrace_wait
}

uvoice.reload() {
	_uvoice_table reload
}

######################################################
# STUB INJECTION
######################################################

uvoice.stub() {
	_uvoice_stub_check || return 1
	_uvoice_table stub_dump
}

uvoice.stub.hook() {
	if ! _uvoice_uint_check "${1:-}" || [ -z "${2:-}" ]; then
		echo "Usage: uvoice.stub.hook <LINE> <seize|offhook|release|onhook|flash|pulse> [PULSES]"
		return 1
	fi
	case $2 in
	seize | offhook | release | onhook | flash | pulse) ;;
	*)
		echo "Unknown hook state \"$2\"; expected seize|offhook|release|onhook|flash|pulse"
		return 1
		;;
	esac
	if [ -n "${3:-}" ] && ! _uvoice_uint_check "$3"; then
		echo "PULSES must be a number"
		return 1
	fi
	_uvoice_stub_check || return 1
	if [ -n "${3:-}" ]; then
		_uvoice_ubus call voice.stub hook "{ \"line\": $1, \"state\": \"$2\", \"pulses\": $3 }"
	else
		_uvoice_ubus call voice.stub hook "{ \"line\": $1, \"state\": \"$2\" }"
	fi
}

uvoice.stub.digit() {
	local digits=${2:-} duration=${3:-100} c
	if ! _uvoice_uint_check "${1:-}" || [ -z "$digits" ]; then
		echo "Usage: uvoice.stub.digit <LINE> <DIGITS> [MS]"
		return 1
	fi
	case $digits in
	*[!0-9A-Da-d*#]*)
		echo "DIGITS must use the DTMF alphabet 0-9, A-D, * and #"
		return 1
		;;
	esac
	if ! _uvoice_uint_check "$duration"; then
		echo "MS must be a number"
		return 1
	fi
	_uvoice_stub_check || return 1
	while [ -n "$digits" ]; do
		c=${digits%"${digits#?}"}
		digits=${digits#?}
		_uvoice_ubus call voice.stub digit \
			"{ \"line\": $1, \"digit\": \"$c\", \"duration\": $duration }" || return 1
	done
}

uvoice.stub.tone() {
	local direction=${3:-local}
	if ! _uvoice_uint_check "${1:-}" || [ -z "${2:-}" ]; then
		echo "Usage: uvoice.stub.tone <LINE> <CODE> [local|remote] [MS]"
		return 1
	fi
	case $2 in
	*[!A-Za-z0-9_-]*)
		echo "CODE must use letters, digits, _ and -"
		return 1
		;;
	esac
	case $direction in
	local | remote) ;;
	*)
		echo "The direction must be local or remote"
		return 1
		;;
	esac
	if [ -n "${4:-}" ] && ! _uvoice_uint_check "$4"; then
		echo "MS must be a number"
		return 1
	fi
	_uvoice_stub_check || return 1
	if [ -n "${4:-}" ]; then
		_uvoice_ubus call voice.stub tone \
			"{ \"line\": $1, \"code\": \"$2\", \"direction\": \"$direction\", \"duration\": $4 }"
	else
		_uvoice_ubus call voice.stub tone \
			"{ \"line\": $1, \"code\": \"$2\", \"direction\": \"$direction\" }"
	fi
}

uvoice.stub.flash() {
	if ! _uvoice_uint_check "${1:-}"; then
		echo "Usage: uvoice.stub.flash <LINE>"
		return 1
	fi
	_uvoice_stub_check || return 1
	_uvoice_ubus call voice.stub flash "{ \"line\": $1 }"
}

uvoice.stub.ring() {
	if ! _uvoice_uint_check "${1:-}"; then
		echo "Usage: uvoice.stub.ring <LINE> [MS]"
		return 1
	fi
	if [ -n "${2:-}" ] && ! _uvoice_uint_check "$2"; then
		echo "MS must be a number"
		return 1
	fi
	_uvoice_stub_check || return 1
	if [ -n "${2:-}" ]; then
		_uvoice_ubus call voice.stub ring "{ \"line\": $1, \"duration\": $2 }"
	else
		_uvoice_ubus call voice.stub ring "{ \"line\": $1 }"
	fi
}

uvoice.stub.ring.stop() {
	if ! _uvoice_uint_check "${1:-}"; then
		echo "Usage: uvoice.stub.ring.stop <LINE>"
		return 1
	fi
	_uvoice_stub_check || return 1
	_uvoice_ubus call voice.stub ring_stop "{ \"line\": $1 }"
}

######################################################
# UBUS HELPERS
######################################################

uvoice.ubus.status() {
	_uvoice_ubus -t 5 call voice status
}

uvoice.ubus.config() {
	_uvoice_ubus -t 5 call voice dump_config
}

uvoice.ubus.cdr() {
	_uvoice_ubus -t 5 call voice cdr
}

uvoice.ubus.stats() {
	if [ -z "${1:-}" ]; then
		_uvoice_ubus -t 5 call voice stats
		return
	fi
	if ! _uvoice_uint_check "$1"; then
		echo "Usage: uvoice.ubus.stats [LINE]"
		return 1
	fi
	_uvoice_ubus -t 5 call voice stats "{ \"line\": $1 }"
}

uvoice.ubus.stats.live() {
	if ! _uvoice_uint_check "${1:-}"; then
		echo "Usage: uvoice.ubus.stats.live <LINE> [SLOT]   (slot default 0)"
		return 1
	fi
	if [ -n "${2:-}" ] && ! _uvoice_uint_check "$2"; then
		echo "SLOT must be a number"
		return 1
	fi
	# 10s outlives the daemon's 5s backstop so the daemon reports a query
	# that never completes
	_uvoice_ubus -t 10 call voice stats_live "{ \"line\": $1, \"slot\": ${2:-0} }"
}

uvoice.ubus.stats.reset() {
	_uvoice_ubus -t 5 call voice stats_reset
}

uvoice.ubus.linetest() {
	if ! _uvoice_uint_check "${1:-}"; then
		echo "Usage: uvoice.ubus.linetest <LINE> [gr909|roh|capacitance|loop]"
		return 1
	fi
	case ${2:-gr909} in
	gr909 | roh | capacitance | loop) ;;
	*)
		echo "TEST must be gr909, roh, capacitance or loop"
		return 1
		;;
	esac
	# 110s outlives the daemon's line test budget so the daemon reports a
	# test that never completes
	_uvoice_ubus -t 110 call voice line_test "{ \"line\": $1, \"test\": \"${2:-gr909}\" }"
}

uvoice.ubus.looptest() {
	local enable
	case ${1:-} in
	start)
		enable=true
		;;
	stop)
		enable=false
		;;
	*)
		echo "Usage: uvoice.ubus.looptest <start|stop> [self|cross]"
		return 1
		;;
	esac
	case ${2:-cross} in
	self | cross) ;;
	*)
		echo "MODE must be self or cross"
		return 1
		;;
	esac
	_uvoice_ubus -t 5 call voice loop_test \
		"{ \"enable\": $enable, \"mode\": \"${2:-cross}\" }"
}

uvoice.ubus.register() {
	if ! _uvoice_uint_check "${1:-}"; then
		echo "Usage: uvoice.ubus.register <LINE>"
		return 1
	fi
	_uvoice_ubus -t 5 call voice register "{ \"line\": $1 }"
}

uvoice.ubus.unregister() {
	if ! _uvoice_uint_check "${1:-}"; then
		echo "Usage: uvoice.ubus.unregister <LINE>"
		return 1
	fi
	_uvoice_ubus -t 5 call voice unregister "{ \"line\": $1 }"
}

uvoice.ubus.seize() {
	if ! _uvoice_uint_check "${1:-}"; then
		echo "Usage: uvoice.ubus.seize <LINE>"
		return 1
	fi
	_uvoice_ubus -t 5 call voice seize "{ \"line\": $1 }"
}

uvoice.ubus.release() {
	if ! _uvoice_uint_check "${1:-}"; then
		echo "Usage: uvoice.ubus.release <LINE>"
		return 1
	fi
	_uvoice_ubus -t 5 call voice release "{ \"line\": $1 }"
}

uvoice.ubus.dial() {
	if ! _uvoice_uint_check "${1:-}" || [ -z "${2:-}" ]; then
		echo "Usage: uvoice.ubus.dial <LINE> <NUMBER>"
		return 1
	fi
	case $2 in
	*[!0-9*#]*)
		echo "The number must be 1 to 32 characters from 0-9, * and #"
		return 1
		;;
	esac
	_uvoice_ubus -t 5 call voice dial "{ \"line\": $1, \"number\": \"$2\" }"
}

uvoice.ubus.reload() {
	_uvoice_ubus -t 5 call voice reload
}

uvoice.ubus.trace() {
	_uvoice_ubus -t 5 call voice.trace get
}

uvoice.ubus.trace.set() {
	if [ $# -lt 2 ] || [ $(($# % 2)) -ne 0 ]; then
		echo "Usage: uvoice.ubus.trace.set <DOMAIN> <on|off> [<DOMAIN> <on|off> ...]"
		echo "  domains: sip fsm hook hal dialplan media jb"
		return 1
	fi
	local json="" value
	while [ $# -gt 0 ]; do
		case $1 in
		sip | fsm | hook | hal | dialplan | media | jb) ;;
		*)
			echo "Unknown domain \"$1\"; expected sip fsm hook hal dialplan media jb"
			return 1
			;;
		esac
		case $2 in
		on) value=true ;;
		off) value=false ;;
		*)
			echo "The value for $1 must be on or off"
			return 1
			;;
		esac
		json="${json:+$json, }\"$1\": $value"
		shift 2
	done
	_uvoice_ubus -t 5 call voice.trace set "{ $json }"
}

uvoice.ubus.stub.dump() {
	_uvoice_stub_check || return 1
	_uvoice_ubus -t 5 call voice.stub dump
}
