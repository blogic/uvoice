// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Fuzz entry for the MGCP text layer.
 *
 * The input is one datagram. It is copied into an exact size allocation, so
 * a read one byte past it hits an ASan redzone instead of slack space. The
 * harness splits the datagram, parses each message, walks every typed
 * accessor and then runs every builder twice: into a small buffer,
 * which must refuse or hold a whole message, and into a large one. Each
 * message a builder accepts is parsed again, and abort() reports a builder
 * output the parser refuses.
 *
 * Build:
 *   clang -fsanitize=fuzzer,address -DUVOICE_FUZZ_LIBFUZZER -I src \
 *         test/fuzz/fuzz_mgcp_msg.c src/proto/mgcp/msg.c \
 *         src/proto/shared/msg.c src/proto/shared/hdr.c -o fuzz_mgcp_msg
 *   gcc -I src test/fuzz/fuzz_mgcp_msg.c src/proto/mgcp/msg.c \
 *       src/proto/shared/msg.c src/proto/shared/hdr.c \
 *       -o fuzz_mgcp_msg      (stub main, replays the saved input files)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto/mgcp/msg.h"

#define SCRATCH_MAX		512
#define BUILD_SMALL		96
#define BUILD_LARGE		8192
#define KV_MAX			8

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static const struct sip_str pkg_l = { "L", 1 };

static void sig_walk(struct sip_str list)
{
	struct mgcp_sig_iter it;
	struct mgcp_event ev;
	struct sip_str rest, name, value;
	char scratch[SCRATCH_MAX];

	if (!mgcp_sig_first(list, pkg_l, &it, &ev))
		return;

	do {
		rest = ev.params;
		while (mgcp_event_param_next(&rest, &name, &value))
			mgcp_unquote(value, scratch, sizeof(scratch));
	} while (mgcp_sig_next(&it, &ev));
}

static void req_walk(struct sip_str list, unsigned int depth)
{
	struct mgcp_req_iter it;
	struct mgcp_req_event re;
	struct mgcp_embedded emb;

	if (!mgcp_req_first(list, pkg_l, &it, &re))
		return;

	do {
		sig_walk(re.ev.params);
		if (!(re.act & MGCP_ACT_E) ||
		    !mgcp_embedded_parse(re.embedded, &emb))
			continue;
		if (depth)
			req_walk(emb.r, depth - 1);
		sig_walk(emb.s);
	} while (mgcp_req_next(&it, &re));
}

static void value_walk(const struct mgcp_msg *msg,
		       const struct mgcp_param_line *pl)
{
	struct mgcp_entity e;
	struct mgcp_lco lco;
	struct mgcp_quarantine q;
	struct mgcp_ack_iter ai;
	struct mgcp_reason r;
	struct mgcp_conn_params cp;
	struct sip_str rest, item;
	enum mgcp_param code;
	uint32_t lo, hi;

	switch (pl->code) {
	case MGCP_PARAM_N:
		mgcp_entity_parse(pl->value, &e);
		break;
	case MGCP_PARAM_L:
	case MGCP_PARAM_A:
		mgcp_lco_parse(pl->value, &lco);
		rest = lco.a;
		while (mgcp_codec_next(&rest, &item))
			;
		break;
	case MGCP_PARAM_M:
		mgcp_mode_parse(pl->value);
		break;
	case MGCP_PARAM_R:
		req_walk(pl->value, 1);
		break;
	case MGCP_PARAM_S:
	case MGCP_PARAM_O:
	case MGCP_PARAM_T:
	case MGCP_PARAM_ES:
		sig_walk(pl->value);
		break;
	case MGCP_PARAM_Q:
		mgcp_quarantine_parse(pl->value, &q);
		break;
	case MGCP_PARAM_K:
		mgcp_ack_init(&ai, pl->value);
		while (mgcp_ack_next(&ai, &lo, &hi))
			;
		mgcp_ack_covers(pl->value, msg->txid);
		break;
	case MGCP_PARAM_F:
		rest = pl->value;
		while (mgcp_info_next(&rest, &code, &item))
			;
		break;
	case MGCP_PARAM_RM:
		mgcp_rm_parse(pl->value);
		break;
	case MGCP_PARAM_E:
		mgcp_reason_parse(pl->value, &r);
		break;
	case MGCP_PARAM_P:
		mgcp_conn_params_parse(pl->value, &cp);
		break;
	case MGCP_PARAM_C:
	case MGCP_PARAM_X:
	case MGCP_PARAM_I:
		mgcp_hex_id_valid(pl->value);
		rest = pl->value;
		while (mgcp_list_next(&rest, &item))
			mgcp_hex_id_valid(item);
		break;
	default:
		break;
	}
}

static void accessors_walk(const struct mgcp_msg *msg)
{
	const struct mgcp_param_line *pl;
	struct mgcp_lco lco;
	struct mgcp_bearer b;
	struct mgcp_reason r;
	struct mgcp_conn_params cp;
	struct sip_str v, rest, one;
	enum mgcp_mode mode;
	enum mgcp_rm rm;
	unsigned int i;
	uint32_t u;

	for (i = 0; i < msg->param_count; i++)
		value_walk(msg, &msg->param[i]);

	for (i = MGCP_PARAM_UNKNOWN + 1; i < MGCP_PARAM_MAX; i++) {
		for (pl = mgcp_msg_param(msg, (enum mgcp_param)i); pl;
		     pl = mgcp_msg_param_next(msg, pl))
			;
	}
	mgcp_msg_param_named(msg, "X-Flower");

	mgcp_msg_version_is(msg, MGCP_VERSION_RFC3435);
	mgcp_msg_version_is(msg, MGCP_VERSION_NCS);
	mgcp_msg_call_id(msg, &v);
	mgcp_msg_request_id(msg, &v);
	mgcp_msg_conn_id(msg, &v);
	mgcp_msg_lco(msg, &lco);
	mgcp_msg_mode(msg, &mode);
	mgcp_msg_digit_map(msg, &v);
	mgcp_msg_bearer(msg, &b);
	mgcp_msg_rm(msg, &rm);
	mgcp_msg_rd(msg, &u);
	mgcp_msg_md(msg, &u);
	mgcp_msg_reason(msg, &r);
	mgcp_msg_conn_params(msg, &cp);

	rest = msg->sdp;
	while (mgcp_sdp_next(&rest, &one))
		;
}

/* a NUL terminated copy of s, or NULL when s holds a NUL */
static const char *cstr(char *dst, size_t dlen, struct sip_str s)
{
	if ((s.len && memchr(s.p, 0, s.len)) || !sip_str_copy(dst, dlen, s))
		return NULL;

	return dst;
}

static void reparse_check(const struct sip_buf *b, uint32_t txid)
{
	struct mgcp_msg back;
	char *copy;
	size_t len = sip_buf_len(b);

	copy = malloc(len ? len : 1);
	if (!copy)
		return;
	memcpy(copy, b->buf, len);

	if (mgcp_msg_parse(&back, copy, len) != MGCP_PARSE_OK ||
	    back.txid != txid)
		abort();

	free(copy);
}

/*
 * One builder into a buffer of size bytes. A refusal must leave the buffer
 * empty and writable, and an accepted message must parse again.
 */
#define BUILD_RUN(size, call, txid)					\
	do {								\
		char mem_[size];					\
		struct sip_buf b_;					\
									\
		sip_buf_init(&b_, mem_, sizeof(mem_));			\
		if (call)						\
			reparse_check(&b_, txid);			\
		else if (sip_buf_len(&b_) || !sip_buf_ok(&b_))		\
			abort();					\
	} while (0)

#define BUILD_BOTH(builder, arg, txid)					\
	do {								\
		BUILD_RUN(BUILD_SMALL, builder(&b_, arg), txid);	\
		BUILD_RUN(BUILD_LARGE, builder(&b_, arg), txid);	\
	} while (0)

static void build_walk(const struct mgcp_msg *msg)
{
	char ep[SCRATCH_MAX], ver[SCRATCH_MAX], comment[SCRATCH_MAX];
	char xid[SCRATCH_MAX], obs[SCRATCH_MAX], rmv[SCRATCH_MAX];
	char cid[SCRATCH_MAX], iid[SCRATCH_MAX];
	char vals[KV_MAX][SCRATCH_MAX], names[KV_MAX][SCRATCH_MAX];
	struct mgcp_param_kv kv[KV_MAX];
	struct mgcp_ack_list acks = { { 1, 2, 3, MGCP_TXID_MAX }, 4 };
	struct mgcp_conn_params cp;
	struct mgcp_resp r;
	struct mgcp_ntfy n;
	struct mgcp_rsip rs;
	struct mgcp_dlcx d;
	struct sip_str v;
	unsigned int i, kn = 0;

	for (i = 0; i < msg->param_count && kn < KV_MAX; i++) {
		const struct mgcp_param_line *pl = &msg->param[i];

		kv[kn].code = pl->code;
		kv[kn].name = cstr(names[kn], sizeof(names[kn]), pl->name);
		kv[kn].value = cstr(vals[kn], sizeof(vals[kn]), pl->value);
		if (!kv[kn].value || (pl->code == MGCP_PARAM_UNKNOWN &&
				      !kv[kn].name))
			continue;
		kn++;
	}

	memset(&r, 0, sizeof(r));
	r.code = msg->is_command ? 200 : msg->code;
	r.txid = msg->txid;
	r.comment = cstr(comment, sizeof(comment), msg->comment);
	r.ack_request = r.code >= 200;
	r.params = kv;
	r.param_count = kn;
	r.sdp = msg->sdp.p;
	r.sdp_len = msg->sdp.len;
	BUILD_BOTH(mgcp_resp_build, &r, r.txid);

	if (!msg->is_command)
		return;

	memset(&n, 0, sizeof(n));
	n.txid = msg->txid;
	n.endpoint = cstr(ep, sizeof(ep), msg->endpoint);
	n.version = cstr(ver, sizeof(ver), msg->version);
	n.acks = &acks;
	n.request_id = mgcp_msg_value(msg, MGCP_PARAM_X, &v) ?
		       cstr(xid, sizeof(xid), v) : "0";
	n.observed = mgcp_msg_value(msg, MGCP_PARAM_O, &v) ?
		     cstr(obs, sizeof(obs), v) : "";
	BUILD_BOTH(mgcp_ntfy_build, &n, n.txid);

	memset(&rs, 0, sizeof(rs));
	rs.txid = msg->txid;
	rs.endpoint = n.endpoint;
	rs.version = n.version;
	rs.method = mgcp_msg_value(msg, MGCP_PARAM_RM, &v) ?
		    cstr(rmv, sizeof(rmv), v) : "restart";
	rs.has_delay = mgcp_msg_rd(msg, &rs.delay);
	rs.reason = 900;
	rs.reason_text = r.comment;
	BUILD_BOTH(mgcp_rsip_build, &rs, rs.txid);

	memset(&d, 0, sizeof(d));
	d.txid = msg->txid;
	d.endpoint = n.endpoint;
	d.version = n.version;
	d.call_id = mgcp_msg_value(msg, MGCP_PARAM_C, &v) ?
		    cstr(cid, sizeof(cid), v) : NULL;
	d.conn_id = mgcp_msg_value(msg, MGCP_PARAM_I, &v) ?
		    cstr(iid, sizeof(iid), v) : NULL;
	if (mgcp_msg_conn_params(msg, &cp))
		d.stats = &cp;
	BUILD_BOTH(mgcp_dlcx_build, &d, d.txid);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct mgcp_split it;
	struct mgcp_msg msg;
	struct sip_str one;
	char *buf;

	if (!size || size > MGCP_MSG_LEN_MAX)
		return 0;

	buf = malloc(size);
	if (!buf)
		return 0;
	memcpy(buf, data, size);

	mgcp_split_init(&it, buf, size);
	while (mgcp_split_next(&it, &one)) {
		if (mgcp_msg_parse(&msg, one.p, one.len) != MGCP_PARSE_OK)
			continue;
		accessors_walk(&msg);
		build_walk(&msg);
	}

	free(buf);

	return 0;
}

#ifndef UVOICE_FUZZ_LIBFUZZER
/*
 * CI without clang still compiles and links this file. With no arguments it
 * is a no-op; with file arguments it replays the saved input files, which
 * is how a libFuzzer crash reproducer is checked without libFuzzer.
 */
int main(int argc, char **argv)
{
	static uint8_t chunk[MGCP_MSG_LEN_MAX];
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
