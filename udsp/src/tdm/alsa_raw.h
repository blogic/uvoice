// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Private interface of the raw ALSA tdm back end (tdm/alsa_raw.c). This header
 * is not public. The stable contract is include/udsp/tdm.h, which also declares
 * the two other backend objects.
 *
 * The back end drives the PCM character devices with the kernel uapi alone, so
 * it makes exactly seven system calls. The host test replaces all seven through
 * the injected syscall table below: udsp_tdm_raw_sys_override installs a table
 * and a NULL argument restores the libc table. The same pattern serves the tdm
 * plug-in itself in core/udsp_priv.h.
 *
 * The pure helpers below carry the arithmetic the back end depends on, so the
 * test pins each one on its own value set: the device string parser, the
 * hw_params builder, the ring pointer folds of include/sound/pcm.h
 * (snd_pcm_capture_avail, snd_pcm_playback_avail, snd_pcm_playback_hw_avail),
 * the boundary of snd_pcm_hw_params, and the channel area address formula of
 * snd_pcm_lib_ioctl_channel_info.
 */
#ifndef UDSP_TDM_ALSA_RAW_H
#define UDSP_TDM_ALSA_RAW_H

#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>

/* The uapi header defines the request numbers with the ioctl macros, and it
 * does not pull them in on Linux. */
#include <sound/asound.h>

#include "udsp/tdm.h"

extern const struct udsp_tdm_ops udsp_tdm_alsa_raw;

/* The seven system calls the back end makes. Each member keeps the libc return
 * convention: a negative result reports the failure in errno. clock_nanosleep
 * is the exception libc makes too, and returns the error itself. */
struct tdm_raw_sys {
	int (*open)(const char *path, int flags);
	int (*ioctl)(int fd, unsigned int req, void *arg);
	void *(*mmap)(void *addr, size_t len, int prot, int flags, int fd,
		      off_t offset);
	int (*munmap)(void *addr, size_t len);
	int (*ppoll)(struct pollfd *fds, nfds_t count,
		     const struct timespec *timeout, const sigset_t *mask);
	int (*clock_nanosleep)(clockid_t clock, int flags,
			       const struct timespec *request,
			       struct timespec *remain);
	int (*close)(int fd);
};

extern const struct tdm_raw_sys tdm_raw_sys_libc;

/* Test only. NULL restores tdm_raw_sys_libc. */
void udsp_tdm_raw_sys_override(const struct tdm_raw_sys *sys);

/* Turns an accepted device string into the character device path of one
 * direction. Accepts hw:C,D, plughw:C,D and /dev/snd/pcmC<n>D<n>[pc]; refuses
 * every other alsa-lib name. Returns false on a refusal. */
bool tdm_raw_path_build(const char *dev, bool capture, char *out, size_t len);

/* Fills a hw_params request: every mask and interval open, then the access mask
 * cut to the two mmap layouts, S16_LE, the standard subformat, and the pinned
 * channel count, rate, period and period count. A zero leaves that one
 * parameter open, so all four zero is the access and format probe alone. */
void tdm_raw_params_build(struct snd_pcm_hw_params *p, unsigned channels,
			  unsigned rate, unsigned period_frames,
			  unsigned periods);

/* The pointer wrap point, as snd_pcm_hw_params computes it. */
unsigned long tdm_raw_boundary_get(unsigned long buffer_frames);

unsigned long tdm_raw_capture_avail_get(unsigned long hw_ptr,
					unsigned long appl_ptr,
					unsigned long boundary);

unsigned long tdm_raw_playback_avail_get(unsigned long hw_ptr,
					 unsigned long appl_ptr,
					 unsigned long buffer_frames,
					 unsigned long boundary);

/* Playback frames the ring still holds. Negative after an underrun, which the
 * delay op clamps to zero for the contract of tdm.h. */
long tdm_raw_playback_queued_get(unsigned long hw_ptr, unsigned long appl_ptr,
				 unsigned long buffer_frames,
				 unsigned long boundary);

/* Byte offset of one sample inside the mapped ring. One formula serves the
 * interleaved and the noninterleaved layout. */
size_t tdm_raw_sample_offset_get(unsigned first_bytes, unsigned step_bytes,
				 unsigned frame);

#endif
