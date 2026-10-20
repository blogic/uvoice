// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Fuzz entry for the SIP message and header layer.
 *
 * The parser writes into the buffer it is given (fold removal), so the input
 * is copied into an exact size allocation: a read or a write one byte past the
 * datagram then lands in an ASan redzone instead of in slack space.
 *
 * After the parse the harness walks every typed accessor. A parser that
 * returns OK is only half the contract; the header layer must survive the
 * same garbage. The builders run too, so response construction from a fuzzed
 * request is covered.
 *
 * Build:
 *   clang -fsanitize=fuzzer,address -DUVOICE_FUZZ_LIBFUZZER -I src \
 *         test/fuzz/fuzz_sip_msg.c src/proto/shared/msg.c \
 *         src/proto/shared/hdr.c -o fuzz_sip_msg
 *   gcc -I src test/fuzz/fuzz_sip_msg.c src/proto/shared/msg.c \
 *       src/proto/shared/hdr.c \
 *       -o fuzz_sip_msg            (stub main, replays the saved input files)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto/shared/hdr.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void uri_walk(const struct sip_uri *uri)
{
	char scratch[512];
	struct sip_buf b;

	sip_buf_init(&b, scratch, sizeof(scratch));
	uri_serialize(uri, &b);
	sip_uri_unescape(uri->user, scratch, sizeof(scratch));
}

static void addr_walk(const struct sip_msg *msg, enum sip_hdr_id id)
{
	struct sip_val_iter it;
	struct sip_addr a;
	char scratch[512];

	if (!hdr_addr_first(msg, id, &it, &a))
		return;

	do {
		hdr_addr_display_get(&a, scratch, sizeof(scratch));
		uri_walk(&a.uri);
	} while (hdr_addr_next(&it, &a));
}

static void alert_walk(const struct sip_msg *msg, const char *prefix)
{
	struct sip_val_iter it;
	struct sip_alert_info ai;
	char scratch[512];

	if (!hdr_alert_info_first(msg, prefix, &it, &ai))
		return;

	do {
		sip_str_copy(scratch, sizeof(scratch), ai.url);
		sip_str_copy(scratch, sizeof(scratch), ai.pattern);
		sip_str_copy(scratch, sizeof(scratch), ai.params);
	} while (hdr_alert_info_next(&it, prefix, &ai));
}

static void chal_walk(const struct sip_msg *msg, enum sip_hdr_id id)
{
	struct sip_chal_iter it;
	struct sip_challenge c;
	char scratch[512];

	if (!hdr_chal_first(msg, id, &it, &c))
		return;

	do {
		sip_unquote(c.realm, scratch, sizeof(scratch));
		sip_unquote(c.nonce, scratch, sizeof(scratch));
		sip_unquote(c.opaque, scratch, sizeof(scratch));
	} while (hdr_chal_next(&it, &c));
}

static void body_walk(const struct sip_msg *msg)
{
	struct sip_part_iter it;
	struct sip_part part;
	struct sip_str body;

	msg_body_get(msg, &body);
	msg_body_sdp_get(msg, &body);

	if (!msg_part_first(msg, &it, &part))
		return;

	do {
		sip_media_type(part.ctype);
	} while (part_next(&it, &part));
}

/*
 * The block is copied out of the parse buffer and written into the next
 * request, so its invariants are what keep a fuzzed hop from reaching the
 * wire. abort() reports a breach as a crash, which is what both build modes
 * of this file record.
 */
static void route_hdrs_check(const struct sip_route_hdrs *r, bool took)
{
	size_t lines = 0, i;

	if (r->len >= SIP_ROUTE_HDRS_MAX || r->text[r->len])
		abort();
	if (r->len != strlen(r->text))
		abort();
	if (!took && (r->len || r->hops))
		abort();
	if ((r->hops == 0) != (r->len == 0))
		abort();
	if (r->hops > SIP_ROUTE_MAX)
		abort();

	for (i = 0; i < r->len; i++) {
		if (r->text[i] == '\r' &&
		    (i + 1 >= r->len || r->text[i + 1] != '\n'))
			abort();
		if (r->text[i] != '\n')
			continue;
		if (!i || r->text[i - 1] != '\r')
			abort();
		lines++;
	}

	if (lines != r->hops)
		abort();
}

static void route_walk(const struct sip_msg *msg, enum sip_hdr_id id)
{
	struct sip_route_hdrs hdrs;
	struct sip_route_set set;
	char scratch[1024];
	struct sip_buf b;
	unsigned int i;
	bool took;

	route_set_collect(msg, id, &set);
	route_set_reverse(&set);
	for (i = 0; i < set.count; i++)
		route_hop_lr(set.hop[i]);

	took = route_hdrs_take(&hdrs, msg, id);
	route_hdrs_check(&hdrs, took);

	sip_buf_init(&b, scratch, sizeof(scratch));
	if (!route_hdrs_write(&b, &hdrs) || sip_buf_len(&b) != hdrs.len)
		abort();
}

static void build_walk(const struct sip_msg *msg)
{
	char scratch[8192];
	struct sip_buf b;

	if (!msg->is_request)
		return;

	/* 100 exercises the untagged To path, 480 the tag insertion path. */
	sip_buf_init(&b, scratch, sizeof(scratch));
	msg_response_build(&b, msg, 100, NULL, NULL, 0);

	sip_buf_init(&b, scratch, sizeof(scratch));
	msg_response_build(&b, msg, 480, NULL, "uvoice-fuzz-tag", SIP_RESP_F_RECORD_ROUTE);
	msg_body_write(&b, NULL, NULL, 0);
}

static void accessors_walk(const struct sip_msg *msg)
{
	struct sip_via via;
	struct sip_event ev;
	struct sip_sub_state st;
	struct sip_alert_info ai;
	struct sip_cid cid;
	struct sip_str v, params;
	enum sip_refresher refresher;
	enum sip_method method;
	uint32_t a, c;

	hdr_via_top(msg, &via);
	hdr_via_count(msg);

	addr_walk(msg, SIP_HDR_FROM);
	addr_walk(msg, SIP_HDR_TO);
	addr_walk(msg, SIP_HDR_CONTACT);
	addr_walk(msg, SIP_HDR_P_ASSERTED_IDENTITY);
	addr_walk(msg, SIP_HDR_REMOTE_PARTY_ID);
	addr_walk(msg, SIP_HDR_REFER_TO);

	hdr_call_id_get(msg, &v);
	hdr_cseq_get(msg, &a, &method);
	hdr_uint_get(msg, SIP_HDR_MAX_FORWARDS, &a);
	hdr_uint_get(msg, SIP_HDR_EXPIRES, &a);
	hdr_uint_get(msg, SIP_HDR_MIN_EXPIRES, &a);
	hdr_rseq_get(msg, &a);
	hdr_rack_get(msg, &a, &c, &method);
	hdr_retry_after_get(msg, &a, &c);
	hdr_session_expires_get(msg, &a, &refresher);
	hdr_min_se_get(msg, &a);
	hdr_content_type_get(msg, &v, &params);

	hdr_token_has(msg, SIP_HDR_SUPPORTED, "100rel");
	hdr_token_has(msg, SIP_HDR_REQUIRE, "timer");
	hdr_token_has(msg, SIP_HDR_UNSUPPORTED, "100rel");
	hdr_token_has(msg, SIP_HDR_ALLOW, "INVITE");
	hdr_privacy_has(msg, "id");

	chal_walk(msg, SIP_HDR_WWW_AUTHENTICATE);
	chal_walk(msg, SIP_HDR_PROXY_AUTHENTICATE);

	hdr_event_get(msg, &ev);
	hdr_sub_state_get(msg, &st);
	hdr_alert_info_get(msg, NULL, &ai);
	hdr_alert_info_get(msg, "da", &ai);
	alert_walk(msg, NULL);
	alert_walk(msg, "da");
	hdr_cid_get(msg, &cid);

	route_walk(msg, SIP_HDR_ROUTE);
	route_walk(msg, SIP_HDR_RECORD_ROUTE);
	route_walk(msg, SIP_HDR_SERVICE_ROUTE);

	body_walk(msg);
	build_walk(msg);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct sip_msg msg;
	char *buf;

	if (!size || size > SIP_MSG_LEN_MAX)
		return 0;

	buf = malloc(size);
	if (!buf)
		return 0;
	memcpy(buf, data, size);

	if (msg_parse(&msg, buf, size) == SIP_PARSE_OK)
		accessors_walk(&msg);

	free(buf);

	return 0;
}

#ifndef UVOICE_FUZZ_LIBFUZZER
/*
 * CI without clang still compiles and links this file. With no arguments it is
 * a no-op; with file arguments it replays the saved input files. That is how
 * a libFuzzer crash reproducer is checked on a machine that has no libFuzzer.
 */
int main(int argc, char **argv)
{
	uint8_t chunk[SIP_MSG_LEN_MAX];
	FILE *f;
	size_t n;
	int i;

	for (i = 1; i < argc; i++) {
		f = fopen(argv[i], "rb");
		if (!f) {
			fprintf(stderr, "cannot open %s\n", argv[i]);
			return 1;
		}
		n = fread(chunk, 1, sizeof(chunk), f);
		fclose(f);
		LLVMFuzzerTestOneInput(chunk, n);
		printf("replayed %s (%zu bytes)\n", argv[i], n);
	}

	return 0;
}
#endif
