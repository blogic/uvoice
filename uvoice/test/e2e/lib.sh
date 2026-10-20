# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
#
# The helpers that run_e2e.sh and run_churn.sh share. Both runners source
# this file. The functions read uvoice_dir, host_prefix and artifact_root of
# the runner when they run. The file runs nothing on its own and needs no
# execute bit.
#
# shellcheck shell=bash disable=SC2154

# ::1 as /proc/net/udp6 and /proc/net/tcp6 print it: sixteen address bytes as
# four 32-bit words in host order, which is little-endian on every host this
# harness runs on. Measured here, not assumed.
loop6_proc_addr=00000000000000000000000001000000

path_absolute()
{
    local path=$1
    local parent

    if [[ $path = /* ]]; then
        printf '%s\n' "$path"
        return
    fi

    parent=$(CDPATH='' cd "$(dirname "$path")" && pwd) || return 1
    printf '%s/%s\n' "$parent" "$(basename "$path")"
}

daemon_build()
{
    local jobs=$1

    if [[ -n ${UVOICED:-} ]]; then
        UVOICED=$(path_absolute "$UVOICED") || return 1
        if [[ ! -x $UVOICED ]]; then
            printf 'ERROR: UVOICED is not executable: %s\n' "$UVOICED" >&2
            return 1
        fi
        export UVOICED
        return
    fi

    build_dir=$artifact_root/build
    mkdir -p "$build_dir" || return 1
    # Both protocols, so the MGCP flows of run_e2e.sh run beside the SIP ones.
    if ! cmake -S "$uvoice_dir/src" -B "$build_dir" -DUVOICE_HAL=stub \
        -DUVOICE_PROTO_SIP=ON -DUVOICE_PROTO_MGCP=ON \
        -DCMAKE_PREFIX_PATH="$host_prefix" >"$artifact_root/cmake.log" 2>&1; then
        printf '%s\n' "ERROR: cmake configure failed, see $artifact_root/cmake.log" >&2
        return 1
    fi
    if ! cmake --build "$build_dir" -j"$jobs" \
        >"$artifact_root/build.log" 2>&1; then
        printf '%s\n' "ERROR: daemon build failed, see $artifact_root/build.log" >&2
        return 1
    fi

    UVOICED=$build_dir/uvoiced
    export UVOICED
}

process_exited()
{
    local pid=$1
    local state

    if ! kill -0 "$pid" 2>/dev/null; then
        return 0
    fi
    state=$(ps -o stat= -p "$pid" 2>/dev/null || true)
    [[ $state = Z* ]]
}

udp_bound_check()
{
    local port=$1
    local hex

    hex=$(printf '%04X' "$port")
    awk -v want=":$hex\$" 'FNR > 1 && $2 ~ want { found = 1 } END { exit !found }' \
        /proc/net/udp 2>/dev/null
}

# The v6 table, read the way the v4 one is. A socket of one family never
# appears in the other family's table, so a v6 run that fell back to v4 waits
# out this probe and reports a startup failure instead of passing on a socket
# in the wrong family. The address is part of the match here, unlike in the v4
# reader: SIPp binds the address -i names, so the row proves the peer came up
# on ::1 and not merely on the port.
udp6_bound_check()
{
    local port=$1
    local hex

    hex=$(printf '%04X' "$port")
    awk -v want="^$loop6_proc_addr:$hex\$" \
        'FNR > 1 && $2 ~ want { found = 1 } END { exit !found }' \
        /proc/net/udp6 2>/dev/null
}

# SIPp only flushes the screen file when it exits by itself, so the statistics
# screen printed on stdout is the fallback source.
sipp_success_count()
{
    local log_dir=$1
    local file

    for file in "$log_dir/sipp-screen.log" "$log_dir/sipp-stdout.log"; do
        [[ -s $file ]] || continue
        awk -F '|' '
            /Successful call/ {
                value = $3
                gsub(/[^0-9]/, "", value)
                if (value != "")
                    last = value
            }
            END {
                if (last == "")
                    exit 1
                print last + 0
            }
        ' "$file" && return 0
    done
    return 1
}
