#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.

set -u

# The churn reads the clock out of EPOCHREALTIME, which is a bash 5 variable,
# because a date process per poll is a hundred thousand processes over a soak.
if (( ${BASH_VERSINFO[0]:-0} < 5 )); then
    printf 'ERROR: this harness needs bash 5 or later for EPOCHREALTIME, found %s\n' \
        "${BASH_VERSION:-unknown}" >&2
    exit 2
fi

# Both the clock and the pause cross a locale: EPOCHREALTIME carries the
# locale's radix character and sleep takes the C one, so the run fixes it.
export LC_NUMERIC=C

script_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
uvoice_dir=$(CDPATH='' cd "$script_dir/../.." && pwd)
# shellcheck source=lib.sh
. "$script_dir/lib.sh"
sipp_dir=$uvoice_dir/test/sipp
default_sipp=/tmp/uvoice-host/sipp/sipp-3.7.3/sipp
sipp_bin=${SIPP_BIN:-$default_sipp}
host_prefix=${UVOICE_HOST_PREFIX:-/tmp/uvoice-host/prefix}
artifact_root=${CHURN_ARTIFACTS:-./churn-artifacts}
scenario_file=${CHURN_SCENARIO:-$sipp_dir/uas_churn.xml}
dial_number=${CHURN_NUMBER:-123#}
talk_ms=${CHURN_TALK_MS:-100}
poll_ms=${CHURN_POLL_MS:-20}
connect_timeout_ms=${CHURN_CONNECT_TIMEOUT_MS:-15000}
idle_timeout_ms=${CHURN_IDLE_TIMEOUT_MS:-5000}
drain_timeout_ms=${CHURN_DRAIN_TIMEOUT_MS:-8000}
record_timeout_ms=${CHURN_RECORD_TIMEOUT_MS:-10000}
rss_growth_pct=${CHURN_RSS_GROWTH_PCT:-5}
sipp_headroom=${CHURN_SIPP_HEADROOM:-128}
trace_domains=${CHURN_TRACE:-}
churn_family=${CHURN_FAMILY:-4}
build_dir=
current_daemon_pid=
current_ubusd_pid=
current_sipp_pid=
current_socket=
daemon_status=
process_status=0
absorbed_register=0
absorbed_register_retrans=0
foreign_abort=0
foreign_abort_detail=
sipp_verdict_detail=
sipp_success=
# The calls and the first REGISTER, which the scenario answers and SIPp counts
# as a successful call. A refresh adds one more.
sipp_expected=0
ubus_json=
ms_value=0
talk_s=0
poll_s=0
calls_total=0
check_every=0
checked_call=0
op_errors_base=0
cdr_seq_base=0
resource_csv=
fail_reason=
resource_detail=
daemon_detail=
sample_call=()
sample_rss=()
sample_fd=()
rss_first=0
rss_base=0
rss_peak=0
rss_last=0
fd_first=0
fd_base=0
fd_last=0
base_index=0
wall_start_ms=0
wall_ms=0
checkpoint_ms=0

usage_print()
{
    printf '%s\n' "Usage: $0 [CALLS]"
    printf '%s\n' "One daemon and one SIPp instance across CALLS sequential outbound calls (default 1000, minimum 10)."
    printf '%s\n' "Environment:"
    printf '  %-28s %s\n' \
        CHURN_FAMILY "address family, 4 or 6, default 4" \
        CHURN_ARTIFACTS "artifact directory, default ./churn-artifacts" \
        CHURN_SCENARIO "UAS scenario file, default test/sipp/uas_churn.xml" \
        CHURN_NUMBER "number the dial verb injects, default 123#" \
        CHURN_TALK_MS "talk pause, default 100, must clear the scenario pause by 50" \
        CHURN_CHECK_EVERY "calls between checkpoints, default 100 or a tenth of a short run" \
        CHURN_POLL_MS "poll interval of the state waits, default 20" \
        CHURN_CONNECT_TIMEOUT_MS "wait for CONNECTED, default 15000" \
        CHURN_IDLE_TIMEOUT_MS "wait for IDLE, default 5000" \
        CHURN_DRAIN_TIMEOUT_MS "wait for both slots at a checkpoint, default 8000" \
        CHURN_RECORD_TIMEOUT_MS "wait for the last record to close, default 10000" \
        CHURN_RSS_GROWTH_PCT "VmRSS growth allowed above the 10 percent mark, default 5" \
        CHURN_SIPP_HEADROOM "scenario instances -m allows above the call count, default 128" \
        CHURN_TRACE "daemon trace domains, default none" \
        CHURN_BUILD_JOBS "cmake job count when the daemon is built here, default 4" \
        CHURN_SIPP_PORT "SIPp port, default drawn from the pid" \
        CHURN_DAEMON_PORT "daemon SIP port, default the pid-drawn seed plus one" \
        CHURN_MEDIA_PORT "SIPp media port, default the pid-drawn seed plus two" \
        SIPP_BIN "SIPp binary" \
        UVOICED "daemon binary, built here when unset" \
        UVOICE_HOST_PREFIX "host library prefix, default /tmp/uvoice-host/prefix"
}

# EPOCHREALTIME is a shell variable, so the churn loop reads the clock without
# a process. The bracket takes both radix characters a locale can produce.
ms_now()
{
    local raw=${EPOCHREALTIME/[.,]/}

    ms_value=$(( ${raw:0:${#raw}-3} ))
}

prerequisites_check()
{
    local command_name

    for command_name in jq ubus unshare; do
        if ! command -v "$command_name" >/dev/null 2>&1; then
            printf 'ERROR: required command absent: %s\n' "$command_name" >&2
            return 1
        fi
    done

    if [[ -z ${UVOICED:-} ]] && ! command -v cmake >/dev/null 2>&1; then
        printf '%s\n' "ERROR: required command absent: cmake" >&2
        return 1
    fi

    if [[ ! -x $sipp_bin ]]; then
        printf 'ERROR: SIPp is not executable: %s\n' "$sipp_bin" >&2
        return 1
    fi

    if [[ ! -r $scenario_file ]]; then
        printf 'ERROR: scenario is not readable: %s\n' "$scenario_file" >&2
        return 1
    fi

    if [[ ! -x /usr/sbin/ubusd ]]; then
        printf '%s\n' "ERROR: /usr/sbin/ubusd is absent" >&2
        return 1
    fi
}

# The same question process_exited answers, asked without a process of its
# own: the churn puts this in every poll, and a ps per poll would be a
# thousand processes the run does not need.
process_live()
{
    local pid=$1
    local key value rest

    [[ -n $pid && -r /proc/$pid/status ]] || return 1
    while read -r key value rest; do
        if [[ $key = State: ]]; then
            [[ $value != Z ]]
            return
        fi
    done </proc/"$pid"/status

    return 1
}

# The stop deadline is a parameter because the daemon takes the unregister
# grace window on the way out and a sanitizer build writes its report after
# that; a SIGKILL there would throw away the leak verdict this harness exists
# for.
process_stop()
{
    local pid=${1:-}
    local timeout_s=${2:-5}
    local signal=${3:-TERM}
    local deadline

    process_status=
    if [[ -z $pid ]] || ! kill -0 "$pid" 2>/dev/null; then
        return
    fi

    kill -"$signal" "$pid" 2>/dev/null || true
    deadline=$((SECONDS + timeout_s))
    while kill -0 "$pid" 2>/dev/null && (( SECONDS < deadline )); do
        if [[ $(ps -o stat= -p "$pid" 2>/dev/null || true) = Z* ]]; then
            break
        fi
        sleep 0.1
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -KILL "$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null
    process_status=$?
}

churn_stop()
{
    process_stop "$current_sipp_pid"
    process_stop "$current_daemon_pid" 20
    if [[ -n $current_daemon_pid && -z $daemon_status ]]; then
        daemon_status=$process_status
    fi
    process_stop "$current_ubusd_pid"
    if [[ -n $current_socket ]]; then
        rm -f "$current_socket"
    fi
    current_sipp_pid=
    current_daemon_pid=
    current_ubusd_pid=
    current_socket=
}

exit_cleanup()
{
    churn_stop
}

churn_loop_addr()
{
    if [[ $churn_family = 6 ]]; then
        printf '%s\n' '::1'
    else
        printf '%s\n' '127.0.0.1'
    fi
}

churn_loop_uri()
{
    if [[ $churn_family = 6 ]]; then
        printf '%s\n' '[::1]'
    else
        printf '%s\n' '127.0.0.1'
    fi
}

config_write()
{
    local config_dir=$1
    local sipp_port=$2
    local daemon_port=$3

    mkdir -p "$config_dir" || return 1
    {
        printf '%s\n' "config base 'base'"
        printf '\toption enabled %s\n' "'1'"
        printf '\toption channels %s\n' "'1'"
        if [[ $churn_family = 6 ]]; then
            printf '\toption ipv6_enabled %s\n' "'1'"
        fi
        printf '%s\n' "config channel 'channel1'"
        printf '\toption id %s\n' "'0'"
        printf '\toption enabled %s\n' "'1'"
        printf '\toption sip_username %s\n' "'5551000'"
        printf '\toption sip_proxy %s\n' "'$(churn_loop_addr)'"
        printf "\toption sip_port '%u'\n" "$sipp_port"
        printf '\toption sip_outbound_proxy %s\n' "'$(churn_loop_addr)'"
        printf "\toption sip_outbound_proxy_port '%u'\n" "$sipp_port"
        printf '\toption sip_domain %s\n' "'$(churn_loop_addr)'"
        printf '\toption sip_registrar %s\n' "'$(churn_loop_addr)'"
        printf "\toption sip_registrar_port '%u'\n" "$sipp_port"
        printf "\toption sip_local_port '%u'\n" "$daemon_port"
        printf '\toption sip_registration_expiry %s\n' "'3600'"
        # 32 even ports, which is the pool call_port_alloc walks round robin.
        # The e2e range of 5 pairs would hand the same port back within a few
        # calls, so a port a finished call has not fully released yet would
        # alias onto the next call and the churn would blame the daemon.
        printf '\toption rtp_local_port_min %s\n' "'24000'"
        printf '\toption rtp_local_port_max %s\n' "'24063'"
    } >"$config_dir/voip"
}

loopback_prepare()
{
    ip link set lo up 2>/dev/null || true
    ip -o link show lo 2>/dev/null | grep -qE '[<,]UP[,>]'
}

churn_bound_check()
{
    if [[ $churn_family = 6 ]]; then
        udp6_bound_check "$1"
        return
    fi
    udp_bound_check "$1"
}

sipp_ready_wait()
{
    local port=$1
    local timeout_s=${2:-10}
    local deadline=$((SECONDS + timeout_s))

    if [[ $churn_family = 6 && ! -e /proc/net/udp6 ]]; then
        fail_reason="the kernel has no IPv6 support: /proc/net/udp6 is absent"
        return 1
    fi

    while (( SECONDS < deadline )); do
        if churn_bound_check "$port"; then
            return 0
        fi
        if process_exited "$current_sipp_pid"; then
            wait "$current_sipp_pid"
            process_status=$?
            current_sipp_pid=
            return 1
        fi
        sleep 0.05
    done
    return 1
}

# Without -trace_msg and without a capture: a message log of a ten thousand
# call run is gigabytes and the run itself is the subject, not one dialog.
#
# The budget is the call count plus headroom, because SIPp counts every call
# it creates against -m and the daemon's REGISTER opens one of those. A
# budget of exactly the call count would starve the last calls of the run.
#
# The simultaneous call limit is eight against a churn that runs one call at a
# time: the room is for the REGISTER instance beside the call and for a
# scenario instance that has not been reaped yet.
sipp_start()
{
    local sipp_port=$1
    local daemon_port=$2
    local media_port=$3
    local budget=$4
    local timeout_s=$5

    "$sipp_bin" "$(churn_loop_uri):$daemon_port" \
        -sf "$scenario_file" -m "$budget" -l 8 \
        -p "$sipp_port" -mp "$media_port" -i "$(churn_loop_addr)" \
        -recv_timeout "$((timeout_s * 1000))" \
        -timeout "$timeout_s" -timeout_error -nostdin \
        -trace_err -error_file "$artifact_root/sipp-errors.log" \
        -trace_screen -screen_file "$artifact_root/sipp-screen.log" \
        >"$artifact_root/sipp-stdout.log" 2>&1 &
    current_sipp_pid=$!
    sipp_ready_wait "$sipp_port"
}

daemon_start()
{
    local config_dir=$1
    local socket_tag=$2
    local deadline
    local -a trace_args=()

    current_socket=/tmp/uchurn.$$.${socket_tag}.sock
    rm -f "$current_socket"
    /usr/sbin/ubusd -s "$current_socket" >"$artifact_root/ubusd.log" 2>&1 &
    current_ubusd_pid=$!
    sleep 0.2
    if ! kill -0 "$current_ubusd_pid" 2>/dev/null; then
        return 1
    fi

    # No trace domain by default: one line per state change is a hundred
    # megabytes over a ten thousand call run, and the counters are the oracle.
    if [[ -n $trace_domains ]]; then
        trace_args=(-d "$trace_domains")
    fi
    LD_LIBRARY_PATH="$host_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        UVOICE_UCI_CONFDIR="$config_dir" \
        "$UVOICED" -s "$current_socket" "${trace_args[@]}" \
        >"$artifact_root/daemon.log" 2>&1 &
    current_daemon_pid=$!
    deadline=$((SECONDS + 10))
    while (( SECONDS < deadline )); do
        if ubus -s "$current_socket" call voice status >"$artifact_root/readiness.json" 2>/dev/null &&
            jq -e '.hal.state == "ready" and (.call | length) == 2' \
                "$artifact_root/readiness.json" >/dev/null; then
            return 0
        fi
        if ! kill -0 "$current_daemon_pid" 2>/dev/null; then
            return 1
        fi
        sleep 0.1
    done
    return 1
}

ubus_read()
{
    local method=$1

    ubus_json=$(ubus -S -s "$current_socket" call voice "$method" 2>/dev/null)
}

# Status 2 is the daemon leaving during the wait, which the caller has to name
# as itself: a poll that reports a timeout for a daemon that is already gone
# sends the reader after the wrong fault.
ubus_expr_wait()
{
    local method=$1
    local expression=$2
    local timeout_ms=$3
    local deadline

    ms_now
    deadline=$((ms_value + timeout_ms))
    while :; do
        if ubus_read "$method" &&
            jq -e "$expression" <<<"$ubus_json" >/dev/null 2>&1; then
            return 0
        fi
        if ! process_live "$current_daemon_pid"; then
            return 2
        fi
        ms_now
        if (( ms_value >= deadline )); then
            return 1
        fi
        sleep "$poll_s"
    done
}

daemon_log_tail()
{
    tail -20 "$artifact_root/daemon.log" 2>/dev/null
}

call_fail()
{
    local call_no=$1
    local reason=$2

    printf 'FAIL: churn call %u of %u, %s\n' "$call_no" "$calls_total" "$reason" >&2
    if ubus_read status; then
        printf '%s\n' "$ubus_json" >"$artifact_root/status-fail-$call_no.json"
        printf 'status: %s\n' "$ubus_json" >&2
    fi
    printf 'daemon log tail:\n' >&2
    daemon_log_tail >&2
    fail_reason="call $call_no: $reason"
}

# A verb the daemon never answered because it is gone is not a refusal, and
# the two read the same from the exit status of ubus alone.
verb_fail()
{
    local call_no=$1
    local verb=$2

    if ! process_live "$current_daemon_pid"; then
        call_fail "$call_no" "the daemon exited before the $verb verb answered"
        return
    fi
    call_fail "$call_no" "the $verb verb was refused"
}

# Both peers, before the call and not after it: a dead daemon or a SIPp that
# spent its budget must be named as itself and not as a poll that timed out.
peers_check()
{
    local call_no=$1

    if ! process_live "$current_daemon_pid"; then
        call_fail "$call_no" "the daemon exited before this call"
        return 1
    fi
    if ! process_live "$current_sipp_pid"; then
        call_fail "$call_no" "SIPp exited before this call, see sipp-stdout.log"
        return 1
    fi
}

call_once()
{
    local call_no=$1
    local status

    peers_check "$call_no" || return 1

    if ! ubus -s "$current_socket" call voice dial \
        "{\"line\":0,\"number\":\"$dial_number\"}" >/dev/null 2>&1; then
        verb_fail "$call_no" dial
        return 1
    fi
    ubus_expr_wait status '.call[0].state == "CONNECTED"' "$connect_timeout_ms"
    status=$?
    if (( status == 2 )); then
        call_fail "$call_no" "the daemon exited while the call was being set up"
        return 1
    fi
    if (( status != 0 )); then
        call_fail "$call_no" "the call did not reach CONNECTED within ${connect_timeout_ms} ms"
        return 1
    fi

    if [[ $churn_family = 6 ]] &&
        (( call_no == 1 || call_no == calls_total )); then
        printf '%s\n' "$ubus_json" >"$artifact_root/status-family-$call_no.json"
        if ! jq -e '.lines[0].family == "ipv6"' \
            <<<"$ubus_json" >/dev/null; then
            call_fail "$call_no" "the connected call did not report family ipv6"
            return 1
        fi
    fi

    sleep "$talk_s"

    if ! ubus -s "$current_socket" call voice release '{"line":0}' >/dev/null 2>&1; then
        verb_fail "$call_no" release
        return 1
    fi
    ubus_expr_wait status '.call[0].state == "IDLE"' "$idle_timeout_ms"
    status=$?
    if (( status == 2 )); then
        call_fail "$call_no" "the daemon exited while the call was being released"
        return 1
    fi
    if (( status != 0 )); then
        call_fail "$call_no" "the call did not return to IDLE within ${idle_timeout_ms} ms"
        return 1
    fi
}

resource_sample()
{
    local call_no=$1
    local key value rest
    local -a fd_list=()
    local rss=

    if [[ ! -r /proc/$current_daemon_pid/status ]]; then
        call_fail "$call_no" "the daemon process state could not be read"
        return 1
    fi
    while read -r key value rest; do
        if [[ $key = VmRSS: ]]; then
            rss=$value
            break
        fi
    done </proc/"$current_daemon_pid"/status
    if [[ -z $rss ]]; then
        call_fail "$call_no" "the daemon reported no VmRSS"
        return 1
    fi
    fd_list=(/proc/"$current_daemon_pid"/fd/*)
    if [[ ! -e ${fd_list[0]} ]]; then
        call_fail "$call_no" "the daemon descriptor table could not be read"
        return 1
    fi

    sample_call+=("$call_no")
    sample_rss+=("$rss")
    sample_fd+=("${#fd_list[@]}")
    printf '%u,%u,%u\n' "$call_no" "$rss" "${#fd_list[@]}" >>"$resource_csv"
}

# The slot outlives the call, and the invariant drains before it asserts. The
# leg stays in bye-sent while its BYE transaction sits in the completed state
# for timer K, which is T4 and five seconds on UDP (RFC 3261 section
# 17.1.2.2); the daemon holds the leg until the transaction terminates, and a
# next call is only that fast because leg_alloc() takes a draining slot over.
# A leaked leg never drains, so the bounded wait is what tells the two apart.
invariants_check()
{
    local call_no=$1
    local status

    ubus_expr_wait status '(.call[0].calls | length) == 0' "$drain_timeout_ms"
    status=$?
    if (( status == 2 )); then
        call_fail "$call_no" "the daemon exited at the checkpoint"
        return 1
    fi
    if (( status != 0 )); then
        call_fail "$call_no" "a call slot was still in use ${drain_timeout_ms} ms after the call: $(jq -c '.call[0].calls' <<<"$ubus_json")"
        return 1
    fi
    if ! jq -e '.events.dropped == 0 and .events.lost == 0' <<<"$ubus_json" >/dev/null; then
        call_fail "$call_no" "the event counters moved: $(jq -c '.events' <<<"$ubus_json")"
        return 1
    fi
    if ! jq -e ".hal.op_errors == $op_errors_base" <<<"$ubus_json" >/dev/null; then
        call_fail "$call_no" "hal op_errors left the baseline of $op_errors_base: $(jq -c '.hal.op_errors' <<<"$ubus_json")"
        return 1
    fi
    if ! ubus_read cdr; then
        call_fail "$call_no" "voice cdr did not answer at the checkpoint"
        return 1
    fi
    if ! jq -e "(.cdr | last | .seq) == $((cdr_seq_base + call_no))" <<<"$ubus_json" >/dev/null; then
        call_fail "$call_no" "the newest record is seq $(jq -c '.cdr | last | .seq' <<<"$ubus_json"), not $((cdr_seq_base + call_no))"
        return 1
    fi
    checked_call=$call_no
}

progress_print()
{
    local call_no=$1
    local last=$(( ${#sample_call[@]} - 1 ))

    ms_now
    printf 'progress: %u of %u calls, %u s elapsed, VmRSS %u kB, fds %u\n' \
        "$call_no" "$calls_total" "$(( (ms_value - wall_start_ms) / 1000 ))" \
        "${sample_rss[$last]}" "${sample_fd[$last]}"
}

churn_run()
{
    local call_no
    local mark

    for (( call_no = 1; call_no <= calls_total; call_no++ )); do
        call_once "$call_no" || return 1
        if (( call_no % check_every == 0 )); then
            # the checkpoint is timed apart from the calls, because its drain
            # waits out timer K and would otherwise be read as call cost
            ms_now
            mark=$ms_value
            invariants_check "$call_no" || return 1
            resource_sample "$call_no" || return 1
            ms_now
            checkpoint_ms=$((checkpoint_ms + ms_value - mark))
            progress_print "$call_no"
        fi
    done
}

# The window a mismatch belongs to is the run since the last checkpoint that
# held, because every invariant before it was answered against the ring.
#
# The span can be empty, and in the default shape it always is: a call count
# that is a multiple of the checkpoint interval makes the last call a
# checkpoint, so nothing stands between it and the end. What is left is the
# final oracle's own window, which holds the close of the last leg and the
# media snapshot behind it, and naming it that way beats printing a range that
# starts after it ends.
window_detail()
{
    if (( checked_call >= calls_total )); then
        printf 'after the final checkpoint at call %u' "$calls_total"
        return
    fi
    if (( checked_call == 0 )); then
        printf 'calls 1 through %u' "$calls_total"
        return
    fi
    printf 'calls %u through %u' "$((checked_call + 1))" "$calls_total"
}

# The record of the last call closes when its leg does, which is timer K after
# the peer answered the BYE, and its media snapshot is a HAL job that lands
# after that. The counters that only move at the end of a leg, out_failed
# among them, are read after that wait and never before it.
oracle_check()
{
    local expect_seq=$((cdr_seq_base + calls_total))
    local stats

    if ! ubus_expr_wait cdr \
        "(.cdr | last | .seq) == $expect_seq and (.cdr | last | .answered) == true and (.cdr | last | .media_state) == \"present\" and (.cdr | last | .in_progress) == null" \
        "$record_timeout_ms"; then
        ubus_read cdr
        printf '%s\n' "$ubus_json" >"$artifact_root/cdr-final.json"
        fail_reason="the newest record is not seq $expect_seq, answered, closed and with media present: $(jq -c '.cdr | last' <<<"$ubus_json"), in the window $(window_detail)"
        return 1
    fi
    printf '%s\n' "$ubus_json" >"$artifact_root/cdr-final.json"

    if ! ubus_read stats; then
        fail_reason="voice stats did not answer"
        return 1
    fi
    stats=$ubus_json
    printf '%s\n' "$stats" >"$artifact_root/stats-final.json"
    if ! jq -e ".stats[0].out_attempted == $calls_total" <<<"$stats" >/dev/null; then
        fail_reason="out_attempted is $(jq -c '.stats[0].out_attempted' <<<"$stats"), not $calls_total, in the window $(window_detail)"
        return 1
    fi
    if ! jq -e ".stats[0].out_answered == $calls_total" <<<"$stats" >/dev/null; then
        fail_reason="out_answered is $(jq -c '.stats[0].out_answered' <<<"$stats"), not $calls_total, in the window $(window_detail)"
        return 1
    fi
    if ! jq -e '.stats[0].out_failed == 0 and .stats[0].in_failed == 0' <<<"$stats" >/dev/null; then
        fail_reason="a failure was counted: $(jq -c '.stats[0]' <<<"$stats"), in the window $(window_detail)"
        return 1
    fi
    if ! ubus_read status; then
        fail_reason="voice status did not answer after the last call"
        return 1
    fi
    printf '%s\n' "$ubus_json" >"$artifact_root/status-final.json"
    # uas_churn answers every refresh, so the binding outlives a soak.
    if ! jq -e '.lines[0].status == "registered"' <<<"$ubus_json" >/dev/null; then
        fail_reason="the line lost its registration: $(jq -c '.lines[0] | {state, status, last_code}' <<<"$ubus_json"), in the window $(window_detail)"
        return 1
    fi
}

# In UAS mode SIPp has no out-of-call scenario (it refuses -oocsf in server
# mode), so every daemon REGISTER opens an instance of the one scenario.
# uas_churn answers it (test/sipp/README.md, "The REGISTER branch"). A
# CHURN_SCENARIO with no REGISTER branch aborts the instance while it waits
# for an INVITE, and those aborts are counted apart; any other abort is not
# expected.
sipp_abort_classify()
{
    local error_file=$artifact_root/sipp-errors.log

    absorbed_register=0
    absorbed_register_retrans=0
    foreign_abort=0
    foreign_abort_detail=

    # The counts of a run whose SIPp never wrote an error file are zero, and
    # the file still says so: a FAIL that carries no attribution artifact
    # leaves the reader to guess what the peer was doing.
    if [[ -f $error_file ]]; then
        absorbed_register=$(grep -c "Aborting call .*received 'REGISTER" "$error_file")
        absorbed_register_retrans=$(grep -c "Dead call .*received 'REGISTER" "$error_file")
        foreign_abort=$(grep "Aborting call" "$error_file" |
            grep -vc "received 'REGISTER")
        foreign_abort_detail=$(grep "Aborting call" "$error_file" |
            grep -v "received 'REGISTER" | head -1 |
            sed -e 's/^.*: Aborting/Aborting/' -e 's/$//' | cut -c 1-180)
    fi
    {
        printf 'absorbed REGISTER aborts: %s\n' "$absorbed_register"
        printf 'absorbed REGISTER retransmits: %s\n' "$absorbed_register_retrans"
        printf 'unattributed aborts: %s\n' "$foreign_abort"
    } >"$artifact_root/sipp-aborts.txt"
    return 0
}

# What the peer was doing, which a FAIL owes as much as a PASS: a run that
# died on call 30 still has to say whether SIPp was answering, and every
# verdict goes through this, so no terminating path reports without it.
sipp_attribution_take()
{
    sipp_abort_classify
    sipp_success=$(sipp_success_count "$artifact_root") || sipp_success=
    sipp_verdict_detail=$(printf 'SIPp successful calls %s of %u, absorbed REGISTER %s (+%s retransmits)' \
        "${sipp_success:-unknown}" "$sipp_expected" "$absorbed_register" \
        "$absorbed_register_retrans")
    if [[ -n $foreign_abort_detail ]]; then
        sipp_verdict_detail="$sipp_verdict_detail, unattributed abort: $foreign_abort_detail"
    fi
}

sipp_verdict_take()
{
    sipp_attribution_take

    if (( foreign_abort != 0 )); then
        return 1
    fi
    if [[ -z $sipp_success ]] || (( sipp_success < sipp_expected )); then
        return 1
    fi
    return 0
}

# The 10 percent mark is the soak baseline: the daemon has taken every
# allocation a call needs by then, so growth after it is growth that stays.
#
# The 5 percent bound describes the shipped shape. A sanitizer build brings an
# allocator of its own that populates its regions over the first hundred calls
# and settles there, which is growth of the tool and not of the daemon, so
# such a run states its own bound instead of pretending to meet this one.
resource_verdict_take()
{
    local i
    local mark=$(( (calls_total + 9) / 10 ))
    local last=$(( ${#sample_call[@]} - 1 ))

    if (( last < 1 )); then
        resource_detail="fewer than two resource samples were taken"
        return 1
    fi
    base_index=-1
    for (( i = 0; i <= last; i++ )); do
        if (( sample_call[i] >= mark )); then
            base_index=$i
            break
        fi
    done
    if (( base_index < 0 || base_index == last )); then
        resource_detail="no resource sample stands between the 10 percent mark of $mark calls and the end of the run"
        return 1
    fi

    rss_first=${sample_rss[0]}
    fd_first=${sample_fd[0]}
    rss_base=${sample_rss[$base_index]}
    fd_base=${sample_fd[$base_index]}
    rss_last=${sample_rss[$last]}
    fd_last=${sample_fd[$last]}
    rss_peak=$rss_base
    for (( i = base_index; i <= last; i++ )); do
        if (( sample_rss[i] > rss_peak )); then
            rss_peak=${sample_rss[i]}
        fi
    done
    resource_detail=$(printf 'VmRSS %u kB at call %u, %u kB at the 10%% mark of call %u, %u kB peak, %u kB at the end; fds %u, %u, %u' \
        "$rss_first" "${sample_call[0]}" "$rss_base" "${sample_call[$base_index]}" \
        "$rss_peak" "$rss_last" "$fd_first" "$fd_base" "$fd_last")

    for (( i = base_index; i <= last; i++ )); do
        if (( sample_fd[i] != fd_base )); then
            resource_detail="$resource_detail, the descriptor count moved to ${sample_fd[i]} at call ${sample_call[i]}"
            return 1
        fi
    done
    if (( (rss_peak - rss_base) * 100 > rss_growth_pct * rss_base )); then
        resource_detail="$resource_detail, which is more than $rss_growth_pct percent above the baseline"
        return 1
    fi
    return 0
}

daemon_verdict_take()
{
    if [[ -z $daemon_status ]]; then
        daemon_detail="the daemon exit status was not collected"
        return 1
    fi
    daemon_detail="daemon exit status $daemon_status"
    (( daemon_status == 0 ))
}

verdict_print()
{
    local status=$1
    local detail=$2

    if [[ $status = PASS && $churn_family = 6 ]]; then
        detail="$detail; family ipv6 on first and last call"
    fi

    if [[ $status = FAIL ]]; then
        # SIPp prints its counts only on the way out, and the verdict is the
        # last thing this run does, so a peer still running here would be
        # attributed as "unknown" for no reason. The aborts are read from the
        # error file either way.
        process_stop "$current_sipp_pid"
        current_sipp_pid=
        sipp_attribution_take
        detail="$detail; $sipp_verdict_detail"
    fi
    printf '%s: churn %u calls, %s\n' "$status" "$calls_total" "$detail" \
        >"$artifact_root/verdict.txt"
    cat "$artifact_root/verdict.txt"
}

# The three numbers of a scenario's pauses: the total of the literal ones, how
# many carry no literal length, and how many pause elements there are at all.
# A tag this cannot read whole is counted with the free ones, so an opening
# tag that spans lines is refused rather than silently skipped.
scenario_pause_scan()
{
    awk '
        {
            line = $0
            while (match(line, /<pause/)) {
                rest = substr(line, RSTART)
                line = substr(line, RSTART + 6)
                seen++
                if (!match(rest, /^<pause[^>]*>/)) {
                    free++
                    continue
                }
                tag = substr(rest, RSTART, RLENGTH)
                if (match(tag, /milliseconds="[0-9]+"/))
                    total += substr(tag, RSTART + 14, RLENGTH - 15)
                else
                    free++
            }
        }
        END { printf "%u %u %u\n", total + 0, free + 0, seen + 0 }
    ' "$1"
}

main()
{
    local sipp_port daemon_port media_port port_seed
    local budget sipp_timeout_s scenario_pause_ms scenario_pause_free
    local config_dir

    if [[ ${1:-} = -h || ${1:-} = --help ]]; then
        usage_print
        return 0
    fi

    calls_total=${1:-1000}
    if [[ ! $calls_total =~ ^[1-9][0-9]*$ ]]; then
        printf 'ERROR: the call count must be a positive integer: %s\n' "$calls_total" >&2
        return 2
    fi
    sipp_expected=$((calls_total + 1))
    if [[ $churn_family != 4 && $churn_family != 6 ]]; then
        printf 'ERROR: CHURN_FAMILY must be 4 or 6: %s\n' "$churn_family" >&2
        return 2
    fi
    # The resource criterion needs a sample strictly between the 10 percent
    # mark and the end, and below ten calls no such sample exists; the first
    # calls are also where the allocator settles, so a baseline taken there
    # measures the start up and not the run.
    if (( calls_total < 10 )); then
        printf 'ERROR: the call count must be at least 10, because the resource baseline needs a sample between the 10 percent mark and the end of the run\n' >&2
        return 2
    fi
    # The checkpoint default is 100 calls, lowered for a short run so that the
    # invariants and the 10 percent resource mark both exist there too.
    check_every=${CHURN_CHECK_EVERY:-$(( calls_total / 10 < 100 ? calls_total / 10 : 100 ))}
    if (( check_every < 1 )); then
        check_every=1
    fi

    artifact_root=$(path_absolute "$artifact_root") || return 2
    sipp_bin=$(path_absolute "$sipp_bin") || return 2
    scenario_file=$(path_absolute "$scenario_file") || return 2
    mkdir -p "$artifact_root" || return 2
    prerequisites_check || return 2
    daemon_build "${CHURN_BUILD_JOBS:-4}" || return 2

    if [[ ${UVOICE_CHURN_USERNS:-0} != 1 ]]; then
        export CHURN_ARTIFACTS=$artifact_root
        export CHURN_SCENARIO=$scenario_file
        export CHURN_CHECK_EVERY=$check_every
        export CHURN_FAMILY=$churn_family
        export SIPP_BIN=$sipp_bin
        export UVOICE_HOST_PREFIX=$host_prefix
        export UVOICE_CHURN_USERNS=1
        # ubusd grants object publication to root only, and a private network
        # namespace keeps the drawn ports away from a parallel run.
        if unshare -r -n true 2>/dev/null; then
            exec unshare -r -n "$script_dir/${0##*/}" "$@"
        fi
        printf '%s\n' "WARNING: no private network namespace, the ports are the host's" >&2
        exec unshare -r "$script_dir/${0##*/}" "$@"
    fi

    if ! loopback_prepare; then
        printf '%s\n' "ERROR: loopback interface is not up" >&2
        return 2
    fi

    # A BYE that arrives before the scenario reaches its recv is an unexpected
    # message and aborts the call, so the talk pause has to clear the scripted
    # one with room for the ubus round trip that ends it. EVERY pause of the
    # scenario counts, because they all stand between the answer and the recv,
    # and a pause whose length is not a literal is refused: a distribution, a
    # variable or a scenario default is a duration this harness cannot budget
    # for, and reading it as zero would arm the abort it exists to prevent.
    read -r scenario_pause_ms scenario_pause_free _ \
        <<<"$(scenario_pause_scan "$scenario_file")"
    if (( scenario_pause_free != 0 )); then
        printf 'ERROR: %s carries %u pause element(s) with no literal milliseconds attribute, so the talk pause cannot be budgeted\n' \
            "$scenario_file" "$scenario_pause_free" >&2
        return 2
    fi
    if (( talk_ms < scenario_pause_ms + 50 )); then
        printf 'ERROR: the talk pause of %u ms does not clear the scenario pause of %u ms by 50 ms\n' \
            "$talk_ms" "$scenario_pause_ms" >&2
        return 2
    fi
    printf -v talk_s '%u.%03u' "$((talk_ms / 1000))" "$((talk_ms % 1000))"
    printf -v poll_s '%u.%03u' "$((poll_ms / 1000))" "$((poll_ms % 1000))"

    trap exit_cleanup EXIT
    trap 'exit 130' HUP INT TERM
    port_seed=$((20000 + ($$ % 500) * 20))
    sipp_port=${CHURN_SIPP_PORT:-$port_seed}
    daemon_port=${CHURN_DAEMON_PORT:-$((port_seed + 1))}
    media_port=${CHURN_MEDIA_PORT:-$((port_seed + 2))}

    resource_csv=$artifact_root/resources.csv
    printf 'call,rss_kb,fds\n' >"$resource_csv"
    config_dir=$artifact_root/config
    config_write "$config_dir" "$sipp_port" "$daemon_port" || return 2

    budget=$((calls_total + sipp_headroom))
    # Sized from the run, never the fixed 90 seconds of the flow harness: one
    # call costs the talk pause plus its two ubus calls and the polls between
    # them, and the scenario carries its own per message timeouts.
    sipp_timeout_s=$(( calls_total * (talk_ms + 900) / 1000 + 300 ))

    if ! sipp_start "$sipp_port" "$daemon_port" "$media_port" "$budget" "$sipp_timeout_s"; then
        if [[ $churn_family = 6 && -n $fail_reason ]]; then
            verdict_print FAIL "$fail_reason"
        else
            verdict_print FAIL "SIPp did not start, see sipp-stdout.log"
        fi
        return 1
    fi
    if ! daemon_start "$config_dir" churn; then
        verdict_print FAIL "the daemon did not reach a ready state, see daemon.log"
        return 1
    fi
    # The status and not the state: a line in REFRESHING still holds its
    # binding, and "voice status" reports it as "registered" too.
    if ! ubus_expr_wait status '.lines[0].status == "registered"' 15000; then
        printf '%s\n' "$ubus_json" >"$artifact_root/status-registered-timeout.json"
        verdict_print FAIL "the line did not register before the first call, see daemon.log"
        return 1
    fi
    printf '%s\n' "$ubus_json" >"$artifact_root/status-registered.json"

    if ! ubus_read status; then
        verdict_print FAIL "voice status did not answer before the first call"
        return 1
    fi
    printf '%s\n' "$ubus_json" >"$artifact_root/status-baseline.json"
    op_errors_base=$(jq -r '.hal.op_errors' <<<"$ubus_json")
    if ! ubus_read stats; then
        verdict_print FAIL "voice stats did not answer before the first call"
        return 1
    fi
    printf '%s\n' "$ubus_json" >"$artifact_root/stats-baseline.json"
    if ! ubus_read cdr; then
        verdict_print FAIL "voice cdr did not answer before the first call"
        return 1
    fi
    cdr_seq_base=$(jq -r '(.cdr | last | .seq) // 0' <<<"$ubus_json")

    printf 'churn: %u calls, checkpoint every %u, talk pause %u ms, SIPp budget %u, daemon %s\n' \
        "$calls_total" "$check_every" "$talk_ms" "$budget" "$UVOICED"

    fail_reason=
    ms_now
    wall_start_ms=$ms_value
    resource_sample 0 || { verdict_print FAIL "$fail_reason"; return 1; }
    if ! churn_run; then
        ms_now
        wall_ms=$((ms_value - wall_start_ms))
        verdict_print FAIL "$fail_reason"
        return 1
    fi
    ms_now
    wall_ms=$((ms_value - wall_start_ms))
    if (( calls_total % check_every != 0 )); then
        resource_sample "$calls_total" || { verdict_print FAIL "$fail_reason"; return 1; }
    fi

    if ! oracle_check; then
        verdict_print FAIL "$fail_reason"
        return 1
    fi
    # SIPp first, because its own statistics screen is what the count is read
    # from and it only prints one on the way out.
    process_stop "$current_sipp_pid"
    current_sipp_pid=
    # the detail itself comes from the attribution every FAIL already carries,
    # so this names the gate and nothing more
    if ! sipp_verdict_take; then
        verdict_print FAIL "the SIPp side did not hold"
        return 1
    fi
    process_stop "$current_daemon_pid" 20
    daemon_status=$process_status
    current_daemon_pid=
    if ! daemon_verdict_take; then
        verdict_print FAIL "$daemon_detail, see daemon.log"
        return 1
    fi
    if ! resource_verdict_take; then
        verdict_print FAIL "$resource_detail"
        return 1
    fi

    printf 'Artifacts: %s\n' "$artifact_root"
    printf 'daemon log tail:\n'
    daemon_log_tail
    # Both figures, because the checkpoint drain waits out timer K and a short
    # run is mostly drain: the overall number is what the run costs, the
    # second is what a call costs.
    verdict_print PASS "$(printf '%s, %s, %s, %u ms per call overall, %u ms between checkpoints, over %u ms with %u ms in checkpoints' \
        "$sipp_verdict_detail" "$resource_detail" "$daemon_detail" \
        "$((wall_ms / calls_total))" "$(((wall_ms - checkpoint_ms) / calls_total))" \
        "$wall_ms" "$checkpoint_ms")"
    return 0
}

main "$@"
