// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Private control surface of the in-tree fake SLIC (slic/fake.c). This header
 * is not the public API: only libudsp itself, its own tests and udsp-cli
 * include it, and it changes with the fake. The stable contract stays
 * include/udsp/slic.h.
 *
 * Name. The fake is linked into libudsp, never dlopened, so it exports
 * udsp_slic_fake_descriptor and leaves the reserved plug-in symbol
 * udsp_slic_descriptor to the proprietary plug-in that libudsp dlsym's
 * (slic.h).
 *
 * Every op records what the caller asked for, as the uvoice stub HAL backend
 * does: the fake validates ranges, because an out-of-range channel would write
 * out of bounds, but it imposes no state policy of its own. A test reads the
 * per-channel record and the call journal, and sets the hook state, the fault
 * bits and the probed channel count, so the engine's paths run with no
 * hardware.
 *
 * Threads. Ten entry points are safe while another thread runs the ops.
 * udsp_slic_fake_fault_set, udsp_slic_fake_error_set,
 * udsp_slic_fake_error_repeat_set, udsp_slic_fake_delay_set and
 * udsp_slic_fake_mlt_len_set write atomic injection cells that the ops load.
 * An engine test therefore injects from its control thread while the slic
 * thread samples. udsp_slic_fake_hook_set and udsp_slic_fake_hook_inject write
 * the hook level and its toggle count under the instance lock that the ops
 * read them under.
 *
 * udsp_slic_fake_chan_state_get copies the per-channel record under the same
 * lock, which the ops hold while they write the record. A test therefore reads
 * a whole record while the slic thread samples. udsp_slic_fake_monitor_set
 * writes the monitor cell under that lock. udsp_slic_fake_journal_get copies a
 * snapshot: an op writes its row before it publishes the new length, so a
 * reader has every row that the length covers. Every other entry point is
 * control-thread-only, and that includes udsp_slic_fake_journal_clear.
 *
 * Quiescence. A snapshot can stop in the middle of a job. An engine test
 * therefore reads the journal or the per-channel record only when no job is
 * in flight, or after the udsp_stop completion. The job counter is the release
 * point that the test acquires (test_core.c tc_quiesce).
 */
#ifndef UDSP_SLIC_FAKE_H
#define UDSP_SLIC_FAKE_H

#include <stdbool.h>
#include <stdint.h>

#include "udsp/slic.h"

#define UDSP_SLIC_FAKE_CHAN_MAX		8	/* as UDSP_LINE_MAX in udsp.h */
#define UDSP_SLIC_FAKE_TX_MAX		64	/* bytes kept of the last transfer */
#define UDSP_SLIC_FAKE_INST_MAX		2
#define UDSP_SLIC_FAKE_JOURNAL_MAX	64

enum udsp_slic_fake_call {
	UDSP_SLIC_FAKE_NONE = 0,
	UDSP_SLIC_FAKE_ATTACH,
	UDSP_SLIC_FAKE_INIT,
	UDSP_SLIC_FAKE_LINEFEED_SET,
	UDSP_SLIC_FAKE_RING_START,
	UDSP_SLIC_FAKE_RING_STOP,
	UDSP_SLIC_FAKE_HOOK_READ,
	UDSP_SLIC_FAKE_FAULT_READ,
	UDSP_SLIC_FAKE_GAIN_SET,
	UDSP_SLIC_FAKE_MWI_LAMP,
	UDSP_SLIC_FAKE_POWERSAVE,
	UDSP_SLIC_FAKE_METERING,
	UDSP_SLIC_FAKE_LOOPBACK,
	UDSP_SLIC_FAKE_MLT_RUN,
	UDSP_SLIC_FAKE_SAFE_XFER,
	UDSP_SLIC_FAKE_REGION_SET,
	UDSP_SLIC_FAKE_REINIT,
	UDSP_SLIC_FAKE_CONVERTER_RESTART,
	UDSP_SLIC_FAKE_MONITOR_READ,
	UDSP_SLIC_FAKE_CONVERTER_READ,
	UDSP_SLIC_FAKE_HOOK_READ_MOVED,
	UDSP_SLIC_FAKE_CALL_MAX,
};

/* Which nullable ops the descriptor exposes. slic.h: a NULL op means the
 * capability is absent, so a test clears a bit to check the engine's gating. */
enum udsp_slic_fake_opt {
	UDSP_SLIC_FAKE_OPT_MWI_LAMP	= (1u << 0),
	UDSP_SLIC_FAKE_OPT_POWERSAVE	= (1u << 1),
	UDSP_SLIC_FAKE_OPT_METERING	= (1u << 2),
	UDSP_SLIC_FAKE_OPT_LOOPBACK	= (1u << 3),
	UDSP_SLIC_FAKE_OPT_MLT		= (1u << 4),
	UDSP_SLIC_FAKE_OPT_REGION	= (1u << 5),
	/* caps_get is nullable too. udsp_open must read a NULL one as "no
	 * wideband". */
	UDSP_SLIC_FAKE_OPT_CAPS		= (1u << 6),
	/* A cleared bit is the shape of a plug-in that offers no recovery or
	 * no monitor, which the engine must hold rather than recover, and
	 * report no row for. */
	UDSP_SLIC_FAKE_OPT_REINIT	= (1u << 7),
	UDSP_SLIC_FAKE_OPT_CONVERTER	= (1u << 8),	/* converter_restart */
	UDSP_SLIC_FAKE_OPT_MONITOR	= (1u << 9),
	UDSP_SLIC_FAKE_OPT_CONVERTER_READ = (1u << 10),
	/* A cleared bit is the shape of a plug-in with no change record,
	 * which the engine samples by level alone. */
	UDSP_SLIC_FAKE_OPT_HOOK_MOVED	= (1u << 11),
	UDSP_SLIC_FAKE_OPT_ALL		= 0xfffu,
};

/* One journal row. arg0 and arg1 carry the op's own arguments: the linefeed,
 * the hook or fault value the op returned, the two gains, the metering
 * frequency and its count, the MLT test id. */
struct udsp_slic_fake_entry {
	uint8_t call;			/* enum udsp_slic_fake_call */
	uint8_t chan;
	int32_t arg0;
	int32_t arg1;
};

struct udsp_slic_fake_chan_state {
	bool inited;
	bool ringing;
	bool offhook;			/* settable: what hook_read returns */
	bool mwi_on;
	bool powersave_on;
	bool loopback_on;
	uint8_t linefeed;		/* enum udsp_linefeed, last set */
	uint8_t meter_freq;		/* enum udsp_slic_meter_freq, last set */
	uint8_t mlt_test_id;
	char region_iso3[8];		/* last region_set name, truncated copy */
	int16_t gain_tx_half_db;
	int16_t gain_rx_half_db;
	uint32_t fault_bits;		/* settable: what fault_read returns */
	uint32_t meter_on_ms;
	uint32_t meter_off_ms;
	uint32_t meter_count;
	unsigned init_count;
	unsigned linefeed_count;
	unsigned ring_start_count;
	unsigned ring_stop_count;
	unsigned hook_read_count;
	unsigned fault_read_count;
	unsigned gain_count;
	unsigned mwi_count;
	unsigned powersave_count;
	unsigned metering_count;
	unsigned loopback_count;
	unsigned mlt_count;
	unsigned region_count;
	/* reinit clears every fault bit and converter_restart the DCDC
	 * bit alone, in this record and in the injected fault cell
	 * (udsp_slic_fake_fault_set), the way the part's own recovery clears
	 * its alarm; a test that wants a fault to outlive a successful op
	 * re-arms the cell after it observed the op's count move, never
	 * while the op may run. Both leave linefeed at UDSP_LF_OPEN. Both
	 * reads copy from the one monitor cell. An armed errno on any of
	 * the four changes nothing, counts nothing and journals nothing,
	 * the rule every other op follows. */
	unsigned reinit_count;
	unsigned converter_restart_count;
	unsigned monitor_count;
	unsigned converter_read_count;
	/* hook_read_moved reports the injected level and whether the
	 * level was toggled since its previous call, then clears the toggle
	 * count; the journal row carries the level in arg0 and the answer in
	 * arg1. It counts in hook_read_count as well, and the error and delay
	 * arms of UDSP_SLIC_FAKE_HOOK_READ govern it, so a case that counts
	 * the sampling cadence or kills a line with hook read failures reads
	 * and arms one cell whichever op the engine has.
	 * udsp_slic_fake_hook_inject adds toggles the level does not show,
	 * the shape of a pair of transitions inside one sample interval or of
	 * the part's race between its two reads. */
	unsigned hook_moved_count;
};

extern const struct udsp_slic_descriptor udsp_slic_fake_descriptor;

/* Defaults for the next attach, plus the shared bus shadow. reset restores
 * two channels and every nullable op, and clears the bus; it detaches
 * nothing. */
void udsp_slic_fake_reset(void);
void udsp_slic_fake_channels_set(unsigned channels);
void udsp_slic_fake_optional_set(uint32_t opt_mask);
/* What caps_get answers. True, the reset default, reports
 * UDSP_SLIC_CAP_WIDEBAND, so host CI reaches the 16 kHz line; false drives the
 * rate degrade. */
void udsp_slic_fake_wideband_set(bool on);

/* A host with no bus behind it. spi_xfer is the one bus primitive: it keeps the
 * last transfer's tx bytes and fills rx with a canned value, because a dumb
 * full-duplex transfer has no registers to shadow. delay_ms advances a virtual
 * clock so no test sleeps; gpio and sem succeed for the slic_reset and
 * slic_interrupt names. udsp-cli and the tests share this one instead of each
 * carrying a copy. */
const struct udsp_slic_host *udsp_slic_fake_host_get(void);

/* Bus recorder of that host. tx_get copies up to max bytes of the last
 * transfer (out may be NULL to measure only) and returns its true length;
 * *calls, when not NULL, takes the number of transfers since the last reset.
 * rx_set fills every rx byte of the following transfers with value. */
unsigned udsp_slic_fake_host_tx_get(uint8_t *out, unsigned max, unsigned *calls);
void udsp_slic_fake_host_rx_set(uint8_t value);

/* Per-instance controls, safe from another thread. An out-of-range channel is
 * a no-op. */
void udsp_slic_fake_hook_set(struct udsp_slic *s, unsigned chan, bool offhook);
/* The level plus toggles the change record counts beyond what the level
 * shows: hook_set is inject with one toggle when the level moves and none
 * when it does not. Level and count move under the one lock the ops read
 * them under, so a read never sees the level of one injection with the count
 * of another. */
void udsp_slic_fake_hook_inject(struct udsp_slic *s, unsigned chan, bool offhook,
				unsigned toggles);
void udsp_slic_fake_fault_set(struct udsp_slic *s, unsigned chan, uint32_t bits);
/* Arms one failure: the next call of op returns errno_neg and changes nothing,
 * then the arm is spent. errno_neg 0 disarms. Only ops that return int can be
 * armed; the check it exists for is "a SYNC-SLIC op returns the real errno".
 * mlt_run is the one op that still writes its caller's record: it reports the
 * armed errno in the status byte of a head-only record, as a part that ends a
 * run early does. */
void udsp_slic_fake_error_set(struct udsp_slic *s, enum udsp_slic_fake_call op,
			      int errno_neg);
/* The same arm for the next count calls of op, so an engine test can drive a
 * recovery that fails a fixed number of times before it succeeds without
 * re-arming from another thread between attempts. count 1 is
 * udsp_slic_fake_error_set; count 0 disarms; errno_neg 0 disarms. Safe from
 * another thread, as error_set is. */
void udsp_slic_fake_error_repeat_set(struct udsp_slic *s,
				     enum udsp_slic_fake_call op,
				     int errno_neg, unsigned count);
/* Makes every later call of op sleep ms milliseconds before it does its work,
 * which is the time a real op spends on the line: the metallic line test takes
 * seconds. The arm stands until it is cleared with 0, the default of a fresh
 * instance. An armed error ends the call before the sleep. */
void udsp_slic_fake_delay_set(struct udsp_slic *s, enum udsp_slic_fake_call op,
			      unsigned ms);

/* Makes mlt_run report a length it did not write, so a caller's bound check on
 * that length is reachable. A negative value, the reset default, reports the
 * true length. */
void udsp_slic_fake_mlt_len_set(int len);

/* What monitor_read copies out for chan, and what converter_read copies its
 * three words from; safe from another thread (the cell is written under the
 * instance lock the ops read it under). A fresh instance holds all zeros.
 * -ENODATA is armed through udsp_slic_fake_error_set on
 * UDSP_SLIC_FAKE_MONITOR_READ, as every other op's errno is. */
void udsp_slic_fake_monitor_set(struct udsp_slic *s, unsigned chan,
				const struct udsp_slic_monitor *m);

/* Per-instance readback, safe from another thread. false when s or chan is
 * invalid; *out is untouched. */
bool udsp_slic_fake_chan_state_get(struct udsp_slic *s, unsigned chan,
				   struct udsp_slic_fake_chan_state *out);

/* Copies up to max journal rows into out (out may be NULL to count only) and
 * returns the number of rows held. *lost, when not NULL, takes the number of
 * rows dropped after the journal filled. */
unsigned udsp_slic_fake_journal_get(struct udsp_slic *s,
				    struct udsp_slic_fake_entry *out,
				    unsigned max, unsigned *lost);
void udsp_slic_fake_journal_clear(struct udsp_slic *s);

#endif
