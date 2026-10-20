#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
#
# Why this exists. SIPp 3.7.3 stores the failure of a check_it, of a
# check_it_inverse and of an rtp_echo action in one member of the call,
# last_action_result. It judges that member once, when the call ends, in
# call::terminate(). The MSG_TYPE_NOP arm of call::executeMessage() assigns
# the member with no condition. A <nop> that runs after a stored failure
# therefore erases it, and the call counts as a successful one. A scenario
# with that shape reports a pass it never earned. The rule and its two
# remedies stand in test/sipp/README.md, "When a failed check is judged".
#
# Why it is conservative. It reads file order and analyses no jump, so a
# <nop> a label could route around is refused too. It also refuses a check on
# a <send> or a <pause>, which SIPp never judges. The alternative is a second
# SIPp written in awk, and the cost of the rule is one edit per scenario.
#
# Why sh and awk. test/sipp/run_sipp.sh is POSIX sh and its README names no
# python3, so a preflight in python would add a prerequisite to a bench host.
# Both runners already depend on awk.
#
# Why a stream scan and not a line scan. A check and the <nop> behind it fit
# on one line. SIPp's own reader also skips CR, LF, tab and space on both
# sides of the '=' of an attribute (xp_parser.cpp, xp_get_value). The scan
# below walks tag by tag over the whole file, so the position of a newline
# changes nothing.

set -u

usage_print()
{
    cat <<EOF
Usage: $0 [DIRECTORY ...]
       $0 -h

It reads every *.xml of each directory, or of test/sipp and test/e2e when
none is named.
It prints one line per hit and exits 1. A clean set gives one summary line
and exit 0. A directory it cannot read, a directory with no scenario and an
absent awk are errors and exit 2.
EOF
}

for argument do
    case "$argument" in
        -h|--help)
            usage_print
            exit 0
            ;;
        -*)
            printf 'sipp_scenario_lint: unknown option: %s\n' "$argument" >&2
            usage_print >&2
            exit 2
            ;;
    esac
done

if ! command -v awk >/dev/null 2>&1; then
    printf 'sipp_scenario_lint: awk is absent, the scenarios stay unread\n' >&2
    exit 2
fi

script_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)

if [ "$#" -eq 0 ]; then
    set -- "$script_dir/../sipp" "$script_dir/../e2e"
fi

# The scan of one file, in stream order. It strips the comments and the CDATA
# bodies first, because a <nop> written inside either is text and not a
# message (SIPp 3.7.3 scenario.cpp, xp_get_cdata). It keeps the newlines of
# what it strips, so the line it reports is the line of the file. It then
# walks the tags in order. Every element start that SIPp counts as a message
# moves the ordinal. The attributes of a tag are read before the next tag
# moves it again.
#
# A value is a check unless it is "false" or a number that reads as zero:
# get_bool() in scenario.cpp takes "true" and "false" without case, and falls
# back to strtol(value, NULL, 0), where any nonzero number is true.
# shellcheck disable=SC2016
# The $0 below belongs to awk and must not expand in the shell.
lint_awk='
# The apostrophe is built and not written. A literal one inside this
# single-quoted program needs the quote-out quote-in idiom, and both
# shellcheck and checkbashisms mis-parse that idiom.
BEGIN {
    apostrophe = sprintf("%c", 39)
}
function count_nl(s) {
    return gsub(/\n/, "", s)
}
function keep_nl(s) {
    gsub(/[^\n]/, "", s)
    return s
}
function strip(text,   out, at, cdata_at, comment_at) {
    out = ""
    while (length(text) > 0) {
        cdata_at = index(text, "<![CDATA[")
        comment_at = index(text, "<!--")
        if (cdata_at > 0 && (comment_at == 0 || cdata_at < comment_at)) {
            out = out substr(text, 1, cdata_at - 1)
            text = substr(text, cdata_at + 9)
            at = index(text, "]]>")
            if (at == 0)
                return out keep_nl(text)
            out = out keep_nl(substr(text, 1, at + 2))
            text = substr(text, at + 3)
            continue
        }
        if (comment_at > 0) {
            out = out substr(text, 1, comment_at - 1)
            text = substr(text, comment_at + 4)
            at = index(text, "-->")
            if (at == 0)
                return out keep_nl(text)
            out = out keep_nl(substr(text, 1, at + 2))
            text = substr(text, at + 3)
            continue
        }
        return out text
    }
    return out
}
# The end of one tag, with the quotes honoured: a > inside an attribute value
# is legal XML and ends no tag.
function tag_end(s,   i, n, c, quote) {
    n = length(s)
    quote = ""
    for (i = 2; i <= n; i++) {
        c = substr(s, i, 1)
        if (quote != "") {
            if (c == quote)
                quote = ""
            continue
        }
        if (c == "\"" || c == apostrophe) {
            quote = c
            continue
        }
        if (c == ">")
            return i
    }
    return 0
}
function attr_value(tag, name,   pattern, rest, quote, at) {
    pattern = "[ \t\r\n]" name "[ \t\r\n]*=[ \t\r\n]*[\"" apostrophe "]"
    if (!match(tag, pattern))
        return ""
    quote = substr(tag, RSTART + RLENGTH - 1, 1)
    rest = substr(tag, RSTART + RLENGTH)
    at = index(rest, quote)
    if (at == 0)
        return ""
    return substr(rest, 1, at - 1)
}
function is_true(value,   digits) {
    sub(/^[ \t\r\n]+/, "", value)
    sub(/[ \t\r\n]+$/, "", value)
    if (tolower(value) == "true")
        return 1
    if (tolower(value) == "false")
        return 0
    sub(/^[+-]/, "", value)
    if (value ~ /^0[xX]/) {
        digits = substr(value, 3)
        if (!match(digits, /^[0-9A-Fa-f]+/))
            return 0
        return (substr(digits, 1, RLENGTH) ~ /[1-9A-Fa-f]/) ? 1 : 0
    }
    if (!match(value, /^[0-9]+/))
        return 0
    return (substr(value, 1, RLENGTH) ~ /[1-9]/) ? 1 : 0
}
function is_message(name) {
    return (name == "send" || name == "recv" || name == "nop" ||
            name == "pause" || name == "timewait" ||
            name == "recvCmd" || name == "sendCmd")
}
function tag_checks(tag,   value) {
    value = attr_value(tag, "check_it")
    if (value != "" && is_true(value))
        return 1
    value = attr_value(tag, "check_it_inverse")
    if (value != "" && is_true(value))
        return 1
    return 0
}
{
    buffer = buffer $0 "\n"
}
END {
    text = strip(buffer)
    line = 1
    message = 0
    message_line = 0
    kind = ""
    judge = 0
    while (1) {
        at = index(text, "<")
        if (at == 0)
            break
        line += count_nl(substr(text, 1, at - 1))
        text = substr(text, at)
        end = tag_end(text)
        if (end == 0)
            break
        tag = substr(text, 1, end)
        if (tag ~ /^<[A-Za-z]/) {
            name = substr(tag, 2)
            sub(/[ \t\r\n\/>].*$/, "", name)
            if (is_message(name)) {
                message++
                message_line = line
                kind = name
                if (name == "nop" && judge > 0) {
                    printf "%s: nop at message %d (line %d) stands behind the %s at message %d (line %d), whose actions carry %s\n",
                        path, message, line, judge_kind, judge, judge_line, judge_what
                    hits++
                }
            }
            if (message > 0 && tag_checks(tag)) {
                judge = message
                judge_kind = kind
                judge_line = message_line
                judge_what = "a check"
            }
            # An rtp_echo action of a receive stores E_AR_RTPECHO_ERROR in
            # the same member a check writes (call.cpp, the action arm of
            # process_incoming). A <nop> behind it erases that failure too.
            # The same action inside a <nop> ends the call on the spot, so it
            # needs no protection.
            if (message > 0 && (kind == "recv" || kind == "recvCmd") &&
                attr_value(tag, "rtp_echo") != "") {
                judge = message
                judge_kind = kind
                judge_line = message_line
                judge_what = "an rtp_echo"
            }
        }
        line += count_nl(tag)
        text = substr(text, end + 1)
    }
    exit hits > 0 ? 1 : 0
}
'

hit_count=0
file_count=0

for scenario_dir do
    if [ ! -d "$scenario_dir" ]; then
        printf 'sipp_scenario_lint: not a directory: %s\n' "$scenario_dir" >&2
        exit 2
    fi
    for scenario_file in "$scenario_dir"/*.xml; do
        [ -f "$scenario_file" ] || continue
        if [ ! -r "$scenario_file" ]; then
            printf 'sipp_scenario_lint: cannot read %s\n' "$scenario_file" >&2
            exit 2
        fi
        file_count=$((file_count + 1))
        awk -v path="$scenario_file" "$lint_awk" "$scenario_file"
        awk_status=$?
        if [ "$awk_status" -eq 1 ]; then
            hit_count=$((hit_count + 1))
        elif [ "$awk_status" -ne 0 ]; then
            printf 'sipp_scenario_lint: awk failed on %s with status %d\n' \
                "$scenario_file" "$awk_status" >&2
            exit 2
        fi
    done
done

if [ "$file_count" -eq 0 ]; then
    printf 'sipp_scenario_lint: no scenario found\n' >&2
    exit 2
fi

if [ "$hit_count" -ne 0 ]; then
    printf 'sipp_scenario_lint: %d scenario(s) of %d hold a nop behind a check\n' \
        "$hit_count" "$file_count"
    exit 1
fi

printf 'sipp_scenario_lint: %d scenario(s) checked, no nop stands behind a check\n' \
    "$file_count"
exit 0
