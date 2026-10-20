// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * udsp TDM plug-in contract. Stable contract. One plug-in moves 16-bit linear
 * PCM between the engine and the SLIC PCM highway. Three implementations:
 * tdm/alsa.c (two unidirectional ALSA devices through alsa-lib, linked),
 * tdm/alsa_raw.c (the same two devices through the kernel ioctl interface
 * alone, linked) and tdm/file.c (a virtual-clock file pair for host CI).
 *
 * Geometry. The engine tick is one period. frames_per_period is rate/100
 * (10 ms: 80 at 8 kHz, 160 at 16 kHz). channels is 1 or 2: some ALSA back
 * ends refuse a period below a byte floor (MediaTek mt7986 period_bytes_min
 * 256), so the engine can open 2 channels and use slot 0. open() takes the
 * requested geometry and fills an out-geometry with what it actually got;
 * read_period and write_period then always present one engine-facing layout,
 * planar with channel 0 first, whatever the hardware access layout is, so the
 * engine never misreads interleaved as noninterleaved. The plug-in does any
 * conversion.
 *
 * Refusal. open() returns NULL on failure and fills the caller's
 * udsp_tdm_fail when one is given: the negative errno of the call or check
 * that refused, the direction it refused in, a static token naming it, and
 * whether the refusal is the device's answer about the geometry. Only that
 * geometry answer may move the line rate (engine.h enum engine_open_fail); a
 * missing device node, a device held by another process, a protocol
 * mismatch or a ring shape the plug-in cannot serve fails the same way at
 * any rate. The plug-in logs nothing: the library composes the one line it
 * prints from the record (core/udsp.c udsp_engine_fail_log).
 *
 * Linked start. Playback and capture do not share a tick on every SoC. The
 * engine prefills playback with geom.prefill_periods of silence before
 * start(); start() starts both together (snd_pcm_link on ALSA). The engine
 * keeps a running phase estimate and resyncs by dropping or inserting a
 * frame, because a TX ring can re-phase after a dry event and an underrun is
 * silent on some drivers. The engine calls recover() on every counted fault,
 * an xrun or any other negative errno, then re-prefills and starts again, so
 * recover() must work from a running group as well as from a stopped one.
 *
 * All calls run on the engine thread only. read_period blocks until one capture
 * period is available; write_period queues one playback period. delay returns
 * the playback frames still buffered, for the stall estimator.
 */
#ifndef UDSP_TDM_H
#define UDSP_TDM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum udsp_tdm_layout {
	UDSP_TDM_INTERLEAVED = 0,
	UDSP_TDM_NONINTERLEAVED,
};

struct udsp_tdm_geom {
	unsigned frames_per_period;	/* rate/100: 80 at 8 kHz, 160 at 16 kHz */
	unsigned channels;		/* 1 or 2 */
	/* in want: the desired playback pre-fill depth. in got: the granted
	 * buffer depth in periods, an upper bound the engine clamps its
	 * prefill to. */
	unsigned prefill_periods;
	enum udsp_tdm_layout layout;
	/* got only: true when the hardware paces the tick (ALSA), false for a
	 * virtual clock (file). The engine refuses an unbounded run on an
	 * unpaced backend, because nothing then bounds what the file backend
	 * writes. */
	bool paced;
	/* Sample rate in Hz; 0 means 8000 on both sides so an untouched
	 * backend still conforms. In want it is the requested line rate; in
	 * got the granted one; the engine refuses a mismatch after
	 * normalising the zeros. */
	unsigned rate;
	/* got only: the granted capture ring depth in periods. A hardware
	 * back end keeps capture deeper than playback, so a late engine finds
	 * its periods still in the ring; the file back end reports its
	 * virtual ring. */
	unsigned capture_periods;
	/* got only: the playback DMA stops when no period is queued, its
	 * pointer stays at the application pointer, and the next write
	 * restarts it at that instant. The engine then restarts a ring that
	 * ran dry on a capture completion (engine.c engine_rephase_read). The
	 * raw back end sets it for a known driver only (alsa_raw.c
	 * tdm_raw_driver_known); every other back end leaves it false. The
	 * field makes the struct larger, so a back end or a caller built
	 * apart from libudsp must be rebuilt against this header. */
	bool tx_restarts;
};

/* The direction a refusal belongs to; NONE for the pair or the arguments. */
enum udsp_tdm_dir {
	UDSP_TDM_DIR_NONE = -1,
	UDSP_TDM_DIR_PLAYBACK = 0,
	UDSP_TDM_DIR_CAPTURE = 1,
};

/* Why open() returned NULL. A plug-in fills every field on every NULL return
 * when the caller passes a record. */
struct udsp_tdm_fail {
	/* true only when the device declined the rate, the period, the
	 * channel count or the ring the geometry asked for. Every other cause
	 * fails the same way at any rate, so the engine must not move the
	 * rate on it. */
	bool geometry;
	/* The negative errno of the refusing call, or -EINVAL for a check of
	 * the plug-in itself. 0 means no record was written. */
	int err;
	enum udsp_tdm_dir stream;
	/* A static token naming the call or the check, not a sentence: the
	 * library composes the line and the plug-in authors no log text. */
	const char *what;
};

struct udsp_tdm;

struct udsp_tdm_ops {
	const char *name;

	/* Open both directions from a config string (ALSA device names or file
	 * paths). want is the requested geometry; got is filled with the
	 * actual one. Returns NULL on failure and, when fail is not NULL,
	 * fills it with the reason (the Refusal note). Does not start the
	 * clocks. */
	struct udsp_tdm *(*open)(const char *play_dev, const char *capture_dev,
				 const struct udsp_tdm_geom *want,
				 struct udsp_tdm_geom *got,
				 struct udsp_tdm_fail *fail);
	void (*close)(struct udsp_tdm *t);

	/* Prepare both directions after an xrun; the engine re-prefills and
	 * calls start() again. */
	int (*recover)(struct udsp_tdm *t);

	/* Start both directions together so their phase is defined (the
	 * clocks must run before the SLIC leaves reset). */
	int (*start)(struct udsp_tdm *t);
	int (*stop)(struct udsp_tdm *t);

	/* One period each. buf is channels * frames_per_period int16 samples,
	 * planar with channel 0 first, the fixed engine-facing layout of the
	 * Geometry note; the plug-in converts to and from the device access
	 * layout. read_period blocks. Both return 0 on success, a negative
	 * errno on a fault the engine must count and, for an xrun, recover()
	 * + re-prefill + start().
	 *
	 * A positive return is a skip, not a fault. read_period returns the
	 * count of capture periods the ring overwrote before the engine read
	 * them; buf then holds the oldest period still intact and the
	 * application pointer stands one period past it. write_period returns
	 * the count of playback periods that expired: the hardware pointer ran
	 * past the application pointer (a cyclic DMA under a no-stop policy),
	 * and the back end moved the application pointer to the next period
	 * boundary past the hardware position before it wrote. The engine
	 * counts both and treats the skipped periods as elapsed time. */
	int (*read_period)(struct udsp_tdm *t, int16_t *buf);
	int (*write_period)(struct udsp_tdm *t, const int16_t *buf);

	/* Playback frames still buffered, for the stall estimator. Negative
	 * errno on error. Read from the pointer pair the last call cached, so
	 * the engine measures with it and never decides on it; a decision
	 * takes playback_room_fresh. */
	long (*delay)(struct udsp_tdm *t);

	/* Capture frames ready to read, for a direct backlog measurement.
	 * Nullable: a backend without it leaves the engine on the
	 * tick-interval inference. Both built-in backends implement it. */
	long (*avail)(struct udsp_tdm *t);

	/* The three calls a late engine decides on, each after one fresh read
	 * of the pointer pair. All nullable; a back end that lacks them
	 * leaves the engine without the catch-up rule.
	 *
	 * capture_avail_fresh: capture frames ready to read now.
	 * playback_room_fresh: playback frames writable now; 0 when the ring
	 * is full, more than one ring when the hardware ran past the
	 * application pointer.
	 * capture_skip: advance the capture application pointer by whole
	 * periods without a copy, so the engine can drop the backlog a hold
	 * left behind. 0 on success, a negative errno on a fault. */
	long (*capture_avail_fresh)(struct udsp_tdm *t);
	long (*playback_room_fresh)(struct udsp_tdm *t);
	int (*capture_skip)(struct udsp_tdm *t, unsigned periods);
};

/* The built-in backends. alsa is compiled only with HAVE_ALSA; alsa_raw needs
 * the kernel uapi header alone and is always present; file is always present
 * for host CI. */
extern const struct udsp_tdm_ops udsp_tdm_alsa;
extern const struct udsp_tdm_ops udsp_tdm_alsa_raw;
extern const struct udsp_tdm_ops udsp_tdm_file;

#endif
