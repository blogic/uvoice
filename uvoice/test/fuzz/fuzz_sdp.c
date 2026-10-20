// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Fuzz entry for the SDP offer/answer layer.
 *
 * The parser only reads the body, but the input is still copied into an exact
 * size allocation: a read one byte past the body then lands in an ASan redzone
 * instead of in slack space.
 *
 * A successful parse is only half the contract, so the harness runs both
 * consumers over the fuzzed description: sdp_answer_build() against a fixed
 * local view, and sdp_answer_take() against a state that carries a pending
 * offer. Both write only into caller owned memory, so a fixed stack buffer
 * covers the builder too.
 *
 * The T.38 paths need their own walk, because each of the three is selected
 * by a different piece of state, and none of the three is state a local view
 * carries on its own. t38_walk() supplies all three: sdp_offer_build() takes
 * the image shape on st->local_t38, which sdp_t38_set() sets;
 * sdp_answer_build() runs its image branch on loc->t38 plus a committed
 * exchange; and sdp_answer_take() takes the image path on st->proposed.t38,
 * which only a T.38 offer of ours leaves behind.
 *
 * Build:
 *   clang -fsanitize=fuzzer,address,undefined -DUVOICE_FUZZ_LIBFUZZER -I src \
 *         test/fuzz/fuzz_sdp.c src/proto/shared/sdp.c \
 *         src/proto/shared/msg.c src/proto/shared/hdr.c -o fuzz_sdp
 *   gcc -I src test/fuzz/fuzz_sdp.c src/proto/shared/sdp.c \
 *       src/proto/shared/msg.c src/proto/shared/hdr.c \
 *       -o fuzz_sdp                (stub main, replays the saved input files)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto/shared/sdp.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void local_init(struct sdp_local *loc)
{
	memset(loc, 0, sizeof(*loc));
	loc->codec[0].id = SDP_CODEC_PCMU;
	loc->codec[0].pt = 0;
	loc->codec[1].id = SDP_CODEC_G729;
	loc->codec[1].pt = 18;
	loc->codecs = 2;
	snprintf(loc->addr, sizeof(loc->addr), "%s", "192.0.2.10");
	loc->port = 4000;
	loc->ptime_ms = 20;
	loc->dtmf_pt = SDP_DTMF_PT_DEFAULT;
	loc->cn = true;
}

/*
 * The second shape drops every optional stream element: one codec, no DTMF and
 * no comfort noise, which is the path where a fuzzed rtpmap has nothing of ours
 * to match. Its media address is an IPv6 one, so a fuzzed IP4 description meets
 * the RFC 6157 section 5 family refusal here.
 */
static void local_min_init(struct sdp_local *loc)
{
	memset(loc, 0, sizeof(*loc));
	loc->codec[0].id = SDP_CODEC_G729;
	loc->codec[0].pt = 18;
	loc->codecs = 1;
	snprintf(loc->addr, sizeof(loc->addr), "%s", "2001:db8::10");
	loc->port = 4002;
	loc->ptime_ms = 30;
}

/*
 * The same minimal shape with an IPv4 media address. The two carry the family
 * refusal both ways: a fuzzed IP6 description is refused against this one and
 * answered against the shape above, and an IP4 one the other way round.
 */
static void local_min4_init(struct sdp_local *loc)
{
	local_min_init(loc);
	snprintf(loc->addr, sizeof(loc->addr), "%s", "192.0.2.11");
}

/*
 * The fourth shape enables T.38 on the leg. This flag is what the answer gate
 * reads, and it alone: with it false, sdp_answer_build() never runs its image
 * branch, whatever the fuzzed description says. The offer builder and the
 * answer take are selected by state instead, st->local_t38 and
 * st->proposed.t38, so the walk below has to drive those two separately.
 */
static void local_t38_init(struct sdp_local *loc)
{
	memset(loc, 0, sizeof(*loc));
	loc->codec[0].id = SDP_CODEC_PCMU;
	loc->codec[0].pt = 0;
	loc->codecs = 1;
	snprintf(loc->addr, sizeof(loc->addr), "%s", "192.0.2.10");
	loc->port = 4000;
	loc->ptime_ms = 20;
	loc->dtmf_pt = SDP_DTMF_PT_DEFAULT;
	loc->t38 = true;
}

/*
 * The fifth shape carries SDES SRTP. loc->srtp is what the answer build reads:
 * its a=crypto scan runs only on an SAVP offer with this flag set. The offer
 * build and the answer take do not read the flag alone: both take the
 * transport from the committed exchange when the leg has one, and fall back to
 * this flag only on an initial offer, so local_walk() below drives a plain and
 * a keyed committed state as well as the initial one. The keys are fixed, the
 * way call.c hands this layer keys it generated once.
 */
static void local_srtp_init(struct sdp_local *loc)
{
	unsigned int i;

	memset(loc, 0, sizeof(*loc));
	loc->codec[0].id = SDP_CODEC_PCMU;
	loc->codec[0].pt = 0;
	loc->codec[1].id = SDP_CODEC_PCMA;
	loc->codec[1].pt = 8;
	loc->codecs = 2;
	snprintf(loc->addr, sizeof(loc->addr), "%s", "192.0.2.10");
	loc->port = 4004;
	loc->ptime_ms = 20;
	loc->dtmf_pt = SDP_DTMF_PT_DEFAULT;
	loc->srtp = true;
	for (i = 0; i < SDP_SRTP_KEY_LEN; i++) {
		loc->srtp_key[0][i] = (uint8_t)(1 + i);
		loc->srtp_key[1][i] = (uint8_t)(101 + i);
	}
}

/*
 * The sixth shape carries G.722, a wideband codec, and it is the one
 * shape whose local view collides with itself: PCMA repeats the payload type
 * G.722 already claimed and G.729 sits on the telephone-event type. The offer
 * build drops both, so this shape is what walks the drop arms, while the
 * answer side still has four codecs the fuzzed description may name. The two
 * entries are otherwise ordinary, so the G.722 rtpmap and its comfort noise
 * pairing ride the same walk.
 */
static void local_g722_init(struct sdp_local *loc)
{
	memset(loc, 0, sizeof(*loc));
	loc->codec[0].id = SDP_CODEC_G722;
	loc->codec[0].pt = 9;
	loc->codec[1].id = SDP_CODEC_PCMU;
	loc->codec[1].pt = 0;
	loc->codec[2].id = SDP_CODEC_PCMA;
	loc->codec[2].pt = 9;
	loc->codec[3].id = SDP_CODEC_G729;
	loc->codec[3].pt = SDP_DTMF_PT_DEFAULT;
	loc->codecs = 4;
	snprintf(loc->addr, sizeof(loc->addr), "%s", "192.0.2.10");
	loc->port = 4006;
	loc->ptime_ms = 20;
	loc->dtmf_pt = SDP_DTMF_PT_DEFAULT;
	loc->cn = true;
}

/*
 * The seventh shape carries the only dynamic payload type on the 8000 clock
 * beside comfort noise.
 */
static void local_g726_init(struct sdp_local *loc)
{
	memset(loc, 0, sizeof(*loc));
	loc->codec[0].id = SDP_CODEC_G726_32;
	loc->codec[0].pt = 111;
	loc->codec[1].id = SDP_CODEC_PCMU;
	loc->codec[1].pt = 0;
	loc->codecs = 2;
	snprintf(loc->addr, sizeof(loc->addr), "%s", "192.0.2.10");
	loc->port = 4008;
	loc->ptime_ms = 20;
	loc->dtmf_pt = SDP_DTMF_PT_DEFAULT;
	loc->cn = true;
}

/*
 * The eighth shape carries Opus, a wideband codec, and it is the one
 * shape whose stream clocks at 48000: it walks the codec clock table, the two
 * fmtp records (ours is written for every Opus rtpmap, theirs is parsed off the
 * fuzzed description), the telephone-event clock that follows the selected
 * codec, and the comfort noise Opus never pairs with. PCMU sits behind it so
 * the answer side can still fall back to a narrowband stream and the 8000
 * clock stays reachable from the same walk.
 *
 * cn is set for that reason: the offer withdraws the CN line while Opus leads
 * the list and writes it again as soon as the fuzzed answer moves the exchange
 * onto PCMU.
 */
static void local_opus_init(struct sdp_local *loc)
{
	memset(loc, 0, sizeof(*loc));
	loc->codec[0].id = SDP_CODEC_OPUS;
	loc->codec[0].pt = 96;
	loc->codec[0].opus.bitrate = 32000;
	loc->codec[0].opus.complexity = 5;
	loc->codec[0].opus.fec = true;
	loc->codec[1].id = SDP_CODEC_PCMU;
	loc->codec[1].pt = 0;
	loc->codecs = 2;
	snprintf(loc->addr, sizeof(loc->addr), "%s", "192.0.2.10");
	loc->port = 4008;
	loc->ptime_ms = 30;
	loc->dtmf_pt = SDP_DTMF_PT_DEFAULT;
	loc->cn = true;
}

/*
 * One offer of ours answered by the fuzzed description, from the committed
 * state the caller names. The state selects the transport of the offer and the
 * one the take demands back (offer_srtp_is), so all three rows are walked: no
 * committed exchange, a committed plain one and a committed keyed one. The
 * keyed row is also what makes the take side's crypto branch reachable from a
 * leg that already ran an exchange.
 */
static void offer_take_walk(const struct sdp_msg *msg, const struct sdp_local *loc,
			    bool committed, uint8_t suite)
{
	struct sdp_state st;
	struct sip_buf b;
	char scratch[2048];

	sdp_state_init(&st, 0x1122334455667788ull);
	if (committed) {
		st.committed.valid = true;
		st.committed.srtp_suite = suite;
	}

	sip_buf_init(&b, scratch, sizeof(scratch));
	sdp_offer_build(&st, loc, &b);
	if (sdp_answer_take(&st, loc, msg) == SDP_OK)
		sdp_commit(&st);
	else
		sdp_rollback(&st);
}

static void local_walk(const struct sdp_msg *msg, const struct sdp_local *loc)
{
	struct sdp_state st;
	struct sip_buf b;
	char scratch[2048];

	sdp_state_init(&st, 0x1122334455667788ull);
	sip_buf_init(&b, scratch, sizeof(scratch));
	if (sdp_answer_build(&st, loc, msg, &b) == SDP_OK)
		sdp_commit(&st);
	else
		sdp_rollback(&st);

	/* the hold rows of the answer table only run with the local direction
	 * sendonly */
	sdp_hold_set(&st, true);
	sip_buf_init(&b, scratch, sizeof(scratch));
	sdp_answer_build(&st, loc, msg, &b);

	/*
	 * The fuzzed description as the answer to an offer of ours, once per
	 * committed row. A committed exchange also re-arms the answer build's
	 * own accept-plain gate, which the second call below drives with the
	 * same state the offer was built from.
	 */
	offer_take_walk(msg, loc, false, SDP_SRTP_NONE);
	offer_take_walk(msg, loc, true, SDP_SRTP_NONE);
	offer_take_walk(msg, loc, true, SDP_SRTP_AES_CM_128_SHA1_80);

	sdp_state_init(&st, 0x1122334455667788ull);
	st.committed.valid = true;
	st.committed.srtp_suite = SDP_SRTP_AES_CM_128_SHA1_32;
	sip_buf_init(&b, scratch, sizeof(scratch));
	sdp_answer_build(&st, loc, msg, &b);
}

/*
 * The T.38 walks. The answer gate also wants a committed exchange, because T.38
 * is a mid-call upgrade, so the flag is set by hand here: a fuzzed description
 * cannot be relied on to complete a real audio exchange first, and without it
 * the image branch stays unreachable.
 */
static void t38_walk(const struct sdp_msg *msg)
{
	struct sdp_local loc;
	struct sdp_state st;
	char scratch[2048];
	char small[48];
	struct sip_buf b;

	local_t38_init(&loc);

	sdp_state_init(&st, 0x1122334455667788ull);
	st.committed.valid = true;
	sip_buf_init(&b, scratch, sizeof(scratch));
	if (sdp_answer_build(&st, &loc, msg, &b) == SDP_OK)
		sdp_commit(&st);
	else
		sdp_rollback(&st);

	/* the same answer under a local hold, and into a buffer that overflows */
	sdp_hold_set(&st, true);
	st.committed.valid = true;
	sip_buf_init(&b, small, sizeof(small));
	sdp_answer_build(&st, &loc, msg, &b);

	/* our own T.38 offer, then the fuzzed description as its answer */
	sdp_state_init(&st, 0x1122334455667788ull);
	sdp_t38_set(&st, true);
	sip_buf_init(&b, scratch, sizeof(scratch));
	sdp_offer_build(&st, &loc, &b);
	if (sdp_answer_take(&st, &loc, msg) == SDP_OK)
		sdp_commit(&st);
	else
		sdp_rollback(&st);

	/* a T.38 offer that does not fit, then a second one that does */
	sdp_state_init(&st, 1);
	sdp_t38_set(&st, true);
	sip_buf_init(&b, small, sizeof(small));
	sdp_offer_build(&st, &loc, &b);
	sip_buf_init(&b, scratch, sizeof(scratch));
	sdp_offer_build(&st, &loc, &b);
}

static void answer_walk(const struct sdp_msg *msg)
{
	struct sdp_local loc;
	struct sdp_state st;
	char small[48];
	struct sip_buf b;

	local_init(&loc);
	local_walk(msg, &loc);

	local_min_init(&loc);
	local_walk(msg, &loc);

	local_min4_init(&loc);
	local_walk(msg, &loc);

	local_srtp_init(&loc);
	local_walk(msg, &loc);

	local_g722_init(&loc);
	local_walk(msg, &loc);

	local_g726_init(&loc);
	local_walk(msg, &loc);

	local_opus_init(&loc);
	local_walk(msg, &loc);

	t38_walk(msg);

	/* a take without a pending offer must refuse before it reads anything */
	local_init(&loc);
	sdp_state_init(&st, 0x1122334455667788ull);
	sdp_answer_take(&st, &loc, msg);

	/* the truncated answer path: every writer must stop at the buffer end */
	sip_buf_init(&b, small, sizeof(small));
	sdp_answer_build(&st, &loc, msg, &b);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct sdp_msg msg;
	char *buf;

	if (!size || size > SIP_MSG_LEN_MAX)
		return 0;

	buf = malloc(size);
	if (!buf)
		return 0;
	memcpy(buf, data, size);

	if (sdp_parse(sip_str_init(buf, size), &msg))
		answer_walk(&msg);

	free(buf);

	return 0;
}

#ifndef UVOICE_FUZZ_LIBFUZZER
/*
 * CI without clang still compiles and links this file. With no arguments it is
 * a no-op; with file arguments it replays the saved input files, which is how
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
