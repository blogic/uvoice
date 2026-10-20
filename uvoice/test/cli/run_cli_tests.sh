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
# run_cli_tests.sh: voice-cli and voip-cli-dispatch against the stub daemon.
#
# The suite needs a stub uvoiced, built from src with cmake -DUVOICE_HAL=stub
# against a host prefix that holds libuci and the uci CLI. It runs the daemon
# under "unshare -rmn": a user namespace, widened by a mount and a network
# namespace. ubusd refuses object publication to a non root client.
# The two scripts name absolute paths (/usr/srg/scripts, /etc/init.d) that only
# a mount namespace can supply off target, so an unprivileged overlay over /usr
# and /etc puts the real paths in place and the suite exercises the shipped
# strings and not a rewritten copy. The network namespace keeps the SIP port
# the account binds free whatever else the build host runs, which is what makes
# the rendered registration state repeatable.
#
#   UVOICED         stub daemon binary   (default $WORK/build/uvoiced)
#   UVOICE_PREFIX   host prefix with libuci and the uci CLI
#                                        (default $WORK/prefix)
#   WORK            host build work directory (default /tmp/uvoice-host)
#   VOIP_CLI_DISPATCH  voip-cli-dispatch source (default: the sibling
#                      voice-scripts package)
#   ARC_VOIP_UC     arc.voip handler source, for the vocabulary check
#                   (default: none, and the check skips)
#   VOIP_XML_DIR    klish scheme directory holding voip.xml, types.xml and
#                   marco.xml (default: none, and the checks skip)
#   UVOICE_UCODE_MODDIR  directory holding a ucode ubus.so, for the
#                   uvoice-cli.uc smoke (default: the modules the host ships;
#                   build one from lib/ubus.c of a ucode source tree)
#
# Output is TAP: one "ok"/"not ok" line per check, a trailing plan line, and a
# non-zero exit when any check fails.

here=$(cd "$(dirname "$0")" && pwd)
uvoice_dir=$(cd "$here/../.." && pwd)

work=${WORK:-/tmp/uvoice-host}
uvoiced=${UVOICED:-$work/build/uvoiced}
prefix=${UVOICE_PREFIX:-$work/prefix}
voice_cli_src=$uvoice_dir/files/voice-cli
dispatch_src=${VOIP_CLI_DISPATCH:-$uvoice_dir/../voice-scripts/files/usr/srg/scripts/voip-cli-dispatch}
arc_voip_uc=${ARC_VOIP_UC:-}
voip_xml_dir=${VOIP_XML_DIR:-}

export here uvoice_dir work uvoiced prefix voice_cli_src
export dispatch_src arc_voip_uc voip_xml_dir

# Re-enter with a user namespace (uid 0 for ubusd), a mount namespace (the
# absolute script paths) and a network namespace (the SIP port the account
# binds must be free whatever else the host runs).
if [ "${VOICE_CLI_TESTS_INNER:-}" != 1 ]; then
	VOICE_CLI_TESTS_INNER=1
	export VOICE_CLI_TESTS_INNER
	exec unshare -rmn "$0" "$@"
fi

bail() {
	printf 'Bail out! %s\n' "$*"
	exit 1
}

[ -x "$uvoiced" ] || bail "no stub daemon at $uvoiced: build src with cmake -DUVOICE_HAL=stub, or set UVOICED"
[ -x "$prefix/bin/uci" ] || bail "no uci CLI at $prefix/bin/uci: install libuci and the uci CLI in a prefix, and set UVOICE_PREFIX"
[ -r "$voice_cli_src" ] || bail "no voice-cli at $voice_cli_src"

tmp=$(mktemp -d) || bail "mktemp failed"

cleanup() {
	[ -n "$uvoiced_pid" ] && kill "$uvoiced_pid" 2>/dev/null
	[ -n "$ubusd_pid" ] && kill "$ubusd_pid" 2>/dev/null
	[ -n "$bare_ubusd_pid" ] && kill "$bare_ubusd_pid" 2>/dev/null
	cd /
	umount -l /tmp/uvoice 2>/dev/null
	umount -l /tmp/.uci 2>/dev/null
	umount -l /etc/init.d 2>/dev/null
	umount -l /usr 2>/dev/null
	rm -rf "$tmp"
}
trap 'cleanup' EXIT

mkdir -p "$tmp/usr-up/share" "$tmp/usr-work" "$tmp/initd" \
	"$tmp/bin" "$tmp/cfg" "$tmp/uci-delta" "$tmp/rec"

# /usr/srg does not exist on a build host, so /usr takes an overlay and the new
# path lands in its upper layer. /etc/init.d does exist, and a directory the
# lower layer owns cannot be written from a user namespace, so that one takes a
# bind mount of a directory this suite owns.
mount -t overlay overlay \
	-o "lowerdir=/usr,upperdir=$tmp/usr-up,workdir=$tmp/usr-work" /usr ||
	bail 'cannot overlay /usr, the kernel refuses an unprivileged overlay mount'
mount --bind "$tmp/initd" /etc/init.d || bail 'cannot bind mount /etc/init.d'

# libuci keeps its built-in save path /tmp/.uci in the delta search list even
# when -t names another one (measured: a stale host /tmp/.uci/voip surfaces in
# every "uci changes voip" the wrapper makes), so an empty directory covers it
# inside this mount namespace.
mkdir -p /tmp/.uci "$tmp/uci-hostmask"
mount --bind "$tmp/uci-hostmask" /tmp/.uci || bail 'cannot bind mount /tmp/.uci'

# The controller's retention sweep must never see another run's archives.
mkdir -p /tmp/uvoice "$tmp/uvoice"
mount --bind "$tmp/uvoice" /tmp/uvoice || bail 'cannot isolate /tmp/uvoice'

mkdir -p /usr/share/uvoice
cp "$uvoice_dir/files/calltrace-finish.sh" /usr/share/uvoice/ ||
	bail 'cannot install call trace finisher'
cp "$uvoice_dir/files/calltrace-README.txt" /usr/share/uvoice/ ||
	bail 'cannot install call trace README'
cp "$uvoice_dir/files/calltrace-summary.uc" /usr/share/uvoice/ ||
	bail 'cannot install the call trace summary generator'
chmod +x /usr/share/uvoice/calltrace-finish.sh

#
# The device layout the two scripts name
#
mkdir -p /usr/srg/scripts
cp "$voice_cli_src" /usr/srg/scripts/voice-cli || bail 'cannot install voice-cli'
chmod +x /usr/srg/scripts/voice-cli

dispatch_present=0
if [ -r "$dispatch_src" ]; then
	cp "$dispatch_src" /usr/srg/scripts/voip-cli-dispatch
	chmod +x /usr/srg/scripts/voip-cli-dispatch
	dispatch_present=1
fi

init_rec=$tmp/rec/init.argv
voip_cli_rec=$tmp/rec/voip_cli.argv
other_test_rec=$tmp/rec/other_test.argv
logread_rec=$tmp/rec/logread.argv
export init_rec voip_cli_rec other_test_rec logread_rec

cat > /etc/init.d/uvoice <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$init_rec"
EOF
chmod +x /etc/init.d/uvoice
[ -x /etc/init.d/uvoice ] || bail 'cannot install the init script stand-in'

cat > /usr/srg/scripts/voip_cli.sh <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$voip_cli_rec"
printf 'VOIP_CLI_STUB %s\n' "$*"
EOF
chmod +x /usr/srg/scripts/voip_cli.sh

# The loop test of the other voice stack, installed beside voip_cli.sh.
# The dispatcher execs one of the two for the klish test-start and test-stop
# words on a board that runs the other stack, so the stand-ins record which
# one ran.
cat > /usr/srg/scripts/voip_test_start.sh <<'EOF'
#!/bin/sh
set -- start "$@"
printf '%s\n' "$*" >> "$other_test_rec"
printf 'OTHER_TEST_START_STUB\n'
EOF
cat > /usr/srg/scripts/voip_test_stop.sh <<'EOF'
#!/bin/sh
set -- stop "$@"
printf '%s\n' "$*" >> "$other_test_rec"
printf 'OTHER_TEST_STOP_STUB\n'
EOF
chmod +x /usr/srg/scripts/voip_test_start.sh /usr/srg/scripts/voip_test_stop.sh

#
# PATH shims. voice-cli calls plain ubus, uci and logread, so the harness
# redirects them here instead of asking the shipped script for a test hook.
#
ubus_socket=$tmp/ubus.sock
bare_ubus_socket=$tmp/bare.sock
harness_ubus_socket=$ubus_socket
ubus_rec=$tmp/rec/ubus.argv
ubus_stats_file=''
ubus_status_file=''
logread_fail=''
export harness_ubus_socket ubus_rec ubus_stats_file ubus_status_file
export logread_fail

cat > "$tmp/bin/ubus" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$ubus_rec"
# a scripted stats or status answer, so a rendered value can be compared with
# one this suite chose rather than with whatever the daemon happens to hold
case " $* " in
*" call voice stats "*)
	if [ -n "$ubus_stats_file" ]; then
		cat "$ubus_stats_file"
		exit 0
	fi
	;;
*" call voice status "*)
	if [ -n "$ubus_status_file" ]; then
		cat "$ubus_status_file"
		exit 0
	fi
	;;
esac
exec /usr/bin/ubus -s "$harness_ubus_socket" "$@"
EOF

cat > "$tmp/bin/uci" <<EOF
#!/bin/sh
LD_LIBRARY_PATH=$prefix/lib
export LD_LIBRARY_PATH
exec $prefix/bin/uci -c "\$UVOICE_UCI_CONFDIR" -t $tmp/uci-delta "\$@"
EOF

cat > "$tmp/bin/logread" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$logread_rec"
[ -n "$logread_fail" ] && exit 3
printf 'Jan  1 00:00:00 host uvoice: MARKER_ALPHA\n'
printf 'Jan  1 00:00:01 host uvoiced: MARKER_BETA\n'
EOF

chmod +x "$tmp/bin/ubus" "$tmp/bin/uci" "$tmp/bin/logread"
PATH=$tmp/bin:$PATH
export PATH

UVOICE_UCI_CONFDIR=$tmp/cfg
export UVOICE_UCI_CONFDIR

region_fixture_supported='US,GB,UK,AU,NZ,EU,AT,RS'
cat > "$tmp/cfg/voip" <<EOF
config base 'base'
	option enabled '1'
	option channels '2'
	option supported_regions '$region_fixture_supported'

config channel 'channel1'
	option id '0'
	option enabled '1'
	option sip_username '5551000'
	option sip_proxy '192.0.2.1'

config channel 'channel2'
	option id '1'
	option enabled '1'

config codec 'channel1_g711u'
	option channel '1'
	option tr104_id '1'
	option priority '1'
	option enabled '0'
EOF

LD_LIBRARY_PATH=$prefix/lib
export LD_LIBRARY_PATH

ubusd -s "$ubus_socket" >/dev/null 2>&1 &
ubusd_pid=$!
ubusd -s "$bare_ubus_socket" >/dev/null 2>&1 &
bare_ubusd_pid=$!
sleep 1
"$uvoiced" -s "$ubus_socket" > "$tmp/uvoiced.log" 2>&1 &
uvoiced_pid=$!
sleep 1

ubus list voice >/dev/null 2>&1 || bail 'the stub daemon published no voice object'

voice_signature=$(ubus -v list voice 2>/dev/null)
for voice_method in status stats cdr reload seize release dial stats_live \
	line_test loop_test; do
	case $voice_signature in
	*"\"$voice_method\":"*) ;;
	*) bail "the voice object carries no \"$voice_method\" method, the daemon is older than this suite" ;;
	esac
done
# The jb and lec rows are part of "voice status". A daemon with a standalone
# jb or lec method is older than this suite. It would fail the status checks
# below with a misleading diagnostic, so the check names it here.
case $voice_signature in
*'"jb":'* | *'"lec":'*)
	bail 'the voice object still carries a standalone jb or lec method, the daemon is older than this suite' ;;
esac

#
# TAP plumbing
#
tests_run=0
fails=0

ok() {
	tests_run=$((tests_run + 1))
	printf 'ok %s - %s\n' "$tests_run" "$1"
}

not_ok() {
	tests_run=$((tests_run + 1))
	fails=$((fails + 1))
	printf 'not ok %s - %s\n' "$tests_run" "$1"
	# a diagnostic runs to several lines when it carries a whole render or a
	# table diff, and TAP wants every one of them commented
	printf '%s\n' "$2" | sed 's/^/# /'
}

skip() {
	tests_run=$((tests_run + 1))
	printf 'ok %s - %s # SKIP %s\n' "$tests_run" "$1" "$2"
}

check_eq() {
	if [ "$2" = "$3" ]; then
		ok "$1"
	else
		not_ok "$1" "want [$2] got [$3]"
	fi
}

check_has() {
	case $3 in
	*"$2"*) ok "$1" ;;
	*) not_ok "$1" "missing [$2] in [$3]" ;;
	esac
}

check_lacks() {
	case $3 in
	*"$2"*) not_ok "$1" "unexpected [$2] in [$3]" ;;
	*) ok "$1" ;;
	esac
}

run_cli() {
	cli_out=$(/usr/srg/scripts/voice-cli "$@" 2>&1 </dev/null)
	cli_rc=$?
}

run_dispatch() {
	dispatch_out=$(/usr/srg/scripts/voip-cli-dispatch "$@" 2>&1 </dev/null)
	dispatch_rc=$?
}

call_state_get() {
	ubus -t 5 call voice status 2>/dev/null |
		jq -r --argjson i "$1" '((.call // []) | map(select(.line == $i)))[0].state // "none"'
}

status_field_get() {
	ubus -t 5 call voice status 2>/dev/null | jq -r "$1"
}

#
# The shipped dispatcher names the two device paths
#
if [ "$dispatch_present" = 1 ]; then
	dispatch_text=$(cat "$dispatch_src")
	check_has 'dispatch/names voice-cli' '/usr/srg/scripts/voice-cli' "$dispatch_text"
	check_has 'dispatch/names voip_cli.sh' '/usr/srg/scripts/voip_cli.sh' "$dispatch_text"
else
	skip 'dispatch/names voice-cli' "no dispatcher at $dispatch_src"
	skip 'dispatch/names voip_cli.sh' "no dispatcher at $dispatch_src"
fi

#
# Service verbs
#
: > "$init_rec"
run_cli start
check_eq 'service/start exit' 0 "$cli_rc"
check_eq 'service/start argv' 'start' "$(cat "$init_rec")"

: > "$init_rec"
run_cli stop
check_eq 'service/stop argv' 'stop' "$(cat "$init_rec")"

: > "$init_rec"
run_cli restart
check_eq 'service/restart argv' 'restart' "$(cat "$init_rec")"

#
# The 1 based CLI against the 0 based ubus surface
#
# Neither fixture line has a registrar target: line 1 stays resolving and line
# 2 has no account. A seize on either gets the fast busy chain, not a dial tone.
# The IPv6 section below seizes a line that has a target but no live
# registration, which gets the same chain.
run_cli seize 1
check_eq 'seize/line 1 exit' 0 "$cli_rc"
check_eq 'seize/line 1 moves ubus line 0' 'CLEARING' "$(call_state_get 0)"
check_eq 'seize/line 1 leaves ubus line 1' 'IDLE' "$(call_state_get 1)"

run_cli release 1
check_eq 'release/line 1 returns ubus line 0 to idle' 'IDLE' "$(call_state_get 0)"

run_cli seize 2
check_eq 'seize/line 2 moves ubus line 1' 'CLEARING' "$(call_state_get 1)"
check_eq 'seize/line 2 leaves ubus line 0' 'IDLE' "$(call_state_get 0)"
run_cli release 2

run_cli dial 1 5551234
check_eq 'dial/line 1 exit' 0 "$cli_rc"
check_has 'dial/line 1 echoes the number' 'Dialing 5551234' "$cli_out"
check_eq 'dial/line 1 with no target clears ubus line 0' 'CLEARING' \
	"$(call_state_get 0)"
check_eq 'dial/line 1 with no target collects no digit' '' \
	"$(status_field_get '((.call // []) | map(select(.line == 0)))[0].digits // ""')"
run_cli release 1

#
# Refusals
#
run_cli seize 9
check_eq 'refuse/seize of an absent line exits 1' 1 "$cli_rc"
check_has 'refuse/seize of an absent line names the verb' 'voice seize failed' "$cli_out"

before_state=$(call_state_get 0)
run_cli seize 0
check_eq 'refuse/seize 0 exits 1' 1 "$cli_rc"
check_eq 'refuse/seize 0 reaches no ubus verb' "$before_state" "$(call_state_get 0)"

run_cli seize two
check_eq 'refuse/seize of a non number exits 1' 1 "$cli_rc"
check_has 'refuse/seize of a non number explains' 'lines count from 1' "$cli_out"

run_cli dial 1
check_eq 'refuse/dial without a number exits 1' 1 "$cli_rc"
run_cli release 1

# voice-cli interpolates the number into the ubus json that it builds by hand.
# An unchecked crafted number could close the "number" string and append a
# second "line" key, which would win: the CLI would report success on the line
# it was given while another line is dialled.
: > "$ubus_rec"
run_cli dial 1 '1","line":1,"z":"'
dial_rec=$(cat "$ubus_rec")
check_eq 'refuse/dial of a crafted number exits 1' 1 "$cli_rc"
check_has 'refuse/dial of a crafted number names the alphabet' 'is not dialable' "$cli_out"
check_eq 'refuse/dial of a crafted number makes no ubus call at all' '' "$dial_rec"
check_eq 'refuse/dial of a crafted number leaves ubus line 1 idle' 'IDLE' "$(call_state_get 1)"
check_eq 'refuse/dial of a crafted number leaves ubus line 0 idle' 'IDLE' "$(call_state_get 0)"

: > "$ubus_rec"
run_cli dial 1 '*67#'
check_eq 'dial/a star code is still dialable' 0 "$cli_rc"
check_has 'dial/a star code reaches the daemon' 'call voice dial' "$(cat "$ubus_rec")"
run_cli release 1

run_cli dial 1 12345678901234567890123456789012
check_eq 'dial/32 characters accepted' 0 "$cli_rc"
run_cli release 1

run_cli dial 1 123456789012345678901234567890123
check_eq 'refuse/dial of 33 characters exits 1' 1 "$cli_rc"

run_cli dial 1 555-1234
check_eq 'refuse/dial of a number outside the alphabet exits 1' 1 "$cli_rc"

run_cli seize 99999999999999999999
check_eq 'refuse/seize of an oversized index exits 1' 1 "$cli_rc"
check_eq 'refuse/seize of an oversized index prints one friendly line' \
	'voice-cli: seize: line "99999999999999999999" is not a line number, lines count from 1' \
	"$cli_out"

#
# A leading zero is decimal to the person who typed it. The shell reads one as
# octal in "$(( ))". With no guard, seize 010 would drive line 7, and seize 08
# would print an arithmetic diagnostic ahead of the friendly refusal.
#
: > "$ubus_rec"
run_cli seize 010
check_eq 'leading-zero/seize 010 drives ubus line 9' \
	'-t 5 call voice seize {"line":9}' "$(cat "$ubus_rec")"
check_eq 'leading-zero/seize 010 exits 1 on the daemon refusal' 1 "$cli_rc"
check_has 'leading-zero/seize 010 names the verb' 'voice seize failed' "$cli_out"

: > "$ubus_rec"
run_cli seize 08
check_eq 'leading-zero/seize 08 drives ubus line 7' \
	'-t 5 call voice seize {"line":7}' "$(cat "$ubus_rec")"
check_eq 'leading-zero/seize 08 exits 1 on the daemon refusal' 1 "$cli_rc"
check_has 'leading-zero/seize 08 names the verb' 'voice seize failed' "$cli_out"
check_lacks 'leading-zero/seize 08 leaks no arithmetic diagnostic' 'arithmetic' "$cli_out"

run_cli seize 002
check_eq 'leading-zero/seize 002 moves ubus line 1' 'CLEARING' "$(call_state_get 1)"
check_eq 'leading-zero/seize 002 leaves ubus line 0' 'IDLE' "$(call_state_get 0)"
run_cli release 002
check_eq 'leading-zero/release 002 returns ubus line 1 to idle' 'IDLE' "$(call_state_get 1)"

# 0 is refused whatever it is spelled with: lines count from 1.
: > "$ubus_rec"
run_cli seize 00
check_eq 'leading-zero/seize 00 exits 1' 1 "$cli_rc"
check_eq 'leading-zero/seize 00 prints one friendly line' \
	'voice-cli: seize: line "00" is not a line number, lines count from 1' \
	"$cli_out"
check_eq 'leading-zero/seize 00 makes no ubus call at all' '' "$(cat "$ubus_rec")"

: > "$ubus_rec"
run_cli seize 0
check_eq 'leading-zero/seize 0 prints one friendly line' \
	'voice-cli: seize: line "0" is not a line number, lines count from 1' \
	"$cli_out"
check_eq 'leading-zero/seize 0 makes no ubus call at all' '' "$(cat "$ubus_rec")"

# The width bound reads the raw text, so a wide run of zeros is refused and not
# narrowed into range.
: > "$ubus_rec"
run_cli seize 0000000001
check_eq 'refuse/seize of a 10 character index exits 1' 1 "$cli_rc"
check_eq 'refuse/seize of a 10 character index prints one friendly line' \
	'voice-cli: seize: line "0000000001" is not a line number, lines count from 1' \
	"$cli_out"
check_eq 'refuse/seize of a 10 character index makes no ubus call at all' '' "$(cat "$ubus_rec")"

run_cli
check_eq 'refuse/no argument exits 1' 1 "$cli_rc"
check_has 'refuse/no argument prints usage' 'voice-cli <cmd>' "$cli_out"

#
# The account command verbs answer a body. Nothing answers DNS or SIP in this
# namespace, so the registration machine never initializes: register (a
# resolve kick) is the one command that succeeds here, and unregister answers
# its refusal. The unregister reply body is pinned by test_ubus.
#
reg_out=$(ubus -t 5 call voice register '{"line":0}' 2>/dev/null)
check_eq 'register/the reply names the line and the request' '0 register' \
	"$(printf '%s' "$reg_out" | jq -r '"\(.line) \(.requested)"')"
if ubus -t 5 call voice unregister '{"line":0}' >/dev/null 2>&1; then
	not_ok 'unregister/an uninitialized registration is refused' 'the call succeeded'
else
	ok 'unregister/an uninitialized registration is refused'
fi
if ubus -t 5 call voice register '{"line":9}' >/dev/null 2>&1; then
	not_ok 'register/a line the board does not carry is refused' 'the call succeeded'
else
	ok 'register/a line the board does not carry is refused'
fi

#
# The jitter buffer and canceller rows of "voice status" are a constant grid:
# one jb row per (line, stream) and one lec row per line, for the life of the
# daemon. A pair that never answered is not absent, it reads sample never, so
# a consumer never handles a missing pair. The live media rows sit beside them,
# gated on their own op, so streams stays empty until a stream runs. The event
# cache keeps its own jb_events key.
#
# Nothing has run a stream yet, so every jb and lec row reads never.
status_out=$(ubus -t 5 call voice status 2>/dev/null)
# The stub SLIC reports no fault, so no call row carries the fault key. The
# key is present only while a SLIC fault holds a line out of service
# (line/line.c lines_status_dump()).
check_eq 'call/no row carries the fault key on the stub' 0 \
	"$(printf '%s' "$status_out" |
		jq -r '[.call[] | select(has("fault"))] | length')"
check_eq 'jb/the grid is constant at 2x2 rows' 4 \
	"$(printf '%s' "$status_out" | jq -r '.jb | length')"
check_eq 'jb/every row reads never before any stream ran' 4 \
	"$(printf '%s' "$status_out" |
		jq -r '[.jb[] |
			select(.sample == "never" and
			       has("stamp_ms") == false)] | length')"
# A never row measured no playout mode either, so it names none. The enum's
# zero word is "adaptive", which would read as a mode the buffer runs.
check_eq 'jb/every never row names an unknown playout mode' 4 \
	"$(printf '%s' "$status_out" |
		jq -r '[.jb[] | select(.mode == "unknown")] | length')"
check_eq 'lec/the grid is constant at one row per line' 2 \
	"$(printf '%s' "$status_out" | jq -r '.lec | length')"
check_eq 'lec/every row reads never before any canceller ran' 2 \
	"$(printf '%s' "$status_out" |
		jq -r '[.lec[] |
			select(.sample == "never" and
			       has("stamp_ms") == false and
			       has("state") == false)] | length')"
# The two layers that measure nothing stand in a never row too: a line that
# never ran a canceller still has a configuration and still runs a policy.
check_eq 'lec/a never row still names its uci options' \
	'control enable tone_force_disable tone_latch' \
	"$(printf '%s' "$status_out" |
		jq -r '.lec[0].config | keys | join(" ")')"
check_eq 'lec/a never row still names the policy and its cause' 'voice config' \
	"$(printf '%s' "$status_out" |
		jq -r '.lec[0] | "\(.policy.kind) \(.policy.source)"')"
# Both lines were taken off hook by the seize verbs above, so both carry the
# record of that push: it landed, it is current, it asked for the canceller
# and it names the instant it went out.
check_eq 'lec/a seized line carries the record of its push' \
	'ok false true true|ok false true true' \
	"$(printf '%s' "$status_out" |
		jq -r '[.lec[] |
			"\(.applied.status) \(.applied.pending) " +
			"\(.applied.ec) \(.applied.stamp_ms > 0)"] |
			join("|")')"
# The line monitor grid beside them, one row per line for the life of the
# daemon. The stub backend carries no monitor op at all, so every row reads
# never: no instant, no measurement and no decode. The three fault keys are the
# backend engine's own state of the line. They are defined before the first
# reading, so they stand in a never row too.
check_eq 'monitor/the grid is constant at one row per line' 2 \
	"$(printf '%s' "$status_out" | jq -r '.monitor | length')"
check_eq 'monitor/every row reads never on a backend with no monitor' 2 \
	"$(printf '%s' "$status_out" |
		jq -r '[.monitor[] |
			select(.sample == "never" and
			       has("stamp_ms") == false and
			       has("vbat_mv") == false and
			       has("dcdc_down") == false)] | length')"
check_eq 'monitor/a never row still names the fault state of the line' \
	'0 false false' \
	"$(printf '%s' "$status_out" |
		jq -r '.monitor[0] |
			"\(.fault_bits) \(.fault_open) \(.fault_held)"')"
check_eq 'monitor/each line carries its own row' '0 never|1 never' \
	"$(printf '%s' "$status_out" |
		jq -r '[.monitor[] | "\(.line) \(.sample)"] | join("|")')"
check_eq 'status/the event cache keeps its own jb_events key' 'array' \
	"$(printf '%s' "$status_out" | jq -r '.jb_events | type')"
check_eq 'streams/an array is always present' 'array' \
	"$(printf '%s' "$status_out" | jq -r '.streams | type')"
check_eq 'streams/an idle backend reports no media row' 0 \
	"$(printf '%s' "$status_out" | jq -r '.streams | length')"

# The stub answers a running stream, which is what a real backend does, so the
# media stream of line 0 is brought up the way the hook verb brings a handset
# off hook.
ubus -t 5 call voice.stub stream '{"line":0,"stream":0,"up":true}' >/dev/null 2>&1
status_out=$(ubus -t 5 call voice status 2>/dev/null)
check_eq 'jb/the grid stays constant with one stream up' 4 \
	"$(printf '%s' "$status_out" | jq -r '.jb | length')"
check_eq 'jb/exactly the running pair is live' 1 \
	"$(printf '%s' "$status_out" |
		jq -r '[.jb[] | select(.sample == "live")] | length')"
check_eq 'jb/the live pair reports its counters' '0 0 4800 12 11 13 50 57' \
	"$(printf '%s' "$status_out" |
		jq -r '[.jb[] | select(.sample == "live")][0] |
			"\(.line) \(.stream) \(.frames_out) \(.plc) " +
			"\(.hold_drop) \(.resync_discard_ms) " +
			"\(.target_ms) \(.pool_free)"')"
# The stub applies the adaptive mode to a stream it brings up, and the row
# reports the mode the engine accepted rather than a constant of the daemon.
check_eq 'jb/the live pair names the playout mode' 'adaptive' \
	"$(printf '%s' "$status_out" |
		jq -r '[.jb[] | select(.sample == "live")][0] | .mode')"
check_eq 'jb/a live row names no instant, it was read in this frame' 'false' \
	"$(printf '%s' "$status_out" |
		jq -r '[.jb[] | select(.sample == "live")][0] |
			has("stamp_ms")')"
check_eq 'streams/a running stream reports a media row' '1 0 0' \
	"$(printf '%s' "$status_out" |
		jq -r '"\(.streams | length) \(.streams[0].line) \(.streams[0].stream)"')"
check_eq 'streams/the row carries the canned media counters' \
	'1500 1498 240000 4.30 88' \
	"$(printf '%s' "$status_out" |
		jq -r '.streams[0].media |
			"\(.packets_sent) \(.packets_recv) \(.bytes_sent) " +
			"\(.mos_lq) \(.rfactor)"')"
# The stub stream was brought up by hand, no call leg owns it, and a stream no
# leg owns has no UI name to carry.
check_eq 'streams/a stream no leg owns names no ui slot' 'false' \
	"$(printf '%s' "$status_out" | jq -r '.streams[0] | has("ui_slot")')"

# A second pair, on the other line and the other stream of a 2x2 cap. The stub
# answers frames_out plus an offset per pair, so a row carrying another
# stream's numbers is visible.
ubus -t 5 call voice.stub stream '{"line":1,"stream":1,"up":true}' >/dev/null 2>&1
status_out=$(ubus -t 5 call voice status 2>/dev/null)
check_eq 'jb/the grid is still four rows with two streams up' 4 \
	"$(printf '%s' "$status_out" | jq -r '.jb | length')"
check_eq 'jb/the two live pairs carry their own identity and counters' \
	'0 0 4800 11 13|1 1 4910 11 13' \
	"$(printf '%s' "$status_out" |
		jq -r '[.jb[] | select(.sample == "live") |
			"\(.line) \(.stream) \(.frames_out) " +
			"\(.hold_drop) \(.resync_discard_ms)"] |
			join("|")')"
check_eq 'streams/two running streams report two media rows' '0 0|1 1' \
	"$(printf '%s' "$status_out" |
		jq -r '[.streams[] | "\(.line) \(.stream)"] | join("|")')"

# The line canceller grid beside the jitter buffer one. Both streams brought
# up above are still running, so both lines answer a live canceller row; the
# frames offset per line makes a row that picked up another line's fill
# visible (the stub comment says so).
check_eq 'lec/two running lines report two live rows' 2 \
	"$(printf '%s' "$status_out" |
		jq -r '[.lec[] | select(.sample == "live")] | length')"
check_eq 'lec/each live row carries its own identity and fields' \
	'0 6000 48 384 240|1 6100 48 384 240' \
	"$(printf '%s' "$status_out" |
		jq -r '[.lec[] | select(.sample == "live") |
			"\(.line) \(.state.frames) \(.state.tail_ms) " +
			"\(.state.taps) \(.state.ref_delay_samples)"] |
			join("|")')"
check_eq 'lec/the flags are booleans, not strings' \
	'boolean true boolean true' \
	"$(printf '%s' "$status_out" |
		jq -r '[.lec[] | select(.sample == "live")][0] |
			"\(.state.double_talk|type) \(.state.double_talk) " +
			"\(.state.ref_ready|type) \(.state.ref_ready)"')"
# The stub fills the shape of a voice call: the filter ran, the processor is on
# and it acted on the frame, so the two words read on and acting.
check_eq 'lec/the live row words the canceller and its processor' 'on acting' \
	"$(printf '%s' "$status_out" |
		jq -r '[.lec[] | select(.sample == "live")][0] |
			"\(.state.ec) \(.state.nlp)"')"
check_eq 'lec/a live row names no instant, it was read in this frame' 'false' \
	"$(printf '%s' "$status_out" |
		jq -r '[.lec[] | select(.sample == "live")][0] |
			has("stamp_ms")')"
# The stub names no carrier on a running stream, and no leg describes one, so
# the canceller follows its configuration and the daemon can name the cause.
check_eq 'lec/the live row names the policy and its cause' 'voice config' \
	"$(printf '%s' "$status_out" |
		jq -r '[.lec[] | select(.sample == "live")][0] |
			"\(.policy.kind) \(.policy.source)"')"
# The processor verdicts, the peak tap and the loop flag of each row, against
# the canned stub values. The flag is a boolean, and the stub models no hook.
check_eq 'lec/each live row carries the verdicts, the peak tap and the loop flag' \
	'0 4944 91 310 470 17 19 84 false|1 4944 91 310 470 17 19 84 false' \
	"$(printf '%s' "$status_out" |
		jq -r '[.lec[] | select(.sample == "live") |
			"\(.line) \(.state.nlp_active_frames) " +
			"\(.state.nlp_wait_frames) \(.state.nlp_erle_frames) " +
			"\(.state.nlp_unlike_frames) \(.state.nlp_dt_frames) " +
			"\(.state.nlp_quiet_frames) \(.state.coef_peak_tap) " +
			"\(.state.loop_open)"] | join("|")')"
# The six verdicts sum under frames by the frames the processor was off for,
# and that remainder follows the per line offset of frames.
check_eq 'lec/the verdicts leave the processor-off frames as the remainder' \
	'0 149|1 249' \
	"$(printf '%s' "$status_out" |
		jq -r '[.lec[] | select(.sample == "live") |
			"\(.line) \(.state.frames - .state.nlp_active_frames -
				.state.nlp_wait_frames - .state.nlp_erle_frames -
				.state.nlp_unlike_frames - .state.nlp_dt_frames -
				.state.nlp_quiet_frames)"] | join("|")')"

#
# The stats line filter, with the refusal shape the old jb and lec filters
# carried for a line the board does not carry.
#
stats_out=$(ubus -t 5 call voice stats '{"line":1}' 2>/dev/null)
check_eq 'stats/a line filter serves that line alone' '1 1' \
	"$(printf '%s' "$stats_out" | jq -r '"\(.stats | length) \(.stats[0].line)"')"
stats_out=$(ubus -t 5 call voice stats '{"line":0}' 2>/dev/null)
check_eq 'stats/the filter reaches line 0 too' '1 0' \
	"$(printf '%s' "$stats_out" | jq -r '"\(.stats | length) \(.stats[0].line)"')"
stats_out=$(ubus -t 5 call voice stats 2>/dev/null)
check_eq 'stats/a bare call still sweeps every slot' 2 \
	"$(printf '%s' "$stats_out" | jq -r '.stats | length')"
if ubus -t 5 call voice stats '{"line":9}' >/dev/null 2>&1; then
	not_ok 'stats/a line the board does not carry is refused' 'the call succeeded'
else
	ok 'stats/a line the board does not carry is refused'
fi

#
# The media trace sweep. On the stub both getters answer only a running
# stream, the udsp gate (stream_stats refuses STREAM_IDLE), so a sweep line
# carries both halves and an idle stream produces no line at all. The
# RTP-alone shape stays reachable on hardware where the jitter buffer getter
# refuses on its own; the stub cannot produce it.
#
ubus -t 5 call voice.stub stream '{"line":0,"stream":0,"up":true}' >/dev/null 2>&1
ubus -t 5 call voice.trace set '{"media":true}' >/dev/null 2>&1
sleep 3
ubus -t 5 call voice.trace set '{"media":false}' >/dev/null 2>&1
media_log=$(cat "$tmp/uvoiced.log")
check_has 'trace/media sweeps the stream that answers both halves' \
	'l0 s0 jb out=4800 plc=12 cn=6 sil=3 late=5 dup=2 hold=11 ovf=1' \
	"$media_log"
check_has 'trace/that jitter buffer half carries discarded milliseconds' \
	'resync=1 starve=0 disc=13' "$media_log"
check_has 'trace/that jitter buffer half names the playout mode last' \
	'free=57 mode=adaptive' "$media_log"
check_has 'trace/that line carries the RTP half too' \
	'mode=adaptive rtp recv=' "$media_log"
check_lacks 'trace/an idle stream produces no sweep line at all' \
	'l0 s1 ' "$media_log"
# The canceller of the line takes two sweep lines: the first ends with the
# loop flag and the peak tap, and the processor verdicts take a line of their
# own under the same "l0 lec" start.
check_has 'trace/the canceller line ends with the loop flag and the peak tap' \
	'kind=voice loop_open=0 peak_tap=84' "$media_log"
check_has 'trace/the processor verdicts take a canceller line of their own' \
	'l0 lec nlp frames=6000 active=4944 wait=91 erle=310 unlike=470 dt=17 quiet=19' \
	"$media_log"

#
# The row outlives the call. The stream goes down, the getter refuses from
# that moment on, and the pair must stay in the constant grid carrying the last
# reading, the last_call word and the instant it was taken, while the pairs
# that never ran keep the never word and the other running pair stays live.
#
ubus -t 5 call voice.stub stream '{"line":0,"stream":0,"up":false}' >/dev/null 2>&1
status_out=$(ubus -t 5 call voice status 2>/dev/null)
check_eq 'jb/the grid stays four rows after a stream stops' 4 \
	"$(printf '%s' "$status_out" | jq -r '.jb | length')"
check_eq 'jb/the stopped pair reads the last call state' 'string last_call' \
	"$(printf '%s' "$status_out" |
		jq -r '[.jb[] | select(.line == 0 and .stream == 0)][0] |
			"\(.sample|type) \(.sample)"')"
check_eq 'jb/the stopped pair carries the last reading' \
	'0 0 4800 12 11 13 50 57' \
	"$(printf '%s' "$status_out" |
		jq -r '[.jb[] | select(.line == 0 and .stream == 0)][0] |
			"\(.line) \(.stream) \(.frames_out) \(.plc) " +
			"\(.hold_drop) \(.resync_discard_ms) " +
			"\(.target_ms) \(.pool_free)"')"
check_eq 'jb/the stopped pair names a nonzero instant, the last call state' 'true' \
	"$(printf '%s' "$status_out" |
		jq -r '[.jb[] | select(.line == 0 and .stream == 0)][0] | .stamp_ms > 0')"
# Every pair reads its own state in one word: the stopped one last_call, the
# two that never ran never, and the other running one live. The sweep is line
# major.
check_eq 'jb/each pair reads its own state in the grid' \
	'0 0 last_call|0 1 never|1 0 never|1 1 live' \
	"$(printf '%s' "$status_out" |
		jq -r '[.jb[] | "\(.line) \(.stream) \(.sample)"] | join("|")')"
# The media row has no cache by design: the stream is gone and its final
# counters live in the call record, so the row leaves with the stream.
check_eq 'streams/a stopped stream loses its media row' '1 1 1' \
	"$(printf '%s' "$status_out" |
		jq -r '"\(.streams | length) \(.streams[0].line) \(.streams[0].stream)"')"
# The line canceller has a cache too: line 0 lost its only running stream, so
# its row falls to the cached reading, while line 1 stays live.
check_eq 'lec/the stopped line keeps its last canceller reading' \
	'last_call true' \
	"$(printf '%s' "$status_out" |
		jq -r '[.lec[] | select(.line == 0)][0] |
			"\(.sample) \(.stamp_ms > 0)"')"
check_eq 'lec/the line with a running stream stays live' 'live' \
	"$(printf '%s' "$status_out" |
		jq -r '[.lec[] | select(.line == 1)][0] | .sample')"
check_eq 'lec/each line reads its own state in the grid' \
	'0 last_call|1 live' \
	"$(printf '%s' "$status_out" |
		jq -r '[.lec[] | "\(.line) \(.sample)"] | join("|")')"

#
# The capability the shipped stub reports. The leg layer offers only what the
# backend advertises (call.h calls_hal_codecs_set), so this list is a shipped
# answer and not a detail. Both wideband codecs are in: the udsp media plane
# carries them, and the stub describes a DSP that matches it.
#
stub_codecs=$(status_field_get '.hal.codecs | join(",")')
check_has 'status/the stub advertises g722' 'g722' "$stub_codecs"
check_has 'status/the stub advertises opus' 'opus' "$stub_codecs"
check_eq 'status/the wideband digit fallback counter starts at zero' '0' \
	"$(status_field_get '.media.opus_dtmf_fallbacks')"

#
# The signaling table. The fixture names no protocol, so the daemon runs sip,
# the default word, and the SIP table adds its two account verbs to the voice
# object (proto/proto.c proto_status_dump(), ubus.c voice_methods_compose()).
# The build of UVOICED says whether the image carries the MGCP table too:
# UVOICE_PROTO_MGCP in the environment, else the CMake cache beside the
# binary.
#
image_mgcp=${UVOICE_PROTO_MGCP:-}
if [ -z "$image_mgcp" ]; then
	image_mgcp=0
	grep -q '^UVOICE_PROTO_MGCP:BOOL=ON$' \
		"$(dirname "$uvoiced")/CMakeCache.txt" 2>/dev/null &&
		image_mgcp=1
fi
image_available=sip
[ "$image_mgcp" = 1 ] && image_available='sip mgcp'
check_eq 'status/signaling names sip running' 'sip running' \
	"$(status_field_get '"\(.signaling.protocol) \(.signaling.state)"')"
check_eq 'status/signaling carries no error while the protocol runs' 'false' \
	"$(status_field_get '.signaling | has("error")')"
check_eq 'status/signaling lists the protocols of this image' \
	"$image_available" \
	"$(status_field_get '.signaling.available | join(" ")')"
check_has 'status/the sip table adds register to the voice object' \
	'"register":' "$voice_signature"
check_has 'status/the sip table adds unregister to the voice object' \
	'"unregister":' "$voice_signature"

#
# show version
#
daemon_version=$(status_field_get '.version')
run_cli show version
check_eq 'show/version exit' 0 "$cli_rc"
check_has 'show/version carries the daemon version' "$daemon_version" "$cli_out"
check_has 'show/version carries the HAL backend' 'stub' "$cli_out"

#
# uvoice-cli.uc, the table formatter uvoice.sh drives, against the same stub
# daemon. The script imports the ucode ubus module, which a build host does
# not always ship, so the section probes for it once and skips whole when it
# cannot run. UVOICE_UCODE_MODDIR names a directory holding a ubus.so built
# from lib/ubus.c of a ucode source tree.
#
cli_uc=$uvoice_dir/files/uvoice-cli.uc
uc_moddir=${UVOICE_UCODE_MODDIR:-}
uc_ready=0
uc_skip='no ucode ubus module, and UVOICE_UCODE_MODDIR names none'

if [ -n "$uc_moddir" ]; then
	ucode -L "$uc_moddir" -e "import * as ubus from 'ubus';" \
		>/dev/null 2>&1 && uc_ready=1
else
	ucode -e "import * as ubus from 'ubus';" >/dev/null 2>&1 && uc_ready=1
fi

run_uc() {
	if [ -n "$uc_moddir" ]; then
		uc_out=$(UVOICE_UBUS_SOCKET=$ubus_socket \
			ucode -L "$uc_moddir" "$cli_uc" -- "$@" 2>&1 </dev/null)
	else
		uc_out=$(UVOICE_UBUS_SOCKET=$ubus_socket \
			ucode "$cli_uc" -- "$@" 2>&1 </dev/null)
	fi
	uc_rc=$?
}

# The color leg: UVOICE_COLOR=1 turns the SGR layer on, and NO_COLOR must be
# absent because its presence wins over the request.
run_uc_color() {
	if [ -n "$uc_moddir" ]; then
		uc_out=$(env -u NO_COLOR UVOICE_COLOR=1 \
			UVOICE_UBUS_SOCKET="$ubus_socket" \
			ucode -L "$uc_moddir" "$cli_uc" -- "$@" 2>&1 </dev/null)
	else
		uc_out=$(env -u NO_COLOR UVOICE_COLOR=1 \
			UVOICE_UBUS_SOCKET="$ubus_socket" \
			ucode "$cli_uc" -- "$@" 2>&1 </dev/null)
	fi
	uc_rc=$?
}

# The display base leg: voice-cli sets both words, so a klish user reads
# 1-based line numbers and footers that name the klish verbs.
run_uc_base1() {
	if [ -n "$uc_moddir" ]; then
		uc_out=$(UVOICE_LINE_BASE=1 UVOICE_HINT_STYLE=klish \
			UVOICE_UBUS_SOCKET=$ubus_socket \
			ucode -L "$uc_moddir" "$cli_uc" -- "$@" 2>&1 </dev/null)
	else
		uc_out=$(UVOICE_LINE_BASE=1 UVOICE_HINT_STYLE=klish \
			UVOICE_UBUS_SOCKET=$ubus_socket \
			ucode "$cli_uc" -- "$@" 2>&1 </dev/null)
	fi
	uc_rc=$?
}

# voice-cli runs the formatter through a plain "ucode" on the PATH, so this
# shim puts the module directory in front of it, and UVOICE_CLI points the
# script at the source file instead of the installed copy. The formatter
# reads the socket from the environment, not through the ubus PATH shim.
ct_ucode=$(command -v ucode)
export ct_ucode uc_moddir
mkdir -p "$tmp/ct-ucode"
cat > "$tmp/ct-ucode/ucode" <<'EOF'
#!/bin/sh
if [ -n "$uc_moddir" ]; then
	exec "$ct_ucode" -L "$uc_moddir" "$@"
fi
exec "$ct_ucode" "$@"
EOF
chmod +x "$tmp/ct-ucode/ucode"

run_cli_uc() {
	cli_out=$(PATH=$tmp/ct-ucode:$PATH UVOICE_CLI=$cli_uc \
		UVOICE_UBUS_SOCKET=$ubus_socket \
		/usr/srg/scripts/voice-cli "$@" 2>&1 </dev/null)
	cli_rc=$?
}

uc_check_has() {
	if [ "$uc_ready" = 1 ]; then
		check_has "$1" "$2" "$3"
	else
		skip "$1" "$uc_skip"
	fi
}

uc_check_eq() {
	if [ "$uc_ready" = 1 ]; then
		check_eq "$1" "$2" "$3"
	else
		skip "$1" "$uc_skip"
	fi
}

uc_check_lacks() {
	if [ "$uc_ready" = 1 ]; then
		check_lacks "$1" "$2" "$3"
	else
		skip "$1" "$uc_skip"
	fi
}

uc_calls_out=''
uc_calls_rc=1
uc_calls_rows=0
uc_line_out=''
uc_line_color_out=''
uc_lec_out=''
uc_monitor_out=''
uc_jb_out=''
uc_config_out=''
uc_codecs_out=''
uc_lines_out=''
uc_lines_base1_out=''
uc_codecs_base1_out=''
uc_lec_base1_out=''
uc_rc=1

if [ "$uc_ready" = 1 ]; then
	# a running stream so one jb row reads live, brought up and taken down
	# the way the row checks above do, so the section leaves the grid as it
	# found it
	ubus -t 5 call voice.stub stream '{"line":0,"stream":0,"up":true}' \
		>/dev/null 2>&1
	run_uc line --line 0
	uc_line_out=$uc_out
	run_uc_color line --line 0
	uc_line_color_out=$uc_out
	run_uc calls
	uc_calls_out=$uc_out
	uc_calls_rc=$uc_rc
	# a call row starts with the line number in the first column, so the
	# count separates rendered rows from the leg lines under them
	uc_calls_rows=$(printf '%s\n' "$uc_calls_out" | grep -c '^[0-9]')
	run_uc lec
	uc_lec_out=$uc_out
	run_uc monitor
	uc_monitor_out=$uc_out
	run_uc jb
	uc_jb_out=$uc_out
	run_uc config --channel 0
	uc_config_out=$uc_out
	run_uc config --codecs
	uc_codecs_out=$uc_out
	run_uc lines
	uc_lines_out=$uc_out
	run_uc_base1 lines
	uc_lines_base1_out=$uc_out
	run_uc_base1 config --codecs
	uc_codecs_base1_out=$uc_out
	run_uc_base1 lec
	uc_lec_base1_out=$uc_out
	run_uc help
	ubus -t 5 call voice.stub stream '{"line":0,"stream":0,"up":false}' \
		>/dev/null 2>&1
fi

# The question row of the Account block. The fixture names a literal proxy and
# no port, so the line asks SRV. The row stands in front of the registration
# half, and this line never reaches that half: its proxy is unroutable, so it
# never takes a local address and stays resolving. The answer row is checked
# in the IPv6 section below, on a line that does register.
uc_check_has 'cli/line names the next-hop question' \
	'dns_lookup             srv' "$uc_line_out"

uc_check_has 'cli/line names the canceller section' 'Echo canceller' \
	"$uc_line_out"
# The three rows of the switch matrix. The uci row carries the ignored mark of
# a config without lec_control; the engine row carries the stub's two words.
uc_check_has 'cli/line names the uci row' 'uci                    yes*' \
	"$uc_line_out"
uc_check_has 'cli/line names the pushed row' 'pushed                 ' \
	"$uc_line_out"
uc_check_has 'cli/line names the policy and its source' \
	'policy                 voice, from config' "$uc_line_out"
uc_check_has 'cli/line names the engine row' \
	'engine                 on         acting' "$uc_line_out"
# The canned stub values of the events row, so the needle fails on a dropped
# key and on a key that carries the wrong field.
uc_check_has 'cli/line renders the retimes counter of the events row' \
	'    events                          2        5        3' "$uc_line_out"
# The derived values against the canned stub row: 384 taps over a 48 ms tail
# is 8 kHz, and 240 samples at that rate is 30 ms; 6000 frames of 10 ms are
# 60 s.
uc_check_has 'cli/line derives the line rate from the taps and the tail' \
	'384 taps (8 kHz)' "$uc_line_out"
uc_check_has 'cli/line derives the reference delay in ms' \
	'240 samples (30 ms)' "$uc_line_out"
uc_check_has 'cli/line derives the filter run time from the frames' \
	'6000   60.0 s' "$uc_line_out"
# The processor row shares the column widths of the run and events rows, and
# each count carries its share of the 6000 frames. The peak tap of 84 samples
# is 10.5 ms at the derived 8 kHz.
uc_check_has 'cli/line renders the processor verdicts as a matrix row' \
	'    processor              4944 (82%)  91 (2%) 310 (5%) 470 (8%)  17 (0%)  19 (0%)' \
	"$uc_line_out"
uc_check_has 'cli/line names the peak tap on the filter line' \
	'ready; peak tap 84 samples (10.5 ms)' "$uc_line_out"
uc_check_has 'cli/lec names the sample column' 'Line Sample' "$uc_lec_out"
uc_check_has 'cli/lec derives the rate column from the stub row' '8 kHz' \
	"$uc_lec_out"
# The duty divides the acted frames by the five verdicts of an active window,
# 4944 of 5832. The quiet and the processor-off frames stay out, which would
# read 84 or 82.
uc_check_has 'cli/lec names the duty column before the age' 'Pend  Duty Age' \
	"$uc_lec_out"
uc_check_has 'cli/lec derives the duty over the judged frames' '  85% -' \
	"$uc_lec_out"
uc_check_has 'cli/line names the line monitor section' 'Line monitor' \
	"$uc_line_out"
# The call state and the stats of a line print as the rows of the calls and
# stats tables, under their own headers.
uc_check_has 'cli/line renders the call state as the calls table' \
	'Line State         OOS' "$uc_line_out"
uc_check_has 'cli/line renders the stats as the stats table' \
	'PktSent    PktRecv' "$uc_line_out"
# The SGR layer rides UVOICE_COLOR alone: a run without it prints plain
# bytes, a run with it carries at least one escape.
uc_esc=$(printf '\033')
uc_check_lacks 'cli/line prints plain bytes without UVOICE_COLOR' "$uc_esc" \
	"$uc_line_out"
uc_check_has 'cli/line colors under UVOICE_COLOR=1' "$uc_esc" \
	"$uc_line_color_out"
uc_check_has 'cli/monitor names the sample column' 'Line Sample' \
	"$uc_monitor_out"
# The stub backend carries no monitor op, so both rows read never: no reading,
# an empty fault set and no age.
uc_check_has 'cli/monitor renders the never row of a line' '0    never' \
	"$uc_monitor_out"
# The table ends at its last row, the way the line monitor block does. Every
# cell names its own unit, so no footer has to.
uc_check_lacks 'cli/monitor prints no row count under the table' 'Total:' \
	"$uc_monitor_out"
uc_check_has 'cli/jb names the mode column' 'Sample    Mode' "$uc_jb_out"
uc_check_has 'cli/jb reads a live stub stream as adaptive' \
	'live      adaptive' "$uc_jb_out"
# The never pair on the wire reads zeros that measure nothing, so the table
# dashes its counters and keeps the two words the daemon chose.
uc_check_has 'cli/jb dashes the counters of a never row' \
	'never     unknown      -    -    -' "$uc_jb_out"
# The matrix block of line, against the canned stub stream the media trace
# check above reads (out=4800 plc=12 late=5 dup=2 hold=11 ovf=1 skew=7).
uc_check_has 'cli/line groups the jitter buffer output counters' \
	'4830     4800  12 (0%)' "$uc_line_out"
uc_check_has 'cli/line groups the jitter buffer drop counters' \
	'    drops                         5        2        1       11        7' \
	"$uc_line_out"
uc_check_has 'cli/config folds the unset lec_control' \
	'lec_control                          (unset)' "$uc_config_out"
# The one codec section of the config above: channel 1 in uci is the 0-based
# id 0 of every other view, and TR-104 id 1 is G.711u.
uc_check_has 'cli/config names the codec and the 0-based channel of a row' \
	'0       G.711u    1      no' "$uc_codecs_out"
# The display base moves the print alone: the same account reads line 0 by
# default and line 1 under the base voice-cli sets, and a footer names the
# klish verb under the klish hint style.
uc_check_eq 'cli/lines prints the account as line 0 by default' 1 \
	"$(printf '%s\n' "$uc_lines_out" | grep -c '^0 ')"
uc_check_eq 'cli/lines prints the account as line 1 under the base' 1 \
	"$(printf '%s\n' "$uc_lines_base1_out" | grep -c '^1 ')"
uc_check_has 'cli/config --codecs follows the base' \
	'1       G.711u    1      no' "$uc_codecs_base1_out"
uc_check_has 'cli/lec names the shell helper by default' \
	'uvoice.line N prints' "$uc_lec_out"
uc_check_has 'cli/lec names the klish verb under the klish hint style' \
	'show voip line N prints' "$uc_lec_base1_out"
# The flag rides the fault key of the call row, so a stub that never faults
# never prints it. The run has to succeed and to render a row per line first:
# the error text of a failed run carries no flag either.
if [ "$uc_ready" = 1 ]; then
	check_eq 'cli/calls exits 0' 0 "$uc_calls_rc"
	check_eq 'cli/calls renders one row per line' 2 "$uc_calls_rows"
else
	skip 'cli/calls exits 0' "$uc_skip"
	skip 'cli/calls renders one row per line' "$uc_skip"
fi
uc_check_lacks 'cli/calls prints no slic-fault flag on the stub' 'slic-fault' \
	"$uc_calls_out"

if [ "$uc_ready" = 1 ]; then
	check_eq 'cli/help exits 0' 0 "$uc_rc"
else
	skip 'cli/help exits 0' "$uc_skip"
fi

#
# The canned reply leg of the formatter. The stub backend carries no line
# monitor at all, so every row of the daemon reads never. The sampled and the
# stale rendering, the unit helpers and the fault bit names then have nothing
# to draw. A ubus stand-in module, in front of the real one on the module
# path, answers one canned "voice status" instead. That reaches those paths
# without a backend that measures a line. The stand-in replaces the ubus
# module, one of the two the formatter imports (fs is the other). This leg
# therefore needs no ubus module of its own. It skips where ucode itself is
# missing.
#
uc_fakemod=$tmp/fakemod
uc_canned_ready=0
uc_canned_skip='no ucode interpreter'
uc_canned_monitor=''
uc_canned_line=''

mkdir -p "$uc_fakemod"
cat > "$uc_fakemod/ubus.uc" <<'EOF'
// The ubus stand-in of the formatter smoke. Every connection answers the
// canned reply set FAKE_REPLIES carries, keyed by method name.
const replies = json(getenv("FAKE_REPLIES") ?? "{}");
let positions = {};

// With FAKE_CLOCK set, the formatter reads a clock that moves only by its own
// sleeps, so a wait of minutes of formatter time takes no wall time.
let fake_now_ms = 1000000000;
if (getenv("FAKE_CLOCK")) {
	global.clock = function(mono) {
		return [fake_now_ms / 1000, (fake_now_ms % 1000) * 1000000];
	};
	global.sleep = function(ms) {
		fake_now_ms += ms;
		return true;
	};
}

export function connect(sock, timeout) {
	let error;
	return {
		call: function(req) {
			let key = req.object + "." + req.method;
			let reply = replies[key] ?? replies[req.method] ?? {};
			if (type(reply) == "array") {
				let pos = positions[key] ?? 0;
				positions[key] = pos + 1;
				reply = reply[min(pos, length(reply) - 1)];
			}
			if (reply.__error) {
				error = reply.__error;
				return null;
			}
			if (reply.__echo)
				return req.data;
			return reply;
		},
		error: function() { return error; },
		list: function(object) { return []; },
		disconnect: function() { return true; }
	};
};
EOF

# The three rows the render needs. The sampled one carries a negative battery
# and a loop current above a milliampere. The stale one carries an instant and
# a converter word past 2^31, the sign-extended u32 of the wire. The never one
# carries a fault word with a bit this build does not name.
cat > "$tmp/gen_replies.uc" <<'EOF'
let now = clock(true);
let now_ms = now[0] * 1000 + now[1] / 1000000;
let recent = (now_ms - 3500) % 4294967296;

let sampled = {
	line: 0, sample: "sampled", stamp_ms: recent,
	vtr_mv: 1250, vtip_mv: -1002, vring_mv: 1003, vlong_mv: -1004,
	vbat_mv: -48250, vdc_mv: 0, itr_ua: -25400, itip_ua: 1008,
	iring_ua: -1009, ilong_ua: 999, p_hvic_mw: 1011,
	vbat_tgt_mv: -48000, vbat_irq_th_mv: 5000,
	pd_dcdc: 1048596, dcdc_status: 21, lcrrtp: 2, linefeed: 17,
	dcdc_down: true, ovlo: false, settled: true, voc_err: false,
	lcr: true, rtp: false,
	fault_bits: 130, fault_open: true, fault_held: false
};

let stale = {
	line: 1, sample: "stale", stamp_ms: -2147483632,
	vtr_mv: -2001, vtip_mv: 2002, vring_mv: -2003, vlong_mv: 2004,
	vbat_mv: -1000, vdc_mv: 0, itr_ua: 1000, itip_ua: -1000,
	iring_ua: 1001, ilong_ua: -1001, p_hvic_mw: 0,
	vbat_tgt_mv: 0, vbat_irq_th_mv: 0,
	pd_dcdc: -2146435052, dcdc_status: 4194304, lcrrtp: 9, linefeed: 0,
	dcdc_down: false, ovlo: true, settled: false, voc_err: true,
	lcr: false, rtp: true,
	fault_bits: 65922, fault_open: false, fault_held: true
};

let never = {
	line: 2, sample: "never",
	fault_bits: 258, fault_open: false, fault_held: true
};

// The rows below carry every key the daemon always writes, and no other,
// because the schema leg validates this reply against the shipped contract.
function call_row(line, state, oos) {
	return {
		line: line, state: state, oos: oos, ringing: false,
		call_waiting: true, dnd: false, mwi: false,
		mwi_new: 0, mwi_old: 0, calls: []
	};
}

let faulted = call_row(2, "OOS", true);
faulted.fault = true;

let call_rows = [ call_row(0, "IDLE", false), call_row(1, "IDLE", false),
                  faulted ];

// The hal table carries the attached shape with the rt table of a backend
// whose SCHED_FIFO request the cgroup refused and whose tick is unpinned, so
// the formatter's rt line and the schema's rt rule both run on this reply.
// The ten health counters carry ten different values, so the check on
// that block reads a transposed pair as a failure.
let hal = {
	state: "ready", backend: "stub", version: "0.0.0 canned", slic: "fake",
	lines: 3, streams_per_channel: 2, features: [], codecs: [],
	op_errors: 1, tdm_stalls: 2, completions_lost: 3,
	holds: 4, capture_drops: 5, trim_drops: 6, playback_expired: 7,
	hook_gaps: 8, hook_moved_gaps: 9, hook_gap_max_ms: 10,
	rt: { policy: "other", prio: 50, cpu: -1, sched_errno: 1,
	      mlock: true, net_policy: "other" }
};

let status = {
	version: "0.0.0",
	config_generation: 1,
	config: { loaded: true, enabled: true, channels: 3 },
	hal: hal,
	region: {
		requested: "USA", applied: "USA", source: "backend",
		iso3: "USA", describe: "Stub USA",
		cid_format: "bellcore-fsk", mwi_format: "bellcore-fsk",
		hook_flash_min_ms: 300, hook_flash_max_ms: 1300,
		hook_release_min_ms: 1301, cw_sas_total_ms: 20600,
		flash_service: "drop", pulse_invert: false,
		confirm_total_ms: 600, confirm_endless: false,
		ring: [ { template: 0, cadence: [ { on_ms: 2000, off_ms: 4000 } ] },
		        { template: 5, cadence: [ { on_ms: 500, off_ms: 500 },
		                                  { on_ms: 500, off_ms: 4000 } ] } ]
	},
	media: { opus_dtmf_fallbacks: 0 },
	signaling: { protocol: "sip", state: "running", available: [ "sip" ] },
	lines: [], call: call_rows,
	streams: [], jb: [], lec: [],
	monitor: [ sampled, stale, never ]
};

print(sprintf("%J", { status: status, stats: { stats: [] },
                      cdr: { cdr: [] } }));
EOF

if command -v ucode >/dev/null 2>&1; then
	uc_replies=$(ucode "$tmp/gen_replies.uc") && uc_canned_ready=1
fi

run_uc_canned() {
	uc_out=$(FAKE_REPLIES=$uc_replies UVOICE_UBUS_SOCKET=$ubus_socket \
		ucode -L "$uc_fakemod" "$cli_uc" -- "$@" 2>&1 </dev/null)
	uc_rc=$?
}

uc_canned_check() {
	if [ "$uc_canned_ready" = 1 ]; then
		check_has "$1" "$2" "$3"
	else
		skip "$1" "$uc_canned_skip"
	fi
}

if [ "$uc_canned_ready" = 1 ]; then
	run_uc_canned monitor
	uc_canned_monitor=$uc_out
	run_uc_canned line --line 1
	uc_canned_line=$uc_out
	run_uc_canned status
	uc_canned_status=$uc_out
fi

uc_canned_check 'cli/status names the running signaling protocol' \
	'protocol               sip' "$uc_canned_status"
uc_canned_check 'cli/status names the protocols of the image' \
	'available              sip' "$uc_canned_status"
uc_canned_check 'cli/status names the region source' \
	'source                 backend' "$uc_canned_status"
uc_canned_check 'cli/status names the region describe' \
	'describe               Stub USA' "$uc_canned_status"
uc_canned_check 'cli/status names the region decadic coding' \
	'pulse_invert           no' "$uc_canned_status"
uc_canned_check 'cli/status names the region confirm tone length' \
	'confirm_total_ms       600' "$uc_canned_status"
uc_canned_check 'cli/status names whether the confirm tone is endless' \
	'confirm_endless        no' "$uc_canned_status"
uc_canned_check 'cli/status renders a ring template as 1-based on/off pairs' \
	'ring r6                500/500 500/4000' "$uc_canned_status"
uc_canned_check 'cli/status renders the real-time outcome on one line' \
	'other prio 50 cpu -1 sched_errno 1 mlock yes net other' \
	"$uc_canned_status"

# The whole health block in one needle: the ten counters of struct
# hal_counters that status_hal_dump() writes, in its order, each against its
# own value, rendered as the two matrix groups of the status page.
uc_health_want=$(cat <<'EOF'
                             op_err tdm_stall cmpl_lost     holds cap_drops
  health                          1         2         3         4         5
                         trim_drops   pb_expd hook_gaps moved_gap gapmax_ms
                                  6         7         8         9        10
EOF
)
uc_canned_check 'cli/status renders the ten health counters in order' \
	"$uc_health_want" "$uc_canned_status"

# The columns carry the words of the Line monitor block of uvoice.line N, so
# the two views read as one vocabulary. The unit rides the value, because one
# row carries both the voltage and the current of the tip to ring pair.
uc_canned_check 'cli/monitor names the columns of the line view' \
	'Tip-ring   Battery   Target   Tip-ring      Long     HVIC' \
	"$uc_canned_monitor"
# The canned row carries -48250 mV of battery and -48000 mV of target. The
# values read in volts at one decimal, and a half at the last digit rounds
# to even.
uc_canned_check 'cli/monitor reads the battery and the target in volts' \
	'-48.2 V  -48.0 V' "$uc_canned_monitor"
# -25400 uA of loop current and 999 uA of longitudinal current: both read in
# milliamperes, the sub-milliampere one included.
uc_canned_check 'cli/monitor reads both currents in milliamperes' \
	'-25.4 mA    1.0 mA' "$uc_canned_monitor"
uc_canned_check 'cli/monitor ages a reading that names its instant' 's ago' \
	"$uc_canned_monitor"
# The fault word of the never row carries thermal and a bit this build does
# not name, which must still reach the operator.
uc_canned_check 'cli/monitor names an unnamed fault bit in raw hex' \
	'thermal, 0x100' "$uc_canned_monitor"
uc_canned_check 'cli/monitor ages a never row with a dash' \
	'thermal, 0x100       no   -' "$uc_canned_monitor"
uc_canned_check 'cli/line marks a stale monitor row' \
	'(stale: the last read answered no data)' "$uc_canned_line"
# A u32 at or past 2^31 arrives sign-extended and reads unsigned again. The
# block prints the converter word raw, so that word carries the check; the
# stamps print as ages, which the host uptime decides.
uc_canned_check 'cli/line reads a wrapped raw word unsigned' \
	'pd_dcdc 0x80100014' "$uc_canned_line"

# A daemon that names sip and runs no SIP table, the shape of an image with
# the MGCP unit alone: the account verbs and the SIP client status row key on
# the running state as well as the word. The formatter reads the canned reply;
# voice-cli reads the same status through the ubus shim.
uc_canned_sipdown_reg=''
uc_canned_sipdown_rc=''
cli_sipdown_out=''
if [ "$uc_canned_ready" = 1 ]; then
	uc_sipdown=$(printf '%s' "$uc_replies" | jq -c '.status.signaling =
		{protocol: "sip", state: "unavailable",
		 error: "the image does not carry this protocol",
		 available: ["mgcp"]}')
	uc_canned_sipdown_reg=$(FAKE_REPLIES=$uc_sipdown \
		UVOICE_UBUS_SOCKET=$ubus_socket ucode -L "$uc_fakemod" \
		"$cli_uc" -- register --line 0 2>&1 </dev/null)
	uc_canned_sipdown_rc=$?
	printf '%s' "$uc_sipdown" | jq '.status' > "$tmp/sipdown.json"
	ubus_status_file=$tmp/sipdown.json
	run_cli show status line 1
	cli_sipdown_out=$cli_out
	ubus_status_file=''
fi
if [ "$uc_canned_ready" = 1 ]; then
	sipdown_said=$(printf '%s\n' "$uc_canned_sipdown_reg" |
		grep -c 'the daemon reports sip unavailable')
	check_eq 'cli/register refuses a sip protocol that does not run' \
		'1 1' "$uc_canned_sipdown_rc $sipdown_said"
	# the hook row proves the view rendered, so the absence is not vacuous
	sipdown_hook=$(printf '%s\n' "$cli_sipdown_out" | grep -c 'Hook status')
	sipdown_row=$(printf '%s\n' "$cli_sipdown_out" |
		grep -c 'SIP client status')
	check_eq 'cli/voice-cli drops the SIP row of a sip that does not run' \
		'1 0' "$sipdown_hook $sipdown_row"
else
	skip 'cli/register refuses a sip protocol that does not run' \
		"$uc_canned_skip"
	skip 'cli/voice-cli drops the SIP row of a sip that does not run' \
		"$uc_canned_skip"
fi

# An MGCP daemon has no call legs, so the summary counts the connections of
# its endpoint rows.
uc_canned_mgcp_out=''
if [ "$uc_canned_ready" = 1 ]; then
	uc_mgcp=$(printf '%s' "$uc_replies" | jq -c '.status.signaling =
		{protocol: "mgcp", state: "running", available: ["sip", "mgcp"]}
		| .status.mgcp = {dialect: "rfc3435", socket: "bound"}
		| .status.call = []
		| .status.lines = [{line: 0, endpoint: "aaln/1@gw.test",
			service: "in_service", state: "connected",
			connections: [{id: "1", call_id: "A1", mode: "sendrecv",
				stream: 0}]}]')
	uc_canned_mgcp_out=$(FAKE_REPLIES=$uc_mgcp \
		UVOICE_UBUS_SOCKET=$ubus_socket ucode -L "$uc_fakemod" \
		"$cli_uc" -- status 2>&1 </dev/null)
fi
uc_canned_check 'cli/status counts the connections of an MGCP endpoint' \
	'call_legs              1' "$uc_canned_mgcp_out"

#
# The schema gate. schema/uvoice-api-schema.json is the machine readable
# contract of every ubus reply, and nothing else in the tree reads it, so a
# rule that leaves the schema goes unnoticed. schema_check.py validates one
# capture envelope {object, method, args, reply} against it. Two captures go
# through: the live status reply of the stub daemon, and the canned one the
# formatter leg above builds. Two mutated copies of the live capture must be
# refused, so the leg cannot pass with a schema or a validator that accepts
# everything. The section probes python3 and the jsonschema module once, the
# way the ucode module is probed, and skips whole when it cannot run.
#
schema_file=$uvoice_dir/schema/uvoice-api-schema.json
schema_tool=$here/schema_check.py
schema_ready=0
schema_skip='python3 jsonschema not available'

python3 -c 'import jsonschema' >/dev/null 2>&1 && schema_ready=1

schema_capture_build() {
	printf '%s' "$1" |
		jq '{object: "voice", method: "status", args: {}, reply: .}'
}

schema_run() {
	schema_out=$(printf '%s' "$1" | python3 "$schema_tool" "$schema_file" 2>&1)
	schema_rc=$?
}

schema_valid_check() {
	if [ "$schema_ready" != 1 ]; then
		skip "$1" "$schema_skip"
		return
	fi
	schema_run "$2"
	if [ "$schema_rc" = 0 ]; then
		ok "$1"
	else
		not_ok "$1" "the capture must validate, exit $schema_rc: $schema_out"
	fi
}

# The mutation runs inside the check, so that a jq program that fails, or one
# that answers nothing, is a failed check and not a refusal the validator
# would report for a capture it could not read at all.
schema_mutant_check() {
	if [ "$schema_ready" != 1 ]; then
		skip "$1" "$schema_skip"
		return
	fi
	schema_mutant=$(printf '%s' "$2" | jq "$3" 2>&1)
	schema_mutant_rc=$?
	if [ "$schema_mutant_rc" != 0 ] || [ -z "$schema_mutant" ]; then
		not_ok "$1" \
			"the mutation answered nothing at exit $schema_mutant_rc: $schema_mutant"
		return
	fi
	schema_run "$schema_mutant"
	if [ "$schema_rc" = 1 ]; then
		ok "$1"
	else
		not_ok "$1" "the capture must be refused, exit $schema_rc: $schema_out"
	fi
}

schema_live=$(schema_capture_build "$(ubus -t 5 call voice status 2>/dev/null)")
schema_valid_check 'schema/the live status capture validates' "$schema_live"

# The configuration reply carries its own required list, so an option added to
# cfg_dump() without its schema row fails here and nowhere else.
schema_config=$(ubus -t 5 call voice dump_config 2>/dev/null |
	jq '{object: "voice", method: "dump_config", args: {}, reply: .}')
schema_valid_check 'schema/the live dump_config capture validates' \
	"$schema_config"

# ucode builds the canned reply, so this row can miss either input. The schema
# reason wins, as it does in the rest of the leg, and the ucode one stands
# where the schema tooling is there and ucode is not.
schema_canned=''
if [ "$uc_canned_ready" = 1 ]; then
	schema_canned=$(printf '%s' "$uc_replies" |
		jq '{object: "voice", method: "status", args: {},
		     reply: .status}')
fi

if [ "$schema_ready" = 1 ] && [ "$uc_canned_ready" != 1 ]; then
	skip 'schema/the canned status reply validates' "$uc_canned_skip"
else
	schema_valid_check 'schema/the canned status reply validates' \
		"$schema_canned"
fi

# The self-test of the leg. A validator that answered valid whatever it read
# would pass both rows above, so one mutated copy of the live capture must be
# refused for a member the status reply always carries, and one for a rule the
# schema states alone: a call row's fault key rides beside oos true and
# nowhere else. Both mutations edit the capture the live row validated, so the
# refusal comes from the edit and from nothing else. Each one states its own
# prerequisite and stops the check where the capture cannot carry it: a row
# built out of nothing is refused for its missing members, which is a refusal
# the gate under test never made.
schema_monitor_mutation='
	if (.reply | has("monitor")) | not then
		error("the capture carries no monitor array to remove")
	else
		del(.reply.monitor)
	end'

schema_call_mutation='
	def members: ["line", "state", "oos", "ringing", "call_waiting",
		      "dnd", "mwi", "mwi_new", "mwi_old", "calls"];
	if (.reply.call | length) == 0 then
		error("the capture carries no call row to fault")
	elif ([.reply.call[0] | has(members[])] | all | not) then
		error("call row 0 lacks a member the schema requires")
	else
		.reply.call[0] |= (.oos = false | .fault = true)
	end'

schema_mutant_check 'schema/a capture that lost its monitor array is refused' \
	"$schema_live" "$schema_monitor_mutation"

schema_mutant_check 'schema/a call row faulting in service is refused' \
	"$schema_live" "$schema_call_mutation"

# addr_srv names a question, never an answer: the answer comes from SRV
# records or from an address, so DnsAnswerWord holds the other two words only.
schema_answer_mutation='
	if (.reply.lines | length) == 0 then
		error("the capture carries no line to give an answer word")
	else
		.reply.lines[0].dns_answer = "addr_srv"
	end'

schema_mutant_check 'schema/a dns_answer of addr_srv is refused' \
	"$schema_live" "$schema_answer_mutation"

# The rt table is optional under hal, so the live stub capture never carries
# it; the canned reply does, and these two mutations pin its own rule: every
# member is required once the table is there, and the policy is a word of
# RtPolicyWord.
schema_rt_member_mutation='
	if (.reply.hal | has("rt")) | not then
		error("the capture carries no rt table to cut")
	else
		del(.reply.hal.rt.net_policy)
	end'

schema_rt_word_mutation='
	if (.reply.hal | has("rt")) | not then
		error("the capture carries no rt table to edit")
	else
		.reply.hal.rt.policy = "batch"
	end'

if [ "$uc_canned_ready" = 1 ]; then
	schema_mutant_check 'schema/an rt table missing a member is refused' \
		"$schema_canned" "$schema_rt_member_mutation"
	schema_mutant_check 'schema/an rt table with a policy off the enum is refused' \
		"$schema_canned" "$schema_rt_word_mutation"
else
	skip 'schema/an rt table missing a member is refused' "$uc_canned_skip"
	skip 'schema/an rt table with a policy off the enum is refused' \
		"$uc_canned_skip"
fi

# An image built with the MGCP option alone carries no SIP table, so a config
# that names sip is unavailable and available[] names mgcp alone. The canned
# reply takes that shape, with the call rows cut to the core keys the null
# table leaves.
schema_empty_mutation='
	if (.reply | has("signaling")) | not then
		error("the capture carries no signaling table to rewrite")
	else
		.reply.signaling = {protocol: "sip", state: "unavailable",
			error: "the image does not carry this protocol",
			available: ["mgcp"]}
		| .reply.call |= map(with_entries(select(.key | IN("line",
			"oos", "fault", "ringing", "ring_held", "mwi",
			"mwi_new", "mwi_old", "fwd_disc_reversed",
			"fwd_disc_down", "loop_test"))))
	end'

schema_word_mutation='.reply.signaling.available = ["h323"]'

schema_empty=''
if [ "$uc_canned_ready" = 1 ]; then
	schema_empty=$(printf '%s' "$schema_canned" |
		jq "$schema_empty_mutation")
fi

if [ "$uc_canned_ready" = 1 ]; then
	schema_valid_check 'schema/an image without the sip table validates' \
		"$schema_empty"
	schema_mutant_check \
		'schema/an available word outside ProtocolWord is refused' \
		"$schema_empty" "$schema_word_mutation"
else
	skip 'schema/an image without the sip table validates' \
		"$uc_canned_skip"
	skip 'schema/an available word outside ProtocolWord is refused' \
		"$uc_canned_skip"
fi

#
# The manufacturing loop test (voice loop_test). The mode takes every line out
# of service, so the block arms, captures what an operator would read, and
# disarms again before anything else runs against this daemon. It never waits
# for a ring: the bell runs for 60 s in the backend and no check reads it.
#
looptest_envelope_build() {
	printf '%s' "$2" |
		jq --argjson enable "$1" \
		   '{object: "voice", method: "loop_test",
		     args: {enable: $enable}, reply: .}'
}

looptest_arm=$(ubus -t 5 call voice loop_test '{"enable":true}' 2>/dev/null)
check_has 'looptest/the arm answers the cross mode' '"mode": "cross"' \
	"$looptest_arm"
check_has 'looptest/the arm answers both lines' '"lines": 2' "$looptest_arm"
check_has 'looptest/the arm answers the 60 s window' '"ring_s": 60' \
	"$looptest_arm"
schema_valid_check 'schema/the loop test arm envelope validates' \
	"$(looptest_envelope_build true "$looptest_arm")"

looptest_status=$(schema_capture_build "$(ubus -t 5 call voice status 2>/dev/null)")
schema_valid_check 'schema/an armed status capture validates' \
	"$looptest_status"
check_eq 'looptest/every line reads out of service' 'true true' \
	"$(printf '%s' "$looptest_status" | jq -r '[.reply.call[].oos] | join(" ")')"
check_eq 'looptest/every line carries the loop test table' 'cross cross' \
	"$(printf '%s' "$looptest_status" |
		jq -r '[.reply.call[].loop_test.mode] | join(" ")')"

# The self-test of the leg above: the table refuses a row that lost its mode.
schema_looptest_mutation='
	if (.reply.call[0] | has("loop_test")) | not then
		error("the capture carries no loop_test table to cut")
	else
		del(.reply.call[0].loop_test.mode)
	end'
schema_mutant_check 'schema/a loop test table without its mode is refused' \
	"$looptest_status" "$schema_looptest_mutation"

looptest_stub=$(ubus -t 5 call voice.stub dump 2>/dev/null |
	jq '{object: "voice.stub", method: "dump", reply: .}')
schema_valid_check 'schema/an armed stub dump validates' "$looptest_stub"
check_eq 'looptest/the stub dump carries the three loop keys' '0 0 0' \
	"$(printf '%s' "$looptest_stub" |
		jq -r '.reply.line[0] | "\(.loop_mode) \(.loop_peer) \(.loop_delay_ms)"')"

# The disarm is the transition, so it runs against the armed board above and
# the service check reads what it left behind.
looptest_off=$(ubus -t 5 call voice loop_test '{"enable":false}' 2>/dev/null)
check_has 'looptest/the disarm answers the off mode' '"mode": "off"' \
	"$looptest_off"
schema_valid_check 'schema/the loop test disarm envelope validates' \
	"$(looptest_envelope_build false "$looptest_off")"
check_eq 'looptest/every line is back in service' 'false false' \
	"$(ubus -t 5 call voice status 2>/dev/null |
		jq -r '[.call[].oos] | join(" ")')"

# Both CLI entry points, once each, on the disarmed board they leave behind.
# The klish verb is 1-based and the formatter is 0-based; neither reads a line
# number here, so both drive the whole board.
run_cli looptest stop
check_eq 'looptest/the klish verb exits 0' 0 "$cli_rc"
check_has 'looptest/the klish verb prints the standing mode' 'enabled=false' \
	"$cli_out"

if [ "$uc_ready" = 1 ]; then
	run_uc looptest --start
	uc_check_eq 'looptest/the formatter arms and exits 0' 0 "$uc_rc"
	uc_check_has 'looptest/the formatter prints the armed mode' 'cross' \
		"$uc_out"
	run_uc looptest --stop
	uc_check_eq 'looptest/the formatter disarms and exits 0' 0 "$uc_rc"
	uc_check_has 'looptest/the formatter prints the off mode' 'off' "$uc_out"
	uc_check_eq 'looptest/the formatter left the lines in service' \
		'false false' \
		"$(ubus -t 5 call voice status 2>/dev/null |
			jq -r '[.call[].oos] | join(" ")')"

	# parse_args() reads a value that starts with "-" as no value at all,
	# so a negative number and a missing one arrive the same way. Both are
	# refused in front of the call, and the board stays disarmed, which is
	# what says no ubus call went out.
	run_uc looptest --start --ring-s -1
	uc_check_eq 'looptest/a negative window exits 1' 1 "$uc_rc"
	uc_check_has 'looptest/a negative window names the option' \
		'--ring-s needs a value' "$uc_out"
	run_uc looptest --start --delay-ms
	uc_check_eq 'looptest/a missing delay exits 1' 1 "$uc_rc"
	uc_check_has 'looptest/a missing delay names the option' \
		'--delay-ms needs a value' "$uc_out"
	uc_check_eq 'looptest/neither refusal armed the board' 'false' \
		"$(ubus -t 5 call voice status 2>/dev/null |
			jq -r '[.call[] | has("loop_test")] | any')"
else
	skip 'looptest/the formatter arms and exits 0' "$uc_skip"
	skip 'looptest/the formatter prints the armed mode' "$uc_skip"
	skip 'looptest/the formatter disarms and exits 0' "$uc_skip"
	skip 'looptest/the formatter prints the off mode' "$uc_skip"
	skip 'looptest/the formatter left the lines in service' "$uc_skip"
	skip 'looptest/a negative window exits 1' "$uc_skip"
	skip 'looptest/a negative window names the option' "$uc_skip"
	skip 'looptest/a missing delay exits 1' "$uc_skip"
	skip 'looptest/a missing delay names the option' "$uc_skip"
	skip 'looptest/neither refusal armed the board' "$uc_skip"
fi

ct_daemon_words=$({
	grep -o 'ct_refuse("[a-z_]*"' "$uvoice_dir/src/calltrace.c" |
		cut -d '"' -f 2
	sed -n '/^static const char \*ct_tool_missing(/,/^}/p' \
		"$uvoice_dir/src/calltrace.c" |
		grep -o 'return "[a-z_]*"' | cut -d '"' -f 2
	grep 'ct_refuse(err == -ENOSYS' "$uvoice_dir/src/calltrace.c" |
		grep -o '"[a-z_]*"' | tr -d '"'
} | sort -u)
ct_cli_words=$(sed -n '/^const CALLTRACE_ERRORS = {/,/^};/p' "$cli_uc" |
	sed -n 's/^[[:space:]]*\([a-z_]*\):.*/\1/p' | sort -u)
check_eq 'calltrace/refusal vocabulary matches the daemon' \
	"$ct_daemon_words" "$ct_cli_words"

# A canned sequence separates terminal results from a replacement trace.
calltrace_canned_tests() {
	ct_saved_replies=$uc_replies
	ct_base='{"id":"fixture","line":0,"options":{"timeout_s":10,
		"arm_timeout_s":20,"grace_s":5,"wire":true,"wire_max_mb":1,
		"rings_max_mb":1,"lec":true,"lec_max_mb":3},
		"wire":{"enabled":true,"ready":true,"running":true},
		"rings":{"registered":false,"relay_dropped":0}}'
	ct_keys='arm_timeout_s grace_s lec lec_max_mb rings_max_mb timeout_s'
	ct_keys="$ct_keys wire wire_max_mb"
	check_eq 'calltrace/fixture options carry all eight keys' "$ct_keys" \
		"$(printf '%s' "$ct_base" | jq -r '.options | keys | join(" ")')"
	for ct_state in idle refused armed running finishing "done" failed; do
		ct_reply=$(printf '%s' "$ct_base" | jq --arg state "$ct_state" '
			.state = $state |
			if $state == "idle" then {state:"idle"}
			elif $state == "refused" then {state:"idle",last_error:"busy"}
			elif $state == "armed" then .waiting_for = "call"
			elif $state == "running" or $state == "finishing" then
				.elapsed_ms = 2500
			elif $state == "done" then .reason = "call_ended"
			else .reason = "finisher_failed" |
				.last_error = "exit 3" | .failure = "exit 3"
			end')
		uc_replies=$(printf '%s' "$ct_reply" | jq '{status:.}')
		if [ "$uc_canned_ready" != 1 ]; then
			skip "calltrace/status $ct_state render" "$uc_canned_skip"
			skip "calltrace/status $ct_state exit" "$uc_canned_skip"
			continue
		fi
		run_uc_canned calltrace_status
		ct_word=$ct_state
		ct_rc=0
		case "$ct_state" in
		refused) ct_word=idle ;;
		failed) ct_rc=1 ;;
		esac
		check_has "calltrace/status $ct_state render" \
			"Call trace: $ct_word" "$uc_out"
		check_eq "calltrace/status $ct_state exit" "$ct_rc" "$uc_rc"
	done
	for ct_word in $ct_daemon_words; do
		if [ "$uc_canned_ready" != 1 ]; then
			skip "calltrace/refusal $ct_word" "$uc_canned_skip"
			continue
		fi
		uc_replies=$(jq -n --arg word "$ct_word" \
			'{arm:{__error:"Unknown error"},
			  status:{state:"idle",last_error:$word}}')
		run_uc_canned calltrace_arm --line 0 --no-wire
		check_has "calltrace/refusal $ct_word" "1 $ct_word:" \
			"$uc_rc $(printf '%s\n' "$uc_out" |
				sed -n 's/^  last_error  *//p')"
	done
	for ct_state in "done" failed cancelled deadline replaced; do
		if [ "$uc_canned_ready" != 1 ]; then
			skip "calltrace/wait $ct_state exit" "$uc_canned_skip"
			skip "calltrace/wait $ct_state result" "$uc_canned_skip"
			continue
		fi
		ct_deadline=5
		ct_rc=1
		ct_needle=$ct_state
		case "$ct_state" in
		done) ct_rc=0 ;;
		deadline) ct_rc=2; ct_deadline=0; ct_needle='wait deadline reached' ;;
		replaced) ct_needle='trace id changed from fixture to other' ;;
		esac
		uc_replies=$(printf '%s' "$ct_base" | jq --arg state "$ct_state" '
			(. + {state:"running",elapsed_ms:1000}) as $running |
			if $state == "deadline" then {status:$running}
			elif $state == "cancelled" then
				{status:[$running,{state:"idle",reason:"cancelled"}]}
			elif $state == "replaced" then
				{status:[$running,($running + {state:"done",id:"other"})]}
			elif $state == "failed" then
				{status:[$running,($running + {state:$state,
				  reason:"finisher_failed",last_error:"exit 3",
				  failure:"exit 3"})]}
			else {status:[$running,($running + {state:$state})]} end')
		run_uc_canned calltrace_wait --deadline "$ct_deadline"
		check_eq "calltrace/wait $ct_state exit" "$ct_rc" "$uc_rc"
		check_has "calltrace/wait $ct_state result" "$ct_needle" "$uc_out"
	done
	if [ "$uc_canned_ready" = 1 ]; then
		uc_replies=$(printf '%s' "$ct_base" | jq '{status:(. + {
			state:"running",elapsed_ms:2500,
			wire:{enabled:false,ready:false,truncated:true},
			rings:{registered:false,truncated:true,relay_dropped:7}})}')
		run_uc_canned calltrace_status
		check_has 'calltrace/status physical line and index' \
			'line                   1 (index 0)' "$uc_out"
		check_has 'calltrace/status wire readiness' \
			'wire.ready             no' "$uc_out"
		check_has 'calltrace/status ring registration' \
			'rings.registered       no' "$uc_out"
		ct_degraded='wire.enabled false (no wire capture), wire.truncated, '
		ct_degraded="$ct_degraded"'rings.truncated, relay_dropped 7'
		check_has 'calltrace/status degraded components' "$ct_degraded" "$uc_out"
		check_has 'calltrace/status elapsed and remaining' \
			'window remaining       7.500s' "$uc_out"
		check_has 'calltrace/status prints the lec option' \
			'options.lec            yes' "$uc_out"
		check_has 'calltrace/status prints the lec cap' \
			'options.lec_max_mb     3' "$uc_out"
		# A daemon older than the canceller capture sends neither key.
		uc_replies=$(printf '%s' "$ct_base" | jq '{status:((. + {
			state:"running",elapsed_ms:2500}) |
			del(.options.lec, .options.lec_max_mb))}')
		run_uc_canned calltrace_status
		check_has 'calltrace/status dashes an absent lec option' \
			'options.lec            -' "$uc_out"
		check_has 'calltrace/status dashes an absent lec cap' \
			'options.lec_max_mb     -' "$uc_out"
		# A refused arm over a failed trace writes its word over
		# last_error, so the view prints the refusal and the cause
		# apart.
		uc_replies=$(printf '%s' "$ct_base" | jq '{status:(. + {
			state:"failed",reason:"finisher_failed",
			last_error:"no_space",failure:"exit 143"})}')
		run_uc_canned calltrace_status
		check_has 'calltrace/status failed prints its failure' \
			'failure                exit 143' "$uc_out"
		check_has 'calltrace/status failed keeps the refusal word' \
			'last_error             no_space:' "$uc_out"
		uc_replies='{"arm":{"__echo":true}}'
		run_uc_canned calltrace_arm --line 0 --timeout 8 --grace 2 \
			--arm-timeout 15 --no-wire --wire-max-mb 1 --rings-max-mb 2 --json
		check_eq 'calltrace/arm maps every option and keeps index zero' \
			'0 0 8 2 15 false 1 2 null null' \
			"$uc_rc $(printf '%s' "$uc_out" | jq -r '
				[.line,.timeout,.grace,.arm_timeout,.wire,
				 .wire_max_mb,.rings_max_mb,.lec,.lec_max_mb] |
				map(tostring) | join(" ")')"
		run_uc_canned calltrace_arm --line 0 --lec --lec-max-mb 4 --json
		check_eq 'calltrace/arm maps --lec and --lec-max-mb' \
			'0 true 4' "$uc_rc $(printf '%s' "$uc_out" | jq -r '
				[.lec,.lec_max_mb] | map(tostring) |
				join(" ")')"
		uc_replies='{"status":[{"state":"running","id":"fixture"},
			{"state":"done","id":"fixture"}]}'
		run_uc_canned calltrace_wait --json --id fixture --deadline 5
		check_eq 'calltrace/wait json prints only the last reply' '0 done' \
			"$uc_rc $(printf '%s' "$uc_out" | jq -r .state)"
		# The default deadline of the wait is the arm timeout plus the
		# window plus 120 s: 122 s with this fixture. A finishing reply
		# that names a finisher budget of 600 s moves it out, so the
		# wait reads the archive at its poll of 131 s. An explicit
		# deadline stays as the caller set it.
		uc_replies=$(printf '%s' "$ct_base" | jq '
			(.options.arm_timeout_s = 1 |
			 .options.timeout_s = 1) as $b |
			($b + {state:"finishing",elapsed_ms:1000,
			  finish_budget_ms:600000}) as $fin |
			{status:([range(131) | $fin] +
			  [$b + {state:"done",reason:"call_ended"}])}')
		FAKE_CLOCK=1
		export FAKE_CLOCK
		run_uc_canned calltrace_wait
		check_eq 'calltrace/wait covers the finisher budget' 0 "$uc_rc"
		run_uc_canned calltrace_wait --deadline 5
		check_eq 'calltrace/wait keeps an explicit deadline' 2 "$uc_rc"
		unset FAKE_CLOCK
	else
		for ct_name in 'physical line and index' 'wire readiness' \
			'ring registration' 'degraded components' \
			'elapsed and remaining' 'prints the lec option' \
			'prints the lec cap' 'dashes an absent lec option' \
			'dashes an absent lec cap' 'failed prints its failure' \
			'failed keeps the refusal word'; do
			skip "calltrace/status $ct_name" "$uc_canned_skip"
		done
		skip 'calltrace/arm maps every option and keeps index zero' "$uc_canned_skip"
		skip 'calltrace/arm maps --lec and --lec-max-mb' \
			"$uc_canned_skip"
		skip 'calltrace/wait json prints only the last reply' "$uc_canned_skip"
		skip 'calltrace/wait covers the finisher budget' \
			"$uc_canned_skip"
		skip 'calltrace/wait keeps an explicit deadline' \
			"$uc_canned_skip"
	fi
	uc_replies=$ct_saved_replies
}
calltrace_canned_tests

ct_live_archive=''
ct_live_id=''

calltrace_live_tests() {
	ct_skip=''
	[ "$uc_ready" = 1 ] || ct_skip=$uc_skip
	command -v tar >/dev/null 2>&1 || ct_skip='no tar command'
	command -v jq >/dev/null 2>&1 || ct_skip='no jq command'
	if [ -n "$ct_skip" ]; then
		for ct_name in arm 'arm index' seize running release wait layout \
			manifest 'manifest names the schema version' \
			collectors 'tls off' 'keylog copied' 'keylog once' \
			'media declared' privacy omissions 'archive size' schema \
			'summary report' 'summary generator' 'summary collector' \
			'summary no diagnostics' 'summary trace id' \
			'voice-cli status' 'voice-cli wait exit' 'voice-cli wait output' \
			'voice-cli line mapping' 'voice-cli cancel'; do
			skip "calltrace/live $ct_name" "$ct_skip"
		done
		return
	fi
	printf 'fixture stale key log\n' > /tmp/uvoice/ssl.keylog
	check_eq 'calltrace/live tls off' false \
		"$(ubus -t 5 call voice dump_config | jq -r .tls_keylog_enabled)"
	run_uc calltrace_arm --line 0 --no-wire --timeout 10 \
		--arm-timeout 10 --wire-max-mb 1 --rings-max-mb 1 --json
	check_eq 'calltrace/live arm' '0 armed' \
		"$uc_rc $(printf '%s' "$uc_out" | jq -r .state)"
	check_eq 'calltrace/live arm index' 0 "$(printf '%s' "$uc_out" | jq -r .line)"
	ct_id=$(printf '%s' "$uc_out" | jq -r .id)
	ubus -t 5 call voice.stub hook '{"line":0,"state":"seize"}' >/dev/null
	check_eq 'calltrace/live seize' 0 "$?"
	ct_end=$(( $(date +%s) + 5 ))
	while [ "$(date +%s)" -lt "$ct_end" ]; do
		ct_status=$(ubus -t 5 call voice.calltrace status)
		[ "$(printf '%s' "$ct_status" | jq -r .state)" = running ] && break
		sleep 0.2
	done
	check_eq 'calltrace/live running' running \
		"$(printf '%s' "$ct_status" | jq -r .state)"
	ubus -t 5 call voice.stub hook '{"line":0,"state":"release"}' >/dev/null
	check_eq 'calltrace/live release' 0 "$?"
	run_uc calltrace_wait --id "$ct_id" --deadline 30 --json
	check_eq 'calltrace/live wait' '0 done' \
		"$uc_rc $(printf '%s' "$uc_out" | jq -r .state)"
	ct_status=$uc_out
	ct_archive=$(printf '%s' "$ct_status" | jq -r '.archive // ""')
	ct_live_archive=$ct_archive
	ct_live_id=$ct_id
	ct_layout=$(tar -tzf "$ct_archive" 2>&1)
	ct_layout_rc=$?
	ct_layout_ok=1
	for ct_file in README.txt manifest.json rings.pcapng snapshots.jsonl \
		config.json system/ SUMMARY.txt tools/calltrace-summary.uc; do
		printf '%s\n' "$ct_layout" | grep -Fx "$ct_id/$ct_file" >/dev/null ||
			ct_layout_ok=0
	done
	check_eq 'calltrace/live layout' '0 1' "$ct_layout_rc $ct_layout_ok"
	ct_manifest=$(tar -xzOf "$ct_archive" "$ct_id/manifest.json" 2>/dev/null)
	check_eq 'calltrace/live manifest' 'done' \
		"$(printf '%s' "$ct_manifest" | jq -r .stage)"
	# cmake reads the version the manifest names from the schema document
	# itself, so the two must agree.
	check_eq 'calltrace/live manifest names the schema version' \
		"$(jq -r .version "$schema_file")" \
		"$(printf '%s' "$ct_manifest" | jq -r .versions.schema)"
	check_eq 'calltrace/live collectors' 18 \
		"$(printf '%s' "$ct_manifest" | jq '.collectors | length')"
	# The report rides the archive with the generator that wrote it, and the
	# collectors table carries its own result.
	ct_summary=$(tar -xzOf "$ct_archive" "$ct_id/SUMMARY.txt" 2>/dev/null)
	check_has 'calltrace/live summary report' 'HOW TO READ' "$ct_summary"
	check_has 'calltrace/live summary generator' 'calltrace-summary.uc' \
		"$(tar -xzOf "$ct_archive" "$ct_id/tools/calltrace-summary.uc" \
			2>/dev/null | head -n 20)"
	check_eq 'calltrace/live summary collector' ok \
		"$(printf '%s' "$ct_manifest" | jq -r '.collectors.summary')"
	# A generator that says nothing leaves no file behind.
	check_eq 'calltrace/live summary no diagnostics' 0 \
		"$(printf '%s\n' "$ct_layout" | grep -c "^$ct_id/system/summary$")"
	check_eq 'calltrace/live summary trace id' "$ct_id" \
		"$(printf '%s\n' "$ct_summary" | sed -n 's/^  id  *//p')"
	check_eq 'calltrace/live keylog copied' 'fixture stale key log' \
		"$(tar -xzOf "$ct_archive" "$ct_id/system/ssl.keylog")"
	check_eq 'calltrace/live keylog once' 1 \
		"$(printf '%s' "$ct_manifest" |
			jq '[.contains[] | select(. == "tls_keylog")] | length')"
	check_eq 'calltrace/live media declared' 1 \
		"$(printf '%s' "$ct_manifest" |
			jq '[.contains[] | select(. == "rtp_plaintext")] | length')"
	rm /tmp/uvoice/ssl.keylog
	run_uc calltrace_status
	check_has 'calltrace/live privacy' \
		'handle this archive like a packet capture' "$uc_out"
	check_has 'calltrace/live omissions' 'platform: omitted' "$uc_out"
	check_has 'calltrace/live archive size' 'B' \
		"$(printf '%s\n' "$uc_out" | grep '^  size ')"
	ct_capture=$(printf '%s' "$ct_status" |
		jq '{object:"voice.calltrace",method:"status",reply:.}')
	schema_valid_check 'calltrace/live schema' "$ct_capture"
	run_cli calltrace status
	check_has 'calltrace/live voice-cli status' 'line 1 (index 0)' "$cli_out"
	run_cli_uc calltrace wait
	check_eq 'calltrace/live voice-cli wait exit' 0 "$cli_rc"
	check_has 'calltrace/live voice-cli wait output' \
		'Call trace: done' "$cli_out"
	# The wrapper still sends an index even if tcpdump preflight refuses.
	: > "$ubus_rec"
	run_cli calltrace arm 1 10
	check_has 'calltrace/live voice-cli line mapping' \
		'call voice.calltrace arm {"line":0,"timeout":10}' "$(cat "$ubus_rec")"
	run_uc calltrace_arm --line 0 --no-wire --wire-max-mb 1 --rings-max-mb 1
	run_cli calltrace cancel
	check_has 'calltrace/live voice-cli cancel' 'cancelled' "$cli_out"
}
calltrace_live_tests

# The canceller option against the stub daemon. The stub backend has no
# canceller capture, so the ring opens empty; the reply still carries the
# option, its cap and the counts of the ring, and the schema requires all
# three. The mutation is the self-test of the ring row.
calltrace_lec_live_tests() {
	ct_skip=''
	[ "$uc_ready" = 1 ] || ct_skip=$uc_skip
	command -v jq >/dev/null 2>&1 || ct_skip='no jq command'
	if [ -n "$ct_skip" ]; then
		for ct_name in 'lec arm' 'lec options' 'lec ring counts' \
			'lec schema' \
			'lec schema refuses a reply without the ring' \
			'lec status view'; do
			skip "calltrace/live $ct_name" "$ct_skip"
		done
		return
	fi
	run_uc calltrace_arm --line 0 --no-wire --wire-max-mb 1 \
		--rings-max-mb 1 --lec --lec-max-mb 1 --json
	check_eq 'calltrace/live lec arm' '0 armed' \
		"$uc_rc $(printf '%s' "$uc_out" | jq -r .state)"
	check_eq 'calltrace/live lec options' 'true 1' \
		"$(printf '%s' "$uc_out" |
			jq -r '[.options.lec,.options.lec_max_mb] |
				map(tostring) | join(" ")')"
	check_eq 'calltrace/live lec ring counts' '0 0 0 0' \
		"$(printf '%s' "$uc_out" |
			jq -r '[.rings.lec.written,.rings.lec.exported,
				.rings.lec.overwritten,.rings.lec.truncated] |
				map(tostring) | join(" ")')"
	ct_capture=$(printf '%s' "$uc_out" |
		jq '{object:"voice.calltrace",method:"arm",
		     args:{line:0,wire:false,wire_max_mb:1,rings_max_mb:1,
			   lec:true,lec_max_mb:1},reply:.}')
	schema_valid_check 'calltrace/live lec schema' "$ct_capture"
	schema_mutant_check \
		'calltrace/live lec schema refuses a reply without the ring' \
		"$ct_capture" 'del(.reply.rings.lec)'
	run_uc calltrace_status
	check_has 'calltrace/live lec status view' \
		'options.lec            yes' "$uc_out"
	run_uc calltrace_cancel
}
calltrace_lec_live_tests

calltrace_fixture_tests() {
	ct_skip=''
	command -v tar >/dev/null 2>&1 || ct_skip='no tar command'
	command -v jq >/dev/null 2>&1 || ct_skip='no jq command'
	if [ -n "$ct_skip" ]; then
		for ct_name in exit archive stage collectors 'proc status' \
			'omitted serial' 'no keylog entry' 'kept notes' \
			'removed directory' 'no part file'; do
			skip "calltrace/finisher $ct_name" "$ct_skip"
		done
		return
	fi
	ct_dir=$tmp/finish-fixture
	mkdir -p "$ct_dir"
	printf '{"stage":"finishing","times":{"armed_at":%s},"notes":["fixture"]}\n' \
		"$(date +%s)" > "$ct_dir/manifest.json"
	printf '{}\n' > "$ct_dir/config.json"
	printf '{}\n' > "$ct_dir/snapshots.jsonl"
	: > "$ct_dir/rings.pcapng"
	"$uvoice_dir/files/calltrace-finish.sh" "$ct_dir" "$ct_dir.tar.gz" all \
		"$$" > "$tmp/finish.out" 2>&1
	check_eq 'calltrace/finisher exit' 0 "$?"
	tar -tzf "$ct_dir.tar.gz" > "$tmp/finish.layout" 2>&1
	check_eq 'calltrace/finisher archive' 0 "$?"
	ct_manifest=$(tar -xzOf "$ct_dir.tar.gz" finish-fixture/manifest.json)
	check_eq 'calltrace/finisher stage' 'done' \
		"$(printf '%s' "$ct_manifest" | jq -r .stage)"
	check_eq 'calltrace/finisher collectors' 18 \
		"$(printf '%s' "$ct_manifest" | jq '.collectors | length')"
	check_eq 'calltrace/finisher proc status' ok \
		"$(printf '%s' "$ct_manifest" | jq -r '.collectors["daemon-status"]')"
	check_eq 'calltrace/finisher omitted serial' false \
		"$(printf '%s' "$ct_manifest" | jq '.board | has("serial")')"
	check_eq 'calltrace/finisher no keylog entry' 0 \
		"$(printf '%s' "$ct_manifest" |
			jq '[(.contains // [])[] | select(. == "tls_keylog")] | length')"
	check_eq 'calltrace/finisher kept notes' fixture \
		"$(printf '%s' "$ct_manifest" | jq -r '.notes[0]')"
	[ ! -e "$ct_dir" ]
	check_eq 'calltrace/finisher removed directory' 0 "$?"
	[ ! -e "$ct_dir.tar.gz.part" ]
	check_eq 'calltrace/finisher no part file' 0 "$?"
}
calltrace_fixture_tests

calltrace_failure_tests() {
	ct_skip=''
	for ct_tool in tar jq awk; do
		command -v "$ct_tool" >/dev/null 2>&1 || ct_skip="no $ct_tool command"
	done
	if [ -n "$ct_skip" ]; then
		for ct_name in 'collector failures preserve archive' 'command exit 125' \
			'pipeline failure' 'platform failure' 'archive failure exit' \
			'archive failure keeps directory' \
			'jq fallback archive' 'jq fallback journal' 'jq fallback collectors' \
			'jq fallback note' 'retry drops stale collector notes' \
			'log window' 'nft chain filter'; do
			skip "calltrace/finisher $ct_name" "$ct_skip"
		done
		return
	fi
	mkdir -p "$tmp/ct-tools" "$tmp/finish-failure"
	cat > "$tmp/ct-tools/uptime" <<'EOF'
#!/bin/sh
exit 125
EOF
	cat > "$tmp/ct-tools/dmesg" <<'EOF'
#!/bin/sh
printf 'partial kernel log\n'
exit 7
EOF
	cat > "$tmp/ct-tools/logread" <<'EOF'
#!/bin/sh
if [ "$1" = --help ]; then
	printf '%s\n' '--since TIME --until TIME (YYYY-MM-DD HH:MM:SS)'
else
	printf '%s\n' "$*"
fi
EOF
	cat > "$tmp/ct-tools/nft" <<'EOF'
#!/bin/sh
cat <<'RULES'
table inet filter {
	chain ordinary {
		tcp dport 22 accept
		udp dport 5060 accept comment "!fw4: Allow_VoIP_SIP"
	}
	chain uvoice-input {
		udp dport { 5060, 5061 } accept
	}
}
RULES
EOF
	chmod +x "$tmp/ct-tools/uptime" "$tmp/ct-tools/dmesg" \
		"$tmp/ct-tools/logread" "$tmp/ct-tools/nft"
	cat > /usr/srg/scripts/platformdb <<'EOF'
#!/bin/sh
exit 7
EOF
	ct_dir=$tmp/finish-failure
	printf '%s\n' '{"stage":"finishing", "times":
		{"armed_at":1700000000,"ended_at":1700000010}}' \
		> "$ct_dir/manifest.json"
	TZ=UTC0 PATH=$tmp/ct-tools:$PATH "$uvoice_dir/files/calltrace-finish.sh" \
		"$ct_dir" "$ct_dir.tar.gz" all "$$" > "$tmp/failure.out" 2>&1
	check_eq 'calltrace/finisher collector failures preserve archive' 0 "$?"
	rm /usr/srg/scripts/platformdb
	ct_manifest=$(tar -xzOf "$ct_dir.tar.gz" finish-failure/manifest.json)
	check_eq 'calltrace/finisher command exit 125' 'failed 125' \
		"$(printf '%s' "$ct_manifest" | jq -r .collectors.uptime)"
	check_eq 'calltrace/finisher pipeline failure' 'failed 7' \
		"$(printf '%s' "$ct_manifest" | jq -r .collectors.dmesg)"
	check_eq 'calltrace/finisher platform failure' 'failed 7' \
		"$(printf '%s' "$ct_manifest" | jq -r .collectors.platform)"
	check_eq 'calltrace/finisher log window' \
		'-S 2023-11-14 22:12:50 -U 2023-11-14 22:14:00 --iso8601 -t' \
		"$(tar -xzOf "$ct_dir.tar.gz" finish-failure/system/logread)"
	ct_nft=$(tar -xzOf "$ct_dir.tar.gz" finish-failure/system/nft-voip)
	check_has 'calltrace/finisher nft chain filter' \
		'chain uvoice-input {' "$ct_nft"
	check_has 'calltrace/finisher nft fw4 rule filter' \
		'Allow_VoIP_SIP' "$ct_nft"
	check_lacks 'calltrace/finisher nft plain rule dropped' \
		'dport 22' "$ct_nft"

	mkdir -p "$ct_dir"
	printf '{"stage":"finishing","times":{"armed_at":1700000000}}\n' \
		> "$ct_dir/manifest.json"
	"$uvoice_dir/files/calltrace-finish.sh" "$ct_dir" \
		"$tmp/no-parent/archive.tar.gz" all "$$" > "$tmp/archive-failure.out" 2>&1
	check_eq 'calltrace/finisher archive failure exit' 1 "$?"
	ct_kept=no
	[ -d "$ct_dir" ] && ct_kept=yes
	check_eq 'calltrace/finisher archive failure keeps directory' yes "$ct_kept"

	mkdir -p "$ct_dir/.finish/nft-voip"
	printf 'stale collector note\n' > "$ct_dir/.finish/nft-voip/note"

	# A private PATH removes jq without changing the host installation.
	mkdir -p "$tmp/no-jq"
	for ct_tool in cat mkdir head tail awk grep date cp rm mv tar gzip \
		dirname basename timeout nice tr wc; do
		ct_path=$(command -v "$ct_tool")
		[ -n "$ct_path" ] && ln -s "$ct_path" "$tmp/no-jq/$ct_tool"
	done
	printf '{"stage":"finishing","notes":["original"]}\n' > "$ct_dir/manifest.json"
	PATH=$tmp/no-jq "$uvoice_dir/files/calltrace-finish.sh" \
		"$ct_dir" "$tmp/fallback.tar.gz" all "$$" \
		> "$tmp/fallback.out" 2>&1
	check_eq 'calltrace/finisher jq fallback archive' 0 "$?"
	ct_journal=$(tar -xzOf "$tmp/fallback.tar.gz" finish-failure/manifest.json)
	check_eq 'calltrace/finisher jq fallback journal' finishing \
		"$(printf '%s' "$ct_journal" | jq -r .stage)"
	ct_manifest=$(tar -xzOf "$tmp/fallback.tar.gz" \
		finish-failure/manifest-finisher.json)
	check_eq 'calltrace/finisher jq fallback collectors' 'done 18' \
		"$(printf '%s' "$ct_manifest" | jq -r '"\(.stage) \(.collectors|length)"')"
	check_has 'calltrace/finisher jq fallback note' 'jq is absent' \
		"$(printf '%s' "$ct_manifest" | jq -r '.notes | join(" ")')"
	check_lacks 'calltrace/finisher retry drops stale collector notes' \
		'stale collector note' "$ct_manifest"
}
calltrace_failure_tests

calltrace_render_tests() {
	ct_render_checks='status keeps a prior refusal
cancel keeps a prior refusal
wait prints one progress line per poll
wait prints one final block
deadline prints no final block
status omits the duplicate stage row
privacy waits for an archive
privacy accompanies an archive
wire exit is degraded
stopped wire is degraded before completion
finishing permits the stopped wire
finishing retains the wire exit
stopped wire is not degraded after completion
status uses the shared color layer
NO_COLOR keeps status plain
json stays plain with color enabled'
	if [ "$uc_canned_ready" != 1 ]; then
		while IFS= read -r ct_name; do
			skip "calltrace/$ct_name" "$uc_canned_skip"
		done <<EOF
$ct_render_checks
EOF
		return
	fi
	ct_saved_replies=$uc_replies
	uc_replies=$(printf '%s' "$ct_base" |
		jq '{status:(. + {state:"running",last_error:"busy"})}')
	run_uc_canned calltrace_status
	check_eq 'calltrace/status keeps a prior refusal' 0 "$uc_rc"
	uc_replies='{"cancel":{"state":"idle","reason":"cancelled",
		"last_error":"busy"}}'
	run_uc_canned calltrace_cancel
	check_eq 'calltrace/cancel keeps a prior refusal' 0 "$uc_rc"
	uc_replies=$(printf '%s' "$ct_base" | jq '{status:[
		(. + {state:"armed",waiting_for:"call"}),
		(. + {state:"running",elapsed_ms:1000}),
		(. + {state:"done",reason:"call_ended"})]}')
	run_uc_canned calltrace_wait --deadline 5
	check_eq 'calltrace/wait prints one progress line per poll' 2 \
		"$(printf '%s\n' "$uc_out" | grep -c ', elapsed .*remaining ')"
	check_eq 'calltrace/wait prints one final block' '0 1' \
		"$uc_rc $(printf '%s\n' "$uc_out" | grep -c '^  id ')"
	check_eq 'calltrace/status omits the duplicate stage row' 0 \
		"$(printf '%s\n' "$uc_out" | grep -c '^  stage ')"
	uc_replies=$(printf '%s' "$ct_base" |
		jq '{status:(. + {state:"running",elapsed_ms:1000})}')
	run_uc_canned calltrace_wait --deadline 0
	check_eq 'calltrace/deadline prints no final block' '2 0' \
		"$uc_rc $(printf '%s\n' "$uc_out" | grep -c '^  id ')"
	run_uc_canned calltrace_status
	check_lacks 'calltrace/privacy waits for an archive' 'Privacy:' "$uc_out"
	uc_replies=$(printf '%s' "$ct_base" | jq '{status:(. + {
		state:"done",archive:"/tmp/uvoice/no-such-archive.tar.gz",
		reason:"call_ended"})}')
	run_uc_canned calltrace_status
	check_has 'calltrace/privacy accompanies an archive' 'Privacy:' "$uc_out"
	uc_replies=$(printf '%s' "$ct_base" | jq '{status:(. + {
		state:"running",wire:{enabled:true,ready:true,running:false,exit:7}})}')
	run_uc_canned calltrace_status
	check_has 'calltrace/wire exit is degraded' 'wire.exit 7' "$uc_out"
	check_has 'calltrace/stopped wire is degraded before completion' \
		'wire stopped before the trace ended' "$uc_out"
	uc_replies=$(printf '%s' "$uc_replies" | jq '.status.state = "finishing"')
	run_uc_canned calltrace_status
	check_lacks 'calltrace/finishing permits the stopped wire' \
		'wire stopped before the trace ended' "$uc_out"
	check_has 'calltrace/finishing retains the wire exit' 'wire.exit 7' "$uc_out"
	uc_replies=$(printf '%s' "$uc_replies" | jq '.status.state = "done"')
	run_uc_canned calltrace_status
	check_lacks 'calltrace/stopped wire is not degraded after completion' \
		'wire stopped before the trace ended' "$uc_out"
	ct_moddir_saved=$uc_moddir
	uc_moddir="$uc_fakemod"
	FAKE_REPLIES="$uc_replies" run_uc_color calltrace_status
	check_has 'calltrace/status uses the shared color layer' "$uc_esc" "$uc_out"
	FAKE_REPLIES="$uc_replies" run_uc_color calltrace_status --json
	ct_json_state=$(printf '%s' "$uc_out" | jq -r .state 2>&1)
	ct_json_rc=$?
	check_eq 'calltrace/json stays plain with color enabled' '0 0 done' \
		"$uc_rc $ct_json_rc $ct_json_state"
	uc_moddir="$ct_moddir_saved"
	NO_COLOR=1 UVOICE_COLOR=1 run_uc_canned calltrace_status
	check_lacks 'calltrace/NO_COLOR keeps status plain' "$uc_esc" "$uc_out"
	uc_replies=$ct_saved_replies
}
calltrace_render_tests

ct_finish_fixture_make() {
	ct_dir=$tmp/$1
	ct_archive=$ct_dir.tar.gz
	mkdir -p "$ct_dir"
	printf '%s\n' '{"stage":"finishing","line":0,
		"times":{"armed_at":1700000000},"notes":["fixture"]}' \
		> "$ct_dir/manifest.json"
	printf '%s\n' '{"channel":[
		{"id":0,"rtp_local_port_min":20000,"rtp_local_port_max":20010},
		{"id":1,"rtp_local_port_min":10000,"rtp_local_port_max":10030}]}' \
		> "$ct_dir/config.json"
}

ct_finish_fixture_run() {
	: > "$ct_nice_rec"
	: > "$ct_timeout_rec"
	ct_tar_mode=${1:-normal} ct_timeout_style=${2:-normal} \
		PATH=$ct_finish_bin:$PATH \
		"$uvoice_dir/files/calltrace-finish.sh" \
		"$ct_dir" "$ct_archive" all "$$" > "$tmp/finish-fixture.out" 2>&1
	ct_finish_rc=$?
	ct_stem=${ct_dir##*/}
	ct_manifest=$(tar -xzOf "$ct_archive" "$ct_stem/manifest.json" 2>/dev/null)
}

ct_finish_tools_make() {
	ct_finish_bin=$tmp/finish-tools
	ct_tar_real=$(command -v tar)
	ct_timeout_real=$(command -v timeout)
	ct_nice_real=$(command -v nice)
	ct_busybox_real=$(command -v busybox)
	ct_nice_rec=$tmp/nice.argv
	ct_timeout_rec=$tmp/timeout.argv
	ct_fifo_root=$tmp/collector-sync
	export ct_tar_real ct_timeout_real ct_nice_real ct_nice_rec ct_fifo_root
	export ct_busybox_real ct_timeout_rec
	mkdir -p "$ct_finish_bin" "$ct_fifo_root"
	mkfifo "$ct_fifo_root/ready" "$ct_fifo_root/release" "$ct_fifo_root/done"
	cat > "$ct_finish_bin/uptime" <<'EOF'
#!/bin/sh
if [ "$ct_timeout_style" != 143 ]; then
	printf 'uptime fixture\n'
	exit 0
fi
printf 'frozen collector output\n'
printf 'ready\n' > "$ct_fifo_root/ready"
read -r token < "$ct_fifo_root/release"
printf 'late collector output\n'
printf 'done\n' > "$ct_fifo_root/done"
EOF
	cat > "$ct_finish_bin/timeout" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$ct_timeout_rec"
case " $* " in
*' --collect uptime '*)
	case "$ct_timeout_style" in
	143)
		shift
		"$ct_busybox_real" timeout 1 "$@" &
		worker=$!
		read -r token < "$ct_fifo_root/ready"
		wait "$worker" 2>/dev/null
		exit $?
		;;
	124) printf 'timed out\n'; exit 124 ;;
	esac
	;;
esac
exec "$ct_timeout_real" "$@"
EOF
	cat > "$ct_finish_bin/nice" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$ct_nice_rec"
exec "$ct_nice_real" "$@"
EOF
	cat > "$ct_finish_bin/tar" <<'EOF'
#!/bin/sh
if [ "$ct_timeout_style" = 143 ]; then
	printf 'release\n' > "$ct_fifo_root/release"
	read -r token < "$ct_fifo_root/done"
fi
case " $* " in
*' --warning=no-file-changed '*) ;;
*) printf 'missing tar warning option\n' >&2; exit 42 ;;
esac
if [ "$ct_tar_mode" = fail ] || [ "$ct_tar_mode" = term ]; then
	want_path=0
	for arg do
		if [ "$want_path" = 1 ]; then
			printf 'partial archive\n' > "$arg"
			break
		fi
		[ "$arg" = -zcf ] && want_path=1
	done
	if [ "$ct_tar_mode" = term ]; then
		kill -TERM "$PPID"
		exit 0
	fi
	exit 2
fi
exec "$ct_tar_real" "$@"
EOF
	cat > "$ct_finish_bin/conntrack" <<'EOF'
#!/bin/sh
cat <<'ROWS'
udp src=192.0.2.1 sport=53 dst=192.0.2.2 dport=53 dns
udp src=192.0.2.1 sport=20000 dst=192.0.2.2 dport=40000 custom-low
udp src=192.0.2.1 sport=40000 dst=192.0.2.2 dport=20010 custom-high
udp src=192.0.2.1 sport=19999 dst=192.0.2.2 dport=20011 outside
udp src=192.0.2.1 sport=10000 dst=192.0.2.2 dport=50000 default-low
udp src=192.0.2.1 sport=50000 dst=192.0.2.2 dport=10030 default-high
udp src=192.0.2.1 sport=10031 dst=192.0.2.2 dport=9999 outside-default
ROWS
EOF
	cat > "$ct_finish_bin/nft" <<'EOF'
#!/bin/sh
awk 'BEGIN { for (i = 1; i <= 2005; i++) print "unrelated rule", i }'
EOF
	cat > "$ct_finish_bin/dmesg" <<'EOF'
#!/bin/sh
awk 'BEGIN { for (i = 1; i <= 600; i++) print "kernel row", i }'
EOF
	chmod +x "$ct_finish_bin/uptime" "$ct_finish_bin/timeout" \
		"$ct_finish_bin/nice" "$ct_finish_bin/tar" \
		"$ct_finish_bin/conntrack" "$ct_finish_bin/nft" "$ct_finish_bin/dmesg"
}

ct_wire_fixture_start() {
	ct_capture=$ct_dir/wire.pcap
	[ "$1" = unrelated ] && ct_capture=$tmp/unrelated.pcap
	mkfifo "$ct_dir/ready" "$ct_dir/hold"
	cat > "$ct_dir/writer" <<'EOF'
#!/bin/sh
if [ "$4" = matching ]; then
	trap '' TERM
else
	trap 'exit 0' TERM
fi
printf 'wire fixture\n' > "$1"
exec 3<> "$3"
printf 'ready\n' > "$2"
read -r token <&3
EOF
	sh "$ct_dir/writer" "$ct_capture" "$ct_dir/ready" "$ct_dir/hold" "$1" &
	ct_wire_pid=$!
	read -r ct_token < "$ct_dir/ready"
	[ "$ct_token" = ready ] || bail 'wire fixture did not become ready'
	jq --argjson pid "$ct_wire_pid" '.children = {wire_pid:$pid}' \
		"$ct_dir/manifest.json" > "$ct_dir/journal.tmp"
	mv "$ct_dir/journal.tmp" "$ct_dir/manifest.json"
	# The archive does not need the control files of the test process.
	rm "$ct_dir/ready" "$ct_dir/hold" "$ct_dir/writer"
}

ct_wire_fixture_alive() {
	ct_alive=0
	if [ -r "/proc/$ct_wire_pid/cmdline" ]; then
		tr '\000' '\n' < "/proc/$ct_wire_pid/cmdline" |
			grep -Fxq "$ct_capture" && ct_alive=1
	fi
}

ct_wire_fixture_stop() {
	[ "$ct_alive" = 1 ] && kill -KILL "$ct_wire_pid" 2>/dev/null
	wait "$ct_wire_pid" 2>/dev/null || :
}

ct_recovery_test() {
	ct_finish_fixture_make "recovery-$1"
	ct_wire_fixture_start "$1"
	ct_finish_fixture_run
	ct_wire_fixture_alive
	check_eq "calltrace/recovery $1 archive" 0 "$ct_finish_rc"
	if [ "$1" = matching ]; then
		check_eq 'calltrace/recovery stops matching wire process' 0 "$ct_alive"
		check_has 'calltrace/recovery records the wire stop' \
			"stopped orphan wire capture PID $ct_wire_pid" "$ct_manifest"
	else
		check_eq 'calltrace/recovery preserves unrelated process' 1 "$ct_alive"
		check_lacks 'calltrace/recovery does not claim an unrelated stop' \
			'stopped orphan wire capture' "$ct_manifest"
	fi
	ct_wire_fixture_stop
}

ct_recovery_scan_test() {
	ct_finish_fixture_make recovery-scan
	ct_wire_fixture_start scan
	PATH=$tmp/no-jq "$uvoice_dir/files/calltrace-finish.sh" \
		"$ct_dir" "$ct_archive" all "$$" > "$tmp/scan.out" 2>&1
	ct_finish_rc=$?
	ct_wire_fixture_alive
	ct_manifest=$(tar -xzOf "$ct_archive" \
		'recovery-scan/manifest-finisher.json' 2>/dev/null)
	ct_notes=$(printf '%s' "$ct_manifest" | jq -r --arg pid "$ct_wire_pid" '
		[.notes[] | select(. == "stopped orphan wire capture PID " + $pid)] |
		length')
	check_eq 'calltrace/recovery finds the wire process without jq' '0 0 1' \
		"$ct_finish_rc $ct_alive $ct_notes"
	ct_wire_fixture_stop
}

calltrace_finish_archive_tests() {
	ct_finish_checks='orphan output is frozen
archive uses the tar warning option
private collector files stay outside the archive
busybox timeout word
collector summary is one line
workers use nice level 19
configured RTP ports
nft fallback note
nft fallback cap
dmesg tail selection
coreutils timeout word
unset RTP ports
invalid RTP ports
other-line RTP ports
failed tar removes its part
failed tar restores finishing
signal restores finishing and removes its part
stale part does not overwrite another file
missing README preserves the archive
missing README records a note
summary budget without capture files
summary budget follows the ring file
summary budget sums both capture files'
	ct_skip=''
	for ct_tool in tar jq nice timeout busybox mkfifo awk dd; do
		command -v "$ct_tool" >/dev/null 2>&1 || ct_skip="no $ct_tool command"
	done
	if [ -n "$ct_skip" ]; then
		while IFS= read -r ct_name; do
			skip "calltrace/finisher $ct_name" "$ct_skip"
		done <<EOF
$ct_finish_checks
EOF
		for ct_name in 'matching archive' 'stops matching wire process' \
			'records the wire stop' 'unrelated archive' \
			'preserves unrelated process' 'does not claim an unrelated stop' \
			'finds the wire process without jq'; do
			skip "calltrace/recovery $ct_name" "$ct_skip"
		done
		return
	fi
	ct_finish_tools_make
	ct_finish_fixture_make orphan-output
	ct_finish_fixture_run normal 143
	check_eq 'calltrace/finisher orphan output is frozen' \
		'frozen collector output' \
		"$(tar -xzOf "$ct_archive" "$ct_stem/system/uptime" 2>/dev/null)"
	check_eq 'calltrace/finisher archive uses the tar warning option' \
		0 "$ct_finish_rc"
	check_eq 'calltrace/finisher private collector files stay outside the archive' \
		0 \
		"$(tar -tzf "$ct_archive" 2>/dev/null | grep -c '/\.finish')"
	check_eq 'calltrace/finisher busybox timeout word' timeout \
		"$(printf '%s' "$ct_manifest" | jq -r .collectors.uptime)"
	ct_summary=$(printf '%s' "$ct_manifest" | jq -r '
		.collectors | to_entries | map(.value) |
		"collectors: ok \(map(select(. == "ok"))|length), " +
		"failed \(map(select(. != "ok" and . != "omitted"))|length), " +
		"omitted \(map(select(. == "omitted"))|length)"')
	check_eq 'calltrace/finisher collector summary is one line' \
		"$ct_summary" "$(cat "$tmp/finish-fixture.out")"
	check_eq 'calltrace/finisher workers use nice level 19' 18 \
		"$(grep -c '^-n 19 ' "$ct_nice_rec")"
	ct_rows=$(tar -xzOf "$ct_archive" "$ct_stem/system/conntrack-rtp")
	check_eq 'calltrace/finisher configured RTP ports' 'custom-low custom-high' \
		"$(printf '%s\n' "$ct_rows" | awk '{printf "%s%s", sep, $NF; sep=" "}')"
	check_has 'calltrace/finisher nft fallback note' \
		'no VoIP nftables rule matched' "$ct_manifest"
	check_eq 'calltrace/finisher nft fallback cap' 2000 \
		"$(tar -xzOf "$ct_archive" "$ct_stem/system/nft-voip" | wc -l)"
	check_eq 'calltrace/finisher dmesg tail selection' '500 101 600' \
		"$(tar -xzOf "$ct_archive" "$ct_stem/system/dmesg" |
			awk 'NR == 1 { first=$NF } END { print NR, first, $NF }')"
	ct_finish_fixture_make timeout-coreutils
	ct_finish_fixture_run normal 124
	check_eq 'calltrace/finisher coreutils timeout word' timeout \
		"$(printf '%s' "$ct_manifest" | jq -r .collectors.uptime)"
	for ct_range in unset invalid other-line; do
		ct_finish_fixture_make "range-$ct_range"
		case "$ct_range" in
		unset) ct_range_expr='.channel[0] |= (.rtp_local_port_min=0 |
			.rtp_local_port_max=0)' ;;
		invalid) ct_range_expr='.channel[0] |= (.rtp_local_port_min=30000 |
			.rtp_local_port_max=20000)' ;;
		other-line) ct_range_expr='.'
			jq '.line=1' "$ct_dir/manifest.json" > "$ct_dir/journal.tmp"
			mv "$ct_dir/journal.tmp" "$ct_dir/manifest.json" ;;
		esac
		jq "$ct_range_expr" "$ct_dir/config.json" > "$ct_dir/config.tmp"
		mv "$ct_dir/config.tmp" "$ct_dir/config.json"
		ct_finish_fixture_run
		ct_rows=$(tar -xzOf "$ct_archive" "$ct_stem/system/conntrack-rtp")
		check_eq "calltrace/finisher $ct_range RTP ports" \
			'default-low default-high' \
			"$(printf '%s\n' "$ct_rows" |
				awk '{printf "%s%s", sep, $NF; sep=" "}')"
	done
	ct_finish_fixture_make tar-failed
	ct_finish_fixture_run fail
	ct_part=no
	[ -e "$ct_archive.part" ] && ct_part=yes
	check_eq 'calltrace/finisher failed tar removes its part' '1 no' \
		"$ct_finish_rc $ct_part"
	check_eq 'calltrace/finisher failed tar restores finishing' finishing \
		"$(jq -r .stage "$ct_dir/manifest.json")"
	ct_finish_fixture_make tar-interrupted
	ct_finish_fixture_run term
	ct_part=no
	[ -e "$ct_archive.part" ] && ct_part=yes
	check_eq 'calltrace/finisher signal restores finishing and removes its part' \
		'143 finishing no' \
		"$ct_finish_rc $(jq -r .stage "$ct_dir/manifest.json") $ct_part"
	ct_finish_fixture_make stale-part
	printf 'keep this file\n' > "$tmp/protected"
	ct_protected_before=$(cksum "$tmp/protected")
	ln -s "$tmp/protected" "$ct_archive.part"
	ct_finish_fixture_run
	check_eq 'calltrace/finisher stale part does not overwrite another file' \
		"$ct_protected_before" "$(cksum "$tmp/protected")"
	ct_finish_fixture_make no-readme
	mv /usr/share/uvoice/calltrace-README.txt "$tmp/readme.saved"
	ct_finish_fixture_run
	mv "$tmp/readme.saved" /usr/share/uvoice/calltrace-README.txt
	check_eq 'calltrace/finisher missing README preserves the archive' \
		0 "$ct_finish_rc"
	check_has 'calltrace/finisher missing README records a note' \
		'calltrace README template is absent' "$ct_manifest"
	# The summary reads the ring file and the wire capture whole, so its
	# budget follows their summed size, rounded up: 3 MiB and one byte
	# give 4 MiB. Two files of 1 MiB and one byte give 3 MiB, not the 4
	# that each file rounded up alone gives. A trace without either file
	# gets the 30 s alone.
	ct_budget_awk='/ --collect summary / { print $1 }'
	ct_finish_fixture_make summary-budget-none
	ct_finish_fixture_run
	ct_budget=$(awk "$ct_budget_awk" "$ct_timeout_rec")
	check_eq 'calltrace/finisher summary budget without capture files' \
		30 "$ct_budget"
	ct_finish_fixture_make summary-budget
	dd if=/dev/null of="$ct_dir/rings.pcapng" bs=1 count=0 seek=3145729 \
		2>/dev/null
	ct_finish_fixture_run
	ct_budget=$(awk "$ct_budget_awk" "$ct_timeout_rec")
	check_eq 'calltrace/finisher summary budget follows the ring file' \
		34 "$ct_budget"
	ct_finish_fixture_make summary-budget-both
	dd if=/dev/null of="$ct_dir/rings.pcapng" bs=1 count=0 seek=1048577 \
		2>/dev/null
	dd if=/dev/null of="$ct_dir/wire.pcap" bs=1 count=0 seek=1048577 \
		2>/dev/null
	ct_finish_fixture_run
	ct_budget=$(awk "$ct_budget_awk" "$ct_timeout_rec")
	check_eq 'calltrace/finisher summary budget sums both capture files' \
		33 "$ct_budget"
	ct_recovery_test matching
	ct_recovery_test unrelated
	ct_recovery_scan_test
}
calltrace_finish_archive_tests

calltrace_wire_tests() {
	ct_wire_script=$uvoice_dir/files/calltrace-wire.sh
	mkdir -p "$tmp/wire-fixture"
	"$ct_wire_script" "$tmp/wire-fixture" all ct-no-such >/dev/null 2>&1
	ct_short=$?
	"$ct_wire_script" "$tmp/wire-fixture" all ct-no-such udp extra \
		>/dev/null 2>&1
	check_eq 'calltrace/wire requires exactly four arguments' '2 2' "$ct_short $?"
	if [ ! -x /usr/bin/tcpdump ]; then
		skip 'calltrace/wire bad interface keeps command version and error' \
			'no /usr/bin/tcpdump on the host'
		return
	fi
	"$ct_wire_script" "$tmp/wire-fixture" all ct-no-such udp >/dev/null 2>&1
	ct_wire_rc=$?
	if [ "$ct_wire_rc" -ne 0 ] &&
		grep -Eq '^command: .*; version: .+' "$tmp/wire-fixture/wire.stats" &&
		grep -v '^command:' "$tmp/wire-fixture/wire.stats" |
		grep -Fq ct-no-such; then
		ok 'calltrace/wire bad interface keeps command version and error'
	else
		not_ok 'calltrace/wire bad interface keeps command version and error' \
			"exit $ct_wire_rc: $(cat "$tmp/wire-fixture/wire.stats")"
	fi
}
calltrace_wire_tests

calltrace_techlog_test() {
	if ! command -v bash >/dev/null 2>&1; then
		skip 'calltrace/techlog records an absent archive' 'no bash command'
		return
	fi
	mkdir -p /usr/srg/scripts/techlog /tmp/uvoice/calltrace "$tmp/no-traces"
	mount --bind "$tmp/no-traces" /tmp/uvoice/calltrace ||
		bail 'cannot isolate completed archives for techlog'
	ct_techlog_rec=$tmp/techlog.omissions
	export ct_techlog_rec
	cat > /usr/srg/scripts/techlog/techlog.src <<'EOF'
techlog_cmd_capture() { return 0; }
my_cp_file() { return 0; }
techlog_omit() { printf '%s\n' "$*" >> "$ct_techlog_rec"; }
EOF
	bash "$uvoice_dir/files/techlog-uvoice.sh" > "$tmp/techlog.out" 2>&1
	ct_techlog_rc=$?
	check_eq 'calltrace/techlog records an absent archive' \
		'0 no completed call trace available' \
		"$ct_techlog_rc $(cat "$ct_techlog_rec" 2>/dev/null)"
	umount /tmp/uvoice/calltrace || bail 'cannot release the techlog fixture'
}
calltrace_techlog_test

#
# The summary report. files/calltrace-summary.uc reads one archive directory
# and writes SUMMARY.txt. Three inputs drive it: a fixture directory with the
# shape of a real archive and neither a wire capture nor rings, a second
# fixture with a capture this suite builds byte by byte for the SIP ladder,
# and the live archive of the call trace leg above, which carries real rings.
#
ct_sum_gen=$uvoice_dir/files/calltrace-summary.uc

ct_sum_byte() {
	printf '%b' "\\0$(printf '%03o' "$1")"
}

ct_sum_le16() {
	ct_sum_byte $(( $1 & 255 ))
	ct_sum_byte $(( ($1 >> 8) & 255 ))
}

ct_sum_le32() {
	ct_sum_byte $(( $1 & 255 ))
	ct_sum_byte $(( ($1 >> 8) & 255 ))
	ct_sum_byte $(( ($1 >> 16) & 255 ))
	ct_sum_byte $(( ($1 >> 24) & 255 ))
}

ct_sum_be16() {
	ct_sum_byte $(( ($1 >> 8) & 255 ))
	ct_sum_byte $(( $1 & 255 ))
}

ct_sum_addr() {
	ct_sum_ifs=$IFS
	IFS=.
	for ct_sum_part in $1; do
		ct_sum_byte "$ct_sum_part"
	done
	IFS=$ct_sum_ifs
}

# The two 16-bit words of a dotted address, for the header checksum.
ct_sum_addr_words() {
	ct_sum_ifs=$IFS
	IFS=.
	# shellcheck disable=SC2086
	set -- $1
	IFS=$ct_sum_ifs
	printf '%s %s' $(( $1 * 256 + $2 )) $(( $3 * 256 + $4 ))
}

# The ones-complement sum of RFC 1071 over the header the writer builds
# (src/trace_ring.c ipv4_hdr_build): version and header length, the DSCP
# byte, the total length, the TTL and the protocol, then both addresses.
# $1 is the DSCP byte, $2 the total length, $3 the source, $4 the target.
ct_sum_cksum() {
	ct_sum_words=$(ct_sum_addr_words "$3")
	ct_sum_s1=${ct_sum_words% *}
	ct_sum_s2=${ct_sum_words#* }
	ct_sum_words=$(ct_sum_addr_words "$4")
	ct_sum_d1=${ct_sum_words% *}
	ct_sum_d2=${ct_sum_words#* }
	ct_sum_acc=$(( 0x4500 + $1 + $2 + 0x4011 + ct_sum_s1 + ct_sum_s2 +
		ct_sum_d1 + ct_sum_d2 ))
	while [ $(( ct_sum_acc >> 16 )) -ne 0 ]; do
		ct_sum_acc=$(( (ct_sum_acc & 65535) + (ct_sum_acc >> 16) ))
	done
	printf '%s' $(( ~ct_sum_acc & 65535 ))
}

ct_sum_zeros() {
	ct_sum_i=0
	while [ "$ct_sum_i" -lt "$1" ]; do
		ct_sum_byte 0
		ct_sum_i=$(( ct_sum_i + 1 ))
	done
}

# One IPv4 and UDP header, then the message. $1 is the source address, $2 the
# destination address, $3 the DSCP byte and $4 the first line of the message,
# which CR LF closes. $5 and $6 are the source and destination ports, 5060
# when absent.
ct_sum_ipudp() {
	ct_sum_pay=$(( ${#4} + 2 ))
	ct_sum_iplen=$(( 28 + ct_sum_pay ))
	ct_sum_byte 69
	ct_sum_byte "$3"
	ct_sum_be16 "$ct_sum_iplen"
	ct_sum_le32 0
	ct_sum_byte 64; ct_sum_byte 17
	ct_sum_be16 "$(ct_sum_cksum "$3" "$ct_sum_iplen" "$1" "$2")"
	ct_sum_addr "$1"
	ct_sum_addr "$2"
	ct_sum_be16 "${5:-5060}"
	ct_sum_be16 "${6:-5060}"
	ct_sum_be16 $(( 8 + ct_sum_pay ))
	ct_sum_be16 0
	printf '%s\r\n' "$4"
}

# One captured datagram: the record header, an Ethernet header with two made
# up addresses, then the message. $4 and $5 are the ports of ct_sum_ipudp.
ct_sum_datagram() {
	ct_sum_len=$(( 42 + ${#3} + 2 ))
	ct_sum_le32 1700000000
	ct_sum_le32 0
	ct_sum_le32 "$ct_sum_len"
	ct_sum_le32 "$ct_sum_len"
	ct_sum_byte 2; ct_sum_byte 0; ct_sum_byte 0
	ct_sum_byte 0; ct_sum_byte 0; ct_sum_byte 1
	ct_sum_byte 2; ct_sum_byte 0; ct_sum_byte 0
	ct_sum_byte 0; ct_sum_byte 0; ct_sum_byte 2
	ct_sum_be16 2048
	ct_sum_ipudp "$1" "$2" 0 "$3" "$4" "$5"
}

# The pcap file header of libpcap 2.4, little endian, Ethernet link type.
ct_sum_pcap_write() {
	ct_sum_le32 2712847316
	ct_sum_le16 2
	ct_sum_le16 4
	ct_sum_le32 0
	ct_sum_le32 0
	ct_sum_le32 65535
	ct_sum_le32 1
	ct_sum_datagram 192.0.2.10 198.51.100.10 'INVITE sip:1@2 SIP/2.0'
	ct_sum_datagram 198.51.100.10 192.0.2.10 'SIP/2.0 200 OK'
}

# The capture of an MGCP trace: an RSIP to the call agent and its answer.
ct_sum_mgcp_pcap_write() {
	ct_sum_le32 2712847316
	ct_sum_le16 2
	ct_sum_le16 4
	ct_sum_le32 0
	ct_sum_le32 0
	ct_sum_le32 65535
	ct_sum_le32 1
	ct_sum_datagram 192.0.2.10 198.51.100.10 \
		'RSIP 1 aaln/*@gw.test MGCP 1.0' 2427 2727
	ct_sum_datagram 198.51.100.10 192.0.2.10 '200 1 OK' 2727 2427
}

# The rings of an MGCP trace: the RSIP in the signaling ring, kind 6 in the
# DSCP byte (src/trace_ring.h TRACE_PKT_MGCP).
ct_sum_mgcp_rings_write() {
	ct_sum_ipudp 192.0.2.10 198.51.100.10 24 \
		'RSIP 1 aaln/*@gw.test MGCP 1.0' 2427 2727 > "$tmp/ct-entry-mgcp"
	ct_sum_shb
	ct_sum_idb 101 uvoiced_sip
	ct_sum_epb 0 1700000004000000 "$tmp/ct-entry-mgcp"
}

# The pcapng blocks the daemon's export writes (src/pcapng.c): a section
# header, one interface per ring and one packet block per entry, all little
# endian with microsecond stamps.
ct_sum_shb() {
	ct_sum_le32 168627466
	ct_sum_le32 28
	ct_sum_le32 439041101
	ct_sum_le16 1
	ct_sum_le16 0
	ct_sum_le32 4294967295
	ct_sum_le32 4294967295
	ct_sum_le32 28
}

# $3 is the if_tsresol byte; 6 is the decimal microseconds the daemon writes.
ct_sum_idb() {
	ct_sum_nlen=$(( ${#2} + 1 ))
	ct_sum_blen=$(( 36 + (ct_sum_nlen + 3) / 4 * 4 ))
	ct_sum_le32 1
	ct_sum_le32 "$ct_sum_blen"
	ct_sum_le16 "$1"
	ct_sum_le16 0
	ct_sum_le32 0
	ct_sum_le16 2
	ct_sum_le16 "$ct_sum_nlen"
	printf '%s' "$2"
	ct_sum_byte 0
	ct_sum_zeros $(( (ct_sum_nlen + 3) / 4 * 4 - ct_sum_nlen ))
	ct_sum_le16 9
	ct_sum_le16 1
	ct_sum_byte "${3:-6}"
	ct_sum_zeros 3
	ct_sum_le16 0
	ct_sum_le16 0
	ct_sum_le32 "$ct_sum_blen"
}

# A packet block shorter than its own five fields.
ct_sum_short_epb() {
	ct_sum_le32 6
	ct_sum_le32 16
	ct_sum_le32 0
	ct_sum_le32 16
}

# $1 is the interface, $2 the stamp in microseconds and $3 a file holding the
# entry.
ct_sum_epb() {
	ct_sum_plen=$(wc -c < "$3")
	ct_sum_pad=$(( (4 - ct_sum_plen % 4) % 4 ))
	ct_sum_blen=$(( 32 + ct_sum_plen + ct_sum_pad ))
	ct_sum_le32 6
	ct_sum_le32 "$ct_sum_blen"
	ct_sum_le32 "$1"
	ct_sum_le32 $(( $2 / 4294967296 ))
	ct_sum_le32 $(( $2 % 4294967296 ))
	ct_sum_le32 "$ct_sum_plen"
	ct_sum_le32 "$ct_sum_plen"
	cat "$3"
	ct_sum_zeros "$ct_sum_pad"
	ct_sum_le32 "$ct_sum_blen"
}

# The rings of one trace: a log line, a run of three sweep lines, a SIP
# message sent, a SIP message received, an entry no header decodes, a status
# entry, a canceller entry and two media entries of one flow. The DSCP byte
# carries the direction in bits 7..6 and the kind in bits 5..2
# (src/trace_ring.h), so kind 3 is RTP and the byte 12 is a transmitted RTP
# entry. The canceller entry is the 64 byte header of an END at 80 samples
# (src/trace_ring.h struct trace_lec_hdr). The report reads no canceller
# entry, so the walk steps over it, and the media entries behind it still
# count.
ct_sum_rings_write() {
	printf '%s' '[fsm] line 0 SEIZE in IDLE arg 0' > "$tmp/ct-entry-log"
	printf '%s' '[jb] l0 s0 get frame ts=1 play=1 depth=20 tgt=30' \
		> "$tmp/ct-entry-jb"
	printf '%s' '[media] l0 s0 jb out=1 plc=0 cn=0 sil=0' \
		> "$tmp/ct-entry-media"
	ct_sum_ipudp 192.0.2.10 198.51.100.10 0 'INVITE sip:1@2 SIP/2.0' \
		> "$tmp/ct-entry-tx"
	ct_sum_ipudp 198.51.100.10 192.0.2.10 64 'SIP/2.0 200 OK' \
		> "$tmp/ct-entry-rx"
	printf '\r\n\r\n' > "$tmp/ct-entry-bad"
	printf '%s' '{"kind":"start"}' > "$tmp/ct-entry-status"
	ct_sum_ipudp 192.0.2.10 203.0.113.10 12 'rtp payload' \
		> "$tmp/ct-entry-rtp"
	{
		ct_sum_zeros 48
		ct_sum_le16 80
		ct_sum_zeros 9
		ct_sum_byte 2
		ct_sum_zeros 4
	} > "$tmp/ct-entry-lec"
	ct_sum_shb
	ct_sum_idb 147 uvoiced_log
	ct_sum_idb 101 uvoiced_sip
	ct_sum_idb 147 uvoiced_status
	ct_sum_idb 101 uvoiced_rtp
	ct_sum_idb 148 uvoiced_lec
	ct_sum_epb 0 1700000000000000 "$tmp/ct-entry-log"
	ct_sum_epb 0 1700000001000000 "$tmp/ct-entry-jb"
	ct_sum_epb 0 1700000002000000 "$tmp/ct-entry-media"
	ct_sum_epb 0 1700000003000000 "$tmp/ct-entry-jb"
	ct_sum_epb 1 1700000004000000 "$tmp/ct-entry-tx"
	ct_sum_epb 1 1700000005000000 "$tmp/ct-entry-rx"
	ct_sum_epb 1 1700000006000000 "$tmp/ct-entry-bad"
	ct_sum_epb 2 1700000007000000 "$tmp/ct-entry-status"
	ct_sum_epb 4 1700000007500000 "$tmp/ct-entry-lec"
	ct_sum_epb 3 1700000008000000 "$tmp/ct-entry-rtp"
	ct_sum_epb 3 1700000009000000 "$tmp/ct-entry-rtp"
}

# An export with three blocks the walk must name and step over: an interface
# whose time resolution is binary, a packet block shorter than its fields,
# and a packet block naming an interface with no description.
ct_sum_odd_rings_write() {
	printf '%s' '[fsm] line 0 SEIZE in IDLE arg 0' > "$tmp/ct-entry-log"
	ct_sum_shb
	ct_sum_idb 147 uvoiced_log 131
	ct_sum_epb 0 1700000000000000 "$tmp/ct-entry-log"
	ct_sum_short_epb
	ct_sum_epb 7 1700000001000000 "$tmp/ct-entry-log"
}

ct_sum_fixture_make() {
	ct_sum_dir=$tmp/summary-fixture
	rm -rf "$ct_sum_dir"
	mkdir -p "$ct_sum_dir"
	cat > "$ct_sum_dir/manifest.json" <<'EOF'
{"id":"20260101-000000-line0-abcd","line":0,"stage":"done","reason":"call_ended",
 "times":{"armed_at":1700000000,"capture_ready_at":1700000001,
	  "started_at":1700000010,"ended_at":1700000040},
 "options":{"timeout_s":300,"grace_s":5,"arm_timeout_s":900,"wire":false,
	    "wire_max_mb":32,"rings_max_mb":64},
 "counts":{"calls":1,"snapshots":3,
	   "rings":{"log":{"written":10,"exported":10,"overwritten":0,
			   "truncated":0},
		    "sip":{"written":4,"exported":4,"overwritten":0,
			   "truncated":0},
		    "status":{"written":3,"exported":3,"overwritten":0,
			      "truncated":0},
		    "rtp":{"written":700,"exported":700,"overwritten":0,
			   "truncated":0},
		    "lec":{"written":5,"exported":3,"overwritten":0,
			   "truncated":2},
		    "relay_dropped":0,"file_bytes":4096,"truncated":false},
	   "wire":{"bytes":0,"restarts":0,"truncated":false,"iface":"wan",
		   "filter":"udp port 5060","cpumask":"0,1"}},
 "versions":{"uvoice":"1.0.0","schema":"1.0.0"},
 "boot_id":"11111111-2222-3333-4444-555555555555","daemon_pid":1234,
 "contains":["sip_plaintext"],"notes":["fixture note"],
 "collectors":{"uptime":"ok","loadavg":"ok","dmesg":"failed 7",
	       "ssl.keylog":"omitted"},
 "board":{"model":"AN7581-board","serial":"AN7581A00010000001"},
 "md5":"0123456789abcdef0123456789abcdef  /usr/sbin/uvoiced"}
EOF
	cat > "$ct_sum_dir/snapshots.jsonl" <<'EOF'
{"kind":"start","seq":0,"t_wall_ms":1700000010000,"fsm_state":"OFFHOOK_DIAL","clock":{"realtime_us":1700000010000000,"monotonic_ns":1000000000,"step_detected":false},"hal":{"holds":1,"capture_drops":0,"trim_drops":0,"playback_expired":0,"completions_lost":0},"events":{"read":1,"sent":1,"dropped":0,"lost":0}}
{"kind":"event","seq":1,"t_wall_ms":1700000020000,"fsm_state":"CONNECTED","reasons":["fsm_offhook_dial_connected","rtp_first_rx"],"clock":{"realtime_us":1700000020000000,"monotonic_ns":11000000000,"step_detected":false},"hal":{"holds":2,"capture_drops":0,"trim_drops":0,"playback_expired":0,"completions_lost":0},"events":{"read":5,"sent":5,"dropped":1,"lost":0},"diag":{"streams":[{"line":0,"stream":0,"net_drops":1,"net_stale":2,"net_trunc":3,"tx_fail":4,"punch_sent":5,"srtp_protect_ok":6,"srtp_protect_fail":7,"srtp_unprotect_ok":8,"srtp_unprotect_auth_fail":9,"srtp_unprotect_replay":10}],"engine":{"running":true,"period_us":10000,"ticks":1234,"holds":5,"capture_drops":0,"trim_drops":0,"playback_expired":0,"xruns":0,"interval":[11,12,13,14,15,16,17,18]}}}
{"kind":"end","seq":2,"t_wall_ms":1700000040000,"fsm_state":"IDLE","clock":{"realtime_us":1700000040000000,"monotonic_ns":31000000000,"step_detected":false},"hal":{"holds":4,"capture_drops":0,"trim_drops":0,"playback_expired":0,"completions_lost":0},"events":{"read":9,"sent":9,"dropped":2,"lost":0},"cdr":[{"seq":7,"line":0,"slot":0,"inbound":false,"answered":true,"start":1700000015,"duration_ms":20000,"answer_offset_ms":5000,"codec":"pcmu","media_state":"present","media":{"packets_sent":4242,"packets_recv":4240,"packets_lost":2,"loss_rate":1,"jitter_avg":3,"jitter_max":9,"rtt_avg":11,"rtt_max":22,"mos_lq":"4.40","mos_cq":"4.30","rfactor":90,"bytes_sent":100000,"bytes_recv":99000},"jb":{"mode":"adaptive","depth_ms":20,"target_ms":30,"underruns":1,"late_drop":2,"resyncs":3}}]}
EOF
	cat > "$ct_sum_dir/config.json" <<'EOF'
{"channel":[{"id":0,"sip_local_port":5060,"rtp_local_port_min":20000,
	     "rtp_local_port_max":20008}]}
EOF
	cat > "$ct_sum_dir/wire.stats" <<'EOF'
command: /usr/bin/tcpdump -i wan; version: tcpdump version 4.99.6
7 packets captured
0 packets dropped by kernel
EOF
}

# The wrap keeps the report at its width, so a needle that runs past it is
# read from the whole file with its line breaks folded into single spaces.
ct_sum_flatten() {
	tr '\n' ' ' < "$1" | tr -s ' '
}

calltrace_summary_tests() {
	ct_sum_checks='fixture exit
fixture sections in order
fixture timeline snapshots
fixture call packets
fixture wire absent
fixture rings absent
fixture diag histogram
fixture failed collector
fixture lec ring line
stdout leaves the file unwritten
a directory without a manifest exits 1
a write failure exits 2
rings timeline log line
rings timeline sip send
rings timeline sip receive
rings sip undecodable
rings sip ladder peers
rings media flow count
rings canceller entry stepped over
rings media walk bound
rings sweep fold
rings full prints every sweep line
odd rings unscaled resolution
odd rings short block
odd rings unknown interface
no config names the file
no config dashes the line
wire read failure
ladder direction
ladder unit address from the interface
ladder unknown unit address
mgcp section
mgcp ring counts
mgcp ring ladder
mgcp unknown unit address
mgcp wire ladder'
	ct_sum_skip=''
	if ! command -v ucode >/dev/null 2>&1; then
		ct_sum_skip='no ucode command'
	elif ! ucode -e "import * as s from 'struct';" >/dev/null 2>&1; then
		ct_sum_skip='no ucode struct module'
	fi
	if [ -n "$ct_sum_skip" ]; then
		while IFS= read -r ct_sum_name; do
			skip "summary/$ct_sum_name" "$ct_sum_skip"
		done <<EOF
$ct_sum_checks
EOF
		return
	fi

	ct_sum_fixture_make
	ucode "$ct_sum_gen" "$ct_sum_dir" > "$tmp/summary.err" 2>&1
	ct_sum_rc=$?
	check_eq 'summary/fixture exit' '0:' \
		"$ct_sum_rc:$(cat "$tmp/summary.err")"
	ct_sum_file=$ct_sum_dir/SUMMARY.txt
	ct_sum_text=$(cat "$ct_sum_file" 2>/dev/null)
	ct_sum_flat=$(ct_sum_flatten "$ct_sum_file" 2>/dev/null)
	check_eq 'summary/fixture sections in order' \
		'TRACE|TIMES|TIMELINE|CALLS|SIP|MEDIA ON THE WIRE|RINGS|DIAG|SYSTEM|HOW TO READ|' \
		"$(printf '%s\n' "$ct_sum_text" |
			sed -n 's/^\([A-Z][A-Z ]*\)$/\1/p' | tr '\n' '|')"
	check_eq 'summary/fixture timeline snapshots' 3 \
		"$(printf '%s\n' "$ct_sum_text" |
			grep -cE '^  [0-9][0-9]:[0-9][0-9]:[0-9][0-9]\.[0-9][0-9][0-9]  snapshot ')"
	check_has 'summary/fixture call packets' 'packets sent       4242' \
		"$ct_sum_text"
	check_has 'summary/fixture wire absent' 'wire.pcap: absent (wire off)' \
		"$(printf '%s\n' "$ct_sum_text" |
			sed -n '/^SIP$/,/^MEDIA ON THE WIRE$/p')"
	check_has 'summary/fixture rings absent' 'rings.pcapng: absent' \
		"$(printf '%s\n' "$ct_sum_text" | sed -n '/^TIMELINE$/,/^CALLS$/p')"
	check_has 'summary/fixture diag histogram' \
		'<=9ms 11 | <=11ms 12 | <=12ms 13 | <=15ms 14 | <=20ms 15 | <=30ms 16 | <=50ms 17 | >50ms 18' \
		"$ct_sum_flat"
	check_has 'summary/fixture failed collector' \
		'collectors failed dmesg (failed 7)' "$ct_sum_flat"
	check_eq 'summary/fixture lec ring line' \
		'written 5, exported 3, overwritten 0, truncated 2' \
		"$(printf '%s\n' "$ct_sum_text" |
			sed -n '/^RINGS$/,/^DIAG$/s/^  lec  *//p')"

	ct_sum_fixture_make
	ct_sum_out=$(ucode "$ct_sum_gen" "$ct_sum_dir" - 2>&1)
	ct_sum_written=no
	[ -e "$ct_sum_dir/SUMMARY.txt" ] && ct_sum_written=yes
	check_eq 'summary/stdout leaves the file unwritten' 'TRACE no' \
		"$(printf '%s\n' "$ct_sum_out" | head -n 1) $ct_sum_written"

	mkdir -p "$tmp/summary-empty"
	ct_sum_out=$(ucode "$ct_sum_gen" "$tmp/summary-empty" 2>&1)
	ct_sum_rc=$?
	check_eq 'summary/a directory without a manifest exits 1' '1 1' \
		"$ct_sum_rc $(printf '%s\n' "$ct_sum_out" | wc -l)"

	# A directory in place of the report is a write the generator cannot
	# make, whatever the caller's privileges are.
	ct_sum_fixture_make
	mkdir "$ct_sum_dir/SUMMARY.txt"
	ct_sum_out=$(ucode "$ct_sum_gen" "$ct_sum_dir" 2>&1)
	ct_sum_rc=$?
	check_eq 'summary/a write failure exits 2' '2 1' \
		"$ct_sum_rc $(printf '%s\n' "$ct_sum_out" | wc -l)"

	# The ring entries the daemon writes, without a daemon: one log line and
	# two SIP messages, one in each direction.
	ct_sum_dir=$tmp/summary-rings
	rm -rf "$ct_sum_dir"
	mkdir -p "$ct_sum_dir"
	printf '%s\n' '{"id":"rings","line":0,"stage":"done"}' \
		> "$ct_sum_dir/manifest.json"
	printf '%s\n' '{"channel":[{"id":0,"sip_local_port":5060}]}' \
		> "$ct_sum_dir/config.json"
	ct_sum_rings_write > "$ct_sum_dir/rings.pcapng"
	ucode "$ct_sum_gen" "$ct_sum_dir" >/dev/null 2>&1
	ct_sum_text=$(cat "$ct_sum_dir/SUMMARY.txt" 2>/dev/null)
	ct_sum_timeline=$(printf '%s\n' "$ct_sum_text" |
		sed -n '/^TIMELINE$/,/^CALLS$/p')
	check_has 'summary/rings timeline log line' \
		'[fsm] line 0 SEIZE in IDLE arg 0' "$ct_sum_timeline"
	check_has 'summary/rings timeline sip send' \
		'SIP -> 198.51.100.10:5060 INVITE sip:1@2 SIP/2.0' "$ct_sum_timeline"
	check_has 'summary/rings timeline sip receive' \
		'SIP <- 198.51.100.10:5060 SIP/2.0 200 OK' "$ct_sum_timeline"
	ct_sum_flat=$(ct_sum_flatten "$ct_sum_dir/SUMMARY.txt")
	check_has 'summary/rings sip undecodable' 'ring undecodable 1 entry' \
		"$ct_sum_flat"
	check_has 'summary/rings sip ladder peers' \
		'ring peers 198.51.100.10:5060 2' "$ct_sum_flat"
	# The two media entries share one flow, so the walk counts them into one
	# record and keeps neither entry.
	check_has 'summary/rings media flow count' \
		'tx RTP 192.0.2.10:5060 > 203.0.113.10:5060 2 records' \
		"$ct_sum_flat"
	# The report reads no canceller entry, so the walk steps over the one
	# in front of the media entries and writes no note for it.
	check_eq 'summary/rings canceller entry stepped over' 0 \
		"$(printf '%s\n' "$ct_sum_timeline" | grep -c 'rings.pcapng: ')"
	# The bound counts the entries the walk reads, not what the manifest
	# claims, so a stale journal cannot lift it. One entry is enough here.
	ct_sum_bound=$(UVOICE_SUMMARY_RTP_MAX=1 ucode "$ct_sum_gen" \
		"$ct_sum_dir" - 2>&1 | tr '\n' ' ' | tr -s ' ')
	check_has 'summary/rings media walk bound' \
		'rtp flows walked for the first 1 of 2 records' "$ct_sum_bound"
	# The three sweep lines of the fixture fold into one line, and --full
	# prints each of them instead.
	check_has 'summary/rings sweep fold' 'jb/media sweep: 3 lines from' \
		"$ct_sum_flat"
	ct_sum_full=$(ucode "$ct_sum_gen" -- "$ct_sum_dir" - --full 2>&1)
	check_eq 'summary/rings full prints every sweep line' '2 0' \
		"$(printf '%s\n' "$ct_sum_full" | grep -c '\[jb\] l0 s0 get frame') $(printf '%s\n' "$ct_sum_full" | grep -c ' sweep: ')"

	ct_sum_dir=$tmp/summary-odd-rings
	rm -rf "$ct_sum_dir"
	mkdir -p "$ct_sum_dir"
	printf '%s\n' '{"id":"odd","line":0,"stage":"done"}' \
		> "$ct_sum_dir/manifest.json"
	ct_sum_odd_rings_write > "$ct_sum_dir/rings.pcapng"
	ct_sum_flat=$(ucode "$ct_sum_gen" "$ct_sum_dir" - 2>&1 | tr '\n' ' ' |
		tr -s ' ')
	check_has 'summary/odd rings unscaled resolution' \
		'unsupported time resolution on uvoiced_log, stamps unscaled' \
		"$ct_sum_flat"
	check_has 'summary/odd rings short block' 'a packet block is too short' \
		"$ct_sum_flat"
	check_has 'summary/odd rings unknown interface' \
		'a packet names an interface with no description' "$ct_sum_flat"

	# Without config.json the report says so and dashes what the manifest
	# does not carry.
	ct_sum_dir=$tmp/summary-no-config
	rm -rf "$ct_sum_dir"
	mkdir -p "$ct_sum_dir"
	printf '%s\n' '{"id":"noconfig","stage":"done"}' \
		> "$ct_sum_dir/manifest.json"
	ct_sum_flat=$(ucode "$ct_sum_gen" "$ct_sum_dir" - 2>&1 | tr '\n' ' ' |
		tr -s ' ')
	check_has 'summary/no config names the file' 'config.json: absent' \
		"$ct_sum_flat"
	check_has 'summary/no config dashes the line' ' line - ' "$ct_sum_flat"

	# A capture tcpdump refuses is a diagnostic and a dash, never a zero.
	ct_sum_dir=$tmp/summary-bad-wire
	rm -rf "$ct_sum_dir"
	mkdir -p "$ct_sum_dir"
	printf '%s\n' '{"id":"badwire","line":0,"stage":"done"}' \
		> "$ct_sum_dir/manifest.json"
	printf '%s\n' '{"channel":[{"id":0,"sip_local_port":5060}]}' \
		> "$ct_sum_dir/config.json"
	printf 'not a capture\n' > "$ct_sum_dir/wire.pcap"
	if [ ! -x /usr/bin/tcpdump ]; then
		skip 'summary/wire read failure' 'no /usr/bin/tcpdump on the host'
	else
		ct_sum_flat=$(ucode "$ct_sum_gen" "$ct_sum_dir" - 2>&1 |
			tr '\n' ' ' | tr -s ' ')
		check_has 'summary/wire read failure' \
			'on the wire - tcpdump: exit 1, the wire ladder is incomplete' \
			"$ct_sum_flat"
	fi

	# An MGCP trace: the manifest names the signaling ring mgcp, and the
	# configuration holds the MGCP port.
	ct_sum_dir=$tmp/summary-mgcp
	rm -rf "$ct_sum_dir"
	mkdir -p "$ct_sum_dir"
	printf '%s\n' '{"id":"mgcp","line":0,"stage":"done","counts":{"rings":{
		"mgcp":{"written":6,"exported":6,"overwritten":0,"truncated":0}},
		"wire":{"filter":"udp port 2427"}}}' > "$ct_sum_dir/manifest.json"
	printf '%s\n' '{"protocol":"mgcp","mgcp":{"local_port":2427},
		"channel":[{"id":0}]}' > "$ct_sum_dir/config.json"
	ct_sum_mgcp_rings_write > "$ct_sum_dir/rings.pcapng"
	ucode "$ct_sum_gen" "$ct_sum_dir" >/dev/null 2>&1
	ct_sum_text=$(cat "$ct_sum_dir/SUMMARY.txt" 2>/dev/null)
	check_eq 'summary/mgcp section' 1 \
		"$(printf '%s\n' "$ct_sum_text" | grep -c '^MGCP$')"
	check_eq 'summary/mgcp ring counts' \
		'written 6, exported 6, overwritten 0, truncated 0' \
		"$(printf '%s\n' "$ct_sum_text" |
			sed -n '/^RINGS$/,/^DIAG$/s/^  mgcp  *//p')"
	check_has 'summary/mgcp ring ladder' \
		'-> 198.51.100.10:2727 RSIP 1 aaln/*@gw.test MGCP 1.0' \
		"$(printf '%s\n' "$ct_sum_text" | sed -n '/^MGCP$/,/^MEDIA/p' |
			tr -s ' ')"

	# With no ring and no interface address, the note names the ring of
	# the protocol. The note comes before tcpdump reads the capture.
	ct_sum_dir=$tmp/summary-mgcp-blind
	rm -rf "$ct_sum_dir"
	mkdir -p "$ct_sum_dir"
	cp "$tmp/summary-mgcp/manifest.json" "$tmp/summary-mgcp/config.json" \
		"$ct_sum_dir/"
	ct_sum_mgcp_pcap_write > "$ct_sum_dir/wire.pcap"
	if [ ! -x /usr/bin/tcpdump ]; then
		skip 'summary/mgcp unknown unit address' \
			'no /usr/bin/tcpdump on the host'
	else
		ucode "$ct_sum_gen" "$ct_sum_dir" >/dev/null 2>&1
		check_has 'summary/mgcp unknown unit address' \
			'the mgcp ring holds no transmit entry' \
			"$(ct_sum_flatten "$ct_sum_dir/SUMMARY.txt")"
	fi

	# The probe runs on a capture of its own, so the fixture under test is
	# never the reason the ladder checks skip. A tcpdump that drops
	# privileges reads nothing inside this user namespace.
	ct_sum_probe=$tmp/summary-probe.pcap
	ct_sum_pcap_write > "$ct_sum_probe"
	ct_sum_skip=''
	if [ ! -x /usr/bin/tcpdump ]; then
		ct_sum_skip='no /usr/bin/tcpdump on the host'
	elif ! /usr/bin/tcpdump -r "$ct_sum_probe" -n 2>/dev/null |
		grep -q ' IP '; then
		ct_sum_skip='tcpdump reads no capture in this namespace'
	fi
	if [ -n "$ct_sum_skip" ]; then
		for ct_sum_name in 'ladder direction' \
			'ladder unit address from the interface' \
			'ladder unknown unit address' 'mgcp wire ladder'; do
			skip "summary/$ct_sum_name" "$ct_sum_skip"
		done
		return
	fi

	# The unit address comes from a transmit entry of the sip ring.
	ct_sum_dir=$tmp/summary-ladder
	rm -rf "$ct_sum_dir"
	mkdir -p "$ct_sum_dir"
	printf '%s\n' '{"id":"ladder","line":0,"stage":"done"}' \
		> "$ct_sum_dir/manifest.json"
	printf '%s\n' '{"channel":[{"id":0,"sip_local_port":5060}]}' \
		> "$ct_sum_dir/config.json"
	ct_sum_pcap_write > "$ct_sum_dir/wire.pcap"
	ct_sum_rings_write > "$ct_sum_dir/rings.pcapng"
	ucode "$ct_sum_gen" "$ct_sum_dir" >/dev/null 2>&1
	# The wire ladder lies between the unit address and the count, so the
	# ladder of the sip ring above it is not read here.
	check_eq 'summary/ladder direction' \
		'-> 198.51.100.10:5060 INVITE sip:1@2 SIP/2.0
<- 198.51.100.10:5060 SIP/2.0 200 OK' \
		"$(sed -n '/^  unit address/,/^  on the wire/p' \
			"$ct_sum_dir/SUMMARY.txt" |
			sed -n 's/^  [0-9:.]*  \(-[>] .*\)$/\1/p;s/^  [0-9:.]*  \([<]- .*\)$/\1/p' |
			tr -s ' ')"

	# Without a sip ring the capture interface of the manifest names it.
	ct_sum_dir=$tmp/summary-ladder-iface
	rm -rf "$ct_sum_dir"
	mkdir -p "$ct_sum_dir/system"
	printf '%s\n' '{"id":"iface","line":0,"stage":"done",
		"counts":{"wire":{"iface":"wan"}}}' > "$ct_sum_dir/manifest.json"
	printf '%s\n' '{"channel":[{"id":0,"sip_local_port":5060}]}' \
		> "$ct_sum_dir/config.json"
	printf '%s\n' '[{"ifname":"lan","addr_info":[{"family":"inet",
		"local":"192.168.1.1"}]},{"ifname":"wan","addr_info":[
		{"family":"inet6","local":"fe80::1"},
		{"family":"inet","local":"192.0.2.10"}]}]' \
		> "$ct_sum_dir/system/ip-addr.json"
	ct_sum_pcap_write > "$ct_sum_dir/wire.pcap"
	ucode "$ct_sum_gen" "$ct_sum_dir" >/dev/null 2>&1
	check_has 'summary/ladder unit address from the interface' \
		'unit address 192.0.2.10:5060' \
		"$(ct_sum_flatten "$ct_sum_dir/SUMMARY.txt")"

	# With neither source the ladder shows no direction and says why.
	ct_sum_dir=$tmp/summary-ladder-blind
	rm -rf "$ct_sum_dir"
	mkdir -p "$ct_sum_dir"
	printf '%s\n' '{"id":"blind","line":0,"stage":"done"}' \
		> "$ct_sum_dir/manifest.json"
	printf '%s\n' '{"channel":[{"id":0,"sip_local_port":5060}]}' \
		> "$ct_sum_dir/config.json"
	ct_sum_pcap_write > "$ct_sum_dir/wire.pcap"
	ucode "$ct_sum_gen" "$ct_sum_dir" >/dev/null 2>&1
	check_eq 'summary/ladder unknown unit address' '1 2' \
		"$(grep -c 'the unit address is unknown' "$ct_sum_dir/SUMMARY.txt") $(grep -c '^  [0-9:.]*  ?? ' "$ct_sum_dir/SUMMARY.txt")"

	# tcpdump reads an MGCP datagram as plain UDP, so the ladder of an MGCP
	# trace prints the length of each datagram on the MGCP port.
	ct_sum_dir=$tmp/summary-mgcp
	ct_sum_mgcp_pcap_write > "$ct_sum_dir/wire.pcap"
	ucode "$ct_sum_gen" "$ct_sum_dir" >/dev/null 2>&1
	check_eq 'summary/mgcp wire ladder' \
		'-> 198.51.100.10:2727 MGCP, 32 bytes
<- 198.51.100.10:2727 MGCP, 10 bytes' \
		"$(sed -n '/^  unit address/,/^  on the wire/p' \
			"$ct_sum_dir/SUMMARY.txt" |
			sed -n 's/^  [0-9:.]*  \(-[>] .*\)$/\1/p;s/^  [0-9:.]*  \([<]- .*\)$/\1/p' |
			tr -s ' ')"
}
calltrace_summary_tests

# The live archive carries real rings, so the timeline merges the log ring,
# the SIP ring and the snapshots there.
calltrace_summary_live_tests() {
	ct_sum_checks='live timeline fsm line
live timeline start snapshot
live timeline time order
live truncated rings name the offset
live truncated rings keep the sections'
	ct_sum_skip=''
	if ! command -v ucode >/dev/null 2>&1; then
		ct_sum_skip='no ucode command'
	elif ! ucode -e "import * as s from 'struct';" >/dev/null 2>&1; then
		ct_sum_skip='no ucode struct module'
	elif ! command -v tar >/dev/null 2>&1; then
		ct_sum_skip='no tar command'
	elif [ -z "$ct_live_archive" ] || [ ! -f "$ct_live_archive" ]; then
		ct_sum_skip='the live call trace leg produced no archive'
	fi
	if [ -n "$ct_sum_skip" ]; then
		while IFS= read -r ct_sum_name; do
			skip "summary/$ct_sum_name" "$ct_sum_skip"
		done <<EOF
$ct_sum_checks
EOF
		return
	fi

	mkdir -p "$tmp/summary-live"
	if ! tar -xzf "$ct_live_archive" -C "$tmp/summary-live"; then
		while IFS= read -r ct_sum_name; do
			skip "summary/$ct_sum_name" 'the live archive did not unpack'
		done <<EOF
$ct_sum_checks
EOF
		return
	fi
	ct_sum_dir=$tmp/summary-live/$ct_live_id
	ct_sum_report=$(ucode "$ct_sum_gen" "$ct_sum_dir" - 2>&1)
	ct_sum_timeline=$(printf '%s\n' "$ct_sum_report" |
		sed -n '/^TIMELINE$/,/^CALLS$/p')
	check_has 'summary/live timeline fsm line' '[fsm]' "$ct_sum_timeline"
	check_has 'summary/live timeline start snapshot' 'snapshot start' \
		"$ct_sum_timeline"
	# Every source of the timeline stamps in wall time, so the stamp as a
	# number orders each pair of a trace that does not cross midnight.
	ct_sum_stamps=$(printf '%s\n' "$ct_sum_timeline" |
		sed -n 's/^  \([0-9][0-9]:[0-9][0-9]:[0-9][0-9]\.[0-9][0-9][0-9]\)  .*/\1/p')
	ct_sum_prev=''
	ct_sum_order=ordered
	while IFS= read -r ct_sum_stamp; do
		[ -n "$ct_sum_stamp" ] || continue
		if [ -n "$ct_sum_prev" ] &&
			[ "$(printf '%s' "$ct_sum_prev" | tr -d ':.')" -gt \
			  "$(printf '%s' "$ct_sum_stamp" | tr -d ':.')" ]; then
			ct_sum_order="$ct_sum_prev before $ct_sum_stamp"
			break
		fi
		ct_sum_prev=$ct_sum_stamp
	done <<EOF
$ct_sum_stamps
EOF
	check_eq 'summary/live timeline time order' ordered "$ct_sum_order"

	ct_sum_cut=$tmp/summary-cut
	rm -rf "$ct_sum_cut"
	mkdir -p "$ct_sum_cut"
	cp "$ct_sum_dir/manifest.json" "$ct_sum_dir/snapshots.jsonl" \
		"$ct_sum_dir/config.json" "$ct_sum_cut/"
	head -c 201 "$ct_sum_dir/rings.pcapng" > "$ct_sum_cut/rings.pcapng"
	ct_sum_report=$(ucode "$ct_sum_gen" "$ct_sum_cut" - 2>&1)
	# The cut lands inside a block or between two of them, and the line
	# names the byte either way.
	check_eq 'summary/live truncated rings name the offset' 1 \
		"$(printf '%s\n' "$ct_sum_report" |
			grep -cE 'rings\.pcapng: .* offset [0-9]+')"
	check_eq 'summary/live truncated rings keep the sections' 10 \
		"$(printf '%s\n' "$ct_sum_report" |
			sed -n 's/^\([A-Z][A-Z ]*\)$/\1/p' | wc -l)"
}
calltrace_summary_live_tests

skip 'calltrace/wire capture' \
	'requires a target wire interface and capture privileges'

#
# show status, against a live state
#
run_cli seize 1
run_cli show status line 1
seized_out=$cli_out
check_eq 'show/status line 1 exit' 0 "$cli_rc"
# line 1 has no registrar target, so the seize gets the fast busy chain
check_has 'show/status line 1 detailed status' 'Fast busy' "$seized_out"
check_has 'show/status line 1 hook status' 'Offhook' "$seized_out"
check_has 'show/status line 1 call status' 'Disconnected' "$seized_out"
check_lacks 'show/status line 1 shows one line only' 'Line 2' "$seized_out"
run_cli release 1

run_cli show status line 2
check_has 'show/status line 2 hook status' 'Onhook' "$cli_out"
check_has 'show/status line 2 sip client status of a disabled line' 'Disabled' "$cli_out"

run_cli show status
check_has 'show/status spans line 1' 'Line 1' "$cli_out"
check_has 'show/status spans line 2' 'Line 2' "$cli_out"
sip_rendered=$(printf '%s\n' "$cli_out" | sed -n 's/^   SIP client status *//p' | sed -n 1p)
case $sip_rendered in
Up | Registering | Error_Registration | Quiescent)
	ok 'show/status renders a mapped SIP client status'
	;;
*)
	not_ok 'show/status renders a mapped SIP client status' "got [$sip_rendered]"
	;;
esac

#
# show statistics
#
run_cli show statistics
check_eq 'show/statistics exit' 0 "$cli_rc"
check_has 'show/statistics spans line 1' 'Line 1' "$cli_out"
check_has 'show/statistics spans line 2' 'Line 2' "$cli_out"
check_has 'show/statistics carries the packet totals' 'Packets sent' "$cli_out"
check_has 'show/statistics carries the call counters' 'Outgoing calls attempted' "$cli_out"

: > "$ubus_rec"
run_cli show statistics line 2
check_has 'show/statistics line 2 shows line 2' 'Line 2' "$cli_out"
check_lacks 'show/statistics line 2 shows one line only' 'Line 1' "$cli_out"
check_has 'show/statistics line 2 lets the daemon filter' \
	'call voice stats {"line":1}' "$(cat "$ubus_rec")"

: > "$ubus_rec"
run_cli show statistics
check_lacks 'show/statistics without a selector sends no filter' \
	'call voice stats {' "$(cat "$ubus_rec")"

# The refusal resolves from the span before any stats call, so the daemon's
# Invalid argument stays unreachable from the CLI.
: > "$ubus_rec"
run_cli show statistics line 9
check_eq 'show/statistics of an absent line exits 1' 1 "$cli_rc"
check_has 'show/statistics of an absent line names the span' \
	'this stack carries' "$cli_out"
check_lacks 'show/statistics of an absent line makes no stats call' \
	'call voice stats' "$(cat "$ubus_rec")"

# Every counter carries a value of this suite's choosing, distinct per line and
# per field, so a render that reads the wrong line or the wrong key is a failed
# comparison and not an equal pair of zeros.
cat > "$tmp/stats.json" <<'EOF'
{"stats":[
{"line":0,"in_received":11,"in_answered":12,"in_failed":13,
 "out_attempted":14,"out_answered":15,"out_failed":16,
 "totals":{"packets_sent":101,"packets_recv":102,"packets_lost":103,
           "bytes_sent":104,"bytes_recv":105}},
{"line":1,"in_received":21,"in_answered":22,"in_failed":23,
 "out_attempted":24,"out_answered":25,"out_failed":26,
 "totals":{"packets_sent":201,"packets_recv":202,"packets_lost":203,
           "bytes_sent":204,"bytes_recv":205}}
]}
EOF
ubus_stats_file=$tmp/stats.json

stat_value_get() {
	printf '%s\n' "$1" | sed -n "s/^   $2  *\\([0-9]*\\)\$/\\1/p" | sed -n "${3}p"
}

run_cli show statistics line 1
check_eq 'statistics/line 1 packets sent' '101' "$(stat_value_get "$cli_out" 'Packets sent' 1)"
check_eq 'statistics/line 1 packets lost' '103' "$(stat_value_get "$cli_out" 'Packets lost' 1)"
check_eq 'statistics/line 1 bytes received' '105' "$(stat_value_get "$cli_out" 'Bytes received' 1)"
check_eq 'statistics/line 1 outgoing calls attempted' '14' \
	"$(stat_value_get "$cli_out" 'Outgoing calls attempted' 1)"
check_eq 'statistics/line 1 incoming calls received' '11' \
	"$(stat_value_get "$cli_out" 'Incoming calls received' 1)"

run_cli show statistics line 2
check_eq 'statistics/line 2 packets sent' '201' "$(stat_value_get "$cli_out" 'Packets sent' 1)"
check_eq 'statistics/line 2 bytes received' '205' "$(stat_value_get "$cli_out" 'Bytes received' 1)"
check_eq 'statistics/line 2 outgoing calls attempted' '24' \
	"$(stat_value_get "$cli_out" 'Outgoing calls attempted' 1)"
check_eq 'statistics/line 2 incoming calls failed' '23' \
	"$(stat_value_get "$cli_out" 'Incoming calls failed' 1)"

run_cli show statistics
check_eq 'statistics/both lines keep line 1 packets sent first' '101' \
	"$(stat_value_get "$cli_out" 'Packets sent' 1)"
check_eq 'statistics/both lines keep line 2 packets sent second' '201' \
	"$(stat_value_get "$cli_out" 'Packets sent' 2)"
check_eq 'statistics/both lines keep line 1 outgoing answered first' '15' \
	"$(stat_value_get "$cli_out" 'Outgoing calls connected' 1)"
check_eq 'statistics/both lines keep line 2 outgoing answered second' '25' \
	"$(stat_value_get "$cli_out" 'Outgoing calls connected' 2)"

ubus_stats_file=''

run_cli show status line 99
check_eq 'refuse/show status of a line the stack has not got' 1 "$cli_rc"
check_has 'refuse/show status of an absent line names the span' 'this stack carries 2 line(s)' "$cli_out"

run_cli show statistics line 99
check_eq 'refuse/show statistics of a line the stack has not got' 1 "$cli_rc"

# The live selector hands the line to the live media query, whose own refusal
# names a line without a call.
run_cli show statistics live 1
check_eq 'show/statistics live of a line without a call exits 1' 1 "$cli_rc"
check_has 'show/statistics live of a line without a call says so' \
	'has no call' "$cli_out"

run_cli show statistics bogus
check_eq 'show/statistics refuses an unknown selector' 1 "$cli_rc"
check_has 'show/statistics names the two selectors' \
	'expected "line" or "live"' "$cli_out"

run_cli show raw
check_eq 'show/raw exit' 0 "$cli_rc"
check_has 'show/raw carries the status dump' 'VoIP Status' "$cli_out"
check_has 'show/raw carries the statistics dump' 'VoIP Statistics' "$cli_out"
check_has 'show/raw carries the call records' 'VoIP Call Records' "$cli_out"
check_has 'show/raw carries the loaded config' 'VoIP Loaded Config' "$cli_out"
check_has 'show/raw carries the uci parameters' 'voip.base' "$cli_out"

#
# stats_live, the live per-call media query
#
# The stub daemon reaches no far end, so no leg of it ever carries a running
# stream: a seized line holds no call slot at all, and a dialled number fails
# the moment it leaves the dialplan. Every row that needs
# a running query therefore reads a canned answer, and the rows that need a
# refusal read the daemon itself.
#
fake_bin=$tmp/fake
real_ubus=$(command -v ubus)
fake_live_file=''
fake_status_file=''
fake_line_test_file=''
fake_line_test_status=''
fake_line_test_positive=''
fake_line_test_missing_object=''
fake_voice_list_rc=''
export real_ubus fake_live_file fake_status_file fake_line_test_file
export fake_line_test_status fake_line_test_positive
export fake_line_test_missing_object fake_voice_list_rc

mkdir -p "$fake_bin"
cat > "$fake_bin/ubus" <<'EOF'
#!/bin/sh
case " $* " in
*" call voice stats_live "*)
	if [ -n "$fake_live_file" ]; then
		cat "$fake_live_file"
		exit 0
	fi
	;;
*" call voice status "*)
	if [ -n "$fake_status_file" ]; then
		cat "$fake_status_file"
		exit 0
	fi
	;;
*" call voice line_test "*)
	if [ -n "$fake_line_test_missing_object" ]; then
		printf 'Command failed: Not found\n' >&2
		exit 4
	fi
	if [ -n "$fake_line_test_status" ]; then
		case $fake_line_test_status in
		2) fake_line_test_reason='Invalid argument' ;;
		4) fake_line_test_reason='Not found' ;;
		5) fake_line_test_reason='No data' ;;
		7) fake_line_test_reason='Request timed out' ;;
		9) fake_line_test_reason='Unknown error' ;;
		esac
		for fake_line_test_arg do
			fake_line_test_json=$fake_line_test_arg
		done
		printf 'Command failed: ubus call voice line_test %s (%s)\n' \
			"$fake_line_test_json" "$fake_line_test_reason" >&2
		if [ "$fake_line_test_status" = 2 ]; then
			cat >&2 <<'EOF_USAGE'
Usage: ubus [<options>] <command> [arguments...]
Options:
 -s <socket>:           Set the unix domain socket to connect to
 -t <timeout>:          Set the timeout (in seconds) for a command to complete
 -S:                    Use simplified output (for scripts)
 -v:                    More verbose output
 -m <type>:             (for monitor): include a specific message type
                        (can be used more than once)
 -M <r|t> (for monitor): only capture received or transmitted traffic

Commands:
 - list [<path>]                        List objects
 - call <path> <method> [<message>]     Call an object method
 - subscribe <path> [<path>...]         Subscribe to object(s) notifications
 - listen [<path>...]                   Listen for events
 - send <type> [<message>]              Send an event
 - wait_for <object> [<object>...]  Wait for multiple objects to appear on ubus
 - monitor                              Monitor ubus traffic

EOF_USAGE
		fi
		if [ -n "$fake_line_test_positive" ]; then
			exit "$fake_line_test_status"
		fi
		exit "$((256 - fake_line_test_status))"
	fi
	if [ -n "$fake_line_test_file" ]; then
		cat "$fake_line_test_file"
		exit 0
	fi
	;;
*" list voice "*)
	if [ -n "$fake_voice_list_rc" ]; then
		exit "$fake_voice_list_rc"
	fi
	;;
esac
exec "$real_ubus" "$@"
EOF
chmod +x "$fake_bin/ubus"
path_saved=$PATH

run_cli
check_has 'stats_live/usage names the verb' 'stats_live <line> [slot]' "$cli_out"
check_has 'line_test/usage names the action' 'line_test <line> <test>' \
	"$cli_out"

: > "$ubus_rec"
run_cli stats_live two
check_eq 'stats_live/a non number line exits 1' 1 "$cli_rc"
# the whole line, so that a CLI carrying no stats_live verb at all cannot
# satisfy the row with its usage text
check_eq 'stats_live/a non number line prints one friendly line' \
	'voice-cli: stats_live: line "two" is not a line number, lines count from 1' \
	"$cli_out"
check_eq 'stats_live/a non number line makes no ubus call at all' '' "$(cat "$ubus_rec")"

: > "$ubus_rec"
run_cli stats_live 1 x
check_eq 'stats_live/a non number slot exits 1' 1 "$cli_rc"
check_has 'stats_live/a non number slot explains' 'slots count from 0' "$cli_out"
check_eq 'stats_live/a non number slot makes no ubus call at all' '' "$(cat "$ubus_rec")"

: > "$ubus_rec"
run_cli stats_live 2
live_idle_rec=$(cat "$ubus_rec")
check_eq 'stats_live/a line with no call exits 1' 1 "$cli_rc"
check_has 'stats_live/a line with no call names the line' 'line 2 has no call' "$cli_out"
# The whole recording of the refusal path, which pins the scope of the wider
# timeout: the status read the resolve makes keeps the script's own 5 s, and
# the query itself never leaves.
check_eq 'stats_live/the resolve reads status at the plain timeout' \
	'-t 5 call voice status' "$live_idle_rec"

# A seized line holds no call slot either: calls_status_dump()
# (src/proto/sip/call.c) emits a slot only while its call is in_use, and the
# stub daemon holds no call in use here, so this is the same refusal.
run_cli seize 1
run_cli stats_live 1
seized_live_out=$cli_out
seized_live_rc=$cli_rc
run_cli release 1
check_eq 'stats_live/a seized line exits 1' 1 "$seized_live_rc"
check_has 'stats_live/a seized line still carries no call slot' 'line 1 has no call' \
	"$seized_live_out"

: > "$ubus_rec"
run_cli stats_live 1 0
live_rec=$(cat "$ubus_rec")
check_eq 'stats_live/an explicit slot with no stream exits 1' 1 "$cli_rc"
check_has 'stats_live/an explicit slot with no stream names the verb' \
	'voice stats_live failed' "$cli_out"
check_has 'stats_live/an explicit slot with no stream carries the daemon refusal' \
	'Not found' "$cli_out"
check_has 'stats_live/an explicit slot reaches the daemon 0 based' \
	'call voice stats_live {"line":0,"slot":0}' "$live_rec"
# the daemon closes a query the DSP never completes after 5 s, so the client
# must wait longer than that or the caller reads the wrong side's timeout
check_has 'stats_live/the query outlives the daemon backstop' \
	'-t 10 call voice stats_live' "$live_rec"

# The slot lands in the json this script builds by hand, and 007 is not a json
# number: RFC 8259 forbids the leading zero. The json parser the build host
# links accepts it anyway, so the pin here is the wire text and not the answer
# that came back: a stricter parser on the far side must never read 007.
: > "$ubus_rec"
run_cli stats_live 1 007
live_zero_rec=$(cat "$ubus_rec")
check_eq 'leading-zero/stats_live slot 007 exits 1 on the daemon refusal' 1 "$cli_rc"
check_eq 'leading-zero/stats_live slot 007 reaches the daemon as a json number' \
	'-t 10 call voice stats_live {"line":0,"slot":7}' "$live_zero_rec"
check_has 'leading-zero/stats_live slot 007 names the verb' 'voice stats_live failed' "$cli_out"
check_has 'leading-zero/stats_live slot 007 carries the daemon refusal' \
	'Invalid argument' "$cli_out"

# lo carries ::1 inside the namespace, so the line can build its Contact.
# No registrar answers on purpose; the line reaches "trying" with its Contact.
# The address6 row reads "-" because the namespace has no netifd.
ipv6_ready=0
ipv6_saved=0
ipv6_skip=''

ipv6_prepare() {
	ip link set lo up 2>/dev/null || true
	ipv6_skip='loopback is not up for the IPv6 checks'
	ip -o link show lo 2>/dev/null | grep -qE '[<,]UP[,>]' || return 1
	ipv6_skip='cannot save the config before the IPv6 checks'
	cp "$tmp/cfg/voip" "$tmp/voip-before-ipv6" || return 1
	ipv6_saved=1
	ipv6_skip='cannot write the IPv6 config'
	uci set voip.base.ipv6_enabled=1 &&
		uci set voip.channel1.sip_proxy=::1 &&
		uci set voip.channel1.sip_outbound_proxy=::1 &&
		uci set voip.channel1.sip_domain=::1 &&
		uci set voip.channel1.sip_registrar=::1 &&
		uci commit voip || return 1
	ipv6_skip='the daemon did not answer the IPv6 reload'
	ubus -t 5 call voice reload >/dev/null 2>&1
}

ipv6_prepare && ipv6_ready=1

if [ "$ipv6_ready" = 1 ]; then
	ipv6_wait=0
	while [ "$ipv6_wait" -lt 50 ]; do
		ipv6_status=$(ubus -t 5 call voice status 2>/dev/null)
		if printf '%s' "$ipv6_status" |
			jq -e '(.lines[0].contact // "") | length > 0' >/dev/null; then
			break
		fi
		ipv6_wait=$((ipv6_wait + 1))
		sleep 0.1
	done
	check_eq 'ipv6/status reports the line family' 'ipv6' \
		"$(printf '%s' "$ipv6_status" | jq -r '.lines[0].family')"
	ipv6_contact=$(printf '%s' "$ipv6_status" |
		jq -r '.lines[0].contact // ""')
	check_has 'ipv6/status carries the bracketed Contact host' '@[::1]:' \
		"$ipv6_contact"

	# the one fixture line with a registrar target. No registrar answers on
	# ::1, so the line holds no live registration, and the seize of the dial
	# verb gets the fast busy chain with no digit collected. The unit and
	# end-to-end suites show a registered line collecting its digits.
	run_cli dial 1 5551234
	check_eq 'ipv6/dial line 1 with no registration seizes into the fast busy' \
		'CLEARING' "$(call_state_get 0)"
	check_eq 'ipv6/dial line 1 with no registration collects no digits' '' \
		"$(status_field_get '((.call // []) | map(select(.line == 0)))[0].digits // ""')"
	run_cli release 1
else
	skip 'ipv6/status reports the line family' "$ipv6_skip"
	skip 'ipv6/status carries the bracketed Contact host' "$ipv6_skip"
	skip 'ipv6/dial line 1 with no registration seizes into the fast busy' "$ipv6_skip"
	skip 'ipv6/dial line 1 with no registration collects no digits' "$ipv6_skip"
fi

if [ "$ipv6_ready" = 1 ] && [ "$uc_ready" = 1 ]; then
	run_uc line --line 0
	check_eq 'ipv6/cli line renders the family row' '0 ipv6' \
		"$uc_rc $(printf '%s\n' "$uc_out" |
			sed -n 's/^  family  *//p')"
	# the literal answers the SRV question itself, so the line that holds
	# targets reports the address answer beside the question it asked
	check_eq 'ipv6/cli line renders the dns answer row' '0 addr' \
		"$uc_rc $(printf '%s\n' "$uc_out" |
			sed -n 's/^  dns_answer  *//p')"
	ipv6_contact_row=$(printf '%s\n' "$uc_out" |
		sed -n 's/^  contact  *//p')
	if [ "$uc_rc" = 0 ] && [ -n "$ipv6_contact" ] &&
		[ "$ipv6_contact_row" = "$ipv6_contact" ]; then
		ok 'ipv6/cli line renders the whole Contact row'
	else
		not_ok 'ipv6/cli line renders the whole Contact row' \
			"exit $uc_rc, want [$ipv6_contact], got [$ipv6_contact_row]"
	fi
	run_uc status
	check_eq 'ipv6/cli status renders address6 as - with no netifd' '0 -' \
		"$uc_rc $(printf '%s\n' "$uc_out" |
			sed -n 's/^  address6  *//p')"
else
	if [ "$ipv6_ready" = 1 ]; then
		ipv6_skip=$uc_skip
	fi
	skip 'ipv6/cli line renders the family row' "$ipv6_skip"
	skip 'ipv6/cli line renders the dns answer row' "$ipv6_skip"
	skip 'ipv6/cli line renders the whole Contact row' "$ipv6_skip"
	skip 'ipv6/cli status renders address6 as - with no netifd' "$ipv6_skip"
fi

if [ "$ipv6_saved" = 1 ]; then
	if ! { uci revert voip &&
		cp "$tmp/voip-before-ipv6" "$tmp/cfg/voip" &&
		ubus -t 5 call voice reload >/dev/null 2>&1; }; then
		not_ok 'ipv6/restores the saved config' \
			'cannot clear staged values, restore the config, or reload it'
	fi
fi

# A present sip_port names the port of the next hop, so the default mode asks
# the address first and SRV only for a name with no address. The question
# word is addr_srv. The fixture proxy is a literal, so nothing goes on the
# wire, and the word comes from the derive alone.
port_ready=0
port_saved=0
port_skip=''

port_prepare() {
	port_skip='cannot save the config before the port checks'
	cp "$tmp/cfg/voip" "$tmp/voip-before-port" || return 1
	port_saved=1
	port_skip='cannot write the port config'
	uci set voip.channel1.sip_port=5060 && uci commit voip || return 1
	port_skip='the daemon did not answer the port reload'
	ubus -t 5 call voice reload >/dev/null 2>&1
}

port_prepare && port_ready=1

if [ "$port_ready" = 1 ]; then
	port_wait=0
	while [ "$port_wait" -lt 50 ]; do
		port_status=$(ubus -t 5 call voice status 2>/dev/null)
		if printf '%s' "$port_status" |
			jq -e '.lines[0].dns_lookup == "addr_srv"' >/dev/null; then
			break
		fi
		port_wait=$((port_wait + 1))
		sleep 0.1
	done
	check_eq 'port/status reports the address-then-SRV question' 'addr_srv' \
		"$(printf '%s' "$port_status" | jq -r '.lines[0].dns_lookup')"
	schema_valid_check 'schema/a capture with the addr_srv question validates' \
		"$(schema_capture_build "$port_status")"
else
	skip 'port/status reports the address-then-SRV question' "$port_skip"
	skip 'schema/a capture with the addr_srv question validates' "$port_skip"
fi

if [ "$port_ready" = 1 ] && [ "$uc_ready" = 1 ]; then
	run_uc line --line 0
	check_eq 'port/cli line renders the addr_srv question' '0 addr_srv' \
		"$uc_rc $(printf '%s\n' "$uc_out" |
			sed -n 's/^  dns_lookup  *//p')"
else
	if [ "$port_ready" = 1 ]; then
		port_skip=$uc_skip
	fi
	skip 'port/cli line renders the addr_srv question' "$port_skip"
fi

if [ "$port_saved" = 1 ]; then
	if ! { uci revert voip &&
		cp "$tmp/voip-before-port" "$tmp/cfg/voip" &&
		ubus -t 5 call voice reload >/dev/null 2>&1; }; then
		not_ok 'port/restores the saved config' \
			'cannot clear staged values, restore the config, or reload it'
	fi
fi

# The slot the caller left out comes from the "calls" array voice status
# carries per line, which the stub daemon never fills, so the resolve reads a
# status of this suite's own making and the query itself still reaches the
# daemon.
cat > "$tmp/status_one_call.json" <<'EOF'
{"call":[{"line":0,"state":"CONNECTED","calls":[{"slot":1,"ui_slot":1,"state":"CONNECTED"}]},
	 {"line":1,"state":"IDLE","calls":[]}]}
EOF
PATH=$fake_bin:$PATH
fake_status_file=$tmp/status_one_call.json
: > "$ubus_rec"
run_cli stats_live 1
fake_status_file=''
PATH=$path_saved
check_eq 'stats_live/slot auto-resolve exits 1 on the daemon refusal' 1 "$cli_rc"
check_has 'stats_live/slot auto-resolve names the verb' 'voice stats_live failed' "$cli_out"
check_has 'stats_live/slot auto-resolve sends the resolved slot' \
	'call voice stats_live {"line":0,"slot":1}' "$(cat "$ubus_rec")"

# The slot the resolve reads comes from the daemon, which never spells a
# leading zero; the normalization still covers that path so both routes reach
# the json in one shape.
cat > "$tmp/status_zero_slot.json" <<'EOF'
{"call":[{"line":0,"state":"CONNECTED","calls":[{"slot":7,"ui_slot":"007","state":"CONNECTED"}]}]}
EOF
PATH=$fake_bin:$PATH
fake_status_file=$tmp/status_zero_slot.json
: > "$ubus_rec"
run_cli stats_live 1
fake_status_file=''
PATH=$path_saved
check_has 'leading-zero/a resolved slot carrying one reaches the daemon as a json number' \
	'call voice stats_live {"line":0,"slot":7}' "$(cat "$ubus_rec")"

cat > "$tmp/status_two_calls.json" <<'EOF'
{"call":[{"line":0,"state":"TWO_CALLS",
	  "calls":[{"slot":0,"ui_slot":0,"state":"HOLDING"},
		   {"slot":1,"ui_slot":1,"state":"CONNECTED"}]}]}
EOF
PATH=$fake_bin:$PATH
fake_status_file=$tmp/status_two_calls.json
: > "$ubus_rec"
run_cli stats_live 1
fake_status_file=''
PATH=$path_saved
check_eq 'stats_live/two calls exits 1' 1 "$cli_rc"
check_has 'stats_live/two calls names both slots and their states' \
	'slot 0 (HOLDING), slot 1 (CONNECTED)' "$cli_out"
check_has 'stats_live/two calls asks for the explicit slot' 'name the slot' "$cli_out"
check_eq 'stats_live/two calls sends no query' '' "$(cat "$ubus_rec")"

# The resolved slot reaches the ubus json this script builds by hand, so it
# passes the same gate the dialled number does: a crafted slot could otherwise
# close the "slot" value and append a second "line" key, which the daemon
# would prefer, and a status entry with no slot at all would send "null".
cat > "$tmp/status_crafted_slot.json" <<'EOF'
{"call":[{"line":0,"state":"CONNECTED",
	  "calls":[{"slot":0,"ui_slot":"0,\"line\":1,\"z\":\"","state":"CONNECTED"}]}]}
EOF
PATH=$fake_bin:$PATH
fake_status_file=$tmp/status_crafted_slot.json
: > "$ubus_rec"
run_cli stats_live 1
fake_status_file=''
PATH=$path_saved
check_eq 'stats_live/a crafted status slot exits 1' 1 "$cli_rc"
check_has 'stats_live/a crafted status slot names the refusal' \
	'is not a slot number' "$cli_out"
check_eq 'stats_live/a crafted status slot sends no query' '' "$(cat "$ubus_rec")"

# A line that carried a transfer holds two legs under one name. Both entries
# below are real: they are the "voice status" the end-to-end transferee flow
# recorded once the child had taken the parent's name over
# (test/e2e/run_e2e.sh, the swapped assertion). The leg that speaks for the
# name is the one without the "shadow" key, and the name it answers to is
# "ui_slot" and not "slot": the owner sits on physical slot 1 and wears UI
# slot 0, which is what the daemon looks the query up by.
cat > "$tmp/status_swapped.json" <<'EOF'
{"call":[{"line":0,"state":"CONNECTED",
	  "calls":[{"slot":0,"ui_slot":0,"shadow":true,"state":"active"},
		   {"slot":1,"ui_slot":0,"state":"active"}]}]}
EOF
PATH=$fake_bin:$PATH
fake_status_file=$tmp/status_swapped.json
: > "$ubus_rec"
run_cli stats_live 1
fake_status_file=''
PATH=$path_saved
check_eq 'stats_live/a shadowed pair resolves to one call' 1 "$cli_rc"
check_has 'stats_live/a shadowed pair queries the owner leg by its UI name' \
	'call voice stats_live {"line":0,"slot":0}' "$(cat "$ubus_rec")"
check_lacks 'stats_live/a shadowed pair asks for no explicit slot' \
	'name the slot' "$cli_out"

# The window with no owner at all is the other half of the same status:
# between the child INVITE and its answer the parent is suppressed and the
# child holds no name yet, so both legs read as shadows. The resolve reports no
# call for it, and an operator who wants that window names the slot.
cat > "$tmp/status_shadow_pair.json" <<'EOF'
{"call":[{"line":0,"state":"CONNECTED",
	  "calls":[{"slot":0,"ui_slot":0,"shadow":true,"state":"active"},
		   {"slot":1,"ui_slot":1,"shadow":true,"state":"proceeding"}]}]}
EOF
PATH=$fake_bin:$PATH
fake_status_file=$tmp/status_shadow_pair.json
: > "$ubus_rec"
run_cli stats_live 1
fake_status_file=''
PATH=$path_saved
check_eq 'stats_live/a pair with no owner exits 1' 1 "$cli_rc"
check_has 'stats_live/a pair with no owner names the line' 'line 1 has no call' "$cli_out"
check_eq 'stats_live/a pair with no owner sends no query' '' "$(cat "$ubus_rec")"

cat > "$tmp/status_no_slot.json" <<'EOF'
{"call":[{"line":0,"state":"CONNECTED","calls":[{"state":"CONNECTED"}]}]}
EOF
PATH=$fake_bin:$PATH
fake_status_file=$tmp/status_no_slot.json
: > "$ubus_rec"
run_cli stats_live 1
fake_status_file=''
PATH=$path_saved
check_eq 'stats_live/a status entry with no slot exits 1' 1 "$cli_rc"
check_has 'stats_live/a status entry with no slot names what arrived' \
	'reported slot "null"' "$cli_out"
check_eq 'stats_live/a status entry with no slot sends no query' '' "$(cat "$ubus_rec")"

# The header arithmetic runs on the reply's own line, so a reply carrying a
# line the shell cannot add to must be refused by name: the bare "$(( ))" would
# print a shell diagnostic, exit 2 and say nothing about the daemon.
cat > "$tmp/stats_live_bad_line.json" <<'EOF'
{"line":"x","slot":0,"media":{"packets_sent":1}}
EOF
PATH=$fake_bin:$PATH
fake_live_file=$tmp/stats_live_bad_line.json
run_cli stats_live 1 0
fake_live_file=''
PATH=$path_saved
check_eq 'stats_live/a reply with a non number line exits 1' 1 "$cli_rc"
check_eq 'stats_live/a reply with a non number line names what arrived' \
	'voice-cli: stats_live: the daemon answered line "x", which is not a line number' \
	"$cli_out"

# The reply's own line reaches that same arithmetic, where a leading zero reads
# as octal: a reply naming line "010" is line 11 of the CLI and not line 9.
cat > "$tmp/stats_live_zero_line.json" <<'EOF'
{"line":"010","slot":0,"media":{"packets_sent":1}}
EOF
PATH=$fake_bin:$PATH
fake_live_file=$tmp/stats_live_zero_line.json
run_cli stats_live 1 0
fake_live_file=''
PATH=$path_saved
check_eq 'leading-zero/a reply line carrying one exits 0' 0 "$cli_rc"
check_has 'leading-zero/a reply line carrying one renders its decimal value' \
	'Line 11 slot 0' "$cli_out"

# The reply of a query that did run, with its own line and slot: the render
# must read those two and not the arguments, and must carry every key of the
# media table in the order the daemon emitted them.
cat > "$tmp/stats_live.json" <<'EOF'
{"line":1,"slot":1,"media":{"packets_sent":4242,"packets_recv":4141,
 "packets_lost":7,"jitter_avg":19,"mos_lq":"4.32"}}
EOF
stats_live_want=$(cat <<'EOF'
Line 2 slot 1
   packets_sent             4242
   packets_recv             4141
   packets_lost             7
   jitter_avg               19
   mos_lq                   4.32
EOF
)
PATH=$fake_bin:$PATH
fake_live_file=$tmp/stats_live.json
# the arguments name line 1 slot 0 and the reply names line 1 slot 1 (0 based),
# so a render that read the arguments would print "Line 2 slot 0" here
run_cli stats_live 1 0
fake_live_file=''
PATH=$path_saved
check_eq 'stats_live/success exit' 0 "$cli_rc"
check_eq 'stats_live/success renders the reply' "$stats_live_want" "$cli_out"

: > "$ubus_rec"
run_cli line_test two gr909
check_eq 'line_test/a non-number line exits with status one' 1 "$cli_rc"
check_eq 'line_test/a non-number line prints one friendly line' \
	'voice-cli: line_test: line "two" is not a line number, lines count from 1' \
	"$cli_out"
check_eq 'line_test/a non-number line makes no ubus call' '' \
	"$(cat "$ubus_rec")"

: > "$ubus_rec"
run_cli line_test 1 unknown
check_eq 'line_test/an unknown test name exits with status one' 1 "$cli_rc"
line_test_name_error='voice-cli: line_test: test "unknown" is not known;'
line_test_name_tail=' expected gr909, roh, capacitance or loop'
line_test_name_error=$line_test_name_error$line_test_name_tail
check_eq 'line_test/an unknown test name prints one friendly line' \
	"$line_test_name_error" "$cli_out"
check_eq 'line_test/an unknown test name makes no ubus call' '' \
	"$(cat "$ubus_rec")"

# The negative voltage and open resistance pin JSON types. Populated header
# fields pin their omission from the CLI output.
cat > "$tmp/line_test_complete.json" <<'EOF'
{"line":0,"test":"gr909","record":{"version":1,"test_id":0,"code":0,
 "valid":32767,"vdc_tr_mv":-25,"vdc_tg_mv":13,"vdc_rg_mv":14,
 "vac_tr_mvrms":15,"vac_tg_mvrms":16,"vac_rg_mvrms":17,
 "r_tr_ohm":"open","r_tg_ohm":19,"r_rg_ohm":20,"roh_offhook":false,
 "roh_res_fault":true,"ren_milli":404},"status":"complete"}
EOF
line_test_complete_want=$(cat <<'EOF'
status=complete
vdc_tr_mv=-25
vdc_tg_mv=13
vdc_rg_mv=14
vac_tr_mvrms=15
vac_tg_mvrms=16
vac_rg_mvrms=17
r_tr_ohm=open
r_tg_ohm=19
r_rg_ohm=20
roh_offhook=false
roh_res_fault=true
ren_milli=404
EOF
)
PATH=$fake_bin:$PATH
fake_line_test_file=$tmp/line_test_complete.json
run_cli line_test 1 gr909
fake_line_test_file=''
PATH=$path_saved
check_eq 'line_test/a complete reply exits with status zero' 0 "$cli_rc"
check_eq 'line_test/a complete reply renders every public record field' \
	"$line_test_complete_want" "$cli_out"

# The failed fixture distinguishes the top-level code from the hidden record.
cat > "$tmp/line_test_failed.json" <<'EOF'
{"line":0,"test":"gr909","record":{"version":1,"test_id":0,"code":7,
 "valid":0},"status":"failed","code":7}
EOF
line_test_failed_want=$(cat <<'EOF'
status=failed
code=7
EOF
)
PATH=$fake_bin:$PATH
fake_line_test_file=$tmp/line_test_failed.json
run_cli line_test 1 gr909
fake_line_test_file=''
PATH=$path_saved
check_eq 'line_test/a failed reply exits with status zero' 0 "$cli_rc"
check_eq 'line_test/a failed reply renders its status and code' \
	"$line_test_failed_want" "$cli_out"

# The populated short head pins omission of malformed record fields.
cat > "$tmp/line_test_malformed.json" <<'EOF'
{"line":0,"test":"loop","record":{"version":1,"test_id":3,"code":7},
 "status":"malformed","length":7}
EOF
line_test_malformed_want=$(cat <<'EOF'
status=malformed
length=7
EOF
)
PATH=$fake_bin:$PATH
fake_line_test_file=$tmp/line_test_malformed.json
run_cli line_test 1 loop
fake_line_test_file=''
PATH=$path_saved
check_eq 'line_test/a malformed reply exits with status zero' 0 "$cli_rc"
check_eq 'line_test/a malformed reply renders only status and length' \
	"$line_test_malformed_want" "$cli_out"

# An empty successful reply pins the gate before status output.
: > "$tmp/line_test_empty.json"
PATH=$fake_bin:$PATH
fake_line_test_file=$tmp/line_test_empty.json
run_cli line_test 1 roh
fake_line_test_file=''
PATH=$path_saved
check_eq 'line_test/an empty reply exits with status one' 1 "$cli_rc"
check_eq 'line_test/an empty reply prints one friendly line' \
	'voice-cli: line_test: the daemon answered no known status' "$cli_out"

# An invalid JSON reply pins suppression of the parser diagnostic.
cat > "$tmp/line_test_unparseable.json" <<'EOF'
{
EOF
PATH=$fake_bin:$PATH
fake_line_test_file=$tmp/line_test_unparseable.json
run_cli line_test 1 roh
fake_line_test_file=''
PATH=$path_saved
check_eq 'line_test/an unparseable reply exits with status one' 1 "$cli_rc"
check_eq 'line_test/an unparseable reply prints one friendly line' \
	'voice-cli: line_test: the daemon answered no known status' "$cli_out"

line_test_fake_run() {
	fake_line_test_status=$1
	fake_line_test_positive=$2
	fake_voice_list_rc=$3
	PATH=$fake_bin:$PATH
	run_cli line_test 1 roh
	PATH=$path_saved
	fake_line_test_status=''
	fake_line_test_positive=''
	fake_voice_list_rc=''
}

line_test_fake_run 2 1 ''
check_eq 'line_test/an invalid argument exits with status one' 1 "$cli_rc"
check_eq 'line_test/an invalid argument prints one friendly line' \
	'voice-cli: line_test: the line or test is not known to the daemon' \
	"$cli_out"

line_test_fake_run 4 1 0
check_eq 'line_test/a missing configured line exits with status one' 1 "$cli_rc"
check_eq 'line_test/a missing configured line prints one friendly line' \
	'voice-cli: line_test: line 1 is not configured' "$cli_out"

fake_line_test_missing_object=1
fake_voice_list_rc=4
PATH=$fake_bin:$PATH
run_cli line_test 1 roh
PATH=$path_saved
fake_line_test_missing_object=''
fake_voice_list_rc=''
check_eq 'line_test/a stopped daemon exits with status one' 1 "$cli_rc"
check_eq 'line_test/a stopped daemon prints the daemon-down line' \
	'voice-cli: the voice daemon does not answer ubus' "$cli_out"

line_test_fake_run 5 1 ''
check_eq 'line_test/a no-data refusal exits with status one' 1 "$cli_rc"
line_test_refusal="voice-cli: line_test: line 1 is down, busy with a call or a \
test, or the backend has no line test"
check_eq 'line_test/a no-data refusal prints one friendly line' \
	"$line_test_refusal" "$cli_out"

line_test_fake_run 7 1 ''
check_eq 'line_test/a daemon timeout exits with status one' 1 "$cli_rc"
check_eq 'line_test/a daemon timeout prints one friendly line' \
	'voice-cli: line_test: line 1 test timed out in the daemon' "$cli_out"

line_test_fake_run 9 1 ''
check_eq 'line_test/an unreadable record exits with status one' 1 "$cli_rc"
line_test_unreadable="voice-cli: line_test: the backend failed the test or the \
record is unreadable"
check_eq 'line_test/an unreadable record prints one friendly line' \
	"$line_test_unreadable" "$cli_out"

line_test_fake_run 2 '' ''
check_eq 'line_test/a captured invalid argument exits with status one' \
	1 "$cli_rc"
check_eq 'line_test/a captured invalid argument prints one friendly line' \
	'voice-cli: line_test: the line or test is not known to the daemon' \
	"$cli_out"

line_test_fake_run 4 '' 0
check_eq 'line_test/a captured missing line exits with status one' 1 "$cli_rc"
check_eq 'line_test/a captured missing line prints one friendly line' \
	'voice-cli: line_test: line 1 is not configured' "$cli_out"

line_test_fake_run 5 '' ''
check_eq 'line_test/a captured no-data refusal exits with status one' \
	1 "$cli_rc"
check_eq 'line_test/a captured no-data refusal prints one friendly line' \
	"$line_test_refusal" "$cli_out"

line_test_fake_run 7 '' ''
check_eq 'line_test/a captured timeout exits with status one' 1 "$cli_rc"
check_eq 'line_test/a captured timeout prints one friendly line' \
	'voice-cli: line_test: line 1 test timed out in the daemon' "$cli_out"

line_test_fake_run 9 '' ''
check_eq 'line_test/a captured unreadable record exits with status one' \
	1 "$cli_rc"
check_eq 'line_test/a captured unreadable record prints one friendly line' \
	"$line_test_unreadable" "$cli_out"

fake_line_test_file=''
PATH=$fake_bin:$PATH
harness_ubus_socket=$tmp/no-daemon.sock
run_cli line_test 1 roh
harness_ubus_socket=$ubus_socket
PATH=$path_saved
check_eq 'line_test/a transport failure exits with status one' 1 "$cli_rc"
check_eq 'line_test/a transport failure uses the generic ubus line' \
	'voice-cli: voice line_test failed: Failed to connect to ubus' "$cli_out"

: > "$ubus_rec"
run_cli line_test 1 roh
line_test_stub_want=$(cat <<'EOF'
status=malformed
length=19
EOF
)
check_eq 'line_test/the stub reply renders only status and length' \
	"$line_test_stub_want" "$cli_out"
check_eq 'line_test/the call uses the zero-based index and long timeout' \
	'-t 110 call voice line_test {"line":0,"test":"roh"}' \
	"$(cat "$ubus_rec")"

#
# The vocabulary the CLI and the arc.voip web handler share
#
cat > "$tmp/uc_table.awk" <<'EOF'
$0 == "const " name " = {" { inside = 1; next }
inside && $0 == "};" { inside = 0 }
inside {
	line = $0
	sub(/^[ \t]+/, "", line)
	if (line ~ /^\/\//) next
	if (line == "") next
	colon = index(line, ":")
	if (colon == 0) next
	key = substr(line, 1, colon - 1)
	value = substr(line, colon + 1)
	sub(/^[ \t]+/, "", value)
	sub(/,$/, "", value)
	gsub(/^['"]|['"]$/, "", value)
	print key "=" value
}
EOF

uc_table_dump() {
	awk -v name="$1" -f "$tmp/uc_table.awk" "$arc_voip_uc" | sort
}

sh_table_dump() {
	sed -n "s/^$1='\\(.*\\)'\$/\\1/p" "$voice_cli_src" | tr '|' '\n' | sort
}

vocabulary_check() {
	if [ ! -r "$arc_voip_uc" ]; then
		skip "vocabulary/$2" "no arc.voip handler at $arc_voip_uc"
		return
	fi

	vocab_want=$(uc_table_dump "$1")
	vocab_got=$(sh_table_dump "$2")
	if [ -z "$vocab_want" ]; then
		not_ok "vocabulary/$2" "table $1 not found in $arc_voip_uc"
		return
	fi
	check_eq "vocabulary/$2" "$vocab_want" "$vocab_got"
}

vocabulary_check SIP_CLIENT_STATUS sip_client_status_map
vocabulary_check LINE_CALL_STATUS line_call_status_map
vocabulary_check LINE_DETAILED_STATUS line_detailed_status_map
vocabulary_check CLEARING_DETAILED_STATUS clearing_status_map
vocabulary_check HOOK_STATE hook_state_map

#
# The commands that only the other voice stack serves
#
unsupported_check() {
	unsup_name=$1
	shift
	run_cli "$@"
	if [ "$cli_rc" -ne 1 ]; then
		not_ok "unsupported/$unsup_name" "exit want 1 got $cli_rc, output [$cli_out]"
		return
	fi
	check_has "unsupported/$unsup_name" 'not supported on the uvoice stack' "$cli_out"
}

unsupported_check 'show reg' show reg line 1
unsupported_check 'show ram' show ram line 1

UVOICE_TLS_KEYLOG=$tmp/ssl.keylog
UVOICE_TLS_STATUS=$tmp/voip_tls_stat.txt
export UVOICE_TLS_KEYLOG UVOICE_TLS_STATUS

run_cli show tlskey
check_eq 'show/tlskey without a key log exit' 0 "$cli_rc"
check_eq 'show/tlskey without a key log says so' 'TLS key log not found' "$cli_out"
printf 'CLIENT_RANDOM 00 11\n' > "$UVOICE_TLS_KEYLOG"
run_cli show tlskey
check_eq 'show/tlskey prints the key log' 'CLIENT_RANDOM 00 11' "$cli_out"

run_cli show tls-status
check_eq 'show/tls-status without a status file exit' 0 "$cli_rc"
check_eq 'show/tls-status without a status file says so' \
	'TLS certificate status not found' "$cli_out"
printf '%s' 'Certificate updated and applied' > "$UVOICE_TLS_STATUS"
run_cli show tls-status
check_eq 'show/tls-status prints the helper status' \
	'Certificate updated and applied' "$cli_out"

unset UVOICE_TLS_KEYLOG UVOICE_TLS_STATUS
unsupported_check 'set global log_module' set global log_module 5 4
unsupported_check 'set global log_preset' set global log_preset 2

#
# Config writes land in the scratch confdir
#
run_cli set line 1 sip_username 5559999
check_eq 'set/line exit' 0 "$cli_rc"
check_eq 'set/line writes the scratch config' '5559999' \
	"$(uci -q get voip.channel1.sip_username)"

run_cli set global local_interface br-lan
check_eq 'set/global writes the scratch config' 'br-lan' \
	"$(uci -q get voip.base.local_interface)"

run_cli set global tls_keylog_enabled 1
check_eq 'set/global tls_keylog_enabled exit' 0 "$cli_rc"
check_eq 'set/global tls_keylog_enabled writes the scratch config' '1' \
	"$(uci -q get voip.base.tls_keylog_enabled)"

run_cli set global tls_cert_path http://x/y
check_eq 'set/global tls_cert_path exit' 0 "$cli_rc"
check_eq 'set/global tls_cert_path writes the scratch config' 'http://x/y' \
	"$(uci -q get voip.base.tls_cert_path)"
run_cli set global tls_cert_username certuser
check_eq 'set/global tls_cert_username writes the scratch config' 'certuser' \
	"$(uci -q get voip.base.tls_cert_username)"
run_cli set global tls_cert_password certpass
check_eq 'set/global tls_cert_password writes the scratch config' 'certpass' \
	"$(uci -q get voip.base.tls_cert_password)"
run_cli set global tls_cert_path none
check_eq 'set/global tls_cert_path none exit' 0 "$cli_rc"
check_eq 'set/global tls_cert_path none clears the option' '' \
	"$(uci -q get voip.base.tls_cert_path)"

check_eq 'fixture/region list matches the seeded value' "$region_fixture_supported" \
	"$(uci -q get voip.base.supported_regions)"

run_cli set global region US
check_eq 'set/global region accepts a supported region' 'US' \
	"$(uci -q get voip.base.region)"

run_cli set global region U
check_eq 'set/global region refuses a partial entry' 1 "$cli_rc"
check_eq 'set/global region keeps the committed value after a partial entry' 'US' \
	"$(uci -q get voip.base.region)"

run_cli set global region ZZ
check_eq 'set/global region refuses an unsupported region' 1 "$cli_rc"

# A hand-written list can use spaces, so syntax validation must reject a
# candidate that spans two adjacent entries.
run_cli set global region 'US EU'
check_eq 'set/global region refuses a multi word value' 1 "$cli_rc"
check_eq 'set/global region keeps the committed value' 'US' "$(uci -q get voip.base.region)"

run_cli set global region GB
check_eq 'set/global region accepts a name from the comma form' 'GB' \
	"$(uci -q get voip.base.region)"

run_cli set global region 'US,EU'
check_eq 'set/global region refuses a comma-joined pair' 1 "$cli_rc"
check_eq 'set/global region keeps the committed value after a comma-joined pair' 'GB' \
	"$(uci -q get voip.base.region)"

uci set voip.base.supported_regions=','
uci commit voip
run_cli set global region DEU
check_eq 'set/global region accepts any well-formed name when the list is only separators' \
	'DEU' "$(uci -q get voip.base.region)"
uci set "voip.base.supported_regions=$region_fixture_supported"
uci commit voip

region_tab=$(printf '\t')
uci set "voip.base.supported_regions=US${region_tab}GB"
uci commit voip
run_cli set global region GB
check_eq 'set/global region accepts a tab-separated list' 'GB' \
	"$(uci -q get voip.base.region)"
uci set "voip.base.supported_regions=$region_fixture_supported"
uci commit voip

uci set voip.base.supported_regions='US EU'
uci commit voip
run_cli set global region EU
check_eq 'set/global region accepts a hand-written space list' 'EU' \
	"$(uci -q get voip.base.region)"
uci set "voip.base.supported_regions=$region_fixture_supported"
uci commit voip

uci -q delete voip.base.supported_regions
uci commit voip
run_cli set global region DEU
check_eq 'set/global region accepts any well-formed name when the list is unset' 'DEU' \
	"$(uci -q get voip.base.region)"
uci set "voip.base.supported_regions=$region_fixture_supported"
uci commit voip

uci -q delete voip.base.supported_regions
uci commit voip
run_cli set global region 'x y'
check_eq 'set/global region still refuses a malformed name when the list is unset' 1 \
	"$cli_rc"
check_eq 'set/global region keeps the committed value after a malformed name' 'DEU' \
	"$(uci -q get voip.base.region)"
uci set "voip.base.supported_regions=$region_fixture_supported"
uci commit voip

# The profile form stages three uci writes; an option name uci cannot parse
# fails the third, and the first two must not survive to be committed by the
# next command that succeeds.
run_cli set profile resid bad-name 1
check_eq 'set/a failed write exits 1' 1 "$cli_rc"
check_eq 'set/a failed write leaves no staged change' '' "$(uci -q changes voip)"
check_eq 'set/a failed write leaves no section behind' '' "$(uci -q get voip.profile_resid)"
run_cli set global local_interface br-wan
check_eq 'set/the next commit carries no residue of the failed write' '' \
	"$(uci -q get voip.profile_resid)"

run_cli set line 9 sip_username 5550000
check_eq 'set/line refuses a line the config has not got' 1 "$cli_rc"

run_cli set line 1_g711u enabled 1
check_eq 'set/line accepts a codec sub section' '1' \
	"$(uci -q get voip.channel1_g711u.enabled)"

run_cli set profile lab sip_domain example.net
check_eq 'set/profile writes the option' 'example.net' \
	"$(uci -q get voip.profile_lab.sip_domain)"
check_eq 'set/profile writes the id' 'lab' "$(uci -q get voip.profile_lab.id)"

run_cli apply profile lab channel2
check_eq 'apply/profile exit' 0 "$cli_rc"
check_eq 'apply/profile copies the option onto the line' 'example.net' \
	"$(uci -q get voip.channel2.sip_domain)"
check_eq 'apply/profile keeps the line id' '1' "$(uci -q get voip.channel2.id)"

run_cli apply profile nosuch channel2
check_eq 'apply/profile refuses an absent profile' 1 "$cli_rc"

run_cli apply profile lab base
check_eq 'apply/profile refuses a target that is not a line' 1 "$cli_rc"
check_eq 'apply/profile leaves the base section alone' '' "$(uci -q get voip.base.sip_domain)"

run_cli apply profile lab profile_lab
check_eq 'apply/profile refuses a profile as its own target' 1 "$cli_rc"

# uci show quotes and escapes what it prints, so a value carrying a quote used
# to reach the line mangled
run_cli set profile quoted sip_domain "it's"
check_eq 'set/profile stores a value carrying a quote' "it's" \
	"$(uci -q get voip.profile_quoted.sip_domain)"
run_cli apply profile quoted channel2
check_eq 'apply/profile carries a quoted value across verbatim' "it's" \
	"$(uci -q get voip.channel2.sip_domain)"

#
# The uci read surfaces, and the loaded views the formatter renders for the
# klish tree. A named option keeps the uci lookup; the section itself renders
# through the formatter with the display base at 1, so the klish user reads
# the line numbers the CLI counts with.
#
run_cli show config
check_has 'show/config reads the config' 'voip.base' "$cli_out"

run_cli show global channels
check_has 'show/global with a field reads one option' 'voip.base.channels=' "$cli_out"

run_cli show channel1 sip_username
check_has 'show/channel1 with a field reads one option' \
	'voip.channel1.sip_username=' "$cli_out"

run_cli show channel9
check_eq 'show/channel refuses a channel the config has not got' 1 "$cli_rc"

run_cli show channel0
check_eq 'show/channel refuses channel 0, lines count from 1' 1 "$cli_rc"

run_cli_uc show global
uc_check_eq 'show/global exit' 0 "$cli_rc"
uc_check_has 'show/global renders the loaded configuration' \
	'uvoice Configuration' "$cli_out"
uc_check_lacks 'show/global prints no uci dump' 'voip.base' "$cli_out"

run_cli_uc show channel1
uc_check_eq 'show/channel1 exit' 0 "$cli_rc"
uc_check_has 'show/channel1 renders the line view as line 1' 'Line 1' "$cli_out"
uc_check_lacks 'show/channel1 prints no 0-based line' 'Line 0' "$cli_out"
uc_check_has 'show/channel1 carries the canceller section' 'Echo canceller' \
	"$cli_out"

run_cli_uc show channel1 config
uc_check_has 'show/channel1 config renders the loaded channel as 1' \
	'Channel 1' "$cli_out"
uc_check_has 'show/channel1 config folds the unset lec_control' \
	'lec_control                          (unset)' "$cli_out"

run_cli_uc show daemon
uc_check_eq 'show/daemon exit' 0 "$cli_rc"
uc_check_has 'show/daemon renders the daemon summary' 'uvoice Status' "$cli_out"
uc_check_has 'show/daemon names the HAL backend' 'backend                stub' \
	"$cli_out"

# The account table holds line 1 alone: channel 2 names no user, so it has no
# account. The row starts with the 1-based number.
run_cli_uc show lines
uc_check_eq 'show/lines exit' 0 "$cli_rc"
uc_check_eq 'show/lines counts the account as line 1' 1 \
	"$(printf '%s\n' "$cli_out" | grep -c '^1 ')"
uc_check_eq 'show/lines prints no line 0' 0 \
	"$(printf '%s\n' "$cli_out" | grep -c '^0 ')"

run_cli_uc show calls
uc_check_eq 'show/calls renders both lines as 1 and 2' 2 \
	"$(printf '%s\n' "$cli_out" | grep -c '^[12] ')"

run_cli_uc show monitor
uc_check_has 'show/monitor renders the never row of line 1' '1    never' \
	"$cli_out"

run_cli_uc show cdr
uc_check_eq 'show/cdr exit' 0 "$cli_rc"
run_cli_uc show cdr last 1
uc_check_eq 'show/cdr last exit' 0 "$cli_rc"
run_cli_uc show cdr seq 999999
uc_check_eq 'show/cdr seq of an absent record exits 1' 1 "$cli_rc"
uc_check_has 'show/cdr seq of an absent record says so' \
	'No call record with seq 999999' "$cli_out"

run_cli show cdr bogus
check_eq 'show/cdr refuses an unknown selector' 1 "$cli_rc"
check_has 'show/cdr names the two selectors' 'expected "last" or "seq"' "$cli_out"

run_cli show cdr last x
check_eq 'show/cdr last refuses a count that is not a number' 1 "$cli_rc"

#
# show log
#
: > "$logread_rec"
run_cli show log
check_eq 'show/log exit' 0 "$cli_rc"
check_eq 'show/log filters on the uvoice tag' '-e uvoice' "$(cat "$logread_rec")"
check_has 'show/log prints the log' 'MARKER_ALPHA' "$cli_out"

: > "$logread_rec"
run_cli show log -l 5
check_eq 'show/log last passes the count through' '-e uvoice -l 5' "$(cat "$logread_rec")"

: > "$logread_rec"
run_cli show log -m MARKER_BETA
check_has 'show/log match keeps the matching line' 'MARKER_BETA' "$cli_out"
check_lacks 'show/log match drops the other line' 'MARKER_ALPHA' "$cli_out"

: > "$logread_rec"
run_cli show log -f
check_eq 'show/log follow exit' 0 "$cli_rc"
check_has 'show/log follow reads the log twice' '-e uvoice
-e uvoice -f' "$(cat "$logread_rec")"

logread_fail=1
run_cli show log
logread_fail=''
check_eq 'show/log propagates a failed read' 1 "$cli_rc"
check_has 'show/log names the failed read' 'logread failed' "$cli_out"

#
# The klish xml half. The scheme files live in the other package, so the suite
# reads them through a path variable and reports SKIP when they are absent,
# as the arc.voip vocabulary check above does.
#
if [ -r "$voip_xml_dir/voip.xml" ]; then
	check_eq 'xml/voip.xml calls voip_cli.sh nowhere' 0 \
		"$(grep -c 'voip_cli\.sh' "$voip_xml_dir/voip.xml")"

	xml_dispatch_count=$(grep -c 'voip-cli-dispatch' "$voip_xml_dir/voip.xml")
	if [ "$xml_dispatch_count" -ge 33 ]; then
		ok 'xml/voip.xml routes every backend call through the dispatcher'
	else
		not_ok 'xml/voip.xml routes every backend call through the dispatcher' \
			"want at least 33 dispatcher calls, got $xml_dispatch_count"
	fi

	# The loop test words route like every other verb, not to the loop test
	# scripts of the other stack, and the dispatcher picks the stack behind
	# them.
	check_eq 'xml/voip.xml calls the other loop test scripts nowhere' 0 \
		"$(grep -cE 'voip_test_(start|stop)\.sh' "$voip_xml_dir/voip.xml")"
	check_eq 'xml/voip.xml starts the loop test through the dispatcher' 1 \
		"$(grep -c 'voip-cli-dispatch looptest start' "$voip_xml_dir/voip.xml")"
	check_eq 'xml/voip.xml stops the loop test through the dispatcher' 1 \
		"$(grep -c 'voip-cli-dispatch looptest stop' "$voip_xml_dir/voip.xml")"

	check_eq 'xml/voip.xml carries no leftover line decrement' 0 \
		"$(grep -c 'let v_line' "$voip_xml_dir/voip.xml")"

	xml_conf_ptype=$(awk '/<PARAM name="local_conf_enabled"/,/<\/PARAM>/' \
		"$voip_xml_dir/voip.xml" | sed -n 's/.*ptype="\([A-Za-z_]*\)".*/\1/p' | sed -n 2p)
	check_eq 'xml/voip.xml gives the conference value the tri-state ptype' \
		'CONF_MODE' "$xml_conf_ptype"

	check_eq 'xml/voip.xml wires the line test command' 1 \
		"$(grep -c 'name="voip line-test"' "$voip_xml_dir/voip.xml")"
	# The four tests are subcommand words and not a select ptype, so klish
	# completes them.
	xml_test_words=0
	for xml_word in gr909 roh capacitance loop; do
		grep -q "<PARAM name=\"$xml_word\"" "$voip_xml_dir/voip.xml" &&
			xml_test_words=$((xml_test_words + 1))
	done
	check_eq 'xml/voip.xml offers the four line tests as words' 4 "$xml_test_words"
	xml_show_words=0
	for xml_word in daemon lines calls line-monitor call-records live; do
		grep -q "<PARAM name=\"$xml_word\"" "$voip_xml_dir/voip.xml" &&
			xml_show_words=$((xml_show_words + 1))
	done
	check_eq 'xml/voip.xml offers the six uvoice show words' 6 "$xml_show_words"
	# voice-cli prints the SIP client status row only under the sip
	# protocol, so the scheme must not read that row out of the status
	# text. It passes the text through.
	check_eq 'xml/voip.xml reads no SIP client status row' 0 \
		"$(grep -c 'SIP client status' "$voip_xml_dir/voip.xml")"
else
	skip 'xml/voip.xml calls voip_cli.sh nowhere' "no voip.xml under $voip_xml_dir"
	skip 'xml/voip.xml routes every backend call through the dispatcher' "no voip.xml under $voip_xml_dir"
	skip 'xml/voip.xml calls the other loop test scripts nowhere' "no voip.xml under $voip_xml_dir"
	skip 'xml/voip.xml starts the loop test through the dispatcher' "no voip.xml under $voip_xml_dir"
	skip 'xml/voip.xml stops the loop test through the dispatcher' "no voip.xml under $voip_xml_dir"
	skip 'xml/voip.xml carries no leftover line decrement' "no voip.xml under $voip_xml_dir"
	skip 'xml/voip.xml gives the conference value the tri-state ptype' "no voip.xml under $voip_xml_dir"
	skip 'xml/voip.xml wires the line test command' "no voip.xml under $voip_xml_dir"
	skip 'xml/voip.xml offers the four line tests as words' "no voip.xml under $voip_xml_dir"
	skip 'xml/voip.xml offers the six uvoice show words' "no voip.xml under $voip_xml_dir"
	skip 'xml/voip.xml reads no SIP client status row' \
		"no voip.xml under $voip_xml_dir"
fi

if [ -r "$voip_xml_dir/types.xml" ]; then
	xml_conf_pattern=$(awk '/PTYPE name="CONF_MODE"/,/\/>/' "$voip_xml_dir/types.xml" |
		sed -n 's/.*pattern="\([^"]*\)".*/\1/p')
	check_eq 'xml/types.xml CONF_MODE names the three conference modes' \
		'disabled(0) conference(1) conference-transfer(2)' "$xml_conf_pattern"
else
	skip 'xml/types.xml CONF_MODE names the three conference modes' "no types.xml under $voip_xml_dir"
fi

if [ -r "$voip_xml_dir/marco.xml" ]; then
	check_eq 'xml/marco.xml calls voip_cli.sh nowhere' 0 \
		"$(grep -c 'voip_cli\.sh' "$voip_xml_dir/marco.xml")"

	xml_marco_count=$(grep -c 'voip-cli-dispatch' "$voip_xml_dir/marco.xml")
	if [ "$xml_marco_count" -ge 2 ]; then
		ok 'xml/marco.xml routes its two backend calls through the dispatcher'
	else
		not_ok 'xml/marco.xml routes its two backend calls through the dispatcher' \
			"want at least 2 dispatcher calls, got $xml_marco_count"
	fi
else
	skip 'xml/marco.xml calls voip_cli.sh nowhere' "no marco.xml under $voip_xml_dir"
	skip 'xml/marco.xml routes its two backend calls through the dispatcher' "no marco.xml under $voip_xml_dir"
fi

#
# The dispatcher
#
if [ "$dispatch_present" = 1 ]; then
	run_dispatch show version
	check_eq 'dispatch/voice present exit' 0 "$dispatch_rc"
	check_has 'dispatch/voice present reaches voice-cli' 'HAL backend' "$dispatch_out"

	: > "$voip_cli_rec"
	harness_ubus_socket=$bare_ubus_socket
	run_dispatch show version
	dispatch_version_out=$dispatch_out
	run_dispatch show daemon
	dispatch_daemon_rc=$dispatch_rc
	dispatch_daemon_out=$dispatch_out
	run_dispatch show channel1 config
	dispatch_chconfig_rc=$dispatch_rc
	run_dispatch line_test 1 roh
	dispatch_linetest_rc=$dispatch_rc
	run_dispatch show channel1 sip_proxy
	harness_ubus_socket=$ubus_socket
	check_has 'dispatch/voice absent reaches voip_cli.sh' 'VOIP_CLI_STUB' \
		"$dispatch_version_out"
	# The record holds the two shapes of the other stack alone: the
	# uvoice-only verbs stop in the dispatcher, because voip_cli.sh would
	# answer them with an "invalid argument" that names the wrong cause.
	check_eq 'dispatch/voice absent keeps the arguments' 'show version
show channel1 sip_proxy' "$(cat "$voip_cli_rec")"
	check_eq 'dispatch/voice absent refuses a uvoice-only show verb' 1 \
		"$dispatch_daemon_rc"
	check_has 'dispatch/voice absent names the stack' \
		'not supported with this voice stack' "$dispatch_daemon_out"
	check_eq 'dispatch/voice absent refuses the channel config view' 1 \
		"$dispatch_chconfig_rc"
	check_eq 'dispatch/voice absent refuses the line test' 1 \
		"$dispatch_linetest_rc"

	#
	# The loop test is the one pair of words both stacks answer, so the
	# dispatcher routes it rather than refusing it: voice-cli on a uvoice
	# board, the two loop test scripts on the other one, and the refusal
	# above only where neither stack installed anything.
	#
	: > "$other_test_rec"
	harness_ubus_socket=$bare_ubus_socket
	run_dispatch looptest start
	dispatch_ltstart_rc=$dispatch_rc
	dispatch_ltstart_out=$dispatch_out
	run_dispatch looptest stop
	dispatch_ltstop_out=$dispatch_out
	harness_ubus_socket=$ubus_socket
	check_eq 'dispatch/voice absent loop test start exit' 0 \
		"$dispatch_ltstart_rc"
	check_has 'dispatch/voice absent reaches voip_test_start.sh' \
		'OTHER_TEST_START_STUB' "$dispatch_ltstart_out"
	check_has 'dispatch/voice absent reaches voip_test_stop.sh' \
		'OTHER_TEST_STOP_STUB' "$dispatch_ltstop_out"
	check_eq 'dispatch/voice absent ran both loop test scripts in order' \
		'start
stop' "$(cat "$other_test_rec")"
	check_eq 'dispatch/the other loop test reached voip_cli.sh nowhere' \
		'show version
show channel1 sip_proxy' "$(cat "$voip_cli_rec")"

	chmod -x /usr/srg/scripts/voip_test_start.sh \
		/usr/srg/scripts/voip_test_stop.sh
	harness_ubus_socket=$bare_ubus_socket
	run_dispatch looptest start
	harness_ubus_socket=$ubus_socket
	chmod +x /usr/srg/scripts/voip_test_start.sh \
		/usr/srg/scripts/voip_test_stop.sh
	check_eq 'dispatch/a board with no loop test refuses the words' 1 \
		"$dispatch_rc"
	check_has 'dispatch/a board with no loop test names the stack' \
		'not supported with this voice stack' "$dispatch_out"

	run_dispatch looptest stop
	check_eq 'dispatch/voice present takes the loop test' 0 "$dispatch_rc"
	check_has 'dispatch/voice present reaches voice-cli for the loop test' \
		'enabled=false' "$dispatch_out"

	chmod -x /usr/srg/scripts/voice-cli /usr/srg/scripts/voip_cli.sh
	run_dispatch show version
	chmod +x /usr/srg/scripts/voice-cli /usr/srg/scripts/voip_cli.sh
	check_eq 'dispatch/no backend exits 1' 1 "$dispatch_rc"
	check_has 'dispatch/no backend names the paths' 'no voice CLI backend' "$dispatch_out"
else
	skip 'dispatch/voice present exit' 'no dispatcher'
	skip 'dispatch/voice present reaches voice-cli' 'no dispatcher'
	skip 'dispatch/voice absent reaches voip_cli.sh' 'no dispatcher'
	skip 'dispatch/voice absent keeps the arguments' 'no dispatcher'
	skip 'dispatch/voice absent refuses a uvoice-only show verb' 'no dispatcher'
	skip 'dispatch/voice absent names the stack' 'no dispatcher'
	skip 'dispatch/voice absent refuses the channel config view' 'no dispatcher'
	skip 'dispatch/voice absent refuses the line test' 'no dispatcher'
	skip 'dispatch/voice absent reaches voip_test_start.sh' 'no dispatcher'
	skip 'dispatch/voice absent reaches voip_test_stop.sh' 'no dispatcher'
	skip 'dispatch/voice absent ran both loop test scripts in order' 'no dispatcher'
	skip 'dispatch/the other loop test reached voip_cli.sh nowhere' 'no dispatcher'
	skip 'dispatch/a board with no loop test refuses the words' 'no dispatcher'
	skip 'dispatch/a board with no loop test names the stack' 'no dispatcher'
	skip 'dispatch/voice present takes the loop test' 'no dispatcher'
	skip 'dispatch/voice present reaches voice-cli for the loop test' 'no dispatcher'
	skip 'dispatch/no backend exits 1' 'no dispatcher'
	skip 'dispatch/no backend names the paths' 'no dispatcher'
fi

tap_finish() {
	printf '1..%s\n' "$tests_run"
	printf '# run_cli_tests: %s checks, %s failed\n' "$tests_run" "$fails"

	[ "$fails" -eq 0 ] || exit 1
	exit 0
}

mgcp_status() {
	ubus -t 5 call voice status 2>/dev/null
}

#
# The protocol mgcp leg, on an image with both units. The config names mgcp
# and a call agent on the loopback where nothing listens, with the shortest
# retransmission bounds, so the restart RSIP of the endpoints is lost and the
# disconnected procedure starts in a second. No netifd answers, so the
# endpoint names take the source address toward the call agent
# (proto/mgcp/proto.c local_addr_get()). It runs last, as the unavailable leg
# does on an image with SIP alone.
#
mgcp_leg() {
	ip link set lo up 2>/dev/null || true
	uci set voip.base.protocol=mgcp
	uci set voip.mgcp=mgcp
	uci set voip.mgcp.call_agent=127.0.0.1
	uci set voip.mgcp.max1=1
	uci set voip.mgcp.max2=2
	uci set voip.mgcp.rto_init_ms=100
	uci set voip.mgcp.rto_max_ms=100
	uci set voip.mgcp.restart_delay_max_s=0
	uci commit voip
	kill "$uvoiced_pid" 2>/dev/null
	wait "$uvoiced_pid" 2>/dev/null
	"$uvoiced" -s "$ubus_socket" > "$tmp/uvoiced-mgcp.log" 2>&1 &
	uvoiced_pid=$!

	mgcp_hal=''
	for mgcp_try in 1 2 3 4 5 6 7 8 9 10; do
		mgcp_hal=$(mgcp_status | jq -r '.hal.state')
		[ "$mgcp_hal" = ready ] && break
		sleep 1
	done
	check_eq 'mgcp/the daemon comes up and the DSP completes' 'ready' \
		"$mgcp_hal"

	mgcp_st=$(mgcp_status)
	check_eq 'mgcp/signaling names mgcp running' 'mgcp running' \
		"$(printf '%s' "$mgcp_st" |
			jq -r '"\(.signaling.protocol) \(.signaling.state)"')"
	check_eq 'mgcp/available lists both tables in registry order' \
		'sip mgcp' "$(printf '%s' "$mgcp_st" |
			jq -r '.signaling.available | join(" ")')"
	check_eq 'mgcp/lines holds one endpoint row per line' \
		'2 aaln/1@127.0.0.1' "$(printf '%s' "$mgcp_st" |
			jq -r '"\(.lines | length) \(.lines[0].endpoint)"')"
	check_eq 'mgcp/the mgcp table names the dialect and the call agent' \
		'rfc3435 127.0.0.1' "$(printf '%s' "$mgcp_st" |
			jq -r '"\(.mgcp.dialect) \(.mgcp.call_agent)"')"
	check_eq 'mgcp/a call entry carries the state word of its endpoint' \
		'true' "$(printf '%s' "$mgcp_st" |
			jq -r '[.call[] | has("state")] | all')"

	mgcp_signature=$(ubus -v list voice 2>/dev/null)
	check_has 'mgcp/the voice object listing names the core methods' \
		'"status":' "$mgcp_signature"
	check_lacks 'mgcp/the voice object carries no register method' \
		'"register":' "$mgcp_signature"
	check_has 'mgcp/register answers method not found' 'Method not found' \
		"$(ubus -t 5 call voice register '{"line":0}' 2>&1)"

	ubus -t 5 call voice seize '{"line":0}' >/dev/null 2>&1
	mgcp_disc=''
	for mgcp_try in 1 2 3 4 5 6 7 8 9 10; do
		mgcp_st=$(mgcp_status)
		mgcp_disc=$(printf '%s' "$mgcp_st" |
			jq -r '"\(.mgcp.disconnected) \(.lines[0].service)"')
		[ "$mgcp_disc" = 'true disconnected' ] && break
		sleep 1
	done
	check_eq 'mgcp/a seize with no call agent starts the disconnected procedure' \
		'true disconnected' "$mgcp_disc"

	mgcp_capture=$(schema_capture_build "$mgcp_st")
	schema_valid_check 'schema/the live mgcp status capture validates' \
		"$mgcp_capture"
	schema_mutant_check \
		'schema/an mgcp capture without its mgcp table is refused' \
		"$mgcp_capture" '
		if (.reply | has("mgcp")) | not then
			error("the capture carries no mgcp table to remove")
		else
			del(.reply.mgcp)
		end'
	schema_mutant_check \
		'schema/an mgcp endpoint row without its name is refused' \
		"$mgcp_capture" '
		if (.reply.lines | length) == 0 then
			error("the capture carries no endpoint row")
		else
			del(.reply.lines[0].endpoint)
		end'

	mgcp_uc_status=''
	mgcp_uc_lines=''
	mgcp_uc_reg=''
	mgcp_uc_ring=''
	if [ "$uc_ready" = 1 ]; then
		run_uc status
		mgcp_uc_status=$uc_out
		run_uc lines
		mgcp_uc_lines=$uc_out
		run_uc register --line 0
		mgcp_uc_reg=$uc_out
		run_uc calltrace_arm --line 1 --no-wire --timeout 10 \
			--arm-timeout 10 --wire-max-mb 1 --rings-max-mb 1 --json
		mgcp_uc_ring=$(ubus -t 5 call voice.calltrace status 2>/dev/null |
			jq -r '.rings | "\(has("mgcp")) \(has("sip"))"')
		ubus -t 5 call voice.calltrace cancel >/dev/null 2>&1
	fi
	uc_check_has 'mgcp/cli status prints the mgcp table' \
		'socket                 bound' "$mgcp_uc_status"
	uc_check_has 'mgcp/cli status names the disconnected procedure' \
		'disconnected           yes' "$mgcp_uc_status"
	uc_check_has 'mgcp/cli status counts the endpoints' \
		'endpoints              2' "$mgcp_uc_status"
	uc_check_has 'mgcp/cli lines prints the endpoint rows' \
		'aaln/1@127.0.0.1' "$mgcp_uc_lines"
	uc_check_has 'mgcp/cli register refuses in one sentence' \
		'the daemon reports mgcp running' "$mgcp_uc_reg"
	uc_check_eq 'mgcp/the signaling ring carries the name mgcp' \
		'true false' "$mgcp_uc_ring"

	run_cli show status line 1
	check_eq 'mgcp/voice-cli show status exits 0' 0 "$cli_rc"
	check_lacks 'mgcp/voice-cli shows no SIP client status row' \
		'SIP client status' "$cli_out"
	check_has 'mgcp/voice-cli shows the MGCP call agent row' \
		'MGCP call agent         127.0.0.1:2727 (disconnected)' "$cli_out"
	check_eq 'mgcp/the seized line waits for its restart' 'restarting' \
		"$(mgcp_status | jq -r '.call | map(select(.line == 0))[0].state')"
	check_has 'mgcp/voice-cli maps a restarting endpoint to a disabled hook' \
		'Hook status             Disabled' "$cli_out"
	check_has 'mgcp/voice-cli maps a restarting endpoint to an idle call' \
		'Call status             Idle' "$cli_out"
	check_has 'mgcp/voice-cli maps a restarting endpoint to an idle detail' \
		'Call detailed status    Idle' "$cli_out"

	mgcp_rows=''
	for mgcp_word in idle offhook ringing connected restarting \
		out_of_service down; do
		mgcp_status | jq --arg word "$mgcp_word" \
			'.call |= map(if .line == 0 then .state = $word else . end)' \
			> "$tmp/mgcp-word.json"
		ubus_status_file=$tmp/mgcp-word.json
		run_cli show status line 1
		ubus_status_file=''
		mgcp_rows="$mgcp_rows$mgcp_word:$(printf '%s\n' "$cli_out" | awk '
			/^   (Hook status|Call status|Call detailed status) / {
				printf "%s/", substr($0, 28)
			}');"
	done
	check_eq 'mgcp/voice-cli maps every endpoint state word' \
		'idle:Onhook/Idle/Idle/;offhook:Offhook/Dialing/Dial tone/;ringing:Onhook/Alerting/Alerting/;connected:Offhook/Connected/Connected/;restarting:Disabled/Idle/Idle/;out_of_service:Disabled/Idle/Idle/;down:Disabled/Idle/Idle/;' \
		"$mgcp_rows"
	ubus -t 5 call voice release '{"line":0}' >/dev/null 2>&1
}

if [ "$image_mgcp" = 1 ]; then
	mgcp_leg
	tap_finish
fi

#
# The unavailable leg. This stub build carries the SIP table alone, so a
# config that names mgcp selects the null table (proto/proto.c): the daemon
# stays up, every line stays out of service through the DSP completion, the
# hook events and a reload, the voice object carries no account verb, and a
# seize plays reorder. A reload refuses a protocol move, so the leg restarts
# the daemon on the new config, the way the init script does. It runs last,
# because every leg above needs the SIP table.
#
uci set voip.base.protocol=mgcp && uci commit voip
kill "$uvoiced_pid" 2>/dev/null
wait "$uvoiced_pid" 2>/dev/null
"$uvoiced" -s "$ubus_socket" > "$tmp/uvoiced-mgcp.log" 2>&1 &
uvoiced_pid=$!

# Up means the DSP bring-up completed, which is the half of the start that
# settles the service state again.
unavail_hal=''
for unavail_try in 1 2 3 4 5 6 7 8 9 10; do
	unavail_hal=$(ubus -t 5 call voice status 2>/dev/null |
		jq -r '.hal.state')
	[ "$unavail_hal" = ready ] && break
	sleep 1
done
check_eq 'unavailable/the daemon comes up and the DSP completes' 'ready' \
	"$unavail_hal"

unavail_status=$(ubus -t 5 call voice status 2>/dev/null)
check_eq 'unavailable/signaling names mgcp unavailable' 'mgcp unavailable' \
	"$(printf '%s' "$unavail_status" |
		jq -r '"\(.signaling.protocol) \(.signaling.state)"')"
check_eq 'unavailable/the error names the reason' \
	'the image does not carry this protocol' \
	"$(printf '%s' "$unavail_status" | jq -r '.signaling.error')"
check_eq 'unavailable/available lists the sip table alone' 'sip' \
	"$(printf '%s' "$unavail_status" |
		jq -r '.signaling.available | join(" ")')"
check_eq 'unavailable/every line is out of service after the DSP completion' \
	'true true' \
	"$(printf '%s' "$unavail_status" |
		jq -r '[.call[].oos] | map(tostring) | join(" ")')"
check_eq 'unavailable/lines is an empty array' 'array 0' \
	"$(printf '%s' "$unavail_status" |
		jq -r '"\(.lines | type) \(.lines | length)"')"
check_eq 'unavailable/a call entry carries no protocol key' 'false' \
	"$(printf '%s' "$unavail_status" |
		jq -r '[.call[] | has("state") or has("calls")] | any')"

unavail_signature=$(ubus -v list voice 2>/dev/null)
# A core method present in the same listing, so the two absence checks below
# cannot pass on an empty capture.
check_has 'unavailable/the voice object listing names the core methods' \
	'"status":' "$unavail_signature"
check_lacks 'unavailable/the voice object carries no register method' \
	'"register":' "$unavail_signature"
check_lacks 'unavailable/the voice object carries no unregister method' \
	'"unregister":' "$unavail_signature"
unavail_reg=$(ubus -t 5 call voice register '{"line":0}' 2>&1)
check_has 'unavailable/register answers method not found' 'Method not found' \
	"$unavail_reg"

# The two refusals are the self-test of the unavailable branch of the
# schema: the reason is required there, and the null table writes no row.
unavail_capture=$(schema_capture_build "$unavail_status")
schema_valid_check 'schema/the live unavailable status capture validates' \
	"$unavail_capture"
schema_mutant_check \
	'schema/an unavailable capture without its reason is refused' \
	"$unavail_capture" '
	if (.reply.signaling | has("error")) | not then
		error("the capture carries no reason to remove")
	else
		del(.reply.signaling.error)
	end'
schema_mutant_check \
	'schema/an unavailable capture with a line entry is refused' \
	"$unavail_capture" '
	if (.reply.lines | length) != 0 then
		error("the capture already carries a line entry")
	else
		.reply.lines = [{"line": 0}]
	end'

# HAL_TONE_REORDER is 6 in enum hal_tone of hal.h; the stub dump carries the
# tone as that number.
unavail_tone() {
	ubus -t 5 call voice.stub dump 2>/dev/null | jq -r ".line[$1].tone"
}
unavail_oos() {
	ubus -t 5 call voice status 2>/dev/null |
		jq -r '[.call[].oos] | map(tostring) | join(" ")'
}
ubus -t 5 call voice seize '{"line":0}' >/dev/null 2>&1
check_eq 'unavailable/a seize plays reorder' 6 "$(unavail_tone 0)"
check_eq 'unavailable/every line stays out of service across the seize' \
	'true true' "$(unavail_oos)"
ubus -t 5 call voice release '{"line":0}' >/dev/null 2>&1
check_eq 'unavailable/the release stops the reorder' 0 "$(unavail_tone 0)"
check_eq 'unavailable/every line stays out of service across the release' \
	'true true' "$(unavail_oos)"

if ubus -t 5 call voice reload >/dev/null 2>&1; then
	ok 'unavailable/a reload on the same protocol takes'
else
	not_ok 'unavailable/a reload on the same protocol takes' \
		'the call failed'
fi
check_eq 'unavailable/every line stays out of service across the reload' \
	'true true' "$(unavail_oos)"
check_eq 'unavailable/the reload keeps the null table' 'unavailable' \
	"$(status_field_get '.signaling.state')"

run_cli show status line 1
check_eq 'unavailable/voice-cli show status exits 0' 0 "$cli_rc"
check_lacks 'unavailable/voice-cli shows no SIP client status row' \
	'SIP client status' "$cli_out"
check_has 'unavailable/voice-cli still shows the hook status' 'Hook status' \
	"$cli_out"

uc_unavail_status=''
uc_unavail_lines=''
uc_unavail_reg=''
uc_unavail_reg_rc=''
if [ "$uc_ready" = 1 ]; then
	run_uc status
	uc_unavail_status=$uc_out
	run_uc lines
	uc_unavail_lines=$uc_out
	run_uc register --line 0
	uc_unavail_reg=$uc_out
	uc_unavail_reg_rc=$uc_rc
fi
uc_check_has 'unavailable/cli status names the state' \
	'state                  unavailable' "$uc_unavail_status"
uc_check_has 'unavailable/cli status names the reason' \
	'error                  the image does not carry this protocol' \
	"$uc_unavail_status"
uc_check_has 'unavailable/cli lines prints the reason' \
	'mgcp is unavailable: the image does not carry this protocol.' \
	"$uc_unavail_lines"
uc_check_eq 'unavailable/cli register exits 1' 1 "$uc_unavail_reg_rc"
uc_check_has 'unavailable/cli register refuses in one sentence' \
	'register applies to a running sip protocol only, and the daemon' \
	"$uc_unavail_reg"
uc_check_has 'unavailable/cli register names the configured protocol' \
	'the daemon reports mgcp unavailable' "$uc_unavail_reg"

tap_finish
