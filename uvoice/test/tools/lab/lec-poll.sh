#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
#
# Usage: sh /tmp/lec-poll.sh <line>. It writes a header row to
# /tmp/lec-ev/poll.log, then every 5 s one row of voice status: the canceller
# state of the line, the engine loop reference and holds, the hook gaps, and
# the calibration restarts and state of the engine delay measurement. Stop
# it with pkill -f '^sh /tmp/lec-poll.sh' (an unanchored pattern also matches
# the invoking ssh shell). It switches no trace domain.
n=$1
mkdir -p /tmp/lec-ev
echo "time sample frames hold skip erle_mdb gain_u coef_u div probes retimes ref ready taps dt ec nlp act wait erle unlike dtf quiet peak loop_open loop_ref holds hook_gaps hook_gap_max cal_restarts delay_calibrated" > /tmp/lec-ev/poll.log
while true; do
	row=$(ubus call voice status | jsonfilter \
		-e "@.lec[$n].sample" -e "@.lec[$n].state.frames" -e "@.lec[$n].state.hold_frames" \
		-e "@.lec[$n].state.skip_frames" -e "@.lec[$n].state.erle_mdb" -e "@.lec[$n].state.echo_gain_micro" \
		-e "@.lec[$n].state.coef_energy_micro" -e "@.lec[$n].state.diverge_resets" -e "@.lec[$n].state.probes" \
		-e "@.lec[$n].state.retimes" -e "@.lec[$n].state.ref_delay_samples" -e "@.lec[$n].state.ref_ready" \
		-e "@.lec[$n].state.taps" -e "@.lec[$n].state.double_talk" -e "@.lec[$n].state.ec" -e "@.lec[$n].state.nlp" \
		-e "@.lec[$n].state.nlp_active_frames" -e "@.lec[$n].state.nlp_wait_frames" -e "@.lec[$n].state.nlp_erle_frames" \
		-e "@.lec[$n].state.nlp_unlike_frames" -e "@.lec[$n].state.nlp_dt_frames" -e "@.lec[$n].state.nlp_quiet_frames" \
		-e "@.lec[$n].state.coef_peak_tap" -e "@.lec[$n].state.loop_open" -e "@.diag.engine.loop_ref" -e "@.diag.engine.holds" \
		-e "@.hal.hook_gaps" -e "@.hal.hook_gap_max_ms" \
		-e "@.diag.engine.cal_restarts" -e "@.diag.engine.delay_calibrated" \
		| tr '\n' ' ')
	echo "$(date +%T) $row" >> /tmp/lec-ev/poll.log
	sleep 5
done
