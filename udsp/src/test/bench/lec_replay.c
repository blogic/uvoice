// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * lec_replay: the host replay of a canceller capture of one line (.lecr).
 *
 *   lec_replay <file.lecr> [--verdicts] [--csv] [--residual out.wav]
 *              [--frames out.csv] [--range A:B] [--skip-s N]
 *
 * The program runs every record of the file through dsp/lec.c under the
 * recorded controls (test/bench/lec_replay.h) and compares the replay with
 * the live record. It prints a header, one block per segment, the totals,
 * the census and one line "result: PASS", "result: FAIL" or
 * "result: INCOMPLETE". The criteria of the result are those of
 * lecr_agree_result.
 *
 * Each instance takes the reference loss of the file header. The header of
 * the report names the loss. A file whose flag says that it carries no loss
 * replays at 0, and the program writes one warning line to stderr.
 *
 * - --verdicts prints each replayed RUN record that differs from the live
 *   record.
 * - --csv prints one row per record, and a "# " before every other line.
 * - --residual writes the output of a shadow instance, whose processor is
 *   off from creation, as a mono 16 bit WAV file at the line rate. A FEED
 *   frame writes the microphone, and a RUN frame the replay skipped writes
 *   zeros. The time axis is the axis of the WAV files of lec-capture.py.
 *   The frame of a RUN or a FEED sits at its tick less the tick of the
 *   first record of the file. Zeros fill the ticks between two such
 *   records: skipped periods, gaps and the time from an END to the next
 *   NEW. One such run of zeros is cut to 60 s, and a note counts the cuts.
 *   A residual that passes the 4 GiB data limit of a WAV file is refused:
 *   the program removes the file and exits 2.
 * - --frames out.csv writes one row per replayed RUN record, from the state
 *   of the replay instance after the frame (udsp_ec_nlms_frame_get and
 *   udsp_ec_nlms_info_get). A header line names the columns. A write error
 *   exits 2. The columns, in order:
 *   - tick, the tick of the record, and frames, the frame count of the
 *     instance;
 *   - verdict, the word of nlp_verdict;
 *   - nlp_active, fired (dt_fired), dt (double_talk), hold_left and probing;
 *   - trial_arm and trial_end, the words of lecr_trial_arm_name and
 *     lecr_trial_end_name;
 *   - probes, diverge_resets, dt_run, stale_run, trusted and classic_arm;
 *   - erle_db, the ERLE that the detector read, then echo_gain, coef_energy
 *     and noise_dbov of the instance;
 *   - bar, ref_power, window_power, mic_power and estimate_power of the
 *     frame reading, then out_power, the output of the replay. These are
 *     mean squares in int16 units;
 *   - talker, the talker verdict of the frame reading, 0 or 1;
 *   - ref_bar and ref_bar_max, the activity bar of the frame and its cap,
 *     mean squares in int16 units; background, 0 or 1, the window under
 *     ref_bar and at or over the fixed bar; ref_floor of the instance, the
 *     far-end floor after the frame;
 *   - trust_run, the trust count of the frame reading after the frame.
 *   Each float prints with %.9g, so it reads back as the same float.
 * - --range A:B limits the census to the frames with index A up to and not
 *   including B. A: runs to the end of the file. Without the option, the
 *   census covers every frame.
 * - --skip-s N leaves the first N seconds of each segment out of the
 *   steady-state figures (default 15). The control figures count from the
 *   first record.
 *
 * The frame index of a replayed RUN record is the count of replayed RUN
 * records from the start of the file, 1 on the first. A second is 100
 * frames: second k holds the indexes 100k to 100k + 99. The census reads
 * levels in dBFS, 10 log10(power / 32768^2), the unit of tools/udsp_audio.py.
 * It compares a power with the power of a level. Its terms:
 * - the guard window of a frame: the larger window_power of the frame and of
 *   the RUN frame before it;
 * - a far frame: the largest ref_power of the frame and of the six RUN
 *   frames before it stands at or over the far bar, -40 dBFS less the
 *   reference loss L of the file in dB;
 * - a leak frame: a far frame whose out_power stands over -65 dBFS;
 * - a low-reference frame: the guard window stands under the far bar;
 * - a near frame: mic_power stands at or over 16 times the guard window
 *   times the bound factor, min(0.25 x 10^(L/10), 2 x echo_gain) after the
 *   frame;
 * - an exposure frame: the guard window under the far bar, mic_power at or
 *   over -60 dBFS, and estimate_power at or over a tenth of mic_power;
 * - the echo loss of a second with 10 or more far frames: the summed
 *   mic_power over the summed out_power of those frames, in dB;
 * - a lockout second: one of the seconds 3 to 29 with all of these:
 *   - 10 or more far frames;
 *   - the verdict dt on 30 or more of its frames;
 *   - an echo loss under 8 dB.
 *
 * The terms read a history: the six RUN frames before a frame, and the frame
 * before it. The history restarts at a NEW record, a new instance, where the
 * tick can jump. It does not restart at a RESET, because the reference before
 * a RESET still returns as echo after it.
 *
 * The census follows the totals. Each line starts with "census", and key
 * value pairs follow it, so a script reads a line with a split:
 * - census range A:B frames N, the range and its RUN frames;
 * - census sel S frames N off N quiet N dt N unlike N erle N pass N
 *   nlp_active N dt_flag N fired N leak N exposure N. S is all, far, low (the
 *   low-reference frames) or near. The six verdict words count nlp_verdict,
 *   and dt_flag counts double_talk;
 *   - the far line adds out_sum X out_max X out_max_dbfs X leak_per_min X.
 *     These are the sum of out_power, the largest out_power as a power and in
 *     dBFS, and the leak frames per minute;
 *   - the near line adds out_sum X mic_sum X out_mic_db X: the sums of
 *     out_power and of mic_power, and their ratio in dB;
 * - census sel far_not_near frames N nlp_active N leak N out_sum X out_max X
 *   out_max_dbfs X, for the far frames that are not near frames;
 * - census second K far N dt N loss_db X, for each second of the range with
 *   10 or more far frames. dt counts the frames with the verdict dt;
 * - census lockout N seconds K,K,..., with "-" for no second.
 * A rate per minute is a count times 6000 over the RUN frames of the range.
 * The sums and the largest power print with %.9g, and a dB figure prints
 * with two decimals.
 *
 * The exit status is 0 for PASS, 1 for FAIL, 3 for INCOMPLETE and 2 for a
 * usage or an I/O error.
 *
 * The CMake cache path UDSP_REPLAY_LEC_C selects the canceller source that
 * the program links, so the same program replays a capture against another
 * copy of dsp/lec.c with the same dsp/lec.h interface.
 *
 * Two limits apply to the result:
 * 1. The replay is a comparison of the DSP under the recorded controls. It
 *    replays the outcome of the fax and modem decisions of the captured
 *    build (the freeze, the processor switch and the skips), not the
 *    detector that took them. A changed canceller changes the input of that
 *    detector, because the fax detector reads the canceller output, so the
 *    live controls of a changed build can differ.
 * 2. A host replay is near-exact against the aarch64 build, not exact. The
 *    target runs the Advanced SIMD kernels, contracts to FMA and uses
 *    another libm, and one LSB can flip a marginal verdict. The replay on
 *    the target is the proof of exactness; the host replay is the
 *    statistical check.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lec_replay.h"

#define WAV_HEAD_BYTES		44u
#define WAV_GAP_FRAMES_MAX	(60u * LECR_TICKS_PER_SECOND)
/* The RIFF size field holds 36 + the data bytes in 32 bits. */
#define WAV_DATA_MAX		(UINT32_MAX - 36u)

#define CENSUS_FULL_SCALE	32768.0
#define CENSUS_FAR_DBFS		-40.0
#define CENSUS_LEAK_DBFS	-65.0
#define CENSUS_EXPOSE_DBFS	-60.0
#define CENSUS_HISTORY		6u
/* LEC_GEIGEL_GAIN_MIN and LEC_GEIGEL_GAIN_HEADROOM of dsp/lec.c: the bound
 * factor of echo_bound_get, which is private to that file. The census scales
 * CENSUS_GAIN_MIN and CENSUS_FAR_DBFS by the reference loss of the file, as
 * the canceller scales its own rules; the microphone figures stay. */
#define CENSUS_GAIN_MIN		0.25
#define CENSUS_GAIN_HEADROOM	2.0
#define CENSUS_NEAR_RATIO	16.0
#define CENSUS_EXPOSE_RATIO	10.0
#define CENSUS_SECOND_FAR_MIN	10u
#define CENSUS_LOCKOUT_FIRST	3u
#define CENSUS_LOCKOUT_LAST	29u
#define CENSUS_LOCKOUT_DT_MIN	30u
#define CENSUS_LOCKOUT_LOSS_DB	8.0
#define CENSUS_MINUTE_FRAMES	(60u * LECR_TICKS_PER_SECOND)

/* range_end 0 runs the census to the end of the file. */
struct replay_opts {
	const char *path;
	const char *residual;
	const char *frames;
	unsigned skip_s;
	uint32_t range_first;
	uint32_t range_end;
	bool verdicts;
	bool csv;
};

/* The state of the replay instance after one replayed RUN record, and the
 * frame index of the record. */
struct frame_view {
	uint64_t tick;
	uint32_t index;
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	float out_power;
};

struct census_sel {
	uint32_t frames;
	uint32_t verdict[LECR_VERDICT_PASS + 1u];
	uint32_t nlp_active;
	uint32_t dt_flag;
	uint32_t fired;
	uint32_t leak;
	uint32_t exposure;
	double out_sum;
	double out_max;
	double mic_sum;
};

/* dt counts the frames of the second with the verdict dt. The powers sum
 * the far frames. */
struct census_second {
	uint32_t second;
	uint32_t far;
	uint32_t dt;
	double mic_sum;
	double out_sum;
};

/* The terms of the header comment for one RUN frame. */
struct census_mark {
	bool far;
	bool leak;
	bool low;
	bool near;
	bool exposure;
};

/*
 * ref_last and window_last hold the RUN frames of the instance before the
 * current one, inside the range or not. A frame at the start of the range
 * therefore reads its history. seconds holds the closed seconds with 10 or
 * more far frames.
 */
struct census {
	uint32_t first;
	uint32_t end;
	double gain_min;
	double far_power;
	double leak_power;
	double expose_power;
	float ref_last[CENSUS_HISTORY];
	float window_last;
	uint32_t runs;
	struct census_sel all;
	struct census_sel far;
	struct census_sel low;
	struct census_sel near;
	struct census_sel far_not_near;
	struct census_second now;
	bool now_open;
	struct census_second *seconds;
	size_t count;
	size_t room;
	int err;
};

/* The counts of the file itself, before any replay, for the header. */
struct replay_scan {
	uint32_t records;
	uint32_t instances;
	uint32_t resets;
	uint32_t losses;
	uint64_t lost;
	unsigned tail_ms;
	bool tail_mixed;
	int err;
};

/* tick_next is the tick of the frame after the last one written, and the
 * tick of the first record before any frame. index is the frame index of the
 * last replayed RUN record. */
struct replay_out {
	const struct replay_opts *opts;
	FILE *wav;
	uint64_t wav_bytes;
	uint64_t wav_max;
	unsigned samples;
	bool have_first;
	uint64_t tick_next;
	uint32_t gaps_cut;
	int err;
	FILE *frames;
	uint32_t index;
	struct census census;
};

__attribute__((format(printf, 2, 3)))
static void report_print(const struct replay_opts *o, const char *fmt, ...)
{
	va_list ap;

	if (o->csv)
		fputs("# ", stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

static void usage_print(FILE *f)
{
	fprintf(f, "usage: lec_replay <file.lecr> [--verdicts] [--csv] "
		"[--residual out.wav] [--frames out.csv] [--range A:B] "
		"[--skip-s N]\n");
}

/* A decimal frame index at s, ended by the character stop. */
static int index_parse(const char *s, char stop, const char **next,
		       uint32_t *value)
{
	unsigned long long v;
	char *end;

	if (*s < '0' || *s > '9')
		return -EINVAL;
	errno = 0;
	v = strtoull(s, &end, 10);
	if (errno || *end != stop || v > UINT32_MAX)
		return -EINVAL;
	*next = end;
	*value = (uint32_t)v;
	return 0;
}

/* A:B with B over A, or A: for the end of the file. */
static int range_parse(const char *s, struct replay_opts *o)
{
	if (index_parse(s, ':', &s, &o->range_first))
		return -EINVAL;
	o->range_end = 0;
	if (!*++s)
		return 0;
	if (index_parse(s, '\0', &s, &o->range_end) ||
	    o->range_end <= o->range_first)
		return -EINVAL;
	return 0;
}

static int opts_parse(int argc, char **argv, struct replay_opts *o)
{
	int i;

	memset(o, 0, sizeof(*o));
	o->skip_s = LECR_SKIP_S_DEFAULT;
	o->range_first = 1;
	for (i = 1; i < argc; i++) {
		char *end;

		if (!strcmp(argv[i], "--verdicts")) {
			o->verdicts = true;
		} else if (!strcmp(argv[i], "--csv")) {
			o->csv = true;
		} else if (!strcmp(argv[i], "--residual") && i + 1 < argc) {
			o->residual = argv[++i];
		} else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
			o->frames = argv[++i];
		} else if (!strcmp(argv[i], "--range") && i + 1 < argc) {
			if (range_parse(argv[++i], o))
				return -EINVAL;
		} else if (!strcmp(argv[i], "--skip-s") && i + 1 < argc) {
			unsigned long v = strtoul(argv[++i], &end, 10);

			if (*end || end == argv[i] || v > 86400u)
				return -EINVAL;
			o->skip_s = (unsigned)v;
		} else if (argv[i][0] == '-' || o->path) {
			return -EINVAL;
		} else {
			o->path = argv[i];
		}
	}
	return o->path ? 0 : -EINVAL;
}

static void scan_run(FILE *f, struct replay_scan *sc)
{
	struct lecr_file file;
	struct lecr_record rec;
	bool have_tail = false;
	int rc;

	memset(sc, 0, sizeof(*sc));
	sc->err = lecr_read_open(f, &file);
	while (!sc->err && (rc = lecr_read_record(f, &file, &rec)) != 0) {
		if (rc < 0) {
			sc->err = rc;
			break;
		}
		sc->records++;
		if (rec.lost_before) {
			sc->losses++;
			sc->lost += rec.lost_before;
		}
		if (rec.kind == LECR_KIND_RUN && (rec.flags & LECR_FLAG_RESET))
			sc->resets++;
		if (rec.kind == LECR_KIND_END || !(rec.flags & LECR_FLAG_NEW))
			continue;
		sc->instances++;
		if (have_tail && rec.tail_ms != sc->tail_ms)
			sc->tail_mixed = true;
		if (!have_tail)
			sc->tail_ms = rec.tail_ms;
		have_tail = true;
	}
}

static int wav_head_write(FILE *f, uint32_t rate, uint32_t data_bytes)
{
	uint8_t h[WAV_HEAD_BYTES];

	memcpy(h, "RIFF", 4);
	lecr_le32_put(h + 4, 36u + data_bytes);
	memcpy(h + 8, "WAVEfmt ", 8);
	lecr_le32_put(h + 16, 16u);
	lecr_le16_put(h + 20, 1u);
	lecr_le16_put(h + 22, 1u);
	lecr_le32_put(h + 24, rate);
	lecr_le32_put(h + 28, rate * 2u);
	lecr_le16_put(h + 32, 2u);
	lecr_le16_put(h + 34, 16u);
	memcpy(h + 36, "data", 4);
	lecr_le32_put(h + 40, data_bytes);
	return lecr_bytes_write(f, h, sizeof(h));
}

/*
 * The data limit of the residual: WAV_DATA_MAX, or a smaller count of bytes
 * in UDSP_TEST_WAV_DATA_MAX. A test can thus reach the limit with a small
 * file. Any other value leaves the limit of the format.
 */
static uint64_t wav_max_get(void)
{
	const char *v = getenv("UDSP_TEST_WAV_DATA_MAX");
	char *end = NULL;
	unsigned long long n;

	if (!v || *v < '0' || *v > '9')
		return WAV_DATA_MAX;
	errno = 0;
	n = strtoull(v, &end, 10);
	if (errno || *end || !n || n > WAV_DATA_MAX)
		return WAV_DATA_MAX;
	return n;
}

/* A frame that would pass the data limit sets -EFBIG and writes nothing, and
 * so does every frame after it. */
static void wav_frame_write(struct replay_out *out, const int16_t *frame)
{
	uint8_t b[2u * LECR_SAMPLES_MAX];
	unsigned bytes = 2u * out->samples;

	if (out->err)
		return;
	if (out->wav_bytes + bytes > out->wav_max) {
		out->err = -EFBIG;
		return;
	}
	lecr_plane_encode(b, frame, out->samples);
	if (lecr_bytes_write(out->wav, b, bytes)) {
		out->err = -EIO;
		return;
	}
	out->wav_bytes += bytes;
}

static void wav_record_write(struct replay_out *out,
			     const struct lecr_record *rec,
			     const struct lecr_replay *r,
			     const struct lecr_step *st)
{
	static const int16_t zeros[LECR_SAMPLES_MAX];
	uint64_t gap = 0;

	if (!out->have_first) {
		out->have_first = true;
		out->tick_next = rec->tick;
	}
	if (rec->kind == LECR_KIND_END)
		return;
	if (rec->tick > out->tick_next)
		gap = rec->tick - out->tick_next;
	out->tick_next = rec->tick + 1u;
	if (gap > WAV_GAP_FRAMES_MAX) {
		gap = WAV_GAP_FRAMES_MAX;
		out->gaps_cut++;
	}
	while (gap-- && !out->err)
		wav_frame_write(out, zeros);
	if (rec->kind == LECR_KIND_FEED)
		wav_frame_write(out, rec->mic);
	else if (st->replayed)
		wav_frame_write(out, r->residual);
	else
		wav_frame_write(out, zeros);
}

static unsigned flag_get(const struct lecr_record *rec, unsigned flag)
{
	return (rec->flags & flag) ? 1u : 0u;
}

static const char *kind_name(unsigned kind)
{
	switch (kind) {
	case LECR_KIND_RUN:
		return "run";
	case LECR_KIND_FEED:
		return "feed";
	case LECR_KIND_END:
		return "end";
	}
	return "?";
}

static void csv_head_print(void)
{
	printf("tick,kind,flags,lost_before,replayed,frames,replay_frames,"
	       "verdict,replay_verdict,fired,replay_fired,dt,replay_dt,"
	       "active,replay_active,probing,replay_probing,hold_frames,"
	       "replay_hold_frames,out_delta\n");
}

static void csv_row_print(const struct lecr_record *rec,
			  const struct lecr_replay *r,
			  const struct lecr_step *st)
{
	const struct lecr_record *got = &r->got;
	const char *live_word = udsp_ec_nlms_verdict_name(rec->nlp_verdict);

	printf("%" PRIu64 ",%s,0x%04x,%" PRIu32 ",%u,%" PRIu32 ",",
	       rec->tick, kind_name(rec->kind), rec->flags, rec->lost_before,
	       st->replayed ? 1u : 0u, rec->frames);
	if (rec->kind != LECR_KIND_RUN || !st->replayed) {
		printf(",%s,,%u,,%u,,%u,,%u,,%" PRIu32 ",,\n", live_word,
		       flag_get(rec, LECR_FLAG_FIRED),
		       flag_get(rec, LECR_FLAG_DT),
		       flag_get(rec, LECR_FLAG_ACTIVE),
		       flag_get(rec, LECR_FLAG_PROBING), rec->hold_frames);
		return;
	}
	printf("%" PRIu32 ",%s,%s,%u,%u,%u,%u,%u,%u,%u,%u,%" PRIu32 ",%"
	       PRIu32 ",%u\n", got->frames, live_word,
	       udsp_ec_nlms_verdict_name(got->nlp_verdict),
	       flag_get(rec, LECR_FLAG_FIRED), flag_get(got, LECR_FLAG_FIRED),
	       flag_get(rec, LECR_FLAG_DT), flag_get(got, LECR_FLAG_DT),
	       flag_get(rec, LECR_FLAG_ACTIVE), flag_get(got, LECR_FLAG_ACTIVE),
	       flag_get(rec, LECR_FLAG_PROBING),
	       flag_get(got, LECR_FLAG_PROBING), rec->hold_frames,
	       got->hold_frames, st->out_delta);
}

static void diff_names_get(unsigned diff, char *buf, size_t size)
{
	static const char *const names[] = {
		"frames", "fired", "dt", "active", "probing", "verdict", "out",
	};
	size_t used = 0;
	unsigned i;

	buf[0] = '\0';
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (!(diff & (1u << i)))
			continue;
		used += (size_t)snprintf(buf + used, size - used, "%s%s",
					 used ? " " : "", names[i]);
		if (used >= size)
			return;
	}
}

static void verdict_line_print(const struct replay_opts *o,
			       const struct lecr_record *rec,
			       const struct lecr_replay *r,
			       const struct lecr_step *st)
{
	const struct lecr_record *got = &r->got;
	char names[64];

	diff_names_get(st->diff, names, sizeof(names));
	report_print(o, "differ tick %" PRIu64 " frames %" PRIu32 "/%" PRIu32
		     " verdict %s/%s flags 0x%02x/0x%02x out delta %u (%s)\n",
		     rec->tick, rec->frames, got->frames,
		     udsp_ec_nlms_verdict_name(rec->nlp_verdict),
		     udsp_ec_nlms_verdict_name(got->nlp_verdict),
		     rec->flags & LECR_FLAGS_VERDICT,
		     got->flags & LECR_FLAGS_VERDICT, st->out_delta, names);
}

static void counts_print(const struct replay_opts *o,
			 const struct lecr_tally *live,
			 const struct lecr_tally *replay)
{
	unsigned i;

	report_print(o, "  %-8s %8s %8s %8s %8s\n", "count", "live", "replay",
		     "delta", "percent");
	for (i = 0; i < LECR_COUNTS; i++) {
		long delta = (long)replay->n[i] - (long)live->n[i];

		if (!live->n[i]) {
			report_print(o, "  %-8s %8" PRIu32 " %8" PRIu32
				     " %8ld %8s\n", lecr_count_name(i),
				     live->n[i], replay->n[i], delta, "-");
			continue;
		}
		report_print(o, "  %-8s %8" PRIu32 " %8" PRIu32 " %8ld %8.2f\n",
			     lecr_count_name(i), live->n[i], replay->n[i],
			     delta, 100.0 * (double)delta / live->n[i]);
	}
}

static double share_get(uint32_t part, uint32_t whole)
{
	return whole ? 100.0 * part / whole : 0.0;
}

static void segment_state_print(const struct replay_opts *o,
				const struct lecr_segment *s)
{
	unsigned faults = lecr_segment_faults(s);

	if (s->lossy) {
		report_print(o, "  segment: loss, not evaluated\n");
		return;
	}
	if (!s->body) {
		report_print(o, "  segment: no frame after the skip, "
			     "not evaluated\n");
		return;
	}
	if (!faults) {
		report_print(o, "  segment: pass\n");
		return;
	}
	report_print(o, "  segment: fail (%s%s%s)\n",
		     (faults & LECR_FAULT_COUNTS) ? "counts " : "",
		     (faults & LECR_FAULT_FIRED) ? "fired " : "",
		     (faults & LECR_FAULT_VERDICT) ? "verdict" : "");
}

static void segment_print(const struct replay_opts *o,
			  const struct lecr_segment *s)
{
	char names[64];

	report_print(o, "segment %u, from %s, ticks %" PRIu64 " to %" PRIu64
		     ": %" PRIu32 " RUN frames, %" PRIu32 " after %u s\n",
		     s->index, s->from_reset ? "RESET" : "NEW", s->tick_first,
		     s->tick_last, s->runs, s->body, o->skip_s);
	counts_print(o, &s->live, &s->replay);
	report_print(o, "  fired symmetric difference %" PRIu32 " (live %"
		     PRIu32 ", bound %" PRIu32 ")\n", s->fired_diff,
		     s->fired_live, lecr_bound_get(s->fired_live));
	report_print(o, "  dt symmetric difference %" PRIu32 "\n", s->dt_diff);
	report_print(o, "  nlp_verdict agreement %" PRIu32 " of %" PRIu32
		     " (%.2f%%)\n", s->verdict_same, s->body,
		     share_get(s->verdict_same, s->body));
	if (s->differs) {
		diff_names_get(s->diff, names, sizeof(names));
		report_print(o, "  first difference at frame %" PRIu32
			     ", tick %" PRIu64 ": %s\n", s->diff_frames,
			     s->diff_tick, names);
	} else {
		report_print(o, "  first difference: none\n");
	}
	report_print(o, "  frames agreement %" PRIu32 " of %" PRIu32 "\n",
		     s->frames_same, s->runs);
	report_print(o, "  out agreement %" PRIu32 " of %" PRIu32
		     " identical, largest delta %u\n", s->out_same, s->runs,
		     s->out_delta);
	segment_state_print(o, s);
}

static void frame_view_fill(struct frame_view *v, const struct lecr_record *rec,
			    const struct lecr_replay *r, uint32_t index)
{
	v->tick = rec->tick;
	v->index = index;
	udsp_ec_nlms_frame_get(r->ec, &v->reading);
	udsp_ec_nlms_info_get(r->ec, &v->info);
	/* The arithmetic of the canceller (dsp/lec.h), so a frame that the
	 * canceller passes unchanged reads out_power equal to mic_power, to
	 * the bit. The step refuses a RUN record with 0 samples. */
	v->out_power = udsp_ec_nlms_frame_power_get(r->out, rec->samples);
}

/* The cells of a --frames line. On the header line a cell prints its name. */
struct frames_line {
	FILE *f;
	bool head;
	bool more;
};

static void frames_text_print(struct frames_line *l, const char *name,
			      const char *value)
{
	fprintf(l->f, "%s%s", l->more ? "," : "", l->head ? name : value);
	l->more = true;
}

static void frames_uint_print(struct frames_line *l, const char *name,
			      uint64_t value)
{
	char text[24];

	snprintf(text, sizeof(text), "%" PRIu64, value);
	frames_text_print(l, name, text);
}

static void frames_float_print(struct frames_line *l, const char *name,
			       double value)
{
	char text[32];

	snprintf(text, sizeof(text), "%.9g", value);
	frames_text_print(l, name, text);
}

/*
 * The header line of --frames when head is set, else the row of v. A column
 * is one call, so the header and the rows name the same columns in the same
 * order.
 */
static void frames_line_print(FILE *f, bool head, const struct frame_view *v)
{
	const struct udsp_ec_nlms_frame *fr = &v->reading;
	const struct udsp_ec_nlms_info *in = &v->info;
	struct frames_line l = { .f = f, .head = head };

	frames_uint_print(&l, "tick", v->tick);
	frames_uint_print(&l, "frames", in->frames);
	frames_text_print(&l, "verdict",
			  udsp_ec_nlms_verdict_name(fr->nlp_verdict));
	frames_uint_print(&l, "nlp_active", in->nlp_active);
	frames_uint_print(&l, "fired", fr->dt_fired);
	frames_uint_print(&l, "dt", in->double_talk);
	frames_uint_print(&l, "hold_left", fr->hold_left);
	frames_uint_print(&l, "probing", fr->probing);
	frames_text_print(&l, "trial_arm", lecr_trial_arm_name(fr->trial_arm));
	frames_text_print(&l, "trial_end", lecr_trial_end_name(fr->trial_end));
	frames_uint_print(&l, "probes", in->probes);
	frames_uint_print(&l, "diverge_resets", in->diverge_resets);
	frames_uint_print(&l, "dt_run", fr->dt_run);
	frames_uint_print(&l, "stale_run", fr->stale_run);
	frames_uint_print(&l, "trusted", fr->trusted);
	frames_uint_print(&l, "classic_arm", fr->classic_arm);
	frames_float_print(&l, "erle_db", fr->erle_db);
	frames_float_print(&l, "echo_gain", in->echo_gain);
	frames_float_print(&l, "coef_energy", in->coef_energy);
	frames_float_print(&l, "noise_dbov", in->noise_dbov);
	frames_float_print(&l, "bar", fr->bar);
	frames_float_print(&l, "ref_power", fr->ref_power);
	frames_float_print(&l, "window_power", fr->window_power);
	frames_float_print(&l, "mic_power", fr->mic_power);
	frames_float_print(&l, "estimate_power", fr->estimate_power);
	frames_float_print(&l, "out_power", v->out_power);
	frames_uint_print(&l, "talker", fr->talker);
	frames_float_print(&l, "ref_bar", fr->ref_bar);
	frames_float_print(&l, "ref_bar_max", fr->ref_bar_max);
	frames_uint_print(&l, "background", fr->background);
	frames_float_print(&l, "ref_floor", in->ref_floor);
	frames_uint_print(&l, "trust_run", fr->trust_run);
	fputc('\n', f);
}

static int frames_open(struct replay_out *out, const char *path)
{
	static const struct frame_view none;

	if (!path)
		return 0;
	out->frames = fopen(path, "w");
	if (!out->frames) {
		fprintf(stderr, "lec_replay: cannot write %s\n", path);
		return -EIO;
	}
	frames_line_print(out->frames, true, &none);
	return 0;
}

static int frames_close(struct replay_out *out, const char *path)
{
	int err = 0;

	if (!out->frames)
		return 0;
	if (ferror(out->frames))
		err = -EIO;
	if (fclose(out->frames))
		err = -EIO;
	out->frames = NULL;
	if (err)
		fprintf(stderr, "lec_replay: cannot write %s\n", path);
	return err;
}

static double dbfs_power_get(double dbfs)
{
	return CENSUS_FULL_SCALE * CENSUS_FULL_SCALE * pow(10.0, dbfs / 10.0);
}

static double power_dbfs_get(double power)
{
	return 10.0 * log10(power / (CENSUS_FULL_SCALE * CENSUS_FULL_SCALE));
}

/* 10 log10(num / den), and NAN when both are 0. */
static double ratio_db_get(double num, double den)
{
	if (!(num > 0.0) && !(den > 0.0))
		return NAN;
	return 10.0 * log10(num / den);
}

/* The far bar reads the reference, so it moves down by the loss, and the
 * cap of the bound factor rises by it. A loss of 0 leaves both figures as
 * they are, bit for bit. */
static void census_init(struct census *c, uint32_t first, uint32_t end,
			unsigned ref_loss_half_db)
{
	double loss_db = ref_loss_half_db / 2.0;

	memset(c, 0, sizeof(*c));
	c->first = first;
	c->end = end;
	c->gain_min = CENSUS_GAIN_MIN / pow(10.0, -loss_db / 10.0);
	c->far_power = dbfs_power_get(CENSUS_FAR_DBFS - loss_db);
	c->leak_power = dbfs_power_get(CENSUS_LEAK_DBFS);
	c->expose_power = dbfs_power_get(CENSUS_EXPOSE_DBFS);
}

static void census_mark_get(const struct census *c,
			    const struct frame_view *v, struct census_mark *m)
{
	const struct udsp_ec_nlms_frame *fr = &v->reading;
	double guard = fr->window_power > c->window_last ? fr->window_power :
							   c->window_last;
	double factor = CENSUS_GAIN_HEADROOM * v->info.echo_gain;
	double ref = fr->ref_power;
	unsigned i;

	for (i = 0; i < CENSUS_HISTORY; i++) {
		if (c->ref_last[i] > ref)
			ref = c->ref_last[i];
	}
	if (factor > c->gain_min)
		factor = c->gain_min;
	m->far = ref >= c->far_power;
	m->leak = m->far && v->out_power > c->leak_power;
	m->low = guard < c->far_power;
	m->near = fr->mic_power >= CENSUS_NEAR_RATIO * guard * factor;
	m->exposure = m->low && fr->mic_power >= c->expose_power &&
		      fr->estimate_power >= fr->mic_power / CENSUS_EXPOSE_RATIO;
}

/*
 * A NEW record starts a new instance, and the tick can jump there. A RESET
 * keeps the history, because the reference before it still returns as echo
 * after it.
 */
static void census_history_clear(struct census *c)
{
	memset(c->ref_last, 0, sizeof(c->ref_last));
	c->window_last = 0.0f;
}

static void census_history_push(struct census *c, const struct frame_view *v)
{
	memmove(c->ref_last + 1, c->ref_last,
		sizeof(c->ref_last) - sizeof(c->ref_last[0]));
	c->ref_last[0] = v->reading.ref_power;
	c->window_last = v->reading.window_power;
}

static void census_sel_add(struct census_sel *s, const struct frame_view *v,
			   const struct census_mark *m)
{
	const struct udsp_ec_nlms_frame *fr = &v->reading;

	s->frames++;
	if (fr->nlp_verdict <= LECR_VERDICT_PASS)
		s->verdict[fr->nlp_verdict]++;
	s->nlp_active += v->info.nlp_active;
	s->dt_flag += v->info.double_talk;
	s->fired += fr->dt_fired;
	s->leak += m->leak;
	s->exposure += m->exposure;
	s->out_sum += v->out_power;
	s->mic_sum += fr->mic_power;
	if (v->out_power > s->out_max)
		s->out_max = v->out_power;
}

/* A memory failure stops the list of the seconds and exits 2 at the end. */
static void census_second_close(struct census *c)
{
	struct census_second *grown;
	size_t room;

	if (!c->now_open)
		return;
	c->now_open = false;
	if (c->now.far < CENSUS_SECOND_FAR_MIN || c->err)
		return;
	if (c->count == c->room) {
		room = c->room ? 2u * c->room : 64u;
		grown = realloc(c->seconds, room * sizeof(*grown));
		if (!grown) {
			c->err = -ENOMEM;
			return;
		}
		c->seconds = grown;
		c->room = room;
	}
	c->seconds[c->count++] = c->now;
}

static void census_second_add(struct census *c, const struct frame_view *v,
			      const struct census_mark *m)
{
	uint32_t second = v->index / LECR_TICKS_PER_SECOND;

	if (c->now_open && c->now.second != second)
		census_second_close(c);
	if (!c->now_open) {
		memset(&c->now, 0, sizeof(c->now));
		c->now.second = second;
		c->now_open = true;
	}
	if (v->reading.nlp_verdict == LECR_VERDICT_DT)
		c->now.dt++;
	if (!m->far)
		return;
	c->now.far++;
	c->now.mic_sum += v->reading.mic_power;
	c->now.out_sum += v->out_power;
}

static void census_add(struct census *c, const struct frame_view *v)
{
	struct census_mark m;

	census_mark_get(c, v, &m);
	census_history_push(c, v);
	if (v->index < c->first || (c->end && v->index >= c->end))
		return;
	c->runs++;
	census_sel_add(&c->all, v, &m);
	if (m.far)
		census_sel_add(&c->far, v, &m);
	if (m.low)
		census_sel_add(&c->low, v, &m);
	if (m.near)
		census_sel_add(&c->near, v, &m);
	if (m.far && !m.near)
		census_sel_add(&c->far_not_near, v, &m);
	census_second_add(c, v, &m);
}

static void census_range_print(const struct replay_opts *o,
			       const struct census *c)
{
	char end[16] = "";

	if (c->end)
		snprintf(end, sizeof(end), "%" PRIu32, c->end);
	report_print(o, "census range %" PRIu32 ":%s frames %" PRIu32 "\n",
		     c->first, end, c->runs);
}

/* extra ends the line: the keys that one selection adds. */
static void census_sel_print(const struct replay_opts *o, const char *name,
			     const struct census_sel *s, const char *extra)
{
	report_print(o, "census sel %s frames %" PRIu32 " off %" PRIu32
		     " quiet %" PRIu32 " dt %" PRIu32 " unlike %" PRIu32
		     " erle %" PRIu32 " pass %" PRIu32 " nlp_active %" PRIu32
		     " dt_flag %" PRIu32 " fired %" PRIu32 " leak %" PRIu32
		     " exposure %" PRIu32 "%s\n", name, s->frames,
		     s->verdict[LECR_VERDICT_OFF],
		     s->verdict[LECR_VERDICT_QUIET],
		     s->verdict[LECR_VERDICT_DT],
		     s->verdict[LECR_VERDICT_UNLIKE],
		     s->verdict[LECR_VERDICT_ERLE],
		     s->verdict[LECR_VERDICT_PASS], s->nlp_active, s->dt_flag,
		     s->fired, s->leak, s->exposure, extra);
}

static void census_far_not_near_print(const struct replay_opts *o,
				      const struct census_sel *s)
{
	report_print(o, "census sel far_not_near frames %" PRIu32
		     " nlp_active %" PRIu32 " leak %" PRIu32 " out_sum %.9g"
		     " out_max %.9g out_max_dbfs %.2f\n", s->frames,
		     s->nlp_active, s->leak, s->out_sum, s->out_max,
		     power_dbfs_get(s->out_max));
}

static void census_second_print(const struct replay_opts *o,
				const struct census_second *s)
{
	report_print(o, "census second %" PRIu32 " far %" PRIu32 " dt %" PRIu32
		     " loss_db %.2f\n", s->second, s->far, s->dt,
		     ratio_db_get(s->mic_sum, s->out_sum));
}

static bool census_lockout_get(const struct census_second *s)
{
	if (s->second < CENSUS_LOCKOUT_FIRST || s->second > CENSUS_LOCKOUT_LAST)
		return false;
	if (s->far < CENSUS_SECOND_FAR_MIN || s->dt < CENSUS_LOCKOUT_DT_MIN)
		return false;
	return ratio_db_get(s->mic_sum, s->out_sum) < CENSUS_LOCKOUT_LOSS_DB;
}

static void census_lockout_print(const struct replay_opts *o,
				 const struct census *c)
{
	char list[160] = "";
	size_t used = 0;
	unsigned count = 0;
	size_t i;

	for (i = 0; i < c->count && used < sizeof(list); i++) {
		if (!census_lockout_get(&c->seconds[i]))
			continue;
		count++;
		used += (size_t)snprintf(list + used, sizeof(list) - used,
					 "%s%" PRIu32, used ? "," : "",
					 c->seconds[i].second);
	}
	report_print(o, "census lockout %u seconds %s\n", count,
		     count ? list : "-");
}

static void census_print(const struct replay_opts *o, const struct census *c)
{
	char extra[160];
	double per_min = 0.0;
	size_t i;

	if (c->runs)
		per_min = (double)c->far.leak * CENSUS_MINUTE_FRAMES / c->runs;
	census_range_print(o, c);
	census_sel_print(o, "all", &c->all, "");
	snprintf(extra, sizeof(extra), " out_sum %.9g out_max %.9g "
		 "out_max_dbfs %.2f leak_per_min %.2f", c->far.out_sum,
		 c->far.out_max, power_dbfs_get(c->far.out_max), per_min);
	census_sel_print(o, "far", &c->far, extra);
	census_sel_print(o, "low", &c->low, "");
	snprintf(extra, sizeof(extra), " out_sum %.9g mic_sum %.9g "
		 "out_mic_db %.2f", c->near.out_sum, c->near.mic_sum,
		 ratio_db_get(c->near.out_sum, c->near.mic_sum));
	census_sel_print(o, "near", &c->near, extra);
	census_far_not_near_print(o, &c->far_not_near);
	for (i = 0; i < c->count; i++)
		census_second_print(o, &c->seconds[i]);
	census_lockout_print(o, c);
}

static void record_report(void *ctx, const struct lecr_record *rec,
			  const struct lecr_replay *r,
			  const struct lecr_step *st,
			  const struct lecr_agree *a, bool closed)
{
	struct replay_out *out = ctx;
	const struct replay_opts *o = out->opts;
	struct frame_view v;

	if (closed)
		segment_print(o, &a->done);
	if (!rec)
		return;
	if (out->wav)
		wav_record_write(out, rec, r, st);
	if (o->csv)
		csv_row_print(rec, r, st);
	if (o->verdicts && st->replayed && st->diff)
		verdict_line_print(o, rec, r, st);
	if (st->fresh)
		census_history_clear(&out->census);
	if (!st->replayed || rec->kind != LECR_KIND_RUN)
		return;
	frame_view_fill(&v, rec, r, ++out->index);
	if (out->frames)
		frames_line_print(out->frames, false, &v);
	census_add(&out->census, &v);
}

static void head_print(const struct replay_opts *o,
		       const struct lecr_file *file,
		       const struct replay_scan *sc)
{
	report_print(o, "file %s\n", o->path);
	report_print(o, "line %u, rate %" PRIu32 " Hz, tail %u ms%s\n",
		     file->line, file->rate, sc->tail_ms,
		     sc->tail_mixed ? " (not the same on every instance)" : "");
	report_print(o, "reference loss %u half-dB%s\n",
		     lecr_file_ref_loss_get(file),
		     (file->flags & LECR_FILE_REF_LOSS) ? "" :
		     " (the file carries none)");
	report_print(o, "records %" PRIu32 ", instances %" PRIu32 ", resets %"
		     PRIu32 ", losses %" PRIu32 " (%" PRIu64 " records)\n",
		     sc->records, sc->instances, sc->resets, sc->losses,
		     sc->lost);
}

static void totals_print(const struct replay_opts *o,
			 const struct lecr_agree *a)
{
	report_print(o, "total: %u segments, %" PRIu32 " evaluated, %" PRIu32
		     " failed; %" PRIu32 " RUN frames replayed, %" PRIu32
		     " frames mismatches, %" PRIu32 " gaps, %" PRIu32
		     " records skipped\n", a->segments, a->evaluated, a->failed,
		     a->runs, a->frames_bad, a->gaps, a->skipped);
	report_print(o, "total over the evaluated bodies: %" PRIu32 " frames, "
		     "nlp_verdict agreement %.2f%%, fired difference %" PRIu32
		     ", dt difference %" PRIu32 "\n", a->body,
		     share_get(a->verdict_same, a->body), a->fired_diff,
		     a->dt_diff);
	counts_print(o, &a->live, &a->replay);
}

static void reason_print(const struct replay_opts *o,
			 const struct lecr_agree *a)
{
	if (a->parse_error && a->refusal)
		report_print(o, "reason: the file does not parse: record %"
			     PRIu32 " has %s\n", a->records + 1u, a->refusal);
	else if (a->parse_error)
		report_print(o, "reason: the file does not parse\n");
	else if (!a->first_new)
		report_print(o, "reason: the first record is not NEW\n");
	else if (a->frames_bad)
		report_print(o, "reason: frames disagree on %" PRIu32
			     " RUN records, so the controls are incomplete\n",
			     a->frames_bad);
	else if (a->gaps)
		report_print(o, "reason: %" PRIu32 " tick gaps without a "
			     "recorded loss\n", a->gaps);
	else if (a->failed)
		report_print(o, "reason: %" PRIu32 " segments outside the "
			     "criteria\n", a->failed);
	else if (!a->evaluated)
		report_print(o, "reason: no segment was evaluated\n");
}

static int residual_open(struct replay_out *out, const char *path,
			 uint32_t rate)
{
	out->wav_max = wav_max_get();
	out->wav = fopen(path, "wb");
	if (!out->wav || wav_head_write(out->wav, rate, 0)) {
		fprintf(stderr, "lec_replay: cannot write %s\n", path);
		return -EIO;
	}
	return 0;
}

/* A refused residual goes, so no truncated file reads as a whole one. */
static int residual_close(struct replay_out *out, const char *path,
			  uint32_t rate)
{
	int err = out->err;

	if (!out->wav)
		return 0;
	if (out->gaps_cut)
		report_print(out->opts, "note: %" PRIu32 " runs of zeros cut "
			     "to %u s in the residual\n", out->gaps_cut,
			     WAV_GAP_FRAMES_MAX / LECR_TICKS_PER_SECOND);
	if (fseek(out->wav, 0, SEEK_SET) ||
	    wav_head_write(out->wav, rate, (uint32_t)out->wav_bytes))
		err = err ? err : -EIO;
	if (fclose(out->wav))
		err = err ? err : -EIO;
	out->wav = NULL;
	if (err != -EFBIG)
		return err;
	remove(path);
	fprintf(stderr, "lec_replay: %s needs more than the %" PRIu64 " data "
		"bytes of a WAV file, not written\n", path, out->wav_max);
	return err;
}

static int status_get(enum lecr_result result)
{
	switch (result) {
	case LECR_PASS:
		return 0;
	case LECR_FAIL:
		return 1;
	case LECR_INCOMPLETE:
		return 3;
	}
	return 2;
}

/* Each output that failed names itself on stderr, and the call then returns
 * nonzero. */
static int outputs_close(struct replay_out *out, const struct replay_opts *o,
			 uint32_t rate)
{
	int err = residual_close(out, o->residual, rate);

	if (err && err != -EFBIG)
		fprintf(stderr, "lec_replay: cannot write %s\n", o->residual);
	if (frames_close(out, o->frames))
		err = err ? err : -EIO;
	if (out->census.err) {
		fprintf(stderr, "lec_replay: no memory for the census\n");
		err = err ? err : out->census.err;
	}
	free(out->census.seconds);
	out->census.seconds = NULL;
	return err;
}

int main(int argc, char **argv)
{
	struct replay_opts o;
	struct replay_scan sc;
	struct replay_out out;
	struct lecr_replay r;
	struct lecr_agree a;
	struct lecr_file file;
	enum lecr_result result;
	FILE *f;
	int err;

	if (opts_parse(argc, argv, &o)) {
		usage_print(stderr);
		return 2;
	}
	f = fopen(o.path, "rb");
	if (!f) {
		fprintf(stderr, "lec_replay: cannot open %s: %s\n", o.path,
			strerror(errno));
		return 2;
	}
	scan_run(f, &sc);
	rewind(f);
	if (lecr_read_open(f, &file))
		memset(&file, 0, sizeof(file));
	else if (!(file.flags & LECR_FILE_REF_LOSS))
		fprintf(stderr, "lec_replay: %s carries no reference loss, "
			"replayed at 0\n", o.path);
	rewind(f);
	memset(&out, 0, sizeof(out));
	out.opts = &o;
	out.samples = file.samples;
	census_init(&out.census, o.range_first, o.range_end,
		    lecr_file_ref_loss_get(&file));
	if (frames_open(&out, o.frames)) {
		fclose(f);
		return 2;
	}
	if (o.residual && file.samples &&
	    residual_open(&out, o.residual, file.rate)) {
		frames_close(&out, o.frames);
		fclose(f);
		return 2;
	}
	head_print(&o, &file, &sc);
	if (o.csv)
		csv_head_print();
	lecr_replay_init(&r, out.wav != NULL);
	lecr_agree_init(&a, o.skip_s);
	err = lecr_stream_replay(f, &file, &r, &a, record_report, &out);
	fclose(f);
	if (err)
		report_print(&o, "error: %s after %" PRIu32 " records\n",
			     strerror(-err), a.records);
	totals_print(&o, &a);
	census_second_close(&out.census);
	census_print(&o, &out.census);
	result = lecr_agree_result(&a);
	reason_print(&o, &a);
	report_print(&o, "result: %s\n", lecr_result_name(result));
	if (outputs_close(&out, &o, file.rate))
		return 2;
	return status_get(result);
}
