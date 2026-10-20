// SPDX-License-Identifier: BSD-2-Clause
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * This file stays BSD-2-Clause so a SLIC or fax plug-in and its tools
 * compile against it without LGPL obligations. The rest of libudsp is
 * LGPL-2.1-only.
 *
 * udsp SLIC plug-in contract. Stable contract: this header is the only thing
 * libudsp and the proprietary udsp-slic-proslic plug-in share. libudsp loads
 * the plug-in with dlopen and resolves one symbol, udsp_slic_descriptor, whose
 * abi_version and struct_size it validates before any call, so a stale
 * plug-in cannot expose incompatible offsets. No Skyworks ProSLIC header ever
 * reaches libudsp, so libudsp stays clear of the Skyworks license.
 *
 * Rules. The core cites these by number.
 *
 *   1. Thread affinity. Every ops call runs on the engine's slic thread,
 *      never on the uloop or engine thread. The ProSLIC API mutates global
 *      preset arrays and is not reentrant, and register access over spidev
 *      is slow. A FAST HAL op that needs a SLIC action posts it there and
 *      blocks on a bounded completion. caps_get is the one exception:
 *      udsp_open calls it on the control thread after the attach, before
 *      any line is up.
 *
 * Host hooks. The plug-in reaches the bus only through struct udsp_slic_host,
 * supplied at attach. The bus primitive is one dumb full-duplex SPI transfer
 * (spi_xfer); the plug-in owns all wire framing, register access and the
 * indirect-RAM sequences, exactly as it pre-frames the safe transfer. libudsp
 * therefore carries no chip knowledge at all: it is a spidev pipe, gpio, clock
 * and a semaphore. Batching is the plug-in's: one spi_xfer may carry many
 * framed operations, so a patch load is not one transfer per syscall.
 *
 * Crash safety. At attach the plug-in builds a pre-framed raw spidev transfer
 * that sets every line to linefeed-open and ring-off, and returns it through
 * safe_xfer as opaque bytes plus the spidev ioctl argument. The host's
 * async-signal-safe fatal handler blits it with one syscall and needs no
 * ProSLIC or MPI knowledge; the plug-in is never called from the handler.
 */
#ifndef UDSP_SLIC_H
#define UDSP_SLIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* libudsp accepts only this exact version (slic/plugin_load.h), so the
 * plug-in and libudsp ship in one image. */
#define UDSP_SLIC_ABI_VERSION	8
#define UDSP_SLIC_SAFE_MAX	256	/* bytes of the pre-framed safe transfer */

enum udsp_linefeed {
	UDSP_LF_OPEN = 0,
	UDSP_LF_FWD_ACTIVE,
	UDSP_LF_FWD_OHT,	/* forward, on-hook transmission: type 1 CID/VMWI */
	UDSP_LF_RINGING,
	UDSP_LF_REV_ACTIVE,	/* polarity reversed */
	UDSP_LF_REV_OHT,
};

/* Pre-framed raw spidev transfer for the async-signal-safe safe-state. */
struct udsp_slic_safe {
	uint8_t bytes[UDSP_SLIC_SAFE_MAX];
	size_t len;
};

/* The severity of one line the plug-in hands to the host's log hook. ERR is
 * a fault that leaves a channel dead, latched or unable to carry a call; WARN
 * a degraded outcome the line still runs with; INFO a one-time fact of the
 * attach; DEBUG the per-op trace and the vendored SDK's own messages. */
enum udsp_slic_log_level {
	UDSP_SLIC_LOG_ERR = 0,
	UDSP_SLIC_LOG_WARN,
	UDSP_SLIC_LOG_INFO,
	UDSP_SLIC_LOG_DEBUG,
};

struct udsp_slic_host {
	void *ctx;
	/* Full-duplex SPI transfer of len bytes; rx may be NULL for write-only.
	 * The one bus primitive. */
	int (*spi_xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
	void (*delay_ms)(void *ctx, unsigned ms);
	uint32_t (*mono_ms)(void *ctx);
	/* The canonical names "slic_reset" and "slic_interrupt" map to the
	 * configured sysfs exports; any other name passes through literally. */
	int (*gpio_set)(void *ctx, const char *name, int value);	/* slic_reset */
	int (*gpio_get)(void *ctx, const char *name, int *value);	/* slic_interrupt */
	int (*sem)(void *ctx, int enter);				/* critical section */
	/* Board fact, not a hook: the PCM highway's data offset in bit clocks
	 * after FSYNC. A nonzero value overrides the plug-in's profile
	 * default; zero selects that default (1 on the MediaTek profiles for
	 * the MT798x DSP_A delay, set by a listening test on MT7987; 0 on
	 * an7581, whose EN7581 engine starts slot 0 at bit 0). The
	 * plug-in adds the resolved value to every timeslot it programs. */
	uint8_t pcm_offset_bits;
	/* Config fact: nonzero asks for the wideband PCM mode, an effective
	 * 16 kHz rate where the part emits its second sample half a frame
	 * after the programmed timeslot (measured: pair {bit 0, bit 128} for
	 * slot 0). The timeslot programming is unchanged from narrowband, so
	 * line k keeps slot k and the pairs never collide. Zero keeps
	 * narrowband. Whether the part and profile can honour it is
	 * caps_get's answer. */
	uint8_t pcm_wideband;
	/* Text sink, appended. NULL means the host offers none and the
	 * plug-in keeps its own fallback. One line per call, without a
	 * trailing newline; the host decides where DEBUG goes and may drop
	 * it. Callable from attach and from every op that runs on the slic
	 * thread (rule 1), never from caps_get, which runs on the control
	 * thread, and never from the consumer of the safe transfer. */
	void (*log)(void *ctx, enum udsp_slic_log_level level, const char *msg);
};

enum udsp_slic_cap {
	/* init programs wideband on this part and profile when
	 * host->pcm_wideband asks for it. A property of the whole part,
	 * never of one channel: a per-channel init failure marks that
	 * channel dead and moves no rate. */
	UDSP_SLIC_CAP_WIDEBAND = (1 << 0),
};

/* MLT (metallic line test) POD result, filled by mlt_run. Opaque per-test
 * bytes the caller decodes; no ProSLIC type crosses the boundary. */
struct udsp_slic_mlt_result {
	uint8_t test_id;
	uint16_t len;
	uint8_t data[512];
};

enum udsp_slic_meter_freq { UDSP_METER_12KHZ = 0, UDSP_METER_16KHZ };

/* One reading of the analog line and its DC-DC converter, filled by
 * monitor_read. Voltages in mV, currents in uA, the HVIC power in mW; vdc_mv
 * is 0 when the plug-in has no VDC measurement, and the vendored ProSLIC
 * line monitor supplies none on either family. pd_dcdc and dcdc_status are
 * the raw 24-bit RAM words, lcrrtp and linefeed the raw registers: the
 * plug-in reads, the host decodes (bit 20 of pd_dcdc is the converter
 * powered down, bit 22 of dcdc_status the over-voltage lock-out). vbat_tgt_mv
 * reads 0 until the converter has come up; vbat_irq_th_mv is the battery
 * trip band the part holds. */
struct udsp_slic_monitor {
	int32_t vtr_mv;
	int32_t vtip_mv;
	int32_t vring_mv;
	int32_t vlong_mv;
	int32_t vbat_mv;
	int32_t vdc_mv;
	int32_t itr_ua;
	int32_t itip_ua;
	int32_t iring_ua;
	int32_t ilong_ua;
	int32_t p_hvic_mw;
	int32_t vbat_tgt_mv;
	int32_t vbat_irq_th_mv;
	uint32_t pd_dcdc;
	uint32_t dcdc_status;
	uint8_t lcrrtp;
	uint8_t linefeed;
};

/* The raw converter and loop-status words alone, from converter_read: the
 * three the host's recovery ladder decides on. Same words as in struct
 * udsp_slic_monitor. */
struct udsp_slic_converter {
	uint32_t pd_dcdc;
	uint32_t dcdc_status;
	uint8_t lcrrtp;
};

struct udsp_slic;

/* Every int op returns 0 on success or a negative errno. attach may receive
 * a NULL host for an in-tree simulation backend (the fake); a dlopened
 * plug-in always gets a real host. A linked-in backend exports
 * udsp_slic_<name>_descriptor; the bare udsp_slic_descriptor name belongs to
 * the dlopened plug-in alone. */
struct udsp_slic_ops {
	const char *name;

	/* Attach to the host hooks; read REG0 for the part and its channel
	 * count into *channels (probe, no init); build the safe transfer.
	 * slic_reset is already released and the PCM clock already running by
	 * the startup order. NULL on failure. */
	struct udsp_slic *(*attach)(const struct udsp_slic_host *host,
				    unsigned *channels);
	void (*detach)(struct udsp_slic *s);

	/* Full init: patch, register tables, the vendor calibrations on an
	 * open line (seconds; the loop balance calibration, which feeds the
	 * line, is never run), PCM linear 16-bit, timeslot = the slot of the
	 * channel * stride + host->pcm_offset_bits, PCM start. The stride and
	 * the slot order are build-profile facts: stride 16, or 32 on MT7622,
	 * where channel 0 takes the second slot of the frame; the highway's
	 * data offset joins every slot. pcm_wideband changes no timeslot: the
	 * part emits its second sample half a frame after the programmed slot,
	 * measured pair {bit 0, bit 128}. SLOW job. */
	int (*init)(struct udsp_slic *s, unsigned chan);

	int (*linefeed_set)(struct udsp_slic *s, unsigned chan, enum udsp_linefeed lf);
	int (*ring_start)(struct udsp_slic *s, unsigned chan);	/* cadence is the host's */
	int (*ring_stop)(struct udsp_slic *s, unsigned chan);
	int (*hook_read)(struct udsp_slic *s, unsigned chan, bool *offhook);
	/* Fault source bits (enum udsp_fault_bit SLIC subset), 0 = none. An
	 * alarm bit is read-to-clear unless it caused a reinit latch: then it
	 * stays in the word beside UDSP_FAULT_SLIC_REINIT, the plug-in's
	 * verdict that the part needs reinit, until reinit succeeds.
	 * UDSP_FAULT_SLIC_DCDC follows the converter state on the plug-in's
	 * own cadence, UDSP_FAULT_SLIC_CLOCK and UDSP_FAULT_DEVICE_RESET the
	 * device's master status (event.h states the two rules). */
	int (*fault_read)(struct udsp_slic *s, unsigned chan, uint32_t *bits);
	int (*gain_set)(struct udsp_slic *s, unsigned chan, int tx_half_db, int rx_half_db);

	/* Nullable ops. A NULL pointer means the capability is absent and the
	 * backend keeps the matching HAL_FEAT bit clear. */
	int (*mwi_lamp)(struct udsp_slic *s, unsigned chan, bool on);
	int (*powersave)(struct udsp_slic *s, unsigned chan, bool on);
	int (*metering)(struct udsp_slic *s, unsigned chan,
			enum udsp_slic_meter_freq f, unsigned on_ms,
			unsigned off_ms, unsigned count);
	int (*loopback)(struct udsp_slic *s, unsigned chan, bool on);
	int (*mlt_run)(struct udsp_slic *s, unsigned chan, uint8_t test_id,
		       struct udsp_slic_mlt_result *out);	/* SLOW job */

	/* Copy the pre-framed safe-state transfer built at attach. No host
	 * call, no allocation: safe to read from a fatal handler's setup. */
	void (*safe_xfer)(struct udsp_slic *s, struct udsp_slic_safe *out);

	/* Decoded part name for udsp_cap.slic, e.g. "si32184". Nullable; a
	 * NULL leaves the backend name alone. */
	const char *(*part_name)(struct udsp_slic *s);

	/* Nullable, like the block above. Program the ring and impedance
	 * presets the region asks for and re-apply the gain pair against
	 * the new impedance index. iso3 is the udsp region name
	 * (data/regions); an unmapped name selects the profile's default pair
	 * and returns 0. Runs on the slic thread after init and on a region
	 * load. */
	int (*region_set)(struct udsp_slic *s, unsigned chan, const char *iso3);

	/* Nullable: enum udsp_slic_cap bits, valid after attach (the part is
	 * decoded and the profile linked there). udsp_open attaches with the
	 * TDM already running, reads this, and settles the line rate before
	 * udsp_start: a wideband ask the bits refuse tears the engine down
	 * and reopens the TDM at 8000. A NULL member means no capability
	 * bit. */
	uint32_t (*caps_get)(struct udsp_slic *s);

	/* Nullable. Per-channel recovery from a latched safety
	 * verdict: the vendor reinit followed by every per-channel step init
	 * performs, ending at UDSP_LF_OPEN with the plug-in's latches cleared.
	 * -EINVAL before init; -EAGAIN when the open feed cannot be
	 * confirmed first (retry later); another errno when the init failed,
	 * and then the latches stand. SLOW: seconds on the si3218x, where the
	 * vendor reinit is a full init. Runs on the slic thread (rule 1) with
	 * the line held out of hook sampling. */
	int (*reinit)(struct udsp_slic *s, unsigned chan);

	/* Nullable. One supervised DC-DC converter cycle for a
	 * converter that shut down: channel enable, open feed, power down,
	 * power up, verify. Ends at UDSP_LF_OPEN; the host re-asserts the base
	 * feed. The host never calls it into a suspected short (LCR or RTP
	 * set) or after ANY over-voltage lock-out reading, confirmed or not
	 * (the vendor power-up clears DCDC_STATUS and would erase the
	 * evidence), and the op refuses with -EBUSY on its own when LCR or
	 * RTP reads set once the feed is open.
	 * Any other negative errno leaves the converter as the part left it.
	 * SLOW: about 0.2 s on a converter that answers. Each of the two
	 * vendor polls runs at most 201 delays of 10 ms, and a power-down that
	 * polls out ends the attempt before the power-up. The long attempt is
	 * a slow power-down followed by a power-up that polls out: about 4.3 s
	 * on the AN7581 bus. */
	int (*converter_restart)(struct udsp_slic *s, unsigned chan);

	/* Nullable. One line-monitor reading plus the converter and
	 * loop-status words. Writes nothing to the part. -ENODATA when the
	 * vendor API holds the channel disabled (a failed converter start),
	 * and *out is then untouched. */
	int (*monitor_read)(struct udsp_slic *s, unsigned chan,
			    struct udsp_slic_monitor *out);

	/* Nullable. The raw converter and loop-status words read from
	 * the part now, whatever the vendor API's channel enable holds. The
	 * host's recovery ladder reads them before every converter attempt
	 * and at every fault trial deadline, because a failed converter start
	 * disables the channel and monitor_read then answers -ENODATA. Writes
	 * nothing to the part. */
	int (*converter_read)(struct udsp_slic *s, unsigned chan,
			      struct udsp_slic_converter *out);

	/* Nullable. The level of hook_read, then one read of the
	 * channel's loop-status interrupt register (AN868 register 19, IRQ2:
	 * LCR_IA bit 1 and RTP_IA bit 0, both Sticky2, set by a change of the
	 * loop closure or a ring trip and cleared by the read). *moved is true
	 * when either bit was set: at least one hook transition happened since
	 * the previous call of this op on the channel, whatever else the host
	 * read in between. The op reads the register itself, so the record is
	 * current on every sample in every sampling mode; the host's fault_read
	 * drains the same register later and maps neither bit, so the fault
	 * word is unchanged. The part keeps one bit, not a count or a
	 * direction: two transitions inside one interval set it once, and the
	 * host reads a set bit with an unchanged level as a lost pair. A
	 * transition that falls between the level read and the register read
	 * reports moved with the old level; the host resolves that on the
	 * next sample. Cost: one register frame per line per sample beside the
	 * level frame. NULL means no change record, and the host samples with
	 * hook_read. Runs on the slic thread (rule 1). */
	int (*hook_read_moved)(struct udsp_slic *s, unsigned chan,
			       bool *offhook, bool *moved);
};

/* The exported, versioned descriptor libudsp dlsym's and validates. */
struct udsp_slic_descriptor {
	uint32_t abi_version;			/* == UDSP_SLIC_ABI_VERSION */
	uint32_t struct_size;			/* == sizeof(struct udsp_slic_ops) */
	const struct udsp_slic_ops *ops;
};

extern const struct udsp_slic_descriptor udsp_slic_descriptor;

#endif
