// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Raw ALSA tdm back end. It serves the contract of tdm.h with the kernel uapi
 * alone: it opens the two PCM character devices, negotiates the geometry with
 * HW_REFINE and HW_PARAMS, maps the data ring and moves one period per tick in
 * and out of that mapping. No sound library takes part. The ops table is
 * udsp_tdm_alsa_raw and the backend name is "alsa-raw".
 *
 * Device strings. The parser accepts hw:C,D, plughw:C,D and an explicit
 * /dev/snd/pcmC<n>D<n>[pc] path, and refuses every other alsa-lib name. Both
 * card names open the same character device, so a configuration written for
 * tdm/alsa.c keeps working when tdm_backend changes.
 *
 * Refusals. Every step of the open hands back the negative errno of the call
 * or check that refused and writes the caller's udsp_tdm_fail with the
 * direction and a static token (tdm.h Refusal). The record marks the device's
 * answer about the geometry only where it is one: the pinned HW_REFINE, a
 * grant that differs from the request, and a pair that disagrees. A bad device
 * name, a node another process holds, the protocol version, an exhausted
 * period-count search, a ring the copy cannot serve, the boundary, the channel
 * areas, the mapping and the link fail the same way at any rate. The refine
 * therefore runs twice: first with access, format and subformat pinned and
 * every interval open, so a device without S16_LE or mmap access refuses on a
 * call of its own, then with the channel count, rate and period pinned. The
 * kernel copies nothing back from a refine that failed, so one call could not
 * tell the two apart.
 *
 * Pointer bookkeeping. arm64 refuses the status and control page mapping
 * (sound/core/pcm_native.c), so every pointer read and every pointer push is a
 * SNDRV_PCM_IOCTL_SYNC_PTR call. A push carries HWSYNC and AVAIL_MIN with the
 * application pointer one period further; a read carries HWSYNC, APPL and
 * AVAIL_MIN. AVAIL_MIN is set on every call, so the wake threshold stays what
 * SW_PARAMS set. Both stop thresholds sit at the boundary, because a late
 * engine must not stop the linked pair and its SLIC clocks. Non-BATCH playback
 * uses kernel silence fill because its DMA reads the ring even without new
 * application data. BATCH is the uapi proxy for the AN7581, which arms only
 * periods the application supplies. A BATCH device with cyclic DMA would
 * replay stale audio during a hold because this policy leaves silence fill
 * off. A failed call leaves the cached pair untouched.
 *
 * Capture targets 16 periods or the deeper playback request, so the ring
 * retains data during a hold.
 * Playback keeps the requested depth to bound latency. A positive read return
 * counts overwritten capture periods; a positive write return counts expired
 * playback periods. The pointers stay on the period grid, so a one-period
 * copy never splits at the ring end. capture_avail_fresh and
 * playback_room_fresh each read the pointer pair once, so engine decisions
 * do not depend on cached delay.
 * capture_skip pushes one whole-period advance so the engine can trim backlog
 * without discarding playback that already entered the echo reference.
 *
 * Every difference between two pointers goes through the folds of
 * include/sound/pcm.h, because the pointers advance modulo the boundary that
 * SW_PARAMS reports and plain subtraction is wrong once one of them wrapped.
 *
 * Linked start. The capture stream links the playback stream, the same order
 * snd_pcm_link(capture, playback) uses in tdm/alsa.c, so the playback stream
 * triggers first. PREPARE, START and DROP then reach both directions through
 * the capture descriptor. PREPARE precedes every START, because the AN7581
 * block zeroes its own cursors at the start and both pointer sets must agree.
 * The recovery adds a DROP before the PREPARE, so both directions are idle
 * whatever path led there and the AN7581 capture prepare runs its framing
 * resync instead of skipping it.
 *
 * Two wake rules, chosen by one bit. Some position registers count samples.
 * Such a register lags the period boundary at the interrupt, because the part
 * holds a few frames of its own. The MediaTek front end states this in
 * sound/soc/mediatek/common/mtk-afe-fe-dai.c. The kernel stores the position as
 * it stands. Its wake rule needs an available count at or above avail_min. That
 * count is short at the interrupt, so the kernel does not wake the reader. The
 * reader waits for the next interrupt and then finds two periods. The MT7987 at
 * 16 kHz measured 67.7 wakes per second, a third of the ticks in pairs.
 *
 * So a device that clears SNDRV_PCM_INFO_BATCH gets a capture wake threshold of
 * three quarters of a period. The read asks for the pointer once. If the period
 * is short, the read sleeps for the missing frames and asks again. It makes
 * four rounds at most. The BATCH bit is the kernel's own statement that a
 * pointer moves in period steps. A device that sets the bit carries a whole
 * period per wake, so the read asks for nothing extra. The playback threshold
 * stays one period on both. Its wake only matters in the back-pressure branch,
 * which reads the pointer first.
 *
 * One tick on a device that sets BATCH costs one ppoll and three SYNC_PTR
 * calls: the capture push in the read, the playback push in the write, and one
 * capture read beside the delay. A device that clears BATCH adds the completion
 * read, and a second read when the wake was short. Four calls, or five, is the
 * common case. The write adds one more read where the playback pointer lags the
 * period boundary by more than the capture pointer does. The write also reads
 * when its cached queue is below one period or playback_room_fresh ran this
 * tick. A completion whose sleeps keep returning early costs up to two reads
 * beyond the five, because it stops at TDM_RAW_COMPLETE_ROUNDS reads. The
 * delay op reads the cached playback pair and
 * issues no call, so this back end never reports a delay error. An xrun reaches
 * the engine through the next push or the refreshed capture backlog.
 *
 * Two waits and the completion sleeps bound one tick. The read waits up to
 * 100 ms for the wake. The completion then sleeps at most three times. Each
 * sleep covers at most a quarter period plus TDM_RAW_SLEEP_MARGIN_NS. The read
 * therefore blocks up to 100 ms plus three quarters of a period and three
 * margins. The write waits up to 100 ms more for ring space. A tick blocks
 * 200 ms plus three quarters of a period and three margins at worst. Each
 * wait ends in the transient -EAGAIN the engine retries.
 *
 * The tick path allocates nothing, logs nothing and uses no floating point.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "udsp/tdm.h"
#include "alsa_raw.h"
#include "tdm_common.h"

/* The 32-bit interface folds the pointers into another layout, so the native
 * SNDRV_PCM_IOCTL_SYNC_PTR structure holds only on a 64-bit user space. */
_Static_assert(sizeof(long) == 8, "the raw ALSA back end needs a 64-bit long");

/* Three times the worst tick interval measured under load, as tdm/alsa.c. */
#define TDM_RAW_WAIT_MS		100

/* /dev/snd/pcmC<10 digits>D<10 digits><direction> and the terminator. */
#define TDM_RAW_PATH_MAX	40

/* How far above the first candidate the period count may climb. The buffer
 * byte step of the MediaTek front end needs a few counts at most. */
#define TDM_RAW_PERIODS_TRIES	16

/* Sixteen covers the AN7581 limit of 15 and the AFE capture target of 16. */
#define TDM_RAW_CAPTURE_PERIODS	16

/* The share of a period the capture wake threshold sits below one period on a
 * device whose pointer moves in samples, and the number of times the read then
 * asks again for the rest of the period. */
#define TDM_RAW_WAKE_MARGIN	4
#define TDM_RAW_COMPLETE_ROUNDS	4

/* Added to every completion sleep, for the granularity of the timer. */
#define TDM_RAW_SLEEP_MARGIN_NS	50000

#define MASK_INDEX(k)		((k) - SNDRV_PCM_HW_PARAM_FIRST_MASK)
#define INTERVAL_INDEX(k)	((k) - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL)

enum tdm_raw_wait {
	TDM_RAW_WAIT_TIMEOUT = 0,
	TDM_RAW_WAIT_READY,
	TDM_RAW_WAIT_ERROR,
};

struct tdm_raw_stream {
	int fd;
	bool capture;
	unsigned char *base;
	size_t map_bytes;
	unsigned channels;
	unsigned rate;
	unsigned period_frames;
	unsigned periods;
	unsigned buffer_frames;
	unsigned avail_min;
	unsigned first_bytes[TDM_MAX_CHANNELS];
	unsigned step_bytes;
	unsigned long boundary;
	unsigned long hw_ptr;
	unsigned long appl_ptr;
	enum udsp_tdm_layout layout;
	/* one period in the device layout, for the interleaved copy */
	int16_t *scratch;
	/* the device moves its pointer in period steps */
	bool batch;
	/* the driver acts on each application pointer move (.ack) */
	bool sync_applptr;
};

struct udsp_tdm {
	const struct tdm_raw_sys *sys;
	struct tdm_raw_stream play;
	struct tdm_raw_stream capture;
	struct udsp_tdm_geom geom;
	struct snd_pcm_sync_ptr sync;
	/* true when a write ran since the last capture pointer read, so the
	 * cached capture pair is older than the playback one. */
	bool capture_stale;
	/* DMA can advance after a fresh-room check and before the write. */
	bool playback_refresh;
};

/* --- the injected syscall table --------------------------------------- */

static int sys_open_libc(const char *path, int flags)
{
	return open(path, flags);
}

static int sys_ioctl_libc(int fd, unsigned int req, void *arg)
{
	return ioctl(fd, req, arg);
}

static void *sys_mmap_libc(void *addr, size_t len, int prot, int flags, int fd,
			   off_t offset)
{
	return mmap(addr, len, prot, flags, fd, offset);
}

static int sys_munmap_libc(void *addr, size_t len)
{
	return munmap(addr, len);
}

static int sys_ppoll_libc(struct pollfd *fds, nfds_t count,
			  const struct timespec *timeout, const sigset_t *mask)
{
	return ppoll(fds, count, timeout, mask);
}

/* CLOCK_MONOTONIC is named rather than left to the clock nanosleep picks,
 * because a step of the wall clock must not lengthen a tick. */
static int sys_sleep_libc(clockid_t clock, int flags,
			  const struct timespec *request,
			  struct timespec *remain)
{
	return clock_nanosleep(clock, flags, request, remain);
}

static int sys_close_libc(int fd)
{
	return close(fd);
}

const struct tdm_raw_sys tdm_raw_sys_libc = {
	.open = sys_open_libc,
	.ioctl = sys_ioctl_libc,
	.mmap = sys_mmap_libc,
	.munmap = sys_munmap_libc,
	.ppoll = sys_ppoll_libc,
	.clock_nanosleep = sys_sleep_libc,
	.close = sys_close_libc,
};

static const struct tdm_raw_sys *tdm_raw_sys = &tdm_raw_sys_libc;

void udsp_tdm_raw_sys_override(const struct tdm_raw_sys *sys)
{
	tdm_raw_sys = sys ? sys : &tdm_raw_sys_libc;
}

/* --- device strings ---------------------------------------------------- */

static const char *uint_parse(const char *p, unsigned *out)
{
	const char *start = p;
	unsigned v = 0;

	while (*p >= '0' && *p <= '9') {
		unsigned digit = (unsigned)(*p - '0');

		if (v > (UINT_MAX - digit) / 10u)
			return NULL;
		v = v * 10u + digit;
		p++;
	}
	if (p == start)
		return NULL;
	*out = v;

	return p;
}

static bool name_parse(const char *dev, unsigned *card, unsigned *device)
{
	const char *p = NULL;

	if (!strncmp(dev, "plughw:", 7))
		p = dev + 7;
	else if (!strncmp(dev, "hw:", 3))
		p = dev + 3;
	if (!p)
		return false;

	p = uint_parse(p, card);
	if (!p || *p != ',')
		return false;
	p = uint_parse(p + 1, device);

	return p && !*p;
}

static bool node_parse(const char *dev, bool capture, unsigned *card,
		       unsigned *device)
{
	static const char prefix[] = "/dev/snd/pcmC";
	const char *p;

	if (strncmp(dev, prefix, sizeof(prefix) - 1))
		return false;

	p = uint_parse(dev + sizeof(prefix) - 1, card);
	if (!p || *p != 'D')
		return false;
	p = uint_parse(p + 1, device);
	if (!p || *p != (capture ? 'c' : 'p'))
		return false;

	return !p[1];
}

bool tdm_raw_path_build(const char *dev, bool capture, char *out, size_t len)
{
	unsigned card = 0, device = 0;
	int n;

	if (!dev || !out || !len)
		return false;
	if (!name_parse(dev, &card, &device) &&
	    !node_parse(dev, capture, &card, &device))
		return false;

	n = snprintf(out, len, "/dev/snd/pcmC%uD%u%c", card, device,
		     capture ? 'c' : 'p');

	return n > 0 && (size_t)n < len;
}

/* --- hw_params ---------------------------------------------------------- */

static void mask_any_set(struct snd_pcm_hw_params *p, int k)
{
	memset(p->masks[MASK_INDEX(k)].bits, 0xff,
	       sizeof(p->masks[MASK_INDEX(k)].bits));
}

static void mask_none_set(struct snd_pcm_hw_params *p, int k)
{
	memset(p->masks[MASK_INDEX(k)].bits, 0,
	       sizeof(p->masks[MASK_INDEX(k)].bits));
}

static void mask_bit_set(struct snd_pcm_hw_params *p, int k, unsigned bit)
{
	p->masks[MASK_INDEX(k)].bits[bit / 32u] |= 1u << (bit % 32u);
}

static void interval_any_set(struct snd_pcm_hw_params *p, int k)
{
	struct snd_interval *i = &p->intervals[INTERVAL_INDEX(k)];

	i->min = 0;
	i->max = UINT_MAX;
	i->openmin = 0;
	i->openmax = 0;
	i->integer = 0;
	i->empty = 0;
}

static void interval_value_set(struct snd_pcm_hw_params *p, int k, unsigned v)
{
	struct snd_interval *i = &p->intervals[INTERVAL_INDEX(k)];

	i->min = v;
	i->max = v;
	i->openmin = 0;
	i->openmax = 0;
	i->integer = 1;
	i->empty = 0;
}

void tdm_raw_params_build(struct snd_pcm_hw_params *p, unsigned channels,
			  unsigned rate, unsigned period_frames,
			  unsigned periods)
{
	int k;

	memset(p, 0, sizeof(*p));
	for (k = SNDRV_PCM_HW_PARAM_FIRST_MASK;
	     k <= SNDRV_PCM_HW_PARAM_LAST_MASK; k++)
		mask_any_set(p, k);
	for (k = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
	     k <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; k++)
		interval_any_set(p, k);

	/* The kernel takes the first access the driver offers, so this pair
	 * grants interleaved on the MediaTek AFE and noninterleaved on the
	 * AN7581 without a second refine. */
	mask_none_set(p, SNDRV_PCM_HW_PARAM_ACCESS);
	mask_bit_set(p, SNDRV_PCM_HW_PARAM_ACCESS,
		     (unsigned)SNDRV_PCM_ACCESS_MMAP_INTERLEAVED);
	mask_bit_set(p, SNDRV_PCM_HW_PARAM_ACCESS,
		     (unsigned)SNDRV_PCM_ACCESS_MMAP_NONINTERLEAVED);
	mask_none_set(p, SNDRV_PCM_HW_PARAM_FORMAT);
	mask_bit_set(p, SNDRV_PCM_HW_PARAM_FORMAT,
		     (unsigned)SNDRV_PCM_FORMAT_S16_LE);
	mask_none_set(p, SNDRV_PCM_HW_PARAM_SUBFORMAT);
	mask_bit_set(p, SNDRV_PCM_HW_PARAM_SUBFORMAT,
		     (unsigned)SNDRV_PCM_SUBFORMAT_STD);

	if (channels)
		interval_value_set(p, SNDRV_PCM_HW_PARAM_CHANNELS, channels);
	if (rate)
		interval_value_set(p, SNDRV_PCM_HW_PARAM_RATE, rate);
	if (period_frames)
		interval_value_set(p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
				   period_frames);
	if (periods)
		interval_value_set(p, SNDRV_PCM_HW_PARAM_PERIODS, periods);

	p->rmask = ~0u;
}

static int bit_first_get(uint32_t word)
{
	for (unsigned b = 0; b < 32; b++) {
		if (word & (1u << b))
			return (int)b;
	}

	return -1;
}

static bool mask_bit_first_get(const struct snd_pcm_hw_params *p, int k,
			       unsigned *out)
{
	const struct snd_mask *m = &p->masks[MASK_INDEX(k)];
	const unsigned words = (unsigned)(sizeof(m->bits) / sizeof(m->bits[0]));

	for (unsigned i = 0; i < words; i++) {
		int b = bit_first_get(m->bits[i]);

		if (b < 0)
			continue;
		*out = i * 32u + (unsigned)b;
		return true;
	}

	return false;
}

static bool interval_value_get(const struct snd_pcm_hw_params *p, int k,
			       unsigned *out)
{
	const struct snd_interval *i = &p->intervals[INTERVAL_INDEX(k)];

	if (i->empty || i->min != i->max)
		return false;
	*out = i->min;

	return true;
}

static bool interval_range_get(const struct snd_pcm_hw_params *p, int k,
			       unsigned *min, unsigned *max)
{
	const struct snd_interval *i = &p->intervals[INTERVAL_INDEX(k)];

	if (i->empty || i->min > i->max)
		return false;
	*min = i->min + i->openmin;
	*max = i->max - i->openmax;

	return *min <= *max && *max != UINT_MAX;
}

/* --- pointer arithmetic -------------------------------------------------- */

unsigned long tdm_raw_boundary_get(unsigned long buffer_frames)
{
	unsigned long boundary = buffer_frames;

	if (!buffer_frames)
		return 0;
	while (boundary * 2 <= (unsigned long)LONG_MAX - buffer_frames)
		boundary *= 2;

	return boundary;
}

unsigned long tdm_raw_capture_avail_get(unsigned long hw_ptr,
					unsigned long appl_ptr,
					unsigned long boundary)
{
	if (hw_ptr >= appl_ptr)
		return hw_ptr - appl_ptr;

	return hw_ptr + boundary - appl_ptr;
}

unsigned long tdm_raw_playback_avail_get(unsigned long hw_ptr,
					 unsigned long appl_ptr,
					 unsigned long buffer_frames,
					 unsigned long boundary)
{
	unsigned long avail;

	if (hw_ptr + buffer_frames >= appl_ptr)
		avail = hw_ptr + buffer_frames - appl_ptr;
	else
		avail = hw_ptr + buffer_frames + boundary - appl_ptr;
	if (avail >= boundary)
		avail -= boundary;

	return avail;
}

long tdm_raw_playback_queued_get(unsigned long hw_ptr, unsigned long appl_ptr,
				 unsigned long buffer_frames,
				 unsigned long boundary)
{
	unsigned long avail = tdm_raw_playback_avail_get(hw_ptr, appl_ptr,
							 buffer_frames,
							 boundary);

	return (long)buffer_frames - (long)avail;
}

size_t tdm_raw_sample_offset_get(unsigned first_bytes, unsigned step_bytes,
				 unsigned frame)
{
	return (size_t)first_bytes + (size_t)frame * step_bytes;
}

/* --- the ioctl and pointer helpers --------------------------------------- */

static int ioctl_run(const struct udsp_tdm *t, int fd, unsigned int req,
		     void *arg)
{
	if (t->sys->ioctl(fd, req, arg) < 0)
		return -errno;

	return 0;
}

static int state_errno_get(snd_pcm_state_t state)
{
	if (state == SNDRV_PCM_STATE_RUNNING ||
	    state == SNDRV_PCM_STATE_PREPARED ||
	    state == SNDRV_PCM_STATE_DRAINING)
		return 0;
	if (state == SNDRV_PCM_STATE_XRUN)
		return -EPIPE;
	if (state == SNDRV_PCM_STATE_SUSPENDED)
		return -ESTRPIPE;

	/* A removed card answers -EBADFD from the ioctl itself, so the engine
	 * sees the same code from either route. */
	return -EBADFD;
}

static int sync_ptr_issue(struct udsp_tdm *t, struct tdm_raw_stream *s,
			  unsigned flags, unsigned long appl)
{
	struct snd_pcm_sync_ptr *sp = &t->sync;
	int rc;

	memset(sp, 0, sizeof(*sp));
	sp->flags = flags;
	sp->c.control.appl_ptr = appl;
	sp->c.control.avail_min = s->avail_min;
	rc = ioctl_run(t, s->fd, SNDRV_PCM_IOCTL_SYNC_PTR, sp);
	if (rc < 0)
		return rc;
	s->hw_ptr = sp->s.status.hw_ptr;
	s->appl_ptr = sp->c.control.appl_ptr;

	return 0;
}

/* Loads the pointer pair without a state gate. The prepare sets the pair, so
 * this is the first read of both streams and the only call that runs before the
 * stream is prepared. */
static int sync_ptr_prime(struct udsp_tdm *t, struct tdm_raw_stream *s)
{
	return sync_ptr_issue(t, s, SNDRV_PCM_SYNC_PTR_APPL |
				    SNDRV_PCM_SYNC_PTR_AVAIL_MIN, s->appl_ptr);
}

static int sync_ptr_read(struct udsp_tdm *t, struct tdm_raw_stream *s)
{
	int rc = sync_ptr_issue(t, s, SNDRV_PCM_SYNC_PTR_HWSYNC |
				      SNDRV_PCM_SYNC_PTR_APPL |
				      SNDRV_PCM_SYNC_PTR_AVAIL_MIN,
			        s->appl_ptr);

	return rc < 0 ? rc : state_errno_get(t->sync.s.status.state);
}

static int sync_ptr_push(struct udsp_tdm *t, struct tdm_raw_stream *s,
			 unsigned long appl)
{
	int rc = sync_ptr_issue(t, s, SNDRV_PCM_SYNC_PTR_HWSYNC |
				      SNDRV_PCM_SYNC_PTR_AVAIL_MIN, appl);

	return rc < 0 ? rc : state_errno_get(t->sync.s.status.state);
}

static unsigned long appl_advance_get(const struct tdm_raw_stream *s,
				      unsigned long appl, unsigned periods)
{
	return (appl + (unsigned long)periods * s->period_frames) % s->boundary;
}

/* --- the ring ------------------------------------------------------------ */

static size_t ring_offset_get(const struct tdm_raw_stream *s, unsigned ch,
			      unsigned frame)
{
	return tdm_raw_sample_offset_get(s->first_bytes[ch], s->step_bytes,
					 frame);
}

/* One channel interleaved holds the same bytes as one channel per area, so the
 * per-area copy serves it and it needs no staging buffer. */
static bool stream_direct_copy(const struct tdm_raw_stream *s)
{
	return s->layout == UDSP_TDM_NONINTERLEAVED || s->channels == 1;
}

/* The ring is a coherent DMA mapping, and on a part whose DMA does not snoop
 * the caches that mapping is uncached, so a per-sample access costs a memory
 * round trip and only a wide transfer is affordable. Both directions therefore
 * move whole areas and do the layout work in ordinary memory. */
static void ring_to_planar(const struct tdm_raw_stream *s, unsigned long appl,
			   int16_t *planar)
{
	unsigned start = (unsigned)(appl % s->buffer_frames);
	size_t bytes = (size_t)s->period_frames * sizeof(int16_t);

	if (stream_direct_copy(s)) {
		for (unsigned ch = 0; ch < s->channels; ch++)
			memcpy(planar + (size_t)ch * s->period_frames,
			       s->base + ring_offset_get(s, ch, start), bytes);
		return;
	}
	memcpy(s->scratch, s->base + ring_offset_get(s, 0, start),
	       bytes * s->channels);
	tdm_interleaved_to_planar(planar, s->scratch, s->channels,
				  s->period_frames);
}

static void planar_to_ring(const struct tdm_raw_stream *s, unsigned long appl,
			   const int16_t *planar)
{
	unsigned start = (unsigned)(appl % s->buffer_frames);
	size_t bytes = (size_t)s->period_frames * sizeof(int16_t);

	if (stream_direct_copy(s)) {
		for (unsigned ch = 0; ch < s->channels; ch++)
			memcpy(s->base + ring_offset_get(s, ch, start),
			       planar + (size_t)ch * s->period_frames, bytes);
		return;
	}
	tdm_planar_to_interleaved(s->scratch, planar, s->channels,
				  s->period_frames);
	memcpy(s->base + ring_offset_get(s, 0, start), s->scratch,
	       bytes * s->channels);
}

/* --- open ---------------------------------------------------------------- */

static int refuse(struct udsp_tdm_fail *fail, const struct tdm_raw_stream *s,
		  bool geometry, int err, const char *what)
{
	enum udsp_tdm_dir dir = UDSP_TDM_DIR_NONE;

	if (s)
		dir = s->capture ? UDSP_TDM_DIR_CAPTURE : UDSP_TDM_DIR_PLAYBACK;

	return tdm_fail_set(fail, dir, geometry, err, what);
}

static int version_check(struct udsp_tdm *t, const struct tdm_raw_stream *s,
			 struct udsp_tdm_fail *fail)
{
	unsigned int user = SNDRV_PCM_VERSION;
	int version = 0;
	int rc = ioctl_run(t, s->fd, SNDRV_PCM_IOCTL_PVERSION, &version);

	if (rc < 0)
		return refuse(fail, s, false, rc, "protocol version");
	if (SNDRV_PROTOCOL_MAJOR(version) != 2 ||
	    SNDRV_PROTOCOL_MINOR(version) != 0)
		return refuse(fail, s, false, -EINVAL, "protocol version");
	/* USER_PVERSION exists from protocol 2.0.14; an older 2.0 kernel
	 * answers -ENOTTY and still serves every other call. */
	if (version < SNDRV_PROTOCOL_VERSION(2, 0, 14))
		return 0;
	rc = ioctl_run(t, s->fd, SNDRV_PCM_IOCTL_USER_PVERSION, &user);

	return rc < 0 ? refuse(fail, s, false, rc, "protocol version") : 0;
}

static bool params_read(const struct snd_pcm_hw_params *p,
			struct tdm_raw_stream *s)
{
	unsigned access = 0;

	if (!mask_bit_first_get(p, SNDRV_PCM_HW_PARAM_ACCESS, &access))
		return false;
	if (access != (unsigned)SNDRV_PCM_ACCESS_MMAP_INTERLEAVED &&
	    access != (unsigned)SNDRV_PCM_ACCESS_MMAP_NONINTERLEAVED)
		return false;
	s->layout = access == (unsigned)SNDRV_PCM_ACCESS_MMAP_NONINTERLEAVED ?
		    UDSP_TDM_NONINTERLEAVED : UDSP_TDM_INTERLEAVED;
	s->batch = (p->info & SNDRV_PCM_INFO_BATCH) != 0;
	s->sync_applptr = (p->info & SNDRV_PCM_INFO_SYNC_APPLPTR) != 0;

	return interval_value_get(p, SNDRV_PCM_HW_PARAM_CHANNELS,
				  &s->channels) &&
	       interval_value_get(p, SNDRV_PCM_HW_PARAM_RATE, &s->rate) &&
	       interval_value_get(p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
				  &s->period_frames) &&
	       interval_value_get(p, SNDRV_PCM_HW_PARAM_PERIODS, &s->periods) &&
	       interval_value_get(p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
				  &s->buffer_frames);
}

static int params_accept(struct tdm_raw_stream *s,
			 const struct snd_pcm_hw_params *p,
			 const struct udsp_tdm_geom *want, unsigned rate,
			 struct udsp_tdm_fail *fail)
{
	if (!params_read(p, s))
		return refuse(fail, s, false, -EINVAL, "hw_params grant");
	if (s->rate != rate)
		return refuse(fail, s, true, -EINVAL, "rate");
	if (s->period_frames != want->frames_per_period)
		return refuse(fail, s, true, -EINVAL, "period");
	if (s->channels != want->channels)
		return refuse(fail, s, true, -EINVAL, "channels");
	/* The AN7581 transmit copy takes one period in one memcpy per channel
	 * and never splits it at the ring end. */
	if (s->buffer_frames % s->period_frames ||
	    s->buffer_frames / s->period_frames != s->periods)
		return refuse(fail, s, false, -EINVAL,
			      "ring not a whole number of periods");
	if (!(p->info & SNDRV_PCM_INFO_MMAP))
		return refuse(fail, s, false, -EINVAL, "no mmap access");

	return 0;
}

/* An open period count grants the largest ring, because
 * snd_pcm_hw_params_choose takes the maximum for the buffer size and the
 * period count is not in its list. The pinned count is refused by a driver
 * rule the refine did not carry. Capture starts at the larger of 16 and the
 * requested playback depth, within the refined range. A shallower capture ring
 * would overrun before playback could pace the engine. Capture trades depth
 * down to that request for an accepted count; playback searches upwards for
 * the byte alignment a driver requires. Neither bounded search proves that
 * the device refuses the rate or period. */
static int hw_params_apply(struct udsp_tdm *t, struct tdm_raw_stream *s,
			   const struct udsp_tdm_geom *want, unsigned rate,
			   struct udsp_tdm_fail *fail)
{
	struct snd_pcm_hw_params p;
	unsigned lo = 0, hi = 0, k, last;
	int step = s->capture ? -1 : 1;
	int rc;

	/* The kernel copies nothing back from a refine that failed, so a
	 * device without S16_LE or mmap access must refuse on a call of its
	 * own before the geometry is pinned; only the second refusal is the
	 * device's answer about the rate, the period and the channel count. */
	tdm_raw_params_build(&p, 0, 0, 0, 0);
	rc = ioctl_run(t, s->fd, SNDRV_PCM_IOCTL_HW_REFINE, &p);
	if (rc < 0)
		return refuse(fail, s, false, rc, "access or format");
	tdm_raw_params_build(&p, want->channels, rate, want->frames_per_period,
			     0);
	rc = ioctl_run(t, s->fd, SNDRV_PCM_IOCTL_HW_REFINE, &p);
	if (rc < 0)
		return refuse(fail, s, rc == -EINVAL, rc, "hw_refine");
	if (!interval_range_get(&p, SNDRV_PCM_HW_PARAM_PERIODS, &lo, &hi))
		return refuse(fail, s, false, -EINVAL, "period count range");

	k = want->prefill_periods < lo ? lo : want->prefill_periods;
	if (k > hi)
		k = hi;
	last = hi - k < TDM_RAW_PERIODS_TRIES ? hi : k + TDM_RAW_PERIODS_TRIES;
	if (s->capture) {
		last = k;
		k = hi < TDM_RAW_CAPTURE_PERIODS ? hi : TDM_RAW_CAPTURE_PERIODS;
		if (k < last)
			k = last;
	}
	for (;; k += step) {
		tdm_raw_params_build(&p, want->channels, rate,
				     want->frames_per_period, k);
		rc = ioctl_run(t, s->fd, SNDRV_PCM_IOCTL_HW_PARAMS, &p);
		if (!rc)
			return params_accept(s, &p, want, rate, fail);
		if (rc != -EINVAL && !(s->capture && rc == -ENOMEM))
			return refuse(fail, s, false, rc, "hw_params");
		if (k == last)
			break;
	}

	return refuse(fail, s, false, rc, "hw_params");
}

static int sw_params_apply(struct udsp_tdm *t, struct tdm_raw_stream *s,
			   struct udsp_tdm_fail *fail)
{
	struct snd_pcm_sw_params sw;
	int rc;

	/* A capture pointer that counts samples stands short of the boundary at
	 * the interrupt, so a threshold of one whole period misses that wake
	 * and the reader takes two periods at the next one. */
	s->avail_min = s->period_frames;
	if (s->capture && !s->batch)
		s->avail_min -= s->period_frames / TDM_RAW_WAKE_MARGIN;

	memset(&sw, 0, sizeof(sw));
	sw.proto = SNDRV_PCM_VERSION;
	sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
	sw.tstamp_type = SNDRV_PCM_TSTAMP_TYPE_MONOTONIC;
	sw.period_step = 1;
	sw.avail_min = s->avail_min;
	/* No kernel start exists on this path; the engine starts the linked
	 * pair itself, and the wrap point states that intent. */
	sw.start_threshold = tdm_raw_boundary_get(s->buffer_frames);
	sw.stop_threshold = sw.start_threshold;
	if (!s->capture && !s->batch)
		sw.silence_size = sw.start_threshold;
	rc = ioctl_run(t, s->fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw);
	if (rc < 0)
		return refuse(fail, s, false, rc, "sw_params");
	/* A wrap point that is not a whole number of rings would move the ring
	 * offset of a pointer at every wrap. */
	if (!sw.boundary || sw.boundary % s->buffer_frames)
		return refuse(fail, s, false, -EINVAL, "boundary");
	s->boundary = sw.boundary;

	return 0;
}

static int area_accept(struct tdm_raw_stream *s, unsigned ch,
		       const struct snd_pcm_channel_info *ci,
		       size_t buffer_bytes, struct udsp_tdm_fail *fail)
{
	size_t first = (size_t)ci->first / 8u;
	size_t step = (size_t)ci->step / 8u;
	size_t last;
	bool ok = ci->offset == 0 && !(ci->first % 8u) && !(ci->step % 8u) &&
		  ci->step && !(first % sizeof(int16_t)) &&
		  !(step % sizeof(int16_t)) && (!ch || step == s->step_bytes);

	if (!ok)
		return refuse(fail, s, false, -EINVAL, "channel area");
	last = first + (size_t)(s->buffer_frames - 1) * step + sizeof(int16_t);
	if (last > buffer_bytes)
		return refuse(fail, s, false, -EINVAL, "channel area");
	s->first_bytes[ch] = (unsigned)first;
	s->step_bytes = (unsigned)step;

	return 0;
}

static size_t ring_bytes_get(const struct tdm_raw_stream *s)
{
	return (size_t)s->buffer_frames * s->channels * sizeof(int16_t);
}

/* One channel area per channel, each holding its own run of samples. */
static bool area_shape_direct(const struct tdm_raw_stream *s)
{
	size_t sample = sizeof(int16_t);

	if (s->step_bytes != sample)
		return false;
	for (unsigned ch = 0; ch < s->channels; ch++) {
		if (s->first_bytes[ch] != ch * s->buffer_frames * sample)
			return false;
	}

	return true;
}

/* One block whose channels sit one sample apart. */
static bool area_shape_block(const struct tdm_raw_stream *s)
{
	size_t sample = sizeof(int16_t);

	if (s->step_bytes != s->channels * sample)
		return false;
	for (unsigned ch = 0; ch < s->channels; ch++) {
		if (s->first_bytes[ch] != ch * sample)
			return false;
	}

	return true;
}

/* The copy serves two shapes, so anything else is refused here rather than
 * assumed on the tick. */
static int areas_shape_check(const struct tdm_raw_stream *s,
			     struct udsp_tdm_fail *fail)
{
	bool ok = stream_direct_copy(s) ? area_shape_direct(s) :
					  area_shape_block(s);

	return ok ? 0 : refuse(fail, s, false, -EINVAL, "channel area shape");
}

static int scratch_alloc(struct tdm_raw_stream *s, struct udsp_tdm_fail *fail)
{
	if (stream_direct_copy(s))
		return 0;
	s->scratch = calloc((size_t)s->channels * s->period_frames,
			    sizeof(*s->scratch));

	return s->scratch ? 0 : refuse(fail, s, false, -ENOMEM, "allocation");
}

static int areas_read(struct udsp_tdm *t, struct tdm_raw_stream *s,
		      struct udsp_tdm_fail *fail)
{
	size_t buffer_bytes = ring_bytes_get(s);

	for (unsigned ch = 0; ch < s->channels; ch++) {
		struct snd_pcm_channel_info ci;
		int rc;

		memset(&ci, 0, sizeof(ci));
		ci.channel = ch;
		rc = ioctl_run(t, s->fd, SNDRV_PCM_IOCTL_CHANNEL_INFO, &ci);
		if (rc < 0)
			return refuse(fail, s, false, rc, "channel_info");
		rc = area_accept(s, ch, &ci, buffer_bytes, fail);
		if (rc < 0)
			return rc;
	}

	return areas_shape_check(s, fail);
}

static int ring_map(struct udsp_tdm *t, struct tdm_raw_stream *s,
		    struct udsp_tdm_fail *fail)
{
	size_t bytes = ring_bytes_get(s);
	int prot = s->capture ? PROT_READ : PROT_READ | PROT_WRITE;
	void *p = t->sys->mmap(NULL, bytes, prot, MAP_SHARED, s->fd, 0);

	if (p == MAP_FAILED)
		return refuse(fail, s, false, -errno, "mmap");
	s->base = p;
	s->map_bytes = bytes;

	return 0;
}

static int stream_open(struct udsp_tdm *t, struct tdm_raw_stream *s,
		       const char *dev, bool capture,
		       const struct udsp_tdm_geom *want, unsigned rate,
		       struct udsp_tdm_fail *fail)
{
	char path[TDM_RAW_PATH_MAX];
	int rc;

	s->capture = capture;
	if (!tdm_raw_path_build(dev, capture, path, sizeof(path)))
		return refuse(fail, s, false, -EINVAL, "device name");
	s->fd = t->sys->open(path, O_RDWR | O_CLOEXEC);
	if (s->fd < 0)
		return refuse(fail, s, false, -errno, "open");

	rc = version_check(t, s, fail);
	if (rc < 0)
		return rc;
	rc = hw_params_apply(t, s, want, rate, fail);
	if (rc < 0)
		return rc;
	rc = sw_params_apply(t, s, fail);
	if (rc < 0)
		return rc;
	rc = areas_read(t, s, fail);
	if (rc < 0)
		return rc;
	rc = scratch_alloc(s, fail);
	if (rc < 0)
		return rc;

	return ring_map(t, s, fail);
}

static int pair_prepare(struct udsp_tdm *t, struct udsp_tdm_fail *fail)
{
	int rc = ioctl_run(t, t->capture.fd, SNDRV_PCM_IOCTL_PREPARE, NULL);

	if (rc < 0)
		return refuse(fail, &t->capture, false, rc, "prepare");
	/* The prepare resets the hardware pointer and sets the application
	 * pointer to it, so both cached pairs are stale until they are read
	 * again. */
	rc = sync_ptr_prime(t, &t->play);
	if (rc < 0)
		return refuse(fail, &t->play, false, rc, "sync_ptr");
	rc = sync_ptr_prime(t, &t->capture);
	if (rc < 0)
		return refuse(fail, &t->capture, false, rc, "sync_ptr");
	t->capture_stale = false;
	t->playback_refresh = false;

	return 0;
}

/*
 * The drivers known to stop the playback DMA when no period is queued and to
 * restart it at the write that queues the next one (tdm.h tx_restarts). Two
 * info flags alone do not state that contract: BATCH says the pointer moves in
 * period steps, and SYNC_APPLPTR says the driver acts on each application
 * pointer move. A cyclic DMA could report both. So the back end also asks for
 * the platform driver by name, which the kernel lists under
 * /sys/bus/platform/drivers once it is registered. The AN7581 PCM driver
 * (an7581-pcm-tdm.c) arms one transmit descriptor per period it is given and
 * kicks the DMA at each arm (an7581_pcm_tx_fill). Restriction: the check
 * names a registered driver, not the card of the opened node, so a board with
 * a second sound card that reports both flags would be misread; no board here
 * has one.
 */
static const char *const tdm_raw_restart_drivers[] = {
	"an7581-pcm-tdm",
};

static bool tdm_raw_driver_known(const struct udsp_tdm *t)
{
	char path[96];

	for (size_t i = 0; i < sizeof(tdm_raw_restart_drivers) /
			       sizeof(tdm_raw_restart_drivers[0]); i++) {
		int fd;

		snprintf(path, sizeof(path), "/sys/bus/platform/drivers/%s",
			 tdm_raw_restart_drivers[i]);
		fd = t->sys->open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (fd < 0)
			continue;
		t->sys->close(fd);
		return true;
	}

	return false;
}

static void geom_fill(struct udsp_tdm *t)
{
	t->geom.frames_per_period = t->capture.period_frames;
	t->geom.channels = t->capture.channels;
	/* The prefill fills the playback ring, so the playback depth is the
	 * bound the engine clamps to. */
	t->geom.prefill_periods = t->play.periods;
	t->geom.capture_periods = t->capture.periods;
	t->geom.layout = t->capture.layout;
	t->geom.paced = true;
	t->geom.rate = t->capture.rate;
	t->geom.tx_restarts = t->play.batch && t->play.sync_applptr &&
			      tdm_raw_driver_known(t);
}

static int pair_open(struct udsp_tdm *t, const char *play_dev,
		     const char *capture_dev, const struct udsp_tdm_geom *want,
		     unsigned rate, struct udsp_tdm_fail *fail)
{
	int rc = stream_open(t, &t->play, play_dev, false, want, rate, fail);

	if (rc < 0)
		return rc;
	rc = stream_open(t, &t->capture, capture_dev, true, want, rate, fail);
	if (rc < 0)
		return rc;

	/* AN7581 shares DCHENR, CHBFOSR and the slot map between both DMA
	 * rings, so the two directions must agree on the channel count, the
	 * period and the rate. */
	if (t->play.channels != t->capture.channels ||
	    t->play.period_frames != t->capture.period_frames ||
	    t->play.rate != t->capture.rate)
		return refuse(fail, NULL, true, -EINVAL, "pair geometry");

	/* The link argument carries the file descriptor by value, and its
	 * stream triggers first. */
	rc = ioctl_run(t, t->capture.fd, SNDRV_PCM_IOCTL_LINK,
		       (void *)(uintptr_t)t->play.fd);
	if (rc < 0)
		return refuse(fail, NULL, false, rc, "link");
	rc = pair_prepare(t, fail);
	if (rc < 0)
		return rc;
	geom_fill(t);

	return 0;
}

static void stream_close(struct udsp_tdm *t, struct tdm_raw_stream *s)
{
	if (s->base)
		t->sys->munmap(s->base, s->map_bytes);
	if (s->fd >= 0)
		t->sys->close(s->fd);
	free(s->scratch);
	s->scratch = NULL;
	s->base = NULL;
	s->fd = -1;
}

static void tdm_raw_close(struct udsp_tdm *t)
{
	if (!t)
		return;
	if (t->capture.fd >= 0)
		(void)t->sys->ioctl(t->capture.fd, SNDRV_PCM_IOCTL_UNLINK,
				    NULL);
	stream_close(t, &t->capture);
	stream_close(t, &t->play);
	free(t);
}

static struct udsp_tdm *tdm_raw_open(const char *play_dev,
				     const char *capture_dev,
				     const struct udsp_tdm_geom *want,
				     struct udsp_tdm_geom *got,
				     struct udsp_tdm_fail *fail)
{
	struct udsp_tdm *t;
	unsigned rate;

	if (!play_dev || !capture_dev || !want || !got) {
		refuse(fail, NULL, false, -EINVAL, "arguments");
		return NULL;
	}
	if (want->channels < 1 || want->channels > TDM_MAX_CHANNELS ||
	    want->frames_per_period == 0) {
		refuse(fail, NULL, false, -EINVAL, "geometry request");
		return NULL;
	}

	t = calloc(1, sizeof(*t));
	if (!t) {
		refuse(fail, NULL, false, -ENOMEM, "allocation");
		return NULL;
	}
	t->sys = tdm_raw_sys;
	t->play.fd = -1;
	t->capture.fd = -1;
	/* The requested line rate, with the zero of tdm.h normalised. */
	rate = want->rate ? want->rate : 8000u;

	if (pair_open(t, play_dev, capture_dev, want, rate, fail) < 0) {
		tdm_raw_close(t);
		return NULL;
	}
	*got = t->geom;

	return t;
}

/* --- the tick ------------------------------------------------------------ */

static int wait_run(struct udsp_tdm *t, int fd, short events)
{
	struct pollfd pfd = { .fd = fd, .events = events, .revents = 0 };
	struct timespec tmo = {
		.tv_sec = 0,
		.tv_nsec = TDM_RAW_WAIT_MS * 1000000L,
	};
	int rc = t->sys->ppoll(&pfd, 1, &tmo, NULL);

	if (rc < 0)
		return -errno;
	if (!rc)
		return TDM_RAW_WAIT_TIMEOUT;
	/* An xrun raises the ready bit together with POLLERR, so the error bits
	 * decide first (sound/core/pcm_native.c snd_pcm_poll). */
	if (pfd.revents & (POLLERR | POLLNVAL | POLLHUP))
		return TDM_RAW_WAIT_ERROR;
	if (pfd.revents & events)
		return TDM_RAW_WAIT_READY;

	return TDM_RAW_WAIT_TIMEOUT;
}

static int fault_errno_get(struct udsp_tdm *t, struct tdm_raw_stream *s)
{
	int rc = sync_ptr_read(t, s);

	return rc < 0 ? rc : -EIO;
}

static unsigned long capture_avail_get(const struct tdm_raw_stream *s)
{
	return tdm_raw_capture_avail_get(s->hw_ptr, s->appl_ptr, s->boundary);
}

/* A sleep that ends early costs the caller one more round of its own bounded
 * loop, so the result decides nothing here. */
static void frames_sleep(struct udsp_tdm *t, const struct tdm_raw_stream *s,
			 unsigned long frames)
{
	uint64_t ns = (uint64_t)frames * 1000000000u / s->rate +
		      TDM_RAW_SLEEP_MARGIN_NS;
	struct timespec ts = {
		.tv_sec = (time_t)(ns / 1000000000u),
		.tv_nsec = (long)(ns % 1000000000u),
	};

	(void)t->sys->clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
}

/* The wake of a device that counts samples carries part of a period. The read
 * sleeps for the missing frames. It does not wait for the next interrupt, which
 * would give it two periods at once. */
static int period_complete_wait(struct udsp_tdm *t, struct tdm_raw_stream *s)
{
	for (unsigned n = 0; n < TDM_RAW_COMPLETE_ROUNDS; n++) {
		unsigned long avail;
		int rc = sync_ptr_read(t, s);

		if (rc < 0)
			return rc;
		avail = capture_avail_get(s);
		if (avail >= s->period_frames)
			return 0;
		if (n + 1 < TDM_RAW_COMPLETE_ROUNDS)
			frames_sleep(t, s, s->period_frames - avail);
	}

	return -EAGAIN;
}

static int tdm_raw_read(struct udsp_tdm *t, int16_t *buf)
{
	struct tdm_raw_stream *s = &t->capture;
	unsigned long appl_pre, avail_pre, safe;
	unsigned long dropped = 0;
	int rc = wait_run(t, s->fd, POLLIN);

	if (rc < 0)
		return rc;
	if (rc == TDM_RAW_WAIT_TIMEOUT)
		return -EAGAIN;
	if (rc == TDM_RAW_WAIT_ERROR)
		return fault_errno_get(t, s);
	if (!s->batch) {
		rc = period_complete_wait(t, s);
		if (rc < 0)
			return rc;
	}

	appl_pre = s->appl_ptr;
	ring_to_planar(s, appl_pre, buf);
	rc = sync_ptr_push(t, s, appl_advance_get(s, appl_pre, 1));
	if (rc < 0)
		return rc;
	avail_pre = tdm_raw_capture_avail_get(s->hw_ptr, appl_pre,
					      s->boundary);
	/* The AN7581 write head can lead its reported pointer by one period.
	 * Leave that period and the period under copy outside the safe span. */
	safe = s->buffer_frames - 2ul * s->period_frames;
	/* A two-period ring cannot reserve the write-head lead and the copy. */
	if (s->buffer_frames >= 3ul * s->period_frames && avail_pre > safe) {
		unsigned long appl;

		dropped = (avail_pre - safe + s->period_frames - 1) /
			  s->period_frames;
		appl = appl_advance_get(s, appl_pre, (unsigned)dropped);
		ring_to_planar(s, appl, buf);
		rc = sync_ptr_push(t, s, appl_advance_get(s, appl, 1));
		if (rc < 0)
			return rc;
	}
	t->capture_stale = false;

	return (int)dropped;
}

static unsigned long playback_avail_get(const struct tdm_raw_stream *s)
{
	return tdm_raw_playback_avail_get(s->hw_ptr, s->appl_ptr,
					  s->buffer_frames, s->boundary);
}

/* A sample-accurate pointer can leave cached room short by a fraction of a
 * period, so only a fresh pair can justify a wait. The caller can supply that
 * pair to avoid a duplicate read. The state gate keeps an xrun distinct from
 * the transient -EAGAIN. */
static int write_room_wait(struct udsp_tdm *t, struct tdm_raw_stream *s,
			   bool pair_fresh)
{
	int rc;

	if (!pair_fresh) {
		rc = sync_ptr_read(t, s);
		if (rc < 0)
			return rc;
	}
	if (playback_avail_get(s) >= s->period_frames)
		return 0;

	rc = wait_run(t, s->fd, POLLOUT);
	if (rc < 0)
		return rc;
	if (rc == TDM_RAW_WAIT_ERROR)
		return fault_errno_get(t, s);
	rc = sync_ptr_read(t, s);
	if (rc < 0)
		return rc;

	return playback_avail_get(s) < s->period_frames ? -EAGAIN : 0;
}

static int tdm_raw_write(struct udsp_tdm *t, const int16_t *buf)
{
	struct tdm_raw_stream *s = &t->play;
	unsigned long appl, expired = 0;
	bool pair_fresh;
	long queued;
	int rc;

	t->capture_stale = true;
	queued = tdm_raw_playback_queued_get(s->hw_ptr, s->appl_ptr,
					     s->buffer_frames, s->boundary);
	pair_fresh = queued < (long)s->period_frames || t->playback_refresh;
	if (pair_fresh) {
		rc = sync_ptr_read(t, s);
		if (rc < 0)
			return rc;
	}
	if (playback_avail_get(s) < s->period_frames) {
		rc = write_room_wait(t, s, pair_fresh);
		if (rc < 0)
			return rc;
	}
	queued = tdm_raw_playback_queued_get(s->hw_ptr, s->appl_ptr,
					     s->buffer_frames, s->boundary);
	appl = s->appl_ptr;
	if (queued < 0) {
		unsigned long next = s->hw_ptr + s->period_frames - 1;

		next -= next % s->period_frames;
		next %= s->boundary;
		expired = tdm_raw_capture_avail_get(next, s->appl_ptr,
						    s->boundary) /
			  s->period_frames;
		appl = next;
	}
	planar_to_ring(s, appl, buf);

	/* Every write moves the application pointer by one period, so every
	 * write reaches the AN7581 .ack; a repeated pointer is not a kick. */
	rc = sync_ptr_push(t, s, appl_advance_get(s, appl, 1));
	if (rc < 0)
		return rc;
	t->playback_refresh = false;

	return (int)expired;
}

static long tdm_raw_delay(struct udsp_tdm *t)
{
	const struct tdm_raw_stream *s = &t->play;
	long queued = tdm_raw_playback_queued_get(s->hw_ptr, s->appl_ptr,
						  s->buffer_frames,
						  s->boundary);

	/* Cyclic DMA can pass appl without a stop. The clamp hides that
	 * distance, so the engine must decide on fresh playback room. */
	return queued < 0 ? 0 : queued;
}

static long tdm_raw_avail(struct udsp_tdm *t)
{
	struct tdm_raw_stream *s = &t->capture;

	/* Beside the delay the pair must share one instant, so a write since
	 * the last capture read costs one refresh. Right after the read the
	 * cached pair is exact and no call is made. */
	if (t->capture_stale) {
		int rc = sync_ptr_read(t, s);

		if (rc < 0)
			return rc;
		t->capture_stale = false;
	}

	return (long)capture_avail_get(s);
}

static long tdm_raw_capture_avail_fresh(struct udsp_tdm *t)
{
	int rc = sync_ptr_read(t, &t->capture);

	if (rc < 0)
		return rc;
	t->capture_stale = false;

	return (long)capture_avail_get(&t->capture);
}

static long tdm_raw_playback_room_fresh(struct udsp_tdm *t)
{
	int rc;

	t->playback_refresh = true;
	rc = sync_ptr_read(t, &t->play);

	return rc < 0 ? rc : (long)playback_avail_get(&t->play);
}

static int tdm_raw_capture_skip(struct udsp_tdm *t, unsigned periods)
{
	struct tdm_raw_stream *s = &t->capture;
	unsigned long appl = appl_advance_get(s, s->appl_ptr, periods);
	int rc = sync_ptr_push(t, s, appl);

	if (rc < 0)
		return rc;
	t->capture_stale = false;

	return 0;
}

static int tdm_raw_start(struct udsp_tdm *t)
{
	return ioctl_run(t, t->capture.fd, SNDRV_PCM_IOCTL_START, NULL);
}

static int tdm_raw_stop(struct udsp_tdm *t)
{
	return ioctl_run(t, t->capture.fd, SNDRV_PCM_IOCTL_DROP, NULL);
}

/* The drop runs in every state, where tdm/alsa.c drops only a running group:
 * the AN7581 capture prepare skips its framing resync while the peer runs,
 * and the drop leaves both directions idle whatever path led here. */
static int tdm_raw_recover(struct udsp_tdm *t)
{
	int rc = ioctl_run(t, t->capture.fd, SNDRV_PCM_IOCTL_DROP, NULL);

	if (rc < 0)
		return rc;

	return pair_prepare(t, NULL);
}

const struct udsp_tdm_ops udsp_tdm_alsa_raw = {
	.name = "alsa-raw",
	.open = tdm_raw_open,
	.close = tdm_raw_close,
	.recover = tdm_raw_recover,
	.start = tdm_raw_start,
	.stop = tdm_raw_stop,
	.read_period = tdm_raw_read,
	.write_period = tdm_raw_write,
	.delay = tdm_raw_delay,
	.avail = tdm_raw_avail,
	.capture_avail_fresh = tdm_raw_capture_avail_fresh,
	.playback_room_fresh = tdm_raw_playback_room_fresh,
	.capture_skip = tdm_raw_capture_skip,
};
