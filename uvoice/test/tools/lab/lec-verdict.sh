#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
#
# Usage: sh /tmp/lec-verdict.sh. It switches the hal trace domain on, which
# also passes SLIC plug-in debug lines, and writes the lec verdict and lec
# second lines to /tmp/lec-ev/verdict.log. Stop it with pkill -f '^logread -f'
# (an unanchored pattern also matches the invoking ssh shell), then run:
# ubus call voice.trace set '{"hal": false, "media": false, "jb": false}'
mkdir -p /tmp/lec-ev
ubus call voice.trace set '{"hal": true}' >/dev/null
logread -f | grep --line-buffered "lec verdict\|lec second" > /tmp/lec-ev/verdict.log
