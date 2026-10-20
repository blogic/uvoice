#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
#
# Completed archives remain useful when the daemon no longer answers ubus.

# shellcheck source=/dev/null
source /usr/srg/scripts/techlog/techlog.src || exit 0

if ubus -t 5 list voice >/dev/null 2>&1; then
	for method in status stats cdr dump_config; do
		techlog_cmd_capture "var/log/uvoice/$method.json" \
			ubus -t 5 call voice "$method"
	done
	techlog_cmd_capture var/log/uvoice/trace.json \
		ubus -t 5 call voice.trace get
	techlog_cmd_capture var/log/uvoice/calltrace.json \
		ubus -t 5 call voice.calltrace status
fi

found=0
for archive in /tmp/uvoice/calltrace/*.tar.gz; do
	[ -f "$archive" ] || continue
	found=1
	printf 'completed call trace: %s\n' "$archive"
	# The library can exit when no bundle directory exists.
	(my_cp_file "$archive")
done
if [ "$found" = 0 ]; then
	printf 'no completed call trace available\n'
	techlog_omit 'no completed call trace available'
fi
exit 0
