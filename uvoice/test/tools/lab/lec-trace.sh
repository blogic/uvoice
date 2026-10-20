#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
#
# Usage: sh /tmp/lec-trace.sh <line>. It switches the media and jb trace
# domains on and writes the two per-second canceller lines of the line to
# /tmp/lec-ev/trace.log. Stop it with pkill -f '^logread -f' (an unanchored
# pattern also matches the invoking ssh shell), then switch the domains off:
# ubus call voice.trace set '{"hal": false, "media": false, "jb": false}'
n=$1
mkdir -p /tmp/lec-ev
ubus call voice.trace set '{"media": true, "jb": true}' >/dev/null
logread -f | grep --line-buffered "l$n lec" > /tmp/lec-ev/trace.log
