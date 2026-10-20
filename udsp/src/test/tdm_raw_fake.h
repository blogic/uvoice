// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * A model of the ALSA PCM core for the host tests of the raw tdm back end
 * (tdm/alsa_raw.c). Two suites share it: test/test_tdm_raw.c drives the back
 * end directly, and test/test_core.c runs the whole library on it. The model
 * is header only and every function is static inline, the same shape as
 * test/util.h, so a suite that uses one part of it still builds clean.
 *
 * fk_reset picks a driver profile and installs the model through
 * udsp_tdm_raw_sys_override. fk_release frees the rings and restores the libc
 * table. The model answers the seven system calls the back end makes, and it
 * follows
 * sound/core/pcm_native.c and sound/core/pcm_lib.c: the state machine, the
 * group actions behind LINK, the flag meaning of SYNC_PTR, the overrun check
 * inside HWSYNC, the poll arms that raise the ready bit together with POLLERR,
 * and the channel area formula of snd_pcm_lib_ioctl_channel_info.
 *
 * Four profiles. an7581 is noninterleaved with a 64 byte period floor, 15
 * descriptors, an even period at 16 kHz, one shared geometry for both
 * directions and a transmit acknowledge, and it sets BATCH. mt7986 and mt2701
 * are interleaved with a 256 and a 320 byte period floor, a capture ring of at
 * least three periods, and they clear BATCH. batch-mmap is interleaved and sets
 * BATCH, which no board here does; it stands so a case can prove the wake rule
 * follows the BATCH bit and not the access layout.
 *
 * Two model choices differ from a real card on purpose. The wrap point the
 * model reports is two ring lengths, so a short run crosses it many times and
 * the folded differences are exercised; the start threshold the back end
 * supplies still carries the value snd_pcm_hw_params would compute, and
 * fk_start_threshold_seen holds it per direction for a case to compare.
 *
 * The DMA clock is separate from the wait. One clock drives both directions,
 * and only a stream whose trigger enabled its DMA moves. A case advances the
 * clock one period at a time with fk_advance, and each period publishes its
 * pointer and runs the state check. The wait advances the same clock while the
 * polled stream has less than one period ready, so a prepared or stopped pair
 * runs the wait out and times out. fk_overrun moves the capture DMA with no
 * interrupt, so the published pointer stays behind and only a HWSYNC call finds
 * the frames.
 *
 * The pointer callback answers a position inside the ring. The MediaTek front
 * end holds a few frames in its own buffer, which fk_lag models, and reports
 * the sample it reached; the AN7581 reports whole periods, so the same internal
 * buffer cannot show in its pointer at all. The core extends the position
 * against the one it published last, so a DMA that ran a whole ring between two
 * reads is invisible. A driver that advances its cursor only in its interrupt
 * sets pointer_isr_published, and a HWSYNC call then finds nothing new.
 *
 * The seventh system call is the sleep. It moves every running DMA by the
 * frames the sleep covers and raises no interrupt, so the progress reaches the
 * pointer only at the next HWSYNC. fk_sleep_deaf holds the DMA still instead,
 * for a case that has to reach a bound. An interrupt fires when a DMA reaches
 * the next period boundary of its own progress, so a sleep shortens the step
 * that follows it and never adds to it.
 *
 * fk_phased switches to a second model of time, for the cases that need the
 * phase between the two directions. One model time in frames (fk_now) runs for
 * both directions, each running stream completes its periods on its own grid,
 * and an interrupt fires at each completion and publishes the streams that
 * completed; completions at one instant publish together, as one AN7581
 * interrupt reports both directions. A transmit ring with nothing armed
 * stands still. With fk_tx_restart_at_kick, the arm of the next push restarts
 * it at that instant, so its completions keep the offset that the kick had
 * from the capture grid (an7581_pcm_tx_fill arms from appl_ptr, and every arm
 * kicks the DMA). Without it, the restarted ring starts at the next capture
 * completion, the grid variant: its first period then completes one period
 * after that completion. That is not the lockstep rule, which completes a
 * restarted period on the next capture completion as if it had started a
 * period before the kick. Both are hypotheses about the hardware; the model
 * assumes an immediate start, exactly one period per descriptor, no FIFO
 * residue, an interrupt at the completion instant and no cost for a read.
 * fk_work passes time with interrupts, for the work of a tick hook between its
 * read and its write; fk_ack_frames passes time inside each playback push
 * before it applies, for the cost of the write and its acknowledge. A hold
 * ends at any phase: fk_hold_extra adds frames to fk_hold_periods, and
 * fk_hold_push holds the caller inside its next playback push. fk_hold_taken
 * and fk_hold_push_taken count the holds a wait or a push consumed.
 *
 * The phased clock also measures. fk_push_at stamps every period position the
 * playback application pointer passes, and fk_cap_at every capture
 * completion. fk_age_down is the time from the stamp of the last period that
 * began to play to the start of its play, recorded at a completion, at the
 * start of the ring and at a restart; fk_age_up is the time from the
 * completion of the last period read to its read. A position that an expiry
 * realignment passes gets a stamp too, and a capture skip moves the read count
 * without a read, so both ages are payload ages only where every position is
 * a written or a read period: the AN7581 transmit model in a settled state.
 * fk_tx_pending is the armed transmit count right after the last playback
 * push, which the reported pointer can hide. fk_tx_dry_n counts the transmit
 * rings that ran dry while their DMA ran. fk_play_empty_reads and
 * fk_play_empty_pushes count the playback reads and pushes that found the
 * hardware pointer exactly at the application pointer on a running ring.
 * fk_sync_fault fails later SYNC_PTR calls of a direction with an errno: a
 * read or a push, after a count of calls of that kind, times calls in a row
 * (one for 0), then then_times more with then_err. With fk_fault_at_hold the
 * count starts when a capture wait takes its hold. fk_hold_gated keeps
 * the capture hold back until a case stores true in fk_hold_go, from any
 * thread, so the hold starts on a running line. fk_play_rec_arm records
 * what the transmit DMA plays from that hold on (fk_play_rec_at,
 * fk_play_rec_v), and fk_hold_end_at stamps the end of the hold.
 * fk_driver_hidden removes the profile's platform driver from
 * /sys/bus/platform/drivers.
 *
 * The counters a case reads are fk_sync_n, fk_sync_flags, fk_poll_n,
 * fk_poll_steps_n, fk_sleep_n, fk_sleep_ns, fk_hw_params_n,
 * fk_avail_min_unset, fk_start_unprepared, fk_fd_live, fk_map_live and the
 * recorded wait bound fk_poll_timeout_sec and fk_poll_timeout_nsec. The
 * journal fk_journal records the actions that carry
 * an order: L link, U unlink, P prepare, S start, D drop, p and c an
 * application pointer push, a one armed AN7581 descriptor, e and E the
 * transmit and receive DMA enable. fk_journal_dir_get names the direction that
 * issued an action, so a case can tell a group action on the capture
 * descriptor from one on the playback descriptor.
 *
 * The knobs answer a card that breaks one rule the open depends on:
 * fk_open_errno, fk_pversion, fk_user_pversion_fail, fk_refine_format_fail,
 * fk_periods_max_open, fk_info_no_mmap, fk_boundary_odd, fk_buffer_extra,
 * fk_channel_info_fault, fk_reject_periods, fk_periods_floor_refuse,
 * fk_peer_busy, fk_poll_timeouts, fk_poll_eintr, fk_poll_group_xrun,
 * fk_sleep_deaf, fk_state_force and the phased knobs above. fk_reset clears
 * every one of them.
 */
#ifndef UDSP_TEST_TDM_RAW_FAKE_H
#define UDSP_TEST_TDM_RAW_FAKE_H

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "alsa_raw.h"

#define FK_PLAY			0u
#define FK_CAPTURE		1u
#define FK_FD_BASE		500
#define FK_DESC			15u	/* AN7581 descriptor ring */
#define FK_JOURNAL_MAX		512u
/* How many interrupt periods one wait absorbs before it gives up. A wake
 * threshold of one period needs two of them at the worst modeled lag, so four
 * separates a slow stream from a stopped one. */
#define FK_POLL_STEPS		4u
#define FK_CHANNELS_MAX		2u

/* What fk_channel_info_fault breaks in the channel area answer. */
enum fk_channel_info_fault {
	FK_CI_NONE = 0,
	FK_CI_OFFSET,		/* an offset the data mmap cannot serve */
	FK_CI_STEP,		/* a step that is not a whole sample */
	FK_CI_OUTSIDE,		/* a last sample past the ring */
	FK_CI_SHAPE,		/* channel areas on top of each other */
};

struct fk_profile {
	const char *name;
	unsigned access;
	unsigned period_bytes_min;
	unsigned period_bytes_max;
	unsigned period_frames_min;
	unsigned period_frames_max;
	unsigned buffer_bytes_max;
	unsigned buffer_bytes_step;
	unsigned periods_min;
	unsigned periods_max;
	unsigned capture_periods_min;
	bool even_period_wb;
	bool shared_geometry;
	bool tx_ack;
	/* SNDRV_PCM_INFO_BATCH in the granted setup. It is the kernel's own
	 * statement that the pointer moves in period steps. */
	bool batch;
	/* The AN7581 reports the pointer once per period; the MediaTek front
	 * end reports the sample the DMA reached. */
	bool pointer_period_granular;
	/* an7581_pcm_pointer returns reported_frames, which only
	 * an7581_pcm_irq advances, and the same interrupt copies the capture
	 * data into the ring. A HWSYNC call on this driver therefore finds
	 * nothing the last interrupt did not already publish. */
	bool pointer_isr_published;
	/* The platform driver the profile registers under
	 * /sys/bus/platform/drivers, or NULL. */
	const char *driver;
};

static const struct fk_profile fk_an7581 = {
	.name = "an7581",
	.access = (unsigned)SNDRV_PCM_ACCESS_MMAP_NONINTERLEAVED,
	.period_bytes_min = 64,
	.period_bytes_max = 4080,
	.period_frames_min = 16,
	.period_frames_max = 1020,
	.buffer_bytes_max = 61200,
	.periods_min = 2,
	.periods_max = 15,
	.even_period_wb = true,
	.shared_geometry = true,
	.tx_ack = true,
	.batch = true,
	.pointer_period_granular = true,
	.pointer_isr_published = true,
	.driver = "an7581-pcm-tdm",
};

/* An interleaved device that moves its pointer in period steps and says so, and
 * whose pointer callback reads the hardware rather than a cursor its interrupt
 * keeps. No supported board has this shape; it stands here so a case can
 * prove the wake rule follows the BATCH bit and not the access layout, and so
 * one case can reach a HWSYNC that finds a period the wait did not. */
static const struct fk_profile fk_batch_mmap = {
	.name = "batch-mmap",
	.access = (unsigned)SNDRV_PCM_ACCESS_MMAP_INTERLEAVED,
	.period_bytes_min = 256,
	.period_bytes_max = 4 * 48 * 1024,
	.period_frames_min = 1,
	.period_frames_max = 65536,
	.buffer_bytes_max = 8 * 48 * 1024,
	.buffer_bytes_step = 16,
	.periods_min = 2,
	.periods_max = 256,
	.batch = true,
	.pointer_period_granular = true,
};

static const struct fk_profile fk_mt7986 = {
	.name = "mt7986",
	.access = (unsigned)SNDRV_PCM_ACCESS_MMAP_INTERLEAVED,
	.period_bytes_min = 256,
	.period_bytes_max = 4 * 48 * 1024,
	.period_frames_min = 1,
	.period_frames_max = 65536,
	.buffer_bytes_max = 8 * 48 * 1024,
	.buffer_bytes_step = 16,
	.periods_min = 2,
	.periods_max = 256,
	.capture_periods_min = 3,
};

static const struct fk_profile fk_mt2701 = {
	.name = "mt2701",
	.access = (unsigned)SNDRV_PCM_ACCESS_MMAP_INTERLEAVED,
	.period_bytes_min = 320,
	.period_bytes_max = 320 * 256,
	.period_frames_min = 1,
	.period_frames_max = 65536,
	.buffer_bytes_max = 320 * 1024,
	.buffer_bytes_step = 16,
	.periods_min = 2,
	.periods_max = 1024,
	.capture_periods_min = 3,
};

struct fk_stream {
	bool open;
	bool capture;
	int state;
	bool hw_set;
	unsigned mmap_count;
	unsigned channels;
	unsigned rate;
	unsigned period_frames;
	unsigned periods;
	unsigned buffer_frames;
	unsigned access;
	unsigned long boundary;
	unsigned long hw_ptr;
	unsigned long appl_ptr;
	unsigned long avail_min;
	unsigned long stop_threshold;
	unsigned long start_threshold;
	unsigned long silence_size;
	unsigned long silence_threshold;
	unsigned long dma_frames;	/* frames the DMA moved, never folded */
	unsigned long hw_base;		/* the extension base of the pointer */
	unsigned long tx_fill;		/* AN7581 transmit cursor */
	unsigned armed;
	/* Phased clock: a restarted transmit ring that waits for the next
	 * capture completion. */
	bool grid_wait;
	bool running;
	bool dma_on;
	unsigned char *dma;
	size_t dma_bytes;
	char path[64];
};

static const struct fk_profile *fk_prof;
static struct fk_stream fk[2];
static bool fk_linked;
static unsigned fk_group[2];		/* trigger order behind LINK */
static char fk_journal[FK_JOURNAL_MAX];
/* The direction that issued each journalled action, so a case can tell a
 * group action on the capture descriptor from one on the playback
 * descriptor. */
static unsigned char fk_journal_dir[FK_JOURNAL_MAX];
static unsigned fk_journal_len;
static unsigned fk_sync_n[2];
static unsigned fk_sync_flags[2];
static unsigned fk_poll_n[2];
static unsigned fk_hw_params_n[2];
static unsigned fk_avail_min_unset;
static unsigned fk_start_unprepared;
static unsigned fk_lag[2];
static unsigned fk_poll_timeouts[2];
static unsigned fk_hold_periods[2];
static bool fk_jit_playback;
static bool fk_silence_fill;
static unsigned fk_reject_periods;
static int fk_reject_errno;
static bool fk_peer_busy;
static unsigned long fk_start_threshold_seen[2];
/* The timeout of the last ppoll, in the units the caller passed. */
static long fk_poll_timeout_sec;
static long fk_poll_timeout_nsec;
/* Interrupts a wait absorbed, sleeps the caller took and the length of the
 * last one. */
static unsigned fk_poll_steps_n;
static unsigned fk_sleep_n;
static uint64_t fk_sleep_ns;
static clockid_t fk_sleep_clock;
static int fk_sleep_flags;
/* A sleep that moves no DMA, for a case that has to reach the round bound. */
static bool fk_sleep_deaf;
/* The phased clock (header). Ages are in frames of model time. */
#define FK_AGE_SLOTS		64u
static bool fk_phased;
static bool fk_tx_restart_at_kick;
static unsigned long fk_now;
static unsigned fk_ack_frames;
static unsigned fk_hold_extra[2];
static unsigned fk_hold_push;
static unsigned fk_hold_push_extra;
static unsigned fk_hold_push_taken;
static unsigned long fk_push_at[FK_AGE_SLOTS];
static unsigned long fk_push_n;
static unsigned long fk_cap_at[FK_AGE_SLOTS];
static unsigned long fk_read_n;
static long fk_age_down;
static long fk_age_up;
/* Transmit rings that ran dry while their DMA ran (AN7581 transmit model). */
static unsigned fk_tx_dry_n;
static unsigned fk_hold_taken[2];
static unsigned fk_tx_pending;
static unsigned fk_play_empty_reads;
static unsigned fk_play_empty_pushes;
static bool fk_driver_hidden;
/* Later SYNC_PTR calls of a direction fail with err: a push (an application
 * pointer move) or a read, after skip calls of that kind. */
struct fk_sync_fault {
	int err;
	bool push;
	unsigned skip;
	unsigned times;		/* calls in a row it fails, one for 0 */
	int then_err;		/* 0 for none */
	unsigned then_times;
};
static struct fk_sync_fault fk_sync_fault[2];
static bool fk_fault_at_hold;
static bool fk_hold_gated;
static atomic_bool fk_hold_go;
/* The playback of channel 0 on the phased clock, as the line plays it: one
 * entry per frame the transmit DMA moved, with the model time it played at.
 * fk_play_rec_arm starts the record at the capture wait that takes the hold,
 * and a case that clears it ends the record. fk_hold_end_at is the model time
 * that wait returns at. The record keeps FK_PLAY_REC_MAX frames. */
#define FK_PLAY_REC_MAX		16384u
static atomic_bool fk_play_rec_arm;
static bool fk_play_rec_on;
static unsigned fk_play_rec_n;
static unsigned long fk_play_rec_at[FK_PLAY_REC_MAX];
static int16_t fk_play_rec_v[FK_PLAY_REC_MAX];
static unsigned long fk_hold_end_at;
#define FK_DRIVER_FD		(FK_FD_BASE + 8)
/* Descriptors and mappings the back end still holds. */
static int fk_fd_live;
static int fk_map_live;
/* What SW_PARAMS and the data mmap carried, per direction. */
static unsigned fk_sw_proto[2];
static unsigned fk_sw_period_step[2];
static int fk_map_prot[2];

/* What PVERSION answers. A case lowers it to reach the kernels that have no
 * USER_PVERSION, or raises the major to reach the refusal. */
static int fk_open_errno;		/* every node open fails with it */
static bool fk_refine_format_fail;	/* the access and format probe fails */
static unsigned fk_pversion;
/* Answers a card that breaks one rule the open depends on. */
static bool fk_user_pversion_fail;	/* ENOTTY above protocol 2.0.14 */
static bool fk_periods_max_open;	/* an unbounded refined period count */
static bool fk_info_no_mmap;		/* the granted setup has no mmap */
static bool fk_boundary_odd;		/* a wrap point off a whole ring */
static unsigned fk_buffer_extra;	/* frames past a whole period count */
static enum fk_channel_info_fault fk_channel_info_fault;
/* Refuses every period count at or above this one, so a case can exhaust the
 * retry the back end allows itself. */
static unsigned fk_periods_floor_refuse;
/* Waits that answer something other than a stream state. */
static unsigned fk_poll_eintr[2];	/* -1 with EINTR, once per count */
static unsigned fk_poll_group_xrun[2];	/* the peer ring overruns in the wait */

/* --- small helpers ------------------------------------------------------- */

static inline int fk_fail(int err)
{
	errno = err;

	return -1;
}

static inline void fk_note(char c, unsigned dir)
{
	if (fk_journal_len + 1 >= FK_JOURNAL_MAX)
		return;
	fk_journal_dir[fk_journal_len] = (unsigned char)dir;
	fk_journal[fk_journal_len++] = c;
}

/* The direction that issued the first action of this kind, or -1. */
static inline int fk_journal_dir_get(char c)
{
	for (unsigned i = 0; i < fk_journal_len; i++) {
		if (fk_journal[i] == c)
			return (int)fk_journal_dir[i];
	}

	return -1;
}

static inline struct fk_stream *fk_stream_get(int fd)
{
	unsigned idx = (unsigned)(fd - FK_FD_BASE);

	if (fd < FK_FD_BASE || idx > FK_CAPTURE)
		return NULL;

	return fk[idx].open ? &fk[idx] : NULL;
}

static inline unsigned fk_index_get(const struct fk_stream *s)
{
	return s == &fk[FK_CAPTURE] ? FK_CAPTURE : FK_PLAY;
}

static inline int16_t fk_sample(unsigned long frame, unsigned ch)
{
	unsigned period = fk[FK_CAPTURE].period_frames;

	return (int16_t)(1 + (frame / period % 100u) * 256u + ch * 128u +
			 (frame % period) % 128u);
}

/* The channel area of snd_pcm_lib_ioctl_channel_info, built here from the
 * layout alone so the back end's own formula is never the reference. */
static inline size_t fk_offset(const struct fk_stream *s, unsigned ch,
			       unsigned slot)
{
	if (s->access == (unsigned)SNDRV_PCM_ACCESS_MMAP_INTERLEAVED)
		return ((size_t)slot * s->channels + ch) * sizeof(int16_t);

	return ((size_t)ch * s->buffer_frames + slot) * sizeof(int16_t);
}

static inline void fk_sample_put(struct fk_stream *s, unsigned ch,
				 unsigned slot, int16_t v)
{
	unsigned char *p = s->dma + fk_offset(s, ch, slot);

	p[0] = (unsigned char)((uint16_t)v & 0xffu);
	p[1] = (unsigned char)(((uint16_t)v >> 8) & 0xffu);
}

static inline int16_t fk_sample_read(const struct fk_stream *s, unsigned ch,
			      unsigned slot)
{
	const unsigned char *p = s->dma + fk_offset(s, ch, slot);

	return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline unsigned long fk_fold(unsigned long v, unsigned long boundary)
{
	return boundary ? v % boundary : v;
}

static inline unsigned long fk_between(unsigned long from, unsigned long to,
				unsigned long boundary)
{
	return to >= from ? to - from : to + boundary - from;
}

static inline unsigned long fk_avail(const struct fk_stream *s)
{
	unsigned long avail;

	if (s->capture)
		return fk_between(s->appl_ptr, s->hw_ptr, s->boundary);

	avail = s->hw_ptr + s->buffer_frames;
	avail = avail >= s->appl_ptr ? avail - s->appl_ptr :
				       avail + s->boundary - s->appl_ptr;

	return avail >= s->boundary ? avail - s->boundary : avail;
}

/* --- the DMA clock ------------------------------------------------------- */

/* The pointer callback of the driver. It answers a position inside the ring,
 * never a running count. The MediaTek front end holds a few frames in its own
 * buffer, which fk_lag models, and reports the sample it reached; the AN7581
 * reports whole periods. */
static inline unsigned long fk_pointer(const struct fk_stream *s)
{
	unsigned long lag = fk_lag[fk_index_get(s)];
	unsigned long shown;

	/* A register that counts whole periods cannot show a part of one, so
	 * the internal buffer of the part is invisible in it. */
	if (fk_prof->pointer_period_granular) {
		shown = s->dma_frames - s->dma_frames % s->period_frames;
		return shown % s->buffer_frames;
	}
	shown = s->dma_frames > lag ? s->dma_frames - lag : 0;

	return shown % s->buffer_frames;
}

static inline void fk_playback_silence(struct fk_stream *s,
				       unsigned long from, unsigned long to)
{
	unsigned long frames = fk_between(from, to, s->boundary);

	if (!fk_silence_fill || s->capture || s->silence_size < s->boundary)
		return;
	for (unsigned long f = 0; f < frames; f++) {
		unsigned slot = (unsigned)((from + f) % s->buffer_frames);

		for (unsigned ch = 0; ch < s->channels; ch++)
			fk_sample_put(s, ch, slot, 0);
	}
}

/* snd_pcm_update_hw_ptr0 extends the position against the pointer it published
 * last: a position that fell back means the ring end was crossed once. A DMA
 * that ran a whole ring or more between two reads is therefore invisible, which
 * bounds what any one call can discover. */
static inline void fk_hw_publish(struct fk_stream *s)
{
	unsigned long pos = fk_pointer(s);
	unsigned long next = s->hw_base + pos;

	if (next < s->hw_ptr) {
		s->hw_base += s->buffer_frames;
		if (s->hw_base >= s->boundary)
			s->hw_base = 0;
		next = s->hw_base + pos;
	}
	fk_playback_silence(s, s->hw_ptr, next);
	s->hw_ptr = next;
}

static inline void fk_capture_fill(struct fk_stream *s, unsigned long from,
			    unsigned long to)
{
	for (unsigned long f = from; f < to; f++) {
		unsigned slot = (unsigned)(f % s->buffer_frames);

		for (unsigned ch = 0; ch < s->channels; ch++)
			fk_sample_put(s, ch, slot, fk_sample(f, ch));
	}
}

/* One descriptor completes at every period boundary the DMA crosses, whatever
 * the size of the step that crossed it. */
static inline void fk_tx_complete(struct fk_stream *s, unsigned long from,
				  unsigned long to)
{
	unsigned done = (unsigned)(to / s->period_frames -
				   from / s->period_frames);
	bool had = s->armed != 0;

	s->armed -= s->armed < done ? s->armed : done;
	if (had && !s->armed && fk_jit_playback && fk_prof->tx_ack)
		fk_tx_dry_n++;
}

/* The frames the transmit DMA plays from from on, at fk_now on. */
static inline void fk_play_record(const struct fk_stream *s,
				  unsigned long from, unsigned long frames)
{
	for (unsigned long i = 0; i < frames; i++) {
		if (fk_play_rec_n >= FK_PLAY_REC_MAX)
			return;
		fk_play_rec_at[fk_play_rec_n] = fk_now + i;
		fk_play_rec_v[fk_play_rec_n++] =
			fk_sample_read(s, 0,
				       (unsigned)((from + i) % s->buffer_frames));
	}
}

/* The DMA moves and raises no interrupt, so the published pointer stays where
 * the last interrupt left it. */
static inline void fk_dma_step_quiet(struct fk_stream *s, unsigned long frames)
{
	unsigned long from = s->dma_frames;

	if (fk_jit_playback && fk_prof->tx_ack && !s->capture) {
		unsigned long ready;

		if (!s->armed)
			return;
		ready = (unsigned long)s->armed * s->period_frames -
			from % s->period_frames;
		if (frames > ready)
			frames = ready;
	}
	if (fk_play_rec_on && !s->capture &&
	    atomic_load_explicit(&fk_play_rec_arm, memory_order_relaxed))
		fk_play_record(s, from, frames);
	s->dma_frames += frames;
	if (s->capture)
		fk_capture_fill(s, from, s->dma_frames);
	else
		fk_tx_complete(s, from, s->dma_frames);
}

static inline void fk_dma_step(struct fk_stream *s, unsigned long frames)
{
	fk_dma_step_quiet(s, frames);
	fk_hw_publish(s);
}

static inline void fk_group_state_set(struct fk_stream *s, int state)
{
	for (unsigned i = 0; i <= FK_CAPTURE; i++) {
		if (!fk[i].open)
			continue;
		if (&fk[i] != s && !fk_linked)
			continue;
		fk[i].state = state;
		fk[i].running = false;
		fk[i].dma_on = false;
	}
}

/* snd_pcm_update_state: an available count at the stop threshold is an xrun,
 * and the stop reaches every linked stream. */
static inline int fk_state_update(struct fk_stream *s)
{
	if (s->state != SNDRV_PCM_STATE_RUNNING)
		return 0;
	if (fk_avail(s) < s->stop_threshold)
		return 0;
	fk_group_state_set(s, SNDRV_PCM_STATE_XRUN);

	return -EPIPE;
}

/* --- the phased clock --------------------------------------------------- */

/* Whether a stream's DMA moves on the phased clock now. */
static inline bool fk_phase_moving(const struct fk_stream *s)
{
	if (!s->open || !s->dma_on)
		return false;
	if (s->capture || !(fk_jit_playback && fk_prof->tx_ack))
		return true;

	return s->armed && !s->grid_wait;
}

/* Frames to the next completion of a moving stream; never zero. */
static inline unsigned long fk_phase_due(const struct fk_stream *s)
{
	return s->period_frames - s->dma_frames % s->period_frames;
}

/* A stream reached a period boundary at fk_now: a capture period completed, or
 * the transmit DMA starts the next armed period. */
static inline void fk_phase_record(const struct fk_stream *s)
{
	unsigned long k = s->dma_frames / s->period_frames;

	if (s->capture) {
		if (k)
			fk_cap_at[(k - 1u) % FK_AGE_SLOTS] = fk_now;
		return;
	}
	if ((s->armed || !(fk_jit_playback && fk_prof->tx_ack)) &&
	    k < fk_push_n)
		fk_age_down = (long)(fk_now - fk_push_at[k % FK_AGE_SLOTS]);
}

/* The transmit DMA starts a period at fk_now outside a completion: at the
 * start of the ring, at a restart by a kick, or at the capture completion a
 * waiting ring keeps. */
static inline void fk_phase_play_start(const struct fk_stream *s)
{
	unsigned long k = s->dma_frames / s->period_frames;

	if (fk_phased && !s->capture && k < fk_push_n)
		fk_age_down = (long)(fk_now - fk_push_at[k % FK_AGE_SLOTS]);
}

/* Passes model time. Every moving stream advances by the same frames, and each
 * completion on the way is an interrupt that publishes the streams that
 * completed and runs the state check. A quiet pass publishes nothing, as the
 * sleep does. */
static inline void fk_time_pass(unsigned long frames, bool irq)
{
	while (frames) {
		unsigned long step = frames;
		bool hit[2] = { false, false };

		for (unsigned i = 0; i <= FK_CAPTURE; i++) {
			if (fk_phase_moving(&fk[i]) &&
			    fk_phase_due(&fk[i]) < step)
				step = fk_phase_due(&fk[i]);
		}
		for (unsigned i = 0; i <= FK_CAPTURE; i++) {
			if (!fk_phase_moving(&fk[i]))
				continue;
			fk_dma_step_quiet(&fk[i], step);
			hit[i] = !(fk[i].dma_frames % fk[i].period_frames);
		}
		fk_now += step;
		frames -= step;
		if (!hit[FK_PLAY] && !hit[FK_CAPTURE])
			continue;
		for (unsigned i = 0; i <= FK_CAPTURE; i++) {
			if (!hit[i])
				continue;
			fk_phase_record(&fk[i]);
			if (irq)
				fk_hw_publish(&fk[i]);
		}
		if (hit[FK_CAPTURE] && fk[FK_PLAY].grid_wait) {
			fk[FK_PLAY].grid_wait = false;
			fk_phase_play_start(&fk[FK_PLAY]);
		}
		if (!irq)
			continue;
		for (unsigned i = 0; i <= FK_CAPTURE; i++)
			(void)fk_state_update(&fk[i]);
	}
}

/* One interrupt on the phased clock: time runs to the next completion of any
 * moving stream. Nothing moves when no DMA runs, as on the lockstep clock. */
static inline void fk_phase_step(void)
{
	unsigned long step = 0;

	for (unsigned i = 0; i <= FK_CAPTURE; i++) {
		if (fk_phase_moving(&fk[i]) &&
		    (!step || fk_phase_due(&fk[i]) < step))
			step = fk_phase_due(&fk[i]);
	}
	if (step)
		fk_time_pass(step, true);
}

/* The work of a tick hook: time passes, and the interrupts on the way fire. */
static inline void fk_work(unsigned long frames)
{
	if (fk_phased)
		fk_time_pass(frames, true);
}

/* Periods the caller pushed or read. A playback push stamps each period it
 * adds; a capture push takes the age of the last period it consumed. */
static inline void fk_phase_appl(const struct fk_stream *s, unsigned long from,
				 unsigned long to)
{
	unsigned long n = fk_between(from, to, s->boundary) / s->period_frames;
	unsigned long done;

	if (!s->capture) {
		while (n--)
			fk_push_at[fk_push_n++ % FK_AGE_SLOTS] = fk_now;
		return;
	}
	fk_read_n += n;
	done = s->dma_frames / s->period_frames;
	if (n && fk_read_n <= done)
		fk_age_up = (long)(fk_now -
				   fk_cap_at[(fk_read_n - 1u) % FK_AGE_SLOTS]);
}

/* The cost of a playback push before it applies, and a holder inside it. */
static inline void fk_phase_push_cost(void)
{
	unsigned long hold;

	fk_time_pass(fk_ack_frames, true);
	if (!fk_hold_push)
		return;
	hold = (unsigned long)fk_hold_push * fk[FK_PLAY].period_frames +
	       fk_hold_push_extra;
	fk_hold_push = 0;
	fk_hold_push_extra = 0;
	fk_hold_push_taken++;
	fk_time_pass(hold, true);
}

/* One interrupt. It fires when the DMA reaches the next period boundary of its
 * own progress, so a sleep that already consumed part of a period shortens the
 * step and never adds to it. One clock drives both directions, and only a
 * stream whose DMA the trigger enabled moves, so a prepared or stopped pair
 * stands still. Each interrupt publishes its pointer and runs the state check,
 * the way the handler of every one of the drivers does. */
static inline void fk_clock_step(void)
{
	if (fk_phased) {
		fk_phase_step();
		return;
	}
	for (unsigned i = 0; i <= FK_CAPTURE; i++) {
		struct fk_stream *s = &fk[i];
		unsigned long next;

		if (!s->open || !s->dma_on)
			continue;
		next = s->dma_frames + s->period_frames;
		next -= next % s->period_frames;
		fk_dma_step(s, next - s->dma_frames);
	}
	for (unsigned i = 0; i <= FK_CAPTURE; i++)
		(void)fk_state_update(&fk[i]);
}

static inline void fk_advance(unsigned periods)
{
	if (fk_phased) {
		const struct fk_stream *s = fk[FK_CAPTURE].open ?
					    &fk[FK_CAPTURE] : &fk[FK_PLAY];

		fk_time_pass((unsigned long)periods * s->period_frames, true);
		return;
	}
	for (unsigned n = 0; n < periods; n++)
		fk_clock_step();
}

/* The late reader: the DMA overwrites the period the reader still owns and the
 * head moves past it without an interrupt, so only the next HWSYNC finds it. */
static inline void fk_overrun(unsigned periods)
{
	struct fk_stream *s = &fk[FK_CAPTURE];

	fk_dma_step_quiet(s, periods * s->period_frames);
}

/* --- the AN7581 transmit model ------------------------------------------- */

static inline void fk_tx_fill(struct fk_stream *s)
{
	if (!s->running)
		return;
	while (s->armed < FK_DESC &&
	       fk_between(s->tx_fill, s->appl_ptr, s->boundary) >=
	       s->period_frames) {
		s->tx_fill = fk_fold(s->tx_fill + s->period_frames,
				     s->boundary);
		s->armed++;
		fk_note('a', FK_PLAY);
	}
}

/* --- hw_params ----------------------------------------------------------- */

#define FKP_MASK(p, k)		((p)->masks[(k) - \
				  SNDRV_PCM_HW_PARAM_FIRST_MASK])
#define FKP_INT(p, k)		((p)->intervals[(k) - \
				  SNDRV_PCM_HW_PARAM_FIRST_INTERVAL])

static inline bool fkp_mask_has(const struct snd_pcm_hw_params *p, int k,
				unsigned bit)
{
	return (FKP_MASK(p, k).bits[bit / 32u] & (1u << (bit % 32u))) != 0;
}

static inline void fkp_mask_one(struct snd_pcm_hw_params *p, int k,
				unsigned bit)
{
	memset(FKP_MASK(p, k).bits, 0, sizeof(FKP_MASK(p, k).bits));
	FKP_MASK(p, k).bits[bit / 32u] = 1u << (bit % 32u);
}

static inline void fkp_range_get(const struct snd_pcm_hw_params *p, int k,
			  unsigned *lo, unsigned *hi)
{
	*lo = FKP_INT(p, k).min + FKP_INT(p, k).openmin;
	*hi = FKP_INT(p, k).max - FKP_INT(p, k).openmax;
}

static inline void fkp_range_set(struct snd_pcm_hw_params *p, int k,
				 unsigned lo, unsigned hi)
{
	FKP_INT(p, k).min = lo;
	FKP_INT(p, k).max = hi;
	FKP_INT(p, k).openmin = 0;
	FKP_INT(p, k).openmax = 0;
	FKP_INT(p, k).integer = 1;
	FKP_INT(p, k).empty = 0;
}

struct fk_hw {
	unsigned channels;
	unsigned rate;
	unsigned period;
	unsigned periods_lo;
	unsigned periods_hi;
	unsigned periods;
	unsigned buffer;
};

static inline unsigned fk_clamp(unsigned v, unsigned lo, unsigned hi)
{
	if (v < lo)
		return lo;

	return v > hi ? hi : v;
}

static inline int fk_rate_pick(unsigned lo, unsigned hi, unsigned *out)
{
	static const unsigned rates[] = { 8000, 16000 };

	for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
		if (rates[i] < lo || rates[i] > hi)
			continue;
		*out = rates[i];
		return 0;
	}

	return -EINVAL;
}

static inline int fk_period_refine(const struct fk_hw *hw, unsigned *lo,
				   unsigned *hi)
{
	unsigned frame_bytes = hw->channels * (unsigned)sizeof(int16_t);
	unsigned floor_frames = (fk_prof->period_bytes_min + frame_bytes - 1) /
				frame_bytes;
	unsigned ceil_frames = fk_prof->period_bytes_max / frame_bytes;

	*lo = fk_clamp(*lo, floor_frames, UINT_MAX);
	*lo = fk_clamp(*lo, fk_prof->period_frames_min, UINT_MAX);
	if (*hi > ceil_frames)
		*hi = ceil_frames;
	if (*hi > fk_prof->period_frames_max)
		*hi = fk_prof->period_frames_max;
	if (fk_prof->even_period_wb && hw->rate == 16000) {
		*lo += *lo % 2;
		*hi -= *hi % 2;
	}

	return *lo <= *hi ? 0 : -EINVAL;
}

static inline int fk_periods_refine(const struct fk_stream *s,
				    const struct fk_hw *hw, unsigned *lo,
				    unsigned *hi)
{
	unsigned frame_bytes = hw->channels * (unsigned)sizeof(int16_t);
	unsigned period_bytes = hw->period * frame_bytes;
	unsigned ring_max = fk_prof->buffer_bytes_max / period_bytes;
	unsigned floor_periods = fk_prof->periods_min;

	if (s->capture && fk_prof->capture_periods_min > floor_periods)
		floor_periods = fk_prof->capture_periods_min;
	*lo = fk_clamp(*lo, floor_periods, UINT_MAX);
	if (*hi > fk_prof->periods_max)
		*hi = fk_prof->periods_max;
	if (*hi > ring_max)
		*hi = ring_max;

	return *lo <= *hi ? 0 : -EINVAL;
}

static inline int fk_hw_resolve(struct fk_stream *s,
				struct snd_pcm_hw_params *p, bool choose,
				struct fk_hw *hw)
{
	unsigned lo, hi;
	int rc;

	if (!fkp_mask_has(p, SNDRV_PCM_HW_PARAM_ACCESS, fk_prof->access) ||
	    !fkp_mask_has(p, SNDRV_PCM_HW_PARAM_FORMAT,
			  (unsigned)SNDRV_PCM_FORMAT_S16_LE) ||
	    !fkp_mask_has(p, SNDRV_PCM_HW_PARAM_SUBFORMAT,
			  (unsigned)SNDRV_PCM_SUBFORMAT_STD))
		return -EINVAL;

	fkp_range_get(p, SNDRV_PCM_HW_PARAM_CHANNELS, &lo, &hi);
	lo = fk_clamp(lo, 1, UINT_MAX);
	if (hi > FK_CHANNELS_MAX)
		hi = FK_CHANNELS_MAX;
	if (lo > hi)
		return -EINVAL;
	hw->channels = lo;

	fkp_range_get(p, SNDRV_PCM_HW_PARAM_RATE, &lo, &hi);
	rc = fk_rate_pick(lo, hi, &hw->rate);
	if (rc < 0)
		return rc;

	fkp_range_get(p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, &lo, &hi);
	rc = fk_period_refine(hw, &lo, &hi);
	if (rc < 0)
		return rc;
	hw->period = lo;

	fkp_range_get(p, SNDRV_PCM_HW_PARAM_PERIODS, &lo, &hi);
	rc = fk_periods_refine(s, hw, &lo, &hi);
	if (rc < 0)
		return rc;
	hw->periods_lo = lo;
	hw->periods_hi = hi;
	/* snd_pcm_hw_params_choose takes the maximum for the buffer size, so an
	 * open period count grants the deepest ring. */
	hw->periods = choose ? hi : lo;
	hw->buffer = hw->periods * hw->period;

	return 0;
}

static inline void fk_hw_write(struct snd_pcm_hw_params *p,
			       const struct fk_hw *hw, bool choose)
{
	fkp_mask_one(p, SNDRV_PCM_HW_PARAM_ACCESS, fk_prof->access);
	fkp_mask_one(p, SNDRV_PCM_HW_PARAM_FORMAT,
		     (unsigned)SNDRV_PCM_FORMAT_S16_LE);
	fkp_mask_one(p, SNDRV_PCM_HW_PARAM_SUBFORMAT,
		     (unsigned)SNDRV_PCM_SUBFORMAT_STD);
	fkp_range_set(p, SNDRV_PCM_HW_PARAM_CHANNELS, hw->channels,
		      hw->channels);
	fkp_range_set(p, SNDRV_PCM_HW_PARAM_RATE, hw->rate, hw->rate);
	fkp_range_set(p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, hw->period,
		      hw->period);
	if (choose) {
		fkp_range_set(p, SNDRV_PCM_HW_PARAM_PERIODS, hw->periods,
			      hw->periods);
		fkp_range_set(p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, hw->buffer,
			      hw->buffer);
	} else {
		fkp_range_set(p, SNDRV_PCM_HW_PARAM_PERIODS, hw->periods_lo,
			      fk_periods_max_open ? UINT_MAX : hw->periods_hi);
		fkp_range_set(p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
			      hw->periods_lo * hw->period,
			      hw->periods_hi * hw->period);
	}
	p->rmask = 0;
	p->info = SNDRV_PCM_INFO_MMAP_VALID;
	if (!fk_info_no_mmap)
		p->info |= SNDRV_PCM_INFO_MMAP;
	if (fk_prof->batch)
		p->info |= SNDRV_PCM_INFO_BATCH;
	if (fk_prof->tx_ack)
		p->info |= SNDRV_PCM_INFO_SYNC_APPLPTR;
}

static inline int fk_peer_check(const struct fk_stream *s,
				const struct fk_hw *hw)
{
	const struct fk_stream *other = s->capture ? &fk[FK_PLAY] :
						     &fk[FK_CAPTURE];

	if (fk_peer_busy && s->capture)
		return -EBUSY;
	if (!fk_prof->shared_geometry || !other->hw_set)
		return 0;
	if (other->channels != hw->channels || other->rate != hw->rate ||
	    other->period_frames != hw->period)
		return -EBUSY;

	return 0;
}

static inline int fk_hw_refine(struct fk_stream *s, struct snd_pcm_hw_params *p)
{
	struct fk_hw hw;
	unsigned lo, hi;
	int rc;

	/* The back end probes access and format with every interval open
	 * before it pins the geometry; the knob refuses that probe alone. */
	fkp_range_get(p, SNDRV_PCM_HW_PARAM_RATE, &lo, &hi);
	if (fk_refine_format_fail && lo == 0 && hi == UINT_MAX)
		return fk_fail(EINVAL);
	rc = fk_hw_resolve(s, p, false, &hw);
	if (rc < 0)
		return fk_fail(-rc);
	fk_hw_write(p, &hw, false);

	return 0;
}

static inline int fk_hw_params(struct fk_stream *s, struct snd_pcm_hw_params *p)
{
	struct fk_hw hw;
	int rc;

	fk_hw_params_n[fk_index_get(s)]++;
	if (s->state != SNDRV_PCM_STATE_OPEN &&
	    s->state != SNDRV_PCM_STATE_SETUP &&
	    s->state != SNDRV_PCM_STATE_PREPARED)
		return fk_fail(EBADFD);
	if (s->mmap_count)
		return fk_fail(EBADFD);

	rc = fk_hw_resolve(s, p, true, &hw);
	if (!rc && fk_reject_periods && hw.periods == fk_reject_periods)
		rc = -fk_reject_errno;
	if (!rc && fk_periods_floor_refuse &&
	    hw.periods >= fk_periods_floor_refuse)
		rc = -fk_reject_errno;
	if (!rc)
		rc = fk_peer_check(s, &hw);
	if (!rc && fk_prof->buffer_bytes_step &&
	    (hw.buffer * hw.channels * sizeof(int16_t)) %
	    fk_prof->buffer_bytes_step)
		rc = -EINVAL;
	if (rc < 0) {
		s->state = SNDRV_PCM_STATE_OPEN;
		return fk_fail(-rc);
	}

	free(s->dma);
	/* fk_buffer_extra grants a ring that is not a whole number of periods,
	 * which the AN7581 transmit copy cannot serve. */
	hw.buffer += fk_buffer_extra;
	s->channels = hw.channels;
	s->rate = hw.rate;
	s->period_frames = hw.period;
	s->periods = hw.periods;
	s->buffer_frames = hw.buffer;
	s->access = fk_prof->access;
	s->dma_bytes = (size_t)hw.buffer * hw.channels * sizeof(int16_t);
	s->dma = calloc(1, s->dma_bytes);
	if (!s->dma)
		return fk_fail(ENOMEM);
	s->avail_min = hw.period;
	s->stop_threshold = hw.buffer;
	s->start_threshold = 1;
	s->boundary = 2u * hw.buffer;
	s->hw_ptr = 0;
	s->hw_base = 0;
	s->appl_ptr = 0;
	s->dma_frames = 0;
	s->hw_set = true;
	s->state = SNDRV_PCM_STATE_SETUP;
	fk_hw_write(p, &hw, true);

	return 0;
}

static inline int fk_sw_params(struct fk_stream *s,
			       struct snd_pcm_sw_params *sw)
{
	if (s->state == SNDRV_PCM_STATE_OPEN)
		return fk_fail(EBADFD);
	if (sw->tstamp_mode < 0 || sw->tstamp_mode > SNDRV_PCM_TSTAMP_LAST)
		return fk_fail(EINVAL);
	if (!sw->avail_min)
		return fk_fail(EINVAL);
	if (sw->silence_size >= s->boundary) {
		if (sw->silence_threshold)
			return fk_fail(EINVAL);
	} else if (sw->silence_size > sw->silence_threshold ||
		   sw->silence_threshold > s->buffer_frames) {
		return fk_fail(EINVAL);
	}
	s->avail_min = sw->avail_min;
	s->start_threshold = sw->start_threshold;
	s->stop_threshold = sw->stop_threshold;
	s->silence_size = sw->silence_size;
	s->silence_threshold = sw->silence_threshold;
	fk_start_threshold_seen[fk_index_get(s)] = sw->start_threshold;
	fk_sw_proto[fk_index_get(s)] = sw->proto;
	fk_sw_period_step[fk_index_get(s)] = sw->period_step;
	sw->boundary = fk_boundary_odd ? s->boundary + s->period_frames :
					 s->boundary;

	return 0;
}

static inline int fk_channel_info(struct fk_stream *s,
			   struct snd_pcm_channel_info *ci)
{
	if (!s->hw_set)
		return fk_fail(EBADFD);
	if (ci->channel >= s->channels)
		return fk_fail(EINVAL);
	ci->offset = 0;
	if (s->access == (unsigned)SNDRV_PCM_ACCESS_MMAP_INTERLEAVED) {
		ci->first = ci->channel * 16u;
		ci->step = s->channels * 16u;
	} else {
		ci->first = (unsigned)(ci->channel *
				       (s->dma_bytes / s->channels) * 8u);
		ci->step = 16u;
	}
	switch (fk_channel_info_fault) {
	case FK_CI_OFFSET:
		ci->offset = 4096;
		break;
	case FK_CI_STEP:
		ci->step += 4u;
		break;
	case FK_CI_OUTSIDE:
		ci->first += (unsigned)(s->dma_bytes * 8u);
		break;
	case FK_CI_SHAPE:
		/* Every channel starts at the same sample. The addresses stay
		 * inside the ring, so only a shape rule refuses this. */
		ci->first = 0;
		break;
	default:
		break;
	}

	return 0;
}

/* --- state actions ------------------------------------------------------- */

static inline bool fk_member_is(unsigned i, const struct fk_stream *s)
{
	return fk_linked ? fk[i].open : &fk[i] == s;
}

static inline int fk_prepare(struct fk_stream *s)
{
	for (unsigned n = 0; n <= FK_CAPTURE; n++) {
		struct fk_stream *m = &fk[fk_group[n]];

		if (!fk_member_is(fk_group[n], s))
			continue;
		if (m->state == SNDRV_PCM_STATE_OPEN)
			return fk_fail(EBADFD);
		if (m->state == SNDRV_PCM_STATE_RUNNING)
			return fk_fail(EBUSY);
	}
	fk_note('P', fk_index_get(s));
	for (unsigned n = 0; n <= FK_CAPTURE; n++) {
		struct fk_stream *m = &fk[fk_group[n]];

		if (!fk_member_is(fk_group[n], s))
			continue;
		m->hw_ptr = 0;
		m->hw_base = 0;
		m->dma_frames = 0;
		m->appl_ptr = m->hw_ptr;
		m->tx_fill = 0;
		m->armed = 0;
		m->grid_wait = false;
		if (m->capture)
			fk_read_n = 0;
		else
			fk_push_n = 0;
		m->running = false;
		m->dma_on = false;
		m->state = SNDRV_PCM_STATE_PREPARED;
	}

	return 0;
}

static inline int fk_start(struct fk_stream *s)
{
	for (unsigned n = 0; n <= FK_CAPTURE; n++) {
		struct fk_stream *m = &fk[fk_group[n]];

		if (!fk_member_is(fk_group[n], s))
			continue;
		if (m->state != SNDRV_PCM_STATE_PREPARED) {
			fk_start_unprepared++;
			return fk_fail(EBADFD);
		}
		if (!m->capture && fk_avail(m) >= m->buffer_frames)
			return fk_fail(EPIPE);
	}
	fk_note('S', fk_index_get(s));
	for (unsigned n = 0; n <= FK_CAPTURE; n++) {
		struct fk_stream *m = &fk[fk_group[n]];

		if (!fk_member_is(fk_group[n], s))
			continue;
		m->state = SNDRV_PCM_STATE_RUNNING;
		m->running = true;
		if (m->capture) {
			m->dma_on = true;
			fk_note('E', FK_CAPTURE);
			continue;
		}
		m->tx_fill = m->hw_ptr;
		m->armed = 0;
		m->grid_wait = false;
		if (fk_prof->tx_ack)
			fk_tx_fill(m);
		m->dma_on = true;
		fk_phase_play_start(m);
		fk_note('e', FK_PLAY);
	}

	return 0;
}

static inline int fk_drop(struct fk_stream *s)
{
	if (s->state == SNDRV_PCM_STATE_OPEN)
		return fk_fail(EBADFD);
	fk_note('D', fk_index_get(s));
	fk_group_state_set(s, SNDRV_PCM_STATE_SETUP);

	return 0;
}

static inline int fk_link(struct fk_stream *s, int peer_fd)
{
	struct fk_stream *peer = fk_stream_get(peer_fd);

	if (!peer || peer == s)
		return fk_fail(EBADFD);
	if (s->state == SNDRV_PCM_STATE_OPEN || s->state != peer->state)
		return fk_fail(EBADFD);
	if (fk_linked)
		return fk_fail(EALREADY);
	fk_linked = true;
	/* snd_pcm_link puts the descriptor argument at the head of the group,
	 * so its stream runs first in every group action. */
	fk_group[0] = fk_index_get(peer);
	fk_group[1] = fk_index_get(s);
	fk_note('L', fk_index_get(s));

	return 0;
}

static inline int fk_unlink(struct fk_stream *s)
{
	if (!fk_linked)
		return fk_fail(EALREADY);
	fk_linked = false;
	fk_note('U', fk_index_get(s));

	return 0;
}

/* --- SYNC_PTR ------------------------------------------------------------ */

static inline int fk_hwsync(struct fk_stream *s)
{
	if (s->state == SNDRV_PCM_STATE_PREPARED ||
	    s->state == SNDRV_PCM_STATE_PAUSED)
		return 0;
	if (s->state == SNDRV_PCM_STATE_XRUN)
		return -EPIPE;
	if (s->state == SNDRV_PCM_STATE_SUSPENDED)
		return -ESTRPIPE;
	if (s->state != SNDRV_PCM_STATE_RUNNING &&
	    !(s->state == SNDRV_PCM_STATE_DRAINING && !s->capture))
		return -EBADFD;
	/* snd_pcm_update_hw_ptr asks the driver where the DMA is now, so the
	 * answer moves on from the one the last interrupt published. A driver
	 * that only advances its cursor in the interrupt has nothing to add. */
	if (!fk_prof->pointer_isr_published)
		fk_hw_publish(s);

	return fk_state_update(s);
}

static inline int fk_appl_apply(struct fk_stream *s, unsigned long appl)
{
	unsigned long from = s->appl_ptr;
	bool dry;

	if (appl == s->appl_ptr)
		return 0;
	if (appl >= s->boundary)
		return -EINVAL;
	s->appl_ptr = appl;
	fk_note(s->capture ? 'c' : 'p', fk_index_get(s));
	if (fk_phased)
		fk_phase_appl(s, from, appl);
	if (fk_prof->tx_ack && !s->capture) {
		dry = !s->armed;
		fk_tx_fill(s);
		/* On the phased clock, a ring that ran dry restarts at this
		 * kick, or waits for the capture grid without the knob. */
		if (fk_phased && dry && s->armed && s->dma_on) {
			if (fk_tx_restart_at_kick)
				fk_phase_play_start(s);
			else
				s->grid_wait = true;
		}
	}
	if (!s->capture)
		fk_tx_pending = s->armed;

	return 0;
}

static inline int fk_sync_ptr(struct fk_stream *s, struct snd_pcm_sync_ptr *sp)
{
	unsigned flags = sp->flags;
	struct fk_sync_fault *fault;
	bool push;
	int rc;

	fk_sync_n[fk_index_get(s)]++;
	fk_sync_flags[fk_index_get(s)] = flags;
	push = !(flags & SNDRV_PCM_SYNC_PTR_APPL) &&
	       sp->c.control.appl_ptr != s->appl_ptr;
	fault = &fk_sync_fault[fk_index_get(s)];
	if (fault->err && fault->push == push &&
	    (!fk_fault_at_hold || fk_hold_taken[FK_CAPTURE])) {
		if (fault->skip) {
			fault->skip--;
		} else {
			rc = fault->err;
			if (fault->times > 1u) {
				fault->times--;
			} else {
				fault->err = fault->then_err;
				fault->times = fault->then_times;
				fault->then_err = 0;
			}
			return fk_fail(rc);
		}
	}
	if (flags & SNDRV_PCM_SYNC_PTR_HWSYNC) {
		rc = fk_hwsync(s);
		if (rc < 0)
			return fk_fail(-rc);
	}
	if (!push && !s->capture && s->running && s->dma_on &&
	    s->hw_ptr == s->appl_ptr)
		fk_play_empty_reads++;
	if (!(flags & SNDRV_PCM_SYNC_PTR_APPL)) {
		if (fk_phased && push && !s->capture)
			fk_phase_push_cost();
		/* The push applies now: an empty ring here is one the push
		 * restarts or refills from nothing. */
		if (push && !s->capture && s->running && s->dma_on &&
		    s->hw_ptr == s->appl_ptr)
			fk_play_empty_pushes++;
		rc = fk_appl_apply(s, sp->c.control.appl_ptr);
		if (rc < 0)
			return fk_fail(-rc);
	}
	if (!(flags & SNDRV_PCM_SYNC_PTR_AVAIL_MIN)) {
		fk_avail_min_unset++;
		s->avail_min = sp->c.control.avail_min;
	}
	memset(&sp->s, 0, sizeof(sp->s));
	sp->s.status.state = s->state;
	sp->s.status.hw_ptr = s->hw_ptr;
	sp->c.control.appl_ptr = s->appl_ptr;
	sp->c.control.avail_min = s->avail_min;

	return 0;
}

/* --- the system call table ----------------------------------------------- */

static inline int fk_open(const char *path, int flags)
{
	size_t len = path ? strlen(path) : 0;
	unsigned idx;

	if (len > 26 && !strncmp(path, "/sys/bus/platform/drivers/", 26)) {
		if (!fk_prof || !fk_prof->driver || fk_driver_hidden ||
		    strcmp(path + 26, fk_prof->driver))
			return fk_fail(ENOENT);
		fk_fd_live++;
		return FK_DRIVER_FD;
	}
	if (!len || strncmp(path, "/dev/snd/pcmC", 13))
		return fk_fail(ENOENT);
	if (fk_open_errno)
		return fk_fail(fk_open_errno);
	if (path[len - 1] == 'p')
		idx = FK_PLAY;
	else if (path[len - 1] == 'c')
		idx = FK_CAPTURE;
	else
		return fk_fail(ENOENT);
	if (fk[idx].open)
		return fk_fail(EBUSY);

	memset(&fk[idx], 0, sizeof(fk[idx]));
	fk[idx].open = true;
	fk[idx].capture = idx == FK_CAPTURE;
	fk[idx].state = SNDRV_PCM_STATE_OPEN;
	snprintf(fk[idx].path, sizeof(fk[idx].path), "%s", path);
	fk_fd_live++;

	return FK_FD_BASE + (int)idx;
}

static inline int fk_ioctl(int fd, unsigned int req, void *arg)
{
	struct fk_stream *s = fk_stream_get(fd);
	int version;

	if (!s)
		return fk_fail(EBADF);
	if (s->state == SNDRV_PCM_STATE_DISCONNECTED)
		return fk_fail(EBADFD);

	switch (req) {
	case SNDRV_PCM_IOCTL_PVERSION:
		version = (int)fk_pversion;
		memcpy(arg, &version, sizeof(version));
		return 0;
	case SNDRV_PCM_IOCTL_USER_PVERSION:
		/* The call exists from protocol 2.0.14; an older kernel has no
		 * entry for it. */
		if (fk_pversion < SNDRV_PROTOCOL_VERSION(2, 0, 14) ||
		    fk_user_pversion_fail)
			return fk_fail(ENOTTY);
		return 0;
	case SNDRV_PCM_IOCTL_HW_REFINE:
		return fk_hw_refine(s, arg);
	case SNDRV_PCM_IOCTL_HW_PARAMS:
		return fk_hw_params(s, arg);
	case SNDRV_PCM_IOCTL_SW_PARAMS:
		return fk_sw_params(s, arg);
	case SNDRV_PCM_IOCTL_CHANNEL_INFO:
		return fk_channel_info(s, arg);
	case SNDRV_PCM_IOCTL_SYNC_PTR:
		return fk_sync_ptr(s, arg);
	case SNDRV_PCM_IOCTL_PREPARE:
		return fk_prepare(s);
	case SNDRV_PCM_IOCTL_START:
		return fk_start(s);
	case SNDRV_PCM_IOCTL_DROP:
		return fk_drop(s);
	case SNDRV_PCM_IOCTL_LINK:
		return fk_link(s, (int)(intptr_t)arg);
	case SNDRV_PCM_IOCTL_UNLINK:
		return fk_unlink(s);
	default:
		break;
	}

	return fk_fail(ENOTTY);
}

static inline void *fk_mmap(void *addr, size_t len, int prot, int flags, int fd,
		     off_t offset)
{
	struct fk_stream *s = fk_stream_get(fd);

	if (!s || !s->hw_set || offset || len > s->dma_bytes) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	s->mmap_count++;
	fk_map_live++;
	fk_map_prot[fk_index_get(s)] = prot;

	return s->dma;
}

static inline int fk_munmap(void *addr, size_t len)
{
	for (unsigned i = 0; i <= FK_CAPTURE; i++) {
		if (fk[i].dma != addr || !fk[i].mmap_count)
			continue;
		fk[i].mmap_count--;
		fk_map_live--;
		return 0;
	}

	return fk_fail(EINVAL);
}

static inline bool fk_poll_error(const struct fk_stream *s)
{
	if (s->state == SNDRV_PCM_STATE_RUNNING ||
	    s->state == SNDRV_PCM_STATE_PREPARED ||
	    s->state == SNDRV_PCM_STATE_PAUSED)
		return false;

	return !(s->state == SNDRV_PCM_STATE_DRAINING && s->capture);
}

static inline int fk_ppoll(struct pollfd *fds, nfds_t count,
		    const struct timespec *timeout, const sigset_t *mask)
{
	struct fk_stream *s = count ? fk_stream_get(fds[0].fd) : NULL;
	short ok;

	if (!s)
		return fk_fail(EBADF);
	fk_poll_n[fk_index_get(s)]++;
	fds[0].revents = 0;
	if (timeout) {
		fk_poll_timeout_sec = (long)timeout->tv_sec;
		fk_poll_timeout_nsec = (long)timeout->tv_nsec;
	}
	if (fk_poll_eintr[fk_index_get(s)]) {
		fk_poll_eintr[fk_index_get(s)]--;
		return fk_fail(EINTR);
	}
	if (fk_hold_periods[fk_index_get(s)] &&
	    (!fk_hold_gated ||
	     atomic_load_explicit(&fk_hold_go, memory_order_acquire))) {
		unsigned periods = fk_hold_periods[fk_index_get(s)];

		fk_hold_periods[fk_index_get(s)] = 0;
		fk_hold_taken[fk_index_get(s)]++;
		if (atomic_load(&fk_play_rec_arm))
			fk_play_rec_on = true;
		if (fk_phased)
			fk_time_pass((unsigned long)periods * s->period_frames +
				     fk_hold_extra[fk_index_get(s)], true);
		else
			fk_advance(periods);
		fk_hold_end_at = fk_now;
		fk_hold_extra[fk_index_get(s)] = 0;
	}
	ok = s->capture ? (short)(POLLIN | POLLRDNORM) :
			  (short)(POLLOUT | POLLWRNORM);
	if (fk_poll_group_xrun[fk_index_get(s)]) {
		fk_poll_group_xrun[fk_index_get(s)]--;
		/* the peer ring overran while this direction waited, and the
		 * stop reaches every linked stream */
		fk_group_state_set(&fk[FK_CAPTURE], SNDRV_PCM_STATE_XRUN);
	}
	/* Two grids interleave on the phased clock, so a wait can take twice
	 * the interrupts to see one period of its own. */
	for (unsigned n = 0; n < (fk_phased ? 2u : 1u) * FK_POLL_STEPS; n++) {
		if (fk_poll_error(s)) {
			fds[0].revents = (short)(ok | POLLERR);
			return 1;
		}
		if (fk_poll_timeouts[fk_index_get(s)]) {
			fk_poll_timeouts[fk_index_get(s)]--;
			return 0;
		}
		if (fk_avail(s) >= s->avail_min) {
			fds[0].revents = ok;
			return 1;
		}
		/* A wait only ends when an interrupt raises it, so a pair with
		 * no DMA running runs the loop out and times out. */
		fk_poll_steps_n++;
		fk_clock_step();
	}

	return 0;
}

/* The DMA runs on while the caller sleeps and raises no interrupt, so the
 * pointer callback only shows the progress at the next HWSYNC. */
static inline int fk_clock_nanosleep(clockid_t clock, int flags,
				     const struct timespec *request,
				     struct timespec *remain)
{
	fk_sleep_n++;
	fk_sleep_clock = clock;
	fk_sleep_flags = flags;
	fk_sleep_ns = (uint64_t)request->tv_sec * 1000000000u +
		      (uint64_t)request->tv_nsec;
	if (fk_sleep_deaf)
		return 0;
	if (fk_phased) {
		const struct fk_stream *s = fk[FK_CAPTURE].open ?
					    &fk[FK_CAPTURE] : &fk[FK_PLAY];

		fk_time_pass(fk_sleep_ns * s->rate / 1000000000u, false);
		return 0;
	}
	for (unsigned i = 0; i <= FK_CAPTURE; i++) {
		if (!fk[i].open || !fk[i].dma_on)
			continue;
		fk_dma_step_quiet(&fk[i],
				  fk_sleep_ns * fk[i].rate / 1000000000u);
	}

	return 0;
}

static inline int fk_close(int fd)
{
	struct fk_stream *s = fk_stream_get(fd);

	if (fd == FK_DRIVER_FD) {
		fk_fd_live--;
		return 0;
	}
	if (!s)
		return fk_fail(EBADF);
	free(s->dma);
	s->dma = NULL;
	s->open = false;
	fk_fd_live--;

	return 0;
}

static const struct tdm_raw_sys fk_sys = {
	.open = fk_open,
	.ioctl = fk_ioctl,
	.mmap = fk_mmap,
	.munmap = fk_munmap,
	.ppoll = fk_ppoll,
	.clock_nanosleep = fk_clock_nanosleep,
	.close = fk_close,
};

/* --- install and release ------------------------------------------------- */

static inline void fk_reset(const struct fk_profile *prof)
{
	for (unsigned i = 0; i <= FK_CAPTURE; i++) {
		free(fk[i].dma);
		memset(&fk[i], 0, sizeof(fk[i]));
		fk_sync_n[i] = 0;
		fk_sync_flags[i] = 0;
		fk_poll_n[i] = 0;
		fk_hw_params_n[i] = 0;
		fk_lag[i] = 0;
		fk_poll_timeouts[i] = 0;
		fk_hold_periods[i] = 0;
		fk_hold_extra[i] = 0;
		fk_hold_taken[i] = 0;
		memset(&fk_sync_fault[i], 0, sizeof(fk_sync_fault[i]));
		fk_poll_eintr[i] = 0;
		fk_poll_group_xrun[i] = 0;
		fk_sw_proto[i] = 0;
		fk_sw_period_step[i] = 0;
		fk_map_prot[i] = 0;
	}
	memset(fk_journal, 0, sizeof(fk_journal));
	memset(fk_journal_dir, 0, sizeof(fk_journal_dir));
	memset(fk_start_threshold_seen, 0, sizeof(fk_start_threshold_seen));
	fk_journal_len = 0;
	fk_group[0] = FK_PLAY;
	fk_group[1] = FK_CAPTURE;
	fk_linked = false;
	fk_avail_min_unset = 0;
	fk_start_unprepared = 0;
	fk_reject_periods = 0;
	fk_reject_errno = EINVAL;
	fk_peer_busy = false;
	fk_jit_playback = prof->tx_ack;
	fk_silence_fill = false;
	fk_poll_timeout_sec = -1;
	fk_poll_timeout_nsec = -1;
	fk_poll_steps_n = 0;
	fk_sleep_n = 0;
	fk_sleep_ns = 0;
	fk_sleep_clock = (clockid_t)-1;
	fk_sleep_flags = -1;
	fk_sleep_deaf = false;
	fk_phased = false;
	fk_tx_restart_at_kick = false;
	fk_now = 0;
	fk_ack_frames = 0;
	fk_hold_push = 0;
	fk_hold_push_extra = 0;
	fk_hold_push_taken = 0;
	fk_push_n = 0;
	fk_read_n = 0;
	fk_age_down = 0;
	fk_age_up = 0;
	fk_tx_dry_n = 0;
	fk_tx_pending = 0;
	fk_play_empty_reads = 0;
	fk_play_empty_pushes = 0;
	fk_driver_hidden = false;
	fk_fault_at_hold = false;
	fk_hold_gated = false;
	atomic_store(&fk_hold_go, false);
	atomic_store(&fk_play_rec_arm, false);
	fk_play_rec_on = false;
	fk_play_rec_n = 0;
	fk_hold_end_at = 0;
	memset(fk_push_at, 0, sizeof(fk_push_at));
	memset(fk_cap_at, 0, sizeof(fk_cap_at));
	fk_fd_live = 0;
	fk_map_live = 0;
	fk_open_errno = 0;
	fk_refine_format_fail = false;
	fk_pversion = SNDRV_PCM_VERSION;
	fk_user_pversion_fail = false;
	fk_periods_max_open = false;
	fk_info_no_mmap = false;
	fk_boundary_odd = false;
	fk_buffer_extra = 0;
	fk_channel_info_fault = FK_CI_NONE;
	fk_periods_floor_refuse = 0;
	fk_prof = prof;
	udsp_tdm_raw_sys_override(&fk_sys);
}

/* Puts the pair, or one direction of it, into a state the cases cannot reach
 * through the model's own clock. It changes the state alone: the trigger flags
 * stay as they were, so a forced xrun still steps its DMA at the next clock
 * step, which is what a case that wants a stopped ring must arrange itself. */
static inline void fk_state_force(int state, bool both)
{
	fk[FK_CAPTURE].state = state;
	if (!both)
		return;
	fk[FK_PLAY].state = state;
}

static inline void fk_release(void)
{
	for (unsigned i = 0; i <= FK_CAPTURE; i++) {
		free(fk[i].dma);
		fk[i].dma = NULL;
		fk[i].open = false;
	}
	udsp_tdm_raw_sys_override(NULL);
}

#endif
