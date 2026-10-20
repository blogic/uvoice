#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# uvoice - native voice stack for OpenWrt
# Copyright (C) 2026 Adtran Inc.
# Author: Chad Monroe <chad.monroe@adtran.com>
#
# Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
#
#
# Make the test certificates of test_sip_tls. The PEM files it writes are
# committed, so a test run never calls openssl: this script exists to say
# exactly what the fixtures are and to remake them when they expire.
#
# Run it from the directory it lives in:  sh gen.sh
#
# It writes ca.crt, ca.key, server.crt, server.key and other-ca.crt. Every
# key is EC P-256 and every certificate is valid for 7300 days.

set -e

days=7300
dir=$(dirname "$0")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cd "$dir"

cat > "$tmp/leaf.ext" <<'EXT'
basicConstraints = CA:FALSE
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid, issuer
subjectAltName = DNS:sip.example.test
EXT

# the trust anchor the verifying client loads
openssl req -x509 -noenc -newkey ec -pkeyopt ec_paramgen_curve:P-256 \
	-keyout ca.key -out ca.crt -days "$days" \
	-subj "/O=uvoice test/CN=uvoice-test-ca" \
	-addext "basicConstraints=critical,CA:TRUE" \
	-addext "keyUsage=critical,keyCertSign,cRLSign"

# the server identity: CN uvoice-test, SAN DNS sip.example.test
openssl req -noenc -newkey ec -pkeyopt ec_paramgen_curve:P-256 \
	-keyout server.key -out "$tmp/server.csr" \
	-subj "/O=uvoice test/CN=uvoice-test"
openssl x509 -req -in "$tmp/server.csr" -CA ca.crt -CAkey ca.key \
	-set_serial 0x1001 -days "$days" -extfile "$tmp/leaf.ext" \
	-out server.crt

# a second, unrelated anchor: a client that trusts only this one must refuse
# the server above, which is the verify-failure case of the suite
openssl req -x509 -noenc -newkey ec -pkeyopt ec_paramgen_curve:P-256 \
	-keyout "$tmp/other-ca.key" -out other-ca.crt -days "$days" \
	-subj "/O=uvoice test/CN=uvoice-other-ca" \
	-addext "basicConstraints=critical,CA:TRUE" \
	-addext "keyUsage=critical,keyCertSign,cRLSign"

chmod 644 ca.crt ca.key server.crt server.key other-ca.crt

openssl verify -CAfile ca.crt server.crt
