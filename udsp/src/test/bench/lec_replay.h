// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The replay of a canceller capture on the host, shared by the program
 * test/bench/lec_replay.c and the suites. A capture holds one record per
 * engine tick of a line: the reference, the microphone and the output of the
 * frame, the controls the tick applied and the verdicts the canceller gave.
 * The replay runs the records through dsp/lec.c under the same controls and
 * compares its verdicts and its output with the recorded ones.
 *
 * The extracted file (.lecr) is little-endian whatever the host is. The
 * reader and the writer encode each field at a fixed offset, so the format
 * does not depend on the layout of a host struct.
 *
 * File header, 32 bytes:
 *
 *   offset  width  field
 *   0       8      magic "UDSPLECR"
 *   8       2      version, 2; a reader also takes 1
 *   10      2      line
 *   12      4      rate in Hz, 8000 or 16000
 *   16      2      samples of each plane, rate / 100
 *   18      2      ref_loss_half_db, 0 to 60, valid with flag bit 1
 *   20      4      record count
 *   24      4      flags; bit 0: the first record is NEW; bit 1: offset 18
 *                  holds the reference loss
 *   28      4      reserved, 0
 *
 * Version 2 adds the reason SKIP_SYNTHETIC to a FEED: the tick rendered a
 * period of the refill of a restart and had no capture period, so its mic
 * plane reads zero. A version 1 file carries no such FEED, and the reader
 * refuses a record of a version 1 file that sets the bit.
 *
 * ref_loss_half_db is the loss in half-dB steps that the engine applied ahead
 * of the reference of the canceller (dsp/lec.h, udsp_ec_nlms_ref_loss_set).
 * The replay gives it to each instance it creates. A file with flag bit 1
 * clear replays at a loss of 0, because the captures without the field were
 * taken with the loss behind the reference.
 *
 * Record, 64 + 6 x samples bytes: the header below, then the planes ref, mic
 * and out of samples int16 each. A FEED record has zeros in out. An END
 * record has zeros in all three planes, so every record has one size.
 *
 *   offset  width  field
 *   0       8      stamp_ns, CLOCK_MONOTONIC of the tick
 *   8       8      tick, the engine tick index
 *   16      4      lost_before, records of the line lost before this one
 *   20      4      frames of the instance after the record
 *   24      4      hold_frames of the instance after the record
 *   28      4      erle_mdb, signed
 *   32      4      echo_gain_micro
 *   36      4      coef_energy_micro
 *   40      4      ref_delay_samples
 *   44      2      gen
 *   46      2      flags (enum lecr_flag)
 *   48      2      samples
 *   50      2      tail_ms
 *   52      2      noise_ddbov, tenths of dB; 1270 is unmeasured
 *   54      2      probes
 *   56      2      diverge_resets
 *   58      1      line
 *   59      1      kind (enum lecr_kind)
 *   60      1      nlp_verdict (enum lecr_verdict)
 *   61      1      hold_left
 *   62      2      skipped, periods the engine skipped before the record
 *   64      2 x samples   ref
 *   64 + 2 x samples      mic
 *   64 + 4 x samples      out
 *
 * The header has no reserved byte. The adapt verdict of a frame is the
 * difference of hold_frames to the previous record, and 0 after NEW or RESET.
 *
 * These rules are the rules of both readers. src/tools/lec-capture.py applies
 * them to the entries of a call trace, and its docstring lists them again.
 *
 * The reader refuses a record that breaks a rule of one record. The file then
 * does not parse, and the replay reads INCOMPLETE. The refused records:
 * - a kind above END;
 * - a line other than the line of the file header;
 * - a RUN or a FEED whose samples differ from the file header, or an END
 *   whose samples are not 80 or 160;
 * - a FEED without exactly one reason bit, or a RUN or an END with one;
 * - SKIP_SYNTHETIC in a version 1 file;
 * - SKIPPED without a skipped count, or a skipped count without SKIPPED;
 * - a skipped count on NEW or on an END.
 *
 * A lineage holds the records of one instance, from its NEW to its END. The
 * step applies the rules of a lineage to a record in this order:
 * 1. A nonzero lost_before is a loss, with SKIPPED or without it. It breaks
 *    the lineage until the next NEW. A loss on NEW belongs to the lineage
 *    before it, because the lost records can hold the END of that lineage.
 * 2. A record without a loss that crosses a lineage boundary without its mark
 *    is a gap. A NEW while a lineage is open misses the END before it. A
 *    record without NEW after an END misses its NEW, and the step skips the
 *    joined records up to the next NEW. The first record of a file crosses
 *    no boundary: a file whose first record is not NEW reads INCOMPLETE.
 * 3. In an intact lineage, the tick of a RUN or a FEED without NEW must follow
 *    the tick of the previous record by one. On a record with SKIPPED, it
 *    must follow by skipped + lost_before + 1: the engine ran no tick in the
 *    skipped periods. Any other jump is a gap.
 * 4. In an intact lineage, a FEED or an END without a loss carries the frames
 *    of the previous record. A FEED with NEW carries 0. A FEED adds no frame,
 *    and an END reads the instance after its last frame. Any other count is
 *    a gap. A RUN adds one frame, or reads 1 on RESET, and the replay
 *    compares that count with its own (frames_bad).
 * 5. NEW destroys the old instance and creates one at the recorded tail and
 *    rate.
 * 6. With no instance, or in a broken lineage, the record is skipped.
 * 7. FEED takes the reference into the history of the instance.
 * 8. RUN resets the instance on RESET, sets the processor and the freeze
 *    from the flags, and runs the microphone. The replay then reads the
 *    frame and compares it with the record.
 * 9. END closes the lineage.
 *
 * A gap breaks the lineage like a loss, because records between the two are
 * lost from the archive. One loss stays invisible: a FEED lost from the
 * archive just before an END. An END carries no skipped count and follows no
 * tick rule, and a FEED adds no frame.
 *
 * A loss invalidates the replay until the next NEW, not until the next
 * RESET, because udsp_ec_nlms_reset keeps the floor, the lifetime counters
 * and the comfort noise state.
 */
#ifndef UDSP_TEST_LEC_REPLAY_H
#define UDSP_TEST_LEC_REPLAY_H

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../dsp/dsp.h"
#include "../../dsp/lec.h"

#define LECR_MAGIC		"UDSPLECR"
#define LECR_VERSION		2u
#define LECR_VERSION_MIN	1u
#define LECR_FILE_BYTES		32u
#define LECR_HEAD_BYTES		64u
#define LECR_SAMPLES_MAX	UDSP_FRAME_MAX
#define LECR_RECORD_BYTES_MAX	(LECR_HEAD_BYTES + 6u * LECR_SAMPLES_MAX)
#define LECR_FILE_FIRST_NEW	(1u << 0)
#define LECR_FILE_REF_LOSS	(1u << 1)
/* The engine tick is 10 ms at every line rate (core/clock.h). */
#define LECR_TICKS_PER_SECOND	100u
#define LECR_NOISE_UNMEASURED	1270u
/* The seconds of each segment that the steady-state figures leave out, by
 * default (lec_replay --skip-s). */
#define LECR_SKIP_S_DEFAULT	15u

enum lecr_kind {
	LECR_KIND_RUN = 0,
	LECR_KIND_FEED = 1,
	LECR_KIND_END = 2,
};

/* NEW, RESET, FREEZE and NLP are the controls of the tick. FIRED, DT, ACTIVE
 * and PROBING are the verdicts the canceller gave on a RUN. The five SKIP
 * flags name the reason of a FEED. The reader checks them and the replay
 * reads them no further. SKIPPED marks a RUN or a FEED whose skipped field is
 * not 0 (step rule 3). */
enum lecr_flag {
	LECR_FLAG_NEW = 1u << 0,
	LECR_FLAG_RESET = 1u << 1,
	LECR_FLAG_FREEZE = 1u << 2,
	LECR_FLAG_NLP = 1u << 3,
	LECR_FLAG_FIRED = 1u << 4,
	LECR_FLAG_DT = 1u << 5,
	LECR_FLAG_ACTIVE = 1u << 6,
	LECR_FLAG_PROBING = 1u << 7,
	LECR_FLAG_SKIP_OPEN = 1u << 8,
	LECR_FLAG_SKIP_MODEM = 1u << 9,
	LECR_FLAG_SKIP_OFF = 1u << 10,
	LECR_FLAG_SKIP_INACTIVE = 1u << 11,
	LECR_FLAG_SKIPPED = 1u << 12,
	LECR_FLAG_SKIP_SYNTHETIC = 1u << 13,
};

#define LECR_FLAGS_VERDICT	(LECR_FLAG_FIRED | LECR_FLAG_DT | \
				 LECR_FLAG_ACTIVE | LECR_FLAG_PROBING)
#define LECR_FLAGS_REASON	(LECR_FLAG_SKIP_OPEN | LECR_FLAG_SKIP_MODEM | \
				 LECR_FLAG_SKIP_OFF | LECR_FLAG_SKIP_INACTIVE | \
				 LECR_FLAG_SKIP_SYNTHETIC)

/*
 * The order of the capture record. dsp/lec.c keeps a private enum in the
 * same order and writes its value to the frame reading, so test_lec checks
 * udsp_ec_nlms_verdict_name against these words.
 */
enum lecr_verdict {
	LECR_VERDICT_OFF,
	LECR_VERDICT_QUIET,
	LECR_VERDICT_DT,
	LECR_VERDICT_UNLIKE,
	LECR_VERDICT_ERLE,
	LECR_VERDICT_PASS,
};

struct lecr_file {
	uint16_t version;
	uint16_t line;
	uint32_t rate;
	uint16_t samples;
	uint16_t ref_loss_half_db;
	uint32_t records;
	uint32_t flags;
};

/* One record in host form, in the field order of the table above. The
 * struct has no padding, so two records compare with memcmp. Samples past
 * samples read 0. */
struct lecr_record {
	uint64_t stamp_ns;
	uint64_t tick;
	uint32_t lost_before;
	uint32_t frames;
	uint32_t hold_frames;
	int32_t erle_mdb;
	uint32_t echo_gain_micro;
	uint32_t coef_energy_micro;
	uint32_t ref_delay_samples;
	uint16_t gen;
	uint16_t flags;
	uint16_t samples;
	uint16_t tail_ms;
	uint16_t noise_ddbov;
	uint16_t probes;
	uint16_t diverge_resets;
	uint8_t line;
	uint8_t kind;
	uint8_t nlp_verdict;
	uint8_t hold_left;
	uint16_t skipped;
	int16_t ref[LECR_SAMPLES_MAX];
	int16_t mic[LECR_SAMPLES_MAX];
	int16_t out[LECR_SAMPLES_MAX];
};

_Static_assert(sizeof(struct lecr_record) == 1024u,
	       "a host record has padding");

static inline void lecr_le16_put(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void lecr_le32_put(uint8_t *p, uint32_t v)
{
	lecr_le16_put(p, (uint16_t)v);
	lecr_le16_put(p + 2, (uint16_t)(v >> 16));
}

static inline void lecr_le64_put(uint8_t *p, uint64_t v)
{
	lecr_le32_put(p, (uint32_t)v);
	lecr_le32_put(p + 4, (uint32_t)(v >> 32));
}

static inline uint16_t lecr_le16_get(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t lecr_le32_get(const uint8_t *p)
{
	return lecr_le16_get(p) | ((uint32_t)lecr_le16_get(p + 2) << 16);
}

static inline uint64_t lecr_le64_get(const uint8_t *p)
{
	return lecr_le32_get(p) | ((uint64_t)lecr_le32_get(p + 4) << 32);
}

static inline bool lecr_rate_valid(uint32_t rate)
{
	return rate == UDSP_SAMPLE_RATE || rate == UDSP_SAMPLE_RATE_WB;
}

static inline void lecr_file_encode(uint8_t *b, const struct lecr_file *file)
{
	memset(b, 0, LECR_FILE_BYTES);
	memcpy(b, LECR_MAGIC, 8);
	lecr_le16_put(b + 8, LECR_VERSION);
	lecr_le16_put(b + 10, file->line);
	lecr_le32_put(b + 12, file->rate);
	lecr_le16_put(b + 16, file->samples);
	lecr_le16_put(b + 18, file->ref_loss_half_db);
	lecr_le32_put(b + 20, file->records);
	lecr_le32_put(b + 24, file->flags);
}

/* The writer sets the loss with its flag, so a file never carries a loss
 * that the reader ignores. */
static inline void lecr_file_ref_loss_set(struct lecr_file *file,
					  uint16_t half_db)
{
	file->ref_loss_half_db = half_db;
	file->flags |= LECR_FILE_REF_LOSS;
}

static inline unsigned lecr_file_ref_loss_get(const struct lecr_file *file)
{
	return (file->flags & LECR_FILE_REF_LOSS) ? file->ref_loss_half_db : 0u;
}

static inline void lecr_head_encode(uint8_t *b, const struct lecr_record *rec)
{
	memset(b, 0, LECR_HEAD_BYTES);
	lecr_le64_put(b + 0, rec->stamp_ns);
	lecr_le64_put(b + 8, rec->tick);
	lecr_le32_put(b + 16, rec->lost_before);
	lecr_le32_put(b + 20, rec->frames);
	lecr_le32_put(b + 24, rec->hold_frames);
	lecr_le32_put(b + 28, (uint32_t)rec->erle_mdb);
	lecr_le32_put(b + 32, rec->echo_gain_micro);
	lecr_le32_put(b + 36, rec->coef_energy_micro);
	lecr_le32_put(b + 40, rec->ref_delay_samples);
	lecr_le16_put(b + 44, rec->gen);
	lecr_le16_put(b + 46, rec->flags);
	lecr_le16_put(b + 48, rec->samples);
	lecr_le16_put(b + 50, rec->tail_ms);
	lecr_le16_put(b + 52, rec->noise_ddbov);
	lecr_le16_put(b + 54, rec->probes);
	lecr_le16_put(b + 56, rec->diverge_resets);
	b[58] = rec->line;
	b[59] = rec->kind;
	b[60] = rec->nlp_verdict;
	b[61] = rec->hold_left;
	lecr_le16_put(b + 62, rec->skipped);
}

static inline void lecr_head_decode(const uint8_t *b, struct lecr_record *rec)
{
	rec->stamp_ns = lecr_le64_get(b + 0);
	rec->tick = lecr_le64_get(b + 8);
	rec->lost_before = lecr_le32_get(b + 16);
	rec->frames = lecr_le32_get(b + 20);
	rec->hold_frames = lecr_le32_get(b + 24);
	rec->erle_mdb = (int32_t)lecr_le32_get(b + 28);
	rec->echo_gain_micro = lecr_le32_get(b + 32);
	rec->coef_energy_micro = lecr_le32_get(b + 36);
	rec->ref_delay_samples = lecr_le32_get(b + 40);
	rec->gen = lecr_le16_get(b + 44);
	rec->flags = lecr_le16_get(b + 46);
	rec->samples = lecr_le16_get(b + 48);
	rec->tail_ms = lecr_le16_get(b + 50);
	rec->noise_ddbov = lecr_le16_get(b + 52);
	rec->probes = lecr_le16_get(b + 54);
	rec->diverge_resets = lecr_le16_get(b + 56);
	rec->line = b[58];
	rec->kind = b[59];
	rec->nlp_verdict = b[60];
	rec->hold_left = b[61];
	rec->skipped = lecr_le16_get(b + 62);
}

/* A NULL plane writes zeros. */
static inline void lecr_plane_encode(uint8_t *b, const int16_t *plane,
				     unsigned samples)
{
	unsigned i;

	for (i = 0; i < samples; i++)
		lecr_le16_put(b + 2u * i, plane ? (uint16_t)plane[i] : 0u);
}

static inline void lecr_plane_decode(const uint8_t *b, int16_t *plane,
				     unsigned samples)
{
	unsigned i;

	for (i = 0; i < samples; i++)
		plane[i] = (int16_t)lecr_le16_get(b + 2u * i);
}

static inline int lecr_bytes_write(FILE *f, const void *p, size_t n)
{
	return fwrite(p, 1, n, f) == n ? 0 : -EIO;
}

/* The header goes out with a count of 0. lecr_write_close writes it again
 * with the count and the flags, so the stream must be seekable. */
static inline int lecr_write_open(FILE *f, struct lecr_file *file,
				  uint16_t line, uint32_t rate)
{
	uint8_t b[LECR_FILE_BYTES];

	if (!lecr_rate_valid(rate))
		return -EINVAL;
	memset(file, 0, sizeof(*file));
	file->version = LECR_VERSION;
	file->line = line;
	file->rate = rate;
	file->samples = (uint16_t)(rate / LECR_TICKS_PER_SECOND);
	lecr_file_encode(b, file);
	return lecr_bytes_write(f, b, sizeof(b));
}

static inline int lecr_write_record(FILE *f, struct lecr_file *file,
				    const struct lecr_record *rec)
{
	uint8_t b[LECR_RECORD_BYTES_MAX];
	unsigned n = file->samples;
	uint8_t *plane = b + LECR_HEAD_BYTES;
	bool end = rec->kind == LECR_KIND_END;

	if (!n || n > LECR_SAMPLES_MAX)
		return -EINVAL;
	lecr_head_encode(b, rec);
	lecr_plane_encode(plane, end ? NULL : rec->ref, n);
	lecr_plane_encode(plane + 2u * n, end ? NULL : rec->mic, n);
	lecr_plane_encode(plane + 4u * n,
			  rec->kind == LECR_KIND_RUN ? rec->out : NULL, n);
	if (lecr_bytes_write(f, b, LECR_HEAD_BYTES + 6u * n))
		return -EIO;
	if (!file->records && (rec->flags & LECR_FLAG_NEW))
		file->flags |= LECR_FILE_FIRST_NEW;
	file->records++;
	return 0;
}

/* The seek back goes to the saved end, not to SEEK_END: after a write at
 * the start, a glibc memory stream puts SEEK_END at the end of that write,
 * and the size it reports then drops the records. */
static inline int lecr_write_close(FILE *f, const struct lecr_file *file)
{
	uint8_t b[LECR_FILE_BYTES];
	long end = ftell(f);

	lecr_file_encode(b, file);
	if (end < 0 || fseek(f, 0, SEEK_SET) ||
	    lecr_bytes_write(f, b, sizeof(b)) || fseek(f, end, SEEK_SET) ||
	    fflush(f))
		return -EIO;
	return 0;
}

static inline int lecr_read_open(FILE *f, struct lecr_file *file)
{
	uint8_t b[LECR_FILE_BYTES];

	memset(file, 0, sizeof(*file));
	if (fread(b, 1, sizeof(b), f) != sizeof(b))
		return -EIO;
	if (memcmp(b, LECR_MAGIC, 8))
		return -EINVAL;
	file->version = lecr_le16_get(b + 8);
	file->line = lecr_le16_get(b + 10);
	file->rate = lecr_le32_get(b + 12);
	file->samples = lecr_le16_get(b + 16);
	file->ref_loss_half_db = lecr_le16_get(b + 18);
	file->records = lecr_le32_get(b + 20);
	file->flags = lecr_le32_get(b + 24);
	if (file->version < LECR_VERSION_MIN ||
	    file->version > LECR_VERSION || !lecr_rate_valid(file->rate) ||
	    file->samples * LECR_TICKS_PER_SECOND != file->rate)
		return -EINVAL;
	/* The canceller clamps a larger loss, so the file would replay under
	 * rules the capture never ran with. */
	if ((file->flags & LECR_FILE_REF_LOSS) &&
	    file->ref_loss_half_db > UDSP_EC_NLMS_REF_LOSS_MAX)
		return -EINVAL;
	return 0;
}

/* The rule of one record that rec breaks, in the list of the header comment,
 * or NULL. */
static inline const char *lecr_refusal_get(const struct lecr_file *file,
					   const struct lecr_record *rec)
{
	unsigned reason = rec->flags & LECR_FLAGS_REASON;
	bool one = reason && !(reason & (reason - 1u));
	bool skipped = (rec->flags & LECR_FLAG_SKIPPED) != 0;
	bool end = rec->kind == LECR_KIND_END;

	if (rec->kind > LECR_KIND_END)
		return "a kind above END";
	if (file->version < 2u && (rec->flags & LECR_FLAG_SKIP_SYNTHETIC))
		return "SKIP_SYNTHETIC in a version 1 file";
	if (rec->line != file->line)
		return "a line other than the line of the file";
	if (end ? !lecr_rate_valid(rec->samples * LECR_TICKS_PER_SECOND) :
		  rec->samples != file->samples)
		return "samples that the file does not allow";
	if (rec->kind == LECR_KIND_FEED && !one)
		return "a FEED without exactly one reason bit";
	if (rec->kind != LECR_KIND_FEED && reason)
		return "a reason bit on a RUN or an END";
	if (skipped != (rec->skipped != 0))
		return "SKIPPED that disagrees with the skipped count";
	if (rec->skipped && (end || (rec->flags & LECR_FLAG_NEW)))
		return "a skipped count on NEW or on an END";
	return NULL;
}

/* 1 for a record, 0 at the end of the stream, a negative errno for a short
 * record or a record the format does not allow. rec holds the header of a
 * refused record, so a caller can name the rule with lecr_refusal_get. */
static inline int lecr_read_record(FILE *f, const struct lecr_file *file,
				   struct lecr_record *rec)
{
	uint8_t b[LECR_RECORD_BYTES_MAX];
	unsigned n = file->samples;
	size_t size = LECR_HEAD_BYTES + 6u * n;
	size_t got;

	memset(rec, 0, sizeof(*rec));
	if (!n || n > LECR_SAMPLES_MAX)
		return -EINVAL;
	got = fread(b, 1, size, f);
	if (!got && !ferror(f))
		return 0;
	if (got != size)
		return -EIO;
	lecr_head_decode(b, rec);
	if (lecr_refusal_get(file, rec))
		return -EINVAL;
	lecr_plane_decode(b + LECR_HEAD_BYTES, rec->ref, n);
	lecr_plane_decode(b + LECR_HEAD_BYTES + 2u * n, rec->mic, n);
	lecr_plane_decode(b + LECR_HEAD_BYTES + 4u * n, rec->out, n);
	return 1;
}

/* The conversions of the stable record, struct udsp_lec_stats of udsp.h:
 * they saturate, and a value that is not a number reads 0. */
static inline int32_t lecr_mdb_get(float db)
{
	double x = (double)db * 1000.0;

	if (isnan(x))
		return 0;
	if (x <= (double)INT32_MIN)
		return INT32_MIN;
	if (x >= (double)INT32_MAX)
		return INT32_MAX;
	return (int32_t)x;
}

static inline uint32_t lecr_micro_get(float v)
{
	double x = (double)v * 1000000.0;

	if (!(x > 0.0))
		return 0;
	if (x >= (double)UINT32_MAX)
		return UINT32_MAX;
	return (uint32_t)x;
}

static inline uint16_t lecr_ddbov_get(float dbov)
{
	double x = (double)dbov * 10.0;

	if (isnan(x))
		return LECR_NOISE_UNMEASURED;
	if (x <= 0.0)
		return 0;
	if (x >= (double)UINT16_MAX)
		return UINT16_MAX;
	return (uint16_t)lrint(x);
}

static inline uint16_t lecr_u16_sat(uint32_t v)
{
	return v > UINT16_MAX ? UINT16_MAX : (uint16_t)v;
}

/*
 * The state fields and the verdicts of the instance after a record, as the
 * tick records them. The kind and the control flags stay as the caller set
 * them. A FEED or an END record carries no verdict, because the frame
 * reading still describes the last RUN.
 */
static inline void lecr_record_fill(struct lecr_record *rec,
				    const struct udsp_ec *ec)
{
	struct udsp_ec_nlms_frame f;
	struct udsp_ec_nlms_info info;
	uint16_t flags = rec->flags & (uint16_t)~LECR_FLAGS_VERDICT;

	udsp_ec_nlms_frame_get(ec, &f);
	udsp_ec_nlms_info_get(ec, &info);
	rec->frames = info.frames;
	rec->hold_frames = info.hold_frames;
	rec->erle_mdb = lecr_mdb_get(info.erle_db);
	rec->echo_gain_micro = lecr_micro_get(info.echo_gain);
	rec->coef_energy_micro = lecr_micro_get(info.coef_energy);
	rec->tail_ms = (uint16_t)info.tail_ms;
	rec->noise_ddbov = lecr_ddbov_get(info.noise_dbov);
	rec->probes = lecr_u16_sat(info.probes);
	rec->diverge_resets = lecr_u16_sat(info.diverge_resets);
	rec->nlp_verdict = LECR_VERDICT_OFF;
	rec->hold_left = 0;
	if (rec->kind == LECR_KIND_RUN) {
		rec->nlp_verdict = f.nlp_verdict;
		rec->hold_left = f.hold_left;
		flags |= (f.dt_fired ? LECR_FLAG_FIRED : 0u) |
			 (info.double_talk ? LECR_FLAG_DT : 0u) |
			 (info.nlp_active ? LECR_FLAG_ACTIVE : 0u) |
			 (f.probing ? LECR_FLAG_PROBING : 0u);
	}
	rec->flags = flags;
}

/* The six processor counts in the order of the lec nlp line of uvoice. */
enum lecr_count {
	LECR_COUNT_ACTIVE,
	LECR_COUNT_WAIT,
	LECR_COUNT_ERLE,
	LECR_COUNT_UNLIKE,
	LECR_COUNT_DT,
	LECR_COUNT_QUIET,
	LECR_COUNTS,
};

struct lecr_tally {
	uint32_t n[LECR_COUNTS];
};

static inline const char *lecr_count_name(unsigned count)
{
	static const char *const names[LECR_COUNTS] = {
		"active", "wait", "erle", "unlike", "dt", "quiet",
	};

	return count < LECR_COUNTS ? names[count] : "?";
}

/*
 * The words of the trial fields of the frame reading (dsp/lec.h). NONE is the
 * empty word. A column therefore stays empty on a frame without a trial start
 * or end. It also stays empty under a canceller source that does not write
 * the fields.
 */
static inline const char *lecr_trial_arm_name(unsigned arm)
{
	static const char *const names[] = {
		[UDSP_EC_NLMS_TRIAL_ARM_NONE] = "",
		[UDSP_EC_NLMS_TRIAL_ARM_LATCH] = "latch",
		[UDSP_EC_NLMS_TRIAL_ARM_PATH] = "path",
		[UDSP_EC_NLMS_TRIAL_ARM_STALE] = "stale",
	};

	return arm < sizeof(names) / sizeof(names[0]) ? names[arm] : "?";
}

static inline const char *lecr_trial_end_name(unsigned end)
{
	static const char *const names[] = {
		[UDSP_EC_NLMS_TRIAL_END_NONE] = "",
		[UDSP_EC_NLMS_TRIAL_END_KEPT] = "kept",
		[UDSP_EC_NLMS_TRIAL_END_REJECTED] = "rejected",
		[UDSP_EC_NLMS_TRIAL_END_DIVERGED] = "diverged",
		[UDSP_EC_NLMS_TRIAL_END_TALKER] = "talker",
	};

	return end < sizeof(names) / sizeof(names[0]) ? names[end] : "?";
}

/* A pass counts as active once the processor acts, and as a wait before.
 * An off verdict counts nowhere, as in nlp_count of dsp/lec.c. */
static inline void lecr_tally_add(struct lecr_tally *t, unsigned verdict,
				  bool active)
{
	switch (verdict) {
	case LECR_VERDICT_QUIET:
		t->n[LECR_COUNT_QUIET]++;
		return;
	case LECR_VERDICT_DT:
		t->n[LECR_COUNT_DT]++;
		return;
	case LECR_VERDICT_UNLIKE:
		t->n[LECR_COUNT_UNLIKE]++;
		return;
	case LECR_VERDICT_ERLE:
		t->n[LECR_COUNT_ERLE]++;
		return;
	case LECR_VERDICT_PASS:
		t->n[active ? LECR_COUNT_ACTIVE : LECR_COUNT_WAIT]++;
		return;
	}
}

static inline void lecr_tally_set(struct lecr_tally *t,
				  const struct udsp_ec_nlms_info *info)
{
	t->n[LECR_COUNT_ACTIVE] = info->nlp_active_frames;
	t->n[LECR_COUNT_WAIT] = info->nlp_wait_frames;
	t->n[LECR_COUNT_ERLE] = info->nlp_erle_frames;
	t->n[LECR_COUNT_UNLIKE] = info->nlp_unlike_frames;
	t->n[LECR_COUNT_DT] = info->nlp_dt_frames;
	t->n[LECR_COUNT_QUIET] = info->nlp_quiet_frames;
}

/* The fields of a RUN that the replay compares. */
enum lecr_diff {
	LECR_DIFF_FRAMES = 1u << 0,
	LECR_DIFF_FIRED = 1u << 1,
	LECR_DIFF_DT = 1u << 2,
	LECR_DIFF_ACTIVE = 1u << 3,
	LECR_DIFF_PROBING = 1u << 4,
	LECR_DIFF_VERDICT = 1u << 5,
	LECR_DIFF_OUT = 1u << 6,
};

/*
 * The shadow instance runs with its processor off from creation. The
 * processor runs last in the frame and feeds nothing back, so the shadow
 * output is the residual before the processor of the main instance.
 * ref_loss_half_db is the reference loss of the file (lecr_stream_replay),
 * which each new instance takes.
 */
struct lecr_replay {
	struct udsp_ec *ec;
	struct udsp_ec *shadow;
	unsigned ref_loss_half_db;
	bool shadow_want;
	bool broken;
	bool have_tick;
	/* A lineage is open: a record other than END came after the last
	 * END. */
	bool open;
	uint64_t tick_last;
	uint32_t frames_last;
	int16_t out[LECR_SAMPLES_MAX];
	int16_t residual[LECR_SAMPLES_MAX];
	struct lecr_record got;
};

/* skip_periods is the skipped field of a record with SKIPPED whose jump the
 * step accepted in an intact lineage. */
struct lecr_step {
	bool loss;
	bool gap;
	bool fresh;
	bool replayed;
	unsigned diff;
	unsigned out_delta;
	uint64_t skip_periods;
};

static inline void lecr_replay_init(struct lecr_replay *r, bool shadow)
{
	memset(r, 0, sizeof(*r));
	r->shadow_want = shadow;
}

static inline void lecr_replay_close(struct lecr_replay *r)
{
	if (r->ec)
		udsp_ec_nlms.destroy(r->ec);
	if (r->shadow)
		udsp_ec_nlms.destroy(r->shadow);
	r->ec = NULL;
	r->shadow = NULL;
}

static inline int lecr_replay_create(struct lecr_replay *r,
				     const struct lecr_record *rec)
{
	unsigned rate = rec->samples * LECR_TICKS_PER_SECOND;

	lecr_replay_close(r);
	r->broken = false;
	r->frames_last = 0;
	r->ec = udsp_ec_nlms.create_rate(rec->tail_ms, rate);
	if (!r->ec)
		return -EINVAL;
	udsp_ec_nlms_ref_loss_set(r->ec, r->ref_loss_half_db);
	if (!r->shadow_want)
		return 0;
	r->shadow = udsp_ec_nlms.create_rate(rec->tail_ms, rate);
	if (!r->shadow)
		return -EINVAL;
	udsp_ec_nlms_ref_loss_set(r->shadow, r->ref_loss_half_db);
	udsp_ec_nlms.nlp(r->shadow, false);
	return 0;
}

static inline unsigned lecr_diff_get(const struct lecr_record *live,
				     const struct lecr_record *got,
				     const int16_t *out, unsigned samples,
				     unsigned *delta)
{
	uint16_t flags = live->flags ^ got->flags;
	unsigned diff = 0;
	unsigned i;

	*delta = 0;
	if (live->frames != got->frames)
		diff |= LECR_DIFF_FRAMES;
	if (flags & LECR_FLAG_FIRED)
		diff |= LECR_DIFF_FIRED;
	if (flags & LECR_FLAG_DT)
		diff |= LECR_DIFF_DT;
	if (flags & LECR_FLAG_ACTIVE)
		diff |= LECR_DIFF_ACTIVE;
	if (flags & LECR_FLAG_PROBING)
		diff |= LECR_DIFF_PROBING;
	if (live->nlp_verdict != got->nlp_verdict)
		diff |= LECR_DIFF_VERDICT;
	for (i = 0; i < samples; i++) {
		unsigned d = (unsigned)abs((int)live->out[i] - (int)out[i]);

		if (d > *delta)
			*delta = d;
	}
	if (*delta)
		diff |= LECR_DIFF_OUT;
	return diff;
}

static inline void lecr_replay_run(struct lecr_replay *r,
				   const struct lecr_record *rec,
				   struct lecr_step *st)
{
	bool freeze = (rec->flags & LECR_FLAG_FREEZE) != 0;
	size_t bytes = rec->samples * sizeof(int16_t);

	if (rec->flags & LECR_FLAG_RESET) {
		udsp_ec_nlms_reset(r->ec);
		if (r->shadow)
			udsp_ec_nlms_reset(r->shadow);
	}
	udsp_ec_nlms.nlp(r->ec, (rec->flags & LECR_FLAG_NLP) != 0);
	udsp_ec_nlms.freeze(r->ec, freeze);
	memcpy(r->out, rec->mic, bytes);
	udsp_ec_nlms.run(r->ec, r->out, rec->ref);
	if (r->shadow) {
		udsp_ec_nlms.freeze(r->shadow, freeze);
		memcpy(r->residual, rec->mic, bytes);
		udsp_ec_nlms.run(r->shadow, r->residual, rec->ref);
	}
	memset(&r->got, 0, sizeof(r->got));
	r->got.kind = LECR_KIND_RUN;
	lecr_record_fill(&r->got, r->ec);
	st->diff = lecr_diff_get(rec, &r->got, r->out, rec->samples,
				 &st->out_delta);
	st->replayed = true;
}

static inline void lecr_gap_mark(struct lecr_replay *r, struct lecr_step *st)
{
	st->gap = true;
	r->broken = true;
}

/* Step rules 1 and 2: the loss and the lineage boundaries. */
static inline void lecr_bound_check(struct lecr_replay *r,
				    const struct lecr_record *rec, bool fresh,
				    struct lecr_step *st)
{
	if (rec->lost_before) {
		st->loss = true;
		r->broken = true;
		return;
	}
	if (!r->have_tick)
		return;
	if (fresh ? r->open : !r->open)
		lecr_gap_mark(r, st);
}

/* The jump rule of step rule 3. */
static inline void lecr_jump_check(struct lecr_replay *r,
				   const struct lecr_record *rec,
				   struct lecr_step *st)
{
	bool skipped = (rec->flags & LECR_FLAG_SKIPPED) != 0;
	uint64_t want = (uint64_t)rec->lost_before + 1u;

	if (skipped)
		want += rec->skipped;
	if (rec->tick > r->tick_last && rec->tick - r->tick_last == want) {
		if (skipped)
			st->skip_periods = rec->skipped;
		return;
	}
	lecr_gap_mark(r, st);
}

/* The frames rule of step rule 4. */
static inline void lecr_frames_check(struct lecr_replay *r,
				     const struct lecr_record *rec,
				     struct lecr_step *st)
{
	if (rec->kind == LECR_KIND_RUN || rec->frames == r->frames_last)
		return;
	lecr_gap_mark(r, st);
}

/* Step rules 3 and 4 for a record without a loss and without NEW in an
 * intact lineage. */
static inline void lecr_continuity_check(struct lecr_replay *r,
					 const struct lecr_record *rec,
					 struct lecr_step *st)
{
	if (rec->kind != LECR_KIND_END)
		lecr_jump_check(r, rec, st);
	if (!st->gap)
		lecr_frames_check(r, rec, st);
}

/* One record through the replay, in the order of the header comment. It
 * returns 0, or a negative errno for a record the replay cannot take. */
static inline int lecr_replay_step(struct lecr_replay *r,
				   const struct lecr_record *rec,
				   struct lecr_step *st)
{
	bool end = rec->kind == LECR_KIND_END;
	bool fresh = !end && (rec->flags & LECR_FLAG_NEW);

	memset(st, 0, sizeof(*st));
	if (!end && (!rec->samples || rec->samples > LECR_SAMPLES_MAX))
		return -EINVAL;
	lecr_bound_check(r, rec, fresh, st);
	if (!st->loss && !st->gap && !fresh && r->ec && !r->broken)
		lecr_continuity_check(r, rec, st);
	r->have_tick = true;
	r->tick_last = rec->tick;
	r->open = !end;
	if (fresh) {
		if (lecr_replay_create(r, rec))
			return -EINVAL;
		st->fresh = true;
		lecr_frames_check(r, rec, st);
	}
	if (end) {
		lecr_replay_close(r);
		return 0;
	}
	if (!r->ec || r->broken)
		return 0;
	r->frames_last = rec->frames;
	if (rec->kind == LECR_KIND_FEED) {
		udsp_ec_nlms_ref_feed(r->ec, rec->ref);
		if (r->shadow)
			udsp_ec_nlms_ref_feed(r->shadow, rec->ref);
		st->replayed = true;
		return 0;
	}
	lecr_replay_run(r, rec, st);
	return 0;
}

/*
 * A segment runs from NEW or RESET to the next one, to END or to a loss. The
 * control figures (frames, out and the first difference) cover every RUN of
 * the segment. The steady-state figures (the counts, the symmetric
 * differences and the verdict agreement) cover the body: the RUN records at
 * least skip_ticks after the first record of the segment.
 */
struct lecr_segment {
	unsigned index;
	bool from_reset;
	bool lossy;
	uint64_t tick_first;
	uint64_t tick_last;
	uint32_t frames_last;
	uint32_t runs;
	uint32_t frames_same;
	uint32_t out_same;
	unsigned out_delta;
	bool differs;
	uint64_t diff_tick;
	uint32_t diff_frames;
	unsigned diff;
	uint32_t body;
	struct lecr_tally live;
	struct lecr_tally replay;
	uint32_t fired_live;
	uint32_t fired_diff;
	uint32_t dt_diff;
	uint32_t verdict_same;
};

enum lecr_fault {
	LECR_FAULT_COUNTS = 1u << 0,
	LECR_FAULT_FIRED = 1u << 1,
	LECR_FAULT_VERDICT = 1u << 2,
};

/* max(5, ceil(0.05 x live)) */
static inline uint32_t lecr_bound_get(uint32_t live)
{
	uint64_t bound = ((uint64_t)live + 19u) / 20u;

	return bound < 5u ? 5u : (uint32_t)bound;
}

static inline uint32_t lecr_delta_get(uint32_t a, uint32_t b)
{
	return a > b ? a - b : b - a;
}

/* The criteria a segment without loss must meet. 0 means it meets them. */
static inline unsigned lecr_segment_faults(const struct lecr_segment *s)
{
	unsigned faults = 0;
	unsigned i;

	for (i = 0; i < LECR_COUNTS; i++)
		if (lecr_delta_get(s->live.n[i], s->replay.n[i]) >
		    lecr_bound_get(s->live.n[i]))
			faults |= LECR_FAULT_COUNTS;
	if (s->fired_diff > lecr_bound_get(s->fired_live))
		faults |= LECR_FAULT_FIRED;
	if ((uint64_t)s->verdict_same * 100u < (uint64_t)s->body * 95u)
		faults |= LECR_FAULT_VERDICT;
	return faults;
}

enum lecr_result {
	LECR_PASS,
	LECR_FAIL,
	LECR_INCOMPLETE,
};

static inline const char *lecr_result_name(enum lecr_result result)
{
	switch (result) {
	case LECR_PASS:
		return "PASS";
	case LECR_FAIL:
		return "FAIL";
	case LECR_INCOMPLETE:
		return "INCOMPLETE";
	}
	return "?";
}

/*
 * The agreement of a whole file. The totals of the six counts, the
 * symmetric differences and the verdict agreement sum the bodies of the
 * evaluated segments: those without loss and with a body. skipped counts the
 * records the replay did not take. skip_marks counts the records with
 * SKIPPED that the step accepted, and skip_periods sums their skipped
 * fields. refusal names the rule of a record that the reader refused.
 */
struct lecr_agree {
	uint64_t skip_ticks;
	bool open;
	struct lecr_segment seg;
	struct lecr_segment done;
	unsigned segments;
	uint32_t records;
	uint32_t instances;
	uint32_t resets;
	uint32_t losses;
	uint64_t lost;
	uint32_t gaps;
	uint32_t skip_marks;
	uint64_t skip_periods;
	uint32_t skipped;
	uint32_t runs;
	uint32_t frames_bad;
	uint32_t evaluated;
	uint32_t failed;
	bool first_seen;
	bool first_new;
	bool parse_error;
	const char *refusal;
	struct lecr_tally live;
	struct lecr_tally replay;
	uint32_t body;
	uint32_t fired_live;
	uint32_t fired_diff;
	uint32_t dt_diff;
	uint32_t verdict_same;
};

static inline void lecr_agree_init(struct lecr_agree *a, unsigned skip_s)
{
	memset(a, 0, sizeof(*a));
	a->skip_ticks = (uint64_t)skip_s * LECR_TICKS_PER_SECOND;
}

static inline bool lecr_segment_evaluated(const struct lecr_segment *s)
{
	return !s->lossy && s->body;
}

static inline void lecr_agree_close(struct lecr_agree *a)
{
	const struct lecr_segment *s = &a->seg;
	unsigned i;

	a->done = *s;
	a->open = false;
	a->runs += s->runs;
	a->frames_bad += s->runs - s->frames_same;
	if (!lecr_segment_evaluated(s))
		return;
	a->evaluated++;
	if (lecr_segment_faults(s))
		a->failed++;
	for (i = 0; i < LECR_COUNTS; i++) {
		a->live.n[i] += s->live.n[i];
		a->replay.n[i] += s->replay.n[i];
	}
	a->body += s->body;
	a->fired_live += s->fired_live;
	a->fired_diff += s->fired_diff;
	a->dt_diff += s->dt_diff;
	a->verdict_same += s->verdict_same;
}

static inline void lecr_agree_open(struct lecr_agree *a,
				   const struct lecr_record *rec,
				   bool from_reset)
{
	memset(&a->seg, 0, sizeof(a->seg));
	a->seg.index = ++a->segments;
	a->seg.from_reset = from_reset;
	a->seg.tick_first = rec->tick;
	a->seg.tick_last = rec->tick;
	a->open = true;
}

static inline void lecr_segment_add(struct lecr_segment *s, uint64_t skip,
				    const struct lecr_record *rec,
				    const struct lecr_step *st,
				    const struct lecr_record *got)
{
	uint16_t fired = rec->flags & LECR_FLAG_FIRED;

	s->runs++;
	s->tick_last = rec->tick;
	s->frames_last = rec->frames;
	if (!(st->diff & LECR_DIFF_FRAMES))
		s->frames_same++;
	if (!(st->diff & LECR_DIFF_OUT))
		s->out_same++;
	if (st->out_delta > s->out_delta)
		s->out_delta = st->out_delta;
	if (st->diff && !s->differs) {
		s->differs = true;
		s->diff_tick = rec->tick;
		s->diff_frames = rec->frames;
		s->diff = st->diff;
	}
	if (rec->tick - s->tick_first < skip)
		return;
	s->body++;
	lecr_tally_add(&s->live, rec->nlp_verdict,
		       (rec->flags & LECR_FLAG_ACTIVE) != 0);
	lecr_tally_add(&s->replay, got->nlp_verdict,
		       (got->flags & LECR_FLAG_ACTIVE) != 0);
	s->fired_live += fired ? 1u : 0u;
	s->fired_diff += (st->diff & LECR_DIFF_FIRED) ? 1u : 0u;
	s->dt_diff += (st->diff & LECR_DIFF_DT) ? 1u : 0u;
	s->verdict_same += (st->diff & LECR_DIFF_VERDICT) ? 0u : 1u;
}

/*
 * Adds one stepped record. It returns true when the record closed a
 * segment, which a->done then holds. A loss on a NEW record belongs to the
 * lineage before it, so the loss closes the open segment before NEW opens
 * the next.
 */
static inline bool lecr_agree_add(struct lecr_agree *a,
				  const struct lecr_record *rec,
				  const struct lecr_step *st,
				  const struct lecr_record *got)
{
	bool closed = false;

	a->records++;
	if (!a->first_seen) {
		a->first_seen = true;
		a->first_new = st->fresh;
	}
	if (st->loss) {
		a->losses++;
		a->lost += rec->lost_before;
	}
	if (st->gap)
		a->gaps++;
	if (st->skip_periods) {
		a->skip_marks++;
		a->skip_periods += st->skip_periods;
	}
	if ((st->loss || st->gap) && a->open) {
		a->seg.lossy = true;
		lecr_agree_close(a);
		closed = true;
	}
	if (st->fresh) {
		a->instances++;
		if (a->open) {
			lecr_agree_close(a);
			closed = true;
		}
		lecr_agree_open(a, rec, false);
	} else if (st->replayed && rec->kind == LECR_KIND_RUN &&
		   (rec->flags & LECR_FLAG_RESET)) {
		a->resets++;
		if (a->open) {
			lecr_agree_close(a);
			closed = true;
		}
		lecr_agree_open(a, rec, true);
	}
	if (rec->kind == LECR_KIND_END) {
		if (a->open) {
			lecr_agree_close(a);
			closed = true;
		}
		return closed;
	}
	if (!st->replayed)
		a->skipped++;
	else if (rec->kind == LECR_KIND_RUN && a->open)
		lecr_segment_add(&a->seg, a->skip_ticks, rec, st, got);
	return closed;
}

/* Closes the segment still open at the end of the file. */
static inline bool lecr_agree_finish(struct lecr_agree *a)
{
	if (!a->open)
		return false;
	lecr_agree_close(a);
	return true;
}

/*
 * The result of a file. PASS needs a parsed file whose first record is NEW,
 * a frames agreement on every replayed RUN and no gap (step rules 2 to 4).
 * It also needs at least one evaluated segment, and every evaluated segment
 * within the criteria. A file that does not parse or starts in the middle of
 * a lineage reads INCOMPLETE. So does a file with no evaluated segment and
 * no failure.
 */
static inline enum lecr_result lecr_agree_result(const struct lecr_agree *a)
{
	if (a->parse_error || !a->first_new)
		return LECR_INCOMPLETE;
	if (a->frames_bad || a->gaps || a->failed)
		return LECR_FAIL;
	if (!a->evaluated)
		return LECR_INCOMPLETE;
	return LECR_PASS;
}

/* rec is NULL on the call that reports the segment open at the end. */
typedef void (*lecr_record_cb)(void *ctx, const struct lecr_record *rec,
			       const struct lecr_replay *r,
			       const struct lecr_step *st,
			       const struct lecr_agree *a, bool closed);

/*
 * Reads a whole .lecr stream through the replay and the agreement. A parse
 * error stops the stream and reads INCOMPLETE; the records before it keep
 * their figures. It returns 0, or a negative errno.
 */
static inline int lecr_stream_replay(FILE *in, struct lecr_file *file,
				     struct lecr_replay *r,
				     struct lecr_agree *a, lecr_record_cb cb,
				     void *ctx)
{
	struct lecr_record rec;
	struct lecr_step st;
	uint32_t count = 0;
	int err = lecr_read_open(in, file);
	int rc = 0;

	r->ref_loss_half_db = lecr_file_ref_loss_get(file);
	while (!err && (rc = lecr_read_record(in, file, &rec)) > 0) {
		bool first_new = (file->flags & LECR_FILE_FIRST_NEW) != 0;
		bool closed;

		if (!count++ && first_new != !!(rec.flags & LECR_FLAG_NEW))
			err = -EINVAL;
		else
			err = lecr_replay_step(r, &rec, &st);
		if (err)
			break;
		closed = lecr_agree_add(a, &rec, &st, &r->got);
		if (cb)
			cb(ctx, &rec, r, &st, a, closed);
	}
	if (!err && rc < 0)
		err = rc;
	if (rc == -EINVAL)
		a->refusal = lecr_refusal_get(file, &rec);
	if (!err && count != file->records)
		err = -EIO;
	if (err)
		a->parse_error = true;
	if (lecr_agree_finish(a) && cb)
		cb(ctx, NULL, r, NULL, a, true);
	lecr_replay_close(r);
	return err;
}

#endif
