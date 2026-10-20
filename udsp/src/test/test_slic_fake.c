// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Unit tests for the in-tree fake SLIC (slic/fake.c) against the stable
 * contract include/udsp/slic.h.
 *
 * The descriptor carries the ABI version, the ops size and every mandatory
 * op. attach reports the settable channel count, and builds a safe transfer
 * that reaches every attached channel inside UDSP_SLIC_SAFE_MAX. Linefeed,
 * ring, hook, fault and gain round trip through the recorders. The journal
 * holds the call order, for example ring_stop after ring_start, and it
 * saturates with a lost count.
 *
 * Two instances live at once, and their slots come back. Every op rejects a
 * channel past the attached count. A cleared nullable op leaves its pointer in
 * the ops table NULL, and the engine gates the capability on that NULL.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "udsp/event.h"
#include "udsp/slic.h"

#include "../slic/fake.h"
#include "util.h"

#define J_MAX	32

static const struct udsp_slic_ops *ops_get(void)
{
	return udsp_slic_fake_descriptor.ops;
}

static struct udsp_slic *attach_fresh(unsigned channels, unsigned *got)
{
	udsp_slic_fake_reset();
	udsp_slic_fake_channels_set(channels);

	return ops_get()->attach(udsp_slic_fake_host_get(), got);
}

static bool journal_call_at(struct udsp_slic *s, unsigned idx,
			    enum udsp_slic_fake_call want)
{
	struct udsp_slic_fake_entry e[J_MAX];
	unsigned len = udsp_slic_fake_journal_get(s, e, J_MAX, NULL);

	if (idx >= len || idx >= J_MAX)
		return false;

	return e[idx].call == (uint8_t)want;
}

static int host_spi_xfer_fail(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
	return -EIO;
}

/* The fake owns its wire framing and uses 3-byte command, address and data
 * frames, the command byte carrying the channel in its low bits and a read flag
 * in bit 7 (slic/fake.c). Every safe-state frame must be a write. Every
 * attached channel must carry its linefeed-open and ring-off frame, or a crash
 * would leave a line powered (slic.h). */
static bool safe_covers_channels(struct udsp_slic *s, unsigned channels)
{
	struct udsp_slic_safe safe;
	unsigned writes[UDSP_SLIC_FAKE_CHAN_MAX] = { 0 };
	uint8_t reg_seen[UDSP_SLIC_FAKE_CHAN_MAX][2] = { { 0 } };

	memset(&safe, 0xa5, sizeof(safe));
	safe.len = 0;
	udsp_slic_fake_descriptor.ops->safe_xfer(s, &safe);

	if (safe.len == 0 || safe.len > UDSP_SLIC_SAFE_MAX)
		return false;
	if (safe.len % 3 || safe.len != (size_t)channels * 6)
		return false;

	for (size_t i = 0; i + 2 < safe.len; i += 3) {
		unsigned chan = safe.bytes[i] & 0x7fu;

		if (safe.bytes[i] & 0x80u)	/* a safe frame is never a read */
			return false;
		if (chan >= channels || writes[chan] >= 2)
			return false;
		if (safe.bytes[i + 2] != 0)	/* open and ring off are both zero */
			return false;
		reg_seen[chan][writes[chan]++] = safe.bytes[i + 1];
	}

	for (unsigned c = 0; c < channels; c++) {
		if (writes[c] != 2 || reg_seen[c][0] == reg_seen[c][1])
			return false;
		if (reg_seen[c][0] != reg_seen[0][0] || reg_seen[c][1] != reg_seen[0][1])
			return false;
	}

	return true;
}

static const struct udsp_slic_monitor monitor_sample = {
	.vtr_mv = -48001,
	.vtip_mv = 1002,
	.vring_mv = -49003,
	.vlong_mv = 1004,
	.vbat_mv = -52005,
	.vdc_mv = 12006,
	.itr_ua = 20007,
	.itip_ua = 21008,
	.iring_ua = -22009,
	.ilong_ua = 1010,
	.p_hvic_mw = 511,
	.vbat_tgt_mv = -53012,
	.vbat_irq_th_mv = 4013,
	.pd_dcdc = 0x123456,
	.dcdc_status = 0x654321,
	.lcrrtp = 0x0b,
	.linefeed = 0x11,
};

static void monitor_check(const struct udsp_slic_monitor *got,
			  const struct udsp_slic_monitor *want)
{
	CHECK(got->vtr_mv == want->vtr_mv);
	CHECK(got->vtip_mv == want->vtip_mv);
	CHECK(got->vring_mv == want->vring_mv);
	CHECK(got->vlong_mv == want->vlong_mv);
	CHECK(got->vbat_mv == want->vbat_mv);
	CHECK(got->vdc_mv == want->vdc_mv);
	CHECK(got->itr_ua == want->itr_ua);
	CHECK(got->itip_ua == want->itip_ua);
	CHECK(got->iring_ua == want->iring_ua);
	CHECK(got->ilong_ua == want->ilong_ua);
	CHECK(got->p_hvic_mw == want->p_hvic_mw);
	CHECK(got->vbat_tgt_mv == want->vbat_tgt_mv);
	CHECK(got->vbat_irq_th_mv == want->vbat_irq_th_mv);
	CHECK_UINT(got->pd_dcdc, want->pd_dcdc);
	CHECK_UINT(got->dcdc_status, want->dcdc_status);
	CHECK_UINT(got->lcrrtp, want->lcrrtp);
	CHECK_UINT(got->linefeed, want->linefeed);
}

static void recovery_cases_run(void)
{
	const struct udsp_slic_ops *ops = ops_get();
	struct udsp_slic_fake_chan_state st;
	unsigned n = 0;
	uint32_t bits = 0;
	struct udsp_slic *s = attach_fresh(2, &n);

	CASE("reinit clears every fault bit and opens an uninited channel");
	udsp_slic_fake_fault_set(s, 1, UINT32_MAX);
	CHECK(ops->fault_read(s, 1, &bits) == 0);
	CHECK_UINT(bits, UINT32_MAX);
	CHECK(ops->linefeed_set(s, 1, UDSP_LF_FWD_ACTIVE) == 0);
	CHECK(ops->reinit(s, 1) == 0);
	CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
	CHECK(!st.inited);
	CHECK_UINT(st.fault_bits, 0);
	CHECK_UINT(st.linefeed, UDSP_LF_OPEN);
	CHECK_UINT(st.reinit_count, 1);
	CHECK(ops->fault_read(s, 1, &bits) == 0);
	CHECK_UINT(bits, 0);
	ops->detach(s);

	CASE("converter_restart clears only DCDC and opens the channel");
	s = attach_fresh(2, &n);
	udsp_slic_fake_fault_set(s, 1,
		UDSP_FAULT_SLIC_THERMAL | UDSP_FAULT_SLIC_DCDC);
	CHECK(ops->fault_read(s, 1, &bits) == 0);
	CHECK_UINT(bits, UDSP_FAULT_SLIC_THERMAL | UDSP_FAULT_SLIC_DCDC);
	CHECK(ops->linefeed_set(s, 1, UDSP_LF_REV_ACTIVE) == 0);
	CHECK(ops->converter_restart(s, 1) == 0);
	CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
	CHECK(!st.inited);
	CHECK_UINT(st.fault_bits, UDSP_FAULT_SLIC_THERMAL);
	CHECK_UINT(st.linefeed, UDSP_LF_OPEN);
	CHECK_UINT(st.converter_restart_count, 1);
	CHECK(ops->fault_read(s, 1, &bits) == 0);
	CHECK_UINT(bits, UDSP_FAULT_SLIC_THERMAL);
	ops->detach(s);

	CASE("converter_restart preserves an unread fault record");
	s = attach_fresh(2, &n);
	udsp_slic_fake_fault_set(s, 1,
		UDSP_FAULT_SLIC_THERMAL | UDSP_FAULT_SLIC_DCDC);
	CHECK(ops->linefeed_set(s, 1, UDSP_LF_REV_ACTIVE) == 0);
	CHECK(ops->converter_restart(s, 1) == 0);
	CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
	CHECK_UINT(st.fault_bits, 0);
	CHECK_UINT(st.fault_read_count, 0);
	CHECK_UINT(st.linefeed, UDSP_LF_OPEN);
	CHECK_UINT(st.converter_restart_count, 1);
	CHECK(ops->fault_read(s, 1, &bits) == 0);
	CHECK_UINT(bits, UDSP_FAULT_SLIC_THERMAL);
	ops->detach(s);

	CASE("armed recovery errors leave the channel and journal unchanged");
	s = attach_fresh(2, &n);
	udsp_slic_fake_fault_set(s, 1, UINT32_MAX);
	CHECK(ops->fault_read(s, 1, &bits) == 0);
	CHECK(ops->linefeed_set(s, 1, UDSP_LF_FWD_ACTIVE) == 0);
	udsp_slic_fake_journal_clear(s);
	udsp_slic_fake_error_set(s, UDSP_SLIC_FAKE_REINIT, -EIO);
	udsp_slic_fake_error_set(s, UDSP_SLIC_FAKE_CONVERTER_RESTART, -EBUSY);
	CHECK(ops->reinit(s, 1) == -EIO);
	CHECK(ops->converter_restart(s, 1) == -EBUSY);
	CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
	CHECK_UINT(st.fault_bits, UINT32_MAX);
	CHECK_UINT(st.linefeed, UDSP_LF_FWD_ACTIVE);
	CHECK_UINT(st.reinit_count, 0);
	CHECK_UINT(st.converter_restart_count, 0);
	CHECK_UINT(udsp_slic_fake_journal_get(s, NULL, 0, NULL), 0);
	CHECK(ops->fault_read(s, 1, &bits) == 0);
	CHECK_UINT(bits, UINT32_MAX);
	CHECK(ops->reinit(s, 1) == 0);
	CHECK(ops->converter_restart(s, 1) == 0);
	CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
	CHECK_UINT(st.reinit_count, 1);
	CHECK_UINT(st.converter_restart_count, 1);
	ops->detach(s);
}

static void repeat_error_cases_run(void)
{
	const struct udsp_slic_ops *ops = ops_get();
	struct udsp_slic_fake_chan_state st;
	unsigned n = 0;
	struct udsp_slic *s = attach_fresh(2, &n);

	CASE("repeat 2 fails REINIT twice before one recorded success");
	udsp_slic_fake_journal_clear(s);
	udsp_slic_fake_error_repeat_set(s, UDSP_SLIC_FAKE_REINIT, -EIO, 2);
	CHECK(ops->reinit(s, 1) == -EIO);
	CHECK(ops->reinit(s, 1) == -EIO);
	CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
	CHECK_UINT(st.reinit_count, 0);
	CHECK_UINT(udsp_slic_fake_journal_get(s, NULL, 0, NULL), 0);
	CHECK(ops->reinit(s, 1) == 0);
	CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
	CHECK_UINT(st.reinit_count, 1);
	CHECK_UINT(udsp_slic_fake_journal_get(s, NULL, 0, NULL), 1);
	ops->detach(s);

	CASE("repeat 0 disarms a standing error");
	s = attach_fresh(2, &n);
	udsp_slic_fake_error_repeat_set(s, UDSP_SLIC_FAKE_REINIT, -EIO, 2);
	udsp_slic_fake_error_repeat_set(s, UDSP_SLIC_FAKE_REINIT, -EBUSY, 0);
	CHECK(ops->reinit(s, 1) == 0);
	CHECK(ops->reinit(s, 1) == 0);
	CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
	CHECK_UINT(st.reinit_count, 2);
	ops->detach(s);

	CASE("repeat errno 0 disarms a standing error");
	s = attach_fresh(2, &n);
	udsp_slic_fake_error_repeat_set(s, UDSP_SLIC_FAKE_REINIT, -EIO, 2);
	udsp_slic_fake_error_repeat_set(s, UDSP_SLIC_FAKE_REINIT, 0, 2);
	CHECK(ops->reinit(s, 1) == 0);
	CHECK(ops->reinit(s, 1) == 0);
	CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
	CHECK_UINT(st.reinit_count, 2);
	ops->detach(s);

	CASE("error_set overrides a repeat arm");
	s = attach_fresh(2, &n);
	udsp_slic_fake_error_repeat_set(s, UDSP_SLIC_FAKE_REINIT, -EIO, 2);
	udsp_slic_fake_error_set(s, UDSP_SLIC_FAKE_REINIT, -EBUSY);
	CHECK(ops->reinit(s, 1) == -EBUSY);
	CHECK(ops->reinit(s, 1) == 0);
	CHECK(ops->reinit(s, 1) == 0);
	CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
	CHECK_UINT(st.reinit_count, 2);
	ops->detach(s);

	CASE("a repeat arm affects only its selected op");
	s = attach_fresh(2, &n);
	udsp_slic_fake_error_repeat_set(s, UDSP_SLIC_FAKE_REINIT, -EIO, 2);
	CHECK(ops->converter_restart(s, 1) == 0);
	CHECK(ops->reinit(s, 1) == -EIO);
	CHECK(ops->converter_restart(s, 1) == 0);
	CHECK(ops->reinit(s, 1) == -EIO);
	CHECK(ops->reinit(s, 1) == 0);
	CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
	CHECK_UINT(st.converter_restart_count, 2);
	CHECK_UINT(st.reinit_count, 1);
	ops->detach(s);

	CASE("a fresh instance starts with repeat errors disarmed");
	s = attach_fresh(2, &n);
	udsp_slic_fake_error_repeat_set(s, UDSP_SLIC_FAKE_REINIT, -EIO, 2);
	ops->detach(s);
	s = ops->attach(udsp_slic_fake_host_get(), &n);
	CHECK(s != NULL);
	CHECK(ops->reinit(s, 1) == 0);
	CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
	CHECK_UINT(st.reinit_count, 1);
	ops->detach(s);

	CASE("reset disarms repeat errors on live instances");
	struct udsp_slic *instances[UDSP_SLIC_FAKE_INST_MAX];

	udsp_slic_fake_reset();
	for (unsigned i = 0; i < UDSP_SLIC_FAKE_INST_MAX; i++) {
		instances[i] = ops->attach(udsp_slic_fake_host_get(), &n);
		CHECK(instances[i] != NULL);
		udsp_slic_fake_error_repeat_set(instances[i],
			UDSP_SLIC_FAKE_REINIT, -EIO, 2);
		udsp_slic_fake_error_repeat_set(instances[i],
			UDSP_SLIC_FAKE_CONVERTER_RESTART, -EBUSY, 2);
	}
	udsp_slic_fake_reset();
	for (unsigned i = 0; i < UDSP_SLIC_FAKE_INST_MAX; i++) {
		CHECK(ops->reinit(instances[i], 1) == 0);
		CHECK(ops->converter_restart(instances[i], 1) == 0);
		CHECK(udsp_slic_fake_chan_state_get(instances[i], 1, &st));
		CHECK_UINT(st.reinit_count, 1);
		CHECK_UINT(st.converter_restart_count, 1);
		ops->detach(instances[i]);
	}
}

static void monitor_cases_run(void)
{
	const struct udsp_slic_ops *ops = ops_get();
	const struct udsp_slic_monitor zero = { 0 };
	struct udsp_slic_monitor got;
	struct udsp_slic_converter converter;
	struct udsp_slic_fake_chan_state st;
	unsigned n = 0;
	struct udsp_slic *s = attach_fresh(2, &n);

	CASE("monitor_read copies every field of the channel cell");
	udsp_slic_fake_monitor_set(s, 1, &monitor_sample);
	memset(&got, 0xa5, sizeof(got));
	CHECK(ops->monitor_read(s, 1, &got) == 0);
	monitor_check(&got, &monitor_sample);
	CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
	CHECK_UINT(st.monitor_count, 1);
	CHECK(ops->monitor_read(s, 0, &got) == 0);
	monitor_check(&got, &zero);
	ops->detach(s);

	CASE("converter_read copies the three words of the monitor cell");
	s = attach_fresh(2, &n);
	udsp_slic_fake_monitor_set(s, 1, &monitor_sample);
	CHECK(ops->converter_read(s, 1, &converter) == 0);
	CHECK_UINT(converter.pd_dcdc, monitor_sample.pd_dcdc);
	CHECK_UINT(converter.dcdc_status, monitor_sample.dcdc_status);
	CHECK_UINT(converter.lcrrtp, monitor_sample.lcrrtp);
	CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
	CHECK_UINT(st.converter_read_count, 1);
	ops->detach(s);

	CASE("monitor cells start at zero "
	     "and survive reset on a live instance");
	s = attach_fresh(2, &n);
	CHECK(ops->monitor_read(s, 1, &got) == 0);
	monitor_check(&got, &zero);
	udsp_slic_fake_monitor_set(s, 1, &monitor_sample);
	udsp_slic_fake_reset();
	CHECK(ops->monitor_read(s, 1, &got) == 0);
	monitor_check(&got, &monitor_sample);
	ops->detach(s);

	CASE("monitor_set ignores invalid instances and channels");
	s = attach_fresh(2, &n);
	udsp_slic_fake_monitor_set(NULL, 0, &monitor_sample);
	udsp_slic_fake_monitor_set(s, n, &monitor_sample);
	udsp_slic_fake_monitor_set(s, ~0u, &monitor_sample);
	CHECK(ops->monitor_read(s, 0, &got) == 0);
	monitor_check(&got, &zero);
	CHECK(ops->monitor_read(s, 1, &got) == 0);
	monitor_check(&got, &zero);
	ops->detach(s);
}

static void monitor_error_cases_run(void)
{
	const struct udsp_slic_ops *ops = ops_get();
	struct udsp_slic_monitor got;
	struct udsp_slic_converter converter;
	struct udsp_slic_fake_chan_state st;
	struct udsp_slic_fake_entry before[J_MAX], after[J_MAX];
	unsigned char sentinel[sizeof(got)];
	unsigned n = 0;
	unsigned len;
	struct udsp_slic *s = attach_fresh(2, &n);

	CASE("an armed monitor ENODATA "
	     "leaves output count and journal untouched");
	udsp_slic_fake_monitor_set(s, 1, &monitor_sample);
	memset(&got, 0xa5, sizeof(got));
	memcpy(sentinel, &got, sizeof(got));
	len = udsp_slic_fake_journal_get(s, before, J_MAX, NULL);
	udsp_slic_fake_error_set(s, UDSP_SLIC_FAKE_MONITOR_READ, -ENODATA);
	CHECK(ops->monitor_read(s, 1, &got) == -ENODATA);
	CHECK(memcmp(&got, sentinel, sizeof(got)) == 0);
	CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
	CHECK_UINT(st.monitor_count, 0);
	CHECK_UINT(udsp_slic_fake_journal_get(s, after, J_MAX, NULL), len);
	CHECK(memcmp(before, after, len * sizeof(before[0])) == 0);
	CHECK(ops->monitor_read(s, 1, &got) == 0);
	monitor_check(&got, &monitor_sample);
	CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
	CHECK_UINT(st.monitor_count, 1);
	CHECK_UINT(udsp_slic_fake_journal_get(s, after, J_MAX, NULL), len + 1);
	ops->detach(s);

	CASE("an armed converter_read error "
	     "leaves output count and journal untouched");
	s = attach_fresh(2, &n);
	udsp_slic_fake_monitor_set(s, 1, &monitor_sample);
	memset(&converter, 0xa5, sizeof(converter));
	memcpy(sentinel, &converter, sizeof(converter));
	udsp_slic_fake_journal_clear(s);
	udsp_slic_fake_error_set(s, UDSP_SLIC_FAKE_CONVERTER_READ, -EIO);
	CHECK(ops->converter_read(s, 1, &converter) == -EIO);
	CHECK(memcmp(&converter, sentinel, sizeof(converter)) == 0);
	CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
	CHECK_UINT(st.converter_read_count, 0);
	CHECK_UINT(udsp_slic_fake_journal_get(s, NULL, 0, NULL), 0);
	CHECK(ops->converter_read(s, 1, &converter) == 0);
	CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
	CHECK_UINT(st.converter_read_count, 1);
	CHECK_UINT(udsp_slic_fake_journal_get(s, NULL, 0, NULL), 1);
	ops->detach(s);
}

static void abi7_cases_run(void)
{
	const struct udsp_slic_ops *ops = ops_get();
	struct udsp_slic_monitor monitor;
	struct udsp_slic_converter converter;
	struct udsp_slic_fake_entry e[4];
	unsigned n = 0;
	struct udsp_slic *s = attach_fresh(2, &n);

	CASE("the four ABI 7 ops journal their call ids and channels");
	udsp_slic_fake_journal_clear(s);
	CHECK(ops->reinit(s, 1) == 0);
	CHECK(ops->converter_restart(s, 0) == 0);
	CHECK(ops->monitor_read(s, 1, &monitor) == 0);
	CHECK(ops->converter_read(s, 0, &converter) == 0);
	CHECK_UINT(udsp_slic_fake_journal_get(s, e, 4, NULL), 4);
	CHECK_UINT(e[0].call, UDSP_SLIC_FAKE_REINIT);
	CHECK_UINT(e[0].chan, 1);
	CHECK_UINT(e[0].arg0, UDSP_LF_OPEN);
	CHECK_UINT(e[1].call, UDSP_SLIC_FAKE_CONVERTER_RESTART);
	CHECK_UINT(e[1].chan, 0);
	CHECK_UINT(e[1].arg0, UDSP_LF_OPEN);
	CHECK_UINT(e[2].call, UDSP_SLIC_FAKE_MONITOR_READ);
	CHECK_UINT(e[2].chan, 1);
	CHECK_UINT(e[3].call, UDSP_SLIC_FAKE_CONVERTER_READ);
	CHECK_UINT(e[3].chan, 0);
	ops->detach(s);

	CASE("the four ABI 7 ops reject NULL and detached instances");
	CHECK(ops->reinit(NULL, 0) == -EINVAL);
	CHECK(ops->converter_restart(NULL, 0) == -EINVAL);
	CHECK(ops->monitor_read(NULL, 0, &monitor) == -EINVAL);
	CHECK(ops->converter_read(NULL, 0, &converter) == -EINVAL);
	CHECK(ops->reinit(s, 0) == -EINVAL);
	CHECK(ops->converter_restart(s, 0) == -EINVAL);
	CHECK(ops->monitor_read(s, 0, &monitor) == -EINVAL);
	CHECK(ops->converter_read(s, 0, &converter) == -EINVAL);

	CASE("each ABI 7 and ABI 8 optional bit controls only its own op");
	for (unsigned i = 7; i <= 11; i++) {
		uint32_t mask = UDSP_SLIC_FAKE_OPT_ALL & ~(1u << i);

		udsp_slic_fake_optional_set(mask);
		CHECK((ops->reinit != NULL) == (i != 7));
		CHECK((ops->converter_restart != NULL) == (i != 8));
		CHECK((ops->monitor_read != NULL) == (i != 9));
		CHECK((ops->converter_read != NULL) == (i != 10));
		CHECK((ops->hook_read_moved != NULL) == (i != 11));
		CHECK(ops->mwi_lamp != NULL);
		CHECK(ops->powersave != NULL);
		CHECK(ops->metering != NULL);
		CHECK(ops->loopback != NULL);
		CHECK(ops->mlt_run != NULL);
		CHECK(ops->part_name == NULL);
		CHECK(ops->region_set != NULL);
		CHECK(ops->caps_get != NULL);
	}

	CASE("reset restores all four ABI 7 ops and the ABI 8 op");
	udsp_slic_fake_optional_set(0);
	udsp_slic_fake_reset();
	CHECK(ops->reinit != NULL);
	CHECK(ops->converter_restart != NULL);
	CHECK(ops->monitor_read != NULL);
	CHECK(ops->converter_read != NULL);
	CHECK(ops->hook_read_moved != NULL);
}

/* hook_read_moved, the hook change record. It reports the toggles since the
 * previous call, whatever the level shows. The call that reports the toggles
 * clears them. */
static void abi8_cases_run(void)
{
	const struct udsp_slic_ops *ops = ops_get();
	struct udsp_slic_fake_chan_state st;
	struct udsp_slic_fake_entry e[2];
	unsigned n = 0;
	struct udsp_slic *s = attach_fresh(2, &n);
	bool offhook = true;
	bool moved = true;

	CASE("hook_read_moved reports the level and no move on a still line");
	udsp_slic_fake_journal_clear(s);
	CHECK(ops->hook_read_moved(s, 1, &offhook, &moved) == 0);
	CHECK(!offhook);
	CHECK(!moved);
	CHECK_UINT(udsp_slic_fake_journal_get(s, e, 2, NULL), 1);
	CHECK_UINT(e[0].call, UDSP_SLIC_FAKE_HOOK_READ_MOVED);
	CHECK_UINT(e[0].chan, 1);
	CHECK_UINT(e[0].arg0, 0);
	CHECK_UINT(e[0].arg1, 0);

	CASE("a level change is one move, reported once");
	udsp_slic_fake_hook_set(s, 1, true);
	CHECK(ops->hook_read_moved(s, 1, &offhook, &moved) == 0);
	CHECK(offhook);
	CHECK(moved);
	CHECK(ops->hook_read_moved(s, 1, &offhook, &moved) == 0);
	CHECK(offhook);
	CHECK(!moved);

	CASE("a pair of toggles moves without a level change");
	udsp_slic_fake_hook_inject(s, 1, true, 2);
	CHECK(ops->hook_read_moved(s, 1, &offhook, &moved) == 0);
	CHECK(offhook);
	CHECK(moved);
	CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
	CHECK_UINT(st.hook_moved_count, 4);
	CHECK_UINT(st.hook_read_count, 4);

	CASE("a level set to its own value is no move; hook_read sees no record");
	udsp_slic_fake_hook_set(s, 1, true);
	CHECK(ops->hook_read(s, 1, &offhook) == 0);
	CHECK(offhook);
	CHECK(ops->hook_read_moved(s, 1, &offhook, &moved) == 0);
	CHECK(!moved);

	CASE("hook_read_moved rejects NULL, a bad channel and a detached instance");
	CHECK(ops->hook_read_moved(NULL, 0, &offhook, &moved) == -EINVAL);
	CHECK(ops->hook_read_moved(s, 0, NULL, &moved) == -EINVAL);
	CHECK(ops->hook_read_moved(s, 0, &offhook, NULL) == -EINVAL);
	CHECK(ops->hook_read_moved(s, 2, &offhook, &moved) == -EINVAL);
	ops->detach(s);
	CHECK(ops->hook_read_moved(s, 0, &offhook, &moved) == -EINVAL);
}

int main(void)
{
	const struct udsp_slic_ops *ops = ops_get();

	CASE("descriptor abi and struct size match the contract");
	{
		CHECK_UINT(udsp_slic_fake_descriptor.abi_version, UDSP_SLIC_ABI_VERSION);
		CHECK_UINT(udsp_slic_fake_descriptor.struct_size,
			   sizeof(struct udsp_slic_ops));
		CHECK(ops != NULL);
	}

	CASE("descriptor exposes every mandatory op");
	{
		CHECK(ops->name != NULL);
		CHECK(ops->attach != NULL);
		CHECK(ops->detach != NULL);
		CHECK(ops->init != NULL);
		CHECK(ops->linefeed_set != NULL);
		CHECK(ops->ring_start != NULL);
		CHECK(ops->ring_stop != NULL);
		CHECK(ops->hook_read != NULL);
		CHECK(ops->fault_read != NULL);
		CHECK(ops->gain_set != NULL);
		CHECK(ops->safe_xfer != NULL);
	}

	CASE("attach reports the settable channel count");
	{
		unsigned n = 0;
		struct udsp_slic *s = attach_fresh(1, &n);

		CHECK(s != NULL);
		CHECK_UINT(n, 1);
		ops->detach(s);

		n = 0;
		s = attach_fresh(4, &n);
		CHECK(s != NULL);
		CHECK_UINT(n, 4);
		ops->detach(s);

		n = 0;
		udsp_slic_fake_reset();
		s = ops->attach(udsp_slic_fake_host_get(), &n);
		CHECK(s != NULL);
		CHECK_UINT(n, 2);
		ops->detach(s);
	}

	CASE("attach rejects a missing channels pointer and a bad count");
	{
		udsp_slic_fake_reset();
		CHECK(ops->attach(udsp_slic_fake_host_get(), NULL) == NULL);

		unsigned n = 0;
		struct udsp_slic *s = attach_fresh(UDSP_SLIC_FAKE_CHAN_MAX + 1, &n);

		CHECK(s == NULL);
		CHECK_UINT(n, 0);
	}

	CASE("attach runs without a host");
	{
		unsigned n = 0;

		udsp_slic_fake_reset();

		struct udsp_slic *s = ops->attach(NULL, &n);

		CHECK(s != NULL);
		CHECK_UINT(n, 2);
		if (s)
			ops->detach(s);
	}

	CASE("attach rejects a host without the bus primitive");
	{
		struct udsp_slic_host broken = { 0 };
		struct udsp_slic_host partial = *udsp_slic_fake_host_get();
		struct udsp_slic_host failing = *udsp_slic_fake_host_get();
		unsigned n = 0;

		struct udsp_slic *bad[3];

		udsp_slic_fake_reset();
		partial.spi_xfer = NULL;
		failing.spi_xfer = host_spi_xfer_fail;

		bad[0] = ops->attach(&broken, &n);
		bad[1] = ops->attach(&partial, &n);
		bad[2] = ops->attach(&failing, &n);

		/* A wrong attach would hold a slot and spoil every later case,
		 * because reset() deliberately detaches nothing. */
		for (unsigned i = 0; i < 3; i++) {
			CHECK(bad[i] == NULL);
			if (bad[i])
				ops->detach(bad[i]);
		}
		CHECK_UINT(n, 0);

		struct udsp_slic *s = ops->attach(udsp_slic_fake_host_get(), &n);

		CHECK(s != NULL);
		CHECK_UINT(n, 2);
		ops->detach(s);
	}

	CASE("attach probes REG0 through the one bus primitive");
	{
		uint8_t tx[UDSP_SLIC_FAKE_TX_MAX];
		unsigned n = 0;
		unsigned calls = 0;
		struct udsp_slic *s;

		udsp_slic_fake_reset();
		CHECK_UINT(udsp_slic_fake_host_tx_get(NULL, 0, &calls), 0);
		CHECK_UINT(calls, 0);

		s = ops->attach(udsp_slic_fake_host_get(), &n);
		CHECK(s != NULL);

		unsigned len = udsp_slic_fake_host_tx_get(tx, sizeof(tx), &calls);

		CHECK_UINT(calls, 1);
		CHECK_UINT(len, 3);
		CHECK((tx[0] & 0x80u) != 0);	/* a read frame */
		CHECK_UINT(tx[1], 0);		/* of REG0 */

		ops->detach(s);

		udsp_slic_fake_reset();
		s = ops->attach(NULL, &n);
		CHECK(s != NULL);
		CHECK_UINT(udsp_slic_fake_host_tx_get(NULL, 0, &calls), 0);
		CHECK_UINT(calls, 0);		/* no host, no bus traffic */
		if (s)
			ops->detach(s);
	}

	CASE("the safe transfer covers every attached channel");
	{
		const unsigned counts[] = { 1, 2, 4 };

		for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
			unsigned n = 0;
			struct udsp_slic *s = attach_fresh(counts[i], &n);

			CHECK(s != NULL);
			CHECK_UINT(n, counts[i]);
			CHECK(safe_covers_channels(s, counts[i]));
			ops->detach(s);
		}
	}

	CASE("linefeed round trips through the recorder");
	{
		unsigned n = 0;
		struct udsp_slic *s = attach_fresh(2, &n);
		struct udsp_slic_fake_chan_state st;

		CHECK(ops->linefeed_set(s, 1, UDSP_LF_FWD_OHT) == 0);
		CHECK(ops->linefeed_set(s, 1, UDSP_LF_RINGING) == 0);
		CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
		CHECK_UINT(st.linefeed, UDSP_LF_RINGING);
		CHECK_UINT(st.linefeed_count, 2);

		CHECK(udsp_slic_fake_chan_state_get(s, 0, &st));
		CHECK_UINT(st.linefeed, UDSP_LF_OPEN);
		CHECK_UINT(st.linefeed_count, 0);
		ops->detach(s);
	}

	CASE("linefeed rejects a value outside the contract enum");
	{
		unsigned n = 0;
		struct udsp_slic *s = attach_fresh(2, &n);
		struct udsp_slic_fake_chan_state st;

		CHECK(ops->linefeed_set(s, 0, (enum udsp_linefeed)(UDSP_LF_REV_OHT + 1)) < 0);
		CHECK(udsp_slic_fake_chan_state_get(s, 0, &st));
		CHECK_UINT(st.linefeed_count, 0);
		ops->detach(s);
	}

	CASE("ring start and stop update the recorder counts");
	{
		unsigned n = 0;
		struct udsp_slic *s = attach_fresh(2, &n);
		struct udsp_slic_fake_chan_state st;

		CHECK(ops->ring_start(s, 0) == 0);
		CHECK(udsp_slic_fake_chan_state_get(s, 0, &st));
		CHECK(st.ringing);

		CHECK(ops->ring_stop(s, 0) == 0);
		CHECK(udsp_slic_fake_chan_state_get(s, 0, &st));
		CHECK(!st.ringing);
		CHECK_UINT(st.ring_start_count, 1);
		CHECK_UINT(st.ring_stop_count, 1);
		ops->detach(s);
	}

	CASE("the journal holds ring_stop after ring_start");
	{
		unsigned n = 0;
		struct udsp_slic *s = attach_fresh(2, &n);

		udsp_slic_fake_journal_clear(s);
		CHECK(ops->ring_start(s, 0) == 0);
		CHECK(ops->ring_stop(s, 0) == 0);

		unsigned lost = 1;
		unsigned len = udsp_slic_fake_journal_get(s, NULL, 0, &lost);

		CHECK_UINT(len, 2);
		CHECK_UINT(lost, 0);
		CHECK(journal_call_at(s, 0, UDSP_SLIC_FAKE_RING_START));
		CHECK(journal_call_at(s, 1, UDSP_SLIC_FAKE_RING_STOP));
		ops->detach(s);
	}

	CASE("the journal holds the whole bring-up sequence in order");
	{
		unsigned n = 0;
		struct udsp_slic *s = attach_fresh(2, &n);
		bool offhook = false;

		CHECK(ops->init(s, 0) == 0);
		CHECK(ops->linefeed_set(s, 0, UDSP_LF_FWD_ACTIVE) == 0);
		CHECK(ops->ring_start(s, 0) == 0);
		CHECK(ops->ring_stop(s, 0) == 0);
		CHECK(ops->hook_read(s, 0, &offhook) == 0);

		struct udsp_slic_fake_entry e[J_MAX];
		unsigned len = udsp_slic_fake_journal_get(s, e, J_MAX, NULL);

		CHECK_UINT(len, 6);
		CHECK(journal_call_at(s, 0, UDSP_SLIC_FAKE_ATTACH));
		CHECK(journal_call_at(s, 1, UDSP_SLIC_FAKE_INIT));
		CHECK(journal_call_at(s, 2, UDSP_SLIC_FAKE_LINEFEED_SET));
		CHECK(journal_call_at(s, 3, UDSP_SLIC_FAKE_RING_START));
		CHECK(journal_call_at(s, 4, UDSP_SLIC_FAKE_RING_STOP));
		CHECK(journal_call_at(s, 5, UDSP_SLIC_FAKE_HOOK_READ));
		CHECK_UINT(e[0].arg0, 2);
		CHECK_UINT(e[2].arg0, UDSP_LF_FWD_ACTIVE);
		ops->detach(s);
	}

	CASE("the journal saturates and counts the rows it lost");
	{
		const unsigned pairs = (UDSP_SLIC_FAKE_JOURNAL_MAX / 2) + 3;
		unsigned n = 0;
		struct udsp_slic *s = attach_fresh(2, &n);
		struct udsp_slic_fake_entry e[UDSP_SLIC_FAKE_JOURNAL_MAX + 4];
		struct udsp_slic_fake_entry part_buf[6];
		unsigned lost = 0;

		udsp_slic_fake_journal_clear(s);
		for (unsigned i = 0; i < pairs; i++) {
			CHECK(ops->ring_start(s, 0) == 0);
			CHECK(ops->ring_stop(s, 0) == 0);
		}

		memset(e, 0xff, sizeof(e));

		unsigned len = udsp_slic_fake_journal_get(s, e, UDSP_SLIC_FAKE_JOURNAL_MAX + 4,
							 &lost);
		bool order_ok = true;

		CHECK_UINT(len, UDSP_SLIC_FAKE_JOURNAL_MAX);
		CHECK_UINT(lost, pairs * 2 - UDSP_SLIC_FAKE_JOURNAL_MAX);

		for (unsigned i = 0; i < UDSP_SLIC_FAKE_JOURNAL_MAX; i++) {
			uint8_t want = (i % 2) ? UDSP_SLIC_FAKE_RING_STOP :
						 UDSP_SLIC_FAKE_RING_START;

			if (e[i].call != want)
				order_ok = false;
		}
		CHECK(order_ok);
		CHECK_UINT(e[UDSP_SLIC_FAKE_JOURNAL_MAX].call, 0xff);

		memset(part_buf, 0xff, sizeof(part_buf));
		CHECK_UINT(udsp_slic_fake_journal_get(s, part_buf, 4, NULL),
			   UDSP_SLIC_FAKE_JOURNAL_MAX);
		CHECK_UINT(part_buf[0].call, UDSP_SLIC_FAKE_RING_START);
		CHECK_UINT(part_buf[3].call, UDSP_SLIC_FAKE_RING_STOP);
		CHECK_UINT(part_buf[4].call, 0xff);

		udsp_slic_fake_journal_clear(s);
		lost = 7;
		CHECK_UINT(udsp_slic_fake_journal_get(s, NULL, 0, &lost), 0);
		CHECK_UINT(lost, 0);
		CHECK(ops->ring_stop(s, 0) == 0);
		CHECK(journal_call_at(s, 0, UDSP_SLIC_FAKE_RING_STOP));
		CHECK_UINT(udsp_slic_fake_journal_get(s, NULL, 0, NULL), 1);
		ops->detach(s);
	}

	CASE("hook state round trips through the recorder");
	{
		unsigned n = 0;
		struct udsp_slic *s = attach_fresh(2, &n);
		struct udsp_slic_fake_chan_state st;
		bool offhook = true;

		CHECK(ops->hook_read(s, 1, &offhook) == 0);
		CHECK(!offhook);

		udsp_slic_fake_hook_set(s, 1, true);
		CHECK(ops->hook_read(s, 1, &offhook) == 0);
		CHECK(offhook);

		offhook = true;
		CHECK(ops->hook_read(s, 0, &offhook) == 0);
		CHECK(!offhook);

		CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
		CHECK_UINT(st.hook_read_count, 2);
		ops->detach(s);
	}

	CASE("fault bitmask round trips through the recorder");
	{
		const uint32_t bits = UDSP_FAULT_SLIC_THERMAL | UDSP_FAULT_SLIC_VBAT;
		unsigned n = 0;
		struct udsp_slic *s = attach_fresh(2, &n);
		struct udsp_slic_fake_chan_state st;
		uint32_t got = 0xffffffffu;

		CHECK(ops->fault_read(s, 0, &got) == 0);
		CHECK_UINT(got, 0);

		udsp_slic_fake_fault_set(s, 0, bits);
		CHECK(ops->fault_read(s, 0, &got) == 0);
		CHECK_UINT(got, bits);

		CHECK(udsp_slic_fake_chan_state_get(s, 0, &st));
		CHECK_UINT(st.fault_read_count, 2);
		CHECK_UINT(st.fault_bits, bits);
		ops->detach(s);
	}

	CASE("gain round trips through the recorder");
	{
		unsigned n = 0;
		struct udsp_slic *s = attach_fresh(2, &n);
		struct udsp_slic_fake_chan_state st;

		CHECK(ops->gain_set(s, 1, -6, 4) == 0);
		CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
		CHECK(st.gain_tx_half_db == -6);
		CHECK(st.gain_rx_half_db == 4);
		CHECK_UINT(st.gain_count, 1);
		ops->detach(s);
	}

	CASE("region_set round trips through the recorder");
	{
		unsigned n = 0;
		struct udsp_slic *s = attach_fresh(2, &n);
		struct udsp_slic_fake_chan_state st;
		struct udsp_slic_fake_entry e[J_MAX];
		unsigned len;

		udsp_slic_fake_journal_clear(s);
		CHECK(ops->region_set(s, 1, "USA") == 0);
		CHECK(ops->region_set(s, 1, "GBR") == 0);
		CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
		CHECK(strcmp(st.region_iso3, "GBR") == 0);
		CHECK_UINT(st.region_count, 2);

		CHECK(udsp_slic_fake_chan_state_get(s, 0, &st));
		CHECK_UINT(st.region_count, 0);
		CHECK_UINT(st.region_iso3[0], 0);

		len = udsp_slic_fake_journal_get(s, e, J_MAX, NULL);
		CHECK_UINT(len, 2);
		CHECK(journal_call_at(s, 0, UDSP_SLIC_FAKE_REGION_SET));
		CHECK(journal_call_at(s, 1, UDSP_SLIC_FAKE_REGION_SET));
		CHECK_UINT(e[0].chan, 1);
		CHECK_UINT(e[1].chan, 1);
		ops->detach(s);
	}

	CASE("an injected region_set error fires once and changes nothing");
	{
		unsigned n = 0;
		struct udsp_slic *s = attach_fresh(2, &n);
		struct udsp_slic_fake_chan_state st;

		CHECK(ops->region_set(s, 0, "USA") == 0);
		udsp_slic_fake_journal_clear(s);

		udsp_slic_fake_error_set(s, UDSP_SLIC_FAKE_REGION_SET, -EIO);
		CHECK(ops->region_set(s, 0, "GBR") == -EIO);
		CHECK(udsp_slic_fake_chan_state_get(s, 0, &st));
		CHECK(strcmp(st.region_iso3, "USA") == 0);
		CHECK_UINT(st.region_count, 1);
		CHECK_UINT(udsp_slic_fake_journal_get(s, NULL, 0, NULL), 0);

		CHECK(ops->region_set(s, 0, "GBR") == 0);
		CHECK(udsp_slic_fake_chan_state_get(s, 0, &st));
		CHECK(strcmp(st.region_iso3, "GBR") == 0);
		CHECK_UINT(st.region_count, 2);
		ops->detach(s);
	}

	CASE("every op rejects the channel past the attached count");
	{
		unsigned n = 0;
		struct udsp_slic *s = attach_fresh(2, &n);
		struct udsp_slic_mlt_result res;
		struct udsp_slic_fake_chan_state st;
		struct udsp_slic_monitor monitor;
		struct udsp_slic_converter converter;
		bool offhook = false;
		uint32_t bits = 0;
		const unsigned over = n;
		bool clean = true;

		udsp_slic_fake_journal_clear(s);
		CHECK(ops->init(s, over) == -EINVAL);
		CHECK(ops->linefeed_set(s, over, UDSP_LF_FWD_ACTIVE) == -EINVAL);
		CHECK(ops->ring_start(s, over) == -EINVAL);
		CHECK(ops->ring_stop(s, over) == -EINVAL);
		CHECK(ops->hook_read(s, over, &offhook) == -EINVAL);
		CHECK(ops->fault_read(s, over, &bits) == -EINVAL);
		CHECK(ops->gain_set(s, over, 3, 3) == -EINVAL);
		CHECK(ops->mwi_lamp(s, over, true) == -EINVAL);
		CHECK(ops->powersave(s, over, true) == -EINVAL);
		CHECK(ops->metering(s, over, UDSP_METER_12KHZ, 1, 1, 1) == -EINVAL);
		CHECK(ops->loopback(s, over, true) == -EINVAL);
		CHECK(ops->mlt_run(s, over, 1, &res) == -EINVAL);
		CHECK(ops->region_set(s, over, "USA") == -EINVAL);
		CHECK(ops->reinit(s, over) == -EINVAL);
		CHECK(ops->converter_restart(s, over) == -EINVAL);
		CHECK(ops->monitor_read(s, over, &monitor) == -EINVAL);
		CHECK(ops->converter_read(s, over, &converter) == -EINVAL);
		CHECK(ops->init(s, UDSP_SLIC_FAKE_CHAN_MAX) == -EINVAL);
		CHECK(ops->init(s, ~0u) == -EINVAL);
		CHECK_UINT(udsp_slic_fake_journal_get(s, NULL, 0, NULL), 0);

		for (unsigned c = 0; c < n; c++) {
			if (!udsp_slic_fake_chan_state_get(s, c, &st))
				clean = false;
			else if (st.inited || st.ringing || st.linefeed != UDSP_LF_OPEN ||
				 st.init_count || st.linefeed_count || st.ring_start_count ||
				 st.ring_stop_count || st.hook_read_count ||
				 st.fault_read_count || st.gain_count || st.mwi_count ||
				 st.powersave_count || st.metering_count ||
				 st.loopback_count || st.mlt_count ||
				 st.reinit_count ||
				 st.converter_restart_count ||
				 st.monitor_count ||
				 st.converter_read_count)
				clean = false;
		}
		CHECK(clean);
		ops->detach(s);
	}

	CASE("a NULL out pointer is rejected");
	{
		unsigned n = 0;
		struct udsp_slic *s = attach_fresh(2, &n);

		CHECK(ops->hook_read(s, 0, NULL) < 0);
		CHECK(ops->fault_read(s, 0, NULL) < 0);
		CHECK(ops->region_set(s, 0, NULL) < 0);
		CHECK(ops->monitor_read(s, 0, NULL) == -EINVAL);
		CHECK(ops->converter_read(s, 0, NULL) == -EINVAL);
		CHECK_UINT(udsp_slic_fake_journal_get(s, NULL, 0, NULL), 1);
		ops->detach(s);
	}

	CASE("the nullable ops record when they are present");
	{
		unsigned n = 0;
		struct udsp_slic *s = attach_fresh(2, &n);
		struct udsp_slic_fake_chan_state st;
		struct udsp_slic_monitor monitor;
		struct udsp_slic_converter converter;

		CHECK(ops->mwi_lamp != NULL);
		CHECK(ops->powersave != NULL);
		CHECK(ops->metering != NULL);
		CHECK(ops->loopback != NULL);
		CHECK(ops->mlt_run != NULL);
		CHECK(ops->region_set != NULL);
		CHECK(ops->reinit != NULL);
		CHECK(ops->converter_restart != NULL);
		CHECK(ops->monitor_read != NULL);
		CHECK(ops->converter_read != NULL);

		CHECK(ops->mwi_lamp(s, 0, true) == 0);
		CHECK(ops->powersave(s, 0, true) == 0);
		CHECK(ops->loopback(s, 0, true) == 0);
		CHECK(ops->metering(s, 0, UDSP_METER_16KHZ, 200, 800, 3) == 0);
		CHECK(ops->reinit(s, 0) == 0);
		CHECK(ops->converter_restart(s, 0) == 0);
		CHECK(ops->monitor_read(s, 0, &monitor) == 0);
		CHECK(ops->converter_read(s, 0, &converter) == 0);

		CHECK(udsp_slic_fake_chan_state_get(s, 0, &st));
		CHECK(st.mwi_on);
		CHECK(st.powersave_on);
		CHECK(st.loopback_on);
		CHECK_UINT(st.meter_freq, UDSP_METER_16KHZ);
		CHECK_UINT(st.meter_on_ms, 200);
		CHECK_UINT(st.meter_off_ms, 800);
		CHECK_UINT(st.meter_count, 3);
		CHECK_UINT(st.metering_count, 1);
		CHECK_UINT(st.reinit_count, 1);
		CHECK_UINT(st.converter_restart_count, 1);
		CHECK_UINT(st.monitor_count, 1);
		CHECK_UINT(st.converter_read_count, 1);
		ops->detach(s);
	}

	CASE("metering rejects a frequency outside the contract enum");
	{
		unsigned n = 0;
		struct udsp_slic *s = attach_fresh(2, &n);

		CHECK(ops->metering(s, 0, (enum udsp_slic_meter_freq)7, 1, 1, 1) < 0);
		ops->detach(s);
	}

	CASE("mlt_run returns the opaque result unchanged");
	{
		unsigned n = 0;
		struct udsp_slic *s = attach_fresh(2, &n);
		struct udsp_slic_mlt_result res;
		struct udsp_slic_fake_chan_state st;

		memset(&res, 0xff, sizeof(res));
		CHECK(ops->mlt_run(s, 1, 0x20, &res) == 0);
		CHECK_UINT(res.test_id, 0x20);
		CHECK(res.len > 0);
		CHECK(res.len <= sizeof(res.data));
		/* The fake's own head: the test id at byte 1 and a clear
		 * status byte, then the channel's call count. */
		CHECK_UINT(res.data[0], 0);
		CHECK_UINT(res.data[1], 0x20);
		CHECK_UINT(res.data[2], 0);
		CHECK_UINT(res.data[8], 1);
		CHECK_UINT(res.data[9], 0);

		CHECK(udsp_slic_fake_chan_state_get(s, 1, &st));
		CHECK_UINT(st.mlt_test_id, 0x20);
		CHECK_UINT(st.mlt_count, 1);
		ops->detach(s);
	}

	CASE("clearing a nullable op leaves the ops pointer NULL");
	{
		udsp_slic_fake_reset();
		udsp_slic_fake_optional_set(UDSP_SLIC_FAKE_OPT_ALL &
					    ~(uint32_t)UDSP_SLIC_FAKE_OPT_MWI_LAMP);

		CHECK(ops_get()->mwi_lamp == NULL);
		CHECK(ops_get()->powersave != NULL);
		CHECK(ops_get()->metering != NULL);
		CHECK(ops_get()->loopback != NULL);
		CHECK(ops_get()->mlt_run != NULL);
		CHECK(ops_get()->reinit != NULL);
		CHECK(ops_get()->converter_restart != NULL);
		CHECK(ops_get()->monitor_read != NULL);
		CHECK(ops_get()->converter_read != NULL);

		udsp_slic_fake_optional_set(0);
		CHECK(ops_get()->mwi_lamp == NULL);
		CHECK(ops_get()->powersave == NULL);
		CHECK(ops_get()->metering == NULL);
		CHECK(ops_get()->loopback == NULL);
		CHECK(ops_get()->mlt_run == NULL);
		CHECK(ops_get()->region_set == NULL);
		CHECK(ops_get()->reinit == NULL);
		CHECK(ops_get()->converter_restart == NULL);
		CHECK(ops_get()->monitor_read == NULL);
		CHECK(ops_get()->converter_read == NULL);

		udsp_slic_fake_reset();
		CHECK(ops_get()->mwi_lamp != NULL);
		CHECK(ops_get()->mlt_run != NULL);
	}

	CASE("clearing the region op alone leaves the others standing");
	{
		udsp_slic_fake_reset();
		udsp_slic_fake_optional_set(UDSP_SLIC_FAKE_OPT_ALL &
					    ~(uint32_t)UDSP_SLIC_FAKE_OPT_REGION);

		CHECK(ops_get()->region_set == NULL);
		CHECK(ops_get()->mwi_lamp != NULL);
		CHECK(ops_get()->powersave != NULL);
		CHECK(ops_get()->loopback != NULL);

		udsp_slic_fake_reset();
		CHECK(ops_get()->region_set != NULL);
	}

	CASE("the shared host serves the bus, gpio and clock hooks");
	{
		const struct udsp_slic_host *h = udsp_slic_fake_host_get();
		const uint8_t out[] = { 0x01, 0x40, 0x5a, 0x02, 0x41, 0x5b };
		uint8_t back[UDSP_SLIC_FAKE_TX_MAX];
		uint8_t rx[6] = { 0 };
		unsigned calls = 0;
		int level = -1;

		udsp_slic_fake_reset();
		CHECK(h->spi_xfer != NULL);

		udsp_slic_fake_host_rx_set(0xa5);
		CHECK(h->spi_xfer(h->ctx, out, rx, sizeof(out)) == 0);
		CHECK_UINT(rx[0], 0xa5);
		CHECK_UINT(rx[5], 0xa5);

		CHECK_UINT(udsp_slic_fake_host_tx_get(back, sizeof(back), &calls),
			   sizeof(out));
		CHECK_UINT(calls, 1);
		CHECK(memcmp(back, out, sizeof(out)) == 0);

		CHECK(h->spi_xfer(h->ctx, out, NULL, sizeof(out)) == 0);  /* write only */
		CHECK(h->spi_xfer(h->ctx, NULL, rx, sizeof(out)) < 0);
		CHECK(h->spi_xfer(h->ctx, out, rx, 0) < 0);

		uint32_t t0 = h->mono_ms(h->ctx);

		h->delay_ms(h->ctx, 25);
		CHECK_UINT(h->mono_ms(h->ctx) - t0, 25);

		CHECK(h->gpio_set(h->ctx, "slic_reset", 1) == 0);
		CHECK(h->gpio_get(h->ctx, "slic_reset", &level) == 0);
		CHECK_UINT(level, 1);
		CHECK(h->gpio_get(h->ctx, "nonesuch", &level) < 0);

		CHECK(h->sem(h->ctx, 1) == 0);
		CHECK(h->sem(h->ctx, 0) == 0);
		CHECK(h->sem(h->ctx, 0) < 0);
	}

	CASE("two instances live at once and both slots come back");
	{
		struct udsp_slic *s[UDSP_SLIC_FAKE_INST_MAX];
		struct udsp_slic_fake_chan_state st;
		unsigned n = 0;

		udsp_slic_fake_reset();
		for (unsigned i = 0; i < UDSP_SLIC_FAKE_INST_MAX; i++) {
			s[i] = ops->attach(udsp_slic_fake_host_get(), &n);
			CHECK(s[i] != NULL);
			for (unsigned j = 0; j < i; j++)
				CHECK(s[i] != s[j]);
		}

		CHECK(ops->attach(udsp_slic_fake_host_get(), &n) == NULL);

		CHECK(ops->init(s[0], 0) == 0);
		CHECK(udsp_slic_fake_chan_state_get(s[UDSP_SLIC_FAKE_INST_MAX - 1], 0, &st));
		CHECK_UINT(st.init_count, 0);

		for (unsigned i = 0; i < UDSP_SLIC_FAKE_INST_MAX; i++)
			ops->detach(s[i]);

		for (unsigned i = 0; i < UDSP_SLIC_FAKE_INST_MAX; i++) {
			s[i] = ops->attach(udsp_slic_fake_host_get(), &n);
			CHECK(s[i] != NULL);
		}
		for (unsigned i = 0; i < UDSP_SLIC_FAKE_INST_MAX; i++)
			ops->detach(s[i]);
	}

	CASE("detach releases the instance for the next attach");
	{
		unsigned n = 0;
		struct udsp_slic *s = attach_fresh(2, &n);

		CHECK(ops->init(s, 0) == 0);
		ops->detach(s);

		for (unsigned i = 0; i < UDSP_SLIC_FAKE_INST_MAX + 2; i++) {
			unsigned again = 0;
			struct udsp_slic *s2 = ops->attach(udsp_slic_fake_host_get(), &again);
			struct udsp_slic_fake_chan_state st;

			CHECK(s2 != NULL);
			CHECK_UINT(again, 2);
			CHECK(udsp_slic_fake_chan_state_get(s2, 0, &st));
			CHECK_UINT(st.init_count, 0);
			CHECK_UINT(udsp_slic_fake_journal_get(s2, NULL, 0, NULL), 1);
			ops->detach(s2);
		}
	}

	/*
	 * caps_get answers for the whole part, so it takes no channel and needs
	 * no init (slic.h). udsp_open reads it right after the attach to settle
	 * the line rate, and the toggle is what lets a case drive the refusal
	 * arm of that settle.
	 */
	CASE("caps_get reports wideband and follows the toggle");
	{
		unsigned n = 0;
		struct udsp_slic *s = attach_fresh(2, &n);

		CHECK(ops->caps_get != NULL);
		CHECK(s != NULL);
		if (s && ops->caps_get) {
			CHECK_UINT(ops->caps_get(s) & UDSP_SLIC_CAP_WIDEBAND,
				   UDSP_SLIC_CAP_WIDEBAND);
			udsp_slic_fake_wideband_set(false);
			CHECK_UINT(ops->caps_get(s), 0);
			/* The answer follows the toggle, not the instance, so an
			 * open that already attached still sees the change. */
			udsp_slic_fake_wideband_set(true);
			CHECK_UINT(ops->caps_get(s) & UDSP_SLIC_CAP_WIDEBAND,
				   UDSP_SLIC_CAP_WIDEBAND);
			ops->detach(s);
			/* A detached instance answers nothing. */
			CHECK_UINT(ops->caps_get(s), 0);
		}
	}

	CASE("the reset restores the wideband default");
	{
		unsigned n = 0;
		struct udsp_slic *s;

		udsp_slic_fake_wideband_set(false);
		s = attach_fresh(1, &n);
		CHECK(s != NULL);
		if (s) {
			CHECK_UINT(ops->caps_get(s) & UDSP_SLIC_CAP_WIDEBAND,
				   UDSP_SLIC_CAP_WIDEBAND);
			ops->detach(s);
		}
	}

	CASE("clearing the caps bit leaves the op NULL");
	{
		udsp_slic_fake_reset();
		udsp_slic_fake_optional_set(UDSP_SLIC_FAKE_OPT_ALL &
					    ~(uint32_t)UDSP_SLIC_FAKE_OPT_CAPS);
		CHECK(ops->caps_get == NULL);
		/* The other nullable ops keep their pointers. */
		CHECK(ops->region_set != NULL);
		CHECK(ops->mwi_lamp != NULL);
		udsp_slic_fake_reset();
		CHECK(ops->caps_get != NULL);
	}

	CASE("caps_get writes no journal row");
	{
		unsigned n = 0;
		struct udsp_slic *s = attach_fresh(2, &n);

		CHECK(s != NULL);
		if (s) {
			unsigned before = udsp_slic_fake_journal_get(s, NULL, 0,
								     NULL);

			(void)ops->caps_get(s);
			CHECK_UINT(udsp_slic_fake_journal_get(s, NULL, 0, NULL),
				   before);
			ops->detach(s);
		}
	}

	recovery_cases_run();
	repeat_error_cases_run();
	monitor_cases_run();
	monitor_error_cases_run();
	abi7_cases_run();
	abi8_cases_run();

	return tu_summary("slic_fake");
}
