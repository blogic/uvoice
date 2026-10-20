#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.

set -u

script_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
uvoice_dir=$(CDPATH='' cd "$script_dir/../.." && pwd)
# shellcheck source=lib.sh
. "$script_dir/lib.sh"
sipp_dir=$uvoice_dir/test/sipp
default_sipp=/tmp/uvoice-host/sipp/sipp-3.7.3/sipp
sipp_bin=${SIPP_BIN:-$default_sipp}
# The second SIPp build, and the only one the tls flows use. It is a separate
# binary because a TLS-enabled SIPp fails every plain-RTP rtp_echo flow of the
# udp set (test/e2e/README.md, "The two SIPp builds").
default_sipp_tls=/tmp/uvoice-host/sipp-tls-build/sipp
sipp_tls_bin=${SIPP_TLS_BIN:-$default_sipp_tls}
host_prefix=${UVOICE_HOST_PREFIX:-/tmp/uvoice-host/prefix}
artifact_root=${E2E_ARTIFACTS:-./e2e-artifacts}
build_dir=
current_daemon_pid=
current_ubusd_pid=
current_sipp_pid=
current_capture_pid=
current_socket=
snapshot_index=0
failure_count=0
partial_count=0
capture_ready=0
process_status=0
sipp_completed=0
absorbed_register=0
absorbed_register_retrans=0
foreign_abort=0
foreign_abort_detail=
sipp_verdict_detail=
# The transport of the running flow, empty for udp. It selects the SIPp binary,
# the SIPp -t mode and the /proc table the readiness probe reads, so one
# variable carries what would otherwise be three arguments through four
# functions.
flow_transport=
# The address family of the whole run, from E2E_FAMILY. 4 is the run every
# default set makes and 6 is the run of the v6 set (test/e2e/README.md,
# "Address family knob"). It is one knob for the whole run and not a per-flow
# one: a flow name selects what is driven, and the family selects the address
# every part of the run uses, so the two never disagree inside a flow.
flow_family=${E2E_FAMILY:-4}
# Why loopback_prepare refused. The v4 text is the one the runner always
# printed. The v6 arm replaces it with the reason the bind gave, so a run that
# asks for ::1 on a host without it names the address and not the link state.
loopback_failure="ERROR: loopback interface is not up"
# What the family readers of the running flow found, for its verdict line, and
# the reason the failing one gives. Both are empty under 4, so every verdict
# line of the default set reads as it always did.
family_detail=
family_failure_detail=
# What the PRACK readers of the running flow found, for its verdict line, and
# the reason the failing one gives. Both are empty outside the prack flows.
prack_detail=
prack_failure_detail=
# Why one verstat value of the verstat-cid flow failed. The flow places three
# calls under one name, so the verdict line has to name the value that broke.
verstat_failure_detail=
# The frozen bundle directory of sip/tls.h. It is not a variable of the daemon,
# so the harness cannot move it out of the way of another run.
tls_bundle_dir=/tmp/voip_tls
tls_bundle_owned=0
# The status file of files/uvoice_tls_cert.sh, which support tooling reads.
# It is a fixed path too, and the lock below covers both.
tls_status_file=/tmp/voip_tls_stat.txt
# Set only by the flow that drives the helper. The bundle token above cannot
# stand in for it: the stream flows compose the bundle by hand and never run
# the helper, so a status file beside one, and the four scratch files the
# helper keeps beside it, belong to somebody else.
tls_status_owned=0
# The one lock over both frozen paths. The bundle directory and the status file
# are absolute paths outside the network namespace, so two harness runs on one
# host meet on them. Per-file existence tests cannot serialise them: they are
# three separate reads, and a run could win one and lose another between them.
# A directory create is the atomic primitive every filesystem gives, so one
# lock directory is taken before either path is read or written and dropped
# after the cleanup. A lock left behind by a killed run names its holder, which
# is what an operator needs to clear it by hand.
tls_lock_dir=/tmp/voip_tls.lock
tls_lock_held=0
current_http_pid=
# The hook driver of inbound-churn, which runs beside SIPp for the length of
# that flow and answers the calls the scenario answers. It is stopped with the
# rest of the flow: it polls the daemon log and the ubus socket, and both are
# gone once the flow ends.
current_hook_pid=
# The netifd stub of wan-flap-outage, which runs beside the daemon for the
# length of that flow. It is stopped with the rest of the flow because it
# holds the ubus socket the teardown removes: a stub left behind would meet
# the socket of the next flow.
current_stub_pid=
# The scripted call agent of an MGCP flow (mgcp_ca.py). It stops after the
# daemon, so the RSIP of the daemon stop still gets its answer.
current_ca_pid=
skip_count=0
# Whether the daemon under test carries the MGCP protocol: unknown until the
# first MGCP flow reads "signaling.available", then yes or no. A no skips every
# later MGCP flow without a daemon start.
mgcp_image=unknown
mgcp_skip_detail="the daemon carries no MGCP protocol: build it with -DUVOICE_PROTO_MGCP=ON"
# T-HIST of the MGCP config. The agent waits that long behind a provisional
# response for its final one (RFC 3435 3.5.6).
mgcp_t_hist_s=10

usage_print()
{
    local usage="Usage: $0 [flow-a|flow-b|flow-c|srtp-outbound|"

    usage+="srtp-inbound|srtp-accept-plain|srtp-refused|"
    usage+="g726-call|g726-refused|g722-call|g722-refused|opus-call|"
    usage+="opus-refused|opus-narrow-dtmf|stream-uac-tcp|stream-uac-tls|"
    usage+="stream-uas-tcp|stream-uas-tls|stream-register-tcp|"
    usage+="stream-register-tls|stream-subscribe-tcp|stream-subscribe-tls|"
    usage+="cert-fetch|reload-loop|xfer-blind|xfer-blind-grace|xfer-attended|xfer-failed|"
    usage+="xfer-transferee|xfer-transferee-replaces|xfer-child-fail|"
    usage+="xfer-replaces|xfer-onhook|xfer-onhook-blind|xfer-two-line|star-network|conf-join|conf-drop|"
    usage+="conf-split|conf-onhook-xfer|conf-onhook-xfer-auth|"
    usage+="conf-remote-bye|conf-ineligible|"
    usage+="legacy-hold|glare-mutual|conf-legacy-hold|hold-dial-abandon|"
    usage+="hold-dial-abandon-onhook|loop-test|"
    usage+="prack-require|prack-supported|prack-supported-plain|"
    usage+="prack-require-plain|"
    usage+="prack-bad-rack|prack-late|prack-giveup|prack-cancel|"
    usage+="prack-uac-180|prack-uac-dup|prack-uac-gap|prack-uac-auth|"
    usage+="prack-early-media|prack-early-media-off|early-media-183|"
    usage+="verstat-cid|inbound-churn|"
    usage+="service-route-absent|service-route-present|"
    usage+="wan-flap-outage|reg-403-reorder|reg-none-reorder|"
    usage+="mgcp-restart|mgcp-outbound|mgcp-inbound-cid-ring|"
    usage+="mgcp-call-waiting|mgcp-hold|mgcp-fax-tone|mgcp-disconnected|"
    usage+="mgcp-piggyback|mgcp-response-cache|mgcp-ncs|"
    usage+="SCENARIO ...]"
    printf '%s\n' "$usage"
    printf '%s\n' "Scenarios: uas_reinvite_hold uas_offerless_reinvite uas_t38_reject uas_session_timer_422 uac_cancel uac_reinvite_hold uas_491_glare uas_503_retry_after uas_mwi_notify uas_cfu_redirect uas_fax_upspeed uas_fax_upspeed_auth uas_far_fax_upspeed uas_far_fax_upspeed_g729 uas_far_fax_tone_g711 uas_fax_upspeed_false uas_far_fax_tone_false uac_t38_offer uac_t38_reject daemon_dsp_fatal daemon_dsp_transient daemon_dsp_fatal_late daemon_dsp_transient_late daemon_dsp_fatal_sync"
}

# True when the run drives at least one tls flow, which is what makes the
# second SIPp build a prerequisite. An empty request list is the default set,
# and the default set carries tls flows.
tls_flow_requested()
{
    local item

    if (( $# == 0 )); then
        return 0
    fi

    for item in "$@"; do
        case $item in
            *-tls)
                return 0
                ;;
        esac
    done

    return 1
}

prerequisites_check()
{
    local command_name
    local lint_script
    local lint_output

    # python3 joins the list with the SRTP flows: their digest pin recomputes
    # the stub's zlib crc32 from the base64 key the wire carried, and every
    # default run drives those flows. The stream flows use it again, for
    # the CRLF ping client and the cleartext reading of the tls capture.
    for command_name in jq ubus unshare python3; do
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

    if tls_flow_requested "$@" && [[ ! -x $sipp_tls_bin ]]; then
        printf 'ERROR: the TLS SIPp build is not executable: %s\n' "$sipp_tls_bin" >&2
        return 1
    fi

    if [[ ! -x /usr/sbin/ubusd ]]; then
        printf '%s\n' "ERROR: /usr/sbin/ubusd is absent" >&2
        return 1
    fi

    # The scenario lint, once per run, before any flow drives a scenario. A
    # nop behind a checking message erases the failure of that check. A flow
    # can then read a pass the scenario never earned (test/sipp/README.md,
    # "When a failed check is judged"). The out-of-call scenarios beside this
    # runner are scenarios too, so the lint reads both directories.
    #
    # The run passes through here twice, once on each side of the namespace
    # re-exec of main(), so the inner frame skips it: UVOICE_E2E_USERNS is
    # set by the outer frame alone. The lint ships beside this runner, so an
    # absent file is a broken copy and not a bench that lacks a tool. It is
    # run through sh, so it needs no execute bit.
    if [[ ${UVOICE_E2E_USERNS:-0} == 1 ]]; then
        return 0
    fi
    lint_script=$uvoice_dir/test/tools/sipp_scenario_lint.sh
    if [[ ! -f $lint_script ]]; then
        printf '%s\n' "ERROR: the SIPp scenario lint is absent: $lint_script" >&2
        return 1
    fi
    if ! lint_output=$(sh "$lint_script" "$sipp_dir" "$script_dir" 2>&1); then
        printf '%s\n' "$lint_output" >&2
        printf '%s\n' "ERROR: the SIPp scenario lint refused the set" >&2
        return 1
    fi
}

process_wait()
{
    local pid=$1
    local timeout_s=$2
    local deadline=$((SECONDS + timeout_s))

    while ! process_exited "$pid"; do
        if (( SECONDS >= deadline )); then
            return 124
        fi
        sleep 0.1
    done

    wait "$pid"
    process_status=$?
    return 0
}

process_stop()
{
    local pid=${1:-}
    local signal=${2:-TERM}
    local deadline

    if [[ -z $pid ]] || ! kill -0 "$pid" 2>/dev/null; then
        return
    fi

    kill -"$signal" "$pid" 2>/dev/null || true
    deadline=$((SECONDS + 5))
    while kill -0 "$pid" 2>/dev/null && (( SECONDS < deadline )); do
        if [[ $(ps -o stat= -p "$pid" 2>/dev/null || true) = Z* ]]; then
            break
        fi
        sleep 0.1
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -KILL "$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true
}

flow_stop()
{
    process_stop "$current_hook_pid"
    process_stop "$current_sipp_pid"
    process_stop "$current_capture_pid" INT
    process_stop "$current_http_pid"
    process_stop "$current_stub_pid"
    process_stop "$current_daemon_pid"
    process_stop "$current_ca_pid"
    process_stop "$current_ubusd_pid"
    if [[ -n $current_socket ]]; then
        rm -f "$current_socket"
    fi
    tls_bundle_remove
    tls_lock_drop
    current_ca_pid=
    current_hook_pid=
    current_http_pid=
    current_stub_pid=
    current_sipp_pid=
    current_capture_pid=
    current_daemon_pid=
    current_ubusd_pid=
    current_socket=
    capture_ready=0
    sipp_completed=0
    flow_transport=
}

exit_cleanup()
{
    flow_stop
}

result_print()
{
    local status=$1
    local name=$2
    local detail=$3

    printf '%s: %s, %s\n' "$status" "$name" "$detail"
    case $status in
        FAIL)
            failure_count=$((failure_count + 1))
            ;;
        PARTIAL)
            partial_count=$((partial_count + 1))
            ;;
        SKIP)
            skip_count=$((skip_count + 1))
            ;;
    esac
}

# The loopback address of the run, bare. It is the SIPp -i argument, the value
# of the four uci address options, and the host the python clients connect to.
# Together with flow_loop_uri below it is the only place the two literals
# appear on the family path, so a site that follows the family cannot drift
# from the one the readers expect.
flow_loop_addr()
{
    if (( flow_family == 6 )); then
        printf '%s\n' "::1"
        return
    fi

    printf '%s\n' "127.0.0.1"
}

# The same address inside a URI, where RFC 3261 19.1.1 asks for brackets around
# an IPv6 reference. It is the SIPp destination argument, which SIPp parses as
# a host and an optional port and would otherwise read the last colon of the
# address as the port separator.
flow_loop_uri()
{
    if (( flow_family == 6 )); then
        printf '%s\n' "[::1]"
        return
    fi

    printf '%s\n' "127.0.0.1"
}

# The default capture filter of one port, qualified by the family of the run.
# tcpdump takes the family as a leading primitive, and an unqualified port
# filter takes both families: a v6 run that fell back to v4 would fill its
# capture and the family reader could not tell that from a v6 window. Under 4
# the filter is the string it always was.
flow_udp_filter()
{
    local port=$1

    if (( flow_family == 6 )); then
        printf 'ip6 and udp port %u\n' "$port"
        return
    fi

    printf 'udp port %u\n' "$port"
}

config_write()
{
    local config_dir=$1
    local sipp_port=$2
    local daemon_port=$3
    local session_expires=${4:-}
    local flash_service_timer=${5:-}
    # the local call forward knobs: the condition mask and its target, which
    # the daemon seeds its runtime state from at every load
    local call_fwd_mode=${6:-}
    local call_fwd_uri=${7:-}
    # the SDES SRTP switch, which is a base option: the fax and T.38 flows can
    # append their channel options behind this function, an SRTP flow cannot
    local srtp_enabled=${8:-}
    # udp, tcp or tls. The transport is a base option only, by design, so it
    # rides this function the way srtp_enabled does. An empty value writes no
    # option at all, so the daemon takes the schema default, udp.
    local transport=${9:-}
    # Further base options, one "\toption name 'value'" line each. A base
    # option cannot be appended behind this function the way a channel option
    # can, and mwi_subscribe and keepalive_interval are base ones, so the
    # stream flows pass their block here instead of growing this argument list
    # once per option.
    local base_extra=${10:-}
    # How many channels the base section declares. It is a base option and it
    # decides how many lines come up at all, so it cannot ride behind this
    # function either. The default is the one channel of a single-line flow;
    # the transfer flow that needs a second subscriber passes 2 and appends
    # its own channel section the way the fax flows append theirs.
    local channels=${11:-1}
    # Whole "config codec" sections, already formatted. A codec section is a
    # section of its own and not an option of the channel, so it cannot ride
    # the channel block a fax flow appends behind this function. An empty
    # value writes nothing.
    local codec_sections=${12:-}
    local loop_addr

    loop_addr=$(flow_loop_addr)

    mkdir -p "$config_dir" || return 1
    {
        printf '%s\n' "config base 'base'"
        printf '\toption enabled %s\n' "'1'"
        printf "\toption channels '%u'\n" "$channels"
        # The operator gate of the family. It is a base option and default 0,
        # so a run under 4 writes no line at all and the daemon behaves as it
        # always did (ipv6_enabled).
        if (( flow_family == 6 )); then
            printf '\toption ipv6_enabled %s\n' "'1'"
        fi
        if [[ -n $srtp_enabled ]]; then
            printf "\toption srtp_enabled '%u'\n" "$srtp_enabled"
        fi
        if [[ -n $transport ]]; then
            printf "\toption transport '%s'\n" "$transport"
        fi
        if [[ -n $base_extra ]]; then
            printf '%s\n' "$base_extra"
        fi
        printf '%s\n' "config channel 'channel1'"
        printf '\toption id %s\n' "'0'"
        printf '\toption enabled %s\n' "'1'"
        printf '\toption sip_username %s\n' "'5551000'"
        printf "\toption sip_proxy '%s'\n" "$loop_addr"
        printf "\toption sip_port '%u'\n" "$sipp_port"
        printf "\toption sip_outbound_proxy '%s'\n" "$loop_addr"
        printf "\toption sip_outbound_proxy_port '%u'\n" "$sipp_port"
        printf "\toption sip_domain '%s'\n" "$loop_addr"
        printf "\toption sip_registrar '%s'\n" "$loop_addr"
        printf "\toption sip_registrar_port '%u'\n" "$sipp_port"
        printf "\toption sip_local_port '%u'\n" "$daemon_port"
        printf '\toption sip_registration_expiry %s\n' "'3600'"
        printf '\toption rtp_local_port_min %s\n' "'24000'"
        printf '\toption rtp_local_port_max %s\n' "'24008'"
        if [[ -n $session_expires ]]; then
            printf "\toption sip_session_max_expires '%u'\n" "$session_expires"
        fi
        if [[ -n $flash_service_timer ]]; then
            printf "\toption flash_service_timer_sec '%u'\n" "$flash_service_timer"
        fi
        if [[ -n $call_fwd_mode ]]; then
            printf "\toption call_fwd_local %s\n" "'1'"
            printf "\toption call_fwd_local_mode '%u'\n" "$call_fwd_mode"
        fi
        if [[ -n $call_fwd_uri ]]; then
            printf "\toption call_fwd_uncond_uri '%s'\n" "$call_fwd_uri"
        fi
        if [[ -n $codec_sections ]]; then
            printf '%s\n' "$codec_sections"
        fi
    } >"$config_dir/voip"
}

# One bind of ::1, which answers what a link state cannot: the loopback can be
# up while the kernel refuses the address. This host disables IPv6 outside a
# network namespace, so the bind is the question a v6 run has to ask before it
# writes a single config file. python3 is a prerequisite of every run already.
# The reason is printed and the status says whether the bind held.
loop6_bind_probe()
{
    python3 -c '
import socket
import sys

try:
    sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    sock.bind(("::1", 0))
    sock.close()
except OSError as err:
    print(err)
    sys.exit(1)
' 2>&1
}

# The loopback the run needs. Under 4 the link state is the whole question.
# Under 6 the address is a second one, and a run that cannot bind ::1 stops
# here with that reason: it never answers with 127.0.0.1, because a run that
# fell back would report a v4 result under a v6 name.
loopback_prepare()
{
    local reason

    ip link set lo up 2>/dev/null || true
    ip -o link show lo 2>/dev/null | grep -qE '[<,]UP[,>]' || return 1

    if (( flow_family != 6 )); then
        return 0
    fi

    reason=$(loop6_bind_probe) && return 0
    loopback_failure="ERROR: E2E_FAMILY 6 needs ::1 on the loopback: $reason"
    return 1
}

capture_start()
{
    local flow_dir=$1
    local port=$2
    # The stream flows override it: their signalling rides tcp on two ports,
    # and the tls cleartext pin has to see the udp side of both ports as well
    # to say that nothing left the box outside the connection.
    local filter=${3:-$(flow_udp_filter "$port")}

    if ! command -v tcpdump >/dev/null 2>&1; then
        return 1
    fi

    # -Z root keeps tcpdump from chowning the capture file to an identity that
    # the user namespace does not map. --immediate-mode drops the one second
    # buffer timeout, without which pcap_breakloop discards the packets that
    # arrive in the last second before the capture stops.
    tcpdump -i lo --immediate-mode -U -s 0 -Z root \
        -w "$flow_dir/traffic.pcap" "$filter" \
        >"$flow_dir/tcpdump.log" 2>&1 &
    current_capture_pid=$!
    sleep 0.2
    if kill -0 "$current_capture_pid" 2>/dev/null; then
        capture_ready=1
        return 0
    fi

    wait "$current_capture_pid" 2>/dev/null || true
    current_capture_pid=
    return 1
}

# The reading of udp_bound_check in lib.sh, on the other table. A SIPp that
# runs over tcp or tls never appears in /proc/net/udp, so the udp probe would
# wait out its whole timeout and report a startup failure for a SIPp that is
# listening. The local address column is the same column in both tables, and
# it names the listening port for a server and the bound source port for a
# client, so one expression answers both.
tcp_bound_check()
{
    local port=$1
    local hex

    # The socket state is part of the reading. Column 4 is st, and 06 is
    # TIME_WAIT: a socket that has just closed sits on the port for two
    # minutes and would otherwise answer "bound" for a SIPp that already
    # left, which turns the next flow's startup failure into a silent one.
    # /proc/net/udp keeps its reading unchanged, because a datagram socket
    # has no TIME_WAIT.
    hex=$(printf '%04X' "$port")
    awk -v want=":$hex\$" \
        'FNR > 1 && $2 ~ want && $4 != "06" { found = 1 } END { exit !found }' \
        /proc/net/tcp 2>/dev/null
}

# The v6 tcp table, with the address match that udp6_bound_check in lib.sh
# states the reason for.
tcp6_bound_check()
{
    local port=$1
    local hex

    # 06 is TIME_WAIT, for the reason tcp_bound_check states.
    hex=$(printf '%04X' "$port")
    awk -v want="^$loop6_proc_addr:$hex\$" \
        'FNR > 1 && $2 ~ want && $4 != "06" { found = 1 } END { exit !found }' \
        /proc/net/tcp6 2>/dev/null
}

# The probe of the transport the running flow uses. tls is a tcp listener with
# a record layer on top, so the two share one table. The family chooses the
# pair of tables the transport then chooses inside.
port_bound_check()
{
    local port=$1

    case $flow_family$flow_transport in
        6tcp|6tls)
            tcp6_bound_check "$port"
            ;;
        6*)
            udp6_bound_check "$port"
            ;;
        *tcp|*tls)
            tcp_bound_check "$port"
            ;;
        *)
            udp_bound_check "$port"
            ;;
    esac
}

# A scenario can run to completion before the first probe, so an exited SIPp
# with status 0 is a finished run and not a startup failure.
sipp_ready_wait()
{
    local port=$1
    local timeout_s=${2:-10}
    local deadline=$((SECONDS + timeout_s))

    sipp_completed=0
    while (( SECONDS < deadline )); do
        if port_bound_check "$port"; then
            return 0
        fi
        if process_exited "$current_sipp_pid"; then
            wait "$current_sipp_pid"
            process_status=$?
            current_sipp_pid=
            sipp_completed=1
            (( process_status == 0 ))
            return $?
        fi
        sleep 0.05
    done
    return 1
}

sipp_finish_wait()
{
    local timeout_s=$1

    if (( sipp_completed )); then
        return 0
    fi
    process_wait "$current_sipp_pid" "$timeout_s" || return 1
    current_sipp_pid=
    sipp_completed=1
}

# The binary the running flow drives. The udp flows keep the non-TLS build,
# whose rtp_echo works; a tls flow needs the TLS build and drives no rtp_echo.
sipp_binary_get()
{
    if [[ $flow_transport = tls ]]; then
        printf '%s\n' "$sipp_tls_bin"
        return
    fi

    printf '%s\n' "$sipp_bin"
}

# The -t mode and, over tls, the identity SIPp presents. The committed fixture
# pair is what it presents: the daemon does not verify a peer certificate in
# either direction (sip/tls.h, "No hostname verification"), so the pair only
# has to parse. An empty transport adds no argument at all, so SIPp runs in its
# default udp mode.
sipp_transport_args()
{
    case $flow_transport in
        tcp)
            printf '%s\n' -t t1
            ;;
        tls)
            printf '%s\n' -t l1 \
                -tls_cert "$uvoice_dir/test/unit/tls/server.crt" \
                -tls_key "$uvoice_dir/test/unit/tls/server.key"
            ;;
    esac
}

sipp_start()
{
    local flow_dir=$1
    local scenario_file=$2
    local sipp_port=$3
    local daemon_port=$4
    local media_port=$5
    local max_calls=${6:-1}
    local transport_args=()
    local extra_args=("${@:7}")

    mapfile -t transport_args < <(sipp_transport_args)

    "$(sipp_binary_get)" "$(flow_loop_uri):$daemon_port" ${transport_args[@]+"${transport_args[@]}"} \
        -sf "$scenario_file" -m "$max_calls" -l "$max_calls" \
        ${extra_args[@]+"${extra_args[@]}"} \
        -p "$sipp_port" -mp "$media_port" -i "$(flow_loop_addr)" \
        -recv_timeout "${SIPP_RECV_TIMEOUT_MS:-60000}" \
        -timeout "${SIPP_TIMEOUT_SECONDS:-90}" -timeout_error -nostdin \
        -trace_err -error_file "$flow_dir/sipp-errors.log" \
        -trace_msg -message_file "$flow_dir/sipp-messages.log" \
        -trace_screen -screen_file "$flow_dir/sipp-screen.log" \
        >"$flow_dir/sipp-stdout.log" 2>&1 &
    current_sipp_pid=$!
    sipp_ready_wait "$sipp_port"
}

sipp_uac_start()
{
    local flow_dir=$1
    local scenario_file=$2
    local sipp_port=$3
    local daemon_port=$4
    local media_port=$5
    # The out-of-call scenario. The default answers the REGISTER; a transfer
    # flow replaces it with the peer that has to answer a request the DUT sends
    # unasked (the child INVITE of a referral, or a call the DUT places while
    # the main scenario is still waiting). SIPp takes exactly one.
    local ooc_file=${6:-$script_dir/register_ooc.xml}
    # How many calls this SIPp places. In UAC mode -m drives the generator, so
    # a value above one puts that many INVITEs on the wire; the calls the
    # out-of-call scenario opens are not of its making and are not counted here
    # (measured: a -m of three placed three INVITEs and the DUT 486ed two).
    local max_calls=${7:-1}
    # SIPp's -l. It bounds the outgoing generator alone: open_calls_allowed is
    # read by CallGenerationTask, and an instance the out-of-call scenario opens
    # never passes through it. What bounds these runs is -m. The value is
    # headroom, not a requirement, and the default is the -m value.
    local call_limit=${8:-$max_calls}
    # [service], the user part of the request URI the scenario writes. An empty
    # value adds no argument, and SIPp writes its own default.
    local service=${9:-}
    local transport_args=()
    local service_args=()
    local extra_args=("${@:10}")

    mapfile -t transport_args < <(sipp_transport_args)
    if [[ -n $service ]]; then
        service_args=(-s "$service")
    fi

    # register_ooc.xml grants the expiry of the expires keyword, and SIPp stops
    # with "Unsupported keyword" when a scenario names a keyword that no -key
    # defines. The default therefore goes on every run. SIPp keeps the last
    # value of a repeated -key (sipp.cpp assigns the generic map), so a -key
    # expires among the extra arguments behind it wins.
    "$(sipp_binary_get)" "$(flow_loop_uri):$daemon_port" ${transport_args[@]+"${transport_args[@]}"} \
        -sf "$scenario_file" -oocsf "$ooc_file" -m "$max_calls" -l "$call_limit" \
        ${service_args[@]+"${service_args[@]}"} \
        -key expires 3600 \
        ${extra_args[@]+"${extra_args[@]}"} \
        -p "$sipp_port" -mp "$media_port" -i "$(flow_loop_addr)" \
        -recv_timeout "${SIPP_RECV_TIMEOUT_MS:-60000}" \
        -timeout "${SIPP_TIMEOUT_SECONDS:-90}" -timeout_error -nostdin \
        -trace_err -error_file "$flow_dir/sipp-errors.log" \
        -trace_msg -message_file "$flow_dir/sipp-messages.log" \
        -trace_screen -screen_file "$flow_dir/sipp-screen.log" \
        >"$flow_dir/sipp-stdout.log" 2>&1 &
    current_sipp_pid=$!
    sipp_ready_wait "$sipp_port"
}

# The private ubus socket of one daemon. The MGCP call agent needs the path
# before daemon_spawn sets current_socket.
ubus_socket_path()
{
    printf '/tmp/u14g.%u.%s.sock\n' "$$" "$1"
}

# ubusd and the daemon, without the readiness wait: a flow whose daemon leaves
# inside its own bring-up never reaches the ready state daemon_start polls for
# (daemon_dsp_fatal). The fourth argument is the stub's bring-up lever and the
# fifth the flash service its region read-back carries (0 off, 1 split, 2
# drop); both default to the shipped behaviour, and each daemon gets its own
# value so no flow inherits the lever of the one before it.
daemon_spawn()
{
    local flow_dir=$1
    local config_dir=$2
    local socket_tag=$3
    local dsp_start=${4:-ok}
    local flash_service=${5:-2}
    local trace=${6:-sip,fsm,hook,hal,dialplan}

    current_socket=$(ubus_socket_path "$socket_tag")
    rm -f "$current_socket"
    /usr/sbin/ubusd -s "$current_socket" >"$flow_dir/ubusd.log" 2>&1 &
    current_ubusd_pid=$!
    sleep 0.2
    if ! kill -0 "$current_ubusd_pid" 2>/dev/null; then
        return 1
    fi

    LD_LIBRARY_PATH="$host_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        UVOICE_UCI_CONFDIR="$config_dir" \
        UVOICE_STUB_DSP_START="$dsp_start" \
        UVOICE_STUB_FLASH_SERVICE="$flash_service" \
        "$UVOICED" -s "$current_socket" -d "$trace" \
        >"$flow_dir/daemon.log" 2>&1 &
    current_daemon_pid=$!
}

daemon_start()
{
    local flow_dir=$1
    local config_dir=$2
    local socket_tag=$3
    local dsp_start=${4:-ok}
    local flash_service=${5:-2}
    local deadline

    daemon_spawn "$flow_dir" "$config_dir" "$socket_tag" "$dsp_start" \
        "$flash_service" || return 1
    deadline=$((SECONDS + 10))
    while (( SECONDS < deadline )); do
        if ubus -s "$current_socket" call voice status >"$flow_dir/readiness.json" 2>/dev/null &&
            jq -e '.hal.state == "ready" and (.call | length) == 2' \
                "$flow_dir/readiness.json" >/dev/null; then
            return 0
        fi
        if ! kill -0 "$current_daemon_pid" 2>/dev/null; then
            return 1
        fi
        sleep 0.1
    done
    return 1
}

status_wait()
{
    local flow_dir=$1
    local wanted=$2
    local timeout_s=$3
    local label=$4
    local deadline=$((SECONDS + timeout_s))
    local output

    while (( SECONDS < deadline )); do
        if output=$(ubus -s "$current_socket" call voice status 2>/dev/null) &&
            jq -e --arg wanted "$wanted" '.call[0].state == $wanted' \
                <<<"$output" >/dev/null; then
            printf '%s\n' "$output" >"$flow_dir/status-$label.json"
            return 0
        fi
        sleep 0.1
    done
    if output=$(ubus -s "$current_socket" call voice status 2>/dev/null); then
        printf '%s\n' "$output" >"$flow_dir/status-$label-timeout.json"
    fi
    return 1
}

# A wait that times out keeps the last answer in <prefix>-<label>-timeout.json,
# so the artifacts of a failed flow show what the wait saw.
ubus_poll_wait()
{
    local flow_dir=$1
    local object=$2
    local method=$3
    local prefix=$4
    local expression=$5
    local timeout_s=$6
    local label=$7
    local deadline=$((SECONDS + timeout_s))
    local call=(ubus -s "$current_socket" call "$object" "$method")
    local output

    while (( SECONDS < deadline )); do
        if output=$("${call[@]}" 2>/dev/null) &&
            jq -e "$expression" <<<"$output" >/dev/null; then
            printf '%s\n' "$output" >"$flow_dir/$prefix-$label.json"
            return 0
        fi
        sleep 0.1
    done
    if output=$("${call[@]}" 2>/dev/null); then
        printf '%s\n' "$output" >"$flow_dir/$prefix-$label-timeout.json"
    fi
    return 1
}

# status_wait answers the one question the call state asks. A flow that waits
# on any other field of "voice status" brings its own jq expression here, the
# way stub_wait takes one for the stub dump.
status_expr_wait()
{
    ubus_poll_wait "$1" voice status status "$2" "$3" "$4"
}

# The status and not the state: a line in REFRESHING still holds its binding,
# and "voice status" reports it as "registered" too.
registration_wait()
{
    local flow_dir=$1
    local line_count=${2:-1}

    status_expr_wait "$flow_dir" \
        "[.lines[range(0; $line_count)].status] | all(. == \"registered\")" \
        15 registered
}

# The daemon in place of daemon_start for a flow whose SIPp calls the DUT. A
# calling SIPp puts its INVITE on the wire before the line registers, so a
# separate registrar grants the binding first, and the caller starts after
# it exits. The fifth argument is the expiry the registrar grants; the rest
# pass through to daemon_start.
register_prime()
{
    local flow_dir=$1
    local config_dir=$2
    local socket_tag=$3
    local line_count=${4:-1}
    local expires=${5:-3600}
    local daemon_args=("${@:6}")

    mkdir -p "$flow_dir/register" || return 1
    sipp_start "$flow_dir/register" "$sipp_dir/uas_register_prime.xml" \
        "$sipp_port" "$daemon_port" "$media_port" "$line_count" \
        -key expires "$expires" || return 1
    daemon_start "$flow_dir" "$config_dir" "$socket_tag" \
        ${daemon_args[@]+"${daemon_args[@]}"} || return 1
    sipp_finish_wait 20 || return 1
    (( process_status == 0 )) || return 1
    registration_wait "$flow_dir" "$line_count"
}

status_expr_check()
{
    local flow_dir=$1
    local expression=$2
    local label=$3
    local output

    output=$(ubus -s "$current_socket" call voice status 2>/dev/null) || return 1
    printf '%s\n' "$output" >"$flow_dir/status-$label.json"
    jq -e "$expression" <<<"$output" >/dev/null
}

# The same poll against the record ring, which needs its own because a record
# closes later than the state it belongs to: the leg holds its slot until the
# BYE transaction sits out timer K, and the media snapshot behind it is a HAL
# job that lands after that (run_churn.sh oracle_check).
cdr_expr_wait()
{
    ubus_poll_wait "$1" voice cdr cdr "$2" "$3" "$4"
}

status_save()
{
    local flow_dir=$1
    local label=$2

    snapshot_index=$((snapshot_index + 1))
    ubus -s "$current_socket" call voice status \
        >"$flow_dir/status-${snapshot_index}-$label.json" 2>/dev/null
}

stub_wait()
{
    ubus_poll_wait "$1" voice.stub dump stub "$2" "$3" "$4"
}

# The daemon's remote port is always the port SIPp offered or answered with, so
# only the SDP inside messages SIPp sent is relevant. Reading the last m=audio
# line of the whole log picks up the daemon's own port on inbound flows.
#
# SIPp names the transport in the marker: the line reads "UDP message sent",
# "TCP message sent" or "TLS message sent" (SIPp 3.7.3 socket.cpp, through
# TRANSPORT_TO_STRING). Every reader below therefore matches the three. The
# anchoring of each pattern is left exactly as it was: an unanchored one also
# takes the "Unexpected <transport> message received" line, and that is the
# direction of that message.
sipp_media_port_get()
{
    local flow_dir=$1

    awk '
        /(UDP|TCP|TLS) message sent/ { sending = 1; next }
        /(UDP|TCP|TLS) message received/ { sending = 0; next }
        sending && /^[[:space:]]*m=audio [0-9]+ / { port = $2 }
        END { if (port != "") print port }
    ' "$flow_dir/sipp-messages.log" 2>/dev/null
}

media_port_wait()
{
    local flow_dir=$1
    local timeout_s=$2
    local deadline=$((SECONDS + timeout_s))
    local negotiated_port=

    while (( SECONDS < deadline )); do
        negotiated_port=$(sipp_media_port_get "$flow_dir")
        if [[ $negotiated_port =~ ^[0-9]+$ ]] &&
            stub_wait "$flow_dir" ".line[0].stream[0].running == true and .line[0].stream[0].remote_port == $negotiated_port" 1 media; then
            printf '%s\n' "$negotiated_port" >"$flow_dir/sipp-media-port.txt"
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# The inline key of one a=crypto line of the message log, by direction and tag.
# The direction is what tells the two keys apart: the DUT's own key and the
# scenario's fixed key both ride an "a=crypto:1" line, and a SDES key is per
# direction, so one side never echoes the other's (RFC 4568 5.1.2). The reader
# splits the log the way sipp_media_port_get above splits it.
sipp_crypto_key_get()
{
    local flow_dir=$1
    local direction=$2
    local tag=$3

    awk -v want="$direction" -v tag="$tag" '
        /(UDP|TCP|TLS) message sent/ { dir = "sent"; next }
        /(UDP|TCP|TLS) message received/ { dir = "received"; next }
        dir == want && $1 == "a=crypto:" tag { key = $3 }
        END {
            if (key == "")
                exit 1
            gsub(/\r/, "", key)
            sub(/^inline:/, "", key)
            sub(/\|.*$/, "", key)
            print key
        }
    ' "$flow_dir/sipp-messages.log" 2>/dev/null
}

# The digest the stub reports for a key, computed here from the base64 the wire
# carried. The stub takes the standard reflected CRC-32 of the raw 30 bytes and
# widens it to 64 bits so the JSON number stays positive (stub.c crc32_get and
# srtp_dump); zlib.crc32() is the same number.
key_crc32_get()
{
    local key_b64=$1

    python3 -c '
import base64, sys, zlib

raw = base64.b64decode(sys.argv[1], validate=True)
if len(raw) != 30:
    sys.exit(1)
print(zlib.crc32(raw))
' "$key_b64" 2>/dev/null
}

# The echo guard. A SDES key is per direction and one side never echoes the
# other's (RFC 4568 5.1.2). A DUT that answered with the peer's own key would
# satisfy both digest comparisons below, because the two numbers they compare
# would come from the same 30 bytes. The keys are drawn from 240 bits of
# entropy, so a genuine collision cannot happen and an equal key means an echo.
#
# The inbound flow is the one that needs it: there the answered key is the DUT's
# and the offered keys are the scenario's. The outbound direction has no such
# check. There the DUT draws the offered keys, and the answer key is a scenario
# literal. SIPp writes the answer, so the check would test SIPp, not the daemon.
srtp_keys_differ_check()
{
    local flow_dir=$1
    local own_dir=$2
    local peer_dir=$3
    local own_key
    local peer_key
    local tag

    own_key=$(sipp_crypto_key_get "$flow_dir" "$own_dir" 1) || return 1
    for tag in 1 2; do
        peer_key=$(sipp_crypto_key_get "$flow_dir" "$peer_dir" "$tag") || return 1
        if [[ $own_key = "$peer_key" ]]; then
            return 1
        fi
    done
}

# How many crypto attributes one message carried. A scenario regex cannot
# count: every "this attribute is present" pin is satisfied by a message that
# also carries a duplicate tag or an attribute under a tag nobody offered, and
# an inverse pin only denies the one tag it names. The message log is the
# countable copy of the wire, so the count is asserted here.
#
# The message is chosen by direction, by the first line of the message and by
# carrying a media description: a 200 for the BYE matches the same start line as
# the 200 that answered the offer, and only the answer carries SDP.
sipp_crypto_lines_count()
{
    local flow_dir=$1
    local direction=$2
    local start_line=$3

    awk -v want="$direction" -v start="$start_line" '
        function msg_end() {
            if (matched && sdp)
                last = count
            matched = 0
            sdp = 0
            count = 0
        }
        /^-----/ { msg_end(); head = 1; next }
        head && /^(UDP|TCP|TLS) message sent/ { dir = "sent"; head = 0; first = 1; next }
        head && /^(UDP|TCP|TLS) message received/ { dir = "received"; head = 0; first = 1; next }
        first {
            if ($0 ~ /^[[:space:]]*$/)
                next
            line = $0
            gsub(/\r/, "", line)
            matched = (dir == want && index(line, start) == 1)
            first = 0
            next
        }
        /^m=audio / { sdp = 1 }
        /^a=crypto:/ { count++ }
        END {
            msg_end()
            if (last == "")
                exit 1
            print last + 0
        }
    ' "$flow_dir/sipp-messages.log" 2>/dev/null
}

# A crypto attribute the peer wrote carries three fields and nothing else: the
# tag, the suite and the key parameter. A fourth field is a session parameter,
# and each one of them changes how the stream is keyed, encrypted or
# authenticated (RFC 4568 9.1). This check exists because the scenario cannot
# make it: SIPp accepts a HTAB inside a [[:cntrl:]] class, so a tab separated
# parameter would read as a line end there.
srtp_crypto_bare_check()
{
    local flow_dir=$1
    local direction=$2

    awk -v want="$direction" '
        /^(UDP|TCP|TLS) message sent/ { dir = "sent"; next }
        /^(UDP|TCP|TLS) message received/ { dir = "received"; next }
        dir == want && /^a=crypto:/ {
            seen++
            line = $0
            gsub(/\r/, "", line)
            if (split(line, field, /[[:space:]]+/) != 3)
                bad++
        }
        END { exit (seen == 0 || bad > 0) }
    ' "$flow_dir/sipp-messages.log" 2>/dev/null
}

# The two shapes above, asked of one message: the count the flow expects, and
# no session parameter on any attribute the peer wrote.
srtp_crypto_shape_check()
{
    local flow_dir=$1
    local direction=$2
    local start_line=$3
    local expected=$4
    local count

    count=$(sipp_crypto_lines_count "$flow_dir" "$direction" "$start_line") || return 1
    printf 'crypto attributes in the %s "%s": %s, expected %s\n' "$direction" \
        "$start_line" "$count" "$expected" >>"$flow_dir/srtp-digests.txt"
    [[ $count = "$expected" ]] || return 1
    srtp_crypto_bare_check "$flow_dir" "$direction"
}

# The digest pin of the SRTP flows. The two numbers come from different places:
# one from the stream the daemon programmed, one from the SDP its peer read off
# the wire. A stream keyed with anything other than the key it negotiated fails
# here. The keys themselves never leave the message log, so only the digest is
# recorded. The tag is the literal 1: both keyed flows negotiate that tag, and
# the scenarios assert on the wire that no other tag was answered.
srtp_digest_check()
{
    local flow_dir=$1
    local direction=$2
    local field=$3
    local key
    local digest

    key=$(sipp_crypto_key_get "$flow_dir" "$direction" 1) || return 1
    digest=$(key_crc32_get "$key") || return 1
    printf '%s: the %s key of the wire digests to %s\n' "$field" "$direction" \
        "$digest" >>"$flow_dir/srtp-digests.txt"
    stub_check "$flow_dir" ".line[0].stream[0].$field == $digest" "$field"
}

# --------------------------------------------------- the stream transport pins

# Every header line of one message of the log, chosen by direction and by the
# first line of the message. The Via and the Contact of a request the DUT sent
# are what the transport token rides, and a SIPp regexp cannot assert them: a
# regexp attribute takes no SIPp keyword, so a scenario cannot name the
# transport the run was given, and one scenario file serves both twins.
#
# The message boundary is the marker line itself, not the row of dashes in
# front of it. SIPp writes an unexpected message with no dashes (call.cpp,
# "Unexpected %s message received"), and a reader that only breaks on dashes
# would append that message's headers to the message before it.
sipp_msg_header_get()
{
    local flow_dir=$1
    local direction=$2
    local start_line=$3
    local header=$4
    # Which message of that direction and start line to read. The default is
    # the first; the auth flow of the prack set reads the second INVITE.
    local occurrence=${5:-1}

    awk -v want="$direction" -v start="$start_line" -v hdr="$header:" \
        -v nth="$occurrence" '
        function msg_end(   i) {
            if (matched && !done && ++seen == nth) {
                for (i = 1; i <= n; i++)
                    print out[i]
                done = 1
            }
            matched = 0
            n = 0
            inhdr = 0
            inbody = 0
        }
        /(UDP|TCP|TLS) message sent/ { msg_end(); dir = "sent"; first = 1; next }
        /(UDP|TCP|TLS) message received/ { msg_end(); dir = "received"; first = 1; next }
        first {
            if ($0 ~ /^[[:space:]]*$/)
                next
            line = $0
            gsub(/\r/, "", line)
            matched = (dir == want && index(line, start) == 1)
            first = 0
            inhdr = 0
            inbody = 0
            next
        }
        matched && !inbody {
            line = $0
            gsub(/\r/, "", line)
            # The header section ends at the first empty line (RFC 3261 7), and
            # nothing behind it is a header however it starts.
            if (line ~ /^[[:space:]]*$/) {
                inbody = 1
                inhdr = 0
                next
            }
            # A continuation line is part of the value before it (RFC 3261
            # 7.3.1): a line opening with SP or HTAB folds onto its header, and
            # the whole run of linear whitespace stands for one space.
            if (inhdr && line ~ /^[ \t]/) {
                sub(/^[ \t]+/, " ", line)
                out[n] = out[n] line
                next
            }
            # The name is matched without case (RFC 3261 7.3.1). The DUT writes
            # the canonical spelling and folds nothing, so neither rule changes
            # what the presence pins read; both are here for the absence pins,
            # which would otherwise answer "no such header" for a header that
            # was there in another legal shape.
            inhdr = (tolower(substr(line, 1, length(hdr))) == tolower(hdr))
            if (inhdr)
                out[++n] = line
            next
        }
        END {
            msg_end()
            if (!done)
                exit 1
        }
    ' "$flow_dir/sipp-messages.log" 2>/dev/null
}

# The Via transport token of one message (RFC 3261 20.42, sent-protocol). Every
# Via of the message must name the token, so a message that carries a second
# Via over another transport fails here rather than passing on its topmost one.
# A message with no Via at all fails too: a missing pin is not a passing pin.
stream_via_check()
{
    local flow_dir=$1
    local direction=$2
    local start_line=$3
    local token=$4
    local label=$5
    local lines
    local seen=0
    local line
    local found

    lines=$(sipp_msg_header_get "$flow_dir" "$direction" "$start_line" Via) || return 1

    while IFS= read -r line; do
        [[ -n $line ]] || continue
        found=$(sed -n 's|^Via:[[:space:]]*SIP/2\.0/\([A-Za-z]\{1,\}\)[[:space:]].*|\1|p' \
            <<<"$line")
        printf '%s "%s" Via: %s\n' "$direction" "$start_line" "$line" \
            >>"$flow_dir/stream-tokens.txt"
        # One value per line is part of the assertion. A comma folds several
        # Via values onto one header line (RFC 3261 7.3.1), and the reader
        # above takes the first of them, so a folded line would pass on its
        # first value while a second value named another transport. The DUT
        # writes one value per line (src/proto/shared/hdr.c); this pins that
        # shape.
        [[ $line != *,* ]] || return 1
        [[ $found = "$token" ]] || return 1
        seen=$((seen + 1))
    done <<<"$lines"

    (( seen > 0 )) || return 1
    printf '%s: %u Via header(s) named %s\n' "$label" "$seen" "$token" \
        >>"$flow_dir/stream-tokens.txt"
}

# The Contact transport parameter of one message. It belongs inside the angle
# brackets, because it is a URI parameter and not a header parameter
# (RFC 3261 19.1.1, and the reg.c writer says so), so the match requires the
# closing bracket or a further parameter behind it. A bare ";transport=tcp"
# after the '>' would be a header parameter and would not reach the registrar's
# binding, and it does not pass here.
stream_contact_check()
{
    local flow_dir=$1
    local direction=$2
    local start_line=$3
    local token=$4
    local label=$5
    local lines
    local seen=0
    local line

    lines=$(sipp_msg_header_get "$flow_dir" "$direction" "$start_line" Contact) || return 1

    while IFS= read -r line; do
        [[ -n $line ]] || continue
        printf '%s "%s" Contact: %s\n' "$direction" "$start_line" "$line" \
            >>"$flow_dir/stream-tokens.txt"
        grep -q "<sip:[^>]*;transport=${token}[;>]" <<<"$line" || return 1
        seen=$((seen + 1))
    done <<<"$lines"

    (( seen > 0 )) || return 1
    printf '%s: %u Contact header(s) carried ;transport=%s\n' "$label" "$seen" "$token" \
        >>"$flow_dir/stream-tokens.txt"
}

# No connection that the peer opened restarted the registration. The status
# checks of the flow cannot see
# such a restart: the caller answers the REGISTER that it gets, and the line is
# registered again before the INVITE is read. The daemon log names an accepted
# connection before its up report, and a connection of the line reports up
# alone, so a re-register behind the up of an accepted connection is the
# defect. The keepalive of "The gap between the two runs" opens a connection of
# the line, and its re-register passes.
accepted_reregister_check()
{
    awk '
        /stream: connection [0-9]+ accepted/ {
            match($0, /connection [0-9]+/)
            accepted[substr($0, RSTART + 11, RLENGTH - 11)] = 1
        }
        /stream: connection [0-9]+ up/ {
            match($0, /connection [0-9]+/)
            id = substr($0, RSTART + 11, RLENGTH - 11)
            inbound = (id in accepted)
            delete accepted[id]
        }
        /registration connection re-established, re-registering/ {
            if (inbound)
                bad = 1
            inbound = 0
        }
        END { exit bad }
    ' "$1/daemon.log"
}

# The RFC 5626 5.4 keepalive, answered inside the connection. The client opens
# one connection to the DUT's stream listener, sends the double CRLF ping and
# reads the answer off the same socket. That is the whole no-cleartext claim in
# one exchange: the pong is a connection payload, so over tls it is inside the
# record layer and no datagram carries it (src/proto/sip/stream.h, "Ping is
# answered inside the connection").
#
# SIPp cannot make this exchange. Its <send> body is a SIP message, and a
# scenario cannot put four bare bytes on the wire and read two back.
stream_ping_check()
{
    local flow_dir=$1
    local port=$2
    local kind=$3

    python3 - "$port" "$kind" "$(flow_loop_addr)" \
        >"$flow_dir/stream-ping.txt" 2>&1 <<'PY'
import socket
import ssl
import sys
import time

port = int(sys.argv[1])
kind = sys.argv[2]
host = sys.argv[3]

sock = socket.create_connection((host, port), timeout=10)
if kind == "tls":
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    sock = ctx.wrap_socket(sock)

sock.settimeout(10)
sock.sendall(b"\r\n\r\n")

# The pong is read until it is whole. A stream carries no message boundary, so
# a first read that returns one of the two bytes is normal and comparing it
# there would fail a listener that answered correctly.
deadline = time.monotonic() + 10
answer = b""
while len(answer) < 2 and time.monotonic() < deadline:
    chunk = sock.recv(16)
    if not chunk:
        break
    answer += chunk

sock.close()

print("ping sent 4 bytes, answer %r on the same connection" % (answer,))
sys.exit(0 if answer == b"\r\n" else 1)
PY
}

# The signalling ports of a stream flow: the DUT's listener and the peer's.
#
# Under 6 the filter names the address and no port at all. A pcap port
# primitive reads the next header field of the fixed IPv6 header (RFC 8200
# section 3) and stops there, so a packet behind a hop-by-hop, routing or
# fragment header matches no port term and never reaches the capture. The
# cleartext reader walks that chain and owes a verdict on it, and a filter that
# dropped those packets would leave the walk and its unreadable verdicts
# unreachable. The reader selects the two ports itself, in transport_scan,
# after the walk.
stream_capture_filter()
{
    if (( flow_family == 6 )); then
        printf 'ip6 and host ::1\n'
        return
    fi

    printf 'port %u or port %u\n' "$sipp_port" "$daemon_port"
}

# The no-cleartext pin. It reads the capture of a tls flow and
# answers one question: does any packet on the two signalling ports carry a
# SIP line or a bare RFC 5626 keepalive in the clear?
#
# What it proves and what it does not. A capture reading cannot prove that
# anything is encrypted; it can only prove that the cleartext forms are absent.
# The exact matcher is stated so the claim is readable: every TCP payload of
# the two ports is scanned for the byte strings below, and a payload made of
# CR and LF bytes alone is counted separately, because the four byte ping and
# the two byte pong are the keepalive forms that must not travel in the clear.
# Any UDP packet on either port is a failure by itself, since a stream account
# sends no datagram at all.
#
# It parses the pcap with python3 rather than tshark, so the pin is not
# silently skipped on a host that has the capture but not the dissector.
pcap_cleartext_check()
{
    local flow_dir=$1
    local port_a=$2
    local port_b=$3

    python3 - "$flow_dir/traffic.pcap" "$port_a" "$port_b" \
        >"$flow_dir/pcap-cleartext.txt" 2>&1 <<'PY'
import struct
import sys

path = sys.argv[1]
ports = {int(sys.argv[2]), int(sys.argv[3])}

# the cleartext forms a SIP element puts on the wire; any one of them inside a
# payload of the signalling ports is a leak
TOKENS = [
    b"SIP/2.0",
    b"REGISTER sip:",
    b"INVITE sip:",
    b"OPTIONS sip:",
    b"SUBSCRIBE sip:",
    b"BYE sip:",
    b"Call-ID:",
]
# The keepalive rule is a byte class, not an exact compare. TCP may coalesce
# two pings into one segment, so a payload of eight CR and LF bytes is as much
# a cleartext keepalive as a payload of four, and an exact compare against the
# ping and the pong would match neither and report the segment clean.
CRLF = set(b"\r\n")

# RFC 8200 section 4 chains the extension headers, and hop-by-hop, routing and
# destination options each state their own length in eight-octet units past the
# first eight. A fragment carries a piece of a datagram this reader cannot put
# back together, and an unknown next header hides whatever follows it, so both
# make the capture unreadable rather than clean.
IP6_EXT = {0: "hop-by-hop", 43: "routing", 60: "destination options"}
IP6_FRAGMENT = 44


# The transport body of one IPv6 frame, and the protocol that carries it. The
# fixed header is 40 bytes, the payload length sits at offset 4 and the next
# header at offset 6 (RFC 8200 section 3). A refusal answers a reason in place
# of the body.
def ip6_walk(ip):
    if len(ip) < 40:
        return None, "the capture holds an IPv6 frame shorter than its header"
    payload_len = struct.unpack(">H", ip[4:6])[0]
    nh = ip[6]
    body = ip[40:40 + payload_len]
    if len(body) < payload_len:
        return None, "the capture holds an IPv6 frame cut short of its payload"
    while nh in IP6_EXT:
        if len(body) < 8:
            return None, "the capture holds a cut IPv6 %s header" % IP6_EXT[nh]
        ext = (body[1] + 1) * 8
        if len(body) < ext:
            return None, "the capture holds a cut IPv6 %s header" % IP6_EXT[nh]
        nh = body[0]
        body = body[ext:]
    if nh == IP6_FRAGMENT:
        return None, "the capture holds an IPv6 fragment the reader cannot join"
    if nh not in (6, 17):
        return None, "the capture holds IPv6 next header %u, which the reader does not know" % nh
    return body, nh


try:
    blob = open(path, "rb").read()
except OSError as err:
    print("the capture could not be opened: %s" % (err,))
    sys.exit(2)

if len(blob) < 24:
    print("the capture is empty")
    sys.exit(2)

magic = blob[:4]
if magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
    endian = ">"
elif magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
    endian = "<"
else:
    print("unknown pcap magic %r" % (magic,))
    sys.exit(2)

linktype = struct.unpack(endian + "I", blob[20:24])[0]
if linktype != 1:
    print("unexpected link type %u, the reader handles ethernet only" % linktype)
    sys.exit(2)

offset = 24
tcp_payloads = 0
tcp_bytes = 0
udp_packets = 0
leaks = []


# The transport half of one frame, which is the same reading whatever carried
# it: the two signalling ports, the payload of a segment and the byte class of
# a keepalive do not change with the family of the header in front of them.
def transport_scan(proto, body):
    global tcp_payloads, tcp_bytes, udp_packets

    if proto == 6:
        if len(body) < 20:
            return
        sport, dport = struct.unpack(">HH", body[:4])
        if sport not in ports and dport not in ports:
            return
        doff = (body[12] >> 4) * 4
        payload = body[doff:]
        if not payload:
            return
        tcp_payloads += 1
        tcp_bytes += len(payload)
        for token in TOKENS:
            if token in payload:
                leaks.append("tcp %u->%u carries %r" % (sport, dport, token))
        if set(payload) <= CRLF:
            leaks.append("tcp %u->%u is a bare keepalive, %u byte(s) %r"
                         % (sport, dport, len(payload), payload))
    elif proto == 17:
        if len(body) < 8:
            return
        sport, dport = struct.unpack(">HH", body[:4])
        if sport not in ports and dport not in ports:
            return
        udp_packets += 1
        leaks.append("udp %u->%u carries %u bytes on a signalling port"
                     % (sport, dport, len(body) - 8))


while offset < len(blob):
    # A truncated capture is unreadable, never clean. tcpdump can be cut off
    # mid record, and a reader that stopped at the last whole record would
    # report the part it happened to hold as a clean window. The record
    # boundaries have to land exactly on the end of the file.
    if offset + 16 > len(blob):
        print("the capture ends inside a record header")
        sys.exit(2)

    _, _, incl, _ = struct.unpack(endian + "IIII", blob[offset:offset + 16])
    offset += 16
    if offset + incl > len(blob):
        print("the capture ends inside a record payload")
        sys.exit(2)

    frame = blob[offset:offset + incl]
    offset += incl
    if len(frame) >= 14 and frame[12:14] == b"\x86\xdd":
        body, proto = ip6_walk(frame[14:])
        if body is None:
            print(proto)
            sys.exit(2)
        transport_scan(proto, body)
        continue
    if len(frame) < 34 or frame[12:14] != b"\x08\x00":
        continue

    ip = frame[14:]
    ihl = (ip[0] & 0x0F) * 4
    proto = ip[9]
    total = struct.unpack(">H", ip[2:4])[0]
    ip = ip[:total]
    if len(ip) < ihl:
        continue

    transport_scan(proto, ip[ihl:])

print("tcp payload segments %u, %u bytes; udp packets on the signalling ports %u"
      % (tcp_payloads, tcp_bytes, udp_packets))
for leak in leaks:
    print("CLEARTEXT: %s" % leak)

# a capture that saw nothing proves nothing, so an empty one is not a pass
if tcp_payloads == 0:
    print("the capture holds no payload on the signalling ports")
    sys.exit(2)

sys.exit(1 if leaks else 0)
PY
}

# ------------------------------------------------------------ the family pins

# Every reader below answers at once under 4, so the default set carries no new
# assertion and every flow of it runs as it always did. Under 6 they are the
# whole added claim of the run: the family on the wire, in the headers the
# daemon wrote itself, in the stream it programmed and in its own status.

# The host of one Contact URI, taken from inside the angle brackets. A URI host
# is where RFC 3261 19.1.1 asks for brackets around an IPv6 reference, so the
# bracketed form is what the comparison expects and a bare "::1" fails it.
family_contact_host()
{
    local rest=$1

    rest=${rest#*<}
    rest=${rest%%>*}
    rest=${rest#sip:}
    rest=${rest#sips:}
    if [[ $rest = *@* ]]; then
        rest=${rest#*@}
    fi
    if [[ $rest = "["* ]]; then
        printf '%s\n' "${rest%%]*}]"
        return
    fi

    printf '%s\n' "${rest%%[:;]*}"
}

# The sent-by of every Via of one message (RFC 3261 20.42). It is asked of a
# message the daemon sent, so the value is the address the daemon puts on its
# own requests. Every Via of the message must carry it, and one value per line
# is part of the assertion for the reason stream_via_check states.
family_via_check()
{
    local flow_dir=$1
    local direction=$2
    local start_line=$3
    local port=$4
    # The directory of the message log, when another SIPp than the call peer
    # took the message: the registrar of register_prime.
    local log_dir=${5:-$flow_dir}
    local want
    local lines
    local line
    local sent_by
    local seen=0

    want="$(flow_loop_uri):$port"
    lines=$(sipp_msg_header_get "$log_dir" "$direction" "$start_line" Via) || return 1

    while IFS= read -r line; do
        [[ -n $line ]] || continue
        sent_by=$(sed -n \
            's|^Via:[[:space:]]*SIP/2\.0/[A-Za-z]\{1,\}[[:space:]]\{1,\}\([^;[:space:]]\{1,\}\).*|\1|p' \
            <<<"$line")
        printf '"%s" Via sent-by: %s\n' "$start_line" "$sent_by" \
            >>"$flow_dir/family-readers.txt"
        [[ $line != *,* ]] || return 1
        [[ $sent_by = "$want" ]] || return 1
        seen=$((seen + 1))
    done <<<"$lines"

    (( seen > 0 )) || return 1
    family_detail+=", Via sent-by $want"
}

# Every URI of every Contact header of one message. A header may carry more
# than one name-addr (RFC 3261 20.10), so the reader takes each of them and a
# second URI in another family fails here rather than passing behind the first.
# A header with no URI at all fails too: a missing pin is not a passing pin.
family_contact_check()
{
    local flow_dir=$1
    local direction=$2
    local start_line=$3
    local log_dir=${4:-$flow_dir}
    local want
    local lines
    local line
    local uri
    local host
    local uris=()
    local seen=0

    want=$(flow_loop_uri)
    lines=$(sipp_msg_header_get "$log_dir" "$direction" "$start_line" Contact) || return 1

    while IFS= read -r line; do
        [[ -n $line ]] || continue
        mapfile -t uris < <(grep -o '<sips\{0,1\}:[^>]*>' <<<"$line")
        (( ${#uris[@]} > 0 )) || return 1
        for uri in "${uris[@]}"; do
            host=$(family_contact_host "$uri")
            printf '"%s" Contact host: %s\n' "$start_line" "$host" \
                >>"$flow_dir/family-readers.txt"
            [[ $host = "$want" ]] || return 1
            seen=$((seen + 1))
        done
    done <<<"$lines"

    (( seen > 0 )) || return 1
    family_detail+=", Contact host $want in $seen URI(s)"
}

# The connection line of the SDP the daemon wrote (RFC 4566 section 5.7). Both
# halves are the assertion: a session with the right line and a second one in
# the other family would answer the presence pin alone.
family_sdp_check()
{
    local flow_dir=$1
    local counts

    counts=$(awk '
        /(UDP|TCP|TLS) message sent/ { dir = "sent"; next }
        /(UDP|TCP|TLS) message received/ { dir = "received"; next }
        dir == "received" {
            line = $0
            gsub(/\r/, "", line)
            if (line == "c=IN IP6 ::1")
                good++
            else if (line ~ /^c=IN IP4 /)
                bad++
        }
        END { print good + 0, bad + 0 }
    ' "$flow_dir/sipp-messages.log" 2>/dev/null)

    # An unreadable log answers no pair of numbers at all, and an empty answer
    # would satisfy a comparison against zero.
    [[ $counts = *' '* ]] || return 1
    printf 'SDP connection lines the daemon sent: %s as IP6 ::1, %s as IP4\n' \
        "${counts% *}" "${counts#* }" >>"$flow_dir/family-readers.txt"
    [[ ${counts% *} != 0 ]] || return 1
    [[ ${counts#* } = 0 ]] || return 1
    family_detail+=", SDP c=IN IP6 ::1 ${counts% *} time(s) and no c=IN IP4"
}

# The whole address half of the stream the daemon programmed, read off the
# backend that took it (hal/stub/stub.c stream_dump). The unit suite
# test_hal_stub links stub.c but makes no ubus connection to read the dump. This
# is therefore the only automated reading of those keys. It requires all eight
# that the dump writes unconditionally and compares every value it can derive:
#
#   local_ipver, remote_ipver    6, the family under test
#   local_addr, remote_addr      "::1", so the address bytes crossed as well
#   remote_port                  the media port the peer offered
#   remote_rtcp_port             that port plus one, which is what the two live
#                                backends derive when no a=rtcp names another
#   local_rtcp_port              local_port plus one, the same derivation
#
# The RTCP address keys are the other half. RFC 3605 puts a third address on
# the stream, and the dump writes the pair only for a stream that took one, so
# a scenario naming no a=rtcp must leave both keys absent and a scenario that
# names one must leave them present in this family.
#
# The reading does not ask for a running stream. A scenario that BYEs a few
# hundred milliseconds after the answer holds no window a poll of the ubus
# object can land in, and the stub keeps the description it was given after the
# stop, so the family of the record is the same answer before and after it. The
# running flag is pinned where a flow owns a window for it, in media_port_wait
# and in srtp_stream_wait, and the sample below records it either way.
family_stream_check()
{
    local flow_dir=$1
    local scenario=${2:-}
    local label=${3:-stream}
    # The port the peer offered for this description, which is the run's media
    # port for an audio leg and the one the T.38 answer names for an image one.
    local remote_port=${4:-$media_port}
    local expression=
    local sample
    local key

    (( flow_family == 6 )) || return 0

    for key in local_ipver local_addr local_port local_rtcp_port \
        remote_ipver remote_addr remote_port remote_rtcp_port; do
        expression+=" and (.line[0].stream[0] | has(\"$key\"))"
    done
    expression+=' and .line[0].stream[0].local_ipver == 6'
    expression+=' and .line[0].stream[0].remote_ipver == 6'
    expression+=' and .line[0].stream[0].local_addr == "::1"'
    expression+=' and .line[0].stream[0].remote_addr == "::1"'
    expression+=" and .line[0].stream[0].remote_port == $remote_port"
    expression+=" and .line[0].stream[0].remote_rtcp_port == $((remote_port + 1))"
    expression+=' and .line[0].stream[0].local_rtcp_port'
    expression+=' == (.line[0].stream[0].local_port + 1)'
    if [[ -n $scenario ]] &&
        grep -q '^[[:space:]]*a=rtcp:[0-9]\{1,\}[[:space:]]\{1,\}IN[[:space:]]' \
            "$scenario" 2>/dev/null; then
        expression+=' and (.line[0].stream[0] | has("rtcp_remote_ipver"))'
        expression+=' and .line[0].stream[0].rtcp_remote_ipver == 6'
    else
        expression+=' and (.line[0].stream[0] | has("rtcp_remote_ipver") | not)'
        expression+=' and (.line[0].stream[0] | has("rtcp_remote_addr") | not)'
    fi
    expression=${expression# and }

    if ! stub_wait "$flow_dir" "$expression" 15 "family-$label"; then
        family_failure_detail="the $label the daemon programmed is not the ipv6 one this flow negotiated: $(jq -c '.line[0].stream[0] | {running, local_ipver, local_addr, local_port, local_rtcp_port, remote_ipver, remote_addr, remote_port, remote_rtcp_port}' "$flow_dir/stub-family-$label-timeout.json" 2>/dev/null), expected remote port $remote_port"
        return 1
    fi

    sample=$(jq -c '.line[0].stream[0] | {running, local_ipver, local_addr, local_port, local_rtcp_port, remote_ipver, remote_addr, remote_port, remote_rtcp_port, rtcp_remote_ipver, rtcp_remote_addr}' \
        "$flow_dir/stub-family-$label.json" 2>/dev/null)
    printf '%s: %s\n' "$label" "$sample" >>"$flow_dir/family-readers.txt"
    family_detail+=", $label ipver 6 both ends on $remote_port"
}

# The family of every frame of the capture. The capture filter of a v6 run
# already names the family, so a run that fell back to v4 leaves an empty
# window here rather than a window of v4 frames; the ip4 count is the second
# half of the reading and stays at zero by that construction.
#
# It answers 2 for a capture it cannot read, which is a tcpdump verdict and not
# a family one, and the caller reports that as PARTIAL.
family_pcap_check()
{
    local flow_dir=$1

    python3 - "$flow_dir/traffic.pcap" >"$flow_dir/pcap-family.txt" 2>&1 <<'PY'
import struct
import sys

path = sys.argv[1]

try:
    blob = open(path, "rb").read()
except OSError as err:
    print("the capture could not be opened: %s" % (err,))
    sys.exit(2)

if len(blob) < 24:
    print("the capture is empty")
    sys.exit(2)

magic = blob[:4]
if magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
    endian = ">"
elif magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
    endian = "<"
else:
    print("unknown pcap magic %r" % (magic,))
    sys.exit(2)

linktype = struct.unpack(endian + "I", blob[20:24])[0]
if linktype != 1:
    print("unexpected link type %u, the reader handles ethernet only" % linktype)
    sys.exit(2)

offset = 24
ip4 = 0
ip6 = 0
other = 0

while offset < len(blob):
    # A truncated capture is unreadable, never a family answer. A reader that
    # stopped at the last whole record would report the part it happened to
    # hold, and an empty part reads the same as a run with no ip4 frame in it.
    if offset + 16 > len(blob):
        print("the capture ends inside a record header")
        sys.exit(2)

    _, _, incl, _ = struct.unpack(endian + "IIII", blob[offset:offset + 16])
    offset += 16
    if offset + incl > len(blob):
        print("the capture ends inside a record payload")
        sys.exit(2)

    frame = blob[offset:offset + incl]
    offset += incl
    if len(frame) < 14:
        other += 1
        continue

    ether = frame[12:14]
    if ether == b"\x86\xdd":
        ip6 += 1
    elif ether == b"\x08\x00":
        ip4 += 1
    else:
        other += 1

print("ip6 frames %u, ip4 frames %u, other frames %u" % (ip6, ip4, other))

sys.exit(0 if ip6 > 0 and ip4 == 0 else 1)
PY
}

# The readers a flow runs once its exchange is over: the two headers the daemon
# wrote, its own SDP for a flow that carries a call, its status and the family
# of the capture. The capture is stopped first, because a reader of a capture
# that is still being written finds a record cut in half.
#
# kind is "call" for a flow whose daemon sends a session description and
# "signal" for one that does not: a flow with no media cannot pass a media
# reader. The status is read last, so a flow that lost its family names the
# header it lost it in first.
family_check()
{
    local flow_dir=$1
    local direction=$2
    local start_line=$3
    local via_port=$4
    local kind=$5
    # The message log of the two headers. A flow under register_prime reads its
    # REGISTER off the log of the registrar; the SDP, the status and the
    # capture stay on the flow directory.
    local header_dir=${6:-$flow_dir}
    local status=0

    (( flow_family == 6 )) || return 0

    if ! family_via_check "$flow_dir" "$direction" "$start_line" "$via_port" \
        "$header_dir"; then
        family_failure_detail="the Via sent-by of the \"$start_line\" message is not $(flow_loop_uri):$via_port"
        return 1
    fi
    if ! family_contact_check "$flow_dir" "$direction" "$start_line" \
        "$header_dir"; then
        family_failure_detail="the Contact host of the \"$start_line\" message is not $(flow_loop_uri)"
        return 1
    fi
    if [[ $kind = call ]] && ! family_sdp_check "$flow_dir"; then
        family_failure_detail="the daemon's own SDP does not carry c=IN IP6 ::1 alone"
        return 1
    fi
    if ! status_expr_check "$flow_dir" '.lines[0].family == "ipv6"' family; then
        family_failure_detail="voice status does not report an ipv6 line: $(ubus -s "$current_socket" call voice status 2>/dev/null | jq -c '.lines[0] | {family, aor, contact}')"
        return 1
    fi
    family_detail+=", status family ipv6"

    capture_finish
    if (( capture_ready != 1 )); then
        family_failure_detail="the family capture was unavailable"
        return 2
    fi
    family_pcap_check "$flow_dir" || status=$?
    if (( status == 2 )); then
        family_failure_detail="the family capture could not be read: $(head -1 "$flow_dir/pcap-family.txt")"
        return 2
    fi
    if (( status != 0 )); then
        family_failure_detail="the capture is not an ipv6 one: $(head -1 "$flow_dir/pcap-family.txt")"
        return 1
    fi
    family_detail+=", capture $(head -1 "$flow_dir/pcap-family.txt")"
}

# The flow names a v6 run drives, and the whole set it drives with no argument
# of its own. Every other flow either asserts a v4 literal of its own (the
# transfer flows carry 127.0.0.1 dialog constants and a second config writer
# that keeps them) or has no family reader, so a run under 6 refuses them
# instead of reporting a mixed result under a v6 name.
family_v6_set=(flow-a flow-b uas_reinvite_hold uac_cancel uas_mwi_notify
    srtp-inbound stream-register-tcp stream-uas-tcp stream-uac-tls
    uac_t38_offer)

# The requested set against the family. It runs before the prerequisites and
# before the build, so a run that names a flow the v6 set does not hold stops
# without touching anything.
family_selection_check()
{
    local item
    local name

    (( flow_family == 6 )) || return 0

    for item in "$@"; do
        for name in "${family_v6_set[@]}"; do
            if [[ $item = "$name" ]]; then
                continue 2
            fi
        done
        printf 'ERROR: E2E_FAMILY 6 drives the v6 set alone, and %s is not in it\n' \
            "$item" >&2
        printf 'The v6 set is: %s\n' "${family_v6_set[*]}" >&2
        return 1
    done
}

# PARTIAL for a capture the reader could not read, FAIL for a family the
# readers refuse. A capture that cannot be read is a tcpdump verdict, and
# reporting it as a family finding would accuse the daemon of it.
family_verdict()
{
    if (( $1 == 2 )); then
        printf '%s\n' PARTIAL
        return
    fi

    printf '%s\n' FAIL
}

# The committed EC fixtures, composed into the frozen bundle paths the daemon
# reads (sip/tls.h, SIP_TLS_CERT_DEFAULT). Without them the TLS context comes
# up client only and the daemon takes no inbound connection, which is the
# state stream-uac-tls runs in and the state stream-uas-tls has to leave.
#
# The install refuses a bundle it does not own. The directory is a fixed
# absolute path outside the network namespace, so a certificate fetch or
# another harness run can already hold it, and overwriting that would break
# the other run.
# The lock over the two frozen paths, taken before either is read or written.
# mkdir is the create-or-fail primitive: it succeeds for exactly one caller and
# needs no read-then-write window of its own.
tls_lock_take()
{
    local name=$1

    if ! mkdir "$tls_lock_dir" 2>/dev/null; then
        return 1
    fi

    tls_lock_held=1
    printf 'pid %u, flow %s\n' "$$" "$name" >"$tls_lock_dir/owner" 2>/dev/null
    return 0
}

# Who holds it, for the verdict of the flow that could not take it. A lock with
# no owner file is one whose holder died between the mkdir and the write.
tls_lock_owner()
{
    local owner

    owner=$(cat "$tls_lock_dir/owner" 2>/dev/null)
    printf '%s\n' "${owner:-unknown}"
}

tls_lock_drop()
{
    if (( !tls_lock_held )); then
        return 0
    fi

    rm -f "$tls_lock_dir/owner"
    rmdir "$tls_lock_dir" 2>/dev/null || true
    tls_lock_held=0
}

# The ownership token on its own. The cert-fetch flow reserves the directory
# without putting anything in it: the certificate helper is what fills it
# there, and the teardown has to know afterwards whether this run made the
# directory or found it.
tls_bundle_claim()
{
    local name=$1
    local file

    # The lock comes first, before the three reads below: without it the reads
    # and the mkdir behind them are a window another run can land in.
    tls_lock_take "$name" || return 1

    # All three files guard the claim, not the pair alone: a bundle another
    # run owns can be mid-write, and a ca.crt without a pair is still someone
    # else's directory.
    for file in cert.pem key.pem ca.crt; do
        if [[ -e $tls_bundle_dir/$file ]]; then
            return 1
        fi
    done

    # Who made the directory decides who removes it. A directory that was
    # already there belongs to whatever put it there, so the teardown takes
    # away the three files and leaves the directory standing.
    if [[ -d $tls_bundle_dir ]]; then
        tls_bundle_owned=1
    else
        mkdir -p "$tls_bundle_dir" || return 1
        tls_bundle_owned=2
    fi
}

tls_bundle_install()
{
    local name=$1
    local src=$uvoice_dir/test/unit/tls

    tls_bundle_claim "$name" || return 1

    # Ownership is claimed before the first copy, so a copy that fails half
    # way still has its files removed by the teardown.
    cp "$src/server.crt" "$tls_bundle_dir/cert.pem" || return 1
    cp "$src/server.key" "$tls_bundle_dir/key.pem" || return 1
    cp "$src/ca.crt" "$tls_bundle_dir/ca.crt" || return 1

    # The post-condition is read back, because the target is a fixed absolute
    # path outside the network namespace: anything else on the host can remove
    # what this just wrote, and a daemon that finds no pair comes up client
    # only and reports no error of its own for it.
    for file in cert.pem key.pem ca.crt; do
        [[ -s $tls_bundle_dir/$file ]] || return 1
    done
}

# What the bundle directory holds right now, kept beside the flow so a run
# that lost its certificate pair says so instead of leaving a bare "no
# listener" verdict behind.
tls_bundle_record()
{
    local flow_dir=$1
    local label=$2

    {
        printf -- '--- %s ---\n' "$label"
        ls -l "$tls_bundle_dir" 2>&1
    } >>"$flow_dir/tls-bundle.txt"
}

tls_bundle_remove()
{
    if (( !tls_bundle_owned )); then
        return 0
    fi

    rm -f "$tls_bundle_dir/cert.pem" "$tls_bundle_dir/key.pem" "$tls_bundle_dir/ca.crt"
    # The helper's own scratch goes with the status token and not with this one.
    # files/uvoice_tls_cert.sh keeps the archive it downloaded, the two header
    # captures it compares and the reason it last reported in the same
    # directory, so a flow that drove the helper has to take them away or it
    # would refuse its own next run. A flow that only found the directory did
    # not write them, and removing them there destroys another helper's work:
    # the bundle token says who made the directory, and only the status token
    # says who ran the helper.
    if (( tls_status_owned )); then
        rm -f "$tls_bundle_dir/voip_cert.tgz" "$tls_bundle_dir/header.txt" \
            "$tls_bundle_dir/prev_header.txt" "$tls_bundle_dir/last_error_reason.txt"
        rm -f "$tls_status_file"
        tls_status_owned=0
    fi

    # 2 means this harness made the directory; 1 means it found it. An empty
    # rmdir of a directory somebody else owns would take their workspace away.
    if (( tls_bundle_owned == 2 )); then
        rmdir "$tls_bundle_dir" 2>/dev/null || true
    fi
    tls_bundle_owned=0
}

# The directory as a single line, for a failure detail. It is a separate
# reader from tls_bundle_record because a verdict string cannot hold a
# multi-line listing.
tls_bundle_list()
{
    local listing

    listing=$(find "$tls_bundle_dir" -maxdepth 1 -type f -printf '%f(%s) ' 2>&1)
    printf '%s\n' "${listing:-none}"
}

# The hook of one line. The line is the second argument and it defaults to the
# first one, the only line of a single-line flow; the two-line transfer needs
# the other subscriber's hook too.
hook_set()
{
    local state=$1
    local line=${2:-0}

    ubus -s "$current_socket" call voice.stub hook \
        "{\"line\":$line,\"state\":\"$state\"}" >/dev/null
}

digit_send()
{
    local digit

    for digit in 1 2 3 '#'; do
        ubus -s "$current_socket" call voice.stub digit \
            "{\"line\":0,\"digit\":\"$digit\"}" >/dev/null || return 1
        sleep 0.05
    done
}

flash_send()
{
    local line=${1:-0}

    ubus -s "$current_socket" call voice.stub flash \
        "{\"line\":$line}" >/dev/null
}

# One detected non-DTMF tone, with the direction the fax trigger gate reads and
# the edge the return-to-voice rule reads (hal.h u.tone.end). end and duration
# are encoded only when the caller names them, so a call that names neither
# injects the start row it always did.
#
#   tone_send <code> [direction] [end] [duration]
tone_send()
{
    local code=$1
    local direction=${2:-local}
    local end=${3:-}
    local duration=${4:-}
    local args="{\"line\":0,\"code\":\"$code\",\"direction\":\"$direction\""

    if [[ -n $end ]]; then
        if (( end )); then
            args+=',"end":true'
        else
            args+=',"end":false'
        fi
    fi
    if [[ -n $duration ]]; then
        args+=",\"duration\":$duration"
    fi
    args+='}'

    ubus -s "$current_socket" call voice.stub tone "$args" >/dev/null
}

# The first packet received on one stream (hal.h HAL_EVENT_RTP_FIRST_RX). The
# stub carries no RTP socket, so this verb is the only source of the record in
# the harness, and the stream has to be running for the stub to accept it.
#
#   rtp_rx_send [line] [stream]
rtp_rx_send()
{
    local line=${1:-0}
    local stream=${2:-0}

    ubus -s "$current_socket" call voice.stub rtp_rx \
        "{\"line\":$line,\"stream\":$stream}" >/dev/null
}

# One of the three diagnostic verbs, which are the production surface the klish
# voice commands drive; every other flow here injects through voice.stub, an
# object no shipped board carries. Both reach the same line_hal_event() intake,
# so the matrix answers a verb as it answers hardware.
verb_send()
{
    local verb=$1
    local args=$2

    ubus -s "$current_socket" call voice "$verb" "$args" >/dev/null
}

# One stub dump answered against one expression, with no waiting. A negative
# assertion cannot poll: a state this denies would pass the first sample and
# fail a later one, so the answer has to be a single reading.
stub_check()
{
    local flow_dir=$1
    local expression=$2
    local label=$3
    local output

    output=$(ubus -s "$current_socket" call voice.stub dump 2>/dev/null) || return 1
    printf '%s\n' "$output" >"$flow_dir/stub-$label.json"
    jq -e "$expression" <<<"$output" >/dev/null
}

# In UAS mode SIPp has no out-of-call scenario (it refuses -oocsf in server
# mode), so every daemon REGISTER opens an instance of the one scenario. A UAS
# call scenario answers it (test/sipp/README.md, "The REGISTER branch"), and
# the flow counts that instance in its expected successes. A scenario with no
# REGISTER branch aborts the instance while it waits for its first request.
# The count of those aborts is recorded; any other abort is not expected.
sipp_abort_classify()
{
    local flow_dir=$1
    local error_file=$flow_dir/sipp-errors.log

    absorbed_register=0
    absorbed_register_retrans=0
    foreign_abort=0
    foreign_abort_detail=

    [[ -f $error_file ]] || return 0

    absorbed_register=$(grep -c "Aborting call .*received 'REGISTER" "$error_file")
    absorbed_register_retrans=$(grep -c "Dead call .*received 'REGISTER" "$error_file")
    foreign_abort=$(grep "Aborting call" "$error_file" |
        grep -vc "received 'REGISTER")
    foreign_abort_detail=$(grep "Aborting call" "$error_file" |
        grep -v "received 'REGISTER" | head -1 |
        sed -e 's/^.*: Aborting/Aborting/' -e 's/$//' | cut -c 1-180)
    {
        printf 'absorbed REGISTER aborts: %s\n' "$absorbed_register"
        printf 'absorbed REGISTER retransmits: %s\n' "$absorbed_register_retrans"
        printf 'unattributed aborts: %s\n' "$foreign_abort"
    } >"$flow_dir/sipp-aborts.txt"
    return 0
}

uas_sipp_verdict()
{
    local flow_dir=$1
    local expected=$2
    local count

    sipp_abort_classify "$flow_dir"
    count=$(sipp_success_count "$flow_dir") || count=
    sipp_verdict_detail=$(printf 'SIPp rc %s, successful calls %s of %s, absorbed REGISTER %s (+%s retransmits)' \
        "$process_status" "${count:-unknown}" "$expected" \
        "$absorbed_register" "$absorbed_register_retrans")

    if (( foreign_abort != 0 )); then
        sipp_verdict_detail="$sipp_verdict_detail, unattributed abort: $foreign_abort_detail"
        return 1
    fi
    if [[ -z $count ]] || (( count < expected )); then
        return 1
    fi
    return 0
}

daemon_failure_reason()
{
    local flow_dir=$1

    sed -n 's/.*\(call [0-9.]\{1,\} [a-z-]\{1,\}: failed .*\)/\1/p' \
        "$flow_dir/daemon.log" 2>/dev/null | tail -1
}

scenario_pause_get()
{
    local scenario_file=$1
    local value

    value=$(sed -n 's/.*<pause[[:space:]][^>]*milliseconds="\([0-9]\{1,\}\)".*/\1/p' \
        "$scenario_file" | head -1)
    printf '%s\n' "${value:-0}"
}

daemon_log_require()
{
    local flow_dir=$1
    local pattern=$2
    local timeout_s=$3
    local deadline=$((SECONDS + timeout_s))

    while (( SECONDS < deadline )); do
        if grep -qF "$pattern" "$flow_dir/daemon.log" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# The same log, read for an order instead of a presence: the fatal bring-up
# owes its failure, then the decision it took, then the teardown that followed,
# and a set of three greps would pass on any order at all.
daemon_log_order()
{
    local flow_dir=$1
    local pattern
    local line
    local previous=0

    shift
    for pattern in "$@"; do
        line=$(grep -nF -m1 "$pattern" "$flow_dir/daemon.log" 2>/dev/null | cut -d: -f1)
        if [[ -z $line ]] || (( line <= previous )); then
            return 1
        fi
        previous=$line
    done
}

# The negative counterpart: a line that must not appear after a marker. A plain
# grep cannot answer it, because the same line is legitimate before the marker
# (a bring-up retry belongs to a running daemon, not to one that is stopping).
daemon_log_absent_after()
{
    local flow_dir=$1
    local marker=$2
    local pattern=$3
    local marker_line
    local hit

    marker_line=$(grep -nF -m1 "$marker" "$flow_dir/daemon.log" 2>/dev/null | cut -d: -f1)
    if [[ -z $marker_line ]]; then
        return 1
    fi
    hit=$(grep -nF "$pattern" "$flow_dir/daemon.log" 2>/dev/null | cut -d: -f1 |
        awk -v marker="$marker_line" '$1 > marker { print; exit }')
    [[ -z $hit ]]
}

flow_prepare()
{
    local name=$1
    local scenario=$2
    local session_expires=${3:-}

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || return 1
    config_dir=$flow_dir/config
    config_write "$config_dir" "$sipp_port" "$daemon_port" "$session_expires" || return 1
    capture_start "$flow_dir" "$sipp_port" || true
    sipp_start "$flow_dir" "$scenario" "$sipp_port" "$daemon_port" "$media_port" 2 || return 1
    daemon_start "$flow_dir" "$config_dir" "$name"
}

outbound_dial_start()
{
    local flow_dir=$1

    registration_wait "$flow_dir" || return 1
    hook_set seize || return 1
    status_wait "$flow_dir" OFFHOOK_DIAL 5 offhook || return 1
    stub_wait "$flow_dir" '.line[0].tone == 1' 3 dial-tone || return 1
    digit_send || return 1
}

capture_finish()
{
    sleep 0.5
    process_stop "$current_capture_pid" INT
    current_capture_pid=
}

# The delay from a final answer to the retry of the request it refused, read
# off the capture with tshark. It writes timing-<code>.txt and
# timing-<code>-result.txt. The statuses: 2 no capture, 3 no retry, 1 a delay
# outside the window. An empty upper bound leaves the window open above. With
# first_only 1, the first answer is the mark, and a later answer of the same
# code does not move it.
timing_retry_check()
{
    local flow_dir=$1
    local code=$2
    local method=$3
    local method_term=$4
    local lower=$5
    local upper=$6
    local first_only=$7

    if [[ $capture_ready -ne 1 ]] || ! command -v tshark >/dev/null 2>&1; then
        return 2
    fi
    # decode-as pins SIP on the drawn ports: the pid-derived seed can land on
    # a port another wireshark dissector claims outright (26000 is Quake), and
    # an exact-port dissector beats the SIP heuristic, emptying the analysis
    tshark -r "$flow_dir/traffic.pcap" \
        -d "udp.port==$sipp_port,sip" -d "udp.port==$daemon_port,sip" \
        -Y "$method_term || sip.Status-Code == $code" \
        -T fields -E separator='|' \
        -e frame.time_epoch -e sip.Method -e sip.Status-Code \
        >"$flow_dir/timing-$code.txt" 2>"$flow_dir/tshark-$code.log" || return 2
    awk -F '|' -v code="$code" -v method="$method" -v lower="$lower" \
        -v upper="$upper" -v first_only="$first_only" '
        $3 == code && !(first_only && response_seen) {
            response_time = $1
            response_seen = 1
            next
        }
        response_seen && $2 == method {
            delta = $1 - response_time
            printf "%s retry delay: %.6f seconds\n", code, delta
            retry_seen = 1
            exit !(delta >= lower && (upper == "" || delta <= upper))
        }
        END { if (!retry_seen) exit 3 }
    ' "$flow_dir/timing-$code.txt" >"$flow_dir/timing-$code-result.txt"
}

# The source port of the INVITE term names the DUT. The retry under test is
# the DUT's own, and a peer that retries inside the same window puts an INVITE
# of its own on the wire first: a field switch stands at 1.9 s against the
# 2.1 s floor of RFC 3261 section 14.1, so the reader would measure the wrong
# message in glare-mutual. The 491 term needs no such term: no flow here sees
# a 491 from the DUT.
timing_491_check()
{
    timing_retry_check "$1" 491 INVITE \
        "(sip.Method == \"INVITE\" && udp.srcport == $daemon_port)" 2.1 4.0 0
}

timing_503_check()
{
    timing_retry_check "$1" 503 REGISTER 'sip.Method == "REGISTER"' 5.0 '' 0
}

# The delay from the first 403 to the next REGISTER. The credential wait is
# fixed at 30 s (sip/reg.h SIP_REG_BACKOFF_AUTH); the window allows for one
# retry timer tick and the scheduling of the harness.
timing_403_check()
{
    timing_retry_check "$1" 403 REGISTER 'sip.Method == "REGISTER"' 29.5 33.0 1
}

# The INVITE to 180 distance of every call of a run, read off the capture the
# way timing_491_check reads the 491 retry. The field fault is a 180 that never
# came, so the reader keys both messages by Call-ID and answers for each call
# instead of for the first one. It writes one line per call to timing-180.txt
# and the verdict to timing-180-result.txt.
#
# The caller passes the number of calls the run placed, because a capture that
# lost packets holds fewer and every reading below would still pass on the
# calls it kept. The statuses: 2 is a capture the reader could not open, which
# the caller reports as PARTIAL; 3 is a capture with no call at all; 4 is a
# capture with fewer calls than the run placed; 5 is a call that drew no 180;
# 1 is a call whose 180 stood past the bound.
timing_180_check()
{
    local flow_dir=$1
    local expected=$2

    # A stale file from an earlier run must not answer for this one: the exits
    # above the bound leave the reader with nothing to write.
    : >"$flow_dir/timing-180.txt"
    if [[ $capture_ready -ne 1 ]] || ! command -v tshark >/dev/null 2>&1; then
        return 2
    fi
    # The decode-as rationale of timing_retry_check, plus the source port: the
    # offer of the caller and the 180 of the DUT are the pair under test, and
    # a copy of either from the other side would enter the same Call-ID with a
    # time of its own.
    tshark -r "$flow_dir/traffic.pcap" \
        -d "udp.port==$sipp_port,sip" -d "udp.port==$daemon_port,sip" \
        -Y "(sip.Method == \"INVITE\" && udp.srcport == $sipp_port) || (sip.Status-Code == 180 && udp.srcport == $daemon_port)" \
        -T fields -E separator='|' \
        -e frame.time_epoch -e sip.Method -e sip.Status-Code -e sip.Call-ID \
        >"$flow_dir/timing-180-fields.txt" 2>"$flow_dir/tshark-180.log" || return 2
    awk -F '|' -v deltas="$flow_dir/timing-180.txt" -v expected="$expected" '
        # The first copy of each message decides. A retransmitted INVITE keeps
        # the time of the offer it repeats, and a repeated 180 keeps the time
        # the phone first rang.
        $2 == "INVITE" && !($4 in offered) { offered[$4] = $1; order[++calls] = $4; next }
        $3 == "180" && ($4 in offered) && !($4 in rang) { rang[$4] = $1 - offered[$4] }
        END {
            for (i = 1; i <= calls; i++) {
                id = order[i]
                if (!(id in rang)) {
                    printf "call %u %s: no 180\n", i, id > deltas
                    silent++
                    continue
                }
                printf "call %u %s: %.6f seconds\n", i, id, rang[id] > deltas
                if (rang[id] > slowest)
                    slowest = rang[id]
                if (fastest == "" || rang[id] < fastest)
                    fastest = rang[id]
            }
            if (calls == 0)
                exit 3
            if (calls < expected) {
                printf "the capture holds %u of the %u calls placed\n", calls, expected
                exit 4
            }
            if (silent) {
                printf "%u of %u calls drew no 180\n", silent, calls
                exit 5
            }
            printf "%u calls, INVITE to 180 from %.6f to %.6f seconds\n",
                   calls, fastest, slowest
            exit !(slowest <= 1.0)
        }
    ' "$flow_dir/timing-180-fields.txt" >"$flow_dir/timing-180-result.txt"
}

# The distance from the 401 to the credentialed retry of a fax upspeed, read
# off the capture the way timing_180_check reads the 180 of a call. The
# field capture measures 62 ms for that round trip.
#
# The bound of 1.0 second is generous, and it is still the one reading of this
# flow that catches a slow retry. The scenario waits 10 seconds for the retry
# and the upspeed stub_wait waits 15, so a retry that sat on a timer for a
# second would satisfy both and nothing else would report it.
#
# The pair is keyed by CSeq number. The 401 answers the challenged re-INVITE
# and the retry takes the next number (RFC 3261 section 22.2), so a
# retransmission of the challenged offer cannot pass for the retry.
#
# The statuses: 2 is a capture the reader could not open, which the caller
# reports as PARTIAL; 3 is a capture with no 401 at all; 4 is a capture that
# holds no retry behind the 401; 1 is a retry past the bound.
timing_fax_auth_check()
{
    local flow_dir=$1
    # The bound, in seconds. It stands here alone: the failing verdict line
    # names it, so the caller repeats no number of its own.
    local bound=1.0

    # A stale file from an earlier run must not answer for this one: the exits
    # above the bound leave the reader with no delta to write.
    : >"$flow_dir/timing-fax-auth.txt"
    if [[ $capture_ready -ne 1 ]] || ! command -v tshark >/dev/null 2>&1; then
        return 2
    fi
    # The decode-as rationale of timing_retry_check, plus the source port: the
    # challenge belongs to the peer and the retry to the daemon, and a copy of
    # either from the other side would enter the reading with a time of its own.
    # The 401 term names the method of its own CSeq too, because the reader
    # anchors on the first 401 the peer sent and a 401 to another request would
    # take that place.
    tshark -r "$flow_dir/traffic.pcap" \
        -d "udp.port==$sipp_port,sip" -d "udp.port==$daemon_port,sip" \
        -Y "(sip.Method == \"INVITE\" && udp.srcport == $daemon_port) || (sip.Status-Code == 401 && sip.CSeq.method == \"INVITE\" && udp.srcport == $sipp_port)" \
        -T fields -E separator='|' \
        -e frame.time_epoch -e sip.Method -e sip.Status-Code -e sip.CSeq.seq \
        >"$flow_dir/timing-fax-auth-fields.txt" 2>"$flow_dir/tshark-fax-auth.log" || return 2
    awk -F '|' -v bound="$bound" '
        # The first copy of each message decides. A retransmitted 401 repeats
        # the answer to the offer in front of it, and a repeated retry keeps
        # the time the credentials first went out.
        $3 == "401" && !challenged { challenge = $1; want = $4 + 1; challenged = 1; next }
        challenged && $2 == "INVITE" && $4 == want && !found {
            delta = $1 - challenge
            found = 1
        }
        END {
            if (!challenged) {
                print "the capture holds no 401 to the upspeed re-INVITE"
                exit 3
            }
            if (!found) {
                printf "the capture holds no CSeq %u retry behind the 401\n", want
                exit 4
            }
            if (delta > bound + 0) {
                printf "401 to credentialed retry: %.6f seconds, past the %s second bound\n",
                       delta, bound
                exit 1
            }
            printf "401 to credentialed retry: %.6f seconds\n", delta
        }
    ' "$flow_dir/timing-fax-auth-fields.txt" >"$flow_dir/timing-fax-auth.txt"
}

flow_a_run()
{
    local name=flow-a-outbound
    local detail
    local bind_dev_note=
    local pause_ms
    local family_status=0

    if ! flow_prepare "$name" "$sipp_dir/uas_basic_call.xml"; then
        flow_stop
        result_print FAIL "$name" "startup failed"
        return
    fi
    if ! outbound_dial_start "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "off-hook dial setup failed"
        return
    fi
    if ! status_wait "$flow_dir" CONNECTED 15 connected; then
        flow_stop
        result_print FAIL "$name" "daemon did not reach CONNECTED"
        return
    fi
    if ! media_port_wait "$flow_dir" 5; then
        flow_stop
        result_print FAIL "$name" "stream 0 did not use the SDP media port"
        return
    fi
    if ! family_stream_check "$flow_dir" "$sipp_dir/uas_basic_call.xml"; then
        flow_stop
        result_print FAIL "$name" "$family_failure_detail"
        return
    fi
    # The harness runs a bare ubusd with no network.interface object, and
    # if_nametoindex("wan") is 0 inside the private network namespace, so the
    # account resolves no device and the key is absent. The logical name "wan"
    # on the stream fails the check too. Outside that namespace the absence
    # says nothing, so the check is reported as skipped instead of being
    # asserted on the host's netdev set.
    if [[ ${E2E_PRIVATE_NET:-0} == 1 ]]; then
        if ! stub_check "$flow_dir" '(.line[0].stream[0] | has("bind_dev")) == false' bind-dev; then
            flow_stop
            result_print FAIL "$name" "stream 0 took a bind device the account did not resolve"
            return
        fi
    else
        bind_dev_note=", bind-dev check skipped: host namespace"
    fi
    # The scenario waits for the BYE only after a scripted pause. A BYE that
    # arrives during that pause is an unexpected message and aborts the call,
    # so the hook release has to wait the pause out.
    pause_ms=$(scenario_pause_get "$sipp_dir/uas_basic_call.xml")
    sleep "$(( (pause_ms + 1500) / 1000 ))"
    hook_set release || true
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not complete the BYE exchange"
        return
    fi
    if ! stub_wait "$flow_dir" '.line[0].stream[0].running == false' 5 stopped; then
        flow_stop
        result_print FAIL "$name" "stream 0 remained active after the BYE"
        return
    fi
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "daemon did not return to IDLE"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 2; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    family_check "$flow_dir" received "INVITE " "$daemon_port" call || family_status=$?
    if (( family_status != 0 )); then
        flow_stop
        result_print "$(family_verdict "$family_status")" "$name" "$family_failure_detail"
        return
    fi
    detail=$sipp_verdict_detail
    flow_stop
    result_print PASS "$name" \
        "$detail, daemon and media assertions passed$bind_dev_note$family_detail"
}

flow_b_run()
{
    local name=flow-b-inbound
    local family_status=0

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    config_write "$config_dir" "$sipp_port" "$daemon_port" || true
    capture_start "$flow_dir" "$sipp_port" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/uac_basic_call.xml" "$sipp_port" "$daemon_port" "$media_port"; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! status_wait "$flow_dir" RINGING 10 ringing; then
        flow_stop
        result_print FAIL "$name" "daemon did not reach RINGING"
        return
    fi
    if ! stub_wait "$flow_dir" '.line[0].ringing == true and .line[0].cid_number == "sipp" and .line[0].cid_name == "SIPp"' 3 cid; then
        flow_stop
        result_print FAIL "$name" "stub ring or caller ID assertion failed"
        return
    fi
    hook_set seize || true
    if ! status_wait "$flow_dir" CONNECTED 10 connected; then
        flow_stop
        result_print FAIL "$name" "daemon did not answer into CONNECTED"
        return
    fi
    if ! media_port_wait "$flow_dir" 5; then
        flow_stop
        result_print FAIL "$name" "stream 0 did not use the SDP media port"
        return
    fi
    if ! family_stream_check "$flow_dir" "$sipp_dir/uac_basic_call.xml"; then
        flow_stop
        result_print FAIL "$name" "$family_failure_detail"
        return
    fi
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not finish its BYE exchange"
        return
    fi
    if (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "SIPp exited with status $process_status"
        return
    fi
    if ! stub_wait "$flow_dir" '.line[0].ringing == false and .line[0].stream[0].running == false' 5 stopped; then
        flow_stop
        result_print FAIL "$name" "ring or stream remained active after BYE"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "daemon did not return to IDLE after release"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    family_check "$flow_dir" received "REGISTER " "$daemon_port" call \
        "$flow_dir/register" || family_status=$?
    if (( family_status != 0 )); then
        flow_stop
        result_print "$(family_verdict "$family_status")" "$name" "$family_failure_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, caller ID, ring, media, and IDLE assertions passed$family_detail"
}

# The three diagnostic verbs end to end. What is under test is not the verbs'
# own answers, which the unit suite already pins, but what they cause: the
# seize has to reach the dial tone state, the release has to put the line back,
# the dial has to place a call the SIPp UAS answers, the second release has to
# clear that call on the wire, and the record left behind has to carry the talk
# window. Every assertion is therefore read from the daemon or the peer, never
# from the verb's return.
verbs_run()
{
    local name=flow-c-verbs
    local number=123
    # One second of talk. The record's duration is read against it below, and
    # the window is a compromise: long enough to stand clear of the call setup,
    # far enough below the five second timer-K drain to tell the two stamps
    # apart, and clear of the scenario's own 20 ms pause by a wide margin.
    local talk_s=1
    local detail

    if ! flow_prepare "$name" "$sipp_dir/uas_churn.xml" ||
        ! registration_wait "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "startup failed"
        return
    fi
    if ! verb_send seize '{"line":0}'; then
        flow_stop
        result_print FAIL "$name" "the seize verb was refused"
        return
    fi
    if ! status_wait "$flow_dir" OFFHOOK_DIAL 5 offhook; then
        flow_stop
        result_print FAIL "$name" "the seize verb did not take the line off hook"
        return
    fi
    if ! verb_send release '{"line":0}'; then
        flow_stop
        result_print FAIL "$name" "the release verb was refused"
        return
    fi
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "the release verb did not return the line to IDLE"
        return
    fi
    # The dial verb makes the seize itself, because the release above left the
    # line on hook. The '#' is the escape digit, so the number the call carries
    # is the digits in front of it.
    if ! verb_send dial "{\"line\":0,\"number\":\"$number#\"}"; then
        flow_stop
        result_print FAIL "$name" "the dial verb was refused"
        return
    fi
    if ! status_wait "$flow_dir" CONNECTED 15 connected; then
        flow_stop
        result_print FAIL "$name" "the dialled call did not reach CONNECTED"
        return
    fi
    if ! status_expr_wait "$flow_dir" \
        "(.call[0].calls | length) == 1 and (.call[0].calls[0].state) == \"active\" and (.call[0].calls[0].inbound) == false and (.call[0].calls[0].number) == \"$number\"" \
        5 leg; then
        flow_stop
        result_print FAIL "$name" "the line carries no active outbound leg for $number"
        return
    fi
    if ! ubus -s "$current_socket" call voice.stub media_stats \
        '{"line":0,"stream":0,"mos_lq":"4.40","mos_cq":"4.01"}' >/dev/null; then
        flow_stop
        result_print FAIL "$name" "the stub refused the two-decimal MOS values"
        return
    fi
    # The stats_live verb. The live per-call media query is the one voice
    # method that answers from the DSP rather than from daemon state, and it is
    # asked here because it needs a stream that is still running: the record's
    # own media table is taken as the call ends, seconds later. The reply is
    # deferred, so a table arriving at all also proves the deferred path
    # completed. The counters keep the canned values, while the MOS strings
    # exercise replay. Every key of the table is compared against the record's
    # table below.
    if ! ubus -s "$current_socket" call voice stats_live '{"line":0,"slot":0}' \
        >"$flow_dir/stats-live.json" 2>&1 ||
        ! jq -e '.line == 0 and .slot == 0 and .media.packets_sent == 1500 and .media.packets_recv == 1498 and .media.bytes_sent == 240000 and .media.bytes_recv == 239680 and .media.rfactor == 88 and .media.mos_lq == "4.40" and .media.mos_cq == "4.01"' \
            "$flow_dir/stats-live.json" >/dev/null; then
        flow_stop
        result_print FAIL "$name" \
            "the live media query answered no table for the connected call: $(head -c 200 "$flow_dir/stats-live.json")"
        return
    fi
    sleep "$talk_s"
    if ! verb_send release '{"line":0}'; then
        flow_stop
        result_print FAIL "$name" "the release verb was refused during the call"
        return
    fi
    if ! status_wait "$flow_dir" IDLE 5 cleared; then
        flow_stop
        result_print FAIL "$name" "the release verb did not clear the call"
        return
    fi
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not complete the BYE exchange"
        return
    fi
    # The slot outlives the call: the leg stays in "bye-sent" while its BYE
    # transaction sits out timer K, which is five seconds on UDP, so the drain
    # is waited out rather than asserted away. A leaked leg never drains.
    if ! status_expr_wait "$flow_dir" '(.call[0].calls | length) == 0' 10 drained; then
        flow_stop
        result_print FAIL "$name" "the call slot did not drain after the clear"
        return
    fi
    if ! cdr_expr_wait "$flow_dir" \
        "(.cdr | length) == 1 and (.cdr | last | .seq) == 1 and (.cdr | last | .inbound) == false and (.cdr | last | .answered) == true and (.cdr | last | .number) == \"$number\" and (.cdr | last | .media_state) == \"present\" and (.cdr | last | .in_progress) == null" \
        15 closed; then
        flow_stop
        result_print FAIL "$name" "the call left no closed answered record with its media"
        return
    fi
    # The record stops at the BYE and not at the drain behind it, so a call
    # that talked for the window above reads as about that window. The upper
    # bound is what tells the two stamps apart: a duration taken at the drain
    # would carry the five seconds of timer K on top.
    if ! jq -e "(.cdr | last | .duration_ms) >= $((talk_s * 1000)) and (.cdr | last | .duration_ms) < $((talk_s * 1000 + 2000))" \
        "$flow_dir/cdr-closed.json" >/dev/null; then
        flow_stop
        result_print FAIL "$name" \
            "the record duration is $(jq -c '.cdr | last | .duration_ms' "$flow_dir/cdr-closed.json") ms, which is outside the talk window"
        return
    fi
    # One vocabulary. The live query and the record render the same media
    # table through the same cdr renderer, and the stub answers one stream
    # with one set of numbers, so the two tables must compare equal key for
    # key. A second renderer, or a key that only one of them carries, fails
    # here rather than reaching an operator as two dialects of one reading.
    if ! jq -es '.[0].media == (.[1].cdr | last | .media)' \
        "$flow_dir/stats-live.json" "$flow_dir/cdr-closed.json" >/dev/null; then
        flow_stop
        result_print FAIL "$name" \
            "the live media table and the record media table are not the same table"
        return
    fi
    # The other half of the timeout: a query that was answered must not fire
    # its timer afterwards, or the daemon sends a second status for a request
    # it already closed. The window is structural in this flow: the talk pause
    # is one second and the slot drain waits out the five of timer K, so more
    # than the five second timer has passed since the query above. The unit
    # suite cannot pin this, because ubus.c is not one of its translation
    # units and the timer lives there.
    if grep -q "produced no answer" "$flow_dir/daemon.log"; then
        flow_stop
        result_print FAIL "$name" \
            "the answered live query fired its timeout anyway"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 2; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    detail=$sipp_verdict_detail
    flow_stop
    result_print PASS "$name" \
        "$detail, the three verbs drove the off-hook state, the answered call, the clear and a $(jq -c '.cdr | last | .duration_ms' "$flow_dir/cdr-closed.json") ms record, and the live media query answered the record's own table mid-call"
}

# One outbound call driven by a committed scenario. The optional arguments
# serve the flows built on the same body: a name of their own and the
# subscriber gesture the scenario waits for. With neither the function is the
# scenario runner it always was.
committed_outbound_run()
{
    local scenario=$1
    local name=${2:-$scenario}
    # The function that plays the subscriber once the call is up. It takes the
    # flow directory, writes its reason into committed_drive_detail and
    # releases the hook itself, because the scenarios it serves wait for the
    # BYE of the DUT.
    local drive=${3:-}
    local committed_drive_detail=
    local session_expires=
    local needs_release=0
    local sipp_ok=0
    local reason=
    local timing_status=0
    local family_status=0

    if [[ $scenario = uas_session_timer_422 ]]; then
        session_expires=1800
        needs_release=1
    fi
    if ! flow_prepare "$name" "$sipp_dir/$scenario.xml" "$session_expires"; then
        flow_stop
        result_print FAIL "$name" "startup failed"
        return
    fi
    if ! outbound_dial_start "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "outbound dial setup failed"
        return
    fi
    # The one moment this runner holds a live stream. It has no media wait of
    # its own, because the scenario is what drives the exchange, so the family
    # reader waits for the stream the answer programmed.
    if ! family_stream_check "$flow_dir" "$sipp_dir/$scenario.xml"; then
        flow_stop
        result_print FAIL "$name" "$family_failure_detail"
        return
    fi
    if [[ $scenario = uas_491_glare || $scenario = uas_glare_mutual ]]; then
        if ! status_wait "$flow_dir" CONNECTED 15 connected; then
            flow_stop
            result_print FAIL "$name" "initial dialog did not reach CONNECTED"
            return
        fi
        flash_send || true
    fi
    if (( needs_release )); then
        if ! status_wait "$flow_dir" CONNECTED 15 connected; then
            reason=$(daemon_failure_reason "$flow_dir")
            flow_stop
            result_print FAIL "$name" \
                "corrected INVITE did not reach CONNECTED${reason:+, daemon reported: $reason}"
            return
        fi
        hook_set release || true
    fi
    if [[ -n $drive ]]; then
        if ! "$drive" "$flow_dir"; then
            flow_stop
            result_print FAIL "$name" "$committed_drive_detail"
            return
        fi
        needs_release=1
    fi
    if sipp_finish_wait 45; then
        if uas_sipp_verdict "$flow_dir" 2; then
            sipp_ok=1
        fi
    else
        sipp_verdict_detail="SIPp did not finish within 45 seconds"
    fi
    if (( !needs_release )); then
        hook_set release || true
    fi
    status_save "$flow_dir" final
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "daemon did not return to IDLE"
        return
    fi
    if (( !sipp_ok )); then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    if [[ $scenario = uas_491_glare || $scenario = uas_glare_mutual ]]; then
        capture_finish
        timing_491_check "$flow_dir" || timing_status=$?
        if (( timing_status == 2 )); then
            flow_stop
            result_print PARTIAL "$name" "$sipp_verdict_detail, packet timing capture unavailable"
            return
        fi
        if (( timing_status == 3 )); then
            flow_stop
            result_print FAIL "$name" "the capture holds no INVITE retry after the 491"
            return
        fi
        if (( timing_status != 0 )); then
            flow_stop
            result_print FAIL "$name" \
                "491 retry was outside the 2.1 through 4.0 second window: $(cat "$flow_dir/timing-491-result.txt")"
            return
        fi
    fi
    family_check "$flow_dir" received "INVITE " "$daemon_port" call || family_status=$?
    if (( family_status != 0 )); then
        flow_stop
        result_print "$(family_verdict "$family_status")" "$name" "$family_failure_detail"
        return
    fi
    flow_stop
    if [[ -s $flow_dir/timing-491-result.txt ]]; then
        result_print PASS "$name" \
            "$sipp_verdict_detail, $(cat "$flow_dir/timing-491-result.txt"), daemon end-state assertions passed$family_detail"
        return
    fi
    result_print PASS "$name" \
        "$sipp_verdict_detail, daemon end-state assertions passed$family_detail"
}

# The subscriber of legacy-hold, and the reader of every step the RFC 2543
# hold sequence owes. Each wait is on the state the exchange in front
# of it produced, so no step rides a margin.
#
# The hold answer of the switch is c=0.0.0.0, which is a remote hold on top of
# our own, so the leg reads inactive on G.729 from the hold onward (sdp.c
# dir_answer(), RFC 3264 section 8.4). The peer then holds the leg itself, and
# that exchange leaves the same reading, so the resume waits for the ACK of the
# peer in the message log and for the leg back at active: a leg inside an
# exchange takes no operation of ours (call.c leg_free_is()).
legacy_hold_drive()
{
    local flow_dir=$1

    committed_drive_detail=
    if ! status_wait "$flow_dir" CONNECTED 15 connected; then
        committed_drive_detail="the call did not reach CONNECTED"
        return 1
    fi
    if ! flash_send; then
        committed_drive_detail="the hold flash was refused"
        return 1
    fi
    if ! status_expr_wait "$flow_dir" \
        '.call[0].state == "HOLD_DIAL" and
         .call[0].calls[0].direction == "inactive" and
         .call[0].calls[0].codec == "G729"' \
        15 legacy-held; then
        committed_drive_detail="the hold answer did not leave the leg inactive on G.729"
        return 1
    fi
    if ! sipp_msg_wait "$flow_dir" sent "ACK " 1 15; then
        committed_drive_detail="the peer never acknowledged its own re-INVITE"
        return 1
    fi
    if ! status_expr_wait "$flow_dir" '.call[0].calls[0].state == "active"' \
        10 legacy-peer-hold; then
        committed_drive_detail="the leg stayed inside the peer's own exchange"
        return 1
    fi
    if ! flash_send; then
        committed_drive_detail="the resume flash was refused"
        return 1
    fi
    # The ACK of the offerless exchange carries the answer, so this reading is
    # the whole verdict of flows c and d: the stale hold answer to the resume
    # did not strand the call, and the offer in the 200 was an active one.
    if ! status_expr_wait "$flow_dir" \
        '.call[0].state == "CONNECTED" and
         .call[0].calls[0].direction == "sendrecv" and
         .call[0].calls[0].codec == "PCMU"' \
        20 legacy-resumed; then
        committed_drive_detail="the offerless exchange left the leg off PCMU sendrecv"
        return 1
    fi
    if ! stub_check "$flow_dir" \
        '.line[0].stream[0].running == true and
         .line[0].stream[0].direction == 3' legacy-stream; then
        committed_drive_detail="the resumed stream was not programmed sendrecv"
        return 1
    fi
    hook_set release || true
}

legacy_hold_run()
{
    committed_outbound_run uas_legacy_hold legacy-hold legacy_hold_drive
}

glare_mutual_run()
{
    committed_outbound_run uas_glare_mutual glare-mutual
}

uac_cancel_run()
{
    local name=uac_cancel
    local marker
    local family_status=0

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    config_write "$config_dir" "$sipp_port" "$daemon_port" || true
    capture_start "$flow_dir" "$sipp_port" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/uac_cancel.xml" "$sipp_port" "$daemon_port" "$media_port"; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! sipp_finish_wait 20; then
        flow_stop
        result_print FAIL "$name" "SIPp CANCEL scenario did not finish"
        return
    fi
    # The checks run after SIPp finished, when the ring is over, so the daemon
    # log is the record of the ring and of its stop.
    for marker in \
        '[fsm] line 0 CALL_INCOMING in IDLE' \
        '[hal] stub: line 0 ring start' \
        '[fsm] line 0 CALL_REMOTE_CANCEL in RINGING' \
        '[hal] stub: line 0 ring stop'; do
        if ! daemon_log_require "$flow_dir" "$marker" 5; then
            flow_stop
            result_print FAIL "$name" "daemon log lacks the ring evidence: $marker"
            return
        fi
    done
    if grep -qF '[hal] stub: line 0 strm 0 start' "$flow_dir/daemon.log"; then
        flow_stop
        result_print FAIL "$name" "media started on a call that was never answered"
        return
    fi
    if (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "SIPp exited with status $process_status"
        return
    fi
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "remote CANCEL did not return daemon to IDLE"
        return
    fi
    if ! stub_wait "$flow_dir" '.line[0].ringing == false and .line[0].stream[0].running == false' 3 stopped; then
        flow_stop
        result_print FAIL "$name" "ring remained active or media started"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    # The call was never answered, so no stream and no session description of
    # the daemon's own exist to read: the family lives in the REGISTER headers,
    # the status and the capture.
    family_check "$flow_dir" received "REGISTER " "$daemon_port" signal \
        "$flow_dir/register" || family_status=$?
    if (( family_status != 0 )); then
        flow_stop
        result_print "$(family_verdict "$family_status")" "$name" "$family_failure_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, 180, CANCEL, 200, 487, ACK, and remote-cancel state passed$family_detail"
}

# The subscriber of the answered calls of inbound-churn. The scenario answers
# the odd call numbers and cancels the even ones, and only the hook can answer
# a call, so this driver runs beside SIPp for the length of the flow. It seizes
# the line of an odd call, holds it for the talk time and puts it back.
#
# The release is what ends the call. The daemon sends the BYE behind it, the
# scenario answers that BYE, and the call ends at SIPp only then, so the line
# is on hook before the next offer can exist. A driver that released behind a
# BYE of the caller would race that offer instead, and a line that is still off
# hook answers an offer 486.
#
# It counts the rings in the daemon log instead of the calls on the wire. The
# daemon starts one ring per offered call whether this loop was quick or not,
# so a late read costs the seize of one call and never moves the count of the
# calls behind it. A count that jumps by more than one is a ring this loop
# missed whole: the driver stops there and names it, because a seize against
# the call behind it would answer a call the scenario cancels.
#
# It writes what it did to hook-driver.txt, which is the record the flow reads
# back.
inbound_churn_hook_drive()
{
    local flow_dir=$1
    local calls=$2
    # The talk time of an answered call. It paces the answered calls too, for
    # the reason the ring pause of the scenario states.
    local talk_s=0.6
    local ring_line='[hal] stub: line 0 ring start'
    local record=$flow_dir/hook-driver.txt
    local deadline=$((SECONDS + 300))
    local seen=0
    local rings

    : >"$record"
    while (( seen < calls )) && (( SECONDS < deadline )); do
        rings=$(grep -cF "$ring_line" "$flow_dir/daemon.log" 2>/dev/null) || rings=0
        if (( rings <= seen )); then
            sleep 0.05
            continue
        fi
        if (( rings > seen + 1 )); then
            printf 'lost ring %u, the daemon already started %u\n' \
                "$((seen + 1))" "$rings" >>"$record"
            return 1
        fi
        seen=$rings
        if (( seen % 2 == 0 )); then
            continue
        fi
        hook_set seize || true
        if ! status_wait "$flow_dir" CONNECTED 5 churn-answered; then
            printf 'ring %u did not reach CONNECTED\n' "$seen" >>"$record"
            hook_set release || true
            return 1
        fi
        sleep "$talk_s"
        hook_set release || true
        # The release ends the call, so the leg leaves CONNECTED behind it.
        # The wait keeps the ring count and the line state in step: the next
        # offer arrives against a line this loop has already let go.
        if ! status_expr_wait "$flow_dir" '.call[0].state != "CONNECTED"' 15 churn-cleared; then
            printf 'ring %u stayed connected behind the release\n' "$seen" >>"$record"
            return 1
        fi
        printf 'ring %u answered and released\n' "$seen" >>"$record"
    done
}

# A field inbound sequence at the real daemon: fifty offered calls,
# the odd ones answered and the even ones cancelled after their 180, with the
# registration refreshing between them. The field device answered 100 and left
# the caller without a 180 for 6.4 s, so what this flow adds to uac_cancel is
# the bound and the repetition: every call of the run draws its 180 inside one
# second, and the line takes a new offer after each ending.
inbound_churn_run()
{
    local name=inbound-churn
    local calls=50
    # The line holds a ten-second binding. register_prime grants the first
    # one, and register_ooc.xml answers each refresh behind it with the same
    # expiry (-key expires 10).
    local refresh_line='[sip] line 0 registration refreshed, expires 10 s'
    local refreshes=0
    local answered=0
    local refused=0
    local timing_status=0

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    config_write "$config_dir" "$sipp_port" "$daemon_port" || true
    # still part of the channel section config_write closed with.
    #
    # The answered half needs it, and it is the first limit of the run. An
    # answered call keeps its 2xx record until 64*T1, because a retransmitted
    # ACK must not reach the call layer twice (RFC 6026, src/proto/sip/ua.c
    # accept_ack), and a line holds eight such records (src/proto/sip/ua.h
    # UA_ACCEPT_MAX). At the shipped T1 of 500 ms the ninth answered call of
    # this run finds none, and the daemon ends that call with a BYE of its own.
    # T1 of 100 ms holds the record for 6.4 s. Two answered calls stand more
    # than 1.3 s apart: the 600 ms talk time of the hook driver, then the
    # 700 ms ring pause of the cancelled call between them. So at most five of
    # the eight records are live at once. The 180 under test is sent when the
    # offer arrives and no timer of this set paces it.
    #
    # The second limit is the transaction table, and the pauses of the scenario
    # hold the run inside it: peak 49 of the 64 entries (src/proto/sip/txn.h
    # SIP_TXN_MAX) at this pace.
    printf "\toption sip_timer_t1 '%u'\n" 100 >>"$config_dir/voip"
    capture_start "$flow_dir" "$sipp_port" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name" 1 10; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    inbound_churn_hook_drive "$flow_dir" "$calls" &
    current_hook_pid=$!
    # One call at a time, through the call limit: the config writes one
    # subscriber, so a second offer inside the ring of the first earns a busy
    # answer and not the field sequence. The request URI names that subscriber,
    # the way the field message names the SIP user name of the line.
    #
    # A refresh REGISTER carries the Call-ID of the binding it renews (RFC 3261
    # section 10.2), and SIPp remembers a finished call for 33 seconds and
    # discards every message that names one. The refresh would reach that dead
    # call instead of the out-of-call scenario and go unanswered, so this run
    # remembers none: each refresh opens an instance of its own and is answered.
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/uac_inbound_churn.xml" \
        "$sipp_port" "$daemon_port" "$media_port" \
        "$script_dir/register_ooc.xml" "$calls" 1 5551000 \
        -deadcall_wait 0 -key expires 10; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! sipp_finish_wait 120; then
        flow_stop
        result_print FAIL "$name" "SIPp did not place the $calls calls"
        return
    fi
    process_stop "$current_hook_pid"
    current_hook_pid=
    # The two readings of the subscriber side, taken before the SIPp status is
    # read. A SIPp that exits 1 exits on the first call that broke, and the
    # cause stands here: half the calls are answered ones and each one owes a
    # seize and a release, and an offer that met a line still off hook was
    # refused 486. Without these two numbers the failure line names the exit
    # status alone.
    answered=$(grep -c 'answered and released' "$flow_dir/hook-driver.txt" 2>/dev/null) ||
        answered=0
    refused=$(grep -c 'rejected with 486' "$flow_dir/daemon.log" 2>/dev/null) || refused=0
    if (( answered != calls / 2 || refused != 0 )); then
        flow_stop
        result_print FAIL "$name" \
            "the hook answered $answered of $((calls / 2)) odd calls and $refused calls were refused 486: $(tail -1 "$flow_dir/hook-driver.txt")"
        return
    fi
    if (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" \
            "SIPp exited with status $process_status, the hook answered $answered of $((calls / 2)) odd calls and $refused calls were refused 486"
        return
    fi
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "the daemon did not return to IDLE after the last cancelled call"
        return
    fi
    if ! stub_check "$flow_dir" '.line[0].ringing == false' ring; then
        flow_stop
        result_print FAIL "$name" "the line still rings after the last cancelled call"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" "$calls"; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    # The refresh traffic of the field capture, which ran on the socket the
    # calls used. A run that placed its calls without one would say nothing
    # about the field sequence. The refresh stands five seconds in.
    # sip_reg_refresh_secs() takes the lesser of half the expiry and the
    # expiry minus thirty seconds, with a fifteen second floor. If that floor
    # is not below the expiry, it takes half the expiry with a two second floor
    # instead. This binding grants ten, so half is the answer.
    if ! daemon_log_require "$flow_dir" "$refresh_line" 5; then
        flow_stop
        result_print FAIL "$name" "the daemon log holds no registration refresh of the run"
        return
    fi
    refreshes=$(grep -cF "$refresh_line" "$flow_dir/daemon.log")
    capture_finish
    timing_180_check "$flow_dir" "$calls" || timing_status=$?
    if (( timing_status == 2 )); then
        flow_stop
        result_print PARTIAL "$name" "$sipp_verdict_detail, packet timing capture unavailable"
        return
    fi
    if (( timing_status == 3 )); then
        flow_stop
        result_print FAIL "$name" "the capture holds no offered call at all"
        return
    fi
    if (( timing_status == 4 )); then
        flow_stop
        result_print FAIL "$name" \
            "the capture is short of the run: $(cat "$flow_dir/timing-180-result.txt")"
        return
    fi
    if (( timing_status == 5 )); then
        flow_stop
        result_print FAIL "$name" \
            "a call drew no 180 at all: $(cat "$flow_dir/timing-180-result.txt")"
        return
    fi
    if (( timing_status != 0 )); then
        flow_stop
        result_print FAIL "$name" \
            "a call did not draw its 180 inside one second: $(cat "$flow_dir/timing-180-result.txt")"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, $(cat "$flow_dir/timing-180-result.txt"), $answered answered calls, $refreshes registration refreshes, and the line idle behind the last cancelled call"
}

# The daemon as the re-INVITE UAC inside a dialog SIPp created: the hold and
# the resume both owe an ACK, and the scenario fails the flow when one is
# absent. The flow pins the ACK for the daemon's own re-INVITE 2xx inside a
# peer-created dialog.
uac_reinvite_run()
{
    local name=uac_reinvite_hold

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    config_write "$config_dir" "$sipp_port" "$daemon_port" || true
    capture_start "$flow_dir" "$sipp_port" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/$name.xml" "$sipp_port" "$daemon_port" "$media_port"; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! status_wait "$flow_dir" RINGING 10 ringing; then
        flow_stop
        result_print FAIL "$name" "daemon did not reach RINGING"
        return
    fi
    hook_set seize || true
    if ! status_wait "$flow_dir" CONNECTED 10 connected; then
        flow_stop
        result_print FAIL "$name" "daemon did not answer into CONNECTED"
        return
    fi
    if ! media_port_wait "$flow_dir" 5; then
        flow_stop
        result_print FAIL "$name" "stream 0 did not use the SDP media port"
        return
    fi
    flash_send || true
    if ! status_wait "$flow_dir" HOLD_DIAL 15 hold; then
        flow_stop
        result_print FAIL "$name" "the hold re-INVITE did not complete"
        return
    fi
    # The hold silence on the programmed stream. a=sendonly says who may send
    # and not what: the flag beside it is what stops the backend encoding the
    # room. It is read off the stream the daemon really programmed, which is
    # the only place the wire cannot show it.
    if ! stub_wait "$flow_dir" ".line[0].stream[0].tx_silence == true" 5 held_silent; then
        flow_stop
        result_print FAIL "$name" "the held stream was not silenced"
        return
    fi
    flash_send || true
    if ! status_wait "$flow_dir" CONNECTED 15 resumed; then
        flow_stop
        result_print FAIL "$name" "the resume re-INVITE did not complete"
        return
    fi
    if ! stub_wait "$flow_dir" ".line[0].stream[0].tx_silence == false" 5 resumed_live; then
        flow_stop
        result_print FAIL "$name" "the resumed stream was left silenced"
        return
    fi
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not finish the re-INVITE and BYE exchanges"
        return
    fi
    if (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "SIPp exited with status $process_status"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "daemon did not return to IDLE after release"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, both DUT re-INVITEs answered and acknowledged, the held stream silenced and the resumed one live"
}

# A flash that dials nothing: the recall dial tone runs out into reorder and
# the call stays held. flash_service_timer_sec is written short, so a resume
# that the option still timed would leave long before the reorder. The
# scenario is the uac_reinvite_hold one, whose resume wait outlasts the
# window of 3 s.
hold_dial_abandon_start()
{
    local name=$1
    local invites
    local held_ns
    local waited_ms

    abandon_failure_detail=
    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        abandon_failure_detail="artifact directory creation failed"
        return 1
    }
    config_dir=$flow_dir/config
    config_write "$config_dir" "$sipp_port" "$daemon_port" "" 1 || true
    printf "\toption start_digit_timer '%u'\n" 3 >>"$config_dir/voip"
    capture_start "$flow_dir" "$sipp_port" || true
    register_prime "$flow_dir" "$config_dir" "$name" || {
        abandon_failure_detail="registration setup failed"
        return 1
    }
    sipp_uac_start "$flow_dir" "$sipp_dir/uac_reinvite_hold.xml" "$sipp_port" \
        "$daemon_port" "$media_port" || {
        abandon_failure_detail="SIPp startup failed"
        return 1
    }
    status_wait "$flow_dir" RINGING 10 ringing || {
        abandon_failure_detail="daemon did not reach RINGING"
        return 1
    }
    hook_set seize || true
    status_wait "$flow_dir" CONNECTED 10 connected || {
        abandon_failure_detail="daemon did not answer into CONNECTED"
        return 1
    }
    media_port_wait "$flow_dir" 5 || {
        abandon_failure_detail="stream 0 did not use the SDP media port"
        return 1
    }
    flash_send || true
    status_wait "$flow_dir" HOLD_DIAL 15 hold || {
        abandon_failure_detail="the hold re-INVITE did not complete"
        return 1
    }
    held_ns=$(date +%s%N)
    # SIPp sent the initial INVITE itself, so every INVITE it received is a
    # re-INVITE of the DUT
    sipp_msg_wait "$flow_dir" received "INVITE " 1 5 || {
        abandon_failure_detail="SIPp logged no hold re-INVITE"
        return 1
    }
    invites=$(sipp_msg_count "$flow_dir" received "INVITE ")
    status_expr_wait "$flow_dir" \
        '.call[0].state == "CLEARING" and .call[0].clearing == "fastbusy"' \
        10 reorder || {
        abandon_failure_detail="the unused recall dial tone did not end in reorder"
        return 1
    }
    # the configured window is 3 s from the hold; each status poll adds up
    # to 0.1 s, so a second either way still tells this window from another
    waited_ms=$(( ($(date +%s%N) - held_ns) / 1000000 ))
    (( waited_ms >= 2000 && waited_ms <= 5000 )) || {
        abandon_failure_detail="the reorder came $waited_ms ms after the hold, not at the start_digit_timer of 3 s"
        return 1
    }
    (( $(sipp_msg_count "$flow_dir" received "INVITE ") == invites )) || {
        abandon_failure_detail="a resume re-INVITE left with no flash"
        return 1
    }
    stub_check "$flow_dir" ".line[0].stream[0].tx_silence == true" reorder_held || {
        abandon_failure_detail="the held stream came back during the reorder"
        return 1
    }
}

hold_dial_abandon_run()
{
    local name=hold-dial-abandon

    if ! hold_dial_abandon_start "$name"; then
        flow_stop
        result_print FAIL "$name" "$abandon_failure_detail"
        return
    fi
    flash_send || true
    if ! status_wait "$flow_dir" CONNECTED 15 resumed; then
        flow_stop
        result_print FAIL "$name" "the flash in the reorder did not resume the held call"
        return
    fi
    if ! stub_wait "$flow_dir" ".line[0].stream[0].tx_silence == false" 5 resumed_live; then
        flow_stop
        result_print FAIL "$name" "the resumed stream was left silenced"
        return
    fi
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not finish the re-INVITE and BYE exchanges"
        return
    fi
    if (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "SIPp exited with status $process_status"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "daemon did not return to IDLE after release"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, the call stayed held into the reorder with no resume re-INVITE, and the flash in the reorder resumed it"
}

hold_dial_abandon_onhook_run()
{
    local name=hold-dial-abandon-onhook

    if ! hold_dial_abandon_start "$name"; then
        flow_stop
        result_print FAIL "$name" "$abandon_failure_detail"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" HOLDING 5 parked; then
        flow_stop
        result_print FAIL "$name" "the on-hook in the reorder did not park the held call"
        return
    fi
    if ! stub_wait "$flow_dir" ".line[0].ringing == true" 5 recall; then
        flow_stop
        result_print FAIL "$name" "the parked call did not ring the subscriber back"
        return
    fi
    hook_set seize || true
    if ! status_wait "$flow_dir" CONNECTED 15 resumed; then
        flow_stop
        result_print FAIL "$name" "the lift did not resume the held call"
        return
    fi
    if ! stub_wait "$flow_dir" ".line[0].ringing == false and .line[0].stream[0].tx_silence == false" 5 resumed_live; then
        flow_stop
        result_print FAIL "$name" "the bell kept ringing or the resumed stream was left silenced"
        return
    fi
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not finish the re-INVITE and BYE exchanges"
        return
    fi
    if (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "SIPp exited with status $process_status"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "daemon did not return to IDLE after release"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, the on-hook in the reorder parked the held call with the recall ring, and the lift resumed it"
}

retry_503_run()
{
    local name=uas_503_retry_after
    local timing_status=0

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    config_write "$config_dir" "$sipp_port" "$daemon_port" || true
    capture_start "$flow_dir" "$sipp_port" || true
    if ! sipp_start "$flow_dir" "$sipp_dir/$name.xml" "$sipp_port" "$daemon_port" "$media_port" 1; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! daemon_start "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    if ! sipp_finish_wait 25; then
        flow_stop
        result_print FAIL "$name" "SIPp did not receive the retry"
        return
    fi
    if (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "SIPp exited with status $process_status"
        return
    fi
    if ! status_wait "$flow_dir" IDLE 3 idle; then
        flow_stop
        result_print FAIL "$name" "call state was not IDLE"
        return
    fi
    status_save "$flow_dir" registered
    if ! jq -e '.lines[0].state == "registered" and .lines[0].last_code == 200' \
        "$flow_dir/status-${snapshot_index}-registered.json" >/dev/null; then
        flow_stop
        result_print FAIL "$name" "registration did not reach a 200 registered state"
        return
    fi
    capture_finish
    timing_503_check "$flow_dir" || timing_status=$?
    if (( timing_status == 2 )); then
        flow_stop
        result_print PARTIAL "$name" "message flow passed, packet timing capture unavailable"
        return
    fi
    if (( timing_status == 3 )); then
        flow_stop
        result_print FAIL "$name" "the capture holds no REGISTER retry after the 503"
        return
    fi
    if (( timing_status != 0 )); then
        flow_stop
        result_print FAIL "$name" \
            "REGISTER retry delay was less than five seconds: $(cat "$flow_dir/timing-503-result.txt")"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "committed retry scenario, registration state, and $(cat "$flow_dir/timing-503-result.txt") passed"
}

# The out_attempted counter of line 0. A call_dial() that the account refuses
# opens no call record, so the counter holds still (src/cdr.c).
out_attempted_get()
{
    ubus -s "$current_socket" call voice stats '{"line":0}' 2>/dev/null |
        jq -r '.stats[0].out_attempted // empty'
}

# The line placed no call: the counter did not move, and the daemon log holds
# no INVITE client transaction. The capture is not read, because
# capture_start can fail and leave none.
reg_no_call_check()
{
    local flow_dir=$1
    local attempted=$2

    [[ -n $attempted && $(out_attempted_get) == "$attempted" ]] || return 1
    ! grep -q 'ict/INVITE' "$flow_dir/daemon.log"
}

# The bad password case. The
# registrar challenges the REGISTER and refuses the credential with 403, so
# the line holds no binding. Its seize plays reorder, and the digits dialed
# over the reorder place no call.
reg_403_run()
{
    local name=reg-403-reorder
    local attempted
    local timing_status=0

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    config_write "$config_dir" "$sipp_port" "$daemon_port" || true
    capture_start "$flow_dir" "$sipp_port" || true
    if ! sipp_start "$flow_dir" "$sipp_dir/uas_register_403.xml" "$sipp_port" \
        "$daemon_port" "$media_port" 1; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! daemon_start "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    if ! status_expr_wait "$flow_dir" \
        '.lines[0].status == "failed" and .lines[0].last_code == 403' 20 refused; then
        flow_stop
        result_print FAIL "$name" "the 403 of the registrar did not fail the registration"
        return
    fi
    if ! sipp_finish_wait 10 || (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "SIPp did not complete the 401 and 403 exchange"
        return
    fi
    attempted=$(out_attempted_get)
    hook_set seize || true
    if ! status_wait "$flow_dir" CLEARING 5 clearing ||
        ! stub_wait "$flow_dir" '.line[0].tone == 6' 3 reorder; then
        flow_stop
        result_print FAIL "$name" "the seize of a line refused with 403 did not play reorder"
        return
    fi
    digit_send || true
    if ! status_expr_wait "$flow_dir" \
        '.call[0].state == "CLEARING" and .lines[0].status == "failed"' 3 digits ||
        ! reg_no_call_check "$flow_dir" "$attempted"; then
        flow_stop
        result_print FAIL "$name" "the digits dialed over the reorder placed a call"
        return
    fi
    # The flash reads the answer of the seize, so it opens no dial tone over
    # the reorder. A single reading, because
    # a wrong flash would pass a first sample and change the state later.
    flash_send || true
    sleep 0.5
    if ! stub_check "$flow_dir" '.line[0].tone == 6' flash-tone ||
        ! status_expr_check "$flow_dir" '.call[0].state == "CLEARING"' flash; then
        flow_stop
        result_print FAIL "$name" "a flash over the reorder gave the line a dial tone"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "the release did not return the line to IDLE"
        return
    fi
    # A new call to the line gets 480 and never rings. The caller answers a
    # REGISTER retry with 403 too, so the line stays without a binding while
    # the call runs.
    mkdir -p "$flow_dir/inbound" || true
    if ! sipp_uac_start "$flow_dir/inbound" "$sipp_dir/uac_refused_480.xml" \
        "$sipp_port" "$daemon_port" "$media_port" \
        "$script_dir/register_ooc_403.xml" 1 1 5551000; then
        flow_stop
        result_print FAIL "$name" "the calling SIPp did not start"
        return
    fi
    if ! sipp_finish_wait 20 || (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "the new call was not refused with 480"
        return
    fi
    if ! stub_check "$flow_dir" '.line[0].ringing == false' inbound-ring ||
        ! status_wait "$flow_dir" IDLE 3 inbound-idle; then
        flow_stop
        result_print FAIL "$name" "the refused call rang the line"
        return
    fi
    # The credential wait is 30 s: the retry REGISTER arrives that long after
    # the first 403, and this registrar refuses it again.
    mkdir -p "$flow_dir/retry" || true
    if ! sipp_start "$flow_dir/retry" "$sipp_dir/uas_register_403.xml" \
        "$sipp_port" "$daemon_port" "$media_port" 1 ||
        ! sipp_finish_wait 40 || (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "the credential retry did not arrive inside 40 s"
        return
    fi
    if ! status_expr_wait "$flow_dir" \
        '.lines[0].status == "failed" and .lines[0].last_code == 403' 5 refused-again; then
        flow_stop
        result_print FAIL "$name" "the refused retry left the line registered"
        return
    fi
    capture_finish
    timing_403_check "$flow_dir" || timing_status=$?
    if (( timing_status == 2 )); then
        flow_stop
        result_print PARTIAL "$name" "message flow passed, packet timing capture unavailable"
        return
    fi
    if (( timing_status != 0 )); then
        flow_stop
        result_print FAIL "$name" \
            "the credential retry left outside 29.5 to 33 s: $(cat "$flow_dir/timing-403-result.txt")"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "the 403 failed the registration, the seize played reorder, the digits over it placed no call, a new call got 480 with no ring, and $(cat "$flow_dir/timing-403-result.txt")"
}

# The server down case. Nothing
# answers the REGISTER. The literal proxy gives the line a target, which is
# the case where a target test alone gave dial tone. The seize plays reorder
# while the REGISTER is in flight and again after its timeout, and the dial
# verb places no call. Timer F is 8 s and the retry 60 s, so each state lasts
# long enough to read.
reg_none_run()
{
    local name=reg-none-reorder
    local attempted

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    config_write "$config_dir" "$sipp_port" "$daemon_port" || true
    {
        printf "\toption sip_timer_f '%u'\n" 8000
        printf "\toption sip_register_retry '%u'\n" 60
    } >>"$config_dir/voip"
    capture_start "$flow_dir" "$sipp_port" || true
    if ! daemon_start "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    if ! status_expr_wait "$flow_dir" \
        '.lines[0].state == "trying" and .lines[0].targets >= 1' 5 trying; then
        flow_stop
        result_print FAIL "$name" "the line did not send a REGISTER to its target"
        return
    fi
    attempted=$(out_attempted_get)
    hook_set seize || true
    if ! status_wait "$flow_dir" CLEARING 5 clearing-trying ||
        ! stub_wait "$flow_dir" '.line[0].tone == 6' 3 reorder-trying; then
        flow_stop
        result_print FAIL "$name" "the seize during the first REGISTER did not play reorder"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" IDLE 5 idle-trying ||
        ! status_expr_wait "$flow_dir" \
            '.lines[0].state == "backoff" and .lines[0].last_code == 408' 15 backoff; then
        flow_stop
        result_print FAIL "$name" "the unanswered REGISTER did not time out into backoff"
        return
    fi
    hook_set seize || true
    if ! status_wait "$flow_dir" CLEARING 5 clearing-backoff ||
        ! stub_wait "$flow_dir" '.line[0].tone == 6' 3 reorder-backoff; then
        flow_stop
        result_print FAIL "$name" "the seize after the timeout did not play reorder"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" IDLE 5 idle-backoff; then
        flow_stop
        result_print FAIL "$name" "the release did not return the line to IDLE"
        return
    fi
    verb_send dial '{"line":0,"number":"123#"}' || true
    if ! status_wait "$flow_dir" CLEARING 5 clearing-dial ||
        ! reg_no_call_check "$flow_dir" "$attempted"; then
        flow_stop
        result_print FAIL "$name" "the dial verb placed a call with no live registration"
        return
    fi
    verb_send release '{"line":0}' || true
    flow_stop
    result_print PASS "$name" \
        "the seize played reorder in trying and in backoff, and the dial verb placed no call"
}

# The message waiting intake. The switch registers the DUT and then sends the
# summary as an unsolicited NOTIFY, which is the path every shipped switch
# uses. The scenario itself asserts both 200s; this flow asserts that the state
# reached the subscriber's line.
mwi_notify_run()
{
    local name=uas_mwi_notify
    local family_status=0

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    config_write "$config_dir" "$sipp_port" "$daemon_port" || true
    capture_start "$flow_dir" "$sipp_port" || true
    if ! sipp_start "$flow_dir" "$sipp_dir/$name.xml" "$sipp_port" "$daemon_port" \
        "$media_port" 1; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! daemon_start "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    # The clear follows the set at once, so the state the first NOTIFY set
    # lives for microseconds and no ubus poll can catch it: the daemon trace
    # keeps it and carries the same three facts a status expression would
    # assert, which is the shape the CANCEL flow already uses for its ring.
    if ! daemon_log_require "$flow_dir" \
        '[fsm] line 0 message waiting 1 new 2 old 1' 15; then
        flow_stop
        result_print FAIL "$name" "the unsolicited NOTIFY did not reach the line state"
        return
    fi
    if ! status_expr_wait "$flow_dir" \
        '.call[0].mwi == false and .call[0].mwi_new == 0 and .call[0].mwi_old == 0' \
        20 mwi-clear; then
        flow_stop
        result_print FAIL "$name" "the second NOTIFY did not clear the line state"
        return
    fi
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not finish the NOTIFY exchange"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    status_save "$flow_dir" registered
    if ! jq -e '.lines[0].state == "registered" and
            (.lines[0] | has("mwi_subscription") | not)' \
        "$flow_dir/status-${snapshot_index}-registered.json" >/dev/null; then
        flow_stop
        result_print FAIL "$name" "the line did not stay registered, or a subscription key appeared on an unsolicited-only line"
        return
    fi
    # A message waiting flow carries no call, so it reads the family off the
    # REGISTER headers, the status and the capture alone.
    family_check "$flow_dir" received "REGISTER " "$daemon_port" signal || family_status=$?
    if (( family_status != 0 )); then
        flow_stop
        result_print "$(family_verdict "$family_status")" "$name" "$family_failure_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, both NOTIFYs answered 200, the unknown package answered 489, and the line state followed$family_detail"
}

# Local call forwarding on the unconditional condition. The config arms it, so
# no star code has to be dialled: the switch registers the DUT, offers it a
# call and requires the 302 that carries the target. The scenario itself
# aborts on a 180, so "the line never rings" is asserted on the wire as well
# as in the daemon state below.
cfu_redirect_run()
{
    local name=uas_cfu_redirect

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    config_write "$config_dir" "$sipp_port" "$daemon_port" "" "" 1 5552000 || true
    capture_start "$flow_dir" "$sipp_port" || true
    # The daemon comes up first here, and its readiness gate replaces a pause
    # in the scenario. This scenario offers its call right after the REGISTER
    # it answers, and the registration completes well inside the DSP bring-up,
    # so a SIPp that is already listening has its INVITE answered 503: a new
    # INVITE before the DSP is ready is refused with a Retry-After.
    # daemon_start waits for hal.state "ready" over ubus, and the daemon
    # retransmits the REGISTER this scenario answers.
    if ! daemon_start "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    if ! sipp_start "$flow_dir" "$sipp_dir/$name.xml" "$sipp_port" "$daemon_port" \
        "$media_port" 1; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! status_expr_wait "$flow_dir" '.call[0].cf_uncond == "5552000"' 10 armed; then
        flow_stop
        result_print FAIL "$name" "the config did not arm the unconditional forward"
        return
    fi
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not receive the redirect"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "the line did not stay idle through the redirect"
        return
    fi
    if ! stub_wait "$flow_dir" '.line[0].ringing == false' 3 not-ringing; then
        flow_stop
        result_print FAIL "$name" "the line rang for a call it was forwarding"
        return
    fi
    status_save "$flow_dir" registered
    if ! jq -e '.lines[0].state == "registered"' \
        "$flow_dir/status-${snapshot_index}-registered.json" >/dev/null; then
        flow_stop
        result_print FAIL "$name" "the line did not stay registered"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, the 302 carried the configured target and the line never rang"
}

# The credential of the challenged fax upspeed flow. The channel block of the
# plain flow carries none, and an absent secret is an empty password and not a
# refusal to answer (src/proto/sip/account.c acct_cfg_derive(),
# src/proto/sip/call.c cred_write()). The challenged flow names a real one, so
# the digest the DUT puts on the wire is the digest a provider would check.
fax_auth_secret='uvoice-fax-secret'

# The codec sections of the fax flows, appended behind the channel section:
# G.729 first, then PCMU and PCMA. G.729 leads because no fax carrier survives
# it, so a call that commits to it has a codec to leave. With g711 0, G.729
# stands alone.
fax_codecs_append()
{
    local config_dir=$1
    local g711=${2:-1}
    local -a ids=(3)
    local i

    if (( g711 )); then
        ids+=(1 2)
    fi
    for (( i = 0; i < ${#ids[@]}; i++ )); do
        printf "config codec 'codec%u'\n" "$((i + 1))"
        printf "\toption tr104_id '%u'\n" "${ids[i]}"
        printf "\toption priority '%u'\n" "$((i + 1))"
        printf '\toption enabled %s\n' "'1'"
    done >>"$config_dir/voip"
}

# The G.711 fax upspeed. The DUT calls out, this scenario answers with G.729
# alone, and the stub then reports the fax answer tone toward the phone. The
# DUT has to convert the DSP to passthrough and re-INVITE with G.711 alone.
#
# The codec sections are what makes the clamp visible. Without a committed
# codec no fax carrier survives, the upspeed offer would look like the offer the
# call already used and the flow would prove nothing about the clamp.
#
# One runner serves the plain peer and the one that challenges the upspeed.
# name selects the flow and the scenario file of the same name; challenged is 1
# for the peer that answers the first upspeed re-INVITE 401. That peer is the
# field switch, and the challenged arm owes two readings of its own: the
# INVITE requests of the dialog, and the distance from the 401 to the retry.
fax_upspeed_run()
{
    local name=$1
    local challenged=$2
    local sipp_ok=0
    local invites
    local timing_status=0

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! config_write "$config_dir" "$sipp_port" "$daemon_port"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    {
        # still part of the channel section config_write closed with: the
        # telephone-event payload type is the one non-codec entry the clamp
        # must not drop, so the flow negotiates one
        printf '\toption sip_dtmf_mode %s\n' "'rfc2833'"
        if (( challenged )); then
            printf "\toption sip_secret '%s'\n" "$fax_auth_secret"
        fi
    } >>"$config_dir/voip"
    fax_codecs_append "$config_dir"
    capture_start "$flow_dir" "$sipp_port" || true
    if ! sipp_start "$flow_dir" "$sipp_dir/$name.xml" "$sipp_port" "$daemon_port" \
        "$media_port" 2; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! daemon_start "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    if ! outbound_dial_start "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "outbound dial setup failed"
        return
    fi
    if ! status_wait "$flow_dir" CONNECTED 15 connected; then
        flow_stop
        result_print FAIL "$name" "the call did not reach CONNECTED"
        return
    fi
    # HAL_CODEC_G729 is 3: the call is on the codec the fax has to leave
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].running == true and .line[0].stream[0].codec == 3' \
        5 committed-g729; then
        flow_stop
        result_print FAIL "$name" "the call did not commit to G.729"
        return
    fi
    # the direction gate: a tone from the wire belongs to the far end's gateway
    tone_send ced remote || true
    sleep 0.5
    if ! stub_check "$flow_dir" '.line[0].stream[0].fax_applied == false' remote-ced; then
        flow_stop
        result_print FAIL "$name" "a CED from the wire switched the call"
        return
    fi
    tone_send ced local || true
    # HAL_FAX_PASSTHROUGH is 1 and HAL_CODEC_PCMU is 1
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].fax_applied == true and .line[0].stream[0].fax_mode == 1 and .line[0].stream[0].fax_codec == 1 and .line[0].stream[0].fax_ec_disable == true and .line[0].stream[0].fax_vad_disable == true and .line[0].stream[0].fax_cng_disable == true and .line[0].stream[0].fax_silence_comp_disable == true and .line[0].stream[0].fax_jb_fixed == true' \
        5 fax-profile; then
        flow_stop
        result_print FAIL "$name" "the fax profile did not reach the stream"
        return
    fi
    if ! stub_wait "$flow_dir" '.line[0].stream[0].codec == 1' 15 upspeed; then
        flow_stop
        result_print FAIL "$name" "the upspeed did not move the stream to G.711"
        return
    fi
    # The description the upspeed answer programmed carries the kind, which is
    # the whole point: the profile the answer replaced is not what keeps the
    # canceller off the carrier. HAL_PASSTHROUGH_MODEM is 1.
    if ! stub_wait "$flow_dir" '.line[0].stream[0].passthrough == 1' \
        5 passthrough-carried; then
        flow_stop
        result_print FAIL "$name" "the upspeed answer dropped the passthrough kind"
        return
    fi
    # stage 2 on the media side: the preamble says the carrier is a fax, and
    # this channel enables no T.38, so the kind is all the tone can move.
    # HAL_PASSTHROUGH_FAX is 2.
    tone_send v21 local || true
    if ! stub_wait "$flow_dir" '.line[0].stream[0].passthrough == 2' 5 fax-kind; then
        flow_stop
        result_print FAIL "$name" "the V.21 preamble did not move the leg to the fax kind"
        return
    fi
    if sipp_finish_wait 45; then
        if uas_sipp_verdict "$flow_dir" 2; then
            sipp_ok=1
        fi
    else
        sipp_verdict_detail="SIPp did not finish within 45 seconds"
    fi
    hook_set release || true
    status_save "$flow_dir" final
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "the daemon did not return to IDLE"
        return
    fi
    if (( !sipp_ok )); then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    if (( !challenged )); then
        flow_stop
        result_print PASS "$name" \
            "$sipp_verdict_detail, the upspeed offered G.711 alone, the passthrough profile reached the stream and the kind survived the answer"
        return
    fi
    # The offer, the challenged upspeed and the credentialed retry, counted by
    # their CSeq numbers. What this catches is a retry that kept the number of
    # the offer it repeats: the scenario takes that message and passes, and the
    # count reads 2. A fourth INVITE is reported by SIPp instead, which aborts
    # the call on a request it has no receive for, so the verdict above returns
    # before this line.
    invites=$(xfer_request_count "$flow_dir" INVITE CSeq)
    if [[ $invites != 3 ]]; then
        flow_stop
        result_print FAIL "$name" \
            "the peer received $invites INVITE request(s), expected 3"
        return
    fi
    capture_finish
    timing_fax_auth_check "$flow_dir" || timing_status=$?
    if (( timing_status == 2 )); then
        flow_stop
        result_print PARTIAL "$name" \
            "$sipp_verdict_detail, packet timing capture unavailable"
        return
    fi
    if (( timing_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "$(cat "$flow_dir/timing-fax-auth.txt")"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, the challenged upspeed retried with the nonce and the opaque and offered G.711 alone, three INVITE requests by CSeq number, $(cat "$flow_dir/timing-fax-auth.txt")"
}

# The far end's own G.711 fax upspeed, followed. The DUT calls out, this
# scenario answers with G.729 alone, and the stub then reports the fax answer
# tone from the wire, which arms the modem kind and applies no profile. The
# scenario re-INVITEs with its list narrowed to G.711, and the DUT has to
# recognize that pair, convert its own DSP and send no offer of its own.
#
# The codec sections are the same three the outgoing flow uses, for the same
# reason: without a committed codec no fax carrier survives, the follow would
# change no payload and the flow would prove nothing about the conversion.
#
# It runs twice, over one scenario file, because the leg's own codec list is
# the second half of the question. g711 1 is the ordinary channel, which shares
# a codec with the narrowing offer and would answer it either way. g711 0 is
# the channel provisioned with G.729 alone: it shares no codec at all with
# that offer, so the answer selection refuses the exchange the recognizer
# wants to follow unless the follow seats the fallback carrier into the leg's
# list first. One daemon carries one channel configuration, so the two are
# separate flows rather than two legs of one.
far_fax_upspeed_run()
{
    local name=$1
    local g711=$2
    local sipp_ok=0

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! config_write "$config_dir" "$sipp_port" "$daemon_port"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    {
        # still part of the channel section config_write closed with: the
        # telephone-event payload type rides the narrowing offer, so the flow
        # negotiates one
        printf '\toption sip_dtmf_mode %s\n' "'rfc2833'"
    } >>"$config_dir/voip"
    fax_codecs_append "$config_dir" "$g711"
    capture_start "$flow_dir" "$sipp_port" || true
    if ! sipp_start "$flow_dir" "$sipp_dir/uas_far_fax_upspeed.xml" "$sipp_port" \
        "$daemon_port" "$media_port" 2; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! daemon_start "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    if ! outbound_dial_start "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "outbound dial setup failed"
        return
    fi
    if ! status_wait "$flow_dir" CONNECTED 15 connected; then
        flow_stop
        result_print FAIL "$name" "the call did not reach CONNECTED"
        return
    fi
    # HAL_CODEC_G729 is 3: the call is on the codec the fax has to leave
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].running == true and .line[0].stream[0].codec == 3' \
        5 committed-g729; then
        flow_stop
        result_print FAIL "$name" "the call did not commit to G.729"
        return
    fi
    tone_send ced remote || true
    # The scenario's own pause does not synchronize with this runner, so the
    # trace row is what bounds the race: it proves the tone reached the leg
    # ahead of the narrowing offer, and it names the failure if the five
    # second margin is ever eaten.
    if ! daemon_log_require "$flow_dir" \
        "fax tone CED from the wire" 5; then
        flow_stop
        result_print FAIL "$name" "the remote answer tone never reached the line"
        return
    fi
    # The tone arms the modem kind on the stream the call already runs and
    # applies no profile: the profile and the codec belong to the exchange
    # behind it. HAL_PASSTHROUGH_MODEM is 1.
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].passthrough == 1 and .line[0].stream[0].fax_applied == false' \
        5 remote-ced; then
        flow_stop
        result_print FAIL "$name" "the remote CED did not arm the modem kind alone"
        return
    fi
    # HAL_FAX_PASSTHROUGH is 1 and HAL_CODEC_PCMU is 1: the narrowing offer
    # behind the tone is what converts the DSP, on the codec the answer chose
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].fax_applied == true and .line[0].stream[0].fax_mode == 1 and .line[0].stream[0].fax_codec == 1 and .line[0].stream[0].fax_ec_disable == true and .line[0].stream[0].fax_vad_disable == true and .line[0].stream[0].fax_cng_disable == true and .line[0].stream[0].fax_silence_comp_disable == true and .line[0].stream[0].fax_jb_fixed == true' \
        15 follow-profile; then
        flow_stop
        result_print FAIL "$name" "the fax profile did not reach the stream"
        return
    fi
    # HAL_PASSTHROUGH_MODEM is 1: the description the answer programmed carries
    # the kind, so the backend keeps the canceller off the carrier
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].codec == 1 and .line[0].stream[0].passthrough == 1' \
        5 follow-stream; then
        flow_stop
        result_print FAIL "$name" "the followed exchange did not program the carrier"
        return
    fi
    # The northbound view of the same thing, plus the latch: this leg took no
    # local tone, so its fax_switched entry must be false. latch_dump() omits
    # an array that is all false, so an absent array is the same answer.
    if ! status_expr_wait "$flow_dir" \
        '.call[0].calls[0].fax == "passthrough" and .call[0].calls[0].fax_kind == "modem" and (((.call[0].fax_switched // [false])[0] // false) | not)' \
        10 followed; then
        flow_stop
        result_print FAIL "$name" "the status view did not name a followed passthrough"
        return
    fi
    if ! daemon_log_require "$flow_dir" "far-end G.711 upspeed followed" 5; then
        flow_stop
        result_print FAIL "$name" "the daemon never traced the follow"
        return
    fi
    if sipp_finish_wait 45; then
        if uas_sipp_verdict "$flow_dir" 2; then
            sipp_ok=1
        fi
    else
        sipp_verdict_detail="SIPp did not finish within 45 seconds"
    fi
    hook_set release || true
    status_save "$flow_dir" final
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "the daemon did not return to IDLE"
        return
    fi
    if (( !sipp_ok )); then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    flow_stop
    if (( g711 )); then
        result_print PASS "$name" \
            "$sipp_verdict_detail, the remote tone armed the modem kind and no profile, the narrowing re-INVITE behind it converted the DSP, and the DUT sent no offer of its own"
        return
    fi
    result_print PASS "$name" \
        "$sipp_verdict_detail, a channel provisioned with G.729 alone answered the narrowing offer with the fallback G.711 and converted the DSP on it"
}

# The milliseconds between a stamp this runner took and the BYE the scenario
# sent, for the flow below. The scenario's quiet window opens at the ACK and
# the runner reaches the reading some way behind it, so the two stamps are the
# only measure of the window the leg got after the modem kind stood on its
# stream. The mark is written in the format SIPp stamps its message log with,
# and the BYE is the first one SIPp sent, because the retransmission of a lost
# BYE says nothing about the window. A run that crosses midnight between the
# two reads the difference against the next day, the xfer_grace_check rule.
sipp_bye_delay_get()
{
    local flow_dir=$1
    local mark=$2
    local bye

    bye=$(awk '
        /^-+ [0-9]{4}-[0-9]{2}-[0-9]{2} / { stamp = $2 " " $3; next }
        /(UDP|TCP|TLS) message received/ { dir = "received"; head = 1; next }
        /(UDP|TCP|TLS) message sent/ { dir = "sent"; head = 1; next }
        head {
            if ($0 ~ /^[[:space:]]*$/)
                next
            line = $0
            gsub(/\r/, "", line)
            head = 0
            if (dir != "sent" || stamp == "")
                next
            if (index(line, "BYE ") == 1 && bye == "")
                bye = stamp
            next
        }
        END { print bye }
    ' "$flow_dir/sipp-messages.log" 2>/dev/null)
    [[ -n $bye ]] || return 1

    awk -v a="$mark" -v b="$bye" '
        function ms(s,    p, t) {
            split(s, p, " ")
            split(p[2], t, /[:.]/)
            return ((t[1] * 60 + t[2]) * 60 + t[3]) * 1000 + t[4] / 1000
        }
        BEGIN {
            d = ms(b) - ms(a)
            if (d < 0)
                d += 86400000
            printf "%d", d
        }'
}

# The calling side with no upspeed behind the tone at all. The DUT calls out,
# this scenario answers with PCMU and telephone-event, and the stub then
# reports the fax answer tone from the wire. Nothing else arrives: a proxy
# between the two gateways answers the far end's upspeed itself and passes no
# offer through (an Asterisk capture shows it), so the tone is the only
# evidence this side gets.
#
# The DUT has to arm the modem kind on the stream it already runs, keep the
# committed codec, apply no fax profile and send no offer of its own. The kind
# is what takes the adaptive jitter buffer, the silence suppression and the
# canceller off the carrier; the codec belongs to the SIP layer, and on a
# G.711 leg it needs no move (ITU-T V.152 clauses 6, 10.1.1 and 10.3).
#
# The codec sections are the three the upspeed flows provision, in the same
# order: the offer leads with G.729 and the answer selects PCMU, so the
# committed codec is the far end's choice and not the only entry of the list.
far_fax_tone_run()
{
    local name=uas_far_fax_tone_g711
    local sipp_ok=0
    local tone_mark=
    local window_ms=
    local window_s=

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! config_write "$config_dir" "$sipp_port" "$daemon_port"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    {
        # still part of the channel section config_write closed with: the
        # telephone-event payload type rides the answer, so the flow
        # negotiates one
        printf '\toption sip_dtmf_mode %s\n' "'rfc2833'"
    } >>"$config_dir/voip"
    fax_codecs_append "$config_dir"
    capture_start "$flow_dir" "$sipp_port" || true
    if ! sipp_start "$flow_dir" "$sipp_dir/$name.xml" "$sipp_port" \
        "$daemon_port" "$media_port" 2; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! daemon_start "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    if ! outbound_dial_start "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "outbound dial setup failed"
        return
    fi
    if ! status_wait "$flow_dir" CONNECTED 15 connected; then
        flow_stop
        result_print FAIL "$name" "the call did not reach CONNECTED"
        return
    fi
    # HAL_CODEC_PCMU is 1: the answer picked the second entry of the offer,
    # and this is the codec the leg keeps for the rest of the flow
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].running == true and .line[0].stream[0].codec == 1' \
        5 committed-pcmu; then
        flow_stop
        result_print FAIL "$name" "the call did not commit to PCMU"
        return
    fi
    tone_send ced remote || true
    # The trace row bounds the race the scenario's own window leaves: it proves
    # the tone reached the leg, and it names the failure if it never did.
    if ! daemon_log_require "$flow_dir" \
        "fax tone CED from the wire" 5; then
        flow_stop
        result_print FAIL "$name" "the remote answer tone never reached the line"
        return
    fi
    # HAL_PASSTHROUGH_MODEM is 1 and HAL_CODEC_PCMU is 1: the kind lands on the
    # committed codec, and no profile goes with it
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].passthrough == 1 and .line[0].stream[0].codec == 1 and .line[0].stream[0].fax_applied == false' \
        5 tone-kind; then
        flow_stop
        result_print FAIL "$name" "the remote tone did not arm the modem kind on the stream"
        return
    fi
    # The stamp the measured window starts from, in SIPp's own log format
    # (sipp_bye_delay_get). It is taken on the sample above, the one that
    # proves the kind reached the stream, so a runner delayed anywhere before
    # this point shortens the reading and never lengthens it.
    tone_mark=$(date '+%Y-%m-%d %H:%M:%S.%6N')
    # The northbound view of the same leg, plus both latches: this side took no
    # local tone, so neither latch stands. latch_dump() omits an array that is
    # all false, so an absent array is the same answer.
    if ! status_expr_wait "$flow_dir" \
        '.call[0].calls[0].fax == "passthrough" and .call[0].calls[0].fax_kind == "modem" and (((.call[0].fax_switched // [false])[0] // false) | not) and (((.call[0].t38_switched // [false])[0] // false) | not)' \
        10 kind-only; then
        flow_stop
        result_print FAIL "$name" "the status view did not name a kind-only passthrough"
        return
    fi
    # The playout the kind pins, read from the live jitter buffer row of the
    # same reply: a modem carrier needs the fixed buffer the voice call never
    # asks for (hal.h hal_jb_mode, media_fill.c media_fill).
    if ! status_expr_wait "$flow_dir" \
        '([.jb[] | select(.line == 0 and .stream == 0)][0] | .sample == "live" and .mode == "fixed")' \
        5 jb-fixed; then
        flow_stop
        result_print FAIL "$name" "the jitter buffer row did not read fixed"
        return
    fi
    if sipp_finish_wait 45; then
        if uas_sipp_verdict "$flow_dir" 2; then
            sipp_ok=1
        fi
    else
        sipp_verdict_detail="SIPp did not finish within 45 seconds"
    fi
    hook_set release || true
    status_save "$flow_dir" final
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "the daemon did not return to IDLE"
        return
    fi
    if (( !sipp_ok )); then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    # The quiet the leg actually got behind the confirmed kind. Without it the
    # flow would pass on a window the bring-up had already eaten, and the claim
    # that the DUT sends no offer would rest on a second or two of silence.
    window_ms=$(sipp_bye_delay_get "$flow_dir" "$tone_mark") || window_ms=
    if [[ -z $window_ms ]]; then
        flow_stop
        result_print FAIL "$name" \
            "the scenario BYE is missing from the message log, so the quiet window has no reading"
        return
    fi
    window_s=$(awk -v ms="$window_ms" 'BEGIN { printf "%.1f", ms / 1000 }')
    if (( window_ms < 8000 )); then
        flow_stop
        result_print FAIL "$name" \
            "the quiet window behind the modem kind measured $window_s seconds, under the eight this flow requires"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, the remote tone armed the modem kind on the committed PCMU with no profile, the jitter buffer went fixed and the DUT sent no offer of its own through $window_s seconds of quiet behind the kind"
}

# The answering side whose answer tone was a false one. The DUT calls out, this
# scenario answers with G.729 alone, the runner injects a CED toward the phone
# and the DUT converts: the profile, the clamp and the upspeed re-INVITE.
#
# The runner then injects the end row of that tone with a 900 ms reading, which
# is 1.7 s under the 2.6 s floor a real answer tone keeps (ITU-T T.30 4.1.1).
# The DUT waits out CALL_FAX_EXIT_DEFER_MS and returns the leg to voice: the
# kind leaves the stream, the status drops the fax word, and the line's tone
# disabler is released once.
#
# The second CED is what proves the exit is a full one. The stage 1 latch is
# per leg and is spent on the first tone, so a leg that returned to voice
# without re-arming it would never convert again on the call it is carrying.
fax_upspeed_false_run()
{
    local name=uas_fax_upspeed_false
    local sipp_ok=0

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! config_write "$config_dir" "$sipp_port" "$daemon_port"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    {
        # still part of the channel section config_write closed with: the
        # codec sections are the three the upspeed flow provisions, so the
        # committed codec is one no fax carrier survives
        printf '\toption sip_dtmf_mode %s\n' "'rfc2833'"
    } >>"$config_dir/voip"
    fax_codecs_append "$config_dir"
    capture_start "$flow_dir" "$sipp_port" || true
    if ! sipp_start "$flow_dir" "$sipp_dir/$name.xml" "$sipp_port" "$daemon_port" \
        "$media_port" 2; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! daemon_start "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    if ! outbound_dial_start "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "outbound dial setup failed"
        return
    fi
    if ! status_wait "$flow_dir" CONNECTED 15 connected; then
        flow_stop
        result_print FAIL "$name" "the call did not reach CONNECTED"
        return
    fi
    # HAL_CODEC_G729 is 3: the call is on the codec the fax has to leave
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].running == true and .line[0].stream[0].codec == 3' \
        5 committed-g729; then
        flow_stop
        result_print FAIL "$name" "the call did not commit to G.729"
        return
    fi
    tone_send ced local || true
    # HAL_CODEC_PCMU is 1 and HAL_PASSTHROUGH_MODEM is 1: the upspeed answer
    # moved the stream and carried the kind the profile armed
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].codec == 1 and .line[0].stream[0].passthrough == 1 and .line[0].stream[0].fax_applied == true' \
        15 first-upspeed; then
        flow_stop
        result_print FAIL "$name" "the first upspeed did not convert the stream"
        return
    fi
    if ! status_expr_wait "$flow_dir" \
        '.call[0].calls[0].fax == "passthrough" and .call[0].calls[0].fax_kind == "modem"' \
        5 converted; then
        flow_stop
        result_print FAIL "$name" "the status view did not name the passthrough"
        return
    fi
    # the end row of the same tone, 1.7 s under the floor of ITU-T T.30 4.1.1
    tone_send ced local 1 900 || true
    # the deferral is 1.5 s, and the wait covers it with the poll behind it
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].passthrough == 0 and .line[0].latch_clears == 1' \
        10 returned; then
        flow_stop
        result_print FAIL "$name" "the short answer tone did not return the leg to voice"
        return
    fi
    if ! status_expr_wait "$flow_dir" \
        '(.call[0].calls[0] | has("fax") | not) and (((.call[0].fax_switched // [false])[0] // false) | not)' \
        5 voice-again; then
        flow_stop
        result_print FAIL "$name" "the status view still names a fax on that leg"
        return
    fi
    # the second, genuine answer tone: the latch re-armed, so this one converts
    tone_send ced local || true
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].codec == 1 and .line[0].stream[0].passthrough == 1' \
        15 second-upspeed; then
        flow_stop
        result_print FAIL "$name" "the second answer tone did not convert the leg again"
        return
    fi
    if sipp_finish_wait 45; then
        if uas_sipp_verdict "$flow_dir" 2; then
            sipp_ok=1
        fi
    else
        sipp_verdict_detail="SIPp did not finish within 45 seconds"
    fi
    hook_set release || true
    status_save "$flow_dir" final
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "the daemon did not return to IDLE"
        return
    fi
    if (( !sipp_ok )); then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, the short answer tone took the leg back to voice with one disabler release, and the genuine tone behind it ran a second upspeed"
}

# The calling side of the same rule. The DUT calls out, this scenario answers
# with PCMU and telephone-event and then sends nothing at all, so the answer
# tone from the wire is the only evidence this side gets.
#
# The tone arms the modem kind on the committed codec, the end row reads 900 ms
# and the DUT returns the leg to voice behind its deferral: no kind on the
# stream, the adaptive jitter buffer again, the disabler released once, and no
# in-dialog INVITE at any point. The return itself spends nothing on the wire,
# which is the second half of what this flow proves. The wire is the one of
# uas_far_fax_tone_g711, so both flows run that scenario; its quiet window is
# long enough for the end row and the deferral too.
far_fax_tone_false_run()
{
    local name=uas_far_fax_tone_false
    local sipp_ok=0
    local tone_mark=
    local window_ms=
    local window_s=

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! config_write "$config_dir" "$sipp_port" "$daemon_port"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    {
        # still part of the channel section config_write closed with: the
        # offer leads with G.729 and the answer selects PCMU, so the committed
        # codec is the far end's choice and not the only entry of the list
        printf '\toption sip_dtmf_mode %s\n' "'rfc2833'"
    } >>"$config_dir/voip"
    fax_codecs_append "$config_dir"
    capture_start "$flow_dir" "$sipp_port" || true
    if ! sipp_start "$flow_dir" "$sipp_dir/uas_far_fax_tone_g711.xml" \
        "$sipp_port" "$daemon_port" "$media_port" 2; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! daemon_start "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    if ! outbound_dial_start "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "outbound dial setup failed"
        return
    fi
    if ! status_wait "$flow_dir" CONNECTED 15 connected; then
        flow_stop
        result_print FAIL "$name" "the call did not reach CONNECTED"
        return
    fi
    # HAL_CODEC_PCMU is 1: the answer picked the second entry of the offer
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].running == true and .line[0].stream[0].codec == 1' \
        5 committed-pcmu; then
        flow_stop
        result_print FAIL "$name" "the call did not commit to PCMU"
        return
    fi
    tone_send ced remote || true
    # HAL_PASSTHROUGH_MODEM is 1: the kind lands on the committed codec
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].passthrough == 1 and .line[0].stream[0].codec == 1 and .line[0].stream[0].fax_applied == false' \
        5 tone-kind; then
        flow_stop
        result_print FAIL "$name" "the remote tone did not arm the modem kind on the stream"
        return
    fi
    # The stamp the measured window starts from, taken on the sample that
    # proves the kind reached the stream, so a runner delayed anywhere before
    # this point shortens the reading and never lengthens it.
    tone_mark=$(date '+%Y-%m-%d %H:%M:%S.%6N')
    # the end row of the same tone, 1.7 s under the floor of ITU-T T.30 4.1.1
    tone_send ced remote 1 900 || true
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].passthrough == 0 and .line[0].stream[0].codec == 1 and .line[0].latch_clears == 1' \
        10 returned; then
        flow_stop
        result_print FAIL "$name" "the short answer tone did not return the leg to voice"
        return
    fi
    if ! status_expr_wait "$flow_dir" \
        '(.call[0].calls[0] | has("fax") | not) and (.call[0].calls[0].codec == "PCMU")' \
        5 voice-again; then
        flow_stop
        result_print FAIL "$name" "the status view still names a fax on that leg"
        return
    fi
    # The playout the kind pinned is released with it: a voice call runs the
    # adaptive buffer the negotiation asked for (hal.h hal_jb_mode).
    if ! status_expr_wait "$flow_dir" \
        '([.jb[] | select(.line == 0 and .stream == 0)][0] | .sample == "live" and .mode == "adaptive")' \
        5 jb-adaptive; then
        flow_stop
        result_print FAIL "$name" "the jitter buffer row did not read adaptive again"
        return
    fi
    if sipp_finish_wait 45; then
        if uas_sipp_verdict "$flow_dir" 2; then
            sipp_ok=1
        fi
    else
        sipp_verdict_detail="SIPp did not finish within 45 seconds"
    fi
    hook_set release || true
    status_save "$flow_dir" final
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "the daemon did not return to IDLE"
        return
    fi
    if (( !sipp_ok )); then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    # The quiet the leg actually got behind the confirmed kind. Without it the
    # flow would pass on a window the bring-up had already eaten, and the claim
    # that the DUT sends no offer would rest on a second or two of silence.
    window_ms=$(sipp_bye_delay_get "$flow_dir" "$tone_mark") || window_ms=
    if [[ -z $window_ms ]]; then
        flow_stop
        result_print FAIL "$name" \
            "the scenario BYE is missing from the message log, so the quiet window has no reading"
        return
    fi
    window_s=$(awk -v ms="$window_ms" 'BEGIN { printf "%.1f", ms / 1000 }')
    if (( window_ms < 8000 )); then
        flow_stop
        result_print FAIL "$name" \
            "the quiet window behind the modem kind measured $window_s seconds, under the eight this flow requires"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, the short answer tone took the leg back to voice on the committed PCMU with one disabler release, the jitter buffer went adaptive again and the DUT sent no offer of its own through $window_s seconds of quiet"
}

# The config both T.38 flows run on. fax_t38_enable is the option
# that gates the upgrade on the leg, and the codec sections commit the call to
# G.729: a codec no fax carrier survives, so the upgrade has something to move
# away from and the G.711 clamp of the failover is visible on the wire.
t38_config_write()
{
    local config_dir=$1

    config_write "$config_dir" "$sipp_port" "$daemon_port" || return 1
    {
        # still part of the channel section config_write closed with
        printf '\toption fax_t38_enable %s\n' "'1'"
        printf '\toption sip_dtmf_mode %s\n' "'rfc2833'"
    } >>"$config_dir/voip" || return 1
    fax_codecs_append "$config_dir"
}

# The connected G.729 call both T.38 flows start from, with the passthrough the
# upgrade needs already on it: SIPp calls the DUT, the hook answers it, the
# stream commits to the codec the fax has to leave, and the local CED converts
# it to G.711 passthrough.
#
# The CED is what says this side is the receiving one, because only an answering
# terminal emits the answer tone, and the upgrade runs from a passthrough leg
# alone. The runner waits for the upspeed answer before it reports the preamble:
# a leg with its own exchange open refuses the upgrade for the state, and a
# runner that reports one tone has no second preamble to retry with.
t38_call_connect()
{
    local flow_dir=$1
    local name=$2

    if ! status_wait "$flow_dir" RINGING 10 ringing; then
        result_print FAIL "$name" "the daemon did not reach RINGING"
        return 1
    fi
    hook_set seize || true
    if ! status_wait "$flow_dir" CONNECTED 10 connected; then
        result_print FAIL "$name" "the daemon did not answer into CONNECTED"
        return 1
    fi
    # HAL_CODEC_G729 is 3
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].running == true and .line[0].stream[0].codec == 3' \
        5 committed-g729; then
        result_print FAIL "$name" "the call did not commit to G.729"
        return 1
    fi
    # the direction gate: a preamble from the wire says the far end answered
    # the fax, and that gateway owns the switch
    tone_send v21 remote || true
    sleep 0.5
    if ! stub_check "$flow_dir" '.line[0].stream[0].codec == 3' remote-v21; then
        result_print FAIL "$name" "a V.21 preamble from the wire upgraded the call"
        return 1
    fi
    # the passthrough rule: a local preamble on a leg the answer tone never
    # converted starts no upgrade, and the far end owns that offer
    tone_send v21 local || true
    sleep 0.5
    if ! stub_check "$flow_dir" '.line[0].stream[0].codec == 3' no-passthrough-v21; then
        result_print FAIL "$name" "a preamble upgraded a leg with no passthrough"
        return 1
    fi
    tone_send ced local || true
    # HAL_CODEC_PCMU is 1: the upspeed answer landed and the leg's own exchange
    # is closed again, so the preamble the flow reports next is not refused
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].fax_applied == true and .line[0].stream[0].codec == 1' \
        15 upspeed; then
        result_print FAIL "$name" "the answer tone did not convert the call to G.711"
        return 1
    fi
    return 0
}

# The T.38 upgrade the DUT offers. The call is already in G.711 passthrough
# from the answer tone, the runner reports a V.21 preamble toward the phone,
# which is the second stage of the fax trigger, and the DUT re-INVITEs with an
# image stream. This scenario accepts it, so the whole chain is exercised: the
# wire offer, the negotiated T.38 parameters reaching the stream, and the fax
# state northbound.
t38_run()
{
    local name=uac_t38_offer
    local sipp_ok=0
    local family_status=0

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! t38_config_write "$config_dir"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/$name.xml" "$sipp_port" \
        "$daemon_port" "$media_port"; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! t38_call_connect "$flow_dir" "$name"; then
        flow_stop
        return
    fi
    # The family of the audio leg, read before the upgrade replaces it. The
    # image session that follows carries the same addresses, and it is read
    # again below once the stub has taken it.
    if ! family_stream_check "$flow_dir" "$sipp_dir/$name.xml" audio; then
        flow_stop
        result_print FAIL "$name" "$family_failure_detail"
        return
    fi
    tone_send v21 local || true
    # The stub pins of the image session: the codec name, which is the only
    # field that says the stream carries UDPTL, and the three negotiated T.38
    # parameters. 9600 and 400 are the numbers this scenario answers with, so
    # they prove the reconciliation ran; HAL_T38_EC_REDUNDANCY is 1.
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].running == true and .line[0].stream[0].codec_name == "t38" and .line[0].stream[0].t38_max_bit_rate == 9600 and .line[0].stream[0].t38_max_datagram == 400 and .line[0].stream[0].t38_ec == 1' \
        15 t38-stream; then
        flow_stop
        result_print FAIL "$name" "the image session did not reach the stream"
        return
    fi
    if ! status_expr_wait "$flow_dir" \
        '.call[0].calls[0].fax == "t38" and .call[0].calls[0].codec == "t38" and .call[0].t38_switched[0] == true' \
        5 fax-t38; then
        flow_stop
        result_print FAIL "$name" "voice status did not report the T.38 session"
        return
    fi
    # the upgrade replaced the carrier of a leg the answer tone had already
    # converted, which is the only leg it starts from
    if ! stub_check "$flow_dir" '.line[0].stream[0].fax_applied == true' from-passthrough; then
        flow_stop
        result_print FAIL "$name" "the upgraded leg carried no passthrough profile"
        return
    fi
    # The image session, which is the one this flow exists for: an UDPTL stream
    # is programmed through the same address record as the audio one, so the
    # family has to survive the upgrade. The scenario answers the image offer
    # on the media port plus two, so the peer's port moves with it.
    if ! family_stream_check "$flow_dir" "$sipp_dir/$name.xml" image \
        "$((media_port + 2))"; then
        flow_stop
        result_print FAIL "$name" "$family_failure_detail"
        return
    fi
    if sipp_finish_wait 45; then
        if uas_sipp_verdict "$flow_dir" 1; then
            sipp_ok=1
        fi
    else
        sipp_verdict_detail="SIPp did not finish within 45 seconds"
    fi
    hook_set release || true
    status_save "$flow_dir" final
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "the daemon did not return to IDLE"
        return
    fi
    # the record of a T.38 call names the image codec, which no audio codec
    # enum can carry: the leg negotiated none at all
    if ! cdr_expr_wait "$flow_dir" \
        '([.cdr[] | select(.codec == "t38")] | length) == 1' 10 cdr-t38; then
        flow_stop
        result_print FAIL "$name" "the call record did not name the T.38 codec"
        return
    fi
    if (( !sipp_ok )); then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    family_check "$flow_dir" received "REGISTER " "$daemon_port" call \
        "$flow_dir/register" || family_status=$?
    if (( family_status != 0 )); then
        flow_stop
        result_print "$(family_verdict "$family_status")" "$name" "$family_failure_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, the image offer was accepted, the negotiated T.38 parameters reached the stream and the record named the codec$family_detail"
}

# A refused T.38 upgrade. The same call and the same preamble, and this scenario
# answers the image offer 488. The DUT keeps the call on the G.711 passthrough
# the answer tone already applied; no second image offer may follow.
t38_reject_run()
{
    local name=uac_t38_reject
    local sipp_ok=0

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! t38_config_write "$config_dir"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/$name.xml" "$sipp_port" \
        "$daemon_port" "$media_port"; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! t38_call_connect "$flow_dir" "$name"; then
        flow_stop
        return
    fi
    tone_send v21 local || true
    # HAL_FAX_PASSTHROUGH is 1, HAL_CODEC_PCMU is 1 and HAL_PASSTHROUGH_FAX is
    # 2: the refusal leaves the carrier of the answer tone standing, with the
    # kind the preamble moved it to
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].fax_applied == true and .line[0].stream[0].fax_mode == 1 and .line[0].stream[0].fax_codec == 1 and .line[0].stream[0].fax_jb_fixed == true' \
        15 fax-profile; then
        flow_stop
        result_print FAIL "$name" "the refused upgrade lost the passthrough profile"
        return
    fi
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].codec == 1 and .line[0].stream[0].codec_name == "pcmu" and .line[0].stream[0].passthrough == 2' \
        15 keeps-g711; then
        flow_stop
        result_print FAIL "$name" "the refused upgrade did not keep the G.711 fax carrier"
        return
    fi
    if ! status_expr_wait "$flow_dir" \
        '.call[0].calls[0].fax == "passthrough" and .call[0].t38_switched[0] == true' \
        5 fax-passthrough; then
        flow_stop
        result_print FAIL "$name" "voice status did not report the passthrough call"
        return
    fi
    if sipp_finish_wait 45; then
        if uas_sipp_verdict "$flow_dir" 1; then
            sipp_ok=1
        fi
    else
        sipp_verdict_detail="SIPp did not finish within 45 seconds"
    fi
    # the attempt latch is spent, so the stream never became an image one
    if ! stub_check "$flow_dir" '.line[0].stream[0].codec_name == "pcmu"' no-second-image; then
        flow_stop
        result_print FAIL "$name" "a second image session reached the stream"
        return
    fi
    hook_set release || true
    status_save "$flow_dir" final
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "the daemon did not return to IDLE"
        return
    fi
    # the call ended on the G.711 the answer tone moved it to, so its record
    # names that codec and no image one
    if ! cdr_expr_wait "$flow_dir" \
        '([.cdr[] | select(.codec == "t38")] | length) == 0 and ([.cdr[] | select(.codec == "PCMU")] | length) == 1' \
        10 cdr-pcmu; then
        flow_stop
        result_print FAIL "$name" "the call record did not name the G.711 codec"
        return
    fi
    if (( !sipp_ok )); then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, the refused upgrade kept the G.711 passthrough, offered no second image session and recorded the G.711 codec"
}

# The config the four SRTP flows run on. srtp_enabled is a base option, so it
# cannot be appended behind config_write the way the fax flows append their
# channel options; it rides config_write's own base section.
srtp_config_write()
{
    local config_dir=$1
    local enabled=$2

    config_write "$config_dir" "$sipp_port" "$daemon_port" "" "" "" "" "$enabled"
}

# The keyed stream both keyed flows read back. HAL_SRTP_AES_CM_128_SHA1_80 is 1,
# and the two digests are present only while the stream runs.
srtp_stream_wait()
{
    local flow_dir=$1

    stub_wait "$flow_dir" \
        '.line[0].stream[0].running == true and .line[0].stream[0].srtp_suite == 1 and .line[0].stream[0].srtp_suite_name == "aes_cm_128_hmac_sha1_80" and (.line[0].stream[0] | has("srtp_key_tx_crc")) and (.line[0].stream[0] | has("srtp_key_rx_crc")) and .line[0].stream[0].srtp_key_tx_crc > 0 and .line[0].stream[0].srtp_key_rx_crc > 0' \
        5 srtp-keyed
}

# The post-stop dump contract (hal.h, the srtp block). What this proves is what
# a reader of a stopped stream sees: the suite reads HAL_SRTP_NONE and no
# digest field is present. It is not a byte-level proof of the wipe, because
# srtp_dump() omits both digests whenever the suite is HAL_SRTP_NONE. The wipe
# itself is pinned one layer down: the stub's stream_stop() clears its srtp
# block with a memset (stub.c), and the call layer's release wipe has its own
# oracle in call_slot_scan (test_call). The second reading is a negative one
# and therefore a single sample, not a poll.
srtp_zeroize_check()
{
    local flow_dir=$1

    stub_wait "$flow_dir" \
        '.line[0].stream[0].running == false and .line[0].stream[0].srtp_suite == 0 and .line[0].stream[0].srtp_suite_name == "none"' \
        5 stopped || return 1
    stub_check "$flow_dir" \
        '(.line[0].stream[0] | has("srtp_key_tx_crc")) == false and (.line[0].stream[0] | has("srtp_key_rx_crc")) == false' \
        no-digest
}

# The outbound SRTP leg. The DUT calls out with srtp_enabled 1 and offers two
# suites; the scenario answers RTP/SAVP with one crypto line that mirrors tag 1
# and carries a fixed key. What this flow adds to the scenario's own SDP
# assertions is the stream: the suite reached the backend, the key the DUT
# offered is the key it transmits with, the key the answer carried is the key
# it receives with, and the stop zeroizes both.
srtp_outbound_run()
{
    local name=srtp-outbound
    local detail
    local pause_ms

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! srtp_config_write "$config_dir" 1; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    if ! sipp_start "$flow_dir" "$sipp_dir/uas_srtp_call.xml" "$sipp_port" \
        "$daemon_port" "$media_port" 2; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! daemon_start "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    if ! outbound_dial_start "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "off-hook dial setup failed"
        return
    fi
    if ! status_wait "$flow_dir" CONNECTED 15 connected; then
        flow_stop
        result_print FAIL "$name" "daemon did not reach CONNECTED"
        return
    fi
    if ! media_port_wait "$flow_dir" 5; then
        flow_stop
        result_print FAIL "$name" "stream 0 did not use the SDP media port"
        return
    fi
    if ! srtp_stream_wait "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "the negotiated suite and keys did not reach the stream"
        return
    fi
    if ! srtp_crypto_shape_check "$flow_dir" received "INVITE " 2; then
        flow_stop
        result_print FAIL "$name" \
            "the INVITE did not carry exactly two bare crypto attributes"
        return
    fi
    if ! srtp_digest_check "$flow_dir" received srtp_key_tx_crc; then
        flow_stop
        result_print FAIL "$name" \
            "the transmit key of the stream is not the tag 1 key the INVITE offered"
        return
    fi
    if ! srtp_digest_check "$flow_dir" sent srtp_key_rx_crc; then
        flow_stop
        result_print FAIL "$name" \
            "the receive key of the stream is not the key the answer carried"
        return
    fi
    # the scenario waits for the BYE only after its pause, the flow-a rule
    pause_ms=$(scenario_pause_get "$sipp_dir/uas_srtp_call.xml")
    sleep "$(( (pause_ms + 1500) / 1000 ))"
    hook_set release || true
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not complete the BYE exchange"
        return
    fi
    if ! srtp_zeroize_check "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "the stopped stream kept its suite or a key digest"
        return
    fi
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "daemon did not return to IDLE"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 2; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    detail=$sipp_verdict_detail
    flow_stop
    result_print PASS "$name" \
        "$detail, the INVITE carried exactly two bare crypto attributes, both key digests matched the wire and the stopped stream reported no suite and no digest"
}

# The inbound SRTP leg. SIPp calls the DUT with RTP/SAVP and two crypto lines;
# the DUT answers one mirrored line with its own key. The digests run in the
# other direction here, and voice status has to name the suite while the call
# is up.
srtp_inbound_run()
{
    local name=srtp-inbound
    local family_status=0

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! srtp_config_write "$config_dir" 1; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/uac_srtp_call.xml" "$sipp_port" \
        "$daemon_port" "$media_port"; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! status_wait "$flow_dir" RINGING 10 ringing; then
        flow_stop
        result_print FAIL "$name" "daemon did not reach RINGING"
        return
    fi
    hook_set seize || true
    if ! status_wait "$flow_dir" CONNECTED 10 connected; then
        flow_stop
        result_print FAIL "$name" "daemon did not answer into CONNECTED"
        return
    fi
    if ! media_port_wait "$flow_dir" 5; then
        flow_stop
        result_print FAIL "$name" "stream 0 did not use the SDP media port"
        return
    fi
    if ! srtp_stream_wait "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "the negotiated suite and keys did not reach the stream"
        return
    fi
    if ! family_stream_check "$flow_dir" "$sipp_dir/uac_srtp_call.xml"; then
        flow_stop
        result_print FAIL "$name" "$family_failure_detail"
        return
    fi
    if ! srtp_crypto_shape_check "$flow_dir" received "SIP/2.0 200" 1; then
        flow_stop
        result_print FAIL "$name" \
            "the 200 did not carry exactly one bare crypto attribute"
        return
    fi
    if ! srtp_keys_differ_check "$flow_dir" received sent; then
        flow_stop
        result_print FAIL "$name" \
            "the key the 200 answered with is one of the keys the offer carried"
        return
    fi
    if ! srtp_digest_check "$flow_dir" received srtp_key_tx_crc; then
        flow_stop
        result_print FAIL "$name" \
            "the transmit key of the stream is not the key the 200 answered with"
        return
    fi
    if ! srtp_digest_check "$flow_dir" sent srtp_key_rx_crc; then
        flow_stop
        result_print FAIL "$name" \
            "the receive key of the stream is not the tag 1 key the offer carried"
        return
    fi
    # The northbound word for the same fact, presence-gated the way the fax key
    # is: a plain call carries no srtp key at all. It is read last of the
    # mid-call assertions, because a poll that waits out its five seconds
    # would spend the talk window the digests need.
    if ! status_expr_wait "$flow_dir" \
        '.call[0].calls[0].srtp == "AES_CM_128_HMAC_SHA1_80"' 5 srtp-status; then
        flow_stop
        result_print FAIL "$name" "voice status did not name the negotiated suite"
        return
    fi
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not finish its BYE exchange"
        return
    fi
    if (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "SIPp exited with status $process_status"
        return
    fi
    if ! srtp_zeroize_check "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "the stopped stream kept its suite or a key digest"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "daemon did not return to IDLE after release"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    family_check "$flow_dir" received "REGISTER " "$daemon_port" call \
        "$flow_dir/register" || family_status=$?
    if (( family_status != 0 )); then
        flow_stop
        result_print "$(family_verdict "$family_status")" "$name" "$family_failure_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, the 200 carried exactly one bare crypto attribute under a key of the DUT's own, voice status named the suite, both key digests matched the wire and the stopped stream reported no suite and no digest$family_detail"
}

# The accept-plain leg. An SRTP-enabled leg answers a plain initial offer
# without SRTP status or stream policy. Its hold and resume re-INVITEs must
# remain plain, and its stop must stay zeroized.
srtp_accept_plain_run()
{
    local name=srtp-accept-plain

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! srtp_config_write "$config_dir" 1; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/uac_srtp_plain_hold.xml" \
        "$sipp_port" "$daemon_port" "$media_port"; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! status_wait "$flow_dir" RINGING 10 ringing; then
        flow_stop
        result_print FAIL "$name" "daemon did not reach RINGING"
        return
    fi
    hook_set seize || true
    if ! status_wait "$flow_dir" CONNECTED 10 connected; then
        flow_stop
        result_print FAIL "$name" "daemon did not answer into CONNECTED"
        return
    fi
    if ! media_port_wait "$flow_dir" 5; then
        flow_stop
        result_print FAIL "$name" "stream 0 did not use the SDP media port"
        return
    fi
    if ! status_expr_check "$flow_dir" \
        '(.call[0].calls[0] | has("srtp")) == false' no-srtp-key; then
        flow_stop
        result_print FAIL "$name" "voice status carried an SRTP key on the plain call"
        return
    fi
    if ! stub_check "$flow_dir" \
        '.line[0].stream[0].running == true and .line[0].stream[0].srtp_suite == 0 and (.line[0].stream[0] | has("srtp_key_tx_crc")) == false and (.line[0].stream[0] | has("srtp_key_rx_crc")) == false' \
        plain-stream; then
        flow_stop
        result_print FAIL "$name" "the plain stream carried an SRTP suite or key digest"
        return
    fi
    flash_send || true
    if ! status_wait "$flow_dir" HOLD_DIAL 15 hold; then
        flow_stop
        result_print FAIL "$name" "the plain hold re-INVITE did not complete"
        return
    fi
    flash_send || true
    if ! status_wait "$flow_dir" CONNECTED 15 resumed; then
        flow_stop
        result_print FAIL "$name" "the plain resume re-INVITE did not complete"
        return
    fi
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not finish the plain re-INVITE and BYE exchanges"
        return
    fi
    if (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "SIPp exited with status $process_status"
        return
    fi
    if ! srtp_zeroize_check "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "the stopped plain stream reported an SRTP suite or key digest"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "daemon did not return to IDLE after release"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, the plain answer carried RTP/AVP with no crypto attribute, voice status carried no srtp key, the running stream carried no suite or key digest, both hold and resume re-INVITEs carried RTP/AVP with no crypto attribute, and the stopped stream reported no suite and no digest"
}

# The SAVP offer that srtp-inbound negotiates, sent to a DUT with the switch
# off. The refusal comes at answer time and not before the ring, because the
# pre-ring check reads the offered address alone (call.c). The flow therefore
# rings the line, takes it off hook and reads the 488 of the answer build.
srtp_refused_run()
{
    local name=srtp-refused

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! srtp_config_write "$config_dir" 0; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/uac_savp_refused.xml" \
        "$sipp_port" "$daemon_port" "$media_port"; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! status_wait "$flow_dir" RINGING 10 ringing; then
        flow_stop
        result_print FAIL "$name" "daemon did not reach RINGING"
        return
    fi
    hook_set seize || true
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not receive the 488 refusal"
        return
    fi
    if (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "SIPp exited with status $process_status"
        return
    fi
    if ! daemon_log_require "$flow_dir" \
        "the offer carries no usable codec or address" 5; then
        flow_stop
        result_print FAIL "$name" "the daemon log carries no answer-build refusal"
        return
    fi
    # The refusal is the leg switch. account.c warns once per line when the
    # switch is on over a transport that does not encrypt the signaling, so
    # that line is absent exactly when the switch is off. The check reads the
    # daemon log and not the config this runner writes, so a config that
    # drifts from the switch the flow claims fails the flow.
    if grep -qF "srtp_enabled over udp: the SDES master keys travel in clear" \
        "$flow_dir/daemon.log"; then
        flow_stop
        result_print FAIL "$name" \
            "the leg ran WITH the SRTP switch, so the refusal is not the switch"
        return
    fi
    # Nothing was ever keyed: a refusal that programmed a stream first would
    # leave the key material behind, which is the state the zeroize exists for
    if grep -qF '[hal] stub: line 0 strm 0 start' "$flow_dir/daemon.log"; then
        flow_stop
        result_print FAIL "$name" "media started on a call that was refused"
        return
    fi
    if ! stub_check "$flow_dir" \
        '.line[0].stream[0].running == false and .line[0].stream[0].srtp_suite == 0 and (.line[0].stream[0] | has("srtp_key_tx_crc")) == false' \
        no-stream; then
        flow_stop
        result_print FAIL "$name" "the refused call left a running or keyed stream"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "daemon did not return to IDLE after release"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, the SAVP offer of a DUT with the switch off was answered 488 and no stream was ever keyed"
}

# ------------------------------------------------------------ the stream flows

# The config of one stream account. transport is a base option, so it rides
# config_write; a channel option a stream flow needs is appended behind this
# call the way the fax flows append theirs.
stream_config_write()
{
    local config_dir=$1
    local transport=$2
    local base_extra=${3:-}

    config_write "$config_dir" "$sipp_port" "$daemon_port" "" "" "" "" "" \
        "$transport" "$base_extra"
}

# The northbound word for the transport (src/proto/sip/account.c
# acct_conn_status_dump). "transport" names it on every line; "listener" is
# present on every stream transport and says whether the inbound half is up;
# "connection" is presence-gated and carries the state, the peer and the queue
# depth of the connection the line is using. These are asserted end to end
# because the unit suite pins the dump function, not the line that reaches an
# operator.
stream_status_check()
{
    local flow_dir=$1
    local transport=$2
    local extra=$3
    local label=$4

    status_expr_wait "$flow_dir" \
        ".lines[0].transport == \"$transport\"$extra" 10 "$label"
}

# The Via token and the Contact parameter of the REGISTER, which every stream
# flow sends and every stream flow therefore pins.
stream_register_wire_check()
{
    local flow_dir=$1
    local transport=$2
    local token=$3

    stream_via_check "$flow_dir" received "REGISTER " "$token" register-via || return 1
    stream_contact_check "$flow_dir" received "REGISTER " "$transport" register-contact
}

# The outbound twin. The DUT is the UAC: it dials out over its configured
# transport and this flow reads the transport off the INVITE it sent, off the
# REGISTER behind it and off the line's own status. The tls twin runs the same
# scenario through the TLS SIPp, so the transport is the only thing that
# differs between the two runs.
#
# No bundle is installed here. A tls line with no certificate pair comes up
# client only, which is the normal state sip/tls.h describes ("Client only is
# a normal state") and the state a box runs in until its bundle lands, so this
# twin also reads that back.
stream_uac_run()
{
    local transport=$1
    local name=stream-uac-$transport
    local token=${transport^^}
    local listener_pin=
    local detail
    local pause_ms
    local family_status=0
    local cleartext_status=0

    flow_transport=$transport
    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        flow_stop
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! stream_config_write "$config_dir" "$transport"; then
        flow_stop
        result_print FAIL "$name" "config write failed"
        return
    fi
    # The client-only reading is taken under the lock. This twin installs
    # nothing, but it reads the frozen bundle path to decide whether it may
    # assert the client-only state, and a read of a shared path is a touch: the
    # lock is what keeps the answer true for the length of the flow. A bundle
    # another run owns would bring the listener up underneath it.
    if [[ $transport = tls ]]; then
        if ! tls_lock_take "$name"; then
            flow_stop
            result_print FAIL "$name" \
                "the frozen bundle paths are locked by another run: $(tls_lock_owner)"
            return
        fi
        if [[ ! -e $tls_bundle_dir/cert.pem ]]; then
            listener_pin=' and .lines[0].listener == false'
        fi
    fi
    capture_start "$flow_dir" "$sipp_port" "$(stream_capture_filter)" || true
    if ! sipp_start "$flow_dir" "$sipp_dir/uas_stream_call.xml" "$sipp_port" \
        "$daemon_port" "$media_port" 2; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! daemon_start "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    if ! outbound_dial_start "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "off-hook dial setup failed"
        return
    fi
    if ! status_wait "$flow_dir" CONNECTED 15 connected; then
        flow_stop
        result_print FAIL "$name" "daemon did not reach CONNECTED over $transport"
        return
    fi
    if ! media_port_wait "$flow_dir" 5; then
        flow_stop
        result_print FAIL "$name" "stream 0 did not use the SDP media port"
        return
    fi
    if ! family_stream_check "$flow_dir" "$sipp_dir/uas_stream_call.xml"; then
        flow_stop
        result_print FAIL "$name" "$family_failure_detail"
        return
    fi
    if ! stream_via_check "$flow_dir" received "INVITE " "$token" invite-via; then
        flow_stop
        result_print FAIL "$name" "the INVITE Via did not name $token"
        return
    fi
    if ! stream_contact_check "$flow_dir" received "INVITE " "$transport" invite-contact; then
        flow_stop
        result_print FAIL "$name" "the INVITE Contact carried no ;transport=$transport"
        return
    fi
    if ! stream_register_wire_check "$flow_dir" "$transport" "$token"; then
        flow_stop
        result_print FAIL "$name" "the REGISTER Via or Contact did not name $transport"
        return
    fi
    # The connection is up while the call runs, and the peer it names is the
    # signalling peer and not the media one.
    if ! stream_status_check "$flow_dir" "$transport" \
        " and .lines[0].connection.state == \"up\" and (.lines[0].connection.peer | endswith(\":$sipp_port\")) and (.lines[0].connection | has(\"queued\"))$listener_pin" \
        connection; then
        flow_stop
        result_print FAIL "$name" \
            "voice status did not carry the transport keys of a live $transport line"
        return
    fi
    # the scenario waits for the BYE only after its pause, the flow-a rule
    pause_ms=$(scenario_pause_get "$sipp_dir/uas_stream_call.xml")
    sleep "$(( (pause_ms + 1500) / 1000 ))"
    hook_set release || true
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not complete the BYE exchange"
        return
    fi
    if ! stub_wait "$flow_dir" '.line[0].stream[0].running == false' 5 stopped; then
        flow_stop
        result_print FAIL "$name" "stream 0 remained active after the BYE"
        return
    fi
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "daemon did not return to IDLE"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 2; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    detail=$sipp_verdict_detail
    family_check "$flow_dir" received "INVITE " "$daemon_port" call || family_status=$?
    if (( family_status != 0 )); then
        flow_stop
        result_print "$(family_verdict "$family_status")" "$name" "$family_failure_detail"
        return
    fi
    # The ip6 arm of the no-cleartext reader, which only a tls flow can ask
    # for: over tcp the SIP text is in the clear by design. It runs here and
    # not in the tcp twin for that reason, and the capture is already stopped
    # by the family readers above.
    if (( flow_family == 6 )) && [[ $transport = tls ]]; then
        pcap_cleartext_check "$flow_dir" "$sipp_port" "$daemon_port" || cleartext_status=$?
        case $cleartext_status in
            0)
                family_detail+=", and the ip6 capture holds no cleartext SIP line and no bare keepalive ($(head -1 "$flow_dir/pcap-cleartext.txt"))"
                ;;
            2)
                flow_stop
                result_print PARTIAL "$name" \
                    "$detail, the ip6 capture could not be read: $(head -1 "$flow_dir/pcap-cleartext.txt")"
                return
                ;;
            *)
                flow_stop
                result_print FAIL "$name" \
                    "the ip6 tls capture carries cleartext: $(sed -n '2p' "$flow_dir/pcap-cleartext.txt")"
                return
                ;;
        esac
    fi
    flow_stop
    result_print PASS "$name" \
        "$detail, the INVITE and the REGISTER named $token in the Via and $transport in the Contact, and the line reported its transport and a live connection$family_detail"
}

# The inbound twin. SIPp calls the DUT through its stream listener, so the tls
# twin needs the certificate bundle: a TLS context with no server half takes
# no inbound connection at all. The Via of a response is the caller's own
# header echoed back, so the DUT's own writer shows in the Contact, and that is
# what this flow pins on the wire.
#
# The line registers first, against the registrar of register_prime over the
# same transport. The caller cannot take that REGISTER itself. SIPp connects
# at startup over a reliable transport and leaves at once when the port it
# dials is not listening (measured: "Error on TCP connection ... Connection
# refused"), so the daemon has to come up first. A SIPp UAC over a stream
# transport also answers an out-of-call REGISTER on the same socket the dialog
# uses, and closes that socket once the out-of-call scenario ends: the capture
# then showed SIPp's FIN between the 180 and the 200.
#
# The registrar closes its connection when it exits, and the caller connects
# at once from the same address and port. The daemon takes that connection as
# one the peer opened, which keeps the binding (src/proto/sip/account.c,
# acct_reconnect_needed()). A keepalive sent in the gap would open a connection
# of the line's own, which is a reconnect and a fresh REGISTER. The first
# keepalive leaves one interval, 1 s here, after the registration, and the gap
# is about a tenth of that (test/e2e/README.md, "The gap between the two runs").
# The REGISTER token pins stay in the outbound twins, where the call peer
# answers the REGISTER.
stream_uas_run()
{
    local transport=$1
    local name=stream-uas-$transport
    local bundle_state=
    local detail
    local family_status=0

    flow_transport=$transport
    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        flow_stop
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    # One second of NAT keepalive. The DUT's own RFC 5626 ping must not travel
    # in the clear, so the tls twin has to carry it inside the
    # captured window; keepalive_interval is a base option and
    # rides config_write's base block.
    if ! stream_config_write "$config_dir" "$transport" \
        "$(printf "\toption keepalive_interval '1'")"; then
        flow_stop
        result_print FAIL "$name" "config write failed"
        return
    fi
    if [[ $transport = tls ]] && ! tls_bundle_install "$name"; then
        tls_bundle_record "$flow_dir" install-failed
        flow_stop
        result_print FAIL "$name" \
            "the certificate bundle could not be composed in $tls_bundle_dir, lock holder: $(tls_lock_owner)"
        return
    fi
    if [[ $transport = tls ]]; then
        tls_bundle_record "$flow_dir" installed
    fi
    capture_start "$flow_dir" "$sipp_port" "$(stream_capture_filter)" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    # The listener is the precondition of this flow, so it is waited for and
    # not merely read afterwards: a SIPp that connects before the listener is
    # bound fails with a connection error and says nothing about the transport.
    if ! stream_status_check "$flow_dir" "$transport" \
        ' and .lines[0].listener == true' listener; then
        # The listing is taken before the teardown. flow_stop removes the
        # bundle this flow owns, so a reading made after it always reports an
        # empty directory and the diagnostic would accuse every failure of a
        # missing bundle.
        bundle_state=$(tls_bundle_list)
        if [[ $transport = tls ]]; then
            tls_bundle_record "$flow_dir" listener-timeout
        fi
        flow_stop
        result_print FAIL "$name" \
            "the line brought up no $transport listener, bundle: $bundle_state"
        return
    fi
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/uac_stream_call.xml" "$sipp_port" \
        "$daemon_port" "$media_port"; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! status_expr_wait "$flow_dir" \
        '.call[0].state == "RINGING" and .lines[0].status == "registered"' \
        10 ringing; then
        flow_stop
        result_print FAIL "$name" "the inbound call did not ring a registered line"
        return
    fi
    hook_set seize || true
    if ! status_wait "$flow_dir" CONNECTED 10 connected; then
        flow_stop
        result_print FAIL "$name" "daemon did not answer into CONNECTED"
        return
    fi
    if ! media_port_wait "$flow_dir" 5; then
        flow_stop
        result_print FAIL "$name" "stream 0 did not use the SDP media port"
        return
    fi
    if ! family_stream_check "$flow_dir" "$sipp_dir/uac_stream_call.xml"; then
        flow_stop
        result_print FAIL "$name" "$family_failure_detail"
        return
    fi
    if ! stream_ping_check "$flow_dir" "$daemon_port" "$transport"; then
        flow_stop
        result_print FAIL "$name" \
            "the listener did not answer the CRLF ping inside the connection: $(head -c 200 "$flow_dir/stream-ping.txt")"
        return
    fi
    if ! stream_contact_check "$flow_dir" received "SIP/2.0 180" "$transport" ring-contact; then
        flow_stop
        result_print FAIL "$name" "the 180 Contact carried no ;transport=$transport"
        return
    fi
    if ! stream_contact_check "$flow_dir" received "SIP/2.0 200" "$transport" answer-contact; then
        flow_stop
        result_print FAIL "$name" "the 200 Contact carried no ;transport=$transport"
        return
    fi
    if ! stream_status_check "$flow_dir" "$transport" \
        " and .lines[0].status == \"registered\" and .lines[0].listener == true and .lines[0].connection.state == \"up\" and (.lines[0].connection.peer | endswith(\":$sipp_port\")) and (.lines[0].connection | has(\"queued\"))" \
        connection; then
        flow_stop
        result_print FAIL "$name" \
            "voice status did not carry the transport keys of a live $transport line"
        return
    fi
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not finish its BYE exchange"
        return
    fi
    if (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "SIPp exited with status $process_status"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "daemon did not return to IDLE after release"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    detail=$sipp_verdict_detail
    if ! accepted_reregister_check "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" \
            "the connection that the caller opened restarted the registration"
        return
    fi
    # The DUT's own keepalive, which is why keepalive_interval is one second in
    # this config. An RFC 5626 ping in the clear is the fault under test, so
    # the window the capture pin reads below has to hold at least one of the
    # DUT's own pings; an absence claim over a window with no keepalive in it
    # would say nothing.
    if ! status_expr_wait "$flow_dir" \
        '.lines[0].status == "registered" and .lines[0].keepalive.crlf_sent >= 1' \
        5 keepalive; then
        flow_stop
        result_print FAIL "$name" "the line sent no CRLF keepalive over $transport"
        return
    fi
    # The 200 of the answered call. The REGISTER went to the registrar of
    # register_prime and the daemon sends no request of its own to this peer,
    # so the Via the reader reads is the caller's, echoed back: it says the
    # exchange rode a v6 wire with a bracketed sent-by, and the Contact beside
    # it is the one the daemon wrote. The port is therefore the peer's and not
    # the daemon's.
    family_check "$flow_dir" received "SIP/2.0 200" "$sipp_port" call || family_status=$?
    if (( family_status != 0 )); then
        flow_stop
        result_print "$(family_verdict "$family_status")" "$name" "$family_failure_detail"
        return
    fi
    status_save "$flow_dir" final
    if [[ $transport = tls ]]; then
        capture_finish
        if [[ $capture_ready -ne 1 ]]; then
            flow_stop
            result_print PARTIAL "$name" "$detail, the cleartext capture was unavailable"
            return
        fi
        pcap_cleartext_check "$flow_dir" "$sipp_port" "$daemon_port"
        case $? in
            0)
                detail="$detail, and the capture holds no cleartext SIP line and no bare keepalive ($(head -1 "$flow_dir/pcap-cleartext.txt"))"
                ;;
            2)
                flow_stop
                result_print PARTIAL "$name" \
                    "$detail, the capture could not be read: $(head -1 "$flow_dir/pcap-cleartext.txt")"
                return
                ;;
            *)
                flow_stop
                result_print FAIL "$name" \
                    "the tls capture carries cleartext: $(sed -n '2p' "$flow_dir/pcap-cleartext.txt")"
                return
                ;;
        esac
    fi
    flow_stop
    result_print PASS "$name" \
        "$detail, the listener answered the CRLF ping inside the connection, the line sent its own, and the 180 and the 200 carried ;transport=$transport$family_detail"
}

# The REGISTER and keepalive twin. The OPTIONS ping is the second request the
# account writes a Via for, and options_interval is a channel alias, so it is
# appended behind config_write the way the fax flows append theirs. The CRLF
# ping is the third payload of the connection and is driven by the harness's
# own client, because SIPp cannot put four bare bytes on the wire.
#
# The tls twin needs the bundle, because the CRLF ping is answered by the
# listener and a TLS context with no server half has none.
stream_register_run()
{
    local transport=$1
    local name=stream-register-$transport
    local token=${transport^^}
    local bundle_state=
    local detail
    local family_status=0

    flow_transport=$transport
    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        flow_stop
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! stream_config_write "$config_dir" "$transport"; then
        flow_stop
        result_print FAIL "$name" "config write failed"
        return
    fi
    # still part of the channel section config_write closed with
    printf "\toption options_interval '%u'\n" 2 >>"$config_dir/voip"
    if [[ $transport = tls ]] && ! tls_bundle_install "$name"; then
        tls_bundle_record "$flow_dir" install-failed
        flow_stop
        result_print FAIL "$name" \
            "the certificate bundle could not be composed in $tls_bundle_dir, lock holder: $(tls_lock_owner)"
        return
    fi
    if [[ $transport = tls ]]; then
        tls_bundle_record "$flow_dir" installed
    fi
    capture_start "$flow_dir" "$sipp_port" "$(stream_capture_filter)" || true
    if ! sipp_start "$flow_dir" "$sipp_dir/uas_options_keepalive.xml" "$sipp_port" \
        "$daemon_port" "$media_port" 2; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! daemon_start "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    # The listener is the precondition of the ping below, so it is waited for
    # rather than assumed: over tls it appears only once the contexts carry a
    # server half.
    if ! stream_status_check "$flow_dir" "$transport" \
        ' and .lines[0].listener == true' listener; then
        # read the directory before the teardown empties it
        bundle_state=$(tls_bundle_list)
        if [[ $transport = tls ]]; then
            tls_bundle_record "$flow_dir" listener-timeout
        fi
        flow_stop
        result_print FAIL "$name" \
            "the line brought up no $transport listener, bundle: $bundle_state"
        return
    fi
    if ! stream_ping_check "$flow_dir" "$daemon_port" "$transport"; then
        flow_stop
        result_print FAIL "$name" \
            "the listener did not answer the CRLF ping inside the connection: $(head -c 200 "$flow_dir/stream-ping.txt")"
        return
    fi
    # The OPTIONS the scenario answered, counted by the daemon itself: the
    # answer is what says the ping completed, and the Via pin below then reads
    # the request that earned it.
    if ! status_expr_wait "$flow_dir" '.lines[0].keepalive.options_alive >= 1' \
        20 options-alive; then
        flow_stop
        result_print FAIL "$name" "no OPTIONS ping was answered over $transport"
        return
    fi
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not finish the OPTIONS exchange"
        return
    fi
    if ! stream_via_check "$flow_dir" received "OPTIONS " "$token" options-via; then
        flow_stop
        result_print FAIL "$name" "the OPTIONS Via did not name $token"
        return
    fi
    if ! stream_register_wire_check "$flow_dir" "$transport" "$token"; then
        flow_stop
        result_print FAIL "$name" "the REGISTER Via or Contact did not name $transport"
        return
    fi
    if ! stream_status_check "$flow_dir" "$transport" \
        ' and .lines[0].listener == true' status; then
        flow_stop
        result_print FAIL "$name" "voice status did not report a $transport line with a listener"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    detail=$sipp_verdict_detail
    # A registration and keepalive flow carries no call, so it reads the family
    # off the REGISTER headers, the status and the capture alone.
    family_check "$flow_dir" received "REGISTER " "$daemon_port" signal || family_status=$?
    if (( family_status != 0 )); then
        flow_stop
        result_print "$(family_verdict "$family_status")" "$name" "$family_failure_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$detail, the REGISTER and the OPTIONS named $token in the Via, and the listener answered the CRLF ping inside the connection$family_detail"
}

# The SUBSCRIBE twin. mwi_subscribe is a base option, so it rides
# config_write's base_extra: a channel section cannot carry it. No bundle is
# installed: the DUT is the client of both requests here, so the line runs
# client only over tls and needs no listener at all.
stream_subscribe_run()
{
    local transport=$1
    local name=stream-subscribe-$transport
    local token=${transport^^}
    local detail

    flow_transport=$transport
    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        flow_stop
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! stream_config_write "$config_dir" "$transport" "$(printf "\toption mwi_subscribe '1'")"; then
        flow_stop
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" "$(stream_capture_filter)" || true
    if ! sipp_start "$flow_dir" "$sipp_dir/uas_stream_subscribe.xml" "$sipp_port" \
        "$daemon_port" "$media_port" 2; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! daemon_start "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    if ! sipp_finish_wait 40; then
        flow_stop
        result_print FAIL "$name" "SIPp did not receive the SUBSCRIBE"
        return
    fi
    if ! stream_via_check "$flow_dir" received "SUBSCRIBE " "$token" subscribe-via; then
        flow_stop
        result_print FAIL "$name" "the SUBSCRIBE Via did not name $token"
        return
    fi
    if ! stream_contact_check "$flow_dir" received "SUBSCRIBE " "$transport" \
        subscribe-contact; then
        flow_stop
        result_print FAIL "$name" "the SUBSCRIBE Contact carried no ;transport=$transport"
        return
    fi
    if ! stream_status_check "$flow_dir" "$transport" '' status; then
        flow_stop
        result_print FAIL "$name" "voice status did not name the $transport transport"
        return
    fi
    # Two successful instances, not one: the scenario answers the registration
    # and the subscription, and the registration is the precondition of the
    # subscription rather than noise this flow absorbs.
    if ! uas_sipp_verdict "$flow_dir" 2; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    detail=$sipp_verdict_detail
    flow_stop
    result_print PASS "$name" \
        "$detail, the SUBSCRIBE named $token in the Via and $transport in the Contact"
}

# ---------------------------------------------------------- the transfer flows

# The exact value of one header of one message of the log. The Via and Contact
# readers above answer a token; a transfer pins whole header values: the
# Refer-To target with its escaped URI headers, the Referred-By identity and
# the Replaces the child INVITE promotes. A SIPp regexp cannot carry one: a
# regexp lives in an XML attribute and an attribute cannot hold the '<' a
# name-addr opens with (RFC 3261 25.1; the Refer-To grammar itself is
# RFC 3515 2.1).
#
# It fails a message that carries the header twice, and a reader that took the
# first line would pass a message carrying two. Exactly one Replaces is what
# RFC 3891 3 requires; Refer-To's single value is RFC 3515 2.1's; for every
# other header the rule is the reader's own.
xfer_hdr_check()
{
    local flow_dir=$1
    local direction=$2
    local start_line=$3
    local header=$4
    local expected=$5
    local lines
    local count

    lines=$(sipp_msg_header_get "$flow_dir" "$direction" "$start_line" "$header") || return 1
    printf '%s "%s" %s\n' "$direction" "$start_line" "$lines" >>"$flow_dir/xfer-headers.txt"

    count=$(grep -c . <<<"$lines")
    (( count == 1 )) || return 1
    # The value is what is pinned, not the spelling of the name in front of it:
    # the reader above matches the name without case, so the comparison strips
    # it through the first colon and the linear whitespace behind it. A SIP
    # header name carries no colon of its own (RFC 3261 7.3.1).
    [[ $(xfer_hdr_value "$lines") = "$expected" ]]
}

# The value half of one header line: everything past the first colon, with the
# leading whitespace of RFC 3261 7.3.1 removed.
xfer_hdr_value()
{
    local value=${1#*:}

    printf '%s\n' "${value#"${value%%[![:space:]]*}"}"
}

# The same message read for an absence: no Replaces on an attended REFER, no
# Require on a child that promoted none.
#
# The message has to be there first. A reader that only asks for the header
# answers "absent" for a message that never arrived at all, and an absence pin
# over a message nobody sent asserts nothing. CSeq is the control header,
# because every SIP request carries one (RFC 3261 8.1.1.5).
#
# The absence is read off the output and not off the status. sipp_msg_header_get
# succeeds for a message it found whether or not the message carried the header,
# and answers an empty list in the second case, which is the same shape the Via
# reader guards with its own seen counter.
#
# The fifth argument picks the message, the way the fifth argument of the
# reader does, and defaults to the first. The service route flows need it. A
# challenged call holds two ACKs of one dialog. The ACK of the 407 carries the
# route it mirrors, and the ACK of the 2xx carries none. An absence read over
# the first ACK alone would answer for the wrong message.
xfer_hdr_absent_check()
{
    local flow_dir=$1
    local direction=$2
    local start_line=$3
    local header=$4
    local occurrence=${5:-1}
    local control
    local lines

    control=$(sipp_msg_header_get "$flow_dir" "$direction" "$start_line" CSeq \
        "$occurrence") || return 1
    [[ -n $control ]] || return 1

    lines=$(sipp_msg_header_get "$flow_dir" "$direction" "$start_line" "$header" \
        "$occurrence") || return 1
    [[ -z $lines ]] || return 1

    printf '%s "%s" #%s carries no %s header\n' "$direction" "$start_line" \
        "$occurrence" "$header" >>"$flow_dir/xfer-headers.txt"
}

# The same reading against a pattern, for a header value that carries tokens
# nobody can predict: the dialog identity a consult transfer escapes into its
# Refer-To is made of a Call-ID and a tag the daemon minted. The shape around
# them is what the pin is for, and it is an anchored extended regular
# expression so a value with anything in front of or behind it fails.
#
# The sixth argument picks the message, the way the fifth argument of the
# absence reader does, and defaults to the first. A challenged request arrives
# twice on one dialog, and each copy owes the pin of its own.
xfer_hdr_match_check()
{
    local flow_dir=$1
    local direction=$2
    local start_line=$3
    local header=$4
    local pattern=$5
    local occurrence=${6:-1}
    local lines
    local count

    lines=$(sipp_msg_header_get "$flow_dir" "$direction" "$start_line" "$header" \
        "$occurrence") || return 1
    printf '%s "%s" #%s %s\n' "$direction" "$start_line" "$occurrence" "$lines" \
        >>"$flow_dir/xfer-headers.txt"

    count=$(grep -c . <<<"$lines")
    (( count == 1 )) || return 1
    grep -Eq "^$pattern\$" <<<"$(xfer_hdr_value "$lines")"
}

# The same value over every message a start line chooses: the two NOTIFYs of
# a referral both carry the DUT's Contact, and the single-message readers
# above refuse two lines. CSeq is the control count, as in the absence reader
# above, so a message that dropped the header cannot pass behind one that
# carried it.
xfer_hdr_all_check()
{
    local flow_dir=$1
    local direction=$2
    local start_line=$3
    local header=$4
    local expected=$5
    local control
    local lines
    local line
    local total
    local seen=0

    control=$(sipp_msg_header_get "$flow_dir" "$direction" "$start_line" CSeq) || return 1
    total=$(grep -c . <<<"$control")
    (( total >= 1 )) || return 1

    lines=$(sipp_msg_header_get "$flow_dir" "$direction" "$start_line" "$header") || return 1
    printf '%s "%s" %s\n' "$direction" "$start_line" "$lines" >>"$flow_dir/xfer-headers.txt"

    while IFS= read -r line; do
        [[ -n $line ]] || continue
        [[ $(xfer_hdr_value "$line") = "$expected" ]] || return 1
        seen=$((seen + 1))
    done <<<"$lines"

    (( seen == total ))
}

# The whole request line of one message, which no other reader answers: the
# readers above choose a message by a prefix of that line and then read its
# headers, so the tail of the line itself is never asserted. The child INVITE
# of a referral needs it: its request URI is the Refer-To stripped of the URI
# headers behind the '?', and a child that carried them through would be chosen
# by the same prefix and pass every header pin (RFC 3515 2.4.2).
xfer_request_line_check()
{
    local flow_dir=$1
    local direction=$2
    local start_line=$3
    local expected=$4
    local line

    line=$(awk -v want="$direction" -v start="$start_line" '
        /(UDP|TCP|TLS) message received/ { dir = "received"; first = 1; next }
        /(UDP|TCP|TLS) message sent/ { dir = "sent"; first = 1; next }
        first {
            if ($0 ~ /^[[:space:]]*$/)
                next
            line = $0
            gsub(/\r/, "", line)
            first = 0
            if (dir == want && index(line, start) == 1) {
                print line
                exit
            }
        }
    ' "$flow_dir/sipp-messages.log" 2>/dev/null)

    printf '%s request line: %s\n' "$direction" "$line" >>"$flow_dir/xfer-headers.txt"
    [[ $line = "$expected" ]]
}

# When one message crossed the wire, as SIPp stamped it. Every message of the
# log is preceded by a rule and a timestamp ("--- YYYY-MM-DD HH:MM:SS.ffffff"),
# and the two SIPp runs of the two-line flow write two logs, so an ordering
# question between them is answered by those stamps and not by file order. The
# format is fixed width and zero padded, so the comparison is a string one.
xfer_msg_time_get()
{
    local flow_dir=$1
    local direction=$2
    local start_line=$3

    awk -v want="$direction" -v start="$start_line" '
        /^-+ [0-9]{4}-[0-9]{2}-[0-9]{2} / { stamp = $2 " " $3; next }
        /(UDP|TCP|TLS) message received/ { dir = "received"; head = 1; next }
        /(UDP|TCP|TLS) message sent/ { dir = "sent"; head = 1; next }
        head {
            if ($0 ~ /^[[:space:]]*$/)
                next
            line = $0
            gsub(/\r/, "", line)
            head = 0
            if (dir == want && index(line, start) == 1 && stamp != "") {
                print stamp
                exit
            }
            next
        }
    ' "$flow_dir/sipp-messages.log" 2>/dev/null
}

# The verdict BYE grace (call.h CALL_XFER_BYE_GRACE_MS): against a peer that
# sends no BYE of its own, the DUT's BYE leaves one grace behind its answer to
# the final NOTIFY. Both stamps are the DUT's own sends as SIPp stamped their
# arrival, "received" in SIPp's log, so the DUT's handling of the NOTIFY is
# not in the delta. The answer is the last 200 the DUT sent BEFORE its first
# BYE, read in one pass, because a consult flow's peer re-INVITEs the
# replaced leg long after the referee ended and its 200 would otherwise be
# the last one. The floor sits below the constant, because uloop rounds its
# timeout down to whole milliseconds and the two stamps come from SIPp's
# clock; the ceiling catches a BYE that came from a guard instead of the
# grace. A run that crosses midnight between the two stamps reads the
# difference against the next day.
xfer_grace_detail=''
xfer_grace_check()
{
    local flow_dir=$1
    local stamps
    local answered
    local bye
    local delta

    stamps=$(awk '
        /^-+ [0-9]{4}-[0-9]{2}-[0-9]{2} / { stamp = $2 " " $3; next }
        /(UDP|TCP|TLS) message received/ { dir = "received"; head = 1; next }
        /(UDP|TCP|TLS) message sent/ { dir = "sent"; head = 1; next }
        head {
            if ($0 ~ /^[[:space:]]*$/)
                next
            line = $0
            gsub(/\r/, "", line)
            head = 0
            if (dir != "received" || stamp == "")
                next
            if (index(line, "BYE ") == 1 && bye == "")
                bye = stamp
            else if (index(line, "SIP/2.0 200") == 1 && bye == "")
                answered = stamp
            next
        }
        END { print answered; print bye }
    ' "$flow_dir/sipp-messages.log" 2>/dev/null)
    answered=${stamps%%$'\n'*}
    bye=${stamps#*$'\n'}
    if [[ -z $answered || -z $bye ]]; then
        xfer_grace_detail="the verdict answer or the DUT BYE is missing from the log"
        return 1
    fi
    delta=$(awk -v a="$answered" -v b="$bye" '
        function ms(s,    p, t) {
            split(s, p, " ")
            split(p[2], t, /[:.]/)
            return ((t[1] * 60 + t[2]) * 60 + t[3]) * 1000 + t[4] / 1000
        }
        BEGIN {
            d = ms(b) - ms(a)
            if (d < 0)
                d += 86400000
            printf "%d", d
        }')
    printf 'verdict answered %s\nBYE sent %s\ndelta %s ms\n' \
        "$answered" "$bye" "$delta" >>"$flow_dir/xfer-grace.txt"
    if (( delta < 900 || delta >= 4000 )); then
        xfer_grace_detail="the DUT BYE left $delta ms behind its verdict answer, expected the 1000 ms grace"
        return 1
    fi

    return 0
}

# How many dialogs the peer was sent one method in, or how many requests of
# that method one dialog carried. The third argument selects which of the two
# it answers. The on-hook consult owes exactly one BYE, on the referee dialog,
# and the ordinary conference hangup owes two; no per-message reader can answer
# "how many", because the readers above all stop at the first message they
# match.
#
# It counts header values and not messages. A request over UDP is retransmitted
# until it is answered, and a run that counted messages would turn a single
# retransmit into a third BYE and the flow red. One BYE per dialog is what the
# rule is about (RFC 3261 15.1.1), so each Call-ID counts once however many
# copies of the request carried it.
#
# The third argument names the header that tells two requests apart, and it
# defaults to the Call-ID the dialog count needs. CSeq answers the other
# question: how many requests of that method one dialog carried. A challenged
# request is sent again on the next number (RFC 3261 22.2), so the numbers
# count the requests and a retransmission of either keeps its own.
xfer_request_count()
{
    local flow_dir=$1
    local method=$2
    local key=${3:-Call-ID}

    awk -v want="$method " -v key="$key: " '
        /(UDP|TCP|TLS) message received/ { dir = "received"; first = 1; next }
        /(UDP|TCP|TLS) message sent/ { dir = "sent"; first = 1; next }
        first {
            if ($0 ~ /^[[:space:]]*$/)
                next
            line = $0
            gsub(/\r/, "", line)
            first = 0
            inmsg = (dir == "received" && index(line, want) == 1)
            next
        }
        inmsg && tolower(substr($0, 1, length(key))) == tolower(key) {
            id = substr($0, length(key) + 1)
            gsub(/[\r[:space:]]/, "", id)
            if (!(id in seen)) {
                seen[id] = 1
                n++
            }
            inmsg = 0
            next
        }
        END { print n + 0 }
    ' "$flow_dir/sipp-messages.log" 2>/dev/null
}

xfer_config_write()
{
    local config_dir=$1
    local channels=${2:-1}

    config_write "$config_dir" "$sipp_port" "$daemon_port" "" "" "" "" "" "" "" \
        "$channels" || return 1

    # still part of the channel section config_write closed with: the three
    # transfer codes are channel options and the flows dial their shipped
    # defaults, so writing them here pins the defaults the wire then carries
    {
        printf "\toption blind_transfer_exten '%s'\n" '*072'
        printf "\toption attended_transfer_exten '%s'\n" '*073'
        printf "\toption consult_transfer_exten '%s'\n" '*074'
    } >>"$config_dir/voip"
}

# The held leg the transfer code is dialled from: a hook flash on a connected
# call holds it and opens the second dial
# tone, and the hold re-INVITE the flash owes is answered by the scenario.
xfer_hold_take()
{
    local flow_dir=$1

    flash_send || return 1
    status_wait "$flow_dir" HOLD_DIAL 15 hold
}

# The transfer code and its target, injected as digits through the dial verb.
# The verb only seizes a line that is on hook, so a line already in HOLD_DIAL
# takes the digits into the state it is in, which is the subscriber's own path.
xfer_code_dial()
{
    local line=$1
    local digits=$2

    verb_send dial "{\"line\":$line,\"number\":\"$digits\"}"
}

# The connected call every transferor flow starts from: the DUT places it, the
# scenario answers it, and the stream reaches the port the answer named.
xfer_call_place()
{
    local flow_dir=$1
    local name=$2
    # Where the SIPp logs of this call live, which is the flow directory for
    # every flow but the two-line one: that flow runs two SIPp instances and
    # keeps a directory per stage, and the media port is read off the log of
    # the stage that answered the call.
    local sipp_dir_at=${3:-$flow_dir}

    if ! outbound_dial_start "$flow_dir"; then
        result_print FAIL "$name" "off-hook dial setup failed"
        return 1
    fi
    if ! status_wait "$flow_dir" CONNECTED 15 connected; then
        result_print FAIL "$name" "the daemon did not reach CONNECTED"
        return 1
    fi
    if ! media_port_wait "$sipp_dir_at" 5; then
        result_print FAIL "$name" "stream 0 did not use the SDP media port"
        return 1
    fi

    return 0
}

# The DUT as transferor, one runner for the four shapes. Each one places a
# call, holds it, dials its transfer code and its target, and the scenario
# answers the REFER its own way. The blind peer accepts with 202, notifies the
# progress and the verdict, re-INVITEs the leg with c=0.0.0.0 and sends its
# own BYE. The blind grace peer does the same without the BYE, so the DUT
# must send the BYE one grace behind its answer to the verdict. The attended
# peer completes the referral with a sipfrag 200, and the failing peer
# refuses it with a 486.
#
# What differs is the outcome and what the record says. An ok transfer leaves
# the machine at the second dial tone and the record carrying the transfer key;
# a failed one resumes the held call and the record carries no key at all,
# because nothing took the call away.
xfer_transferor_run()
{
    local name=$1
    local scenario=$2
    local code=$3
    local target=$4
    local outcome=$5
    local subject=$6
    local byes=$7
    local grace=$8
    local end_state=OFFHOOK_DIAL
    local transferred='(.cdr | last | .transferred) == true'
    local detail
    local sent

    if [[ $outcome = fail ]]; then
        end_state=CONNECTED
        transferred='(.cdr | last | .transferred) == null'
    fi

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! xfer_config_write "$config_dir"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    if ! sipp_start "$flow_dir" "$sipp_dir/$scenario.xml" "$sipp_port" \
        "$daemon_port" "$media_port" 2; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! daemon_start "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    if ! xfer_call_place "$flow_dir" "$name"; then
        flow_stop
        return
    fi
    if ! xfer_hold_take "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "the hook flash did not hold the call"
        return
    fi
    if ! xfer_code_dial 0 "$code$target#"; then
        flow_stop
        result_print FAIL "$name" "the transfer code was refused"
        return
    fi
    # The outcome is the FSM state: the ok arm of
    # the pending op is the fresh dial tone, and the fail arm resumes the leg
    # the transfer was going to give away.
    if ! status_wait "$flow_dir" "$end_state" 40 xfer-outcome; then
        flow_stop
        result_print FAIL "$name" \
            "the transfer outcome did not put the machine in $end_state"
        return
    fi
    # A transfer that succeeded ends its own leg, and the scenario waits for
    # that BYE. A transfer that failed handed the call back to the subscriber,
    # so the hook is what ends it and the scenario cannot finish before it.
    if [[ $outcome = fail ]]; then
        hook_set release || true
    fi
    if ! sipp_finish_wait 40; then
        flow_stop
        result_print FAIL "$name" "SIPp did not complete the referral exchange"
        return
    fi
    # The referral on the wire. The target is the one the subscriber collected
    # behind the star code, the referrer is the leg's own identity (RFC 3892 5)
    # and a transfer this side collected as digits carries no Replaces.
    if ! xfer_hdr_check "$flow_dir" received "REFER " Refer-To \
        "<sip:$target@127.0.0.1>"; then
        flow_stop
        result_print FAIL "$name" \
            "the REFER did not carry exactly one Refer-To naming $target"
        return
    fi
    if ! xfer_hdr_check "$flow_dir" received "REFER " Referred-By \
        "<sip:5551000@127.0.0.1>"; then
        flow_stop
        result_print FAIL "$name" "the REFER did not name the referrer"
        return
    fi
    if ! xfer_hdr_absent_check "$flow_dir" received "REFER " Replaces; then
        flow_stop
        result_print FAIL "$name" "the REFER carried a Replaces header"
        return
    fi
    # Contact is mandatory on a REFER (RFC 3515 2.4): the same contact the
    # leg's dialog-forming messages carry, host and port. A DC-SIP capture
    # shows a switch that refuses a REFER without it.
    if ! xfer_hdr_check "$flow_dir" received "REFER " Contact \
        "<sip:5551000@127.0.0.1:$daemon_port>"; then
        flow_stop
        result_print FAIL "$name" "the REFER did not carry our Contact"
        return
    fi
    # How many dialogs the DUT BYEd, counted by Call-ID so a retransmit is not
    # a second: none against a switch that ends the referred leg itself, the
    # one referred leg against a transferee that leaves the BYE to the
    # transferor, and the one hangup of a failed transfer.
    sent=$(xfer_request_count "$flow_dir" BYE)
    if [[ $sent != "$byes" ]]; then
        flow_stop
        result_print FAIL "$name" \
            "the peer was sent a BYE in $sent dialog(s), expected $byes"
        return
    fi
    if [[ $grace = grace ]] && ! xfer_grace_check "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "$xfer_grace_detail"
        return
    fi
    if [[ $outcome != fail ]]; then
        hook_set release || true
    fi
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "the daemon did not return to IDLE"
        return
    fi
    # One record per leg, and its transfer key says whether a transfer took the
    # call away (call.h call_ended transferred).
    if ! cdr_expr_wait "$flow_dir" \
        "(.cdr | length) == 1 and $transferred and (.cdr | last | .answered) == true and (.cdr | last | .transferred_from) == null" \
        15 closed; then
        flow_stop
        result_print FAIL "$name" "the record of the referred leg reads wrong"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 2; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    detail=$sipp_verdict_detail
    flow_stop
    result_print PASS "$name" \
        "$detail, the REFER named $target and the referrer and carried no Replaces, and $subject"
}

# The transfer gesture on the shipped config, which carries no star code at
# all: the base star_codes_local defaults to 0, so the schema code defaults do
# not load and the transfer code is a plain star string to the dial plan
# (star_codes_local). The held call stays held, the string
# reaches the peer as a second INVITE whose request URI carries it whole, the
# second call connects, and no REFER leaves. Under the shipped
# digit_esc_enabled 1 the request URI carries the '*' escaped, as %2A. The
# on-hook then ends both legs.
star_network_run()
{
    local name=star-network
    local code='*072'
    local target=5552000
    # the wire form of $code$target: the shipped escape hides the '*'
    local wire_user="%2A072$target"
    local detail
    local lines
    local count

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    # the config is the one config_write writes, with neither
    # star_codes_local nor a code
    if ! config_write "$config_dir" "$sipp_port" "$daemon_port"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    # two calls on one peer, the held one and the star string, plus the
    # REGISTER of the line, which the scenario answers
    if ! sipp_start "$flow_dir" "$sipp_dir/uas_star_dial.xml" "$sipp_port" \
        "$daemon_port" "$media_port" 3; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! daemon_start "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    if ! xfer_call_place "$flow_dir" "$name"; then
        flow_stop
        return
    fi
    if ! xfer_hold_take "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "the hook flash did not hold the call"
        return
    fi
    if ! xfer_code_dial 0 "$code$target#"; then
        flow_stop
        result_print FAIL "$name" "the star string was refused"
        return
    fi
    # the string is a number to the dial plan, so the second call connects
    # beside the held one
    if ! status_wait "$flow_dir" TWO_CALLS 20 star-dialled; then
        flow_stop
        result_print FAIL "$name" \
            "the star string did not place a second call"
        return
    fi
    # the request line the peer saw carries the whole string escaped, once
    lines=$(sipp_msg_header_get "$flow_dir" received "INVITE sip:$wire_user@" \
        CSeq) || lines=
    count=$(grep -c . <<<"$lines")
    if (( count != 1 )); then
        flow_stop
        result_print FAIL "$name" \
            "the peer saw $count INVITE(s) addressed to $wire_user, expected 1"
        return
    fi
    if sipp_msg_header_get "$flow_dir" received "REFER " CSeq >/dev/null; then
        flow_stop
        result_print FAIL "$name" "a REFER left the DUT"
        return
    fi
    # the on-hook ends the star call and rings the held leg back (the
    # TWO_CALLS RELEASE cell); the seize resumes it and the second on-hook
    # ends it
    hook_set release || true
    if ! status_wait "$flow_dir" HOLDING 10 holding; then
        flow_stop
        result_print FAIL "$name" "the on-hook did not ring the held leg back"
        return
    fi
    hook_set seize || true
    if ! status_wait "$flow_dir" CONNECTED 10 resumed; then
        flow_stop
        result_print FAIL "$name" "the seize did not resume the held leg"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" IDLE 10 idle; then
        flow_stop
        result_print FAIL "$name" "the daemon did not return to IDLE"
        return
    fi
    if ! sipp_finish_wait 40; then
        flow_stop
        result_print FAIL "$name" "SIPp did not complete both calls"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 3; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    detail=$sipp_verdict_detail
    flow_stop
    result_print PASS "$name" \
        "$detail, the star string reached the peer as one INVITE to $wire_user and no REFER left"
}

# The DUT as transferee. The peer calls the DUT, refers it to a target of the
# peer's own, and the child INVITE the DUT places comes back to the same
# process through the out-of-call scenario. Nothing of this reaches the
# machine: the line stays CONNECTED from the answer to the hangup, and what
# moves under it is the two-name contract.
xfer_transferee_run()
{
    local name=$1
    local scenario=$2
    local replaces=$3
    local detail

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! xfer_config_write "$config_dir"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    # The daemon comes up and registers first: this peer is the UAC and it puts
    # its INVITE on the wire as soon as it starts.
    if ! register_prime "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    # One call placed, which is what -m bounds. The -l value is headroom over
    # the two instances this flow holds (the call this peer placed and the
    # referred child the out-of-call scenario answers) and it gates neither of
    # them: -l drives the outgoing generator alone, measured at -l 1.
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/$scenario.xml" "$sipp_port" \
        "$daemon_port" "$media_port" "$sipp_dir/xfer_peer_ooc.xml" 1 3; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! status_wait "$flow_dir" RINGING 10 ringing; then
        flow_stop
        result_print FAIL "$name" "the daemon did not reach RINGING"
        return
    fi
    hook_set seize || true
    if ! status_wait "$flow_dir" CONNECTED 10 connected; then
        flow_stop
        result_print FAIL "$name" "the daemon did not answer into CONNECTED"
        return
    fi
    if ! media_port_wait "$flow_dir" 5; then
        flow_stop
        result_print FAIL "$name" "stream 0 did not use the SDP media port"
        return
    fi
    # The window the two-name contract is read in (call.c calls_status_dump).
    # The child INVITE is out and the peer is ringing it: the parent still
    # holds the name it will hand over and says nothing, the child holds a name
    # of its own and speaks for none, so both legs read as shadows. The machine
    # is told nothing at all, which is why the line is still CONNECTED here.
    if ! status_expr_wait "$flow_dir" \
        '.call[0].state == "CONNECTED" and (.call[0].calls | length) == 2 and ((.call[0].calls | map(select(.slot == 0)))[0] | .ui_slot == 0 and .shadow == true) and ((.call[0].calls | map(select(.slot == 1)))[0] | .ui_slot == 1 and .shadow == true)' \
        20 shadow; then
        flow_stop
        result_print FAIL "$name" \
            "the referred child did not open as a shadow leg beside its parent"
        return
    fi
    # The swap is visible: the child answered, took the name over and speaks
    # for it, and the parent it demoted is the shadow now.
    if ! status_expr_wait "$flow_dir" \
        '.call[0].state == "CONNECTED" and (.call[0].calls | length) == 2 and ((.call[0].calls | map(select(.slot == 1)))[0] | .ui_slot == 0 and (has("shadow") | not) and .state == "active") and ((.call[0].calls | map(select(.slot == 0)))[0] | .shadow == true)' \
        20 swapped; then
        flow_stop
        result_print FAIL "$name" \
            "the child did not take the parent's name over after the swap"
        return
    fi
    # and the transferor's BYE takes the parent away, leaving one leg on the
    # other physical slot answering to the name the first one held
    if ! status_expr_wait "$flow_dir" \
        '.call[0].state == "CONNECTED" and (.call[0].calls | length) == 1 and (.call[0].calls[0] | .slot == 1 and .ui_slot == 0 and (has("shadow") | not) and .state == "active")' \
        20 parent-gone; then
        flow_stop
        result_print FAIL "$name" "the transferred parent did not leave the line"
        return
    fi
    # The hook ends the transferred call, and it has to end before SIPp leaves:
    # a call still up when the last scenario ends is aborted by SIPp with a BYE
    # of its own making, and its record would never close.
    hook_set release || true
    if ! sipp_finish_wait 60; then
        flow_stop
        result_print FAIL "$name" "SIPp did not complete the referral exchange"
        return
    fi
    # The child INVITE, byte for byte. The request URI is the Refer-To stripped
    # of its URI headers, the Referred-By is copied verbatim (RFC 3892 5), and
    # a referral that carried an embedded Replaces promotes exactly one of them
    # beside the Require of RFC 3891 4.
    if ! xfer_request_line_check "$flow_dir" received "INVITE sip:xfer@" \
        "INVITE sip:xfer@127.0.0.1:$sipp_port SIP/2.0"; then
        flow_stop
        result_print FAIL "$name" \
            "the child INVITE did not address the Refer-To stripped of its URI headers"
        return
    fi
    if ! xfer_hdr_check "$flow_dir" received "INVITE sip:xfer@" Referred-By \
        "<sip:sipp@127.0.0.1:$sipp_port>"; then
        flow_stop
        result_print FAIL "$name" "the child INVITE did not carry the Referred-By verbatim"
        return
    fi
    if [[ -n $replaces ]]; then
        if ! xfer_hdr_check "$flow_dir" received "INVITE sip:xfer@" Replaces \
            "$replaces"; then
            flow_stop
            result_print FAIL "$name" \
                "the child INVITE did not promote exactly one Replaces"
            return
        fi
        if ! xfer_hdr_check "$flow_dir" received "INVITE sip:xfer@" Require \
            "replaces"; then
            flow_stop
            result_print FAIL "$name" "the promoted Replaces carried no Require"
            return
        fi
    else
        if ! xfer_hdr_absent_check "$flow_dir" received "INVITE sip:xfer@" Replaces; then
            flow_stop
            result_print FAIL "$name" "the child INVITE carried a Replaces nobody asked for"
            return
        fi
        if ! xfer_hdr_absent_check "$flow_dir" received "INVITE sip:xfer@" Require; then
            flow_stop
            result_print FAIL "$name" "the child INVITE carried a Require nobody asked for"
            return
        fi
    fi
    # Both NOTIFYs of the implicit subscription carry our Contact
    # (RFC 6665 8.6), and so does the 202 that accepted the REFER
    # (RFC 3515 2.4).
    if ! xfer_hdr_all_check "$flow_dir" received "NOTIFY " Contact \
        "<sip:5551000@127.0.0.1:$daemon_port>"; then
        flow_stop
        result_print FAIL "$name" "a transfer NOTIFY did not carry our Contact"
        return
    fi
    if ! xfer_hdr_check "$flow_dir" received "SIP/2.0 202" Contact \
        "<sip:5551000@127.0.0.1:$daemon_port>"; then
        flow_stop
        result_print FAIL "$name" "the 202 did not carry our Contact"
        return
    fi
    if ! status_wait "$flow_dir" IDLE 10 idle; then
        flow_stop
        result_print FAIL "$name" "the daemon did not return to IDLE"
        return
    fi
    # One record per leg, and the child's names the parent's sequence: the
    # correlation cdr.h cdr_call_correlate writes at the one moment both
    # records are open.
    if ! cdr_expr_wait "$flow_dir" \
        '(.cdr | length) == 2 and (.cdr[0].seq) == 1 and (.cdr[1].seq) == 2 and (.cdr[1].transferred_from) == 1 and (.cdr[0].transferred_from) == null and (.cdr[1].answered) == true' \
        20 closed; then
        flow_stop
        result_print FAIL "$name" \
            "the two legs left no correlated pair of records"
        return
    fi
    # One successful instance: SIPp counts the calls of the main scenario, and
    # the referred child belongs to the out-of-call one (measured).
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    detail=$sipp_verdict_detail
    flow_stop
    result_print PASS "$name" \
        "$detail, both legs read as shadows while the child rang, the child took the parent's name over, and its record names the parent's sequence"
}

# The referred child that fails. The same referral, refused by the target: the
# DUT reports the code in its final notification and stays on the call it had,
# which is the whole claim. Nothing of it reaches the machine here either.
xfer_child_fail_run()
{
    local name=xfer-child-fail
    local detail

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! xfer_config_write "$config_dir"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/uac_xfer_child_fail.xml" "$sipp_port" \
        "$daemon_port" "$media_port" "$sipp_dir/xfer_busy_ooc.xml" 1 3; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! status_wait "$flow_dir" RINGING 10 ringing; then
        flow_stop
        result_print FAIL "$name" "the daemon did not reach RINGING"
        return
    fi
    hook_set seize || true
    if ! status_wait "$flow_dir" CONNECTED 10 connected; then
        flow_stop
        result_print FAIL "$name" "the daemon did not answer into CONNECTED"
        return
    fi
    if ! status_expr_wait "$flow_dir" \
        '.call[0].state == "CONNECTED" and (.call[0].calls | length) == 2 and ((.call[0].calls | map(select(.slot == 0)))[0] | .ui_slot == 0 and .shadow == true) and ((.call[0].calls | map(select(.slot == 1)))[0] | .ui_slot == 1 and .shadow == true)' \
        20 shadow; then
        flow_stop
        result_print FAIL "$name" \
            "the referred child did not open as a shadow leg beside its parent"
        return
    fi
    # The old call lives. The child is gone and the leg that never stopped
    # holding the name speaks for it again: one leg, on its own physical slot,
    # no shadow, still answered.
    if ! status_expr_wait "$flow_dir" \
        '.call[0].state == "CONNECTED" and (.call[0].calls | length) == 1 and (.call[0].calls[0] | .slot == 0 and .ui_slot == 0 and (has("shadow") | not) and .state == "active")' \
        20 survived; then
        flow_stop
        result_print FAIL "$name" \
            "the refused referral did not leave the original call in place"
        return
    fi
    if ! sipp_finish_wait 60; then
        flow_stop
        result_print FAIL "$name" "SIPp did not complete the referral exchange"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" IDLE 10 idle; then
        flow_stop
        result_print FAIL "$name" "the daemon did not return to IDLE"
        return
    fi
    # Two records all the same: the child opened one and it closes unanswered
    # and correlated, while the call the referral could not move keeps its own
    # and carries no transfer key, because nothing took it away.
    if ! cdr_expr_wait "$flow_dir" \
        '(.cdr | length) == 2 and (.cdr[0].seq) == 1 and (.cdr[0].answered) == true and (.cdr[0].transferred) == null and (.cdr[1].seq) == 2 and (.cdr[1].transferred_from) == 1 and (.cdr[1].answered) == false' \
        20 closed; then
        flow_stop
        result_print FAIL "$name" "the refused child left no unanswered correlated record"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    detail=$sipp_verdict_detail
    flow_stop
    result_print PASS "$name" \
        "$detail, the refusal reached the referrer as a sipfrag 486, the original call kept its name and its record carries no transfer key"
}

# The DUT as the target of a completed attended transfer (RFC 3891). The DUT
# places a call the out-of-call peer answers, and the peer then places a
# second one whose Replaces names that dialog. The DUT answers it without
# ringing, moves the name onto it and BYEs the dialog it replaced, and the
# machine is told nothing at all: no incoming offer, no call waiting, and the
# line never leaves CONNECTED.
xfer_replaces_run()
{
    local name=xfer-replaces
    local detail

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! xfer_config_write "$config_dir"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    # The replacing INVITE names the subscriber, so the request URI carries the
    # line's own user and no fallback decides which line serves it.
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/uac_xfer_replaces.xml" "$sipp_port" \
        "$daemon_port" "$media_port" "$sipp_dir/xfer_peer_ooc.xml" 1 3 5551000; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! xfer_call_place "$flow_dir" "$name"; then
        flow_stop
        return
    fi
    # The takeover. The replacement sits on the other physical slot and answers
    # to the name the replaced leg held, the replaced leg is gone, and the line
    # never left CONNECTED on the way.
    if ! status_expr_wait "$flow_dir" \
        '.call[0].state == "CONNECTED" and (.call[0].calls | length) == 1 and (.call[0].calls[0] | .slot == 1 and .ui_slot == 0 and (has("shadow") | not) and .state == "active" and .inbound == true)' \
        30 replaced; then
        flow_stop
        result_print FAIL "$name" \
            "the replacing call did not take the replaced leg's name over"
        return
    fi
    # No ring reached the subscriber. The target role answers a Replaces
    # INVITE by design and bypasses the call waiting policy with it, so neither
    # the ring nor a waiting tone may appear (call.h, the CW bypass).
    if ! stub_check "$flow_dir" '.line[0].ringing == false' no-ring; then
        flow_stop
        result_print FAIL "$name" "the replacing call rang the subscriber"
        return
    fi
    if grep -qF '[fsm] line 0 CALL_INCOMING' "$flow_dir/daemon.log"; then
        flow_stop
        result_print FAIL "$name" "the replacing call reached the machine as an offer"
        return
    fi
    hook_set release || true
    if ! sipp_finish_wait 60; then
        flow_stop
        result_print FAIL "$name" "SIPp did not complete the replacing call"
        return
    fi
    if ! status_wait "$flow_dir" IDLE 10 idle; then
        flow_stop
        result_print FAIL "$name" "the daemon did not return to IDLE"
        return
    fi
    # The replaced dialog was ended by this side (RFC 3891 4): its record
    # closes, and the replacement's names it.
    if ! cdr_expr_wait "$flow_dir" \
        '(.cdr | length) == 2 and (.cdr[0].seq) == 1 and (.cdr[0].inbound) == false and (.cdr[0].answered) == true and (.cdr[1].seq) == 2 and (.cdr[1].inbound) == true and (.cdr[1].answered) == true and (.cdr[1].transferred_from) == 1' \
        20 closed; then
        flow_stop
        result_print FAIL "$name" \
            "the replacement left no answered record naming the replaced one"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    detail=$sipp_verdict_detail
    flow_stop
    result_print PASS "$name" \
        "$detail, the Replaces INVITE was answered without a ring, the replaced dialog was BYEd and the replacement wears the name it took"
}

# The on-hook consult. The peer calls the DUT, the subscriber flashes and dials
# the consult code with a second number, and that second call is placed back
# at the same peer. Going on hook with both calls up refers the held leg to
# the other one.
#
# The machine is fire-and-forget here: it does not wait for the transfer to be
# reported done. The call layer then owns the two legs in the RFC 5589 7.1
# order: the 202 BYEs nothing, the referee is BYEd behind the peer's verdict
# NOTIFY (uac_xfer_onhook.xml fails on a BYE in front of it), and the replaced
# leg is NOT BYEd, because the network ends that dialog with a Replaces-driven
# BYE once the referred party has replaced it. The consulted peer,
# xfer_replaced_ooc.xml, re-INVITEs that leg first, as Asterisk does, and
# requires a 200 sendonly; then it
# plays that BYE and requires the DUT's 200. A DUT that BYEd the replaced leg
# first would fail that scenario.
xfer_onhook_run()
{
    local name=xfer-onhook
    local byes
    local detail

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! xfer_config_write "$config_dir"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    # the request URI names the subscriber, so the dialog's local identity is
    # the line's own and the Referred-By below reads as one
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/uac_xfer_onhook.xml" "$sipp_port" \
        "$daemon_port" "$media_port" "$sipp_dir/xfer_replaced_ooc.xml" 1 3 5551000; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! status_wait "$flow_dir" RINGING 10 ringing; then
        flow_stop
        result_print FAIL "$name" "the daemon did not reach RINGING"
        return
    fi
    hook_set seize || true
    if ! status_wait "$flow_dir" CONNECTED 10 connected; then
        flow_stop
        result_print FAIL "$name" "the daemon did not answer into CONNECTED"
        return
    fi
    if ! xfer_hold_take "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "the hook flash did not hold the call"
        return
    fi
    if ! xfer_code_dial 0 '*0745552000#'; then
        flow_stop
        result_print FAIL "$name" "the consult code was refused"
        return
    fi
    # The consult call is a second call, and the machine knows it: the held leg
    # stays and the line reaches the two-call state.
    if ! status_wait "$flow_dir" TWO_CALLS 30 two-calls; then
        flow_stop
        result_print FAIL "$name" "the consult call did not reach the two-call state"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" IDLE 30 idle; then
        flow_stop
        result_print FAIL "$name" "the on-hook consult did not clear the line"
        return
    fi
    if ! sipp_finish_wait 60; then
        flow_stop
        result_print FAIL "$name" "SIPp did not complete the referral exchange"
        return
    fi
    # The referral, byte for byte. Refer-To names the other call's remote target
    # and carries that call's dialog escaped behind the '?', receiver oriented:
    # to-tag is the peer's tag and from-tag is ours (RFC 3891 3, RFC 3261 25.1).
    # Every bracket expression here names its characters, because a range does
    # not mean one thing across the engines this repository runs patterns
    # through. Measured on this host: [!-~] matches through glibc regcomp under
    # LC_ALL=C and LC_ALL=POSIX and not under en_US.UTF-8, where the range
    # collates; and to a shell pattern [! opens a negated class, not a range at
    # all. A pin that quietly matched nothing would pass a wrong value.
    if ! xfer_hdr_match_check "$flow_dir" received "REFER " Refer-To \
        "<sip:sipp@127\.0\.0\.1:$sipp_port;transport=UDP\?Replaces=[0-9a-zA-Z._-]+%40127\.0\.0\.1%3Bto-tag%3Dsipp-peer-tag%3Bfrom-tag%3D[0-9a-f]+>"; then
        flow_stop
        result_print FAIL "$name" \
            "the REFER did not carry the consulted dialog escaped into its Refer-To"
        return
    fi
    # Referred-By is the dialog's local URI (RFC 3892 5), and on a call this
    # side answered that is the URI the caller addressed, port and all.
    if ! xfer_hdr_check "$flow_dir" received "REFER " Referred-By \
        "<sip:5551000@127.0.0.1:$daemon_port>"; then
        flow_stop
        result_print FAIL "$name" "the REFER did not name the referrer"
        return
    fi
    if ! xfer_hdr_check "$flow_dir" received "REFER " Contact \
        "<sip:5551000@127.0.0.1:$daemon_port>"; then
        flow_stop
        result_print FAIL "$name" "the REFER did not carry our Contact"
        return
    fi
    # One dialog was BYEd by the DUT, the referee's, counted by Call-ID so a
    # retransmit is not a second. The replaced dialog was ended by the
    # consulted peer and answered 200, which its scenario verdict below pins:
    # a DUT BYE on that dialog lands as an unexpected message there.
    byes=$(xfer_request_count "$flow_dir" BYE)
    if [[ $byes != 1 ]]; then
        flow_stop
        result_print FAIL "$name" \
            "the peer was sent a BYE in $byes dialog(s), expected the referee alone"
        return
    fi
    # and that BYE left one grace behind the DUT's answer to the verdict
    if ! xfer_grace_check "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "$xfer_grace_detail"
        return
    fi
    # both records close: the referee at its BYE, the replaced leg at the
    # peer's BYE, and both carry the transfer key
    if ! cdr_expr_wait "$flow_dir" '(.cdr | length) == 2 and ([.cdr[] | select(.in_progress)] | length) == 0 and ([.cdr[] | select(.transferred == true)] | length) == 2' \
        20 closed; then
        flow_stop
        result_print FAIL "$name" \
            "the two legs did not both leave a closed record carrying the transfer key"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    detail=$sipp_verdict_detail
    flow_stop
    result_print PASS "$name" \
        "$detail, the REFER carried the consulted dialog escaped and receiver oriented, the referee alone was BYEd behind the verdict NOTIFY, and the replaced leg answered the peer's re-INVITE and BYE"
}

# The on-hook transfer of a ringing second call. The peer calls the DUT, the
# subscriber flashes and dials a second number, that call rings at the
# consulted peer and is never answered, and the subscriber goes on hook while
# it rings. The DUT then cancels the ringing leg and refers the held leg blind
# to the number dialled, and the line is idle: no recall ring.
#
# The gate under test is the configured one, local_conf_enabled 2 with the
# stub's drop flash service (the read-back default) and the held leg inbound
# with the ringing one outbound, which is the shape of the reported gesture.
# The armed *074 form of the same on-hook is pinned in test_fsm and needs no
# second flow.
xfer_onhook_blind_run()
{
    local name=xfer-onhook-blind
    local byes
    local cancels
    local detail

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! xfer_config_write "$config_dir"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    # the configured half of the gate, appended to the channel section the
    # way conf_config_write appends it; the drop service is the stub default
    printf "\toption local_conf_enabled '%u'\n" 2 >>"$config_dir/voip"
    capture_start "$flow_dir" "$sipp_port" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/uac_xfer_onhook_blind.xml" "$sipp_port" \
        "$daemon_port" "$media_port" "$sipp_dir/xfer_ringing_ooc.xml" 1 3 5551000; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! status_wait "$flow_dir" RINGING 10 ringing; then
        flow_stop
        result_print FAIL "$name" "the daemon did not reach RINGING"
        return
    fi
    hook_set seize || true
    if ! status_wait "$flow_dir" CONNECTED 10 connected; then
        flow_stop
        result_print FAIL "$name" "the daemon did not answer into CONNECTED"
        return
    fi
    if ! xfer_hold_take "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "the hook flash did not hold the call"
        return
    fi
    # an ordinary second number, no star code: the gesture is the configured
    # gate's own
    if ! xfer_code_dial 0 '5552000#'; then
        flow_stop
        result_print FAIL "$name" "the second number was refused"
        return
    fi
    # the consulted peer rings the second call and never answers it
    if ! status_wait "$flow_dir" PROCEEDING 15 ringing-target; then
        flow_stop
        result_print FAIL "$name" "the second call did not reach PROCEEDING"
        return
    fi
    hook_set release || true
    # IDLE within seconds is the assertion that no recall ring ran: a park
    # would hold the line in HOLDING until the held peer gave up, and this
    # peer waits for a REFER that a park never sends
    if ! status_wait "$flow_dir" IDLE 10 idle; then
        flow_stop
        result_print FAIL "$name" "the on-hook did not clear the line at once"
        return
    fi
    if ! stub_check "$flow_dir" '.line[0].ringing == false' no-recall; then
        flow_stop
        result_print FAIL "$name" "the line rang after the on-hook"
        return
    fi
    if ! sipp_finish_wait 60; then
        flow_stop
        result_print FAIL "$name" "SIPp did not complete the referral exchange"
        return
    fi
    # The referral on the wire: the number dialled, in the account domain, no
    # Replaces, and the referrer's own identity, which on a call this side
    # answered is the URI the caller addressed, port and all (RFC 3892 5).
    if ! xfer_hdr_check "$flow_dir" received "REFER " Refer-To \
        "<sip:5552000@127.0.0.1>"; then
        flow_stop
        result_print FAIL "$name" \
            "the REFER did not carry exactly one Refer-To naming the dialled number"
        return
    fi
    if ! xfer_hdr_absent_check "$flow_dir" received "REFER " Replaces; then
        flow_stop
        result_print FAIL "$name" "the REFER carried a Replaces header"
        return
    fi
    if ! xfer_hdr_check "$flow_dir" received "REFER " Referred-By \
        "<sip:5551000@127.0.0.1:$daemon_port>"; then
        flow_stop
        result_print FAIL "$name" "the REFER did not name the referrer"
        return
    fi
    if ! xfer_hdr_check "$flow_dir" received "REFER " Contact \
        "<sip:5551000@127.0.0.1:$daemon_port>"; then
        flow_stop
        result_print FAIL "$name" "the REFER did not carry our Contact"
        return
    fi
    # one CANCEL, on the ringing dialog, and one BYE, on the referee's; both
    # counted by Call-ID so a retransmit is not a second
    cancels=$(xfer_request_count "$flow_dir" CANCEL)
    if [[ $cancels != 1 ]]; then
        flow_stop
        result_print FAIL "$name" \
            "the peer was sent a CANCEL in $cancels dialog(s), expected the ringing one"
        return
    fi
    byes=$(xfer_request_count "$flow_dir" BYE)
    if [[ $byes != 1 ]]; then
        flow_stop
        result_print FAIL "$name" \
            "the peer was sent a BYE in $byes dialog(s), expected the referee alone"
        return
    fi
    # and that BYE left one grace behind the DUT's answer to the verdict
    if ! xfer_grace_check "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "$xfer_grace_detail"
        return
    fi
    # two records: the referred inbound call carries the transfer key, the
    # cancelled outbound one was never answered
    if ! cdr_expr_wait "$flow_dir" \
        '(.cdr | length) == 2 and ([.cdr[] | select(.in_progress)] | length) == 0 and ([.cdr[] | select(.inbound == true and .answered == true and .transferred == true)] | length) == 1 and ([.cdr[] | select(.inbound == false and .answered == false)] | length) == 1' \
        20 closed; then
        flow_stop
        result_print FAIL "$name" \
            "the two legs did not leave one transferred record and one unanswered one"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    detail=$sipp_verdict_detail
    flow_stop
    result_print PASS "$name" \
        "$detail, the ringing call was cancelled, the held leg was referred blind to the dialled number with no Replaces, and the line idled with no recall ring"
}

# The two-line transfer, the software half of the on-unit two-line transfer.
# One daemon carries two subscribers on one signalling port, the daemon routes
# by the user part of the request URI, and the first subscriber transfers the
# call it is on to the second subscriber's number through the attended star
# code.
#
# Why attended and not blind. Both put the same REFER on the wire; they differ
# in what the transferor does with the answer. A blind transferor BYEs its leg
# at the 202 and never learns whether the referral was carried out, so the
# completion would be unobservable on this side. The attended shape makes the
# peer say so with a sipfrag 200, and the transferor's outcome then depends on
# it, which is what turns the second half of this flow into an assertion
# instead of a hope. The blind shape is pinned on its own in xfer-blind.
#
# The completion is caused by the referral, and the two SIPp runs are what make
# that true rather than merely likely. The peer answers the transferor in one
# run and places the referred call in a second one, and the second run is
# started only after the harness has read the REFER out of the first run's log
# and taken the target out of its Refer-To. A DUT that sent no REFER never
# reaches the second stage; a DUT that named the wrong target sends this peer
# at the wrong number and the second line never rings. Two sequential runs, not
# two concurrent ones: the transferor exchange is over before the referred call
# begins, so nothing here depends on a pause or on which process wakes first.
xfer_two_line_run()
{
    local name=xfer-two-line
    local target
    local refer_at
    local invite_at
    local detail

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! xfer_config_write "$config_dir" 2; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    # The second subscriber, on the same signalling port. Two accounts that ask
    # for one local port share one socket (src/proto/sip/transport.c
    # sip_sock_open), and an inbound request is routed by the user part of its
    # request URI (src/proto/sip/account.c account_for_request), so the peer
    # reaches either line at one address and the transfer target is an ordinary
    # number.
    {
        printf '%s\n' "config channel 'channel2'"
        printf '\toption id %s\n' "'1'"
        printf '\toption enabled %s\n' "'1'"
        printf '\toption sip_username %s\n' "'5551001'"
        printf '\toption sip_proxy %s\n' "'127.0.0.1'"
        printf "\toption sip_port '%u'\n" "$sipp_port"
        printf '\toption sip_outbound_proxy %s\n' "'127.0.0.1'"
        printf "\toption sip_outbound_proxy_port '%u'\n" "$sipp_port"
        printf '\toption sip_domain %s\n' "'127.0.0.1'"
        printf '\toption sip_registrar %s\n' "'127.0.0.1'"
        printf "\toption sip_registrar_port '%u'\n" "$sipp_port"
        printf "\toption sip_local_port '%u'\n" "$daemon_port"
        printf '\toption sip_registration_expiry %s\n' "'3600'"
        printf '\toption rtp_local_port_min %s\n' "'24010'"
        printf '\toption rtp_local_port_max %s\n' "'24018'"
    } >>"$config_dir/voip"
    capture_start "$flow_dir" "$sipp_port" || true
    # Each stage keeps its own SIPp artifacts. The two runs would otherwise
    # write one set of log paths and the second would take the first's evidence
    # away, which is also the evidence the gate below reads.
    mkdir -p "$flow_dir/stage1" "$flow_dir/stage2" || {
        flow_stop
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    # Stage one, the transferor and its peer. Three instances: the call, and
    # the REGISTER of each subscriber, which the scenario answers. Both
    # bindings hold through stage two, which calls the second line.
    if ! sipp_start "$flow_dir/stage1" "$sipp_dir/uas_xfer_attended.xml" "$sipp_port" \
        "$daemon_port" "$media_port" 3; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! daemon_start "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    if ! registration_wait "$flow_dir" 2; then
        flow_stop
        result_print FAIL "$name" "the two lines did not register"
        return
    fi
    if ! xfer_call_place "$flow_dir" "$name" "$flow_dir/stage1"; then
        flow_stop
        return
    fi
    if ! xfer_hold_take "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "the hook flash did not hold the call"
        return
    fi
    if ! xfer_code_dial 0 '*0735551001#'; then
        flow_stop
        result_print FAIL "$name" "the transfer code was refused"
        return
    fi
    if ! status_wait "$flow_dir" OFFHOOK_DIAL 40 xfer-ok; then
        flow_stop
        result_print FAIL "$name" \
            "the transfer outcome did not reach the first subscriber's machine"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" IDLE 10 line0-idle; then
        flow_stop
        result_print FAIL "$name" "the first line did not return to IDLE"
        return
    fi
    if ! sipp_finish_wait 40; then
        flow_stop
        result_print FAIL "$name" "SIPp did not complete the referral exchange"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir/stage1" 3; then
        flow_stop
        result_print FAIL "$name" "stage one: $sipp_verdict_detail"
        return
    fi
    detail=$sipp_verdict_detail
    # The referral named the other subscriber, byte for byte.
    if ! xfer_hdr_check "$flow_dir/stage1" received "REFER " Refer-To \
        "<sip:5551001@127.0.0.1>"; then
        flow_stop
        result_print FAIL "$name" "the REFER did not name the second subscriber"
        return
    fi
    if ! xfer_hdr_check "$flow_dir/stage1" received "REFER " Contact \
        "<sip:5551000@127.0.0.1:$daemon_port>"; then
        flow_stop
        result_print FAIL "$name" "the REFER did not carry our Contact"
        return
    fi
    # And the target of stage two comes out of that very header. Nothing else
    # tells this peer where to call, so the referred call is the referral being
    # carried out and not a call the harness placed on its own account.
    target=$(sipp_msg_header_get "$flow_dir/stage1" received "REFER " Refer-To |
        sed -n 's|^[^:]*:[[:space:]]*<sip:\([0-9]\{1,\}\)@.*|\1|p')
    if [[ -z $target ]]; then
        flow_stop
        result_print FAIL "$name" "no dialable target could be read out of the Refer-To"
        return
    fi
    # Stage two: the peer carries the referral out by calling that number, and
    # the number reaches the other line of the same daemon.
    if ! sipp_uac_start "$flow_dir/stage2" "$sipp_dir/uac_xfer_two_line.xml" "$sipp_port" \
        "$daemon_port" "$media_port" "$script_dir/register_ooc.xml" 1 1 "$target"; then
        flow_stop
        result_print FAIL "$name" "the referred call could not be placed"
        return
    fi
    if ! status_expr_wait "$flow_dir" '.call[1].state == "RINGING"' 30 line1-ringing; then
        flow_stop
        result_print FAIL "$name" \
            "the referred call to $target did not ring the second line"
        return
    fi
    hook_set seize 1 || true
    if ! status_expr_wait "$flow_dir" \
        '.call[0].state == "IDLE" and .call[1].state == "CONNECTED" and (.call[1].calls | length) == 1 and (.call[1].calls[0] | .slot == 0 and .ui_slot == 0 and .inbound == true and .state == "active")' \
        15 both-lines; then
        flow_stop
        result_print FAIL "$name" \
            "the two lines did not end up idle and connected"
        return
    fi
    hook_set release 1 || true
    if ! sipp_finish_wait 40; then
        flow_stop
        result_print FAIL "$name" "SIPp did not complete the referred call"
        return
    fi
    if ! status_expr_wait "$flow_dir" '.call[1].state == "IDLE"' 15 line1-idle; then
        flow_stop
        result_print FAIL "$name" "the second line did not return to IDLE"
        return
    fi
    # The order is on the wire too. The gate above makes it so by construction,
    # and this reads it back off the two runs' own timestamps: the REFER stage
    # one received is stamped before the INVITE stage two sent to the target.
    refer_at=$(xfer_msg_time_get "$flow_dir/stage1" received "REFER ")
    invite_at=$(xfer_msg_time_get "$flow_dir/stage2" sent "INVITE sip:$target@")
    printf 'REFER received %s, referred INVITE sent %s\n' "$refer_at" "$invite_at" \
        >"$flow_dir/xfer-order.txt"
    if [[ -z $refer_at || -z $invite_at || ! $refer_at < $invite_at ]]; then
        flow_stop
        result_print FAIL "$name" \
            "the referred INVITE does not follow the REFER: $(cat "$flow_dir/xfer-order.txt")"
        return
    fi
    # One record per line. The transferred leg carries the transfer key. The
    # second line's record carries none, and correctly: line 1 is the target of
    # a fresh INVITE, not a transferee child, so no leg of it took a call over
    # from another and nothing correlates it (cdr.h cdr_call_correlate).
    if ! cdr_expr_wait "$flow_dir" \
        '(.cdr | length) == 2 and ([.cdr[] | select(.line == 0)] | length) == 1 and ([.cdr[] | select(.line == 1)] | length) == 1 and ([.cdr[] | select(.line == 0)][0] | .transferred == true and .answered == true) and ([.cdr[] | select(.line == 1)][0] | .inbound == true and .answered == true and (has("transferred") | not) and (has("transferred_from") | not))' \
        20 closed; then
        flow_stop
        result_print FAIL "$name" \
            "the two lines did not leave one transferred record and one answered one"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir/stage2" 1; then
        flow_stop
        result_print FAIL "$name" "stage two: $sipp_verdict_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "stage one $detail, the first subscriber transferred its call to $target, the peer carried that referral out by calling it, and both lines ended where they should"
}

# -------------------------------------------------------- conference flows

# the flash service is the region's, not a channel row: the flows hand it to
# daemon_start as the stub's read-back lever
conf_config_write()
{
    local config_dir=$1
    local local_conf=$2

    config_write "$config_dir" "$sipp_port" "$daemon_port" || return 1
    printf "\toption local_conf_enabled '%u'\n" "$local_conf" \
        >>"$config_dir/voip"
}

# ooc is the out-of-call peer that answers the second, outbound leg. The
# default ends when the DUT ends it; the on-hook transfer flow passes the
# consulted peer that ends the dialog itself (xfer_replaced_ooc).
conf_incoming_start()
{
    local name=$1
    local scenario=$2
    local local_conf=$3
    local flash_mode=$4
    local ooc=${5:-xfer_peer_ooc}

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || return 1
    config_dir=$flow_dir/config
    conf_config_write "$config_dir" "$local_conf" || return 1
    capture_start "$flow_dir" "$sipp_port" || true
    register_prime "$flow_dir" "$config_dir" "$name" 1 3600 ok "$flash_mode" ||
        return 1
    sipp_uac_start "$flow_dir" "$sipp_dir/$scenario.xml" "$sipp_port" \
        "$daemon_port" "$media_port" "$sipp_dir/$ooc.xml" 1 3 \
        5551000
}

conf_first_answer()
{
    local flow_dir=$1

    status_wait "$flow_dir" RINGING 10 ringing || return 1
    hook_set seize || return 1
    status_wait "$flow_dir" CONNECTED 10 connected
}

conf_second_place()
{
    local flow_dir=$1

    flash_send || return 1
    status_wait "$flow_dir" HOLD_DIAL 15 hold || return 1
    xfer_code_dial 0 '5552000#' || return 1
    status_wait "$flow_dir" TWO_CALLS 30 two-calls
}

# The join, and the directions it leaves on the two streams and the two calls.
# The two expressions are empty for the sendrecv pair an ordinary resume earns.
# A peer that answers the resume with a hold leaves the resumed leg recvonly,
# so the RFC 2543 hold flow names that pair instead and reads the sendrecv
# pair once the offerless exchange behind it has run.
conf_join_take()
{
    local flow_dir=$1
    local stream_dirs=${2:-}
    local call_dirs=${3:-}

    conf_failure_detail=
    if [[ -z $stream_dirs ]]; then
        stream_dirs='.line[0].stream[0].direction == 3 and
         .line[0].stream[1].direction == 3'
    fi
    if [[ -z $call_dirs ]]; then
        call_dirs='([.call[0].calls[] | select(.direction == "sendrecv")] |
          length) == 2'
    fi
    flash_send || {
        conf_failure_detail="the conference flash was refused"
        return 1
    }
    status_wait "$flow_dir" CONF 20 conference || {
        conf_failure_detail="the line did not reach CONF"
        return 1
    }
    stub_wait "$flow_dir" '.line[0].conf == 1' 5 conf-mark || {
        conf_failure_detail="the stub conference mark did not appear"
        return 1
    }
    stub_wait "$flow_dir" \
        ".line[0].stream[0].running == true and
         .line[0].stream[1].running == true and
         $stream_dirs" \
        5 conf-streams || {
        conf_failure_detail="both conference streams did not run the "
        conf_failure_detail+="directions the join owes"
        return 1
    }
    status_expr_check "$flow_dir" \
        ".call[0].conf == true and
         (.call[0].calls | length) == 2 and
         $call_dirs" \
        conf-status || {
        conf_failure_detail="status did not report the conference and "
        conf_failure_detail+="the two calls the join owes"
        return 1
    }
}

conf_joined_start()
{
    local name=$1
    local scenario=$2
    local local_conf=${3:-1}
    local ooc=${4:-xfer_peer_ooc}
    # Handed to conf_join_take, which states what an empty pair selects.
    local stream_dirs=${5:-}
    local call_dirs=${6:-}

    conf_failure_detail=
    conf_incoming_start "$name" "$scenario" "$local_conf" 2 "$ooc" || {
        conf_failure_detail="startup failed"
        return 1
    }
    conf_first_answer "$flow_dir" || {
        conf_failure_detail="the inbound call did not reach CONNECTED"
        return 1
    }
    conf_second_place "$flow_dir" || {
        conf_failure_detail="the outbound call did not reach TWO_CALLS"
        return 1
    }
    conf_join_take "$flow_dir" "$stream_dirs" "$call_dirs"
}

conf_remote_cycle_wait()
{
    local flow_dir=$1

    status_expr_wait "$flow_dir" \
        '.call[0].state == "CONF" and .call[0].conf == true and
         ((.call[0].calls | map(select(.slot == 0)))[0].direction) ==
             "recvonly" and
         ((.call[0].calls | map(select(.slot == 1)))[0].direction) ==
             "sendrecv"' \
        10 remote-hold || return 1
    stub_check "$flow_dir" '.line[0].conf == 1' remote-hold-conf || return 1
    status_expr_wait "$flow_dir" \
        '.call[0].state == "CONF" and .call[0].conf == true and
         ([.call[0].calls[] | select(.direction == "sendrecv")] |
          length) == 2' \
        10 remote-resume || return 1
    stub_check "$flow_dir" '.line[0].conf == 1' remote-resume-conf
}

conf_closed_wait()
{
    local flow_dir=$1

    status_expr_wait "$flow_dir" \
        '.call[0].state == "IDLE" and (.call[0].calls | length) == 0' \
        15 drained || return 1
    cdr_expr_wait "$flow_dir" \
        '(.cdr | length) == 2 and
         ([.cdr[] | select(.in_progress)] | length) == 0 and
         ([.cdr[] | select(.conferenced == true)] | length) == 2' \
        20 closed
}

conf_join_run()
{
    local name=conf-join
    local byes
    local detail

    if ! conf_joined_start "$name" uac_conf_onhook_xfer; then
        flow_stop
        result_print FAIL "$name" "$conf_failure_detail"
        return
    fi
    if ! conf_remote_cycle_wait "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" \
            "the remote hold or resume changed the conference or the other leg"
        return
    fi
    hook_set release || true
    if ! sipp_finish_wait 60; then
        flow_stop
        result_print FAIL "$name" "SIPp did not complete both BYE exchanges"
        return
    fi
    byes=$(xfer_request_count "$flow_dir" BYE)
    if [[ $byes != 2 ]]; then
        flow_stop
        detail="the peer received BYE in $byes dialogs, expected both "
        detail+="conference legs"
        result_print FAIL "$name" "$detail"
        return
    fi
    if ! conf_closed_wait "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" \
            "both conference records did not close with the conference mark"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    detail="$sipp_verdict_detail, join, remote hold and resume, both BYEs, "
    detail+="and both conference records passed"
    flow_stop
    result_print PASS "$name" "$detail"
}

# The RFC 2543 hold sequence of a field switch against a conference join. The
# peer answers the resume the join owes with the stale hold SDP, so the resumed
# leg reaches CONF recvonly, and the INVITE with no SDP behind it is what puts
# both calls back on sendrecv.
conf_legacy_hold_run()
{
    local name=conf-legacy-hold
    local detail
    local stream_dirs='.line[0].stream[0].direction == 2 and
         .line[0].stream[1].direction == 3'
    local call_dirs='([.call[0].calls[] | select(.direction == "recvonly")] |
          length) == 1 and
         ([.call[0].calls[] | select(.direction == "sendrecv")] |
          length) == 1'

    if ! conf_joined_start "$name" uac_legacy_hold_conf 1 xfer_peer_ooc \
        "$stream_dirs" "$call_dirs"; then
        flow_stop
        result_print FAIL "$name" "$conf_failure_detail"
        return
    fi
    if ! status_expr_wait "$flow_dir" \
        '.call[0].state == "CONF" and .call[0].conf == true and
         ([.call[0].calls[] | select(.direction == "sendrecv")] |
          length) == 2' \
        20 legacy-conf-resumed; then
        flow_stop
        result_print FAIL "$name" \
            "the offerless exchange did not put both conference calls on sendrecv"
        return
    fi
    if ! stub_check "$flow_dir" \
        '.line[0].conf == 1 and
         .line[0].stream[0].direction == 3 and
         .line[0].stream[1].direction == 3' legacy-conf-streams; then
        flow_stop
        result_print FAIL "$name" \
            "the conference streams did not follow the offerless answer"
        return
    fi
    hook_set release || true
    if ! sipp_finish_wait 60; then
        flow_stop
        result_print FAIL "$name" "SIPp did not complete the BYE exchange"
        return
    fi
    if ! conf_closed_wait "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" \
            "both conference records did not close with the conference mark"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    detail="$sipp_verdict_detail, the stale hold answer reached CONF recvonly, "
    detail+="the offerless exchange put both calls back on sendrecv, and both "
    detail+="conference records closed marked"
    flow_stop
    result_print PASS "$name" "$detail"
}

conf_drop_run()
{
    local name=conf-drop
    local detail

    if ! conf_joined_start "$name" uac_conf_leg; then
        flow_stop
        result_print FAIL "$name" "$conf_failure_detail"
        return
    fi
    flash_send || true
    if ! status_expr_wait "$flow_dir" \
        '.call[0].state == "CONNECTED" and
         (.call[0] | has("conf") | not) and
         (.call[0].calls | length) == 1 and
         .call[0].calls[0].inbound == true and
         .call[0].calls[0].state == "active"' \
        15 survivor; then
        flow_stop
        result_print FAIL "$name" "the conference drop kept the wrong leg"
        return
    fi
    if ! stub_wait "$flow_dir" '.line[0].conf == 0' 5 conf-off; then
        flow_stop
        result_print FAIL "$name" "the conference mark remained after the drop"
        return
    fi
    if ! cdr_expr_wait "$flow_dir" \
        '([.cdr[] | select(.inbound == true and .in_progress == true)] |
          length) == 1' \
        10 survivor-open; then
        flow_stop
        result_print FAIL "$name" \
            "the surviving inbound record ended during the drop"
        return
    fi
    hook_set release || true
    if ! sipp_finish_wait 60 || ! conf_closed_wait "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "the dropped pair did not close cleanly"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    detail="$sipp_verdict_detail, the second leg dropped, the inbound leg "
    detail+="survived, and both records closed marked"
    flow_stop
    result_print PASS "$name" "$detail"
}

conf_sdp_sequence_check()
{
    local flow_dir=$1

    awk '
        function finish() {
            if (invite && id != "" && media && dir == "")
                dir = "sendrecv"
            if (invite && id != "" && dir != "")
                seq[id] = seq[id] (seq[id] == "" ? "" : " ") dir
            invite = 0
            id = ""
            dir = ""
            media = 0
        }
        /(UDP|TCP|TLS) message received/ {
            finish(); received = 1; first = 1; next
        }
        /(UDP|TCP|TLS) message sent/ { finish(); received = 0; first = 1; next }
        first && /^[[:space:]]*$/ { next }
        first {
            line = $0
            gsub(/\r/, "", line)
            invite = received && line ~ /^INVITE /
            first = 0
            next
        }
        invite && tolower(substr($0, 1, 9)) == "call-id: " {
            id = substr($0, 10)
            gsub(/[\r[:space:]]/, "", id)
            next
        }
        invite && /^m=/ { media = 1 }
        invite && /^a=(sendonly|sendrecv|recvonly|inactive)/ {
            dir = substr($0, 3)
            gsub(/\r/, "", dir)
        }
        END {
            finish()
            for (key in seq) {
                print key ": " seq[key]
                if (seq[key] ~ /sendonly sendrecv sendonly/)
                    ok = 1
            }
            exit !ok
        }
    ' "$flow_dir/sipp-messages.log" >"$flow_dir/conf-direction-sequence.txt"
}

conf_split_run()
{
    local name=conf-split
    local detail

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! conf_config_write "$config_dir" 1; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name" 1 3600 ok 1; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    if ! sipp_start "$flow_dir" "$sipp_dir/uas_conf_leg.xml" "$sipp_port" \
        "$daemon_port" "$media_port" 2; then
        flow_stop
        result_print FAIL "$name" "conference peer startup failed"
        return
    fi
    if ! outbound_dial_start "$flow_dir" ||
        ! status_wait "$flow_dir" CONNECTED 15 connected; then
        flow_stop
        result_print FAIL "$name" "the first outbound call did not connect"
        return
    fi
    if ! conf_second_place "$flow_dir" || ! conf_join_take "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "$conf_failure_detail"
        return
    fi
    flash_send || true
    if ! status_expr_wait "$flow_dir" \
        '.call[0].state == "TWO_CALLS" and
         (.call[0] | has("conf") | not) and
         ((.call[0].calls | map(select(.slot == 0)))[0].direction) ==
             "sendonly" and
         ((.call[0].calls | map(select(.slot == 1)))[0].direction) ==
             "sendrecv"' \
        20 split || ! stub_wait "$flow_dir" \
        '.line[0].conf == 0 and
         .line[0].stream[0].direction == 1 and
         .line[0].stream[1].direction == 3' \
        5 split-streams; then
        flow_stop
        result_print FAIL "$name" "the split did not re-hold the resumed leg"
        return
    fi
    if ! conf_sdp_sequence_check "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" \
            "the split leg did not carry sendonly, sendrecv, sendonly"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" HOLDING 15 holding; then
        flow_stop
        result_print FAIL "$name" "on-hook did not park the held leg"
        return
    fi
    hook_set seize || true
    if ! status_wait "$flow_dir" CONNECTED 15 retrieved; then
        flow_stop
        result_print FAIL "$name" "seize did not retrieve the held leg"
        return
    fi
    hook_set release || true
    if ! sipp_finish_wait 60 || ! conf_closed_wait "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "the split calls did not close cleanly"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 2; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    detail="$sipp_verdict_detail, split direction sequence, park, retrieve, "
    detail+="and both marked records passed"
    flow_stop
    result_print PASS "$name" "$detail"
}

# The nonce each challenge of uac_conf_onhook_xfer_auth.xml carries. The two
# differ because the DUT reads a repeated nonce as the password it already
# offered and refused, and answers no second challenge with it
# (src/proto/sip/auth.c, sip_auth_challenge_take). The harness names them so a
# scenario that moved one fails here instead of passing on a credential of the
# other request.
conf_xfer_refer_nonce='uvoice-refer-nonce'
conf_xfer_bye_nonce='uvoice-bye-nonce'

# The credentials one copy of a request carried, read off the message log. The
# scenario fails SIPp on its own check_it, and this reading is what names the
# message and the nonce in the verdict line.
conf_xfer_cred_check()
{
    local flow_dir=$1
    local start_line=$2
    local nonce=$3
    local occurrence=$4

    xfer_hdr_match_check "$flow_dir" received "$start_line" Authorization \
        "Digest .*nonce=\"$nonce\".*" "$occurrence"
}

# The on-hook transfer from a conference, one runner for the plain peer and for
# the one that challenges it. The consulted peer re-INVITEs and ends the
# replaced dialog itself, as in xfer-onhook: the DUT BYEs the referee alone,
# behind the verdict NOTIFY.
#
# refers and byes are how many REFER and BYE requests the referee dialog owes.
# The plain peer owes one of each; the challenging peer answers each request
# 401 and owes the credential retry behind it (RFC 3261 22.2). A copy past the
# first exists only because the peer challenged the one in front of it, so the
# last copy of each is read for the credentials of that challenge.
#
# The Refer-To and the Contact are read on every copy on the wire. The retry
# builds its headers again rather than replaying the request
# (src/proto/sip/call.c, refer_resend), so a retry that lost the Replaces would
# hand one call away where the subscriber asked to join two.
conf_onhook_xfer_run()
{
    local name=$1
    local scenario=$2
    local refers=$3
    local byes=$4
    local subject=$5
    local contact_pattern
    local copies
    local detail
    local dialogs
    local n
    local placed
    local refer_pattern

    if ! conf_joined_start "$name" "$scenario" 2 xfer_replaced_ooc; then
        flow_stop
        result_print FAIL "$name" "$conf_failure_detail"
        return
    fi
    if ! conf_remote_cycle_wait "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "the peer did not finish its hold and resume"
        return
    fi
    hook_set release || true
    if ! sipp_finish_wait 60; then
        flow_stop
        result_print FAIL "$name" "SIPp did not complete the referral exchange"
        return
    fi
    dialogs=$(xfer_request_count "$flow_dir" REFER)
    if [[ $dialogs != 1 ]]; then
        flow_stop
        result_print FAIL "$name" \
            "the peer received REFER in $dialogs dialogs, expected the referee alone"
        return
    fi
    placed=$(xfer_request_count "$flow_dir" REFER CSeq)
    if [[ $placed != "$refers" ]]; then
        flow_stop
        result_print FAIL "$name" \
            "the referee dialog carried $placed REFER request(s), expected $refers"
        return
    fi
    refer_pattern="<sip:sipp@127\\.0\\.0\\.1:$sipp_port;transport=UDP\\?"
    refer_pattern+="Replaces=[0-9a-zA-Z._-]+%40127\\.0\\.0\\.1%3B"
    refer_pattern+="to-tag%3Dsipp-peer-tag%3Bfrom-tag%3D[0-9a-f]+>"
    # A DC-SIP capture shows the mixer's REFER refused 400 without a Contact.
    # Both pins are read on every copy on the wire, because the copy the peer
    # accepts is the retry: a retry that lost either header would draw that
    # 400 and pass a reading of the first copy.
    contact_pattern="<sip:5551000@127\\.0\\.0\\.1:$daemon_port>"
    copies=$(sipp_msg_count "$flow_dir" received "REFER ")
    for (( n = 1; n <= copies; n++ )); do
        if ! xfer_hdr_match_check "$flow_dir" received "REFER " Refer-To \
            "$refer_pattern" "$n"; then
            flow_stop
            result_print FAIL "$name" \
                "REFER copy $n of $copies did not carry the other dialog in escaped Replaces form"
            return
        fi
        if ! xfer_hdr_match_check "$flow_dir" received "REFER " Contact \
            "$contact_pattern" "$n"; then
            flow_stop
            result_print FAIL "$name" \
                "REFER copy $n of $copies did not carry our Contact"
            return
        fi
    done
    if (( refers > 1 )) && ! conf_xfer_cred_check "$flow_dir" "REFER " \
        "$conf_xfer_refer_nonce" "$copies"; then
        flow_stop
        result_print FAIL "$name" \
            "REFER copy $copies carried no credentials naming $conf_xfer_refer_nonce"
        return
    fi
    dialogs=$(xfer_request_count "$flow_dir" BYE)
    if [[ $dialogs != 1 ]]; then
        flow_stop
        result_print FAIL "$name" \
            "the peer received BYE in $dialogs dialogs, expected the referee alone"
        return
    fi
    placed=$(xfer_request_count "$flow_dir" BYE CSeq)
    if [[ $placed != "$byes" ]]; then
        flow_stop
        result_print FAIL "$name" \
            "the referee dialog carried $placed BYE request(s), expected $byes"
        return
    fi
    copies=$(sipp_msg_count "$flow_dir" received "BYE ")
    if (( byes > 1 )) && ! conf_xfer_cred_check "$flow_dir" "BYE " \
        "$conf_xfer_bye_nonce" "$copies"; then
        flow_stop
        result_print FAIL "$name" \
            "BYE copy $copies carried no credentials naming $conf_xfer_bye_nonce"
        return
    fi
    if ! conf_closed_wait "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" \
            "the transferred conference records did not close"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    detail="$sipp_verdict_detail, $subject"
    flow_stop
    result_print PASS "$name" "$detail"
}

conf_remote_bye_run()
{
    local name=conf-remote-bye
    local detail

    if ! conf_joined_start "$name" uac_conf_remote_bye; then
        flow_stop
        result_print FAIL "$name" "$conf_failure_detail"
        return
    fi
    if ! status_expr_wait "$flow_dir" \
        '.call[0].state == "CONNECTED" and
         (.call[0] | has("conf") | not) and
         (.call[0].calls | length) == 1 and
         .call[0].calls[0].inbound == false' \
        15 survivor || ! stub_wait "$flow_dir" \
        '.line[0].conf == 0 and .line[0].tone == 0 and
         .line[0].ringing == false' \
        5 clean-path; then
        flow_stop
        result_print FAIL "$name" \
            "the remote BYE did not leave the outbound leg on a clean path"
        return
    fi
    hook_set release || true
    if ! sipp_finish_wait 60 || ! conf_closed_wait "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "the surviving call did not close cleanly"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    detail="$sipp_verdict_detail, remote BYE survivor, clean tone path, and "
    detail+="marked records passed"
    flow_stop
    result_print PASS "$name" "$detail"
}

conf_ineligible_run()
{
    local name=conf-ineligible
    local detail

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! conf_config_write "$config_dir" 1; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name" ||
        ! sipp_uac_start "$flow_dir" "$sipp_dir/uac_conf_leg.xml" \
            "$sipp_port" "$daemon_port" "$media_port" \
            "$script_dir/register_ooc.xml" 2 2 5551000 -r 1 -rp 8s; then
        flow_stop
        result_print FAIL "$name" "startup failed"
        return
    fi
    if ! conf_first_answer "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "the first inbound call did not connect"
        return
    fi
    if ! status_wait "$flow_dir" CW_OFFERED 20 waiting; then
        flow_stop
        result_print FAIL "$name" \
            "the spaced second INVITE did not become call waiting"
        return
    fi
    flash_send || true
    if ! status_expr_wait "$flow_dir" \
        '.call[0].state == "TWO_CALLS" and
         (.call[0].calls | length) == 2 and
         ((.call[0].calls | map(select(.slot == 0)))[0].direction) ==
             "sendonly" and
         ((.call[0].calls | map(select(.slot == 1)))[0].direction) ==
             "sendrecv"' \
        20 answered; then
        flow_stop
        result_print FAIL "$name" "the flash did not answer the waiting call"
        return
    fi
    flash_send || true
    if ! status_expr_wait "$flow_dir" \
        '.call[0].state == "TWO_CALLS" and
         (.call[0] | has("conf") | not) and
         ((.call[0].calls | map(select(.slot == 0)))[0].direction) ==
             "sendrecv" and
         ((.call[0].calls | map(select(.slot == 1)))[0].direction) ==
             "sendonly"' \
        20 swapped ||
        ! stub_check "$flow_dir" '.line[0].conf == 0' no-conf; then
        flow_stop
        result_print FAIL "$name" \
            "two inbound calls did not swap with the conference mark absent"
        return
    fi
    if grep -qF 'stub: line 0 conf mode=1' "$flow_dir/daemon.log"; then
        flow_stop
        result_print FAIL "$name" \
            "the ineligible pair reached the conference mixer"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" HOLDING 15 holding; then
        flow_stop
        result_print FAIL "$name" "on-hook did not park the held inbound leg"
        return
    fi
    hook_set seize || true
    if ! status_wait "$flow_dir" CONNECTED 15 retrieved; then
        flow_stop
        result_print FAIL "$name" "seize did not retrieve the held inbound leg"
        return
    fi
    hook_set release || true
    if ! sipp_finish_wait 60 ||
        ! status_expr_wait "$flow_dir" \
            '.call[0].state == "IDLE" and (.call[0].calls | length) == 0' \
            15 drained ||
        ! cdr_expr_wait "$flow_dir" \
            '(.cdr | length) == 2 and
             ([.cdr[] | select(.in_progress)] | length) == 0 and
             ([.cdr[] | select(.conferenced == true)] | length) == 0' \
            20 closed; then
        flow_stop
        result_print FAIL "$name" \
            "the inbound pair did not close without conference marks"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 2; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    detail="$sipp_verdict_detail, spaced inbound offers, answer, swap guard, "
    detail+="and clean records passed"
    flow_stop
    result_print PASS "$name" "$detail"
}

# ------------------------------------------------------ the certificate fetch

# The archive the certificate server hands out, composed from the committed EC
# fixtures under the names files/uvoice_tls_cert.sh unpacks and installs.
cert_archive_build()
{
    local serve_dir=$1
    local src=$uvoice_dir/test/unit/tls

    mkdir -p "$serve_dir" || return 1
    cp "$src/server.crt" "$serve_dir/cert.pem" || return 1
    cp "$src/server.key" "$serve_dir/key.pem" || return 1
    cp "$src/ca.crt" "$serve_dir/ca.crt" || return 1
    tar czf "$serve_dir/voip_cert.tgz" -C "$serve_dir" cert.pem key.pem ca.crt || return 1
    rm -f "$serve_dir/cert.pem" "$serve_dir/key.pem" "$serve_dir/ca.crt"
}

# The certificate server, inside the namespace the flow runs in. It is a few
# lines of python3 rather than a general file server because the helper reads
# three things off the response and each one has to be exactly right: the
# status line, a Content-Type of application/x-gzip and a Content-Length under
# its own five kilobyte ceiling. It also answers a stable ETag, which is what
# the unchanged path compares.
cert_server_start()
{
    local flow_dir=$1
    local port=$2
    local archive=$3
    local deadline

    python3 - "$port" "$archive" >"$flow_dir/http-server.log" 2>&1 <<'PY' &
import http.server
import os
import sys

port = int(sys.argv[1])
archive = sys.argv[2]

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        if not self.path.endswith("/voip_cert.tgz"):
            self.send_error(404)
            return
        body = open(archive, "rb").read()
        self.send_response(200)
        self.send_header("Content-Type", "application/x-gzip")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("ETag", '"uvoice-e2e-fixture"')
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))
        sys.stderr.flush()

os.chdir("/")
http.server.HTTPServer(("127.0.0.1", port), Handler).serve_forever()
PY
    current_http_pid=$!

    deadline=$((SECONDS + 10))
    while (( SECONDS < deadline )); do
        if tcp_bound_check "$port"; then
            return 0
        fi
        if ! kill -0 "$current_http_pid" 2>/dev/null; then
            return 1
        fi
        sleep 0.1
    done

    return 1
}

# The status file, byte for byte. files/uvoice_tls_cert.sh writes it with no
# trailing newline and support tooling reads the exact strings, so the
# comparison is an equality and not a search.
cert_status_wait()
{
    local flow_dir=$1
    local wanted=$2
    local timeout_s=$3
    local label=$4
    local deadline=$((SECONDS + timeout_s))
    local text

    while (( SECONDS < deadline )); do
        if [[ -f $tls_status_file ]]; then
            text=$(cat "$tls_status_file")
            if [[ $text = "$wanted" ]]; then
                printf '%s: %s\n' "$label" "$text" >>"$flow_dir/tls-status.txt"
                return 0
            fi
        fi
        sleep 0.1
    done

    printf '%s: wanted [%s] got [%s]\n' "$label" "$wanted" "${text-<absent>}" \
        >>"$flow_dir/tls-status.txt"
    return 1
}

# The config of the certificate flows: a TLS line, which is what arms the fetch
# at all (src/proto/sip/account.c acct_tls_take), and the three fetch options.
# The flow runs on the shared daemon_port like every other flow. The reload
# rebind failure was never another flow's port state: the holder was the
# daemon's own helper child, forked between the spawn and its exec with a copy
# of the listener descriptor (src/spawn_run.h; the reload-loop flow drives it).
cert_config_write()
{
    local config_dir=$1
    local url=$2
    local username=$3

    config_write "$config_dir" "$sipp_port" "$daemon_port" "" "" "" "" "" tls \
        "$(printf "\toption tls_cert_path '%s'\n\toption tls_cert_username '%s'" \
            "$url" "$username")"
}

# The certificate fetch end to end, against a local http server.
#
# It is driven through the daemon itself, not by running the helper by hand: a
# TLS line arms event_tls_cert_start(), the daemon spawns
# files/uvoice_tls_cert.sh with the four arguments of its contract, and the
# exit code decides what happens next (src/event.h). All three paths under test
# are reached that way, the second and third through a live "voice reload".
#
# No SIPp takes part. What is asserted is the bundle, the status file the
# support tooling reads, and the listener the reload brings up; there is no
# peer to talk to and the registration is expected to find none.
cert_fetch_run()
{
    local name=cert-fetch
    local url
    local cert_sum
    local started

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! cert_archive_build "$flow_dir/serve"; then
        result_print FAIL "$name" "the certificate archive could not be composed"
        return
    fi
    # The bundle directory is claimed empty: what fills it is the helper, and
    # the claim is what makes the teardown safe for a directory this run made.
    # The claim takes the lock over both frozen paths, so the status-file test
    # below cannot race the run that would write it.
    if ! tls_bundle_claim "$name"; then
        result_print FAIL "$name" \
            "the bundle paths are held by another run, lock holder: $(tls_lock_owner), bundle: $(tls_bundle_list)"
        return
    fi
    if [[ -e $tls_status_file ]]; then
        flow_stop
        result_print FAIL "$name" "$tls_status_file is held by another run"
        return
    fi
    tls_status_owned=1
    if ! cert_server_start "$flow_dir" "$http_port" "$flow_dir/serve/voip_cert.tgz"; then
        flow_stop
        result_print FAIL "$name" "the local certificate server did not come up"
        return
    fi
    url=http://127.0.0.1:$http_port/voip_cert.tgz
    if ! cert_config_write "$config_dir" "$url" ""; then
        flow_stop
        result_print FAIL "$name" "config write failed"
        return
    fi
    # The helper path is an environment override (src/event.c
    # event_tls_helper_take), and it is exported around this one spawn: every
    # other flow must keep finding no helper at all, or a tls line with no
    # certificate URL would run it and write into the bundle directory the
    # stream flows own.
    export UVOICE_TLS_CERT=$uvoice_dir/files/uvoice_tls_cert.sh
    daemon_start "$flow_dir" "$config_dir" "$name"
    started=$?
    unset UVOICE_TLS_CERT
    if (( started != 0 )); then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    # The install path. Exit 0, the bundle unpacked into the fixed bundle path,
    # and the status string the support tooling reads.
    if ! cert_status_wait "$flow_dir" "Certificate updated and applied" 30 install; then
        flow_stop
        result_print FAIL "$name" \
            "the fetch did not report the install: $(tail -1 "$flow_dir/tls-status.txt")"
        return
    fi
    if [[ ! -s $tls_bundle_dir/cert.pem || ! -s $tls_bundle_dir/key.pem ||
        ! -s $tls_bundle_dir/ca.crt ]]; then
        flow_stop
        result_print FAIL "$name" "the bundle was not unpacked, bundle: $(tls_bundle_list)"
        return
    fi
    tls_bundle_record "$flow_dir" installed
    cert_sum=$(md5sum "$tls_bundle_dir/cert.pem" | cut -d' ' -f1)
    # The daemon-side observable (src/proto/sip/account.h accounts_tls_reload):
    # exit 0 calls the done hook, the hook loads the contexts again, and a
    # bundle with a server certificate in it is what brings the TLS listener up.
    if ! status_expr_wait "$flow_dir" \
        '.lines[0].transport == "tls" and .lines[0].listener == true' 20 listener; then
        flow_stop
        result_print FAIL "$name" \
            "the installed bundle did not bring the tls listener up"
        return
    fi
    # The unchanged path. The same URL answers the same ETag, so a second fetch
    # keeps the bundle it already has and exits 2. The fetch is re-armed by a
    # configuration move, which is the only thing that re-arms it once a bundle
    # is in place (src/proto/sip/account.c acct_tls_cfg_changed).
    if ! cert_config_write "$config_dir" "$url" probe; then
        flow_stop
        result_print FAIL "$name" "the second config write failed"
        return
    fi
    if ! ubus -s "$current_socket" call voice reload >/dev/null 2>&1; then
        flow_stop
        result_print FAIL "$name" "the daemon refused the reload"
        return
    fi
    if ! cert_status_wait "$flow_dir" "Certificate applied (etag unchanged)" 30 etag; then
        flow_stop
        result_print FAIL "$name" \
            "the second fetch did not report the unchanged bundle: $(tail -1 "$flow_dir/tls-status.txt")"
        return
    fi
    if [[ $cert_sum != $(md5sum "$tls_bundle_dir/cert.pem" | cut -d' ' -f1) ]]; then
        flow_stop
        result_print FAIL "$name" "the unchanged path rewrote the certificate"
        return
    fi
    # The config refusal. Exit 4 is the one code that arms no retry, and the
    # helper refuses the scheme before it touches anything, so the bundle that
    # is already installed survives it.
    if ! cert_config_write "$config_dir" gopher://127.0.0.1/voip_cert.tgz probe; then
        flow_stop
        result_print FAIL "$name" "the third config write failed"
        return
    fi
    if ! ubus -s "$current_socket" call voice reload >/dev/null 2>&1; then
        flow_stop
        result_print FAIL "$name" "the daemon refused the second reload"
        return
    fi
    if ! cert_status_wait "$flow_dir" "Unsupported file transfer protocol" 30 refused; then
        flow_stop
        result_print FAIL "$name" \
            "the refused configuration was not reported: $(tail -1 "$flow_dir/tls-status.txt")"
        return
    fi
    if [[ $cert_sum != $(md5sum "$tls_bundle_dir/cert.pem" | cut -d' ' -f1) ]]; then
        flow_stop
        result_print FAIL "$name" "the refused configuration took the bundle away"
        return
    fi
    if ! status_expr_wait "$flow_dir" '.lines[0].listener == true' 15 listener-kept; then
        flow_stop
        result_print FAIL "$name" "the refused configuration took the listener down"
        return
    fi
    tls_bundle_record "$flow_dir" refused
    flow_stop
    result_print PASS "$name" \
        "the daemon fetched and installed the bundle, the listener followed it up, the same ETag was answered with the unchanged path and an unsupported scheme was refused with the bundle left standing"
}

# The helper child is gone, which the status file cannot answer. event.c clears
# tls_cert.busy at the uloop reap, and the helper writes its final status string
# before its last steps, so a status wait can return while the child still runs.
# A reload that lands there takes the busy arm of event_tls_cert_start(), parks
# the request in tls_cert.restart and spawns nothing in the reload frame, which
# is the frame under test. The daemon's own children are the answer: the TLS
# helper is the only one it starts here (the event publisher and the firewall
# script are both absent on this host).
daemon_helper_wait()
{
    local timeout_s=$1
    local deadline=$((SECONDS + timeout_s))
    local count

    if [[ -z $current_daemon_pid ]]; then
        return 1
    fi

    while (( SECONDS < deadline )); do
        count=$(pgrep -cP "$current_daemon_pid" 2>/dev/null)
        if [[ $count = 0 ]]; then
            return 0
        fi
        sleep 0.05
    done

    return 1
}

# ------------------------------------------------------ the reload rebind loop

# The TLS line must survive every live reload.
#
# A reload that moves one of the five tls_* options runs this frame:
# acct_tls_take() starts the certificate helper, and the per-line loop
# behind it takes the REPLACE path, which tears the line down and builds it
# again microseconds later. The teardown closes the TLS listener and the build
# binds a new socket on the same port. A helper child that still held a copy of
# the daemon's descriptors would keep the old socket in TCP_LISTEN, and Linux
# refuses the bind against a TCP_LISTEN holder whatever SO_REUSEADDR says on
# either side: the line would come up dead with "Address already in use" and
# the firewall rules would go with it, until the next reload. src/spawn_run.c
# returns only after the child closed the daemon's descriptors.
#
# Thirty iterations, one assertion each. The username alternates so that every
# write really moves the configuration, the status file is removed before the
# reload so the wait proves this run of the helper finished, the daemon is left
# with no helper child before the next reload so that reload really spawns one,
# and the listener is read back before the next reload starts, because the next
# build would repair the line and hide the failure.
#
# What this flow proves and what it does not. The reap wait below is structural:
# it makes every iteration spawn a helper inside the reload frame, so all thirty
# drive the frame where a leaked descriptor loses the bind. Whether a daemon
# that leaks the descriptor actually loses the bind there is a matter of
# scheduling.
# On an idle multi-core host it usually does not. A loss on the bench needs an
# external CPU pin (taskset -c 0 on the daemon), and this harness applies none.
#
# No SIPp takes part, the same way the certificate flow needs none.
reload_loop_run()
{
    local name=reload-loop
    local iterations=${E2E_RELOAD_ITERATIONS:-30}
    local url
    local username
    local started
    local i

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    # The child wait below is the flow's own tool need, so it is checked here
    # and not in the runner's prerequisites: no other flow reads the process
    # tree.
    if ! command -v pgrep >/dev/null 2>&1; then
        result_print FAIL "$name" "pgrep is absent, the helper-child wait cannot run"
        return
    fi
    if ! cert_archive_build "$flow_dir/serve"; then
        result_print FAIL "$name" "the certificate archive could not be composed"
        return
    fi
    if ! tls_bundle_claim "$name"; then
        result_print FAIL "$name" \
            "the bundle paths are held by another run, lock holder: $(tls_lock_owner), bundle: $(tls_bundle_list)"
        return
    fi
    if [[ -e $tls_status_file ]]; then
        flow_stop
        result_print FAIL "$name" "$tls_status_file is held by another run"
        return
    fi
    tls_status_owned=1
    if ! cert_server_start "$flow_dir" "$http_port" "$flow_dir/serve/voip_cert.tgz"; then
        flow_stop
        result_print FAIL "$name" "the local certificate server did not come up"
        return
    fi
    url=http://127.0.0.1:$http_port/voip_cert.tgz
    if ! cert_config_write "$config_dir" "$url" ""; then
        flow_stop
        result_print FAIL "$name" "config write failed"
        return
    fi
    export UVOICE_TLS_CERT=$uvoice_dir/files/uvoice_tls_cert.sh
    daemon_start "$flow_dir" "$config_dir" "$name"
    started=$?
    unset UVOICE_TLS_CERT
    if (( started != 0 )); then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    if ! cert_status_wait "$flow_dir" "Certificate updated and applied" 30 install; then
        flow_stop
        result_print FAIL "$name" \
            "the fetch did not report the install: $(tail -1 "$flow_dir/tls-status.txt")"
        return
    fi
    # The install fetch owns a child too, so the loop starts with the daemon
    # idle, and the first reload spawns its own helper as every later one does.
    if ! daemon_helper_wait 30; then
        flow_stop
        result_print FAIL "$name" "the install fetch left a helper child running"
        return
    fi
    # The starting point of the loop: an inbound service that is really up, so
    # a later listener that is missing is the reload's doing and not a bundle
    # that never landed.
    if ! status_expr_wait "$flow_dir" \
        '.lines[0].transport == "tls" and .lines[0].listener == true' 20 listener; then
        flow_stop
        result_print FAIL "$name" "the installed bundle did not bring the tls listener up"
        return
    fi
    for (( i = 1; i <= iterations; i++ )); do
        if (( i % 2 )); then
            username=loop-a
        else
            username=loop-b
        fi
        if ! cert_config_write "$config_dir" "$url" "$username"; then
            flow_stop
            result_print FAIL "$name" "config write failed at iteration $i"
            return
        fi
        rm -f "$tls_status_file"
        if ! ubus -s "$current_socket" call voice reload >/dev/null 2>&1; then
            flow_stop
            result_print FAIL "$name" "the daemon refused the reload at iteration $i"
            return
        fi
        # The helper writes "N/A" first and its verdict last, and the file was
        # removed above, so this wait ends when this helper run reached its
        # verdict.
        if ! cert_status_wait "$flow_dir" "Certificate applied (etag unchanged)" 30 "iter-$i"; then
            flow_stop
            result_print FAIL "$name" \
                "the helper did not finish at iteration $i: $(tail -1 "$flow_dir/tls-status.txt")"
            return
        fi
        # The verdict is not the exit. The status string is written before the
        # helper's last steps and busy clears at the reap, so without this wait
        # the next reload could find tls_cert.busy set, park the request in
        # tls_cert.restart and spawn nothing in the frame under test.
        if ! daemon_helper_wait 30; then
            flow_stop
            result_print FAIL "$name" \
                "the helper child was still running at iteration $i, the next reload would defer its spawn"
            return
        fi
        if ! status_expr_wait "$flow_dir" '.lines[0].listener == true' 15 iter; then
            flow_stop
            result_print FAIL "$name" \
                "the rebind lost the tls listener at iteration $i of $iterations: $(grep -m1 -F 'Address already in use' "$flow_dir/daemon.log" || printf 'no bind failure logged')"
            return
        fi
    done
    if grep -qF "Address already in use" "$flow_dir/daemon.log"; then
        flow_stop
        result_print FAIL "$name" \
            "a bind failed during the loop: $(grep -m1 -F 'Address already in use' "$flow_dir/daemon.log")"
        return
    fi
    if grep -qF "no active line" "$flow_dir/daemon.log"; then
        flow_stop
        result_print FAIL "$name" \
            "the firewall reconcile found no active line: $(grep -m1 -F 'no active line' "$flow_dir/daemon.log")"
        return
    fi
    tls_bundle_record "$flow_dir" looped
    flow_stop
    result_print PASS "$name" \
        "$iterations live reloads rebuilt the tls line, every one of them kept its listener, and the daemon log carries no failed bind and no line-less firewall reconcile"
}

# The fatal bring-up exit. No SIPp and no peer: what is under test is the
# daemon's own decision, so the flow drives the stub lever to fatal and reads
# the exit status, the order of the three log lines the exit owes and the ubus
# object the teardown had to take down first.
daemon_dsp_fatal_run()
{
    local name=daemon_dsp_fatal
    local wait_status=0

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    # A failed config write is not tolerated here, unlike in the flows that
    # only place a call: the exit this flow reads runs the registration
    # teardown, which is what the config arms
    if ! config_write "$config_dir" "$sipp_port" "$daemon_port"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    if ! daemon_spawn "$flow_dir" "$config_dir" "$name" fatal; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    process_wait "$current_daemon_pid" 15 || wait_status=$?
    if (( wait_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "the daemon was still running 15 seconds after the fatal bring-up"
        return
    fi
    current_daemon_pid=
    if (( process_status != 1 )); then
        flow_stop
        result_print FAIL "$name" "the daemon exit status was $process_status, not 1"
        return
    fi
    # The teardown is proven in the log, not by the object list below: ubusd
    # drops every object of a closed connection, so the list alone would pass
    # on a daemon that crashed. The dsp stop and the transport close are the
    # two markers only the ordinary teardown emits.
    if ! daemon_log_order "$flow_dir" "dsp bring-up failed: fatal" \
        "dsp bring-up is terminal, exiting for the procd respawn" \
        "uvoiced stopping" "stub: dsp stop" "transport: socket 0 closed"; then
        flow_stop
        result_print FAIL "$name" "the daemon log does not carry the failure, the decision and the teardown in that order"
        return
    fi
    if ! ubus -s "$current_socket" list >"$flow_dir/ubus-list.txt" 2>/dev/null; then
        flow_stop
        result_print FAIL "$name" "the ubus socket did not answer after the exit"
        return
    fi
    if grep -q '^voice' "$flow_dir/ubus-list.txt"; then
        flow_stop
        result_print FAIL "$name" "the voice object outlived the daemon"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "the fatal bring-up exited 1, the teardown reached the dsp stop and the transport close, and no voice object was left"
}

# The manufacturing loop test, against the stub backend alone: no SIPp, no
# call, no wire. The stub renders no audio, so what this flow can assert is
# the record the daemon asked the backend for, which is the whole of the
# contract this side owns. UVOICE_STUB_RING_CAP_MS shortens the stub's ring
# timer so the capped expiry lands inside the flow; the daemon still asks for
# the 60 s window the arm carries.
loop_test_run()
{
    local name=loop-test

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! config_write "$config_dir" "$sipp_port" "$daemon_port" '' '' '' '' \
        '' '' '' 2; then
        result_print FAIL "$name" "config write failed"
        return
    fi

    export UVOICE_STUB_RING_CAP_MS=400
    if ! daemon_start "$flow_dir" "$config_dir" "$name"; then
        unset UVOICE_STUB_RING_CAP_MS
        flow_stop
        result_print FAIL "$name" "startup failed"
        return
    fi
    unset UVOICE_STUB_RING_CAP_MS

    # The self loop, so one handset is enough to arm the audio: the cross
    # connection waits for both handsets.
    if ! ubus -s "$current_socket" call voice loop_test \
        '{"enable":true,"mode":"self","delay_ms":200}' \
        >"$flow_dir/looptest-arm.json" 2>&1 ||
        ! jq -e '.enabled == true and .mode == "self" and .lines == 2' \
            "$flow_dir/looptest-arm.json" >/dev/null; then
        flow_stop
        result_print FAIL "$name" "the arm was refused"
        return
    fi
    if ! status_expr_wait "$flow_dir" \
        '[.call[] | .oos == true and .loop_test.mode == "self"] | all' \
        5 armed; then
        flow_stop
        result_print FAIL "$name" "the lines did not leave service for the mode"
        return
    fi

    hook_set seize || true
    if ! stub_wait "$flow_dir" '.line[0].loop_mode == 1 and
        .line[0].loop_delay_ms == 200 and .line[0].ringing == false' \
        5 offhook; then
        flow_stop
        result_print FAIL "$name" "the lifted handset armed no self loop"
        return
    fi

    hook_set release || true
    if ! stub_wait "$flow_dir" '.line[0].loop_mode == 0 and
        .line[0].ringing == true' 5 ringing; then
        flow_stop
        result_print FAIL "$name" "the on-hook edge did not ring the line again"
        return
    fi

    # The capped ring ends on its own, and the end record clears the status
    # key of the held port. The daemon paces no ring timer, so this is the
    # backend's own expiry.
    if ! stub_wait "$flow_dir" '.line[0].ringing == false' 5 ring-expired ||
        ! status_expr_wait "$flow_dir" \
            '.call[0].loop_test.ringing == false and
             .call[0].loop_test.offhook == false' 5 ring-end; then
        flow_stop
        result_print FAIL "$name" "the capped ring did not end the bell"
        return
    fi

    hook_set seize || true
    if ! status_expr_wait "$flow_dir" \
        '.call[0].loop_test.audio == true and
         .call[0].loop_test.offhook == true' 5 audio; then
        flow_stop
        result_print FAIL "$name" "the second lift armed no audio"
        return
    fi

    hook_set release || true
    if ! ubus -s "$current_socket" call voice loop_test '{"enable":false}' \
        >"$flow_dir/looptest-disarm.json" 2>&1 ||
        ! jq -e '.enabled == false and .mode == "off"' \
            "$flow_dir/looptest-disarm.json" >/dev/null; then
        flow_stop
        result_print FAIL "$name" "the disarm was refused"
        return
    fi
    if ! status_expr_wait "$flow_dir" \
        '([.call[] | (.loop_test | not)] | all) and .call[0].oos == false' \
        5 disarmed ||
        ! stub_check "$flow_dir" '.line[0].loop_mode == 0' cleared; then
        flow_stop
        result_print FAIL "$name" "the lines did not return to service"
        return
    fi

    status_save "$flow_dir" done
    flow_stop
    result_print PASS "$name" \
        "the mode rang, looped a lifted handset and gave the lines back"
}

# The other half of the pair: one transient bring-up is retried, the retry
# succeeds, and this daemon stays. The readiness wait of daemon_start is the
# "ready" assertion, the log line is the retry itself.
daemon_dsp_transient_run()
{
    local name=daemon_dsp_transient
    local wait_status=0

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! config_write "$config_dir" "$sipp_port" "$daemon_port"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    if ! daemon_start "$flow_dir" "$config_dir" "$name" transient_once; then
        flow_stop
        result_print FAIL "$name" "the retried bring-up did not reach the ready state"
        return
    fi
    if ! daemon_log_require "$flow_dir" "dsp bring-up retry" 5; then
        flow_stop
        result_print FAIL "$name" "the daemon log carries no bring-up retry"
        return
    fi
    process_wait "$current_daemon_pid" 2 || wait_status=$?
    if (( wait_status != 124 )); then
        flow_stop
        result_print FAIL "$name" "the daemon left after the retried bring-up, status $process_status"
        return
    fi
    status_save "$flow_dir" ready
    flow_stop
    result_print PASS "$name" \
        "the single retry brought the dsp up and the daemon stayed"
}

# A terminal bring-up completion that lands inside the stop. The lever holds
# the completion back two seconds, the flow sends the SIGTERM in front of it,
# and the completion then arrives while the teardown's own nested loop runs.
# The stop keeps the exit code, so the daemon has to leave with 0 and the
# teardown has to reach the dsp stop. The race is read from the log first: a
# run whose completion missed the stop window measured nothing and says so.
daemon_dsp_fatal_late_run()
{
    local name=daemon_dsp_fatal_late
    local wait_status=0

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! config_write "$config_dir" "$sipp_port" "$daemon_port"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    if ! daemon_spawn "$flow_dir" "$config_dir" "$name" fatal_late; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    if ! status_expr_wait "$flow_dir" '.hal.state == "starting"' 10 starting; then
        flow_stop
        result_print FAIL "$name" "the daemon never reported a bring-up in flight"
        return
    fi
    kill -TERM "$current_daemon_pid" 2>/dev/null
    process_wait "$current_daemon_pid" 15 || wait_status=$?
    if (( wait_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "the daemon did not stop within 15 seconds of the SIGTERM"
        return
    fi
    current_daemon_pid=
    if ! daemon_log_absent_after "$flow_dir" "uvoiced stopping" \
        "dsp bring-up is terminal, exiting for the procd respawn"; then
        flow_stop
        result_print FAIL "$name" "the late completion took over the stop with its own exit"
        return
    fi
    if ! daemon_log_order "$flow_dir" "uvoiced stopping" \
        "dsp bring-up failed inside the stop: fatal"; then
        flow_stop
        result_print PARTIAL "$name" "the late completion did not land inside the stop window"
        return
    fi
    if (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "the stop exited with status $process_status, not 0"
        return
    fi
    if ! grep -qF "stub: dsp stop" "$flow_dir/daemon.log"; then
        flow_stop
        result_print FAIL "$name" "the teardown never reached the dsp stop"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "the late terminal completion left the stop and its exit status alone"
}

# The retry must not arm inside the stop. The same lever shape as the fatal
# one, with a transient completion: the daemon that is already stopping owes
# no retry, because the bring-up it would submit lands behind the dsp stop the
# teardown is running. The two negative reads come first, so a retry that did
# arm fails the flow instead of shortening it.
daemon_dsp_transient_late_run()
{
    local name=daemon_dsp_transient_late
    local wait_status=0

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! config_write "$config_dir" "$sipp_port" "$daemon_port"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    if ! daemon_spawn "$flow_dir" "$config_dir" "$name" transient_late; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    if ! status_expr_wait "$flow_dir" '.hal.state == "starting"' 10 starting; then
        flow_stop
        result_print FAIL "$name" "the daemon never reported a bring-up in flight"
        return
    fi
    kill -TERM "$current_daemon_pid" 2>/dev/null
    process_wait "$current_daemon_pid" 15 || wait_status=$?
    if (( wait_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "the daemon did not stop within 15 seconds of the SIGTERM"
        return
    fi
    current_daemon_pid=
    if ! daemon_log_absent_after "$flow_dir" "uvoiced stopping" "dsp bring-up retry"; then
        flow_stop
        result_print FAIL "$name" "the stopping daemon armed a bring-up retry"
        return
    fi
    if ! daemon_log_absent_after "$flow_dir" "uvoiced stopping" "stub: dsp start"; then
        flow_stop
        result_print FAIL "$name" "the stopping daemon submitted a bring-up behind the dsp stop"
        return
    fi
    if ! daemon_log_order "$flow_dir" "uvoiced stopping" \
        "dsp bring-up failed inside the stop: transient"; then
        flow_stop
        result_print PARTIAL "$name" "the late completion did not land inside the stop window"
        return
    fi
    if (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "the stop exited with status $process_status, not 0"
        return
    fi
    if ! grep -qF "stub: dsp stop" "$flow_dir/daemon.log"; then
        flow_stop
        result_print FAIL "$name" "the teardown never reached the dsp stop"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "the late transient completion armed no retry and left the stop and its exit status alone"
}

# The bring-up the backend refuses on the spot. The submit answers fatal
# instead of scheduling a job, which a backend does when its worker is
# not running, so the exit is decided before the loop starts and the daemon
# has to leave through the same teardown all the same.
daemon_dsp_fatal_sync_run()
{
    local name=daemon_dsp_fatal_sync
    local wait_status=0

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! config_write "$config_dir" "$sipp_port" "$daemon_port"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    if ! daemon_spawn "$flow_dir" "$config_dir" "$name" fatal_sync; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    process_wait "$current_daemon_pid" 15 || wait_status=$?
    if (( wait_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "the daemon was still running 15 seconds after the refused bring-up"
        return
    fi
    current_daemon_pid=
    if (( process_status != 1 )); then
        flow_stop
        result_print FAIL "$name" "the daemon exit status was $process_status, not 1"
        return
    fi
    if ! daemon_log_order "$flow_dir" "dsp bring-up rejected: fatal" \
        "dsp bring-up is terminal, exiting for the procd respawn" \
        "uvoiced stopping" "stub: dsp stop"; then
        flow_stop
        result_print FAIL "$name" "the daemon log does not carry the refusal, the decision and the teardown in that order"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "the refused bring-up exited 1 through the ordinary teardown"
}

# The two G.722 flows. Both send the same offer, which lists G.722 first, PCMU
# second and telephone-event third. They differ in the DUT configuration
# alone:
#
#   - g722-call runs a DUT whose config names a codec section with tr104_id 5,
#     so the answer takes payload type 9 and the stream is programmed with
#     HAL_CODEC_G722. The stub advertises G.722 in its codec mask, so the
#     capability gate of call.c passes.
#   - g722-refused runs the default codec list, which stays PCMU, PCMA and
#     G.729. The answer skips 9 and takes 0, and the call still completes: a
#     codec refusal is not a call refusal.
#
# HAL_CODEC_G722 is 8 and HAL_CODEC_PCMU is 1 (hal.h). Both numbers are read
# beside the name, the way the SRTP flows read the suite beside its name.
g722_config_write()
{
    local config_dir=$1
    local wideband=$2
    local sections=

    if (( wideband == 1 )); then
        sections=$(printf "config codec 'g722'\n\toption enabled '1'\n\toption tr104_id '5'\n\toption priority '1'\nconfig codec 'pcmu'\n\toption enabled '1'\n\toption tr104_id '1'\n\toption priority '2'")
    fi

    config_write "$config_dir" "$sipp_port" "$daemon_port" "" "" "" "" "" "" "" 1 \
        "$sections"
}

g726_config_write()
{
    local config_dir=$1
    local enabled=$2
    local sections=

    if (( enabled == 1 )); then
        # %b expands the escapes; %s would write them as text.
        sections=$(printf '%b' \
            "config codec 'g726'\n" \
            "\toption enabled '1'\n" \
            "\toption tr104_id '4'\n" \
            "\toption priority '1'\n" \
            "config codec 'pcmu'\n" \
            "\toption enabled '1'\n" \
            "\toption tr104_id '1'\n" \
            "\toption priority '2'")
    fi

    config_write "$config_dir" "$sipp_port" "$daemon_port" "" "" "" \
        "" "" "" "" 1 "$sections"
}

codec_inbound_run()
{
    local name=$1
    local scenario=$2
    local enabled=$3
    local config_writer=$4
    local codec_id=$5
    local codec_name=$6
    local detail=$7

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! "$config_writer" "$config_dir" "$enabled"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/$scenario.xml" "$sipp_port" \
        "$daemon_port" "$media_port"; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! status_wait "$flow_dir" RINGING 10 ringing; then
        flow_stop
        result_print FAIL "$name" "daemon did not reach RINGING"
        return
    fi
    hook_set seize || true
    if ! status_wait "$flow_dir" CONNECTED 10 connected; then
        flow_stop
        result_print FAIL "$name" "daemon did not answer into CONNECTED"
        return
    fi
    if ! media_port_wait "$flow_dir" 5; then
        flow_stop
        result_print FAIL "$name" "stream 0 did not use the SDP media port"
        return
    fi
    if ! stub_wait "$flow_dir" \
        ".line[0].stream[0].running == true and .line[0].stream[0].codec == $codec_id and .line[0].stream[0].codec_name == \"$codec_name\"" \
        5 codec; then
        flow_stop
        result_print FAIL "$name" "the stream was not programmed with $codec_name"
        return
    fi
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not finish its BYE exchange"
        return
    fi
    if (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "SIPp exited with status $process_status"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "daemon did not return to IDLE after release"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" "$sipp_verdict_detail, $detail"
}

# The three Opus flows. The first two mirror the two G.722 flows: one offer,
# two DUT configurations.
#
#   - opus-call runs a DUT whose config names a codec section with tr104_id 6,
#     so the answer takes payload type 96 with opus/48000/2, our own decoder
#     fmtp, telephone-event/48000 and a=maxptime:20.
#   - opus-refused runs the default codec list, which stays PCMU, PCMA and
#     G.729. The answer skips 96, takes 0 and takes the 8000 telephone-event
#     beside it. The stub advertises Opus for this flow too, so the refusal it
#     asserts comes from the codec list alone.
#   - opus-narrow-dtmf offers Opus with the narrowband telephone-event only.
#     The answer takes Opus with no event line, so the daemon falls back to
#     inband digits once and counts it: inband digits over Opus are unreliable.
#
# All three run by default. The udsp media plane carries the codec and the
# stub names it in its shipped mask (hal/stub/stub.c stub_open), so nothing
# gates them. Each flow reads that premise from voice status before it drives
# the call, so a backend that stops advertising Opus fails the flow by name
# instead of answering PCMU quietly.
#
# What the stub proves and what it does not. It has no media plane: no Opus
# packet is encoded, sent or received on any of these flows. What runs end to
# end is the SDP negotiation, the capability gate of call.c and the stream
# description the daemon programmes, which the stub records and the dump
# returns. A hardware run is what checks the codec on the wire.
#
# HAL_CODEC_OPUS is 9 and HAL_CODEC_PCMU is 1 (hal.h). Both numbers are read
# beside the name, the way the G.722 flows read theirs.
opus_config_write()
{
    local config_dir=$1
    local wideband=$2
    local sections=

    # config_write prints this block while the channel section is still open,
    # so the option line lands in the channel and the codec headers behind it
    # start their own sections. Both flows need the RFC 2833 mode: the offer
    # names both telephone-event clocks and each answer has to mirror the one
    # that matches the codec it selected.
    sections=$(printf "\toption sip_dtmf_mode '%s'" "rfc2833")

    if (( wideband == 1 )); then
        sections=$(printf "%s\nconfig codec 'opus'\n\toption enabled '1'\n\toption tr104_id '6'\n\toption priority '1'\n\toption bitrate '32000'\n\toption fec '1'\n\toption complexity '5'\nconfig codec 'pcmu'\n\toption enabled '1'\n\toption tr104_id '1'\n\toption priority '2'" "$sections")
    fi

    config_write "$config_dir" "$sipp_port" "$daemon_port" "" "" "" "" "" "" "" 1 \
        "$sections"
}

# The whole constraint chain is observable here. The stub's stream dump
# carries the applied opus block beside the codec (hal/stub/stub.c
# stream_dump), so a wideband flow reads back what the negotiation settled on:
# the config asks 32000 bit/s, the peer's a=fmtp caps its decoder at 20000, and
# the number the stream runs with is the peer's. The bandwidth ceiling crosses
# the same way (maxplaybackrate 16000 is HAL_OPUS_BW_WB, hal.h enum hal_opus_bw).
#
# Asserted beside it: the dynamic payload type crossed the whole stack, the
# Opus packet mask reached the stream as ptime 20, and the digit fallback
# fired or stayed silent as the offer decides, once per exchange, read from
# the daemon's own counter ("voice status", the media table).
opus_inbound_run()
{
    local name=$1
    local scenario=$2
    local wideband=$3
    local codec_id=$4
    local codec_name=$5
    local codec_pt=$6
    local ptime=$7
    local fallback=$8
    local detail=$9
    local opus_expr=

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! opus_config_write "$config_dir" "$wideband"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    # The backend advertises the codec for every one of these flows. The leg
    # offers only what the HAL mask carries, and the stub names Opus in its
    # shipped mask, so the refusing flow refuses on its codec list against a
    # backend that could have run it.
    if ! register_prime "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    # The premise of all three flows, the refusing one included. The refusal
    # opus-refused asserts must come from the codec list, so the backend must
    # be one that could have run Opus. A backend that dropped the codec
    # answers PCMU for the other reason, and the flow would then pass on a
    # premise it never read.
    if ! status_expr_check "$flow_dir" \
        '.hal.codecs | index("opus") != null' hal-codecs; then
        flow_stop
        result_print FAIL "$name" "the backend advertises no opus"
        return
    fi
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/$scenario.xml" "$sipp_port" \
        "$daemon_port" "$media_port"; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! status_wait "$flow_dir" RINGING 10 ringing; then
        flow_stop
        result_print FAIL "$name" "daemon did not reach RINGING"
        return
    fi
    hook_set seize || true
    if ! status_wait "$flow_dir" CONNECTED 10 connected; then
        flow_stop
        result_print FAIL "$name" "daemon did not answer into CONNECTED"
        return
    fi
    if ! media_port_wait "$flow_dir" 5; then
        flow_stop
        result_print FAIL "$name" "stream 0 did not use the SDP media port"
        return
    fi
    # The applied encoder. A wideband flow asks 32000 bit/s and the peer's
    # a=fmtp caps its decoder at 20000, so the stream must run at the peer's
    # number; maxplaybackrate 16000 buckets as HAL_OPUS_BW_WB, which is
    # 3, and the two local asks cross untouched. A flow that took no Opus
    # carries an all-zero block, which is the other half of the assertion.
    if (( wideband == 1 )); then
        opus_expr=" and .line[0].stream[0].opus_bitrate == 20000 and .line[0].stream[0].opus_fec == true and .line[0].stream[0].opus_complexity == 5 and .line[0].stream[0].opus_max_bandwidth == 3"
    else
        opus_expr=" and .line[0].stream[0].opus_bitrate == 0 and .line[0].stream[0].opus_fec == false and .line[0].stream[0].opus_complexity == 0 and .line[0].stream[0].opus_max_bandwidth == 0"
    fi
    if ! stub_wait "$flow_dir" \
        ".line[0].stream[0].running == true and .line[0].stream[0].codec == $codec_id and .line[0].stream[0].codec_name == \"$codec_name\" and .line[0].stream[0].payload_type == $codec_pt and .line[0].stream[0].ptime == $ptime$opus_expr" \
        5 codec; then
        flow_stop
        result_print FAIL "$name" \
            "the stream was not programmed with $codec_name on payload type $codec_pt at ptime $ptime with the applied opus block"
        return
    fi
    # The warning names the fallback and the daemon counter says how often it
    # happened, once per exchange. The absence of both is as much of an
    # assertion as their presence.
    if (( fallback == 1 )); then
        if ! daemon_log_require "$flow_dir" \
            "the peer named no telephone-event/48000" 5; then
            flow_stop
            result_print FAIL "$name" "the daemon did not report the digit fallback"
            return
        fi
    elif grep -qF "the peer named no telephone-event/48000" "$flow_dir/daemon.log" 2>/dev/null; then
        flow_stop
        result_print FAIL "$name" "the daemon reported a digit fallback this flow negotiated away"
        return
    fi
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not finish its BYE exchange"
        return
    fi
    if (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "SIPp exited with status $process_status"
        return
    fi
    # Once per exchange, read after the BYE so both programmings of the one
    # answer are behind us: the 2xx and the ACK carry the same description and
    # only the first of them counts. A single read, not a poll: a poll for 1
    # would pass on its way to 2.
    if ! status_expr_check "$flow_dir" \
        ".media.opus_dtmf_fallbacks == $fallback" fallback-count; then
        flow_stop
        result_print FAIL "$name" \
            "the daemon did not count exactly $fallback digit fallbacks"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "daemon did not return to IDLE after release"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" "$sipp_verdict_detail, $detail"
}

# ------------------------------------------------------------- the PRACK flows
#
# The RFC 3262 set. Both roles run here: the uac_prack_* scenarios call the DUT
# and read the reliable 180 it sends, and the uas_prack_* scenarios answer a
# call the DUT places and read the PRACK it sends back.
#
# The answer of every uac_prack_* flow waits for a line of the daemon log
# before it seizes the hook. The model of the other inbound flows seizes as
# soon as the daemon reports RINGING, and the 200 of the INVITE would then
# overtake the PRACK the scenario has yet to send, which reorders the responses
# the scenario waits for.

# The config of a PRACK flow. prack, the lowered T1 of the give-up flow and the
# early media switch are channel options, so they ride behind config_write the
# way the T.38 options do.
prack_config_write()
{
    local config_dir=$1
    local prack=$2
    local timer_t1=${3:-}
    local early_media=${4:-}

    config_write "$config_dir" "$sipp_port" "$daemon_port" || return 1
    {
        # still part of the channel section config_write closed with
        printf "\toption prack '%u'\n" "$prack"
        if [[ -n $timer_t1 ]]; then
            printf "\toption sip_timer_t1 '%u'\n" "$timer_t1"
        fi
        if [[ -n $early_media ]]; then
            printf "\toption early_media_enabled '%u'\n" "$early_media"
        fi
    } >>"$config_dir/voip"
}

# How many messages of one direction open with a given start line, and where
# one of them sits among all messages of the log. sipp_msg_header_get answers
# for one message; a retransmission needs the count, and "no copy after the
# final response" needs the position. One walk answers both, and it splits the
# log the way every other reader here splits it.
#
# The fourth argument picks the position: "first" reports the earliest match
# and anything else the latest. A "no copy of X after Y" question needs the
# last X and the first Y, because a copy of Y behind the first one says nothing
# about X.
prack_msg_scan()
{
    local flow_dir=$1
    local direction=$2
    local start_line=$3
    local which=${4:-last}

    awk -v want="$direction" -v start="$start_line" -v which="$which" '
        /(UDP|TCP|TLS) message sent/ { dir = "sent"; first = 1; seen++; next }
        /(UDP|TCP|TLS) message received/ { dir = "received"; first = 1; seen++; next }
        first {
            if ($0 ~ /^[[:space:]]*$/)
                next
            line = $0
            gsub(/\r/, "", line)
            if (dir == want && index(line, start) == 1) {
                count++
                if (!earliest)
                    earliest = seen
                last = seen
            }
            first = 0
            next
        }
        END { printf "%u %u\n", count, which == "first" ? earliest : last }
    ' "$flow_dir/sipp-messages.log" 2>/dev/null
}

prack_msg_count()
{
    local scan

    scan=$(prack_msg_scan "$@") || return 1
    printf '%s\n' "${scan% *}"
}

# The first number of a header value one of the readers above returned: the
# RSeq of a reliable 180 and the acknowledged RSeq of a RAck (RFC 3262 7.1
# and 7.2). An empty value prints nothing, so the caller sees the absence.
prack_seq_get()
{
    printf '%s\n' "$1" | awk 'NR == 1 && NF > 1 { print $2 + 0 }'
}

# The tag parameter of a To header the readers above returned. It is what tells
# the 487 of a cancelled call and the 180 that preceded it apart from a
# response of another dialog.
prack_to_tag_get()
{
    printf '%s\n' "$1" | sed -n 's/.*;[Tt][Aa][Gg]=\([^;[:space:]]*\).*/\1/p'
}

# The cadence of the reliable 180 retransmissions, read off the capture the way
# timing_491_check reads the 491 retry. Status 2 is a capture the reader could
# not open, which the caller reports as PARTIAL; status 3 is a capture that
# holds another number of copies than the three this flow produces.
timing_prack_check()
{
    local flow_dir=$1

    if [[ $capture_ready -ne 1 ]] || ! command -v tshark >/dev/null 2>&1; then
        return 2
    fi
    # same decode-as rationale as timing_retry_check
    tshark -r "$flow_dir/traffic.pcap" \
        -d "udp.port==$sipp_port,sip" -d "udp.port==$daemon_port,sip" \
        -Y 'sip.Status-Code == 180' \
        -T fields -E separator='|' \
        -e frame.time_epoch -e sip.Status-Code \
        >"$flow_dir/timing-prack.txt" 2>"$flow_dir/tshark-prack.log" || return 2
    awk -F '|' '
        { stamp[++n] = $1 }
        END {
            if (n != 3)
                exit 3
            first = stamp[2] - stamp[1]
            second = stamp[3] - stamp[1]
            printf "180 retransmissions at %.6f and %.6f seconds\n", first, second
            exit !(first >= 0.4 && first <= 0.9 && second >= 1.3 && second <= 2.1)
        }
    ' "$flow_dir/timing-prack.txt" >"$flow_dir/timing-prack-result.txt"
}

# The distance from the first reliable 180 to the 504 that gives up on it, read
# off the capture the way timing_prack_check reads the cadence. T1 is 100 ms in
# this flow, so 64*T1 is 6.4 seconds. Status 2 is a capture the reader could
# not open, which the caller reports as PARTIAL; status 3 is a capture that
# holds no 180 or no 504 at all.
timing_giveup_check()
{
    local flow_dir=$1

    if [[ $capture_ready -ne 1 ]] || ! command -v tshark >/dev/null 2>&1; then
        return 2
    fi
    # same decode-as rationale as timing_retry_check
    tshark -r "$flow_dir/traffic.pcap" \
        -d "udp.port==$sipp_port,sip" -d "udp.port==$daemon_port,sip" \
        -Y 'sip.Status-Code == 180 || sip.Status-Code == 504' \
        -T fields -E separator='|' \
        -e frame.time_epoch -e sip.Status-Code \
        >"$flow_dir/timing-giveup.txt" 2>"$flow_dir/tshark-giveup.log" || return 2
    awk -F '|' '
        $2 == "180" && !ring_seen { ring_time = $1; ring_seen = 1 }
        $2 == "504" && ring_seen && !giveup_seen {
            delta = $1 - ring_time
            giveup_seen = 1
        }
        END {
            if (!giveup_seen)
                exit 3
            printf "the 504 followed the first 180 by %.6f seconds\n", delta
            exit !(delta >= 6.3 && delta <= 6.6)
        }
    ' "$flow_dir/timing-giveup.txt" >"$flow_dir/timing-giveup-result.txt"
}

# The reliable 180 and the one PRACK that matched it: the RSeq on the wire is
# the first number of the RAck that came back (RFC 3262 7.2), and the daemon
# names that same number as matched.
prack_matched_check()
{
    local flow_dir=$1
    local rseq
    local rack
    local rings
    local pracks

    rseq=$(prack_seq_get "$(sipp_msg_header_get "$flow_dir" received "SIP/2.0 180" RSeq)")
    rack=$(prack_seq_get "$(sipp_msg_header_get "$flow_dir" sent "PRACK " RAck)")
    if [[ -z $rseq || $rseq != "$rack" ]]; then
        prack_failure_detail="the 180 carried RSeq '$rseq' and the PRACK acknowledged '$rack'"
        return 1
    fi
    rings=$(prack_msg_count "$flow_dir" received "SIP/2.0 180")
    pracks=$(prack_msg_count "$flow_dir" sent "PRACK ")
    if (( rings != 1 || pracks != 1 )); then
        prack_failure_detail="the run carried $rings copies of the 180 and $pracks PRACK requests, not one of each"
        return 1
    fi
    if ! grep -qF "180 sent reliably, RSeq $rseq" "$flow_dir/daemon.log"; then
        prack_failure_detail="the daemon log does not report RSeq $rseq sent reliably"
        return 1
    fi
    if ! grep -qF "PRACK matched RSeq $rseq" "$flow_dir/daemon.log"; then
        prack_failure_detail="the daemon log does not report RSeq $rseq matched"
        return 1
    fi
    prack_detail="the 180 went out reliably with RSeq $rseq and its single PRACK was matched"
}

# The other value of the option under the same INVITE: the 180 stays plain, so
# it carries no RSeq and earns no PRACK at all.
prack_plain_check()
{
    local flow_dir=$1
    local rseq
    local pracks

    if grep -qF "180 sent reliably" "$flow_dir/daemon.log"; then
        prack_failure_detail="the 180 went out reliably against a clear prack option"
        return 1
    fi
    if ! grep -qF "180 sent" "$flow_dir/daemon.log"; then
        prack_failure_detail="the daemon log reports no 180 at all"
        return 1
    fi
    rseq=$(sipp_msg_header_get "$flow_dir" received "SIP/2.0 180" RSeq)
    if [[ -n $rseq ]]; then
        prack_failure_detail="the plain 180 carried '$rseq'"
        return 1
    fi
    pracks=$(prack_msg_count "$flow_dir" sent "PRACK ")
    if (( pracks != 0 )); then
        prack_failure_detail="the plain 180 drew $pracks PRACK requests"
        return 1
    fi
    prack_detail="the 180 stayed plain, carried no RSeq and drew no PRACK"
}

# Three refused PRACKs and one accepted. Only two of the three reach the PRACK
# handler: the copy with a foreign To tag names a dialog the DUT never created,
# so the no-dialog arm of the request path answers it.
prack_bad_rack_check()
{
    local flow_dir=$1
    local refusals
    local traces
    local matches

    refusals=$(prack_msg_count "$flow_dir" received "SIP/2.0 481")
    if (( refusals != 3 )); then
        prack_failure_detail="the three bad PRACKs drew $refusals responses of 481"
        return 1
    fi
    traces=$(grep -cF "PRACK matches no reliable provisional" "$flow_dir/daemon.log")
    if (( traces != 2 )); then
        prack_failure_detail="the PRACK handler refused $traces requests, not the two that reach it"
        return 1
    fi
    matches=$(grep -cF "PRACK matched RSeq" "$flow_dir/daemon.log")
    if (( matches != 1 )); then
        prack_failure_detail="the daemon matched $matches PRACK requests, not one"
        return 1
    fi
    prack_detail="three PRACKs drew 481, two of them from the PRACK handler, and the fourth earned the 200"
}

# The inbound half of the set: SIPp calls the DUT, the scenario drives the
# PRACK exchange, and the hook is seized once the daemon log carries the gate
# line. Every flow here completes the call and takes the scenario's own BYE.
prack_answer_run()
{
    local name=$1
    local scenario=$2
    local prack=$3
    local gate=$4
    local checker=$5
    local subject=$6

    prack_detail=
    prack_failure_detail=
    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! prack_config_write "$config_dir" "$prack"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/$scenario.xml" "$sipp_port" \
        "$daemon_port" "$media_port"; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! status_wait "$flow_dir" RINGING 10 ringing; then
        flow_stop
        result_print FAIL "$name" "daemon did not reach RINGING"
        return
    fi
    if ! daemon_log_require "$flow_dir" "$gate" 20; then
        flow_stop
        result_print FAIL "$name" "the daemon log carries no \"$gate\" line"
        return
    fi
    hook_set seize || true
    if ! status_wait "$flow_dir" CONNECTED 15 connected; then
        flow_stop
        result_print FAIL "$name" "daemon did not answer into CONNECTED"
        return
    fi
    if ! media_port_wait "$flow_dir" 5; then
        flow_stop
        result_print FAIL "$name" "stream 0 did not use the SDP media port"
        return
    fi
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not finish its BYE exchange"
        return
    fi
    if (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "SIPp exited with status $process_status"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "daemon did not return to IDLE after release"
        return
    fi
    if ! "$checker" "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "$prack_failure_detail"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" "$sipp_verdict_detail, $subject: $prack_detail"
}

# The PRACK held back past two retransmissions. The count and the cadence are
# read here and not in the scenario: SIPp absorbs a retransmitted response, so
# no scenario can count one.
prack_late_run()
{
    local name=prack-late
    local rings
    local retransmissions
    local timing_status=0

    prack_detail=
    prack_failure_detail=
    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! prack_config_write "$config_dir" 1; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/uac_prack_late.xml" "$sipp_port" \
        "$daemon_port" "$media_port"; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! status_wait "$flow_dir" RINGING 10 ringing; then
        flow_stop
        result_print FAIL "$name" "daemon did not reach RINGING"
        return
    fi
    if ! daemon_log_require "$flow_dir" "PRACK matched RSeq" 20; then
        flow_stop
        result_print FAIL "$name" "the late PRACK was never matched"
        return
    fi
    hook_set seize || true
    if ! status_wait "$flow_dir" CONNECTED 15 connected; then
        flow_stop
        result_print FAIL "$name" "daemon did not answer into CONNECTED"
        return
    fi
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not finish its BYE exchange"
        return
    fi
    if (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "SIPp exited with status $process_status"
        return
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "daemon did not return to IDLE after release"
        return
    fi
    rings=$(prack_msg_count "$flow_dir" received "SIP/2.0 180")
    if (( rings != 3 )); then
        flow_stop
        result_print FAIL "$name" "the 1700 ms wait carried $rings copies of the 180, not three"
        return
    fi
    retransmissions=$(grep -cF "180 retransmitted, RSeq" "$flow_dir/daemon.log")
    if (( retransmissions != 2 )); then
        flow_stop
        result_print FAIL "$name" "the daemon reported $retransmissions retransmissions, not two"
        return
    fi
    if ! daemon_log_absent_after "$flow_dir" "PRACK matched RSeq" \
        "180 retransmitted, RSeq"; then
        flow_stop
        result_print FAIL "$name" "the 180 was retransmitted after its PRACK matched"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    capture_finish
    timing_prack_check "$flow_dir" || timing_status=$?
    if (( timing_status == 2 )); then
        flow_stop
        result_print PARTIAL "$name" \
            "$sipp_verdict_detail, three 180s and two retransmission traces, packet timing capture unavailable"
        return
    fi
    if (( timing_status == 3 )); then
        flow_stop
        result_print FAIL "$name" "the capture holds another number of 180 frames than three"
        return
    fi
    if (( timing_status != 0 )); then
        flow_stop
        result_print FAIL "$name" \
            "the retransmissions were outside the T1 and 2*T1 windows: $(cat "$flow_dir/timing-prack-result.txt")"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, $(cat "$flow_dir/timing-prack-result.txt"), two retransmission traces and none behind the PRACK"
}

# The give-up: no PRACK ever arrives, so the DUT retransmits its 180 to 64*T1
# and rejects the INVITE 504. T1 is lowered to 100 ms, which puts the seven
# sends inside 6.3 seconds and the rejection at 6.4.
prack_giveup_run()
{
    local name=prack-giveup
    local rings
    local rejections
    local marker
    local timing_status=0

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! prack_config_write "$config_dir" 1 100; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/uac_prack_giveup.xml" \
        "$sipp_port" "$daemon_port" "$media_port"; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! status_wait "$flow_dir" RINGING 10 ringing; then
        flow_stop
        result_print FAIL "$name" "daemon did not reach RINGING"
        return
    fi
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not receive the 504 rejection"
        return
    fi
    if (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "SIPp exited with status $process_status"
        return
    fi
    if ! status_wait "$flow_dir" IDLE 10 idle; then
        flow_stop
        result_print FAIL "$name" "the rejected call did not return the daemon to IDLE"
        return
    fi
    if ! daemon_log_require "$flow_dir" \
        "no PRACK within 64*T1, INVITE rejected 504" 5; then
        flow_stop
        result_print FAIL "$name" "the daemon log carries no give-up"
        return
    fi
    rejections=$(prack_msg_count "$flow_dir" received "SIP/2.0 504")
    if (( rejections < 1 )); then
        flow_stop
        result_print FAIL "$name" "the run carried no 504 at all"
        return
    fi
    # Seven copies are owed: the first send and the six doublings at T1, 3*T1,
    # 7*T1, 15*T1, 31*T1 and 63*T1, all inside the 64*T1 window.
    rings=$(prack_msg_count "$flow_dir" received "SIP/2.0 180")
    if (( rings != 7 )); then
        flow_stop
        result_print FAIL "$name" "the give-up carried $rings copies of the 180, not seven"
        return
    fi
    if ! daemon_log_order "$flow_dir" \
        '[hal] stub: line 0 ring start' '[hal] stub: line 0 ring stop'; then
        flow_stop
        result_print FAIL "$name" "the ring did not start and stop in that order"
        return
    fi
    for marker in '[hal] stub: line 0 strm 0 start'; do
        if grep -qF "$marker" "$flow_dir/daemon.log"; then
            flow_stop
            result_print FAIL "$name" "media started on a call that was never answered"
            return
        fi
    done
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    capture_finish
    timing_giveup_check "$flow_dir" || timing_status=$?
    if (( timing_status == 2 )); then
        flow_stop
        result_print PARTIAL "$name" \
            "$sipp_verdict_detail, $rings copies of the 180, the ring stopped and no stream ever ran, packet timing capture unavailable"
        return
    fi
    if (( timing_status == 3 )); then
        flow_stop
        result_print FAIL "$name" "the capture holds no 180 or no 504 frame"
        return
    fi
    if (( timing_status != 0 )); then
        flow_stop
        result_print FAIL "$name" \
            "the give-up fell outside the 64*T1 window: $(cat "$flow_dir/timing-giveup-result.txt")"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, $rings copies of the 180, $(cat "$flow_dir/timing-giveup-result.txt"), the ring stopped and no stream ever ran"
}

# The CANCEL under an unacknowledged reliable 180. The 487 has to wear the tag
# of that 180, and no copy of it may follow the 487 on the wire. The scenario
# holds the call open behind the ACK, so a copy the daemon sends after the 487
# reaches the message log, and the position of the first 487 is what the copy
# is compared against: a retransmitted 487 sits behind a stray 180 and would
# hide it.
prack_cancel_run()
{
    local name=prack-cancel
    local ring_tag
    local reject_tag
    local ring_scan
    local reject_scan
    local marker

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! prack_config_write "$config_dir" 1; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "registration setup failed"
        return
    fi
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/uac_prack_cancel.xml" \
        "$sipp_port" "$daemon_port" "$media_port"; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! sipp_finish_wait 25; then
        flow_stop
        result_print FAIL "$name" "SIPp did not finish the CANCEL exchange"
        return
    fi
    if (( process_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "SIPp exited with status $process_status"
        return
    fi
    for marker in \
        '180 sent reliably, RSeq' \
        '[hal] stub: line 0 ring start' \
        '[fsm] line 0 CALL_REMOTE_CANCEL in RINGING' \
        '[hal] stub: line 0 ring stop'; do
        if ! daemon_log_require "$flow_dir" "$marker" 5; then
            flow_stop
            result_print FAIL "$name" "daemon log lacks the ring evidence: $marker"
            return
        fi
    done
    ring_tag=$(prack_to_tag_get "$(sipp_msg_header_get "$flow_dir" received "SIP/2.0 180" To)")
    reject_tag=$(prack_to_tag_get "$(sipp_msg_header_get "$flow_dir" received "SIP/2.0 487" To)")
    if [[ -z $ring_tag || $ring_tag != "$reject_tag" ]]; then
        flow_stop
        result_print FAIL "$name" \
            "the 180 wore tag '$ring_tag' and the 487 wore '$reject_tag'"
        return
    fi
    ring_scan=$(prack_msg_scan "$flow_dir" received "SIP/2.0 180")
    reject_scan=$(prack_msg_scan "$flow_dir" received "SIP/2.0 487" first)
    if (( ${ring_scan#* } > ${reject_scan#* } )); then
        flow_stop
        result_print FAIL "$name" "a copy of the 180 followed the 487"
        return
    fi
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "remote CANCEL did not return daemon to IDLE"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, the 487 wore the tag of the reliable 180 and no copy of it followed"
}

# The outbound half of the set. The DUT places the call, SIPp answers it
# reliably, and the PRACK the DUT sends back is what each checker reads.
prack_dial_run()
{
    local name=$1
    local scenario=$2
    local checker=$3
    local subject=$4
    local pause_ms

    prack_detail=
    prack_failure_detail=
    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! prack_config_write "$config_dir" 0; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    if ! sipp_start "$flow_dir" "$sipp_dir/$scenario.xml" "$sipp_port" \
        "$daemon_port" "$media_port" 2; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! daemon_start "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    if ! outbound_dial_start "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "off-hook dial setup failed"
        return
    fi
    if ! status_wait "$flow_dir" CONNECTED 30 connected; then
        flow_stop
        result_print FAIL "$name" "daemon did not reach CONNECTED"
        return
    fi
    if ! media_port_wait "$flow_dir" 5; then
        flow_stop
        result_print FAIL "$name" "stream 0 did not use the SDP media port"
        return
    fi
    if ! "$checker" "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "$prack_failure_detail"
        return
    fi
    # The scenario waits for the BYE only after a scripted pause, the way
    # flow_a's does, so the hook release has to wait that pause out.
    pause_ms=$(scenario_pause_get "$sipp_dir/$scenario.xml")
    sleep "$(( (pause_ms + 1500) / 1000 ))"
    hook_set release || true
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not complete the BYE exchange"
        return
    fi
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "daemon did not return to IDLE"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 2; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" "$sipp_verdict_detail, $subject: $prack_detail"
}

# Our own INVITE and the one PRACK a reliable 180 draws from it: the
# advertisement of RFC 3262 4, the RAck triple of 7.2, and the CSeq the dialog
# layer draws for an ordinary in-dialog request.
prack_uac_180_check()
{
    local flow_dir=$1
    local supported
    local allow
    local require
    local cseq
    local rack
    local prack_cseq
    local pracks

    supported=$(sipp_msg_header_get "$flow_dir" received "INVITE " Supported)
    allow=$(sipp_msg_header_get "$flow_dir" received "INVITE " Allow)
    require=$(sipp_msg_header_get "$flow_dir" received "INVITE " Require)
    if [[ $supported != *100rel* ]]; then
        prack_failure_detail="the INVITE advertised '$supported'"
        return 1
    fi
    if [[ $allow != *PRACK* ]]; then
        prack_failure_detail="the INVITE allowed '$allow'"
        return 1
    fi
    if [[ -n $require ]]; then
        prack_failure_detail="the INVITE insisted with '$require'"
        return 1
    fi
    cseq=$(sipp_msg_header_get "$flow_dir" received "INVITE " CSeq | awk '{ print $2 + 0 }')
    rack=$(sipp_msg_header_get "$flow_dir" received "PRACK " RAck)
    if [[ ${rack#RAck: } != "4242 $cseq INVITE" ]]; then
        prack_failure_detail="the PRACK carried '$rack' against INVITE CSeq $cseq"
        return 1
    fi
    prack_cseq=$(sipp_msg_header_get "$flow_dir" received "PRACK " CSeq | awk '{ print $2 + 0 }')
    if (( prack_cseq != cseq + 1 )); then
        prack_failure_detail="the PRACK took CSeq $prack_cseq behind INVITE CSeq $cseq"
        return 1
    fi
    pracks=$(prack_msg_count "$flow_dir" received "PRACK ")
    if (( pracks != 1 )); then
        prack_failure_detail="the reliable 180 drew $pracks PRACK requests, not one"
        return 1
    fi
    if ! grep -qF "reliable 1xx RSeq 4242, PRACK sent" "$flow_dir/daemon.log"; then
        prack_failure_detail="the daemon log does not report the PRACK for RSeq 4242"
        return 1
    fi
    prack_detail="the INVITE advertised 100rel and PRACK without insisting, and the RAck read $cseq with CSeq $prack_cseq"
}

# The same 180 twice: RFC 3262 4 discards the copy, so one PRACK stands.
prack_uac_dup_check()
{
    local flow_dir=$1
    local pracks

    pracks=$(prack_msg_count "$flow_dir" received "PRACK ")
    if (( pracks != 1 )); then
        prack_failure_detail="the repeated 180 drew $pracks PRACK requests, not one"
        return 1
    fi
    if ! grep -qF "retransmitted reliable 1xx RSeq 4242 discarded" \
        "$flow_dir/daemon.log"; then
        prack_failure_detail="the daemon did not report the repeated RSeq as discarded"
        return 1
    fi
    prack_detail="the repeated RSeq 4242 was discarded and drew no second PRACK"
}

# A reliable 180 whose RSeq is not one higher than the acknowledged one: the
# DUT acknowledges the 183 alone and processes the 180 no further.
prack_uac_gap_check()
{
    local flow_dir=$1
    local rack
    local pracks

    pracks=$(prack_msg_count "$flow_dir" received "PRACK ")
    if (( pracks != 1 )); then
        prack_failure_detail="the gap drew $pracks PRACK requests, not one"
        return 1
    fi
    rack=$(prack_seq_get "$(sipp_msg_header_get "$flow_dir" received "PRACK " RAck)")
    if [[ $rack != 10 ]]; then
        prack_failure_detail="the one PRACK acknowledged RSeq '$rack', not 10"
        return 1
    fi
    if ! grep -qF "reliable 1xx RSeq 12 out of order, ignored" \
        "$flow_dir/daemon.log"; then
        prack_failure_detail="the daemon did not report RSeq 12 as out of order"
        return 1
    fi
    prack_detail="RSeq 10 was acknowledged and RSeq 12 was ignored"
}

# The challenged INVITE. Both attempts advertise 100rel, and the RAck names the
# CSeq of the attempt that was really answered.
prack_uac_auth_check()
{
    local flow_dir=$1
    local invites
    local first_supported
    local retry_supported
    local retry_cseq
    local rack
    local pracks

    invites=$(prack_msg_count "$flow_dir" received "INVITE ")
    if (( invites != 2 )); then
        prack_failure_detail="the challenge drew $invites INVITE requests, not two"
        return 1
    fi
    first_supported=$(sipp_msg_header_get "$flow_dir" received "INVITE " Supported 1)
    retry_supported=$(sipp_msg_header_get "$flow_dir" received "INVITE " Supported 2)
    if [[ $first_supported != *100rel* || $retry_supported != *100rel* ]]; then
        prack_failure_detail="the two INVITEs advertised '$first_supported' and '$retry_supported'"
        return 1
    fi
    retry_cseq=$(sipp_msg_header_get "$flow_dir" received "INVITE " CSeq 2 | awk '{ print $2 + 0 }')
    rack=$(sipp_msg_header_get "$flow_dir" received "PRACK " RAck)
    if [[ ${rack#RAck: } != "4242 $retry_cseq INVITE" ]]; then
        prack_failure_detail="the PRACK carried '$rack' against the retried CSeq $retry_cseq"
        return 1
    fi
    pracks=$(prack_msg_count "$flow_dir" received "PRACK ")
    if (( pracks != 1 )); then
        prack_failure_detail="the retried INVITE drew $pracks PRACK requests, not one"
        return 1
    fi
    prack_detail="both INVITEs advertised 100rel and the RAck named the retried CSeq $retry_cseq"
}

# The answer inside a reliable 183 (RFC 3262 5 with RFC 3261 13.2.1). The
# option decides the early stream alone: at either value the answer closes the
# exchange, so the 2xx behind it carries no body and the call connects on the
# media of the 183.
#
# The second argument is the early_media_enabled the config names. A flow that
# names none runs on the schema default 1, which is the shipped behaviour.
#
# With the early stream running, the local ringback plays on and the first
# packet of that stream is what stops it (RFC 3960 3.2). The stub carries no
# RTP socket, so rtp_rx_send is what delivers that packet.
prack_early_media_run()
{
    local name=$1
    local early_media=${2:-}
    local byes
    local pause_ms

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! prack_config_write "$config_dir" 0 "" "$early_media"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    if ! sipp_start "$flow_dir" "$sipp_dir/uas_prack_183_sdp.xml" "$sipp_port" \
        "$daemon_port" "$media_port" 2; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! daemon_start "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    if ! outbound_dial_start "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "off-hook dial setup failed"
        return
    fi
    if ! daemon_log_require "$flow_dir" "reliable 1xx answer taken" 20; then
        flow_stop
        result_print FAIL "$name" "the answer of the reliable 183 was not taken"
        return
    fi
    # The stub reports the applied direction as the raw hal_media_dir number
    # (schema/uvoice-api-schema.json, the stub stream entry): 3 is
    # HAL_MEDIA_SENDRECV, which is what the sendrecv answer of the 183
    # negotiates. The tone is the raw hal_tone number: 4 is HAL_TONE_RINGBACK
    # and 0 is HAL_TONE_NONE.
    #
    # One sample carries the three readings, so the early session is read
    # once inside the pause of the scenario and no new race window opens.
    if [[ $early_media != 0 ]]; then
        if ! stub_wait "$flow_dir" \
            '.line[0].stream[0].running == true and .line[0].stream[0].direction == 3 and .line[0].tone == 4' \
            3 early-sendrecv; then
            flow_stop
            result_print FAIL "$name" "the early stream did not run sendrecv under the local ringback"
            return
        fi
        # The log line is the primary evidence and the tone reading follows
        # it: the 200 also sets the tone to 0, so a tone sample alone could
        # pass for the wrong reason if the answer won the race.
        if ! rtp_rx_send 0 0; then
            flow_stop
            result_print FAIL "$name" "the first packet could not be injected"
            return
        fi
        if ! daemon_log_require "$flow_dir" "early media flowing" 5; then
            flow_stop
            result_print FAIL "$name" "the first packet of the early stream did not reach the line"
            return
        fi
        if ! stub_wait "$flow_dir" '.line[0].tone == 0' 5 ringback-stopped; then
            flow_stop
            result_print FAIL "$name" "the local ringback kept playing behind the first packet"
            return
        fi
    else
        if ! stub_wait "$flow_dir" '.line[0].tone == 4' 3 ringback; then
            flow_stop
            result_print FAIL "$name" "the local ringback did not play behind the 183"
            return
        fi
        if ! stub_check "$flow_dir" '.line[0].stream[0].running == false' \
            no-early-stream; then
            flow_stop
            result_print FAIL "$name" "a stream ran before the call was answered"
            return
        fi
        # The other half of the reading above: the stub refuses the verb on a
        # stream that is not running (stub.c stub_ubus_rtp_rx), so a call that
        # succeeds here means a stream ran after all.
        if rtp_rx_send 0 0 2>/dev/null; then
            flow_stop
            result_print FAIL "$name" "the stub took a first packet on a stream that is not running"
            return
        fi
    fi
    if ! status_wait "$flow_dir" CONNECTED 30 connected; then
        flow_stop
        result_print FAIL "$name" "daemon did not reach CONNECTED on the bodiless 200"
        return
    fi
    if ! media_port_wait "$flow_dir" 5; then
        flow_stop
        result_print FAIL "$name" "stream 0 did not use the media port of the 183"
        return
    fi
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].running == true and .line[0].stream[0].direction == 3' \
        5 sendrecv; then
        flow_stop
        result_print FAIL "$name" "the answered call did not run sendrecv"
        return
    fi
    # The 2xx carried no body at all, so the daemon had nothing to report
    # against the answer it already held.
    if grep -qF "2xx body ignored" "$flow_dir/daemon.log"; then
        flow_stop
        result_print FAIL "$name" "the 200 of the INVITE carried a body"
        return
    fi
    pause_ms=$(scenario_pause_get "$sipp_dir/uas_prack_183_sdp.xml")
    sleep "$(( (pause_ms + 1500) / 1000 ))"
    hook_set release || true
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not complete the BYE exchange"
        return
    fi
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "daemon did not return to IDLE"
        return
    fi
    # One BYE and no other: the bodiless 2xx must not end the call by itself.
    byes=$(prack_msg_count "$flow_dir" received "BYE ")
    if (( byes != 1 )); then
        flow_stop
        result_print FAIL "$name" "the call carried $byes BYE requests, not the one the hook release owes"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 2; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, the answer of the reliable 183 stood, the bodiless 200 connected the call sendrecv on the 183 port and one BYE ended it"
}

# The same ringback rule under the plain 183, which is the common field case:
# the carrier relays a 183 with SDP, names no PRACK tag and may send no early
# media at all (RFC 3960 3.2). The channel runs prack 0 and the shipped early
# media default, so the 183 is unreliable, the DUT sends no PRACK, and the
# answer stays provisional: uac_ok() reopens the offer, so the 200 carries the
# same body again.
early_media_183_run()
{
    local name=$1
    local pracks
    local pause_ms

    flow_dir=$artifact_root/$name
    mkdir -p "$flow_dir" || {
        result_print FAIL "$name" "artifact directory creation failed"
        return
    }
    config_dir=$flow_dir/config
    if ! prack_config_write "$config_dir" 0; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    if ! sipp_start "$flow_dir" "$sipp_dir/uas_183_sdp.xml" "$sipp_port" \
        "$daemon_port" "$media_port" 2; then
        flow_stop
        result_print FAIL "$name" "SIPp startup failed"
        return
    fi
    if ! daemon_start "$flow_dir" "$config_dir" "$name"; then
        flow_stop
        result_print FAIL "$name" "daemon startup failed"
        return
    fi
    if ! outbound_dial_start "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "off-hook dial setup failed"
        return
    fi
    # One sample for the three readings, inside the pause of the scenario: the
    # early stream runs sendrecv (3, the raw hal_media_dir), the direction the
    # answer of the 183 negotiated, and the local ringback still plays (4, the
    # raw hal_tone).
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].running == true and .line[0].stream[0].direction == 3 and .line[0].tone == 4' \
        5 early-sendrecv; then
        flow_stop
        result_print FAIL "$name" "the early stream did not run sendrecv under the local ringback"
        return
    fi
    # The log line is the primary evidence and the tone reading follows it:
    # the 200 also sets the tone to 0.
    if ! rtp_rx_send 0 0; then
        flow_stop
        result_print FAIL "$name" "the first packet could not be injected"
        return
    fi
    if ! daemon_log_require "$flow_dir" "early media flowing" 5; then
        flow_stop
        result_print FAIL "$name" "the first packet of the early stream did not reach the line"
        return
    fi
    if ! stub_wait "$flow_dir" '.line[0].tone == 0' 5 ringback-stopped; then
        flow_stop
        result_print FAIL "$name" "the local ringback kept playing behind the first packet"
        return
    fi
    if ! status_wait "$flow_dir" CONNECTED 30 connected; then
        flow_stop
        result_print FAIL "$name" "daemon did not reach CONNECTED on the 200 with SDP"
        return
    fi
    if ! media_port_wait "$flow_dir" 5; then
        flow_stop
        result_print FAIL "$name" "stream 0 did not use the media port of the 183"
        return
    fi
    if ! stub_wait "$flow_dir" \
        '.line[0].stream[0].running == true and .line[0].stream[0].direction == 3' \
        5 sendrecv; then
        flow_stop
        result_print FAIL "$name" "the answered call did not run sendrecv"
        return
    fi
    # The scenario waits for the BYE behind a pause of 2000 ms, and a BYE
    # inside that pause is an unexpected message that aborts the call.
    # scenario_pause_get reads the first pause of the file, the 5000 ms one
    # behind the 183, so the wait is never the shorter of the two.
    pause_ms=$(scenario_pause_get "$sipp_dir/uas_183_sdp.xml")
    sleep "$(( (pause_ms + 1500) / 1000 ))"
    hook_set release || true
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not complete the BYE exchange"
        return
    fi
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        flow_stop
        result_print FAIL "$name" "daemon did not return to IDLE"
        return
    fi
    # An unreliable 18x takes no PRACK and leaves the answer provisional, so
    # neither the request nor the reliable-answer line may appear.
    pracks=$(prack_msg_count "$flow_dir" received "PRACK ")
    if (( pracks != 0 )); then
        flow_stop
        result_print FAIL "$name" "the plain 183 drew $pracks PRACK requests"
        return
    fi
    if grep -qF "reliable 1xx answer taken" "$flow_dir/daemon.log"; then
        flow_stop
        result_print FAIL "$name" "the daemon took the plain 183 as a reliable answer"
        return
    fi
    if ! uas_sipp_verdict "$flow_dir" 2; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "$sipp_verdict_detail, the plain 183 started the early stream under the local ringback, the first packet stopped the tone, no PRACK left the DUT and the 200 with SDP connected the call sendrecv on the 183 port"
}

# One call of the field INVITE, under one verstat value. The
# From carries the caller's number twice, in the quoted display name and in
# the user part, and each copy wears the parameter. What the line must receive
# is the bare number and no name at all: hdr_cid_get() drops a display name
# that is the number with a parameter behind it (src/proto/shared/hdr.c), and an
# empty name is what raises the GR-30 absence letter "O" in cid_fill()
# (src/line/line.c). Neither field is withheld, so both privacy flags stay
# clear; a withheld field would carry "P" in place of that letter.
#
# The request URI names the subscriber the config writes, the way the field
# message names the SIP user name of the line, so account_for_request() finds
# the line by name and not by the owner of the socket.
verstat_value_run()
{
    local name=$1
    local value=$2

    verstat_failure_detail=
    flow_dir=$artifact_root/$name/$value
    mkdir -p "$flow_dir" || {
        verstat_failure_detail="$value: artifact directory creation failed"
        return 1
    }
    config_dir=$flow_dir/config
    if ! config_write "$config_dir" "$sipp_port" "$daemon_port"; then
        verstat_failure_detail="$value: config write failed"
        return 1
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    if ! register_prime "$flow_dir" "$config_dir" "$name-$value"; then
        verstat_failure_detail="$value: registration setup failed"
        return 1
    fi
    if ! sipp_uac_start "$flow_dir" "$sipp_dir/uac_verstat_call.xml" \
        "$sipp_port" "$daemon_port" "$media_port" \
        "$script_dir/register_ooc.xml" 1 1 5551000 -key verstat "$value"; then
        verstat_failure_detail="$value: SIPp startup failed"
        return 1
    fi
    if ! status_wait "$flow_dir" RINGING 10 ringing; then
        verstat_failure_detail="$value: the daemon did not reach RINGING"
        return 1
    fi
    if ! stub_wait "$flow_dir" \
        '.line[0].ringing == true and .line[0].cid_number == "2015550123" and
         .line[0].cid_name == "" and .line[0].cid_blockinfo == "O" and
         .line[0].cid_number_private == false and
         .line[0].cid_name_private == false' \
        3 cid; then
        verstat_failure_detail="$value: the line did not ring with the number alone"
        return 1
    fi
    hook_set seize || true
    if ! status_wait "$flow_dir" CONNECTED 10 connected; then
        verstat_failure_detail="$value: the daemon did not answer into CONNECTED"
        return 1
    fi
    if ! media_port_wait "$flow_dir" 5; then
        verstat_failure_detail="$value: stream 0 did not use the SDP media port"
        return 1
    fi
    if ! sipp_finish_wait 30; then
        verstat_failure_detail="$value: SIPp did not finish its BYE exchange"
        return 1
    fi
    if (( process_status != 0 )); then
        verstat_failure_detail="$value: SIPp exited with status $process_status"
        return 1
    fi
    hook_set release || true
    if ! status_wait "$flow_dir" IDLE 5 idle; then
        verstat_failure_detail="$value: the daemon did not return to IDLE"
        return 1
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        verstat_failure_detail="$value: $sipp_verdict_detail"
        return 1
    fi
    return 0
}

# The three values of the field case, one call each. The rule under test is the
# shape of the display name and not the text of one value, so a run that
# passed on one value alone would say nothing about the other two.
verstat_run()
{
    local name=verstat-cid
    local value

    for value in No-TN-Validation TN-Validation-Passed TN-Validation-Failed; do
        if ! verstat_value_run "$name" "$value"; then
            flow_stop
            result_print FAIL "$name" "$verstat_failure_detail"
            return
        fi
        flow_stop
    done
    result_print PASS "$name" \
        "$sipp_verdict_detail, all three verstat values rang the line with the number 2015550123, no name, the absence letter O and neither privacy flag"
}

# ------------------------------------------------- the service route flows

# The hops the registrar of service-route-present grants, in the order it
# writes them into its one Service-Route header
# (test/sipp/uas_service_route.xml). They name a host of their own, which is
# the field shape. The next hop of the INVITE stays the target the
# registration selected, however a hop reads (src/proto/sip/reg.h). The
# INVITE arrives at SIPp, and its Route headers are read off the log here.
service_route_hops=('<sip:sbc1.example.net:5065;lr>' '<sip:sbc2.example.net:5065;lr>')
# Why one reading of a service route flow failed, for the verdict line. Both
# flows run several readings under one name, so the line has to name the one
# that broke.
service_route_failure_detail=

# How many received messages open with one start line. Every reader above
# takes one message at a time, and the route rides every copy of a request
# (RFC 3608 section 5.1). A challenged call carries two INVITEs, and a slow
# run adds a retransmission of one. A check of the first copy alone would
# pass while a later copy lost the header. CSeq is the control header,
# because every SIP request carries one (RFC 3261 8.1.1.5).
sipp_msg_count()
{
    local flow_dir=$1
    local direction=$2
    local start_line=$3
    local total=0

    while sipp_msg_header_get "$flow_dir" "$direction" "$start_line" CSeq \
        "$((total + 1))" >/dev/null; do
        total=$((total + 1))
    done

    printf '%u\n' "$total"
}

# A bounded poll of one command that answers true or false. The waits on a
# count from the wire or from a log use it, so each reading is the event
# itself and not a margin behind it.
condition_wait()
{
    local timeout_s=$1
    local deadline=$((SECONDS + timeout_s))

    shift
    while (( SECONDS < deadline )); do
        if "$@"; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

count_reached()
{
    local wanted=$1

    shift
    (( $("$@") >= wanted ))
}

# The same count, polled until it reaches a wanted value. A flow whose next
# gesture must stand behind a message of the scenario waits here: SIPp writes
# the log line when it puts the message on the wire.
#
# The count holds every copy of that start line, retransmissions included, so
# a wanted value above one names copies and not distinct messages. For a
# first message, which every caller waits for today, the two are the same.
sipp_msg_wait()
{
    condition_wait "$5" count_reached "$4" sipp_msg_count "$1" "$2" "$3"
}

# The CSeq number of one message, bare. It tells the ACK of a non-2xx from the
# ACK of a 2xx. The first takes the number of the INVITE it answers (RFC 3261
# section 17.1.1.3). The second takes the number of the INVITE that earned the
# 2xx. A challenged call therefore writes two numbers, and a retransmission of
# either keeps its own.
sipp_msg_cseq_get()
{
    local flow_dir=$1
    local direction=$2
    local start_line=$3
    local occurrence=${4:-1}
    local line

    line=$(sipp_msg_header_get "$flow_dir" "$direction" "$start_line" CSeq \
        "$occurrence") || return 1

    xfer_hdr_value "$line" | awk '{ print $1 }'
}

# The Route headers of one message: one hop per line, in the order the
# registrar granted them (RFC 3608 section 5.1). The Via reader above answers
# a token and xfer_hdr_check answers one value over one line; a service route
# is a list, and the order and the count are the assertion.
route_hdr_check()
{
    local flow_dir=$1
    local direction=$2
    local start_line=$3
    local occurrence=$4
    local label=$5
    shift 5
    local expected=("$@")
    local lines
    local line
    local seen=0

    lines=$(sipp_msg_header_get "$flow_dir" "$direction" "$start_line" Route \
        "$occurrence") || return 1

    while IFS= read -r line; do
        [[ -n $line ]] || continue
        printf '%s "%s" #%s %s\n' "$direction" "$start_line" "$occurrence" "$line" \
            >>"$flow_dir/route-headers.txt"
        # One hop per line is part of the assertion. A comma folds a route set
        # onto one header line (RFC 3261 7.3.1), and the reader above takes
        # that line whole. A folded line would pass on a value nothing
        # compared. The daemon renders one line per hop (src/proto/shared/hdr.c
        # route_hdrs_take), and this pins that shape.
        [[ $line != *,* ]] || return 1
        (( seen < ${#expected[@]} )) || return 1
        [[ $(xfer_hdr_value "$line") = "${expected[seen]}" ]] || return 1
        seen=$((seen + 1))
    done <<<"$lines"

    (( seen == ${#expected[@]} )) || return 1
    printf '%s: %u Route header(s) in the granted order\n' "$label" "$seen" \
        >>"$flow_dir/route-headers.txt"
}

# What one message of the log carried, for a verdict line that names a cause.
service_route_seen_get()
{
    local flow_dir=$1
    local start_line=$2
    local occurrence=$3
    local seen

    seen=$(sipp_msg_header_get "$flow_dir" received "$start_line" Route \
        "$occurrence" | tr '\n' ' ')
    printf '%s\n' "${seen:-no Route header}"
}

# Every INVITE the registrar saw. The challenged INVITE and its retry both
# carry the route, because the leg writes it into each build of the request
# (src/proto/sip/call.c invite_build).
service_route_invite_check()
{
    local flow_dir=$1
    local present=$2
    local total
    local occ=1

    total=$(sipp_msg_count "$flow_dir" received "INVITE ")
    if (( total < 1 )); then
        service_route_failure_detail="no INVITE reached the registrar"
        return 1
    fi
    if (( present == 1 && total < 2 )); then
        service_route_failure_detail="the registrar saw $total INVITE, not the challenged one and its retry"
        return 1
    fi

    while (( occ <= total )); do
        if (( present == 0 )); then
            if ! xfer_hdr_absent_check "$flow_dir" received "INVITE " Route "$occ"; then
                service_route_failure_detail="the 200 granted no service route and INVITE $occ of $total still carried $(service_route_seen_get "$flow_dir" "INVITE " "$occ")"
                return 1
            fi
        elif ! route_hdr_check "$flow_dir" received "INVITE " "$occ" \
            "invite-$occ" "${service_route_hops[@]}"; then
            service_route_failure_detail="INVITE $occ of $total carried '$(service_route_seen_get "$flow_dir" "INVITE " "$occ")', not one Route header per hop for ${service_route_hops[*]} in that order"
            return 1
        fi
        occ=$((occ + 1))
    done

    return 0
}

# The ACKs, told apart by their CSeq number and not by their place in the log.
# The ACK of the 407 mirrors the request it answers, so it carries the hops
# (src/proto/sip/call.c req_mirror_build). The ACK of a 2xx is built from the
# dialog and carries the dialog route set alone (src/proto/sip/dialog.c
# route_update). This peer records no Record-Route, so that ACK carries
# nothing.
service_route_ack_check()
{
    local flow_dir=$1
    local mirror_cseq=$2
    local total
    local occ=1
    local cseq
    local mirrors=0

    total=$(sipp_msg_count "$flow_dir" received "ACK ")
    if (( total < 1 )); then
        service_route_failure_detail="no ACK reached the registrar"
        return 1
    fi

    while (( occ <= total )); do
        cseq=$(sipp_msg_cseq_get "$flow_dir" received "ACK " "$occ") || return 1
        if [[ -n $mirror_cseq && $cseq = "$mirror_cseq" ]]; then
            if ! route_hdr_check "$flow_dir" received "ACK " "$occ" \
                "ack-$occ" "${service_route_hops[@]}"; then
                service_route_failure_detail="the ACK of the 407 (CSeq $cseq) carried '$(service_route_seen_get "$flow_dir" "ACK " "$occ")', not the hops of the request it mirrors"
                return 1
            fi
            mirrors=$((mirrors + 1))
        elif ! xfer_hdr_absent_check "$flow_dir" received "ACK " Route "$occ"; then
            service_route_failure_detail="the ACK of the 2xx (CSeq $cseq) carried $(service_route_seen_get "$flow_dir" "ACK " "$occ")"
            return 1
        fi
        occ=$((occ + 1))
    done

    if [[ -n $mirror_cseq ]] && (( mirrors == 0 )); then
        service_route_failure_detail="no ACK carried the CSeq $mirror_cseq of the challenged INVITE"
        return 1
    fi

    return 0
}

# What the two flows read off the wire after SIPp ends. present is 1 for
# the registrar that granted a route and 0 for the one that granted none.
service_route_wire_check()
{
    local flow_dir=$1
    local present=$2
    local mirror_cseq=

    # The REGISTER is read under both flows. A REGISTER never carries the
    # route it asks for (RFC 3608 section 5.1), and that header on a REGISTER
    # is a field defect.
    if ! xfer_hdr_absent_check "$flow_dir" received "REGISTER " Route; then
        service_route_failure_detail="the REGISTER carried $(service_route_seen_get "$flow_dir" "REGISTER " 1)"
        return 1
    fi

    service_route_invite_check "$flow_dir" "$present" || return 1

    # The challenged INVITE is the first one, and its number is what names the
    # ACK that mirrors it. The flow with no challenge has no such ACK.
    if (( present == 1 )); then
        mirror_cseq=$(sipp_msg_cseq_get "$flow_dir" received "INVITE " 1) || return 1
    fi
    service_route_ack_check "$flow_dir" "$mirror_cseq" || return 1

    # The BYE is in-dialog, like the ACK of the 2xx above.
    if ! xfer_hdr_absent_check "$flow_dir" received "BYE " Route; then
        service_route_failure_detail="the in-dialog BYE carried $(service_route_seen_get "$flow_dir" "BYE " 1)"
        return 1
    fi

    return 0
}

# One outbound call from a registered line, which is the shape the field fault
# needs: the registrar answers first and the INVITE behind it carries what
# that answer granted. codec_inbound_run drives an inbound call and passes no
# out-of-call file. SIPp therefore answers the REGISTER from the default
# register_ooc.xml, which grants no Service-Route. The flow is modeled on
# flow_a_run instead, which dials out through flow_prepare and the stub hook.
#
# The registration is waited for and not assumed. The route is stored by the
# 200 (src/proto/sip/reg.c resp_ok). An INVITE placed in front of that answer
# would read an empty block, and the flow would pin the wrong thing.
service_route_run()
{
    local name=$1
    local scenario=$2
    local present=$3
    local detail=$4

    service_route_failure_detail=
    if ! flow_prepare "$name" "$sipp_dir/$scenario.xml"; then
        flow_stop
        result_print FAIL "$name" "startup failed"
        return
    fi
    if ! status_expr_wait "$flow_dir" '.lines[0].state == "registered"' 20 \
        registered; then
        flow_stop
        result_print FAIL "$name" "the line did not register against the scenario"
        return
    fi
    if ! outbound_dial_start "$flow_dir"; then
        flow_stop
        result_print FAIL "$name" "off-hook dial setup failed"
        return
    fi
    if ! status_wait "$flow_dir" CONNECTED 20 connected; then
        flow_stop
        result_print FAIL "$name" "daemon did not reach CONNECTED"
        return
    fi
    hook_set release || true
    if ! sipp_finish_wait 30; then
        flow_stop
        result_print FAIL "$name" "SIPp did not finish the BYE exchange"
        return
    fi
    if ! status_wait "$flow_dir" IDLE 10 idle; then
        flow_stop
        result_print FAIL "$name" "daemon did not return to IDLE"
        return
    fi
    if ! service_route_wire_check "$flow_dir" "$present"; then
        flow_stop
        result_print FAIL "$name" "$service_route_failure_detail"
        return
    fi
    # Two successful instances, not one: the scenario answers the
    # registration and the call, and the registration is the precondition of
    # the route rather than noise this flow absorbs.
    if ! uas_sipp_verdict "$flow_dir" 2; then
        flow_stop
        result_print FAIL "$name" "$sipp_verdict_detail"
        return
    fi
    detail="$sipp_verdict_detail, $detail"
    flow_stop
    result_print PASS "$name" "$detail"
}

# ---------------------------------------------- the netifd stub and the flap

# The addresses of wan-flap-outage. The daemon takes the address netifd
# reports as authoritative for the Via and the Contact of an IPv4 line
# (src/proto/sip/account.c account_local_host), and the sockets stay bound to
# the wildcard with SO_BINDTODEVICE (src/proto/sip/transport.h), so no
# address is added to the loopback: 127.0.0.0/8 is local to it by construction.
# The boot address is the one the route probe answers with before the first
# interface status arrives.
wan_flap_addr_boot=127.0.0.1
wan_flap_addr_one=127.0.0.2
wan_flap_addr_two=127.0.0.3
wan_flap_addr_start=127.0.0.4
wan_flap_addr_back=127.0.0.5
# SIPp's own run timeout for this flow. The bounded waits that run while
# SIPp lives add up to 160 s (the daemon start, the stub start and the eight
# waits of the move part). A SIPp that left before them would take the
# verdict away from the wait that failed. The operator's own value wins.
wan_flap_sipp_timeout=200
# The refresh the granted expiry of the scenario earns: ten seconds grant
# five (src/proto/sip/reg.c sip_reg_refresh_secs). The window around it is what
# tells one REGISTER from a second one behind it.
wan_flap_refresh_min_ms=4000
wan_flap_refresh_max_ms=9000
# Why one part of the flow failed, for the verdict line. The flow runs two
# daemons under one name, so the line has to name the part that broke.
wan_flap_failure_detail=
# Which ucode modules are absent, empty when the flow can run.
wan_flap_module_detail=

# The ucode modules the stub needs. The harness runs a bare ubusd, so the
# stub is the only netifd the daemon can ask, and it imports ubus, uloop and
# fs. A build host does not always ship them, and UVOICE_UCODE_MODDIR names
# a directory that holds them: the variable and the probe of the CLI suite
# (test/cli/run_cli_tests.sh).
#
# An absent module fails the flow and names itself. A run that has to cover
# the WAN address change covers nothing without the stub, so the verdict is
# the one that counts in the summary and in the exit status.
netifd_stub_check()
{
    local moddir=${UVOICE_UCODE_MODDIR:-}
    local module_args=()
    local absent=()
    local module

    wan_flap_module_detail=
    if ! command -v ucode >/dev/null 2>&1; then
        wan_flap_module_detail='no ucode interpreter'
        return 1
    fi
    if [[ -n $moddir ]]; then
        module_args=(-L "$moddir")
    fi

    # One import per module, so the verdict names the module that is absent
    # and not the whole list.
    for module in ubus uloop fs; do
        ucode ${module_args[@]+"${module_args[@]}"} \
            -e "import * as m from '$module';" >/dev/null 2>&1 && continue
        absent+=("$module")
    done
    if (( ${#absent[@]} == 0 )); then
        return 0
    fi

    wan_flap_module_detail="ucode modules absent: ${absent[*]}"
    if [[ -n $moddir ]]; then
        wan_flap_module_detail+="; UVOICE_UCODE_MODDIR=$moddir holds none of them"
    else
        wan_flap_module_detail+='; UVOICE_UCODE_MODDIR names no directory'
    fi

    return 1
}

# The answer file of the stub, rewritten between the steps of the sequence.
# answer is "ok" or "error", and the second makes the status method end on a
# ubus status instead of a reply, which is the failed invoke of the recovery
# timer (src/proto/sip/account.c netifd_query). l3_device is the loopback, the
# netdev these addresses live on, so the line scopes its socket to a device that
# exists (acct_bind_device).
#
# The write is a rename and not a truncate. The stub reads the file inside a
# ubus call the daemon is waiting on, and an ifdown drives a dump query of
# its own (src/proto/sip/account.c netifd_event_cb), so a plain redirection has
# a window in which the stub reads half a file: measured here, that window
# refused one dump and armed the recovery timer the flow measures.
netifd_state_write()
{
    local state_file=$1
    local address=$2
    local answer=${3:-ok}

    printf '{"interface":"wan","up":true,"l3_device":"lo","address":"%s","mask":32,"answer":"%s"}\n' \
        "$address" "$answer" >"$state_file.next" || return 1
    mv "$state_file.next" "$state_file"
}

# The stub, on the ubus socket of the running flow. It is a process of its
# own because the daemon's status query is synchronous: the runner cannot
# answer a call the daemon makes while it waits for it.
netifd_stub_start()
{
    local flow_dir=$1
    local state_file=$2
    local moddir=${UVOICE_UCODE_MODDIR:-}
    local deadline
    local module_args=()

    if [[ -n $moddir ]]; then
        module_args=(-L "$moddir")
    fi

    ucode ${module_args[@]+"${module_args[@]}"} "$script_dir/netifd_stub.uc" \
        "$current_socket" "$state_file" >"$flow_dir/netifd-stub.log" 2>&1 &
    current_stub_pid=$!

    # The object has to be on the bus before the flow drives an event at it:
    # the daemon looks it up per query, and a lookup that misses answers
    # nothing and arms nothing (src/proto/sip/account.c netifd_retry_needed).
    deadline=$((SECONDS + 10))
    while (( SECONDS < deadline )); do
        if ubus -s "$current_socket" list 2>/dev/null |
            grep -qx 'network.interface.wan'; then
            return 0
        fi
        if ! kill -0 "$current_stub_pid" 2>/dev/null; then
            return 1
        fi
        sleep 0.1
    done

    return 1
}

netifd_stub_stop()
{
    process_stop "$current_stub_pid"
    current_stub_pid=
}

# One netifd event, under the name the daemon subscribed to. The handler is
# registered on the event name and not on the interface object
# (src/proto/sip/account.c accounts_ubus_attach), so the event arrives whether
# that object exists or not.
netifd_event_send()
{
    local action=$1
    local iface=${2:-wan}

    ubus -s "$current_socket" send network.interface \
        "{\"action\":\"$action\",\"interface\":\"$iface\"}"
}

# Every REGISTER the registrar received, one line each, in log order:
# "<n>|<stamp>|<CSeq number>|<Call-ID>|<Via value>|<Contact values>". The
# stamp is the one SIPp wrote in front of the message ("YYYY-MM-DD
# HH:MM:SS.ffffff"), and the Contact values of one message are joined by a
# space, which none of them carries.
#
# No reader above answers this. sipp_msg_header_get chooses a message by an
# occurrence count, and the outage of this flow puts an unpredictable number
# of retransmissions into the log in front of the message that matters.
register_wire_dump()
{
    local flow_dir=$1

    awk '
        function value_of(line,   v) {
            v = substr(line, index(line, ":") + 1)
            sub(/^[ \t]+/, "", v)
            sub(/[ \t]+$/, "", v)
            return v
        }
        function name_is(line, name) {
            return tolower(substr(line, 1, length(name))) == name
        }
        function msg_end() {
            if (matched)
                printf "%u|%s|%s|%s|%s|%s\n", ++n, at, cseq, callid, via, contacts
            matched = 0
            cseq = ""
            callid = ""
            via = ""
            contacts = ""
        }
        # The rule of the log: the stamp stands on its own line in front of
        # the marker, so it is latched per message at the marker below.
        /^-+ [0-9]{4}-[0-9]{2}-[0-9]{2} / { stamp = $2 " " $3; next }
        /(UDP|TCP|TLS) message received/ { msg_end(); at = stamp; dir = "received"; first = 1; next }
        /(UDP|TCP|TLS) message sent/ { msg_end(); at = stamp; dir = "sent"; first = 1; next }
        first {
            if ($0 ~ /^[[:space:]]*$/)
                next
            line = $0
            gsub(/\r/, "", line)
            matched = (dir == "received" && index(line, "REGISTER ") == 1)
            first = 0
            next
        }
        matched {
            line = $0
            gsub(/\r/, "", line)
            if (name_is(line, "cseq:")) {
                split(value_of(line), field, /[ \t]+/)
                cseq = field[1]
            } else if (name_is(line, "call-id:")) {
                callid = value_of(line)
            } else if (name_is(line, "via:") && via == "") {
                via = value_of(line)
            } else if (name_is(line, "contact:")) {
                contacts = contacts (contacts == "" ? "" : " ") value_of(line)
            }
        }
        END { msg_end() }
    ' "$flow_dir/sipp-messages.log" 2>/dev/null
}

# The distinct REGISTER transactions, the first copy of each, keyed on the
# Call-ID and the CSeq number. A retransmission repeats the whole text (RFC
# 3261 17.1.2.2) and SIPp logs every copy it reads, so a count over the raw
# log counts the outage and not the exchanges.
register_txn_dump()
{
    local flow_dir=$1

    register_wire_dump "$flow_dir" |
        awk -F '|' '!(($4 "/" $3) in seen) { seen[$4 "/" $3] = 1; print }'
}

register_txn_count()
{
    local flow_dir=$1

    register_txn_dump "$flow_dir" | grep -c . || true
}

# One field of the nth REGISTER transaction: 2 is the stamp, 3 the CSeq
# number, 4 the Call-ID, 5 the Via and 6 the Contact list.
register_txn_field()
{
    local flow_dir=$1
    local nth=$2
    local field=$3

    register_txn_dump "$flow_dir" | sed -n "${nth}p" | cut -d '|' -f "$field"
}

# The milliseconds between two REGISTER transactions, as SIPp stamped the
# first copy of each. It is what tells one REGISTER from a second one behind
# it: a refresh stands one refresh interval behind the exchange it follows,
# and anything the daemon sends in between lands inside that interval.
#
# A run that crosses midnight between the two stamps reads the difference
# against the next day, the rule xfer_grace_check states.
register_gap_get()
{
    local flow_dir=$1
    local first=$2
    local second=$3
    local a
    local b

    a=$(register_txn_field "$flow_dir" "$first" 2)
    b=$(register_txn_field "$flow_dir" "$second" 2)
    if [[ -z $a || -z $b ]]; then
        return 1
    fi

    awk -v a="$a" -v b="$b" '
        function ms(s,    p, t) {
            split(s, p, " ")
            split(p[2], t, /[:.]/)
            return ((t[1] * 60 + t[2]) * 60 + t[3]) * 1000 + t[4] / 1000
        }
        BEGIN {
            d = ms(b) - ms(a)
            if (d < 0)
                d += 86400000
            printf "%d", d
        }'
}

# How many copies of one REGISTER transaction the registrar saw.
register_copy_count()
{
    local flow_dir=$1
    local cseq=$2

    register_wire_dump "$flow_dir" |
        awk -F '|' -v want="$cseq" '$3 == want' | grep -c . || true
}

# A bounded wait for the nth copy of one REGISTER. An unanswered non-INVITE
# request is retransmitted at T1 and at 2*T1 (RFC 3261 17.1.2.2), so the
# copies are what says the outage lasted and the registrar stayed silent
# through it. The flow waits on them instead of sleeping the window out, and
# the readers above key on the CSeq number for the same reason.
register_copy_wait()
{
    condition_wait "$4" count_reached "$3" register_copy_count "$1" "$2"
}

# A bounded wait for the nth REGISTER transaction. A refresh shows on the
# wire, so the flow waits there and never on a delay of its own.
register_txn_wait()
{
    condition_wait "$3" count_reached "$2" register_txn_count "$1"
}

# How often the daemon armed its recovery timer. The line is written once per
# delay and not once per attempt (src/proto/sip/account.c acct_recover_arm), so
# this counts the steps of the ladder.
recover_arm_count()
{
    local flow_dir=$1

    grep -c "voice recovery armed" "$flow_dir/daemon.log" 2>/dev/null || true
}

# The delay the last step of the ladder named. The first step of a ladder is
# NET_IFACE_RECOVER_MS, and a second fault behind a disarm starts a fresh
# ladder (src/net/iface.c recover_disarm), so the delay of the last line says
# whether the ladder really started again.
recover_arm_last_ms()
{
    local flow_dir=$1

    grep "voice recovery armed" "$flow_dir/daemon.log" 2>/dev/null |
        tail -1 | sed -n 's/.*next attempt in \([0-9]\{1,\}\) ms.*/\1/p'
}

# How many status calls the stub refused. The daemon reads the refusal, not
# the runner, so this is what says a fault the flow wrote really reached it:
# an event the daemon did not handle yet leaves the count where it was.
netifd_refusal_count()
{
    local flow_dir=$1

    grep -c "^netifd stub: status: refused" "$flow_dir/netifd-stub.log" \
        2>/dev/null || true
}

# A bounded wait for one fault to arrive and to be answered: the stub
# refused the call, and the daemon wrote the step of the ladder that the
# refusal earns. Both counts are needed. The refusal alone leaves the arm
# unread, and the arm alone is satisfied by the step an earlier fault wrote,
# because the log is read whole.
recover_fault_seen()
{
    local flow_dir=$1
    local arms=$2
    local refusals=$3

    count_reached "$refusals" netifd_refusal_count "$flow_dir" &&
        count_reached "$arms" recover_arm_count "$flow_dir"
}

recover_fault_wait()
{
    condition_wait "$4" recover_fault_seen "$1" "$2" "$3"
}

# The ladder ended: no further step inside a whole period of it. A run that
# settled pays the window once; a run whose ladder runs on fails as soon as
# its next step is written, which is what the window is long enough for.
recover_arm_quiet_check()
{
    local flow_dir=$1
    local expected=$2
    local window_s=$3
    local deadline=$((SECONDS + window_s))

    while (( SECONDS < deadline )); do
        if (( $(recover_arm_count "$flow_dir") != expected )); then
            return 1
        fi
        sleep 0.2
    done

    return 0
}

# What one REGISTER carried. The Via sent-by and every Contact are read
# together, because the claim is about one address in three places: the
# sent-by, the binding the line asks for and the binding it gives up.
# removed names the address of the second Contact, which has to carry
# expires=0 (RFC 3261 10.2.2); an empty value requires the Contact to stand
# alone with no expires=0 anywhere in the message.
wan_flap_txn_check()
{
    local record=$1
    local label=$2
    local addr=$3
    local removed=${4:-}
    local contact_list=()
    local idx at cseq callid via contacts

    IFS='|' read -r idx at cseq callid via contacts <<<"$record"
    read -r -a contact_list <<<"$contacts"

    if [[ $via != *"$addr:$daemon_port;"* ]]; then
        wan_flap_failure_detail="the $label REGISTER named '$via' in its Via, not $addr:$daemon_port"
        return 1
    fi
    if [[ -z $removed ]]; then
        if (( ${#contact_list[@]} != 1 )) || [[ $contacts != *"@$addr:$daemon_port>"* ]]; then
            wan_flap_failure_detail="the $label REGISTER carried '$contacts', not the single Contact of $addr"
            return 1
        fi
        if [[ $contacts == *expires=0* ]]; then
            wan_flap_failure_detail="the $label REGISTER still carried a removal Contact: $contacts"
            return 1
        fi

        return 0
    fi
    if (( ${#contact_list[@]} != 2 )); then
        wan_flap_failure_detail="the $label REGISTER carried '$contacts', not the new Contact and the removal of $removed"
        return 1
    fi
    if [[ ${contact_list[0]} != *"@$addr:$daemon_port>" ]]; then
        wan_flap_failure_detail="the $label REGISTER asked for '${contact_list[0]}', not the plain Contact of $addr"
        return 1
    fi
    if [[ ${contact_list[1]} != *"@$removed:$daemon_port>;expires=0" ]]; then
        wan_flap_failure_detail="the $label REGISTER removed '${contact_list[1]}', not $removed with expires=0"
        return 1
    fi

    return 0
}

# The five exchanges of the move part, read off the message log once SIPp has
# ended. The order of the addresses on the wire is read as well: the exchange
# the change dropped was aborted with its transaction (src/proto/sip/reg.c
# attempt_prepare through txn_drop), so no copy of it may appear behind the
# first REGISTER of the new address.
wan_flap_wire_check()
{
    local flow_dir=$1
    local records=()
    local copies=()
    local record
    local first_two=0
    local last_one=0
    local idx at cseq callid via contacts
    local boot_id=
    local move_id=
    local refresh_cseq=0
    local move_cseq=0
    local gap=

    mapfile -t records < <(register_txn_dump "$flow_dir")
    register_txn_dump "$flow_dir" >"$flow_dir/register-transactions.txt"
    register_wire_dump "$flow_dir" >"$flow_dir/register-wire.txt"

    if (( ${#records[@]} != 5 )); then
        wan_flap_failure_detail="the registrar saw ${#records[@]} REGISTER transactions, not the five of the sequence"
        return 1
    fi

    wan_flap_txn_check "${records[0]}" "first" "$wan_flap_addr_boot" || return 1
    wan_flap_txn_check "${records[1]}" "first stub" "$wan_flap_addr_one" \
        "$wan_flap_addr_boot" || return 1
    wan_flap_txn_check "${records[2]}" "unanswered refresh" "$wan_flap_addr_one" || return 1
    wan_flap_txn_check "${records[3]}" "address change" "$wan_flap_addr_two" \
        "$wan_flap_addr_one" || return 1
    wan_flap_txn_check "${records[4]}" "refresh behind the change" "$wan_flap_addr_two" || return 1

    IFS='|' read -r idx at cseq callid via contacts <<<"${records[0]}"
    boot_id=$callid
    IFS='|' read -r idx at cseq callid via contacts <<<"${records[2]}"
    refresh_cseq=$cseq
    IFS='|' read -r idx at cseq callid via contacts <<<"${records[3]}"
    move_id=$callid
    move_cseq=$cseq

    if [[ $move_id != "$boot_id" ]]; then
        wan_flap_failure_detail="the REGISTER after the change carried Call-ID $move_id, not the $boot_id the line already had"
        return 1
    fi
    if (( move_cseq <= refresh_cseq )); then
        wan_flap_failure_detail="the REGISTER after the change carried CSeq $move_cseq, not one above the $refresh_cseq of the refresh it replaced"
        return 1
    fi

    # One REGISTER, not one and another behind it. The transaction that
    # follows an answered exchange is its refresh, and a refresh stands one
    # refresh interval behind it; anything the daemon sent in between would
    # take that place and shorten the gap. The pair before the outage is read
    # the same way, so the reading is pinned on an exchange the change did
    # not touch as well.
    for record in "2 3" "4 5"; do
        gap=$(register_gap_get "$flow_dir" ${record% *} ${record#* }) || gap=
        printf 'transactions %s: %s ms apart\n' "$record" "${gap:-unreadable}" \
            >>"$flow_dir/register-gaps.txt"
        if [[ -z $gap ]]; then
            wan_flap_failure_detail="the log carries no stamp for the REGISTER transactions $record"
            return 1
        fi
        if (( gap < wan_flap_refresh_min_ms || gap > wan_flap_refresh_max_ms )); then
            wan_flap_failure_detail="the REGISTER transactions $record stand $gap ms apart, not the refresh interval of the granted expiry: a second REGISTER rode inside it"
            return 1
        fi
    done

    mapfile -t copies < <(register_wire_dump "$flow_dir")
    for record in "${copies[@]}"; do
        IFS='|' read -r idx at cseq callid via contacts <<<"$record"
        if [[ $via == *"$wan_flap_addr_two:$daemon_port;"* ]] && (( first_two == 0 )); then
            first_two=$idx
        fi
        if [[ $via == *"$wan_flap_addr_one:$daemon_port;"* ]]; then
            last_one=$idx
        fi
    done
    if (( first_two == 0 )) || (( last_one > first_two )); then
        wan_flap_failure_detail="a REGISTER of $wan_flap_addr_one reached the registrar behind the first one of $wan_flap_addr_two"
        return 1
    fi

    return 0
}

# The first daemon of the flow: the field sequence of a WAN address change.
#
# The line comes up with no network.interface object at all, which is the
# state every other flow of this harness runs in, and registers on the
# address the route probe answers with. The stub then appears with the first
# address and an ifup carries it in. The registrar goes silent inside its own
# scenario, so the refresh stands unanswered while the interface goes down
# and comes back on a second address. What the registrar sees then is the
# whole claim of the unit: one REGISTER, on the Call-ID the line already had,
# with the new address in the Via and the Contact and the Contact the
# registrar still holds beside it with expires=0.
wan_flap_move_run()
{
    local name=$1
    local state_file
    local refresh_cseq
    local boot_id
    local moved_id

    wan_flap_failure_detail=
    flow_dir=$artifact_root/$name/move
    mkdir -p "$flow_dir" || {
        wan_flap_failure_detail="move: artifact directory creation failed"
        return 1
    }
    config_dir=$flow_dir/config
    state_file=$flow_dir/netifd-state.json
    if ! config_write "$config_dir" "$sipp_port" "$daemon_port"; then
        wan_flap_failure_detail="move: config write failed"
        return 1
    fi
    capture_start "$flow_dir" "$sipp_port" || true
    # The timeout rides one call and is restored behind it, so no other flow
    # of the run inherits the long one.
    if ! SIPP_TIMEOUT_SECONDS=${SIPP_TIMEOUT_SECONDS:-$wan_flap_sipp_timeout} \
        sipp_start "$flow_dir" "$sipp_dir/uas_register_outage.xml" "$sipp_port" \
        "$daemon_port" "$media_port" 1; then
        wan_flap_failure_detail="move: SIPp startup failed"
        return 1
    fi
    if ! daemon_start "$flow_dir" "$config_dir" "$name-move"; then
        wan_flap_failure_detail="move: daemon startup failed"
        return 1
    fi
    if ! status_expr_wait "$flow_dir" '.lines[0].state == "registered"' 15 boot; then
        wan_flap_failure_detail="move: the line did not register before the stub appeared"
        return 1
    fi
    # An absent object is no fault of the path: no attempt can make it
    # appear, so nothing is armed for it (src/proto/sip/account.c
    # netifd_retry_needed).
    if (( $(recover_arm_count "$flow_dir") != 0 )); then
        wan_flap_failure_detail="move: the daemon armed its recovery timer with no network.interface object on the bus"
        return 1
    fi
    boot_id=$(register_txn_field "$flow_dir" 1 4)
    if ! netifd_state_write "$state_file" "$wan_flap_addr_one"; then
        wan_flap_failure_detail="move: the state file of $wan_flap_addr_one could not be written"
        return 1
    fi
    if ! netifd_stub_start "$flow_dir" "$state_file"; then
        wan_flap_failure_detail="move: the netifd stub did not publish network.interface.wan"
        return 1
    fi
    if ! netifd_event_send ifup; then
        wan_flap_failure_detail="move: the ifup event was not accepted"
        return 1
    fi
    # The Call-ID is read off the wire and not off the state the line
    # reaches. A line that rebuilt its registration reaches the same state
    # under a new Call-ID, and only the registrar sees which one it is.
    if ! register_txn_wait "$flow_dir" 2 15; then
        wan_flap_failure_detail="move: no REGISTER followed the first ifup"
        return 1
    fi
    moved_id=$(register_txn_field "$flow_dir" 2 4)
    if [[ $moved_id != "$boot_id" ]]; then
        wan_flap_failure_detail="move: the REGISTER of $wan_flap_addr_one carried Call-ID $moved_id, not the $boot_id the line already had"
        return 1
    fi
    if ! status_expr_wait "$flow_dir" \
        ".lines[0].state == \"registered\" and .network.address == \"$wan_flap_addr_one\" and
         ((.lines[0].contact // \"\") | contains(\"$wan_flap_addr_one\"))" \
        15 address-one; then
        wan_flap_failure_detail="move: the line did not register on $wan_flap_addr_one after the ifup"
        return 1
    fi
    # The refresh of the expiry the registrar granted, which it answers with
    # nothing: the outage of the field sequence starts here.
    if ! register_txn_wait "$flow_dir" 3 15; then
        wan_flap_failure_detail="move: the refresh REGISTER did not reach the silent registrar"
        return 1
    fi
    # The outage outlives two retransmissions of that refresh, which is what
    # puts the line in the state of the field case: a binding the registrar
    # still holds and an exchange that is going nowhere. It stays far inside
    # timer F (32 s at the default T1), because a binding that lapsed would
    # take the full rebuild instead of the move.
    refresh_cseq=$(register_txn_field "$flow_dir" 3 3)
    if ! register_copy_wait "$flow_dir" "$refresh_cseq" 3 15; then
        wan_flap_failure_detail="move: the unanswered refresh was not retransmitted through the outage"
        return 1
    fi
    if ! netifd_event_send ifdown; then
        wan_flap_failure_detail="move: the ifdown event was not accepted"
        return 1
    fi
    if ! netifd_state_write "$state_file" "$wan_flap_addr_two"; then
        wan_flap_failure_detail="move: the state file of $wan_flap_addr_two could not be written"
        return 1
    fi
    if ! netifd_event_send ifup; then
        wan_flap_failure_detail="move: the second ifup event was not accepted"
        return 1
    fi
    if ! register_txn_wait "$flow_dir" 4 15; then
        wan_flap_failure_detail="move: no REGISTER followed the address change"
        return 1
    fi
    moved_id=$(register_txn_field "$flow_dir" 4 4)
    if [[ $moved_id != "$boot_id" ]]; then
        wan_flap_failure_detail="move: the REGISTER of $wan_flap_addr_two carried Call-ID $moved_id, not the $boot_id the line already had"
        return 1
    fi
    if ! status_expr_wait "$flow_dir" \
        ".lines[0].state == \"registered\" and .network.address == \"$wan_flap_addr_two\" and
         ((.lines[0].contact // \"\") | contains(\"$wan_flap_addr_two\"))" \
        15 address-two; then
        wan_flap_failure_detail="move: the line did not register on $wan_flap_addr_two after the change"
        return 1
    fi
    if ! register_txn_wait "$flow_dir" 5 15; then
        wan_flap_failure_detail="move: no refresh followed the address change"
        return 1
    fi
    if ! sipp_finish_wait 20; then
        wan_flap_failure_detail="move: SIPp did not complete the five exchanges"
        return 1
    fi
    if ! uas_sipp_verdict "$flow_dir" 1; then
        wan_flap_failure_detail="move: $sipp_verdict_detail"
        return 1
    fi
    if ! wan_flap_wire_check "$flow_dir"; then
        wan_flap_failure_detail="move: $wan_flap_failure_detail"
        return 1
    fi
    if (( $(recover_arm_count "$flow_dir") != 0 )); then
        wan_flap_failure_detail="move: the daemon armed its recovery timer during the flap"
        return 1
    fi

    return 0
}

# The second daemon of the flow: the netifd query that ends on ubus, which is
# the other half of the retry and the one path no unit test reaches. The unit
# suite has no ubus connection at all, so the arm rule of a failed invoke and
# the end rule of a lookup that finds nothing (src/proto/sip/account.c
# netifd_query, acct_recover_lookup_failed) have this part as their only oracle.
#
# It drives no SIPp and places no call. What it asserts is the ladder: one
# step for the refused invoke, the address picked up by the attempt behind it
# with no netifd event of any kind, the timer disarmed, a second fault that
# starts the ladder again from its first delay, and the end of that ladder
# once the object leaves the bus.
#
# Every fault is counted and never merely matched. The log is read whole, so
# the step of the first fault answers a plain search for the second one, and
# a daemon that armed once would pass. The stub's own refusals are counted
# beside the steps, so an event the daemon did not handle yet cannot pass
# for a fault it answered.
wan_flap_recover_run()
{
    local name=$1
    local state_file
    local last_ms

    wan_flap_failure_detail=
    flow_dir=$artifact_root/$name/recover
    mkdir -p "$flow_dir" || {
        wan_flap_failure_detail="recover: artifact directory creation failed"
        return 1
    }
    config_dir=$flow_dir/config
    state_file=$flow_dir/netifd-state.json
    if ! config_write "$config_dir" "$sipp_port" "$daemon_port"; then
        wan_flap_failure_detail="recover: config write failed"
        return 1
    fi
    if ! daemon_start "$flow_dir" "$config_dir" "$name-recover"; then
        wan_flap_failure_detail="recover: daemon startup failed"
        return 1
    fi
    if ! netifd_state_write "$state_file" "$wan_flap_addr_start"; then
        wan_flap_failure_detail="recover: the state file of $wan_flap_addr_start could not be written"
        return 1
    fi
    if ! netifd_stub_start "$flow_dir" "$state_file"; then
        wan_flap_failure_detail="recover: the netifd stub did not publish network.interface.wan"
        return 1
    fi
    if ! netifd_event_send ifup; then
        wan_flap_failure_detail="recover: the ifup event was not accepted"
        return 1
    fi
    if ! status_expr_wait "$flow_dir" \
        ".network.address == \"$wan_flap_addr_start\" and .network.l3_device == \"lo\"" \
        20 start; then
        wan_flap_failure_detail="recover: the daemon did not take $wan_flap_addr_start from the stub"
        return 1
    fi
    if ! netifd_state_write "$state_file" "$wan_flap_addr_start" error; then
        wan_flap_failure_detail="recover: the refusal could not be written"
        return 1
    fi
    if ! netifd_event_send update; then
        wan_flap_failure_detail="recover: the update event was not accepted"
        return 1
    fi
    if ! recover_fault_wait "$flow_dir" 1 1 15; then
        wan_flap_failure_detail="recover: the first refused status query armed no attempt, $(recover_arm_count "$flow_dir") steps behind $(netifd_refusal_count "$flow_dir") refusals"
        return 1
    fi
    last_ms=$(recover_arm_last_ms "$flow_dir")
    if [[ $last_ms != 5000 ]]; then
        wan_flap_failure_detail="recover: the first step of the ladder named ${last_ms:-no} ms, not the 5000 ms of the first delay"
        return 1
    fi
    if ! netifd_state_write "$state_file" "$wan_flap_addr_back"; then
        wan_flap_failure_detail="recover: the state file of $wan_flap_addr_back could not be written"
        return 1
    fi
    # No event follows. The attempt repeats the query that failed
    # (src/proto/sip/account.c acct_recover_cb), so the address arrives on the
    # timer.
    if ! status_expr_wait "$flow_dir" \
        ".network.address == \"$wan_flap_addr_back\" and
         ((.lines[0].contact // \"\") | contains(\"$wan_flap_addr_back\"))" \
        20 recovered; then
        wan_flap_failure_detail="recover: the recovery attempt did not pick $wan_flap_addr_back up"
        return 1
    fi
    if ! recover_arm_quiet_check "$flow_dir" 1 8; then
        wan_flap_failure_detail="recover: the ladder ran on behind the answer, $(recover_arm_count "$flow_dir") steps"
        return 1
    fi
    # The same fault again. The daemon disarmed behind the answer, so the
    # second ladder starts at the first delay again; a step that named
    # another delay would say the disarm kept the count of the first one.
    if ! netifd_state_write "$state_file" "$wan_flap_addr_back" error; then
        wan_flap_failure_detail="recover: the second refusal could not be written"
        return 1
    fi
    if ! netifd_event_send update; then
        wan_flap_failure_detail="recover: the second update event was not accepted"
        return 1
    fi
    if ! recover_fault_wait "$flow_dir" 2 2 15; then
        wan_flap_failure_detail="recover: the second refused status query armed no attempt of its own, $(recover_arm_count "$flow_dir") steps behind $(netifd_refusal_count "$flow_dir") refusals"
        return 1
    fi
    last_ms=$(recover_arm_last_ms "$flow_dir")
    if [[ $last_ms != 5000 ]]; then
        wan_flap_failure_detail="recover: the second ladder started at ${last_ms:-no} ms, not at the 5000 ms of a ladder that begins again"
        return 1
    fi
    # The object leaves the bus under the pending attempt, and an event
    # arrives behind it with an address the daemon already knows. A lookup
    # that finds nothing is no fault of the path, so neither the attempt nor
    # the event may add a step (src/proto/sip/account.c
    # acct_recover_lookup_failed). The window outlives the pending attempt, so
    # the ladder is read where it would climb.
    netifd_stub_stop
    if ! netifd_event_send update; then
        wan_flap_failure_detail="recover: the event behind the departed stub was not accepted"
        return 1
    fi
    if ! recover_arm_quiet_check "$flow_dir" 2 10; then
        wan_flap_failure_detail="recover: the ladder ran on with no object on the bus, $(recover_arm_count "$flow_dir") steps"
        return 1
    fi

    return 0
}

# The two parts under one name, each with its own daemon, ubusd, stub and
# artifact directory. They answer two questions of the same unit and neither
# can be read from the other: the first is the field sequence on the wire,
# the second is the recovery ladder in the log.
wan_flap_run()
{
    local name=wan-flap-outage

    if ! netifd_stub_check; then
        result_print FAIL "$name" "$wan_flap_module_detail"
        return
    fi
    if ! wan_flap_move_run "$name"; then
        flow_stop
        result_print FAIL "$name" "$wan_flap_failure_detail"
        return
    fi
    flow_stop
    if ! wan_flap_recover_run "$name"; then
        flow_stop
        result_print FAIL "$name" "$wan_flap_failure_detail"
        return
    fi
    flow_stop
    result_print PASS "$name" \
        "the change during the outage sent one REGISTER on the old Call-ID with $wan_flap_addr_two in the Via and the Contact and $wan_flap_addr_one removed with expires=0, the refresh behind it stood one refresh interval away and carried no removal, and two refused netifd queries each armed one attempt at 5000 ms, the first taking the address and the second ending with the object"
}

# One channel section of the MGCP config. The two lines take RTP ranges that
# do not overlap.
mgcp_channel_write()
{
    local line=$1
    local channel_extra=$2

    printf "config channel 'channel%u'\n" $((line + 1))
    printf "\toption id '%u'\n" "$line"
    printf '\toption enabled %s\n' "'1'"
    printf "\toption rtp_local_port_min '%u'\n" $((24000 + line * 10))
    printf "\toption rtp_local_port_max '%u'\n" $((24008 + line * 10))
    if [[ -n $channel_extra ]]; then
        printf '%s\n' "$channel_extra"
    fi
}

# The voip config of an MGCP flow: protocol mgcp, two lines and the mgcp
# section toward the call agent of mgcp_ca.py. The MGCP socket is IPv4 alone,
# so the literal does not follow the family. restart_delay_max_s 0 sends the
# restart RSIP at once, and the timers keep a lost datagram inside the length
# of a flow. The third argument adds lines to the mgcp section, the fourth to
# each channel section.
mgcp_config_write()
{
    local config_dir=$1
    local dialect=$2
    local mgcp_extra=${3:-}
    local channel_extra=${4:-}
    local line

    mkdir -p "$config_dir" || return 1
    {
        printf '%s\n' "config base 'base'"
        printf '\toption enabled %s\n' "'1'"
        printf '\toption channels %s\n' "'2'"
        printf '\toption protocol %s\n' "'mgcp'"
        for line in 0 1; do
            mgcp_channel_write "$line" "$channel_extra"
        done
        printf '%s\n' "config mgcp 'mgcp'"
        printf '\toption call_agent %s\n' "'127.0.0.1'"
        printf "\toption call_agent_port '%u'\n" "$ca_port"
        printf "\toption local_port '%u'\n" "$gw_port"
        printf "\toption dialect '%s'\n" "$dialect"
        printf '\toption restart_delay_max_s %s\n' "'0'"
        printf '\toption t_max_s %s\n' "'10'"
        printf "\toption t_hist_s '%u'\n" "$mgcp_t_hist_s"
        if [[ -n $mgcp_extra ]]; then
            printf '%s\n' "$mgcp_extra"
        fi
    } >"$config_dir/voip"
}

# The call agent, bound before the daemon starts so the restart RSIP of the
# daemon has an answer. Its standard output is the flow log.
mgcp_agent_start()
{
    local flow_dir=$1
    local name=$2
    local dialect=$3
    local socket=$4
    local rto_ms=$5
    local max1=$6
    local deadline=$((SECONDS + 5))

    python3 -I "$script_dir/mgcp_ca.py" --flow "$name" --flow-dir "$flow_dir" \
        --port "$ca_port" --socket "$socket" --media-port "$ca_media_port" \
        --dialect "$dialect" --rto-ms "$rto_ms" --max1 "$max1" \
        --t-hist-s "$mgcp_t_hist_s" >"$flow_dir/flow.log" 2>&1 &
    current_ca_pid=$!
    while (( SECONDS < deadline )); do
        if udp_bound_check "$ca_port"; then
            return 0
        fi
        if process_exited "$current_ca_pid"; then
            return 1
        fi
        sleep 0.05
    done
    return 1
}

# Whether the daemon carries the MGCP protocol, from "signaling.available":
# status 0 for yes, 2 for no, and 1 when the daemon gives no answer.
mgcp_image_probe()
{
    local flow_dir=$1
    local deadline=$((SECONDS + 20))
    local status=$flow_dir/readiness.json
    local word

    while (( SECONDS < deadline )); do
        ubus -s "$current_socket" call voice status >"$status" 2>/dev/null
        word=$(jq -r 'if (.signaling.available | type) != "array" then "wait"
            elif .signaling.available | index("mgcp") then "present"
            else "absent" end' "$status" 2>/dev/null)
        case $word in
            present)
                mgcp_image=yes
                return 0
                ;;
            absent)
                mgcp_image=no
                return 2
                ;;
        esac
        if ! kill -0 "$current_daemon_pid" 2>/dev/null; then
            return 1
        fi
        sleep 0.1
    done
    return 1
}

# The readiness of daemon_start. The image probe decides first, so an agent
# verdict on a daemon without MGCP never stands for a result. The agent then
# decides the readiness: it writes ready.json when both endpoints read
# in_service. A flow can leave service again at once, so a poll of its own
# here could miss that window. A verdict of the agent ends the wait too.
# Status 2 is an image without the MGCP protocol.
mgcp_ready_wait()
{
    local flow_dir=$1
    local deadline
    local probe_status=0

    mgcp_image_probe "$flow_dir" || probe_status=$?
    if (( probe_status != 0 )); then
        return "$probe_status"
    fi
    deadline=$((SECONDS + 20))
    while (( SECONDS < deadline )); do
        if [[ -s $flow_dir/ready.json || -s $flow_dir/verdict.txt ]]; then
            return 0
        fi
        if ! kill -0 "$current_daemon_pid" 2>/dev/null; then
            return 1
        fi
        sleep 0.1
    done
    return 1
}

# The verdict line the agent writes after its flow. An agent that exits with
# no verdict, or a flow past its bound, fails here.
mgcp_verdict_wait()
{
    local flow_dir=$1
    local timeout_s=$2
    local deadline=$((SECONDS + timeout_s))

    while (( SECONDS < deadline )); do
        if [[ -s $flow_dir/verdict.txt ]]; then
            return 0
        fi
        if process_exited "$current_ca_pid"; then
            return 1
        fi
        sleep 0.1
    done
    return 1
}

# The second reading of the spacing of mgcp-disconnected, from the capture:
# the first and the last copy of each RSIP RM: disconnected, and the ratio of
# the two gaps between the first three attempts. It writes disc-wire.txt. The
# statuses: 2 no capture or no tshark, 3 fewer than three attempts, 1 a ratio
# outside the bounds of mgcp_ca.py.
mgcp_disc_wire_check()
{
    local flow_dir=$1

    if [[ $capture_ready -ne 1 ]] || ! command -v tshark >/dev/null 2>&1; then
        return 2
    fi
    # decode-as names MGCP on the drawn ports, which the dissector does not
    # take by default
    tshark -r "$flow_dir/traffic.pcap" \
        -d "udp.port==$gw_port,mgcp" -d "udp.port==$ca_port,mgcp" \
        -Y 'mgcp.req.verb == "RSIP" && mgcp.param.restartmethod == "disconnected"' \
        -T fields -E separator='|' -e frame.time_epoch -e mgcp.transid \
        >"$flow_dir/disc-wire-fields.txt" 2>"$flow_dir/tshark-disc.log" || return 2
    awk -F '|' '
        !($2 in first) { first[$2] = $1; order[++n] = $2 }
        { last[$2] = $1 }
        END {
            if (n < 3) {
                printf "the capture holds %u RSIP RM: disconnected attempts\n", n
                exit 3
            }
            g1 = first[order[2]] - last[order[1]]
            g2 = first[order[3]] - last[order[2]]
            ratio = g2 / g1
            printf "capture gaps %.3f s and %.3f s, ratio %.3f\n", g1, g2, ratio
            exit !(ratio >= 1.8 && ratio <= 2.2)
        }
    ' "$flow_dir/disc-wire-fields.txt" >"$flow_dir/disc-wire.txt"
}

# One MGCP flow. The agent runs the flow of its own name against the daemon
# and writes the verdict; the harness gives it the config, the capture, the
# daemon and the teardown. An image without the MGCP protocol skips the flow.
#
#   mgcp_flow_run <name> <dialect> <timeout_s> [mgcp options] [channel options]
#                 [rto_ms] [max1]
mgcp_flow_run()
{
    local name=$1
    local dialect=$2
    local timeout_s=$3
    local mgcp_extra=${4:-}
    local channel_extra=${5:-}
    local rto_ms=${6:-200}
    local max1=${7:-5}
    local ready_status=0
    local wire_status=0
    local verdict
    local status
    local detail

    if [[ $mgcp_image == no ]]; then
        result_print SKIP "$name" "$mgcp_skip_detail"
        return
    fi
    flow_dir=$artifact_root/$name
    config_dir=$flow_dir/config
    # a verdict or a transcript of an earlier run must not answer for this one
    mkdir -p "$flow_dir" && rm -f "$flow_dir/verdict.txt" "$flow_dir/ca.json" \
        "$flow_dir/ready.json" "$flow_dir/disc-spacing.txt" \
        "$flow_dir/disc-wire.txt"
    if ! mgcp_config_write "$config_dir" "$dialect" "$mgcp_extra" \
        "$channel_extra"; then
        result_print FAIL "$name" "config write failed"
        return
    fi
    capture_start "$flow_dir" "$gw_port" || true
    if ! mgcp_agent_start "$flow_dir" "$name" "$dialect" \
        "$(ubus_socket_path "$name")" "$rto_ms" "$max1"; then
        flow_stop
        result_print FAIL "$name" "the call agent did not bind port $ca_port"
        return
    fi
    if ! daemon_spawn "$flow_dir" "$config_dir" "$name" ok 2 mgcp,hook,hal; then
        flow_stop
        result_print FAIL "$name" "startup failed"
        return
    fi
    mgcp_ready_wait "$flow_dir" || ready_status=$?
    if (( ready_status == 2 )); then
        flow_stop
        result_print SKIP "$name" "$mgcp_skip_detail"
        return
    fi
    if (( ready_status != 0 )); then
        flow_stop
        result_print FAIL "$name" "the call agent saw no endpoint in service"
        return
    fi
    if ! mgcp_verdict_wait "$flow_dir" "$timeout_s"; then
        flow_stop
        result_print FAIL "$name" "the call agent gave no verdict in $timeout_s s"
        return
    fi
    verdict=$(head -1 "$flow_dir/verdict.txt")
    status=${verdict%% *}
    detail=${verdict#* }
    if [[ $status != PASS || $name != mgcp-disconnected ]]; then
        flow_stop
        result_print "$status" "$name" "$detail"
        return
    fi
    capture_finish
    mgcp_disc_wire_check "$flow_dir" || wire_status=$?
    flow_stop
    case $wire_status in
        0)
            result_print PASS "$name" "$detail, $(cat "$flow_dir/disc-wire.txt")"
            ;;
        2)
            result_print PARTIAL "$name" "$detail, no capture to read the spacing again"
            ;;
        *)
            result_print FAIL "$name" "$(cat "$flow_dir/disc-wire.txt")"
            ;;
    esac
}

selection_run()
{
    local selection=$1

    family_detail=
    family_failure_detail=
    case $selection in
        flow-a)
            flow_a_run
            ;;
        flow-b)
            flow_b_run
            ;;
        flow-c)
            verbs_run
            ;;
        uas_reinvite_hold|uas_offerless_reinvite|uas_t38_reject|uas_session_timer_422|uas_491_glare)
            committed_outbound_run "$selection"
            ;;
        uac_cancel)
            uac_cancel_run
            ;;
        uac_reinvite_hold)
            uac_reinvite_run
            ;;
        uas_503_retry_after)
            retry_503_run
            ;;
        reg-403-reorder)
            reg_403_run
            ;;
        reg-none-reorder)
            reg_none_run
            ;;
        uas_mwi_notify)
            mwi_notify_run
            ;;
        uas_cfu_redirect)
            cfu_redirect_run
            ;;
        uas_fax_upspeed)
            fax_upspeed_run uas_fax_upspeed 0
            ;;
        uas_fax_upspeed_auth)
            fax_upspeed_run uas_fax_upspeed_auth 1
            ;;
        uas_far_fax_upspeed)
            far_fax_upspeed_run uas_far_fax_upspeed 1
            ;;
        uas_far_fax_upspeed_g729)
            far_fax_upspeed_run uas_far_fax_upspeed_g729 0
            ;;
        uas_far_fax_tone_g711)
            far_fax_tone_run
            ;;
        uas_fax_upspeed_false)
            fax_upspeed_false_run
            ;;
        uas_far_fax_tone_false)
            far_fax_tone_false_run
            ;;
        uac_t38_offer)
            t38_run
            ;;
        uac_t38_reject)
            t38_reject_run
            ;;
        srtp-outbound)
            srtp_outbound_run
            ;;
        srtp-inbound)
            srtp_inbound_run
            ;;
        srtp-accept-plain)
            srtp_accept_plain_run
            ;;
        srtp-refused)
            srtp_refused_run
            ;;
        g722-call)
            codec_inbound_run g722-call uac_g722_call 1 g722_config_write \
                8 g722 \
                "the answer took payload type 9 with G722/8000 and the stream ran as g722"
            ;;
        g722-refused)
            codec_inbound_run g722-refused uac_g722_refused 0 \
                g722_config_write 1 pcmu \
                "the default codec list skipped payload type 9, answered PCMU and ran as pcmu"
            ;;
        g726-call)
            codec_inbound_run g726-call uac_g726_call 1 g726_config_write \
                4 g726_32 \
                "the answer and stream used G726-32 at payload type 111"
            ;;
        g726-refused)
            codec_inbound_run g726-refused uac_g726_refused 0 \
                g726_config_write 1 pcmu \
                "the default codecs skipped G726-32 and used PCMU"
            ;;
        opus-call)
            opus_inbound_run opus-call uac_opus_call 1 9 opus 96 20 0 \
                "the answer took payload type 96 with opus/48000/2, our decoder fmtp and telephone-event/48000, the stream ran as opus at ptime 20 under the peer's 20000 bit/s cap at wideband, and no digit fallback was counted"
            ;;
        opus-refused)
            opus_inbound_run opus-refused uac_opus_refused 0 1 pcmu 0 20 0 \
                "the default codec list skipped payload type 96, answered PCMU with telephone-event/8000, ran as pcmu with an empty opus block and counted no fallback"
            ;;
        opus-narrow-dtmf)
            opus_inbound_run opus-narrow-dtmf uac_opus_narrow_dtmf 1 9 opus 96 20 1 \
                "the answer took Opus under the peer's 20000 bit/s cap, refused the 8000 telephone-event, and the daemon reported and counted the digit fallback once"
            ;;
        prack-require)
            prack_answer_run prack-require uac_prack_require 1 \
                "PRACK matched RSeq" prack_matched_check \
                "Require: 100rel drew a reliable 180 under the option"
            ;;
        prack-supported)
            prack_answer_run prack-supported uac_prack_supported 1 \
                "PRACK matched RSeq" prack_matched_check \
                "Supported: 100rel drew a reliable 180 under the option"
            ;;
        prack-supported-plain)
            prack_answer_run prack-supported-plain uac_prack_supported_plain 0 \
                "180 sent" prack_plain_check \
                "Supported: 100rel left the 180 plain at a clear option"
            ;;
        prack-require-plain)
            prack_answer_run prack-require-plain uac_prack_require_plain 0 \
                "180 sent" prack_plain_check \
                "Require: 100rel left the 180 plain at a clear option"
            ;;
        prack-bad-rack)
            prack_answer_run prack-bad-rack uac_prack_bad_rack 1 \
                "PRACK matched RSeq" prack_bad_rack_check \
                "a PRACK that names no outstanding provisional draws 481"
            ;;
        prack-late)
            prack_late_run
            ;;
        prack-giveup)
            prack_giveup_run
            ;;
        prack-cancel)
            prack_cancel_run
            ;;
        prack-uac-180)
            prack_dial_run prack-uac-180 uas_prack_180 prack_uac_180_check \
                "a reliable 180 draws one PRACK"
            ;;
        prack-uac-dup)
            prack_dial_run prack-uac-dup uas_prack_dup_180 prack_uac_dup_check \
                "a repeated reliable 180 draws no second PRACK"
            ;;
        prack-uac-gap)
            prack_dial_run prack-uac-gap uas_prack_gap prack_uac_gap_check \
                "an out-of-order RSeq is neither acknowledged nor processed"
            ;;
        prack-uac-auth)
            prack_dial_run prack-uac-auth uas_prack_auth prack_uac_auth_check \
                "a challenged INVITE keeps its advertisement and the RAck names the retry"
            ;;
        prack-early-media)
            prack_early_media_run prack-early-media
            ;;
        prack-early-media-off)
            prack_early_media_run prack-early-media-off 0
            ;;
        early-media-183)
            early_media_183_run early-media-183
            ;;
        verstat-cid)
            verstat_run
            ;;
        inbound-churn)
            inbound_churn_run
            ;;
        service-route-absent)
            service_route_run service-route-absent uas_service_route_none 0 \
                "the 200 granted no service route, and the REGISTER, the INVITE, the ACK and the BYE all carried no Route header"
            ;;
        service-route-present)
            service_route_run service-route-present uas_service_route 1 \
                "the two granted hops rode the challenged INVITE, its credential retry and the ACK of the 407 as one Route header each, in that order, while the REGISTER, the ACK of the 2xx and the BYE carried none"
            ;;
        wan-flap-outage)
            wan_flap_run
            ;;
        stream-uac-tcp|stream-uac-tls)
            stream_uac_run "${selection##*-}"
            ;;
        stream-uas-tcp|stream-uas-tls)
            stream_uas_run "${selection##*-}"
            ;;
        stream-register-tcp|stream-register-tls)
            stream_register_run "${selection##*-}"
            ;;
        stream-subscribe-tcp|stream-subscribe-tls)
            stream_subscribe_run "${selection##*-}"
            ;;
        xfer-blind)
            xfer_transferor_run xfer-blind uas_xfer_blind '*072' 5552000 ok \
                "the drained leg answered both NOTIFYs, the c=0.0.0.0 re-INVITE and the switch's BYE, and sent no BYE of its own" \
                0 none
            ;;
        xfer-blind-grace)
            xfer_transferor_run xfer-blind-grace uas_xfer_blind_grace '*072' 5552000 ok \
                "the drained leg answered both NOTIFYs and the re-INVITE and BYEd one grace behind the verdict" \
                1 grace
            ;;
        xfer-attended)
            xfer_transferor_run xfer-attended uas_xfer_attended '*073' 5552000 ok \
                "the sipfrag 200 earned the BYE one grace behind its answer and the record carries the transfer key" \
                1 grace
            ;;
        xfer-failed)
            xfer_transferor_run xfer-failed uas_xfer_failed '*073' 5552000 fail \
                "the sipfrag 486 resumed the held call and left its record unmarked" \
                1 none
            ;;
        star-network)
            star_network_run
            ;;
        xfer-transferee)
            xfer_transferee_run xfer-transferee uac_xfer_refer ''
            ;;
        cert-fetch)
            cert_fetch_run
            ;;
        reload-loop)
            reload_loop_run
            ;;
        xfer-two-line)
            xfer_two_line_run
            ;;
        xfer-onhook)
            xfer_onhook_run
            ;;
        xfer-onhook-blind)
            xfer_onhook_blind_run
            ;;
        xfer-replaces)
            xfer_replaces_run
            ;;
        xfer-child-fail)
            xfer_child_fail_run
            ;;
        xfer-transferee-replaces)
            xfer_transferee_run xfer-transferee-replaces uac_xfer_refer_replaces \
                'xfercall99@127.0.0.1;to-tag=peertag99;from-tag=duttag99'
            ;;
        conf-join)
            conf_join_run
            ;;
        conf-drop)
            conf_drop_run
            ;;
        conf-split)
            conf_split_run
            ;;
        conf-onhook-xfer)
            conf_onhook_xfer_run conf-onhook-xfer uac_conf_onhook_xfer 1 1 \
                "escaped Replaces, the referee BYE alone, the peer's BYE answered on the replaced leg, and two marked records passed"
            ;;
        conf-onhook-xfer-auth)
            conf_onhook_xfer_run conf-onhook-xfer-auth \
                uac_conf_onhook_xfer_auth 2 2 \
                "the challenged REFER retried with credentials and the same escaped Replaces, the challenged referee BYE retried with credentials, and two marked records passed"
            ;;
        conf-remote-bye)
            conf_remote_bye_run
            ;;
        conf-ineligible)
            conf_ineligible_run
            ;;
        legacy-hold)
            legacy_hold_run
            ;;
        glare-mutual)
            glare_mutual_run
            ;;
        conf-legacy-hold)
            conf_legacy_hold_run
            ;;
        hold-dial-abandon)
            hold_dial_abandon_run
            ;;
        hold-dial-abandon-onhook)
            hold_dial_abandon_onhook_run
            ;;
        loop-test)
            loop_test_run
            ;;
        daemon_dsp_fatal)
            daemon_dsp_fatal_run
            ;;
        daemon_dsp_transient)
            daemon_dsp_transient_run
            ;;
        daemon_dsp_fatal_late)
            daemon_dsp_fatal_late_run
            ;;
        daemon_dsp_transient_late)
            daemon_dsp_transient_late_run
            ;;
        daemon_dsp_fatal_sync)
            daemon_dsp_fatal_sync_run
            ;;
        mgcp-restart|mgcp-outbound|mgcp-inbound-cid-ring|mgcp-call-waiting|\
        mgcp-hold|mgcp-response-cache)
            mgcp_flow_run "$selection" rfc3435 60
            ;;
        mgcp-fax-tone)
            mgcp_flow_run "$selection" rfc3435 60 '' \
                "$(printf '\toption fax_t38_enable %s' "'1'")"
            ;;
        mgcp-disconnected|mgcp-piggyback)
            # An unanswered gateway command ends 100 ms behind its third
            # copy, so each measured gap holds the same 100 ms. The bound
            # covers the 150 s wait of mgcp-disconnected for its third
            # attempt and the steps before it.
            mgcp_flow_run "$selection" rfc3435 200 \
                "$(printf '\toption max1 %s\n\toption max2 %s\n\toption rto_init_ms %s\n\toption rto_max_ms %s' \
                    "'1'" "'2'" "'100'" "'100'")" '' 100 1
            ;;
        mgcp-ncs)
            mgcp_flow_run "$selection" ncs 60
            ;;
        *)
            printf 'ERROR: unknown flow or scenario: %s\n' "$selection" >&2
            return 2
            ;;
    esac
}

main()
{
    local requested=()
    local item
    local port_seed

    if [[ ${1:-} = -h || ${1:-} = --help ]]; then
        usage_print
        return 0
    fi

    if [[ $flow_family != 4 && $flow_family != 6 ]]; then
        printf 'ERROR: E2E_FAMILY must be 4 or 6, not %s\n' "$flow_family" >&2
        return 2
    fi
    family_selection_check "$@" || return 2

    artifact_root=$(path_absolute "$artifact_root") || return 2
    sipp_bin=$(path_absolute "$sipp_bin") || return 2
    if [[ -e $sipp_tls_bin ]]; then
        sipp_tls_bin=$(path_absolute "$sipp_tls_bin") || return 2
    fi
    mkdir -p "$artifact_root" || return 2
    prerequisites_check "$@" || return 2
    daemon_build "${E2E_BUILD_JOBS:-4}" || return 2

    if [[ ${UVOICE_E2E_USERNS:-0} != 1 ]]; then
        export E2E_ARTIFACTS=$artifact_root
        export SIPP_BIN=$sipp_bin
        export SIPP_TLS_BIN=$sipp_tls_bin
        export UVOICE_HOST_PREFIX=$host_prefix
        export UVOICE_E2E_USERNS=1
        # The family of the run crosses the re-exec the way every other choice
        # of this frame does, and it crosses normalized: an empty value on the
        # outside is the 4 the inside reads.
        export E2E_FAMILY=$flow_family
        # A user namespace alone holds no capability over the host network
        # namespace, so tcpdump cannot open a capture socket. A private network
        # namespace both grants that capability and isolates the ports.
        if unshare -r -n true 2>/dev/null; then
            # The private network namespace is its own pin. A flow that
            # asserts the absence of a netdev can only do so where the host's
            # netdevs are out of reach: the fallback arm below runs in the
            # host namespace, where an interface really named wan would make
            # correct code emit the device.
            export E2E_PRIVATE_NET=1
            exec unshare -r -n "$script_dir/run_e2e.sh" "$@"
        fi
        printf '%s\n' "WARNING: no private network namespace, timing checks stay partial" >&2
        exec unshare -r "$script_dir/run_e2e.sh" "$@"
    fi

    if ! loopback_prepare; then
        printf '%s\n' "$loopback_failure" >&2
        return 2
    fi

    trap exit_cleanup EXIT
    trap 'exit 130' HUP INT TERM
    port_seed=$((20000 + ($$ % 500) * 20))
    sipp_port=${E2E_SIPP_PORT:-$port_seed}
    daemon_port=${E2E_DAEMON_PORT:-$((port_seed + 1))}
    media_port=${E2E_MEDIA_PORT:-$((port_seed + 2))}
    # the certificate server of the cert-fetch and reload-loop flows, inside
    # the same namespace
    http_port=${E2E_HTTP_PORT:-$((port_seed + 3))}
    # the call agent of the MGCP flows, the gateway port of the daemon and the
    # media port the agent offers. SIPp binds media_port and media_port plus
    # 2 for video, and keeps the port above each for RTCP, so these start
    # behind that block.
    ca_port=${E2E_CA_PORT:-$((port_seed + 6))}
    gw_port=${E2E_GW_PORT:-$((port_seed + 7))}
    ca_media_port=${E2E_CA_MEDIA_PORT:-$((port_seed + 8))}

    if (( $# != 0 )); then
        requested=("$@")
    elif (( flow_family == 6 )); then
        # A run under 6 with no argument drives the documented v6 set, so the
        # default set never runs under 6 by construction.
        requested=("${family_v6_set[@]}")
    else
        requested=(flow-a flow-b flow-c uas_reinvite_hold
            uas_offerless_reinvite uas_t38_reject uas_session_timer_422
            uac_cancel uac_reinvite_hold uas_491_glare uas_503_retry_after
            uas_mwi_notify uas_cfu_redirect uas_fax_upspeed
            uas_far_fax_upspeed uas_far_fax_upspeed_g729
            uas_far_fax_tone_g711 uas_fax_upspeed_false
            uas_far_fax_tone_false uac_t38_offer
            uac_t38_reject srtp-outbound srtp-inbound srtp-accept-plain
            srtp-refused g726-call g726-refused g722-call
            g722-refused opus-call opus-refused opus-narrow-dtmf
            stream-uac-tcp stream-uac-tls stream-uas-tcp stream-uas-tls
            stream-register-tcp stream-register-tls stream-subscribe-tcp
            stream-subscribe-tls cert-fetch reload-loop xfer-blind
            xfer-blind-grace xfer-attended xfer-failed xfer-transferee
            xfer-transferee-replaces xfer-child-fail xfer-replaces
            xfer-onhook xfer-onhook-blind xfer-two-line star-network
            loop-test daemon_dsp_fatal
            daemon_dsp_transient daemon_dsp_fatal_late
            daemon_dsp_transient_late daemon_dsp_fatal_sync conf-join
            conf-drop conf-split conf-onhook-xfer
            conf-onhook-xfer-auth conf-remote-bye
            conf-ineligible legacy-hold glare-mutual conf-legacy-hold
            hold-dial-abandon hold-dial-abandon-onhook
            prack-require prack-supported
            prack-supported-plain prack-require-plain prack-bad-rack
            prack-late prack-giveup
            prack-cancel prack-uac-180 prack-uac-dup prack-uac-gap
            prack-uac-auth prack-early-media prack-early-media-off
            early-media-183
            verstat-cid inbound-churn service-route-absent
            service-route-present wan-flap-outage reg-403-reorder
            reg-none-reorder mgcp-restart mgcp-outbound
            mgcp-inbound-cid-ring mgcp-call-waiting mgcp-hold mgcp-fax-tone
            mgcp-disconnected mgcp-piggyback mgcp-response-cache mgcp-ncs)
    fi

    for item in "${requested[@]}"; do
        selection_run "$item" || return $?
    done

    printf 'Artifacts: %s\n' "$artifact_root"
    printf 'Summary: %u failed, %u partial, %u skipped\n' "$failure_count" \
        "$partial_count" "$skip_count"
    if (( failure_count != 0 )); then
        return 1
    fi
    return 0
}

main "$@"
