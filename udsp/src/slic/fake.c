// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * In-tree fake SLIC. It implements the whole struct udsp_slic_ops of the stable
 * contract include/udsp/slic.h as an in-memory simulation, so the engine,
 * udsp-cli and the uvoice hal/udsp backend run on a host with no SLIC and no
 * SPI bus. It is linked into libudsp, not dlopened, and so exports
 * udsp_slic_fake_descriptor; the plug-in symbol udsp_slic_descriptor stays
 * reserved for the proprietary plug-in.
 *
 * The fake follows the uvoice stub HAL backend: it records what the caller
 * asked for and succeeds. It checks ranges, because an out-of-range channel
 * would write past a per-channel record, and it checks the arguments the
 * contract enumerates, but it enforces no ordering of its own: the tests, not
 * the fake, say whether the engine drives the line correctly.
 *
 * Error convention. slic.h states none, so the ops return 0 on success and a
 * negative errno on failure, as tdm.h fixes for the tdm plug-ins.
 *
 * Bus. The one bus primitive is spi_xfer: the plug-in owns all wire framing, so
 * the frame below is the fake's own, and the transitional reg_read, reg_write
 * and burst hooks are never touched here.
 *
 * The fake needs no bus. It accepts a NULL host, because CI and the udsp-cli
 * selftest run where no spidev exists. A supplied host must carry the
 * primitive: attach issues the REG0 probe of the attach contract as one framed
 * transfer, so a real host implementation is exercised. The attach fails when
 * the transfer fails, and it fails when the host carries no spi_xfer at all. A
 * zeroed host struct is a caller defect, not a request to simulate. The channel
 * count never comes from the probe: no part answers behind the fake, so it
 * comes from the test-settable default.
 */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "udsp/event.h"
#include "udsp/slic.h"
#include "fake.h"

/* An invented wire frame and register map. The fake has no part behind it, and
 * a real ProSLIC number here would suggest knowledge libudsp must not hold
 * (slic.h). A frame is command, address, data; the command byte
 * carries the channel in its low bits and the read flag in bit 7. */
#define FAKE_FRAME_LEN		3
#define FAKE_FRAME_READ		0x80
#define FAKE_CMD_WRITE(chan)	((uint8_t)(chan))
#define FAKE_CMD_READ(chan)	((uint8_t)(FAKE_FRAME_READ | (chan)))
#define FAKE_REG_PART		0x00	/* REG0 of the attach contract */
#define FAKE_REG_LINEFEED	0x01
#define FAKE_REG_RING		0x02
#define FAKE_SAFE_STRIDE	(2 * FAKE_FRAME_LEN)	/* per channel */

/* The fake's own metallic line test record: an eight byte head with the test
 * id at byte 1 and the status byte clear, then the call counter. It carries
 * none of the plug-in's record layout, because libudsp never decodes these
 * bytes and a test only has to see them cross the boundary unchanged. */
#define FAKE_MLT_HEAD_LEN	8
#define FAKE_MLT_HEAD_TEST	1
#define FAKE_MLT_HEAD_STATUS	2
#define FAKE_MLT_RESULT_LEN	(FAKE_MLT_HEAD_LEN + 4)

_Static_assert(UDSP_SLIC_FAKE_CHAN_MAX * FAKE_SAFE_STRIDE <= UDSP_SLIC_SAFE_MAX,
	       "the fake safe transfer outgrew UDSP_SLIC_SAFE_MAX");

/*
 * The three injection cells are atomic and live beside the plain per-channel
 * record rather than inside it, so a test thread can inject while another
 * thread runs the ops without a memberwise copy of an atomic ever happening.
 * The record is a plain struct that udsp_slic_fake_chan_state_get copies out
 * whole from another thread, so chan_lock orders the ops' writes against that
 * copy: per field atomics would still let one copy mix two op results.
 */
struct udsp_slic {
	bool in_use;
	const struct udsp_slic_host *host;
	unsigned channels;
	struct udsp_slic_safe safe;
	struct udsp_slic_fake_chan_state chan[UDSP_SLIC_FAKE_CHAN_MAX];
	struct udsp_slic_monitor monitor[UDSP_SLIC_FAKE_CHAN_MAX];
	pthread_mutex_t chan_lock;
	_Atomic bool inject_hook[UDSP_SLIC_FAKE_CHAN_MAX];
	/* The toggles hook_read_moved has not reported yet; the injector
	 * adds under chan_lock and the op reads and clears under it. */
	unsigned hook_toggles[UDSP_SLIC_FAKE_CHAN_MAX];
	_Atomic uint_least32_t inject_fault[UDSP_SLIC_FAKE_CHAN_MAX];
	_Atomic uint_least64_t inject_err[UDSP_SLIC_FAKE_CALL_MAX];
	_Atomic uint_least32_t inject_delay[UDSP_SLIC_FAKE_CALL_MAX];
	struct udsp_slic_fake_entry journal[UDSP_SLIC_FAKE_JOURNAL_MAX];
	/* The slic thread appends and a test thread reads a snapshot, so the
	 * length publishes the entries: a reader that sees a length has the
	 * entries it points to (udsp_slic_fake_journal_get). */
	_Atomic unsigned journal_len;
	_Atomic unsigned journal_lost;
};

static struct udsp_slic fake_inst[UDSP_SLIC_FAKE_INST_MAX];

static unsigned fake_channels_default = 2;

/* What caps_get reports. The fake is the only wideband-capable backend host CI
 * has, so it claims the capability by default and a case clears it to drive the
 * rate degrade. */
static bool fake_wideband = true;

/* The length mlt_run reports, negative for the true one (fake.h). A test
 * writes it from its control thread while the slic thread runs the op. */
static _Atomic int_least32_t fake_mlt_len = -1;

/* One shadow bus behind udsp_slic_fake_host_get(), shared by every instance as
 * one spidev is. It keeps the last transfer's tx bytes and a canned rx byte,
 * because a dumb full-duplex transfer has no registers to shadow. mono_ms only
 * advances in delay_ms, so a test that asks the same clock twice gets the same
 * answer and never sleeps. */
static struct {
	uint8_t tx[UDSP_SLIC_FAKE_TX_MAX];
	size_t tx_len;
	unsigned calls;
	uint8_t rx_value;
	uint32_t mono_ms;
	int gpio_reset;
	int gpio_interrupt;
	int sem_depth;
} fake_bus;

static struct udsp_slic_fake_chan_state *chan_get(struct udsp_slic *s, unsigned chan)
{
	if (!s || !s->in_use || chan >= s->channels)
		return NULL;

	return &s->chan[chan];
}

static int inject_err_take(struct udsp_slic *s, enum udsp_slic_fake_call call)
{
	uint_least64_t arm;

	if (!s || call >= UDSP_SLIC_FAKE_CALL_MAX)
		return 0;

	arm = atomic_load_explicit(&s->inject_err[call], memory_order_acquire);
	while ((uint32_t)arm) {
		if (atomic_compare_exchange_weak_explicit(&s->inject_err[call],
				&arm, arm - 1, memory_order_acq_rel,
				memory_order_acquire))
			return (int32_t)(arm >> 32);
	}

	return 0;
}

static void inject_delay_run(struct udsp_slic *s, enum udsp_slic_fake_call call)
{
	uint32_t ms;
	struct timespec rel;

	if (!s || call >= UDSP_SLIC_FAKE_CALL_MAX)
		return;
	ms = (uint32_t)atomic_load_explicit(&s->inject_delay[call],
					    memory_order_acquire);
	if (!ms)
		return;

	rel.tv_sec = ms / 1000u;
	rel.tv_nsec = (long)(ms % 1000u) * 1000000L;
	while (nanosleep(&rel, &rel) < 0 && errno == EINTR)
		continue;
}

/* An armed error skips the delay because a failed op never reaches the line. */
static int inject_take(struct udsp_slic *s, enum udsp_slic_fake_call call)
{
	int err = inject_err_take(s, call);

	if (err)
		return err;
	inject_delay_run(s, call);

	return 0;
}

static void journal_push(struct udsp_slic *s, enum udsp_slic_fake_call call,
			 unsigned chan, int32_t arg0, int32_t arg1)
{
	unsigned len = atomic_load_explicit(&s->journal_len,
					    memory_order_relaxed);

	if (len >= UDSP_SLIC_FAKE_JOURNAL_MAX) {
		atomic_fetch_add_explicit(&s->journal_lost, 1u,
					  memory_order_relaxed);
		return;
	}

	struct udsp_slic_fake_entry *e = &s->journal[len];

	e->call = (uint8_t)call;
	e->chan = (uint8_t)chan;
	e->arg0 = arg0;
	e->arg1 = arg1;
	atomic_store_explicit(&s->journal_len, len + 1u, memory_order_release);
}

static size_t frame_put(uint8_t *out, unsigned chan, uint8_t reg, uint8_t value)
{
	out[0] = FAKE_CMD_WRITE(chan);
	out[1] = reg;
	out[2] = value;

	return FAKE_FRAME_LEN;
}

/* The pre-framed safe transfer: every line to linefeed-open and ring-off, in
 * the fake's own write frames, so the signal handler blits it with one spidev
 * call. */
static void safe_build(struct udsp_slic *s)
{
	size_t len = 0;

	for (unsigned c = 0; c < s->channels; c++) {
		len += frame_put(&s->safe.bytes[len], c, FAKE_REG_LINEFEED, UDSP_LF_OPEN);
		len += frame_put(&s->safe.bytes[len], c, FAKE_REG_RING, 0);
	}

	s->safe.len = len;
}

static int probe_reg0(struct udsp_slic *s)
{
	uint8_t tx[FAKE_FRAME_LEN] = { FAKE_CMD_READ(0), FAKE_REG_PART, 0 };
	uint8_t rx[FAKE_FRAME_LEN] = { 0 };

	if (!s->host)
		return 0;
	if (!s->host->spi_xfer)
		return -EINVAL;

	return s->host->spi_xfer(s->host->ctx, tx, rx, sizeof(tx));
}

static void inst_free(struct udsp_slic *s)
{
	pthread_mutex_destroy(&s->chan_lock);
	memset(s, 0, sizeof(*s));
}

static struct udsp_slic *inst_alloc(void)
{
	for (unsigned i = 0; i < UDSP_SLIC_FAKE_INST_MAX; i++) {
		if (fake_inst[i].in_use)
			continue;
		memset(&fake_inst[i], 0, sizeof(fake_inst[i]));
		pthread_mutex_init(&fake_inst[i].chan_lock, NULL);
		fake_inst[i].in_use = true;
		return &fake_inst[i];
	}

	return NULL;
}

static struct udsp_slic *fake_attach(const struct udsp_slic_host *host,
				     unsigned *channels)
{
	unsigned n = fake_channels_default;

	if (!channels || n < 1 || n > UDSP_SLIC_FAKE_CHAN_MAX)
		return NULL;

	struct udsp_slic *s = inst_alloc();

	if (!s)
		return NULL;

	s->host = host;
	s->channels = n;
	safe_build(s);

	if (probe_reg0(s) < 0) {
		inst_free(s);
		return NULL;
	}

	journal_push(s, UDSP_SLIC_FAKE_ATTACH, 0, (int32_t)n, 0);
	*channels = n;

	return s;
}

static void fake_detach(struct udsp_slic *s)
{
	if (!s || !s->in_use)
		return;

	inst_free(s);
}

/* The shared open of a fake op: look the channel up, take the armed error
 * or delay for the call, then take chan_lock. An op with an argument guard
 * runs the guard before this, so a rejected argument never spends the
 * injection. The delay sleeps before the lock, so a getter never waits on it;
 * the journal push runs under the lock, because it never blocks. An op that
 * passed here ends with chan_end. */
static int chan_begin(struct udsp_slic *s, unsigned chan,
		      enum udsp_slic_fake_call call,
		      struct udsp_slic_fake_chan_state **out)
{
	struct udsp_slic_fake_chan_state *c = chan_get(s, chan);
	int err;

	if (!c)
		return -EINVAL;
	err = inject_take(s, call);
	if (err)
		return err;
	pthread_mutex_lock(&s->chan_lock);
	*out = c;

	return 0;
}

static void chan_end(struct udsp_slic *s)
{
	pthread_mutex_unlock(&s->chan_lock);
}

static int fake_init(struct udsp_slic *s, unsigned chan)
{
	struct udsp_slic_fake_chan_state *c;
	int err = chan_begin(s, chan, UDSP_SLIC_FAKE_INIT, &c);

	if (err)
		return err;

	c->inited = true;
	c->init_count++;
	journal_push(s, UDSP_SLIC_FAKE_INIT, chan, 0, 0);
	chan_end(s);

	return 0;
}

static int fake_linefeed_set(struct udsp_slic *s, unsigned chan,
			     enum udsp_linefeed lf)
{
	struct udsp_slic_fake_chan_state *c;
	int err;

	if ((unsigned)lf > UDSP_LF_REV_OHT)
		return -EINVAL;
	err = chan_begin(s, chan, UDSP_SLIC_FAKE_LINEFEED_SET, &c);
	if (err)
		return err;

	c->linefeed = (uint8_t)lf;
	c->linefeed_count++;
	journal_push(s, UDSP_SLIC_FAKE_LINEFEED_SET, chan, (int32_t)lf, 0);
	chan_end(s);

	return 0;
}

/* Recovery clears the injection cell because fault_read reads it; an alarm
 * that the part clears must read clean after recovery. */
static int fake_reinit(struct udsp_slic *s, unsigned chan)
{
	struct udsp_slic_fake_chan_state *c;
	int err = chan_begin(s, chan, UDSP_SLIC_FAKE_REINIT, &c);

	if (err)
		return err;

	atomic_store_explicit(&s->inject_fault[chan], 0, memory_order_relaxed);
	c->fault_bits = 0;
	c->linefeed = UDSP_LF_OPEN;
	c->reinit_count++;
	journal_push(s, UDSP_SLIC_FAKE_REINIT, chan, c->linefeed, 0);
	chan_end(s);

	return 0;
}

static int fake_converter_restart(struct udsp_slic *s, unsigned chan)
{
	struct udsp_slic_fake_chan_state *c;
	int err = chan_begin(s, chan, UDSP_SLIC_FAKE_CONVERTER_RESTART, &c);

	if (err)
		return err;

	atomic_fetch_and_explicit(&s->inject_fault[chan],
				  ~(uint32_t)UDSP_FAULT_SLIC_DCDC,
				  memory_order_relaxed);
	c->fault_bits &= ~UDSP_FAULT_SLIC_DCDC;
	c->linefeed = UDSP_LF_OPEN;
	c->converter_restart_count++;
	journal_push(s, UDSP_SLIC_FAKE_CONVERTER_RESTART, chan, c->linefeed, 0);
	chan_end(s);

	return 0;
}

static int fake_monitor_read(struct udsp_slic *s, unsigned chan,
			     struct udsp_slic_monitor *out)
{
	struct udsp_slic_fake_chan_state *c;
	int err;

	if (!out)
		return -EINVAL;
	err = chan_begin(s, chan, UDSP_SLIC_FAKE_MONITOR_READ, &c);
	if (err)
		return err;

	*out = s->monitor[chan];
	c->monitor_count++;
	journal_push(s, UDSP_SLIC_FAKE_MONITOR_READ, chan, 0, 0);
	chan_end(s);

	return 0;
}

static int fake_converter_read(struct udsp_slic *s, unsigned chan,
			       struct udsp_slic_converter *out)
{
	struct udsp_slic_fake_chan_state *c;
	int err;

	if (!out)
		return -EINVAL;
	err = chan_begin(s, chan, UDSP_SLIC_FAKE_CONVERTER_READ, &c);
	if (err)
		return err;

	out->pd_dcdc = s->monitor[chan].pd_dcdc;
	out->dcdc_status = s->monitor[chan].dcdc_status;
	out->lcrrtp = s->monitor[chan].lcrrtp;
	c->converter_read_count++;
	journal_push(s, UDSP_SLIC_FAKE_CONVERTER_READ, chan, 0, 0);
	chan_end(s);

	return 0;
}

static int fake_ring_start(struct udsp_slic *s, unsigned chan)
{
	struct udsp_slic_fake_chan_state *c;
	int err = chan_begin(s, chan, UDSP_SLIC_FAKE_RING_START, &c);

	if (err)
		return err;

	c->ringing = true;
	c->ring_start_count++;
	journal_push(s, UDSP_SLIC_FAKE_RING_START, chan, 0, 0);
	chan_end(s);

	return 0;
}

static int fake_ring_stop(struct udsp_slic *s, unsigned chan)
{
	struct udsp_slic_fake_chan_state *c;
	int err = chan_begin(s, chan, UDSP_SLIC_FAKE_RING_STOP, &c);

	if (err)
		return err;

	c->ringing = false;
	c->ring_stop_count++;
	journal_push(s, UDSP_SLIC_FAKE_RING_STOP, chan, 0, 0);
	chan_end(s);

	return 0;
}

static int fake_hook_read(struct udsp_slic *s, unsigned chan, bool *offhook)
{
	struct udsp_slic_fake_chan_state *c;
	int err;

	if (!offhook)
		return -EINVAL;
	err = chan_begin(s, chan, UDSP_SLIC_FAKE_HOOK_READ, &c);
	if (err)
		return err;

	c->offhook = atomic_load_explicit(&s->inject_hook[chan],
					  memory_order_relaxed);
	*offhook = c->offhook;
	c->hook_read_count++;
	journal_push(s, UDSP_SLIC_FAKE_HOOK_READ, chan, c->offhook ? 1 : 0, 0);
	chan_end(s);

	return 0;
}

static int fake_hook_read_moved(struct udsp_slic *s, unsigned chan,
				bool *offhook, bool *moved)
{
	struct udsp_slic_fake_chan_state *c;
	int err;

	if (!offhook || !moved)
		return -EINVAL;
	/* The HOOK_READ arms govern this op too: to the engine both are the
	 * hook read, and a case that kills a line with hook read failures must
	 * not depend on which op the fake offers. */
	err = chan_begin(s, chan, UDSP_SLIC_FAKE_HOOK_READ, &c);
	if (err)
		return err;

	c->offhook = atomic_load_explicit(&s->inject_hook[chan],
					  memory_order_relaxed);
	*offhook = c->offhook;
	*moved = s->hook_toggles[chan] != 0;
	s->hook_toggles[chan] = 0;
	/* A hook read by either op: the sampling cadence a case counts is the
	 * same whichever op the engine has. */
	c->hook_read_count++;
	c->hook_moved_count++;
	journal_push(s, UDSP_SLIC_FAKE_HOOK_READ_MOVED, chan,
		     c->offhook ? 1 : 0, *moved ? 1 : 0);
	chan_end(s);

	return 0;
}

static int fake_fault_read(struct udsp_slic *s, unsigned chan, uint32_t *bits)
{
	struct udsp_slic_fake_chan_state *c;
	int err;

	if (!bits)
		return -EINVAL;
	err = chan_begin(s, chan, UDSP_SLIC_FAKE_FAULT_READ, &c);
	if (err)
		return err;

	c->fault_bits = (uint32_t)atomic_load_explicit(&s->inject_fault[chan],
						       memory_order_relaxed);
	*bits = c->fault_bits;
	c->fault_read_count++;
	journal_push(s, UDSP_SLIC_FAKE_FAULT_READ, chan, (int32_t)c->fault_bits, 0);
	chan_end(s);

	return 0;
}

static int fake_gain_set(struct udsp_slic *s, unsigned chan, int tx_half_db,
			 int rx_half_db)
{
	struct udsp_slic_fake_chan_state *c;
	int err = chan_begin(s, chan, UDSP_SLIC_FAKE_GAIN_SET, &c);

	if (err)
		return err;

	c->gain_tx_half_db = (int16_t)tx_half_db;
	c->gain_rx_half_db = (int16_t)rx_half_db;
	c->gain_count++;
	journal_push(s, UDSP_SLIC_FAKE_GAIN_SET, chan, tx_half_db, rx_half_db);
	chan_end(s);

	return 0;
}

static int fake_mwi_lamp(struct udsp_slic *s, unsigned chan, bool on)
{
	struct udsp_slic_fake_chan_state *c;
	int err = chan_begin(s, chan, UDSP_SLIC_FAKE_MWI_LAMP, &c);

	if (err)
		return err;

	c->mwi_on = on;
	c->mwi_count++;
	journal_push(s, UDSP_SLIC_FAKE_MWI_LAMP, chan, on ? 1 : 0, 0);
	chan_end(s);

	return 0;
}

static int fake_powersave(struct udsp_slic *s, unsigned chan, bool on)
{
	struct udsp_slic_fake_chan_state *c;
	int err = chan_begin(s, chan, UDSP_SLIC_FAKE_POWERSAVE, &c);

	if (err)
		return err;

	c->powersave_on = on;
	c->powersave_count++;
	journal_push(s, UDSP_SLIC_FAKE_POWERSAVE, chan, on ? 1 : 0, 0);
	chan_end(s);

	return 0;
}

static int fake_metering(struct udsp_slic *s, unsigned chan,
			 enum udsp_slic_meter_freq f, unsigned on_ms,
			 unsigned off_ms, unsigned count)
{
	struct udsp_slic_fake_chan_state *c;
	int err;

	if (f != UDSP_METER_12KHZ && f != UDSP_METER_16KHZ)
		return -EINVAL;
	err = chan_begin(s, chan, UDSP_SLIC_FAKE_METERING, &c);
	if (err)
		return err;

	c->meter_freq = (uint8_t)f;
	c->meter_on_ms = on_ms;
	c->meter_off_ms = off_ms;
	c->meter_count = count;
	c->metering_count++;
	journal_push(s, UDSP_SLIC_FAKE_METERING, chan, (int32_t)f, (int32_t)count);
	chan_end(s);

	return 0;
}

static int fake_loopback(struct udsp_slic *s, unsigned chan, bool on)
{
	struct udsp_slic_fake_chan_state *c;
	int err = chan_begin(s, chan, UDSP_SLIC_FAKE_LOOPBACK, &c);

	if (err)
		return err;

	c->loopback_on = on;
	c->loopback_count++;
	journal_push(s, UDSP_SLIC_FAKE_LOOPBACK, chan, on ? 1 : 0, 0);
	chan_end(s);

	return 0;
}

/* Only mlt_run answers a failure with bytes: a run that ends early still
 * reports why, in the status byte of the head alone, and the core keeps that
 * record. The channel record and the journal stay untouched, because the call
 * reached no line. */
static int fake_mlt_fail(struct udsp_slic_mlt_result *out, uint8_t test_id,
			 int err)
{
	int32_t force = (int32_t)atomic_load_explicit(&fake_mlt_len,
						      memory_order_acquire);

	memset(out, 0, sizeof(*out));
	out->test_id = test_id;
	out->len = force < 0 ? FAKE_MLT_HEAD_LEN : (uint16_t)force;
	out->data[FAKE_MLT_HEAD_TEST] = test_id;
	out->data[FAKE_MLT_HEAD_STATUS] = (uint8_t)-err;

	return err;
}

/*
 * The record is the fake's own eight byte head plus the per channel call
 * count, so a caller can prove the opaque bytes crossed the boundary unchanged
 * and can tell one run from the next (slic.h: the caller decodes per-test
 * bytes and no ProSLIC type crosses).
 */
static int fake_mlt_run(struct udsp_slic *s, unsigned chan, uint8_t test_id,
			struct udsp_slic_mlt_result *out)
{
	struct udsp_slic_fake_chan_state *c;
	int32_t force;
	int err;

	/* The channel is checked here too, so a bad channel returns a bare
	 * -EINVAL and only an armed error writes the head-only record. */
	if (!chan_get(s, chan) || !out)
		return -EINVAL;
	err = chan_begin(s, chan, UDSP_SLIC_FAKE_MLT_RUN, &c);
	if (err)
		return fake_mlt_fail(out, test_id, err);

	c->mlt_test_id = test_id;
	c->mlt_count++;

	memset(out, 0, sizeof(*out));
	out->test_id = test_id;
	out->data[FAKE_MLT_HEAD_TEST] = test_id;
	out->data[FAKE_MLT_HEAD_STATUS] = 0;
	for (unsigned i = 0; i < 4; i++)
		out->data[FAKE_MLT_HEAD_LEN + i] =
			(uint8_t)(c->mlt_count >> (8u * i));
	force = (int32_t)atomic_load_explicit(&fake_mlt_len,
					      memory_order_acquire);
	out->len = force < 0 ? FAKE_MLT_RESULT_LEN : (uint16_t)force;

	journal_push(s, UDSP_SLIC_FAKE_MLT_RUN, chan, test_id, 0);
	chan_end(s);

	return 0;
}

/* A whole-part property with no journal row: the record is what the part can
 * do, not a call the engine made, and udsp_open reads it once per open
 * (slic.h caps_get). */
static uint32_t fake_caps_get(struct udsp_slic *s)
{
	if (!s || !s->in_use)
		return 0;

	return fake_wideband ? UDSP_SLIC_CAP_WIDEBAND : 0u;
}

static int fake_region_set(struct udsp_slic *s, unsigned chan, const char *iso3)
{
	struct udsp_slic_fake_chan_state *c;
	int err;

	if (!iso3)
		return -EINVAL;
	err = chan_begin(s, chan, UDSP_SLIC_FAKE_REGION_SET, &c);
	if (err)
		return err;

	strncpy(c->region_iso3, iso3, sizeof(c->region_iso3) - 1);
	c->region_iso3[sizeof(c->region_iso3) - 1] = '\0';
	c->region_count++;
	journal_push(s, UDSP_SLIC_FAKE_REGION_SET, chan, 0, 0);
	chan_end(s);

	return 0;
}

static void fake_safe_xfer(struct udsp_slic *s, struct udsp_slic_safe *out)
{
	if (!s || !s->in_use || !out)
		return;

	*out = s->safe;
	journal_push(s, UDSP_SLIC_FAKE_SAFE_XFER, 0, (int32_t)s->safe.len, 0);
}

static int host_spi_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
	if (!tx || !len)
		return -EINVAL;

	fake_bus.tx_len = len < UDSP_SLIC_FAKE_TX_MAX ? len : UDSP_SLIC_FAKE_TX_MAX;
	memcpy(fake_bus.tx, tx, fake_bus.tx_len);
	fake_bus.calls++;

	if (rx)
		memset(rx, fake_bus.rx_value, len);

	return 0;
}

static void host_delay_ms(void *ctx, unsigned ms)
{
	fake_bus.mono_ms += ms;
}

static uint32_t host_mono_ms(void *ctx)
{
	return fake_bus.mono_ms;
}

static int *gpio_pin(const char *name)
{
	if (name && !strcmp(name, "slic_reset"))
		return &fake_bus.gpio_reset;
	if (name && !strcmp(name, "slic_interrupt"))
		return &fake_bus.gpio_interrupt;

	return NULL;
}

static int host_gpio_set(void *ctx, const char *name, int value)
{
	int *pin = gpio_pin(name);

	if (!pin)
		return -ENOENT;

	*pin = value;

	return 0;
}

static int host_gpio_get(void *ctx, const char *name, int *value)
{
	int *pin = gpio_pin(name);

	if (!pin || !value)
		return -ENOENT;

	*value = *pin;

	return 0;
}

static int host_sem(void *ctx, int enter)
{
	if (!enter && fake_bus.sem_depth <= 0)
		return -EINVAL;

	fake_bus.sem_depth += enter ? 1 : -1;

	return 0;
}

static const struct udsp_slic_host fake_host = {
	.ctx = NULL,
	.spi_xfer = host_spi_xfer,
	.delay_ms = host_delay_ms,
	.mono_ms = host_mono_ms,
	.gpio_set = host_gpio_set,
	.gpio_get = host_gpio_get,
	.sem = host_sem,
};

/* Not const: udsp_slic_fake_optional_set() clears a nullable op in place so a
 * test drives the engine's capability gating both ways. The descriptor still
 * hands out a const pointer, as the contract does. */
static struct udsp_slic_ops fake_ops = {
	.name = "fake",
	.attach = fake_attach,
	.detach = fake_detach,
	.init = fake_init,
	.linefeed_set = fake_linefeed_set,
	.ring_start = fake_ring_start,
	.ring_stop = fake_ring_stop,
	.hook_read = fake_hook_read,
	.fault_read = fake_fault_read,
	.gain_set = fake_gain_set,
	.mwi_lamp = fake_mwi_lamp,
	.powersave = fake_powersave,
	.metering = fake_metering,
	.loopback = fake_loopback,
	.mlt_run = fake_mlt_run,
	.safe_xfer = fake_safe_xfer,
	.region_set = fake_region_set,
	.caps_get = fake_caps_get,
	.reinit = fake_reinit,
	.converter_restart = fake_converter_restart,
	.monitor_read = fake_monitor_read,
	.converter_read = fake_converter_read,
	.hook_read_moved = fake_hook_read_moved,
};

const struct udsp_slic_descriptor udsp_slic_fake_descriptor = {
	.abi_version = UDSP_SLIC_ABI_VERSION,
	.struct_size = sizeof(struct udsp_slic_ops),
	.ops = &fake_ops,
};

void udsp_slic_fake_optional_set(uint32_t opt_mask)
{
	uint32_t m = opt_mask & UDSP_SLIC_FAKE_OPT_ALL;

	fake_ops.mwi_lamp = (m & UDSP_SLIC_FAKE_OPT_MWI_LAMP) ? fake_mwi_lamp : NULL;
	fake_ops.powersave = (m & UDSP_SLIC_FAKE_OPT_POWERSAVE) ? fake_powersave : NULL;
	fake_ops.metering = (m & UDSP_SLIC_FAKE_OPT_METERING) ? fake_metering : NULL;
	fake_ops.loopback = (m & UDSP_SLIC_FAKE_OPT_LOOPBACK) ? fake_loopback : NULL;
	fake_ops.mlt_run = (m & UDSP_SLIC_FAKE_OPT_MLT) ? fake_mlt_run : NULL;
	fake_ops.region_set = (m & UDSP_SLIC_FAKE_OPT_REGION) ? fake_region_set : NULL;
	fake_ops.caps_get = (m & UDSP_SLIC_FAKE_OPT_CAPS) ? fake_caps_get : NULL;
	fake_ops.reinit = (m & UDSP_SLIC_FAKE_OPT_REINIT) ? fake_reinit : NULL;
	fake_ops.converter_restart = (m & UDSP_SLIC_FAKE_OPT_CONVERTER) ?
		fake_converter_restart : NULL;
	fake_ops.monitor_read = (m & UDSP_SLIC_FAKE_OPT_MONITOR) ?
		fake_monitor_read : NULL;
	fake_ops.converter_read = (m & UDSP_SLIC_FAKE_OPT_CONVERTER_READ) ?
		fake_converter_read : NULL;
	fake_ops.hook_read_moved = (m & UDSP_SLIC_FAKE_OPT_HOOK_MOVED) ?
		fake_hook_read_moved : NULL;
}

void udsp_slic_fake_channels_set(unsigned channels)
{
	fake_channels_default = channels;
}

void udsp_slic_fake_wideband_set(bool on)
{
	fake_wideband = on;
}

void udsp_slic_fake_mlt_len_set(int len)
{
	atomic_store_explicit(&fake_mlt_len, len, memory_order_release);
}

void udsp_slic_fake_reset(void)
{
	memset(&fake_bus, 0, sizeof(fake_bus));
	udsp_slic_fake_channels_set(2);
	udsp_slic_fake_wideband_set(true);
	udsp_slic_fake_mlt_len_set(-1);
	udsp_slic_fake_optional_set(UDSP_SLIC_FAKE_OPT_ALL);
	for (unsigned i = 0; i < UDSP_SLIC_FAKE_INST_MAX; i++) {
		for (unsigned op = 0; op < UDSP_SLIC_FAKE_CALL_MAX; op++)
			atomic_store_explicit(&fake_inst[i].inject_err[op], 0,
					      memory_order_release);
	}
}

const struct udsp_slic_host *udsp_slic_fake_host_get(void)
{
	return &fake_host;
}

unsigned udsp_slic_fake_host_tx_get(uint8_t *out, unsigned max, unsigned *calls)
{
	if (calls)
		*calls = fake_bus.calls;

	unsigned n = fake_bus.tx_len < max ? (unsigned)fake_bus.tx_len : max;

	if (out)
		memcpy(out, fake_bus.tx, n);

	return (unsigned)fake_bus.tx_len;
}

void udsp_slic_fake_host_rx_set(uint8_t value)
{
	fake_bus.rx_value = value;
}

void udsp_slic_fake_hook_inject(struct udsp_slic *s, unsigned chan, bool offhook,
				unsigned toggles)
{
	if (!chan_get(s, chan))
		return;

	pthread_mutex_lock(&s->chan_lock);
	atomic_store_explicit(&s->inject_hook[chan], offhook,
			      memory_order_relaxed);
	s->hook_toggles[chan] += toggles;
	pthread_mutex_unlock(&s->chan_lock);
}

void udsp_slic_fake_hook_set(struct udsp_slic *s, unsigned chan, bool offhook)
{
	if (!chan_get(s, chan))
		return;

	pthread_mutex_lock(&s->chan_lock);
	if (atomic_load_explicit(&s->inject_hook[chan], memory_order_relaxed) !=
	    offhook)
		s->hook_toggles[chan]++;
	atomic_store_explicit(&s->inject_hook[chan], offhook,
			      memory_order_relaxed);
	pthread_mutex_unlock(&s->chan_lock);
}

void udsp_slic_fake_fault_set(struct udsp_slic *s, unsigned chan, uint32_t bits)
{
	if (!chan_get(s, chan))
		return;

	atomic_store_explicit(&s->inject_fault[chan], bits, memory_order_relaxed);
}

void udsp_slic_fake_error_set(struct udsp_slic *s, enum udsp_slic_fake_call op,
			      int errno_neg)
{
	udsp_slic_fake_error_repeat_set(s, op, errno_neg, 1);
}

void udsp_slic_fake_error_repeat_set(struct udsp_slic *s,
				     enum udsp_slic_fake_call op,
				     int errno_neg, unsigned count)
{
	uint_least64_t arm = 0;

	if (!s || !s->in_use || (unsigned)op >= UDSP_SLIC_FAKE_CALL_MAX)
		return;

	if (errno_neg && count)
		arm = ((uint_least64_t)(uint32_t)errno_neg << 32) | count;
	atomic_store_explicit(&s->inject_err[op], arm, memory_order_release);
}

void udsp_slic_fake_monitor_set(struct udsp_slic *s, unsigned chan,
				const struct udsp_slic_monitor *m)
{
	if (!chan_get(s, chan) || !m)
		return;

	pthread_mutex_lock(&s->chan_lock);
	s->monitor[chan] = *m;
	pthread_mutex_unlock(&s->chan_lock);
}

void udsp_slic_fake_delay_set(struct udsp_slic *s, enum udsp_slic_fake_call op,
			      unsigned ms)
{
	if (!s || !s->in_use || op >= UDSP_SLIC_FAKE_CALL_MAX)
		return;

	atomic_store_explicit(&s->inject_delay[op], ms, memory_order_release);
}

bool udsp_slic_fake_chan_state_get(struct udsp_slic *s, unsigned chan,
				   struct udsp_slic_fake_chan_state *out)
{
	struct udsp_slic_fake_chan_state *c = chan_get(s, chan);

	if (!c || !out)
		return false;

	pthread_mutex_lock(&s->chan_lock);
	*out = *c;
	pthread_mutex_unlock(&s->chan_lock);

	return true;
}

unsigned udsp_slic_fake_journal_get(struct udsp_slic *s,
				    struct udsp_slic_fake_entry *out,
				    unsigned max, unsigned *lost)
{
	unsigned len;

	if (!s || !s->in_use)
		return 0;

	if (lost)
		*lost = atomic_load_explicit(&s->journal_lost,
					     memory_order_relaxed);

	len = atomic_load_explicit(&s->journal_len, memory_order_acquire);

	unsigned n = len < max ? len : max;

	if (out)
		memcpy(out, s->journal, n * sizeof(*out));

	return len;
}

void udsp_slic_fake_journal_clear(struct udsp_slic *s)
{
	if (!s || !s->in_use)
		return;

	atomic_store_explicit(&s->journal_len, 0u, memory_order_relaxed);
	atomic_store_explicit(&s->journal_lost, 0u, memory_order_relaxed);
}
