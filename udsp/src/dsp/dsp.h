// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#ifndef UDSP_DSP_H
#define UDSP_DSP_H

#include <complex.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "udsp/codec.h"
#include "udsp/event.h"
#include "udsp/udsp.h"

#define UDSP_SAMPLE_RATE	8000U
#define UDSP_FRAME_SAMPLES	80U
/* The wideband line rate and the widest per-tick frame.
 * UDSP_SAMPLE_RATE/UDSP_FRAME_SAMPLES stay the narrowband DSP frame: the
 * detectors keep running at 8 kHz behind the line decimators. */
#define UDSP_SAMPLE_RATE_WB	16000U
#define UDSP_FRAME_MAX		160U
#define UDSP_TONE_FREQ_MAX	4U
#define UDSP_TONE_CADENCE_MAX	8U
#define UDSP_GOERTZEL_MAX	16U
#define UDSP_DTMF_WINDOW_SAMPLES	205U
#define UDSP_DTMF_HOP_SAMPLES	40U

struct udsp_goertzel_bank {
	unsigned count;
	double coeff[UDSP_GOERTZEL_MAX];
	double sine[UDSP_GOERTZEL_MAX];
	double q1[UDSP_GOERTZEL_MAX];
	double q2[UDSP_GOERTZEL_MAX];
};

struct udsp_goertzel_iq {
	double re;
	double im;
};

struct udsp_dtmf_offset_run {
	double complex quarter;
	double complex half;
	double half_magnitude;
	unsigned decisions;
	unsigned adjacent;
	unsigned centroid_hops_x2;
};

struct udsp_dtmf_detector {
	int16_t sample_window[UDSP_DTMF_WINDOW_SAMPLES];
	char candidate;
	char active;
	/* The row level in dBm0 of the digit held and of the candidate run. The
	 * run keeps the loudest steady window, and the latest window until one
	 * is steady (dtmf_det.c candidate_level_add). */
	int8_t active_level_dbm0;
	int8_t candidate_level_dbm0;
	bool candidate_level_steady;
	unsigned candidate_decisions;
	unsigned active_decisions;
	unsigned sample_count;
	unsigned missed_decisions;
	unsigned missed_silent;
	char ended_digit;
	bool ended_digit_clear;
	unsigned no_vsc_run;
	bool remote;
	struct udsp_goertzel_bank bank;		/* built once in _init */
	struct udsp_goertzel_bank sub_bank;
	double complex offset_kernel[16][4];
	struct udsp_dtmf_offset_run offset_run[2];
};

struct udsp_oscillator {
	double phase;
};

struct udsp_dtmf_generator {
	struct udsp_oscillator low;
	struct udsp_oscillator high;
	unsigned rate_hz;
	char digit;
	int level_dbm0;
};

struct udsp_tone_cadence {
	uint16_t make_ms;
	uint16_t break_ms;
};

struct udsp_tone_desc {
	enum udsp_tone tone;
	uint8_t frequency_count;
	uint16_t frequency_hz[UDSP_TONE_FREQ_MAX];
	int8_t level_dbm0[UDSP_TONE_FREQ_MAX];
	uint8_t cadence_count;
	struct udsp_tone_cadence cadence[UDSP_TONE_CADENCE_MAX];
	bool repeat;
};

struct udsp_tone_generator {
	const struct udsp_tone_desc *desc;
	struct udsp_oscillator oscillator[UDSP_TONE_FREQ_MAX];
	double amplitude[UDSP_TONE_FREQ_MAX];	/* peak per frequency, from init */
	unsigned rate_hz;
	unsigned cadence_index;
	uint32_t cadence_samples;
	bool make;
	bool complete;
};

struct udsp_vad {
	double noise_energy;
	unsigned hangover;
	bool initialized;
};

struct udsp_cn {
	uint32_t seed;
};

/* dsp/mix.c, the conference bus. One per line; the file header carries
 * why it accumulates in int32 and saturates exactly once. */
struct udsp_mix_bus {
	int32_t acc[UDSP_FRAME_MAX];
};

struct udsp_codec {
	void *state;
	unsigned bits;
	uint32_t cn_seed;
};

/* Every built-in codec allocates through this scaffold: one codec object
 * carrying a zeroed state block of state_size bytes, with cn_seed armed. A
 * codec fills its own state fields after the call. All of them except Opus also
 * free through udsp_codec_free; Opus keeps its own destroy to release the
 * libopus handles. It stays a static inline in this header because test_g722,
 * test_g729 and test_opus link the codec sources without a shared dsp object. */
static inline struct udsp_codec *udsp_codec_alloc(size_t state_size)
{
	struct udsp_codec *codec = calloc(1, sizeof(*codec));
	void *state = calloc(1, state_size);

	if (!codec || !state) {
		free(state);
		free(codec);
		return NULL;
	}
	codec->state = state;
	codec->cn_seed = 1;
	return codec;
}

static inline void udsp_codec_free(struct udsp_codec *codec)
{
	if (!codec)
		return;
	free(codec->state);
	free(codec);
}

/* The digit the detector holds between the decision that validated it and the
 * decision that ends it, or 0 for none. The uplink mute of core/chan.c keys on
 * it, because the detector reports a digit only once it has ended. */
static inline char udsp_dtmf_detector_active(const struct udsp_dtmf_detector *d)
{
	return d->active;
}

/* The row level in dBm0 of that digit, 0 for none, measured after the SLIC
 * transmit gain and gain_tx, which core/chan.c chan_ul applies first. */
static inline int udsp_dtmf_detector_active_level(
	const struct udsp_dtmf_detector *d)
{
	return d->active_level_dbm0;
}

/* Clamp a double to the int16 range and round to nearest, the shared output
 * stage of the tone, DTMF, comfort-noise and gain paths. */
static inline int16_t udsp_sat16_round(double sample)
{
	if (sample > 32767.0)
		return 32767;
	if (sample < -32768.0)
		return -32768;
	return (int16_t)lrint(sample);
}

/* The peak of a sine at level_dbm0, the one level plan of the tone, DTMF and
 * FSK generators: 0 dBm0 is a sine 3.14 dB below int16 full scale. */
static inline double udsp_dbm0_amplitude(int level_dbm0)
{
	return 32767.0 * pow(10.0, (-3.14 + level_dbm0) / 20.0);
}

/* The level of a DTMF digit is the level of its row tone, and the column tone
 * plays this many dB higher. The value follows a measured handset, which sends
 * its column 2.0 to 2.3 dB above its row. The pair peak
 * is 2.259 times the row peak, so the pair fits int16 while the row is at -4
 * dBm0 or lower (the exact bound is -3.94 dBm0). */
#define UDSP_DTMF_TWIST_DB	2

extern const struct udsp_codec_descriptor udsp_codec_builtin;
extern const struct udsp_codec_ops udsp_codec_pcmu_ops;
extern const struct udsp_codec_ops udsp_codec_pcma_ops;
extern const struct udsp_codec_ops udsp_codec_g726_16_ops;
extern const struct udsp_codec_ops udsp_codec_g726_24_ops;
extern const struct udsp_codec_ops udsp_codec_g726_32_ops;
extern const struct udsp_codec_ops udsp_codec_g726_40_ops;
extern const struct udsp_codec_ops udsp_codec_g722_ops;
extern const struct udsp_codec_ops udsp_codec_g729_ops;
extern const struct udsp_codec_ops udsp_codec_opus_ops;	/* HAVE_OPUS only */

int16_t udsp_alaw_decode(uint8_t code);
uint8_t udsp_alaw_encode(int16_t sample);
int16_t udsp_ulaw_decode(uint8_t code);
uint8_t udsp_ulaw_encode(int16_t sample);

void udsp_goertzel_init(struct udsp_goertzel_bank *bank,
			const double *frequencies, unsigned count);
void udsp_goertzel_reset(struct udsp_goertzel_bank *bank);
void udsp_goertzel_reset_index(struct udsp_goertzel_bank *bank, unsigned index);
void udsp_goertzel_process(struct udsp_goertzel_bank *bank,
			   const int16_t *samples, size_t count);
void udsp_goertzel_process_index(struct udsp_goertzel_bank *bank, unsigned index,
				 const int16_t *samples, size_t count);
double udsp_goertzel_power(const struct udsp_goertzel_bank *bank,
			   unsigned index);
void udsp_goertzel_result(const struct udsp_goertzel_bank *bank, unsigned index,
			  struct udsp_goertzel_iq *out);

void udsp_dtmf_detector_init(struct udsp_dtmf_detector *detector, bool remote);
bool udsp_dtmf_detector_process(struct udsp_dtmf_detector *detector,
				const int16_t *samples, struct udsp_event *event);
bool udsp_dtmf_detector_flush(struct udsp_dtmf_detector *detector,
			      struct udsp_event *event);
void udsp_dtmf_generator_init(struct udsp_dtmf_generator *generator,
			      char digit, int level_dbm0, unsigned rate_hz);
bool udsp_dtmf_generator_render(struct udsp_dtmf_generator *generator,
				int16_t *samples, size_t count);

bool udsp_tone_generator_init(struct udsp_tone_generator *generator,
			      const struct udsp_tone_desc *desc,
			      unsigned rate_hz);
bool udsp_tone_generator_render(struct udsp_tone_generator *generator,
				int16_t *samples, size_t count);

void udsp_vad_init(struct udsp_vad *vad);
bool udsp_vad_process(struct udsp_vad *vad, const int16_t *samples);

void udsp_cn_init(struct udsp_cn *cn, uint32_t seed);
int udsp_cn_level_get(const int16_t *samples, size_t count);
void udsp_cn_generate(struct udsp_cn *cn, int level_dbov,
		      int16_t *samples, size_t count);
void udsp_codec_cn(struct udsp_codec *codec, int level_dbov, int16_t *samples,
		   unsigned count);

void udsp_gain_apply(int16_t *samples, size_t count, int half_db_steps);

void udsp_mix_bus_reset(struct udsp_mix_bus *bus);
void udsp_mix_bus_add(struct udsp_mix_bus *bus, const int16_t *src, unsigned n);
/* out = base + (bus - self), saturated once. self is the destination's own
 * contribution to the bus, or NULL for a destination that put nothing on it.
 * n is the line's frame count, at most UDSP_FRAME_MAX. */
void udsp_mix_minus(const struct udsp_mix_bus *bus, const int16_t *self,
		    const int16_t *base, int16_t *out, unsigned n);

_Static_assert(UDSP_FRAME_SAMPLES % UDSP_DTMF_HOP_SAMPLES == 0,
	       "DTMF decisions must divide a frame");
_Static_assert(UDSP_DTMF_HOP_SAMPLES < UDSP_DTMF_WINDOW_SAMPLES,
	       "DTMF decisions must overlap");

#define UDSP_CODEC_ENUMS_PINNED 1
_Static_assert((int)UDSP_CODEC_NONE == (int)UDSP_SEL_NONE,
	       "codec NONE mapping changed");
_Static_assert((int)UDSP_CODEC_PCMU == (int)UDSP_SEL_PCMU,
	       "codec PCMU mapping changed");
_Static_assert((int)UDSP_CODEC_PCMA == (int)UDSP_SEL_PCMA,
	       "codec PCMA mapping changed");
_Static_assert((int)UDSP_CODEC_G729 == (int)UDSP_SEL_G729,
	       "codec G729 mapping changed");
_Static_assert((int)UDSP_CODEC_G726_16 == (int)UDSP_SEL_G726_16,
	       "codec G726-16 mapping changed");
_Static_assert((int)UDSP_CODEC_G726_24 == (int)UDSP_SEL_G726_24,
	       "codec G726-24 mapping changed");
_Static_assert((int)UDSP_CODEC_G726_32 == (int)UDSP_SEL_G726_32,
	       "codec G726-32 mapping changed");
_Static_assert((int)UDSP_CODEC_G726_40 == (int)UDSP_SEL_G726_40,
	       "codec G726-40 mapping changed");
_Static_assert((int)UDSP_CODEC_G722 == (int)UDSP_SEL_G722,
	       "codec G722 mapping changed");
_Static_assert((int)UDSP_CODEC_OPUS == (int)UDSP_SEL_OPUS,
	       "codec OPUS mapping changed");

#endif
