#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
#
# SIPp 3.6.1 or later is required. The scenarios use named variables,
# transaction matching, inverse regular-expression checks, and RTP echo.
# The runner uses command options present in SIPp 3.6.1 and later.

set -u

script_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
all_scenarios='uas_register uas_register_auth_2069 uas_register_auth_qop uas_register_423 uas_basic_call uac_basic_call uac_verstat_call uas_service_route uas_service_route_none uas_reinvite_hold uas_offerless_reinvite uas_legacy_hold uas_t38_reject uas_options_keepalive uac_cancel uas_491_glare uas_glare_mutual uas_503_retry_after uas_session_timer_422 uac_prack_supported_plain uac_prack_require_plain uas_prack_180 uas_prack_dup_180 uas_prack_gap uas_prack_183_sdp uas_prack_auth uas_183_sdp'
current_capture_pid=

usage_print()
{
    cat <<EOF
Usage: $0 DUT_ADDRESS SCENARIO [SCENARIO ...]
       $0 DUT_ADDRESS all

Scenario names can include or omit the .xml suffix.
Set SIPP_LOCAL_IP when SIPp cannot select the correct source address.
Set SIPP_BIN to select a SIPp binary other than the one on PATH.
Set SIPP_KEYS to replace the "keyword value" pairs of a scenario that
names a SIPp keyword, for example "verstat TN-Validation-Passed".
A SIPP_TRANSPORT of l1 or ln needs a SIPp built with USE_SSL=ON.
EOF
}

capture_stop()
{
    if [ -z "$current_capture_pid" ]; then
        return
    fi

    kill -INT "$current_capture_pid" 2>/dev/null || true
    wait "$current_capture_pid" 2>/dev/null || true
    current_capture_pid=
}

scenario_name_normalize()
{
    requested_name=$1
    case "$requested_name" in
        */*|*..*)
            return 1
            ;;
    esac

    normalized_name=${requested_name%.xml}
    if [ ! -f "$script_dir/$normalized_name.xml" ]; then
        return 1
    fi

    printf '%s\n' "$normalized_name"
}

# The SIPp -key arguments of one scenario, as whitespace separated "keyword
# value" pairs. SIPp stops with "Unsupported keyword" when a scenario uses a
# generic keyword that the command line does not define with -key (SIPp 3.7.3
# src/message.cpp), so a scenario that names one carries its default here.
# SIPP_KEYS replaces the whole list, which is how the verstat matrix runs the
# other two values.
scenario_keys_get()
{
    if [ -n "${SIPP_KEYS:-}" ]; then
        printf '%s\n' "$SIPP_KEYS"
        return
    fi

    case "$1" in
        uac_verstat_call)
            printf '%s\n' 'verstat No-TN-Validation'
            ;;
        uas_register_prime)
            printf '%s\n' 'expires 3600'
            ;;
    esac
}

local_ip_find()
{
    if [ -n "${SIPP_LOCAL_IP:-}" ]; then
        printf '%s\n' "$SIPP_LOCAL_IP"
        return
    fi

    if ! command -v ip >/dev/null 2>&1; then
        return
    fi

    ip route get "$1" 2>/dev/null |
        awk '{ for (i = 1; i <= NF; i++) if ($i == "src") { print $(i + 1); exit } }'
}

capture_start()
{
    capture_dut=$1
    capture_file=$2
    capture_log=$3

    if ! command -v tcpdump >/dev/null 2>&1; then
        printf '%s\n' "INFO: tcpdump absent, packet capture skipped"
        return 1
    fi

    tcpdump -i "${SIPP_CAPTURE_INTERFACE:-any}" -U -s 0 \
        -w "$capture_file" host "$capture_dut" >"$capture_log" 2>&1 &
    current_capture_pid=$!
    sleep 1

    if kill -0 "$current_capture_pid" 2>/dev/null; then
        return 0
    fi

    wait "$current_capture_pid" 2>/dev/null || true
    current_capture_pid=
    printf '%s\n' "INFO: tcpdump could not start, packet capture skipped"
    return 1
}

timing_503_check()
{
    pcap_file=$1
    timing_file=$2

    tshark -r "$pcap_file" \
        -Y 'sip.Method == "REGISTER" || sip.Status-Code == 503' \
        -T fields -E separator='|' \
        -e frame.time_epoch -e sip.Method -e sip.Status-Code \
        >"$timing_file" 2>/dev/null || return 1

    awk -F '|' '
        $3 == "503" { response_time = $1; response_seen = 1; next }
        response_seen && $2 == "REGISTER" {
            delta = $1 - response_time
            printf "503 retry delay: %.6f seconds\n", delta
            retry_seen = 1
            if (delta >= 5.0) exit 0
            exit 1
        }
        END { if (!retry_seen) exit 2 }
    ' "$timing_file"
}

# The retry a DUT owes after a 491, read off the capture. Only an INVITE the
# DUT sent counts: uas_glare_mutual has the peer put an INVITE of its own on
# the wire at 1.9 s, in front of the retry, so a reader that took the first
# INVITE of either direction would measure the wrong message.
#
# The source address is what names the DUT here. The bench runner knows no
# port of the DUT, and the address is the one capture_start already filtered
# on: tcpdump refuses "host <address>:<port>", so a run that holds a capture
# at all was given a bare address. A colon in it names an IPv6 one.
#
# A run of both ends on one host cannot be separated this way. The end-to-end
# harness is the loopback one, and it names the daemon port instead
# (test/e2e/run_e2e.sh, timing_491_check()).
#
# The refusal of tshark is printed and not swallowed. The filter now carries
# an operator argument: a host name or a bracketed IPv6 address is no value
# for an address field, and tshark then refuses the whole expression. Without
# the text the operator reads a bare FAIL and no reason.
timing_491_check()
{
    pcap_file=$1
    timing_file=$2
    dut_host=$3
    src_field=ip.src

    case "$dut_host" in
        *:*)
            src_field=ipv6.src
            ;;
    esac

    if ! tshark_reason=$(tshark -r "$pcap_file" \
        -Y "(sip.Method == \"INVITE\" && $src_field == $dut_host) || sip.Status-Code == 491" \
        -T fields -E separator='|' \
        -e frame.time_epoch -e sip.Method -e sip.Status-Code \
        2>&1 >"$timing_file"); then
        printf 'tshark refused the 491 filter for DUT %s: %s\n' \
            "$dut_host" "$tshark_reason"
        return 1
    fi

    awk -F '|' '
        $3 == "491" { response_time = $1; response_seen = 1; next }
        response_seen && $2 == "INVITE" {
            delta = $1 - response_time
            printf "491 retry delay: %.6f seconds\n", delta
            retry_seen = 1
            if (delta >= 2.1 && delta <= 4.0) exit 0
            exit 1
        }
        END { if (!retry_seen) exit 2 }
    ' "$timing_file"
}

rfc2833_check()
{
    pcap_file=$1
    payload_type=${SIPP_RFC2833_PAYLOAD:-101}

    tshark -r "$pcap_file" -Y "rtp.p_type == $payload_type" \
        -T fields -e rtp.p_type 2>/dev/null |
        awk -v payload_type="$payload_type" '$1 == payload_type { found = 1 } END { exit !found }'
}

# The INVITE the two service route scenarios exist for, read off the SIPp
# message log. A DUT that sent two REGISTERs and no INVITE satisfies the call
# count of the run, and SIPp reports success, so the run has to ask for the
# request the field case is about. want_route selects the granting scenario,
# whose every INVITE must carry the two hops of the 200, one Route header line
# each and in the granted order (test/sipp/uas_service_route.xml).
service_route_msg_check()
{
    message_file=$1
    want_route=$2

    if [ ! -f "$message_file" ]; then
        printf '%s\n' "no SIPp message log to read"
        return 1
    fi

    awk -v want_route="$want_route" '
        function msg_end() {
            if (!matched)
                return
            invites++
            if (route != want)
                bad = bad "INVITE " invites " carried [" route "]; "
        }
        /(UDP|TCP|TLS) message sent/ { msg_end(); dir = "sent"; first = 1; matched = 0; route = ""; next }
        /(UDP|TCP|TLS) message received/ { msg_end(); dir = "received"; first = 1; matched = 0; route = ""; next }
        first {
            if ($0 ~ /^[[:space:]]*$/)
                next
            line = $0
            gsub(/\r/, "", line)
            matched = (dir == "received" && index(line, "INVITE ") == 1)
            first = 0
            body = 0
            next
        }
        matched && !body {
            line = $0
            gsub(/\r/, "", line)
            if (line ~ /^[[:space:]]*$/) {
                body = 1
                next
            }
            if (index(line, "Route:") != 1)
                next
            value = substr(line, length("Route:") + 1)
            sub(/^[ \t]+/, "", value)
            sub(/[ \t]+$/, "", value)
            route = route (route == "" ? "" : " ") value
        }
        BEGIN {
            want = ""
            if (want_route == "1")
                want = "<sip:sbc1.example.net:5065;lr> <sip:sbc2.example.net:5065;lr>"
        }
        END {
            msg_end()
            if (invites < 1) {
                print "the DUT sent no INVITE during the run"
                exit 1
            }
            if (bad != "") {
                printf "%u INVITE(s) received, wanted Route [%s]: %s\n", invites, want, bad
                exit 1
            }
            printf "%u INVITE(s) received, each with Route [%s]\n", invites, want
        }
    ' "$message_file"
}

postcheck_run()
{
    scenario_name=$1
    pcap_file=$2
    scenario_dir=$3
    capture_ready=$4
    dut_host=$5

    case "$scenario_name" in
        uas_491_glare|uas_glare_mutual|uas_503_retry_after)
            if [ "$capture_ready" -ne 1 ]; then
                printf '%s\n' "Packet timestamps required for $scenario_name"
                return 1
            fi
            if ! command -v tshark >/dev/null 2>&1; then
                printf '%s\n' "tshark required for $scenario_name"
                return 1
            fi
            ;;
    esac

    case "$scenario_name" in
        uas_491_glare|uas_glare_mutual)
            timing_491_check "$pcap_file" "$scenario_dir/timing.txt" \
                "$dut_host" || return 1
            ;;
        uas_503_retry_after)
            timing_503_check "$pcap_file" "$scenario_dir/timing.txt" || return 1
            ;;
        uas_service_route)
            service_route_msg_check "$scenario_dir/messages.log" 1 || return 1
            ;;
        uas_service_route_none)
            service_route_msg_check "$scenario_dir/messages.log" 0 || return 1
            ;;
    esac

    if [ "${SIPP_RFC2833_CHECK:-0}" -eq 1 ]; then
        if [ "$capture_ready" -ne 1 ] || ! command -v tshark >/dev/null 2>&1; then
            printf '%s\n' "tcpdump and tshark required for the RFC2833 check"
            return 1
        fi
        rfc2833_check "$pcap_file" || return 1
    fi

    return 0
}

scenario_run()
{
    scenario_name=$1
    dut_address=$2
    local_ip=$3
    run_root=$4
    scenario_file=$script_dir/$scenario_name.xml
    scenario_dir=$run_root/$scenario_name
    pcap_file=$scenario_dir/$scenario_name.pcap
    capture_ready=0
    call_count=1
    call_limit=
    pending_keyword=

    mkdir -p "$scenario_dir" || return 1

    if [ "$scenario_name" = uas_options_keepalive ]; then
        call_count=3
    fi

    # Fifty offered calls, one at a time. The subscriber under test is one
    # line, so a second offer inside the ring of the first earns a busy answer
    # and not the field sequence. The scenario is absent from the all
    # set: half its calls wait for somebody to lift the handset, which no
    # unattended bench run provides (test/sipp/README.md, "The inbound churn
    # scenario").
    if [ "$scenario_name" = uac_inbound_churn ]; then
        call_count=50
        call_limit=1
    fi

    # The two service route scenarios answer a REGISTER and an INVITE, and the
    # two requests carry different Call-IDs, so each run holds two scenario
    # instances (test/sipp/README.md, "The two service route scenarios").
    case "$scenario_name" in
        uas_service_route|uas_service_route_none)
            call_count=2
            ;;
    esac

    # The -key arguments of the scenario, before the capture starts: a pair
    # list this refuses must leave no tcpdump behind. The split runs with
    # pathname expansion off, because a value that held a * would take the
    # file names beside it. A scenario with no keyword adds no argument and
    # keeps the command line it always had.
    set -f
    set -- "$dut_address"
    for key_field in $(scenario_keys_get "$scenario_name"); do
        if [ -z "$pending_keyword" ]; then
            pending_keyword=$key_field
            continue
        fi
        set -- "$@" -key "$pending_keyword" "$key_field"
        pending_keyword=
    done
    set +f

    if [ -n "$pending_keyword" ]; then
        printf '%s\n' "SIPP_KEYS needs keyword and value pairs: ${SIPP_KEYS:-}"
        return 1
    fi

    if capture_start "$dut_address" "$pcap_file" "$scenario_dir/tcpdump.log"; then
        capture_ready=1
    fi

    set -- "$@" \
        -sf "$scenario_file" \
        -m "$call_count" \
        -l "${SIPP_CALL_LIMIT:-${call_limit:-$call_count}}" \
        -p "${SIPP_PORT:-5060}" \
        -mp "${SIPP_MEDIA_PORT:-6000}" \
        -recv_timeout "${SIPP_RECV_TIMEOUT_MS:-120000}" \
        -timeout "${SIPP_TIMEOUT_SECONDS:-180}" \
        -timeout_error \
        -nostdin \
        -trace_err -error_file "$scenario_dir/errors.log" \
        -trace_msg -message_file "$scenario_dir/messages.log" \
        -trace_screen -screen_file "$scenario_dir/screen.log"

    if [ -n "$local_ip" ]; then
        set -- "$@" -i "$local_ip"
    fi

    # The transfer scenarios in which the DUT places a call of its own need a
    # second scenario to answer it, and SIPp takes exactly one out-of-call
    # scenario. An empty value adds no argument, so every other scenario keeps
    # the command line it always had.
    if [ -n "${SIPP_OOC_SCENARIO:-}" ]; then
        set -- "$@" -oocsf "$SIPP_OOC_SCENARIO"
    fi

    # [service], the user part of the request URI a UAC scenario writes. The
    # transfer scenarios name a subscriber with it.
    if [ -n "${SIPP_SERVICE:-}" ]; then
        set -- "$@" -s "$SIPP_SERVICE"
    fi

    # The three stream scenarios need the DUT and SIPp on the same reliable
    # transport. An empty SIPP_TRANSPORT adds no argument, so every other
    # scenario keeps the udp command line it always had.
    case "${SIPP_TRANSPORT:-}" in
        '')
            ;;
        l1|ln)
            set -- "$@" -t "${SIPP_TRANSPORT}" \
                -tls_cert "${SIPP_TLS_CERT:-$script_dir/../unit/tls/server.crt}" \
                -tls_key "${SIPP_TLS_KEY:-$script_dir/../unit/tls/server.key}"
            ;;
        *)
            set -- "$@" -t "${SIPP_TRANSPORT}"
            ;;
    esac

    "${SIPP_BIN:-sipp}" "$@" >"$scenario_dir/stdout.log" 2>&1
    sipp_status=$?
    capture_stop

    if [ "$sipp_status" -ne 0 ]; then
        printf '%s\n' "SIPp exit status: $sipp_status"
        return 1
    fi

    postcheck_run "$scenario_name" "$pcap_file" "$scenario_dir" \
        "$capture_ready" "$dut_address"
}

if [ "$#" -lt 2 ]; then
    usage_print >&2
    exit 2
fi

# SIPP_BIN names the binary, the way test/e2e/run_e2e.sh names it. It exists
# because a l1 or ln run needs a SIPp built with USE_SSL=ON, and that is a
# second binary: the TLS build fails every scenario that carries rtp_echo, so
# it cannot be the one on PATH for everything.
if ! command -v "${SIPP_BIN:-sipp}" >/dev/null 2>&1; then
    printf '%s\n' "ERROR: sipp is not installed or is not in PATH: ${SIPP_BIN:-sipp}" >&2
    exit 2
fi

# The scenario lint, once, before any scenario runs. A nop behind a checking
# message erases the failure of that check. A scenario with that shape then
# reports a pass it never earned (test/sipp/README.md, "When a failed check
# is judged").
#
# The lint reads every scenario of this directory and of test/e2e, not the
# selection alone: the file the next operator asks for must be sound too. The
# e2e directory holds the out-of-call scenarios. The lint ships beside
# this runner, so an absent file is a broken copy and not a bench that lacks
# a tool. It is run through sh, so it needs no execute bit.
lint_script=$script_dir/../tools/sipp_scenario_lint.sh
if [ ! -f "$lint_script" ]; then
    printf '%s\n' "ERROR: the scenario lint is absent: $lint_script" >&2
    exit 2
fi
e2e_dir=$script_dir/../e2e
if ! lint_output=$(sh "$lint_script" "$script_dir" "$e2e_dir" 2>&1); then
    printf '%s\n' "$lint_output" >&2
    printf '%s\n' "ERROR: the scenario lint refused the suite" >&2
    exit 2
fi

dut_address=$1
shift

if [ "$#" -eq 1 ] && [ "$1" = all ]; then
    selected_scenarios=$all_scenarios
else
    selected_scenarios=
    for requested_scenario do
        if ! normalized_scenario=$(scenario_name_normalize "$requested_scenario"); then
            printf '%s\n' "ERROR: unknown scenario: $requested_scenario" >&2
            exit 2
        fi
        selected_scenarios="$selected_scenarios $normalized_scenario"
    done
fi

local_ip=$(local_ip_find "$dut_address")
artifact_root=${SIPP_ARTIFACT_DIR:-$script_dir/artifacts}
run_root=$artifact_root/$(date '+%Y%m%d-%H%M%S')-$$
mkdir -p "$run_root" || exit 2

trap 'capture_stop; exit 130' HUP INT TERM
failure_count=0

for scenario_name in $selected_scenarios; do
    printf '%s\n' "RUN: $scenario_name"
    if scenario_run "$scenario_name" "$dut_address" "$local_ip" "$run_root"; then
        printf '%s\n' "PASS: $scenario_name"
    else
        printf '%s\n' "FAIL: $scenario_name"
        failure_count=$((failure_count + 1))
    fi
done

printf '%s\n' "Artifacts: $run_root"

if [ "$failure_count" -ne 0 ]; then
    printf '%s\n' "Failed scenarios: $failure_count"
    exit 1
fi

exit 0
