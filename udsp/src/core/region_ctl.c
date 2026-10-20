// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Region slots and the region_load job. Two struct udsp_region live in the
 * handle with an atomic active index: the engine reads the active slot only,
 * the parser fills the spare one, and the swap is one index flip at a tick
 * boundary, so a failed load never leaves a half-applied region behind
 * (line.h parse-all-then-swap).
 *
 * line_region_load carries a 16 KiB text buffer on its stack, which is why the
 * parse runs on the slic thread inside the SLOW job and never on the RT tick.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "udsp_priv.h"

#define REGION_VIEW_PUBLISH_NAPS	50u

_Static_assert(UDSP_REGION_RING_VIEW_MAX == UDSP_REGION_RING_MAX,
	       "the view carries every grammar ring template");
_Static_assert(UDSP_REGION_DESCRIBE_MAX == UDSP_REGION_NAME_MAX,
	       "the view carries the whole describe string");
_Static_assert(UDSP_REGION_CW_VIEW_MAX == UDSP_CW_VARIANT_MAX,
	       "the view carries every grammar cw variant");

#ifndef UDSP_REGION_DIR
/* The install path; src/CMakeLists.txt sets the definition per target. */
#define UDSP_REGION_DIR	"/usr/share/udsp/regions"
#endif

int udsp_region_path_build(const char *iso3, char *out, size_t cap)
{
	const char *dir;
	int n;

	if (!iso3 || !*iso3 || !out)
		return -EINVAL;
	/* An argument that carries a separator is a file path, not an ISO3. */
	if (strchr(iso3, '/')) {
		n = snprintf(out, cap, "%s", iso3);
		return (n < 0 || (size_t)n >= cap) ? -ENAMETOOLONG : 0;
	}
	if (strlen(iso3) >= UDSP_ISO3_MAX)
		return -EINVAL;

	dir = getenv("UDSP_REGION_DIR");
	if (!dir || !*dir)
		dir = UDSP_REGION_DIR;
	n = snprintf(out, cap, "%s/%s.conf", dir, iso3);

	return (n < 0 || (size_t)n >= cap) ? -ENAMETOOLONG : 0;
}

/* The buffer has the width of the job path, so a name that udsp_region_load
 * refuses for its length fails here too. */
int udsp_region_probe(const char *iso3)
{
	char path[SLIC_JOB_PATH_MAX];
	struct stat st;
	int rc = udsp_region_path_build(iso3, path, sizeof(path));

	if (rc)
		return rc;
	if (stat(path, &st))
		return -errno;

	return 0;
}

/* The parser's own words when it has any (line_region_load clears err on
 * entry and fills it for a rejected file), else the errno of the file. Control
 * thread at open, slic thread on a region job. */
static void region_load_fail_log(struct udsp *s, const char *path, int rc,
				 const struct line_region_error *err)
{
	if (err->text[0]) {
		udsp_log(s, UDSP_LOG_ERR, "region file %s: line %u: %s", path,
			 err->line, err->text);
		return;
	}
	udsp_log(s, UDSP_LOG_ERR, "region file %s: %s", path, strerror(-rc));
}

/* A call-waiting total pairs with line_region_cw_lead_ms(), which knows no
 * lead for a segmented tone, so a segmented tone bounds nothing here. */
static uint32_t region_cw_total_ms(const struct udsp_region_tone *t)
{
	return t->segmented ? 0 : line_region_tone_total_ms(t);
}

static void region_view_fill(struct udsp_region_view *dst,
			     const struct udsp_region *src)
{
	unsigned i, k;
	uint32_t base_lead;

	memset(dst, 0, sizeof(*dst));
	snprintf(dst->iso3, sizeof(dst->iso3), "%s", src->iso3);
	dst->cid_format = src->cid.format;
	dst->mwi_format = src->cid.mwi_format;
	dst->ring_count = src->ring_count;
	dst->hook_flash_min_ms = src->hook.flash_min_ms;
	dst->hook_flash_max_ms = src->hook.flash_max_ms;
	dst->hook_release_min_ms = src->hook.release_min_ms;
	dst->cw_sas_total_ms =
		region_cw_total_ms(&src->tone[UDSP_TONE_CALL_WAITING]);
	dst->flash_service = src->flash_service;
	snprintf(dst->describe, sizeof(dst->describe), "%s", src->describe);
	dst->pulse_invert = src->hook.pulse_invert;
	dst->confirm_total_ms =
		line_region_tone_total_ms(&src->tone[UDSP_TONE_CONFIRM]);
	for (i = 0; i < src->ring_count; i++) {
		const struct udsp_region_ring *ring = &src->ring[i];

		for (k = 0; k < ring->cadence_count; k++)
			dst->ring[i].cadence[k] = ring->cadence[k];
		dst->ring[i].cadence_count = ring->cadence_count;
	}
	/* The base tone is not bounded by cw_check, so its presentation could
	 * exceed the uint16 field; clamp it. The variants are bounded to the X
	 * window, so they never reach the clamp. */
	base_lead = line_region_cw_lead_ms(&src->tone[UDSP_TONE_CALL_WAITING]);
	dst->cw_count = src->cw_count;
	dst->cw[0].present = src->tone[UDSP_TONE_CALL_WAITING].present;
	dst->cw[0].total_ms = dst->cw_sas_total_ms;
	dst->cw[0].lead_ms = base_lead > UINT16_MAX ? UINT16_MAX : (uint16_t)base_lead;
	for (i = 0; i < src->cw_count; i++) {
		dst->cw[i + 1].present = src->cw[i].present;
		dst->cw[i + 1].total_ms = region_cw_total_ms(&src->cw[i]);
		dst->cw[i + 1].lead_ms =
			(uint16_t)line_region_cw_lead_ms(&src->cw[i]);
	}
}

/* Control thread at open (no reader exists yet) or slic thread after a swap.
 * The active slot is stable under this call: the next region job runs on this
 * same thread and only its parse retargets a slot. snap.h skips a publish
 * while a reader holds the slot, so a bounded retry replaces it, the
 * slic_monitor_publish shape; a publish that never succeeds keeps the previous
 * view and counts an op error. */
static void region_view_publish(struct udsp *s)
{
	const struct udsp_region *r = udsp_region_active(s);
	struct udsp_region_view view;
	unsigned naps = REGION_VIEW_PUBLISH_NAPS;

	if (!r)
		return;
	region_view_fill(&view, r);
	while (!region_view_snap_publish(&s->region_view, &view)) {
		struct timespec nap = { .tv_sec = 0, .tv_nsec = 1000000L };

		if (!naps--) {
			udsp_op_error(s);
			return;
		}
		if (atomic_load_explicit(&s->slic_quit, memory_order_acquire))
			return;
		nanosleep(&nap, NULL);
	}
}

enum udsp_status udsp_region_get(udsp_t *s, struct udsp_region_view *out)
{
	struct udsp_region_view view;

	if (!s || !out)
		return UDSP_UNSUPPORTED;
	region_view_snap_get(&s->region_view, &view);
	if (!view.iso3[0])
		return UDSP_UNSUPPORTED;
	*out = view;
	return UDSP_OK;
}

int udsp_region_open_parse(struct udsp *s, const char *iso3)
{
	struct line_region_error err;
	char path[SLIC_JOB_PATH_MAX];
	int rc = udsp_region_path_build(iso3, path, sizeof(path));

	if (rc) {
		udsp_log(s, UDSP_LOG_ERR, "region %s: no file name (%d)", iso3, rc);
		return rc;
	}

	rc = line_region_load(path, &s->region[0], &err);
	if (rc) {
		region_load_fail_log(s, path, rc, &err);
		return rc;
	}

	atomic_store_explicit(&s->region_idx, 0u, memory_order_release);
	atomic_store_explicit(&s->region_valid, true, memory_order_release);
	region_view_publish(s);

	return 0;
}

/* Engine thread, at a tick boundary. The timing windows, the pulse
 * classification and the decadic coding move; the three windows the core set
 * through udsp_line_hook_timing survive the swap. */
void udsp_region_swap_apply(struct udsp *s)
{
	unsigned next = (atomic_load_explicit(&s->region_idx,
					      memory_order_acquire) ^ 1u) & 1u;
	const struct udsp_region *r = &s->region[next];

	atomic_store_explicit(&s->region_idx, next, memory_order_release);
	atomic_store_explicit(&s->region_valid, true, memory_order_release);

	for (unsigned i = 0; i < s->cap.lines; i++) {
		struct udsp_chan *c = &s->chan[i];
		uint16_t flash_min = c->hook.timing.flash_min_ms;
		uint16_t flash_max = c->hook.timing.flash_max_ms;
		uint16_t release_min = c->hook.timing.release_min_ms;

		c->hook.timing = r->hook;
		if (c->core_windows)
			line_hook_timing_set(&c->hook, flash_min, flash_max,
					     release_min);
		chan_hook_pulse_apply(c, r);
	}
}

/* Slic thread, inside the SLOW job. */
int udsp_region_job_run(struct udsp *s, const char *path)
{
	struct line_region_error err;
	unsigned spare;
	int32_t status = 0;
	int rc;

	spare = (atomic_load_explicit(&s->region_idx, memory_order_acquire) ^ 1u) & 1u;
	rc = line_region_load(path, &s->region[spare], &err);
	if (rc) {
		region_load_fail_log(s, path, rc, &err);
		return rc;
	}

	/* A parked tick (a deferred engine before its start) has no acker for a
	 * REGION_SWAP, and it does not run beside this thread, so the swap is
	 * applied here. A live tick, even one before started, is the only other
	 * writer of the hook state the swap touches, so it must ack the swap. */
	if (atomic_load_explicit(&s->tick_parked, memory_order_acquire)) {
		udsp_region_swap_apply(s);
		region_view_publish(s);
		return 0;
	}

	rc = slic_up_post(s, SLIC_UP_REGION_SWAP, 0, SLIC_ACK_WAIT_MS, &status);
	if (!rc && !status)
		region_view_publish(s);

	return rc ? rc : status;
}
