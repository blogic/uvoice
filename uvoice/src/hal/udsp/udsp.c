// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Adtran softDSP HAL backend: the hal_ops table on top of libudsp
 * (udsp/src/include/udsp/udsp.h, the stable contract).
 *
 * The whole file is a translation. libudsp was specified against hal.h, so
 * every op below is one udsp call plus a status map, and the two op classes
 * line up: an udsp SYNC-SLIC or SYNC-TICK op is a hal FAST op (it blocks on a
 * bounded ack, udsp.h section 1), an udsp SLOW op is a hal SLOW op (it
 * returns a token and completes on the event fd). Nothing here starts a
 * thread, and nothing here queues work of its own.
 *
 * The media plane is the one part that is more than a call. libudsp never
 * creates a socket (udsp.h section 3), so the media ops carry the socket
 * ownership of hal.h stream_config_set as well as the translation: this
 * backend makes, binds, marks, connects and closes the two descriptors of
 * every stream through the shared hal_media.c, and libudsp dups them.
 *
 * The engine stall is consumed both ways: through the return codes, and
 * through the main.c HAL_EVENT_SLIC_FAULT arm. See the fault note in
 * udsp_events.c.
 */
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "udsp_int.h"
#include "../hal_media.h"
#include "../../posix.h"
#include "../../str.h"

/* The log sink relays the library's lines to the call trace rings. */
#include "../../trace_ring.h"

#ifndef UDSP_TX_PUNCH_DEFAULT_MS
#error "libudsp 0.3.0 or later: the media punch holds the conntrack tuples"
#endif

const char *hal_udsp_fxsnum_path = HAL_UDSP_FXSNUM_RUN;

/*
 * The two status enums carry the same four members in the same order, and the
 * switch is still written out: a renumbering on either side has to break a
 * build here rather than turn a fatal answer into a transient one.
 */
enum hal_status hal_udsp_status_map(enum udsp_status st)
{
	switch (st) {
	case UDSP_OK:
		return HAL_OK;
	case UDSP_TRANSIENT:
		return HAL_TRANSIENT;
	case UDSP_UNSUPPORTED:
		return HAL_UNSUPPORTED;
	case UDSP_FATAL:
		break;
	}

	return HAL_FATAL;
}

static enum hal_status call_check(enum udsp_status rc, const char *what)
{
	enum hal_status st = hal_udsp_status_map(rc);

	if (st == HAL_OK)
		return st;

	/* libudsp counts its own op errors and publishes them through
	 * udsp_counters, so counters_get already reports this one. */
	TRACE(TRACE_HAL, "udsp: %s rc=%d -> %s", what, (int)rc,
	      hal_status_name(st));

	return st;
}

/*
 * The op enums are the same numbers on both sides, so an op is a cast and not
 * a table. Each pair is asserted by name; a member added to one enum alone, or
 * a reordering, stops the build instead of programming the wrong line state.
 */
_Static_assert((int)HAL_LINE_DOWN == (int)UDSP_LINE_DOWN, "line state DOWN moved");
_Static_assert((int)HAL_LINE_ACTIVE == (int)UDSP_LINE_ACTIVE, "line state ACTIVE moved");
_Static_assert((int)HAL_LINE_OHT == (int)UDSP_LINE_OHT, "line state OHT moved");
_Static_assert((int)HAL_LINE_BUSY == (int)UDSP_LINE_BUSY, "line state BUSY moved");
_Static_assert((int)HAL_LINE_SLEEP == (int)UDSP_LINE_SLEEP, "line state SLEEP moved");
_Static_assert((int)HAL_LINE_HOWLER == (int)UDSP_LINE_HOWLER, "line state HOWLER moved");

_Static_assert((int)HAL_POL_FORWARD == (int)UDSP_POL_FORWARD, "polarity FORWARD moved");
_Static_assert((int)HAL_POL_REVERSE == (int)UDSP_POL_REVERSE, "polarity REVERSE moved");

_Static_assert((int)HAL_TONE_NONE == (int)UDSP_TONE_NONE, "tone NONE moved");
_Static_assert((int)HAL_TONE_DIAL == (int)UDSP_TONE_DIAL, "tone DIAL moved");
_Static_assert((int)HAL_TONE_STUTTER == (int)UDSP_TONE_STUTTER, "tone STUTTER moved");
_Static_assert((int)HAL_TONE_SECOND_DIAL == (int)UDSP_TONE_SECOND_DIAL,
	       "tone SECOND_DIAL moved");
_Static_assert((int)HAL_TONE_RINGBACK == (int)UDSP_TONE_RINGBACK, "tone RINGBACK moved");
_Static_assert((int)HAL_TONE_BUSY == (int)UDSP_TONE_BUSY, "tone BUSY moved");
_Static_assert((int)HAL_TONE_REORDER == (int)UDSP_TONE_REORDER, "tone REORDER moved");
_Static_assert((int)HAL_TONE_SIT == (int)UDSP_TONE_SIT, "tone SIT moved");
_Static_assert((int)HAL_TONE_CALL_WAITING == (int)UDSP_TONE_CALL_WAITING,
	       "tone CALL_WAITING moved");
_Static_assert((int)HAL_TONE_CONFIRM == (int)UDSP_TONE_CONFIRM, "tone CONFIRM moved");
_Static_assert((int)HAL_TONE_HOWLER == (int)UDSP_TONE_HOWLER, "tone HOWLER moved");
_Static_assert((int)HAL_TONE_SHORT_STUTTER_DIAL ==
	       (int)UDSP_TONE_SHORT_STUTTER_DIAL,
	       "tone SHORT_STUTTER_DIAL moved");
_Static_assert((int)HAL_TONE_NUMBER_UNOBTAINABLE ==
	       (int)UDSP_TONE_NUMBER_UNOBTAINABLE,
	       "tone NUMBER_UNOBTAINABLE moved");

_Static_assert((int)HAL_CID_TYPE1 == (int)UDSP_CID_TYPE1, "cid kind TYPE1 moved");
_Static_assert((int)HAL_CID_TYPE2 == (int)UDSP_CID_TYPE2, "cid kind TYPE2 moved");

_Static_assert((int)HAL_LINE_TEST_GR909 == (int)UDSP_TEST_GR909,
	       "line test GR909 moved");
_Static_assert((int)HAL_LINE_TEST_ROH == (int)UDSP_TEST_ROH,
	       "line test ROH moved");
_Static_assert((int)HAL_LINE_TEST_CAPACITANCE ==
	       (int)UDSP_TEST_CAPACITANCE, "line test CAPACITANCE moved");
_Static_assert((int)HAL_LINE_TEST_LOOP == (int)UDSP_TEST_LOOP,
	       "line test LOOP moved");

_Static_assert((int)HAL_CID_BELLCORE_FSK == (int)UDSP_CIDF_BELLCORE_FSK,
	       "cid format BELLCORE_FSK moved");
_Static_assert((int)HAL_CID_ETSI_FSK == (int)UDSP_CIDF_ETSI_FSK,
	       "cid format ETSI_FSK moved");
_Static_assert((int)HAL_CID_ETSI_RPAS == (int)UDSP_CIDF_ETSI_RPAS,
	       "cid format ETSI_RPAS moved");
_Static_assert((int)HAL_CID_ETSI_DTAS == (int)UDSP_CIDF_ETSI_DTAS,
	       "cid format ETSI_DTAS moved");
_Static_assert((int)HAL_CID_DTMF == (int)UDSP_CIDF_DTMF, "cid format DTMF moved");
_Static_assert((int)HAL_CID_NTT == (int)UDSP_CIDF_NTT, "cid format NTT moved");
_Static_assert((int)HAL_CID_ETSI_LR_DTAS == (int)UDSP_CIDF_ETSI_LR_DTAS,
	       "cid format ETSI_LR_DTAS moved");
_Static_assert((int)HAL_CID_DTMF_DTAS == (int)UDSP_CIDF_DTMF_DTAS,
	       "cid format DTMF_DTAS moved");

_Static_assert((int)HAL_DETECT_DTMF == (int)UDSP_DET_DTMF, "detect DTMF moved");
_Static_assert((int)HAL_DETECT_PULSE == (int)UDSP_DET_PULSE, "detect PULSE moved");
_Static_assert((int)HAL_DETECT_MODEM == (int)UDSP_DET_MODEM, "detect MODEM moved");
_Static_assert((int)HAL_DETECT_JB_EVENT == (int)UDSP_DET_JB_EVENT,
	       "detect JB_EVENT moved");

/* The two ring records are copied field by field, so their widths have to
 * agree; a cadence array that outgrew the other side would truncate a pattern
 * silently. */
_Static_assert(HAL_CADENCE_MAX == UDSP_CADENCE_MAX, "the cadence array widths differ");
_Static_assert(HAL_REGION_ISO3_MAX == UDSP_ISO3_MAX,
	       "the region name widths differ");
_Static_assert(HAL_REGION_CW_MAX == UDSP_REGION_CW_VIEW_MAX + 1,
	       "the hal cw lead-in array carries the base plus every variant");
_Static_assert(HAL_REGION_RING_MAX == UDSP_REGION_RING_VIEW_MAX,
	       "the region ring template counts differ");
_Static_assert(HAL_REGION_DESCRIBE_MAX == UDSP_REGION_DESCRIBE_MAX,
	       "the region describe widths differ");
_Static_assert((int)HAL_FLASH_SERVICE_OFF == (int)UDSP_FLASH_SERVICE_OFF &&
	       (int)HAL_FLASH_SERVICE_SPLIT == (int)UDSP_FLASH_SERVICE_SPLIT &&
	       (int)HAL_FLASH_SERVICE_DROP == (int)UDSP_FLASH_SERVICE_DROP,
	       "the flash service vocabularies differ");
_Static_assert(HAL_CID_STR_MAX == UDSP_CID_STR_MAX, "the cid string widths differ");
/* The engine diagnostic copy below writes the histogram in one move, so the
 * two records must carry the same number of buckets. */
_Static_assert(sizeof(((struct hal_engine_diag *)0)->interval) ==
	       sizeof(((struct udsp_engine_diag *)0)->interval),
	       "the engine diag interval histograms differ");

/* --- capability ---------------------------------------------------------- */

/*
 * What this backend advertises, and why it is still less than the library
 * reports. A bit here is a promise the call layer acts on, so a bit only
 * travels when a hal op behind it can honour the promise end to end.
 *
 * UDSP_FEAT_G726 and the one G.726 rate hal.h can name travel, because the
 * media ops reach the G.726 streams. UDSP_FEAT_SRTP travels too, and it is
 * the one feature bit with a live consumer: call.c offers SDES only when it
 * is set (call.c: srtp_enabled && HAL_FEAT_SRTP), so dropping it would turn
 * every SRTP configuration into a plain RTP call.
 *
 * Every pair the two contracts share is mapped, and the library decides. A bit
 * is set here when libudsp reports it and not otherwise, so the capability
 * follows the library with no second opinion in this file and no edit here
 * when a coder lands. T.38 and G.729 show both halves of that rule. libudsp
 * carries an in-tree G.729 coder and sets UDSP_FEAT_G729 on every instance, so
 * HAL_FEAT_G729 travels; it sets UDSP_FEAT_T38 only where a fax plug-in
 * registered, so HAL_FEAT_T38 follows the plug-in and this file still names no
 * condition of its own.
 *
 * HAL_FEAT_G729B is the one codec bit with no library pair, and it rides the
 * G.729 one rather than a condition of its own. libudsp reports no Annex B
 * feature: the fact lives on the codec ops, whose sid_bytes the in-tree coder
 * carries, and hal_cap has no field for a codec ops table. That coder is the
 * only G.729 libudsp holds, so the two bits are one capability here and stay
 * one until the library reports them apart.
 *
 * HAL_FEAT_IPV6_MEDIA and HAL_FEAT_RTCP_ADDR are the two bits this file
 * decides on its own, and the socket section below is where they are made
 * true. See the note beside the assignment.
 *
 * HAL_FEAT_CONF travels like every other pair: libudsp sets UDSP_FEAT_CONF
 * and implements the mix-minus, and conf_set forwards, so the core can offer
 * three-way.
 *
 * HAL_FEAT_KERNEL_RTP has no library source and is not a capability: it is a
 * statement of who makes the packets (hal.h). libudsp makes and reads them in
 * user space on the sockets this backend owns, so the bit is clear and the
 * media works.
 *
 * The three G.726 rates beside 32 kbit/s are dropped for want of a name:
 * hal.h defines one G.726 codec value and it is the 32 kbit/s one, so the
 * other three rates cannot be expressed in a hal_media at all.
 *
 * HAL_FEAT_POLARITY and HAL_FEAT_MWI_LAMP travel unchanged, and the
 * lamp bit is the plug-in's own answer rather than a constant: libudsp sets
 * UDSP_FEAT_MWI_LAMP only when the SLIC plug-in exposes the lamp op (udsp.h
 * struct udsp_cap).
 *
 * The real-time outcome travels field for field; the mlock and affinity
 * errnos stay in the library, whose open record names them.
 */
static uint8_t hal_rt_policy_word(uint8_t policy)
{
	switch (policy) {
	case UDSP_RT_POLICY_FIFO:
		return HAL_RT_POLICY_FIFO;
	case UDSP_RT_POLICY_RR:
		return HAL_RT_POLICY_RR;
	case UDSP_RT_POLICY_OTHER:
		return HAL_RT_POLICY_OTHER;
	case UDSP_RT_POLICY_UNKNOWN:
		return HAL_RT_POLICY_UNKNOWN;
	default:
		return HAL_RT_POLICY_NONE;
	}
}

void hal_udsp_cap_fill(struct hal_cap *dst, const struct udsp_cap *src)
{
	memset(dst, 0, sizeof(*dst));
	snprintf(dst->backend, sizeof(dst->backend), "udsp");
	/* libudsp's receive ring accepts 576-byte datagrams, so the endpoint
	 * advertises a 512-byte T.38 receive limit */
	dst->t38_max_datagram = 512;

	if (!src)
		return;

	snprintf(dst->version, sizeof(dst->version), "%s", src->version);
	snprintf(dst->slic, sizeof(dst->slic), "%s", src->slic);
	dst->lines = src->lines > HAL_LINE_MAX ? HAL_LINE_MAX : src->lines;
	dst->streams_per_chan = src->streams_per_chan > HAL_STREAM_MAX ?
				HAL_STREAM_MAX : src->streams_per_chan;

	if (src->features & UDSP_FEAT_POLARITY)
		dst->features |= HAL_FEAT_POLARITY;
	if (src->features & UDSP_FEAT_MWI_LAMP)
		dst->features |= HAL_FEAT_MWI_LAMP;
	if (src->features & UDSP_FEAT_G726)
		dst->features |= HAL_FEAT_G726;
	if (src->features & UDSP_FEAT_G729)
		dst->features |= HAL_FEAT_G729 | HAL_FEAT_G729B;
	if (src->features & UDSP_FEAT_T38)
		dst->features |= HAL_FEAT_T38;
	if (src->features & UDSP_FEAT_LINE_TEST)
		dst->features |= HAL_FEAT_LINE_TEST;
	if (src->features & UDSP_FEAT_SRTP)
		dst->features |= HAL_FEAT_SRTP;
	if (src->features & UDSP_FEAT_CONF)
		dst->features |= HAL_FEAT_CONF;
	if (src->features & UDSP_FEAT_WIDEBAND)
		dst->features |= HAL_FEAT_WIDEBAND;
	if (src->features & UDSP_FEAT_DTMF_RELAY)
		dst->features |= HAL_FEAT_DTMF_RELAY;
	if (src->features & UDSP_FEAT_DTMF_PLAYOUT)
		dst->features |= HAL_FEAT_DTMF_PLAYOUT;
	if (src->features & UDSP_FEAT_LOOP)
		dst->features |= HAL_FEAT_LOOP;

	/*
	 * Neither of these is a library bit. libudsp takes connected
	 * descriptors and owns no address at all (udsp.h section 3), so the
	 * family a stream can carry, and the address its RTCP goes to, are
	 * decided by the socket code of this file.
	 */
	dst->features |= HAL_FEAT_IPV6_MEDIA | HAL_FEAT_RTCP_ADDR;
	/* Not a library bit either: line/hook.c takes its windows from the
	 * region file at every reset and swap, so the core's push has nothing
	 * to add and would latch a stale window against the next swap. */
	dst->features |= HAL_FEAT_REGION_HOOK;

	if (src->codecs & (1u << UDSP_SEL_PCMU))
		dst->codecs |= 1u << HAL_CODEC_PCMU;
	if (src->codecs & (1u << UDSP_SEL_PCMA))
		dst->codecs |= 1u << HAL_CODEC_PCMA;
	if (src->codecs & (1u << UDSP_SEL_G726_32))
		dst->codecs |= 1u << HAL_CODEC_G726_32;
	if (src->codecs & (1u << UDSP_SEL_G729))
		dst->codecs |= 1u << HAL_CODEC_G729;
	if (src->codecs & (1u << UDSP_SEL_T38))
		dst->codecs |= 1u << HAL_CODEC_T38;
	if (src->codecs & (1u << UDSP_SEL_G722))
		dst->codecs |= 1u << HAL_CODEC_G722;
	if (src->codecs & (1u << UDSP_SEL_OPUS))
		dst->codecs |= 1u << HAL_CODEC_OPUS;

	dst->rt_policy = hal_rt_policy_word(src->rt_policy);
	dst->rt_net_policy = hal_rt_policy_word(src->rt_net_policy);
	dst->rt_mlock = src->rt_mlock;
	dst->rt_cpu = src->rt_cpu;
	dst->rt_prio = src->rt_prio;
	dst->rt_sched_errno = src->rt_sched_errno;
}

static const struct hal_cap *udsp_cap_get(struct hal *h)
{
	return &h->cap;
}

/*
 * tdm_stalls is the library's own number and travels unchanged. It is the
 * audio ring diagnostic udsp carries beyond the transport three, and
 * hal_counters has a field for it. It is up to one second
 * stale by design: the library watchdog republishes the engine's count once a
 * second and this read takes no lock (udsp.h struct udsp_counters), so a
 * caller reads the last publication and never a value it had to wait for.
 *
 * The two loss classes of hal.h section 2 are counted apart, and each loss
 * lands in exactly one counter. A hardware event either hop refused is class
 * (a) and joins events_dropped; a completion this backend's queue could not
 * hold is class (b) and joins completions_lost. The library cannot lose a
 * completion of its own, so comp_lost is the whole class (b) number.
 *
 * The four catch-up counters are the library's own numbers as well and travel
 * unchanged, on the staleness rule tdm_stalls carries: the same watchdog
 * publishes them.
 */
static void udsp_counters_get(struct hal *h, struct hal_counters *out)
{
	struct udsp_counters c;

	memset(&c, 0, sizeof(c));
	udsp_counters(h->s, &c);

	out->events_sent = c.events_sent;
	/* Both hops count: the library's emitter drops on a full relay fd and
	 * the relay drops on a full core fd, and either one is a lost class (a)
	 * record the core has to see in one number. */
	out->events_dropped = c.events_dropped + h->relay.dropped;
	out->op_errors = c.op_errors;
	out->tdm_stalls = c.tdm_stalls;
	out->completions_lost = h->relay.comp_lost;
	out->holds = c.holds;
	out->capture_drops = c.capture_drops;
	out->trim_drops = c.trim_drops;
	out->playback_expired = c.playback_expired;
	out->hook_gaps = c.hook_gaps;
	out->hook_moved_gaps = c.hook_moved_gaps;
	out->hook_gap_max_ms = c.hook_gap_max_ms;
}

/* --- the library's log sink ---------------------------------------------- */

int hal_udsp_log_prio(enum udsp_log_level level)
{
	switch (level) {
	case UDSP_LOG_ERR:
		return LOG_ERR;
	case UDSP_LOG_WARN:
		return LOG_WARNING;
	case UDSP_LOG_INFO:
		return LOG_INFO;
	case UDSP_LOG_DEBUG:
		break;
	}

	return LOG_DEBUG;
}

/*
 * The sink of udsp.h udsp_config.log, called on the control thread and on the
 * library's slic and emitter threads. It takes the shape of trace_emit() by
 * hand, because the domain masks belong to the uloop thread: the two atomic
 * mirrors of trace.h answer the same two questions from any thread.
 *
 * A DEBUG line has two destinations and the mirrors choose them. The operator
 * masks send it to syslog. The hal bit of the ring mask sends it
 * to the relay, which carries it onto the uloop thread and into the log ring.
 * The library gates the line on the debug bit that either mask sets through
 * debug_set, so a line that arrives while only the ring asked for it must not
 * reach the log.
 *
 * A WARN or an ERR line always goes to syslog, and to the relay whenever the
 * rings are open, whatever the mask says. The engine hold record and the slic
 * fault are what an archive is read for, and they must not depend on an
 * operator having named the hal domain.
 *
 * The relay takes the line under the same "udsp: " marker syslog gives it, so
 * one line reads the same in the log and in the archive.
 *
 * main() logs before the library starts those threads and closes the log
 * after udsp_close() joined them; a slic thread the bounded stop abandoned
 * can still reach ulog while the process exits for procd, the one moment this
 * sink is not covered.
 */
void hal_udsp_log(void *ctx, enum udsp_log_level level, const char *msg)
{
	char line[TRACE_RELAY_LINE_MAX];
	uint32_t bit = 1u << TRACE_HAL;
	int prio = hal_udsp_log_prio(level);
	bool debug = level == UDSP_LOG_DEBUG;
	bool relay = debug ? (trace_ring_mask_atomic() & bit) != 0u
			   : trace_ring_open_atomic();

	(void)ctx;

	if (relay) {
		snprintf(line, sizeof(line), "udsp: %s", msg);
		trace_relay_put(prio, line);
	}

	if (!debug) {
		ulog(prio, "udsp: %s\n", msg);
		return;
	}

	if (trace_mask_atomic() & bit)
		ulog(LOG_DEBUG, "[hal] udsp: %s\n", msg);
}

/* --- configuration ------------------------------------------------------- */

/*
 * hal_ops.open takes only the event fd, so the backend reads the loaded
 * configuration itself (udsp.h struct udsp_config). It loads a
 * private copy rather than reading the daemon's: uv.cfg belongs to main.c and
 * does not exist in a host suite that links this backend alone, and the copy
 * is read once and freed inside open().
 *
 * Every field comes from the voip.hal section and the region is not
 * one of them. udsp_config.region is left empty on purpose: udsp_open() fails
 * when a named region does not parse (core udsp.c: "a parse failure fails the
 * open"), and libudsp ships seven region files against the thirty three names
 * the uvoice tone loader can derive, so a config naming any of the other
 * twenty six would take the whole HAL down instead of one tone table. The
 * region belongs to region_apply alone, which line/line.c submits at
 * lines_start: a load that fails there is reported and the call path keeps
 * running (line/line.c region_done: "tones stay unconfigured"). The window
 * between open and that submission carries no tone request, because no line is
 * in service yet.
 */
void hal_udsp_config_fill(struct udsp_config *dst, const struct cfg *cfg)
{
	const struct cfg_hal *hal = &cfg->hal;

	memset(dst, 0, sizeof(*dst));
	str_copy(dst->tdm_backend, sizeof(dst->tdm_backend), hal->tdm_backend);
	str_copy(dst->tdm_playback, sizeof(dst->tdm_playback),
		 hal->tdm_playback);
	str_copy(dst->tdm_capture, sizeof(dst->tdm_capture), hal->tdm_capture);
	str_copy(dst->slic_plugin, sizeof(dst->slic_plugin), hal->slic_plugin);
	str_copy(dst->fax_plugin, sizeof(dst->fax_plugin), hal->fax_plugin);
	str_copy(dst->spidev, sizeof(dst->spidev), hal->spidev);
	str_copy(dst->gpio_reset, sizeof(dst->gpio_reset), hal->gpio_reset);
	str_copy(dst->gpio_interrupt, sizeof(dst->gpio_interrupt),
		 hal->gpio_interrupt);

	dst->populated_ports = (uint8_t)hal->populated_ports;
	dst->channels = (uint8_t)hal->channels;
	dst->prefill_periods = (uint8_t)hal->prefill_periods;
	dst->pcm_offset_bits = (uint8_t)hal->pcm_offset_bits;
	dst->engine_cpu = hal->engine_cpu;
	dst->engine_prio = hal->engine_prio;
	dst->sample_rate = (uint16_t)hal->sample_rate;
	/* The battery follows the service state (line/line.c battery_sync):
	 * every line comes up open and the core raises the ones in service. */
	dst->lines_start_down = true;
}

/* --- line count file ------------------------------------------------------ */

/*
 * Best effort by design: a read-only or absent /var/run costs a diagnostic
 * file, never a voice path, so a failure is traced and the attach continues.
 */
void hal_udsp_fxsnum_write(unsigned int lines)
{
	FILE *f = fopen(hal_udsp_fxsnum_path, "w");

	if (!f) {
		TRACE(TRACE_HAL, "udsp: %s not written: %s", hal_udsp_fxsnum_path,
		      strerror(errno));
		return;
	}

	fprintf(f, "%u\n", lines);
	fclose(f);
}

/* --- the event relay ------------------------------------------------------
 *
 * The transport, the completion queue and the drop counters live in the shared
 * hal_relay.c. This backend keeps only the two bring-up translations below:
 * the stream-dead fault it posts as a completion, and the chain that turns the
 * engine's start and region loads into the one completion the core waits for.
 */

static void stream_dead_report(struct hal *h, unsigned int line,
			       unsigned int stream, const char *op)
{
	struct hal_event ev = {
		.type = HAL_EVENT_SLIC_FAULT,
		.line = (uint16_t)line,
		.stream = (uint16_t)stream,
		.stamp_ms = uvoice_mono_ms(),
	};
	bool offhook;

	/* A dead-marked stream keeps its duplicate descriptors until udsp_close
	 * (libudsp stream.h), so its local port stays bound for the life of the
	 * handle. udsp_line_hook_query() reads s->fatal and s->chan[line].dead
	 * and never the stream's dead mark, so a nonfatal answer isolates the
	 * mark to the stream. The daemon already leaves for the procd respawn
	 * when the engine stalls; this accepts the same cost. Every other call
	 * on the box drops, and the respawn releases the descriptors. */
	if (udsp_line_hook_query(h->s, line, &offhook) == UDSP_FATAL)
		return;

	ULOG_ERR("udsp: line %u stream %u: the library marked the stream "
		 "dead on %s\n", line, stream, op);
	ev.u.fault.code = HAL_FAULT_STREAM_DEAD;
	hal_relay_completion_send(&h->relay, &ev);
}

/*
 * The second half of the bring-up. The start succeeded, so the tone plan is
 * the only thing still missing before a line can carry a call.
 */
static void region_embrace(struct hal *h)
{
	struct udsp_job j = { 0 };
	enum udsp_status rc;

	if (!h->region[0]) {
		ULOG_WARN("udsp: no region configured, tones wait for the first "
			  "region_apply\n");
		hal_relay_completion_post(&h->relay, h->start_token, HAL_OK);
		return;
	}

	rc = udsp_region_load(h->s, h->region, &j);
	if (rc != UDSP_OK) {
		/* Soft (line/line.c region_done): a missing tone table is a
		 * silent phone, a failed bring-up is no phone at all. */
		ULOG_WARN("udsp: initial region %s refused: %s, tones wait for "
			  "region_apply\n", h->region, udsp_status_name(rc));
		hal_relay_completion_post(&h->relay, h->start_token, HAL_OK);
		return;
	}

	h->region_token = j.token;
	h->region_armed = true;
}

/*
 * Returns true when the record belonged to the chain and must not reach the
 * core: the core knows one token for the whole bring-up and gets exactly one
 * completion for it.
 */
static bool chain_take(void *ctx, const struct hal_event *ev)
{
	struct hal *h = ctx;
	enum hal_status st;

	if (ev->type != HAL_EVENT_JOB_DONE)
		return false;

	st = (enum hal_status)ev->u.job.status;

	if (h->start_armed && ev->u.job.token == h->start_token) {
		h->start_armed = false;
		if (st != HAL_OK) {
			hal_relay_completion_post(&h->relay, h->start_token, st);
			return true;
		}

		region_embrace(h);
		return true;
	}

	if (h->region_armed && ev->u.job.token == h->region_token) {
		h->region_armed = false;
		if (st != HAL_OK)
			ULOG_WARN("udsp: initial region %s not loaded: %s, tones "
				  "wait for region_apply\n", h->region,
				  hal_status_name(st));

		hal_relay_completion_post(&h->relay, h->start_token, HAL_OK);
		return true;
	}

	return false;
}

/* --- lifecycle ----------------------------------------------------------- */

/* --- the jitter buffer decision trace -------------------- */

/*
 * One consumer, and it is this file. udsp.h makes udsp_jb_trace_read a
 * single-consumer call, so a second reader anywhere in the process would take
 * records from this one. Nothing else in uvoice calls it.
 *
 * The names belong to rtp/jb.h and are mirrored here because that header keeps
 * enum jb_state private to its own translation unit and udsp.h says the number
 * is a diagnostic, not a contract. The suite pins both tables against the
 * library, which is the only thing that can catch a table that slipped.
 */
static const char * const jb_state_names[] = { "idle", "prefill", "play" };

const char *hal_udsp_jb_state_name(uint8_t state)
{
	if (state >= ARRAY_SIZE(jb_state_names))
		return "?";

	return jb_state_names[state];
}

const char *hal_udsp_jb_decision_name(uint8_t decision)
{
	static const char * const names[] = { "frame", "plc", "cn", "silence" };

	if (decision >= ARRAY_SIZE(names))
		return "?";

	return names[decision];
}

/* The errno set udsp_jb_put answers with (udsp.h struct udsp_jb_trace). A get
 * record carries a decision here instead and never reaches this table. */
const char *hal_udsp_jb_rc_name(int8_t rc)
{
	switch (rc) {
	case 0:
		return "ok";
	case -ETIME:
		return "late";
	case -EEXIST:
		return "dup";
	case -ENOBUFS:
		return "overflow";
	case -EMSGSIZE:
		return "oversize";
	case -EINVAL:
		return "invalid";
	}

	return "?";
}

static const char *jb_skew_name(uint8_t skew)
{
	static const char * const names[] = { "none", "drop", "insert" };

	if (skew >= ARRAY_SIZE(names))
		return "?";

	return names[skew];
}

static const struct {
	uint16_t bit;
	const char *word;
} jb_flag_words[] = {
	{ UDSP_JB_TRACE_ANCHOR, "anchor" },
	{ UDSP_JB_TRACE_REFILL, "refill" },
	{ UDSP_JB_TRACE_STARVE, "starve" },
	{ UDSP_JB_TRACE_RESYNC, "resync" },
	{ UDSP_JB_TRACE_CN_REANCHOR, "cn_reanchor" },
	{ UDSP_JB_TRACE_SKEW_DROP, "skew_drop" },
	{ UDSP_JB_TRACE_SKEW_INSERT, "skew_insert" },
	{ UDSP_JB_TRACE_PARKED, "parked" },
	{ UDSP_JB_TRACE_UNDERRUN, "underrun" },
	{ UDSP_JB_TRACE_HEARTBEAT, "heartbeat" },
};

/* Each word carries its own leading space, so a record with none of the bits
 * the caller asked for renders as nothing at all and the line closes up. */
static void jb_words(char *buf, size_t len, uint16_t flags, uint16_t want)
{
	size_t off = 0;
	unsigned int i;

	buf[0] = '\0';
	for (i = 0; i < ARRAY_SIZE(jb_flag_words); i++) {
		int n;

		if (!(flags & want & jb_flag_words[i].bit))
			continue;
		n = snprintf(buf + off, len - off, " %s", jb_flag_words[i].word);
		if (n < 0 || (size_t)n >= len - off)
			return;
		off += (size_t)n;
	}
}

static void jb_trace_put(const struct udsp_jb_trace *r)
{
	char words[HAL_UDSP_TRACE_WORDS];

	jb_words(words, sizeof(words), r->flags,
		 UDSP_JB_TRACE_ANCHOR | UDSP_JB_TRACE_REFILL |
		 UDSP_JB_TRACE_STARVE | UDSP_JB_TRACE_RESYNC |
		 UDSP_JB_TRACE_CN_REANCHOR | UDSP_JB_TRACE_UNDERRUN);
	TRACE(TRACE_JB,
	      "l%u s%u +%u put %s seq=%u ts=%u play=%u late=%dms depth=%u "
	      "tgt=%u run=%u st=%s>%s n=%u skew=%s%s lost=%u",
	      r->line, r->stream, r->stamp_ms, hal_udsp_jb_rc_name(r->rc),
	      r->seq, r->pkt_ts, r->playout_ts,
	      (int)(r->lateness_ts / HAL_UDSP_TS_PER_MS), r->depth_ms,
	      r->target_ms, r->conceal_run_ms,
	      hal_udsp_jb_state_name(r->state_before),
	      hal_udsp_jb_state_name(r->state_after), r->count,
	      jb_skew_name(r->skew_pending), words, r->lost_before);
}

/*
 * The two flag groups fill the two word slots of the line: what the output
 * unit itself carried, then what the buffer counted while it made it. A
 * reader scanning for a parked clock and a reader scanning for underruns then
 * look in the same two places on every line.
 */
static void jb_trace_get(const struct udsp_jb_trace *r)
{
	char marks[HAL_UDSP_TRACE_WORDS];
	char events[HAL_UDSP_TRACE_WORDS];

	jb_words(marks, sizeof(marks), r->flags,
		 UDSP_JB_TRACE_SKEW_DROP | UDSP_JB_TRACE_SKEW_INSERT |
		 UDSP_JB_TRACE_PARKED);
	jb_words(events, sizeof(events), r->flags,
		 UDSP_JB_TRACE_UNDERRUN | UDSP_JB_TRACE_HEARTBEAT);
	TRACE(TRACE_JB,
	      "l%u s%u +%u get %s%s%s ts=%u play=%u depth=%u tgt=%u run=%u "
	      "st=%s>%s n=%u skew=%s lost=%u",
	      r->line, r->stream, r->stamp_ms,
	      hal_udsp_jb_decision_name((uint8_t)r->rc), marks, events,
	      r->pkt_ts, r->playout_ts, r->depth_ms, r->target_ms,
	      r->conceal_run_ms, hal_udsp_jb_state_name(r->state_before),
	      hal_udsp_jb_state_name(r->state_after), r->count,
	      jb_skew_name(r->skew_pending), r->lost_before);
}

static void jb_trace_record(const struct udsp_jb_trace *r)
{
	if (r->kind == UDSP_JB_TRACE_PUT) {
		jb_trace_put(r);
		return;
	}

	jb_trace_get(r);
}

/*
 * A short read ends the drain, and the round bound ends a flood. The library
 * ring holds 511 records, so a producer that outruns both counts its own
 * losses and the next record that fits carries the count: this loop can fall
 * behind, and it can never hold the uloop the core's own reader runs on.
 */
static void jb_trace_drain(struct hal *h)
{
	struct udsp_jb_trace rec[HAL_UDSP_TRACE_BATCH];
	unsigned int round;

	for (round = 0; round < HAL_UDSP_TRACE_ROUNDS; round++) {
		unsigned int n = udsp_jb_trace_read(h->s, rec,
						    HAL_UDSP_TRACE_BATCH);
		unsigned int i;

		for (i = 0; i < n; i++)
			jb_trace_record(&rec[i]);

		if (n < HAL_UDSP_TRACE_BATCH)
			return;
	}
}

static void jb_trace_expire(struct uloop_timeout *t)
{
	struct hal *h = container_of(t, struct hal, jb_trace);

	jb_trace_drain(h);
	uloop_timeout_set(&h->jb_trace, HAL_UDSP_TRACE_MS);
}

/* Empties the ring and writes nothing. It is the arming half below; the
 * records it drops belong to a session that is over. */
static void jb_trace_discard(struct hal *h)
{
	struct udsp_jb_trace rec[HAL_UDSP_TRACE_BATCH];
	unsigned int round;

	for (round = 0; round < HAL_UDSP_TRACE_ROUNDS; round++) {
		if (udsp_jb_trace_read(h->s, rec, HAL_UDSP_TRACE_BATCH) <
		    HAL_UDSP_TRACE_BATCH)
			return;
	}
}

/*
 * The disarm drains once more. The records the engine wrote between the last
 * firing and the clear are already in the ring, and nothing else empties it,
 * so they would sit there until the next arm and then arrive stamped with a
 * time long past.
 *
 * The arm then discards what is still there, and that is not the same thing.
 * The engine reads the debug word once per tick on its own thread, so a tick
 * already inside its receive path can push one more record after the disarm
 * drain returned; the cancelled timer strands it. Without this the next arm
 * would open its session with a line from the previous one, carrying a stamp
 * from before the trace was ever turned off. Dropping those records loses
 * nothing: nobody asked for them.
 */
static void jb_trace_arm(struct hal *h, bool on)
{
	if (!on) {
		uloop_timeout_cancel(&h->jb_trace);
		jb_trace_drain(h);
		return;
	}

	if (h->jb_trace.pending)
		return;

	jb_trace_discard(h);
	uloop_timeout_set(&h->jb_trace, HAL_UDSP_TRACE_MS);
}

/* --- the plaintext packet trace ------------------------------------------- */

/*
 * Every media datagram of every stream in the clear, drained from the
 * library's two rings onto the call trace ring. A transmit record is taken
 * before the library protects the packet and a receive record after it opens
 * one, so an encrypted call reads here and nowhere else.
 *
 * One consumer, and it is this file, the rule the decision trace above follows
 * for its own ring. The drain runs on a timer because the rings carry no
 * readable descriptor, and on a timer of its own because the two debug bits
 * arm independently.
 *
 * A record names the description it travelled under and carries no address, so
 * this file keeps the endpoints of the last few descriptions of every stream.
 * A record drained after the stop that closed the sockets still renders the
 * far end it went to.
 */

/* One batch of records, and the reason it is not a local: one record is 600
 * bytes, so the array would be the largest stack frame of the uloop thread by
 * an order of magnitude. One drain runs at a time, on that thread alone. */
static struct udsp_pkt_trace pkt_batch[HAL_UDSP_PKT_BATCH];

static struct hal_udsp_gen_table *pkt_gen_table(struct hal *h,
						unsigned int line,
						unsigned int stream)
{
	if (line >= HAL_LINE_MAX || stream >= HAL_STREAM_MAX)
		return NULL;

	return &h->pkt_gen[line][stream];
}

/*
 * The description one successful configuration applied. The generation is the
 * library's own answer for the live description, so an entry and a record name
 * the same thing, and the addresses are the ones this backend bound and
 * connected under it.
 */
static void pkt_gen_add(struct hal *h, unsigned int line, unsigned int stream,
			const struct hal_udsp_stream *s)
{
	struct hal_udsp_gen_table *t = pkt_gen_table(h, line, stream);
	struct hal_udsp_gen *e;

	if (!t)
		return;

	e = &t->entry[t->next];
	e->gen = udsp_stream_gen(h->s, line, stream);
	e->local = s->sock.local;
	e->remote = s->sock.remote;
	e->rtcp_remote = s->sock.rtcp_remote;
	t->next = (uint8_t)((t->next + 1u) % HAL_UDSP_PKT_GEN_MAX);
	if (t->count < HAL_UDSP_PKT_GEN_MAX)
		t->count++;
}

/*
 * The description a record travelled under. A generation the table carries
 * answers itself. One it does not answers the newest description of that
 * stream and is counted, because a record older than the table can hold still
 * belongs on the ring with the endpoints this backend last knew. A stream with
 * no description at all answers nothing, and the record is dropped and counted
 * the same way.
 */
static const struct hal_udsp_gen *pkt_gen_find(struct hal *h,
					       const struct udsp_pkt_trace *rec)
{
	struct hal_udsp_gen_table *t = pkt_gen_table(h, rec->line, rec->stream);
	unsigned int newest;
	unsigned int i;

	if (!t || !t->count) {
		h->pkt_gen_miss++;
		return NULL;
	}

	for (i = 0; i < t->count; i++) {
		if (t->entry[i].gen == rec->gen)
			return &t->entry[i];
	}

	h->pkt_gen_miss++;
	newest = (t->next + HAL_UDSP_PKT_GEN_MAX - 1u) % HAL_UDSP_PKT_GEN_MAX;

	return &t->entry[newest];
}

static void endpoint_of(struct trace_endpoint *out, const struct hal_addr *a,
			uint16_t port)
{
	memset(out, 0, sizeof(*out));
	out->ipver = a->ipver;
	out->port = port;
	memcpy(out->addr, a->addr, sizeof(out->addr));
}

/* The two vocabularies name the same three payloads at different numbers, so
 * the map is a switch and not a cast. A kind this backend cannot name has no
 * endpoints to render and never reaches the ring. */
static int pkt_kind_of(uint8_t kind)
{
	switch (kind) {
	case UDSP_PKT_RTP:
		return TRACE_PKT_RTP;
	case UDSP_PKT_RTCP:
		return TRACE_PKT_RTCP;
	case UDSP_PKT_UDPTL:
		return TRACE_PKT_UDPTL;
	}

	return -1;
}

void pkt_trace_record(struct hal *h, const struct udsp_pkt_trace *rec,
		      int64_t offset_ns)
{
	const struct trace_endpoint *src, *dst;
	struct trace_endpoint local, remote;
	const struct hal_udsp_gen *g;
	enum trace_pkt_dir dir;
	uint64_t stamp_us;
	bool rtcp;
	int kind;

	kind = pkt_kind_of(rec->kind);
	if (kind < 0)
		return;

	g = pkt_gen_find(h, rec);
	if (!g)
		return;

	/* The far end can name an RTCP address of its own (RFC 3605 section
	 * 2.1), so an RTCP record takes both its ports and its far address
	 * from the description's own rtcp record. */
	rtcp = kind == TRACE_PKT_RTCP;
	endpoint_of(&local, &g->local,
		    rtcp ? rtcp_port_of(&g->local) : g->local.port);
	endpoint_of(&remote, rtcp ? &g->rtcp_remote : &g->remote,
		    rtcp ? rtcp_port_of(&g->rtcp_remote) : g->remote.port);

	dir = rec->dir == UDSP_PKT_RX ? TRACE_PKT_RX : TRACE_PKT_TX;
	src = dir == TRACE_PKT_TX ? &local : &remote;
	dst = dir == TRACE_PKT_TX ? &remote : &local;
	/* The sum runs unsigned, so a stamp in the upper half of the range
	 * cannot overflow a signed add. A negative offset wraps, and the wrap
	 * answers the right number whenever the true sum fits, which it does
	 * for every instant a monotonic clock and a wall clock can name. */
	stamp_us = (rec->stamp_ns + (uint64_t)offset_ns) / 1000u;

	if (trace_ring_packet(TRACE_RING_RTP, dir, (enum trace_pkt_kind)kind,
			      src, dst, stamp_us, rec->data, rec->len))
		h->pkt_ring_fail++;
}

/*
 * The wall clock of the drain, read once. A record carries a monotonic stamp
 * and the ring carries microseconds since the epoch, so the difference between
 * the two clocks converts every record of the batch. Reading a clock per
 * record would cost one call per packet and would still not be more truthful:
 * the instants the records name are the producers' own.
 */
static int64_t pkt_clock_offset(void)
{
	struct timespec real, mono;

	if (clock_gettime(CLOCK_REALTIME, &real) ||
	    clock_gettime(CLOCK_MONOTONIC, &mono))
		return 0;

	return ((int64_t)real.tv_sec - (int64_t)mono.tv_sec) * 1000000000 +
	       ((int64_t)real.tv_nsec - (int64_t)mono.tv_nsec);
}

/*
 * A short read ends the drain and the round bound ends a flood, the rule the
 * decision trace drain follows. The loop may not stop at a full batch: one
 * call pops the transmit ring first and the receive ring after it (udsp.h
 * udsp_pkt_trace_read), so a batch the transmit backlog fills on its own would
 * starve the receive direction for the whole period.
 */
static void pkt_trace_drain(struct hal *h)
{
	int64_t offset_ns = pkt_clock_offset();
	unsigned int round;

	for (round = 0; round < HAL_UDSP_PKT_ROUNDS; round++) {
		unsigned int n = udsp_pkt_trace_read(h->s, pkt_batch,
						     HAL_UDSP_PKT_BATCH);
		unsigned int i;

		for (i = 0; i < n; i++)
			pkt_trace_record(h, &pkt_batch[i], offset_ns);

		if (n < HAL_UDSP_PKT_BATCH)
			return;
	}
}

static void pkt_trace_expire(struct uloop_timeout *t)
{
	struct hal *h = container_of(t, struct hal, pkt_trace);

	pkt_trace_drain(h);
	uloop_timeout_set(&h->pkt_trace, HAL_UDSP_TRACE_MS);
}

/*
 * Empties both rings and writes nothing, the arming half of the pair below.
 * udsp_pkt_trace_read answers what the rings hold and never reads the debug
 * bit, so this reaches the records of a session that is over.
 *
 * Its bound is the capacity of both rings and not the drain's own round bound:
 * a disarm keeps the memory, so the backlog a stranded session left can be the
 * whole ring, and a short bound would carry it into the next trace.
 */
static void pkt_trace_discard(struct hal *h)
{
	unsigned int round;

	for (round = 0; round < HAL_UDSP_PKT_DISCARD_ROUNDS; round++) {
		if (udsp_pkt_trace_read(h->s, pkt_batch, HAL_UDSP_PKT_BATCH) <
		    HAL_UDSP_PKT_BATCH)
			return;
	}
}

/* The two halves the decision trace explains: the disarm drains once more,
 * because nothing else empties the rings and those records belong to the trace
 * that just ended, and the arm discards what a producer left after an earlier
 * disarm, because a stamp from before the trace was off belongs to no
 * session. */
static void pkt_trace_arm(struct hal *h, bool on)
{
	if (!on) {
		uloop_timeout_cancel(&h->pkt_trace);
		pkt_trace_drain(h);
		return;
	}

	if (h->pkt_trace.pending)
		return;

	pkt_trace_discard(h);
	uloop_timeout_set(&h->pkt_trace, HAL_UDSP_TRACE_MS);
}

/*
 * The arm half. The library maps 1.2 MiB behind the trace and the call can
 * fail, so the entry is its own and not a flag of udsp_debug_set, and the
 * timer starts only where the memory stands. It answers whether the trace
 * runs, which is what decides the library mask below.
 *
 * A refusal takes the bit out of the stored mask, the rule media_tos_apply
 * follows for a mark: a record that kept a bit the backend could not honour
 * would make the next push read as no edge, and the allocation would never be
 * tried again. The warning is the record of the refusal.
 */
static bool pkt_trace_start(struct hal *h)
{
	if (udsp_pkt_trace_enable(h->s, true)) {
		h->pkt_arm_fail++;
		h->debug &= ~HAL_DEBUG_PKT_TRACE;
		hal_udsp_log(h, UDSP_LOG_WARN,
			     "the packet trace found no memory and stays off");
		return false;
	}

	pkt_trace_arm(h, true);

	return true;
}

/* The disarm half, in the order the records need: the caller lowers the
 * library mask first, this lowers the flag the library keeps under its own
 * lock, and the drain then empties what the producers left behind. */
static void pkt_trace_stop(struct hal *h)
{
	udsp_pkt_trace_enable(h->s, false);
	pkt_trace_arm(h, false);
}

/* --- the canceller capture ------------------------------------------------ */

/*
 * Every frame of every line canceller, drained from the library's ring onto
 * the canceller ring of the call trace, where a host program replays it
 * through the same canceller code (udsp.h struct udsp_lec_capture). One
 * consumer, this file, on a timer of its own for the reason the packet trace
 * gives. The start and the stop take other orders than the packet trace, and
 * each says why.
 */

/* One batch of records, static for the reason pkt_batch gives: one record is
 * 1024 bytes. One drain runs at a time, on the uloop thread alone. */
static struct udsp_lec_capture lec_batch[HAL_UDSP_LEC_BATCH];

/* The two vocabularies are separate types, so the map is a switch, the rule
 * pkt_kind_of follows. A kind this backend cannot name never reaches the
 * ring. */
static int lec_kind_of(uint8_t kind)
{
	switch (kind) {
	case UDSP_LEC_CAPTURE_RUN:
		return TRACE_LEC_RUN;
	case UDSP_LEC_CAPTURE_FEED:
		return TRACE_LEC_FEED;
	case UDSP_LEC_CAPTURE_END:
		return TRACE_LEC_END;
	}

	return -1;
}

void lec_capture_record(struct hal *h, const struct udsp_lec_capture *rec,
			int64_t offset_ns)
{
	struct trace_lec_hdr hdr;
	uint64_t stamp_us;
	int kind;

	kind = lec_kind_of(rec->kind);
	if (kind < 0)
		return;

	memset(&hdr, 0, sizeof(hdr));
	hdr.stamp_ns = rec->stamp_ns;
	hdr.tick = rec->tick;
	hdr.lost_before = rec->lost_before;
	hdr.frames = rec->frames;
	hdr.hold_frames = rec->hold_frames;
	hdr.erle_mdb = rec->erle_mdb;
	hdr.echo_gain_micro = rec->echo_gain_micro;
	hdr.coef_energy_micro = rec->coef_energy_micro;
	hdr.ref_delay_samples = rec->ref_delay_samples;
	hdr.gen = rec->gen;
	hdr.flags = rec->flags;
	hdr.samples = rec->samples;
	hdr.tail_ms = rec->tail_ms;
	hdr.noise_ddbov = rec->noise_ddbov;
	hdr.probes = rec->probes;
	hdr.diverge_resets = rec->diverge_resets;
	hdr.line = rec->line;
	hdr.kind = (uint8_t)kind;
	hdr.nlp_verdict = rec->nlp_verdict;
	hdr.hold_left = rec->hold_left;
	hdr.skipped = rec->skipped;

	/* The wrap of pkt_trace_record, for its reason. */
	stamp_us = (rec->stamp_ns + (uint64_t)offset_ns) / 1000u;

	if (trace_ring_lec(&hdr, stamp_us, rec->ref, rec->mic, rec->out))
		h->lec_ring_fail++;
}

/* A short read ends the drain, and rounds bounds it: the round bound of one
 * period on the timer, the whole ring after a disarm. */
static void lec_capture_drain(struct hal *h, unsigned int rounds)
{
	int64_t offset_ns = pkt_clock_offset();
	unsigned int round;

	for (round = 0; round < rounds; round++) {
		unsigned int n = udsp_lec_capture_read(h->s, lec_batch,
						       HAL_UDSP_LEC_BATCH);
		unsigned int i;

		for (i = 0; i < n; i++)
			lec_capture_record(h, &lec_batch[i], offset_ns);

		if (n < HAL_UDSP_LEC_BATCH)
			return;
	}
}

static void lec_capture_expire(struct uloop_timeout *t)
{
	struct hal *h = container_of(t, struct hal, lec_capture);

	lec_capture_drain(h, HAL_UDSP_LEC_ROUNDS);
	uloop_timeout_set(&h->lec_capture, HAL_UDSP_TRACE_MS);
}

/* Empties the ring and writes nothing: the records of a session that is over,
 * a late tick after a disarm that timed out among them. */
static void lec_capture_discard(struct hal *h)
{
	unsigned int round;

	for (round = 0; round < HAL_UDSP_LEC_RING_ROUNDS; round++) {
		if (udsp_lec_capture_read(h->s, lec_batch, HAL_UDSP_LEC_BATCH) <
		    HAL_UDSP_LEC_BATCH)
			return;
	}
}

/*
 * The timer half. The disarm drains the whole ring and not one period: the
 * last records of the call belong to the trace, and the ring can hold more
 * than the round bound of one timer takes.
 */
static void lec_capture_arm(struct hal *h, bool on)
{
	if (!on) {
		uloop_timeout_cancel(&h->lec_capture);
		lec_capture_drain(h, HAL_UDSP_LEC_RING_ROUNDS);
		return;
	}

	if (!h->lec_capture.pending)
		uloop_timeout_set(&h->lec_capture, HAL_UDSP_TRACE_MS);
}

/*
 * The arm half. The discard runs before the library raises the bit, the
 * reverse of the packet trace: the library discards nothing (udsp.h
 * udsp_lec_capture_enable), and a NEW record published between the raise
 * and a later discard would be lost, while it is the only point a replay can
 * start from. With the bit clear no producer writes, so the discard cannot
 * take a record of this session.
 *
 * A refusal takes the bit out of the stored mask and warns, the rule of
 * pkt_trace_start.
 */
static bool lec_capture_start(struct hal *h)
{
	lec_capture_discard(h);
	if (udsp_lec_capture_enable(h->s, true)) {
		h->lec_arm_fail++;
		h->debug &= ~HAL_DEBUG_LEC_CAPTURE;
		hal_udsp_log(h, UDSP_LOG_WARN,
			     "the canceller capture found no memory and stays "
			     "off");
		return false;
	}

	lec_capture_arm(h, true);

	return true;
}

/*
 * The disarm half, after the caller lowered the library mask. The library
 * call returns once the engine tick acknowledged a command posted after the
 * clear, so no producer still holds a record half written and the drain
 * takes every record. A tick that does not acknowledge in time can still
 * publish one record per line after the drain; the next start discards it,
 * and the drain here still takes what the ring holds.
 */
static void lec_capture_stop(struct hal *h)
{
	if (udsp_lec_capture_enable(h->s, false) == -ETIMEDOUT)
		hal_udsp_log(h, UDSP_LOG_WARN,
			     "the engine tick did not confirm the end of the "
			     "canceller capture, so a late record can stay "
			     "behind");

	lec_capture_arm(h, false);
}

/* The media plane owns descriptors, so the lifecycle has two halves of its own.
 * Both live in the media section further down, beside the socket rules they
 * apply. */
static void media_fds_init(struct hal *h);
static void media_streams_stop(struct hal *h);

/*
 * The whole process is locked down. The engine tick is a
 * real time thread with a 10 ms period, and a major fault inside it is an
 * audible gap; mlockall keeps uvoiced out of the page fault path whatever the
 * SIP layer allocates later. It is not fatal: a kernel without the accounting,
 * or an RLIMIT_MEMLOCK a container imposes, must not cost the subscriber the
 * line, so the errno is reported and the attach continues.
 *
 * It runs after udsp_open(), not before, and the order matters.
 * MCL_FUTURE puts every later mapping under RLIMIT_MEMLOCK, and udsp_open
 * allocates the stream pool and the net buffers of the media plane after that
 * point; locking first therefore turns a memlock shortfall into a refused
 * attach: under an 8 MB limit, stream_pool_alloc fails with EAGAIN and
 * udsp_open returns NULL. Locking after costs the same shortfall one warning
 * and keeps the line. Everything the library mapped is still covered, by
 * MCL_CURRENT, and this still runs before uloop_run() and long before the
 * first call: between open and dsp_start the engine only ticks silence.
 */
/* Some target headers gate MCL_ONFAULT behind a feature macro; define it if the
 * build did not surface it. An old kernel rejects the flag and the fallback
 * keeps the eager lock. */
#ifndef MCL_ONFAULT
#define MCL_ONFAULT 4
#endif

static void memory_lock(void)
{
	/*
	 * MCL_ONFAULT locks each page as it faults in, so cold library text and
	 * untouched .bss stay out of RSS. libudsp prefaults every real-time
	 * working set (the tick stack in rt_apply, the net stack in net_loop, the
	 * per-stream buffers in stream_activate), so the RT path still never
	 * faults. A kernel without the flag rejects it, so fall back to the eager
	 * lock. When engine_prio > 0 the engine already locked in rt_apply; this
	 * covers the engine_prio <= 0 path and re-asserts the policy either way.
	 */
	if (mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT) == 0 ||
	    mlockall(MCL_CURRENT | MCL_FUTURE) == 0)
		return;

	ULOG_WARN("udsp: mlockall failed: %s, the engine may fault on a page\n",
		  strerror(errno));
}

static struct hal *udsp_backend_open(int event_fd)
{
	struct udsp_config scfg;
	struct cfg *cfg;
	struct hal *h;

	cfg = cfg_load();
	if (!cfg) {
		ULOG_ERR("udsp: no config, no voice hardware\n");
		return NULL;
	}

	h = calloc(1, sizeof(*h));
	if (!h) {
		cfg_free(cfg);
		return NULL;
	}

	h->jb_trace.cb = jb_trace_expire;
	h->pkt_trace.cb = pkt_trace_expire;
	h->lec_capture.cb = lec_capture_expire;
	/* Descriptor 0 is the daemon's stdin and calloc leaves every int at 0,
	 * so the media descriptors are marked absent before any op can read
	 * one. An assignment, never a close. */
	media_fds_init(h);
	hal_udsp_config_fill(&scfg, cfg);
	scfg.log = hal_udsp_log;
	scfg.log_ctx = h;
	/* The name the bring-up chain loads: the config's own region resolved
	 * the way the core will resolve it, so the initial load and the one
	 * line/line.c submits at lines_start name the same table. */
	hal_udsp_region_prefetch(h, cfg->region, h->region, sizeof(h->region));
	cfg_free(cfg);

	if (hal_relay_open(&h->relay, event_fd, "udsp", chain_take, h)) {
		ULOG_ERR("udsp: event relay: %s\n", strerror(errno));
		free(h);
		return NULL;
	}

	/*
	 * The crash recovery pass of hal.h open() is udsp_open's own design,
	 * so there is nothing to repeat here. It opens the TDM devices, starts
	 * the streams on silence, releases the SLIC reset and probes the part
	 * (udsp.h section 2): the SLIC comes out of a hardware reset with no
	 * ring running, no stream configured and every line at its reset
	 * linefeed, which is the state the pass exists to reach. A previous
	 * uvoiced cannot have left a stream behind either, because libudsp
	 * holds the audio devices for the life of the process.
	 */
	h->s = udsp_open(&scfg, h->relay.lib_fd);
	if (!h->s) {
		ULOG_ERR("udsp: udsp_open failed (tdm %s, slic %s), no voice "
			 "hardware\n", scfg.tdm_backend,
			 scfg.slic_plugin[0] ? scfg.slic_plugin : "(fake)");
		hal_relay_close(&h->relay);
		free(h);
		return NULL;
	}

	memory_lock();
	hal_udsp_cap_fill(&h->cap, udsp_cap(h->s));
	if (scfg.fax_plugin[0] && !(h->cap.features & HAL_FEAT_T38))
		ULOG_ERR("udsp: fax plug-in %s unavailable, fax passthrough "
			 "only\n", scfg.fax_plugin);
	hal_udsp_fxsnum_write(h->cap.lines);

	/* No region is named here: the tone table arrives with the first
	 * region_apply job, which line/line.c submits at lines_start. */
	ULOG_INFO("udsp hal attached: %u lines, %u streams per channel, slic %s, "
		  "%u Hz, features 0x%08x, codecs 0x%08x\n", h->cap.lines,
		  h->cap.streams_per_chan, h->cap.slic,
		  (h->cap.features & HAL_FEAT_WIDEBAND) ? 16000u : 8000u,
		  h->cap.features, h->cap.codecs);

	return h;
}

static void udsp_backend_close(struct hal *h)
{
	if (!h)
		return;

	/*
	 * The diagnostic producers and their timers stop before anything is
	 * freed. main.c closes the backend while the uloop is still up, so a
	 * timer left armed here fires on the next turn against a freed handle.
	 * The library flags are cleared in the same breath, because the engine
	 * tick and the net thread outlive this call by as long as udsp_close
	 * takes to join them. udsp_close frees the packet trace rings and the
	 * canceller capture ring.
	 */
	uloop_timeout_cancel(&h->jb_trace);
	uloop_timeout_cancel(&h->pkt_trace);
	uloop_timeout_cancel(&h->lec_capture);
	udsp_debug_set(h->s, 0);
	udsp_pkt_trace_enable(h->s, false);
	udsp_lec_capture_enable(h->s, false);

	/*
	 * The media plane goes first, while the library can still act on a
	 * stream: every pair this backend still holds is stopped through
	 * udsp_stream_stop and closed behind it. On the daemon's own path the
	 * core has already stopped every stream through the op, so this finds
	 * nothing; it covers a close that skipped the teardown.
	 */
	media_streams_stop(h);

	/* udsp_close is release-only and idempotent, and the orderly stop is
	 * the dsp_stop job the core already ran (udsp.h section 2). It joins
	 * every library thread, so no record can arrive after it and the relay
	 * closes behind it with nothing in flight. Its stream pool shutdown
	 * also drops any dup a dead-marked stream still held and zeroizes the
	 * key material with it. */
	udsp_close(h->s);
	hal_relay_close(&h->relay);
	free(h);
}

/*
 * --- line control ---------------------------------------------------------
 *
 * the _op suffix is not decoration. Six hal ops carry the name of the udsp
 * function they call (line_hook_query, tone_play, tone_stop, digit_play,
 * stream_start, stream_stop), and a static function of that name would shadow
 * the library symbol inside this file and call itself. Every other op keeps
 * the plain hal name.
 */

static enum hal_status udsp_line_state_set(struct hal *h, unsigned int line,
					   enum hal_line_state state)
{
	return call_check(udsp_line_state(h->s, line, (enum udsp_line_state)state),
			  "line_state");
}

static enum hal_status udsp_line_polarity_set(struct hal *h, unsigned int line,
					      enum hal_polarity pol)
{
	return call_check(udsp_line_polarity(h->s, line, (enum udsp_polarity)pol),
			  "line_polarity");
}

static enum hal_status udsp_line_hook_query_op(struct hal *h, unsigned int line,
					       bool *offhook)
{
	return call_check(udsp_line_hook_query(h->s, line, offhook), "hook_query");
}

static enum hal_status udsp_line_hook_timing_set(struct hal *h, unsigned int line,
						 const struct hal_hook_timing *ts)
{
	return call_check(udsp_line_hook_timing(h->s, line, ts->flash_min_ms,
						ts->flash_max_ms,
						ts->release_min_ms),
			  "hook_timing");
}

/*
 * The visual message waiting rule (udsp.h struct udsp_cid).
 * hal_cid.mwi is an enum and udsp_cid.mwi is a bool, and that is the whole
 * difference: On becomes true, which is the indicator-on burst, and off
 * becomes false. The rest of the record passes through unchanged, because the
 * off burst is what libudsp reads out of an all-empty identity: uvoice
 * mwi_fsk_send() builds exactly that record, and its cid_fill() always writes
 * blockinfo for a real caller, so no caller identity can be mistaken for one.
 */
void hal_udsp_cid_fill(struct udsp_cid *dst, const struct hal_cid *src)
{
	memset(dst, 0, sizeof(*dst));
	dst->format = src->format;
	dst->mwi = src->mwi == HAL_MWI_ON;
	dst->number_private = src->number_private;
	dst->name_private = src->name_private;
	memcpy(dst->number, src->number, sizeof(dst->number));
	memcpy(dst->name, src->name, sizeof(dst->name));
	memcpy(dst->datetime, src->datetime, sizeof(dst->datetime));
	memcpy(dst->blockinfo, src->blockinfo, sizeof(dst->blockinfo));
}

void hal_udsp_ring_fill(struct udsp_ring *dst, const struct hal_ring *src)
{
	unsigned int i;

	memset(dst, 0, sizeof(*dst));
	for (i = 0; i < HAL_CADENCE_MAX; i++) {
		dst->cadence[i].on_ms = src->cadence[i].on_ms;
		dst->cadence[i].off_ms = src->cadence[i].off_ms;
		dst->first[i].on_ms = src->first[i].on_ms;
		dst->first[i].off_ms = src->first[i].off_ms;
	}

	dst->cadence_count = src->cadence_count;
	dst->first_count = src->first_count;
	dst->cid_at_ring = src->cid_at_ring;
	dst->cid_wait_ms = src->cid_wait_ms;
	dst->duration_ms = src->duration_ms;
	dst->cid_present = src->cid_present;

	if (src->cid_present)
		hal_udsp_cid_fill(&dst->cid, &src->cid);
}

static enum hal_status udsp_line_ring_start(struct hal *h, unsigned int line,
					    const struct hal_ring *ring)
{
	struct udsp_ring r;

	hal_udsp_ring_fill(&r, ring);

	return call_check(udsp_ring_start(h->s, line, &r), "ring_start");
}

static enum hal_status udsp_line_ring_stop(struct hal *h, unsigned int line)
{
	return call_check(udsp_ring_stop(h->s, line), "ring_stop");
}

static enum hal_status udsp_line_cid_send(struct hal *h, unsigned int line,
					  enum hal_cid_kind kind,
					  const struct hal_cid *cid)
{
	struct udsp_cid c;

	hal_udsp_cid_fill(&c, cid);

	return call_check(udsp_cid_send(h->s, line, (enum udsp_cid_kind)kind, &c),
			  "cid_send");
}

/* The lamp path alone: the FSK indicator travels
 * line_cid_send, which is how the core drives it. */
static enum hal_status udsp_line_mwi_set(struct hal *h, unsigned int line,
					 enum hal_mwi mwi)
{
	return call_check(udsp_mwi(h->s, line, mwi == HAL_MWI_ON), "mwi");
}

static enum hal_status udsp_line_test_result_get(struct hal *h, unsigned int line,
						 void *buf, size_t buf_len,
						 size_t *out_len)
{
	return call_check(udsp_line_test_result(h->s, line, buf, buf_len,
						 out_len), "line_test_result");
}

/* --- tones, digits and the channel ---------------------------------------- */

static enum hal_status udsp_tone_play_op(struct hal *h, unsigned int line,
					 enum hal_tone tone, uint32_t duration_ms)
{
	return call_check(udsp_tone_play(h->s, line, (enum udsp_tone)tone,
					 duration_ms), "tone_play");
}

static enum hal_status udsp_tone_stop_op(struct hal *h, unsigned int line)
{
	return call_check(udsp_tone_stop(h->s, line), "tone_stop");
}

static enum hal_status udsp_cw_tone_play_op(struct hal *h, unsigned int line,
					    unsigned int variant,
					    uint32_t duration_ms)
{
	return call_check(udsp_cw_tone_play(h->s, line, variant, duration_ms),
			  "cw_tone_play");
}

static enum hal_status udsp_digit_play_op(struct hal *h, unsigned int line,
					  char digit, uint32_t duration_ms)
{
	return call_check(udsp_digit_play(h->s, line, digit, duration_ms),
			  "digit_play");
}

static enum hal_status udsp_channel_config_set(struct hal *h, unsigned int line,
					       const struct hal_channel_cfg *cfg)
{
	struct udsp_chan_cfg c;

	memset(&c, 0, sizeof(c));
	c.detect_mask = cfg->detect_mask;
	/* The HAL pair names its gains by the interface (hal.h), and libudsp
	 * names its gains by the direction of the signal: gain_tx acts on what
	 * the subscriber sends. The backend owns the translation. */
	c.gain_tx = cfg->gain_rx;
	c.gain_rx = cfg->gain_tx;
	c.ec_enable = cfg->ec_enable;
	c.ec_nlp_enable = cfg->ec_nlp_enable;
	c.ec_tail_ms = cfg->ec_tail_ms;
	c.slic_gain_tx = cfg->slic_gain_tx;
	c.slic_gain_rx = cfg->slic_gain_rx;
	c.ec_tone_latch = cfg->ec_tone_latch;
	c.ec_tone_force_disable = cfg->ec_tone_force_disable;

	return call_check(udsp_chan_config(h->s, line, &c), "chan_config");
}

/* --- media --------------------------------------------------------- */

/*
 * The population gate, and the array bound in the same test.
 * cap.lines is the minimum of what the SLIC part reports, what the platform
 * database says the board populates and what hal.h can name, so a media op on
 * a line the board does not carry answers HAL_UNSUPPORTED before it can index
 * anything. libudsp gates on its own cap as well (core udsp_guard), but its
 * number can be the larger one: HAL_LINE_MAX is 2 and UDSP_LINE_MAX is 8.
 */
static struct hal_udsp_stream *stream_get(struct hal *h, unsigned int line,
					  unsigned int stream)
{
	if (line >= h->cap.lines || stream >= h->cap.streams_per_chan)
		return NULL;

	return &h->stream[line][stream];
}

/*
 * The codec selector is a switch and not a cast. The two enums agree on the
 * first four members and diverge after them, and udsp_events.c asserts both
 * halves of that: the agreement by name, the divergence as a disagreement, so
 * neither can be flattened into a cast by accident.
 *
 * The three missing rows are missing for three different reasons. hal.h names
 * one G.726 rate and it is the 32 kbit/s one, so the 16, 24 and 40 kbit/s
 * coders libudsp carries cannot be expressed in a hal_media at all. The two
 * V.152 voice band data codecs have no libudsp member of any kind: udsp.h
 * carries no VBD selector, and passthrough is what libudsp does with a G.711
 * stream and a fax profile instead. T38 has a member and is named here.
 * media_validate() refuses it on the library codec bit before any socket.
 * The library also applies its own capability gate.
 */
int hal_udsp_codec_sel(uint8_t codec)
{
	switch (codec) {
	case HAL_CODEC_PCMU:
		return UDSP_SEL_PCMU;
	case HAL_CODEC_PCMA:
		return UDSP_SEL_PCMA;
	case HAL_CODEC_G729:
		return UDSP_SEL_G729;
	case HAL_CODEC_G726_32:
		return UDSP_SEL_G726_32;
	case HAL_CODEC_T38:
		return UDSP_SEL_T38;
	case HAL_CODEC_G722:
		return UDSP_SEL_G722;
	case HAL_CODEC_OPUS:
		return UDSP_SEL_OPUS;
	case HAL_CODEC_NONE:
	case HAL_CODEC_VBD_PCMU:
	case HAL_CODEC_VBD_PCMA:
		break;
	}

	return -1;
}

/*
 * HAL_FAX_VBD has no library member, and that is the whole reason this is a
 * switch: udsp.h names three fax modes and hal.h names four, so the T.38 value
 * sits at a different number on the two sides. A V.152 request answers -1 and
 * the op refuses it, which is honest: libudsp implements passthrough by
 * clamping the stream to G.711 and switching the suppression family off,
 * and it carries no voice band data mode of its own.
 */
int hal_udsp_fax_mode(uint8_t mode)
{
	switch (mode) {
	case HAL_FAX_OFF:
		return UDSP_FAX_OFF;
	case HAL_FAX_PASSTHROUGH:
		return UDSP_FAX_PASSTHROUGH;
	case HAL_FAX_T38:
		return UDSP_FAX_T38;
	case HAL_FAX_VBD:
		break;
	}

	return -1;
}

/*
 * The media record, field by field. Every enum below is pinned as a pair in
 * udsp_events.c, so the byte that travels means the same thing on both sides;
 * the codec is the one field with a map in front of it.
 *
 * Both endpoints travel whole, all sixteen address bytes and both ports, so an
 * IPv6 description reaches the library as it stands. hal_media.rtcp_remote is
 * the one address that does not travel: struct udsp_media has no field for it,
 * and it needs none, because this backend owns the connect and libudsp sends on
 * the descriptor it was handed (udsp.h section 3).
 *
 * The key material is copied only for a suite that names one. hal.h says the
 * two keys are meaningless on a plain stream and a backend must not read them
 * in that mode. A suite this function cannot name leaves both keys zero
 * rather than a plausible looking one, so a value that never passed
 * media_validate() can never key a stream whatever a later refactor does to
 * the paths that reach here.
 *
 * dst holds key bytes when this returns. The caller zeroizes it once the
 * library call is done (udsp.h struct udsp_media: libudsp holds the keys
 * from there and zeroizes its own copy at stream stop).
 */
void hal_udsp_media_fill(struct udsp_media *dst, const struct hal_media *src)
{
	int sel = hal_udsp_codec_sel(src->codec);

	memset(dst, 0, sizeof(*dst));

	dst->local.ipver = src->local.ipver;
	memcpy(dst->local.addr, src->local.addr, sizeof(dst->local.addr));
	dst->local.port = src->local.port;
	dst->local.rtcp_port = rtcp_port_of(&src->local);
	dst->remote.ipver = src->remote.ipver;
	memcpy(dst->remote.addr, src->remote.addr, sizeof(dst->remote.addr));
	dst->remote.port = src->remote.port;
	dst->remote.rtcp_port = rtcp_port_of(&src->remote);

	/* A codec with no member reaches the library as UDSP_SEL_NONE, which it
	 * refuses; media_validate() has already refused it one step earlier. */
	dst->codec = sel < 0 ? (uint8_t)UDSP_SEL_NONE : (uint8_t)sel;
	dst->codec_pt = src->codec_pt;
	dst->dtmf_pt = src->dtmf_pt;
	dst->dtmf_mode = src->dtmf_mode;
	dst->direction = src->direction;
	dst->ptime_ms = src->ptime_ms;
	dst->jb_mode = src->jb_mode;
	dst->vad = src->vad;
	dst->cng = src->cng;
	/* The codec-native discontinuous transmission of hal.h, which libudsp
	 * spells dtx. The library forces the generic pair above off behind it
	 * and clears it on a codec with no SID frame, so this copy states the
	 * negotiation and settles nothing. The G.729 policy sits above this
	 * copy too: media_fill clears vad on an unarmed G.729 leg. */
	dst->dtx = src->annexb;
	dst->rtcp_enable = src->rtcp_enable;
	dst->rtp_dscp = src->rtp_dscp;
	dst->rtcp_dscp = src->rtcp_dscp;
	dst->jb_init_ms = src->jb_init_ms;
	dst->jb_max_ms = src->jb_max_ms;
	dst->rtcp_interval_ms = src->rtcp_interval_ms;
	dst->punch_interval_ms = src->punch_interval_ms;
	memcpy(dst->bind_dev, src->bind_dev, sizeof(dst->bind_dev));

	dst->t38.max_bit_rate = src->t38.max_bit_rate;
	dst->t38.max_datagram = src->t38.max_datagram;
	dst->t38.ec = src->t38.ec;
	dst->opus.bitrate = src->opus.bitrate;
	dst->opus.complexity = src->opus.complexity;
	dst->opus.max_bandwidth = src->opus.max_bandwidth;
	dst->opus.fec = src->opus.fec;
	/* The hold silence of hal.h, which libudsp spells the same way. It sits
	 * above the SRTP return like every other scalar, so a plain stream
	 * carries it too. */
	dst->tx_silence = src->tx_silence;
	/* The passthrough kind, one vocabulary pinned against the other by name
	 * in udsp_events.c, so the byte travels as it stands. */
	dst->passthrough = src->passthrough;

	if (src->srtp.suite != HAL_SRTP_AES_CM_128_SHA1_80 &&
	    src->srtp.suite != HAL_SRTP_AES_CM_128_SHA1_32)
		return;

	dst->srtp.suite = src->srtp.suite;
	memcpy(dst->srtp.key_tx, src->srtp.key_tx, sizeof(dst->srtp.key_tx));
	memcpy(dst->srtp.key_rx, src->srtp.key_rx, sizeof(dst->srtp.key_rx));
}

/*
 * One for one. hal.h reads the MOS values as floats of a scale it does not
 * document and the two decimal presentation belongs to the consumer
 * (udsp.h struct udsp_media_stats), so nothing is rounded, scaled or clamped
 * here: a number this layer reshaped could not be told from one the engine
 * measured.
 */
void hal_udsp_stats_fill(struct hal_media_stats *dst,
			 const struct udsp_media_stats *src)
{
	memset(dst, 0, sizeof(*dst));
	dst->packets_sent = src->packets_sent;
	dst->packets_recv = src->packets_recv;
	dst->packets_lost = src->packets_lost;
	dst->packets_discarded = src->packets_discarded;
	dst->loss_rate = src->loss_rate;
	dst->loss_rate_far = src->loss_rate_far;
	dst->jitter_avg = src->jitter_avg;
	dst->jitter_max = src->jitter_max;
	dst->jitter_far = src->jitter_far;
	dst->rtt_avg = src->rtt_avg;
	dst->rtt_max = src->rtt_max;
	dst->mos_lq = src->mos_lq;
	dst->mos_cq = src->mos_cq;
	dst->rfactor = src->rfactor;
	dst->bytes_sent = src->bytes_sent;
	dst->bytes_recv = src->bytes_recv;
}

/*
 * The JITTER buffer counters. One field at a time, for the reason
 * hal_udsp_stats_fill is: hal.h and udsp.h keep the two records as separate
 * types so the core carries no library header, and the getter copies rather
 * than asserting a layout. pool_free is the gauge and travels like every
 * other field.
 */
void hal_udsp_jb_stats_fill(struct hal_jb_stats *dst,
			    const struct udsp_jb_stats *src)
{
	memset(dst, 0, sizeof(*dst));
	dst->frames_out = src->frames_out;
	dst->plc = src->plc;
	dst->cn = src->cn;
	dst->silence = src->silence;
	dst->late_drop = src->late_drop;
	dst->dup_drop = src->dup_drop;
	dst->overflow_drop = src->overflow_drop;
	dst->hold_drop = src->hold_drop;
	dst->resync_discard_ms = src->resync_discard_ms;
	dst->resyncs = src->resyncs;
	dst->starve_resyncs = src->starve_resyncs;
	dst->cn_reanchors = src->cn_reanchors;
	dst->underruns = src->underruns;
	dst->skew_drops = src->skew_drops;
	dst->skew_inserts = src->skew_inserts;
	dst->target_ms = src->target_ms;
	dst->depth_ms = src->depth_ms;
	dst->jitter_ms = src->jitter_ms;
	dst->pool_free = src->pool_free;
	dst->mode = src->mode;
}

/*
 * The line canceller state, copied one field at a time for the reason the two
 * fills above are: hal.h and udsp.h keep the two records as separate types so
 * the core carries no library header. The scaled fields keep the library's
 * scale, milli-decibels and micro-units, because a number this layer rescaled
 * could not be told from one the canceller measured.
 */
void hal_udsp_lec_stats_fill(struct hal_lec_stats *dst,
			     const struct udsp_lec_stats *src)
{
	memset(dst, 0, sizeof(*dst));
	dst->tail_ms = src->tail_ms;
	dst->taps = src->taps;
	dst->frames = src->frames;
	dst->hold_frames = src->hold_frames;
	dst->skip_frames = src->skip_frames;
	dst->retimes = src->retimes;
	dst->nlp_active_frames = src->nlp_active_frames;
	dst->nlp_wait_frames = src->nlp_wait_frames;
	dst->nlp_erle_frames = src->nlp_erle_frames;
	dst->nlp_unlike_frames = src->nlp_unlike_frames;
	dst->nlp_dt_frames = src->nlp_dt_frames;
	dst->nlp_quiet_frames = src->nlp_quiet_frames;
	dst->coef_peak_tap = src->coef_peak_tap;
	dst->loop_open = src->loop_open;
	dst->diverge_resets = src->diverge_resets;
	dst->probes = src->probes;
	dst->erle_mdb = src->erle_mdb;
	dst->coef_energy_micro = src->coef_energy_micro;
	dst->echo_gain_micro = src->echo_gain_micro;
	dst->double_talk = src->double_talk;
	dst->nlp_active = src->nlp_active;
	dst->ref_delay_samples = src->ref_delay_samples;
	dst->ref_ready = src->ref_ready;
	dst->nlp_enabled = src->nlp_enabled;
	dst->ec_active = src->ec_active;
	dst->passthrough = src->passthrough;
}

/*
 * The newest line-monitor reading, copied one field at a time for the reason
 * the fill above is: the two records stay separate types. The measurements
 * keep the library's units, mV, uA and mW. The decoded booleans keep the
 * library's decode. A number this layer recomputed could not be told from one
 * the part measured.
 */
static void hal_udsp_monitor_fill(struct hal_monitor *dst,
				  const struct udsp_monitor *src)
{
	memset(dst, 0, sizeof(*dst));
	dst->stamp_ms = src->stamp_ms;
	dst->valid = src->valid;
	dst->stale = src->stale;
	dst->vtr_mv = src->vtr_mv;
	dst->vtip_mv = src->vtip_mv;
	dst->vring_mv = src->vring_mv;
	dst->vlong_mv = src->vlong_mv;
	dst->vbat_mv = src->vbat_mv;
	dst->vdc_mv = src->vdc_mv;
	dst->itr_ua = src->itr_ua;
	dst->itip_ua = src->itip_ua;
	dst->iring_ua = src->iring_ua;
	dst->ilong_ua = src->ilong_ua;
	dst->p_hvic_mw = src->p_hvic_mw;
	dst->vbat_tgt_mv = src->vbat_tgt_mv;
	dst->vbat_irq_th_mv = src->vbat_irq_th_mv;
	dst->pd_dcdc = src->pd_dcdc;
	dst->dcdc_status = src->dcdc_status;
	dst->lcrrtp = src->lcrrtp;
	dst->linefeed = src->linefeed;
	dst->dcdc_down = src->dcdc_down;
	dst->ovlo = src->ovlo;
	dst->settled = src->settled;
	dst->voc_err = src->voc_err;
	dst->lcr = src->lcr;
	dst->rtp = src->rtp;
	dst->fault_bits = src->fault_bits;
	dst->fault_open = src->fault_open;
	dst->fault_held = src->fault_held;
}

/*
 * The transport and encryption counters of one stream, copied one field at a
 * time for the reason the fills above are: the two records stay separate types
 * so the core carries no library header.
 */
static void hal_udsp_stream_diag_fill(struct hal_stream_diag *dst,
				      const struct udsp_stream_diag *src)
{
	memset(dst, 0, sizeof(*dst));
	dst->net_drops = src->net_drops;
	dst->net_stale = src->net_stale;
	dst->net_trunc = src->net_trunc;
	dst->tx_fail = src->tx_fail;
	dst->punch_sent = src->punch_sent;
	dst->srtp_protect_ok = src->srtp_protect_ok;
	dst->srtp_protect_fail = src->srtp_protect_fail;
	dst->srtp_unprotect_ok = src->srtp_unprotect_ok;
	dst->srtp_unprotect_auth_fail = src->srtp_unprotect_auth_fail;
	dst->srtp_unprotect_replay = src->srtp_unprotect_replay;
}

/*
 * The engine snapshot, copied the same way. The histogram travels in one move
 * and the assert at the head of this file binds the two widths.
 */
static void hal_udsp_engine_diag_fill(struct hal_engine_diag *dst,
				      const struct udsp_engine_diag *src)
{
	memset(dst, 0, sizeof(*dst));
	dst->serial = src->serial;
	dst->ticks = src->ticks;
	memcpy(dst->interval, src->interval, sizeof(dst->interval));
	dst->interval_min_us = src->interval_min_us;
	dst->interval_max_us = src->interval_max_us;
	dst->capture_stalls = src->capture_stalls;
	dst->delay_stalls = src->delay_stalls;
	dst->xruns = src->xruns;
	dst->holds = src->holds;
	dst->capture_drops = src->capture_drops;
	dst->trim_drops = src->trim_drops;
	dst->playback_expired = src->playback_expired;
	dst->recover_failures = src->recover_failures;
	dst->read_errors = src->read_errors;
	dst->write_errors = src->write_errors;
	dst->cal_restarts = src->cal_restarts;
	dst->delay_ref = src->delay_ref;
	dst->loop_ref = src->loop_ref;
	dst->delay_drift_min = src->delay_drift_min;
	dst->delay_drift_max = src->delay_drift_max;
	dst->granted_periods = src->granted_periods;
	dst->period_us = src->period_us;
	dst->running = src->running;
	dst->stopped_fatal = src->stopped_fatal;
	dst->delay_calibrated = src->delay_calibrated;
}

/*
 * The region view, copied one field at a time for the reason the fills above
 * are: the two records stay separate types so the core carries no library
 * header. The format bytes assign directly because the two enums are asserted
 * equal member by member.
 */
void hal_udsp_region_fill(struct hal_region *dst,
			  const struct udsp_region_view *src)
{
	unsigned i, k;

	memset(dst, 0, sizeof(*dst));
	snprintf(dst->iso3, sizeof(dst->iso3), "%s", src->iso3);
	dst->cid_format = src->cid_format;
	dst->mwi_format = src->mwi_format;
	dst->hook_flash_min_ms = src->hook_flash_min_ms;
	dst->hook_flash_max_ms = src->hook_flash_max_ms;
	dst->hook_release_min_ms = src->hook_release_min_ms;
	dst->cw_sas_total_ms = src->cw_sas_total_ms;
	dst->flash_service = src->flash_service;
	snprintf(dst->describe, sizeof(dst->describe), "%s", src->describe);
	dst->pulse_invert = src->pulse_invert;
	/* libudsp reports a confirm tone that never ends as 0, so 0 here is
	 * known to be endless, unlike the 0 of a zeroed record. */
	dst->confirm_total_ms = src->confirm_total_ms;
	dst->confirm_endless = !src->confirm_total_ms;
	for (i = 0; i < src->ring_count && i < HAL_REGION_RING_MAX; i++) {
		for (k = 0; k < src->ring[i].cadence_count; k++) {
			dst->ring[i].cadence[k].on_ms =
				src->ring[i].cadence[k].on_ms;
			dst->ring[i].cadence[k].off_ms =
				src->ring[i].cadence[k].off_ms;
		}
		dst->ring[i].cadence_count = src->ring[i].cadence_count;
	}
	/* cw[] is variant indexed: index 0 is the plain tone, 1..cw_count the
	 * distinctive rows. cw_sas_total_ms becomes the maximum over the base
	 * and every variant, so the offer window covers whichever one plays. */
	dst->cw_count = src->cw_count;
	for (i = 0; i < HAL_REGION_CW_MAX; i++) {
		dst->cw_lead_in_ms[i] = src->cw[i].lead_ms;
		if (src->cw[i].total_ms > dst->cw_sas_total_ms)
			dst->cw_sas_total_ms = src->cw[i].total_ms;
	}
}

/*
 * The media sockets live in the shared hal_media.c: struct hal_media_sock and
 * hal_media_sock_open / _connect / _tos_refresh / _close. libudsp never creates
 * one (udsp.h section 3), so this backend owns the pair and hands the numbers
 * to the library. The order inside a program is in udsp_stream_config_set.
 */

/*
 * What this backend can program at all, tested before anything is touched
 * (hal.h stream_config_set rule 4). A refusal must leave a running stream
 * exactly as it was, and every test below is answered from the caller's record
 * and the library's own capability, so nothing is bound, marked, connected or
 * handed over until all of them pass.
 *
 * The capability is asked, not remembered. The codec and the SRTP tests read
 * udsp_cap() rather than a table of this file's own, so a library built
 * without a coder or without libsrtp2 refuses here instead of failing one step
 * later with the sockets already made.
 */
static enum hal_status media_validate(struct hal *h, const struct hal_media *m)
{
	const struct udsp_cap *cap = udsp_cap(h->s);
	int sel = hal_udsp_codec_sel(m->codec);

	/*
	 * The defence against a silent downgrade. A suite the backend cannot
	 * carry is a capability it lacks, so the refusal is HAL_UNSUPPORTED and
	 * not the HAL_FATAL of a missing parameter (hal.h enum hal_srtp_suite).
	 */
	if (!srtp_suite_known(m->srtp.suite)) {
		TRACE(TRACE_HAL, "udsp: media srtp suite %u is not one this "
		      "backend carries", m->srtp.suite);
		return HAL_UNSUPPORTED;
	}

	if (m->srtp.suite != HAL_SRTP_NONE && !(cap->features & UDSP_FEAT_SRTP)) {
		TRACE(TRACE_HAL, "udsp: media srtp suite %u on a library with no "
		      "srtp", m->srtp.suite);
		return HAL_UNSUPPORTED;
	}

	if (sel < 0 || !(cap->codecs & (1u << (unsigned)sel))) {
		TRACE(TRACE_HAL, "udsp: media codec %u is not one the library "
		      "carries", m->codec);
		return HAL_UNSUPPORTED;
	}

	/*
	 * A kind the enum does not name is a policy this backend cannot carry,
	 * so it is refused as a capability like the suite and the codec above.
	 * The library refuses it too, but only once the sockets are made, and
	 * rule 4 asks for the test in front of them.
	 */
	if (m->passthrough > HAL_PASSTHROUGH_FAX) {
		TRACE(TRACE_HAL, "udsp: media passthrough kind %u is not one "
		      "this backend carries", m->passthrough);
		return HAL_UNSUPPORTED;
	}

	/*
	 * The address shape (families, ports, the RTCP port wrap) is the shared
	 * check, which reads the backend's feature bits: this backend sets
	 * HAL_FEAT_IPV6_MEDIA, so it carries IPv6.
	 */
	return hal_media_validate_addrs(m, h->cap.features);
}

/*
 * The stream stop of this form, used by the op and by the local address move.
 * The library stop comes first and the descriptors close after it: libudsp
 * drops its own dups inside that call, so by the time it returns nothing but
 * this backend names the sockets.
 *
 * The descriptors close whatever the library answered. libudsp holds a dup,
 * which is an independent descriptor, so closing this side can strand nothing.
 * A stream the library dead-marked keeps its dups until udsp_close tears the
 * pool down, and holding the originals open beside them would leak two
 * descriptors per dead stream for the life of the daemon.
 */
static enum hal_status stream_halt(struct hal *h, unsigned int line,
				   unsigned int stream, struct hal_udsp_stream *s)
{
	enum hal_status st = call_check(udsp_stream_stop(h->s, line, stream),
					"stream_stop");

	if (st == HAL_FATAL)
		stream_dead_report(h, line, stream, "stream_stop");

	hal_media_sock_close(&s->sock);
	s->running = false;
	/* The pushed configuration named descriptors that no longer exist, so
	 * the stream must be programmed again before it can start. */
	s->configured = false;

	return st;
}

/*
 * The sockets a programming pass needs. A stream that already holds a pair on
 * the same local tuple keeps it: a re-INVITE re-applies the configuration on a
 * running stream with no stop, and libudsp re-dups whatever it is passed
 * unconditionally, so the same descriptors are exactly what that
 * pass should hand over again. A local address or port move cannot rebind a
 * live socket, so that stream stops, closes and starts from a new pair.
 *
 * The bound device is part of that local identity. libudsp never binds
 * anything, so hal_media.bind_dev reaches the wire only through the
 * SO_BINDTODEVICE this backend set when it made the socket; a description that
 * changed the device alone would otherwise keep sending out of the old one for
 * the rest of the call. It is treated as a move rather than as an in-place
 * setsockopt because it is one: hal.h already routes every change of the local
 * binding through a new pair, and re-homing a live media socket onto another
 * netdev mid-call would change the source address under a flow the far end is
 * already filtering on.
 */
static enum hal_status stream_sockets_take(struct hal *h, unsigned int line,
					   unsigned int stream,
					   struct hal_udsp_stream *s,
					   const struct hal_media *m, bool *fresh)
{
	enum hal_status st;

	*fresh = false;
	if (s->sock.rtp_fd >= 0 && addr_same(&s->sock.local, &m->local) &&
	    !memcmp(s->sock.bind_dev, m->bind_dev, sizeof(s->sock.bind_dev)))
		return HAL_OK;

	if (s->sock.rtp_fd >= 0) {
		TRACE(TRACE_HAL, "udsp: media local port %u moved to %u, new "
		      "sockets", s->sock.local.port, m->local.port);
		st = stream_halt(h, line, stream, s);
		if (st != HAL_OK && st != HAL_UNSUPPORTED)
			return st;
	}

	st = hal_media_sock_open(&s->sock, m);
	if (st != HAL_OK)
		return st;

	*fresh = true;

	return HAL_OK;
}

/*
 * The reconfiguration of a running stream is a bare re-push, which
 * is the common re-INVITE: call.c re-calls this op on a live stream with no
 * preceding stop, and it stops first only for a T.38 mode move or an SRTP
 * rekey. The descriptors are therefore passed again unconditionally, and
 * libudsp quiesces its net and engine I/O on the prior dups, drops them and
 * re-dups what it was handed. It never compares fd numbers to detect the same
 * socket, because the OS reuses closed numbers and a number is not an
 * identity.
 *
 * The order inside one pass. Validate, take the sockets, connect if the far
 * end moved or the pair is new, refresh the mark if the pair is not, hand over,
 * zeroize. Everything that can refuse comes before anything that can be seen,
 * so a refusal leaves a running stream exactly as it was.
 *
 * Which failure closes the pair. libudsp holds a dup, which is an independent
 * descriptor, so closing this side can strand nothing at any point. The split
 * is therefore about the program and not about safety: a pair this pass
 * created belongs to a program that never happened and closes with it, and a
 * pair an earlier pass handed over belongs to the stream the failure left
 * standing and is kept for its ordered stop.
 */
static enum hal_status udsp_stream_config_set(struct hal *h, unsigned int line,
					      unsigned int stream,
					      const struct hal_media *media)
{
	struct hal_udsp_stream *s = stream_get(h, line, stream);
	struct hal_addr rtcp;
	struct udsp_media m;
	enum hal_status st;
	bool was_running;
	bool fresh;

	if (!s || !media)
		return HAL_UNSUPPORTED;

	/* Read before stream_sockets_take, which halts a running stream to
	 * rebind it and clears the flag on the way (see the restart below). */
	was_running = s->running;

	st = media_validate(h, media);
	if (st != HAL_OK)
		return st;

	st = stream_sockets_take(h, line, stream, s, media, &fresh);
	if (st != HAL_OK)
		return st;

	/* Both peers decide the reconnect. A description that kept its media
	 * address and named another RTCP address moved the far end of one
	 * descriptor, and RTCP would keep going to the old one otherwise. */
	hal_media_rtcp_peer(media, &rtcp);
	if (fresh || !addr_same(&s->sock.remote, &media->remote) ||
	    !addr_same(&s->sock.rtcp_remote, &rtcp)) {
		st = hal_media_sock_connect(&s->sock, media);
		if (st != HAL_OK) {
			/* Nothing reached the library in this pass, so a pair
			 * this pass created can close here; one an earlier pass
			 * handed over stays, because libudsp holds its dups. */
			if (fresh)
				hal_media_sock_close(&s->sock);

			return st;
		}
	}

	if (!fresh)
		hal_media_sock_tos_refresh(&s->sock, media);

	hal_udsp_media_fill(&m, media);
	st = call_check(udsp_stream_config(h->s, line, stream, &m, s->sock.rtp_fd,
					   s->sock.rtcp_fd), "stream_config");
	/* The stack copy is dead from here whatever the library answered,
	 * and it holds both keys. */
	explicit_bzero(&m, sizeof(m));
	if (st != HAL_OK) {
		if (st == HAL_FATAL)
			stream_dead_report(h, line, stream, "stream_config");
		if (fresh)
			hal_media_sock_close(&s->sock);

		return st;
	}

	s->configured = true;
	/* The description the packet trace renders a record of this generation
	 * with. It is taken after the connect wrote the endpoints, and it
	 * outlives the stop that closes them. */
	pkt_gen_add(h, line, stream, s);

	/*
	 * The local move halted a running stream and nobody else will start it
	 * again. hal.h has a local address or port move stop the stream and
	 * rebind, and stream_halt() cleared running with it; the core does not
	 * put it back, because call.c media_program only calls stream_start for
	 * a leg whose media has not started yet and this leg's has (its
	 * media_started is still set). Without this the re-INVITE that moved the
	 * port would return HAL_OK and the call would go silent for the rest of
	 * its life.
	 *
	 * A re-program that kept its pair never entered stream_halt, so its
	 * running flag still stands and this restart is skipped: libudsp keeps
	 * a reconfigured stream running by itself.
	 */
	if (!was_running || s->running)
		return HAL_OK;

	st = call_check(udsp_stream_start(h->s, line, stream), "stream_start");
	if (st != HAL_OK) {
		if (st == HAL_FATAL)
			stream_dead_report(h, line, stream, "stream_start");
		return st;
	}

	s->running = true;

	return HAL_OK;
}

static enum hal_status udsp_stream_start_op(struct hal *h, unsigned int line,
					    unsigned int stream)
{
	struct hal_udsp_stream *s = stream_get(h, line, stream);
	enum hal_status st;

	if (!s)
		return HAL_UNSUPPORTED;

	/* A start with no program behind it is a core defect and not a missing
	 * capability. */
	if (!s->configured)
		return HAL_FATAL;

	st = call_check(udsp_stream_start(h->s, line, stream), "stream_start");
	if (st != HAL_OK) {
		if (st == HAL_FATAL)
			stream_dead_report(h, line, stream, "stream_start");
		return st;
	}

	s->running = true;

	return HAL_OK;
}

static enum hal_status udsp_stream_stop_op(struct hal *h, unsigned int line,
					   unsigned int stream)
{
	struct hal_udsp_stream *s = stream_get(h, line, stream);

	if (!s)
		return HAL_UNSUPPORTED;

	return stream_halt(h, line, stream, s);
}

/*
 * The digit is not validated here. libudsp maps the character to its RFC 4733
 * event on the engine side and answers -EINVAL for one it cannot, which
 * reaches the caller as HAL_UNSUPPORTED; a second copy of that table in this
 * file could only drift from it. The library also refuses a stream that is not
 * running, a mode that is not RFC 2833, a telephone-event payload type of 0
 * and a direction that does not send, and a digit still on the wire answers
 * HAL_TRANSIENT, which is back pressure and not a bad request.
 */
static enum hal_status udsp_stream_dtmf_send(struct hal *h, unsigned int line,
					     unsigned int stream, char digit,
					     uint32_t duration_ms)
{
	if (!stream_get(h, line, stream))
		return HAL_UNSUPPORTED;

	return call_check(udsp_stream_dtmf(h->s, line, stream, digit, duration_ms),
			  "stream_dtmf");
}

static enum hal_status udsp_stream_stats_result_get(struct hal *h, unsigned int line,
						    unsigned int stream,
						    struct hal_media_stats *out)
{
	struct hal_udsp_stream *s = stream_get(h, line, stream);

	if (!s || !out)
		return HAL_UNSUPPORTED;

	*out = s->stats;

	return HAL_OK;
}

/*
 * The fax profile is forwarded. PASSTHROUGH clamps the stream to the named
 * codec and switches the suppression family off, and OFF restores what the
 * profile displaced. The library refuses the T38 profile mode by design,
 * whether or not a plug-in is loaded: T.38 is the stream-only path, the codec
 * on stream_config_set, never a profile.
 *
 * The signature is narrower than the record and that is lossless.
 * hal_fax_profile carries five switches beside the mode and the codec, and
 * libudsp fixes every one of them per mode instead of taking them: Passthrough
 * forces the echo canceller to bypass, VAD, CNG and silence suppression off and
 * the jitter buffer to fixed, which is exactly the combination the core sends;
 * off restores the pre-fax stream and channel state. So the mode and the codec
 * pair are the whole request and the switches carry no information the library
 * would act on differently.
 *
 * The library also requires a running stream, and answers UDSP_UNSUPPORTED
 * without one: a profile has nothing to displace on a stream that is not
 * carrying audio.
 *
 * The library fixes the kind as well: the passthrough mode arms
 * UDSP_PASSTHROUGH_MODEM, which is what a CED answer proves. The fax kind
 * arrives on the description instead (hal_media.passthrough), so it needs no
 * profile of its own and this op never carries it.
 */
static enum hal_status udsp_fax_profile_apply(struct hal *h, unsigned int line,
					      unsigned int stream,
					      const struct hal_fax_profile *fax)
{
	int mode;
	int sel;

	if (!stream_get(h, line, stream) || !fax)
		return HAL_UNSUPPORTED;

	mode = hal_udsp_fax_mode(fax->mode);
	sel = hal_udsp_codec_sel(fax->codec);
	if (mode < 0 || sel < 0) {
		TRACE(TRACE_HAL, "udsp: fax mode %u codec %u has no library "
		      "member", fax->mode, fax->codec);
		return HAL_UNSUPPORTED;
	}

	return call_check(udsp_fax_profile(h->s, line, stream,
					   (enum udsp_fax_mode)mode, (uint8_t)sel,
					   fax->codec_pt), "fax_profile");
}

/*
 * Three-way is forwarded: libudsp sets UDSP_FEAT_CONF and implements
 * udsp_conf, so the capability travels through hal_udsp_cap_fill like every
 * other pair and this op is a plain translation.
 *
 * The library owns the rest of the policy, and one part of it is worth naming
 * because it looks like a refusal this backend could have avoided: a line with
 * no configured stream answers UDSP_UNSUPPORTED, because the mix is a property
 * of the line's media plane and that plane exists only while a stream does
 * (core stream_conf). So a conference asked for before the legs have media is
 * refused, and the core reverts to the consultative hold with no tone, which
 * is the same answer a board with no mixer gives.
 */
static enum hal_status udsp_conf_set(struct hal *h, unsigned int line,
				     enum hal_conf_mode mode)
{
	if (line >= h->cap.lines)
		return HAL_UNSUPPORTED;

	return call_check(udsp_conf(h->s, line, (enum udsp_conf_mode)mode), "conf");
}

/*
 * The manufacturing loop, forwarded the way the conference is. The library
 * owns the whole refusal order and the two exclusions hal.h names, so this op
 * adds the line bound alone: udsp_line_loop reads the library capability and
 * this backend may carry fewer lines than the library reports.
 */
static enum hal_status udsp_line_loop_set(struct hal *h, unsigned int line,
					  const struct hal_loop *loop)
{
	if (line >= h->cap.lines || !loop)
		return HAL_UNSUPPORTED;

	return call_check(udsp_line_loop(h->s, line,
					 (enum udsp_loop_mode)loop->mode,
					 loop->peer, loop->delay_ms),
			  "line_loop");
}

/*
 * The two lifecycle halves of the media plane, declared above the lifecycle
 * section and defined here beside the socket rules they apply.
 *
 * The sweep walks the full arrays rather than cap.lines, because it runs at
 * close and a capability that shrank between an open and a close would leave a
 * descriptor behind. Nothing else in this file may read past the capability.
 */
static void media_fds_init(struct hal *h)
{
	unsigned int line, stream;

	for (line = 0; line < HAL_LINE_MAX; line++)
		for (stream = 0; stream < HAL_STREAM_MAX; stream++)
			hal_media_sock_init(&h->stream[line][stream].sock);
}

static void media_stream_release(struct hal *h, unsigned int line,
				 unsigned int stream)
{
	struct hal_udsp_stream *s = &h->stream[line][stream];

	if (s->sock.rtp_fd < 0 && !s->configured)
		return;

	stream_halt(h, line, stream, s);
}

static void media_streams_stop(struct hal *h)
{
	unsigned int line, stream;

	for (line = 0; line < HAL_LINE_MAX; line++)
		for (stream = 0; stream < HAL_STREAM_MAX; stream++)
			media_stream_release(h, line, stream);
}

/* --- slow ops ------------------------------------------------------------- */

/*
 * No worker thread runs these. An udsp SLOW op only enqueues: it takes the
 * control lock, pushes the job onto the SLIC thread's queue and returns with
 * the token filled (udsp.h section 1), so calling it on the uloop thread costs
 * one uncontended lock. The completion is written by the library's emitter as
 * UDSP_EV_JOB_DONE, which the core reads as HAL_EVENT_JOB_DONE off the same
 * socket.
 *
 * The token is the library's. hal.h wants job->token filled before the op
 * returns, and that is exactly when udsp fills its own, so the two records
 * name the same number and no map table stands between them.
 */
static enum hal_status job_submit(struct hal_job *out, enum udsp_status rc,
				  const struct udsp_job *job, const char *what)
{
	enum hal_status st = call_check(rc, what);

	if (st == HAL_OK)
		out->token = job->token;

	return st;
}

/*
 * The token the core waits on is the start job's, and the completion it
 * eventually reads is the one this backend posts once the region has landed
 * too (see the relay note in udsp_int.h). libudsp's own completion for that
 * token is consumed by the chain, so the core sees exactly one record for it.
 */
static enum hal_status udsp_dsp_start(struct hal *h, struct hal_job *job)
{
	struct udsp_job j = { 0 };
	enum hal_status st = job_submit(job, udsp_start(h->s, &j), &j, "start");

	if (st != HAL_OK)
		return st;

	/* libudsp refuses a second start while a job is inflight, so an armed
	 * chain cannot be overwritten by an accepted one; a library that ever
	 * accepted it would strand the earlier core job on its 30 s backstop,
	 * which is why the replacement is traced. */
	if (h->start_armed || h->region_armed)
		TRACE(TRACE_HAL, "udsp: bring-up chain replaced, token %u",
		      j.token);

	h->start_token = j.token;
	h->start_armed = true;
	h->region_armed = false;

	return HAL_OK;
}

static enum hal_status udsp_dsp_stop(struct hal *h, struct hal_job *job)
{
	struct udsp_job j = { 0 };

	return job_submit(job, udsp_stop(h->s, &j), &j, "stop");
}

static char char_upper(char c)
{
	if (c >= 'a' && c <= 'z')
		return (char)(c - 'a' + 'A');

	return c;
}

/*
 * The name the core derived, as LIBUDSP spells it. line/line.c
 * region_iso3_get() is the only producer of this argument and it already
 * resolved the config into a region directory name, so there is no second map
 * here and no default: an upper case guard and one exception.
 *
 * The exception is YUG. That table maps the Serbian alpha-2 "RS" onto the
 * region name YUG, and libudsp ships that tone plan as SRB.conf. Nothing else
 * in the two sets disagrees in a way a spelling can repair.
 * hal_udsp_region_resolve() below picks the region file for a name that
 * libudsp does not ship.
 *
 * The core never passes a file path here, so the guard cannot mangle one: the
 * '/' form belongs to udsp-cli. Both buffers are eight bytes
 * wide (LINE_REGION_MAX and UDSP_ISO3_MAX), so no name can truncate.
 */
void hal_udsp_region_name(const char *iso3, char *out, size_t len)
{
	char up[HAL_UDSP_ISO3_MAX] = { 0 };
	size_t n = iso3 ? strlen(iso3) : 0;
	size_t i;

	memset(out, 0, len);
	if (!n || n >= sizeof(up))
		return;

	for (i = 0; i < n; i++)
		up[i] = char_upper(iso3[i]);

	if (!strcmp(up, "YUG")) {
		snprintf(out, len, "SRB");
		return;
	}

	str_copy(out, len, up);
}

/* The European names that load the generic ETSI profile when libudsp ships
 * no file of their own. Sorted, because region_ets_has() runs bsearch. */
static const char region_ets_names[][HAL_UDSP_ISO3_MAX] = {
	"BEL", "CHE", "DEU", "DNK", "ESP", "FIN", "FRA", "HUN", "ITA", "NLD",
	"POL", "RUS", "SWE",
};

static int region_name_cmp(const void *key, const void *row)
{
	return strcmp(key, row);
}

static bool region_ets_has(const char *name)
{
	return bsearch(name, region_ets_names, ARRAY_SIZE(region_ets_names),
		       sizeof(region_ets_names[0]), region_name_cmp) != NULL;
}

/*
 * The region file the backend loads. A name with no file loads ETS or USA,
 * because without a tone plan libudsp refuses every ring with caller identity.
 * Only -ENOENT selects the fallback: an unreadable or malformed file fails
 * the load, and the library keeps the previous region. An absent fallback
 * file fails the same way, and the resolver tries no second fallback.
 */
void hal_udsp_region_resolve(struct hal *h, const char *iso3, char *out,
			     size_t len)
{
	const char *fallback;

	hal_udsp_region_name(iso3, out, len);
	if (!out[0] || udsp_region_probe(out) != -ENOENT) {
		h->region_warned[0] = 0;
		return;
	}

	fallback = region_ets_has(out) ? "ETS" : "USA";
	if (strcmp(h->region_warned, out)) {
		ULOG_WARN("udsp: region %s has no file, loading %s\n", out,
			  fallback);
		str_copy(h->region_warned, sizeof(h->region_warned), out);
	}
	str_copy(out, len, fallback);
}

/*
 * The name the bring-up prefetches, which is the name the core will ask for.
 *
 * open() reads voip.base.region, and that option carries an ISO 3166 ALPHA-2
 * name on a default board ("US", cfg/schema.h). The map above resolves the
 * three letter form alone, so the chain would take its soft path on every
 * shipped configuration and the window this whole chain exists to close would
 * open again.
 *
 * The eight rows are a composition, not a second opinion: line/line.c
 * region_iso3_get maps the alpha-2 name onto a region name, and
 * hal_udsp_region_name maps the one name whose libudsp spelling differs
 * (YUG) onto its region file. Every row below is that composition, restricted
 * to the names that reach a region file libudsp actually ships, so the table
 * cannot disagree with the core about a name it knows. One consequence is
 * deliberate: an alpha-2 name outside these rows resolves to nothing and takes
 * the soft path. Only line/line.c holds the whole alpha-2 table, so the
 * fallback of such a name arrives with the region_apply that lines_start()
 * submits.
 *
 * A disagreement corrects itself. lines_start() blanks region_applied and
 * calls region_take() unconditionally, so whatever this prefetched, the name
 * the core derives is re-applied moments later and wins. The prefetch decides
 * what the line can carry during the bring-up window and nothing beyond it.
 */
static const struct {
	char alpha2[3];
	char iso3[4];
} region_prefetch[] = {
	{ "US", "USA" }, { "GB", "GBR" }, { "UK", "GBR" }, { "EU", "ETS" },
	{ "AT", "AUT" }, { "AU", "AUS" }, { "NZ", "NZL" }, { "RS", "SRB" },
};

void hal_udsp_region_prefetch(struct hal *h, const char *region, char *out,
			      size_t len)
{
	char up[HAL_UDSP_ISO3_MAX] = { 0 };
	size_t n = region ? strlen(region) : 0;
	size_t i;

	/* Three characters or more is already a directory name, so it takes
	 * the same path the core's own region_apply argument takes. */
	if (n != 2) {
		hal_udsp_region_resolve(h, region, out, len);
		return;
	}

	memset(out, 0, len);
	up[0] = char_upper(region[0]);
	up[1] = char_upper(region[1]);

	for (i = 0; i < ARRAY_SIZE(region_prefetch); i++) {
		if (strcmp(region_prefetch[i].alpha2, up))
			continue;

		str_copy(out, len, region_prefetch[i].iso3);
		return;
	}
}

static enum hal_status udsp_region_apply(struct hal *h, const char *iso3,
					 struct hal_job *job)
{
	char name[HAL_UDSP_ISO3_MAX];
	struct udsp_job j = { 0 };

	if (!iso3 || !iso3[0])
		return HAL_FATAL;

	hal_udsp_region_resolve(h, iso3, name, sizeof(name));
	if (!name[0])
		return HAL_FATAL;

	return job_submit(job, udsp_region_load(h->s, name, &j), &j, "region_load");
}

static enum hal_status udsp_line_test_start(struct hal *h, unsigned int line,
					    enum hal_line_test test,
					    struct hal_job *job)
{
	struct udsp_job j = { 0 };

	return job_submit(job,
		udsp_line_test(h->s, line, (enum udsp_line_test)test, &j), &j,
		"line_test");
}

/*
 * The one SLOW op with no SLOW library call behind it.
 *
 * hal.h classifies stream_stats_start SLOW to leave room for a backend that
 * must query the device (hal.h section 1). libudsp answers the question from
 * a double buffered snapshot the engine publishes every tick, which is a
 * FASTPATH with no cross thread wait at all (udsp.h udsp_stream_stats), so
 * there is nothing to defer and this backend runs no worker to defer it to
 * (see the threads note in udsp_int.h).
 *
 * The op therefore reads the snapshot inline and posts its own completion, and
 * the contract is unchanged: the op still returns as soon as the work is
 * accepted, still fills job->token before it returns, and the result still
 * arrives as a HAL_EVENT_JOB_DONE record the core dispatches on the uloop
 * thread. It cannot arrive early: the relay only writes the socket, and
 * the core's reader is a uloop callback, so the core has returned from this op
 * and called job_add() long before the loop turns again (line/line.c
 * media_stats_snapshot: the op is called, then the job is added).
 *
 * The snapshot outlives the stream. stream_halt does not clear it, so the core
 * reads it back with stream_stats_result_get after the stop the snapshot ran
 * in front of, which is why the query may run before the teardown at all.
 */
static enum hal_status udsp_stream_stats_start(struct hal *h, unsigned int line,
					       unsigned int stream,
					       struct hal_job *job)
{
	struct hal_udsp_stream *s = stream_get(h, line, stream);
	struct udsp_media_stats snap;
	enum hal_status st;

	if (!s || !job)
		return HAL_UNSUPPORTED;

	memset(&snap, 0, sizeof(snap));
	st = call_check(udsp_stream_stats(h->s, line, stream, &snap),
			"stream_stats");
	if (st != HAL_OK)
		return st;

	hal_udsp_stats_fill(&s->stats, &snap);
	job->token = hal_relay_stats_token_next(&h->relay);
	hal_relay_completion_post(&h->relay, job->token, HAL_OK);

	return HAL_OK;
}

/* --- the fast diagnostic getters ------------------------- */

/*
 * The population gate first, exactly as every other media op takes it, so a
 * line the board does not carry answers before anything is indexed. The
 * library gates again on its own cap and adds one refusal this one cannot:
 * a stream that has not run a tick since its last configuration has no
 * counters of that call yet and answers unsupported too (udsp.h
 * udsp_stream_jb_stats). A caller skips such a stream; hal.h says so.
 */
static enum hal_status udsp_stream_jb_stats_get(struct hal *h, unsigned int line,
						unsigned int stream,
						struct hal_jb_stats *out)
{
	struct udsp_jb_stats snap;
	enum hal_status st;

	if (!stream_get(h, line, stream) || !out)
		return HAL_UNSUPPORTED;

	/*
	 * The status is mapped and not traced. call_check writes a line for
	 * every answer that is not HAL_OK, and this op is polled: "voice jb"
	 * walks the whole grid and the media trace sweeps it once a second, so
	 * every unconfigured stream would cost a line per pass. A refusal here
	 * is an answer ("no counters for that stream"), not an error, and the
	 * caller skips the row. debug_set keeps call_check, because a failure
	 * there is a real one.
	 */
	memset(&snap, 0, sizeof(snap));
	st = hal_udsp_status_map(udsp_stream_jb_stats(h->s, line, stream, &snap));
	if (st != HAL_OK)
		return st;

	hal_udsp_jb_stats_fill(out, &snap);

	return HAL_OK;
}

/*
 * The line canceller getter. It takes the line and no
 * stream, because the canceller is the line's, and it refuses the same way
 * the jitter buffer getter does: the status is mapped and not traced, since
 * "voice lec" walks every line and a line without a canceller is an answer
 * and not an error.
 *
 * The library refuses one state alone: a line that holds no canceller instance
 * (udsp.h udsp_line_lec_stats). A line whose canceller the modem policy skips
 * still answers HAL_OK. So does a line whose channel configuration switched
 * the canceller off, and a line whose loop is open. All three read ec_active 0
 * with frozen counters, so the northbound reads the live policy and not a
 * cached row. loop_open 1 tells the open loop from the other two.
 */
static enum hal_status udsp_line_lec_stats_get(struct hal *h, unsigned int line,
					       struct hal_lec_stats *out)
{
	struct udsp_lec_stats snap;
	enum hal_status st;

	if (line >= h->cap.lines || !out)
		return HAL_UNSUPPORTED;

	memset(&snap, 0, sizeof(snap));
	st = hal_udsp_status_map(udsp_line_lec_stats(h->s, line, &snap));
	if (st != HAL_OK)
		return st;

	hal_udsp_lec_stats_fill(out, &snap);

	return HAL_OK;
}

/*
 * The caller's own release of the tone-disabler latch (udsp.h
 * udsp_line_lec_latch_clear). The library posts the policy record with the
 * caller reason when a latch stood and changes nothing when none did, so this
 * backend forwards the line and maps the status.
 *
 * The library refuses a bad line UDSP_UNSUPPORTED, a stopped or parked engine
 * UDSP_TRANSIENT and a fatal engine UDSP_FATAL. The caller traces a refusal
 * and carries on, so the status is mapped and not traced here.
 */
static enum hal_status udsp_line_lec_latch_clear_op(struct hal *h,
						    unsigned int line)
{
	if (line >= h->cap.lines)
		return HAL_UNSUPPORTED;

	return hal_udsp_status_map(udsp_line_lec_latch_clear(h->s, line));
}

/*
 * The line monitor getter, the shape the canceller getter above has. The
 * status is mapped and not traced, because "voice monitor" walks every line.
 * A line the library answers no monitor for is an answer and not an error.
 *
 * The library refuses two states. A SLIC plug-in with no monitor op and a
 * line outside the capability both answer UDSP_UNSUPPORTED. A dead line
 * answers UDSP_FATAL with the record untouched (udsp.h udsp_line_monitor).
 * Every other line answers UDSP_OK, with valid 0 until the first reading
 * landed.
 */
static enum hal_status udsp_line_monitor_get(struct hal *h, unsigned int line,
					     struct hal_monitor *out)
{
	struct udsp_monitor snap;
	enum hal_status st;

	if (line >= h->cap.lines || !out)
		return HAL_UNSUPPORTED;

	memset(&snap, 0, sizeof(snap));
	st = hal_udsp_status_map(udsp_line_monitor(h->s, line, &snap));
	if (st != HAL_OK)
		return st;

	hal_udsp_monitor_fill(out, &snap);

	return HAL_OK;
}

/* status mapped and not traced, the line_monitor_get rule: "not loaded yet"
 * is an answer the core polls for, not an error. */
static enum hal_status udsp_region_get_op(struct hal *h, struct hal_region *out)
{
	struct udsp_region_view view;
	enum hal_status st;

	if (!out)
		return HAL_UNSUPPORTED;

	st = hal_udsp_status_map(udsp_region_get(h->s, &view));
	if (st != HAL_OK)
		return st;

	hal_udsp_region_fill(out, &view);

	return HAL_OK;
}

/*
 * The FAST half of stream_stats_start, which udsp.h anticipated:
 * the numbers are a double-buffer snapshot the engine republishes, so there is
 * nothing slow about reading them and a diagnostic that wants them every
 * second needs no job at all. stream_stats_result_get keeps its own meaning:
 * it answers the snapshot the last slow query took, which outlives the stream.
 */
static enum hal_status udsp_stream_stats_get(struct hal *h, unsigned int line,
					     unsigned int stream,
					     struct hal_media_stats *out)
{
	struct udsp_media_stats snap;
	enum hal_status st;

	if (!stream_get(h, line, stream) || !out)
		return HAL_UNSUPPORTED;

	/* Polled like the getter above, so the status is mapped and not
	 * traced, for the reason spelled out there. */
	memset(&snap, 0, sizeof(snap));
	st = hal_udsp_status_map(udsp_stream_stats(h->s, line, stream, &snap));
	if (st != HAL_OK)
		return st;

	hal_udsp_stats_fill(out, &snap);

	return HAL_OK;
}

/*
 * The transport and encryption counters of one stream. The status is mapped
 * and not traced, the rule the getters above follow: the diag table sweeps the
 * whole grid, and an idle stream is an answer and not an error.
 *
 * The library refuses an idle stream, a bad index and a NULL argument
 * UDSP_UNSUPPORTED, and a dead stream UDSP_FATAL (udsp.h udsp_stream_diag).
 * The output is cleared on every refusal, so a caller that renders a row
 * anyway renders no number the engine did not measure.
 *
 * It runs on the uloop thread, where every op of this backend runs. The
 * library reads its counters with no lock and the SRTP session is zeroized at
 * destroy without a fence, so a read from another thread could race that
 * teardown.
 */
static enum hal_status udsp_stream_diag_get_op(struct hal *h, unsigned int line,
					       unsigned int stream,
					       struct hal_stream_diag *out)
{
	struct udsp_stream_diag snap;
	enum hal_status st;

	if (!out)
		return HAL_UNSUPPORTED;

	/* Every refusal leaves a cleared record, this one included, so a
	 * caller that renders a row anyway renders no number the engine did
	 * not measure. */
	memset(out, 0, sizeof(*out));
	if (!stream_get(h, line, stream))
		return HAL_UNSUPPORTED;

	memset(&snap, 0, sizeof(snap));
	st = hal_udsp_status_map(udsp_stream_diag(h->s, line, stream, &snap));
	if (st != HAL_OK)
		return st;

	hal_udsp_stream_diag_fill(out, &snap);

	return HAL_OK;
}

/*
 * One snapshot of the engine, on the thread rule and the status rule of the
 * getter above. The watchdog publishes it once per period, so the library
 * answers UDSP_UNSUPPORTED until the first publication and the caller renders
 * no engine row before it.
 */
static enum hal_status udsp_engine_diag_get_op(struct hal *h,
					       struct hal_engine_diag *out)
{
	struct udsp_engine_diag snap;
	enum hal_status st;

	if (!out)
		return HAL_UNSUPPORTED;

	memset(out, 0, sizeof(*out));
	memset(&snap, 0, sizeof(snap));
	st = hal_udsp_status_map(udsp_engine_diag(h->s, &snap));
	if (st != HAL_OK)
		return st;

	hal_udsp_engine_diag_fill(out, &snap);

	return HAL_OK;
}

/*
 * The mask is stored and translated in one place. The library takes its own
 * relaxed store and the engine picks it up on its next tick, so an arm and a
 * disarm both apply without a tick boundary of their own; the ring drain is
 * this backend's alone and follows the jitter buffer bit, not the mask.
 *
 * The packet trace and the canceller capture each take two calls, and this is
 * why the word this store writes has to carry their bits. The enable call of
 * each maps the memory and raises the bit under the library's own lock, but
 * udsp_debug_set writes the WHOLE mask with a relaxed store, so a later push
 * that left a bit out would take its producers down while the rings, the flag
 * this file keeps and the drain timer all said the trace runs. Each bit
 * therefore travels in every push made while its trace is armed, and the
 * memory call runs on the edge alone, because no repeat of the same request
 * should ask for a second mapping.
 *
 * The two edges take opposite orders. The arm maps first, so the mask that
 * follows carries a bit whose rings exist. The disarm lowers the mask first,
 * so the producers stop before the memory flag does and the drain that follows
 * empties what they left.
 *
 * The answer is HAL_OK whatever either trace did, because the other bits
 * reached the library and a caller that gets a refusal stops asking (hal.h
 * debug_set). A refused arm is a warning and a counter.
 */
static enum hal_status udsp_debug_set_op(struct hal *h, uint32_t flags)
{
	bool pkt_had = (h->debug & HAL_DEBUG_PKT_TRACE) != 0u;
	bool pkt_want = (flags & HAL_DEBUG_PKT_TRACE) != 0u;
	bool lec_had = (h->debug & HAL_DEBUG_LEC_CAPTURE) != 0u;
	bool lec_want = (flags & HAL_DEBUG_LEC_CAPTURE) != 0u;
	uint32_t lib = 0u;

	if (flags & HAL_DEBUG_JB_TRACE)
		lib |= UDSP_DEBUG_JB_TRACE;
	if (flags & HAL_DEBUG_SLIC_LOG)
		lib |= UDSP_DEBUG_SLIC_LOG;

	h->debug = flags;
	if (pkt_want && (pkt_had || pkt_trace_start(h)))
		lib |= UDSP_DEBUG_PKT_TRACE;
	if (lec_want && (lec_had || lec_capture_start(h)))
		lib |= UDSP_DEBUG_LEC_CAPTURE;

	udsp_debug_set(h->s, lib);
	jb_trace_arm(h, (lib & UDSP_DEBUG_JB_TRACE) != 0u);
	if (!pkt_want && pkt_had)
		pkt_trace_stop(h);
	if (!lec_want && lec_had)
		lec_capture_stop(h);

	return HAL_OK;
}

const struct hal_ops hal_backend_ops = {
	.name = "udsp",
	.open = udsp_backend_open,
	.close = udsp_backend_close,
	.cap_get = udsp_cap_get,
	.counters_get = udsp_counters_get,
	.line_state_set = udsp_line_state_set,
	.line_polarity_set = udsp_line_polarity_set,
	.line_hook_query = udsp_line_hook_query_op,
	.line_hook_timing_set = udsp_line_hook_timing_set,
	.line_ring_start = udsp_line_ring_start,
	.line_ring_stop = udsp_line_ring_stop,
	.line_cid_send = udsp_line_cid_send,
	.line_mwi_set = udsp_line_mwi_set,
	.line_test_result_get = udsp_line_test_result_get,
	.tone_play = udsp_tone_play_op,
	.tone_stop = udsp_tone_stop_op,
	.cw_tone_play = udsp_cw_tone_play_op,
	.digit_play = udsp_digit_play_op,
	.channel_config_set = udsp_channel_config_set,
	.stream_config_set = udsp_stream_config_set,
	.stream_start = udsp_stream_start_op,
	.stream_stop = udsp_stream_stop_op,
	.stream_dtmf_send = udsp_stream_dtmf_send,
	.stream_stats_result_get = udsp_stream_stats_result_get,
	.fax_profile_apply = udsp_fax_profile_apply,
	.conf_set = udsp_conf_set,
	.stream_jb_stats_get = udsp_stream_jb_stats_get,
	.stream_stats_get = udsp_stream_stats_get,
	.line_lec_stats_get = udsp_line_lec_stats_get,
	.line_lec_latch_clear = udsp_line_lec_latch_clear_op,
	.line_monitor_get = udsp_line_monitor_get,
	.stream_diag_get = udsp_stream_diag_get_op,
	.engine_diag_get = udsp_engine_diag_get_op,
	.region_get = udsp_region_get_op,
	.debug_set = udsp_debug_set_op,
	.dsp_start = udsp_dsp_start,
	.dsp_stop = udsp_dsp_stop,
	.line_test_start = udsp_line_test_start,
	.stream_stats_start = udsp_stream_stats_start,
	.region_apply = udsp_region_apply,
	.line_loop_set = udsp_line_loop_set,
};
