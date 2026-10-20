// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * libudsp public API. Stable contract: the uvoice hal/udsp backend is a thin
 * translation of struct hal_ops onto these calls, so the op set and the
 * FAST/SLOW split mirror uvoice/src/hal/hal.h. A change here changes uvoice.
 *
 *
 * 1. Op classes. uvoice FAST ops return the real hal_status the core acts on;
 * they cannot fake success. Each udsp op is one of:
 *
 *   SYNC-SLIC  A FAST op whose only action is on the SLIC (linefeed, polarity,
 *              hook timing, ring on/off, mwi). The call posts to the slic
 *              thread and blocks on a bounded completion (a few register
 *              writes), then returns the real result.
 *   SYNC-TICK  A FAST op with an engine action (stream_config, stream_start,
 *              stream_stop, chan_config, stream_dtmf, fax_profile, conf, and
 *              tone_play/tone_stop/digit_play/cid_send, all of which the engine
 *              generates or plays out). The call applies at the next tick
 *              boundary, or at the next read the engine drops to restart a
 *              dry ring, and blocks on that arm ack. A multi-second playout
 *              does not hold the ack: a CID burst completes later as
 *              UDSP_EV_CID_SENT, a finite tone/digit as UDSP_EV_AUDIO_END. A
 *              cid_send arms the OHT linefeed on the slic thread and the FSK
 *              burst on the engine, and its ack is the arm, not the burst.
 *   FASTPATH   A FAST op answered from cached state with no cross-thread wait
 *              (line_hook_query, counters, cap, stream_stats, line_test_result).
 *   SLOW       start/stop/region_load/line_test. Returns UDSP_OK once accepted
 *              and fills *token; the result arrives as UDSP_EV_JOB_DONE and the
 *              backend then calls the hal_job callback. The op is named "slow"
 *              in its doc line.
 *
 * Worst-case bound. A SYNC-TICK ack costs up to one period (10 ms) plus the
 * PREEMPT_NONE wakeup. A SYNC-SLIC ack costs one tick boundary plus the slic
 * thread's register writes, on AN7581 over a bit-banged spi-gpio. The engine is
 * the single producer of SLIC commands, so line control and the FAST ops share
 * one total order of register writes. uvoice's uloop carries SIP timers in the
 * hundreds of ms, so a few-ms FAST op during call setup is tolerated; cid and
 * tone bursts never block the uloop because they complete as async events.
 *
 * The class of every op below is stated in its comment. A backend built against
 * this header maps SYNC-* onto FAST hal_ops and SLOW onto SLOW hal_ops.
 *
 *
 * 2. Startup order. MediaTek AFE clocks are stream-driven. udsp_open() opens
 * the TDM devices, prefills playback, starts the linked streams and starts the
 * engine tick loop running silence, so the ring stays healthy from t0: nothing
 * else drains capture or fills playback before udsp_start. It then releases
 * slic_reset through the host gpio hook and probes the SLIC (REG0, over SPI)
 * to fill udsp_cap. The streams stay open throughout: on the MT7987 AFE the
 * Si32282's control interface answers only while the PCM clock runs.
 * udsp_start() runs the seconds-long SLIC init and calibration in
 * the SLOW dsp_start job. udsp_stop() (SLOW, may block) reverses it: every line
 * to a safe linefeed, then stop the streams and the engine. udsp_close() is
 * release-only and idempotent, and blocks on nothing, so udsp_open() stays the
 * only blocking call, matching hal.h open().
 *
 *
 * 3. Media sockets. libudsp never creates a socket. The backend owns, binds,
 * marks (IP_TOS for AF_INET, IPV6_TCLASS for AF_INET6) and connects the RTP
 * and RTCP descriptors per the hal.h stream_config_set rules and passes them
 * to udsp_stream_config(); libudsp dups them for its net and engine threads.
 *
 * Reconfiguring a running stream is the common re-INVITE: uvoice
 * call_media_program re-calls stream_config on a live stream with no preceding
 * stop (it stops first only for a T.38 mode move). So udsp_stream_config on a
 * stream that already holds dups, at the tick boundary, quiesces the net and
 * engine I/O on the prior dups, drops them, and re-dups the passed descriptors
 * unconditionally. It never compares fd numbers to detect "same socket": the OS
 * reuses closed numbers, so the number is not an identity. The backend may
 * close its originals only after the reconfig ack returns. A far-end move
 * (hold/resume) reconnects the backend's original and passes the same fd; the
 * re-dup makes that safe. A live SRTP rekey arrives as a bare reconfig too and
 * is a re-dup plus a libsrtp2 stream replace, not a stop.
 *
 * udsp_stream_stop() removes the sockets from the net epoll and drops libudsp's
 * dups before it returns, so the backend can then close the originals with no
 * fd-reuse race. It also zeroizes the SRTP key material (see udsp_media.srtp).
 */
#ifndef UDSP_UDSP_H
#define UDSP_UDSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "event.h"

#define UDSP_LINE_MAX		8
#define UDSP_STREAM_MAX		4
#define UDSP_CADENCE_MAX	5
#define UDSP_IFNAME_MAX		16
#define UDSP_ISO3_MAX		8
/* The longest delay udsp_line_loop takes, in milliseconds. */
#define UDSP_LOOP_DELAY_MAX_MS	1000u

enum udsp_status {
	UDSP_OK = 0,
	UDSP_TRANSIENT,
	UDSP_UNSUPPORTED,
	UDSP_FATAL,
};

enum udsp_feature {
	UDSP_FEAT_G729		= (1 << 0),
	UDSP_FEAT_G726		= (1 << 1),
	UDSP_FEAT_T38		= (1 << 2),
	UDSP_FEAT_LINE_TEST	= (1 << 3),
	UDSP_FEAT_POLARITY	= (1 << 4),
	UDSP_FEAT_MWI_LAMP	= (1 << 5),
	UDSP_FEAT_CONF		= (1 << 6),
	UDSP_FEAT_SRTP		= (1 << 7),
	/* The line runs at 16 kHz: the config asked for it, the SLIC plug-in
	 * reports UDSP_SLIC_CAP_WIDEBAND and the TDM granted the rate. Clear
	 * means every line runs narrowband. */
	UDSP_FEAT_WIDEBAND	= (1 << 8),
	/*
	 * The library relays a digit it detects on the line itself. It starts
	 * an RFC 4733 event on every stream in RFC2833 mode with a negotiated
	 * dtmf_pt when the detector validates the digit, and ends it when the
	 * tone ends or at 8000 ms, whichever comes first. The caller sends no
	 * event of its own for such a digit.
	 *
	 * udsp_stream_dtmf stays the way in for a digit the line never heard,
	 * a pulse digit among them. It answers UDSP_TRANSIENT while an event
	 * runs, and a digit the detector validates pre-empts an event it
	 * started.
	 */
	UDSP_FEAT_DTMF_RELAY	= (1 << 9),
	/*
	 * The library renders a telephone-event it receives toward the line
	 * itself, from the first packet of the event to its end bit or to the
	 * lost-end timeout. The caller plays no digit of its own for a wire
	 * digit the library reports.
	 *
	 * The report is unchanged: UDSP_EV_DIGIT with remote 1 still leaves at
	 * the end of the event, so a caller keeps its trace and its record. A
	 * digit that reaches the caller by any other transport is the caller's
	 * to play, through udsp_digit_play.
	 */
	UDSP_FEAT_DTMF_PLAYOUT	= (1 << 10),
	/*
	 * The library plays a line the capture of a line, its own or another,
	 * a fixed delay later (udsp_line_loop). It is always set, as
	 * UDSP_FEAT_CONF is: the loop runs inside the engine and asks the SLIC
	 * plug-in for nothing.
	 */
	UDSP_FEAT_LOOP		= (1 << 11),
};

enum udsp_codec_sel {
	UDSP_SEL_NONE = 0,
	UDSP_SEL_PCMU, UDSP_SEL_PCMA, UDSP_SEL_G729,
	UDSP_SEL_G726_16, UDSP_SEL_G726_24, UDSP_SEL_G726_32, UDSP_SEL_G726_40,
	UDSP_SEL_T38,
	/* Appended; uvoice udsp_events.c pins the values. */
	UDSP_SEL_G722 = 9,
	UDSP_SEL_OPUS = 10,
};

/* Opus bandwidth ceiling (udsp_media.opus.max_bandwidth). AUTO follows the
 * line rate: WB on a 16 kHz line, NB on an 8 kHz line. SWB and FB are
 * deliberately absent: the line rate caps at 16 kHz, so a far-end
 * maxplaybackrate above 16000 maps to WB. */
enum udsp_opus_bw {
	UDSP_OPUS_BW_AUTO = 0, UDSP_OPUS_BW_NB, UDSP_OPUS_BW_MB, UDSP_OPUS_BW_WB,
};

enum udsp_line_state {
	UDSP_LINE_DOWN = 0, UDSP_LINE_ACTIVE, UDSP_LINE_OHT,
	UDSP_LINE_BUSY, UDSP_LINE_SLEEP, UDSP_LINE_HOWLER,
};

enum udsp_polarity { UDSP_POL_FORWARD = 0, UDSP_POL_REVERSE };

enum udsp_tone {
	UDSP_TONE_NONE = 0, UDSP_TONE_DIAL, UDSP_TONE_STUTTER,
	UDSP_TONE_SECOND_DIAL, UDSP_TONE_RINGBACK, UDSP_TONE_BUSY,
	UDSP_TONE_REORDER, UDSP_TONE_SIT, UDSP_TONE_CALL_WAITING,
	UDSP_TONE_CONFIRM, UDSP_TONE_HOWLER, UDSP_TONE_SHORT_STUTTER_DIAL,
	UDSP_TONE_NUMBER_UNOBTAINABLE,
};

/*
 * What the library does with a digit the line's own detector hears. INBAND
 * keeps the tone in the uplink audio and puts no event on the wire. RFC2833
 * and INFO remove the tone from the uplink of the stream, because the digit
 * travels out of band and a far end that runs its own detector would
 * otherwise report it twice; RFC2833 with dtmf_pt 0 keeps the tone, because
 * nothing carries the digit then. RFC2833 relays the digit as an RFC 4733
 * event. INFO puts no event on the wire: the caller sends the message.
 */
enum udsp_dtmf_mode { UDSP_DTMF_INBAND = 0, UDSP_DTMF_RFC2833, UDSP_DTMF_INFO };
enum udsp_jb_mode { UDSP_JB_ADAPTIVE = 0, UDSP_JB_FIXED, UDSP_JB_OFF };
enum udsp_media_dir {
	UDSP_MEDIA_INACTIVE = 0, UDSP_MEDIA_SENDONLY,
	UDSP_MEDIA_RECVONLY, UDSP_MEDIA_SENDRECV,
};
enum udsp_srtp_suite {
	UDSP_SRTP_NONE = 0,
	UDSP_SRTP_AES_CM_128_SHA1_80 = 1,
	UDSP_SRTP_AES_CM_128_SHA1_32 = 2,
};
enum udsp_conf_mode { UDSP_CONF_OFF = 0, UDSP_CONF_LOCAL_MIX };
/* The delayed loop of udsp_line_loop: SELF plays the line its own capture,
 * PEER plays it the capture of another line, OFF plays neither. */
enum udsp_loop_mode { UDSP_LOOP_OFF = 0, UDSP_LOOP_SELF, UDSP_LOOP_PEER };
enum udsp_t38_ec { UDSP_T38_EC_NONE = 0, UDSP_T38_EC_REDUNDANCY };	/* media.t38.ec */
enum udsp_fax_mode { UDSP_FAX_OFF = 0, UDSP_FAX_PASSTHROUGH, UDSP_FAX_T38 };
/* What a G.711 passthrough carries (udsp_media.passthrough). The kind selects
 * the canceller policy: a modem exchange is full duplex, a fax exchange is
 * half duplex. */
enum udsp_passthrough {
	UDSP_PASSTHROUGH_OFF = 0, UDSP_PASSTHROUGH_MODEM, UDSP_PASSTHROUGH_FAX,
};
enum udsp_line_test {
	UDSP_TEST_GR909 = 0, UDSP_TEST_ROH, UDSP_TEST_CAPACITANCE, UDSP_TEST_LOOP,
};
enum udsp_cid_kind { UDSP_CID_TYPE1 = 0, UDSP_CID_TYPE2 };

/* The scheduling policy a library thread runs under, read back after the
 * request (udsp_cap.rt_policy, rt_net_policy). NONE means the library
 * reported nothing; UNKNOWN means the read of the policy failed. */
enum udsp_rt_policy {
	UDSP_RT_POLICY_NONE = 0,
	UDSP_RT_POLICY_UNKNOWN,
	UDSP_RT_POLICY_OTHER,
	UDSP_RT_POLICY_FIFO,
	UDSP_RT_POLICY_RR,
};

/* Probed once, valid until close. lines = min(SLIC channels, populated ports);
 * the backend passes the populated count from the platform DB in the config,
 * because REG0 reports the SLIC part, not the board. */
struct udsp_cap {
	char backend[16];
	char version[32];		/* libudsp + slic plug-in build */
	char slic[16];
	uint8_t lines;
	uint8_t streams_per_chan;
	uint32_t features;	/* enum udsp_feature */
	uint32_t codecs;	/* 1 << enum udsp_codec_sel */
	/*
	 * Appended. What the real-time setup of the open achieved, read back
	 * after the request (core/rt.c, core/thread.c). rt_prio is the
	 * SCHED_FIFO priority the config asked for, 0 = no request.
	 * rt_sched_errno is the refusal of that request, 0 when it was granted
	 * or not made; EPERM is the RT_GROUP_SCHED gate. rt_cpu is the pinned
	 * cpu, -1 when the tick is not pinned. rt_mlock is the lock the library
	 * itself took, which it takes only with a request; a host that locks
	 * the process on its own is not reflected here. The record names the
	 * engine of the open: a recovery that recreates the engine does not
	 * rewrite it, because a write from the slic thread would race a reader
	 * of udsp_cap() on the control thread.
	 */
	uint8_t rt_policy;		/* enum udsp_rt_policy, the tick thread */
	uint8_t rt_net_policy;		/* enum udsp_rt_policy, the net thread */
	bool rt_mlock;
	int16_t rt_cpu;
	int32_t rt_prio;
	int32_t rt_sched_errno;
	int32_t rt_mlock_errno;
	int32_t rt_affinity_errno;
};

struct udsp_counters {
	uint64_t events_sent;
	uint64_t events_dropped;
	uint64_t op_errors;
	uint64_t tdm_stalls;	/* engine-detected capture/playback resyncs */
	/* The catch-up record of a late engine, appended. A hold
	 * is one episode in which the engine found two or more capture
	 * periods waiting; the three drop counters are in periods. */
	uint64_t holds;
	uint64_t capture_drops;		/* periods the ring overwrote unread */
	uint64_t trim_drops;		/* periods the engine skipped to catch up */
	uint64_t playback_expired;	/* periods the hardware ran past */
	/* The slic thread's hook sampling record, appended, in
	 * wall-clock terms: the hook machine measures a pulse between sample
	 * stamps, so a sample the thread took late mismeasures the pulse.
	 * hook_gaps counts the samples that came more than 20 ms (two ticks)
	 * after the previous one while the thread was sampling per tick,
	 * hook_gap_max_ms is the widest such gap, and hook_moved_gaps counts
	 * the samples on which the part recorded a hook change the level did
	 * not show: an even number of transitions inside one gap, a lost
	 * pulse. */
	uint64_t hook_gaps;
	uint64_t hook_moved_gaps;
	uint32_t hook_gap_max_ms;
};

/* The severity of a line the library hands to udsp_config.log. */
enum udsp_log_level {
	UDSP_LOG_ERR = 0,
	UDSP_LOG_WARN,
	UDSP_LOG_INFO,
	UDSP_LOG_DEBUG,
};

/*
 * udsp_config. Source is the voip.hal UCI section the backend reads; every
 * field has a board default. hal_ops.open() still takes only event_fd, so the
 * backend builds this struct itself.
 */
struct udsp_config {
	char tdm_backend[16];		/* "alsa", "alsa-raw" or "file" */
	char tdm_playback[64];		/* e.g. "plughw:0,0" (alsa),
					 * "hw:0,0" (alsa-raw) or a file path */
	char tdm_capture[64];		/* e.g. "plughw:0,1" (alsa),
					 * "hw:0,1" (alsa-raw) or a file path */
	char slic_plugin[128];		/* .so path for dlopen, empty = fake */
	char fax_plugin[128];		/* .so path, empty = passthrough only */
	char spidev[64];		/* e.g. "/dev/spidev0.0" */
	char gpio_reset[32];		/* slic_reset sysfs name */
	char gpio_interrupt[32];	/* slic_interrupt sysfs name, empty = poll */
	uint8_t populated_ports;	/* PDB_NUM_POTS, the population authority */
	uint8_t channels;		/* TDM channels to open (1 or 2) */
	uint8_t prefill_periods;	/* default 3 */
	int engine_cpu;			/* -1 = no pinning */
	int engine_prio;		/* SCHED_FIFO prio, default 50; <= 0 = no RT
					 * setup and no mlock (test mode) */
	char region[UDSP_ISO3_MAX];	/* initial region, may be empty */
	/* PCM data offset in bit clocks after FSYNC. 0 selects the SLIC
	 * plug-in's profile default (1 on the MediaTek profiles, 0 on
	 * an7581); a nonzero value overrides it. Reaches the plug-in
	 * through udsp_slic_host untouched. */
	uint8_t pcm_offset_bits;
	/* Line sample rate in Hz: 0 or 8000 = narrowband; 16000 asks for the
	 * wideband line. When the SLIC plug-in reports no
	 * UDSP_SLIC_CAP_WIDEBAND at attach, or the TDM device refuses the
	 * rate, udsp_open degrades: it tears the engine down and reopens the
	 * TDM at 8000 with a logged reason, before udsp_start (the tdm open
	 * itself never renegotiates). The region never changes the line rate:
	 * a region without wideband impedance data keeps 16 kHz on narrowband
	 * hybrid coefficients with a log. Any other value fails udsp_open. */
	uint16_t sample_rate;
	/*
	 * Appended. The library's text sink: the open failures with their
	 * reason, the rate settle, the slic thread's faults and the SLIC
	 * plug-in's lines. NULL is silent. The library calls it on the control
	 * thread inside udsp_open and on the slic thread, never on the engine
	 * or the net thread; one line per call, without a trailing newline. A
	 * UDSP_LOG_DEBUG line travels only while udsp_debug_set holds
	 * UDSP_DEBUG_SLIC_LOG.
	 */
	void (*log)(void *ctx, enum udsp_log_level level, const char *msg);
	void *log_ctx;
	/*
	 * Appended. Every line comes up with the feed open at udsp_start, and
	 * stays open through a recovery and a dead line's return, until
	 * udsp_line_state asks for a state above UDSP_LINE_DOWN. Zero keeps
	 * the default: every initialised line takes the active feed at start.
	 * A consumer whose lines carry battery only while they are in service
	 * sets it and raises each line itself.
	 */
	bool lines_start_down;
};

struct udsp_addr {
	uint8_t ipver;			/* 4 or 6 */
	uint8_t addr[16];
	uint16_t port;
	uint16_t rtcp_port;
};

enum udsp_cid_format {
	UDSP_CIDF_BELLCORE_FSK = 0, UDSP_CIDF_ETSI_FSK, UDSP_CIDF_ETSI_RPAS,
	UDSP_CIDF_ETSI_DTAS, UDSP_CIDF_DTMF, UDSP_CIDF_NTT,
	UDSP_CIDF_ETSI_LR_DTAS, UDSP_CIDF_DTMF_DTAS,
	/* The bound of the name tables, never a format. */
	UDSP_CIDF_COUNT,
};

/*
 * What a hook flash does with two calls: the [hook] flash_service key of the
 * region grammar (off, split, drop). The library never reads it. It is call
 * control policy the region file carries for the region read-back, the r2..r8
 * shape, and nothing under line/ or core/ may branch on it. The numbers equal
 * enum hal_flash_service of uvoice hal.h, which the uvoice udsp backend checks
 * at compile time.
 */
enum udsp_flash_service {
	UDSP_FLASH_SERVICE_OFF = 0,	/* the flash is a plain network event */
	UDSP_FLASH_SERVICE_SPLIT,	/* the flash splits a conference in two */
	UDSP_FLASH_SERVICE_DROP,	/* the flash drops the last call */
	/* The bound of the name table, never a service. */
	UDSP_FLASH_SERVICE_COUNT,
};

struct udsp_cadence { uint16_t on_ms; uint16_t off_ms; };

/* VMWI: mwi true is the visual message-waiting on burst; a record with mwi
 * false and number, name and blockinfo all empty is the off burst (uvoice
 * mwi_fsk_send builds exactly that record, and its cid_fill always writes
 * blockinfo for a real caller, so no caller identity matches); anything else
 * is a caller identity. udsp_mwi() is the lamp op alone (SYNC-SLIC;
 * UDSP_UNSUPPORTED without the plug-in lamp op): the FSK VMWI path is cid_send,
 * exactly as the uvoice core drives it. */
struct udsp_cid {
	uint8_t format;			/* enum udsp_cid_format */
	bool mwi;			/* FSK VMWI when set */
	bool number_private, name_private;
	char number[UDSP_CID_STR_MAX];
	char name[UDSP_CID_STR_MAX];
	char datetime[UDSP_CID_STR_MAX];	/* MMDDHHMM, empty = DSP fills */
	/* GR-30 absence reason rendered into whichever identity field is empty
	 * (line.c cid_fill: "O" unavailable, "P" withheld). */
	char blockinfo[UDSP_CID_STR_MAX];
};

struct udsp_ring {
	struct udsp_cadence cadence[UDSP_CADENCE_MAX];
	struct udsp_cadence first[UDSP_CADENCE_MAX];
	uint8_t cadence_count, first_count;
	uint8_t cid_at_ring;		/* send CID after this break, 0 = before */
	/* cid_wait_ms is the delay from the break to the burst, the GR-30 pause
	 * the terminal needs before the carrier; 0 takes the region [cid]
	 * wait_ms. It is not a bound on the break: the engine holds the break
	 * for the whole sequence and refuses at ring_start any record the break
	 * cannot deliver. */
	uint32_t cid_wait_ms, duration_ms;
	bool cid_present;
	struct udsp_cid cid;
};

enum udsp_detect {
	UDSP_DET_DTMF		= (1 << 0),
	/* The [hook] pulse_enable of the active region decides pulse
	 * classification, because its break window can overlap the flash
	 * window. A clear bit turns pulses off on the line; a set bit keeps the
	 * region value. Before the first udsp_chan_config of a line the region
	 * value applies alone, and with no region loaded pulses stay off. */
	UDSP_DET_PULSE		= (1 << 1),
	UDSP_DET_MODEM		= (1 << 2),
	/* The value of HAL_DETECT_JB_EVENT; core never sets it. */
	UDSP_DET_JB_EVENT	= (1 << 3),
};

struct udsp_chan_cfg {
	uint32_t detect_mask;		/* enum udsp_detect */
	int8_t gain_tx, gain_rx;	/* 0.5 dB steps */
	bool ec_enable, ec_nlp_enable;
	uint16_t ec_tail_ms;		/* 0 = default */
	/*
	 * 0.5 dB steps. slic_gain_tx is the SLIC analog transmit gain.
	 * slic_gain_rx is the receive gain of the port: the engine applies a
	 * negative value as a loss ahead of the SLIC and programs only a
	 * positive value into the SLIC. A local signal therefore plays at its
	 * own level whatever the value (core/chan.c chan_rx_split_apply).
	 */
	int8_t slic_gain_tx, slic_gain_rx;
	/*
	 * The answer-tone disabler, appended after the SLIC gain pair by the
	 * rule the record follows, so a backend built against the older layout
	 * keeps its offsets. The engine latches a passthrough kind on an
	 * answer tone from either direction (core/chan.c chan_fax_latch): the
	 * phase reversals take the modem policy, which skips the canceller,
	 * and a plain tone takes the fax policy, which turns the non-linear
	 * processor off. That is the mapping of G.168 7.1, and these two pick
	 * the rules it applies. Both read false as the recommendation's own
	 * behaviour, so a caller that leaves them alone gets it.
	 *
	 * ec_tone_latch false releases the disabler after the holding band
	 * falls quiet (G.168 7.8). True turns that quiet release off, for a far
	 * end whose carrier the release window cuts into; a move of the line's
	 * governing stream count, a caller clear and the end of the line's
	 * streams still release it.
	 *
	 * ec_tone_force_disable false takes the phase-reversed tone to the
	 * modem policy and the plain tone to the fax policy, which is the
	 * mapping of G.168 7.1; true takes the plain tone to the modem policy
	 * too, for gear that expects a full disable on a plain 2100 Hz. That
	 * is not what the recommendation asks for: it forbids a full disable
	 * on anything but the reversals.
	 */
	bool ec_tone_latch;
	bool ec_tone_force_disable;
};


struct udsp_media {
	struct udsp_addr local;
	struct udsp_addr remote;
	uint8_t codec;			/* enum udsp_codec_sel */
	uint8_t codec_pt;
	uint8_t dtmf_pt;
	uint8_t dtmf_mode;		/* enum udsp_dtmf_mode */
	uint8_t direction;		/* enum udsp_media_dir */
	uint8_t ptime_ms;		/* 10, 20, 30; the engine clamps a 40 to 30 */
	uint8_t jb_mode;		/* enum udsp_jb_mode */
	/*
	 * The generic voice detector and the RFC 3389 comfort noise it sends
	 * in the silence it closes. The library keeps vad only beside cng: a
	 * closed talkspurt with nothing that describes the silence leaves the
	 * far end digital silence and a backward-adaptive decoder out of step
	 * at every onset, so a description that asks for vad without cng runs
	 * no detector and the encoder sees every frame. The codec-native pair
	 * (dtx below) replaces both.
	 */
	bool vad, cng, rtcp_enable;
	uint8_t rtp_dscp, rtcp_dscp;
	uint16_t jb_init_ms, jb_max_ms, rtcp_interval_ms;
	char bind_dev[UDSP_IFNAME_MAX];
	struct { uint32_t max_bit_rate; uint16_t max_datagram; uint8_t ec; } t38;
	/* AES_CM_128 master key||salt, 30 bytes each. libudsp holds the keys
	 * (libsrtp2), zeroizes key_tx/key_rx and suite in udsp_stream_stop, and
	 * never emits key bytes to any log, trace or counter, suite name only. */
	struct { uint8_t suite; uint8_t key_tx[30]; uint8_t key_rx[30]; } srtp;
	/* Opus encoder parameters, consumed only when codec is UDSP_SEL_OPUS;
	 * zero selects the library default (32 kbit/s VBR on a 16 kHz line,
	 * complexity from the bench, FEC decode always on). DTX is absent:
	 * the encoder runs with Opus DTX off. The jitter buffer bridges a
	 * codec-native DTX silence only behind a SID frame (rtp/jb.c
	 * jb_dtx_resume_exempt), and the Opus ops carry no SID frame, so the
	 * library clears dtx below on an Opus stream. */
	struct { uint32_t bitrate; uint8_t complexity; uint8_t max_bandwidth;
		 bool fec; } opus;
	/*
	 * Appended after the opus block, by the rule that block followed, so a
	 * backend built against the older record keeps its layout.
	 *
	 * The local hold silence (hold-tx). Set means the transmit path encodes
	 * silence instead of the uplink audio: the tick replaces the captured
	 * frame with zeros before the measurement, the resampler and the packet
	 * staging, so every codec encodes its own silence (G.711 mu-law writes
	 * 0xff), the voice detector and the comfort noise follow the rules they
	 * already have, and a whole-packet codec stages the same zeros. RFC
	 * 3264 asks a holder to send silence or music and not the room, which
	 * on an FXS line carries the dial tone its hybrid leaks back; the
	 * caller sets this for the leg it put on local hold and clears it on
	 * resume.
	 *
	 * It is not a direction. direction still says who may send, so a stream
	 * that does not send puts nothing on the wire whatever this field
	 * holds.
	 *
	 * It silences the telephone events too. A held leg puts nothing of its
	 * own on the wire and a digit is as much its own as the room is, so
	 * udsp_stream_dtmf answers UDSP_UNSUPPORTED while this is set, and a
	 * description that turns it on cuts an event in flight, exactly as a
	 * direction that stops sending does. The far end closes the digit on the
	 * ends it already saw (RFC 4733 2.5.1.4).
	 *
	 * A description that moves only this field is a bare reconfiguration.
	 * It reaches no codec create parameter and no field the RTP sender
	 * compares, so the codec pair stands and the SSRC, the sequence and the
	 * timestamp all continue across the change.
	 */
	bool tx_silence;
	/*
	 * enum udsp_passthrough: the stream carries a data carrier, not
	 * speech. Appended after tx_silence, by the rule the block above
	 * followed. A value above UDSP_PASSTHROUGH_FAX is refused, as a bad
	 * jb_mode is.
	 *
	 * It is the description side record of a G.711 passthrough. The caller
	 * clamps the codec itself and writes this field into every description
	 * it programs while the carrier runs, so a later udsp_stream_config
	 * keeps the effects the fax profile armed instead of putting the
	 * speech treatment back under a live carrier.
	 *
	 * It is not the V.152 voice band data mode. That mode negotiates a
	 * payload type change on the wire; this field describes what the
	 * stream carries and asks for no signalling of its own.
	 *
	 * The kind selects the canceller policy of the line, and nothing else
	 * reads it. A modem exchange is full duplex, so the canceller is
	 * skipped whole: a converged filter subtracts its estimate from the
	 * very carrier the far end demodulates. A fax exchange is half duplex,
	 * so the canceller runs and only its non-linear processor goes off:
	 * the processor replaces the residual and mutes the inbound direction,
	 * which a half-duplex turn cannot afford, and the filter itself gets
	 * clean single talk to adapt on.
	 *
	 * Either kind carries the rest. The stream sends no telephone event of
	 * its own: udsp_stream_dtmf answers UDSP_UNSUPPORTED, and a
	 * description that turns a kind on cuts an event in flight, exactly as
	 * the hold silence does. Every concealment is silence, never packet
	 * loss concealment and never comfort noise, because both inject
	 * samples the far end demodulates as data. media_prepare forces vad
	 * and cng false and the jitter buffer to UDSP_JB_FIXED, because a
	 * carrier survives neither detector and no adaptive playout. The
	 * buffer holds the depth it adapted to at the switch for the whole
	 * passthrough, and a later description that keeps the codec and the
	 * packet duration keeps the queue with it.
	 *
	 * The line holds its DTMF detector for as long as a kind stands on
	 * it. A carrier is not a digit, so every decision the detector makes
	 * over one is talk-off, and the caller relays a digit as an event
	 * whose packets replace carrier frames. The hold reports a digit the
	 * detector already decided, as the disarm does, and arms the detector
	 * again when the kind leaves. The detector belongs to the line, so a
	 * second call of the line detects no digit either, until the carrier
	 * ends or its own leg goes on local hold.
	 *
	 * udsp_fax_profile(UDSP_FAX_PASSTHROUGH) sets UDSP_PASSTHROUGH_MODEM
	 * and UDSP_FAX_OFF puts back what the description carried. The fax
	 * kind reaches the library through the description alone, because the
	 * caller learns it from the V.21 preamble after the profile is already
	 * on. A plain udsp_stream_config clears the profile record, so the
	 * effects live past that call only where the new description carries a
	 * kind.
	 */
	uint8_t passthrough;
	/*
	 * Codec-native discontinuous transmission, appended after passthrough
	 * by the rule the fields above followed. Set means the encoder runs
	 * the codec's own voice detector: it returns SID frames and
	 * untransmitted frames in place of speech, and the generic voice
	 * detector and the RFC 3389 comfort noise stand aside, because two
	 * silence machines on one talkspurt would fight over it. It is
	 * honoured only when the codec's ops carry sid_bytes (codec.h); on
	 * any other codec, and on a passthrough description, the library
	 * clears it and the vad/cng pair keeps its meaning.
	 */
	bool dtx;
	/*
	 * Appended after dtx, by the rule the fields above followed, so a
	 * backend built against the older record keeps its layout.
	 *
	 * The longest an idle descriptor stays silent, in milliseconds, before
	 * the engine sends an empty datagram on it. Inbound media is admitted
	 * by connection tracking alone, and an entry exists only after this
	 * box sent on the tuple, so the engine sends one empty datagram on
	 * each descriptor at every configuration and again whenever nothing
	 * else left on it for this long (RFC 6263 section 4.1: a receiver
	 * discards it on the RFC 3550 minimum length). A descriptor that
	 * carries media or reports never reaches the interval. 0 takes
	 * UDSP_TX_PUNCH_DEFAULT_MS.
	 */
	uint16_t punch_interval_ms;
};

/* The default udsp_media.punch_interval_ms: the shortest keepalive interval
 * RFC 6263 section 7 recommends for UDP. */
#define UDSP_TX_PUNCH_DEFAULT_MS	15000u

struct udsp_media_stats {
	uint64_t packets_sent, packets_recv, packets_lost, packets_discarded;
	uint64_t loss_rate, loss_rate_far;
	uint64_t jitter_avg, jitter_max, jitter_far, rtt_avg, rtt_max;
	uint64_t bytes_sent, bytes_recv;
	float mos_lq, mos_cq;		/* two-decimal presentation is the backend's */
	uint32_t rfactor;
};

/* Receive jitter buffer counters, one set per stream (rtp/jb.h owns the
 * buffer; udsp_stream_jb_stats copies this record). The output counters
 * frames_out, plc, cn, silence and skew_inserts partition the 10 ms output
 * units; late_drop, dup_drop, overflow_drop and hold_drop count the
 * rejected packets; every time is in milliseconds. */
struct udsp_jb_stats {
	uint64_t frames_out;
	uint64_t plc;
	uint64_t cn;
	uint64_t silence;
	uint64_t late_drop;
	uint64_t dup_drop;
	uint64_t overflow_drop;
	/* Far-ahead packet refused while a parked clock awaited a confirming
	 * successor; distinct from late_drop so the CDR keeps its meaning. */
	uint64_t hold_drop;
	/* Playable audio a timeline reset threw away: the sum of the
	 * reachable queued slot spans at every resync, in milliseconds.
	 * Not a drop count and not part of u.jb.drop. */
	uint64_t resync_discard_ms;
	/* An SSRC change, a timestamp jump past the resync bound or a confirmed
	 * timestamp step. */
	uint64_t resyncs;
	uint64_t starve_resyncs;	/* far end resumed behind a parked clock */
	uint64_t cn_reanchors;		/* audio resumed behind a comfort-noise period */
	uint64_t underruns;
	uint64_t skew_drops;
	uint64_t skew_inserts;
	uint16_t target_ms;
	uint16_t depth_ms;
	uint16_t jitter_ms;
	/* Free payload slots (gauge). A value that decays across a call is a
	 * slot leak. */
	uint16_t pool_free;
	/* enum udsp_jb_mode, appended: the playout mode the buffer runs, which
	 * is the value udsp_jb_init_cfg accepted (an unknown request falls to
	 * UDSP_JB_ADAPTIVE) or udsp_jb_mode_set moved it to. Under a
	 * passthrough the stream owner pins UDSP_JB_FIXED at the depth the line
	 * adapted to, so target_ms then differs from the configured jb_init_ms
	 * on purpose; UDSP_JB_OFF reads target_ms 0 with a nonzero depth. */
	uint8_t mode;
};

/*
 * Line echo canceller state, one set per line. The canceller belongs to the
 * analog line, so this record carries no stream index; udsp_line_lec_stats
 * copies it from the double-buffer snapshot the engine publishes every tick,
 * the shape udsp_stream_jb_stats uses.
 *
 * Every field is an integer, and every scaled field carries its unit in its
 * name. The canceller holds these quantities as floats (dsp/lec.h struct
 * udsp_ec_nlms_info); a float in this record would put a target's float
 * layout in the contract, and a fixed-point integer never rounds twice. Fields
 * are appended, never reordered. Every conversion saturates, so a value that
 * leaves its range reads as the range end and never wraps.
 */
struct udsp_lec_stats {
	/* The tail the instance applied, in ms. create() clamps the request to
	 * 16..128 ms at 8 kHz and to 16..64 ms at 16 kHz, so a 128 ms ask on a
	 * wideband line reads back as 64 here (dsp/lec.h). */
	uint32_t tail_ms;
	uint32_t taps;			/* tail_ms times rate/1000 */
	/* The frames the canceller processed, and the frames inside them whose
	 * update did not stand, for a silent reference, freeze or double talk.
	 * hold_frames against frames is the hold ratio this pair exists for;
	 * neither number is a rate on its own. */
	uint32_t frames;
	uint32_t hold_frames;
	/* Divergence watchdog resets: the filter passed the coefficient energy
	 * bound and was zeroed. */
	uint32_t diverge_resets;
	/* Adaptation trials the detector ran to escape a hold it could not end
	 * by verdict (dsp/lec.h probes). */
	uint32_t probes;
	/* The tracked echo return loss enhancement in millidecibels: 20 dB
	 * reads 20000. The tracker divides two floored power averages, so the
	 * range belongs to the line and not to the type; the conversion
	 * saturates at INT32_MIN and INT32_MAX. A value that is not a number
	 * reads 0, which is what a fresh instance and a divergence reset both
	 * leave behind. */
	int32_t erle_mdb;
	/* The energy of the filter in micro-units: the energy times 1000000,
	 * so 1.0 reads 1000000 and a filter that never adapted reads 0. The
	 * scale is micro and not milli because a filter converged against a
	 * 30 dB hybrid holds 0.001 and one against a 60 dB hybrid 0.000001,
	 * and milli-units quantise both to zero; "converged against a quiet
	 * hybrid" and "never adapted" are the two readings this field exists
	 * to tell apart. The divergence watchdog bounds the energy at 4.0,
	 * which is 4000000 here, so the whole watched interval fits the 32-bit
	 * range three orders of magnitude over. */
	uint32_t coef_energy_micro;
	/* The learned lower bound of the hybrid's power gain, in the same
	 * micro-units, so the two energy figures read alike. The tracker
	 * floors at 0.001 (1000 here) and the consumers cap it at 0.25
	 * (250000), which is also the value it holds until the tracker has
	 * seen enough active frames to say less. It is a minimum statistic and
	 * under-reads by design (dsp/lec.h echo_gain). */
	uint32_t echo_gain_micro;
	/* The detector's standing verdict, and whether the non-linear
	 * processor replaced the residual on the last frame. 0 or 1. */
	uint8_t double_talk;
	uint8_t nlp_active;
	/* The reference alignment, appended. These two are state of the line
	 * and not of the instance. The engine measures one loop delay per
	 * handle. A line takes the current measurement verbatim on the first
	 * frame of a new canceller. While that canceller stands, the line keeps
	 * its value within one period of the measurement. Two lines can
	 * therefore read different values. A line whose canceller starts
	 * inside an open window, a fresh engine included, applies no reference
	 * yet. It publishes ref_delay_samples 0 and ref_ready 0 until a window
	 * completes. Past the allowance, the fallback gives it the last window,
	 * or the nominal prefill before the first one. The tick itself reads
	 * one frame there.
	 * ref_delay_samples is the reference the tick applied, in samples at
	 * the line rate. It is the loop the line stands on less one period of
	 * causal margin. At 8 kHz, 160 is therefore a loop of 240 (30 ms) under
	 * an 80-sample guard; divide by rate/1000 for milliseconds. It is the
	 * applied value and not the engine's last measurement. The rule
	 * compares the unclamped measurement against the applied value, keeps
	 * it within one period, and publishes the clamp of what stands under
	 * the guard. The tolerance keeps a value up to one period from the
	 * measurement. The published value therefore stands zero to two
	 * periods under loop_ref inside the reach, and more where the clamp
	 * engages. A re-time can leave it unchanged (retimes below). ref_ready
	 * is 1 once the measurement stands. While it is 0 the engine holds
	 * adaptation, because a canceller that adapts against a reference
	 * timed wrong diverges instead of converging. A canceller with frames
	 * of its own, a ref_ready of 1 and no coefficient energy saw the line
	 * and learned nothing from it. The same reading with ref_ready 0 is a
	 * filter that was never let adapt. */
	uint32_t ref_delay_samples;
	uint8_t ref_ready;
	/* The non-linear processor's switch, appended: 1 when the policy
	 * leaves the processor on, 0 under the fax policy, under a channel
	 * configuration that asks for no processor, and on every frame the
	 * filter itself did not run (ec_active 0 below). It is the switch the
	 * policy asks for and not a measurement, which is what tells it apart
	 * from nlp_active above: a line with nlp_enabled 1 and nlp_active 0
	 * ran a frame the processor was on for and did not act on. */
	uint8_t nlp_enabled;
	/* Whether the filter ran on the frame this record describes, appended.
	 * The tick MEASURES it (core/chan.c chan_lec_run sets it after the
	 * run, chan_tick clears it first), so it reads 0 under the modem
	 * policy, under a channel configuration that switched the canceller
	 * off, on an open loop (loop_open below reads 1 there), and on a line
	 * whose media does not run; the counters then stand still and the
	 * record still answers, because the instance exists. */
	uint8_t ec_active;
	/* enum udsp_passthrough, appended: the kind the tick applied to this
	 * frame (core/chan.c chan_passthrough_get: the strongest kind over the
	 * line's governing streams, else the tone disabler's latch). A T.38
	 * stream reads UDSP_PASSTHROUGH_MODEM here, because the image session
	 * runs the modem policy. */
	uint8_t passthrough;
	/* The frames the tap loops did not run on because the whole reference
	 * window was zero, appended (dsp/lec.h skip_frames). Every one of them
	 * is also in hold_frames; the two together say how much of a hold was
	 * digital silence from the far end (a muted peer, a held leg, the
	 * jitter buffer's fill) and how much a quiet or a refused reference.
	 * The skipped frames are the cost the canceller does not pay, so the
	 * ratio against frames is what the saving on a given call was. */
	uint32_t skip_frames;
	/* The re-times the line paid, appended: every reset the reference
	 * imposed on the filter (core/chan.c chan_lec_run). Two causes reset
	 * it. The first is a measured reference that moved beyond the
	 * tolerance. The engine re-latches the loop delay after every hold
	 * episode. A re-latch within one period of the standing reference
	 * keeps the taps, so the count follows the moves and not the episodes.
	 * The second is a replacement engine (udsp_stop then udsp_start). It
	 * re-times the reference whatever it goes on to measure, because the
	 * new ring is not the one the taps were aligned to.
	 *
	 * The count belongs to the line and not to the instance. It survives
	 * the reset it names, like diverge_resets and unlike frames, and it
	 * reads 0 again for a canceller the line took after a release. A
	 * canceller created after a move starts at 0 and is not reset by it.
	 * A completed window at the value already applied counts nothing.
	 * Several moves decided while the filter did not run coalesce into one
	 * reset and one count on the frame it runs again. The generation
	 * compare is on the level and not on the edge. */
	uint32_t retimes;
	/* The verdicts of the non-linear processor, appended (dsp/lec.h): one
	 * count per frame the filter ran with the processor on. A refused frame
	 * counts under the first condition that refused it, in this order: a
	 * quiet reference window, double talk, a frame the echo estimate does
	 * not explain, the ERLE gate. An eligible frame counts as a wait until
	 * the engage run completes, and as active from the frame the processor
	 * acts on. The six add up to frames less the frames the processor was
	 * off for. A re-time zeroes them with frames, and a release ends them
	 * with the instance. */
	uint32_t nlp_active_frames;
	uint32_t nlp_wait_frames;
	uint32_t nlp_erle_frames;
	uint32_t nlp_unlike_frames;
	uint32_t nlp_dt_frames;
	uint32_t nlp_quiet_frames;
	/* The index of the largest coefficient, appended, counted from tap 0 of
	 * the applied reference (ref_delay_samples). The reference stands one
	 * period under the loop, so an echo that arrives at the loop reads one
	 * period in: 80 at 8 kHz, 160 at 16 kHz. A live line reads later by
	 * the lag of the path outside the engine rings. It reads 0 while the
	 * filter holds no energy. Read it beside coef_energy_micro, because a
	 * filter aligned on its first tap also reads 0. */
	uint32_t coef_peak_tap;
	/* 1 when the tick held the filter because the loop was open, appended.
	 * The run reads the level that the previous tick consumed, so the flag
	 * follows the loop one tick late in both directions. It is the reason
	 * a record reads ec_active 0 with the counters of a call behind it,
	 * and it tells that record apart from a policy or a configuration that
	 * switched the filter off. 0 or 1. */
	uint8_t loop_open;
};

/* Runtime diagnostics. udsp_debug_set stores the mask; the engine reads it
 * once per tick, so a clear bit costs one relaxed load. */
enum udsp_debug_flag {
	UDSP_DEBUG_JB_TRACE = 1u << 0,
	/* Lets the UDSP_LOG_DEBUG lines through udsp_config.log: the SLIC
	 * plug-in's per-op trace and the vendored SDK's own messages. It also
	 * starts the verdict trace of each line's echo canceller, which the
	 * slic thread prints as lec verdict and lec second lines. */
	UDSP_DEBUG_SLIC_LOG = 1u << 1,
	/* Lets the producers of the plaintext packet trace run. The bit alone
	 * starts nothing: the rings exist only after udsp_pkt_trace_enable, and
	 * a producer that finds none pushes nothing. */
	UDSP_DEBUG_PKT_TRACE = 1u << 2,
	/* Lets the producer of the canceller capture run. The bit alone starts
	 * nothing: the ring exists only after udsp_lec_capture_enable, and a
	 * producer that finds none pushes nothing. */
	UDSP_DEBUG_LEC_CAPTURE = 1u << 3,
};

enum udsp_jb_trace_kind { UDSP_JB_TRACE_PUT = 0, UDSP_JB_TRACE_GET };

enum udsp_jb_trace_flag {
	UDSP_JB_TRACE_SKEW_DROP = 1u << 0, UDSP_JB_TRACE_SKEW_INSERT = 1u << 1,
	UDSP_JB_TRACE_PARKED = 1u << 2, UDSP_JB_TRACE_ANCHOR = 1u << 3,
	UDSP_JB_TRACE_REFILL = 1u << 4, UDSP_JB_TRACE_STARVE = 1u << 5,
	UDSP_JB_TRACE_RESYNC = 1u << 6, UDSP_JB_TRACE_CN_REANCHOR = 1u << 7,
	UDSP_JB_TRACE_UNDERRUN = 1u << 8, UDSP_JB_TRACE_HEARTBEAT = 1u << 9,
};

/* One record per jitter buffer decision that changed something: a PUT record
 * describes one packet the buffer took, a GET record one output unit whose
 * decision differs from the last one recorded for the stream. */
struct udsp_jb_trace {
	uint32_t stamp_ms;	/* the engine's now_ms */
	uint32_t pkt_ts;	/* PUT: the packet's RTP timestamp; GET: out.rtp_timestamp */
	uint32_t playout_ts;	/* the media clock before the call */
	int32_t lateness_ts;	/* PUT: playout_ts - pkt_ts, > 0 is late; GET: 0 */
	uint16_t seq;		/* PUT only */
	uint16_t depth_ms, target_ms, conceal_run_ms;	/* after the call */
	uint16_t flags;		/* enum udsp_jb_trace_flag mask */
	uint32_t lost_before;	/* records the ring could not hold before this one */
	uint8_t line, stream, kind;
	int8_t rc;		/* PUT: 0, -ETIME, -EEXIST, -ENOBUFS, -EMSGSIZE, -EINVAL; GET: the decision, 0 frame 1 plc 2 cn 3 silence */
	uint8_t state_before, state_after;	/* the buffer's state number, 0 idle 1 prefill 2 play; diagnostic, not a contract */
	uint8_t count;		/* queued slots after the call */
	uint8_t skew_pending;	/* 0 none, 1 drop, 2 insert, after the call */
};
_Static_assert(sizeof(struct udsp_jb_trace) <= 48, "udsp_jb_trace outgrew its budget");

/* The payload bytes one packet trace record carries. It is the largest
 * datagram the receive path admits, the IPv4 minimum reassembly buffer, so no
 * received datagram is ever cut; a longer transmit datagram is cut here and the
 * record keeps the cut length. */
#define UDSP_PKT_TRACE_MAX	576

enum udsp_pkt_dir { UDSP_PKT_TX, UDSP_PKT_RX };
enum udsp_pkt_kind { UDSP_PKT_RTP, UDSP_PKT_RTCP, UDSP_PKT_UDPTL };

/*
 * One media datagram in the clear: a transmit record is taken before SRTP
 * protect and a receive record after SRTP unprotect, so an encrypted call
 * reads here and nowhere else.
 *
 * A transmit record is a send attempt. The tick takes it before it protects
 * the packet, so the send that follows can still fail. The empty datagram that
 * holds an idle tuple open is not media and earns no record. A receive record
 * is one datagram the net thread took; len 0 is a datagram the receive session
 * refused or an empty keepalive the far end sent.
 *
 * stamp_ns is the instant the record was taken, CLOCK_MONOTONIC nanoseconds in
 * both directions. A receive record carries the arrival stamp the net thread
 * took; a transmit record carries the stamp the tick read at the top of the
 * period that built the packet, so the tick reads no clock of its own. One
 * base carries both, so a reader orders the whole exchange on one axis.
 *
 * gen is the stream's description generation: udsp_stream_config raises it on
 * every description it applies, and the producer reads it at the tap. A reader
 * that keeps a table of the addresses each description carried can therefore
 * give a record drained after a stop the endpoints it travelled with.
 * udsp_stream_gen answers the value the live description carries.
 *
 * lost_before counts the records the ring could not hold before this one, the
 * rule struct udsp_jb_trace follows. The bytes after len are undefined.
 */
struct udsp_pkt_trace {
	uint64_t stamp_ns;
	uint32_t lost_before;
	uint32_t gen;
	uint16_t len;			/* plaintext bytes, after the cut */
	uint8_t line, stream;
	uint8_t dir;			/* enum udsp_pkt_dir */
	uint8_t kind;			/* enum udsp_pkt_kind */
	uint8_t pad[2];
	uint8_t data[UDSP_PKT_TRACE_MAX];
};
_Static_assert(sizeof(struct udsp_pkt_trace) == 600,
	       "udsp_pkt_trace changed its layout");

/* The samples of one plane of a canceller capture record: one frame of a
 * 16 kHz line. An 8 kHz line fills the first 80. */
#define UDSP_LEC_CAPTURE_SAMPLES	160

enum udsp_lec_capture_kind {
	UDSP_LEC_CAPTURE_RUN = 0,
	UDSP_LEC_CAPTURE_FEED = 1,
	UDSP_LEC_CAPTURE_END = 2,
};

/* The verdict of the processor gate on a RUN. A refused frame reads the first
 * condition that refused it, in the order quiet, dt, unlike, erle. */
enum udsp_lec_capture_verdict {
	UDSP_LEC_CAPTURE_VERDICT_OFF = 0,
	UDSP_LEC_CAPTURE_VERDICT_QUIET = 1,
	UDSP_LEC_CAPTURE_VERDICT_DT = 2,
	UDSP_LEC_CAPTURE_VERDICT_UNLIKE = 3,
	UDSP_LEC_CAPTURE_VERDICT_ERLE = 4,
	UDSP_LEC_CAPTURE_VERDICT_PASS = 5,
};

/*
 * The flags of a record:
 * - NEW: the first record of an instance;
 * - RESET: a RUN whose tick reset the instance before the run;
 * - FREEZE: a RUN with adaptation held;
 * - NLP: a RUN with the processor switched on;
 * - FIRED, DT, ACTIVE and PROBING: the verdicts of a RUN;
 * - SKIPPED: a RUN or a FEED whose skipped field is not 0.
 *
 * A FEED carries exactly one SKIP flag, the reason the tick skipped the
 * filter:
 * - SKIP_OPEN: the loop was open;
 * - SKIP_MODEM: the modem passthrough kind;
 * - SKIP_OFF: the channel configuration switched the canceller off;
 * - SKIP_INACTIVE: the stream of the line was inactive;
 * - SKIP_SYNTHETIC: the tick had no capture period. The engine rendered the
 *   period ahead, to refill a playback ring that ran dry, and mic reads zero.
 *
 * SKIPPED stands exactly when skipped is not 0. A NEW record and an END
 * carry neither (struct udsp_lec_capture).
 */
enum udsp_lec_capture_flag {
	UDSP_LEC_CAPTURE_NEW = 1u << 0,
	UDSP_LEC_CAPTURE_RESET = 1u << 1,
	UDSP_LEC_CAPTURE_FREEZE = 1u << 2,
	UDSP_LEC_CAPTURE_NLP = 1u << 3,
	UDSP_LEC_CAPTURE_FIRED = 1u << 4,
	UDSP_LEC_CAPTURE_DT = 1u << 5,
	UDSP_LEC_CAPTURE_ACTIVE = 1u << 6,
	UDSP_LEC_CAPTURE_PROBING = 1u << 7,
	UDSP_LEC_CAPTURE_SKIP_OPEN = 1u << 8,
	UDSP_LEC_CAPTURE_SKIP_MODEM = 1u << 9,
	UDSP_LEC_CAPTURE_SKIP_OFF = 1u << 10,
	UDSP_LEC_CAPTURE_SKIP_INACTIVE = 1u << 11,
	UDSP_LEC_CAPTURE_SKIPPED = 1u << 12,
	UDSP_LEC_CAPTURE_SKIP_SYNTHETIC = 1u << 13,
};

/*
 * One engine tick of the echo canceller of one line: what the canceller saw,
 * the controls the tick applied and what the canceller did. A host program
 * replays the records through the same canceller code. While
 * UDSP_DEBUG_LEC_CAPTURE stands, every line that holds a canceller gives one
 * record per tick. Integers only, as in struct udsp_lec_stats.
 *
 * The kinds:
 * - RUN: the filter ran. ref is the reference the tick took, mic the frame
 *   before the run and out the frame after it.
 * - FEED: the tick skipped the filter and fed ref into the history of the
 *   instance. mic is the frame of the tick, and out is undefined.
 * - END: the instance left the line. The planes are undefined. The instance
 *   ran no frame after its last record, so frames equals the frames of that
 *   record.
 * The bytes past samples in a plane are undefined too, the rule struct
 * udsp_pkt_trace follows for the bytes past len.
 *
 * The first frame that the tick hands an instance gives its NEW record. An
 * instance that leaves before its first frame gives no record, END included.
 * A capture enabled while an instance lives has no NEW for that instance, so
 * a reader replays from the next NEW.
 *
 * stamp_ns is the CLOCK_MONOTONIC instant at the top of the tick, the base of
 * the packet trace. tick is the index of the engine tick. Except on NEW and
 * on an END, a record follows the previous record of its line by skipped +
 * lost_before + 1 ticks. Any other jump covers ticks that the capture cannot
 * account for, for example a time with the bit clear or a dead line.
 *
 * skipped counts the periods that the engine skipped since the previous
 * record of the line. The engine ran no tick in them, so the canceller of no
 * line saw them. skipped saturates at 65535 and reads 0 on NEW and on an END.
 *
 * The rule for a reader: on a record with SKIPPED, the tick jump must equal
 * skipped + lost_before + 1. Any other jump is a gap. A record with a loss
 * reads as a loss, with SKIPPED or without it. An END without a loss whose
 * frames differs from the previous record of its instance is a gap too: a RUN
 * before it is missing. A FEED missing just before an END is not visible,
 * because an END carries no skipped count and follows no tick rule.
 *
 * gen is the generation of the verdict trace of the line. It moves on a new
 * instance, on a re-time and on a change of the reference loss, because each
 * starts the frame count over. The FEED records of a new instance carry the
 * generation of its first RUN.
 *
 * frames, hold_frames, erle_mdb, echo_gain_micro, coef_energy_micro, probes
 * and diverge_resets read the instance after the record, in the units of
 * struct udsp_lec_stats. probes and diverge_resets saturate at 65535. The
 * difference of hold_frames to the previous record is the adapt verdict: a
 * frame that did not adapt raises it by one. ref_delay_samples is the delay
 * of the reference the tick took, 0 on an END. noise_ddbov is the near-end
 * floor that the canceller tracks, in tenths of dB on the dBov scale, where
 * larger is quieter. It reads 1270 while no floor is measured.
 *
 * The six processor counts of struct udsp_lec_stats follow from nlp_verdict
 * and ACTIVE. A pass with ACTIVE is an active frame and a pass without it a
 * wait frame. The four refusals count under their names, and off counts
 * nowhere. hold_left is the hangover that the frame found.
 *
 * lost_before counts the records of this line that the ring could not hold
 * before this one, the rule struct udsp_pkt_trace follows. A record that
 * finds the ring full is dropped, and the next record of the same line that
 * fits carries the count. An END carries the count still open when the
 * instance leaves. An END that finds the ring full leaves its count, the END
 * included, to the next record of the line that fits. While the bit stands,
 * that record belongs to the next instance. A reader attributes the loss
 * count of a NEW record to the lineage before it. A loss invalidates a replay
 * until the next NEW, because a reset keeps state of the instance that the
 * records do not carry.
 */
struct udsp_lec_capture {
	uint64_t stamp_ns;
	uint64_t tick;
	uint32_t lost_before;
	uint32_t frames;
	uint32_t hold_frames;
	int32_t erle_mdb;
	uint32_t echo_gain_micro;
	uint32_t coef_energy_micro;
	uint32_t ref_delay_samples;
	uint16_t gen;
	uint16_t flags;			/* enum udsp_lec_capture_flag */
	uint16_t samples;		/* the line frame: 80 or 160 */
	uint16_t tail_ms;
	uint16_t noise_ddbov;
	uint16_t probes;
	uint16_t diverge_resets;
	uint8_t line;
	uint8_t kind;			/* enum udsp_lec_capture_kind */
	uint8_t nlp_verdict;		/* enum udsp_lec_capture_verdict */
	uint8_t hold_left;
	uint16_t skipped;
	int16_t ref[UDSP_LEC_CAPTURE_SAMPLES];
	int16_t mic[UDSP_LEC_CAPTURE_SAMPLES];
	int16_t out[UDSP_LEC_CAPTURE_SAMPLES];
};
_Static_assert(sizeof(struct udsp_lec_capture) == 1024,
	       "udsp_lec_capture changed its layout");
_Static_assert(offsetof(struct udsp_lec_capture, ref) == 64,
	       "the planes of udsp_lec_capture moved");

typedef struct udsp udsp_t;

/* Job completion, same shape as hal_job semantics: the backend owns the token
 * and dispatches the callback on the uloop thread from UDSP_EV_JOB_DONE. */
struct udsp_job { uint32_t token; };

/* --- lifecycle (see sections 1 and 2) --- */

/* blocking: open TDM, start clocks, release reset, probe. The only blocker. */
udsp_t *udsp_open(const struct udsp_config *cfg, int event_fd);
/* fastpath: release-only and idempotent: the safe linefeed and the stream
 * stop belong to udsp_stop. Blocks only for the bounded thread joins. */
void udsp_close(udsp_t *s);
/* fastpath: probed values, valid until close. */
const struct udsp_cap *udsp_cap(udsp_t *s);
/* fastpath: counters incl. the tdm stall count. */
void udsp_counters(udsp_t *s, struct udsp_counters *out);

/* slow: full SLIC init and calibration, ALSA confirmed running. */
enum udsp_status udsp_start(udsp_t *s, struct udsp_job *job);
/* slow: orderly teardown. */
enum udsp_status udsp_stop(udsp_t *s, struct udsp_job *job);

/* --- line control (SYNC-SLIC unless noted) --- */
enum udsp_status udsp_line_state(udsp_t *s, unsigned line, enum udsp_line_state st);
enum udsp_status udsp_line_polarity(udsp_t *s, unsigned line, enum udsp_polarity pol);
/* fastpath: cached hook state, never touches SPI. */
enum udsp_status udsp_line_hook_query(udsp_t *s, unsigned line, bool *offhook);
enum udsp_status udsp_line_hook_timing(udsp_t *s, unsigned line,
				       uint32_t flash_min_ms, uint32_t flash_max_ms,
				       uint32_t release_min_ms);
enum udsp_status udsp_ring_start(udsp_t *s, unsigned line, const struct udsp_ring *ring);
enum udsp_status udsp_ring_stop(udsp_t *s, unsigned line);
enum udsp_status udsp_cid_send(udsp_t *s, unsigned line, enum udsp_cid_kind kind,
			       const struct udsp_cid *cid);
/* Lamp only; VMWI FSK goes through udsp_cid_send. */
enum udsp_status udsp_mwi(udsp_t *s, unsigned line, bool on);
enum udsp_status udsp_tone_play(udsp_t *s, unsigned line, enum udsp_tone tone,
				uint32_t duration_ms);
enum udsp_status udsp_tone_stop(udsp_t *s, unsigned line);
/*
 * fast: render a call-waiting subscriber alerting signal. variant 0 is the
 * plain UDSP_TONE_CALL_WAITING tone, the unchanged default; 1 to
 * UDSP_REGION_CW_VIEW_MAX select a [cw cN] distinctive row of the active
 * region. A variant inside the bound that the region does not define falls back
 * to the plain tone. UDSP_UNSUPPORTED when variant is above the bound or the
 * region defines no call-waiting tone. duration_ms 0 lets the region cadence
 * bound the tone. Stop it through udsp_tone_stop, like any tone.
 */
enum udsp_status udsp_cw_tone_play(udsp_t *s, unsigned line, unsigned variant,
				   uint32_t duration_ms);
/*
 * The far end's digit that reached the caller by a transport the library does
 * not render (UDSP_FEAT_DTMF_PLAYOUT). It is network audio at the line: it takes
 * the port's receive gain (gain_rx plus slic_gain_rx), as decoded audio and a
 * rendered telephone event do, where a tone keeps its region level.
 */
enum udsp_status udsp_digit_play(udsp_t *s, unsigned line, char digit,
				 uint32_t duration_ms);

/* --- channel and stream (SYNC-TICK) --- */
/* SYNC-TICK; a changed SLIC gain is programmed through one machine SLIC
 * write; a failure is counted and healed at the next seize, never answered
 * to the caller; the gain pair is also programmed at LINES_UP with the
 * current value. The SLIC takes slic_gain_tx and a positive slic_gain_rx.
 * The engine applies a negative slic_gain_rx as a digital loss at once, so
 * a move between two values of zero or less writes no SLIC record. */
enum udsp_status udsp_chan_config(udsp_t *s, unsigned line, const struct udsp_chan_cfg *cfg);
/* SYNC-TICK. rtp_fd and rtcp_fd are the backend-owned, bound, connected
 * descriptors; libudsp dups them. media has no fd of its own. */
enum udsp_status udsp_stream_config(udsp_t *s, unsigned line, unsigned stream,
				    const struct udsp_media *media,
				    int rtp_fd, int rtcp_fd);
enum udsp_status udsp_stream_start(udsp_t *s, unsigned line, unsigned stream);
/* SYNC-TICK: removes the sockets from epoll and drops libudsp's dup'd fds
 * before returning, so the backend may close its originals. Admitted on a
 * dead line as a release-only op: the stream a call left on a line the
 * library declared dead must still be returned, or the line stays busy and
 * its init retry never runs. Every other op answers UDSP_FATAL there. */
enum udsp_status udsp_stream_stop(udsp_t *s, unsigned line, unsigned stream);
enum udsp_status udsp_stream_dtmf(udsp_t *s, unsigned line, unsigned stream,
				  char digit, uint32_t duration_ms);
/* fastpath: engine-written stats behind the double-buffer snapshot. Always
 * available, so the hal/udsp backend implements the SLOW hal
 * stream_stats_start by reading this on its worker and writing JOB_DONE. */
enum udsp_status udsp_stream_stats(udsp_t *s, unsigned line, unsigned stream,
				   struct udsp_media_stats *out);
/* fastpath: the engine-written jitter buffer counters behind a double-buffer
 * snapshot published every tick. */
enum udsp_status udsp_stream_jb_stats(udsp_t *s, unsigned line, unsigned stream,
				      struct udsp_jb_stats *out);
/* fastpath: the engine-written line echo canceller state behind a
 * double-buffer snapshot published every tick, the udsp_stream_jb_stats shape.
 * Per line, because the canceller is. A line with NO canceller instance (no
 * stream configured since the last release, core/stream.c stream_lec_ensure
 * and stream_lec_release) answers UDSP_UNSUPPORTED, the answer
 * udsp_stream_jb_stats gives for a stream that cannot answer, and the caller
 * skips that line instead of reading a row of zeros. Every line that holds an
 * instance answers UDSP_OK, whatever policy or configuration the frame ran
 * under: a canceller the modem policy skips whole, one the channel
 * configuration switched off, or one on an open loop (loop_open 1) reads
 * ec_active 0 with its counters standing still, and a canceller that has run
 * no frame yet reads frames 0. A dead line answers UDSP_FATAL. */
enum udsp_status udsp_line_lec_stats(udsp_t *s, unsigned line,
				     struct udsp_lec_stats *out);

/* SYNC-TICK: clear the line's tone-disabler latch.
 * On a standing latch, post UDSP_EV_LEC_POLICY with RELEASE_CALLER and kind
 * OFF. The local carrier freeze follows the detector's own edges and is not
 * touched.
 * If no latch stands, the call returns UDSP_OK, changes nothing and posts
 * nothing.
 * The next answer-tone start can latch again.
 * This entry does not change a stream's described kind.
 *
 * A bad line returns UDSP_UNSUPPORTED. A stopped or parked engine returns
 * UDSP_TRANSIENT. A fatal engine or dead line returns UDSP_FATAL. */
enum udsp_status udsp_line_lec_latch_clear(udsp_t *s, unsigned line);

/*
 * The newest line-monitor reading of one analog line. The slic thread samples
 * every line on a 10 s cadence while it idles, on a fixed pass count while it
 * polls hooks, and once at every fault edge; the getter copies
 * the double-buffer snapshot, the shape udsp_line_lec_stats uses.
 *
 * valid is 0 until the first reading arrives; the measurement fields, the raw
 * words, their decode and stamp_ms are then unset, while the three fault
 * fields are always defined. stale is 1 when the last read answered no data
 * (the vendor API holds the channel disabled after a failed converter
 * start); the measurement fields then carry the previous reading and
 * stamp_ms the time of THAT reading. stamp_ms is CLOCK_MONOTONIC
 * milliseconds modulo 2^32. It shares no origin with the stamp_ms of an
 * event, which is engine time (the tick count times the tick period), so
 * the two cannot be compared directly. Units are those of the plug-in
 * contract: mV, uA, mW; the raw
 * words pd_dcdc, dcdc_status, lcrrtp and linefeed sit beside their decode.
 *
 * The decoded booleans read the raw words: dcdc_down is PD_DCDC bit 20, ovlo
 * is DCDC_STATUS bit 22, settled is LINEFEED requested LF[2:0] equal to the
 * actual LFS[6:4] in an active or on-hook-transmission state, and voc_err,
 * lcr and rtp are LCRRTP bits 3, 1 and 0. vbat_tgt_mv reads 0 until the
 * converter has come up, so a deficit against it means nothing before that.
 *
 * fault_bits, fault_open and fault_held are the engine's fault state of the
 * line at the time of the CALL, not of the sample: the last set the slic
 * thread read, whether the feed is held open for the fault trial or a
 * recovery, and whether a hold is in force that no fault trial ends. Such a
 * hold lifts on the next start, or, for a clock fault alone, when the clocks
 * return.
 */
struct udsp_monitor {
	uint32_t stamp_ms;		/* monotonic, the reading's instant */
	uint8_t valid;
	uint8_t stale;
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
	uint8_t dcdc_down;
	uint8_t ovlo;
	uint8_t settled;
	uint8_t voc_err;
	uint8_t lcr;
	uint8_t rtp;
	uint32_t fault_bits;
	uint8_t fault_open;
	uint8_t fault_held;
};
/* fast: copies the newest line-monitor reading. UDSP_UNSUPPORTED when the
 * SLIC backend offers no monitor_read op or the line is outside the
 * capability; UDSP_OK otherwise, with valid 0 before the first sample so the
 * caller can render a never row. A dead line answers UDSP_FATAL, as
 * udsp_line_lec_stats does. */
enum udsp_status udsp_line_monitor(udsp_t *s, unsigned line,
				   struct udsp_monitor *out);
/* fastpath: relaxed atomic store of enum udsp_debug_flag bits; the engine
 * reads them once per tick; zero cost while clear. */
void udsp_debug_set(udsp_t *s, uint32_t flags);
/* fastpath, one consumer thread: pops up to cap records of the decision trace
 * (oldest first), returns the count; 0 when the trace is off or empty. */
unsigned udsp_jb_trace_read(udsp_t *s, struct udsp_jb_trace *out, unsigned cap);
/*
 * Control thread: arm or disarm the plaintext packet trace. It answers 0 or a
 * negative errno, which is why it is not a flag of udsp_debug_set: the memory
 * behind the trace is 1.2 MiB per handle and its allocation can fail.
 *
 * on true allocates the two rings if the handle holds none, makes every page of
 * them resident so the producers never fault, and raises UDSP_DEBUG_PKT_TRACE.
 * A second call finds the rings and allocates nothing. -ENOMEM is the only
 * failure, and it leaves the bit clear.
 *
 * on false lowers the bit and keeps the memory, so a later arm costs no
 * allocation. udsp_close frees the rings.
 *
 * A NULL handle answers -EINVAL. -ENOMEM is the only other answer, and it
 * leaves both rings absent and the bit clear, whatever the caller set through
 * udsp_debug_set before the call.
 *
 * The call reads and writes the debug mask under the handle's control mutex,
 * while udsp_debug_set writes the whole mask without it. Both belong to the
 * control thread; a udsp_debug_set from another thread can therefore drop the
 * bit this call raised.
 */
int udsp_pkt_trace_enable(udsp_t *s, bool on);
/* fastpath, one consumer thread (the udsp_jb_trace_read rule): pops up to cap
 * records, the transmit ring first and the receive ring after it, oldest first
 * inside each; returns the count, 0 when no ring exists or both are empty. It
 * reads the rings and not the debug bit, so the final drain after a disarm
 * still answers every record the producers left behind. */
unsigned udsp_pkt_trace_read(udsp_t *s, struct udsp_pkt_trace *out,
			     unsigned cap);
/*
 * Control thread: arm or disarm the canceller capture. It answers 0 or a
 * negative errno, for the reason udsp_pkt_trace_enable gives. The ring behind
 * the capture is 1 MiB per handle, one ring for every line, and its
 * allocation can fail.
 *
 * on true allocates the ring if the handle holds none, makes every page of it
 * resident so the producer never faults, and raises UDSP_DEBUG_LEC_CAPTURE. A
 * second call finds the ring and allocates nothing. The call discards no
 * record: a caller that wants a clean start drains the ring first.
 *
 * on false lowers the bit and keeps the memory. It returns after the engine
 * tick acknowledged a command posted after the clear. That tick loads the bit
 * after it takes the command, so no producer holds a record half written. The
 * drain that follows therefore answers every record. A handle that holds no
 * ring returns 0 at once. So does a handle whose engine never started,
 * stopped cleanly or is parked, because it has no producer. udsp_close frees
 * the ring.
 *
 * A NULL handle answers -EINVAL. An arm that cannot allocate answers -ENOMEM
 * and leaves the ring absent and the bit clear. A disarm whose tick does not
 * acknowledge within the SYNC-TICK bound answers -ETIMEDOUT with the bit
 * clear. A late tick can then still publish one record per line after the
 * drain. A command ring with no free slot also answers -ETIMEDOUT, because
 * only commands whose wait expired fill it. A disarm after an engine stop that
 * timed out answers -ETIMEDOUT with the bit clear too. The tick thread did not
 * join, so the call cannot show that the producer finished.
 *
 * The call moves the bit under the handle's control mutex, and the rule of
 * udsp_pkt_trace_enable about udsp_debug_set applies.
 */
int udsp_lec_capture_enable(udsp_t *s, bool on);
/* fastpath, one consumer thread (the udsp_jb_trace_read rule): pops up to cap
 * records, oldest first, every line in one ring. It returns the count, 0 when
 * no ring exists or it is empty. It reads the ring and not the debug bit, so
 * the final drain after a disarm answers every record. */
unsigned udsp_lec_capture_read(udsp_t *s, struct udsp_lec_capture *out,
			       unsigned cap);
/*
 * fastpath: the description generation of one stream, the value the records of
 * udsp_stream_config's live description carry. It answers 0 for a stream that
 * never took a description and for a line or stream outside the capability.
 *
 * A caller that keeps the endpoints of each description reads this right after
 * a udsp_stream_config that answered UDSP_OK, and keys its table with the
 * answer. A configuration that fails after it applied the description raises
 * the counter too, so this is not a count of calls or of calls that worked;
 * the generation such a failure consumed belongs to no description and no
 * record can carry it.
 */
uint32_t udsp_stream_gen(udsp_t *s, unsigned line, unsigned stream);

struct udsp_stream_diag {
	uint64_t net_drops;	/* full ring: newest datagrams refused */
	uint64_t net_stale;	/* full ring: oldest datagrams dropped */
	uint64_t net_trunc;	/* datagrams too long or rejected by SRTP */
	uint64_t tx_fail;
	uint64_t punch_sent;	/* empty datagrams sent successfully */
	uint64_t srtp_protect_ok;
	uint64_t srtp_protect_fail;
	uint64_t srtp_unprotect_ok;
	uint64_t srtp_unprotect_auth_fail;
	uint64_t srtp_unprotect_replay;
};

struct udsp_engine_diag {
	uint64_t serial;
	uint64_t ticks;
	/* Tick interval histogram. Upper edges 9, 11, 12, 15, 20, 30 and 50 ms;
	 * the last bucket holds everything above 50 ms. */
	uint64_t interval[8];
	uint64_t interval_min_us;
	uint64_t interval_max_us;
	uint64_t capture_stalls;
	uint64_t delay_stalls;
	uint64_t xruns;
	uint64_t holds;
	uint64_t capture_drops;
	uint64_t trim_drops;
	uint64_t playback_expired;
	uint64_t recover_failures;
	uint64_t read_errors;
	uint64_t write_errors;
	uint64_t cal_restarts;
	int32_t delay_ref;
	int32_t loop_ref;
	int32_t delay_drift_min;
	int32_t delay_drift_max;
	uint32_t granted_periods;
	uint32_t period_us;
	bool running;
	bool stopped_fatal;
	bool delay_calibrated;
};

/*
 * fastpath, control thread: independent relaxed counter reads, with no lock or
 * cross-thread wait. The fields need not describe the same instant.
 *
 * An adoption restarts the five transport counters. This includes the first
 * description and a re-INVITE that changes the codec clock.
 * The five SRTP counters restart when a session is created and survive a rekey.
 *
 * tx_fail counts every failed media or punch send, not only EAGAIN.
 * UDSP_UNSUPPORTED for an idle stream, a bad index, or a NULL argument;
 * UDSP_FATAL for a dead stream. A refusal clears the output when supplied.
 */
enum udsp_status udsp_stream_diag(udsp_t *s, unsigned line, unsigned stream,
				  struct udsp_stream_diag *out);
/*
 * fastpath, control thread: one consistent engine snapshot, with no lock or
 * cross-thread wait. The watchdog refreshes the values once per watchdog
 * period, 1000 ms by default. A concurrent read can defer a refresh to the
 * next run.
 * UDSP_UNSUPPORTED before the first publication, with no engine, or for a
 * NULL argument. A refusal clears the output when supplied.
 */
enum udsp_status udsp_engine_diag(udsp_t *s, struct udsp_engine_diag *out);
/* SYNC-TICK. The per-mode rule is fixed, so the compact signature is
 * lossless: PASSTHROUGH sets udsp_media.passthrough to UDSP_PASSTHROUGH_MODEM,
 * which forces EC bypass, VAD/CNG/silence suppression off and the jitter
 * buffer to fixed, and clamps to codec/codec_pt; OFF restores the pre-fax
 * stream and channel state; T38 is the stream-only path (codec UDSP_SEL_T38 on
 * udsp_stream_config), it touches no channel profile. The modem kind is what a
 * CED answer proves, so it is what this op arms; the fax kind arrives later
 * through udsp_stream_config, on the description. A profile that moves the RTP
 * clock ends the source and starts a new one, the rule udsp_stream_config
 * follows for the same move. The stream description outlives the profile
 * record: after a udsp_stream_config that carries a kind, OFF finds nothing to
 * restore and answers UDSP_OK, and the caller clears the kind through the
 * description instead. */
enum udsp_status udsp_fax_profile(udsp_t *s, unsigned line, unsigned stream,
				  enum udsp_fax_mode mode, uint8_t codec, uint8_t codec_pt);
/* SYNC-TICK: three-way local mix (mix-minus). HAL_FEAT_CONF gates it. */
enum udsp_status udsp_conf(udsp_t *s, unsigned line, enum udsp_conf_mode mode);
/*
 * SYNC-TICK: the delayed loop the manufacturing test plays. SELF plays the
 * line its own capture delay_ms later. PEER plays it the capture of peer, so
 * two lines armed at each other carry a conversation between the two ports.
 * UDSP_FEAT_LOOP gates it and is always set.
 *
 * The loop carries the raw TDM capture, taken before gain_tx. The receive gain
 * and the receive loss of the port (gain_rx and slic_gain_rx) apply on the way
 * out. delay_ms rounds up to whole ticks and takes one tick at the low end, so
 * 0 asks for one tick.
 *
 * Refusals, in this order. UDSP_UNSUPPORTED for a NULL handle, a line outside
 * the capability and a mode above PEER; then, for SELF and PEER,
 * UDSP_UNSUPPORTED for a delay_ms above UDSP_LOOP_DELAY_MAX_MS, and under PEER
 * for a peer outside the capability or equal to line. Then the lifecycle
 * guard: UDSP_FATAL for a dead line or a fatal handle, UDSP_TRANSIENT while
 * the engine is not live, and under PEER the same guard on the peer. Then, for
 * SELF and PEER alone, UDSP_TRANSIENT while the line or the peer is under a
 * metallic line test, carries an active stream, or is owned by a ring session
 * or a caller identity burst.
 *
 * OFF reads neither peer nor delay_ms and takes no busy refusal. It can still
 * fail on the lifecycle guard or on the command transport.
 *
 * While a loop stands on a line, udsp_line_test, udsp_stream_config and
 * udsp_stream_start on that line answer UDSP_TRANSIENT, so the two exclude
 * each other in both directions. An arm that a metallic line test wins at the
 * tick boundary answers UDSP_TRANSIENT as well. udsp_stop disarms every loop.
 */
enum udsp_status udsp_line_loop(udsp_t *s, unsigned line,
				enum udsp_loop_mode mode, unsigned peer,
				uint32_t delay_ms);

/* --- slow diagnostics --- */
enum udsp_status udsp_line_test(udsp_t *s, unsigned line, enum udsp_line_test test,
				struct udsp_job *job);
/* fastpath: copy the last line_test result (up to cap bytes) after its
 * JOB_DONE. The reserved interface: a line test adds no field later. */
enum udsp_status udsp_line_test_result(udsp_t *s, unsigned line, void *buf,
				       size_t cap, size_t *out_len);
enum udsp_status udsp_region_load(udsp_t *s, const char *iso3, struct udsp_job *job);
/*
 * Control thread: tells whether the region file of iso3 exists. It takes no
 * handle and parses nothing. It builds the path as udsp_region_load does, so
 * the UDSP_REGION_DIR override applies and a name with a '/' is a file path.
 *
 * It answers 0 when the file exists and -ENOENT when it is absent. Every other
 * failure answers its own negative errno: -EINVAL or -ENAMETOOLONG for a name
 * that builds no path, else the errno of stat(). A file that exists but does
 * not read or parse answers 0, and the region job of that file then fails.
 */
int udsp_region_probe(const char *iso3);

/* The ring templates the region view carries: the r1..r8 of the region file
 * grammar. A template the file does not define reads cadence_count 0. */
#define UDSP_REGION_RING_VIEW_MAX	8
/* The [region] describe string with its terminator, the grammar's own bound. */
#define UDSP_REGION_DESCRIBE_MAX	64

struct udsp_region_ring_view {
	struct udsp_cadence cadence[UDSP_CADENCE_MAX];
	uint8_t cadence_count;
};

/* The distinctive call-waiting variants the region view carries: the [cw c1..cN]
 * rows of the region file grammar. */
#define UDSP_REGION_CW_VIEW_MAX		4

/*
 * One call-waiting variant. lead_ms is the first-presentation length, the point
 * after which the type 2 CAS may inject, so a caller defers the CAS by lead_ms
 * plus its own gap. total_ms is the whole alerting signal like cw_sas_total_ms
 * but for this variant. present is 0 for a variant the file omits, and the
 * caller falls back to index 0, the plain tone.
 */
struct udsp_region_cw_view {
	uint32_t total_ms;
	uint16_t lead_ms;
	uint8_t present;
};

/*
 * The public view of the active region: what a backend needs to program the
 * lines it owns, and no more. It is not the region model: the tone plans, the
 * segment forms and the pulse windows are the library's own business and no
 * caller may depend on their shape.
 *
 * cid_format is the [cid] format key and mwi_format the [cid] mwi_format key,
 * which defaults to format; both are enum udsp_cid_format. The three hook
 * windows are the [hook] keys in milliseconds, the windows the caller may
 * override per line through udsp_line_hook_timing.
 *
 * cw_sas_total_ms is the whole subscriber alerting signal: the [tone
 * call_waiting] cycle times its repeat count, trailing silence included, which
 * is exactly the playout limit the engine stops the tone at, so a caller whose
 * call waiting offer must outlast the tone computes that from this one number.
 * It reads 0 when the tone repeats forever, is segmented or carries then = on:
 * those bound nothing and the caller keeps its own timeout.
 *
 * ring_count is the number of templates the file defines, 1 to 8; ring[i] is
 * template r(i+1), the row a distinctive-ring id i selects.
 *
 * flash_service is the [hook] flash_service key, enum udsp_flash_service, a
 * policy the library never reads. describe is the [region] describe string,
 * empty when the file omits it (the key is optional). Both are appended after
 * ring[]: the record is append only, like every udsp.h contract.
 *
 * cw_count is the number of [cw cN] distinctive rows the file defines, 0 to
 * UDSP_REGION_CW_VIEW_MAX. cw[] is indexed by variant: cw[0] describes the
 * plain [tone call_waiting] tone and cw[1..N] the distinctive rows, so a caller
 * reads cw[variant].lead_ms for the deferral of every variant. cw_sas_total_ms
 * still carries cw[0].total_ms for an older reader; a caller whose offer must
 * outlast any variant takes the maximum of cw[i].total_ms. Appended after
 * describe[]: the record is append only.
 *
 * pulse_invert is the [hook] pulse_invert key, 0 when the file omits it. At 1
 * the country codes a dialed N as 10 - N pulses, and the digit 0 as 10. The
 * view carries it for the read-back. A UDSP_HOOK_PULSE event carries the count
 * and the coding of the region that counted the train (event.h u.hook). The
 * caller maps the count with the coding of the event. Appended after cw[]: the
 * record is append only.
 *
 * confirm_total_ms is the whole [tone confirm] signal: one cycle times its
 * repeat count, trailing silence included, in the simple and the segmented
 * form. The engine stops the tone at that bound, so a caller can start the
 * next tone when the confirm tone ends. It reads 0 when the tone repeats
 * forever or carries then = on: such a tone plays until the caller stops it.
 * Appended after pulse_invert: the record is append only.
 */
struct udsp_region_view {
	char iso3[UDSP_ISO3_MAX];
	uint8_t cid_format;		/* enum udsp_cid_format */
	uint8_t mwi_format;		/* enum udsp_cid_format */
	uint8_t ring_count;
	uint32_t hook_flash_min_ms;
	uint32_t hook_flash_max_ms;
	uint32_t hook_release_min_ms;
	uint32_t cw_sas_total_ms;
	struct udsp_region_ring_view ring[UDSP_REGION_RING_VIEW_MAX];
	uint8_t flash_service;		/* enum udsp_flash_service */
	char describe[UDSP_REGION_DESCRIBE_MAX];
	uint8_t cw_count;
	struct udsp_region_cw_view cw[UDSP_REGION_CW_VIEW_MAX + 1];
	uint8_t pulse_invert;
	uint32_t confirm_total_ms;
};

/*
 * fastpath: copies the view of the active region from a double-buffer
 * snapshot, the udsp_line_monitor shape. The region job's thread publishes a
 * fresh view after every swap (and the open publishes the initial one), so
 * the view always describes the table the engine plays from: during a load
 * the old view stands until the swap completes. UDSP_UNSUPPORTED before the
 * first region file parses, because no table is active then; UDSP_OK
 * otherwise, with the record untouched on the refusal.
 */
enum udsp_status udsp_region_get(udsp_t *s, struct udsp_region_view *out);

static inline const char *udsp_status_name(enum udsp_status st)
{
	switch (st) {
	case UDSP_OK: return "ok";
	case UDSP_TRANSIENT: return "transient";
	case UDSP_UNSUPPORTED: return "unsupported";
	case UDSP_FATAL: return "fatal";
	}
	return "unknown";
}

#endif
