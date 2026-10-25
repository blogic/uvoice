// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * One media stream. Rules. Three threads touch this object, and the split
 * below is what keeps them apart:
 *
 *   net thread     fills the SPSC ring below and nothing else. It recvs, stamps
 *                  CLOCK_MONOTONIC and unprotects SRTP in place on the receive
 *                  session, which uvsrtp.h reserves for it.
 *   engine tick    owns every byte of RTP, RTCP, telephone-event, jitter buffer
 *                  and codec state. rtp_packet_parse and rtp_receiver_report
 *                  both mutate struct rtp_receiver and rtp_stats is read whole
 *                  by the snapshot, so splitting them across threads would
 *                  race the snapshot.
 *   control thread creates and destroys the codec instances, the SRTP object
 *                  and the descriptors, and only ever while the other two are
 *                  quiesced at the tick boundary (udsp.h section 3).
 *
 * The engine reads the control-owned members (media, codec ops, descriptors)
 * only for a stream it has activated, and every activation travels the command
 * ring, whose release and acquire pair is what publishes those writes.
 */
#ifndef UDSP_CORE_STREAM_H
#define UDSP_CORE_STREAM_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "udsp/codec.h"
#include "udsp/fax.h"
#include "udsp/udsp.h"

#include "../dsp/dsp.h"
#include "../dsp/lec.h"
#include "../dsp/resamp.h"
#include "../rtp/jb.h"
#include "../rtp/rtp.h"

#include <uvsrtp/uvsrtp.h>

#include "snap.h"

/* 32 slots of 576 bytes per stream, the recvmmsg budget. 576 is the IPv4
 * minimum reassembly buffer: a datagram above it is counted and dropped
 * rather than truncated into the parser. */
#define UDSP_NET_SLOTS		32u
#define UDSP_NET_DGRAM_MAX	576u
/* At most this many slots per stream per tick reach the parser; the rest of a
 * burst is the oldest audio and is dropped and counted. */
#define UDSP_NET_DRAIN_MAX	16u
/* uvsrtp.h: protect needs len + UVSRTP_MAX_SRTCP_TRAILER (148 bytes) of
 * headroom and a 32-bit aligned start. */
#define UDSP_PKT_CAP		(UDSP_NET_DGRAM_MAX + 160u)
/* The transmit staging. codec.h bounds every codec's max_payload by
 * UDSP_CODEC_PAYLOAD_MAX, and media_prepare refuses a codec that asks for
 * more, so the aggregate of one packet always fits and the encode capacity is
 * what is left of it. The PCM staging holds the widest packet a whole-packet
 * codec can stage, three ticks of the wideband frame. */
#define UDSP_PCM_STAGE_MAX	(3u * UDSP_FRAME_MAX)
/* RFC 3550 6.2 floors the RTCP interval at five seconds. */
#define UDSP_RTCP_MIN_MS	5000u
#define UDSP_JB_EVENT_MIN_MS	1000u
/* A stream that is not playing frames still says so at this cadence, so a
 * reader can tell a long outage from a trace that stopped. */
#define UDSP_JB_TRACE_BEAT_MS	1000u
#define UDSP_STATS_PERIOD_MS	100u
/* RFC 4733 volume in -dBm0 of a digit the line never heard (udsp_stream_dtmf),
 * the level plan's digit level (line.h). A detected digit carries the row
 * level that the detector measured (chan.h digit_level_dbm0). */
#define UDSP_TELEV_VOLUME	10u
#define UDSP_TELEV_DEFAULT_MS	100u
/* RFC 4733 2.5.1.4: a packet that ends something is sent three times. It
 * applies to the closing packet of an intermediate segment as much as to the
 * last packet of the event: a single lost close takes a whole segment, 65535
 * ticks, out of the duration the far end reports. */
#define UDSP_TELEV_REPEAT	3u
#define UDSP_CNAME_MAX		64u
/* A comfort-noise packet is repeated at this cadence while the talkspurt stays
 * closed, so a receiver that joined late still learns the level. */
#define UDSP_CN_REPEAT_MS	1000u

enum net_kind {
	NET_KIND_RTP = 0,
	NET_KIND_RTCP,
};

enum stream_state {
	STREAM_IDLE = 0,
	STREAM_READY,
	STREAM_RUNNING,
};

/*
 * Which chain the tick runs for this stream. The description decides it:
 * udsp_media.codec UDSP_SEL_T38 is the T.38 mode and every other selector is
 * a codec. The two chains share the line frame, the direction and the
 * descriptors and nothing else.
 */
enum stream_mode {
	STREAM_MODE_AUDIO = 0,
	STREAM_MODE_T38,
};

/* What the gateway instance was created with, so a description that moves one
 * of them recreates it (the codec_params rule applied to udsp/fax.h create).
 * media_prepare resolves all three before they get here. */
struct udsp_t38_params {
	uint32_t max_bit_rate;
	uint16_t max_datagram;
	uint8_t ec;
};

/* The T.38 gateway, built and installed the way the codec pair is: aside
 * first, so a configuration that fails later keeps the gateway the stream
 * runs, and an empty gate means the description asks for the one it has. */
struct fax_gate {
	const struct udsp_fax_ops *ops;
	struct udsp_fax *inst;
	struct udsp_t38_params params;
};

enum stream_op {
	STREAM_OP_ADOPT = 0,	/* first configuration: init every machine */
	STREAM_OP_RECONFIG,	/* keep SSRC, sequence and timestamp */
	STREAM_OP_ACTIVATE,
	STREAM_OP_DEACTIVATE,	/* flags bit 0: emit BYE before it goes quiet */
	STREAM_OP_DTMF,
	/* CONF is line scoped: arg1 is the enum udsp_conf_mode. */
	STREAM_OP_CONF,
	/* LOOP is line scoped too: arg1 packs the enum udsp_loop_mode in bits
	 * 0 to 7, the peer line in bits 8 to 15 and the delay in frames in
	 * bits 16 to 31. */
	STREAM_OP_LOOP,
};

/* What UDSP_FAX_PASSTHROUGH displaced in the stream description, so
 * UDSP_FAX_OFF puts back exactly what it found. Control thread only, and
 * cleared with the description it belongs to. */
struct stream_fax_saved {
	bool armed;
	bool vad;
	bool cng;
	bool dtx;
	uint8_t passthrough;		/* enum udsp_passthrough */
	uint8_t codec;
	uint8_t codec_pt;
	uint8_t jb_mode;
};

struct net_slot {
	uint64_t arrival_ns;
	uint32_t len;
	uint8_t kind;
	_Alignas(uint32_t) uint8_t data[UDSP_NET_DGRAM_MAX];
};

struct net_ring {
	struct net_slot slot[UDSP_NET_SLOTS];
	atomic_uint head;		/* net thread */
	atomic_uint tail;		/* engine tick */
};

/* The epoll cookie: one per descriptor, so a wake names its stream and its
 * kind without a search. */
struct net_reg {
	struct udsp_stream *stream;
	uint8_t kind;
};

UDSP_SNAP_DECLARE(media_snap, struct udsp_media_stats);
/* The jitter buffer counters get their own snapshot: the engine republishes
 * the E-model behind the media statistics at a diagnostic cadence, and these
 * counters every tick. */
UDSP_SNAP_DECLARE(jb_snap, struct udsp_jb_stats);

struct udsp_stream {
	uint8_t line;
	uint8_t index;
	/* Written by the control thread, read by udsp_stream_stats, which is a
	 * FASTPATH and takes no lock (udsp.h section 1). */
	_Atomic uint_least8_t state;	/* enum stream_state */
	/*
	 * The bounded-stop rule applied per stream. A step of the quiesce
	 * protocol that did not complete leaves a thread that may still hold
	 * the descriptors, the SRTP session or the codec instances, so nothing
	 * here is released. Every public op on the stream answers UDSP_FATAL
	 * and stream_pool_shutdown does the teardown, from udsp_close, after
	 * every thread has stopped.
	 */
	_Atomic bool dead;

	struct udsp_media media;	/* control thread; zeroized at stop */
	/*
	 * The description generation (udsp.h struct udsp_pkt_trace):
	 * stream_config raises it on every description it applies, and every
	 * packet trace record carries the value its producer read. The control
	 * thread is the one writer; the engine tick and the net thread read it
	 * relaxed at the tap. It counts the descriptions of the handle's life
	 * and a stop does not reset it, so no two descriptions of one stream
	 * share a value.
	 */
	_Atomic uint_least32_t gen;
	const struct udsp_codec_ops *ops;
	/* The T.38 gateway, control thread, empty outside the mode: the vtable
	 * of the loaded plug-in, the instance it made and the values it was
	 * made with (core/fax_plugin.h). */
	struct fax_gate fax_gate;
	/* What the live instances were created with, so a description that
	 * moves a parameter recreates them. */
	struct udsp_codec_params params;
	struct udsp_codec *enc;
	struct udsp_codec *dec;
	struct uvsrtp *srtp;
	bool srtp_live;			/* control thread: an object exists */
	int rtp_fd;			/* libudsp's dup, -1 when none */
	int rtcp_fd;
	struct rtp_entropy entropy;	/* three independent getrandom draws */
	char cname[UDSP_CNAME_MAX];

	/* Read by the net thread and by the engine, written by the control
	 * thread only while both are quiesced (uvsrtp.h thread contract). */
	_Atomic bool srtp_armed;
	_Atomic bool active;

	struct net_ring ring;
	struct net_reg reg[2];
	bool attached;			/* net thread only */
	int net_rtp_fd;			/* net thread only */
	int net_rtcp_fd;
	_Atomic uint_least64_t net_drops;	/* ring full, newest refused */
	_Atomic uint_least64_t net_stale;	/* drain bound, oldest dropped */
	_Atomic uint_least64_t net_trunc;	/* MSG_TRUNC or unprotect failed */

	struct rtp_stats stats;
	struct rtp_sender sender;
	struct rtp_receiver receiver;
	struct rtcp_state rtcp;
	struct telev_sender tev_tx;
	struct telev_receiver tev_rx;
	/* Engine only: the wire event this stream renders toward the line and
	 * the generator it renders with (rtp.h struct telev_play). */
	struct telev_play tev_play;
	struct udsp_dtmf_generator tev_gen;
	struct udsp_jb jb;
	struct udsp_vad vad;
	struct media_snap snap;
	struct jb_snap jb_snap;
	/*
	 * True once a tick of this configuration has published into jb_snap.
	 * A stream that was adopted but never ticked still holds the previous
	 * call's counters in the slot, and the engine may not spin to replace
	 * them, so the getter refuses instead.
	 */
	_Atomic bool jb_snap_valid;

	struct stream_fax_saved fax;	/* control thread */
	/* Engine only (enum udsp_passthrough): what this stream carries while
	 * it is active. ADOPT and RECONFIG derive it from the committed
	 * description, the T.38 mode reading as the modem kind, so a stop, or
	 * a description that drops the kind, lifts the line policy with no
	 * control-thread write of its own. */
	uint8_t passthrough_kind;
	/* Engine only: the playout depth the buffer had when the passthrough
	 * began, in ms, and 0 outside one. Every later description of the same
	 * passthrough re-inits the buffer at this depth instead of the
	 * configured target, because the depth the line adapted to is what the
	 * carrier needs (jb.h udsp_jb_mode_set). */
	uint16_t jb_pin_ms;
	/* Engine only: what the live jitter buffer was configured for. A
	 * description that keeps both keeps the slicing rule and the learned
	 * span, so the mode may move without a re-init. */
	const struct udsp_codec_ops *jb_ops;
	uint8_t jb_ptime_ms;
	/* Engine only (enum stream_mode): the chain this tick runs. ADOPT and
	 * RECONFIG publish it from the committed description, so it changes at
	 * a tick boundary with the rest of the engine view. */
	uint8_t mode;
	/*
	 * The mode of the description the net thread is attached for, which is
	 * what tells a UDPTL datagram from an RTP one on the one descriptor
	 * both travel (core/net.c). It is not the field above: the control
	 * thread publishes it in stream_activate, before the descriptors enter
	 * the epoll set, and nothing moves it while they are inside it. The
	 * stop therefore leaves it standing, because a datagram that arrives
	 * before the detach still belongs to the session that is ending.
	 */
	_Atomic uint_least8_t net_mode;

	bool rx_seen;			/* UDSP_EV_RTP_FIRST_RX emitted */
	bool tx_seen;
	bool silent;			/* the VAD holds the talkspurt closed */
	uint8_t frames_per_packet;	/* ptime / 10 */
	uint8_t agg_frames;
	uint16_t payload_len;
	uint8_t payload[UDSP_CODEC_PAYLOAD_MAX];
	int16_t dl[UDSP_FRAME_MAX];
	/*
	 * Rate conversion between the line and the codec. codec_rate is the
	 * PCM rate the codec works at: 8000 for G.711 and G.726, 16000 for
	 * G.722, and the line rate for a codec that follows the line. The flag
	 * below is true when the two rates differ; that is the only condition
	 * under which a resampler here runs. At a matched rate the codec reads
	 * and writes the line frame itself.
	 *
	 * One resampler instance per feed (dsp/resamp.h): the three feeds
	 * advance on different ticks, so a shared history would jump. rs_rx
	 * carries every output unit the jitter buffer decides, audio,
	 * concealment, comfort noise and silence alike. rs_tx carries the
	 * frames the encoder consumes, so it stands still through a closed
	 * talkspurt. rs_vad carries the frame the voice detector and the
	 * RFC 3389 level measurement read, which is the feed that keeps
	 * running while the talkspurt is closed.
	 */
	bool resample;
	unsigned codec_rate;
	unsigned codec_frame;		/* codec_rate / 100 */
	unsigned line_frame;		/* the line's rate / 100 */
	uint32_t ts_per_frame;		/* clock_rate units in one 10 ms tick */
	struct udsp_resamp rs_tx;
	struct udsp_resamp rs_rx;
	struct udsp_resamp rs_vad;
	int16_t tx_pcm[UDSP_PCM_STAGE_MAX];
	int16_t rx_pcm[UDSP_PCM_STAGE_MAX];
	int16_t vad_pcm[UDSP_FRAME_MAX];
	/*
	 * The whole-packet receive staging (jb.h Units). unit_packet says the
	 * codec cannot be sliced by bytes, so the jitter buffer hands one
	 * packet out whole and marks its later slices. The unit is decoded
	 * once into rx_unit and one 10 ms slice of it plays per tick. It is
	 * its own buffer, not rx_pcm: a concealed or comfort-noise tick
	 * between two slices of a unit writes rx_pcm, and the slices after it
	 * must still find their audio here.
	 */
	bool unit_packet;
	bool rx_unit_valid;
	uint8_t rx_unit_slices;
	uint32_t rx_unit_ts;
	int16_t rx_unit[UDSP_PCM_STAGE_MAX];
	uint32_t cn_next_ms;
	uint32_t rtcp_next_ms;
	/* rtp_tx_ms and rtcp_tx_ms hold the last successful send times on
	 * their descriptors, in engine milliseconds. Every successful send
	 * refreshes its descriptor's stamp, so media and reports defer punches.
	 * punch_sent counts successful empty datagrams that hold idle tuples
	 * open. The tick owns the stamps; control also reads the counter. */
	uint32_t rtp_tx_ms;
	uint32_t rtcp_tx_ms;
	_Atomic uint_least64_t punch_sent;
	uint32_t stats_next_ms;
	uint32_t tev_elapsed_ms;
	uint32_t tev_target_ms;
	uint32_t tev_seg_ts;		/* what the closed segments carry */
	/* Engine only: the event follows the line's own digit, so the digit
	 * the line holds closes it and tev_target_ms holds the 8000 ms ceiling
	 * alone. Clear for an event udsp_stream_dtmf started, which runs for
	 * the duration the caller gave. tev_digit is the digit a native event
	 * carries, and the transmit tick closes the event as soon as the line
	 * no longer holds it. */
	bool tev_native;
	char tev_digit;
	struct udsp_jb_stats jb_last;
	uint32_t jb_event_ms;
	/* Engine only: the last output decision the trace recorded for this
	 * stream and the instant it recorded it. Steady playout repeats the
	 * key every tick, which is what keeps clean audio out of the ring. */
	uint32_t jb_trace_ms;
	uint16_t jb_trace_key;
	bool jb_trace_have;
	_Atomic uint_least64_t tx_eagain;
	/*
	 * Engine only, the T.38 mode. The datagrams and bytes each way are
	 * what the statistics snapshot reports for the session; dropped counts
	 * a datagram the direction or the descriptor refused, and errors counts
	 * a gateway call that reported one. They live here and not in the RTP
	 * record because no RTP receiver ran: a reader of that record would
	 * turn them into a report of a source this mode has none of.
	 */
	uint64_t t38_sent;
	uint64_t t38_recv;
	uint64_t t38_bytes_sent;
	uint64_t t38_bytes_recv;
	uint64_t t38_dropped;
	uint64_t t38_errors;
	_Alignas(uint32_t) uint8_t pkt[UDSP_PKT_CAP];
};

struct udsp;
struct udsp_chan;
struct udsp_cmd;

enum udsp_status stream_config(struct udsp *s, unsigned line, unsigned stream,
			       const struct udsp_media *media, int rtp_fd,
			       int rtcp_fd);
enum udsp_status stream_start(struct udsp *s, unsigned line, unsigned stream);
enum udsp_status stream_stop(struct udsp *s, unsigned line, unsigned stream);
enum udsp_status stream_dtmf(struct udsp *s, unsigned line, unsigned stream,
			     char digit, uint32_t duration_ms);
enum udsp_status stream_stats(struct udsp *s, unsigned line, unsigned stream,
			      struct udsp_media_stats *out);
enum udsp_status stream_jb_stats(struct udsp *s, unsigned line, unsigned stream,
				 struct udsp_jb_stats *out);
enum udsp_status stream_diag(struct udsp *s, unsigned line, unsigned stream,
			     struct udsp_stream_diag *out);
/* FASTPATH: the reader half of the gen field below, one relaxed load. */
uint32_t stream_gen(struct udsp *s, unsigned line, unsigned stream);
enum udsp_status stream_fax_profile(struct udsp *s, unsigned line,
				    unsigned stream, enum udsp_fax_mode mode,
				    uint8_t codec, uint8_t codec_pt);
enum udsp_status stream_conf(struct udsp *s, unsigned line,
			     enum udsp_conf_mode mode);
/* The arm and the disarm of the delayed loop (udsp.h udsp_line_loop). The
 * caller checked every argument and every guard; this posts the tick command
 * and waits for it, and the tick writes the record the control-side guards
 * read (udsp_priv.h loop_armed). */
enum udsp_status stream_line_loop(struct udsp *s, unsigned line,
				  enum udsp_loop_mode mode, unsigned peer,
				  uint32_t delay_ms);

int stream_pool_alloc(struct udsp *s);
void stream_pool_free(struct udsp *s);
void stream_pool_shutdown(struct udsp *s);
struct udsp_stream *stream_at(struct udsp *s, unsigned line, unsigned stream);
/* The engine's half of UDSP_CMD_STREAM; the status goes with the ack. */
int stream_cmd_run(struct udsp *s, const struct udsp_cmd *cmd, uint32_t now_ms);
/* Both halves of one tick, called from the chan.c media hooks. */
void stream_tick_rx(struct udsp *s, struct udsp_chan *c, int16_t *frame,
		    uint32_t now_ms);
void stream_tick_tx(struct udsp *s, struct udsp_chan *c, const int16_t *frame,
		    uint32_t now_ms);
/* periods the engine never ran; now_ns is the skip hook's own
 * CLOCK_MONOTONIC stamp, the base of the jitter buffer and of the arrival
 * stamps. */
void stream_tick_skip(struct udsp *s, struct udsp_chan *c, unsigned periods,
		      uint64_t now_ns, uint32_t now_ms);
void stream_tick_tx_synthetic(struct udsp *s, struct udsp_chan *c,
			      uint32_t now_ms);
bool stream_line_active(const struct udsp *s, unsigned line);
/* What the line carries: the strongest kind over its governing streams, the
 * modem kind winning over the fax kind, so the line's one canceller gets one
 * policy. It is the description half of that policy; chan.c
 * chan_passthrough_get folds it with the tone disabler's latch and is what
 * chan_lec_run reads. */
enum udsp_passthrough stream_line_passthrough(const struct udsp *s,
					      unsigned line);
/* How many streams of the line govern it, by the same rules. The tone
 * disabler records this count when it latches and releases the latch when the
 * count moves, because a latch describes the topology it was set on. */
unsigned stream_line_govern_count(const struct udsp *s, unsigned line);

/*
 * Test hooks, engine-internal otherwise. Two contract corners are hard to
 * reach on a live stream. The jitter buffer sets udsp_jb_out.cn_sid only on a
 * stream that states a SID length (jb.h SID). The fixed-frame flush-first
 * rules fire on encode returns (0 with staged frames, an error) that no
 * built-in codec produces on a live stream, because the G.729 encoder always
 * closes a talkspurt with a SID. test_media fabricates the stream and
 * the returns; nothing else calls these two from outside stream.c.
 *
 * stream_tx_media transmits one 10 ms frame. The fabricated stream needs ops,
 * enc, codec_frame, ts_per_frame, frames_per_packet, an rtp_fd and a settled
 * sender, and tx_seen true so no event is emitted (s may then be NULL).
 * stream_conceal_unit fills one concealment unit from the buffer's decision.
 */
void stream_tx_media(struct udsp *s, struct udsp_stream *st,
		     const int16_t *frame, unsigned n, uint32_t now_ms);
void stream_conceal_unit(struct udsp_stream *st,
			 const struct udsp_jb_out *out, int16_t *pcm,
			 unsigned n);

#endif
