// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * A real T.30 page through the G.711 passthrough path under packet loss. Two
 * spandsp fax terminals, a caller and an answerer, exchange
 * audio through two udsp streams that the test builds. Each stream has the
 * description of a fax call. It carries PCMU at 20 ms, the fixed jitter buffer
 * at 50 ms and the fax passthrough kind, and its decoder has no concealment
 * history.
 *
 * No engine runs. Each side gets one line of one handle, and the harness is
 * the tick. The transmit half of a tick feeds the audio of the terminal to
 * stream_tx_media and reads the packet off a socketpair. It applies the loss
 * schedule of the case. It writes each packet that is not lost into the net
 * ring of the peer, with an arrival stamp of its own. The receive half writes
 * the tick clock of the handle and runs stream_tick_rx, and the output frame
 * goes to the terminal of the same side.
 *
 * Time is virtual. A side advances its clock 10 ms per tick, or a little less
 * for a sender that runs fast. Nothing in the run reads a real clock.
 *
 * The loss schedules come from three field captures of fax calls over a lossy
 * network: one passing call and two failing calls. A schedule lists the index
 * of each lost packet in the stream of its sender. The suite asserts a floor.
 * On the path with no loss, the page must arrive intact with ECM on and with
 * ECM off. A page must also arrive from a sender 200 ppm fast, with the skew
 * drops counted. Under one loss per 750 packets, every concealed unit must be
 * silence.
 *
 * The capture schedules and the run with one loss per 750 packets record the
 * outcome of the page. Each run prints two lines: the result and the timeline
 * of the T.30 frames. These runs assert that the call ends, but not the
 * outcome of the page. An assertion on the outcome needs recorded values that
 * are stable across the sanitizer runs, and a decision on the concealment
 * policy of a passthrough.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spandsp.h>
#include <tiffio.h>

#include "udsp/udsp.h"

#include "../core/udsp_priv.h"
#include "../core/stream.h"
#include "../dsp/dsp.h"

#include "util.h"

#define T30_PAGE_WIDTH		1728
/* One A4 page at the T.4 standard resolution, 3.85 rows per mm. The image
 * of this page runs about two minutes at V.17 14400, so the harness call is
 * about as long as the field calls of the captures (149 s to 153 s of RTP).
 * Three schedule entries still fall outside a run: the fwd 7573 (150.1 s)
 * and rev 7466 (148.0 s) of the second failing capture lie past the end of a
 * clean ECM page (145.7 s), and its rev 48 lies before the fax began (below
 * index 0). None of the three changes a result: every capture run ends on an
 * earlier loss inside an image block, and the rev 48 packet carried speech. */
#define T30_PAGE_ROWS		1143
#define T30_TICK_NS		10000000ull
#define T30_FRAME		UDSP_FRAME_SAMPLES
#define T30_PTIME_MS		20u
#define T30_JB_INIT_MS		50u
/* The failing capture calls ran 88 s and 153 s of RTP, so a run under a
 * capture schedule gets that much before it counts as hung. */
#define T30_TICK_MAX		18000u
/* Bound on bad rows of the clean ECM-off page, from the measured clean run. */
#define T30_BAD_ROWS_CLEAN	0
#define T30_LOSS_MAX		128u
#define T30_TIMELINE_MAX	64u
#define T30_ROWS(a)		(unsigned)(sizeof(a) / sizeof((a)[0]))

/* --- loss schedules ------------------------------------------------------ */

/*
 * fwd is caller to answerer, rev is answerer to caller; each entry is the
 * index of a lost packet counted from the first packet its sender sent in
 * the capture. An offset is the capture index of the last packet of that
 * direction in flight when the caller answered the fax re-INVITE with 200,
 * which is where the harness index starts: a harness packet of index i is
 * the capture packet i + offset. The answerer's stream began about 30 ms
 * after the caller's in every capture, so the two directions differ.
 *
 * A schedule takes its name from the outcome of its call and its loss rate
 * over both directions of 20 ms packets. The passing call loses 10 packets in
 * 149 s, one per 1490. The failing calls lose 10 in 88 s and 20 in 153 s, one
 * per 880 and one per 765.
 */
struct t30_loss {
	const char *name;
	const uint16_t *fwd;
	const uint16_t *rev;
	unsigned fwd_n, rev_n;
	int fwd_offset, rev_offset;
};

static const uint16_t t30_pass_1490_fwd[] = { 3000, 3543, 3568, 4150, 4179 };
static const uint16_t t30_pass_1490_rev[] = { 1460, 1506, 2035, 5036, 6420 };
static const uint16_t t30_fail_880_fwd[] = { 2137, 2287, 3093, 3531, 3712 };
static const uint16_t t30_fail_880_rev[] = { 207, 379, 2514, 3879, 4289 };
static const uint16_t t30_fail_765_fwd[] = {
	1745, 1903, 4614, 4883, 5027, 5772, 6022, 6209, 6234, 6306, 6608, 7573,
};
static const uint16_t t30_fail_765_rev[] = {
	48, 329, 1277, 1367, 3999, 5772, 6239, 7466,
};

/* The offsets were measured with tshark on the captures. In every capture, the
 * caller's 200 to the fax re-INVITE comes between index 67 and 68 of the
 * caller's stream. In the answerer's stream, it comes between 65 and 66. */
static const struct t30_loss t30_clean = { "clean", NULL, NULL, 0, 0, 0, 0 };
static const struct t30_loss t30_pass_1490 = {
	"pass 1/1490", t30_pass_1490_fwd, t30_pass_1490_rev,
	T30_ROWS(t30_pass_1490_fwd), T30_ROWS(t30_pass_1490_rev), 67, 65,
};
static const struct t30_loss t30_fail_880 = {
	"fail 1/880", t30_fail_880_fwd, t30_fail_880_rev,
	T30_ROWS(t30_fail_880_fwd), T30_ROWS(t30_fail_880_rev), 67, 65,
};
static const struct t30_loss t30_fail_765 = {
	"fail 1/765", t30_fail_765_fwd, t30_fail_765_rev,
	T30_ROWS(t30_fail_765_fwd), T30_ROWS(t30_fail_765_rev), 67, 65,
};

static uint16_t t30_every_buf[T30_LOSS_MAX];
static char t30_every_name[32];

/* One loss every n packets in both directions, the n-th packet first. */
static const struct t30_loss *every_n(struct t30_loss *out, unsigned n)
{
	unsigned count = 0;

	for (unsigned i = n - 1u; i < T30_TICK_MAX && count < T30_LOSS_MAX;
	     i += n)
		t30_every_buf[count++] = (uint16_t)i;
	snprintf(t30_every_name, sizeof(t30_every_name), "every %u", n);
	out->name = t30_every_name;
	out->fwd = t30_every_buf;
	out->rev = t30_every_buf;
	out->fwd_n = count;
	out->rev_n = count;
	out->fwd_offset = 0;
	out->rev_offset = 0;

	return out;
}

static bool loss_hit(const struct t30_loss *l, bool fwd, unsigned index)
{
	const uint16_t *table = fwd ? l->fwd : l->rev;
	unsigned n = fwd ? l->fwd_n : l->rev_n;
	long capture = (long)index + (fwd ? l->fwd_offset : l->rev_offset);

	for (unsigned i = 0; i < n; i++) {
		if ((long)table[i] == capture)
			return true;
	}

	return false;
}

/* --- the page files ------------------------------------------------------ */

/* The page and the compare of udsp-fax-spandsp/src/test/test_fax_spandsp.c,
 * so the two suites carry the same image. */
static int page_write(const char *path, unsigned rows)
{
	uint8_t row[T30_PAGE_WIDTH / 8];
	TIFF *t = TIFFOpen(path, "w");

	if (!t)
		return -1;
	TIFFSetField(t, TIFFTAG_IMAGEWIDTH, T30_PAGE_WIDTH);
	TIFFSetField(t, TIFFTAG_IMAGELENGTH, rows);
	TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, 1);
	TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, 1);
	TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE);
	TIFFSetField(t, TIFFTAG_FILLORDER, FILLORDER_MSB2LSB);
	TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
	TIFFSetField(t, TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX4);
	TIFFSetField(t, TIFFTAG_ROWSPERSTRIP, -1L);
	TIFFSetField(t, TIFFTAG_XRESOLUTION, 204.0);
	TIFFSetField(t, TIFFTAG_YRESOLUTION, 98.0);
	TIFFSetField(t, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
	for (unsigned y = 0; y < rows; y++) {
		for (unsigned x = 0; x < T30_PAGE_WIDTH / 8; x++)
			row[x] = ((x + y) % 7 == 0) ? 0xa5 : 0x00;
		if (TIFFWriteScanline(t, row, y, 0) < 0) {
			TIFFClose(t);
			return -1;
		}
	}
	TIFFClose(t);

	return 0;
}

static bool page_same(const char *a_path, const char *b_path)
{
	uint8_t row_a[T30_PAGE_WIDTH / 8];
	uint8_t row_b[T30_PAGE_WIDTH / 8];
	uint32_t wa = 0, wb = 0, ha = 0, hb = 0;
	TIFF *a = TIFFOpen(a_path, "r");
	TIFF *b = TIFFOpen(b_path, "r");
	bool same = true;

	if (!a || !b) {
		if (a)
			TIFFClose(a);
		if (b)
			TIFFClose(b);
		return false;
	}
	TIFFGetField(a, TIFFTAG_IMAGEWIDTH, &wa);
	TIFFGetField(b, TIFFTAG_IMAGEWIDTH, &wb);
	TIFFGetField(a, TIFFTAG_IMAGELENGTH, &ha);
	TIFFGetField(b, TIFFTAG_IMAGELENGTH, &hb);
	if (wa != wb || ha != hb || wa != T30_PAGE_WIDTH ||
	    TIFFScanlineSize(a) != (tmsize_t)sizeof(row_a))
		same = false;
	for (uint32_t y = 0; same && y < ha; y++) {
		if (TIFFReadScanline(a, row_a, y, 0) < 0 ||
		    TIFFReadScanline(b, row_b, y, 0) < 0)
			same = false;
		else if (memcmp(row_a, row_b, sizeof(row_a)) != 0)
			same = false;
	}
	TIFFClose(a);
	TIFFClose(b);

	return same;
}

/* --- the harness --------------------------------------------------------- */

struct t30_side {
	unsigned line;
	fax_state_t *fax;
	struct udsp_stream *st;
	/* wire[0] is the stream's rtp_fd, wire[1] the end the harness reads. */
	int wire[2];
	/* The side's clock on the one virtual timeline, and its tick period:
	 * 10 ms, or less for a sender that runs fast. */
	uint64_t next_ns;
	uint64_t period_ns;
	unsigned sent;		/* packets put on the wire: the loss index */
	unsigned lost;
	unsigned concealed;	/* output units the buffer could not fill */
	unsigned concealed_loud;	/* of those, with a non-zero sample */
	unsigned plc_units;	/* concealed units the buffer decided as PLC */
	bool done;
	int code;
};

/* One T.30 command frame as the caller saw it, for the timeline a record
 * line carries: the timeline shows the T.30 phase that each loss hit. */
struct t30_frame {
	uint32_t ms;
	uint8_t fcf;
	bool incoming;
};

struct t30_cfg {
	const struct t30_loss *loss;
	bool ecm;
	/* The caller's clock runs this many parts per million fast. */
	unsigned caller_ppm;
};

struct t30_result {
	bool ended;		/* both terminals reached phase E */
	unsigned ticks;
	/* Each side's completion code is valid only after its phase E handler
	 * ran; a side the tick bound stopped has none (a zero would read as
	 * T30_ERR_OK). */
	bool done_caller, done_answerer;
	int code_caller, code_answerer;
	/* Counters of what the harness itself dropped, which must stay 0: a
	 * refused send and a full or overrun ring would otherwise count as
	 * network loss in the record. */
	uint64_t tx_eagain, net_drops, net_stale;
	int pages_tx, pages_rx, bad_rows;
	unsigned lost_fwd, lost_rev;
	unsigned concealed, concealed_loud, plc_units;
	uint64_t skew_drops;	/* of the answerer, the fast sender's peer */
	bool same;
};

static const char *t30_page_in;
static const char *t30_page_out;
static struct t30_side t30_caller, t30_answerer;
static struct t30_frame t30_timeline[T30_TIMELINE_MAX];
static unsigned t30_timeline_n;

static void phase_e_note(void *user_data, int code)
{
	struct t30_side *side = user_data;

	side->done = true;
	side->code = code;
}

/* The caller's view of the command frames. The ECM image frames are left
 * out: a page carries dozens of them and they name no phase. */
static void frame_note(void *user_data, bool incoming, const uint8_t msg[],
		       int len)
{
	struct t30_side *side = user_data;
	uint8_t fcf;

	if (side != &t30_caller || len < 3 ||
	    t30_timeline_n >= T30_TIMELINE_MAX)
		return;
	fcf = msg[2];
	if (fcf == T4_FCD || fcf == T4_RCP)
		return;
	t30_timeline[t30_timeline_n].ms =
		(uint32_t)(side->next_ns / 1000000ull);
	t30_timeline[t30_timeline_n].fcf = fcf;
	t30_timeline[t30_timeline_n].incoming = incoming;
	t30_timeline_n++;
}

static void timeline_print(void)
{
	printf("# t30 frames:");
	for (unsigned i = 0; i < t30_timeline_n; i++) {
		const struct t30_frame *f = &t30_timeline[i];

		printf(" %s%s@%u.%02u", f->incoming ? "<" : ">",
		       t30_frametype(f->fcf), f->ms / 1000u,
		       (f->ms % 1000u) / 10u);
	}
	printf("\n");
}

/*
 * The description a fax call runs on, as media_prepare settles it for a
 * passthrough: no detector, no comfort noise, the fixed buffer, and the
 * codec pair built without concealment history (codec_params_build).
 */
static bool stream_up(struct udsp *s, struct t30_side *side)
{
	struct udsp_stream *st = stream_at(s, side->line, 0);
	struct udsp_codec_params params;
	struct udsp_cmd cmd;

	if (!st)
		return false;
	st->media.codec = UDSP_SEL_PCMU;
	st->media.codec_pt = 0;
	st->media.dtmf_mode = UDSP_DTMF_INBAND;
	st->media.direction = UDSP_MEDIA_SENDRECV;
	st->media.ptime_ms = T30_PTIME_MS;
	st->media.jb_mode = UDSP_JB_FIXED;
	st->media.jb_init_ms = T30_JB_INIT_MS;
	st->media.jb_max_ms = 2u * T30_JB_INIT_MS;
	st->media.rtcp_interval_ms = UDSP_RTCP_MIN_MS;
	st->media.punch_interval_ms = UDSP_TX_PUNCH_DEFAULT_MS;
	st->media.passthrough = UDSP_PASSTHROUGH_FAX;
	memset(&params, 0, sizeof(params));
	params.line_rate = UDSP_SAMPLE_RATE;
	params.plc_off = true;
	st->ops = &udsp_codec_pcmu_ops;
	st->params = params;
	st->enc = st->ops->create(&params);
	st->dec = st->ops->create(&params);
	if (!st->enc || !st->dec)
		return false;
	st->entropy.ssrc = 0x5150aa00u + side->line;
	st->entropy.sequence = 1000u * (side->line + 1u);
	st->entropy.timestamp = 8000u * (side->line + 1u);
	st->rtp_fd = side->wire[0];
	st->rtcp_fd = -1;
	side->st = st;

	memset(&cmd, 0, sizeof(cmd));
	cmd.type = UDSP_CMD_STREAM;
	cmd.line = (uint8_t)side->line;
	cmd.arg0 = STREAM_OP_ADOPT;
	if (stream_cmd_run(s, &cmd, 0))
		return false;
	cmd.arg0 = STREAM_OP_ACTIVATE;

	return stream_cmd_run(s, &cmd, 0) == 0;
}

static void stream_down(struct udsp_stream *st)
{
	if (!st)
		return;
	if (st->enc)
		st->ops->destroy(st->enc);
	if (st->dec)
		st->ops->destroy(st->dec);
	st->enc = NULL;
	st->dec = NULL;
}

/* A side that holds nothing: side_down on it is safe, and so is a run that
 * never brought it up. */
static void side_clear(struct t30_side *side)
{
	memset(side, 0, sizeof(*side));
	side->wire[0] = -1;
	side->wire[1] = -1;
}

static bool side_up(struct udsp *s, struct t30_side *side, unsigned line,
		    bool caller, const struct t30_cfg *cfg)
{
	t30_state_t *t30;

	side_clear(side);
	side->line = line;
	side->period_ns = T30_TICK_NS;
	if (caller && cfg->caller_ppm)
		side->period_ns = T30_TICK_NS * 1000000ull /
				  (1000000ull + cfg->caller_ppm);
	if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, side->wire))
		return false;
	if (!stream_up(s, side))
		return false;

	side->fax = fax_init(NULL, caller);
	if (!side->fax)
		return false;
	fax_set_transmit_on_idle(side->fax, true);
	t30 = fax_get_t30_state(side->fax);
	t30_set_phase_e_handler(t30, phase_e_note, side);
	t30_set_real_time_frame_handler(t30, frame_note, side);
	t30_set_tx_ident(t30, caller ? "1234567890" : "0987654321");
	t30_set_supported_modems(t30, T30_SUPPORT_V27TER | T30_SUPPORT_V29 |
				 T30_SUPPORT_V17);
	t30_set_ecm_capability(t30, cfg->ecm);
	if (caller)
		t30_set_tx_file(t30, t30_page_in, -1, -1);
	else
		t30_set_rx_file(t30, t30_page_out, -1);

	return true;
}

static void side_down(struct t30_side *side)
{
	if (side->fax)
		fax_free(side->fax);
	side->fax = NULL;
	stream_down(side->st);
	if (side->wire[0] >= 0)
		close(side->wire[0]);
	if (side->wire[1] >= 0)
		close(side->wire[1]);
	side_clear(side);
}

/* The harness is the one producer of the ring, as the net thread is. */
static void ring_push(struct udsp_stream *st, const uint8_t *data, size_t len,
		      uint64_t arrival_ns)
{
	unsigned head = atomic_load_explicit(&st->ring.head,
					     memory_order_relaxed);
	unsigned tail = atomic_load_explicit(&st->ring.tail,
					     memory_order_acquire);
	struct net_slot *slot = &st->ring.slot[head % UDSP_NET_SLOTS];

	CHECK(head - tail < UDSP_NET_SLOTS);
	CHECK(len <= UDSP_NET_DGRAM_MAX);
	if (head - tail >= UDSP_NET_SLOTS || len > UDSP_NET_DGRAM_MAX)
		return;
	slot->arrival_ns = arrival_ns;
	slot->len = (uint32_t)len;
	slot->kind = NET_KIND_RTP;
	memcpy(slot->data, data, len);
	atomic_store_explicit(&st->ring.head, head + 1u, memory_order_release);
}

/* The transmit half of one tick: the terminal's 10 ms of audio through the
 * stream, and whatever left the socket through the loss schedule into the
 * peer's ring. */
static void side_tx(struct udsp *s, struct t30_side *side,
		    struct t30_side *peer, const struct t30_cfg *cfg,
		    uint64_t now_ns)
{
	uint8_t dgram[UDSP_NET_DGRAM_MAX];
	int16_t pcm[T30_FRAME];
	uint32_t now_ms = (uint32_t)(now_ns / 1000000ull);
	bool fwd = side == &t30_caller;
	int n;

	memset(pcm, 0, sizeof(pcm));
	n = fax_tx(side->fax, pcm, T30_FRAME);
	if (n < (int)T30_FRAME)
		memset(pcm + n, 0, (T30_FRAME - (unsigned)n) * sizeof(pcm[0]));
	stream_tx_media(s, side->st, pcm, T30_FRAME, now_ms);
	for (;;) {
		ssize_t len = recv(side->wire[1], dgram, sizeof(dgram),
				   MSG_DONTWAIT);
		unsigned index;

		if (len <= 0)
			break;
		index = side->sent++;
		if (loss_hit(cfg->loss, fwd, index)) {
			side->lost++;
			continue;
		}
		ring_push(peer->st, dgram, (size_t)len, now_ns);
	}
}

static bool frame_silent(const int16_t *frame, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		if (frame[i])
			return false;
	}

	return true;
}

/* The receive half: the tick clock, the stream's output frame, the terminal.
 * A unit the buffer could not fill must reach the terminal as silence. */
static void side_rx(struct udsp *s, struct t30_side *side, uint64_t now_ns)
{
	const struct udsp_jb_stats *jb = &side->st->jb.total;
	uint64_t plc = jb->plc;
	uint64_t mark = jb->plc + jb->cn + jb->silence + jb->skew_inserts;
	int16_t frame[UDSP_FRAME_MAX];

	memset(frame, 0, sizeof(frame));
	s->tick_ns = now_ns;
	stream_tick_rx(s, &s->chan[side->line], frame,
		       (uint32_t)(now_ns / 1000000ull));
	if (jb->plc + jb->cn + jb->silence + jb->skew_inserts != mark) {
		side->concealed++;
		if (!frame_silent(frame, T30_FRAME))
			side->concealed_loud++;
	}
	if (jb->plc != plc)
		side->plc_units++;
	fax_rx(side->fax, frame, T30_FRAME);
}

static void side_tick(struct udsp *s, struct t30_side *side,
		      struct t30_side *peer, const struct t30_cfg *cfg)
{
	side_tx(s, side, peer, cfg, side->next_ns);
	side_rx(s, side, side->next_ns);
	side->next_ns += side->period_ns;
}

static struct udsp *handle_up(int event_fd)
{
	struct udsp *s = calloc(1, sizeof(*s));

	if (!s)
		return NULL;
	s->cap.lines = 2;
	s->rate = UDSP_SAMPLE_RATE;
	s->frame = T30_FRAME;
	atomic_init(&s->lines, 2u);
	atomic_init(&s->debug, 0u);
	atomic_init(&s->pkt_tx, NULL);
	atomic_init(&s->pkt_rx, NULL);
	for (unsigned i = 0; i < UDSP_LINE_MAX; i++) {
		atomic_init(&s->lec_delay[i], 0u);
		atomic_init(&s->lec_ready[i], false);
	}
	atomic_init(&s->lec_loop, 0u);
	for (unsigned i = 0; i < 2u; i++) {
		s->chan[i].line = i;
		s->chan[i].rate = UDSP_SAMPLE_RATE;
		s->chan[i].frame = T30_FRAME;
		atomic_init(&s->chan[i].mute, false);
	}
	if (udsp_emitter_init(&s->emitter, event_fd, &s->clock, 0, NULL,
			      NULL) || stream_pool_alloc(s)) {
		udsp_emitter_release(&s->emitter);
		stream_pool_free(s);
		free(s);
		return NULL;
	}

	return s;
}

static void handle_down(struct udsp *s)
{
	if (!s)
		return;
	stream_pool_free(s);
	udsp_emitter_release(&s->emitter);
	free(s);
}

/* What a stream refused or overran of its own, as the engine reads them. */
static void harness_drops_add(struct t30_result *res,
			      const struct udsp_stream *st)
{
	res->tx_eagain += atomic_load_explicit(&st->tx_eagain,
					       memory_order_relaxed);
	res->net_drops += atomic_load_explicit(&st->net_drops,
					       memory_order_relaxed);
	res->net_stale += atomic_load_explicit(&st->net_stale,
					       memory_order_relaxed);
}

static void result_fill(struct t30_result *res, unsigned ticks)
{
	struct udsp_jb_stats jb;
	t30_stats_t stats;

	memset(res, 0, sizeof(*res));
	res->ticks = ticks;
	res->ended = t30_caller.done && t30_answerer.done;
	res->done_caller = t30_caller.done;
	res->done_answerer = t30_answerer.done;
	res->code_caller = t30_caller.code;
	res->code_answerer = t30_answerer.code;
	harness_drops_add(res, t30_caller.st);
	harness_drops_add(res, t30_answerer.st);
	CHECK_UINT(res->tx_eagain, 0);
	CHECK_UINT(res->net_drops, 0);
	CHECK_UINT(res->net_stale, 0);
	t30_get_transfer_statistics(fax_get_t30_state(t30_caller.fax), &stats);
	res->pages_tx = stats.pages_tx;
	t30_get_transfer_statistics(fax_get_t30_state(t30_answerer.fax),
				    &stats);
	res->pages_rx = stats.pages_rx;
	res->bad_rows = stats.bad_rows;
	res->lost_fwd = t30_caller.lost;
	res->lost_rev = t30_answerer.lost;
	res->concealed = t30_caller.concealed + t30_answerer.concealed;
	res->concealed_loud = t30_caller.concealed_loud +
			      t30_answerer.concealed_loud;
	res->plc_units = t30_caller.plc_units + t30_answerer.plc_units;
	udsp_jb_stats(&t30_answerer.st->jb, &jb);
	res->skew_drops = jb.skew_drops;
	res->same = res->ended && page_same(t30_page_in, t30_page_out);
}

/*
 * One call. The two sides tick on one virtual timeline in 10 ms steps; a
 * side whose clock runs fast takes a second tick in the step its clock
 * crosses. The caller ticks first, so its packet of a step is in the
 * answerer's ring before the answerer's receive half of the same step.
 */
static void page_run(const struct t30_cfg *cfg, struct t30_result *res)
{
	int event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	struct udsp *s = handle_up(event_fd);
	unsigned tick = 0;
	bool up;

	memset(res, 0, sizeof(*res));
	t30_timeline_n = 0;
	side_clear(&t30_caller);
	side_clear(&t30_answerer);
	remove(t30_page_out);
	CHECK(event_fd >= 0);
	CHECK(s != NULL);
	up = s && page_write(t30_page_in, T30_PAGE_ROWS) == 0 &&
	     side_up(s, &t30_caller, 0, true, cfg) &&
	     side_up(s, &t30_answerer, 1, false, cfg);
	CHECK(up);
	for (tick = 0; up && tick < T30_TICK_MAX; tick++) {
		uint64_t now_ns = (uint64_t)tick * T30_TICK_NS;

		while (t30_caller.next_ns <= now_ns)
			side_tick(s, &t30_caller, &t30_answerer, cfg);
		while (t30_answerer.next_ns <= now_ns)
			side_tick(s, &t30_answerer, &t30_caller, cfg);
		if (t30_caller.done && t30_answerer.done)
			break;
	}
	if (up)
		result_fill(res, tick);
	side_down(&t30_caller);
	side_down(&t30_answerer);
	handle_down(s);
	if (event_fd >= 0)
		close(event_fd);
}

/* A side the tick bound stopped has no completion code; its zero would read
 * as T30_ERR_OK. */
static const char *code_str(bool done, int code)
{
	return done ? t30_completion_code_to_str(code) : "-";
}

/* The record of each run: a result line and a frame timeline. A value becomes
 * an assertion only after the lines of the plain and the sanitizer runs agree.
 * The sweep's ended= is the same both-sides end; a run the tick bound
 * stopped shows ticks= at the bound and ended=0. */
static void result_record(const struct t30_cfg *cfg,
			  const struct t30_result *res, bool sweep)
{
	printf("# t30 result: %s ecm=%d caller=%s answerer=%s pages=%d "
	       "bad_rows=%d lost=%u/%u ticks=%u",
	       cfg->loss->name, cfg->ecm ? 1 : 0,
	       code_str(res->done_caller, res->code_caller),
	       code_str(res->done_answerer, res->code_answerer), res->pages_rx,
	       res->bad_rows, res->lost_fwd, res->lost_rev, res->ticks);
	if (sweep)
		printf(" ended=%d", res->ended ? 1 : 0);
	printf("\n");
	timeline_print();
}

static void page_check(const struct t30_result *res)
{
	CHECK(res->ended);
	CHECK_UINT(res->code_caller, T30_ERR_OK);
	CHECK_UINT(res->code_answerer, T30_ERR_OK);
	CHECK_UINT(res->pages_tx, 1);
	CHECK_UINT(res->pages_rx, 1);
}

static void page_check_ecm(const struct t30_result *res)
{
	page_check(res);
	CHECK(res->same);
}

/* --- cases --------------------------------------------------------------- */

static void case_clean(void)
{
	struct t30_cfg cfg = { &t30_clean, true, 0 };
	struct t30_result res;

	CASE("t30: a clean G.711 passthrough carries a page with ECM on");
	page_run(&cfg, &res);
	page_check_ecm(&res);
	CHECK_UINT(res.lost_fwd + res.lost_rev, 0);
	CHECK_UINT(res.plc_units, 0);
	result_record(&cfg, &res, false);

	CASE("t30: and with ECM off, with no bad rows");
	cfg.ecm = false;
	page_run(&cfg, &res);
	page_check(&res);
	CHECK(res.bad_rows <= T30_BAD_ROWS_CLEAN);
	CHECK(res.same);
	result_record(&cfg, &res, false);
}

static void case_capture(void)
{
	struct t30_cfg cfg = { &t30_pass_1490, true, 0 };
	struct t30_result res;

	CASE("t30: the passing capture schedule with ECM on is recorded");
	page_run(&cfg, &res);
	CHECK(res.ended);
	result_record(&cfg, &res, false);

	CASE("t30: the first failing capture schedule is recorded with ECM "
	     "on");
	cfg.loss = &t30_fail_880;
	page_run(&cfg, &res);
	CHECK(res.ended);
	result_record(&cfg, &res, false);

	CASE("t30: the second failing capture schedule is recorded with ECM "
	     "on");
	cfg.loss = &t30_fail_765;
	page_run(&cfg, &res);
	CHECK(res.ended);
	result_record(&cfg, &res, false);

	CASE("t30: the passing capture schedule with ECM off is recorded");
	cfg.loss = &t30_pass_1490;
	cfg.ecm = false;
	page_run(&cfg, &res);
	CHECK(res.ended);
	result_record(&cfg, &res, false);

	CASE("t30: the failing capture schedules with ECM off are recorded");
	cfg.loss = &t30_fail_880;
	page_run(&cfg, &res);
	CHECK(res.ended);
	result_record(&cfg, &res, false);
	cfg.loss = &t30_fail_765;
	page_run(&cfg, &res);
	CHECK(res.ended);
	result_record(&cfg, &res, false);
}

static void case_floor(void)
{
	struct t30_loss loss;
	struct t30_cfg cfg = { every_n(&loss, 750), true, 0 };
	struct t30_result res;

	CASE("t30: one lost packet per fifteen seconds with ECM on is "
	     "recorded");
	page_run(&cfg, &res);
	CHECK(res.ended);
	CHECK(res.lost_fwd > 0);
	CHECK(res.lost_rev > 0);
	result_record(&cfg, &res, false);

	CASE("t30: every concealed unit of a passthrough page is silence");
	CHECK(res.plc_units > 0);
	CHECK(res.concealed > 0);
	CHECK_UINT(res.concealed_loud, 0);
}

static void case_drift(void)
{
	struct t30_cfg cfg = { &t30_clean, true, 200 };
	struct t30_result res;

	CASE("t30: a page survives a sender 200 ppm fast against the fixed "
	     "buffer");
	page_run(&cfg, &res);
	page_check_ecm(&res);
	result_record(&cfg, &res, false);

	CASE("t30: and the skew corrector's drops are counted, not silent");
	CHECK(res.skew_drops > 0);
	printf("# t30 drift: skew_drops=%llu concealed=%u loud=%u ticks=%u\n",
	       (unsigned long long)res.skew_drops, res.concealed,
	       res.concealed_loud, res.ticks);
}

static void case_sweep(void)
{
	static const unsigned every[] = { 100, 250, 500, 750, 1500 };
	struct t30_loss loss;
	struct t30_result res;
	char name[96];

	for (unsigned e = 0; e < T30_ROWS(every); e++) {
		for (unsigned ecm = 0; ecm < 2u; ecm++) {
			struct t30_cfg cfg = { every_n(&loss, every[e]),
					       ecm == 1u, 0 };

			snprintf(name, sizeof(name),
				 "t30 sweep: one lost packet per %u with ECM "
				 "%s is recorded", every[e],
				 ecm ? "on" : "off");
			CASE(name);
			page_run(&cfg, &res);
			CHECK(res.ticks <= T30_TICK_MAX);
			result_record(&cfg, &res, true);
		}
	}
}

int main(int argc, char **argv)
{
	bool sweep = argc > 1 && !strcmp(argv[1], "--sweep");
	int rc;

	t30_page_in = sweep ? "test_fax_t30-sweep-in.tif" :
			      "test_fax_t30-in.tif";
	t30_page_out = sweep ? "test_fax_t30-sweep-out.tif" :
			       "test_fax_t30-out.tif";
	/* A run that failed leaves no output page; page_same reports that
	 * through its return, so libtiff's own message is noise. */
	TIFFSetErrorHandler(NULL);
	TIFFSetWarningHandler(NULL);
	if (sweep) {
		case_sweep();
	} else {
		case_clean();
		case_capture();
		case_floor();
		case_drift();
	}
	remove(t30_page_in);
	remove(t30_page_out);
	rc = tu_summary(sweep ? "test_fax_t30_sweep" : "test_fax_t30");

	return rc;
}
