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
# run_init_tests.sh: the engine-CPU isolation of the shipped init script
# (uvoice.init) against a fake /proc, a fake workqueue node and a recording
# uci, and its reload trigger (reload_service) against a scripted ubus.
#
# The init script reads /proc/cpuinfo and /proc/interrupts, writes
# /proc/irq/<n>/smp_affinity and /sys/devices/virtual/workqueue/cpumask, and
# takes voip.hal.engine_cpu from uci. A build host has none of that to spare,
# so the suite re-enters itself under "unshare -rm". It bind-mounts fakes over
# /proc/cpuinfo, /proc/interrupts and /proc/irq, and mounts a tmpfs over
# /sys/devices/virtual/workqueue. It puts an overlay over /usr for the
# voice-stack helper the script sources. The shipped script then runs with its
# own strings. The script ships as "#!/bin/sh /etc/rc.common" and is sourced
# here, with the procd verbs stubbed, so start_service() runs the same wiring
# it runs on a target.
#
# Every leg runs under every interpreter the host offers among dash and
# busybox sh, the run_seed_tests.sh shape.
#
#   INIT      the script under test (default ../../files/uvoice.init)
#
# Output is TAP: one "ok"/"not ok" line per check, a trailing plan line, and a
# non-zero exit when any check fails.

here=$(cd "$(dirname "$0")" && pwd)
uvoice_dir=$(cd "$here/../.." && pwd)
init=${INIT:-$uvoice_dir/files/uvoice.init}
export here uvoice_dir init

if [ "${INIT_TESTS_INNER:-}" != 1 ]; then
	INIT_TESTS_INNER=1
	export INIT_TESTS_INNER
	exec unshare -rm "$0" "$@"
fi

bail() {
	printf 'Bail out! %s\n' "$*"
	exit 1
}

[ -r "$init" ] || bail "no init script at $init"

tmp=$(mktemp -d) || bail "mktemp failed"

wq_dir=/sys/devices/virtual/workqueue
wq_node=$wq_dir/cpumask

cleanup() {
	cd /
	umount -l "$wq_dir" 2>/dev/null
	umount -l /proc/irq 2>/dev/null
	umount -l /proc/interrupts 2>/dev/null
	umount -l /proc/cpuinfo 2>/dev/null
	umount -l /usr 2>/dev/null
	rm -rf "$tmp"
}
trap 'cleanup' EXIT

mkdir -p "$tmp/usr-up/srg/scripts" "$tmp/usr-work" "$tmp/bin" "$tmp/rec" \
	"$tmp/proc" "$tmp/irq/42" "$tmp/irq/43"

mount -t overlay overlay \
	-o "lowerdir=/usr,upperdir=$tmp/usr-up,workdir=$tmp/usr-work" /usr ||
	bail 'cannot overlay /usr, the kernel refuses an unprivileged overlay mount'
mkdir -p /usr/srg/scripts || bail 'cannot create the device paths'

# The stack helper the script sources: the answer that makes start_service()
# run. The platform database stays absent, so the port count takes its
# default and the DPCM route takes its generic arm.
cat > /usr/srg/scripts/voice-stack.sh <<'LIB'
voice_stack_get() { echo uvoice; }
LIB

: > "$tmp/proc/cpuinfo"
: > "$tmp/proc/interrupts"
mount --bind "$tmp/proc/cpuinfo" /proc/cpuinfo ||
	bail 'cannot bind a fake over /proc/cpuinfo'
mount --bind "$tmp/proc/interrupts" /proc/interrupts ||
	bail 'cannot bind a fake over /proc/interrupts'
mount --bind "$tmp/irq" /proc/irq || bail 'cannot bind a fake over /proc/irq'
[ -d "$wq_dir" ] || bail "no $wq_dir on this host to cover"
mount -t tmpfs tmpfs "$wq_dir" || bail "cannot cover $wq_dir with a tmpfs"

#
# The shims. uci answers get from a state file; logger and amixer record their
# argument vectors and succeed, so a log line the script emits is an assertion
# and a mixer switch on the generic arm costs nothing.
#
UCI_STATE=$tmp/rec/state
LOG_REC=$tmp/rec/logger.argv
export UCI_STATE LOG_REC

cat > "$tmp/bin/uci" <<'SHIM'
#!/bin/sh
[ "$1" = -q ] && shift
case "$1" in
get)
	awk -v k="$2=" 'index($0, k) == 1 { print substr($0, length(k) + 1); f = 1 }
			END { exit !f }' "$UCI_STATE"
	;;
*)
	exit 2
	;;
esac
SHIM
cat > "$tmp/bin/logger" <<'SHIM'
#!/bin/sh
printf '%s\n' "$*" >> "$LOG_REC"
SHIM
cat > "$tmp/bin/amixer" <<'SHIM'
#!/bin/sh
exit 0
SHIM
# ubus answers from a script of scenario files. UBUS_RUNNING holds one answer
# per "service list" call, true or false, and the last answer repeats. A wait
# for the object exits with the content of UBUS_WAIT_RC, and each "voice
# reload" call takes the next exit status of UBUS_RELOAD_RC the same way.
# Every call is recorded.
UBUS_REC=$tmp/rec/ubus.argv
UBUS_RUNNING=$tmp/rec/ubus.running
UBUS_WAIT_RC=$tmp/rec/ubus.wait
UBUS_RELOAD_RC=$tmp/rec/ubus.reload
export UBUS_REC UBUS_RUNNING UBUS_WAIT_RC UBUS_RELOAD_RC
cat > "$tmp/bin/ubus" <<'SHIM'
#!/bin/sh
printf '%s\n' "$*" >> "$UBUS_REC"
next() {
	head -n 1 "$1"
	[ "$(wc -l < "$1")" -gt 1 ] && sed -i 1d "$1"
}
case "$*" in
*"call service list"*)
	printf '{ "uvoice": { "instances": { "instance1": { "running": %s } } } }\n' \
		"$(next "$UBUS_RUNNING")"
	;;
*"wait_for voice"*)
	exit "$(cat "$UBUS_WAIT_RC")"
	;;
*"call voice reload"*)
	exit "$(next "$UBUS_RELOAD_RC")"
	;;
*)
	exit 2
	;;
esac
SHIM
chmod +x "$tmp/bin/uci" "$tmp/bin/logger" "$tmp/bin/amixer" "$tmp/bin/ubus"

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

node_read() {
	[ -e "$1" ] && cat "$1"
}

log_count() {
	grep -c -F -- "$1" "$LOG_REC"
}

# reload_fixture running wait reload: the service list answers (one word per
# call, the last repeats), the exit status of the wait for the object and the
# exit statuses of the reload calls.
reload_fixture() {
	: > "$LOG_REC"
	: > "$UBUS_REC"
	printf '%s\n' $1 > "$UBUS_RUNNING"
	printf '%s\n' "$2" > "$UBUS_WAIT_RC"
	printf '%s\n' $3 > "$UBUS_RELOAD_RC"
}

ubus_count() {
	grep -c -F -- "$1" "$UBUS_REC"
}

# init_fixture cpus engine_cpu wq_preset: cpus processors, the engine_cpu
# option as given (an empty value leaves it absent), the PCM irq 42 and an
# ethernet irq 43 both at the full mask, and the workqueue node at wq_preset
# (an empty preset removes the node).
init_fixture() {
	cpus=$1
	engine_cpu=$2
	wq_preset=$3
	full=$(printf '%x' $(( (1 << cpus) - 1 )))

	: > "$UCI_STATE"
	: > "$LOG_REC"
	[ -n "$engine_cpu" ] && printf 'voip.hal.engine_cpu=%s\n' "$engine_cpu" \
		> "$UCI_STATE"

	: > "$tmp/proc/cpuinfo"
	i=0
	while [ "$i" -lt "$cpus" ]; do
		printf 'processor\t: %d\n\n' "$i" >> "$tmp/proc/cpuinfo"
		i=$((i + 1))
	done
	cat > "$tmp/proc/interrupts" <<EOF
           CPU0       CPU1
 42:      70429          0    GICv3  59 Level     1fbd0000.pcm
 43:      10971          0    GICv3  69 Level     airoha_eth.0
EOF
	printf '%s\n' "$full" > "$tmp/irq/42/smp_affinity"
	printf '%s\n' "$full" > "$tmp/irq/43/smp_affinity"
	rm -f "$wq_node"
	[ -n "$wq_preset" ] && printf '%s\n' "$wq_preset" > "$wq_node"
}

# run_init shell verbs...: source the shipped script under the shell with the
# procd verbs stubbed, then run the named functions in order.
run_init() {
	shell=$1
	shift

	PATH="$tmp/bin:$PATH" $shell -c '
		procd_open_instance() { logger -t procd -- open_instance; }
		procd_set_param() { :; }
		procd_close_instance() { logger -t procd -- close_instance; }
		. "$init"
		restart() { logger -t rc -- restart; }
		sleep() { :; }
		udsp_stack_present() { return 0; }
		for verb in "$@"; do
			$verb
		done' init_tests "$@" >/dev/null 2>&1
	rc=$?
}

legs() {
	shell=$1

	$shell -n "$init" && ok "$shell -n accepts the init script" ||
		not_ok "$shell -n accepts the init script"
	$shell -n "$0" && ok "$shell -n accepts this harness" ||
		not_ok "$shell -n accepts this harness"

	init_fixture 4 3 f
	run_init "$shell" udsp_pcm_irq_move udsp_workqueue_move
	check "$shell engine 3 of 4: exit 0" "$rc" 0
	check "$shell engine 3 of 4: the pcm irq moves to the other cpus" \
		"$(node_read "$tmp/irq/42/smp_affinity")" 7
	check "$shell engine 3 of 4: an irq of another name stays" \
		"$(node_read "$tmp/irq/43/smp_affinity")" f
	check "$shell engine 3 of 4: the unbound workqueues move too" \
		"$(node_read "$wq_node")" 7
	check "$shell engine 3 of 4: nothing is logged" "$(log_count '')" 0

	init_fixture 4 -1 7
	run_init "$shell" udsp_pcm_irq_move udsp_workqueue_move
	check "$shell pinning off: the pcm irq stays" \
		"$(node_read "$tmp/irq/42/smp_affinity")" f
	check "$shell pinning off: the workqueues get the full mask back" \
		"$(node_read "$wq_node")" f

	init_fixture 4 5 7
	run_init "$shell" udsp_pcm_irq_move udsp_workqueue_move
	check "$shell an engine cpu the box lacks: the pcm irq stays" \
		"$(node_read "$tmp/irq/42/smp_affinity")" f
	check "$shell an engine cpu the box lacks: the full mask comes back" \
		"$(node_read "$wq_node")" f

	init_fixture 4 3 ''
	run_init "$shell" udsp_pcm_irq_move udsp_workqueue_move
	check "$shell no workqueue node: the pcm irq still moves" \
		"$(node_read "$tmp/irq/42/smp_affinity")" 7
	check "$shell no workqueue node: nothing is created" \
		"$(node_read "$wq_node")" ""
	check "$shell no workqueue node: nothing is logged" "$(log_count '')" 0

	init_fixture 2 1 3
	run_init "$shell" udsp_pcm_irq_move udsp_workqueue_move
	check "$shell engine 1 of 2: the pcm irq moves to cpu 0" \
		"$(node_read "$tmp/irq/42/smp_affinity")" 1
	check "$shell engine 1 of 2: the workqueues move to cpu 0" \
		"$(node_read "$wq_node")" 1

	init_fixture 4 abc f
	run_init "$shell" udsp_pcm_irq_move udsp_workqueue_move
	check "$shell an unparsable option takes the schema default 3" \
		"$(node_read "$wq_node")" 7

	init_fixture 4 '' f
	run_init "$shell" udsp_pcm_irq_move udsp_workqueue_move
	check "$shell an absent option takes the schema default 3" \
		"$(node_read "$wq_node")" 7

	# The wiring: start_service() itself, with the stack gate answering
	# uvoice and the platform gate stubbed open, writes both nodes and
	# reaches procd.
	init_fixture 4 3 f
	run_init "$shell" start_service
	check "$shell start_service: exit 0" "$rc" 0
	check "$shell start_service: the pcm irq moved" \
		"$(node_read "$tmp/irq/42/smp_affinity")" 7
	check "$shell start_service: the workqueues moved" \
		"$(node_read "$wq_node")" 7
	check "$shell start_service: the instance opened" \
		"$(log_count 'open_instance')" 1
	check "$shell start_service: the start was logged" \
		"$(log_count 'uvoiced started')" 1

	# reload_service, the procd trigger of every voip commit.
	reload_fixture true 0 0
	run_init "$shell" reload_service
	check "$shell reload: a running daemon takes the reload" \
		"$(ubus_count 'call voice reload')" 1
	check "$shell reload: no restart" "$(log_count 'restart')" 0
	check "$shell reload: the reload is logged" \
		"$(log_count 'config reloaded through ubus')" 1

	reload_fixture false 0 0
	run_init "$shell" reload_service
	check "$shell reload: no instance, no call to the daemon" \
		"$(ubus_count 'call voice reload')" 0
	check "$shell reload: no instance, no restart" \
		"$(log_count 'restart')" 0
	check "$shell reload: no instance is logged" \
		"$(log_count 'its start reads the config')" 1

	reload_fixture 'true false' 0 '1 0'
	run_init "$shell" reload_service
	check "$shell reload: a daemon stopping under the reload is not restarted" \
		"$(log_count 'restart')" 0
	check "$shell reload: the stopped daemon takes one call" \
		"$(ubus_count 'call voice reload')" 1

	reload_fixture true 1 0
	run_init "$shell" reload_service
	check "$shell reload: an object that never appears takes no call" \
		"$(ubus_count 'call voice reload')" 0
	check "$shell reload: an object that never appears restarts once" \
		"$(log_count 'rc -- restart')" 1

	reload_fixture true 0 '1 0'
	run_init "$shell" reload_service
	check "$shell reload: a second call that takes needs no restart" \
		"$(log_count 'restart')" 0
	check "$shell reload: two calls" "$(ubus_count 'call voice reload')" 2

	reload_fixture true 0 '1 1'
	run_init "$shell" reload_service
	check "$shell reload: a running daemon that refuses twice is restarted" \
		"$(log_count 'rc -- restart')" 1
	check "$shell reload: the restart is logged crit" \
		"$(log_count 'ubus reload failed, restarting uvoiced')" 1
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
