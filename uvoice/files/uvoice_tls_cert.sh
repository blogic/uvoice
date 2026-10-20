#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.

# Fetch argv contract: url, username, password, verify ("0" or "1").
# Clear argv contract: clear.
# Exit 0 installs, 1 keeps and retries, 2 is unchanged, 3 clears, and 4 refuses config.
# Both curl calls use a 10 second connect timeout and a 25 second total timeout.

TLS_DIR=${UVOICE_TLS_DIR:-/tmp/voip_tls}
TLS_TGZ=$TLS_DIR/voip_cert.tgz
TLS_CERT=$TLS_DIR/cert.pem
TLS_KEY=$TLS_DIR/key.pem
TLS_CA=$TLS_DIR/ca.crt
TLS_HEADER=$TLS_DIR/header.txt
TLS_PREV_HEADER=$TLS_DIR/prev_header.txt
TLS_REASON=$TLS_DIR/last_error_reason.txt
TLS_STATUS=${UVOICE_TLS_STATUS:-/tmp/voip_tls_stat.txt}
TLS_KEYLOG=/tmp/uvoice/ssl.keylog
TLS_EVENT_TOPIC=voip/nodeMAC/TLS-cert/error
TLS_MAX_FSIZE=5000
TLS_CONNECT_TIMEOUT=10
TLS_TOTAL_TIMEOUT=25

tls_url=${1-}
tls_username=${2-}
tls_password=${3-}
tls_verify=${4-0}
event_post=${UVOICE_EVENT_POST:-/usr/srg/scripts/event-post.sh}

status_write()
{
	printf '%s' "$1" > "$TLS_STATUS"
}

event_error_post()
{
	if [ -f "$TLS_REASON" ] && [ "$(cat "$TLS_REASON")" = "$1" ]; then
		return
	fi

	event_reason=$(printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g')

	if [ -f "$event_post" ] && [ -x "$event_post" ]; then
		"$event_post" -p crit -t "$TLS_EVENT_TOPIC" \
			"{\"Reason\":\"$event_reason\"}" >/dev/null 2>&1 || :
		mkdir -p "$TLS_DIR"
		printf '%s' "$1" > "$TLS_REASON"
	fi
}

status_fail()
{
	status_write "$1"
	event_error_post "$1"
}

bundle_clear()
{
	reason=
	reason_present=0
	if [ -f "$TLS_REASON" ]; then
		reason=$(cat "$TLS_REASON")
		reason_present=1
	fi

	rm -rf "$TLS_DIR"
	mkdir -p "$TLS_DIR"
	if [ "$reason_present" -eq 1 ]; then
		printf '%s' "$reason" > "$TLS_REASON"
	fi
}

bundle_clear_all()
{
	rm -rf "$TLS_DIR"
	mkdir -p "$TLS_DIR"
}

server_verify()
{
	if [ "$tls_verify" != "1" ] || [ -f "$TLS_CA" ]; then
		return 0
	fi

	bundle_clear
	status_fail "File ca.crt not found"
	return 3
}

curl_head_get()
{
	if [ -n "$tls_username" ]; then
		curl -g -sS --connect-timeout "$TLS_CONNECT_TIMEOUT" \
			-m "$TLS_TOTAL_TIMEOUT" -u "$tls_username:$tls_password" \
			-D "$TLS_HEADER" \
			-o /dev/null "$tls_url" >/dev/null 2>&1
	else
		curl -g -sS --connect-timeout "$TLS_CONNECT_TIMEOUT" \
			-m "$TLS_TOTAL_TIMEOUT" -D "$TLS_HEADER" -o /dev/null \
			"$tls_url" >/dev/null 2>&1
	fi
}

curl_archive_get()
{
	if [ -n "$tls_username" ]; then
		curl -g -sS --connect-timeout "$TLS_CONNECT_TIMEOUT" \
			-m "$TLS_TOTAL_TIMEOUT" --max-filesize "$TLS_MAX_FSIZE" \
			-u "$tls_username:$tls_password" -o "$TLS_TGZ" \
			--remote-time "$tls_url" >/dev/null 2>&1
	else
		curl -g -sS --connect-timeout "$TLS_CONNECT_TIMEOUT" \
			-m "$TLS_TOTAL_TIMEOUT" --max-filesize "$TLS_MAX_FSIZE" \
			-o "$TLS_TGZ" --remote-time "$tls_url" >/dev/null 2>&1
	fi
}

if [ "$#" -eq 1 ] && [ "$1" = "clear" ]; then
	bundle_clear_all
	status_write "TLS disabled"
	rm -f "$TLS_KEYLOG" "$TLS_KEYLOG.old"
	exit 0
fi

status_write "N/A"

if [ -z "$tls_url" ]; then
	status_fail "Unspecified certificate server URL"
	exit 4
fi

path_words=$(printf '%s\n' "$tls_url" | wc -w)
if [ "$path_words" -ne 1 ]; then
	status_fail "Unexpected space in certificate server URL"
	exit 4
fi

protocol=$(printf '%s' "${tls_url%%:*}" | tr '[:upper:]' '[:lower:]')
case "$protocol" in
	ftp)
		protocol=ftp
		;;
	http|https)
		protocol=http
		;;
	*)
		status_fail "Unsupported file transfer protocol"
		exit 4
		;;
esac

mkdir -p "$TLS_DIR"
: > "$TLS_HEADER"
curl_head_get || :

if [ "$protocol" = "http" ]; then
	curl_status=$(awk 'BEGIN { RS="\r\n" } toupper($1) ~ /^HTTP\// { value=$2 } END { print value }' "$TLS_HEADER")
	if [ "$curl_status" != "200" ]; then
		status_fail "Bad certificate server URL ($curl_status)"
		exit 1
	fi

	curl_ftype=$(awk 'BEGIN { RS="\r\n" } tolower($1) == "content-type:" { print $2; exit }' "$TLS_HEADER")
	if [ "$curl_ftype" != "application/x-gzip" ]; then
		status_fail "Wrong file type $curl_ftype"
		exit 1
	fi

	curl_fsize=$(awk 'BEGIN { RS="\r\n" } tolower($1) == "content-length:" { print $2; exit }' "$TLS_HEADER")
else
	curl_fsize=$(awk 'BEGIN { RS="\r\n" } $1 == 213 { print $2; exit }' "$TLS_HEADER")
	if [ -z "$curl_fsize" ]; then
		status_fail "File not found (FTP 213)"
		exit 1
	fi

	case "$curl_fsize" in
		*[!0-9]*)
			status_fail "File size not a number (FTP)"
			exit 1
			;;
	esac
fi

case "$curl_fsize" in
	''|*[!0-9]*)
		;;
	*)
		if [ "$curl_fsize" -gt "$TLS_MAX_FSIZE" ]; then
			status_fail "File size ($curl_fsize) exceeded $TLS_MAX_FSIZE"
			exit 1
		fi
		;;
esac

if [ "$protocol" = "http" ] && [ -f "$TLS_PREV_HEADER" ] && \
	[ -f "$TLS_CERT" ] && [ -f "$TLS_KEY" ]; then
	curl_etag=$(grep -i '^ETag:' "$TLS_HEADER" || :)
	curl_prev_etag=$(grep -i '^ETag:' "$TLS_PREV_HEADER" || :)
	if [ "$curl_prev_etag" = "$curl_etag" ]; then
		status_write "Certificate applied (etag unchanged)"
		server_verify
		verify_status=$?
		[ "$verify_status" -eq 0 ] && exit 2
		exit "$verify_status"
	fi
fi

cp "$TLS_HEADER" "$TLS_PREV_HEADER"

prev_md5=
if [ "$protocol" = "ftp" ] && [ -f "$TLS_TGZ" ]; then
	prev_md5=$(md5sum "$TLS_TGZ" 2>/dev/null || :)
fi

curl_archive_get || :

if [ "$protocol" = "ftp" ] && [ -n "$prev_md5" ] && \
	[ -f "$TLS_CERT" ] && [ -f "$TLS_KEY" ] && \
	[ "$prev_md5" = "$(md5sum "$TLS_TGZ" 2>/dev/null || :)" ]; then
	status_write "Certificate applied (md5sum unchanged)"
	server_verify
	verify_status=$?
	[ "$verify_status" -eq 0 ] && exit 2
	exit "$verify_status"
fi

rm -f "$TLS_CERT" "$TLS_KEY" "$TLS_CA"
tar xzf "$TLS_TGZ" -C "$TLS_DIR" >/dev/null 2>&1 || :

if [ ! -f "$TLS_CERT" ] || [ ! -f "$TLS_KEY" ]; then
	bundle_clear
	status_fail "Files cert.pem and key.pem not found"
	bundle_status=3
else
	chmod 600 "$TLS_KEY"
	status_write "Certificate updated and applied"
	bundle_status=0
fi

server_verify
verify_status=$?
[ "$verify_status" -eq 0 ] && exit "$bundle_status"
exit "$verify_status"
