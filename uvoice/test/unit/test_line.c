// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the line shell: the core half (src/line/line.c) and the SIP
 * half (src/proto/sip/line.c) together.
 *
 * Isolation. This binary compiles line/line.c, proto/sip/line.c and
 * proto/sip/proto.c with the real proto/proto.c, proto/sip/fsm.c, dialplan.c,
 * proto/sip/features.c and cdr.c (src/CMakeLists.txt). A test of the shell
 * has value only against the real
 * machine and the real matcher. A local hal_backend_ops (a struct hal_ops)
 * replaces the backend, and the binary does not link stub.c. Local definitions
 * of the call.h API replace call.c. These recorders let a case assert the exact
 * order of the HAL ops, which the detector-before-dial-tone rule is about.
 *
 * The clock is real. sip/line.c owns its uloop timer environment
 * (sip_txn_env_uloop_init), and line/line.c runs uloop timers of its own.
 * line/line.h and proto/proto.h are fixed interfaces with no hook for a virtual
 * one. Timer cases therefore run the real uloop for a bounded window and assert
 * around the expiry, with margins far larger than scheduler noise. The suite is
 * slower, and in return the shell has no hook that only tests would use. Cases
 * assert timer values by behavior. A one second start-digit timer must not
 * fire at 300 ms and must fire by 1300 ms, which checks the conversion from
 * seconds to milliseconds.
 *
 * The stub define is on. A cmake build with -DUVOICE_HAL=stub compiles every
 * suite with UVOICE_HAL_STUB (src/CMakeLists.txt add_definitions).
 * LINE_DIAL_TONE_DEFER_MS is therefore 0, and the dial tone defer itself is
 * invisible in this build. The defer cases check the class through
 * line_tone_is_dial() instead. src/CMakeLists.txt adds the stub define only
 * for the stub backend. The timing half therefore compiles only in a build
 * with the udsp backend.
 *
 * State is read through the dump. The line array is static to line/line.c, so a
 * case reads the machine state through lines_status_dump(). That also keeps the
 * status keys under test.
 */
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubox/ulog.h>
#include <libubus.h>

#include "proto/sip/account.h"
#include "proto/sip/call.h"
#include "trace.h"
#include "proto/sip/fsm.h"
#include "cdr.h"
#include "calltrace.h"
#include "cfg/cfg.h"
#include "dialplan.h"
#include "event.h"
#include "fw.h"
#include "hal/hal.h"
#include "line/line.h"
#include "net/fw_reconcile.h"
#include "net/iface.h"
#include "net/ports.h"
#include "proto/proto.h"
#include "proto/sip/line.h"
#include "proto/sip/tap.h"
#include "proto/sip/transport.h"
#include "uvoice.h"
#include "test_util.h"
#include "test_uloop.h"

#define WARN_CAPTURE_ULOG_OWN
#include "test_warn.h"

#define HOP_MAX			64
#define COP_MAX			64

/*
 * The truncation case below only tests a truncation while a configurable
 * warmline number can be longer than the buffer the machine dials from. Pin
 * the relation, so a narrower CFG_STR_MAX turns that case into a build
 * failure instead of a case that quietly proves nothing.
 */
_Static_assert(CFG_STR_MAX - 1 > FSM_DIGITS_MAX,
	       "a warmline number must be able to overflow the FSM digit buffer");

struct uvoice uv;

static struct {
	unsigned int state_count;
	unsigned int state_line;
	enum fsm_state before;
	enum fsm_state after;
	/* where the line rests at the notification, the reader the call trace
	 * runs on every move (calltrace.c calltrace_line_state) */
	enum line_rest rest;
	unsigned int start_count;
	unsigned int start_line;
	uint32_t seq;
	unsigned int event_count;
	unsigned int event_line;
	char reason[32];
} trace_note;

/* The machine state a name of fsm_state_name() names, FSM_OOS for none. */
static enum fsm_state fsm_state_of_name(const char *name)
{
	int st;

	for (st = FSM_IDLE; st <= FSM_SILENT; st++) {
		if (name && !strcmp(fsm_state_name((enum fsm_state)st), name))
			return (enum fsm_state)st;
	}

	return FSM_OOS;
}

void calltrace_line_state(unsigned int line, const char *before,
			  const char *after)
{
	trace_note.state_count++;
	trace_note.state_line = line;
	trace_note.before = fsm_state_of_name(before);
	trace_note.after = fsm_state_of_name(after);
	trace_note.rest = line_rest_of(line);
}

void calltrace_call_started(unsigned int line, uint32_t cdr_seq)
{
	trace_note.start_count++;
	trace_note.start_line = line;
	trace_note.seq = cdr_seq;
}

void calltrace_event(unsigned int line, const char *reason)
{
	trace_note.event_count++;
	trace_note.event_line = line;
	snprintf(trace_note.reason, sizeof(trace_note.reason), "%s", reason);
}

/* the backend owns this type; the recorder below is this suite's backend */
struct hal {
	int marker;
};

static struct hal fake_hal = { .marker = 1 };

/* ------------------------------------------------------------ HAL recorder */

enum hop_kind {
	H_CHAN_CFG = 0,
	H_TONE,
	H_TONE_STOP,
	H_CW_TONE,
	H_RING_START,
	H_RING_STOP,
	H_CID,
	H_STRM_CFG,
	H_STRM_START,
	H_STRM_STOP,
	H_STRM_DTMF,
	H_STRM_STATS,
	H_HOOK_TS,
	H_POLARITY,
	H_LINE_STATE,
	H_FAX,
	H_LINE_TEST,
	H_CONF,
	H_DIGIT_PLAY,
	H_LOOP,
};

struct hop {
	enum hop_kind kind;
	unsigned int line;
	/* tone id, cid kind, stream index, conference mode */
	unsigned int val;
	unsigned int mask;		/* tone duration */
	char digit;
	struct hal_ring ring;
	struct hal_cid cid;
	struct hal_hook_timing hook;
	/* the whole channel view, not only its mask: hal.h makes the op
	 * authoritative for the gains and the canceller too, so a case has to
	 * be able to read every field the shell pushed */
	struct hal_channel_cfg chan;
	/* the whole fax profile, for the same reason */
	struct hal_fax_profile fax;
	/* the whole loop request, so a case reads the mode, the peer and the
	 * delay the controller asked for */
	struct hal_loop loop;
};

static struct hop hops[HOP_MAX];
static unsigned int nhops;
/* the stub backend advertises the polarity feature (stub.c), so the
 * recorder does too; the availability cases
 * take it away and fixture_start puts it back */
static struct hal_cap fake_cap = {
	.backend = "test",
	.lines = 2,
	.streams_per_chan = 2,
	.features = HAL_FEAT_POLARITY,
	.codecs = (1u << HAL_CODEC_PCMU) | (1u << HAL_CODEC_PCMA),
};

static struct hop *hop_add(enum hop_kind kind, unsigned int line)
{
	static struct hop sink;
	struct hop *h = nhops < HOP_MAX ? &hops[nhops++] : &sink;

	memset(h, 0, sizeof(*h));
	h->kind = kind;
	h->line = line;

	return h;
}

static void hops_reset(void)
{
	nhops = 0;
}

/*
 * The lookups never return NULL. A failing CHECK must not take the case down
 * with a NULL dereference on the next line, because a suite that crashes on a
 * failure cannot be used to prove that a case catches a fault.
 */
static struct hop hop_none;

static bool hop_has(enum hop_kind kind)
{
	unsigned int i;

	for (i = 0; i < nhops; i++) {
		if (hops[i].kind == kind)
			return true;
	}

	return false;
}

static struct hop *hop_find(enum hop_kind kind)
{
	unsigned int i;

	for (i = 0; i < nhops; i++) {
		if (hops[i].kind == kind)
			return &hops[i];
	}

	memset(&hop_none, 0, sizeof(hop_none));

	return &hop_none;
}

static struct hop *hop_last(enum hop_kind kind)
{
	struct hop *found = NULL;
	unsigned int i;

	for (i = 0; i < nhops; i++) {
		if (hops[i].kind == kind)
			found = &hops[i];
	}

	if (found)
		return found;

	memset(&hop_none, 0, sizeof(hop_none));

	return &hop_none;
}

static unsigned int hop_count(enum hop_kind kind)
{
	unsigned int i, n = 0;

	for (i = 0; i < nhops; i++) {
		if (hops[i].kind == kind)
			n++;
	}

	return n;
}

static int hop_index(enum hop_kind kind)
{
	unsigned int i;

	for (i = 0; i < nhops; i++) {
		if (hops[i].kind == kind)
			return (int)i;
	}

	return -1;
}

static struct hal *fake_open(int event_fd)
{
	return &fake_hal;
}

static void fake_close(struct hal *h)
{
}

static const struct hal_cap *fake_cap_get(struct hal *h)
{
	return &fake_cap;
}

static void fake_counters_get(struct hal *h, struct hal_counters *out)
{
	memset(out, 0, sizeof(*out));
}

/*
 * A backend that refuses the bell. libudsp answers HAL_TRANSIENT to a ring on
 * any line while a metallic line test runs, and there is no other way to drive
 * the shell through that answer. The count says how many of the next starts
 * refuse and the status says with what.
 *
 * This records the attempt either way, as the hook timing and battery
 * recorders record theirs, and puts the answer in val. A case that asks how
 * many bells really started reads ring_ok_count(); a case that asks how many
 * attempts left the shell reads hop_count().
 */
static enum hal_status ring_start_status = HAL_OK;
static unsigned int ring_start_fail_n;

static enum hal_status fake_ring_start(struct hal *h, unsigned int line,
				       const struct hal_ring *ring)
{
	struct hop *op = hop_add(H_RING_START, line);
	enum hal_status st = HAL_OK;

	op->ring = *ring;

	if (ring_start_fail_n) {
		ring_start_fail_n--;
		st = ring_start_status;
	}

	op->val = st;

	return st;
}

static unsigned int ring_ok_count(void)
{
	unsigned int i, n = 0;

	for (i = 0; i < nhops; i++) {
		if (hops[i].kind == H_RING_START && hops[i].val == HAL_OK)
			n++;
	}

	return n;
}

static enum hal_status fake_ring_stop(struct hal *h, unsigned int line)
{
	hop_add(H_RING_STOP, line);

	return HAL_OK;
}

/*
 * The loop test audio path. The status is settable for the same reason the
 * bell's is: the controller's single retry is reachable only through a
 * transient refusal, and the count says how many of the next calls answer it.
 * The request is recorded whatever the answer was, so a case that asks what
 * the shell requested reads hop->loop and one that asks what the backend
 * accepted reads hop->mask.
 */
static enum hal_status loop_set_status = HAL_OK;
static unsigned int loop_set_fail_n;

static enum hal_status fake_loop_set(struct hal *h, unsigned int line,
				     const struct hal_loop *loop)
{
	struct hop *op = hop_add(H_LOOP, line);
	enum hal_status st = HAL_OK;

	op->loop = *loop;
	op->val = loop->mode;

	if (loop_set_fail_n) {
		loop_set_fail_n--;
		st = loop_set_status;
	}

	op->mask = st;

	return st;
}

/* the loop hops of one line, which is what a two-port case counts */
static unsigned int loop_count_line(unsigned int line, uint8_t mode)
{
	unsigned int i, n = 0;

	for (i = 0; i < nhops; i++) {
		if (hops[i].kind != H_LOOP || hops[i].line != line)
			continue;
		if (hops[i].loop.mode == mode)
			n++;
	}

	return n;
}

/*
 * The hook timing push: what the shell programmed and how often. The hop
 * recorder alone cannot answer it, because the bring-up push happens inside
 * fixture_start() and the fixture clears the hops behind it.
 */
static enum hal_status hook_ts_status = HAL_OK;
static unsigned int hook_ts_transient;		/* how many of the next pushes refuse */
static struct hal_hook_timing hook_ts[HAL_LINE_MAX];
static unsigned int hook_ts_calls[HAL_LINE_MAX];

static enum hal_status fake_hook_timing(struct hal *h, unsigned int line,
					const struct hal_hook_timing *ts)
{
	hop_add(H_HOOK_TS, line)->hook = *ts;

	if (line < ARRAY_SIZE(hook_ts)) {
		hook_ts[line] = *ts;
		hook_ts_calls[line]++;
	}

	/* The stub backend refuses the op for a line the board does not
	 * carry (stub line_get); this recorder does the same.
	 * The attempt is counted above it, so a push that should never have
	 * left is visible. */
	if (line >= fake_cap.lines)
		return HAL_FATAL;

	if (hook_ts_transient) {
		hook_ts_transient--;
		return HAL_TRANSIENT;
	}

	return hook_ts_status;
}

/*
 * The start-up hook query. What each line reports, how the op answers, and
 * how often it was asked: the last one is what pins the OOS skip, because a
 * line that is never queried cannot be seized by accident.
 *
 * The recorder answers on hook by default, which is the stub backend's own
 * start state (stub.c takes l->offhook from the seize and release verbs alone),
 * so every fixture that says nothing about the hook boots on hook.
 *
 * Every knob is per line, so a case can refuse one line, answer another, and
 * pin that the closing loop of lines_init() keeps going through the refusal.
 */
static bool boot_offhook[HAL_LINE_MAX];
static enum hal_status hook_query_status[HAL_LINE_MAX];
static unsigned int hook_query_transient[HAL_LINE_MAX];	/* how many of the next answers refuse */
static unsigned int hook_query_calls[HAL_LINE_MAX];

static enum hal_status fake_hook_query(struct hal *h, unsigned int line, bool *offhook)
{
	if (line < ARRAY_SIZE(hook_query_calls))
		hook_query_calls[line]++;

	/* The stub backend refuses the op for a line the board does not
	 * carry (stub line_get); this recorder does the same.
	 * The attempt is counted above it, so a query that should never have
	 * left is visible. */
	if (line >= fake_cap.lines || line >= ARRAY_SIZE(boot_offhook))
		return HAL_FATAL;

	/*
	 * A refused query still writes the answer, and it writes the one that
	 * would seize the line. hal.h says nothing about the out parameter of a
	 * refused op, so a shell that reads it before it reads the status has to
	 * fail the refusal case instead of passing it by luck. The transient arm
	 * writes it for the same reason: the retry is what supplies the answer,
	 * never the refusal in front of it.
	 */
	if (hook_query_transient[line]) {
		hook_query_transient[line]--;
		*offhook = true;
		return HAL_TRANSIENT;
	}

	if (hook_query_status[line] != HAL_OK) {
		*offhook = true;
		return hook_query_status[line];
	}

	*offhook = boot_offhook[line];

	return HAL_OK;
}

/* the SLIC refuses the reversal, which must not stop the restore */
static enum hal_status polarity_status = HAL_OK;

/*
 * The battery feed itself (hal.h line_state_set). It is the whole removal form
 * of the forward disconnect: HAL_LINE_DOWN is the loop break and HAL_LINE_ACTIVE
 * is the restore, and no other op tells them apart. The knob is what a refused
 * removal is driven through.
 */
static enum hal_status line_state_status = HAL_OK;
/* how many of the next calls refuse, which is how the restore retry is driven:
 * the attempt is recorded either way, so a refusal is visible as an attempt */
static unsigned int line_state_fail_n;

static enum hal_status fake_line_state(struct hal *h, unsigned int line,
				       enum hal_line_state state)
{
	hop_add(H_LINE_STATE, line)->val = state;

	if (line_state_fail_n) {
		line_state_fail_n--;
		return HAL_FATAL;
	}

	return line_state_status;
}

static enum hal_status fake_polarity(struct hal *h, unsigned int line,
				     enum hal_polarity pol)
{
	hop_add(H_POLARITY, line)->val = pol;

	return polarity_status;
}

static enum hal_status fake_cid_send(struct hal *h, unsigned int line,
				     enum hal_cid_kind kind, const struct hal_cid *cid)
{
	struct hop *op = hop_add(H_CID, line);

	op->val = kind;
	op->cid = *cid;

	return HAL_OK;
}

/* a backend with a number-unobtainable tone, which stub lacks */
static bool tone_unobtainable_ok;

static enum hal_status fake_tone_play(struct hal *h, unsigned int line,
				      enum hal_tone tone, uint32_t duration_ms)
{
	struct hop *op = hop_add(H_TONE, line);

	op->val = tone;
	op->mask = duration_ms;

	/* stub.c carries neither tone and refuses both. */
	if ((tone == HAL_TONE_NUMBER_UNOBTAINABLE || tone == HAL_TONE_SIT) &&
	    !tone_unobtainable_ok)
		return HAL_UNSUPPORTED;

	return HAL_OK;
}

static enum hal_status fake_tone_stop(struct hal *h, unsigned int line)
{
	hop_add(H_TONE_STOP, line);

	return HAL_OK;
}

static enum hal_status fake_cw_tone_play(struct hal *h, unsigned int line,
					 unsigned int variant, uint32_t duration_ms)
{
	struct hop *op = hop_add(H_CW_TONE, line);

	op->val = variant;
	op->mask = duration_ms;

	return HAL_OK;
}

/* the digit the far end sent, played toward the phone; the status is what the
 * backend answers, so a case can drive the refusal arm */
static enum hal_status digit_play_status = HAL_OK;

static enum hal_status fake_digit_play(struct hal *h, unsigned int line,
				       char digit, uint32_t duration_ms)
{
	struct hop *op = hop_add(H_DIGIT_PLAY, line);

	op->digit = digit;
	op->mask = duration_ms;

	return digit_play_status;
}

/* how many of the next channel_config_set calls refuse, and with what */
static unsigned int chan_cfg_transient;
static bool chan_cfg_fatal;

static enum hal_status fake_chan_cfg(struct hal *h, unsigned int line,
				     const struct hal_channel_cfg *cfg)
{
	hop_add(H_CHAN_CFG, line)->chan = *cfg;

	if (chan_cfg_fatal)
		return HAL_FATAL;

	if (chan_cfg_transient) {
		chan_cfg_transient--;
		return HAL_TRANSIENT;
	}

	return HAL_OK;
}

static enum hal_status fake_strm_cfg(struct hal *h, unsigned int line, unsigned int stream,
				     const struct hal_media *media)
{
	hop_add(H_STRM_CFG, line)->val = stream;

	return HAL_OK;
}

/*
 * A backend that refuses the start. The shell takes its running-stream
 * tracking from this op's answer, so without a knob here the only refusal a
 * case could drive is the no-HAL guard in front of the op, which never
 * reaches the backend and never reaches the tracking either.
 */
static enum hal_status strm_start_status = HAL_OK;

static enum hal_status fake_strm_start(struct hal *h, unsigned int line, unsigned int stream)
{
	hop_add(H_STRM_START, line)->val = stream;

	return strm_start_status;
}

static enum hal_status fake_strm_stop(struct hal *h, unsigned int line, unsigned int stream)
{
	hop_add(H_STRM_STOP, line)->val = stream;

	return HAL_OK;
}

static enum hal_status fake_strm_dtmf(struct hal *h, unsigned int line, unsigned int stream,
				      char digit, uint32_t duration_ms)
{
	struct hop *op = hop_add(H_STRM_DTMF, line);

	op->val = stream;
	op->digit = digit;

	return HAL_OK;
}

/*
 * The recorded HAL end of the fax passthrough: what profile reached which
 * stream, and how many of the next calls answer HAL_TRANSIENT so the hal.h
 * section 3 retry-once is observable.
 */
static enum hal_status fax_apply_status = HAL_OK;
static unsigned int fax_apply_transient;

static enum hal_status fake_fax_apply(struct hal *h, unsigned int line, unsigned int stream,
				      const struct hal_fax_profile *fax)
{
	struct hop *op = hop_add(H_FAX, line);

	op->val = stream;
	op->fax = *fax;

	if (fax_apply_transient) {
		fax_apply_transient--;
		return HAL_TRANSIENT;
	}

	return fax_apply_status;
}

/*
 * The media snapshot of call-records, which is a slow op like the region load:
 * the start is recorded and answers a token, the numbers are read back with the
 * fast result op after the case dispatches the completion. The values are
 * settable, so a case can prove the record carries what the backend reported
 * instead of a constant this suite and cdr.c happen to share.
 */
static struct hal_media_stats fake_stats;
static enum hal_status stats_start_status = HAL_OK;
static enum hal_status stats_result_status = HAL_OK;
static unsigned int stats_starts;
static uint32_t stats_token;

/* one distinct value per member, so a crossed pair cannot pass a case */
static void stats_seed(struct hal_media_stats *st, uint64_t base)
{
	memset(st, 0, sizeof(*st));
	st->packets_sent = base;
	st->packets_recv = base + 1;
	st->packets_lost = base + 2;
	st->bytes_sent = base + 3;
	st->bytes_recv = base + 4;
	st->mos_lq = 4.3f;
	st->mos_cq = 4.1f;
	st->rfactor = 88;
}

static enum hal_status fake_stats_start(struct hal *h, unsigned int line,
					unsigned int stream, struct hal_job *job)
{
	hop_add(H_STRM_STATS, line)->val = stream;
	stats_starts++;

	if (stats_start_status != HAL_OK)
		return stats_start_status;

	job->token = ++stats_token;

	return HAL_OK;
}

static enum hal_status fake_stats_result(struct hal *h, unsigned int line,
					 unsigned int stream,
					 struct hal_media_stats *out)
{
	if (stats_result_status != HAL_OK)
		return stats_result_status;

	*out = fake_stats;
	/*
	 * The answer depends on the stream. Every stream of this fake carries
	 * one seed, so a completion that read the numbers of the wrong stream
	 * would fill the record with values that match by luck. The offset
	 * makes that fill visible in the record instead.
	 */
	out->packets_sent += stream;
	out->bytes_recv += stream;

	return HAL_OK;
}

static enum hal_status line_test_start_status = HAL_OK;
static enum hal_status line_test_result_status = HAL_OK;
static uint8_t line_test_record[LINE_TEST_RECORD_MAX];
static size_t line_test_record_len;
static uint32_t line_test_token;

static enum hal_status fake_line_test_start(struct hal *h, unsigned int line,
					     enum hal_line_test test,
					     struct hal_job *job)
{
	hop_add(H_LINE_TEST, line)->val = test;
	if (line_test_start_status != HAL_OK)
		return line_test_start_status;

	job->token = ++line_test_token;

	return HAL_OK;
}

static enum hal_status fake_line_test_result(struct hal *h,
					      unsigned int line, void *buf,
					      size_t buf_len, size_t *out_len)
{
	size_t copy_len = line_test_record_len < buf_len ?
		line_test_record_len : buf_len;

	if (line_test_result_status != HAL_OK)
		return line_test_result_status;

	memcpy(buf, line_test_record, copy_len);
	*out_len = line_test_record_len;

	return HAL_OK;
}

/* the country tone table load: what it was asked for and how it answers */
static char region_seen[8];
static unsigned int region_calls;
static enum hal_status region_status = HAL_OK;
static uint32_t region_token;

static enum hal_status fake_region_apply(struct hal *h, const char *iso3,
					 struct hal_job *job)
{
	snprintf(region_seen, sizeof(region_seen), "%s", iso3 ? iso3 : "");
	region_calls++;

	if (region_status != HAL_OK)
		return region_status;

	job->token = ++region_token;

	return HAL_OK;
}

/* the region read-back: what the getter answers when a case arms it */
static struct hal_region fake_region;
static enum hal_status region_get_status = HAL_UNSUPPORTED;
static unsigned int region_get_calls;

static enum hal_status fake_region_get(struct hal *h, struct hal_region *out)
{
	region_get_calls++;

	if (region_get_status != HAL_OK)
		return region_get_status;

	*out = fake_region;

	return HAL_OK;
}

/*
 * The shape the fake answers unless a case seeds its own: the compiled
 * default of line/line.c with a name, so a case that changes nothing sees the
 * numbers the daemon stands on before any read-back, and a case that seeds a
 * field sees that field alone move. cfg_default() calls it, because the
 * country data is config to the cases, and fixture_start() leaves it alone,
 * because a case seeds it before the bring-up and completes the region job
 * behind it (region_ready) to land it.
 */
static void fake_region_default(void)
{
	memset(&fake_region, 0, sizeof(fake_region));
	snprintf(fake_region.iso3, sizeof(fake_region.iso3), "USA");
	snprintf(fake_region.describe, sizeof(fake_region.describe), "Fake USA");
	fake_region.cid_format = HAL_CID_BELLCORE_FSK;
	fake_region.mwi_format = HAL_CID_BELLCORE_FSK;
	fake_region.hook_flash_min_ms = 300;
	fake_region.hook_flash_max_ms = 1300;
	fake_region.hook_release_min_ms = 1301;
	fake_region.flash_service = HAL_FLASH_SERVICE_DROP;
	region_get_status = HAL_OK;
}

/* A view the default cannot be mistaken for: etsi-fsk is no default, the
 * windows differ from the compiled ones, and template r4 is unique, so every
 * assertion that sees these values saw the read-back. */
static void region_readback_seed(void)
{
	memset(&fake_region, 0, sizeof(fake_region));
	snprintf(fake_region.iso3, sizeof(fake_region.iso3), "GBR");
	fake_region.cid_format = HAL_CID_ETSI_FSK;
	fake_region.mwi_format = HAL_CID_ETSI_DTAS;
	fake_region.hook_flash_min_ms = 35;
	fake_region.hook_flash_max_ms = 145;
	fake_region.hook_release_min_ms = 590;
	fake_region.cw_sas_total_ms = 70000;
	fake_region.ring[0].cadence_count = 1;
	fake_region.ring[0].cadence[0].on_ms = 400;
	fake_region.ring[0].cadence[0].off_ms = 2000;
	fake_region.ring[3].cadence_count = 2;
	fake_region.ring[3].cadence[0].on_ms = 111;
	fake_region.ring[3].cadence[0].off_ms = 222;
	fake_region.ring[3].cadence[1].on_ms = 333;
	fake_region.ring[3].cadence[1].off_ms = 444;
	fake_region.flash_service = HAL_FLASH_SERVICE_DROP;
	snprintf(fake_region.describe, sizeof(fake_region.describe),
		 "Read-back GBR");
	region_get_status = HAL_OK;
}

/*
 * The jitter buffer counters are a fast op, which is what lets the shell read
 * them without a job. The status is settable, because the case that matters
 * is the one where this op answers and the slow media query refuses: the
 * record has to carry the buffer anyway, and the only way to see the ordering
 * is to make the two disagree.
 */
static struct hal_jb_stats fake_jb;
static enum hal_status jb_stats_status = HAL_OK;
static unsigned int jb_stats_reads;

static void jb_seed(struct hal_jb_stats *jb, uint64_t base)
{
	memset(jb, 0, sizeof(jb[0]));
	jb->frames_out = base;
	jb->plc = base + 1;
	jb->late_drop = base + 2;
	jb->underruns = base + 3;
	jb->target_ms = 50;
	jb->depth_ms = 44;
	jb->pool_free = 60;
}

static enum hal_status fake_jb_stats_get(struct hal *h, unsigned int line,
					 unsigned int stream,
					 struct hal_jb_stats *out)
{
	jb_stats_reads++;
	if (jb_stats_status != HAL_OK)
		return jb_stats_status;

	*out = fake_jb;
	/* the stream offset of fake_stats_result, for the same reason */
	out->frames_out += stream;

	return HAL_OK;
}

static struct hal_lec_stats fake_lec;
static enum hal_status lec_stats_status = HAL_OK;

static void lec_seed(struct hal_lec_stats *lec, uint32_t base)
{
	memset(lec, 0, sizeof(lec[0]));
	lec->tail_ms = base;
	lec->taps = base + 1;
	lec->frames = base + 2;
	lec->erle_mdb = -(int32_t)base;
	lec->ref_ready = 1;
	lec->nlp_quiet_frames = base + 3;
	lec->coef_peak_tap = base + 4;
}

static enum hal_status fake_lec_stats_get(struct hal *h, unsigned int line,
					  struct hal_lec_stats *out)
{
	if (lec_stats_status != HAL_OK)
		return lec_stats_status;

	*out = fake_lec;
	/* a per line offset so a row filled from the wrong line is visible */
	out->tail_ms += line;

	return HAL_OK;
}

/*
 * The reference clock is read directly and not through uvoice_mono_ms(): a
 * bracket built with the same call the code under test makes would hold
 * whatever clock that call happened to read, so a helper switched to
 * CLOCK_REALTIME would pass. This one is CLOCK_MONOTONIC by construction.
 */
static uint32_t mono_ms_ref(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/*
 * The mixer op (hal.h conf_set). The hop keeps the mode, which is what says an
 * on reached the backend as the local mix and an off as the off mode.
 *
 * Three levers stand for the three refusals hal.h names, because the shell
 * answers each of them differently: a run of transient answers, which is what
 * pins the retry at exactly one; the unsupported answer a backend without
 * HAL_FEAT_CONF gives; and the fatal one.
 */
/* how many of the next calls answer busy */
static unsigned int conf_transient_n;
static bool conf_unsupported;
static bool conf_refuse;

static enum hal_status fake_conf_set(struct hal *h, unsigned int line,
				     enum hal_conf_mode mode)
{
	hop_add(H_CONF, line)->val = mode;

	if (conf_transient_n) {
		conf_transient_n--;
		return HAL_TRANSIENT;
	}

	if (conf_unsupported)
		return HAL_UNSUPPORTED;

	return conf_refuse ? HAL_FATAL : HAL_OK;
}

const struct hal_ops hal_backend_ops = {
	.name = "test",
	.open = fake_open,
	.close = fake_close,
	.cap_get = fake_cap_get,
	.counters_get = fake_counters_get,
	.line_hook_timing_set = fake_hook_timing,
	.line_hook_query = fake_hook_query,
	.line_state_set = fake_line_state,
	.line_polarity_set = fake_polarity,
	.line_ring_start = fake_ring_start,
	.line_ring_stop = fake_ring_stop,
	.line_cid_send = fake_cid_send,
	.tone_play = fake_tone_play,
	.tone_stop = fake_tone_stop,
	.cw_tone_play = fake_cw_tone_play,
	.digit_play = fake_digit_play,
	.channel_config_set = fake_chan_cfg,
	.stream_config_set = fake_strm_cfg,
	.stream_start = fake_strm_start,
	.stream_stop = fake_strm_stop,
	.stream_dtmf_send = fake_strm_dtmf,
	.fax_profile_apply = fake_fax_apply,
	.stream_stats_start = fake_stats_start,
	.stream_stats_result_get = fake_stats_result,
	.stream_jb_stats_get = fake_jb_stats_get,
	.line_lec_stats_get = fake_lec_stats_get,
	.region_apply = fake_region_apply,
	.region_get = fake_region_get,
	.line_test_start = fake_line_test_start,
	.line_test_result_get = fake_line_test_result,
	.conf_set = fake_conf_set,
	.line_loop_set = fake_loop_set,
};

/*
 * The slow job plumbing of main.c. line/line.c hands the region job to job_add
 * and takes it back with job_take, and the daemon dispatches the completion
 * from the event reader; here the case dispatches it by hand.
 */
static struct hal_job *job_pending;

void job_add_budget(struct hal_job *job, unsigned int budget_ms)
{
	job_pending = job;
	job->budget_ms = budget_ms;
	uloop_timeout_set(&job->timeout, budget_ms);
}

void job_add(struct hal_job *job)
{
	job_add_budget(job, HAL_JOB_TIMEOUT_MS);
}

struct hal_job *job_take(uint32_t token)
{
	struct hal_job *job = job_pending;

	if (!job || job->token != token)
		return NULL;

	uloop_timeout_cancel(&job->timeout);
	job_pending = NULL;

	return job;
}

static void job_complete(enum hal_status status)
{
	struct hal_job *job = job_pending;
	struct hal_event ev = { .type = HAL_EVENT_JOB_DONE };

	if (!job) {
		printf("    no job to complete\n");
		return;
	}

	uloop_timeout_cancel(&job->timeout);
	job_pending = NULL;
	job->cb(job, status, &ev);
}

static void job_timeout(void)
{
	struct hal_job *job = job_pending;

	if (!job) {
		printf("    no job to time out\n");
		return;
	}

	uloop_timeout_cancel(&job->timeout);
	job_pending = NULL;
	job->cb(job, HAL_TRANSIENT, NULL);
}

/* ----------------------------------------------------------- call recorder */

enum cop_kind {
	C_INIT = 0,
	C_RELOAD,
	C_HAL_READY,
	C_HAL_FEATURES,
	C_DONE,
	C_TEARDOWN,
	C_DIAL,
	C_RING,
	C_ANSWER,
	C_REJECT,
	C_HANGUP,
	C_HOLD,
	C_RESUME,
	C_DTMF,
	C_FLASH_RELAY,
	C_FORWARD,
	C_FAX,
	/* the answer tone from the wire, recorded on the leg for the far-end
	 * upspeed recognizer */
	C_FAX_HINT,
	C_FAX_ON,
	C_FAX_KIND_GET,
	/* the kind of a passthrough the preamble named: val is the enum
	 * hal_passthrough the shell asked for */
	C_FAX_KIND,
	/* one tone row against the return-to-voice rule: val is the tone code,
	 * duration_ms its reading, slot the leg and measured the end edge */
	C_FAX_TONE_NOTE,
	/* the handle a deferral takes for its leg */
	C_LEG_HANDLE,
	/* the return to voice itself: val is the handle the timer carried */
	C_FAX_EXIT,
	C_T38,
	/* the REFER of one leg: slot is the leg, val the enum call_xfer_kind
	 * the shell mapped the star-code row onto, num the target */
	C_XFER,
	/* the on-hook consult REFER: slot is the referee, val the replaced leg */
	C_XFER_CONSULT,
	/* the daemon-stop end of every leg of one line (call.h
	 * calls_line_end) */
	C_LINE_END,
};

struct cop {
	enum cop_kind kind;
	unsigned int line;
	unsigned int slot;
	uint32_t val;
	uint16_t duration_ms;
	/* the feed and the edge of a tone row (hal.h u.tone) */
	uint8_t dir;
	bool end;
	bool measured;			/* a detector timed the digit */
	bool cid_present;		/* the cid verdict a dial carried */
	char num[FSM_DIGITS_MAX + 1];
};

static struct cop cops[COP_MAX];
static unsigned int ncops;
static struct call_cb rec_cb;
static struct call_media_ops rec_media;
static unsigned int dial_fail_times;	/* synchronous failures per call_dial */

static struct cop *cop_add(enum cop_kind kind, unsigned int line)
{
	static struct cop sink;
	struct cop *c = ncops < COP_MAX ? &cops[ncops++] : &sink;

	memset(c, 0, sizeof(*c));
	c->kind = kind;
	c->line = line;

	return c;
}

static void cops_reset(void)
{
	ncops = 0;
}

static struct cop cop_none;

static bool cop_has(enum cop_kind kind)
{
	unsigned int i;

	for (i = 0; i < ncops; i++) {
		if (cops[i].kind == kind)
			return true;
	}

	return false;
}

static struct cop *cop_find(enum cop_kind kind)
{
	unsigned int i;

	for (i = 0; i < ncops; i++) {
		if (cops[i].kind == kind)
			return &cops[i];
	}

	memset(&cop_none, 0, sizeof(cop_none));

	return &cop_none;
}

/* where an op landed in the recorded order, -1 when it never did */
static int cop_index(enum cop_kind kind)
{
	unsigned int i;

	for (i = 0; i < ncops; i++) {
		if (cops[i].kind == kind)
			return (int)i;
	}

	return -1;
}

static unsigned int cop_count(enum cop_kind kind)
{
	unsigned int i, n = 0;

	for (i = 0; i < ncops; i++) {
		if (cops[i].kind == kind)
			n++;
	}

	return n;
}

/* how often one op named one slot, which is what "both legs, exactly once
 * each" needs: a cleanup that hung the same leg up twice would pass a count of
 * two on its own */
static unsigned int cop_slot_count(enum cop_kind kind, unsigned int slot)
{
	unsigned int i, n = 0;

	for (i = 0; i < ncops; i++) {
		if (cops[i].kind == kind && cops[i].slot == slot)
			n++;
	}

	return n;
}

/*
 * The two things the leg layer needs before it can dial, tracked here because
 * call_dial() below refuses without them. calls_init() wires the vtable and the
 * callbacks, calls_reload() gives the layer a config, and lines_init() records
 * that "a call_dial before that would refuse silently". A recorder that took a
 * dial anyway would report a call the daemon never placed, which is exactly
 * what an ordering change inside lines_init() has to be caught by.
 */
static bool calls_inited;
static bool calls_cfg_loaded;

void calls_init(const struct call_media_ops *ops, const struct call_cb *cb)
{
	cop_add(C_INIT, 0);
	memset(&rec_media, 0, sizeof(rec_media));
	memset(&rec_cb, 0, sizeof(rec_cb));
	if (ops)
		rec_media = *ops;
	if (cb)
		rec_cb = *cb;

	calls_inited = true;
}

void calls_reload(const struct cfg *cfg)
{
	cop_add(C_RELOAD, 0);
	calls_cfg_loaded = true;
}

/*
 * The admission predicate the shell hands the leg layer (call.h
 * calls_hal_ready_bind). The recorder keeps the function, because what the
 * split has to prove is not that a value was copied once: it is that the
 * answer the leg layer reads follows the daemon HAL state afterwards.
 */
static bool (*rec_hal_ready)(void);

void calls_hal_ready_bind(bool (*hal_ready)(void))
{
	cop_add(C_HAL_READY, 0);
	rec_hal_ready = hal_ready;
}

/*
 * The service predicate the shell hands the leg layer beside the readiness one
 * (call.h calls_line_admit_bind). The recorder keeps the function for the
 * reason it keeps the one above: the answer the leg layer reads has to follow
 * the service state of the line afterwards.
 */
static bool (*rec_line_admit)(unsigned int line);

void calls_line_admit_bind(bool (*line_admit)(unsigned int line))
{
	rec_line_admit = line_admit;
}

/*
 * The backend feature mask the shell hands the leg layer (call.h
 * calls_hal_features_set). The recorder keeps the value, because what this
 * binding has to prove is that the mask the backend reported is the one the
 * leg layer reads: a board without HAL_FEAT_T38 never offers an image stream,
 * and the only place that fact crosses is this call.
 */
static uint32_t rec_hal_features;

void calls_hal_features_set(uint32_t features)
{
	cop_add(C_HAL_FEATURES, 0)->val = features;
	rec_hal_features = features;
}

/*
 * The same mask handed to the account layer (account.h
 * accounts_hal_features_set), which reads one bit of it: a line registers over
 * IPv6 only on a backend that carries IPv6 media. account.c is not linked into
 * this suite, so the entry point is recorded here, the way test_account.c
 * records sip_line_mwi_push().
 */
static uint32_t rec_acct_features;

void accounts_hal_features_set(uint32_t features)
{
	rec_acct_features = features;
}

/*
 * The backend codec mask, pushed beside the feature mask at both edges (call.h
 * calls_hal_codecs_set). It takes no op of its own: the two pushes are
 * one event, and the ordering cases below count that event once.
 */
static uint32_t rec_hal_codecs;

void calls_hal_codecs_set(uint32_t codecs)
{
	rec_hal_codecs = codecs;
}

/* the host table the SIP init hands the leg layer (call.h calls_host_bind) */
static const struct proto_host *rec_calls_host;

void calls_host_bind(const struct proto_host *host)
{
	rec_calls_host = host;
}

void calls_done(void)
{
	cop_add(C_DONE, 0);
	calls_inited = false;
	calls_cfg_loaded = false;
}

void calls_line_teardown(unsigned int line)
{
	cop_add(C_TEARDOWN, line);
}

void calls_line_end(unsigned int line)
{
	cop_add(C_LINE_END, line);
}

/* the account half of the dial refusal (call.h calls_line_dial_ready); a case
 * clears it to stage a line that cannot carry a dial */
static bool dial_ready_answer = true;

bool calls_line_dial_ready(unsigned int line)
{
	(void)line;
	return dial_ready_answer;
}

bool call_slot_free(unsigned int line, unsigned int *slot)
{
	(void)line;
	*slot = 0;
	return true;
}

bool cfg_ui_desk(const struct cfg *cfg)
{
	(void)cfg;
	return false;
}

const char *const *cfg_ring_sound_names(void)
{
	static const char *const names[] = { NULL };

	return names;
}

int cfg_hal_range(const char *name, int *min, int *max)
{
	(void)name;
	(void)min;
	(void)max;
	return -ENOENT;
}

uint8_t calls_line_fax_kind(unsigned int line, bool *media_running)
{
	if (media_running)
		*media_running = false;

	return HAL_PASSTHROUGH_OFF;
}

unsigned int calls_opus_dtmf_fallbacks(void)
{
	return 0;
}

/*
 * The account layer behind the SIP ops table (sip/proto.c sip_proto_ops).
 * account.c is not linked into this suite, so the table's lifecycle calls are
 * counted here: the start, reload and stop order of proto/proto.c is what the
 * cases read.
 */
static struct {
	unsigned int init;
	unsigned int start;
	unsigned int reload;
	unsigned int done;
	unsigned int shutdown_begin;
	const struct proto_host *host;
} acct_rec;

void accounts_host_bind(const struct proto_host *host)
{
	acct_rec.host = host;
}

void accounts_init(void)
{
	acct_rec.init++;
}

void accounts_start(const struct cfg *cfg)
{
	acct_rec.start++;
}

void accounts_reload(const struct cfg *old_cfg, const struct cfg *new_cfg)
{
	acct_rec.reload++;
}

void accounts_done(void)
{
	acct_rec.done++;
}

void accounts_shutdown_begin(void)
{
	acct_rec.shutdown_begin++;
}

bool accounts_shutdown_pending(void)
{
	return false;
}

void accounts_status_dump(struct blob_buf *b, int want_line)
{
	void *arr = blobmsg_open_array(b, "lines");

	blobmsg_close_array(b, arr);
}

static bool acct_cmd_answer;
static unsigned int request_replies;
static unsigned int request_line;
static char request_verb[16];

bool account_register_cmd(unsigned int line)
{
	return acct_cmd_answer;
}

bool account_unregister_cmd(unsigned int line)
{
	return acct_cmd_answer;
}

/* a pool that never opened, so the wire counters of sip/proto.c read zero */
const struct sip_transport *acct_transport(void)
{
	static const struct sip_transport none;

	return &none;
}

/*
 * A NOTIFY the account layer took (account.c account_mwi_recv): the core store
 * behind host->mwi_set, then the push into the SIP machine.
 */
static void mwi_notify(unsigned int line, bool on, uint32_t new_msgs,
		       uint32_t old_msgs)
{
	line_mwi_set(line, on, new_msgs, old_msgs);
	sip_line_mwi_push(line);
}

/*
 * The core services proto/proto.c fills the host table from. The suite drives
 * the line shell, so every one of them is inert here.
 */
void event_reg_post(unsigned int line, bool up, const char *proxy)
{
}

void event_tls_cert_start(const char *url, const char *username,
			  const char *password, bool verify, void (*done)(void))
{
}

void event_tls_cert_stop(enum event_tls_stop_mode mode)
{
}

void net_iface_subscribe(const struct net_iface_sub *sub)
{
}

const struct net_iface_state *net_iface_state_get(void)
{
	static const struct net_iface_state none;

	return &none;
}

const char *net_iface_bind_device(const char *iface)
{
	return NULL;
}

bool net_iface_source_host_probe(const char *dev, int family,
				 const struct sockaddr *sa, socklen_t salen,
				 char *out, size_t olen)
{
	return false;
}

void net_iface_ipv6_served_set(bool served)
{
}

void net_iface_recover_settle(void)
{
}

void fw_reconcile_ask(const struct fw_sig_ask *ask)
{
}

void fw_reconcile_clear(void)
{
}

unsigned int fw_reconcile_proxy_add(char (*list)[FW_SRC_MAX],
				    unsigned int count, const char *addr)
{
	return count;
}

void fw_shutdown(void)
{
}

struct ubus_context *uvoice_ubus_ctx(void)
{
	return NULL;
}

void uvoice_ubus_notify(const char *type, struct blob_attr *msg)
{
	(void)type;
	(void)msg;
}

int ubus_send_reply(struct ubus_context *ctx, struct ubus_request_data *req,
		    struct blob_attr *msg)
{
	(void)ctx;
	(void)req;
	(void)msg;
	return 0;
}

void uvoice_ubus_request_reply(struct ubus_context *ctx,
			       struct ubus_request_data *req, unsigned int line,
			       const char *verb)
{
	request_replies++;
	request_line = line;
	snprintf(request_verb, sizeof(request_verb), "%s", verb ? verb : "");
}

/* set by the FIFO case: makes the next dial wake the descriptor below */
static int notify_fd[2] = { -1, -1 };
static bool notify_armed;

static void busy_wait_ms(unsigned int ms)
{
	struct timespec start, now;
	uint64_t elapsed;

	clock_gettime(CLOCK_MONOTONIC, &start);
	do {
		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (uint64_t)(now.tv_sec - start.tv_sec) * 1000 +
			  (uint64_t)(now.tv_nsec / 1000000) -
			  (uint64_t)(start.tv_nsec / 1000000);
	} while (elapsed < ms);
}

bool call_dial(unsigned int line, unsigned int slot, const char *number, bool cid_present)
{
	struct cop *c;
	unsigned int i;

	/* As in call.c, a dial that reaches the leg layer before it is
	 * wired, or before it holds a config, is refused and leaves no leg.
	 * Nothing is recorded, because nothing went out. */
	if (!calls_inited || !calls_cfg_loaded)
		return false;

	c = cop_add(C_DIAL, line);
	c->slot = slot;
	c->cid_present = cid_present;
	snprintf(c->num, sizeof(c->num), "%s", number ? number : "");

	/*
	 * The real dial builds SDP, resolves and sends, so the queueing of its
	 * failure lands well after the uloop pass sampled its clock, and the
	 * 0 ms drain timer then belongs to the next pass. The spin reproduces
	 * that without the SIP stack; without it the drain runs inside the
	 * same timeout sweep and the overtake window never opens.
	 */
	if (notify_armed)
		busy_wait_ms(2);

	if (dial_fail_times) {
		/* call_dial may report the failure inside its own frame. The
		 * repeat count drives the deferred ring overflow case. */
		for (i = 0; i < dial_fail_times; i++)
			rec_cb.failed(line, slot, CALL_FAIL_CONGESTION);
	}

	/* Make the descriptor readable while the failure sits in the queue.
	 * uloop dispatches descriptors after the timeout sweep of the same
	 * pass, so the callback lands in the window this dial just opened. */
	if (notify_armed) {
		notify_armed = false;
		if (write(notify_fd[1], "x", 1) != 1)
			printf("    notify write failed\n");
	}

	return dial_fail_times == 0;
}

/*
 * A leg layer that cannot build the 180. call.c answers that INVITE 500 itself
 * and fails the leg, so the pair this fake reproduces is the contract: a false
 * answer plus one failure report. The report lands inside the shell's own ring
 * frame, which is where the deferred ring holds it until the frame closes.
 */
static bool call_ring_refuse;

bool call_ring(unsigned int line, unsigned int slot)
{
	cop_add(C_RING, line)->slot = slot;

	if (!call_ring_refuse)
		return true;

	rec_cb.failed(line, slot, CALL_FAIL_CONGESTION);

	return false;
}

bool call_answer(unsigned int line, unsigned int slot)
{
	cop_add(C_ANSWER, line)->slot = slot;

	return true;
}

bool call_reject(unsigned int line, unsigned int slot, uint32_t code)
{
	struct cop *c = cop_add(C_REJECT, line);

	c->slot = slot;
	c->val = code;

	return true;
}

bool call_hangup(unsigned int line, unsigned int slot)
{
	cop_add(C_HANGUP, line)->slot = slot;

	return true;
}

/* a leg layer that refuses the re-INVITE, so no callback will ever arrive */
static bool hold_refuse;
static bool resume_refuse;

bool call_hold(unsigned int line, unsigned int slot)
{
	cop_add(C_HOLD, line)->slot = slot;

	return !hold_refuse;
}

bool call_resume(unsigned int line, unsigned int slot)
{
	cop_add(C_RESUME, line)->slot = slot;

	return !resume_refuse;
}

bool call_dtmf(unsigned int line, unsigned int slot, char digit,
	       uint16_t duration_ms, bool measured)
{
	struct cop *c = cop_add(C_DTMF, line);

	c->slot = slot;
	c->val = (uint32_t)(unsigned char)digit;
	c->duration_ms = duration_ms;
	c->measured = measured;

	return true;
}

/* a leg layer with no offered call to redirect */
static bool forward_refuse;

bool call_forward(unsigned int line, unsigned int slot, const char *target)
{
	struct cop *c = cop_add(C_FORWARD, line);

	c->slot = slot;
	snprintf(c->num, sizeof(c->num), "%s", target ? target : "");

	return !forward_refuse;
}

/* a leg layer with no confirmed dialog to relay the flash on */
static bool flash_relay_refuse;

bool call_flash_relay(unsigned int line, unsigned int slot)
{
	cop_add(C_FLASH_RELAY, line)->slot = slot;

	return !flash_relay_refuse;
}

/* a leg layer that cannot take the fax switch, which the shell only traces */
static bool fax_start_refuse;

bool call_fax_start(unsigned int line, unsigned int slot)
{
	cop_add(C_FAX, line)->slot = slot;

	return !fax_start_refuse;
}

/*
 * The tone half of the far-end upspeed recognizer (call.h call_fax_hint). It
 * has no outcome at all, so the recorder is the whole of what a case can read:
 * the shell owes the leg one record per remote answer tone on a connected line
 * and none anywhere else.
 */
void call_fax_hint(unsigned int line, unsigned int slot)
{
	cop_add(C_FAX_HINT, line)->slot = slot;
}

static bool fax_on_answer;

bool call_fax_on(unsigned int line, unsigned int slot)
{
	cop_add(C_FAX_ON, line)->slot = slot;

	return fax_on_answer;
}

static bool fax_kind_refuse;
static uint8_t fax_kind_answer;

uint8_t call_fax_kind_get(unsigned int line, unsigned int slot)
{
	cop_add(C_FAX_KIND_GET, line)->slot = slot;

	return fax_kind_answer;
}

bool call_fax_kind_set(unsigned int line, unsigned int slot, uint8_t kind)
{
	struct cop *c = cop_add(C_FAX_KIND, line);

	c->slot = slot;
	c->val = kind;
	if (!fax_kind_refuse)
		fax_kind_answer = kind;

	return !fax_kind_refuse;
}

/*
 * The return-to-voice rule of the leg layer (call.h call_fax_tone_note,
 * call_fax_exit). The shell owns the deferral and the leg owns the rule, so
 * the recorder answers what a case scripts and keeps every argument: the two
 * halves meet on the tone row and on the handle alone.
 */
static enum call_fax_tone fax_tone_answer;
static bool fax_exit_answer;
/*
 * The generation the fake leg layer stamps into a handle. A case bumps it to
 * model the release call.c makes, so a deferral that named the leg before the
 * bump can be told from one that named the leg after it.
 */
static uint16_t leg_gen_answer = 1;

/* the handle shape of call.c leg_handle(): the physical leg index over the
 * generation, so one number names the line, the slot and the call */
static uint32_t leg_handle_of(unsigned int line, unsigned int slot,
			      uint16_t gen)
{
	return ((line * CALL_SLOTS + slot) << 16) | gen;
}

enum call_fax_tone call_fax_tone_note(unsigned int line, unsigned int slot,
				      uint16_t code, uint8_t direction,
				      bool end, uint16_t duration_ms)
{
	struct cop *c = cop_add(C_FAX_TONE_NOTE, line);

	c->slot = slot;
	c->val = code;
	c->dir = direction;
	c->end = end;
	c->duration_ms = duration_ms;

	return end ? fax_tone_answer : CALL_FAX_TONE_NOTED;
}

uint32_t call_leg_handle_get(unsigned int line, unsigned int ui_slot)
{
	struct cop *c = cop_add(C_LEG_HANDLE, line);
	uint32_t handle = leg_handle_of(line, ui_slot, leg_gen_answer);

	c->slot = ui_slot;
	c->val = handle;

	return handle;
}

bool call_fax_exit(uint32_t handle)
{
	unsigned int index = handle >> 16;
	struct cop *c = cop_add(C_FAX_EXIT, index / CALL_SLOTS);

	c->slot = index % CALL_SLOTS;
	c->val = handle;

	return fax_exit_answer;
}

/*
 * What the leg layer answers one upgrade attempt with (call.h). CALL_T38_STARTED
 * is 0, so the fixture reset arms the answer every case takes unless it names
 * another. CALL_T38_REFUSED stands for every refusal the leg makes for good: a
 * channel or backend without T.38, the daemon's one image session already
 * taken, a session that is already an image one. CALL_T38_BUSY is the exchange
 * of the leg's own, which the next preamble of the burst finds gone.
 */
static enum call_t38_answer t38_start_answer;

enum call_t38_answer call_t38_start(unsigned int line, unsigned int slot)
{
	cop_add(C_T38, line)->slot = slot;

	return t38_start_answer;
}

/*
 * The two transfer entries (call.h). The recorder keeps the kind and the
 * target, because the map from the star-code row onto the call.c vocabulary is
 * what this shell owns and nothing below it can be asked about.
 *
 * The two refusal flags stand for every synchronous refusal call.h names (an
 * early dialog, a leg that already carries a REFER, an empty target): they are
 * one answer for the caller, no wire exchange happened and no callback follows,
 * so the shell owes the machine the failure itself and, for the consult form,
 * the cleanup of both legs.
 */
static bool xfer_refuse;

bool call_xfer(unsigned int line, unsigned int slot, enum call_xfer_kind kind,
	       const char *target)
{
	struct cop *c = cop_add(C_XFER, line);

	c->slot = slot;
	c->val = (uint32_t)kind;
	snprintf(c->num, sizeof(c->num), "%s", target ? target : "");

	return !xfer_refuse;
}

static bool xfer_consult_refuse;

bool call_xfer_consult(unsigned int line, unsigned int referee, unsigned int replaced)
{
	struct cop *c = cop_add(C_XFER_CONSULT, line);

	c->slot = referee;
	c->val = replaced;

	return !xfer_consult_refuse;
}

/*
 * The UI-to-physical lookup (call.h call_slot_stream). The stub is the identity
 * until a case moves it, which is what every leg of an untransferred line
 * answers, so a case that never heard of a transfer drives the shell exactly as
 * the daemon does. xfer_stream_of[] is the swap: a transferee's child sits on
 * the other physical slot and wears the name of the leg it replaced.
 */
static unsigned int xfer_stream_of[CALL_SLOTS];
static bool xfer_stream_set;

static void xfer_stream_map(unsigned int ui_slot, unsigned int stream)
{
	unsigned int i;

	if (!xfer_stream_set) {
		for (i = 0; i < CALL_SLOTS; i++)
			xfer_stream_of[i] = i;
		xfer_stream_set = true;
	}

	xfer_stream_of[ui_slot] = stream;
}

unsigned int call_slot_stream(unsigned int line, unsigned int ui_slot)
{
	if (line >= CFG_CHANNEL_MAX || ui_slot >= CALL_SLOTS)
		return CALL_SLOTS;

	return xfer_stream_set ? xfer_stream_of[ui_slot] : ui_slot;
}

void calls_status_dump(struct blob_buf *b, unsigned int line)
{
	void *arr = blobmsg_open_array(b, "calls");

	blobmsg_close_array(b, arr);
}

void call_codec_dump(struct blob_buf *b, unsigned int line,
		     unsigned int ui_slot)
{
}

/* ------------------------------------------------------------- status view */

struct line_view {
	char state[32];
	char clearing[32];
	char digits[FSM_DIGITS_MAX + 1];
	unsigned int line;
	unsigned int deferred;
	unsigned int deferred_lost;
	bool oos;
	bool fault;
	bool fault_set;
	bool ringing;
	bool ring_held;
	bool call_waiting;
	bool cid_once;
	bool cid_once_set;
	bool cid_restrict;
	bool cid_restrict_set;
	bool mwi;
	unsigned int mwi_new;
	unsigned int mwi_old;
	bool dnd;
	char cf_uncond[64];
	char cf_busy[64];
	char cf_no_answer[64];
	bool cf_uncond_set;
	bool cf_busy_set;
	bool cf_no_answer_set;
	bool conf;
	bool fwd_disc_reversed;
	bool fwd_disc_armed;
	bool fwd_disc_down;
	/* the two fax latches, each an array indexed by slot and present only
	 * while this line holds one of them */
	bool fax_switched_set;
	bool t38_switched_set;
	bool fax_switched[CALL_SLOTS];
	bool t38_switched[CALL_SLOTS];
	/* the loop test table, present only while the mode holds this port */
	bool loop_test_set;
	char loop_test_mode[16];
	bool loop_test_offhook;
	bool loop_test_ringing;
	bool loop_test_audio;
	bool found;
	bool has_calls;
};

enum {
	LV_LINE,
	LV_STATE,
	LV_CLEARING,
	LV_OOS,
	LV_FAULT,
	LV_RINGING,
	LV_RING_HELD,
	LV_CW,
	LV_CID_ONCE,
	LV_CID_RESTRICT,
	LV_DIGITS,
	LV_DEFERRED,
	LV_LOST,
	LV_MWI,
	LV_MWI_NEW,
	LV_MWI_OLD,
	LV_DND,
	LV_CF_UNCOND,
	LV_CF_BUSY,
	LV_CF_NO_ANSWER,
	LV_CONF,
	LV_FWD_DISC_REV,
	LV_FWD_DISC_ARMED,
	LV_FWD_DISC_DOWN,
	LV_FAX_SWITCHED,
	LV_T38_SWITCHED,
	LV_LOOP_TEST,
	LV_CALLS,
	__LV_MAX,
};

static const struct blobmsg_policy lv_policy[__LV_MAX] = {
	[LV_LINE] = { "line", BLOBMSG_TYPE_INT32 },
	[LV_STATE] = { "state", BLOBMSG_TYPE_STRING },
	[LV_CLEARING] = { "clearing", BLOBMSG_TYPE_STRING },
	[LV_OOS] = { "oos", BLOBMSG_TYPE_BOOL },
	[LV_FAULT] = { "fault", BLOBMSG_TYPE_BOOL },
	[LV_RINGING] = { "ringing", BLOBMSG_TYPE_BOOL },
	[LV_RING_HELD] = { "ring_held", BLOBMSG_TYPE_BOOL },
	[LV_CW] = { "call_waiting", BLOBMSG_TYPE_BOOL },
	[LV_CID_ONCE] = { "cid_once", BLOBMSG_TYPE_BOOL },
	[LV_CID_RESTRICT] = { "cid_restrict", BLOBMSG_TYPE_BOOL },
	[LV_DIGITS] = { "digits", BLOBMSG_TYPE_STRING },
	[LV_DEFERRED] = { "deferred", BLOBMSG_TYPE_INT32 },
	[LV_LOST] = { "deferred_lost", BLOBMSG_TYPE_INT32 },
	[LV_MWI] = { "mwi", BLOBMSG_TYPE_BOOL },
	[LV_MWI_NEW] = { "mwi_new", BLOBMSG_TYPE_INT32 },
	[LV_MWI_OLD] = { "mwi_old", BLOBMSG_TYPE_INT32 },
	[LV_DND] = { "dnd", BLOBMSG_TYPE_BOOL },
	[LV_CF_UNCOND] = { "cf_uncond", BLOBMSG_TYPE_STRING },
	[LV_CF_BUSY] = { "cf_busy", BLOBMSG_TYPE_STRING },
	[LV_CF_NO_ANSWER] = { "cf_no_answer", BLOBMSG_TYPE_STRING },
	[LV_CONF] = { "conf", BLOBMSG_TYPE_BOOL },
	[LV_FWD_DISC_REV] = { "fwd_disc_reversed", BLOBMSG_TYPE_BOOL },
	[LV_FWD_DISC_ARMED] = { "fwd_disc_armed", BLOBMSG_TYPE_BOOL },
	[LV_FWD_DISC_DOWN] = { "fwd_disc_down", BLOBMSG_TYPE_BOOL },
	[LV_FAX_SWITCHED] = { "fax_switched", BLOBMSG_TYPE_ARRAY },
	[LV_T38_SWITCHED] = { "t38_switched", BLOBMSG_TYPE_ARRAY },
	[LV_LOOP_TEST] = { "loop_test", BLOBMSG_TYPE_TABLE },
	[LV_CALLS] = { "calls", BLOBMSG_TYPE_ARRAY },
};

enum {
	LT_MODE,
	LT_OFFHOOK,
	LT_RINGING,
	LT_AUDIO,
	__LT_MAX,
};

static const struct blobmsg_policy lt_policy[__LT_MAX] = {
	[LT_MODE] = { "mode", BLOBMSG_TYPE_STRING },
	[LT_OFFHOOK] = { "offhook", BLOBMSG_TYPE_BOOL },
	[LT_RINGING] = { "ringing", BLOBMSG_TYPE_BOOL },
	[LT_AUDIO] = { "audio", BLOBMSG_TYPE_BOOL },
};

static struct blob_buf sb;
static unsigned int dump_lines;

/* one latch array, slot by slot; an absent key leaves every slot clear */
static void latch_take(struct blob_attr *arr, bool *out)
{
	struct blob_attr *cur;
	unsigned int i = 0, rem;

	if (!arr)
		return;

	blobmsg_for_each_attr(cur, arr, rem) {
		if (i < CALL_SLOTS)
			out[i++] = blobmsg_get_bool(cur);
	}
}

/* the loop test table of one line, whose four keys are present together or
 * not at all */
static void loop_test_take(struct blob_attr *table, struct line_view *v)
{
	struct blob_attr *tb[__LT_MAX] = {};

	if (!table)
		return;

	blobmsg_parse(lt_policy, __LT_MAX, tb, blobmsg_data(table),
		      blobmsg_data_len(table));

	if (tb[LT_MODE])
		snprintf(v->loop_test_mode, sizeof(v->loop_test_mode), "%s",
			 blobmsg_get_string(tb[LT_MODE]));
	if (tb[LT_OFFHOOK])
		v->loop_test_offhook = blobmsg_get_bool(tb[LT_OFFHOOK]);
	if (tb[LT_RINGING])
		v->loop_test_ringing = blobmsg_get_bool(tb[LT_RINGING]);
	if (tb[LT_AUDIO])
		v->loop_test_audio = blobmsg_get_bool(tb[LT_AUDIO]);
}

static void view_take(struct blob_attr *entry, struct line_view *v)
{
	struct blob_attr *tb[__LV_MAX] = {};

	blobmsg_parse(lv_policy, __LV_MAX, tb, blobmsg_data(entry), blobmsg_data_len(entry));

	memset(v, 0, sizeof(*v));
	v->found = true;
	if (tb[LV_LINE])
		v->line = blobmsg_get_u32(tb[LV_LINE]);
	if (tb[LV_STATE])
		snprintf(v->state, sizeof(v->state), "%s", blobmsg_get_string(tb[LV_STATE]));
	if (tb[LV_CLEARING])
		snprintf(v->clearing, sizeof(v->clearing), "%s",
			 blobmsg_get_string(tb[LV_CLEARING]));
	if (tb[LV_OOS])
		v->oos = blobmsg_get_bool(tb[LV_OOS]);
	v->fault_set = tb[LV_FAULT] != NULL;
	if (tb[LV_FAULT])
		v->fault = blobmsg_get_bool(tb[LV_FAULT]);
	if (tb[LV_RINGING])
		v->ringing = blobmsg_get_bool(tb[LV_RINGING]);
	v->ring_held = tb[LV_RING_HELD] != NULL;
	if (tb[LV_CW])
		v->call_waiting = blobmsg_get_bool(tb[LV_CW]);
	v->cid_once_set = tb[LV_CID_ONCE] != NULL;
	if (tb[LV_CID_ONCE])
		v->cid_once = blobmsg_get_bool(tb[LV_CID_ONCE]);
	v->cid_restrict_set = tb[LV_CID_RESTRICT] != NULL;
	if (tb[LV_CID_RESTRICT])
		v->cid_restrict = blobmsg_get_bool(tb[LV_CID_RESTRICT]);
	if (tb[LV_DIGITS])
		snprintf(v->digits, sizeof(v->digits), "%s",
			 blobmsg_get_string(tb[LV_DIGITS]));
	if (tb[LV_DEFERRED])
		v->deferred = blobmsg_get_u32(tb[LV_DEFERRED]);
	if (tb[LV_LOST])
		v->deferred_lost = blobmsg_get_u32(tb[LV_LOST]);
	if (tb[LV_MWI])
		v->mwi = blobmsg_get_bool(tb[LV_MWI]);
	if (tb[LV_MWI_NEW])
		v->mwi_new = blobmsg_get_u32(tb[LV_MWI_NEW]);
	if (tb[LV_MWI_OLD])
		v->mwi_old = blobmsg_get_u32(tb[LV_MWI_OLD]);
	if (tb[LV_DND])
		v->dnd = blobmsg_get_bool(tb[LV_DND]);
	/* a forward key is present only while its condition is armed */
	v->cf_uncond_set = tb[LV_CF_UNCOND] != NULL;
	if (tb[LV_CF_UNCOND])
		snprintf(v->cf_uncond, sizeof(v->cf_uncond), "%s",
			 blobmsg_get_string(tb[LV_CF_UNCOND]));
	v->cf_busy_set = tb[LV_CF_BUSY] != NULL;
	if (tb[LV_CF_BUSY])
		snprintf(v->cf_busy, sizeof(v->cf_busy), "%s",
			 blobmsg_get_string(tb[LV_CF_BUSY]));
	v->cf_no_answer_set = tb[LV_CF_NO_ANSWER] != NULL;
	if (tb[LV_CF_NO_ANSWER])
		snprintf(v->cf_no_answer, sizeof(v->cf_no_answer), "%s",
			 blobmsg_get_string(tb[LV_CF_NO_ANSWER]));
	/* the conference key is present only while the mixer runs */
	v->conf = tb[LV_CONF] != NULL;
	/* every forward disconnect key is present only while true */
	v->fwd_disc_reversed = tb[LV_FWD_DISC_REV] != NULL;
	v->fwd_disc_armed = tb[LV_FWD_DISC_ARMED] != NULL;
	v->fwd_disc_down = tb[LV_FWD_DISC_DOWN] != NULL;
	v->fax_switched_set = tb[LV_FAX_SWITCHED] != NULL;
	v->t38_switched_set = tb[LV_T38_SWITCHED] != NULL;
	latch_take(tb[LV_FAX_SWITCHED], v->fax_switched);
	latch_take(tb[LV_T38_SWITCHED], v->t38_switched);
	v->loop_test_set = tb[LV_LOOP_TEST] != NULL;
	loop_test_take(tb[LV_LOOP_TEST], v);
	v->has_calls = tb[LV_CALLS] != NULL;
}

static bool view_get(unsigned int line, struct line_view *v)
{
	struct blob_attr *cur, *entry;
	unsigned int rem, rem2;

	memset(v, 0, sizeof(*v));
	dump_lines = 0;

	blob_buf_init(&sb, 0);
	lines_status_dump(&sb);

	blob_for_each_attr(cur, sb.head, rem) {
		if (strcmp(blobmsg_name(cur), "call"))
			continue;

		blobmsg_for_each_attr(entry, cur, rem2) {
			struct line_view tmp;

			dump_lines++;
			view_take(entry, &tmp);
			if (tmp.line != line)
				continue;

			*v = tmp;
		}
	}

	return v->found;
}

static const char *state_of(unsigned int line)
{
	static struct line_view v;

	if (!view_get(line, &v))
		return "?";

	return v.state;
}

enum {
	RV_REQUESTED,
	RV_APPLIED,
	RV_SOURCE,
	RV_ISO3,
	RV_DESCRIBE,
	RV_CID_FORMAT,
	RV_MWI_FORMAT,
	RV_FLASH_MIN,
	RV_FLASH_MAX,
	RV_RELEASE_MIN,
	RV_CW_TOTAL,
	RV_FLASH_SERVICE,
	RV_PULSE_INVERT,
	RV_CONFIRM_TOTAL,
	RV_CONFIRM_ENDLESS,
	RV_RING,
	__RV_MAX,
};

static const struct blobmsg_policy rv_policy[__RV_MAX] = {
	[RV_REQUESTED] = { "requested", BLOBMSG_TYPE_STRING },
	[RV_APPLIED] = { "applied", BLOBMSG_TYPE_STRING },
	[RV_SOURCE] = { "source", BLOBMSG_TYPE_STRING },
	[RV_ISO3] = { "iso3", BLOBMSG_TYPE_STRING },
	[RV_DESCRIBE] = { "describe", BLOBMSG_TYPE_STRING },
	[RV_CID_FORMAT] = { "cid_format", BLOBMSG_TYPE_STRING },
	[RV_MWI_FORMAT] = { "mwi_format", BLOBMSG_TYPE_STRING },
	[RV_FLASH_MIN] = { "hook_flash_min_ms", BLOBMSG_TYPE_INT32 },
	[RV_FLASH_MAX] = { "hook_flash_max_ms", BLOBMSG_TYPE_INT32 },
	[RV_RELEASE_MIN] = { "hook_release_min_ms", BLOBMSG_TYPE_INT32 },
	[RV_CW_TOTAL] = { "cw_sas_total_ms", BLOBMSG_TYPE_INT32 },
	[RV_FLASH_SERVICE] = { "flash_service", BLOBMSG_TYPE_STRING },
	[RV_PULSE_INVERT] = { "pulse_invert", BLOBMSG_TYPE_BOOL },
	[RV_CONFIRM_TOTAL] = { "confirm_total_ms", BLOBMSG_TYPE_INT32 },
	[RV_CONFIRM_ENDLESS] = { "confirm_endless", BLOBMSG_TYPE_BOOL },
	[RV_RING] = { "ring", BLOBMSG_TYPE_ARRAY },
};

struct region_view {
	bool found;
	char requested[16];
	char applied[16];
	char source[16];
	char iso3[16];
	char describe[64];
	char cid_format[16];
	char mwi_format[16];
	char flash_service[16];
	uint32_t flash_min_ms;
	uint32_t flash_max_ms;
	uint32_t release_min_ms;
	uint32_t cw_total_ms;
	bool pulse_invert_set;
	bool pulse_invert;
	bool confirm_total_set;
	uint32_t confirm_total_ms;
	bool confirm_endless_set;
	bool confirm_endless;
	unsigned int templates;
	unsigned int last_template;
	unsigned int last_pairs;
};

static void rv_string_take(struct blob_attr *attr, char *out, size_t len)
{
	snprintf(out, len, "%s", attr ? blobmsg_get_string(attr) : "?");
}

static void rv_ring_take(struct blob_attr *arr, struct region_view *v)
{
	struct blob_attr *entry, *cur;
	unsigned int rem, rem2;

	if (!arr)
		return;

	blobmsg_for_each_attr(entry, arr, rem) {
		v->templates++;
		v->last_pairs = 0;
		blobmsg_for_each_attr(cur, entry, rem2) {
			if (!strcmp(blobmsg_name(cur), "template"))
				v->last_template = blobmsg_get_u32(cur);
			else if (!strcmp(blobmsg_name(cur), "cadence"))
				v->last_pairs = blobmsg_check_array(cur, BLOBMSG_TYPE_TABLE);
		}
	}
}

static bool region_view_get(struct region_view *v)
{
	struct blob_attr *tb[__RV_MAX] = {};
	struct blob_attr *cur;
	unsigned int rem;

	memset(v, 0, sizeof(*v));
	blob_buf_init(&sb, 0);
	lines_region_status_dump(&sb);

	blob_for_each_attr(cur, sb.head, rem) {
		if (strcmp(blobmsg_name(cur), "region"))
			continue;

		v->found = true;
		blobmsg_parse(rv_policy, __RV_MAX, tb, blobmsg_data(cur),
			      blobmsg_data_len(cur));
	}

	if (!v->found)
		return false;

	rv_string_take(tb[RV_REQUESTED], v->requested, sizeof(v->requested));
	rv_string_take(tb[RV_APPLIED], v->applied, sizeof(v->applied));
	rv_string_take(tb[RV_SOURCE], v->source, sizeof(v->source));
	rv_string_take(tb[RV_ISO3], v->iso3, sizeof(v->iso3));
	rv_string_take(tb[RV_DESCRIBE], v->describe, sizeof(v->describe));
	rv_string_take(tb[RV_CID_FORMAT], v->cid_format, sizeof(v->cid_format));
	rv_string_take(tb[RV_MWI_FORMAT], v->mwi_format, sizeof(v->mwi_format));
	rv_string_take(tb[RV_FLASH_SERVICE], v->flash_service,
		       sizeof(v->flash_service));
	v->flash_min_ms = tb[RV_FLASH_MIN] ? blobmsg_get_u32(tb[RV_FLASH_MIN]) : 0;
	v->flash_max_ms = tb[RV_FLASH_MAX] ? blobmsg_get_u32(tb[RV_FLASH_MAX]) : 0;
	v->release_min_ms = tb[RV_RELEASE_MIN] ?
			    blobmsg_get_u32(tb[RV_RELEASE_MIN]) : 0;
	v->cw_total_ms = tb[RV_CW_TOTAL] ? blobmsg_get_u32(tb[RV_CW_TOTAL]) : 0;
	v->pulse_invert_set = tb[RV_PULSE_INVERT] != NULL;
	if (tb[RV_PULSE_INVERT])
		v->pulse_invert = blobmsg_get_bool(tb[RV_PULSE_INVERT]);
	v->confirm_total_set = tb[RV_CONFIRM_TOTAL] != NULL;
	if (tb[RV_CONFIRM_TOTAL])
		v->confirm_total_ms = blobmsg_get_u32(tb[RV_CONFIRM_TOTAL]);
	v->confirm_endless_set = tb[RV_CONFIRM_ENDLESS] != NULL;
	if (tb[RV_CONFIRM_ENDLESS])
		v->confirm_endless = blobmsg_get_bool(tb[RV_CONFIRM_ENDLESS]);
	rv_ring_take(tb[RV_RING], v);

	return true;
}

static bool state_is(unsigned int line, const char *name)
{
	return strcmp(state_of(line), name) == 0;
}

/* --------------------------------------------------------------- the clock */

/*
 * The type 2 record of a call waiting alert waits out the 300 ms SAS and the
 * 50 ms gap the shell plays in front of it, so a case that wants the record
 * has to let that timer run. The two windows straddle the 350 ms, which is how
 * this suite pins a timer value (see "The clock is real" above): nothing may
 * leave inside the first, everything must have left by the end of the second.
 *
 * The early window is the whole SAS and not a part of it, so a deferral that
 * loses the gap and fires at 300 ms fails the case instead of passing it. The
 * margins either side of the deadline are 50 ms and 80 ms. A pump only ever
 * overruns, which widens the late margin and can only narrow the early one if
 * the scheduler stalls this process for 50 ms.
 */
#define CW_CID_EARLY_MS		300
#define CW_CID_LATE_MS		130

static void cw_cid_pump(void)
{
	uloop_pump(CW_CID_EARLY_MS + CW_CID_LATE_MS);
}

/*
 * The playout values of the digit the far end sent, restated here because the
 * constants are file static in line/line.c. The timed cases straddle the
 * deadline the way the call waiting record cases above straddle theirs: nothing
 * may leave inside the early window and everything must have left by the end of
 * the late one.
 */
#define DIGIT_PLAY_MIN_MS	100
#define DIGIT_PLAY_MAX_MS	500
#define DIGIT_PLAY_GAP_MS	70
#define DIGIT_PLAY_TAIL_MS	100
#define DIGIT_Q_MAX		16
#define DIGIT_DEDUP_MS		80

/* short of the 170 ms step of a shortest digit, and past it */
#define DIGIT_STEP_EARLY_MS	100
#define DIGIT_STEP_LATE_MS	150

/*
 * LINE_DIAL_TONE_DEFER_MS plus a margin, for a case that waits for a dial
 * class tone or checks that none leaves. The window covers the defer of the
 * non-stub build. The stub build compiles the defer to 0, so a tone leaves
 * inside the event and one loop pass proves its arrival or its absence.
 */
#ifdef UVOICE_HAL_STUB
#define DIAL_TONE_PUMP_MS	50
#else
#define DIAL_TONE_PUMP_MS	700
#endif

/* ------------------------------------------------------------- the fixture */

static struct cfg cfg;
static bool lines_live;

static void cfg_channel_default(struct cfg_channel *ch, unsigned int id, bool enabled)
{
	memset(ch, 0, sizeof(*ch));
	ch->present = true;
	ch->enabled = enabled;
	ch->id = id;
	snprintf(ch->digit_map, sizeof(ch->digit_map), "%s", DIALPLAN_DEFAULT_MAP);
	ch->digit_esc = 1;
	ch->start_digit_s = 16;
	ch->interdigit_s = 4;
	ch->interdigit_long_s = 16;
	ch->roh_s = 60;
	ch->busy_tone_s = 40;
	ch->flash_svc_s = 10;
	ch->call_waiting_enabled = true;
	/* the schema defaults of the two CID bools: this line announces its
	 * callers and presents its own number */
	ch->cid_remote_enable = true;
	ch->cid_outgoing_enable = true;
	/* the schema defaults of the two message waiting gates: this line both
	 * plays the stutter dial tone and sends the FSK record */
	ch->amwi = true;
	ch->vmwi = true;
	/* the schema default of lec_enable is 1. lec_control and lec_nlp carry
	 * no default at all, so the memset above is their loaded state and the
	 * daemon policy is what the line pushes. */
	ch->lec_enable = true;
}

static void cfg_default(void)
{
	memset(&cfg, 0, sizeof(cfg));
	cfg.loaded = true;
	cfg.enabled = true;
	cfg.channels = 2;
	snprintf(cfg.region, sizeof(cfg.region), "US");	/* the schema default */
	cfg_channel_default(&cfg.channel[0], 0, true);
	cfg_channel_default(&cfg.channel[1], 1, false);
	/* the country data is config too: the shape the backend answers when
	 * the region job the fixture submits completes */
	fake_region_default();
}

/* The stop of main.c around the lines: the protocol drain, the lines, and the
 * protocol's own end. */
static void lines_stop(void)
{
	if (!lines_live)
		return;

	proto_stop_wait();
	lines_done();
	proto_done();
	lines_live = false;
}

/*
 * The protocol the config names, over a host table built against the backend
 * of the moment, the order of main.c: the table goes in front of lines_init(),
 * which hands it to the protocol.
 */
static void proto_bind(void)
{
	proto_host_build();
	proto_select_from_cfg(&cfg);
}

/*
 * The whole bring-up, both halves (line/line.h). The daemon runs the early half
 * at start-up and the DSP half on the bring-up completion, and every case that
 * is not about the split wants the settled line the pair leaves behind. The two
 * cases that are about it call the halves themselves.
 */
static void lines_bring_up(void)
{
	proto_bind();
	lines_init(&cfg);
	proto_start(&cfg);
	lines_start(&cfg);
	lines_live = true;
}

/*
 * What the fake backend reports beside the polarity bit every fixture carries.
 * A case sets it before fixture_start(), because lines_init() reads the
 * capability once and hands the mask down to the leg layer, and it clears it
 * again when its block ends.
 */
static uint32_t fake_features_extra;

static void fixture_start(void)
{
	lines_stop();
	uv.hal = &fake_hal;
	uv.cfg = &cfg;
	uv.hal_state = UVOICE_HAL_READY;
	fake_cap.lines = 2;
	/* the loop bit rides with the polarity one: the loop test reads it
	 * before it arms, and the case that drives a backend without it takes
	 * it away after this fixture built the board */
	fake_cap.features = HAL_FEAT_POLARITY | HAL_FEAT_LOOP |
			    fake_features_extra;
	region_seen[0] = 0;
	region_calls = 0;
	region_status = HAL_OK;
	region_get_calls = 0;
	job_pending = NULL;
	dial_fail_times = 0;
	dial_ready_answer = true;
	chan_cfg_transient = 0;
	chan_cfg_fatal = false;
	tone_unobtainable_ok = false;
	digit_play_status = HAL_OK;
	hook_ts_status = HAL_OK;
	hook_ts_transient = 0;
	memset(hook_ts, 0, sizeof(hook_ts));
	memset(hook_ts_calls, 0, sizeof(hook_ts_calls));
	memset(hook_query_status, 0, sizeof(hook_query_status));
	memset(hook_query_transient, 0, sizeof(hook_query_transient));
	memset(boot_offhook, 0, sizeof(boot_offhook));
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	call_ring_refuse = false;
	hold_refuse = false;
	resume_refuse = false;
	flash_relay_refuse = false;
	fax_start_refuse = false;
	fax_kind_refuse = false;
	fax_on_answer = false;
	fax_kind_answer = HAL_PASSTHROUGH_OFF;
	fax_tone_answer = CALL_FAX_TONE_END;
	fax_exit_answer = false;
	leg_gen_answer = 1;
	t38_start_answer = CALL_T38_STARTED;
	xfer_refuse = false;
	xfer_consult_refuse = false;
	conf_transient_n = 0;
	conf_unsupported = false;
	conf_refuse = false;
	/* the two names are one again: every case that never heard of a
	 * transfer drives the shell exactly as the daemon does */
	xfer_stream_set = false;
	rec_hal_features = 0;
	fax_apply_status = HAL_OK;
	fax_apply_transient = 0;
	polarity_status = HAL_OK;
	line_state_status = HAL_OK;
	line_state_fail_n = 0;
	stats_start_status = HAL_OK;
	stats_result_status = HAL_OK;
	strm_start_status = HAL_OK;
	stats_starts = 0;
	stats_seed(&fake_stats, 1000);
	line_test_start_status = HAL_OK;
	line_test_result_status = HAL_OK;
	line_test_record_len = 0;
	memset(line_test_record, 0, sizeof(line_test_record));
	ring_start_status = HAL_OK;
	ring_start_fail_n = 0;
	loop_set_status = HAL_OK;
	loop_set_fail_n = 0;
	hops_reset();
	cops_reset();
	lines_bring_up();
	hops_reset();
	cops_reset();
	memset(&trace_note, 0, sizeof(trace_note));
}

/*
 * The country tone table lands on a job completion, and no tone profile exists
 * until it does (line/line.c "Replay"): a line that never loaded one plays no
 * SAS, so the shell refuses a call waiting record that would reach the phone in
 * front of the beep. Every call waiting case therefore completes the job the
 * fixture submitted and runs against a normally loaded line. The two bring-up
 * cases of t_call_waiting are the exception: they complete nothing, or a
 * failure.
 */
static void region_ready(void)
{
	/* the table loads once per fixture, and the helpers below are called
	 * more than once inside one case */
	if (job_pending)
		job_complete(HAL_OK);

	hops_reset();
}

/*
 * An inbound call from a caller who withheld nothing, which is what almost
 * every case wants. The privacy cases call rec_cb.incoming themselves.
 */
static void inject_incoming(unsigned int line, unsigned int slot, const char *number,
			    const char *name, unsigned int ring_id)
{
	rec_cb.incoming(line, slot, number, name, false, false, ring_id);
}

static void inject_hook_coded(unsigned int line, uint8_t state, uint8_t pulses,
			      uint8_t pulse_invert)
{
	struct hal_event ev = { .type = HAL_EVENT_HOOK, .line = (uint16_t)line };

	ev.u.hook.state = state;
	ev.u.hook.pulses = pulses;
	ev.u.hook.pulse_invert = pulse_invert;
	line_hal_event(&ev);
}

static void inject_hook(unsigned int line, uint8_t state, uint8_t pulses)
{
	inject_hook_coded(line, state, pulses, 0);
}

/*
 * The stream travels because a digit from the wire is played on the leg the
 * record names, and the shell reads the UI slot back from that number
 * (line/line.c slot_ui_of).
 */
static void inject_digit_stream(unsigned int line, unsigned int stream,
				char digit, bool remote, uint16_t duration_ms)
{
	struct hal_event ev = { .type = HAL_EVENT_DIGIT, .line = (uint16_t)line };

	ev.stream = (uint16_t)stream;
	ev.u.digit.digit = digit;
	ev.u.digit.remote = remote ? 1 : 0;
	ev.u.digit.duration_ms = duration_ms;
	line_hal_event(&ev);
}

static void inject_digit_duration(unsigned int line, char digit, bool remote,
				  uint16_t duration_ms)
{
	inject_digit_stream(line, 0, digit, remote, duration_ms);
}

static void inject_digit(unsigned int line, char digit, bool remote)
{
	inject_digit_duration(line, digit, remote, 100);
}

static void inject_digits(unsigned int line, const char *s)
{
	while (*s)
		inject_digit(line, *s++, false);
}

static void inject_event(unsigned int line, uint16_t type)
{
	struct hal_event ev = { .type = type, .line = (uint16_t)line };

	line_hal_event(&ev);
}

/* The thermal fault bit of the SLIC, as udsp event.h names it
 * (UDSP_FAULT_SLIC_THERMAL). This suite links no library header, so the value
 * is spelled out; udsp_events.c pins it against the library enum. */
#define FAULT_THERMAL	(1u << 1)

/*
 * One fault record, with the word the backend reports. A code of 0 is the
 * clear and not a fault (hal.h u.fault.code). A case that wants a fault
 * therefore names a bit, and inject_event() above cannot serve for it.
 */
static void inject_fault(unsigned int line, uint32_t code)
{
	struct hal_event ev = {
		.type = HAL_EVENT_SLIC_FAULT,
		.line = (uint16_t)line,
	};

	ev.u.fault.code = code;
	line_hal_event(&ev);
}

/*
 * One line state report (hal.h u.line.state). HAL_LINE_DOWN says the backend
 * declared the port dead, and every other state says the port carries a feed
 * again. A record with no state set is therefore a dead line and not a null
 * report, which is why inject_event() above cannot serve for it either.
 */
static void inject_line_state(unsigned int line, uint8_t state)
{
	struct hal_event ev = {
		.type = HAL_EVENT_LINE,
		.line = (uint16_t)line,
	};

	ev.u.line.state = state;
	line_hal_event(&ev);
}

/* one T.38 stream report */
static void inject_t38(unsigned int line, unsigned int stream, uint8_t state,
		       uint8_t payload_type)
{
	struct hal_event ev = { .type = HAL_EVENT_T38, .line = (uint16_t)line };

	ev.stream = (uint16_t)stream;
	ev.u.t38.state = state;
	ev.u.t38.payload_type = payload_type;
	line_hal_event(&ev);
}

/*
 * The first packet of a stream (hal.h HAL_EVENT_RTP_FIRST_RX). The record
 * names a stream and nothing else, and inject_event() above sets no stream,
 * so it cannot serve for this one either.
 */
static void inject_rtp_first_rx(unsigned int line, unsigned int stream)
{
	struct hal_event ev = {
		.type = HAL_EVENT_RTP_FIRST_RX,
		.line = (uint16_t)line,
	};

	ev.stream = (uint16_t)stream;
	line_hal_event(&ev);
}

/*
 * One detected non-DTMF tone. end selects the edge (hal.h u.tone.end): 0 is
 * the start row every gate of the two stages reads, and 1 is the end row the
 * return-to-voice rule times. The duration is the detector's reading of the
 * tone that ended, so an end row carries the one the case names.
 */
static void inject_tone_ms(unsigned int line, uint16_t code, uint8_t direction,
			   uint8_t end, uint16_t duration_ms)
{
	struct hal_event ev = { .type = HAL_EVENT_TONE, .line = (uint16_t)line };

	ev.u.tone.code = code;
	ev.u.tone.direction = direction;
	ev.u.tone.end = end;
	ev.u.tone.duration_ms = duration_ms;
	line_hal_event(&ev);
}

static void inject_tone(unsigned int line, uint16_t code, uint8_t direction,
			uint8_t end)
{
	inject_tone_ms(line, code, direction, end, 450);
}

/* ------------------------------------------------------------------- cases */

static void t_hook_intake(void)
{
	struct line_view v;

	cfg_default();
	fixture_start();

	CASE("seize arms the digit detector with DTMF, PULSE and MODEM");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(hop_has(H_CHAN_CFG));
	CHECK_UINT(hop_find(H_CHAN_CFG)->chan.detect_mask,
		   HAL_DETECT_DTMF | HAL_DETECT_PULSE | HAL_DETECT_MODEM);

	CASE("seize enters OFFHOOK_DIAL");
	CHECK(state_is(0, "OFFHOOK_DIAL"));

	CASE("the detector is armed BEFORE any tone reaches the line");
	CHECK(hop_index(H_CHAN_CFG) == 0);
#ifdef UVOICE_HAL_STUB
	/* the stub defer is 0 ms, so the tone lands in the same event, still
	 * behind the detector */
	CHECK(hop_index(H_TONE) > 0);
#else
	CHECK_UINT(hop_count(H_TONE), 0);
#endif

	CASE("the dial tone is deferred, not dropped");
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);
	CHECK_UINT(hop_last(H_TONE)->line, 0);

	CASE("a release before the deferred tone cancels it");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	hops_reset();
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK_UINT(hop_count(H_TONE), 0);
	CHECK(state_is(0, "IDLE"));

	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(DIAL_TONE_PUMP_MS);

	CASE("tone_play carries duration 0, which means until stopped");
	CHECK_UINT(hop_last(H_TONE)->mask, 0);

	CASE("a digit collected off hook reaches the machine");
	hops_reset();
	inject_digit(0, '5', false);
	CHECK(state_is(0, "COLLECTING"));
	view_get(0, &v);
	CHECK(strcmp(v.digits, "5") == 0);

	/* the far end dials, this subscriber does not, and a line that still
	 * collects plays nothing either */
	CASE("a digit from the wire is never collected");
	hops_reset();
	inject_digit(0, '7', true);
	view_get(0, &v);
	CHECK(strcmp(v.digits, "5") == 0);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 0);

	CASE("release returns the line to IDLE");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));

	CASE("release stops the tone");
	CHECK(hop_count(H_TONE_STOP) >= 1);

	CASE("flash while idle changes nothing");
	hops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(hop_count(H_TONE), 0);

	CASE("flash off hook restarts the dial tone window");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	CHECK(state_is(0, "COLLECTING"));
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	view_get(0, &v);
	CHECK(strcmp(v.digits, "") == 0);

	CASE("an event for a line the config does not carry is dropped");
	inject_hook(7, HAL_HOOK_SEIZE, 0);
	CHECK(state_is(0, "OFFHOOK_DIAL"));

	CASE("a ring event from the backend reaches no machine");
	hops_reset();
	inject_event(0, HAL_EVENT_RING_END);
	inject_event(0, HAL_EVENT_RING_START);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK_UINT(nhops, 0);

	inject_hook(0, HAL_HOOK_RELEASE, 0);
}

static void t_pulse(void)
{
	struct line_view v;

	cfg_default();
	fixture_start();

	CASE("one pulse is the digit 1");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_hook(0, HAL_HOOK_PULSE, 1);
	view_get(0, &v);
	CHECK(strcmp(v.digits, "1") == 0);

	CASE("nine pulses are the digit 9");
	inject_hook(0, HAL_HOOK_PULSE, 9);
	view_get(0, &v);
	CHECK(strcmp(v.digits, "19") == 0);

	CASE("ten pulses are the digit 0");
	inject_hook(0, HAL_HOOK_PULSE, 10);
	view_get(0, &v);
	CHECK(strcmp(v.digits, "190") == 0);

	CASE("eleven pulses are ignored");
	inject_hook(0, HAL_HOOK_PULSE, 11);
	view_get(0, &v);
	CHECK(strcmp(v.digits, "190") == 0);

	CASE("a zero pulse count is ignored");
	inject_hook(0, HAL_HOOK_PULSE, 0);
	view_get(0, &v);
	CHECK(strcmp(v.digits, "190") == 0);

	CASE("an unknown hook state is ignored");
	inject_hook(0, HAL_HOOK_ERROR, 0);
	CHECK(state_is(0, "COLLECTING"));

	inject_hook(0, HAL_HOOK_RELEASE, 0);
}

/*
 * New Zealand codes a dialed N as 10 - N pulses and the digit 0 as 10. The
 * hook event carries the coding of the region that counted the train, and the
 * line maps the count with that coding. The read-back can already hold the
 * next region, because a region swap can complete before a queued train
 * arrives. Two cases therefore give the event and the read-back different
 * codings, and the event decides.
 */
static void t_pulse_invert(void)
{
	struct region_view rv;
	struct line_view v;

	cfg_default();
	fake_region.pulse_invert = 1;
	fixture_start();
	region_ready();

	CASE("an inverted train maps nine pulses to the digit 1");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_hook_coded(0, HAL_HOOK_PULSE, 9, 1);
	view_get(0, &v);
	CHECK(strcmp(v.digits, "1") == 0);

	CASE("an inverted train keeps five pulses as the digit 5");
	inject_hook_coded(0, HAL_HOOK_PULSE, 5, 1);
	view_get(0, &v);
	CHECK(strcmp(v.digits, "15") == 0);

	CASE("an inverted train maps ten pulses to the digit 0");
	inject_hook_coded(0, HAL_HOOK_PULSE, 10, 1);
	view_get(0, &v);
	CHECK(strcmp(v.digits, "150") == 0);

	CASE("an inverted train maps one pulse to the digit 9");
	inject_hook_coded(0, HAL_HOOK_PULSE, 1, 1);
	view_get(0, &v);
	CHECK(strcmp(v.digits, "1509") == 0);

	CASE("an inverted train still ignores eleven pulses");
	inject_hook_coded(0, HAL_HOOK_PULSE, 11, 1);
	view_get(0, &v);
	CHECK(strcmp(v.digits, "1509") == 0);

	CASE("the status dump carries the inversion of the read-back");
	CHECK(region_view_get(&rv));
	CHECK(rv.pulse_invert_set);
	CHECK(rv.pulse_invert);

	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("a direct train under an NZL read-back maps nine pulses to 9");
	cfg_default();
	fake_region.pulse_invert = 1;
	fixture_start();
	region_ready();
	CHECK(region_view_get(&rv));
	CHECK(rv.pulse_invert);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_hook_coded(0, HAL_HOOK_PULSE, 9, 0);
	view_get(0, &v);
	CHECK(strcmp(v.digits, "9") == 0);

	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("an inverted train under a USA read-back maps nine pulses to 1");
	cfg_default();
	fixture_start();
	region_ready();
	CHECK(region_view_get(&rv));
	CHECK(rv.pulse_invert_set);
	CHECK(!rv.pulse_invert);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_hook_coded(0, HAL_HOOK_PULSE, 9, 1);
	view_get(0, &v);
	CHECK(strcmp(v.digits, "1") == 0);

	inject_hook(0, HAL_HOOK_RELEASE, 0);
}

static void t_dialplan_cfg(void)
{
	struct line_view v;

	CASE("a compiled map dials as soon as it matches");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, "5") == 0);
	CHECK(state_is(0, "CALLING"));

	CASE("call_dial takes the slot the machine chose");
	CHECK_UINT(cop_find(C_DIAL)->slot, 0);
	CHECK_UINT(cop_find(C_DIAL)->line, 0);

	CASE("a refused map falls back to the compiled-in default");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "N5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	CHECK(!cop_has(C_DIAL));
	CHECK(state_is(0, "COLLECTING"));

	CASE("an empty map falls back too");
	cfg_default();
	cfg.channel[0].digit_map[0] = 0;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '1', false);
	CHECK(state_is(0, "COLLECTING"));

	CASE("a pound the map cannot take dials the buffer");
	cfg_default();
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '1', false);
	inject_digit(0, '2', false);
	inject_digit(0, '#', false);
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, "12") == 0);

	/* the collector never reads digit_esc_enabled: the option selects the
	 * URI escaping in call.c and the map decides what a '#' is */
	CASE("a pound the map admits is a plain digit at every escape value");
	for (unsigned int esc = 0; esc <= 2; esc++) {
		cfg_default();
		cfg.channel[0].digit_esc = esc;
		snprintf(cfg.channel[0].digit_map,
			 sizeof(cfg.channel[0].digit_map), "1#");
		fixture_start();
		inject_hook(0, HAL_HOOK_SEIZE, 0);
		inject_digit(0, '1', false);
		view_get(0, &v);
		CHECK(strcmp(v.digits, "1") == 0);
		inject_digit(0, '#', false);
		CHECK(cop_has(C_DIAL));
		CHECK(strcmp(cop_find(C_DIAL)->num, "1#") == 0);
	}

	CASE("a pound the map cannot take is the delimiter at every escape value");
	for (unsigned int esc = 0; esc <= 2; esc++) {
		cfg_default();
		cfg.channel[0].digit_esc = esc;
		fixture_start();
		inject_hook(0, HAL_HOOK_SEIZE, 0);
		inject_digit(0, '1', false);
		inject_digit(0, '#', false);
		CHECK(cop_has(C_DIAL));
		CHECK(strcmp(cop_find(C_DIAL)->num, "1") == 0);
		CHECK(state_is(0, "CALLING"));
	}

	/* the table holds no code, as on a unit that names none, so the star
	 * string reaches the map, and "*#" is no prefix of "*xx.T" */
	CASE("a star-pound string the table does not hold dials the star alone");
	cfg_default();
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '*', false);
	CHECK(state_is(0, "COLLECTING"));
	inject_digit(0, '#', false);
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, "*") == 0);
	CHECK(state_is(0, "CALLING"));

	CASE("a digit no map alternative accepts gives reorder");
	cfg_default();
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '1', false);
	inject_digit(0, '*', false);
	CHECK(!cop_has(C_DIAL));
	CHECK(state_is(0, "CLEARING"));
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);

	/* the loader warns once per section and names it (cfg/uci.c
	 * digit_map_check), so the derive stays silent on every line of every
	 * reload and keeps its trace alone */
	CASE("a refused map falls back without a derive warning, the loader "
	     "reports it");
	cfg_default();
	cfg.channel[0].interdigit_s = 1;
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map),
		 "N5");
	warn_capture_begin();
	fixture_start();
	CHECK_UINT(warn_capture_count("digit map"), 0);
	CHECK_UINT(warn_capture_count("digit_map"), 0);
	warn_capture_end();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "*67");
	CHECK(!cop_has(C_DIAL));
	uloop_pump(1300);
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, "*67") == 0);
}

static void expert_fixture(const char *map)
{
	cfg_default();
	cfg.channel[0].interdigit_s = 1;
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map),
		 "%s", map);
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
}

static void expert_dial_now(const char *map, const char *dialled)
{
	expert_fixture(map);
	inject_digits(0, dialled);
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, dialled) == 0);
	CHECK(state_is(0, "CALLING"));
}

static void expert_dial_on_timer(const char *map, const char *dialled)
{
	expert_fixture(map);
	inject_digits(0, dialled);
	CHECK(!cop_has(C_DIAL));
	uloop_pump(1300);
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, dialled) == 0);
	CHECK(state_is(0, "CALLING"));
}

/*
 * A mid-map timer through the real matcher and collector (dialplan.h, the
 * collector contract). The status reports the number without its markers.
 */
static void t_dialplan_timer_symbol(void)
{
	static const char map[] = "9T11|9xxx";
	struct line_view v;

	CASE("timer symbol map '9T11|9xxx': '9123' dials at once");
	expert_dial_now(map, "9123");

	CASE("timer symbol map '9T11|9xxx': 9, the long timer, 1, 1 dials "
	     "911, and the status leaves the T out");
	cfg_default();
	cfg.channel[0].interdigit_s = 1;
	cfg.channel[0].interdigit_long_s = 2;
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map),
		 "%s", map);
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "9");
	uloop_pump(2300);
	CHECK(!cop_has(C_DIAL));
	CHECK(state_is(0, "COLLECTING"));
	CHECK(view_get(0, &v));
	CHECK(strcmp(v.digits, "9") == 0);
	inject_digits(0, "11");
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, "911") == 0);
	CHECK(state_is(0, "CALLING"));
}

/* the operator acceptance matrix through the real matcher and collector */
static void t_dialplan_expert(void)
{
	static const char map[] = "*xx.T|x.T|911|411|[2-9]xxxxxx|1xxxxxxxxxx"
				  "|011x.T|*81|*83*|*84#|#85#|*72x.T";

	CASE("expert map: '1234' dials on the short timer");
	expert_dial_on_timer(map, "1234");

	CASE("expert map: '411' dials at once");
	expert_dial_now(map, "411");

	CASE("expert map: '7770005' dials at once");
	expert_dial_now(map, "7770005");

	CASE("expert map: '15552345678' dials at once");
	expert_dial_now(map, "15552345678");

	CASE("expert map: '*81' dials at once");
	expert_dial_now(map, "*81");

	CASE("expert map: '*83*' dials at once");
	expert_dial_now(map, "*83*");

	CASE("expert map: '*84#' dials at once with its '#'");
	expert_dial_now(map, "*84#");

	CASE("expert map: '#85#' dials at once with both '#'");
	expert_dial_now(map, "#85#");

	CASE("expert map: '1234#' dials '1234' at once, the '#' delimits");
	expert_fixture(map);
	inject_digits(0, "1234#");
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, "1234") == 0);
	CHECK(state_is(0, "CALLING"));

	CASE("expert map: '#96' dies on the '9' and clears");
	expert_fixture(map);
	inject_digits(0, "#96");
	CHECK(!cop_has(C_DIAL));
	CHECK(state_is(0, "CLEARING"));
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);

	CASE("service map '*xx*.x.#.T': '*61*123#' dials on the short timer");
	expert_dial_on_timer("*xx*.x.#.T", "*61*123#");

	CASE("service map '*03*xx.*xx.*xx.#': the full code dials at once");
	expert_dial_now("*03*xx.*xx.*xx.#", "*03*12*34*56#");

	CASE("service map '*#xx*x.#': the full code dials at once");
	expert_dial_now("*#xx*x.#", "*#61*22#");

	CASE("service map '#xx*.x.T': '#33*123' dials on the short timer");
	expert_dial_on_timer("#xx*.x.T", "#33*123");
}

/*
 * The field map through the real collector. Its '[*#x]' class carries
 * the wildcard, so these cases check the whole map on a line and not only in
 * the matcher.
 */
static void t_dialplan_field(void)
{
	static const char map[] =
		"[2-9]11|[2-9]xxxxxxxxx|1[2-9]xxxxxxxxx|*55|*56|*57|*69|*72|*73"
		"|*81|*90|*98|xx.T|[*#][*#x].T|xx.#";

	CASE("field map: '911' dials at once");
	expert_dial_now(map, "911");

	CASE("field map: '2125551234' dials at once");
	expert_dial_now(map, "2125551234");

	CASE("field map: '12125551234' dials at once");
	expert_dial_now(map, "12125551234");

	CASE("field map: '*72' dials at once");
	expert_dial_now(map, "*72");

	CASE("field map: '*55' dials at once");
	expert_dial_now(map, "*55");

	CASE("field map: '55#' dials at once with its '#' in the number");
	expert_dial_now(map, "55#");

	CASE("field map: '*5' dials on the short timer, so '*55' needs its "
	     "third digit inside it");
	expert_dial_on_timer(map, "*5");

	CASE("field map: '5*' leaves no alternative and clears");
	expert_fixture(map);
	inject_digits(0, "5*");
	CHECK(!cop_has(C_DIAL));
	CHECK(state_is(0, "CLEARING"));
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);
}

/* a warmline number of exactly n digits, so the boundary cases name the
 * length they are about instead of counting characters in a literal */
static void warmline_number_set(struct cfg_channel *ch, unsigned int digits)
{
	unsigned int i;

	for (i = 0; i < digits && i + 1 < sizeof(ch->warmline_number); i++)
		ch->warmline_number[i] = (char)('0' + i % 10);

	ch->warmline_number[i] = 0;
}

static void t_timers(void)
{
	CASE("the start digit timer runs in seconds converted to milliseconds");
	cfg_default();
	cfg.channel[0].start_digit_s = 1;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(300);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	uloop_pump(1000);
	CHECK(state_is(0, "CLEARING"));

	CASE("the expired start digit window plays reorder");
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);

	CASE("a zero timer fires on the next loop pass");
	cfg_default();
	cfg.channel[0].start_digit_s = 0;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(50);
	CHECK(state_is(0, "CLEARING"));

	CASE("the warmline number is dialled when its timer expires");
	cfg_default();
	cfg.channel[0].warmline_enable = true;
	cfg.channel[0].warmline_timeout_s = 1;
	snprintf(cfg.channel[0].warmline_number, sizeof(cfg.channel[0].warmline_number),
		 "5551234");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(300);
	CHECK(!cop_has(C_DIAL));
	uloop_pump(1000);
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, "5551234") == 0);

	/*
	 * The hotline dials inside the seize. No pump runs between the hook
	 * event and the assert, so a zero length timer would still be sitting
	 * in the uloop list here.
	 */
	CASE("a zero warmline window dials inside the seize itself");
	cfg_default();
	cfg.channel[0].warmline_enable = true;
	cfg.channel[0].warmline_timeout_s = 0;
	snprintf(cfg.channel[0].warmline_number, sizeof(cfg.channel[0].warmline_number),
		 "5551234");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, "5551234") == 0);
	CHECK(state_is(0, "CALLING"));

	CASE("the hotline leaves no dial tone behind the call it started");
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK(!hop_has(H_TONE));

	/* all three conjuncts, not two: a number and a zero window with the
	 * service switched off is an ordinary start digit window */
	CASE("a zero window with the warmline disabled dials nothing");
	cfg_default();
	cfg.channel[0].warmline_enable = false;
	cfg.channel[0].warmline_timeout_s = 0;
	snprintf(cfg.channel[0].warmline_number, sizeof(cfg.channel[0].warmline_number),
		 "5551234");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(!cop_has(C_DIAL));
	CHECK(state_is(0, "OFFHOOK_DIAL"));

	/*
	 * The entry that carries its own window is not a hotline entry. A dead
	 * held leg hands the line back to dial tone with a start digit window
	 * the caller armed, and a hotline that fired there would dial the
	 * moment the other party hung up on a call the subscriber is still
	 * holding the handset for.
	 */
	CASE("a hotline does not fire for the entry that carries its own window");
	cfg_default();
	fake_region.flash_service = HAL_FLASH_SERVICE_SPLIT;
	cfg.channel[0].warmline_enable = true;
	cfg.channel[0].warmline_timeout_s = 0;
	snprintf(cfg.channel[0].warmline_number, sizeof(cfg.channel[0].warmline_number),
		 "5551234");
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(state_is(0, "CALLING"));
	rec_cb.answered(0, 0);
	CHECK(state_is(0, "CONNECTED"));
	inject_hook(0, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(0, 0, true);
	CHECK(state_is(0, "HOLD_DIAL"));
	cops_reset();
	rec_cb.remote_bye(0, 0);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK(!cop_has(C_DIAL));

	/*
	 * A number that does not fit dials nothing. Dialling it cut would reach
	 * a different party than the config names, so the view carries it as
	 * absent and both fire points give the reorder an unusable warmline
	 * earns. This case takes the hotline fire point.
	 */
	CASE("a warmline number longer than the digit buffer gives reorder");
	cfg_default();
	cfg.channel[0].warmline_enable = true;
	cfg.channel[0].warmline_timeout_s = 0;
	warmline_number_set(&cfg.channel[0], FSM_DIGITS_MAX + 1);
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(50);
	CHECK(!cop_has(C_DIAL));
	CHECK(state_is(0, "CLEARING"));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);

	/* and the same at the timer fire point, which is the other one */
	CASE("the timer fire point gives reorder for it too");
	cfg_default();
	cfg.channel[0].warmline_enable = true;
	cfg.channel[0].warmline_timeout_s = 1;
	warmline_number_set(&cfg.channel[0], FSM_DIGITS_MAX + 1);
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(1300);
	CHECK(!cop_has(C_DIAL));
	CHECK(state_is(0, "CLEARING"));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);

	/* the boundary pair: the longest number that fits still dials */
	CASE("a number of exactly the buffer length still dials");
	cfg_default();
	cfg.channel[0].warmline_enable = true;
	cfg.channel[0].warmline_timeout_s = 0;
	warmline_number_set(&cfg.channel[0], FSM_DIGITS_MAX);
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(cop_has(C_DIAL));
	CHECK_UINT(strlen(cop_find(C_DIAL)->num), FSM_DIGITS_MAX);

	CASE("the boundary number warns about nothing");
	cfg_default();
	cfg.channel[0].warmline_enable = true;
	warmline_number_set(&cfg.channel[0], FSM_DIGITS_MAX);
	warn_capture_begin();
	fixture_start();
	CHECK_UINT(warn_capture_count("warmline_number"), 0);

	CASE("one digit past the boundary warns once");
	cfg_default();
	cfg.channel[0].warmline_enable = true;
	warmline_number_set(&cfg.channel[0], FSM_DIGITS_MAX + 1);
	warn_capture_begin();
	fixture_start();
	CHECK_UINT(warn_capture_count("warmline_number"), 1);

	/* a count that closed the capture would answer 0 to every question
	 * after the first, and an expected 0 could not fail anywhere in this
	 * file */
	CASE("one capture answers the same warning twice");
	cfg_default();
	cfg.channel[0].warmline_enable = true;
	warmline_number_set(&cfg.channel[0], FSM_DIGITS_MAX + 1);
	warn_capture_begin();
	fixture_start();
	CHECK_UINT(warn_capture_count("warmline_number"), 1);
	CHECK_UINT(warn_capture_count("warmline_number"), 1);

	CASE("a disabled warmline is reported about nothing either");
	cfg_default();
	warmline_number_set(&cfg.channel[0], FSM_DIGITS_MAX + 1);
	warn_capture_begin();
	fixture_start();
	CHECK_UINT(warn_capture_count("warmline_number"), 0);
	warn_capture_end();

	/*
	 * A reload lands at the next rest state, which the fixed fsm.h
	 * interface enforces, so the window a seize opened is the window that
	 * governs that seizure however long the reload takes to be accepted.
	 */
	CASE("a reload cannot move a warmline window the seize already opened");
	cfg_default();
	cfg.channel[0].warmline_enable = true;
	cfg.channel[0].warmline_timeout_s = 1;
	snprintf(cfg.channel[0].warmline_number, sizeof(cfg.channel[0].warmline_number),
		 "5551234");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(300);
	CHECK(!cop_has(C_DIAL));
	cfg.channel[0].warmline_timeout_s = 30;
	snprintf(cfg.channel[0].warmline_number, sizeof(cfg.channel[0].warmline_number),
		 "5559999");
	lines_reload(&cfg);
	cops_reset();
	uloop_pump(1000);
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, "5551234") == 0);

	CASE("and the staged window governs the next seizure");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	cops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(1300);
	CHECK(!cop_has(C_DIAL));
	CHECK(state_is(0, "OFFHOOK_DIAL"));

	CASE("an empty warmline number gives reorder, not silence");
	cfg_default();
	cfg.channel[0].warmline_enable = true;
	cfg.channel[0].warmline_timeout_s = 0;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(50);
	CHECK(!cop_has(C_DIAL));
	CHECK(state_is(0, "CLEARING"));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);

	CASE("the interdigit timer dials the collected buffer");
	cfg_default();
	cfg.channel[0].interdigit_s = 1;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '1', false);
	inject_digit(0, '2', false);
	uloop_pump(300);
	CHECK(!cop_has(C_DIAL));
	uloop_pump(1000);
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, "12") == 0);
	CHECK(state_is(0, "CALLING"));
}

static void t_defer(void)
{
	struct line_view v;

	CASE("a synchronous dial failure does not re-enter the machine");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	dial_fail_times = 1;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	CHECK(cop_has(C_DIAL));
	CHECK(state_is(0, "CLEARING"));

	CASE("the deferred failure arrives after the transition, as fastbusy");
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);

	CASE("the shell drained the failure without a uloop pass");
	CHECK(state_is(0, "CLEARING"));
	view_get(0, &v);
	CHECK_UINT(v.deferred_lost, 0);

	CASE("a timer driven dial defers its failure the same way");
	cfg_default();
	cfg.channel[0].interdigit_s = 1;
	fixture_start();
	dial_fail_times = 1;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '1', false);
	uloop_pump(1400);
	CHECK(cop_has(C_DIAL));
	CHECK(state_is(0, "CLEARING"));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);

	CASE("the deferred ring drops what does not fit and counts it");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	dial_fail_times = 9;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	view_get(0, &v);
	CHECK_UINT(v.deferred_lost, 1);

	CASE("an overflowing ring still lands the line on the failure tone");
	CHECK(state_is(0, "CLEARING"));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);

	dial_fail_times = 0;
}

/*
 * A line whose account cannot carry a dial: no account, no registrar target
 * or no live registration. The IDLE seize gets the fast busy chain at once,
 * because every dial it could make would be refused with congestion.
 */
static void t_no_target(void)
{
	unsigned int digits = 99;

	CASE("no target: a seize plays reorder and never a dial tone");
	cfg_default();
	fixture_start();
	dial_ready_answer = false;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK(state_is(0, "CLEARING"));
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_find(H_TONE)->val, HAL_TONE_REORDER);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);
	CHECK(!cop_has(C_DIAL));

	CASE("no target: the seize still arms the digit detector");
	CHECK(hop_has(H_CHAN_CFG));

	CASE("no target: digits over the reorder dial nothing");
	inject_digit(0, '5', false);
	uloop_pump(50);
	CHECK(!cop_has(C_DIAL));
	CHECK(state_is(0, "CLEARING"));

	/* the CLEARING flash reads the account at that instant, the way the
	 * seize does */
	CASE("no target: a flash over the reorder gives no dial tone");
	inject_hook(0, HAL_HOOK_FLASH, 0);
	uloop_pump(50);
	CHECK(state_is(0, "CLEARING"));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);

	CASE("no target: a flash after the account can dial again gives the dial tone");
	dial_ready_answer = true;
	inject_hook(0, HAL_HOOK_FLASH, 0);
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);
	dial_ready_answer = false;

	CASE("no target: the release returns the line to IDLE");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	uloop_pump(50);
	CHECK(state_is(0, "IDLE"));

	CASE("a target again: the next seize plays the dial tone");
	dial_ready_answer = true;
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);

	CASE("no target: a hotline line does not dial at the seize");
	cfg_default();
	cfg.channel[0].warmline_enable = true;
	cfg.channel[0].warmline_timeout_s = 0;
	snprintf(cfg.channel[0].warmline_number,
		 sizeof(cfg.channel[0].warmline_number), "5551234");
	fixture_start();
	dial_ready_answer = false;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(50);
	CHECK(!cop_has(C_DIAL));
	CHECK(state_is(0, "CLEARING"));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);

	CASE("no target: an injected dial seizes into reorder and dials nothing");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	dial_ready_answer = false;
	CHECK_UINT(line_inject_dial(0, "5", &digits), LINE_INJECT_OK);
	CHECK(!cop_has(C_DIAL));
	CHECK(state_is(0, "CLEARING"));

	CASE("no target: a seize still answers a ringing call");
	cfg_default();
	fixture_start();
	inject_incoming(0, 0, "5551000", "Alice", 0);
	CHECK(state_is(0, "RINGING"));
	dial_ready_answer = false;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(cop_has(C_ANSWER));

	dial_ready_answer = true;
}

/*
 * The overtake shape of the daemon: an FSM timer dials, call.c reports the
 * failure inside that frame, and the hook event of the next poll arrives
 * before the 0 ms drain timer of the queued failure runs. uloop processes
 * ready descriptors after the timeout sweep of a pass, so the daemon really
 * does deliver the hook event first. Here the pump stops in the same pass as
 * the timer, which leaves the queue loaded, and the case asserts that
 * precondition before it injects the release, so a drained queue fails the
 * case instead of passing it for the wrong reason.
 */
static unsigned int overtake_deferred;
static char overtake_state[32];

static void overtake_cb(struct uloop_fd *ufd, unsigned int events)
{
	struct line_view v;
	char byte;

	if (read(ufd->fd, &byte, 1) != 1)
		return;

	/* the overtake window: a frame closed, its callback event is still
	 * queued, and the next descriptor event arrives */
	view_get(0, &v);
	overtake_deferred = v.deferred;
	snprintf(overtake_state, sizeof(overtake_state), "%s", v.state);

	inject_hook(0, HAL_HOOK_RELEASE, 0);
	uloop_end();
}

static struct uloop_fd overtake_ufd = { .cb = overtake_cb };

static void t_fifo(void)
{
	struct line_view v;

	CASE("the overtake window exists: a dial failure waits while a hook event arrives");
	cfg_default();
	cfg.channel[0].interdigit_s = 1;
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5|1T|1x");
	fixture_start();

	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	CHECK(state_is(0, "CONNECTED"));
	inject_hook(0, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(0, 0, true);
	CHECK(state_is(0, "HOLD_DIAL"));

	/* the second number is collected in HOLD_DIAL itself, not COLLECTING */
	inject_digit(0, '1', false);
	CHECK(state_is(0, "HOLD_DIAL"));
	dial_fail_times = 1;
	notify_armed = true;
	overtake_deferred = 0;
	overtake_state[0] = 0;
	cops_reset();
	hops_reset();
	uloop_pump(2000);

	CHECK(cop_has(C_DIAL));
	CHECK_UINT(overtake_deferred, 1);
	CHECK(strcmp(overtake_state, "CALLING") == 0);

	CASE("the queued failure runs first, so the release parks the held leg");
	CHECK(state_is(0, "HOLDING"));
	view_get(0, &v);
	CHECK_UINT(v.deferred, 0);

	/* a release that overtook the failure would hang up the failed call as
	 * if it were live; in order, the release finds only the held leg */
	CASE("the held leg rings back, and no call is hung up");
	CHECK(!cop_has(C_HANGUP));
	CHECK(hop_has(H_RING_START));

	CASE("the failure was announced before the release cleared it");
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_find(H_TONE)->val, HAL_TONE_REORDER);
}

static void t_reinvite_refused(void)
{
	CASE("a refused hold ends the pending operation instead of stranding it");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	CHECK(state_is(0, "CONNECTED"));
	hold_refuse = true;
	cops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(cop_has(C_HOLD));
	CHECK(state_is(0, "CONNECTED"));

	CASE("the refusal is not lost, the call keeps running");
	inject_digit(0, '4', false);
	CHECK(cop_has(C_DTMF));

	CASE("a refused resume clears the line audibly");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	inject_hook(0, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(0, 0, true);
	CHECK(state_is(0, "HOLD_DIAL"));
	resume_refuse = true;
	hops_reset();
	cops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(cop_has(C_RESUME));
	CHECK(state_is(0, "CLEARING"));
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);

	/* a recall ring would only lead to the same refusal */
	CASE("the on-hook after a refused resume hangs up the held call");
	hops_reset();
	cops_reset();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK(cop_has(C_HANGUP));
	CHECK(!hop_has(H_RING_START));
}

static void t_detector(void)
{
	struct line_view v;

	CASE("a transient detector refusal is retried once and then works");
	cfg_default();
	fixture_start();
	chan_cfg_transient = 1;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK_UINT(hop_count(H_CHAN_CFG), 2);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	view_get(0, &v);
	CHECK(!v.oos);

	CASE("a detector that stays transient takes the line out of service");
	cfg_default();
	fixture_start();
	chan_cfg_transient = 2;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK_UINT(hop_count(H_CHAN_CFG), 2);
	CHECK(state_is(0, "OOS"));
	view_get(0, &v);
	CHECK(v.oos);

	CASE("the seized out of service line plays reorder");
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);

	CASE("a fatal detector failure is not retried");
	cfg_default();
	fixture_start();
	chan_cfg_fatal = true;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK_UINT(hop_count(H_CHAN_CFG), 1);
	CHECK(state_is(0, "OOS"));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);

	CASE("no digit is collected on the line the detector lost");
	inject_digit(0, '5', false);
	view_get(0, &v);
	CHECK(strcmp(v.digits, "") == 0);
	CHECK(state_is(0, "OOS"));
}

/*
 * The SLIC fault branch, the line half.
 *
 * A HAL_EVENT_SLIC_FAULT with a code that names a bit says that the SLIC of
 * that port reported a fault. Such a port cannot carry a call, so it leaves
 * service, as a failed detector arm makes it leave. The shell keeps that
 * reason, so a reload leaves the line out. A record with a code of 0 is the
 * clear, and it removes the reason again.
 *
 * The branch takes every record that the dispatch hands it, on purpose:
 * event_dispatch.c owns the decisions in front of it. It exits the daemon on
 * the engine stall bit and on the device reset bit, and does not hand those
 * records over. It hands nothing over unless the backend that produced the
 * record fixes the meaning of its fault bits. That is the udsp backend alone.
 *
 * A second gate here would copy that policy into a shell that is otherwise
 * free of backend knowledge, and two copies drift.
 */
static void t_slic_fault(void)
{
	struct line_view v;

	CASE("a slic fault takes the reporting line out of service");
	cfg_default();
	fixture_start();
	view_get(0, &v);
	CHECK(!v.oos);
	CHECK(!v.fault);
	inject_fault(0, FAULT_THERMAL);
	CHECK(state_is(0, "OOS"));
	view_get(0, &v);
	CHECK(v.oos);
	CHECK(v.fault);

	/*
	 * The admission gate of the leg layer (call.h calls_line_admit_bind).
	 * A faulted line runs a machine that ignores the incoming call event,
	 * so an INVITE to it is refused in front of the leg.
	 */
	CASE("the faulted line admits no inbound call");
	CHECK(!line_call_admit(0));
	CHECK(rec_line_admit && !rec_line_admit(0));
	CHECK(rec_line_admit && rec_line_admit(1) == line_call_admit(1));

	CASE("the seized faulted line plays reorder, like any out of service port");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);

	CASE("no digit is collected on the faulted line");
	inject_digit(0, '5', false);
	view_get(0, &v);
	CHECK(strcmp(v.digits, "") == 0);
	CHECK(state_is(0, "OOS"));

	CASE("a second fault on the same line is idempotent");
	inject_fault(0, FAULT_THERMAL);
	CHECK(state_is(0, "OOS"));
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "OOS"));

	/*
	 * A SLIC fault names one port, so it must take that port and no other.
	 * The default fixture leaves channel 1 disabled, which is out of
	 * service for its own reason, so the second line is enabled here to
	 * make the assertion mean something.
	 */
	CASE("the fault takes only the port that reported it");
	cfg_default();
	cfg.channel[1].enabled = true;
	fixture_start();
	CHECK(state_is(0, "IDLE"));
	CHECK(state_is(1, "IDLE"));
	inject_fault(0, FAULT_THERMAL);
	CHECK(state_is(0, "OOS"));
	CHECK(state_is(1, "IDLE"));
	view_get(1, &v);
	CHECK(!v.oos);
	CHECK(!v.fault);

	CASE("and a fault on the second port takes that one instead");
	inject_fault(1, FAULT_THERMAL);
	CHECK(state_is(1, "OOS"));
	view_get(1, &v);
	CHECK(v.oos);
	CHECK(v.fault);

	/*
	 * The memory is what a reload cannot argue with. lines_reload()
	 * derives the service state from the config, and the config says
	 * nothing about a faulted part. Without the memory the line comes back
	 * into service with the fault standing.
	 */
	CASE("a reload keeps a faulted line out of service");
	cfg_default();
	fixture_start();
	inject_fault(0, FAULT_THERMAL);
	CHECK(state_is(0, "OOS"));
	lines_reload(&cfg);
	CHECK(state_is(0, "OOS"));
	view_get(0, &v);
	CHECK(v.oos);
	CHECK(v.fault);

	/*
	 * The clear is the backend's own record (hal.h u.fault.code): the
	 * fault bits read clean and the feed is confirmed, so the line is
	 * servable again.
	 */
	CASE("the clear returns the line to service and re-reads its hook");
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	CHECK(!line_call_admit(0));
	inject_fault(0, 0);
	CHECK(state_is(0, "IDLE"));
	view_get(0, &v);
	CHECK(!v.oos);
	CHECK(!v.fault);
	CHECK_UINT(hook_query_calls[0], 1);

	CASE("and the line admits an inbound call again");
	CHECK(line_call_admit(0));
	/* the port this fixture leaves disabled is out of service for its own
	 * reason, so a gate that answered the daemon state alone would admit
	 * it */
	CHECK(!line_call_admit(1));

	/* the hook read on the service entry decides the state after the
	 * clear; without it the line stays in the IDLE the entry leaves */
	CASE("a handset lifted through the fault seizes on the clear");
	cfg_default();
	fixture_start();
	inject_fault(0, FAULT_THERMAL);
	CHECK(state_is(0, "OOS"));
	boot_offhook[0] = true;
	inject_fault(0, 0);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	boot_offhook[0] = false;

	CASE("a clear on a line with no fault moves nothing and reads no hook");
	cfg_default();
	fixture_start();
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	inject_fault(0, 0);
	CHECK(state_is(0, "IDLE"));
	view_get(0, &v);
	CHECK(!v.oos);
	CHECK(!v.fault);
	CHECK_UINT(hook_query_calls[0], 0);

	/* A failed detector arm takes a line out for its own reason and a
	 * reload heals it. The clear
	 * answers the SLIC fault alone, so it must not heal that line. */
	CASE("a clear leaves a line the detector took out where it is");
	cfg_default();
	fixture_start();
	chan_cfg_fatal = true;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(state_is(0, "OOS"));
	view_get(0, &v);
	CHECK(v.oos);
	CHECK(!v.fault);
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	inject_fault(0, 0);
	CHECK(state_is(0, "OOS"));
	view_get(0, &v);
	CHECK(v.oos);
	CHECK_UINT(hook_query_calls[0], 0);
	chan_cfg_fatal = false;

	/* the two halves of the predicate are separate: the clear ends the
	 * fault half alone, and the config half still holds the line out */
	CASE("a clear on a config disabled line leaves it out of service");
	cfg_default();
	fixture_start();
	inject_fault(0, FAULT_THERMAL);
	cfg.channel[0].enabled = false;
	lines_reload(&cfg);
	CHECK(state_is(0, "OOS"));
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	inject_fault(0, 0);
	CHECK(state_is(0, "OOS"));
	view_get(0, &v);
	CHECK(v.oos);
	CHECK(!v.fault);
	CHECK_UINT(hook_query_calls[0], 0);

	/* The key rides the present-only-when-true convention of ring_held and
	 * conf, and the schema declares the value true. A dump that emitted a
	 * false would break both, so the value is read beside the key. */
	CASE("the status dump carries the fault key only while faulted");
	cfg_default();
	fixture_start();
	view_get(0, &v);
	CHECK(!v.fault_set);
	inject_fault(0, FAULT_THERMAL);
	view_get(0, &v);
	CHECK(v.fault_set);
	CHECK(v.fault);
	inject_fault(0, 0);
	view_get(0, &v);
	CHECK(!v.fault_set);
	CHECK(!v.fault);

	/*
	 * The fault arrives between the two halves of the bring-up. The early
	 * half wires the machines, and the DSP half settles the service state
	 * against the capability the backend reports. That settle must read
	 * the fault the window recorded.
	 */
	CASE("a fault between the bring-up halves survives the dsp half");
	cfg_default();
	lines_stop();
	lines_init(&cfg);
	lines_live = true;
	inject_fault(0, FAULT_THERMAL);
	CHECK(state_is(0, "OOS"));
	lines_start(&cfg);
	CHECK(state_is(0, "OOS"));
	view_get(0, &v);
	CHECK(v.oos);
	CHECK(v.fault);

	/* the line is already out for the config reason when the fault lands;
	 * the record is the only place that fault can be kept */
	CASE("a fault on a config disabled line is recorded too");
	cfg_default();
	cfg.channel[1].enabled = false;
	fixture_start();
	CHECK(state_is(1, "OOS"));
	inject_fault(1, FAULT_THERMAL);
	view_get(1, &v);
	CHECK(v.oos);
	CHECK(v.fault);

	CASE("and the fault keeps it out of service when a reload enables it");
	cfg.channel[1].enabled = true;
	lines_reload(&cfg);
	CHECK(state_is(1, "OOS"));
	view_get(1, &v);
	CHECK(v.oos);
	CHECK(v.fault);

	CASE("its clear then returns it to service");
	inject_fault(1, 0);
	CHECK(state_is(1, "IDLE"));
	view_get(1, &v);
	CHECK(!v.oos);
	CHECK(!v.fault_set);

	/*
	 * The fault and the detector arm both took this line out. The clear
	 * drops the fault, the derive finds the line servable and the line
	 * comes back. A reload performs the same heal for a failed detector
	 * arm, and the detector arms
	 * again at the next seize.
	 */
	CASE("a clear heals a line the fault and the detector both took out");
	cfg_default();
	fixture_start();
	chan_cfg_fatal = true;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(state_is(0, "OOS"));
	inject_fault(0, FAULT_THERMAL);
	view_get(0, &v);
	CHECK(v.oos);
	CHECK(v.fault);
	chan_cfg_fatal = false;
	inject_fault(0, 0);
	CHECK(state_is(0, "IDLE"));
	view_get(0, &v);
	CHECK(!v.oos);
	CHECK(!v.fault_set);

	/* the fault tore the call down; the hook read on the service entry
	 * then makes the state after the clear the state of the handset */
	CASE("a fault in a call and a clear on a lifted handset seize again");
	cfg_default();
	snprintf(cfg.channel[0].digit_map,
		 sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	CHECK(state_is(0, "CONNECTED"));
	inject_fault(0, FAULT_THERMAL);
	CHECK(state_is(0, "OOS"));
	boot_offhook[0] = true;
	inject_fault(0, 0);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	boot_offhook[0] = false;

	/*
	 * The daemon frees the config on the way out, and the suites re-enter
	 * the bring-up per fixture. A record can therefore meet a shell with
	 * no config. The fault ends, and nothing is derived from a config that
	 * is not there.
	 */
	CASE("a clear with no config drops the fault and derives nothing");
	cfg_default();
	fixture_start();
	inject_fault(0, FAULT_THERMAL);
	CHECK(state_is(0, "OOS"));
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	uv.cfg = NULL;
	inject_fault(0, 0);
	uv.cfg = &cfg;
	CHECK(state_is(0, "OOS"));
	view_get(0, &v);
	CHECK(v.oos);
	CHECK(!v.fault_set);
	CHECK_UINT(hook_query_calls[0], 0);
}

/*
 * The dead line branch, the line half.
 *
 * A HAL_EVENT_LINE record with HAL_LINE_DOWN says that the backend declared
 * this port dead: its SLIC init failed, or its control bus answers nothing.
 * Such a port cannot carry a call, so it leaves service, and the shell records
 * the reason. A record with any other state says that the port carries a feed
 * again, and it ends that reason.
 *
 * One memory holds both reasons, so every case below reads the same `fault`
 * key of the status dump that the SLIC fault cases read. The two sources
 * cannot cross on hardware. The backend takes a dead line out of its fault
 * scan, so no SLIC clear follows a dead line.
 *
 * The branch takes every record that the dispatch hands it, with the layering
 * of the SLIC fault branch above. event_dispatch.c hands nothing over unless
 * the udsp backend produced the record.
 */
static void t_line_down(void)
{
	struct line_view v;
	uint8_t state;

	CASE("a dead line report takes the reporting line out of service");
	cfg_default();
	fixture_start();
	view_get(0, &v);
	CHECK(!v.oos);
	CHECK(!v.fault);
	inject_line_state(0, HAL_LINE_DOWN);
	CHECK(state_is(0, "OOS"));
	view_get(0, &v);
	CHECK(v.oos);
	CHECK(v.fault);

	/* the dead line is the fault's own reason under the fault's own key,
	 * so "voice status" reports it with no new word */
	CASE("and the status dump carries the fault key only while it is down");
	CHECK(v.fault_set);
	inject_line_state(0, HAL_LINE_ACTIVE);
	view_get(0, &v);
	CHECK(!v.fault_set);
	CHECK(!v.fault);

	CASE("the seized dead line plays reorder like any dead port");
	inject_line_state(0, HAL_LINE_DOWN);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);

	CASE("a second dead line report on the same line is idempotent");
	inject_line_state(0, HAL_LINE_DOWN);
	CHECK(state_is(0, "OOS"));
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "OOS"));
	view_get(0, &v);
	CHECK(v.fault);

	CASE("the report takes only the port that reported it");
	cfg_default();
	cfg.channel[1].enabled = true;
	fixture_start();
	CHECK(state_is(0, "IDLE"));
	CHECK(state_is(1, "IDLE"));
	inject_line_state(0, HAL_LINE_DOWN);
	CHECK(state_is(0, "OOS"));
	CHECK(state_is(1, "IDLE"));
	view_get(1, &v);
	CHECK(!v.oos);
	CHECK(!v.fault);

	/*
	 * The memory is what a reload cannot argue with. lines_reload()
	 * derives the service state from the config, and the config says
	 * nothing about a dead port. Without the memory the line comes back
	 * into service with the port still dead.
	 */
	CASE("a reload keeps a dead line out of service");
	cfg_default();
	fixture_start();
	inject_line_state(0, HAL_LINE_DOWN);
	CHECK(state_is(0, "OOS"));
	lines_reload(&cfg);
	CHECK(state_is(0, "OOS"));
	view_get(0, &v);
	CHECK(v.oos);
	CHECK(v.fault);

	CASE("the line return puts the line back and re-reads its hook");
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	inject_line_state(0, HAL_LINE_ACTIVE);
	CHECK(state_is(0, "IDLE"));
	view_get(0, &v);
	CHECK(!v.oos);
	CHECK(!v.fault);
	CHECK_UINT(hook_query_calls[0], 1);

	/* the hook read on the service entry decides the state after the
	 * return; without it the line stays in the IDLE the entry leaves */
	CASE("a handset lifted through the dead line seizes on the return");
	cfg_default();
	fixture_start();
	inject_line_state(0, HAL_LINE_DOWN);
	CHECK(state_is(0, "OOS"));
	boot_offhook[0] = true;
	inject_line_state(0, HAL_LINE_ACTIVE);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	boot_offhook[0] = false;

	CASE("a return with no memory moves nothing and reads no hook");
	cfg_default();
	fixture_start();
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	inject_line_state(0, HAL_LINE_ACTIVE);
	CHECK(state_is(0, "IDLE"));
	view_get(0, &v);
	CHECK(!v.oos);
	CHECK(!v.fault);
	CHECK_UINT(hook_query_calls[0], 0);

	/* A failed detector arm takes a line out for its own reason and a
	 * reload heals it. The return
	 * answers the dead line alone, so it must not heal that line. */
	CASE("a return leaves a line the detector took out where it is");
	cfg_default();
	fixture_start();
	chan_cfg_fatal = true;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(state_is(0, "OOS"));
	view_get(0, &v);
	CHECK(v.oos);
	CHECK(!v.fault);
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	inject_line_state(0, HAL_LINE_ACTIVE);
	CHECK(state_is(0, "OOS"));
	view_get(0, &v);
	CHECK(v.oos);
	CHECK_UINT(hook_query_calls[0], 0);
	chan_cfg_fatal = false;

	/* the two halves of the predicate are separate: the return ends the
	 * hardware half alone, and the config half still holds the line out */
	CASE("a return on a config disabled line leaves it out of service");
	cfg_default();
	fixture_start();
	inject_line_state(0, HAL_LINE_DOWN);
	cfg.channel[0].enabled = false;
	lines_reload(&cfg);
	CHECK(state_is(0, "OOS"));
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	inject_line_state(0, HAL_LINE_ACTIVE);
	CHECK(state_is(0, "OOS"));
	view_get(0, &v);
	CHECK(v.oos);
	CHECK(!v.fault);
	CHECK_UINT(hook_query_calls[0], 0);

	/* the line is already out for the config reason when the report lands;
	 * the record is the only place that reason can be kept */
	CASE("a dead line report on a config disabled line is recorded too");
	cfg_default();
	cfg.channel[1].enabled = false;
	fixture_start();
	CHECK(state_is(1, "OOS"));
	inject_line_state(1, HAL_LINE_DOWN);
	view_get(1, &v);
	CHECK(v.oos);
	CHECK(v.fault);

	CASE("and the report keeps it out of service when a reload enables it");
	cfg.channel[1].enabled = true;
	lines_reload(&cfg);
	CHECK(state_is(1, "OOS"));
	view_get(1, &v);
	CHECK(v.oos);
	CHECK(v.fault);

	CASE("its return then puts it back in service");
	inject_line_state(1, HAL_LINE_ACTIVE);
	CHECK(state_is(1, "IDLE"));
	view_get(1, &v);
	CHECK(!v.oos);
	CHECK(!v.fault_set);

	/* HAL_LINE_DOWN is the one state that says the port is dead, so every
	 * other state of the enum returns the line */
	CASE("every line state above down returns the line");
	for (state = HAL_LINE_ACTIVE; state <= HAL_LINE_HOWLER; state++) {
		cfg_default();
		fixture_start();
		inject_line_state(0, HAL_LINE_DOWN);
		CHECK(state_is(0, "OOS"));
		inject_line_state(0, state);
		CHECK(state_is(0, "IDLE"));
		view_get(0, &v);
		CHECK(!v.fault_set);
	}

	/* the report tore the call down; the hook read on the service entry
	 * then makes the state after the return the state of the handset */
	CASE("a dead line in a call and a return on a lifted handset seize");
	cfg_default();
	snprintf(cfg.channel[0].digit_map,
		 sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	CHECK(state_is(0, "CONNECTED"));
	inject_line_state(0, HAL_LINE_DOWN);
	CHECK(state_is(0, "OOS"));
	boot_offhook[0] = true;
	inject_line_state(0, HAL_LINE_ACTIVE);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	boot_offhook[0] = false;

	/*
	 * The report arrives between the two halves of the bring-up. The early
	 * half wires the machines, and the DSP half settles the service state
	 * against the capability the backend reports. That settle must read
	 * the memory the window recorded.
	 */
	CASE("a dead line between the bring-up halves survives the dsp half");
	cfg_default();
	lines_stop();
	lines_init(&cfg);
	lines_live = true;
	inject_line_state(0, HAL_LINE_DOWN);
	CHECK(state_is(0, "OOS"));
	lines_start(&cfg);
	CHECK(state_is(0, "OOS"));
	view_get(0, &v);
	CHECK(v.oos);
	CHECK(v.fault);

	/*
	 * The daemon frees the config on the way out, and the suites re-enter
	 * the bring-up per fixture. A record can therefore meet a shell with
	 * no config. The memory ends, and nothing is derived from a config
	 * that is not there.
	 */
	CASE("a return with no config drops the memory and derives nothing");
	cfg_default();
	fixture_start();
	inject_line_state(0, HAL_LINE_DOWN);
	CHECK(state_is(0, "OOS"));
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	uv.cfg = NULL;
	inject_line_state(0, HAL_LINE_ACTIVE);
	uv.cfg = &cfg;
	CHECK(state_is(0, "OOS"));
	view_get(0, &v);
	CHECK(v.oos);
	CHECK(!v.fault_set);
	CHECK_UINT(hook_query_calls[0], 0);

	/*
	 * The SLIC fault and the dead line each ride their own bit, so a return
	 * ends the dead line alone and cannot lift a fault that still stands.
	 * The library cannot post a fault clear for a dead line, but the two
	 * records ride different rings, so a return can still overtake a fault
	 * that landed just before it. The bit split is what keeps the fault.
	 */
	CASE("fault, then dead line, then return leaves it out on the fault");
	cfg_default();
	fixture_start();
	inject_fault(0, FAULT_THERMAL);
	inject_line_state(0, HAL_LINE_DOWN);
	CHECK(state_is(0, "OOS"));
	inject_line_state(0, HAL_LINE_ACTIVE);
	CHECK(state_is(0, "OOS"));
	view_get(0, &v);
	CHECK(v.oos);
	CHECK(v.fault);

	CASE("a fault before the return leaves it out on the fault");
	cfg_default();
	fixture_start();
	inject_line_state(0, HAL_LINE_DOWN);
	inject_fault(0, FAULT_THERMAL);
	CHECK(state_is(0, "OOS"));
	inject_line_state(0, HAL_LINE_ACTIVE);
	CHECK(state_is(0, "OOS"));
	view_get(0, &v);
	CHECK(v.oos);
	CHECK(v.fault);

	/* the reverse: a fault clear cannot lift a dead line, which is the same
	 * split read from the other side */
	CASE("a fault clear leaves a still down line out of service");
	cfg_default();
	fixture_start();
	inject_line_state(0, HAL_LINE_DOWN);
	inject_fault(0, FAULT_THERMAL);
	inject_fault(0, 0);
	CHECK(state_is(0, "OOS"));
	view_get(0, &v);
	CHECK(v.oos);
	CHECK(v.fault);

	/*
	 * The ERR rides the entry, not the record. The backend re-posts DOWN
	 * after a lost completion, so a per record log would double count. The
	 * level is read too, so a NOTE cannot pass for the ERR the rule names.
	 */
	CASE("a dead line logs one error at the entry and nothing on a repeat");
	cfg_default();
	fixture_start();
	warn_capture_begin();
	inject_line_state(0, HAL_LINE_DOWN);
	inject_line_state(0, HAL_LINE_DOWN);
	/* one count reads the whole capture, so both records are counted at
	 * once: the first count call closes the capture to new writes */
	CHECK_UINT(warn_capture_count_at(LOG_ERR,
			"line 0 backend reports the line down"), 1);
	CHECK_UINT(warn_capture_count_at(LOG_NOTICE,
			"backend reports the line down"), 0);
	warn_capture_end();
}

/*
 * What the seize pushes, field by field. hal.h makes channel_config_set
 * authoritative for the gains, the detector mask and the echo canceller
 * together. Every one of them must therefore carry the configured value,
 * because a member left at zero switches something off on the hardware. A
 * push of the mask alone turns the canceller off on every call.
 */
static void t_channel_cfg(void)
{
	struct line_lec_view lv;
	uint32_t before, after;
	struct hop *c;

	/* The applied table of the lec row is a record and not a derivation, so
	 * a line that never seized has nothing to report there. */
	CASE("a line that never seized records no push at all");
	cfg_default();
	fixture_start();
	line_lec_view_get(0, &lv);
	CHECK(!lv.applied.valid);
	CHECK(!lv.applied.pending);

	CASE("a line with no lec options pushes the daemon policy");
	before = mono_ms_ref();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	after = mono_ms_ref();
	c = hop_find(H_CHAN_CFG);
	CHECK(c->chan.ec_enable);
	CHECK(c->chan.ec_nlp_enable);

	/*
	 * The push instant is the monotonic clock, read at the push. A wall
	 * clock value would jump under an NTP step, so the bracket pins it
	 * between two direct readings taken around the seize.
	 */
	CASE("and the record stamps the instant that push happened at");
	line_lec_view_get(0, &lv);
	CHECK(lv.applied.valid);
	CHECK(lv.applied.stamp_ms >= before);
	CHECK(lv.applied.stamp_ms <= after);

	CASE("and no gain step, because no loss plan asked for one");
	CHECK_UINT((unsigned int)(int)c->chan.gain_tx, 0);
	CHECK_UINT((unsigned int)(int)c->chan.gain_rx, 0);

	/* hal.h: 0 asks for the backend default tail, and no uci option carries
	 * a tail length for the shell to send instead */
	CASE("and the tail stays the backend default");
	CHECK_UINT(c->chan.ec_tail_ms, 0);

	/*
	 * The modem detector joins the two digit detectors and is always on:
	 * it is armed for the life of the channel. The policy that decides
	 * which reported tone switches the media is in the tone handler, not
	 * here, so this mask carries no fax condition of its own.
	 *
	 * The jitter buffer event bit stays out, and main.c caching those
	 * reports does not put it in. The mask is a detector request this
	 * shell composes from the channel policy, and no policy
	 * option asks for one; a backend that reports playout state does it on
	 * its own schedule, so the cache fills without the bit.
	 */
	CASE("and the mask is DTMF, PULSE and MODEM, nothing else");
	CHECK_UINT(c->chan.detect_mask,
		   HAL_DETECT_DTMF | HAL_DETECT_PULSE | HAL_DETECT_MODEM);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("manual control turns the canceller off");
	cfg_default();
	cfg.channel[0].lec_control = true;
	cfg.channel[0].lec_control_set = true;
	cfg.channel[0].lec_enable = false;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	c = hop_find(H_CHAN_CFG);
	CHECK(!c->chan.ec_enable);

	/* an absent lec_nlp follows the policy value, so switching the
	 * canceller off does not silently switch its processor off too */
	CASE("and an absent lec_nlp keeps the policy value");
	CHECK(c->chan.ec_nlp_enable);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("manual control with an explicit lec_nlp 0 turns the processor off");
	cfg_default();
	cfg.channel[0].lec_control = true;
	cfg.channel[0].lec_control_set = true;
	cfg.channel[0].lec_nlp = false;
	cfg.channel[0].lec_nlp_set = true;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	c = hop_find(H_CHAN_CFG);
	CHECK(!c->chan.ec_nlp_enable);

	CASE("and leaves the canceller on, which lec_enable still asks for");
	CHECK(c->chan.ec_enable);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	/*
	 * A config that carries lec_enable 0 and no lec_control asks for
	 * nothing: lec_enable takes effect only under manual control. A line
	 * that read the value anyway would switch the canceller off for a
	 * config that never turned manual control on.
	 */
	CASE("lec_enable 0 without lec_control leaves the policy standing");
	cfg_default();
	cfg.channel[0].lec_enable = false;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	c = hop_find(H_CHAN_CFG);
	CHECK(c->chan.ec_enable);
	CHECK(c->chan.ec_nlp_enable);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("lec_nlp 0 without lec_control leaves the processor on");
	cfg_default();
	cfg.channel[0].lec_nlp_set = true;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(hop_find(H_CHAN_CFG)->chan.ec_nlp_enable);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("a line with no tone options asks for the G.168 rules");
	cfg_default();
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	c = hop_find(H_CHAN_CFG);
	CHECK(!c->chan.ec_tone_latch);
	CHECK(!c->chan.ec_tone_force_disable);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	/*
	 * The disabler is not part of the manual block. It runs under the
	 * daemon policy as well, and these two only pick its release rule and
	 * its trigger mapping, so a config that never takes manual control
	 * still reaches them. A build that gated them on lec_control would
	 * leave the gear that needs the override with no way to ask for it.
	 */
	CASE("both tone options reach the DSP without lec_control");
	cfg_default();
	cfg.channel[0].lec_tone_latch = true;
	cfg.channel[0].lec_tone_force_disable = true;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	c = hop_find(H_CHAN_CFG);
	CHECK(c->chan.ec_tone_latch);
	CHECK(c->chan.ec_tone_force_disable);

	CASE("and they disturb neither the canceller nor its processor");
	CHECK(c->chan.ec_enable);
	CHECK(c->chan.ec_nlp_enable);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("manual control with the canceller off still carries them");
	cfg_default();
	cfg.channel[0].lec_control = true;
	cfg.channel[0].lec_control_set = true;
	cfg.channel[0].lec_enable = false;
	cfg.channel[0].lec_tone_latch = true;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	c = hop_find(H_CHAN_CFG);
	CHECK(!c->chan.ec_enable);
	CHECK(c->chan.ec_tone_latch);
	CHECK(!c->chan.ec_tone_force_disable);

	/*
	 * The lec row of "voice status" reports the same pair, and it must be
	 * the record of that push: a second copy of the lec_control resolution
	 * would let the row claim a canceller policy the DSP never got.
	 */
	CASE("the applied view answers the pair the push carried");
	line_lec_view_get(0, &lv);
	CHECK(lv.applied.valid);
	CHECK_UINT(lv.applied.status, HAL_OK);
	CHECK_UINT(lv.applied.ec, c->chan.ec_enable);
	CHECK_UINT(lv.applied.nlp, c->chan.ec_nlp_enable);
	CHECK(!lv.applied.ec);
	CHECK(lv.applied.nlp);
	CHECK(lv.applied.tone_latch);
	CHECK(!lv.applied.tone_force_disable);

	CASE("and nothing is pending, because that push is the current view");
	CHECK(!lv.applied.pending);

	/* The config half is the uci options unresolved, which is what lets a
	 * reader see that lec_control is why the two halves differ. */
	CASE("and the config half carries the uci options unresolved");
	CHECK(lv.config.control);
	CHECK(!lv.config.enable);
	CHECK(!lv.config.nlp_set);
	CHECK(lv.config.tone_latch);
	CHECK(!lv.config.tone_force_disable);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("and a line this board does not carry answers the daemon policy");
	line_lec_view_get(CFG_CHANNEL_MAX, &lv);
	CHECK(!lv.config.control);
	CHECK(lv.config.enable);
	CHECK(!lv.config.nlp_set);
	CHECK(!lv.config.tone_latch);
	CHECK(!lv.config.tone_force_disable);
	CHECK(!lv.applied.valid);

	/*
	 * A channel section above the backend line count names a port this
	 * board does not carry. The shell still holds a slot for it, so the
	 * view has to gate on the port and not on the array bound: a row built
	 * from that slot would report a configuration no DSP was ever sent.
	 */
	CASE("a channel above the line count answers the daemon policy too");
	cfg_default();
	cfg.channel[1].enabled = true;
	cfg.channel[1].lec_control = true;
	cfg.channel[1].lec_control_set = true;
	cfg.channel[1].lec_enable = false;
	cfg.channel[1].lec_tone_latch = true;
	fixture_start();
	fake_cap.lines = 1;
	line_lec_view_get(1, &lv);
	CHECK(!lv.config.control);
	CHECK(lv.config.enable);
	CHECK(!lv.config.tone_latch);
	CHECK(!lv.applied.valid);
	fake_cap.lines = 2;

	/* A refused push is still the last view the DSP was asked for, and the
	 * status beside it is what says the ask did not land. */
	CASE("a push the backend refuses is recorded with its status");
	cfg_default();
	fixture_start();
	chan_cfg_fatal = true;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	line_lec_view_get(0, &lv);
	CHECK(lv.applied.valid);
	CHECK_UINT(lv.applied.status, HAL_FATAL);
	CHECK(lv.applied.ec);
	CHECK(!lv.applied.pending);
	chan_cfg_fatal = false;
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	/*
	 * The tone disabler's view is dropped when the line comes to rest. The
	 * DSP clears its own latch once the last stream of the line stops and
	 * sends no record for that drop (udsp/src/core/stream.c
	 * stream_lec_release), so a view that outlived the call would report a
	 * hold the canceller no longer has.
	 */
	CASE("the rest state drops the tone disabler view of the line");
	cfg_default();
	fixture_start();
	uv.lec_policy[0].valid = true;
	uv.lec_policy[0].kind = HAL_PASSTHROUGH_MODEM;
	uv.lec_policy[1].valid = true;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(uv.lec_policy[0].valid);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(!uv.lec_policy[0].valid);
	CHECK_UINT(uv.lec_policy[0].kind, HAL_PASSTHROUGH_OFF);

	CASE("and it drops that line's view alone");
	CHECK(uv.lec_policy[1].valid);
	memset(uv.lec_policy, 0, sizeof(uv.lec_policy));

	/*
	 * The loss plan is reversed. half_db_rx_loss attenuates what the
	 * subscriber hears, which is the gain toward the interface, so it lands
	 * on gain_tx and not on gain_rx. A mapping that reads straight through
	 * would take the half decibel off the wrong direction.
	 */
	CASE("a receive loss lands on the transmit gain alone");
	cfg_default();
	cfg.channel[0].half_db_rx_loss = true;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	c = hop_find(H_CHAN_CFG);
	CHECK(c->chan.gain_tx == -1);
	CHECK(c->chan.gain_rx == 0);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("a transmit loss lands on the receive gain alone");
	cfg_default();
	cfg.channel[0].half_db_tx_loss = true;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	c = hop_find(H_CHAN_CFG);
	CHECK(c->chan.gain_rx == -1);
	CHECK(c->chan.gain_tx == 0);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("both losses take one step off each direction");
	cfg_default();
	cfg.channel[0].half_db_rx_loss = true;
	cfg.channel[0].half_db_tx_loss = true;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	c = hop_find(H_CHAN_CFG);
	CHECK(c->chan.gain_tx == -1);
	CHECK(c->chan.gain_rx == -1);

	CASE("and a loss plan does not disturb the canceller");
	CHECK(c->chan.ec_enable);
	CHECK(c->chan.ec_nlp_enable);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	/*
	 * The analog pair is not reversed: the tx gain goes to the SLIC TX
	 * path, so tx lands on tx. The uci value is whole dB and the HAL pair
	 * is half-dB steps, so -9 dB arrives as -18.
	 */
	CASE("the analog pair doubles the whole dB values, tx on tx");
	cfg_default();
	cfg.channel[0].ps_tx_gain = -3;
	cfg.channel[0].ps_rx_gain = -9;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	c = hop_find(H_CHAN_CFG);
	CHECK(c->chan.slic_gain_tx == -6);
	CHECK(c->chan.slic_gain_rx == -18);

	CASE("and the DSP gains stay untouched");
	CHECK(c->chan.gain_tx == 0);
	CHECK(c->chan.gain_rx == 0);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("the loss plan and the analog pair do not interact");
	cfg_default();
	cfg.channel[0].ps_tx_gain = -3;
	cfg.channel[0].ps_rx_gain = -9;
	cfg.channel[0].half_db_rx_loss = true;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	c = hop_find(H_CHAN_CFG);
	CHECK(c->chan.gain_tx == -1);
	CHECK(c->chan.slic_gain_rx == -18);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("every line pushes its own channel view");
	cfg_default();
	cfg.channel[1].enabled = true;
	cfg.channel[1].lec_control = true;
	cfg.channel[1].lec_control_set = true;
	cfg.channel[1].lec_enable = false;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(hop_last(H_CHAN_CFG)->chan.ec_enable);
	inject_hook(1, HAL_HOOK_SEIZE, 0);
	CHECK_UINT(hop_last(H_CHAN_CFG)->line, 1);
	CHECK(!hop_last(H_CHAN_CFG)->chan.ec_enable);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	inject_hook(1, HAL_HOOK_RELEASE, 0);

	/*
	 * The view rides the staged copy, like every other channel field: the
	 * helper reads the live copy, and a reload lands at the next rest
	 * state. A canceller that moved mid-call would change the audio of a
	 * call already in progress.
	 */
	CASE("a reload does not push a channel view of its own");
	cfg_default();
	cfg.channel[0].lec_control = true;
	cfg.channel[0].lec_control_set = true;
	cfg.channel[0].ps_tx_gain = -3;
	cfg.channel[0].ps_rx_gain = -9;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	CHECK(state_is(0, "COLLECTING"));
	CHECK(hop_last(H_CHAN_CFG)->chan.ec_enable);
	cfg.channel[0].lec_enable = false;
	lines_reload(&cfg);
	CHECK_UINT(hop_count(H_CHAN_CFG), 1);

	/* The staged view lands at the rest state, and the record still holds
	 * the old push, so the row can say the DSP is behind the config. */
	CASE("the applied record reads pending once the line comes to rest");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(hop_count(H_CHAN_CFG), 1);
	line_lec_view_get(0, &lv);
	CHECK(lv.applied.valid);
	CHECK(lv.applied.ec);
	CHECK(lv.applied.pending);
	CHECK(!lv.config.enable);

	CASE("and the flipped canceller reaches the backend at the NEXT seize");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK_UINT(hop_count(H_CHAN_CFG), 2);
	CHECK(!hop_last(H_CHAN_CFG)->chan.ec_enable);
	CHECK(hop_last(H_CHAN_CFG)->chan.slic_gain_tx == -6);
	CHECK(hop_last(H_CHAN_CFG)->chan.slic_gain_rx == -18);

	CASE("and the record follows that push, with nothing left pending");
	line_lec_view_get(0, &lv);
	CHECK(lv.applied.valid);
	CHECK(!lv.applied.ec);
	CHECK(!lv.applied.pending);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	/*
	 * The answer-tone disabler travels on the same push and is part of the
	 * comparison. A pending test that read the canceller pair alone would
	 * call a staged disabler rule current, and the row would say the DSP
	 * holds a release rule it was never sent.
	 */
	CASE("a staged tone option reads pending until the next seize");
	cfg_default();
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	line_lec_view_get(0, &lv);
	CHECK(!lv.applied.tone_latch);
	CHECK(!lv.applied.pending);
	cfg.channel[0].lec_tone_latch = true;
	lines_reload(&cfg);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	line_lec_view_get(0, &lv);
	CHECK(lv.applied.pending);

	CASE("and the record keeps the old rule until that seize runs");
	CHECK(!lv.applied.tone_latch);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	line_lec_view_get(0, &lv);
	CHECK(lv.applied.tone_latch);
	CHECK(!lv.applied.pending);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	/* the retry of hal.h section 3 carries the same view, not a narrower
	 * one, so a transient first attempt cannot leave the canceller behind */
	CASE("the transient retry pushes the same view again");
	cfg_default();
	cfg.channel[0].half_db_rx_loss = true;
	cfg.channel[0].ps_tx_gain = -3;
	cfg.channel[0].ps_rx_gain = -9;
	fixture_start();
	chan_cfg_transient = 1;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK_UINT(hop_count(H_CHAN_CFG), 2);
	CHECK(hop_last(H_CHAN_CFG)->chan.gain_tx == -1);
	CHECK(hop_last(H_CHAN_CFG)->chan.slic_gain_tx == -6);
	CHECK(hop_last(H_CHAN_CFG)->chan.slic_gain_rx == -18);
	CHECK(hop_last(H_CHAN_CFG)->chan.ec_enable);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
}

static void t_ring(void)
{
	struct hop *r;
	struct line_view v;

	cfg_default();
	fixture_start();

	CASE("an incoming call rings the line");
	inject_incoming(0, 0, "5551000", "Alice", 0);
	CHECK(state_is(0, "RINGING"));
	CHECK(hop_has(H_RING_START));

	CASE("the default profile is the 2 s on 4 s off cadence");
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence_count, 1);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);
	CHECK_UINT(r->ring.cadence[0].off_ms, 4000);

	CASE("the ring runs until it is stopped");
	CHECK_UINT(r->ring.duration_ms, 0);

	CASE("the caller id of the offer rides the ring");
	CHECK(r->ring.cid_present);
	CHECK(strcmp(r->ring.cid.number, "5551000") == 0);
	CHECK(strcmp(r->ring.cid.name, "Alice") == 0);

	CASE("the caller id is type 1 Bellcore FSK with an empty date");
	CHECK_UINT(r->ring.cid.format, HAL_CID_BELLCORE_FSK);
	CHECK(r->ring.cid.datetime[0] == 0);

	CASE("the status dump reports the ring");
	view_get(0, &v);
	CHECK(v.ringing);

	CASE("an answered inbound call stops the ring");
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(state_is(0, "CONNECTED"));
	CHECK(hop_has(H_RING_STOP));
	CHECK(cop_has(C_ANSWER));

	CASE("a digit in a call becomes DTMF on the active slot");
	cops_reset();
	inject_digit(0, '4', false);
	CHECK(cop_has(C_DTMF));
	CHECK_UINT(cop_find(C_DTMF)->slot, 0);
	CHECK_UINT(cop_find(C_DTMF)->val, '4');

	CASE("on hook hangs the call up");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(cop_has(C_HANGUP));
	CHECK(state_is(0, "IDLE"));

	CASE("a two burst distinctive ring takes the dr2 pattern");
	fixture_start();
	inject_incoming(0, 0, "5551000", "", 2);
	CHECK(hop_has(H_RING_START));
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence_count, 2);
	CHECK_UINT(r->ring.cadence[0].on_ms, 800);
	CHECK_UINT(r->ring.cadence[0].off_ms, 400);
	CHECK_UINT(r->ring.cadence[1].on_ms, 800);
	CHECK_UINT(r->ring.cadence[1].off_ms, 4000);

	CASE("the CID break index follows the two burst cycle, not the first burst");
	CHECK_UINT(r->ring.cid_at_ring, 2);
	CHECK_UINT(r->ring.cid_wait_ms, 500);

	CASE("a three burst distinctive ring takes the dr3 pattern");
	fixture_start();
	inject_incoming(0, 0, "5551000", "", 3);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence_count, 3);
	CHECK_UINT(r->ring.cadence[2].off_ms, 4000);

	CASE("the CID break index follows the three burst cycle");
	CHECK_UINT(r->ring.cid_at_ring, 3);
	CHECK_UINT(r->ring.cid_wait_ms, 500);

	CASE("a ring id the table does not carry falls back to the default");
	fixture_start();
	inject_incoming(0, 0, "5551000", "", 9);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence_count, 1);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);

	CASE("the one burst default keeps the CID break index at one");
	CHECK_UINT(r->ring.cid_at_ring, 1);

	/*
	 * With cid_remote_enable on, every inbound call carries a record, even
	 * from these callers: GR-30 says why a field is empty with a reason
	 * letter in the field, and a display that shows nothing at all cannot
	 * say it. The two reasons are different, so the two cases below are
	 * different too.
	 */
	CASE("an anonymous caller rings WITH a record whose fields are withheld");
	fixture_start();
	rec_cb.incoming(0, 0, "", "", true, true, 0);
	r = hop_find(H_RING_START);
	CHECK(r->ring.cid_present);
	CHECK(r->ring.cid.number_private);
	CHECK(r->ring.cid.name_private);
	CHECK(strcmp(r->ring.cid.blockinfo, "") == 0);

	CASE("a caller id longer than the HAL field is truncated");
	fixture_start();
	inject_incoming(0, 0, "0123456789012345678901234567890123456789", "", 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(strlen(r->ring.cid.number), HAL_CID_STR_MAX - 1);

	CASE("a withheld number with a name still delivers a caller id");
	fixture_start();
	rec_cb.incoming(0, 0, "5551000", "Alice", true, false, 0);
	r = hop_find(H_RING_START);
	CHECK(r->ring.cid_present);
	CHECK(strcmp(r->ring.cid.name, "Alice") == 0);
	CHECK(r->ring.cid.number_private);
	CHECK(!r->ring.cid.name_private);

	/* the daemon keeps the number the caller withheld; only the backend
	 * render hides it, and the backend needs the string to be there */
	CASE("the withheld number itself still reaches the backend record");
	CHECK(strcmp(r->ring.cid.number, "5551000") == 0);

	CASE("a caller with neither field carries a record with the O reason");
	fixture_start();
	inject_incoming(0, 0, "", "", 0);
	r = hop_find(H_RING_START);
	CHECK(r->ring.cid_present);
	CHECK(strcmp(r->ring.cid.blockinfo, "O") == 0);
	CHECK(!r->ring.cid.number_private);
	CHECK(!r->ring.cid.name_private);

	/*
	 * The absence reason is per field: one reason for the number and
	 * another for the name. Half an identity
	 * still leaves half a display empty, and an empty half with no reason in
	 * it says nothing at all.
	 */
	CASE("a caller who sent only a name carries the absence reason for the number");
	fixture_start();
	inject_incoming(0, 0, "", "Alice", 0);
	r = hop_find(H_RING_START);
	CHECK(r->ring.cid_present);
	CHECK(strcmp(r->ring.cid.name, "Alice") == 0);
	CHECK(r->ring.cid.number[0] == 0);
	CHECK(strcmp(r->ring.cid.blockinfo, "O") == 0);

	CASE("a caller who sent only a number carries it for the name");
	fixture_start();
	inject_incoming(0, 0, "5551000", "", 0);
	r = hop_find(H_RING_START);
	CHECK(r->ring.cid_present);
	CHECK(strcmp(r->ring.cid.number, "5551000") == 0);
	CHECK(r->ring.cid.name[0] == 0);
	CHECK(strcmp(r->ring.cid.blockinfo, "O") == 0);

	CASE("a caller who sent both fields carries no absence reason");
	fixture_start();
	inject_incoming(0, 0, "5551000", "Alice", 0);
	r = hop_find(H_RING_START);
	CHECK(r->ring.cid_present);
	CHECK(strcmp(r->ring.cid.blockinfo, "") == 0);

	/* a caller who withheld one field has an identity for it, so that field
	 * is not absent and raises no reason of its own */
	CASE("a withheld field is not an absent one");
	fixture_start();
	rec_cb.incoming(0, 0, "5551000", "", true, true, 0);
	r = hop_find(H_RING_START);
	CHECK(strcmp(r->ring.cid.blockinfo, "") == 0);
	fixture_start();
	rec_cb.incoming(0, 0, "5551000", "", true, false, 0);
	r = hop_find(H_RING_START);
	CHECK(strcmp(r->ring.cid.blockinfo, "O") == 0);
}

/* one template of the region read-back, as the backend would answer it */
static void cfg_pattern_set(unsigned int idx, uint16_t on_ms, uint16_t off_ms)
{
	struct hal_region_ring *pattern = &fake_region.ring[idx];

	memset(pattern, 0, sizeof(*pattern));
	pattern->cadence_count = 1;
	pattern->cadence[0].on_ms = on_ms;
	pattern->cadence[0].off_ms = off_ms;
}

/*
 * A region change the case drives to completion: the reload submits the job
 * for the new selector and the completion lands the read-back the case
 * seeded in fake_region, at the rest state of each line.
 */
static void region_change(const char *region)
{
	snprintf(cfg.region, sizeof(cfg.region), "%s", region);
	lines_reload(&cfg);
	job_complete(HAL_OK);
}

/*
 * The join from a ring template id to a cadence. A template that the
 * read-back defines wins for every id from 0 to 7. An id from 1 to 5 that the
 * read-back left empty takes the built-in GR-506 row. Everything else ends at
 * the default shape. No id is clamped, so all eight rows are reachable, and
 * the splash comes from the fallback alone.
 */
static void t_ring_pattern(void)
{
	struct hal_region_ring *pattern;
	struct hop *r;

	CASE("a template the read-back defines wins over the built-in one");
	cfg_default();
	cfg_pattern_set(2, 1200, 2400);
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "5551000", "", 2);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence_count, 1);
	CHECK_UINT(r->ring.cadence[0].on_ms, 1200);
	CHECK_UINT(r->ring.cadence[0].off_ms, 2400);

	CASE("all three pairs of a template reach the profile in order");
	cfg_default();
	pattern = &fake_region.ring[3];
	memset(pattern, 0, sizeof(*pattern));
	pattern->cadence_count = 3;
	pattern->cadence[0].on_ms = 100;
	pattern->cadence[0].off_ms = 150;
	pattern->cadence[1].on_ms = 200;
	pattern->cadence[1].off_ms = 250;
	pattern->cadence[2].on_ms = 300;
	pattern->cadence[2].off_ms = 3500;
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "5551000", "", 3);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence_count, 3);
	CHECK_UINT(r->ring.cadence[0].on_ms, 100);
	CHECK_UINT(r->ring.cadence[1].off_ms, 250);
	CHECK_UINT(r->ring.cadence[2].on_ms, 300);
	CHECK_UINT(r->ring.cadence[2].off_ms, 3500);

	CASE("an unset id from 1 to 5 falls to the built-in GR-506 row");
	cfg_default();
	fixture_start();
	inject_incoming(0, 0, "5551000", "", 2);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence_count, 2);
	CHECK_UINT(r->ring.cadence[0].on_ms, 800);
	CHECK_UINT(r->ring.cadence[1].off_ms, 4000);

	CASE("the dr5 splash comes from that fallback table only");
	fixture_start();
	inject_incoming(0, 0, "5551000", "", 5);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence_count, 1);
	CHECK_UINT(r->ring.cadence[0].on_ms, 500);
	CHECK_UINT(r->ring.cadence[0].off_ms, 4000);

	CASE("a configured pattern5 replaces the splash");
	cfg_default();
	cfg_pattern_set(5, 1000, 2000);
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "5551000", "", 5);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 1000);
	CHECK_UINT(r->ring.cadence[0].off_ms, 2000);

	CASE("row 6 and row 7 ring too: all eight are honored");
	cfg_default();
	cfg_pattern_set(6, 610, 620);
	cfg_pattern_set(7, 710, 720);
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "5551000", "", 6);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 610);
	CHECK_UINT(r->ring.cadence[0].off_ms, 620);
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "5551000", "", 7);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 710);
	CHECK_UINT(r->ring.cadence[0].off_ms, 720);

	CASE("an id above the built-in table with no region row rings the default");
	cfg_default();
	fixture_start();
	inject_incoming(0, 0, "5551000", "", 6);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence_count, 1);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);
	CHECK_UINT(r->ring.cadence[0].off_ms, 4000);

	CASE("dr8 and dr9 fall to the default shape as well");
	fixture_start();
	inject_incoming(0, 0, "5551000", "", 8);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence_count, 1);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);
	fixture_start();
	inject_incoming(0, 0, "5551000", "", 9);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence_count, 1);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);

	CASE("with template r1 defined the default shape is r1");
	cfg_default();
	cfg_pattern_set(0, 700, 2100);
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "5551000", "", 9);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 700);
	CHECK_UINT(r->ring.cadence[0].off_ms, 2100);

	CASE("and a call with no template at all takes it too");
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "5551000", "", 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 700);
	CHECK_UINT(r->ring.cadence[0].off_ms, 2100);

	CASE("a region load cannot move a cadence under a line that is not at rest");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	cfg_pattern_set(2, 1200, 2400);
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	inject_hook(0, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(0, 0, true);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "HOLDING"));
	cfg_pattern_set(2, 900, 900);
	region_change("GB");
	hops_reset();
	inject_incoming(0, 1, "5552000", "", 2);
	CHECK(state_is(0, "RINGING"));
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 1200);
	CHECK_UINT(r->ring.cadence[0].off_ms, 2400);

	CASE("the staged cadence lands once the line reaches IDLE");
	rec_cb.remote_cancel(0, 1);
	rec_cb.remote_bye(0, 0);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	hops_reset();
	inject_incoming(0, 0, "5551000", "", 2);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 900);
	CHECK_UINT(r->ring.cadence[0].off_ms, 900);

	CASE("a region load with the line at rest lands it at once");
	cfg_default();
	cfg_pattern_set(2, 1200, 2400);
	fixture_start();
	region_ready();
	cfg_pattern_set(2, 950, 1950);
	region_change("GB");
	hops_reset();
	inject_incoming(0, 0, "5551000", "", 2);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 950);
	CHECK_UINT(r->ring.cadence[0].off_ms, 1950);
}

/* one distinctive-ring number slot of a channel */
static void cfg_ring_number(struct cfg_channel *ch, unsigned int slot,
			    const char *number, unsigned int def)
{
	snprintf(ch->distinctive_ring[slot], sizeof(ch->distinctive_ring[slot]),
		 "%s", number);
	ch->distinctive_ring_def[slot] = def;
	ch->distinctive_ring_def_set[slot] = true;
}

/*
 * The caller number picks a template out of the ten slots, and an Alert-Info
 * that the offer carried overrides that choice. The comparison is full string
 * and exact, with no wildcard and no prefix. That is the rule that the block
 * and allow lists use.
 */
static void t_distinctive_ring(void)
{
	char number[CFG_STR_MAX];
	char cut[CFG_STR_MAX];
	struct hop *r;

	CASE("an exact number match rings the template its slot names");
	cfg_default();
	cfg.channel[0].distinctive_ring_enable = true;
	cfg_ring_number(&cfg.channel[0], 0, "5551000", 2);
	cfg_pattern_set(2, 1200, 2400);
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "5551000", "", 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 1200);
	CHECK_UINT(r->ring.cadence[0].off_ms, 2400);

	CASE("a caller no slot carries rings the default");
	fixture_start();
	inject_incoming(0, 0, "5559999", "", 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);
	CHECK_UINT(r->ring.cadence[0].off_ms, 4000);

	CASE("a prefix of the slot is not a match");
	fixture_start();
	inject_incoming(0, 0, "555", "", 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);

	CASE("nor is a number the slot is a prefix of");
	fixture_start();
	inject_incoming(0, 0, "55510001", "", 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);

	CASE("distinctive_ring_enable off ignores the whole table");
	cfg_default();
	cfg.channel[0].distinctive_ring_enable = false;
	cfg_ring_number(&cfg.channel[0], 0, "5551000", 2);
	cfg_pattern_set(2, 1200, 2400);
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "5551000", "", 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);

	CASE("a slot without a definition never participates");
	cfg_default();
	cfg.channel[0].distinctive_ring_enable = true;
	cfg_ring_number(&cfg.channel[0], 0, "5551000", 2);
	cfg.channel[0].distinctive_ring_def_set[0] = false;
	cfg_pattern_set(2, 1200, 2400);
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "5551000", "", 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);

	CASE("an empty slot matches no caller, not even one with no number");
	cfg_default();
	cfg.channel[0].distinctive_ring_enable = true;
	cfg.channel[0].distinctive_ring_def[1] = 2;
	cfg.channel[0].distinctive_ring_def_set[1] = true;
	cfg_pattern_set(2, 1200, 2400);
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "", "", 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);

	CASE("the first matching slot wins");
	cfg_default();
	cfg.channel[0].distinctive_ring_enable = true;
	cfg_ring_number(&cfg.channel[0], 0, "5551000", 2);
	cfg_ring_number(&cfg.channel[0], 3, "5551000", 4);
	cfg_pattern_set(2, 1200, 2400);
	cfg_pattern_set(4, 400, 4400);
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "5551000", "", 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 1200);
	CHECK_UINT(r->ring.cadence[0].off_ms, 2400);

	CASE("a withheld caller still matches: privacy gates the display only");
	cfg_default();
	cfg.channel[0].distinctive_ring_enable = true;
	cfg_ring_number(&cfg.channel[0], 0, "5551000", 2);
	cfg_pattern_set(2, 1200, 2400);
	fixture_start();
	region_ready();
	rec_cb.incoming(0, 0, "5551000", "Alice", true, true, 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 1200);
	CHECK(r->ring.cid.number_private);

	CASE("an Alert-Info id overrides the number match");
	fixture_start();
	inject_incoming(0, 0, "5551000", "", 3);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence_count, 3);
	CHECK_UINT(r->ring.cadence[0].on_ms, 400);
	CHECK_UINT(r->ring.cadence[0].off_ms, 200);

	CASE("an offer with no Alert-Info leaves the number match standing");
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "5551000", "", 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 1200);

	/* the display record is cut to the HAL field; the match is not, or a long
	 * number would ring the template of the slot its truncation equals */
	CASE("a caller number longer than the CID field does not match its truncation");
	snprintf(number, sizeof(number), "%s", "0123456789012345678901234567890123456789");
	snprintf(cut, sizeof(cut), "%.*s", HAL_CID_STR_MAX - 1, number);
	cfg_default();
	cfg.channel[0].distinctive_ring_enable = true;
	cfg_ring_number(&cfg.channel[0], 0, cut, 2);
	cfg_pattern_set(2, 1200, 2400);
	fixture_start();
	region_ready();
	inject_incoming(0, 0, number, "", 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(strlen(r->ring.cid.number), HAL_CID_STR_MAX - 1);
	CHECK(strcmp(r->ring.cid.number, cut) == 0);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);
	CHECK_UINT(r->ring.cadence[0].off_ms, 4000);

	CASE("the slot the whole number equals still rings its template");
	cfg_default();
	cfg.channel[0].distinctive_ring_enable = true;
	cfg_ring_number(&cfg.channel[0], 0, number, 2);
	cfg_pattern_set(2, 1200, 2400);
	fixture_start();
	region_ready();
	inject_incoming(0, 0, number, "", 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 1200);
	CHECK_UINT(r->ring.cadence[0].off_ms, 2400);

	CASE("the template of one call does not leak into the next");
	fixture_start();
	inject_incoming(0, 0, "5551000", "", 3);
	rec_cb.remote_cancel(0, 0);
	CHECK(state_is(0, "IDLE"));
	hops_reset();
	inject_incoming(0, 0, "5559999", "", 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);
}

/*
 * The three hook thresholds /etc/config/voip drives reach the backend
 * once per line at bring-up, and again only when a reload moves them. The
 * seize threshold and pulse detection are not in the struct at all, because no
 * config source drives them.
 */
static void t_hook_timing(void)
{
	unsigned int calls;

	CASE("the compiled default window reaches the backend at bring-up");
	cfg_default();
	fixture_start();
	CHECK_UINT(hook_ts_calls[0], 1);
	CHECK_UINT(hook_ts[0].flash_min_ms, 300);
	CHECK_UINT(hook_ts[0].flash_max_ms, 1300);
	CHECK_UINT(hook_ts[0].release_min_ms, 1301);

	CASE("every line carries its own window");
	CHECK_UINT(hook_ts_calls[1], 1);
	CHECK_UINT(hook_ts[1].flash_min_ms, 300);
	CHECK_UINT(hook_ts[1].flash_max_ms, 1300);
	CHECK_UINT(hook_ts[1].release_min_ms, 1301);

	CASE("the read-back window replaces it on every line once the region loads");
	fake_region.hook_flash_min_ms = 320;
	fake_region.hook_flash_max_ms = 1320;
	fake_region.hook_release_min_ms = 1321;
	region_ready();
	CHECK_UINT(hook_ts_calls[0], 2);
	CHECK_UINT(hook_ts[0].flash_min_ms, 320);
	CHECK_UINT(hook_ts[0].flash_max_ms, 1320);
	CHECK_UINT(hook_ts[0].release_min_ms, 1321);
	CHECK_UINT(hook_ts_calls[1], 2);
	CHECK_UINT(hook_ts[1].flash_min_ms, 320);

	CASE("a read-back that repeats the default pushes nothing");
	cfg_default();
	fixture_start();
	region_ready();
	CHECK_UINT(hook_ts_calls[0], 1);
	CHECK_UINT(hook_ts_calls[1], 1);

	/*
	 * The effective release floor. The backend raises a release window that
	 * sits below the flash window before it programs the DSP, because a
	 * release that would have to fire inside a flash can never fire; the
	 * shell applies the same floor, so what it pushes and what it reports
	 * the forward disconnect band against are the window the backend really
	 * classifies with.
	 */
	CASE("a release window below the flash window is raised to it");
	cfg_default();
	fake_region.hook_release_min_ms = 900;
	fixture_start();
	region_ready();
	CHECK_UINT(hook_ts[0].flash_max_ms, 1300);
	CHECK_UINT(hook_ts[0].release_min_ms, 1300);

	CASE("and a release window above it is pushed as read");
	cfg_default();
	fake_region.hook_release_min_ms = 1400;
	fixture_start();
	region_ready();
	CHECK_UINT(hook_ts[0].release_min_ms, 1400);

	cfg_default();
	fake_region.hook_flash_min_ms = 320;
	fake_region.hook_flash_max_ms = 1320;
	fake_region.hook_release_min_ms = 1321;
	fixture_start();
	region_ready();
	calls = hook_ts_calls[0];

	CASE("a reload that leaves the window alone pushes nothing");
	lines_reload(&cfg);
	CHECK_UINT(hook_ts_calls[0], calls);
	CHECK_UINT(hook_ts_calls[1], calls);

	CASE("a region load that moves the window pushes the new one to every line");
	fake_region.hook_flash_max_ms = 900;
	fake_region.hook_release_min_ms = 1500;
	region_change("GB");
	CHECK_UINT(hook_ts_calls[0], calls + 1);
	CHECK_UINT(hook_ts[0].flash_max_ms, 900);
	CHECK_UINT(hook_ts[0].release_min_ms, 1500);
	CHECK_UINT(hook_ts_calls[1], calls + 1);
	CHECK_UINT(hook_ts[1].flash_max_ms, 900);

	CASE("the push waits for the staged copy, like every other channel field");
	cfg_default();
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '1', false);
	CHECK(state_is(0, "COLLECTING"));
	fake_region.hook_flash_min_ms = 450;
	region_change("GB");
	CHECK_UINT(hook_ts_calls[0], 1);

	CASE("and lands with it at the next rest state");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(hook_ts_calls[0], 2);
	CHECK_UINT(hook_ts[0].flash_min_ms, 450);

	CASE("a backend that refuses the push leaves the line in service");
	cfg_default();
	fixture_start();
	region_ready();
	hook_ts_status = HAL_FATAL;
	fake_region.hook_flash_min_ms = 460;
	region_change("GB");
	CHECK_UINT(hook_ts_calls[0], 2);
	CHECK(state_is(0, "IDLE"));
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	hook_ts_status = HAL_OK;

	/*
	 * hal.h section 3. A dropped transient is invisible without the retry:
	 * the staged view already records the window as applied, so no later
	 * reload of the same config pushes it again and the hardware keeps the
	 * thresholds it had.
	 */
	CASE("a transient push is retried once, and the window lands");
	cfg_default();
	fixture_start();
	region_ready();
	hook_ts_transient = 1;
	fake_region.hook_flash_min_ms = 470;
	region_change("GB");
	CHECK_UINT(hook_ts_calls[0], 3);
	CHECK_UINT(hook_ts[0].flash_min_ms, 470);
	CHECK_UINT(hook_ts_transient, 0);

	CASE("a retry that is transient too warns and leaves the line in service");
	cfg_default();
	fixture_start();
	region_ready();
	hook_ts_transient = 2;
	fake_region.hook_flash_min_ms = 480;
	region_change("GB");
	CHECK_UINT(hook_ts_calls[0], 3);
	CHECK_UINT(hook_ts_transient, 0);
	CHECK(state_is(0, "IDLE"));
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(state_is(0, "OFFHOOK_DIAL"));

	/* libudsp seeds every line's windows from the region file itself; a
	 * push there would only latch a stale window against the next swap */
	CASE("a backend that programs its own region windows receives no push");
	cfg_default();
	fake_features_extra = HAL_FEAT_REGION_HOOK;
	fixture_start();
	region_ready();
	CHECK_UINT(hook_ts_calls[0], 0);
	CHECK_UINT(hook_ts_calls[1], 0);
	fake_region.hook_flash_min_ms = 35;
	region_change("GB");
	CHECK_UINT(hook_ts_calls[0], 0);
	fake_features_extra = 0;

	/* a 1-FXS board carrying a 2 channel config: the second section names a
	 * port that does not exist, and the backend refuses the op for it */
	CASE("a line the board does not carry is never pushed at all");
	cfg_default();
	fixture_start();
	lines_stop();
	fake_cap.lines = 1;
	memset(hook_ts_calls, 0, sizeof(hook_ts_calls));
	lines_bring_up();
	CHECK_UINT(hook_ts_calls[0], 1);
	CHECK_UINT(hook_ts_calls[1], 0);
	fake_cap.lines = 2;
}

/*
 * A bring-up the case controls. fixture_start() clears the recorder knobs
 * before it runs lines_init(), so a case that needs the hook query to answer
 * off hook, or to fail, re-enters the bring-up itself with the knobs already
 * set. It is the shape the narrow-board case of t_hook_timing above uses.
 */
static void boot_run(void)
{
	lines_stop();
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	hops_reset();
	cops_reset();
	lines_bring_up();
}

/*
 * The boot off hook is deterministic. lines_start() ends by reading the hook
 * state of every line in service. A handset that was already lifted when the
 * daemon started therefore seizes on every boot.
 *
 * The start-up hook report and the dsp_start completion arrive in no fixed
 * order. The machines are wired before the event loop starts, so the report
 * seizes the line in either order. The read still derives the state for a
 * boot that produced no hook report at all. A report that already seized the
 * line leaves it in OFFHOOK_DIAL, or in CLEARING for a line that cannot carry a
 * dial. The matrix ignores the second SEIZE in both.
 */
static void t_boot_seize(void)
{
	struct line_view v;

	CASE("a line off hook at bring-up ends in OFFHOOK_DIAL");
	cfg_default();
	fixture_start();
	boot_offhook[0] = true;
	boot_run();
	CHECK_UINT(hook_query_calls[0], 1);
	CHECK(state_is(0, "OFFHOOK_DIAL"));

	/* the injected seize took the ordinary intake, which is what arms the
	 * digit detector: a state written straight into the machine would not */
	CASE("the boot seize arms the digit detector like a lifted handset");
	CHECK(hop_has(H_CHAN_CFG));
	CHECK_UINT(hop_find(H_CHAN_CFG)->chan.detect_mask,
		   HAL_DETECT_DTMF | HAL_DETECT_PULSE | HAL_DETECT_MODEM);

	CASE("and the dial tone of the boot seize reaches the line");
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);
	CHECK_UINT(hop_last(H_TONE)->line, 0);

	/*
	 * The start-up hook report that won the race delivers the same seize a
	 * moment later. Its cell in OFFHOOK_DIAL is the duplicate hook report
	 * ignore, so the machine needs no boot flag of its own: nothing moves,
	 * no tone is re-issued and no call is placed.
	 */
	CASE("a real seize behind the boot one is the duplicate hook ignore");
	hops_reset();
	cops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK_UINT(hop_count(H_TONE), 0);
	CHECK(!cop_has(C_DIAL));
	view_get(0, &v);
	CHECK(strcmp(v.digits, "") == 0);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	/*
	 * The hotline entry of OFFHOOK_DIAL dials inside the seize, so a boot
	 * that finds the handset lifted places the call at once.
	 */
	CASE("a hotline line dials on the boot seize");
	cfg_default();
	cfg.channel[0].warmline_enable = true;
	cfg.channel[0].warmline_timeout_s = 0;
	snprintf(cfg.channel[0].warmline_number, sizeof(cfg.channel[0].warmline_number),
		 "5551234");
	fixture_start();
	boot_offhook[0] = true;
	boot_run();
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, "5551234") == 0);
	CHECK(state_is(0, "CALLING"));

	CASE("an on-hook line is queried and seizes nothing");
	cfg_default();
	warn_capture_begin();
	fixture_start();
	boot_run();
	CHECK_UINT(hook_query_calls[0], 1);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(hop_count(H_CHAN_CFG), 0);
	CHECK(!cop_has(C_DIAL));
	/* And it releases nothing either, which is the second arm of the
	 * sync: the boot machine is at IDLE, so the on-hook answer it reads
	 * describes a line that is already there. A release injected here would
	 * be a matrix no-op and invisible, so the log line is the pin. */
	CHECK_UINT(warn_capture_count("releasing"), 0);
	warn_capture_end();

	/* line 1 is the disabled channel of the fixture config, so the bring-up
	 * forces it out of service before the closing query runs */
	CASE("an out of service line is not queried at all");
	cfg_default();
	fixture_start();
	boot_offhook[1] = true;
	boot_run();
	CHECK(state_is(1, "OOS"));
	CHECK_UINT(hook_query_calls[1], 0);
	CHECK_UINT(hop_count(H_CHAN_CFG), 0);
	CHECK_UINT(hop_count(H_TONE), 0);

	/*
	 * The other half of the skip, and the one a service test written from
	 * the config alone would miss: a 1-FXS board carrying a 2 channel config
	 * has a section for a port that does not exist, and the query for it
	 * would be refused by the backend anyway.
	 */
	CASE("a port the board does not carry is not queried either");
	cfg_default();
	cfg.channel[1].enabled = true;
	fixture_start();
	fake_cap.lines = 1;
	boot_offhook[1] = true;
	boot_run();
	CHECK(state_is(1, "OOS"));
	CHECK_UINT(hook_query_calls[1], 0);
	CHECK_UINT(hook_query_calls[0], 1);
	fake_cap.lines = 2;

	/*
	 * The line keeps the outcome of the race, which is the state nobody
	 * read; a fabricated one would be worse. The recorder answers off hook
	 * while it refuses, so the status has to be read before the answer.
	 */
	CASE("a query the backend refuses seizes nothing");
	cfg_default();
	fixture_start();
	boot_offhook[0] = true;
	hook_query_status[0] = HAL_FATAL;
	boot_run();
	CHECK_UINT(hook_query_calls[0], 1);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(hop_count(H_CHAN_CFG), 0);
	CHECK(!cop_has(C_DIAL));

	/* one refused line is one refused line: the boot of every other line
	 * still has to be derived, so the loop carries on through it */
	CASE("a refusal on one line does not stop the closing loop");
	cfg_default();
	cfg.channel[1].enabled = true;
	fixture_start();
	hook_query_status[0] = HAL_FATAL;
	boot_offhook[1] = true;
	boot_run();
	CHECK_UINT(hook_query_calls[0], 1);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(hook_query_calls[1], 1);
	CHECK(state_is(1, "OFFHOOK_DIAL"));

	/*
	 * hal.h section 3. This query runs at the instant the DSP finished its
	 * bring-up, which is where a busy answer is most likely, and a dropped
	 * transient would hand the line back to the race the closing query
	 * exists to close.
	 */
	CASE("a transient refusal is retried once, and the line then seizes");
	cfg_default();
	fixture_start();
	boot_offhook[0] = true;
	hook_query_transient[0] = 1;
	boot_run();
	CHECK_UINT(hook_query_calls[0], 2);
	CHECK_UINT(hook_query_transient[0], 0);
	CHECK(state_is(0, "OFFHOOK_DIAL"));

	CASE("a retry that is transient too seizes nothing");
	cfg_default();
	fixture_start();
	boot_offhook[0] = true;
	hook_query_transient[0] = 2;
	boot_run();
	CHECK_UINT(hook_query_calls[0], 2);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(hop_count(H_CHAN_CFG), 0);

	/* a registrar that has not resolved at bring-up (the boot query comment
	 * in line/line.c) */
	CASE("no dial target: a line off hook at bring-up gets reorder");
	cfg_default();
	fixture_start();
	dial_ready_answer = false;
	boot_offhook[0] = true;
	boot_run();
	CHECK(state_is(0, "CLEARING"));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);
	dial_ready_answer = true;

	/* the knobs outlive this suite like the message store does, so the
	 * cases that follow start from a board that boots on hook */
	memset(boot_offhook, 0, sizeof(boot_offhook));
	memset(hook_query_status, 0, sizeof(hook_query_status));
	memset(hook_query_transient, 0, sizeof(hook_query_transient));
}

/*
 * Entering service derives the hook state again too. A line that came up out
 * of service never reached the closing query of lines_start(). Without this
 * query, a handset lifted through the whole boot would leave the machine IDLE
 * when a reload put that line in service. That is the class of the boot
 * seize, reached through a config move and not through the start-up race.
 *
 * The knobs are the ones that the boot suite above uses, and the recorder
 * counts every query. A case therefore checks both the seize that must arrive
 * and the query that must never leave for a line whose service did not move.
 */
static void t_reload_seize(void)
{
	CASE("a line the reload takes into service is queried once");
	cfg_default();
	fixture_start();
	CHECK(state_is(1, "OOS"));
	CHECK_UINT(hook_query_calls[1], 0);
	boot_offhook[1] = true;
	cfg.channel[1].enabled = true;
	lines_reload(&cfg);
	CHECK_UINT(hook_query_calls[1], 1);

	CASE("and the handset that was lifted all along seizes it");
	CHECK(state_is(1, "OFFHOOK_DIAL"));

	/* the injected seize took the ordinary intake, which is what arms the
	 * digit detector: a state written straight into the machine would not */
	CASE("the reload seize arms the digit detector like a lifted handset");
	CHECK(hop_has(H_CHAN_CFG));
	CHECK_UINT(hop_last(H_CHAN_CFG)->line, 1);
	CHECK_UINT(hop_last(H_CHAN_CFG)->chan.detect_mask,
		   HAL_DETECT_DTMF | HAL_DETECT_PULSE | HAL_DETECT_MODEM);

	CASE("and the dial tone of the reload seize reaches the line");
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);
	CHECK_UINT(hop_last(H_TONE)->line, 1);
	inject_hook(1, HAL_HOOK_RELEASE, 0);

	/*
	 * On this path too, the hotline entry of OFFHOOK_DIAL dials inside the
	 * seize, and the staged view the reload carries is already applied when
	 * the query runs, so the call goes to the number this reload set.
	 */
	CASE("a hotline line dials when the reload takes it into service");
	cfg_default();
	fixture_start();
	CHECK(state_is(1, "OOS"));
	boot_offhook[1] = true;
	/* the hotline number arrives with this reload and with nothing before
	 * it, so a query that ran in front of the config apply would read the
	 * old view and dial nothing */
	cfg.channel[1].warmline_enable = true;
	cfg.channel[1].warmline_timeout_s = 0;
	snprintf(cfg.channel[1].warmline_number, sizeof(cfg.channel[1].warmline_number),
		 "5551234");
	cfg.channel[1].enabled = true;
	lines_reload(&cfg);
	CHECK(cop_has(C_DIAL));
	CHECK_UINT(cop_find(C_DIAL)->line, 1);
	CHECK(strcmp(cop_find(C_DIAL)->num, "5551234") == 0);
	CHECK(state_is(1, "CALLING"));

	CASE("the same reload with the handset down injects nothing");
	cfg_default();
	fixture_start();
	cfg.channel[1].enabled = true;
	warn_capture_begin();
	lines_reload(&cfg);
	CHECK_UINT(hook_query_calls[1], 1);
	CHECK(state_is(1, "IDLE"));
	CHECK_UINT(hop_count(H_CHAN_CFG), 0);
	CHECK(!cop_has(C_DIAL));
	/* neither half of the sync fires: the line enters service at IDLE, which
	 * the release arm skips */
	CHECK_UINT(warn_capture_count("releasing"), 0);
	warn_capture_end();

	/* the state the line already holds is the subscriber's, and a query that
	 * re-derived it on every reload would seize a line nobody touched */
	CASE("a reload that leaves the service state alone queries nothing");
	cfg_default();
	fixture_start();
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	boot_offhook[0] = true;
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	lines_reload(&cfg);
	CHECK_UINT(hook_query_calls[0], 0);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(hop_count(H_CHAN_CFG), 0);

	CASE("and a line that stays out of service is not queried either");
	CHECK_UINT(hook_query_calls[1], 0);
	CHECK(state_is(1, "OOS"));

	CASE("a reload taking a line out of service queries nothing");
	cfg_default();
	fixture_start();
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	boot_offhook[0] = true;
	cfg.channel[0].enabled = false;
	lines_reload(&cfg);
	CHECK_UINT(hook_query_calls[0], 0);
	CHECK(state_is(0, "OOS"));
	CHECK_UINT(hop_count(H_CHAN_CFG), 0);

	/*
	 * The line keeps what the reload gave it, which is a machine at rest; a
	 * hook state invented here would be worse than the one nobody read. The
	 * reload path takes the same refusal shape as the boot query, so a
	 * backend that cannot answer right now costs the line nothing.
	 */
	CASE("a query the backend refuses on the reload path injects nothing");
	cfg_default();
	fixture_start();
	boot_offhook[1] = true;
	hook_query_status[1] = HAL_FATAL;
	cfg.channel[1].enabled = true;
	lines_reload(&cfg);
	CHECK_UINT(hook_query_calls[1], 1);
	CHECK(state_is(1, "IDLE"));
	CHECK_UINT(hop_count(H_CHAN_CFG), 0);
	CHECK(!cop_has(C_DIAL));

	/* hal.h section 3, through the same primitive: the reload path inherits
	 * the retry instead of carrying a query of its own */
	CASE("a transient refusal on the reload path is retried once");
	cfg_default();
	fixture_start();
	boot_offhook[1] = true;
	hook_query_transient[1] = 1;
	cfg.channel[1].enabled = true;
	lines_reload(&cfg);
	CHECK_UINT(hook_query_calls[1], 2);
	CHECK_UINT(hook_query_transient[1], 0);
	CHECK(state_is(1, "OFFHOOK_DIAL"));
	inject_hook(1, HAL_HOOK_RELEASE, 0);

	/* the knobs outlive this suite, so the cases that follow start from a
	 * board whose handsets are down and whose queries answer */
	memset(boot_offhook, 0, sizeof(boot_offhook));
	memset(hook_query_status, 0, sizeof(hook_query_status));
	memset(hook_query_transient, 0, sizeof(hook_query_transient));
}

/*
 * The off flash service of the region hands the hook flash to the network,
 * so the local flash-hold path of CONNECTED is off. The two local services
 * keep it.
 */
static void t_flash_service(void)
{
	CASE("the off service ignores a flash in CONNECTED");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fake_region.flash_service = HAL_FLASH_SERVICE_OFF;
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	CHECK(state_is(0, "CONNECTED"));
	cops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(!cop_has(C_HOLD));
	CHECK(state_is(0, "CONNECTED"));

	CASE("the split service holds the call and opens the recall dial tone");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fake_region.flash_service = HAL_FLASH_SERVICE_SPLIT;
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	cops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(cop_has(C_HOLD));
	CHECK(state_is(0, "PENDING"));

	CASE("the drop service keeps it too");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fake_region.flash_service = HAL_FLASH_SERVICE_DROP;
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	cops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(cop_has(C_HOLD));
	CHECK(state_is(0, "PENDING"));

	CASE("a region load moves the gate with the view it belongs to");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fake_region.flash_service = HAL_FLASH_SERVICE_OFF;
	fixture_start();
	region_ready();
	fake_region.flash_service = HAL_FLASH_SERVICE_DROP;
	region_change("GB");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	cops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(cop_has(C_HOLD));
	CHECK(state_is(0, "PENDING"));
}

static void t_dtmf_duration(void)
{
	struct cop *dtmf;

	CASE("the line reaches CONNECTED for the DTMF duration cases");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	CHECK(state_is(0, "CONNECTED"));

	CASE("a detected digit carries its measured duration to the call layer");
	cops_reset();
	inject_digit_duration(0, '7', false, 110);
	dtmf = cop_find(C_DTMF);
	CHECK(cop_has(C_DTMF));
	CHECK(dtmf->val == '7');
	CHECK_UINT(dtmf->duration_ms, 110);

	CASE("a detected digit duration is capped at eight seconds");
	cops_reset();
	inject_digit_duration(0, '8', false, 9000);
	CHECK_UINT(cop_find(C_DTMF)->duration_ms, 8000);

	CASE("the detector duration window closes before the same pulse digit");
	cops_reset();
	inject_hook(0, HAL_HOOK_PULSE, 8);
	dtmf = cop_find(C_DTMF);
	CHECK(dtmf->val == '8');
	CHECK_UINT(dtmf->duration_ms, 0);

	CASE("a remote digit cannot open the detector duration window");
	cops_reset();
	inject_digit_duration(0, '6', true, 777);
	inject_hook(0, HAL_HOOK_PULSE, 6);
	dtmf = cop_find(C_DTMF);
	CHECK(dtmf->val == '6');
	CHECK_UINT(dtmf->duration_ms, 0);

	/* that remote digit is played toward the phone now, and its tail
	 * suppresses the next digit from the phone (t_digit_play below), so
	 * the line is driven past the deadline before the cases that follow */
	uloop_pump(DIGIT_PLAY_MAX_MS + DIGIT_PLAY_TAIL_MS + 100);

	/*
	 * The measured flag is that same window plus a length, because a
	 * backend that relays a digit it hears sends the event itself and the
	 * call layer must not send a second one. A pulse digit is the
	 * contrast: no detector hears it, so the call layer still sends it.
	 */
	CASE("a detected digit tells the call layer it was measured");
	cops_reset();
	inject_digit_duration(0, '7', false, 110);
	dtmf = cop_find(C_DTMF);
	CHECK(dtmf->val == '7');
	CHECK(dtmf->measured);

	CASE("a pulse digit does not");
	cops_reset();
	inject_hook(0, HAL_HOOK_PULSE, 7);
	dtmf = cop_find(C_DTMF);
	CHECK(dtmf->val == '7');
	CHECK(!dtmf->measured);
}

/*
 * The digit the far end sent out of band, played toward the phone. Both
 * transports end in one queue: the INFO the leg layer parses, and the wire
 * event the backend reports.
 */
static void digit_play_connect(void)
{
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	hops_reset();
	cops_reset();
}

static void t_digit_play(void)
{
	char digits[FSM_DIGITS_MAX + 1];
	struct line_view v;
	unsigned int i;

	CASE("a digit from the wire is played toward the phone on a connected line");
	digit_play_connect();
	CHECK(state_is(0, "CONNECTED"));
	view_get(0, &v);
	snprintf(digits, sizeof(digits), "%s", v.digits);
	inject_digit_stream(0, 0, '7', true, 160);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 1);
	CHECK(hop_find(H_DIGIT_PLAY)->digit == '7');
	CHECK_UINT(hop_find(H_DIGIT_PLAY)->mask, 160);
	CHECK_UINT(hop_find(H_DIGIT_PLAY)->line, 0);

	CASE("and it is not collected as a dialled digit");
	view_get(0, &v);
	CHECK(strcmp(v.digits, digits) == 0);
	CHECK(state_is(0, "CONNECTED"));

	CASE("and the call layer is asked to send nothing back");
	CHECK(!cop_has(C_DTMF));

	CASE("a length under the floor is raised to it");
	digit_play_connect();
	inject_digit_stream(0, 0, '5', true, 20);
	CHECK_UINT(hop_find(H_DIGIT_PLAY)->mask, DIGIT_PLAY_MIN_MS);

	CASE("a length over the ceiling is cut to it");
	digit_play_connect();
	inject_digit_stream(0, 0, '5', true, 3000);
	CHECK_UINT(hop_find(H_DIGIT_PLAY)->mask, DIGIT_PLAY_MAX_MS);

	CASE("an announced length of zero takes the floor as well");
	digit_play_connect();
	inject_digit_stream(0, 0, '5', true, 0);
	CHECK_UINT(hop_find(H_DIGIT_PLAY)->mask, DIGIT_PLAY_MIN_MS);

	CASE("two digits play one after the other and not together");
	digit_play_connect();
	inject_digit_stream(0, 0, '5', true, 0);
	inject_digit_stream(0, 0, '6', true, 0);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 1);
	CHECK(hop_last(H_DIGIT_PLAY)->digit == '5');

	CASE("the second waits for the first tone and the gap behind it");
	uloop_pump(DIGIT_STEP_EARLY_MS);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 1);
	uloop_pump(DIGIT_STEP_LATE_MS);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 2);
	CHECK(hop_last(H_DIGIT_PLAY)->digit == '6');

	/*
	 * A peer that sends one key press as an INFO and as a wire event would
	 * play it twice. The record is the last digit and its instant, so a far
	 * end that really presses one key twice is still heard.
	 */
	CASE("the same digit twice inside the window plays once");
	digit_play_connect();
	inject_digit_stream(0, 0, '5', true, 0);
	inject_digit_stream(0, 0, '5', true, 0);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 1);

	CASE("two different digits inside the window both play");
	digit_play_connect();
	inject_digit_stream(0, 0, '5', true, 0);
	inject_digit_stream(0, 0, '6', true, 0);
	uloop_pump(DIGIT_STEP_EARLY_MS + DIGIT_STEP_LATE_MS);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 2);

	CASE("and the same digit again once the window passed");
	digit_play_connect();
	inject_digit_stream(0, 0, '5', true, 0);
	uloop_pump(DIGIT_PLAY_MIN_MS + DIGIT_PLAY_GAP_MS + DIGIT_DEDUP_MS);
	inject_digit_stream(0, 0, '5', true, 0);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 2);

	CASE("an INFO digit takes the same path as a wire one");
	digit_play_connect();
	rec_cb.digit_net(0, 0, '8', 200);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 1);
	CHECK(hop_find(H_DIGIT_PLAY)->digit == '8');
	CHECK_UINT(hop_find(H_DIGIT_PLAY)->mask, 200);

	CASE("an INFO digit for a line that does not exist changes nothing");
	rec_cb.digit_net(CFG_CHANNEL_MAX, 0, '9', 200);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 1);

	/*
	 * The backend renders the wire event itself (hal.h
	 * HAL_FEAT_DTMF_PLAYOUT), so its report is a report and nothing else.
	 * The INFO that carries the same key press still lands in the record,
	 * so the second transport plays nothing either.
	 */
	CASE("a backend that plays the wire digit itself is not doubled");
	fake_features_extra = HAL_FEAT_DTMF_PLAYOUT;
	digit_play_connect();
	inject_digit_stream(0, 0, '5', true, 160);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 0);

	CASE("and the INFO of the same key press inside the window plays nothing");
	rec_cb.digit_net(0, 0, '5', 160);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 0);

	CASE("while another key press on the INFO still plays");
	rec_cb.digit_net(0, 0, '6', 160);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 1);
	fake_features_extra = 0;

	CASE("a digit from the phone inside the playout window is dropped");
	digit_play_connect();
	inject_digit_stream(0, 0, '5', true, 160);
	inject_digit_duration(0, '7', false, 110);
	CHECK(!cop_has(C_DTMF));

	CASE("and one after the tail reaches the machine again");
	uloop_pump(160 + DIGIT_PLAY_TAIL_MS + 100);
	inject_digit_duration(0, '7', false, 110);
	CHECK(cop_has(C_DTMF));
	CHECK(cop_find(C_DTMF)->val == '7');

	/* no tone sounded, so there is no echo of one to suppress */
	CASE("a playout the backend refused opens no window");
	digit_play_connect();
	digit_play_status = HAL_UNSUPPORTED;
	inject_digit_stream(0, 0, '5', true, 160);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 1);
	inject_digit_duration(0, '7', false, 110);
	CHECK(cop_has(C_DTMF));
	digit_play_status = HAL_OK;

	/* a delayed event from a leg a transfer replaced names a stream no UI
	 * slot holds, and no other leg may take its digit */
	CASE("a digit for a stream no UI slot holds is dropped");
	digit_play_connect();
	inject_digit_stream(0, HAL_STREAM_MAX, '5', true, 160);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 0);

	CASE("the offer window of a waiting call refuses a digit");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	hops_reset();
	inject_digit_stream(0, 0, '5', true, 160);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 0);

	CASE("a queue that overruns drops the digit it cannot hold");
	digit_play_connect();
	for (i = 0; i < DIGIT_Q_MAX + 4; i++)
		inject_digit_stream(0, 0, "0123456789*#ABCD0123"[i], true, 0);
	uloop_pump((DIGIT_PLAY_MIN_MS + DIGIT_PLAY_GAP_MS) * (DIGIT_Q_MAX + 2) +
		   DIGIT_STEP_LATE_MS);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), DIGIT_Q_MAX + 1);

	CASE("the release of the line drops what is still queued");
	digit_play_connect();
	inject_digit_stream(0, 0, '5', true, 0);
	inject_digit_stream(0, 0, '6', true, 0);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 1);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	uloop_pump(DIGIT_STEP_EARLY_MS + DIGIT_STEP_LATE_MS);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 1);

	/*
	 * The window is a deadline on the 32 bit monotonic clock, so the
	 * signed difference is what decides it. The suite cannot move that
	 * clock, so the helper itself carries the case.
	 */
	CASE("the suppression window survives the wrap of the clock");
	CHECK(uvoice_deadline_ahead(50u, UINT32_MAX - 50u));
	CHECK(uvoice_deadline_ahead(UINT32_MAX, UINT32_MAX - 1u));
	CHECK(!uvoice_deadline_ahead(UINT32_MAX - 50u, 50u));
	CHECK(!uvoice_deadline_ahead(100u, 100u));
	CHECK(!uvoice_deadline_ahead(99u, 100u));
}

/*
 * The three diagnostic injections. The cases drive them at their line/line.c
 * entry points, where the refusal set, the seize skip and the injected digit
 * class live. The ubus handlers above them only marshal, which is the register
 * and unregister shape. Neither pair carries a suite at the ubus level.
 */
static void t_inject(void)
{
	char longnum[FSM_DIGITS_MAX + 3];
	unsigned int digits = 99;
	struct line_view v;
	struct cop *dtmf;

	CASE("an injected seize enters OFFHOOK_DIAL through the matrix cell");
	cfg_default();
	fixture_start();
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(line_inject_seize(0), LINE_INJECT_OK);
	CHECK(state_is(0, "OFFHOOK_DIAL"));

	CASE("an injected seize arms the detector like a lifted handset does");
	CHECK(hop_has(H_CHAN_CFG));
	CHECK_UINT(hop_find(H_CHAN_CFG)->chan.detect_mask,
		   HAL_DETECT_DTMF | HAL_DETECT_PULSE | HAL_DETECT_MODEM);
	CHECK(hop_index(H_CHAN_CFG) == 0);

	/* the tone rides LINE_DIAL_TONE_DEFER_MS, which the stub build compiles
	 * to 0, so the case pins the order the rule is about and lets the pump
	 * cover both windows */
	CASE("an injected seize plays the dial tone, behind the detector");
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);
	CHECK(hop_index(H_TONE) > hop_index(H_CHAN_CFG));

	CASE("an injected release returns the line to IDLE");
	CHECK_UINT(line_inject_release(0), LINE_INJECT_OK);
	CHECK(state_is(0, "IDLE"));

	CASE("an injected release on an on hook line is the matrix ignore");
	CHECK_UINT(line_inject_release(0), LINE_INJECT_OK);
	CHECK(state_is(0, "IDLE"));

	CASE("an injected dial on an on hook line seizes first");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(line_inject_dial(0, "5", &digits), LINE_INJECT_OK);
	CHECK_UINT(digits, 1);
	CHECK_UINT(hop_count(H_CHAN_CFG), 1);
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, "5") == 0);
	CHECK(state_is(0, "CALLING"));

	CASE("an injected dial delivers its digits in order");
	cfg_default();
	fixture_start();
	CHECK_UINT(line_inject_dial(0, "5551234", &digits), LINE_INJECT_OK);
	CHECK_UINT(digits, 7);
	CHECK(state_is(0, "COLLECTING"));
	view_get(0, &v);
	CHECK(strcmp(v.digits, "5551234") == 0);

	CASE("an injected star reaches the collector as a dialled star");
	cfg_default();
	fixture_start();
	CHECK_UINT(line_inject_dial(0, "*67", &digits), LINE_INJECT_OK);
	CHECK_UINT(digits, 3);
	view_get(0, &v);
	CHECK(strcmp(v.digits, "*67") == 0);

	CASE("an injected pound reaches the escape as a dialled pound");
	cfg_default();
	fixture_start();
	CHECK_UINT(line_inject_dial(0, "555#", &digits), LINE_INJECT_OK);
	CHECK_UINT(digits, 4);
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, "555") == 0);
	CHECK(state_is(0, "CALLING"));

	CASE("a number that exactly fills the machine digit buffer is dialled");
	cfg_default();
	fixture_start();
	memset(longnum, '5', sizeof(longnum) - 1);
	longnum[sizeof(longnum) - 1] = 0;
	CHECK_UINT(strlen(longnum), FSM_DIGITS_MAX + 2);
	longnum[FSM_DIGITS_MAX] = 0;
	CHECK_UINT(line_inject_dial(0, longnum, &digits), LINE_INJECT_OK);
	CHECK_UINT(digits, FSM_DIGITS_MAX);
	view_get(0, &v);
	CHECK(strcmp(v.digits, longnum) == 0);

	CASE("the line reaches CONNECTED for the off hook injection cases");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	CHECK(state_is(0, "CONNECTED"));

	CASE("an injected dial on an off hook line makes no seize");
	hops_reset();
	cops_reset();
	CHECK_UINT(line_inject_dial(0, "7", &digits), LINE_INJECT_OK);
	CHECK_UINT(digits, 1);
	CHECK_UINT(hop_count(H_CHAN_CFG), 0);
	CHECK(state_is(0, "CONNECTED"));

	/*
	 * The window op_call_dtmf() reads is a detector measurement and no
	 * detector measured this digit, so the injected class is duration
	 * 0, which is the same length hook_pulse() gives a pulse digit. The case
	 * below it is the contrast that keeps this one honest: the window still
	 * works for a digit that really was measured.
	 */
	CASE("an injected digit reaches the leg with no measured duration");
	dtmf = cop_find(C_DTMF);
	CHECK(cop_has(C_DTMF));
	CHECK(dtmf->val == '7');
	CHECK_UINT(dtmf->duration_ms, 0);

	CASE("an injected digit does not");
	CHECK(!cop_find(C_DTMF)->measured);

	CASE("a detected digit beside it still carries its measured duration");
	cops_reset();
	inject_digit_duration(0, '7', false, 110);
	CHECK_UINT(cop_find(C_DTMF)->duration_ms, 110);

	/*
	 * Dial is not uniform, and these two states are why (line/line.h).
	 * RINGING is an on hook state, so the verb seizes and the RINGING cell
	 * answers the caller with that seize; the digits behind it are then
	 * DTMF toward the answered leg and never reach a dial plan. A
	 * line_on_hook() that dropped RINGING would turn the whole call into an
	 * ignored digit and say nothing.
	 */
	CASE("an injected dial on a ringing line answers the caller");
	cfg_default();
	fixture_start();
	inject_incoming(0, 0, "5552000", "Bob", 0);
	CHECK(state_is(0, "RINGING"));
	hops_reset();
	cops_reset();
	CHECK_UINT(line_inject_dial(0, "7", &digits), LINE_INJECT_OK);
	CHECK_UINT(digits, 1);
	CHECK(cop_has(C_ANSWER));
	CHECK_UINT(cop_find(C_ANSWER)->slot, 0);
	CHECK(hop_has(H_RING_STOP));
	CHECK(state_is(0, "CONNECTED"));

	CASE("and its digits reach that caller as DTMF, not a dial plan");
	dtmf = cop_find(C_DTMF);
	CHECK(cop_has(C_DTMF));
	CHECK(dtmf->val == '7');
	CHECK_UINT(dtmf->duration_ms, 0);
	CHECK_UINT(cop_count(C_DIAL), 0);

	/*
	 * The other one: HOLDING is on hook as well, so the seize resumes the
	 * held leg and the machine parks in PENDING until the re-INVITE
	 * answers. PENDING ignores the digits that ride behind it; what this
	 * case is about is that the seize was made at all.
	 */
	CASE("an injected dial on a holding line resumes the held leg");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	inject_hook(0, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(0, 0, true);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "HOLDING"));
	hops_reset();
	cops_reset();
	CHECK_UINT(line_inject_dial(0, "7", &digits), LINE_INJECT_OK);
	CHECK_UINT(digits, 1);
	CHECK(cop_has(C_RESUME));
	CHECK(state_is(0, "PENDING"));

	CASE("and the resumed leg connects when its re-INVITE answers");
	rec_cb.reinvite_done(0, 0, true);
	CHECK(state_is(0, "CONNECTED"));

	CASE("an out of service line is not refused: the matrix answers it");
	cfg_default();
	fixture_start();
	CHECK(state_is(1, "OOS"));
	hops_reset();
	CHECK_UINT(line_inject_seize(1), LINE_INJECT_OK);
	CHECK(state_is(1, "OOS"));
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);
	CHECK_UINT(hop_last(H_TONE)->line, 1);

	CASE("an injected dial on an out of service line reaches no dial plan");
	hops_reset();
	cops_reset();
	CHECK_UINT(line_inject_dial(1, "555", &digits), LINE_INJECT_OK);
	CHECK_UINT(digits, 3);
	CHECK(state_is(1, "OOS"));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);
	CHECK_UINT(cop_count(C_DIAL), 0);

	/*
	 * A port the config array holds and the board does not, which is the
	 * one shape that tests line_exists() on its own: an index past the
	 * array is caught by line_get() whether that gate runs or not, so it
	 * proves nothing about the gate. This is the t_oos "a channel above the
	 * HAL line count" board seen from the injection side.
	 */
	CASE("a port this board does not carry is refused, and nothing is injected");
	cfg_default();
	fixture_start();
	hops_reset();
	cops_reset();
	digits = 99;
	fake_cap.lines = 1;
	CHECK_UINT(line_inject_seize(1), LINE_INJECT_NO_LINE);
	CHECK_UINT(line_inject_release(1), LINE_INJECT_NO_LINE);
	CHECK_UINT(line_inject_dial(1, "5", &digits), LINE_INJECT_NO_LINE);
	CHECK_UINT(digits, 0);
	CHECK_UINT(nhops, 0);
	CHECK_UINT(ncops, 0);
	fake_cap.lines = 2;

	CASE("an index past the config array is refused as well");
	digits = 99;
	CHECK_UINT(line_inject_seize(CFG_CHANNEL_MAX), LINE_INJECT_NO_LINE);
	CHECK_UINT(line_inject_release(CFG_CHANNEL_MAX), LINE_INJECT_NO_LINE);
	CHECK_UINT(line_inject_dial(CFG_CHANNEL_MAX, "5", &digits), LINE_INJECT_NO_LINE);
	CHECK_UINT(digits, 0);
	CHECK_UINT(nhops, 0);
	CHECK_UINT(ncops, 0);

	CASE("a line the HAL counts but the config array cannot hold is refused too");
	fake_cap.lines = CFG_CHANNEL_MAX + 1;
	CHECK_UINT(line_inject_seize(CFG_CHANNEL_MAX), LINE_INJECT_NO_LINE);
	CHECK_UINT(nhops, 0);
	fake_cap.lines = 2;

	CASE("an injection before the lines are up is refused");
	lines_stop();
	digits = 99;
	CHECK_UINT(line_inject_seize(0), LINE_INJECT_NO_LINES);
	CHECK_UINT(line_inject_release(0), LINE_INJECT_NO_LINES);
	CHECK_UINT(line_inject_dial(0, "5", &digits), LINE_INJECT_NO_LINES);
	CHECK_UINT(digits, 0);

	/*
	 * The precedence between the two gates, which the case above cannot see
	 * on its own: lines_done() leaves uv.hal set, so line_exists() still
	 * answers for a live board after the lines are gone. A gate order that
	 * asked it first would call an out of range index a missing port on a
	 * daemon that has no lines at all. "The lines are not up" is the wider
	 * truth and has to win, so the two answers differ only here.
	 */
	CASE("the lines-not-up refusal outranks the bad port one");
	digits = 99;
	CHECK_UINT(line_inject_seize(CFG_CHANNEL_MAX), LINE_INJECT_NO_LINES);
	CHECK_UINT(line_inject_release(CFG_CHANNEL_MAX), LINE_INJECT_NO_LINES);
	CHECK_UINT(line_inject_dial(CFG_CHANNEL_MAX, "5", &digits), LINE_INJECT_NO_LINES);
	CHECK_UINT(digits, 0);

	CASE("an empty number is refused and nothing is injected");
	cfg_default();
	fixture_start();
	hops_reset();
	cops_reset();
	digits = 99;
	CHECK_UINT(line_inject_dial(0, "", &digits), LINE_INJECT_BAD_NUMBER);
	CHECK_UINT(digits, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(nhops, 0);
	CHECK_UINT(ncops, 0);

	CASE("a missing number is refused");
	CHECK_UINT(line_inject_dial(0, NULL, &digits), LINE_INJECT_BAD_NUMBER);

	CASE("a character outside the dialable alphabet refuses the whole number");
	CHECK_UINT(line_inject_dial(0, "555A", &digits), LINE_INJECT_BAD_NUMBER);
	CHECK_UINT(line_inject_dial(0, "555-1234", &digits), LINE_INJECT_BAD_NUMBER);
	CHECK_UINT(line_inject_dial(0, "555 1234", &digits), LINE_INJECT_BAD_NUMBER);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(nhops, 0);
	CHECK_UINT(ncops, 0);

	CASE("a number longer than the machine digit buffer refuses whole");
	memset(longnum, '5', sizeof(longnum) - 1);
	longnum[sizeof(longnum) - 1] = 0;
	CHECK_UINT(line_inject_dial(0, longnum, &digits), LINE_INJECT_BAD_NUMBER);
	longnum[FSM_DIGITS_MAX + 1] = 0;
	CHECK_UINT(line_inject_dial(0, longnum, &digits), LINE_INJECT_BAD_NUMBER);
	CHECK_UINT(digits, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(nhops, 0);
	CHECK_UINT(ncops, 0);
}

static void flash_relay_cfg(unsigned int local_conf, enum cfg_flash_relay relay)
{
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	cfg.channel[0].local_conf_enabled = local_conf;
	cfg.channel[0].sip_flash_relay = relay;
}

static void flash_relay_connect(void)
{
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	CHECK(state_is(0, "CONNECTED"));
	cops_reset();
}

/*
 * The flash relay gates. The CONNECTED gate belongs to the FSM. The two
 * config gates are here, in order: local feature ownership first, then the
 * form. The flash service of the region gates the local branch alone, which
 * the cases of t_flash_service() above check.
 */
static void t_flash_relay(void)
{
	CASE("the shipped defaults keep the flash local: no INFO reaches the wire");
	flash_relay_cfg(1, CFG_FLASH_RELAY_INFO);
	flash_relay_connect();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(!cop_has(C_FLASH_RELAY));
	CHECK(cop_has(C_HOLD));
	CHECK(state_is(0, "PENDING"));

	CASE("no local conference and relay info sends the flash to the far end");
	flash_relay_cfg(0, CFG_FLASH_RELAY_INFO);
	flash_relay_connect();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(cop_has(C_FLASH_RELAY));
	CHECK_UINT(cop_find(C_FLASH_RELAY)->slot, 0);
	CHECK(!cop_has(C_HOLD));
	CHECK(state_is(0, "CONNECTED"));

	CASE("the relay off leaves the local hold path exactly as it was");
	flash_relay_cfg(0, CFG_FLASH_RELAY_OFF);
	flash_relay_connect();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(!cop_has(C_FLASH_RELAY));
	CHECK(cop_has(C_HOLD));
	CHECK(state_is(0, "PENDING"));

	CASE("local conference plus the relay off is the baseline");
	flash_relay_cfg(1, CFG_FLASH_RELAY_OFF);
	flash_relay_connect();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(!cop_has(C_FLASH_RELAY));
	CHECK(cop_has(C_HOLD));
	CHECK(state_is(0, "PENDING"));

	/*
	 * The flash service gates the local branch and nothing else, so the
	 * derive of flash_relay must never read it. The off service turns the
	 * local hold path off, and the relay still fires; a derive that
	 * consulted flash_features would drop this flash on the floor.
	 */
	CASE("the off flash service still relays, so the derive never reads flash_features");
	flash_relay_cfg(0, CFG_FLASH_RELAY_INFO);
	fake_region.flash_service = HAL_FLASH_SERVICE_OFF;
	flash_relay_connect();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(cop_has(C_FLASH_RELAY));
	CHECK(!cop_has(C_HOLD));
	CHECK(state_is(0, "CONNECTED"));

	/* the udsp telephone-event sender maps events 0 to 15 only, so
	 * rfc2833 cannot be relayed at all; the derive says so once and relays
	 * as info */
	CASE("rfc2833 warns once at the derive and relays as info");
	warn_capture_begin();
	flash_relay_cfg(0, CFG_FLASH_RELAY_RFC2833);
	flash_relay_connect();
	CHECK_UINT(warn_capture_count("rfc2833"), 1);
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(cop_has(C_FLASH_RELAY));
	CHECK(!cop_has(C_HOLD));
	CHECK(state_is(0, "CONNECTED"));

	CASE("the info form warns about nothing");
	warn_capture_begin();
	flash_relay_cfg(0, CFG_FLASH_RELAY_INFO);
	flash_relay_connect();
	CHECK_UINT(warn_capture_count("rfc2833"), 0);

	CASE("local ownership suppresses the rfc2833 warning with the relay");
	warn_capture_begin();
	flash_relay_cfg(1, CFG_FLASH_RELAY_RFC2833);
	flash_relay_connect();
	CHECK_UINT(warn_capture_count("rfc2833"), 0);
	warn_capture_end();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(!cop_has(C_FLASH_RELAY));
	CHECK(cop_has(C_HOLD));

	/* the leg layer refuses when no confirmed dialog is left; the shell
	 * traces it and the call goes on, because there is no local fallback */
	CASE("a refused relay changes nothing on the line");
	flash_relay_cfg(0, CFG_FLASH_RELAY_INFO);
	flash_relay_connect();
	flash_relay_refuse = true;
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(cop_has(C_FLASH_RELAY));
	CHECK(!cop_has(C_HOLD));
	CHECK(state_is(0, "CONNECTED"));

	CASE("a reload moves the relay gate with the view it belongs to");
	flash_relay_cfg(1, CFG_FLASH_RELAY_INFO);
	fixture_start();
	cfg.channel[0].local_conf_enabled = 0;
	lines_reload(&cfg);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	cops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(cop_has(C_FLASH_RELAY));
	CHECK(!cop_has(C_HOLD));
	CHECK(state_is(0, "CONNECTED"));
}

/* the connected line with a loaded tone table every call waiting case starts from */
static void cw_offer_line_connect(void)
{
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	hops_reset();
	cops_reset();
}

/*
 * The alert of a waiting call is a sequence: the host plays the subscriber
 * alerting signal first and the type 2 record follows it. A record pushed
 * into the beep reaches the subscriber as a modem chirp with no alert in
 * front of it.
 */
static void t_call_waiting(void)
{
	struct hop *cid;

	CASE("a waiting call alerts with the call waiting tone");
	cw_offer_line_connect();
	CHECK(state_is(0, "CONNECTED"));
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_CALL_WAITING);

	CASE("the alert event itself pushes no record: the SAS goes first");
	CHECK(!hop_has(H_CID));

	CASE("and none goes out while that SAS is still playing");
	uloop_pump(CW_CID_EARLY_MS);
	CHECK(!hop_has(H_CID));

	CASE("the alert carries the type 2 caller id of the waiting leg");
	uloop_pump(CW_CID_LATE_MS);
	CHECK(hop_has(H_CID));
	cid = hop_find(H_CID);
	CHECK_UINT(cid->val, HAL_CID_TYPE2);
	CHECK(strcmp(cid->cid.number, "5552000") == 0);
	CHECK(strcmp(cid->cid.name, "Bob") == 0);

	CASE("the tone reached the line before the record did");
	CHECK(hop_index(H_TONE) < hop_index(H_CID));

	CASE("the record goes out once, the tone profile owns the repeat");
	cw_cid_pump();
	CHECK_UINT(hop_count(H_CID), 1);

	CASE("the waiting leg gets a 180");
	CHECK(cop_has(C_RING));
	CHECK_UINT(cop_find(C_RING)->slot, 1);

	CASE("the alert stops when the offer ends");
	hops_reset();
	rec_cb.remote_cancel(0, 1);
	CHECK(state_is(0, "CONNECTED"));
	CHECK(hop_count(H_TONE_STOP) >= 1);

	/*
	 * A push that outlives its alert announces a caller the subscriber is no
	 * longer being alerted about. The four cases below are the four ways an
	 * alert ends inside the 350 ms.
	 */
	CASE("an offer withdrawn before the record cancels it");
	cw_offer_line_connect();
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	rec_cb.remote_cancel(0, 1);
	CHECK(state_is(0, "CONNECTED"));
	cw_cid_pump();
	CHECK(!hop_has(H_CID));

	CASE("a flash that answers the waiting call cancels it too");
	cw_offer_line_connect();
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(state_is(0, "PENDING"));
	cw_cid_pump();
	CHECK(!hop_has(H_CID));

	CASE("the on hook that turns the offer into a ring cancels it");
	cw_offer_line_connect();
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "RINGING"));
	cw_cid_pump();
	CHECK(!hop_has(H_CID));

	CASE("and the bell announces that caller itself, as a type 1 record");
	CHECK(hop_has(H_RING_START));
	CHECK(hop_find(H_RING_START)->ring.cid_present);
	CHECK(strcmp(hop_find(H_RING_START)->ring.cid.number, "5552000") == 0);

	/*
	 * The active party hangs up while the call waits. The alert gives way
	 * to the reorder, which is the cue the
	 * subscriber gets; the on hook stops it and rings the waiting caller, and
	 * the seize answers that call.
	 */
	CASE("the active party hanging up swaps the alert for the reorder");
	cw_offer_line_connect();
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	hops_reset();
	rec_cb.remote_bye(0, 0);
	CHECK(state_is(0, "CW_OFFERED"));
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);

	CASE("and the deferred type 2 record of that alert is cancelled");
	cw_cid_pump();
	CHECK(!hop_has(H_CID));

	CASE("the on hook then stops the reorder and rings the waiting caller");
	hops_reset();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "RINGING"));
	CHECK(hop_count(H_TONE_STOP) >= 1);
	CHECK(hop_has(H_RING_START));
	CHECK(strcmp(hop_find(H_RING_START)->ring.cid.number, "5552000") == 0);

	CASE("and the seize answers that caller");
	cops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(state_is(0, "CONNECTED"));
	CHECK(cop_has(C_ANSWER));
	CHECK_UINT(cop_find(C_ANSWER)->slot, 1);

	CASE("a reload that takes the line out of service cancels it");
	cw_offer_line_connect();
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	cfg.channel[0].enabled = false;
	lines_reload(&cfg);
	CHECK(state_is(0, "OOS"));
	hops_reset();
	cw_cid_pump();
	CHECK(!hop_has(H_CID));

	/*
	 * Bring-up. No tone profile exists until the country table lands, so
	 * the SAS this alert asks for is held for the replay and the record
	 * must not go out in front of it. The shell drops the record; the tone
	 * still replays when the table arrives.
	 */
	CASE("an alert before the first table load pushes no record");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	hops_reset();
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_CALL_WAITING);
	cw_cid_pump();
	CHECK(!hop_has(H_CID));

	CASE("the table that lands later replays that alert tone");
	hops_reset();
	job_complete(HAL_OK);
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_CALL_WAITING);

	CASE("and the replay resurrects no record behind it");
	cw_cid_pump();
	CHECK(!hop_has(H_CID));

	CASE("a first table load that FAILED gives the same answer");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	job_complete(HAL_FATAL);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	hops_reset();
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_CALL_WAITING);
	cw_cid_pump();
	CHECK(!hop_has(H_CID));

	CASE("call waiting off answers a second offer with 486");
	cfg_default();
	cfg.channel[0].call_waiting_enabled = false;
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	cops_reset();
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CONNECTED"));
	CHECK(cop_has(C_REJECT));
	CHECK_UINT(cop_find(C_REJECT)->val, 486);
}

/* A region whose read-back carries four distinctive call-waiting variants, with
 * the plain tone and c3 at deliberately different lead-ins, so the deferral
 * test can tell them apart. */
static void cw_variant_region_seed(void)
{
	fake_region.cw_count = 4;
	fake_region.cw_lead_in_ms[0] = 300;	/* plain */
	fake_region.cw_lead_in_ms[1] = 300;	/* c1 */
	fake_region.cw_lead_in_ms[2] = 300;	/* c2 */
	fake_region.cw_lead_in_ms[3] = 500;	/* c3 */
	fake_region.cw_lead_in_ms[4] = 700;	/* c4 */
}

static void cw_variant_line_connect(void)
{
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	cw_variant_region_seed();
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	hops_reset();
	cops_reset();
}

/*
 * The distinctive call-waiting tone. The Alert-Info template of a waiting
 * call selects a variant, and the shell plays that variant instead of the
 * plain tone. It defers the type 2 record by the first-presentation lead-in of
 * the variant, not by the fixed 350 ms.
 */
static void t_cw_variant(void)
{
	struct hop *cid;

	CASE("a distinctive Alert-Info selects the call waiting variant");
	cw_variant_line_connect();
	inject_incoming(0, 1, "5552000", "Bob", 3);	/* Bellcore-dr3 -> variant 3 */
	CHECK(state_is(0, "CW_OFFERED"));
	CHECK(hop_has(H_CW_TONE));
	CHECK_UINT(hop_last(H_CW_TONE)->val, 3);
	CHECK(!hop_has(H_TONE));

	CASE("the variant defers the record by its own lead-in, not 350 ms");
	/* c3 lead-in 500 ms plus the 50 ms gap is 550 ms. 430 ms is past the
	 * plain 350 ms deadline and short of this one. */
	uloop_pump(CW_CID_EARLY_MS + CW_CID_LATE_MS);
	CHECK(!hop_has(H_CID));
	uloop_pump(200);
	CHECK(hop_has(H_CID));
	cid = hop_find(H_CID);
	CHECK_UINT(cid->val, HAL_CID_TYPE2);
	CHECK(strcmp(cid->cid.number, "5552000") == 0);

	CASE("an Alert-Info id past the variant count plays the plain tone");
	cw_variant_line_connect();
	inject_incoming(0, 1, "5552000", "Bob", 6);	/* dr6 -> 0, plain */
	CHECK(state_is(0, "CW_OFFERED"));
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_CALL_WAITING);
	CHECK(!hop_has(H_CW_TONE));

	CASE("the plain variant still defers at the plain lead-in");
	cw_variant_line_connect();
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	uloop_pump(CW_CID_EARLY_MS);
	CHECK(!hop_has(H_CID));
	uloop_pump(CW_CID_LATE_MS);
	CHECK(hop_has(H_CID));

	CASE("a region without variants plays the plain tone for any id");
	cw_offer_line_connect();	/* fake_region_default: cw_count 0 */
	inject_incoming(0, 1, "5552000", "Bob", 2);
	CHECK(state_is(0, "CW_OFFERED"));
	CHECK(hop_has(H_TONE));
	CHECK(!hop_has(H_CW_TONE));
	cw_cid_pump();
}

static void t_call_events(void)
{
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();

	CASE("a provisional response moves the line to PROCEEDING");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	hops_reset();
	rec_cb.proceeding(0, 0, false);
	CHECK(state_is(0, "PROCEEDING"));

	CASE("without early media the line hears ringback");
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_RINGBACK);

	CASE("a busy final gives the busy tone");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	hops_reset();
	rec_cb.failed(0, 0, CALL_FAIL_BUSY);
	CHECK(state_is(0, "CLEARING"));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_BUSY);

	CASE("an unobtainable final asks for the number-unobtainable tone");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	hops_reset();
	rec_cb.failed(0, 0, CALL_FAIL_UNOBTAINABLE);
	CHECK_UINT(hop_find(H_TONE)->val, HAL_TONE_NUMBER_UNOBTAINABLE);

	CASE("a far end BYE clears an answered call quietly");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	hops_reset();
	rec_cb.remote_bye(0, 0);
	CHECK(state_is(0, "CLEARING"));
	CHECK(hop_count(H_TONE_STOP) >= 1);

	CASE("a remote hold report reaches no matrix cell");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	hops_reset();
	cops_reset();
	rec_cb.hold_changed(0, 0, true);
	CHECK(state_is(0, "CONNECTED"));
	CHECK_UINT(nhops, 0);
	CHECK_UINT(ncops, 0);

	CASE("a flash holds the answered call and asks for a second one");
	cops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(cop_has(C_HOLD));
	CHECK(state_is(0, "PENDING"));

	CASE("the accepted re-INVITE opens the recall dial tone");
	rec_cb.reinvite_done(0, 0, true);
	CHECK(state_is(0, "HOLD_DIAL"));

	CASE("a refused re-INVITE keeps the call");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	inject_hook(0, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(0, 0, false);
	CHECK(state_is(0, "CONNECTED"));

	CASE("an event for an unknown line is dropped");
	rec_cb.answered(9, 0);
	CHECK(state_is(0, "CONNECTED"));
}

static void t_oos(void)
{
	struct line_view v;

	CASE("a disabled channel starts out of service");
	cfg_default();
	fixture_start();
	CHECK(state_is(1, "OOS"));
	view_get(1, &v);
	CHECK(v.oos);

	CASE("an enabled channel starts idle");
	CHECK(state_is(0, "IDLE"));
	view_get(0, &v);
	CHECK(!v.oos);

	CASE("seizing an out of service line plays reorder");
	hops_reset();
	inject_hook(1, HAL_HOOK_SEIZE, 0);
	CHECK(state_is(1, "OOS"));
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);
	CHECK_UINT(hop_last(H_TONE)->line, 1);

	CASE("releasing an out of service line stops the tone");
	hops_reset();
	inject_hook(1, HAL_HOOK_RELEASE, 0);
	CHECK(hop_count(H_TONE_STOP) >= 1);

	CASE("a disabled base config takes every line out of service");
	cfg_default();
	cfg.enabled = false;
	fixture_start();
	CHECK(state_is(0, "OOS"));
	CHECK(state_is(1, "OOS"));

	CASE("a channel above the HAL line count is out of service");
	cfg_default();
	cfg.channel[1].enabled = true;
	fixture_start();
	CHECK(state_is(1, "IDLE"));
	lines_stop();
	fake_cap.lines = 1;
	cops_reset();
	hops_reset();
	lines_bring_up();
	CHECK(state_is(1, "OOS"));
	fake_cap.lines = 2;

	CASE("no HAL means no line");
	lines_stop();
	uv.hal = NULL;
	lines_bring_up();
	CHECK(state_is(0, "OOS"));
	CHECK(state_is(1, "OOS"));
	uv.hal = &fake_hal;

	CASE("a channel above voip.base.channels is out of service");
	cfg_default();
	cfg.channels = 1;
	cfg.channel[1].enabled = true;
	fixture_start();
	CHECK(state_is(0, "IDLE"));
	CHECK(state_is(1, "OOS"));

	CASE("raising the channel count puts the line in service");
	cfg.channels = 2;
	lines_reload(&cfg);
	CHECK(state_is(1, "IDLE"));
}

/*
 * The country tone table. Every local tone is a row of the DSP tone profile,
 * so a line whose region never loaded is silent on real hardware. The mapping
 * is asserted through the argument the backend op receives, which covers the
 * helper and the wiring from cfg->region in one step.
 */
static void t_region(void)
{
	static const struct {
		const char *in;
		const char *want;
	} rows[] = {
		{ "AU", "AUS" }, { "BE", "BEL" }, { "BR", "BRA" }, { "CH", "CHE" },
		{ "CL", "CHL" }, { "CN", "CHN" }, { "DE", "DEU" }, { "DK", "DNK" },
		{ "ES", "ESP" }, { "FI", "FIN" }, { "FR", "FRA" }, { "GB", "GBR" },
		{ "UK", "GBR" }, { "HK", "HKO" }, { "HU", "HUN" }, { "IN", "IND" },
		{ "IR", "IRN" }, { "IT", "ITA" }, { "JP", "JPN" }, { "LK", "LKA" },
		{ "MX", "MEX" }, { "NL", "NLD" }, { "NZ", "NZL" }, { "PL", "POL" },
		{ "RU", "RUS" }, { "SE", "SWE" }, { "TH", "THA" }, { "US", "USA" },
		{ "VN", "VNM" },
		/* the two seeded regions whose region file is not an ISO name:
		 * EU takes the generic ETSI profile and RS takes the 425 Hz YUG
		 * set; AT is a plain ISO row (AUT, the udsp region file) */
		{ "EU", "ETS" }, { "AT", "AUT" }, { "RS", "YUG" },
	};
	unsigned int i;
	bool ok = true;

	CASE("every mapped alpha-2 region names its row of the region table");
	for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		cfg_default();
		snprintf(cfg.region, sizeof(cfg.region), "%s", rows[i].in);
		fixture_start();
		if (!strcmp(region_seen, rows[i].want))
			continue;

		ok = false;
		printf("      region \"%s\" gave \"%s\", want \"%s\"\n", rows[i].in,
		       region_seen, rows[i].want);
	}
	CHECK(ok);

	CASE("the alpha-2 form is matched without case");
	cfg_default();
	snprintf(cfg.region, sizeof(cfg.region), "us");
	fixture_start();
	CHECK(strcmp(region_seen, "USA") == 0);

	CASE("a three letter region passes through, uppercased");
	cfg_default();
	snprintf(cfg.region, sizeof(cfg.region), "deu");
	fixture_start();
	CHECK(strcmp(region_seen, "DEU") == 0);

	CASE("the names ISO 3166 does not define pass through too");
	cfg_default();
	snprintf(cfg.region, sizeof(cfg.region), "ETS");
	fixture_start();
	CHECK(strcmp(region_seen, "ETS") == 0);
	cfg_default();
	snprintf(cfg.region, sizeof(cfg.region), "YUG");
	fixture_start();
	CHECK(strcmp(region_seen, "YUG") == 0);

	CASE("an unknown alpha-2 region falls back to USA");
	cfg_default();
	snprintf(cfg.region, sizeof(cfg.region), "XX");
	fixture_start();
	CHECK(strcmp(region_seen, "USA") == 0);

	/*
	 * The two India variants are longer than three characters, and the
	 * backend buffer is what limits the name. A form that accepted two and
	 * three characters only would leave these two directories unreachable,
	 * and a config naming one would silently load the USA tones.
	 */
	CASE("a long region name passes through verbatim");
	cfg_default();
	snprintf(cfg.region, sizeof(cfg.region), "IND_SK");
	fixture_start();
	CHECK(strcmp(region_seen, "IND_SK") == 0);
	cfg_default();
	snprintf(cfg.region, sizeof(cfg.region), "IND_SV");
	fixture_start();
	CHECK(strcmp(region_seen, "IND_SV") == 0);

	/* the widest name the backend buffer can carry, terminator included: the
	 * bound is the buffer, not a guess about how long a directory name gets */
	CASE("a name that exactly fills the backend buffer still passes through");
	cfg_default();
	snprintf(cfg.region, sizeof(cfg.region), "ABCDEFG");
	fixture_start();
	CHECK(strcmp(region_seen, "ABCDEFG") == 0);

	CASE("a region too short or too long for the backend falls back to USA");
	cfg_default();
	cfg.region[0] = 0;
	fixture_start();
	CHECK(strcmp(region_seen, "USA") == 0);
	cfg_default();
	snprintf(cfg.region, sizeof(cfg.region), "U");
	fixture_start();
	CHECK(strcmp(region_seen, "USA") == 0);
	/* one character more than the backend region buffer can carry */
	cfg_default();
	snprintf(cfg.region, sizeof(cfg.region), "IND_SKXY");
	fixture_start();
	CHECK(strcmp(region_seen, "USA") == 0);

	CASE("the tone table load is submitted once the DSP is ready");
	cfg_default();
	fixture_start();
	CHECK_UINT(region_calls, 1);
	CHECK(strcmp(region_seen, "USA") == 0);
	CHECK(job_pending != NULL);

	CASE("a reload that keeps the region does not reload the table");
	job_complete(HAL_OK);
	lines_reload(&cfg);
	CHECK_UINT(region_calls, 1);

	CASE("a reload that changes the region reloads the table");
	snprintf(cfg.region, sizeof(cfg.region), "DE");
	lines_reload(&cfg);
	CHECK_UINT(region_calls, 2);
	CHECK(strcmp(region_seen, "DEU") == 0);
	job_complete(HAL_OK);

	CASE("a region changed while the job runs is applied at its completion");
	cfg_default();
	fixture_start();
	CHECK_UINT(region_calls, 1);
	snprintf(cfg.region, sizeof(cfg.region), "FR");
	lines_reload(&cfg);
	CHECK_UINT(region_calls, 1);	/* one job at a time */
	job_complete(HAL_OK);
	CHECK_UINT(region_calls, 2);
	CHECK(strcmp(region_seen, "FRA") == 0);
	job_complete(HAL_OK);

	CASE("a failed load leaves the daemon running and retries on the next reload");
	cfg_default();
	fixture_start();
	job_complete(HAL_TRANSIENT);
	CHECK(state_is(0, "IDLE"));
	snprintf(cfg.region, sizeof(cfg.region), "GB");
	lines_reload(&cfg);
	CHECK_UINT(region_calls, 2);
	CHECK(strcmp(region_seen, "GBR") == 0);

	CASE("a backend that refuses the op does not leave the shell waiting");
	cfg_default();
	fixture_start();
	job_complete(HAL_OK);
	region_status = HAL_UNSUPPORTED;
	snprintf(cfg.region, sizeof(cfg.region), "IT");
	lines_reload(&cfg);
	CHECK_UINT(region_calls, 2);
	CHECK(job_pending == NULL);
	region_status = HAL_OK;
	snprintf(cfg.region, sizeof(cfg.region), "SE");
	lines_reload(&cfg);
	CHECK(strcmp(region_seen, "SWE") == 0);
	CHECK(job_pending != NULL);
	job_complete(HAL_OK);

	CASE("no table load without a HAL");
	lines_stop();
	region_calls = 0;
	uv.hal = NULL;
	lines_bring_up();
	CHECK_UINT(region_calls, 0);
	uv.hal = &fake_hal;

	CASE("no table load before the DSP is ready");
	lines_stop();
	region_calls = 0;
	uv.hal_state = UVOICE_HAL_STARTING;
	lines_bring_up();
	CHECK_UINT(region_calls, 0);
	uv.hal_state = UVOICE_HAL_READY;

	CASE("shutdown takes a pending table load back off the job list");
	cfg_default();
	fixture_start();
	CHECK(job_pending != NULL);
	lines_done();
	lines_live = false;
	CHECK(job_pending == NULL);

	CASE("a failed load still tries the region that moved during the flight");
	cfg_default();
	fixture_start();
	CHECK_UINT(region_calls, 1);
	snprintf(cfg.region, sizeof(cfg.region), "DE");
	lines_reload(&cfg);
	CHECK_UINT(region_calls, 1);		/* one job at a time */
	job_complete(HAL_TRANSIENT);
	CHECK_UINT(region_calls, 2);
	CHECK(strcmp(region_seen, "DEU") == 0);
	job_complete(HAL_OK);

	/*
	 * A failed job leaves the DSP table unknown. The backend push can stop
	 * partway, so the name of the region that loaded last no longer
	 * describes what the DSP holds. The shell therefore forgets that name,
	 * and a config that returns to it reloads it in full instead of being
	 * skipped as a no-op; without that the hybrid table would persist for
	 * the life of the daemon.
	 */
	CASE("a failed load forgets the region that was applied before it");
	cfg_default();
	fixture_start();
	job_complete(HAL_OK);
	CHECK_UINT(region_calls, 1);
	CHECK(strcmp(region_seen, "USA") == 0);
	snprintf(cfg.region, sizeof(cfg.region), "IT");
	lines_reload(&cfg);
	CHECK_UINT(region_calls, 2);
	job_complete(HAL_FATAL);

	CASE("so returning to that region reloads it instead of skipping it");
	snprintf(cfg.region, sizeof(cfg.region), "US");
	lines_reload(&cfg);
	CHECK_UINT(region_calls, 3);
	CHECK(strcmp(region_seen, "USA") == 0);
	CHECK(job_pending != NULL);
	job_complete(HAL_OK);

	CASE("and a successful load is still skipped when nothing moved");
	lines_reload(&cfg);
	CHECK_UINT(region_calls, 3);

	CASE("a failed load with an unmoved region waits for the next reload");
	cfg_default();
	fixture_start();
	job_complete(HAL_TRANSIENT);
	CHECK_UINT(region_calls, 1);
	CHECK(job_pending == NULL);

	CASE("the retry after that failure happens on the next reload");
	snprintf(cfg.region, sizeof(cfg.region), "FR");
	lines_reload(&cfg);
	CHECK_UINT(region_calls, 2);
	CHECK(strcmp(region_seen, "FRA") == 0);
	job_complete(HAL_OK);
}

/*
 * The tone the subscriber asked for before the DSP had a tone table. The
 * region job can take seconds and the dial tone defer is half a second, so
 * the request has to survive until the profiles exist.
 */
static void t_tone_replay(void)
{
	CASE("a tone asked for before the region lands is re-issued when it does");
	cfg_default();
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	hops_reset();
	job_complete(HAL_OK);
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);
	CHECK_UINT(hop_last(H_TONE)->line, 0);

	CASE("the replay consumes the record, so nothing repeats it");
	hops_reset();
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK(hop_count(H_TONE) <= 1);		/* at most the deferred original */

	CASE("a handset back on hook before the region lands gets no replay");
	cfg_default();
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	hops_reset();
	job_complete(HAL_OK);
	CHECK_UINT(hop_count(H_TONE), 0);

	CASE("a failure tone asked for before the region lands is replayed too");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	dial_fail_times = 1;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	CHECK(state_is(0, "CLEARING"));
	hops_reset();
	job_complete(HAL_OK);
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);
	dial_fail_times = 0;

	CASE("a second region load replays nothing");
	cfg_default();
	fixture_start();
	job_complete(HAL_OK);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	snprintf(cfg.region, sizeof(cfg.region), "FR");
	lines_reload(&cfg);
	hops_reset();
	job_complete(HAL_OK);
	CHECK_UINT(hop_count(H_TONE), 0);

	CASE("only the line that asked for a tone gets one");
	cfg_default();
	cfg.channel[1].enabled = true;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	hops_reset();
	job_complete(HAL_OK);
	CHECK_UINT(hop_count(H_TONE), 1);
	CHECK_UINT(hop_last(H_TONE)->line, 0);
}

/*
 * The tone the stub backend does not carry. A HAL that refuses
 * HAL_TONE_NUMBER_UNOBTAINABLE must still leave the subscriber with an audible
 * failure, so the shell substitutes reorder at the submission point. The
 * refusal is the behavior of the stub backend, which is why the recorder
 * above mirrors it.
 */
static void t_tone_unobtainable(void)
{
	CASE("a refused number-unobtainable tone reaches the HAL as reorder");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	job_complete(HAL_OK);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	hops_reset();
	rec_cb.failed(0, 0, CALL_FAIL_UNOBTAINABLE);
	CHECK_UINT(hop_count(H_TONE), 2);
	CHECK_UINT(hop_find(H_TONE)->val, HAL_TONE_NUMBER_UNOBTAINABLE);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);
	CHECK_UINT(hop_last(H_TONE)->line, 0);

	CASE("the substitution is submitted once, not once per pass");
	hops_reset();
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK_UINT(hop_count(H_TONE), 0);

	CASE("a backend with a number-unobtainable profile plays it unchanged");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	job_complete(HAL_OK);
	tone_unobtainable_ok = true;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	hops_reset();
	rec_cb.failed(0, 0, CALL_FAIL_UNOBTAINABLE);
	CHECK_UINT(hop_count(H_TONE), 1);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_NUMBER_UNOBTAINABLE);
	tone_unobtainable_ok = false;

	CASE("a tone the backend accepts is not substituted");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	job_complete(HAL_OK);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	hops_reset();
	rec_cb.failed(0, 0, CALL_FAIL_BUSY);
	CHECK_UINT(hop_count(H_TONE), 1);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_BUSY);

	CASE("the pending record keeps the tone the machine asked for");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.failed(0, 0, CALL_FAIL_UNOBTAINABLE);
	CHECK(state_is(0, "CLEARING"));
	hops_reset();
	job_complete(HAL_OK);
	CHECK_UINT(hop_count(H_TONE), 2);
	CHECK_UINT(hop_find(H_TONE)->val, HAL_TONE_NUMBER_UNOBTAINABLE);

	CASE("the replay of that record substitutes reorder again");
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);
}

/*
 * Clearing tones take the staged window that also controls their FSM timer.
 * NUMBER_UNOBTAINABLE and REORDER share the fixed reorder window
 * (FSM_REORDER_MS). The substitution case therefore checks the carried value,
 * but cannot identify which tone selected it.
 */
static void t_tone_duration(void)
{
	CASE("the busy tone carries the busy clearing window");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	job_complete(HAL_OK);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	hops_reset();
	rec_cb.failed(0, 0, CALL_FAIL_BUSY);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_BUSY);
	CHECK_UINT(hop_last(H_TONE)->mask, 40000);

	CASE("the reorder tone carries the fixed reorder window");
	fixture_start();
	job_complete(HAL_OK);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	hops_reset();
	rec_cb.failed(0, 0, CALL_FAIL_CONGESTION);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);
	CHECK_UINT(hop_last(H_TONE)->mask, 40000);

	CASE("a number-unobtainable request carries the fixed reorder window");
	fixture_start();
	job_complete(HAL_OK);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	hops_reset();
	rec_cb.failed(0, 0, CALL_FAIL_UNOBTAINABLE);
	CHECK_UINT(hop_find(H_TONE)->val, HAL_TONE_NUMBER_UNOBTAINABLE);
	CHECK_UINT(hop_find(H_TONE)->mask, 40000);

	CASE("the substitution keeps the window of the request");
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);
	CHECK_UINT(hop_last(H_TONE)->mask, 40000);

	CASE("a pending clearing tone replays with its window");
	fixture_start();
	dial_fail_times = 1;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	hops_reset();
	job_complete(HAL_OK);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);
	CHECK_UINT(hop_last(H_TONE)->mask, 40000);
	dial_fail_times = 0;

	CASE("a staged busy window does not move the active call");
	fixture_start();
	job_complete(HAL_OK);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	cfg.channel[0].busy_tone_s = 23;
	lines_reload(&cfg);
	hops_reset();
	rec_cb.failed(0, 0, CALL_FAIL_BUSY);
	CHECK_UINT(hop_last(H_TONE)->mask, 40000);

	CASE("the next busy submission takes the reloaded window");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	hops_reset();
	rec_cb.failed(0, 0, CALL_FAIL_BUSY);
	CHECK_UINT(hop_last(H_TONE)->mask, 23000);
}

static void t_preinit(void)
{
	struct line_view v;

	CASE("a hook event before lines_init is dropped");
	cops_reset();
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	CHECK_UINT(nhops, 0);
	CHECK_UINT(ncops, 0);

	CASE("the status dump reports no line before lines_init");
	CHECK(!view_get(0, &v));
	CHECK_UINT(dump_lines, 0);
}

/*
 * The bring-up is two halves, and these cases check the line between them.
 * The early half wires everything that needs no hardware. Only the ops that
 * the DSP must accept wait for the DSP half. Registration never waits for the
 * DSP. A call layer that came up on the dsp_start completion would therefore
 * leave a call offered inside that window with no leg layer. The caller would
 * then hear that the subscriber was busy.
 */
static void t_bring_up_split(void)
{
	CASE("the early half wires the call layer");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	/* the cases below run the halves by hand, so they set the state
	 * fixture_start() would have set for them */
	uv.hal_state = UVOICE_HAL_READY;
	lines_stop();
	hops_reset();
	cops_reset();
	memset(hook_ts_calls, 0, sizeof(hook_ts_calls));
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	region_calls = 0;
	lines_init(&cfg);
	lines_live = true;
	CHECK_UINT(cop_count(C_INIT), 1);
	CHECK_UINT(cop_count(C_RELOAD), 1);
	CHECK_UINT(cop_count(C_HAL_READY), 1);
	CHECK(rec_cb.incoming != NULL);
	CHECK(rec_media.priv == &fake_hal);
	CHECK(state_is(0, "IDLE"));

	CASE("and a call reaches the leg layer with the DSP half still to come");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	CHECK(cop_has(C_DIAL));
	CHECK(state_is(0, "CALLING"));

	CASE("while nothing the DSP has to accept has gone out");
	CHECK_UINT(hook_ts_calls[0], 0);
	CHECK_UINT(hook_ts_calls[1], 0);
	CHECK_UINT(region_calls, 0);
	CHECK_UINT(hook_query_calls[0], 0);

	CASE("the DSP half pushes the hook window of every port the board carries");
	lines_start(&cfg);
	CHECK_UINT(hook_ts_calls[0], 1);
	CHECK_UINT(hook_ts_calls[1], 1);

	CASE("and loads the country tone table");
	CHECK_UINT(region_calls, 1);

	CASE("and reads the boot hook state of the line in service");
	CHECK_UINT(hook_query_calls[0], 1);
	CHECK_UINT(hook_query_calls[1], 0);

	/* the forced OOS entry of the early half stops the tone and the bell on
	 * a DSP that has not started, and libudsp refuses both */
	CASE("the DSP half silences the port the early half put out of service");
	cfg_default();
	lines_stop();
	cops_reset();
	lines_init(&cfg);
	lines_live = true;
	hops_reset();
	lines_start(&cfg);
	CHECK_UINT(hop_count(H_TONE_STOP), 1);
	CHECK_UINT(hop_last(H_TONE_STOP)->line, 1);
	CHECK_UINT(hop_count(H_RING_STOP), 1);
	CHECK_UINT(hop_last(H_RING_STOP)->line, 1);

	/*
	 * The leg layer is handed a predicate, not a value: the admission rule
	 * of call.c reads it on every new INVITE, so the state it answers for
	 * is the one the daemon holds at that moment.
	 */
	CASE("the predicate the leg layer reads follows the daemon HAL state");
	CHECK(rec_hal_ready != NULL);
	CHECK(rec_hal_ready && rec_hal_ready());
	uv.hal_state = UVOICE_HAL_STARTING;
	CHECK(rec_hal_ready && !rec_hal_ready());

	CASE("and a bring-up that failed is not ready either");
	uv.hal_state = UVOICE_HAL_FAILED;
	CHECK(rec_hal_ready && !rec_hal_ready());
	uv.hal_state = UVOICE_HAL_READY;

	CASE("the DSP half does nothing before the early one wired the lines");
	lines_stop();
	hops_reset();
	memset(hook_ts_calls, 0, sizeof(hook_ts_calls));
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	region_calls = 0;
	lines_start(&cfg);
	CHECK_UINT(nhops, 0);
	CHECK_UINT(hook_ts_calls[0], 0);
	CHECK_UINT(region_calls, 0);
	CHECK_UINT(hook_query_calls[0], 0);

	/*
	 * The port count is not final at the attach. A backend can answer zero
	 * lines when its attach-time bring-up fails and recover the real count
	 * inside the DSP start, so the service state the early half derives
	 * from it is provisional.
	 * Without the reconcile in the DSP half every line of that board stays
	 * out of service for the life of the daemon while the status reports
	 * the DSP ready, and only a config reload heals it.
	 */
	CASE("a board whose port count arrives with the DSP puts its lines in service");
	cfg_default();
	cfg.channel[1].enabled = true;
	lines_stop();
	uv.hal_state = UVOICE_HAL_STARTING;
	fake_cap.lines = 0;
	hops_reset();
	cops_reset();
	lines_init(&cfg);
	lines_live = true;
	CHECK(state_is(0, "OOS"));
	CHECK(state_is(1, "OOS"));
	fake_cap.lines = 2;
	uv.hal_state = UVOICE_HAL_READY;
	lines_start(&cfg);
	CHECK(state_is(0, "IDLE"));
	CHECK(state_is(1, "IDLE"));

	/* the same reconcile in the other direction: a section for a port the
	 * recovered count does not carry is a line this board does not have */
	CASE("and a port the recovered count does not carry goes out of service");
	cfg_default();
	cfg.channel[1].enabled = true;
	lines_stop();
	fake_cap.lines = 2;
	lines_init(&cfg);
	lines_live = true;
	CHECK(state_is(1, "IDLE"));
	fake_cap.lines = 1;
	hops_reset();
	lines_start(&cfg);
	CHECK(state_is(1, "OOS"));
	CHECK(state_is(0, "IDLE"));
	CHECK(hop_has(H_TONE_STOP));
	CHECK_UINT(hop_last(H_TONE_STOP)->line, 1);
	CHECK(hop_has(H_RING_STOP));
	CHECK_UINT(hop_last(H_RING_STOP)->line, 1);
	fake_cap.lines = 2;

	/*
	 * A NOTIFY inside the window. The channel view is loaded before the
	 * DSP, so the vmwi gate opens while the channels are not started, and a
	 * record pushed there would cross a backend that cannot take it. The
	 * switch does not send that message again, so the record is held and
	 * the DSP half delivers it.
	 */
	CASE("a message that arrives inside the window is not pushed at a dead DSP");
	cfg_default();
	lines_stop();
	uv.hal_state = UVOICE_HAL_STARTING;
	lines_init(&cfg);
	lines_live = true;
	hops_reset();
	mwi_notify(0, true, 1, 0);
	CHECK_UINT(hop_count(H_CID), 0);

	CASE("and the DSP half delivers it once the channels are up");
	uv.hal_state = UVOICE_HAL_READY;
	lines_start(&cfg);
	CHECK_UINT(hop_count(H_CID), 1);
	CHECK_UINT(hop_last(H_CID)->line, 0);
	CHECK_UINT(hop_last(H_CID)->cid.mwi, HAL_MWI_ON);

	CASE("a message that arrives after the DSP is ready is pushed at once");
	hops_reset();
	mwi_notify(0, true, 3, 1);
	CHECK_UINT(hop_count(H_CID), 1);
	CHECK_UINT(hop_last(H_CID)->cid.mwi, HAL_MWI_ON);

	/*
	 * A NOTIFY before the port exists. The line enters service inside the
	 * DSP half, and the drain of that entry replays the held record, so the
	 * start must not present it a second time.
	 */
	CASE("a message that arrives before the line exists is presented once");
	mwi_notify(0, false, 0, 0);
	lines_stop();
	uv.hal_state = UVOICE_HAL_STARTING;
	fake_cap.lines = 0;
	lines_init(&cfg);
	lines_live = true;
	CHECK(state_is(0, "OOS"));
	mwi_notify(0, true, 2, 0);
	hops_reset();
	fake_cap.lines = 2;
	uv.hal_state = UVOICE_HAL_READY;
	lines_start(&cfg);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(hop_count(H_CID), 1);
	CHECK_UINT(hop_last(H_CID)->line, 0);
	CHECK_UINT(hop_last(H_CID)->cid.mwi, HAL_MWI_ON);

	/* the vmwi gate holds the record, and a reload inside the window turns
	 * the option on before the DSP half runs. The protocol derives the
	 * reloaded view from the daemon config, as main.c sets it. */
	CASE("a message held behind vmwi 0 is presented at the start a reload "
	     "enabled");
	mwi_notify(0, false, 0, 0);
	cfg_default();
	cfg.channel[0].vmwi = false;
	lines_stop();
	uv.cfg = &cfg;
	uv.hal_state = UVOICE_HAL_STARTING;
	lines_init(&cfg);
	lines_live = true;
	mwi_notify(0, true, 3, 0);
	cfg.channel[0].vmwi = true;
	lines_reload(&cfg);
	hops_reset();
	uv.hal_state = UVOICE_HAL_READY;
	lines_start(&cfg);
	CHECK_UINT(hop_count(H_CID), 1);
	CHECK_UINT(hop_last(H_CID)->line, 0);
	CHECK_UINT(hop_last(H_CID)->cid.mwi, HAL_MWI_ON);

	/* the store outlives this suite, so the cases that follow start from a
	 * line the switch has nothing waiting for */
	mwi_notify(0, false, 0, 0);
	uv.cfg = NULL;
}

static void t_reload(void)
{
	struct line_view v;

	/* the reload of main.c: the protocol takes the config, then the lines */
	CASE("a reload applies a new dial plan at rest");
	cfg_default();
	fixture_start();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	proto_reload(&cfg, &cfg);
	lines_reload(&cfg);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	CHECK(cop_has(C_DIAL));
	CHECK(state_is(0, "CALLING"));

	CASE("a reload reaches the call layer too");
	CHECK(cop_count(C_RELOAD) >= 1);
	CHECK_UINT(acct_rec.reload, 1);

	CASE("a reload is refused while the machine is busy");
	cfg_default();
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '1', false);
	CHECK(state_is(0, "COLLECTING"));
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	lines_reload(&cfg);
	cops_reset();
	inject_digit(0, '5', false);
	CHECK(!cop_has(C_DIAL));

	CASE("the pending view lands at the next rest state");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, "5") == 0);

	CASE("a newly disabled channel tears its legs down and goes OOS");
	cfg_default();
	fixture_start();
	cfg.channel[0].enabled = false;
	cops_reset();
	lines_reload(&cfg);
	CHECK(cop_has(C_TEARDOWN));
	CHECK_UINT(cop_find(C_TEARDOWN)->line, 0);
	CHECK(state_is(0, "OOS"));

	CASE("a newly enabled channel returns to idle");
	cfg.channel[0].enabled = true;
	lines_reload(&cfg);
	CHECK(state_is(0, "IDLE"));
	view_get(0, &v);
	CHECK(!v.oos);

	CASE("a reload before lines_init does nothing");
	lines_stop();
	cops_reset();
	lines_reload(&cfg);
	CHECK_UINT(ncops, 0);
}

/*
 * The star code plumbing. The suite drives the real features.c through
 * the real machine, so every case dials what the subscriber dials and reads
 * the outcome where the subscriber hears it: the tone, the state, and the
 * runtime toggles the status page publishes.
 */
/* a star code of exactly len characters, for the digit buffer boundary */
static void code_fill(char *dst, size_t len)
{
	memset(dst, '1', len);
	dst[0] = '*';
	dst[len] = 0;
}

static void cfg_star_codes(struct cfg_channel *ch)
{
	snprintf(ch->cw_enable_code, sizeof(ch->cw_enable_code), "*43#");
	snprintf(ch->cw_disable_code, sizeof(ch->cw_disable_code), "#43#");
	snprintf(ch->cw_query_code, sizeof(ch->cw_query_code), "*#43#");
	snprintf(ch->cid_single_call_enable_exten,
		 sizeof(ch->cid_single_call_enable_exten), "*31#");
	snprintf(ch->cid_single_call_disable_exten,
		 sizeof(ch->cid_single_call_disable_exten), "#31#");
}

static void t_features(void)
{
	struct line_view v;

	/* the map is "5", so every prefix of a star code is a dial plan
	 * NO_MATCH and the line would clear without the star code rule */
	CASE("an unfinished star code keeps collecting where the map would clear");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	cfg.channel[0].call_waiting_enabled = false;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "*43");
	CHECK(state_is(0, "COLLECTING"));

	/* the confirmation is immediate, the dial tone behind it takes the same
	 * defer every OFFHOOK_DIAL entry takes, so the order is asserted rather
	 * than the timing */
	CASE("the completed code confirms");
	hops_reset();
	inject_digit(0, '#', false);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_find(H_TONE)->val, HAL_TONE_CONFIRM);

	CASE("and hands the line a fresh dial tone behind it");
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);

	CASE("the toggle it applied is published with the state");
	view_get(0, &v);
	CHECK(v.call_waiting);
	CHECK(strcmp(v.digits, "") == 0);

	CASE("and reaches the guard of a call that starts with no on hook between");
	inject_digit(0, '5', false);
	CHECK(state_is(0, "CALLING"));
	rec_cb.answered(0, 0);
	CHECK(state_is(0, "CONNECTED"));
	cops_reset();
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	CHECK(!cop_has(C_REJECT));

	CASE("the disable code takes call waiting off the next call");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "#43#");
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	view_get(0, &v);
	CHECK(!v.call_waiting);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	CHECK(state_is(0, "CONNECTED"));
	cops_reset();
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CONNECTED"));
	CHECK(cop_has(C_REJECT));
	CHECK_UINT(cop_find(C_REJECT)->val, 486);

	/* the do-not-disturb option is unset in this fixture, so its row
	 * matches nothing and the digits reach the dial plan, as the digits of
	 * every unconfigured code do */
	CASE("an unconfigured code dials through to the network");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	cfg.channel[0].interdigit_s = 1;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "*57");
	uloop_pump(300);
	CHECK(!cop_has(C_DIAL));
	uloop_pump(1000);
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, "*57") == 0);
	CHECK(state_is(0, "CALLING"));

	CASE("a star string no code carries still reaches the dial plan");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "*99");
	cops_reset();
	inject_digit(0, '#', false);
	CHECK(state_is(0, "CALLING"));
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, "*99") == 0);

	/* a code the config leaves to the switch is a plain star string here,
	 * and the digit escape runs once the table stops matching, so a code
	 * that ends in a pound reaches the switch without it: the escape dials
	 * the buffer on the pound (the star_codes_local option) */
	CASE("a code ending in pound reaches the switch without it under the "
	     "seeded escape");
	cfg_default();
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	cops_reset();
	inject_digits(0, "*43#");
	CHECK(state_is(0, "CALLING"));
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, "*43") == 0);

	/* a service the config arms while the config carries no code that
	 * cancels it is the ordinary network-code deployment, so the derive
	 * names it in the trace and never as a warning; the all-forward cancel
	 * clears the busy condition too, so it counts */
	CASE("a service armed from config with no cancel code reports quietly");
	cfg_default();
	cfg.channel[0].do_not_disturb_enabled = true;
	cfg.channel[0].call_fwd_local = true;
	cfg.channel[0].call_fwd_local_mode = 2;
	snprintf(cfg.channel[0].call_fwd_busy_uri,
		 sizeof(cfg.channel[0].call_fwd_busy_uri), "5551111");
	trace_domain_set(TRACE_FSM, true);
	warn_capture_begin();
	fixture_start();
	CHECK_UINT(warn_capture_count("no code cancels it"), 2);
	CHECK_UINT(warn_capture_count("do_not_disturb_enabled arms the service"), 1);
	CHECK_UINT(warn_capture_count("arms the busy forward"), 1);
	CHECK_UINT(warn_capture_count_at(LOG_WARNING, "no code cancels it"), 0);

	/* a reload takes the codes from the new config and checks them again,
	 * so a fault the config still carries is reported once more */
	CASE("a reload takes the star codes again and reports the fault again");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	snprintf(cfg.channel[0].cw_disable_code,
		 sizeof(cfg.channel[0].cw_disable_code), "*43#");
	warn_capture_begin();
	fixture_start();
	CHECK_UINT(warn_capture_count("so neither runs"), 1);
	warn_capture_begin();
	lines_reload(&cfg);
	CHECK_UINT(warn_capture_count("so neither runs"), 1);
	warn_capture_end();

	CASE("only the combined cancel code configured reports nothing");
	cfg_default();
	cfg.channel[0].do_not_disturb_enabled = true;
	cfg.channel[0].call_fwd_local = true;
	cfg.channel[0].call_fwd_local_mode = 2;
	snprintf(cfg.channel[0].call_fwd_busy_uri,
		 sizeof(cfg.channel[0].call_fwd_busy_uri), "5551111");
	snprintf(cfg.channel[0].call_fwd_all_cancel_exten,
		 sizeof(cfg.channel[0].call_fwd_all_cancel_exten), "#004#");
	snprintf(cfg.channel[0].do_not_disturb_off_exten,
		 sizeof(cfg.channel[0].do_not_disturb_off_exten), "#78");
	warn_capture_begin();
	fixture_start();
	CHECK_UINT(warn_capture_count("no code cancels it"), 0);
	warn_capture_end();
	trace_domain_set(TRACE_FSM, false);

	CASE("a one-call caller id code is stored for the call that follows");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "#31#");
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	view_get(0, &v);
	CHECK(v.cid_once_set);
	CHECK(!v.cid_once);

	CASE("the one-call override is cleared when the line reaches IDLE");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	view_get(0, &v);
	CHECK(!v.cid_once_set);

	CASE("a reload leaves the toggle alone until the line rests");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "#43#");
	inject_digit(0, '5', false);
	CHECK(state_is(0, "CALLING"));
	lines_reload(&cfg);
	view_get(0, &v);
	CHECK(!v.call_waiting);

	CASE("and drops it at that rest state, because nothing persists it");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	view_get(0, &v);
	CHECK(v.call_waiting);

	CASE("a reload with the line already at rest drops it at once");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "#43#");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	view_get(0, &v);
	CHECK(!v.call_waiting);
	lines_reload(&cfg);
	view_get(0, &v);
	CHECK(v.call_waiting);

	/*
	 * A code the machine's digit buffer cannot hold. line/line.c blanks it
	 * in its private copy, so the table behaves as if the option were unset
	 * and those digits reach the dial plan as they would with no code
	 * configured.
	 */
	CASE("a code exactly as long as the digit buffer still completes");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	code_fill(cfg.channel[0].cw_enable_code, FSM_DIGITS_MAX);
	cfg.channel[0].call_waiting_enabled = false;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	hops_reset();
	inject_digits(0, cfg.channel[0].cw_enable_code);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK_UINT(hop_find(H_TONE)->val, HAL_TONE_CONFIRM);
	view_get(0, &v);
	CHECK(v.call_waiting);

	CASE("one digit more is ignored, and the digits reach the dial plan");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	code_fill(cfg.channel[0].cw_enable_code, FSM_DIGITS_MAX + 1);
	cfg.channel[0].call_waiting_enabled = false;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '*', false);
	inject_digit(0, '1', false);
	CHECK(state_is(0, "CLEARING"));
	view_get(0, &v);
	CHECK(strcmp(v.clearing, "fastbusy") == 0);

	CASE("and the overlength code applies no toggle");
	CHECK(!v.call_waiting);

	/* features.h "Precedence": a code fires on the digit that completes
	 * it */
	CASE("a complete code fires even when a longer code extends it");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	snprintf(cfg.channel[0].cw_disable_code,
		 sizeof(cfg.channel[0].cw_disable_code), "*1");
	snprintf(cfg.channel[0].cid_single_call_enable_exten,
		 sizeof(cfg.channel[0].cid_single_call_enable_exten), "*12");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	hops_reset();
	inject_digits(0, "*1");
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK_UINT(hop_find(H_TONE)->val, HAL_TONE_CONFIRM);
	view_get(0, &v);
	CHECK(!v.call_waiting);

	CASE("so the longer code cannot be dialled and stores nothing");
	CHECK(!v.cid_once_set);

	CASE("a line with no star codes configured collects digits as before");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "*99");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	cops_reset();
	inject_digits(0, "*99");
	CHECK(state_is(0, "CALLING"));
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, "*99") == 0);
}

/*
 * The confirm tone of a star code plays its region length, and the dial tone
 * starts at its end (line/line.c dial_tone_set()). The line copies the length
 * from the read-back when the confirm tone starts. Each case therefore seeds
 * the fake region before the bring-up and loads it through the region job. A
 * case that changes the region during the confirm tone completes a second job,
 * and a flash then asks for the dial tone again.
 *
 * NZL ships 3000 ms. The cases that check how the record clears take a short
 * length, so the suite waits less. Each wait stops short of the length by the
 * early margin, and the next one passes it by the late margin.
 */
#define CONFIRM_NZL_MS		3000
#define CONFIRM_SHORT_MS	400
#define CONFIRM_EARLY_MS	200
#define CONFIRM_LATE_MS		300
#define CONFIRM_FLASH_MS	1000

static void confirm_cfg(uint32_t total_ms, bool endless)
{
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map),
		 "5");
	fake_region.confirm_total_ms = total_ms;
	fake_region.confirm_endless = endless;
}

/* The disable code applies on a line with call waiting on, so the machine
 * plays the confirm tone and asks for the dial tone behind it. */
static void confirm_reach(void)
{
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "#43");
	hops_reset();
	inject_digit(0, '#', false);
}

static void t_confirm_tone(void)
{
	struct region_view rv;
	struct line_view v;

	CASE("a 3000 ms confirm tone holds the dial tone until it ends");
	confirm_cfg(CONFIRM_NZL_MS, false);
	fixture_start();
	region_ready();
	confirm_reach();
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK_UINT(hop_count(H_TONE), 1);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_CONFIRM);
	uloop_pump(CONFIRM_NZL_MS - CONFIRM_EARLY_MS);
	CHECK_UINT(hop_count(H_TONE), 1);

	CASE("and plays the dial tone when the confirm tone ends");
	uloop_pump(CONFIRM_EARLY_MS + CONFIRM_LATE_MS);
	CHECK_UINT(hop_count(H_TONE), 2);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);

	CASE("the status dump carries the confirm length of the read-back");
	CHECK(region_view_get(&rv));
	CHECK(rv.confirm_total_set);
	CHECK_UINT(rv.confirm_total_ms, CONFIRM_NZL_MS);
	CHECK(rv.confirm_endless_set);
	CHECK(!rv.confirm_endless);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("a confirm tone that repeats until stopped gets no dial tone");
	confirm_cfg(0, true);
	cfg.channel[0].start_digit_s = 1;
	fixture_start();
	region_ready();
	confirm_reach();
	uloop_pump(300);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK_UINT(hop_count(H_TONE), 1);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_CONFIRM);

	CASE("and the start-digit timer ends it with the fast busy");
	uloop_pump(1000);
	CHECK(state_is(0, "CLEARING"));
	view_get(0, &v);
	CHECK(strcmp(v.clearing, "fastbusy") == 0);
	CHECK(hop_has(H_TONE_STOP));
	CHECK_UINT(hop_count(H_TONE), 2);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);

	CASE("the status dump names the endless confirm tone");
	CHECK(region_view_get(&rv));
	CHECK_UINT(rv.confirm_total_ms, 0);
	CHECK(rv.confirm_endless);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("a read-back with no confirm length keeps the usual dial tone");
	confirm_cfg(0, false);
	fixture_start();
	region_ready();
	confirm_reach();
	CHECK_UINT(hop_find(H_TONE)->val, HAL_TONE_CONFIRM);
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK_UINT(hop_count(H_TONE), 2);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("a digit during the confirm tone stops it and cancels the dial "
	     "tone");
	confirm_cfg(CONFIRM_SHORT_MS, false);
	fixture_start();
	region_ready();
	confirm_reach();
	inject_digit(0, '5', false);
	CHECK(state_is(0, "CALLING"));
	CHECK(hop_has(H_TONE_STOP));
	uloop_pump(CONFIRM_SHORT_MS + CONFIRM_LATE_MS);
	CHECK_UINT(hop_count(H_TONE), 1);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("a flash after the confirm tone plays the dial tone at once");
	confirm_cfg(CONFIRM_SHORT_MS, false);
	fixture_start();
	region_ready();
	confirm_reach();
	uloop_pump(CONFIRM_SHORT_MS + CONFIRM_LATE_MS);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);
	hops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK_UINT(hop_count(H_TONE), 1);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("a flash during the confirm tone waits only for the rest of it");
	confirm_cfg(CONFIRM_NZL_MS, false);
	fixture_start();
	region_ready();
	confirm_reach();
	uloop_pump(CONFIRM_FLASH_MS);
	hops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	uloop_pump(CONFIRM_NZL_MS - CONFIRM_FLASH_MS - CONFIRM_EARLY_MS);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK_UINT(hop_count(H_TONE), 0);
	uloop_pump(CONFIRM_EARLY_MS + CONFIRM_LATE_MS);
	CHECK_UINT(hop_count(H_TONE), 1);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("a release during the confirm tone stops it and plays no dial "
	     "tone");
	confirm_cfg(CONFIRM_SHORT_MS, false);
	fixture_start();
	region_ready();
	confirm_reach();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK(hop_has(H_TONE_STOP));
	uloop_pump(CONFIRM_SHORT_MS + CONFIRM_LATE_MS);
	CHECK_UINT(hop_count(H_TONE), 1);

	CASE("and the next seize plays its dial tone without the confirm wait");
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK_UINT(hop_count(H_TONE), 1);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("a region that loads late replays the confirm tone before the "
	     "dial tone");
	confirm_cfg(CONFIRM_SHORT_MS, false);
	fixture_start();
	confirm_reach();
	uloop_pump(DIAL_TONE_PUMP_MS);
	hops_reset();
	job_complete(HAL_OK);
	CHECK_UINT(hop_count(H_TONE), 1);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_CONFIRM);
	uloop_pump(CONFIRM_SHORT_MS - CONFIRM_EARLY_MS);
	CHECK_UINT(hop_count(H_TONE), 1);

	CASE("and plays the dial tone when the replayed confirm tone ends");
	uloop_pump(CONFIRM_EARLY_MS + CONFIRM_LATE_MS);
	CHECK_UINT(hop_count(H_TONE), 2);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("a load that succeeds after a failed one replays the confirm "
	     "tone");
	confirm_cfg(CONFIRM_SHORT_MS, false);
	fixture_start();
	job_complete(HAL_TRANSIENT);
	confirm_reach();
	uloop_pump(CONFIRM_SHORT_MS + CONFIRM_LATE_MS);
	lines_reload(&cfg);
	CHECK(job_pending != NULL);
	hops_reset();
	job_complete(HAL_OK);
	CHECK_UINT(hop_count(H_TONE), 1);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_CONFIRM);

	CASE("and the dial tone follows the end of that confirm tone");
	uloop_pump(CONFIRM_SHORT_MS - CONFIRM_EARLY_MS);
	CHECK_UINT(hop_count(H_TONE), 1);
	uloop_pump(CONFIRM_EARLY_MS + CONFIRM_LATE_MS);
	CHECK_UINT(hop_count(H_TONE), 2);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("an endless region that loads during a 3000 ms confirm tone keeps "
	     "the dial tone at 3000 ms");
	confirm_cfg(CONFIRM_NZL_MS, false);
	fixture_start();
	region_ready();
	confirm_reach();
	uloop_pump(CONFIRM_FLASH_MS);
	fake_region.confirm_total_ms = 0;
	fake_region.confirm_endless = true;
	region_change("AUS");
	CHECK(region_view_get(&rv));
	CHECK(rv.confirm_endless);
	hops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	uloop_pump(CONFIRM_NZL_MS - CONFIRM_FLASH_MS - CONFIRM_EARLY_MS);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK_UINT(hop_count(H_TONE), 0);
	uloop_pump(CONFIRM_EARLY_MS + CONFIRM_LATE_MS);
	CHECK_UINT(hop_count(H_TONE), 1);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("a 3000 ms region that loads during a 400 ms confirm tone keeps "
	     "the dial tone at 400 ms");
	confirm_cfg(CONFIRM_SHORT_MS, false);
	fixture_start();
	region_ready();
	confirm_reach();
	fake_region.confirm_total_ms = CONFIRM_NZL_MS;
	region_change("NZL");
	CHECK(region_view_get(&rv));
	CHECK_UINT(rv.confirm_total_ms, CONFIRM_NZL_MS);
	hops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	uloop_pump(CONFIRM_SHORT_MS - CONFIRM_EARLY_MS);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK_UINT(hop_count(H_TONE), 0);
	uloop_pump(CONFIRM_EARLY_MS + CONFIRM_LATE_MS);
	CHECK_UINT(hop_count(H_TONE), 1);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
}

/*
 * Do not disturb and local call forwarding through the whole shell: the
 * config the loader would hand over, the codes a subscriber dials, the state
 * the status page publishes, and the leg command the offer ends in.
 */
static void cfg_dnd_cf_codes(struct cfg_channel *ch)
{
	cfg_star_codes(ch);
	snprintf(ch->do_not_disturb_on_exten, sizeof(ch->do_not_disturb_on_exten),
		 "*78");
	snprintf(ch->do_not_disturb_off_exten, sizeof(ch->do_not_disturb_off_exten),
		 "#78");
	snprintf(ch->call_fwd_uncond_exten, sizeof(ch->call_fwd_uncond_exten), "*21#");
	snprintf(ch->call_fwd_uncond_cancel_exten,
		 sizeof(ch->call_fwd_uncond_cancel_exten), "#21#");
	snprintf(ch->call_fwd_uncond_query_exten,
		 sizeof(ch->call_fwd_uncond_query_exten), "*#21#");
	snprintf(ch->call_fwd_busy_exten, sizeof(ch->call_fwd_busy_exten), "*67#");
	snprintf(ch->call_fwd_no_answer_exten, sizeof(ch->call_fwd_no_answer_exten),
		 "*61#");
	ch->call_fwd_local = true;
	ch->call_fwd_no_answer_timeout = 15;
}

/* the thirteen forward codes, for the cases that need every one of them */
static void cfg_dnd_cf_codes_all(struct cfg_channel *ch)
{
	cfg_dnd_cf_codes(ch);
	snprintf(ch->call_fwd_busy_cancel_exten,
		 sizeof(ch->call_fwd_busy_cancel_exten), "#67#");
	snprintf(ch->call_fwd_busy_query_exten,
		 sizeof(ch->call_fwd_busy_query_exten), "*#67#");
	snprintf(ch->call_fwd_no_answer_cancel_exten,
		 sizeof(ch->call_fwd_no_answer_cancel_exten), "#61#");
	snprintf(ch->call_fwd_no_answer_query_exten,
		 sizeof(ch->call_fwd_no_answer_query_exten), "*#61#");
	snprintf(ch->call_fwd_unreachable_exten,
		 sizeof(ch->call_fwd_unreachable_exten), "*62#");
	snprintf(ch->call_fwd_unreachable_cancel_exten,
		 sizeof(ch->call_fwd_unreachable_cancel_exten), "#62#");
	snprintf(ch->call_fwd_all_exten, sizeof(ch->call_fwd_all_exten), "*004#");
	snprintf(ch->call_fwd_all_cancel_exten,
		 sizeof(ch->call_fwd_all_cancel_exten), "#004#");
}

static void t_dnd_cf(void)
{
	struct line_view v;

	CASE("the do not disturb code confirms and reaches the status page");
	cfg_default();
	cfg_dnd_cf_codes(&cfg.channel[0]);
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	hops_reset();
	inject_digits(0, "*78");
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK_UINT(hop_find(H_TONE)->val, HAL_TONE_CONFIRM);
	view_get(0, &v);
	CHECK(v.dnd);

	CASE("and the next offer is refused busy instead of ringing the phone");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	cops_reset();
	hops_reset();
	inject_incoming(0, 0, "5552000", "Bob", 0);
	CHECK(state_is(0, "IDLE"));
	CHECK(cop_has(C_REJECT));
	CHECK_UINT(cop_find(C_REJECT)->val, 486);
	CHECK(!hop_has(H_RING_START));

	CASE("the off code lifts it and the phone rings again");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "#78");
	view_get(0, &v);
	CHECK(!v.dnd);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	cops_reset();
	hops_reset();
	inject_incoming(0, 0, "5552000", "Bob", 0);
	CHECK(state_is(0, "RINGING"));
	CHECK(hop_has(H_RING_START));

	CASE("the config seeds do not disturb without a code being dialled");
	cfg_default();
	cfg_dnd_cf_codes(&cfg.channel[0]);
	cfg.channel[0].do_not_disturb_enabled = true;
	fixture_start();
	view_get(0, &v);
	CHECK(v.dnd);
	cops_reset();
	inject_incoming(0, 0, "5552000", "Bob", 0);
	CHECK(cop_has(C_REJECT));

	/* The second stage, end to end: the code, the recall dial tone, the
	 * target dialled behind it and the '#' that ends it */
	CASE("the unconditional code collects its target behind the recall tone");
	cfg_default();
	cfg_dnd_cf_codes(&cfg.channel[0]);
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	hops_reset();
	inject_digits(0, "*21#");
	CHECK(state_is(0, "SPRE_COLLECT"));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_SECOND_DIAL);
	inject_digits(0, "5551234");
	CHECK(state_is(0, "SPRE_COLLECT"));
	hops_reset();
	inject_digit(0, '#', false);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK_UINT(hop_find(H_TONE)->val, HAL_TONE_CONFIRM);

	CASE("the target it collected is the one the status page publishes");
	view_get(0, &v);
	CHECK(v.cf_uncond_set);
	CHECK(strcmp(v.cf_uncond, "5551234") == 0);
	CHECK(!v.cf_busy_set);

	CASE("and the next offer is redirected to it, with no ring at all");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	cops_reset();
	hops_reset();
	inject_incoming(0, 0, "5552000", "Bob", 0);
	CHECK(state_is(0, "IDLE"));
	CHECK(cop_has(C_FORWARD));
	CHECK(strcmp(cop_find(C_FORWARD)->num, "5551234") == 0);
	CHECK_UINT(cop_find(C_FORWARD)->line, 0);
	CHECK(!cop_has(C_RING));
	CHECK(!hop_has(H_RING_START));

	CASE("the query code answers CONFIRM while it is armed");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	hops_reset();
	inject_digits(0, "*#21#");
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK_UINT(hop_find(H_TONE)->val, HAL_TONE_CONFIRM);

	CASE("the cancel code disarms it, and the query then clears fast busy");
	inject_digits(0, "#21#");
	view_get(0, &v);
	CHECK(!v.cf_uncond_set);
	hops_reset();
	inject_digits(0, "*#21#");
	CHECK(state_is(0, "CLEARING"));
	view_get(0, &v);
	CHECK(strcmp(v.clearing, "fastbusy") == 0);

	CASE("an empty target falls back to the configured uri");
	cfg_default();
	cfg_dnd_cf_codes(&cfg.channel[0]);
	snprintf(cfg.channel[0].call_fwd_uncond_uri,
		 sizeof(cfg.channel[0].call_fwd_uncond_uri), "5559999");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "*21##");
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	view_get(0, &v);
	CHECK(strcmp(v.cf_uncond, "5559999") == 0);

	CASE("with no configured uri the same code clears fast busy");
	cfg_default();
	cfg_dnd_cf_codes(&cfg.channel[0]);
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "*21##");
	CHECK(state_is(0, "CLEARING"));
	view_get(0, &v);
	CHECK(!v.cf_uncond_set);

	CASE("the config arms a condition on its own, with no code dialled");
	cfg_default();
	cfg_dnd_cf_codes(&cfg.channel[0]);
	cfg.channel[0].call_fwd_local_mode = 1;
	snprintf(cfg.channel[0].call_fwd_uncond_uri,
		 sizeof(cfg.channel[0].call_fwd_uncond_uri), "5551111");
	fixture_start();
	view_get(0, &v);
	CHECK(v.cf_uncond_set);
	CHECK(strcmp(v.cf_uncond, "5551111") == 0);
	cops_reset();
	inject_incoming(0, 0, "5552000", "Bob", 0);
	CHECK(cop_has(C_FORWARD));
	CHECK(strcmp(cop_find(C_FORWARD)->num, "5551111") == 0);

	/* the gate: with the service on the network the codes dial through and
	 * nothing local is armed, whatever the mode says */
	/*
	 * All thirteen codes, one case: a single pin would let twelve of the
	 * blanking statements vanish unseen. The map admits every symbol and
	 * the escape is off, so each code reaches the dial plan whole and its
	 * interdigit timer dials it. A code that were still a table row would
	 * confirm, open the second stage or answer the fast busy instead, and
	 * no C_DIAL would carry that string.
	 */
	CASE("call_fwd_local 0 dials every one of the thirteen codes through");
	cfg_default();
	cfg_dnd_cf_codes_all(&cfg.channel[0]);
	cfg.channel[0].call_fwd_local = false;
	cfg.channel[0].call_fwd_local_mode = 1;
	cfg.channel[0].digit_esc = 0;
	cfg.channel[0].interdigit_s = 1;
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map),
		 "[0-9*#].T");
	snprintf(cfg.channel[0].call_fwd_uncond_uri,
		 sizeof(cfg.channel[0].call_fwd_uncond_uri), "5551111");
	fixture_start();
	view_get(0, &v);
	CHECK(!v.cf_uncond_set);
	{
		static const char *const codes[] = {
			"*21#", "#21#", "*#21#", "*67#", "#67#", "*#67#",
			"*61#", "#61#", "*#61#", "*62#", "#62#", "*004#", "#004#",
		};
		unsigned int i;

		CHECK_UINT(sizeof(codes) / sizeof(codes[0]), 13);
		for (i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
			inject_hook(0, HAL_HOOK_SEIZE, 0);
			cops_reset();
			hops_reset();
			inject_digits(0, codes[i]);
			uloop_pump(1300);
			CHECK(state_is(0, "CALLING"));
			CHECK(cop_has(C_DIAL));
			CHECK(strcmp(cop_find(C_DIAL)->num, codes[i]) == 0);
			CHECK(!hop_has(H_TONE) ||
			      hop_last(H_TONE)->val != HAL_TONE_CONFIRM);
			inject_hook(0, HAL_HOOK_RELEASE, 0);
		}
	}
	inject_hook(0, HAL_HOOK_SEIZE, 0);

	CASE("and no offer is redirected on such a line");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	cops_reset();
	inject_incoming(0, 0, "5552000", "Bob", 0);
	CHECK(state_is(0, "RINGING"));
	CHECK(!cop_has(C_FORWARD));

	/* the do-not-disturb codes are not gated */
	CASE("the do not disturb code stays local with the forward service off");
	/* answer and end the call the case above let through */
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(state_is(0, "CONNECTED"));
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	hops_reset();
	inject_digits(0, "*78");
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK_UINT(hop_find(H_TONE)->val, HAL_TONE_CONFIRM);
	view_get(0, &v);
	CHECK(v.dnd);

	CASE("a busy forward takes the 486 of a do not disturb refusal");
	cfg_default();
	cfg_dnd_cf_codes(&cfg.channel[0]);
	cfg.channel[0].do_not_disturb_enabled = true;
	cfg.channel[0].call_fwd_local_mode = 2;
	snprintf(cfg.channel[0].call_fwd_busy_uri,
		 sizeof(cfg.channel[0].call_fwd_busy_uri), "5552222");
	fixture_start();
	cops_reset();
	inject_incoming(0, 0, "5552000", "Bob", 0);
	CHECK(!cop_has(C_REJECT));
	CHECK(cop_has(C_FORWARD));
	CHECK(strcmp(cop_find(C_FORWARD)->num, "5552222") == 0);

	CASE("the ring no answer window redirects a call nobody answered");
	cfg_default();
	cfg_dnd_cf_codes(&cfg.channel[0]);
	cfg.channel[0].call_fwd_local_mode = 4;
	cfg.channel[0].call_fwd_no_answer_timeout = 1;
	snprintf(cfg.channel[0].call_fwd_no_answer_uri,
		 sizeof(cfg.channel[0].call_fwd_no_answer_uri), "5553333");
	fixture_start();
	cops_reset();
	inject_incoming(0, 0, "5552000", "Bob", 0);
	CHECK(state_is(0, "RINGING"));
	uloop_pump(300);
	CHECK(!cop_has(C_FORWARD));
	uloop_pump(1000);
	CHECK(cop_has(C_FORWARD));
	CHECK(strcmp(cop_find(C_FORWARD)->num, "5553333") == 0);
	CHECK(state_is(0, "IDLE"));

	CASE("a line without the service rings on and on");
	cfg_default();
	cfg_dnd_cf_codes(&cfg.channel[0]);
	fixture_start();
	cops_reset();
	inject_incoming(0, 0, "5552000", "Bob", 0);
	CHECK(state_is(0, "RINGING"));
	uloop_pump(2000);
	CHECK(!cop_has(C_FORWARD));
	CHECK(state_is(0, "RINGING"));

	/*
	 * A condition the config arms with no uri to send the call to. The
	 * machine lets the offer through, and the operator hears about it
	 * exactly once per reload, per condition.
	 */
	CASE("a condition armed with no target warns once at the derive");
	cfg_default();
	cfg_dnd_cf_codes(&cfg.channel[0]);
	cfg.channel[0].call_fwd_local_mode = 1;
	warn_capture_begin();
	fixture_start();
	CHECK_UINT(warn_capture_count("call_fwd_uncond_uri"), 1);

	CASE("and the offer it cannot forward rings the phone as usual");
	view_get(0, &v);
	CHECK(v.cf_uncond_set);
	CHECK(strcmp(v.cf_uncond, "") == 0);
	cops_reset();
	inject_incoming(0, 0, "5552000", "Bob", 0);
	CHECK(state_is(0, "RINGING"));
	CHECK(!cop_has(C_FORWARD));
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("a config that arms nothing warns nothing");
	cfg_default();
	cfg_dnd_cf_codes(&cfg.channel[0]);
	warn_capture_begin();
	fixture_start();
	CHECK_UINT(warn_capture_count("call_fwd_uncond_uri"), 0);

	/* mode 6 arms two conditions and only one of them has a target, so the
	 * warning names that one and stays quiet about the other. The capture
	 * is one shot, so each needle gets its own derive. */
	CASE("the warning names the condition that has no target");
	cfg_default();
	cfg_dnd_cf_codes(&cfg.channel[0]);
	cfg.channel[0].call_fwd_local_mode = 6;
	snprintf(cfg.channel[0].call_fwd_busy_uri,
		 sizeof(cfg.channel[0].call_fwd_busy_uri), "5552222");
	warn_capture_begin();
	fixture_start();
	CHECK_UINT(warn_capture_count("call_fwd_no_answer_uri"), 1);

	CASE("and stays quiet about the one that has it");
	warn_capture_begin();
	fixture_start();
	CHECK_UINT(warn_capture_count("call_fwd_busy_uri"), 0);
	warn_capture_end();

	CASE("a reload drops what the subscriber dialled and seeds the config");
	cfg_default();
	cfg_dnd_cf_codes(&cfg.channel[0]);
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "*21#5551234#");
	inject_digits(0, "*78");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	view_get(0, &v);
	CHECK(v.cf_uncond_set);
	CHECK(v.dnd);
	cfg.channel[0].call_fwd_local_mode = 2;
	snprintf(cfg.channel[0].call_fwd_busy_uri,
		 sizeof(cfg.channel[0].call_fwd_busy_uri), "5552222");
	lines_reload(&cfg);
	uloop_pump(100);
	view_get(0, &v);
	CHECK(!v.cf_uncond_set);
	CHECK(!v.dnd);
	CHECK(v.cf_busy_set);
	CHECK(strcmp(v.cf_busy, "5552222") == 0);
}

/*
 * The transfer plumbing. Three things belong to this shell and are pinned
 * here: the star-code row mapped onto the call.c kind, the two refusals the
 * leg layer answers synchronously, and the outcome callback mapped onto the
 * two FSM events. The cells themselves belong to the matrix and test_fsm pins
 * them; every case below drives the real machine, so the plumbing and the
 * cell it feeds cannot drift apart.
 *
 * The faults these cases catch, and the case that catches each:
 *  1. xfer_kind_of() answers CALL_XFER_BLIND for the attended row, or swaps the
 *     two arms: "the attended row maps to the other kind".
 *  2. op_call_xfer() drops the injection on a false return: "a refused transfer
 *     resumes the held leg inside the same frame".
 *  3. op_call_xfer() drops the fsm_depth raise from around the injection: "a
 *     refusal from the short interdigit timer resumes the held leg", which is
 *     the one frame where the raise decides the outcome.
 *  4. op_call_xfer() names slot 0 rather than the slot it was given, in the
 *     call or in the injection: "the subject leg is the one the machine held,
 *     not slot 0" and "a refusal on that leg resumes that leg".
 *  5. op_call_xfer() or op_call_xfer_consult() names line 0 rather than its own
 *     line: "the op names the line the gesture was made on" and "and the
 *     consult names it too".
 *  6. op_call_xfer_consult() hangs the referee up alone, or hangs one leg up
 *     twice: "a refused consult transfer clears both legs".
 *  7. op_call_xfer_consult() swaps its two slots: the case that refers the
 *     held leg to the active one.
 *  8. cb_xfer_done() injects XFER_OK for both polarities: "a failed outcome
 *     resumes the held leg".
 *  9. cb_xfer_done() passes anything but the slot as the event argument: "an
 *     outcome for another slot is inert" and "and the outcome of that leg is
 *     the one the machine takes".
 * 10. line_cfg_derive() ORs the two on-hook options instead of ANDing them:
 *     "flash service mode 1 consults nobody" and "conference mode 1 consults
 *     nobody either".
 * 11. line_cfg_derive() reads local_conf_enabled != 0 instead of == 2:
 *     "conference mode 1 consults nobody either".
 */
static void cfg_xfer_codes(struct cfg_channel *ch)
{
	snprintf(ch->blind_transfer_exten, sizeof(ch->blind_transfer_exten), "*072");
	snprintf(ch->attended_transfer_exten, sizeof(ch->attended_transfer_exten),
		 "*073");
	snprintf(ch->consult_transfer_exten, sizeof(ch->consult_transfer_exten),
		 "*074");
}

/*
 * The transfer fixture. The map admits "5" for a first call and any three
 * digits behind a 9 for a transfer target, so a collected target is asserted as
 * a whole string rather than as one digit that could have come from anywhere.
 * The flash service stays at the fixture default, drop, which is what opens
 * the flash-hold path every gesture below starts from.
 *
 * Both lines are configured the same way, because the op carries a line beside
 * its slot and a shell that named the wrong one would still pass every case
 * driven on line 0 alone.
 */
static void xfer_cfg(unsigned int local_conf)
{
	unsigned int i;

	cfg_default();

	for (i = 0; i < 2; i++) {
		cfg_channel_default(&cfg.channel[i], i, true);
		cfg_xfer_codes(&cfg.channel[i]);
		snprintf(cfg.channel[i].digit_map, sizeof(cfg.channel[i].digit_map),
			 "5|9xx");
		cfg.channel[i].local_conf_enabled = local_conf;
	}
}

/*
 * An answered inbound call, held by the flash, with the recall dial tone
 * running: the HOLD_DIAL line every transfer gesture starts from. The slot is
 * a parameter, because the subject of a transfer is whichever leg the machine
 * held and call.c hands out both.
 */
static void xfer_held_call(unsigned int line, unsigned int slot)
{
	inject_incoming(line, slot, "5551000", "Alice", 0);
	inject_hook(line, HAL_HOOK_SEIZE, 0);
	inject_hook(line, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(line, slot, true);
	cops_reset();
}

/*
 * The TWO_CALLS shape that the direction rule names: the held leg is the call
 * this subscriber took, the active one is the party they dialled from the
 * recall dial tone.
 */
static void xfer_two_calls(unsigned int line)
{
	xfer_held_call(line, 0);
	inject_digit(line, '5', false);
	rec_cb.answered(line, 1);
	cops_reset();
}

/* the line in PENDING{XFER}: the blind row referred the held leg to "955" */
static void xfer_pending(unsigned int line, unsigned int slot)
{
	xfer_held_call(line, slot);
	inject_digits(line, "*072");
	inject_digits(line, "955");
	cops_reset();
}

/*
 * The ringback gate of RFC 3960 3.2 (line/line.c early_rx_arm()). An 18x whose
 * answer started the early stream says nothing about audio. The tone keeps
 * playing, and the first packet of that stream stops it. The gate is one bool
 * per leg, so every case below asks which leg the report named.
 *
 * The group runs after t_call_events() and sits here because its second-call
 * case drives the held-call fixture above.
 */
static void t_early_media(void)
{
	struct call_ended info;

	/* the leg release report, whose only reader here is the gate */
	memset(&info, 0, sizeof(info));
	info.number = "5";
	info.remote_number = "";
	info.remote_name = "";

	cfg_default();
	snprintf(cfg.channel[0].digit_map,
		 sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();

	CASE("an 18x answer arms the gate and leaves the ringback playing");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	hops_reset();
	rec_cb.proceeding(0, 0, true);
	CHECK(state_is(0, "PROCEEDING"));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_RINGBACK);
	CHECK_UINT(hop_count(H_TONE_STOP), 0);

	CASE("the first packet of that stream stops it");
	hops_reset();
	inject_rtp_first_rx(0, 0);
	CHECK(hop_count(H_TONE_STOP) >= 1);
	CHECK(state_is(0, "PROCEEDING"));

	CASE("the gate is cleared, so a second report changes nothing");
	hops_reset();
	inject_rtp_first_rx(0, 0);
	CHECK_UINT(nhops, 0);

	CASE("an 18x without an answer arms nothing");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	hops_reset();
	rec_cb.proceeding(0, 0, false);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_RINGBACK);
	hops_reset();
	inject_rtp_first_rx(0, 0);
	CHECK_UINT(nhops, 0);

	CASE("a packet on another leg leaves the ringback alone");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.proceeding(0, 0, true);
	hops_reset();
	inject_rtp_first_rx(0, 1);
	CHECK_UINT(nhops, 0);

	CASE("the answer clears the gate before the stream is programmed "
	     "again");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.proceeding(0, 0, true);
	rec_cb.answered(0, 0);
	CHECK(state_is(0, "CONNECTED"));
	hops_reset();
	inject_rtp_first_rx(0, 0);
	CHECK_UINT(nhops, 0);

	/* the end runs alone here for the reason the case below states */
	CASE("a leg that ended arms nothing for the next call on its slot");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.proceeding(0, 0, true);
	rec_cb.ended(0, 0, &info);
	CHECK(state_is(0, "PROCEEDING"));
	hops_reset();
	inject_rtp_first_rx(0, 0);
	CHECK_UINT(nhops, 0);

	/*
	 * cb.ended names the leg's own slot and cb.failed names the UI one
	 * (call.h, the two names of a slot), so the two reports carry
	 * different numbers as soon as a transfer moves the name.
	 *
	 * No failure report runs in front of the end here. cb_ended() is
	 * invisible to the machine, so the state stays PROCEEDING and a gate
	 * the end did not clear would still stop the tone.
	 */
	CASE("a leg that ended under a moved name clears its own gate");
	fixture_start();
	xfer_stream_map(0, 1);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.proceeding(0, 0, true);
	rec_cb.ended(0, 1, &info);
	CHECK(state_is(0, "PROCEEDING"));
	hops_reset();
	inject_rtp_first_rx(0, 1);
	CHECK_UINT(nhops, 0);

	CASE("a first packet with no gate open reaches no machine");
	fixture_start();
	hops_reset();
	inject_rtp_first_rx(0, 0);
	CHECK_UINT(nhops, 0);
	CHECK(state_is(0, "IDLE"));

	CASE("the second call holds its own gate");
	xfer_cfg(0);
	fixture_start();
	xfer_held_call(0, 0);
	inject_digit(0, '5', false);
	CHECK(state_is(0, "CALLING"));
	rec_cb.proceeding(0, 1, true);
	CHECK(state_is(0, "PROCEEDING"));
	hops_reset();
	inject_rtp_first_rx(0, 0);
	CHECK_UINT(nhops, 0);
	inject_rtp_first_rx(0, 1);
	CHECK(hop_count(H_TONE_STOP) >= 1);

	CASE("the report names the leg, so a moved name still stops the "
	     "ringback");
	cfg_default();
	snprintf(cfg.channel[0].digit_map,
		 sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	xfer_stream_map(0, 1);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.proceeding(0, 0, true);
	hops_reset();
	inject_rtp_first_rx(0, 1);
	CHECK(hop_count(H_TONE_STOP) >= 1);
	hops_reset();
	inject_rtp_first_rx(0, 0);
	CHECK_UINT(nhops, 0);

	/*
	 * The slot_ui_of() return of CALL_SLOTS is not reachable from here:
	 * the map above leaves both UI slots on stream 1, so a packet on
	 * stream 0 meets the unarmed gate first. It is the cb_started()
	 * precedent and stays untested.
	 */
	CASE("a packet for a stream the core carries no slot for is dropped");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.proceeding(0, 0, true);
	hops_reset();
	inject_rtp_first_rx(0, CALL_SLOTS);
	CHECK_UINT(nhops, 0);
}

static void t_xfer(void)
{
	/*
	 * The wiring assertion, and it covers both ops. Neither is reachable
	 * from this suite as a pointer, because the vtable is file static in
	 * line/line.c. The machine reads both, and its HOLD_DIAL transfer hook
	 * check (fsm.c xfer_hooks_present) passes only when call_xfer and
	 * call_xfer_consult are both wired, so a line that reaches
	 * XFER_COLLECT from the star code has proved the pair. Which entry
	 * each op calls is proved by the cases below it.
	 */
	/*
	 * The other half of the same wiring, in the shape t_media_ops takes for
	 * the entropy op: a shell that left this callback empty would strand
	 * every transfer in PENDING until the subscriber hung up, with nothing
	 * failing anywhere.
	 */
	CASE("the leg layer is handed the transfer outcome callback");
	xfer_cfg(0);
	fixture_start();
	CHECK(rec_cb.xfer_done != NULL);

	CASE("the star code opens the second stage, which needs both ops wired");
	xfer_held_call(0, 0);
	CHECK(state_is(0, "HOLD_DIAL"));
	inject_digits(0, "*072");
	CHECK(state_is(0, "XFER_COLLECT"));

	CASE("the blind row refers the held leg with the target it collected");
	inject_digits(0, "955");
	CHECK(cop_has(C_XFER));
	CHECK_UINT(cop_find(C_XFER)->line, 0);
	CHECK_UINT(cop_find(C_XFER)->slot, 0);
	CHECK_UINT(cop_find(C_XFER)->val, CALL_XFER_BLIND);
	CHECK(strcmp(cop_find(C_XFER)->num, "955") == 0);

	/* one gesture is one REFER: a wrapper that referred again on its way
	 * into PENDING would put a second request on a leg call.h refuses one
	 * for, and the machine would still read the state as clean */
	CASE("and it refers exactly once");
	CHECK_UINT(cop_count(C_XFER), 1);

	CASE("and the machine waits for the outcome, holding the leg");
	CHECK(state_is(0, "PENDING"));
	CHECK(!cop_has(C_HANGUP));
	CHECK(!cop_has(C_RESUME));

	/* the '#' escape is the other way a target completes, and it carries
	 * the buffer to the op exactly as a dial plan verdict does */
	CASE("the attended row maps to the other kind");
	xfer_cfg(0);
	fixture_start();
	xfer_held_call(0, 0);
	inject_digits(0, "*073");
	inject_digit(0, '9', false);
	inject_digit(0, '#', false);
	CHECK(cop_has(C_XFER));
	CHECK_UINT(cop_find(C_XFER)->slot, 0);
	CHECK_UINT(cop_find(C_XFER)->val, CALL_XFER_ATTENDED);
	CHECK(strcmp(cop_find(C_XFER)->num, "9") == 0);
	CHECK(state_is(0, "PENDING"));

	/*
	 * The subject is whichever leg the machine held. call.c hands out both
	 * slots, and a wrapper that named slot 0 would refer the wrong call, or
	 * none at all, on every line whose inbound leg landed in slot 1.
	 */
	CASE("the subject leg is the one the machine held, not slot 0");
	xfer_cfg(0);
	fixture_start();
	xfer_held_call(0, 1);
	inject_digits(0, "*072");
	inject_digits(0, "955");
	CHECK(cop_has(C_XFER));
	CHECK_UINT(cop_find(C_XFER)->slot, 1);
	CHECK(state_is(0, "PENDING"));

	CASE("and the outcome of that leg is the one the machine takes");
	rec_cb.xfer_done(0, 1, true, 202);
	CHECK(state_is(0, "OFFHOOK_DIAL"));

	CASE("a refusal on that leg resumes that leg");
	xfer_cfg(0);
	fixture_start();
	xfer_held_call(0, 1);
	xfer_refuse = true;
	inject_digits(0, "*072");
	inject_digits(0, "955");
	CHECK_UINT(cop_find(C_XFER)->slot, 1);
	CHECK(cop_has(C_RESUME));
	CHECK_UINT(cop_find(C_RESUME)->slot, 1);
	CHECK(state_is(0, "PENDING"));

	CASE("and that leg is the call again");
	rec_cb.reinvite_done(0, 1, true);
	CHECK(state_is(0, "CONNECTED"));
	xfer_refuse = false;

	/* the op carries a line as well as a slot, and every case above is
	 * driven on line 0, where a hard-coded line would look correct */
	CASE("the op names the line the gesture was made on");
	xfer_cfg(0);
	fixture_start();
	xfer_held_call(1, 0);
	inject_digits(1, "*072");
	inject_digits(1, "955");
	CHECK(cop_has(C_XFER));
	CHECK_UINT(cop_find(C_XFER)->line, 1);
	CHECK_UINT(cop_find(C_XFER)->slot, 0);
	CHECK(state_is(1, "PENDING"));
	CHECK(state_is(0, "IDLE"));

	CASE("and the consult names it too");
	xfer_cfg(2);
	fixture_start();
	xfer_two_calls(1);
	inject_hook(1, HAL_HOOK_RELEASE, 0);
	CHECK(cop_has(C_XFER_CONSULT));
	CHECK_UINT(cop_find(C_XFER_CONSULT)->line, 1);
	CHECK_UINT(cop_find(C_XFER_CONSULT)->slot, 0);
	CHECK_UINT(cop_find(C_XFER_CONSULT)->val, 1);
	CHECK(state_is(1, "IDLE"));

	CASE("an ok outcome takes the line to a fresh dial tone");
	xfer_cfg(0);
	fixture_start();
	xfer_pending(0, 0);
	hops_reset();
	rec_cb.xfer_done(0, 0, true, 202);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
#ifdef UVOICE_HAL_STUB
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);
#else
	/* line/line.c tone_set(): a dial class tone waits
	 * LINE_DIAL_TONE_DEFER_MS on every real HAL, so no tone leaves in this
	 * event */
	CHECK_UINT(hop_count(H_TONE), 0);
#endif
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);
	CHECK_UINT(hop_last(H_TONE)->line, 0);

	/* call.h: call.c has already BYEd and released that leg, so the shell
	 * owes it neither a hangup nor a resume */
	CASE("and the leg it referred is neither resumed nor hung up");
	CHECK(!cop_has(C_RESUME));
	CHECK(!cop_has(C_HANGUP));

	/* call.h: an ok outcome is the last word on that slot, so anything that
	 * still arrives for it describes a leg this line no longer owns */
	CASE("a late outcome for the slot it released is inert");
	rec_cb.xfer_done(0, 0, true, 202);
	rec_cb.xfer_done(0, 0, false, 486);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK(!cop_has(C_RESUME));

	CASE("a failed outcome resumes the held leg");
	xfer_cfg(0);
	fixture_start();
	xfer_pending(0, 0);
	rec_cb.xfer_done(0, 0, false, 486);
	CHECK(cop_has(C_RESUME));
	CHECK_UINT(cop_find(C_RESUME)->slot, 0);
	CHECK(state_is(0, "PENDING"));

	CASE("and the resumed leg is the call again");
	rec_cb.reinvite_done(0, 0, true);
	CHECK(state_is(0, "CONNECTED"));

	CASE("an outcome for another slot is inert");
	xfer_cfg(0);
	fixture_start();
	xfer_pending(0, 0);
	rec_cb.xfer_done(0, 1, false, 486);
	rec_cb.xfer_done(0, 1, true, 202);
	CHECK(state_is(0, "PENDING"));
	CHECK(!cop_has(C_RESUME));

	/*
	 * A refused request made no wire exchange, so no outcome will ever end
	 * the record the machine just entered: the shell owes it the failure.
	 * The resume is asserted with no uloop pass between the digit and the
	 * check, so the injection was drained by the frame that made it.
	 */
	CASE("a refused transfer resumes the held leg inside the same frame");
	xfer_cfg(0);
	fixture_start();
	xfer_held_call(0, 0);
	xfer_refuse = true;
	inject_digits(0, "*072");
	inject_digits(0, "955");
	CHECK(cop_has(C_XFER));
	CHECK(cop_has(C_RESUME));
	CHECK_UINT(cop_find(C_RESUME)->slot, 0);
	CHECK(state_is(0, "PENDING"));

	CASE("and no callback was needed to get there");
	rec_cb.reinvite_done(0, 0, true);
	CHECK(state_is(0, "CONNECTED"));
	xfer_refuse = false;

	/*
	 * The single call frame in which the re-entrancy guard does its work.
	 * The short interdigit window completes the target from a timer fsm.c
	 * owns, so the shell has no frame of its own around the op and the
	 * count is zero: an injection made outside the raise re-enters the
	 * machine from inside its own transition, where XFER_COLLECT ignores
	 * the failure and the record strands in PENDING with nobody left to
	 * end it.
	 *
	 * The map is what arms that window. "9T" reaches its timer and "9xx"
	 * extends it, so the verdict is WAIT_SHORT; the "5|9xx" of the other
	 * cases completes on the third digit and never reaches a timer.
	 */
	CASE("a refusal from the short interdigit timer resumes the held leg");
	xfer_cfg(0);
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "9T|9xx");
	cfg.channel[0].interdigit_s = 1;
	fixture_start();
	xfer_held_call(0, 0);
	xfer_refuse = true;
	inject_digits(0, "*072");
	inject_digit(0, '9', false);
	CHECK(state_is(0, "XFER_COLLECT"));
	CHECK(!cop_has(C_XFER));
	uloop_pump(1300);
	CHECK(cop_has(C_XFER));
	CHECK(strcmp(cop_find(C_XFER)->num, "9") == 0);
	CHECK(cop_has(C_RESUME));
	CHECK(state_is(0, "PENDING"));

	CASE("and the machine takes the resume that timer frame carried");
	rec_cb.reinvite_done(0, 0, true);
	CHECK(state_is(0, "CONNECTED"));
	xfer_refuse = false;

	CASE("the consult refers the HELD leg to the ACTIVE one");
	xfer_cfg(2);
	fixture_start();
	xfer_two_calls(0);
	CHECK(state_is(0, "TWO_CALLS"));
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(cop_has(C_XFER_CONSULT));
	CHECK_UINT(cop_find(C_XFER_CONSULT)->slot, 0);
	CHECK_UINT(cop_find(C_XFER_CONSULT)->val, 1);
	CHECK(state_is(0, "IDLE"));

	CASE("an accepted consult clears no leg here");
	CHECK(!cop_has(C_HANGUP));

	/*
	 * call.h: the cell has already entered IDLE, so nobody is left to clear
	 * the two legs a refusal leaves up. This wrapper is that nobody.
	 */
	CASE("a refused consult transfer clears both legs");
	xfer_cfg(2);
	fixture_start();
	xfer_two_calls(0);
	xfer_consult_refuse = true;
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(cop_has(C_XFER_CONSULT));
	CHECK_UINT(cop_count(C_HANGUP), 2);
	CHECK_UINT(cop_slot_count(C_HANGUP, 0), 1);
	CHECK_UINT(cop_slot_count(C_HANGUP, 1), 1);

	CASE("and the line reaches IDLE either way");
	CHECK(state_is(0, "IDLE"));
	xfer_consult_refuse = false;

	/*
	 * The on-hook transfer of a ringing second call, in the RELEASE cell of
	 * CALLING and PROCEEDING. The machine asks for the blind row; this
	 * wrapper reads the state it asks from and hands the call layer the
	 * on-hook kind, whose failure BYEs the referred leg itself (call.h
	 * CALL_XFER_ONHOOK). The ringing leg is cancelled by the cell, and
	 * nothing else is hung up.
	 */
	CASE("the on-hook of a ringing second call refers the held leg on hook");
	xfer_cfg(2);
	fixture_start();
	xfer_held_call(0, 0);
	inject_digit(0, '5', false);
	CHECK(state_is(0, "CALLING"));
	cops_reset();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(cop_has(C_XFER));
	CHECK_UINT(cop_find(C_XFER)->slot, 0);
	CHECK_UINT(cop_find(C_XFER)->val, CALL_XFER_ONHOOK);
	CHECK(strcmp(cop_find(C_XFER)->num, "5") == 0);
	CHECK(!cop_has(C_XFER_CONSULT));
	CHECK_UINT(cop_count(C_HANGUP), 1);
	CHECK_UINT(cop_slot_count(C_HANGUP, 1), 1);
	CHECK(state_is(0, "IDLE"));

	/* the flash and the release drive this transfer and no star code does,
	 * so a config that leaves every code to the switch (star_codes_local 0
	 * loads none) keeps it */
	CASE("the flash on-hook transfer runs with no transfer code configured");
	xfer_cfg(2);
	cfg.channel[0].blind_transfer_exten[0] = 0;
	cfg.channel[0].attended_transfer_exten[0] = 0;
	cfg.channel[0].consult_transfer_exten[0] = 0;
	fixture_start();
	xfer_held_call(0, 0);
	inject_digit(0, '5', false);
	CHECK(state_is(0, "CALLING"));
	cops_reset();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(cop_has(C_XFER));
	CHECK_UINT(cop_find(C_XFER)->val, CALL_XFER_ONHOOK);
	CHECK(strcmp(cop_find(C_XFER)->num, "5") == 0);
	CHECK(state_is(0, "IDLE"));

	CASE("and from PROCEEDING, once the second call rings, the same");
	xfer_cfg(2);
	fixture_start();
	xfer_held_call(0, 0);
	inject_digit(0, '5', false);
	rec_cb.proceeding(0, 1, false);
	CHECK(state_is(0, "PROCEEDING"));
	cops_reset();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK_UINT(cop_find(C_XFER)->val, CALL_XFER_ONHOOK);
	CHECK_UINT(cop_slot_count(C_HANGUP, 1), 1);
	CHECK(state_is(0, "IDLE"));

	/*
	 * call.h: a refusal made no wire exchange and the cell has entered IDLE,
	 * so this wrapper hangs the referred leg up, the consult wrapper's shape,
	 * and the injected failure then finds the IDLE cell, whose hangup of
	 * the same leg is idempotent at the call layer. Two hangups of slot 0
	 * is that pair; one of slot 1 is the cell's own cancel.
	 */
	CASE("a refused on-hook transfer clears the referred leg from this wrapper");
	xfer_cfg(2);
	fixture_start();
	xfer_held_call(0, 0);
	inject_digit(0, '5', false);
	xfer_refuse = true;
	cops_reset();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(cop_has(C_XFER));
	CHECK_UINT(cop_slot_count(C_HANGUP, 1), 1);
	CHECK_UINT(cop_slot_count(C_HANGUP, 0), 2);
	CHECK(state_is(0, "IDLE"));
	xfer_refuse = false;

	/*
	 * The other way into the same op, through XFER_COLLECT: the consult
	 * star code arms the transfer for the next on-hook and dials the target
	 * as an ordinary second call, so it refers nothing at the completion
	 * and reaches the consult entry from a line whose configured pair is
	 * off.
	 */
	CASE("the consult star code arms the transfer instead of referring one");
	xfer_cfg(0);
	fixture_start();
	xfer_held_call(0, 0);
	inject_digits(0, "*074");
	CHECK(state_is(0, "XFER_COLLECT"));
	inject_digits(0, "955");
	CHECK(!cop_has(C_XFER));
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, "955") == 0);
	CHECK(state_is(0, "CALLING"));

	CASE("and the on-hook behind it consults on the armed pair of legs");
	rec_cb.answered(0, 1);
	CHECK(state_is(0, "TWO_CALLS"));
	cops_reset();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(cop_has(C_XFER_CONSULT));
	CHECK_UINT(cop_find(C_XFER_CONSULT)->slot, 0);
	CHECK_UINT(cop_find(C_XFER_CONSULT)->val, 1);

	/* the derive is local_conf_enabled 2 and the drop flash service, and
	 * the two cases below are its isolating halves */
	CASE("the split flash service consults nobody");
	xfer_cfg(2);
	fake_region.flash_service = HAL_FLASH_SERVICE_SPLIT;
	fixture_start();
	region_ready();
	xfer_two_calls(0);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(!cop_has(C_XFER_CONSULT));
	CHECK(state_is(0, "HOLDING"));

	CASE("conference mode 1 consults nobody either");
	xfer_cfg(1);
	fixture_start();
	xfer_two_calls(0);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(!cop_has(C_XFER_CONSULT));
	CHECK(state_is(0, "HOLDING"));

	/*
	 * The pair a shipped config carries. Mode 0 closes the flash-hold path
	 * as well, so this line is driven to TWO_CALLS through the call waiting
	 * answer instead; that route also faces the two legs the other way
	 * round, so the case pins the pair and the two above are what isolate
	 * the derive.
	 */
	CASE("and a line with neither option consults nobody");
	xfer_cfg(0);
	fake_region.flash_service = HAL_FLASH_SERVICE_OFF;
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	inject_hook(0, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(0, 0, true);
	CHECK(state_is(0, "TWO_CALLS"));
	cops_reset();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(!cop_has(C_XFER_CONSULT));
	CHECK(state_is(0, "HOLDING"));

	CASE("a reload lands the on-hook pair at the rest state");
	xfer_cfg(1);
	fixture_start();
	cfg.channel[0].local_conf_enabled = 2;
	lines_reload(&cfg);
	xfer_two_calls(0);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(cop_has(C_XFER_CONSULT));

	CASE("and it cannot move the pair under a call already running");
	xfer_cfg(2);
	fixture_start();
	xfer_two_calls(0);
	cfg.channel[0].local_conf_enabled = 0;
	lines_reload(&cfg);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(cop_has(C_XFER_CONSULT));

	CASE("while the staged pair governs the call after it");
	xfer_two_calls(0);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(!cop_has(C_XFER_CONSULT));
	CHECK(state_is(0, "HOLDING"));
}

/*
 * The network call waiting alert. The switch holds the waiting call, so
 * the shell has no waiting leg to build a CID record from: it stores what the
 * INFO carried and renders that, and it drops the record when the alert ends,
 * when the line reaches IDLE and when a reload lands.
 */
static void cw_net_line_connect(void)
{
	region_ready();
	inject_incoming(0, 0, "5551000", "Alice", 0);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	hops_reset();
	cops_reset();
}

static void t_cw_net(void)
{
	struct hop *cid;

	CASE("a network alert plays the call waiting tone on a connected line");
	cfg_default();
	fixture_start();
	cw_net_line_connect();
	CHECK(state_is(0, "CONNECTED"));
	rec_cb.cw_net(0, 0, true, 2, "Bob Jones", "5552000");
	CHECK(state_is(0, "CONNECTED"));
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_CALL_WAITING);

	CASE("the network alert holds its record behind the SAS as well");
	CHECK(!hop_has(H_CID));
	uloop_pump(CW_CID_EARLY_MS);
	CHECK(!hop_has(H_CID));

	/* the answered leg carries a different caller, so a slot lookup would
	 * announce the wrong one */
	CASE("the alert carries the type 2 caller id the INFO named");
	uloop_pump(CW_CID_LATE_MS);
	CHECK(hop_has(H_CID));
	cid = hop_find(H_CID);
	CHECK_UINT(cid->val, HAL_CID_TYPE2);
	CHECK(strcmp(cid->cid.number, "5552000") == 0);
	CHECK(strcmp(cid->cid.name, "Bob Jones") == 0);

	CASE("the tone reached the line before that record too");
	CHECK(hop_index(H_TONE) < hop_index(H_CID));
	CHECK_UINT(hop_count(H_CID), 1);

	CASE("the alert raises no leg command: there is no waiting call here");
	CHECK(!cop_has(C_RING));
	CHECK(!cop_has(C_REJECT));

	CASE("the stop silences the line");
	hops_reset();
	rec_cb.cw_net(0, 0, false, 0, "", "");
	CHECK(state_is(0, "CONNECTED"));
	CHECK(hop_count(H_TONE_STOP) >= 1);
	CHECK(!hop_has(H_CID));

	CASE("a stop inside the SAS cancels the record the alert armed");
	hops_reset();
	rec_cb.cw_net(0, 0, true, 2, "Bob Jones", "5552000");
	rec_cb.cw_net(0, 0, false, 0, "", "");
	cw_cid_pump();
	CHECK(!hop_has(H_CID));

	CASE("an on hook inside the SAS cancels it as well");
	hops_reset();
	rec_cb.cw_net(0, 0, true, 2, "Bob Jones", "5552000");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	cw_cid_pump();
	CHECK(!hop_has(H_CID));

	CASE("the stop dropped the stored caller: the next alert announces nobody");
	cfg_default();
	fixture_start();
	cw_net_line_connect();
	rec_cb.cw_net(0, 0, true, 2, "Bob Jones", "5552000");
	rec_cb.cw_net(0, 0, false, 0, "", "");
	hops_reset();
	rec_cb.cw_net(0, 0, true, 1, "", "");
	cw_cid_pump();
	CHECK(hop_has(H_CID));
	cid = hop_find(H_CID);
	CHECK(cid->cid.number[0] == 0);
	CHECK(cid->cid.name[0] == 0);

	CASE("on hook ends the alert and drops the stored caller");
	hops_reset();
	rec_cb.cw_net(0, 0, true, 2, "Bob Jones", "5552000");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK(hop_count(H_TONE_STOP) >= 1);
	cw_net_line_connect();
	rec_cb.cw_net(0, 0, true, 2, "", "");
	cw_cid_pump();
	CHECK(hop_has(H_CID));
	cid = hop_find(H_CID);
	CHECK(cid->cid.number[0] == 0);
	CHECK(cid->cid.name[0] == 0);

	CASE("a reload drops the stored caller too");
	cfg_default();
	fixture_start();
	cw_net_line_connect();
	rec_cb.cw_net(0, 0, true, 2, "Bob Jones", "5552000");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	lines_reload(&cfg);
	CHECK(state_is(0, "IDLE"));
	cw_net_line_connect();
	hops_reset();
	rec_cb.cw_net(0, 0, true, 2, "", "");
	cw_cid_pump();
	CHECK(hop_has(H_CID));
	cid = hop_find(H_CID);
	CHECK(cid->cid.number[0] == 0);

	CASE("call waiting off in the config drops the play");
	cfg_default();
	cfg.channel[0].call_waiting_enabled = false;
	fixture_start();
	cw_net_line_connect();
	rec_cb.cw_net(0, 0, true, 2, "Bob Jones", "5552000");
	CHECK(state_is(0, "CONNECTED"));
	cw_cid_pump();
	CHECK(!hop_has(H_CID));
	CHECK(!hop_has(H_TONE));

	/*
	 * The runtime override the subscriber dialled gates
	 * the network alert exactly as it gates the local offer, on the call
	 * they dialled it for and without an on hook in between.
	 */
	CASE("the star code that disables call waiting drops the play too");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	cfg.channel[0].call_waiting_enabled = true;
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "#43#");
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	CHECK(state_is(0, "CONNECTED"));
	hops_reset();
	rec_cb.cw_net(0, 0, true, 2, "Bob Jones", "5552000");
	CHECK(state_is(0, "CONNECTED"));
	cw_cid_pump();
	CHECK(!hop_has(H_CID));

	CASE("the same line takes the alert again once the code re-enables it");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "*43#");
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	CHECK(state_is(0, "CONNECTED"));
	hops_reset();
	rec_cb.cw_net(0, 0, true, 2, "Bob Jones", "5552000");
	cw_cid_pump();
	CHECK(hop_has(H_CID));
	CHECK(strcmp(hop_find(H_CID)->cid.number, "5552000") == 0);

	CASE("a network alert on an idle line changes nothing");
	cfg_default();
	fixture_start();
	hops_reset();
	rec_cb.cw_net(0, 0, true, 2, "Bob Jones", "5552000");
	CHECK(state_is(0, "IDLE"));
	cw_cid_pump();
	CHECK(!hop_has(H_CID));
	CHECK(!hop_has(H_TONE));

	CASE("a local offer takes the alert over from a network one");
	cfg_default();
	fixture_start();
	cw_net_line_connect();
	rec_cb.cw_net(0, 0, true, 2, "Bob Jones", "5552000");
	hops_reset();
	cops_reset();
	inject_incoming(0, 1, "5553000", "Carol", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	CHECK(cop_has(C_RING));
	cw_cid_pump();
	CHECK(hop_has(H_CID));
	CHECK(strcmp(hop_find(H_CID)->cid.number, "5553000") == 0);

	/* the takeover replaces the pending record, it does not add a second one:
	 * the caller the switch named never reaches the display */
	CASE("and the network record it replaced never goes out");
	CHECK_UINT(hop_count(H_CID), 1);

	/*
	 * The one timer per line is re-armed, not left running. The takeover
	 * lands 200 ms into the first alert, so the two deadlines are 350 ms and
	 * 550 ms from the first alert and 180 ms of pump separates them. A
	 * takeover that inherited the first deadline would push at 350, which is
	 * inside the window this case checks is silent, and would push the
	 * record of the alert that no longer owns the line.
	 */
	CASE("a takeover partway into the window moves the deadline with it");
	cfg_default();
	fixture_start();
	cw_net_line_connect();
	rec_cb.cw_net(0, 0, true, 2, "Bob Jones", "5552000");
	uloop_pump(200);
	CHECK(!hop_has(H_CID));
	hops_reset();
	inject_incoming(0, 1, "5553000", "Carol", 0);
	CHECK(state_is(0, "CW_OFFERED"));

	CASE("so nothing goes out at the deadline the first alert set");
	uloop_pump(180);
	CHECK(!hop_has(H_CID));

	CASE("and the second alert pushes its own record at the new one");
	uloop_pump(250);
	CHECK(hop_has(H_CID));
	CHECK_UINT(hop_count(H_CID), 1);
	CHECK(strcmp(hop_find(H_CID)->cid.number, "5553000") == 0);

	CASE("the local offer owns the alert, so a network stop leaves it alone");
	cfg_default();
	fixture_start();
	cw_net_line_connect();
	rec_cb.cw_net(0, 0, true, 2, "Bob Jones", "5552000");
	inject_incoming(0, 1, "5553000", "Carol", 0);
	hops_reset();
	rec_cb.cw_net(0, 0, false, 0, "", "");
	CHECK(state_is(0, "CW_OFFERED"));
	CHECK(!hop_has(H_TONE_STOP));

	CASE("so the record of that offer still goes out behind its SAS");
	cw_cid_pump();
	CHECK(hop_has(H_CID));
	CHECK(strcmp(hop_find(H_CID)->cid.number, "5553000") == 0);

	CASE("an unknown line is ignored");
	rec_cb.cw_net(CFG_CHANNEL_MAX, 0, true, 2, "Bob Jones", "5552000");
	CHECK(state_is(0, "CW_OFFERED"));

	CASE("a network INFO digit selects a distinctive variant and its lead-in");
	cfg_default();
	cw_variant_region_seed();
	fixture_start();
	cw_net_line_connect();
	CHECK(state_is(0, "CONNECTED"));
	rec_cb.cw_net(0, 0, true, 3, "Bob", "5552000");
	CHECK(hop_has(H_CW_TONE));
	CHECK_UINT(hop_last(H_CW_TONE)->val, 3);
	CHECK(!hop_has(H_TONE));
	/* variant 3 lead-in 500 ms plus the 50 ms gap is 550 ms, not 350. */
	uloop_pump(CW_CID_EARLY_MS + CW_CID_LATE_MS);
	CHECK(!hop_has(H_CID));
	uloop_pump(200);
	CHECK(hop_has(H_CID));
	CHECK_UINT(hop_find(H_CID)->val, HAL_CID_TYPE2);
}

/*
 * Which path presents the offer.
 * call_waiting_local 1 keeps the local offer of CONNECTED and drops the
 * network alert; an unset option keeps both, which is the compatibility
 * state. The mode rides the line's channel copy, so a reload cannot move it
 * under an alert already playing.
 */
static void cw_local_set(bool local)
{
	cfg.channel[0].call_waiting_local_set = true;
	cfg.channel[0].call_waiting_local = local;
}

static void t_cw_mode(void)
{
	struct line_view v;

	CASE("an unset call_waiting_local presents the network alert");
	cfg_default();
	fixture_start();
	cw_net_line_connect();
	rec_cb.cw_net(0, 0, true, 2, "Bob Jones", "5552000");
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_CALL_WAITING);

	CASE("and it presents the local offer as well");
	cfg_default();
	fixture_start();
	cw_offer_line_connect();
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CW_OFFERED"));

	/* the machine never sees the event, so the alert tone is not requested
	 * and the record behind it is never armed */
	CASE("call_waiting_local 1 drops the network alert at the dispatch");
	cfg_default();
	cw_local_set(true);
	fixture_start();
	cw_net_line_connect();
	rec_cb.cw_net(0, 0, true, 2, "Bob Jones", "5552000");
	CHECK(state_is(0, "CONNECTED"));
	CHECK(!hop_has(H_TONE));
	cw_cid_pump();
	CHECK(!hop_has(H_CID));

	/*
	 * A stop is not a play. Its cell is unconditional by design, so it has
	 * to reach the machine whatever the mode says: the mode may have moved
	 * under an alert that is still running, and a dropped stop would leave
	 * that alert playing with nothing left to end it.
	 */
	CASE("but a network stop still reaches the machine in that mode");
	hops_reset();
	rec_cb.cw_net(0, 0, false, 0, "", "");
	CHECK(state_is(0, "CONNECTED"));
	CHECK(hop_has(H_TONE_STOP));

	CASE("the local offer still runs under call_waiting_local 1");
	cfg_default();
	cw_local_set(true);
	fixture_start();
	cw_offer_line_connect();
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_CALL_WAITING);
	cw_cid_pump();
	CHECK(hop_has(H_CID));
	CHECK(strcmp(hop_find(H_CID)->cid.number, "5552000") == 0);

	CASE("call_waiting_local 0 presents the network alert");
	cfg_default();
	cw_local_set(false);
	fixture_start();
	cw_net_line_connect();
	rec_cb.cw_net(0, 0, true, 2, "Bob Jones", "5552000");
	CHECK(state_is(0, "CONNECTED"));
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_CALL_WAITING);
	cw_cid_pump();
	CHECK(hop_has(H_CID));
	CHECK(strcmp(hop_find(H_CID)->cid.number, "5552000") == 0);

	/*
	 * The other half of mode 0: the switch serves the waiting call, so a
	 * second leg that reaches this line is refused the way a line with the
	 * service off refuses it. The refusal comes from the machine, which is
	 * where the 486 policy lives.
	 */
	CASE("call_waiting_local 0 refuses a local second leg with 486");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	cw_local_set(false);
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	CHECK(state_is(0, "CONNECTED"));
	cops_reset();
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CONNECTED"));
	CHECK(cop_has(C_REJECT));
	CHECK_UINT(cop_find(C_REJECT)->val, 486);

	/* the service is enabled and merely network served, so the query code
	 * must not answer as if the subscriber had switched it off */
	CASE("and the query code still confirms in that mode");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	hops_reset();
	inject_digits(0, "*#43#");
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK_UINT(hop_find(H_TONE)->val, HAL_TONE_CONFIRM);

	CASE("and the status dump still reads call waiting enabled");
	view_get(0, &v);
	CHECK(v.call_waiting);

	/*
	 * The mode is staged. A reload that lands while an alert is running
	 * would otherwise change the alert source under it, so the new mode
	 * waits for the same rest state the rest of the channel copy waits for.
	 */
	CASE("a reload cannot move the mode under a running alert");
	cfg_default();
	fixture_start();
	cw_net_line_connect();
	rec_cb.cw_net(0, 0, true, 2, "Bob Jones", "5552000");
	CHECK(hop_has(H_TONE));
	cw_local_set(true);
	lines_reload(&cfg);
	CHECK(state_is(0, "CONNECTED"));
	hops_reset();
	rec_cb.cw_net(0, 0, true, 2, "Bob Jones", "5552000");
	CHECK(hop_has(H_TONE));

	CASE("and it takes effect once the line rests");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	cw_net_line_connect();
	hops_reset();
	rec_cb.cw_net(0, 0, true, 2, "Bob Jones", "5552000");
	CHECK(state_is(0, "CONNECTED"));
	CHECK(!hop_has(H_TONE));
}

/*
 * The region CID format reaches the ring. The format decides the modulation,
 * and where the record sits relative to the bell. The FSK families use the
 * silence after the first burst. Every alerting family runs its whole sequence
 * before the bell.
 */
static void t_region_readback(void)
{
	struct region_view rv;
	struct hop *r;
	unsigned int calls;

	CASE("the read-back formats, windows and templates win over the config");
	cfg_default();
	fixture_start();
	region_readback_seed();
	job_complete(HAL_OK);
	CHECK_UINT(region_get_calls, 1);
	CHECK_UINT(hook_ts[0].flash_min_ms, 35);
	CHECK_UINT(hook_ts[0].flash_max_ms, 145);
	CHECK_UINT(hook_ts[0].release_min_ms, 590);
	hops_reset();
	inject_incoming(0, 0, "5551000", "", 3);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cid.format, HAL_CID_ETSI_FSK);
	CHECK_UINT(r->ring.cadence_count, 2);
	CHECK_UINT(r->ring.cadence[0].on_ms, 111);
	CHECK_UINT(r->ring.cadence[1].off_ms, 444);

	CASE("the read-back mwi format reaches the VMWI record");
	cfg_default();
	fixture_start();
	region_readback_seed();
	job_complete(HAL_OK);
	hops_reset();
	mwi_notify(0, true, 1, 1);
	CHECK(hop_has(H_CID));
	CHECK_UINT(hop_last(H_CID)->cid.format, HAL_CID_ETSI_DTAS);
	/* the store survives a re-init by design, so the message is cleared
	 * here rather than leaked into the mwi cases behind this one */
	mwi_notify(0, false, 0, 0);

	CASE("the read-back flash service reaches the flash-derived view");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_readback_seed();
	fake_region.flash_service = HAL_FLASH_SERVICE_OFF;
	job_complete(HAL_OK);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	cops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(!cop_has(C_HOLD));
	CHECK(state_is(0, "CONNECTED"));

	CASE("the status dump names the read-back and its source");
	cfg_default();
	fixture_start();
	region_readback_seed();
	job_complete(HAL_OK);
	CHECK(region_view_get(&rv));
	CHECK(strcmp(rv.requested, "USA") == 0);
	CHECK(strcmp(rv.applied, "USA") == 0);
	CHECK(strcmp(rv.source, "backend") == 0);
	CHECK(strcmp(rv.iso3, "GBR") == 0);
	CHECK(strcmp(rv.describe, "Read-back GBR") == 0);
	CHECK(strcmp(rv.cid_format, "etsi-fsk") == 0);
	CHECK(strcmp(rv.mwi_format, "etsi-dtas") == 0);
	CHECK_UINT(rv.flash_min_ms, 35);
	CHECK_UINT(rv.flash_max_ms, 145);
	CHECK_UINT(rv.release_min_ms, 590);
	CHECK_UINT(rv.cw_total_ms, 70000);
	CHECK(strcmp(rv.flash_service, "drop") == 0);
	CHECK(rv.pulse_invert_set);
	CHECK(!rv.pulse_invert);
	CHECK_UINT(rv.templates, 2);
	CHECK_UINT(rv.last_template, 3);
	CHECK_UINT(rv.last_pairs, 2);

	CASE("a refused read-back leaves the compiled default standing");
	cfg_default();
	region_get_status = HAL_UNSUPPORTED;
	fixture_start();
	calls = hook_ts_calls[0];
	job_complete(HAL_OK);
	CHECK_UINT(region_get_calls, 1);
	CHECK_UINT(hook_ts_calls[0], calls);
	CHECK_UINT(hook_ts[0].flash_min_ms, 300);
	hops_reset();
	inject_incoming(0, 0, "5551000", "", 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cid.format, HAL_CID_BELLCORE_FSK);
	CHECK(region_view_get(&rv));
	CHECK(strcmp(rv.requested, "USA") == 0);
	CHECK(strcmp(rv.applied, "USA") == 0);
	CHECK(strcmp(rv.source, "default") == 0);
	CHECK(strcmp(rv.iso3, "") == 0);
	CHECK(strcmp(rv.describe, "") == 0);
	CHECK(strcmp(rv.cid_format, "bellcore-fsk") == 0);
	CHECK(strcmp(rv.mwi_format, "bellcore-fsk") == 0);
	CHECK_UINT(rv.flash_min_ms, 300);
	CHECK_UINT(rv.flash_max_ms, 1300);
	CHECK_UINT(rv.release_min_ms, 1301);
	CHECK_UINT(rv.cw_total_ms, 0);
	CHECK(strcmp(rv.flash_service, "drop") == 0);
	CHECK(rv.pulse_invert_set);
	CHECK(!rv.pulse_invert);
	CHECK(rv.confirm_total_set);
	CHECK_UINT(rv.confirm_total_ms, 0);
	CHECK(rv.confirm_endless_set);
	CHECK(!rv.confirm_endless);
	CHECK_UINT(rv.templates, 0);

	CASE("a failed load keeps a view the getter still confirms");
	cfg_default();
	fixture_start();
	region_readback_seed();
	job_complete(HAL_OK);
	snprintf(cfg.region, sizeof(cfg.region), "GB");
	lines_reload(&cfg);
	job_complete(HAL_FATAL);
	CHECK(region_get_calls >= 2);
	CHECK_UINT(hook_ts[0].flash_min_ms, 35);

	CASE("a failed load with a refusing getter falls back to the compiled default");
	cfg_default();
	fixture_start();
	region_readback_seed();
	job_complete(HAL_OK);
	CHECK_UINT(hook_ts[0].flash_min_ms, 35);
	region_get_status = HAL_UNSUPPORTED;
	snprintf(cfg.region, sizeof(cfg.region), "GB");
	lines_reload(&cfg);
	job_complete(HAL_FATAL);
	CHECK_UINT(hook_ts[0].flash_min_ms, 300);
	hops_reset();
	inject_incoming(0, 0, "5551000", "", 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cid.format, HAL_CID_BELLCORE_FSK);

	/*
	 * A region job that completes with a reload staged under a live call:
	 * the active view keeps the service it started with, and the flash
	 * fields land from the staged channel copy at the rest state. The
	 * on-hook consult needs local_conf_enabled 2 (only the staged copy
	 * carries it) and the drop service (only the new read-back carries it),
	 * so the consult proves both halves came from the right places.
	 */
	CASE("a region job under a staged reload derives the flash view from the staged copy");
	xfer_cfg(1);
	fake_region.flash_service = HAL_FLASH_SERVICE_SPLIT;
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "5551000", "Alice", 0);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(state_is(0, "CONNECTED"));
	cfg.channel[0].local_conf_enabled = 2;
	fake_region.flash_service = HAL_FLASH_SERVICE_DROP;
	region_change("GB");
	cops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(cop_has(C_HOLD));
	rec_cb.reinvite_done(0, 0, true);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(!cop_has(C_XFER_CONSULT));

	CASE("and the staged pair lands once the line rests");
	rec_cb.remote_bye(0, 0);
	CHECK(state_is(0, "IDLE"));
	xfer_two_calls(0);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(cop_has(C_XFER_CONSULT));
}

static void t_hook_push_idempotent(void)
{
	unsigned int calls;

	CASE("an unchanged reload pushes no hook window");
	cfg_default();
	fixture_start();
	region_ready();
	calls = hook_ts_calls[0];
	lines_reload(&cfg);
	CHECK_UINT(hook_ts_calls[0], calls);

	CASE("a failed push stays owed and the next apply retries it");
	cfg_default();
	fixture_start();
	region_ready();
	fake_region.hook_flash_min_ms = 200;
	hook_ts_status = HAL_FATAL;
	calls = hook_ts_calls[0];
	region_change("GB");
	CHECK_UINT(hook_ts_calls[0], calls + 1);
	hook_ts_status = HAL_OK;
	lines_reload(&cfg);
	CHECK_UINT(hook_ts_calls[0], calls + 2);
	CHECK_UINT(hook_ts[0].flash_min_ms, 200);

	CASE("the accepted window is recorded and a repeat costs no op");
	lines_reload(&cfg);
	CHECK_UINT(hook_ts_calls[0], calls + 2);
}

static void t_cid_format(void)
{
	struct hop *r;

	CASE("the Bellcore format rings its FSK after the first burst");
	cfg_default();
	fake_region.cid_format = HAL_CID_BELLCORE_FSK;
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "5551000", "Alice", 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cid.format, HAL_CID_BELLCORE_FSK);
	CHECK_UINT(r->ring.cid_at_ring, 1);
	CHECK_UINT(r->ring.cid_wait_ms, 500);

	CASE("etsi-lr-dtas rings the line reversal DTAS sequence BEFORE the first burst");
	cfg_default();
	fake_region.cid_format = HAL_CID_ETSI_LR_DTAS;
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "5551000", "Alice", 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cid.format, HAL_CID_ETSI_LR_DTAS);
	CHECK_UINT(r->ring.cid_at_ring, 0);
	CHECK_UINT(r->ring.cid_wait_ms, 0);

	CASE("the cadence and the record itself are untouched by the format");
	CHECK_UINT(r->ring.cadence_count, 1);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);
	CHECK(strcmp(r->ring.cid.number, "5551000") == 0);

	CASE("etsi-dtas is the DTAS sequence, also before the first burst");
	cfg_default();
	fake_region.cid_format = HAL_CID_ETSI_DTAS;
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "5551000", "Alice", 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cid.format, HAL_CID_ETSI_DTAS);
	CHECK_UINT(r->ring.cid_at_ring, 0);

	CASE("dtmf-dtas is the DTMF class, before the first burst too");
	cfg_default();
	fake_region.cid_format = HAL_CID_DTMF_DTAS;
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "5551000", "Alice", 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cid.format, HAL_CID_DTMF_DTAS);
	CHECK_UINT(r->ring.cid_at_ring, 0);

	CASE("a region load moves the format with the view it belongs to");
	cfg_default();
	fake_region.cid_format = HAL_CID_BELLCORE_FSK;
	fixture_start();
	region_ready();
	fake_region.cid_format = HAL_CID_ETSI_LR_DTAS;
	region_change("GB");
	hops_reset();
	inject_incoming(0, 0, "5551000", "Alice", 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cid.format, HAL_CID_ETSI_LR_DTAS);
}

/*
 * One uci bool decides whether this line announces callers at all, on the
 * ring and on both call waiting paths. The type 2 push exists only for the FSK
 * families.
 */
static void t_cid_gate(void)
{
	struct hop *r;

	CASE("cid_remote_enable off rings with no record at all");
	cfg_default();
	cfg.channel[0].cid_remote_enable = false;
	fixture_start();
	inject_incoming(0, 0, "5551000", "Alice", 0);
	r = hop_find(H_RING_START);
	CHECK(hop_has(H_RING_START));
	CHECK(!r->ring.cid_present);

	CASE("and the bell still rings its normal cadence");
	CHECK_UINT(r->ring.cadence_count, 1);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);

	CASE("cid_remote_enable off drops the type 2 record of a local offer");
	cfg_default();
	cfg.channel[0].cid_remote_enable = false;
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	hops_reset();
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	CHECK(!hop_has(H_CID));

	/* the gate refuses the record before the shell arms anything, so there is
	 * no push waiting behind the SAS either */
	CASE("and arms no deferred push behind the alert");
	cw_cid_pump();
	CHECK(!hop_has(H_CID));

	CASE("the alert tone itself is not gated by it");
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_CALL_WAITING);

	CASE("cid_remote_enable off drops the network alert record too");
	cfg_default();
	cfg.channel[0].cid_remote_enable = false;
	fixture_start();
	cw_net_line_connect();
	rec_cb.cw_net(0, 0, true, 2, "Bob Jones", "5552000");
	CHECK(state_is(0, "CONNECTED"));
	cw_cid_pump();
	CHECK(!hop_has(H_CID));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_CALL_WAITING);

	CASE("the DTMF class has no type 2 form, so the alert carries no record");
	cfg_default();
	fake_region.cid_format = HAL_CID_DTMF_DTAS;
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	hops_reset();
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	cw_cid_pump();
	CHECK(!hop_has(H_CID));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_CALL_WAITING);

	CASE("the type 1 ring record of that same format is unaffected");
	cfg_default();
	fake_region.cid_format = HAL_CID_DTMF_DTAS;
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "5551000", "Alice", 0);
	r = hop_find(H_RING_START);
	CHECK(r->ring.cid_present);
	CHECK_UINT(r->ring.cid.format, HAL_CID_DTMF_DTAS);
	CHECK(strcmp(r->ring.cid.number, "5551000") == 0);

	CASE("an alerting FSK format keeps its type 2, in the family it modulates");
	cfg_default();
	fake_region.cid_format = HAL_CID_ETSI_LR_DTAS;
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	hops_reset();
	inject_incoming(0, 1, "5552000", "Bob", 0);
	cw_cid_pump();
	CHECK(hop_has(H_CID));
	CHECK_UINT(hop_find(H_CID)->val, HAL_CID_TYPE2);
	CHECK_UINT(hop_find(H_CID)->cid.format, HAL_CID_ETSI_FSK);

	CASE("a withheld caller reaches the type 2 record as withheld");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	hops_reset();
	rec_cb.incoming(0, 1, "5552000", "Bob", true, true, 0);
	cw_cid_pump();
	CHECK(hop_has(H_CID));
	CHECK(hop_find(H_CID)->cid.number_private);
	CHECK(hop_find(H_CID)->cid.name_private);
}

/*
 * cid_delivery_local 0 hands the caller-id service to the network. The four
 * codes must dial through as ordinary digits, and the CPE must synthesize no
 * privacy of its own.
 */
static void t_cid_delivery(void)
{
	struct line_view v;

	CASE("with the service local the one-call code is consumed locally");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	cfg.channel[0].cid_delivery_local_set = true;
	cfg.channel[0].cid_delivery_local = true;
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	cops_reset();
	inject_digits(0, "#31#");
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK(!cop_has(C_DIAL));
	view_get(0, &v);
	CHECK(v.cid_once_set);

	/* the shipped map takes '#' as the escape, so the code reaches the dial
	 * plan as the digits in front of it, exactly like any other number */
	CASE("with the service on the network the same code dials through");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	cfg.channel[0].cid_delivery_local_set = true;
	cfg.channel[0].cid_delivery_local = false;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	cops_reset();
	inject_digits(0, "*31#");
	CHECK(state_is(0, "CALLING"));
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, "*31") == 0);

	CASE("nothing local was toggled by it");
	view_get(0, &v);
	CHECK(!v.cid_once_set);

	CASE("and the presentation of that call followed cid_outgoing_enable");
	CHECK(cop_find(C_DIAL)->cid_present);

	CASE("a config that withholds still withholds under the network service");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	cfg.channel[0].cid_delivery_local_set = true;
	cfg.channel[0].cid_delivery_local = false;
	cfg.channel[0].cid_outgoing_enable = false;
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	cops_reset();
	inject_digit(0, '5', false);
	CHECK(cop_has(C_DIAL));
	CHECK(!cop_find(C_DIAL)->cid_present);

	CASE("the persistent codes dial through under the network service too");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	snprintf(cfg.channel[0].cid_enable_exten,
		 sizeof(cfg.channel[0].cid_enable_exten), "*99#");
	snprintf(cfg.channel[0].cid_disable_exten,
		 sizeof(cfg.channel[0].cid_disable_exten), "#99#");
	cfg.channel[0].cid_delivery_local_set = true;
	cfg.channel[0].cid_delivery_local = false;
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	cops_reset();
	inject_digits(0, "*99#");
	CHECK(state_is(0, "CALLING"));
	CHECK(cop_has(C_DIAL));
	CHECK(strcmp(cop_find(C_DIAL)->num, "*99") == 0);

	CASE("an unset cid_delivery_local keeps the local service");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	CHECK(!cfg.channel[0].cid_delivery_local_set);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	cops_reset();
	inject_digits(0, "#31#");
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK(!cop_has(C_DIAL));
	view_get(0, &v);
	CHECK(v.cid_once_set);

	CASE("the call waiting codes are untouched by the CID service mode");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	cfg.channel[0].cid_delivery_local_set = true;
	cfg.channel[0].cid_delivery_local = false;
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "#43#");
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	view_get(0, &v);
	CHECK(!v.call_waiting);
}

/*
 * The presentation the dial carries, and the order the three sources
 * are consulted in. The recorded call_dial argument is the whole observable.
 */
static void t_cid_dial(void)
{
	struct line_view v;

	CASE("with nothing dialled the config decides");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	cops_reset();
	inject_digit(0, '5', false);
	CHECK(cop_has(C_DIAL));
	CHECK(cop_find(C_DIAL)->cid_present);

	CASE("a config that withholds reaches the leg as a withheld call");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	cfg.channel[0].cid_outgoing_enable = false;
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	cops_reset();
	inject_digit(0, '5', false);
	CHECK(!cop_find(C_DIAL)->cid_present);

	CASE("the one-call disable code withholds the call behind it");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "#31#");
	cops_reset();
	inject_digit(0, '5', false);
	CHECK(cop_has(C_DIAL));
	CHECK(!cop_find(C_DIAL)->cid_present);

	/* one call, and one only: the override is consumed at the dial */
	CASE("the call after it presents the number again");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	cops_reset();
	inject_digit(0, '5', false);
	CHECK(cop_has(C_DIAL));
	CHECK(cop_find(C_DIAL)->cid_present);

	/*
	 * The override dies at the dial, not on the way back to IDLE. A
	 * subscriber who flashes and dials a second call inside the same
	 * off-hook gesture never passes IDLE, so only a consuming take presents
	 * that second number: a peek would withhold both.
	 */
	CASE("a second call in one off-hook gesture presents the number again");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "#31#");
	cops_reset();
	inject_digit(0, '5', false);
	CHECK(cop_has(C_DIAL));
	CHECK(!cop_find(C_DIAL)->cid_present);
	rec_cb.answered(0, 0);
	inject_hook(0, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(0, 0, true);
	CHECK(state_is(0, "HOLD_DIAL"));
	cops_reset();
	inject_digit(0, '5', false);
	CHECK(cop_has(C_DIAL));
	CHECK(cop_find(C_DIAL)->cid_present);

	CASE("the one-call enable code presents over a config that withholds");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	cfg.channel[0].cid_outgoing_enable = false;
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "*31#");
	cops_reset();
	inject_digit(0, '5', false);
	CHECK(cop_find(C_DIAL)->cid_present);

	CASE("the persistent enable code withholds every call after it");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	snprintf(cfg.channel[0].cid_enable_exten,
		 sizeof(cfg.channel[0].cid_enable_exten), "*99#");
	snprintf(cfg.channel[0].cid_disable_exten,
		 sizeof(cfg.channel[0].cid_disable_exten), "#99#");
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "*99#");
	cops_reset();
	inject_digit(0, '5', false);
	CHECK(!cop_find(C_DIAL)->cid_present);

	CASE("and the call after that one, because it is persistent");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	cops_reset();
	inject_digit(0, '5', false);
	CHECK(!cop_find(C_DIAL)->cid_present);

	CASE("a one-call enable beats the standing restriction, for that call");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "*31#");
	cops_reset();
	inject_digit(0, '5', false);
	CHECK(cop_find(C_DIAL)->cid_present);

	CASE("the restriction is back for the next call");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	cops_reset();
	inject_digit(0, '5', false);
	CHECK(!cop_find(C_DIAL)->cid_present);

	CASE("the persistent disable code lifts it");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "#99#");
	cops_reset();
	inject_digit(0, '5', false);
	CHECK(cop_find(C_DIAL)->cid_present);

	CASE("a reload drops the restriction with every other runtime toggle");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	snprintf(cfg.channel[0].cid_enable_exten,
		 sizeof(cfg.channel[0].cid_enable_exten), "*99#");
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "*99#");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	lines_reload(&cfg);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	cops_reset();
	inject_digit(0, '5', false);
	CHECK(cop_find(C_DIAL)->cid_present);

	/*
	 * The restriction never reaches uci, so the status dump is the only way
	 * an operator can see that a subscriber is withholding. The key carries
	 * the restriction, not the presentation: true means hidden.
	 */
	CASE("the status dump publishes the standing restriction");
	cfg_default();
	cfg_star_codes(&cfg.channel[0]);
	snprintf(cfg.channel[0].cid_enable_exten,
		 sizeof(cfg.channel[0].cid_enable_exten), "*99#");
	snprintf(cfg.channel[0].cid_disable_exten,
		 sizeof(cfg.channel[0].cid_disable_exten), "#99#");
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	view_get(0, &v);
	CHECK(!v.cid_restrict_set);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digits(0, "*99#");
	view_get(0, &v);
	CHECK(v.cid_restrict_set);
	CHECK(v.cid_restrict);
	inject_digits(0, "#99#");
	view_get(0, &v);
	CHECK(v.cid_restrict_set);
	CHECK(!v.cid_restrict);

	CASE("the standing restriction survives the call it was dialled before");
	inject_digits(0, "*99#");
	inject_digit(0, '5', false);
	CHECK(state_is(0, "CALLING"));
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	view_get(0, &v);
	CHECK(v.cid_restrict_set);
	CHECK(v.cid_restrict);

	CASE("and a reload takes it off the dump with the rest of the state");
	lines_reload(&cfg);
	view_get(0, &v);
	CHECK(!v.cid_restrict_set);
}

static void t_status(void)
{
	struct line_view v;

	CASE("the dump carries one entry per channel");
	cfg_default();
	fixture_start();
	view_get(0, &v);
	CHECK_UINT(dump_lines, CFG_CHANNEL_MAX);

	CASE("the entry names the line, the state and the service flag");
	CHECK(v.found);
	CHECK_UINT(v.line, 0);
	CHECK(strcmp(v.state, "IDLE") == 0);
	CHECK(!v.oos);

	CASE("the entry nests the call layer dump");
	CHECK(v.has_calls);

	CASE("the collected digits appear while they are collected");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '1', false);
	inject_digit(0, '2', false);
	view_get(0, &v);
	CHECK(strcmp(v.digits, "12") == 0);

	CASE("the digits disappear again at rest");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	view_get(0, &v);
	CHECK(strcmp(v.digits, "") == 0);

	/*
	 * Four states of the wire read as CLEARING on the status page and the
	 * handset hears four different things, so the class is reported with the
	 * state. It comes from fsm.clearing, which the chain itself set.
	 */
	CASE("a busy final names the busy clearing class");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.failed(0, 0, CALL_FAIL_BUSY);
	view_get(0, &v);
	CHECK(strcmp(v.state, "CLEARING") == 0);
	CHECK(strcmp(v.clearing, "busy") == 0);

	CASE("an unobtainable final names the unobtainable class");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.failed(0, 0, CALL_FAIL_UNOBTAINABLE);
	view_get(0, &v);
	CHECK(strcmp(v.clearing, "unobtainable") == 0);

	CASE("a congestion final names the fastbusy class");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.failed(0, 0, CALL_FAIL_CONGESTION);
	view_get(0, &v);
	CHECK(strcmp(v.clearing, "fastbusy") == 0);

	CASE("a far end BYE on an answered call names the quiet class");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	rec_cb.remote_bye(0, 0);
	view_get(0, &v);
	CHECK(strcmp(v.state, "CLEARING") == 0);
	CHECK(strcmp(v.clearing, "quiet") == 0);

	CASE("a line that is not clearing carries no class at all");
	fixture_start();
	view_get(0, &v);
	CHECK(strcmp(v.state, "IDLE") == 0);
	CHECK(!v.clearing[0]);
}

/*
 * The fax trigger gate. Four gates decide whether a detected tone converts
 * the call, and each one is a separate way to convert a call that must not be
 * converted:
 *
 *   the tone family    a CNG is the calling terminal's tone, so a device that
 *                      hears it has not answered anything;
 *   the direction      a tone from the wire says the far end answered the fax,
 *                      and that gateway owns the switch;
 *   the call           there has to be one answered leg with media running;
 *   the latch          a fax answer emits CED for seconds and the detector
 *                      reports every onset.
 *
 * The switch drives no FSM event, so every case reads the state back to prove
 * the machine did not move.
 */
static void fax_connect(unsigned int line)
{
	inject_hook(line, HAL_HOOK_SEIZE, 0);
	inject_digit(line, '5', false);
	rec_cb.answered(line, 0);
}

static void t_fax(void)
{
	struct call_ended info;

	/* the leg release report, whose only reader here is the fax latch */
	memset(&info, 0, sizeof(info));
	info.answered = true;
	info.media_ran = true;
	info.number = "5";
	info.remote_number = "";
	info.remote_name = "";

	CASE("the line reaches CONNECTED for the fax trigger cases");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();
	fax_connect(0);
	CHECK(state_is(0, "CONNECTED"));

	CASE("a local CED switches the active leg to fax");
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK(cop_has(C_FAX));
	CHECK_UINT(cop_count(C_FAX), 1);
	CHECK_UINT(cop_find(C_FAX)->line, 0);
	CHECK_UINT(cop_find(C_FAX)->slot, 0);

	CASE("and the fax switch does not move the machine");
	CHECK(state_is(0, "CONNECTED"));

	CASE("a second CED on the same leg is latched and dropped");
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 0);

	CASE("and no other tone of the family gets past the latch either");
	inject_tone(0, HAL_TONE_DET_ANS, HAL_TONE_DIR_LOCAL, 0);
	inject_tone(0, HAL_TONE_DET_ANSAM, HAL_TONE_DIR_LOCAL, 0);
	inject_tone(0, HAL_TONE_DET_ANSBAR, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 0);

	/* the leg release is the only exit, and it is what re-arms the
	 * slot for the next call on it */
	CASE("the end of the leg re-arms the switch for the next call");
	rec_cb.ended(0, 0, &info);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	fax_connect(0);
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 1);

	CASE("a CED from the wire arms MODEM without a local upspeed");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();
	fax_connect(0);
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 0);
	CHECK_UINT(cop_count(C_FAX), 0);

	CHECK_UINT(cop_count(C_FAX_KIND), 1);
	CHECK_UINT(cop_find(C_FAX_KIND)->line, 0);
	CHECK_UINT(cop_find(C_FAX_KIND)->slot, 0);
	CHECK_UINT(cop_find(C_FAX_KIND)->val, HAL_PASSTHROUGH_MODEM);

	CASE("and it records the hint before the kind for a later upspeed");
	CHECK_UINT(cop_count(C_FAX_HINT), 1);
	CHECK_UINT(cop_find(C_FAX_HINT)->line, 0);
	CHECK_UINT(cop_find(C_FAX_HINT)->slot, 0);
	CHECK(cop_index(C_FAX_HINT) < cop_index(C_FAX_KIND));

	CASE("and every member of the answer tone family records one");
	cops_reset();
	inject_tone(0, HAL_TONE_DET_ANS, HAL_TONE_DIR_REMOTE, 0);
	inject_tone(0, HAL_TONE_DET_ANSAM, HAL_TONE_DIR_REMOTE, 0);
	inject_tone(0, HAL_TONE_DET_ANSBAR, HAL_TONE_DIR_REMOTE, 0);
	CHECK_UINT(cop_count(C_FAX_HINT), 3);
	CHECK_UINT(cop_count(C_FAX_KIND), 3);
	CHECK_UINT(cop_find(C_FAX_KIND)->val, HAL_PASSTHROUGH_MODEM);
	CHECK_UINT(cop_count(C_FAX), 0);

	/* the calling terminal's own tone says nothing about the far end, and
	 * the preamble belongs to the T.38 stage */
	CASE("a remote CNG or preamble records none");
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CNG, HAL_TONE_DIR_REMOTE, 0);
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_REMOTE, 0);
	CHECK_UINT(cop_count(C_FAX_HINT), 0);

	/* the local tone takes the switch, and a leg that switched needs no
	 * recognizer for an upspeed it is making itself */
	CASE("and the local CED behind it still fires, so nothing latched");
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 1);
	CHECK_UINT(cop_count(C_FAX_HINT), 0);

	CASE("an idle line records no hint at all");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(cop_count(C_FAX_HINT), 0);
	CHECK_UINT(cop_count(C_FAX_KIND), 0);
	CHECK_UINT(cop_count(C_FAX), 0);

	CASE("nor does a call that is not answered yet");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	CHECK(state_is(0, "CALLING"));
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 0);
	CHECK_UINT(cop_count(C_FAX_HINT), 0);
	CHECK_UINT(cop_count(C_FAX_KIND), 0);
	CHECK_UINT(cop_count(C_FAX), 0);

	CASE("and the answer behind it opens the record");
	rec_cb.answered(0, 0);
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 0);
	CHECK_UINT(cop_count(C_FAX_HINT), 1);

	/*
	 * The tone traces name the tone. A dropped fax is read from these rows
	 * and nowhere else, and the number the record carries costs the reader
	 * a lookup into hal.h before the row says anything at all. The four
	 * below are every row of this file that carries a code.
	 */
	CASE("every tone trace of the line names the tone it dropped or took");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();
	fax_connect(0);
	cops_reset();
	trace_domain_set(TRACE_HOOK, true);
	trace_domain_set(TRACE_FSM, true);
	warn_capture_begin();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 0);
	inject_tone(0, HAL_TONE_DET_CNG, HAL_TONE_DIR_LOCAL, 0);
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_REMOTE, 0);
	inject_tone(0, HAL_TONE_DET_ANSBAR, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(warn_capture_count("line 0 fax tone CED from the wire"), 1);
	CHECK_UINT(warn_capture_count("line 0 tone CNG starts no fax"), 1);
	CHECK_UINT(warn_capture_count("line 0 tone V21_PREAMBLE came from the "
				      "wire"), 1);
	CHECK_UINT(warn_capture_count("line 0 slot 0 fax tone ANSBAR, switching "
				      "to passthrough"), 1);
	/* and the code itself appears in none of them: a name and a number in
	 * one log cannot be read together */
	CHECK_UINT(warn_capture_count("fax tone 2 from the wire"), 0);
	CHECK_UINT(warn_capture_count("tone 1 starts no fax"), 0);
	CHECK_UINT(warn_capture_count("fax tone 5, switching"), 0);
	warn_capture_end();
	trace_domain_set(TRACE_FSM, false);
	trace_domain_set(TRACE_HOOK, false);
	CHECK_UINT(cop_count(C_FAX), 1);

	CASE("the calling fax tone does not start a switch");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();
	fax_connect(0);
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CNG, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 0);

	CASE("nor the V.21 preamble, whose only reference role was T.38");
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 0);

	CASE("nor the V.8 CM, the fax timeout or an unmapped tone");
	inject_tone(0, HAL_TONE_DET_FAX_CM, HAL_TONE_DIR_LOCAL, 0);
	inject_tone(0, HAL_TONE_DET_FAX_TIMEOUT, HAL_TONE_DIR_LOCAL, 0);
	inject_tone(0, HAL_TONE_DET_OTHER, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 0);

	CASE("each remaining member of the answer tone family does switch");
	inject_tone(0, HAL_TONE_DET_ANS, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 1);
	rec_cb.ended(0, 0, &info);
	inject_tone(0, HAL_TONE_DET_ANSAM, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 2);
	rec_cb.ended(0, 0, &info);
	inject_tone(0, HAL_TONE_DET_ANSBAR, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 3);

	CASE("an idle line with no call at all switches nothing");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(cop_count(C_FAX), 0);

	CASE("a call that is not answered yet switches nothing");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	CHECK(state_is(0, "CALLING"));
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 0);

	CASE("and the answer behind it lets the CED switch the leg");
	rec_cb.answered(0, 0);
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 1);

	/*
	 * A line with two legs is not one answered call. The subscriber
	 * holding a call and
	 * talking on another has no single stream to convert, so the tone is
	 * dropped rather than applied to whichever leg happens to be active.
	 */
	CASE("a line holding a second call switches nothing");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();
	fax_connect(0);
	inject_incoming(0, 1, "5551234", "Caller", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 0);

	CASE("a leg layer that refuses the switch only costs a trace");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();
	fax_connect(0);
	fax_start_refuse = true;
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 1);
	CHECK(state_is(0, "CONNECTED"));

	CASE("and the refused switch is latched, so no CED burst retries it");
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 0);
	fax_start_refuse = false;
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("a tone for a line this board does not carry is dropped");
	cops_reset();
	inject_tone(9, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 0);

	/*
	 * The switch follows the active slot, and so does the latch. The
	 * subscriber answers a waiting call and the first caller then hangs up,
	 * which leaves the line CONNECTED on slot 1. A switch that named the
	 * active slot but a latch that was keyed to slot 0 would be cleared by
	 * the wrong leg's release: the field consequence is a line that never
	 * faxes again, because slot 1 stays latched for the life of the daemon
	 * while every later call on it drops its CED.
	 */
	CASE("a waiting call puts the fax line in CONNECTED on slot 1");
	cw_offer_line_connect();
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(state_is(0, "PENDING"));
	rec_cb.reinvite_done(0, 0, true);
	rec_cb.remote_bye(0, 0);
	rec_cb.ended(0, 0, &info);
	CHECK(state_is(0, "CONNECTED"));

	CASE("the switch of that line names slot 1, not slot 0");
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 1);
	CHECK_UINT(cop_find(C_FAX)->slot, 1);

	CASE("and the end of the OTHER slot leaves that latch standing");
	rec_cb.ended(0, 0, &info);
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 0);

	CASE("only the end of slot 1 re-arms it");
	rec_cb.ended(0, 1, &info);
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 1);
	CHECK_UINT(cop_find(C_FAX)->slot, 1);
}

static void fax_line_connect(bool t38)
{
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	cfg.channel[0].fax_t38_enable = t38;
	fake_features_extra = HAL_FEAT_T38;
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	cops_reset();
}

static void t38_line_connect(void)
{
	fax_line_connect(true);
}

static void t_t38(void)
{
	struct call_ended info;
	struct line_view v;

	/* the leg release report, whose only readers here are the two latches */
	memset(&info, 0, sizeof(info));
	info.answered = true;
	info.media_ran = true;
	info.number = "5";
	info.remote_number = "";
	info.remote_name = "";

	CASE("the backend feature mask reaches the leg layer at the bring-up");
	t38_line_connect();
	CHECK_UINT(rec_hal_features,
		   HAL_FEAT_POLARITY | HAL_FEAT_LOOP | HAL_FEAT_T38);
	/* the codec mask crosses on the same push */
	CHECK_UINT(rec_hal_codecs, fake_cap.codecs);

	/*
	 * calls_init() clears the mask (call.h), so a push in front of it would
	 * leave the leg layer carrying no feature at all. The bring-up runs by
	 * hand here, because fixture_start() drops the recorded ops behind it.
	 */
	CASE("and it is pushed after calls_init(), which clears it");
	lines_stop();
	cops_reset();
	lines_bring_up();
	CHECK(cop_index(C_HAL_FEATURES) > cop_index(C_INIT));
	CHECK(cop_index(C_HAL_FEATURES) > cop_index(C_RELOAD));
	CHECK_UINT(rec_hal_features,
		   HAL_FEAT_POLARITY | HAL_FEAT_LOOP | HAL_FEAT_T38);

	/*
	 * The attach that probed nothing. A bring-up that failed at the attach
	 * hands back a handle with an empty capability, and the DSP start is
	 * what probes it: the early half then pushes a mask of zero, and a leg
	 * that read only that would carry no optional feature
	 * for the life of the daemon. The late half runs on the probe edge and
	 * pushes what the board really reports.
	 */
	CASE("an attach that probed nothing pushes an empty mask at the early half");
	lines_stop();
	fake_cap.features = 0;
	cops_reset();
	rec_hal_features = HAL_FEAT_T38;	/* a value the push has to replace */
	lines_init(&cfg);
	lines_live = true;
	CHECK_UINT(cop_count(C_HAL_FEATURES), 1);
	CHECK_UINT(rec_hal_features, 0);

	CASE("and the DSP start pushes the capability it probed");
	fake_cap.features = HAL_FEAT_POLARITY | HAL_FEAT_T38;
	lines_start(&cfg);
	CHECK_UINT(cop_count(C_HAL_FEATURES), 2);
	CHECK_UINT(rec_hal_features, HAL_FEAT_POLARITY | HAL_FEAT_T38);

	/*
	 * The first of the two rules the upgrade runs on: the leg is already in
	 * G.711 passthrough, which only the local CED arms. T.30 has both
	 * machines send V.21 flags, so a preamble on a leg without it belongs to
	 * the calling side and the far end owns that offer.
	 */
	CASE("a V.21 preamble without a passthrough starts no T.38");
	t38_line_connect();
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_T38), 0);
	CHECK_UINT(cop_count(C_FAX), 0);

	/* the kind describes a passthrough this leg never took, so the shell
	 * has nothing to name */
	CASE("and a leg that never switched to passthrough names no kind");
	CHECK_UINT(cop_count(C_FAX_KIND), 0);

	CASE("and the dropped preamble does not move the machine");
	CHECK(state_is(0, "CONNECTED"));

	CASE("and neither latch is spent by it");
	view_get(0, &v);
	CHECK(!v.t38_switched_set);
	CHECK(!v.fax_switched_set);

	CASE("a local CED and the preamble behind it upgrade the active leg to T.38");
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK(cop_has(C_T38));
	CHECK_UINT(cop_count(C_T38), 1);
	CHECK_UINT(cop_find(C_T38)->line, 0);
	CHECK_UINT(cop_find(C_T38)->slot, 0);

	CASE("the status dump carries the latch of that slot alone");
	view_get(0, &v);
	CHECK(v.t38_switched_set);
	CHECK(v.t38_switched[0]);
	CHECK(!v.t38_switched[1]);

	CASE("a second preamble on the same leg is latched and dropped");
	cops_reset();
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_T38), 0);

	CASE("a preamble from the wire is dropped: the far end owns that switch");
	t38_line_connect();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	cops_reset();
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_REMOTE, 0);
	CHECK_UINT(cop_count(C_T38), 0);

	CASE("and the local preamble behind it still fires, so nothing latched");
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_T38), 1);

	CASE("an idle line with no call at all upgrades nothing");
	t38_line_connect();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	cops_reset();
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_T38), 0);

	CASE("a call that is not answered yet upgrades nothing");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	CHECK(state_is(0, "CALLING"));
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_T38), 0);

	CASE("and the answer behind it lets the preamble upgrade the leg");
	rec_cb.answered(0, 0);
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_T38), 1);

	CASE("a line holding a second call upgrades nothing");
	t38_line_connect();
	inject_incoming(0, 1, "5551234", "Caller", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	cops_reset();
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_T38), 0);

	/* the passthrough of stage 1 is armed first, so the option is the gate
	 * this case reads and not the rule above it */
	CASE("a channel with fax_t38_enable 0 never reaches the leg layer");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fake_features_extra = HAL_FEAT_T38;
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_T38), 0);

	CASE("and its latch is not spent, because no attempt was made");
	view_get(0, &v);
	CHECK(!v.t38_switched_set);

	/*
	 * The kind is not the upgrade. A channel with T.38 disabled still runs
	 * the G.711 passthrough of stage 1, and the preamble still says what
	 * that carrier is, so the drop above must not swallow it.
	 */
	CASE("but a channel with T.38 disabled still names the kind of its passthrough");
	CHECK_UINT(cop_count(C_FAX), 1);
	CHECK_UINT(cop_count(C_FAX_KIND), 1);
	CHECK_UINT(cop_find(C_FAX_KIND)->val, HAL_PASSTHROUGH_FAX);

	CASE("a leg layer that refuses the upgrade only costs a trace");
	t38_line_connect();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	t38_start_answer = CALL_T38_REFUSED;
	cops_reset();
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_T38), 1);
	CHECK(state_is(0, "CONNECTED"));

	/* a peer that refused one image offer
	 * must not be offered a second one by the next preamble burst */
	CASE("and the refused upgrade is latched, so no preamble retries it");
	cops_reset();
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_T38), 0);
	t38_start_answer = CALL_T38_STARTED;

	/*
	 * The state refusal is the one the shell does not spend the latch on.
	 * The exchange in the leg's way is the upspeed of stage 1, which the
	 * preamble follows by a second or two, and the preamble repeats through
	 * the whole T.30 negotiation: the one that arrives behind the upspeed
	 * answer is the one that upgrades.
	 */
	CASE("a V.21 preamble under the open upspeed keeps the T.38 latch for the next one");
	t38_line_connect();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	t38_start_answer = CALL_T38_BUSY;
	cops_reset();
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_T38), 1);
	view_get(0, &v);
	CHECK(!v.t38_switched_set);

	CASE("and the preamble behind the upspeed answer takes both the upgrade and the latch");
	t38_start_answer = CALL_T38_STARTED;
	cops_reset();
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_T38), 1);
	view_get(0, &v);
	CHECK(v.t38_switched[0]);

	CASE("and the one behind that is latched and dropped");
	cops_reset();
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_T38), 0);

	/* The two stages on one leg, in the order a fax call makes them: the
	 * answer tone converts the DSP and the preamble upgrades the session */
	CASE("the answer tone switches to passthrough first");
	t38_line_connect();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 1);
	CHECK_UINT(cop_count(C_T38), 0);

	CASE("and the preamble behind it upgrades the same leg");
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 1);
	CHECK_UINT(cop_count(C_T38), 1);
	CHECK_UINT(cop_find(C_T38)->slot, 0);

	/*
	 * The same tone names the kind of the passthrough stage 1 applied. The
	 * answer tone could not say whether a fax or a modem answered; this one
	 * says fax, and the backend runs a different canceller policy for the
	 * two (hal.h hal_media.passthrough).
	 */
	CASE("and it names the fax kind of that same leg, in front of the upgrade");
	CHECK_UINT(cop_count(C_FAX_KIND), 1);
	CHECK_UINT(cop_find(C_FAX_KIND)->slot, 0);
	CHECK_UINT(cop_find(C_FAX_KIND)->val, HAL_PASSTHROUGH_FAX);
	CHECK(cop_index(C_FAX_KIND) < cop_index(C_T38));

	CASE("a preamble from the wire names no kind either");
	cops_reset();
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_REMOTE, 0);
	CHECK_UINT(cop_count(C_FAX_KIND), 0);

	/* the preamble repeats through the T.30 negotiation, and the leg
	 * answers a kind it already holds with no work, so no latch is
	 * needed here and the T.38 latch must not swallow the tone */
	CASE("a second local preamble names the kind again, though T.38 is latched");
	cops_reset();
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX_KIND), 1);
	CHECK_UINT(cop_count(C_T38), 0);

	CASE("a leg layer that refuses the kind only costs a trace");
	fax_kind_refuse = true;
	cops_reset();
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX_KIND), 1);
	CHECK(state_is(0, "CONNECTED"));
	fax_kind_refuse = false;

	CASE("the dump then carries both latches for that slot");
	view_get(0, &v);
	CHECK(v.fax_switched_set);
	CHECK(v.t38_switched_set);
	CHECK(v.fax_switched[0]);
	CHECK(v.t38_switched[0]);

	CASE("neither latch re-arms the other");
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 0);
	CHECK_UINT(cop_count(C_T38), 0);

	CASE("the end of the leg re-arms both switches for the next call");
	rec_cb.ended(0, 0, &info);
	view_get(0, &v);
	CHECK(!v.fax_switched_set);
	CHECK(!v.t38_switched_set);
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 1);
	CHECK_UINT(cop_count(C_T38), 1);

	/*
	 * The followed leg, which is where the two rules of the upgrade come
	 * apart. Such a leg reached the G.711 carrier by recognizing the far
	 * end's own upspeed, so it satisfies rule 1 with no latch of its own,
	 * and the tone that put it there came from the wire, so it is the
	 * calling side and rule 2 forbids its image offer. The channel below
	 * has fax_t38_enable on and the leg layer reports the carrier, which is
	 * exactly the state that would offer T.38 if the latch were not read
	 * again past the kind.
	 */
	CASE("a followed leg names the kind of the carrier it already runs");
	t38_line_connect();
	fax_on_answer = true;
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_MODEM));
	cops_reset();
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX_KIND_GET), 1);
	CHECK_UINT(cop_find(C_FAX_KIND_GET)->slot, 0);
	CHECK_UINT(cop_count(C_FAX_KIND), 1);
	CHECK_UINT(cop_find(C_FAX_KIND)->slot, 0);
	CHECK_UINT(cop_find(C_FAX_KIND)->val, HAL_PASSTHROUGH_FAX);

	CASE("and offers NO T.38, though the channel enables it");
	CHECK(cfg.channel[0].fax_t38_enable);
	CHECK_UINT(cop_count(C_T38), 0);
	CHECK(state_is(0, "CONNECTED"));

	CASE("and it spends neither latch, so the leg keeps its media");
	view_get(0, &v);
	CHECK(!v.fax_switched_set);
	CHECK(!v.t38_switched_set);

	/* the preamble repeats through the T.30 negotiation, and every one of
	 * them takes the same two answers */
	CASE("a second preamble on that leg answers the same way");
	cops_reset();
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX_KIND), 1);
	CHECK_UINT(cop_count(C_T38), 0);

	/* the regression the gate above must not break: a leg that took the
	 * local latch is the answering side and still offers the upgrade */
	CASE("the leg that took the local latch still upgrades");
	t38_line_connect();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	fax_on_answer = true;
	cops_reset();
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX_KIND), 1);
	CHECK_UINT(cop_count(C_T38), 1);
	CHECK(cop_index(C_FAX_KIND) < cop_index(C_T38));
	CHECK_UINT(cop_count(C_FAX_KIND_GET), 0);
	CHECK_UINT(cop_count(C_FAX_ON), 0);
	fax_on_answer = false;

	CASE("a local CED on a followed profile takes no latch or upspeed");
	t38_line_connect();
	fax_on_answer = true;
	CHECK(call_fax_kind_set(0, 0, HAL_PASSTHROUGH_MODEM));
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX_ON), 1);
	CHECK_UINT(cop_count(C_FAX), 0);
	view_get(0, &v);
	CHECK(!v.fax_switched_set);

	CASE("a preamble after the dropped local CED offers no T.38");
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX_KIND), 1);
	CHECK_UINT(cop_find(C_FAX_KIND)->val, HAL_PASSTHROUGH_FAX);
	CHECK_UINT(cop_count(C_T38), 0);
	view_get(0, &v);
	CHECK(!v.fax_switched_set);
	CHECK(!v.t38_switched_set);

	CASE("a kind-only preamble selects FAX with T.38 enabled");
	t38_line_connect();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 0);
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_MODEM);
	CHECK(!fax_on_answer);
	cops_reset();
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX_KIND), 1);
	CHECK_UINT(cop_find(C_FAX_KIND)->slot, 0);
	CHECK_UINT(cop_find(C_FAX_KIND)->val, HAL_PASSTHROUGH_FAX);
	CHECK_UINT(cop_count(C_T38), 0);
	CHECK_UINT(cop_count(C_FAX), 0);
	view_get(0, &v);
	CHECK(!v.fax_switched_set);
	CHECK(!v.t38_switched_set);

	CASE("a kind-only preamble selects FAX with T.38 disabled");
	fax_line_connect(false);
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 0);
	CHECK_UINT(call_fax_kind_get(0, 0), HAL_PASSTHROUGH_MODEM);
	CHECK(!fax_on_answer);
	cops_reset();
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX_KIND), 1);
	CHECK_UINT(cop_find(C_FAX_KIND)->slot, 0);
	CHECK_UINT(cop_find(C_FAX_KIND)->val, HAL_PASSTHROUGH_FAX);
	CHECK_UINT(cop_count(C_T38), 0);
	CHECK_UINT(cop_count(C_FAX), 0);
	view_get(0, &v);
	CHECK(!v.fax_switched_set);
	CHECK(!v.t38_switched_set);

	CASE("a leg with no latch and no carrier still names no kind");
	t38_line_connect();
	cops_reset();
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX_KIND), 0);
	CHECK_UINT(cop_count(C_T38), 0);

	/*
	 * The upgrade follows the active slot, and so does its latch. The
	 * subscriber answers a waiting call and the first caller hangs up, which
	 * leaves the line CONNECTED on slot 1. A latch keyed to slot 0 would be
	 * cleared by the wrong leg's release, and slot 1 would never fax again.
	 */
	CASE("a waiting call puts the T.38 line in CONNECTED on slot 1");
	t38_line_connect();
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(state_is(0, "PENDING"));
	rec_cb.reinvite_done(0, 0, true);
	rec_cb.remote_bye(0, 0);
	rec_cb.ended(0, 0, &info);
	CHECK(state_is(0, "CONNECTED"));

	CASE("the upgrade of that line names slot 1, not slot 0");
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	cops_reset();
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_T38), 1);
	CHECK_UINT(cop_find(C_T38)->slot, 1);
	view_get(0, &v);
	CHECK(!v.t38_switched[0]);
	CHECK(v.t38_switched[1]);

	CASE("and only the end of slot 1 re-arms it");
	rec_cb.ended(0, 0, &info);
	cops_reset();
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_T38), 0);
	rec_cb.ended(0, 1, &info);
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_T38), 1);
	CHECK_UINT(cop_find(C_T38)->slot, 1);

	/*
	 * The T.38 stream report is diagnostic. The SIP session decides when an
	 * image session ends, so the record is consumed and traced and drives
	 * nothing: a call it moved would end a fax the far end is still sending.
	 */
	CASE("a T.38 stream report moves nothing on the line");
	t38_line_connect();
	inject_t38(0, 0, 0, 0);
	inject_t38(0, 0, 1, 96);
	inject_t38(0, 1, 9, 0);
	CHECK(state_is(0, "CONNECTED"));
	CHECK_UINT(ncops, 0);
	CHECK_UINT(cop_count(C_T38), 0);
	CHECK_UINT(cop_count(C_HANGUP), 0);

	/*
	 * The trace is the behavior of this record, so it is read where it
	 * lands, the way the derive warnings are. It names the two states of
	 * hal.h u.t38.state, because a number in a log tells a field engineer
	 * nothing about a fax that ended.
	 */
	CASE("the report is traced with the state named and the stream it belongs to");
	trace_domain_set(TRACE_HAL, true);
	warn_capture_begin();
	inject_t38(0, 0, 0, 0);
	inject_t38(0, 1, 1, 96);
	inject_t38(0, 0, 9, 0);
	CHECK_UINT(warn_capture_count("line 0 stream 0 t38 disconnect"), 1);
	CHECK_UINT(warn_capture_count("line 0 stream 1 t38 payload-type-change"), 1);
	CHECK_UINT(warn_capture_count("t38 unknown"), 1);
	/* the record's payload_type has no producer: UDPTL carries no payload
	 * type, so the trace names it nowhere */
	CHECK_UINT(warn_capture_count("payload type"), 0);
	warn_capture_end();
	trace_domain_set(TRACE_HAL, false);

	CASE("a T.38 report for a line this board does not carry is dropped");
	inject_t38(9, 0, 0, 0);
	CHECK(state_is(0, "CONNECTED"));
	CHECK_UINT(ncops, 0);

	/* the fixture is shared: every later block runs without the feature */
	fake_features_extra = 0;
}

/*
 * The wait behind the end row of a plain answer tone (call.h
 * CALL_FAX_EXIT_DEFER_MS, 1500 ms). The margins straddle the deadline the way
 * the call-waiting record above does: nothing may fire inside the first window
 * and everything must have fired by the end of the second. A pump only ever
 * overruns, so it can widen the late margin and never the early one.
 */
#define FAX_EXIT_EARLY_MS	(CALL_FAX_EXIT_DEFER_MS - 200)
#define FAX_EXIT_LATE_MS	400

/* the connected line of the fax cases, with a plain answer tone reported from
 * the feed the case names and its end row behind it */
static void fax_exit_arm_line(unsigned int line, uint8_t dir, uint16_t ms)
{
	fax_tone_answer = CALL_FAX_TONE_ENDED;
	inject_tone(line, HAL_TONE_DET_CED, dir, 0);
	inject_tone_ms(line, HAL_TONE_DET_CED, dir, 1, ms);
}

/* the connected line the return-to-voice cases start from: the one-digit map
 * of the fax cases, the fixture, the region and one answered leg */
static void fax_exit_line_up(void)
{
	cfg_default();
	snprintf(cfg.channel[0].digit_map,
		 sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();
	fax_connect(0);
}

/*
 * The line half of the return to voice. The rule belongs to the leg layer
 * (call.h call_fax_exit), and these cases check what this shell owns: the
 * deferral, and the rows that cancel it. They also check the end rows that
 * the two stages must never see, and the leg that the handle names. Last,
 * they check the stage 1 latch that a true exit arms again.
 */
static void t_fax_exit(void)
{
	struct call_ended info;
	struct line_view v;

	/* the leg release report, whose readers here are the two latches and
	 * the deferral */
	memset(&info, 0, sizeof(info));
	info.answered = true;
	info.media_ran = true;
	info.number = "5";
	info.remote_number = "";
	info.remote_name = "";

	CASE("every tone row reaches the rule before any gate reads it");
	fax_exit_line_up();
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 0);
	CHECK_UINT(cop_count(C_FAX_TONE_NOTE), 1);
	CHECK_UINT(cop_find(C_FAX_TONE_NOTE)->val, HAL_TONE_DET_CED);
	CHECK_UINT(cop_find(C_FAX_TONE_NOTE)->dir, HAL_TONE_DIR_REMOTE);
	CHECK(!cop_find(C_FAX_TONE_NOTE)->end);
	CHECK(cop_index(C_FAX_TONE_NOTE) < cop_index(C_FAX_HINT));
	CHECK(cop_index(C_FAX_TONE_NOTE) < cop_index(C_FAX_KIND));

	CASE("and a CNG the gates drop is recorded all the same");
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CNG, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX_TONE_NOTE), 1);
	CHECK_UINT(cop_find(C_FAX_TONE_NOTE)->val, HAL_TONE_DET_CNG);

	/*
	 * The end row carries the duration the rule counts, and it reaches
	 * neither the hint nor the kind: the detector reports both edges and
	 * every gate below reads a start.
	 */
	CASE("the end row of an answer tone starts nothing and defers an exit");
	cops_reset();
	fax_exit_arm_line(0, HAL_TONE_DIR_REMOTE, 900);
	CHECK_UINT(cop_count(C_FAX_TONE_NOTE), 2);
	CHECK_UINT(cop_count(C_FAX_HINT), 1);
	CHECK_UINT(cop_count(C_FAX_KIND), 1);
	CHECK_UINT(cop_count(C_FAX), 0);
	CHECK_UINT(cops[ncops - 2].duration_ms, 900);
	CHECK(cops[ncops - 2].end);

	CASE("and the deferral takes the handle of the leg it was armed on");
	CHECK_UINT(cop_count(C_LEG_HANDLE), 1);
	CHECK_UINT(cop_find(C_LEG_HANDLE)->slot, 0);

	CASE("nothing returns to voice inside the deferral");
	uloop_pump(FAX_EXIT_EARLY_MS);
	CHECK_UINT(cop_count(C_FAX_EXIT), 0);

	CASE("and the exit runs once behind it, on that handle");
	fax_exit_answer = true;
	uloop_pump(FAX_EXIT_LATE_MS);
	CHECK_UINT(cop_count(C_FAX_EXIT), 1);
	CHECK_UINT(cop_find(C_FAX_EXIT)->val, cop_find(C_LEG_HANDLE)->val);
	CHECK_UINT(cop_find(C_FAX_EXIT)->line, 0);
	CHECK_UINT(cop_find(C_FAX_EXIT)->slot, 0);

	/* --- the rows that cancel the deferral --- */

	/*
	 * A start row of the answer family, of the V.21 preamble or of a modem
	 * carrier says the tone was real. Two lines are armed and cancelled by
	 * different rows on different feeds, so one pump answers for both.
	 */
	CASE("a modem carrier and a preamble each cancel a standing deferral");
	cfg_default();
	snprintf(cfg.channel[0].digit_map,
		 sizeof(cfg.channel[0].digit_map), "5");
	snprintf(cfg.channel[1].digit_map,
		 sizeof(cfg.channel[1].digit_map), "5");
	cfg.channel[1].enabled = true;
	fixture_start();
	region_ready();
	fax_connect(0);
	fax_connect(1);
	fax_exit_arm_line(0, HAL_TONE_DIR_REMOTE, 900);
	fax_exit_arm_line(1, HAL_TONE_DIR_REMOTE, 900);
	cops_reset();
	fax_exit_answer = true;
	inject_tone(0, HAL_TONE_DET_MODEM, HAL_TONE_DIR_REMOTE, 0);
	inject_tone(1, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	uloop_pump(FAX_EXIT_EARLY_MS + FAX_EXIT_LATE_MS);
	CHECK_UINT(cop_count(C_FAX_EXIT), 0);

	CASE("and so does a second answer tone, on either feed");
	fax_exit_arm_line(0, HAL_TONE_DIR_REMOTE, 900);
	fax_exit_arm_line(1, HAL_TONE_DIR_LOCAL, 900);
	cops_reset();
	inject_tone(0, HAL_TONE_DET_ANSAM, HAL_TONE_DIR_LOCAL, 0);
	inject_tone(1, HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 0);
	uloop_pump(FAX_EXIT_EARLY_MS + FAX_EXIT_LATE_MS);
	CHECK_UINT(cop_count(C_FAX_EXIT), 0);

	/* a tone that proves nothing leaves the deferral standing */
	CASE("a CNG cancels nothing");
	fax_exit_arm_line(0, HAL_TONE_DIR_REMOTE, 900);
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CNG, HAL_TONE_DIR_REMOTE, 0);
	uloop_pump(FAX_EXIT_EARLY_MS + FAX_EXIT_LATE_MS);
	CHECK_UINT(cop_count(C_FAX_EXIT), 1);

	/*
	 * The deadline itself, from both sides. A carrier the detector reports
	 * 100 ms before it is still in time, and one that lands behind the
	 * expiry has nothing left to cancel: the leg is already back on voice
	 * and the tone that follows is a new conversion, not a repair of this
	 * one.
	 */
	CASE("evidence just inside the deadline cancels the deferral");
	fax_exit_arm_line(0, HAL_TONE_DIR_REMOTE, 900);
	cops_reset();
	uloop_pump(FAX_EXIT_EARLY_MS + 100);
	CHECK_UINT(cop_count(C_FAX_EXIT), 0);
	inject_tone(0, HAL_TONE_DET_MODEM, HAL_TONE_DIR_REMOTE, 0);
	uloop_pump(FAX_EXIT_LATE_MS + 400);
	CHECK_UINT(cop_count(C_FAX_EXIT), 0);

	CASE("and evidence behind it leaves the return to voice standing");
	fax_exit_arm_line(0, HAL_TONE_DIR_REMOTE, 900);
	cops_reset();
	uloop_pump(FAX_EXIT_EARLY_MS + FAX_EXIT_LATE_MS);
	CHECK_UINT(cop_count(C_FAX_EXIT), 1);
	inject_tone(0, HAL_TONE_DET_MODEM, HAL_TONE_DIR_REMOTE, 0);
	uloop_pump(FAX_EXIT_EARLY_MS + FAX_EXIT_LATE_MS);
	CHECK_UINT(cop_count(C_FAX_EXIT), 1);

	/* --- the end rows the two stages never see --- */

	/*
	 * A leg the preamble already moved to the fax kind is carrying a fax,
	 * and the end row of the answer tone that started it must not run the
	 * upspeed a second time or move the kind back.
	 */
	CASE("an answer tone end row behind the fax kind moves nothing");
	fax_exit_line_up();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 0);
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(fax_kind_answer, HAL_PASSTHROUGH_FAX);
	cops_reset();
	fax_tone_answer = CALL_FAX_TONE_ENDED;
	inject_tone_ms(0, HAL_TONE_DET_CED, HAL_TONE_DIR_REMOTE, 1, 900);
	CHECK_UINT(cop_count(C_FAX_TONE_NOTE), 1);
	CHECK_UINT(cop_count(C_FAX_HINT), 0);
	CHECK_UINT(cop_count(C_FAX_KIND), 0);
	CHECK_UINT(cop_count(C_FAX), 0);

	CASE("and the exit of that deferral is the leg's own answer");
	fax_exit_answer = false;
	uloop_pump(FAX_EXIT_EARLY_MS + FAX_EXIT_LATE_MS);
	CHECK_UINT(cop_count(C_FAX_EXIT), 1);

	/*
	 * The preamble repeats through the T.30 negotiation and its end rows
	 * repeat with it. A leg that offered one image session must not be
	 * offered another by an edge the detector reports twice.
	 */
	CASE("a V.21 end row retries no image offer and arms no deferral");
	t38_line_connect();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	inject_tone(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_T38), 1);
	cops_reset();
	fax_tone_answer = CALL_FAX_TONE_ENDED;
	fax_exit_answer = true;
	inject_tone_ms(0, HAL_TONE_DET_V21_PREAMBLE, HAL_TONE_DIR_LOCAL, 1,
		       900);
	CHECK_UINT(cop_count(C_T38), 0);
	CHECK_UINT(cop_count(C_FAX_KIND), 0);
	CHECK_UINT(cop_count(C_LEG_HANDLE), 0);
	uloop_pump(FAX_EXIT_EARLY_MS + FAX_EXIT_LATE_MS);
	CHECK_UINT(cop_count(C_FAX_EXIT), 0);

	/* --- the stage 1 latch --- */

	/*
	 * The leg carries voice again, so a second answer tone from the phone
	 * is a new conversion. The latch is the one state the shell owns here,
	 * and only a true exit re-arms it.
	 */
	CASE("a true exit re-arms the stage 1 latch of that leg");
	fax_exit_line_up();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 1);
	view_get(0, &v);
	CHECK(v.fax_switched_set);
	CHECK(v.fax_switched[0]);
	fax_tone_answer = CALL_FAX_TONE_ENDED;
	fax_exit_answer = true;
	inject_tone_ms(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 1, 900);
	uloop_pump(FAX_EXIT_EARLY_MS + FAX_EXIT_LATE_MS);
	CHECK_UINT(cop_count(C_FAX_EXIT), 1);
	view_get(0, &v);
	CHECK(!v.fax_switched_set);

	CASE("and the next answer tone from the phone converts again");
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 1);

	CASE("a refused exit leaves that latch where it was");
	fax_exit_line_up();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	fax_tone_answer = CALL_FAX_TONE_ENDED;
	fax_exit_answer = false;
	inject_tone_ms(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 1, 900);
	uloop_pump(FAX_EXIT_EARLY_MS + FAX_EXIT_LATE_MS);
	CHECK_UINT(cop_count(C_FAX_EXIT), 1);
	view_get(0, &v);
	CHECK(v.fax_switched_set);
	CHECK(v.fax_switched[0]);

	CASE("and no burst retries the upspeed of that leg");
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK_UINT(cop_count(C_FAX), 0);

	/* --- the leg the deferral names --- */

	/*
	 * The leg ended inside the window. Its record is the one edge that
	 * re-arms the latches of the slot, and it drops the deferral with them:
	 * the handle names a call that is gone.
	 */
	CASE("a leg that ends inside the window cancels its own deferral");
	fax_exit_line_up();
	fax_exit_arm_line(0, HAL_TONE_DIR_REMOTE, 900);
	cops_reset();
	fax_exit_answer = true;
	rec_cb.ended(0, 0, &info);
	uloop_pump(FAX_EXIT_EARLY_MS + FAX_EXIT_LATE_MS);
	CHECK_UINT(cop_count(C_FAX_EXIT), 0);

	/*
	 * A transfer moves a UI name between two physical legs, so the slot
	 * read at expiry can name a call this tone never belonged to. The
	 * handle is taken when the deferral is armed and carried to the end.
	 */
	CASE("a deferral names the call it was armed on and not the slot");
	fax_exit_line_up();
	fax_exit_arm_line(0, HAL_TONE_DIR_REMOTE, 900);
	CHECK_UINT(cop_count(C_LEG_HANDLE), 1);
	cops_reset();
	fax_exit_answer = false;
	leg_gen_answer = 2;
	uloop_pump(FAX_EXIT_EARLY_MS + FAX_EXIT_LATE_MS);
	CHECK_UINT(cop_count(C_FAX_EXIT), 1);
	CHECK_UINT(cop_count(C_LEG_HANDLE), 0);
	CHECK_UINT(cop_find(C_FAX_EXIT)->val, leg_handle_of(0, 0, 1));

	/*
	 * A daemon that stops cancels every timer of the line in front of the
	 * re-init memset that would corrupt the uloop list an armed timer is
	 * linked into.
	 */
	CASE("a line that stops drops the deferral it was holding");
	fax_exit_line_up();
	fax_exit_arm_line(0, HAL_TONE_DIR_REMOTE, 900);
	fax_exit_answer = true;
	fixture_start();
	region_ready();
	uloop_pump(FAX_EXIT_EARLY_MS + FAX_EXIT_LATE_MS);
	CHECK_UINT(cop_count(C_FAX_EXIT), 0);
}

static void t_media_ops(void)
{
	struct hal_media media = {};
	uint16_t first = 0, next = 0;

	CASE("the media ops reach the backend stream configuration");
	cfg_default();
	fixture_start();
	CHECK(rec_media.config_set != NULL);
	CHECK_UINT(rec_media.config_set(rec_media.priv, 0, 1, &media), HAL_OK);
	CHECK(hop_has(H_STRM_CFG));
	CHECK_UINT(hop_find(H_STRM_CFG)->val, 1);

	CASE("the media ops start and stop a stream");
	hops_reset();
	CHECK_UINT(rec_media.start(rec_media.priv, 0, 0), HAL_OK);
	CHECK_UINT(rec_media.stop(rec_media.priv, 0, 0), HAL_OK);
	CHECK(hop_has(H_STRM_START));
	CHECK(hop_has(H_STRM_STOP));

	CASE("the media ops relay one DTMF digit");
	hops_reset();
	CHECK_UINT(rec_media.dtmf_send(rec_media.priv, 0, 0, '7', 100), HAL_OK);
	CHECK(hop_has(H_STRM_DTMF));
	CHECK(hop_find(H_STRM_DTMF)->digit == '7');

	/*
	 * The leg builds the profile and the shell carries it to the backend,
	 * so this case proves the crossing: the whole record has to arrive at
	 * the named stream, because a profile that landed on the other stream
	 * would convert the wrong call.
	 */
	CASE("the media ops carry a fax profile to the named stream");
	hops_reset();
	CHECK(rec_media.fax_apply != NULL);

	/*
	 * The leg draws its two SDES master keys through this op, because the
	 * entropy source lives in the socket tier the leg suite does not link
	 * (call.h, rand_bytes). What the op does is pinned in test_call through
	 * the media ops it injects; what this suite owns is the binding,
	 * because a shell that forgot to fill the entry would leave every leg
	 * on plain RTP with the config asking for SRTP and nothing failing.
	 */
	CASE("the media ops carry the entropy op the SRTP legs key from");
	CHECK(rec_media.rand_bytes != NULL);
	{
		struct hal_fax_profile fax = {
			.mode = HAL_FAX_PASSTHROUGH,
			.codec = HAL_CODEC_PCMA,
			.codec_pt = 8,
			.ec_disable = true,
			.vad_disable = true,
			.cng_disable = true,
			.silence_comp_disable = true,
			.jb_fixed = true,
		};
		struct hop *op;

		CHECK_UINT(rec_media.fax_apply(rec_media.priv, 0, 1, &fax), HAL_OK);
		CHECK(hop_has(H_FAX));
		op = hop_find(H_FAX);
		CHECK_UINT(op->val, 1);
		CHECK_UINT(op->fax.mode, HAL_FAX_PASSTHROUGH);
		CHECK_UINT(op->fax.codec, HAL_CODEC_PCMA);
		CHECK_UINT(op->fax.codec_pt, 8);
		CHECK(op->fax.ec_disable);
		CHECK(op->fax.vad_disable);
		CHECK(op->fax.cng_disable);
		CHECK(op->fax.silence_comp_disable);
		CHECK(op->fax.jb_fixed);

		CASE("a fax profile the backend refuses answers the refusal");
		fax_apply_status = HAL_UNSUPPORTED;
		CHECK_UINT(rec_media.fax_apply(rec_media.priv, 0, 0, &fax),
			   HAL_UNSUPPORTED);
		fax_apply_status = HAL_OK;

		/*
		 * hal.h section 3, the same retry detector_arm() and
		 * hook_timing_push() make. It matters more here: the switch is
		 * once per leg and the answer tone does not come again, so a
		 * transient refusal that is dropped costs that call its fax.
		 */
		CASE("a transient fax profile refusal is retried once and then works");
		hops_reset();
		fax_apply_transient = 1;
		CHECK_UINT(rec_media.fax_apply(rec_media.priv, 0, 1, &fax), HAL_OK);
		CHECK_UINT(hop_count(H_FAX), 2);

		CASE("and the retry carried the whole profile to the same stream");
		op = hop_last(H_FAX);
		CHECK_UINT(op->val, 1);
		CHECK_UINT(op->fax.mode, HAL_FAX_PASSTHROUGH);
		CHECK_UINT(op->fax.codec, HAL_CODEC_PCMA);
		CHECK(op->fax.jb_fixed);

		/* the retry is one, not a loop: a backend that stays transient
		 * answers transient and the leg hears it */
		CASE("a profile that stays transient is refused after one retry");
		hops_reset();
		fax_apply_transient = 2;
		CHECK_UINT(rec_media.fax_apply(rec_media.priv, 0, 0, &fax),
			   HAL_TRANSIENT);
		CHECK_UINT(hop_count(H_FAX), 2);
		fax_apply_transient = 0;

		CASE("a media op without a HAL fails instead of crashing");
		CHECK_UINT(rec_media.fax_apply(NULL, 0, 0, &fax), HAL_FATAL);
	}

	CHECK_UINT(rec_media.config_set(NULL, 0, 0, &media), HAL_FATAL);
	CHECK_UINT(rec_media.start(NULL, 0, 0), HAL_FATAL);
	CHECK_UINT(rec_media.stop(NULL, 0, 0), HAL_FATAL);
	CHECK_UINT(rec_media.dtmf_send(NULL, 0, 0, '1', 100), HAL_FATAL);

	CASE("lines_init wires the call layer and hands it the config");
	lines_stop();
	cops_reset();
	lines_bring_up();
	CHECK_UINT(cop_count(C_INIT), 1);
	/* calls_init takes no config, so the reload call is what leaves call.c
	 * able to dial */
	CHECK_UINT(cop_count(C_RELOAD), 1);
	CHECK(rec_cb.incoming != NULL);
	CHECK(rec_cb.failed != NULL);
	CHECK(rec_media.priv == &fake_hal);

	CASE("the SIP init hands the host table to the leg layer and the wire");
	CHECK(proto_host_get() != NULL);
	CHECK(rec_calls_host == proto_host_get());
	CHECK(sip_wire_host == proto_host_get());

	CASE("lines_init gives the RTP port pool back");
	lines_stop();
	CHECK(net_port_alloc(10000, 10030, &first));
	CHECK(net_port_alloc(10000, 10030, &next));
	CHECK(next != first);
	lines_bring_up();
	CHECK(net_port_alloc(10000, 10030, &next));
	CHECK_UINT(next, first);
	net_port_reset();
}

/* -------------------------------------------------------- the call record */

/*
 * What the record says, read through the northbound dump like every other
 * state in this suite. Only the fields these cases assert are parsed; the
 * whole shape is pinned by test_cdr, which owns the ring.
 */
#define DVIEW_MAX	8

struct cdr_view {
	uint32_t seq;
	uint32_t line;
	uint32_t slot;
	char media_state[16];
	char number[64];
	uint64_t packets_sent;
	uint64_t bytes_recv;
	bool inbound;
	bool answered;
	bool in_progress;
	bool has_media;
	bool has_jb;
	uint64_t jb_frames_out;
	uint32_t jb_pool_free;
	bool transferred;
	uint32_t transferred_from;
	bool conferenced;
};

enum {
	DV_SEQ,
	DV_LINE,
	DV_SLOT,
	DV_INBOUND,
	DV_ANSWERED,
	DV_NUMBER,
	DV_MEDIA_STATE,
	DV_MEDIA,
	DV_JB,
	DV_IN_PROGRESS,
	DV_TRANSFERRED,
	DV_TRANSFERRED_FROM,
	DV_CONFERENCED,
	__DV_MAX,
};

static const struct blobmsg_policy dv_policy[__DV_MAX] = {
	[DV_SEQ] = { "seq", BLOBMSG_TYPE_INT32 },
	[DV_LINE] = { "line", BLOBMSG_TYPE_INT32 },
	[DV_SLOT] = { "slot", BLOBMSG_TYPE_INT32 },
	[DV_INBOUND] = { "inbound", BLOBMSG_TYPE_BOOL },
	[DV_ANSWERED] = { "answered", BLOBMSG_TYPE_BOOL },
	[DV_NUMBER] = { "number", BLOBMSG_TYPE_STRING },
	[DV_MEDIA_STATE] = { "media_state", BLOBMSG_TYPE_STRING },
	[DV_MEDIA] = { "media", BLOBMSG_TYPE_TABLE },
	[DV_JB] = { "jb", BLOBMSG_TYPE_TABLE },
	[DV_IN_PROGRESS] = { "in_progress", BLOBMSG_TYPE_BOOL },
	[DV_TRANSFERRED] = { "transferred", BLOBMSG_TYPE_BOOL },
	[DV_TRANSFERRED_FROM] = { "transferred_from", BLOBMSG_TYPE_INT32 },
	[DV_CONFERENCED] = { "conferenced", BLOBMSG_TYPE_BOOL },
};

enum {
	DM_PACKETS_SENT,
	DM_BYTES_RECV,
	__DM_MAX,
};

static const struct blobmsg_policy dm_policy[__DM_MAX] = {
	[DM_PACKETS_SENT] = { "packets_sent", BLOBMSG_TYPE_INT64 },
	[DM_BYTES_RECV] = { "bytes_recv", BLOBMSG_TYPE_INT64 },
};

enum {
	DJ_FRAMES_OUT,
	DJ_POOL_FREE,
	__DJ_MAX,
};

static const struct blobmsg_policy dj_policy[__DJ_MAX] = {
	[DJ_FRAMES_OUT] = { "frames_out", BLOBMSG_TYPE_INT64 },
	[DJ_POOL_FREE] = { "pool_free", BLOBMSG_TYPE_INT32 },
};

static struct cdr_view dviews[DVIEW_MAX];

static void dview_take(struct blob_attr *entry, struct cdr_view *v)
{
	struct blob_attr *tb[__DV_MAX] = {};
	struct blob_attr *tm[__DM_MAX] = {};

	memset(v, 0, sizeof(*v));
	blobmsg_parse(dv_policy, __DV_MAX, tb, blobmsg_data(entry),
		      blobmsg_data_len(entry));

	if (tb[DV_SEQ])
		v->seq = blobmsg_get_u32(tb[DV_SEQ]);
	if (tb[DV_LINE])
		v->line = blobmsg_get_u32(tb[DV_LINE]);
	if (tb[DV_SLOT])
		v->slot = blobmsg_get_u32(tb[DV_SLOT]);
	if (tb[DV_INBOUND])
		v->inbound = blobmsg_get_bool(tb[DV_INBOUND]);
	if (tb[DV_ANSWERED])
		v->answered = blobmsg_get_bool(tb[DV_ANSWERED]);
	if (tb[DV_IN_PROGRESS])
		v->in_progress = blobmsg_get_bool(tb[DV_IN_PROGRESS]);
	if (tb[DV_TRANSFERRED])
		v->transferred = blobmsg_get_bool(tb[DV_TRANSFERRED]);
	if (tb[DV_TRANSFERRED_FROM])
		v->transferred_from = blobmsg_get_u32(tb[DV_TRANSFERRED_FROM]);
	/* the conference mark is present only on a leg the mixer joined */
	v->conferenced = tb[DV_CONFERENCED] != NULL;
	if (tb[DV_NUMBER])
		snprintf(v->number, sizeof(v->number), "%s",
			 blobmsg_get_string(tb[DV_NUMBER]));
	if (tb[DV_MEDIA_STATE])
		snprintf(v->media_state, sizeof(v->media_state), "%s",
			 blobmsg_get_string(tb[DV_MEDIA_STATE]));

	v->has_jb = tb[DV_JB] != NULL;
	if (tb[DV_JB]) {
		struct blob_attr *tj[__DJ_MAX] = {};

		blobmsg_parse(dj_policy, __DJ_MAX, tj, blobmsg_data(tb[DV_JB]),
			      blobmsg_data_len(tb[DV_JB]));
		if (tj[DJ_FRAMES_OUT])
			v->jb_frames_out = blobmsg_get_u64(tj[DJ_FRAMES_OUT]);
		if (tj[DJ_POOL_FREE])
			v->jb_pool_free = blobmsg_get_u32(tj[DJ_POOL_FREE]);
	}

	v->has_media = tb[DV_MEDIA] != NULL;
	if (!tb[DV_MEDIA])
		return;

	blobmsg_parse(dm_policy, __DM_MAX, tm, blobmsg_data(tb[DV_MEDIA]),
		      blobmsg_data_len(tb[DV_MEDIA]));
	if (tm[DM_PACKETS_SENT])
		v->packets_sent = blobmsg_get_u64(tm[DM_PACKETS_SENT]);
	if (tm[DM_BYTES_RECV])
		v->bytes_recv = blobmsg_get_u64(tm[DM_BYTES_RECV]);
}

static unsigned int cdr_take(void)
{
	struct blob_attr *cur, *entry;
	unsigned int rem, rem2, n = 0;

	memset(dviews, 0, sizeof(dviews));
	blob_buf_init(&sb, 0);
	cdr_dump(&sb);

	blob_for_each_attr(cur, sb.head, rem) {
		if (strcmp(blobmsg_name(cur), "cdr"))
			continue;

		blobmsg_for_each_attr(entry, cur, rem2) {
			if (n < DVIEW_MAX)
				dview_take(entry, &dviews[n]);
			n++;
		}
	}

	return n;
}

struct cdr_stats_view {
	uint32_t in_received;
	uint32_t in_answered;
	uint32_t in_failed;
	uint32_t out_attempted;
	uint32_t out_answered;
	uint32_t out_failed;
	uint64_t packets_sent;
	uint64_t bytes_recv;
	bool found;
};

enum {
	DS_LINE,
	DS_IN_RECEIVED,
	DS_IN_ANSWERED,
	DS_IN_FAILED,
	DS_OUT_ATTEMPTED,
	DS_OUT_ANSWERED,
	DS_OUT_FAILED,
	DS_TOTALS,
	__DS_MAX,
};

static const struct blobmsg_policy ds_policy[__DS_MAX] = {
	[DS_LINE] = { "line", BLOBMSG_TYPE_INT32 },
	[DS_IN_RECEIVED] = { "in_received", BLOBMSG_TYPE_INT32 },
	[DS_IN_ANSWERED] = { "in_answered", BLOBMSG_TYPE_INT32 },
	[DS_IN_FAILED] = { "in_failed", BLOBMSG_TYPE_INT32 },
	[DS_OUT_ATTEMPTED] = { "out_attempted", BLOBMSG_TYPE_INT32 },
	[DS_OUT_ANSWERED] = { "out_answered", BLOBMSG_TYPE_INT32 },
	[DS_OUT_FAILED] = { "out_failed", BLOBMSG_TYPE_INT32 },
	[DS_TOTALS] = { "totals", BLOBMSG_TYPE_TABLE },
};

static bool cdr_stats(unsigned int line, struct cdr_stats_view *v)
{
	struct blob_attr *cur, *entry;
	unsigned int rem, rem2;

	memset(v, 0, sizeof(*v));
	blob_buf_init(&sb, 0);
	cdr_stats_dump(&sb, -1);

	blob_for_each_attr(cur, sb.head, rem) {
		if (strcmp(blobmsg_name(cur), "stats"))
			continue;

		blobmsg_for_each_attr(entry, cur, rem2) {
			struct blob_attr *tb[__DS_MAX] = {};
			struct blob_attr *tt[__DM_MAX] = {};

			blobmsg_parse(ds_policy, __DS_MAX, tb, blobmsg_data(entry),
				      blobmsg_data_len(entry));
			if (!tb[DS_LINE] || blobmsg_get_u32(tb[DS_LINE]) != line)
				continue;

			v->found = true;
			if (tb[DS_IN_RECEIVED])
				v->in_received = blobmsg_get_u32(tb[DS_IN_RECEIVED]);
			if (tb[DS_IN_ANSWERED])
				v->in_answered = blobmsg_get_u32(tb[DS_IN_ANSWERED]);
			if (tb[DS_IN_FAILED])
				v->in_failed = blobmsg_get_u32(tb[DS_IN_FAILED]);
			if (tb[DS_OUT_ATTEMPTED])
				v->out_attempted = blobmsg_get_u32(tb[DS_OUT_ATTEMPTED]);
			if (tb[DS_OUT_ANSWERED])
				v->out_answered = blobmsg_get_u32(tb[DS_OUT_ANSWERED]);
			if (tb[DS_OUT_FAILED])
				v->out_failed = blobmsg_get_u32(tb[DS_OUT_FAILED]);
			if (!tb[DS_TOTALS])
				continue;

			blobmsg_parse(dm_policy, __DM_MAX, tt,
				      blobmsg_data(tb[DS_TOTALS]),
				      blobmsg_data_len(tb[DS_TOTALS]));
			if (tt[DM_PACKETS_SENT])
				v->packets_sent = blobmsg_get_u64(tt[DM_PACKETS_SENT]);
			if (tt[DM_BYTES_RECV])
				v->bytes_recv = blobmsg_get_u64(tt[DM_BYTES_RECV]);
		}
	}

	return v->found;
}

/* what call.c reports when a leg is released; the fields the shell reads */
static struct call_ended ended_of(bool inbound, bool answered, bool failed,
				  const char *number)
{
	struct call_ended info;

	memset(&info, 0, sizeof(info));
	info.inbound = inbound;
	info.answered = answered;
	info.failed = failed;
	info.fail_class = CALL_FAIL_BUSY;
	info.media_ran = true;
	info.number = number ? number : "";
	info.remote_number = "";
	info.remote_name = "";

	return info;
}

/* the leg layer's snapshot request, which call.c makes from media_stop() */
static void inject_snapshot(unsigned int line, unsigned int stream)
{
	rec_media.stats_snapshot(rec_media.priv, line, stream);
}

static void t_cdr(void)
{
	struct cdr_stats_view s;
	struct call_ended info;
	unsigned int n;

	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();

	CASE("a dialled call opens a record and counts the attempt");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	CHECK(cop_has(C_DIAL));
	CHECK_UINT(cdr_take(), 1);
	CHECK(dviews[0].in_progress);
	CHECK(!dviews[0].inbound);
	CHECK(cdr_stats(0, &s));
	CHECK_UINT(s.out_attempted, 1);
	CHECK_UINT(s.in_received, 0);

	CASE("the far end answering counts one answered outbound call");
	rec_cb.answered(0, 0);
	CHECK(cdr_stats(0, &s));
	CHECK_UINT(s.out_answered, 1);
	CHECK_UINT(s.in_answered, 0);

	CASE("the snapshot the leg layer asks for reaches the backend");
	hops_reset();
	inject_snapshot(0, 0);
	CHECK(hop_has(H_STRM_STATS));
	CHECK_UINT(hop_find(H_STRM_STATS)->val, 0);
	CHECK_UINT(cdr_take(), 1);
	CHECK(!strcmp(dviews[0].media_state, "pending"));

	CASE("the record closes with the leg and stays pending for its numbers");
	info = ended_of(false, true, false, "5");
	rec_cb.ended(0, 0, &info);
	CHECK_UINT(cdr_take(), 1);
	CHECK(!dviews[0].in_progress);
	CHECK(dviews[0].answered);
	CHECK(!strcmp(dviews[0].number, "5"));
	CHECK(!strcmp(dviews[0].media_state, "pending"));

	CASE("the completion fills the record with what the backend reported");
	job_complete(HAL_OK);
	CHECK_UINT(cdr_take(), 1);
	CHECK(!strcmp(dviews[0].media_state, "present"));
	CHECK(dviews[0].has_media);
	CHECK_UINT(dviews[0].packets_sent, 1000);
	CHECK_UINT(dviews[0].bytes_recv, 1004);

	CASE("the totals moved with it");
	CHECK(cdr_stats(0, &s));
	CHECK_UINT(s.packets_sent, 1000);
	CHECK_UINT(s.bytes_recv, 1004);

	CASE("a second call carries the numbers of ITS snapshot, not the first ones");
	stats_seed(&fake_stats, 2000);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	inject_snapshot(0, 0);
	rec_cb.ended(0, 0, &info);
	job_complete(HAL_OK);
	n = cdr_take();
	CHECK_UINT(n, 2);
	CHECK_UINT(dviews[1].packets_sent, 2000);
	CHECK_UINT(dviews[1].bytes_recv, 2004);
	CHECK_UINT(dviews[0].packets_sent, 1000);

	CASE("the totals of the line are the sum of both snapshots");
	CHECK(cdr_stats(0, &s));
	CHECK_UINT(s.packets_sent, 3000);
	CHECK_UINT(s.bytes_recv, 3008);

	/*
	 * The busy slot. Two calls on one stream, the second torn down while
	 * the query of the first is still out. The stale answer must not be
	 * credited to the second call, so the first record is closed as failed
	 * and the job slot is reused.
	 */
	CASE("a second snapshot on one stream takes the pending job slot");
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	inject_snapshot(0, 0);
	info = ended_of(false, true, false, "5");
	rec_cb.ended(0, 0, &info);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	stats_seed(&fake_stats, 3000);
	inject_snapshot(0, 0);
	rec_cb.ended(0, 0, &info);
	n = cdr_take();
	CHECK_UINT(n, 2);
	CHECK(!strcmp(dviews[0].media_state, "failed"));
	CHECK(!strcmp(dviews[1].media_state, "pending"));
	CHECK_UINT(stats_starts, 2);

	CASE("the completion that follows belongs to the second call alone");
	job_complete(HAL_OK);
	n = cdr_take();
	CHECK_UINT(n, 2);
	CHECK(!strcmp(dviews[0].media_state, "failed"));
	CHECK(!dviews[0].has_media);
	CHECK(!strcmp(dviews[1].media_state, "present"));
	CHECK_UINT(dviews[1].packets_sent, 3000);

	CASE("the failed record moved no total");
	CHECK(cdr_stats(0, &s));
	CHECK_UINT(s.packets_sent, 3000);

	CASE("a snapshot with the dsp down leaves the record with no media");
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	hops_reset();
	uv.hal_state = UVOICE_HAL_STARTING;
	inject_snapshot(0, 0);
	uv.hal_state = UVOICE_HAL_READY;
	CHECK(!hop_has(H_STRM_STATS));
	rec_cb.ended(0, 0, &info);
	CHECK_UINT(cdr_take(), 1);
	CHECK(!strcmp(dviews[0].media_state, "absent"));
	CHECK(!dviews[0].has_media);

	CASE("a backend that refuses the query marks the record failed");
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	stats_start_status = HAL_FATAL;
	inject_snapshot(0, 0);
	stats_start_status = HAL_OK;
	rec_cb.ended(0, 0, &info);
	CHECK_UINT(cdr_take(), 1);
	CHECK(!strcmp(dviews[0].media_state, "failed"));

	CASE("a completion that reports a failure marks the record failed");
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	inject_snapshot(0, 0);
	rec_cb.ended(0, 0, &info);
	job_complete(HAL_TRANSIENT);
	CHECK_UINT(cdr_take(), 1);
	CHECK(!strcmp(dviews[0].media_state, "failed"));

	CASE("numbers the backend cannot hand back mark the record failed");
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	inject_snapshot(0, 0);
	rec_cb.ended(0, 0, &info);
	stats_result_status = HAL_FATAL;
	job_complete(HAL_OK);
	stats_result_status = HAL_OK;
	CHECK_UINT(cdr_take(), 1);
	CHECK(!strcmp(dviews[0].media_state, "failed"));

	CASE("an inbound call is counted when the offer arrives");
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "5551000", "Alice", 0);
	CHECK(cdr_stats(0, &s));
	CHECK_UINT(s.in_received, 1);
	CHECK_UINT(s.out_attempted, 0);
	CHECK_UINT(cdr_take(), 1);
	CHECK(dviews[0].inbound);
	CHECK(dviews[0].in_progress);

	CASE("answering it counts an inbound answer, not an outbound one");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(cop_has(C_ANSWER));
	CHECK(cdr_stats(0, &s));
	CHECK_UINT(s.in_answered, 1);
	CHECK_UINT(s.out_answered, 0);

	CASE("a failure at the end counts against the inbound side");
	info = ended_of(true, true, true, "");
	rec_cb.ended(0, 0, &info);
	CHECK(cdr_stats(0, &s));
	CHECK_UINT(s.in_failed, 1);
	CHECK_UINT(s.out_failed, 0);

	CASE("a reload keeps the counters and the ring");
	lines_reload(&cfg);
	CHECK(cdr_stats(0, &s));
	CHECK_UINT(s.in_received, 1);
	CHECK_UINT(s.in_answered, 1);
	CHECK_UINT(s.in_failed, 1);
	CHECK_UINT(cdr_take(), 1);
	CHECK_UINT(dviews[0].seq, 1);

	/*
	 * The second stream, which every case above leaves alone: a completion
	 * that read stream 0 for a snapshot of stream 1 would fill the record
	 * with the numbers of another call. The fake adds the stream index to
	 * two counters, so the record says which stream answered.
	 */
	CASE("the snapshot of the second stream carries that stream's numbers");
	fixture_start();
	region_ready();
	inject_incoming(0, 1, "5551000", "", 0);
	stats_seed(&fake_stats, 5000);
	inject_snapshot(0, 1);
	CHECK(hop_has(H_STRM_STATS));
	CHECK_UINT(hop_last(H_STRM_STATS)->val, 1);
	info = ended_of(true, false, false, "");
	rec_cb.ended(0, 1, &info);
	job_complete(HAL_OK);
	n = cdr_take();
	CHECK_UINT(n, 1);
	CHECK_UINT(dviews[0].slot, 1);
	CHECK(!strcmp(dviews[0].media_state, "present"));
	CHECK_UINT(dviews[0].packets_sent, 5001);
	CHECK_UINT(dviews[0].bytes_recv, 5005);

	CASE("the totals of the line took those numbers, not the stream 0 ones");
	CHECK(cdr_stats(0, &s));
	CHECK_UINT(s.packets_sent, 5001);
	CHECK_UINT(s.bytes_recv, 5005);

	/*
	 * "Here and nowhere else" is the function's own rule, not a property of
	 * having one caller: the early half of the bring-up empties the ring
	 * only when the lines are down, so a second one over live lines cannot
	 * drop a record or a counter whoever calls it. lines_done() is what puts
	 * the ring back to empty, which is the path every fixture takes.
	 */
	CASE("a second early half over live lines keeps the call history");
	n = cdr_take();
	CHECK_UINT(n, 1);
	lines_init(&cfg);
	CHECK_UINT(cdr_take(), n);
	CHECK(cdr_stats(0, &s));
	CHECK_UINT(s.packets_sent, 5001);

	CASE("and a bring-up behind lines_done starts the history empty again");
	fixture_start();
	CHECK_UINT(cdr_take(), 0);
	CHECK(cdr_stats(0, &s));
	CHECK_UINT(s.packets_sent, 0);
}

/* --------------------------------------------------- the live media query */

/*
 * The line/line.c half of "voice stats_live": the submission against a still
 * running stream, the answer handed straight to the consumer, the refusals
 * the shell makes before anything reaches the backend, and both directions of
 * the interplay with the record snapshot that shares the one job slot per
 * (line, stream). The ubus half, which is the deferred reply, its timeout and
 * the render, has no hook in this suite; the live smoke and the e2e flow
 * carry it.
 */
static unsigned int live_answers;
static unsigned int live_failures;
static struct hal_media_stats live_last;
static void *live_priv_seen;

static void live_take(void *priv, const struct hal_media_stats *stats)
{
	live_priv_seen = priv;

	if (!stats) {
		live_failures++;
		return;
	}

	live_answers++;
	live_last = *stats;
}

static void live_reset(void)
{
	live_answers = 0;
	live_failures = 0;
	live_priv_seen = NULL;
	memset(&live_last, 0, sizeof(live_last));
}

/* the running stream a live query needs, started through the media vtable the
 * leg layer drives: that op is where the shell learns the stream is up */
static void media_run(unsigned int line, unsigned int stream)
{
	rec_media.start(rec_media.priv, line, stream);
}

/*
 * The snapshot request carries the jitter buffer counters, and the shell
 * reads them before the slow media query can refuse. The two ops are
 * independent: a DSP that cannot answer the media query still has a buffer to
 * report. A record that lost the buffer because of that refusal would lose a
 * fact that nothing can produce again, because the leg is gone.
 */
static void t_cdr_jb(void)
{
	struct call_ended info;
	unsigned int n;

	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();
	jb_seed(&fake_jb, 700);
	jb_stats_status = HAL_OK;
	stats_start_status = HAL_OK;

	/* The ring is not cleared between cases of this suite, so every check
	 * below reads the newest record rather than a fixed index. */
	CASE("the snapshot reads the jitter buffer counters into the record");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	jb_stats_reads = 0;
	inject_snapshot(0, 0);
	CHECK_UINT(jb_stats_reads, 1);
	n = cdr_take();
	CHECK(n > 0);
	if (n) {
		CHECK(dviews[n - 1].has_jb);
		CHECK_UINT(dviews[n - 1].jb_frames_out, 700);
		CHECK_UINT(dviews[n - 1].jb_pool_free, 60);
	}
	info = ended_of(false, true, false, "5");
	rec_cb.ended(0, 0, &info);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	/*
	 * The ordering case. The media query is refused, so the record can
	 * never carry media numbers; the buffer answered, so it must carry
	 * those. A read placed behind the refusal return would lose them, and
	 * the leg is gone by then, so nothing can produce them again.
	 */
	CASE("a refused media query still leaves the buffer counters behind");
	stats_start_status = HAL_TRANSIENT;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	jb_stats_reads = 0;
	inject_snapshot(0, 0);
	CHECK_UINT(jb_stats_reads, 1);
	n = cdr_take();
	CHECK(n > 0);
	if (n) {
		CHECK(!strcmp(dviews[n - 1].media_state, "failed"));
		CHECK(!dviews[n - 1].has_media);
		CHECK(dviews[n - 1].has_jb);
		CHECK_UINT(dviews[n - 1].jb_frames_out, 700);
	}
	stats_start_status = HAL_OK;
	info = ended_of(false, true, false, "5");
	rec_cb.ended(0, 0, &info);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("a backend that answers no counters leaves the record without them");
	jb_stats_status = HAL_UNSUPPORTED;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	inject_snapshot(0, 0);
	n = cdr_take();
	CHECK(n > 0);
	if (n)
		CHECK(!dviews[n - 1].has_jb);
	jb_stats_status = HAL_OK;
	info = ended_of(false, true, false, "5");
	rec_cb.ended(0, 0, &info);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
}

/*
 * The same capture feeds the poll cache, which lets "voice jb" still show a
 * stream after its call ended. The teardown is the only writer that runs
 * without a caller. Without it, a call that nobody polled during the call
 * would leave no numbers at all, and the leg is gone by then.
 */
static void t_jb_cache(void)
{
	struct call_ended info;
	uint32_t before, after;

	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();
	jb_seed(&fake_jb, 900);
	jb_stats_status = HAL_OK;
	lec_seed(&fake_lec, 300);
	lec_stats_status = HAL_OK;
	stats_start_status = HAL_OK;
	memset(&uv.jb_last, 0, sizeof(uv.jb_last));
	memset(&uv.lec_last, 0, sizeof(uv.lec_last));

	CASE("no capture has run, so the poll cache holds nothing");
	CHECK(!uv.jb_last[0][0].valid);
	CHECK(!uv.jb_last[0][1].valid);
	CHECK(!uv.lec_last[0].valid);

	CASE("the teardown capture fills the poll cache of that stream alone");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	before = mono_ms_ref();
	inject_snapshot(0, 0);
	after = mono_ms_ref();
	CHECK(uv.jb_last[0][0].valid);
	CHECK_UINT(uv.jb_last[0][0].jb.frames_out, 900);
	CHECK_UINT(uv.jb_last[0][0].jb.pool_free, 60);
	CHECK(!uv.jb_last[0][1].valid);
	CHECK(!uv.jb_last[1][1].valid);

	CASE("the same teardown fills the line canceller cache");
	CHECK(uv.lec_last[0].valid);
	CHECK_UINT(uv.lec_last[0].lec.tail_ms, 300);
	CHECK(uv.lec_last[0].lec.erle_mdb == -300);
	/* two appended fields, so a cache copy made field by field that
	 * stopped at an older tail of the record reads wrong */
	CHECK_UINT(uv.lec_last[0].lec.nlp_quiet_frames, 303);
	CHECK_UINT(uv.lec_last[0].lec.coef_peak_tap, 304);
	CHECK(!uv.lec_last[1].valid);
	CHECK(uv.lec_last[0].stamp_ms >= before);
	CHECK(uv.lec_last[0].stamp_ms <= after);
	CHECK(uv.lec_last[0].stamp_ms > 0);
	info = ended_of(false, true, false, "5");
	rec_cb.ended(0, 0, &info);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	/*
	 * The stamp is the monotonic instant of that capture. Zero would read
	 * as "at start-up" and a wall clock value would jump under an NTP
	 * step, so the bracket pins both: the stamp lies between two direct
	 * readings of CLOCK_MONOTONIC taken around the capture.
	 */
	CASE("the stamp is the monotonic instant the capture happened at");
	CHECK(uv.jb_last[0][0].stamp_ms >= before);
	CHECK(uv.jb_last[0][0].stamp_ms <= after);
	CHECK(uv.jb_last[0][0].stamp_ms > 0);

	/*
	 * A capture on a nonzero pair lands on that pair. Every other case of
	 * this unit drives (0, 0), which an implementation that always wrote
	 * row zero would pass; the fake answers frames_out plus the stream
	 * index, so a row filled from the wrong pair carries the wrong number
	 * as well as sitting in the wrong slot.
	 */
	CASE("a capture on a nonzero pair fills that pair and no other");
	inject_snapshot(1, 1);
	CHECK(uv.jb_last[1][1].valid);
	CHECK_UINT(uv.jb_last[1][1].jb.frames_out, 901);
	CHECK(uv.jb_last[0][0].valid);
	CHECK_UINT(uv.jb_last[0][0].jb.frames_out, 900);
	CHECK(!uv.jb_last[0][1].valid);
	CHECK(!uv.jb_last[1][0].valid);

	/*
	 * A refusal leaves the cache alone. The stream may have gone quiet a
	 * tick before the teardown arrived, and the reading taken while it was
	 * running is still the best answer anyone has; invalidating it would
	 * throw away the only numbers that call ever produced.
	 */
	CASE("a refused capture leaves the previous reading in place");
	jb_stats_status = HAL_UNSUPPORTED;
	lec_stats_status = HAL_UNSUPPORTED;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	inject_snapshot(0, 0);
	CHECK(uv.jb_last[0][0].valid);
	CHECK_UINT(uv.jb_last[0][0].jb.frames_out, 900);
	CHECK(uv.lec_last[0].valid);
	CHECK_UINT(uv.lec_last[0].lec.tail_ms, 300);
	jb_stats_status = HAL_OK;
	lec_stats_status = HAL_OK;
	info = ended_of(false, true, false, "5");
	rec_cb.ended(0, 0, &info);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("a later capture replaces the reading with the newer one");
	jb_seed(&fake_jb, 1300);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	inject_snapshot(0, 0);
	CHECK(uv.jb_last[0][0].valid);
	CHECK_UINT(uv.jb_last[0][0].jb.frames_out, 1300);
	info = ended_of(false, true, false, "5");
	rec_cb.ended(0, 0, &info);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
}

static void t_stamp_clamp(void)
{
	/*
	 * A stamp whose 0 means "never" must never take a real instant of 0.
	 * uvoice_mono_ms() reads 0 for the first millisecond after boot and
	 * again at every 2^32 ms wrap, so the clamp maps 0 to 1 and leaves
	 * every other value alone. An assertion at normal uptime does not
	 * exercise the zero case; this does.
	 */
	CASE("the stamp clamp maps zero to one and keeps every other value");
	CHECK_UINT(uvoice_stamp_nonzero(0), 1);
	CHECK_UINT(uvoice_stamp_nonzero(1), 1);
	CHECK_UINT(uvoice_stamp_nonzero(5000), 5000);
	CHECK_UINT(uvoice_stamp_nonzero(0xFFFFFFFF), 0xFFFFFFFF);
}

static void t_stats_live(void)
{
	struct cdr_stats_view s;
	struct call_ended info;
	int consumer = 0;

	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();

	CASE("a live query on a running stream submits one job");
	media_run(0, 0);
	hops_reset();
	live_reset();
	CHECK_UINT(line_stats_live_query(0, 0, live_take, &consumer), LINE_STATS_OK);
	CHECK_UINT(hop_count(H_STRM_STATS), 1);
	CHECK_UINT(hop_last(H_STRM_STATS)->val, 0);

	CASE("and answers nobody until the completion lands");
	CHECK_UINT(live_answers, 0);
	CHECK_UINT(live_failures, 0);

	CASE("the completion hands the backend's numbers to the consumer");
	job_complete(HAL_OK);
	CHECK_UINT(live_answers, 1);
	CHECK_UINT(live_failures, 0);
	CHECK(live_priv_seen == &consumer);
	CHECK_UINT(live_last.packets_sent, 1000);
	CHECK_UINT(live_last.bytes_recv, 1004);

	/* the whole point of the split: a live query is a read. It opens no
	 * record, closes none and moves no total, so an operator polling it
	 * cannot change what the call history says */
	CASE("the live query left no record and moved no total");
	CHECK_UINT(cdr_take(), 0);
	CHECK(cdr_stats(0, &s));
	CHECK_UINT(s.packets_sent, 0);
	CHECK_UINT(s.bytes_recv, 0);

	CASE("a second live query while one is out refuses busy");
	live_reset();
	hops_reset();
	CHECK_UINT(line_stats_live_query(0, 0, live_take, &consumer), LINE_STATS_OK);
	CHECK_UINT(line_stats_live_query(0, 0, live_take, &consumer), LINE_STATS_BUSY);

	CASE("and the busy refusal made no query of its own");
	CHECK_UINT(hop_count(H_STRM_STATS), 1);

	CASE("the refused caller is never called back, the accepted one once");
	job_complete(HAL_OK);
	CHECK_UINT(live_answers, 1);
	CHECK_UINT(live_failures, 0);

	CASE("the pair takes a query again once the first one answered");
	live_reset();
	CHECK_UINT(line_stats_live_query(0, 0, live_take, &consumer), LINE_STATS_OK);
	job_complete(HAL_OK);
	CHECK_UINT(live_answers, 1);

	/* The second stream is its own pair: a query out on stream 0 must not
	 * make stream 1 look busy, because the job slot is per (line, stream) */
	CASE("a query on one stream leaves the other stream free");
	live_reset();
	hops_reset();
	media_run(0, 1);
	CHECK_UINT(line_stats_live_query(0, 0, live_take, &consumer), LINE_STATS_OK);
	CHECK_UINT(line_stats_live_query(0, 1, live_take, &consumer), LINE_STATS_OK);
	CHECK_UINT(hop_count(H_STRM_STATS), 2);
	CHECK_UINT(hop_last(H_STRM_STATS)->val, 1);

	/* the fake keeps one pending job, so this completion is the second
	 * submission's; the first is abandoned with the fixture below */
	CASE("and the answer carries the numbers of ITS stream");
	job_complete(HAL_OK);
	CHECK_UINT(live_answers, 1);
	CHECK_UINT(live_last.packets_sent, 1001);
	CHECK_UINT(live_last.bytes_recv, 1005);

	CASE("a slot with no running stream is refused with nothing asked");
	fixture_start();
	region_ready();
	live_reset();
	hops_reset();
	CHECK_UINT(line_stats_live_query(0, 0, live_take, &consumer),
		   LINE_STATS_NO_STREAM);
	CHECK_UINT(hop_count(H_STRM_STATS), 0);
	CHECK_UINT(live_answers, 0);
	CHECK_UINT(live_failures, 0);

	CASE("a stream that stopped refuses the next live query");
	media_run(0, 0);
	CHECK_UINT(line_stats_live_query(0, 0, live_take, &consumer), LINE_STATS_OK);
	job_complete(HAL_OK);
	rec_media.stop(rec_media.priv, 0, 0);
	CHECK_UINT(line_stats_live_query(0, 0, live_take, &consumer),
		   LINE_STATS_NO_STREAM);

	CASE("a media start with no HAL leaves the pair with no stream");
	fixture_start();
	region_ready();
	CHECK_UINT(rec_media.start(NULL, 0, 0), HAL_FATAL);
	CHECK_UINT(line_stats_live_query(0, 0, live_take, &consumer),
		   LINE_STATS_NO_STREAM);

	/*
	 * The case above never reaches the backend: the no-HAL guard answers it
	 * in front of the op, so it pins the guard and not the tracking. This
	 * one drives a real refusal, with the op called and the backend saying
	 * no, which is the only shape that pins "the pair is running only when
	 * the backend accepted the start".
	 */
	CASE("a start the backend refused leaves the pair with no stream");
	hops_reset();
	strm_start_status = HAL_FATAL;
	CHECK_UINT(rec_media.start(rec_media.priv, 0, 0), HAL_FATAL);
	strm_start_status = HAL_OK;
	CHECK_UINT(hop_count(H_STRM_START), 1);
	CHECK_UINT(line_stats_live_query(0, 0, live_take, &consumer),
		   LINE_STATS_NO_STREAM);

	CASE("and a start the backend accepts makes the same pair answerable");
	live_reset();
	CHECK_UINT(rec_media.start(rec_media.priv, 0, 0), HAL_OK);
	CHECK_UINT(line_stats_live_query(0, 0, live_take, &consumer), LINE_STATS_OK);
	job_complete(HAL_OK);
	CHECK_UINT(live_answers, 1);

	CASE("a port this board does not carry is a bad argument");
	CHECK_UINT(line_stats_live_query(CFG_CHANNEL_MAX, 0, live_take, &consumer),
		   LINE_STATS_NO_LINE);

	/* the query is keyed by stream; ubus.c validates the slot and the
	 * protocol names its stream (ops->stream_of_slot) */
	CASE("a stream past the HAL width has no stream");
	CHECK_UINT(line_stats_live_query(0, HAL_STREAM_MAX, live_take, &consumer),
		   LINE_STATS_NO_STREAM);

	CASE("a live query before the lines are up is refused");
	lines_stop();
	CHECK_UINT(line_stats_live_query(0, 0, live_take, &consumer),
		   LINE_STATS_NO_LINES);

	CASE("a backend that refuses the query says so in the same frame");
	fixture_start();
	region_ready();
	media_run(0, 0);
	live_reset();
	stats_start_status = HAL_FATAL;
	CHECK_UINT(line_stats_live_query(0, 0, live_take, &consumer),
		   LINE_STATS_REFUSED);
	stats_start_status = HAL_OK;
	CHECK_UINT(live_answers, 0);
	CHECK_UINT(live_failures, 0);

	CASE("and the refused pair is not left busy");
	CHECK_UINT(line_stats_live_query(0, 0, live_take, &consumer), LINE_STATS_OK);
	job_complete(HAL_OK);
	CHECK_UINT(live_answers, 1);

	CASE("a dsp that is not ready refuses the query");
	uv.hal_state = UVOICE_HAL_STARTING;
	CHECK_UINT(line_stats_live_query(0, 0, live_take, &consumer),
		   LINE_STATS_REFUSED);
	uv.hal_state = UVOICE_HAL_READY;

	CASE("a completion that reports a failure tells the consumer so");
	live_reset();
	CHECK_UINT(line_stats_live_query(0, 0, live_take, &consumer), LINE_STATS_OK);
	job_complete(HAL_TRANSIENT);
	CHECK_UINT(live_answers, 0);
	CHECK_UINT(live_failures, 1);

	CASE("numbers the backend cannot hand back are a failure too");
	live_reset();
	CHECK_UINT(line_stats_live_query(0, 0, live_take, &consumer), LINE_STATS_OK);
	stats_result_status = HAL_FATAL;
	job_complete(HAL_OK);
	stats_result_status = HAL_OK;
	CHECK_UINT(live_answers, 0);
	CHECK_UINT(live_failures, 1);

	/*
	 * The direction the record wins. A call that ends while a live query
	 * is out submits its record snapshot on the same pair, and the snapshot
	 * takes the slot: the record keeps a fact nothing can produce again,
	 * and the live query is answered with its failure rather than left for
	 * its own timeout.
	 */
	CASE("a call ending mid query takes the pair and fails the live query");
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	media_run(0, 0);
	live_reset();
	hops_reset();
	CHECK_UINT(line_stats_live_query(0, 0, live_take, &consumer), LINE_STATS_OK);
	inject_snapshot(0, 0);
	CHECK_UINT(live_failures, 1);
	CHECK_UINT(live_answers, 0);
	CHECK_UINT(hop_count(H_STRM_STATS), 2);

	CASE("and the completion behind it belongs to the record alone");
	info = ended_of(false, true, false, "5");
	rec_cb.ended(0, 0, &info);
	job_complete(HAL_OK);
	CHECK_UINT(cdr_take(), 1);
	CHECK(!strcmp(dviews[0].media_state, "present"));
	CHECK_UINT(dviews[0].packets_sent, 1000);
	CHECK_UINT(live_answers, 0);
	CHECK_UINT(live_failures, 1);

	/*
	 * The direction the live query loses. The snapshot of a call that is
	 * ending holds the pair until its answer lands, and a live query that
	 * arrives inside that window refuses busy instead of taking it: a
	 * takeover here would cost the record its media for a reading an
	 * operator can simply ask for again.
	 */
	CASE("a live query refuses busy while a record snapshot holds the pair");
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	media_run(0, 0);
	live_reset();
	hops_reset();
	inject_snapshot(0, 0);
	CHECK_UINT(line_stats_live_query(0, 0, live_take, &consumer), LINE_STATS_BUSY);
	CHECK_UINT(hop_count(H_STRM_STATS), 1);

	CASE("and the record kept its snapshot through the refusal");
	CHECK_UINT(cdr_take(), 1);
	CHECK(!strcmp(dviews[0].media_state, "pending"));
	job_complete(HAL_OK);
	CHECK_UINT(cdr_take(), 1);
	CHECK(!strcmp(dviews[0].media_state, "present"));
	CHECK_UINT(dviews[0].packets_sent, 1000);

	/*
	 * The consumer walking away. The ubus timeout drops
	 * the query it can no longer answer; the job leaves the pending list, so
	 * a completion that lands afterwards is unknown to the core and the
	 * consumer hears nothing at all.
	 */
	CASE("an abandoned live query answers nobody");
	fixture_start();
	region_ready();
	media_run(0, 0);
	live_reset();
	CHECK_UINT(line_stats_live_query(0, 0, live_take, &consumer), LINE_STATS_OK);
	line_stats_live_drop(0, 0);
	CHECK_UINT(live_answers, 0);
	CHECK_UINT(live_failures, 0);

	/*
	 * The job leaves the pending list, which is the hal.h stale-handle
	 * rule: a completion that lands afterwards is unknown to the core and
	 * main.c discards it. The absence of the job is the observable, because
	 * a completion the shell still dispatched would also answer nobody,
	 * having had its consumer cleared.
	 */
	CASE("and the job it held left the pending list with it");
	CHECK(job_pending == NULL);
	job_complete(HAL_OK);
	CHECK_UINT(live_answers, 0);
	CHECK_UINT(live_failures, 0);

	CASE("and the pair it held takes a query again");
	CHECK_UINT(line_stats_live_query(0, 0, live_take, &consumer), LINE_STATS_OK);
	job_complete(HAL_OK);
	CHECK_UINT(live_answers, 1);

	CASE("a drop leaves a record snapshot alone");
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	inject_snapshot(0, 0);
	line_stats_live_drop(0, 0);
	info = ended_of(false, true, false, "5");
	rec_cb.ended(0, 0, &info);
	job_complete(HAL_OK);
	CHECK_UINT(cdr_take(), 1);
	CHECK(!strcmp(dviews[0].media_state, "present"));
	CHECK_UINT(dviews[0].packets_sent, 1000);

	CASE("a drop on a pair with nothing out is harmless");
	line_stats_live_drop(0, 0);
	line_stats_live_drop(CFG_CHANNEL_MAX, 0);
	line_stats_live_drop(0, CALL_SLOTS);
	CHECK_UINT(cdr_take(), 1);
}

struct line_test_answer {
	enum line_test_result_status status;
	uint8_t record[LINE_TEST_RECORD_MAX];
	size_t len;
	void *priv;
	unsigned int calls;
};

static void line_test_answer_take(void *priv,
				  enum line_test_result_status status,
				  const void *record, size_t len)
{
	struct line_test_answer *answer = priv;
	size_t copy_len = len < sizeof(answer->record) ?
		len : sizeof(answer->record);

	answer->status = status;
	answer->len = len;
	answer->priv = priv;
	answer->calls++;
	if (record)
		memcpy(answer->record, record, copy_len);
}

static void t_line_test(void)
{
	struct line_test_answer answer = {};
	int64_t remaining_ms;

	cfg_default();
	fixture_start();

	CASE("a region job keeps the default budget");
	CHECK(job_pending != NULL);
	remaining_ms = uloop_timeout_remaining64(&job_pending->timeout);
	CHECK(remaining_ms > 0);
	CHECK(remaining_ms <= HAL_JOB_TIMEOUT_MS);
	region_ready();

	CASE("a line test rejects an index outside the line array");
	CHECK_UINT(line_test_query(CFG_CHANNEL_MAX, HAL_LINE_TEST_GR909,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_BAD_ARGUMENT);

	CASE("a line absent from the configuration rejects a line test");
	cfg.channel[0].present = false;
	CHECK_UINT(line_test_query(0, HAL_LINE_TEST_GR909,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_NO_LINE);
	cfg.channel[0].present = true;

	CASE("a line test rejects a line before the line shell is up");
	lines_stop();
	CHECK_UINT(line_test_query(0, HAL_LINE_TEST_GR909,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_NOT_UP);

	CASE("an out of service line can start a line test");
	cfg_default();
	fixture_start();
	region_ready();
	CHECK(state_is(1, "OOS"));
	CHECK_UINT(line_test_query(1, HAL_LINE_TEST_ROH,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_OK);

	CASE("a line test job carries its own budget above the default");
	CHECK(job_pending != NULL);
	remaining_ms = uloop_timeout_remaining64(&job_pending->timeout);
	CHECK(remaining_ms > HAL_JOB_TIMEOUT_MS);
	line_test_drop(1);
	hops_reset();

	CASE("a pending line test rejects another query on its slot");
	CHECK_UINT(line_test_query(0, HAL_LINE_TEST_GR909,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_OK);
	CHECK_UINT(line_test_query(0, HAL_LINE_TEST_ROH,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_PENDING);
	CHECK_UINT(hop_count(H_LINE_TEST), 1);
	line_test_drop(0);

	CASE("an off hook dial state rejects a line test on that line");
	cfg_default();
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK_UINT(line_test_query(0, HAL_LINE_TEST_GR909,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_CALL_ACTIVE);

	/*
	 * `line_test_state_safe()` admits FSM_SILENT beside FSM_IDLE, FSM_OOS
	 * and FSM_ROH, and `line_test_call_active()` groups it with CLEARING
	 * and ROH: a call only when a leg is held. A driven SILENT case is
	 * omitted on purpose: reaching FSM_SILENT needs the fixed 40 s reorder
	 * plus the howler window, which is over a minute of real-time uloop
	 * pumping. The FSM transition into FSM_SILENT is covered by test_fsm,
	 * and the held CLEARING case below drives the same switch branch.
	 */

	CASE("a call on another line rejects a line test");
	cfg_default();
	cfg.channel[1].enabled = true;
	fixture_start();
	region_ready();
	inject_incoming(1, 0, "1001", "caller", 0);
	CHECK(state_is(1, "RINGING"));
	CHECK_UINT(line_test_query(0, HAL_LINE_TEST_GR909,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_CALL_ACTIVE);

	/* the clearing chain that HOLD_DIAL enters keeps the held call, so a
	 * line there owns a call while a leg is held */
	CASE("a held call in the clearing chain rejects a line test on another line");
	cfg_default();
	cfg.channel[1].enabled = true;
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	inject_hook(0, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(0, 0, true);
	inject_digit(0, '5', false);
	rec_cb.failed(0, 1, CALL_FAIL_BUSY);
	CHECK(state_is(0, "CLEARING"));
	CHECK(state_is(1, "IDLE"));
	CHECK_UINT(line_test_query(1, HAL_LINE_TEST_GR909,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_CALL_ACTIVE);

	CASE("an unsupported backend rejects a line test as unsupported");
	cfg_default();
	fixture_start();
	region_ready();
	line_test_start_status = HAL_UNSUPPORTED;
	CHECK_UINT(line_test_query(0, HAL_LINE_TEST_GR909,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_UNSUPPORTED);

	CASE("a busy backend rejects a line test as busy");
	line_test_start_status = HAL_TRANSIENT;
	CHECK_UINT(line_test_query(0, HAL_LINE_TEST_GR909,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_BUSY);

	CASE("a fatal backend refusal rejects the line test");
	line_test_start_status = HAL_FATAL;
	CHECK_UINT(line_test_query(0, HAL_LINE_TEST_GR909,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_REFUSED);

	CASE("a line test completion hands the record to its consumer");
	line_test_start_status = HAL_OK;
	line_test_record_len = 6;
	memcpy(line_test_record, "record", line_test_record_len);
	memset(&answer, 0, sizeof(answer));
	CHECK_UINT(line_test_query(0, HAL_LINE_TEST_LOOP,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_OK);
	job_complete(HAL_OK);
	CHECK_UINT(answer.calls, 1);
	CHECK_UINT(answer.status, LINE_TEST_RESULT_COMPLETE);
	CHECK_UINT(answer.len, line_test_record_len);
	CHECK(answer.priv == &answer);
	CHECK(!memcmp(answer.record, line_test_record, line_test_record_len));

	CASE("the line test job timeout drops the pending slot");
	memset(&answer, 0, sizeof(answer));
	CHECK_UINT(line_test_query(0, HAL_LINE_TEST_ROH,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_OK);
	job_timeout();
	CHECK_UINT(answer.calls, 1);
	CHECK_UINT(answer.status, LINE_TEST_RESULT_TIMEOUT);
	CHECK_UINT(line_test_query(0, HAL_LINE_TEST_ROH,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_OK);
	line_test_drop(0);

	CASE("lines_done abandons a pending line test without a callback");
	memset(&answer, 0, sizeof(answer));
	CHECK_UINT(line_test_query(0, HAL_LINE_TEST_GR909,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_OK);
	lines_done();
	lines_live = false;
	CHECK(job_pending == NULL);
	CHECK_UINT(answer.calls, 0);
}

/* ------------------------------------------------------------ the held bell */

/*
 * A ring the backend refuses while a metallic line test runs. libudsp answers
 * HAL_TRANSIENT to a ring on any line for the length of the measurement. The
 * shell holds the bell and rings it when the test completes, and the caller
 * hears ringback until then.
 *
 * The pump behind the retry window is four times the 50 ms of HAL_RETRY_MS,
 * which is far more than the scheduler noise of a loaded builder. The pump
 * behind the hold cadence covers LINE_RING_HOLD_RETRY_MS with the same
 * margin, and it is the window a case waits out to prove that a hold armed no
 * timer.
 */
#define RING_RETRY_PUMP_MS	200
#define RING_HOLD_PUMP_MS	700

/*
 * The ring-no-answer window of the case that pins its start. Two seconds is
 * the shortest window that leaves half a second of margin either side of both
 * deadlines: the one the offer would have set, and the one the first bell
 * really sets a second later.
 */
#define RING_HOLD_CFNR_S	2
#define RING_HOLD_DELAY_MS	1000

/*
 * The hold of the case that runs the other way round, one second longer than
 * the window itself. A window the offer started would have expired and
 * forwarded the call while the telephone was still silent.
 */
#define RING_HOLD_LONG_MS	3000

static void t_ring_hold(void)
{
	struct line_test_answer answer = {};
	struct line_view v;
	struct hop *r;

	CASE("a ring refused during a line test is held and starts when the "
	     "test completes");
	cfg_default();
	cfg.channel[1].enabled = true;
	fixture_start();
	region_ready();
	CHECK_UINT(line_test_query(0, HAL_LINE_TEST_GR909,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_OK);
	hops_reset();
	ring_start_status = HAL_TRANSIENT;
	ring_start_fail_n = 1;
	inject_incoming(1, 0, "5551000", "Alice", 0);
	CHECK(state_is(1, "RINGING"));
	CHECK_UINT(ring_ok_count(), 0);
	view_get(1, &v);
	CHECK(v.ringing);
	CHECK(v.ring_held);
	/* a hold waits for the test and arms no timer of its own, so neither
	 * cadence may put a second attempt on the line before the completion */
	uloop_pump(RING_HOLD_PUMP_MS);
	CHECK_UINT(hop_count(H_RING_START), 1);
	job_complete(HAL_OK);
	CHECK_UINT(ring_ok_count(), 1);
	r = hop_last(H_RING_START);
	CHECK_UINT(r->line, 1);
	CHECK(r->ring.cid_present);
	CHECK(strcmp(r->ring.cid.number, "5551000") == 0);
	CHECK(strcmp(r->ring.cid.name, "Alice") == 0);
	view_get(1, &v);
	CHECK(v.ringing);
	CHECK(!v.ring_held);

	CASE("a held ring is dropped when the call ends before the test "
	     "completes");
	cfg_default();
	cfg.channel[1].enabled = true;
	fixture_start();
	region_ready();
	CHECK_UINT(line_test_query(0, HAL_LINE_TEST_GR909,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_OK);
	hops_reset();
	ring_start_status = HAL_TRANSIENT;
	ring_start_fail_n = 1;
	inject_incoming(1, 0, "5551000", "Alice", 0);
	CHECK(state_is(1, "RINGING"));
	CHECK_UINT(ring_ok_count(), 0);
	rec_cb.remote_cancel(1, 0);
	CHECK(state_is(1, "IDLE"));
	job_complete(HAL_OK);
	CHECK_UINT(ring_ok_count(), 0);
	view_get(1, &v);
	CHECK(!v.ringing);
	CHECK(!v.ring_held);

	CASE("a held ring on the tested line starts after its own test");
	cfg_default();
	fixture_start();
	region_ready();
	CHECK_UINT(line_test_query(0, HAL_LINE_TEST_GR909,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_OK);
	hops_reset();
	ring_start_status = HAL_TRANSIENT;
	ring_start_fail_n = 1;
	inject_incoming(0, 0, "5552000", "Bob", 0);
	CHECK(state_is(0, "RINGING"));
	CHECK_UINT(ring_ok_count(), 0);
	view_get(0, &v);
	CHECK(v.ring_held);
	job_complete(HAL_OK);
	CHECK_UINT(ring_ok_count(), 1);
	r = hop_last(H_RING_START);
	CHECK_UINT(r->line, 0);
	CHECK(strcmp(r->ring.cid.number, "5552000") == 0);
	view_get(0, &v);
	CHECK(v.ringing);
	CHECK(!v.ring_held);

	CASE("a transient ring with no test running is retried once");
	cfg_default();
	fixture_start();
	region_ready();
	ring_start_status = HAL_TRANSIENT;
	ring_start_fail_n = 1;
	inject_incoming(0, 0, "5551000", "Alice", 0);
	CHECK_UINT(hop_count(H_RING_START), 1);
	CHECK_UINT(ring_ok_count(), 0);
	view_get(0, &v);
	CHECK(v.ringing);
	CHECK(!v.ring_held);
	uloop_pump(RING_RETRY_PUMP_MS);
	CHECK_UINT(hop_count(H_RING_START), 2);
	CHECK_UINT(ring_ok_count(), 1);
	CHECK(strcmp(hop_last(H_RING_START)->ring.cid.number, "5551000") == 0);

	CASE("a fatal ring refusal is logged and not retried");
	cfg_default();
	fixture_start();
	region_ready();
	ring_start_status = HAL_FATAL;
	ring_start_fail_n = 1;
	warn_capture_begin();
	inject_incoming(0, 0, "5551000", "Alice", 0);
	CHECK_UINT(warn_capture_count_at(LOG_ERR,
					 "line 0 ring not started: fatal"), 1);
	/* a call the subscriber loses is an error and not a warning, so the
	 * level is half of what this case pins */
	CHECK_UINT(warn_capture_count_at(LOG_WARNING,
					 "line 0 ring not started: fatal"), 0);
	warn_capture_end();
	CHECK_UINT(hop_count(H_RING_START), 1);
	CHECK_UINT(ring_ok_count(), 0);
	uloop_pump(RING_RETRY_PUMP_MS);
	CHECK_UINT(hop_count(H_RING_START), 1);
	/* the give-up behind the log refuses the caller, so the request ends
	 * with the call (t_ring_give_up below) */
	view_get(0, &v);
	CHECK(!v.ringing);
	CHECK(!v.ring_held);

	CASE("a held ring survives a line test that outlasts the core's "
	     "budget");
	cfg_default();
	cfg.channel[1].enabled = true;
	fixture_start();
	region_ready();
	CHECK_UINT(line_test_query(0, HAL_LINE_TEST_GR909,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_OK);
	hops_reset();
	ring_start_status = HAL_TRANSIENT;
	ring_start_fail_n = 2;
	inject_incoming(1, 0, "5551000", "Alice", 0);
	CHECK(state_is(1, "RINGING"));
	CHECK_UINT(ring_ok_count(), 0);
	/* the budget ran out while the plug-in still held the part, so the
	 * re-issue is refused too and the hold has to outlive that refusal */
	job_timeout();
	CHECK_UINT(hop_count(H_RING_START), 2);
	CHECK_UINT(ring_ok_count(), 0);
	view_get(1, &v);
	CHECK(v.ringing);
	CHECK(v.ring_held);
	uloop_pump(RING_HOLD_PUMP_MS);
	CHECK_UINT(ring_ok_count(), 1);
	CHECK(strcmp(hop_last(H_RING_START)->ring.cid.number, "5551000") == 0);
	view_get(1, &v);
	CHECK(v.ringing);
	CHECK(!v.ring_held);

	CASE("a held ring restarts the no answer window when the bell rings");
	cfg_default();
	cfg_dnd_cf_codes(&cfg.channel[1]);
	cfg.channel[1].enabled = true;
	cfg.channel[1].call_fwd_local_mode = 4;
	cfg.channel[1].call_fwd_no_answer_timeout = RING_HOLD_CFNR_S;
	snprintf(cfg.channel[1].call_fwd_no_answer_uri,
		 sizeof(cfg.channel[1].call_fwd_no_answer_uri), "5553333");
	fixture_start();
	region_ready();
	CHECK_UINT(line_test_query(0, HAL_LINE_TEST_GR909,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_OK);
	hops_reset();
	cops_reset();
	ring_start_status = HAL_TRANSIENT;
	ring_start_fail_n = 1;
	inject_incoming(1, 0, "5551000", "Alice", 0);
	CHECK(state_is(1, "RINGING"));
	uloop_pump(RING_HOLD_DELAY_MS);
	job_complete(HAL_OK);
	CHECK_UINT(ring_ok_count(), 1);
	/* half a second past the deadline the offer would have set */
	uloop_pump(RING_HOLD_CFNR_S * 1000 + 500 - RING_HOLD_DELAY_MS);
	CHECK(!cop_has(C_FORWARD));
	CHECK(state_is(1, "RINGING"));
	/* and half a second past the deadline the bell set */
	uloop_pump(RING_HOLD_DELAY_MS);
	CHECK(cop_has(C_FORWARD));
	CHECK(strcmp(cop_find(C_FORWARD)->num, "5553333") == 0);
	CHECK(state_is(1, "IDLE"));

	CASE("a hold longer than the no answer window still rings before it "
	     "forwards");
	cfg_default();
	cfg_dnd_cf_codes(&cfg.channel[1]);
	cfg.channel[1].enabled = true;
	cfg.channel[1].call_fwd_local_mode = 4;
	cfg.channel[1].call_fwd_no_answer_timeout = RING_HOLD_CFNR_S;
	snprintf(cfg.channel[1].call_fwd_no_answer_uri,
		 sizeof(cfg.channel[1].call_fwd_no_answer_uri), "5553333");
	fixture_start();
	region_ready();
	CHECK_UINT(line_test_query(0, HAL_LINE_TEST_GR909,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_OK);
	hops_reset();
	cops_reset();
	ring_start_status = HAL_TRANSIENT;
	ring_start_fail_n = 1;
	inject_incoming(1, 0, "5551000", "Alice", 0);
	CHECK(state_is(1, "RINGING"));
	/* a whole second past the window, which no bell has opened yet */
	uloop_pump(RING_HOLD_LONG_MS);
	CHECK_UINT(ring_ok_count(), 0);
	CHECK(!cop_has(C_FORWARD));
	CHECK(state_is(1, "RINGING"));
	job_complete(HAL_OK);
	CHECK_UINT(ring_ok_count(), 1);
	/* half a second short of the deadline that bell set */
	uloop_pump(RING_HOLD_CFNR_S * 1000 - 500);
	CHECK(!cop_has(C_FORWARD));
	CHECK(state_is(1, "RINGING"));
	/* and half a second past it */
	uloop_pump(RING_HOLD_DELAY_MS);
	CHECK(cop_has(C_FORWARD));
	CHECK(strcmp(cop_find(C_FORWARD)->num, "5553333") == 0);
	CHECK(state_is(1, "IDLE"));
}

/* ---------------------------------------------------- HOLDING recall ring */

static void recall_fixture(void)
{
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();
}

static void recall_two_calls_reach(void)
{
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	inject_hook(0, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(0, 0, true);
	CHECK(state_is(0, "TWO_CALLS"));
	hops_reset();
	cops_reset();
}

static void recall_hold_dial_reach(void)
{
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	inject_hook(0, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(0, 0, true);
	CHECK(state_is(0, "HOLD_DIAL"));
	hops_reset();
	cops_reset();
}

#define RECALL_CFNR_S		1
#define RECALL_CFNR_PUMP_MS	1500

static void recall_cfnr_cfg(void)
{
	cfg_dnd_cf_codes(&cfg.channel[0]);
	cfg.channel[0].call_fwd_local_mode = 4;
	cfg.channel[0].call_fwd_no_answer_timeout = RECALL_CFNR_S;
	snprintf(cfg.channel[0].call_fwd_no_answer_uri,
		 sizeof(cfg.channel[0].call_fwd_no_answer_uri), "5553333");
}

static void recall_deferred_reach(struct line_test_answer *answer)
{
	recall_fixture();
	CHECK_UINT(line_test_query(1, HAL_LINE_TEST_GR909,
				   line_test_answer_take, answer),
		   LINE_TEST_QUERY_OK);
	recall_two_calls_reach();
	ring_start_status = HAL_TRANSIENT;
	ring_start_fail_n = 1;
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "HOLDING"));
}

static void t_recall(void)
{
	struct line_test_answer answer = {};
	struct line_view v;
	struct hop *r;

	CASE("on hook from two calls rings the held call at once");
	cfg_default();
	recall_fixture();
	recall_two_calls_reach();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "HOLDING"));
	CHECK(cop_has(C_HANGUP));
	CHECK_UINT(hop_count(H_RING_STOP), 1);
	CHECK_UINT(hop_count(H_RING_START), 1);
	CHECK(hop_index(H_RING_STOP) < hop_index(H_RING_START));
	view_get(0, &v);
	CHECK(v.ringing);
	CHECK(!v.ring_held);

	CASE("the recall is the default cadence, unbounded, with no caller id");
	r = hop_find(H_RING_START);
	CHECK_UINT(r->line, 0);
	CHECK_UINT(r->ring.cadence_count, 1);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);
	CHECK_UINT(r->ring.cadence[0].off_ms, 4000);
	CHECK_UINT(r->ring.duration_ms, 0);
	CHECK(!r->ring.cid_present);
	CHECK(r->ring.cid.number[0] == 0);
	CHECK(r->ring.cid.name[0] == 0);

	CASE("on hook ends an outgoing second call and rings the first held call");
	cfg_default();
	recall_fixture();
	recall_hold_dial_reach();
	inject_digit(0, '5', false);
	CHECK(state_is(0, "CALLING"));
	rec_cb.answered(0, 1);
	CHECK(state_is(0, "TWO_CALLS"));
	hops_reset();
	cops_reset();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "HOLDING"));
	CHECK_UINT(cop_count(C_HANGUP), 1);
	CHECK_UINT(cop_find(C_HANGUP)->slot, 1);
	CHECK_UINT(hop_count(H_RING_START), 1);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);
	CHECK_UINT(r->ring.cadence[0].off_ms, 4000);
	CHECK_UINT(r->ring.duration_ms, 0);
	CHECK(!r->ring.cid_present);

	CASE("nothing restarts or stops the recall on its own");
	hops_reset();
	uloop_pump(RING_HOLD_PUMP_MS);
	CHECK_UINT(hop_count(H_RING_START), 0);
	CHECK_UINT(hop_count(H_RING_STOP), 0);
	CHECK(state_is(0, "HOLDING"));

	CASE("pickup stops the recall and retrieves the held call");
	hops_reset();
	cops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(hop_has(H_RING_STOP));
	CHECK(!hop_has(H_RING_START));
	CHECK(cop_has(C_RESUME));
	CHECK_UINT(cop_find(C_RESUME)->slot, 0);
	CHECK(state_is(0, "PENDING"));
	view_get(0, &v);
	CHECK(!v.ringing);

	CASE("on hook from the flash hold rings the held call at once");
	cfg_default();
	recall_fixture();
	recall_hold_dial_reach();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "HOLDING"));
	CHECK_UINT(hop_count(H_RING_STOP), 1);
	CHECK_UINT(hop_count(H_RING_START), 1);
	CHECK(hop_index(H_RING_STOP) < hop_index(H_RING_START));
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);
	CHECK_UINT(r->ring.duration_ms, 0);
	CHECK(!r->ring.cid_present);

	CASE("the held party clearing stops the recall");
	hops_reset();
	rec_cb.remote_bye(0, 0);
	CHECK(hop_has(H_RING_STOP));
	CHECK(!hop_has(H_RING_START));
	CHECK(state_is(0, "IDLE"));
	view_get(0, &v);
	CHECK(!v.ringing);

	CASE("the region's pattern 0 is the recall cadence");
	cfg_default();
	cfg_pattern_set(0, 700, 2100);
	recall_fixture();
	recall_hold_dial_reach();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence_count, 1);
	CHECK_UINT(r->ring.cadence[0].on_ms, 700);
	CHECK_UINT(r->ring.cadence[0].off_ms, 2100);
	CHECK(!r->ring.cid_present);

	CASE("the held caller's distinctive template does not ride the recall");
	cfg_default();
	cfg.channel[0].distinctive_ring_enable = true;
	cfg_ring_number(&cfg.channel[0], 0, "5552000", 2);
	cfg_pattern_set(2, 1200, 2400);
	recall_fixture();
	inject_incoming(0, 0, "5552000", "Bob", 2);
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 1200);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(state_is(0, "CONNECTED"));
	inject_hook(0, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(0, 0, true);
	CHECK(state_is(0, "HOLD_DIAL"));
	hops_reset();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "HOLDING"));
	r = hop_find(H_RING_START);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);
	CHECK_UINT(r->ring.cadence[0].off_ms, 4000);
	CHECK(!r->ring.cid_present);

	CASE("an accepted offer in HOLDING stops the recall before its own bell");
	cfg_default();
	cfg.channel[0].distinctive_ring_enable = true;
	cfg_pattern_set(2, 1200, 2400);
	recall_fixture();
	recall_two_calls_reach();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	hops_reset();
	inject_incoming(0, 1, "5553000", "Carol", 2);
	CHECK(state_is(0, "RINGING"));
	CHECK_UINT(hop_count(H_RING_STOP), 1);
	CHECK_UINT(hop_count(H_RING_START), 1);
	CHECK(hop_index(H_RING_STOP) < hop_index(H_RING_START));
	r = hop_find(H_RING_START);
	CHECK(r->ring.cid_present);
	CHECK(strcmp(r->ring.cid.number, "5553000") == 0);
	CHECK_UINT(r->ring.cadence[0].on_ms, 1200);
	CHECK_UINT(r->ring.cadence[0].off_ms, 2400);
	view_get(0, &v);
	CHECK(v.ringing);

	CASE("the caller giving up brings the recall back");
	hops_reset();
	rec_cb.remote_cancel(0, 1);
	CHECK(state_is(0, "HOLDING"));
	CHECK_UINT(hop_count(H_RING_START), 1);
	CHECK(hop_index(H_RING_STOP) < hop_index(H_RING_START));
	r = hop_find(H_RING_START);
	CHECK(!r->ring.cid_present);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);
	view_get(0, &v);
	CHECK(v.ringing);

	CASE("the no answer forward of the offer brings the recall back too");
	cfg_default();
	recall_cfnr_cfg();
	recall_fixture();
	recall_hold_dial_reach();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "HOLDING"));
	hops_reset();
	cops_reset();
	inject_incoming(0, 1, "5553000", "Carol", 0);
	CHECK(state_is(0, "RINGING"));
	uloop_pump(RECALL_CFNR_PUMP_MS);
	CHECK(cop_has(C_FORWARD));
	CHECK(strcmp(cop_find(C_FORWARD)->num, "5553333") == 0);
	CHECK(state_is(0, "HOLDING"));
	CHECK_UINT(hop_count(H_RING_START), 2);
	r = hop_last(H_RING_START);
	CHECK(!r->ring.cid_present);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);

	CASE("the recall opens no answer window of its own");
	cops_reset();
	uloop_pump(RECALL_CFNR_PUMP_MS);
	CHECK(!cop_has(C_FORWARD));
	CHECK(state_is(0, "HOLDING"));

	CASE("do not disturb refuses an offer and leaves the recall alone");
	cfg_default();
	cfg_dnd_cf_codes(&cfg.channel[0]);
	cfg.channel[0].do_not_disturb_enabled = true;
	recall_fixture();
	recall_hold_dial_reach();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "HOLDING"));
	hops_reset();
	cops_reset();
	inject_incoming(0, 1, "5553000", "Carol", 0);
	CHECK(cop_has(C_REJECT));
	CHECK(state_is(0, "HOLDING"));
	CHECK_UINT(nhops, 0);
	view_get(0, &v);
	CHECK(v.ringing);

	CASE("the unconditional forward leaves the recall alone too");
	cfg_default();
	cfg_dnd_cf_codes(&cfg.channel[0]);
	cfg.channel[0].call_fwd_local_mode = 1;
	snprintf(cfg.channel[0].call_fwd_uncond_uri,
		 sizeof(cfg.channel[0].call_fwd_uncond_uri), "5551111");
	recall_fixture();
	recall_hold_dial_reach();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "HOLDING"));
	hops_reset();
	cops_reset();
	inject_incoming(0, 1, "5553000", "Carol", 0);
	CHECK(cop_has(C_FORWARD));
	CHECK(strcmp(cop_find(C_FORWARD)->num, "5551111") == 0);
	CHECK(state_is(0, "HOLDING"));
	CHECK_UINT(nhops, 0);

	CASE("out of service stops the recall");
	cfg_default();
	recall_fixture();
	recall_hold_dial_reach();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	hops_reset();
	cfg.channel[0].enabled = false;
	lines_reload(&cfg);
	CHECK(state_is(0, "OOS"));
	CHECK(hop_has(H_RING_STOP));
	view_get(0, &v);
	CHECK(!v.ringing);

	CASE("a transient refusal of the recall is retried once");
	cfg_default();
	recall_fixture();
	recall_hold_dial_reach();
	ring_start_status = HAL_TRANSIENT;
	ring_start_fail_n = 1;
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "HOLDING"));
	CHECK_UINT(hop_count(H_RING_START), 1);
	CHECK_UINT(ring_ok_count(), 0);
	view_get(0, &v);
	CHECK(v.ringing);
	CHECK(!v.ring_held);
	uloop_pump(RING_RETRY_PUMP_MS);
	CHECK_UINT(hop_count(H_RING_START), 2);
	CHECK_UINT(ring_ok_count(), 1);
	r = hop_last(H_RING_START);
	CHECK(!r->ring.cid_present);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);

	CASE("out of service cancels the retry the recall still owed");
	cfg_default();
	recall_fixture();
	recall_hold_dial_reach();
	ring_start_status = HAL_TRANSIENT;
	ring_start_fail_n = 1;
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK_UINT(ring_ok_count(), 0);
	cfg.channel[0].enabled = false;
	lines_reload(&cfg);
	CHECK(state_is(0, "OOS"));
	hops_reset();
	uloop_pump(RING_RETRY_PUMP_MS);
	CHECK(!hop_has(H_RING_START));
	view_get(0, &v);
	CHECK(!v.ringing);

	CASE("a fatal refusal of the recall is logged and not retried");
	cfg_default();
	recall_fixture();
	recall_hold_dial_reach();
	ring_start_status = HAL_FATAL;
	ring_start_fail_n = 1;
	warn_capture_begin();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	/* the recall profile has no caller to refuse, and the message says so
	 * rather than naming a 480 that never leaves */
	CHECK_UINT(warn_capture_count_at(
			   LOG_ERR,
			   "line 0 ring not started: fatal, the recall stays "
			   "silent"),
		   1);
	warn_capture_end();
	CHECK(state_is(0, "HOLDING"));
	CHECK_UINT(hop_count(H_RING_START), 1);
	CHECK_UINT(ring_ok_count(), 0);
	uloop_pump(RING_RETRY_PUMP_MS);
	CHECK_UINT(hop_count(H_RING_START), 1);

	CASE("a line test defers the recall and rings it when the test completes");
	cfg_default();
	recall_cfnr_cfg();
	recall_deferred_reach(&answer);
	CHECK_UINT(hop_count(H_RING_START), 1);
	CHECK_UINT(ring_ok_count(), 0);
	view_get(0, &v);
	CHECK(v.ringing);
	CHECK(v.ring_held);
	uloop_pump(RING_HOLD_PUMP_MS);
	CHECK_UINT(hop_count(H_RING_START), 1);
	job_complete(HAL_OK);
	CHECK_UINT(ring_ok_count(), 1);
	r = hop_last(H_RING_START);
	CHECK_UINT(r->line, 0);
	CHECK(!r->ring.cid_present);
	CHECK_UINT(r->ring.cadence[0].on_ms, 2000);
	CHECK_UINT(r->ring.duration_ms, 0);
	CHECK(state_is(0, "HOLDING"));
	view_get(0, &v);
	CHECK(v.ringing);
	CHECK(!v.ring_held);

	CASE("the deferred recall opens no answer window either");
	cops_reset();
	uloop_pump(RECALL_CFNR_PUMP_MS);
	CHECK(!cop_has(C_FORWARD));
	CHECK(state_is(0, "HOLDING"));

	CASE("a held call that ends before the test completes rings nothing late");
	cfg_default();
	recall_deferred_reach(&answer);
	view_get(0, &v);
	CHECK(v.ring_held);
	rec_cb.remote_bye(0, 0);
	CHECK(state_is(0, "IDLE"));
	hops_reset();
	job_complete(HAL_OK);
	CHECK(!hop_has(H_RING_START));
	view_get(0, &v);
	CHECK(!v.ringing);
	CHECK(!v.ring_held);

	CASE("pickup cancels a recall deferred by a metallic test");
	cfg_default();
	recall_deferred_reach(&answer);
	view_get(0, &v);
	CHECK(v.ring_held);
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(state_is(0, "PENDING"));
	CHECK(hop_has(H_RING_STOP));
	CHECK(cop_has(C_RESUME));
	CHECK_UINT(cop_find(C_RESUME)->slot, 0);
	view_get(0, &v);
	CHECK(!v.ringing);
	CHECK(!v.ring_held);
	hops_reset();
	CHECK(job_pending != NULL);
	job_complete(HAL_OK);
	CHECK(!hop_has(H_RING_START));
	rec_cb.reinvite_done(0, 0, true);
	CHECK(state_is(0, "CONNECTED"));

	CASE("out of service cancels a recall deferred by a metallic test");
	cfg_default();
	recall_deferred_reach(&answer);
	view_get(0, &v);
	CHECK(v.ring_held);
	hops_reset();
	inject_fault(0, FAULT_THERMAL);
	CHECK(state_is(0, "OOS"));
	CHECK(hop_has(H_RING_STOP));
	view_get(0, &v);
	CHECK(!v.ringing);
	CHECK(!v.ring_held);
	hops_reset();
	CHECK(job_pending != NULL);
	job_complete(HAL_OK);
	CHECK(!hop_has(H_RING_START));
	CHECK(state_is(0, "OOS"));

	CASE("the ring status survives the backend's late end report of the "
	     "replaced recall");
	cfg_default();
	recall_fixture();
	recall_two_calls_reach();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "HOLDING"));
	hops_reset();
	inject_incoming(0, 1, "5553000", "Carol", 0);
	CHECK(state_is(0, "RINGING"));
	CHECK(hop_index(H_RING_STOP) < hop_index(H_RING_START));
	view_get(0, &v);
	CHECK(v.ringing);
	inject_event(0, HAL_EVENT_RING_END);
	view_get(0, &v);
	CHECK(v.ringing);
	inject_event(0, HAL_EVENT_RING_START);
	CHECK(state_is(0, "RINGING"));
	view_get(0, &v);
	CHECK(v.ringing);

	CASE("ring trip clears the request through the hook event");
	inject_event(0, HAL_EVENT_RING_END);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(state_is(0, "TWO_CALLS"));
	view_get(0, &v);
	CHECK(!v.ringing);

	CASE("a late ring start cannot restore a stopped request");
	inject_event(0, HAL_EVENT_RING_START);
	view_get(0, &v);
	CHECK(!v.ringing);

	CASE("queued indications cannot cancel a deferred replacement ring");
	cfg_default();
	recall_fixture();
	inject_incoming(0, 0, "5551000", "Alice", 0);
	CHECK(state_is(0, "RINGING"));
	rec_cb.remote_cancel(0, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(line_test_query(1, HAL_LINE_TEST_GR909,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_OK);
	hops_reset();
	ring_start_status = HAL_TRANSIENT;
	ring_start_fail_n = 1;
	inject_incoming(0, 0, "5553000", "Carol", 0);
	CHECK(state_is(0, "RINGING"));
	CHECK_UINT(ring_ok_count(), 0);
	inject_event(0, HAL_EVENT_RING_START);
	inject_event(0, HAL_EVENT_RING_END);
	view_get(0, &v);
	CHECK(v.ringing);
	CHECK(v.ring_held);
	job_complete(HAL_OK);
	CHECK_UINT(ring_ok_count(), 1);
	r = hop_last(H_RING_START);
	CHECK(r->ring.cid_present);
	CHECK(strcmp(r->ring.cid.number, "5553000") == 0);
	view_get(0, &v);
	CHECK(v.ringing);
	CHECK(!v.ring_held);
}

/* ------------------------------------------------------- the refused caller */

/*
 * A bell the backend gives up on. The 180 is already out, so the caller hears
 * ringback for a telephone that never rings. The shell refuses that caller 480
 * one uloop pass behind the refusal, because the bell op runs in front of the
 * RINGING entry and the machine is not alerting inside that frame.
 *
 * The group sits behind the recall one, because two of its cases follow a
 * refused recall ring and the first of the two calls the fixture helpers
 * above.
 *
 * The give-up timer is armed with 0 ms, so one turn of the loop applies it.
 * RING_RETRY_PUMP_MS is far longer than that, and four times HAL_RETRY_MS, so
 * a case that expects the ordinary retry first sees both.
 */

/* the log line of the give-up, which the last case counts at two levels */
#define RING_GIVE_UP_LOG \
	"line 0 ring not started: fatal, the caller gets 480"

static void t_ring_give_up(void)
{
	struct line_view v;

	CASE("a fatal ring refusal refuses the caller 480 and the line goes "
	     "idle");
	cfg_default();
	fixture_start();
	region_ready();
	ring_start_status = HAL_FATAL;
	ring_start_fail_n = 1;
	inject_incoming(0, 0, "5551000", "Alice", 0);
	CHECK(state_is(0, "RINGING"));
	CHECK(!cop_has(C_REJECT));
	uloop_pump(RING_RETRY_PUMP_MS);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(cop_count(C_REJECT), 1);
	CHECK_UINT(cop_find(C_REJECT)->slot, 0);
	CHECK_UINT(cop_find(C_REJECT)->val, 480);
	/* the busy forward is the one redirect reject_send() makes, and this is
	 * not the busy condition */
	CHECK(!cop_has(C_FORWARD));
	view_get(0, &v);
	CHECK(!v.ringing);
	CHECK(!v.ring_held);

	CASE("the give-up runs a pass later, so the ring start is recorded "
	     "before the stop");
	cfg_default();
	fixture_start();
	region_ready();
	ring_start_status = HAL_FATAL;
	ring_start_fail_n = 1;
	inject_incoming(0, 0, "5551000", "Alice", 0);
	CHECK_UINT(hop_count(H_RING_START), 1);
	CHECK(!hop_has(H_RING_STOP));
	CHECK(!cop_has(C_REJECT));
	uloop_pump(RING_RETRY_PUMP_MS);
	CHECK_UINT(hop_count(H_RING_STOP), 1);
	CHECK(hop_index(H_RING_START) < hop_index(H_RING_STOP));
	CHECK(cop_has(C_REJECT));
	CHECK(state_is(0, "IDLE"));

	CASE("a transient refusal with no test running refuses the caller "
	     "after one retry");
	cfg_default();
	fixture_start();
	region_ready();
	ring_start_status = HAL_TRANSIENT;
	ring_start_fail_n = 2;
	inject_incoming(0, 0, "5551000", "Alice", 0);
	CHECK_UINT(hop_count(H_RING_START), 1);
	CHECK(!cop_has(C_REJECT));
	uloop_pump(RING_RETRY_PUMP_MS);
	CHECK_UINT(hop_count(H_RING_START), 2);
	CHECK_UINT(ring_ok_count(), 0);
	CHECK_UINT(cop_count(C_REJECT), 1);
	CHECK_UINT(cop_find(C_REJECT)->val, 480);
	CHECK(state_is(0, "IDLE"));
	view_get(0, &v);
	CHECK(!v.ringing);

	CASE("a call that ended before the give-up refuses nobody");
	cfg_default();
	fixture_start();
	region_ready();
	ring_start_status = HAL_FATAL;
	ring_start_fail_n = 1;
	inject_incoming(0, 0, "5551000", "Alice", 0);
	CHECK(state_is(0, "RINGING"));
	rec_cb.remote_cancel(0, 0);
	CHECK(state_is(0, "IDLE"));
	cops_reset();
	uloop_pump(RING_RETRY_PUMP_MS);
	CHECK(!cop_has(C_REJECT));
	CHECK(state_is(0, "IDLE"));
	view_get(0, &v);
	CHECK(!v.ringing);

	CASE("a recall ring the backend refuses keeps the held call and "
	     "refuses nobody");
	cfg_default();
	recall_fixture();
	recall_hold_dial_reach();
	ring_start_status = HAL_FATAL;
	ring_start_fail_n = 1;
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "HOLDING"));
	CHECK_UINT(hop_count(H_RING_START), 1);
	CHECK_UINT(ring_ok_count(), 0);
	cops_reset();
	uloop_pump(RING_RETRY_PUMP_MS);
	CHECK(!cop_has(C_REJECT));
	CHECK(!cop_has(C_HANGUP));
	CHECK(state_is(0, "HOLDING"));
	view_get(0, &v);
	CHECK(v.ringing);

	CASE("after a refused recall the next transient refusal still retries");
	ring_start_status = HAL_TRANSIENT;
	ring_start_fail_n = 1;
	hops_reset();
	cops_reset();
	inject_incoming(0, 1, "5553000", "Carol", 0);
	CHECK(state_is(0, "RINGING"));
	CHECK_UINT(hop_count(H_RING_START), 1);
	CHECK_UINT(ring_ok_count(), 0);
	uloop_pump(RING_RETRY_PUMP_MS);
	CHECK_UINT(hop_count(H_RING_START), 2);
	CHECK_UINT(ring_ok_count(), 1);
	CHECK(!cop_has(C_REJECT));
	CHECK(state_is(0, "RINGING"));

	/*
	 * The other half of the same failure: the 180 itself cannot be
	 * built. call.c refuses that INVITE and fails the leg, and the report
	 * is deferred behind the offer frame, so the bell starts and stops in
	 * one pass and nothing waits on a timer.
	 */
	CASE("a 180 the leg cannot build stops the bell in the ring pass");
	cfg_default();
	fixture_start();
	region_ready();
	call_ring_refuse = true;
	inject_incoming(0, 0, "5551000", "Alice", 0);
	call_ring_refuse = false;
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(hop_count(H_RING_START), 1);
	CHECK_UINT(hop_count(H_RING_STOP), 1);
	CHECK(hop_index(H_RING_START) < hop_index(H_RING_STOP));
	/* the leg layer already answered that INVITE, so the shell owes it
	 * no reject of its own */
	CHECK(!cop_has(C_REJECT));
	view_get(0, &v);
	CHECK(!v.ringing);
	CHECK(!v.ring_held);

	/*
	 * Both halves at once. The leg layer refused the 180 and the backend
	 * refused the bell inside one frame, so two refusals are in flight for
	 * one call. The leg layer owns the answer to that caller, and the
	 * give-up record goes with the request that op_ring_stop() ends, so the
	 * shell adds no 480 of its own.
	 */
	CASE("a refused 180 and a fatal bell in one pass add no second "
	     "refusal");
	cfg_default();
	fixture_start();
	region_ready();
	call_ring_refuse = true;
	ring_start_status = HAL_FATAL;
	ring_start_fail_n = 1;
	inject_incoming(0, 0, "5551000", "Alice", 0);
	call_ring_refuse = false;
	CHECK_UINT(cop_count(C_RING), 1);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(hop_count(H_RING_START), 1);
	CHECK_UINT(hop_count(H_RING_STOP), 1);
	CHECK(!cop_has(C_REJECT));
	uloop_pump(RING_RETRY_PUMP_MS);
	CHECK(!cop_has(C_REJECT));
	CHECK_UINT(hop_count(H_RING_START), 1);
	CHECK(state_is(0, "IDLE"));
	view_get(0, &v);
	CHECK(!v.ringing);
	CHECK(!v.ring_held);

	CASE("the give-up is logged once at error level and names the 480");
	cfg_default();
	fixture_start();
	region_ready();
	ring_start_status = HAL_FATAL;
	ring_start_fail_n = 1;
	warn_capture_begin();
	inject_incoming(0, 0, "5551000", "Alice", 0);
	uloop_pump(RING_RETRY_PUMP_MS);
	CHECK_UINT(warn_capture_count_at(LOG_ERR, RING_GIVE_UP_LOG), 1);
	/* a call the subscriber loses is an error and not a warning */
	CHECK_UINT(warn_capture_count_at(LOG_WARNING, RING_GIVE_UP_LOG), 0);
	warn_capture_end();
	CHECK(state_is(0, "IDLE"));
}

/* ------------------------------------------------ the transferee shadow leg */

/*
 * The swap through the real record path, with call.c stubbed. The cases drive
 * the shell half of the two-name contract. One part of that half is the
 * callbacks of the leg layer during a transfer that this line was asked to
 * take. The other part is the record and stream state that the shell keeps by
 * the leg. The machine keeps everything by the UI name.
 *
 * The invariant under test is the absence of events: a whole transferee
 * exchange runs, and the matrix never leaves CONNECTED. Between the answer of
 * the parent and the takeover of the child, the leg layer emits nothing to the
 * machine. Only the two record callbacks fire, and neither reaches the FSM.
 */
static void t_xfer_swap(void)
{
	struct call_ended parent, child;
	struct cdr_stats_view s;
	int consumer = 0;

	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();

	CASE("xfer swap: the call the transfer will move is connected");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	media_run(0, 0);
	CHECK(state_is(0, "CONNECTED"));
	CHECK_UINT(cdr_take(), 1);
	CHECK(dviews[0].in_progress);
	CHECK_UINT(dviews[0].slot, 0);

	/*
	 * The child INVITE reaches the wire. It is a leg no op of this shell
	 * placed, so cb.started is the only edge that can open its record, and
	 * it names the leg's own slot: the child sits on the other physical
	 * one.
	 */
	/*
	 * The name moves before the shell's record edges run, so every one
	 * of them exercises the UI-to-physical mapping rather than the identity
	 * it happens to be on an untransferred line.
	 */
	CASE("xfer swap: the child leg opens a record of its own on the other slot");
	cops_reset();
	xfer_stream_map(0, 1);
	rec_cb.started(0, 1, false, 0);
	CHECK_UINT(cdr_take(), 2);
	CHECK_UINT(dviews[1].slot, 1);
	CHECK(dviews[1].in_progress);
	CHECK(!dviews[1].inbound);
	CHECK(dviews[0].in_progress);

	CASE("xfer swap: opening it counted one outbound attempt and no offer");
	CHECK(cdr_stats(0, &s));
	CHECK_UINT(s.out_attempted, 2);
	CHECK_UINT(s.in_received, 0);

	CASE("xfer swap: and the machine was told nothing at all");
	CHECK(state_is(0, "CONNECTED"));
	CHECK_UINT(ncops, 0);

	CASE("xfer swap: the correlation names the record the call came from");
	CHECK_UINT(dviews[1].transferred_from, dviews[0].seq);
	CHECK(!dviews[0].transferred_from);

	/* the swap itself: the child now runs the stream the UI slot names,
	 * and the protocol answers that stream to the ubus path */
	CASE("xfer swap: a live query on the UI slot reaches the child's stream");
	media_run(0, 1);
	hops_reset();
	live_reset();
	CHECK_UINT(proto_ops_get()->stream_of_slot(0, 0), 1);
	CHECK_UINT(line_stats_live_query(0,
					 proto_ops_get()->stream_of_slot(0, 0),
					 live_take, &consumer), LINE_STATS_OK);
	CHECK_UINT(hop_count(H_STRM_STATS), 1);
	CHECK_UINT(hop_last(H_STRM_STATS)->val, 1);
	job_complete(HAL_OK);
	CHECK_UINT(live_answers, 1);

	CASE("xfer swap: and dropping it by the UI name finds the query it made");
	live_reset();
	CHECK_UINT(line_stats_live_query(0,
					 proto_ops_get()->stream_of_slot(0, 0),
					 live_take, &consumer), LINE_STATS_OK);
	line_stats_live_drop(0, proto_ops_get()->stream_of_slot(0, 0));
	CHECK_UINT(live_answers, 0);
	CHECK_UINT(live_failures, 0);

	/*
	 * The transferor ends the leg its referral replaced. The record closes
	 * against the leg it belongs to and carries the transfer flag, which is
	 * the correlation key of the frozen handoff.
	 */
	CASE("xfer swap: the replaced leg closes its own record, transferred");
	parent = ended_of(false, true, false, "5");
	parent.transferred = true;
	rec_cb.ended(0, 0, &parent);
	CHECK_UINT(cdr_take(), 2);
	CHECK_UINT(dviews[0].slot, 0);
	CHECK(!dviews[0].in_progress);
	CHECK(dviews[0].transferred);

	CASE("xfer swap: the child's record is still open and is not transferred");
	CHECK_UINT(dviews[1].slot, 1);
	CHECK(dviews[1].in_progress);
	CHECK(!dviews[1].transferred);

	CASE("xfer swap: the two records are adjacent on the same line");
	CHECK_UINT(dviews[0].line, dviews[1].line);
	CHECK_UINT(dviews[1].seq, dviews[0].seq + 1);

	CASE("xfer swap: the FSM never saw the exchange");
	CHECK(state_is(0, "CONNECTED"));
	CHECK_UINT(ncops, 0);

	CASE("xfer swap: the child closes its own record when the call ends");
	child = ended_of(false, true, false, "1003");
	rec_cb.ended(0, 1, &child);
	CHECK_UINT(cdr_take(), 2);
	CHECK(!dviews[0].in_progress);
	CHECK(!dviews[1].in_progress);
	CHECK(!strcmp(dviews[1].number, "1003"));

	CASE("xfer swap: exactly one record opened and closed per leg");
	CHECK(cdr_stats(0, &s));
	CHECK_UINT(s.out_attempted, 2);
	CHECK_UINT(s.out_answered, 2);

	/*
	 * The fax latches are the leg's too. The switch is asked for under the
	 * UI name and re-armed by the record edge under the leg's own slot, so
	 * both have to name one array entry or a transferred call could never
	 * switch again.
	 */
	CASE("xfer swap: the fax switch of the UI slot reaches the child's leg");
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	xfer_stream_map(0, 1);
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK(cop_has(C_FAX));
	CHECK_UINT(cop_find(C_FAX)->slot, 0);

	CASE("xfer swap: a second tone on the same leg is latched");
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK(!cop_has(C_FAX));

	CASE("xfer swap: the child's own end re-arms it, the replaced leg's does not");
	rec_cb.ended(0, 0, &parent);
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK(!cop_has(C_FAX));
	child = ended_of(false, true, false, "1003");
	rec_cb.ended(0, 1, &child);
	cops_reset();
	inject_tone(0, HAL_TONE_DET_CED, HAL_TONE_DIR_LOCAL, 0);
	CHECK(cop_has(C_FAX));
}

/* ------------------------------------------------- the three-way conference */

/*
 * The conference of TWO_CALLS and CONF. The machine owns the states and the
 * eligibility, and test_fsm covers those; what these cases drive is the
 * shell half: the mixer op with its retry and its two refusal answers, the
 * capability term of the derive with the fault behind it, the digit fan-out,
 * the direction a takeover pushes, the status key and the record mark.
 *
 * Both lines carry the same conference config, because the op names a line
 * beside its mode and a shell that named the wrong one would still pass every
 * case driven on line 0.
 */
static void conf_cfg(unsigned int local_conf, unsigned int flash_mode)
{
	unsigned int i;

	cfg_default();

	for (i = 0; i < 2; i++) {
		struct cfg_channel *ch = &cfg.channel[i];

		cfg_channel_default(ch, i, true);
		snprintf(ch->digit_map, sizeof(ch->digit_map), "5");
		ch->local_conf_enabled = local_conf;
	}
	/* the flash service is the region's, one value for both lines */
	fake_region.flash_service = (uint8_t)flash_mode;

	/* the mixer the derive reads; the availability cases take it away */
	fake_features_extra = HAL_FEAT_CONF;
}

/*
 * The TWO_CALLS line the conference is offered from: two calls this
 * subscriber placed, the second one active and the first one held. Both went
 * out, which is what the two direction terms of the split mode need.
 */
static void conf_two_calls(unsigned int line)
{
	inject_hook(line, HAL_HOOK_SEIZE, 0);
	inject_digit(line, '5', false);
	rec_cb.answered(line, 0);
	inject_hook(line, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(line, 0, true);
	inject_digit(line, '5', false);
	rec_cb.answered(line, 1);
	hops_reset();
	cops_reset();
}

/* the flash that joins them, and the resume completion the mixer rides */
static void conf_join(unsigned int line)
{
	inject_hook(line, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(line, 0, true);
}

/*
 * The same pair, with the held leg replaced by a transferee's child. The call
 * came in and the child that took its name went out, so the direction the
 * split mode reads for the first call is the one cb.started pushed and nothing
 * else on this line carries it. from_slot CALL_SLOTS is the start that names
 * no parent.
 *
 * The call this line took sits on the second physical slot and wears the first
 * UI name, so the leg the callback reports and the name the machine knows are
 * different numbers. A shell that pushed the reported leg straight at the
 * machine would move the wrong slot and the flash below would swap.
 */
static void conf_takeover_pair(unsigned int from_slot)
{
	struct call_ended parent = ended_of(true, true, false, "5551000");

	xfer_stream_map(0, 1);
	xfer_stream_map(1, 0);
	inject_incoming(0, 0, "5551000", "Alice", 0);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	/* the child INVITE reaches the wire on the free physical slot, while
	 * the leg it replaces still holds the UI name (call.h, the two-name
	 * contract) */
	rec_cb.started(0, 0, false, from_slot);
	/* the child is answered, so the name moves onto its leg and the leg it
	 * replaced ends */
	xfer_stream_map(0, 0);
	xfer_stream_map(1, 1);
	rec_cb.ended(0, 1, &parent);
	/* the second call, placed from the recall dial tone of HOLD_DIAL */
	inject_hook(0, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(0, 0, true);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 1);
	hops_reset();
	cops_reset();
}

static void t_conf(void)
{
	struct line_view v;

	CASE("the join reaches the backend as the local mix");
	conf_cfg(1, 1);
	fixture_start();
	region_ready();
	conf_two_calls(0);
	conf_join(0);
	CHECK_UINT(hop_count(H_CONF), 1);
	CHECK_UINT(hop_last(H_CONF)->val, HAL_CONF_LOCAL_MIX);
	CHECK_UINT(hop_last(H_CONF)->line, 0);

	CASE("and the line is in the conference");
	CHECK(state_is(0, "CONF"));

	CASE("the status carries the conference key while the mixer runs");
	CHECK(view_get(0, &v));
	CHECK(v.conf);

	CASE("a digit in the conference reaches both legs");
	cops_reset();
	inject_digit(0, '1', false);
	CHECK_UINT(cop_count(C_DTMF), 2);
	CHECK_UINT(cop_slot_count(C_DTMF, 0), 1);
	CHECK_UINT(cop_slot_count(C_DTMF, 1), 1);

	CASE("the split holds the leg the join resumed and keeps the mixer on");
	hops_reset();
	cops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(state_is(0, "PENDING"));
	CHECK(cop_has(C_HOLD));
	CHECK_UINT(cop_find(C_HOLD)->slot, 0);
	CHECK_UINT(hop_count(H_CONF), 0);

	CASE("and the completion hands the mixer the off mode");
	rec_cb.reinvite_done(0, 0, true);
	CHECK_UINT(hop_count(H_CONF), 1);
	CHECK_UINT(hop_last(H_CONF)->val, HAL_CONF_OFF);
	CHECK(state_is(0, "TWO_CALLS"));

	CASE("the status key goes with the mixer");
	CHECK(view_get(0, &v));
	CHECK(!v.conf);

	CASE("a digit outside the conference reaches the active leg alone");
	cops_reset();
	inject_digit(0, '1', false);
	CHECK_UINT(cop_count(C_DTMF), 1);
	CHECK_UINT(cop_find(C_DTMF)->slot, 1);

	CASE("a connected line carries no conference key");
	conf_cfg(1, 1);
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	CHECK(state_is(0, "CONNECTED"));
	CHECK(view_get(0, &v));
	CHECK(!v.conf);

	CASE("and an idle one carries none either");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK(view_get(0, &v));
	CHECK(!v.conf);

	/* hal.h section 3: one transient answer is retried in this frame */
	CASE("a transient answer is retried and the conference stands");
	conf_cfg(1, 1);
	fixture_start();
	region_ready();
	conf_two_calls(0);
	conf_transient_n = 1;
	conf_join(0);
	CHECK(state_is(0, "CONF"));
	CHECK_UINT(hop_count(H_CONF), 2);

	CASE("a second transient answer injects the refusal instead");
	conf_cfg(1, 1);
	fixture_start();
	region_ready();
	conf_two_calls(0);
	conf_transient_n = 2;
	conf_join(0);
	CHECK(state_is(0, "PENDING"));
	CHECK(cop_has(C_HOLD));
	CHECK_UINT(cop_find(C_HOLD)->slot, 0);

	CASE("and the mixer was asked exactly twice, so the retry ran once");
	CHECK_UINT(hop_count(H_CONF), 2);

	CASE("the split behind the refusal gives the two calls back");
	rec_cb.reinvite_done(0, 0, true);
	CHECK(state_is(0, "TWO_CALLS"));
	CHECK_UINT(hop_last(H_CONF)->val, HAL_CONF_OFF);

	CASE("and the refusal keeps the next flash on the swap");
	hops_reset();
	cops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK_UINT(hop_count(H_CONF), 0);
	CHECK(cop_has(C_HOLD));
	CHECK_UINT(cop_find(C_HOLD)->slot, 1);
	CHECK(state_is(0, "PENDING"));

	CASE("a fatal answer injects the refusal with no retry at all");
	conf_cfg(1, 1);
	fixture_start();
	region_ready();
	conf_two_calls(0);
	conf_refuse = true;
	conf_join(0);
	CHECK_UINT(hop_count(H_CONF), 1);
	CHECK(state_is(0, "PENDING"));
	CHECK(cop_has(C_HOLD));
	CHECK_UINT(cop_find(C_HOLD)->slot, 0);
	conf_refuse = false;

	/* hal.h section 3 answers unsupported for a backend whose feature bit
	 * is clear. The shell owes the machine a refusal for it, as it does
	 * for a fatal one; only the off reads that answer as done */
	CASE("an on the backend calls unsupported is a refusal too");
	conf_cfg(1, 1);
	fixture_start();
	region_ready();
	conf_two_calls(0);
	conf_unsupported = true;
	conf_join(0);
	CHECK_UINT(hop_count(H_CONF), 1);
	CHECK(state_is(0, "PENDING"));
	CHECK(cop_has(C_HOLD));
	CHECK_UINT(cop_find(C_HOLD)->slot, 0);
	conf_unsupported = false;

	CASE("a refused off is reported loud and both legs stay");
	conf_cfg(1, 1);
	fixture_start();
	region_ready();
	conf_two_calls(0);
	conf_join(0);
	cops_reset();
	conf_refuse = true;
	warn_capture_begin();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(0, 0, true);
	CHECK_UINT(warn_capture_count("conference mixer did not stop"), 1);
	CHECK(state_is(0, "TWO_CALLS"));
	CHECK(!cop_has(C_HANGUP));
	conf_refuse = false;

	CASE("an off still transient after the retry is reported the same way");
	conf_cfg(1, 1);
	fixture_start();
	region_ready();
	conf_two_calls(0);
	conf_join(0);
	hops_reset();
	cops_reset();
	conf_transient_n = 2;
	warn_capture_begin();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(0, 0, true);
	CHECK_UINT(warn_capture_count("conference mixer did not stop"), 1);
	CHECK_UINT(hop_count(H_CONF), 2);
	CHECK(!cop_has(C_HANGUP));
	CHECK(state_is(0, "TWO_CALLS"));

	CASE("an off the backend calls unsupported completes quietly");
	conf_cfg(1, 1);
	fixture_start();
	region_ready();
	conf_two_calls(0);
	conf_join(0);
	cops_reset();
	conf_unsupported = true;
	warn_capture_begin();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(0, 0, true);
	CHECK_UINT(warn_capture_count("conference mixer"), 0);
	CHECK(state_is(0, "TWO_CALLS"));
	CHECK(!cop_has(C_HANGUP));
	conf_unsupported = false;

	CASE("a backend with no mixer keeps the flash on the swap");
	conf_cfg(1, 1);
	fake_features_extra = 0;
	fixture_start();
	region_ready();
	conf_two_calls(0);
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK_UINT(hop_count(H_CONF), 0);
	CHECK_UINT(cop_find(C_HOLD)->slot, 1);
	CHECK(state_is(0, "PENDING"));

	/*
	 * The fault waits for the capability to be final. A backend can fill
	 * it as late as the DSP start, so a report from the early half could
	 * name a missing mixer on a board that has one. Both configured lines
	 * report it, which is what says the report is per line.
	 */
	CASE("the early half reports no fault, the capability is not final");
	conf_cfg(1, 1);
	fake_features_extra = 0;
	lines_stop();
	uv.hal = &fake_hal;
	uv.cfg = &cfg;
	uv.hal_state = UVOICE_HAL_READY;
	fake_cap.lines = 2;
	fake_cap.features = HAL_FEAT_POLARITY;
	warn_capture_begin();
	lines_init(&cfg);
	lines_live = true;
	CHECK_UINT(warn_capture_count("carries no mixer"), 0);

	CASE("and the DSP half reports it once for each configured line");
	warn_capture_begin();
	lines_start(&cfg);
	CHECK_UINT(warn_capture_count("carries no mixer"), 2);

	CASE("a reload reports it once more");
	warn_capture_begin();
	lines_reload(&cfg);
	CHECK_UINT(warn_capture_count("carries no mixer"), 2);
	warn_capture_end();

	CASE("a mixer with the conference service off reports nothing");
	conf_cfg(0, 1);
	warn_capture_begin();
	fixture_start();
	region_ready();
	CHECK_UINT(warn_capture_count("carries no mixer"), 0);

	CASE("and that line swaps the two calls");
	conf_two_calls(0);
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK_UINT(hop_count(H_CONF), 0);
	CHECK_UINT(cop_find(C_HOLD)->slot, 1);
	CHECK(state_is(0, "PENDING"));

	CASE("flash service mode 0 offers no local branch at all");
	conf_cfg(1, 0);
	warn_capture_begin();
	fixture_start();
	region_ready();
	CHECK_UINT(warn_capture_count("carries no mixer"), 0);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	cops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(!cop_has(C_HOLD));
	CHECK(state_is(0, "CONNECTED"));

	CASE("flash service mode 1 splits the conference back into two calls");
	conf_cfg(1, 1);
	fixture_start();
	region_ready();
	conf_two_calls(0);
	conf_join(0);
	CHECK(state_is(0, "CONF"));
	cops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(cop_has(C_HOLD));
	CHECK_UINT(cop_find(C_HOLD)->slot, 0);
	rec_cb.reinvite_done(0, 0, true);
	CHECK(state_is(0, "TWO_CALLS"));
	CHECK(!cop_has(C_HANGUP));

	CASE("flash service mode 2 drops the second call and keeps the first");
	conf_cfg(1, 2);
	fixture_start();
	region_ready();
	conf_two_calls(0);
	conf_join(0);
	CHECK(state_is(0, "CONF"));
	hops_reset();
	cops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(state_is(0, "CONNECTED"));
	CHECK(cop_has(C_HANGUP));
	CHECK_UINT(cop_find(C_HANGUP)->slot, 1);
	/* HAL_CONF_OFF is zero and an absent hop answers zero, so the mode
	 * assertion needs the count beside it to mean anything */
	CHECK_UINT(hop_count(H_CONF), 1);
	CHECK_UINT(hop_last(H_CONF)->val, HAL_CONF_OFF);

	CASE("the mixer op names the line the gesture was made on");
	conf_cfg(1, 1);
	fixture_start();
	region_ready();
	conf_two_calls(1);
	conf_join(1);
	CHECK_UINT(hop_last(H_CONF)->line, 1);
	CHECK(state_is(1, "CONF"));
	CHECK(state_is(0, "IDLE"));

	CASE("a reload cannot turn the conference on under a live pair");
	conf_cfg(0, 1);
	fixture_start();
	region_ready();
	conf_two_calls(0);
	cfg.channel[0].local_conf_enabled = 1;
	cfg.channel[1].local_conf_enabled = 1;
	lines_reload(&cfg);
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK_UINT(hop_count(H_CONF), 0);
	CHECK_UINT(cop_find(C_HOLD)->slot, 1);

	CASE("and it lands on the pair after it");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	conf_two_calls(0);
	conf_join(0);
	CHECK(state_is(0, "CONF"));
	CHECK_UINT(hop_count(H_CONF), 1);

	CASE("the join marks both records");
	conf_cfg(1, 1);
	fixture_start();
	region_ready();
	conf_two_calls(0);
	conf_join(0);
	CHECK_UINT(cdr_take(), 2);
	CHECK(dviews[0].conferenced);
	CHECK(dviews[1].conferenced);

	/* the mark is keyed by the leg and the machine names UI slots. This
	 * pair wears its two names the other way round, which is what a
	 * transfer leaves behind, and both legs are still the joined ones */
	CASE("and both records carry it when a transfer swapped the two names");
	conf_cfg(1, 1);
	fixture_start();
	region_ready();
	conf_two_calls(0);
	xfer_stream_map(0, 1);
	xfer_stream_map(1, 0);
	conf_join(0);
	CHECK_UINT(cdr_take(), 2);
	CHECK(dviews[0].conferenced);
	CHECK(dviews[1].conferenced);

	CASE("a takeover start pushes the child's direction onto the name");
	conf_cfg(1, 1);
	fixture_start();
	region_ready();
	conf_takeover_pair(1);
	CHECK(state_is(0, "TWO_CALLS"));
	inject_hook(0, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(0, 0, true);
	CHECK_UINT(hop_count(H_CONF), 1);
	CHECK(state_is(0, "CONF"));

	CASE("a start that names no parent writes no direction");
	conf_cfg(1, 1);
	fixture_start();
	region_ready();
	conf_takeover_pair(CALL_SLOTS);
	CHECK(state_is(0, "TWO_CALLS"));
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK_UINT(hop_count(H_CONF), 0);
	CHECK_UINT(cop_find(C_HOLD)->slot, 1);
	CHECK(state_is(0, "PENDING"));

	/*
	 * The two halves disagree about the capability: the early half
	 * derives no conference and the DSP half has to
	 * correct a view the machine already holds. Both cases below run the
	 * halves by hand and move the mask between them.
	 */
	CASE("a mixer that arrives with the DSP still reaches the machine");
	conf_cfg(1, 1);
	fake_features_extra = 0;
	lines_stop();
	uv.hal = &fake_hal;
	uv.cfg = &cfg;
	uv.hal_state = UVOICE_HAL_READY;
	fake_cap.lines = 2;
	fake_cap.features = HAL_FEAT_POLARITY;
	hops_reset();
	cops_reset();
	memset(boot_offhook, 0, sizeof(boot_offhook));
	lines_init(&cfg);
	lines_live = true;
	fake_cap.features = HAL_FEAT_POLARITY | HAL_FEAT_CONF;
	lines_start(&cfg);
	conf_two_calls(0);
	conf_join(0);
	CHECK(state_is(0, "CONF"));
	CHECK_UINT(hop_count(H_CONF), 1);

	/*
	 * The same flip with a reload already staged behind it. The settle
	 * corrects the staged view instead of replacing it: a settle that
	 * seeded the staged copies again would put the running config back and
	 * the reload would be lost.
	 */
	CASE("a reload staged in the bring-up window survives the settle");
	conf_cfg(1, 1);
	lines_stop();
	fake_cap.features = HAL_FEAT_POLARITY;
	hops_reset();
	cops_reset();
	lines_init(&cfg);
	lines_live = true;
	/* the handset comes up before the DSP does, so no view can land until
	 * it goes back on the cradle */
	boot_offhook[0] = true;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	cfg.channel[0].call_waiting_enabled = false;
	cfg.channel[1].call_waiting_enabled = false;
	lines_reload(&cfg);
	fake_cap.features = HAL_FEAT_POLARITY | HAL_FEAT_CONF;
	lines_start(&cfg);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK(view_get(0, &v));
	CHECK(v.call_waiting);

	CASE("and the reloaded value lands at the rest state");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK(view_get(0, &v));
	CHECK(!v.call_waiting);

	CASE("and the mode the final capability derived lands with it");
	boot_offhook[0] = false;
	conf_two_calls(0);
	conf_join(0);
	CHECK(state_is(0, "CONF"));
	CHECK_UINT(hop_count(H_CONF), 1);

	/*
	 * The settle of the view the mixer landed ends in the line settle, and
	 * it runs behind the presentation of the bring-up: a clear the switch
	 * sent before the DSP was ready reaches the phone there. With no mixer
	 * moving, no view lands, and the same record waits for the next rest.
	 */
	CASE("a clear held before the DSP is presented when the mixer lands");
	conf_cfg(1, 1);
	lines_stop();
	uv.hal_state = UVOICE_HAL_STARTING;
	fake_cap.features = HAL_FEAT_POLARITY;
	lines_init(&cfg);
	lines_live = true;
	mwi_notify(0, false, 0, 0);
	mwi_notify(0, true, 1, 0);
	mwi_notify(0, false, 0, 0);
	CHECK_UINT(hop_count(H_CID), 0);
	uv.hal_state = UVOICE_HAL_READY;
	fake_cap.features = HAL_FEAT_POLARITY | HAL_FEAT_CONF;
	hops_reset();
	lines_start(&cfg);
	CHECK_UINT(hop_count(H_CID), 1);
	CHECK_UINT(hop_last(H_CID)->line, 0);
	CHECK_UINT(hop_last(H_CID)->cid.mwi, HAL_MWI_OFF);

	CASE("and with no mixer moving the same clear waits for the next rest");
	conf_cfg(1, 1);
	lines_stop();
	uv.hal_state = UVOICE_HAL_STARTING;
	fake_cap.features = HAL_FEAT_POLARITY;
	lines_init(&cfg);
	lines_live = true;
	mwi_notify(0, true, 1, 0);
	mwi_notify(0, false, 0, 0);
	uv.hal_state = UVOICE_HAL_READY;
	hops_reset();
	lines_start(&cfg);
	CHECK_UINT(hop_count(H_CID), 0);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK_UINT(hop_count(H_CID), 0);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK_UINT(hop_count(H_CID), 1);
	CHECK_UINT(hop_last(H_CID)->cid.mwi, HAL_MWI_OFF);

	/*
	 * The landing of that view carries the core half too: a hook window
	 * the backend refused at the bring-up push (both tries) is pushed again
	 * behind the presentation of the line. With no mixer moving nothing
	 * lands, and the window stays owed.
	 */
	CASE("a refused hook window is pushed again when the mixer lands");
	conf_cfg(1, 1);
	lines_stop();
	uv.hal_state = UVOICE_HAL_STARTING;
	fake_cap.features = HAL_FEAT_POLARITY;
	lines_init(&cfg);
	lines_live = true;
	uv.hal_state = UVOICE_HAL_READY;
	fake_cap.features = HAL_FEAT_POLARITY | HAL_FEAT_CONF;
	memset(hook_ts_calls, 0, sizeof(hook_ts_calls));
	hook_ts_transient = 2;
	hops_reset();
	lines_start(&cfg);
	hook_ts_transient = 0;
	CHECK_UINT(hook_ts_calls[0], 3);
	CHECK_UINT(hook_ts_calls[1], 1);
	CHECK_UINT(hop_last(H_HOOK_TS)->line, 0);

	CASE("and with no mixer moving the refused window is not pushed again");
	conf_cfg(1, 1);
	lines_stop();
	uv.hal_state = UVOICE_HAL_STARTING;
	fake_cap.features = HAL_FEAT_POLARITY;
	lines_init(&cfg);
	lines_live = true;
	uv.hal_state = UVOICE_HAL_READY;
	memset(hook_ts_calls, 0, sizeof(hook_ts_calls));
	hook_ts_transient = 2;
	lines_start(&cfg);
	hook_ts_transient = 0;
	CHECK_UINT(hook_ts_calls[0], 2);
	CHECK_UINT(hook_ts_calls[1], 1);

	/* the mask outlives this block, so the suites after it run on the
	 * capability every other fixture expects */
	fake_features_extra = 0;
}

/*
 * The second half of the playout gate of t_digit_play above: the states and
 * the slot roles it reads. It sits here because the two-call, conference and
 * network alert fixtures it drives are defined above it.
 */
static void t_digit_play_states(void)
{
	/* the alert keeps the machine CONNECTED and holds the playout slot the
	 * digit would take */
	CASE("a network call waiting alert refuses a digit");
	cfg_default();
	fixture_start();
	cw_net_line_connect();
	CHECK(state_is(0, "CONNECTED"));
	rec_cb.cw_net(0, 0, true, 2, "Bob Jones", "5552000");
	hops_reset();
	inject_digit_stream(0, 0, '5', true, 160);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 0);

	CASE("and a digit plays once the alert stops");
	rec_cb.cw_net(0, 0, false, 0, "", "");
	hops_reset();
	inject_digit_stream(0, 0, '6', true, 160);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 1);

	CASE("the held leg of two calls refuses a digit");
	xfer_cfg(0);
	fixture_start();
	xfer_two_calls(0);
	CHECK(state_is(0, "TWO_CALLS"));
	hops_reset();
	inject_digit_stream(0, 0, '5', true, 160);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 0);

	CASE("and the active leg takes one");
	inject_digit_stream(0, 1, '5', true, 160);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 1);

	/*
	 * A swap moves the leg an entry named to hold while the entry waits,
	 * and no frame of this shell closes on that move, so the queue asks
	 * the gate again for every entry it plays.
	 */
	CASE("a queued digit whose leg went on hold is dropped at its turn");
	xfer_cfg(0);
	fixture_start();
	xfer_two_calls(0);
	hops_reset();
	inject_digit_stream(0, 1, '5', true, 0);
	inject_digit_stream(0, 1, '6', true, 0);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 1);
	inject_hook(0, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(0, 1, true);
	rec_cb.reinvite_done(0, 0, true);
	CHECK(state_is(0, "TWO_CALLS"));
	uloop_pump(DIGIT_STEP_EARLY_MS + DIGIT_STEP_LATE_MS);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 1);

	/*
	 * A transfer child takes the UI name of the leg it replaces without a
	 * state change, so the slot alone would let the replacement play the
	 * digits the replaced leg queued.
	 */
	CASE("a queued digit of a leg a transfer replaced is dropped at its turn");
	digit_play_connect();
	inject_digit_stream(0, 0, '5', true, 0);
	inject_digit_stream(0, 0, '6', true, 0);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 1);
	xfer_stream_map(0, 1);
	uloop_pump(DIGIT_STEP_EARLY_MS + DIGIT_STEP_LATE_MS);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 1);

	CASE("both legs of the conference take a digit");
	conf_cfg(1, 1);
	fixture_start();
	region_ready();
	conf_two_calls(0);
	conf_join(0);
	CHECK(state_is(0, "CONF"));
	hops_reset();
	inject_digit_stream(0, 1, '5', true, 0);
	uloop_pump(DIGIT_STEP_EARLY_MS + DIGIT_STEP_LATE_MS);
	inject_digit_stream(0, 0, '6', true, 0);
	CHECK_UINT(hop_count(H_DIGIT_PLAY), 2);
	fake_features_extra = 0;
}

/* ------------------------------------------------------ forward disconnect */

/*
 * Forward disconnect, from CONNECTED and CLEARING. The cases configure a
 * short reversal length, because the restore uses a real timer like every
 * other timer in this suite. The delayed window is the fixed 15 s of
 * LINE_FWD_DISC_HOLD_MS. Most cases read the armed window through the status
 * dump. Five cases wait the window out:
 *
 * - the straddle of the deadline, in this group;
 * - the re-arm that moves the deadline, in this group;
 * - the held-leg case of t_fwd_disc_remove(), which needs the removal window
 *   open;
 * - the first case of t_fwd_disc_held(), which needs the reversal open;
 * - the later clearing in t_fwd_disc_held(). That case follows a recall
 *   ring. fwd_disc_blocked() can drop an armed window at its expiry, so only
 *   the wait shows the reversal.
 */
#define FD_DELAY_MS		200
#define FD_DELAY_WAIT_MS	400
/* LINE_FWD_DISC_HOLD_MS, the 15 s delayed window, restated here for the
 * same reason CW_CID_EARLY_MS is: the constant is private to line/line.c. The
 * straddle pumps to 14.5 s and then past 15 s, so the window is pinned from
 * both sides rather than by one wait long enough to hide a wrong value. */
#define FD_HOLD_MS		15000
#define FD_HOLD_EARLY_MS	14500
#define FD_HOLD_LATE_MS		1000

static void fd_cfg(unsigned int delay_ms, enum cfg_fwd_disc_battery batt)
{
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	cfg.channel[0].forward_disconnect_delay_ms = delay_ms;
	cfg.channel[0].forward_disconnect_battery = batt;
}

/* the answered call every far-end-clear case starts from */
static void fd_connect(void)
{
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	hops_reset();
	cops_reset();
}

/*
 * A peer BYE on an answered leg, which is what call.c rx_bye reports and the
 * whole far-end-clear signal: the shell narrows it to the leg facing the
 * phone and to the state the machine lands in.
 */
static void inject_far_end_clear(unsigned int line, unsigned int slot)
{
	rec_cb.remote_bye(line, slot);
}

static bool polarity_seen(enum hal_polarity pol)
{
	unsigned int i;

	for (i = 0; i < nhops; i++) {
		if (hops[i].kind == H_POLARITY && hops[i].val == (unsigned int)pol)
			return true;
	}

	return false;
}

static void t_fwd_disc(void)
{
	struct line_view v;

	CASE("the shipped config reverses nothing on a far end clear");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fd_connect();
	inject_far_end_clear(0, 0);
	CHECK(state_is(0, "CLEARING"));
	CHECK(!hop_has(H_POLARITY));

	CASE("a reversal length with no battery form arms nothing");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_NONE);
	fd_connect();
	inject_far_end_clear(0, 0);
	CHECK(!hop_has(H_POLARITY));

	CASE("a battery form with no reversal length arms nothing");
	fd_cfg(0, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	inject_far_end_clear(0, 0);
	CHECK(!hop_has(H_POLARITY));

	CASE("a far end clear reverses the line battery");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	inject_far_end_clear(0, 0);
	CHECK(state_is(0, "CLEARING"));
	CHECK(hop_has(H_POLARITY));
	CHECK_UINT(hop_last(H_POLARITY)->val, HAL_POL_REVERSE);
	view_get(0, &v);
	CHECK(v.fwd_disc_reversed);

	CASE("the quiet clearing of that clear arms no delayed window");
	CHECK(!v.fwd_disc_armed);

	CASE("the reversal is restored when its own window ends");
	hops_reset();
	uloop_pump(FD_DELAY_WAIT_MS);
	CHECK(hop_has(H_POLARITY));
	CHECK_UINT(hop_last(H_POLARITY)->val, HAL_POL_FORWARD);
	view_get(0, &v);
	CHECK(!v.fwd_disc_reversed);

	CASE("the restore happens once, not on every later pass");
	hops_reset();
	uloop_pump(FD_DELAY_WAIT_MS);
	CHECK(!hop_has(H_POLARITY));

	/*
	 * The far-end distinction. A session timer death and a transport death
	 * reach the machine with the same quiet class the far-end clear lands
	 * in, so a shell that read the class alone would reverse for both.
	 */
	CASE("a local failure in the quiet class is not a far end clear");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	rec_cb.failed(0, 0, CALL_FAIL_QUIET);
	CHECK(state_is(0, "CLEARING"));
	CHECK(!hop_has(H_POLARITY));

	CASE("a clear that names a leg the line is not talking on is ignored");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	inject_far_end_clear(0, 1);
	CHECK(state_is(0, "CONNECTED"));
	CHECK(!hop_has(H_POLARITY));

	/*
	 * The backstop. hal.h records a polarity set made while a line is not
	 * ACTIVE and applies it at the next ACTIVE entry, so a reversal the
	 * subscriber outran would come back on the next call.
	 */
	CASE("going on hook inside the reversal restores the battery at IDLE");
	fd_cfg(600000, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	inject_far_end_clear(0, 0);
	CHECK(polarity_seen(HAL_POL_REVERSE));
	hops_reset();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK(hop_has(H_POLARITY));
	CHECK_UINT(hop_last(H_POLARITY)->val, HAL_POL_FORWARD);
	view_get(0, &v);
	CHECK(!v.fwd_disc_reversed);

	CASE("a line that never reversed asks the backend for nothing at IDLE");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	hops_reset();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK(!hop_has(H_POLARITY));

	CASE("a backend that refuses the reversal still gets the restore");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	polarity_status = HAL_FATAL;
	inject_far_end_clear(0, 0);
	polarity_status = HAL_OK;
	hops_reset();
	uloop_pump(FD_DELAY_WAIT_MS);
	CHECK(hop_has(H_POLARITY));
	CHECK_UINT(hop_last(H_POLARITY)->val, HAL_POL_FORWARD);

	/* ------------------------------------------------- the delayed form */

	CASE("a busy clearing arms the delayed window");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	rec_cb.failed(0, 0, CALL_FAIL_BUSY);
	view_get(0, &v);
	CHECK(strcmp(v.clearing, "busy") == 0);
	CHECK(v.fwd_disc_armed);
	CHECK(!hop_has(H_POLARITY));

	CASE("an unobtainable clearing arms it too");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	rec_cb.failed(0, 0, CALL_FAIL_UNOBTAINABLE);
	view_get(0, &v);
	CHECK(strcmp(v.clearing, "unobtainable") == 0);
	CHECK(v.fwd_disc_armed);

	CASE("a fast busy clearing arms it too");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	rec_cb.failed(0, 0, CALL_FAIL_CONGESTION);
	view_get(0, &v);
	CHECK(strcmp(v.clearing, "fastbusy") == 0);
	CHECK(v.fwd_disc_armed);

	/*
	 * The quiet class arms nothing. It is where the far-end clear lands,
	 * and the immediate trigger has already taken it; a delayed window here
	 * would reverse a line whose call ended 15 s earlier.
	 */
	CASE("a quiet clearing arms no delayed window");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	rec_cb.failed(0, 0, CALL_FAIL_QUIET);
	view_get(0, &v);
	CHECK(strcmp(v.clearing, "quiet") == 0);
	CHECK(!v.fwd_disc_armed);

	CASE("the delayed window is not armed while the feature is off");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fd_connect();
	rec_cb.failed(0, 0, CALL_FAIL_BUSY);
	view_get(0, &v);
	CHECK(!v.fwd_disc_armed);

	CASE("going on hook cancels the delayed window");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	rec_cb.failed(0, 0, CALL_FAIL_BUSY);
	view_get(0, &v);
	CHECK(v.fwd_disc_armed);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	view_get(0, &v);
	CHECK(!v.fwd_disc_armed);

	/*
	 * The flash shortcut of CLEARING leaves the clearing without going on
	 * hook, so it is the path that proves the cancel is the class rule and
	 * not the rest state: a window left armed here would reverse a line
	 * that is back at dial tone and about to make a new call.
	 */
	CASE("a flash out of the clearing cancels the delayed window too");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	rec_cb.failed(0, 0, CALL_FAIL_BUSY);
	view_get(0, &v);
	CHECK(v.fwd_disc_armed);
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	view_get(0, &v);
	CHECK(!v.fwd_disc_armed);

	/* the busy clearing chains to the fast busy one at busy_tone_timer_sec,
	 * and each treatment owns a window: the chain must not cancel it */
	CASE("the chain from busy to fast busy keeps a window armed");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	cfg.channel[0].busy_tone_s = 1;
	fd_connect();
	rec_cb.failed(0, 0, CALL_FAIL_BUSY);
	view_get(0, &v);
	CHECK(v.fwd_disc_armed);
	uloop_pump(1300);
	view_get(0, &v);
	CHECK(strcmp(v.clearing, "fastbusy") == 0);
	CHECK(v.fwd_disc_armed);

	/*
	 * A clearing the machine reaches from its own timer closes no shell
	 * frame, so the settle that arms the window has to be scheduled behind
	 * the entry tone instead of behind an event.
	 */
	CASE("a start digit window that expires into fast busy arms it as well");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	cfg.channel[0].start_digit_s = 1;
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(1300);
	view_get(0, &v);
	CHECK(strcmp(v.state, "CLEARING") == 0);
	CHECK(strcmp(v.clearing, "fastbusy") == 0);
	CHECK(v.fwd_disc_armed);

	/*
	 * The cases that wait out the 15 s window. The straddle is what pins
	 * the window: a shorter hold would fire early and a longer one late,
	 * and only asserting both sides of the boundary catches either.
	 */
	CASE("the delayed window has not fired just before its 15 s");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	rec_cb.failed(0, 0, CALL_FAIL_BUSY);
	hops_reset();
	uloop_pump(FD_HOLD_EARLY_MS);
	CHECK(!polarity_seen(HAL_POL_REVERSE));
	view_get(0, &v);
	CHECK(v.fwd_disc_armed);

	CASE("and it reverses the battery just after it");
	uloop_pump(FD_HOLD_LATE_MS);
	CHECK(polarity_seen(HAL_POL_REVERSE));

	CASE("and that reversal is restored by its own window too");
	CHECK(polarity_seen(HAL_POL_FORWARD));
	view_get(0, &v);
	CHECK(!v.fwd_disc_reversed);

	/*
	 * The delay is a duration, not a one-shot that restores on the next
	 * pass. Half of a long window must still be reversed.
	 */
	CASE("the reversal is still held at half of its own window");
	fd_cfg(4000, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	inject_far_end_clear(0, 0);
	CHECK(polarity_seen(HAL_POL_REVERSE));
	hops_reset();
	uloop_pump(2000);
	CHECK(!polarity_seen(HAL_POL_FORWARD));
	view_get(0, &v);
	CHECK(v.fwd_disc_reversed);

	CASE("and it is released once the whole window has run");
	uloop_pump(2400);
	CHECK(polarity_seen(HAL_POL_FORWARD));
	view_get(0, &v);
	CHECK(!v.fwd_disc_reversed);

	/* ------------------------------------------- once per clearing episode */

	/*
	 * A far-end clear is announced once. Its quiet clearing chains to the
	 * fast busy one after 2 s, and that class is a delayed trigger, so
	 * without the episode rule the same call would be signalled twice: once
	 * at the clear and again 15 s later, long after the subscriber was told.
	 */
	CASE("the chain behind a far end clear arms no second window");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	inject_far_end_clear(0, 0);
	CHECK(polarity_seen(HAL_POL_REVERSE));
	uloop_pump(2400);
	view_get(0, &v);
	CHECK(strcmp(v.clearing, "fastbusy") == 0);
	CHECK(!v.fwd_disc_armed);

	CASE("and no second reversal follows it");
	hops_reset();
	uloop_pump(FD_HOLD_LATE_MS);
	CHECK(!polarity_seen(HAL_POL_REVERSE));

	CASE("the next call starts a fresh episode");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.failed(0, 0, CALL_FAIL_BUSY);
	view_get(0, &v);
	CHECK(v.fwd_disc_armed);

	/*
	 * The re-arm moves the deadline. A busy clearing that chains to the
	 * fast busy one gets a window of its own, so the reversal is 15 s from
	 * the chain and not from the original clearing.
	 */
	CASE("a re-armed window does not fire on the deadline it replaced");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	cfg.channel[0].busy_tone_s = 1;
	fd_connect();
	rec_cb.failed(0, 0, CALL_FAIL_BUSY);
	uloop_pump(1300);
	view_get(0, &v);
	CHECK(strcmp(v.clearing, "fastbusy") == 0);
	hops_reset();
	uloop_pump(FD_HOLD_EARLY_MS);
	CHECK(!polarity_seen(HAL_POL_REVERSE));

	CASE("it fires 15 s after the chain instead");
	uloop_pump(2000);
	CHECK(polarity_seen(HAL_POL_REVERSE));

	/* ------------------------------------------------- the step out to ROH */

	/*
	 * No case checks that the step into ROH cancels the armed window. The
	 * fixed reorder window (FSM_REORDER_MS, 40 s) outlasts the delayed
	 * reversal hold (LINE_FWD_DISC_HOLD_MS, 15 s), so the reversal always
	 * fires during the reorder (covered by "it fires 15 s after the chain"
	 * above) and no armed window can survive into ROH. The cancel on the ROH
	 * tone frame stays as a defence, but the path cannot be driven here
	 * without a 40 s real-time pump, so it is not exercised by this suite.
	 */

	/* ------------------------------------------ the clear that is not one */

	/*
	 * The line has to be in a clearing. CW_OFFERED keeps its state when the
	 * answered leg clears, because a call still waits for the on-hook or the
	 * flash: the subscriber hears the reorder as the cue, but no clearing
	 * runs, so nothing is reversed and no delayed window is armed. The
	 * reversal would otherwise land before the recall ring that on-hook owes
	 * the waiting caller.
	 */
	CASE("a clear that leaves a call waiting reverses nothing");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	inject_incoming(0, 1, "5552000", "Bob", 0);
	CHECK(state_is(0, "CW_OFFERED"));
	hops_reset();
	inject_far_end_clear(0, 0);
	CHECK(state_is(0, "CW_OFFERED"));
	CHECK(!polarity_seen(HAL_POL_REVERSE));
	view_get(0, &v);
	CHECK(!v.fwd_disc_armed);

	CASE("and the subscriber hears the reorder as the cue");
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);

	/* the waiting caller gives up too: the reorder runs on into the fast
	 * busy clearing, which is a treatment like any other and arms the
	 * delayed window, and the immediate reversal still never fires */
	CASE("and the clearing that follows arms the window without a reversal");
	hops_reset();
	rec_cb.remote_cancel(0, 1);
	CHECK(state_is(0, "CLEARING"));
	view_get(0, &v);
	CHECK(strcmp(v.clearing, "fastbusy") == 0);
	CHECK(v.fwd_disc_armed);
	CHECK(!polarity_seen(HAL_POL_REVERSE));

	CASE("a clear the machine ignores outright reverses nothing");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	hops_reset();
	inject_far_end_clear(0, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK(!polarity_seen(HAL_POL_REVERSE));

	/*
	 * Only the leg facing the phone. A clearing owns no active slot, so a
	 * BYE that arrives for a leg the line already gave up on names a slot
	 * the machine is not talking on. Without the slot test it would reverse
	 * a line whose call failed locally, which is the same defect the quiet
	 * class case above guards from the other side.
	 */
	CASE("a clear for a leg the clearing already released reverses nothing");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	rec_cb.failed(0, 0, CALL_FAIL_QUIET);
	CHECK(state_is(0, "CLEARING"));
	hops_reset();
	inject_far_end_clear(0, 0);
	CHECK(state_is(0, "CLEARING"));
	CHECK(!polarity_seen(HAL_POL_REVERSE));

	/* ------------------------------------------------- derive and reload */

	/* the removal form and its derive-time band fault live in
	 * t_fwd_disc_remove() below */

	CASE("a reload cannot turn the feature off under a running reversal");
	fd_cfg(600000, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	inject_far_end_clear(0, 0);
	CHECK(polarity_seen(HAL_POL_REVERSE));
	cfg.channel[0].forward_disconnect_battery = CFG_FWD_DISC_BATT_NONE;
	hops_reset();
	lines_reload(&cfg);
	view_get(0, &v);
	CHECK(v.fwd_disc_reversed);

	CASE("and the rest state still puts that reversal back");
	hops_reset();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK(hop_has(H_POLARITY));
	CHECK_UINT(hop_last(H_POLARITY)->val, HAL_POL_FORWARD);

	CASE("shutdown restores a reversal the timer never reached");
	fd_cfg(600000, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	inject_far_end_clear(0, 0);
	CHECK(polarity_seen(HAL_POL_REVERSE));
	hops_reset();
	lines_done();
	lines_live = false;
	CHECK(hop_has(H_POLARITY));
	CHECK_UINT(hop_last(H_POLARITY)->val, HAL_POL_FORWARD);

	/* ------------------------------------------ the backend has to carry it */

	/*
	 * A board that cannot reverse the loop says so once per reload, not
	 * once per call: the capability is resolved at the derive and the
	 * reversal path never asks the backend a question it already answered.
	 */
	CASE("a backend that cannot carry it names the loop break for remove");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REMOVE);
	fixture_start();
	fake_cap.features &= ~(uint32_t)HAL_FEAT_POLARITY;
	warn_capture_begin();
	lines_reload(&cfg);
	CHECK_UINT(warn_capture_count("break the line loop"), 1);
	CHECK_UINT(warn_capture_count("reverse the line battery"), 0);
	warn_capture_end();

	CASE("a backend without the polarity feature warns once");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	fixture_start();
	fake_cap.features &= ~(uint32_t)HAL_FEAT_POLARITY;
	warn_capture_begin();
	lines_reload(&cfg);
	CHECK_UINT(warn_capture_count("reverse the line battery"), 1);

	CASE("and it reverses nothing");
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	hops_reset();
	inject_far_end_clear(0, 0);
	CHECK(state_is(0, "CLEARING"));
	CHECK(!hop_has(H_POLARITY));

	CASE("and it arms no delayed window either");
	view_get(0, &v);
	CHECK(!v.fwd_disc_armed);

	CASE("a backend that carries it warns about nothing");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	fixture_start();
	warn_capture_begin();
	lines_reload(&cfg);
	CHECK_UINT(warn_capture_count("reverse the line battery"), 0);

	CASE("and a line the config leaves off warns about nothing either");
	cfg_default();
	fixture_start();
	fake_cap.features &= ~(uint32_t)HAL_FEAT_POLARITY;
	warn_capture_begin();
	lines_reload(&cfg);
	CHECK_UINT(warn_capture_count("reverse the line battery"), 0);
	warn_capture_end();

	/*
	 * The timers under a line outlive nothing. lines_init() zeroes each
	 * line, and both the shell timeouts and the machine's own sip_timers
	 * are linked into lists threaded through that storage, so one left
	 * armed corrupts a list the next fixture walks. ASan is the reader
	 * here: the case is a use-after-free detector, not an assertion.
	 */
	CASE("a re-init over an armed clearing timer leaves no timer behind");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect();
	rec_cb.failed(0, 0, CALL_FAIL_BUSY);
	view_get(0, &v);
	CHECK(v.fwd_disc_armed);
	CHECK(state_is(0, "CLEARING"));
	lines_bring_up();
	CHECK(state_is(0, "IDLE"));
	view_get(0, &v);
	CHECK(!v.fwd_disc_armed);

	CASE("and the clock runs past both windows without touching them");
	hops_reset();
	uloop_pump(2400);
	CHECK(!hop_has(H_POLARITY));
}

/* ------------------------------------------------- the battery removal form */

/*
 * The battery removal form. The remove form breaks the loop where the reverse
 * form turns it around, and the DSP hands this daemon a classified hook
 * decision rather than an edge, so it reports our own break back as a FLASH
 * or as a RELEASE. The window is therefore two rules: the line state the
 * backend programs, and a hook intake that drops every class for the removal
 * and for the settle behind the restore.
 *
 * The settle is what costs real clock here (see "The clock is real" above). One
 * straddle of LINE_FWD_DISC_SETTLE_MS pins both halves of it: nothing may be
 * re-derived inside the early window, and exactly one hook read must have
 * landed by the end of the late one, which is also what a 200 ms settle would
 * fail. The held-leg case pays the 15 s delayed window once, because a far-end
 * clear can never leave a held leg in CLEARING: TWO_CALLS resumes it instead.
 */
#define FD_SETTLE_EARLY_MS	1600
#define FD_SETTLE_LATE_MS	900
/* LINE_FWD_DISC_HOLD_MS plus a margin, for the one held-leg fixture, and the
 * removal that fixture runs: the window opens at 15 s, the battery comes back
 * at 17 s and the settle ends at 19 s, so the second pump straddles all three */
#define FD_HOLD_PAST_MS		15200
#define FD_HELD_DELAY_MS	2000
#define FD_HELD_SETTLE_MS	4200
/* LINE_FWD_DISC_RETRY_MS plus a margin, and the settle straddle behind a retry
 * that landed 400 ms into the wait above */
#define FD_RETRY_WAIT_MS	1200
#define FD_RETRY_SETTLE_EARLY_MS	1200
#define FD_RETRY_SETTLE_LATE_MS		800

static unsigned int line_state_count(enum hal_line_state state)
{
	unsigned int i, n = 0;

	for (i = 0; i < nhops; i++) {
		if (hops[i].kind == H_LINE_STATE && hops[i].val == (unsigned int)state)
			n++;
	}

	return n;
}

/*
 * The one shape that puts a held leg under a clearing: the subscriber holds an
 * answered call, dials a second one from HOLD_DIAL and that call fails. The
 * far-end clear cannot build it, because TWO_CALLS answers the death of the
 * active leg by resuming the held one.
 */
static void fd_connect_held_clearing(void)
{
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	inject_hook(0, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(0, 0, true);
	inject_digit(0, '5', false);
	rec_cb.failed(0, 1, CALL_FAIL_BUSY);
	hops_reset();
	cops_reset();
}

static void t_fwd_disc_remove(void)
{
	struct line_view v;
	unsigned int queries;

	CASE("battery remove takes the line down at the far end clear");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REMOVE);
	fd_connect();
	inject_far_end_clear(0, 0);
	CHECK(state_is(0, "CLEARING"));
	CHECK_UINT(line_state_count(HAL_LINE_DOWN), 1);
	view_get(0, &v);
	CHECK(v.fwd_disc_down);

	CASE("and it reverses nothing while it does");
	CHECK(!hop_has(H_POLARITY));
	CHECK(!v.fwd_disc_reversed);

	/* the quiet clearing a far-end clear lands in chains to the fast busy
	 * one after 2 s, and that class is a delayed trigger: the episode rule
	 * is what keeps the same call from being announced twice. The window
	 * here is long, so the chain is observed with the battery still off */
	CASE("the chain behind it takes the line down only once");
	fd_cfg(600000, CFG_FWD_DISC_BATT_REMOVE);
	fd_connect();
	inject_far_end_clear(0, 0);
	uloop_pump(2400);
	view_get(0, &v);
	CHECK(strcmp(v.clearing, "fastbusy") == 0);
	CHECK_UINT(line_state_count(HAL_LINE_DOWN), 1);
	CHECK(!v.fwd_disc_armed);
	CHECK(v.fwd_disc_down);

	CASE("the restore brings the battery back when the removal window ends");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REMOVE);
	fd_connect();
	inject_far_end_clear(0, 0);
	hops_reset();
	uloop_pump(FD_DELAY_WAIT_MS);
	CHECK_UINT(line_state_count(HAL_LINE_ACTIVE), 1);
	CHECK_UINT(line_state_count(HAL_LINE_DOWN), 0);

	CASE("and the suppression outlives that restore");
	view_get(0, &v);
	CHECK(v.fwd_disc_down);
	cops_reset();
	hops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(state_is(0, "CLEARING"));
	CHECK_UINT(nhops, 0);
	CHECK_UINT(ncops, 0);

	/* ----------------------------------------- the suppression, class by class */

	CASE("a flash inside the removal window never reaches the machine");
	fd_cfg(FD_HELD_DELAY_MS, CFG_FWD_DISC_BATT_REMOVE);
	fd_connect_held_clearing();
	uloop_pump(FD_HOLD_PAST_MS);
	CHECK(state_is(0, "CLEARING"));
	view_get(0, &v);
	CHECK(v.fwd_disc_down);
	cops_reset();
	hops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(state_is(0, "CLEARING"));
	CHECK_UINT(nhops, 0);
	CHECK_UINT(ncops, 0);

	CASE("so the held leg is neither resumed nor hung up");
	CHECK(!cop_has(C_RESUME));
	CHECK(!cop_has(C_HANGUP));

	CASE("a seize inside it arms no detector");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(state_is(0, "CLEARING"));
	CHECK(!hop_has(H_CHAN_CFG));
	CHECK_UINT(nhops, 0);

	CASE("a pulse digit inside it reaches nothing either");
	inject_hook(0, HAL_HOOK_PULSE, 5);
	CHECK(state_is(0, "CLEARING"));
	CHECK_UINT(nhops, 0);
	CHECK_UINT(ncops, 0);

	CASE("and the on hook that would end the call is dropped with them");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "CLEARING"));
	CHECK(!cop_has(C_HANGUP));
	CHECK_UINT(nhops, 0);
	CHECK_UINT(ncops, 0);

	/*
	 * The orphaned settle. A digit is not a hook report, so it passes the
	 * suppression and brings the line to the settle point. A delayed arm
	 * that did not mark the episode as signalled would re-arm the window
	 * there and take the battery off a second time while the first settle
	 * was still armed, and that settle would lift the suppression over a
	 * dead loop with nothing left to command it back.
	 */
	CASE("a digit inside the clearing re-arms no second window");
	inject_digit(0, '5', false);
	CHECK(state_is(0, "CLEARING"));
	view_get(0, &v);
	CHECK(!v.fwd_disc_armed);
	/* the window that is already open is still the only one: the recorder
	 * was cleared above, so a second removal would show as a new record */
	CHECK(v.fwd_disc_down);
	CHECK_UINT(line_state_count(HAL_LINE_DOWN), 0);

	/*
	 * And the settle is where that on-hook is answered. The query reads the
	 * handset the subscriber put down inside the window, the release takes
	 * the ordinary intake, and CLEARING parks the held leg with the recall
	 * ring.
	 */
	CASE("the settle reads that on hook and parks the held leg");
	cops_reset();
	uloop_pump(FD_HELD_SETTLE_MS);
	CHECK(state_is(0, "HOLDING"));
	view_get(0, &v);
	CHECK(!v.fwd_disc_down);

	CASE("and the held leg rings back on the restored loop, not hung up");
	CHECK(!cop_has(C_HANGUP));
	CHECK(hop_has(H_RING_START));
	CHECK(hop_index(H_LINE_STATE) >= 0 &&
	      hop_index(H_LINE_STATE) < hop_index(H_RING_START));

	CASE("and nothing took the battery off a second time");
	CHECK_UINT(line_state_count(HAL_LINE_DOWN), 0);
	CHECK_UINT(line_state_count(HAL_LINE_ACTIVE), 1);

	/* --------------------------------------------------------- the settle */

	CASE("nothing is re-derived before the settle expires");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REMOVE);
	fd_connect();
	boot_offhook[0] = true;
	inject_far_end_clear(0, 0);
	queries = hook_query_calls[0];
	hops_reset();
	uloop_pump(FD_SETTLE_EARLY_MS);
	CHECK_UINT(hook_query_calls[0], queries);
	CHECK(!hop_has(H_CHAN_CFG));
	view_get(0, &v);
	CHECK(v.fwd_disc_down);

	CASE("and exactly one hook read lands once it does");
	uloop_pump(FD_SETTLE_LATE_MS);
	CHECK_UINT(hook_query_calls[0], queries + 1);
	view_get(0, &v);
	CHECK(!v.fwd_disc_down);

	/* the injected seize takes the ordinary intake, which is what arms the
	 * digit detector: in CLEARING the matrix itself ignores it */
	CASE("an off hook line seizes through the real intake at the settle");
	CHECK_UINT(hop_count(H_CHAN_CFG), 1);

	/*
	 * The other direction of the same read. The subscriber's on-hook was
	 * dropped inside the window, so the machine still believes the handset
	 * is up; the settle reads the cradle and injects the release the
	 * suppression swallowed. Without that arm the line would announce a
	 * clear into a dead handset until some later hook edge corrected it.
	 */
	CASE("an on hook answer at the settle releases the line");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REMOVE);
	fd_connect();
	inject_far_end_clear(0, 0);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "CLEARING"));
	queries = hook_query_calls[0];
	hops_reset();
	warn_capture_begin();
	uloop_pump(FD_SETTLE_EARLY_MS + FD_SETTLE_LATE_MS);
	/* the positive control of the two boot and reload pins above: this is
	 * the log line those cases require to be absent */
	CHECK_UINT(warn_capture_count("releasing"), 1);
	warn_capture_end();
	CHECK_UINT(hook_query_calls[0], queries + 1);
	view_get(0, &v);
	CHECK(!v.fwd_disc_down);

	CASE("and the line the suppressed on hook left in CLEARING reaches IDLE");
	CHECK(state_is(0, "IDLE"));
	CHECK(!hop_has(H_CHAN_CFG));

	/* ------------------------------------------- one window at a time */

	/*
	 * A repeated clear changes nothing, and three rules stand in its way in
	 * this order: the slot narrowing of cb_remote_bye(), because a clearing
	 * owns no active slot, then the once-per-episode rule, and only then the
	 * guard on an open window. That guard is a stated invariant like the bell
	 * and the caller id record beside it: with the episode marked by either
	 * arm, no path of the matrix arms a second signal inside a window.
	 */
	CASE("a second far end clear inside the window opens no second removal");
	fd_cfg(600000, CFG_FWD_DISC_BATT_REMOVE);
	fd_connect();
	inject_far_end_clear(0, 0);
	CHECK_UINT(line_state_count(HAL_LINE_DOWN), 1);
	inject_far_end_clear(0, 0);
	inject_far_end_clear(0, 1);
	CHECK(state_is(0, "CLEARING"));
	CHECK_UINT(line_state_count(HAL_LINE_DOWN), 1);

	/*
	 * The diagnostic verbs take the same intake, so the window drops them
	 * exactly as it drops the DSP's own reports. They still answer, because
	 * the verb reports what it injected and not what the machine did with
	 * it; the fwd_disc_down key of the status dump is where an operator
	 * sees why nothing moved.
	 */
	CASE("the diagnostic verbs are dropped inside the window and still answer");
	hops_reset();
	cops_reset();
	CHECK_UINT(line_inject_release(0), LINE_INJECT_OK);
	CHECK(state_is(0, "CLEARING"));
	CHECK_UINT(ncops, 0);
	CHECK_UINT(line_inject_seize(0), LINE_INJECT_OK);
	CHECK(state_is(0, "CLEARING"));
	CHECK_UINT(nhops, 0);

	/* ------------------------------------------ the restore the backend refuses */

	/*
	 * A refused restore leaves a line with no battery, which is the one
	 * outcome this whole unit must not produce: nothing else in the daemon
	 * ever commands the line state, so the retry is the only way back. The
	 * suppression is lifted while it runs, because no hook report can arrive
	 * on a dead loop and holding the intake shut would hide the subscriber
	 * from the moment it comes back.
	 */
	CASE("a refused restore lifts the suppression and derives nothing");
	fd_cfg(FD_DELAY_MS, CFG_FWD_DISC_BATT_REMOVE);
	fd_connect();
	inject_far_end_clear(0, 0);
	queries = hook_query_calls[0];
	hops_reset();
	line_state_fail_n = 1;
	uloop_pump(FD_DELAY_WAIT_MS);
	CHECK_UINT(line_state_count(HAL_LINE_ACTIVE), 1);
	CHECK_UINT(hook_query_calls[0], queries);
	view_get(0, &v);
	CHECK(!v.fwd_disc_down);
	CHECK(state_is(0, "CLEARING"));

	CASE("and the retry behind it brings the battery back");
	hops_reset();
	uloop_pump(FD_RETRY_WAIT_MS);
	CHECK_UINT(line_state_count(HAL_LINE_ACTIVE), 1);
	view_get(0, &v);
	CHECK(v.fwd_disc_down);
	CHECK_UINT(hook_query_calls[0], queries);

	/* the settle measures the debounce from the ACTIVE that succeeded, so a
	 * restore that took two attempts moved the whole window with it */
	/* the straddle is measured from the retry that landed, which is 400 ms
	 * behind the pump above: 1.2 s more is still inside the 2 s settle, and
	 * 800 ms after that is past it */
	CASE("and the full settle runs from that attempt, not the first one");
	uloop_pump(FD_RETRY_SETTLE_EARLY_MS);
	CHECK_UINT(hook_query_calls[0], queries);
	uloop_pump(FD_RETRY_SETTLE_LATE_MS);
	CHECK_UINT(hook_query_calls[0], queries + 1);
	CHECK(state_is(0, "IDLE"));

	CASE("a reload that disables the line inside a window leaves it down");
	fd_cfg(600000, CFG_FWD_DISC_BATT_REMOVE);
	fd_connect();
	inject_far_end_clear(0, 0);
	hops_reset();
	/*
	 * The window owed the battery back, but the config now wants the port
	 * without one: no abort on the path restores it, and the removal the
	 * window made is the state the config wants, so nothing is written.
	 */
	cfg.channel[0].enabled = false;
	lines_reload(&cfg);
	CHECK(state_is(0, "OOS"));
	CHECK(!hop_has(H_LINE_STATE));
	view_get(0, &v);
	CHECK(!v.fwd_disc_down);

	CASE("and nothing restores it once the window would have ended");
	hops_reset();
	uloop_pump(FD_RETRY_WAIT_MS);
	CHECK(!hop_has(H_LINE_STATE));

	CASE("and the config that enables it again raises the battery first");
	hops_reset();
	cfg.channel[0].enabled = true;
	lines_reload(&cfg);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(line_state_count(HAL_LINE_ACTIVE), 1);
	CHECK_UINT(line_state_count(HAL_LINE_DOWN), 0);
	CHECK(hop_index(H_LINE_STATE) >= 0);

	CASE("a removal for good the backend refuses is asked again");
	fd_cfg(600000, CFG_FWD_DISC_BATT_REMOVE);
	fd_connect();
	hops_reset();
	/* an answered call is torn down and the port silenced before the
	 * battery goes, so a handset in the call hears the clear over a fed
	 * loop; the refused removal is retried until it lands */
	line_state_fail_n = 1;
	cfg.channel[0].enabled = false;
	lines_reload(&cfg);
	CHECK(state_is(0, "OOS"));
	CHECK_UINT(line_state_count(HAL_LINE_ACTIVE), 0);
	CHECK_UINT(line_state_count(HAL_LINE_DOWN), 1);
	CHECK(hop_index(H_TONE_STOP) >= 0);
	CHECK(hop_index(H_LINE_STATE) > hop_index(H_TONE_STOP));
	hops_reset();
	uloop_pump(FD_RETRY_WAIT_MS);
	CHECK_UINT(line_state_count(HAL_LINE_ACTIVE), 0);
	CHECK_UINT(line_state_count(HAL_LINE_DOWN), 1);

	CASE("and nothing retries once it has landed");
	hops_reset();
	uloop_pump(FD_RETRY_WAIT_MS);
	CHECK(!hop_has(H_LINE_STATE));

	CASE("a shutdown that cannot restore it tries once more and stops");
	fd_cfg(600000, CFG_FWD_DISC_BATT_REMOVE);
	fd_connect();
	inject_far_end_clear(0, 0);
	hops_reset();
	/* the force, the drain inside it and then line_timers_cancel(), which is
	 * the leaving one: it makes the last attempt and cancels the retry, so
	 * exactly three of the four refusals are consumed and none is left owed */
	line_state_fail_n = 4;
	lines_done();
	lines_live = false;
	CHECK_UINT(line_state_count(HAL_LINE_ACTIVE), 3);
	CHECK_UINT(line_state_fail_n, 1);

	CASE("and no retry outlives the daemon");
	line_state_fail_n = 0;
	hops_reset();
	uloop_pump(FD_RETRY_WAIT_MS);
	CHECK(!hop_has(H_LINE_STATE));

	/* ---------------------------------------------------- the abort paths */

	CASE("a reload that takes the line out of service keeps the battery off");
	fd_cfg(600000, CFG_FWD_DISC_BATT_REMOVE);
	fd_connect();
	inject_far_end_clear(0, 0);
	queries = hook_query_calls[0];
	hops_reset();
	cfg.channel[0].enabled = false;
	lines_reload(&cfg);
	CHECK(state_is(0, "OOS"));
	CHECK(!hop_has(H_LINE_STATE));
	view_get(0, &v);
	CHECK(!v.fwd_disc_down);

	CASE("and a line leaving service re-derives no hook state");
	CHECK_UINT(hook_query_calls[0], queries);

	CASE("and both removal timers went with the window");
	hops_reset();
	uloop_pump(FD_DELAY_WAIT_MS);
	CHECK(!hop_has(H_LINE_STATE));

	CASE("shutdown puts back a battery the removal never restored");
	fd_cfg(600000, CFG_FWD_DISC_BATT_REMOVE);
	fd_connect();
	inject_far_end_clear(0, 0);
	queries = hook_query_calls[0];
	hops_reset();
	lines_done();
	lines_live = false;
	CHECK_UINT(line_state_count(HAL_LINE_ACTIVE), 1);

	CASE("and a daemon that is stopping re-derives nothing either");
	CHECK_UINT(hook_query_calls[0], queries);

	CASE("a re-init over an open removal window puts the battery back");
	fd_cfg(600000, CFG_FWD_DISC_BATT_REMOVE);
	fd_connect();
	inject_far_end_clear(0, 0);
	hops_reset();
	lines_bring_up();
	CHECK(state_is(0, "IDLE"));
	CHECK(line_state_count(HAL_LINE_ACTIVE) >= 1);
	view_get(0, &v);
	CHECK(!v.fwd_disc_down);

	CASE("and the clock runs past both its windows without touching it");
	hops_reset();
	uloop_pump(FD_DELAY_WAIT_MS);
	CHECK(!hop_has(H_LINE_STATE));

	/* ------------------------------------------------ the backend refuses */

	CASE("a removal the backend refuses opens no window");
	fd_cfg(600000, CFG_FWD_DISC_BATT_REMOVE);
	fd_connect();
	line_state_status = HAL_FATAL;
	inject_far_end_clear(0, 0);
	line_state_status = HAL_OK;
	CHECK_UINT(line_state_count(HAL_LINE_DOWN), 1);
	view_get(0, &v);
	CHECK(!v.fwd_disc_down);

	CASE("and it drops no hook event");
	hops_reset();
	cops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(state_is(0, "OFFHOOK_DIAL"));

	/* -------------------------------------------------- the derive faults */

	CASE("a removal delay inside the flash band warns once");
	fd_cfg(750, CFG_FWD_DISC_BATT_REMOVE);
	warn_capture_begin();
	fixture_start();
	CHECK_UINT(warn_capture_count("flash band"), 1);

	CASE("a removal delay at or above the release window warns once too");
	fd_cfg(2000, CFG_FWD_DISC_BATT_REMOVE);
	warn_capture_begin();
	fixture_start();
	CHECK_UINT(warn_capture_count("release window"), 1);

	CASE("a removal delay below the flash window warns about nothing");
	fd_cfg(250, CFG_FWD_DISC_BATT_REMOVE);
	warn_capture_begin();
	fixture_start();
	CHECK_UINT(warn_capture_count("forward_disconnect_delay"), 0);

	/* the band is the region's, so a read-back that moves the flash window
	 * moves it and the fault with it: the same 250 ms now falls inside the
	 * flash window, and the settle behind the load reports it */
	CASE("the band moves with the region's hook flash window");
	fd_cfg(250, CFG_FWD_DISC_BATT_REMOVE);
	fake_region.hook_flash_min_ms = 200;
	warn_capture_begin();
	fixture_start();
	region_ready();
	CHECK_UINT(warn_capture_count("flash band"), 1);

	CASE("the band belongs to the removal, not to the reversal");
	fd_cfg(750, CFG_FWD_DISC_BATT_REVERSE);
	warn_capture_begin();
	fixture_start();
	CHECK_UINT(warn_capture_count("forward_disconnect_delay"), 0);

	CASE("a delay the pair leaves off warns about nothing");
	fd_cfg(0, CFG_FWD_DISC_BATT_REMOVE);
	warn_capture_begin();
	fixture_start();
	CHECK_UINT(warn_capture_count("forward_disconnect_delay"), 0);

	CASE("and battery none warns about nothing either");
	fd_cfg(750, CFG_FWD_DISC_BATT_NONE);
	warn_capture_begin();
	fixture_start();
	CHECK_UINT(warn_capture_count("forward_disconnect_delay"), 0);
	warn_capture_end();

	CASE("and battery none takes no line down");
	fd_cfg(750, CFG_FWD_DISC_BATT_NONE);
	fd_connect();
	inject_far_end_clear(0, 0);
	CHECK(state_is(0, "CLEARING"));
	CHECK(!hop_has(H_LINE_STATE));
}

/*
 * The on-hook that parks a held call out of a reversed clearing. The recall
 * ring starts inside the CLEARING RELEASE cell, so the forward polarity must
 * precede the first ring op and not fall in the cadence. The park is also
 * the end of the episode, so the next clearing of the same session signals
 * again.
 */
static void t_fwd_disc_held(void)
{
	struct line_view v;

	CASE("a held call parked out of a reversed clearing gets the loop back "
	     "before the ring");
	fd_cfg(600000, CFG_FWD_DISC_BATT_REVERSE);
	fd_connect_held_clearing();
	uloop_pump(FD_HOLD_PAST_MS);
	CHECK(state_is(0, "CLEARING"));
	CHECK(polarity_seen(HAL_POL_REVERSE));
	hops_reset();
	cops_reset();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "HOLDING"));
	CHECK(!cop_has(C_HANGUP));
	CHECK_UINT(hop_find(H_POLARITY)->val, HAL_POL_FORWARD);
	CHECK(hop_index(H_POLARITY) >= 0 &&
	      hop_index(H_POLARITY) < hop_index(H_RING_START));
	view_get(0, &v);
	CHECK(!v.fwd_disc_reversed);

	CASE("and a later clearing of the same session reverses again");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	rec_cb.reinvite_done(0, 0, true);
	CHECK(state_is(0, "CONNECTED"));
	inject_hook(0, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(0, 0, true);
	inject_digit(0, '5', false);
	rec_cb.failed(0, 1, CALL_FAIL_BUSY);
	CHECK(state_is(0, "CLEARING"));
	hops_reset();
	uloop_pump(FD_HOLD_PAST_MS);
	CHECK(polarity_seen(HAL_POL_REVERSE));
}

/* ----------------------------------------- the hook resync behind a gap */

/*
 * The third reader of the same query (line/line.h lines_hook_resync). The event
 * pump loses records under load. A lost HAL_HOOK_RELEASE then leaves the
 * machine off hook and the line answers every caller busy. The dispatch
 * reports the gap and this pass reads the hardware again.
 *
 * The knobs are the ones the two seize groups use, and the recorder counts
 * every query. A case therefore pins three things: the read that must happen,
 * the read that must not, and the injection each answer earns.
 *
 * The group sits here and runs beside those two groups. Three of the states
 * it skips are built by fixtures above it:
 *
 * - the recall ring of a held call;
 * - the battery removal window;
 * - a metallic line test with a consumer to answer.
 */

/*
 * The pass is armed with 0 ms, so any pump carries it. The pump below is
 * short enough that a case can then straddle the confirm window of its own.
 * The confirm is 1500 ms behind the pass. The two pumps straddle it: the
 * early one ends at 1300 ms after the trigger and the late one at 1900 ms.
 *
 * The floor pair belongs to the case that raises the flash window of the
 * region to RESYNC_FLASH_MAX_MS. That puts the confirm at 2200 ms, so 2000 ms
 * is short of it and 2600 ms is past it.
 *
 * The postpone pump covers LINE_HOOK_RESYNC_MIN_MS with a margin, because a
 * metallic line test moves the whole pass by that value.
 */
#define RESYNC_PASS_PUMP_MS		100
#define RESYNC_CONFIRM_EARLY_MS		1200
#define RESYNC_CONFIRM_LATE_MS		600
#define RESYNC_FLASH_MAX_MS		2000
#define RESYNC_FLOOR_EARLY_MS		1900
#define RESYNC_FLOOR_LATE_MS		600
#define RESYNC_POSTPONE_PUMP_MS		5400

/*
 * The gap itself. event_seq_check() moves the counter and then asks the
 * shell. A case that asked with the counter still where it was would drive an
 * order the daemon never produces.
 */
static void gap_report(void)
{
	uv.events_lost++;
	lines_hook_resync();
}

/* the connected line most cases start from, with the query answering on hook:
 * the shape a lost release leaves behind */
static void resync_connect(void)
{
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map),
		 "5");
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	CHECK(state_is(0, "CONNECTED"));
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	hops_reset();
	cops_reset();
}

static void t_hook_resync(void)
{
	struct line_test_answer answer = {};
	struct line_view v;

	CASE("a lost on-hook report is healed by the resync behind the event "
	     "gap");
	resync_connect();
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	CHECK_UINT(hook_query_calls[0], 1);
	CHECK(state_is(0, "CONNECTED"));
	uloop_pump(RESYNC_CONFIRM_EARLY_MS + RESYNC_CONFIRM_LATE_MS);
	CHECK_UINT(hook_query_calls[0], 2);
	CHECK(state_is(0, "IDLE"));
	CHECK(cop_has(C_HANGUP));

	/* the pass walks the whole board, so a stuck line the fixture does not
	 * start from is read and healed like the first one */
	CASE("the pass reads every line, so the second one is healed too");
	cfg_default();
	cfg.channel[1].enabled = true;
	snprintf(cfg.channel[1].digit_map, sizeof(cfg.channel[1].digit_map),
		 "5");
	fixture_start();
	region_ready();
	inject_hook(1, HAL_HOOK_SEIZE, 0);
	inject_digit(1, '5', false);
	rec_cb.answered(1, 0);
	CHECK(state_is(1, "CONNECTED"));
	CHECK(state_is(0, "IDLE"));
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	cops_reset();
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	CHECK_UINT(hook_query_calls[0], 1);
	CHECK_UINT(hook_query_calls[1], 1);
	CHECK(state_is(1, "CONNECTED"));
	uloop_pump(RESYNC_CONFIRM_EARLY_MS + RESYNC_CONFIRM_LATE_MS);
	CHECK_UINT(hook_query_calls[1], 2);
	CHECK(state_is(1, "IDLE"));
	CHECK(state_is(0, "IDLE"));

	CASE("an event gap with the hook unchanged injects nothing");
	cfg_default();
	fixture_start();
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	hops_reset();
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	CHECK_UINT(hook_query_calls[0], 1);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(hop_count(H_CHAN_CFG), 0);
	/* the release arm armed nothing either, so no second reading follows */
	uloop_pump(RESYNC_CONFIRM_EARLY_MS + RESYNC_CONFIRM_LATE_MS);
	CHECK_UINT(hook_query_calls[0], 1);

	/* event_read() drains the socket in one loop. A query made inside the
	 * gap report would read the hook in front of the records that gap is
	 * queued behind. */
	CASE("the resync waits one loop pass, so the records behind the gap "
	     "land first");
	cfg_default();
	fixture_start();
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	gap_report();
	CHECK_UINT(hook_query_calls[0], 0);
	uloop_pump(RESYNC_PASS_PUMP_MS);
	CHECK_UINT(hook_query_calls[0], 1);

	CASE("a line at IDLE that reads off hook is seized by the resync");
	cfg_default();
	fixture_start();
	region_ready();
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	hops_reset();
	boot_offhook[0] = true;
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	CHECK_UINT(hook_query_calls[0], 1);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	/* the injected seize took the ordinary intake, which is what arms the
	 * digit detector */
	CHECK(hop_has(H_CHAN_CFG));
	CHECK_UINT(hop_find(H_CHAN_CFG)->chan.detect_mask,
		   HAL_DETECT_DTMF | HAL_DETECT_PULSE | HAL_DETECT_MODEM);
	boot_offhook[0] = false;
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	/* the query inside a ring cadence has no bench reading, so a ringing
	 * line is left alone whatever the backend answers */
	CASE("a ringing line that reads off hook is left alone");
	cfg_default();
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "5551000", "Alice", 0);
	CHECK(state_is(0, "RINGING"));
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	hops_reset();
	cops_reset();
	boot_offhook[0] = true;
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	CHECK_UINT(hook_query_calls[0], 1);
	CHECK(state_is(0, "RINGING"));
	CHECK(!cop_has(C_ANSWER));
	CHECK_UINT(hop_count(H_CHAN_CFG), 0);
	boot_offhook[0] = false;

	/* holding_enter() starts the bell, so l->ringing covers the recall ring
	 * of a held call and the same skip holds */
	CASE("a held call with its recall ring that reads off hook is left "
	     "alone too");
	cfg_default();
	recall_fixture();
	recall_two_calls_reach();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "HOLDING"));
	view_get(0, &v);
	CHECK(v.ringing);
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	hops_reset();
	cops_reset();
	boot_offhook[0] = true;
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	CHECK_UINT(hook_query_calls[0], 1);
	CHECK(state_is(0, "HOLDING"));
	CHECK(!cop_has(C_RESUME));
	CHECK_UINT(hop_count(H_CHAN_CFG), 0);
	boot_offhook[0] = false;

	CASE("the release arm waits for a second reading before it injects");
	resync_connect();
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	uloop_pump(RESYNC_CONFIRM_EARLY_MS);
	CHECK_UINT(hook_query_calls[0], 1);
	CHECK(state_is(0, "CONNECTED"));
	uloop_pump(RESYNC_CONFIRM_LATE_MS);
	CHECK_UINT(hook_query_calls[0], 2);
	CHECK(state_is(0, "IDLE"));

	/* a flash breaks the loop open in normal use, so the reading in front
	 * of it describes a moment that is gone. The drop service of the region
	 * holds the call and waits for the far end. That is the state the
	 * release the arm abandoned would have cleared. */
	CASE("a hook event between the two readings abandons the release");
	resync_connect();
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	inject_hook(0, HAL_HOOK_FLASH, 0);
	CHECK(state_is(0, "PENDING"));
	uloop_pump(RESYNC_CONFIRM_EARLY_MS + RESYNC_CONFIRM_LATE_MS);
	CHECK_UINT(hook_query_calls[0], 1);
	CHECK(state_is(0, "PENDING"));

	CASE("a digit between the two readings abandons it too");
	resync_connect();
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	inject_digit(0, '5', false);
	uloop_pump(RESYNC_CONFIRM_EARLY_MS + RESYNC_CONFIRM_LATE_MS);
	CHECK_UINT(hook_query_calls[0], 1);
	CHECK(state_is(0, "CONNECTED"));

	/* The far end pressed that key on its own handset, so it says nothing
	 * about this loop. A peer that sends one every second would otherwise
	 * hold the release off for the length of the call. */
	CASE("a digit from the far end between the two readings does not "
	     "abandon the release");
	resync_connect();
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	inject_digit(0, '7', true);
	uloop_pump(RESYNC_CONFIRM_EARLY_MS + RESYNC_CONFIRM_LATE_MS);
	CHECK_UINT(hook_query_calls[0], 2);
	CHECK(state_is(0, "IDLE"));

	/* the pump lost more records while the wait ran, so the first reading
	 * is older than the state the line is in now */
	CASE("a second gap between the two readings starts the pass again");
	resync_connect();
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	uv.events_lost++;
	uloop_pump(RESYNC_CONFIRM_EARLY_MS + RESYNC_CONFIRM_LATE_MS);
	CHECK_UINT(hook_query_calls[0], 2);
	CHECK(state_is(0, "CONNECTED"));
	uloop_pump(RESYNC_CONFIRM_EARLY_MS + RESYNC_CONFIRM_LATE_MS);
	CHECK_UINT(hook_query_calls[0], 3);
	CHECK(state_is(0, "IDLE"));

	CASE("a second reading that says off hook injects nothing");
	resync_connect();
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	boot_offhook[0] = true;
	uloop_pump(RESYNC_CONFIRM_EARLY_MS + RESYNC_CONFIRM_LATE_MS);
	CHECK_UINT(hook_query_calls[0], 2);
	CHECK(state_is(0, "CONNECTED"));
	boot_offhook[0] = false;

	/* line 1 is the disabled channel of the fixture config */
	CASE("an out of service line is not read at all");
	cfg_default();
	fixture_start();
	CHECK(state_is(1, "OOS"));
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	boot_offhook[1] = true;
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	CHECK_UINT(hook_query_calls[1], 0);
	CHECK(state_is(1, "OOS"));
	boot_offhook[1] = false;

	CASE("a line inside the battery removal window is not read at all");
	fd_cfg(600000, CFG_FWD_DISC_BATT_REMOVE);
	fd_connect();
	rec_cb.remote_bye(0, 0);
	CHECK(state_is(0, "CLEARING"));
	view_get(0, &v);
	CHECK(v.fwd_disc_down);
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	CHECK_UINT(hook_query_calls[0], 0);
	CHECK(state_is(0, "CLEARING"));

	CASE("a line the loop test holds is not read at all");
	cfg_default();
	fixture_start();
	region_ready();
	CHECK_UINT(line_loop_test_set(true, true, LINE_LOOP_TEST_RING_S_DEFAULT,
				      LINE_LOOP_TEST_DELAY_DEFAULT_MS, NULL),
		   LINE_LOOP_TEST_OK);
	view_get(0, &v);
	CHECK(v.loop_test_set);
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	boot_offhook[0] = true;
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	CHECK_UINT(hook_query_calls[0], 0);
	CHECK(state_is(0, "OOS"));
	boot_offhook[0] = false;
	CHECK_UINT(line_loop_test_set(false, false,
				      LINE_LOOP_TEST_RING_S_DEFAULT,
				      LINE_LOOP_TEST_DELAY_DEFAULT_MS, NULL),
		   LINE_LOOP_TEST_OK);

	/* a metallic test blocks the hook sample of every port, so the pass
	 * waits for it instead of reading a part the test owns */
	CASE("a metallic line test postpones the whole pass");
	cfg_default();
	fixture_start();
	region_ready();
	CHECK_UINT(line_test_query(1, HAL_LINE_TEST_ROH,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_OK);
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	CHECK_UINT(hook_query_calls[0], 0);
	line_test_drop(1);
	uloop_pump(RESYNC_POSTPONE_PUMP_MS);
	CHECK_UINT(hook_query_calls[0], 1);

	/* The pass the first gap asked for reads the same hardware. A pump that
	 * is already in trouble must not be asked once per lost record. */
	CASE("a second gap inside the rate limit starts no second pass");
	cfg_default();
	fixture_start();
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	CHECK_UINT(hook_query_calls[0], 1);
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	CHECK_UINT(hook_query_calls[0], 1);

	/*
	 * The line keeps the state nobody read. The recorder answers off hook
	 * while it refuses. A shell that read the answer before the status
	 * would seize this line instead of leaving it alone.
	 */
	CASE("a query the backend refuses injects nothing and leaves the "
	     "state alone");
	cfg_default();
	fixture_start();
	region_ready();
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	hops_reset();
	hook_query_status[0] = HAL_FATAL;
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	CHECK_UINT(hook_query_calls[0], 1);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(hop_count(H_CHAN_CFG), 0);
	memset(hook_query_status, 0, sizeof(hook_query_status));

	/* hal.h section 3, through the same primitive the two seize paths use:
	 * the resync inherits the retry instead of carrying one of its own */
	CASE("a transient refusal inside the pass is retried once, and the "
	     "line heals");
	resync_connect();
	hook_query_transient[0] = 1;
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	CHECK_UINT(hook_query_calls[0], 2);
	CHECK_UINT(hook_query_transient[0], 0);
	CHECK(state_is(0, "CONNECTED"));
	uloop_pump(RESYNC_CONFIRM_EARLY_MS + RESYNC_CONFIRM_LATE_MS);
	CHECK_UINT(hook_query_calls[0], 3);
	CHECK(state_is(0, "IDLE"));

	/* the seize arm of the service entry injects on every off-hook answer,
	 * which arms the detector again; under a call that must not happen */
	CASE("a connected line whose hook still reads off hook keeps its call "
	     "and its detector");
	resync_connect();
	boot_offhook[0] = true;
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	CHECK_UINT(hook_query_calls[0], 1);
	CHECK(state_is(0, "CONNECTED"));
	CHECK_UINT(hop_count(H_CHAN_CFG), 0);
	CHECK(!cop_has(C_HANGUP));
	uloop_pump(RESYNC_CONFIRM_EARLY_MS + RESYNC_CONFIRM_LATE_MS);
	CHECK_UINT(hook_query_calls[0], 1);
	CHECK(state_is(0, "CONNECTED"));
	boot_offhook[0] = false;

	/* two readings inside one flash both answer on hook for a subscriber
	 * who never put the handset down */
	CASE("the confirm wait never drops below the region flash maximum "
	     "plus 200 ms");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map),
		 "5");
	fake_region.hook_flash_max_ms = RESYNC_FLASH_MAX_MS;
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	CHECK(state_is(0, "CONNECTED"));
	memset(hook_query_calls, 0, sizeof(hook_query_calls));
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	uloop_pump(RESYNC_FLOOR_EARLY_MS);
	CHECK_UINT(hook_query_calls[0], 1);
	CHECK(state_is(0, "CONNECTED"));
	uloop_pump(RESYNC_FLOOR_LATE_MS);
	CHECK_UINT(hook_query_calls[0], 2);
	CHECK(state_is(0, "IDLE"));

	/* the daemon stop, with a second reading owed. A timer the stop left
	 * armed would query a backend the teardown is taking away. */
	CASE("the daemon stop drops the pass that waits for a second reading");
	resync_connect();
	gap_report();
	uloop_pump(RESYNC_PASS_PUMP_MS);
	CHECK_UINT(hook_query_calls[0], 1);
	lines_done();
	lines_live = false;
	uloop_pump(RESYNC_CONFIRM_EARLY_MS + RESYNC_CONFIRM_LATE_MS);
	CHECK_UINT(hook_query_calls[0], 1);

	/* the knobs outlive this suite, like the two seize groups above */
	memset(boot_offhook, 0, sizeof(boot_offhook));
	memset(hook_query_status, 0, sizeof(hook_query_status));
	memset(hook_query_transient, 0, sizeof(hook_query_transient));
}

/*
 * The dead line record and the forward disconnect share the OOS force. A line
 * that removed its battery and could not restore it needs a retry, and the
 * force runs that retry through fwd_disc_abort(). A repeat dead line record on
 * a line that is already out must not run the force again. Otherwise it
 * attempts the refused restore again at once. The guard on the OOS entry stops
 * the second run, and an extra HAL_LINE_ACTIVE op shows a lost guard.
 */
static void t_line_down_abort(void)
{
	fd_cfg(600000, CFG_FWD_DISC_BATT_REMOVE);
	fd_connect();
	inject_far_end_clear(0, 0);
	CHECK_UINT(line_state_count(HAL_LINE_DOWN), 1);

	/* the restore the abort attempts is refused, so the loop stays owed and
	 * a second force would attempt it again */
	line_state_fail_n = 5;

	CASE("a dead line records the memory and attempts the owed restore");
	hops_reset();
	inject_line_state(0, HAL_LINE_DOWN);
	CHECK(state_is(0, "OOS"));
	CHECK(line_state_count(HAL_LINE_ACTIVE) >= 1);

	CASE("a repeat dead line does not re-run the force or the restore");
	hops_reset();
	inject_line_state(0, HAL_LINE_DOWN);
	CHECK(state_is(0, "OOS"));
	CHECK(!hop_has(H_LINE_STATE));

	line_state_fail_n = 0;
}

/*
 * The manufacturing loop test (line/line.h line_loop_test_set). The mode holds
 * every port out of service, rings each one while its handset is down and
 * plays the audio path back on an off-hook edge, and this file's recorder is
 * where every one of those ops lands.
 */
static enum line_loop_test_status loop_arm(bool want_self)
{
	return line_loop_test_set(true, want_self,
				  LINE_LOOP_TEST_RING_S_DEFAULT,
				  LINE_LOOP_TEST_DELAY_DEFAULT_MS, NULL);
}

static enum line_loop_test_status loop_disarm(void)
{
	return line_loop_test_set(false, false,
				  LINE_LOOP_TEST_RING_S_DEFAULT,
				  LINE_LOOP_TEST_DELAY_DEFAULT_MS, NULL);
}

static unsigned int hop_count_line(enum hop_kind kind, unsigned int line)
{
	unsigned int i, n = 0;

	for (i = 0; i < nhops; i++) {
		if (hops[i].kind == kind && hops[i].line == line)
			n++;
	}

	return n;
}

/* one battery write for one port, which is what a port the config leaves
 * unpowered records at the arm and again at the disarm */
static unsigned int battery_count_line(unsigned int line,
				       enum hal_line_state state)
{
	unsigned int i, n = 0;

	for (i = 0; i < nhops; i++) {
		if (hops[i].kind != H_LINE_STATE || hops[i].line != line)
			continue;
		if (hops[i].val == (unsigned int)state)
			n++;
	}

	return n;
}

static void t_loop_test(void)
{
	struct call_ended ended = { .inbound = true, .failed = true,
				    .fail_class = CALL_FAIL_BUSY };
	struct line_test_answer answer = {};
	struct line_view v;

	/*
	 * The arm. Both ports leave service whatever the config said about
	 * them, a port the config does not enable is powered for the test,
	 * and each one rings with the window the arm carried.
	 */
	CASE("the arm takes every port out of service and rings it");
	cfg_default();
	cfg.channels = 1;
	fixture_start();
	region_ready();
	hops_reset();
	CHECK_UINT(loop_arm(false), LINE_LOOP_TEST_OK);
	CHECK(state_is(0, "OOS"));
	CHECK(state_is(1, "OOS"));
	view_get(0, &v);
	CHECK(v.oos);
	CHECK(v.loop_test_set);
	view_get(1, &v);
	CHECK(v.oos);
	CHECK(v.loop_test_set);

	CASE("and it powers the port the config leaves unpowered");
	CHECK_UINT(battery_count_line(1, HAL_LINE_ACTIVE), 1);
	CHECK_UINT(battery_count_line(0, HAL_LINE_ACTIVE), 0);

	CASE("and each port rings for the window the arm carried");
	CHECK_UINT(hop_count(H_RING_START), 2);
	CHECK_UINT(hop_count_line(H_RING_START, 0), 1);
	CHECK_UINT(hop_count_line(H_RING_START, 1), 1);
	CHECK_UINT(hop_find(H_RING_START)->ring.duration_ms, 60000);
	CHECK_UINT(hop_last(H_RING_START)->ring.duration_ms, 60000);
	CHECK(!hop_has(H_LOOP));
	view_get(0, &v);
	CHECK(v.loop_test_ringing);
	CHECK(!v.loop_test_offhook);
	CHECK(!v.loop_test_audio);
	CHECK(!strcmp(v.loop_test_mode, "cross"));

	/*
	 * The cross connection. It carries audio only while both handsets are
	 * lifted: the subscriber who lifted first hears silence rather than
	 * the ring voltage of the other port.
	 */
	CASE("a seize on one port of a cross pair stops its bell alone");
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK_UINT(hop_count_line(H_RING_STOP, 0), 1);
	CHECK(!hop_has(H_LOOP));
	view_get(0, &v);
	CHECK(v.loop_test_offhook);
	CHECK(!v.loop_test_ringing);
	CHECK(!v.loop_test_audio);

	CASE("the second seize arms the cross connection on both ports");
	hops_reset();
	inject_hook(1, HAL_HOOK_SEIZE, 0);
	CHECK_UINT(hop_count_line(H_RING_STOP, 1), 1);
	CHECK_UINT(loop_count_line(0, HAL_LOOP_PEER), 1);
	CHECK_UINT(loop_count_line(1, HAL_LOOP_PEER), 1);
	CHECK_UINT(hop_find(H_LOOP)->loop.peer, 1);
	CHECK_UINT(hop_last(H_LOOP)->loop.peer, 0);
	/* the cross connection reads no delay: it is a conversation between
	 * two ports and a delay in it would be an echo */
	CHECK_UINT(hop_find(H_LOOP)->loop.delay_ms, 0);
	view_get(0, &v);
	CHECK(v.loop_test_audio);
	view_get(1, &v);
	CHECK(v.loop_test_audio);

	CASE("the first release takes the audio off both ports and rings one");
	hops_reset();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK_UINT(loop_count_line(0, HAL_LOOP_OFF), 1);
	CHECK_UINT(loop_count_line(1, HAL_LOOP_OFF), 1);
	CHECK_UINT(hop_count(H_RING_START), 1);
	CHECK_UINT(hop_count_line(H_RING_START, 0), 1);
	CHECK(hop_index(H_RING_START) > hop_index(H_LOOP));
	view_get(1, &v);
	CHECK(!v.loop_test_audio);
	CHECK(!v.loop_test_ringing);

	CASE("the second release rings the other port and arms nothing");
	hops_reset();
	inject_hook(1, HAL_HOOK_RELEASE, 0);
	CHECK_UINT(hop_count(H_RING_START), 1);
	CHECK_UINT(hop_count_line(H_RING_START, 1), 1);
	CHECK(!hop_has(H_LOOP));
	CHECK_UINT(loop_disarm(), LINE_LOOP_TEST_OK);

	/* the self loop, which is what a one-port board runs whatever the
	 * operator asked for */
	CASE("a one-port board runs the self loop with the arm's delay");
	cfg_default();
	fixture_start();
	region_ready();
	fake_cap.lines = 1;
	hops_reset();
	CHECK_UINT(loop_arm(false), LINE_LOOP_TEST_OK);
	CHECK_UINT(hop_count(H_RING_START), 1);
	view_get(0, &v);
	CHECK(!strcmp(v.loop_test_mode, "self"));
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK_UINT(hop_count_line(H_RING_STOP, 0), 1);
	CHECK_UINT(loop_count_line(0, HAL_LOOP_SELF), 1);
	CHECK_UINT(hop_last(H_LOOP)->loop.delay_ms, 500);
	CHECK(hop_index(H_LOOP) > hop_index(H_RING_STOP));

	CASE("and the release clears it before the port rings again");
	hops_reset();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK_UINT(loop_count_line(0, HAL_LOOP_OFF), 1);
	CHECK_UINT(hop_count(H_RING_START), 1);
	CHECK(hop_index(H_RING_START) > hop_index(H_LOOP));

	CASE("a flash and a pulse while the mode holds the port do nothing");
	hops_reset();
	inject_hook(0, HAL_HOOK_FLASH, 0);
	inject_hook(0, HAL_HOOK_PULSE, 3);
	CHECK_UINT(nhops, 0);
	CHECK(state_is(0, "OOS"));
	view_get(0, &v);
	CHECK(strcmp(v.digits, "") == 0);
	fake_cap.lines = 2;

	/*
	 * The disarm. The ports go quiet, the service state comes back from
	 * the config and the line carries a call again.
	 */
	CASE("the disarm clears the audio, stops the bell and returns service");
	cfg_default();
	cfg.channels = 1;
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map),
		 "5");
	fixture_start();
	region_ready();
	CHECK_UINT(loop_arm(false), LINE_LOOP_TEST_OK);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_hook(1, HAL_HOOK_SEIZE, 0);
	hops_reset();
	CHECK_UINT(loop_disarm(), LINE_LOOP_TEST_OK);
	CHECK_UINT(loop_count_line(0, HAL_LOOP_OFF), 1);
	CHECK_UINT(loop_count_line(1, HAL_LOOP_OFF), 1);
	/* one stop per held port, and the machine of the port that comes back
	 * into service silences its own bell on the way to IDLE */
	CHECK_UINT(hop_count_line(H_RING_STOP, 1), 1);
	CHECK(hop_count_line(H_RING_STOP, 0) >= 1);
	CHECK_UINT(battery_count_line(1, HAL_LINE_DOWN), 1);
	view_get(0, &v);
	CHECK(!v.loop_test_set);
	CHECK(!v.oos);

	CASE("and the line places a call again");
	hops_reset();
	cops_reset();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	CHECK(cop_has(C_DIAL));
	CHECK(state_is(0, "CALLING"));
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("a call on any port refuses the arm and records nothing");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map),
		 "5");
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	CHECK(state_is(0, "CALLING"));
	hops_reset();
	CHECK_UINT(loop_arm(false), LINE_LOOP_TEST_CALL_ACTIVE);
	CHECK_UINT(nhops, 0);
	CHECK(!state_is(0, "OOS"));
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	/* the arm takes every port out of service, which would end a call
	 * that the clearing chain after HOLD_DIAL still holds */
	CASE("a held call in the clearing chain refuses the arm and keeps the call");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map),
		 "5");
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	inject_hook(0, HAL_HOOK_FLASH, 0);
	rec_cb.reinvite_done(0, 0, true);
	inject_digit(0, '5', false);
	rec_cb.failed(0, 1, CALL_FAIL_BUSY);
	CHECK(state_is(0, "CLEARING"));
	cops_reset();
	CHECK_UINT(loop_arm(false), LINE_LOOP_TEST_CALL_ACTIVE);
	CHECK(state_is(0, "CLEARING"));
	CHECK(!cop_has(C_HANGUP));
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("a dsp that is not ready refuses the arm");
	uv.hal_state = UVOICE_HAL_STARTING;
	CHECK_UINT(loop_arm(false), LINE_LOOP_TEST_NOT_READY);
	uv.hal_state = UVOICE_HAL_READY;

	CASE("a backend without the loop capability refuses the arm");
	fake_cap.features = HAL_FEAT_POLARITY;
	CHECK_UINT(loop_arm(false), LINE_LOOP_TEST_UNSUPPORTED);
	fake_cap.features = HAL_FEAT_POLARITY | HAL_FEAT_LOOP;

	CASE("a reload while the mode holds the board keeps every port out");
	cfg_default();
	fixture_start();
	region_ready();
	CHECK_UINT(loop_arm(false), LINE_LOOP_TEST_OK);
	lines_reload(&cfg);
	CHECK(state_is(0, "OOS"));
	CHECK(state_is(1, "OOS"));
	view_get(0, &v);
	CHECK(v.oos);
	CHECK(v.loop_test_set);
	CHECK_UINT(loop_disarm(), LINE_LOOP_TEST_OK);

	/*
	 * The entry reads the hook itself, because the mode consumes the
	 * edges of a held port and has no other way to learn where the
	 * handset is.
	 */
	CASE("an arm over a lifted handset arms the audio and rings nothing");
	cfg_default();
	fixture_start();
	region_ready();
	fake_cap.lines = 1;
	boot_offhook[0] = true;
	hops_reset();
	CHECK_UINT(loop_arm(true), LINE_LOOP_TEST_OK);
	CHECK_UINT(loop_count_line(0, HAL_LOOP_SELF), 1);
	CHECK(!hop_has(H_RING_START));
	view_get(0, &v);
	CHECK(v.loop_test_offhook);
	CHECK(v.loop_test_audio);
	CHECK(!v.loop_test_ringing);
	CHECK_UINT(loop_disarm(), LINE_LOOP_TEST_OK);
	boot_offhook[0] = false;
	fake_cap.lines = 2;

	CASE("an arm over a port already out of service stops its tone");
	cfg_default();
	fixture_start();
	region_ready();
	/* the port the config disables answers a seize with the reorder of
	 * OOS, and nothing else would silence it: the service sync does not
	 * force a machine that is already out of service */
	inject_hook(1, HAL_HOOK_SEIZE, 0);
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);
	hops_reset();
	CHECK_UINT(loop_arm(false), LINE_LOOP_TEST_OK);
	CHECK_UINT(hop_count_line(H_TONE_STOP, 1), 1);
	inject_hook(1, HAL_HOOK_RELEASE, 0);

	CASE("the disarm seizes a handset the subscriber left lifted");
	boot_offhook[0] = true;
	hops_reset();
	CHECK_UINT(loop_disarm(), LINE_LOOP_TEST_OK);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);
	CHECK_UINT(hop_last(H_TONE)->line, 0);
	boot_offhook[0] = false;
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	/*
	 * The two refusals the backend can answer, each retried once behind
	 * HAL_RETRY_MS, which is the rule of hal.h section 3.
	 */
	CASE("a transient bell is asked once more and then stands");
	cfg_default();
	fixture_start();
	region_ready();
	fake_cap.lines = 1;
	ring_start_status = HAL_TRANSIENT;
	ring_start_fail_n = 1;
	hops_reset();
	CHECK_UINT(loop_arm(true), LINE_LOOP_TEST_OK);
	CHECK_UINT(hop_count(H_RING_START), 1);
	view_get(0, &v);
	CHECK(!v.loop_test_ringing);
	uloop_pump(HAL_RETRY_MS + 50);
	CHECK_UINT(hop_count(H_RING_START), 2);
	view_get(0, &v);
	CHECK(v.loop_test_ringing);
	uloop_pump(HAL_RETRY_MS + 50);
	CHECK_UINT(hop_count(H_RING_START), 2);

	CASE("a transient arm is asked once more and a second refusal stands");
	loop_set_status = HAL_TRANSIENT;
	loop_set_fail_n = 2;
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK_UINT(hop_count(H_LOOP), 1);
	uloop_pump(HAL_RETRY_MS + 50);
	CHECK_UINT(hop_count(H_LOOP), 2);
	uloop_pump(HAL_RETRY_MS + 50);
	CHECK_UINT(hop_count(H_LOOP), 2);
	view_get(0, &v);
	CHECK(!v.loop_test_audio);

	CASE("a release that rings the port cancels a pending arm retry");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	loop_set_status = HAL_TRANSIENT;
	loop_set_fail_n = 1;
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK_UINT(hop_count(H_LOOP), 1);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK_UINT(hop_count(H_RING_START), 1);
	uloop_pump(HAL_RETRY_MS + 50);
	CHECK_UINT(hop_count(H_LOOP), 1);
	loop_set_status = HAL_OK;
	loop_set_fail_n = 0;
	CHECK_UINT(loop_disarm(), LINE_LOOP_TEST_OK);
	fake_cap.lines = 2;

	/*
	 * The same retry on a cross pair, where the edge that ends it lands on
	 * the other port. Nothing on this port moves, so only the rule can
	 * drop the pending arm: a retry that fired anyway would cross-connect
	 * one handset to a port that is back on its cradle.
	 */
	CASE("a release on the peer drops the pending arm of a cross pair");
	cfg_default();
	fixture_start();
	region_ready();
	CHECK_UINT(loop_arm(false), LINE_LOOP_TEST_OK);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	loop_set_status = HAL_TRANSIENT;
	loop_set_fail_n = 1;
	hops_reset();
	inject_hook(1, HAL_HOOK_SEIZE, 0);
	/* the pair wants audio on both ports, and port 0 is refused once */
	CHECK_UINT(loop_count_line(0, HAL_LOOP_PEER), 1);
	loop_set_status = HAL_OK;
	loop_set_fail_n = 0;
	hops_reset();
	inject_hook(1, HAL_HOOK_RELEASE, 0);
	/* port 1 armed and is taken down again; port 0 armed nothing */
	CHECK_UINT(loop_count_line(1, HAL_LOOP_OFF), 1);
	CHECK_UINT(hop_count_line(H_LOOP, 0), 0);
	uloop_pump(HAL_RETRY_MS + 50);
	CHECK_UINT(hop_count_line(H_LOOP, 0), 0);
	view_get(0, &v);
	CHECK(!v.loop_test_audio);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK_UINT(loop_disarm(), LINE_LOOP_TEST_OK);

	/*
	 * The clear the backend refuses transiently. The loop may still stand
	 * in the engine, and a line that carries one refuses every stream
	 * configuration, so the mode holds every port until a disarm lands.
	 */
	CASE("a transient clear refuses the disarm and holds the mode");
	cfg_default();
	fixture_start();
	region_ready();
	CHECK_UINT(loop_arm(false), LINE_LOOP_TEST_OK);
	loop_set_status = HAL_TRANSIENT;
	loop_set_fail_n = 1;
	hops_reset();
	CHECK_UINT(loop_disarm(), LINE_LOOP_TEST_REFUSED);
	CHECK(state_is(0, "OOS"));
	view_get(0, &v);
	CHECK(v.oos);
	CHECK(v.loop_test_set);

	CASE("and the repeat takes the mode down");
	CHECK_UINT(loop_disarm(), LINE_LOOP_TEST_OK);
	CHECK(state_is(0, "IDLE"));
	view_get(0, &v);
	CHECK(!v.oos);
	CHECK(!v.loop_test_set);

	CASE("the daemon stop clears the audio and stops the bell");
	cfg_default();
	fixture_start();
	region_ready();
	CHECK_UINT(loop_arm(false), LINE_LOOP_TEST_OK);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_hook(1, HAL_HOOK_SEIZE, 0);
	hops_reset();
	lines_done();
	lines_live = false;
	CHECK_UINT(loop_count_line(0, HAL_LOOP_OFF), 1);
	CHECK_UINT(loop_count_line(1, HAL_LOOP_OFF), 1);
	CHECK(hop_count(H_RING_STOP) >= 2);

	CASE("a ring end record clears the ringing key of a held port");
	cfg_default();
	fixture_start();
	region_ready();
	CHECK_UINT(loop_arm(false), LINE_LOOP_TEST_OK);
	view_get(0, &v);
	CHECK(v.loop_test_ringing);
	inject_event(0, HAL_EVENT_RING_END);
	view_get(0, &v);
	CHECK(!v.loop_test_ringing);
	CHECK(v.loop_test_set);
	CHECK_UINT(loop_disarm(), LINE_LOOP_TEST_OK);

	CASE("and the same record is still dropped on a port the mode left");
	inject_incoming(0, 0, "5551000", "", 0);
	CHECK(state_is(0, "RINGING"));
	view_get(0, &v);
	CHECK(v.ringing);
	inject_event(0, HAL_EVENT_RING_END);
	view_get(0, &v);
	CHECK(v.ringing);
	rec_cb.ended(0, 0, &ended);

	CASE("the mode refuses an inbound call and a metallic line test");
	cfg_default();
	fixture_start();
	region_ready();
	CHECK(line_call_admit(0));
	CHECK_UINT(loop_arm(false), LINE_LOOP_TEST_OK);
	CHECK(!line_call_admit(0));
	CHECK(!line_call_admit(1));
	CHECK_UINT(line_test_query(0, HAL_LINE_TEST_GR909,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_CALL_ACTIVE);

	CASE("and both come back with the disarm");
	CHECK_UINT(loop_disarm(), LINE_LOOP_TEST_OK);
	CHECK(line_call_admit(0));
	CHECK_UINT(line_test_query(0, HAL_LINE_TEST_GR909,
				   line_test_answer_take, &answer),
		   LINE_TEST_QUERY_OK);
	line_test_drop(0);
}

static void t_shutdown(void)
{
	CASE("lines_done hangs up what the machine still owns");
	cfg_default();
	snprintf(cfg.channel[0].digit_map, sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	rec_cb.answered(0, 0);
	cops_reset();
	hops_reset();
	/* the stop of main.c: the lines, then the protocol's own end */
	lines_done();
	proto_done();
	lines_live = false;
	CHECK(cop_has(C_HANGUP));
	CHECK(cop_has(C_DONE));

	CASE("lines_done stops the tone through the backend");
	CHECK(hop_count(H_TONE_STOP) >= 1);

	CASE("an event after lines_done is dropped");
	cops_reset();
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK_UINT(nhops, 0);
	CHECK_UINT(ncops, 0);

	CASE("lines_done twice is harmless");
	cops_reset();
	lines_done();
	proto_done();
	CHECK_UINT(ncops, 0);
}

/* ------------------------------------------------------- message waiting */

/*
 * Message waiting. Every NOTIFY writes the store, and the two presentations
 * are gated one by one. amwi picks the stutter dial tone through the narrow
 * entry of the machine. vmwi sends the FSK record through the type 1 CID op.
 * The store itself outlives the IDLE edge and a reload, on purpose, unlike
 * every other runtime state in the shell.
 */
static void t_mwi(void)
{
	struct line_view v;
	struct hop *cid;

	cfg_default();
	fixture_start();
	region_ready();

	CASE("a line with no message reports none");
	CHECK(view_get(0, &v));
	CHECK(!v.mwi);
	CHECK_UINT(v.mwi_new, 0);
	CHECK_UINT(v.mwi_old, 0);

	CASE("a notify stores the flag and both counts");
	mwi_notify(0, true, 2, 1);
	CHECK(view_get(0, &v));
	CHECK(v.mwi);
	CHECK_UINT(v.mwi_new, 2);
	CHECK_UINT(v.mwi_old, 1);

	CASE("and sends the visual record at once on an idle on-hook line");
	CHECK(hop_has(H_CID));
	cid = hop_last(H_CID);
	CHECK_UINT(cid->val, HAL_CID_TYPE1);
	CHECK_UINT(cid->cid.mwi, HAL_MWI_ON);
	CHECK_UINT(cid->cid.format, HAL_CID_BELLCORE_FSK);

	CASE("the record announces no caller, so it carries no absence reason");
	CHECK(strcmp(cid->cid.number, "") == 0);
	CHECK(strcmp(cid->cid.name, "") == 0);
	CHECK(strcmp(cid->cid.blockinfo, "") == 0);

	CASE("a notify that repeats what the line holds presents nothing again");
	hops_reset();
	mwi_notify(0, true, 2, 1);
	CHECK_UINT(hop_count(H_CID), 0);
	CHECK(view_get(0, &v));
	CHECK(v.mwi);

	CASE("a count that moves while the indication stays on is a transition");
	hops_reset();
	mwi_notify(0, true, 3, 1);
	CHECK_UINT(hop_count(H_CID), 1);
	CHECK_UINT(hop_last(H_CID)->cid.mwi, HAL_MWI_ON);
	CHECK(view_get(0, &v));
	CHECK_UINT(v.mwi_new, 3);

	CASE("clearing the indication sends the off record and keeps the counts");
	hops_reset();
	mwi_notify(0, false, 0, 0);
	CHECK_UINT(hop_count(H_CID), 1);
	CHECK_UINT(hop_last(H_CID)->cid.mwi, HAL_MWI_OFF);
	CHECK(view_get(0, &v));
	CHECK(!v.mwi);
	CHECK_UINT(v.mwi_new, 0);

	CASE("a repeated clear is not presented again");
	hops_reset();
	mwi_notify(0, false, 0, 0);
	CHECK_UINT(hop_count(H_CID), 0);

	/* the stutter, gated on amwi and carried by the machine's narrow
	 * entry */
	CASE("a waiting message turns the first dial tone into the stutter");
	cfg_default();
	fixture_start();
	region_ready();
	mwi_notify(0, true, 1, 0);
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK(state_is(0, "OFFHOOK_DIAL"));
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_STUTTER);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	/*
	 * The first digit, pinned as a class. The deferral itself is invisible
	 * in this build: cmake compiles every suite with UVOICE_HAL_STUB, which
	 * sets LINE_DIAL_TONE_DEFER_MS to 0, so the two branches of tone_set()
	 * do the same thing here. The class is what decides, and line/line.h
	 * exports it for exactly that reason. src/CMakeLists.txt adds the stub
	 * define only for the stub backend, so the timing half compiles only in
	 * a build with the udsp backend.
	 */
	CASE("the stutter belongs to the first dial tone class, which waits for the detector");
	CHECK(line_tone_is_dial(HAL_TONE_STUTTER));
	CHECK(line_tone_is_dial(HAL_TONE_DIAL));
	CHECK(!line_tone_is_dial(HAL_TONE_RINGBACK));
	CHECK(!line_tone_is_dial(HAL_TONE_SECOND_DIAL));
	CHECK(!line_tone_is_dial(HAL_TONE_CALL_WAITING));
	CHECK(!line_tone_is_dial(HAL_TONE_NONE));

	/*
	 * The first digit. The stutter takes the same detector defer the plain
	 * dial tone takes, so nothing reaches the line inside the seize event
	 * and a digit dialled during that window still reaches the machine.
	 * Without the defer the tone leaves before the detector is armed and
	 * the first digit of every line with a message is lost.
	 */
	CASE("the stutter is deferred like the dial tone, so the first digit survives");
	cfg_default();
	fixture_start();
	region_ready();
	mwi_notify(0, true, 1, 0);
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(hop_has(H_CHAN_CFG));
	CHECK(hop_index(H_CHAN_CFG) == 0);
#ifdef UVOICE_HAL_STUB
	CHECK(hop_index(H_TONE) > 0);
#else
	CHECK_UINT(hop_count(H_TONE), 0);
#endif
	inject_digit(0, '5', false);
	CHECK(state_is(0, "COLLECTING"));
	view_get(0, &v);
	CHECK(strcmp(v.digits, "5") == 0);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("the deferred stutter reaches the line when the window ends");
	cfg_default();
	fixture_start();
	region_ready();
	mwi_notify(0, true, 1, 0);
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_STUTTER);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("amwi 0 keeps the plain dial tone and stores the message just the same");
	cfg_default();
	cfg.channel[0].amwi = false;
	fixture_start();
	region_ready();
	mwi_notify(0, true, 1, 0);
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);
	CHECK(view_get(0, &v));
	CHECK(v.mwi);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("a message cleared while off hook gives the next seizure a dial tone");
	cfg_default();
	fixture_start();
	region_ready();
	mwi_notify(0, true, 1, 0);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	mwi_notify(0, false, 0, 0);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	/* the FSK gate */
	CASE("vmwi 0 stores the message and sends no record");
	cfg_default();
	cfg.channel[0].vmwi = false;
	fixture_start();
	region_ready();
	mwi_notify(0, true, 4, 2);
	CHECK_UINT(hop_count(H_CID), 0);
	CHECK(view_get(0, &v));
	CHECK(v.mwi);
	CHECK_UINT(v.mwi_new, 4);

	CASE("and the next rest sends no record while vmwi stays 0");
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK_UINT(hop_count(H_CID), 0);

	/* the switch does not send the summary again, so the gate holds the
	 * record instead of dropping it */
	CASE("a reload that enables vmwi presents the held message once");
	cfg.channel[0].vmwi = true;
	lines_reload(&cfg);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK_UINT(hop_count(H_CID), 1);
	CHECK_UINT(hop_last(H_CID)->cid.mwi, HAL_MWI_ON);

	/* the deferred presentation */
	CASE("a message that arrives off hook sends no record then");
	cfg_default();
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(DIAL_TONE_PUMP_MS);
	hops_reset();
	mwi_notify(0, true, 5, 0);
	CHECK_UINT(hop_count(H_CID), 0);

	CASE("and it is presented at the on-hook edge");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(hop_count(H_CID), 1);
	CHECK_UINT(hop_last(H_CID)->cid.mwi, HAL_MWI_ON);

	CASE("the held record is presented once, not on every rest");
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK_UINT(hop_count(H_CID), 0);

	CASE("a message that arrives while the bell rings waits for the bell");
	cfg_default();
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "5552000", "Bob", 0);
	CHECK(state_is(0, "RINGING"));
	hops_reset();
	mwi_notify(0, true, 6, 1);
	CHECK_UINT(hop_count(H_CID), 0);

	CASE("and reaches the line when the call ends and IDLE returns");
	rec_cb.remote_cancel(0, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK(hop_has(H_CID));
	CHECK_UINT(hop_last(H_CID)->val, HAL_CID_TYPE1);
	CHECK_UINT(hop_last(H_CID)->cid.mwi, HAL_MWI_ON);

	/*
	 * The store survives a reload. Every neighbouring runtime state is
	 * dropped by a reload because it describes a call or a subscriber
	 * toggle; a waiting message is owned by the switch and written in no
	 * config file, so a reload that cleared it would leave the line dark
	 * until the switch happened to send another NOTIFY.
	 */
	CASE("a reload does not clear the stored message");
	cfg_default();
	fixture_start();
	region_ready();
	mwi_notify(0, true, 7, 3);
	lines_reload(&cfg);
	CHECK(view_get(0, &v));
	CHECK(v.mwi);
	CHECK_UINT(v.mwi_new, 7);
	CHECK_UINT(v.mwi_old, 3);

	CASE("and the stutter still plays after that reload");
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_STUTTER);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("a reload that turns amwi off silences the stutter, message kept");
	cfg.channel[0].amwi = false;
	lines_reload(&cfg);
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_DIAL);
	CHECK(view_get(0, &v));
	CHECK(v.mwi);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("a reload that turns amwi back on restores it");
	cfg.channel[0].amwi = true;
	lines_reload(&cfg);
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_STUTTER);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	CASE("the record follows the region mwi_format, not the cid_format");
	cfg_default();
	fake_region.cid_format = HAL_CID_BELLCORE_FSK;
	fake_region.mwi_format = HAL_CID_ETSI_DTAS;
	fixture_start();
	region_ready();
	mwi_notify(0, true, 1, 1);
	CHECK(hop_has(H_CID));
	CHECK_UINT(hop_last(H_CID)->cid.format, HAL_CID_ETSI_DTAS);

	CASE("an event for a line the board does not carry is dropped");
	mwi_notify(7, true, 1, 1);
	CHECK(view_get(0, &v));
	CHECK_UINT(v.mwi_new, 1);

	CASE("the second line keeps its own store");
	cfg_default();
	cfg.channel[1].enabled = true;
	fixture_start();
	region_ready();
	/* the store survives the fixture restart by design (the warm init case
	 * below), so line 0 is cleared explicitly before the two are compared */
	mwi_notify(0, false, 0, 0);
	mwi_notify(1, true, 9, 8);
	CHECK(view_get(0, &v));
	CHECK(!v.mwi);
	CHECK(view_get(1, &v));
	CHECK(v.mwi);
	CHECK_UINT(v.mwi_new, 9);
	CHECK_UINT(v.mwi_old, 8);

	/*
	 * The pre-init NOTIFY is presented, not only kept. lines_init() runs on
	 * the dsp_start completion and the switch registers the daemon before
	 * that, so the e2e flow delivers a NOTIFY first. The switch does not
	 * send that message again, so the line presents it at the first moment
	 * it can carry one.
	 */
	CASE("a message stored before the lines exist is presented once at bring-up");
	cfg_default();
	fixture_start();
	region_ready();
	lines_stop();
	mwi_notify(0, true, 2, 1);
	hops_reset();
	lines_bring_up();
	CHECK_UINT(hop_count(H_CID), 1);
	CHECK_UINT(hop_last(H_CID)->val, HAL_CID_TYPE1);
	CHECK_UINT(hop_last(H_CID)->cid.mwi, HAL_MWI_ON);
	CHECK(view_get(0, &v));
	CHECK(v.mwi);
	CHECK_UINT(v.mwi_new, 2);

	CASE("and a bring-up with nothing waiting presents nothing");
	mwi_notify(0, false, 0, 0);
	lines_stop();
	hops_reset();
	lines_bring_up();
	CHECK_UINT(hop_count(H_CID), 0);

	/*
	 * The replay reads the view the apply installs. A reload staged while
	 * the line was busy lands when the line comes to rest, and the replay
	 * runs behind it: a reload that turned vmwi off must not put one more
	 * record on the line it was just switched off for.
	 */
	CASE("a reload that turned vmwi off is honoured by the release replay");
	cfg_default();
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(DIAL_TONE_PUMP_MS);
	mwi_notify(0, true, 3, 0);
	CHECK_UINT(hop_count(H_CID), 0);
	cfg.channel[0].vmwi = false;
	lines_reload(&cfg);
	hops_reset();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(hop_count(H_CID), 0);

	CASE("and the same release presents it when the reload left vmwi on");
	cfg_default();
	fixture_start();
	region_ready();
	/* the store survives the restart by design, so the state this case
	 * starts from is set explicitly and the notify below is a transition */
	mwi_notify(0, false, 0, 0);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(DIAL_TONE_PUMP_MS);
	hops_reset();
	mwi_notify(0, true, 4, 1);
	CHECK_UINT(hop_count(H_CID), 0);
	lines_reload(&cfg);
	hops_reset();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK_UINT(hop_count(H_CID), 1);
	CHECK_UINT(hop_last(H_CID)->cid.mwi, HAL_MWI_ON);

	CASE("a message stored out of service is held, not presented");
	cfg_default();
	cfg.channel[0].enabled = false;
	fixture_start();
	region_ready();
	CHECK(state_is(0, "OOS"));
	mwi_notify(0, true, 2, 2);
	CHECK_UINT(hop_count(H_CID), 0);

	CASE("and reaches the line when it comes back into service");
	cfg.channel[0].enabled = true;
	lines_reload(&cfg);
	CHECK(state_is(0, "IDLE"));
	CHECK(hop_has(H_CID));
	CHECK_UINT(hop_last(H_CID)->cid.mwi, HAL_MWI_ON);

	/*
	 * A warm init keeps it too. lines_init() runs on every dsp_start
	 * completion, and a NOTIFY can arrive before the first one: the e2e
	 * flow does exactly that, because the switch registers the daemon
	 * before its DSP reports ready. A clear here would lose the message,
	 * because a message the switch already delivered is not sent again.
	 */
	CASE("a warm re-init keeps the stored message");
	mwi_notify(0, true, 5, 4);
	fixture_start();
	region_ready();
	CHECK(view_get(0, &v));
	CHECK(v.mwi);
	CHECK_UINT(v.mwi_new, 5);
	CHECK_UINT(v.mwi_old, 4);

	CASE("and the machine hears it across that re-init");
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(DIAL_TONE_PUMP_MS);
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_STUTTER);
	inject_hook(0, HAL_HOOK_RELEASE, 0);

	/* the store outlives this suite by design, so the cases that follow
	 * start from a line the switch has nothing waiting for */
	mwi_notify(0, false, 0, 0);
	mwi_notify(1, false, 0, 0);
}

static void t_calltrace(void)
{
	struct call_ended info = ended_of(false, false, false, "5");

	CASE("calltrace: seize reports the line and state pair");
	cfg_default();
	cfg.channel[1].enabled = true;
	fixture_start();
	region_ready();
	inject_hook(1, HAL_HOOK_SEIZE, 0);
	CHECK_UINT(trace_note.state_count, 1);
	CHECK_UINT(trace_note.state_line, 1);
	CHECK_UINT(trace_note.before, FSM_IDLE);
	CHECK_UINT(trace_note.after, FSM_OFFHOOK_DIAL);

	CASE("calltrace: outbound dial reports the CDR sequence");
	cfg_default();
	snprintf(cfg.channel[0].digit_map,
		 sizeof(cfg.channel[0].digit_map), "5");
	fixture_start();
	region_ready();
	cdr_call_started(1, 0, false);
	xfer_stream_map(0, 1);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '5', false);
	CHECK_UINT(cdr_take(), 2);
	CHECK_UINT(dviews[1].slot, 1);
	CHECK_UINT(trace_note.start_count, 1);
	CHECK_UINT(trace_note.start_line, 0);
	CHECK_UINT(trace_note.seq, dviews[1].seq);
	CHECK_UINT(trace_note.seq, 2);

	CASE("calltrace: inbound offer reports the CDR sequence");
	cfg_default();
	cfg.channel[1].enabled = true;
	fixture_start();
	region_ready();
	cdr_call_started(0, 0, false);
	inject_incoming(1, 0, "5551000", "caller", 0);
	CHECK_UINT(cdr_take(), 2);
	CHECK_UINT(trace_note.start_count, 1);
	CHECK_UINT(trace_note.start_line, 1);
	CHECK_UINT(trace_note.seq, dviews[1].seq);
	CHECK_UINT(trace_note.seq, 2);

	CASE("calltrace: inbound offer reports the mapped stream sequence");
	cfg_default();
	fixture_start();
	region_ready();
	cdr_call_started(1, 0, false);
	xfer_stream_map(0, 1);
	inject_incoming(0, 0, "5551000", "caller", 0);
	CHECK_UINT(cdr_take(), 2);
	CHECK_UINT(dviews[1].slot, 1);
	CHECK_UINT(trace_note.start_count, 1);
	CHECK_UINT(trace_note.start_line, 0);
	CHECK_UINT(trace_note.seq, dviews[1].seq);
	CHECK_UINT(trace_note.seq, 2);

	CASE("calltrace: transferred leg reports its sequence and start event");
	cfg_default();
	fixture_start();
	cdr_call_started(0, 0, false);
	rec_cb.started(0, 1, false, 0);
	CHECK_UINT(cdr_take(), 2);
	CHECK_UINT(trace_note.start_count, 1);
	CHECK_UINT(trace_note.start_line, 0);
	CHECK_UINT(trace_note.seq, dviews[1].seq);
	CHECK_UINT(trace_note.seq, 2);
	CHECK_UINT(trace_note.event_count, 1);
	CHECK_UINT(trace_note.event_line, 0);
	CHECK(!strcmp(trace_note.reason, "leg_started"));

	CASE("calltrace: ended leg reports the line and end event");
	cfg_default();
	fixture_start();
	cdr_call_started(1, 0, false);
	rec_cb.ended(1, 0, &info);
	CHECK_UINT(trace_note.event_count, 1);
	CHECK_UINT(trace_note.event_line, 1);
	CHECK(!strcmp(trace_note.reason, "leg_ended"));

	CASE("calltrace: reload reports the forced OOS transition");
	cfg_default();
	fixture_start();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	trace_note.state_count = 0;
	cfg.channel[0].enabled = false;
	lines_reload(&cfg);
	CHECK_UINT(trace_note.state_count, 1);
	CHECK_UINT(trace_note.state_line, 0);
	CHECK_UINT(trace_note.before, FSM_OFFHOOK_DIAL);
	CHECK_UINT(trace_note.after, FSM_OOS);
}

/* -------------------------------------------------- the protocol contract */

/*
 * The line shell behind the contract of proto/proto.h. The group checks the
 * order of the service move, and the staged core copy with its apply. It also
 * checks the settle at the end of a drain, the stream key of the live query and
 * the SIP command rows. Last, it checks the null table that stands while no
 * protocol runs.
 */
static void t_proto_service(void)
{
	cfg_default();
	fixture_start();
	region_ready();

	CASE("proto: the SIP table runs and the bring-up started it");
	CHECK(proto_running());
	CHECK(!strcmp(proto_ops_get()->name, "sip"));
	CHECK(acct_rec.start >= 1);

	/* M1: the protocol hears the move with the oos bit of its outcome */
	CASE("service: the protocol hears the move out with the oos bit set");
	memset(&trace_note, 0, sizeof(trace_note));
	cfg.channel[0].enabled = false;
	lines_reload(&cfg);
	CHECK(state_is(0, "OOS"));
	CHECK(trace_note.after == FSM_OOS);
	CHECK_UINT(trace_note.rest, LINE_REST_OOS);

	CASE("service: and the move back in with the oos bit clear");
	memset(&trace_note, 0, sizeof(trace_note));
	cfg.channel[0].enabled = true;
	lines_reload(&cfg);
	CHECK(state_is(0, "IDLE"));
	CHECK(trace_note.before == FSM_OOS);
	CHECK(trace_note.after == FSM_IDLE);
	CHECK_UINT(trace_note.rest, LINE_REST_IDLE);
}

static void t_proto_staging(void)
{
	struct line_view v;

	/*
	 * The core copy is staged in front of ops->line_cfg_changed and lands
	 * through host->line_cfg_landed when the protocol applies its own view
	 * at a rest state. The call_waiting key reads the core copy.
	 */
	CASE("staging: a reload under a call leaves the core copy in place");
	cfg_default();
	fixture_start();
	region_ready();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	inject_digit(0, '1', false);
	CHECK(state_is(0, "COLLECTING"));
	cfg.channel[0].call_waiting_enabled = false;
	lines_reload(&cfg);
	CHECK(view_get(0, &v));
	CHECK(v.call_waiting);

	CASE("staging: the core copy lands with the protocol's view at rest");
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK(view_get(0, &v));
	CHECK(!v.call_waiting);

	/*
	 * The settle at the end of a drain presents a held visual record once,
	 * and only after the staged copy landed: the record has to ride the
	 * channel view the reload installs.
	 */
	/* The service entry drains before the protocol takes the reload, so
	 * that drain must not present the record over the old copy: the
	 * reload below turns vmwi off for the line it brings into service. */
	CASE("settle: a staged core copy holds the replay back");
	cfg_default();
	cfg.channel[0].enabled = false;
	fixture_start();
	region_ready();
	mwi_notify(0, true, 2, 0);
	hops_reset();
	cfg.channel[0].enabled = true;
	cfg.channel[0].vmwi = false;
	lines_reload(&cfg);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(hop_count(H_CID), 0);
	mwi_notify(0, false, 0, 0);

	CASE("settle: the replay runs once, behind the apply at rest");
	cfg_default();
	fixture_start();
	region_ready();
	mwi_notify(0, false, 0, 0);
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	uloop_pump(DIAL_TONE_PUMP_MS);
	mwi_notify(0, true, 2, 0);
	lines_reload(&cfg);
	hops_reset();
	line_settle(0);
	CHECK_UINT(hop_count(H_CID), 0);
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(state_is(0, "IDLE"));
	CHECK_UINT(hop_count(H_CID), 1);
	line_settle(0);
	CHECK_UINT(hop_count(H_CID), 1);
	mwi_notify(0, false, 0, 0);
}

static void t_proto_streams(void)
{
	const struct ubus_method *rows;
	struct blob_buf args = { 0 };
	unsigned int n = 0;

	cfg_default();
	fixture_start();
	region_ready();

	CASE("stats_live: the SIP table names the stream of a UI slot");
	CHECK_UINT(proto_ops_get()->stream_of_slot(0, 1), 1);

	CASE("stats_live: and no stream for a slot no leg can occupy");
	CHECK_UINT(proto_ops_get()->stream_of_slot(0, CALL_SLOTS),
		   HAL_STREAM_MAX);
	CHECK_UINT(line_stats_live_query(0,
		   proto_ops_get()->stream_of_slot(0, CALL_SLOTS),
		   live_take, NULL), LINE_STATS_NO_STREAM);

	CASE("sip ubus: the table adds register and unregister");
	rows = proto_ops_get()->ubus_methods(&n);
	CHECK_UINT(n, 2);
	CHECK(rows && !strcmp(rows[0].name, "register"));
	CHECK(rows && !strcmp(rows[1].name, "unregister"));
	if (!rows || n != 2)
		return;

	blob_buf_init(&args, 0);
	blobmsg_add_u32(&args, "line", 0);

	CASE("sip ubus: a refused command answers not found");
	acct_cmd_answer = false;
	request_replies = 0;
	CHECK_UINT(rows[0].handler(NULL, NULL, NULL, "register", args.head),
		   UBUS_STATUS_NOT_FOUND);
	CHECK_UINT(request_replies, 0);

	CASE("sip ubus: an accepted command answers through the host");
	acct_cmd_answer = true;
	CHECK_UINT(rows[1].handler(NULL, NULL, NULL, "unregister", args.head), 0);
	CHECK_UINT(request_replies, 1);
	CHECK_UINT(request_line, 0);
	CHECK(!strcmp(request_verb, "unregister"));
	CHECK_UINT(rows[0].handler(NULL, NULL, NULL, "register", args.head), 0);
	CHECK_UINT(request_replies, 2);
	CHECK_UINT(request_line, 0);
	CHECK(!strcmp(request_verb, "register"));
	acct_cmd_answer = false;

	CASE("sip ubus: a call with no line is an invalid argument");
	CHECK_UINT(rows[0].handler(NULL, NULL, NULL, "register", NULL),
		   UBUS_STATUS_INVALID_ARGUMENT);
	blob_buf_free(&args);
}

/* Whether both lines of the fixture read out of service. */
static bool lines_both_oos(void)
{
	struct line_view v0, v1;

	return view_get(0, &v0) && view_get(1, &v1) && v0.oos && v1.oos &&
	       !line_call_admit(0) && !line_call_admit(1);
}

/*
 * A config can name a protocol that an image does not carry. Its null table
 * then holds every line out of service (fifth reason) through DSP completion,
 * hook events and a reload. A seize plays reorder until its release.
 */
static void t_proto_null(void)
{
	struct line_view v;

	CASE("null table: a config naming mgcp selects it");
	cfg_default();
	snprintf(cfg.protocol, sizeof(cfg.protocol), "mgcp");
	fixture_start();
	region_ready();
	CHECK(!proto_running());
	CHECK(!strcmp(proto_ops_get()->name, "none"));

	CASE("null table: the fifth reason holds every line out of service");
	CHECK(lines_both_oos());

	/* the two halves of the bring-up apart, the way main.c runs them */
	CASE("null table: the early half holds every line out of service");
	lines_stop();
	proto_bind();
	lines_init(&cfg);
	proto_start(&cfg);
	lines_live = true;
	CHECK(lines_both_oos());

	CASE("null table: and the DSP completion keeps them out");
	lines_start(&cfg);
	region_ready();
	CHECK(lines_both_oos());

	CASE("null table: a call entry carries the core keys alone");
	CHECK(view_get(0, &v));
	CHECK(!v.state[0]);
	CHECK(!v.has_calls);

	CASE("null table: a seize plays reorder");
	hops_reset();
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);
	CHECK(lines_both_oos());

	CASE("null table: and the release stops it");
	hops_reset();
	inject_hook(0, HAL_HOOK_RELEASE, 0);
	CHECK(hop_has(H_TONE_STOP));
	CHECK(lines_both_oos());

	CASE("null table: a reload keeps the lines out");
	lines_reload(&cfg);
	CHECK(lines_both_oos());

	CASE("null table: the SIP table comes back with a config that "
	     "names it");
	cfg_default();
	fixture_start();
	CHECK(proto_running());
	CHECK(view_get(0, &v));
	CHECK(!v.oos);
}

/*
 * The core services a protocol reaches through the host table, where the
 * contract of proto/proto.h says more than the SIP machine uses: the replay of
 * a tone the protocol timed itself, and the loop feed.
 */
static void t_proto_host(void)
{
	const struct proto_host *h = proto_host_get();
	uint16_t ports[4] = { 0 };
	struct line_view v;

	CASE("host tone: a tone until stopped replays until stopped");
	cfg_default();
	fixture_start();
	h->tone_play(h->ctx, 0, HAL_TONE_REORDER, 0);
	hops_reset();
	job_complete(HAL_OK);
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_REORDER);
	CHECK_UINT(hop_last(H_TONE)->mask, 0);

	CASE("host tone: a timed tone replays with the length it asked for");
	cfg_default();
	fixture_start();
	h->tone_play(h->ctx, 0, HAL_TONE_BUSY, 1234);
	hops_reset();
	job_complete(HAL_OK);
	CHECK(hop_has(H_TONE));
	CHECK_UINT(hop_last(H_TONE)->val, HAL_TONE_BUSY);
	CHECK_UINT(hop_last(H_TONE)->mask, 1234);

	CASE("host battery: ACTIVE after REVERSE and DOWN puts the polarity "
	     "back first");
	cfg_default();
	fixture_start();
	region_ready();
	CHECK_UINT(h->battery_set(h->ctx, 0, PROTO_BATT_REVERSE), HAL_OK);
	CHECK_UINT(h->battery_set(h->ctx, 0, PROTO_BATT_DOWN), HAL_OK);
	hops_reset();
	CHECK_UINT(h->battery_set(h->ctx, 0, PROTO_BATT_ACTIVE), HAL_OK);
	CHECK(hop_has(H_POLARITY));
	CHECK_UINT(hop_last(H_POLARITY)->val, HAL_POL_FORWARD);
	CHECK(hop_has(H_LINE_STATE));
	CHECK_UINT(hop_last(H_LINE_STATE)->val, HAL_LINE_ACTIVE);
	CHECK(hop_index(H_POLARITY) < hop_index(H_LINE_STATE));
	CHECK(view_get(0, &v));
	CHECK(!v.fwd_disc_reversed);

	/* the settle behind a revive holds fwd_disc_down with the feed up */
	CASE("host battery: DOWN inside the settle window removes the feed "
	     "again");
	cfg_default();
	fixture_start();
	region_ready();
	CHECK_UINT(h->battery_set(h->ctx, 0, PROTO_BATT_DOWN), HAL_OK);
	CHECK_UINT(h->battery_set(h->ctx, 0, PROTO_BATT_ACTIVE), HAL_OK);
	CHECK(view_get(0, &v));
	CHECK(v.fwd_disc_down);
	hops_reset();
	CHECK_UINT(h->battery_set(h->ctx, 0, PROTO_BATT_DOWN), HAL_OK);
	CHECK_UINT(hop_count(H_LINE_STATE), 1);
	CHECK_UINT(hop_last(H_LINE_STATE)->val, HAL_LINE_DOWN);
	CHECK_UINT(h->battery_set(h->ctx, 0, PROTO_BATT_DOWN), HAL_OK);
	CHECK_UINT(hop_count(H_LINE_STATE), 1);
	CHECK_UINT(h->battery_set(h->ctx, 0, PROTO_BATT_ACTIVE), HAL_OK);

	/* the capture filter of a call trace is built from these ports */
	CASE("sip wire ports: the local and the remote port of the line");
	cfg_default();
	cfg.channel[0].sip_local_port = 5062;
	cfg.channel[0].sip_port = 5070;
	fixture_start();
	CHECK_UINT(proto_ops_get()->wire_ports(0, ports, 4), 2);
	CHECK_UINT(ports[0], 5062);
	CHECK_UINT(ports[1], 5070);
}

/*
 * The two hardware ways out of service end a call the way the machine ends
 * it, with no leg teardown in front. The forced OOS entry hangs the legs up.
 * The daemon stop ends the legs at once, before the backend closes. The rest
 * edge drops what the core held for the call, out of service too.
 */
static void t_proto_oos(void)
{
	struct line_view v;

	CASE("oos: a dead line under a ringing call hangs up through the "
	     "machine");
	cfg_default();
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "1002", "Alice", 0);
	CHECK(state_is(0, "RINGING"));
	cops_reset();
	inject_line_state(0, HAL_LINE_DOWN);
	CHECK(state_is(0, "OOS"));
	CHECK(cop_has(C_REJECT) || cop_has(C_HANGUP));
	CHECK(!cop_has(C_TEARDOWN));
	inject_line_state(0, HAL_LINE_ACTIVE);

	CASE("oos: a seize whose detector arm fails hangs up through the "
	     "machine");
	cfg_default();
	fixture_start();
	region_ready();
	inject_incoming(0, 0, "1002", "Alice", 0);
	CHECK(state_is(0, "RINGING"));
	cops_reset();
	chan_cfg_fatal = true;
	inject_hook(0, HAL_HOOK_SEIZE, 0);
	chan_cfg_fatal = false;
	CHECK(state_is(0, "OOS"));
	CHECK(cop_has(C_REJECT) || cop_has(C_HANGUP));
	CHECK(!cop_has(C_TEARDOWN));

	CASE("stop: inside the daemon stop every line ends its legs at once");
	cfg_default();
	fixture_start();
	region_ready();
	proto_stop_wait();
	cops_reset();
	lines_done();
	CHECK_UINT(cop_count(C_LINE_END), CFG_CHANNEL_MAX);
	CHECK(!cop_has(C_TEARDOWN));
	CHECK(!cop_has(C_DONE));
	proto_done();
	CHECK(cop_index(C_LINE_END) < cop_index(C_DONE));
	lines_live = false;

	CASE("transition: a rest move under the oos bit runs the rest drops");
	cfg_default();
	fixture_start();
	region_ready();
	cfg.channel[0].enabled = false;
	lines_reload(&cfg);
	CHECK(view_get(0, &v));
	CHECK(v.oos);
	uv.lec_policy[0].valid = true;
	line_transition(0, FSM_OOS, 0);
	CHECK(!uv.lec_policy[0].valid);
}

int main(void)
{
	uloop_init();
	uv.hal = &fake_hal;
	/* the host table carries the backend of the attach, the main.c order */
	proto_bind();

	if (pipe(notify_fd) != 0) {
		printf("# test_line: pipe failed\n");
		return 1;
	}
	overtake_ufd.fd = notify_fd[0];
	uloop_fd_add(&overtake_ufd, ULOOP_READ);

	t_preinit();
	t_bring_up_split();
	t_proto_service();
	t_proto_staging();
	t_proto_streams();
	t_proto_null();
	t_proto_host();
	t_proto_oos();
	t_region();
	t_tone_replay();
	t_tone_unobtainable();
	t_tone_duration();
	t_hook_intake();
	t_pulse();
	t_pulse_invert();
	t_dialplan_cfg();
	t_dialplan_expert();
	t_dialplan_timer_symbol();
	t_dialplan_field();
	t_timers();
	t_defer();
	t_no_target();
	t_fifo();
	t_reinvite_refused();
	t_detector();
	t_channel_cfg();
	t_ring();
	t_ring_pattern();
	t_distinctive_ring();
	t_hook_timing();
	t_boot_seize();
	t_reload_seize();
	t_hook_resync();
	t_flash_service();
	t_dtmf_duration();
	t_digit_play();
	t_inject();
	t_fax();
	t_t38();
	t_fax_exit();
	t_flash_relay();
	t_recall();
	t_call_waiting();
	t_cw_variant();
	t_call_events();
	t_early_media();
	t_oos();
	t_reload();
	t_features();
	t_confirm_tone();
	t_dnd_cf();
	t_xfer();
	t_cw_net();
	t_cw_mode();
	t_region_readback();
	t_hook_push_idempotent();
	t_cid_format();
	t_cid_gate();
	t_cid_delivery();
	t_cid_dial();
	t_mwi();
	t_status();
	t_media_ops();
	t_cdr();
	t_calltrace();
	t_cdr_jb();
	t_jb_cache();
	t_stamp_clamp();
	t_stats_live();
	t_line_test();
	t_ring_hold();
	t_ring_give_up();
	t_xfer_swap();
	t_conf();
	t_digit_play_states();
	t_fwd_disc();
	t_fwd_disc_remove();
	t_fwd_disc_held();
	t_slic_fault();
	t_line_down();
	t_line_down_abort();
	t_loop_test();
	t_shutdown();

	lines_stop();
	uloop_fd_delete(&overtake_ufd);
	close(notify_fd[0]);
	close(notify_fd[1]);
	blob_buf_free(&sb);
	uloop_done();

	return tu_summary("test_line");
}
