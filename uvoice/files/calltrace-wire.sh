#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
#
# The capture replaces this process so the controller owns tcpdump's lifetime.

[ "$#" -eq 4 ] || exit 2
[ -n "$1" ] && [ -n "$2" ] && [ -n "$3" ] && [ -n "$4" ] || exit 2

dir=$1
cpumask=$2
iface=$3
filter=$4
umask 077
exec 2>> "$dir/wire.stats"
version=$(/usr/bin/tcpdump --version 2>&1 | head -n 1)
set -- /usr/bin/tcpdump -i "$iface" -s 0 -U -n -w "$dir/wire.pcap" "$filter"
if [ "$cpumask" != all ]; then
	set -- /usr/bin/taskset -c "$cpumask" "$@"
fi
printf 'command: %s; version: %s\n' "$*" "$version" >&2
exec "$@"
