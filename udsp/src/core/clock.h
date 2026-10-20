// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The engine tick is the master clock of libudsp: every line machine call and
 * every event stamp uses now_ms = tick * 10. Two clocks live here because the
 * engine object is recreated on every udsp_start: the engine clock counts
 * that engine's own ticks, and the run clock carries a base so the stamp a
 * consumer sees never goes backwards across a stop and start.
 *
 * Publication is one release store per tick from the engine thread and a pair
 * of relaxed loads from the slic thread, so the RT tick takes no lock. A
 * reader can pair a tick with the microsecond stamp of the tick after it; the
 * sub-tick offset is therefore clamped into the tick it belongs to.
 */
#ifndef UDSP_CORE_CLOCK_H
#define UDSP_CORE_CLOCK_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "engine.h"

#define UDSP_TICK_MS	10u

/* core/engine.c, the engine's own clock. */
uint32_t engine_now_ms(const struct engine *e);
void engine_clock_get(const struct engine *e, uint64_t *tick, uint64_t *mono_us);

struct udsp_clock {
	_Atomic uint_least64_t tick;
	_Atomic uint_least64_t mono_us;
	uint64_t base;		/* ticks of every engine that already ran */
	_Atomic bool paced;
};

static inline uint64_t udsp_mono_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static inline uint64_t udsp_mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void udsp_clock_publish(struct udsp_clock *c, uint64_t engine_tick,
				      uint64_t mono_us)
{
	atomic_store_explicit(&c->mono_us, mono_us, memory_order_relaxed);
	atomic_store_explicit(&c->tick, c->base + engine_tick,
			      memory_order_release);
}

static inline uint64_t udsp_clock_tick(const struct udsp_clock *c)
{
	return atomic_load_explicit(&c->tick, memory_order_acquire);
}

static inline uint32_t udsp_clock_now_ms(const struct udsp_clock *c)
{
	return (uint32_t)(udsp_clock_tick(c) * UDSP_TICK_MS);
}

/*
 * The stamp the slic thread puts on a hook edge. On an unpaced backend the
 * published microseconds belong to a virtual clock, so the tick boundary is
 * the only honest answer; on a paced backend the offset inside the tick is
 * real and is clamped into [tick * 10, tick * 10 + 9].
 */
static inline uint32_t udsp_clock_stamp_ms(const struct udsp_clock *c)
{
	uint64_t tick = udsp_clock_tick(c);
	uint32_t base = (uint32_t)(tick * UDSP_TICK_MS);
	uint64_t at, now;

	if (!atomic_load_explicit(&c->paced, memory_order_relaxed))
		return base;

	at = atomic_load_explicit(&c->mono_us, memory_order_relaxed);
	now = udsp_mono_us();
	if (now <= at)
		return base;

	now = (now - at) / 1000u;

	return base + (uint32_t)(now > UDSP_TICK_MS - 1 ? UDSP_TICK_MS - 1 : now);
}

#endif
